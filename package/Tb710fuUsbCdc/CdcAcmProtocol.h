#ifndef TB710FU_CDC_ACM_PROTOCOL_H
#define TB710FU_CDC_ACM_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

enum tb710fu_cdc_speed {
    TB710FU_CDC_FULL_SPEED,
    TB710FU_CDC_HIGH_SPEED
};

enum tb710fu_cdc_request {
    TB710FU_CDC_REQ_BAD,
    TB710FU_CDC_REQ_GET_DESCRIPTOR,
    TB710FU_CDC_REQ_SET_ADDRESS,
    TB710FU_CDC_REQ_SET_CONFIGURATION,
    TB710FU_CDC_REQ_GET_CONFIGURATION,
    TB710FU_CDC_REQ_GET_STATUS,
    TB710FU_CDC_REQ_SET_INTERFACE,
    TB710FU_CDC_REQ_GET_INTERFACE,
    TB710FU_CDC_REQ_SET_LINE_CODING,
    TB710FU_CDC_REQ_GET_LINE_CODING,
    TB710FU_CDC_REQ_SET_CONTROL_LINE_STATE,
    TB710FU_CDC_REQ_SEND_BREAK,
    TB710FU_CDC_REQ_CLEAR_ENDPOINT_HALT
};

size_t tb710fu_cdc_build_device_descriptor(uint8_t *out, size_t capacity,
                                           uint16_t vendor_id, uint16_t product_id,
                                           uint16_t device_revision);
size_t tb710fu_cdc_build_configuration_descriptor(uint8_t *out, size_t capacity,
                                                  enum tb710fu_cdc_speed speed,
                                                  uint8_t attributes,
                                                  uint8_t max_power_2ma);
size_t tb710fu_cdc_build_device_qualifier_descriptor(uint8_t *out, size_t capacity);
/* current_speed is the speed of this connection; describe the opposite speed. */
size_t tb710fu_cdc_build_other_speed_configuration_descriptor(
    uint8_t *out, size_t capacity, enum tb710fu_cdc_speed current_speed,
    uint8_t attributes, uint8_t max_power_2ma);
size_t tb710fu_cdc_build_string_descriptor(uint8_t *out, size_t capacity,
                                           uint8_t string_index,
                                           const char *ascii, size_t length);
enum tb710fu_cdc_request tb710fu_cdc_classify_setup(uint8_t request_type,
                                                   uint8_t request,
                                                   uint16_t value,
                                                   uint16_t index,
                                                   uint16_t length);

#endif
