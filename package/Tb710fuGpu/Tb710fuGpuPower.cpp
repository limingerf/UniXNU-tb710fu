// SPDX-License-Identifier: BSD-2-Clause-Patent
// gpu-power=<n>: the staged SM8650 (Adreno 750) bring-up.
//
//   1  census: the ADT's kgsl-3d0/kgsl-smmu/gen7-gmu nodes and the boot addon's
//      GPU entries, printed and counted. No mapping, no write.
//   2  map the GCC GPU page and the GCC vote page, read the clock/power state,
//      and read GPUCC only if gcc_gpu_cfg_ahb_clk already runs. No write.
//   3  CX: gpu_cc_cx_gdsc on (gdsc.c sequence), then gpu_cc_ahb_clk and
//      gpu_cc_hlos1_vote_gpu_smmu_clk on. Writes start here.
//   4  the GPU SMMU global space inventory (read-only, GR0 only): ID registers,
//      sGFSR and every valid SMR. Programs nothing.
//   5  the GMU clock tree a6xx_gmu_resume needs: GPLL0 votes, PLL1 configure and
//      enable, HUB and GMU RCGs, then the GMU branches.
//   6  the four gen70900 images: size and SHA-256 against the pins.
//   7  GMU cold start: refused in this milestone. It needs the GMU's own SMMU
//      context (bank 1, ArmSmmu4K) and the SM8650 RSC/PDC setup words, neither
//      of which is pinned yet; the line states that instead of guessing.
//   8  GX power: refused. It needs the HFI startup votes built from the AOP
//      command DB tables, which this board has not derived.
//   9  the user client ABI is published (Tb710fuGpuClient). ring=0 until 7+8.
//
// Nothing runs without gpu-power/gpu-metal; the default boot reads both boot
// arguments once and returns from tb710fu_gpu_wanted() without printing.
//
// Read/write discipline copied from the reference board (UniXNU-sheng
// GpuPowerBoard.h, BSD-2-Clause-Patent) with the SM8650 offsets: GPUCC needs
// gcc_gpu_cfg_ahb_clk on before its first read, the SMMU needs CX and its HLOS
// vote clock, the GPU window needs GX (never powered here), and the SMMU scan
// stays in global space 0 (the apps-SMMU abort on a GR1 read is recorded there).
#include "Tb710fuGpu.h"
#include "Tb710fuGpuFirmware.h"
#include "Tb710fuGpuHw.h"
#include "Tb710fuGpuMmio.h"
#include "Tb710fuAddon.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/libkern.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" {
/* The loader driver and the kernel's gpu-addon mount counters (weak: a kernel
 * built without that driver has no addon). */
int native_ram_block_gpu_addon(const uint8_t **data, uint64_t *bytes) __attribute__((weak_import));
volatile int native_gpu_addon_stage __attribute__((weak_import));
volatile int native_gpu_addon_status __attribute__((weak_import));
/* Called only from tb710fu_gpu_power_meta_init, never from __mod_init_func. */
void tb710fu_gpu_power_static_init(void) __asm__("__GLOBAL__sub_I_Tb710fuGpuPower.cpp");
}

namespace {
const char *const kBundleExe =
    "GPUBundles/NativeAdrenoMetal.bundle/Contents/MacOS/NativeAdrenoMetal";
const char *const kPublish = "GPUBundles/NativeAdrenoMetal.publish";

/* scripts/data/tb710fu-gpu-firmware.json (reference pins, gen70900). */
struct FirmwarePin {
    const char *name;
    uint64_t bytes;
    const char *sha256;
};
const FirmwarePin kFirmware[] = {
    {"gmu_gen70900.bin", 78588, "9685f62c42543befcaee68d25355e16bf55cdb0d573bddcc9f6633f59eec3f72"},
    {"gen70900_sqe.fw", 85300, "8e2c5ce74140de7e161f14654b44de999855bce01b690fdd18c3358a73498cd1"},
    {"gen70900_aqe.fw", 33176, "25237c161d727b8ea92b4beb837a31651b44ea78bfc34d95672af8f346a45825"},
    {"gen70900_zap.mbn", 12088, "890f7e9da007890a40804d3110169ccc887e8a03bdf29e8b7bc3df0016bf9860"},
};
const unsigned kFirmwareCount = sizeof(kFirmware) / sizeof(kFirmware[0]);

/* The board's windows. GCC pages are separate mappings so the vote register's
 * page never aliases the GPU branch page (same 0x71000/0x52000 split the UFS
 * and ADSP probes use). */
Tb710fuGpuWindow gGcc;
Tb710fuGpuWindow gVote;
Tb710fuGpuWindow gGpucc;
Tb710fuGpuWindow gSmmu;
Tb710fuGpuWindow gGpu;
uint32_t gWrites = 0;

/* Facts the accelerator publishes and the host checks pin. */
uint32_t gSmmuSmrValid = 0;
uint32_t gSmmuId2 = 0;
uint32_t gCxOn = 0;
uint32_t gMmuClocksStep = 0;
uint32_t gMmuClocksFailed = 0;
uint32_t gFirmwareVerified = 0;
uint32_t gFirmwareBytes = 0;
uint32_t gFirmwareAddon = 0;

int rd(const Tb710fuGpuWindow &window, uint32_t offset, uint32_t *value)
{
    return tb710fu_gpu_read32(&window, offset, value);
}

int wr(const Tb710fuGpuWindow &window, uint32_t offset, uint32_t value)
{
    if (tb710fu_gpu_write32(&window, offset, value) != 0) {
        return -1;
    }
    gWrites = gWrites + 1;
    return 0;
}

/* A bounded copy of a DT string property; the ADT's value includes its NUL. */
const char *dtString(DTEntry entry, const char *name, char *buffer, unsigned size)
{
    const void *value = nullptr;
    unsigned length = 0;
    if (size == 0) {
        return "-";
    }
    buffer[0] = '-';
    buffer[1] = 0;
    if (!entry || SecureDTGetProperty(entry, name, &value, &length) != kSuccess ||
        !value || length == 0) {
        return buffer;
    }
    if (length > size) {
        length = size;
    }
    memcpy(buffer, value, length - 1);
    buffer[length - 1] = 0;
    return buffer;
}

/* "dir" + "name" in one bounded buffer, without libkern printf helpers: every
 * path this file builds is ASCII and shorter than the addon's 160-byte limit. */
bool joinPath(char *destination, unsigned size, const char *directory, const char *name)
{
    if (!destination || size == 0 || !directory || !name) {
        return false;
    }
    unsigned at = 0;
    while (*directory && at + 1 < size) {
        destination[at++] = *directory++;
    }
    while (*name && at + 1 < size) {
        destination[at++] = *name++;
    }
    destination[at] = 0;
    return *directory == 0 && *name == 0;
}

bool dtHasNode(const char *path)
{
    DTEntry entry = nullptr;
    return SecureDTLookupEntry(nullptr, path, &entry) == kSuccess && entry;
}

/* The live ADT names the node qcom,kgsl-3d0@3d00000; the unit-address-less
 * spelling is accepted because older dumps of this board dropped it. */
DTEntry findGpuNode(void)
{
    static const char *const paths[] = {
        "/soc/qcom,kgsl-3d0@3d00000", "/soc/qcom,kgsl-3d0",
    };
    DTEntry entry = nullptr;
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        if (SecureDTLookupEntry(nullptr, paths[i], &entry) == kSuccess && entry) {
            return entry;
        }
    }
    return nullptr;
}

