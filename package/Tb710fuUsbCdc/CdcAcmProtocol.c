#include "CdcAcmProtocol.h"

#define CDC_DEVICE_DESCRIPTOR_SIZE 18u
#define CDC_CONFIGURATION_DESCRIPTOR_SIZE 67u
#define CDC_LANGUAGE_ID 0x0409u

static void put_u16le(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

size_t tb710fu_cdc_build_device_descriptor(uint8_t *out, size_t capacity,
                                           uint16_t vendor_id, uint16_t product_id,
                                           uint16_t device_revision)
{
    if (out == NULL || capacity < CDC_DEVICE_DESCRIPTOR_SIZE ||
        vendor_id == 0 || product_id == 0) {
        return 0;
    }

    const uint8_t descriptor[CDC_DEVICE_DESCRIPTOR_SIZE] = {
        18, 1, 0x00, 0x02, 0x02, 0x02, 0x01, 64,
        0, 0, 0, 0, 0, 0, 1, 2, 3, 1
    };
    for (size_t i = 0; i < sizeof(descriptor); ++i) {
        out[i] = descriptor[i];
    }
    put_u16le(&out[8], vendor_id);
    put_u16le(&out[10], product_id);
    put_u16le(&out[12], device_revision);
    return CDC_DEVICE_DESCRIPTOR_SIZE;
}

size_t tb710fu_cdc_build_configuration_descriptor(uint8_t *out, size_t capacity,
                                                  enum tb710fu_cdc_speed speed,
                                                  uint8_t attributes,
                                                  uint8_t max_power_2ma)
{
    if (out == NULL || capacity < CDC_CONFIGURATION_DESCRIPTOR_SIZE ||
        (speed != TB710FU_CDC_FULL_SPEED && speed != TB710FU_CDC_HIGH_SPEED) ||
        (attributes & 0x80u) == 0 || (attributes & 0x1fu) != 0 ||
        max_power_2ma > 250u) {
        return 0;
    }

    const uint16_t bulk_packet = speed == TB710FU_CDC_HIGH_SPEED ? 512u : 64u;
    const uint8_t descriptor[CDC_CONFIGURATION_DESCRIPTOR_SIZE] = {
        9, 2, 67, 0, 2, 1, 0, 0x80, 0,
        9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,
        5, 0x24, 0x00, 0x10, 0x01,
        5, 0x24, 0x01, 0x00, 0x01,
        4, 0x24, 0x02, 0x02,
        5, 0x24, 0x06, 0x00, 0x01,
        7, 5, 0x82, 0x03, 16, 0, 9,
        9, 4, 1, 0, 2, 0x0a, 0, 0, 0,
        7, 5, 0x01, 0x02, 0, 0, 0,
        7, 5, 0x81, 0x02, 0, 0, 0
    };
    for (size_t i = 0; i < sizeof(descriptor); ++i) {
        out[i] = descriptor[i];
    }
    out[7] = attributes;
    out[8] = max_power_2ma;
    put_u16le(&out[57], bulk_packet);
    put_u16le(&out[64], bulk_packet);
    return CDC_CONFIGURATION_DESCRIPTOR_SIZE;
}

size_t tb710fu_cdc_build_string_descriptor(uint8_t *out, size_t capacity,
                                           uint8_t string_index,
                                           const char *ascii, size_t length)
{
    if (out == NULL) {
        return 0;
    }
    if (string_index == 0) {
        if (capacity < 4 || ascii != NULL || length != 0) {
            return 0;
        }
        out[0] = 4;
        out[1] = 3;
        put_u16le(&out[2], CDC_LANGUAGE_ID);
        return 4;
    }
    if (string_index > 3 || ascii == NULL || length == 0 || length > 126 ||
        capacity < 2 + length * 2) {
        return 0;
    }
    for (size_t i = 0; i < length; ++i) {
        if ((unsigned char)ascii[i] > 0x7fu) {
            return 0;
        }
    }
    out[0] = (uint8_t)(2 + length * 2);
    out[1] = 3;
    for (size_t i = 0; i < length; ++i) {
        out[2 + i * 2] = (uint8_t)ascii[i];
        out[3 + i * 2] = 0;
    }
    return 2 + length * 2;
}

static int valid_endpoint(uint16_t index)
{
    if ((index & 0xff00u) != 0) {
        return 0;
    }
    return index == 0x01u || index == 0x81u || index == 0x82u;
}

enum tb710fu_cdc_request tb710fu_cdc_classify_setup(uint8_t request_type,
                                                   uint8_t request,
                                                   uint16_t value,
                                                   uint16_t index,
                                                   uint16_t length)
{
    switch (request_type) {
    case 0x80:
        if (request == 6 && (value >> 8) >= 1 && (value >> 8) <= 7) {
            const uint8_t descriptor_type = (uint8_t)(value >> 8);
            const uint8_t descriptor_index = (uint8_t)value;
            if (descriptor_type != 1 && descriptor_type != 2 &&
                descriptor_type != 3 && descriptor_type != 6) {
                return TB710FU_CDC_REQ_BAD;
            }
            if ((descriptor_type == 1 || descriptor_type == 2 || descriptor_type == 6) &&
                (descriptor_index != 0 || index != 0)) {
                return TB710FU_CDC_REQ_BAD;
            }
            if (descriptor_type == 3 &&
                (descriptor_index > 3 ||
                 (descriptor_index == 0 ? index != 0 : index != CDC_LANGUAGE_ID))) {
                return TB710FU_CDC_REQ_BAD;
            }
            return TB710FU_CDC_REQ_GET_DESCRIPTOR;
        }
        if (request == 8 && value == 0 && index == 0 && length == 1) {
            return TB710FU_CDC_REQ_GET_CONFIGURATION;
        }
        if (request == 0 && value == 0 && index == 0 && length == 2) {
            return TB710FU_CDC_REQ_GET_STATUS;
        }
        break;
    case 0x00:
        if (request == 5 && value < 128 && index == 0 && length == 0) {
            return TB710FU_CDC_REQ_SET_ADDRESS;
        }
        if (request == 9 && value <= 1 && index == 0 && length == 0) {
            return TB710FU_CDC_REQ_SET_CONFIGURATION;
        }
        break;
    case 0x81:
        if (request == 0 && value == 0 && index <= 1 && length == 2) {
            return TB710FU_CDC_REQ_GET_STATUS;
        }
        if (request == 10 && value == 0 && index <= 1 && length == 1) {
            return TB710FU_CDC_REQ_GET_INTERFACE;
        }
        break;
    case 0x82:
        if (request == 0 && value == 0 && valid_endpoint(index) && length == 2) {
            return TB710FU_CDC_REQ_GET_STATUS;
        }
        break;
    case 0x01:
        if (request == 11 && value == 0 && index <= 1 && length == 0) {
            return TB710FU_CDC_REQ_SET_INTERFACE;
        }
        break;
    case 0x21:
        if (request == 0x20 && value == 0 && index == 0 && length == 7) {
            return TB710FU_CDC_REQ_SET_LINE_CODING;
        }
        if (request == 0x22 && index == 0 && length == 0 && (value & ~3u) == 0) {
            return TB710FU_CDC_REQ_SET_CONTROL_LINE_STATE;
        }
        if (request == 0x23 && index == 0 && length == 0) {
            return TB710FU_CDC_REQ_SEND_BREAK;
        }
        break;
    case 0xa1:
        if (request == 0x21 && value == 0 && index == 0 && length == 7) {
            return TB710FU_CDC_REQ_GET_LINE_CODING;
        }
        break;
    case 0x02:
        if (request == 1 && value == 0 && valid_endpoint(index) && length == 0) {
            return TB710FU_CDC_REQ_CLEAR_ENDPOINT_HALT;
        }
        break;
    default:
        break;
    }
    return TB710FU_CDC_REQ_BAD;
}
