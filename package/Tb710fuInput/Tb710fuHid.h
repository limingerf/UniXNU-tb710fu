/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* HID descriptors and report packing for the TB710FU digitizer and keys.
 *
 * The descriptors are written by hand and the report writers are checked
 * against them: scripts/check-tb710fu-input-host.py parses the descriptors
 * with a small HID item interpreter, sums the Input field widths and asserts
 * that the writers fill exactly that many bits. That is the property that
 * keeps the two halves from drifting apart, not a comment.
 *
 * Both devices carry exactly one input report, so neither descriptor needs a
 * Report ID: the report bytes below are the whole transaction.
 *
 * Digitizer layout (61 bytes):
 *   byte  0      Contact Count          (0..10)
 *   per finger, 6 bytes:
 *     byte 0     Contact Identifier     (8 bits)
 *     byte 1     Tip Switch, In Range   (2 bits) + 6 bits padding
 *     bytes 2-3  X, logical 0..3199     (16 bits)
 *     bytes 4-5  Y, logical 0..1999     (16 bits)
 * A finger slot whose identifier is absent is emitted with Tip Switch and
 * In Range cleared and coordinates left at the previous value.
 *
 * Key layout (1 byte): Power, Volume Increment, Volume
 * Decrement as three absolute bits plus five constant bits.
 */
#ifndef TB710FU_HID_H
#define TB710FU_HID_H

#include <stdint.h>
#include <stddef.h>

#define TB710FU_HID_TOUCH_BYTES       61u
#define TB710FU_HID_TOUCH_MAX_CONTACTS 10u
#define TB710FU_HID_TOUCH_LOGICAL_X   3199u
#define TB710FU_HID_TOUCH_LOGICAL_Y   1999u

#define TB710FU_HID_KEY_BYTES         1u

#define TB710FU_HID_KEY_POWER         (1u << 0)
#define TB710FU_HID_KEY_VOLUME_UP     (1u << 1)
#define TB710FU_HID_KEY_VOLUME_DOWN   (1u << 2)

struct tb710fu_hid_contact {
    uint8_t id;
    int active;         /* tip switch + in range */
    uint16_t x;
    uint16_t y;
};

extern const uint8_t tb710fu_hid_touch_descriptor[];
extern const unsigned tb710fu_hid_touch_descriptor_bytes;
extern const uint8_t tb710fu_hid_keys_descriptor[];
extern const unsigned tb710fu_hid_keys_descriptor_bytes;

/* Pack one touch report. Returns the filled length (TB710FU_HID_TOUCH_BYTES) or
 * 0 on a malformed argument. `contacts` must hold `count` entries; slots
 * without an identifier are padded with inactive zeroed fingers. */
unsigned tb710fu_hid_touch_report(const struct tb710fu_hid_contact *contacts, unsigned count,
                                  uint8_t *report, size_t bytes);

/* Pack one key report. Returns TB710FU_HID_KEY_BYTES or 0. */
unsigned tb710fu_hid_key_report(unsigned keys, uint8_t *report, size_t bytes);

/* Minimal HID item interpreter used by the host tests: returns the total
 * number of Input-report bits the descriptor declares for `report_id`, or 0
 * when the descriptor is inconsistent. */
unsigned tb710fu_hid_report_bits(const uint8_t *descriptor, size_t bytes, unsigned report_id);

#endif /* TB710FU_HID_H */
