#ifndef TB710FU_CDC_KERNEL_TAP_H
#define TB710FU_CDC_KERNEL_TAP_H

#include <stddef.h>
#include <stdint.h>

void tb710fu_cdc_log_tap_putc(char byte);
void tb710fu_cdc_log_tap_append(const char *bytes, size_t length);
size_t tb710fu_cdc_log_tap_copy(uint8_t *out, size_t capacity);
int tb710fu_cdc_log_tap_consume(size_t length);
/* Span pair used by the USB transport: read reports the ring position just past
 * the copied span, commit retires exactly that position and never fails. */
size_t tb710fu_cdc_log_tap_read(uint8_t *out, size_t capacity, uint64_t *end_index);
void tb710fu_cdc_log_tap_commit(uint64_t end_index);
size_t tb710fu_cdc_log_tap_pending(void);
uint64_t tb710fu_cdc_log_tap_dropped(void);

#endif
