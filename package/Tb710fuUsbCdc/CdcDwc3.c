// SPDX-License-Identifier: BSD-2-Clause-Patent
// Device-owned CDC logging implementation. Register/USB programming facts:
// Linux v6.1 drivers/usb/dwc3/{core.h,ep0.c,gadget.c}; reviewed alongside
// QcomXnuUsbDebug@18bcc385 (MIT, itself derived from m1n1). No Vermeer code.
// Board clocks, PHY, PMIC, SMMU and resource acquisition belong to CdcXnu.
#include "CdcDwc3.h"
#include "CdcAcmProtocol.h"

#define GCTL 0xc110u
#define GSTS 0xc118u
#define GUCTL1 0xc11cu
#define GSNPSID 0xc120u
#define GHWPARAMS1 0xc144u
#define GHWPARAMS3 0xc14cu
#define USB2PHYCFG 0xc200u
#define GUSB3PIPECTL 0xc2c0u
#define EVENT_LO(n) (0xc400u + (n) * 16u)
#define EVENT_HI(n) (0xc404u + (n) * 16u)
#define EVENT_SIZE(n) (0xc408u + (n) * 16u)
#define EVENT_COUNT(n) (0xc40cu + (n) * 16u)
#define DCFG 0xc700u
#define DCTL 0xc704u
#define DEVTEN 0xc708u
#define DSTS 0xc70cu
#define DALEPENA 0xc720u
#define EP_PAR2(n) (0xc800u + (n) * 16u)
#define EP_PAR1(n) (0xc804u + (n) * 16u)
#define EP_PAR0(n) (0xc808u + (n) * 16u)
#define EP_CMD(n) (0xc80cu + (n) * 16u)
#define RUN_STOP (1u << 31)
#define SOFT_RESET (1u << 30)
#define CONTROLLER_HALTED (1u << 22)
#define COMMAND_ACTIVE (1u << 10)
#define COMMAND_CONFIGURE 1u
#define COMMAND_RESOURCE 2u
#define COMMAND_STALL 4u
#define COMMAND_CLEAR_STALL 5u
#define COMMAND_START 6u
#define COMMAND_START_CONFIGURATION 9u
// Linux v6.1 gadget.c:1690-1730 (__dwc3_stop_active_transfer) and ep0.c:1096-1112
// (dwc3_ep0_end_control_data): a transfer the host abandoned stays owned by the
// core until an End Transfer command retires it. DWC_usb31 completes that command
// asynchronously and has no GUCTL2[14] CMDACT-polling mode, so CMDIOC is set and
// the completion is read from the endpoint event ring instead of from CMDACT.
// DEPCMD bit layout, drivers/usb/dwc3/core.h (v6.1): CMDACT BIT(10),
// HIPRI_FORCERM/CLEARPENDIN BIT(11), CMDIOC BIT(8), and CMD_STATUS is the
// read-only field at bits 15:12 (DWC3_DEPCMD_STATUS(x) = (x >> 12) & 0xf).
// pl42/pl43 wrote CMDIOC as bit 12, i.e. into CMD_STATUS: no command-complete
// event was ever requested and every readback then looked like a failed command.
#define COMMAND_ENDTRANSFER 8u
#define COMMAND_CMDIOC (1u << 8)
#define CANCEL_POLLS 20u
#define PHY_SUSPEND ((1u << 6) | (1u << 8))
#define PIPE_SUSPEND (1u << 17)
#define GCTL_DISABLE_CLK_GATING 1u
#define GUCTL1_FORCE_20_CLK_FOR_30_CLK (1u << 26)
#define DCTL_LPM_MASK (0x3ffu << 19)
// One microsecond per poll gave the core 10 ms to answer a reset and a command.
// Linux allows 500 ms for the reset alone, and this core is adopted from
// firmware with its clock still gated by whatever the boot chain left running;
// a window that short reads as "never answered" even when the answer is merely
// late. Each phase now polls a realistic window, and the delay step is the
// clock the report already carries (cntv_ctl=1), not a fixed spin count.
#define WAIT_POLLS 2000u
#define WAIT_DELAY_US 100u
#define EVENT_BUDGET 64u
#define EVENT_COUNT_MASK 0xfffcu
#define EVENT_INTERRUPT_MASK (1u << 31)
#define TRB_HWO 1u
#define TRB_LAST 2u
#define TRB_IOC (1u << 11)
#define TRB_SHORT (1u << 10)
#define TRB_NORMAL 1u
#define TRB_SETUP 2u
#define TRB_STATUS2 3u
#define TRB_STATUS3 4u
#define TRB_DATA 5u
#define LENGTH_MASK 0x00ffffffu
#define EP_RX 2u
#define EP_TX 3u
#define EP_NOTIFY 5u
#define ENABLED_ENDPOINTS 0x2fu
#define TX_IDLE 0u
#define TX_ACTIVE 1u
#define TX_COMMIT 2u
#define TX_ZLP_PENDING 3u
#define TX_ZLP_ACTIVE 4u

static void zero_bytes(void *target, size_t count)
{
    uint8_t *p = target;
    for (size_t i = 0; i < count; ++i) p[i] = 0;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8));
}

static uint32_t read_reg(struct tb710fu_dwc3 *d, unsigned reg)
{
    return d->ops.read32(d->ops.context, reg);
}

static void write_reg(struct tb710fu_dwc3 *d, unsigned reg, uint32_t value)
{
    d->ops.write32(d->ops.context, reg, value);
}

static int wait_reg(struct tb710fu_dwc3 *d, unsigned reg, uint32_t mask, uint32_t expected)
{
    for (unsigned i = 0; i < WAIT_POLLS; ++i) {
        if ((read_reg(d, reg) & mask) == expected) return 1;
        d->ops.delay_us(d->ops.context, WAIT_DELAY_US);
    }
    return 0;
}

static int halt_firmware(struct tb710fu_dwc3 *d, unsigned interrupts)
{
    // Linux v6.1 gadget.c:2555-2560: pending events can prevent DEVCTLHLT.
    // Acknowledge old producer counts without replacing its DMA addresses or
    // accessing its memory. The whole adoption still has one bounded timeout.
    for (unsigned poll = 0; poll < WAIT_POLLS; ++poll) {
        for (unsigned i = 0; i < interrupts; ++i) {
            const uint32_t count = read_reg(d, EVENT_COUNT(i));
            if (count & 3u) return 0;
            if (count & EVENT_COUNT_MASK)
                write_reg(d, EVENT_COUNT(i), count & EVENT_COUNT_MASK);
        }
        if (read_reg(d, DSTS) & CONTROLLER_HALTED) return 1;
        d->ops.delay_us(d->ops.context, WAIT_DELAY_US);
    }
    return 0;
}

void tb710fu_dwc3_stop(struct tb710fu_dwc3 *d, const char *reason)
{
    if (d == NULL) return;
    if (!d->failed) d->failure = reason;
    d->failed = 1;
    d->running = 0;
    // Never infer endpoint ownership from CMDACT, especially on USB31. Even
    // successful global halt here does not authorize freeing/restarting DMA.
    if (d->ops.read32 != NULL && d->ops.write32 != NULL) {
        write_reg(d, DEVTEN, 0);
        // Stop clears both bits: leaving a stuck CSFTRST in DCTL would keep the
        // core claiming reset for whatever runs next.
        write_reg(d, DCTL, read_reg(d, DCTL) & ~(RUN_STOP | SOFT_RESET));
        if (d->ops.delay_us != NULL)
            (void)wait_reg(d, DSTS, CONTROLLER_HALTED, CONTROLLER_HALTED);
    }
}

static int fail(struct tb710fu_dwc3 *d, const char *reason)
{
    tb710fu_dwc3_stop(d, reason);
    return 0;
}

