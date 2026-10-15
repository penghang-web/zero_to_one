/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* main.c 中定义的系统侧全局量（USER CODE PV 段，非 static，可 extern 引用） */
extern volatile uint32_t g_systick_ms;    /* TIM2 1ms 节拍计数器 */
extern volatile uint32_t g_main_feed_ms;  /* 系统"活体"时间戳：IWDG 喂狗门控用 */
#include "console.h"   /* Console_Process：Phase 2 串口任务周期性调用的命令行处理 */
#include "ssd1306.h"   /* SSD1306_UI_Process：Phase 3 OLED 显示任务 */
#include "mpu6050.h"   /* MPU6050_Process：周期性采样 g_mpu，供 OLED 三轴屏显示 */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
osThreadId consoleTaskHandle;   /* Phase 2：Console 串口任务句柄 */
osThreadId oledTaskHandle;      /* Phase 3：OLED 显示任务句柄 */
osMutexId  i2c_mutexHandle;     /* I2C 总线互斥锁：串行化 OLED/MPU6050 共享总线访问 */
/* USER CODE END Variables */
osThreadId defaultTaskHandle;

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
void StartConsoleTask(void const * argument);   /* Phase 2：Console 串口任务入口 */
void StartOledTask(void const * argument);       /* Phase 3：OLED 显示任务入口 */
/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void const * argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* GetIdleTaskMemory prototype (linked to static allocation support) */
void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize );

/* USER CODE BEGIN GET_IDLE_TASK_MEMORY */
static StaticTask_t xIdleTaskTCBBuffer;
static StackType_t xIdleStack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize )
{
  *ppxIdleTaskTCBBuffer = &xIdleTaskTCBBuffer;
  *ppxIdleTaskStackBuffer = &xIdleStack[0];
  *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
  /* place for user code */
}
/* USER CODE END GET_IDLE_TASK_MEMORY */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* I2C 总线互斥锁：OLED 任务与 MPU6050 任务（Phase 4）共享 PB6/PB7 总线，
   * 用它串行化访问，避免两任务交错传输互相打断导致花屏。
   * 内核启动前 i2c_mutexHandle=NULL，i2c.c 据此跳过加锁（兼容启动前初始化）。 */
  osMutexDef(i2cMutex);
  i2c_mutexHandle = osMutexCreate(osMutex(i2cMutex));
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* definition and creation of defaultTask */
  osThreadDef(defaultTask, StartDefaultTask, osPriorityNormal, 0, 128);
  defaultTaskHandle = osThreadCreate(osThread(defaultTask), NULL);

  /* USER CODE BEGIN RTOS_THREADS */
  /* Phase 2：Console 串口任务 —— 周期性调用 Console_Process 处理 RX 环形缓冲里的命令行。
   * ISR(DMA+IDLE) 仅做硬件层收数，不调任何 RTOS API（零耦合、零 HardFault 风险）；
   * 本任务以轮询方式（约 100Hz）取字节，平移"中断收数、主循环处理"为"中断收数、任务处理"。 */
  osThreadDef(consoleTask, StartConsoleTask, osPriorityNormal, 0, 384);
  consoleTaskHandle = osThreadCreate(osThread(consoleTask), NULL);

  /* Phase 3：OLED 显示任务 —— 周期性调用 SSD1306_UI_Process 重绘状态屏/MPU 屏。
   * 屏内刷新（SSD1306_Refresh）经 I2C1_SafeMemWrite 走软件 I2C，互斥锁保证
   * 与后续 MPU6050 任务不抢总线。栈 384 words 与 Console 任务同级。 */
  osThreadDef(oledTask, StartOledTask, osPriorityNormal, 0, 384);
  oledTaskHandle = osThreadCreate(osThread(oledTask), NULL);
  /* USER CODE END RTOS_THREADS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void const * argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  /* Phase 1 心跳任务：唯一目的是证明"FreeRTOS 内核已在调度"。
   * - PC13(LED) 每 500ms 翻转一次：肉眼可见的调度证据。
   * - 每圈刷新 g_main_feed_ms：TIM2 ISR 里的喂狗门控据此放行
   *   （任务活着 -> 喂狗；任务卡死 -> 停止喂狗 -> IWDG 1.5s 超时复位）。 */
  for(;;)
  {
    HAL_GPIO_TogglePin(LED_PC13_GPIO_Port, LED_PC13_Pin);
    g_main_feed_ms = g_systick_ms;
    osDelay(500);
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* Phase 2：Console 串口任务
 * 周期性调用 Console_Process()：读 UART RX 环形缓冲、解析命令行、回显、5s 心跳日志、
 * mpu 持续打印流。RX 已在 main.c(USER CODE 2) 经 Console_RxStart() 武装(IDLE+DMA)，
 * 此处不再重复初始化。
 * osDelay(10) 让出 CPU（约 100Hz 轮询），交互延迟 <10ms 足够；期间其它任务/ISR 正常跑。
 * ⚠ 设计说明：刻意不采用"IDLE ISR 内 xTaskNotifyFromISR 唤醒任务"方案——
 * 那样 ISR 需调 RTOS API，须满足 configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 约束
 * 且易踩 HardFault；轮询式只在任务上下文运行，最简最稳，契合本项目既有的ISR极简哲学。 */
void StartConsoleTask(void const * argument)
{
  (void)argument;
  for (;;)
  {
    Console_Process();
    osDelay(10);
  }
}
/* Phase 3：OLED 显示任务
 * 周期性调用 SSD1306_UI_Process()：其内部按自身节拍（状态屏 1s / MPU 屏 500ms /
 * 两屏每 4s 切换）重绘并刷屏。刷屏走软件 I2C（i2c.c 互斥锁串行化）。
 * 同时在本循环里调用 MPU6050_Process() 周期性采样 g_mpu（每 100ms 读 14 字节），
 * 供 OLED 三轴屏显示——Phase 4 原计划的独立 MPU 任务因 configTOTAL_HEAP_SIZE=4096
 * 太小、加任务易挤爆堆，故把采样并入 OLED 任务（两者都走 i2c_mutex 串行化，
 * 同一任务内顺序执行不抢总线，最省资源）。
 * osDelay(10) 让出 CPU（约 100Hz 轮询），交互延迟 <10ms 足够。 */
void StartOledTask(void const * argument)
{
  (void)argument;
  for (;;)
  {
    MPU6050_Process();    /* 采样 g_mpu（缺省每 100ms 一次），先于显示使其最新 */
    SSD1306_UI_Process();
    osDelay(10);
  }
}
/* USER CODE END Application */

