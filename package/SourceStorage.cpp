// SPDX-License-Identifier: BSD-2-Clause-Patent
// Adapted from UniXNU/UniXNU-sheng@1108068537d5396edb2c63b2f8c6f5b97b0a4bbc,
// package/SourceStorage.cpp. This slice starts only authenticated, read-only UFS.
// See Tb710fuSourceStorage/README.md for the separate ABI and integration gates.
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void source_storage_base_init(void) __asm__("__GLOBAL__sub_I_IOBlockStorageDevice.cpp");
extern "C" void source_storage_ufs_init(void) __asm__("__GLOBAL__sub_I_NativeUfs.cpp");
extern "C" void source_storage_ram_init(void) __asm__("__GLOBAL__sub_I_NativeRamBlock.cpp");
extern "C" IOService *native_ufs_source_create(void);
extern "C" bool native_source_root_policies_load(void);
extern "C" volatile uint32_t native_ufs_root_step;
extern "C" const char *volatile native_ufs_root_failure;
extern "C" int native_ufs_store_write(uint64_t block, uint32_t count, const void *src);

// Pinned ShengUfsPort provides FUA native_ufs_store_write() without the newer
// non-FUA cached variant; delegate directly to real FUA UFS writes so every
// NativeRamBlock persistent overlay write lands durably on media.
extern "C" int native_ufs_store_write_cached(uint64_t block, uint32_t count, const void *src)
{
    return native_ufs_store_write(block, count, src);
}

namespace {
const OSMetaClass *storageParent;
IOService *storageDriver;
uint32_t attempted;
}

// Synchronous boot-progress checkpoints for the UFS takeover path.
//
// Source: TB710FU device source storage (this file). Reason: the device photo
// IMG20261120171040 reached "source-storage-start begin" with no "done", no
// panic and ERR=0, so execution is inside this function but the 0032 markers
// cannot show which call blocks. Each step writes a text line into the live
// capture ring (the console tap on printf) and immediately refreshes the
// on-screen scroll log. The refresh is synchronous because a halted path may
// hold a lock or disable interrupts, in which case the checkpoint worker never
// runs and a ring-only printf would stay invisible.
//
// The provider is a weak, conditional reference, matching the conditional
// strong hook in patch 0011: standalone archive builds that do not link the
// live-log archive still compile and archive with no undefined strong symbol,
// while a kernel link carrying that archive binds it. The provider itself is
// gated at runtime by tb710fu-live-log / tb710fu-live-checkpoints boot args.
extern "C" void tb710fu_live_log_checkpoint_text(const char *stage)
    __attribute__((weak_import));

#define TB710FU_SS_STEP(label)                                    \
    do {                                                          \
        printf("TB710FU SourceStorage: " label "\n");             \
        if (tb710fu_live_log_checkpoint_text)                     \
            tb710fu_live_log_checkpoint_text("ss-" label);        \
    } while (0)

// The generated public base implementation registers below the original class.
// Keep the C++ class name for compiler-authored arm64e PAC discriminators.
extern "C" const OSMetaClass *native_source_storage_parent(void)
{
    if (!storageParent) panic("TB710FU SourceStorage: base registered before original family");
    return storageParent;
}

