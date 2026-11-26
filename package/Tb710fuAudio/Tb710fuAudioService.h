// SPDX-License-Identifier: BSD-2-Clause-Patent
// The TB710FU audio service: declarations shared by Tb710fuAudio.cpp (bring-up) and
// Tb710fuAudioService.cpp (the IOService + IOUserClient pair a CoreAudio HAL plugin opens).
//
// The service is deliberately not an IOAudioFamily driver: on macOS 27 the family is closed to
// new drivers and every one of its symbols would be an undefined link symbol for this archive.
// The pair below publishes the ABI in Tb710fuAudioAbi.h through an IORegistry property
// ("tb710fu-audio", magic + version) and a user client whose selectors are the ABI's.
#ifndef TB710FU_AUDIO_SERVICE_H
#define TB710FU_AUDIO_SERVICE_H

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <stdint.h>

extern "C" {
#include "Tb710fuAcf.h"
#include "Tb710fuAudioAbi.h"
#include "Tb710fuAudioEngine.h"
#include "Tb710fuAudioReach.h"
#include "Tb710fuAw88461.h"
#include "Tb710fuGeniI2c.h"
#include "Tb710fuGpr.h"
#include "Tb710fuPowerLink.h"

// The ADSP transport (package/Tb710fuAdsp) provides the GLINK client this driver sends through.
Tb710fuPowerLink *tb710fu_power_link_acquire(const char *, uint32_t, uint32_t, const Tb710fuPowerLinkClient *,
                                             void *) __attribute__((weak_import));
int tb710fu_power_link_send(Tb710fuPowerLink *, const void *, uint32_t) __attribute__((weak_import));
// Firmware files: the Data volume or the boot addon (Tb710fuAdspFirmware.c).
int tb710fu_fw_open(const char *, void **, uint64_t *) __attribute__((weak_import));
int tb710fu_fw_read(void *, uint64_t, void *, size_t) __attribute__((weak_import));
void tb710fu_fw_close(void *) __attribute__((weak_import));

extern volatile int32_t native_tb710fu_audio_stats[16];
extern volatile int32_t native_tb710fu_audio_pins[4];
extern char native_tb710fu_audio_text[TB710FU_AUDIO_TEXT_BYTES];
}

/* Hardware facts, each from one device tree node (see docs/AUDIO_AW88461_ADSP.md):
 *   i2c_hub_4: i2c@990000 in sm8650.dtsi (reg 0x00990000, 0x4000 bytes)
 *   tlmm: pinctrl@f100000 in sm8650.dtsi (reg 0x0F100000, 0x300000 bytes)
 *   the four amps' reset lines: awinic_1..4's reset-gpio in sm8650-lenovo-tb710fu.dts (9, 8, 2, 60). */
static constexpr uint32_t kI2cBase = 0x00990000u;
static constexpr uint32_t kI2cBytes = 0x00004000u;
static constexpr uint32_t kTlmmBase = 0x0F100000u;
static constexpr uint32_t kTlmmBytes = 0x00300000u;
static constexpr uint32_t kTlmmPinStride = 0x1000u;   // the msm TLMM family's layout; read-only here
static constexpr uint32_t kAmpCount = TB710FU_AMP_COUNT;
static constexpr uint32_t kResetPin[kAmpCount] = {9, 8, 2, 60};
static constexpr char kAcfDefault[] = "/private/var/firmware/awinic/aw88461_tb710fu.bin";
static constexpr char kChannel[] = "adsp_apps";        // sm8650.dtsi: gpr qcom,glink-channels
static constexpr uint32_t kI2cTimeoutUs = 2000;
static constexpr uint32_t kTickMs = 10;
static constexpr uint32_t kRing = 8;
static constexpr uint32_t kMessage = TB710FU_GPR_MAX_PACKET;

struct Tb710fuAudioBootOptions {
    uint32_t stage;
    uint32_t rate;
    uint32_t speakerBits, speakerChannels;
    uint32_t captureBits, captureChannels;
    uint32_t ampMute, ampVolume;
    uint32_t tdmInterface, tdmSd, tdmWs, tdmParam;
    uint32_t gprDst, gprSrc;
    char acfPath[128];
};

