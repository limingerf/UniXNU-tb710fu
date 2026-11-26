/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* GENI UART transport for the TB710FU Bluetooth HSUART (QUP wrapper 2, serial engine 6).
 *
 * Register offsets and field encodings are from the upstream QUP v3 sources, not invented here:
 *   include/linux/soc/qcom/geni-se.h          common SE registers, FIFO status fields, IRQ bits,
 *                                             SER_M_CLK_CFG encoding, HW_PARAM layout
 *   drivers/tty/serial/qcom_geni_serial.c     UART-specific block (0x22c..0x2ac), M_CMD/S_CMD opcodes,
 *                                             oversampling (32, halved to 16 for QUP >= 2.5),
 *                                             clk_div = ceil(rate / (baud * sampling)), termios writes
 *   drivers/soc/qcom/qcom-geni-se.c           geni_se_init/geni_se_config_packing/geni_se_select_mode
 *                                             order and values
 * Three of those values are also used by this repository's own GENI I2C driver (GeniI2c.h: SER_M_CLK_CFG
 * 0x48, M_CMD0 0x600 with the opcode at bit 27, FIFO ports at 0x700/0x780, status at 0x800/0x804, HW_PARAM
 * at 0xe24/0xe28), which is how the same SE controller is programmed on this SoC class already.
 *
 * Everything is FIFO/PIO: no SE DMA, no GSI, no GPI. That is the mode upstream uses for a UART console and
 * the only mode that needs no iommu work, so it is also the mode with the least to get wrong at boot. The
 * register window is reached only through 32-bit accesses supplied by the caller (Tb710fuBluetoothGeniOps),
 * which is what keeps memcpy/bzero off MMIO: this file never touches memory that maps a device.
 */
#ifndef TB710FU_BLUETOOTH_GENI_H
#define TB710FU_BLUETOOTH_GENI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Common SE registers (geni-se.h). */
#define TB_GENI_FORCE_DEFAULT 0x020u
#define TB_GENI_OUTPUT_CTRL 0x024u
#define TB_GENI_STATUS 0x040u
#define TB_GENI_SER_M_CLK_CFG 0x048u
#define TB_GENI_SER_S_CLK_CFG 0x04cu
#define TB_GENI_CLK_CTRL_RO 0x060u
#define TB_GENI_IF_DISABLE_RO 0x064u
#define TB_GENI_FW_REVISION_RO 0x068u
#define TB_GENI_CLK_SEL 0x07cu
#define TB_GENI_CGC_CTRL 0x028u
#define TB_GENI_DMA_MODE_EN 0x258u
#define TB_GENI_M_CMD0 0x600u
#define TB_GENI_M_CMD_CTRL 0x604u
#define TB_GENI_M_IRQ_STATUS 0x610u
#define TB_GENI_M_IRQ_EN 0x614u
#define TB_GENI_M_IRQ_CLEAR 0x618u
#define TB_GENI_S_CMD0 0x630u
#define TB_GENI_S_CMD_CTRL 0x634u
#define TB_GENI_S_IRQ_STATUS 0x640u
#define TB_GENI_S_IRQ_EN 0x644u
#define TB_GENI_S_IRQ_CLEAR 0x648u
#define TB_GENI_TX_FIFO 0x700u
#define TB_GENI_RX_FIFO 0x780u
#define TB_GENI_TX_FIFO_STATUS 0x800u
#define TB_GENI_RX_FIFO_STATUS 0x804u
#define TB_GENI_TX_WATERMARK 0x80cu
#define TB_GENI_RX_WATERMARK 0x810u
#define TB_GENI_RX_RFR_WATERMARK 0x814u
#define TB_GENI_IOS 0x908u
#define TB_GENI_M_GP_LENGTH 0x910u
#define TB_GENI_GSI_EVENT_EN 0xe18u
#define TB_GENI_SE_IRQ_EN 0xe1cu
#define TB_GENI_HW_PARAM_0 0xe24u
#define TB_GENI_HW_PARAM_1 0xe28u
#define TB_GENI_HW_PARAM_2 0xe2cu
#define TB_GENI_DMA_GENERAL_CFG 0xe30u

