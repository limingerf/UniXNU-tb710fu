/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* See Tb710fuHid.h for the layouts this implements. */

#include "Tb710fuHid.h"

#include <string.h>

#define HID_USAGE_PAGE          0x05u
#define HID_USAGE               0x09u
#define HID_COLLECTION          0xA1u
#define HID_END_COLLECTION      0xC0u
#define HID_LOGICAL_MIN         0x15u
#define HID_LOGICAL_MAX         0x25u /* or 0x26 with two bytes */
#define HID_REPORT_SIZE         0x75u
#define HID_REPORT_COUNT        0x95u
#define HID_INPUT               0x81u

#define HID_PAGE_DIGITIZER      0x0Du
#define HID_PAGE_DESKTOP        0x01u
#define HID_PAGE_CONSUMER       0x0Cu

#define DIGITIZER_TOUCH_SCREEN  0x04u
#define DIGITIZER_FINGER        0x22u
#define DIGITIZER_TIP_SWITCH    0x42u
#define DIGITIZER_IN_RANGE      0x32u
#define DIGITIZER_CONTACT_ID    0x51u
#define DIGITIZER_CONTACT_COUNT 0x54u
#define DESKTOP_X               0x30u
#define DESKTOP_Y               0x31u
#define DESKTOP_KEYBOARD        0x06u
#define CONSUMER_CONTROL        0x01u
#define CONSUMER_POWER          0x30u
#define CONSUMER_VOLUME_UP      0xE9u
#define CONSUMER_VOLUME_DOWN    0xEAu

#define INPUT_DATA_VAR_ABS      0x02u
#define INPUT_CONST_VAR_ABS     0x03u

/* Per finger: Contact Identifier (8) + Tip/In Range (2) + padding (6) +
 * X (16) + Y (16). */
#define FINGER_BITS (8u + 2u + 6u + 16u + 16u)

/* One finger collection. Repeated ten times; the page switch to Desktop for
 * X/Y and back to Digitizer is part of the block so the repetition stays
 * self-contained. */
#define FINGER_COLLECTION                                                        \
    HID_USAGE, DIGITIZER_FINGER,                                                 \
    HID_COLLECTION, 0x02,                     /* Logical */                      \
      HID_USAGE, DIGITIZER_CONTACT_ID,                                           \
      HID_LOGICAL_MIN, 0x00,                                                     \
      HID_LOGICAL_MAX, 0x7F,                                                     \
      HID_REPORT_SIZE, 8,                                                        \
      HID_REPORT_COUNT, 1,                                                       \
      HID_INPUT, INPUT_DATA_VAR_ABS,                                             \
      HID_USAGE, DIGITIZER_TIP_SWITCH,                                           \
      HID_USAGE, DIGITIZER_IN_RANGE,                                             \
      HID_LOGICAL_MIN, 0x00,                                                     \
      HID_LOGICAL_MAX, 0x01,                                                     \
      HID_REPORT_SIZE, 1,                                                        \
      HID_REPORT_COUNT, 2,                                                       \
      HID_INPUT, INPUT_DATA_VAR_ABS,                                             \
      HID_REPORT_SIZE, 6,                                                        \
      HID_REPORT_COUNT, 1,                                                       \
      HID_INPUT, INPUT_CONST_VAR_ABS,                                            \
      HID_USAGE_PAGE, HID_PAGE_DESKTOP,                                          \
      HID_USAGE, DESKTOP_X,                                                      \
      HID_LOGICAL_MIN, 0x00,                                                     \
      0x26, 0x7F, 0x0C,                       /* Logical Maximum 3199 */           \
      HID_REPORT_SIZE, 16,                                                       \
      HID_REPORT_COUNT, 1,                                                       \
      HID_INPUT, INPUT_DATA_VAR_ABS,                                             \
      HID_USAGE, DESKTOP_Y,                                                      \
      HID_LOGICAL_MIN, 0x00,                                                     \
      0x26, 0xCF, 0x07,                       /* Logical Maximum 1999 */           \
      HID_REPORT_SIZE, 16,                                                       \
      HID_REPORT_COUNT, 1,                                                       \
      HID_INPUT, INPUT_DATA_VAR_ABS,                                             \
      HID_USAGE_PAGE, HID_PAGE_DIGITIZER,                                        \
    HID_END_COLLECTION

