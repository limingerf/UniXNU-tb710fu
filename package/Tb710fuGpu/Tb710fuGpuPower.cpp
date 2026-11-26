// SPDX-License-Identifier: BSD-2-Clause-Patent
// GpuPower for the SM8650 (Adreno 750) milestone: a staged, read-only census.
//
// gpu-power=<n>
//   1  survey the ADT's kgsl-3d0 / kgsl-smmu nodes and the boot addon's GPU
//      entries, and print and count the result
//   2  also publish Tb710fuGpuPower (a plain IOService) on the platform expert
//      with that census as properties, so a later GMU/SMMU stage has a registry
//      anchor whose prerequisites are already recorded
//   >2 the same as 2 (reserved for the power-domain enable stage)
//
// This file deliberately maps no device memory and writes no register: the GPU
// power domain, RPMh/RSCC votes, GMU firmware handshake and SMMU attachment each
// need their ownership qualified first, which is the order the reference
// SM8650 project keeps and the reason its own MMIO trials came after the power
// and security closure. Milestone 1 therefore only records what the board and
// the payload already say (docs/GPU_METAL_M1.md).
#include "Tb710fuGpu.h"
#include "Tb710fuAddon.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/libkern.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>

/* From the loader driver (weak: a kernel built without it has no addon) and
 * the kernel's gpu-addon mount counters. tb710fu_addon_find comes from
 * package/Tb710fuAdsp and is always present in this board's kernel. */
extern "C" int native_ram_block_gpu_addon(const uint8_t **data, uint64_t *bytes)
    __attribute__((weak_import));
extern "C" volatile int native_gpu_addon_stage __attribute__((weak_import));
extern "C" volatile int native_gpu_addon_status __attribute__((weak_import));

/* The constructor clang emits for this translation unit, called only from
 * tb710fu_gpu_power_meta_init (never from __mod_init_func). */
extern "C" void tb710fu_gpu_power_static_init(void)
    __asm__("__GLOBAL__sub_I_Tb710fuGpuPower.cpp");

namespace {
/* The real-metal firmware the reference SM8650 project pins for gen70900 (same
 * GPU family, same blob names). Milestone 1 only reports whether the payload
 * carries them; nothing loads them. */
const char *const kFirmware[] = {
    "Gpu/Firmware/gmu_gen70900.bin",
    "Gpu/Firmware/gen70900_sqe.fw",
    "Gpu/Firmware/gen70900_aqe.fw",
    "Gpu/Firmware/gen70900_zap.mbn",
};
const char *const kBundleExe =
    "GPUBundles/NativeAdrenoMetal.bundle/Contents/MacOS/NativeAdrenoMetal";
const char *const kPublish = "GPUBundles/NativeAdrenoMetal.publish";

uint32_t weakInt(const volatile int *value, uint32_t fallback)
{
    return value ? (uint32_t)*value : fallback;
}

/* A bounded copy of a DT string property; the ADT's value includes its NUL. */
const char *dtString(DTEntry entry, const char *name, char *buffer, unsigned size)
{
    const void *value = nullptr;
    unsigned length = 0;
    strlcpy(buffer, "-", size);
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
    DTEntry gpu = nullptr;
    if (SecureDTLookupEntry(nullptr, "/soc/qcom,kgsl-3d0", &gpu) != kSuccess || !gpu) {
        printf("TB710FU GPU: ADT has no /soc/qcom,kgsl-3d0; the A750 node is not described\n");
    } else {
        model = dtString(gpu, "qcom,gpu-model", modelBuffer, sizeof(modelBuffer));
        compatible = dtString(gpu, "compatible", compatibleBuffer, sizeof(compatibleBuffer));
        const void *value = nullptr;
        unsigned length = 0;
        if (SecureDTGetProperty(gpu, "qcom,chipid", &value, &length) == kSuccess && value && length >= sizeof(chipid)) {
            memcpy(&chipid, value, sizeof(chipid));
        }
        tb710fu_gpu_stats[TB710FU_GPU_MODEL] = 1;
    }
    /* The board's own DTS names the GPU's SMMU kgsl-smmu@3da0000; presence only,
     * no mapping. */
    DTEntry smmu = nullptr;
    const bool hasSmmu = SecureDTLookupEntry(nullptr, "/soc/qcom,kgsl-smmu@3da0000", &smmu) == kSuccess && smmu;
    printf("TB710FU GPU: ADT kgsl-3d0 model=%s chipid=%x compatible=%s smmu=%u power=%u\n",
           model, chipid, compatible, hasSmmu ? 1u : 0u, tb710fu_gpu_stats[TB710FU_GPU_POWER_LEVEL]);
}

extern "C" void tb710fu_gpu_census_read(Tb710fuGpuCensus *census)
{
    if (!census) {
        return;
    }
    memset(census, 0, sizeof(*census));
    tb710fu_gpu_stats[TB710FU_GPU_ADDON_STAGE] = weakInt(&native_gpu_addon_stage, 0xffffffffu);
    tb710fu_gpu_stats[TB710FU_GPU_ADDON_STATUS] = weakInt(&native_gpu_addon_status, 0xffffffffu);
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
    for (size_t i = 0; i < sizeof(kFirmware) / sizeof(kFirmware[0]); i++) {
        if (tb710fu_addon_find(addon, bytes, kFirmware[i], &entry) == TB710FU_ADDON_OK) {
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
    OSNumber *value = OSNumber::withNumber((uint64_t)census->bundle, 32);
    if (level) {
        domain->setProperty("Tb710fuGpuPowerLevel", level);
        level->release();
    }
    if (value) {
        domain->setProperty("Tb710fuGpuBundle", value);
        value->release();
    }
    domain->setName("Tb710fuGpuPower");
    IOService *platform = IOService::getPlatform();
    if (!platform || !domain->attach(platform)) {
        printf("TB710FU GPU: power domain node not attached\n");
        domain->release();
        return;
    }
    domain->registerService();
    tb710fu_gpu_stats[TB710FU_GPU_POWER_STAGE] = 2;
    printf("TB710FU GPU: power domain node registered (level=%u)\n",
           tb710fu_gpu_stats[TB710FU_GPU_POWER_LEVEL]);
    domain->release();
}
