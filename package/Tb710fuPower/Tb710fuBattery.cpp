// SPDX-License-Identifier: BSD-2-Clause-Patent
// The battery as macOS reads it (boot argument battery=1): an IOPMPowerSource whose values come from the battery
// manager the audio DSP runs over PMIC GLINK (Tb710fuBattmgr.c). powerd finds every IOPMPowerSource, reads
// CurrentCapacity/MaxCapacity, IsCharging, ExternalConnected, voltage, amperage and AdapterDetails and listens for
// the status change message updateStatus sends; the menu bar, the lock screen's charging state and Settings >
// Battery follow from that.
//
// Design: everything runs on the service's work loop. The transport's callbacks only copy a message into a small
// ring and signal an IOInterruptEventSource (no provider: a software event), so its thread never waits for this
// driver. One request is in flight at a time; a 2 s timer gives up on an unanswered one. Battery notifications
// start a refresh at once, and a 60 s timer refreshes in any case (the fields that hardly change, every tenth
// time). Charging itself is the DSP firmware's; the only write is the input current limit on a computer's USB port,
// and only when the boot argument usb-sdp-ma asks for it.
//
// The transport is a separate subsystem (Tb710fuPowerLink.h). Without it the driver prints one line saying so and
// does not register: a power source with no readings would read as an empty battery.
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "Tb710fuBattmgr.h"
#include "Tb710fuPowerLink.h"
}

extern "C" Tb710fuPowerLink *tb710fu_power_link_acquire(const char *, uint32_t, uint32_t,
                                                        const Tb710fuPowerLinkClient *, void *) __attribute__((weak_import));
extern "C" int tb710fu_power_link_send(Tb710fuPowerLink *, const void *, uint32_t) __attribute__((weak_import));

// [0] stage (1 class up, 2 channel registered, 3 channel up, 4 published; negative: the step that failed),
// [1] percent, [2] mV, [3] mA, [4] temperature (raw hundredths), [5] flags (bit 0 present, 1 external, 2 charging,
// 3 full, 4 notifications enabled), [6] refresh cycles, [7] answers, [8] timeouts | errors << 16,
// [9] notifications, [10] USB type (raw), [11] full mAh, [12] design mAh, [13] cycles,
// [14] messages dropped (ring full) | ignored << 16, [15] send refusals
extern "C" {volatile int32_t native_battery_stats[16];}
// [0] capacity property (hundredths of a percent), [1..4] gauges, [5] temperature property, [6] current property
// (uA, discharge positive), [7] valid field bits, [8] the field the last timeout skipped (-1 none), [9] charge
// control limit (uA)
extern "C" {volatile int32_t native_battery_raw[10];}

namespace {
constexpr uint32_t kRequestMs = 2000, kRefreshMs = 60000, kSlowEvery = 10, kRing = 16, kMessage = 560;
constexpr char kChannel[] = "PMIC_RTR_ADSP_APPS";

// One line per boot describing what the ADT says about the path this driver needs. Read-only.
void survey()
{
    DTEntry glink = nullptr, charger = nullptr;
    if (SecureDTLookupEntry(nullptr, "/soc/qcom,pmic_glink", &glink) != kSuccess || !glink) {
        printf("TB710FU battery: ADT has no /soc/qcom,pmic_glink; the DSP battery channel is not described\n");
        return;
    }
    const void *value = nullptr; unsigned size = 0;
    const char *channel = "-";
    if (SecureDTGetProperty(glink, "qcom,pmic-glink-channel", &value, &size) == kSuccess && value && size) {
        channel = static_cast<const char *>(value);
    }
    const char *domain = "-";
    if (SecureDTGetProperty(glink, "qcom,protection-domain", &value, &size) == kSuccess && value && size) {
        domain = static_cast<const char *>(value);
    }
    const bool hasCharger = SecureDTLookupEntry(glink, "qcom,battery_charger", &charger) == kSuccess && charger;
    printf("TB710FU battery: ADT pmic-glink channel=%s protection=%s battery-charger=%u\n",
           channel, domain, hasCharger ? 1u : 0u);
}
}

class Tb710fuBattery : public IOPMPowerSource {
    OSDeclareDefaultStructors(Tb710fuBattery)
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *request = nullptr, *refresh = nullptr;
    IOInterruptEventSource *inbox = nullptr;
    IOSimpleLock *ringLock = nullptr;
    struct {uint32_t length; uint8_t data[kMessage];} ring[kRing];
    uint32_t ringHead = 0, ringCount = 0;
    volatile uint32_t channelUp = 0, channelChanged = 0, channelLost = 0;
    bool wasUp = false, published = false;
    uint32_t refreshes = 0;
    Tb710fuBattmgr bm;
    uint32_t sdpLimit = 0;
    Tb710fuPowerLink *link = nullptr;

