/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The service-side fader. The AW88461's own digital attenuation is only used when
 * audio-amp-volume=1 (see Tb710fuAw88461.h); by default the level is applied here, on the copy
 * from the user client's chunk into the DSP's shared-memory endpoint, which is exactly where a
 * HAL's software volume would sit. One dB step per entry, 0 dB (unity) down to -96 dB, in
 * millionths: 10^(-dB/20) * 1e6 rounded. */
#include "Tb710fuAudioAbi.h"

#include <stddef.h>

static const int32_t kGainPpm[97] = {
    1000000,  891251,  794328,  707946,  630957,  562341,  501187,  446684,
    398107,   354813,  316228,  281838,  251189,  223872,  199526,  177828,
    158489,   141254,  125893,  112202,  100000,   89125,   79433,   70795,
    63096,    56234,   50119,   44668,   39811,   35481,   31623,   28184,
    25119,    22387,   19953,   17783,   15849,   14125,   12589,   11220,
    10000,     8913,    7943,    7079,    6310,    5623,    5012,    4467,
    3981,      3548,    3162,    2818,    2512,    2239,    1995,    1778,
    1585,      1413,    1259,    1122,    1000,     891,     794,     708,
    631,       562,     501,     447,     398,     355,     316,     282,
    251,       224,     200,     178,     158,     141,     126,     112,
    100,        89,      79,      71,      63,      56,      50,      45,
    40,         35,      32,      28,      25,      22,      20,      18,
    16,
};

int32_t tb710fu_audio_gain_ppm(int32_t db)
{
    if (db >= 0) return kGainPpm[0];
    if (db <= -96) return kGainPpm[96];
    return kGainPpm[-db];
}

void tb710fu_audio_apply_gain(int16_t *samples, uint32_t count, int32_t db)
{
    if (!samples) return;
    const int32_t ppm = tb710fu_audio_gain_ppm(db);
    if (ppm == kGainPpm[0]) return;
    for (uint32_t i = 0; i < count; ++i) {
        int64_t scaled = (int64_t)samples[i] * ppm / 1000000;
        if (scaled > 32767) scaled = 32767;
        else if (scaled < -32768) scaled = -32768;
        samples[i] = (int16_t)scaled;
    }
}
