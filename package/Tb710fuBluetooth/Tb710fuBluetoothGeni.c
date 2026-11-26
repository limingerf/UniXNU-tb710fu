/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* See Tb710fuBluetoothGeni.h for register provenance. Content rules: no IOKit, no allocation, no globals;
 * the only way this file touches the controller is through the caller's 32-bit ops, which is what keeps
 * memcpy/bzero/memset off MMIO by construction. */
#include "Tb710fuBluetoothGeni.h"

static uint32_t rd(const TbBtGeni *g, uint32_t offset)
{
    return g->ops->read32(g->ops->ctx, offset);
}

static void wr(const TbBtGeni *g, uint32_t offset, uint32_t value)
{
    g->ops->write32(g->ops->ctx, offset, value);
}

/* One barrier between a burst of register writes and the poll that waits on their effect. The caller's
 * barrier is dsb(sy) on the kernel side, which is what orders posted writes to Device memory. */
static void settle(const TbBtGeni *g)
{
    if (g->ops->barrier) g->ops->barrier(g->ops->ctx);
}

static void note(const TbBtGeni *g, const char *what, uint32_t a, uint32_t b)
{
    if (g->ops->note) g->ops->note(g->ops->ctx, what, a, b);
}

static uint64_t now_ns(const TbBtGeni *g)
{
    return g->ops->now_ns(g->ops->ctx);
}

static void pause_us(const TbBtGeni *g, uint32_t us)
{
    g->ops->delay_us(g->ops->ctx, us);
}

static void fail(TbBtGeni *g, int code)
{
    g->errors++;
    g->last_error = code < 0 ? (uint32_t)(-code) : (uint32_t)code;
}

static int wait_mask(const TbBtGeni *g, uint32_t offset, uint32_t mask, uint32_t want, uint64_t deadline)
{
    while (now_ns(g) < deadline) {
        if ((rd(g, offset) & mask) == want) return TB_BT_GENI_OK;
        pause_us(g, 10u);
    }
    return TB_BT_GENI_TIMEOUT;
}

/* qcom_geni_serial_poll_cancel_tx_cmd / _rx_cmd: ask for CANCEL, wait for the cancel bit, otherwise ABORT
 * and wait for that. A previous owner of the SE can leave either sequencer mid-command. */
static void cancel_m(TbBtGeni *g)
{
    if (!(rd(g, TB_GENI_STATUS) & TB_M_GENI_CMD_ACTIVE)) return;
    wr(g, TB_GENI_M_CMD_CTRL, TB_M_GENI_CMD_CANCEL);
    settle(g);
    if (wait_mask(g, TB_GENI_M_IRQ_STATUS, TB_M_CMD_CANCEL, TB_M_CMD_CANCEL, now_ns(g) + TB_BT_CMD_TIMEOUT_NS)) {
        wr(g, TB_GENI_M_CMD_CTRL, TB_M_GENI_CMD_ABORT);
        settle(g);
        (void)wait_mask(g, TB_GENI_M_IRQ_STATUS, TB_M_CMD_ABORT, TB_M_CMD_ABORT, now_ns(g) + TB_BT_CMD_TIMEOUT_NS);
        wr(g, TB_GENI_M_IRQ_CLEAR, TB_M_CMD_ABORT);
    }
    wr(g, TB_GENI_M_IRQ_CLEAR, TB_M_CMD_CANCEL);
    settle(g);
}

static void cancel_s(TbBtGeni *g)
{
    if (!(rd(g, TB_GENI_STATUS) & TB_S_GENI_CMD_ACTIVE)) return;
    wr(g, TB_GENI_S_CMD_CTRL, TB_S_GENI_CMD_CANCEL);
    settle(g);
    if (wait_mask(g, TB_GENI_S_IRQ_STATUS, TB_S_CMD_CANCEL, TB_S_CMD_CANCEL, now_ns(g) + TB_BT_CMD_TIMEOUT_NS)) {
        wr(g, TB_GENI_S_CMD_CTRL, TB_S_GENI_CMD_ABORT);
        settle(g);
        (void)wait_mask(g, TB_GENI_S_IRQ_STATUS, TB_S_CMD_ABORT, TB_S_CMD_ABORT, now_ns(g) + TB_BT_CMD_TIMEOUT_NS);
        wr(g, TB_GENI_S_IRQ_CLEAR, TB_S_CMD_ABORT);
    }
    wr(g, TB_GENI_S_IRQ_CLEAR, TB_S_CMD_CANCEL);
    settle(g);
}

