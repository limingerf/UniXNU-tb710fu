/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host-side checks for the portable half of Tb710fuWifi: TLMM words, BAR sizing/placement, iATU layouts, the apps-SMMU
 * identity plan, MHI ring/context/BHIe builders and the MHI boot phases, the firmware manifest, and the
 * QRTR/QMI/HTC/WMI/DP encoders. No XNU or IOKit header is touched, so clang or gcc can build it anywhere. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Tb710fuWifiBoard.h"
#include "Tb710fuWifiManifest.h"
#include "Tb710fuWifiMhi.h"
#include "Tb710fuWifiProto.h"

static unsigned gChecks, gFailures;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++gChecks;                                                               \
        if (!(cond)) {                                                           \
            ++gFailures;                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (what));              \
        }                                                                        \
    } while (0)

static void testBoard(void)
{
    CHECK(tb710fu_wifi_tlmm_config(TB710FU_WIFI_FUNC_GPIO, 2, TB710FU_WIFI_PULL_DOWN, 1) == 0x0201u,
          "perst pad word (gpio, 2 mA, pull-down, output)");
    CHECK(tb710fu_wifi_tlmm_config(TB710FU_WIFI_FUNC_GPIO, 8, TB710FU_WIFI_PULL_DOWN, 1) == 0x0201u + (3u << 6),
          "wlan pad drive 8 mA");
    CHECK(tb710fu_wifi_tlmm_config(3, 2, TB710FU_WIFI_PULL_UP, 0) == 0x0eu, "clkreq function 3, pull-up, input");
    CHECK(tb710fu_wifi_tlmm_level(1) == 2u && tb710fu_wifi_tlmm_level(0) == 0u, "TLMM IO level coding");

    struct tb710fu_wifi_pin_plan pins;
    tb710fu_wifi_pin_plan(&pins);
    CHECK(pins.perst_cfg == 0x0201u && pins.perst_level == 0u, "PERST asserted at plan time");
    CHECK(pins.wlan_cfg == tb710fu_wifi_tlmm_config(0, 8, 1, 1) && pins.wlan_level == 2u, "WLAN_EN plan");
    CHECK(pins.bt_cfg == tb710fu_wifi_tlmm_config(0, 16, 0, 1) && pins.bt_level == 0u, "BT_EN held low");
    CHECK(pins.wake_cfg == tb710fu_wifi_tlmm_config(0, 2, 2, 0) && pins.wake_level == 0u, "WAKE input");

    struct tb710fu_wifi_gcc_plan gcc;
    tb710fu_wifi_gcc_plan(&gcc);
    CHECK(gcc.vote8_mask == 0x7fu && gcc.collapse_clear == ((1u << 0) | (1u << 3)), "GCC vote/collapse plan");
    CHECK(tb710fu_wifi_gdsc_on(1u << 16) && !tb710fu_wifi_gdsc_on(0), "GDSC power-up complete bit");
    CHECK(tb710fu_wifi_branch_halted(1u << 31) && !tb710fu_wifi_branch_halted(1u << 30), "branch halt bit31");

    CHECK(tb710fu_wifi_bar_size(0xfffff000u) == 0x1000u, "4 KiB BAR mask");
    CHECK(tb710fu_wifi_bar_size(0xfff00000u) == 0x100000u, "1 MiB BAR mask");
    CHECK(tb710fu_wifi_bar_size(0) == 0, "zero BAR mask is refused");
    uint64_t placed = 0;
    CHECK(tb710fu_wifi_bar_place(0x40000, &placed, TB710FU_WIFI_PCIE_MMIO_BASE, TB710FU_WIFI_PCIE_MMIO_BYTES) &&
          placed == TB710FU_WIFI_PCIE_MMIO_BASE, "64 KiB BAR placed in the DTS MMIO window");
    CHECK(!tb710fu_wifi_bar_place(0x40000000, &placed, TB710FU_WIFI_PCIE_MMIO_BASE, TB710FU_WIFI_PCIE_MMIO_BYTES),
          "over-large BAR is refused");

    struct tb710fu_wifi_atu_region region;
    tb710fu_wifi_atu_config(&region, TB710FU_WIFI_PCIE_CFG_BASE, 0x100000, TB710FU_WIFI_PCI_BUS);
    CHECK(region.ctrl1 == TB710FU_WIFI_ATU_CTRL1_CFG0 && region.ctrl2 == TB710FU_WIFI_ATU_CTRL2_ENABLE,
          "iATU config region control words");
    CHECK(region.target == (uint64_t)TB710FU_WIFI_PCI_BUS << 24, "config target carries bus 1 in bits 31:24");
    CHECK(region.limit == TB710FU_WIFI_PCIE_CFG_BASE + 0xfffff, "config limit covers 1 MiB");
    tb710fu_wifi_atu_memory(&region, TB710FU_WIFI_PCIE_MMIO_BASE, TB710FU_WIFI_PCIE_MMIO_BYTES,
                            TB710FU_WIFI_PCIE_MMIO_BASE);
    CHECK(region.ctrl1 == TB710FU_WIFI_ATU_CTRL1_MEM && region.target == TB710FU_WIFI_PCIE_MMIO_BASE,
          "identity memory window");
    CHECK(region.limit == TB710FU_WIFI_PCIE_MMIO_BASE + TB710FU_WIFI_PCIE_MMIO_BYTES - 1, "memory limit");
}

