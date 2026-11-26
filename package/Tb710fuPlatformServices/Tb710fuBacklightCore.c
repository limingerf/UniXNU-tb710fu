// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tb710fuBacklightCore.c - brightness arithmetic, no I/O, no Apple headers.
#include "Tb710fuBacklightCore.h"

#include <stddef.h>

unsigned tb710fu_backlight_clamp(unsigned level)
{
    return level > TB710FU_SY7758_MAX_LEVEL ? TB710FU_SY7758_MAX_LEVEL
                                            : level;
}

unsigned tb710fu_backlight_level_for_nits(int32_t fixed_16_16, unsigned max_nits)
{
    if (fixed_16_16 <= 0 || max_nits == 0u) {
        return 0u;
    }
    /* Proportional: the LED current, and so roughly the luminance, follows the
     * level. Rounded half up in 64 bits; the 16.16 scale is max_nits << 16. */
    const uint64_t scale = (uint64_t)max_nits << 16;
    const uint64_t scaled =
        ((uint64_t)fixed_16_16 * TB710FU_SY7758_MAX_LEVEL + scale / 2u) / scale;
    if (scaled < 1u) {
        return 1u; /* a positive request never means "off" */
    }
    return tb710fu_backlight_clamp((unsigned)scaled);
}

unsigned tb710fu_backlight_milli_nits_for_level(unsigned level, unsigned max_nits)
{
    const unsigned clamped = tb710fu_backlight_clamp(level);

    /* 64-bit product: 4080 * 500000 overflows 32 bits. */
    return (unsigned)(((uint64_t)clamped * (uint64_t)max_nits * 1000u) /
                      TB710FU_SY7758_MAX_LEVEL);
}

void tb710fu_backlight_split12(unsigned level, uint8_t *low, uint8_t *high)
{
    const unsigned clamped = tb710fu_backlight_clamp(level);

    if (low != NULL) {
        *low = (uint8_t)(clamped & 0xffu);
    }
    if (high != NULL) {
        *high = (uint8_t)((clamped >> 8) & 0x0fu);
    }
}

unsigned tb710fu_backlight_join12(uint8_t low, uint8_t high)
{
    return (unsigned)(((unsigned)(high & 0x0fu) << 8) | (unsigned)low);
}

int tb710fu_backlight_mode_12bit(uint8_t dev_ctl)
{
    return (dev_ctl & TB710FU_SY7758_BRT_MODE_MASK) ==
        TB710FU_SY7758_BRT_MODE_12BIT;
}

unsigned tb710fu_backlight_fade_level(unsigned from, unsigned to,
                                      uint64_t elapsed_ns, uint64_t period_ns)
{
    /* A fade longer than about 2.4 hours is not a fade. The bound also keeps
     * (end - start) * elapsed inside 63 bits, which is why the product below
     * needs no 128-bit arithmetic. */
    if (period_ns == 0u || elapsed_ns >= period_ns || period_ns > (1ull << 43)) {
        return tb710fu_backlight_clamp(to);
    }
    const int64_t start = (int64_t)(int32_t)tb710fu_backlight_clamp(from);
    const int64_t end = (int64_t)(int32_t)tb710fu_backlight_clamp(to);
    const int64_t step =
        (end - start) * (int64_t)elapsed_ns / (int64_t)period_ns;
    int64_t next = start + step;

    /* The integer division truncates towards zero, so a step that is not the
     * whole distance lands short of `to` - never past it. Clamp anyway: the
     * caller may hand in an elapsed time that has already run backwards. */
    if (start <= end) {
        if (next > end) {
            next = end;
        }
    } else {
        if (next < end) {
            next = end;
        }
    }
    if (next < 0) {
        next = 0;
    }
    if (next > (int64_t)TB710FU_SY7758_MAX_LEVEL) {
        next = (int64_t)TB710FU_SY7758_MAX_LEVEL;
    }
    return (unsigned)next;
}
