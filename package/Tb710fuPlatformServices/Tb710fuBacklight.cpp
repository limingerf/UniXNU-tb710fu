// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tb710fuBacklight.cpp - the TB710FU panel backlight: the SY7758 at 0x2e on QUP
// wrap-1 serial engine 6 (i2c@a98000), published as the AppleARMBacklight the
// display server drives (boot argument backlight=0..2).
//
// Hardware, from the board devicetree only (sm8650-lenovo-tb710fu.dts):
//   backlight: backlight@2e on &i2c6, compatible "silergy,sy7758-master";
//   panel: "lenovo,nt36532-wqhd" with backlight = <&backlight>.
//   i2c6 is qupv3_id_0's serial engine 6: reg = <0x00a98000 0x4000>,
//   interrupts = <GIC_SPI 363 ...>, clock GCC_QUPV3_WRAP1_S6_CLK.
// See Tb710fuBacklightCore.h for the register map's origin and for the two
// deliberate limits (only the two brightness bytes are written, and only when
// the part already selects 12-bit mode).
//
// The engine runs the GENI I2C firmware the boot loader left loaded and
// clocked; this file votes no GCC clock, because no SM8650 GCC register offset
// is available from the pinned sources (the USB driver's are a different
// block). geni_i2c_prepare() is the liveness check instead: it reads
// FW_REVISION and M_CLK_CFG and refuses an engine that is not running the I2C
// firmware with its timing set.
//
// Transfers never wait in the service path: each step is advanced by the work
// loop from the engine's M_IRQ status, polled every millisecond while a
// transfer is in flight (this port has no verified ArmCpu SPI-attach symbol,
// so the interrupt path the sheng board used is deliberately not taken). A step
// with no answer within 25 ms is cancelled, then aborted. A newer brightness
// request replaces one not yet written, so a fade never queues up. The one
// bounded, synchronous loop is the diagnose stage's three register reads,
// which run once, on their own thread, before the display server exists.
//
// The class is not named AppleARMBacklight: AppleARMPlatform already defines
// that class and a second registration panics the kernel. Instead the class's
// metaclass hangs from the live AppleARMBacklight metaclass, resolved at
// meta_init time (the Tb710fuGpuAccelerator.cpp pattern). The class chain then
// contains AppleARMBacklight, so IOObjectConformsTo, the registry and
// backboardd's sandbox walk all answer yes, with no copied vtable and no second
// metaclass carrying Apple's name. CoreBrightness then finds the service by
// backlight-control = true, reads min and max from IODisplayParameters
// brightness and sets {brightness, FadePeriod (16.16 seconds)} through
// setProperties; iPadOS 27 sends brightness-nits (signed 16.16) and ramps by
// itself. When AppleARMBacklight is absent the metaclass falls back to IOService
// and the service is published without backlight-control, so nothing claims a
// conformance it does not have.
//
// Staged bring-up (Tb710fuPlatformStage.h):
//   backlight=0  nothing.
//   backlight=1  map the window, prepare the engine, read DEV_ID, DEV_CTL and
//                the 12-bit level, print them. No write, nothing published.
//   backlight=2  the published service, whose own read chain does the same
//                reads, then writes the 12-bit brightness registers.
//
// Only a kernel build has the IOKit ABI, so a host build gets an empty unit and
// the arithmetic keeps testing on Linux.
#ifdef KERNEL

#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <libkern/OSKextLib.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>

#include "GeniI2c.h"
#include "Tb710fuBacklightCore.h"
#include "Tb710fuPlatformStage.h"

