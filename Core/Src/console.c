/**
  ******************************************************************************
  * @file    console.c
  * @brief   USART1 控制台：printf 日志 + 命令行 Shell + flashcn 烧录
  * @note    收发由 uart_driver 接管（中断 + 环形缓冲，非阻塞，带错误恢复）。
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
#include "w25qxx.h"   /* W25QXX_JEDEC / FontReady / WriteData：flashcn 烧录 + w25 命令 */
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
#define LINE_MAX     160          /* 命令行缓冲（含烧录可能的较长行） */

/* flashcn 烧录模式状态：进入后 Console_Process 把收到的二进制流写 W25Q，每 60 字节回 '>' */
static uint8_t  s_flash_mode   = 0;   /* 1 = 正在接收二进制流写 W25Q */
static uint32_t s_flash_total  = 0;   /* 期望接收总字节 */
static uint32_t s_flash_offset = 0;   /* 已写入字节 */
static uint8_t  s_flash_buf[128];     /* 累积接收的二进制帧，按 60 字节切 */
static uint16_t s_flash_buf_len = 0;

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

/* 启动 RX：USART 初始化后调用一次；初始化 uart_driver（接管收发中断）。
 * 之后每字节由 UART_IRQHandler 自动写入环形缓冲，无需手动重武装。 */
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
    SSD1306_UI_Demo();
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
    s_flash_buf_len = 0;
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

/* 主循环调用：心跳日志 + 处理已收数据（命令行/烧录二进制流） */
void Console_Process(void)
{
  static uint32_t s_heartbeat_ms = 0;
  static uint32_t s_mpu_print_ms = 0;
  uint32_t now = g_systick_ms;

  /* 5s 心跳：证明系统存活、日志通道通畅（由 TIM2 1ms 节拍驱动，非阻塞）。
     烧录模式下不打心跳，避免 '>' 之外多出字符污染二进制流。 */
  if (!s_flash_mode && now - s_heartbeat_ms >= HEARTBEAT_MS)
  {
    s_heartbeat_ms = now;
    Console_Printf("[hb] uptime=%lu s, font=%s\r\n",
                   (unsigned long)(now / 1000u),
                   W25QXX_FontReady() ? "OK" : "none");
  }

  /* mpu 持续打印流：'mpu on' 开启后每 500ms 打印一次原始三轴数据。
     主循环已持续把采样写进 g_mpu，这里只负责按节拍输出（烧录模式不打，避免污染二进制流）。 */
  if (s_mpu_stream && !s_flash_mode && g_mpu_ok && now - s_mpu_print_ms >= 500U)
  {
    s_mpu_print_ms = now;
    Console_Printf("MPU ax=%d ay=%d az=%d gx=%d gy=%d gz=%d\r\n",
                   g_mpu.ax, g_mpu.ay, g_mpu.az, g_mpu.gx, g_mpu.gy, g_mpu.gz);
  }

  /* ---- 烧录模式：收的是二进制流，按 60 字节/帧顺序写 W25Q，每帧回 '>' 节流 ---- */
  if (s_flash_mode)
  {
    uint8_t tmp[128];
    uint16_t got = UART_ReadRaw(&g_uart1, tmp, (uint16_t)sizeof(tmp));
    if (got > 0)
    {
      /* 累积进 flash 缓冲（防止单圈只到半帧） */
      for (uint16_t i = 0; i < got && s_flash_buf_len < (uint16_t)sizeof(s_flash_buf); i++)
      {
        s_flash_buf[s_flash_buf_len++] = tmp[i];
      }
      /* 凑满一帧（60 字节，或最后一帧不足 60）即写 W25Q 并回 '>' */
      uint16_t frame = (s_flash_offset + 60U <= s_flash_total)
                            ? 60U : (uint16_t)(s_flash_total - s_flash_offset);
      while (s_flash_buf_len >= frame && s_flash_offset < s_flash_total)
      {
        W25QXX_WriteData(s_flash_offset, s_flash_buf, frame);
        s_flash_offset += frame;
        memmove(s_flash_buf, s_flash_buf + frame, (size_t)(s_flash_buf_len - frame));
        s_flash_buf_len -= frame;
        Console_Tx((const uint8_t *)">", 1);
        frame = (s_flash_offset + 60U <= s_flash_total)
                    ? 60U : (uint16_t)(s_flash_total - s_flash_offset);
      }
      if (s_flash_offset >= s_flash_total)
      {
        s_flash_mode = 0;
        Console_Printf("\r\nFLASCN DONE bytes=%lu\r\n", (unsigned long)s_flash_offset);
      }
    }
    return;   /* RX 由 uart_driver 持续接收，无需手动重开 */
  }

  /* ---- 命令模式：把收到的原始字节汇编成行（遇 \n / \r 结束） ---- */
  uint8_t tmp[LINE_MAX];
  uint16_t got = UART_ReadRaw(&g_uart1, tmp, (uint16_t)sizeof(tmp));
  if (got > 0)
  {
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
}
