/**
  ******************************************************************************
  * @file    uart_driver.c
  * @brief   基于 STM32 HAL 的稳健 UART 驱动（中断 + 环形缓冲 FIFO）底层实现
  * @note    本文件不直接调用 HAL_UART_Transmit / HAL_UART_Receive_IT 等阻塞/单次接口，
  *         而是直接操作 USART 寄存器，配合自定义 UART_IRQHandler 实现持续收发。
  ******************************************************************************
  */
#include "uart_driver.h"
#include "stm32f1xx_hal_dma.h"   /* __HAL_DMA_GET_COUNTER / DMA_CCR_EN 等 DMA 宏 */

/* 当前激活的 UART 实例（UART_Init 时记录），供 HAL 回调定位驱动。
 * 本项目仅一个 USART1，单实例即可；多实例可改为数组/链表。 */
static UART_Driver_t *s_active = NULL;

/* ===========================================================================
 * 临界区工具：Cortex-M3 上 16 位环形索引读写本身是原子的，但为了防止
 * "应用读索引" 与 "ISR 写索引" 在判断满/空时出现撕裂，凡涉及 head/tail 的
 * 读-改-写均在关闭中断(全局 PRIMASK)下进行。
 * ========================================================================= */
static inline uint32_t uart_enter_critical(void)
{
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    return pm;
}

static inline void uart_exit_critical(uint32_t pm)
{
    __set_PRIMASK(pm);
}

/* ===========================================================================
 * 环形缓冲内部工具
 * ========================================================================= */

/* 在环形缓冲区域 [start, start+len) 中从 0 偏移起匹配 pattern，
 * 返回匹配的相对偏移；未找到返回 -1。 */
static int uart_find_pattern(const uint8_t *buf, uint16_t size,
                             uint16_t start, uint16_t len,
                             const uint8_t *pat, uint16_t pat_len)
{
    if (pat_len == 0U || pat_len > len)
        return -1;
    for (uint16_t i = 0U; i + pat_len <= len; i++)
    {
        bool match = true;
        for (uint16_t j = 0U; j < pat_len; j++)
        {
            if (buf[(start + i + j) % size] != pat[j])
            {
                match = false;
                break;
            }
        }
        if (match)
            return (int)i;
    }
    return -1;
}

/* DMA circular 模式下，串口数据已由 DMA 实时刻写入 rx_buf；
 * rx_head 须反映 DMA 当前写入位置 = rx_size - NDTR(剩余未传字节数)。
 * 在读取共享索引(rx_head)前调用，使软件环形缓冲视角与硬件 DMA 一致。
 * 注：DMA 是总线主控，不受 PRIMASK 影响；此处仅在临界区内取一次快照即可。 */
static inline void uart_sync_rx_head(UART_Driver_t *d)
{
    if (d->huart->hdmarx != NULL)
    {
        d->rx_head = (uint16_t)(d->rx_size
                  - __HAL_DMA_GET_COUNTER(d->huart->hdmarx));
    }
}

/* ===========================================================================
 * 帧定位：在接收缓冲中找一帧，输出帧边界与载荷信息
 *   frame_start : 帧起始的绝对环形下标
 *   frame_total : 从 frame_start 起需消费的总字节数(含帧头/长度/帧尾)
 *   payload_off : 载荷相对 frame_start 的偏移
 *   payload_len : 载荷字节数
 * 返回 true 表示找到完整帧(可消费)，false 表示尚无完整帧(可能已做重同步)。
 * 注意：调用方须处于临界区，以保证 rx_head/rx_tail 不被 ISR 改写。
 * ========================================================================= */