namespace {

/* sm8650.dtsi i2c6: a 16 KiB window on QUP wrap 1, serial engine 6. */
constexpr uint64_t kEngineBase = 0x00a98000ull;
constexpr uint64_t kEngineSize = 0x4000ull;
constexpr uint32_t kStepMs = 16;
constexpr uint32_t kPollMs = 1;
constexpr uint32_t kStepDeadlineMs = 25;
constexpr uint32_t kMaxFadeMs = 5000;
constexpr unsigned kProbePolls = 4000;
constexpr uint32_t kMinNits = TB710FU_BACKLIGHT_MIN_NITS;
constexpr uint32_t kMidNits = TB710FU_BACKLIGHT_MID_NITS;
constexpr uint32_t kMaxNits = TB710FU_BACKLIGHT_MAX_NITS;

/* Field diagnosis, published on the service as "tb710fu-backlight-stats".
 * [0] stage (1 mapped, 2 engine ready, 3 published, 4 published with
 * conformance; negative: the step that failed), [1] brightness requests,
 * [2] last level written, [3] device id read, [4] DEV_CTL as read,
 * [5] level read at start, [6] transfer errors, [7] engine configuration
 * (protocol << 24 | M_CLK_CFG), [8] last error (step << 8 | GeniI2c.h code),
 * [9] timeouts, [10] transfers done, [11] 1 when the 12-bit registers are
 * written, [12] fade steps, [13] write refusals. */
constexpr unsigned kStatCount = 14u;
enum {
    kStatStage = 0, kStatRequests, kStatWritten, kStatDeviceId, kStatDevCtl,
    kStatReadLevel, kStatErrors, kStatConfig, kStatLastError, kStatTimeouts,
    kStatTransfers, kStatWritesEnabled, kStatFadeSteps, kStatRefusals
};
int32_t gStats[kStatCount];

/* The live AppleARMBacklight metaclass this class hangs from, the size it
 * reports, and whether it was found. The metaclass parent decides conformance:
 * a service whose class chain contains AppleARMBacklight answers
 * IOObjectConformsTo for it without claiming its name, which is what the
 * registry, CoreBrightness and backboardd's sandbox walk check. No vtable is
 * copied and no second metaclass with Apple's name is registered - the same
 * pattern package/Tb710fuGpu/Tb710fuGpuAccelerator.cpp uses for IOAccelerator. */
const OSMetaClass *gBase = nullptr;
uint32_t gBaseSize = 0;
bool gConforming = false;
/* Set by the metaclass constructor: until it runs, the default constructor
 * would hand IOService an unconstructed metaclass, so nothing is allocated. */
bool gMetaInstalled = false;

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
        /* A zero ml_io_map()/getVirtualAddress() result is a failure, not an
         * address. */
        return base != nullptr && ((uintptr_t)base >> 48) == 0xffffull;
    }
};

Window gEngine;

/* Aligned 32-bit accesses only: SM8650 faults with ESR 0x96000061 on an
 * unaligned Device-GRE store, so nothing here uses memcpy/bzero/memset on the
 * mapping. */
uint32_t rd(volatile uint32_t *base, uint32_t reg)
{
    return base[reg / 4u];
}

void wr(volatile uint32_t *base, uint32_t reg, uint32_t value)
{
    base[reg / 4u] = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

uint32_t ioRead(void *context, uint32_t reg)
{
    return rd(static_cast<volatile uint32_t *>(context), reg);
}

void ioWrite(void *context, uint32_t reg, uint32_t value)
{
    wr(static_cast<volatile uint32_t *>(context), reg, value);
}

void bump(unsigned index)
{
    gStats[index] = gStats[index] + 1;
}

void fail(unsigned step, int error)
{
    gStats[kStatStage] = -(int32_t)step;
    gStats[kStatLastError] = error;
}

/* One register read through the engine's own state machine, driven with a
 * bounded poll and a 1 us delay between status reads. Only the diagnose stage
 * calls it; the service path never waits. */
int probeRead(GeniI2c *bus, uint8_t reg, uint32_t *value)
{
    if (geni_i2c_start_read(bus, reg) != 0) {
        return -1;
    }
    for (unsigned poll = 0; poll < kProbePolls; ++poll) {
        const uint32_t status = rd(gEngine.base, GENI_M_IRQ_STATUS);

        if (status != 0u) {
            wr(gEngine.base, GENI_M_IRQ_CLEAR, status);
            const int result = geni_i2c_event(bus, status);

            if (result == GeniDone) {
                *value = bus->data;
                return 0;
            }
            if (result == GeniFailed) {
                return -2;
            }
        }
        IODelay(1);
    }
    (void)geni_i2c_timeout(bus);
    return -3;
}

}  // namespace

class Tb710fuBacklight final : public IOService {
    OSDeclareDefaultStructors(Tb710fuBacklight)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn setProperties(OSObject *properties) override;

private:
    /* What the engine is doing: identify the part, read its brightness mode,
     * read the level at start, then write the newest level asked for. */
    enum Step : uint8_t {
        StepNone, StepReadId, StepReadDevCtl, StepReadLsb, StepReadMsb,
        StepWriteLsb, StepWriteMsb
    };
    struct Engine {
        GeniI2c bus;
        Step step;
        bool started;
        bool usable;
        uint8_t field;   /* the byte of the current read or write */
        int32_t written; /* the level the part holds; -1 unknown */
        uint32_t sending;
        uint64_t deadline;
    };

    Engine engine = {};
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *fade = nullptr;
    IOTimerEventSource *tick = nullptr;
    IOTimerEventSource *request = nullptr;
    uint32_t level = 0;
    uint32_t target = 0;
    uint32_t requestMs = 0;
    uint32_t from = 0;
    uint32_t to = 0;
    uint64_t startTime = 0;
    uint64_t period = 0;
    bool writeEnabled = false;
    bool published = false;
    bool conforming = false;

