// SPDX-License-Identifier: BSD-2-Clause-Patent
// XNU 12377 adapter for the original, polling-only DWC3 CDC transport.
// Hardware facts: tb710fu-linux@4d04410798b538fbe4b50664e3a66d9500a2ed31,
// sm8650.dtsi, gcc-sm8650.c, gdsc.c, tcsrcc-sm8650.c, phy-snps-eusb2.c,
// dwc3-qcom.c; SMMU fields: ARM IHI 0062. Firmware adoption plus a bounded
// GCC-side self-heal (USB30_PRIM GDSC, branch clocks, block reset). No PHY,
// regulator, PMIC repeater, SMMU or UFS reinitialization is attempted here.
// Replay an osfmk/startup.c compile record (MACH_KERNEL_PRIVATE). Use its
// native declarations; BSD param/systm mixes incompatible BSD/libsa types.
#include <kern/misc_protos.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <mach/vm_statistics.h>
#include <pexpert/pexpert.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>

#include "CdcDwc3.h"
#include "CdcHardware.h"
#include "CdcKernelTap.h"
#include "CdcTransportGuard.h"

#if !defined(__arm64__)
#error The TB710FU USB adapter requires ARM64 cache and MMIO ordering.
#endif

void tb710fu_cdc_start(void);

struct tb710fu_cdc_xnu {
    vm_offset_t gcc, phy_reset, reference, phy, smmu, dwc, qscratch;
    vm_offset_t dma;
    unsigned cache_line;
    struct tb710fu_dwc3 controller;
};

static struct tb710fu_cdc_xnu gCdc;
static unsigned gAttempted;
static unsigned gSettled;
static unsigned gSleepChannel;

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
    delay((int)microseconds);
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
// may instead finish charger detection with that block gated off, which the
// firmware-adoption predicate alone cannot recover from. Every register below
// sits in the always-on GCC aperture, so replaying the sequence cannot fault
// the USB interconnect: the GDSC only gates DWC3/PHY, never GCC itself.
#define CDC_GCC_BCR 0x00u          // block reset, bit0 = assert
#define CDC_GCC_GDSCR 0x04u        // GDSC control, bit0 = software collapse
#define CDC_GCC_GDSCR_STATUS 0x08u // GDSC status, bit16 = powered
#define CDC_GCC_MASTER 0x18u       // usb30_prim_master_clk
#define CDC_GCC_SLEEP 0x24u        // usb30_prim_sleep_clk
#define CDC_GCC_MOCK_UTMI 0x28u    // usb30_prim_mock_utmi_clk
#define CDC_GCC_CFG_NOC 0x88u      // cfg_noc_usb3_prim_axi_clk, halt voted
#define CDC_GCC_AGGRE_NOC 0x8cu    // aggre_usb3_prim_axi_clk, halt voted

static int cdc_branch_on(struct tb710fu_cdc_xnu *x, unsigned offset)
{
    uint32_t value = cdc_read(x->gcc, offset);
    if ((value & 1u) && !(value & (1u << 31))) return 1;
    // Bit31 is read-only halt state; clearing the hardware-gating vote keeps a
    // software enable from being masked by an idle vote on the NOC branches.
    value = (value & ~((1u << 31) | 2u)) | 1u;
    cdc_write(x->gcc, offset, value);
    for (unsigned i = 0; i < 50; ++i) {
        value = cdc_read(x->gcc, offset);
        if ((value & 1u) && !(value & (1u << 31))) return 1;
        delay(10);
    }
    return 0;
}

static void cdc_gcc_bring_up(struct tb710fu_cdc_xnu *x)
{
    uint32_t bcr = cdc_read(x->gcc, CDC_GCC_BCR);
    if (bcr & 1u) {
        cdc_write(x->gcc, CDC_GCC_BCR, bcr & ~1u);
        delay(1);
    }
    if (!(cdc_read(x->gcc, CDC_GCC_GDSCR_STATUS) & (1u << 16))) {
        cdc_write(x->gcc, CDC_GCC_GDSCR,
                  cdc_read(x->gcc, CDC_GCC_GDSCR) & ~1u);
        for (unsigned i = 0; i < 100; ++i) {
            if (cdc_read(x->gcc, CDC_GCC_GDSCR_STATUS) & (1u << 16)) break;
            delay(10);
        }
    }
    (void)cdc_branch_on(x, CDC_GCC_MASTER);
    (void)cdc_branch_on(x, CDC_GCC_SLEEP);
    (void)cdc_branch_on(x, CDC_GCC_MOCK_UTMI);
    (void)cdc_branch_on(x, CDC_GCC_CFG_NOC);
    (void)cdc_branch_on(x, CDC_GCC_AGGRE_NOC);
}

