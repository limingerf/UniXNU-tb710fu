// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB710FU audio: the four Awinic AW88461 speakers and the DSP's playback and capture graphs, on
// macOS 27.3 / SM8650Q-AB. This file owns bring-up (I2C, amplifiers, the APM graphs, the shared
// buffer) and the run loop; Tb710fuAudioService.cpp owns the IOService + IOUserClient pair.
//
// Staged bring-up, selected by the boot argument audio=N (default 0, nothing is touched):
//   audio=1  GENI I2C on i2c_hub_4 comes up and the four parts at 0x34..0x37 are identified by
//            read-only register reads. Nothing is written.
//   audio=2  the stock ACF profile (awinic/aw88461_tb710fu.bin) is replayed into each part and
//            the part's control word is confirmed. No tones, no graph.
//   audio=3  the ADSP's GPR/APM answers, and the speaker and (optionally) microphone graphs are
//            opened, configured, prepared and started.
//   audio=4  a physically contiguous region is mapped into the DSP's shared-memory endpoint and
//            the service accepts chunks from its user client: audio actually flows.
// Every stage stops at the first answer it does not understand and reports the step number, so a
// hardware run bisects without a rebuild. The I2C and amplifier stages are complete work; the
// graph stage speaks the same AudioReach protocol the sheng stack runs on SM8550, with the
// TB710FU TDM frame (4 slots of 32 bits, from the board DTS) as its only platform input.
//
// What this file does not do: it never writes an AW88461 control bit that the stock ACF did not
// write, unless the owner asks with audio-amp-mute=1 (the family's HIZ bit) or
// audio-amp-volume=1 (the 6-bit attenuation in 0x0004). By default mute and volume are applied
// where they are provable: the fader on the copy into the DSP buffer (Tb710fuAudioGain.c).
//
// No IOAudioFamily: the service is an IOService plus an IOUserClient (Tb710fuAudioAbi.h), and the
// family is only looked up by name to report whether it is present. See docs/AUDIO_AW88461_ADSP.md.
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "Tb710fuAudioService.h"

// [0] stage reached, [1] requested stage, [2] amps present, [3] amps loaded, [4] amp writes,
// [5] amp write errors | readback bad << 16, [6] GPR packets sent, [7] answers, [8] timeouts,
// [9] graph step, [10] last status, [11] I2C transfers ok, [12] I2C failures, [13] write completions,
// [14] read completions, [15] last error | flags << 16
extern "C" {
volatile int32_t native_tb710fu_audio_stats[16];
volatile int32_t native_tb710fu_audio_pins[4];   // TLMM GPIO_CFG of the four reset lines, as read
char native_tb710fu_audio_text[TB710FU_AUDIO_TEXT_BYTES];
}

IOLock *gTextLock = nullptr;

void tb710fu_audio_note(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (gTextLock) IOLockLock(gTextLock);
    vsnprintf(native_tb710fu_audio_text, sizeof native_tb710fu_audio_text, format, args);
    if (gTextLock) IOLockUnlock(gTextLock);
    va_end(args);
}

/* ------------------------------ programmable I2C ------------------------------ */

bool Tb710fuAudioGeniChannel::mapRegisters()
{
    IOMemoryDescriptor *descriptor =
        IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress)kI2cBase, kI2cBytes, kIODirectionInOut);
    if (!descriptor) return false;
    map = descriptor->map(kIOMapAnywhere | kIOMapInhibitCache);
    descriptor->release();
    return map != nullptr;
}

uint32_t Tb710fuAudioGeniChannel::readRegister(void *context, uint32_t offset)
{
    Tb710fuAudioGeniChannel *self = static_cast<Tb710fuAudioGeniChannel *>(context);
    uint8_t *base = (uint8_t *)self->map->getVirtualAddress();
    // Aligned 32-bit access only: byte-granular loads on this MMIO have produced ESR 0x96000061.
    return *(volatile uint32_t *)(base + (offset & ~3u));
}

