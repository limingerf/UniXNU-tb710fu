/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* See Tb710fuKeys.h for the wiring and the filter contract. */

#include "Tb710fuKeys.h"
#include "Tb710fuHid.h"
#include "Tb710fuInputHardware.h"

#include <string.h>

#define PPID_PON      ((TB710FU_PMIC_SID_PMK8550 << 8) | TB710FU_PON_PID)
#define PPID_VOLUP    ((TB710FU_PMIC_SID_PM8550 << 8) | TB710FU_PM8550_VOLUP_PID)

int tb710fu_keys_open(const struct tb710fu_spmi *spmi, struct tb710fu_keys *keys)
{
    int result;

    if (!keys) return 4;
    keys->pon_apid = -1;
    keys->gpio_apid = -1;
    keys->have_pon = 0;
    keys->have_gpio = 0;

    result = tb710fu_spmi_find_apid(spmi, PPID_PON, &keys->pon_apid);
    if (result != 0) return result;
    keys->have_pon = 1;

    if (tb710fu_spmi_find_apid(spmi, PPID_VOLUP, &keys->gpio_apid) == 0)
        keys->have_gpio = 1;
    return 0;
}

int tb710fu_keys_sample(const struct tb710fu_spmi *spmi, const struct tb710fu_keys *keys,
                        uint8_t *pressed)
{
    uint8_t value = 0;
    int result;

    if (!pressed || !keys || !keys->have_pon) return 4;
    *pressed = 0u;
    result = tb710fu_spmi_read8(spmi, keys->pon_apid, TB710FU_PON_RT_STS, &value);
    if (result != 0) return result;
    if ((value & TB710FU_PON_GEN3_KPDPWR) != 0u) *pressed |= TB710FU_KEY_SAMPLE_POWER;
    if ((value & TB710FU_PON_GEN3_RESIN) != 0u) *pressed |= TB710FU_KEY_SAMPLE_VOLUME_DOWN;

    if (keys->have_gpio) {
        result = tb710fu_spmi_read8(spmi, keys->gpio_apid, TB710FU_PM_GPIO_RT_STS, &value);
        if (result != 0) {
            *pressed = 0u;
            return result;
        }
        /* active-low pad: a cleared status bit is a pressed key */
        if ((value & TB710FU_PM_GPIO_RT_STS_LOW) == 0u) *pressed |= TB710FU_KEY_SAMPLE_VOLUME_UP;
    }
    return 0;
}

void tb710fu_key_filter_reset(struct tb710fu_key_filter *filter)
{
    if (!filter) return;
    memset(filter, 0, sizeof(*filter));
}

uint8_t tb710fu_key_filter_step(struct tb710fu_key_filter *filter, uint8_t sample,
                                unsigned delay_polls, unsigned period_polls)
{
    uint8_t out = 0u;
    unsigned i;

    if (!filter) return 0u;
    if (sample == filter->last) {
        const uint8_t fresh = (uint8_t)(sample & ~filter->stable);

        filter->stable = sample;
        out = fresh;
        for (i = 0u; i < TB710FU_KEY_SAMPLE_COUNT; ++i) {
            const uint8_t bit = (uint8_t)(1u << i);

            if ((sample & bit) == 0u || (fresh & bit) != 0u) {
                filter->held[i] = 0u;
                filter->repeat[i] = 0u;
                continue;
            }
            /* The power key is never repeated; volume keys repeat only when
             * both the delay and the period are configured. */
            if (i == 0u || delay_polls == 0u || period_polls == 0u) continue;
            if (filter->held[i] < delay_polls) {
                filter->held[i]++;
                if (filter->held[i] == delay_polls) {
                    out |= bit;
                    filter->repeat[i] = 0u;
                }
            } else {
                if (filter->repeat[i] + 1u >= period_polls) {
                    out |= bit;
                    filter->repeat[i] = 0u;
                } else {
                    filter->repeat[i]++;
                }
            }
        }
    }
    filter->last = sample;
    return out;
}

unsigned tb710fu_keys_hid_bits(uint8_t pressed)
{
    unsigned bits = 0u;

    if ((pressed & TB710FU_KEY_SAMPLE_POWER) != 0u) bits |= TB710FU_HID_KEY_POWER;
    if ((pressed & TB710FU_KEY_SAMPLE_VOLUME_UP) != 0u) bits |= TB710FU_HID_KEY_VOLUME_UP;
    if ((pressed & TB710FU_KEY_SAMPLE_VOLUME_DOWN) != 0u) bits |= TB710FU_HID_KEY_VOLUME_DOWN;
    return bits;
}
