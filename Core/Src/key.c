/**
  ******************************************************************************
  * @file    key.c
  * @brief   按键状态机 + LED/蜂鸣器报警行为（模块私有，不污染全局命名空间）
  ******************************************************************************
  */
#include "key.h"
#include "main.h"   /* LED_PC13 / KEY_EXTI / BUZZER 引脚宏，由 CubeMX 生成 */

/* ------------------------------------------------------------------ */
/* 模块私有状态（static 封装）                                          */
/* ------------------------------------------------------------------ */
typedef enum
{
  KEY_IDLE = 0,       /* 空闲，可响应新的按下                       */
  KEY_WAIT_RELEASE,   /* 已确认按下并翻转，等待松开（忽略抖动边沿）   */
  KEY_DEBOUNCE_UP     /* 检测到松开，做 15ms 松开消抖后回到空闲        */
} key_state_t;

static key_state_t s_key_state   = KEY_IDLE;
static volatile uint8_t s_key_irq = 0;   /* EXTI 边沿标志：ISR 置位，主循环消费 */
static uint8_t    s_alarm_state  = 0;    /* 报警状态：0=正常，1=报警（蜂鸣器响+LED常亮） */
static uint32_t   s_key_tick     = 0;    /* 松开消抖计时起点 */

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */
void Key_Init(void)
{
  s_key_state   = KEY_IDLE;
  s_key_irq     = 0;
  s_alarm_state = 0;
  s_key_tick    = 0;
}

uint8_t Key_GetAlarmState(void)
{
  return s_alarm_state;
}

/* EXTI 回调入口：ISR 保持极简，只置边沿标志（契合"中断收数、主循环处理"原则） */
void Key_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == KEY_EXTI_Pin)
  {
    s_key_irq = 1;
  }
}

/* 有源蜂鸣器：低电平响、高电平停（与 .ioc 初始高电平 GPIO_PIN_SET 一致） */
static void Buzzer_On(void)
{
  HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_RESET);
}

static void Buzzer_Off(void)
{
  HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_SET);
}

/* 主循环调用：非阻塞按键状态机 + LED/蜂鸣器行为
 * 设计要点：按下瞬间（下降沿）立即翻转报警状态，不依赖"消抖后读引脚低才翻转"，
 *          因此短按、长按都能可靠触发；松开与消抖用轮询 + HAL_GetTick() 非阻塞完成。 */
void Key_Process(void)
{
  switch (s_key_state)
  {
    case KEY_IDLE:
      if (s_key_irq)                       /* 下降沿 = 用户按下，立即认定并翻转 */
      {
        s_key_irq = 0;
        s_alarm_state ^= 1;                /* 翻转报警状态 */
        if (s_alarm_state)
        {
          Buzzer_On();                     /* 报警：蜂鸣器响 */
          HAL_GPIO_WritePin(LED_PC13_GPIO_Port, LED_PC13_Pin, GPIO_PIN_RESET); /* LED 常亮 */
        }
        else
        {
          Buzzer_Off();                    /* 报警解除：静音 */
        }
        s_key_state = KEY_WAIT_RELEASE;    /* 锁住，忽略抖动边沿，直到松开 */
      }
      break;

    case KEY_WAIT_RELEASE:
      if (HAL_GPIO_ReadPin(KEY_EXTI_GPIO_Port, KEY_EXTI_Pin) == GPIO_PIN_SET)
      {                                    /* 轮询发现已松开（非阻塞） */
        s_key_tick = HAL_GetTick();
        s_key_state = KEY_DEBOUNCE_UP;
      }
      break;

    case KEY_DEBOUNCE_UP:
      if (HAL_GetTick() - s_key_tick >= 15)/* 松开消抖 15ms */
      {
        s_key_irq = 0;                     /* 清掉抖动期间可能残留的边沿标志 */
        s_key_state = KEY_IDLE;            /* 解锁，准备下一次按下 */
      }
      break;
  }

  /* ---- LED 闪烁 / 报警态常亮 ---- */
  if (s_alarm_state)
  {
    HAL_Delay(100);                        /* 报警态：LED 保持常亮，轮询等待下次按键 */
  }
  else
  {
    HAL_GPIO_TogglePin(LED_PC13_GPIO_Port, LED_PC13_Pin); /* 正常态：1 Hz 闪烁 = 系统存活 */
    HAL_Delay(500);
  }
}
