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
extern "C" void tb710fu_battery_meta_init(void) __asm__("__GLOBAL__sub_I_Tb710fuBattery.cpp") __attribute__((weak_import));
extern "C" void tb710fu_battery_start(void) __attribute__((weak_import));
extern "C" void tb710fu_ethernet_probe(void) __attribute__((weak_import));
#if defined(XNU_TB710FU_ADSP) && XNU_TB710FU_ADSP
extern "C" void tb710fu_adsp_probe(void) __attribute__((weak_import));
#endif
extern "C" IOService *native_keyevents_allocate(void);
extern "C" IOService *native_aes_allocate(void);
extern "C" void native_tb710fu_gop_display_start(void);
/* Answered by CdcXnu.c: 1 when a CDC worker was queued for this boot and will
 * start the display itself once its platform is live. Weak, so a build without
 * that archive keeps the unconditional start below. */
extern "C" int tb710fu_cdc_owns_display(void) __attribute__((weak_import));
/* GPU/Metal milestone 1 (package/Tb710fuGpu). Weak, so a kernel built without
 * that archive keeps the same boot: probe resolves IOGraphicsFamily's
 * IOAccelerator before the metaclass window, meta_init installs the two
 * metaclasses inside it, and start publishes the accelerator only when
 * gpu-metal=1 (it reads the argument itself). */
extern "C" int tb710fu_gpu_probe(void) __attribute__((weak_import));
extern "C" void tb710fu_gpu_meta_init(void) __attribute__((weak_import));
extern "C" void tb710fu_gpu_power_meta_init(void) __attribute__((weak_import));
extern "C" void tb710fu_gpu_start(void) __attribute__((weak_import));
/* Peripheral bring-up archives (all boot-arg gated, default OFF). */
extern "C" void tb710fu_wifi_probe(void) __attribute__((weak_import));
extern "C" void tb710fu_audio_meta_init(void) __asm__("__GLOBAL__sub_I_Tb710fuAudioService.cpp") __attribute__((weak_import));
extern "C" void tb710fu_audio_start(void) __attribute__((weak_import));
extern "C" int tb710fu_input_start(void) __attribute__((weak_import));
extern "C" void tb710fu_bluetooth_meta_init(void) __asm__("__GLOBAL__sub_I_Tb710fuBluetooth.cpp") __attribute__((weak_import));
extern "C" void tb710fu_bluetooth_probe(void) __attribute__((weak_import));
extern "C" void tb710fu_platform_meta_all(void) __attribute__((weak_import));
extern "C" void tb710fu_platform_services_start(void) __attribute__((weak_import));
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

    /* GPU: the accelerator's parent is a prelinked IOGraphicsFamily class, so it
     * is resolved first, while the kernel still blocks on this boot thread; its
     * metaclass is then installed inside the window with every other
     * constructor of the module. Both calls read gpu-metal/gpu-power and stay
     * silent when neither is asked. */
    if (tb710fu_gpu_probe) {
        tb710fu_gpu_probe();
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
    if (tb710fu_gpu_meta_init) {
        tb710fu_gpu_meta_init();
    }
    if (tb710fu_gpu_power_meta_init) {
        tb710fu_gpu_power_meta_init();
    }
    /* The battery's IOKit metaclass lives in its own archive; installing it before postModLoad keeps every
     * constructor of this module in the same registration pass. Weak, so a kernel without that archive still
     * loads. */
    if (tb710fu_battery_meta_init) {
        tb710fu_battery_meta_init();
    }
    if (tb710fu_audio_meta_init) {
        tb710fu_audio_meta_init();
    }
    if (tb710fu_bluetooth_meta_init) {
        tb710fu_bluetooth_meta_init();
    }
    if (tb710fu_platform_meta_all) {
        tb710fu_platform_meta_all();
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

#if defined(XNU_TB710FU_ADSP) && XNU_TB710FU_ADSP
    /* ADSP: the thread reads the adsp=1/2 boot argument itself, surveys (1) or starts the DSP and brings up GLINK (2).
     * It runs ahead of the battery so the battery's channel can be registered before the DSP opens it. */
    if (tb710fu_adsp_probe) {
        tb710fu_adsp_probe();
    }
#endif

    /* Battery: the service checks the battery=1 boot argument itself, prints its ADT survey, and stays unregistered
     * when the PMIC GLINK transport (the ADSP archive) is not part of this kernel. */
    if (tb710fu_battery_start) {
        tb710fu_battery_start();
    }

    /* GPU/Metal: reads gpu-power/gpu-metal itself. With gpu-power it surveys the
     * ADT and the payload addon and, at level 2, publishes the power-domain node;
     * with gpu-metal=1 it publishes the IOAccelerator service that IOGPU's
     * discovery matches. Without either argument it returns before touching the
     * registry, so the default boot is byte-for-byte the baseline one. */
    if (tb710fu_gpu_start) {
        tb710fu_gpu_start();
    }

    /* Ethernet (en0): macOS IOTimeSyncFamily derives the TimeSync clock ID from an
     * IOEthernetInterface MAC; without en0, clocksyncd never publishes a clock and
     * WindowServer waits indefinitely on clocksyncd. Runs on its own worker thread. */
    if (tb710fu_ethernet_probe) {
        tb710fu_ethernet_probe();
    }

    /* Peripheral drivers: each reads its own boot-arg and stays idle when unset. */
    if (tb710fu_platform_services_start) {
        tb710fu_platform_services_start();
    }
    if (tb710fu_input_start) {
        tb710fu_input_start();
    }
    if (tb710fu_audio_start) {
        tb710fu_audio_start();
    }
    if (tb710fu_wifi_probe) {
        tb710fu_wifi_probe();
    }
    if (tb710fu_bluetooth_probe) {
        tb710fu_bluetooth_probe();
    }

    /* Display bring-up used to run here unconditionally, about 1.5 s before the
     * CDC worker reached platform-up: native-source-storage defers that worker by
     * 1500 ms, and this function runs right after bsd_init's storage hook. A panic
     * inside display bring-up then had no COM5 to report to - the panic loop
     * returns immediately when the controller was never started - so the boot
     * looked like a silent hang. The CDC worker now owns the call and runs it on
     * its own display thread only after the DWC3 poll loop answers EP0, which is
     * still far ahead of WindowServer. Nothing on a boot thread may start it. */
    if (tb710fu_cdc_owns_display == NULL || tb710fu_cdc_owns_display() == 0) {
        native_tb710fu_gop_display_start();
    } else {
        printf("TB710FU Services: GopDisplay deferred to the CDC worker\n");
    }

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
