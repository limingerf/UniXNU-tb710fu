#ifndef TB710FU_CDC_LOG_RING_H
#define TB710FU_CDC_LOG_RING_H

#include <stddef.h>
#include <stdint.h>

struct tb710fu_cdc_log_ring {
    uint8_t *storage;
    size_t capacity;
    uint64_t read_index;
    uint64_t write_index;
    uint64_t dropped_bytes;
    uint32_t lock;
};

void tb710fu_cdc_log_ring_init(struct tb710fu_cdc_log_ring *ring,
                               uint8_t *storage, size_t capacity);
size_t tb710fu_cdc_log_ring_append(struct tb710fu_cdc_log_ring *ring,
                                   const uint8_t *bytes, size_t length);
size_t tb710fu_cdc_log_ring_append_overwrite(struct tb710fu_cdc_log_ring *ring,
                                             const uint8_t *bytes, size_t length);
size_t tb710fu_cdc_log_ring_copy(struct tb710fu_cdc_log_ring *ring,
                                 uint8_t *out, size_t capacity);
int tb710fu_cdc_log_ring_consume(struct tb710fu_cdc_log_ring *ring,
                                 size_t length);
size_t tb710fu_cdc_log_ring_pending(struct tb710fu_cdc_log_ring *ring);
uint64_t tb710fu_cdc_log_ring_dropped(struct tb710fu_cdc_log_ring *ring);

#endif