/* gpucc-sm8650.c / clk-alpha-pll.c lucid_ole: write the config words that are
 * nonzero, then put the PLL in standby with RESET_N. */
int pllConfigure(const Tb710fuGpuWindow &window, uint32_t base, uint32_t l, uint32_t alpha)
{
    static const uint32_t offsets[] = {0x10, 0x14, 0x20, 0x24, 0x28, 0x18, 0x1c, 0x2c, 0x30, 0x34, 0x38};
    const uint32_t values[] = {l | (0x44u << 16) | (0x44u << 24), alpha, 0x20485699u, 0x00182261u,
                               0x82aa299cu, 0x00000000u, 0x00000005u, 0x00000000u, 0x00000003u,
                               0x00009000u, 0x00000034u};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        if (values[i] && wr(window, base + offsets[i], values[i]) != 0) {
            return -1;
        }
    }
    uint32_t mode = 0;
    if (rd(window, base + TBGPU_PLL_MODE, &mode) != 0) {
        return -1;
    }
    if (wr(window, base + TBGPU_PLL_MODE, mode & ~TBGPU_PLL_OUT_CTRL) != 0) {
        return -1;
    }
    if (wr(window, base + TBGPU_PLL_OPMODE, 0) != 0) {
        return -1;
    }
    if (rd(window, base + TBGPU_PLL_MODE, &mode) != 0) {
        return -1;
    }
    return wr(window, base + TBGPU_PLL_MODE, mode | TBGPU_PLL_RESET_N);
}

bool pllRunning(uint32_t mode, uint32_t opmode)
{
    return (opmode & TBGPU_PLL_RUN) != 0 && (mode & TBGPU_PLL_OUT_CTRL) != 0;
}

/* clk_rcg2_shared_force_enable_clear, verified against clk-rcg2.c: root enable,
 * wait ROOT_OFF clear, write CFG (sel/div field), CMD_UPDATE until it clears,
 * root off. The divider field is the half-divider encoding. */
int rcgSet(const Tb710fuGpuWindow &window, uint32_t rcgr, uint32_t sel, uint32_t divisor, uint32_t *cfgAfter)
{
    uint32_t cmd = 0;
    uint32_t cfg = 0;
    if (rd(window, rcgr, &cmd) != 0 || wr(window, rcgr, cmd | TBGPU_RCG_ROOT_EN) != 0) {
        return -1;
    }
    bool rootOff = false;
    for (unsigned i = 0; i < 500; ++i) {
        if (rd(window, rcgr, &cmd) != 0) {
            return -1;
        }
        if (!(cmd & TBGPU_RCG_ROOT_OFF)) {
            rootOff = true;
            break;
        }
        tb710fu_gpu_delay_us(1);
    }
    if (!rootOff) {
        return 1;
    }
    if (rd(window, rcgr + 4, &cfg) != 0) {
        return -1;
    }
    const uint32_t wanted = (cfg & ~TBGPU_RCG_CFG_CLEAR) | (sel << 8) | TBGPU_RCG_FIELD(divisor);
    if (wr(window, rcgr + 4, wanted) != 0) {
        return -1;
    }
    if (rd(window, rcgr, &cmd) != 0 || wr(window, rcgr, cmd | TBGPU_RCG_UPDATE) != 0) {
        return -1;
    }
    bool updated = false;
    for (unsigned i = 0; i < 500; ++i) {
        if (rd(window, rcgr, &cmd) != 0) {
            return -1;
        }
        if (!(cmd & TBGPU_RCG_UPDATE)) {
            updated = true;
            break;
        }
        tb710fu_gpu_delay_us(1);
    }
    if (!updated) {
        return 1;
    }
    if (wr(window, rcgr, cmd & ~TBGPU_RCG_ROOT_EN) != 0) {
        return -1;
    }
    if (cfgAfter && rd(window, rcgr + 4, cfgAfter) != 0) {
        return -1;
    }
    return 0;
}

int branchEnable(const Tb710fuGpuWindow &window, uint32_t offset)
{
    uint32_t value = 0;
    if (rd(window, offset, &value) != 0) {
        return -1;
    }
    if (value & TBGPU_CBCR_ENABLE) {
        return 0;
    }
    return wr(window, offset, value | TBGPU_CBCR_ENABLE);
}

/* gdsc.c gdsc_enable for gpu_cc_cx_gdsc, then the two clock enables the
 * adreno-smmu node lists (ahb + the HLOS1 vote). GX is never touched: the GMU
 * owns it, and this milestone cannot boot the GMU. */