void Tb710fuAudioGeniChannel::writeRegister(void *context, uint32_t offset, uint32_t value)
{
    Tb710fuAudioGeniChannel *self = static_cast<Tb710fuAudioGeniChannel *>(context);
    uint8_t *base = (uint8_t *)self->map->getVirtualAddress();
    *(volatile uint32_t *)(base + (offset & ~3u)) = value;
}

int Tb710fuAudioGeniChannel::finishTransfer()
{
    // ~2 ms at 10 us per poll; the engine's own cancel/abort sequence is what ends a stuck command.
    for (uint32_t waited = 0; waited < kI2cTimeoutUs / 10; ++waited) {
        const int status = tb710fu_geni_poll(&geni);
        if (status == Tb710fuGeniDone) { ok++; return 0; }
        if (status == Tb710fuGeniFailed) {
            fail++;
            native_tb710fu_audio_stats[12] = (int32_t)fail;
            return -1;
        }
        IODelay(10);
    }
    timeouts++;
    native_tb710fu_audio_stats[12] = (int32_t)fail;
    // Cancel, then abort, then give up with the engine idle so the next transfer can start.
    for (uint32_t i = 0; i < 64; ++i) {
        const int status = tb710fu_geni_timeout(&geni);
        if (status == Tb710fuGeniFailed) { fail++; return -1; }
        IODelay(10);
    }
    geni.phase = Tb710fuGeniIdle;
    fail++;
    return -1;
}

int Tb710fuAudioGeniChannel::write(uint8_t address, const uint8_t *bytes, uint32_t length)
{
    if (!map || tb710fu_geni_busy(&geni)) return -1;
    geni.address = address;
    if (tb710fu_geni_write(&geni, bytes, length)) return -1;
    return finishTransfer();
}

int Tb710fuAudioGeniChannel::read(uint8_t address, const uint8_t *command, uint32_t commandLength, uint8_t *to,
                                  uint32_t readLength)
{
    if (!map || !to || tb710fu_geni_busy(&geni)) return -1;
    geni.address = address;
    if (tb710fu_geni_read(&geni, command, commandLength, readLength)) return -1;
    if (finishTransfer()) return -1;
    if (geni.rx_filled < readLength || geni.data_bytes < readLength) return -1;
    for (uint32_t i = 0; i < readLength; ++i) to[i] = geni.rx[i];
    return 0;
}

// The amplifier's register convention: 16-bit register address, 16-bit value, most significant
// byte first on the wire (the AW88xx family's I2C register format; the ACF's pairs are 16-bit
// little-endian values in the file, this is the wire order).
static int ampWrite(void *context, uint16_t address, uint16_t value)
{
    Tb710fuAudioGeniChannel *channel = static_cast<Tb710fuAudioGeniChannel *>(context);
    const uint8_t bytes[4] = {(uint8_t)(address >> 8), (uint8_t)address, (uint8_t)(value >> 8), (uint8_t)value};
    return channel->write(channel->geni.address, bytes, 4);
}

static int ampRead(void *context, uint16_t address, uint16_t *value)
{
    Tb710fuAudioGeniChannel *channel = static_cast<Tb710fuAudioGeniChannel *>(context);
    const uint8_t command[2] = {(uint8_t)(address >> 8), (uint8_t)address};
    uint8_t bytes[2] = {0, 0};
    if (channel->read(channel->geni.address, command, 2, bytes, 2)) return -1;
    *value = (uint16_t)((uint16_t)bytes[0] << 8 | bytes[1]);
    return 0;
}

/* ------------------------------ link callbacks ------------------------------- */

void Tb710fuAudioDevice::linkState(void *context, int up)
{
    Tb710fuAudioDevice *self = static_cast<Tb710fuAudioDevice *>(context);
    __atomic_store_n(&self->linkUp, up ? 1u : 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&self->linkChanged, 1u, __ATOMIC_RELEASE);
}

