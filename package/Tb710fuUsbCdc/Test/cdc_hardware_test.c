// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../CdcHardware.h"
#include "../CdcBoardPower.h"
#include "../CdcQcomRpmh.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t registers[TB710FU_USB_SMMU_BYTES / 4u];

static uint32_t read_register(void *context, unsigned offset)
{
    (void)context;
    assert(offset < sizeof(registers));
    assert(!(offset & 3u));
    return registers[offset / 4u];
}

static void fixture(void)
{
    memset(registers, 0, sizeof(registers));
    registers[0x20 / 4] = (1u << 30) | (1u << 27) | 128u;
    registers[0x24 / 4] = (6u << 28) | 128u; // 128 global pages, 128 banks.
    registers[0x28 / 4] = 0x555; // IAS, OAS and UBS each 48 bits.
    registers[(0x800 + 7 * 4) / 4] = (1u << 31) | 0x40u;
    registers[(0xc00 + 7 * 4) / 4] = 1u << 16; // Explicit bypass.
}

static void test_clocks(void)
{
    struct tb710fu_cdc_clock_state s = {0, 1u << 16, 1, 1, 1, 1, 1};
    assert(tb710fu_cdc_clocks_active(&s));
    s.power_status = 1u << 31; // GDSCR.PWR_ON is not the CFG status bit.
    assert(!tb710fu_cdc_clocks_active(&s));
    s.power_status = 1u << 16;
    s.reset = 1;
    assert(!tb710fu_cdc_clocks_active(&s));
    s.reset = 0;
    s.master = 0x80000003u; // Only cfg/aggre have Linux HWCG branch metadata.
    assert(!tb710fu_cdc_clocks_active(&s));
    s.master = 1;
    s.sleep = 0;
    assert(!tb710fu_cdc_clocks_active(&s));
    s.sleep = 1;
    s.mock_utmi = 0x80000001u;
    assert(!tb710fu_cdc_clocks_active(&s));
    s.mock_utmi = 1;
    s.config = s.aggregate = 0x80000003u;
    assert(tb710fu_cdc_clocks_active(&s));
    s.aggregate = 0x80000001u;
    assert(!tb710fu_cdc_clocks_active(&s));
    s.aggregate = 1;
    s.config = 2;
    assert(!tb710fu_cdc_clocks_active(&s));
    assert(TB710FU_USB_CLKREF + 0x118 == UINT64_C(0x01ff1118));
}

static void test_phy(void)
{
    struct tb710fu_cdc_phy_state s = {0, 1, 1, 0, 9, 0};
    assert(tb710fu_cdc_phy_adoptable(&s));
    s.control2 = 12;
    assert(tb710fu_cdc_phy_adoptable(&s));
    s.control2 = 8;
    assert(!tb710fu_cdc_phy_adoptable(&s));
    s.control2 = 0;
    s.common0 = 13; // SIDDQ is power-down, despite ENABLE and RETENABLEN.
    assert(!tb710fu_cdc_phy_adoptable(&s));
    s.common0 = 1;
    assert(!tb710fu_cdc_phy_adoptable(&s));
    s.common0 = 8;
    assert(!tb710fu_cdc_phy_adoptable(&s));
    s.common0 = 9;
    s.control0 = 0;
    assert(!tb710fu_cdc_phy_adoptable(&s));
    for (unsigned opmode = 1; opmode < 4; ++opmode) {
        s.control0 = 1 | (opmode << 3);
        assert(!tb710fu_cdc_phy_adoptable(&s));
    }
    s.control0 = 1;
    s.control5 = 2;
    assert(!tb710fu_cdc_phy_adoptable(&s));
    s.control5 = 0;
    s.reference = 0;
    assert(!tb710fu_cdc_phy_adoptable(&s));
    s.reference = 1;
    s.reset = 1;
    assert(!tb710fu_cdc_phy_adoptable(&s));
}

static int route(struct tb710fu_cdc_identity_route *r)
{
    return tb710fu_cdc_identity_route_read(read_register, NULL, r);
}

static enum tb710fu_cdc_smmu_result scan(struct tb710fu_cdc_identity_route *r)
{
    return tb710fu_cdc_identity_route_scan(read_register, NULL, r);
}

static unsigned stuck_offset = UINT32_MAX;

