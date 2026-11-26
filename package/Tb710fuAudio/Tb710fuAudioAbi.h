/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The contract between the TB710FU audio service in the kernel and the CoreAudio HAL plugin (or
 * any other user-space client) that talks to it.
 *
 * Shape of the path, and why it is not IOAudioFamily:
 *   coreaudiod loads a HAL driver plugin (.driver bundle) from /System/Library/Audio/Plug-Ins/HAL
 *   or /Library/Audio/Plug-Ins/HAL. That plugin does not need a kernel audio family: it finds our
 *   IOService in the IORegistry (provider class "Tb710fuAudioDevice", property "tb710fu-audio"
 *   naming this ABI's magic and version), then opens the user client this service publishes and
 *   speaks the commands below. On macOS 27 IOAudioFamily still exists as a kext, but it is closed
 *   to new drivers and every symbol of it would be an undefined link symbol for our archive, so
 *   this package deliberately carries none: Tb710fuAudioService.cpp is a plain IOService and
 *   IOUserClient pair. The service reports whether IOAudioFamily is present in the running
 *   kernel through OSMetaClass::getMetaClassWithName (a lookup, not a symbol), so a plugin can
 *   tell the two paths apart without guessing.
 *
 * The audio data does not cross the user client as copies per period: the service hands the
 * client a shared, physically contiguous region (clientMemoryForType), the client fills a chunk
 * and commits it with kTb710fuAudioCommitPlayback, and the service copies that chunk into the
 * DSP's shared-memory endpoint after applying the current gain. Capture works the other way:
 * the service fetches a captured chunk into the shared region and the client fetches it.
 *
 * Everything here is little endian and fixed width; the layout is asserted by
 * tests/tb710fu_audio_test.c so a plugin built against this header cannot drift. */
#ifndef TB710FU_AUDIO_ABI_H
#define TB710FU_AUDIO_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TB710FU_AUDIO_ABI_MAGIC 0x54413130u     /* "TA10" */
#define TB710FU_AUDIO_ABI_VERSION 3u
#define TB710FU_AUDIO_PROPERTY "tb710fu-audio"
#define TB710FU_AUDIO_PROVIDER_CLASS "Tb710fuAudioDevice"
#define TB710FU_AUDIO_UC_CLASS "Tb710fuAudioUserClient"

#define TB710FU_AUDIO_CHUNK_BYTES 4096u
#define TB710FU_AUDIO_CHUNKS 4u
#define TB710FU_AUDIO_FRAMES_PER_CHUNK (TB710FU_AUDIO_CHUNK_BYTES / 8u)   /* 4 channels, 16-bit */
#define TB710FU_AUDIO_SHARED_BYTES (TB710FU_AUDIO_CHUNK_BYTES * TB710FU_AUDIO_CHUNKS)
#define TB710FU_AUDIO_TEXT_BYTES 256u

/* The stage the service is at; the boot argument audio=N selects the ceiling. */
enum {
    Tb710fuAudioOff = 0,          /* audio=0 (default): nothing is touched */
    Tb710fuAudioI2cProbe = 1,     /* audio=1: GENI I2C up, the four AW88461 identified */
    Tb710fuAudioAmpInit = 2,      /* audio=2: the ACF profile replayed, control word written */
    Tb710fuAudioGraphOpen = 3,    /* audio=3: APM graphs open, configured, prepared, started */
    Tb710fuAudioStreaming = 4,    /* audio=4: buffer mapping done, writes and reads flowing */
};

enum {
    Tb710fuAudioNoError = 0,
    Tb710fuAudioErrorI2c = 1,
    Tb710fuAudioErrorAmp = 2,
    Tb710fuAudioErrorFirmware = 3,
    Tb710fuAudioErrorGpr = 4,
    Tb710fuAudioErrorGraph = 5,
    Tb710fuAudioErrorLink = 6,
    Tb710fuAudioErrorBuffer = 7,
    Tb710fuAudioErrorArgument = 8,
    Tb710fuAudioErrorTimeout = 9,
};

/* Status flags. */
enum {
    Tb710fuAudioFlagI2c = 1u << 0,
    Tb710fuAudioFlagAmpControl = 1u << 1,
    Tb710fuAudioFlagSpeakerGraph = 1u << 2,
    Tb710fuAudioFlagCaptureGraph = 1u << 3,
    Tb710fuAudioFlagMapped = 1u << 4,
    Tb710fuAudioFlagRunning = 1u << 5,
    Tb710fuAudioFlagMuted = 1u << 6,
    Tb710fuAudioFlagIOAudioFamily = 1u << 7,   /* the family's metaclass answered a lookup */
    Tb710fuAudioFlagAmpVolume = 1u << 8,       /* audio-amp-volume=1: the amp's own attenuation is in use */
};

/* The shared region's header; data chunks follow at shared + TB710FU_AUDIO_SHARED_HEADER. */
#define TB710FU_AUDIO_SHARED_HEADER 64u
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t bytes;          /* whole region, header included */
    uint32_t chunk_bytes;
    uint32_t chunks;
    volatile uint32_t produced;    /* chunks committed by the client (playback) */
    volatile uint32_t consumed;    /* chunks written to the DSP by the service */
    volatile uint32_t captured;    /* chunks filled by the service (capture) */
    volatile uint32_t fetched;     /* chunks taken by the client */
    uint32_t reserved[7];          /* keeps the header at TB710FU_AUDIO_SHARED_HEADER bytes */
} Tb710fuAudioSharedHeader;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t counter;          /* bumped when anything below changes */

    uint32_t state;            /* Tb710fuAudioOff .. Streaming */
    uint32_t requested_stage;  /* the boot argument's value */
    uint32_t flags;            /* Tb710fuAudioFlag... */
    uint32_t last_error;       /* Tb710fuAudioError... */

    uint32_t amps_present;
    uint32_t amps_loaded;
    uint32_t amps_muted;
    uint32_t amp_writes;
    uint32_t amp_write_errors;
    uint32_t amp_readback;
    uint32_t amp_readback_bad;
    uint32_t amp_identity[4];  /* the ACF profile identity word per speaker */

    uint32_t gpr_sent;
    uint32_t gpr_answers;
    uint32_t gpr_timeouts;
    uint32_t gpr_foreign;

    uint32_t speaker_rate;
    uint32_t speaker_bits;
    uint32_t speaker_channels;
    uint32_t capture_rate;
    uint32_t capture_bits;
    uint32_t capture_channels;

    uint32_t volume_db;        /* 0 .. -96 */
    uint32_t write_done;
    uint32_t write_busy;
    uint32_t read_done;
    uint32_t read_busy;

    uint32_t i2c_ok;
    uint32_t i2c_fail;
    uint32_t i2c_timeouts;

    uint32_t buffer_bytes;
    uint32_t buffer_physical_low;    /* the mapped region's physical address, for logs */
    uint32_t buffer_physical_high;
    uint32_t reserved[6];
} Tb710fuAudioStatus;

