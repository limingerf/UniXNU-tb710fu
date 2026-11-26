/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Tb710fuWifiBoard.h"

uint32_t tb710fu_wifi_tlmm_config(unsigned function, unsigned drive_ma, unsigned pull, int output)
{
    unsigned drive = 0;
    if (drive_ma >= 2u) {
        drive = (drive_ma / 2u) - 1u;
        if (drive > 7u) drive = 7u;
    }
    return (pull & 3u) | ((function & 0xfu) << 2) | (drive << 6) | (output ? (1u << 9) : 0u);
}

uint32_t tb710fu_wifi_tlmm_level(int high)
{
    return high ? 2u : 0u;
}

void tb710fu_wifi_pin_plan(struct tb710fu_wifi_pin_plan *plan)
{
    if (!plan) return;
    /* sm8650.dtsi pcie0_default_state: perst gpio94 2 mA pull-down; wake gpio96 2 mA pull-up; clkreq gpio95 function 3. */
    plan->perst_cfg = tb710fu_wifi_tlmm_config(TB710FU_WIFI_FUNC_GPIO, 2, TB710FU_WIFI_PULL_DOWN, 1);
    plan->perst_level = tb710fu_wifi_tlmm_level(0); /* PERST# asserted (active low) */
    /* board wlan_en: gpio16 8 mA pull-down; the enable is driven high once PERST# is released. */
    plan->wlan_cfg = tb710fu_wifi_tlmm_config(TB710FU_WIFI_FUNC_GPIO, 8, TB710FU_WIFI_PULL_DOWN, 1);
    plan->wlan_level = tb710fu_wifi_tlmm_level(1);
    /* board bt_default: gpio17 16 mA no bias, held low (Bluetooth is not part of this milestone). */
    plan->bt_cfg = tb710fu_wifi_tlmm_config(TB710FU_WIFI_FUNC_GPIO, 16, TB710FU_WIFI_PULL_NONE, 1);
    plan->bt_level = tb710fu_wifi_tlmm_level(0);
    /* board &pcie0 wake: gpio96 2 mA pull-up, input. */
    plan->wake_cfg = tb710fu_wifi_tlmm_config(TB710FU_WIFI_FUNC_GPIO, 2, TB710FU_WIFI_PULL_UP, 0);
    plan->wake_level = 0;
    /* pcie0_default_state clkreq-pins: function "pcie0_clk_req_n" is 3 on this TLMM; the endpoint drives it. */
    plan->clkreq_cfg = tb710fu_wifi_tlmm_config(3, 2, TB710FU_WIFI_PULL_UP, 0);
}

void tb710fu_wifi_gcc_plan(struct tb710fu_wifi_gcc_plan *plan)
{
    if (!plan) return;
    plan->vote0_mask = (1u << 11) | (1u << 12) | (1u << 19) | (1u << 20) | (1u << 22);
    plan->vote8_mask = 0x7fu; /* slv, mstr, cfg, aux, pipe, q2a, cnoc_sf are bits 0..6 */
    plan->collapse_clear = (1u << 0) | (1u << 3);
    plan->gdsc_cfg_offset = TB710FU_WIFI_GCC_GDSC_CFG_DELTA;
}

int tb710fu_wifi_gdsc_on(uint32_t cfg_gdscr)
{
    return (cfg_gdscr & (1u << 16)) != 0; /* GDSC_POWER_UP_COMPLETE in CFG_GDSCR */
}

int tb710fu_wifi_branch_halted(uint32_t halt)
{
    return (halt & (1u << 31)) != 0;
}

int tb710fu_wifi_branch_acked(uint32_t halt)
{
    return (halt & (1u << 30)) != 0;
}

void tb710fu_wifi_atu_config(struct tb710fu_wifi_atu_region *region, uint64_t cpu_base, uint64_t bytes,
                             unsigned bus)
{
    if (!region) return;
    if (!bytes) bytes = 1;
    region->ctrl1 = TB710FU_WIFI_ATU_CTRL1_CFG0;
    region->ctrl2 = TB710FU_WIFI_ATU_CTRL2_ENABLE;
    region->base = cpu_base;
    region->limit = cpu_base + bytes - 1;
    region->target = (uint64_t)(bus & 0xffu) << 24; /* CFG0 target: bus number in bits 31:24 */
    /* Region 0 has no upper-limit register of its own; the upper base/target carry the high address bits. */
    if (region->base >> 32) region->ctrl1 |= 0; /* no-op: kept explicit that only the low 4 GiB CPU window is used */
}

