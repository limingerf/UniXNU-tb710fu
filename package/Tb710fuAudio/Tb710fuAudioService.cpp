// SPDX-License-Identifier: BSD-2-Clause-Patent
// The TB710FU audio service's user-facing half: the IOService whose IORegistry properties name the
// ABI (Tb710fuAudioAbi.h) and the IOUserClient a CoreAudio HAL plugin opens to drive it.
//
// Why not IOAudioFamily: on macOS 27 the family still exists as a kext, but it is closed to new
// drivers and every symbol of it (IOAudioDevice, IOAudioEngine, IOAudioStream, ...) would become an
// undefined link symbol for libTb710fuAudio.a. Nothing here references the family; the only
// interaction is a name lookup at start (OSMetaClass::getMetaClassWithName on "IOAudioDevice")
// whose result is reported as a status flag, so a plugin can tell what the kernel has without
// guessing. The same pattern package/Tb710fuEthernet.cpp uses for IONetworkingFamily.
//
// Audio data crosses as a shared, physically contiguous region: clientMemoryForType(0) hands the
// client the region Tb710fuAudio.cpp mapped; the client fills one 4 KiB chunk, commits it with
// kTb710fuAudioCommitPlayback, and the service copies it into the DSP's shared-memory endpoint
// with the current gain applied. Capture is the mirror: the service asks the DSP for a chunk, the
// client takes it with kTb710fuAudioFetchCapture.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <stdint.h>
#include <string.h>

#include "Tb710fuAudioService.h"

OSDefineMetaClassAndStructors(Tb710fuAudioDevice, IOService)
OSDefineMetaClassAndStructors(Tb710fuAudioUserClient, IOUserClient)

/* --------------------------------- the user client -------------------------------- */

bool Tb710fuAudioUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type,
                                          OSDictionary *properties)
{
    if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) return false;
    owner = owningTask;
    return true;
}

bool Tb710fuAudioUserClient::start(IOService *provider)
{
    if (!IOUserClient::start(provider)) return false;
    device = OSDynamicCast(Tb710fuAudioDevice, provider);
    return device != nullptr;
}

void Tb710fuAudioUserClient::stop(IOService *provider)
{
    device = nullptr;
    IOUserClient::stop(provider);
}

IOReturn Tb710fuAudioUserClient::clientClose()
{
    device = nullptr;
    return kIOReturnSuccess;
}

IOReturn Tb710fuAudioUserClient::clientDied() {return clientClose();}

IOReturn Tb710fuAudioUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
    if (type != 0 || !device || !device->shared || !memory) return kIOReturnNoResources;
    device->shared->retain();
    *memory = device->shared;
    *options = 0;
    return kIOReturnSuccess;
}

IOReturn Tb710fuAudioUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                                                IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
    (void)dispatch;
    (void)target;
    (void)reference;
    if (!device || !arguments) return kIOReturnNotReady;
    const uint32_t first = (arguments->scalarInputCount > 0) ? (uint32_t)arguments->scalarInput[0] : 0;

    switch (selector) {
    case kTb710fuAudioGetStatus: {
        if (!arguments->structureOutput || arguments->structureOutputSize < sizeof(Tb710fuAudioStatus))
            return kIOReturnBadArgument;
        Tb710fuAudioStatus status = {};
        if (device->status(&status)) return kIOReturnNotReady;
        memcpy(arguments->structureOutput, &status, sizeof status);
        arguments->structureOutputSize = sizeof status;
        return kIOReturnSuccess;
    }
    case kTb710fuAudioSetVolume:
        return device->setVolume((int32_t)first) ? kIOReturnNotReady : kIOReturnSuccess;
    case kTb710fuAudioSetMute:
        return device->setMute(first ? 1u : 0u) ? kIOReturnNotReady : kIOReturnSuccess;
    case kTb710fuAudioStart:
        return device->startStop(first ? first : 3u, 1) ? kIOReturnNotReady : kIOReturnSuccess;
    case kTb710fuAudioStop:
        return device->startStop(first ? first : 3u, 0) ? kIOReturnNotReady : kIOReturnSuccess;
    case kTb710fuAudioCommitPlayback:
        return device->commitPlayback(first) ? kIOReturnNotReady : kIOReturnSuccess;
    case kTb710fuAudioFetchCapture:
        // The service fills captured chunks itself; fetching only moves the client's index.
        return kIOReturnSuccess;
    case kTb710fuAudioGetText: {
        if (!arguments->structureOutput || arguments->structureOutputSize < TB710FU_AUDIO_TEXT_BYTES)
            return kIOReturnBadArgument;
        if (gTextLock) IOLockLock(gTextLock);
        memcpy(arguments->structureOutput, native_tb710fu_audio_text, TB710FU_AUDIO_TEXT_BYTES);
        if (gTextLock) IOLockUnlock(gTextLock);
        arguments->structureOutputSize = TB710FU_AUDIO_TEXT_BYTES;
        return kIOReturnSuccess;
    }
    default:
        return kIOReturnUnsupported;
    }
}

