/**
  ******************************************************************************
  * @file    w25qxx.h
  * @brief   W25Q / GD25 系列 SPI Flash 驱动（软件片选 PB0=W25Q_CS，轮询方式）
  ******************************************************************************
  *
  * 适配：本项目的 GD25Q32 模块（手持实为兆易创新 GD25Q32，4MB），命令集与
  *       Winbond W25Qxx 兼容，故驱动以 W25Q 命名通用之。
  * 硬件：SPI1（PA5/SCK, PA6/MISO, PA7/MOSI, Mode0, /4=18MHz），CS 为软件 GPIO。
  * 说明：HAL 的 SPI 收发不会自动拉 CS，所有读写前后由本驱动手动拉低/拉高 PB0。
  */
#ifndef W25QXX_H
#define W25QXX_H

#include <stdint.h>

#define W25QXX_OK    0
#define W25QXX_ERR  -1

/* 字库在 Flash 中的布局：地址 [0..3] 存 4 字节魔数 "HZK1"，其后才是 HZK16 点阵 */
#define W25Q_FONT_BASE       4      /* 字库数据起始地址（魔数占前 4 字节） */
#define W25Q_FONT_MAGIC      "HZK1" /* 魔数，用于 FontReady 检测是否已烧录 */
#define W25Q_FONT_MAGIC_LEN  4

/* GB2312 16x16 字模偏移（与 PC 端 gen_hzk16.py 一致）：
 *   offset = W25Q_FONT_BASE + ((区码-0xA1)*94 + (位码-0xA1)) * 32          */

/* 对外接口（实现见 w25qxx.c）
 * - W25QXX_Init        : 拉高 CS、读 JEDEC 缓存容量；main 初始化后调用一次
 * - W25QXX_JEDEC       : 取缓存的 JEDEC（id[0]=厂商,id[1]=类型,id[2]=容量字节）
 * - W25QXX_CapacityBytes: 由 JEDEC 推算总字节数
 * - W25QXX_ReadData    : 任意地址/长度读
 * - W25QXX_WriteData   : 任意地址/长度写（仅页编程，调用前须确保目标区已擦除）
 * - W25QXX_EraseSector : 擦除 4KB 扇区（阻塞直到完成）
 * - W25QXX_EraseRange  : 擦除覆盖 [addr, addr+len) 的全部扇区（供烧录前调用）
 * - W25QXX_ReadGlyph16x16 : 读一个 GB2312 汉字的 32 字节 16x16 字模
 * - W25QXX_FontReady   : 检测字库魔数：1=已烧录，0=未烧录/读失败
 */
void    W25QXX_Init(void);
void    W25QXX_JEDEC(uint8_t id[3]);
uint32_t W25QXX_CapacityBytes(void);
int     W25QXX_ReadData(uint32_t addr, uint8_t *buf, uint32_t len);
int     W25QXX_WriteData(uint32_t addr, const uint8_t *buf, uint32_t len);
int     W25QXX_EraseSector(uint32_t addr);
int     W25QXX_EraseRange(uint32_t addr, uint32_t len);
int     W25QXX_ReadGlyph16x16(uint8_t qu, uint8_t wei, uint8_t buf[32]);
int     W25QXX_FontReady(void);

#endif /* W25QXX_H */
