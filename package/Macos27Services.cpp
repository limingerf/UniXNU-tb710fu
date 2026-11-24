// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB710FU macOS 27 OS services bootstrap: AppleKeyStoreState (aks=1 aks-sls=3)
// plus IOCryptoAcceleratorFamily/IOAESAccelerator ABI probe for pl18 SoftwareAES.
#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOService.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void tb710fu_aks_meta_init(void) __asm__("__GLOBAL__sub_I_NativeKeyStoreEvents.cpp");
extern "C" void tb710fu_aes_meta_init(void) __asm__("__GLOBAL__sub_I_NativeAES.cpp");
extern "C" IOService *native_keyevents_allocate(void);
extern "C" IOService *native_aes_allocate(void);
extern "C" void native_tb710fu_gop_display_start(void);
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

    native_tb710fu_gop_display_start();

    resources->release();
}

/* ---------------------------------------------------------------------------
 * COM5 debug shell: ioreg <text>.
 *
 * Lives here because the registry plane is an IOKit symbol and this file is the
 * one C++ translation unit in the boot set that already links IOKit. CdcXnu.c
 * declares it weak, so a build without this archive answers "unavailable"
 * instead of failing to link.
 *
 * Recursive iteration over gIOServicePlane, one line per matching service, in
 * the same shape ioreg(8) prints: name, class, busy count, registration state.
 * The output is deliberately capped: the ring that carries it to COM5 holds
 * 64 KiB, and an unbounded dump would evict the transcripts around it.
 * ------------------------------------------------------------------------- */
namespace {

bool ioreg_contains(const char *text, const char *needle)
{
    if (!text || !needle || !needle[0]) {
        return false;
    }
    for (const char *start = text; *start; ++start) {
        const char *t = start;
        const char *n = needle;
        while (*t && *n && *t == *n) {
            ++t;
            ++n;
        }
        if (!*n) {
            return true;
        }
    }
    return false;
}

}

extern "C" void tb710fu_ioreg_dump(const char *filter)
{
    const unsigned limit = 64u;
    unsigned visited = 0u, shown = 0u, skipped = 0u;
    auto iterator = IORegistryIterator::iterateOver(gIOServicePlane,
                                                   kIORegistryIterateRecursively);
    if (!iterator) {
        printf("TB710FU IOREG: iterator unavailable\n");
        return;
    }
    printf("TB710FU IOREG: begin plane=IOService filter=%s\n",
           filter && filter[0] ? filter : "*");
    while (IORegistryEntry *entry = iterator->getNextObject()) {
        ++visited;
        const char *name = entry->getName();
        const OSMetaClass *meta = entry->getMetaClass();
        const char *className = meta ? meta->getClassName() : "?";
        if (filter && filter[0] && !ioreg_contains(name, filter) &&
            !ioreg_contains(className, filter)) {
            continue;
        }
        if (shown >= limit) {
            ++skipped;
            continue;
        }
        IOService *service = OSDynamicCast(IOService, entry);
        const IOOptionBits state = service ? service->getState() : 0u;
        printf("TB710FU IOREG: %s class=%s busy=%u registered=%u state=%x\n",
               name ? name : "?", className,
               service ? (unsigned)service->getBusyState() : 0u,
               (unsigned)((state & kIOServiceRegisteredState) != 0), (unsigned)state);
        ++shown;
    }
    printf("TB710FU IOREG: end visited=%u shown=%u skipped=%u\n", visited, shown, skipped);
#if !defined(IOKIT_ENABLE_SHARED_PTR)
    /* Without shared pointers OSPtr is a raw pointer and the created iterator is
     * owned by the caller; with them the local wrapper releases it. */
    iterator->release();
#endif
}
