/**
  ******************************************************************************
  * @file    ssd1306.c
  * @brief   SSD1306 OLED 驱动（I2C1，128x64，阻塞模式），含 ASCII 英文字符串绘制
  ******************************************************************************
  *
  * I2C 命令约定：控制字节 0x00=后续为命令，0x40=后续为显存数据。
  * 这里用 HAL_I2C_Mem_Write 把"控制字节"当作 mem address 发出，简洁可靠。
  */
#include "ssd1306.h"
#include "main.h"      /* 引脚/外设句柄所在 */
#include "i2c.h"       /* hi2c1 (extern) */
#include "mpu6050.h"   /* g_mpu / g_mpu_ok：MPU 三轴屏数据显示 */
#include "font8x16.h"  /* 内置 ASCII 8x16 字模 */
#include "font6x8.h"    /* 内置 ASCII 6x8 小字模（密集信息显示） */
#include "stm32f1xx_hal.h"
#include <string.h>     /* memset */
#include <stdio.h>      /* snprintf：UI 屏格式化 */

/* TIM2 1ms 节拍（main.c 维护），UI 屏切换/重绘节拍使用 */
extern volatile uint32_t g_systick_ms;

#define DEV       (SSD1306_I2C_ADDR << 1)   /* HAL 用 8 位地址（已左移） */
#define I2C_TICK  100
#define SSD1306_CHAR_GAP 3   /* 字符之间留 3px 宽度 */

/* 1024 字节影子显存（128 列 x 64 行 / 8 行每字节） */
static uint8_t s_buf[SSD1306_BUF_SIZE];

/* ---- 底层 I2C 写：控制字节 0x00=命令, 0x40=数据 ----
 * 改用 I2C1_SafeMemWrite：自带 BUSY 检测 + 总线恢复 + 失败重试，
 * 规避 STM32F1 I2C 外设锁死导致整个主循环卡死的缺陷。
 * 返回 HAL 状态，便于 Refresh 在失败时及时跳出、避免把整圈拖死。 */
