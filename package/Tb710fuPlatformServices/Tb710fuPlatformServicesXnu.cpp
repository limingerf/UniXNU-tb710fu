// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tb710fuPlatformServicesXnu.cpp - the XNU/IOKit edge of the TB710FU platform
// services: SPMI v7, the PMK8550 wall clock, and the restart/shutdown/EDL path.
//
// Only a kernel build compiles this: XNU's build defines KERNEL for kernel
// sources, and a host build gets an empty translation unit, so the MMIO core
// keeps building and testing on Linux/WSL.
//
// What it wires, and why:
//
//   * ml_io_map() maps the four spmi@c400000 windows once and hands them to
//     tb710fu_spmi_mmio_init(), which bounds-checks every access against the
//     devicetree sizes (sm8650.dtsi: core 0x0c400000/0x3000, cnfg
//     0x0c42d000/0x4000, obsrvr 0x0c440000/0x80000, chnls 0x0c500000/0x400000;
//     qcom,ee = <0>, qcom,channel = <0>, qcom,bus-id = <0>). The core then runs
//     the same code the host model exercises.
//
//   * The wall clock is the PMK8550 RTC at SID 0 / PPID 0x61 (pmk8550.dtsi:
//     rtc@6100, "qcom,pmk8350-rtc", alarm at 0x6200), read through
//     rtc-pm8xxx.c's pmk8350 register layout and reported through
//     IORTCController::getRealTimeClock()/setRealTimeClock() as this port's
//     eight-byte blob (Tb710fuRtcTime.h). IORTC::getGMTTimeOfDay()/
//     setGMTTimeOfDay()/getUTCTimeOfDay() sit on top, and
//     tb710fu_platform_clock_start() publishes the service under the "IORTC"
//     resource that IOKitInitializeTime waits up to 30 s for before it
//     initialises the calendar.
//
//   * PE_halt_restart (IOKit/IOPlatformExpert.h) is the platform's shutdown and
//     restart entry. kPEHaltCPU powers off and kPERestartCPU restarts
//     (osfmk/i386/acpi.c documents the convention); both map onto PSCI
//     SYSTEM_OFF/SYSTEM_RESET through SMC #0, the method the board's psci node
//     selects. recovery and bootloader also program the SDAM2 reboot-reason
//     cell first (pmk8550.dtsi reboot-reason@48, bits <1 7>: recovery 0x01,
//     bootloader 0x02). EDL does not touch the cell - it has no EDL mode - and
//     uses the verified PSCI vendor reset type instead (Tb710fuPmk8550Reset.h).
//
//   * armdt_platform_clock_register() is UniXNU's ArmDTPlatform hook: a board
//     clock driver registers its read and set functions there, and
//     ArmDTPlatform::getGMTTimeOfDay() then feeds PEGetUTCTimeOfDay()/
//     clock_initialize_calendar() without patching ArmDTPlatform.cpp. The
//     symbol is weak here: a kernel whose ArmDTPlatform does not carry the hook
//     still links, logs that the hook is absent, and keeps the IORTC resource.
//
// Staged bring-up (Tb710fuPlatformStage.h): platform-svc=0 leaves every window
// unmapped and PE_halt_restart at the platform's own value; 1 maps, probes and
// prints the clock and the reboot cell; 2 also installs the hooks and publishes
// IORTC. The SDAM2 write path keeps its own opt-in (tb710fu-rtc-store=1).
#ifdef KERNEL

#include <stdint.h>

#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern "C" {
#include "Tb710fuPlatformStage.h"
#include "Tb710fuPmk8550Reset.h"
#include "Tb710fuPmk8550Rtc.h"
#include "Tb710fuRtcTime.h"
#include "Tb710fuSpmiArbiter.h"
#include "Tb710fuSpmiMmio.h"
}

#if __has_include(<IOKit/rtc/IORTCController.h>)
#include <IOKit/rtc/IORTCController.h>
#define TB710FU_HAS_IORTC 1
#else
/* A kernel build without the IORTC ABI keeps the clock hook and the
 * diagnostic service below, and says so once instead of failing to build. */