/* UART protocol block (qcom_geni_serial.c). */
#define TB_UART_LOOPBACK_CFG 0x22cu
#define TB_UART_IO_MACRO_CTRL 0x240u
#define TB_UART_TX_TRANS_CFG 0x25cu
#define TB_UART_TX_WORD_LEN 0x268u
#define TB_UART_TX_STOP_BIT_LEN 0x26cu
#define TB_UART_TX_TRANS_LEN 0x270u
#define TB_UART_RX_TRANS_CFG 0x280u
#define TB_UART_RX_WORD_LEN 0x28cu
#define TB_UART_RX_STALE_CNT 0x294u
#define TB_UART_TX_PARITY_CFG 0x2a4u
#define TB_UART_RX_PARITY_CFG 0x2a8u
#define TB_UART_MANUAL_RFR 0x2acu
#define TB_GENI_BYTE_GRAN 0x254u
#define TB_GENI_TX_PACK_0 0x260u
#define TB_GENI_TX_PACK_1 0x264u
#define TB_GENI_RX_PACK_0 0x284u
#define TB_GENI_RX_PACK_1 0x288u

/* Fields (geni-se.h / qcom_geni_serial.c). */
#define TB_SER_CLK_EN (1u << 0)
#define TB_CLK_DIV_SHIFT 4u
#define TB_CLK_DIV_MAX 0xfffu /* CLK_DIV_MSK >> CLK_DIV_SHFT */
#define TB_CLK_SEL_MASK 0x7u
#define TB_FW_REV_PROTOCOL_SHIFT 8u
#define TB_FW_REV_PROTOCOL_MASK 0xff00u
#define TB_FW_REV_PROTOCOL_UART 2u     /* enum geni_se_protocol_type GENI_SE_UART */
#define TB_FW_REV_PROTOCOL_INVALID 255u
#define TB_M_OPCODE_SHIFT 27u
#define TB_UART_START_TX 0x1u
#define TB_UART_START_READ 0x1u
#define TB_S_PARAM_RFR_OPEN (1u << 7)
#define TB_M_GENI_CMD_ACTIVE (1u << 0)
#define TB_S_GENI_CMD_ACTIVE (1u << 12)
#define TB_M_GENI_CMD_CANCEL (1u << 2)
#define TB_M_GENI_CMD_ABORT (1u << 1)
#define TB_S_GENI_CMD_CANCEL (1u << 2)
#define TB_S_GENI_CMD_ABORT (1u << 1)
#define TB_M_CMD_DONE (1u << 0)
#define TB_M_CMD_FAILURE (1u << 3)
#define TB_M_CMD_CANCEL (1u << 4)
#define TB_M_CMD_ABORT (1u << 5)
#define TB_M_RX_FIFO_WATERMARK (1u << 26)
#define TB_M_RX_FIFO_LAST (1u << 27)
#define TB_M_TX_FIFO_WATERMARK (1u << 30)
#define TB_S_CMD_DONE (1u << 0)
#define TB_S_CMD_CANCEL (1u << 4)
#define TB_S_CMD_ABORT (1u << 5)
#define TB_S_RX_FIFO_WATERMARK (1u << 26)
#define TB_S_RX_FIFO_LAST (1u << 27)
#define TB_S_RX_FIFO_WR_ERR (1u << 25)
#define TB_S_GP_IRQ_0 (1u << 9)
#define TB_S_GP_IRQ_1 (1u << 10)
#define TB_S_GP_IRQ_2 (1u << 11)
#define TB_S_GP_IRQ_3 (1u << 12)
#define TB_TX_FIFO_WC_MASK 0x0fffffffu
#define TB_RX_FIFO_WC_MASK 0x01ffffffu
#define TB_RX_LAST (1u << 31)
#define TB_RX_LAST_BYTE_SHIFT 28u
#define TB_RX_LAST_BYTE_MASK 0x70000000u
#define TB_UART_CTS_MASK (1u << 1) /* TX_TRANS_CFG: mask CTS = no hardware flow control */
#define TB_UART_MANUAL_RFR_EN (1u << 31)
#define TB_UART_RFR_NOT_READY (1u << 1)
#define TB_TX_STOP_BIT_LEN_1 0u
#define TB_WATERMARK_MASK 0x3fu
#define TB_IO2_DATA_IN (1u << 1) /* SE_GENI_IOS: CTS pin level */
#define TB_FIFO_IF_DISABLE (1u << 0)
#define TB_QUP_HW_VER_SHIFT 16u
#define TB_QUP_HW_VER_MINOR_SHIFT 8u
#define TB_QUP_SE_VERSION_2_5 0x0002u /* major 2, minor 5: sampling rate 16, not 32 */
#define TB_UART_OVERSAMPLING 16u     /* UART_OVERSAMPLING / 2 for QUP >= 2.5 */
#define TB_BYTES_PER_FIFO_WORD 4u
#define TB_UART_BITS_PER_CHAR 10u    /* 8 data + start + stop; tty_get_char_size(CS8, 1 stop) */
#define TB_UART_RX_STALE 160u        /* DEFAULT_BITS_PER_CHAR * STALE_TIMEOUT (10 * 16) */
#define TB_UART_RX_WM 2u
#define TB_UART_TX_WM 2u
#define TB_GENI_DEFAULT_OUTPUT_CTRL 0x7fu /* DEFAULT_IO_OUTPUT_CTRL_MSK */