void Tb710fuAudioDevice::linkReceive(void *context, const uint8_t *data, uint32_t length)
{
    Tb710fuAudioDevice *self = static_cast<Tb710fuAudioDevice *>(context);
    if (!data || !length || length > kMessage) return;
    IOSimpleLockLock(self->ringLock);
    if (self->ringCount < kRing) {
        auto &slot = self->ring[(self->ringHead + self->ringCount) % kRing];
        memcpy(slot.data, data, length);
        slot.length = length;
        self->ringCount++;
    }
    IOSimpleLockUnlock(self->ringLock);
}

void Tb710fuAudioDevice::onTick(OSObject *owner, IOTimerEventSource *sender)
{
    Tb710fuAudioDevice *self = static_cast<Tb710fuAudioDevice *>(owner);
    if (!self) return;
    self->tickOnce();
    if (sender) sender->setTimeoutMS(kTickMs);
}

/* ------------------------------ bring-up stages ------------------------------ */

bool Tb710fuAudioDevice::bringUpI2c()
{
    if (!i2c.mapRegisters()) {
        tb710fu_audio_note("i2c: i2c_hub_4 at 0x%08x did not map\n", kI2cBase);
        lastError = Tb710fuAudioErrorI2c;
        return false;
    }
    const Tb710fuGeniIo io = {&Tb710fuAudioGeniChannel::readRegister, &Tb710fuAudioGeniChannel::writeRegister, &i2c};
    const int prepared = tb710fu_geni_prepare(&i2c.geni, &io, TB710FU_AMP_ADDRESS_FIRST, 1 /* master hub */);
    printf("TB710FU audio: i2c_hub_4 proto=%u clk=0x%x counters=0x%x fifo=%u/%u prepare=%d\n", i2c.geni.proto,
           i2c.geni.clock_config, i2c.geni.timing_counters, i2c.geni.tx_fifo_words, i2c.geni.rx_fifo_words,
           prepared);
    tb710fu_audio_note("i2c proto=%u clk=0x%x fifo=%u/%u\n", i2c.geni.proto, i2c.geni.clock_config,
                       i2c.geni.tx_fifo_words, i2c.geni.rx_fifo_words);
    if (prepared != Tb710fuGeniOk) {
        lastError = Tb710fuAudioErrorI2c;
        return false;
    }
    return true;
}

bool Tb710fuAudioDevice::loadAcf()
{
    if (!tb710fu_fw_open) {
        tb710fu_audio_note("acf: no firmware reader in this kernel\n");
        return false;
    }
    void *handle = nullptr;
    uint64_t size = 0;
    if (tb710fu_fw_open(options.acfPath, &handle, &size) || !handle || !size) {
        tb710fu_audio_note("acf: %s not readable\n", options.acfPath);
        return false;
    }
    if (size > 64 * 1024) { tb710fu_fw_close(handle); return false; }
    acfBytes = (uint8_t *)IOMalloc((vm_size_t)size);
    if (!acfBytes) { tb710fu_fw_close(handle); return false; }
    if (tb710fu_fw_read(handle, 0, acfBytes, (size_t)size)) {
        tb710fu_fw_close(handle);
        tb710fu_audio_note("acf: read failed\n");
        return false;
    }
    tb710fu_fw_close(handle);
    acfLength = (uint32_t)size;
    char why[48] = {0};
    const int opened = tb710fu_acf_open(&acf, acfBytes, acfLength, why, sizeof why);
    if (opened != Tb710fuAcfOk) {
        tb710fu_audio_note("acf: refused (%d: %s)\n", opened, why);
        lastError = Tb710fuAudioErrorFirmware;
        return false;
    }
    printf("TB710FU audio: acf %u bytes project=%s part=%s records=%u profiles=%u image=0x%x\n", acfLength,
           acf.project, acf.record[0].model, acf.record_count, acf.profile_count, acf.image_id);
    tb710fu_audio_note("acf %uB %s/%s rec=%u\n", acfLength, acf.project, acf.record[0].model, acf.record_count);
    return true;
}