#define TB710FU_HAS_IORTC 0
#endif

/* UniXNU's ArmDTPlatform hook (see the file comment). Weak: absent is a
 * supported build. */
extern "C" void armdt_platform_clock_register(int (*read)(uint64_t *seconds),
                                             void (*set)(uint64_t seconds))
    __attribute__((weak_import));

/* The two other drivers of this package (their own translation units). */
extern "C" void tb710fu_backlight_resolve_parent(void);
extern "C" int tb710fu_backlight_probe(unsigned *dev_id, unsigned *dev_ctl,
                                       unsigned *read_level);
extern "C" int tb710fu_backlight_start(void);
extern "C" int tb710fu_thermal_start(void);
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" int tb710fu_platform_power_action(int action);

namespace {

struct tb710fu_spmi_mmio gSpmi;
struct tb710fu_pmk8550_rtc gRtc;
struct tb710fu_psci_caps gPsci;
bool gClockReady;
bool gSmcReady;

/* The SMC register convention is Linux arm64's arm_smccc_smc shape: x0 holds
 * the function, x1..x3 the arguments, x0 returns the result. The "dsb sy"
 * publishes every preceding device store (in particular the SDAM2 reboot
 * reason) before the secure monitor takes the CPU down. */
int64_t smcInvoke(void *context, uint64_t function, uint64_t arg0,
                  uint64_t arg1, uint64_t arg2)
{
    register uint64_t x0 __asm__("x0") = function;
    register uint64_t x1 __asm__("x1") = arg0;
    register uint64_t x2 __asm__("x2") = arg1;
    register uint64_t x3 __asm__("x3") = arg2;

    __asm__ volatile("dsb sy; smc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     :
                     : "memory");
    (void)context;
    return (int64_t)x0;
}

const struct tb710fu_smc_io gSmc = {nullptr, smcInvoke};

uint64_t fineNanoseconds(void *context)
{
    uint64_t nanoseconds = 0;

    (void)context;
    absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
    return nanoseconds;
}

const struct tb710fu_rtc_fine_source gFineSource = {nullptr, fineNanoseconds};

/* The boot arguments, parsed by the host-tested parser rather than by
 * PE_parse_boot_argn(), so the ladder's clamping is the same code the tests
 * exercise. An argument that does not parse leaves the service off. */
unsigned readStage(const char *key, unsigned maximum)
{
    unsigned requested = 0;
    int clamped = 0;
    const char *arguments = PE_boot_args();
    const int found =
        arguments ? tb710fu_bootarg_value(arguments, key, &requested) : 0;

    if (found < 0) {
        printf("TB710FU Platform: %s does not parse, staying off\n", key);
        return 0;
    }
    if (found == 0) {
        return 0;
    }
    const unsigned level = tb710fu_stage_clamp(requested, maximum, &clamped);

    if (clamped) {
        printf("TB710FU Platform: %s=%u is above %u, using %u\n", key,
               requested, maximum, level);
    } else {
        printf("TB710FU Platform: %s=%u\n", key, level);
    }
    return level;
}

bool bootFlag(const char *key)
{
    unsigned value = 0;
    const char *arguments = PE_boot_args();

    return arguments && tb710fu_bootarg_value(arguments, key, &value) == 1 &&
        value != 0u;
}

struct tb710fu_pmk8550_rtc_config rtcConfig()
{
    struct tb710fu_pmk8550_rtc_config config;

