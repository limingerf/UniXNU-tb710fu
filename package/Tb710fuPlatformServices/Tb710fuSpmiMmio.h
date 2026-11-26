// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tb710fuSpmiMmio.h - the real MMIO transport of the arbiter core.
//
// The kernel maps the four windows the core reads of spmi@c400000 once and
// hands them here;
// the core then talks to the hardware through the same struct tb710fu_spmi_io
// the host model implements. Every access is bounds-checked against the
// window size the devicetree declares, so a corrupt APID or EE can never walk
// off a mapping:
//
//   core     0x0c400000, 0x3000  (version, features, 1024 APID map words)
//   cnfg     0x0c42d000, 0x4000  (1024 owner words at +4*apid)
//   obsrvr   0x0c440000, 0x80000 (0x8000 per EE, 0x20 per APID)
//   chnls    0x0c500000, 0x400000 (0x1000 per APID)
//   intr     0x0c4c0000, 0x20000 (irq status, not used by this core)
//
// The interrupt window is part of the devicetree node but the synchronous
// polling core never touches it; it is not mapped here until an interrupt
// path exists.
#ifndef TB710FU_SPMI_MMIO_H
#define TB710FU_SPMI_MMIO_H

#include "Tb710fuSpmiArbiter.h"

struct tb710fu_spmi_mmio {
    volatile uint32_t *core;
    unsigned core_bytes;
    volatile uint32_t *config;
    unsigned config_bytes;
    volatile uint32_t *observer;
    unsigned observer_bytes;
    volatile uint32_t *channel;
    unsigned channel_bytes;
    /* Out-of-range accesses. The transport cannot fail a read, so it returns
     * zero and counts the fault; tb710fu_spmi_probe() then refuses the layout
     * (version 0 is not v7), which keeps a broken mapping from looking like a
     * valid but empty arbiter. */
    unsigned faults;
    struct tb710fu_spmi_io io;
};

/* Validate and publish the transport. ee is this execution environment, used
 * only to require the observer window to cover its channel block. Returns a
 * tb710fu_spmi_result. */
int tb710fu_spmi_mmio_init(struct tb710fu_spmi_mmio *mmio,
                           volatile uint32_t *core, unsigned core_bytes,
                           volatile uint32_t *config, unsigned config_bytes,
                           volatile uint32_t *observer, unsigned observer_bytes,
                           volatile uint32_t *channel, unsigned channel_bytes,
                           unsigned ee);

unsigned tb710fu_spmi_mmio_faults(const struct tb710fu_spmi_mmio *mmio);

#endif
