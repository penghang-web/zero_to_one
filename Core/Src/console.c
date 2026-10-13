/**
  ******************************************************************************
  * @file    console.c
  * @brief   USART1 控制台：printf 日志 + IDLE+DMA 收帧 + 简易 Shell（方案A）
  ******************************************************************************
  *
 * 接收：HAL_UART_Receive_IT 单字节中断把数据搬进 s_rx_buf，遇换行或超时(主循环判定)
 *       成帧；ISR 内只记账并立即重新武装中断（规避 DMA+IDLE 在 F1 上的重武装隐患）。
 * 发送：方案A 用阻塞 HAL_UART_Transmit；printf 用 vsnprintf 格式化后发送。
 * 处理：Console_Process 在主循环里取帧、回显(echo)、解析命令。
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

#define RX_BUF_SIZE  128
#define TX_BUF_SIZE  128
#define HEARTBEAT_MS 5000
#define RX_IDLE_MS       20   /* 文本帧超时成帧（无换行时兜底） */
#define RX_FLASH_IDLE_MS  5   /* 烧录二进制帧超时成帧（字节间隔） */

/* ---- 模块私有 ---- */
static uint8_t  s_rx_buf[RX_BUF_SIZE];
static volatile uint8_t  s_rx_ready = 0;   /* 文本帧就绪（遇换行置位），主循环消费 */
static volatile uint16_t s_rx_len   = 0;   /* 已收字节数 */
static uint8_t  s_rx_byte;                 /* 单字节中断收数暂存 */
static volatile uint32_t s_last_rx_ms = 0; /* 最近一字节到达时刻（超时成帧用） */
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

/* RX 单字节中断回调：每收到 1 字节追加到缓冲，遇换行立即成帧，并重新武装 IT。
   用中断（非 DMA）收数，规避 HAL_UARTEx_ReceiveToIdle_DMA 在 F1 上的重武装/IDLE 隐患。 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    if (s_rx_len < RX_BUF_SIZE)
    {
      s_rx_buf[s_rx_len++] = s_rx_byte;
    }
    s_last_rx_ms = g_systick_ms;
    if (s_rx_byte == '\n' || s_rx_byte == '\r')
    {
      if (s_rx_len > 1)          /* 避免单纯回车成空帧 */
      {
        s_rx_ready = 1;
      }
    }
    HAL_UART_Receive_IT(&huart1, &s_rx_byte, 1);   /* 重新武装，准备收下一字节 */
  }
}

/* 启动 RX：USART 初始化后调用一次；之后每字节由中断自动重新武装 */
void Console_RxStart(void)
{
  s_rx_ready = 0;
  s_rx_len   = 0;
  if (HAL_UART_Receive_IT(&huart1, &s_rx_byte, 1) != HAL_OK)
  {
    /* 启动失败暂不处理，留待后续错误恢复专题 */
  }
}

/* 简易命令解析 + 回显（echo）。命令字全小写 ASCII。 */
static void Shell_Exec(char *line)
{
  /* 去掉结尾的 \r / \n，方便解析与回显 */
  size_t len = strlen(line);
  /* 去除前导空白/控制字符（防御 DMA 帧偶尔带前导垃圾，导致命令词识别失败） */
  size_t start = 0;
  while (start < len && (line[start] == ' ' || line[start] == '\t' || line[start] < 0x20U))
  {
    start++;
  }
  if (start > 0)
  {
    memmove(line, line + start, len - start + 1);
    len -= start;
  }
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
  else if (strncmp(line, "flashcn", 6) == 0)
  {
    /* 烧录字库：PC 侧把 [魔数4字节 + hzk16.bin] 流发进来。默认大小 261700 = 4 + 261696 */
    uint32_t size = 261700;
    if (line[6] == ' ')
    {
      sscanf(line + 7, "%lu", &size);
    }
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
    Console_Printf("unknown: %s (try 'help') [hex", line);
    for (size_t i = 0; i < len && i < 24; i++)
    {
      Console_Printf(" %02X", (uint8_t)line[i]);
    }
    Console_Printf("]\r\n");
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

  /* 判断是否有完整帧：
     文本命令 —— 遇换行立即成帧（s_rx_ready）；
     烧录二进制 —— 靠字节间隔超时成帧（RX_FLASH_IDLE_MS）；
     兜底 —— 文本无换行时 RX_IDLE_MS 超时成帧。 */
  uint8_t  frame_ready = 0;
  uint16_t len = 0;
  __disable_irq();
  if (s_flash_mode)
  {
    if (s_rx_len > 0 && (now - s_last_rx_ms) >= RX_FLASH_IDLE_MS)
    {
      frame_ready = 1;
      len = s_rx_len;
    }
  }
  else
  {
    if (s_rx_ready)
    {
      frame_ready = 1;
      len = s_rx_len;
    }
    else if (s_rx_len > 0 && (now - s_last_rx_ms) >= RX_IDLE_MS)
    {
      frame_ready = 1;
      len = s_rx_len;
    }
  }
  if (!frame_ready)
  {
    return;
  }

  /* 拷出帧数据并清缓冲标志（ISR 仍在收后续字节，不影响本次处理） */
  char line[RX_BUF_SIZE + 1];
  memcpy(line, s_rx_buf, len);
  s_rx_len   = 0;
  s_rx_ready = 0;
  __enable_irq();
  line[len] = '\0';

  /* 烧录模式：收的是二进制流，顺序写 W25Q，每帧回 '>' 节流 */
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
    return;   /* RX 由中断持续武装，无需手动重开 */
  }

  Shell_Exec(line);
  /* 文本命令处理完即返回；RX 由中断持续武装 */
}
