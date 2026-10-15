/**
  ******************************************************************************
  * @file    i2c.c
  * @brief   I2C1 总线安全读写封装实现（软件 I2C + FreeRTOS 互斥锁串行化）
  *
  * 说明：本文件为用户自有文件（非 CubeMX 生成区）。原 CubeMX 在 I2C1 启用时
  * 生成的 i2c.c/i2c.h（含 hi2c1 / MX_I2C1_Init）已随 I2C1 禁用被删除；
  * 这里改名为"总线安全封装"，底层改用软件 I2C，调用方（ssd1306.c / mpu6050.c）
  * 无需改动接口。
  ******************************************************************************
  */
#include "i2c.h"
#include "soft_i2c.h"
#include "cmsis_os.h"

/* 互斥锁在 freertos.c 中创建（RTOS_MUTEX 块）。此处仅 extern 引用。
 * 内核启动前它为 NULL，I2C1_SafeMem* 自动跳过加锁，兼容启动前初始化。 */
extern osMutexId i2c_mutexHandle;

HAL_StatusTypeDef I2C1_SafeMemWrite(uint8_t DevAddress, uint16_t MemAddress,
                                    uint16_t MemAddSize, const uint8_t *pData, uint16_t Size)
{
  if (i2c_mutexHandle != NULL)
  {
    osMutexWait(i2c_mutexHandle, osWaitForever);
  }
  HAL_StatusTypeDef st = soft_i2c_mem_write(DevAddress, MemAddress, MemAddSize, pData, Size);
  if (i2c_mutexHandle != NULL)
  {
    osMutexRelease(i2c_mutexHandle);
  }
  return st;
}

HAL_StatusTypeDef I2C1_SafeMemRead(uint8_t DevAddress, uint16_t MemAddress,
                                   uint16_t MemAddSize, uint8_t *pData, uint16_t Size)
{
  if (i2c_mutexHandle != NULL)
  {
    osMutexWait(i2c_mutexHandle, osWaitForever);
  }
  HAL_StatusTypeDef st = soft_i2c_mem_read(DevAddress, MemAddress, MemAddSize, pData, Size);
  if (i2c_mutexHandle != NULL)
  {
    osMutexRelease(i2c_mutexHandle);
  }
  return st;
}