static bool uart_locate_frame(UART_Driver_t *drv,
                              uint16_t *frame_start, uint16_t *frame_total,
                              uint16_t *payload_off, uint16_t *payload_len)
{
    uint16_t size  = drv->rx_size;
    uint16_t tail  = drv->rx_tail;
    uint16_t head  = drv->rx_head;
    uint16_t avail = (uint16_t)(head - tail) % size;
    if (avail == 0U)
        return false;

    const uart_frame_cfg_t *f = &drv->frame;

    /* 1) 定位帧头 */
    uint16_t start = tail;
    if (f->header_len > 0U && f->header != NULL)
    {
        int off = uart_find_pattern(drv->rx_buf, size, tail, avail,
                                    f->header, f->header_len);
        if (off < 0)
        {
            /* 未找到帧头：保留末尾 (header_len-1) 字节，避免帧头被截断在两次读取之间 */
            uint16_t keep = (uint16_t)f->header_len - 1U;
            if (avail > keep)
                drv->rx_tail = (uint16_t)(tail + avail - keep) % size;
            return false;
        }
        start = (uint16_t)(tail + (uint16_t)off) % size;
        avail = (uint16_t)(head - start) % size;
        if (avail < f->header_len)
            return false;       /* 帧头尚未收全，等更多数据 */
    }

    uint16_t body = (uint16_t)(start + f->header_len) % size;  /* 帧头之后位置 */

    /* 2) 确定帧长度与载荷 */
    uint16_t total;     /* 从 start 起的总字节数(含帧头) */
    uint16_t pay_off;   /* 载荷相对 start 偏移 */
    uint16_t pay_len;   /* 载荷字节数 */

    if (f->use_length)
    {
        uint16_t len_field = (uint16_t)(body + f->length_offset) % size;
        uint16_t need = (uint16_t)(f->header_len + f->length_offset + f->length_size);
        if (avail < need)
            return false;   /* 长度字段未收全，等更多数据 */

        /* 读取长度值 L */
        uint32_t L = 0U;
        for (uint8_t i = 0U; i < f->length_size; i++)
        {
            uint8_t b = drv->rx_buf[(uint16_t)(len_field + i) % size];
            if (f->big_endian)
                L = (L << 8U) | b;
            else
                L = L | ((uint32_t)b << (8U * i));
        }
        if (L > 0xFFFFU)
            return false;

        /* 整帧长度 = 帧头 + 长度字段 + L(载荷+帧尾) */
        uint32_t t = (uint32_t)f->header_len + f->length_size + L;
        if (f->max_frame != 0U && t > f->max_frame)
        {
            /* 超长帧，视为损坏：丢弃到帧头之后，等待重同步 */
            drv->rx_tail = body;
            return false;
        }
        if (avail < (uint16_t)t)
            return false;   /* 整帧尚未收全 */

        /* 校验帧尾(若启用)：应为 L 字节末尾的 tail_len 个字节 */
        if (f->tail_len > 0U && f->tail != NULL)
        {
            uint16_t tail_pos = (uint16_t)(start + f->header_len + f->length_size
                                           + (uint16_t)L - f->tail_len) % size;
            for (uint8_t i = 0U; i < f->tail_len; i++)
            {
                if (drv->rx_buf[(uint16_t)(tail_pos + i) % size] != f->tail[i])
                {
                    /* 帧尾不匹配，丢弃该帧头，从下一字节重新同步 */
                    drv->rx_tail = (uint16_t)(start + 1U) % size;
                    return false;
                }
            }
        }
        total   = (uint16_t)t;
        pay_off = (uint16_t)(f->header_len + f->length_size);
        pay_len = (uint16_t)L - (uint16_t)f->tail_len;  /* 帧尾不计入载荷 */
    }
    else if (f->tail_len > 0U && f->tail != NULL)
    {
        /* 无长度字段，仅用帧尾界定 */
        int off = uart_find_pattern(drv->rx_buf, size, body, avail, f->tail, f->tail_len);
        if (off < 0)
            return false;   /* 帧尾未到 */
        total   = (uint16_t)(f->header_len + (uint16_t)off + f->tail_len);
        pay_off = f->header_len;
        pay_len = (uint16_t)off;   /* 帧头之后到帧尾之前 */
    }
    else
    {
        /* 无帧头/长度/帧尾：原始流模式，整段作为一帧 */
        total   = avail;
        pay_off = 0U;
        pay_len = avail;
    }

    *frame_start = start;
    *frame_total = total;
    *payload_off = pay_off;
    *payload_len = pay_len;
    return true;
}