// One bounded line per control block: the boot log is the only channel until
// enumeration works, and the last screenful is what survives to a photo.
static void cdc_report_controls(struct tb710fu_cdc_xnu *x)
{
    printf("TB710FU CDC: gcc bcr=%08x gdscr=%08x sts=%08x m=%08x s=%08x "
           "u=%08x cfg=%08x ag=%08x\n",
           cdc_read(x->gcc, CDC_GCC_BCR), cdc_read(x->gcc, CDC_GCC_GDSCR),
           cdc_read(x->gcc, CDC_GCC_GDSCR_STATUS), cdc_read(x->gcc, CDC_GCC_MASTER),
           cdc_read(x->gcc, CDC_GCC_SLEEP), cdc_read(x->gcc, CDC_GCC_MOCK_UTMI),
           cdc_read(x->gcc, CDC_GCC_CFG_NOC), cdc_read(x->gcc, CDC_GCC_AGGRE_NOC));
}

static int cdc_active_controls(struct tb710fu_cdc_xnu *x)
{
    struct tb710fu_cdc_clock_state clocks = {
        .reset = cdc_read(x->gcc, 0),
        .power_status = cdc_read(x->gcc, 8),
        .master = cdc_read(x->gcc, 0x18),
        .sleep = cdc_read(x->gcc, 0x24),
        .mock_utmi = cdc_read(x->gcc, 0x28),
        .config = cdc_read(x->gcc, 0x88),
        .aggregate = cdc_read(x->gcc, 0x8c)
    };
    if (!tb710fu_cdc_clocks_active(&clocks)) {
        printf("TB710FU CDC: refuse clocks bcr=%08x cfg_gdsc=%08x "
               "master=%08x sleep=%08x utmi=%08x cfg=%08x aggre=%08x\n",
               clocks.reset, clocks.power_status, clocks.master, clocks.sleep,
               clocks.mock_utmi, clocks.config, clocks.aggregate);
        return 0;
    }
    struct tb710fu_cdc_phy_state phy = {
        .reset = cdc_read(x->phy_reset, 0),
        .reference = cdc_read(x->reference, 0x118),
        .control0 = cdc_read(x->phy, 0x3c),
        .control5 = cdc_read(x->phy, 0x50),
        .common0 = cdc_read(x->phy, 0x54),
        .control2 = cdc_read(x->phy, 0x64)
    };
    printf("TB710FU CDC: phy rst=%08x ref=%08x ctl0=%08x ctl5=%08x "
           "common0=%08x ctl2=%08x\n",
           phy.reset, phy.reference, phy.control0, phy.control5,
           phy.common0, phy.control2);
    if (!tb710fu_cdc_phy_adoptable(&phy)) {
        printf("TB710FU CDC: refuse PHY controls reset=%08x ref=%08x "
               "utmi0=%08x utmi5=%08x common0=%08x hs2=%08x\n",
               phy.reset, phy.reference, phy.control0, phy.control5,
               phy.common0, phy.control2);
        return 0;
    }
    return 1;
}

static int cdc_route(struct tb710fu_cdc_xnu *x,
                      struct tb710fu_cdc_identity_route *route)
{
    if (tb710fu_cdc_identity_route_read(cdc_smmu_read, x, route)) return 1;
    printf("TB710FU CDC: refuse SID40 route scr0=%08x id0=%08x id1=%08x "
           "id2=%08x matches=%u slot=%u smr=%08x s2cr=%08x bank=%u "
           "cbar=%08x cba2r=%08x sctlr=%08x\n",
           cdc_read(x->smmu, 0), cdc_read(x->smmu, 0x20),
           cdc_read(x->smmu, 0x24), cdc_read(x->smmu, 0x28),
           route->matching_streams, route->slot, route->smr, route->s2cr,
           route->bank, route->cbar, route->cba2r, route->sctlr);
    return 0;
}

