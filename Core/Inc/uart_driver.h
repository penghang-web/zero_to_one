/**
  ******************************************************************************
  * @file    uart_driver.h
  * @brief   基于 STM32 HAL 的稳健 UART 驱动（中断 + 环形缓冲 FIFO）
 * @note    设计要点：
 *          - 接收：DMA 循环搬运(直接写入接收环形缓冲) + IDLE(帧空闲) 中断；
 *                    不使用 HAL_UART_Receive_IT 这种"每字节单次接收"模型(已删除)。
  *          - 发送：TXE 中断驱动，数据先压入发送环形缓冲，非阻塞发出。
  *          - 帧解析：支持可选 帧头 / 长度字段 / 帧尾，简单帧提取。
  *          - 错误处理：溢出(ORE)/奇偶(PE)/帧(FE)/噪声(NE) 错误自动恢复。
  *          - 防溢出：缓冲满时丢弃新字节并计数，绝不越界写。
  *          - 防竞争：所有共享索引的读写均关闭中断保护（临界区）。
  ******************************************************************************
  */
#ifndef UART_DRIVER_H
#define UART_DRIVER_H

#include "stm32f1xx_hal.h"
#include "stm32f1xx_hal_uart.h"   /* 确保 UART_FLAG_* / USART_CR1_* 宏可用 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 帧解析配置
 * ----------------------------------------------------------------------------
 * 约定（可在 uart_frame_cfg_t 中自由组合）：
 *   - 帧头(header) ：可选，位于帧最前端，用于同步。
 *   - 长度字段(len)：可选，位于"帧头之后 length_offset 字节处"，
 *                    占用 length_size(1 或 2) 字节。
 *                    长度值 L = 帧头之后、到帧尾之前(若启用帧尾)的【载荷字节数】。
 *                    即：启用长度时，一帧总长 = header_len + length_size + L (+ tail_len)，
 *                    其中 tail(若有) 是这 L 字节末尾的 tail_len 个字节。
 *   - 帧尾(tail)   ：可选，位于帧最末端，用于界定帧结束。
 *   - 三者可任意组合；若全部关闭，则 UART_ReadFrame 直接返回缓冲区内当前所有字节(原始流)。
 * 说明：这是"简单帧解析"，不做转义 / 校验和（可在载荷中自行实现）。
 * ==========================================================================*/
typedef struct {
    const uint8_t *header;     /* 帧头指针；NULL 或 header_len==0 表示不使用 */
    uint8_t        header_len; /* 帧头长度(字节) */
    const uint8_t *tail;       /* 帧尾指针；NULL 或 tail_len==0 表示不使用 */
    uint8_t        tail_len;   /* 帧尾长度(字节) */
    bool           use_length; /* 是否启用长度字段 */
    uint8_t        length_offset; /* 长度字段相对"帧头之后"的偏移(字节) */
    uint8_t        length_size;   /* 长度字段字节数：1 或 2 */
    bool           big_endian;    /* 长度字段大小端：true=大端 */
    uint16_t       max_frame;  /* 单帧最大长度上限(用于溢出保护，0=不限制) */
} uart_frame_cfg_t;

/* 帧就绪回调(可选)：在 IDLE 中断中调用，用于唤醒上层任务 */
typedef void (*uart_frame_cb_t)(void);

/* 驱动运行实例：每个 UART 一个，由用户定义并传入缓冲 */
typedef struct {
    UART_HandleTypeDef *huart;   /* CubeMX 已初始化的句柄 */

    /* 接收环形缓冲(FIFO) */
    uint8_t       *rx_buf;       /* 用户提供的接收缓冲 */
    uint16_t       rx_size;      /* 接收缓冲大小(建议 2 的幂，≥2) */
    volatile uint16_t rx_head;   /* ISR 写：下一个存入位置 */
    volatile uint16_t rx_tail;   /* 应用读：下一个取出位置 */

    /* 发送环形缓冲(FIFO) */
    uint8_t       *tx_buf;       /* 用户提供的发送缓冲 */
    uint16_t       tx_size;      /* 发送缓冲大小(建议 2 的幂，≥2) */
    volatile uint16_t tx_head;   /* 应用写：下一个存入位置 */
    volatile uint16_t tx_tail;   /* ISR 读：下一个取出位置 */
    volatile uint16_t tx_sending;/* 当前 HAL_UART_Transmit_IT 正在发送的字节数 */
    volatile bool   tx_ongoing;  /* 发送进行中(TXE 中断已开启) */

    /* 统计与状态 */
    volatile uint32_t rx_overflow; /* 接收溢出(缓冲满被丢弃)次数 */
    volatile uint32_t err_count;   /* 串口错误(ORE/PE/FE/NE)次数 */
    volatile bool      frame_ready;/* IDLE 检测到帧结束的提示标志 */

    uart_frame_cfg_t frame;      /* 帧解析配置 */
    uart_frame_cb_t  frame_cb;   /* 帧就绪回调(可为 NULL) */
} UART_Driver_t;