    static uint64_t now()
    {
        return mach_absolute_time();
    }

    static uint64_t after(uint32_t milliseconds)
    {
        uint64_t delta = 0;
        nanoseconds_to_absolutetime((uint64_t)milliseconds * NSEC_PER_MSEC,
                                    &delta);
        return now() + delta;
    }

    void publishCalibration();
    void publish(uint32_t value);
    void publishFirst();
    void beginFade(uint32_t goal, uint32_t milliseconds);

    void kick();
    void noteFailure(int step);
    void finished(int result);
    void advance(uint32_t status);
    void armTick();
    void service();

    static void tickFired(OSObject *owner, IOTimerEventSource *);
    static void requestFired(OSObject *owner, IOTimerEventSource *);
    static void fadeFired(OSObject *owner, IOTimerEventSource *sender);
};

/* The hand-written metaclass, as Tb710fuGpuAccelerator.cpp does it: the
 * generated macros cannot be used because they define getMetaClass() to answer
 * this class's own metaclass, and the parent has to be resolved at run time.
 * OSDefineBasicStructors supplies the default constructor and alloc();
 * OSDefineOperatorMethods supplies the zone-backed operator new/delete. */
Tb710fuBacklight::MetaClass Tb710fuBacklight::gMetaClass;
const OSMetaClass * const Tb710fuBacklight::metaClass =
    &Tb710fuBacklight::gMetaClass;
const OSMetaClass * const Tb710fuBacklight::superClass = &IOService::gMetaClass;

Tb710fuBacklight::Tb710fuBacklight(const OSMetaClass *meta) : IOService(meta) {}
Tb710fuBacklight::~Tb710fuBacklight() {}
const OSMetaClass *Tb710fuBacklight::getMetaClass() const
{
    return &gMetaClass;
}

/* Called from the metaclass constructor, which runs inside the preModLoad
 * window: no kext is loaded here, so a kernel that booted without
 * AppleARMPlatform gets a plain IOService-rooted class and the service is
 * published without backlight-control (see publishFirst). */
Tb710fuBacklight::MetaClass::MetaClass()
    : OSMetaClass("Tb710fuBacklight",
                  gBase != nullptr ? gBase : &IOService::gMetaClass,
                  sizeof(Tb710fuBacklight))
{
    gMetaInstalled = true;
}
OSDefineBasicStructors(Tb710fuBacklight, IOService)
OSDefineOperatorMethods(Tb710fuBacklight)

/* Resolve the parent once, before the metaclass is constructed. Done at
 * meta_init time: OSKextLoadKextWithIdentifier on a class that is already in
 * the fileset blocks the boot (the pl21 note Tb710fuGpuAccelerator.cpp cites),
 * so the prelinked class is looked up first and the kext is only asked for when
 * it is genuinely absent. */
static bool resolveParent()
{
    if (gBase != nullptr) {
        return gConforming;
    }
    const OSSymbol *name = OSSymbol::withCString("AppleARMBacklight");

    const OSMetaClass *meta =
        name ? OSMetaClass::getMetaClassWithName(name) : nullptr;

    OSSafeReleaseNULL(name);
    if (meta == nullptr) {
        printf("TB710FU Backlight: AppleARMBacklight not prelinked, loading "
               "com.apple.driver.AppleARMPlatform once\n");
        const OSReturn loaded =
            OSKextLoadKextWithIdentifier("com.apple.driver.AppleARMPlatform");

        printf("TB710FU Backlight: AppleARMPlatform load=%x\n", loaded);
        name = OSSymbol::withCString("AppleARMBacklight");
        meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
        OSSafeReleaseNULL(name);
    }
    if (meta != nullptr) {
        /* The parent must still reach IOService: this object is an IOService,
         * and a chain that does not reach it would make the registry hand an
         * IOService method table to a different class. */
        bool reachesIOService = false;

        for (const OSMetaClass *cur = meta; cur != nullptr;
             cur = cur->getSuperClass()) {
            if (strcmp(cur->getClassName(), "IOService") == 0) {
                reachesIOService = true;
                break;
            }
        }
        if (reachesIOService) {
            gBase = meta;
            gBaseSize = meta->getClassSize();
            gConforming = true;
            printf("TB710FU Backlight: AppleARMBacklight size=0x%x, parent "
                   "resolved\n", gBaseSize);
            return true;
        }
        printf("TB710FU Backlight: AppleARMBacklight does not reach IOService\n");
    }
    gConforming = false;
    return false;
}