static void testManifest(void)
{
    const uint64_t amssBytes = 1u << 20, m3Bytes = 64u << 10, boardBytes = 64u << 10;
    uint8_t *amss = calloc(1, (size_t)amssBytes);
    uint8_t *m3 = calloc(1, (size_t)m3Bytes);
    uint8_t *board = calloc(1, (size_t)boardBytes);
    CHECK(amss && m3 && board, "manifest test allocations");
    if (!amss || !m3 || !board) {
        free(amss);
        free(m3);
        free(board);
        return;
    }
    const uint8_t elf[] = { 0x7f, 'E', 'L', 'F', 1, 1 };
    memcpy(amss, elf, sizeof elf);
    amss[18] = 0x28;
    memcpy(m3, elf, sizeof elf);
    m3[18] = 0x28;
    memcpy(board, "QCA-ATH12K-BOARD", 16);
    board[16] = 0;

    struct tb710fu_wifi_fw_info info;
    CHECK(tb710fu_wifi_fw_check(TB710FU_WIFI_FW_AMSS_NAME, amss, amssBytes, &info) == TB710FU_WIFI_FW_OK &&
          info.bits == TB710FU_WIFI_FW_BIT_AMSS && info.elf_class == 1 && info.elf_machine == 0x28,
          "amss.bin ELF32 LE ARM accepted");
    amss[5] = 2;
    CHECK(tb710fu_wifi_fw_check(TB710FU_WIFI_FW_AMSS_NAME, amss, amssBytes, &info) == TB710FU_WIFI_FW_BAD_MAGIC,
          "big-endian amss refused");
    amss[5] = 1;
    CHECK(tb710fu_wifi_fw_check(TB710FU_WIFI_FW_M3_NAME, m3, m3Bytes, &info) == TB710FU_WIFI_FW_OK,
          "m3.bin ELF accepted");
    CHECK(tb710fu_wifi_fw_check(TB710FU_WIFI_FW_BOARD_NAME, board, boardBytes, &info) == TB710FU_WIFI_FW_OK &&
          info.bits == TB710FU_WIFI_FW_BIT_BOARD, "board-2.bin container accepted");
    board[3] = 'X';
    CHECK(tb710fu_wifi_fw_check(TB710FU_WIFI_FW_BOARD_NAME, board, boardBytes, &info) == TB710FU_WIFI_FW_BAD_MAGIC,
          "wrong board magic refused");
    CHECK(tb710fu_wifi_fw_check("nope.bin", amss, amssBytes, &info) == TB710FU_WIFI_FW_UNKNOWN_NAME,
          "unknown firmware name refused");
    CHECK(tb710fu_wifi_fw_check(TB710FU_WIFI_FW_AMSS_NAME, amss, 4096, &info) == TB710FU_WIFI_FW_TOO_SMALL,
          "short amss refused");

    struct tb710fu_wifi_manifest manifest = {0};
    CHECK(tb710fu_wifi_manifest_note(&manifest, &(struct tb710fu_wifi_fw_info){ .bits = TB710FU_WIFI_FW_BIT_AMSS,
                                                                              .bytes = amssBytes }) &&
          !tb710fu_wifi_manifest_complete(&manifest), "manifest tracks a single file");
    manifest.present = TB710FU_WIFI_FW_BIT_ALL;
    manifest.amss_bytes = amssBytes;
    manifest.m3_bytes = m3Bytes;
    manifest.board_bytes = boardBytes;
    CHECK(tb710fu_wifi_manifest_complete(&manifest), "manifest complete with all three files");
    free(amss);
    free(m3);
    free(board);
}

