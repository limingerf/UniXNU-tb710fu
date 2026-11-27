// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tb710fuThermal.cpp - the SM8650 TSENS monitor (boot argument thermal=0..1).
//
// Windows, from sm8650.dtsi only:
//   tsens0 tm 0x0c228000 srot 0x0c222000 #qcom,sensors = <15>
//   tsens1 tm 0x0c229000 srot 0x0c223000 #qcom,sensors = <16>
//   tsens2 tm 0x0c22a000 srot 0x0c224000 #qcom,sensors = <13>
// The register offsets inside those windows and the status-word layout are
// tsens-v2.c/tsens.c's and are documented in Tb710fuThermalCore.h, together
// with the board's zone-to-sensor table.
//
// This is a monitor, not a thermal policy: it reads the sensors at start,
// publishes the readings on the service and prints one row per controller, and
// does nothing else. There is no trip handling, no throttling and no SMC
// thermal-pressure service - those need the trip table and Apple's
// AppleSMCSensorDispatcher ABI, which is a later rung.
//
// The one write is the SROT control word, and only when the block is found
// disabled: tsens.c enables it on probe in exactly this shape, and without it
// every status word reads invalid, so the monitor could never work. Nothing is
// written when the block is already on, which is the normal case on a board
// whose boot loader ran a thermal-aware firmware.
//
// thermal=0 leaves the six windows unmapped. thermal=1 maps them, enables what
// is off, samples once and publishes. setProperties{"sample"} re-samples,
// which is how a user-space agent can pull a fresh reading without a periodic
// timer in the kernel.
//
// Only a kernel build has the IOKit ABI; a host build gets an empty unit.
#ifdef KERNEL

#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>

extern "C" {
#include "Tb710fuPlatformStage.h"
#include "Tb710fuThermalCore.h"
}

namespace {

/* sm8650.dtsi thermal-sensor@...: one 0x1000 window each. */
constexpr uint64_t kTmBase[TB710FU_TSENS_CONTROLLERS] = {
    TB710FU_TSENS0_TM_PHYS, TB710FU_TSENS1_TM_PHYS, TB710FU_TSENS2_TM_PHYS
};
constexpr uint64_t kSrotBase[TB710FU_TSENS_CONTROLLERS] = {
    TB710FU_TSENS0_SROT_PHYS, TB710FU_TSENS1_SROT_PHYS,
    TB710FU_TSENS2_SROT_PHYS
};

constexpr unsigned kStatCount = 6u;
enum { kStatStage = 0, kStatReads, kStatInvalid, kStatEnabled, kStatHottest,
       kStatLastError };
int32_t gStats[kStatCount];

struct Window {
    IOMemoryMap *map;
    volatile uint32_t *base;

    bool open(uint64_t address, uint64_t size)
    {
        if (base != nullptr) {
            return true;
        }
        IODeviceMemory *memory = IODeviceMemory::withRange(address, size);
        map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache)
                     : nullptr;
        if (memory) {
            memory->release();
        }
        base = map ? reinterpret_cast<volatile uint32_t *>(
                         map->getVirtualAddress())
                   : nullptr;
        return base != nullptr && ((uintptr_t)base >> 48) == 0xffffull;
    }
};

Window gTm[TB710FU_TSENS_CONTROLLERS];
Window gSrot[TB710FU_TSENS_CONTROLLERS];
int32_t gReadings[TB710FU_TSENS_CONTROLLERS][TB710FU_TSENS_SENSORS];

/* Aligned 32-bit accesses only: an unaligned Device-GRE store faults with
 * ESR 0x96000061 on SM8650, so no memcpy/bzero/memset touches a mapping. */
uint32_t rd(volatile uint32_t *base, uint32_t reg)
{
    return base[reg / 4u];
}

void wr(volatile uint32_t *base, uint32_t reg, uint32_t value)
{
    base[reg / 4u] = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

void fail(unsigned step, int error)
{
    gStats[kStatStage] = -(int32_t)step;
    gStats[kStatLastError] = error;
}

}  // namespace

class Tb710fuThermal final : public IOService {
    OSDeclareDefaultStructors(Tb710fuThermal)

public:
    bool start(IOService *provider) override;
    IOReturn setProperties(OSObject *properties) override;

private:
    bool sample();
    void publishReadings();
};

