// SPDX-License-Identifier: BSD-2-Clause-Patent
// Drives the production controller core through MMIO/event/DMA/cache callbacks.
// Hardware keeps an independent DMA view: missing clean/invalidate operations
// cannot accidentally pass by reading the CPU's original buffer.
#include "../CdcDwc3.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define REG_WORDS (0xd000u / 4u)
#define PHYSICAL UINT64_C(0x123400000)

struct fixture {
    struct tb710fu_dwc3 core;
    uint32_t regs[REG_WORDS];
    _Alignas(TB710FU_DWC3_DMA_SIZE) uint8_t cpu[TB710FU_DWC3_DMA_SIZE];
    uint8_t device[TB710FU_DWC3_DMA_SIZE];
    uint8_t log[4096];
    size_t log_length, log_position;
    unsigned copy_calls, consume_calls, consume_block;
    unsigned sync_cpu, sync_device, address_writes, waits, stall_commands;
    unsigned starts[6], active[6], buffer[6], length[6], kind[6];
    unsigned producer, halted, reset_complete;
    unsigned block_halt, block_reset, block_command, bad_command_status;
    unsigned stick_reset;
    unsigned halt_needs_event_ack;
};

static uint32_t load32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8));
}

static unsigned trb(unsigned ep)
{
    return TB710FU_DWC3_TRB_BASE + ep * TB710FU_DWC3_TRB_STRIDE;
}

static uint32_t read32(void *context, unsigned reg)
{
    struct fixture *f = context;
    assert(reg < REG_WORDS * 4 && (reg & 3u) == 0);
    if (reg == 0xc70c && f->halted) return f->regs[reg / 4] | (1u << 22);
    return f->regs[reg / 4];
}

static void write32(void *context, unsigned reg, uint32_t value)
{
    struct fixture *f = context;
    assert(reg < REG_WORDS * 4 && (reg & 3u) == 0);
    if (reg == 0xc704) {
        if (!(value & (1u << 31)) && !f->block_halt &&
            !(f->halt_needs_event_ack && f->regs[0xc40c / 4])) f->halted = 1;
        if (value & (1u << 31)) {
            assert(f->active[0] && f->kind[0] == 2);
            f->halted = 0;
        }
        if ((value & (1u << 30)) && !f->block_reset) {
            assert(f->halted);
            assert((f->regs[0xc200 / 4] & ((1u << 6) | (1u << 8))) == 0);
            assert((f->regs[0xc2c0 / 4] & (1u << 17)) == 0);
            f->reset_complete = 1;
            value &= ~(1u << 30);
        }
        // A core that never answers the request at all keeps the bit set even
        // after software retracts it.
        if (f->stick_reset) value |= 1u << 30;
    }
    if (reg >= 0xc400 && reg < 0xc600 && ((reg - 0xc400) & 15u) < 8) {
        assert(f->halted && (f->reset_complete || f->core.reset_adopted ||
                             f->core.reset_forced));
        ++f->address_writes;
    }
    if (reg >= 0xc40c && reg < 0xc600 && ((reg - 0xc40c) & 15u) == 0) {
        assert(value <= f->regs[reg / 4]);
        f->regs[reg / 4] -= value;
        if (f->halt_needs_event_ack && !f->regs[reg / 4] &&
            !(f->regs[0xc704 / 4] & (1u << 31)) && !f->block_halt)
            f->halted = 1;
        return;
    }
    f->regs[reg / 4] = value;
    if (reg >= 0xc80c && reg < 0xc86c && ((reg - 0xc80c) & 15u) == 0) {
        const unsigned ep = (reg - 0xc80c) / 16;
        const unsigned command = value & 15u;
        assert((f->regs[0xc200 / 4] & ((1u << 6) | (1u << 8))) == 0);
        if (f->block_command) return;
        f->regs[reg / 4] = command | (f->bad_command_status << 12);
        if (f->bad_command_status) return;
        if (command == 4) ++f->stall_commands;
        if (command == 6) {
            const uint64_t descriptor = (uint64_t)f->regs[(reg - 4) / 4] << 32 |
                                         f->regs[(reg - 8) / 4];
            assert(descriptor == PHYSICAL + trb(ep));
            assert(!f->active[ep]);
            const uint8_t *t = f->device + trb(ep);
            const uint64_t buffer = (uint64_t)load32(t + 4) << 32 | load32(t);
            assert(buffer >= PHYSICAL && buffer < PHYSICAL + TB710FU_DWC3_DMA_SIZE);
            assert(load32(t + 12) & 1u);
            f->buffer[ep] = (unsigned)(buffer - PHYSICAL);
            f->length[ep] = load32(t + 8) & 0xffffffu;
            f->kind[ep] = (load32(t + 12) >> 4) & 63u;
            assert(f->length[ep] <= TB710FU_DWC3_DMA_SIZE - f->buffer[ep]);
            f->active[ep] = 1;
            ++f->starts[ep];
            // EP0 OUT can legitimately return resource index zero. It must
            // still remain busy until the real completion relinquishes DMA.
            if (ep != 0) f->regs[reg / 4] |= 1u << 16;
        }
    }
}