static void testMhiBuilders(void)
{
    Tb710fuWifiMhiPlan plan;
    CHECK(tb710fu_wifi_mhi_plan_make(0x10000000u, &plan), "MHI plan");
    CHECK(plan.ch_off == 0 && plan.ev_off == 128 * 44 && plan.cmd_off == plan.ev_off + 88,
          "context offsets: 128 channels then 2 events then command");
    CHECK(plan.ring_off == 0x2000u, "rings page-aligned after contexts");
    CHECK(plan.ev_bytes[0] == 32 * 16 && plan.ev_bytes[1] == 256 * 16, "event ring sizes 32/256 elements");
    CHECK(plan.ev_base[1] == 0x10000000u + 0x2000u + 512, "event1 base follows event0");
    CHECK(plan.cmd_base == 0x10000000u + 0x2000u + 512 + 4096 && plan.cmd_bytes == 8 * 16, "command ring");
    CHECK(plan.total_bytes == 0x4000u, "whole plan rounds to one page");

    uint8_t *buffer = calloc(1, plan.total_bytes);
    CHECK(buffer && tb710fu_wifi_mhi_contexts(buffer, plan.total_bytes, 0x10000000u, 0, &plan),
          "context array written");
    if (buffer) {
        const uint32_t ev0 = plan.ev_off;
        CHECK(*(uint32_t *)(buffer + ev0 + TB710FU_WIFI_MHI_CTX_TYPE) == TB710FU_WIFI_MHI_ER_TYPE_VALID,
              "event context type valid");
        CHECK(*(uint32_t *)(buffer + ev0 + TB710FU_WIFI_MHI_CTX_MSIVEC) == 0, "event0 MSI vector");
        CHECK(*(uint64_t *)(buffer + ev0 + TB710FU_WIFI_MHI_CTX_LEN) == 512, "event0 length");
        CHECK(*(uint64_t *)(buffer + ev0 + TB710FU_WIFI_MHI_CTX_RP) == 0x10000000u + 0x2000u,
              "event0 read pointer at base");
        CHECK(*(uint64_t *)(buffer + ev0 + TB710FU_WIFI_MHI_CTX_WP) == 0x10000000u + 0x2000u + 512 - 16,
              "event0 write pointer at last element");
        CHECK(*(uint64_t *)(buffer + plan.cmd_off + TB710FU_WIFI_MHI_CTX_LEN) == 128, "command ring length");
        CHECK(*(uint32_t *)(buffer + plan.ev_off + 44 + TB710FU_WIFI_MHI_CTX_MODERATION) == (1u << 16),
              "event1 moderation flag");
        free(buffer);
    }

    Tb710fuWifiMhiMmio mmio;
    CHECK(tb710fu_wifi_mhi_mmio(&plan, 0x10000000u, &mmio), "MHI register builder");
    CHECK(mmio.words[0][0] == TB710FU_WIFI_MHI_CTX_CH_BASE && mmio.words[0][1] == 0x10000000u, "CCABAP");
    CHECK(mmio.words[1][0] == TB710FU_WIFI_MHI_CTX_EV_BASE && mmio.words[1][1] == 0x10000000u + 0x1600u, "ECABAP");
    CHECK(mmio.words[2][1] == 0x10000000u + 0x1658u, "CRCBAP");
    CHECK(mmio.cfg_mask == (0xffu << 16) && mmio.cfg_value == (2u << 16), "MHICFG NER = 2 events");

    CHECK(tb710fu_wifi_mhi_ring_advance(0x10000000u + 0x2000u + 512 - 16, 0x10000000u + 0x2000u, 512) ==
          0x10000000u + 0x2000u, "event ring wraps");
    CHECK(tb710fu_wifi_mhi_ring_advance(0x10000000u + 0x2000u, 0x10000000u + 0x2000u, 512) ==
          0x10000000u + 0x2010u, "event ring advances by 16 bytes");

    uint64_t addresses[4] = { 0x20000000u, 0x20080000u, 0x20100000u, 0x20180000u };
    Tb710fuWifiBhieEntry table[4];
    CHECK(tb710fu_wifi_bhie_table(table, 4, addresses, 0x180000u, 0x80000u) == 3, "BHIe splits 1.5 MiB into 3");
    CHECK(table[0].address == 0x20000000u && table[0].size == 0x80000u, "BHIe entry 0");
    CHECK(table[2].address == 0x20100000u && table[2].size == 0x80000u, "BHIe entry 2");
    CHECK(tb710fu_wifi_bhie_table(table, 2, addresses, 0x180000u, 0x80000u) == 0, "too few entries refused");

    uint8_t entry[16];
    for (unsigned i = 0; i < 16; ++i) entry[i] = (uint8_t)(i + 1);
    Tb710fuWifiMhiEvent event;
    tb710fu_wifi_mhi_event_read(entry, &event);
    CHECK(event.dword[0] == 0x04030201u && event.dword[3] == 0x100f0e0du, "16-byte event record decoding");
}

