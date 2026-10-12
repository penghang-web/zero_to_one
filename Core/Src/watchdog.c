/**
  ******************************************************************************
  * @file    watchdog.c
  * @brief   IWDG 喂狗模块实现。详见 watchdog.h。
  ******************************************************************************
  */
#include "watchdog.h"
#include "iwdg.h"        /* hiwdg (extern)，由 CubeMX 生成的 MX_IWDG_Init 初始化 */
#include "stm32f1xx_hal.h"

static volatile uint8_t s_feed_enable = 1;   /* 默认开启喂狗，系统正常运行即喂 */
static uint32_t        s_feed_count   = 0;   /* 喂狗累计次数（状态观测用） */

/* 喂狗：写 KR=0xAAAA 重装计数器，防止 IWDG 超时复位。
 * 仅在 s_feed_enable 为真时执行；ISR 调用，短小且无阻塞。 */
void WDG_Feed(void)
{
  if (s_feed_enable)
  {
    HAL_IWDG_Refresh(&hiwdg);
    s_feed_count++;
  }
}

/* 测试/守卫用：开/关喂狗。 */
void WDG_SetFeed(uint8_t on)
{
  s_feed_enable = on ? 1U : 0U;
}

uint8_t WDG_IsFeeding(void)
{
  return s_feed_enable;
}

uint32_t WDG_FeedCount(void)
{
  return s_feed_count;
}

/* 复位测试：停止喂狗并死循环。TIM2 节拍 ISR 因 s_feed_enable=0 不再喂狗，
 * IWDG 计数器递减到 0 后自动复位 MCU（默认约 1.5s），用于验证看门狗生效。 */
void WDG_TestHang(void)
{
  s_feed_enable = 0;
  while (1)
  {
    /* 故意不喂狗，等待看门狗超时复位 */
  }
}