    tb710fu_rtc_config_default(&config, TB710FU_SPMI_EE);
    /* The SDAM2 0xbc offset cell is not declared by the TB710FU devicetree, so
     * storing the RTC offset stays off unless the boot args opt in. Reading
     * the clock never needs it. */
    config.allow_store = bootFlag("tb710fu-rtc-store");
    return config;
}

/* Map every window with the devicetree size and probe the arbiter. A zero
 * ml_io_map() result is a failure, not a valid address. */
int hwInit()
{
    if (gClockReady) {
        return TB710FU_RTC_OK;
    }
    volatile uint32_t *core = (volatile uint32_t *)ml_io_map(
        TB710FU_SPMI_CORE_PHYS, TB710FU_SPMI_CORE_BYTES);
    volatile uint32_t *configuration = (volatile uint32_t *)ml_io_map(
        TB710FU_SPMI_CONFIG_PHYS, TB710FU_SPMI_CONFIG_BYTES);
    volatile uint32_t *observer = (volatile uint32_t *)ml_io_map(
        TB710FU_SPMI_OBSERVER_PHYS, TB710FU_SPMI_OBSERVER_BYTES);
    volatile uint32_t *channel = (volatile uint32_t *)ml_io_map(
        TB710FU_SPMI_CHANNEL_PHYS, TB710FU_SPMI_CHANNEL_BYTES);

    if (core == nullptr || configuration == nullptr || observer == nullptr ||
        channel == nullptr) {
        printf("TB710FU Platform: spmi window mapping failed\n");
        return TB710FU_RTC_NO_DEVICE;
    }
    const int mapped = tb710fu_spmi_mmio_init(
        &gSpmi, core, TB710FU_SPMI_CORE_BYTES, configuration,
        TB710FU_SPMI_CONFIG_BYTES, observer, TB710FU_SPMI_OBSERVER_BYTES,
        channel, TB710FU_SPMI_CHANNEL_BYTES, TB710FU_SPMI_EE);

    if (mapped != TB710FU_SPMI_OK) {
        printf("TB710FU Platform: spmi transport refused: %s\n",
               tb710fu_spmi_result_name(mapped));
        return TB710FU_RTC_ARGUMENT;
    }
    struct tb710fu_spmi_bus bus;

    const int probed = tb710fu_spmi_probe(&gSpmi.io, &bus);

    if (probed != TB710FU_SPMI_OK) {
        printf("TB710FU Platform: no v7 arbiter: %s (%u faults)\n",
               tb710fu_spmi_result_name(probed),
               tb710fu_spmi_mmio_faults(&gSpmi));
        return TB710FU_RTC_NO_DEVICE;
    }
    printf("TB710FU Platform: spmi v%08x, %u APIDs, ee %u\n", bus.version,
           bus.apid_count, TB710FU_SPMI_EE);

    const struct tb710fu_pmk8550_rtc_config config = rtcConfig();
    const int opened = tb710fu_rtc_open(&gRtc, &gSpmi.io, &config, &gFineSource);

    if (opened != TB710FU_RTC_OK) {
        printf("TB710FU Platform: rtc open failed: %s\n",
               tb710fu_rtc_result_name(opened));
        return opened;
    }
    printf("TB710FU Platform: rtc %s\n",
           config.allow_store ? "read-write" : "read-only");
    gClockReady = true;
    return TB710FU_RTC_OK;
}

void smcInit()
{
    if (gSmcReady) {
        return;
    }
    const int probed = tb710fu_psci_probe(&gSmc, &gPsci);

    if (probed == TB710FU_POWER_SMC_RETURNED) {
        printf("TB710FU Platform: psci v%08x off=%d reset=%d reset2=%d/%d\n",
               gPsci.version, gPsci.system_off, gPsci.system_reset,
               gPsci.reset2_64, gPsci.reset2_32);
    } else {
        printf("TB710FU Platform: psci probe returned %d\n", probed);
    }
    gSmcReady = true;
}

/* platform-svc=1: the clock and the reboot cell, read and printed, nothing
 * published and nothing written. */
void diagnose()
{
    struct tb710fu_rtc_reading reading;
    const int read = tb710fu_rtc_read(&gRtc, &reading);

    if (read == TB710FU_RTC_OK) {
        if (!reading.seconds_valid) {
            printf("TB710FU Platform: rtc counter %u, no wall clock\n",
                   reading.counter);
            return;
        }
        struct tb710fu_civil_time civil;

        if (tb710fu_rtc_epoch_to_civil(reading.seconds, &civil) == 0) {
            printf("TB710FU Platform: rtc %llu.%06u %04d-%02u-%02u "
                   "%02u:%02u:%02uZ (counter %u offset %u)\n",
                   (unsigned long long)reading.seconds, reading.useconds,
                   civil.year, civil.month, civil.day, civil.hour,
                   civil.minute, civil.second, reading.counter,
                   reading.offset);
        } else {
            printf("TB710FU Platform: rtc %llu (counter %u offset %u)\n",
                   (unsigned long long)reading.seconds, reading.counter,
                   reading.offset);
        }
    } else {
        printf("TB710FU Platform: rtc read: %s\n",
               tb710fu_rtc_result_name(read));
    }

    struct tb710fu_reboot_state reason;

    const int cell = tb710fu_reboot_reason_read(&gSpmi.io, TB710FU_SPMI_EE,
                                                &reason);

    if (cell == TB710FU_CELL_OK) {
        printf("TB710FU Platform: sdam2 0x7148 raw %02x mode %u owner %u\n",
               reason.raw, reason.mode, reason.owner);
    } else {
        printf("TB710FU Platform: sdam2 0x7148: %s\n",
               tb710fu_cell_result_name(cell));
    }
}

extern "C" int platformGetUtcTimeOfDay(clock_sec_t *seconds,
                                      clock_usec_t *microseconds)
{
    if (seconds == nullptr || microseconds == nullptr) {
        return -1;
    }
    if (hwInit() != TB710FU_RTC_OK) {
        return -2;
    }
    struct tb710fu_rtc_reading reading;

    if (tb710fu_rtc_read(&gRtc, &reading) != TB710FU_RTC_OK ||
        !reading.seconds_valid) {
        return -3;
    }
    *seconds = (clock_sec_t)reading.seconds;
    *microseconds = reading.useconds_valid ? (clock_usec_t)reading.useconds
                                          : 0;
    return 0;
}

extern "C" int platformSetUtcTimeOfDay(clock_sec_t seconds,
                                      clock_usec_t microseconds)
{
    (void)microseconds; /* the offset cell has one-second resolution */
    if (hwInit() != TB710FU_RTC_OK) {
        return -2;
    }
    struct tb710fu_rtc_set_result stored;

    return tb710fu_rtc_set_epoch(&gRtc, (uint64_t)seconds, &stored);
}

/* The platform expert's RTC read hook. 0 on success; a non-zero return leaves
 * the platform expert on its build-date floor instead of inventing a time. */
extern "C" int peReadRtc(uint64_t *seconds)
{
    if (seconds == nullptr) {
        return -1;
    }
    clock_sec_t secs = 0;
    clock_usec_t usecs = 0;

    if (platformGetUtcTimeOfDay(&secs, &usecs) != 0) {
        return -2;
    }
    *seconds = (uint64_t)secs;
    return 0;
}

extern "C" void peSetRtc(uint64_t seconds)
{
    if (platformSetUtcTimeOfDay((clock_sec_t)seconds, 0) != 0) {
        printf("TB710FU Platform: system time not stored "
               "(clock is read-only)\n");
    }
}

/* One entry for every power transition this package offers; the exported
 * wrapper is defined below, outside the namespace. */
int performPower(int action)
{
    enum tb710fu_power_action option;

    switch (action) {
    case 0:
        option = TB710FU_POWER_OFF;
        break;
    case 1:
        option = TB710FU_POWER_RESTART;
        break;
    case 2:
        option = TB710FU_POWER_RESTART_RECOVERY;
        break;
    case 3:
        option = TB710FU_POWER_RESTART_BOOTLOADER;
        break;
    case 4:
        option = TB710FU_POWER_RESTART_EDL;
        break;
    default:
        return -1;
    }
    smcInit();
    /* A power-off or a plain restart needs no SPMI access at all, and EDL
     * writes no cell, so a failed arbiter mapping is not a reason to refuse. */
    struct tb710fu_power_report report;
    const struct tb710fu_spmi_io *io = gClockReady ? &gSpmi.io : nullptr;
    const int result = tb710fu_power_action(io, TB710FU_SPMI_EE, &gSmc, &gPsci,
                                            option, &report);

    printf("TB710FU Platform: power %d stage %d cell %s smc 0x%llx "
           "(type 0x%llx cookie %llu) = %lld edl %d\n",
           action, report.stage, tb710fu_cell_result_name(report.reason_result),
           (unsigned long long)report.smc_function,
           (unsigned long long)report.smc_reset_type,
           (unsigned long long)report.smc_cookie, (long long)report.smc_result,
           report.edl_vendor);
    return result;
}

/* The platform expert's shutdown/restart entry. */
extern "C" int peHaltRestart(unsigned int type)
{
    switch (type) {
    case kPEHaltCPU:
        return tb710fu_platform_power_action(0);
    case kPERestartCPU:
    case kPEPanicRestartCPU:
    case kPEPanicRestartCPUNoCallouts:
        return tb710fu_platform_power_action(1);
    default:
        return -1;
    }
}

}  // namespace