const uint8_t tb710fu_hid_touch_descriptor[] = {
    HID_USAGE_PAGE, HID_PAGE_DIGITIZER,
    HID_USAGE, DIGITIZER_TOUCH_SCREEN,
    HID_COLLECTION, 0x01,                        /* Application */
    /* Contact count, absolute, 0..10 */
    HID_USAGE, DIGITIZER_CONTACT_COUNT,
    HID_LOGICAL_MIN, 0x00,
    HID_LOGICAL_MAX, 0x0A,
    HID_REPORT_SIZE, 8,
    HID_REPORT_COUNT, 1,
    HID_INPUT, INPUT_DATA_VAR_ABS,

    FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION,
    FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION, FINGER_COLLECTION,
    FINGER_COLLECTION, FINGER_COLLECTION,

    HID_END_COLLECTION,
};
const uint8_t tb710fu_hid_keys_descriptor[] = {
    /* Primary usage must be Keyboard (1,6): on 26C5035r the only personality
     * declaring (0x0C,0x01) is "Peppy Touch Display" gated on Transport=FIFO.
     * "HID Keyboard Driver" matches (1,6) and handles Consumer-page media keys. */
    HID_USAGE_PAGE, HID_PAGE_DESKTOP,
    HID_USAGE, DESKTOP_KEYBOARD,
    HID_COLLECTION, 0x01,                        /* Application */
    HID_USAGE_PAGE, HID_PAGE_CONSUMER,
    HID_USAGE, CONSUMER_CONTROL,
    HID_COLLECTION, 0x02,                        /* Logical */
    HID_USAGE, CONSUMER_POWER,
    HID_USAGE, CONSUMER_VOLUME_UP,
    HID_USAGE, CONSUMER_VOLUME_DOWN,
    HID_LOGICAL_MIN, 0x00,
    HID_LOGICAL_MAX, 0x01,
    HID_REPORT_SIZE, 1,
    HID_REPORT_COUNT, 3,
    HID_INPUT, INPUT_DATA_VAR_ABS,
    HID_REPORT_SIZE, 5,
    HID_REPORT_COUNT, 1,
    HID_INPUT, INPUT_CONST_VAR_ABS,
    HID_END_COLLECTION,                          /* Logical, consumer */
    HID_END_COLLECTION,
};

const unsigned tb710fu_hid_touch_descriptor_bytes =
    (unsigned)(sizeof(tb710fu_hid_touch_descriptor) / sizeof(tb710fu_hid_touch_descriptor[0]));
const unsigned tb710fu_hid_keys_descriptor_bytes =
    (unsigned)(sizeof(tb710fu_hid_keys_descriptor) / sizeof(tb710fu_hid_keys_descriptor[0]));

/* ---- bit writer ---- */

struct bit_writer {
    uint8_t *report;
    size_t bytes;
    unsigned bit;
    int overflow;
};

static void put_bits(struct bit_writer *writer, uint32_t value, unsigned bits)
{
    unsigned i;

    for (i = 0u; i < bits; ++i) {
        const unsigned index = writer->bit >> 3;
        const unsigned offset = writer->bit & 7u;

        if (index >= writer->bytes) {
            writer->overflow = 1;
            return;
        }
        if ((value >> i) & 1u) writer->report[index] |= (uint8_t)(1u << offset);
        writer->bit++;
    }
}

/* ---- report writers ---- */