/* Called by tb710fu_platform_meta_all() before any deferred constructor runs:
 * the metaclass cannot pick its parent after it is registered. */
extern "C" void tb710fu_backlight_resolve_parent(void)
{
    (void)resolveParent();
}

void Tb710fuBacklight::kick()
{
    Engine &e = engine;

    if (!e.usable || geni_i2c_busy(&e.bus)) {
        return;
    }
    int rc;
    if (!e.started) {
        e.step = StepReadId;
        rc = geni_i2c_start_read(&e.bus, TB710FU_SY7758_REG_DEV_ID);
    } else if (e.written != (int32_t)target) {
        if (!writeEnabled) {
            e.step = StepNone;
            bump(kStatRefusals);
            return;
        }
        uint8_t low = 0;

        e.sending = target;
        tb710fu_backlight_split12(e.sending, &low, nullptr);
        e.field = low;
        e.step = StepWriteLsb;
        rc = geni_i2c_start_write(&e.bus, TB710FU_SY7758_REG_BRT_12BIT_L, low);
    } else {
        e.step = StepNone;
        return;
    }
    if (rc != 0) {
        e.step = StepNone;
        return;
    }
    e.deadline = after(kStepDeadlineMs);
}

void Tb710fuBacklight::noteFailure(int step)
{
    bump(kStatErrors);
    gStats[kStatLastError] = (int32_t)(((unsigned)step << 8) |
                                       (unsigned)engine.bus.error);
    gStats[kStatTimeouts] = (int32_t)engine.bus.timeouts;
}

void Tb710fuBacklight::finished(int result)
{
    Engine &e = engine;
    const bool ok = result == GeniDone;
    int rc = 0;

    if (ok) {
        bump(kStatTransfers);
    }
    switch (e.step) {
    case StepReadId:
        if (ok) {
            gStats[kStatDeviceId] = (int32_t)e.bus.data;
            e.step = StepReadDevCtl;
            rc = geni_i2c_start_read(&e.bus, TB710FU_SY7758_REG_DEV_CTL);
        } else {
            noteFailure(e.step);
            gStats[kStatDeviceId] = -1;
            e.started = true;
            e.written = -1;
            gStats[kStatReadLevel] = -1;
            writeEnabled = false;
        }
        break;
    case StepReadDevCtl:
        if (ok) {
            gStats[kStatDevCtl] = (int32_t)e.bus.data;
            writeEnabled = tb710fu_backlight_mode_12bit(e.bus.data) != 0;
            if (!writeEnabled) {
                printf("TB710FU Backlight: DEV_CTL %02x selects 8-bit "
                       "brightness; 12-bit writes stay off\n",
                       (unsigned)e.bus.data);
            }
            e.step = StepReadLsb;
            rc = geni_i2c_start_read(&e.bus, TB710FU_SY7758_REG_BRT_12BIT_L);
        } else {
            noteFailure(e.step);
            gStats[kStatDevCtl] = -1;
            writeEnabled = false;
            e.step = StepReadLsb;
            e.field = 0;
            rc = geni_i2c_start_read(&e.bus, TB710FU_SY7758_REG_BRT_12BIT_L);
        }
        break;
    case StepReadLsb:
        if (ok) {
            e.field = e.bus.data;
            e.step = StepReadMsb;
            rc = geni_i2c_start_read(&e.bus, TB710FU_SY7758_REG_BRT_12BIT_H);
        } else {
            noteFailure(e.step);
            e.field = 0;
            e.started = true;
            e.written = -1;
            gStats[kStatReadLevel] = -1;
        }
        break;
    case StepReadMsb: {
        /* Both halves landed: the part holds this level. */
        const unsigned read = tb710fu_backlight_join12(e.field,
                                                      ok ? e.bus.data : 0u);

        e.started = true;
        if (ok) {
            gStats[kStatReadLevel] = (int32_t)read;
            e.written = (int32_t)read;
        } else {
            noteFailure(e.step);
            gStats[kStatReadLevel] = -1;
            e.written = -1;
        }
        break;
    }
    case StepWriteLsb:
        if (ok) {
            uint8_t high = 0;

            tb710fu_backlight_split12(e.sending, nullptr, &high);
            e.field = high;
            e.step = StepWriteMsb;
            rc = geni_i2c_start_write(&e.bus, TB710FU_SY7758_REG_BRT_12BIT_H,
                                      high);
        } else {
            /* Given up: the next request writes it again. */
            noteFailure(e.step);
            e.written = -1;
        }
        break;
    case StepWriteMsb:
        if (ok) {
            gStats[kStatWritten] = (int32_t)e.sending;
            e.written = (int32_t)e.sending;
        } else {
            noteFailure(e.step);
            e.written = -1;
        }
        break;
    default:
        break;
    }
    if (rc != 0) {
        noteFailure(e.step);
    }
    if (geni_i2c_busy(&e.bus)) {
        e.deadline = after(kStepDeadlineMs);
        return;
    }
    e.step = StepNone;
    /* The read chain ends here: the part was identified and its level read,
     * or a read failed and the service refuses. */
    if (!published && e.started) {
        publishFirst();
    }
    kick();
}

