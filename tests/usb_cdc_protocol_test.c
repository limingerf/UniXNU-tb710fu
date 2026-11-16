#include "CdcAcmProtocol.h"
#include "CdcLogRing.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void test_device_descriptor(void)
{
    uint8_t descriptor[18];
    assert(tb710fu_cdc_build_device_descriptor(descriptor, sizeof(descriptor),
                                              0x1209, 0x710f, 0x0100) == sizeof(descriptor));
    assert(descriptor[0] == 18 && descriptor[1] == 1);
    assert(descriptor[8] == 0x09 && descriptor[9] == 0x12);
    assert(descriptor[10] == 0x0f && descriptor[11] == 0x71);
    assert(descriptor[12] == 0 && descriptor[13] == 1);
    assert(tb710fu_cdc_build_device_descriptor(descriptor, 17, 0x1209, 0x710f, 0) == 0);
    assert(tb710fu_cdc_build_device_descriptor(descriptor, sizeof(descriptor), 0, 1, 0) == 0);
}

static void test_configuration_descriptors(void)
{
    uint8_t descriptor[67];
    assert(tb710fu_cdc_build_configuration_descriptor(descriptor, sizeof(descriptor),
                                                      TB710FU_CDC_HIGH_SPEED, 0xc0, 50) == 67);
    assert(descriptor[0] == 9 && descriptor[1] == 2);
    assert(descriptor[2] == 67 && descriptor[3] == 0 && descriptor[4] == 2);
    assert(descriptor[7] == 0xc0 && descriptor[8] == 50);
    assert(descriptor[37] == 7 && descriptor[38] == 5 && descriptor[39] == 0x82);
    assert(descriptor[53] == 7 && descriptor[55] == 0x01 && descriptor[56] == 0x02);
    assert(descriptor[57] == 0 && descriptor[58] == 2);
    assert(descriptor[64] == 0 && descriptor[65] == 2);
    assert(tb710fu_cdc_build_configuration_descriptor(descriptor, sizeof(descriptor),
                                                      TB710FU_CDC_FULL_SPEED, 0x80, 50) == 67);
    assert(descriptor[57] == 64 && descriptor[58] == 0);
    assert(tb710fu_cdc_build_configuration_descriptor(descriptor, 66,
                                                      TB710FU_CDC_HIGH_SPEED, 0x80, 50) == 0);
    assert(tb710fu_cdc_build_configuration_descriptor(descriptor, sizeof(descriptor),
                                                      TB710FU_CDC_HIGH_SPEED, 0x00, 50) == 0);
    assert(tb710fu_cdc_build_configuration_descriptor(descriptor, sizeof(descriptor),
                                                      TB710FU_CDC_HIGH_SPEED, 0x80, 251) == 0);
}

static void test_strings(void)
{
    uint8_t descriptor[32];
    assert(tb710fu_cdc_build_string_descriptor(descriptor, sizeof(descriptor),
                                              0, NULL, 0) == 4);
    assert(descriptor[0] == 4 && descriptor[1] == 3 && descriptor[2] == 9 && descriptor[3] == 4);
    assert(tb710fu_cdc_build_string_descriptor(descriptor, sizeof(descriptor),
                                              2, "TB710FU", 7) == 16);
    assert(descriptor[0] == 16 && descriptor[1] == 3 && descriptor[2] == 'T' && descriptor[3] == 0);
    assert(tb710fu_cdc_build_string_descriptor(descriptor, 15, 2, "TB710FU", 7) == 0);
    assert(tb710fu_cdc_build_string_descriptor(descriptor, sizeof(descriptor),
                                              2, "bad\x80", 4) == 0);
    assert(tb710fu_cdc_build_string_descriptor(descriptor, sizeof(descriptor),
                                              4, "x", 1) == 0);
}