// The device's conditional strong bsd_init hook calls this after vfsinit,
// before root discovery. The boot argument remains an explicit runtime gate.
extern "C" void native_source_storage_start(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("native-source-storage", &enabled, sizeof(enabled)) || enabled != 1) return;
    if (__atomic_exchange_n(&attempted, 1, __ATOMIC_ACQ_REL)) return;

    TB710FU_SS_STEP("gate-passed");

    TB710FU_SS_STEP("kext-ioStorage-begin");
    OSReturn status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
    if (status != kOSReturnSuccess)
        panic("TB710FU SourceStorage: IOStorageFamily load failed %x", status);
    TB710FU_SS_STEP("kext-ioStorage-done");

    TB710FU_SS_STEP("base-class-lookup-begin");
    const OSSymbol *name = OSSymbol::withCString("IOBlockStorageDevice");
    storageParent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    if (name) name->release();
    // A runtime size check supplements the separate binary dispatch/PAC audit.
    if (!storageParent || storageParent->getClassSize() != sizeof(IOBlockStorageDevice))
        panic("TB710FU SourceStorage: base size %u differs from source %lu",
              storageParent ? storageParent->getClassSize() : 0, sizeof(IOBlockStorageDevice));
    TB710FU_SS_STEP("base-class-lookup-done");

    TB710FU_SS_STEP("root-policy-begin");
    if (!native_source_root_policies_load())
        panic("TB710FU SourceStorage: root policy handoff authentication failed");
    TB710FU_SS_STEP("root-policy-done");

    TB710FU_SS_STEP("kext-apfs-begin");
    status = OSKextLoadKextWithIdentifier("com.apple.filesystems.apfs");
    if (status != kOSReturnSuccess)
        panic("TB710FU SourceStorage: APFS load failed %x", status);
    TB710FU_SS_STEP("kext-apfs-done");

    TB710FU_SS_STEP("premodload-begin");
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("TB710FU SourceStorage: cannot begin class registration");
    TB710FU_SS_STEP("premodload-done");

    TB710FU_SS_STEP("class-reg-base-begin");
    source_storage_base_init();
    TB710FU_SS_STEP("class-reg-base-done");
    TB710FU_SS_STEP("class-reg-ufs-begin");
    source_storage_ufs_init();
    TB710FU_SS_STEP("class-reg-ufs-done");
    TB710FU_SS_STEP("class-reg-ram-begin");
    source_storage_ram_init();
    TB710FU_SS_STEP("class-reg-ram-done");
    TB710FU_SS_STEP("postmodload-begin");
    status = OSMetaClass::postModLoad(handle);
    if (status != kOSReturnSuccess)
        panic("TB710FU SourceStorage: class registration failed %x", status);
    TB710FU_SS_STEP("postmodload-done");

    TB710FU_SS_STEP("wait-platform-begin");
    OSDictionary *matching = IOService::serviceMatching("IOPlatformExpertDevice");
    IOService *provider = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    // Unlike waitForService, waitForMatchingService does not consume matching.
    if (matching) matching->release();
    if (!provider) panic("TB710FU SourceStorage: platform root unavailable");
    TB710FU_SS_STEP("wait-platform-done");

    TB710FU_SS_STEP("ufs-create-begin");
    storageDriver = native_ufs_source_create();
    if (!storageDriver) panic("TB710FU SourceStorage: cannot allocate UFS service");
    TB710FU_SS_STEP("ufs-create-done");

    // Expand the former short-circuit ternary into equivalent ordered steps so
    // each UFS lifecycle call has its own pre-call marker. init -> attach ->
    // start short-circuit behavior and the recorded `step` value are unchanged.
    const char *step;
    TB710FU_SS_STEP("ufs-init-begin");
    if (!storageDriver->init()) {
        step = "init";
    } else {
        TB710FU_SS_STEP("ufs-init-done");
        TB710FU_SS_STEP("ufs-attach-begin");
        if (!storageDriver->attach(provider)) {
            step = "attach";
        } else {
            TB710FU_SS_STEP("ufs-attach-done");
            TB710FU_SS_STEP("ufs-start-begin");
            step = storageDriver->start(provider) ? nullptr : "start";
            if (!step) TB710FU_SS_STEP("ufs-start-done");
        }
    }
    // Failure after hardware acquisition must keep the driver's DMA allocation
    // alive. NativeUfs owns transactional queue/SMMU recovery, so never detach
    // or free it here after a partial start.
    if (step)
        panic("TB710FU SourceStorage: UFS %s failed (root step %u: %s)", step, native_ufs_root_step,
              native_ufs_root_failure ? native_ufs_root_failure : "no reason recorded");

    // Start the Data seed RAM disk after UFS (whose store window persists
    // changed Data pages when data-persist=2 is active).
    IORegistryEntry *dtMap = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
    const bool hasSeed = dtMap && (dtMap->getProperty("PackedRAMDisk") || dtMap->getProperty("RAMDisk"));
    OSSafeReleaseNULL(dtMap);
    if (hasSeed) {
        TB710FU_SS_STEP("ramdisk-start-begin");
        IOService *disk = OSDynamicCast(IOService, OSMetaClass::allocClassWithName("NativeRamBlock"));
        if (!disk) panic("TB710FU SourceStorage: cannot allocate NativeRamBlock");
        const char *rstep = !disk->init() ? "init" : !disk->attach(provider) ? "attach" :
                            !disk->start(provider) ? "start" : nullptr;
        if (rstep) panic("TB710FU SourceStorage: NativeRamBlock %s failed", rstep);
        TB710FU_SS_STEP("ramdisk-start-done");
    }

    provider->release();
    TB710FU_SS_STEP("started");
}