int enableCx(uint32_t *gdscrOut, uint32_t *hwctrlOut)
{
    uint32_t gdscr = 0;
    uint32_t hwctrl = 0;
    if (rd(gGpucc, TBGPU_CC_CX_GDSCR, &gdscr) != 0) {
        return -1;
    }
    if (!tb710fu_gpu_gdsc_on(gdscr)) {
        if (wr(gGpucc, TBGPU_CC_CX_GDSCR, gdscr & ~TBGPU_GDSC_SW_COLLAPSE) != 0) {
            return -1;
        }
        tb710fu_gpu_delay_us(1);
        bool on = false;
        for (uint32_t waited = 0; waited < TBGPU_GDSC_POLL_US; waited += 10) {
            if (rd(gGpucc, TBGPU_CC_CX_GDS_HW_CTRL, &hwctrl) != 0) {
                return -1;
            }
            if (tb710fu_gpu_gdsc_on(hwctrl)) {
                on = true;
                break;
            }
            tb710fu_gpu_delay_us(10);
        }
        if (!on) {
            *gdscrOut = gdscr;
            *hwctrlOut = hwctrl;
            return -2;
        }
        if (rd(gGpucc, TBGPU_CC_CX_GDSCR, &gdscr) != 0) {
            return -1;
        }
        if (wr(gGpucc, TBGPU_CC_CX_GDSCR, gdscr | TBGPU_GDSC_RETAIN_MEM | TBGPU_GDSC_RETAIN_PERIPH) != 0) {
            return -1;
        }
        tb710fu_gpu_delay_us(1);
        if (wr(gGpucc, TBGPU_CC_CX_GDSCR,
               gdscr | TBGPU_GDSC_RETAIN_MEM | TBGPU_GDSC_RETAIN_PERIPH | TBGPU_GDSC_RETAIN_FF) != 0) {
            return -1;
        }
        tb710fu_gpu_delay_us(1);
    }
    if (rd(gGpucc, TBGPU_CC_CX_GDSCR, &gdscr) != 0 ||
        rd(gGpucc, TBGPU_CC_CX_GDS_HW_CTRL, &hwctrl) != 0) {
        return -1;
    }
    if (!tb710fu_gpu_gdsc_on(gdscr) || !tb710fu_gpu_gdsc_on(hwctrl)) {
        *gdscrOut = gdscr;
        *hwctrlOut = hwctrl;
        return -3;
    }
    uint32_t ahb = 0;
    uint32_t vote = 0;
    if (rd(gGpucc, TBGPU_CC_AHB_CLK, &ahb) != 0 || rd(gGpucc, TBGPU_CC_SMMU_VOTE_CLK, &vote) != 0) {
        return -1;
    }
    if (branchEnable(gGpucc, TBGPU_CC_AHB_CLK) != 0 ||
        branchEnable(gGpucc, TBGPU_CC_SMMU_VOTE_CLK) != 0) {
        return -1;
    }
    bool running = false;
    for (unsigned waited = 0; waited < 500; ++waited) {
        if (rd(gGpucc, TBGPU_CC_SMMU_VOTE_CLK, &vote) != 0) {
            return -1;
        }
        if (tb710fu_gpu_clock_on(vote)) {
            running = true;
            break;
        }
        tb710fu_gpu_delay_us(10);
    }
    if (rd(gGpucc, TBGPU_CC_AHB_CLK, &ahb) != 0) {
        return -1;
    }
    *gdscrOut = gdscr;
    *hwctrlOut = hwctrl;
    return running ? 0 : -4;
}

}  // namespace

class Tb710fuGpuPower : public IOService {
    OSDeclareDefaultStructors(Tb710fuGpuPower)
public:
    /* A census carrier: the properties are set by tb710fu_gpu_power_publish and
     * a later GMU/SMMU stage attaches under this node. */
};
OSDefineMetaClassAndStructors(Tb710fuGpuPower, IOService)

extern "C" int tb710fu_gpu_wanted(void)
{
    uint32_t power = 0;
    uint32_t metal = 0;
    const bool powerAsked = PE_parse_boot_argn("gpu-power", &power, sizeof(power));
    const bool metalAsked = PE_parse_boot_argn("gpu-metal", &metal, sizeof(metal));
    tb710fu_gpu_stats[TB710FU_GPU_POWER_LEVEL] = power;
    return (powerAsked && power >= 1) || (metalAsked && metal >= 1);
}

extern "C" void tb710fu_gpu_power_survey(void)
{
    if (!tb710fu_gpu_wanted()) {
        return;
    }
    tb710fu_gpu_stats[TB710FU_GPU_POWER_STAGE] = 1;

    const char *model = "-";
    const char *compatible = "-";
    char modelBuffer[48];
    char compatibleBuffer[64];
    uint32_t chipid = 0;
    DTEntry gpu = findGpuNode();
    if (!gpu) {
        printf("TB710FU GPU: ADT has no /soc/qcom,kgsl-3d0; the A750 node is not described\n");
    } else {
        model = dtString(gpu, "qcom,gpu-model", modelBuffer, sizeof(modelBuffer));
        compatible = dtString(gpu, "compatible", compatibleBuffer, sizeof(compatibleBuffer));
        const void *value = nullptr;
        unsigned length = 0;
        if (SecureDTGetProperty(gpu, "qcom,chipid", &value, &length) == kSuccess && value &&
            length >= sizeof(chipid)) {
            memcpy(&chipid, value, sizeof(chipid));
        }
        tb710fu_gpu_stats[TB710FU_GPU_MODEL] = 1;
    }
    const bool hasSmmu = dtHasNode("/soc/qcom,kgsl-smmu@3da0000") || dtHasNode("/soc/qcom,kgsl-smmu");
    const bool hasGmu = dtHasNode("/soc/qcom,gmu@3d68000") || dtHasNode("/soc/qcom,kgsl-gmu@3d68000") ||
                        dtHasNode("/soc/gmu@3d68000");
    printf("TB710FU GPU: ADT kgsl-3d0 model=%s chipid=%x compatible=%s smmu=%u gmu=%u power=%u\n",
           model, chipid, compatible, hasSmmu ? 1u : 0u, hasGmu ? 1u : 0u,
           tb710fu_gpu_stats[TB710FU_GPU_POWER_LEVEL]);
    printf("TB710FU GPU: ADT windows gpu=0x%llx/0x%x gmu=0x%llx/0x%x gpucc=0x%llx/0x%x smmu=0x%llx/0x%x\n",
           (unsigned long long)TBGPU_GPU_BASE, (unsigned)TBGPU_GPU_BYTES,
           (unsigned long long)TBGPU_GMU_BASE, (unsigned)TBGPU_GMU_BYTES,
           (unsigned long long)TBGPU_GPUCC_BASE, (unsigned)TBGPU_GPUCC_BYTES,
           (unsigned long long)TBGPU_SMMU_BASE, (unsigned)TBGPU_SMMU_GR0_BYTES);
    tb710fu_gpu_stats[TB710FU_GPU_BRINGUP] = Tb710fuGpuStageCensus;
}

