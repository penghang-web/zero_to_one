/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "main.h"
#include "cmsis_os.h"
#include "dma.h"
#include "iwdg.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "key.h"
#include "console.h"
#include "watchdog.h"   /* 阶段4：WDG_Feed 由 TIM2 节拍 ISR 调用 */
#include "ssd1306.h"   /* 阶段5：OLED 驱动 + UI 层（SSD1306_UI_*） */
#include "mpu6050.h"   /* 阶段6：MPU6050 I2C 驱动（Init/SelfTest/ReadAll/Begin/Process） */
#include "soft_i2c.h"  /* 阶段3：软件 I2C 驱动（替代硬件 I2C，消除 BUSY 锁死） */
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

/* USER CODE BEGIN PV */
/* 按键相关状态已封装到 key.c（key.h 提供 Key_Init / Key_Process / Key_EXTI_Callback 接口） */
volatile uint32_t g_systick_ms = 0;   /* TIM2 1ms 节拍计数器，由 HAL_TIM_PeriodElapsedCallback 累加 */
volatile uint32_t g_main_feed_ms = 0;   /* 主循环活体时间戳：每圈正常跑完即刷新；喂狗门控据此判断是否卡死 */
/* 注：g_mpu / g_mpu_ok 已下沉到 mpu6050.c（见 mpu6050.h 的 extern 声明），供 UI/控制台使用 */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */
/* 蜂鸣器驱动已独立成 buzzzer.c（Buzzer_On/Buzzer_Off/Buzzer_Beep/Buzzer_Process，
 * TIM3_CH4 PWM），由 key.c 与 console.c 通过 buzzer.h 调用 */
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_TIM2_Init();
  MX_TIM3_Init();
  MX_IWDG_Init();
  MX_SPI1_Init();
  /* USER CODE BEGIN 2 */
  Key_Init();                          /* 初始化按键模块（状态机/报警标志清零） */
  Console_RxStart();                   /* 启动 USART1 RX（IDLE+DMA 收帧） */
  SOFT_I2C_Init();                     /* 阶段3：配置 PB6/SCL、PB7/SDA 为开漏输出并释放总线（软件 I2C） */
  SSD1306_UI_Init();                   /* 初始化 SSD1306 OLED（英文/ASCII 显示） */
  Console_Printf("\r\n==== MiniBlackBox USART1 Shell ====\r\n");
  Console_Printf("baud 115200 8N1, RX=IDLE+DMA. type 'help'.\r\n");

  /* 阶段6：MPU6050 初始化检测（唤醒+识别），失败每 5s 重试由 MPU6050_Process 处理 */
  MPU6050_Begin();

  /* 阶段3：启动系统节拍与蜂鸣器 PWM（PB1=TIM3_CH4） */
  HAL_TIM_Base_Start_IT(&htim2);                    /* TIM2 1ms 节拍中断 -> 累加 g_systick_ms */
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_4);         /* 蜂鸣器 PWM 运行，占空比初始 0 = 静音 */
  g_main_feed_ms = g_systick_ms;   /* 启动时先打一次活体标记，避免上电初期 TIM2 节拍未起时误判卡死 */

  /* 阶段4：调试期冻结 IWDG——ST-LINK 暂停在断点时看门狗不计数，
   * 避免"断点被狗咬"误复位。仅调试器连接时生效，正常运行照常喂狗自愈。 */
  __HAL_DBGMCU_FREEZE_IWDG();
  /* USER CODE END 2 */

  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* ==== Phase 1（FreeRTOS 迁移）：本块已不可达 ====
     * osKernelStart() 之后控制权交给调度器，永不回到这个 while(1)。
     * 原先四个 Process 调用已下线，功能按分期迁到各自任务：
     *   Key_Process()        -> Phase 4 按键任务
     *   Console_Process()    -> Phase 2 串口任务
     *   SSD1306_UI_Process() -> Phase 3 显示任务
     *   MPU6050_Process()    -> 并入 Phase 3 OLED 任务循环（FreeRTOS 堆仅 4096B，
     *                          不另开 MPU 任务以免堆溢出；两者同走 i2c_mutex 串行化）
     * 活体喂狗（g_main_feed_ms）改由默认任务里的心跳每 500ms 刷新。 */
  }                                        /* while(1) 闭合 */
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
/* PB5/EXTI5 按键中断回调：ISR 保持极简——只把边沿交给按键模块，消抖与状态处理放到主循环做（契合"中断收数、主循环处理"原则） */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  Key_EXTI_Callback(GPIO_Pin);   /* 路由到 key.c 的按键模块 */
}

/* 说明（FreeRTOS 迁移）：TIM2 节拍与 IWDG 喂狗逻辑已合并进本文件末尾
 * CubeMX 生成的时间基准回调 HAL_TIM_PeriodElapsedCallback() 的
 * "USER CODE BEGIN Callback 0" 受保护块内。
 * 原因：启用 FreeRTOS 后 HAL 时基源改为 TIM4，CubeMX 会在 main.c 生成它自己的
 * HAL_TIM_PeriodElapsedCallback；若此处再定义一份同名强函数，同一编译单元重复定义，
 * 编译报 "redefinition of 'HAL_TIM_PeriodElapsedCallback'"。
 * ⚠ 若将来把时基源改回 SysTick，CubeMX 会连同该回调一起删掉，届时需把 TIM2 逻辑
 *   从 Callback 0 块搬回本处。 */
/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM4 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */
  /* TIM2 1ms 节拍中断：仅累加系统节拍，长逻辑留给任务/主循环
   * （契合"中断收数、主循环处理"原则）。
   * 说明：启用 FreeRTOS 后 HAL 时基源由 SysTick 改为 TIM4，CubeMX 会在本回调内
   * 处理 TIM4(HAL_IncTick)；用户侧的中断逻辑因此必须写在这个受保护的 USER CODE 块里，
   * 不能再单独定义一个同名函数（同一编译单元重复定义会编译报
   * "redefinition of 'HAL_TIM_PeriodElapsedCallback'"，详见文件末尾 USER CODE 4 的说明）。 */
  if (htim->Instance == TIM2)
  {
    g_systick_ms++;
    /* 阶段4：IWDG 喂狗——每 500ms 喂一次（远小于 1.5s 超时）。
     * 关键：用"活体门控"——只有当 g_main_feed_ms（由默认任务心跳每 500ms 刷新）
     * 近期（<1s）仍被正常刷新才喂狗；否则认为系统卡死，停止喂狗，
     * 让 IWDG 在 1.5s 超时后自动复位 MCU，实现"死机自愈"。
     * 此前把 WDG_Feed 无条件放中断里会导致主循环卡死时中断照常喂狗、看门狗彻底失效。 */
    static uint32_t last_feed_ms = 0;
    if (g_systick_ms - last_feed_ms >= 500U)
    {
      last_feed_ms = g_systick_ms;
      if (g_systick_ms - g_main_feed_ms < 1000U)
      {
        WDG_Feed();
      }
    }
  }
  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM4)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
