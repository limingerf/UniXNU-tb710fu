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
#define TB710FU_USB_CMD_DB_BASE UINT64_C(0x081c60000)
#define TB710FU_USB_CMD_DB_BYTES 0x20000u
#define TB710FU_USB_RSC_DRV2_BASE UINT64_C(0x17a20000)
#define TB710FU_USB_RSC_BYTES 0x10000u
#define TB710FU_USB_SPMI_CORE_BASE UINT64_C(0x0c400000)
#define TB710FU_USB_SPMI_CONFIG_BASE UINT64_C(0x0c42d000)
#define TB710FU_USB_SPMI_OBSERVER_BASE UINT64_C(0x0c440000)
#define TB710FU_USB_SPMI_CHANNEL_BASE UINT64_C(0x0c500000)
#define TB710FU_USB_SPMI_CORE_BYTES 0x3000u
#define TB710FU_USB_SPMI_CONFIG_BYTES 0x4000u
#define TB710FU_USB_SPMI_OBSERVER_STRIDE 0x20u
#define TB710FU_USB_SPMI_CHANNEL_STRIDE 0x1000u

struct tb710fu_cdc_masked_write {
    uint16_t offset;
    uint32_t mask, value;
};

static inline uint32_t tb710fu_cdc_masked_value(uint32_t old_value,
                                                 uint32_t mask, uint32_t value)
{
    return (old_value & ~mask) | (value & mask);
}

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

// These active bits follow the Linux QCOM sequence. They do not measure the
// eUSB2 analog link or replace the PMIC repeater's own ready check.
static inline int tb710fu_cdc_phy_adoptable(const struct tb710fu_cdc_phy_state *s)
{
    return tb710fu_cdc_phy_clocked(s->reset, s->reference) &&
        (s->common0 & 9u) == 9u && !(s->common0 & 4u) &&
        !(s->control5 & 2u) && (s->control0 & 1u) &&
        !(s->control0 & (3u << 3)) &&
        (!(s->control2 & 8u) || (s->control2 & 4u));
}

typedef uint32_t (*tb710fu_cdc_register_read)(void *, unsigned);
typedef void (*tb710fu_cdc_register_write)(void *, unsigned, uint32_t);
typedef void (*tb710fu_cdc_register_barrier)(void *);

struct tb710fu_cdc_identity_route {
    unsigned address_bits, matching_streams, slot, bank, context_offset;
    // Group count from the geometry pass, so a claim can walk every group
    // without a second register sweep.
    unsigned groups;
    // Set when the group that routes 0x40 is a masked (range) group, so the
    // log can say the route is shared rather than exclusive to this SID.
    unsigned masked;
    // How many groups matched at all, and how many of those pin the SID bits
    // (SMR.MASK leaves every bit of SID 0x40 in place). A catch-all group such
    // as {ID=0, MASK=0x7fff} matches every stream, so "more than one match" on
    // its own says nothing about a conflict.
    unsigned candidates, exact_candidates;
    uint32_t smr, s2cr, cbar, cba2r, sctlr;
};

// A single boolean never said which of the mutually exclusive conditions a live
// SMMU failed, and the device only has eight characters of bottom-band text.
// The scan therefore reports the class and lets the caller name it.
enum tb710fu_cdc_smmu_result {
    TB710FU_CDC_SMMU_OK = 0,     // Exact group; explicit bypass or identity VA.
    TB710FU_CDC_SMMU_ABSENT,     // No group names SID 0x40; a free slot exists.
    TB710FU_CDC_SMMU_FAULTED,    // Exact group exists but its S2CR faults.
    TB710FU_CDC_SMMU_AMBIGUOUS,  // Masked alias or more than one group matches.
    TB710FU_CDC_SMMU_TRANSLATES, // Exact group resolves through a bank; not ours.
    TB710FU_CDC_SMMU_GEOMETRY,   // sCR0, SMS or the register geometry is unusable.
    TB710FU_CDC_SMMU_FULL        // Every stream-match group is already valid.
};

struct tb710fu_cdc_smmu_geometry {
    uint32_t id0, id1, id2;
    unsigned groups, banks, pages, address_bits;
};

