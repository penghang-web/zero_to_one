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
#include "console.h" /* Console_Printf：初始化/重连日志 */

/* 最新一次原始采样与连接状态（原在 main.c，下沉到本模块由 MPU6050_Process 维护） */
MPU6050_RawData g_mpu;
uint8_t         g_mpu_ok = 0;
extern volatile uint32_t g_systick_ms;   /* TIM2 1ms 节拍，main.c 维护 */

/* -------------------------------------------------------------------------- */
/* 内部辅助：单寄存器写                                                        */
/* -------------------------------------------------------------------------- */
static HAL_StatusTypeDef MPU6050_WriteReg(uint8_t reg, uint8_t val)
{
  return I2C1_SafeMemWrite(MPU6050_ADDR, reg,
                            I2C_MEMADD_SIZE_8BIT, &val, 1U);
}

/* -------------------------------------------------------------------------- */
/* 内部辅助：单寄存器读                                                        */
/* -------------------------------------------------------------------------- */
static HAL_StatusTypeDef MPU6050_ReadReg(uint8_t reg, uint8_t *val)
{
  return I2C1_SafeMemRead(MPU6050_ADDR, reg,
                           I2C_MEMADD_SIZE_8BIT, val, 1U);
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
  HAL_StatusTypeDef st = I2C1_SafeMemRead(MPU6050_ADDR,
                                          MPU6050_REG_ACCEL_XOUT_H,
                                          I2C_MEMADD_SIZE_8BIT,
                                          buf, sizeof(buf));
  if (st != HAL_OK)
  {
    return st;   /* 通信异常（已尝试总线恢复）：data 不更新，由调用方判断 */
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

/* -------------------------------------------------------------------------- */
/* 初始化期检测 + 主循环采样调度（原 main.c 业务节拍逻辑，下沉到本模块）      */
/* -------------------------------------------------------------------------- */

/**
  * @brief  初始化期检测：唤醒器件并比对 WHO_AM_I。
  *        成功置 g_mpu_ok=1 并打印 "MPU6050 ok"；失败保持 0 并打印提示，
  *        后续由 MPU6050_Process 每 5s 重试（未接/热插拔自愈）。
  */
void MPU6050_Begin(void)
{
  uint8_t who = 0;
  if (MPU6050_Init() == HAL_OK &&
      MPU6050_SelfTestRegister(&who) == HAL_OK &&
      who == MPU6050_WHO_AM_I_VAL)
  {
    g_mpu_ok = 1;
    Console_Printf("MPU6050 ok who=0x%02X (exp 0x%02X)\r\n", who, MPU6050_WHO_AM_I_VAL);
  }
  else
  {
    g_mpu_ok = 0;
    Console_Printf("MPU6050 not found (who=0x%02X), will retry every 5s\r\n", who);
  }
}

/**
  * @brief  主循环每圈调用。
  *        - g_mpu_ok 时每 100ms 突发读 14 字节更新 g_mpu；读取失败标记失连。
  *        - 失连后每 5s 重试一次初始化（HAL_I2C 超时 100ms，不阻塞主循环）。
  */
void MPU6050_Process(void)
{
  static uint32_t s_mpu_ms = 0;       /* 采样节拍 */
  static uint32_t s_mpu_retry_ms = 0; /* 重试节拍 */

  if (g_mpu_ok)
  {
    if (g_systick_ms - s_mpu_ms >= 100U)
    {
      s_mpu_ms = g_systick_ms;
      if (MPU6050_ReadAll(&g_mpu) != HAL_OK)
      {
        g_mpu_ok = 0;   /* 读取失败：标记失连，下一轮转入重试 */
      }
    }
  }
  else if (g_systick_ms - s_mpu_retry_ms >= 5000U)
  {
    s_mpu_retry_ms = g_systick_ms;
    uint8_t who = 0;
    if (MPU6050_Init() == HAL_OK &&
        MPU6050_SelfTestRegister(&who) == HAL_OK &&
        who == MPU6050_WHO_AM_I_VAL)
    {
      g_mpu_ok = 1;
      Console_Printf("MPU6050 reconnected who=0x%02X\r\n", who);
    }
  }
}