bool Tb710fuAudioDevice::bringUpAmps()
{
    ampBus.write = &ampWrite;
    ampBus.read = &ampRead;
    ampBus.context = &i2c;
    // The ACF's per-profile identity word is what the probe records next to the live reads, so
    // the profile file is opened before the parts are identified when this stage needs it.
    if (options.stage >= Tb710fuAudioAmpInit && !acf.record_count && !loadAcf()) return false;
    uint32_t present = 0, loaded = 0;
    for (uint32_t i = 0; i < kAmpCount; ++i) {
        const uint8_t address = (uint8_t)(TB710FU_AMP_ADDRESS_FIRST + i);
        const Tb710fuAcfRecord *record = acf.record_count ? tb710fu_acf_find(&acf, (uint16_t)i, 0) : nullptr;
        const uint32_t identity = record ? record->identity : 0;
        i2c.geni.address = address;
        if (tb710fu_amp_probe(&amp[i], &ampBus, address, (uint8_t)i, identity) == 0) {
            present++;
            printf("TB710FU audio: amp %u at 0x%02x identity=0x%x reads=%04x %04x %04x %04x\n", i, address, identity,
                   amp[i].probe[0], amp[i].probe[1], amp[i].probe[2], amp[i].probe[3]);
        } else {
            printf("TB710FU audio: amp %u at 0x%02x silent (reads=%04x %04x %04x %04x errors=%u)\n", i, address,
                   amp[i].probe[0], amp[i].probe[1], amp[i].probe[2], amp[i].probe[3], amp[i].read_errors);
        }
    }
    native_tb710fu_audio_stats[2] = (int32_t)present;
    tb710fu_audio_note("amps %u/%u present\n", present, kAmpCount);
    if (!present) {
        lastError = Tb710fuAudioErrorAmp;
        return false;
    }
    if (options.stage < Tb710fuAudioAmpInit) {
        stage = Tb710fuAudioI2cProbe;
        return true;
    }
    for (uint32_t i = 0; i < kAmpCount; ++i) {
        if (!amp[i].present) continue;
        const Tb710fuAcfRecord *record = tb710fu_acf_find(&acf, (uint16_t)i, 0);
        if (!record) {
            tb710fu_audio_note("acf: no profile for channel %u\n", i);
            continue;
        }
        i2c.geni.address = amp[i].address;
        if (tb710fu_amp_load(&amp[i], &acf, record, 8) == 0) {
            loaded++;
            printf("TB710FU audio: amp %u loaded %u pairs, readback %u/%u ok\n", i, amp[i].pairs,
                   amp[i].readback - amp[i].readback_bad, amp[i].readback);
        } else {
            printf("TB710FU audio: amp %u load failed after %u pairs (%u write errors, %u readback bad)\n", i,
                   amp[i].pairs, amp[i].pair_errors, amp[i].readback_bad);
        }
    }
    native_tb710fu_audio_stats[3] = (int32_t)loaded;
    native_tb710fu_audio_stats[4] = (int32_t)amp[0].writes;
    native_tb710fu_audio_stats[5] = (int32_t)(amp[0].write_errors | amp[0].readback_bad << 16);
    if (!loaded) {
        lastError = Tb710fuAudioErrorAmp;
        return false;
    }
    if (options.ampMute) {
        for (uint32_t i = 0; i < kAmpCount; ++i)
            if (amp[i].present) tb710fu_amp_mute(&amp[i], 0);
    }
    stage = Tb710fuAudioAmpInit;
    return true;
}