/* Bounded waits. A whole HCI packet at 115200 baud takes 26 ms for 300 bytes; flow control can hold it
 * longer. Nothing here runs in an interrupt context, so a generous deadline is the right trade: a stalled
 * controller should fail the stage, not wedge the thread forever. */
#define TB_BT_TX_TIMEOUT_NS 2000000000ull
#define TB_BT_CMD_TIMEOUT_NS 100000000ull
#define TB_BT_RX_START_TIMEOUT_NS 20000000ull

typedef struct {
    uint32_t (*read32)(void *ctx, uint32_t offset);
    void (*write32)(void *ctx, uint32_t offset, uint32_t value);
    void (*barrier)(void *ctx);
    void (*delay_us)(void *ctx, uint32_t us);
    uint64_t (*now_ns)(void *ctx);
    void (*note)(void *ctx, const char *what, uint32_t a, uint32_t b); /* optional evidence line */
    void *ctx;
} TbBtGeniOps;

typedef struct {
    const TbBtGeniOps *ops;
    uint32_t hw_version; /* QUP wrapper version, as read by the caller (may be 0 if unreadable) */
    uint32_t proto;
    uint32_t tx_depth_words;
    uint32_t tx_width_bits;
    uint32_t rx_depth_words;
    uint32_t baud;
    uint32_t clk_rate;
    uint32_t clk_div;
    bool rx_open;
    bool flow;
    bool configured;
    uint32_t tx_packets;
    uint32_t rx_words;
    uint32_t errors;
    uint32_t last_error;
} TbBtGeni;

enum {
    TB_BT_GENI_OK = 0,
    TB_BT_GENI_ERROR = -1,      /* SE answered but not with what the driver needs */
    TB_BT_GENI_TIMEOUT = -2,
    TB_BT_GENI_OVERFLOW = -3,   /* caller's buffer too small for the bytes in the FIFO */
    TB_BT_GENI_ARGUMENT = -4,
};

/* Reads the SE identification registers only; no write reaches the hardware, so this is the bt=1 probe. */
int tb_bt_geni_probe(TbBtGeni *geni, const TbBtGeniOps *ops);
/* Cancels any stale command, checks the UART firmware, and programs FIFO mode at geni->baud (the caller
 * sets geni->baud / geni->clk_rate / geni->clk_div before the call; the clock itself is TB710FUBluetoothClock). */
int tb_bt_geni_config(TbBtGeni *geni);
/* Rewrites the two SER clock dividers and the SE clock select for a new rate. RX is stopped first and
 * restarted after, as upstream does around a baud change. */
int tb_bt_geni_set_rate(TbBtGeni *geni, uint32_t baud, uint32_t rate, uint32_t clk_div);
int tb_bt_geni_start_rx(TbBtGeni *geni);
int tb_bt_geni_stop_rx(TbBtGeni *geni);
/* Hardware flow control: clear CTS_MASK so the engine honours CTS, and release RTS; or mask CTS and hold
 * RTS not-ready, the sequence hci_qca uses around a baud change. */
int tb_bt_geni_set_flow(TbBtGeni *geni, bool enabled);
bool tb_bt_geni_cts(TbBtGeni *geni);
/* One whole H4 packet of len bytes; returns bytes queued (len) or a TB_BT_GENI_* code. */
int tb_bt_geni_write(TbBtGeni *geni, const uint8_t *bytes, uint32_t len);
/* Drains the RX FIFO into out, up to cap bytes; returns 0 and sets *got, or TB_BT_GENI_OVERFLOW with *got
 * set to cap when the FIFO held more (the excess is discarded so the stream stays in step). */
int tb_bt_geni_read(TbBtGeni *geni, uint8_t *out, uint32_t cap, uint32_t *got);
/* Clears the latched M/S status words and returns the bits that were set (for the poll thread's counters). */
uint32_t tb_bt_geni_service(TbBtGeni *geni);
/* The bytes still queued in the TX FIFO, for a drain check before a reset or a baud change. */
uint32_t tb_bt_geni_tx_words(TbBtGeni *geni);

#ifdef __cplusplus
}
#endif
#endif /* TB710FU_BLUETOOTH_GENI_H */