/* ===========================================================================
 * 对外 API 实现
 * ========================================================================= */

HAL_StatusTypeDef UART_Init(UART_Driver_t *drv, UART_HandleTypeDef *huart,
                             const UART_DriverConfig_t *cfg)
{
    if (drv == NULL || huart == NULL || cfg == NULL)
        return HAL_ERROR;
    if (cfg->rx_buf == NULL || cfg->rx_size < 2U ||
        cfg->tx_buf == NULL || cfg->tx_size < 2U)
        return HAL_ERROR;

    drv->huart       = huart;
    drv->rx_buf      = cfg->rx_buf;
    drv->rx_size     = cfg->rx_size;
    drv->rx_head     = 0U;
    drv->rx_tail     = 0U;
    drv->tx_buf      = cfg->tx_buf;
    drv->tx_size     = cfg->tx_size;
    drv->tx_head     = 0U;
    drv->tx_tail     = 0U;
    drv->tx_sending  = 0U;
    drv->tx_ongoing  = false;
    drv->rx_overflow = 0U;
    drv->err_count   = 0U;
    drv->frame_ready = false;
    drv->frame       = cfg->frame;
    drv->frame_cb    = cfg->frame_cb;

    /* 记录当前激活实例，供 HAL 回调(TxCplt/Error)定位驱动 */
    s_active = drv;

    /* 开启 DMA 循环接收：DMA 直接把串口数据搬进 rx_buf(全程零拷贝、无每字节中断)。
     * IDLE 线中断单独使能，由 UART_IRQHandler 检测"一帧收完"并通知上层。
     * 注意：DMA circular 模式下若消费不及时，最旧数据会被覆盖(环形缓冲固有行为)，
     *       本项目 rx_buf=512 远大于单帧，正常主循环消费足够快，可接受。 */
    if (HAL_UART_Receive_DMA(huart, drv->rx_buf, drv->rx_size) != HAL_OK)
    {
        return HAL_ERROR;
    }
    __HAL_UART_ENABLE_IT(huart, UART_IT_IDLE);
    return HAL_OK;
}

void UART_DeInit(UART_Driver_t *drv)
{
    if (drv == NULL || drv->huart == NULL)
        return;
    /* 停止 DMA 循环接收并复位状态 */
    (void)HAL_UART_Abort(drv->huart);
    if (s_active == drv)
        s_active = NULL;   /* 撤销激活实例记录，避免残留 ISR 指向已失效驱动 */
    drv->rx_head = drv->rx_tail = 0U;
    drv->tx_head = drv->tx_tail = 0U;
    drv->tx_sending = 0U;
    drv->tx_ongoing = false;
    drv->frame_ready = false;
}

void UART_IRQHandler(UART_Driver_t *drv)
{
    if (drv == NULL || drv->huart == NULL)
        return;
    UART_HandleTypeDef *huart = drv->huart;
    /* IDLE 线中断：总线静默超过 1 个字符时间后触发，表示"一帧已收完"。
     * 它由我们自己开启，HAL_UART_IRQHandler 不处理它，须在此手动清标志并通知。 */
    if (__HAL_UART_GET_FLAG(huart, UART_FLAG_IDLE))
    {
        __HAL_UART_CLEAR_IDLEFLAG(huart);   /* 读 SR + 读 DR 清 IDLE 标志 */
        drv->frame_ready = true;            /* 帧就绪提示(供 UART_FrameReady 读取) */
        if (drv->frame_cb != NULL)
        {
            drv->frame_cb();                /* 帧就绪回调(唤醒上层任务等) */
        }
    }
    /* 其余(RX=DMA 搬运完成/半完成、TX=Transmit_IT→TxCpltCallback、错误)交给 HAL */
    HAL_UART_IRQHandler(huart);
}

/* ===========================================================================
 * HAL 回调（覆盖 HAL 的 __weak 默认实现）
 *   仅处理本驱动激活的 UART；其余 UART(如有)不受影响。
 * ========================================================================= */
