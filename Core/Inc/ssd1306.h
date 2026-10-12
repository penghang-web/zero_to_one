/**
  ******************************************************************************
  * @file    ssd1306.h
  * @brief   SSD1306 OLED 驱动（I2C1，128x64，阻塞模式），含中英文混排绘制
  ******************************************************************************
  *
  * 依赖：I2C1（PB6/SCL, PB7/SDA，地址 0x3C）；字库来自 W25Q（DrawCN）。
  * 取模方向：横向，每行像素 bit7 = 最左，与 HZK16 / font8x16 一致。
  */
#ifndef SSD1306_H
#define SSD1306_H

#include <stdint.h>

#define SSD1306_I2C_ADDR  0x3C    /* 7 位地址；HAL 调用时自动左移 */
#define SSD1306_WIDTH     128
#define SSD1306_HEIGHT    64
#define SSD1306_BUF_SIZE  (SSD1306_WIDTH * SSD1306_HEIGHT / 8)  /* 1024 字节影子显存 */

/* 对外接口（实现见 ssd1306.c）
 * - SSD1306_Init     : 发送初始化序列（开电荷泵等），屏幕就绪
 * - SSD1306_Clear    : 清影子显存（不会立即上屏，需 Refresh）
 * - SSD1306_Refresh  : 把整块影子显存刷写到 OLED
 * - SSD1306_DrawPixel: 画单点
 * - SSD1306_DrawChar : 内置 8x16 ASCII（font8x16.h）
 * - SSD1306_DrawCN   : 从 W25Q 读 16x16 汉字字模绘制（字库未烧录时画占位框）
 * - SSD1306_DrawString: 中英文混排。字符串按 GB2312 解析（汉字双字节 + ASCII 单字节）
 *                       —— 注意：源码是 UTF-8，中文请传 GB2312 字节数组（见 ui.c 演示）
 * color: 0 = 灭（黑），1 = 亮（白）
 */
void SSD1306_Init(void);
void SSD1306_Clear(void);
void SSD1306_Refresh(void);
void SSD1306_DrawPixel(uint8_t x, uint8_t y, uint8_t color);
void SSD1306_DrawChar(uint8_t x, uint8_t y, char c, uint8_t color);
void SSD1306_DrawCN(uint8_t x, uint8_t y, uint8_t qu, uint8_t wei, uint8_t color);
void SSD1306_DrawString(uint8_t x, uint8_t y, const char *s, uint8_t color);

/* 小号 6x8 ASCII（一行 21 字符 / 全屏 8 行），仅 ASCII，用于密集信息显示 */
void SSD1306_DrawChar6x8(uint8_t x, uint8_t y, char c, uint8_t color);
void SSD1306_DrawString6x8(uint8_t x, uint8_t y, const char *s, uint8_t color);

#endif /* SSD1306_H */
