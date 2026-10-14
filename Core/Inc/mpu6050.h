/**
  ******************************************************************************
  * @file    mpu6050.h
  * @brief   MPU6050 / 兼容器件（WHO_AM_I=0x74）I2C 驱动头文件
  * @note    基于 STM32 HAL 库，使用 I2C1 总线（hi2c1）。
  *          本款器件 WHO_AM_I 识别值为 0x74（非标准 MPU6050 的 0x68，
  *          由硬件/批次决定，以实测为准）。
  ******************************************************************************
  */
#ifndef MPU6050_H
#define MPU6050_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f1xx_hal.h"

/* -------------------------------------------------------------------------- */
/* 1. 基本配置（按需要修改）                                                   */
/* -------------------------------------------------------------------------- */

/* I2C 7 位从机地址：AD0 接地=0x68，AD0 接 VCC=0x69。HAL 接口需要 8 位地址，故左移1位 */
#define MPU6050_ADDR_7BIT   0x68U
#define MPU6050_ADDR        (MPU6050_ADDR_7BIT << 1)   /* HAL 用 8 位地址(已左移) */

/* 本款器件的 WHO_AM_I 期望值（自检比对用） */
#define MPU6050_WHO_AM_I_VAL   0x74U

/* I2C 读写统一超时时间(ms) */
#define MPU6050_I2C_TIMEOUT    100U

/* -------------------------------------------------------------------------- */
/* 2. 关键寄存器地址                                                          */
/* -------------------------------------------------------------------------- */
#define MPU6050_REG_SMPLRT_DIV   0x19U   /* 采样率分频 */
#define MPU6050_REG_CONFIG       0x1AU   /* 配置（含 DLPF 低通滤波） */
#define MPU6050_REG_GYRO_CONFIG  0x1BU   /* 陀螺仪量程配置 */
#define MPU6050_REG_ACCEL_CONFIG 0x1CU   /* 加速度计量程配置 */
#define MPU6050_REG_ACCEL_XOUT_H 0x3BU   /* 突发读取起始：加速度X高字节 */
#define MPU6050_REG_PWR_MGMT_1   0x6BU   /* 电源管理1（含 SLEEP 唤醒位） */
#define MPU6050_REG_WHO_AM_I     0x75U   /* 器件标识寄存器 */

/* PWR_MGMT_1 寄存器位定义 */
#define MPU6050_PWR1_SLEEP       (1U << 6)   /* SLEEP 位：1=休眠，0=唤醒 */

/* -------------------------------------------------------------------------- */
/* 3. 数据结构                                                                */
/* -------------------------------------------------------------------------- */

/**
  * @brief  传感器原始采样数据（仅高低字节拼接，未做物理单位换算）
  * @note   三轴加速度/陀螺仪、温度均为原始 16 位有符号值。
  *         az 静置平放时约等于 +16384（对应 ±2g 量程下 1g）。
  */
typedef struct
{
  int16_t ax;     /* 加速度 X 原始值 */
  int16_t ay;     /* 加速度 Y 原始值 */
  int16_t az;     /* 加速度 Z 原始值 */
  int16_t temp;   /* 温度原始值（未换算为 °C） */
  int16_t gx;     /* 陀螺仪 X 原始值 */
  int16_t gy;     /* 陀螺仪 Y 原始值 */
  int16_t gz;     /* 陀螺仪 Z 原始值 */
} MPU6050_RawData;

/* -------------------------------------------------------------------------- */
/* 4. 对外接口                                                                */
/* -------------------------------------------------------------------------- */

/**
  * @brief  初始化 MPU 器件：先唤醒，再配置量程与 DLPF
  * @retval HAL_OK       初始化成功（所有寄存器写入均成功）
  *         ！=HAL_OK    某一寄存器 I2C 写入失败，返回具体 HAL 状态
  * @note   必须按照"先唤醒(PWR_MGMT_1) → 再配置"的顺序；若未唤醒，
  *         后续读取会全部返回 0xFF。
  */
HAL_StatusTypeDef MPU6050_Init(void);

/**
  * @brief  读取 WHO_AM_I 寄存器（自检调用）
  * @param  who_am_i  [out] 实际读到的器件标识值
  * @retval HAL_OK       读取成功
  *         ！=HAL_OK    I2C 读取失败
  * @note   预期值应为 MPU6050_WHO_AM_I_VAL(0x74)。比对是否相等由调用方完成。
  */
HAL_StatusTypeDef MPU6050_SelfTestRegister(uint8_t *who_am_i);

/**
  * @brief  一次性突发读取 14 字节原始采样数据
  * @param  data  [out] 解析后的原始数据结构
  * @retval HAL_OK       读取成功
  *         ！=HAL_OK    I2C 读取失败（未修改 data，或仅部分写入）
  * @note   约束：必须从 ACCEL_XOUT_H(0x3B) 起，单次突发读 14 字节，
  *         禁止分 14 次单字节读取。顺序为：
  *         axH,axL, ayH,ayL, azH,azL, tH,tL, gxH,gxL, gyH,gyL, gzH,gzL
  */
HAL_StatusTypeDef MPU6050_ReadAll(MPU6050_RawData *data);

/* -------------------------------------------------------------------------- */
/* 5. 采样状态（供 UI / 控制台使用，由 MPU6050_Process 维护）                   */
/* -------------------------------------------------------------------------- */
extern MPU6050_RawData g_mpu;     /* 最新一次原始采样（主循环持续更新） */
extern uint8_t         g_mpu_ok;  /* 1=已唤醒且 WHO_AM_I 匹配；0=未连/失连（Process 会重试） */

/**
  * @brief  初始化期检测：唤醒器件并比对 WHO_AM_I，成功置 g_mpu_ok=1 并打印状态。
  *         失败则保持 g_mpu_ok=0，由 MPU6050_Process 周期重试（支持未接/热插拔自愈）。
  */
void MPU6050_Begin(void);

/**
  * @brief  主循环每圈调用：g_mpu_ok 时每 100ms 突发读 14 字节更新 g_mpu；
  *         读取失败标记失连，转入每 5s 重试初始化。
  */
void MPU6050_Process(void);

#ifdef __cplusplus
}
#endif

#endif /* MPU6050_H */