static void write_register(void *context, unsigned offset, uint32_t value)
{
    (void)context;
    assert(offset < sizeof(registers));
    assert(!(offset & 3u));
    // A register that silently drops a write models the resource-owned SMMU a
    // firmware handoff can leave behind; the caller must undo its own writes.
    if (offset == stuck_offset) return;
    registers[offset / 4u] = value;
}

static void barrier(void *context)
{
    (void)context;
}

static int attach(struct tb710fu_cdc_identity_route *planned,
                  struct tb710fu_cdc_smmu_saved *saved)
{
    return tb710fu_cdc_stream_bypass_attach(read_register, write_register,
                                            barrier, NULL, planned, saved);
}

static void test_route(void)
{
    struct tb710fu_cdc_identity_route r;
    fixture();
    assert(route(&r));
    assert(r.address_bits == 48 && r.matching_streams == 1 && r.slot == 7);    registers[0] = 1; // CLIENTPD bypasses stream decisions globally.
    assert(!route(&r));
    registers[0] = 8; // Extended SID semantics not implemented.
    assert(!route(&r));
    fixture();
    registers[0x20 / 4] &= ~(1u << 27);
    assert(!route(&r));
    fixture();
    registers[0x20 / 4] &= ~255u;
    assert(!route(&r));
    fixture();
    registers[0x24 / 4] |= 1u << 31; // 64-KiB register pages unsupported.
    assert(!route(&r));
    fixture();
    registers[0x24 / 4] = 7u << 28 | 128u; // Too many register pages.
    assert(!route(&r));
    fixture();
    registers[0x24 / 4] &= ~255u;
    assert(!route(&r));
    fixture();
    registers[(0x800 + 7 * 4) / 4] = 0x40; // Invalid stream match.
    assert(!route(&r));
    fixture();
    registers[(0x800 + 7 * 4) / 4] = (1u << 31) | 0x41; // Wrong SID.
    assert(!route(&r));
    fixture();
    // SMMUv2 matches (sid & ~MASK) == (SMR.ID & ~MASK), so a masked group whose
    // masked-away bits agree with 0x40 already routes this stream. This is the
    // state the device actually reports, and refusing it was the sidamb stop.
    registers[(0x800 + 7 * 4) / 4] = (1u << 31) | (1u << 16) | 0x41;
    assert(scan(&r) == TB710FU_CDC_SMMU_OK && r.masked && r.slot == 7);
    assert(route(&r));
    // A masked group whose stored ID disagrees on the visible bits is another
    // stream's range and must not be adopted.
    fixture();
    registers[(0x800 + 7 * 4) / 4] = (1u << 31) | (1u << 16) | 0x43;
    assert(scan(&r) != TB710FU_CDC_SMMU_OK);
    // A masked group is never rewritten: its range would carry other masters.
    {
        struct tb710fu_cdc_identity_route planned;
        struct tb710fu_cdc_smmu_saved saved;
        fixture();
        registers[(0x800 + 7 * 4) / 4] = (1u << 31) | (1u << 16) | 0x41;
        registers[(0xc00 + 7 * 4) / 4] = 2u << 16; // Faulting, still not ours.
        assert(scan(&planned) == TB710FU_CDC_SMMU_FAULTED);
        const uint32_t smr_before = registers[(0x800 + 7 * 4) / 4];
        const uint32_t s2cr_before = registers[(0xc00 + 7 * 4) / 4];
        assert(!attach(&planned, &saved));
        assert(registers[(0x800 + 7 * 4) / 4] == smr_before);
        assert(registers[(0xc00 + 7 * 4) / 4] == s2cr_before);
    }
    fixture();
    registers[(0x800 + 8 * 4) / 4] = (1u << 31) | 0x40;
    assert(!route(&r)); // Multiple matching stream slots.
    fixture();
    registers[(0xc00 + 7 * 4) / 4] = 2u << 16; // Faulting route.
    assert(!route(&r));
    fixture();
    registers[(0xc00 + 7 * 4) / 4] = 3; // Bank 3, translation disabled.
    registers[(0x1000 + 3 * 4) / 4] = 1u << 16;
    registers[(0x1800 + 3 * 4) / 4] = 1;
    assert(route(&r));
    assert(r.bank == 3 && r.context_offset == 0x83000);
    registers[0x83000 / 4] = 1;
    assert(!route(&r)); // An enabled page table cannot prove identity.
    registers[0x83000 / 4] = 0;
    registers[(0x1800 + 3 * 4) / 4] = 0;
    assert(!route(&r)); // Disabled 32-bit contexts are not this DMA route.
    registers[(0x1800 + 3 * 4) / 4] = 1;
    registers[(0x1000 + 3 * 4) / 4] = 2u << 16;
    assert(!route(&r)); // Stage 2 does not bypass translation.
    fixture();
    registers[(0xc00 + 7 * 4) / 4] = 128;
    assert(!route(&r)); // Bank outside NUMCB.
    fixture();
    registers[0x20 / 4] &= ~(1u << 30);
    registers[(0xc00 + 7 * 4) / 4] = 0;
    assert(!route(&r)); // No stage 1 capability.
    fixture();
    registers[0x28 / 4] = 0x501;
    assert(route(&r) && r.address_bits == 32);
    registers[0x28 / 4] = 0x656;
    assert(!route(&r)); // Reserved IDR2 address-size encoding.
}

