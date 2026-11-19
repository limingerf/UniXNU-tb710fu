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
static struct tb710fu_live_log_sender xnu_sender, ramoops_sender;
static struct tb710fu_live_log_sender checkpoint_sender;
/* Zero initialization is the capture-ring initialization. Never clear this in
 * display init: PE/console output can arrive before VM and LiveLog init. */
static struct tb710fu_live_capture xnu_capture;
static struct tb710fu_live_capture_info early_state, xnu_state;
static struct tb710fu_ramoops_snapshot ramoops_state;
static uint8_t snapshot[TB710FU_LIVE_LOG_MAX_BYTES];
static uint8_t xnu_file[TB710FU_LIVE_LOG_MAX_BYTES], checkpoint_file[TB710FU_LIVE_LOG_MAX_BYTES];
static uint8_t early_bytes[512], frame[752];
static unsigned enabled, guard, redraw, worker_started, sleep_channel, early_frozen, early_once;
static unsigned checkpoint_mode, checkpoint_guard, checkpoint_failures, checkpoint_bands_pending;
static unsigned checkpoint_file_generation;
static unsigned qr_encoded[2], qr_drawn[2];
static unsigned next_slot;
static uint64_t frequency, next_frame, poll_count, xnu_generation, ramoops_generation;
static uint64_t checkpoint_generation, checkpoint_last_attempt_end;
static uint64_t checkpoint_last_qr_attempt_end, xnu_last_dropped;
static char checkpoint_stage[49];

/* This strong writer is capture only: no framebuffer, encoding, allocation,
 * polling or logging. A contending writer records a drop and returns. */
void tb710fu_live_log_putc(char c)
{
    (void)tb710fu_live_capture_putc(&xnu_capture, c);
}

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

static const char *state_name(unsigned state)
{
    return state == 0 ? "unmapped" : state == 1 ? "busy" :
           state == 2 ? "empty" : "ready";
}

static void status_line(unsigned slot)
{
    char text[256];
    struct tb710fu_live_log_info xnu = {0}, ramoops = {0};
    (void)tb710fu_live_log_get_info(&xnu_sender, &xnu);
    (void)tb710fu_live_log_get_info(&ramoops_sender, &ramoops);
    (void)snprintf(text, sizeof(text),
        "W=%u P=%llu S=%s | X=%s:%uB %u/%u R=%s:%uB %u/%u | QR=X%u/%u R%u/%u | E=%s | %.24s",
        __atomic_load_n(&worker_started, __ATOMIC_ACQUIRE),
        (unsigned long long)poll_count, slot == 0 ? "XNU" : "RAMOOPS",
        state_name(xnu_state.state), xnu_state.copied, xnu.rounds, xnu.block_count,
        state_name(ramoops_state.state), ramoops_state.copied,
        ramoops.rounds, ramoops.block_count,
        qr_encoded[0], qr_drawn[0], qr_encoded[1], qr_drawn[1],
        __atomic_load_n(&early_frozen, __ATOMIC_ACQUIRE) ? state_name(early_state.state) : "pending",
        TB_LIVE_BUILD_LABEL);
#ifdef TB_LIVE_HOST_TEST
    tb_live_host_status(text);
#endif
    (void)tb_live_display_status(&display, text);
}

