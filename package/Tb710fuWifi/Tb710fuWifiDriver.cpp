/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Tb710fuWifi stage machine (wifi=0..5).
 *
 * 0  off (default): nothing is mapped, no thread is started.
 * 1  board power, PCIe0 root complex and PHY, link training, endpoint ID, BAR0, WCN chip power-up.
 * 2  firmware images (m3.bin through BHI, amss.bin through BHIe), MHI contexts, mission mode.
 * 3  QRTR/QMI scaffolding: the mission-mode check plus the QRTR/QMI encoders this subsystem carries; the MHI pipe
 *    mapping is not wired yet, so this level reports that instead of pretending a QMI exchange happened.
 * 4  HTC/WMI ready: same gate as 3 (HTC needs the WMI pipe assignment the firmware publishes).
 * 5  attaches the dormant Tb710fuWifiNet IOEthernetController (link stays down until 3/4 are real).
 *
 * A failure records the stage it reached and a negative code in native_tb710fu_wifi_stats; the thread never spins.
 * Provenance: stage ordering, boot-arg style and stats/log layout follow package/Tb710fuAdsp/Tb710fuAdsp.cpp; the chip
 * power-up recipe is WifiProbe.cpp's chipPowerUp (ath12k_pci_power_up). */

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <ptrauth.h>
#include <string.h>

#include "Tb710fuWifiBoard.h"
#include "Tb710fuWifiFirmware.h"
#include "Tb710fuWifiMhi.h"
#include "Tb710fuWifiManifest.h"
#include "Tb710fuWifiPcie.h"
#include "Tb710fuWifiRegs.h"

extern "C" void tb710fu_wifi_net_start(void);

/* Same public layout as the other TB710FU drivers, so the merge only adds a weak call. */
extern "C" volatile int32_t native_tb710fu_wifi_stats[16];
extern "C" char native_tb710fu_wifi_text[256];

/* [0] last completed stage, [1] negative error code, [2] LTSSM register, [3] endpoint vendor|device, [4] BAR0 size,
 * [5] MHI phase | failed phase << 8, [6] execution environment, [7] MHI state, [8] BHI status, [9] BHIe status,
 * [10] firmware bytes, [11] apps-SMMU bank (0 = not programmed), [12] milliseconds, [13] port_debug0,
 * [14] port_debug1, [15] net interface attached. */
volatile int32_t native_tb710fu_wifi_stats[16];
char native_tb710fu_wifi_text[256];

