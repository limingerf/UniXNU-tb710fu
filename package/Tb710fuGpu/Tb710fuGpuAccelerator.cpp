// SPDX-License-Identifier: BSD-2-Clause-Patent
// The registry-visible accelerator for macOS Metal discovery (gpu-metal=1).
//
// IOGPU's +[IOGPUMetalDevice registerAcceleratorService:] looks the accelerator
// up with IOServiceMatching("IOAccelerator") and loads
// <MetalPluginName>.bundle from libsystem_sandbox's trusted bundle directories
// (/Library/GPUBundles first, /System/Library/Extensions second). This file
// supplies that service and nothing else: it maps no device memory, starts no
// engine and answers no user-client request. Its purpose is the first milestone
// the reference SM8650 project's stage A names - WindowServer's discovery must
// reach a Metal device source before any GPU work can be qualified - plus the
// evidence that it did: property reads, open attempts and the accelerator's
// registry identity are all counted.
//
// The base declaration and its 136-byte extent live in Tb710fuGpuABI.h. The
// parent metaclass is resolved before the module's metaclass window opens
// (tb710fu_gpu_probe), the class is installed inside that window
// (tb710fu_gpu_meta_init) and every object is made after postModLoad
// (tb710fu_gpu_start), because the parent does not exist before IOGraphicsFamily
// is in the kernel.
#include "Tb710fuGpu.h"
#include "Tb710fuGpuABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <libkern/OSKextLib.h>
#include <libkern/OSReturn.h>
#include <pexpert/pexpert.h>

#if defined(APPLE_KEXT_OVERRIDE)
#define TB710FU_OVERRIDE APPLE_KEXT_OVERRIDE
#else
#define TB710FU_OVERRIDE
#endif

/* The constructor clang emits for this translation unit. The build moves it out
 * of __mod_init_func so only preModLoad's explicit call runs it. */
extern "C" void tb710fu_gpu_accelerator_static_init(void)
    __asm__("__GLOBAL__sub_I_Tb710fuGpuAccelerator.cpp");

namespace {
const OSMetaClass *gBase = nullptr;
volatile uint32_t gMetalAsked = 0;
volatile uint32_t gMetaInstalled = 0;
volatile uint32_t gMetalPluginNameReads = 0;
}  // namespace

extern "C" volatile uint32_t tb710fu_gpu_stats[TB710FU_GPU_STAT_COUNT] = {};

class Tb710fuGpuAccelerator : public IOAccelerator {
    OSDeclareDefaultStructors(Tb710fuGpuAccelerator)
public:
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                           OSDictionary *properties, IOUserClient **handler) TB710FU_OVERRIDE;
    OSObject *copyProperty(const OSSymbol *key) const TB710FU_OVERRIDE;
    OSObject *copyProperty(const OSString *key) const TB710FU_OVERRIDE;
    OSObject *copyProperty(const char *key) const TB710FU_OVERRIDE;
};
OSDefineMetaClassAndStructors(Tb710fuGpuAccelerator, IOAccelerator)

/* Every open is refused for now: a request is evidence that a process found the
 * accelerator and asked for the GPU, and the answer says no work exists yet.
 * Nothing is printed here - newUserClient runs with preemption disabled while
 * IOKit holds the service lock, and a console write at that point has already
 * stopped this board's boot once (see the pl49 note in GopDisplayStart.cpp). */
IOReturn Tb710fuGpuAccelerator::newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                              OSDictionary *properties, IOUserClient **handler)
{
    (void)owningTask;
    (void)securityID;
    (void)properties;
    if (handler) {
        *handler = nullptr;
    }
    tb710fu_gpu_stats[TB710FU_GPU_OPENS]++;
    tb710fu_gpu_stats[TB710FU_GPU_OPEN_TYPE] = type;
    return kIOReturnUnsupported;
}

/* Diagnostic counting only: the value is the base class's own. The first read of
 * MetalPluginName is printed once, because that single line is the proof that
 * the discovery reached this service rather than timing out with no device. */
OSObject *Tb710fuGpuAccelerator::copyProperty(const OSSymbol *key) const
{
    const char *name = key ? key->getCStringNoCopy() : nullptr;
    tb710fu_gpu_stats[TB710FU_GPU_PROPERTY_READS]++;
    if (name && strcmp(name, "MetalPluginName") == 0 &&
        __atomic_exchange_n(&gMetalPluginNameReads, 1u, __ATOMIC_ACQ_REL) == 0) {
        printf("TB710FU GPU: discovery read MetalPluginName (accelerator reached)\n");
    }
    return IORegistryEntry::copyProperty(key);
}