/* ---- MHI boot phases against a register file ---- */
struct FakeMhi {
    uint32_t regs[0x400];
    uint32_t writes;
};

static uint32_t fakeRead(void *ctx, uint32_t reg)
{
    struct FakeMhi *fake = ctx;
    return reg < sizeof(fake->regs) ? fake->regs[reg / 4] : 0xffffffffu;
}

static void fakeWrite(void *ctx, uint32_t reg, uint32_t value)
{
    struct FakeMhi *fake = ctx;
    if (reg < sizeof(fake->regs)) {
        fake->regs[reg / 4] = value;
        ++fake->writes;
    }
}

static void testMhiBoot(void)
{
    struct FakeMhi fake = {0};
    Tb710fuWifiMhiIo io = { &fake, fakeRead, fakeWrite };
    Tb710fuWifiMhi mhi;
    fake.regs[TB710FU_WIFI_MHI_REGLEN / 4] = 0x400; /* 1 KiB register file */
    fake.regs[TB710FU_WIFI_MHI_BHIOFF / 4] = 0x100;
    fake.regs[TB710FU_WIFI_MHI_BHIEOFF / 4] = 0x200;
    fake.regs[(0x100 + TB710FU_WIFI_MHI_BHI_EXECENV) / 4] = TB710FU_WIFI_EE_PBL;
    CHECK(tb710fu_wifi_mhi_open(&mhi, &io) == 0 && mhi.bhi == 0x100 && mhi.bhie == 0x200,
          "MHI layout read from BHIOFF/BHIEOFF");
    CHECK(tb710fu_wifi_mhi_ee(&mhi) == TB710FU_WIFI_EE_PBL, "execution environment read");
    CHECK(tb710fu_wifi_mhi_state(&mhi) == TB710FU_WIFI_MHI_STATE_RESET, "reset state");

    uint32_t phase = tb710fu_wifi_mhi_begin(&mhi, 0, 0x30000000u, 0x40000u, 0x31000000u, 0x30u, 1);
    CHECK(phase == TB710FU_WIFI_MHI_SBL_LOADING, "begin starts the BHI image");
    CHECK(fake.regs[(0x100 + TB710FU_WIFI_MHI_BHI_IMGADDR_LOW) / 4] == 0x30000000u &&
          fake.regs[(0x100 + TB710FU_WIFI_MHI_BHI_IMGSIZE) / 4] == 0x40000u &&
          fake.regs[(0x100 + TB710FU_WIFI_MHI_BHI_IMGTXDB) / 4] == 1,
          "BHI image address/size/doorbell written");

    fake.regs[(0x100 + TB710FU_WIFI_MHI_BHI_STATUS) / 4] = 2u << 30; /* transfer success */
    fake.regs[TB710FU_WIFI_MHI_CTRL / 4] = 0;
    fake.regs[TB710FU_WIFI_MHI_STATUS / 4] = TB710FU_WIFI_MHI_STATUS_READY;
    phase = tb710fu_wifi_mhi_step(&mhi);
    CHECK(phase == TB710FU_WIFI_MHI_NEED_CONTEXTS, "BHI success leads to the context hand-off");

    phase = tb710fu_wifi_mhi_contexts_done(&mhi);
    CHECK(phase == TB710FU_WIFI_MHI_WAIT_SBL, "M0 requested, waiting for the SBL");
    fake.regs[(0x100 + TB710FU_WIFI_MHI_BHI_EXECENV) / 4] = TB710FU_WIFI_EE_SBL;
    phase = tb710fu_wifi_mhi_step(&mhi);
    CHECK(phase == TB710FU_WIFI_MHI_IMAGE_LOADING, "SBL present starts the BHIe image");
    CHECK(fake.regs[(0x200 + TB710FU_WIFI_MHI_BHIE_TXVECADDR_LOW) / 4] == 0x31000000u &&
          fake.regs[(0x200 + TB710FU_WIFI_MHI_BHIE_TXVECSIZE) / 4] == 0x30u,
          "BHIe vector table address/size written");
    CHECK(fake.regs[(0x200 + TB710FU_WIFI_MHI_BHIE_TXVECDB) / 4] == 2, "BHIe doorbell carries the second sequence");

    fake.regs[(0x200 + TB710FU_WIFI_MHI_BHIE_TXVECSTATUS) / 4] = (2u << 30) | 2u;
    phase = tb710fu_wifi_mhi_step(&mhi);
    CHECK(phase == TB710FU_WIFI_MHI_WAIT_MISSION, "BHIe success waits for mission mode");
    fake.regs[(0x100 + TB710FU_WIFI_MHI_BHI_EXECENV) / 4] = TB710FU_WIFI_EE_AMSS;
    phase = tb710fu_wifi_mhi_step(&mhi);
    CHECK(phase == TB710FU_WIFI_MHI_DONE, "AMSS environment = mission mode");

    /* A device that already runs AMSS is accepted by begin; a passthrough EE is refused. */
    struct FakeMhi already = {0};
    already.regs[TB710FU_WIFI_MHI_REGLEN / 4] = 0x400;
    already.regs[TB710FU_WIFI_MHI_BHIOFF / 4] = 0x100;
    already.regs[TB710FU_WIFI_MHI_BHIEOFF / 4] = 0x200;
    already.regs[(0x100 + TB710FU_WIFI_MHI_BHI_EXECENV) / 4] = TB710FU_WIFI_EE_AMSS;
    already.regs[TB710FU_WIFI_MHI_STATUS / 4] = TB710FU_WIFI_MHI_STATUS_READY;
    Tb710fuWifiMhi second;
    Tb710fuWifiMhiIo io2 = { &already, fakeRead, fakeWrite };
    CHECK(tb710fu_wifi_mhi_open(&second, &io2) == 0, "second MHI opens");
    CHECK(tb710fu_wifi_mhi_begin(&second, 0, 0x30000000u, 0x40000u, 0x31000000u, 0x30u, 3) ==
          TB710FU_WIFI_MHI_FAILED && second.error == TB710FU_WIFI_MHI_ERR_NOT_PBL, "AMSS device is not a boot target");
    already.regs[(0x100 + TB710FU_WIFI_MHI_BHI_EXECENV) / 4] = 5; /* passthrough: not a boot environment */
    Tb710fuWifiMhi third;
    Tb710fuWifiMhiIo io3 = { &already, fakeRead, fakeWrite };
    CHECK(tb710fu_wifi_mhi_open(&third, &io3) == 0, "third MHI opens");
    CHECK(tb710fu_wifi_mhi_begin(&third, 0, 0x30000000u, 0x40000u, 0x31000000u, 0x30u, 1) ==
          TB710FU_WIFI_MHI_FAILED && third.error == TB710FU_WIFI_MHI_ERR_NOT_PBL,
          "passthrough EE refused");

    /* Out-of-range BHI is a layout error, never a read past the mapped block. */
    struct FakeMhi bad = {0};
    bad.regs[TB710FU_WIFI_MHI_REGLEN / 4] = 0x100;
    bad.regs[TB710FU_WIFI_MHI_BHIOFF / 4] = 0x100;
    bad.regs[TB710FU_WIFI_MHI_BHIEOFF / 4] = 0x200;
    Tb710fuWifiMhiIo io4 = { &bad, fakeRead, fakeWrite };
    Tb710fuWifiMhi fourth;
    CHECK(tb710fu_wifi_mhi_open(&fourth, &io4) == -1 && fourth.error == TB710FU_WIFI_MHI_ERR_LAYOUT,
          "BHI outside the register block refused");
}

