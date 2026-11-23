#include "../CdcKernelTap.h"
#include "../CdcLogRing.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    uint8_t bytes[8] = {0};

    assert(tb710fu_cdc_log_tap_pending() == 0);
    tb710fu_cdc_log_tap_putc('A');
    tb710fu_cdc_log_tap_putc('\r');
    tb710fu_cdc_log_tap_putc('\n');
    assert(tb710fu_cdc_log_tap_pending() == 2);
    assert(tb710fu_cdc_log_tap_copy(bytes, sizeof(bytes)) == 2);
    assert(bytes[0] == 'A' && bytes[1] == '\n');
    assert(tb710fu_cdc_log_tap_consume(2));
    assert(tb710fu_cdc_log_tap_pending() == 0);
    tb710fu_cdc_log_tap_append("ready\n", 6);
    assert(tb710fu_cdc_log_tap_copy(bytes, sizeof(bytes)) == 6);
    assert(bytes[0] == 'r' && bytes[5] == '\n');
    assert(tb710fu_cdc_log_tap_consume(6));
    assert(tb710fu_cdc_log_tap_pending() == 0);
    assert(tb710fu_cdc_log_tap_dropped() == 0);

    /* Verify drop-oldest overwrite keeps the newest tail across wraparound. */
    uint8_t storage[8] = {0};
    struct tb710fu_cdc_log_ring ring;
    tb710fu_cdc_log_ring_init(&ring, storage, sizeof(storage));
    assert(tb710fu_cdc_log_ring_append_overwrite(&ring, (const uint8_t *)"123456", 6) == 6);
    assert(tb710fu_cdc_log_ring_append_overwrite(&ring, (const uint8_t *)"abcde", 5) == 5);
    assert(tb710fu_cdc_log_ring_pending(&ring) == 8);
    assert(tb710fu_cdc_log_ring_dropped(&ring) == 3);
    assert(tb710fu_cdc_log_ring_copy(&ring, bytes, sizeof(bytes)) == 8);
    assert(memcmp(bytes, "456abcde", 8) == 0);
    assert(tb710fu_cdc_log_ring_append_overwrite(&ring, (const uint8_t *)"0123456789AB", 12) == 8);
    assert(tb710fu_cdc_log_ring_pending(&ring) == 8);
    assert(tb710fu_cdc_log_ring_dropped(&ring) == 15);
    assert(tb710fu_cdc_log_ring_copy(&ring, bytes, sizeof(bytes)) == 8);
    assert(memcmp(bytes, "456789AB", 8) == 0);
    assert(tb710fu_cdc_log_ring_consume(&ring, 8));
    assert(tb710fu_cdc_log_ring_pending(&ring) == 0);
    return 0;
}
