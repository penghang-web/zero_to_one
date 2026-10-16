/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f1xx_it.c
  * @brief   Interrupt Service Routines.
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
#include "stm32f1xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "console.h"   /* g_uart1 + UART_IRQHandler：接管 USART1 收发中断 */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* ── HardFault 诊断陷阱（无调试器也能定位崩溃点）──
 * 通过 USART1 轮询打印故障现场，不依赖任何中断（故障态下也可用）。
 * 打印完回到默认死循环，等 IWDG(~26s) 或手动复位。
 * 仅用于诊断；正常不应进 HardFault。 */
static void hf_uart_putc(char c)
{
  USART_TypeDef *u = USART1;
  uint32_t t = 0U;
  while (!(u->SR & 0x80U))          /* 等 TXE */
  {
    if (++t > 2000000U) return;     /* USART 未就绪则放弃，避免死等 */
  }
  u->DR = (uint8_t)c;
}
static void hf_uart_puts(const char *s)
{
  while (*s) hf_uart_putc(*s++);
}
static void hf_uart_hex(uint32_t v)
{
  hf_uart_puts("0x");
  for (int i = 7; i >= 0; i--)
  {
    uint32_t n = (v >> (uint32_t)(i * 4U)) & 0xFU;
    hf_uart_putc((char)((n < 10U) ? ('0' + n) : ('A' + n - 10U)));
  }
}
/* sp = 故障现场栈帧指针；exc_lr = 进入 HardFault 时的 EXC_RETURN（bit2 指示用 MSP/PSP） */
static void hf_dump_fault(uint32_t *sp, uint32_t exc_lr)
{
  uint32_t pc = sp[6];        /* 栈帧顺序: R0,R1,R2,R3,R12,LR,PC,xPSR */
  uint32_t lr_fault = sp[5];
  SCB_Type *scb = SCB;
  (void)exc_lr;
  hf_uart_puts("\r\n[HardFault] PC="); hf_uart_hex(pc);
  hf_uart_puts(" LR=");        hf_uart_hex(lr_fault);
  hf_uart_puts(" CFSR=");      hf_uart_hex(scb->CFSR);
  hf_uart_puts(" MMFAR=");     hf_uart_hex(scb->MMFAR);
  hf_uart_puts(" BFAR=");       hf_uart_hex(scb->BFAR);
  hf_uart_puts("\r\n");
}
/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern DMA_HandleTypeDef hdma_adc1;
extern TIM_HandleTypeDef htim2;
extern DMA_HandleTypeDef hdma_usart1_rx;
extern UART_HandleTypeDef huart1;
extern TIM_HandleTypeDef htim4;

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M3 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */
  /* 取故障现场栈帧：进 HardFault 时 LR=EXC_RETURN，
   * bit2(0x4) 指示栈帧在 PSP(线程栈) 还是 MSP(主栈)。 */
  uint32_t lr_val;
  uint32_t *sp;
  __asm volatile ("MOV %0, LR" : "=r" (lr_val));
  if ((lr_val & 0x4U) != 0U)
    __asm volatile ("MRS %0, PSP" : "=r" (sp));
  else
    __asm volatile ("MRS %0, MSP" : "=r" (sp));
  hf_dump_fault(sp, lr_val);   /* 通过 USART1 打印 PC/LR/CFSR/MMFAR/BFAR */
  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */

  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Prefetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */

  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */

  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/******************************************************************************/
/* STM32F1xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32f1xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles DMA1 channel1 global interrupt.
  */
void DMA1_Channel1_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Channel1_IRQn 0 */

  /* USER CODE END DMA1_Channel1_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_adc1);
  /* USER CODE BEGIN DMA1_Channel1_IRQn 1 */

  /* USER CODE END DMA1_Channel1_IRQn 1 */
}

/**
  * @brief This function handles DMA1 channel5 global interrupt.
  */
void DMA1_Channel5_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Channel5_IRQn 0 */

  /* USER CODE END DMA1_Channel5_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_usart1_rx);
  /* USER CODE BEGIN DMA1_Channel5_IRQn 1 */

  /* USER CODE END DMA1_Channel5_IRQn 1 */
}

/**
  * @brief This function handles EXTI line[9:5] interrupts.
  */
void EXTI9_5_IRQHandler(void)
{
  /* USER CODE BEGIN EXTI9_5_IRQn 0 */

  /* USER CODE END EXTI9_5_IRQn 0 */
  HAL_GPIO_EXTI_IRQHandler(KEY_EXTI_Pin);
  /* USER CODE BEGIN EXTI9_5_IRQn 1 */

  /* USER CODE END EXTI9_5_IRQn 1 */
}

/**
  * @brief This function handles TIM2 global interrupt.
  */
void TIM2_IRQHandler(void)
{
  /* USER CODE BEGIN TIM2_IRQn 0 */

  /* USER CODE END TIM2_IRQn 0 */
  HAL_TIM_IRQHandler(&htim2);
  /* USER CODE BEGIN TIM2_IRQn 1 */

  /* USER CODE END TIM2_IRQn 1 */
}

/**
  * @brief This function handles TIM4 global interrupt.
  */
void TIM4_IRQHandler(void)
{
  /* USER CODE BEGIN TIM4_IRQn 0 */

  /* USER CODE END TIM4_IRQn 0 */
  HAL_TIM_IRQHandler(&htim4);
  /* USER CODE BEGIN TIM4_IRQn 1 */

  /* USER CODE END TIM4_IRQn 1 */
}

/**
  * @brief This function handles USART1 global interrupt.
  */
void USART1_IRQHandler(void)
{
  /* USER CODE BEGIN USART1_IRQn 0 */
  /* ★ 必须走自定义 UART_IRQHandler：它负责清 IDLE 标志(在 HAL_UART_IRQHandler
   *   之前)，并把其余 RX/TX/错误转交 HAL_UART_IRQHandler。
   *   若直接调 HAL_UART_IRQHandler，IDLE 标志永不清 → ISR 风暴 → 系统卡死 →
   *   IWDG 超时复位（表现即"发数据就重启"）。详见 uart_driver.h 注释。 */
  UART_IRQHandler(&g_uart1);
  return;   /* 跳过下方生成的 HAL_UART_IRQHandler 重复调用 */
  /* USER CODE END USART1_IRQn 0 */
  HAL_UART_IRQHandler(&huart1);
  /* USER CODE BEGIN USART1_IRQn 1 */

  /* USER CODE END USART1_IRQn 1 */
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
