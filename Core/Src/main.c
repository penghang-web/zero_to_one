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
#include "dma.h"
#include "i2c.h"
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
#include "w25qxx.h"    /* 阶段5：W25Q/GD25 SPI Flash（字库存储），W25QXX_Init/JEDEC */
#include "ui.h"        /* 阶段5：OLED UI 层（SSD1306 演示屏） */
#include "mpu6050.h"   /* 阶段6：MPU6050 I2C 驱动（Init/SelfTest/ReadAll） */
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

/* 阶段6：MPU6050 原始数据（主循环持续采样，控制台 mpu 命令打印）
 * g_mpu        —— 最新一次原始采样（供 mpu 命令打印，后续也可供 UI 显示）
 * g_mpu_ok     —— 1=已唤醒且 WHO_AM_I 匹配，主循环才会周期读；否则转入重试 */
MPU6050_RawData g_mpu;
uint8_t g_mpu_ok = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
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
  MX_I2C1_Init();
  MX_SPI1_Init();
  /* USER CODE BEGIN 2 */
  Key_Init();                          /* 初始化按键模块（状态机/报警标志清零） */
  Console_RxStart();                   /* 启动 USART1 RX（IDLE+DMA 收帧） */
  W25QXX_Init();                       /* 阶段5：读 W25Q/GD25 JEDEC 并缓存容量 */
  UI_Init();                           /* 阶段5：初始化 SSD1306 OLED */
  Console_Printf("\r\n==== MiniBlackBox USART1 Shell ====\r\n");
  Console_Printf("baud 115200 8N1, RX=IDLE+DMA. type 'help'.\r\n");
  {
    uint8_t jid[3];
    W25QXX_JEDEC(jid);
    Console_Printf("W25Q JEDEC %02X%02X%02X cap=%lu B font=%s\r\n",
                   jid[0], jid[1], jid[2],
                   (unsigned long)W25QXX_CapacityBytes(),
                   W25QXX_FontReady() ? "READY" : "none");
    Console_Printf("type 'oled' to draw demo, 'flashcn <size>' to burn font.\r\n");
  }

  /* 阶段6：MPU6050 初始化（先唤醒 PWR_MGMT_1，再配置量程/DLPF）。
   * 失败不阻塞主流程——置 g_mpu_ok=0，由主循环每 5s 重试（支持未接/热插拔自愈）。 */
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

  /* 阶段3：启动系统节拍与蜂鸣器 PWM（PB1=TIM3_CH4） */
  HAL_TIM_Base_Start_IT(&htim2);                    /* TIM2 1ms 节拍中断 -> 累加 g_systick_ms */
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_4);         /* 蜂鸣器 PWM 运行，占空比初始 0 = 静音 */

  /* 阶段4：调试期冻结 IWDG——ST-LINK 暂停在断点时看门狗不计数，
   * 避免"断点被狗咬"误复位。仅调试器连接时生效，正常运行照常喂狗自愈。 */
  __HAL_DBGMCU_FREEZE_IWDG();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* ---- 按键逻辑已封装到 key.c：主循环只负责"喂"状态机 ---- */
    Key_Process();
    /* ---- USART 控制台：处理串口帧 + 心跳日志（每圈调用） ---- */
    Console_Process();

    /* ---- OLED 显示：状态屏(UI_Demo) 与 MPU 三轴屏(UI_MPU) 每 4s 交替 ---- */
    {
      static uint32_t s_oled_ms = 0;    /* 屏幕切换节拍 */
      static uint32_t s_oled_draw = 0;  /* 当前屏重绘节拍 */
      static uint8_t  s_oled_mode = 0;  /* 0=状态屏, 1=MPU屏 */
      if (g_systick_ms - s_oled_ms >= 4000U)
      {
        s_oled_ms = g_systick_ms;
        s_oled_mode ^= 1U;
      }
      /* MPU 屏数据变化快，每 500ms 重绘；状态屏每 1s 重绘即可 */
      uint32_t period = (s_oled_mode == 1U) ? 500U : 1000U;
      if (g_systick_ms - s_oled_draw >= period)
      {
        s_oled_draw = g_systick_ms;
        if (s_oled_mode == 1U)
          UI_MPU();
        else
          UI_Demo();
      }
    }

    /* ---- 阶段6：MPU6050 持续采样（与 OLED 共用 I2C1 总线） ----
     * g_mpu_ok 时每 100ms 突发读 14 字节更新 g_mpu；读取失败则标记失连转重试。
     * 未连接时每 5s 重试一次初始化（HAL_I2C 超时 100ms，避免每圈阻塞主循环）。 */
    {
      static uint32_t s_mpu_ms = 0;
      static uint32_t s_mpu_retry_ms = 0;
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

/* TIM2 1ms 节拍中断回调：仅累加系统节拍，长逻辑留主循环（契合"中断收数、主循环处理"原则） */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM2)
  {
    g_systick_ms++;
    /* 阶段4：IWDG 喂狗——每 500ms 喂一次（远小于 1.5s 超时），防误触发。
     * 放在节拍 ISR 里契合指南"在节拍中断里定期喂狗"；若节拍卡死则狗会复位，正是自愈目的。 */
    static uint32_t last_feed_ms = 0;
    if (g_systick_ms - last_feed_ms >= 500U)
    {
      last_feed_ms = g_systick_ms;
      WDG_Feed();
    }
  }
}
/* USER CODE END 4 */

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
