// SPDX-License-Identifier: BSD-2-Clause-Patent
// XNU 12377 adapter for the DWC3 CDC transport.
// SM8650 controller, clock and PHY facts: tb710fu-linux@4d04410798b538fbe4b50664e3a66d9500a2ed31.
// RPMh and PM8550B SPMI transaction formats follow the official UniXNU-sheng
// reference at 516a780473a2ac59c8b438e132bb60204dda9e22. SMMU is adopted
// read-only; the only exception is one bypass stream group for SID 0x40, taken
// when the boot chain left no route, and published with read-back and undo.
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
#include <IOKit/IOLib.h>
#include <stdarg.h>

#include "CdcBoardPower.h"
#include "CdcDwc3.h"
#include "CdcHardware.h"
#include "CdcKernelTap.h"
#include "CdcQcomRpmh.h"
#include "CdcTransportGuard.h"

#if !defined(__arm64__)
#error The TB710FU USB adapter requires ARM64 cache and MMIO ordering.
#endif

void tb710fu_cdc_start(void);
extern int vsnprintf(char *, size_t, const char *, va_list) __printflike(3, 0);
extern void tb710fu_live_log_boot_mark(unsigned lane, const char *stage)
    __attribute__((weak_import));
/* A boot mark only paints the lane strip; it never builds a frame. CDC text
 * therefore stayed outside every QR frame, since frames are only rebuilt by a
 * checkpoint publish. The text checkpoint paints the captured tail, which is
 * where the CDC transcript lives once BSD has flooded the console. */
extern void tb710fu_live_log_checkpoint_text(const char *stage)
    __attribute__((weak_import));
extern void tb710fu_live_log_putc(char byte)
    __attribute__((weak_import));
/* Optional: only the LiveLog archive provides this, and CDC must still link and
 * run when that archive is not linked in. The length is unsigned long rather
 * than size_t so the declaration also matches the osfmk header, which does not
 * include the C library type. */
extern void tb710fu_live_log_cdc_bytes(const char *bytes, unsigned long length)
    __attribute__((weak_import));
extern volatile uint32_t armcpu_gic_ready;
extern volatile uint64_t armcpu_timer_interrupts;
extern kern_return_t armcpu_spi_attach(uint32_t intid, bool edge,
    void (*handler)(void *), void *context, uint32_t *readback, uint64_t *router);
extern kern_return_t armcpu_spi_mask(uint32_t intid, bool masked);

struct tb710fu_cdc_xnu {
    vm_offset_t gcc, phy_reset, reference, phy, smmu, dwc, qscratch;
    vm_offset_t cmd_db, rsc;
    vm_offset_t dma;
    unsigned cache_line;
    struct tb710fu_dwc3 controller;
};

static struct tb710fu_cdc_xnu gCdc;
static unsigned gAttempted;
static unsigned gSleepChannel;
static IOLock *gCdcTcsLock;

static const char *gStage;
// Only the worker reads or writes the platform samples.
static struct {
    const char *failure;
    uint32_t gcc[9];   // bcr, gdscr, status, master, sleep, utmi, cfg, aggre, xo
    uint32_t phy[6];   // reset, reference, control0, control5, common0, control2
    uint32_t power_flags, repeater_status;
    uint32_t bcm_flags, bcm_values[4];
    uint32_t id, gctl, dma_bits, scr0, id0;
    uint32_t smmu_saved_smr, smmu_saved_s2cr;
    uint32_t smmu_probe_smr, smmu_probe_s2cr;
    uint32_t smmu_actions[4], smmu_acr, smmu_gfsr, smmu_scr0;
    uint32_t smmu_smr, smmu_s2cr, smmu_cbar, smmu_cba2r, smmu_sctlr;
    uint32_t qscratch_gen[2], qscratch_hs[2];
    unsigned streams, role, spmi_apid, smmu_slot, smmu_bank, smmu_tries, smmu_bank_tries;
    unsigned smmu_claimed, smmu_masked, smmu_open;
    uint64_t dma_physical;
    uint32_t dma_bytes;
} gReport;

/* Bump this literal on every build. Three boots were already spent
 * reverse-identifying a photographed frame by grepping kernel strings; a frame
 * that names its own image costs one line and ends that. */
#define TB710FU_CDC_IMAGE "pl6"

static void cdc_checkpoint(const char *label)
{
    if (tb710fu_live_log_boot_mark) tb710fu_live_log_boot_mark(1, label);
    // Marks move the bottom lane, not the frame; without this publish the CDC
    // transcript never reaches a QR frame at all.
    if (tb710fu_live_log_checkpoint_text)
        tb710fu_live_log_checkpoint_text(label);
}

// Lane-only mark for steps that may never return: a register access that
// wedges the bus cannot publish a frame, but it can still leave its tag on
// the bottom band, which is what the next boot is read from.
static void cdc_lane_op(void *context, const char *label)
{
    (void)context;
    if (tb710fu_live_log_boot_mark) tb710fu_live_log_boot_mark(1, label);
}

/* CDC can carry its own diagnostics even if the ordinary console is blocked. */static void cdc_log(const char *format, ...) __printflike(1, 2);
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
    // The tap above is readable only by the CDC channel, so it cannot report the
    // failure that keeps that channel down. Mirror the same line into the ring
    // the QR frames are built from; the ordinary console scrolls far too fast
    // to be read by eye during a CDC start.
    if (tb710fu_live_log_cdc_bytes) tb710fu_live_log_cdc_bytes(text, count);
}

static void cdc_mark(const char *stage)
{
    gStage = stage;
}

