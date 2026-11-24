// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB710FU macOS 27 OS services bootstrap: AppleKeyStoreState (aks=1 aks-sls=3)
// plus IOCryptoAcceleratorFamily/IOAESAccelerator ABI probe for pl18 SoftwareAES.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void tb710fu_aks_meta_init(void) __asm__("__GLOBAL__sub_I_NativeKeyStoreEvents.cpp");
extern "C" void tb710fu_aes_meta_init(void) __asm__("__GLOBAL__sub_I_NativeAES.cpp");
extern "C" IOService *native_keyevents_allocate(void);
extern "C" IOService *native_aes_allocate(void);
extern "C" volatile uint32_t native_aes_kat_status;
extern "C" volatile uint32_t native_aes_registered;
extern "C" volatile uint32_t native_aes_interface_status;

namespace {
uint32_t gServicesStarted = 0;
const OSMetaClass *gAesParent = nullptr;
}

extern "C" const OSMetaClass *native_tb710fu_aes_parent(void)
{
    if (!gAesParent) {
        panic("TB710FU Services: IOAESAccelerator parent not resolved before NativeAES metaclass init");
    }
    return gAesParent;
}

extern "C" void native_tb710fu_services_start(void)
{
    if (__atomic_exchange_n(&gServicesStarted, 1u, __ATOMIC_ACQ_REL)) {
        return;
    }

    /* Load IOCryptoAcceleratorFamily first so IOAESAccelerator metaclass is live. */
    OSReturn aesLoad = OSKextLoadKextWithIdentifier("com.apple.iokit.IOCryptoAcceleratorFamily");
    const OSSymbol *aesName = OSSymbol::withCString("IOAESAccelerator");
    const OSMetaClass *aesMeta = aesName ? OSMetaClass::getMetaClassWithName(aesName) : nullptr;
    if (aesName) {
        aesName->release();
    }
    uint32_t aesSize = aesMeta ? aesMeta->getClassSize() : 0u;
    printf("TB710FU Services: IOAESAccelerator load=%x meta=%p size=0x%x\n",
           aesLoad, aesMeta, aesSize);
    bool aesParentOk = (aesMeta != nullptr && aesSize == 0x120u);
    if (aesParentOk) {
        gAesParent = aesMeta;
    }

    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        printf("TB710FU Services: preModLoad failed\n");
        return;
    }
    tb710fu_aks_meta_init();
    if (aesParentOk) {
        tb710fu_aes_meta_init();
    }
    OSReturn status = OSMetaClass::postModLoad(handle);
    if (status != kOSReturnSuccess) {
        printf("TB710FU Services: postModLoad failed %x\n", status);
        return;
    }

    OSDictionary *matching = IOService::serviceMatching("IOResources");
    IOService *resources = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    if (matching) {
        matching->release();
    }
    if (!resources) {
        printf("TB710FU Services: IOResources unavailable\n");
        return;
    }

    if (IOService *aks = native_keyevents_allocate()) {
        const char *step = !aks->init() ? "init" : !aks->attach(resources) ? "attach" :
                           !aks->start(resources) ? "start" : nullptr;
        printf("TB710FU Services: AppleKeyStore %s\n", step ? step : "started");
    } else {
        printf("TB710FU Services: AppleKeyStore alloc failed\n");
    }

    if (aesParentOk) {
        if (IOService *aes = native_aes_allocate()) {
            const char *step = !aes->init() ? "init" : !aes->attach(resources) ? "attach" :
                               !aes->start(resources) ? "start" : nullptr;
            printf("TB710FU Services: SoftwareAES %s kat=%u reg=%u iface=%u\n",
                   step ? step : "started",
                   native_aes_kat_status, native_aes_registered, native_aes_interface_status);
        } else {
            printf("TB710FU Services: SoftwareAES alloc failed\n");
        }
    }

    resources->release();
}