void tb710fu_wifi_atu_memory(struct tb710fu_wifi_atu_region *region, uint64_t cpu_base, uint64_t bytes,
                             uint64_t pci_target)
{
    if (!region) return;
    if (!bytes) bytes = 1;
    region->ctrl1 = TB710FU_WIFI_ATU_CTRL1_MEM;
    region->ctrl2 = TB710FU_WIFI_ATU_CTRL2_ENABLE;
    region->base = cpu_base;
    region->limit = cpu_base + bytes - 1;
    region->target = pci_target;
}

uint64_t tb710fu_wifi_bar_size(uint32_t mask)
{
    const uint32_t bits = mask & ~0xfu;
    if (!bits) return 0;
    uint64_t size = 1;
    uint32_t probe = bits;
    while (!(probe & 1u)) {
        probe >>= 1;
        size <<= 1;
    }
    return size;
}

int tb710fu_wifi_bar_place(uint64_t size, uint64_t *address, uint64_t window_base, uint64_t window_bytes)
{
    if (!size || !address || (size & (size - 1)) != 0) return 0;
    uint64_t base = (window_base + size - 1) & ~(size - 1);
    if (base < window_base || base + size > window_base + window_bytes) return 0;
    *address = base;
    return 1;
}

/* ---- apps-SMMU identity route ---- */

static unsigned smmu_bits(unsigned code)
{
    switch (code) {
    case 0: return 32;
    case 1: return 36;
    case 2: return 40;
    case 3: return 42;
    case 4: return 44;
    case 5: return 48;
    default: return 0;
    }
}

enum tb710fu_wifi_smmu_result tb710fu_wifi_smmu_plan(struct tb710fu_wifi_smmu_io *io, uint16_t sid,
                                                      unsigned bank, struct tb710fu_wifi_smmu_plan *plan)
{
    if (!io || !plan || !io->read) return TB710FU_WIFI_SMMU_GEOMETRY;
    *plan = (struct tb710fu_wifi_smmu_plan){0};
    plan->error = "SMMU geometry unusable";
    const uint32_t scr0 = io->read(io->ctx, 0x000u);
    const uint32_t idr0 = io->read(io->ctx, 0x020u);
    const uint32_t idr1 = io->read(io->ctx, 0x024u);
    const uint32_t idr2 = io->read(io->ctx, 0x028u);
    const unsigned groups = idr0 & 0xffu;
    const unsigned banks = idr1 & 0xffu;
    const unsigned pages = 1u << (((idr1 >> 28) & 7u) + 1u);
    unsigned address_bits = 64;
    if ((scr0 & 0x9u) || !(idr0 & (1u << 27)) || !groups || groups > 128u ||
        (idr1 & (1u << 31)) || !banks || banks > 128u || pages > 128u ||
        (pages + banks) * 4096u > TB710FU_WIFI_SMMU_BYTES) {
        return TB710FU_WIFI_SMMU_GEOMETRY;
    }
    for (unsigned shift = 0; shift <= 8; shift += 4) {
        const unsigned bits = smmu_bits((idr2 >> shift) & 0xfu);
        if (!bits) return TB710FU_WIFI_SMMU_GEOMETRY;
        if (bits < address_bits) address_bits = bits;
    }
    /* Two 4 GiB segments of DMA must stay inside the input address width. */
    if (address_bits < 33u) {
        plan->error = "SMMU input width below 33 bits";
        return TB710FU_WIFI_SMMU_GEOMETRY;
    }
    if (!bank || bank >= banks) {
        plan->error = "named context bank does not exist";
        return TB710FU_WIFI_SMMU_BAD_BANK;
    }
    plan->pages = pages;
    plan->bank = bank;
    plan->sctlr = io->read(io->ctx, (pages + bank) * 4096u);
    plan->cbar = io->read(io->ctx, 0x1000u + 4u * bank);
    plan->cba2r = io->read(io->ctx, 0x1800u + 4u * bank);
    plan->bank_ready = (plan->sctlr == 0 && plan->cbar == 0x10000u &&
                        (plan->cba2r & ~1u) == 0 && (plan->cba2r & 1u) != 0);
    if (plan->sctlr & 1u) {
        plan->error = "named context bank has translation enabled";
        return TB710FU_WIFI_SMMU_BAD_BANK;
    }
    unsigned free_group = groups;
    unsigned matches = 0;
    for (unsigned i = 0; i < groups; ++i) {
        const uint32_t smr = io->read(io->ctx, 0x800u + 4u * i);
        if (!(smr & (1u << 31))) {
            if (free_group == groups) free_group = i;
            continue;
        }
        const uint32_t mask = (smr >> 16) & 0x7fffu;
        if (((smr ^ sid) & 0xffffu & ~mask) != 0) continue;
        if (mask != 0) {
            plan->error = "stream id is covered by a masked group; refusing to rewrite a range";
            return TB710FU_WIFI_SMMU_CONFLICT;
        }
        const uint32_t s2cr = io->read(io->ctx, 0xc00u + 4u * i);
        if (++matches > 1u) {
            plan->error = "more than one group names this stream id";
            return TB710FU_WIFI_SMMU_CONFLICT;
        }
        plan->group = i;
        plan->smr = smr;
        plan->s2cr = s2cr;
        const unsigned type = (s2cr >> 16) & 3u;
        const unsigned cb = s2cr & 0xffu;
        if (type == 0 && cb == bank) {
            plan->reuse = 1;
        } else {
            plan->error = "stream id already routes elsewhere or faults";
            return TB710FU_WIFI_SMMU_CONFLICT;
        }
    }
    if (plan->reuse) {
        if (!plan->bank_ready) {
            plan->error = "bypass bank is not the identity context";
            return TB710FU_WIFI_SMMU_BAD_BANK;
        }
        plan->error = 0;
        return TB710FU_WIFI_SMMU_OK;
    }
    if (free_group == groups) {
        plan->error = "no free stream-match group";
        return TB710FU_WIFI_SMMU_NO_GROUP;
    }
    plan->group = free_group;
    plan->error = 0;
    return TB710FU_WIFI_SMMU_OK;
}