bool Tb710fuThermal::sample()
{
    uint32_t control[TB710FU_TSENS_CONTROLLERS] = {};
    uint32_t version[TB710FU_TSENS_CONTROLLERS] = {};

    for (unsigned controller = 0; controller < TB710FU_TSENS_CONTROLLERS;
         ++controller) {
        if (!gTm[controller].open(kTmBase[controller],
                                  TB710FU_TSENS_WINDOW) ||
            !gSrot[controller].open(kSrotBase[controller],
                                    TB710FU_TSENS_WINDOW)) {
            fail(1u, -12);
            printf("TB710FU Thermal: tsens%u window map failed\n", controller);
            return false;
        }
        version[controller] = rd(gSrot[controller].base,
                                TB710FU_TSENS_SROT_VERSION);
        control[controller] = rd(gSrot[controller].base,
                                 TB710FU_TSENS_SROT_CTRL);
    }
    gStats[kStatStage] = 1;

    for (unsigned controller = 0; controller < TB710FU_TSENS_CONTROLLERS;
         ++controller) {
        const unsigned count = tb710fu_thermal_sensor_count[controller];
        int enabled = 1;

        for (unsigned sensor = 0; sensor < count; ++sensor) {
            if (!tb710fu_thermal_enabled(control[controller], sensor)) {
                enabled = 0;
                break;
            }
        }
        if (!enabled) {
            /* The documented enable (tsens.c): block bit 0 plus one bit per
             * sensor, then read back. This is the only thermal write. */
            const uint32_t wanted = tb710fu_thermal_control_for(count);

            wr(gSrot[controller].base, TB710FU_TSENS_SROT_CTRL, wanted);
            control[controller] = rd(gSrot[controller].base,
                                     TB710FU_TSENS_SROT_CTRL);
            gStats[kStatEnabled] = gStats[kStatEnabled] + 1;
            printf("TB710FU Thermal: tsens%u was disabled, control %08x -> "
                   "%08x\n",
                   controller, wanted,
                   (unsigned)control[controller]);
        }
        printf("TB710FU Thermal: tsens%u version %08x control %08x sensors %u\n",
               controller, (unsigned)version[controller],
               (unsigned)control[controller], count);
    }

    for (unsigned controller = 0; controller < TB710FU_TSENS_CONTROLLERS;
         ++controller) {
        const unsigned count = tb710fu_thermal_sensor_count[controller];

        printf("TB710FU Thermal: tsens%u mc:", controller);
        for (unsigned sensor = 0; sensor < TB710FU_TSENS_SENSORS; ++sensor) {
            uint32_t status[TB710FU_TSENS_READ_TRIES] = {};
            int32_t mc = TB710FU_TSENS_NONE;

            if (sensor >= count) {
                gReadings[controller][sensor] = TB710FU_TSENS_NONE;
                printf(" --");
                continue;
            }
            for (unsigned attempt = 0; attempt < TB710FU_TSENS_READ_TRIES;
                 ++attempt) {
                status[attempt] = rd(gTm[controller].base,
                                     TB710FU_TSENS_TM_STATUS +
                                         4u * sensor);
                gStats[kStatReads] = gStats[kStatReads] + 1;
            }
            if (!tb710fu_thermal_pick(status, TB710FU_TSENS_READ_TRIES, &mc) ||
                !tb710fu_thermal_plausible(mc)) {
                mc = TB710FU_TSENS_NONE;
                gStats[kStatInvalid] = gStats[kStatInvalid] + 1;
            }
            gReadings[controller][sensor] = mc;
            if (mc == TB710FU_TSENS_NONE) {
                printf(" --");
            } else {
                printf(" %d", (int)mc);
            }
        }
        printf("\n");
    }

    int32_t groups[TB710FU_THERMAL_GROUPS];
    const int32_t hottest = tb710fu_thermal_hottest(gReadings);

    tb710fu_thermal_groups(gReadings, groups);
    gStats[kStatHottest] = hottest;
    printf("TB710FU Thermal: hottest %d mc;", (int)hottest);
    for (unsigned group = 0; group < TB710FU_THERMAL_GROUPS; ++group) {
        if (groups[group] != TB710FU_TSENS_NONE) {
            printf(" %s %d",
                   tb710fu_thermal_group_name[group], (int)groups[group]);
        }
    }
    printf("\n");
    return true;
}