// One group quarantined from the boot chain, kept so a failed publication can
// put it back exactly as it was found.
struct tb710fu_cdc_smmu_saved {
    unsigned slot;
    uint32_t smr, s2cr;
    // What the last publication attempt actually read back, plus how many
    // groups were tried. Without these a refused claim cannot say whether a
    // write was ignored, landed somewhere else, or never ran at all.
    unsigned tried;
    uint32_t probe_smr, probe_s2cr;
};

static inline unsigned tb710fu_cdc_smmu_width(unsigned field)
{
    static const unsigned widths[] = {32, 36, 40, 42, 44, 48};
    return field < sizeof(widths) / sizeof(widths[0]) ? widths[field] : 0;
}

static inline enum tb710fu_cdc_smmu_result tb710fu_cdc_smmu_geometry_read(
    tb710fu_cdc_register_read read, void *context,
    struct tb710fu_cdc_smmu_geometry *g)
{
    const uint32_t control = read(context, 0);
    g->id0 = read(context, 0x20);
    g->id1 = read(context, 0x24);
    g->id2 = read(context, 0x28);
    g->groups = g->id0 & 255u;
    g->banks = g->id1 & 255u;
    // Linux arm-smmu.c: size = 1 << (NUMPAGENDXB + 1); CB = (size + bank) << 12.
    g->pages = 1u << (((g->id1 >> 28) & 7u) + 1u);
    g->address_bits = 64;
    // CLIENTPD must be clear. EXIDENABLE changes stream validity semantics;
    // this bounded path only implements SMR.VALID with 4 KiB register pages.
    if ((control & 9u) || !(g->id0 & (1u << 27)) ||
        !g->groups || g->groups > 128u || (g->id1 & (1u << 31)) ||
        !g->banks || g->banks > 128u || g->pages > 128u ||
        (g->pages + g->banks) * 4096u > TB710FU_USB_SMMU_BYTES)
        return TB710FU_CDC_SMMU_GEOMETRY;
    for (unsigned shift = 0; shift <= 8; shift += 4) {
        const unsigned bits = tb710fu_cdc_smmu_width((g->id2 >> shift) & 15u);
        if (!bits) return TB710FU_CDC_SMMU_GEOMETRY;
        if (bits < g->address_bits) g->address_bits = bits;
    }
    return TB710FU_CDC_SMMU_OK;
}

