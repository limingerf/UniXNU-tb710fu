/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Boot-argument policy for the TB710FU input subsystem.
 *
 * The subsystem is off unless the firmware passes an explicit numeric level.
 * Every level is a strictly increasing bring-up step and each one is safe to
 * stop at:
 *
 *   tb710fu-touch=0  off (default). Nothing is mapped, nothing is written.
 *   tb710fu-touch=1  snapshot. Maps the SE4 and QUP wrapper windows and reads
 *                    their identity registers. Writes nothing at all, so it
 *                    cannot disturb a controller the firmware left running.
 *   tb710fu-touch=2  engine. Adds GENI initialisation and the SPI divider
 *                    (register writes inside SE4 only) and proves that the
 *                    engine reaches idle. No controller traffic yet.
 *   tb710fu-touch=3  full. Adds the non-destructive NT36532 identify, the
 *                    frame loop, multi-touch tracking and HID publication.
 *
 *   tb710fu-keys=0   off (default).
 *   tb710fu-keys=1   poll the PMIC PON and PM8550 GPIO6 status registers
 *                    through the SPMI observer channel and publish the
 *                    Consumer-page key device.
 *
 * Additional, separately gated privileges:
 *
 *   tb710fu-touch-fuse=1   allow the one read of SE_GENI_IF_DISABLE (0x64)
 *                          that the sibling hardware is documented to abort
 *                          on. Off by default; only a deliberate diagnosis
 *                          should touch it.
 *   tb710fu-touch-irq=1    service GENI completion from the GIC instead of
 *                          polling. Off by default: an unhandled level
 *                          interrupt from an SE that is not ours to own is a
 *                          boot-regression risk.
 *   tb710fu-input-verbose=1  per-frame logging.
 *
 * There is deliberately no firmware-download switch. Uploading an image
 * resets the controller and, if anything about the blob is wrong, leaves the
 * panel without a digitizer until the next power cycle; the reference blob
 * is validated offline instead (Tb710fuNvtFirmware.c, docs/INPUT_TOUCH_KEYS.md).
 */
#ifndef TB710FU_INPUT_CONFIG_H
#define TB710FU_INPUT_CONFIG_H

#include <stdint.h>

enum tb710fu_touch_stage {
    TB710FU_TOUCH_OFF = 0,
    TB710FU_TOUCH_SNAPSHOT = 1,
    TB710FU_TOUCH_ENGINE = 2,
    TB710FU_TOUCH_FULL = 3,
};

enum tb710fu_keys_stage {
    TB710FU_KEYS_OFF = 0,
    TB710FU_KEYS_ON = 1,
};

struct tb710fu_input_options {
    int touch_stage;      /* enum tb710fu_touch_stage */
    int keys_stage;       /* enum tb710fu_keys_stage  */
    int fuse_probe;       /* tb710fu-touch-fuse       */
    int use_irq;          /* tb710fu-touch-irq        */
    int verbose;          /* tb710fu-input-verbose    */
    int frame_limit;      /* 0 = unlimited            */
};

/* Parse one "name=value" token. Returns 0 when the token is ours (whether or
 * not it was accepted), 1 when it belongs to another subsystem. On a value
 * error *error is set to a short reason and the field keeps its safe default.
 * `name` and `name_len` describe the part before '=', `value` may be NULL when
 * the token has no '='. */
int tb710fu_input_option(const char *name, unsigned name_len,
                         const char *value, struct tb710fu_input_options *options,
                         const char **error);

/* Split a boot-argument string on spaces and apply every recognised token.
 * Returns the number of tokens applied; options always ends in a valid state
 * (unknown or malformed values leave the safe default in place). */
int tb710fu_input_bootargs(const char *arguments,
                           struct tb710fu_input_options *options,
                           const char **first_error);

void tb710fu_input_options_default(struct tb710fu_input_options *options);

#endif /* TB710FU_INPUT_CONFIG_H */
