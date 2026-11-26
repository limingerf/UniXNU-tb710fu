// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef TB710FU_CDC_DWC3_H
#define TB710FU_CDC_DWC3_H

#include <stddef.h>
#include <stdint.h>

#define TB710FU_DWC3_DMA_SIZE 0x4000u
#define TB710FU_DWC3_EVENTS 0x0000u
#define TB710FU_DWC3_EVENT_BYTES 0x1000u
#define TB710FU_DWC3_TRB_BASE 0x1000u
#define TB710FU_DWC3_TRB_STRIDE 64u
#define TB710FU_DWC3_SETUP 0x1200u
#define TB710FU_DWC3_CONTROL 0x1300u
#define TB710FU_DWC3_NOTIFY 0x1500u
#define TB710FU_DWC3_RX 0x1800u
#define TB710FU_DWC3_TX 0x2000u
#define TB710FU_DWC3_TX_BYTES 2048u

// One worker owns the controller and consumes logs. The producer remains the
// nonblocking RAM tap. sync_for_cpu must invalidate device-written cache lines;
// sync_for_device must clean CPU writes before ownership passes to hardware.
struct tb710fu_dwc3_ops {
    void *context;
    uint32_t (*read32)(void *, unsigned);
    void (*write32)(void *, unsigned, uint32_t);
    void (*delay_us)(void *, unsigned);
    void (*sync_for_cpu)(void *, unsigned, unsigned);
    void (*sync_for_device)(void *, unsigned, unsigned);
    // Bulk IN log source. read copies up to capacity bytes and reports the ring
    // position just past the span in *end; commit retires that exact position
    // once the transfer completed. commit must not fail: the RAM ring is
    // drop-oldest, so a producer may retire the span while it is on the wire.
    size_t (*log_read)(void *, uint8_t *, size_t, uint64_t *);
    void (*log_commit)(void *, uint64_t);
    // Optional: paints the eight-character lane tag without rebuilding a frame,
    // so a bus access that never returns still leaves its step on screen.
    void (*lane)(void *, const char *);
    // Optional: receives host-to-device bulk OUT bytes on EP_RX.
    void (*on_rx)(void *, const uint8_t *, unsigned);
};

enum tb710fu_dwc3_control_phase {
    TB710FU_DWC3_SETUP_PHASE,
    TB710FU_DWC3_DATA_PHASE,
    TB710FU_DWC3_WAIT_STATUS2,
    TB710FU_DWC3_WAIT_STATUS3,
    TB710FU_DWC3_STATUS_PHASE
};