/* geni_se_config_packing(se, 8, 4, false, true, true), walked through with the upstream loop: bpw 8 and
 * pack_words 4 give iter = (8 * 4) / 8 = 4 vectors, first vector index 0, direction LSB first, length 7,
 * start 0/8/16/24, stop bit on the last vector. cfg[0] and cfg[1] share TX_PACKING_CFG0 (ten bits each),
 * cfg[2] and cfg[3] share CFG1, and the RX pair gets the same values. That is the layout documented in
 * the upstream "word_size = 7, packing_mode = 4 x 8" table, and the four vector values are
 * 0x0e, 0x10e, 0x20e, 0x30f. Byte granularity is bpw / 16 = 0 (4x8). */
static void config_packing(TbBtGeni *g)
{
    const uint32_t v0 = (0u << 5) | (7u << 1);
    const uint32_t v1 = (8u << 5) | (7u << 1);
    const uint32_t v2 = (16u << 5) | (7u << 1);
    const uint32_t v3 = (24u << 5) | (7u << 1) | 1u; /* stop bit */
    const uint32_t cfg0 = v0 | (v1 << 10);
    const uint32_t cfg1 = v2 | (v3 << 10);
    wr(g, TB_GENI_TX_PACK_0, cfg0);
    wr(g, TB_GENI_TX_PACK_1, cfg1);
    wr(g, TB_GENI_RX_PACK_0, cfg0);
    wr(g, TB_GENI_RX_PACK_1, cfg1);
    wr(g, TB_GENI_BYTE_GRAN, 0u);
    note(g, "packing", cfg0, cfg1);
}

int tb_bt_geni_probe(TbBtGeni *geni, const TbBtGeniOps *ops)
{
    if (!geni || !ops || !ops->read32 || !ops->write32 || !ops->now_ns || !ops->delay_us)
        return TB_BT_GENI_ARGUMENT;
    geni->ops = ops;

    const uint32_t fw = rd(geni, TB_GENI_FW_REVISION_RO);
    geni->proto = (fw & TB_FW_REV_PROTOCOL_MASK) >> TB_FW_REV_PROTOCOL_SHIFT;
    const uint32_t hp0 = rd(geni, TB_GENI_HW_PARAM_0);
    const uint32_t hp1 = rd(geni, TB_GENI_HW_PARAM_1);
    geni->tx_width_bits = (hp0 >> 24) & 0xffu;
    geni->tx_depth_words = (hp0 >> 16) & 0xffu;
    geni->rx_depth_words = (hp1 >> 16) & 0xffu;
    if (geni->tx_depth_words == 0u) geni->tx_depth_words = 16u;
    if (geni->rx_depth_words == 0u) geni->rx_depth_words = 16u;
    if (geni->tx_width_bits == 0u) geni->tx_width_bits = 32u;

    note(geni, "fwrevision", fw, geni->proto);
    note(geni, "tx-fifo", geni->tx_depth_words, geni->tx_width_bits);
    note(geni, "rx-fifo", geni->rx_depth_words, hp1);
    note(geni, "status", rd(geni, TB_GENI_STATUS), rd(geni, TB_GENI_IF_DISABLE_RO));
    note(geni, "clkcfg", rd(geni, TB_GENI_SER_M_CLK_CFG), rd(geni, TB_GENI_SER_S_CLK_CFG));
    note(geni, "clksel", rd(geni, TB_GENI_CLK_SEL), rd(geni, TB_GENI_CLK_CTRL_RO));
    note(geni, "ios", rd(geni, TB_GENI_IOS), rd(geni, TB_GENI_M_GP_LENGTH));
    return TB_BT_GENI_OK;
}

