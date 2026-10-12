/**
  ******************************************************************************
  * @file    ui.h
  * @brief   OLED 显示 UI 层：解耦"显示什么"与"怎么画点/取字模"
  ******************************************************************************
  *
  * SSD1306 驱动只负责画点/刷屏；这里组织成具体的界面（标题、传感器行等）。
  * 后续阶段要做"状态屏/记录条数/报警提示"，都加在这里，不污染底层驱动。
  */
#ifndef UI_H
#define UI_H

#include <stdint.h>

/* 对外接口（实现见 ui.c）
 * - UI_Init  : 初始化 OLED（内部调 SSD1306_Init）
 * - UI_Demo  : 绘制状态屏（中英文混排）并立即刷屏，用于验证字库链路
 * - UI_MPU   : 绘制 MPU 三轴原始数据屏（ACC/EGYRO 两页每 2s 自动切换），并刷屏
 * - UI_Refresh: 仅重刷当前显存（内容由 UI_Demo/UI_MPU 等写入）
 */
void UI_Init(void);
void UI_Demo(void);
void UI_MPU(void);
void UI_Refresh(void);

#endif /* UI_H */
