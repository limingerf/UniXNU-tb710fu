/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* See Tb710fuGeni.h for the register provenance and the removed paths. */

#include "Tb710fuGeni.h"

#include <string.h>

static uint32_t geni_read(struct tb710fu_geni_spi *spi, uint32_t offset)
{
    return spi->io.read(spi->io.ctx, offset);
}

static void geni_write(struct tb710fu_geni_spi *spi, uint32_t offset, uint32_t value)
{
    spi->io.write(spi->io.ctx, offset, value);
}

static void geni_set_bits(struct tb710fu_geni_spi *spi, uint32_t offset, uint32_t bits)
{
    uint32_t value = geni_read(spi, offset);

    geni_write(spi, offset, value | bits);
}

static void geni_irq_clear_all(struct tb710fu_geni_spi *spi)
{
    geni_write(spi, TB710FU_GENI_GSI_EVENT_EN, 0u);
    geni_write(spi, TB710FU_GENI_M_IRQ_CLEAR, 0xFFFFFFFFu);
    geni_write(spi, TB710FU_GENI_S_IRQ_CLEAR, 0xFFFFFFFFu);
    geni_write(spi, TB710FU_GENI_IRQ_EN, 0xFFFFFFFFu);
}

static unsigned geni_bytes_per_word(const struct tb710fu_geni_spi *spi)
{
    return spi->info.fifo_width_bits / 8u;
}