OSObject *Tb710fuGpuAccelerator::copyProperty(const OSString *key) const
{
    const char *name = key ? key->getCStringNoCopy() : nullptr;
    tb710fu_gpu_stats[TB710FU_GPU_PROPERTY_READS]++;
    if (name && strcmp(name, "MetalPluginName") == 0 &&
        __atomic_exchange_n(&gMetalPluginNameReads, 1u, __ATOMIC_ACQ_REL) == 0) {
        printf("TB710FU GPU: discovery read MetalPluginName (accelerator reached)\n");
    }
    return IORegistryEntry::copyProperty(key);
}

OSObject *Tb710fuGpuAccelerator::copyProperty(const char *key) const
{
    tb710fu_gpu_stats[TB710FU_GPU_PROPERTY_READS]++;
    if (key && strcmp(key, "MetalPluginName") == 0 &&
        __atomic_exchange_n(&gMetalPluginNameReads, 1u, __ATOMIC_ACQ_REL) == 0) {
        printf("TB710FU GPU: discovery read MetalPluginName (accelerator reached)\n");
    }
    return IORegistryEntry::copyProperty(key);
}

static void setNumber(IOService *service, const char *key, uint64_t value)
{
    OSNumber *number = OSNumber::withNumber(value, 32);
    if (!number) {
        return;
    }
    service->setProperty(key, number);
    number->release();
}

extern "C" int tb710fu_gpu_probe(void)
{
    uint32_t metal = 0;
    gMetalAsked = PE_parse_boot_argn("gpu-metal", &metal, sizeof(metal)) && metal >= 1;
    if (!tb710fu_gpu_wanted()) {
        return 0;
    }
    /* Resolve the prelinked class first: OSKextLoadKextWithIdentifier on a class
     * that is already in the fileset blocks this boot long enough to lose the
     * display (the pl21 note in GopDisplayStart.cpp). */
    const OSSymbol *symbol = OSSymbol::withCString("IOAccelerator");
    const OSMetaClass *meta = symbol ? OSMetaClass::getMetaClassWithName(symbol) : nullptr;
    OSReturn loaded = kOSReturnSuccess;
    if (!meta) {
        printf("TB710FU GPU: prelinked IOAccelerator absent, loading com.apple.iokit.IOGraphicsFamily\n");
        loaded = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGraphicsFamily");
        meta = symbol ? OSMetaClass::getMetaClassWithName(symbol) : nullptr;
    }
    if (symbol) {
        symbol->release();
    }
    tb710fu_gpu_stats[TB710FU_GPU_BASE_SIZE] = meta ? meta->getClassSize() : 0u;
    if (!meta || meta->getClassSize() != 136u) {
        tb710fu_gpu_stats[TB710FU_GPU_STAGE] = 1;
        tb710fu_gpu_stats[TB710FU_GPU_STATUS] = loaded ? (uint32_t)loaded : (uint32_t)kIOReturnNotFound;
        printf("TB710FU GPU: IOAccelerator extent %u (load=%x), accelerator not published\n",
               meta ? meta->getClassSize() : 0u, (unsigned)loaded);
        return 0;
    }
    /* The class must still be an IOService: the discovery matches by class chain
     * and the sandbox verdicts key on the service's class name. */
    bool reachesIOService = false;
    for (const OSMetaClass *cur = meta; cur; cur = cur->getSuperClass()) {
        if (strcmp(cur->getClassName(), "IOService") == 0) {
            reachesIOService = true;
            break;
        }
    }
    if (!reachesIOService) {
        tb710fu_gpu_stats[TB710FU_GPU_STAGE] = 1;
        tb710fu_gpu_stats[TB710FU_GPU_STATUS] = (uint32_t)kIOReturnUnsupported;
        printf("TB710FU GPU: IOAccelerator is not an IOService subclass, accelerator not published\n");
        return 0;
    }
    gBase = meta;
    tb710fu_gpu_stats[TB710FU_GPU_STAGE] = 2;
    printf("TB710FU GPU: IOAccelerator size=0x%x, base resolved\n", meta->getClassSize());
    return 1;
}

