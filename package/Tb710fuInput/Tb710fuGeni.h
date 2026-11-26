/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* GENI SPI engine for the TB710FU touch controller.
 *
 * The register offsets are QUPv3 GENI architectural offsets (Linux
 * drivers/spi/spi-geni-qcom.c, geni_se.c register map), identical for every
 * SE; the SE instance itself is `spi@a90000` from sm8650.dtsi (see
 * Tb710fuInputHardware.h). The port keeps the reference state machine but
 * removes everything the pinned hardware cannot support:
 *
 *   - no FIFO availability read at probe time. SE_GENI_IF_DISABLE (0x64) is
 *     documented to abort on this controller generation, so reading it is an
 *     explicit, boot-arg gated diagnosis (options->fuse_probe) instead of part
 *     of bring-up. FIFO health is proven by the first transfer, which fails
 *     with a bounded timeout and never with a machine check.
 *   - no GPI DMA. This transport is polled FIFO only; the DMA-capable
 *     alternative is described in docs/INPUT_TOUCH_KEYS.md, not stubbed here.
 *   - completion is polled. The IRQ path exists for completeness but is off
 *     unless tb710fu-touch-irq=1: an SE interrupt line the firmware still owns
 *     is a boot-regression risk.
 */
#ifndef TB710FU_GENI_H
#define TB710FU_GENI_H

#include <stdint.h>
#include <stddef.h>

/* ---- SE register map (QUPv3 GENI) ---- */
#define TB710FU_GENI_FORCE_DEFAULT        0x20u
#define TB710FU_GENI_OUTPUT_CTRL          0x24u
#define TB710FU_GENI_CGC_CTRL             0x28u
#define TB710FU_GENI_STATUS               0x40u
#define TB710FU_GENI_SER_M_CLK_CFG        0x48u
#define TB710FU_GENI_SER_S_CLK_CFG        0x4Cu
#define TB710FU_GENI_IF_DISABLE_RO        0x64u
#define TB710FU_GENI_FW_REVISION_RO       0x68u
#define TB710FU_GENI_CLK_SEL              0x7Cu
#define TB710FU_GENI_GSI_EVENT_EN         0xE18u
#define TB710FU_GENI_IRQ_EN               0xE1Cu
#define TB710FU_GENI_HW_PARAM_0           0xE24u
#define TB710FU_GENI_HW_PARAM_1           0xE28u
#define TB710FU_GENI_DMA_GENERAL_CFG      0xE30u
#define TB710FU_GENI_SPI_CPHA             0x224u
#define TB710FU_GENI_SPI_LOOPBACK         0x22Cu
#define TB710FU_GENI_SPI_CPOL             0x230u
#define TB710FU_GENI_SPI_DEMUX_OUTPUT_INV 0x24Cu
#define TB710FU_GENI_SPI_DEMUX_SEL        0x250u
#define TB710FU_GENI_BYTE_GRAN            0x254u
#define TB710FU_GENI_DMA_MODE_EN          0x258u
#define TB710FU_GENI_SPI_TRANS_CFG        0x25Cu
#define TB710FU_GENI_TX_PACKING_CFG0      0x260u
#define TB710FU_GENI_TX_PACKING_CFG1      0x264u
#define TB710FU_GENI_SPI_WORD_LEN         0x268u
#define TB710FU_GENI_SPI_TX_TRANS_LEN     0x26Cu
#define TB710FU_GENI_SPI_RX_TRANS_LEN     0x270u
#define TB710FU_GENI_RX_PACKING_CFG0      0x284u
#define TB710FU_GENI_RX_PACKING_CFG1      0x288u
#define TB710FU_GENI_M_CMD0               0x600u
#define TB710FU_GENI_M_CMD_CTRL           0x604u
#define TB710FU_GENI_M_IRQ_STATUS         0x610u
#define TB710FU_GENI_M_IRQ_EN             0x614u
#define TB710FU_GENI_M_IRQ_CLEAR          0x618u
#define TB710FU_GENI_S_IRQ_STATUS         0x640u
#define TB710FU_GENI_S_IRQ_EN             0x644u
#define TB710FU_GENI_S_IRQ_CLEAR          0x648u
#define TB710FU_GENI_TX_FIFO              0x700u
#define TB710FU_GENI_RX_FIFO              0x780u
#define TB710FU_GENI_TX_FIFO_STATUS       0x800u
#define TB710FU_GENI_RX_FIFO_STATUS       0x804u
#define TB710FU_GENI_TX_WATERMARK         0x80Cu
#define TB710FU_GENI_RX_WATERMARK         0x810u
#define TB710FU_GENI_RX_RFR_WATERMARK     0x814u

/* QUP wrapper window: HW version register, sm8650.dtsi geniqup@ac0000 + 0x4. */
#define TB710FU_QUP_HW_VER                0x4u