static void delay(void *context, unsigned us)
{
    struct fixture *f = context;
    // The poll step is the only delay the core asks for; it is long enough to be
    // a real window (100 us) rather than a single microsecond.
    assert(us >= 1 && us <= 100);
    ++f->waits;
}

static const char *lane_tag;

static void lane(void *context, const char *tag)
{
    (void)context;
    lane_tag = tag;
}

static void sync_cpu(void *context, unsigned off, unsigned size)
{
    struct fixture *f = context;
    assert(off <= sizeof(f->cpu) && size <= sizeof(f->cpu) - off);
    memcpy(f->cpu + off, f->device + off, size);
    ++f->sync_cpu;
}

static void sync_device(void *context, unsigned off, unsigned size)
{
    struct fixture *f = context;
    assert(off <= sizeof(f->cpu) && size <= sizeof(f->cpu) - off);
    memcpy(f->device + off, f->cpu + off, size);
    ++f->sync_device;
}

static size_t log_copy(void *context, uint8_t *out, size_t capacity)
{
    struct fixture *f = context;
    ++f->copy_calls;
    size_t n = f->log_length - f->log_position;
    if (n > capacity) n = capacity;
    memcpy(out, f->log + f->log_position, n);
    return n;
}

static int log_consume(void *context, size_t count)
{
    struct fixture *f = context;
    ++f->consume_calls;
    if (f->consume_block) {
        --f->consume_block;
        return 0;
    }
    assert(count <= f->log_length - f->log_position);
    f->log_position += count;
    return 1;
}

static struct tb710fu_dwc3_ops operations(struct fixture *f)
{
    const struct tb710fu_dwc3_ops ops = {
        f, read32, write32, delay, sync_cpu, sync_device, log_copy, log_consume,
        lane
    };
    return ops;
}

static void prepare(struct fixture *f)
{
    memset(f, 0, sizeof(*f));
    memset(f->cpu, 0xa5, sizeof(f->cpu));
    f->regs[0xc120 / 4] = 0x3331300au; // DWC_usb31, 64-bit DMA addresses.
    f->regs[0xc110 / 4] = 2u << 12; // Firmware device role.
    f->regs[0xc144 / 4] = 1u << 15;
    f->regs[0xc14c / 4] = (6u << 12) | (3u << 18);
    f->regs[0xc200 / 4] = (1u << 6) | (1u << 8);
    f->regs[0xc2c0 / 4] = 0x030e1002u; // Includes GUSB3PIPECTL.SUSPHY bit 17.
    f->regs[0xc704 / 4] = 1u << 31;
}

static int start(struct fixture *f, const char *serial)
{
    struct tb710fu_dwc3_ops ops = operations(f);
    return tb710fu_dwc3_start(&f->core, &ops, f->cpu, PHYSICAL, 0x1209, 1, serial);
}

