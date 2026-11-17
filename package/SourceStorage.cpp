// SPDX-License-Identifier: BSD-2-Clause-Patent
// Adapted from UniXNU/UniXNU-sheng@1108068537d5396edb2c63b2f8c6f5b97b0a4bbc,
// package/SourceStorage.cpp. This slice starts only authenticated, read-only UFS.
// See Tb710fuSourceStorage/README.md for the separate ABI and integration gates.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void source_storage_base_init(void) __asm__("__GLOBAL__sub_I_IOBlockStorageDevice.cpp");
extern "C" void source_storage_ufs_init(void) __asm__("__GLOBAL__sub_I_NativeUfs.cpp");
extern "C" IOService *native_ufs_source_create(void);
extern "C" bool native_source_root_policies_load(void);
extern "C" volatile uint32_t native_ufs_root_step;
extern "C" const char *volatile native_ufs_root_failure;

namespace {
const OSMetaClass *storageParent;
IOService *storageDriver;
uint32_t attempted;
}

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

    OSReturn status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
    if (status != kOSReturnSuccess)
        panic("TB710FU SourceStorage: IOStorageFamily load failed %x", status);
    const OSSymbol *name = OSSymbol::withCString("IOBlockStorageDevice");
    storageParent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    if (name) name->release();
    // A runtime size check supplements the separate binary dispatch/PAC audit.
    if (!storageParent || storageParent->getClassSize() != sizeof(IOBlockStorageDevice))
        panic("TB710FU SourceStorage: base size %u differs from source %lu",
              storageParent ? storageParent->getClassSize() : 0, sizeof(IOBlockStorageDevice));
    if (!native_source_root_policies_load())
        panic("TB710FU SourceStorage: root policy handoff authentication failed");
    status = OSKextLoadKextWithIdentifier("com.apple.filesystems.apfs");
    if (status != kOSReturnSuccess)
        panic("TB710FU SourceStorage: APFS load failed %x", status);

    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("TB710FU SourceStorage: cannot begin class registration");
    source_storage_base_init();
    source_storage_ufs_init();
    status = OSMetaClass::postModLoad(handle);
    if (status != kOSReturnSuccess)
        panic("TB710FU SourceStorage: class registration failed %x", status);

    OSDictionary *matching = IOService::serviceMatching("IOPlatformExpertDevice");
    IOService *provider = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    // Unlike waitForService, waitForMatchingService does not consume matching.
    if (matching) matching->release();
    if (!provider) panic("TB710FU SourceStorage: platform root unavailable");
    storageDriver = native_ufs_source_create();
    if (!storageDriver) panic("TB710FU SourceStorage: cannot allocate UFS service");
    const char *step = !storageDriver->init() ? "init" : !storageDriver->attach(provider) ? "attach" :
                       !storageDriver->start(provider) ? "start" : nullptr;
    // Failure after hardware acquisition must keep the driver's DMA allocation
    // alive. NativeUfs owns transactional queue/SMMU recovery, so never detach
    // or free it here after a partial start.
    if (step)
        panic("TB710FU SourceStorage: UFS %s failed (root step %u: %s)", step, native_ufs_root_step,
              native_ufs_root_failure ? native_ufs_root_failure : "no reason recorded");
    provider->release();
    printf("TB710FU SourceStorage: authenticated read-only UFS service started\n");
}