// Polls an abandoned endpoint stays out of service. Long enough for the core to
// queue the cancellation completion it owes for the terminated transfer, short
// enough that a host that reopens the port still finds a live channel.
#define STALE_POLLS 4u

// Polls a bare Bulk IN ZLP may stay unclaimed before it is retired. The worker
// polls every 2 ms once configured, so four polls are ~8 ms: any host that
// wanted that terminator issues its IN token well inside this window.
#define ZLP_POLLS 4u

// Polls a Bulk transfer may keep an endpoint owned in panic mode before it is
// retired and re-armed. A CPU halted inside transfer() can leave busy[] set for
// a TRB the core never received; no completion can ever arrive for it, and the
// panic shell would then wait on that endpoint for the rest of the boot. 32
// polls are 16 ms at the panic loop's 500 us cadence (64 ms at the worker's
// 2 ms cadence), far longer than any real 2 KiB Bulk transfer needs, so this
// can only fire on a transfer that is genuinely lost.
#define BULK_RETIRE_POLLS 32u

// Polls an outstanding End Transfer may keep an endpoint out of service. The
// command-complete event normally releases it within a few polls, but a lost
// event must never be able to kill an endpoint for the rest of the boot: that
// is exactly how a COM5 open reads zero bytes and the port disappears. After
// this window the caller retries; a Start Transfer the core still refuses
// simply defers again (deferred_starts), so releasing early cannot corrupt a
// live transfer - it only stops the wedge.
#define CANCEL_GIVEUP_POLLS 128u

// A control stage the host abandoned leaves no trace at all: this core reports
// nothing when the host stops driving a stage. Silence for a full control window
// is the only evidence, and recycling EP0 after it is what keeps the next open
// answerable (2 s at the 2 ms configured cadence, 100 ms during the 100 us early
// loop). Windows itself gives a control request several seconds, so this window
// never races a slow but live host.
#define EP0_CONTROL_POLLS 1000u

static void abandon_endpoint(struct tb710fu_dwc3 *d, unsigned ep);

// A host reset, a disconnect, or a host pipe reset ends that endpoint's DMA on
// the device side. Software cannot prove the TRB free from a completion, so the
// endpoint is quarantined: the next completion for it is dropped instead of
// being credited to a new transfer, and it is not re-armed until the quarantine
// expires. None of this fails the transport - latching failure on a normal
// Windows close/open cycle is what took COM5 down for good.
static void cancel_endpoint(struct tb710fu_dwc3 *d, unsigned ep)
{
    if (ep >= 6u || ep == 4u || d->cancel_pending[ep] || !d->busy[ep]) return;
    // Linux v6.1 gadget.c dwc3_stop_active_transfer(): End Transfer is only issued
    // to the control endpoint while its Data Phase is actually running. A SETUP
    // packet still waiting to be DMA'd out blocks that command on every endpoint,
    // and a STATUS stage has no transfer left to terminate.
    if (ep <= 1u && d->phase != TB710FU_DWC3_DATA_PHASE) return;
    // Resource index zero is valid for EP0 OUT only (Linux v6.1 ep0.c:1096-1106).
    if (ep != 0u && d->resource[ep] == 0u) return;
    write_reg(d, EP_PAR0(ep), 0);
    write_reg(d, EP_PAR1(ep), 0);
    write_reg(d, EP_PAR2(ep), 0);
    write_reg(d, EP_CMD(ep), COMMAND_ENDTRANSFER | COMMAND_CMDIOC |
                             COMMAND_ACTIVE | ((uint32_t)d->resource[ep] << 16));
    // DWC_usb31 has no CMDACT polling mode, so a command that is still running
    // legitimately keeps CMDACT set. Only a command that already retired with
    // success frees the endpoint here; anything else waits for its event.
    int retired = 0;
    for (unsigned i = 0; i < CANCEL_POLLS; ++i) {
        if (!(read_reg(d, EP_CMD(ep)) & COMMAND_ACTIVE)) {
            retired = 1;
            break;
        }
        d->ops.delay_us(d->ops.context, 10u);
    }
    if (retired && !((read_reg(d, EP_CMD(ep)) >> 12) & 15u)) {
        // Completed and the core retired the transfer: the endpoint is free now.
        d->resource[ep] = 0;
        ++d->cancel_completions;
        return;
    }
    // Still terminating. Nothing may be armed here until the command-complete
    // event for this endpoint arrives, and CANCEL_GIVEUP_POLLS bounds that wait
    // so a completion the ring loses cannot wedge the channel forever.
    d->cancel_pending[ep] = 1;
    d->cancel_until[ep] = d->polls + CANCEL_GIVEUP_POLLS;
}

static void abandon_endpoint(struct tb710fu_dwc3 *d, unsigned ep)
{
    if (ep >= 6u || ep == 4u || !d->busy[ep]) return;
    cancel_endpoint(d, ep);
    d->busy[ep] = 0;
    d->requested[ep] = 0;
    d->resource[ep] = 0;
    d->stale[ep] = 1;
    d->stale_until[ep] = d->polls + STALE_POLLS;
    ++d->abandoned_transfers;
}

static int endpoint_usable(struct tb710fu_dwc3 *d, unsigned ep)
{
    if (ep >= 6u || ep == 4u) return 0;
    // The core still owns this endpoint's cancellation. Arming it now is the
    // collision that a reopen used to hit. The wait is bounded: a command
    // completion the event ring never delivers must not take EP0 - the only
    // channel this port has - out of service for the rest of the boot.
    if (d->cancel_pending[ep]) {
        if (d->polls < d->cancel_until[ep]) return 0;
        d->cancel_pending[ep] = 0;
        ++d->cancel_timeouts;
    }
    if (!d->stale[ep]) return 1;
    if (d->polls < d->stale_until[ep]) return 0;
    d->stale[ep] = 0;
    return 1;
}

// Panic context only: hand an endpoint back without posting any command. The
// core may still own the TRB (its completion then arrives for an endpoint
// nobody owns and is dropped as an orphan), or may never have received it at
// all because the CPU was halted inside transfer() before STARTTRANSFER ran.
// The stale window covers both: a late completion is never credited to the
// transfer the re-arm is about to start.
static void retire_bulk_owned(struct tb710fu_dwc3 *d, unsigned ep)
{
    if (ep >= 6u || ep == 4u || !d->busy[ep]) return;
    d->busy[ep] = 0;
    d->requested[ep] = 0;
    d->resource[ep] = 0;
    d->armed_at[ep] = 0;
    d->stale[ep] = 1;
    d->stale_until[ep] = d->polls + STALE_POLLS;
    ++d->abandoned_transfers;
}

static int command(struct tb710fu_dwc3 *d, unsigned ep, unsigned cmd,
                   uint32_t p0, uint32_t p1, uint32_t p2)
{
    const uint32_t phy = read_reg(d, USB2PHYCFG);
    if (phy & PHY_SUSPEND) write_reg(d, USB2PHYCFG, phy & ~PHY_SUSPEND);
    write_reg(d, EP_PAR0(ep), p0);
    write_reg(d, EP_PAR1(ep), p1);
    write_reg(d, EP_PAR2(ep), p2);
    write_reg(d, EP_CMD(ep), cmd | COMMAND_ACTIVE);
    const int complete = wait_reg(d, EP_CMD(ep), COMMAND_ACTIVE, 0);
    const uint32_t result = read_reg(d, EP_CMD(ep));
    if (phy & PHY_SUSPEND) write_reg(d, USB2PHYCFG, phy);
    if (!complete) return fail(d, "endpoint command timeout; retain DMA");
    if ((result >> 12) & 15u) return fail(d, "endpoint command status; retain DMA");
    if (cmd == COMMAND_START) {
        d->resource[ep] = (result >> 16) & 127u;
        // Index zero is valid for EP0 OUT DATA/STATUS (Linux v6.1
        // ep0.c:1102-1106). Only CMDACT/command status establishes success;
        // busy[] tracks ownership independently of the resource value.
    }
    return 1;
}