#if TB710FU_HAS_IORTC
class Tb710fuRtcController : public IORTCController {
    OSDeclareDefaultStructors(Tb710fuRtcController)

public:
    IOReturn getRealTimeClock(UInt8 *currentTime,
                              IOByteCount *length) override;
    IOReturn setRealTimeClock(UInt8 *newTime) override;
};

class Tb710fuRtcService : public IORTC {
    OSDeclareDefaultStructors(Tb710fuRtcService)

public:
    long getGMTTimeOfDay() override;
    void setGMTTimeOfDay(long seconds) override;
    void setAlarmEnable(IOOptionBits message) override;
    void getUTCTimeOfDay(clock_sec_t *seconds,
                         clock_nsec_t *nanoseconds) override;
    void setUTCTimeOfDay(clock_sec_t seconds, clock_nsec_t nanoseconds) override;
};

OSDefineMetaClassAndStructors(Tb710fuRtcController, IORTCController)
OSDefineMetaClassAndStructors(Tb710fuRtcService, IORTC)

Tb710fuRtcController *gController = nullptr;
Tb710fuRtcService *gService = nullptr;

IOReturn Tb710fuRtcController::getRealTimeClock(UInt8 *currentTime,
                                               IOByteCount *length)
{
    if (currentTime == nullptr || length == nullptr) {
        return kIOReturnBadArgument;
    }
    struct tb710fu_rtc_reading reading;

    const int read = tb710fu_rtc_read(&gRtc, &reading);

    if (read != TB710FU_RTC_OK) {
        return read == TB710FU_RTC_READ_ONLY ? kIOReturnNotPermitted
                                            : kIOReturnNoDevice;
    }
    unsigned produced = 0;

    if (tb710fu_rtc_blob_pack(reading.seconds,
                              reading.useconds_valid ? reading.useconds : 0,
                              currentTime, (unsigned)*length, &produced) != 0) {
        return kIOReturnNoResources;
    }
    *length = produced;
    return kIOReturnSuccess;
}