int tb_bt_geni_config(TbBtGeni *geni)
{
    if (!geni || !geni->ops) return TB_BT_GENI_ARGUMENT;
    TbBtGeni *g = geni;

    /* Polled mode. geni_se_io_set_mode() clears DMA_RX/DMA_TX/GENI_M/GENI_S in SE_IRQ_EN; leaving those
     * cleared also means nothing in the SE ever raises the GIC line, so the driver needs no interrupt
     * handler and cannot leave a level interrupt pending. Status words still latch and are polled. */
    wr(g, TB_GENI_SE_IRQ_EN, rd(g, TB_GENI_SE_IRQ_EN) & ~0xfu);

    cancel_m(g);
    cancel_s(g);

    if (geni->proto == TB_FW_REV_PROTOCOL_INVALID) {
        /* No protocol firmware in the SE. Upstream loads GENI_SE_UART through qcom_scm here; this kernel
         * has no SCM firmware channel, so refusing is the only honest answer. */
        note(g, "proto-invalid", geni->proto, 0u);
        fail(geni, TB_BT_GENI_ERROR);
        return TB_BT_GENI_ERROR;
    }
    if (geni->proto != TB_FW_REV_PROTOCOL_UART) {
        note(g, "proto-mismatch", geni->proto, TB_FW_REV_PROTOCOL_UART);
        fail(geni, TB_BT_GENI_ERROR);
        return TB_BT_GENI_ERROR;
    }

    /* geni_se_select_fifo_mode(): clear GENI_DMA_MODE_EN, clear the GSI event enables. */
    wr(g, TB_GENI_DMA_MODE_EN, rd(g, TB_GENI_DMA_MODE_EN) & ~1u);
    wr(g, TB_GENI_GSI_EVENT_EN, 0u);
    /* geni_se_io_init(): keep the DMA/AHB sub-block clocks gated (bits 3..0) and gate the SE sub-clocks. */
    wr(g, TB_GENI_DMA_GENERAL_CFG, rd(g, TB_GENI_DMA_GENERAL_CFG) | 0xfu);
    wr(g, TB_GENI_CGC_CTRL, rd(g, TB_GENI_CGC_CTRL) | 0x7fu);

    config_packing(geni);

    wr(g, TB_UART_LOOPBACK_CFG, 0u);
    wr(g, TB_UART_IO_MACRO_CTRL, rd(g, TB_UART_IO_MACRO_CTRL) & ~0xffffu);
    wr(g, TB_UART_RX_STALE_CNT, TB_UART_RX_STALE);
    wr(g, TB_UART_TX_WORD_LEN, TB_UART_BITS_PER_CHAR);
    wr(g, TB_UART_RX_WORD_LEN, TB_UART_BITS_PER_CHAR);
    wr(g, TB_UART_TX_STOP_BIT_LEN, TB_TX_STOP_BIT_LEN_1);
    wr(g, TB_UART_TX_PARITY_CFG, 0u);
    wr(g, TB_UART_RX_PARITY_CFG, 0u);
    wr(g, TB_UART_RX_TRANS_CFG, 0u);
    wr(g, TB_UART_TX_TRANS_CFG, TB_UART_CTS_MASK); /* masked until tb_bt_geni_set_flow(true) */
    wr(g, TB_UART_MANUAL_RFR, TB_UART_MANUAL_RFR_EN | TB_UART_RFR_NOT_READY);
    geni->flow = false;

    wr(g, TB_GENI_RX_WATERMARK, TB_UART_RX_WM & TB_WATERMARK_MASK);
    wr(g, TB_GENI_TX_WATERMARK, TB_UART_TX_WM & TB_WATERMARK_MASK);
    uint32_t rfr = geni->rx_depth_words > 2u ? geni->rx_depth_words - 2u : 1u;
    if (rfr > TB_WATERMARK_MASK) rfr = TB_WATERMARK_MASK;
    wr(g, TB_GENI_RX_RFR_WATERMARK, rfr);
    note(g, "rfr", rfr, geni->rx_depth_words);

    wr(g, TB_GENI_OUTPUT_CTRL, TB_GENI_DEFAULT_OUTPUT_CTRL);
    wr(g, TB_GENI_FORCE_DEFAULT, 1u);
    wr(g, TB_GENI_M_IRQ_CLEAR, 0xffffffffu);
    wr(g, TB_GENI_S_IRQ_CLEAR, 0xffffffffu);
    settle(g);

    geni->configured = true;
    int rc = tb_bt_geni_start_rx(geni);
    if (rc) fail(geni, rc);
    return rc;
}

int tb_bt_geni_start_rx(TbBtGeni *geni)
{
    if (!geni || !geni->ops) return TB_BT_GENI_ARGUMENT;
    if (geni->rx_open) tb_bt_geni_stop_rx(geni);
    /* geni_se_setup_s_cmd(se, UART_START_READ, 0): opcode in bits 31:27, param 0 (the FIFO path passes no
     * RFR_OPEN; hardware drives RTS from the RFR watermark instead). */
    wr(geni, TB_GENI_S_CMD0, TB_UART_START_READ << TB_M_OPCODE_SHIFT);
    settle(geni);
    if (wait_mask(geni, TB_GENI_STATUS, TB_S_GENI_CMD_ACTIVE, TB_S_GENI_CMD_ACTIVE,
                  now_ns(geni) + TB_BT_RX_START_TIMEOUT_NS)) {
        note(geni, "rx-start-timeout", rd(geni, TB_GENI_STATUS), 0u);
        fail(geni, TB_BT_GENI_TIMEOUT);
        return TB_BT_GENI_TIMEOUT;
    }
    geni->rx_open = true;
    return TB_BT_GENI_OK;
}

