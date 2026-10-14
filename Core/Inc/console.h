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
#include "uart_driver.h"   /* UART_Driver_t：本模块用其接管 USART1 收发 */

/* 控制台使用的 UART 驱动实例（定义于 console.c，stm32f1xx_it.c 的 USART1 中断入口引用） */
extern UART_Driver_t g_uart1;

/* 对外接口（实现见 console.c）
 * - Console_Tx        : 非阻塞发送（压入 uart_driver 发送环形缓冲，由 TXE 中断发出）
 * - Console_Printf    : printf 风格日志（vsnprintf + Console_Tx），字符串请保持 ASCII
 * - Console_RxStart   : 启动 RX（初始化 uart_driver，接管 USART1 收发中断），main 初始化后调用一次
 * - Console_Process   : 主循环每圈调用，处理已收帧 + 5s 心跳日志
 */
void Console_Tx(const uint8_t *buf, uint16_t len);
void Console_Printf(const char *fmt, ...);
void Console_RxStart(void);
void Console_Process(void);

#endif /* CONSOLE_H */