static void test_route_classes(void)
{
    struct tb710fu_cdc_identity_route r;
    // The boot chain owns no route for 0x40: that is ABSENT, not a refusal.
    fixture();
    registers[(0x800 + 7 * 4) / 4] = 0;
    assert(scan(&r) == TB710FU_CDC_SMMU_ABSENT && r.slot == 0);
    assert(!route(&r));
    // A group that already faults is claimable, not ambiguous.
    fixture();
    registers[(0xc00 + 7 * 4) / 4] = 2u << 16;
    assert(scan(&r) == TB710FU_CDC_SMMU_FAULTED && r.slot == 7);
    // A masked group that visibly matches 0x40 routes it, so it is adopted and
    // flagged as shared rather than treated as ambiguous.
    fixture();
    registers[(0x800 + 7 * 4) / 4] = (1u << 31) | (1u << 16) | 0x41;
    assert(scan(&r) == TB710FU_CDC_SMMU_OK && r.masked);
    // A wide catch-all plus one exact group is not a conflict: the exact group
    // pins every SID bit, so it wins regardless of group order. This is the
    // shape Android leaves behind and it produced sidamb before.
    fixture();
    registers[(0x800 + 0 * 4) / 4] = (1u << 31) | (0x7fffu << 16) | 0x0000u;
    registers[(0xc00 + 0 * 4) / 4] = 2u << 16;
    assert(scan(&r) == TB710FU_CDC_SMMU_OK && r.slot == 7);
    assert(r.candidates == 2 && r.exact_candidates == 1);
    // Two exact groups remain ambiguous even with a catch-all present.
    fixture();
    registers[(0x800 + 0 * 4) / 4] = (1u << 31) | (0x7fffu << 16) | 0x0000u;
    registers[(0xc00 + 0 * 4) / 4] = 2u << 16;
    registers[(0x800 + 8 * 4) / 4] = (1u << 31) | 0x40;
    assert(scan(&r) == TB710FU_CDC_SMMU_AMBIGUOUS);
    // Two wide groups with no exact one are ambiguous too.
    fixture();
    registers[(0x800 + 7 * 4) / 4] = 0;
    registers[(0xc00 + 7 * 4) / 4] = 0;
    registers[(0x800 + 0 * 4) / 4] = (1u << 31) | (0x7fffu << 16) | 0x0000u;
    registers[(0xc00 + 0 * 4) / 4] = 2u << 16;
    registers[(0x800 + 1 * 4) / 4] = (1u << 31) | (0x7ff0u << 16) | 0x0000u;
    registers[(0xc00 + 1 * 4) / 4] = 2u << 16;
    assert(scan(&r) == TB710FU_CDC_SMMU_AMBIGUOUS);
    // A lone catch-all is a usable route, flagged as shared.
    fixture();
    registers[(0x800 + 7 * 4) / 4] = 0;
    registers[(0xc00 + 7 * 4) / 4] = 0;
    registers[(0x800 + 0 * 4) / 4] = (1u << 31) | (1u << 16) | 0x0041u;
    registers[(0xc00 + 0 * 4) / 4] = 1u << 16;
    assert(scan(&r) == TB710FU_CDC_SMMU_OK && r.slot == 0 && r.masked);
    // Two exact groups for one SID are genuinely ambiguous.
    fixture();
    registers[(0x800 + 8 * 4) / 4] = (1u << 31) | 0x40;
    assert(scan(&r) == TB710FU_CDC_SMMU_AMBIGUOUS);
    // A live fast-path translation is someone else's route.
    fixture();
    registers[(0xc00 + 7 * 4) / 4] = 0;
    assert(scan(&r) == TB710FU_CDC_SMMU_TRANSLATES);
    // Geometry failures stay distinguishable from stream failures.
    fixture();
    registers[0] = 1;
    assert(scan(&r) == TB710FU_CDC_SMMU_GEOMETRY);
    // The storage planner expects the top group free, so CDC never takes it:
    // with only group 127 available the claim must report FULL.
    fixture();
    for (unsigned i = 0; i < 127u; ++i)
        registers[(0x800 + i * 4) / 4] = (1u << 31) | (0x100u + i);
    registers[(0x800 + 127 * 4) / 4] = 0;
    assert(scan(&r) == TB710FU_CDC_SMMU_FULL);
}