    static void linkState(void *context, int up) {
        Tb710fuBattery *self = static_cast<Tb710fuBattery *>(context);
        if (!up) __atomic_store_n(&self->channelLost, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&self->channelUp, up ? 1u : 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&self->channelChanged, 1u, __ATOMIC_RELEASE);
        self->inbox->interruptOccurred(nullptr, nullptr, 0);
    }
    static void linkReceive(void *context, const uint8_t *data, uint32_t length) {
        Tb710fuBattery *self = static_cast<Tb710fuBattery *>(context);
        IOSimpleLockLock(self->ringLock);
        if (self->ringCount < kRing && length <= kMessage) {
            auto &slot = self->ring[(self->ringHead + self->ringCount) % kRing];
            memcpy(slot.data, data, length);
            slot.length = length;
            self->ringCount++;
        } else {
            native_battery_stats[14] = native_battery_stats[14] + 1;
        }
        IOSimpleLockUnlock(self->ringLock);
        self->inbox->interruptOccurred(nullptr, nullptr, 0);
    }

    void pump() {
        if (!wasUp || !link) return;
        uint8_t out[TB710FU_BATTMGR_REQUEST_BYTES];
        const uint32_t n = tb710fu_battmgr_next(&bm, out);
        if (!n) return;
        if (tb710fu_power_link_send(link, out, n)) native_battery_stats[15] = native_battery_stats[15] + 1;  // the timer skips the field
        request->setTimeoutMS(kRequestMs);
    }
    void number(const char *key, int64_t value) {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        OSNumber *n = OSNumber::withNumber((unsigned long long)value, 32);
        if (symbol && n) setPSProperty(symbol, n);
        OSSafeReleaseNULL(n); OSSafeReleaseNULL(symbol);
    }
    void flag(const char *key, bool value) {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        if (symbol) setPSProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
        OSSafeReleaseNULL(symbol);
    }
    void recordRaw() {
        static const int fields[7] = {BmCapacity, BmGauge1Remaining, BmGauge1Full, BmGauge2Remaining, BmGauge2Full, BmTemperature, BmCurrent};
        for (unsigned i = 0; i < 7; ++i) native_battery_raw[i] = (bm.valid >> fields[i]) & 1 ? bm.value[fields[i]] : -999999;
        native_battery_raw[7] = (int32_t)bm.valid;
        native_battery_raw[8] = bm.last_timeout;
        native_battery_raw[9] = (bm.valid >> BmCtrlLimit) & 1 ? bm.value[BmCtrlLimit] : -1;
    }
    void publish() {
        Tb710fuBatterySnapshot s = {};
        tb710fu_battmgr_snapshot(&bm, &s);
        setBatteryInstalled(s.present);
        if (s.external_known) {
            setExternalConnected(s.external);
            setExternalChargeCapable(s.external);
        }
        if (s.external_known && s.status_known) setIsCharging(s.charging);
        setAtCriticalLevel(s.critical);
        setCurrentCapacity((unsigned)s.percent);
        setMaxCapacity(100);
        setVoltage((unsigned)(s.voltage_mv > 0 ? s.voltage_mv : 0));
        setAmperage(s.current_ma);
        if (s.cycles > 0) setCycleCount((unsigned)s.cycles);
        setTimeRemaining(s.charging ? (s.time_to_full_min >= 0 ? s.time_to_full_min : 0xffff)
                                    : (s.time_to_empty_min >= 0 ? s.time_to_empty_min : 0xffff));
        if (s.external_known && s.status_known) flag("FullyCharged", s.fully_charged);
        number("InstantAmperage", s.current_ma);
        if (s.temperature_valid) number("Temperature", s.temperature);
        if (s.raw_mah > 0) number("AppleRawCurrentCapacity", s.raw_mah);
        if (s.full_mah > 0) {number("AppleRawMaxCapacity", s.full_mah); number("NominalChargeCapacity", s.full_mah);}
        if (s.design_mah > 0) number("DesignCapacity", s.design_mah);
        if (s.ctrl_limit_ua > 0) number("ChargeControlLimit", s.ctrl_limit_ua / 1000);
        OSDictionary *adapter = OSDictionary::withCapacity(5);
        if (adapter) {
            // The same family mapping sheng verified: the USB family's error space, 0xe0004000 + n.
            uint32_t family = 0xe0004007u;  // unknown
            switch (s.adapter_type) {
            case 1: family = 0xe0004000u; break;  // SDP, a host port
            case 2: family = 0xe0004004u; break;  // DCP
            case 3: family = 0xe0004005u; break;  // CDP
            case 5: family = 0xe0004009u; break;  // Type-C
            case 6: case 7: case 8: family = 0xe000400au; break;  // PD, PD DRP, PD PPS
            default: break;
            }
            OSNumber *fam = OSNumber::withNumber((unsigned long long)family, 32);
            if (fam) {adapter->setObject(kIOPMPSAdapterDetailsFamilyKey, fam); fam->release();}
            if (s.external) {
                OSNumber *w = OSNumber::withNumber((unsigned long long)s.adapter_watts, 32),
                         *mv = OSNumber::withNumber((unsigned long long)s.adapter_mv, 32),
                         *ma = OSNumber::withNumber((unsigned long long)s.adapter_ma, 32);
                if (w) {adapter->setObject(kIOPMPSAdapterDetailsWattsKey, w); w->release();}
                if (mv) {adapter->setObject(kIOPMPSAdapterDetailsVoltage, mv); mv->release();}
                if (ma) {adapter->setObject(kIOPMPSAdapterDetailsAmperageKey, ma); ma->release();}
            }
            const OSSymbol *key = OSSymbol::withCString(kIOPMPSAdapterDetailsKey);
            if (key) setPSProperty(key, adapter);
            OSSafeReleaseNULL(key);
            adapter->release();
        }
        native_battery_stats[1] = s.percent; native_battery_stats[2] = s.voltage_mv; native_battery_stats[3] = s.current_ma;
        native_battery_stats[4] = s.temperature;
        native_battery_stats[5] = s.present | s.external << 1 | s.charging << 2 | s.fully_charged << 3 | bm.enabled << 4;
        native_battery_stats[10] = (bm.valid >> BmUsbType) & 1 ? bm.value[BmUsbType] : -1;
        native_battery_stats[11] = s.full_mah; native_battery_stats[12] = s.design_mah; native_battery_stats[13] = s.cycles;
        recordRaw();
        if (!published && !s.charge_known) return;  // no charge reading yet: 0 % would read as an empty battery
        if (!published) {
            // The properties are in place before anyone finds the service.
            published = true;
            registerService();
            native_battery_stats[0] = 4;
            printf("TB710FU battery: published %d%% %d mV %d mA charging=%d\n",
                   s.percent, s.voltage_mv, s.current_ma, s.charging);
        }
        updateStatus();
    }
    void counters() {
        native_battery_stats[6] = (int32_t)bm.cycles_done;
        native_battery_stats[7] = (int32_t)bm.responses;
        native_battery_stats[8] = (int32_t)(bm.timeouts | bm.errors << 16);
        native_battery_stats[9] = (int32_t)bm.notifications;
        native_battery_stats[14] = (native_battery_stats[14] & 0xffff) | (int32_t)(bm.ignored << 16);
    }
    void handle(int result) {
        if (result == BmCycleDone) publish();
        pump();
        counters();
    }

