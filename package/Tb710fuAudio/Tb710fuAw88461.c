/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Tb710fuAw88461.h"

#include <stdbool.h>

static int amp_write(Tb710fuAmp *amp, uint16_t address, uint16_t value)
{
    if (!amp->bus.write) return -1;
    const int rc = amp->bus.write(amp->bus.context, address, value);
    if (rc) amp->write_errors++;
    else amp->writes++;
    return rc;
}

static int amp_read(Tb710fuAmp *amp, uint16_t address, uint16_t *value)
{
    if (!amp->bus.read || !value) return -1;
    const int rc = amp->bus.read(amp->bus.context, address, value);
    if (rc) amp->read_errors++;
    else amp->reads++;
    return rc;
}

int tb710fu_amp_probe(Tb710fuAmp *amp, const Tb710fuAmpBus *bus, uint8_t address, uint8_t channel,
                      uint32_t identity)
{
    if (!amp || !bus || !bus->read) return -1;
    for (unsigned i = 0; i < sizeof *amp; ++i) ((uint8_t *)amp)[i] = 0;
    amp->bus = *bus;
    amp->address = address;
    amp->channel = channel;
    amp->identity = identity;
    uint32_t answered = 0;
    bool plausible = false;
    for (uint16_t reg = 0; reg < TB710FU_AMP_PROBE_REGISTERS; ++reg) {
        uint16_t value = 0xFFFFu;
        if (amp_read(amp, reg, &value) == 0) {
            answered++;
            if (value != 0xFFFFu && value != 0x0000u) plausible = true;
        }
        amp->probe[reg] = value;
    }
    if (answered >= TB710FU_AMP_PROBE_REGISTERS - 1 && plausible) {
        amp->present = 1;
        return 0;
    }
    return -1;
}

int tb710fu_amp_load(Tb710fuAmp *amp, const Tb710fuAcf *acf, const Tb710fuAcfRecord *record,
                     uint32_t verify_every)
{
    if (!amp || !acf || !record || !record->pairs) return -1;
    if (!amp->bus.write) return -1;
    uint32_t consecutive = 0;
    for (uint32_t i = 0; i < record->pairs; ++i) {
        uint16_t address = 0, value = 0;
        if (tb710fu_acf_pair(acf, record, i, &address, &value)) return -1;
        amp->pairs++;
        if (amp_write(amp, address, value)) {
            amp->pair_errors++;
            if (++consecutive >= TB710FU_AMP_MAX_WRITE_ERRORS) return -1;
            continue;
        }
        consecutive = 0;
        if (i == 0) {
            /* The stock profile's first write is the part's control word; remember that it is in. */
            if (address == TB710FU_AMP_REG_CONTROL) amp->control_written = 1;
        }
        if (verify_every && amp->bus.read && (i % verify_every) == 0) {
            uint16_t back = 0;
            amp->readback++;
            if (amp_read(amp, address, &back) || back != value) amp->readback_bad++;
        }
    }
    return amp->pair_errors ? -1 : 0;
}

int tb710fu_amp_mute(Tb710fuAmp *amp, int mute)
{
    if (!amp || !amp->present || !amp->bus.write) return -1;
    const uint16_t word = mute ? TB710FU_AMP_CONTROL_STANDBY : TB710FU_AMP_CONTROL_ACTIVE;
    if (amp_write(amp, TB710FU_AMP_REG_CONTROL, word)) return -1;
    amp->muted = (uint8_t)(mute ? 1 : 0);
    return 0;
}

uint16_t tb710fu_amp_volume_word(int32_t db)
{
    if (db >= 0) return TB710FU_AMP_VOLUME_0DB;
    if (db <= TB710FU_AMP_VOLUME_MIN_DB) return 0;
    /* 0 dB is the top of the field; each step down is 1.5 dB of attenuation. Steps are rounded
     * to the nearest whole step so -1 dB lands one step down rather than silently at unity. */
    const int32_t steps = (2 * -db + 1) / 3;
    const int32_t top = (int32_t)TB710FU_AMP_VOLUME_0DB - steps;
    return (uint16_t)(top < 0 ? 0 : top);
}

int tb710fu_amp_volume(Tb710fuAmp *amp, int32_t db)
{
    if (!amp || !amp->present || !amp->bus.write) return -1;
    uint16_t word = 0;
    if (amp_read(amp, TB710FU_AMP_REG_VOLUME, &word)) return -1;
    word = (uint16_t)((word & (uint16_t)~(uint16_t)(TB710FU_AMP_VOLUME_MASK << TB710FU_AMP_VOLUME_SHIFT)) |
                      (tb710fu_amp_volume_word(db) << TB710FU_AMP_VOLUME_SHIFT));
    if (amp_write(amp, TB710FU_AMP_REG_VOLUME, word)) return -1;
    amp->volume_db = db < TB710FU_AMP_VOLUME_MIN_DB ? TB710FU_AMP_VOLUME_MIN_DB
                                                    : (db > 0 ? 0 : db);
    return 0;
}
