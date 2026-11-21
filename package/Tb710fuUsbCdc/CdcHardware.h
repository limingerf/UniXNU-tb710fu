// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only SM8650 firmware-adoption predicates. Register facts are from
// tb710fu-linux@4d04410798b538fbe4b50664e3a66d9500a2ed31:
// drivers/clk/qcom/{gcc-sm8650.c,gdsc.c,tcsrcc-sm8650.c},
// drivers/phy/phy-snps-eusb2.c and arch/arm64/boot/dts/qcom/sm8650.dtsi.
// SMMU register fields follow ARM SMMUv2 (IHI 0062). No source code is copied.
#ifndef TB710FU_CDC_HARDWARE_H
#define TB710FU_CDC_HARDWARE_H

#include <stdint.h>

#define TB710FU_USB_GCC_BASE UINT64_C(0x00100000)
#define TB710FU_USB_GCC_WINDOW UINT64_C(0x00139000)
#define TB710FU_USB_GCC_PHY_RESET UINT64_C(0x00112000)
#define TB710FU_USB_DWC3_BASE UINT64_C(0x0a600000)
// sm8650.dtsi dwc_usb3 is GIC_SPI 133 (INTID 133 + 32), level high.
#define TB710FU_USB_DWC3_INTID 165u
#define TB710FU_USB_QSCRATCH_BASE UINT64_C(0x0a6f8800)
#define TB710FU_USB_EUSB2_BASE UINT64_C(0x088e3000)
#define TB710FU_USB_CLKREF UINT64_C(0x01ff1000)
#define TB710FU_USB_WRAPPER_XO 0x108u
#define TB710FU_USB_PHY_XO 0x118u
#define TB710FU_USB_SMMU_BASE UINT64_C(0x15000000)
#define TB710FU_USB_SMMU_BYTES 0x100000u

struct tb710fu_cdc_clock_state {
    uint32_t reset, power_status, master, sleep, mock_utmi, config, aggregate;
};

static inline int tb710fu_cdc_branch_active(uint32_t value, int hardware_gated)
{
    return (value & 1u) && (!(value & (1u << 31)) ||
                           (hardware_gated && (value & 2u)));
}

static inline int tb710fu_cdc_clocks_active(const struct tb710fu_cdc_clock_state *s)
{
    return !(s->reset & 1u) && (s->power_status & (1u << 16)) &&
        tb710fu_cdc_branch_active(s->master, 0) &&
        tb710fu_cdc_branch_active(s->sleep, 0) &&
        tb710fu_cdc_branch_active(s->mock_utmi, 0) &&
        tb710fu_cdc_branch_active(s->config, 1) &&
        tb710fu_cdc_branch_active(s->aggregate, 1);
}

struct tb710fu_cdc_phy_state {
    uint32_t reset, reference, control0, control5, common0, control2;
};

static inline int tb710fu_cdc_phy_clocked(uint32_t reset, uint32_t reference)
{
    return !(reset & 1u) && (reference & 1u);
}

// This recognizes the active control state established by the Linux sequence.
// There is no documented PLL-ready bit in that driver. It does not measure the
// PMIC repeater, regulator voltages or analog link; enumeration is still needed.
static inline int tb710fu_cdc_phy_adoptable(const struct tb710fu_cdc_phy_state *s)
{
    return tb710fu_cdc_phy_clocked(s->reset, s->reference) &&
        (s->common0 & 9u) == 9u && !(s->common0 & 4u) &&
        !(s->control5 & 2u) && (s->control0 & 1u) &&
        !(s->control0 & (3u << 3)) &&
        (!(s->control2 & 8u) || (s->control2 & 4u));
}

typedef uint32_t (*tb710fu_cdc_register_read)(void *, unsigned);

struct tb710fu_cdc_identity_route {
    unsigned address_bits, matching_streams, slot, bank, context_offset;
    uint32_t smr, s2cr, cbar, cba2r, sctlr;
};

static inline unsigned tb710fu_cdc_smmu_width(unsigned field)
{
    static const unsigned widths[] = {32, 36, 40, 42, 44, 48};
    return field < sizeof(widths) / sizeof(widths[0]) ? widths[field] : 0;
}

static inline int tb710fu_cdc_identity_route_read(tb710fu_cdc_register_read read,
                                                  void *context,
                                                  struct tb710fu_cdc_identity_route *r)
{
    uint32_t control = read(context, 0), id0 = read(context, 0x20);
    uint32_t id1 = read(context, 0x24), id2 = read(context, 0x28);
    unsigned groups = id0 & 255u, banks = id1 & 255u;
    unsigned pages = 1u << (((id1 >> 28) & 7u) + 1u);
    *r = (struct tb710fu_cdc_identity_route){0};
    // CLIENTPD must be clear. EXIDENABLE changes stream validity semantics;
    // this bounded path only implements SMR.VALID with 4 KiB register pages.
    if ((control & 9u) || !(id0 & (1u << 27)) ||
        !groups || groups > 128u || (id1 & (1u << 31)) ||
        !banks || banks > 128u || pages > 128u ||
        (pages + banks) * 4096u > TB710FU_USB_SMMU_BYTES) return 0;
    r->address_bits = 64;
    for (unsigned shift = 0; shift <= 8; shift += 4) {
        unsigned bits = tb710fu_cdc_smmu_width((id2 >> shift) & 15u);
        if (!bits) return 0;
        if (bits < r->address_bits) r->address_bits = bits;
    }
    for (unsigned i = 0; i < groups; ++i) {
        uint32_t smr = read(context, 0x800u + 4u * i);
        uint32_t mask = (smr >> 16) & 0x7fffu;
        if (!(smr & (1u << 31)) || ((smr ^ 0x40u) & 0xffffu & ~mask)) continue;
        ++r->matching_streams;
        if (r->matching_streams != 1 || mask) return 0;
        r->slot = i;
        r->smr = smr;
        r->s2cr = read(context, 0xc00u + 4u * i);
    }
    if (r->matching_streams != 1) return 0;
    unsigned type = (r->s2cr >> 16) & 3u;
    if (type == 1) return 1; // Explicit bypass; no context-bank translation.
    if (type != 0 || !(id0 & (1u << 30))) return 0;
    r->bank = r->s2cr & 255u;
    if (r->bank >= banks) return 0;
    r->context_offset = (pages + r->bank) * 4096u;
    r->cbar = read(context, 0x1000u + 4u * r->bank);
    r->cba2r = read(context, 0x1800u + 4u * r->bank);
    r->sctlr = read(context, r->context_offset);
    // Only stage-1-with-stage-2-bypass, translation disabled and VA64. Enabled
    // page tables, stage 2 and 32-bit disabled contexts are deliberately refused.
    return ((r->cbar >> 16) & 3u) == 1u && (r->cba2r & 1u) && !(r->sctlr & 1u);
}

#endif