static void snapshot_xnu(void)
{
    if (!__atomic_load_n(&early_frozen, __ATOMIC_ACQUIRE)) return;
    int started = __atomic_load_n(&worker_started, __ATOMIC_ACQUIRE);
    size_t capacity = started ? 3072u : 512u;
    int copied = tb710fu_live_capture_copy(&xnu_capture, snapshot, capacity, &xnu_state);
    if (xnu_state.state != TB710FU_LIVE_CAPTURE_EMPTY &&
        xnu_state.state != TB710FU_LIVE_CAPTURE_READY) return;
    struct tb710fu_live_log_info info = {0};
    (void)tb710fu_live_log_get_info(&xnu_sender, &info);
    int permitted = started ? tb710fu_live_log_can_snapshot(&xnu_sender) :
                              tb710fu_live_log_can_snapshot_single(&xnu_sender);
    if (!permitted ||
        (xnu_sender.valid && xnu_state.start == info.start && xnu_state.end == info.end &&
         xnu_state.dropped == xnu_last_dropped)) return;
    /* Before the worker runs, export one compact current-tail frame. The early
     * backup remains in early_bytes for the later multi-frame export, but is not
     * silently truncated into this single-frame file. */
    if (!started) {
        uint64_t generation = xnu_generation + 1u;
        size_t count = (size_t)copied;
        while (count > 0 || xnu_state.state == TB710FU_LIVE_CAPTURE_EMPTY) {
            uint64_t start = xnu_state.end - count;
            size_t capacity = tb710fu_live_log_single_file_capacity(
                TB710FU_LIVE_LOG_XNU, generation, start, xnu_state.end);
            int header = snprintf((char *)xnu_file, sizeof(xnu_file),
                "TB710FU XNU DEBUG EXPORT v1\nsource=xnu-debug\nbuild=%.64s\n"
                "earlybackup_state=%s\nearlybackup_start=%llu\nearlybackup_end=%llu\n"
                "earlybackup_bytes=%u\nearlybackup_export=deferred_multi_frame\n"
                "current_start=%llu\ncurrent_end=%llu\ncurrent_bytes=%u\n"
                "dropped=%llu\nraw_layout=current_only_single_frame\nraw_bytes_begin\n",
                TB_LIVE_BUILD_LABEL, state_name(early_state.state),
                (unsigned long long)early_state.start, (unsigned long long)early_state.end,
                early_state.copied, (unsigned long long)start,
                (unsigned long long)xnu_state.end, (unsigned)count,
                (unsigned long long)xnu_state.dropped);
            if (header >= 0 && (size_t)header + count <= capacity) {
                for (size_t i = 0; i < count; ++i)
                    xnu_file[(size_t)header + i] = snapshot[copied - count + i];
                if (tb710fu_live_log_snapshot_single_file(&xnu_sender, xnu_file,
                    (size_t)header + count, generation, start, xnu_state.end) ==
                    TB710FU_LIVE_LOG_READY) {
                    ++xnu_generation;
                    xnu_last_dropped = xnu_state.dropped;
                }
                return;
            }
            if (count == 0) break;
            --count;
        }
        return;
    }

    /* The header supplies exact raw-section lengths; no separator or newline is
     * inserted into either captured byte sequence. The filename range describes
     * only the current tail, not metadata or the preserved early backup. */
    int header = snprintf((char *)xnu_file, 512u,
        "TB710FU XNU DEBUG EXPORT v1\nsource=xnu-debug\nbuild=%.64s\n"
        "earlybackup_state=%s\nearlybackup_start=%llu\nearlybackup_end=%llu\n"
        "earlybackup_bytes=%u\ncurrent_start=%llu\ncurrent_end=%llu\ncurrent_bytes=%u\n"
        "dropped=%llu\nraw_layout=earlybackup_then_current\nraw_bytes_begin\n",
        TB_LIVE_BUILD_LABEL, state_name(early_state.state),
        (unsigned long long)early_state.start, (unsigned long long)early_state.end,
        early_state.copied, (unsigned long long)xnu_state.start,
        (unsigned long long)xnu_state.end, (unsigned)copied,
        (unsigned long long)xnu_state.dropped);
    if (header < 0 || header >= 512) return;
    size_t length = (size_t)header;
    for (size_t i = 0; i < early_state.copied; ++i) xnu_file[length++] = early_bytes[i];
    for (int i = 0; i < copied; ++i) xnu_file[length++] = snapshot[i];
    if (tb710fu_live_log_snapshot_file(&xnu_sender, xnu_file, length,
        xnu_generation + 1u, xnu_state.start, xnu_state.end) == TB710FU_LIVE_LOG_READY) {
        ++xnu_generation;
        xnu_last_dropped = xnu_state.dropped;
    }
}

