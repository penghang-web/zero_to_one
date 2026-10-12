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
#include "buzzer.h"  /* Buzzer_Beep：一次性蜂鸣（非阻塞） */
#include "watchdog.h" /* WDG_Feed / WDG_TestHang：看门狗状态查询与复位验证 */
#include "w25qxx.h"   /* W25QXX_JEDEC / FontReady / WriteData：flashcn 烧录 + w25 命令 */
#include "ui.h"       /* UI_Demo：oled 命令触发 OLED 演示屏 */
#include "mpu6050.h"  /* 阶段6：MPU6050 原始数据打印 */
#include "stm32f1xx_hal.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

extern volatile uint32_t g_systick_ms;   /* TIM2 1ms 节拍，main.c 维护 */
extern MPU6050_RawData g_mpu;            /* 阶段6：最新采样，main.c 维护 */
extern uint8_t g_mpu_ok;                 /* 阶段6：MPU 是否已唤醒并识别成功 */

#define RX_BUF_SIZE  64
#define TX_BUF_SIZE  128
#define HEARTBEAT_MS 5000

/* ---- 模块私有 ---- */
static uint8_t  s_rx_buf[RX_BUF_SIZE];
static volatile uint8_t  s_rx_ready = 0;   /* 帧就绪标志：ISR 置位，主循环消费 */
static volatile uint16_t s_rx_len   = 0;   /* 本帧字节数 */
static uint32_t s_heartbeat_ms = 0;

/* flashcn 烧录模式状态：进入后 Console_Process 把收到的帧当作二进制流直接写 W25Q */
static uint8_t  s_flash_mode   = 0;   /* 1 = 正在接收二进制流写 W25Q */
static uint32_t s_flash_total  = 0;   /* 期望接收总字节 */
static uint32_t s_flash_offset = 0;   /* 已写入字节 */

/* mpu 持续打印流：'mpu on' 开启后每 500ms 打印一行原始三轴数据 */
static uint8_t  s_mpu_stream   = 0;

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
    Console_Printf("Commands:\r\n help\r\n info\r\n led on\r\n led off\r\n beep <ms> [duty]\r\n wdg [status|stop]\r\n w25\r\n oled\r\n flashcn <size>\r\n mpu [on|off]\r\n");
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
  else if (strncmp(line, "beep", 4) == 0 && (line[4] == ' ' || line[4] == '\0'))
  {
    unsigned long ms = 200;
    unsigned duty = 50;
    sscanf(line, "beep %lu %u", &ms, &duty);
    if (duty > 100) duty = 100;
    Buzzer_Beep((uint16_t)ms, (uint8_t)duty);   /* 一次性蜂鸣（非阻塞，ms 后自动停） */
    Console_Printf("beep %lu ms @ %u%%\r\n", ms, duty);
  }
  else if (strncmp(line, "wdg", 3) == 0 && (line[3] == ' ' || line[3] == '\0'))
  {
    if (strcmp(line, "wdg") == 0 || strcmp(line, "wdg status") == 0)
    {
      Console_Printf("IWDG active, timeout~1.5s, feeding=%s, feed_cnt=%lu, uptime=%lu s\r\n",
                     WDG_IsFeeding() ? "ON" : "OFF",
                     (unsigned long)WDG_FeedCount(),
                     (unsigned long)(g_systick_ms / 1000u));
    }
    else if (strcmp(line, "wdg stop") == 0 || strcmp(line, "wdg test") == 0 ||
             strcmp(line, "wdg hang") == 0)
    {
      Console_Printf("stop feeding IWDG and hang -> MCU will reset in ~1.5s\r\n");
      WDG_TestHang();   /* 关喂狗 + 死循环，验证看门狗超时复位 */
    }
    else
    {
      Console_Printf("usage: wdg [status|stop]\r\n");
    }
  }
  else if (strcmp(line, "w25") == 0)
  {
    uint8_t id[3];
    W25QXX_JEDEC(id);
    Console_Printf("W25Q JEDEC %02X %02X %02X, cap=%lu B, font=%s\r\n",
                   id[0], id[1], id[2],
                   (unsigned long)W25QXX_CapacityBytes(),
                   W25QXX_FontReady() ? "READY" : "none");
  }
  else if (strcmp(line, "oled") == 0)
  {
    UI_Demo();
    Console_Printf("oled demo refreshed (font=%s)\r\n", W25QXX_FontReady() ? "READY" : "none");
  }
  else if (strncmp(line, "mpu", 3) == 0 && (line[3] == ' ' || line[3] == '\0'))
  {
    /* mpu         —— 打印一次当前原始三轴数据
     * mpu on      —— 持续打印流（每 500ms 一行），便于观察晃动时数据跟随变化
     * mpu off     —— 关闭持续打印流 */
    if (strcmp(line, "mpu") == 0)
    {
      if (g_mpu_ok)
      {
        Console_Printf("MPU raw: ax=%d ay=%d az=%d gx=%d gy=%d gz=%d t=%d\r\n",
                       g_mpu.ax, g_mpu.ay, g_mpu.az,
                       g_mpu.gx, g_mpu.gy, g_mpu.gz, g_mpu.temp);
      }
      else
      {
        Console_Printf("MPU not ready (device absent or not woken). retrying...\r\n");
      }
    }
    else if (strcmp(line, "mpu on") == 0)
    {
      s_mpu_stream = 1;
      Console_Printf("MPU stream ON (500ms/tick). 'mpu off' to stop.\r\n");
    }
    else if (strcmp(line, "mpu off") == 0)
    {
      s_mpu_stream = 0;
      Console_Printf("MPU stream OFF.\r\n");
    }
    else
    {
      Console_Printf("usage: mpu | mpu on | mpu off\r\n");
    }
  }
  else if (strncmp(line, "flashcn", 6) == 0 && (line[6] == ' ' || line[6] == '\0'))
  {
    /* 烧录字库：PC 侧把 [魔数4字节 + hzk16.bin] 流发进来。默认大小 261700 = 4 + 261696 */
    uint32_t size = 261700;
    sscanf(line, "flashcn %lu", &size);
    if (size < 4) size = 4;
    Console_Printf("FLASCN erase %lu bytes...\r\n", (unsigned long)size);
    W25QXX_EraseRange(0, size);          /* 擦除字库区（阻塞；TIM2 ISR 仍喂狗，不会误复位） */
    s_flash_offset = 0;
    s_flash_total  = size;
    s_flash_mode   = 1;
    Console_Printf("FLASCN READY %lu\r\n", (unsigned long)size);
    Console_Printf("send binary now (<=60B/frame); MCU ACKs each frame with '>'.\r\n");
  }
  else
  {
    Console_Printf("unknown: %s (try 'help')\r\n", line);
  }
}