extern "C" void tb710fu_gpu_meta_init(void)
{
    if (!gBase || !gMetalAsked || __atomic_exchange_n(&gMetaInstalled, 1u, __ATOMIC_ACQ_REL) != 0) {
        return;
    }
    /* The constructor this alias names is the one clang emitted for this
     * translation unit; build-tb710fu-gpu.py moves it out of __mod_init_func
     * (`__mod_deferred`) so preModLoad runs it here, after the parent exists. */
    tb710fu_gpu_accelerator_static_init();
    printf("TB710FU GPU: Tb710fuGpuAccelerator metaclass installed\n");
}

extern "C" void tb710fu_gpu_start(void)
{
    if (!tb710fu_gpu_wanted()) {
        return;
    }
    /* Census first: it is read-only and its result is published on the
     * accelerator, so a boot that fails before Metal gets that far still shows
     * what the payload carried. */
    tb710fu_gpu_power_survey();
    Tb710fuGpuCensus census = {};
    tb710fu_gpu_census_read(&census);
    tb710fu_gpu_power_publish(&census);

    uint32_t metal = 0;
    PE_parse_boot_argn("gpu-metal", &metal, sizeof(metal));
    if (metal != 1) {
        printf("TB710FU GPU: gpu-metal=%u, accelerator not published\n", metal);
        return;
    }
    if (!gBase || !gMetaInstalled) {
        tb710fu_gpu_stats[TB710FU_GPU_STATUS] = (uint32_t)kIOReturnNotReady;
        printf("TB710FU GPU: accelerator metaclass unavailable, accelerator not published\n");
        return;
    }
    IOService *accelerator = new Tb710fuGpuAccelerator;
    if (!accelerator) {
        tb710fu_gpu_stats[TB710FU_GPU_STATUS] = (uint32_t)kIOReturnNoMemory;
        return;
    }
    if (!accelerator->init()) {
        accelerator->release();
        tb710fu_gpu_stats[TB710FU_GPU_STATUS] = (uint32_t)kIOReturnNoResources;
        printf("TB710FU GPU: accelerator init refused\n");
        return;
    }
    tb710fu_gpu_stats[TB710FU_GPU_STAGE] = 3;
    /* The two keys the original discovery helper reads. The plugin name is this
     * port's own bundle; no Apple device identity (gpu,t8011 and the like) is
     * claimed, so an Apple accelerator kext can never match this service. */
    accelerator->setProperty("MetalPluginName", "NativeAdrenoMetal");
    accelerator->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
    /* The original sandbox profiles name no class of this driver; the grant
     * property is read by patches/xnu/zzw-native-adreno-sandbox-grant.patch,
     * which this macOS series does not apply. It is published anyway because
     * WindowServer itself is not a sandboxed client, and a series that adds the
     * grant later must find the property already correct. */
    accelerator->setProperty("NativeSandboxGrantProcess", "*");
    setNumber(accelerator, "Tb710fuGpuMilestone", 1);
    setNumber(accelerator, "Tb710fuGpuPowerLevel", tb710fu_gpu_stats[TB710FU_GPU_POWER_LEVEL]);
    setNumber(accelerator, "Tb710fuGpuBundle", census.bundle);
    setNumber(accelerator, "Tb710fuGpuFirmware", census.firmware);
    OSNumber *chipid = OSNumber::withNumber(0x43051401ULL, 32);
    if (chipid) {
        accelerator->setProperty("Tb710fuGpuChipId", chipid);
        chipid->release();
    }
    accelerator->setName("Tb710fuGpuAccelerator");
    IOService *platform = IOService::getPlatform();
    if (!platform) {
        accelerator->release();
        tb710fu_gpu_stats[TB710FU_GPU_STATUS] = (uint32_t)kIOReturnNotAttached;
        printf("TB710FU GPU: no platform expert, accelerator not published\n");
        return;
    }
    if (!accelerator->attach(platform)) {
        accelerator->release();
        tb710fu_gpu_stats[TB710FU_GPU_STATUS] = (uint32_t)kIOReturnNotAttached;
        printf("TB710FU GPU: accelerator attach refused\n");
        return;
    }
    tb710fu_gpu_stats[TB710FU_GPU_STAGE] = 4;
    /* registerService is the step WindowServer's 30-second wait observes: from
     * here IOServiceMatching("IOAccelerator") can find the service. */
    accelerator->registerService();
    tb710fu_gpu_stats[TB710FU_GPU_REGISTERED] = 1;
    tb710fu_gpu_stats[TB710FU_GPU_STAGE] = 5;
    printf("TB710FU GPU: accelerator registered plugin=NativeAdrenoMetal class=NativeAdrenoMetalDevice "
           "bundle=%u firmware=%u (%u)\n", census.bundle, census.firmware, census.announced);
    accelerator->release();
}