bool Tb710fuAudioDevice::bringUpGraph()
{
    if (!tb710fu_power_link_acquire || !tb710fu_power_link_send) {
        tb710fu_audio_note("graph: no ADSP transport in this kernel\n");
        lastError = Tb710fuAudioErrorLink;
        return false;
    }
    const Tb710fuPowerLinkClient client = {&Tb710fuAudioDevice::linkState, &Tb710fuAudioDevice::linkReceive};
    link = tb710fu_power_link_acquire(kChannel, kMessage, 4, &client, this);
    if (!link) {
        tb710fu_audio_note("graph: adsp_apps not registered (add adsp=2)\n");
        lastError = Tb710fuAudioErrorLink;
        return false;
    }
    tb710fu_gpr_init(&gpr, options.gprSrc, options.gprDst, TB710FU_GPR_PORT_APM);
    graphConfig.speaker.rate = (uint16_t)options.rate;
    graphConfig.speaker.bits = (uint16_t)options.speakerBits;
    graphConfig.speaker.channels = (uint16_t)options.speakerChannels;
    graphConfig.capture.rate = (uint16_t)options.rate;
    graphConfig.capture.bits = (uint16_t)options.captureBits;
    graphConfig.capture.channels = (uint16_t)options.captureChannels;
    graphConfig.tdm.lpaif = 0;
    graphConfig.tdm.interface = options.tdmInterface;
    graphConfig.tdm.sd_line = options.tdmSd;
    graphConfig.tdm.ws_source = options.tdmWs;
    graphConfig.tdm.slots = options.speakerChannels;
    graphConfig.tdm.slot_bits = options.speakerBits;
    graphConfig.tdm.channels = options.speakerChannels;
    graphConfig.tdm.send_tdm_parameter = (int)options.tdmParam;
    graphConfig.mic.lpaif = 1;
    graphConfig.mic.codec_dma_index = 4;   // TX_CODEC_DMA_TX_3
    graphConfig.mic.channel_mask = options.captureChannels > 1 ? 0x3u : 0x1u;
    graphConfig.open_capture = 1;
    graphConfig.map_buffer = options.stage >= Tb710fuAudioStreaming;
    graphConfig.buffer_address = sharedPhysical;
    graphConfig.buffer_bytes = sharedBytes;
    const Tb710fuAudioPort port = {&Tb710fuAudioDevice::portSend, this};
    tb710fu_graph_init(&graph, &port, &graphConfig);
    tb710fu_audio_note("graph armed spk=%uch@%u/%ubit tdm intf=%u sd=%u ws=%u cap=%uch gpr=%u->%u\n",
                       options.speakerChannels, options.rate, options.speakerBits, options.tdmInterface,
                       options.tdmSd, options.tdmWs, options.captureChannels, options.gprSrc, options.gprDst);
    stage = Tb710fuAudioGraphOpen;
    return true;
}

bool Tb710fuAudioDevice::mapBuffer()
{
    const uint32_t bytes = TB710FU_AUDIO_SHARED_HEADER + TB710FU_AUDIO_SHARED_BYTES;
    shared = IOBufferMemoryDescriptor::withOptions(kIODirectionOutIn | kIOMemoryPhysicallyContiguous,
                                                  (vm_size_t)bytes, (vm_size_t)PAGE_SIZE);
    if (!shared) {
        tb710fu_audio_note("buffer: no contiguous %u bytes\n", bytes);
        lastError = Tb710fuAudioErrorBuffer;
        return false;
    }
    IOPhysicalAddress physical = 0;
    if (shared->getPhysicalSegment(0, &physical, 0) != (IOByteCount)bytes) {
        tb710fu_audio_note("buffer: the region is not contiguous\n");
        lastError = Tb710fuAudioErrorBuffer;
        return false;
    }
    sharedBytes = bytes;
    sharedPhysical = (uint64_t)physical;
    sharedHeader = (Tb710fuAudioSharedHeader *)shared->getBytesNoCopy();
    memset(sharedHeader, 0, sizeof *sharedHeader);
    char *data = (char *)sharedHeader + TB710FU_AUDIO_SHARED_HEADER;
    memset(data, 0, TB710FU_AUDIO_SHARED_BYTES);
    sharedHeader->magic = TB710FU_AUDIO_ABI_MAGIC;
    sharedHeader->version = TB710FU_AUDIO_ABI_VERSION;
    sharedHeader->bytes = bytes;
    sharedHeader->chunk_bytes = TB710FU_AUDIO_CHUNK_BYTES;
    sharedHeader->chunks = TB710FU_AUDIO_CHUNKS;
    tb710fu_audio_note("buffer %u bytes at 0x%llx\n", bytes, sharedPhysical);
    return true;
}

