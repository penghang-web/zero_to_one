/**
  ******************************************************************************
  * @file    ssd1306.c
  * @brief   SSD1306 OLED 驱动（I2C1，128x64，阻塞模式），含中英文混排绘制
  ******************************************************************************
  *
  * I2C 命令约定：控制字节 0x00=后续为命令，0x40=后续为显存数据。
  * 这里用 HAL_I2C_Mem_Write 把"控制字节"当作 mem address 发出，简洁可靠。
  */
#include "ssd1306.h"
#include "main.h"      /* 引脚/外设句柄所在 */
#include "i2c.h"       /* hi2c1 (extern) */
#include "w25qxx.h"    /* DrawCN 从 W25Q 读字库 */
#include "font8x16.h"  /* 内置 ASCII 8x16 字模 */
#include "font6x8.h"    /* 内置 ASCII 6x8 小字模（密集信息显示） */
#include "stm32f1xx_hal.h"
#include <string.h>     /* memset */

#define DEV       (SSD1306_I2C_ADDR << 1)   /* HAL 用 8 位地址（已左移） */
#define I2C_TICK  100
#define SSD1306_CHAR_GAP 3   /* 字符之间留 3px 宽度 */

/* 1024 字节影子显存（128 列 x 64 行 / 8 行每字节） */
static uint8_t s_buf[SSD1306_BUF_SIZE];

/* ---- 底层 I2C 写：控制字节 0x00=命令, 0x40=数据 ---- */
static void write_cmd(uint8_t c)
{
  HAL_I2C_Mem_Write(&hi2c1, DEV, 0x00, I2C_MEMADD_SIZE_8BIT, &c, 1, I2C_TICK);
}
static void write_cmds(const uint8_t *p, uint16_t n)
{
  if (n == 0) return;
  HAL_I2C_Mem_Write(&hi2c1, DEV, 0x00, I2C_MEMADD_SIZE_8BIT, (uint8_t *)p, n, I2C_TICK);
}
static void write_data(const uint8_t *p, uint16_t n)
{
  if (n == 0) return;
  HAL_I2C_Mem_Write(&hi2c1, DEV, 0x40, I2C_MEMADD_SIZE_8BIT, (uint8_t *)p, n, I2C_TICK);
}

/* -------------------------------------------------------------------------- */
void SSD1306_Init(void)
{
  /* 128x64 标准初始化序列（3.3V 供电，必须开启电荷泵 0x8D/0x14 否则屏暗） */
  static const uint8_t init_seq[] = {
    0xAE,             /* 关显示 */
    0xD5, 0x80,       /* 显示时钟分频 */
    0xA8, 0x3F,       /* 复用率 64 */
    0xD3, 0x00,       /* 显示偏移 0 */
    0x40,             /* 起始行 0 */
    0x8D, 0x14,       /* 电荷泵使能 */
    0x20, 0x00,       /* 寻址模式：水平 */
    0xA1,             /* 段重映射（左右镜像，使字正向） */
    0xC8,             /* COM 扫描方向重映射 */
    0xDA, 0x12,       /* COM 引脚硬件配置（128x64 用 0x12） */
    0x81, 0xCF,       /* 对比度 */
    0xD9, 0xF1,       /* 预充电周期 */
    0xDB, 0x40,       /* VCOMH 电平 */
    0xA4,             /* 显示内容来自 RAM（不全亮） */
    0xA6,             /* 正常显示（不反相） */
    0x2E,             /* 关闭滚动 */
    0xAF              /* 开显示 */
  };
  write_cmds(init_seq, sizeof(init_seq));
  SSD1306_Clear();
}

void SSD1306_Clear(void)
{
  memset(s_buf, 0, sizeof(s_buf));
}

void SSD1306_Refresh(void)
{
  /* 水平寻址模式下，先设定列/页窗口，再一次性送出 1024 字节 */
  write_cmd(0x21); write_cmd(0);    write_cmd(SSD1306_WIDTH - 1);   /* 列 0..127 */
  write_cmd(0x22); write_cmd(0);    write_cmd(SSD1306_HEIGHT / 8 - 1); /* 页 0..7 */
  write_data(s_buf, SSD1306_BUF_SIZE);
}

void SSD1306_DrawPixel(uint8_t x, uint8_t y, uint8_t color)
{
  if (x >= SSD1306_WIDTH || y >= SSD1306_HEIGHT)
  {
    return;
  }
  uint16_t idx = (uint16_t)x + (uint16_t)(y / 8) * SSD1306_WIDTH;
  uint8_t  bit = y % 8;
  if (color)
  {
    s_buf[idx] |= (uint8_t)(1U << bit);
  }
  else
  {
    s_buf[idx] &= (uint8_t) ~(1U << bit);
  }
}