static int configure_ep(struct tb710fu_dwc3 *d, unsigned ep, unsigned type,
                        unsigned mps, int modify)
{
    uint32_t p0 = (type << 1) | (mps << 3);
    // Physical endpoint index: EP0-out=0, EP0-in=1. IN FIFO is logical EP.
    if (ep & 1u) p0 |= (ep >> 1) << 17;
    if (modify) p0 |= 2u << 30;
    uint32_t p1 = (1u << 8) | (1u << 10) | (ep << 25);
    if (type == 3) p1 |= 8u << 16; // bInterval=9, HS exponent minus one.
    if (!command(d, ep, COMMAND_CONFIGURE, p0, p1, 0)) return 0;
    return modify || command(d, ep, COMMAND_RESOURCE, 1, 0, 0);
}

static unsigned trb_offset(unsigned ep)
{
    return TB710FU_DWC3_TRB_BASE + ep * TB710FU_DWC3_TRB_STRIDE;
}

// Start Transfer. A command the core refuses says one thing only: this endpoint
// still owns the transfer that the host abandoned, so there is nothing to start
// on it yet. Treating that as a transport failure stopped every other endpoint
// with it - EP0 included - and the next COM5 open answered with "A device
// attached to the system is not functioning".
static int start_ep(struct tb710fu_dwc3 *d, unsigned ep, uint32_t p0, uint32_t p1)
{
    const uint32_t phy = read_reg(d, USB2PHYCFG);
    if (phy & PHY_SUSPEND) write_reg(d, USB2PHYCFG, phy & ~PHY_SUSPEND);
    write_reg(d, EP_PAR0(ep), p0);
    write_reg(d, EP_PAR1(ep), p1);
    write_reg(d, EP_PAR2(ep), 0);
    write_reg(d, EP_CMD(ep), COMMAND_START | COMMAND_ACTIVE);
    const int complete = wait_reg(d, EP_CMD(ep), COMMAND_ACTIVE, 0);
    const uint32_t result = read_reg(d, EP_CMD(ep));
    if (phy & PHY_SUSPEND) write_reg(d, USB2PHYCFG, phy);
    if (!complete || ((result >> 12) & 15u)) {
        ++d->deferred_starts;
        return 0;
    }
    d->resource[ep] = (result >> 16) & 127u;
    return 1;
}

static int transfer(struct tb710fu_dwc3 *d, unsigned ep, unsigned type,
                    unsigned buffer, unsigned bytes)
{
    if (ep >= 6 || d->busy[ep] || buffer > TB710FU_DWC3_DMA_SIZE ||
        bytes > TB710FU_DWC3_DMA_SIZE - buffer)
        return fail(d, "invalid or overlapping transfer ownership");
    const unsigned off = trb_offset(ep);
    uint8_t *trb = d->dma + off;
    const uint64_t address = d->dma_physical + buffer;
    const uint64_t descriptor = d->dma_physical + off;
    put32(trb, (uint32_t)address);
    put32(trb + 4, (uint32_t)(address >> 32));
    put32(trb + 8, bytes);
    put32(trb + 12, TRB_HWO | TRB_LAST | TRB_IOC | TRB_SHORT | (type << 4));
    if (bytes) d->ops.sync_for_device(d->ops.context, buffer, bytes);
    d->ops.sync_for_device(d->ops.context, off, TB710FU_DWC3_TRB_STRIDE);
    d->requested[ep] = bytes;
    d->busy[ep] = 1; // A timed-out command may still have started device DMA.
    d->armed_at[ep] = d->polls;
    // A new transfer owns this endpoint from here on: any cancellation evidence
    // owed for the abandoned predecessor is no longer attributable to it.
    d->stale[ep] = 0;
    if (!start_ep(d, ep, (uint32_t)(descriptor >> 32), (uint32_t)descriptor)) {
        // Hand this endpoint back to the quarantine and let the next poll retry;
        // EP0 keeps serving the host in the meantime.
        d->busy[ep] = 0;
        d->requested[ep] = 0;
        d->stale[ep] = 1;
        d->stale_until[ep] = d->polls + STALE_POLLS;
    }
    return 1;
}

static int setup_phase(struct tb710fu_dwc3 *d)
{
    d->phase = TB710FU_DWC3_SETUP_PHASE;
    d->control_polls = d->polls;
    d->setup_pending = d->set_line = 0;
    d->pending_address_valid = d->pending_configuration_valid = d->pending_dtr_valid = 0;
    return transfer(d, 0, TRB_SETUP, TB710FU_DWC3_SETUP, 8);
}

static int stall_setup(struct tb710fu_dwc3 *d)
{
    if (d->busy[0] || d->busy[1]) return fail(d, "control stall while DMA active");
    if (!command(d, 0, COMMAND_STALL, 0, 0, 0)) return 0;
    return setup_phase(d);
}

static int data_in(struct tb710fu_dwc3 *d, unsigned bytes, unsigned requested)
{
    const unsigned sent = bytes < requested ? bytes : requested;
    d->data_in = 1;
    d->control_bytes = (uint16_t)sent;
    // Every selected reply is nonzero and not an exact EP0 packet multiple.
    // Serial strings are bounded to 30 characters (62-byte descriptors).
    // Supporting an aligned short response requires a chained DATA/ZLP pair,
    // not two independent STARTTRANSFER commands (Linux ep0.c:1016-1038).
    if (!sent || (sent < requested && (sent % 64u) == 0)) return stall_setup(d);
    d->phase = TB710FU_DWC3_DATA_PHASE;
    // Start DATA immediately after SETUP (Linux v6.1 ep0.c:142-173).
    return transfer(d, 1, TRB_DATA, TB710FU_DWC3_CONTROL, sent);
}

static int configure_data_endpoints(struct tb710fu_dwc3 *d)
{
    return configure_ep(d, EP_RX, 2, d->packet_size, 1) &&
           configure_ep(d, EP_TX, 2, d->packet_size, 1) &&
           configure_ep(d, EP_NOTIFY, 3, 16, 1);
}

