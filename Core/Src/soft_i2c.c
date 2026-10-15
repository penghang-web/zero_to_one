/**
  ******************************************************************************
  * @file    soft_i2c.c
  * @brief   软件 I2C（bit-bang）驱动实现
  *
  * 引脚：PB6 = SCL，PB7 = SDA，开漏输出（GPIO_MODE_OUTPUT_OD）+ 内部上拉。
  * 无外部上拉电阻时靠 STM32 内部 ~40kΩ 上拉拉高总线（100kHz 单从机足够）；
  * 若 OLED 模块本身带 4.7kΩ 板上上拉则二者并联更稳，无副作用。
  * 时序目标 ~100kHz（I2C 标准模式）。软件 I2C 对"偏慢"不敏感（单主、SCL 可任意慢），
  * 故用简单空转延时；若实测波形过快导致从机不认，调大 SOFT_I2C_DELAY_LOOP 即可。
  *
  * 关键技巧：开漏输出下"写 1"= 释放总线（上拉拉高），"写 0"= 主动拉低；
  * 读 SDA 时直接读 IDR，得到的是总线实际电平（线与关系），无需切输入模式。
  ******************************************************************************
  */
#include "soft_i2c.h"

/* ============================ 引脚与时序 ============================ */
#define SOFT_I2C_PORT   GPIOB
#define SOFT_I2C_SCL    GPIO_PIN_6
#define SOFT_I2C_SDA    GPIO_PIN_7

/* 半周期延时步数：72MHz 下几十次空转约 1~2us；配合下方每拍约 2~3 次调用，
 * 得到 SCL 周期 ~10us（≈100kHz）。用示波器/逻辑分析仪核对后再微调。 */
#define SOFT_I2C_DELAY_LOOP   40

static inline void i2c_dly(void)
{
  for (volatile uint32_t i = 0; i < SOFT_I2C_DELAY_LOOP; i++);
}

static inline void sda_set(int hi)
{
  HAL_GPIO_WritePin(SOFT_I2C_PORT, SOFT_I2C_SDA, hi ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
static inline void scl_set(int hi)
{
  HAL_GPIO_WritePin(SOFT_I2C_PORT, SOFT_I2C_SCL, hi ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
/* 读 SDA 实际电平（开漏下 IDR 即总线电平） */
static inline int sda_get(void)
{
  return HAL_GPIO_ReadPin(SOFT_I2C_PORT, SOFT_I2C_SDA) == GPIO_PIN_SET;
}

void SOFT_I2C_Init(void)
{
  GPIO_InitTypeDef g = {0};
  g.Pin  = SOFT_I2C_SCL | SOFT_I2C_SDA;
  g.Mode = GPIO_MODE_OUTPUT_OD;   /* 开漏：写1释放(上拉拉高)，写0驱动低 */
  g.Pull = GPIO_PULLUP;           /* 无外部上拉时靠内部 ~40kΩ 拉高总线 */
  g.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(SOFT_I2C_PORT, &g);
  sda_set(1);                     /* 释放 SDA */
  scl_set(1);                     /* 释放 SCL，总线空闲态 */
}

/* ---- 起始 / 停止条件 ---- */
static void i2c_start(void)
{
  sda_set(1); scl_set(1); i2c_dly();
  sda_set(0); i2c_dly();          /* SCL=1 期间 SDA 1->0 = START */
  scl_set(0);
}
static void i2c_stop(void)
{
  sda_set(0); i2c_dly();
  scl_set(1); i2c_dly();
  sda_set(1); i2c_dly();          /* SCL=1 期间 SDA 0->1 = STOP */
}

/* 写 1 字节，返回从机应答：0=ACK，1=NACK（从机不应答） */
static int i2c_wr_byte(uint8_t b)
{
  for (int i = 7; i >= 0; i--)
  {
    sda_set(b & (1u << i));
    i2c_dly();
    scl_set(1); i2c_dly(); i2c_dly();
    scl_set(0);
  }
  sda_set(1); i2c_dly();          /* 释放 SDA 等待从机 ACK */
  scl_set(1); i2c_dly(); i2c_dly();
  int ack = sda_get() ? 1 : 0;    /* 从机拉低 SDA = ACK(0) */
  scl_set(0);
  return ack;
}

/* 读 1 字节；ack=1 主机回 ACK（继续收），ack=0 主机回 NACK（末字节） */
static uint8_t i2c_rd_byte(int ack)
{
  uint8_t b = 0;
  sda_set(1);                     /* 释放 SDA，准备接收 */
  for (int i = 7; i >= 0; i--)
  {
    i2c_dly();
    scl_set(1); i2c_dly(); i2c_dly();
    b = (uint8_t)((b << 1) | (sda_get() ? 1 : 0));
    scl_set(0);
  }
  sda_set(ack ? 0 : 1);           /* 第 9 拍主机回 ACK / NACK */
  i2c_dly();
  scl_set(1); i2c_dly(); i2c_dly();
  scl_set(0);
  return b;
}

/* ============================ 对外 Mem 接口 ============================ */
HAL_StatusTypeDef soft_i2c_mem_write(uint8_t DevAddress, uint16_t MemAddress,
                                     uint16_t MemAddSize, const uint8_t *pData, uint16_t Size)
{
  if (pData == NULL && Size > 0U)
  {
    return HAL_ERROR;
  }
  i2c_start();
  if (i2c_wr_byte((uint8_t)(DevAddress & 0xFEU))) { i2c_stop(); return HAL_ERROR; }  /* SLA+W */
  if (MemAddSize == I2C_MEMADD_SIZE_16BIT)
  {
    if (i2c_wr_byte((uint8_t)(MemAddress >> 8))) { i2c_stop(); return HAL_ERROR; }
  }
  if (i2c_wr_byte((uint8_t)MemAddress)) { i2c_stop(); return HAL_ERROR; }
  for (uint16_t i = 0; i < Size; i++)
  {
    if (i2c_wr_byte(pData[i])) { i2c_stop(); return HAL_ERROR; }
  }
  i2c_stop();
  return HAL_OK;
}

HAL_StatusTypeDef soft_i2c_mem_read(uint8_t DevAddress, uint16_t MemAddress,
                                    uint16_t MemAddSize, uint8_t *pData, uint16_t Size)
{
  if (pData == NULL && Size > 0U)
  {
    return HAL_ERROR;
  }
  i2c_start();
  if (i2c_wr_byte((uint8_t)(DevAddress & 0xFEU))) { i2c_stop(); return HAL_ERROR; }  /* SLA+W */
  if (MemAddSize == I2C_MEMADD_SIZE_16BIT)
  {
    if (i2c_wr_byte((uint8_t)(MemAddress >> 8))) { i2c_stop(); return HAL_ERROR; }
  }
  if (i2c_wr_byte((uint8_t)MemAddress)) { i2c_stop(); return HAL_ERROR; }
  i2c_start();                                                                    /* repeated START */
  if (i2c_wr_byte((uint8_t)(DevAddress | 0x01U))) { i2c_stop(); return HAL_ERROR; } /* SLA+R */
  for (uint16_t i = 0; i < Size; i++)
  {
    int last = (i == (uint16_t)(Size - 1U));
    pData[i] = i2c_rd_byte(!last);    /* 末字节回 NACK，其余回 ACK */
  }
  i2c_stop();
  return HAL_OK;
}