void Tb710fuBacklight::advance(uint32_t status)
{
    if (!geni_i2c_busy(&engine.bus)) {
        return;
    }
    const int result = geni_i2c_event(&engine.bus, status);

    if (result != GeniPending) {
        finished(result);
    }
}

void Tb710fuBacklight::armTick()
{
    if (!geni_i2c_busy(&engine.bus)) {
        tick->cancelTimeout();
        return;
    }
    tick->setTimeoutMS(kPollMs);
}

void Tb710fuBacklight::service()
{
    const uint32_t status = rd(gEngine.base, GENI_M_IRQ_STATUS);

    if (status != 0u) {
        wr(gEngine.base, GENI_M_IRQ_CLEAR, status);
        advance(status);
    }
    if (geni_i2c_busy(&engine.bus) && now() >= engine.deadline) {
        const int result = geni_i2c_timeout(&engine.bus);

        gStats[kStatTimeouts] = (int32_t)engine.bus.timeouts;
        if (result != GeniPending) {
            finished(result);
        } else {
            engine.deadline = after(kStepDeadlineMs);
        }
    }
    armTick();
}

void Tb710fuBacklight::tickFired(OSObject *owner, IOTimerEventSource *)
{
    if (Tb710fuBacklight *self = static_cast<Tb710fuBacklight *>(owner)) {
        self->service();
    }
}

void Tb710fuBacklight::requestFired(OSObject *owner, IOTimerEventSource *)
{
    if (Tb710fuBacklight *self = static_cast<Tb710fuBacklight *>(owner)) {
        self->beginFade(self->target, self->requestMs);
    }
}

void Tb710fuBacklight::fadeFired(OSObject *owner, IOTimerEventSource *sender)
{
    Tb710fuBacklight *self = static_cast<Tb710fuBacklight *>(owner);

    if (!self) {
        return;
    }
    const uint32_t next = tb710fu_backlight_fade_level(
        self->from, self->to, now() - self->startTime, self->period);

    if (next != self->level) {
        bump(kStatFadeSteps);
        self->level = next;
        self->target = next;
        self->kick();
        self->armTick();
    }
    if (next != self->to) {
        sender->setTimeoutMS(kStepMs);
    }
}

void Tb710fuBacklight::beginFade(uint32_t goal, uint32_t milliseconds)
{
    fade->cancelTimeout();
    from = level;
    to = tb710fu_backlight_clamp(goal);
    period = 0;
    if (milliseconds > kMaxFadeMs) {
        milliseconds = kMaxFadeMs;
    }
    nanoseconds_to_absolutetime((uint64_t)milliseconds * NSEC_PER_MSEC,
                               &period);
    startTime = now();
    fadeFired(this, fade);
}

void Tb710fuBacklight::publish(uint32_t value)
{
    OSDictionary *brightness = OSDictionary::withCapacity(3);
    OSDictionary *milliNits = OSDictionary::withCapacity(3);
    OSDictionary *parameters = OSDictionary::withCapacity(2);
    OSNumber *min = OSNumber::withNumber(0ull, 32);
    OSNumber *max =
        OSNumber::withNumber((uint64_t)TB710FU_SY7758_MAX_LEVEL, 32);
    OSNumber *current = OSNumber::withNumber((uint64_t)value, 32);
    OSNumber *minMilli = OSNumber::withNumber((uint64_t)kMinNits * 1000u, 32);
    OSNumber *maxMilli = OSNumber::withNumber((uint64_t)kMaxNits * 1000u, 32);
    OSNumber *currentMilli = OSNumber::withNumber(
        tb710fu_backlight_milli_nits_for_level(value, kMaxNits), 32);

    if (brightness && milliNits && parameters && min && max && current &&
        minMilli && maxMilli && currentMilli) {
        brightness->setObject("min", min);
        brightness->setObject("max", max);
        brightness->setObject("value", current);
        milliNits->setObject("min", minMilli);
        milliNits->setObject("max", maxMilli);
        milliNits->setObject("value", currentMilli);
        parameters->setObject("brightness", brightness);
        /* BrightnessControl takes the nits range and the current nits from
         * here on iPadOS 27. */
        parameters->setObject("BrightnessMilliNits", milliNits);
        setProperty("IODisplayParameters", parameters);
    }
    OSSafeReleaseNULL(min);
    OSSafeReleaseNULL(max);
    OSSafeReleaseNULL(current);
    OSSafeReleaseNULL(minMilli);
    OSSafeReleaseNULL(maxMilli);
    OSSafeReleaseNULL(currentMilli);
    OSSafeReleaseNULL(brightness);
    OSSafeReleaseNULL(milliNits);
    OSSafeReleaseNULL(parameters);
}

