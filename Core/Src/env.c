/**
  ******************************************************************************
  * @file    env.c
  * @brief   Phase 7：ADC1 双通道(PA0 热敏 / PA1 光敏) + TIM1 触发 + DMA 循环采集
  ******************************************************************************
  *
  * 注意：本文件是【用户自有模块】，刻意不叫 adc.c —— 因为 CubeMX 启用 ADC1 后会
  *       生成同名 Core/Src/adc.c（含 MX_ADC1_Init），若同名会被覆盖。故以 env.c 命名。
  *
 * 设计要点：
 * - ADC1 规则组扫描 CH0(PA0) 与 CH1(PA1)，由 TIM1_CH1 的比较事件触发。
 *   注意：STM32F1 的 ADC1 规则组可选触发源里【没有 T1_TRGO】，只有
 *   T1_CC1 / T1_CC2 / T1_CC3 / T2_CC2 / T3_TRGO / T4_CC4 / EXTI11，
 *   因此走 "TIM1 CH1 = PWM Generation No Output" + "Timer 1 Capture Compare 1 event" 这条线。
 *   连续扫描 + DMA 循环搬运到 g_adc_dma，采集零 CPU 占用。
 * - DMA 半/全完成回调仅置标志（不调任何 RTOS FromISR API），由 envTask 消费。
 * - 应用层把原始值换算成温度趋势 / 光照强度，并做光照突变检测。
 * - 暴露 g_env 给阶段8/9 消费（OLED 显示已在本阶段接入）。
 */
#include "env.h"

/* CubeMX 生成的句柄（在 CubeMX 的 adc.c / tim.c 中定义），此处仅作 extern 引用，
 * 避免 include 其 adc.h/tim.h，从而与生成物彻底解耦、不碰生成区。 */
extern ADC_HandleTypeDef hadc1;   /* ADC1 句柄（MX_ADC1_Init 初始化） */
extern TIM_HandleTypeDef htim1;   /* TIM1 句柄（MX_TIM1_Init 初始化，CH1=PWM No Output 供 T1_CC1 触发） */

/* -------------------------------------------------------------------------- */
/* DMA 缓冲：双通道扫描，每帧 = [temp, light]。循环搬运，半/全完成分段处理。     */
static uint16_t g_adc_dma[ADC_DMA_LEN];

/* envTask 消费标志（由 ISR 置位，任务清位） */
static volatile uint8_t s_adc_half_done = 0;
static volatile uint8_t s_adc_full_done = 0;

/* 暴露给阶段8/9 的全局环境状态 */
Env_t g_env = {0};

/* -------------------------------------------------------------------------- */
/* 启动：TIM1_CH1 周期性触发 + ADC DMA 循环搬运（在 main.c USER CODE 2 调用） */
void ENV_Init(void)
{
  if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)g_adc_dma, ADC_DMA_LEN) != HAL_OK)
  {
    /* 启动失败：此处无日志系统直接可用，留待后续诊断；不影响其它任务启动 */
    return;
  }
  /* 启动 TIM1_CH1：CH1 工作在 PWM Mode 1 "No Output"（无引脚），
   * 每个周期产生一次 CH1 比较事件（CC1），ADC1 的 EXTSEL 选中该事件后
   * 即"每个 TIM1 周期触发一次规则组扫描"。
   *
   * 注意：当前 CubeMX 版本对 "No Output" PWM 通道【不会把 Pulse 写回代码】
   *（生成出的 tim.c 永远是 sConfigOC.Pulse = 0）。项目里 TIM3 蜂鸣器也是
   * 运行时用 __HAL_TIM_SET_COMPARE 设占空比，这里沿用同一套路：在启动前
   * 用代码把 CH1 比较值设为 500（ARR=999 → 占空比 50%，每周期产生一次 CC1 事件）。
   * 若这里不补这行，CCR1=0 → OC1REF 恒无效 → ADC 永远收不到触发。 */
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, 500);
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
}

/* -------------------------------------------------------------------------- */
/* ISR 上下文：仅置标志，绝不调用 RTOS FromISR API（遵守既有中断纪律） */
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1)
  {
    s_adc_half_done = 1;
  }
}
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1)
  {
    s_adc_full_done = 1;
  }
}

/* -------------------------------------------------------------------------- */
/* 对一段半缓冲求两通道平均，更新 g_env（在任务上下文调用） */
static void env_average_half(const uint16_t *p)
{
  uint32_t t_sum = 0U, l_sum = 0U;
  const uint16_t frames = ADC_DMA_HALF / ADC_FRAME_LEN;   /* 8 帧 */
  for (uint16_t i = 0U; i < ADC_DMA_HALF; i += ADC_FRAME_LEN)
  {
    t_sum += p[i + ADC_CH_TEMP];
    l_sum += p[i + ADC_CH_LIGHT];
  }
  const uint16_t t_avg = (uint16_t)(t_sum / frames);
  const uint16_t l_avg = (uint16_t)(l_sum / frames);

  int16_t temp  = ADC_ToTemp(t_avg);
  int16_t light = ADC_ToLight(l_avg);

  g_env.temp_raw       = t_avg;
  g_env.light_raw      = l_avg;
  g_env.temp_trend     = temp;
  g_env.light_sudden   = Light_SuddenChangeDetect(g_env.light_lvl_prev, light);
  g_env.light_lvl      = light;
  g_env.light_lvl_prev = light;
  g_env.ready = 1;
}

/* envTask 循环调用：消费半/全完成标志 */
void ENV_Process(void)
{
  if (s_adc_half_done)
  {
    s_adc_half_done = 0;
    env_average_half(&g_adc_dma[0]);
  }
  if (s_adc_full_done)
  {
    s_adc_full_done = 0;
    env_average_half(&g_adc_dma[ADC_DMA_HALF]);
  }
}

/* -------------------------------------------------------------------------- */
/* 物理量换算（阶段7：未绝对标定，仅作趋势；定标留待阶段9 报警整合） */
int16_t ADC_ToTemp(uint16_t raw)
{
  /* 常见接法：NTC 与固定上拉构成分压，温度↑→R_ntc↓→Vadc↓→raw↓。
   * 为使"手捂热敏→读数上升"，这里用 (4095 - raw) 映射。
   * 若实测方向相反，把下面一行符号翻转即可（定标前）。 */
  int32_t t = ((int32_t)4095 - (int32_t)raw) * 400 / 4095;  /* 0..400（单位 0.1°C，约 0.0~40.0°C） */
  return (int16_t)t;
}

int16_t ADC_ToLight(uint16_t raw)
{
  /* 常见接法：光敏(LDR)与固定上拉分压，光照↑→R_ldr↓→Vadc↓→raw↓。
   * 为使"手遮光敏→读数明显下降"，直接用 raw 映射（raw 越小越暗）。 */
  int32_t l = (int32_t)raw * 100 / 4095;   /* 0..100（光照强度百分比） */
  return (int16_t)l;
}

uint8_t Light_SuddenChangeDetect(int16_t prev, int16_t now)
{
  if ((now > prev + LIGHT_SUDDEN_DELTA) || (now < prev - LIGHT_SUDDEN_DELTA))
  {
    return 1;
  }
  return 0;
}
