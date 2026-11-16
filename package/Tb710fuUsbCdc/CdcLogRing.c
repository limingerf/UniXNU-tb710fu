#include "CdcLogRing.h"

static int try_lock(struct tb710fu_cdc_log_ring *ring)
{
    return __atomic_exchange_n(&ring->lock, 1, __ATOMIC_ACQUIRE) == 0;
}

static void unlock(struct tb710fu_cdc_log_ring *ring)
{
    __atomic_store_n(&ring->lock, 0, __ATOMIC_RELEASE);
}

static void drop_bytes(struct tb710fu_cdc_log_ring *ring, size_t length)
{
    __atomic_fetch_add(&ring->dropped_bytes, (uint64_t)length, __ATOMIC_RELAXED);
}

static void ring_copy_in(struct tb710fu_cdc_log_ring *ring, uint64_t index,
                         const uint8_t *bytes, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        ring->storage[(size_t)((index + i) % ring->capacity)] = bytes[i];
    }
}

static void ring_copy_out(struct tb710fu_cdc_log_ring *ring, uint64_t index,
                          uint8_t *out, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        out[i] = ring->storage[(size_t)((index + i) % ring->capacity)];
    }
}

void tb710fu_cdc_log_ring_init(struct tb710fu_cdc_log_ring *ring,
                               uint8_t *storage, size_t capacity)
{
    if (ring == NULL) {
        return;
    }
    ring->storage = storage;
    ring->capacity = storage == NULL ? 0 : capacity;
    ring->read_index = 0;
    ring->write_index = 0;
    ring->dropped_bytes = 0;
    ring->lock = 0;
}

size_t tb710fu_cdc_log_ring_append(struct tb710fu_cdc_log_ring *ring,
                                   const uint8_t *bytes, size_t length)
{
    if (ring == NULL || bytes == NULL || length == 0 ||
        ring->storage == NULL || ring->capacity == 0) {
        return 0;
    }
    if (!try_lock(ring)) {
        drop_bytes(ring, length);
        return 0;
    }
    const uint64_t used = ring->write_index - ring->read_index;
    const size_t available = ring->capacity - (size_t)used;
    const size_t written = length < available ? length : available;
    ring_copy_in(ring, ring->write_index, bytes, written);
    ring->write_index += written;
    drop_bytes(ring, length - written);
    unlock(ring);
    return written;
}

size_t tb710fu_cdc_log_ring_copy(struct tb710fu_cdc_log_ring *ring,
                                 uint8_t *out, size_t capacity)
{
    if (ring == NULL || out == NULL || capacity == 0 ||
        ring->storage == NULL || ring->capacity == 0 || !try_lock(ring)) {
        return 0;
    }
    const uint64_t pending = ring->write_index - ring->read_index;
    const size_t length = capacity < pending ? capacity : (size_t)pending;
    ring_copy_out(ring, ring->read_index, out, length);
    unlock(ring);
    return length;
}

int tb710fu_cdc_log_ring_consume(struct tb710fu_cdc_log_ring *ring,
                                 size_t length)
{
    if (ring == NULL || ring->storage == NULL || ring->capacity == 0 ||
        !try_lock(ring)) {
        return 0;
    }
    const uint64_t pending = ring->write_index - ring->read_index;
    if ((uint64_t)length > pending) {
        unlock(ring);
        return 0;
    }
    ring->read_index += length;
    unlock(ring);
    return 1;
}

size_t tb710fu_cdc_log_ring_pending(struct tb710fu_cdc_log_ring *ring)
{
    if (ring == NULL || ring->storage == NULL || ring->capacity == 0 ||
        !try_lock(ring)) {
        return 0;
    }
    const size_t pending = (size_t)(ring->write_index - ring->read_index);
    unlock(ring);
    return pending;
}

uint64_t tb710fu_cdc_log_ring_dropped(struct tb710fu_cdc_log_ring *ring)
{
    if (ring == NULL) {
        return 0;
    }
    return __atomic_load_n(&ring->dropped_bytes, __ATOMIC_RELAXED);
}