int tb_bt_geni_stop_rx(TbBtGeni *geni)
{
    if (!geni || !geni->ops) return TB_BT_GENI_ARGUMENT;
    cancel_s(geni);
    wr(geni, TB_GENI_S_IRQ_CLEAR, 0xffffffffu);
    settle(geni);
    geni->rx_open = false;
    return TB_BT_GENI_OK;
}

int tb_bt_geni_set_rate(TbBtGeni *geni, uint32_t baud, uint32_t rate, uint32_t clk_div)
{
    if (!geni || !geni->ops || !baud || !rate || !clk_div || clk_div > TB_CLK_DIV_MAX)
        return TB_BT_GENI_ARGUMENT;
    const bool was_open = geni->rx_open;
    if (was_open) tb_bt_geni_stop_rx(geni);

    const uint32_t cfg = TB_SER_CLK_EN | (clk_div << TB_CLK_DIV_SHIFT);
    wr(geni, TB_GENI_SER_M_CLK_CFG, cfg);
    wr(geni, TB_GENI_SER_S_CLK_CFG, cfg);
    /* qcom_geni_serial writes the SE's clock index into GENI_CLK_SEL. The only parent this SE has in the
     * device tree is the GCC S6 branch, which the wrapper presents at index 0. */
    wr(geni, TB_GENI_CLK_SEL, 0u & TB_CLK_SEL_MASK);
    settle(geni);

    geni->baud = baud;
    geni->clk_rate = rate;
    geni->clk_div = clk_div;
    note(geni, "se-rate", rate, clk_div);
    note(geni, "se-baud", baud, 0u);

    if (was_open) return tb_bt_geni_start_rx(geni);
    return TB_BT_GENI_OK;
}

int tb_bt_geni_set_flow(TbBtGeni *geni, bool enabled)
{
    if (!geni || !geni->ops) return TB_BT_GENI_ARGUMENT;
    uint32_t cfg = rd(geni, TB_UART_TX_TRANS_CFG);
    if (enabled) {
        cfg &= ~TB_UART_CTS_MASK;
        wr(geni, TB_UART_MANUAL_RFR, 0u);
    } else {
        cfg |= TB_UART_CTS_MASK;
        wr(geni, TB_UART_MANUAL_RFR, TB_UART_MANUAL_RFR_EN | TB_UART_RFR_NOT_READY);
    }
    wr(geni, TB_UART_TX_TRANS_CFG, cfg);
    settle(geni);
    geni->flow = enabled;
    return TB_BT_GENI_OK;
}

bool tb_bt_geni_cts(TbBtGeni *geni)
{
    if (!geni || !geni->ops) return false;
    /* IO2_DATA_IN follows qcom_geni_serial_get_mctrl: a 0 bit means the controller is asserting CTS. */
    return (rd(geni, TB_GENI_IOS) & TB_IO2_DATA_IN) == 0u;
}

uint32_t tb_bt_geni_tx_words(TbBtGeni *geni)
{
    if (!geni || !geni->ops) return 0u;
    return rd(geni, TB_GENI_TX_FIFO_STATUS) & TB_TX_FIFO_WC_MASK;
}

