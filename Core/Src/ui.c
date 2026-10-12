/**
  ******************************************************************************
  * @file    ui.c
  * @brief   OLED 显示 UI 层（中英文混排演示屏）
  ******************************************************************************
  *
  * 布局规格（字符高度固定 16px）：
  *   共 3 行，行间距 OLED_LINE_GAP = 3px，底部留白 64-(38+16) = 10px
  *     行0 Y = 0
  *     行1 Y = 0 + 16 + 3 = 19
  *     行2 Y = 19 + 16 + 3 = 38
  *   汉字 16px(宽)x16px(高)；ASCII/数字/符号 8px(宽)x16px(高)
  *   （由 SSD1306_DrawString 按 GB2312 自动区分：双字节走 16x16、单字节走 8x16）
  *
  * 重要：字库是 GB2312 编码，而 C 源文件是 UTF-8，字符串字面量里的"中文"是
  * UTF-8 字节、与字库对不上。因此中文标题用 Python 预先转成 GB2312 字节数组
  * （见下方 s_title_gb），再交给 SSD1306_DrawString 按 GB2312 解析。
  * 纯英文/数字串用普通字面量即可（ASCII 部分字节一致）。
  */
#include "ui.h"
#include "ssd1306.h"
#include "w25qxx.h"     /* W25QXX_FontReady：字库是否已烧录 */
#include "mpu6050.h"    /* 阶段6：MPU6050_RawData 类型 + WHO_AM_I 常量 */
#include "stm32f1xx_hal.h"
#include <stdio.h>

extern volatile uint32_t g_systick_ms;   /* main.c 定义的 1ms 节拍计数 */
extern MPU6050_RawData g_mpu;            /* main.c 持续采样的最新原始数据 */
extern uint8_t g_mpu_ok;                 /* main.c：MPU 是否已唤醒并识别成功 */

/* ---- OLED 文本布局规格（字符高度固定 16px）---- */
#define OLED_LINE_GAP  3       /* 行间距 */
#define UI_LINE_H      16      /* 字符高度（汉字/英文 同行高） */
#define UI_LINE0_Y     0
#define UI_LINE1_Y     (UI_LINE0_Y + UI_LINE_H + OLED_LINE_GAP)   /* 0+16+3 = 19 */
#define UI_LINE2_Y     (UI_LINE1_Y + UI_LINE_H + OLED_LINE_GAP)   /* 19+16+3 = 38 */
/* 总占用：UI_LINE2_Y + UI_LINE_H = 38+16 = 54px；屏幕 64px，底部留白 10px */

/* 迷你黑匣子 -> GB2312(C3 D4 C4 E3 BA DA CF BB D7 D3) */
static const uint8_t s_title_gb[] = { 0xC3, 0xD4, 0xC4, 0xE3, 0xBA, 0xDA, 0xCF, 0xBB, 0xD7, 0xD3 };

void UI_Init(void)
{
  SSD1306_Init();
}

void UI_Demo(void)
{
  SSD1306_Clear();

  /* 行0（Y=0）：16x16 中文标题 */
  SSD1306_DrawString(0, UI_LINE0_Y, (const char *)s_title_gb, 1);

  /* 行1（Y=19）：版本 + 运行时长（8x16 ASCII） */
  char line[24];
  unsigned long sec = (unsigned long)(g_systick_ms / 1000u);
  snprintf(line, sizeof(line), "MBB v1.0 %lu s", sec);
  SSD1306_DrawString(0, UI_LINE1_Y, line, 1);

  /* 行2（Y=38）：报警 + 字库状态（8x16 ASCII） */
  snprintf(line, sizeof(line), "ALM:%s F:%s",
           "OFF", W25QXX_FontReady() ? "READY" : "--");
  SSD1306_DrawString(0, UI_LINE2_Y, line, 1);

  SSD1306_Refresh();
}

void UI_Refresh(void)
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
void UI_MPU(void)
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