/* 错误处理回调：STM32 HAL 在出现 ORE/PE/FE/NE 等串口错误时，
 * 默认只置错误标志、关闭 RX 并调用本弱函数（默认空实现），
 * 若不在此恢复，DMA 循环接收会在首次出错后永久停摆——表现就是"板子能发心跳、
 * 但串口助手发的命令收不到任何回显/响应"（这正是本次"点了发送没反应"的根因）。
 * 这里清错误标志并重新启动 DMA 循环接收 + IDLE 中断，使链路在噪声/溢出后能自愈。 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  UART_Driver_t *drv = s_active;
  if (drv == NULL || huart != drv->huart)
    return;
  drv->err_count++;
  /* 先读 SR 再读 DR，清除 ORE（该操作同时清掉 FE/NE/PE 标志） */
  __HAL_UART_CLEAR_OREFLAG(huart);
  huart->ErrorCode = HAL_UART_ERROR_NONE;
  /* HAL 出错后可能仍置 BUSY_RX 且不使能 DMA 接收，强制回 READY 再重新启动，
   * 否则 HAL_UART_Receive_DMA 会返回 BUSY、DMA 不被重新打开。 */
  if (huart->RxState != HAL_UART_STATE_READY)
  {
    huart->RxState = HAL_UART_STATE_READY;
  }
  (void)HAL_UART_Receive_DMA(huart, drv->rx_buf, drv->rx_size);
  __HAL_UART_ENABLE_IT(huart, UART_IT_IDLE);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    UART_Driver_t *drv = s_active;
    if (drv == NULL || huart != drv->huart)
        return;
    /* 推进发送尾指针（已发出的字节数 = tx_sending），若环形缓冲还有数据
     * 则启动下一帧 HAL_UART_Transmit_IT，否则标记发送结束。 */
    drv->tx_tail = (uint16_t)(drv->tx_tail + drv->tx_sending) % drv->tx_size;
    drv->tx_sending = 0U;
    uint16_t avail = (uint16_t)(drv->tx_head - drv->tx_tail) % drv->tx_size;
    if (avail > 0U)
    {
        uint16_t n = (avail > 64U) ? 64U : avail;
        /* 不跨环形缓冲末尾发送，避免一次 HAL 调用读到越界内存 */
        if (n > (uint16_t)(drv->tx_size - drv->tx_tail))
            n = (uint16_t)(drv->tx_size - drv->tx_tail);
        drv->tx_sending = n;
        (void)HAL_UART_Transmit_IT(drv->huart, &drv->tx_buf[drv->tx_tail], n);
    }
    else
    {
        drv->tx_ongoing = false;
    }
}

uint16_t UART_GetRxCount(const UART_Driver_t *drv)
{
    if (drv == NULL)
        return 0U;
    uint32_t pm = uart_enter_critical();
    uart_sync_rx_head(drv);   /* 同步 DMA 已写入位置 */
    uint16_t c = (uint16_t)(drv->rx_head - drv->rx_tail) % drv->rx_size;
    uart_exit_critical(pm);
    return c;
}

void UART_ClearRx(UART_Driver_t *drv)
{
    if (drv == NULL)
        return;
    uint32_t pm = uart_enter_critical();
    uart_sync_rx_head(drv);   /* 同步到 DMA 当前位置，丢弃此刻之前的数据 */
    drv->rx_tail = drv->rx_head;
    uart_exit_critical(pm);
}

uint16_t UART_ReadRaw(UART_Driver_t *drv, uint8_t *out, uint16_t max_len)
{
    if (drv == NULL || out == NULL)
        return 0U;
    uint32_t pm = uart_enter_critical();
    uart_sync_rx_head(drv);   /* 同步 DMA 已写入位置 */
    uint16_t avail = (uint16_t)(drv->rx_head - drv->rx_tail) % drv->rx_size;
    uint16_t n = (avail < max_len) ? avail : max_len;
    for (uint16_t i = 0U; i < n; i++)
    {
        out[i] = drv->rx_buf[drv->rx_tail];
        drv->rx_tail = (uint16_t)(drv->rx_tail + 1U) % drv->rx_size;
    }
    uart_exit_critical(pm);
    if (n > 0U)
        drv->frame_ready = false;
    return n;
}