// Read-only. Names the class of the SID 0x40 route and, when a takeover is
// possible, the group it would use; it never writes a register.
static inline enum tb710fu_cdc_smmu_result tb710fu_cdc_identity_route_scan(
    tb710fu_cdc_register_read read, void *context,
    struct tb710fu_cdc_identity_route *r)
{
    struct tb710fu_cdc_smmu_geometry g;
    *r = (struct tb710fu_cdc_identity_route){0};
    const enum tb710fu_cdc_smmu_result status =
        tb710fu_cdc_smmu_geometry_read(read, context, &g);
    if (status != TB710FU_CDC_SMMU_OK) return status;
    r->address_bits = g.address_bits;
    r->groups = g.groups;
    // The storage planner scans groups from the top down and expects the last
    // one free (its own test asserts slot 127). CDC is queued before storage
    // starts, so claiming from the bottom keeps the two claims disjoint even
    // when only a couple of groups remain.
    const unsigned reserved = g.groups - 1u;
    unsigned free_slot = g.groups;
    // SMMUv2 matches when (sid & ~SMR.MASK) == (SMR.ID & ~SMR.MASK). A group
    // whose MASK covers the whole SID therefore matches every stream, including
    // 0x40; Android leaves exactly one such catch-all behind. Counting it as a
    // second route made every board look ambiguous. Selection is therefore two
    // tiers: a group that pins every SID bit (exact) wins outright, and the
    // wide groups are only considered when no exact one exists. Conflicting
    // groups within the same tier are still refused, because which one the SMMU
    // applies then depends on group order.
    unsigned exact_slot = g.groups, wide_slot = g.groups;
    unsigned exact_count = 0, wide_count = 0;
    uint32_t exact_smr = 0, exact_s2cr = 0, wide_smr = 0, wide_s2cr = 0;
    for (unsigned i = 0; i < reserved; ++i) {
        const uint32_t smr = read(context, 0x800u + 4u * i);
        if (!(smr & (1u << 31))) {
            if (free_slot == g.groups) free_slot = i;
            continue;
        }
        const uint32_t mask = (smr >> 16) & 0x7fffu;
        if ((smr ^ 0x40u) & 0xffffu & ~mask) continue;
        const uint32_t s2cr = read(context, 0xc00u + 4u * i);
        if ((mask & 0x40u) == 0u) {
            ++exact_count;
            exact_slot = i;
            exact_smr = smr;
            exact_s2cr = s2cr;
        } else {
            ++wide_count;
            wide_slot = i;
            wide_smr = smr;
            wide_s2cr = s2cr;
        }
    }
    r->candidates = exact_count + wide_count;
    r->exact_candidates = exact_count;
    if (exact_count > 1u || (exact_count == 0u && wide_count > 1u))
        return TB710FU_CDC_SMMU_AMBIGUOUS;
    if (exact_count) {
        r->slot = exact_slot;
        r->smr = exact_smr;
        r->s2cr = exact_s2cr;
    } else if (wide_count) {
        r->slot = wide_slot;
        r->smr = wide_smr;
        r->s2cr = wide_s2cr;
    } else {
        r->slot = free_slot;
        return free_slot < g.groups ? TB710FU_CDC_SMMU_ABSENT : TB710FU_CDC_SMMU_FULL;
    }
    r->matching_streams = 1u;
    if (r->smr & 0x7fff0000u) r->masked = 1;
    const unsigned type = (r->s2cr >> 16) & 3u;
    if (type == 1) return TB710FU_CDC_SMMU_OK; // Explicit bypass, no bank.
    if (type == 2) return TB710FU_CDC_SMMU_FAULTED; // Already blocked; safe to claim.
    if (type != 0 || !(g.id0 & (1u << 30))) return TB710FU_CDC_SMMU_TRANSLATES;
    r->bank = r->s2cr & 255u;
    if (r->bank >= g.banks) return TB710FU_CDC_SMMU_TRANSLATES;
    r->context_offset = (g.pages + r->bank) * 4096u;
    r->cbar = read(context, 0x1000u + 4u * r->bank);
    r->cba2r = read(context, 0x1800u + 4u * r->bank);
    r->sctlr = read(context, r->context_offset);
    // Only stage-1-with-stage-2-bypass, translation disabled and VA64. Enabled
    // page tables, stage 2 and 32-bit disabled contexts are deliberately refused.
    return (((r->cbar >> 16) & 3u) == 1u && (r->cba2r & 1u) && !(r->sctlr & 1u))
        ? TB710FU_CDC_SMMU_OK : TB710FU_CDC_SMMU_TRANSLATES;
}

// True only for a route this adapter may use as it stands.
static inline int tb710fu_cdc_identity_route_read(tb710fu_cdc_register_read read,
                                                  void *context,
                                                  struct tb710fu_cdc_identity_route *r)
{
    return tb710fu_cdc_identity_route_scan(read, context, r) == TB710FU_CDC_SMMU_OK;
}

// What a group actually keeps of each action encoding. The measured board
// wrote TYPE=BYPASS into a free group and read back TYPE=FAULT, so the value
// that survives a write has to be observed rather than assumed.
struct tb710fu_cdc_smmu_actions {
    uint32_t keep, fault, bypass, both, smr;
};