/* ---- field values ---- */
#define TB710FU_GENI_PROTO_SPI            1u
#define TB710FU_GENI_FW_PROTO_SHIFT       8u
#define TB710FU_GENI_FORCE_DEFAULT_BIT    (1u << 0)
#define TB710FU_GENI_SER_CLK_EN           (1u << 0)
#define TB710FU_GENI_CLK_DIV_SHIFT        4u
#define TB710FU_GENI_CLK_DIV_MAX          0xFFFu
#define TB710FU_GENI_FIFO_IF_DISABLE      (1u << 0)
#define TB710FU_GENI_DMA_MODE_ENABLE      (1u << 0)
#define TB710FU_GENI_DEFAULT_CGC          0x7Fu
#define TB710FU_GENI_DEFAULT_OUTPUT_CTRL  0x7Fu
#define TB710FU_GENI_STATUS_M_ACTIVE      (1u << 0)
#define TB710FU_GENI_M_OPCODE_SHIFT       27u
#define TB710FU_GENI_M_PARAM_FRAGMENTATION (1u << 2)
#define TB710FU_GENI_M_CMD_CANCEL         (1u << 2)
#define TB710FU_GENI_M_CMD_ABORT          (1u << 1)
#define TB710FU_GENI_M_CMD_TX_ONLY        1u
#define TB710FU_GENI_M_CMD_RX_ONLY        2u
#define TB710FU_GENI_M_CMD_TX_RX          3u
#define TB710FU_GENI_TRANS_LEN_MASK       0xFFFFFFu
#define TB710FU_GENI_WORD_LEN_MIN         4u
#define TB710FU_GENI_CPHA_BIT             (1u << 0)
#define TB710FU_GENI_CPOL_BIT             (1u << 2)
#define TB710FU_GENI_CS_TOGGLE            (1u << 1)
#define TB710FU_GENI_LOOPBACK_ENABLE      1u
#define TB710FU_GENI_RX_LAST              (1u << 31)
#define TB710FU_GENI_RX_LAST_VALID_SHIFT  28u
#define TB710FU_GENI_RX_LAST_VALID_MASK   7u
#define TB710FU_GENI_RX_WC_MASK           0x1FFFFFFu
#define TB710FU_GENI_TX_FIFO_WC_MASK      0x1FFFFFFu

#define TB710FU_GENI_IRQ_DMA_RX           (1u << 0)
#define TB710FU_GENI_IRQ_DMA_TX           (1u << 1)
#define TB710FU_GENI_IRQ_M                (1u << 2)
#define TB710FU_GENI_IRQ_S                (1u << 3)

#define TB710FU_GENI_M_CMD_DONE           (1u << 0)
#define TB710FU_GENI_M_CMD_OVERRUN        (1u << 1)
#define TB710FU_GENI_M_ILLEGAL_CMD        (1u << 2)
#define TB710FU_GENI_M_CMD_FAILURE        (1u << 3)
#define TB710FU_GENI_M_CMD_CANCEL_DONE     (1u << 4)
#define TB710FU_GENI_M_CMD_ABORT_DONE     (1u << 5)
#define TB710FU_GENI_M_RX_FIFO_RD_ERR     (1u << 24)
#define TB710FU_GENI_M_RX_FIFO_WR_ERR     (1u << 25)
#define TB710FU_GENI_M_RX_FIFO_WATERMARK  (1u << 26)
#define TB710FU_GENI_M_RX_FIFO_LAST       (1u << 27)
#define TB710FU_GENI_M_TX_FIFO_RD_ERR     (1u << 28)
#define TB710FU_GENI_M_TX_FIFO_WR_ERR     (1u << 29)
#define TB710FU_GENI_M_TX_FIFO_WATERMARK  (1u << 30)
#define TB710FU_GENI_M_ERRORS                                                  \
    (TB710FU_GENI_M_CMD_OVERRUN | TB710FU_GENI_M_ILLEGAL_CMD |                 \
     TB710FU_GENI_M_CMD_FAILURE | TB710FU_GENI_M_RX_FIFO_RD_ERR |              \
     TB710FU_GENI_M_RX_FIFO_WR_ERR | TB710FU_GENI_M_TX_FIFO_RD_ERR |            \
     TB710FU_GENI_M_TX_FIFO_WR_ERR)

/* Packing vectors: 4 x 10-bit descriptors per pair register. */
#define TB710FU_GENI_PACK_START_SHIFT     5u
#define TB710FU_GENI_PACK_DIR_SHIFT       4u
#define TB710FU_GENI_PACK_LEN_SHIFT       1u
#define TB710FU_GENI_PACK_STOP            (1u << 0)
#define TB710FU_GENI_PACK_VECTOR_SHIFT    10u
#define TB710FU_GENI_PACK_VECTORS         4u

