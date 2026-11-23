#include "CdcKernelTap.h"

#include "CdcLogRing.h"

/* Keep static __DATA at 64 KiB so native __DATA fits the MH_FILESET slot;
 * drop-oldest overwrite preserves the newest ~1,100 lines of BSD/UFS/DLIL/CDC. */
#define TB710FU_CDC_KERNEL_LOG_CAPACITY (64u * 1024u)

static uint8_t gKernelLogBytes[TB710FU_CDC_KERNEL_LOG_CAPACITY]
    __attribute__((aligned(64)));
static struct tb710fu_cdc_log_ring gKernelLog = {
    .storage = gKernelLogBytes,
    .capacity = sizeof(gKernelLogBytes),
};

void tb710fu_cdc_log_tap_putc(char byte)
{
    const uint8_t value = (uint8_t)byte;
    if (value == '\r') {
        return;
    }
    (void)tb710fu_cdc_log_ring_append_overwrite(&gKernelLog, &value, sizeof(value));
}

void tb710fu_cdc_log_tap_append(const char *bytes, size_t length)
{
    (void)tb710fu_cdc_log_ring_append_overwrite(&gKernelLog, (const uint8_t *)bytes, length);
}

size_t tb710fu_cdc_log_tap_copy(uint8_t *out, size_t capacity)
{
    return tb710fu_cdc_log_ring_copy(&gKernelLog, out, capacity);
}

int tb710fu_cdc_log_tap_consume(size_t length)
{
    return tb710fu_cdc_log_ring_consume(&gKernelLog, length);
}

size_t tb710fu_cdc_log_tap_pending(void)
{
    return tb710fu_cdc_log_ring_pending(&gKernelLog);
}

uint64_t tb710fu_cdc_log_tap_dropped(void)
{
    return tb710fu_cdc_log_ring_dropped(&gKernelLog);
}