unsigned tb710fu_hid_touch_report(const struct tb710fu_hid_contact *contacts, unsigned count,
                                  uint8_t *report, size_t bytes)
{
    struct bit_writer writer;
    unsigned i;

    if (!report || bytes < TB710FU_HID_TOUCH_BYTES) return 0;
    if (count > TB710FU_HID_TOUCH_MAX_CONTACTS) return 0;
    if (count != 0u && !contacts) return 0;
    memset(report, 0, bytes);
    writer.report = report;
    writer.bytes = bytes;
    writer.bit = 0u;
    writer.overflow = 0;

    put_bits(&writer, count, 8u);
    for (i = 0u; i < TB710FU_HID_TOUCH_MAX_CONTACTS; ++i) {
        const struct tb710fu_hid_contact *contact = i < count ? &contacts[i] : NULL;

        if (!contact || !contact->active) {
            put_bits(&writer, 0u, 8u);   /* no identifier, tip and range clear */
            put_bits(&writer, 0u, 8u);
            put_bits(&writer, 0u, 16u);
            put_bits(&writer, 0u, 16u);
            continue;
        }
        put_bits(&writer, contact->id & 0x7Fu, 8u);
        put_bits(&writer, 0x3u, 2u);      /* tip switch + in range */
        put_bits(&writer, 0u, 6u);        /* padding */
        put_bits(&writer, (uint32_t)contact->x & 0xFFFFu, 16u);
        put_bits(&writer, (uint32_t)contact->y & 0xFFFFu, 16u);
    }
    if (writer.overflow || writer.bit != TB710FU_HID_TOUCH_BYTES * 8u) return 0;
    return TB710FU_HID_TOUCH_BYTES;
}

unsigned tb710fu_hid_key_report(unsigned keys, uint8_t *report, size_t bytes)
{
    if (!report || bytes < TB710FU_HID_KEY_BYTES) return 0;
    report[0] = (uint8_t)(keys & 0x7u);
    return TB710FU_HID_KEY_BYTES;
}

/* ---- descriptor interpreter (host tests and the probe's self-check) ---- */

struct hid_state {
    unsigned report_id;
    unsigned report_size;
    unsigned report_count;
    unsigned logical_max;
    int has_count;
    int has_size;
    int in_collection;
    unsigned bits;
};

unsigned tb710fu_hid_report_bits(const uint8_t *descriptor, size_t bytes, unsigned report_id)
{
    struct hid_state state;
    size_t offset = 0;

    if (!descriptor || bytes == 0u) return 0;
    memset(&state, 0, sizeof(state));
    state.report_id = 0u; /* descriptors without a report id report to id 0 */

    while (offset < bytes) {
        const uint8_t prefix = descriptor[offset];
        unsigned size;
        unsigned value = 0u;
        size_t payload;

        if (prefix == 0xFEu) {
            /* long item: skip its declared payload */
            const unsigned length = offset + 1u < bytes ? descriptor[offset + 1] : 0u;
            offset += 3u + length;
            continue;
        }
        size = prefix & 0x3u;
        if (size == 3u) size = 4u;
        payload = (size_t)size;
        if (offset + 1u + payload > bytes) return 0;
        switch (size) {
        case 1u: value = descriptor[offset + 1u]; break;
        case 2u: value = (unsigned)descriptor[offset + 1u] |
                         ((unsigned)descriptor[offset + 2u] << 8); break;
        case 4u: value = (unsigned)descriptor[offset + 1u] |
                         ((unsigned)descriptor[offset + 2u] << 8) |
                         ((unsigned)descriptor[offset + 3u] << 16) |
                         ((unsigned)descriptor[offset + 4u] << 24); break;
        default: value = 0u; break;
        }

        switch (prefix & 0xFCu) {
        case 0x84u: /* Report ID */
            state.report_id = value;
            break;
        case 0x74u: /* Report Size */
            state.report_size = value;
            state.has_size = 1;
            break;
        case 0x94u: /* Report Count */
            state.report_count = value;
            state.has_count = 1;
            break;
        case 0x24u: /* Logical Maximum */
            state.logical_max = value;
            break;
        case 0xA0u: /* Collection */
            state.in_collection = 1;
            break;
        case 0xC0u: /* End Collection */
            state.in_collection = 0;
            break;
        case 0x80u: /* Input */
            if (state.report_id == report_id) {
                if (!state.has_size || !state.has_count) return 0;
                state.bits += state.report_size * state.report_count;
            }
            break;
        default:
            break;
        }
        offset += 1u + payload;
    }
    return state.bits;
}
