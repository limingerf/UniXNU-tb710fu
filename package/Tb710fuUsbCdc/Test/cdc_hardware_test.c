// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../CdcHardware.h"
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

static void test_route(void)
{
    struct tb710fu_cdc_identity_route r;
    fixture();
    assert(route(&r));
    assert(r.address_bits == 48 && r.matching_streams == 1 && r.slot == 7);
    registers[0] = 1; // CLIENTPD bypasses stream decisions globally.
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
    registers[(0x800 + 7 * 4) / 4] = (1u << 31) | (1u << 16) | 0x41;
    assert(!route(&r)); // A masked alias for 0x40 is not an exact route.
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

int main(void)
{
    test_clocks();
    test_phy();
    test_route();
    puts("CDC hardware predicates: clock/PHY/SMMU rejection and identity cases passed");
    return 0;
}