const char *tb710fu_geni_state_name(enum tb710fu_geni_state state)
{
    switch (state) {
    case TB710FU_GENI_RESET:
        return "reset";
    case TB710FU_GENI_CONFIG:
        return "config";
    case TB710FU_GENI_IDLE:
        return "idle";
    case TB710FU_GENI_ACTIVE:
        return "active";
    case TB710FU_GENI_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

/* 8-bit words packed four to a FIFO word, MSB-first inside each byte. */
static void geni_config_packing(struct tb710fu_geni_spi *spi, unsigned bits_per_word,
                                unsigned pack_words, int msb_first)
{
    uint32_t cfg[TB710FU_GENI_PACK_VECTORS] = {0u, 0u, 0u, 0u};
    int index = msb_first ? (int)bits_per_word - 1 : 0;
    const int start = index;
    const int delta = msb_first ? -8 : 8;
    const unsigned ceil_bpw = (bits_per_word + 7u) & ~7u;
    const unsigned iterations = ceil_bpw * pack_words / 8u;
    int remaining = (int)bits_per_word;
    unsigned i;

    if (iterations == 0u || iterations > TB710FU_GENI_PACK_VECTORS)
        return;
    for (i = 0u; i < iterations; ++i) {
        const int length = (remaining < 8 ? remaining : 8) - 1;

        cfg[i] = ((uint32_t)index << TB710FU_GENI_PACK_START_SHIFT) |
                 ((uint32_t)(msb_first != 0) << TB710FU_GENI_PACK_DIR_SHIFT) |
                 ((uint32_t)length << TB710FU_GENI_PACK_LEN_SHIFT);
        if (remaining <= 8) {
            index = (int)(i + 1u) * 8 + start;
            remaining = (int)bits_per_word;
        } else {
            index += delta;
            remaining -= 8;
        }
    }
    cfg[iterations - 1u] |= TB710FU_GENI_PACK_STOP;
    geni_write(spi, TB710FU_GENI_TX_PACKING_CFG0,
               cfg[0] | (cfg[1] << TB710FU_GENI_PACK_VECTOR_SHIFT));
    geni_write(spi, TB710FU_GENI_TX_PACKING_CFG1,
               cfg[2] | (cfg[3] << TB710FU_GENI_PACK_VECTOR_SHIFT));
    geni_write(spi, TB710FU_GENI_RX_PACKING_CFG0,
               cfg[0] | (cfg[1] << TB710FU_GENI_PACK_VECTOR_SHIFT));
    geni_write(spi, TB710FU_GENI_RX_PACKING_CFG1,
               cfg[2] | (cfg[3] << TB710FU_GENI_PACK_VECTOR_SHIFT));
    if (pack_words != 0u || bits_per_word == 32u)
        geni_write(spi, TB710FU_GENI_BYTE_GRAN, bits_per_word / 16u);
}

static void geni_set_word_len(struct tb710fu_geni_spi *spi, unsigned bits_per_word,
                              int msb_first)
{
    unsigned pack;

    if (bits_per_word == 0u || spi->info.fifo_width_bits == 0u)
        return;
    pack = (spi->info.fifo_width_bits % bits_per_word)
               ? 1u
               : spi->info.fifo_width_bits / bits_per_word;
    geni_config_packing(spi, bits_per_word, pack, msb_first);
    geni_write(spi, TB710FU_GENI_SPI_WORD_LEN,
               (bits_per_word - TB710FU_GENI_WORD_LEN_MIN) & 0x3FFu);
}

int tb710fu_geni_read_info(struct tb710fu_geni_spi *spi, int probe_fuse)
{
    uint32_t fw;
    uint32_t major, minor, mask;

    if (!spi || !spi->io.read || !spi->io.write || !spi->io.read_wrapper)
        return TB710FU_GENI_ERR_PARAM;
    memset(&spi->info, 0, sizeof(spi->info));

    fw = geni_read(spi, TB710FU_GENI_FW_REVISION_RO);
    spi->info.fw_revision = fw;
    spi->info.protocol = (fw >> TB710FU_GENI_FW_PROTO_SHIFT) & 0xFFu;
    spi->info.geni_status = geni_read(spi, TB710FU_GENI_STATUS);
    spi->info.hw_param0 = geni_read(spi, TB710FU_GENI_HW_PARAM_0);
    spi->info.qup_hw_version = spi->io.read_wrapper(spi->io.ctx, TB710FU_QUP_HW_VER);

    /* A window that is not clocked reads all-zero or all-ones; both are
     * reported as "no device" so the caller never writes into it. */
    if (fw == 0u || fw == 0xFFFFFFFFu)
        return TB710FU_GENI_ERR_NODEV;

    /* QUPv3 3.10 and later widen the Tx FIFO depth field (0xFF over 0x3F). */
    major = spi->info.qup_hw_version >> 28;
    minor = (spi->info.qup_hw_version >> 16) & 0xFFFu;
    mask = ((major == 3u && minor >= 10u) || major > 3u) ? 0xFF0000u : 0x3F0000u;
    spi->info.tx_fifo_depth = (spi->info.hw_param0 & mask) >> 16;
    spi->info.fifo_width_bits = (spi->info.hw_param0 >> 24) & 0x3Fu;

    /* SE_GENI_IF_DISABLE is documented to abort on this controller
     * generation. It is read only when the caller explicitly asks for it. */
    spi->info.fuse_probed = probe_fuse ? 1 : 0;
    if (probe_fuse) {
        uint32_t value = geni_read(spi, TB710FU_GENI_IF_DISABLE_RO);

        spi->info.if_disable = value;
        spi->info.fifo_disabled = (value & TB710FU_GENI_FIFO_IF_DISABLE) != 0u;
    }

    if (spi->state == TB710FU_GENI_RESET || spi->state == TB710FU_GENI_ERROR)
        spi->state = TB710FU_GENI_CONFIG;
    return TB710FU_GENI_OK;
}

int tb710fu_geni_init(struct tb710fu_geni_spi *spi)
{
    uint32_t major, minor;
    int result;

    if (!spi || !spi->io.read || !spi->io.write)
        return TB710FU_GENI_ERR_PARAM;

    if (spi->state == TB710FU_GENI_RESET) {
        result = tb710fu_geni_read_info(spi, 0);
        if (result != TB710FU_GENI_OK) return result;
    }
    if (spi->state == TB710FU_GENI_IDLE)
        return TB710FU_GENI_OK;
    if (spi->state != TB710FU_GENI_CONFIG && spi->state != TB710FU_GENI_ERROR)
        return TB710FU_GENI_ERR_STATE;

    if (spi->info.protocol != TB710FU_GENI_PROTO_SPI) {
        spi->state = TB710FU_GENI_ERROR;
        return TB710FU_GENI_ERR_PROTO;
    }
    if (spi->info.fifo_disabled) {
        /* Explicit diagnosis reported a fused-off FIFO interface; the DMA
         * alternative is not implemented, so refuse instead of stubbing. */
        spi->state = TB710FU_GENI_ERROR;
        return TB710FU_GENI_ERR_NOTSUP;
    }
    if (spi->info.tx_fifo_depth < 4u || spi->info.fifo_width_bits != 32u) {
        spi->state = TB710FU_GENI_ERROR;
        return TB710FU_GENI_ERR_NODEV;
    }

    /* geni_se_init: drop stale interrupts, enable the core clock, release
     * force-default and output controls. */
    geni_irq_clear_all(spi);
    geni_set_bits(spi, TB710FU_GENI_CGC_CTRL, TB710FU_GENI_DEFAULT_CGC);
    geni_set_bits(spi, TB710FU_GENI_DMA_GENERAL_CFG, 0x0Fu);
    geni_write(spi, TB710FU_GENI_OUTPUT_CTRL, TB710FU_GENI_DEFAULT_OUTPUT_CTRL);
    geni_write(spi, TB710FU_GENI_FORCE_DEFAULT, TB710FU_GENI_FORCE_DEFAULT_BIT);
    geni_set_bits(spi, TB710FU_GENI_IRQ_EN,
                  TB710FU_GENI_IRQ_M | TB710FU_GENI_IRQ_S |
                      TB710FU_GENI_IRQ_DMA_TX | TB710FU_GENI_IRQ_DMA_RX);
    {
        uint32_t mode = geni_read(spi, TB710FU_GENI_DMA_MODE_EN);

        geni_write(spi, TB710FU_GENI_DMA_MODE_EN,
                   mode & ~TB710FU_GENI_DMA_MODE_ENABLE);
    }
    geni_write(spi, TB710FU_GENI_GSI_EVENT_EN, 0u);
    geni_write(spi, TB710FU_GENI_RX_WATERMARK, spi->info.tx_fifo_depth - 3u);
    geni_write(spi, TB710FU_GENI_RX_RFR_WATERMARK, spi->info.tx_fifo_depth - 2u);
    geni_set_bits(spi, TB710FU_GENI_M_IRQ_EN,
                  TB710FU_GENI_M_CMD_DONE | TB710FU_GENI_M_RX_FIFO_RD_ERR |
                      TB710FU_GENI_M_RX_FIFO_WR_ERR |
                      TB710FU_GENI_M_TX_FIFO_RD_ERR |
                      TB710FU_GENI_M_TX_FIFO_WR_ERR);
    geni_set_bits(spi, TB710FU_GENI_S_IRQ_EN, 0x3E3Eu | (1u << 24) | (1u << 25));

    spi->tx_watermark = 1u;
    major = spi->info.qup_hw_version >> 28;
    minor = (spi->info.qup_hw_version >> 16) & 0xFFFu;
    spi->oversampling = (major == 1u && minor == 0u) ? 2u : 1u;

    geni_irq_clear_all(spi);
    geni_set_bits(spi, TB710FU_GENI_M_IRQ_EN,
                  TB710FU_GENI_M_CMD_DONE | TB710FU_GENI_M_TX_FIFO_WATERMARK |
                      TB710FU_GENI_M_RX_FIFO_WATERMARK |
                      TB710FU_GENI_M_RX_FIFO_LAST);
    geni_write(spi, TB710FU_GENI_SPI_LOOPBACK, 0u);
    geni_write(spi, TB710FU_GENI_SPI_DEMUX_SEL, 0u);
    geni_write(spi, TB710FU_GENI_SPI_CPHA, 0u);
    geni_write(spi, TB710FU_GENI_SPI_CPOL, 0u);
    geni_write(spi, TB710FU_GENI_SPI_DEMUX_OUTPUT_INV, 0u);
    {
        uint32_t cfg = geni_read(spi, TB710FU_GENI_SPI_TRANS_CFG);

        geni_write(spi, TB710FU_GENI_SPI_TRANS_CFG, cfg & ~TB710FU_GENI_CS_TOGGLE);
    }

    if (spi->poll_limit == 0u) spi->poll_limit = 200000u;
    spi->cur_bpw = 0u;
    spi->cur_speed_hz = 0u;
    spi->last_mode = 0u;
    spi->last_cs = 0;
    spi->last_result = TB710FU_GENI_OK;
    spi->snap_valid = 0;
    /* polled completion: no engine interrupt reaches the GIC. */
    if (spi->poll_only == 1) geni_write(spi, TB710FU_GENI_IRQ_EN, 0u);
    spi->state = TB710FU_GENI_IDLE;
    return TB710FU_GENI_OK;
}

int tb710fu_geni_setup(struct tb710fu_geni_spi *spi, unsigned mode, unsigned chip_select)
{
    unsigned changed;
    int cs_changed;

    if (!spi || chip_select > 15u) return TB710FU_GENI_ERR_PARAM;
    if (spi->state != TB710FU_GENI_IDLE && spi->state != TB710FU_GENI_CONFIG)
        return TB710FU_GENI_ERR_STATE;

    changed = spi->last_mode ^ mode;
    cs_changed = spi->last_cs != (int)chip_select;
    spi->last_cs = (int)chip_select;
    spi->last_mode = mode;
    if ((changed & TB710FU_SPI_MODE_LSB_FIRST) != 0u) spi->cur_bpw = 0u;
    if ((changed & TB710FU_SPI_MODE_LOOP) != 0u)
        geni_write(spi, TB710FU_GENI_SPI_LOOPBACK,
                   (mode & TB710FU_SPI_MODE_LOOP) ? TB710FU_GENI_LOOPBACK_ENABLE : 0u);
    if (cs_changed) geni_write(spi, TB710FU_GENI_SPI_DEMUX_SEL, chip_select);
    if ((changed & TB710FU_SPI_MODE_CPHA) != 0u)
        geni_write(spi, TB710FU_GENI_SPI_CPHA,
                   (mode & TB710FU_SPI_MODE_CPHA) ? TB710FU_GENI_CPHA_BIT : 0u);
    if ((changed & TB710FU_SPI_MODE_CPOL) != 0u)
        geni_write(spi, TB710FU_GENI_SPI_CPOL,
                   (mode & TB710FU_SPI_MODE_CPOL) ? TB710FU_GENI_CPOL_BIT : 0u);
    if ((changed & TB710FU_SPI_MODE_CS_HIGH) != 0u ||
        (cs_changed && (mode & TB710FU_SPI_MODE_CS_HIGH)))
        geni_write(spi, TB710FU_GENI_SPI_DEMUX_OUTPUT_INV,
                   (mode & TB710FU_SPI_MODE_CS_HIGH) ? (1u << chip_select) : 0u);
    return TB710FU_GENI_OK;
}

int tb710fu_geni_set_clock(struct tb710fu_geni_spi *spi, unsigned speed_hz,
                           unsigned source_hz, unsigned clock_source)
{
    unsigned long long denominator;
    unsigned long long divider;

    if (!spi || speed_hz == 0u || source_hz == 0u || clock_source > 7u ||
        spi->oversampling == 0u)
        return TB710FU_GENI_ERR_PARAM;
    if (spi->state != TB710FU_GENI_IDLE && spi->state != TB710FU_GENI_CONFIG)
        return TB710FU_GENI_ERR_STATE;

    denominator = (unsigned long long)spi->oversampling * speed_hz;
    divider = ((unsigned long long)source_hz + denominator - 1u) / denominator;
    if (divider == 0u || divider > TB710FU_GENI_CLK_DIV_MAX)
        return TB710FU_GENI_ERR_PARAM;

    geni_write(spi, TB710FU_GENI_CLK_SEL, clock_source);
    geni_write(spi, TB710FU_GENI_SER_M_CLK_CFG,
               ((uint32_t)divider << TB710FU_GENI_CLK_DIV_SHIFT) |
                   TB710FU_GENI_SER_CLK_EN);
    spi->cur_speed_hz = speed_hz;
    return TB710FU_GENI_OK;
}

/* Fill the Tx FIFO up to the watermark, accounting for words the engine has
 * not drained yet. A blind watermark-sized write overflows the FIFO on the
 * second and later fills of a long transfer. */
static int geni_handle_tx(struct tb710fu_geni_spi *spi)
{
    const unsigned bytes_per_word = geni_bytes_per_word(spi);
    unsigned workspace;
    unsigned used;
    size_t limit;
    size_t written = 0;
    const uint8_t *base;

    if (!spi->xfer_tx || bytes_per_word == 0u) return 0;
    if (spi->tx_watermark > spi->info.tx_fifo_depth)
        spi->tx_watermark = 0u;
    used = geni_read(spi, TB710FU_GENI_TX_FIFO_STATUS) & TB710FU_GENI_TX_FIFO_WC_MASK;
    if (used > spi->info.tx_fifo_depth) used = spi->info.tx_fifo_depth;
    if (used > spi->info.tx_fifo_depth - spi->tx_watermark)
        used = spi->info.tx_fifo_depth - spi->tx_watermark;
    workspace = spi->info.tx_fifo_depth - spi->tx_watermark - used;
    limit = (size_t)workspace * bytes_per_word;
    if (limit > spi->xfer_tx_left) limit = spi->xfer_tx_left;
    base = spi->xfer_tx + (spi->xfer_len - spi->xfer_tx_left);
    if (limit == 0u) return 1;

    while (written < limit) {
        uint32_t word = 0u;
        size_t in_word = limit - written;
        size_t byte;

        if (in_word > bytes_per_word) in_word = bytes_per_word;
        for (byte = 0u; byte < in_word; ++byte)
            word |= (uint32_t)base[written + byte] << (8u * byte);
        geni_write(spi, TB710FU_GENI_TX_FIFO, word);
        written += in_word;
    }
    spi->xfer_tx_left -= written;
    if (spi->xfer_tx_left == 0u) {
        geni_write(spi, TB710FU_GENI_TX_WATERMARK, 0u);
        return 0;
    }
    return 1;
}

static void geni_handle_rx(struct tb710fu_geni_spi *spi)
{
    const unsigned bytes_per_word = geni_bytes_per_word(spi);
    uint32_t status;
    size_t bytes;
    size_t words;
    size_t read = 0;
    uint8_t *base;

    if (bytes_per_word == 0u) return;
    status = geni_read(spi, TB710FU_GENI_RX_FIFO_STATUS);
    bytes = (size_t)(status & TB710FU_GENI_RX_WC_MASK) * bytes_per_word;
    if ((status & TB710FU_GENI_RX_LAST) != 0u) {
        unsigned valid = (status >> TB710FU_GENI_RX_LAST_VALID_SHIFT) &
                         TB710FU_GENI_RX_LAST_VALID_MASK;

        if (valid != 0u && valid < bytes_per_word)
            bytes -= bytes_per_word - valid;
    }
    words = (bytes + bytes_per_word - 1u) / bytes_per_word;
    if (!spi->xfer_rx) {
        size_t word;

        for (word = 0u; word < words; ++word)
            (void)geni_read(spi, TB710FU_GENI_RX_FIFO);
        return;
    }
    if (bytes > spi->xfer_rx_left) bytes = spi->xfer_rx_left;
    base = spi->xfer_rx + (spi->xfer_len - spi->xfer_rx_left);
    while (read < bytes) {
        uint32_t word = geni_read(spi, TB710FU_GENI_RX_FIFO);
        size_t in_word = bytes - read;
        size_t byte;

        if (in_word > bytes_per_word) in_word = bytes_per_word;
        for (byte = 0u; byte < in_word; ++byte)
            base[read + byte] = (uint8_t)(word >> (8u * byte));
        read += in_word;
    }
    spi->xfer_rx_left -= bytes;
}

static void geni_snapshot(struct tb710fu_geni_spi *spi)
{
    spi->snap_irq = geni_read(spi, TB710FU_GENI_M_IRQ_STATUS);
    spi->snap_status = geni_read(spi, TB710FU_GENI_STATUS);
    spi->snap_tx_fifo = geni_read(spi, TB710FU_GENI_TX_FIFO_STATUS);
    spi->snap_rx_fifo = geni_read(spi, TB710FU_GENI_RX_FIFO_STATUS);
    spi->snap_cmd0 = geni_read(spi, TB710FU_GENI_M_CMD0);
    spi->snap_valid = 1;
}

static int geni_wait_bits(struct tb710fu_geni_spi *spi, uint32_t bits)
{
    unsigned spin;

    for (spin = 0u; spin < spi->poll_limit; ++spin) {
        uint32_t status = geni_read(spi, TB710FU_GENI_M_IRQ_STATUS);

        if ((status & bits) != 0u) {
            geni_write(spi, TB710FU_GENI_M_IRQ_CLEAR, status);
            return TB710FU_GENI_OK;
        }
        if (spi->io.delay_us) spi->io.delay_us(spi->io.ctx, 1u);
    }
    return TB710FU_GENI_ERR_TIMEOUT;
}

int tb710fu_geni_recover(struct tb710fu_geni_spi *spi)
{
    if (!spi) return TB710FU_GENI_ERR_PARAM;
    if (spi->state == TB710FU_GENI_IDLE) return TB710FU_GENI_OK;

    geni_write(spi, TB710FU_GENI_TX_WATERMARK, 0u);
    geni_write(spi, TB710FU_GENI_M_CMD_CTRL, TB710FU_GENI_M_CMD_CANCEL);
    if (geni_wait_bits(spi, TB710FU_GENI_M_CMD_CANCEL_DONE |
                                TB710FU_GENI_M_CMD_DONE) == TB710FU_GENI_OK) {
        spi->state = TB710FU_GENI_IDLE;
        return TB710FU_GENI_OK;
    }
    geni_write(spi, TB710FU_GENI_M_CMD_CTRL, TB710FU_GENI_M_CMD_ABORT);
    if (geni_wait_bits(spi, TB710FU_GENI_M_CMD_ABORT_DONE |
                                TB710FU_GENI_M_CMD_DONE) != TB710FU_GENI_OK) {
        spi->state = TB710FU_GENI_ERROR;
        return TB710FU_GENI_ERR_TIMEOUT;
    }
    spi->state = TB710FU_GENI_IDLE;
    return TB710FU_GENI_ERR_TIMEOUT;
}

static int geni_begin_transfer(struct tb710fu_geni_spi *spi, const uint8_t *tx,
                               uint8_t *rx, size_t len, int keep_cs)
{
    uint32_t command = 0u;

    if (!spi || len == 0u || len > TB710FU_GENI_TRANS_LEN_MASK || (!tx && !rx))
        return TB710FU_GENI_ERR_PARAM;
    if (spi->state != TB710FU_GENI_IDLE)
        return TB710FU_GENI_ERR_STATE;
    if (spi->cur_speed_hz == 0u)
        return TB710FU_GENI_ERR_PARAM;

    if (spi->cur_bpw != 8u) {
        geni_set_word_len(spi, 8u,
                          (spi->last_mode & TB710FU_SPI_MODE_LSB_FIRST) == 0u);
        spi->cur_bpw = 8u;
    }

    spi->xfer_tx = tx;
    spi->xfer_rx = rx;
    spi->xfer_len = len;
    spi->xfer_tx_left = tx ? len : 0u;
    spi->xfer_rx_left = rx ? len : 0u;
    spi->xfer_keep_cs = keep_cs;

    if (tx) {
        command |= TB710FU_GENI_M_CMD_TX_ONLY;
        geni_write(spi, TB710FU_GENI_SPI_TX_TRANS_LEN, (uint32_t)len);
    }
    if (rx) {
        command |= TB710FU_GENI_M_CMD_RX_ONLY;
        geni_write(spi, TB710FU_GENI_SPI_RX_TRANS_LEN, (uint32_t)len);
    }
    if (command == (TB710FU_GENI_M_CMD_TX_ONLY | TB710FU_GENI_M_CMD_RX_ONLY))
        command = TB710FU_GENI_M_CMD_TX_RX;

    geni_write(spi, TB710FU_GENI_M_IRQ_CLEAR, 0xFFFFFFFFu);
    geni_write(spi, TB710FU_GENI_M_CMD0,
               (command << TB710FU_GENI_M_OPCODE_SHIFT) |
                   (keep_cs ? TB710FU_GENI_M_PARAM_FRAGMENTATION : 0u));
    spi->state = TB710FU_GENI_ACTIVE;
    if (tx) (void)geni_handle_tx(spi);
    return TB710FU_GENI_OK;
}

static int geni_finish_transfer(struct tb710fu_geni_spi *spi, int status)
{
    spi->xfer_tx = 0;
    spi->xfer_rx = 0;
    spi->last_result = status;
    if (status == TB710FU_GENI_OK || spi->state == TB710FU_GENI_IDLE)
        spi->state = TB710FU_GENI_IDLE;
    else
        spi->state = TB710FU_GENI_ERROR;
    return status;
}

int tb710fu_geni_transfer(struct tb710fu_geni_spi *spi, const uint8_t *tx,
                          uint8_t *rx, size_t len, int keep_cs)
{
    unsigned spin;
    int status = TB710FU_GENI_ERR_TIMEOUT;
    int result;

    if (!spi || !spi->io.read) return TB710FU_GENI_ERR_PARAM;
    result = geni_begin_transfer(spi, tx, rx, len, keep_cs);
    if (result != TB710FU_GENI_OK) return result;

    for (spin = 0u; spin < spi->poll_limit; ++spin) {
        uint32_t irq = geni_read(spi, TB710FU_GENI_M_IRQ_STATUS);

        if (irq == 0u) {
            if (spi->io.delay_us) spi->io.delay_us(spi->io.ctx, 1u);
            continue;
        }
        geni_write(spi, TB710FU_GENI_M_IRQ_CLEAR, irq);
        if ((irq & TB710FU_GENI_M_ERRORS) != 0u) {
            status = TB710FU_GENI_ERR_IO;
            break;
        }
        if ((irq & (TB710FU_GENI_M_RX_FIFO_WATERMARK | TB710FU_GENI_M_RX_FIFO_LAST)) != 0u)
            geni_handle_rx(spi);
        if ((irq & TB710FU_GENI_M_TX_FIFO_WATERMARK) != 0u && spi->xfer_tx)
            (void)geni_handle_tx(spi);
        if ((irq & TB710FU_GENI_M_CMD_DONE) != 0u) {
            if (spi->xfer_rx && spi->xfer_rx_left) geni_handle_rx(spi);
            status = (spi->xfer_tx_left || spi->xfer_rx_left)
                         ? TB710FU_GENI_ERR_IO
                         : TB710FU_GENI_OK;
            if (spi->xfer_tx_left) geni_write(spi, TB710FU_GENI_TX_WATERMARK, 0u);
            break;
        }
    }

    if (status != TB710FU_GENI_OK) {
        geni_snapshot(spi);
        (void)tb710fu_geni_recover(spi);
    }
    return geni_finish_transfer(spi, status);
}
