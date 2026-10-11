/**
  ******************************************************************************
  * @file    key.h
  * @brief   按键模块对外接口（状态机 + LED/蜂鸣器报警行为）
  ******************************************************************************
  */
#ifndef KEY_H
#define KEY_H

#include <stdint.h>

/* 按键模块对外接口，实现见 key.c
 * - Key_Init()        : 初始化模块状态（状态机/报警标志清零），main 初始化阶段调用一次
 * - Key_Process()     : 主循环每圈调用，非阻塞地推进状态机并驱动 LED/蜂鸣器
 * - Key_EXTI_Callback : 由 HAL_GPIO_EXTI_Callback 路由进来，ISR 里只置边沿标志
 * - Key_GetAlarmState : 读取当前报警状态（0=正常，1=报警），供其他模块查询
 */
void     Key_Init(void);
void     Key_Process(void);
void     Key_EXTI_Callback(uint16_t GPIO_Pin);
uint8_t  Key_GetAlarmState(void);

#endif /* KEY_H */