static int cdc_prepare(struct tb710fu_cdc_xnu *x)
{
    // All mappings and checks below are read-only until the final QSCRATCH
    // session override. The exact DTS bounds the SMMU aperture at 1 MiB.
    x->gcc = ml_io_map(TB710FU_USB_GCC_WINDOW, 0x1000);
    x->phy_reset = ml_io_map(TB710FU_USB_GCC_PHY_RESET, 0x1000);
    x->reference = ml_io_map(TB710FU_USB_CLKREF, 0x1000);
    x->phy = ml_io_map(TB710FU_USB_EUSB2_BASE, 0x1000);
    if (!x->gcc || !x->phy_reset || !x->reference || !x->phy) {
        printf("TB710FU CDC: clock/PHY mapping unavailable\n");
        return 0;
    }
    // Bounded GCC-side bring-up: GDSC power, five branch clocks, block reset.
    // No PHY, PMIC, regulator or SMMU programming happens before the adoption
    // predicates below, which still decide whether this session may proceed.
    cdc_gcc_bring_up(x);
    cdc_report_controls(x);
    if (!cdc_active_controls(x)) return 0;
    x->dwc = ml_io_map(TB710FU_USB_DWC3_BASE, 0xe000);
    x->smmu = ml_io_map(TB710FU_USB_SMMU_BASE, TB710FU_USB_SMMU_BYTES);
    x->qscratch = ml_io_map(TB710FU_USB_QSCRATCH_BASE, 0x1000);
    if (!x->dwc || !x->smmu || !x->qscratch) {
        printf("TB710FU CDC: controller/SMMU mapping unavailable\n");
        return 0;
    }
    uint32_t id = cdc_read(x->dwc, 0xc120), control = cdc_read(x->dwc, 0xc110);
    unsigned role = (control >> 12) & 3u;
    unsigned address_bits = (cdc_read(x->dwc, 0xc140) >> 24) & 255u;
    if (((id >> 16) != 0x5533u && (id >> 16) != 0x3331u) ||
        address_bits < 32u || address_bits > 64u) {
        printf("TB710FU CDC: refuse controller id=%08x gctl=%08x dma_bits=%u\n",
               id, control, address_bits);
        return 0;
    }
    // The loader may hand the core over in host or OTG role. tb710fu_dwc3_start
    // halts the core and reprograms GCTL.PrtCapDir to device, so this is
    // reported rather than refused.
    if (role != 2u)
        printf("TB710FU CDC: controller role=%u at handoff, forcing device\n", role);
    struct tb710fu_cdc_identity_route route, refreshed;
    if (!cdc_route(x, &route)) return 0;
    if (route.address_bits < address_bits) address_bits = route.address_bits;
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    x->cache_line = 4u << ((ctr >> 16) & 15u);
    if (x->cache_line > 64u || PAGE_SIZE > TB710FU_DWC3_DMA_SIZE ||
        TB710FU_DWC3_DMA_SIZE % PAGE_SIZE) {
        printf("TB710FU CDC: unsupported cache line=%u page=%lu\n",
               x->cache_line, (unsigned long)PAGE_SIZE);
        return 0;
    }
    uint64_t max_page = address_bits == 64u ? UINT64_MAX >> PAGE_SHIFT :
                       ((UINT64_C(1) << address_bits) - 1u) >> PAGE_SHIFT;
    ppnum_t max_pnum = (ppnum_t)~(ppnum_t)0;
    if (max_page < max_pnum) max_pnum = (ppnum_t)max_page;
    kern_return_t kr = kmem_alloc_contig(kernel_map, &x->dma,
        TB710FU_DWC3_DMA_SIZE, TB710FU_CDC_DMA_ALIGNMENT - 1u, max_pnum,
        (TB710FU_CDC_DMA_ALIGNMENT - 1u) >> PAGE_SHIFT,
        KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT);
    if (kr != KERN_SUCCESS) {
        printf("TB710FU CDC: permanent DMA allocation failed kr=%d\n", kr);
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
    if (!cdc_active_controls(x) || !cdc_route(x, &refreshed)) return 0;
    if (memcmp(&route, &refreshed, sizeof(route)) ||
        (cdc_read(x->dwc, 0xc120) & 0xffff0000u) != (id & 0xffff0000u) ||
        ((cdc_read(x->dwc, 0xc110) >> 12) & 3u) != role) {
        printf("TB710FU CDC: controller/SID40 ownership changed during allocation\n");
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
    enum tb710fu_cdc_preflight_result preflight = tb710fu_cdc_preflight(1, &snapshot);
    if (preflight != TB710FU_CDC_PREFLIGHT_READY) {
        printf("TB710FU CDC: DMA preflight=%u pa=%llx bits=%u contiguous=%d\n",
               (unsigned)preflight, (unsigned long long)physical,
               address_bits, contiguous);
        return 0;
    }
    // Linux dwc3_qcom_vbus_override_enable defines these wrapper session bits.
    // USB2-only operation does not enable the SuperSpeed lane power override.
    uint32_t hs = cdc_read(x->qscratch, 0x10);
    cdc_write(x->qscratch, 0x10, hs | (1u << 20) | (1u << 28));
    if ((cdc_read(x->qscratch, 0x10) & ((1u << 20) | (1u << 28))) !=
        ((1u << 20) | (1u << 28))) {
        printf("TB710FU CDC: QSCRATCH VBUS override did not latch\n");
        return 0;
    }
    struct tb710fu_dwc3_ops ops = {
        .context = x, .read32 = cdc_dwc_read, .write32 = cdc_dwc_write,
        .delay_us = cdc_delay, .sync_for_cpu = cdc_sync_cpu,
        .sync_for_device = cdc_sync_device, .log_copy = cdc_copy,
        .log_consume = cdc_consume
    };
    // Development identity, deliberately synthetic and not a vendor serial.
    if (!tb710fu_dwc3_start(&x->controller, &ops, (uint8_t *)x->dma,
                            physical, 0x1209, 0x0001, "TB710FUCDC001")) {
        printf("TB710FU CDC: DWC3 start refused: %s\n", x->controller.failure);
        return 0;
    }
    printf("TB710FU CDC: USB2 platform up, DMA=%llx/%x, waiting for CDC "
           "enumeration and host DTR\n",
           (unsigned long long)physical, TB710FU_DWC3_DMA_SIZE);
    return 1;
}

static void cdc_worker(void *argument, wait_result_t wait_result)
{
    (void)argument;
    (void)wait_result;
    int ready = cdc_prepare(&gCdc);
    __atomic_store_n(&gSettled, 1u, __ATOMIC_RELEASE);
    if (ready) {
        unsigned previous = 0;
        while (gCdc.controller.running && !gCdc.controller.failed) {
            tb710fu_dwc3_poll(&gCdc.controller);
            unsigned current = gCdc.controller.configured |
                               ((unsigned)gCdc.controller.dtr << 1);
            if (current != previous) {
                printf("TB710FU CDC: configured=%u host_dtr=%u events=%llu tx=%llu\n",
                       (unsigned)gCdc.controller.configured,
                       (unsigned)gCdc.controller.dtr,
                       (unsigned long long)gCdc.controller.events,
                       (unsigned long long)gCdc.controller.transmitted);
                previous = current;
            }
            uint64_t deadline;
            clock_interval_to_deadline(1, NSEC_PER_MSEC, &deadline);
            (void)assert_wait_deadline((event_t)&gSleepChannel, THREAD_UNINT, deadline);
            thread_block(THREAD_CONTINUE_NULL);
        }
        printf("TB710FU CDC: worker stopped: %s (DMA retained)\n",
               gCdc.controller.failure ? gCdc.controller.failure : "controller stopped");
    }
    (void)thread_terminate(current_thread());
    thread_block(THREAD_CONTINUE_NULL);
}

// Called after the UFS/source-storage handoff and before bsd_autoconf by the
// device-owned, conditional BSD integration hook. Starting this worker never
// waits for a host; the bounded settle below only keeps the platform verdict
// on the last screenful of the boot log.
void tb710fu_cdc_start(void)
{
    int enabled = 0;
    if (!PE_parse_boot_argn("tb710fu-cdc", &enabled, sizeof(enabled)) || !enabled) return;
    if (enabled != 1) {
        printf("TB710FU CDC: tb710fu-cdc must be 0 or 1\n");
        return;
    }
    if (__atomic_exchange_n(&gAttempted, 1u, __ATOMIC_ACQ_REL)) return;
    thread_t worker = THREAD_NULL;
    kern_return_t kr = kernel_thread_start(cdc_worker, NULL, &worker);
    if (kr != KERN_SUCCESS) {
        printf("TB710FU CDC: worker creation failed kr=%d; controller untouched\n", kr);
        return;
    }
    // Up to 20 ms for the platform decision; the worker itself never blocks on
    // the host, and a slow scheduler simply resumes BSD without the verdict.
    for (unsigned i = 0; i < 200; ++i) {
        if (__atomic_load_n(&gSettled, __ATOMIC_ACQUIRE)) break;
        delay(100);
    }
    thread_deallocate(worker);
}