/* SPI modes accepted by tb710fu_geni_setup. */
#define TB710FU_SPI_MODE_CPHA      0x01u
#define TB710FU_SPI_MODE_CPOL      0x02u
#define TB710FU_SPI_MODE_CS_HIGH   0x04u
#define TB710FU_SPI_MODE_LSB_FIRST 0x08u
#define TB710FU_SPI_MODE_LOOP      0x20u

/* ---- status codes (Linux errno values, as the reference uses) ---- */
#define TB710FU_GENI_OK            0
#define TB710FU_GENI_ERR_PARAM     (-22)
#define TB710FU_GENI_ERR_PROTO     (-71)
#define TB710FU_GENI_ERR_NODEV     (-19)
#define TB710FU_GENI_ERR_TIMEOUT   (-110)
#define TB710FU_GENI_ERR_IO        (-5)
#define TB710FU_GENI_ERR_NOTSUP    (-95)
#define TB710FU_GENI_ERR_STATE     (-75)

enum tb710fu_geni_state {
    TB710FU_GENI_RESET = 0,
    TB710FU_GENI_CONFIG = 1,
    TB710FU_GENI_IDLE = 2,
    TB710FU_GENI_ACTIVE = 3,
    TB710FU_GENI_ERROR = 4,
};

/* Register access. `read`/`write` address the SE window (TB710FU_SE4_BASE);
 * `read_wrapper` addresses the QUP wrapper (TB710FU_QUP_WRAP1_BASE). All
 * callbacks receive aligned 32-bit offsets, so the probe can serve them with
 * tb710fu_mmio_read32/write32 without further arithmetic. */
struct tb710fu_geni_io {
    uint32_t (*read)(void *ctx, uint32_t offset);
    void (*write)(void *ctx, uint32_t offset, uint32_t value);
    uint32_t (*read_wrapper)(void *ctx, uint32_t offset);
    void (*delay_us)(void *ctx, unsigned microseconds);
    void *ctx;
};

struct tb710fu_geni_info {
    uint32_t fw_revision;
    uint32_t qup_hw_version;
    uint32_t geni_status;
    uint32_t hw_param0;
    uint32_t if_disable;      /* valid only after an explicit fuse probe */
    unsigned protocol;
    unsigned tx_fifo_depth;
    unsigned fifo_width_bits;
    int fifo_disabled;        /* set only by the explicit fuse probe */
    int fuse_probed;
};

struct tb710fu_geni_spi {
    struct tb710fu_geni_io io;
    struct tb710fu_geni_info info;
    enum tb710fu_geni_state state;
    unsigned oversampling;
    unsigned tx_watermark;
    unsigned poll_limit;
    unsigned cur_bpw;
    unsigned cur_speed_hz;
    unsigned last_mode;
    int last_cs;
    int poll_only;
    int last_result;

    const uint8_t *xfer_tx;
    uint8_t *xfer_rx;
    size_t xfer_len;
    size_t xfer_tx_left;
    size_t xfer_rx_left;
    int xfer_keep_cs;

    /* last failure snapshot, kept for the boot log */
    uint32_t snap_irq;
    uint32_t snap_status;
    uint32_t snap_tx_fifo;
    uint32_t snap_rx_fifo;
    uint32_t snap_cmd0;
    int snap_valid;
};

const char *tb710fu_geni_state_name(enum tb710fu_geni_state state);

/* Identity read. Reads FW_REVISION, STATUS, HW_PARAM_0 and the QUP wrapper
 * version. SE_GENI_IF_DISABLE is read only when `probe_fuse` is non-zero. */
int tb710fu_geni_read_info(struct tb710fu_geni_spi *spi, int probe_fuse);

/* Bring the engine to IDLE. Refuses when the SE does not answer as a GENI SPI
 * engine (protocol mismatch) or when the explicit fuse probe reported a
 * disabled FIFO interface. */
int tb710fu_geni_init(struct tb710fu_geni_spi *spi);

int tb710fu_geni_setup(struct tb710fu_geni_spi *spi, unsigned mode, unsigned chip_select);

/* Program the SPI clock divider from a known parent rate. Returns
 * TB710FU_GENI_ERR_PARAM when the divider cannot represent the request. */
int tb710fu_geni_set_clock(struct tb710fu_geni_spi *spi, unsigned speed_hz,
                           unsigned source_hz, unsigned clock_source);

/* One polled FIFO transfer. `tx` and `rx` may each be NULL but not both; the
 * length is the SPI transaction length. keep_cs keeps the chip select
 * asserted between fragments. */
int tb710fu_geni_transfer(struct tb710fu_geni_spi *spi, const uint8_t *tx,
                          uint8_t *rx, size_t len, int keep_cs);

/* Cancel/abort recovery after a failed transfer. */
int tb710fu_geni_recover(struct tb710fu_geni_spi *spi);

#endif /* TB710FU_GENI_H */
