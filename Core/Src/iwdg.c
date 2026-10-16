/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    iwdg.c
  * @brief   This file provides code for the configuration
  *          of the IWDG instances.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "iwdg.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

IWDG_HandleTypeDef hiwdg;

/* IWDG init function */
void MX_IWDG_Init(void)
{

  /* USER CODE BEGIN IWDG_Init 0 */

  /* USER CODE END IWDG_Init 0 */

  /* USER CODE BEGIN IWDG_Init 1 */
  /* 注：原先在此处写 `hiwdg.Init.Reload = 234;` 是无效的——它位于下方生成行
   * `hiwdg.Init.Reload = 4095;` 之前，会被后者覆盖。真正的覆盖已改到本函数末尾
   * 的 USER CODE IWDG_Init 2 块内，在 HAL_IWDG_Init() 之后直接写硬件寄存器实现。 */
  /* USER CODE END IWDG_Init 1 */
  hiwdg.Instance = IWDG;
  hiwdg.Init.Prescaler = IWDG_PRESCALER_256;
  hiwdg.Init.Reload = 234;
  if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN IWDG_Init 2 */
  /* CubeMX 图形界面里 "IWDG down-counter reload value" 字段被锁定（齿轮图标）无法在 GUI
   * 把 4095 改成 234。因此改在这里——HAL_IWDG_Init() 之后直接写硬件寄存器，把看门狗超时
   * 从 ~26s 缩短到 ~1.5s（LSI 40kHz，预分频 256 → 6.4ms/格，234×6.4ms ≈ 1.5s）。
   * 全程不触碰 CubeMX 生成区。若日后 CubeMX 该字段解锁并设为 234，本段即可整段删除。 */
  IWDG->KR  = 0x00005555U;        /* 解开 IWDG_PR / IWDG_RLR 写保护 */
  IWDG->RLR = 234U;               /* 重装载值 = 234（约 1.5s） */
  while (IWDG->SR != 0U) { }      /* 等待 RLR/PR 更新同步完成（RVU/PVU 清零） */
  IWDG->KR  = 0x0000AAAAU;        /* 立即喂一次狗，按新值重新计数 */
  /* USER CODE END IWDG_Init 2 */

}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