IOReturn Tb710fuRtcController::setRealTimeClock(UInt8 *newTime)
{
    if (newTime == nullptr) {
        return kIOReturnBadArgument;
    }
    /* setRealTimeClock() carries no length, so the full eight-byte blob is
     * the only layout it can accept. */
    uint64_t seconds = 0;
    uint32_t microseconds = 0;

    if (tb710fu_rtc_blob_unpack(newTime, TB710FU_RTC_BLOB_BYTES, &seconds,
                                &microseconds) != 0) {
        return kIOReturnBadArgument;
    }
    struct tb710fu_rtc_set_result stored;
    const int result = tb710fu_rtc_set_epoch(&gRtc, seconds, &stored);

    if (result == TB710FU_RTC_OK || result == TB710FU_RTC_CURRENT) {
        return kIOReturnSuccess;
    }
    return result == TB710FU_RTC_READ_ONLY ? kIOReturnNotPermitted
                                           : kIOReturnError;
}

long Tb710fuRtcService::getGMTTimeOfDay()
{
    UInt8 blob[TB710FU_RTC_BLOB_BYTES];
    IOByteCount length = sizeof(blob);
    uint64_t seconds = 0;
    uint32_t microseconds = 0;

    if (gController == nullptr ||
        gController->getRealTimeClock(blob, &length) != kIOReturnSuccess ||
        tb710fu_rtc_blob_unpack(blob, (unsigned)length, &seconds,
                                &microseconds) != 0) {
        return 0;
    }
    return (long)seconds;
}