static int setup_request(struct tb710fu_dwc3 *d)
{
    const uint8_t *s = d->dma + TB710FU_DWC3_SETUP;
    const uint16_t value = get16(s + 2), index = get16(s + 4), length = get16(s + 6);
    uint8_t *reply = d->dma + TB710FU_DWC3_CONTROL;
    const enum tb710fu_cdc_speed speed = d->packet_size == 512 ?
        TB710FU_CDC_HIGH_SPEED : TB710FU_CDC_FULL_SPEED;
    const enum tb710fu_cdc_request request = tb710fu_cdc_classify_setup(s[0], s[1], value, index, length);
    size_t n = 0;
    ++d->setup_requests;
    d->data_in = (s[0] >> 7) & 1u;
    switch (request) {
    case TB710FU_CDC_REQ_GET_DESCRIPTOR:
        switch (value >> 8) {
        case 1:
            n = tb710fu_cdc_build_device_descriptor(reply, 256, d->vendor_id, d->product_id, 0x0100);
            break;
        case 2:
            n = tb710fu_cdc_build_configuration_descriptor(reply, 256, speed, 0x80, 50);
            break;
        case 6:
            n = tb710fu_cdc_build_device_qualifier_descriptor(reply, 256);
            break;
        case 7:
            n = tb710fu_cdc_build_other_speed_configuration_descriptor(reply, 256, speed, 0x80, 50);
            break;
        case 3: {
            const char *text = (value & 255u) == 1 ? "TB710FU" :
                               (value & 255u) == 2 ? "TB710FU XNU logs" : d->serial;
            size_t chars = 0;
            if ((value & 255u) != 0) while (text[chars] != 0 && chars < 31) ++chars;
            n = tb710fu_cdc_build_string_descriptor(reply, 256, (uint8_t)value,
                                                   (value & 255u) ? text : NULL, chars);
            break;
        }
        default: break;
        }
        if (!n) return stall_setup(d);
        return data_in(d, (unsigned)n, length);
    case TB710FU_CDC_REQ_GET_CONFIGURATION:
        reply[0] = d->configured;
        return data_in(d, 1, length);
    case TB710FU_CDC_REQ_GET_STATUS:
        if ((s[0] & 31u) == 1 && !d->configured) return stall_setup(d);
        if ((s[0] & 31u) == 2 && (index & 15u) && !d->configured) return stall_setup(d);
        reply[0] = reply[1] = 0; // Bus powered; no advertised remote wakeup/halt.
        return data_in(d, 2, length);
    case TB710FU_CDC_REQ_SET_ADDRESS:
        if (d->configured) return stall_setup(d);
        // DWC3 hardware applies DEVADDR after STATUS. Linux ep0.c:587-614
        // writes DCFG during SETUP, rather than racing the next host token.
        write_reg(d, DCFG, (read_reg(d, DCFG) & ~(127u << 3)) | ((uint32_t)value << 3));
        d->pending_address_valid = 1;
        d->pending_address = (uint8_t)value;
        break;
    case TB710FU_CDC_REQ_SET_CONFIGURATION:
        // Reconfiguration with outstanding endpoints needs a proven cancel.
        if (d->configured || d->busy[EP_RX] || d->busy[EP_TX] || d->busy[EP_NOTIFY])
            return fail(d, "configuration change needs DMA cancellation");
        if (value && !configure_data_endpoints(d)) return 0;
        d->pending_configuration_valid = 1;
        d->pending_configuration = (uint8_t)value;
        break;
    case TB710FU_CDC_REQ_GET_INTERFACE:
        if (!d->configured) return stall_setup(d);
        reply[0] = 0;
        return data_in(d, 1, length);
    case TB710FU_CDC_REQ_SET_INTERFACE:
        if (!d->configured) return stall_setup(d);
        break; // Only the already active alternate setting zero is exposed.
    case TB710FU_CDC_REQ_GET_LINE_CODING:
        if (!d->configured) return stall_setup(d);
        for (unsigned i = 0; i < 7; ++i) reply[i] = d->coding[i];
        return data_in(d, 7, length);
    case TB710FU_CDC_REQ_SET_LINE_CODING:
        if (!d->configured) return stall_setup(d);
        d->data_in = 0;
        d->set_line = 1;
        d->control_bytes = 7;
        d->phase = TB710FU_DWC3_DATA_PHASE;
        return transfer(d, 0, TRB_DATA, TB710FU_DWC3_CONTROL, 64);
    case TB710FU_CDC_REQ_SET_CONTROL_LINE_STATE:
        if (!d->configured) return stall_setup(d);
        d->pending_dtr_valid = 1;
        d->pending_dtr = (uint8_t)(value & 1u);
        break;
    case TB710FU_CDC_REQ_SEND_BREAK:
        if (!d->configured) return stall_setup(d);
        break; // A log-only endpoint has no external serial break line.
    case TB710FU_CDC_REQ_CLEAR_ENDPOINT_HALT: {
        if (!d->configured) return stall_setup(d);
        const unsigned ep = ((index & 15u) << 1) | ((index >> 7) & 1u);
        if (ep >= 6u || ep == 4u) return stall_setup(d);
        // Windows clears the halt of a bulk pipe right after it aborted that
        // pipe's pending URB, which is exactly the COM5 close path. The
        // transfer it abandoned still owns this endpoint here, and latching
        // failure on this request stopped the transport and left EP0 dead for
        // the next open. Abandon it, clear the halt, and keep serving.
        if (d->busy[ep]) {
            if (ep == EP_TX) {
                // The host stopped reading this span mid-flight; nothing was
                // retired, so the ring keeps the bytes for the next transfer.
                d->tx_state = TX_IDLE;
                d->tx_commit = 0;
                d->tx_end = 0;
            }
            abandon_endpoint(d, ep);
        }
        if (!command(d, ep, COMMAND_CLEAR_STALL, 0, 0, 0)) return 0;
        break;
    }
    default:
        return stall_setup(d);
    }
    d->data_in = 0;
    d->phase = TB710FU_DWC3_WAIT_STATUS2;
    return 1;
}

static int control_not_ready(struct tb710fu_dwc3 *d, unsigned ep, unsigned status)
{
    d->control_polls = d->polls;
    if (status == 1 && d->phase == TB710FU_DWC3_DATA_PHASE) {
        if (ep != d->data_in) return fail(d, "new control direction while DMA active");
        return 1; // DATA was already started, not delayed until this event.
    }
    if (status != 2) return 1;
    if (d->phase != TB710FU_DWC3_WAIT_STATUS2 && d->phase != TB710FU_DWC3_WAIT_STATUS3)
        return fail(d, "unexpected control status phase");
    if (d->setup_pending) return stall_setup(d);
    const unsigned expected_ep = d->phase == TB710FU_DWC3_WAIT_STATUS2 ? 1u : !d->data_in;
    if (ep != expected_ep) return fail(d, "wrong control status direction");
    const unsigned kind = d->phase == TB710FU_DWC3_WAIT_STATUS2 ? TRB_STATUS2 : TRB_STATUS3;
    d->phase = TB710FU_DWC3_STATUS_PHASE;
    return transfer(d, ep, kind, TB710FU_DWC3_CONTROL, 0);
}

static int control_complete(struct tb710fu_dwc3 *d, unsigned ep, unsigned bytes,
                            unsigned trb_status)
{
    d->control_polls = d->polls;
    if (d->phase == TB710FU_DWC3_SETUP_PHASE) {
        if (ep != 0 || bytes != 8 || trb_status != 0) return fail(d, "invalid SETUP completion");
        d->ops.sync_for_cpu(d->ops.context, TB710FU_DWC3_SETUP, 8);
        return setup_request(d);
    }
    if (d->phase == TB710FU_DWC3_DATA_PHASE) {
        if (ep != d->data_in) return fail(d, "wrong DATA completion endpoint");
        if (trb_status == 2) {
            // Linux ep0.c:873-879,1146-1153: wait STATUS_NOTREADY, then
            // stall/restart. Directly reusing the SETUP TRB is premature.
            d->setup_pending = 1;
            d->phase = TB710FU_DWC3_WAIT_STATUS3;
            return 1;
        }
        if (trb_status != 0 || bytes != d->control_bytes) return stall_setup(d);
        if (d->set_line) {
            d->ops.sync_for_cpu(d->ops.context, TB710FU_DWC3_CONTROL, 64);
            const uint8_t *coding = d->dma + TB710FU_DWC3_CONTROL;
            const uint8_t bits = coding[6];
            if (get32(coding) == 0 || coding[4] > 2 || coding[5] > 4 ||
                (bits != 5 && bits != 6 && bits != 7 && bits != 8 && bits != 16))
                return stall_setup(d);
            for (unsigned i = 0; i < 7; ++i) d->coding[i] = coding[i];
            d->set_line = 0;
        }
        d->phase = TB710FU_DWC3_WAIT_STATUS3;
        return 1;
    }
    if (d->phase != TB710FU_DWC3_STATUS_PHASE || bytes != 0 || trb_status != 0)
        return fail(d, "invalid control STATUS completion");
    if (d->pending_configuration_valid) {
        d->configured = d->pending_configuration;
        write_reg(d, DALEPENA, d->configured ? ENABLED_ENDPOINTS : 3u);
    }
    if (d->pending_dtr_valid) {
        d->dtr = d->pending_dtr;
        d->notification_pending = 1;
    }
    return setup_phase(d);
}

