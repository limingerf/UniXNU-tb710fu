/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Tb710fuBluetoothClock.h"

typedef struct {
    uint32_t baud;
    uint32_t m;
    uint32_t n;
} TbBtClockEntry;

/* ftbl_gcc_qupv3_wrap2_s6_clk_src, GPLL0 even 300 MHz, pre-div 1 (linux-gcc-sm8650.c:1167-1180), plus the
 * one extra entry the 3.2 Mbaud line needs. Listed by baud so the driver never has to search. */
static const TbBtClockEntry kPlans[] = {
    {115200u, 384u, 15625u},  /* 7,372,800 Hz  (ftbl entry) */
    {921600u, 768u, 15625u},  /* 14,745,600 Hz (ftbl entry) */
    {3000000u, 4u, 25u},      /* 48,000,000 Hz (ftbl entry) */
    {3200000u, 64u, 375u},    /* 51,200,000 Hz (exact M/N of the same parent, not in ftbl) */
    {4000000u, 16u, 75u},     /* 64,000,000 Hz (ftbl entry) */
};

uint32_t tb_bt_clock_div(uint32_t rate, uint32_t baud)
{
    if (!rate || !baud) return 0u;
    const uint64_t need = (uint64_t)baud * 16u; /* QUP >= 2.5 oversampling (qcom_geni_serial) */
    uint64_t div = ((uint64_t)rate + need - 1u) / need;
    if (!div || div > 0xfffu) return 0u;
    if ((uint64_t)div * need > rate) return 0u; /* cannot run faster than the source */
    return (uint32_t)div;
}

int tb_bt_clock_plan(uint32_t baud, TbBtClockPlan *out)
{
    if (!out) return -1;
    for (unsigned i = 0; i < sizeof(kPlans) / sizeof(kPlans[0]); ++i) {
        if (kPlans[i].baud != baud) continue;
        const uint64_t rate64 = (TB_BT_CLK_GPLL0_EVEN_HZ * kPlans[i].m) / kPlans[i].n;
        if (rate64 * kPlans[i].n != TB_BT_CLK_GPLL0_EVEN_HZ * kPlans[i].m) return -1; /* not exact */
        const uint32_t div = tb_bt_clock_div((uint32_t)rate64, baud);
        if (!div) return -1;
        out->baud = baud;
        out->rate = (uint32_t)rate64;
        out->m = kPlans[i].m;
        out->n = kPlans[i].n;
        out->pre_div = TB_BT_RCG_PRE_DIV;
        out->src = TB_BT_CLK_SRC_SEL;
        out->clk_div = div;
        return 0;
    }
    return -1;
}

void tb_bt_clock_regs(const TbBtClockPlan *plan, uint32_t *m_reg, uint32_t *n_reg, uint32_t *d_reg,
                      uint32_t *cfg_reg)
{
    const uint32_t mask = 0xffffu; /* mnd_width 16 */
    const uint32_t n_minus_m = plan->n - plan->m;
    uint32_t d = plan->n;
    const uint32_t hi = 2u * n_minus_m;
    if (d > hi) d = hi;            /* clamp_t(u32, n, m, 2 * (n - m)) */
    if (d < plan->m) d = plan->m;
    if (m_reg) *m_reg = plan->m & mask;
    if (n_reg) *n_reg = (~n_minus_m) & mask;
    if (d_reg) *d_reg = (~d) & mask;
    if (cfg_reg) {
        /* div field bits 7:0, src bits 10:8, mode bits 13:12, hardware clock control bit 20 cleared. */
        *cfg_reg = (plan->pre_div & 0xffu) | ((plan->src & 0x7u) << 8) |
                   ((TB_BT_CLK_MODE_DUAL_EDGE & 0x3u) << 12);
    }
}

