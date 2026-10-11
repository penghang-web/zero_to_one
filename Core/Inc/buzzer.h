/**
  ******************************************************************************
  * @file    buzzer.h
  * @brief   有源蜂鸣器驱动（TIM3_CH4 PWM，active-low），从 key.c 独立成模块
  ******************************************************************************
  */
#ifndef BUZZER_H
#define BUZZER_H

#include <stdint.h>

/* 蜂鸣器模块对外接口，实现见 buzzer.c
 * - Buzzer_On()     : 常响（报警态用），全占空比，直到 Buzzer_Off
 * - Buzzer_Off()    : 静音，并解除常响保持、清一次性 beep 计时
 * - Buzzer_Beep()   : 一次性蜂鸣，duty 0~100，响 ms 毫秒后由 Buzzer_Process 自动停
 * - Buzzer_Process(): 主循环每圈调用，处理一次性 beep 的到期自动关断（非阻塞）
 * 依赖：htim3（由 main.c 启动 HAL_TIM_PWM_Start）与 g_systick_ms（TIM2 1ms 节拍）
 */
void Buzzer_On(void);
void Buzzer_Off(void);
void Buzzer_Beep(uint16_t ms, uint8_t duty);
void Buzzer_Process(void);

#endif /* BUZZER_H */