/* ---------------------------------- run loop --------------------------------- */

int Tb710fuAudioDevice::portSend(void *context, const uint8_t *packet, uint32_t bytes)
{
    Tb710fuAudioDevice *self = static_cast<Tb710fuAudioDevice *>(context);
    return self ? self->pushPacket(packet, bytes) : -1;
}

uint64_t Tb710fuAudioDevice::nowUs()
{
    uint64_t nanoseconds = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
    return nanoseconds / 1000u;
}

int Tb710fuAudioDevice::pushPacket(const uint8_t *packet, uint32_t bytes)
{
    if (!link || !tb710fu_power_link_send) return -1;
    return tb710fu_power_link_send(link, packet, bytes) ? -1 : 0;
}

void Tb710fuAudioDevice::serviceRing()
{
    for (;;) {
        uint32_t length = 0;
        uint8_t packet[kMessage];
        IOSimpleLockLock(ringLock);
        if (!ringCount) { IOSimpleLockUnlock(ringLock); return; }
        auto &slot = ring[ringHead];
        length = slot.length;
        memcpy(packet, slot.data, length);
        ringHead = (ringHead + 1) % kRing;
        ringCount--;
        IOSimpleLockUnlock(ringLock);
        if (tb710fu_graph_packet(&graph, packet, length) < 0) {
            lastError = Tb710fuAudioErrorGraph;
            native_tb710fu_audio_stats[9] = (int32_t)graph.step;
            native_tb710fu_audio_stats[10] = (int32_t)graph.last_status;
            printf("TB710FU audio: graph stopped at step %u status 0x%x\n", graph.step, graph.last_status);
            tb710fu_audio_note("graph stopped at step %u status 0x%x\n", graph.step, graph.last_status);
        } else {
            native_tb710fu_audio_stats[9] = (int32_t)graph.step;
            native_tb710fu_audio_stats[10] = (int32_t)graph.last_status;
        }
    }
}

void Tb710fuAudioDevice::tickOnce()
{
    const uint64_t now = nowUs();
    IOLockLock(stateLock);
    native_tb710fu_audio_stats[1] = (int32_t)options.stage;
    if (link) {
        serviceRing();
        if (graph.step != Tb710fuGraphFailed) {
            (void)tb710fu_graph_timeout(&graph, now);
            (void)tb710fu_graph_pump(&graph, now);
        }
        if (graph.step == Tb710fuGraphFailed) {
            lastError = graph.last_error ? graph.last_error : Tb710fuAudioErrorGraph;
            native_tb710fu_audio_stats[15] = (int32_t)lastError;
            static uint32_t reportedStep = 0xffffffffu;
            if (reportedStep != graph.step) {
                reportedStep = graph.step;
                printf("TB710FU audio: graph did not pass its step (status 0x%x, timeouts %u)\n", graph.last_status,
                       graph.timeouts);
                tb710fu_audio_note("graph failed status=0x%x timeouts=%u\n", graph.last_status, graph.timeouts);
            }
        }
        native_tb710fu_audio_stats[6] = (int32_t)graph.gpr.sent;
        native_tb710fu_audio_stats[7] = (int32_t)graph.answers;
        native_tb710fu_audio_stats[8] = (int32_t)graph.timeouts;
        native_tb710fu_audio_stats[13] = (int32_t)graph.write_done;
        native_tb710fu_audio_stats[14] = (int32_t)graph.read_done;
        if (graph.step == Tb710fuGraphRunning)
            stage = graph.config.map_buffer ? Tb710fuAudioStreaming : Tb710fuAudioGraphOpen;
    }
    IOLockUnlock(stateLock);
    native_tb710fu_audio_stats[0] = (int32_t)stage;
}