/* ---- apps-SMMU identity plan ---- */
struct FakeSmmu {
    uint32_t regs[TB710FU_WIFI_SMMU_BYTES / 4];
};

static uint32_t smmuFakeRead(void *ctx, uint32_t offset)
{
    struct FakeSmmu *fake = ctx;
    return offset < sizeof(fake->regs) ? fake->regs[offset / 4] : 0xffffffffu;
}

static void smmuFakeWrite(void *ctx, uint32_t offset, uint32_t value)
{
    struct FakeSmmu *fake = ctx;
    if (offset < sizeof(fake->regs)) fake->regs[offset / 4] = value;
}

static void smmuFakeBarrier(void *ctx)
{
    (void)ctx;
}

static void testSmmu(void)
{
    struct FakeSmmu *fake = calloc(1, sizeof *fake);
    CHECK(fake != NULL, "SMMU test allocation");
    if (!fake) return;
    const unsigned groups = 32, banks = 16, pages = 8;
    fake->regs[0x000 / 4] = 0; /* SCR0: translation off for the plan's sanity gate */
    fake->regs[0x020 / 4] = groups | (1u << 27);
    fake->regs[0x024 / 4] = banks | (2u << 28);
    fake->regs[0x028 / 4] = 0x555;
    /* Group 5 names SID 0x1401 without a mask and routes it to bank 3, which holds the identity context. */
    fake->regs[(0x800 + 5 * 4) / 4] = 0x80000000u | TB710FU_WIFI_ENDPOINT_SID;
    fake->regs[(0xc00 + 5 * 4) / 4] = 3;
    fake->regs[((pages + 3) * 4096) / 4] = 0;
    fake->regs[(0x1000 + 3 * 4) / 4] = 0x10000u;
    fake->regs[(0x1800 + 3 * 4) / 4] = 1;
    struct tb710fu_wifi_smmu_io io = { fake, smmuFakeRead, smmuFakeWrite, smmuFakeBarrier };
    struct tb710fu_wifi_smmu_plan plan;
    CHECK(tb710fu_wifi_smmu_plan(&io, TB710FU_WIFI_ENDPOINT_SID, 3, &plan) == TB710FU_WIFI_SMMU_OK,
          "existing identity route is reused");
    CHECK(plan.reuse == 1 && plan.group == 5 && plan.bank_ready == 1, "reuse plan names group 5, bank 3");
    CHECK(tb710fu_wifi_smmu_apply(&io, TB710FU_WIFI_ENDPOINT_SID, &plan), "apply verifies the existing bank");

    /* A masked group that covers the SID must never be rewritten. */
    fake->regs[(0x800 + 5 * 4) / 4] = 0; /* drop the exact route so the masked group decides */
    fake->regs[(0x800 + 6 * 4) / 4] = 0x80000000u | (0x1u << 16) | 0x1400u;
    CHECK(tb710fu_wifi_smmu_plan(&io, TB710FU_WIFI_ENDPOINT_SID, 3, &plan) == TB710FU_WIFI_SMMU_CONFLICT,
          "masked group conflict refused");

    /* Fresh bank: no group names the SID, so the first free group is claimed and the bank programmed. */
    struct FakeSmmu *fresh = calloc(1, sizeof *fresh);
    CHECK(fresh != NULL, "fresh SMMU allocation");
    if (fresh) {
        fresh->regs[0x000 / 4] = 0;
        fresh->regs[0x020 / 4] = groups | (1u << 27);
        fresh->regs[0x024 / 4] = banks | (2u << 28);
        fresh->regs[0x028 / 4] = 0x555;
        struct tb710fu_wifi_smmu_io io2 = { fresh, smmuFakeRead, smmuFakeWrite, smmuFakeBarrier };
        CHECK(tb710fu_wifi_smmu_plan(&io2, TB710FU_WIFI_ENDPOINT_SID, 3, &plan) == TB710FU_WIFI_SMMU_OK,
              "free group/bank plan");
        CHECK(plan.reuse == 0 && plan.group == 0 && plan.bank_ready == 0, "first free group is 0");
        CHECK(tb710fu_wifi_smmu_apply(&io2, TB710FU_WIFI_ENDPOINT_SID, &plan), "fresh identity context applied");
        CHECK(fresh->regs[((pages + 3) * 4096) / 4] == 0 && fresh->regs[(0x1000 + 12) / 4] == 0x10000u &&
              fresh->regs[(0x1800 + 12) / 4] == 1, "SCTLR/CBAR/CBA2R readback");
        CHECK(fresh->regs[0xc00 / 4] == 3 && fresh->regs[0x800 / 4] == (0x80000000u | TB710FU_WIFI_ENDPOINT_SID),
              "S2CR/SMR readback");
        free(fresh);
    }
    CHECK(tb710fu_wifi_smmu_plan(&io, TB710FU_WIFI_ENDPOINT_SID, 0, &plan) == TB710FU_WIFI_SMMU_BAD_BANK,
          "bank 0 is not a claimable bank");
    free(fake);
}

