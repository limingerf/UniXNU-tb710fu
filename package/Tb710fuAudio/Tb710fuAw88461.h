/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The Awinic AW88461 smart amplifier, as TB710FU wires it: four parts on i2c_hub_4 at 0x34..0x37
 * (sm8650-lenovo-tb710fu.dts: awinic_1@34 .. awinic_4@37, compatible "awinic,aw88461",
 * awinic,audio-channel 0..3, sound-name-prefix AMP0..AMP3, firmware-name
 * "awinic/aw88461_tb710fu.bin"). Each part takes one slot of the "Secondary TDM0 Playback" link
 * (dai-tdm-slot-num 4, dai-tdm-slot-width 32, dai-tdm-slot-rx-mask <1 1 1 1>).
 *
 * This module is the part's logic only: it never touches a bus register file itself, it calls the
 * caller's 16-bit register read/write (Tb710fuGeniI2c over the hub channel in the driver). That
 * keeps the probe/init/volume/mute sequencing testable on a host with a fake register file.
 *
 * Register semantics used here, and where they come from:
 *  - the init sequence is the stock ACF profile: Tb710fuAcf.c replays it in order, exactly as
 *    shipped (7ba6173fedee5eaea1a1e41a2164bb88). Its first write is 0x0003 <- 0xFFFF.
 *  - mute/unmute patch that same control word: the ACF's 0xFFFF switches every block of the part
 *    on, and bit 15 is the family's HIZ/standby bit, so standby is 0x7FFF. This bit is the one
 *    register semantic that the file cannot prove; the driver only sends it when the boot
 *    argument audio-amp-mute=1 asks for it (see docs/AUDIO_AW88461_ADSP.md).
 *  - volume is the part's 6-bit digital attenuation in the word at 0x0004, bits 13:8, which the
 *    datasheet's "-96 dB .. 0 dB" range fits at 1.5 dB per step (0x3F = 0 dB). Same status: off
 *    unless audio-amp-volume=1, because the stock ACF's 0x0004 words are channel tuning, not a
 *    0 dB default.
 *  - the probe reads 0x0000..0x0003 and calls a part present when three of the four reads answer
 *    and at least one value is neither 0xFFFF (a floating bus) nor zero. The ACF's per-profile
 *    identity word is recorded with it as the board's identity evidence. */
#ifndef TB710FU_AW88461_H
#define TB710FU_AW88461_H

#include <stdint.h>

#include "Tb710fuAcf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TB710FU_AMP_COUNT 4u
#define TB710FU_AMP_ADDRESS_FIRST 0x34u     /* awinic_1@34; the DTS gives 0x34..0x37 */
#define TB710FU_AMP_CHANNELS 4u
#define TB710FU_AMP_PROBE_REGISTERS 4u
#define TB710FU_AMP_REG_CONTROL 0x0003u     /* the ACF's first write: the part's control word */
#define TB710FU_AMP_CONTROL_ACTIVE 0xFFFFu
#define TB710FU_AMP_CONTROL_STANDBY 0x7FFFu /* bit 15 cleared: HIZ/standby */
#define TB710FU_AMP_REG_VOLUME 0x0004u
#define TB710FU_AMP_VOLUME_SHIFT 8
#define TB710FU_AMP_VOLUME_MASK 0x3Fu
#define TB710FU_AMP_VOLUME_0DB 0x3Fu
#define TB710FU_AMP_VOLUME_MIN_DB (-96)
#define TB710FU_AMP_VOLUME_STEP_DB_X10 15   /* 0.15 dB * 10 = 1.5 dB per step */
#define TB710FU_AMP_MAX_WRITE_ERRORS 8u

typedef struct {
    /* 16-bit register address, 16-bit value: returns 0 on success. */
    int (*write)(void *context, uint16_t address, uint16_t value);
    int (*read)(void *context, uint16_t address, uint16_t *value);
    void *context;
} Tb710fuAmpBus;

typedef struct {
    uint8_t address;
    uint8_t channel;
    uint8_t present;
    uint8_t muted;
    uint8_t control_written;   /* the ACF's control word is in the part */
    uint16_t probe[TB710FU_AMP_PROBE_REGISTERS];
    uint32_t identity;         /* the ACF profile's identity word */
    uint32_t reads, read_errors;
    uint32_t writes, write_errors;
    uint32_t pairs, pair_errors;
    uint32_t readback, readback_bad;
    int32_t volume_db;
    Tb710fuAmpBus bus;         /* copied by the probe; load/mute/volume use it */
} Tb710fuAmp;

/* Reads the identity registers of one part. Returns 0 when the part answered (amp->present set),
 * -1 when it did not; the raw reads stay in amp->probe either way. */
int tb710fu_amp_probe(Tb710fuAmp *amp, const Tb710fuAmpBus *bus, uint8_t address, uint8_t channel,
                      uint32_t identity);

/* Replays the profile's register pairs in order. verify_every > 0 reads back every N-th written
 * pair and counts differences. Returns 0 when every write was accepted, -1 otherwise. */
int tb710fu_amp_load(Tb710fuAmp *amp, const Tb710fuAcf *acf, const Tb710fuAcfRecord *record,
                     uint32_t verify_every);

/* Writes the control word active (unmute) or standby (mute). Returns 0 on success. */
int tb710fu_amp_mute(Tb710fuAmp *amp, int mute);

/* The 6-bit attenuation word for a level in dB (-96..0); pure arithmetic, exposed for the test. */
uint16_t tb710fu_amp_volume_word(int32_t db);

/* Read-modify-writes the attenuation field of the volume word. Returns 0 on success. */
int tb710fu_amp_volume(Tb710fuAmp *amp, int32_t db);

#ifdef __cplusplus
}
#endif
#endif
