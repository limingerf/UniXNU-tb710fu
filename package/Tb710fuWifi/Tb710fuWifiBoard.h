/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Pure planning helpers for the TB710FU WCN7850 board bring-up: TLMM pin words, GCC votes/GDSC predicates, iATU
 * descriptors, BAR sizing and the apps-SMMU identity stream route. No MMIO here, so the host tests can exercise every
 * value the kernel writes. All addresses live in Tb710fuWifiRegs.h with their DTS provenance. */
#ifndef TB710FU_WIFI_BOARD_H
#define TB710FU_WIFI_BOARD_H

#include <stdint.h>

#include "Tb710fuWifiRegs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TLMM_CFG: pull 1:0 (2 up, 1 down), function 5:2, drive 8:6 (mA/2 - 1), output enable 9. */
#define TB710FU_WIFI_PULL_NONE 0u
#define TB710FU_WIFI_PULL_DOWN 1u
#define TB710FU_WIFI_PULL_UP   2u
#define TB710FU_WIFI_FUNC_GPIO 0u

uint32_t tb710fu_wifi_tlmm_config(unsigned function, unsigned drive_ma, unsigned pull, int output);
uint32_t tb710fu_wifi_tlmm_level(int high); /* the IO register's value: 2 drives high, 0 drives low */

/* Board pin plan, in the order the board's power sequence applies it. */
struct tb710fu_wifi_pin_plan {
    uint32_t perst_cfg, perst_level;   /* gpio94: function gpio, 2 mA, pull-down, output */
    uint32_t wlan_cfg, wlan_level;     /* gpio16: function gpio, 8 mA, pull-down, output */
    uint32_t bt_cfg, bt_level;         /* gpio17: function gpio, 16 mA, no bias, output, held low */
    uint32_t wake_cfg, wake_level;     /* gpio96: function gpio, 2 mA, pull-up, input */
    uint32_t clkreq_cfg;               /* gpio95: function pcie0_clk_req_n (3), 2 mA, pull-up, input */
};

void tb710fu_wifi_pin_plan(struct tb710fu_wifi_pin_plan *plan);

/* GCC: the PCIe branch votes and the two votable GDSCs. */
struct tb710fu_wifi_gcc_plan {
    uint32_t vote0_mask;   /* 0x52000: qmip bit11, aggre bit12, ddrss bit19, cfg_noc bit20, rchng bit22 */
    uint32_t vote8_mask;   /* 0x52008: slv bit0, mstr bit1, cfg bit2, aux bit3, pipe bit4, q2a bit5, cnoc_sf bit6 */
    uint32_t collapse_clear; /* 0x5214c: PCIe0 bit0 and PHY bit3 */
    uint32_t gdsc_cfg_offset; /* +4: CFG_GDSCR holds POWER_UP_COMPLETE bit16 */
};
void tb710fu_wifi_gcc_plan(struct tb710fu_wifi_gcc_plan *plan);

int tb710fu_wifi_gdsc_on(uint32_t cfg_gdscr);      /* CFG_GDSCR bit16: GDSC_POWER_UP_COMPLETE */
int tb710fu_wifi_branch_halted(uint32_t halt);     /* bit31: the branch has not started */
int tb710fu_wifi_branch_acked(uint32_t halt);      /* bit30: the clock owner acknowledged the vote */

/* iATU: one outbound region. The config window uses CFG0 with the bus number in target bits 31:24 (as the verified
 * SM8550-compatible endpoint probe writes it); a memory window targets the PCI address. */
struct tb710fu_wifi_atu_region {
    uint32_t ctrl1, ctrl2;
    uint64_t base, limit, target;
};

void tb710fu_wifi_atu_config(struct tb710fu_wifi_atu_region *region, uint64_t cpu_base, uint64_t bytes, unsigned bus);
void tb710fu_wifi_atu_memory(struct tb710fu_wifi_atu_region *region, uint64_t cpu_base, uint64_t bytes,
                             uint64_t pci_target);

/* BAR sizing: a write of all ones reads back the size mask (bit 0..3 are the type bits). */
uint64_t tb710fu_wifi_bar_size(uint32_t mask);
int tb710fu_wifi_bar_place(uint64_t size, uint64_t *address, uint64_t window_base, uint64_t window_bytes);

/* apps-SMMU identity route for the endpoint's stream id 0x1401.
 *
 * The SM8650 firmware SMMU rejects S2CR.TYPE=BYPASS (it reads back as FAULT; Tb710fuUsbCdc/CdcHardware.h measured the
 * same on this SoC). A translation to a disabled VA64 stage-1 context bank is the identity route this board uses for
 * USB: SCTLR=0, CBA2R=1, CBAR=0x10000. Banks are shared with the other drivers, so the caller names the bank it may
 * use (boot argument wifi-smmu-bank, default none) and every write is verified by read-back. */
struct tb710fu_wifi_smmu_io {
    void *ctx;
    uint32_t (*read)(void *ctx, uint32_t offset);
    void (*write)(void *ctx, uint32_t offset, uint32_t value);
    void (*barrier)(void *ctx);
};

struct tb710fu_wifi_smmu_plan {
    unsigned group, bank, pages;
    uint32_t smr, s2cr, cbar, cba2r, sctlr; /* values read before the claim */
    int reuse;                              /* the stream already maps to this bank */
    int bank_ready;                         /* the bank already holds the identity context */
    const char *error;
};

enum tb710fu_wifi_smmu_result {
    TB710FU_WIFI_SMMU_OK = 0,
    TB710FU_WIFI_SMMU_GEOMETRY,
    TB710FU_WIFI_SMMU_NO_GROUP,
    TB710FU_WIFI_SMMU_CONFLICT,
    TB710FU_WIFI_SMMU_BAD_BANK,
};

enum tb710fu_wifi_smmu_result tb710fu_wifi_smmu_plan(struct tb710fu_wifi_smmu_io *io, uint16_t sid,
                                                      unsigned bank, struct tb710fu_wifi_smmu_plan *plan);
int tb710fu_wifi_smmu_apply(struct tb710fu_wifi_smmu_io *io, uint16_t sid, const struct tb710fu_wifi_smmu_plan *plan);

#ifdef __cplusplus
}
#endif

#endif /* TB710FU_WIFI_BOARD_H */
