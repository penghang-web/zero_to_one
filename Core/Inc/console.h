/**
  ******************************************************************************
  * @file    console.h
  * @brief   USART1 控制台接口：printf 风格日志 + IDLE+DMA 收帧 + Shell
  ******************************************************************************
  */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>
#include <stddef.h>

/* 对外接口（实现见 console.c）
 * - Console_Tx        : 原始发送，阻塞（HAL_UART_Transmit）
 * - Console_Printf    : printf 风格日志（vsnprintf + Console_Tx），字符串请保持 ASCII
 * - Console_RxStart   : 启动 RX（HAL_UARTEx_ReceiveToIdle_DMA），main 初始化后调用一次
 * - Console_Process   : 主循环每圈调用，处理已收帧 + 5s 心跳日志
 */
void Console_Tx(const uint8_t *buf, uint16_t len);
void Console_Printf(const char *fmt, ...);
void Console_RxStart(void);
void Console_Process(void);

#endif /* CONSOLE_H */
