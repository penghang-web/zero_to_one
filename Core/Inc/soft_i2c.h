/**
  ******************************************************************************
  * @file    soft_i2c.h
  * @brief   软件 I2C（bit-bang）驱动头文件
  *
  * 用 PB6(SCL) / PB7(SDA) 开漏 GPIO 模拟 I2C，替代 STM32F1 硬件 I2C。
  * 硬件 I2C 在从机拉低 SDA 时会 BUSY 锁死，软件 I2C 无此缺陷；
  * 且本工程 I2C 总线被 OLED 与 MPU6050 两任务共享，访问串行化由上层（i2c.c）
  * 的 FreeRTOS 互斥锁负责，本文件只管"按时序打出 SCL/SDA"。
  *
  * 引脚用内部上拉（GPIO_PULLUP）即可，无需外部 4.7kΩ；若 OLED 模块自带板上上拉则并联更稳。
  ******************************************************************************
  */
#ifndef SOFT_I2C_H
#define SOFT_I2C_H

#include <stdint.h>
#include "stm32f1xx_hal.h"

/* 内存地址宽度宏。
 * 这两个宏原本由 HAL I2C 头文件(stm32f1xx_hal_i2c.h)在 HAL_I2C_MODULE_ENABLED
 * 定义时提供；本工程已禁用硬件 I2C(I2C1)，该头文件不再被包含，故在此等价定义，
 * 使 ssd1306.c / mpu6050.c 调用 I2C1_SafeMemWrite/Read 时无需任何改动。
 * 取值与 STM32 HAL 库完全一致。 */
#ifndef I2C_MEMADD_SIZE_8BIT
#define I2C_MEMADD_SIZE_8BIT    0x00000001U
#endif
#ifndef I2C_MEMADD_SIZE_16BIT
#define I2C_MEMADD_SIZE_16BIT   0x00000010U
#endif

/* 配置 PB6/PB7 为开漏输出并释放总线（高电平）。CubeMX 禁用 I2C1 后 PB6/PB7 为普通 GPIO。 */
void SOFT_I2C_Init(void);

/* 与 HAL_I2C_Mem_Write / HAL_I2C_Mem_Read 同语义：
 *   DevAddress —— 8 位地址（已左移，HAL 约定），如 0x78(OLED) / 0xD0(MPU6050)
 *   MemAddress —— 寄存器 / 控制字节地址
 *   MemAddSize —— 仅支持 I2C_MEMADD_SIZE_8BIT（本项目全部为 8 位）
 * 返回 HAL_OK / HAL_ERROR（从机不应答或参数错误）。 */
HAL_StatusTypeDef soft_i2c_mem_write(uint8_t DevAddress, uint16_t MemAddress,
                                     uint16_t MemAddSize, const uint8_t *pData, uint16_t Size);
HAL_StatusTypeDef soft_i2c_mem_read(uint8_t DevAddress, uint16_t MemAddress,
                                    uint16_t MemAddSize, uint8_t *pData, uint16_t Size);

#endif /* SOFT_I2C_H */
