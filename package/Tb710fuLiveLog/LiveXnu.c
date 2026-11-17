/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Native XNU adapter. Never called by the character writer or under kprintf's
 * lock. One renderer owns the immutable senders and private Mu arena at a time.
 */
#ifndef TB_LIVE_HOST_TEST
#include <kern/misc_protos.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/startup.h>
#include <kern/tb710fu_ramoops.h>
#include <kern/tb710fu_live_log.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#endif
#include "LiveProtocol.h"
#include "LiveDisplay.h"
#include "LiveQr.h"
#ifndef TB_LIVE_BUILD_LABEL
#define TB_LIVE_BUILD_LABEL "source-unbound"
#endif

static TB_LIVE_DISPLAY display;
static struct tb710fu_live_log_sender live_sender, fixed_sender;
static uint8_t snapshot[TB710FU_LIVE_LOG_MAX_BYTES], frame[752];
static uint32_t fixed_pixels[121u * 121u];
static unsigned enabled, guard, redraw, worker_started, sleep_channel, fixed_valid;
static uint64_t frequency, next_frame, generation;
static unsigned small_screen_phase;

static uint64_t live_ticks(void)
{
#ifdef TB_LIVE_HOST_TEST
    return tb_live_host_ticks();
#else
    uint64_t value;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(value));
    return value;
#endif
}