static void test_device_role_and_event_irq(void)
{
    struct fixture f;
    for (unsigned role = 1; role <= 3; role += 2) {
        prepare(&f);
        f.regs[0xc110 / 4] = role << 12;
        assert(!start(&f, "TB710FUCDC001"));
        assert(!f.halted && !f.reset_complete && !f.address_writes);
        assert(f.regs[0xc704 / 4] == (1u << 31));
    }

    prepare(&f);
    assert(start(&f, "TB710FUCDC001"));
    assert(!(f.regs[0xc200 / 4] & ((1u << 6) | (1u << 8))));
    assert(f.regs[0xc408 / 4] == ((1u << 31) | TB710FU_DWC3_EVENT_BYTES));
    tb710fu_dwc3_enable_event_irq(&f.core);
    assert(f.regs[0xc408 / 4] == TB710FU_DWC3_EVENT_BYTES);
}

static void event(struct fixture *f, uint32_t value)
{
    assert(f->regs[0xc40c / 4] <= TB710FU_DWC3_EVENT_BYTES - 4);
    store32(f->device + f->producer, value);
    f->producer = (f->producer + 4) % TB710FU_DWC3_EVENT_BYTES;
    f->regs[0xc40c / 4] += 4;
}

static void device_event(struct fixture *f, unsigned type)
{
    event(f, 1u | (type << 8));
}

static void complete(struct fixture *f, unsigned ep, unsigned actual, unsigned status)
{
    assert(f->active[ep] && actual <= f->length[ep]);
    uint8_t *t = f->device + trb(ep);
    store32(t + 8, (f->length[ep] - actual) | (status << 28));
    store32(t + 12, load32(t + 12) & ~1u);
    f->active[ep] = 0;
    event(f, (ep << 1) | (1u << 6));
}

static void connect(struct fixture *f, unsigned speed)
{
    f->regs[0xc70c / 4] = speed;
    device_event(f, 1);
    device_event(f, 2);
    tb710fu_dwc3_poll(&f->core);
    assert(!f->core.failed && f->active[0] && f->kind[0] == 2);
}

static void setup(struct fixture *f, uint8_t type, uint8_t request,
                  uint16_t value, uint16_t index, uint16_t length)
{
    assert(f->active[0] && f->kind[0] == 2);
    const uint8_t bytes[8] = {type, request, (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)length, (uint8_t)(length >> 8)};
    memcpy(f->device + f->buffer[0], bytes, sizeof(bytes));
    complete(f, 0, 8, 0);
    tb710fu_dwc3_poll(&f->core);
    assert(!f->core.failed);
}

static void status(struct fixture *f, unsigned ep)
{
    event(f, (ep << 1) | (3u << 6) | (2u << 12));
    tb710fu_dwc3_poll(&f->core);
    assert(!f->core.failed && f->active[ep] && f->length[ep] == 0);
    assert(f->kind[ep] == 3 || f->kind[ep] == 4);
    complete(f, ep, 0, 0);
    tb710fu_dwc3_poll(&f->core);
    assert(!f->core.failed && f->active[0] && f->kind[0] == 2);
}

static void read_request(struct fixture *f, unsigned value, unsigned index, unsigned requested, unsigned actual)
{
    const unsigned before = f->starts[1];
    setup(f, 0x80, 6, (uint16_t)value, (uint16_t)index, (uint16_t)requested);
    // No XferNotReady(DATA) is supplied: production must start DATA now.
    assert(f->active[1] && f->kind[1] == 5 && f->length[1] == actual);
    assert(f->starts[1] == before + 1);
    complete(f, 1, actual, 0);
    tb710fu_dwc3_poll(&f->core);
    assert(!f->core.failed);
    status(f, 0);
}

static void configure(struct fixture *f)
{
    setup(f, 0, 5, 7, 0, 0);
    assert(((f->regs[0xc700 / 4] >> 3) & 127u) == 7);
    assert(f->core.pending_address_valid);
    status(f, 1);
    setup(f, 0, 9, 1, 0, 0);
    assert(!f->core.configured && !f->active[2]);
    status(f, 1);
    assert(f->core.configured && f->active[2] && f->length[2] == f->core.packet_size);
}

