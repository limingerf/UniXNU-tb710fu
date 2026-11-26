// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tb710fuSpmiMmio.c - bounds-checked 32-bit access to the mapped arbiter.
//
// Ordering: the arbiter latches a command when the command word is written
// and re-arms STATUS for that transfer. A store followed by a load must not be
// reordered by the compiler, and on the kernel side it must not pass the
// device mapping either, so every access is followed/preceded by a fence.
// XNU's own OSMemoryBarrier() is used in a kernel build; the host build uses
// the compiler's sequentially consistent fence.
#include "Tb710fuSpmiMmio.h"

#include <stddef.h>
#include <stdint.h>

#ifdef KERNEL
#include <IOKit/IOLib.h>
#define TB710FU_MMIO_FENCE() OSMemoryBarrier()
#else
#define TB710FU_MMIO_FENCE() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

struct tb710fu_spmi_mmio_window {
    volatile uint32_t *base;
    unsigned bytes;
};

static int tb710fu_mmio_window_valid(const struct tb710fu_spmi_mmio_window *w,
                                     unsigned minimum)
{
    return w->base != NULL && w->bytes >= minimum &&
        (((uintptr_t)w->base) & 3u) == 0u;
}

static int tb710fu_mmio_offset_valid(const struct tb710fu_spmi_mmio_window *w,
                                     unsigned byte_offset)
{
    return (byte_offset & 3u) == 0u && byte_offset < w->bytes &&
        w->bytes - byte_offset >= 4u;
}

static struct tb710fu_spmi_mmio_window tb710fu_mmio_window(
    struct tb710fu_spmi_mmio *mmio, enum tb710fu_spmi_window window)
{
    struct tb710fu_spmi_mmio_window result = {NULL, 0u};

    switch (window) {
    case TB710FU_SPMI_WINDOW_CORE:
        result.base = mmio->core;
        result.bytes = mmio->core_bytes;
        break;
    case TB710FU_SPMI_WINDOW_CONFIG:
        result.base = mmio->config;
        result.bytes = mmio->config_bytes;
        break;
    case TB710FU_SPMI_WINDOW_OBSERVER:
        result.base = mmio->observer;
        result.bytes = mmio->observer_bytes;
        break;
    case TB710FU_SPMI_WINDOW_CHANNEL:
        result.base = mmio->channel;
        result.bytes = mmio->channel_bytes;
        break;
    }
    return result;
}

static uint32_t tb710fu_mmio_read32(void *context,
                                    enum tb710fu_spmi_window window,
                                    unsigned byte_offset)
{
    struct tb710fu_spmi_mmio *mmio = context;
    const struct tb710fu_spmi_mmio_window w =
        tb710fu_mmio_window(mmio, window);

    if (!tb710fu_mmio_offset_valid(&w, byte_offset)) {
        ++mmio->faults;
        return 0u;
    }
    TB710FU_MMIO_FENCE();
    return *(volatile uint32_t *)((uintptr_t)w.base + byte_offset);
}

static void tb710fu_mmio_write32(void *context,
                                 enum tb710fu_spmi_window window,
                                 unsigned byte_offset, uint32_t value)
{
    struct tb710fu_spmi_mmio *mmio = context;
    const struct tb710fu_spmi_mmio_window w =
        tb710fu_mmio_window(mmio, window);

    if (!tb710fu_mmio_offset_valid(&w, byte_offset)) {
        ++mmio->faults;
        return;
    }
    *(volatile uint32_t *)((uintptr_t)w.base + byte_offset) = value;
    TB710FU_MMIO_FENCE();
}

int tb710fu_spmi_mmio_init(struct tb710fu_spmi_mmio *mmio,
                           volatile uint32_t *core, unsigned core_bytes,
                           volatile uint32_t *config, unsigned config_bytes,
                           volatile uint32_t *observer, unsigned observer_bytes,
                           volatile uint32_t *channel, unsigned channel_bytes,
                           unsigned ee)
{
    struct tb710fu_spmi_mmio_window w;

    if (mmio == NULL || ee > TB710FU_SPMI_MAX_EE)
        return TB710FU_SPMI_ARGUMENT;
    /* The sizes are the tables the devicetree declares: the version/features
     * words plus one word per APID, one owner word per APID, one 0x20-byte
     * channel per APID, and an 0x8000 block per EE. */
    w = (struct tb710fu_spmi_mmio_window){core, core_bytes};
    if (!tb710fu_mmio_window_valid(&w, TB710FU_SPMI_APID_MAP_BASE +
                                       4u * TB710FU_SPMI_MAX_APIDS))
        return TB710FU_SPMI_ARGUMENT;
    w = (struct tb710fu_spmi_mmio_window){config, config_bytes};
    if (!tb710fu_mmio_window_valid(&w, 4u * TB710FU_SPMI_MAX_APIDS))
        return TB710FU_SPMI_ARGUMENT;
    w = (struct tb710fu_spmi_mmio_window){observer, observer_bytes};
    if (!tb710fu_mmio_window_valid(
            &w, TB710FU_SPMI_OBSERVER_EE_STRIDE * (ee + 1u)))
        return TB710FU_SPMI_ARGUMENT;
    w = (struct tb710fu_spmi_mmio_window){channel, channel_bytes};
    if (!tb710fu_mmio_window_valid(&w,
                                   TB710FU_SPMI_CHANNEL_APID_STRIDE *
                                       TB710FU_SPMI_MAX_APIDS))
        return TB710FU_SPMI_ARGUMENT;

    mmio->core = core;
    mmio->core_bytes = core_bytes;
    mmio->config = config;
    mmio->config_bytes = config_bytes;
    mmio->observer = observer;
    mmio->observer_bytes = observer_bytes;
    mmio->channel = channel;
    mmio->channel_bytes = channel_bytes;
    mmio->faults = 0u;
    mmio->io.context = mmio;
    mmio->io.read32 = tb710fu_mmio_read32;
    mmio->io.write32 = tb710fu_mmio_write32;
    return TB710FU_SPMI_OK;
}

unsigned tb710fu_spmi_mmio_faults(const struct tb710fu_spmi_mmio *mmio)
{
    return mmio != NULL ? mmio->faults : 0u;
}