int tb710fu_wifi_smmu_apply(struct tb710fu_wifi_smmu_io *io, uint16_t sid,
                            const struct tb710fu_wifi_smmu_plan *plan)
{
    if (!io || !plan || plan->error) return 0;
    const uint32_t cbndx = (uint32_t)plan->bank;
    const uint32_t bypass_sctlr = 0;
    const uint32_t cba2r = 1u;
    const uint32_t cbar = 0x10000u;
    const unsigned cb = (plan->pages + plan->bank) * 4096u;
    const unsigned cbar_off = 0x1000u + 4u * plan->bank;
    const unsigned cba2r_off = 0x1800u + 4u * plan->bank;
    if (!plan->bank_ready) {
        io->write(io->ctx, cb, bypass_sctlr);
        io->write(io->ctx, cba2r_off, cba2r);
        io->write(io->ctx, cbar_off, cbar);
        if (io->barrier) io->barrier(io->ctx);
        if (io->read(io->ctx, cb) != bypass_sctlr || io->read(io->ctx, cba2r_off) != cba2r ||
            io->read(io->ctx, cbar_off) != cbar) return 0;
    } else if (io->read(io->ctx, cb) != bypass_sctlr || io->read(io->ctx, cbar_off) != cbar ||
               io->read(io->ctx, cba2r_off) != cba2r) {
        return 0;
    }
    if (!plan->reuse) {
        const unsigned s2cr_off = 0xc00u + 4u * plan->group;
        const unsigned smr_off = 0x800u + 4u * plan->group;
        io->write(io->ctx, s2cr_off, cbndx); /* type 0: stage-1 identity context, stage-2 bypass */
        if (io->barrier) io->barrier(io->ctx);
        if (io->read(io->ctx, s2cr_off) != cbndx) return 0;
        io->write(io->ctx, smr_off, (1u << 31) | (uint32_t)sid);
        if (io->barrier) io->barrier(io->ctx);
        if (io->read(io->ctx, smr_off) != ((1u << 31) | (uint32_t)sid)) return 0;
    }
    return 1;
}