static void test_enumeration_and_normal_host_reset(void)
{
    struct fixture f;
    prepare(&f);
    assert(start(&f, "diagnostic-test-1"));
    assert(f.active[0] && f.kind[0] == 2 && f.address_writes >= 4);
    assert(!(f.regs[0xc200 / 4] & ((1u << 6) | (1u << 8))));
    const unsigned initial_setup_starts = f.starts[0];
    connect(&f, 0);
    assert(f.starts[0] == initial_setup_starts);
    read_request(&f, 0x0100, 0, 8, 8);
    const unsigned setup_starts = f.starts[0];
    // Windows commonly resets after reading the first eight device bytes.
    device_event(&f, 1);
    device_event(&f, 2);
    tb710fu_dwc3_poll(&f.core);
    assert(!f.core.failed && f.starts[0] == setup_starts);
    read_request(&f, 0x0100, 0, 18, 18);
    read_request(&f, 0x0200, 0, 9, 9);
    read_request(&f, 0x0200, 0, 255, 67);
    assert(f.device[TB710FU_DWC3_CONTROL + 57] == 0 && f.device[TB710FU_DWC3_CONTROL + 58] == 2);
    read_request(&f, 0x0600, 0, 10, 10);
    read_request(&f, 0x0700, 0, 67, 67);
    assert(f.device[TB710FU_DWC3_CONTROL + 57] == 64 && f.device[TB710FU_DWC3_CONTROL + 58] == 0);
    configure(&f);
    assert(f.core.packet_size == 512 && !f.active[3]);
    assert(f.sync_cpu && f.sync_device && !f.core.failed);
}

static void test_full_speed_and_bounded_control_strings(void)
{
    struct fixture f;
    prepare(&f);
    assert(!start(&f, "1234567890123456789012345678901"));
    assert(f.address_writes == 0);
    assert(start(&f, "123456789012345678901234567890"));
    connect(&f, 1);
    read_request(&f, 0x0200, 0, 255, 67);
    assert(f.device[TB710FU_DWC3_CONTROL + 57] == 64);
    setup(&f, 0x80, 6, 0x0303, 0x0409, 255);
    assert(f.active[1] && f.length[1] == 62);
    complete(&f, 1, 62, 0);
    tb710fu_dwc3_poll(&f.core);
    status(&f, 0);
    configure(&f);
    assert(f.length[2] == 64);
    const unsigned stalls = f.stall_commands;
    setup(&f, 0x80, 6, 0x0100, 0, 0);
    assert(!f.core.failed && f.stall_commands == stalls + 1 && f.active[0]);
}

static void test_firmware_pending_events_before_halt(void)
{
    struct fixture f;
    prepare(&f);
    f.halt_needs_event_ack = 1;
    f.regs[0xc40c / 4] = 16;
    assert(start(&f, "diagnostic-test-1"));
    assert(f.reset_complete && f.regs[0xc40c / 4] == 0);
    assert(!f.core.failed && f.address_writes >= 4);
}

static void test_line_coding_and_setup_abort(void)
{
    struct fixture f;
    prepare(&f);
    assert(start(&f, "diagnostic-test-1"));
    connect(&f, 0);
    configure(&f);
    setup(&f, 0x21, 0x20, 0, 0, 7);
    assert(f.active[0] && f.kind[0] == 5 && f.length[0] == 64);
    const uint8_t coding[7] = {0x80, 0x25, 0, 0, 0, 0, 8};
    memcpy(f.device + f.buffer[0], coding, sizeof(coding));
    complete(&f, 0, 7, 0);
    tb710fu_dwc3_poll(&f.core);
    assert(memcmp(f.core.coding, coding, 7) == 0);
    status(&f, 1);
    const unsigned stalled = f.stall_commands;
    setup(&f, 0x21, 0x20, 0, 0, 7);
    complete(&f, 0, 6, 0); // Actual OUT data, not just wLength, must be seven.
    tb710fu_dwc3_poll(&f.core);
    assert(!f.core.failed && f.stall_commands == stalled + 1);
    assert(f.active[0] && f.kind[0] == 2 && memcmp(f.core.coding, coding, 7) == 0);
    setup(&f, 0x80, 6, 0x0100, 0, 18);
    complete(&f, 1, 0, 2); // Hardware reports a new SETUP during DATA.
    tb710fu_dwc3_poll(&f.core);
    assert(f.core.setup_pending && !f.active[0]);
    event(&f, (3u << 6) | (2u << 12));
    tb710fu_dwc3_poll(&f.core);
    assert(!f.core.failed && f.active[0] && f.kind[0] == 2);
    assert(f.stall_commands == stalled + 2);
    setup(&f, 0xc0, 0x55, 0, 0, 0);
    assert(f.stall_commands == stalled + 3 && !f.core.failed);
}