/* ------------------------------------ setup ---------------------------------- */

void tb710fu_audio_parse_options(Tb710fuAudioBootOptions *o)
{
    if (!o) return;
    memset(o, 0, sizeof *o);
    uint32_t value = 0;
    o->rate = 48000;
    o->speakerBits = 32;
    o->speakerChannels = 4;      // dai-tdm-slot-num
    o->captureBits = 16;
    o->captureChannels = 2;
    o->tdmInterface = 1;         // secondary: SECONDARY_TDM_RX_0
    o->tdmSd = 0;
    o->tdmWs = 1;                // internal
    o->tdmParam = 0;
    o->gprDst = TB710FU_GPR_DOMAIN_ADSP;
    o->gprSrc = TB710FU_GPR_DOMAIN_APPS;
    strlcpy(o->acfPath, kAcfDefault, sizeof o->acfPath);
    if (!PE_parse_boot_argn("audio", &value, sizeof value)) return;   // default off
    o->stage = value > Tb710fuAudioStreaming ? Tb710fuAudioStreaming : value;
    if (PE_parse_boot_argn("audio-rate", &value, sizeof value) && value >= 8000 && value <= 192000) o->rate = value;
    if (PE_parse_boot_argn("audio-speaker-bits", &value, sizeof value) && (value == 16 || value == 32))
        o->speakerBits = value;
    if (PE_parse_boot_argn("audio-speaker-channels", &value, sizeof value) && value >= 1 && value <= 8)
        o->speakerChannels = value;
    if (PE_parse_boot_argn("audio-capture-bits", &value, sizeof value) && (value == 16 || value == 32))
        o->captureBits = value;
    if (PE_parse_boot_argn("audio-capture-channels", &value, sizeof value) && value >= 1 && value <= 8)
        o->captureChannels = value;
    if (PE_parse_boot_argn("audio-amp-mute", &value, sizeof value)) o->ampMute = value ? 1 : 0;
    if (PE_parse_boot_argn("audio-amp-volume", &value, sizeof value)) o->ampVolume = value ? 1 : 0;
    if (PE_parse_boot_argn("audio-tdm-intf", &value, sizeof value)) o->tdmInterface = value;
    if (PE_parse_boot_argn("audio-tdm-sd", &value, sizeof value)) o->tdmSd = value;
    if (PE_parse_boot_argn("audio-tdm-ws", &value, sizeof value)) o->tdmWs = value;
    if (PE_parse_boot_argn("audio-tdm-param", &value, sizeof value)) o->tdmParam = value ? 1 : 0;
    if (PE_parse_boot_argn("audio-gpr-dst", &value, sizeof value)) o->gprDst = value;
    if (PE_parse_boot_argn("audio-gpr-src", &value, sizeof value)) o->gprSrc = value;
    if (PE_parse_boot_argn("audio-acf", o->acfPath, sizeof o->acfPath)) { /* keep the owner's path */ }
}