void Tb710fuRtcService::setGMTTimeOfDay(long seconds)
{
    UInt8 blob[TB710FU_RTC_BLOB_BYTES];
    unsigned length = 0;

    if (gController == nullptr ||
        tb710fu_rtc_blob_pack((uint64_t)seconds, 0, blob, sizeof(blob),
                              &length) != 0) {
        return;
    }
    if (gController->setRealTimeClock(blob) != kIOReturnSuccess) {
        printf("TB710FU Platform: setGMTTimeOfDay refused (the clock is "
               "read-only unless policy allows the offset cell)\n");
    }
}

void Tb710fuRtcService::setAlarmEnable(IOOptionBits message)
{
    /* No alarm driver: the PMK8550 alarm block (PPID 0x062) stays untouched,
     * and sleep/wake is outside this package. Accept the call so the platform
     * does not fail its power path. */
    (void)message;
}

void Tb710fuRtcService::getUTCTimeOfDay(clock_sec_t *seconds,
                                        clock_nsec_t *nanoseconds)
{
    UInt8 blob[TB710FU_RTC_BLOB_BYTES];
    IOByteCount length = sizeof(blob);
    uint64_t secondsValue = 0;
    uint32_t microseconds = 0;

    if (seconds == nullptr || nanoseconds == nullptr) {
        return;
    }
    *seconds = 0;
    *nanoseconds = 0;
    if (gController == nullptr ||
        gController->getRealTimeClock(blob, &length) != kIOReturnSuccess ||
        tb710fu_rtc_blob_unpack(blob, (unsigned)length, &secondsValue,
                                &microseconds) != 0) {
        return;
    }
    *seconds = (clock_sec_t)secondsValue;
    *nanoseconds = (clock_nsec_t)microseconds * NSEC_PER_USEC;
}

void Tb710fuRtcService::setUTCTimeOfDay(clock_sec_t seconds,
                                        clock_nsec_t nanoseconds)
{
    (void)nanoseconds; /* one-second cell resolution */
    setGMTTimeOfDay((long)seconds);
}
#endif /* TB710FU_HAS_IORTC */

void installHaltRestart()
{
    PE_halt_restart = peHaltRestart;
    printf("TB710FU Platform: PE_halt_restart installed\n");
}

/* platform-svc=2: the hooks and the IORTC resource. */
int serve()
{
    installHaltRestart();
    if (gClockReady) {
        if (armdt_platform_clock_register) {
            armdt_platform_clock_register(peReadRtc, peSetRtc);
            printf("TB710FU Platform: platform clock hook registered\n");
        } else {
            printf("TB710FU Platform: ArmDTPlatform clock hook absent, "
                   "clock hook not registered\n");
        }
    }
#if TB710FU_HAS_IORTC
    IOService *provider = IOService::getPlatform();
    IOService *resources = IOService::getResourceService();

    if (provider == nullptr || resources == nullptr) {
        printf("TB710FU Platform: no platform expert or IOResources, "
               "IORTC not published\n");
        return -1;
    }
    if (gClockReady && gController == nullptr) {
        gController = new Tb710fuRtcController;
        if (gController == nullptr || !gController->init() ||
            !gController->attach(provider)) {
            OSSafeReleaseNULL(gController);
            return -2;
        }
    }
    if (gClockReady && gService == nullptr) {
        gService = new Tb710fuRtcService;
        if (gService == nullptr || !gService->init() ||
            !gService->attach(provider)) {
            OSSafeReleaseNULL(gService);
            return -3;
        }
    }
    if (gService != nullptr) {
        /* IOResources exists here, so the resource is published; a boot whose
         * IOKitInitializeTime already ran gets the resource for later readers
         * and the next initialisation (docs/PLATFORM_BACKLIGHT_RTC.md, 3). */
        IOService::publishResource("IORTC", gService);
        printf("TB710FU Platform: IORTC published\n");
    }
#else
    printf("TB710FU Platform: this kernel has no IORTC ABI; the clock hook "
           "is the only time path\n");
#endif
    return 0;
}

