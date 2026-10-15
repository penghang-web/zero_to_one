/**
  ******************************************************************************
  * @file    console.c
  * @brief   USART1 控制台：printf 日志 + 命令行 Shell
 * @note    收发由 uart_driver 接管（DMA 循环接收 + 环形缓冲，非阻塞，带错误恢复）。
 *          本文件不再使用 HAL_UART_Receive_IT / HAL_UART_Transmit 等阻塞/单次接口，
 *          统一通过 uart_driver 的 UART_IRQHandler、UART_ReadRaw、UART_Send 完成。
  ******************************************************************************
  */
#include "console.h"
#include "uart_driver.h"   /* g_uart1（本文件定义） + UART_* API */
#include "usart.h"   /* huart1 (extern) */
#include "key.h"     /* Key_SetAlarm / Key_GetAlarmState */
#include "buzzer.h"  /* Buzzer_Beep：一次性蜂鸣（非阻塞） */
#include "watchdog.h" /* WDG_Feed / WDG_TestHang：看门狗状态查询与复位验证 */
#include "ssd1306.h"   /* SSD1306_UI_Demo：oled 命令触发 OLED 演示屏 */
#include "mpu6050.h"  /* 阶段6：MPU6050 原始数据打印 */
#include "stm32f1xx_hal.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

extern volatile uint32_t g_systick_ms;   /* TIM2 1ms 节拍，main.c 维护 */
/* g_mpu / g_mpu_ok 已由 mpu6050.h 通过 extern 声明（定义于 mpu6050.c），此处不再重复 */

/* ---- UART 驱动实例与缓冲（由 uart_driver 接管 USART1 收发）---- */
UART_Driver_t g_uart1;
static uint8_t s_uart_rx[512];   /* 接收环形缓冲（≥2 的幂，远大于 60 字节/帧） */
static uint8_t s_uart_tx[256];   /* 发送环形缓冲 */

#define HEARTBEAT_MS 5000
#define LINE_MAX     160          /* 命令行缓冲 */

/* mpu 持续打印流：'mpu on' 开启后每 500ms 打印一行原始三轴数据 */
static uint8_t  s_mpu_stream   = 0;

/* 命令行汇编缓冲（命令模式使用） */
static char     s_line[LINE_MAX + 1];
static uint16_t s_line_len = 0;

/* 阻塞发送：把数据压入 uart_driver 发送环形缓冲，由 TXE 中断非阻塞发出。
 * 若发送缓冲暂时满了，短自旋等待 ISR 腾出空间（带上限防止极端情况下死循环）。 */
void Console_Tx(const uint8_t *buf, uint16_t len)
{
  if (buf == NULL || len == 0)
  {
    return;
  }
  const uint8_t *p = buf;
  uint16_t rem = len;
  uint32_t guard = 0;
  while (rem > 0)
  {
    uint16_t free = UART_GetTxFree(&g_uart1);
    if (free > 0)
    {
      uint16_t n = (rem < free) ? rem : free;
      UART_Send(&g_uart1, p, n);
      p += n;
      rem -= n;
      guard = 0;
    }
    else if (++guard > 2000000U)
    {
      break;   /* 极端异常（如中断未接管）时主动放弃，避免主循环卡死 */
    }
  }
}

/* printf 风格日志：格式化进局部缓冲再发送（字符串保持 ASCII，中文走注释） */
void Console_Printf(const char *fmt, ...)
{
  static char tbuf[LINE_MAX];
  va_list ap;

  va_start(ap, fmt);
  int n = vsnprintf(tbuf, sizeof(tbuf), fmt, ap);
  va_end(ap);

  if (n > 0)
  {
    Console_Tx((uint8_t *)tbuf, (uint16_t)n);
  }
}

/* 启动 RX：USART 初始化后调用一次；初始化 uart_driver（接管收发）。
 * 之后 DMA 循环把串口数据直接写入环形缓冲，IDLE 线中断负责"一帧收完"通知，
 * 全程非阻塞、零拷贝，无需每字节中断或手动重武装。 */