static inline void tb710fu_cdc_smmu_probe_actions(
    tb710fu_cdc_register_read read, tb710fu_cdc_register_write write,
    tb710fu_cdc_register_barrier barrier, void *context, unsigned slot,
    struct tb710fu_cdc_smmu_saved *saved,
    struct tb710fu_cdc_smmu_actions *out)
{
    const unsigned s2cr_offset = 0xc00u + 4u * slot;
    const unsigned smr_offset = 0x800u + 4u * slot;
    write(context, s2cr_offset, 0);
    barrier(context);
    out->keep = read(context, s2cr_offset);
    write(context, s2cr_offset, 2u << 16);
    barrier(context);
    out->fault = read(context, s2cr_offset);
    write(context, s2cr_offset, 1u << 16);
    barrier(context);
    out->bypass = read(context, s2cr_offset);
    write(context, s2cr_offset, 3u << 16);
    barrier(context);
    out->both = read(context, s2cr_offset);
    // Only the action word is exercised here. The stream table is left exactly
    // as measured: a group whose action is FAULT was the one place the walk
    // could still stall, and the SID write is only worth trying once an action
    // has held.
    out->smr = read(context, smr_offset);
    write(context, s2cr_offset, saved->s2cr);
    barrier(context);
}

// Publish an explicit BYPASS route for SID 0x40 in a group the scan proved free
// or faulted. S2CR is written before SMR is made valid, so an interrupted
// publication leaves the stream unmatched rather than pointed at a live bank.
// Every write is read back; any mismatch restores the two saved words.
//
// A masked group is never rewritten. Its SMR covers a range of stream IDs, so
// flipping its action would change the routing of every other master behind the
// same range; that is a decision the boot chain owner has to make, not a side
// effect of bringing up a console.
static inline int tb710fu_cdc_stream_bypass_attach(
    tb710fu_cdc_register_read read, tb710fu_cdc_register_write write,
    tb710fu_cdc_register_barrier barrier, void *context,
    const struct tb710fu_cdc_identity_route *planned,
    struct tb710fu_cdc_smmu_saved *saved)
{
    if (!planned || !saved || planned->slot >= 128u || planned->masked) return 0;
    unsigned groups = planned->groups ? planned->groups : 128u;
    if (groups > 128u) groups = 128u;
    // Never take the top group: the storage planner claims downwards from there.
    const unsigned stop = groups > 2u ? groups - 1u : groups;
    const unsigned first = planned->slot < stop ? planned->slot : 0u;
    // SMMUv2 S2CR.TYPE: 0 translate, 1 bypass, 2 fault. CBNDX is irrelevant
    // to bypass, so the value is fixed rather than taken from the old route.
    const uint32_t bypass = 1u << 16;
    const uint32_t match = (1u << 31) | 0x40u;
    saved->tried = 0;
    saved->probe_smr = 0;
    saved->probe_s2cr = 0;
    // The measured board accepts the scan's first free group in the scan but
    // drops the publication, and only the read-back says so. Walking the free
    // groups keeps one hard-wired or locked group from killing the console:
    // every candidate is tried, and the last read-back is reported.
    for (unsigned slot = first; slot < stop; ++slot) {
        const unsigned smr_offset = 0x800u + 4u * slot;
        const unsigned s2cr_offset = 0xc00u + 4u * slot;
        const uint32_t smr = read(context, smr_offset);
        if ((smr & (1u << 31)) && slot != planned->slot) continue;
        saved->slot = slot;
        saved->smr = smr;
        saved->s2cr = read(context, s2cr_offset);
        ++saved->tried;
        int ok = 0;
        // Block first, then nominate the exact SID, so an interrupted
        // publication leaves the stream unmatched rather than pointed at a bank.
        write(context, s2cr_offset, bypass);
        barrier(context);
        saved->probe_s2cr = read(context, s2cr_offset);
        ok = saved->probe_s2cr == bypass;
        if (ok && !(smr & (1u << 31))) {
            write(context, smr_offset, match);
            barrier(context);
            saved->probe_smr = read(context, smr_offset);
            ok = saved->probe_smr == match;
        }
        if (!ok) {
            write(context, smr_offset, saved->smr);
            write(context, s2cr_offset, saved->s2cr);
            barrier(context);
            continue;
        }
        struct tb710fu_cdc_identity_route attested;
        if (tb710fu_cdc_identity_route_scan(read, context, &attested) !=
                TB710FU_CDC_SMMU_OK ||
            attested.slot != slot || attested.matching_streams != 1u) {
            write(context, smr_offset, saved->smr);
            write(context, s2cr_offset, saved->s2cr);
            barrier(context);
            continue;
        }
        return 1;
    }
    return 0;
}

#endif