static HAL_StatusTypeDef write_cmd(uint8_t c)
{
  return I2C1_SafeMemWrite(DEV, 0x00, I2C_MEMADD_SIZE_8BIT, &c, 1);
}
static HAL_StatusTypeDef write_cmds(const uint8_t *p, uint16_t n)
{
  if (n == 0) return HAL_OK;
  return I2C1_SafeMemWrite(DEV, 0x00, I2C_MEMADD_SIZE_8BIT, (uint8_t *)p, n);
}
static HAL_StatusTypeDef write_data(const uint8_t *p, uint16_t n)
{
  if (n == 0) return HAL_OK;
  return I2C1_SafeMemWrite(DEV, 0x40, I2C_MEMADD_SIZE_8BIT, (uint8_t *)p, n);
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
    0x20, 0x00,       /* 寻址模式：页（经典模式，Refresh 逐页设置页地址） */
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
  /* 页寻址模式（init 设 0x20 0x00）下，SSD1306 写满一页后列地址回绕、
   * 页地址不会自动递增。因此必须逐页显式设置页地址 + 列起始，再写 128 字节，
   * 否则 8 次数据全堆在页 0，页 1~7 永远是残留值（花屏）。
   * 任一次 I2C 失败即跳出本圈刷新（后续节拍再重试），避免 8 页全走 BusRecovery
   * 把主循环单圈拖到看门狗门控窗口（1s）以上，导致反复误复位。 */
  for (uint8_t page = 0; page < (SSD1306_HEIGHT / 8); page++)
  {
    if (write_cmd((uint8_t)(0xB0 + page)) != HAL_OK) break;   /* 设置当前页 0..7 */
    if (write_cmd(0x00) != HAL_OK) break;                      /* 列地址低位 = 0 */
    if (write_cmd(0x10) != HAL_OK) break;                      /* 列地址高位 = 0 → 列 = 0 */
    if (write_data(&s_buf[(uint16_t)page * SSD1306_WIDTH], SSD1306_WIDTH) != HAL_OK) break;
  }
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

void SSD1306_DrawString(uint8_t x, uint8_t y, const char *s, uint8_t color)
{
  if (s == NULL)
  {
    return;
  }
  uint8_t cx = x;
  while (*s)
  {
    /* 仅支持 ASCII：逐字节按 8x16 字模绘制（中文显示已移除） */
    SSD1306_DrawChar(cx, y, (char)(*s), color);
    cx = (uint8_t)(cx + 8 + SSD1306_CHAR_GAP);
    s++;
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

/* -------------------------------------------------------------------------- */
/* 高层 UI：状态屏 / MPU 三轴屏 / 屏幕切换调度（原 ui.c 内容合并到本驱动文件） */
/* -------------------------------------------------------------------------- */
/* 布局规格（字符高度固定 16px）：
 *   共 3 行，行间距 OLED_LINE_GAP = 3px，底部留白 64-(38+16) = 10px
 *     行0 Y = 0
 *     行1 Y = 0 + 16 + 3 = 19
 *     行2 Y = 19 + 16 + 3 = 38
 *   仅 ASCII/数字/符号，8px(宽)x16px(高)
 *   （由 SSD1306_DrawString 逐字节按 8x16 字模绘制；中文显示已移除，标题使用英文） */
#define OLED_LINE_GAP  3       /* 行间距 */
#define UI_LINE_H      16      /* 字符高度（汉字/英文 同行高） */
#define UI_LINE0_Y     0
#define UI_LINE1_Y     (UI_LINE0_Y + UI_LINE_H + OLED_LINE_GAP)   /* 0+16+3 = 19 */
#define UI_LINE2_Y     (UI_LINE1_Y + UI_LINE_H + OLED_LINE_GAP)   /* 19+16+3 = 38 */
/* 总占用：UI_LINE2_Y + UI_LINE_H = 38+16 = 54px；屏幕 64px，底部留白 10px */

/* 英文标题：替代中文"迷你黑匣子"，避免依赖 W25Q 字库（供"先英文显示"场景）。
 * SSD1306_DrawString 对纯 ASCII 串按 8x16 渲染，无需字库。 */
static const char *s_title_en = "MiniBlackBox";

void SSD1306_UI_Init(void)
{
  SSD1306_Init();
}

void SSD1306_UI_Demo(void)
{
  SSD1306_Clear();

  /* 行0（Y=0）：英文标题（无需字库） */
  SSD1306_DrawString(0, UI_LINE0_Y, s_title_en, 1);

  /* 行1（Y=19）：版本 + 运行时长（8x16 ASCII） */
  char line[24];
  unsigned long sec = (unsigned long)(g_systick_ms / 1000u);
  snprintf(line, sizeof(line), "MBB v1.0 %lu s", sec);
  SSD1306_DrawString(0, UI_LINE1_Y, line, 1);

  /* 行2（Y=38）：报警 + 字库状态（8x16 ASCII） */
  snprintf(line, sizeof(line), "ALM:%s", "OFF");
  SSD1306_DrawString(0, UI_LINE2_Y, line, 1);

  SSD1306_Refresh();
}

void SSD1306_UI_Refresh(void)
{
  SSD1306_Refresh();
}

/* -------------------------------------------------------------------------- */
/* MPU 三轴原始数据屏                                                          */
/* 约束：同样遵循"字符高度固定 16px、3 行、行间距 3px、底部 10px 留白"规格。   */
/* 128px 宽度约 11 字符/行，6 个轴(各 9 字符)放不下，故分两页每 2s 自动切换： */
/*   页0(ACC)  : AX= / AY= / AZ=                                            */
/*   页1(GYRO) : GX= / GY= / GZ=                                            */
/* 标签首字母 A/G 即区分加速度/陀螺仪。未连接时显示 "MPU N/A"。               */
void SSD1306_UI_MPU(void)
{
  SSD1306_Clear();

  if (!g_mpu_ok)
  {
    SSD1306_DrawString(0, UI_LINE0_Y, "MPU N/A", 1);
    SSD1306_Refresh();
    return;
  }

  /* 每 2s 在 加速度(ACC) / 陀螺仪(GYRO) 两页间切换 */
  uint8_t accel = ((g_systick_ms / 2000U) % 2U) == 0U;
  char line[16];

  if (accel)
  {
    snprintf(line, sizeof(line), "AX=%+6d", g_mpu.ax);
    SSD1306_DrawString(0, UI_LINE0_Y, line, 1);
    snprintf(line, sizeof(line), "AY=%+6d", g_mpu.ay);
    SSD1306_DrawString(0, UI_LINE1_Y, line, 1);
    snprintf(line, sizeof(line), "AZ=%+6d", g_mpu.az);
    SSD1306_DrawString(0, UI_LINE2_Y, line, 1);
  }
  else
  {
    snprintf(line, sizeof(line), "GX=%+6d", g_mpu.gx);
    SSD1306_DrawString(0, UI_LINE0_Y, line, 1);
    snprintf(line, sizeof(line), "GY=%+6d", g_mpu.gy);
    SSD1306_DrawString(0, UI_LINE1_Y, line, 1);
    snprintf(line, sizeof(line), "GZ=%+6d", g_mpu.gz);
    SSD1306_DrawString(0, UI_LINE2_Y, line, 1);
  }

  SSD1306_Refresh();
}

/* 主循环每圈调用：状态屏(SSD1306_UI_Demo) 与 MPU 屏(SSD1306_UI_MPU) 每 4s 交替；
 * MPU 屏数据变化快，每 500ms 重绘；状态屏每 1s 重绘即可（原 main.c 调度逻辑下沉）。 */
void SSD1306_UI_Process(void)
{
  static uint32_t s_oled_ms = 0;    /* 屏幕切换节拍 */
  static uint32_t s_oled_draw = 0;  /* 当前屏重绘节拍 */
  static uint8_t  s_oled_mode = 0;  /* 0=状态屏, 1=MPU屏 */

  if (g_systick_ms - s_oled_ms >= 4000U)
  {
    s_oled_ms = g_systick_ms;
    s_oled_mode ^= 1U;
  }
  /* MPU 屏数据变化快，每 500ms 重绘；状态屏每 1s 重绘即可 */
  uint32_t period = (s_oled_mode == 1U) ? 500U : 1000U;
  if (g_systick_ms - s_oled_draw >= period)
  {
    s_oled_draw = g_systick_ms;
    if (s_oled_mode == 1U)
      SSD1306_UI_MPU();
    else
      SSD1306_UI_Demo();
  }
}