void Console_RxStart(void)
{
  UART_DriverConfig_t cfg = {0};
  cfg.rx_buf  = s_uart_rx;
  cfg.rx_size = (uint16_t)sizeof(s_uart_rx);
  cfg.tx_buf  = s_uart_tx;
  cfg.tx_size = (uint16_t)sizeof(s_uart_tx);
  /* frame 全零 = 原始流模式：UART_ReadRaw 直接取字节，帧界定交给上层（命令行/烧录） */
  if (UART_Init(&g_uart1, &huart1, &cfg) != HAL_OK)
  {
    /* 初始化失败暂不处理，留待后续错误恢复专题 */
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
  /* 去除前导空白/控制字符（防御偶尔带前导垃圾，导致命令词识别失败） */
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

  /* 回显收到的行（验证 TX/RX 双通） */
  Console_Printf("%s\r\n", line);

  if (len == 0)
  {
    return;
  }

  if (strcmp(line, "help") == 0)
  {
    Console_Printf("Commands:\r\n help\r\n info\r\n led on\r\n led off\r\n beep <ms> [duty]\r\n wdg [status|stop]\r\n oled\r\n mpu [on|off]\r\n");
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
      Console_Printf("IWDG active, feeding=%s, feed_cnt=%lu, uptime=%lu s\r\n",
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
  else if (strcmp(line, "oled") == 0)
  {
    SSD1306_UI_Demo();
    Console_Printf("oled demo refreshed\r\n");
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
  /* flashcn 字库烧录功能已移除（不再依赖 W25Q 字库，仅英文/ASCII 显示） */
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

/* 主循环调用：心跳日志 + 处理已收数据（命令行/烧录二进制流） */
void Console_Process(void)
{
  static uint32_t s_heartbeat_ms = 0;
  static uint32_t s_mpu_print_ms = 0;
  static uint32_t s_last_rx_ms   = 0;   /* 最近一次收到字节的时间戳，用于空闲自动提交 */
  uint32_t now = g_systick_ms;

  /* ---- RX 链路自愈（兜底，DMA 循环接收）----
   * 正常接收由 DMA 直接搬入 rx_buf，IDLE 线中断负责"一帧收完"通知。
   * 若 HAL 因 ORE/PE/FE 等串口错误把 DMA 通道关闭，后续字节便永久收不到
   * （表现即“首字节后无响应 / 点发送没反应”）。这里每圈主循环检测一次：
   * DMA 通道使能位(CCR.EN)若被清掉，就强制回 READY 并重启 DMA 循环接收，
   * 开销极小，可兜住任何静态分析之外的卡死。 */
  if (huart1.hdmarx == NULL ||
      (huart1.hdmarx->Instance->CCR & DMA_CCR_EN) == 0U)
  {
    huart1.RxState = HAL_UART_STATE_READY;
    (void)HAL_UART_Receive_DMA(&huart1, g_uart1.rx_buf, g_uart1.rx_size);
    __HAL_UART_ENABLE_IT(&huart1, UART_IT_IDLE);
  }

  /* 5s 心跳：证明系统存活、日志通道通畅（由 TIM2 1ms 节拍驱动，非阻塞） */
  if (now - s_heartbeat_ms >= HEARTBEAT_MS)
  {
    s_heartbeat_ms = now;
    Console_Printf("[hb] uptime=%lu s\r\n",
                   (unsigned long)(now / 1000u));
  }

  /* mpu 持续打印流：'mpu on' 开启后每 500ms 打印一次原始三轴数据。
     主循环已持续把采样写进 g_mpu，这里只负责按节拍输出（烧录模式不打，避免污染二进制流）。 */
  if (s_mpu_stream && g_mpu_ok && now - s_mpu_print_ms >= 500U)
  {
    s_mpu_print_ms = now;
    Console_Printf("MPU ax=%d ay=%d az=%d gx=%d gy=%d gz=%d\r\n",
                   g_mpu.ax, g_mpu.ay, g_mpu.az, g_mpu.gx, g_mpu.gy, g_mpu.gz);
  }

  /* ---- 命令模式：把收到的原始字节汇编成行（遇 \n / \r 结束） ---- */
  uint8_t tmp[LINE_MAX];
  uint16_t got = UART_ReadRaw(&g_uart1, tmp, (uint16_t)sizeof(tmp));
  if (got > 0)
  {
    s_last_rx_ms = now;   /* 记录最近收字节时间，供下方空闲自动提交判断 */
    for (uint16_t i = 0; i < got; i++)
    {
      uint8_t c = tmp[i];
      if (c == '\n' || c == '\r')
      {
        if (s_line_len > 0)
        {
          s_line[s_line_len] = '\0';
          Shell_Exec(s_line);   /* Shell_Exec 内部会再做一次 trim */
          s_line_len = 0;
        }
        /* 连续 \r\n 不产生空行 */
      }
      else if (c == '\b' || c == 0x7F)   /* 退格 / DEL：删除一个字符 */
      {
        if (s_line_len > 0) s_line_len--;
      }
      else if (s_line_len < LINE_MAX)
      {
        s_line[s_line_len++] = (char)c;
      }
    }
  }

  /* 空闲自动提交：串口助手的"发送"按钮常整段突发发送、且不带换行符，
   * 若只等 \n/\r，命令行会永远堆在缓冲里不执行（表现即"点了发送没反应"）。
   * 这里：只要缓冲里有未提交内容、且距最近一个字节已空闲 250ms，就自动提交。
   * 115200bps 下 250ms 可收约 2800 字节，远大于单条命令，不会把一条命令拆断；
   * 交互式逐字符输入每字符间隔>250ms 才会触发，本场景不会误触发。 */
  if (s_line_len > 0 && (now - s_last_rx_ms) >= 250U)
  {
    s_line[s_line_len] = '\0';
    Shell_Exec(s_line);
    s_line_len = 0;
  }
}
