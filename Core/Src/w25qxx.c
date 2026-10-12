/**
  ******************************************************************************
  * @file    w25qxx.c
  * @brief   W25Q / GD25 系列 SPI Flash 驱动（软件片选 PB0=W25Q_CS，轮询方式）
  ******************************************************************************
  *
  * 协议要点：
  * - 所有命令/地址/数据的收发都用 SPI 全双工；读数据时用 TransmitReceive 产生时钟。
  * - CS 为软件 GPIO，每次事务 CS 拉低、结束拉高；空闲时保持高（失能）。
  * - 写/擦除前必须先 WriteEnable；操作后轮询状态寄存器 WIP(bit0) 直到清 0。
  */
#include "w25qxx.h"
#include "main.h"   /* W25Q_CS_Pin / W25Q_CS_GPIO_Port */
#include "spi.h"    /* hspi1 (extern) */
#include "stm32f1xx_hal.h"
#include <string.h>

/* ---- 指令集（W25Q / GD25 通用） ---- */
#define CMD_WREN  0x06   /* 写使能 */
#define CMD_WRDI  0x04   /* 写禁止 */
#define CMD_RDSR  0x05   /* 读状态寄存器1 */
#define CMD_RDID  0x9F   /* 读 JEDEC ID（厂商+类型+容量） */
#define CMD_READ  0x03   /* 读数据（慢速，最高 50MHz，18MHz 下足够） */
#define CMD_PP    0x02   /* 页编程（最多 256 字节） */
#define CMD_SE    0x20   /* 扇区擦除（4KB） */

#define SPI_TIMEOUT  100   /* ms */
#define PAGE_SIZE    256
#define SECTOR_SIZE  4096

/* ---- 模块私有 ---- */
static uint8_t  s_jedec[3] = {0};
static uint32_t s_capacity = 0;

/* 软件片选 */
static void CS_Low(void)
{
  HAL_GPIO_WritePin(W25Q_CS_GPIO_Port, W25Q_CS_Pin, GPIO_PIN_RESET);
}
static void CS_High(void)
{
  HAL_GPIO_WritePin(W25Q_CS_GPIO_Port, W25Q_CS_Pin, GPIO_PIN_SET);
}

/* 单字节收发（产生时钟并取回 MISO） */
static uint8_t spi_xfer(uint8_t tx)
{
  uint8_t rx = 0;
  HAL_SPI_TransmitReceive(&hspi1, &tx, &rx, 1, SPI_TIMEOUT);
  return rx;
}

/* 读状态寄存器1，返回 WIP(bit0) 表示忙 */
static uint8_t read_sr(void)
{
  uint8_t cmd = CMD_RDSR;
  uint8_t tx  = 0;
  uint8_t sr  = 0;
  CS_Low();
  HAL_SPI_Transmit(&hspi1, &cmd, 1, SPI_TIMEOUT);
  HAL_SPI_TransmitReceive(&hspi1, &tx, &sr, 1, SPI_TIMEOUT);
  CS_High();
  return sr;
}

static void wait_busy(void)
{
  while (read_sr() & 0x01U)   /* WIP = 1 表示忙 */
  {
    /* 空转；写/擦除完成自动清 0。打断点调试时看门狗由 TIM2 ISR 喂，不会误复位 */
  }
}

static void write_enable(void)
{
  uint8_t cmd = CMD_WREN;
  CS_Low();
  HAL_SPI_Transmit(&hspi1, &cmd, 1, SPI_TIMEOUT);
  CS_High();
}

/* 容量字节 -> 总字节数（GD25Q32: id[2]=0x16 => 4MB） */
static uint32_t capacity_from_jedec(uint8_t cap)
{
  if (cap < 0x11U)
  {
    return 0;
  }
  uint32_t mbit = 1UL << (cap - 0x11U);   /* Mbit */
  return mbit * 1024UL * 1024UL / 8UL;    /* 转字节 */
}

/* -------------------------------------------------------------------------- */
void W25QXX_Init(void)
{
  CS_High();                          /* 默认失能片选 */
  W25QXX_JEDEC(s_jedec);              /* 读 JEDEC 并缓存 */
  s_capacity = capacity_from_jedec(s_jedec[2]);
}

void W25QXX_JEDEC(uint8_t id[3])
{
  uint8_t cmd = CMD_RDID;
  CS_Low();
  HAL_SPI_Transmit(&hspi1, &cmd, 1, SPI_TIMEOUT);
  id[0] = spi_xfer(0);
  id[1] = spi_xfer(0);
  id[2] = spi_xfer(0);
  CS_High();
}