static void test_route_claim(void)
{
    struct tb710fu_cdc_identity_route planned;
    struct tb710fu_cdc_smmu_saved saved;
    // Claim a free group, then require the scan to attest it.
    fixture();
    registers[(0x800 + 7 * 4) / 4] = 0;
    registers[(0xc00 + 7 * 4) / 4] = 0;
    assert(scan(&planned) == TB710FU_CDC_SMMU_ABSENT && planned.slot == 0);
    assert(attach(&planned, &saved));
    assert(saved.slot == 0 && saved.smr == 0 && saved.s2cr == 0);
    assert(registers[0x800 / 4] == ((1u << 31) | 0x40u));
    assert(registers[0xc00 / 4] == (1u << 16));
    assert(scan(&planned) == TB710FU_CDC_SMMU_OK && planned.slot == 0);
    // Claiming a faulted group only rewrites its action.
    fixture();
    registers[(0xc00 + 7 * 4) / 4] = 2u << 16;
    assert(scan(&planned) == TB710FU_CDC_SMMU_FAULTED && planned.slot == 7);
    assert(attach(&planned, &saved));
    assert(saved.smr == ((1u << 31) | 0x40u) && saved.s2cr == (2u << 16));
    assert(registers[(0xc00 + 7 * 4) / 4] == (1u << 16));
    assert(scan(&planned) == TB710FU_CDC_SMMU_OK);
    // A publication that does not read back is undone in full.
    fixture();
    registers[(0x800 + 7 * 4) / 4] = 0;
    assert(scan(&planned) == TB710FU_CDC_SMMU_ABSENT && planned.slot == 0);
    stuck_offset = 0xc00; // The S2CR write is silently dropped.
    assert(!attach(&planned, &saved));
    stuck_offset = UINT32_MAX;
    assert(registers[0x800 / 4] == 0 && registers[0xc00 / 4] == 0);
    assert(scan(&planned) == TB710FU_CDC_SMMU_ABSENT);
}