static void test_real_dma_log_delivery_and_commit_contention(void)
{
    struct fixture f;
    prepare(&f);
    assert(start(&f, "diagnostic-test-1"));
    connect(&f, 0);
    configure(&f);
    for (unsigned i = 0; i < 2048; ++i) f.log[i] = (uint8_t)i;
    f.log_length = 2048;
    tb710fu_dwc3_poll(&f.core);
    assert(!f.active[3] && f.log_position == 0); // No host DTR yet.
    setup(&f, 0x21, 0x22, 3, 0, 0);
    assert(!f.core.dtr);
    status(&f, 1);
    assert(f.core.dtr && f.active[3] && f.length[3] == 2048);
    assert(f.active[5] && f.length[5] == 10);
    assert(f.device[f.buffer[5]] == 0xa1 && f.device[f.buffer[5] + 1] == 0x20);
    assert(f.device[f.buffer[5] + 8] == 3);
    assert(memcmp(f.device + f.buffer[3], f.log, 2048) == 0);
    const unsigned starts = f.starts[3];
    f.consume_block = 2;
    complete(&f, 3, 2048, 0);
    tb710fu_dwc3_poll(&f.core);
    assert(f.log_position == 0 && !f.active[3]);
    tb710fu_dwc3_poll(&f.core);
    assert(f.log_position == 0 && f.starts[3] == starts);
    tb710fu_dwc3_poll(&f.core);
    assert(f.log_position == 2048 && f.core.transmitted == 2048);
    assert(f.active[3] && f.length[3] == 0); // Exact multiple ends with ZLP.
    complete(&f, 3, 0, 0);
    complete(&f, 5, 10, 0);
    complete(&f, 2, 11, 0);
    tb710fu_dwc3_poll(&f.core);
    assert(!f.core.failed && !f.active[3] && f.active[2]);
    assert(f.core.discarded_input == 11 && f.consume_calls == 3);
    memcpy(f.log + 2048, "x\ny", 3);
    f.log_length += 3;
    tb710fu_dwc3_poll(&f.core);
    assert(f.active[3] && f.length[3] == 3);
    complete(&f, 3, 3, 0);
    tb710fu_dwc3_poll(&f.core);
    assert(!f.active[3] && f.core.transmitted == 2051 && f.log_position == 2051);
}