/* 主循环调用：心跳日志 + 处理已收帧并重新启动 RX */
void Console_Process(void)
{
  /* 5s 心跳：证明系统存活、日志通道通畅（由 TIM2 1ms 节拍驱动，非阻塞）。
     烧录模式下不打心跳，避免 '>' 之外多出字符污染二进制流。 */
  uint32_t now = g_systick_ms;
  if (!s_flash_mode && now - s_heartbeat_ms >= HEARTBEAT_MS)
  {
    s_heartbeat_ms = now;
    Console_Printf("[hb] uptime=%lu s, font=%s\r\n",
                   (unsigned long)(now / 1000u),
                   W25QXX_FontReady() ? "OK" : "none");
  }

  /* mpu 持续打印流：'mpu on' 开启后每 500ms 打印一次原始三轴数据。
     主循环已持续把采样写进 g_mpu，这里只负责按节拍输出（烧录模式不打，避免污染二进制流）。 */
  static uint32_t s_mpu_print_ms = 0;
  if (s_mpu_stream && !s_flash_mode && g_mpu_ok && now - s_mpu_print_ms >= 500U)
  {
    s_mpu_print_ms = now;
    Console_Printf("MPU ax=%d ay=%d az=%d gx=%d gy=%d gz=%d\r\n",
                   g_mpu.ax, g_mpu.ay, g_mpu.az, g_mpu.gx, g_mpu.gy, g_mpu.gz);
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

  /* 烧录模式：收的是二进制流，不解析为命令，直接顺序写 W25Q，每帧回 ACK 节流 */
  if (s_flash_mode)
  {
    W25QXX_WriteData(s_flash_offset, (const uint8_t *)line, len);
    s_flash_offset += len;
    Console_Tx((const uint8_t *)">", 1);
    if (s_flash_offset >= s_flash_total)
    {
      s_flash_mode = 0;
      Console_Printf("\r\nFLASCN DONE bytes=%lu\r\n", (unsigned long)s_flash_offset);
    }
    HAL_UARTEx_ReceiveToIdle_DMA(&huart1, s_rx_buf, RX_BUF_SIZE);
    return;
  }

  Shell_Exec(line);

  /* 重新武装 RX，准备收下一帧（Normal 模式 DMA 每次需重启） */
  HAL_UARTEx_ReceiveToIdle_DMA(&huart1, s_rx_buf, RX_BUF_SIZE);
}