static void cdc_step(const char *stage, const char *label)
{
    cdc_mark(stage);
    // Print first, publish second: a checkpoint captures the ring tail before
    // this line exists otherwise, so the frame always showed the *previous*
    // stage while the lane already named this one.
    cdc_log("TB710FU CDC: %s\n", stage);
    cdc_checkpoint(label);
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

static void cdc_write(vm_offset_t base, unsigned offset, uint32_t value);
static void cdc_delay(void *context, unsigned microseconds);

static int cdc_rpmh_issue(struct tb710fu_cdc_xnu *x,
                          const struct tb710fu_cdc_rsc_layout *layout,
                          const uint32_t *addresses, const uint32_t *values,
                          unsigned count, const char *stage)
{
    struct tb710fu_cdc_rpmh_write writes[18];
    unsigned n = tb710fu_cdc_rpmh_active_request(layout, 2u, addresses,
                                                   values, count, writes,
                                                   sizeof(writes) / sizeof(writes[0]));
    if (!n) return 0;
    cdc_mark(stage);
    cdc_checkpoint(stage);
    for (unsigned i = 0; i < n; ++i)
        cdc_write(x->rsc, 0xd00u + writes[i].offset, writes[i].value);
    // On this board the active-TCS completion latch is not visible through
    // the CPU read path, even though the RPMh request is accepted. Match the
    // same-device helper's bounded delay and always return the TCS to idle.
    cdc_delay(NULL, 2000u);
    n = tb710fu_cdc_rpmh_finish(layout, 2u, writes,
                                  sizeof(writes) / sizeof(writes[0]));
    if (!n) return 0;
    for (unsigned i = 0; i < n; ++i)
        cdc_write(x->rsc, 0xd00u + writes[i].offset, writes[i].value);
    return 1;
}

static int cdc_tcs_lock(void)
{
    if (!gCdcTcsLock) gCdcTcsLock = IOLockAlloc();
    if (!gCdcTcsLock) return 0;
    IOLockLock(gCdcTcsLock);
    return 1;
}

static void cdc_tcs_unlock(void)
{
    if (gCdcTcsLock) IOLockUnlock(gCdcTcsLock);
}

static uint64_t cdc_bcm_div(uint64_t numerator, uint64_t denominator)
{
    // Match Linux bcm-voter.c. Small non-zero votes survive integer scaling,
    // while the normal path intentionally truncates like the RPMh driver.
    if (!denominator) return 0;
    if (numerator && numerator < denominator) return 1;
    return numerator / denominator;
}

static int cdc_bcm_data(const struct tb710fu_cdc_rpmh_resource *resource,
                        uint64_t average, uint64_t peak, unsigned buswidth,
                        unsigned channels, int enable_only, uint32_t *data)
{
    if (!resource || !data || !resource->address || !buswidth || !channels)
        return 0;
    if (enable_only) {
        *data = (1u << 30) | (1u << 29) | 1u;
        return 1;
    }
    if (resource->level_count < 3u) return 0;
    const uint64_t unit = resource->levels[0] |
                          (uint64_t)resource->levels[1] << 16;
    const uint64_t width = resource->levels[2];
    if (!unit || !width) return 0;
    const uint64_t average_node = cdc_bcm_div(
        average * width, (uint64_t)buswidth * channels);
    const uint64_t peak_node = cdc_bcm_div(peak * width, buswidth);
    uint64_t vote_x = cdc_bcm_div(average_node * 1000u, unit);
    uint64_t vote_y = cdc_bcm_div(peak_node * 1000u, unit);
    if (vote_x > 0x3fffu) vote_x = 0x3fffu;
    if (vote_y > 0x3fffu) vote_y = 0x3fffu;
    *data = (1u << 30) | (1u << 29) |
            ((uint32_t)vote_x << 14) | (uint32_t)vote_y;
    return 1;
}

static int cdc_bcm_vote(struct tb710fu_cdc_xnu *x,
                        const struct tb710fu_cdc_rsc_layout *layout,
                        const uint8_t *db, const char *name,
                        uint64_t average, uint64_t peak, unsigned buswidth,
                        unsigned channels, int enable_only, unsigned bit)
{
    struct tb710fu_cdc_rpmh_resource resource;
    if (tb710fu_cdc_cmd_db_find(db, TB710FU_USB_CMD_DB_BYTES, name,
                                &resource) != TB710FU_CDC_RPMH_OK) {
        cdc_refuse(name);
        return 0;
    }
    uint32_t value = 0;
    if (!cdc_bcm_data(&resource, average, peak, buswidth, channels,
                      enable_only, &value)) {
        cdc_refuse("bcm-aux");
        return 0;
    }
    const uint32_t address = resource.address;
    if (!cdc_rpmh_issue(x, layout, &address, &value, 1u, name)) {
        cdc_refuse("bcm-vote");
        return 0;
    }
    gReport.bcm_flags |= bit;
    gReport.bcm_values[bit == 1u ? 0u : bit == 2u ? 1u : bit == 4u ? 2u : 3u] = value;
    return 1;
}

static int cdc_power_rails(struct tb710fu_cdc_xnu *x)
{
    cdc_step("rpmh-map", "rpmh-map");
    x->cmd_db = ml_io_map(TB710FU_USB_CMD_DB_BASE, TB710FU_USB_CMD_DB_BYTES);
    x->rsc = ml_io_map(TB710FU_USB_RSC_DRV2_BASE, TB710FU_USB_RSC_BYTES);
    if (!x->cmd_db || !x->rsc) {
        cdc_refuse("rpmh-map");
        return 0;
    }

    uint8_t *db = IOMallocData(TB710FU_USB_CMD_DB_BYTES);
    if (!db) {
        cdc_refuse("rpmh-alloc");
        return 0;
    }
    volatile uint32_t *db_words = (volatile uint32_t *)x->cmd_db;
    for (unsigned offset = 0; offset < TB710FU_USB_CMD_DB_BYTES; offset += 4u) {
        const uint32_t word = db_words[offset / 4u];
        db[offset] = (uint8_t)word;
        db[offset + 1u] = (uint8_t)(word >> 8);
        db[offset + 2u] = (uint8_t)(word >> 16);
        db[offset + 3u] = (uint8_t)(word >> 24);
    }
    __asm__ volatile("dmb oshld" ::: "memory");
    const struct tb710fu_cdc_rsc_layout layout =
        tb710fu_cdc_rsc_layout(cdc_read(x->rsc, 0));
    int ok = layout.tcs_stride != 0;
    int tcs_locked = 0;
    if (ok) {
        tcs_locked = cdc_tcs_lock();
        if (!tcs_locked) ok = 0;
    }
    if (ok) {
        // TCS 2 is shared with the board's other RPMh clients. The active
        // completion IRQ must be enabled before the first request.
        cdc_step("rpmh-irq", "rpmh-irq");
        const unsigned irq_offset = 0xd00u + layout.irq_enable;
        cdc_write(x->rsc, irq_offset, cdc_read(x->rsc, irq_offset) | (1u << 2));
    }

    for (unsigned i = 0; i < TB710FU_CDC_PHY_RAIL_COUNT && ok; ++i) {
        struct tb710fu_cdc_rpmh_resource resource;
        const struct tb710fu_cdc_board_rail *rail = &tb710fu_cdc_phy_rails[i];
        cdc_mark(rail->resource);
        cdc_checkpoint(rail->resource);
        if (tb710fu_cdc_cmd_db_find(db, TB710FU_USB_CMD_DB_BYTES,
                                    rail->resource, &resource) != TB710FU_CDC_RPMH_OK) {
            ok = 0;
            break;
        }
        const uint32_t mode_address = resource.address + 8u;
        const uint32_t voltage_address = resource.address;
        const uint32_t enable_address = resource.address + 4u;
        const uint32_t mode = 7u;
        const uint32_t voltage = rail->millivolts;
        const uint32_t enable = 1u;
        ok = cdc_rpmh_issue(x, &layout, &mode_address, &mode, 1u, "rpmh-mode") &&
             cdc_rpmh_issue(x, &layout, &voltage_address, &voltage, 1u, "rpmh-voltage") &&
             cdc_rpmh_issue(x, &layout, &enable_address, &enable, 1u, "rpmh-enable");
        if (ok) gReport.power_flags |= 1u << i;
    }

    // The TCSR USB2 reference is sourced from the board XO. Match Linux's
    // rpmh XO vote when that ARC is present, while keeping older firmware
    // command databases usable if the vote is already held by the loader.
    struct tb710fu_cdc_rpmh_resource xo;
    if (ok && tb710fu_cdc_cmd_db_find(db, TB710FU_USB_CMD_DB_BYTES,
                                      "xo.lvl", &xo) == TB710FU_CDC_RPMH_OK) {
        const uint32_t address = xo.address;
        const uint32_t value = 3u;
        ok = cdc_rpmh_issue(x, &layout, &address, &value, 1u, "xo-lvl");
        if (ok) gReport.power_flags |= 1u << 4;
    }
    // Linux also raises usb-ddr/apps-usb interconnect paths. This adapter
    // uses the same command-db BCM resources directly until an XNU
    // interconnect client is available. USB2 HS requests 240/700 MB/s.
    if (ok && !cdc_bcm_vote(x, &layout, db, "CN0", 0u, 40000u,
                            4u, 1u, 1, 1u)) ok = 0;
    if (ok && !cdc_bcm_vote(x, &layout, db, "SN2", 240000u, 700000u,
                            16u, 1u, 0, 2u)) ok = 0;
    if (ok && !cdc_bcm_vote(x, &layout, db, "ACV", 0u, 700000u,
                            4u, 4u, 1, 4u)) ok = 0;
    if (ok && !cdc_bcm_vote(x, &layout, db, "MC0", 240000u, 700000u,
                            4u, 4u, 0, 8u)) ok = 0;
    if (tcs_locked) cdc_tcs_unlock();
    IOFreeData(db, TB710FU_USB_CMD_DB_BYTES);
    if (!ok) {
        cdc_refuse("rpmh-vote");
        return 0;
    }
    return 1;
}

static uint32_t cdc_spmi_command(unsigned operation, unsigned address)
{
    return (operation << 27) | (address << 4);
}

static int cdc_spmi_wait(vm_offset_t registers)
{
    for (unsigned attempt = 0; attempt < 1000u; ++attempt) {
        const uint32_t status = cdc_read(registers, 8u);
        if (status & 1u) return !(status & 0xeu);
        cdc_delay(NULL, 10u);
    }
    return 0;
}

static int cdc_spmi_write(vm_offset_t channels, unsigned apid,
                          uint8_t address, uint8_t value)
{
    const vm_offset_t channel = channels +
        TB710FU_USB_SPMI_CHANNEL_STRIDE * apid;
    cdc_write(channel, 16u, value);
    cdc_write(channel, 0, cdc_spmi_command(0u, address));
    return cdc_spmi_wait(channel);
}

static int cdc_spmi_read(vm_offset_t observers, unsigned apid,
                         uint8_t address, uint8_t *value)
{
    const vm_offset_t observer = observers +
        TB710FU_USB_SPMI_OBSERVER_STRIDE * apid;
    cdc_write(observer, 0, cdc_spmi_command(1u, address));
    if (!cdc_spmi_wait(observer)) return 0;
    *value = (uint8_t)cdc_read(observer, 24u);
    return 1;
}

static int cdc_pm8550b_repeater_start(void)
{
    cdc_step("spmi-map", "spmi-map");
    const vm_offset_t core = ml_io_map(TB710FU_USB_SPMI_CORE_BASE,
                                       TB710FU_USB_SPMI_CORE_BYTES);
    const vm_offset_t config = ml_io_map(TB710FU_USB_SPMI_CONFIG_BASE,
                                         TB710FU_USB_SPMI_CONFIG_BYTES);
    if (!core || !config) {
        cdc_refuse("spmi-map");
        return 0;
    }

    cdc_mark("spmi-apid");
    cdc_checkpoint("spmi-apid");
    const unsigned count = cdc_read(core, 4u) & 0x7ffu;
    if (!count || count > 1024u || count > TB710FU_USB_SPMI_CORE_BYTES / 4u) {
        cdc_refuse("spmi-count");
        return 0;
    }
    int apid = -1;
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t mapping = cdc_read(core, 0x2000u + 4u * i);
        const unsigned owner = cdc_read(config, 4u * i) & 7u;
        if (((mapping >> 8) & 0xfffu) != TB710FU_CDC_SPMI_REPEATER_PPID ||
            owner != TB710FU_CDC_SPMI_EE) continue;
        if (apid >= 0) {
            cdc_refuse("spmi-ambig");
            return 0;
        }
        apid = (int)i;
    }
    if (apid < 0) {
        cdc_refuse("spmi-owner");
        return 0;
    }
    gReport.spmi_apid = (unsigned)apid;

    const size_t observer_bytes = TB710FU_USB_SPMI_OBSERVER_STRIDE *
                                  ((size_t)apid + 1u);
    const size_t channel_bytes = TB710FU_USB_SPMI_CHANNEL_STRIDE *
                                 ((size_t)apid + 1u);
    const vm_offset_t observers = ml_io_map(TB710FU_USB_SPMI_OBSERVER_BASE,
                                             observer_bytes);
    const vm_offset_t channels = ml_io_map(TB710FU_USB_SPMI_CHANNEL_BASE,
                                            channel_bytes);
    if (!observers || !channels) {
        cdc_refuse("spmi-data-map");
        return 0;
    }

    cdc_mark("rptr-enable");
    cdc_checkpoint("rptr-en");
    if (!cdc_spmi_write(channels, (unsigned)apid,
                        TB710FU_CDC_REPEATER_ENABLE, 0x80u) ||
        !cdc_spmi_write(channels, (unsigned)apid,
                        TB710FU_CDC_REPEATER_TUNE_CURRENT, 0x08u) ||
        !cdc_spmi_write(channels, (unsigned)apid,
                        TB710FU_CDC_REPEATER_TUNE_SQUELCH, 0x03u) ||
        !cdc_spmi_write(channels, (unsigned)apid,
                        TB710FU_CDC_REPEATER_TUNE_PREEMPHASIS, 0x05u) ||
        !cdc_spmi_write(channels, (unsigned)apid, 0xe8u, 0) ||
        !cdc_spmi_write(channels, (unsigned)apid, 0xedu, 0)) {
        cdc_refuse("spmi-write");
        return 0;
    }

    for (unsigned attempt = 0; attempt < 100u; ++attempt) {
        uint8_t status = 0;
        cdc_mark("rptr-ready");
        cdc_checkpoint("rptr-rd");
        if (!cdc_spmi_read(observers, (unsigned)apid,
                           TB710FU_CDC_REPEATER_STATUS, &status)) {
            cdc_refuse("spmi-read");
            return 0;
        }
        gReport.repeater_status = status;
        if (status & TB710FU_CDC_REPEATER_READY) return 1;
        cdc_delay(NULL, 100u);
    }
    cdc_refuse("repeater-ready");
    return 0;
}

