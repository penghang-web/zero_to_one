/**
  ******************************************************************************
  * @file    watchdog.h
  * @brief   独立看门狗(IWDG)喂狗模块：封装"节拍喂狗 + 使能开关 + 复位测试"，
  *          支撑《从0到1搭建指南》阶段4"死机自愈"。字符串保持 ASCII，中文走注释。
  ******************************************************************************
  */
#ifndef __WATCHDOG_H__
#define __WATCHDOG_H__

#include <stdint.h>

/* 喂狗：内部按使能开关执行 HAL_IWDG_Refresh，由 TIM2 1ms 节拍 ISR 周期调用。
 * 设计为 ISR 安全（仅写寄存器 + 改 volatile 标志），调用极短，不阻塞中断。 */
void WDG_Feed(void);

/* 测试/守卫用：开关喂狗。on=1 正常喂；on=0 停止喂（用于触发看门狗复位验证，
 * 或将来 RTOS 阶段做"任务心跳 AND"决定是否放狗）。 */
void WDG_SetFeed(uint8_t on);

/* 当前是否在喂狗（供状态查询）。 */
uint8_t WDG_IsFeeding(void);

/* 累计喂狗次数（供状态查询，观察喂狗节律是否正常）。 */
uint32_t WDG_FeedCount(void);

/* 复位测试：关闭喂狗并陷入死循环，看门狗将在超时后自动复位 MCU（默认约 1.5s）。
 * 仅用于验证 IWDG 生效，正常业务代码不可调用。 */
void WDG_TestHang(void);

#endif /* __WATCHDOG_H__ */