static void testProto(void)
{
    struct tb710fu_wifi_qrtr qrtr = { 1, TB710FU_WIFI_QRTR_DATA, 0, 0x10, 0x11, 0x20, 0, 3 };
    uint8_t payload[3] = { 0xaa, 0xbb, 0xcc };
    uint8_t out[64];
    const uint32_t qrtrBytes = tb710fu_wifi_qrtr_encode(&qrtr, payload, out, sizeof out);
    CHECK(qrtrBytes == 32 + 3, "QRTR header + payload length");
    struct tb710fu_wifi_qrtr decoded;
    CHECK(tb710fu_wifi_qrtr_decode(out, qrtrBytes, &decoded) && decoded.type == TB710FU_WIFI_QRTR_DATA &&
          decoded.size == 3 && decoded.dst_port == 0x20, "QRTR round-trip");
    out[0] = 2;
    CHECK(!tb710fu_wifi_qrtr_decode(out, qrtrBytes, &decoded), "QRTR version 2 refused");

    struct tb710fu_wifi_qmi qmi = { TB710FU_WIFI_QMI_REQUEST, 0x21, 0x0026, 4 };
    uint8_t body[4] = { 1, 2, 3, 4 };
    const uint32_t qmiBytes = tb710fu_wifi_qmi_encode(&qmi, body, out, sizeof out);
    CHECK(qmiBytes == 10 && out[0] == 0 && out[1] == 0x21 && out[2] == 0x26 && out[3] == 0 && out[4] == 4,
          "QMI header encoding");
    struct tb710fu_wifi_qmi qmiDecoded;
    CHECK(tb710fu_wifi_qmi_decode(out, qmiBytes, &qmiDecoded) && qmiDecoded.message_id == 0x26,
          "QMI round-trip");
    CHECK(tb710fu_wifi_qmi_tlv_encode(2, 4, body, out, sizeof out) == 7 && out[0] == 2 && out[1] == 4 &&
          out[2] == 0, "QMI TLV encoding");
    uint32_t offset = 0;
    struct tb710fu_wifi_qmi_tlv tlv;
    CHECK(tb710fu_wifi_qmi_tlv_next(out, 7, &offset, &tlv) && tlv.length == 4 && tlv.value[1] == 2 &&
          offset == 7, "QMI TLV walk");

    const uint64_t connect = tb710fu_wifi_htc_connect_words(TB710FU_WIFI_HTC_SVC_ID_WMI_CONTROL, 0, 0);
    CHECK((uint32_t)connect == 0x01000002u && (uint32_t)(connect >> 32) == 0, "HTC connect word0");
    const uint64_t ready = ((uint64_t)((4u << 16) | 0x100u) << 32) | (8u << 16) | TB710FU_WIFI_HTC_MSG_READY;
    struct tb710fu_wifi_htc_message message;
    CHECK(tb710fu_wifi_htc_ready_words(ready, &message) && message.credits == 8 && message.credit_size == 0x100 &&
          message.max_endpoints == 4, "HTC ready decode");
    const uint64_t response = ((uint64_t)((2048u << 16) | (2u << 8) | 0u) << 32) |
                              ((TB710FU_WIFI_HTC_SVC_ID_WMI_CONTROL) << 16) | TB710FU_WIFI_HTC_MSG_CONNECT_SERVICE_RESP;
    CHECK(tb710fu_wifi_htc_connect_response(response, 2, &message) && message.status == 0 &&
          message.max_message_len == 2048, "HTC connect response decode");
    struct tb710fu_wifi_htc_frame frame = { 2, 0, 4 };
    CHECK(tb710fu_wifi_htc_encode(&frame, body, out, sizeof out) == 12 &&
          out[0] == 2 && out[2] == 4, "HTC frame encoding");
    struct tb710fu_wifi_htc_frame frameDecoded;
    CHECK(tb710fu_wifi_htc_decode(out, 12, &frameDecoded) && frameDecoded.endpoint == 2 &&
          frameDecoded.payload_len == 4, "HTC frame round-trip");

    CHECK(tb710fu_wifi_wmi_encode_cmd(TB710FU_WIFI_WMI_INIT_CMDID, body, 4, out, sizeof out) == 8 &&
          out[0] == 1 && out[4] == 1, "WMI command encoding");
    uint32_t cmdId = 0, bodyOffset = 0;
    CHECK(tb710fu_wifi_wmi_decode_cmd(out, 8, &cmdId, &bodyOffset) && cmdId == 1 && bodyOffset == 4,
          "WMI command decode");
    CHECK(tb710fu_wifi_wmi_tlv(6, 10) == ((6u << 16) | 10u), "WMI TLV word");
    CHECK(tb710fu_wifi_wmi_tlv(6, 0x1ffff) == ((6u << 16) | 0xffffu), "WMI TLV length masks to 16 bits");

    struct tb710fu_wifi_dp_plan dp;
    CHECK(tb710fu_wifi_dp_plan_make(256, 512, 128, &dp) && dp.tx_bytes == 256u * 128u &&
          dp.rx_bytes == 512u * 128u, "DP ring geometry");
    CHECK(!tb710fu_wifi_dp_plan_make(256, 512, 96, &dp), "DP element size must be a power of two");
    CHECK(tb710fu_wifi_dp_next(255, 256) == 0 && tb710fu_wifi_dp_next(3, 256) == 4 && tb710fu_wifi_dp_next(0, 0) == 0,
          "DP ring index wraps");
}

int main(void)
{
    testBoard();
    testManifest();
    testMhiBuilders();
    testMhiBoot();
    testSmmu();
    testProto();
    if (gFailures) {
        printf("FAIL: %u of %u checks failed\n", gFailures, gChecks);
        return 1;
    }
    printf("PASS: %u checks\n", gChecks);
    return 0;
}
