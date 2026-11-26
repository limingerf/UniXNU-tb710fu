/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* GCC RCG (root clock generator) programming for gcc_qupv3_wrap2_s6_clk_src, the only clock feeding the
 * TB710FU Bluetooth UART's serial engine.
 *
 * Encoding, from drivers/clk/qcom/clk-rcg2.c (Linux master, read at the time of writing):
 *   CMD_REG 0x0   CMD_UPDATE BIT(0)   -- write 1, poll until it self-clears (500 x 1 us)
 *   CFG_REG 0x4   CFG_SRC_DIV_SHIFT 0 length 8 | CFG_SRC_SEL_SHIFT 8 mask 0x7 |
 *                 CFG_MODE_SHIFT 12 mask 0x3 (DUAL_EDGE = 2) | CFG_HW_CLK_CTRL_MASK BIT(20)
 *   M_REG 0x8     __clk_rcg2_configure_mnd writes f->m outright
 *   N_REG 0xc     writes ~(f->n - f->m) & mask
 *   D_REG 0x10    writes ~d & mask, d = clamp(f->n, f->m, 2 * (f->n - f->m))
 * and the repository's own SM8550 GENI UART source sequence (refs/UniXNU-sm8550-common .../Sm8550GeniUart.c)
 * writes the same three values in the same order, which is the second independent confirmation of the
 * complemented N/D layout. mnd_width is 16 and hid_width is 5 for this root (linux-gcc-sm8650.c:1191-1198).
 *
 * Frequencies are the ftbl_gcc_qupv3_wrap2_s6_clk_src entries (linux-gcc-sm8650.c:1167-1180): GPLL0 even
 * 300 MHz, pre-div 1, 16-bit M/N. 51.2 MHz is not in that table but is an exact M/N of the same parent,
 * 300 MHz * 64 / 375, which is what a 3.2 Mbaud line needs at the QUP >= 2.5 oversampling of 16.
 */
#ifndef TB710FU_BLUETOOTH_CLOCK_H
#define TB710FU_BLUETOOTH_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TB_BT_CLK_GPLL0_EVEN_HZ 300000000ull
#define TB_BT_CLK_SRC_SEL 6u      /* gcc_parent_map_10 index 6 = GPLL0_OUT_EVEN */
#define TB_BT_CLK_MODE_DUAL_EDGE 2u
#define TB_BT_RCG_PRE_DIV 1u      /* every ftbl entry for this root uses pre-div 1 */

typedef struct {
    uint32_t (*read)(void *ctx, uint32_t offset);
    void (*write)(void *ctx, uint32_t offset, uint32_t value);
    void (*delay_us)(void *ctx, uint32_t us);
    void *ctx;
} TbBtClockIo;

typedef struct {
    uint32_t baud;
    uint32_t rate;   /* serial clock the SE divides from */
    uint32_t m;
    uint32_t n;
    uint32_t pre_div;
    uint32_t src;
    uint32_t clk_div; /* value for SER_M/S_CLK_CFG: rate / (baud * oversampling), rounded up */
} TbBtClockPlan;

typedef struct {
    uint32_t cmd;
    uint32_t cfg;
    uint32_t m;
    uint32_t n;
    uint32_t d;
    uint32_t branch_before;
    uint32_t branch_after;
    uint32_t halt_s6;
    uint32_t halt_core;
} TbBtClockEvidence;

/* Fills in the plan for a supported baud rate; returns 0, or -1 when there is no exact M/N plan. Every
 * plan is checked against 300 MHz * m / n before it is returned, so a typo in the table cannot reach a
 * register write. */
int tb_bt_clock_plan(uint32_t baud, TbBtClockPlan *out);
/* ceil(rate / (baud * oversampling)); 0 when the arguments are unusable. */
uint32_t tb_bt_clock_div(uint32_t rate, uint32_t baud);
/* Votes the S6 branch and the QUP2 core branch on/off in GCC (enable_reg 0x52010, bits 10 and 0) and
 * checks the halt state of each (BRANCH_HALT_VOTED: bit 31 set means the vote is through). */
bool tb_bt_clock_vote(const TbBtClockIo *io, bool enable, TbBtClockEvidence *evidence);
/* Runs the RCG sequence: clear DFS, M, N, D, CFG, then update. Returns 0 when CMD_UPDATE cleared in time. */
int tb_bt_clock_program(const TbBtClockIo *io, const TbBtClockPlan *plan, TbBtClockEvidence *evidence);
/* The register values the sequence writes, exposed so the host test can assert them without sim hardware. */
void tb_bt_clock_regs(const TbBtClockPlan *plan, uint32_t *m_reg, uint32_t *n_reg, uint32_t *d_reg,
                      uint32_t *cfg_reg);
/* Absolute GCC offsets (sm8650.dtsi:1967 base 0x00100000 + linux-gcc-sm8650.c offsets). */
#define TB_BT_RCG_OFF 0x1e760u
#define TB_BT_RCG_DFS_OFF 0x1e774u /* cmd_rcgr + 0x14, SE_CMD_DFS */
#define TB_BT_BRANCH_REG 0x52010u
#define TB_BT_BRANCH_S6_HALT 0x1e754u
#define TB_BT_BRANCH_CORE_HALT 0x23294u
#define TB_BT_BRANCH_S6_BIT 10u
#define TB_BT_BRANCH_CORE_BIT 0u
#define TB_BT_BRANCH_HALT_VOTED_BIT 31u

#ifdef __cplusplus
}
#endif
#endif /* TB710FU_BLUETOOTH_CLOCK_H */