static int endpoint_event(struct tb710fu_dwc3 *d, uint32_t event)
{
    const unsigned ep = (event >> 1) & 31u;
    const unsigned type = (event >> 6) & 15u;
    const unsigned status = (event >> 12) & 15u;
    if (ep >= 6 || ep == 4) return fail(d, "event for unowned endpoint");
    if (type == 3) return ep < 2 ? control_not_ready(d, ep, status) : 1;
    if (type == 7) {
        // Command completion. The only asynchronous command posted here is End
        // Transfer with CMDIOC set, and this event is what releases the endpoint:
        // until it arrives the core still owns the transfer the host abandoned.
        if (d->cancel_pending[ep]) {
            d->cancel_pending[ep] = 0;
            d->cancel_until[ep] = 0;
            d->resource[ep] = 0;
            ++d->cancel_completions;
        }
        return 1;
    }
    if (type != 1) return fail(d, "unexpected endpoint event");
    if (d->stale[ep]) {
        // Cancellation completion for the transfer a reset or pipe reset
        // abandoned: its residual describes the dead transfer, not the next one.
        d->stale[ep] = 0;
        ++d->stale_completions;
        return 1;
    }
    if (!d->busy[ep]) {
        // A completion nobody owns is the tail of a transfer this driver already
        // abandoned (the host closed COM5, reset the pipe, or the quarantine
        // expired). It cannot corrupt a live transfer, and failing the transport
        // here is what made the next SerialPort.Open() report "A device attached
        // to the system is not functioning".
        ++d->orphan_completions;
        return 1;
    }
    const unsigned off = trb_offset(ep);
    d->ops.sync_for_cpu(d->ops.context, off, TB710FU_DWC3_TRB_STRIDE);
    const uint8_t *trb = d->dma + off;
    const uint32_t size = get32(trb + 8), control = get32(trb + 12);
    const unsigned residual = size & LENGTH_MASK;
    const unsigned trb_status = size >> 28;
    if ((control & TRB_HWO) || (status & 1u) || residual > d->requested[ep])
        return fail(d, "invalid device DMA completion ownership/status");
    const unsigned bytes = d->requested[ep] - residual;
    d->busy[ep] = 0;
    d->resource[ep] = 0;
    if (ep < 2) return control_complete(d, ep, bytes, trb_status);
    if (trb_status != 0) return fail(d, "non-control TRB failed");
    if (ep == EP_RX) {
        d->ops.sync_for_cpu(d->ops.context, TB710FU_DWC3_RX, d->requested[ep]);
        if (d->ops.on_rx && bytes > 0u) {
            d->ops.on_rx(d->ops.context, d->dma + TB710FU_DWC3_RX, bytes);
        } else {
            d->discarded_input += bytes;
        }
        return 1;
    }
    if (residual != 0) {
        // Bulk IN ended with bytes still owed: the host stopped reading inside
        // this span (COM5 close) and the core terminated it. Nothing was sent, so
        // nothing is retired and the ring keeps the bytes; the transport stays up.
        if (ep == EP_TX) {
            d->tx_state = TX_IDLE;
            d->tx_commit = 0;
            d->tx_end = 0;
            ++d->aborted_tx;
            return 1;
        }
        if (ep == EP_NOTIFY) {
            d->notification_pending = 1;
            return 1;
        }
        return fail(d, "short IN completion; log consumption ambiguous");
    }
    if (ep == EP_TX) {
        if (d->tx_state == TX_ACTIVE) {
            d->tx_commit = bytes;
            d->tx_state = TX_COMMIT;
        } else if (d->tx_state == TX_ZLP_ACTIVE) d->tx_state = TX_IDLE;
        else return fail(d, "unexpected log completion state");
    }
    return 1;
}

static int device_event(struct tb710fu_dwc3 *d, uint32_t event)
{
    if (((event >> 1) & 127u) != 0) return fail(d, "non-device global event");
    switch ((event >> 8) & 15u) {
    case 0:
    case 1: {
        // A USB reset is the host restarting the session, and Windows issues
        // one around a COM5 close/open cycle. Linux v6.1 gadget.c treats it as
        // recoverable: it stops the active transfers, then re-arms EP0 OUT.
        // This core preserves an already armed EP0 SETUP TRB, abandons every
        // other in-flight transfer, and keeps serving EP0, so the host can
        // re-enumerate and COM5 comes back without a reboot.
        ++d->resets;
        if (d->busy[0] && d->phase != TB710FU_DWC3_SETUP_PHASE) abandon_endpoint(d, 0);
        abandon_endpoint(d, 1);
        abandon_endpoint(d, EP_RX);
        abandon_endpoint(d, EP_TX);
        abandon_endpoint(d, EP_NOTIFY);
        // The bus reset itself terminated every transfer, so an End Transfer that
        // was still outstanding has nothing left to release.
        for (unsigned i = 0; i < 6u; ++i) {
            d->cancel_pending[i] = 0;
            d->cancel_until[i] = 0;
        }
        d->phase = TB710FU_DWC3_SETUP_PHASE;
        d->control_polls = d->polls;
        d->data_in = d->control_bytes = d->setup_pending = d->set_line = 0;
        d->pending_address_valid = d->pending_configuration_valid = 0;
        d->pending_dtr_valid = d->pending_dtr = 0;
        d->connected = d->configured = d->dtr = 0;
        d->notification_pending = 0;
        // The in-flight Bulk IN span never completed, so it is not retired: the
        // ring keeps those bytes and the next session sends them again from the
        // start. Repeating one fragment beats dropping it.
        d->tx_state = TX_IDLE;
        d->tx_commit = 0;
        d->tx_end = 0;
        write_reg(d, DCFG, read_reg(d, DCFG) & ~(127u << 3));
        write_reg(d, DALEPENA, 3);
        return 1;
    }
    case 2: {
        if (d->connected) return fail(d, "duplicate CONNECT_DONE");
        const unsigned speed = read_reg(d, DSTS) & 7u;
        if (speed != 0 && speed != 1 && speed != 3) return fail(d, "unsupported USB connection speed");
        d->packet_size = speed == 0 ? 512u : 64u;
        d->connected = 1;
        if (!configure_ep(d, 0, 0, 64, 1) || !configure_ep(d, 1, 0, 64, 1)) return 0;
        // A quarantined EP0 is re-armed by pump once its cancellation window
        // closed; arming it here would credit the old completion to the new TRB.
        return (d->busy[0] || !endpoint_usable(d, 0)) ? 1 : setup_phase(d);
    }
    case 3: case 4: case 6:
        return 1; // Link-state, wake and suspend do not grant DMA ownership.
    case 9: case 11:
        return fail(d, "USB erratic error/event overflow; retain DMA");
    default:
        return fail(d, "unexpected device event");
    }
}

// Retire the span that just left on Bulk IN. The RAM ring is drop-oldest, so the
// producer may have evicted the head of this span while it was on the wire: the
// commit below retires a ring *position*, not a length. A length-based consume
// refused forever once eviction shrank the pending window below the span, and
// that permanent TX_COMMIT is what stopped Bulk IN until the port was reopened.
static void tx_retire(struct tb710fu_dwc3 *d)
{
    if (d->tx_commit == 0u) return;
    d->ops.log_commit(d->ops.context, d->tx_end);
    d->transmitted += d->tx_commit;
    d->tx_state = (d->packet_size != 0u && (d->tx_commit % d->packet_size) == 0u) ?
        TX_ZLP_PENDING : TX_IDLE;
    d->tx_commit = 0;
    d->tx_end = 0;
}