void Tb710fuThermal::publishReadings()
{
    if (OSData *data = OSData::withBytes(gReadings, sizeof(gReadings))) {
        setProperty("temperatures-mc", data);
        data->release();
    }
    int32_t groups[TB710FU_THERMAL_GROUPS];

    tb710fu_thermal_groups(gReadings, groups);
    OSDictionary *dictionary = OSDictionary::withCapacity(TB710FU_THERMAL_GROUPS);
    if (dictionary) {
        for (unsigned group = 0; group < TB710FU_THERMAL_GROUPS; ++group) {
            if (groups[group] == TB710FU_TSENS_NONE) {
                continue;
            }
            if (OSNumber *number =
                    OSNumber::withNumber((uint64_t)(uint32_t)groups[group], 32)) {
                dictionary->setObject(tb710fu_thermal_group_name[group], number);
                number->release();
            }
        }
        setProperty("groups-mc", dictionary);
        dictionary->release();
    }
    if (OSNumber *hottest =
            OSNumber::withNumber((uint64_t)(uint32_t)tb710fu_thermal_hottest(gReadings),
                                 32)) {
        setProperty("hottest-mc", hottest);
        hottest->release();
    }
    if (OSData *data = OSData::withBytes(gStats, sizeof(gStats))) {
        setProperty("tb710fu-thermal-stats", data);
        data->release();
    }
}

bool Tb710fuThermal::start(IOService *provider)
{
    if (!IOService::start(provider)) {
        return false;
    }
    if (!sample()) {
        return false;
    }
    publishReadings();
    registerService();
    return true;
}

IOReturn Tb710fuThermal::setProperties(OSObject *properties)
{
    OSDictionary *dictionary = OSDynamicCast(OSDictionary, properties);

    if (!dictionary) {
        return kIOReturnBadArgument;
    }
    /* A user-space agent asks for a fresh reading by setting "sample". The
     * call runs on that agent's thread, and every read is a single 32-bit
     * load, so no timer is needed in the kernel. */
    if (dictionary->getObject("sample")) {
        if (!sample()) {
            return kIOReturnNoDevice;
        }
        publishReadings();
    }
    return kIOReturnSuccess;
}

OSDefineMetaClassAndStructors(Tb710fuThermal, IOService)

/* thermal=1: create and register the monitor on the platform expert. */
extern "C" int tb710fu_thermal_start(void)
{
    unsigned stage = 0;

    /* The ladder's decision repeated at the driver's own entry: a direct
     * caller cannot map a TSENS window unless the boot asked for thermal=1. */
    if (!tb710fu_stage_allow(PE_boot_args(), TB710FU_BOOTARG_THERMAL,
                             TB710FU_STAGE_THERMAL_MAX, 1u, &stage)) {
        fail(1u, -1);
        printf("TB710FU Thermal: thermal=%u is below 1, monitor refused\n",
               stage);
        return -4;
    }
    /* The class's static constructor runs from tb710fu_platform_meta_all(); if
     * nobody called it, the metaclass object has not been constructed and an
     * instance would hand IOService uninitialised memory. */
    const OSSymbol *name = OSSymbol::withCString("Tb710fuThermal");
    const OSMetaClass *meta =
        name ? OSMetaClass::getMetaClassWithName(name) : nullptr;

    OSSafeReleaseNULL(name);
    if (meta == nullptr) {
        fail(1u, -19);
        printf("TB710FU Thermal: metaclass not registered "
               "(meta_init not called), monitor refused\n");
        return -4;
    }
    IOService *provider = IOService::getPlatform();

    if (!provider) {
        fail(1u, -19);
        printf("TB710FU Thermal: platform expert absent\n");
        return -1;
    }
    Tb710fuThermal *service = new Tb710fuThermal;

    if (!service) {
        fail(1u, -12);
        return -2;
    }
    if (!service->init() || !service->attach(provider) ||
        !service->start(provider)) {
        printf("TB710FU Thermal: service did not start\n");
        service->detach(provider);
        service->release();
        return -3;
    }
    /* Registered and never released: it is a property holder for user space. */
    return 0;
}

#endif /* KERNEL */
