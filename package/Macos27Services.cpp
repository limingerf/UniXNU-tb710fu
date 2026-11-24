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
extern "C" IOService *native_keyevents_allocate(void);

namespace {
uint32_t gServicesStarted = 0;
}

extern "C" void native_tb710fu_services_start(void)
{
    if (__atomic_exchange_n(&gServicesStarted, 1u, __ATOMIC_ACQ_REL)) {
        return;
    }

    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        printf("TB710FU Services: preModLoad failed\n");
        return;
    }
    tb710fu_aks_meta_init();
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

    /* Probe IOAESAccelerator metaclass size for pl18 SoftwareAES binding audit. */
    OSReturn aesLoad = OSKextLoadKextWithIdentifier("com.apple.iokit.IOCryptoAcceleratorFamily");
    const OSSymbol *aesName = OSSymbol::withCString("IOAESAccelerator");
    const OSMetaClass *aesMeta = aesName ? OSMetaClass::getMetaClassWithName(aesName) : nullptr;
    if (aesName) {
        aesName->release();
    }
    printf("TB710FU Services: IOAESAccelerator load=%x meta=%p size=0x%x\n",
           aesLoad, aesMeta, aesMeta ? aesMeta->getClassSize() : 0u);

    resources->release();
}