static int pump(struct tb710fu_dwc3 *d)
{
    // The panic shell drives a controller whose worker is gone. A Bulk TRB the
    // worker was halted on can never complete: busy[] stays set, every stage
    // below waits on it, and COM5 answers nothing for the rest of the boot.
    // Retire it after a bounded window and let the normal re-arm below serve
    // the host again. No End Transfer is posted here: a command from panic
    // context is exactly what may block, and the stale window covers the late
    // completion the old TRB may still produce.
    if (d->panic_mode) {
        if (d->busy[EP_RX] && d->polls - d->armed_at[EP_RX] >= BULK_RETIRE_POLLS)
            retire_bulk_owned(d, EP_RX);
        if (d->busy[EP_TX] && d->polls - d->armed_at[EP_TX] >= BULK_RETIRE_POLLS) {
            retire_bulk_owned(d, EP_TX);
            d->tx_state = TX_IDLE;
            d->tx_commit = 0;
            d->tx_end = 0;
        }
    }
    // A control stage the host never finished: Windows can cancel the STATUS
    // stage of its close-time SET_CONTROL_LINE_STATE, and a panic can land
    // between a SETUP and its DATA/STATUS. EP0 is then left without an armed
    // SETUP TRB, the next open is answered by nobody, and Windows reports
    // "A device attached to the system is not functioning". Silence for a whole
    // control window means the stage was abandoned: give it back, stall the pipe
    // and re-arm SETUP (Linux v6.1 ep0.c:221-246 dwc3_ep0_stall_and_restart).
    if (d->phase != TB710FU_DWC3_SETUP_PHASE &&
        d->polls - d->control_polls >= EP0_CONTROL_POLLS) {
        if (d->phase == TB710FU_DWC3_DATA_PHASE) cancel_endpoint(d, d->data_in);
        abandon_endpoint(d, 0);
        abandon_endpoint(d, 1);
        ++d->ep0_recoveries;
        if (!d->cancel_pending[0]) {
            if (!stall_setup(d)) return 0;
        } else {
            // The core is still terminating EP0's transfer; its command-complete
            // event releases the endpoint and the re-arm below picks it up.
            d->phase = TB710FU_DWC3_SETUP_PHASE;
            d->data_in = d->control_bytes = d->setup_pending = d->set_line = 0;
        }
        d->control_polls = d->polls;
    }
    // EP0 must never be left without an armed SETUP TRB. A host SETUP that
    // finds none is answered by nobody, and Windows reports that as
    // "A device attached to the system is not functioning" on the next open.
    // This also re-arms EP0 after a reset abandoned a DATA/STATUS phase.
    if (!d->busy[0] && d->phase == TB710FU_DWC3_SETUP_PHASE && endpoint_usable(d, 0)) {
        ++d->ep0_rearms;
        if (!setup_phase(d)) return 0;
    }
    if (!d->configured) return 1;
    if (!d->busy[EP_RX] && endpoint_usable(d, EP_RX) &&
        !transfer(d, EP_RX, TRB_NORMAL, TB710FU_DWC3_RX, d->packet_size)) return 0;
    if (d->notification_pending && !d->busy[EP_NOTIFY] && endpoint_usable(d, EP_NOTIFY)) {
        uint8_t *p = d->dma + TB710FU_DWC3_NOTIFY;
        const uint8_t notification[10] = {0xa1, 0x20, 0, 0, 0, 0, 2, 0, 0, 0};
        for (unsigned i = 0; i < 10; ++i) p[i] = notification[i];
        p[8] = d->dtr ? 3u : 0u; // RX/TX carrier state for this log channel.
        (void)transfer(d, EP_NOTIFY, TRB_NORMAL, TB710FU_DWC3_NOTIFY, 10);
        // Only a state the core accepted is reported; a deferred start retries.
        if (d->busy[EP_NOTIFY]) d->notification_pending = 0;
    }
    // A successful USB transfer is retired once, from the position it started
    // at. Retiring can never fail, so a stalled consume can no longer hold Bulk
    // IN at TX_COMMIT while the ring still has bytes.
    if (d->tx_state == TX_COMMIT) tx_retire(d);
    if (!d->dtr) return 1;
    if (d->busy[EP_TX]) {
        // A bare ZLP is only a framing terminator: it carries no data of its
        // own. When the host's read URB was satisfied by the exact multiple it
        // asked for, no IN token arrives for it, and every later byte waits
        // behind it forever - the COM5 session that stopped after ~15 KiB and
        // only recovered when the port was reopened. Retire it after a bounded
        // window so the data path always makes progress; the next data transfer
        // supplies the host whatever short packet it still needs.
        if (d->tx_state == TX_ZLP_ACTIVE && d->polls - d->zlp_armed >= ZLP_POLLS) {
            abandon_endpoint(d, EP_TX);
            d->tx_state = TX_IDLE;
        }
        return 1;
    }
    if (!endpoint_usable(d, EP_TX)) return 1;
    if (d->tx_state == TX_ZLP_PENDING) {
        d->zlp_armed = d->polls;
        (void)transfer(d, EP_TX, TRB_NORMAL, TB710FU_DWC3_TX, 0);
        // A span is only on the wire when the core accepted it. A deferred start
        // leaves the state idle so the next poll retries the same bytes.
        d->tx_state = d->busy[EP_TX] ? TX_ZLP_ACTIVE : TX_IDLE;
        return 1;
    }
    if (d->tx_state != TX_IDLE) return 1;
    d->tx_end = 0;
    const size_t bytes = d->ops.log_read(d->ops.context, d->dma + TB710FU_DWC3_TX,
                                        TB710FU_DWC3_TX_BYTES, &d->tx_end);
    if (bytes > TB710FU_DWC3_TX_BYTES) return fail(d, "log producer returned oversized copy");
    if (!bytes) return 1;
    (void)transfer(d, EP_TX, TRB_NORMAL, TB710FU_DWC3_TX, (unsigned)bytes);
    d->tx_state = d->busy[EP_TX] ? TX_ACTIVE : TX_IDLE;
    return 1;
}

void tb710fu_dwc3_poll(struct tb710fu_dwc3 *d)
{
    if (d == NULL || !d->running || d->failed) return;
    ++d->polls;
    const uint32_t count = read_reg(d, EVENT_COUNT(0));
    if ((count & 3u) || (count & EVENT_COUNT_MASK) > TB710FU_DWC3_EVENT_BYTES) {
        (void)fail(d, "corrupt event ring count; retain DMA");
        return;
    }
    unsigned bytes = count & EVENT_COUNT_MASK;
    unsigned budget = EVENT_BUDGET;
    while (bytes >= 4 && budget-- != 0) {
        d->ops.sync_for_cpu(d->ops.context, d->event_position, 4);
        const uint32_t event = get32(d->dma + d->event_position);
        d->last_event = event;
        d->event_position = (d->event_position + 4u) % TB710FU_DWC3_EVENT_BYTES;
        ++d->events;
        // ACK the consumed event without rewinding the producer or touching any
        // other event. Failures below never reuse a DMA buffer/event position.
        write_reg(d, EVENT_COUNT(0), 4);
        if (!(event & 1u) ? !endpoint_event(d, event) : !device_event(d, event)) return;
        bytes -= 4;
    }
    if (!d->failed) (void)pump(d);
}

void tb710fu_dwc3_enable_event_irq(struct tb710fu_dwc3 *d)
{
    write_reg(d, EVENT_SIZE(0), TB710FU_DWC3_EVENT_BYTES);
}

// Linux reaches this point on a core that its own power-on sequence just
// brought out of reset. This core is adopted from firmware while running,
// which is the one state Linux never sees: the request can be discarded and
// the bit stay set forever (the previous package proved exactly that, with
// GCC still reporting power and clocks). So this asserts the request, retries
// it once, and then retracts the bit by hand: retracting is reported as its
// own stage, never as a completed reset, and the caller decides whether the
// handed-over state is usable. Every step paints the lane tag first, because
// a register access that never returns leaves no other evidence behind.
static void reset_lane(struct tb710fu_dwc3 *d, const char *tag)
{
    if (d->ops.lane != NULL) d->ops.lane(d->ops.context, tag);
}