static void snapshot_ramoops(void)
{
    int started = __atomic_load_n(&worker_started, __ATOMIC_ACQUIRE);
    size_t capacity = started ? sizeof(snapshot) : 512u;
    int copied = tb710fu_ramoops_snapshot_copy(snapshot, capacity, &ramoops_state);
    struct tb710fu_live_log_info info = {0};
    (void)tb710fu_live_log_get_info(&ramoops_sender, &info);
    /* Before the early freeze, the ring may contain only its synthetic banner.
     * Do not latch that as generation 1 and delay useful text behind the dwell. */
    if (!started && !__atomic_load_n(&early_frozen, __ATOMIC_ACQUIRE)) return;
    int permitted = started ? tb710fu_live_log_can_snapshot(&ramoops_sender) :
                              tb710fu_live_log_can_snapshot_single(&ramoops_sender);
    if (copied > 0 && permitted &&
        (!ramoops_sender.valid || ramoops_state.start != info.start || ramoops_state.end != info.end)) {
        enum tb710fu_live_log_result result;
        if (!started)
            result = tb710fu_live_log_snapshot_single_file(&ramoops_sender,
                snapshot, (size_t)copied, ramoops_generation + 1u,
                ramoops_state.start, ramoops_state.end);
        else
            result = tb710fu_live_log_snapshot(&ramoops_sender, snapshot,
                (size_t)copied, ramoops_generation + 1u,
                ramoops_state.start, ramoops_state.end);
        if (result == TB710FU_LIVE_LOG_READY)
            ++ramoops_generation;
    }
}

/* Each global 250 ms tick handles at most one stream/frame. Slots alternate;
 * drawing one dual-screen slot never clears the other stream's last frame.
 * A refresh repaints the three bands on the tick that consumed the redraw flag
 * and then draws only that tick's slot, so the other rectangle returns on the
 * following tick; no extra frame or snapshot is created for it. */
void tb710fu_live_log_poll(void)
{
    if (!tb710fu_live_log_active()) return;
    if (__atomic_load_n(&checkpoint_mode, __ATOMIC_ACQUIRE)) return;
    if (__atomic_exchange_n(&guard, 1u, __ATOMIC_ACQUIRE)) return;
    uint64_t now = live_ticks();
    if (now < next_frame) goto done;
    unsigned refresh = __atomic_exchange_n(&redraw, 0u, __ATOMIC_ACQ_REL);
    next_frame = now + frequency / 4u;
    ++poll_count;
    if (refresh) (void)tb_live_display_bands(&display);
    unsigned slot = next_slot;
    next_slot ^= 1u;
    if (slot == 0) snapshot_xnu();
    else snapshot_ramoops();
    struct tb710fu_live_log_sender *sender = slot == 0 ? &xnu_sender : &ramoops_sender;
    if (tb710fu_live_log_next_frame(sender, frame)) {
        const uint32_t *pixels;
        if (tb_live_qr_encode(frame, sizeof(frame), &pixels)) {
            ++qr_encoded[slot];
            if (tb_live_display_qr_slot(&display, pixels, 121u * 121u, 121, slot))
                ++qr_drawn[slot];
        }
    }
    status_line(slot);
    live_barrier();
done:
    __atomic_store_n(&guard, 0u, __ATOMIC_RELEASE);
}

static void checkpoint_status(const char *result);