static void live_barrier(void)
{
#ifndef TB_LIVE_HOST_TEST
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

int tb710fu_live_log_active(void)
{
    return __atomic_load_n(&enabled, __ATOMIC_ACQUIRE) == 1u;
}

static void status_line(const struct tb710fu_ramoops_snapshot *state,
                        const struct tb710fu_live_log_info *info)
{
    char text[256];
    const char *label = state->state == 0 ? "unmapped" :
                        state->state == 1 ? "busy" :
                        state->state == 2 ? "empty" : "ready";
    (void)snprintf(text, sizeof(text),
        TB_LIVE_BUILD_LABEL " %s | LEFT fixed=%s | RIGHT live | gen=%llu end=%llu drop=%llu frame=%u/%u",
        label, fixed_valid ? "ready" : "pending", (unsigned long long)info->generation,
        (unsigned long long)state->end, (unsigned long long)state->dropped,
        info->next_sequence, info->block_count);
    (void)tb_live_display_status(&display, text);
}

/* Every call handles at most one live frame. There is no replay wait. */
void tb710fu_live_log_poll(void)
{
    if (!tb710fu_live_log_active()) return;
    if (__atomic_exchange_n(&guard, 1u, __ATOMIC_ACQUIRE)) return;
    uint64_t now = live_ticks();
    unsigned refresh = __atomic_exchange_n(&redraw, 0u, __ATOMIC_ACQ_REL);
    if (!refresh && now < next_frame) goto done;
    next_frame = now + frequency / 2u;
    if (refresh) (void)tb_live_display_bands(&display);
    struct tb710fu_ramoops_snapshot state = {0};
    struct tb710fu_live_log_info info = {0};
    size_t capacity = __atomic_load_n(&worker_started, __ATOMIC_ACQUIRE) ?
                      sizeof(snapshot) : 512u;
    int copied = tb710fu_ramoops_snapshot_copy(snapshot, capacity, &state);
    (void)tb710fu_live_log_get_info(&live_sender, &info);
    if (copied > 0 && tb710fu_live_log_can_snapshot(&live_sender) &&
        (!live_sender.valid || state.start != info.start || state.end != info.end)) {
        (void)tb710fu_live_log_snapshot(&live_sender, snapshot, (size_t)copied,
                                       ++generation, state.start, state.end);
    }
    if (fixed_valid && ((display.DualQr && refresh) ||
        (!display.DualQr && !(small_screen_phase++ & 1u)))) {
        (void)tb_live_display_qr_slot(&display, fixed_pixels, 121u * 121u, 121, 0);
        if (!display.DualQr) {
            (void)tb_live_display_status(&display, "RAMOOPS FIXED: immutable pre-machine_startup snapshot (up to 512 bytes)");
            goto painted;
        }
    }
    if (tb710fu_live_log_next_frame(&live_sender, frame)) {
        const uint32_t *pixels;
        if (tb_live_qr_encode(frame, sizeof(frame), &pixels))
            (void)tb_live_display_qr_slot(&display, pixels, 121u * 121u, 121, 1);
    }
    (void)tb710fu_live_log_get_info(&live_sender, &info);
    status_line(&state, &info);
painted:
    live_barrier();
done:
    __atomic_store_n(&guard, 0u, __ATOMIC_RELEASE);
}

void tb710fu_live_log_refresh(void)
{
    if (!tb710fu_live_log_active()) return;
    __atomic_store_n(&redraw, 1u, __ATOMIC_RELEASE);
    tb710fu_live_log_poll();
}

/* A single-block immutable snapshot remains independently scannable even if
 * scheduling never starts or the live ring is later overwritten by a loop. */
void tb710fu_live_log_freeze(void)
{
    if (!tb710fu_live_log_active()) return;
    if (__atomic_exchange_n(&guard, 1u, __ATOMIC_ACQUIRE)) return;
    if (!fixed_valid) {
        struct tb710fu_ramoops_snapshot state = {0};
        int copied = tb710fu_ramoops_snapshot_copy(snapshot, 512u, &state);
        if (copied > 0 && tb710fu_live_log_snapshot(&fixed_sender, snapshot,
            (size_t)copied, 0, state.start, state.end) == TB710FU_LIVE_LOG_READY &&
            fixed_sender.info.block_count == 1u &&
            tb710fu_live_log_next_frame(&fixed_sender, frame)) {
            const uint32_t *pixels;
            if (tb_live_qr_encode(frame, sizeof(frame), &pixels)) {
                for (size_t i = 0; i < 121u * 121u; i++) fixed_pixels[i] = pixels[i];
                fixed_valid = 1u;
                (void)tb_live_display_qr_slot(&display, fixed_pixels, 121u * 121u, 121, 0);
                struct tb710fu_live_log_info info = {0};
                (void)tb710fu_live_log_get_info(&live_sender, &info);
                status_line(&state, &info);
                live_barrier();
            }
        }
    }
    __atomic_store_n(&guard, 0u, __ATOMIC_RELEASE);
}

void tb710fu_live_log_init(void)
{
    int requested = 0;
    if (!PE_parse_boot_argn("tb710fu-live-log", &requested, sizeof(requested)) || requested != 1) return;
    PE_Video video = {0};
    (void)PE_current_console(&video);
    uint64_t physical = video.v_baseAddr & ~UINT64_C(3);
    /* Called once after arm_vm_init, while PE_state still contains boot PA.
     * Never reinterpret a later virtual console mapping as a physical address.
     */
    if (video.v_depth != 32 || !physical || physical >= (UINT64_C(1) << 48) ||
        video.v_width > 8192u || video.v_height > 8192u ||
        video.v_rowBytes > 65536u || !video.v_height || !video.v_rowBytes) return;
    uint64_t bytes = video.v_rowBytes * video.v_height;
    uint64_t length = video.v_length ? video.v_length : bytes;
    if (bytes > (64u << 20) || length > (64u << 20) ||
        video.v_offset > length || bytes > length - video.v_offset ||
        physical + length < physical || physical + length >= (UINT64_C(1) << 48)) return;
    vm_offset_t mapping = ml_io_map(physical, (vm_size_t)length);
    if (!mapping || !tb_live_display_bind(&display, (void *)(mapping + video.v_offset),
        (size_t)(length - video.v_offset), (uint32_t)video.v_width,
        (uint32_t)video.v_height, (uint32_t)video.v_rowBytes)) return;
    /* ml_io_map is permanent; video_console's replaceable mapping is separate.
     * Its default VM_WIMG_IO attributes match this non-Apple target. */
#ifdef TB_LIVE_HOST_TEST
    frequency = tb_live_host_frequency();
#else
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
#endif
    if (frequency < 2u) return;
    tb710fu_live_log_sender_init(&live_sender);
    tb710fu_live_log_sender_init(&fixed_sender);
    (void)tb_live_display_qr_slot(&display, fixed_pixels, 121u * 121u, 121, 0);
    if (display.DualQr)
        (void)tb_live_display_qr_slot(&display, fixed_pixels, 121u * 121u, 121, 1);
    __atomic_store_n(&enabled, 1u, __ATOMIC_RELEASE);
    tb710fu_live_log_refresh();
}

static void live_worker(void *argument, wait_result_t result)
{
    (void)argument; (void)result;
    __atomic_store_n(&worker_started, 1u, __ATOMIC_RELEASE);
    for (;;) {
        tb710fu_live_log_poll();
        uint64_t deadline;
        clock_interval_to_deadline(250, NSEC_PER_MSEC, &deadline);
        (void)assert_wait_deadline((event_t)&sleep_channel, THREAD_UNINT, deadline);
        thread_block(THREAD_CONTINUE_NULL);
    }
}

static void live_start_worker(void)
{
    if (!tb710fu_live_log_active()) return;
    thread_t worker = THREAD_NULL;
    kern_return_t result = kernel_thread_start(live_worker, NULL, &worker);
    if (result == KERN_SUCCESS) thread_deallocate(worker);
    else printf("TB710FU live log: worker creation failed %d\n", result);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_FIRST, live_start_worker);
