/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    i2c.c
  * @brief   This file provides code for the configuration
  *          of the I2C instances.
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
#include "i2c.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

I2C_HandleTypeDef hi2c1;

/* I2C1 init function */
void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 100000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

void HAL_I2C_MspInit(I2C_HandleTypeDef* i2cHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(i2cHandle->Instance==I2C1)
  {
  /* USER CODE BEGIN I2C1_MspInit 0 */

  /* USER CODE END I2C1_MspInit 0 */

    __HAL_RCC_GPIOB_CLK_ENABLE();
    /**I2C1 GPIO Configuration
    PB6     ------> I2C1_SCL
    PB7     ------> I2C1_SDA
    */
    GPIO_InitStruct.Pin = GPIO_PIN_6|GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* I2C1 clock enable */
    __HAL_RCC_I2C1_CLK_ENABLE();
  /* USER CODE BEGIN I2C1_MspInit 1 */

  /* USER CODE END I2C1_MspInit 1 */
  }
}

void HAL_I2C_MspDeInit(I2C_HandleTypeDef* i2cHandle)
{

  if(i2cHandle->Instance==I2C1)
  {
  /* USER CODE BEGIN I2C1_MspDeInit 0 */

  /* USER CODE END I2C1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_I2C1_CLK_DISABLE();

    /**I2C1 GPIO Configuration
    PB6     ------> I2C1_SCL
    PB7     ------> I2C1_SDA
    */
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_6);

    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_7);

  /* USER CODE BEGIN I2C1_MspDeInit 1 */

  /* USER CODE END I2C1_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* TIM2 1ms 节拍（main.c 维护），用于 BusRecovery 冷却限频 */
extern volatile uint32_t g_systick_ms;

/* I2C 单次传输超时：调小到 50ms，既保证总线偶发卡顿时不会长时间阻塞主循环，
 * 又给安全读写/重试留出余量（失败 1 次 + 重试 1 次最坏 ~100ms + 恢复开销）。 */
#define I2C_TIMEOUT_MS   50

/* BusRecovery 冷却：避免总线持续锁死时"每次调用都 DeInit/Init 重活"雪崩，
 * 把单圈 I2C 阻塞时间压在门控窗口内。冷却期内直接快速失败。 */
static uint32_t s_last_recovery_ms = 0;

/* ===========================================================================
 * STM32F1 I2C 总线恢复与安全读写
 * ---------------------------------------------------------------------------
 * STM32F1 的 I2C 外设有已知缺陷：一旦从机拉低 SDA（时钟拉伸/噪声/突发读被打断），
 * I2C_FLAG_BUSY 会卡死，HAL 的 Mem_Read/Write 每次都等到超时返回却永不恢复，
 * 进而导致所有共用该总线的设备（OLED + MPU6050）全部失效、主循环被阻塞卡死。
 *
 * 标准恢复手法：用 GPIO 模拟 9 个 SCL 脉冲，让从机把 SDA 释放出来，再产生 STOP，
 * 最后 DeInit/Init 复位外设（MspInit 会把引脚重新配成 AF_OD）。
 * ========================================================================= */
void I2C1_BusRecovery(void)
{
  GPIO_InitTypeDef gpio = {0};

  /* 1. 关闭 I2C 外设，避免恢复过程中硬件状态干扰 */
  __HAL_I2C_DISABLE(&hi2c1);

  /* 2. 把 SCL/PB6、SDA/PB7 临时配成开漏输出，由我们手动产生时钟 */
  gpio.Pin  = GPIO_PIN_6 | GPIO_PIN_7;
  gpio.Mode = GPIO_MODE_OUTPUT_OD;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &gpio);

  /* 先释放 SDA（输出高），SCL 也拉高 */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
  for (volatile uint32_t d = 0; d < 200; d++);

  /* 3. 产生 9 个 SCL 脉冲，让仍占用总线的从机把数据位全部移出、松开 SDA */
  for (int i = 0; i < 9; i++)
  {
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_RESET);
    for (volatile uint32_t d = 0; d < 200; d++);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
    for (volatile uint32_t d = 0; d < 200; d++);
  }

  /* 4. 产生 STOP 条件：SCL 为高时，SDA 由低变高 */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_RESET);
  for (volatile uint32_t d = 0; d < 200; d++);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);
  for (volatile uint32_t d = 0; d < 200; d++);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7, GPIO_PIN_SET);
  for (volatile uint32_t d = 0; d < 200; d++);

  /* 5. 复位 I2C 外设（HAL_I2C_Init 内部会调用 MspInit 重新把引脚配成 AF_OD） */
  HAL_I2C_DeInit(&hi2c1);
  HAL_I2C_Init(&hi2c1);
}

/* 安全 I2C 写：传输前若总线 BUSY 则先恢复；首次失败再恢复并重试一次。
 * 冷却期内（距上次恢复 <300ms）不重复重活，直接快速失败，交由看门狗门控兜底。 */
HAL_StatusTypeDef I2C1_SafeMemWrite(uint16_t DevAddress, uint16_t MemAddress,
                                    uint16_t MemAddSize, uint8_t *pData, uint16_t Size)
{
  if (__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_BUSY))
  {
    if (g_systick_ms - s_last_recovery_ms >= 300U)
    {
      I2C1_BusRecovery();
      s_last_recovery_ms = g_systick_ms;
    }
    else
    {
      return HAL_BUSY;   /* 冷却期内直接失败，避免主循环被反复恢复拖死 */
    }
  }
  HAL_StatusTypeDef st = HAL_I2C_Mem_Write(&hi2c1, DevAddress, MemAddress,
                                          MemAddSize, pData, Size, I2C_TIMEOUT_MS);
  if (st != HAL_OK)
  {
    if (g_systick_ms - s_last_recovery_ms >= 300U)
    {
      I2C1_BusRecovery();
      s_last_recovery_ms = g_systick_ms;
    }
    st = HAL_I2C_Mem_Write(&hi2c1, DevAddress, MemAddress,
                           MemAddSize, pData, Size, I2C_TIMEOUT_MS);
  }
  return st;
}

/* 安全 I2C 读：与写同理，失败自动恢复并重试 */
HAL_StatusTypeDef I2C1_SafeMemRead(uint16_t DevAddress, uint16_t MemAddress,
                                   uint16_t MemAddSize, uint8_t *pData, uint16_t Size)
{
  if (__HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_BUSY))
  {
    if (g_systick_ms - s_last_recovery_ms >= 300U)
    {
      I2C1_BusRecovery();
      s_last_recovery_ms = g_systick_ms;
    }
    else
    {
      return HAL_BUSY;
    }
  }
  HAL_StatusTypeDef st = HAL_I2C_Mem_Read(&hi2c1, DevAddress, MemAddress,
                                         MemAddSize, pData, Size, I2C_TIMEOUT_MS);
  if (st != HAL_OK)
  {
    if (g_systick_ms - s_last_recovery_ms >= 300U)
    {
      I2C1_BusRecovery();
      s_last_recovery_ms = g_systick_ms;
    }
    st = HAL_I2C_Mem_Read(&hi2c1, DevAddress, MemAddress,
                          MemAddSize, pData, Size, I2C_TIMEOUT_MS);
  }
  return st;
}

/* USER CODE END 1 */