static void checkpoint_qr_advance_locked(void)
{
    uint64_t now = live_ticks();
    if (!checkpoint_sender.valid ||
        (checkpoint_last_qr_attempt_end &&
         now - checkpoint_last_qr_attempt_end < frequency / 4u))
        return;
    if (!tb710fu_live_log_next_frame(&checkpoint_sender, frame)) {
        (void)__atomic_fetch_add(&checkpoint_failures, 1u, __ATOMIC_RELAXED);
        checkpoint_status("FRAME-ERR");
        checkpoint_last_qr_attempt_end = live_ticks();
        return;
    }
    const uint32_t *pixels = NULL;
    if (!tb_live_qr_encode(frame, sizeof(frame), &pixels)) {
        (void)__atomic_fetch_add(&checkpoint_failures, 1u, __ATOMIC_RELAXED);
        checkpoint_status("ENCODE-ERR");
        checkpoint_last_qr_attempt_end = live_ticks();
        return;
    }
    ++qr_encoded[0];
    if (!tb_live_display_checkpoint_qr(&display, pixels, 121u * 121u, 121u)) {
        (void)__atomic_fetch_add(&checkpoint_failures, 1u, __ATOMIC_RELAXED);
        checkpoint_status("DRAW-ERR");
        checkpoint_last_qr_attempt_end = live_ticks();
        return;
    }
    ++qr_drawn[0];
    ++checkpoint_generation;
    checkpoint_status("QR-LOOP");
    live_barrier();
    checkpoint_last_qr_attempt_end = live_ticks();
}

static void checkpoint_qr_tick(void)
{
    if (!tb710fu_live_log_active() ||
        !__atomic_load_n(&checkpoint_mode, __ATOMIC_ACQUIRE) ||
        __atomic_exchange_n(&checkpoint_guard, 1u, __ATOMIC_ACQUIRE)) return;
    checkpoint_qr_advance_locked();
    __atomic_store_n(&checkpoint_guard, 0u, __ATOMIC_RELEASE);
}

void tb710fu_live_log_refresh(void)
{
    if (!tb710fu_live_log_active()) return;
    if (__atomic_load_n(&checkpoint_mode, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&checkpoint_bands_pending, 1u, __ATOMIC_RELEASE);
        tb710fu_live_log_checkpoint("display-refresh");
        return;
    }
    __atomic_store_n(&redraw, 1u, __ATOMIC_RELEASE);
    tb710fu_live_log_poll();
}

static void checkpoint_set_stage(const char *stage)
{
    size_t index = 0;
    if (stage == NULL) stage = "unspecified";
    while (index < sizeof(checkpoint_stage) - 1u && stage[index] != '\0') {
        unsigned char c = (unsigned char)stage[index];
        checkpoint_stage[index] = c >= 32u && c <= 126u ? (char)c : '?';
        ++index;
    }
    checkpoint_stage[index] = '\0';
}

static void checkpoint_status(const char *result)
{
    char text[256];
    (void)snprintf(text, sizeof(text), "CKPT=%s N=%llu/%u ERR=%u LOG-END=%llu %.18s",
        result, (unsigned long long)checkpoint_generation,
        checkpoint_sender.valid ? checkpoint_sender.info.rounds : 0u,
        __atomic_load_n(&checkpoint_failures, __ATOMIC_RELAXED),
        (unsigned long long)display.LogEnd, checkpoint_stage);
    (void)tb_live_display_checkpoint_status(&display, text);
    live_barrier();
}

