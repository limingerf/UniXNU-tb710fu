/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Power / volume key policy on top of the SPMI observer reads.
 *
 * Hardware mapping (all from the board DTS and the PMIC bindings, see
 * Tb710fuInputHardware.h):
 *   PON GEN3 KPDPWR bit 7, set = pressed  -> KEY_POWER      -> HID Consumer Power
 *   PON GEN3 RESIN  bit 6, set = pressed  -> KEY_VOLUMEDOWN -> HID Consumer Volume Decrement
 *   PM8550 GPIO6 RT_STS bit 0, active low -> KEY_VOLUMEUP   -> HID Consumer Volume Increment
 *
 * The filter needs two identical samples before it reports a change, which
 * covers the DTS debounce-interval (15 ms) at a 10 ms poll. Only the volume
 * keys repeat: on this board a held PON key is a hardware power-off handled
 * below this driver.
 */
#ifndef TB710FU_KEYS_H
#define TB710FU_KEYS_H

#include <stdint.h>

#include "Tb710fuSpmi.h"

#define TB710FU_KEY_SAMPLE_POWER       (1u << 0)
#define TB710FU_KEY_SAMPLE_VOLUME_DOWN (1u << 1)
#define TB710FU_KEY_SAMPLE_VOLUME_UP   (1u << 2)
#define TB710FU_KEY_SAMPLE_COUNT       3u

struct tb710fu_keys {
    int pon_apid;
    int gpio_apid;
    int have_pon;
    int have_gpio;
};

struct tb710fu_key_filter {
    uint8_t last;
    uint8_t stable;
    unsigned held[TB710FU_KEY_SAMPLE_COUNT];
    unsigned repeat[TB710FU_KEY_SAMPLE_COUNT];
};

/* Locate the two APIDs. Returns 0 when the PON peripheral was found (the
 * power and volume-down keys work without PM8550 GPIO6); 3 when the PON
 * peripheral is absent and 1/2 for an unusable arbiter layout. */
int tb710fu_keys_open(const struct tb710fu_spmi *spmi, struct tb710fu_keys *keys);

/* Raw pressed set as TB710FU_KEY_SAMPLE_* bits. 0 on success; on a read error
 * nothing is reported pressed so a bus fault cannot synthesize a key. */
int tb710fu_keys_sample(const struct tb710fu_spmi *spmi, const struct tb710fu_keys *keys,
                        uint8_t *pressed);

void tb710fu_key_filter_reset(struct tb710fu_key_filter *filter);

/* Advance the filter by one sample. `delay_polls` is the hold count before the
 * first repeat, `period_polls` the repeat period; 0 disables repeat. Returns
 * the keys to report this poll (TB710FU_KEY_SAMPLE_* bits). */
uint8_t tb710fu_key_filter_step(struct tb710fu_key_filter *filter, uint8_t sample,
                                unsigned delay_polls, unsigned period_polls);

/* Map a sample set onto the HID key bits of Tb710fuHid.h. */
unsigned tb710fu_keys_hid_bits(uint8_t pressed);

#endif /* TB710FU_KEYS_H */