/* The exported power entry: 0 off, 1 restart, 2 recovery, 3 bootloader, 4 EDL.
 * Anything else is refused before a register is touched. */
extern "C" int tb710fu_platform_power_action(int action)
{
    return performPower(action);
}

/* Called from Macos27Services.cpp between preModLoad and postModLoad, so this
 * package's metaclasses register in the same pass as the module's other
 * classes. The three deferred constructors belong to this package's three
 * translation units; the build script renames their __mod_init_func sections
 * and exports their symbols for exactly this call. */
extern "C" void tb710fu_platform_meta_init(void)
    __asm__("__GLOBAL__sub_I_Tb710fuPlatformServicesXnu.cpp");

extern "C" void tb710fu_backlight_meta_init(void)
    __asm__("__GLOBAL__sub_I_Tb710fuBacklight.cpp");

extern "C" void tb710fu_thermal_meta_init(void)
    __asm__("__GLOBAL__sub_I_Tb710fuThermal.cpp");

extern "C" void tb710fu_platform_meta_all(void)
{
    /* The backlight metaclass hangs from the live AppleARMBacklight metaclass,
     * which has to be resolved before the class is registered. */
    tb710fu_backlight_resolve_parent();
    tb710fu_platform_meta_init();
    tb710fu_backlight_meta_init();
    tb710fu_thermal_meta_init();
}

/* The package entry point. Reads every ladder itself, so a kernel that links
 * this archive without touching Macos27Services.cpp still boots byte-for-byte
 * the baseline when no argument is passed. */
extern "C" void tb710fu_platform_services_start(void)
{
    const unsigned stage = readStage(TB710FU_BOOTARG_PLATFORM,
                                    TB710FU_STAGE_PLATFORM_MAX);

    /* The three ladders are independent: a boot with only backlight= asked
     * still runs the backlight. platform-svc=0 is the default and maps
     * nothing. */
    if (stage != 0u) {
        if (hwInit() != TB710FU_RTC_OK) {
            printf("TB710FU Platform: spmi/rtc unavailable, stage %u "
                   "degraded\n", stage);
        } else {
            diagnose();
            smcInit();
        }
        if (tb710fu_stage_action(stage, gClockReady ? 1 : 0) ==
            TB710FU_STAGE_SERVE) {
            (void)serve();
        } else if (stage >= 2u) {
            /* Level 2 with a failed probe: the shutdown path does not depend
             * on SPMI, so it is still installed. */
            installHaltRestart();
        }
    }

    /* ---- panel backlight -------------------------------------------------- */
    const unsigned backlight = readStage(TB710FU_BOOTARG_BACKLIGHT,
                                         TB710FU_STAGE_BACKLIGHT_MAX);

    if (tb710fu_stage_action(backlight, 1) == TB710FU_STAGE_DIAGNOSE) {
        unsigned id = 0;
        unsigned control = 0;
        unsigned level = 0;

        if (tb710fu_backlight_probe(&id, &control, &level) == 0) {
            printf("TB710FU Platform: backlight probe ok id %02x dev-ctl %02x "
                   "level %u\n", id, control, level);
        }
    } else if (tb710fu_stage_action(backlight, 1) == TB710FU_STAGE_SERVE) {
        if (tb710fu_backlight_start() != 0) {
            printf("TB710FU Platform: backlight service refused\n");
        }
    }

    /* ---- thermal monitor -------------------------------------------------- */
    const unsigned thermal = readStage(TB710FU_BOOTARG_THERMAL,
                                       TB710FU_STAGE_THERMAL_MAX);

    if (tb710fu_stage_action(thermal, 1) != TB710FU_STAGE_NONE) {
        if (tb710fu_thermal_start() != 0) {
            printf("TB710FU Platform: thermal monitor refused\n");
        }
    }
}

#endif /* KERNEL */
