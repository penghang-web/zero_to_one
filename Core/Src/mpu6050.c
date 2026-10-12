/**
  ******************************************************************************
  * @file    mpu6050.c
  * @brief   MPU6050 / 兼容器件（WHO_AM_I=0x74）I2C 驱动实现
  * @note    基于 STM32 HAL 库，使用 I2C1 总线（hi2c1）。
  *
  *  工作流程：上电 → 唤醒(PWR_MGMT_1 清 SLEEP) → 配置量程/DLPF → 循环读取。
  *  若不预先唤醒，读取出来的数据全部为 0xFF。
  ******************************************************************************
  */
#include "mpu6050.h"
#include "main.h"   /* 工程全局头（含引脚/外设句柄声明） */
#include "i2c.h"    /* hi2c1 外设句柄（extern） */

/* -------------------------------------------------------------------------- */
/* 内部辅助：单寄存器写                                                        */
/* -------------------------------------------------------------------------- */
static HAL_StatusTypeDef MPU6050_WriteReg(uint8_t reg, uint8_t val)
{
  return HAL_I2C_Mem_Write(&hi2c1, MPU6050_ADDR, reg,
                           I2C_MEMADD_SIZE_8BIT, &val, 1U,
                           MPU6050_I2C_TIMEOUT);
}

/* -------------------------------------------------------------------------- */
/* 内部辅助：单寄存器读                                                        */
/* -------------------------------------------------------------------------- */
static HAL_StatusTypeDef MPU6050_ReadReg(uint8_t reg, uint8_t *val)
{
  return HAL_I2C_Mem_Read(&hi2c1, MPU6050_ADDR, reg,
                          I2C_MEMADD_SIZE_8BIT, val, 1U,
                          MPU6050_I2C_TIMEOUT);
}

/* -------------------------------------------------------------------------- */
HAL_StatusTypeDef MPU6050_Init(void)
{
  HAL_StatusTypeDef st;

  /* ---- 步骤1：唤醒器件 ----
   * PWR_MGMT_1 的 SLEEP 位(位6)上电默认为 1（休眠）。
   * 必须写入 0x00：清 SLEEP + 选择内部 8MHz 振荡器(CLKSEL=0)。
   * 若不唤醒，后续所有读取都会返回 0xFF。 */
  st = MPU6050_WriteReg(MPU6050_REG_PWR_MGMT_1, 0x00U);
  if (st != HAL_OK)
  {
    return st;   /* I2C 通信异常，直接返回，不再继续配置 */
  }

  /* 唤醒后需等待时钟稳定（典型数十 ms），这里保守延时 100ms */
  HAL_Delay(100U);

  /* ---- 步骤2：采样率分频（SMPLRT_DIV） ----
   * 采样率 = 陀螺仪输出速率 / (1 + SMPLRT_DIV)。
   * 开启 DLPF 后陀螺仪输出速率为 1kHz，设 0x07 → 约 125Hz。 */
  st = MPU6050_WriteReg(MPU6050_REG_SMPLRT_DIV, 0x07U);
  if (st != HAL_OK) { return st; }

  /* ---- 步骤3：开启 DLPF 低通滤波（CONFIG） ----
   * DLPF_CFG=1：加速度计带宽 184Hz、陀螺仪带宽 188Hz（使能低通滤波）。 */
  st = MPU6050_WriteReg(MPU6050_REG_CONFIG, 0x01U);
  if (st != HAL_OK) { return st; }

  /* ---- 步骤4：陀螺仪量程 ±250dps（GYRO_CONFIG） ----
   * FS_SEL[4:3]=00 → ±250dps。 */
  st = MPU6050_WriteReg(MPU6050_REG_GYRO_CONFIG, 0x00U);
  if (st != HAL_OK) { return st; }

  /* ---- 步骤5：加速度计量程 ±2g（ACCEL_CONFIG） ----
   * AFS_SEL[4:3]=00 → ±2g（静置平放 az≈+16384=1g）。 */
  st = MPU6050_WriteReg(MPU6050_REG_ACCEL_CONFIG, 0x00U);
  if (st != HAL_OK) { return st; }

  return HAL_OK;
}

/* -------------------------------------------------------------------------- */
HAL_StatusTypeDef MPU6050_SelfTestRegister(uint8_t *who_am_i)
{
  if (who_am_i == NULL)
  {
    return HAL_ERROR;
  }
  return MPU6050_ReadReg(MPU6050_REG_WHO_AM_I, who_am_i);
}

/* -------------------------------------------------------------------------- */
HAL_StatusTypeDef MPU6050_ReadAll(MPU6050_RawData *data)
{
  if (data == NULL)
  {
    return HAL_ERROR;
  }

  /* 单次突发读取 14 字节：从 ACCEL_XOUT_H(0x3B) 连续读出
   * axH,axL, ayH,ayL, azH,azL, tH,tL, gxH,gxL, gyH,gyL, gzH,gzL */
  uint8_t buf[14];
  HAL_StatusTypeDef st = HAL_I2C_Mem_Read(&hi2c1, MPU6050_ADDR,
                                          MPU6050_REG_ACCEL_XOUT_H,
                                          I2C_MEMADD_SIZE_8BIT,
                                          buf, sizeof(buf),
                                          MPU6050_I2C_TIMEOUT);
  if (st != HAL_OK)
  {
    return st;   /* 通信异常：data 不更新，由调用方判断 */
  }

  /* 仅做原始高低字节拼接，不做任何物理单位换算 */
  data->ax   = (int16_t)((uint16_t)buf[0]  << 8 | buf[1]);
  data->ay   = (int16_t)((uint16_t)buf[2]  << 8 | buf[3]);
  data->az   = (int16_t)((uint16_t)buf[4]  << 8 | buf[5]);
  data->temp = (int16_t)((uint16_t)buf[6]  << 8 | buf[7]);
  data->gx   = (int16_t)((uint16_t)buf[8]  << 8 | buf[9]);
  data->gy   = (int16_t)((uint16_t)buf[10] << 8 | buf[11]);
  data->gz   = (int16_t)((uint16_t)buf[12] << 8 | buf[13]);

  return HAL_OK;
}