struct tb710fu_dwc3 {
    struct tb710fu_dwc3_ops ops;
    uint8_t *dma;
    uint64_t dma_physical;
    uint16_t vendor_id, product_id;
    const char *serial;
    const char *failure;
    uint32_t event_position, packet_size;
    uint32_t requested[6], resource[6];
    uint8_t busy[6];
    // Set when a USB reset, a disconnect or a host pipe reset abandoned this
    // endpoint's in-flight transfer. Hardware ended that DMA with the reset,
    // so exactly one late completion per abandoned endpoint is dropped instead
    // of being attributed to the next transfer; the endpoint stays
    // quarantined (stale_until) across that window. Cleared when a new transfer
    // is armed or when the quarantine expires.
    uint8_t stale[6];
    uint64_t stale_until[6];
    // Set when the core, not this driver, still owns the End Transfer command
    // posted for this endpoint. Nothing may be armed on it until that command
    // completes: a Start Transfer issued over a transfer the core has not
    // retired is what the core answers with "endpoint busy", and in the
    // previous code that status latched failure and took EP0 down with it.
    // cancel_until bounds that wait, so an event the ring never delivers cannot
    // wedge an endpoint for the rest of the boot.
    uint8_t cancel_pending[6];
    uint8_t cancel_pad[2];
    uint64_t cancel_until[6];
    // Poll at which busy[ep] was set. pump() uses it only in panic mode, where
    // nothing else can ever complete a TRB a halted worker never submitted.
    uint64_t armed_at[6];
    enum tb710fu_dwc3_control_phase phase;
    // panic_mode is set by tb710fu_dwc3_panic_rearm(): from then on the worker
    // that normally drives this controller is gone, so pump() bounds how long
    // a Bulk transfer may stay owned before it is retired and re-armed.
    uint8_t running, failed, configured, dtr, notification_pending, panic_mode;
    uint8_t data_in, set_line, pending_address_valid;
    uint8_t coding[7], pending_address;
    uint8_t connected, setup_pending, tx_state;
    uint8_t pending_configuration_valid, pending_configuration;
    uint8_t pending_dtr_valid, pending_dtr;
    char serial_copy[32];
    uint16_t control_bytes;
    size_t tx_commit;
    uint64_t tx_end;
    uint64_t zlp_armed;
    uint64_t events, setup_requests, transmitted, discarded_input;
    uint32_t last_event;
    // Recovery evidence: how many endpoints a host reset/disconnect/pipe reset
    // had to abandon, how many late completions were dropped for them, how many
    // bus resets were survived, and how often EP0 lost and regained its SETUP.
    uint64_t polls, abandoned_transfers, stale_completions, resets, ep0_rearms;
    // Close/reopen evidence: the poll at which a control stage last advanced,
    // how many control pipes were recycled after the host abandoned a stage,
    // how many End Transfer commands completed, how many completions arrived for
    // an endpoint nobody owns, how many Bulk IN spans the host aborted, how many
    // transfer starts the core refused while it was still terminating, and how
    // often an End Transfer completion had to be assumed after its window.
    uint64_t control_polls, ep0_recoveries, cancel_completions;
    uint64_t orphan_completions, aborted_tx, deferred_starts, cancel_timeouts;
    // Core reset evidence: DCTL before the halt, right after the assert, and
    // GCTL/phycfg after the preconditions, plus the state registers read at the
    // end. reset_stage is how the bit finally cleared, 0 when it never did.
    uint32_t reset_trace[6];
    // A core that never answered the reset request is still driven: reset_forced
    // records that, and probe_trace keeps the readbacks of the writes actually
    // issued afterwards plus DSTS as read once the pull-up was exposed, so a
    // single frame says whether the core accepts configuration at all.
    uint32_t probe_trace[6];
    uint8_t reset_stage;
    uint8_t reset_adopted;
    uint8_t reset_forced;
    // Set only when start() completed: the core was halted and reset, the
    // event ring and every endpoint were configured and RUN_STOP was raised.
    // It is never cleared, because a transport failure before a panic (which
    // clears DEVTEN/RUN_STOP) must not hide the channel from the panic shell.
    uint8_t adopted;
};

// The XNU adapter must first validate live role/clocks/PHY/SMMU and permanent,
// contiguous identity DMA. A failure after start never authorizes freeing DMA.
// Context must initially be zeroed. Restarting an initialized context is refused.
// Serial must contain 1..30 printable ASCII characters, keeping every selected
// control reply below an aligned short response that would require DATA/ZLP chaining.
// A bus reset or disconnect abandons every in-flight endpoint instead of latching
// failure: the host re-enumerates, EP0 SETUP is preserved when it was already
// armed, and a late completion for an abandoned transfer is dropped once.
int tb710fu_dwc3_start(struct tb710fu_dwc3 *, const struct tb710fu_dwc3_ops *,
                       uint8_t *dma, uint64_t dma_physical,
                       uint16_t vendor_id, uint16_t product_id, const char *serial);
void tb710fu_dwc3_poll(struct tb710fu_dwc3 *);
void tb710fu_dwc3_enable_event_irq(struct tb710fu_dwc3 *);
void tb710fu_dwc3_stop(struct tb710fu_dwc3 *, const char *reason);
// A panic stops the worker that normally drives the controller. This re-asserts
// only the state start()/stop() own - pull-up and device events - and reclaims
// every endpoint before re-arming EP0, leaving every DMA address untouched, so
// a COM5 opened during or after the panic is answered instead of never being
// heard at all. It is the panic hand-over: it runs once, unconditionally, on
// a controller that start() adopted, even when that controller still looked
// healthy immediately before the panic.
void tb710fu_dwc3_panic_rearm(struct tb710fu_dwc3 *);

#endif
