/**
  ******************************************************************************
  * @file    buzzer.c
  * @brief   有源蜂鸣器驱动（TIM3_CH4 PWM，active-low），从 key.c 独立出来
  ******************************************************************************
  */
#include "buzzer.h"
#include "tim.h"    /* htim3 句柄（extern），TIM3_CH4 PWM 驱动蜂鸣器 */

extern volatile uint32_t g_systick_ms;   /* TIM2 1ms 节拍，main.c 维护 */

/* ------------------------------------------------------------------ */
/* 模块私有状态（static 封装）                                          */
/* ------------------------------------------------------------------ */
static uint32_t s_beep_until = 0;   /* 一次性 beep 到期时间戳（由节拍关断） */
static uint8_t  s_hold      = 0;    /* 常响保持：报警态为 1，抑制一次性 beep 的自动关断 */

/* 蜂鸣器由 TIM3_CH4 PWM 驱动（active-low：比较值=ARR 全低=响，=0 静音）。
 * ARR 与极性已在 tim.c MX_TIM3_Init 的 USER CODE 内统一配置（regen 安全）。 */
void Buzzer_On(void)
{
  s_hold = 1;
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_4, htim3.Init.Period); /* 全占空比 -> 常响 */
}

void Buzzer_Off(void)
{
  s_hold       = 0;
  s_beep_until = 0;
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_4, 0);                 /* 占空比 0 -> 静音 */
}

/* 一次性蜂鸣：duty 0~100，响 ms 毫秒后自动停（非阻塞，由 Buzzer_Process 在节拍里关断） */
void Buzzer_Beep(uint16_t ms, uint8_t duty)
{
  uint32_t cmp = (htim3.Init.Period * duty) / 100u;
  __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_4, cmp);
  s_beep_until = g_systick_ms + ms;
}

/* 主循环每圈调用：处理一次性 beep 的到期自动关断（常响保持态不抢蜂鸣器） */
void Buzzer_Process(void)
{
  if (s_beep_until != 0 && g_systick_ms >= s_beep_until && !s_hold)
  {
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_4, 0);  /* 静音 */
    s_beep_until = 0;
  }
}