void Tb710fuBacklight::publishCalibration()
{
    const int32_t table[] = {(int32_t)(kMinNits << 16),
                             (int32_t)(kMidNits << 16),
                             (int32_t)(kMaxNits << 16)};

    /* CoreBrightness's Display.m fails without a marketing table (equally
     * spaced slider samples, signed 16.16 nits), and without calibration
     * parameters it reads that table before loading it. Both describe the
     * same declared range as BrightnessMilliNits; none of these is a
     * measurement. */
    if (OSData *marketing = OSData::withBytes(table, sizeof(table))) {
        setProperty("backlight-marketing-table", marketing);
        marketing->release();
    }
    /* LmaxProduct is read as four bytes of 16.16; LmidProduct and LminProduct
     * take a number as it is (as data they would be read as a float). */
    if (OSData *value = OSData::withBytes(&table[2], sizeof(table[2]))) {
        setProperty("LmaxProduct", value);
        value->release();
    }
    const struct {
        const char *key;
        uint32_t nits;
    } products[] = {{"LminProduct", kMinNits}, {"LmidProduct", kMidNits}};

    for (const auto &product : products) {
        if (OSNumber *value = OSNumber::withNumber(product.nits, 32)) {
            setProperty(product.key, value);
            value->release();
        }
    }
    OSDictionary *calibration = OSDictionary::withCapacity(6);

    if (!calibration) {
        return;
    }
    const struct {
        const char *key;
        uint32_t nits;
    } fields[] = {{"lMinProduct", kMinNits}, {"lMidProduct", kMidNits},
                  {"lMaxProduct", kMaxNits}, {"lMinPanel", kMinNits},
                  {"lMidPanel", kMidNits}, {"lMaxPanel", kMaxNits}};

    for (const auto &field : fields) {
        if (OSNumber *number =
                OSNumber::withNumber((uint64_t)field.nits << 16, 32)) {
            calibration->setObject(field.key, number);
            number->release();
        }
    }
    setProperty("backlight-calibration-parameters", calibration);
    calibration->release();
}

void Tb710fuBacklight::publishFirst()
{
    published = true;
    const int32_t read = gStats[kStatReadLevel];
    const int32_t id = gStats[kStatDeviceId];

    if (id != (int32_t)TB710FU_SY7758_DEV_ID_VALUE || read < 0) {
        /* Nothing answered with the part's id: publishing a slider for a
         * backlight that cannot be read would lie to the display server. */
        fail(8u, read);
        printf("TB710FU Backlight: probe refused (id %d level %d)\n", id, read);
        gStats[kStatStage] = -8;
        return;
    }
    level = (uint32_t)read;
    to = target = level;
    /* CoreBrightness takes the service for the built-in display's only when
     * IOObjectConformsTo(service, "AppleARMBacklight") holds. Without that
     * conformance, publishing backlight-control leaves the libEDR server
     * unstarted and the display server blocking on it, so the conformance
     * decides, not a boot argument. */
    if (conforming) {
        setProperty("NativeSandboxGrantProcess", "backboardd");
        setProperty("backlight-control", true);
        gStats[kStatStage] = 4;
    } else {
        gStats[kStatStage] = 3;
    }
    publishCalibration();
    publish(level);
    if (OSData *data = OSData::withBytes(gStats, sizeof(gStats))) {
        setProperty("tb710fu-backlight-stats", data);
        data->release();
    }
    registerService();
    printf("TB710FU Backlight: level %u/%u id %d dev-ctl %d 12-bit %d "
           "conforming %d\n",
           level, TB710FU_SY7758_MAX_LEVEL, (int)id, (int)gStats[kStatDevCtl],
           writeEnabled ? 1 : 0, conforming ? 1 : 0);
}