static void test_failures_retain_dma_and_do_not_restart(void)
{
    struct fixture f;
    prepare(&f);
    f.block_halt = 1;
    assert(!start(&f, "diagnostic-test-1"));
    assert(f.core.failed && f.core.dma == f.cpu && f.address_writes == 0);
    assert(f.cpu[0] == 0xa5 && f.waits >= 1000);
    prepare(&f);
    f.block_reset = 1;
    // Retracting the bit is a controlled state, not a wedged one: the adoption
    // path must program the core and expose its pull-up instead of giving up.
    const unsigned before = f.address_writes;
    assert(start(&f, "diagnostic-test-1"));
    assert(f.core.reset_adopted && f.core.reset_stage == 3 && f.core.running);
    assert(f.core.dma == f.cpu && f.address_writes > before);
    prepare(&f);
    f.stick_reset = 1;
    // Not even the retraction clears the bit. The status keeps saying "in reset",
    // so it cannot be the verdict on its own: the core is driven anyway and the
    // commands that complete are what decide. Without that the board would stay
    // on this one line for every boot and never test the configuration path.
    const unsigned forced_before = f.address_writes;
    assert(start(&f, "diagnostic-test-1"));
    assert(f.core.reset_forced && f.core.reset_stage == 0 && f.core.running);
    assert(!f.core.reset_adopted && f.core.dma == f.cpu);
    assert(f.address_writes > forced_before);
    // The arena is handed to the core on this path, so its contents are gone by
    // design; only the register traffic proves the core was driven.
    assert(f.cpu[0] == 0);
    // The probe keeps the readbacks, not the values software wrote: that is what
    // the report needs to tell a core that accepted the write from one that
    // ignored it.
    assert(f.core.probe_trace[1] == ((1u << 11) | (1u << 9) | 7u));
    assert(f.core.probe_trace[2] == (uint32_t)PHYSICAL);
    assert(f.core.probe_trace[4] == (uint32_t)(PHYSICAL >> 32));
    prepare(&f);
    f.block_command = 1;
    assert(!start(&f, "diagnostic-test-1"));
    assert(f.core.failed && f.core.dma == f.cpu);
    prepare(&f);
    f.bad_command_status = 4;
    assert(!start(&f, "diagnostic-test-1"));
    assert(f.core.failed);
    prepare(&f);
    assert(start(&f, "diagnostic-test-1"));
    connect(&f, 0);
    setup(&f, 0x80, 6, 0x0100, 0, 18);
    // A completion event is not enough: HWO still set means DMA is unproven.
    event(&f, (1u << 1) | (1u << 6));
    tb710fu_dwc3_poll(&f.core);
    assert(f.core.failed && f.core.busy[1] && f.log_position == 0);
    const unsigned writes = f.address_writes, starts = f.starts[1];
    tb710fu_dwc3_poll(&f.core);
    assert(f.address_writes == writes && f.starts[1] == starts);
    assert(!start(&f, "diagnostic-test-1"));
    prepare(&f);
    assert(start(&f, "diagnostic-test-1"));
    f.regs[0xc40c / 4] = 3;
    tb710fu_dwc3_poll(&f.core);
    assert(f.core.failed);
    prepare(&f);
    assert(start(&f, "diagnostic-test-1"));
    connect(&f, 0);
    configure(&f);
    device_event(&f, 1); // RX owns DMA: no unsafe cancel/reuse is attempted.
    tb710fu_dwc3_poll(&f.core);
    assert(f.core.failed && f.core.busy[2] && f.core.dma == f.cpu);
}

static void test_event_budget_and_wrap(void)
{
    struct fixture f;
    prepare(&f);
    assert(start(&f, "diagnostic-test-1"));
    for (unsigned turn = 0; turn < 20; ++turn) {
        for (unsigned i = 0; i < 80; ++i) device_event(&f, 3);
        const uint64_t before = f.core.events;
        tb710fu_dwc3_poll(&f.core);
        assert(f.core.events - before == 64 && f.regs[0xc40c / 4] == 64);
        tb710fu_dwc3_poll(&f.core);
        assert(f.core.events - before == 80 && f.regs[0xc40c / 4] == 0);
    }
    assert(!f.core.failed && f.core.events == 1600);
    assert(f.core.event_position == (1600u * 4) % TB710FU_DWC3_EVENT_BYTES);
}

int main(void)
{
    test_enumeration_and_normal_host_reset();
    test_full_speed_and_bounded_control_strings();
    test_firmware_pending_events_before_halt();
    test_line_coding_and_setup_abort();
    test_real_dma_log_delivery_and_commit_contention();
    test_failures_retain_dma_and_do_not_restart();
    test_device_role_and_event_irq();
    test_event_budget_and_wrap();
    puts("DWC3 production core: enumeration/reset, EP0, cache/DMA, CDC log delivery and fault tests PASS");
    return 0;
}