// One line per boot about the device tree nodes this driver depends on. Read-only.
static void survey()
{
    struct Node { const char *path; const char *what; };
    const Node nodes[] = {
        {"/soc/i2c@990000", "i2c_hub_4 (AW88461 bus)"},
        {"/soc/qcom,gpr", "GPR (adsp_apps)"},
        {"/soc/lpass-cpu", "LPASS audio routing"},
        {"/soc/pinctrl@f100000", "TLMM (amp reset lines)"},
    };
    for (unsigned i = 0; i < sizeof nodes / sizeof nodes[0]; ++i) {
        DTEntry entry = nullptr;
        if (SecureDTLookupEntry(nullptr, nodes[i].path, &entry) == kSuccess && entry)
            printf("TB710FU audio: ADT %s present: %s\n", nodes[i].what, nodes[i].path);
        else
            printf("TB710FU audio: ADT %s not at %s\n", nodes[i].what, nodes[i].path);
    }
    IOMemoryDescriptor *tlmm = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress)kTlmmBase, kTlmmBytes,
                                                                     kIODirectionIn);
    IOMemoryMap *map = tlmm ? tlmm->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (map) {
        uint8_t *base = (uint8_t *)map->getVirtualAddress();
        for (uint32_t i = 0; i < kAmpCount; ++i) {
            const uint32_t cfg = *(volatile uint32_t *)(base + kResetPin[i] * kTlmmPinStride);
            native_tb710fu_audio_pins[i] = (int32_t)cfg;
            printf("TB710FU audio: reset line amp %u: TLMM %u cfg=0x%08x\n", i, kResetPin[i], cfg);
        }
        map->release();
    }
    if (tlmm) tlmm->release();
}

bool Tb710fuAudioDevice::setup()
{
    IOLock *lock = IOLockAlloc();
    if (!lock) return false;
    stateLock = lock;
    stage = Tb710fuAudioOff;
    tb710fu_audio_parse_options(&options);
    native_tb710fu_audio_stats[1] = (int32_t)options.stage;
    if (!options.stage) {
        printf("TB710FU audio: not requested (add audio=1..4)\n");
        return false;
    }
    printf("TB710FU audio: stage=%u spk=%uch/%ubit@%u tdm if=%u sd=%u ws=%u cap=%uch/%ubit amp-mute=%u amp-volume=%u\n",
           options.stage, options.speakerChannels, options.speakerBits, options.rate, options.tdmInterface, options.tdmSd,
           options.tdmWs, options.captureChannels, options.captureBits, options.ampMute, options.ampVolume);
    survey();
    ringLock = IOSimpleLockAlloc();
    loop = IOWorkLoop::workLoop();
    tick = IOTimerEventSource::timerEventSource(this, &Tb710fuAudioDevice::onTick);
    if (!ringLock || !loop || !tick || loop->addEventSource(tick) != kIOReturnSuccess) return false;
    if (!bringUpI2c()) return false;
    if (!bringUpAmps()) return false;
    if (options.stage >= Tb710fuAudioGraphOpen) {
        if (options.stage >= Tb710fuAudioStreaming && !mapBuffer()) return false;
        if (!bringUpGraph()) return false;
        tick->setTimeoutMS(kTickMs);
    }
    stage = options.stage == Tb710fuAudioStreaming ? Tb710fuAudioGraphOpen : stage;
    native_tb710fu_audio_stats[0] = (int32_t)stage;
    {
        // Is IOAudioFamily in this kernel? A name lookup, not a symbol: this archive stays free of
        // the family's link symbols either way, and a HAL plugin can read the answer from status.
        OSSymbol *name = OSSymbol::withCString("IOAudioDevice");
        if (name) {
            ioAudioFamily = OSMetaClass::getMetaClassWithName(name) != nullptr;
            name->release();
        }
        printf("TB710FU audio: IOAudioFamily device class %s; serving through IOService + IOUserClient\n",
               ioAudioFamily ? "present" : "absent");
    }
    OSNumber *magic = OSNumber::withNumber((unsigned long long)TB710FU_AUDIO_ABI_MAGIC, 32);
    if (magic) { setProperty(TB710FU_AUDIO_PROPERTY, magic); magic->release(); }
    OSNumber *stageNumber = OSNumber::withNumber((unsigned long long)stage, 32);
    if (stageNumber) { setProperty("tb710fu-audio-stage", stageNumber); stageNumber->release(); }
    OSString *format = OSString::withCString("speaker: 4ch 32-bit 48 kHz TDM secondary; capture: 2ch 16-bit 48 kHz TX3");
    if (format) { setProperty("tb710fu-audio-format", format); format->release(); }
    tb710fu_audio_note("stage %u up\n", stage);
    return true;
}