static void test_setup_requests(void)
{
    assert(tb710fu_cdc_classify_setup(0x80, 6, 0x0100, 0, 18) == TB710FU_CDC_REQ_GET_DESCRIPTOR);
    assert(tb710fu_cdc_classify_setup(0x80, 6, 0x0302, 0x0409, 16) == TB710FU_CDC_REQ_GET_DESCRIPTOR);
    assert(tb710fu_cdc_classify_setup(0x00, 5, 127, 0, 0) == TB710FU_CDC_REQ_SET_ADDRESS);
    assert(tb710fu_cdc_classify_setup(0x00, 9, 1, 0, 0) == TB710FU_CDC_REQ_SET_CONFIGURATION);
    assert(tb710fu_cdc_classify_setup(0x80, 8, 0, 0, 1) == TB710FU_CDC_REQ_GET_CONFIGURATION);
    assert(tb710fu_cdc_classify_setup(0x82, 0, 0, 0x81, 2) == TB710FU_CDC_REQ_GET_STATUS);
    assert(tb710fu_cdc_classify_setup(0x01, 11, 0, 1, 0) == TB710FU_CDC_REQ_SET_INTERFACE);
    assert(tb710fu_cdc_classify_setup(0x81, 10, 0, 0, 1) == TB710FU_CDC_REQ_GET_INTERFACE);
    assert(tb710fu_cdc_classify_setup(0x21, 0x20, 0, 0, 7) == TB710FU_CDC_REQ_SET_LINE_CODING);
    assert(tb710fu_cdc_classify_setup(0xa1, 0x21, 0, 0, 7) == TB710FU_CDC_REQ_GET_LINE_CODING);
    assert(tb710fu_cdc_classify_setup(0x21, 0x22, 3, 0, 0) == TB710FU_CDC_REQ_SET_CONTROL_LINE_STATE);
    assert(tb710fu_cdc_classify_setup(0x21, 0x23, 0xffff, 0, 0) == TB710FU_CDC_REQ_SEND_BREAK);
    assert(tb710fu_cdc_classify_setup(0x02, 1, 0, 0x82, 0) == TB710FU_CDC_REQ_CLEAR_ENDPOINT_HALT);
    assert(tb710fu_cdc_classify_setup(0x21, 0x20, 0, 0, 6) == TB710FU_CDC_REQ_BAD);
    assert(tb710fu_cdc_classify_setup(0x21, 0x22, 4, 0, 0) == TB710FU_CDC_REQ_BAD);
    assert(tb710fu_cdc_classify_setup(0x82, 0, 0, 0x83, 2) == TB710FU_CDC_REQ_BAD);
    assert(tb710fu_cdc_classify_setup(0x00, 5, 128, 0, 0) == TB710FU_CDC_REQ_BAD);
    assert(tb710fu_cdc_classify_setup(0x80, 6, 0x0302, 0, 16) == TB710FU_CDC_REQ_BAD);
    assert(tb710fu_cdc_classify_setup(0x80, 6, 0x0400, 0, 9) == TB710FU_CDC_REQ_BAD);
    assert(tb710fu_cdc_classify_setup(0x80, 6, 0x0500, 0, 7) == TB710FU_CDC_REQ_BAD);
    assert(tb710fu_cdc_classify_setup(0xc0, 0x55, 0, 0, 0) == TB710FU_CDC_REQ_BAD);
}

static void test_log_ring(void)
{
    struct tb710fu_cdc_log_ring ring;
    uint8_t storage[8];
    uint8_t copy[8];
    tb710fu_cdc_log_ring_init(&ring, storage, sizeof(storage));

    assert(tb710fu_cdc_log_ring_append(&ring, (const uint8_t *)"abcdef", 6) == 6);
    assert(tb710fu_cdc_log_ring_copy(&ring, copy, 4) == 4);
    assert(memcmp(copy, "abcd", 4) == 0);
    assert(tb710fu_cdc_log_ring_pending(&ring) == 6);
    assert(tb710fu_cdc_log_ring_consume(&ring, 4));
    assert(!tb710fu_cdc_log_ring_consume(&ring, 3));
    assert(tb710fu_cdc_log_ring_append(&ring, (const uint8_t *)"ghijklm", 7) == 6);
    assert(tb710fu_cdc_log_ring_dropped(&ring) == 1);
    assert(tb710fu_cdc_log_ring_copy(&ring, copy, sizeof(copy)) == sizeof(copy));
    assert(memcmp(copy, "efghijkl", sizeof(copy)) == 0);
    assert(tb710fu_cdc_log_ring_consume(&ring, sizeof(copy)));
    assert(tb710fu_cdc_log_ring_pending(&ring) == 0);
    ring.lock = 1;
    assert(tb710fu_cdc_log_ring_append(&ring, (const uint8_t *)"lost", 4) == 0);
    assert(tb710fu_cdc_log_ring_dropped(&ring) == 5);
    ring.lock = 0;
}

int main(void)
{
    test_device_descriptor();
    test_configuration_descriptors();
    test_strings();
    test_setup_requests();
    test_log_ring();
    return 0;
}