static int core_reset(struct tb710fu_dwc3 *d, uint32_t pre)
{
    reset_lane(d, "dwcreg");
    // Synopsys DWC3 / Linux dwc3_gadget_soft_reset: PHY suspend/SLPM bits must
    // be clear BEFORE asserting DCTL.CSFTRST, or the reset FSM can stall
    // waiting on a gated PHY clock (observed on TB710FU with pcfg=00102500).
    const uint32_t pcfg_pre = read_reg(d, USB2PHYCFG);
    if ((pcfg_pre & PHY_SUSPEND) != 0)
        write_reg(d, USB2PHYCFG, pcfg_pre & ~PHY_SUSPEND);
    const uint32_t pectl_pre = read_reg(d, GUSB3PIPECTL);
    if ((pectl_pre & PIPE_SUSPEND) != 0)
        write_reg(d, GUSB3PIPECTL, pectl_pre & ~PIPE_SUSPEND);
    if (((pcfg_pre & PHY_SUSPEND) != 0) || ((pectl_pre & PIPE_SUSPEND) != 0))
        d->ops.delay_us(d->ops.context, WAIT_DELAY_US);
    d->reset_trace[2] = read_reg(d, GCTL);
    d->reset_trace[3] = read_reg(d, USB2PHYCFG);
    d->reset_trace[5] = read_reg(d, DSTS);

    reset_lane(d, "dwcset");
    write_reg(d, DCTL, (pre & ~RUN_STOP) | SOFT_RESET);
    reset_lane(d, "dwcrd");
    d->reset_trace[1] = read_reg(d, DCTL);
    reset_lane(d, "dwcwt");
    if (wait_reg(d, DCTL, SOFT_RESET, 0)) return 1;

    reset_lane(d, "dwcw1");
    write_reg(d, DCTL, read_reg(d, DCTL) | SOFT_RESET);
    if (wait_reg(d, DCTL, SOFT_RESET, 0)) return 2;

    reset_lane(d, "dwcw2");
    write_reg(d, DCTL, read_reg(d, DCTL) & ~SOFT_RESET);
    if (!(read_reg(d, DCTL) & SOFT_RESET)) return 3;
    return 0;
}

int tb710fu_dwc3_start(struct tb710fu_dwc3 *d, const struct tb710fu_dwc3_ops *ops,
                       uint8_t *dma, uint64_t dma_physical,
                       uint16_t vendor_id, uint16_t product_id, const char *serial)
{
    if (d == NULL || d->dma != NULL || d->failed || d->running) return 0;
    if (ops == NULL || ops->read32 == NULL || ops->write32 == NULL ||
        ops->delay_us == NULL || ops->sync_for_cpu == NULL || ops->sync_for_device == NULL ||
        ops->log_read == NULL || ops->log_commit == NULL || dma == NULL ||
        ((uintptr_t)dma & (TB710FU_DWC3_DMA_SIZE - 1u)) ||
        (dma_physical & (TB710FU_DWC3_DMA_SIZE - 1u)) ||
        dma_physical > UINT64_MAX - TB710FU_DWC3_DMA_SIZE ||
        vendor_id == 0 || product_id == 0 || serial == NULL) return 0;
    size_t chars = 0;
    while (chars < 31 && serial[chars] != 0) {
        if ((unsigned char)serial[chars] < 32 || (unsigned char)serial[chars] > 126) return 0;
        ++chars;
    }
    if (chars == 0 || chars > 30) return 0;
    zero_bytes(d, sizeof(*d));
    d->ops = *ops;
    d->dma = dma;
    d->dma_physical = dma_physical;
    d->vendor_id = vendor_id;
    d->product_id = product_id;
    for (size_t i = 0; i <= chars; ++i) d->serial_copy[i] = serial[i];
    d->serial = d->serial_copy;
    d->packet_size = 512;
    d->coding[1] = 0xc2; d->coding[2] = 1; d->coding[6] = 8; // 115200 8N1.
    const uint32_t id = read_reg(d, GSNPSID) & 0xffff0000u;
    if (id != 0x55330000u && id != 0x33310000u) {
        d->failure = "unrecognized DWC3 core ID";
        return 0;
    }
    if (((read_reg(d, GCTL) >> 12) & 3u) != 2u) {
        d->failure = "firmware controller is not in device role";
        return 0;
    }
    const uint32_t hw = read_reg(d, GHWPARAMS3);
    const unsigned total_eps = (hw >> 12) & 63u, in_eps = (hw >> 18) & 31u;
    const unsigned interrupts = (read_reg(d, GHWPARAMS1) >> 15) & 63u;
    if (total_eps < 6 || in_eps < 3 || in_eps > total_eps || total_eps - in_eps < 2 ||
        interrupts == 0 || interrupts > 32) {
        d->failure = "insufficient DWC3 resources";
        return 0;
    }
    // Do not replace firmware DMA addresses before proving the core halted and
    // its device reset completed. Data-plane cancellation issued later uses
    // ENDTRANSFER (see cancel_endpoint); this adoption path itself never assumes
    // that a posted cancel completed.
    // The SS PIPE control is read but never written: it is the register that says
    // whether the PHY handed the core a PIPE clock at all, which is the one
    // reason a live controller would answer commands but never finish them.
    // Each access below names itself before it runs: this sequence is the one
    // place a frame died with no line at all, and only a lane tag survives an
    // access that never returns.
    reset_lane(d, "pctlrd");
    d->probe_trace[5] = read_reg(d, GUSB3PIPECTL);
    reset_lane(d, "dctlrd");
    const uint32_t pre = read_reg(d, DCTL);
    d->reset_trace[0] = pre;
    reset_lane(d, "haltwr");
    write_reg(d, DCTL, pre & ~RUN_STOP);
    // A core that firmware already left stopped has nothing to halt; only a
    // running one owes the DEVCTLHLT handshake. Everything the handshake would
    // acknowledge is rewritten below anyway.
    if ((pre & RUN_STOP) && !halt_firmware(d, interrupts))
        return fail(d, "firmware controller did not halt");
    d->reset_stage = (uint8_t)core_reset(d, pre);
    d->reset_trace[4] = read_reg(d, GSTS);
    if (d->reset_stage == 0) {
        // The request was never answered and the retraction did not clear the
        // bit either, so the status still claims the core is in reset. Stopping
        // on that bit is what has kept this board on this one line for every
        // boot: it never learns whether the core accepts configuration in that
        // state. Drive the rest of the path anyway. A command that completes is
        // proof the core is clocked and answering, which is stronger evidence
        // than the status bit, and a command that times out already fails with
        // its own reason. Every step below paints its lane tag first, so an
        // access that never returns names itself on the bottom band instead of
        // taking the whole report with it.
        d->reset_forced = 1;
        reset_lane(d, "dwcfail");
    }
    if (d->reset_stage == 3) {
        // The bit cleared because software retracted it, so the core is out of
        // reset but was never reset by hardware. Firmware had left it running
        // in device role with charger detection done, and everything this path
        // relies on (halt, event ring, endpoint configuration) is rewritten
        // below anyway, so adopt that state and let enumeration judge it.
        // Recording the stage keeps the difference visible in the report.
        d->reset_adopted = 1;
        reset_lane(d, "dwcadopt");
    }
    write_reg(d, DEVTEN, 0);
    write_reg(d, DALEPENA, 0);
    for (unsigned i = 0; i < interrupts; ++i) {
        write_reg(d, EVENT_SIZE(i), EVENT_INTERRUPT_MASK);
        write_reg(d, EVENT_COUNT(i), read_reg(d, EVENT_COUNT(i)) & EVENT_COUNT_MASK);
        write_reg(d, EVENT_LO(i), 0);
        write_reg(d, EVENT_HI(i), 0);
    }
    zero_bytes(dma, TB710FU_DWC3_DMA_SIZE);
    d->ops.sync_for_device(d->ops.context, 0, TB710FU_DWC3_DMA_SIZE);
    // Role and phycfg are written only when they actually differ: this board
    // hands over a core already in device role with the USB2 suspend bit clear,
    // and a write that changes nothing is one more bus access that can wedge -
    // which is exactly what the last precondition attempt did and cost a boot.
    const uint32_t gctl_now = read_reg(d, GCTL);
    // twrp.dts declares snps,disable-clk-gating for dwc3@a600000.
    const uint32_t gctl_want =
        ((gctl_now & ~((3u << 12) | (1u << 1))) | (2u << 12) | GCTL_DISABLE_CLK_GATING);
    if (gctl_want != gctl_now) write_reg(d, GCTL, gctl_want);
    // Linux drivers/usb/dwc3/core.c: in USB 2.0 peripheral mode without a
    // SuperSpeed PIPE clock, route the internal 2.0 clock for the 3.0 domain.
    const uint32_t guctl1_now = read_reg(d, GUCTL1);
    if (!(guctl1_now & GUCTL1_FORCE_20_CLK_FOR_30_CLK))
        write_reg(d, GUCTL1, guctl1_now | GUCTL1_FORCE_20_CLK_FOR_30_CLK);
    // sm8650.dtsi declares both USB2 and USB3 suspend quirks for the primary core.
    const uint32_t pcfg_now = read_reg(d, USB2PHYCFG);
    if ((pcfg_now & PHY_SUSPEND) != 0) write_reg(d, USB2PHYCFG, pcfg_now & ~PHY_SUSPEND);
    const uint32_t pectl_now = read_reg(d, GUSB3PIPECTL);
    if ((pectl_now & PIPE_SUSPEND) != 0) write_reg(d, GUSB3PIPECTL, pectl_now & ~PIPE_SUSPEND);
    reset_lane(d, "dcfgrw");
    write_reg(d, DCFG, read_reg(d, DCFG) & ~((127u << 3) | (31u << 12) | 7u | (1u << 22)));
    d->probe_trace[0] = read_reg(d, DCFG);
    reset_lane(d, "evtrw");
    write_reg(d, EVENT_LO(0), (uint32_t)dma_physical);
    write_reg(d, EVENT_HI(0), (uint32_t)(dma_physical >> 32));
    write_reg(d, EVENT_SIZE(0), TB710FU_DWC3_EVENT_BYTES | EVENT_INTERRUPT_MASK);
    write_reg(d, EVENT_COUNT(0), 0);
    d->probe_trace[2] = read_reg(d, EVENT_LO(0));
    d->probe_trace[4] = read_reg(d, EVENT_HI(0));
    reset_lane(d, "starcfg");
    if (!command(d, 0, COMMAND_START_CONFIGURATION, 0, 0, 0)) {
        d->probe_trace[3] = read_reg(d, DSTS);
        return 0;
    }
    for (unsigned ep = 0; ep < 6; ++ep) {
        if (ep == 4) continue;
        const unsigned type = ep < 2 ? 0u : ep == EP_NOTIFY ? 3u : 2u;
        const unsigned mps = ep < 2 ? 64u : ep == EP_NOTIFY ? 16u : 512u;
        if (!configure_ep(d, ep, type, mps, 0)) return 0;
    }
    write_reg(d, DALEPENA, 3);
    // Linux v6.1 gadget.c __dwc3_gadget_start posts EP0 SETUP before
    // dwc3_gadget_run_stop exposes the pull-up. A host can issue SETUP
    // before the worker has drained CONNECT_DONE.
    if (!setup_phase(d)) return 0;
    write_reg(d, DEVTEN, (1u << 11) | (1u << 9) | 7u);
    d->probe_trace[1] = read_reg(d, DEVTEN);
    reset_lane(d, "dwcrun");
    // sm8650.dtsi and twrp.dts declare snps,usb2-gadget-lpm-disable and
    // snps,hird-threshold = <0>; clear HIRD/LPM bits (28:19) and CSFTRST when
    // asserting RUN_STOP.
    write_reg(d, DCTL, (read_reg(d, DCTL) & ~(SOFT_RESET | DCTL_LPM_MASK)) | RUN_STOP);
    d->probe_trace[3] = read_reg(d, DSTS);
    d->running = 1;
    // The controller is fully adopted only here: every earlier failure path
    // leaves it configured halfway, and the panic shell must not touch that.
    d->adopted = 1;
    return 1;
}

