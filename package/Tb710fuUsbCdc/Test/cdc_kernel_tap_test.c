#include "../CdcKernelTap.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

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
    return 0;
}