extern "C" void tb710fu_gpu_census_read(Tb710fuGpuCensus *census)
{
    if (!census) {
        return;
    }
    memset(census, 0, sizeof(*census));
    const volatile int *addonStage = &native_gpu_addon_stage;
    const volatile int *addonStatus = &native_gpu_addon_status;
    tb710fu_gpu_stats[TB710FU_GPU_ADDON_STAGE] = addonStage ? (uint32_t)*addonStage : 0xffffffffu;
    tb710fu_gpu_stats[TB710FU_GPU_ADDON_STATUS] = addonStatus ? (uint32_t)*addonStatus : 0xffffffffu;
    /* Weak only for the loader driver: tb710fu_addon_find is a strong symbol of
     * this board's kernel, so a plain "always true" test on it would be a
     * pointer-bool conversion warning under -Werror. */
    if (!native_ram_block_gpu_addon) {
        printf("TB710FU GPU: kernel has no NativeRamBlock addon reader\n");
        return;
    }
    const uint8_t *addon = nullptr;
    uint64_t bytes = 0;
    if (!native_ram_block_gpu_addon(&addon, &bytes) || !addon || bytes < TB710FU_ADDON_HEADER) {
        printf("TB710FU GPU: payload carries no GPU addon (stage=%d)\n",
               (int)tb710fu_gpu_stats[TB710FU_GPU_ADDON_STAGE]);
        return;
    }
    Tb710fuAddonEntry entry = {};
    if (tb710fu_addon_find(addon, bytes, kBundleExe, &entry) == TB710FU_ADDON_OK) {
        census->bundle = 1;
    }
    if (tb710fu_addon_find(addon, bytes, kPublish, &entry) == TB710FU_ADDON_OK) {
        census->publish = 1;
    }
    char path[192];
    for (unsigned i = 0; i < kFirmwareCount; ++i) {
        if (joinPath(path, sizeof(path), "Gpu/Firmware/", kFirmware[i].name) &&
            tb710fu_addon_find(addon, bytes, path, &entry) == TB710FU_ADDON_OK) {
            census->firmware++;
        }
    }
    census->announced = 1;
    tb710fu_gpu_stats[TB710FU_GPU_BUNDLE] = census->bundle;
    tb710fu_gpu_stats[TB710FU_GPU_PUBLISH_MARKER] = census->publish;
    tb710fu_gpu_stats[TB710FU_GPU_FIRMWARE] = census->firmware;
    printf("TB710FU GPU: addon bundle=%u publish=%u firmware=%u/4 stage=%d\n",
           census->bundle, census->publish, census->firmware,
           (int)tb710fu_gpu_stats[TB710FU_GPU_ADDON_STAGE]);
}

/* Stage 1..9. Every action is gated by the requested level; an early return
 * leaves the board exactly as the previous stage found it. */