static uint32_t cdc_phy_read(vm_offset_t base, unsigned offset, const char *stage)
{
    // MMIO reads have no software timeout. Publish the exact next access first.
    cdc_mark(stage);
    cdc_checkpoint(stage);
    return cdc_read(base, offset);
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

static void cdc_smmu_write(void *context, unsigned offset, uint32_t value)
{
    cdc_write(((struct tb710fu_cdc_xnu *)context)->smmu, offset, value);
}

static void cdc_smmu_barrier(void *context)
{
    (void)context;
    __asm__ volatile("dsb sy" ::: "memory");
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

static void cdc_phy_write_mask(struct tb710fu_cdc_xnu *x,
                               const struct tb710fu_cdc_masked_write *write,
                               const char *stage)
{
    cdc_mark(stage);
    cdc_checkpoint(stage);
    const uint32_t old_value = cdc_read(x->phy, write->offset);
    const uint32_t new_value = tb710fu_cdc_masked_value(
        old_value, write->mask, write->value);
    cdc_write(x->phy, write->offset, new_value);
    (void)cdc_read(x->phy, write->offset);
}

static int cdc_phy_initialize(struct tb710fu_cdc_xnu *x)
{
    static const char *const write_stages[TB710FU_CDC_PHY_INIT_COUNT] = {
        "phy-w00", "phy-w01", "phy-w02", "phy-w03", "phy-w04", "phy-w05",
        "phy-w06", "phy-w07", "phy-w08", "phy-w09", "phy-w10", "phy-w11",
        "phy-w12", "phy-w13", "phy-w14", "phy-w15", "phy-w16", "phy-w17",
        "phy-w18", "phy-w19", "phy-w20", "phy-w21", "phy-w22", "phy-w23",
        "phy-w24", "phy-w25", "phy-w26", "phy-w27"
    };
    _Static_assert(TB710FU_CDC_PHY_INIT_COUNT ==
                   sizeof(write_stages) / sizeof(write_stages[0]),
                   "PHY stage labels must match the programming table");

    cdc_mark("phy-xo");
    cdc_checkpoint("phy-xo");
    uint32_t reference = cdc_read(x->reference, TB710FU_USB_PHY_XO);
    if (!(reference & 1u)) {
        cdc_write(x->reference, TB710FU_USB_PHY_XO, reference | 1u);
        cdc_delay(NULL, 10u);
    }
    if (!(cdc_read(x->reference, TB710FU_USB_PHY_XO) & 1u)) {
        cdc_refuse("phy-xo");
        return 0;
    }

    cdc_mark("phy-rset");
    cdc_checkpoint("phy-rset");
    const uint32_t reset = cdc_read(x->phy_reset, 0);
    cdc_write(x->phy_reset, 0, reset | 1u);
    cdc_delay(NULL, 125u);
    cdc_mark("phy-rclr");
    cdc_checkpoint("phy-rclr");
    cdc_write(x->phy_reset, 0, cdc_read(x->phy_reset, 0) & ~1u);
    cdc_delay(NULL, 10u);

    for (unsigned i = 0; i < TB710FU_CDC_PHY_INIT_COUNT; ++i) {
        cdc_phy_write_mask(x, &tb710fu_cdc_phy_init[i], write_stages[i]);
    }
    cdc_mark("phy-settle");
    cdc_checkpoint("phy-settle");
    cdc_delay(NULL, 2000u);
    return 1;
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
        else __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
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
#define CDC_GCC_MASTER_SRC 0x2cu // usb30_prim_master_clk_src
#define CDC_GCC_MASTER 0x18u       // usb30_prim_master_clk
#define CDC_GCC_MOCK_UTMI_SRC 0x44u // usb30_prim_mock_utmi_clk_src
#define CDC_GCC_SLEEP 0x24u        // usb30_prim_sleep_clk
#define CDC_GCC_MOCK_UTMI 0x28u    // usb30_prim_mock_utmi_clk
#define CDC_GCC_CFG_NOC 0x88u      // cfg_noc_usb3_prim_axi_clk, halt voted
#define CDC_GCC_AGGRE_NOC 0x8cu    // aggre_usb3_prim_axi_clk, halt voted

static int cdc_branch_on(struct tb710fu_cdc_xnu *x, unsigned offset,
                         int hardware_gated)
{
    uint32_t value = cdc_read(x->gcc, offset);
    if (tb710fu_cdc_branch_active(value, hardware_gated)) return 1;
    // Linux's branch gate updates only BIT(0). Keep the NOC HWCG vote.
    if (!(value & 1u)) cdc_write(x->gcc, offset, value | 1u);
    for (unsigned i = 0; i < 50; ++i) {
        value = cdc_read(x->gcc, offset);
        if (tb710fu_cdc_branch_active(value, hardware_gated)) return 1;
        cdc_delay(NULL, 10);
    }
    return 0;
}

static int cdc_rcg_set(struct tb710fu_cdc_xnu *x, unsigned offset,
                       uint32_t configuration, const char *stage)
{
    cdc_mark(stage);
    cdc_checkpoint(stage);
    uint32_t current = cdc_read(x->gcc, offset + 4u);
    if ((current & 0x71fu) == configuration) return 1;
    cdc_write(x->gcc, offset + 4u,
              (current & ~0x71fu) | (configuration & 0x71fu));
    cdc_write(x->gcc, offset, cdc_read(x->gcc, offset) | 1u);
    for (unsigned attempt = 0; attempt < 1000u; ++attempt) {
        if (!(cdc_read(x->gcc, offset) & 1u)) return 1;
        cdc_delay(NULL, 10u);
    }
    return 0;
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
    if (!cdc_rcg_set(x, CDC_GCC_MASTER_SRC, (1u << 8) | 5u, "gcc-master") ||
        !cdc_rcg_set(x, CDC_GCC_MOCK_UTMI_SRC, 0, "gcc-utmi")) {
        cdc_refuse("gcc-rate");
        return 0;
    }
    cdc_step("gcc-branches", "cdcclks");
    if (!cdc_branch_on(x, CDC_GCC_MASTER, 0) ||
        !cdc_branch_on(x, CDC_GCC_SLEEP, 0) ||
        !cdc_branch_on(x, CDC_GCC_MOCK_UTMI, 0) ||
        !cdc_branch_on(x, CDC_GCC_CFG_NOC, 1) ||
        !cdc_branch_on(x, CDC_GCC_AGGRE_NOC, 1)) {
        cdc_refuse("gcc-branch");
        return 0;
    }

    // sm8650.dtsi names TCSR_USB3_CLKREF_EN as the wrapper's "xo".
    cdc_step("wrapper-xo", "cdcxoref");
    uint32_t xo = cdc_read(x->reference, TB710FU_USB_WRAPPER_XO);
    if (!(xo & 1u)) {
        cdc_write(x->reference, TB710FU_USB_WRAPPER_XO, xo | 1u);
        cdc_delay(NULL, 10);  // Linux BRANCH_HALT_DELAY.
    }
    if (!(cdc_read(x->reference, TB710FU_USB_WRAPPER_XO) & 1u)) {
        cdc_refuse("wrapper-xo");
        return 0;
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
    gReport.phy[0] = cdc_phy_read(x->phy_reset, 0, "phy-rst");
    gReport.phy[1] = cdc_phy_read(x->reference, TB710FU_USB_PHY_XO, "phy-ref");
    if (!tb710fu_cdc_phy_clocked(gReport.phy[0], gReport.phy[1])) {
        cdc_refuse("phy-gates");
        return 0;
    }
    struct tb710fu_cdc_phy_state phy = {
        .reset = gReport.phy[0],
        .reference = gReport.phy[1],
        .control0 = 0,
        .control5 = 0,
        .common0 = 0,
        .control2 = 0
    };
    phy.control0 = gReport.phy[2] = cdc_phy_read(x->phy, 0x3c, "phy-u0");
    phy.control5 = gReport.phy[3] = cdc_phy_read(x->phy, 0x50, "phy-u5");
    phy.common0 = gReport.phy[4] = cdc_phy_read(x->phy, 0x54, "phy-c0");
    phy.control2 = gReport.phy[5] = cdc_phy_read(x->phy, 0x64, "phy-h2");
    if (!tb710fu_cdc_phy_adoptable(&phy)) {
        cdc_refuse("phy");
        return 0;
    }
    return 1;
}

// The USB30 block reset in GCC was tried here as the reset firmware cannot
// shadow, and it was withdrawn again: both boots that carried it stopped right
// after the next checkpoint inside the stream route - once after the attach
// returned, once after the read-only rescan - while every boot without it went
// on to dwc3-start. That is one variable too expensive to keep while the
// controller path itself is still unmeasured, so the pulse is gone until the
// core has reported whether it accepts configuration at all.

// The eight-character bottom band cannot carry a sentence, so every refusal
// class gets its own short tag; "sid40" remains only as an unreachable default.
// The masked-group outcome is split further, because the device now proved that
// a catch-all group is what the boot chain leaves behind.
static const char *cdc_smmu_tag(enum tb710fu_cdc_smmu_result result)
{
    switch (result) {
    case TB710FU_CDC_SMMU_OK: return "sidok";
    case TB710FU_CDC_SMMU_ABSENT: return "sidnone";
    case TB710FU_CDC_SMMU_FAULTED: return "sidflt";
    case TB710FU_CDC_SMMU_AMBIGUOUS: return "sidamb";
    case TB710FU_CDC_SMMU_TRANSLATES: return "sidxl";
    case TB710FU_CDC_SMMU_GEOMETRY: return "sidgeom";
    case TB710FU_CDC_SMMU_FULL: return "sidfull";
    }
    return "sid40";
}

static int cdc_route(struct tb710fu_cdc_xnu *x,
                     struct tb710fu_cdc_identity_route *route)
{
    cdc_step("smmu-route", "cdcsmmu");
    enum tb710fu_cdc_smmu_result result =
        tb710fu_cdc_identity_route_scan(cdc_smmu_read, x, route);
    gReport.scr0 = cdc_read(x->smmu, 0);
    gReport.id0 = cdc_read(x->smmu, 0x20);
    gReport.streams = route->matching_streams;
    gReport.smmu_slot = route->slot;
    gReport.smmu_bank = route->bank;
    gReport.smmu_smr = route->smr;
    gReport.smmu_s2cr = route->s2cr;
    gReport.smmu_cbar = route->cbar;
    gReport.smmu_cba2r = route->cba2r;
    gReport.smmu_sctlr = route->sctlr;
    gReport.smmu_masked = route->masked;
    if (result == TB710FU_CDC_SMMU_OK) return 1;
    // Once the route is open, the stream table has already refused a bypass
    // publication and every action that would test it again has been measured.
    // Re-walking the groups or re-probing writes the same registers a second
    // time for no new information, so a later verification stops at the
    // read-only scan that just ran.
    if (gReport.smmu_open) {
        cdc_checkpoint("sidkeep");
        return 1;
    }
    // An absent or already-faulted group is exactly the state the boot chain
    // leaves for a SID it does not own; claim it, then require the same scan to
    // attest the route this adapter will rely on.
    if (result == TB710FU_CDC_SMMU_ABSENT || result == TB710FU_CDC_SMMU_FAULTED) {
        cdc_checkpoint(result == TB710FU_CDC_SMMU_ABSENT ? "sidclaim" : "sidtake");
        struct tb710fu_cdc_smmu_saved saved;
        // Separate checkpoints bracket the walk and the probe: the frame only
        // advances on a checkpoint, so a stall inside either step leaves the
        // last label behind as the only evidence of how far the worker got.
        // Qualcomm SM8650 QHEE coerces S2CR.TYPE=BYPASS to FAULT, so try the
        // UFS-proven Stage-1 disabled VA64 context-bank route first and fall
        // back to explicit bypass only if no context bank is available.
        cdc_checkpoint("sidcbwk");
        int cb_attached = tb710fu_cdc_stream_cb_bypass_attach(
            cdc_smmu_read, cdc_smmu_write, cdc_smmu_barrier, x, route, &saved);
        int attached = cb_attached;
        if (!attached) {
            cdc_checkpoint("sidwalk");
            attached = tb710fu_cdc_stream_bypass_attach(
                cdc_smmu_read, cdc_smmu_write, cdc_smmu_barrier, x, route, &saved);
        }
        cdc_checkpoint("sidwlkd");
        if (attached) {
            // Re-scan into *route so callers comparing this route against a
            // post-DMA-alloc rescan see the newly published state rather than
            // the stale pre-claim ABSENT snapshot.
            if (tb710fu_cdc_identity_route_scan(cdc_smmu_read, x, route) !=
                TB710FU_CDC_SMMU_OK) {
                cdc_refuse("sidver");
                return 0;
            }
            gReport.streams = route->matching_streams;
            gReport.smmu_smr = route->smr;
            gReport.smmu_s2cr = route->s2cr;
            gReport.smmu_cbar = route->cbar;
            gReport.smmu_cba2r = route->cba2r;
            gReport.smmu_sctlr = route->sctlr;
            gReport.smmu_saved_smr = saved.smr;
            gReport.smmu_saved_s2cr = saved.s2cr;
            gReport.smmu_slot = saved.slot;
            gReport.smmu_bank = saved.bank;
            gReport.smmu_tries = saved.tried;
            gReport.smmu_bank_tries = saved.bank_tries;
            gReport.smmu_probe_smr = saved.probe_smr;
            gReport.smmu_probe_s2cr = saved.probe_s2cr;
            gReport.smmu_claimed = 1;
            // A publication the first free group dropped but a later one kept
            // means the bottom groups are not uniformly writable.
            cdc_checkpoint(cb_attached ? "sidcb" : saved.slot ? "sidmove" : "sidpin");
            return 1;
        }
        gReport.smmu_slot = saved.slot;
        gReport.smmu_bank = saved.bank;
        gReport.smmu_tries = saved.tried;
        gReport.smmu_bank_tries = saved.bank_tries;
        gReport.smmu_saved_smr = saved.smr;
        gReport.smmu_saved_s2cr = saved.s2cr;
        gReport.smmu_probe_smr = saved.probe_smr;
        gReport.smmu_probe_s2cr = saved.probe_s2cr;
        struct tb710fu_cdc_smmu_actions actions;
        cdc_checkpoint("sidprob");
        tb710fu_cdc_smmu_probe_actions(cdc_smmu_read, cdc_smmu_write,
                                       cdc_smmu_barrier, x, saved.slot, &saved,
                                       &actions);
        cdc_checkpoint("sidprbd");
        gReport.smmu_actions[0] = actions.keep;
        gReport.smmu_actions[1] = actions.fault;
        gReport.smmu_actions[2] = actions.bypass;
        gReport.smmu_actions[3] = actions.smr;
        gReport.smmu_acr = cdc_read(x->smmu, 0x18);
        gReport.smmu_gfsr = cdc_read(x->smmu, 0x58);
        gReport.smmu_scr0 = cdc_read(x->smmu, 0);
        // The console must outlive a refused stream route: DWC3 register
        // accesses are CPU cycles, not streams, so the controller still has to
        // be driven and observed. Only the DMA traffic can fault, and the lanes
        // will say so. The refusal is kept as a checkpoint, not as the run's
        // failure, so later stages report themselves honestly.
        gReport.smmu_open = 1;
        gReport.failure = 0;
        cdc_checkpoint("sidopen");
        return 1;
    }
    cdc_refuse(cdc_smmu_tag(result));
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
    // Follow the device Linux order for the PHY, then initialize DWC3 clocks.
    if (!cdc_power_rails(x)) return 0;
    if (!cdc_pm8550b_repeater_start()) return 0;
    if (!cdc_gcc_bring_up(x)) return 0;
    if (!cdc_phy_initialize(x)) return 0;
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
    // The route rescan above is the last table access before this check, and two
    // consecutive boots stopped with no output right after a checkpoint inside
    // it. Tag the check itself so the band says whether the bus died in the
    // stream table or in the controller registers.
    cdc_lane_op(NULL, "ownchk");
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
        .smmu_verified = gReport.smmu_open ? 0 : 1,
        .smmu_identity = gReport.smmu_open ? 0 : 1,
        .smmu_stream_id = TB710FU_CDC_DWC3_STREAM_ID,
        .matching_streams = (uint16_t)route.matching_streams,
        .dma_contiguous = (uint8_t)contiguous,
        .dma_address_bits = (uint8_t)address_bits,
        .dma_physical = physical, .dma_bytes = TB710FU_DWC3_DMA_SIZE
    };
    cdc_mark("dma-preflight");
    cdc_checkpoint("dmapre");
    // With the stream route open, the guard's own verdict is already known:
    // the run continues only to find out whether the stream table gates the
    // controller, so the refusal is recorded instead of ending the attempt.
    if (!gReport.smmu_open) {
        enum tb710fu_cdc_preflight_result preflight =
            tb710fu_cdc_preflight(1, &snapshot);
        if (preflight != TB710FU_CDC_PREFLIGHT_READY) {
            cdc_refuse("preflight");
            return 0;
        }
    }
    // Linux drivers/usb/dwc3/dwc3-qcom.c: dwc3_qcom_select_utmi_clk().
    // USB2-only bring-up does not power the SuperSpeed QMP PHY, so DWC3 has no
    // PIPE3 clock until QSCRATCH_GENERAL_CFG switches PIPE to UTMI. Without it
    // DCTL.CSFTRST cannot finish its clock-domain reset handshake.
    cdc_step("qscratch-utmi", "cdcqutmi");
    const uint32_t gen0 = cdc_read(x->qscratch, 0x08);
    cdc_write(x->qscratch, 0x08, gen0 | (1u << 8));
    cdc_delay(NULL, 200u);
    cdc_write(x->qscratch, 0x08, cdc_read(x->qscratch, 0x08) | (1u << 0) | (1u << 3));
    cdc_delay(NULL, 200u);
    cdc_write(x->qscratch, 0x08, cdc_read(x->qscratch, 0x08) & ~(1u << 8));
    cdc_delay(NULL, 200u);
    const uint32_t gen1 = cdc_read(x->qscratch, 0x08);
    gReport.qscratch_gen[0] = gen0;
    gReport.qscratch_gen[1] = gen1;
    // Linux dwc3_qcom_vbus_override_enable defines these wrapper session bits.
    // USB2-only operation does not enable the SuperSpeed lane power override.
    cdc_step("vbus", "cdcvbus");
    const uint32_t hs0 = cdc_read(x->qscratch, 0x10);
    cdc_write(x->qscratch, 0x10, hs0 | (1u << 20) | (1u << 28));
    const uint32_t hs1 = cdc_read(x->qscratch, 0x10);
    gReport.qscratch_hs[0] = hs0;
    gReport.qscratch_hs[1] = hs1;
    cdc_log("TB710FU CDC: qscratch gen=%08x->%08x hs=%08x->%08x\n",
            gen0, gen1, hs0, hs1);
    if ((hs1 & ((1u << 20) | (1u << 28))) != ((1u << 20) | (1u << 28))) {
        cdc_refuse("vbus");
        return 0;
    }
    struct tb710fu_dwc3_ops ops = {
        .context = x, .read32 = cdc_dwc_read, .write32 = cdc_dwc_write,
        .delay_us = cdc_delay, .sync_for_cpu = cdc_sync_cpu,
        .sync_for_device = cdc_sync_device, .log_copy = cdc_copy,
        .log_consume = cdc_consume, .lane = cdc_lane_op
    };
    // Development identity, deliberately synthetic and not a vendor serial.
    // The controller's own report prints only after tb710fu_dwc3_start returns,
    // so a boot that stops inside that routine leaves no register evidence at
    // all - the last frame ended at "dwc3-start" with nothing after it. Read the
    // same registers from this side first, one frame earlier, and read
    // GUSB3PIPECTL before any halt or reset write so the PIPE-control access is
    // proven safe on its own rather than inside the sequence that wedges.
    cdc_step("dwc-pre", "dwcpre");
    const uint32_t pectl = cdc_read(x->dwc, 0xc2c0);
    const uint32_t dctl0 = cdc_read(x->dwc, 0xc704);
    const uint32_t gctl0 = cdc_read(x->dwc, 0xc110);
    const uint32_t pcfg0 = cdc_read(x->dwc, 0xc200);
    const uint32_t gsts0 = cdc_read(x->dwc, 0xc118);
    const uint32_t dsts0 = cdc_read(x->dwc, 0xc70c);
    cdc_log("TB710FU CDC: dwc pre pectl=%08x dctl=%08x gctl=%08x pcfg=%08x "
           "gsts=%08x dsts=%08x\n", pectl, dctl0, gctl0, pcfg0, gsts0, dsts0);
    cdc_step("dwc3-start", "cdcdwc");
    const int started = tb710fu_dwc3_start(&x->controller, &ops, (uint8_t *)x->dma,
                                          physical, 0x1209, 0x0001, "TB710FUCDC001");
    // The core reset is the first place this board disagrees with a freshly
    // powered Linux core, so keep its whole trace on the frame regardless of
    // whether the adoption succeeded.
    cdc_log("TB710FU CDC: dwc reset stage=%u adopt=%u dctl=%08x/%08x gctl=%08x "
           "pcfg=%08x gsts=%08x dsts=%08x\n",
           x->controller.reset_stage, x->controller.reset_adopted,
           x->controller.reset_trace[0], x->controller.reset_trace[1],
           x->controller.reset_trace[2], x->controller.reset_trace[3],
           x->controller.reset_trace[4], x->controller.reset_trace[5]);
    // The status bit above only says what the core thinks of itself. These are
    // the readbacks of the writes actually issued after it, so one frame
    // separates "core deaf to reset but alive" from "core not clocked at all".
    cdc_log("TB710FU CDC: dwc probe forced=%u dcfg=%08x devten=%08x evt=%08x/%08x "
           "dsts=%08x pectl=%08x\n",
           x->controller.reset_forced, x->controller.probe_trace[0],
           x->controller.probe_trace[1], x->controller.probe_trace[2],
           x->controller.probe_trace[4], x->controller.probe_trace[3],
           x->controller.probe_trace[5]);
    if (!started) {
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
    /* Entry marker before anything that can take a lock or a guard. The adapter
     * putc is capture-only and returns on contention, so these bytes prove the
     * new thread was scheduled and reached user code. A missing marker means the
     * thread never ran, which is a scheduler/interrupt problem, not a CDC one. */
    if (tb710fu_live_log_putc) {
        const char entry[] = "\nCDC-ENTRY\n";
        for (size_t index = 0; index < sizeof(entry) - 1u; ++index)
            tb710fu_live_log_putc(entry[index]);
    }
    uint64_t timer_ctl;
    __asm__ volatile("mrs %0, cntv_ctl_el0" : "=r"(timer_ctl));
    cdc_log("TB710FU CDC: worker gic=%u timer=%llu cntv_ctl=%llx irq=%u\n",
           armcpu_gic_ready, (unsigned long long)armcpu_timer_interrupts,
           (unsigned long long)timer_ctl, (unsigned)ml_get_interrupts_enabled());
    cdc_checkpoint("cdcentry");
    cdc_log("TB710FU CDC: image %s\n", TB710FU_CDC_IMAGE);
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
    cdc_log("TB710FU CDC: power rails=%02x repeater=%02x spmi-apid=%u\n",
            gReport.power_flags, gReport.repeater_status, gReport.spmi_apid);
    cdc_log("TB710FU CDC: bcm flags=%02x cn0=%08x sn2=%08x acv=%08x mc0=%08x\n",
            gReport.bcm_flags, gReport.bcm_values[0], gReport.bcm_values[1],
            gReport.bcm_values[2], gReport.bcm_values[3]);
    cdc_log("TB710FU CDC: dwc id=%08x gctl=%08x role=%u bits=%u "
           "smmu scr0=%08x id0=%08x streams=%u\n",
           gReport.id, gReport.gctl, gReport.role, gReport.dma_bits,
           gReport.scr0, gReport.id0, gReport.streams);
    cdc_log("TB710FU CDC: smmu claimed=%u slot=%u bank=%u masked=%u tries=%u/%u open=%u "
           "smr=%08x s2cr=%08x cbar=%08x cba2r=%08x sctlr=%08x\n",
           gReport.smmu_claimed, gReport.smmu_slot, gReport.smmu_bank,
           gReport.smmu_masked, gReport.smmu_tries, gReport.smmu_bank_tries,
           gReport.smmu_open, gReport.smmu_smr, gReport.smmu_s2cr,
           gReport.smmu_cbar, gReport.smmu_cba2r, gReport.smmu_sctlr);
    cdc_log("TB710FU CDC: smmu probe slot=%u pre_smr=%08x pre_s2cr=%08x "
           "keep=%08x fault=%08x bypass=%08x smr=%08x acr=%08x gfsr=%08x scr0=%08x\n",
           gReport.smmu_slot, gReport.smmu_saved_smr, gReport.smmu_saved_s2cr,
           gReport.smmu_actions[0], gReport.smmu_actions[1],
           gReport.smmu_actions[2], gReport.smmu_actions[3],
           gReport.smmu_acr, gReport.smmu_gfsr, gReport.smmu_scr0);
    if (ready) {
        /* For the first time the controller returned with no failure: every
         * command issued after the ignored reset was acknowledged. What that
         * leaves is the event ring, whose base has read back as zero on every
         * boot so far. Read the live copy of every register that decides whether
         * an event can arrive at all, and if the base did not stick, drop a
         * sentinel into the same address to separate "register not writable"
         * from "value dropped", then put the real base back. That late re-arm,
         * made while the core is actually running, has never been tried. */
        cdc_step("dwc-state", "dwcstate");
        const uint32_t evtbase = (uint32_t)gReport.dma_physical;
        uint32_t evtlo_now = cdc_read(gCdc.dwc, 0xc400);
        const uint32_t evthi_now = cdc_read(gCdc.dwc, 0xc404);
        const uint32_t evtsz_now = cdc_read(gCdc.dwc, 0xc408);
        const uint32_t evtcnt_now = cdc_read(gCdc.dwc, 0xc40c);
        cdc_log("TB710FU CDC: dwc state dctl=%08x dsts=%08x devten=%08x "
               "dalepn=%08x\n",
               cdc_read(gCdc.dwc, 0xc704), cdc_read(gCdc.dwc, 0xc70c),
               cdc_read(gCdc.dwc, 0xc708), cdc_read(gCdc.dwc, 0xc720));
        cdc_log("TB710FU CDC: dwc evt want=%08x lo=%08x hi=%08x sz=%08x "
               "cnt=%08x\n", evtbase, evtlo_now, evthi_now, evtsz_now, evtcnt_now);
        if (evtlo_now != evtbase ||
            evthi_now != (uint32_t)(gReport.dma_physical >> 32)) {
            cdc_write(gCdc.dwc, 0xc400, evtbase);
            cdc_write(gCdc.dwc, 0xc404, (uint32_t)(gReport.dma_physical >> 32));
            cdc_write(gCdc.dwc, 0xc408, TB710FU_DWC3_EVENT_BYTES | (1u << 31));
            cdc_write(gCdc.dwc, 0xc40c, 0);
            evtlo_now = cdc_read(gCdc.dwc, 0xc400);
            cdc_log("TB710FU CDC: dwc evt rearm lo=%08x hi=%08x sz=%08x\n",
                   evtlo_now, cdc_read(gCdc.dwc, 0xc404), cdc_read(gCdc.dwc, 0xc408));
        }
        cdc_checkpoint("irqroute");
        uint32_t spi_state = 0;
        kern_return_t spi = armcpu_spi_attach(TB710FU_USB_DWC3_INTID, false,
                                              cdc_irq, NULL, &spi_state, NULL);
        const int event_irq = spi == KERN_SUCCESS && (spi_state & 7u) == 3u;
        if (spi == KERN_SUCCESS) {
            // Keep the GIC line masked and poll every 100 us directly on this
            // core: with smp=0, calling thread_block() on round 1 handed the
            // single CPU to a BSD thread that never yielded back (observed on
            // pl4, where neither "beat" nor "timer quiet" ever ran).
            (void)armcpu_spi_mask(TB710FU_USB_DWC3_INTID, true);
        }
        cdc_log("TB710FU CDC: event irq=%u kr=%d gic=%08x guctl1=%08x\n",
               (unsigned)event_irq, spi, spi_state, cdc_read(gCdc.dwc, 0xc11c));
        cdc_log("TB710FU CDC: platform up dma=%llx/%x, waiting for enumeration "
               "and host DTR\n",
               (unsigned long long)gReport.dma_physical, (unsigned)gReport.dma_bytes);
        cdc_checkpoint("cdcready");
        // On pl5 (smp=0), beat #1 ran at 50 ms with ev=1 (USBRST) and High-Speed
        // chirp (dsts=000e0000), then the worker was preempted away to the hung
        // BSD p0a_01/e thread before beat #2 (100 ms), causing Windows to fail
        // GET_DESCRIPTOR(DEVICE) with Code 43. Mask IRQs while polling DWC3 via
        // CNTVCT_EL0 so this core stays dedicated to USB enumeration and log TX.
        const boolean_t prev_irq = ml_set_interrupts_enabled(FALSE);
        unsigned previous = 0;
        int bus_seen = 0, setup_seen = 0;
        unsigned rounds = 0, beats = 0;
        uint32_t last_dsts_link = cdc_read(gCdc.dwc, 0xc70c) & 0x007fffffu;
        uint64_t last_events = 0;
        while (gCdc.controller.running && !gCdc.controller.failed) {
            tb710fu_dwc3_poll(&gCdc.controller);
            ++rounds;
            const uint32_t dsts_now = cdc_read(gCdc.dwc, 0xc70c);
            const uint32_t dsts_link = dsts_now & 0x007fffffu;
            const int early_beat = (rounds == 1000u || rounds == 3000u || rounds == 6000u);
            const int periodic_beat = ((rounds % 10000u) == 0u && beats < 12u);
            const int state_changed = (dsts_link != last_dsts_link ||
                                       gCdc.controller.events != last_events);
            if (early_beat || periodic_beat ||
                (state_changed && (rounds % 500u) == 0u)) {
                last_dsts_link = dsts_link;
                last_events = gCdc.controller.events;
                ++beats;
                cdc_log("TB710FU CDC: beat #%u r=%u dsts=%08x cnt=%08x ev=%llu "
                       "last=%08x req=%llu conn=%u cfg=%u dtr=%u\n",
                       beats, rounds / 10u, dsts_now, cdc_read(gCdc.dwc, 0xc40c),
                       (unsigned long long)gCdc.controller.events,
                       gCdc.controller.last_event,
                       (unsigned long long)gCdc.controller.setup_requests,
                       (unsigned)gCdc.controller.connected,
                       (unsigned)gCdc.controller.configured,
                       (unsigned)gCdc.controller.dtr);
                if (!gCdc.controller.dtr) {
                    cdc_checkpoint(gCdc.controller.configured ? "cdccfg" :
                                   gCdc.controller.setup_requests ? "cdcsetup" :
                                   gCdc.controller.connected ? "cdcbus" : "cdcbeat");
                }
            }
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
            cdc_delay(NULL, 100u);
        }
        (void)ml_set_interrupts_enabled(prev_irq);
        cdc_checkpoint("cdcstop");
        cdc_log("TB710FU CDC: transport stopped: %s (ev=%llu last=%08x req=%llu)\n",
               gCdc.controller.failure ? gCdc.controller.failure : "stopped",
               (unsigned long long)gCdc.controller.events,
               gCdc.controller.last_event,
               (unsigned long long)gCdc.controller.setup_requests);
    }
    // One last text checkpoint after the report: the QR frame is rebuilt on a
    // one second dwell, so a failure that ends the worker right after printing
    // would otherwise leave the newest lines out of every frame.
    cdc_checkpoint("cdcrept");
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