bool Tb710fuBacklight::start(IOService *provider)
{
    if (!super::start(provider)) {
        return false;
    }
    if (!gEngine.open(kEngineBase, kEngineSize)) {
        fail(1u, -12);
        printf("TB710FU Backlight: i2c6 window map failed\n");
        return false;
    }
    gStats[kStatStage] = 1;
    conforming = resolveParent();
    loop = IOWorkLoop::workLoop();
    fade = IOTimerEventSource::timerEventSource(this,
                                                &Tb710fuBacklight::fadeFired);
    tick = IOTimerEventSource::timerEventSource(this,
                                                &Tb710fuBacklight::tickFired);
    request = IOTimerEventSource::timerEventSource(
        this, &Tb710fuBacklight::requestFired);
    if (!loop || !fade || !tick || !request) {
        fail(2u, -12);
        return false;
    }
    if (loop->addEventSource(fade) != kIOReturnSuccess ||
        loop->addEventSource(tick) != kIOReturnSuccess ||
        loop->addEventSource(request) != kIOReturnSuccess) {
        fail(2u, -16);
        return false;
    }
    const GeniIo io = {ioRead, ioWrite, (void *)gEngine.base};
    uint32_t config = 0;

    engine.usable = geni_i2c_prepare(&engine.bus, &io, TB710FU_SY7758_ADDRESS,
                                     &config) == 0;
    gStats[kStatConfig] = (int32_t)config;
    engine.started = !engine.usable;
    engine.written = -1;
    if (!engine.usable) {
        gStats[kStatReadLevel] = -1;
        gStats[kStatDeviceId] = -1;
        fail(2u, (int)engine.bus.error);
        printf("TB710FU Backlight: engine not usable (config %08x)\n",
               (unsigned)config);
        return false;
    }
    printf("TB710FU Backlight: engine proto %u clk_cfg %06x\n",
           (unsigned)(config >> 24), (unsigned)(config & 0xffffffu));
    gStats[kStatStage] = 2;
    kick();
    armTick();
    return true;
}

void Tb710fuBacklight::stop(IOService *provider)
{
    if (tick) {
        tick->cancelTimeout();
    }
    if (fade) {
        fade->cancelTimeout();
    }
    if (request) {
        request->cancelTimeout();
    }
    /* The window and the service stay: the display server holds a reference to
     * the registered service, and a released object would leave it dangling
     * (the sheng board's rule). */
    super::stop(provider);
}

IOReturn Tb710fuBacklight::setProperties(OSObject *properties)
{
    OSDictionary *dictionary = OSDynamicCast(OSDictionary, properties);

    if (!dictionary) {
        return kIOReturnBadArgument;
    }
    /* iPadOS 27 sends signed 16.16 nits and ramps them itself (zero or less is
     * the display going dark); iPadOS 17 sends a level and a fade period in
     * 16.16 seconds. */
    uint32_t goal;
    uint32_t milliseconds = 0;

    if (OSNumber *nits =
            OSDynamicCast(OSNumber, dictionary->getObject("brightness-nits"))) {
        goal = tb710fu_backlight_level_for_nits(
            (int32_t)nits->unsigned32BitValue(), kMaxNits);
    } else if (OSNumber *brightness = OSDynamicCast(
                   OSNumber, dictionary->getObject("brightness"))) {
        const int32_t requested = (int32_t)brightness->unsigned32BitValue();

        goal = requested < 0 ? 0u
                             : tb710fu_backlight_clamp((unsigned)requested);
        if (OSNumber *fadePeriod =
                OSDynamicCast(OSNumber,
                              dictionary->getObject("FadePeriod"))) {
            milliseconds =
                (uint32_t)(((uint64_t)fadePeriod->unsigned32BitValue() *
                            1000u) >> 16);
        }
    } else {
        return kIOReturnSuccess;
    }
    bump(kStatRequests);
    /* Handed to the work loop without waiting for it: a runAction() here
     * blocks backboardd's brightness thread on the work loop's gate, and the
     * request timer then never fires again. The newest request replaces one
     * not yet taken, so a fade never queues up. */
    __atomic_store_n(&requestMs, milliseconds, __ATOMIC_RELEASE);
    __atomic_store_n(&target, goal, __ATOMIC_RELEASE);
    request->setTimeoutUS(1);
    publish(goal);
    return kIOReturnSuccess;
}

/* ---- the two entry points the platform services call -------------------- */

/* backlight=1: map, prepare, read DEV_ID / DEV_CTL / the 12-bit level, print.
 * No write, no service. Returns 0 when every read landed. */