uint16_t UART_ReadFrame(UART_Driver_t *drv, uint8_t *out, uint16_t max_len)
{
    if (drv == NULL || out == NULL)
        return 0U;

    uint16_t fs, ft, po, pl;
    uint32_t pm = uart_enter_critical();
    uart_sync_rx_head(drv);   /* 同步 DMA 已写入位置，避免漏读刚到达的新字节 */
    if (!uart_locate_frame(drv, &fs, &ft, &po, &pl))
    {
        uart_exit_critical(pm);
        return 0U;
    }
    uint16_t copy = (pl < max_len) ? pl : max_len;   /* 截断保护 */
    for (uint16_t i = 0U; i < copy; i++)
    {
        out[i] = drv->rx_buf[(uint16_t)(fs + po + i) % drv->rx_size];
    }
    /* 消费整帧：推进 rx_tail 到帧尾 */
    drv->rx_tail = (uint16_t)(fs + ft) % drv->rx_size;
    drv->frame_ready = false;
    uart_exit_critical(pm);
    return copy;
}

bool UART_FrameReady(const UART_Driver_t *drv)
{
    if (drv == NULL)
        return false;
    return drv->frame_ready;
}

HAL_StatusTypeDef UART_Send(UART_Driver_t *drv, const uint8_t *data, uint16_t len)
{
    if (drv == NULL || data == NULL)
        return HAL_ERROR;
    if (len == 0U)
        return HAL_OK;

    uint32_t pm = uart_enter_critical();
    /* 预留 1 个槽位区分满/空，避免覆盖未读数据 */
    uint16_t free = (uint16_t)(drv->tx_size
                  - (drv->tx_head - drv->tx_tail) % drv->tx_size - 1U);
    if (len > free)
    {
        uart_exit_critical(pm);
        return HAL_ERROR;   /* 空间不足，非阻塞返回，调用方稍后重试 */
    }
    for (uint16_t i = 0U; i < len; i++)
    {
        drv->tx_buf[drv->tx_head] = data[i];
        drv->tx_head = (uint16_t)(drv->tx_head + 1U) % drv->tx_size;
    }
    /* 若发送未在进行，启动 HAL 发送中断(之后由 HAL_UART_TxCpltCallback 续发) */
    if (!drv->tx_ongoing)
    {
        drv->tx_ongoing = true;
        uint16_t avail = (uint16_t)(drv->tx_head - drv->tx_tail) % drv->tx_size;
        uint16_t n = (avail > 64U) ? 64U : avail;
        /* 不跨环形缓冲末尾发送，避免一次 HAL 调用读到越界内存 */
        if (n > (uint16_t)(drv->tx_size - drv->tx_tail))
            n = (uint16_t)(drv->tx_size - drv->tx_tail);
        drv->tx_sending = n;
        if (HAL_UART_Transmit_IT(drv->huart, &drv->tx_buf[drv->tx_tail], n) != HAL_OK)
        {
            drv->tx_ongoing = false;
            drv->tx_sending = 0U;
        }
    }
    uart_exit_critical(pm);
    return HAL_OK;
}

HAL_StatusTypeDef UART_SendString(UART_Driver_t *drv, const char *str)
{
    if (str == NULL)
        return HAL_ERROR;
    return UART_Send(drv, (const uint8_t *)str, (uint16_t)strlen(str));
}

uint16_t UART_GetTxFree(const UART_Driver_t *drv)
{
    if (drv == NULL)
        return 0U;
    uint32_t pm = uart_enter_critical();
    uint16_t free = (uint16_t)(drv->tx_size
                  - (drv->tx_head - drv->tx_tail) % drv->tx_size - 1U);
    uart_exit_critical(pm);
    return free;
}

bool UART_TxComplete(const UART_Driver_t *drv)
{
    if (drv == NULL)
        return true;
    uint32_t pm = uart_enter_critical();
    bool done = (drv->tx_head == drv->tx_tail) && (!drv->tx_ongoing);
    uart_exit_critical(pm);
    return done;
}