/* 初始化配置：传入缓冲与帧参数 */
typedef struct {
    uint8_t  *rx_buf;
    uint16_t  rx_size;
    uint8_t  *tx_buf;
    uint16_t  tx_size;
    uart_frame_cfg_t frame;
    uart_frame_cb_t  frame_cb;
} UART_DriverConfig_t;

/* ============================== 对外 API ============================== */

/**
  * @brief  初始化 UART 驱动：绑定 huart，挂载收发环形缓冲，开启持续接收。
  * @note   huart 须已由 CubeMX(MX_USARTx_UART_Init) 初始化完成。
 *         接收采用 HAL_UART_Receive_DMA 循环搬运，DMA 直接把串口数据写入接收环形
 *         缓冲；IDLE 线中断触发"帧收完"通知(frame_cb / frame_ready)。无需每字节
 *         中断、无需手动重武装，CPU 占用最低(连续、非阻塞、零拷贝)。
  *         发送采用 HAL_UART_Transmit_IT + 发送环形缓冲(非阻塞)。
  *         本驱动的 UART_IRQHandler 作为薄包装转调 HAL_UART_IRQHandler，
  *         由其统一处理 RX/TX/错误。
  * @retval HAL_OK 成功；参数非法返回 HAL_ERROR。
  */
HAL_StatusTypeDef UART_Init(UART_Driver_t *drv, UART_HandleTypeDef *huart,
                             const UART_DriverConfig_t *cfg);

/**
  * @brief  反初始化：关闭 RX/TX 中断，复位索引与状态(不释放用户缓冲)。
  */
void UART_DeInit(UART_Driver_t *drv);

/**
  * @brief  ★ 中断入口：在 stm32f1xx_it.c 的 USARTx_IRQHandler 内调用本函数。
 * @note   本函数先处理 IDLE 线中断(自己开启，HAL 不处理)：清 IDLE 标志并调用 frame_cb；
 *         其余(RX=DMA 搬运完成/半完成 / TX=Transmit_IT→TxCpltCallback / 错误)转交 HAL_UART_IRQHandler(huart)。
  *         务必调用 UART_IRQHandler(&g_uartX) 而非直接 HAL_UART_IRQHandler。
  */
void UART_IRQHandler(UART_Driver_t *drv);

/* ----------------------------- 接收 API ----------------------------- */

/** @brief 返回接收缓冲中未读字节数(临界区快照，线程安全)。 */
uint16_t UART_GetRxCount(const UART_Driver_t *drv);

/** @brief 清空接收缓冲(丢弃所有未读数据)。 */
void UART_ClearRx(UART_Driver_t *drv);

/**
  * @brief  解析并读取"一帧完整数据"的【载荷】到 out，返回载荷字节数。
  * @note   无完整帧时返回 0 且不作任何消费；解析依据 drv->frame 配置，
  *         自动处理环形缓冲回绕；若载荷超过 max_len 则截断到 max_len(仍消费整帧)。
  */
uint16_t UART_ReadFrame(UART_Driver_t *drv, uint8_t *out, uint16_t max_len);

/**
  * @brief  非帧模式：直接读取最多 max_len 个原始字节，返回实际读取数。
  * @note   用于终端回显 / 命令行等不定长文本场景。
  */
uint16_t UART_ReadRaw(UART_Driver_t *drv, uint8_t *out, uint16_t max_len);

/** @brief  IDLE 提示标志(只读)；读到帧或清缓冲后由驱动内部清零。 */
bool UART_FrameReady(const UART_Driver_t *drv);

/* ----------------------------- 发送 API ----------------------------- */

/**
  * @brief  非阻塞发送字节数组：压入发送环形缓冲，由 TXE 中断陆续发出。
  * @retval HAL_OK 已全部入队；缓冲空间不足返回 HAL_ERROR(调用方可稍后重试)，不阻塞。
  */
HAL_StatusTypeDef UART_Send(UART_Driver_t *drv, const uint8_t *data, uint16_t len);

/** @brief 非阻塞发送以 '\0' 结尾的字符串。 */
HAL_StatusTypeDef UART_SendString(UART_Driver_t *drv, const char *str);

/** @brief 返回发送缓冲剩余空闲字节数。 */
uint16_t UART_GetTxFree(const UART_Driver_t *drv);

/** @brief 所有数据是否均已从缓冲送出(不含移位寄存器中的最后 1 字节)。 */
bool UART_TxComplete(const UART_Driver_t *drv);

#ifdef __cplusplus
}
#endif

#endif /* UART_DRIVER_H */