extern "C" int tb710fu_backlight_probe(unsigned *dev_id, unsigned *dev_ctl,
                                       unsigned *read_level)
{
    unsigned stage = 0;

    if (dev_id == nullptr || dev_ctl == nullptr || read_level == nullptr) {
        return -1;
    }
    /* The ladder's own decision, repeated here: a direct caller cannot map the
     * window unless the boot asked for backlight=1 or more. */
    if (!tb710fu_stage_allow(PE_boot_args(), TB710FU_BOOTARG_BACKLIGHT,
                             TB710FU_STAGE_BACKLIGHT_MAX, 1u, &stage)) {
        printf("TB710FU Backlight: backlight=%u is below 1, probe refused\n",
               stage);
        return -1;
    }
    if (!gEngine.open(kEngineBase, kEngineSize)) {
        fail(1u, -12);
        printf("TB710FU Backlight: i2c6 window map failed\n");
        return -1;
    }
    gStats[kStatStage] = 1;
    printf("TB710FU Backlight: i2c6 %llx mapped (%u bytes)\n",
           (unsigned long long)kEngineBase, (unsigned)kEngineSize);

    const GeniIo io = {ioRead, ioWrite, (void *)gEngine.base};
    GeniI2c bus = {};
    uint32_t config = 0;

    if (geni_i2c_prepare(&bus, &io, TB710FU_SY7758_ADDRESS, &config) != 0) {
        fail(2u, (int)bus.error);
        printf("TB710FU Backlight: engine not usable (config %08x)\n",
               (unsigned)config);
        return -2;
    }
    gStats[kStatConfig] = (int32_t)config;
    printf("TB710FU Backlight: engine proto %u clk_cfg %06x\n",
           (unsigned)(config >> 24), (unsigned)(config & 0xffffffu));

    uint32_t value = 0;
    int rc = probeRead(&bus, TB710FU_SY7758_REG_DEV_ID, &value);

    if (rc != 0) {
        fail(4u, rc);
        printf("TB710FU Backlight: DEV_ID read failed (%d)\n", rc);
        return -3;
    }
    *dev_id = value;
    gStats[kStatDeviceId] = (int32_t)value;
    rc = probeRead(&bus, TB710FU_SY7758_REG_DEV_CTL, &value);
    if (rc != 0) {
        fail(5u, rc);
        printf("TB710FU Backlight: DEV_CTL read failed (%d)\n", rc);
        return -4;
    }
    *dev_ctl = value;
    gStats[kStatDevCtl] = (int32_t)value;
    uint8_t low = 0;
    uint8_t high = 0;

    rc = probeRead(&bus, TB710FU_SY7758_REG_BRT_12BIT_L, &value);
    if (rc == 0) {
        low = (uint8_t)value;
        rc = probeRead(&bus, TB710FU_SY7758_REG_BRT_12BIT_H, &value);
        if (rc == 0) {
            high = (uint8_t)value;
        }
    }
    if (rc != 0) {
        fail(6u, rc);
        printf("TB710FU Backlight: brightness read failed (%d)\n", rc);
        return -5;
    }
    *read_level = tb710fu_backlight_join12(low, high);
    gStats[kStatReadLevel] = (int32_t)*read_level;
    printf("TB710FU Backlight: id %02x dev-ctl %02x level %u 12-bit %d\n",
           (unsigned)*dev_id, (unsigned)*dev_ctl, *read_level,
           tb710fu_backlight_mode_12bit((uint8_t)*dev_ctl));
    return 0;
}

/* backlight=2: the published service. Its own read chain repeats the probe
 * asynchronously and publishes when it ends. */
extern "C" int tb710fu_backlight_start(void)
{
    unsigned stage = 0;

    /* Only the serving level publishes a service the display server drives. */
    if (!tb710fu_stage_allow(PE_boot_args(), TB710FU_BOOTARG_BACKLIGHT,
                             TB710FU_STAGE_BACKLIGHT_MAX, 2u, &stage)) {
        fail(3u, -1);
        printf("TB710FU Backlight: backlight=%u is below 2, service refused\n",
               stage);
        return -4;
    }
    if (!gMetaInstalled) {
        fail(3u, -19);
        printf("TB710FU Backlight: metaclass not installed "
               "(meta_init not called), service refused\n");
        return -4;
    }
    IOService *provider = IOService::getPlatform();

    if (!provider) {
        fail(3u, -19);
        printf("TB710FU Backlight: platform expert absent\n");
        return -1;
    }
    Tb710fuBacklight *service = new Tb710fuBacklight;

    if (!service) {
        fail(3u, -12);
        return -2;
    }
    if (!service->init() || !service->attach(provider) ||
        !service->start(provider)) {
        printf("TB710FU Backlight: service did not start\n");
        service->detach(provider);
        service->release();
        return -3;
    }
    /* Never released: the registered service outlives this call, and the
     * display server keeps a reference to it. */
    return 0;
}

#endif /* KERNEL */