extern "C" void tb710fu_gpu_bring_up(void)
{
    const uint32_t level = tb710fu_gpu_stats[TB710FU_GPU_POWER_LEVEL];
    if (level < 2) {
        return;
    }
    uint32_t status = 0;

    /* ---- stage 2: map and read --------------------------------------------- */
    uint32_t mapped = 0;
    if (tb710fu_gpu_window_map(&gGcc, TBGPU_GCC_GPU_PAGE, TBGPU_GCC_PAGE_BYTES, kIOMapInhibitCache) == 0) {
        mapped |= 1u;
    }
    if (tb710fu_gpu_window_map(&gVote, TBGPU_GCC_VOTE_PAGE, TBGPU_GCC_PAGE_BYTES, kIOMapInhibitCache) == 0) {
        mapped |= 2u;
    }
    if (tb710fu_gpu_window_map(&gGpucc, TBGPU_GPUCC_BASE, TBGPU_GPUCC_BYTES, kIOMapInhibitCache) == 0) {
        mapped |= 4u;
    }
    if (tb710fu_gpu_window_map(&gSmmu, TBGPU_SMMU_BASE, TBGPU_SMMU_GR0_BYTES, kIOMapInhibitCache) == 0) {
        mapped |= 8u;
    }
    /* The GPU window is mapped but never read: RBBM_STATUS is GX-gated. */
    if (tb710fu_gpu_window_map(&gGpu, TBGPU_GPU_BASE, TBGPU_GPU_PROBE_BYTES, kIOMapInhibitCache) == 0) {
        mapped |= 16u;
    }
    tb710fu_gpu_stats[TB710FU_GPU_MMIO_MAPPED] = mapped;
    if (!(mapped & 1u) || !(mapped & 4u)) {
        status = (uint32_t)kIOReturnNoResources;
    }

    uint32_t cfgAhb = 0;
    uint32_t memnoc = 0;
    uint32_t dvm = 0;
    uint32_t pllVote = 0;
    bool gccReadable = rd(gGcc, TBGPU_GCC_GPU_CFG_AHB, &cfgAhb) == 0;
    if (gccReadable) {
        rd(gGcc, TBGPU_GCC_GPU_MEMNOC_GFX, &memnoc);
        rd(gGcc, TBGPU_GCC_GPU_SNOC_DVM_GFX, &dvm);
        if (gVote.va) {
            rd(gVote, TBGPU_GCC_PLL_VOTE, &pllVote);
        }
        printf("TB710FU GPU: stage2 gcc cfg_ahb=0x%x memnoc=0x%x dvm=0x%x gpll0_vote=0x%x\n",
               cfgAhb, memnoc, dvm, pllVote);
    } else {
        status = (uint32_t)kIOReturnNoResources;
        printf("TB710FU GPU: stage2 gcc page not readable (mapping failed)\n");
    }

    uint32_t cx = 0, gx = 0, ccAhb = 0, ccSmmu = 0, ccGmu = 0, cxo = 0, hubCx = 0, demet = 0;
    bool gpuccReadable = tb710fu_gpu_branch_enabled(cfgAhb) && rd(gGpucc, TBGPU_CC_CX_GDSCR, &cx) == 0;
    if (gpuccReadable) {
        rd(gGpucc, TBGPU_CC_GX_GDSCR, &gx);
        rd(gGpucc, TBGPU_CC_AHB_CLK, &ccAhb);
        rd(gGpucc, TBGPU_CC_SMMU_VOTE_CLK, &ccSmmu);
        rd(gGpucc, TBGPU_CC_CX_GMU_CLK, &ccGmu);
        rd(gGpucc, TBGPU_CC_CXO_CLK, &cxo);
        rd(gGpucc, TBGPU_CC_HUB_CX_INT_CLK, &hubCx);
        rd(gGpucc, TBGPU_CC_DEMET_CLK, &demet);
        printf("TB710FU GPU: stage2 gpucc cx=0x%x gx=0x%x ahb=0x%x smmu_vote=0x%x cx_gmu=0x%x "
               "cxo=0x%x hub_cx=0x%x demet=0x%x\n",
               cx, gx, ccAhb, ccSmmu, ccGmu, cxo, hubCx, demet);
    } else if (!gccReadable) {
        printf("TB710FU GPU: stage2 gpucc skipped: gcc page unavailable\n");
    } else {
        printf("TB710FU GPU: stage2 gpucc skipped: gcc_gpu_cfg_ahb_clk is off\n");
    }
    if (gpuccReadable && !tb710fu_gpu_gdsc_on(cx)) {
        printf("TB710FU GPU: stage2 smmu skipped: CX is off (use gpu-power>=3)\n");
    }
    printf("TB710FU GPU: stage2 gpu skipped: GX is off (the GMU owns it)\n");
    tb710fu_gpu_stats[TB710FU_GPU_BRINGUP] = Tb710fuGpuStageMmio;
    if (level < 3) {
        printf("TB710FU GPU: bringup stage=2 of 9 (gpu-power=%u)\n", level);
        return;
    }
    /* level >= 3 may write. gcc_gpu_cfg_ahb_clk is the one write Linux's gcc probe
     * makes (qcom_branch_set_clk_en) and it is the precondition of every GPUCC read. */
    if (!gpuccReadable && gccReadable && !tb710fu_gpu_branch_enabled(cfgAhb)) {
        if (wr(gGcc, TBGPU_GCC_GPU_CFG_AHB, cfgAhb | TBGPU_CBCR_ENABLE) == 0) {
            tb710fu_gpu_delay_us(10);
            if (rd(gGcc, TBGPU_GCC_GPU_CFG_AHB, &cfgAhb) == 0) {
                printf("TB710FU GPU: stage3 gcc cfg_ahb enabled readback=0x%x\n", cfgAhb);
                gpuccReadable = tb710fu_gpu_branch_enabled(cfgAhb) && rd(gGpucc, TBGPU_CC_CX_GDSCR, &cx) == 0;
            }
        }
    }
    if (!gccReadable || !gpuccReadable) {
        tb710fu_gpu_stats[TB710FU_GPU_STATUS] = status ? status : (uint32_t)kIOReturnNotReady;
        printf("TB710FU GPU: bringup stopped at stage 2 (cx needs the full GPUCC read)\n");
        return;
    }

    /* ---- stage 3: CX domain and its two clocks ----------------------------- */
    uint32_t cxAfter = 0;
    uint32_t hwctrl = 0;
    const int cxResult = enableCx(&cxAfter, &hwctrl);
    uint32_t voteNow = 0;
    uint32_t ahbNow = 0;
    rd(gGpucc, TBGPU_CC_SMMU_VOTE_CLK, &voteNow);
    rd(gGpucc, TBGPU_CC_AHB_CLK, &ahbNow);
    ccAhb = ahbNow;
    ccSmmu = voteNow;
    tb710fu_gpu_stats[TB710FU_GPU_CX] = tb710fu_gpu_gdsc_on(cxAfter) ? 1u : 0u;
    tb710fu_gpu_stats[TB710FU_GPU_CLOCKS] = tb710fu_gpu_clock_on(voteNow) ? 1u : 0u;
    if (cxResult != 0) {
        tb710fu_gpu_stats[TB710FU_GPU_STATUS] = (uint32_t)kIOReturnTimeout;
        printf("TB710FU GPU: stage3 cx failed code=%d gdscr=0x%x hw_ctrl=0x%x (writes=%u)\n",
               cxResult, cxAfter, hwctrl, gWrites);
        printf("TB710FU GPU: bringup stopped at stage 3\n");
        return;
    }
    gCxOn = 1;
    printf("TB710FU GPU: stage3 cx on gdscr=0x%x hw_ctrl=0x%x ahb=0x%x smmu_vote=0x%x (writes=%u)\n",
           cxAfter, hwctrl, ccAhb, ccSmmu, gWrites);
    tb710fu_gpu_stats[TB710FU_GPU_BRINGUP] = Tb710fuGpuStageCx;
    if (level < 4) {
        printf("TB710FU GPU: bringup stage=3 of 9 (gpu-power=%u)\n", level);
        return;
    }

    /* ---- stage 4: SMMU global space inventory (read-only) -------------------- */
    uint32_t scr0 = 0, id0 = 0, id1 = 0, id2 = 0, gfsr = 0;
    if (!gSmmu.va || rd(gSmmu, TBGPU_SMMU_SCR0, &scr0) != 0 || rd(gSmmu, TBGPU_SMMU_ID0, &id0) != 0 ||
        rd(gSmmu, TBGPU_SMMU_ID1, &id1) != 0 || rd(gSmmu, TBGPU_SMMU_ID2, &id2) != 0) {
        tb710fu_gpu_stats[TB710FU_GPU_STATUS] = (uint32_t)kIOReturnNoResources;
        printf("TB710FU GPU: stage4 smmu not readable (GR0 mapping failed)\n");
        printf("TB710FU GPU: bringup stopped at stage 4\n");
        return;
    }
    rd(gSmmu, TBGPU_SMMU_GFSR, &gfsr);
    unsigned smrCount = tb710fu_gpu_num_smr(id0);
    if (smrCount > TBGPU_SMMU_MAX_SMR) {
        smrCount = TBGPU_SMMU_MAX_SMR;
    }
    uint32_t firstSmrIndex = 0xffffffffu;
    uint32_t firstSmrValue = 0;
    uint32_t firstS2crValue = 0;
    uint32_t valid = 0;
    for (unsigned i = 0; i < smrCount; ++i) {
        uint32_t smr = 0;
        if (rd(gSmmu, TBGPU_SMMU_SMR(i), &smr) != 0) {
            break;
        }
        if (smr & 0x80000000u) {
            valid++;
            if (firstSmrIndex == 0xffffffffu) {
                firstSmrIndex = i;
                firstSmrValue = smr;
                rd(gSmmu, TBGPU_SMMU_S2CR(i), &firstS2crValue);
            }
        }
    }
    gSmmuSmrValid = valid;
    gSmmuId2 = id2;
    tb710fu_gpu_stats[TB710FU_GPU_SMMU_ID] = id2;
    tb710fu_gpu_stats[TB710FU_GPU_SMMU_SMRS] = valid;
    printf("TB710FU GPU: stage4 smmu scr0=0x%x id0=0x%x id1=0x%x id2=0x%x oas=%u gfsr=0x%x "
           "smrs=%u/%u first=%d smr=0x%x s2cr=0x%x\n",
           scr0, id0, id1, id2, tb710fu_gpu_oas(id2), gfsr, valid, smrCount,
           firstSmrIndex == 0xffffffffu ? -1 : (int)firstSmrIndex, firstSmrValue, firstS2crValue);
    printf("TB710FU GPU: stage4 smmu programmed=none (contexts need the pinned ArmSmmu4K tables)\n");
    tb710fu_gpu_stats[TB710FU_GPU_BRINGUP] = Tb710fuGpuStageSmmu;
    if (level < 5) {
        printf("TB710FU GPU: bringup stage=4 of 9 (gpu-power=%u)\n", level);
        return;
    }

    /* ---- stage 5: GMU clock tree ------------------------------------------- */

    uint32_t pll0Mode = 0, pll0Opmode = 0, pll1Mode = 0, pll1Opmode = 0, pll1User = 0;
    uint32_t hubCfg = 0, gmuCfg = 0, cxGmuAfter = 0;
    uint32_t step = 1;
    int failed = 0;
    auto gmuStep = [&](uint32_t value) { step = value; };
    if (rd(gGpucc, TBGPU_CC_PLL0 + TBGPU_PLL_MODE, &pll0Mode) != 0 ||
        rd(gGpucc, TBGPU_CC_PLL0 + TBGPU_PLL_OPMODE, &pll0Opmode) != 0 ||
        rd(gGpucc, TBGPU_CC_PLL1 + TBGPU_PLL_MODE, &pll1Mode) != 0 ||
        rd(gGpucc, TBGPU_CC_PLL1 + TBGPU_PLL_OPMODE, &pll1Opmode) != 0) {
        failed = 2;
    }
    if (!failed) {
        /* GPLL0 votes (gcc_gpu_gpll0_clk_src / _div_clk_src, bits 15/16). */
        gmuStep(1);
        uint32_t vote = 0;
        if (rd(gVote, TBGPU_GCC_PLL_VOTE, &vote) != 0) {
            failed = 2;
        } else if ((vote & TBGPU_GPLL0_VOTES) != TBGPU_GPLL0_VOTES) {
            if (wr(gVote, TBGPU_GCC_PLL_VOTE, vote | TBGPU_GPLL0_VOTES) != 0) {
                failed = 2;
            } else {
                tb710fu_gpu_delay_us(1000);
            }
        }
    }
    if (!failed) {
        gmuStep(2);
        if (!pllRunning(pll1Mode, pll1Opmode)) {
            if (!pllRunning(pll0Mode, pll0Opmode) &&
                pllConfigure(gGpucc, TBGPU_CC_PLL0, TBGPU_PLL0_L, TBGPU_PLL0_ALPHA) != 0) {
                failed = 2;
            }
            if (!failed && pllConfigure(gGpucc, TBGPU_CC_PLL1, TBGPU_PLL1_L, TBGPU_PLL1_ALPHA) != 0) {
                failed = 2;
            }
            if (!failed) {
                gmuStep(3);
                if (rd(gGpucc, TBGPU_CC_PLL1 + TBGPU_PLL_MODE, &pll1Mode) != 0 ||
                    wr(gGpucc, TBGPU_CC_PLL1 + TBGPU_PLL_MODE, pll1Mode | TBGPU_PLL_RESET_N) != 0 ||
                    wr(gGpucc, TBGPU_CC_PLL1 + TBGPU_PLL_OPMODE, TBGPU_PLL_RUN) != 0) {
                    failed = 2;
                }
            }
            if (!failed) {
                bool locked = false;
                for (unsigned i = 0; i < 1500; ++i) {
                    if (rd(gGpucc, TBGPU_CC_PLL1 + TBGPU_PLL_MODE, &pll1Mode) != 0) {
                        failed = 2;
                        break;
                    }
                    if (pll1Mode & TBGPU_PLL_LOCK_DET) {
                        locked = true;
                        break;
                    }
                    tb710fu_gpu_delay_us(1);
                }
                if (!failed && !locked) {
                    failed = 1;
                }
            }
            if (!failed) {
                if (rd(gGpucc, TBGPU_CC_PLL1 + TBGPU_PLL_USER_CTL, &pll1User) != 0 ||
                    wr(gGpucc, TBGPU_CC_PLL1 + TBGPU_PLL_USER_CTL, pll1User | TBGPU_PLL_OUT_MASK) != 0 ||
                    rd(gGpucc, TBGPU_CC_PLL1 + TBGPU_PLL_MODE, &pll1Mode) != 0 ||
                    wr(gGpucc, TBGPU_CC_PLL1 + TBGPU_PLL_MODE, pll1Mode | TBGPU_PLL_OUT_CTRL) != 0) {
                    failed = 2;
                }
            }
        }
    }
    if (!failed) {
        gmuStep(4);
        const int hub = rcgSet(gGpucc, TBGPU_CC_HUB_RCGR, TBGPU_HUB_SEL, TBGPU_HUB_DIV, &hubCfg);
        if (hub) {
            failed = hub < 0 ? 2 : 1;
        }
    }
    if (!failed) {
        gmuStep(5);
        const int gmu = rcgSet(gGpucc, TBGPU_CC_GMU_RCGR, TBGPU_GMU_SEL, TBGPU_GMU_DIV, &gmuCfg);
        if (gmu) {
            failed = gmu < 0 ? 2 : 1;
        }
    }
    if (!failed) {
        gmuStep(6);
        if (branchEnable(gGpucc, TBGPU_CC_CXO_AON_CLK) != 0 ||
            branchEnable(gGpucc, TBGPU_CC_DEMET_CLK) != 0 ||
            branchEnable(gGpucc, TBGPU_CC_AHB_CLK) != 0 ||
            branchEnable(gGpucc, TBGPU_CC_CX_GMU_CLK) != 0 ||
            branchEnable(gGpucc, TBGPU_CC_CXO_CLK) != 0 ||
            branchEnable(gGpucc, TBGPU_CC_HUB_CX_INT_CLK) != 0 ||
            branchEnable(gGcc, TBGPU_GCC_GPU_MEMNOC_GFX) != 0) {
            failed = 2;
        }
        /* GCC_DDRSS_GPU_AXI (0x71154) is the one GMU clock whose SM8650 offset is
         * not verified in this port; it is read and reported, never written. */
        uint32_t axi = 0;
        rd(gGcc, TBGPU_GCC_DDRSS_GPU_AXI, &axi);
        bool running = false;
        for (unsigned i = 0; i < 500; ++i) {
            if (rd(gGpucc, TBGPU_CC_CX_GMU_CLK, &cxGmuAfter) != 0) {
                failed = 2;
                break;
            }
            if (tb710fu_gpu_clock_on(cxGmuAfter)) {
                running = true;
                break;
            }
            tb710fu_gpu_delay_us(1);
        }
        if (!failed && !running) {
            failed = 1;
        }
        if (!failed) {
            gmuStep(7);
        }
        printf("TB710FU GPU: stage5 gmu_clocks step=%u/7 failed=%d pll1_mode=0x%x opmode=0x%x "
               "hub_cfg=0x%x gmu_cfg=0x%x cx_gmu=0x%x ddrs_axi=0x%x (writes=%u)\n",
               step, failed, pll1Mode, pll1Opmode, hubCfg, gmuCfg, cxGmuAfter, axi, gWrites);
    } else {
        printf("TB710FU GPU: stage5 gmu_clocks step=%u/7 failed=%d pll1_mode=0x%x (writes=%u)\n",
               step, failed, pll1Mode, gWrites);
    }
    gMmuClocksStep = step;
    gMmuClocksFailed = (uint32_t)failed;
    tb710fu_gpu_stats[TB710FU_GPU_GMU_CLOCKS] = failed ? 0u : step;
    tb710fu_gpu_stats[TB710FU_GPU_BRINGUP] = Tb710fuGpuStageGmuClocks;
    if (failed) {
        tb710fu_gpu_stats[TB710FU_GPU_STATUS] = (uint32_t)(failed == 1 ? kIOReturnTimeout : kIOReturnIOError);
        printf("TB710FU GPU: bringup stopped at stage 5 (GMU clocks)\n");
        return;
    }
    if (level < 6) {
        printf("TB710FU GPU: bringup stage=5 of 9 (gpu-power=%u)\n", level);
        return;
    }

    /* ---- stage 6: firmware provenance -------------------------------------- */

    uint32_t verified = 0;
    uint32_t totalBytes = 0;
    uint32_t fromAddon = 0;
    for (unsigned i = 0; i < kFirmwareCount; ++i) {
        Tb710fuGpuFirmware image = {};
        const int result = tb710fu_gpu_firmware_verify(kFirmware[i].name, kFirmware[i].bytes,
                                                       kFirmware[i].sha256, &image);
        if (result == 0) {
            verified++;
            totalBytes += (uint32_t)image.bytes;
            if (image.source == 1) {
                fromAddon++;
            }
            printf("TB710FU GPU: stage6 fw %s=ok bytes=%u source=%s digest=%02x%02x%02x%02x\n",
                   kFirmware[i].name, (unsigned)image.bytes, image.source == 1 ? "addon" : "data",
                   image.digest[0], image.digest[1], image.digest[2], image.digest[3]);
        } else {
            printf("TB710FU GPU: stage6 fw %s=fail(%d) expected=%u\n",
                   kFirmware[i].name, result, (unsigned)kFirmware[i].bytes);
        }
    }
    gFirmwareVerified = verified;
    gFirmwareBytes = totalBytes;
    gFirmwareAddon = fromAddon;
    tb710fu_gpu_stats[TB710FU_GPU_FIRMWARE_VERIFIED] = verified;
    tb710fu_gpu_stats[TB710FU_GPU_BRINGUP] = Tb710fuGpuStageFirmware;
    printf("TB710FU GPU: stage6 firmware=%u/4 bytes=%u addon=%u\n", verified, totalBytes, fromAddon);
    if (verified != kFirmwareCount) {
        tb710fu_gpu_stats[TB710FU_GPU_STATUS] = (uint32_t)kIOReturnNotFound;
        printf("TB710FU GPU: bringup stopped at stage 6 (firmware provenance)\n");
        return;
    }

    /* ---- stages 7 and 8: prerequisites that are not pinned yet --------------- */
    printf("TB710FU GPU: stage7 skipped: GMU cold start needs the GMU SMMU context (bank 1, ArmSmmu4K) "
           "and the SM8650 RSC/PDC setup (not pinned)\n");
    printf("TB710FU GPU: stage8 skipped: GX needs HFI startup votes from the AOP command DB tables "
           "(not derived for SM8650)\n");
    tb710fu_gpu_stats[TB710FU_GPU_STATUS] = (uint32_t)kIOReturnUnsupported;
    if (level < 9) {
        printf("TB710FU GPU: bringup stage=6 of 9 (gpu-power=%u)\n", level);
        return;
    }

    /* ---- stage 9: ABI published; the ring still needs 7 and 8 ---------------- */
    tb710fu_gpu_stats[TB710FU_GPU_ABI] = 1;
    tb710fu_gpu_stats[TB710FU_GPU_RING_READY] = 0;
    tb710fu_gpu_stats[TB710FU_GPU_BRINGUP] = Tb710fuGpuStageAbi;
    printf("TB710FU GPU: stage9 abi live version=2 ring=0 (Tb710fuGpuClient accepts type=0x4147)\n");
    printf("TB710FU GPU: bringup stage=9 of 9 ring=0 cx=1 gmu_clocks=1 firmware=4/4 (writes=%u)\n", gWrites);
}