int tb_bt_geni_write(TbBtGeni *geni, const uint8_t *bytes, uint32_t len)
{
    if (!geni || !geni->ops || (!bytes && len)) return TB_BT_GENI_ARGUMENT;
    if (!len) return TB_BT_GENI_OK;
    const uint64_t deadline = now_ns(geni) + TB_BT_TX_TIMEOUT_NS;

    /* Anything still active belongs to a transfer that already failed: clear it before the length
     * register is reused, because writing TRANS_LEN under a live command corrupts both transfers. */
    if (rd(geni, TB_GENI_STATUS) & TB_M_GENI_CMD_ACTIVE) {
        note(geni, "tx-stale", rd(geni, TB_GENI_STATUS), geni->last_error);
        cancel_m(geni);
    }
    wr(geni, TB_GENI_M_IRQ_CLEAR, 0xffffffffu);
    settle(geni);

    wr(geni, TB_UART_TX_TRANS_LEN, len);
    wr(geni, TB_GENI_M_CMD0, TB_UART_START_TX << TB_M_OPCODE_SHIFT);
    settle(geni);

    uint32_t sent = 0;
    while (sent < len) {
        const uint32_t queued = rd(geni, TB_GENI_TX_FIFO_STATUS) & TB_TX_FIFO_WC_MASK;
        const uint32_t space = queued >= geni->tx_depth_words ? 0u : geni->tx_depth_words - queued;
        if (!space) {
            if (now_ns(geni) >= deadline) {
                cancel_m(geni);
                note(geni, "tx-space-timeout", queued, len - sent);
                fail(geni, TB_BT_GENI_TIMEOUT);
                return TB_BT_GENI_TIMEOUT;
            }
            pause_us(geni, 4u);
            continue;
        }
        uint32_t room = space * TB_BYTES_PER_FIFO_WORD;
        if (room > len - sent) room = len - sent;
        const uint32_t words = (room + TB_BYTES_PER_FIFO_WORD - 1u) / TB_BYTES_PER_FIFO_WORD;
        for (uint32_t i = 0; i < words; ++i) {
            const uint32_t at = sent + i * TB_BYTES_PER_FIFO_WORD;
            uint32_t word = 0u;
            for (uint32_t b = 0; b < TB_BYTES_PER_FIFO_WORD; ++b) {
                if (at + b < len) word |= (uint32_t)bytes[at + b] << (8u * b);
            }
            wr(geni, TB_GENI_TX_FIFO, word);
        }
        sent += room;
    }
    settle(geni);

    int rc = wait_mask(geni, TB_GENI_M_IRQ_STATUS, TB_M_CMD_DONE, TB_M_CMD_DONE, deadline);
    const uint32_t status = rd(geni, TB_GENI_M_IRQ_STATUS);
    wr(geni, TB_GENI_M_IRQ_CLEAR, 0xffffffffu);
    settle(geni);
    if (rc || (status & (TB_M_CMD_FAILURE | TB_M_CMD_ABORT))) {
        note(geni, "tx-fail", status, len);
        if (status & (TB_M_CMD_FAILURE | TB_M_CMD_ABORT)) cancel_m(geni);
        fail(geni, rc ? rc : TB_BT_GENI_ERROR);
        return rc ? rc : TB_BT_GENI_ERROR;
    }
    geni->tx_packets++;
    return (int)len;
}

int tb_bt_geni_read(TbBtGeni *geni, uint8_t *out, uint32_t cap, uint32_t *got)
{
    if (!geni || !geni->ops || !got) return TB_BT_GENI_ARGUMENT;
    *got = 0u;
    uint32_t produced = 0;
    bool overflow = false;

    for (unsigned guard = 0; guard < 64u; ++guard) {
        const uint32_t status = rd(geni, TB_GENI_RX_FIFO_STATUS);
        const uint32_t words = status & TB_RX_FIFO_WC_MASK;
        if (!words) break;
        const bool last = (status & TB_RX_LAST) != 0u;
        const uint32_t last_bytes = (status & TB_RX_LAST_BYTE_MASK) >> TB_RX_LAST_BYTE_SHIFT;
        for (uint32_t i = 0; i < words; ++i) {
            const uint32_t word = rd(geni, TB_GENI_RX_FIFO);
            uint32_t n = TB_BYTES_PER_FIFO_WORD;
            /* Only the final word of a burst can be partial, and a zero field there means four bytes
             * (qcom_geni_serial_get_char). */
            if (last && i + 1u == words && last_bytes) n = last_bytes;
            for (uint32_t b = 0; b < n; ++b) {
                if (produced < cap) out[produced++] = (uint8_t)(word >> (8u * b));
                else overflow = true;
            }
        }
        geni->rx_words += words;
        if (words < 4u) break;
    }
    settle(geni);
    *got = produced;
    if (overflow) {
        note(geni, "rx-overflow", produced, cap);
        fail(geni, TB_BT_GENI_OVERFLOW);
        return TB_BT_GENI_OVERFLOW;
    }
    return TB_BT_GENI_OK;
}

uint32_t tb_bt_geni_service(TbBtGeni *geni)
{
    if (!geni || !geni->ops) return 0u;
    const uint32_t m = rd(geni, TB_GENI_M_IRQ_STATUS);
    const uint32_t s = rd(geni, TB_GENI_S_IRQ_STATUS);
    wr(geni, TB_GENI_M_IRQ_CLEAR, m);
    wr(geni, TB_GENI_S_IRQ_CLEAR, s);
    settle(geni);
    if (s & (TB_S_RX_FIFO_WR_ERR | TB_S_GP_IRQ_0 | TB_S_GP_IRQ_1 | TB_S_GP_IRQ_2 | TB_S_GP_IRQ_3)) {
        fail(geni, TB_BT_GENI_ERROR);
        geni->last_error = s;
    }
    return m | (s << 16);
}