uint32_t W25QXX_CapacityBytes(void)
{
  return s_capacity;
}

int W25QXX_ReadData(uint32_t addr, uint8_t *buf, uint32_t len)
{
  if (buf == NULL || len == 0)
  {
    return W25QXX_ERR;
  }
  uint8_t hdr[4];
  hdr[0] = CMD_READ;
  hdr[1] = (uint8_t)((addr >> 16) & 0xFFU);
  hdr[2] = (uint8_t)((addr >> 8) & 0xFFU);
  hdr[3] = (uint8_t)(addr & 0xFFU);

  CS_Low();
  if (HAL_SPI_Transmit(&hspi1, hdr, 4, SPI_TIMEOUT) != HAL_OK)
  {
    CS_High();
    return W25QXX_ERR;
  }
  /* 分块读取：用全零 dummy 产生 SCK，同时收数据 */
  static uint8_t dummy[64];
  uint32_t off = 0;
  while (off < len)
  {
    uint16_t n = (len - off > 64U) ? 64U : (uint16_t)(len - off);
    memset(dummy, 0, n);
    if (HAL_SPI_TransmitReceive(&hspi1, dummy, buf + off, n, SPI_TIMEOUT) != HAL_OK)
    {
      CS_High();
      return W25QXX_ERR;
    }
    off += n;
  }
  CS_High();
  return W25QXX_OK;
}

int W25QXX_WriteData(uint32_t addr, const uint8_t *buf, uint32_t len)
{
  if (buf == NULL || len == 0)
  {
    return W25QXX_ERR;
  }
  uint32_t off = 0;
  while (off < len)
  {
    /* 不能超过 256 字节页边界（越界会回卷覆盖页首） */
    uint32_t page_rem = PAGE_SIZE - ((addr + off) % PAGE_SIZE);
    uint16_t n = (len - off > page_rem) ? (uint16_t)page_rem : (uint16_t)(len - off);

    uint8_t hdr[4];
    hdr[0] = CMD_PP;
    hdr[1] = (uint8_t)(((addr + off) >> 16) & 0xFFU);
    hdr[2] = (uint8_t)(((addr + off) >> 8) & 0xFFU);
    hdr[3] = (uint8_t)((addr + off) & 0xFFU);

    write_enable();
    CS_Low();
    HAL_SPI_Transmit(&hspi1, hdr, 4, SPI_TIMEOUT);
    HAL_SPI_Transmit(&hspi1, (uint8_t *)(buf + off), n, SPI_TIMEOUT);
    CS_High();
    wait_busy();
    off += n;
  }
  return W25QXX_OK;
}

int W25QXX_EraseSector(uint32_t addr)
{
  uint8_t hdr[4];
  hdr[0] = CMD_SE;
  hdr[1] = (uint8_t)((addr >> 16) & 0xFFU);
  hdr[2] = (uint8_t)((addr >> 8) & 0xFFU);
  hdr[3] = (uint8_t)(addr & 0xFFU);
  write_enable();
  CS_Low();
  HAL_SPI_Transmit(&hspi1, hdr, 4, SPI_TIMEOUT);
  CS_High();
  wait_busy();
  return W25QXX_OK;
}

int W25QXX_EraseRange(uint32_t addr, uint32_t len)
{
  if (len == 0)
  {
    return W25QXX_ERR;
  }
  uint32_t a = addr - (addr % SECTOR_SIZE);   /* 对齐到扇区首 */
  uint32_t end = addr + len;
  while (a < end)
  {
    W25QXX_EraseSector(a);
    a += SECTOR_SIZE;
  }
  return W25QXX_OK;
}

int W25QXX_ReadGlyph16x16(uint8_t qu, uint8_t wei, uint8_t buf[32])
{
  if (buf == NULL)
  {
    return W25QXX_ERR;
  }
  /* 仅处理 GB2312 区/位码范围（0xA1 起） */
  if (qu < 0xA1U || wei < 0xA1U)
  {
    return W25QXX_ERR;
  }
  uint32_t idx  = ((uint32_t)(qu - 0xA1U) * 94U + (uint32_t)(wei - 0xA1U)) * 32UL;
  uint32_t addr = W25Q_FONT_BASE + idx;
  return W25QXX_ReadData(addr, buf, 32);
}

int W25QXX_FontReady(void)
{
  uint8_t magic[4];
  if (W25QXX_ReadData(0, magic, 4) != W25QXX_OK)
  {
    return 0;
  }
  return (magic[0] == 'H' && magic[1] == 'Z' && magic[2] == 'K' && magic[3] == '1');
}