/* Keep each published QR file immutable until its multiframe dwell completes. */
static void checkpoint_publish(const char *stage, int sampled)
{
    if (!tb710fu_live_log_active()) return;
    if (!__atomic_load_n(&checkpoint_mode, __ATOMIC_ACQUIRE)) {
        tb710fu_live_log_poll();
        return;
    }
    if (__atomic_exchange_n(&checkpoint_guard, 1u, __ATOMIC_ACQUIRE)) {
        (void)__atomic_fetch_add(&checkpoint_failures, 1u, __ATOMIC_RELAXED);
        return;
    }

    /* Measure the quiet period from the end of the last attempt so a slow
     * encoder or framebuffer does not make the next hot-loop marker eligible. */
    uint64_t now = live_ticks();
    int qr_due = !checkpoint_sender.valid || !qr_drawn[0] ||
        now - checkpoint_last_qr_attempt_end >= frequency / 4u;
    int session_replaced = 0;
    if (sampled && now - checkpoint_last_attempt_end < frequency / 4u) {
        if (!qr_due) goto unlock;
    }

    checkpoint_set_stage(stage);
    struct tb710fu_live_capture_info state = {0};
    int copied = 0;
    for (unsigned attempt = 0; attempt < 8u; ++attempt) {
        copied = tb710fu_live_capture_copy(&xnu_capture, snapshot, 3072u, &state);
        if (state.state != TB710FU_LIVE_CAPTURE_BUSY) break;
    }
    if (__atomic_exchange_n(&checkpoint_bands_pending, 0u, __ATOMIC_ACQ_REL))
        (void)tb_live_display_bands(&display);
    if ((state.state != TB710FU_LIVE_CAPTURE_EMPTY &&
        state.state != TB710FU_LIVE_CAPTURE_READY) || copied < 0) {
        (void)__atomic_fetch_add(&checkpoint_failures, 1u, __ATOMIC_RELAXED);
        checkpoint_status(state.state == TB710FU_LIVE_CAPTURE_BUSY ? "BUSY" : "CAPTURE-ERR");
        goto done;
    }
    uint64_t log_start = state.end - (uint64_t)copied;
    (void)tb_live_display_scroll_log(&display, (const char *)snapshot,
                                     (size_t)copied, log_start, state.end);

    int backup_frozen = __atomic_load_n(&early_frozen, __ATOMIC_ACQUIRE);
    int backup_ready = backup_frozen && early_state.state == TB710FU_LIVE_CAPTURE_READY;
    if (backup_frozen && (!checkpoint_sender.valid ||
        tb710fu_live_log_can_snapshot(&checkpoint_sender))) {
        size_t early_count = backup_ready ? early_state.copied : 0u;
        size_t current_count = (size_t)copied;
        ++checkpoint_file_generation;
        if (checkpoint_file_generation == 0) checkpoint_file_generation = 1u;
        for (;;) {
            uint64_t current_start = state.end - current_count;
            const char *backup_export = early_count ? "included_multi_frame" : "unavailable";
            const char *layout = early_count ? "earlybackup_then_current" : "current_only_multi_frame";
            int header = snprintf((char *)checkpoint_file, sizeof(checkpoint_file),
                "TB710FU XNU DEBUG EXPORT v1\nsource=xnu-debug\nbuild=%.64s\n"
                "checkpoint=%.48s\ncheckpoint_mode=synchronous_multiframe_loop\n"
                "earlybackup_state=%s\nearlybackup_start=%llu\nearlybackup_end=%llu\n"
                "earlybackup_bytes=%u\nearlybackup_export=%s\n"
                "current_start=%llu\ncurrent_end=%llu\ncurrent_bytes=%u\n"
                "dropped=%llu\nraw_layout=%s\nraw_bytes_begin\n",
                TB_LIVE_BUILD_LABEL, checkpoint_stage,
                __atomic_load_n(&early_frozen, __ATOMIC_ACQUIRE) ? state_name(early_state.state) : "pending",
                (unsigned long long)early_state.start, (unsigned long long)early_state.end,
                (unsigned)early_count, backup_export, (unsigned long long)current_start,
                (unsigned long long)state.end, (unsigned)current_count,
                (unsigned long long)state.dropped, layout);
            if (header >= 0 && (size_t)header + early_count + current_count <= sizeof(checkpoint_file)) {
                size_t offset = (size_t)header;
                for (size_t i = 0; i < early_count; ++i) checkpoint_file[offset++] = early_bytes[i];
                for (size_t i = 0; i < current_count; ++i)
                    checkpoint_file[offset++] = snapshot[(size_t)copied - current_count + i];
                enum tb710fu_live_log_result replaced = tb710fu_live_log_snapshot_file(
                    &checkpoint_sender, checkpoint_file, offset, checkpoint_file_generation,
                    current_start, state.end);
                if (replaced == TB710FU_LIVE_LOG_READY) {
                    session_replaced = 1;
                    checkpoint_status("QR-SESSION");
                } else if (replaced != TB710FU_LIVE_LOG_BUSY) {
                    (void)__atomic_fetch_add(&checkpoint_failures, 1u, __ATOMIC_RELAXED);
                    checkpoint_status("FILE-ERR");
                    goto done;
                }
                break;
            }
            if (current_count == 0u) {
                (void)__atomic_fetch_add(&checkpoint_failures, 1u, __ATOMIC_RELAXED);
                checkpoint_status("SIZE-ERR");
                goto done;
            }
            size_t excess = header < 0 || (size_t)header + early_count > sizeof(checkpoint_file) ?
                current_count : (size_t)header + early_count + current_count - sizeof(checkpoint_file);
            size_t step = excess > 16u ? excess : 16u;
            current_count = current_count > step ? current_count - step : 0u;
        }
    }
    if (!checkpoint_sender.valid) {
        checkpoint_status(backup_frozen ? "FILE-ERR" : "QR-WAIT-BACKUP");
        goto done;
    }
    if (session_replaced)
        qr_due = 1;
    if (qr_due)
        checkpoint_qr_advance_locked();
    checkpoint_status("OK");

done:
    checkpoint_last_attempt_end = live_ticks();
unlock:
    __atomic_store_n(&checkpoint_guard, 0u, __ATOMIC_RELEASE);
}

