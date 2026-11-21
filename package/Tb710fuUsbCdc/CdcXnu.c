// SPDX-License-Identifier: BSD-2-Clause-Patent
// XNU 12377 adapter for the DWC3 CDC transport.
// Hardware facts: tb710fu-linux@4d04410798b538fbe4b50664e3a66d9500a2ed31,
// sm8650.dtsi, gcc-sm8650.c, gdsc.c, tcsrcc-sm8650.c, phy-snps-eusb2.c,
// dwc3-qcom.c; SMMU fields: ARM IHI 0062. Firmware adoption plus a bounded
// GCC-side self-heal (USB30_PRIM GDSC, branch clocks, block reset). No PHY,
// regulator, PMIC repeater, SMMU or UFS reinitialization is attempted here.
// Replay an osfmk/startup.c compile record (MACH_KERNEL_PRIVATE). Use its
// native declarations; BSD param/systm mixes incompatible BSD/libsa types.
#include <kern/misc_protos.h>
#include <kern/clock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <mach/vm_statistics.h>
#include <pexpert/pexpert.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>
#include <stdarg.h>

#include "CdcDwc3.h"
#include "CdcHardware.h"
#include "CdcKernelTap.h"
#include "CdcTransportGuard.h"

#if !defined(__arm64__)
#error The TB710FU USB adapter requires ARM64 cache and MMIO ordering.
#endif

void tb710fu_cdc_start(void);
extern int vsnprintf(char *, size_t, const char *, va_list) __printflike(3, 0);
extern void tb710fu_live_log_boot_mark(unsigned lane, const char *stage)
    __attribute__((weak_import));
extern void tb710fu_live_log_putc(char byte)
    __attribute__((weak_import));
extern volatile uint32_t armcpu_gic_ready;
extern volatile uint64_t armcpu_timer_interrupts;
extern kern_return_t armcpu_spi_attach(uint32_t intid, bool edge,
    void (*handler)(void *), void *context, uint32_t *readback, uint64_t *router);
extern kern_return_t armcpu_spi_mask(uint32_t intid, bool masked);

struct tb710fu_cdc_xnu {
    vm_offset_t gcc, phy_reset, reference, phy, smmu, dwc, qscratch;
    vm_offset_t dma;
    unsigned cache_line;
    struct tb710fu_dwc3 controller;
};

static struct tb710fu_cdc_xnu gCdc;
static unsigned gAttempted;
static unsigned gSleepChannel;

static const char *gStage;
// Only the worker reads or writes the platform samples.
static struct {
    const char *failure;
    uint32_t gcc[9];   // bcr, gdscr, status, master, sleep, utmi, cfg, aggre, xo
    uint32_t phy[6];   // reset, reference, control0, control5, common0, control2
    uint32_t id, gctl, dma_bits, scr0, id0;
    unsigned streams, role;
    uint64_t dma_physical;
    uint32_t dma_bytes;
} gReport;

static void cdc_checkpoint(const char *label)
{
    if (tb710fu_live_log_boot_mark) tb710fu_live_log_boot_mark(1, label);
}

/* CDC can carry its own diagnostics even if the ordinary console is blocked. */
static void cdc_log(const char *format, ...) __printflike(1, 2);
static void cdc_log(const char *format, ...)
{
    char text[192];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (length <= 0) return;
    size_t count = (size_t)length < sizeof(text) ? (size_t)length : sizeof(text) - 1u;
    tb710fu_cdc_log_tap_append(text, count);
    if (tb710fu_live_log_putc) {
        for (size_t i = 0; i < count; ++i) tb710fu_live_log_putc(text[i]);
    }
}

static void cdc_mark(const char *stage)
{
    gStage = stage;
}

static void cdc_step(const char *stage, const char *label)
{
    cdc_mark(stage);
    cdc_checkpoint(label);
    cdc_log("TB710FU CDC: %s\n", stage);
}

static void cdc_refuse(const char *reason)
{
    gReport.failure = reason;
    cdc_checkpoint(reason);
}