extern "C" void tb710fu_gpu_power_meta_init(void)
{
    if (!tb710fu_gpu_wanted()) {
        return;
    }
    tb710fu_gpu_power_static_init();
    printf("TB710FU GPU: Tb710fuGpuPower metaclass installed\n");
}

extern "C" void tb710fu_gpu_power_publish(const Tb710fuGpuCensus *census)
{
    if (!census || tb710fu_gpu_stats[TB710FU_GPU_POWER_LEVEL] < 2) {
        return;
    }
    Tb710fuGpuPower *domain = new Tb710fuGpuPower;
    if (!domain) {
        return;
    }
    if (!domain->init()) {
        domain->release();
        return;
    }
    domain->setProperty("Tb710fuGpuDomain", "gpu-cx-gx");
    domain->setProperty("Tb710fuGpuCompatible", "qcom,adreno-gpu-gen7-9-1");
    OSNumber *level = OSNumber::withNumber(tb710fu_gpu_stats[TB710FU_GPU_POWER_LEVEL], 32);
    OSNumber *stage = OSNumber::withNumber(tb710fu_gpu_stats[TB710FU_GPU_BRINGUP], 32);
    OSNumber *cx = OSNumber::withNumber(gCxOn, 32);
    OSNumber *smrs = OSNumber::withNumber(gSmmuSmrValid, 32);
    OSNumber *id2 = OSNumber::withNumber(gSmmuId2, 32);
    OSNumber *clocks = OSNumber::withNumber(gMmuClocksFailed ? 0u : gMmuClocksStep, 32);
    OSNumber *firmware = OSNumber::withNumber(gFirmwareVerified, 32);
    OSNumber *bytes = OSNumber::withNumber(gFirmwareBytes, 32);
    OSNumber *ring = OSNumber::withNumber(tb710fu_gpu_stats[TB710FU_GPU_RING_READY], 32);
    OSNumber *bundle = OSNumber::withNumber((uint64_t)census->bundle, 32);
    if (level) { domain->setProperty("Tb710fuGpuPowerLevel", level); level->release(); }
    if (stage) { domain->setProperty("Tb710fuGpuBringUpStage", stage); stage->release(); }
    if (cx) { domain->setProperty("Tb710fuGpuCxOn", cx); cx->release(); }
    if (smrs) { domain->setProperty("Tb710fuGpuSmmuSmrs", smrs); smrs->release(); }
    if (id2) { domain->setProperty("Tb710fuGpuSmmuId2", id2); id2->release(); }
    if (clocks) { domain->setProperty("Tb710fuGpuGmuClocks", clocks); clocks->release(); }
    if (firmware) { domain->setProperty("Tb710fuGpuFirmware", firmware); firmware->release(); }
    if (bytes) { domain->setProperty("Tb710fuGpuFirmwareBytes", bytes); bytes->release(); }
    if (ring) { domain->setProperty("Tb710fuGpuRingReady", ring); ring->release(); }
    if (bundle) { domain->setProperty("Tb710fuGpuBundle", bundle); bundle->release(); }
    domain->setName("Tb710fuGpuPower");
    IOService *platform = IOService::getPlatform();
    if (!platform || !domain->attach(platform)) {
        printf("TB710FU GPU: power domain node not attached\n");
        domain->release();
        return;
    }
    domain->registerService();
    tb710fu_gpu_stats[TB710FU_GPU_POWER_STAGE] = 2;
    printf("TB710FU GPU: power domain node registered (level=%u stage=%u)\n",
           tb710fu_gpu_stats[TB710FU_GPU_POWER_LEVEL], tb710fu_gpu_stats[TB710FU_GPU_BRINGUP]);
    domain->release();
}

extern "C" int tb710fu_gpu_abi_ready(void)
{
    return tb710fu_gpu_stats[TB710FU_GPU_ABI] != 0;
}

extern "C" int tb710fu_gpu_ring_ready(void)
{
    return tb710fu_gpu_stats[TB710FU_GPU_RING_READY] != 0;
}