    static void inboxAction(OSObject *owner, IOInterruptEventSource *, int) {
        Tb710fuBattery *self = OSDynamicCast(Tb710fuBattery, owner);
        if (!self || !self->link) return;
        if (__atomic_exchange_n(&self->channelChanged, 0u, __ATOMIC_ACQ_REL)) {
            const bool up = __atomic_load_n(&self->channelUp, __ATOMIC_ACQUIRE) != 0;
            // A down/up pair may coalesce into one event. Remember the loss so the readings are re-read.
            if (__atomic_exchange_n(&self->channelLost, 0u, __ATOMIC_ACQ_REL)) {
                self->request->cancelTimeout();
                tb710fu_battmgr_init(&self->bm);
                self->bm.sdp_limit_ua = self->sdpLimit;
                self->wasUp = false;
            }
            const bool before = self->wasUp;
            self->wasUp = up;
            if (up && !before) {
                native_battery_stats[0] = self->published ? 4 : 3;
                uint8_t enable[TB710FU_BATTMGR_ENABLE_BYTES];
                tb710fu_power_link_send(self->link, enable, tb710fu_battmgr_enable_request(enable));
                tb710fu_battmgr_refresh(&self->bm, 1);
                self->pump();
                self->refresh->setTimeoutMS(kRefreshMs);
                printf("TB710FU battery: channel up\n");
            }
            if (!up && before) {
                self->request->cancelTimeout();
                tb710fu_battmgr_init(&self->bm);
                self->bm.sdp_limit_ua = self->sdpLimit;
                printf("TB710FU battery: channel down\n");
            }
        }
        for (;;) {
            uint8_t data[kMessage];
            uint32_t length = 0;
            IOSimpleLockLock(self->ringLock);
            if (self->ringCount) {
                auto &slot = self->ring[self->ringHead];
                length = slot.length;
                memcpy(data, slot.data, length);
                self->ringHead = (self->ringHead + 1) % kRing;
                self->ringCount--;
            }
            IOSimpleLockUnlock(self->ringLock);
            if (!length) break;
            const int result = tb710fu_battmgr_receive(&self->bm, data, length);
            if (result == BmAnswer || result == BmCycleDone) self->request->cancelTimeout();
            self->handle(result);
        }
    }
    static void requestAction(OSObject *owner, IOTimerEventSource *) {
        Tb710fuBattery *self = OSDynamicCast(Tb710fuBattery, owner);
        if (!self || !self->wasUp) return;
        const uint32_t before = self->bm.cycles_done;
        tb710fu_battmgr_timeout(&self->bm);
        self->handle(self->bm.cycles_done != before ? BmCycleDone : BmAnswer);
    }
    static void refreshAction(OSObject *owner, IOTimerEventSource *sender) {
        Tb710fuBattery *self = OSDynamicCast(Tb710fuBattery, owner);
        if (!self || !self->wasUp) return;
        tb710fu_battmgr_refresh(&self->bm, ++self->refreshes % kSlowEvery == 0);
        self->handle(BmAnswer);
        sender->setTimeoutMS(kRefreshMs);
    }

public:
    bool startBattery() {
        tb710fu_battmgr_init(&bm);
        uint32_t ma = 0;
        if (PE_parse_boot_argn("usb-sdp-ma", &ma, sizeof ma)) bm.sdp_limit_ua = ma > 3000 ? 3000000u : ma * 1000u;
        sdpLimit = bm.sdp_limit_ua;
        loop = IOWorkLoop::workLoop();
        ringLock = IOSimpleLockAlloc();
        inbox = IOInterruptEventSource::interruptEventSource(this, &Tb710fuBattery::inboxAction);
        request = IOTimerEventSource::timerEventSource(this, &Tb710fuBattery::requestAction);
        refresh = IOTimerEventSource::timerEventSource(this, &Tb710fuBattery::refreshAction);
        if (!loop || !ringLock || !inbox || !request || !refresh || loop->addEventSource(inbox) != kIOReturnSuccess ||
            loop->addEventSource(request) != kIOReturnSuccess || loop->addEventSource(refresh) != kIOReturnSuccess)
            return false;
        native_battery_stats[0] = 1;
        if (!tb710fu_power_link_acquire || !tb710fu_power_link_send) {
            printf("TB710FU battery: no PMIC GLINK transport in this kernel; ADSP battery channel unread\n");
            native_battery_stats[0] = -1;
            return false;
        }
        const Tb710fuPowerLinkClient client = {&Tb710fuBattery::linkState, &Tb710fuBattery::linkReceive};
        link = tb710fu_power_link_acquire(kChannel, kMessage, 4, &client, this);
        if (!link) {
            printf("TB710FU battery: the ADSP transport is not running (add adsp=2); channel %s unread\n", kChannel);
            native_battery_stats[0] = -2;
            return false;
        }
        native_battery_stats[0] = 2;
        inbox->interruptOccurred(nullptr, nullptr, 0);
        return true;
    }
};
OSDefineMetaClassAndStructors(Tb710fuBattery, IOPMPowerSource)

// Called by Macos27Services.cpp before postModLoad (the constructor below is deferred, like the AES class).
extern "C" void tb710fu_battery_meta_init(void) __asm__("__GLOBAL__sub_I_Tb710fuBattery.cpp");

extern "C" void tb710fu_battery_start(void)
{
    uint32_t enabled = 0;
    if (!PE_parse_boot_argn("battery", &enabled, sizeof enabled) || !enabled) {
        printf("TB710FU battery: not requested (add battery=1)\n");
        return;
    }
    survey();
    Tb710fuBattery *battery = OSTypeAlloc(Tb710fuBattery);
    if (!battery || !battery->init() || !battery->attach(IOService::getPlatform())) {
        if (battery) battery->release();
        native_battery_stats[0] = -1;
        printf("TB710FU battery: service did not attach\n");
        return;
    }
    if (!battery->startBattery()) {
        native_battery_stats[0] = native_battery_stats[0] > 0 ? -native_battery_stats[0] - 1 : -1;
        battery->detach(IOService::getPlatform());
        battery->release();
        return;
    }
    battery->release();  // the platform's attachment keeps it
}
