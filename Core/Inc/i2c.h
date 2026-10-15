/**
  ******************************************************************************
  * @file    i2c.h
  * @brief   I2C1 总线安全读写封装（软件 I2C 实现 + FreeRTOS 互斥锁串行化）
  ******************************************************************************
  */
#ifndef I2C_H
#define I2C_H

#include <stdint.h>
#include "stm32f1xx_hal.h"
#include "soft_i2c.h"   /* 透传 I2C_MEMADD_SIZE_8BIT/16BIT 宏给 ssd1306.c / mpu6050.c */

/* I2C1 总线安全读写封装。
 * - 底层走软件 I2C（soft_i2c.c 的 bit-bang，PB6=SCL / PB7=SDA 开漏），
 *   取代 STM32F1 硬件 I2C，消除从机拉低 SDA 时的 BUSY 锁死。
 * - 访问由 FreeRTOS 互斥锁串行化，避免 OLED 任务与 MPU6050 任务（共用 PB6/PB7）
 *   交错传输导致花屏。
 * - 内核启动前 i2c_mutexHandle=NULL，本封装自动跳过加锁，兼容
 *   SSD1306_UI_Init / MPU6050_Begin 在 osKernelStart() 之前的初始化调用。
 * 接口语义与 HAL_I2C_Mem_Write / HAL_I2C_Mem_Read 一致。 */
HAL_StatusTypeDef I2C1_SafeMemWrite(uint8_t DevAddress, uint16_t MemAddress,
                                    uint16_t MemAddSize, const uint8_t *pData, uint16_t Size);
HAL_StatusTypeDef I2C1_SafeMemRead(uint8_t DevAddress, uint16_t MemAddress,
                                   uint16_t MemAddSize, uint8_t *pData, uint16_t Size);

#endif /* I2C_H */