// Panic context only. The worker that normally drives this controller is gone,
// so nothing here may block on the core: no command is posted and no DMA address
// is rewritten. What a panic needs is the two registers start()/stop() own -
// device events and the pull-up - plus a SETUP TRB that the host's next
// SET_CONTROL_LINE_STATE can land on. Without this, a panic that arrives while a
// transport failure has already cleared DEVTEN/RUN_STOP leaves the shell deaf,
// and a COM5 opened after the panic never sees the log it was opened for.
void tb710fu_dwc3_panic_rearm(struct tb710fu_dwc3 *d)
{
    if (d == NULL || d->dma == NULL || d->ops.read32 == NULL || d->ops.write32 == NULL)
        return;
    /* Only a core that start() actually completed is safe to touch here. The
     * previous gate read probe_trace[1], the DEVTEN readback, but RUN_STOP is
     * raised after that readback and a transport failure clears it; a boot
     * whose DEVTEN write never stuck therefore skipped the panic rearm on
     * exactly the controller that needed it. adopted is a fact, not evidence. */
    if (!d->adopted) return;
    /* From here on nothing else drives this controller, so every endpoint is
     * reclaimed unconditionally. A CPU halted inside transfer() can leave busy[]
     * set for a TRB the core never received; keeping an armed EP0 SETUP or Bulk
     * transfer "to be safe" then parks the shell forever. A TRB the core did
     * receive still completes, and that completion arrives for an endpoint
     * nobody owns and is dropped as an orphan, after which EP0 is re-armed
     * below. Marking panic_mode makes pump() bound any Bulk ownership that is
     * lost later. */
    for (unsigned ep = 0; ep < 6u; ++ep) {
        if (ep == 4u) continue;
        if (d->busy[ep]) ++d->abandoned_transfers;
        d->busy[ep] = 0;
        d->requested[ep] = 0;
        d->resource[ep] = 0;
        d->armed_at[ep] = 0;
        d->stale[ep] = 0;
        d->cancel_pending[ep] = 0;
        d->cancel_until[ep] = 0;
    }
    d->panic_mode = 1;
    d->tx_state = TX_IDLE;
    d->tx_commit = 0;
    d->tx_end = 0;
    d->zlp_armed = 0;
    d->phase = TB710FU_DWC3_SETUP_PHASE;
    d->data_in = d->control_bytes = d->setup_pending = d->set_line = 0;
    d->pending_address_valid = d->pending_configuration_valid = 0;
    d->pending_dtr_valid = d->pending_dtr = 0;
    d->failed = 0;
    d->failure = NULL;
    d->running = 1;
    d->control_polls = d->polls;
    write_reg(d, DALEPENA, d->configured ? ENABLED_ENDPOINTS : 3u);
    write_reg(d, DEVTEN, (1u << 11) | (1u << 9) | 7u);
    // SETUP is posted before the pull-up is exposed so a host that arrives with
    // the shell does not time out on its first request. A core that still owns
    // the abandoned SETUP refuses this start; transfer() defers it and the next
    // poll retries, while the core's own TRB still answers the host's SETUP.
    if (!d->busy[0] && d->phase == TB710FU_DWC3_SETUP_PHASE) (void)setup_phase(d);
    write_reg(d, DCTL, (read_reg(d, DCTL) & ~(SOFT_RESET | DCTL_LPM_MASK)) | RUN_STOP);
}