static void cdc_irq(void *argument)
{
    (void)argument;
    // Level IRQ stays asserted until the worker drains the event ring.
    (void)armcpu_spi_mask(TB710FU_USB_DWC3_INTID, true);
    thread_wakeup_one((event_t)&gSleepChannel);
}

static uint32_t cdc_read(vm_offset_t base, unsigned offset)
{
    uint32_t value = *(volatile uint32_t *)(base + offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

static void cdc_write(vm_offset_t base, unsigned offset, uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint32_t cdc_dwc_read(void *context, unsigned offset)
{
    return cdc_read(((struct tb710fu_cdc_xnu *)context)->dwc, offset);
}

static void cdc_dwc_write(void *context, unsigned offset, uint32_t value)
{
    cdc_write(((struct tb710fu_cdc_xnu *)context)->dwc, offset, value);
}

static uint32_t cdc_smmu_read(void *context, unsigned offset)
{
    return cdc_read(((struct tb710fu_cdc_xnu *)context)->smmu, offset);
}

static void cdc_delay(void *context, unsigned microseconds)
{
    (void)context;
    // XNU delay() may block on the architectural timer in this context.
    // Match the short counter-based delay verified by the UFS 0007 patch.
    uint64_t deadline;
    clock_interval_to_deadline(microseconds, NSEC_PER_USEC, &deadline);
    while (mach_absolute_time() < deadline) __asm__ volatile("yield");
}

// Each controller descriptor occupies its own 64-byte cache line; the layout
// is rejected if CTR_EL0 advertises a larger minimum data cache line. Keep this
// explicit: Apple-coherent cache helpers can otherwise be compiled to no-ops.
static void cdc_cache_sync(struct tb710fu_cdc_xnu *x, unsigned offset,
                           unsigned bytes, int to_device)
{
    if (!bytes) return;
    if (offset > TB710FU_DWC3_DMA_SIZE ||
        bytes > TB710FU_DWC3_DMA_SIZE - offset) {
        panic("TB710FU CDC: invalid DMA cache range");
    }
    uintptr_t begin = (x->dma + offset) & ~(uintptr_t)(x->cache_line - 1u);
    uintptr_t end = x->dma + offset + bytes;
    for (uintptr_t p = begin; p < end; p += x->cache_line) {
        if (to_device) __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
        else __asm__ volatile("dc ivac, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static void cdc_sync_cpu(void *context, unsigned offset, unsigned bytes)
{
    cdc_cache_sync(context, offset, bytes, 0);
}

static void cdc_sync_device(void *context, unsigned offset, unsigned bytes)
{
    cdc_cache_sync(context, offset, bytes, 1);
}

static size_t cdc_copy(void *context, uint8_t *out, size_t bytes)
{
    (void)context;
    return tb710fu_cdc_log_tap_copy(out, bytes);
}

static int cdc_consume(void *context, size_t bytes)
{
    (void)context;
    return tb710fu_cdc_log_tap_consume(bytes);
}

// SM8650 USB30_PRIM block inside GCC (GCC 0x00100000 + 0x39000). Linux's
// dwc3-qcom probe powers the USB30_PRIM GDSC, enables the five branch clocks
// and deasserts the block reset before it touches DWC3; a normal ABL handoff
// may instead finish charger detection with that block gated off.
#define CDC_GCC_BCR 0x00u          // block reset, bit0 = assert
#define CDC_GCC_GDSCR 0x04u        // GDSC control, bit0 = software collapse
#define CDC_GCC_GDSCR_STATUS 0x08u // GDSC status, bit16 = powered
#define CDC_GCC_MASTER 0x18u       // usb30_prim_master_clk
#define CDC_GCC_SLEEP 0x24u        // usb30_prim_sleep_clk
#define CDC_GCC_MOCK_UTMI 0x28u    // usb30_prim_mock_utmi_clk
#define CDC_GCC_CFG_NOC 0x88u      // cfg_noc_usb3_prim_axi_clk, halt voted
#define CDC_GCC_AGGRE_NOC 0x8cu    // aggre_usb3_prim_axi_clk, halt voted

static void cdc_branch_on(struct tb710fu_cdc_xnu *x, unsigned offset, int hardware_gated)
{
    uint32_t value = cdc_read(x->gcc, offset);
    if (tb710fu_cdc_branch_active(value, hardware_gated)) return;
    // Linux's branch gate updates only BIT(0). Keep the NOC HWCG vote.
    if (!(value & 1u)) cdc_write(x->gcc, offset, value | 1u);
    for (unsigned i = 0; i < 50; ++i) {
        value = cdc_read(x->gcc, offset);
        if (tb710fu_cdc_branch_active(value, hardware_gated)) return;
        cdc_delay(NULL, 10);
    }
}

static int cdc_gcc_bring_up(struct tb710fu_cdc_xnu *x)
{
    cdc_step("gcc-reset", "cdcgcc");
    uint32_t bcr = cdc_read(x->gcc, CDC_GCC_BCR);
    if (bcr & 1u) {
        cdc_write(x->gcc, CDC_GCC_BCR, bcr & ~1u);
        cdc_delay(NULL, 1);
    }
    cdc_step("gcc-gdsc", "cdcgdsc");
    if (!(cdc_read(x->gcc, CDC_GCC_GDSCR_STATUS) & (1u << 16))) {
        cdc_write(x->gcc, CDC_GCC_GDSCR,
                  cdc_read(x->gcc, CDC_GCC_GDSCR) & ~1u);
        for (unsigned i = 0; i < 100; ++i) {
            if (cdc_read(x->gcc, CDC_GCC_GDSCR_STATUS) & (1u << 16)) break;
            cdc_delay(NULL, 10);
        }
    }
    gReport.gcc[2] = cdc_read(x->gcc, CDC_GCC_GDSCR_STATUS);
    if (!(gReport.gcc[2] & (1u << 16))) {
        cdc_refuse("gcc-power");
        return 0;
    }
    cdc_step("gcc-branches", "cdcclks");
    cdc_branch_on(x, CDC_GCC_MASTER, 0);
    cdc_branch_on(x, CDC_GCC_SLEEP, 0);
    cdc_branch_on(x, CDC_GCC_MOCK_UTMI, 0);
    cdc_branch_on(x, CDC_GCC_CFG_NOC, 1);
    cdc_branch_on(x, CDC_GCC_AGGRE_NOC, 1);

    // sm8650.dtsi names TCSR_USB3_CLKREF_EN as the wrapper's "xo".
    cdc_step("wrapper-xo", "cdcxoref");
    uint32_t xo = cdc_read(x->reference, TB710FU_USB_WRAPPER_XO);
    if (!(xo & 1u)) {
        cdc_write(x->reference, TB710FU_USB_WRAPPER_XO, xo | 1u);
        cdc_delay(NULL, 10);  // Linux BRANCH_HALT_DELAY.
    }
    return 1;
}

// Sample every control block in one fixed order so the photo always shows the
// same eight words, whether the session went on to enumerate or refused.
static void cdc_sample_controls(struct tb710fu_cdc_xnu *x)
{
    cdc_mark("gcc-sample");
    cdc_checkpoint("gccread");
    gReport.gcc[0] = cdc_read(x->gcc, CDC_GCC_BCR);
    gReport.gcc[1] = cdc_read(x->gcc, CDC_GCC_GDSCR);
    gReport.gcc[2] = cdc_read(x->gcc, CDC_GCC_GDSCR_STATUS);
    gReport.gcc[3] = cdc_read(x->gcc, CDC_GCC_MASTER);
    gReport.gcc[4] = cdc_read(x->gcc, CDC_GCC_SLEEP);
    gReport.gcc[5] = cdc_read(x->gcc, CDC_GCC_MOCK_UTMI);
    gReport.gcc[6] = cdc_read(x->gcc, CDC_GCC_CFG_NOC);
    gReport.gcc[7] = cdc_read(x->gcc, CDC_GCC_AGGRE_NOC);
    gReport.gcc[8] = cdc_read(x->reference, TB710FU_USB_WRAPPER_XO);
}

static int cdc_active_controls(struct tb710fu_cdc_xnu *x)
{
    struct tb710fu_cdc_clock_state clocks = {
        .reset = gReport.gcc[0],
        .power_status = gReport.gcc[2],
        .master = gReport.gcc[3],
        .sleep = gReport.gcc[4],
        .mock_utmi = gReport.gcc[5],
        .config = gReport.gcc[6],
        .aggregate = gReport.gcc[7]
    };
    cdc_mark("clock-check");
    cdc_checkpoint("clkcheck");
    if (!tb710fu_cdc_clocks_active(&clocks) || !(gReport.gcc[8] & 1u)) {
        cdc_refuse("clocks");
        return 0;
    }
    cdc_step("phy-gates", "cdcphyg");
    gReport.phy[0] = cdc_read(x->phy_reset, 0);
    gReport.phy[1] = cdc_read(x->reference, TB710FU_USB_PHY_XO);
    if (!tb710fu_cdc_phy_clocked(gReport.phy[0], gReport.phy[1])) {
        cdc_refuse("phy-gates");
        return 0;
    }
    cdc_step("phy-sample", "cdcphyr");
    struct tb710fu_cdc_phy_state phy = {
        .reset = gReport.phy[0],
        .reference = gReport.phy[1],
        .control0 = cdc_read(x->phy, 0x3c),
        .control5 = cdc_read(x->phy, 0x50),
        .common0 = cdc_read(x->phy, 0x54),
        .control2 = cdc_read(x->phy, 0x64)
    };
    gReport.phy[2] = phy.control0; gReport.phy[3] = phy.control5;
    gReport.phy[4] = phy.common0; gReport.phy[5] = phy.control2;
    if (!tb710fu_cdc_phy_adoptable(&phy)) {
        cdc_refuse("phy");
        return 0;
    }
    return 1;
}

static int cdc_route(struct tb710fu_cdc_xnu *x,
                      struct tb710fu_cdc_identity_route *route)
{
    cdc_step("smmu-route", "cdcsmmu");
    int routed = tb710fu_cdc_identity_route_read(cdc_smmu_read, x, route);
    gReport.scr0 = cdc_read(x->smmu, 0);
    gReport.id0 = cdc_read(x->smmu, 0x20);
    gReport.streams = route->matching_streams;
    if (routed) return 1;
    cdc_refuse("sid40");
    return 0;
}

static int cdc_prepare(struct tb710fu_cdc_xnu *x)
{
    cdc_step("map-clock", "cdcmap");
    x->gcc = ml_io_map(TB710FU_USB_GCC_WINDOW, 0x1000);
    x->phy_reset = ml_io_map(TB710FU_USB_GCC_PHY_RESET, 0x1000);
    x->reference = ml_io_map(TB710FU_USB_CLKREF, 0x1000);
    x->phy = ml_io_map(TB710FU_USB_EUSB2_BASE, 0x1000);
    if (!x->gcc || !x->phy_reset || !x->reference || !x->phy) {
        cdc_refuse("clock-map");
        return 0;
    }
    // Bounded GCC-side bring-up: GDSC power, five branch clocks, block reset.
    // No PHY, PMIC, regulator or SMMU programming happens before the adoption
    // predicates below, which still decide whether this session may proceed.
    if (!cdc_gcc_bring_up(x)) return 0;
    cdc_sample_controls(x);
    if (!cdc_active_controls(x)) return 0;
    cdc_mark("map-controller");
    cdc_checkpoint("coremap");
    x->dwc = ml_io_map(TB710FU_USB_DWC3_BASE, 0xe000);
    x->smmu = ml_io_map(TB710FU_USB_SMMU_BASE, TB710FU_USB_SMMU_BYTES);
    x->qscratch = ml_io_map(TB710FU_USB_QSCRATCH_BASE, 0x1000);
    if (!x->dwc || !x->smmu || !x->qscratch) {
        cdc_refuse("controller-map");
        return 0;
    }
    cdc_step("core-id", "cdccore");
    uint32_t id = cdc_read(x->dwc, 0xc120), control = cdc_read(x->dwc, 0xc110);
    unsigned role = (control >> 12) & 3u;
    unsigned address_bits = (cdc_read(x->dwc, 0xc140) >> 24) & 255u;
    gReport.id = id; gReport.gctl = control; gReport.dma_bits = address_bits;
    gReport.role = role;
    if (((id >> 16) != 0x5533u && (id >> 16) != 0x3331u) ||
        address_bits < 32u || address_bits > 64u) {
        cdc_refuse("controller");
        return 0;
    }
    // The device halt sequence cannot quiesce an xHCI host or an OTG session.
    if (role != 2u) {
        cdc_refuse("handoff-role");
        return 0;
    }
    struct tb710fu_cdc_identity_route route, refreshed;
    if (!cdc_route(x, &route)) return 0;
    if (route.address_bits < address_bits) address_bits = route.address_bits;
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    x->cache_line = 4u << ((ctr >> 16) & 15u);
    if (x->cache_line > 64u || PAGE_SIZE > TB710FU_DWC3_DMA_SIZE ||
        TB710FU_DWC3_DMA_SIZE % PAGE_SIZE) {
        cdc_refuse("cache-line");
        return 0;
    }
    uint64_t max_page = address_bits == 64u ? UINT64_MAX >> PAGE_SHIFT :
                       ((UINT64_C(1) << address_bits) - 1u) >> PAGE_SHIFT;
    ppnum_t max_pnum = (ppnum_t)~(ppnum_t)0;
    if (max_page < max_pnum) max_pnum = (ppnum_t)max_page;
    cdc_step("dma-alloc", "cdcdma");
    kern_return_t kr = kmem_alloc_contig(kernel_map, &x->dma,
        TB710FU_DWC3_DMA_SIZE, TB710FU_CDC_DMA_ALIGNMENT - 1u, max_pnum,
        (TB710FU_CDC_DMA_ALIGNMENT - 1u) >> PAGE_SHIFT,
        KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT);
    if (kr != KERN_SUCCESS) {
        cdc_refuse("dma-alloc");
        return 0;
    }
    // This permanent arena is never freed, including any failure after a
    // controller write: a timed-out stop cannot prove DMA has ceased.
    uint64_t physical = ptoa_64(pmap_find_phys(kernel_pmap, x->dma));
    int contiguous = physical != 0;
    for (unsigned offset = 0; offset < TB710FU_DWC3_DMA_SIZE; offset += PAGE_SIZE) {
        if (ptoa_64(pmap_find_phys(kernel_pmap, x->dma + offset)) != physical + offset)
            contiguous = 0;
    }
    // Re-read the live route and controls after allocation, before any device
    // write. No translation register or stream table is changed by this path.
    cdc_mark("dma-check");
    cdc_checkpoint("dmacheck");
    cdc_sample_controls(x);
    if (!cdc_active_controls(x) || !cdc_route(x, &refreshed)) return 0;
    if (memcmp(&route, &refreshed, sizeof(route)) ||
        (cdc_read(x->dwc, 0xc120) & 0xffff0000u) != (id & 0xffff0000u) ||
        ((cdc_read(x->dwc, 0xc110) >> 12) & 3u) != role) {
        cdc_refuse("ownership");
        return 0;
    }
    struct tb710fu_cdc_transport_snapshot snapshot = {
        .usb_role = TB710FU_CDC_USB_ROLE_DEVICE,
        .clocks_verified = 1, .required_clocks_enabled = 1,
        // Readiness here means the source-backed active controls above. PMIC
        // rail/repeater and analog link readiness require real enumeration.
        .phy_verified = 1, .phy_ready = 1,
        .smmu_verified = 1, .smmu_identity = 1,
        .smmu_stream_id = TB710FU_CDC_DWC3_STREAM_ID,
        .matching_streams = (uint16_t)route.matching_streams,
        .dma_contiguous = (uint8_t)contiguous,
        .dma_address_bits = (uint8_t)address_bits,
        .dma_physical = physical, .dma_bytes = TB710FU_DWC3_DMA_SIZE
    };
    cdc_mark("dma-preflight");
    cdc_checkpoint("dmapre");
    enum tb710fu_cdc_preflight_result preflight = tb710fu_cdc_preflight(1, &snapshot);
    if (preflight != TB710FU_CDC_PREFLIGHT_READY) {
        cdc_refuse("preflight");
        return 0;
    }
    // Linux dwc3_qcom_vbus_override_enable defines these wrapper session bits.
    // USB2-only operation does not enable the SuperSpeed lane power override.
    cdc_step("vbus", "cdcvbus");
    uint32_t hs = cdc_read(x->qscratch, 0x10);
    cdc_write(x->qscratch, 0x10, hs | (1u << 20) | (1u << 28));
    if ((cdc_read(x->qscratch, 0x10) & ((1u << 20) | (1u << 28))) !=
        ((1u << 20) | (1u << 28))) {
        cdc_refuse("vbus");
        return 0;
    }
    struct tb710fu_dwc3_ops ops = {
        .context = x, .read32 = cdc_dwc_read, .write32 = cdc_dwc_write,
        .delay_us = cdc_delay, .sync_for_cpu = cdc_sync_cpu,
        .sync_for_device = cdc_sync_device, .log_copy = cdc_copy,
        .log_consume = cdc_consume
    };
    // Development identity, deliberately synthetic and not a vendor serial.
    cdc_step("dwc3-start", "cdcdwc");
    if (!tb710fu_dwc3_start(&x->controller, &ops, (uint8_t *)x->dma,
                            physical, 0x1209, 0x0001, "TB710FUCDC001")) {
        cdc_refuse(x->controller.failure ? x->controller.failure : "dwc3-start");
        return 0;
    }
    gReport.dma_physical = physical;
    gReport.dma_bytes = TB710FU_DWC3_DMA_SIZE;
    cdc_mark("ready");
    cdc_checkpoint("dwcstart");
    return 1;
}

static void cdc_worker(void *argument, wait_result_t wait_result)
{
    (void)argument;
    (void)wait_result;
    cdc_checkpoint("cdcentry");
    uint64_t timer_ctl;
    __asm__ volatile("mrs %0, cntv_ctl_el0" : "=r"(timer_ctl));
    cdc_log("TB710FU CDC: worker gic=%u timer=%llu cntv_ctl=%llx irq=%u\n",
           armcpu_gic_ready, (unsigned long long)armcpu_timer_interrupts,
           (unsigned long long)timer_ctl, (unsigned)ml_get_interrupts_enabled());
    cdc_checkpoint("cdcwork");
    const int ready = cdc_prepare(&gCdc);
    cdc_log("TB710FU CDC: stage=%s failure=%s\n", gStage,
           gReport.failure ? gReport.failure : "-");
    cdc_log("TB710FU CDC: gcc bcr=%08x gdscr=%08x sts=%08x m=%08x s=%08x "
           "u=%08x cfg=%08x ag=%08x xo=%08x\n",
           gReport.gcc[0], gReport.gcc[1], gReport.gcc[2], gReport.gcc[3],
           gReport.gcc[4], gReport.gcc[5], gReport.gcc[6], gReport.gcc[7],
           gReport.gcc[8]);
    cdc_log("TB710FU CDC: phy rst=%08x ref=%08x ctl0=%08x ctl5=%08x "
           "common0=%08x ctl2=%08x\n",
           gReport.phy[0], gReport.phy[1], gReport.phy[2], gReport.phy[3],
           gReport.phy[4], gReport.phy[5]);
    cdc_log("TB710FU CDC: dwc id=%08x gctl=%08x role=%u bits=%u "
           "smmu scr0=%08x id0=%08x streams=%u\n",
           gReport.id, gReport.gctl, gReport.role, gReport.dma_bits,
           gReport.scr0, gReport.id0, gReport.streams);
    if (ready) {
        cdc_checkpoint("irqroute");
        uint32_t spi_state = 0;
        kern_return_t spi = armcpu_spi_attach(TB710FU_USB_DWC3_INTID, false,
                                              cdc_irq, NULL, &spi_state, NULL);
        const int event_irq = spi == KERN_SUCCESS && (spi_state & 7u) == 3u;
        if (event_irq) {
            tb710fu_dwc3_enable_event_irq(&gCdc.controller);
        } else if (spi == KERN_SUCCESS) {
            (void)armcpu_spi_mask(TB710FU_USB_DWC3_INTID, true);
        }
        cdc_log("TB710FU CDC: event irq=%u kr=%d gic=%08x\n",
               (unsigned)event_irq, spi, spi_state);
        cdc_log("TB710FU CDC: platform up dma=%llx/%x, waiting for enumeration "
               "and host DTR\n",
               (unsigned long long)gReport.dma_physical, (unsigned)gReport.dma_bytes);
        cdc_checkpoint("cdcready");
        unsigned previous = 0;
        int bus_seen = 0, setup_seen = 0;
        while (gCdc.controller.running && !gCdc.controller.failed) {
            tb710fu_dwc3_poll(&gCdc.controller);
            if (!bus_seen && gCdc.controller.connected) {
                cdc_checkpoint("cdcbus");
                cdc_log("TB710FU CDC: connect-done maxpacket=%u\n", gCdc.controller.packet_size);
                bus_seen = 1;
            }
            if (!setup_seen && gCdc.controller.setup_requests) {
                cdc_checkpoint("cdcsetup");
                cdc_log("TB710FU CDC: first EP0 SETUP\n");
                setup_seen = 1;
            }
            unsigned state = gCdc.controller.configured |
                             ((unsigned)gCdc.controller.dtr << 1);
            if (state != previous) {
                cdc_checkpoint(gCdc.controller.dtr ? "cdcdtr" :
                               gCdc.controller.configured ? "cdccfg" : "cdcevent");
                cdc_log("TB710FU CDC: configured=%u dtr=%u\n",
                       (unsigned)gCdc.controller.configured,
                       (unsigned)gCdc.controller.dtr);
                previous = state;
            }
            if (!gCdc.controller.running) break;
            uint64_t deadline;
            clock_interval_to_deadline(1, NSEC_PER_MSEC, &deadline);
            (void)assert_wait_deadline((event_t)&gSleepChannel, THREAD_UNINT, deadline);
            // Register the waiter first. A pending level interrupt can now
            // wake it even if the architectural timer never fires.
            if (event_irq) (void)armcpu_spi_mask(TB710FU_USB_DWC3_INTID, false);
            thread_block(THREAD_CONTINUE_NULL);
        }
        if (event_irq) (void)armcpu_spi_mask(TB710FU_USB_DWC3_INTID, true);
        cdc_checkpoint("cdcstop");
        cdc_log("TB710FU CDC: transport stopped: %s\n",
               gCdc.controller.failure ? gCdc.controller.failure : "stopped");
    }
    (void)thread_terminate(current_thread());
    thread_block(THREAD_CONTINUE_NULL);
}

// BSD queues optional USB capture and proceeds immediately to root discovery.
void tb710fu_cdc_start(void)
{
    int enabled = 0;
    if (!PE_parse_boot_argn("tb710fu-cdc", &enabled, sizeof(enabled)) || !enabled) return;
    if (enabled != 1) {
        cdc_log("TB710FU CDC: tb710fu-cdc must be 0 or 1\n");
        return;
    }
    if (__atomic_exchange_n(&gAttempted, 1u, __ATOMIC_ACQ_REL)) return;
    thread_t worker = THREAD_NULL;
    // Match the bootstrap thread's priority so USB can receive an early
    // scheduling quantum without a wait or hardware access in the BSD hook.
    cdc_checkpoint("cdcqueue");
    kern_return_t kr = kernel_thread_start_priority(cdc_worker, NULL, MAXPRI_KERNEL, &worker);
    if (kr != KERN_SUCCESS) {
        cdc_checkpoint("cdcnothr");
        cdc_log("TB710FU CDC: worker creation failed kr=%d\n", kr);
        return;
    }
    thread_deallocate(worker);
    cdc_log("TB710FU CDC: worker queued gic=%u timer=%llu irq=%u\n",
           armcpu_gic_ready, (unsigned long long)armcpu_timer_interrupts,
           (unsigned)ml_get_interrupts_enabled());
}
