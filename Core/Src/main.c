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
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

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
volatile uint8_t g_key_irq = 0;     /* 按键边沿标志：由 EXTI 回调置位，主循环消费 */
uint8_t        g_alarm_state = 0;   /* 演示用报警状态：0=正常，1=报警（蜂鸣器响+LED常亮） */

/* 按键状态机：避免"消抖后读引脚低才翻转"对短按失效，以及"阻塞等松开"对长推迟滞 */
typedef enum
{
  KEY_IDLE = 0,        /* 空闲，可响应新的按下 */
  KEY_WAIT_RELEASE,    /* 已确认按下并翻转，等待松开（期间忽略抖动边沿） */
  KEY_DEBOUNCE_UP     /* 检测到松开，做 15ms 松开消抖后回到空闲 */
} key_state_t;
key_state_t g_key_state = KEY_IDLE;
uint32_t    g_key_tick = 0;        /* 松开消抖计时起点 */
/* USER CODE END PV */



/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
void Buzzer_On(void);
void Buzzer_Off(void);
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
  /* USER CODE BEGIN 2 */

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /* ---- 按键状态机（非阻塞；不依赖"消抖后读低才翻转"，短按/长按均可靠）---- */
    switch (g_key_state)
    {
      case KEY_IDLE:
        if (g_key_irq)                       /* 下降沿 = 用户按下，立即认定并翻转 */
        {
          g_key_irq = 0;
          g_alarm_state ^= 1;                /* 翻转报警状态 */
          if (g_alarm_state)
          {
            Buzzer_On();                     /* 报警：蜂鸣器响 */
            HAL_GPIO_WritePin(LED_PC13_GPIO_Port, LED_PC13_Pin, GPIO_PIN_RESET); /* LED 常亮 */
          }
          else
          {
            Buzzer_Off();                    /* 报警解除：静音 */
          }
          g_key_state = KEY_WAIT_RELEASE;     /* 锁住，忽略抖动边沿，直到松开 */
        }
        break;

      case KEY_WAIT_RELEASE:
        if (HAL_GPIO_ReadPin(KEY_EXTI_GPIO_Port, KEY_EXTI_Pin) == GPIO_PIN_SET)
        {                                    /* 轮询发现已松开（非阻塞） */
          g_key_tick = HAL_GetTick();
          g_key_state = KEY_DEBOUNCE_UP;
        }
        break;

      case KEY_DEBOUNCE_UP:
        if (HAL_GetTick() - g_key_tick >= 15)/* 松开消抖 15ms */
        {
          g_key_irq = 0;                     /* 清掉抖动期间可能残留的边沿标志 */
          g_key_state = KEY_IDLE;            /* 解锁，准备下一次按下 */
        }
        break;
    }

    /* ---- LED 闪烁 / 报警态常亮 ---- */
    if (g_alarm_state)
    {
      HAL_Delay(100);                        /* 报警态：LED 保持常亮，轮询等待下次按键 */
    }
    else
    {
      HAL_GPIO_TogglePin(LED_PC13_GPIO_Port, LED_PC13_Pin); /* 正常态：1 Hz 闪烁 = 系统存活 */
      HAL_Delay(500);
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
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
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
/* 有源蜂鸣器：低电平响、高电平停（与 .ioc 初始高电平 GPIO_PIN_SET 一致） */
void Buzzer_On(void)
{
  HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_RESET);
}

void Buzzer_Off(void)
{
  HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin, GPIO_PIN_SET);
}

/* PB5/EXTI5 按键中断回调：ISR 保持极简——只置边沿标志，消抖与状态处理放到主循环做（契合"中断收数、主循环处理"原则） */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == KEY_EXTI_Pin)
  {
    g_key_irq = 1;
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