/* One GENI serial engine, mapped and polled. Only 32-bit aligned MMIO accesses: byte-granular
 * loads on this block have produced ESR 0x96000061 on SM8650. */
class Tb710fuAudioGeniChannel {
public:
    IOMemoryMap *map = nullptr;
    Tb710fuGeni geni = {};
    uint32_t ok = 0, fail = 0, timeouts = 0;

    bool mapRegisters();
    static uint32_t readRegister(void *context, uint32_t offset);
    static void writeRegister(void *context, uint32_t offset, uint32_t value);
    int write(uint8_t address, const uint8_t *bytes, uint32_t length);
    int read(uint8_t address, const uint8_t *command, uint32_t commandLength, uint8_t *to, uint32_t readLength);
    int finishTransfer();
};

class Tb710fuAudioDevice;

class Tb710fuAudioUserClient : public IOUserClient {
    OSDeclareDefaultStructors(Tb710fuAudioUserClient)
public:
    Tb710fuAudioDevice *device = nullptr;
    task_t owner = nullptr;

    bool initWithTask(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn clientDied() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                            IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) override;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;
};

class Tb710fuAudioDevice : public IOService {
    OSDeclareDefaultStructors(Tb710fuAudioDevice)
public:
    Tb710fuAudioBootOptions options = {};
    Tb710fuAudioGeniChannel i2c;
    Tb710fuAmp amp[kAmpCount] = {};
    Tb710fuAcf acf = {};
    uint8_t *acfBytes = nullptr;
    uint32_t acfLength = 0;
    Tb710fuAmpBus ampBus = {};
    Tb710fuGpr gpr = {};
    Tb710fuAudioGraph graph = {};
    Tb710fuGraphConfig graphConfig = {};
    Tb710fuPowerLink *link = nullptr;
    IOSimpleLock *ringLock = nullptr;
    struct { uint32_t length; uint8_t data[kMessage]; } ring[kRing] = {};
    uint32_t ringHead = 0, ringCount = 0;
    volatile uint32_t linkUp = 0, linkChanged = 0;
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *tick = nullptr;
    IOLock *stateLock = nullptr;
    IOMemoryDescriptor *shared = nullptr;
    Tb710fuAudioSharedHeader *sharedHeader = nullptr;
    uint32_t sharedBytes = 0;
    uint64_t sharedPhysical = 0;
    uint32_t stage = 0;
    uint32_t lastError = Tb710fuAudioNoError;
    int32_t volumeDb = 0;
    uint32_t muted = 0;
    uint32_t streamMask = 0;
    bool ioAudioFamily = false;   // looked up by name at start; never a link symbol

    bool setup();
    bool bringUpI2c();
    bool bringUpAmps();
    bool loadAcf();
    bool bringUpGraph();
    bool mapBuffer();
    void tickOnce();
    uint64_t nowUs();
    int pushPacket(const uint8_t *packet, uint32_t bytes);
    void serviceRing();
    int status(Tb710fuAudioStatus *out);
    int setVolume(int32_t db);
    int setMute(uint32_t mute);
    int commitPlayback(uint32_t chunk);
    int startStop(uint32_t mask, uint32_t on);
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
                           IOUserClient **handler) override;
    static void onTick(OSObject *owner, IOTimerEventSource *sender);
    static void linkState(void *context, int up);
    static void linkReceive(void *context, const uint8_t *data, uint32_t length);
    static int portSend(void *context, const uint8_t *packet, uint32_t bytes);
};

/* One line of bring-up text the user client can read back (kTb710fuAudioGetText). */
extern IOLock *gTextLock;
void tb710fu_audio_note(const char *format, ...);

/* Boot-argument options; exported so the service reports the requested stage. */
void tb710fu_audio_parse_options(Tb710fuAudioBootOptions *o);

#endif