/* --------------------------------- the service API -------------------------------- */

IOReturn Tb710fuAudioDevice::newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                           OSDictionary *properties, IOUserClient **handler)
{
    if (type != 0 || !handler) return kIOReturnUnsupported;
    Tb710fuAudioUserClient *client = new Tb710fuAudioUserClient;
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(owningTask, securityID, type, properties) || !client->attach(this) ||
        !client->start(this)) {
        client->release();
        return kIOReturnNoResources;
    }
    *handler = client;
    return kIOReturnSuccess;
}

int Tb710fuAudioDevice::status(Tb710fuAudioStatus *out)
{
    if (!out) return -1;
    IOLockLock(stateLock);
    memset(out, 0, sizeof *out);
    out->magic = TB710FU_AUDIO_ABI_MAGIC;
    out->version = TB710FU_AUDIO_ABI_VERSION;
    out->size = sizeof *out;
    out->state = stage;
    out->requested_stage = options.stage;
    out->last_error = lastError;
    uint32_t flags = 0;
    if (stage >= Tb710fuAudioI2cProbe) flags |= Tb710fuAudioFlagI2c;
    if (stage >= Tb710fuAudioAmpInit) flags |= Tb710fuAudioFlagAmpControl;
    if (graph.step >= Tb710fuGraphPlaybackStart) flags |= Tb710fuAudioFlagSpeakerGraph;
    if (graph.step >= Tb710fuGraphCaptureStart) flags |= Tb710fuAudioFlagCaptureGraph;
    if (graph.handle) flags |= Tb710fuAudioFlagMapped;
    if (graph.step == Tb710fuGraphRunning) flags |= Tb710fuAudioFlagRunning;
    if (muted) flags |= Tb710fuAudioFlagMuted;
    if (options.ampVolume) flags |= Tb710fuAudioFlagAmpVolume;
    if (ioAudioFamily) flags |= Tb710fuAudioFlagIOAudioFamily;
    out->flags = flags;
    uint32_t present = 0, loaded = 0, writes = 0, errors = 0, readback = 0, bad = 0;
    for (uint32_t i = 0; i < kAmpCount; ++i) {
        if (amp[i].present) present++;
        if (amp[i].pairs && !amp[i].pair_errors) loaded++;
        writes += amp[i].writes;
        errors += amp[i].write_errors;
        readback += amp[i].readback;
        bad += amp[i].readback_bad;
        out->amp_identity[i] = amp[i].identity;
    }
    out->amps_present = present;
    out->amps_loaded = loaded;
    out->amps_muted = muted;
    out->amp_writes = writes;
    out->amp_write_errors = errors;
    out->amp_readback = readback;
    out->amp_readback_bad = bad;
    out->gpr_sent = graph.gpr.sent;
    out->gpr_answers = graph.answers;
    out->gpr_timeouts = graph.timeouts;
    out->gpr_foreign = graph.foreign;
    out->speaker_rate = options.rate;
    out->speaker_bits = options.speakerBits;
    out->speaker_channels = options.speakerChannels;
    out->capture_rate = options.rate;
    out->capture_bits = options.captureBits;
    out->capture_channels = options.captureChannels;
    out->volume_db = (uint32_t)(volumeDb < 0 ? -volumeDb : 0);
    out->write_done = graph.write_done;
    out->write_busy = graph.write_started > graph.write_done ? graph.write_started - graph.write_done : 0;
    out->read_done = graph.read_done;
    out->read_busy = graph.read_started > graph.read_done ? graph.read_started - graph.read_done : 0;
    out->i2c_ok = i2c.ok;
    out->i2c_fail = i2c.fail;
    out->i2c_timeouts = i2c.timeouts;
    out->buffer_bytes = sharedBytes;
    out->buffer_physical_low = (uint32_t)sharedPhysical;
    out->buffer_physical_high = (uint32_t)(sharedPhysical >> 32);
    out->counter++;
    IOLockUnlock(stateLock);
    return 0;
}

