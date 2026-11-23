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
    size_t (*log_copy)(void *, uint8_t *, size_t);
    int (*log_consume)(void *, size_t);
    // Optional: paints the eight-character lane tag without rebuilding a frame,
    // so a bus access that never returns still leaves its step on screen.
    void (*lane)(void *, const char *);
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
    enum tb710fu_dwc3_control_phase phase;
    uint8_t running, failed, configured, dtr, notification_pending;
    uint8_t data_in, set_line, pending_address_valid;
    uint8_t coding[7], pending_address;
    uint8_t connected, setup_pending, tx_state;
    uint8_t pending_configuration_valid, pending_configuration;
    uint8_t pending_dtr_valid, pending_dtr;
    char serial_copy[32];
    uint16_t control_bytes;
    size_t tx_commit;
    uint64_t events, setup_requests, transmitted, discarded_input;
    uint32_t last_event;
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
    uint8_t reset_pad;
};

// The XNU adapter must first validate live role/clocks/PHY/SMMU and permanent,
// contiguous identity DMA. A failure after start never authorizes freeing DMA.
// Context must initially be zeroed. Restarting an initialized context is refused.
// Serial must contain 1..30 printable ASCII characters, keeping every selected
// control reply below an aligned short response that would require DATA/ZLP chaining.
// A bus reset with only EP0 SETUP armed preserves that transfer (normal host
// enumeration). Other active transfers require proven cancellation; this core
// latches failure instead of an event-ring rewind/unproven DMA-buffer reuse.
int tb710fu_dwc3_start(struct tb710fu_dwc3 *, const struct tb710fu_dwc3_ops *,
                       uint8_t *dma, uint64_t dma_physical,
                       uint16_t vendor_id, uint16_t product_id, const char *serial);
void tb710fu_dwc3_poll(struct tb710fu_dwc3 *);
void tb710fu_dwc3_enable_event_irq(struct tb710fu_dwc3 *);
void tb710fu_dwc3_stop(struct tb710fu_dwc3 *, const char *reason);

#endif