void SSD1306_DrawChar(uint8_t x, uint8_t y, char c, uint8_t color)
{
  if ((uint8_t)c < FONT8X16_FIRST || (uint8_t)c > 0x7E)
  {
    return;   /* 仅覆盖可打印 ASCII */
  }
  uint16_t base = (uint16_t)((uint8_t)c - FONT8X16_FIRST) * FONT8X16_H;
  for (uint8_t row = 0; row < FONT8X16_H; row++)
  {
    uint8_t bits = font8x16[base + row];   /* bit7 = 最左列 */
    for (uint8_t col = 0; col < FONT8X16_W; col++)
    {
      if (bits & (0x80U >> col))
      {
        SSD1306_DrawPixel((uint8_t)(x + col), (uint8_t)(y + row), color);
      }
    }
  }
}

void SSD1306_DrawCN(uint8_t x, uint8_t y, uint8_t qu, uint8_t wei, uint8_t color)
{
  uint8_t glyph[32];
  int ok = W25QXX_ReadGlyph16x16(qu, wei, glyph);

  /* 字库未烧录（读回全 0xFF）或读取失败：画占位框，便于先验证 I2C 连线 */
  int blank = 1;
  for (int i = 0; i < 32; i++)
  {
    if (glyph[i] != 0xFF) { blank = 0; break; }
  }
  if (ok != W25QXX_OK || blank)
  {
    for (uint8_t i = 0; i < 16; i++)
    {
      SSD1306_DrawPixel(x,           (uint8_t)(y + i), color);
      SSD1306_DrawPixel((uint8_t)(x + 15), (uint8_t)(y + i), color);
      SSD1306_DrawPixel((uint8_t)(x + i), y,       color);
      SSD1306_DrawPixel((uint8_t)(x + i), (uint8_t)(y + 15), color);
    }
    return;
  }

  /* 正常：16 行，每行 2 字节（16 位），bit15..bit0 = 左..右 */
  for (uint8_t row = 0; row < 16; row++)
  {
    uint8_t b0 = glyph[row * 2];
    uint8_t b1 = glyph[row * 2 + 1];
    for (uint8_t col = 0; col < 16; col++)
    {
      uint8_t bit;
      if (col < 8)
      {
        bit = (b0 >> (7 - col)) & 0x01U;
      }
      else
      {
        bit = (b1 >> (7 - (col - 8))) & 0x01U;
      }
      if (bit)
      {
        SSD1306_DrawPixel((uint8_t)(x + col), (uint8_t)(y + row), color);
      }
    }
  }
}

void SSD1306_DrawString(uint8_t x, uint8_t y, const char *s, uint8_t color)
{
  if (s == NULL)
  {
    return;
  }
  uint8_t cx = x;
  while (*s)
  {
    uint8_t b = (uint8_t)(*s);
    if (b >= 0xA1U)
    {
      /* GB2312 双字节汉字：当前为区码(qu)，下一字节为位码(wei) */
      uint8_t qu = b;
      uint8_t wei = (uint8_t)(*(s + 1));
      SSD1306_DrawCN(cx, y, qu, wei, color);
      cx = (uint8_t)(cx + 16 + SSD1306_CHAR_GAP);
      s += 2;
    }
    else
    {
      SSD1306_DrawChar(cx, y, (char)b, color);
      cx = (uint8_t)(cx + 8 + SSD1306_CHAR_GAP);
      s += 1;
    }
  }
}

/* -------------------------------------------------------------------------- */
/* 小号 6x8 ASCII（仅 ASCII，用于密集信息显示）                                */
void SSD1306_DrawChar6x8(uint8_t x, uint8_t y, char c, uint8_t color)
{
  if ((uint8_t)c < FONT6X8_FIRST || (uint8_t)c > 0x7E)
  {
    return;   /* 仅覆盖可打印 ASCII */
  }
  uint16_t base = (uint16_t)((uint8_t)c - FONT6X8_FIRST) * FONT6X8_H;
  for (uint8_t row = 0; row < FONT6X8_H; row++)
  {
    uint8_t bits = font6x8[base + row];   /* bit7 = 最左列 */
    for (uint8_t col = 0; col < FONT6X8_W; col++)
    {
      if (bits & (0x80U >> col))
      {
        SSD1306_DrawPixel((uint8_t)(x + col), (uint8_t)(y + row), color);
      }
    }
  }
}

void SSD1306_DrawString6x8(uint8_t x, uint8_t y, const char *s, uint8_t color)
{
  if (s == NULL)
  {
    return;
  }
  uint8_t cx = x;
  while (*s)
  {
    SSD1306_DrawChar6x8(cx, y, (char)(*s), color);
    cx = (uint8_t)(cx + FONT6X8_W + SSD1306_CHAR_GAP);   /* 每个字符占 6 像素宽 + 3px 间隔 */
    s++;
  }
}