int Tb710fuAudioDevice::setVolume(int32_t db)
{
    if (db > 0) db = 0;
    if (db < -96) db = -96;
    IOLockLock(stateLock);
    volumeDb = db;
    if (options.ampVolume) {
        for (uint32_t i = 0; i < kAmpCount; ++i) {
            if (!amp[i].present) continue;
            i2c.geni.address = amp[i].address;
            tb710fu_amp_volume(&amp[i], db);
        }
    }
    IOLockUnlock(stateLock);
    return 0;
}

int Tb710fuAudioDevice::setMute(uint32_t mute)
{
    IOLockLock(stateLock);
    muted = mute ? 1u : 0u;
    if (options.ampMute) {
        for (uint32_t i = 0; i < kAmpCount; ++i) {
            if (!amp[i].present) continue;
            i2c.geni.address = amp[i].address;
            tb710fu_amp_mute(&amp[i], (int)muted);
        }
    }
    IOLockUnlock(stateLock);
    return 0;
}

int Tb710fuAudioDevice::startStop(uint32_t mask, uint32_t on)
{
    IOLockLock(stateLock);
    streamMask = on ? (streamMask | mask) : (streamMask & ~mask);
    const int running = graph.step == Tb710fuGraphRunning ? 0 : -1;
    IOLockUnlock(stateLock);
    return running;
}

int Tb710fuAudioDevice::commitPlayback(uint32_t chunk)
{
    if (chunk >= TB710FU_AUDIO_CHUNKS || !sharedHeader || !shared) return -1;
    IOLockLock(stateLock);
    if (graph.step != Tb710fuGraphRunning || !graph.handle) {
        IOLockUnlock(stateLock);
        return -1;
    }
    int16_t *samples = (int16_t *)((char *)sharedHeader + TB710FU_AUDIO_SHARED_HEADER +
                                   chunk * TB710FU_AUDIO_CHUNK_BYTES);
    if (!muted) tb710fu_audio_apply_gain(samples, TB710FU_AUDIO_CHUNK_BYTES / 2, volumeDb);
    const uint64_t offset = chunk * TB710FU_AUDIO_CHUNK_BYTES;
    int rc = 0;
    if (tb710fu_graph_write(&graph, offset, TB710FU_AUDIO_CHUNK_BYTES, nowUs())) {
        rc = -1;
    } else {
        sharedHeader->produced = sharedHeader->produced + 1u;
        native_tb710fu_audio_stats[13] = (int32_t)graph.write_done;
    }
    IOLockUnlock(stateLock);
    return rc;
}

/* ------------------------------------ the entry point ---------------------------- */

// Keeps the metaclass registration constructor in this translation unit linked, the way the
// other TB710FU subsystems do (package/Tb710fuPower/Tb710fuBattery.cpp).
extern "C" void tb710fu_audio_meta_init(void) __asm__("__GLOBAL__sub_I_Tb710fuAudioService.cpp");

/* Called by Macos27Services.cpp, like the other subsystems' entry points. */
extern "C" void tb710fu_audio_start(void)
{
    uint32_t stage = 0;
    if (!PE_parse_boot_argn("audio", &stage, sizeof stage) || !stage) {
        printf("TB710FU audio: not requested (add audio=1..4)\n");
        return;
    }
    gTextLock = IOLockAlloc();
    Tb710fuAudioDevice *device = OSTypeAlloc(Tb710fuAudioDevice);
    if (!device || !device->init()) {
        if (device) device->release();
        printf("TB710FU audio: service did not initialise\n");
        return;
    }
    if (!device->attach(IOService::getPlatform()) || !device->setup()) {
        device->detach(IOService::getPlatform());
        device->release();
        printf("TB710FU audio: stage %u did not come up; see native_tb710fu_audio_stats and the text buffer\n", stage);
        return;
    }
    device->release();   // the platform's attachment keeps it
}