namespace {

constexpr uint32_t kStageMax = 5;
constexpr uint32_t kMhiBarOffset = TB710FU_WIFI_CHIP_MHI_BASE;
constexpr uint32_t kMhiWaitMs = 20000;

enum {
    WIFI_ERR_ARG = -1,
    WIFI_ERR_STAGE1 = -0x100,
    WIFI_ERR_FW = -3,
    WIFI_ERR_SMMU = -4,
    WIFI_ERR_DMA = -5,
    WIFI_ERR_MHI = -6,
    WIFI_ERR_MISSION = -7,
    WIFI_ERR_PROTO = -8,
    WIFI_ERR_NET = -9,
};

struct WifiDriver {
    Tb710fuWifiPcie pci;
    Tb710fuWifiMhi mhi;
    uint32_t window;
    uint8_t started;
};

WifiDriver gWifi;

void logText(const char *text)
{
    if (!text) {
        return;
    }
    strlcpy(native_tb710fu_wifi_text, text, sizeof(native_tb710fu_wifi_text));
}

/* ---- chip register window (ath12k_pci_select_window): 0x310c takes 0x40000000 | offset[24:19]; the register then
 * lives at BAR + 0x80000 + offset[18:0], except the MHI block which appears at BAR + 0. ---- */
uint32_t chipRead(volatile uint32_t *bar, uint32_t offset)
{
    if (offset < TB710FU_WIFI_CHIP_WINDOW_BASE) {
        return bar[offset >> 2];
    }
    const uint32_t window = (offset >> 19) & 0x3fu;
    if (window != gWifi.window) {
        bar[TB710FU_WIFI_CHIP_WINDOW_REG >> 2] = 0x40000000u | window;
        (void)bar[TB710FU_WIFI_CHIP_WINDOW_REG >> 2];
        gWifi.window = window;
    }
    if (offset >= TB710FU_WIFI_CHIP_MHI_BASE && offset <= 0x1e0effcu) {
        return bar[((offset - TB710FU_WIFI_CHIP_MHI_BASE) & 0x7ffffu) >> 2];
    }
    return bar[(TB710FU_WIFI_CHIP_WINDOW_BASE + (offset & 0x7ffffu)) >> 2];
}

void chipWrite(volatile uint32_t *bar, uint32_t offset, uint32_t value)
{
    if (offset < TB710FU_WIFI_CHIP_WINDOW_BASE) {
        bar[offset >> 2] = value;
        __asm__ volatile("dsb sy" ::: "memory");
        return;
    }
    const uint32_t window = (offset >> 19) & 0x3fu;
    if (window != gWifi.window) {
        bar[TB710FU_WIFI_CHIP_WINDOW_REG >> 2] = 0x40000000u | window;
        (void)bar[TB710FU_WIFI_CHIP_WINDOW_REG >> 2];
        gWifi.window = window;
    }
    if (offset >= TB710FU_WIFI_CHIP_MHI_BASE && offset <= 0x1e0effcu) {
        bar[((offset - TB710FU_WIFI_CHIP_MHI_BASE) & 0x7ffffu) >> 2] = value;
    } else {
        bar[(TB710FU_WIFI_CHIP_WINDOW_BASE + (offset & 0x7ffffu)) >> 2] = value;
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ath12k_pci_sw_reset(power on): LTSSM reset word, hot reset, stale interrupts, warm-boot marker, reset cause. */
void chipPowerUp(volatile uint32_t *bar)
{
    gWifi.window = 0xffffffffu;
    uint32_t v = chipRead(bar, TB710FU_WIFI_CHIP_LTSSM_RESET);
    for (unsigned i = 0; v != TB710FU_WIFI_CHIP_LTSSM_VALUE && i < 5; ++i) {
        if (v == 0xffffffffu) {
            IOSleep(5);
        }
        chipWrite(bar, TB710FU_WIFI_CHIP_LTSSM_RESET, TB710FU_WIFI_CHIP_LTSSM_VALUE);
        v = chipRead(bar, TB710FU_WIFI_CHIP_LTSSM_RESET);
    }
    native_tb710fu_wifi_stats[2] = (int32_t)v;
    v = chipRead(bar, TB710FU_WIFI_CHIP_HOT_RESET);
    chipWrite(bar, TB710FU_WIFI_CHIP_HOT_RESET, v | 0x10u);
    IOSleep(5);
    chipWrite(bar, TB710FU_WIFI_CHIP_IRQ_CLR, 0xffffffffu);
    v = chipRead(bar, TB710FU_WIFI_CHIP_WARM_CTRL);
    chipWrite(bar, TB710FU_WIFI_CHIP_WARM_CTRL, v & ~4u);
    chipWrite(bar, 0x360u, 0);
    chipWrite(bar, 0x368u, 0);
    chipWrite(bar, 0x394u, 0);
    chipWrite(bar, 0x39cu, 0); /* BHIe doorbells and status left from a previous boot */
    IOSleep(10);
    chipWrite(bar, TB710FU_WIFI_CHIP_WARM_ENTRY, 0); /* keep the Q6 off the warm-boot path */
    IOSleep(10);
    (void)chipRead(bar, TB710FU_WIFI_CHIP_WARM_ENTRY);
    (void)chipRead(bar, TB710FU_WIFI_CHIP_RESET_CAUSE);
    v = chipRead(bar, TB710FU_WIFI_CHIP_GLOBAL_RESET);
    chipWrite(bar, TB710FU_WIFI_CHIP_GLOBAL_RESET, v | 1u);
    IOSleep(2);
    chipWrite(bar, TB710FU_WIFI_CHIP_GLOBAL_RESET, chipRead(bar, TB710FU_WIFI_CHIP_GLOBAL_RESET) & ~1u);
    IOSleep(2);
    logText("chip power-up complete");
}

/* ---- DMA ---- */
struct DmaBlock {
    IOBufferMemoryDescriptor *buffer;
    uint8_t *va;
    uint64_t pa;
    uint32_t bytes;
};

void cleanToPoc(const void *start, size_t length)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = (uintptr_t)4 << ((ctr >> 16) & 15);
    for (uintptr_t a = (uintptr_t)start & ~(line - 1); a < (uintptr_t)start + length; a += line) {
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

bool dmaAlloc(uint32_t bytes, DmaBlock *block)
{
    if (!block) {
        return false;
    }
    *block = DmaBlock{};
    block->buffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous, bytes,
        0xfffffffffffff000ull);
    if (!block->buffer || block->buffer->prepare() != kIOReturnSuccess) {
        if (block->buffer) {
            block->buffer->release();
            block->buffer = nullptr;
        }
        return false;
    }
    block->va = static_cast<uint8_t *>(block->buffer->getBytesNoCopy());
    block->pa = block->buffer->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    block->bytes = bytes;
    return block->va && block->pa;
}

void dmaFree(DmaBlock *block)
{
    if (!block || !block->buffer) {
        return;
    }
    block->buffer->complete();
    block->buffer->release();
    *block = DmaBlock{};
}

/* ---- MHI register access through the BAR window ---- */
uint32_t mhiBarRead(void *ctx, uint32_t reg)
{
    return chipRead((volatile uint32_t *)ctx, kMhiBarOffset + reg);
}

void mhiBarWrite(void *ctx, uint32_t reg, uint32_t value)
{
    chipWrite((volatile uint32_t *)ctx, kMhiBarOffset + reg, value);
}

/* ---- apps-SMMU preflight ---- */
uint32_t smmuRead(void *ctx, uint32_t offset)
{
    return ((volatile uint32_t *)ctx)[offset >> 2];
}

void smmuWrite(void *ctx, uint32_t offset, uint32_t value)
{
    ((volatile uint32_t *)ctx)[offset >> 2] = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

void smmuBarrier(void *)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

IOMemoryMap *mapPhys(uint64_t address, uint32_t bytes)
{
    IOMemoryDescriptor *desc = IOMemoryDescriptor::withPhysicalAddress(address, bytes, kIODirectionInOut);
    if (!desc) {
        return nullptr;
    }
    IOMemoryMap *map = desc->map(kIOMapAnywhere);
    desc->release();
    return map;
}

/* Returns 0 when the endpoint's DMA may use physical addresses; -1 when the SMMU is on and no bank was accepted. */
int smmuPreflight(uint32_t *bankOut)
{
    *bankOut = 0;
    IOMemoryMap *map = mapPhys(TB710FU_WIFI_SMMU_BASE, TB710FU_WIFI_SMMU_BYTES);
    if (!map) {
        return -1;
    }
    volatile uint32_t *base = (volatile uint32_t *)map->getVirtualAddress();
    const uint32_t scr0 = base[0];
    if (!(scr0 & 1u)) { /* translation off: the aperture passes through */
        map->release();
        return 0;
    }
    uint32_t bank = 0;
    if (!PE_parse_boot_argn("wifi-smmu-bank", &bank, sizeof bank) || !bank) {
        map->release();
        logText("apps-SMMU translation is on and wifi-smmu-bank is unset");
        return -1;
    }
    struct tb710fu_wifi_smmu_io io = { (void *)base, smmuRead, smmuWrite, smmuBarrier };
    struct tb710fu_wifi_smmu_plan plan = {};
    const enum tb710fu_wifi_smmu_result result = tb710fu_wifi_smmu_plan(&io, TB710FU_WIFI_ENDPOINT_SID, bank, &plan);
    if (result != TB710FU_WIFI_SMMU_OK) {
        logText(plan.error ? plan.error : "apps-SMMU plan rejected");
        map->release();
        return -1;
    }
    if (!tb710fu_wifi_smmu_apply(&io, TB710FU_WIFI_ENDPOINT_SID, &plan)) {
        map->release();
        return -1;
    }
    *bankOut = bank;
    map->release();
    return 0;
}

/* ---- stage 1 ---- */
int stage1(void)
{
    const uint64_t start = mach_absolute_time();
    int rc = tb710fu_wifi_pcie_power(&gWifi.pci);
    if (rc) {
        return WIFI_ERR_STAGE1 + rc;
    }
    native_tb710fu_wifi_stats[2] = (int32_t)gWifi.pci.ltssm;
    native_tb710fu_wifi_stats[13] = (int32_t)gWifi.pci.port_debug0;
    native_tb710fu_wifi_stats[14] = (int32_t)gWifi.pci.port_debug1;
    rc = tb710fu_wifi_pcie_config_window(&gWifi.pci);
    if (rc) {
        return WIFI_ERR_STAGE1 + rc;
    }
    native_tb710fu_wifi_stats[3] = (int32_t)gWifi.pci.endpoint_id;
    rc = tb710fu_wifi_pcie_claim_bar(&gWifi.pci);
    if (rc) {
        return WIFI_ERR_STAGE1 + rc;
    }
    native_tb710fu_wifi_stats[4] = (int32_t)gWifi.pci.bar0_size;
    rc = tb710fu_wifi_pcie_map_bar(&gWifi.pci);
    if (rc) {
        return WIFI_ERR_STAGE1 + rc;
    }
    if (!gWifi.pci.bar) {
        return WIFI_ERR_STAGE1;
    }
    chipPowerUp(gWifi.pci.bar);
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    native_tb710fu_wifi_stats[12] = (int32_t)(ns / 1000000ull);
    logText("stage 1: PCIe link up, endpoint 17cb:1107, BAR0 mapped, chip powered");
    return 0;
}

/* ---- stage 2 ---- */
int stage2(void)
{
    uint32_t smmuBank = 0;
    if (smmuPreflight(&smmuBank)) {
        return WIFI_ERR_SMMU;
    }
    native_tb710fu_wifi_stats[11] = (int32_t)smmuBank;

    struct tb710fu_wifi_fw amss, m3;
    int error = tb710fu_wifi_fw_load(nullptr, TB710FU_WIFI_FW_AMSS_NAME, TB710FU_WIFI_FW_AMSS_NAME,
                                     TB710FU_WIFI_FW_AMSS_BYTES / 2, TB710FU_WIFI_FW_AMSS_MAX, &amss);
    if (error) {
        logText("amss.bin not found or out of range");
        return WIFI_ERR_FW;
    }
    error = tb710fu_wifi_fw_load(nullptr, TB710FU_WIFI_FW_M3_NAME, TB710FU_WIFI_FW_M3_NAME,
                                 TB710FU_WIFI_FW_M3_BYTES / 2, TB710FU_WIFI_FW_M3_MAX, &m3);
    if (error) {
        tb710fu_wifi_fw_release(&amss);
        logText("m3.bin not found or out of range");
        return WIFI_ERR_FW;
    }
    tb710fu_wifi_fw_info amssInfo, m3Info;
    if (tb710fu_wifi_fw_check(TB710FU_WIFI_FW_AMSS_NAME, amss.bytes, amss.size, &amssInfo) != TB710FU_WIFI_FW_OK ||
        tb710fu_wifi_fw_check(TB710FU_WIFI_FW_M3_NAME, m3.bytes, m3.size, &m3Info) != TB710FU_WIFI_FW_OK) {
        tb710fu_wifi_fw_release(&amss);
        tb710fu_wifi_fw_release(&m3);
        logText("firmware header does not match the WCN7850 manifest");
        return WIFI_ERR_FW;
    }
    /* board-2.bin is not booted through MHI; it is the container QMI hands to the firmware later. Load and validate it
     * here so a wrong board file is refused before any protocol stage could use it. */
    struct tb710fu_wifi_fw board;
    error = tb710fu_wifi_fw_load(nullptr, TB710FU_WIFI_FW_BOARD_NAME, TB710FU_WIFI_FW_BOARD_NAME,
                                 64u << 10, TB710FU_WIFI_FW_BOARD_MAX, &board);
    tb710fu_wifi_fw_info boardInfo;
    if (error || tb710fu_wifi_fw_check(TB710FU_WIFI_FW_BOARD_NAME, board.bytes, board.size, &boardInfo) !=
                     TB710FU_WIFI_FW_OK) {
        tb710fu_wifi_fw_release(&amss);
        tb710fu_wifi_fw_release(&m3);
        tb710fu_wifi_fw_release(&board);
        logText("board-2.bin not found or not an ath12k board container");
        return WIFI_ERR_FW;
    }
    tb710fu_wifi_fw_release(&board);
    native_tb710fu_wifi_stats[10] = (int32_t)amss.size;

    const uint32_t segments = (uint32_t)((amss.size + TB710FU_WIFI_FW_SEGMENT - 1) / TB710FU_WIFI_FW_SEGMENT);
    if (!segments || segments > TB710FU_WIFI_FW_MAX_SEGMENTS) {
        tb710fu_wifi_fw_release(&amss);
        tb710fu_wifi_fw_release(&m3);
        return WIFI_ERR_FW;
    }
    Tb710fuWifiMhiPlan plan = {};
    if (!tb710fu_wifi_mhi_plan_make(0, &plan)) {
        tb710fu_wifi_fw_release(&amss);
        tb710fu_wifi_fw_release(&m3);
        return WIFI_ERR_MHI;
    }
    DmaBlock mhiDma = {}, sblDma = {}, imageDma = {}, tableDma = {};
    bool ok = dmaAlloc(plan.total_bytes, &mhiDma) && dmaAlloc((uint32_t)m3.size, &sblDma) &&
              dmaAlloc((uint32_t)amss.size, &imageDma) && dmaAlloc(segments * (uint32_t)sizeof(Tb710fuWifiBhieEntry), &tableDma);
    if (!ok) {
        dmaFree(&mhiDma);
        dmaFree(&sblDma);
        dmaFree(&imageDma);
        dmaFree(&tableDma);
        tb710fu_wifi_fw_release(&amss);
        tb710fu_wifi_fw_release(&m3);
        logText("firmware DMA allocation failed");
        return WIFI_ERR_DMA;
    }

    /* Device addresses: with the SMMU off (or an identity bank) these are the physical addresses. */
    const uint64_t iova = mhiDma.pa;
    memcpy(sblDma.va, m3.bytes, (size_t)m3.size);
    memcpy(imageDma.va, amss.bytes, (size_t)amss.size);
    cleanToPoc(sblDma.va, (size_t)m3.size);
    cleanToPoc(imageDma.va, (size_t)amss.size);

    if (!tb710fu_wifi_mhi_plan_make(iova, &plan) ||
        !tb710fu_wifi_mhi_contexts(mhiDma.va, plan.total_bytes, iova, 0, &plan)) {
        dmaFree(&mhiDma);
        dmaFree(&sblDma);
        dmaFree(&imageDma);
        dmaFree(&tableDma);
        tb710fu_wifi_fw_release(&amss);
        tb710fu_wifi_fw_release(&m3);
        return WIFI_ERR_MHI;
    }
    cleanToPoc(mhiDma.va, plan.total_bytes);

    /* MHI context records are 44 bytes inside a host buffer; the device-visible addresses are the DMA ones. */
    uint64_t segmentAddresses[TB710FU_WIFI_FW_MAX_SEGMENTS];
    for (uint32_t i = 0; i < segments; ++i) {
        segmentAddresses[i] = imageDma.pa + (uint64_t)i * TB710FU_WIFI_FW_SEGMENT;
    }
    uint32_t entries = tb710fu_wifi_bhie_table((Tb710fuWifiBhieEntry *)tableDma.va, segments, segmentAddresses,
                                               (uint32_t)amss.size, TB710FU_WIFI_FW_SEGMENT);
    if (!entries) {
        dmaFree(&mhiDma);
        dmaFree(&sblDma);
        dmaFree(&imageDma);
        dmaFree(&tableDma);
        tb710fu_wifi_fw_release(&amss);
        tb710fu_wifi_fw_release(&m3);
        return WIFI_ERR_MHI;
    }
    cleanToPoc(tableDma.va, entries * (uint32_t)sizeof(Tb710fuWifiBhieEntry));

    Tb710fuWifiMhiIo io = { (void *)gWifi.pci.bar, mhiBarRead, mhiBarWrite };
    if (tb710fu_wifi_mhi_open(&gWifi.mhi, &io)) {
        native_tb710fu_wifi_stats[5] = (int32_t)(gWifi.mhi.phase | gWifi.mhi.error << 8);
        dmaFree(&mhiDma);
        dmaFree(&sblDma);
        dmaFree(&imageDma);
        dmaFree(&tableDma);
        tb710fu_wifi_fw_release(&amss);
        tb710fu_wifi_fw_release(&m3);
        logText("MHI register layout unusable");
        return WIFI_ERR_MHI;
    }
    uint32_t phase = tb710fu_wifi_mhi_begin(&gWifi.mhi, 0, sblDma.pa, (uint32_t)m3.size,
                                            tableDma.pa, entries * (uint32_t)sizeof(Tb710fuWifiBhieEntry), 1);
    const uint64_t start = mach_absolute_time();
    for (unsigned elapsed = 0; elapsed < kMhiWaitMs; ++elapsed) {
        phase = tb710fu_wifi_mhi_step(&gWifi.mhi);
        if (phase == TB710FU_WIFI_MHI_NEED_CONTEXTS) {
            Tb710fuWifiMhiMmio mmio = {};
            if (!tb710fu_wifi_mhi_mmio(&plan, iova, &mmio)) {
                phase = tb710fu_wifi_mhi_timeout(&gWifi.mhi);
                break;
            }
            const uint32_t cfg = mhiBarRead((void *)gWifi.pci.bar, TB710FU_WIFI_MHI_CFG);
            mhiBarWrite((void *)gWifi.pci.bar, TB710FU_WIFI_MHI_CFG,
                        (cfg & ~mmio.cfg_mask) | mmio.cfg_value);
            for (unsigned i = 0; i < sizeof(mmio.words) / sizeof(mmio.words[0]); ++i) {
                mhiBarWrite((void *)gWifi.pci.bar, mmio.words[i][0], mmio.words[i][1]);
            }
            phase = tb710fu_wifi_mhi_contexts_done(&gWifi.mhi);
        }
        if (phase == TB710FU_WIFI_MHI_DONE || phase == TB710FU_WIFI_MHI_FAILED) {
            break;
        }
        IOSleep(1);
    }
    if (phase != TB710FU_WIFI_MHI_DONE) {
        tb710fu_wifi_mhi_timeout(&gWifi.mhi);
    }
    native_tb710fu_wifi_stats[5] = (int32_t)(gWifi.mhi.phase | gWifi.mhi.failed_phase << 8);
    native_tb710fu_wifi_stats[6] = (int32_t)gWifi.mhi.ee;
    native_tb710fu_wifi_stats[7] = (int32_t)gWifi.mhi.state;
    native_tb710fu_wifi_stats[8] = (int32_t)gWifi.mhi.last_status;
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    native_tb710fu_wifi_stats[12] = (int32_t)(ns / 1000000ull);
    tb710fu_wifi_fw_release(&amss);
    tb710fu_wifi_fw_release(&m3);
    if (phase != TB710FU_WIFI_MHI_DONE ||
        (gWifi.mhi.ee != TB710FU_WIFI_EE_AMSS && gWifi.mhi.ee != TB710FU_WIFI_EE_WFW)) {
        dmaFree(&mhiDma);
        dmaFree(&sblDma);
        dmaFree(&imageDma);
        dmaFree(&tableDma);
        logText("MHI mission mode not reached");
        return WIFI_ERR_MISSION;
    }
    /* Mission mode means the device owns the MHI rings; freeing those DMA pages while it could still post an event
     * would leave the MHI channel pointing at released memory, so the buffers stay allocated for this boot. Only the
     * host-side firmware copies (already copied into DMA) are released above. */
    logText("stage 2: m3.bin via BHI and amss.bin via BHIe loaded, MHI mission mode");
    return 0;
}

/* ---- stage 3/4: the transport between this host and the firmware is not wired in this milestone. The QRTR/QMI
 * and HTC/WMI builders live in Tb710fuWifiProto.c and are covered by the host tests, so the encoders cannot silently
 * drift; the driver reports the missing pipe assignment rather than faking a ready WMI. ---- */
int stageProto(uint32_t needStage)
{
    (void)needStage;
    static const char *text = "QRTR/QMI transport over MHI pipes not implemented in this milestone";
    logText(text);
    return WIFI_ERR_PROTO;
}

int stageNet(void)
{
    tb710fu_wifi_net_start();
    return 0;
}

void wifiWorker(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "tb710fu-wifi");
    uint32_t mode = 0;
    if (!PE_parse_boot_argn("wifi", &mode, sizeof mode) || mode == 0) {
        thread_terminate(current_thread());
        return;
    }
    gWifi.started = 1;
    int error = stage1();
    if (error) {
        native_tb710fu_wifi_stats[1] = (int32_t)error;
        printf("TB710FU Wifi: stage 1 failed (%d)\n", error);
        thread_terminate(current_thread());
        return;
    }
    native_tb710fu_wifi_stats[0] = 1;
    if (mode >= 2) {
        error = stage2();
        if (error) {
            native_tb710fu_wifi_stats[1] = (int32_t)error;
            native_tb710fu_wifi_stats[0] = 2;
            printf("TB710FU Wifi: stage 2 failed (%d)\n", error);
            thread_terminate(current_thread());
            return;
        }
        native_tb710fu_wifi_stats[0] = 2;
    }
    if (mode >= 3) {
        error = stageProto(3);
        native_tb710fu_wifi_stats[1] = (int32_t)error;
        printf("TB710FU Wifi: stage 3 is scaffolding only (%d), no QMI exchange\n", error);
        if (mode < 5) {
            thread_terminate(current_thread());
            return;
        }
    }
    if (mode >= 4) {
        error = stageProto(4);
        native_tb710fu_wifi_stats[1] = (int32_t)error;
        printf("TB710FU Wifi: stage 4 is scaffolding only (%d)\n", error);
        if (mode < 5) {
            thread_terminate(current_thread());
            return;
        }
    }
    if (mode >= 5) {
        error = stageNet();
        if (error) {
            native_tb710fu_wifi_stats[1] = (int32_t)error;
        }
    }
    thread_terminate(current_thread());
}

} /* namespace */

extern "C" void tb710fu_wifi_probe(void)
{
    uint32_t mode = 0;
    if (!PE_parse_boot_argn("wifi", &mode, sizeof mode) || mode == 0) {
        return; /* default: entirely absent, as every staged driver in this package */
    }
    if (mode > kStageMax) {
        mode = kStageMax;
    }
    if (native_tb710fu_wifi_stats[0] != 0 || gWifi.started) {
        return;
    }
    thread_t thread = nullptr;
    if (kernel_thread_start(wifiWorker, nullptr, &thread) != KERN_SUCCESS) {
        native_tb710fu_wifi_stats[1] = (int32_t)WIFI_ERR_ARG;
        printf("TB710FU Wifi: worker spawn failed\n");
        return;
    }
    thread_deallocate(thread);
    printf("TB710FU Wifi: worker started, wifi=%u\n", mode);
}
