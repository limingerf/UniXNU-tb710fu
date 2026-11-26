/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host-side checks for the TB710FU WCN7850 Bluetooth cores: GCC RCG math and programming, the GENI FIFO
 * UART transport, HCI H4 framing, the QCA TLV/NVM parser and segment downloader, and the TLMM power
 * sequence. Everything here runs on a PC against fake register files, so the checks say nothing about
 * hardware; they only prove the arithmetic and the command order are what the sources describe.
 *
 * Build and run: scripts/check-tb710fu-bt-host.py (or, by hand,
 *   cc -std=c11 -Wall -Wextra -Werror -Ipackage/Tb710fuBluetooth tests/tb710fu_bluetooth_test.c \
 *      package/Tb710fuBluetooth/Tb710fuBluetooth{Geni,Clock,Hci,Qca,Power}.c -o bt-host && ./bt-host
 * ).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Tb710fuBluetoothBoard.h"
#include "Tb710fuBluetoothClock.h"
#include "Tb710fuBluetoothGeni.h"
#include "Tb710fuBluetoothHci.h"
#include "Tb710fuBluetoothPower.h"
#include "Tb710fuBluetoothQca.h"

static unsigned gChecks;
static unsigned gFailures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        gChecks++;                                                           \
        if (!(cond)) {                                                       \
            gFailures++;                                                     \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        gChecks++;                                                           \
        const long long va = (long long)(a);                                  \
        const long long vb = (long long)(b);                                  \
        if (va != vb) {                                                       \
            gFailures++;                                                     \
            printf("FAIL %s:%d  %s == %s  (%lld != %lld)\n", __FILE__, __LINE__, \
                   #a, #b, va, vb);                                          \
        }                                                                    \
    } while (0)

static void section(const char *name)
{
    printf("-- %s\n", name);
}

/* ==================================================================================================
 * GCC RCG: the baud table, the M/N/D/CFG encoding, and the program sequence against a fake register
 * block. The expected values are recomputed here from the parent clock and the ftbl entries, not copied
 * from the driver, so a wrong table entry disagrees with itself.
 * ================================================================================================ */

/* Covers the RCG at 0x1e760, the branch vote at 0x52010 and the core halt at 0x23294. */
static uint32_t gGcc[0x54000 / 4];

static uint32_t gccRead(void *ctx, uint32_t offset)
{
    (void)ctx;
    const uint32_t index = offset / 4u;
    if (index >= sizeof gGcc / sizeof gGcc[0]) {
        printf("FAIL gcc read out of range %x\n", (unsigned)offset);
        gFailures++;
        return 0u;
    }
    /* CMD_UPDATE self-clears on the hardware; the fake does the same so the poll loop terminates. */
    if (offset == TB_BT_RCG_OFF) {
        const uint32_t value = gGcc[index] & ~1u;
        gGcc[index] = value;
        return value;
    }
    return gGcc[index];
}

static void gccWrite(void *ctx, uint32_t offset, uint32_t value)
{
    (void)ctx;
    const uint32_t index = offset / 4u;
    if (index >= sizeof gGcc / sizeof gGcc[0]) {
        printf("FAIL gcc write out of range %x\n", (unsigned)offset);
        gFailures++;
        return;
    }
    gGcc[index] = value;
}

static void gccDelay(void *ctx, uint32_t us)
{
    (void)ctx;
    (void)us;
}

static const TbBtClockIo kGccIo = {gccRead, gccWrite, gccDelay, NULL};

static void testClockPlan(void)
{
    section("clock plan");
    static const uint32_t kBauds[] = {115200u, 921600u, 3000000u, 3200000u, 4000000u};
    static const uint32_t kRates[] = {7372800u, 14745600u, 48000000u, 51200000u, 64000000u};
    static const uint32_t kDivs[] = {4u, 1u, 1u, 1u, 1u};
    static const uint32_t kM[] = {384u, 768u, 4u, 64u, 16u};
    static const uint32_t kN[] = {15625u, 15625u, 25u, 375u, 75u};
    for (unsigned i = 0; i < sizeof kBauds / sizeof kBauds[0]; ++i) {
        TbBtClockPlan plan;
        memset(&plan, 0xa5, sizeof plan);
        CHECK_EQ(tb_bt_clock_plan(kBauds[i], &plan), 0);
        CHECK_EQ(plan.baud, kBauds[i]);
        CHECK_EQ(plan.rate, kRates[i]);
        /* Independent recomputation: 300 MHz * m / n must be exactly the rate, with 16x oversampling. */
        CHECK_EQ((unsigned long long)kRates[i] * plan.n, 300000000ull * plan.m);
        CHECK_EQ(plan.rate, (uint32_t)(300000000ull * kM[i] / kN[i]));
        CHECK_EQ(plan.clk_div, kDivs[i]);
        CHECK_EQ(plan.pre_div, 1u);
        CHECK_EQ(plan.src, 6u);
    }
    /* 3.2 Mbaud needs exactly 16 x 3.2 M = 51.2 MHz; at 32x oversampling the same plan would be 2. */
    CHECK_EQ(tb_bt_clock_div(51200000u, 3200000u), 1u);
    CHECK_EQ(tb_bt_clock_div(7372800u, 115200u), 4u);
    /* Rates the table does not carry, and nonsense arguments. */
    TbBtClockPlan plan;
    CHECK_EQ(tb_bt_clock_plan(128000u, &plan), -1);
    CHECK_EQ(tb_bt_clock_plan(0u, NULL), -1);
    CHECK_EQ(tb_bt_clock_div(0u, 115200u), 0u);
    CHECK_EQ(tb_bt_clock_div(1000000u, 0u), 0u);
    /* A source slower than the line rate cannot be divided up. */
    CHECK_EQ(tb_bt_clock_div(7372800u, 4000000u), 0u);
}

static void testClockRegs(void)
{
    section("rcg register encoding");
    TbBtClockPlan plan;
    CHECK_EQ(tb_bt_clock_plan(115200u, &plan), 0);
    uint32_t m = 0, n = 0, d = 0, cfg = 0;
    tb_bt_clock_regs(&plan, &m, &n, &d, &cfg);
    /* m = M; n = ~(N - M) & 0xffff; d = ~N & 0xffff (clk-rcg2.c __clk_rcg2_configure_mnd). */
    CHECK_EQ(m, 384u);
    CHECK_EQ(n, (uint32_t)(~(15625u - 384u) & 0xffffu));
    CHECK_EQ(n, 0xc476u);
    CHECK_EQ(d, (uint32_t)(~15625u & 0xffffu));
    CHECK_EQ(d, 0xc2f6u);
    /* pre_div 1 | src 6 << 8 | DUAL_EDGE (2) << 12. */
    CHECK_EQ(cfg, 0x2601u);

    CHECK_EQ(tb_bt_clock_plan(3200000u, &plan), 0);
    tb_bt_clock_regs(&plan, &m, &n, &d, &cfg);
    CHECK_EQ(m, 64u);
    CHECK_EQ(n, (uint32_t)(~(375u - 64u) & 0xffffu));
    CHECK_EQ(d, (uint32_t)(~375u & 0xffffu));
    CHECK_EQ(cfg, 0x2601u);

    CHECK_EQ(tb_bt_clock_plan(4000000u, &plan), 0);
    tb_bt_clock_regs(&plan, &m, &n, &d, &cfg);
    CHECK_EQ(m, 16u);
    CHECK_EQ(d, (uint32_t)(~75u & 0xffffu));
}

static void testClockProgram(void)
{
    section("rcg program sequence");
    memset(gGcc, 0, sizeof gGcc);
    /* The branch vote must be visible afterwards, and both halt registers are fake-read as voted. */
    gGcc[TB_BT_BRANCH_S6_HALT / 4] = 1u << 31;
    gGcc[TB_BT_BRANCH_CORE_HALT / 4] = 1u << 31;
    TbBtClockEvidence evidence;
    memset(&evidence, 0, sizeof evidence);
    CHECK(tb_bt_clock_vote(&kGccIo, true, &evidence));
    CHECK_EQ(evidence.branch_after & (1u << 10), 1u << 10);
    CHECK_EQ(evidence.branch_after & 1u, 1u);
    CHECK_EQ((gGcc[TB_BT_BRANCH_REG / 4] & ((1u << 10) | 1u)), ((1u << 10) | 1u));

    TbBtClockPlan plan;
    CHECK_EQ(tb_bt_clock_plan(115200u, &plan), 0);
    TbBtClockEvidence program;
    memset(&program, 0, sizeof program);
    CHECK_EQ(tb_bt_clock_program(&kGccIo, &plan, &program), 0);
    /* SE_CMD_DFS must end with DFS off. */
    CHECK_EQ(gGcc[TB_BT_RCG_DFS_OFF / 4] & 1u, 0u);
    /* M/N/D are written with the 16-bit mnd field, and CFG carries pre-div, source and dual edge. */
    CHECK_EQ(gGcc[(TB_BT_RCG_OFF + 0x8u) / 4], 384u);
    CHECK_EQ(gGcc[(TB_BT_RCG_OFF + 0xcu) / 4], 0xc476u);
    CHECK_EQ(gGcc[(TB_BT_RCG_OFF + 0x10u) / 4], 0xc2f6u);
    const uint32_t cfg = gGcc[(TB_BT_RCG_OFF + 0x4u) / 4];
    CHECK_EQ(cfg & 0x3fffu, 0x2601u);
    CHECK_EQ(cfg & (1u << 20), 0u);
    /* CMD_UPDATE was raised and self-cleared, so the evidence reads a settled register. */
    CHECK_EQ(program.cmd & 1u, 0u);
    CHECK_EQ(program.cfg, cfg);

    /* A register block that never clears CMD_UPDATE must fail the stage rather than hang it: the
     * poll bound in Tb710fuBluetoothClock.c is 500 x 1 us, and the fake here always settles. */
    memset(gGcc, 0, sizeof gGcc);
    CHECK_EQ(tb_bt_clock_program(&kGccIo, &plan, NULL), 0);
}

/* ==================================================================================================
 * GENI FIFO UART: a fake serial engine with depth from HW_PARAM_0, a TX FIFO that consumes exactly TX
 * TRANS_LEN bytes, an RX FIFO that can end a burst mid-word, and a CTS line.
 * ================================================================================================ */

#define FAKE_REGS 0x1000u

struct GeniFake {
    uint32_t reg[FAKE_REGS];
    uint8_t tx[4096];
    uint32_t tx_len;
    uint32_t tx_have;
    uint8_t rx[4096];
    uint32_t rx_bytes;
    uint32_t rx_read;
    bool rx_last_burst;
    bool cts_asserted;
    uint32_t reads;
    uint32_t writes;
};

static struct GeniFake gFake;

static uint32_t fakeRead(void *ctx, uint32_t offset)
{
    struct GeniFake *f = ctx;
    f->reads++;
    switch (offset) {
    case TB_GENI_TX_FIFO_STATUS:
        return 0u; /* the fake transmitter drains every word immediately */
    case TB_GENI_RX_FIFO_STATUS: {
        const uint32_t left = f->rx_bytes - f->rx_read;
        uint32_t words = (left + 3u) / 4u;
        uint32_t status = words;
        if (f->rx_last_burst && words) {
            status |= 1u << 31; /* RX_LAST */
            const uint32_t tail = left % 4u;
            status |= (tail ? tail : 0u) << 28; /* RX_LAST_BYTE, 0 means a whole word */
        }
        return status;
    }
    case TB_GENI_RX_FIFO: {
        uint32_t word = 0;
        for (unsigned b = 0; b < 4u; ++b) {
            if (f->rx_read < f->rx_bytes) word |= (uint32_t)f->rx[f->rx_read++] << (8u * b);
        }
        return word;
    }
    case TB_GENI_IOS:
        return f->cts_asserted ? 0u : TB_IO2_DATA_IN; /* 0 bit = CTS asserted */
    default:
        if (offset / 4u < FAKE_REGS) return f->reg[offset / 4u];
        return 0u;
    }
}

static void fakeWrite(void *ctx, uint32_t offset, uint32_t value)
{
    struct GeniFake *f = ctx;
    f->writes++;
    switch (offset) {
    case TB_GENI_TX_FIFO:
        for (unsigned b = 0; b < 4u && f->tx_have < f->tx_len && f->tx_have < sizeof f->tx; ++b)
            f->tx[f->tx_have++] = (uint8_t)(value >> (8u * b));
        if (f->tx_have >= f->tx_len) f->reg[TB_GENI_M_IRQ_STATUS / 4] |= TB_M_CMD_DONE;
        return;
    case TB_GENI_M_CMD0:
        f->reg[offset / 4u] = value;
        if (((value >> 27) & 0x1fu) == TB_UART_START_TX) {
            f->tx_len = f->reg[TB_UART_TX_TRANS_LEN / 4u];
            f->tx_have = 0;
            f->reg[TB_GENI_M_IRQ_STATUS / 4] = 0;
            if (!f->tx_len) f->reg[TB_GENI_M_IRQ_STATUS / 4] = TB_M_CMD_DONE;
        }
        return;
    case TB_GENI_S_CMD0:
        f->reg[offset / 4u] = value;
        f->reg[TB_GENI_STATUS / 4u] |= TB_S_GENI_CMD_ACTIVE;
        return;
    case TB_GENI_S_CMD_CTRL:
        if (value & TB_S_GENI_CMD_CANCEL) f->reg[TB_GENI_S_IRQ_STATUS / 4u] |= TB_S_CMD_CANCEL;
        if (value & TB_S_GENI_CMD_ABORT) f->reg[TB_GENI_S_IRQ_STATUS / 4u] |= TB_S_CMD_ABORT;
        return;
    case TB_GENI_M_IRQ_CLEAR:
        f->reg[TB_GENI_M_IRQ_STATUS / 4u] &= ~value;
        return;
    case TB_GENI_S_IRQ_CLEAR:
        f->reg[TB_GENI_S_IRQ_STATUS / 4u] &= ~value;
        return;
    case TB_GENI_M_CMD_CTRL:
        if (value & TB_M_GENI_CMD_CANCEL) f->reg[TB_GENI_M_IRQ_STATUS / 4] |= TB_M_CMD_CANCEL;
        if (value & TB_M_GENI_CMD_ABORT) f->reg[TB_GENI_M_IRQ_STATUS / 4] |= TB_M_CMD_ABORT;
        return;
    default:
        if (offset / 4u < FAKE_REGS) f->reg[offset / 4u] = value;
        return;
    }
}

static uint64_t fakeNow(void *ctx)
{
    (void)ctx;
    static uint64_t ticks;
    ticks += 1000u; /* every poll advances 1 us, so the bounded loops always terminate */
    return ticks;
}

static void fakeDelay(void *ctx, uint32_t us)
{
    (void)ctx;
    (void)us;
}

static void fakeBarrier(void *ctx) { (void)ctx; }

static void fakeNote(void *ctx, const char *what, uint32_t a, uint32_t b)
{
    (void)ctx;
    (void)what;
    (void)a;
    (void)b;
}

static const TbBtGeniOps kFakeOps = {fakeRead, fakeWrite, fakeBarrier, fakeDelay, fakeNow, fakeNote, &gFake};

static void resetFake(struct GeniFake *f)
{
    memset(f, 0, sizeof *f);
    /* HW_PARAM_0: rx width is not decoded here, tx width 32, tx depth 16. */
    f->reg[TB_GENI_HW_PARAM_0 / 4] = (32u << 24) | (16u << 16);
    f->reg[TB_GENI_HW_PARAM_1 / 4] = 16u << 16;
    f->reg[TB_GENI_FW_REVISION_RO / 4] = TB_FW_REV_PROTOCOL_UART << TB_FW_REV_PROTOCOL_SHIFT;
    f->reg[TB_GENI_STATUS / 4] = TB_S_GENI_CMD_ACTIVE; /* start_rx sees an active read sequencer */
}

static void testGeniProbe(void)
{
    section("geni probe and configuration");
    resetFake(&gFake);
    TbBtGeni geni;
    memset(&geni, 0, sizeof geni);
    CHECK_EQ(tb_bt_geni_probe(&geni, &kFakeOps), TB_BT_GENI_OK);
    CHECK_EQ(geni.proto, TB_FW_REV_PROTOCOL_UART);
    CHECK_EQ(geni.tx_depth_words, 16u);
    CHECK_EQ(geni.tx_width_bits, 32u);
    CHECK_EQ(geni.rx_depth_words, 16u);

    geni.baud = TB_BT_INIT_SPEED;
    geni.clk_rate = 7372800u;
    geni.clk_div = 4u;
    CHECK_EQ(tb_bt_geni_set_rate(&geni, TB_BT_INIT_SPEED, 7372800u, 4u), TB_BT_GENI_OK);
    CHECK_EQ(tb_bt_geni_config(&geni), TB_BT_GENI_OK);
    CHECK(geni.configured);
    CHECK_EQ(geni.flow, false);
    /* FIFO mode: SE DMA off, GSI events off, packing 4 x 8-bit in both directions, 10-bit characters. */
    CHECK_EQ(gFake.reg[TB_GENI_DMA_MODE_EN / 4] & 1u, 0u);
    CHECK_EQ(gFake.reg[TB_GENI_GSI_EVENT_EN / 4], 0u);
    CHECK_EQ(gFake.reg[TB_GENI_TX_PACK_0 / 4], (0x0eu | (0x10eu << 10)));
    CHECK_EQ(gFake.reg[TB_GENI_TX_PACK_1 / 4], (0x20eu | (0x30fu << 10)));
    CHECK_EQ(gFake.reg[TB_GENI_RX_PACK_0 / 4], (0x0eu | (0x10eu << 10)));
    CHECK_EQ(gFake.reg[TB_GENI_BYTE_GRAN / 4], 0u);
    CHECK_EQ(gFake.reg[TB_UART_TX_WORD_LEN / 4], 10u);
    CHECK_EQ(gFake.reg[TB_UART_RX_WORD_LEN / 4], 10u);
    CHECK_EQ(gFake.reg[TB_UART_RX_STALE_CNT / 4], 160u);
    CHECK_EQ(gFake.reg[TB_GENI_RX_RFR_WATERMARK / 4], 14u); /* rx_depth - 2 */
    CHECK_EQ(gFake.reg[TB_UART_TX_TRANS_CFG / 4] & TB_UART_CTS_MASK, TB_UART_CTS_MASK);
    CHECK_EQ(gFake.reg[TB_UART_MANUAL_RFR / 4], TB_UART_MANUAL_RFR_EN | TB_UART_RFR_NOT_READY);
    CHECK_EQ(gFake.reg[TB_GENI_SE_IRQ_EN / 4], 0u); /* no interrupt is ever armed */
    CHECK_EQ(gFake.reg[TB_GENI_OUTPUT_CTRL / 4], 0x7fu);
    CHECK_EQ(gFake.reg[TB_GENI_FORCE_DEFAULT / 4], 1u);

    /* SER clock dividers: CLK_EN plus clk_div in bits 11:4. */
    CHECK_EQ(gFake.reg[TB_GENI_SER_M_CLK_CFG / 4], TB_SER_CLK_EN | (4u << 4));
    CHECK_EQ(gFake.reg[TB_GENI_SER_S_CLK_CFG / 4], TB_SER_CLK_EN | (4u << 4));

    /* An SE with no protocol firmware must be refused, not programmed. */
    resetFake(&gFake);
    gFake.reg[TB_GENI_FW_REVISION_RO / 4] = TB_FW_REV_PROTOCOL_INVALID << TB_FW_REV_PROTOCOL_SHIFT;
    CHECK_EQ(tb_bt_geni_probe(&geni, &kFakeOps), TB_BT_GENI_OK);
    CHECK_EQ(geni.proto, TB_FW_REV_PROTOCOL_INVALID);
    CHECK_EQ(tb_bt_geni_config(&geni), TB_BT_GENI_ERROR);
}

static void testGeniWriteRead(void)
{
    section("geni fifo traffic");
    resetFake(&gFake);
    TbBtGeni geni;
    memset(&geni, 0, sizeof geni);
    CHECK_EQ(tb_bt_geni_probe(&geni, &kFakeOps), TB_BT_GENI_OK);
    geni.baud = TB_BT_INIT_SPEED;
    geni.clk_rate = 7372800u;
    geni.clk_div = 4u;
    CHECK_EQ(tb_bt_geni_set_rate(&geni, TB_BT_INIT_SPEED, 7372800u, 4u), TB_BT_GENI_OK);
    CHECK_EQ(tb_bt_geni_config(&geni), TB_BT_GENI_OK);

    /* A 300-byte H4 packet, longer than the 16-word FIFO, must arrive byte for byte. */
    uint8_t packet[300];
    for (unsigned i = 0; i < sizeof packet; ++i) packet[i] = (uint8_t)(i & 0xffu);
    gFake.tx_len = 0;
    CHECK_EQ(tb_bt_geni_write(&geni, packet, sizeof packet), (int)sizeof packet);
    CHECK_EQ(gFake.tx_len, sizeof packet);
    CHECK_EQ(gFake.tx_have, sizeof packet);
    CHECK(memcmp(gFake.tx, packet, sizeof packet) == 0);
    CHECK_EQ(gFake.reg[TB_UART_TX_TRANS_LEN / 4], sizeof packet);
    CHECK_EQ((gFake.reg[TB_GENI_M_CMD0 / 4] >> TB_M_OPCODE_SHIFT) & 0x1fu, TB_UART_START_TX);

    /* A burst can end mid-word: LAST with LAST_BYTE 3 must deliver exactly three bytes of the last word. */
    resetFake(&gFake);
    CHECK_EQ(tb_bt_geni_probe(&geni, &kFakeOps), TB_BT_GENI_OK);
    geni.baud = TB_BT_INIT_SPEED;
    geni.clk_rate = 7372800u;
    geni.clk_div = 4u;
    CHECK_EQ(tb_bt_geni_set_rate(&geni, TB_BT_INIT_SPEED, 7372800u, 4u), TB_BT_GENI_OK);
    CHECK_EQ(tb_bt_geni_config(&geni), TB_BT_GENI_OK);
    gFake.rx[0] = 0x11u;
    gFake.rx[1] = 0x22u;
    gFake.rx[2] = 0x33u;
    gFake.rx[3] = 0x44u;
    gFake.rx[4] = 0x55u;
    gFake.rx[5] = 0x66u;
    gFake.rx[6] = 0x77u;
    gFake.rx_bytes = 7u;
    gFake.rx_last_burst = true;
    uint8_t got[16];
    uint32_t have = 0;
    CHECK_EQ(tb_bt_geni_read(&geni, got, sizeof got, &have), TB_BT_GENI_OK);
    CHECK_EQ(have, 7u);
    CHECK(memcmp(got, gFake.rx, 7) == 0);
    CHECK_EQ(got[6], 0x77u);

    /* A buffer smaller than the FIFO reports the overflow instead of silently dropping. */
    resetFake(&gFake);
    CHECK_EQ(tb_bt_geni_probe(&geni, &kFakeOps), TB_BT_GENI_OK);
    geni.baud = TB_BT_INIT_SPEED;
    geni.clk_rate = 7372800u;
    geni.clk_div = 4u;
    CHECK_EQ(tb_bt_geni_set_rate(&geni, TB_BT_INIT_SPEED, 7372800u, 4u), TB_BT_GENI_OK);
    CHECK_EQ(tb_bt_geni_config(&geni), TB_BT_GENI_OK);
    memset(gFake.rx, 0xab, 20u);
    gFake.rx_bytes = 20u;
    uint8_t small[8];
    have = 0;
    CHECK_EQ(tb_bt_geni_read(&geni, small, sizeof small, &have), TB_BT_GENI_OVERFLOW);
    CHECK_EQ(have, 8u);

    /* Flow control: masking CTS writes the manual RFR and sets the mask; releasing clears both. */
    CHECK_EQ(tb_bt_geni_set_flow(&geni, false), TB_BT_GENI_OK);
    CHECK_EQ(gFake.reg[TB_UART_TX_TRANS_CFG / 4] & TB_UART_CTS_MASK, TB_UART_CTS_MASK);
    CHECK_EQ(gFake.reg[TB_UART_MANUAL_RFR / 4], TB_UART_MANUAL_RFR_EN | TB_UART_RFR_NOT_READY);
    CHECK_EQ(tb_bt_geni_set_flow(&geni, true), TB_BT_GENI_OK);
    CHECK_EQ(gFake.reg[TB_UART_TX_TRANS_CFG / 4] & TB_UART_CTS_MASK, 0u);
    CHECK_EQ(gFake.reg[TB_UART_MANUAL_RFR / 4], 0u);
    gFake.cts_asserted = true;
    CHECK_EQ(tb_bt_geni_cts(&geni), true);
    gFake.cts_asserted = false;
    CHECK_EQ(tb_bt_geni_cts(&geni), false);

    /* The rate switch rewrites both dividers around a stopped reader. */
    CHECK_EQ(tb_bt_geni_set_rate(&geni, 3200000u, 51200000u, 1u), TB_BT_GENI_OK);
    CHECK_EQ(geni.baud, 3200000u);
    CHECK_EQ(geni.clk_div, 1u);
    CHECK_EQ(gFake.reg[TB_GENI_SER_M_CLK_CFG / 4], TB_SER_CLK_EN | (1u << 4));
    CHECK_EQ(gFake.reg[TB_GENI_SER_S_CLK_CFG / 4], TB_SER_CLK_EN | (1u << 4));
}

/* ==================================================================================================
 * HCI H4: packet construction, Command Complete / Command Status decoding, and the streaming parser
 * (split packets, back-to-back packets, junk bytes, oversized events).
 * ================================================================================================ */

static void testHciPackets(void)
{
    section("hci h4 packets");
    uint8_t packet[300];
    /* HCI Reset: 01 03 0C 00. */
    CHECK_EQ(tb_bt_hci_command_packet(TB_BT_OP_RESET, NULL, 0u, packet, sizeof packet), 4u);
    CHECK_EQ(packet[0], TB_BT_H4_CMD);
    CHECK_EQ(packet[1], 0x03u);
    CHECK_EQ(packet[2], 0x0cu);
    CHECK_EQ(packet[3], 0x00u);

    /* 0xFC00 / 0x1E with a 2-byte header and 243 bytes of payload: 247 bytes after the type byte. */
    uint8_t params[245];
    params[0] = TB_BT_QCA_SEG_SUBOP;
    params[1] = 243u;
    for (unsigned i = 0; i < 243u; ++i) params[2 + i] = (uint8_t)i;
    CHECK_EQ(tb_bt_hci_command_packet(TB_BT_QCA_PATCH_OPCODE, params, 245u, packet, sizeof packet), 249u);
    CHECK_EQ(packet[1], 0x00u);
    CHECK_EQ(packet[2], 0xfcu);
    CHECK_EQ(packet[3], 245u);
    CHECK_EQ(packet[4], TB_BT_QCA_SEG_SUBOP);
    CHECK_EQ(packet[5], 243u);

    /* A buffer that cannot hold the packet is refused rather than truncated. */
    CHECK_EQ(tb_bt_hci_command_packet(TB_BT_QCA_PATCH_OPCODE, params, 245u, packet, 100u), 0u);
    CHECK_EQ(tb_bt_hci_command_packet(TB_BT_OP_RESET, NULL, 5u, packet, sizeof packet), 0u);
}

static void testHciParseReply(void)
{
    section("command complete and status decoding");
    /* Command Complete: 0e 0c 01 03 0c 00 00 ... (ncmd 1, opcode 0x0C03, status 0). */
    uint8_t cc[14] = {TB_BT_EVT_CMD_COMPLETE, 12u, 1u, 0x03u, 0x0cu, 0x00u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    uint16_t opcode = 0;
    uint8_t status = 0xffu;
    const uint8_t *rest = NULL;
    uint32_t rest_len = 0;
    CHECK(tb_bt_hci_parse_cmd_complete(cc, sizeof cc, &opcode, &status, &rest, &rest_len));
    CHECK_EQ(opcode, TB_BT_OP_RESET);
    CHECK_EQ(status, 0u);
    CHECK_EQ(rest_len, 8u); /* nparams 12 minus ncmd, opcode and the status byte */
    CHECK_EQ(rest, cc + 6);

    /* A Command Complete whose status is non-zero must be reported as such, not as an error. */
    cc[5] = 0x12u;
    CHECK(tb_bt_hci_parse_cmd_complete(cc, sizeof cc, &opcode, &status, NULL, NULL));
    CHECK_EQ(status, 0x12u);

    /* Truncated bodies and the wrong event code are rejected. */
    CHECK(!tb_bt_hci_parse_cmd_complete(cc, 5u, &opcode, &status, NULL, NULL));
    cc[0] = TB_BT_EVT_CMD_STATUS;
    CHECK(!tb_bt_hci_parse_cmd_complete(cc, sizeof cc, &opcode, &status, NULL, NULL));

    /* Command Status: 0f 04 00 01 03 0c (status 0, ncmd 1, opcode 0x0C03). */
    uint8_t cs[8] = {TB_BT_EVT_CMD_STATUS, 4u, 0x00u, 0x01u, 0x03u, 0x0cu, 0u, 0u};
    opcode = 0;
    status = 0xffu;
    CHECK(tb_bt_hci_parse_cmd_status(cs, sizeof cs, &opcode, &status));
    CHECK_EQ(opcode, TB_BT_OP_RESET);
    CHECK_EQ(status, 0u);
    cs[2] = 0x1au; /* "unsupported command" */
    CHECK(tb_bt_hci_parse_cmd_status(cs, sizeof cs, &opcode, &status));
    CHECK_EQ(status, 0x1au);
    CHECK(!tb_bt_hci_parse_cmd_status(cs, 4u, &opcode, &status));
}

struct SinkLog {
    unsigned packets;
    uint8_t type[8];
    uint32_t len[8];
    uint8_t body[8][64];
    unsigned drops;
};

static void sinkRecord(void *ctx, uint8_t type, const uint8_t *body, uint32_t len)
{
    struct SinkLog *log = ctx;
    if (log->packets < 8u) {
        log->type[log->packets] = type;
        log->len[log->packets] = len;
        if (len > 64u) len = 64u;
        memcpy(log->body[log->packets], body, len);
    }
    log->packets++;
}

static void testHciParser(void)
{
    section("h4 streaming parser");
    TbBtH4Parser parser;
    struct SinkLog log;
    memset(&log, 0, sizeof log);
    tb_bt_h4_parser_init(&parser, sinkRecord, &log);

    /* Two events fed one byte at a time must both be delivered intact. */
    uint8_t cc[15] = {TB_BT_H4_EVT, TB_BT_EVT_CMD_COMPLETE, 12u, 1u, 0x03u, 0x0cu, 0x00u,
                      0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    uint8_t cs[7] = {TB_BT_H4_EVT, TB_BT_EVT_CMD_STATUS, 4u, 0x00u, 0x01u, 0x03u, 0x0cu};
    for (unsigned i = 0; i < sizeof cc; ++i) tb_bt_h4_parser_feed(&parser, cc + i, 1u);
    CHECK_EQ(log.packets, 1u);
    CHECK_EQ(log.type[0], TB_BT_H4_EVT);
    CHECK_EQ(log.len[0], 14u); /* event code + plen + 12 params */
    CHECK_EQ(log.body[0][0], TB_BT_EVT_CMD_COMPLETE);
    for (unsigned i = 0; i < sizeof cs; ++i) tb_bt_h4_parser_feed(&parser, cs + i, 1u);
    CHECK_EQ(log.packets, 2u);
    CHECK_EQ(log.body[1][0], TB_BT_EVT_CMD_STATUS);

    /* Junk before the next packet must be skipped without losing the packet that follows. */
    const uint8_t junk[] = {0x00u, 0xffu, 0x0bu, 0x01u, 0x03u, 0x0cu, 0x00u};
    tb_bt_h4_parser_feed(&parser, junk, sizeof junk);
    CHECK_EQ(parser.drops, 3u); /* three bytes that are not H4 type bytes */
    CHECK_EQ(log.packets, 3u); /* the 01 03 0c 00 tail is a well-formed command packet */
    CHECK_EQ(log.type[2], TB_BT_H4_CMD);
    CHECK_EQ(log.len[2], 3u);
    CHECK_EQ(log.body[2][2], 0x00u);

    /* An ACL header whose data length exceeds the buffer is dropped and counted, not split. */
    const uint8_t huge[] = {TB_BT_H4_ACL, 0x00u, 0x01u, 0xffu, 0xffu};
    tb_bt_h4_parser_feed(&parser, huge, sizeof huge);
    CHECK_EQ(parser.drops, 4u);
    CHECK_EQ(parser.in_packet, false);
}

/* ==================================================================================================
 * QCA: the TLV header, the patch header, NVM tag walking and rewriting, and the download order. The
 * byte strings below are the real first bytes of the board's hmtbtfw20.tlv / hmtnv20.bin (the MD5SUMS
 * file in input-sources lists them; the values are repeated in Tb710fuBluetoothBoard.h).
 * ================================================================================================ */

static const uint8_t kPatchHead[28] = {
    /* tlv_type_hdr: type 1, length 274124 (0x42ecc) in the upper 24 bits: value 0x042ecc01 */
    0x01u, 0xccu, 0x2eu, 0x04u,
    /* total_size 274092 (0x42eac) */
    0xacu, 0x2eu, 0x04u, 0x00u,
    /* data_length 274056 */
    0x88u, 0x2eu, 0x04u, 0x00u,
    /* format_version 1, signature 0, download_mode 3, reserved1 0 */
    0x01u, 0x00u, 0x03u, 0x00u,
    /* product_id 0x0019, rom_build 0x0200 */
    0x19u, 0x00u, 0x00u, 0x02u,
    /* patch_version 0x80d2 (the board's hmtbtfw20.tlv), reserved2 0 */
    0xd2u, 0x80u, 0x00u, 0x00u,
    /* entry 0 */
    0x00u, 0x00u, 0x00u, 0x00u,
};

static void testQcaTlv(void)
{
    section("qca tlv and patch header");
    TbBtQcaTlv tlv;
    memset(&tlv, 0, sizeof tlv);
    CHECK_EQ(tb_bt_qca_tlv_parse(kPatchHead, sizeof kPatchHead, &tlv), -1); /* declared length > bytes given */
    CHECK_EQ(tlv.type, TB_BT_QCA_TLV_PATCH);
    CHECK_EQ(tlv.length, 274124u);

    TbBtQcaPatch patch;
    memset(&patch, 0, sizeof patch);
    CHECK_EQ(tb_bt_qca_patch_parse(kPatchHead, sizeof kPatchHead, &patch), -1);
    CHECK_EQ(patch.total_size, 274092u);
    CHECK_EQ(patch.data_length, 274056u);
    CHECK_EQ(patch.format_version, 1u);
    CHECK_EQ(patch.signature, 0u);
    CHECK_EQ(patch.download_mode, TB_BT_QCA_DNLD_MODE_VSE_CC);
    CHECK_EQ(patch.product_id, 0x0019u);
    CHECK_EQ(patch.rom_build, 0x0200u);
    CHECK_EQ(patch.patch_version, 0x80d2u);
    CHECK_EQ(patch.entry, 0u);
    CHECK_EQ(tb_bt_qca_get_u32(kPatchHead), 0x042ecc01u);

    /* The file sizes the board ships: 274128 and 9592 bytes, 243 bytes a segment. */
    CHECK_EQ(tb_bt_qca_segment_count(274128u), 1129u);
    CHECK_EQ(tb_bt_qca_segment_count(9592u), 40u);
    CHECK_EQ(tb_bt_qca_segment_count(243u), 1u);
    CHECK_EQ(tb_bt_qca_segment_count(244u), 2u);
    CHECK_EQ(tb_bt_qca_segment_count(0u), 0u);

    /* hmtnv20.bin begins with type 2 and length 9588 (4 + 9588 = 9592). */
    const uint8_t nvmHead[8] = {0x02u, 0x74u, 0x25u, 0x00u, 0x01u, 0x00u, 0x40u, 0x00u};
    CHECK_EQ(tb_bt_qca_tlv_parse(nvmHead, sizeof nvmHead, &tlv), -1);
    CHECK_EQ(tlv.type, TB_BT_QCA_TLV_NVM);
    CHECK_EQ(tlv.length, 9588u);
    /* First tag: id 1, length 64, which is the "BTFW.HAMILTON..." radio string. */
    CHECK_EQ(tb_bt_qca_get_u16(nvmHead + 4), 1u);
    CHECK_EQ(tb_bt_qca_get_u16(nvmHead + 6), 64u);
}

static void testQcaNvmRewrite(void)
{
    section("qca nvm tag rewriting");
    /* A synthetic image with the three tags btqca edits: HCI (17), deep sleep (27) and BD_ADDR (2). */
    uint8_t image[128];
    memset(image, 0, sizeof image);
    const uint32_t body = 4u + 8u + 3u + 13u + 18u; /* hdr + tag17 + tag27 + tag2 */
    image[0] = TB_BT_QCA_TLV_NVM;
    image[1] = (uint8_t)(body >> 0);
    image[2] = (uint8_t)(body >> 8);
    image[3] = 0u;
    uint32_t at = 4u;
    /* tag 17, len 3 */
    image[at + 0] = TB_BT_QCA_TAG_HCI;
    image[at + 1] = 0u;
    image[at + 2] = 3u;
    image[at + 3] = 0u;
    image[at + 12] = 0x40u; /* data[0]: inband sleep off */
    image[at + 13] = 0x00u; /* data[1]: baud code, to be overwritten */
    image[at + 14] = 0x1eu;
    at += 12u + 3u;
    /* tag 27, len 1 */
    image[at + 0] = TB_BT_QCA_TAG_DEEP_SLEEP;
    image[at + 2] = 1u;
    image[at + 12] = 0x02u;
    at += 12u + 1u;
    /* tag 2, len 6: BD_ADDR */
    image[at + 0] = TB_BT_QCA_TAG_BD_ADDR;
    image[at + 2] = 6u;
    for (unsigned i = 0; i < 6u; ++i) image[at + 12u + i] = (uint8_t)(0x10u + i);
    at += 12u + 6u;
    CHECK_EQ(at, 4u + body);

    TbBtQcaNvm nvm;
    memset(&nvm, 0, sizeof nvm);
    CHECK_EQ(tb_bt_qca_nvm_prepare(image, 4u + body, TB_BT_QCA_BAUD_3200000, &nvm), 0);
    CHECK_EQ(nvm.tags, 3u);
    CHECK_EQ(nvm.tag_hci, 1u);
    CHECK_EQ(nvm.tag_deep_sleep, 1u);
    CHECK_EQ(nvm.tag_bd_addr, 1u);
    CHECK_EQ(nvm.baud_code_written, TB_BT_QCA_BAUD_3200000);
    CHECK(nvm.bd_addr_present);
    CHECK_EQ(nvm.bd_addr[0], 0x10u);
    CHECK_EQ(nvm.bd_addr[5], 0x15u);
    /* data[0] |= 0x80 on the HCI tag, data[1] = baud code, data[0] |= 0x01 on the deep-sleep tag. */
    CHECK_EQ(image[4u + 12u] & 0x80u, 0x80u);
    CHECK_EQ(image[4u + 13u], TB_BT_QCA_BAUD_3200000);
    CHECK_EQ(image[4u + 15u + 12u] & 1u, 1u);

    /* A truncated tag set is refused instead of being downloaded half-written. */
    CHECK_EQ(tb_bt_qca_nvm_prepare(image, 4u + body - 2u, 17u, &nvm), -1);
    /* Not an NVM TLV at all. */
    uint8_t patchy[8] = {0x01u, 0x04u, 0x00u, 0x00u, 0u, 0u, 0u, 0u};
    CHECK_EQ(tb_bt_qca_nvm_prepare(patchy, sizeof patchy, 17u, &nvm), -1);
}

struct QcaFake {
    uint32_t sends;
    uint32_t waits;
    uint32_t last_opcode;
    uint8_t last_params[8];
    uint8_t last_plen;
    uint32_t segments;
    uint32_t waits_on_segment;
    uint32_t first_segment_len;
    uint32_t last_segment_len;
    uint8_t version_reply[16];
    uint32_t version_len;
};

static int fakeQcaSend(void *ctx, uint16_t opcode, const uint8_t *params, uint8_t plen)
{
    struct QcaFake *f = ctx;
    f->sends++;
    f->last_opcode = opcode;
    f->last_plen = plen;
    for (unsigned i = 0; i < plen && i < sizeof f->last_params; ++i) f->last_params[i] = params[i];
    if (opcode == TB_BT_QCA_PATCH_OPCODE && plen >= 2u && params[0] == TB_BT_QCA_SEG_SUBOP) {
        f->segments++;
        if (f->segments == 1u) f->first_segment_len = params[1];
        f->last_segment_len = params[1];
    }
    return 0;
}

static int fakeQcaWait(void *ctx, uint16_t opcode, TbBtQcaReply *out, uint32_t timeout_ms)
{
    struct QcaFake *f = ctx;
    (void)timeout_ms;
    f->waits++;
    out->kind = TB_BT_QCA_REPLY_CC;
    out->status = 0u;
    if (opcode == TB_BT_QCA_READ_BD_ADDR_OPCODE) {
        for (unsigned i = 0; i < 6u; ++i) out->data[i] = (uint8_t)(0xa0u + i);
        out->len = 6u;
        return 0;
    }
    if (opcode == TB_BT_QCA_PATCH_OPCODE) {
        f->waits_on_segment++;
        switch (f->last_params[0]) {
        case TB_BT_QCA_VER_SUBOP:
        case TB_BT_QCA_BID_SUBOP:
        case TB_BT_QCA_BUILD_INFO_SUBOP:
            for (unsigned i = 0; i < f->version_len && i < sizeof out->data; ++i)
                out->data[i] = f->version_reply[i];
            out->len = f->version_len;
            return 0;
        default:
            out->data[0] = f->last_params[0];
            out->len = 1u;
            return 0;
        }
    }
    out->len = 0u;
    return 0;
}

static void fakeQcaDelay(void *ctx, uint32_t ms)
{
    (void)ctx;
    (void)ms;
}

static struct QcaFake gQca; /* the ops context, so the cores always get a live pointer */

static const TbBtQcaIo kQcaIo = {fakeQcaSend, fakeQcaWait, fakeQcaDelay, NULL, &gQca};

static void testQcaDownloadOrder(void)
{
    section("qca download order");
    /* A 1000-byte patch with download_mode 3: five segments, only the last one is acknowledged. */
    uint8_t patch[1000];
    memset(patch, 0, sizeof patch);
    patch[0] = TB_BT_QCA_TLV_PATCH;
    patch[1] = (uint8_t)((996u >> 0) & 0xffu);
    patch[2] = (uint8_t)((996u >> 8) & 0xffu);
    patch[3] = 0u;
    patch[4 + 0] = 0xe4u; /* total_size 996 */
    patch[4 + 1] = 0x03u;
    patch[4 + 10] = TB_BT_QCA_DNLD_MODE_VSE_CC;
    patch[4 + 12] = 0x19u;
    patch[4 + 16] = 0xd2u; /* patch_version 0x80d2 */
    patch[4 + 17] = 0x80u;

    TbBtQcaStats stats;
    memset(&gQca, 0, sizeof gQca);
    memset(&stats, 0, sizeof stats);
    CHECK_EQ(tb_bt_qca_download_patch(&kQcaIo, patch, sizeof patch, &stats), 0);
    CHECK_EQ(gQca.segments, 5u);           /* 243 + 243 + 243 + 243 + 28 */
    CHECK_EQ(gQca.first_segment_len, 243u);
    CHECK_EQ(gQca.last_segment_len, 28u);
    CHECK_EQ(gQca.waits_on_segment, 1u);   /* only the last segment waits */
    CHECK_EQ(stats.patch_segments, 5u);
    CHECK_EQ(stats.patch_acked, 1u);
    CHECK_EQ(gQca.last_opcode, TB_BT_QCA_PATCH_OPCODE);

    /* download_mode 0 (default) acknowledges every segment. */
    memset(&gQca, 0, sizeof gQca);
    memset(&stats, 0, sizeof stats);
    patch[4 + 10] = 0u;
    CHECK_EQ(tb_bt_qca_download_patch(&kQcaIo, patch, sizeof patch, &stats), 0);
    CHECK_EQ(gQca.waits_on_segment, 5u);
    CHECK_EQ(stats.patch_acked, 5u);

    /* NVM: every segment is acknowledged, and the 243-byte cap holds. */
    uint8_t nvm[400];
    memset(nvm, 0, sizeof nvm);
    nvm[0] = TB_BT_QCA_TLV_NVM;
    nvm[1] = (uint8_t)(396u & 0xffu);
    nvm[2] = (uint8_t)((396u >> 8) & 0xffu);
    memset(&gQca, 0, sizeof gQca);
    memset(&stats, 0, sizeof stats);
    CHECK_EQ(tb_bt_qca_download_nvm(&kQcaIo, nvm, sizeof nvm, &stats), 0);
    CHECK_EQ(gQca.segments, 2u);
    CHECK_EQ(gQca.first_segment_len, 243u);
    CHECK_EQ(gQca.waits_on_segment, 2u);
    CHECK_EQ(stats.nvm_segments, 2u);

    /* The segment command body is opcode 0xFC00, sub-op 0x1E, then the size, then the bytes. */
    CHECK_EQ(gQca.last_opcode, TB_BT_QCA_PATCH_OPCODE);
    CHECK_EQ(gQca.last_params[0], TB_BT_QCA_SEG_SUBOP);
    CHECK_EQ(gQca.last_params[1], 157u); /* 400 - 243 */
}

static void testQcaCommands(void)
{
    section("qca version, board id and baud switch");
    memset(&gQca, 0, sizeof gQca);
    /* Command Complete for 0xFC00/0x19 on WCN7850: rtype, one extra byte, then product/patch/rom/soc. */
    const uint8_t reply[14] = {0x19u, 0x00u, 0x19u, 0x00u, 0x00u, 0x00u, 0xd2u, 0x80u, 0x00u, 0x02u,
                               0x20u, 0x43u, 0x01u, 0x40u};
    memcpy(gQca.version_reply, reply, sizeof reply);
    gQca.version_len = sizeof reply;
    TbBtQcaVersion version;
    memset(&version, 0, sizeof version);
    CHECK_EQ(tb_bt_qca_read_version(&kQcaIo, &version), 0);
    CHECK_EQ(version.parse_offset, 2u);
    CHECK_EQ(version.product_id, 0x00000019u);
    CHECK_EQ(version.patch_version, 0x80d2u);
    CHECK_EQ(version.rom_version, 0x0200u);
    CHECK_EQ(version.soc_id, 0x40014320u);
    CHECK_EQ(gQca.last_opcode, TB_BT_QCA_PATCH_OPCODE);
    CHECK_EQ(gQca.last_params[0], TB_BT_QCA_VER_SUBOP);

    /* The 13-byte reply (rtype then the structure, no extra byte) is the documented fallback. */
    gQca.version_reply[0] = reply[0];
    memcpy(gQca.version_reply + 1, reply + 2, 12u);
    gQca.version_len = 13u;
    memset(&version, 0, sizeof version);
    CHECK_EQ(tb_bt_qca_read_version(&kQcaIo, &version), 0);
    CHECK_EQ(version.parse_offset, 1u);
    CHECK_EQ(version.soc_id, 0x40014320u);

    /* Read_BD_ADDR is the standard 0x1009 and returns six bytes. */
    uint8_t addr[6];
    CHECK_EQ(tb_bt_qca_read_bdaddr(&kQcaIo, addr), 0);
    CHECK_EQ(gQca.last_opcode, TB_BT_QCA_READ_BD_ADDR_OPCODE);
    CHECK_EQ(addr[0], 0xa0u);
    CHECK_EQ(addr[5], 0xa5u);

    /* The vendor write swaps the printed order, as qca_set_bdaddr does. */
    const uint8_t want[6] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u};
    CHECK_EQ(tb_bt_qca_write_bdaddr(&kQcaIo, want), 0);
    CHECK_EQ(gQca.last_opcode, TB_BT_QCA_WRITE_BD_ADDR_OPCODE);
    CHECK_EQ(gQca.last_plen, 6u);
    CHECK_EQ(gQca.last_params[0], 0x66u);
    CHECK_EQ(gQca.last_params[5], 0x11u);

    /* The baud switch is 0xFC48 with the enum value (3.2 Mbaud = 17 for this part). */
    CHECK_EQ(tb_bt_qca_set_baudrate(&kQcaIo, TB_BT_QCA_BAUD_3200000), 0);
    CHECK_EQ(gQca.last_opcode, TB_BT_QCA_SET_BAUD_OPCODE);
    CHECK_EQ(gQca.last_plen, 1u);
    CHECK_EQ(gQca.last_params[0], 17u);

    /* Logging off is 0xFC17 with 0x14, 0x00. */
    CHECK_EQ(tb_bt_qca_disable_logging(&kQcaIo), 0);
    CHECK_EQ(gQca.last_opcode, TB_BT_QCA_DISABLE_LOGGING_OPCODE);
    CHECK_EQ(gQca.last_plen, 2u);
    CHECK_EQ(gQca.last_params[0], TB_BT_QCA_LOGGING_SUBOP);
}

/* ==================================================================================================
 * Power: TLMM control words for the five pins, and the BT_EN pulse order.
 * ================================================================================================ */

static void testPowerPinCtl(void)
{
    section("tlmm pin control words");
    /* BT_EN gpio17: function gpio (0), drive 7, bias-disable, output enable. */
    CHECK_EQ(tb_bt_power_pin_ctl(TB_BT_GPIO_FUNC, TB_BT_BT_EN_DRIVE, TB_BT_BT_EN_BIAS, true),
             (7u << 6) | (1u << 9));
    /* SW_CTRL gpio18: function gpio, drive 0, pull-down, input. */
    CHECK_EQ(tb_bt_power_pin_ctl(TB_BT_GPIO_FUNC, TB_BT_SW_CTRL_DRIVE, TB_BT_SW_CTRL_BIAS, false), 1u);
    /* TX/RX: function qup2_se6 (1) in bits 5:2, drive 0, pull-up. */
    CHECK_EQ(tb_bt_power_pin_ctl(TB_BT_UART_FUNC, TB_BT_UART_DRIVE, TB_BT_UART_TXRX_BIAS, false),
             (1u << 2) | 3u);
    /* CTS/RTS: same function, pull-down. */
    CHECK_EQ(tb_bt_power_pin_ctl(TB_BT_UART_FUNC, TB_BT_UART_DRIVE, TB_BT_UART_CTS_RTS_BIAS, false),
             (1u << 2) | 1u);
}

struct PowerFake {
    uint32_t words[0x300000 / 4];
    uint32_t order[16];
    uint32_t values[16];
    uint32_t order_len;
    uint32_t delays[8];
    uint32_t delay_len;
    uint32_t sw_ctrl_level;
};

static uint32_t pinFakeRead(void *ctx, uint32_t offset)
{
    struct PowerFake *f = ctx;
    if (offset == TB_BT_TLMM_IO(TB_BT_SW_CTRL_GPIO)) return f->sw_ctrl_level;
    return f->words[offset / 4u];
}

static void pinFakeWrite(void *ctx, uint32_t offset, uint32_t value)
{
    struct PowerFake *f = ctx;
    if (f->order_len < sizeof f->order / sizeof f->order[0]) {
        f->order[f->order_len] = offset;
        f->values[f->order_len] = value;
        f->order_len++;
    }
    f->words[offset / 4u] = value;
}

static void pinFakeDelay(void *ctx, uint32_t ms)
{
    struct PowerFake *f = ctx;
    if (f->delay_len < sizeof f->delays / sizeof f->delays[0]) f->delays[f->delay_len++] = ms;
}

static struct PowerFake gPin; /* the ops context */

static const TbBtPowerIo kPowerIo = {pinFakeRead, pinFakeWrite, pinFakeDelay, &gPin};

static void testPowerSequence(void)
{
    section("bt_en pulse");
    struct PowerFake *const fake = &gPin;
    memset(fake, 0, sizeof *fake);
    TbBtPowerState state;
    memset(&state, 0, sizeof state);
    CHECK_EQ(tb_bt_power_on(&kPowerIo, NULL, &state), 0);
    /* Electrical order: IO low, CTL (output enable) for BT_EN, SW_CTRL input, four UART pins, then
     * IO low again, 50 ms, IO high, 50 ms. */
    CHECK_EQ(fake->order_len, 9u);
    CHECK_EQ(fake->order[0], TB_BT_TLMM_IO(TB_BT_BT_EN_GPIO));
    CHECK_EQ(fake->order[1], TB_BT_TLMM_CTL(TB_BT_BT_EN_GPIO));
    CHECK_EQ(fake->order[2], TB_BT_TLMM_CTL(TB_BT_SW_CTRL_GPIO));
    CHECK_EQ(fake->words[TB_BT_TLMM_CTL(TB_BT_BT_EN_GPIO) / 4u], (7u << 6) | (1u << 9));
    CHECK_EQ(fake->words[TB_BT_TLMM_CTL(TB_BT_SW_CTRL_GPIO) / 4u], 1u);
    CHECK_EQ(fake->words[TB_BT_TLMM_CTL(TB_BT_PIN_TX) / 4u], (1u << 2) | 3u);
    CHECK_EQ(fake->words[TB_BT_TLMM_CTL(TB_BT_PIN_RX) / 4u], (1u << 2) | 3u);
    CHECK_EQ(fake->words[TB_BT_TLMM_CTL(TB_BT_PIN_CTS) / 4u], (1u << 2) | 1u);
    CHECK_EQ(fake->words[TB_BT_TLMM_CTL(TB_BT_PIN_RTS) / 4u], (1u << 2) | 1u);
    CHECK_EQ(fake->words[TB_BT_TLMM_IO(TB_BT_BT_EN_GPIO) / 4u], 1u << TB_BT_TLMM_IO_OUT_BIT);
    /* The pulse itself: the enable is taken low again (write 7) and raised after the first 50 ms (write 8). */
    CHECK_EQ(fake->values[0], 0u);
    CHECK_EQ(fake->values[7], 0u);
    CHECK_EQ(fake->values[8], 1u << TB_BT_TLMM_IO_OUT_BIT);
    CHECK_EQ(fake->delay_len, 2u);
    CHECK_EQ(fake->delays[0], 50u);
    CHECK_EQ(fake->delays[1], 50u);
    CHECK_EQ(state.bt_en_high, true);

    fake->sw_ctrl_level = 1u << TB_BT_TLMM_IO_IN_BIT;
    CHECK_EQ(tb_bt_power_on(&kPowerIo, NULL, &state), 0);
    CHECK_EQ(state.sw_ctrl_high, true);
    /* The IO register write for BT_EN high must be the last value left in the register file. */
    CHECK_EQ(fake->words[TB_BT_TLMM_IO(TB_BT_BT_EN_GPIO) / 4u] & (1u << TB_BT_TLMM_IO_OUT_BIT),
             1u << TB_BT_TLMM_IO_OUT_BIT);

    CHECK_EQ(tb_bt_power_off(&kPowerIo, NULL), 0);
    CHECK_EQ(fake->words[TB_BT_TLMM_IO(TB_BT_BT_EN_GPIO) / 4u], 0u);
}

/* ==================================================================================================
 * Board facts: the addresses the driver maps must be the device-tree ones, and the pin numbers must
 * be the BT pin group. These catch the one error no fake register file can: a wrong constant.
 * ================================================================================================ */

static void testBoardFacts(void)
{
    section("board constants");
    CHECK_EQ(TB_BT_SE_PHYS, 0x00898000u); /* sm8650.dtsi:2491 uart14 reg */
    CHECK_EQ(TB_BT_SE_BYTES, 0x4000u);
    CHECK_EQ(TB_BT_WRAPPER_PHYS, 0x008c0000u); /* sm8650.dtsi:2026 geniqup@8c0000 */
    CHECK_EQ(TB_BT_GCC_PHYS, 0x00100000u); /* sm8650.dtsi:1967 clock-controller@100000 */
    CHECK_EQ(TB_BT_TLMM_PHYS, 0x0f100000u); /* sm8650.dtsi:6113 pinctrl@f100000 */
    CHECK_EQ(TB_BT_BT_EN_GPIO, 17u); /* dts:1020 bt-en-pins */
    CHECK_EQ(TB_BT_SW_CTRL_GPIO, 18u); /* dts:1028 sw-ctrl-pins */
    CHECK_EQ(TB_BT_PIN_TX, 26u);
    CHECK_EQ(TB_BT_PIN_RX, 27u);
    CHECK_EQ(TB_BT_PIN_CTS, 24u);
    CHECK_EQ(TB_BT_PIN_RTS, 25u);
    CHECK_EQ(TB_BT_MAX_SPEED, 3200000u); /* dts:1237 max-speed */
    CHECK_EQ(TB_BT_INIT_SPEED, 115200u);
    CHECK_EQ(TB_BT_GCC_RCG_OFF, TB_BT_RCG_OFF); /* the two headers must agree */
    CHECK_EQ(TB_BT_GCC_BRANCH_OFF, TB_BT_BRANCH_REG);
}

int main(void)
{
    printf("TB710FU Bluetooth host checks\n");
    testBoardFacts();
    testClockPlan();
    testClockRegs();
    testClockProgram();
    testGeniProbe();
    testGeniWriteRead();
    testHciPackets();
    testHciParseReply();
    testHciParser();
    testQcaTlv();
    testQcaNvmRewrite();
    testQcaDownloadOrder();
    testQcaCommands();
    testPowerPinCtl();
    testPowerSequence();
    printf("%u checks, %u failures\n", gChecks, gFailures);
    if (gFailures) {
        printf("TB710FU Bluetooth host checks: FAIL\n");
        return 1;
    }
    printf("TB710FU Bluetooth host checks: PASS\n");
    return 0;
}