bool tb_bt_clock_vote(const TbBtClockIo *io, bool enable, TbBtClockEvidence *evidence)
{
    if (!io || !io->read || !io->write) return false;
    const uint32_t bits = (1u << TB_BT_BRANCH_S6_BIT) | (1u << TB_BT_BRANCH_CORE_BIT);
    uint32_t v = io->read(io->ctx, TB_BT_BRANCH_REG);
    if (evidence) evidence->branch_before = v;
    v = enable ? (v | bits) : (v & ~bits);
    io->write(io->ctx, TB_BT_BRANCH_REG, v);
    v = io->read(io->ctx, TB_BT_BRANCH_REG);
    if (evidence) evidence->branch_after = v;

    const uint32_t h_s6 = io->read(io->ctx, TB_BT_BRANCH_S6_HALT);
    const uint32_t h_core = io->read(io->ctx, TB_BT_BRANCH_CORE_HALT);
    if (evidence) {
        evidence->halt_s6 = h_s6;
        evidence->halt_core = h_core;
    }
    if (!enable) return true;
    /* BRANCH_HALT_VOTED: once the vote bit is visible the clock is considered running, and the halt
     * status bit is only valid for unvoted branches. */
    const bool s6_ok = (h_s6 & (1u << TB_BT_BRANCH_HALT_VOTED_BIT)) || !(h_s6 & 1u);
    const bool core_ok = (h_core & (1u << TB_BT_BRANCH_HALT_VOTED_BIT)) || !(h_core & 1u);
    return s6_ok && core_ok;
}

int tb_bt_clock_program(const TbBtClockIo *io, const TbBtClockPlan *plan, TbBtClockEvidence *evidence)
{
    if (!io || !io->read || !io->write || !io->delay_us || !plan) return -1;

    /* The SE owns the source only while DFS is off (clk-rcg2 DFS bypass, as the SM8550 reference does). */
    uint32_t dfs = io->read(io->ctx, TB_BT_RCG_DFS_OFF);
    io->write(io->ctx, TB_BT_RCG_DFS_OFF, dfs & ~1u);
    if (io->read(io->ctx, TB_BT_RCG_DFS_OFF) & 1u) return -1;

    uint32_t m_reg, n_reg, d_reg, cfg_reg;
    tb_bt_clock_regs(plan, &m_reg, &n_reg, &d_reg, &cfg_reg);

    uint32_t v = io->read(io->ctx, TB_BT_RCG_OFF + 0x8u);
    io->write(io->ctx, TB_BT_RCG_OFF + 0x8u, (v & ~0xffffu) | m_reg);
    v = io->read(io->ctx, TB_BT_RCG_OFF + 0xcu);
    io->write(io->ctx, TB_BT_RCG_OFF + 0xcu, (v & ~0xffffu) | n_reg);
    v = io->read(io->ctx, TB_BT_RCG_OFF + 0x10u);
    io->write(io->ctx, TB_BT_RCG_OFF + 0x10u, (v & ~0xffffu) | d_reg);

    v = io->read(io->ctx, TB_BT_RCG_OFF + 0x4u);
    v &= ~(0xffu | (0x7u << 8) | (0x3u << 12) | (1u << 20));
    v |= cfg_reg;
    io->write(io->ctx, TB_BT_RCG_OFF + 0x4u, v);

    /* CMD_UPDATE handshake, update_config() in clk-rcg2.c: 500 attempts at 1 us. */
    v = io->read(io->ctx, TB_BT_RCG_OFF);
    io->write(io->ctx, TB_BT_RCG_OFF, v | 1u);
    bool settled = false;
    for (unsigned i = 0; i < 500u; ++i) {
        if (!(io->read(io->ctx, TB_BT_RCG_OFF) & 1u)) {
            settled = true;
            break;
        }
        io->delay_us(io->ctx, 1u);
    }

    if (evidence) {
        evidence->cmd = io->read(io->ctx, TB_BT_RCG_OFF);
        evidence->cfg = io->read(io->ctx, TB_BT_RCG_OFF + 0x4u);
        evidence->m = io->read(io->ctx, TB_BT_RCG_OFF + 0x8u);
        evidence->n = io->read(io->ctx, TB_BT_RCG_OFF + 0xcu);
        evidence->d = io->read(io->ctx, TB_BT_RCG_OFF + 0x10u);
    }
    return settled ? 0 : -1;
}
