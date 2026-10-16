/**
  ******************************************************************************
  * @file    env.h
  * @brief   Phase 7：ADC1 双通道(PA0 热敏 / PA1 光敏) + TIM1 触发 + DMA 循环采集
  ******************************************************************************
  *
  * 注意：本文件是【用户自有模块】，刻意不叫 adc.h —— 因为 CubeMX 启用 ADC1 后会
  *       生成同名 Core/Inc/adc.h / Core/Src/adc.c（含 MX_ADC1_Init 与 hadc1 句柄），
  *       若同名会被覆盖。故本模块以 env.* 命名，避免与生成物冲突。
  *
  * 依赖：CubeMX 生成的 hadc1（ADC1，扫描 IN0/IN1）、htim1（TIM1，CH1=PWM No Output，
  *       供 ADC1 的 "Timer 1 Capture Compare 1 event" 触发）、
  *       hdma_adc1（DMA1_Channel1，Peripheral->Memory 循环搬运）。
  * 设计要点：
  * - ADC1 规则组扫描 CH0(PA0 热敏) 与 CH1(PA1 光敏)，由 TIM1_CH1 比较事件周期性触发，
  *   连续扫描 + DMA 循环搬运到内存，采集零 CPU 占用。
  * - DMA 半/全完成回调仅置标志（不调任何 RTOS FromISR API），由 envTask 消费。
  * - 应用层把原始值换算成温度趋势 / 光照强度，并做光照突变检测。
  * - 暴露 g_env 给阶段8/9 消费（OLED 显示已在本阶段接入）。
  */
#ifndef ENV_H
#define ENV_H

#include <stdint.h>
#include "stm32f1xx_hal.h"

/* 采集通道顺序 = ADC1 规则组扫描顺序：rank1=CH0(PA0 热敏) -> rank2=CH1(PA1 光敏) */
#define ADC_CH_TEMP    0   /* 缓冲内相对位置：每帧第 0 个 = 热敏 */
#define ADC_CH_LIGHT   1   /* 缓冲内相对位置：每帧第 1 个 = 光敏 */

/* DMA 缓冲：双通道扫描，每帧 = [temp, light]。循环搬运，半/全完成分段处理。
 * 半缓冲 = 8 帧，TIM1 触发一次扫描一帧；TIM1 当前为 10Hz（PSC=7199/ARR=999），
 * 故约 0.8s 更新一次，平滑够用。 */
#define ADC_FRAME_LEN  2                       /* 一帧 = 2 通道 */
#define ADC_DMA_HALF   16                      /* 半缓冲样本数（= 8 帧） */
#define ADC_DMA_LEN    (ADC_DMA_HALF * 2)      /* 全缓冲样本数 = 32 */

/* 光照突变检测阈值（百分比点），在 Light_SuddenChangeDetect 内使用 */
#define LIGHT_SUDDEN_DELTA  20

/* 暴露给阶段8/9 的全局环境状态（envTask 写，OLED/console 任务读） */
typedef struct
{
  uint16_t temp_raw;        /* 最近一次温度通道平均原始值(0..4095) */
  uint16_t light_raw;       /* 最近一次光照通道平均原始值(0..4095) */
  int16_t  temp_trend;      /* 温度趋势值（单位 0.1°C，阶段7未绝对标定，仅趋势） */
  int16_t  light_lvl;       /* 光照强度（0..100 百分比） */
  int16_t  light_lvl_prev;  /* 上一次光照值，供突变检测 */
  uint8_t  light_sudden;    /* 光照突变标志（Light_SuddenChangeDetect 置位） */
  uint8_t  ready;           /* 至少完成一次完整转换 = 1 */
} Env_t;

extern Env_t g_env;

/* 启动：TIM1 触发 + ADC DMA 循环搬运（在 main.c USER CODE 2 调用，须 ADC1/TIM1 已 Init） */
void ENV_Init(void);

/* envTask 循环调用：消费半/全完成标志，换算 + 突变检测，更新 g_env */
void ENV_Process(void);

/* 物理量换算（阶段7：未绝对标定，仅作趋势；定标留待阶段9 报警整合） */
int16_t ADC_ToTemp(uint16_t raw);    /* raw(0..4095) -> 温度趋势(0.1°C) */
int16_t ADC_ToLight(uint16_t raw);   /* raw(0..4095) -> 光照强度(0..100) */
uint8_t Light_SuddenChangeDetect(int16_t prev, int16_t now);  /* 突变判定，返回 0/1 */

#endif /* ENV_H */