void tb710fu_live_log_checkpoint(const char *stage)
{
    checkpoint_publish(stage, 0);
}

void tb710fu_live_log_checkpoint_sampled(const char *stage)
{
    checkpoint_publish(stage, 1);
}

/* Preserve precisely this pre-machine_startup boundary once. A busy/empty
 * result is exported honestly; a later post-startup tail is never called early.
 * Freeze itself never encodes, draws, waits or depends on a worker. */
void tb710fu_live_log_freeze(void)
{
    if (!tb710fu_live_log_active()) return;
    if (__atomic_exchange_n(&early_once, 1u, __ATOMIC_ACQUIRE)) return;
    (void)tb710fu_live_capture_copy(&xnu_capture, early_bytes, sizeof(early_bytes), &early_state);
    __atomic_store_n(&early_frozen, 1u, __ATOMIC_RELEASE);
    next_slot = 1u;
}

void tb710fu_live_log_init(void)
{
    int requested = 0;
    if (!PE_parse_boot_argn("tb710fu-live-log", &requested, sizeof(requested)) || requested != 1) return;
    int checkpoints = 0;
    int checkpoint_arg = PE_parse_boot_argn("tb710fu-live-checkpoints", &checkpoints, sizeof(checkpoints));
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
    if (frequency < 4u) return;
    tb710fu_live_log_sender_init(&xnu_sender);
    tb710fu_live_log_sender_init(&ramoops_sender);
    tb710fu_live_log_sender_init(&checkpoint_sender);
    (void)tb710fu_live_log_set_source(&xnu_sender, TB710FU_LIVE_LOG_XNU);
    (void)tb710fu_live_log_set_source(&checkpoint_sender, TB710FU_LIVE_LOG_XNU);
    next_slot = 1u; /* First sample ramoops, but publish only after early freeze. */
    if (checkpoint_arg && checkpoints == 1 && !display.CheckpointQrScale) return;
    __atomic_store_n(&checkpoint_mode, checkpoint_arg && checkpoints == 1, __ATOMIC_RELEASE);
    __atomic_store_n(&enabled, 1u, __ATOMIC_RELEASE);
    tb710fu_live_log_refresh();
}

static void live_worker(void *argument, wait_result_t result)
{
    (void)argument; (void)result;
    __atomic_store_n(&worker_started, 1u, __ATOMIC_RELEASE);
    for (;;) {
        if (__atomic_load_n(&checkpoint_mode, __ATOMIC_ACQUIRE))
            checkpoint_qr_tick();
        else
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