static void put16(uint8_t *target, uint16_t value)
{
    target[0] = (uint8_t)value;
    target[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *target, uint32_t value)
{
    put16(target, (uint16_t)value);
    put16(target + 2, (uint16_t)(value >> 16));
}

static void make_cmd_db(uint8_t *db, size_t bytes)
{
    assert(bytes >= 172u);
    memset(db, 0, bytes);
    put32(db + 4, TB710FU_CDC_CMD_DB_MAGIC);
    put16(db + 8, 1u);
    put16(db + 12, 24u);
    put16(db + 14, 1u);
    memcpy(db + 144, "ldoi1", 5);
    put32(db + 160, 0x12345000u);
    put16(db + 164, 4u);
    put16(db + 166, 0u);
    put16(db + 168, 10u);
    put16(db + 170, 30u);
}

static void test_rpmh(void)
{
    uint8_t db[256];
    struct tb710fu_cdc_rpmh_resource resource;
    uint32_t index = UINT32_MAX;
    make_cmd_db(db, sizeof(db));
    assert(tb710fu_cdc_cmd_db_find(db, sizeof(db), "ldoi1", &resource) ==
           TB710FU_CDC_RPMH_OK);
    assert(resource.address == 0x12345000u && resource.level_count == 2u);
    assert(resource.levels[0] == 10u && resource.levels[1] == 30u);
    assert(tb710fu_cdc_rpmh_level_index(&resource, 11u, &index) ==
           TB710FU_CDC_RPMH_OK && index == 1u);
    assert(tb710fu_cdc_rpmh_level_index(&resource, 0u, &index) ==
           TB710FU_CDC_RPMH_OK && index == 0u);
    assert(tb710fu_cdc_cmd_db_find(db, sizeof(db), "ldob5", &resource) ==
           TB710FU_CDC_RPMH_MISSING);
    assert(tb710fu_cdc_cmd_db_find(db, 100u, "ldoi1", &resource) ==
           TB710FU_CDC_RPMH_NO_DB);
    db[4] = 0;
    assert(tb710fu_cdc_cmd_db_find(db, sizeof(db), "ldoi1", &resource) ==
           TB710FU_CDC_RPMH_NO_DB);

    const struct tb710fu_cdc_rsc_layout layout =
        tb710fu_cdc_rsc_layout(3u << 16);
    uint32_t addresses[] = {0x100u, 0x104u, 0x108u};
    uint32_t values[] = {7u, 880u, 1u};
    struct tb710fu_cdc_rpmh_write writes[16];
    const unsigned count = tb710fu_cdc_rpmh_active_request(
        &layout, 2u, addresses, values, 3u, writes, 16u);
    assert(count == 15u);
    assert(writes[0].offset == 2u * 672u + layout.control && writes[0].value == 0);
    assert(writes[1].offset == layout.irq_clear && writes[1].value == 4u);
    assert(writes[2].offset == 2u * 672u + layout.cmd_msgid);
    assert(writes[4].offset == 2u * 672u + layout.cmd_data && writes[4].value == 7u);
    assert(writes[14].offset == 2u * 672u + layout.control);
    assert(writes[14].value == ((1u << 16) | (1u << 24)));
    assert(tb710fu_cdc_rpmh_finish(&layout, 2u, writes, 3u) == 3u);
    assert(writes[0].offset == layout.irq_clear && writes[0].value == 4u);
    assert(writes[1].offset == 2u * 672u + layout.control && writes[1].value == 0);
    assert(tb710fu_cdc_rpmh_active_request(&layout, 2u, addresses, values,
        3u, writes, 14u) == 0);
}

static void test_board_power(void)
{
    assert(TB710FU_CDC_PHY_RAIL_COUNT == 4u);
    assert(strcmp(tb710fu_cdc_phy_rails[0].resource, "ldoi1") == 0);
    assert(tb710fu_cdc_phy_rails[0].millivolts == 880u);
    assert(strcmp(tb710fu_cdc_phy_rails[1].resource, "ldoi3") == 0);
    assert(tb710fu_cdc_phy_rails[1].millivolts == 1200u);
    assert(strcmp(tb710fu_cdc_phy_rails[2].resource, "ldob15") == 0);
    assert(tb710fu_cdc_phy_rails[2].millivolts == 1800u);
    assert(strcmp(tb710fu_cdc_phy_rails[3].resource, "ldob5") == 0);
    assert(tb710fu_cdc_phy_rails[3].millivolts == 3104u);
    assert(TB710FU_CDC_SPMI_REPEATER_PPID == 0x7fdu);
    assert(TB710FU_CDC_PHY_REF_HZ == 38400000u);
    assert(TB710FU_CDC_PHY_INIT_COUNT == 28u);
    assert(tb710fu_cdc_phy_init[0].offset == 0x94u);
    assert(tb710fu_cdc_phy_init[6].offset == 0x54u);
    assert(tb710fu_cdc_phy_init[6].value == 0x40u);
    assert(tb710fu_cdc_phy_init[7].offset == 0x5cu);
    assert(tb710fu_cdc_phy_init[7].value == 0xc8u);
    assert(tb710fu_cdc_phy_init[8].offset == 0x60u);
    assert(tb710fu_cdc_phy_init[8].mask == 0x0fu);
    assert(tb710fu_cdc_phy_init[27].offset == 0x94u);
    assert(tb710fu_cdc_phy_init[27].value == 0);
    assert(tb710fu_cdc_masked_value(0xa5a500ffu, 0x0fu, 0x05u) ==
           0xa5a500f5u);
}

int main(void)
{
    test_clocks();
    test_phy();
    test_route();
    test_route_classes();
    test_route_claim();
    test_rpmh();
    test_board_power();
    puts("CDC hardware predicates: clock/PHY/SMMU, RPMh and board sequencing cases passed");
    return 0;
}