/* User client selectors. Scalar arguments travel in scalarInput[0..1]; the status struct is
 * copied out of structOutput. */
enum {
    kTb710fuAudioGetStatus = 0,
    kTb710fuAudioSetVolume = 1,        /* scalarInput[0]: dB, 0 .. -96 */
    kTb710fuAudioSetMute = 2,          /* scalarInput[0]: 1 mute, 0 unmute all speakers */
    kTb710fuAudioStart = 3,            /* scalarInput[0]: 1 playback, 2 capture, 3 both */
    kTb710fuAudioStop = 4,             /* scalarInput[0]: as Start */
    kTb710fuAudioCommitPlayback = 5,   /* scalarInput[0]: chunk index */
    kTb710fuAudioFetchCapture = 6,     /* scalarInput[0]: chunk index */
    kTb710fuAudioGetText = 7,          /* structOutput: TB710FU_AUDIO_TEXT_BYTES of log text */
    kTb710fuAudioCount = 8,
};

/* The gain a level in dB applies to one sample; the service uses the same arithmetic when it
 * copies a chunk into the DSP buffer. Pure function, exposed for the host test. */
int32_t tb710fu_audio_gain_ppm(int32_t db);

/* Applies the service gain to count interleaved 16-bit samples in place. */
void tb710fu_audio_apply_gain(int16_t *samples, uint32_t count, int32_t db);

#ifdef __cplusplus
}
#endif
#endif
