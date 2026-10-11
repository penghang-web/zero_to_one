/**
  ******************************************************************************
  * @file    console.c
  * @brief   USART1 控制台：printf 日志 + IDLE+DMA 收帧 + 简易 Shell（方案A）
  ******************************************************************************
  *
  * 接收：HAL_UARTEx_ReceiveToIdle_DMA 把数据搬进 s_rx_buf，空闲(IDLE)中断触发
  *       HAL_UARTEx_RxEventCallback 只记录长度并置"帧就绪"标志（ISR 极简）。
  * 发送：方案A 用阻塞 HAL_UART_Transmit；printf 用 vsnprintf 格式化后发送。
  * 处理：Console_Process 在主循环里取帧、回显(echo)、解析命令，最后重新启动 RX。
  */
#include "console.h"
#include "usart.h"   /* huart1 (extern) */
#include "key.h"     /* Key_SetAlarm / Key_GetAlarmState */
#include "stm32f1xx_hal.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define RX_BUF_SIZE  64
#define TX_BUF_SIZE  128
#define HEARTBEAT_MS 5000

/* ---- 模块私有 ---- */
static uint8_t  s_rx_buf[RX_BUF_SIZE];
static volatile uint8_t  s_rx_ready = 0;   /* 帧就绪标志：ISR 置位，主循环消费 */
static volatile uint16_t s_rx_len   = 0;   /* 本帧字节数 */
static uint32_t s_heartbeat_ms = 0;

/* 阻塞发送：方案A 简单直接，发长串时会短暂占用主循环（115200 下几毫秒） */
void Console_Tx(const uint8_t *buf, uint16_t len)
{
  if (len == 0 || buf == NULL)
  {
    return;
  }
  HAL_UART_Transmit(&huart1, (uint8_t *)buf, len, HAL_MAX_DELAY);
}

/* printf 风格日志：格式化进局部缓冲再发送（字符串保持 ASCII，中文走注释） */
void Console_Printf(const char *fmt, ...)
{
  static char tbuf[TX_BUF_SIZE];
  va_list ap;

  va_start(ap, fmt);
  int n = vsnprintf(tbuf, sizeof(tbuf), fmt, ap);
  va_end(ap);

  if (n > 0)
  {
    Console_Tx((uint8_t *)tbuf, (uint16_t)n);
  }
}

/* RX 帧就绪回调（HAL 弱函数，这里覆盖）：ISR 内只记账，不处理 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
  if (huart->Instance == USART1)
  {
    s_rx_len   = Size;
    s_rx_ready = 1;
  }
}

/* 启动/重新启动 RX：必须在 USART 初始化后调用；每次收完一帧都要再调用一次 */
void Console_RxStart(void)
{
  s_rx_ready = 0;
  s_rx_len   = 0;
  if (HAL_UARTEx_ReceiveToIdle_DMA(&huart1, s_rx_buf, RX_BUF_SIZE) != HAL_OK)
  {
    /* 启动失败暂不处理，留待后续错误恢复专题 */
  }
}

/* 简易命令解析 + 回显（echo）。命令字全小写 ASCII。 */
static void Shell_Exec(char *line)
{
  /* 去掉结尾的 \r / \n，方便解析与回显 */
  size_t len = strlen(line);
  while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n'))
  {
    line[--len] = '\0';
  }

  /* 回显收到的行（验证 TX/RX 双通） */
  Console_Printf("%s\r\n", line);

  if (len == 0)
  {
    return;
  }

  if (strcmp(line, "help") == 0)
  {
    Console_Printf("Commands:\r\n help\r\n info\r\n led on\r\n led off\r\n");
  }
  else if (strcmp(line, "info") == 0)
  {
    Console_Printf("uptime=%lu s, alarm=%u\r\n",
                   (unsigned long)(HAL_GetTick() / 1000u),
                   (unsigned)Key_GetAlarmState());
  }
  else if (strcmp(line, "led on") == 0)
  {
    Key_SetAlarm(1);
    Console_Printf("led on\r\n");
  }
  else if (strcmp(line, "led off") == 0)
  {
    Key_SetAlarm(0);
    Console_Printf("led off\r\n");
  }
  else
  {
    Console_Printf("unknown: %s (try 'help')\r\n", line);
  }
}

/* 主循环调用：心跳日志 + 处理已收帧并重新启动 RX */
void Console_Process(void)
{
  /* 5s 心跳：证明系统存活、日志通道通畅 */
  uint32_t now = HAL_GetTick();
  if (now - s_heartbeat_ms >= HEARTBEAT_MS)
  {
    s_heartbeat_ms = now;
    Console_Printf("[hb] uptime=%lu s\r\n", (unsigned long)(now / 1000u));
  }

  if (!s_rx_ready)
  {
    return;
  }

  /* 关中断保护，快速拷出帧数据并清标志（避免 ISR 在拷贝途中改写缓冲） */
  uint16_t len = s_rx_len;
  char line[RX_BUF_SIZE + 1];
  __disable_irq();
  memcpy(line, s_rx_buf, len);
  s_rx_ready = 0;
  __enable_irq();
  line[len] = '\0';

  Shell_Exec(line);

  /* 重新武装 RX，准备收下一帧（Normal 模式 DMA 每次需重启） */
  HAL_UARTEx_ReceiveToIdle_DMA(&huart1, s_rx_buf, RX_BUF_SIZE);
}
