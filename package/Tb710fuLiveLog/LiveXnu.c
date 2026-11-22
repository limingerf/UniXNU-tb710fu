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
/* CDC runs before BSD fills the console, so its lines are the oldest bytes in
 * the shared ring and are the first to leave a compact frame's tail. Keep its
 * transcript in its own ring; a compact frame prefers it when it has any. */
static struct tb710fu_live_capture cdc_capture;
static uint8_t cdc_tail[512];
static struct tb710fu_live_capture_info early_state, xnu_state;
static struct tb710fu_ramoops_snapshot ramoops_state;
static uint8_t snapshot[TB710FU_LIVE_LOG_MAX_BYTES];
static uint8_t xnu_file[TB710FU_LIVE_LOG_MAX_BYTES], checkpoint_file[TB710FU_LIVE_LOG_MAX_BYTES];
static uint8_t early_bytes[512], frame[752];
static unsigned enabled, guard, redraw, worker_started, sleep_channel, early_frozen, early_once;
static unsigned checkpoint_mode, checkpoint_guard, checkpoint_failures, checkpoint_bands_pending;
static unsigned scheduler_marks;
static unsigned checkpoint_file_generation, checkpoint_qr_attempted, checkpoint_compact, checkpoint_text_attempted;
static unsigned qr_encoded[2], qr_drawn[2];
static unsigned next_slot;
static uint64_t frequency, next_frame, poll_count, xnu_generation, ramoops_generation;
static uint64_t checkpoint_generation, checkpoint_last_qr_attempt_end, xnu_last_dropped;
static uint64_t checkpoint_last_text_end, checkpoint_export_dropped, checkpoint_drawn_end;
static uint64_t checkpoint_stage_generation, checkpoint_export_stage_generation;
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

static __attribute__((noinline)) const char *checkpoint_qr_advance_locked(void)
{
    checkpoint_qr_attempted = 1u;
    uint32_t previous_sequence = checkpoint_sender.info.next_sequence;
    uint32_t previous_source_in_round = checkpoint_sender.source_in_round;
    uint32_t previous_rounds = checkpoint_sender.info.rounds;
    if (!tb710fu_live_log_next_frame(&checkpoint_sender, frame)) {
        (void)__atomic_fetch_add(&checkpoint_failures, 1u, __ATOMIC_RELAXED);
        checkpoint_last_qr_attempt_end = live_ticks();
        return "FRAME-ERR";
    }
    const uint32_t *pixels = NULL;
    if (!tb_live_qr_encode(frame, sizeof(frame), &pixels)) {
        checkpoint_sender.info.next_sequence = previous_sequence;
        checkpoint_sender.source_in_round = previous_source_in_round;
        checkpoint_sender.info.rounds = previous_rounds;
        (void)__atomic_fetch_add(&checkpoint_failures, 1u, __ATOMIC_RELAXED);
        checkpoint_last_qr_attempt_end = live_ticks();
        return "ENCODE-ERR";
    }
    ++qr_encoded[0];
    if (!tb_live_display_checkpoint_qr(&display, pixels, 121u * 121u, 121u)) {
        checkpoint_sender.info.next_sequence = previous_sequence;
        checkpoint_sender.source_in_round = previous_source_in_round;
        checkpoint_sender.info.rounds = previous_rounds;
        (void)__atomic_fetch_add(&checkpoint_failures, 1u, __ATOMIC_RELAXED);
        checkpoint_last_qr_attempt_end = live_ticks();
        return "DRAW-ERR";
    }
    ++qr_drawn[0];
    ++checkpoint_generation;
    checkpoint_drawn_end = checkpoint_sender.info.end;
    live_barrier();
    checkpoint_last_qr_attempt_end = live_ticks();
    return "QR-LOOP";
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
    int changed = 0;
    if (stage == NULL) stage = "unspecified";
    while (index < sizeof(checkpoint_stage) - 1u && stage[index] != '\0') {
        unsigned char c = (unsigned char)stage[index];
        char mapped = c >= 32u && c <= 126u ? (char)c : '?';
        if (checkpoint_stage[index] != mapped) changed = 1;
        checkpoint_stage[index] = mapped;
        ++index;
    }
    if (checkpoint_stage[index] != '\0') changed = 1;
    checkpoint_stage[index] = '\0';
    if (changed) (void)__atomic_fetch_add(&checkpoint_stage_generation, 1u, __ATOMIC_RELAXED);
}

static void checkpoint_status(const char *result, const char *stage)
{
    char text[256];
    (void)snprintf(text, sizeof(text), "CKPT=%s LOG=%llu QR=%llu W=%u N=%llu Q=%u B=%u/%u R=%u ERR=%u %.8s",
        result,
        (unsigned long long)display.LogEnd,
        (unsigned long long)checkpoint_drawn_end,
        __atomic_load_n(&worker_started, __ATOMIC_ACQUIRE),
        (unsigned long long)checkpoint_generation,
        checkpoint_sender.valid ? checkpoint_sender.info.next_sequence : 0u,
        checkpoint_sender.valid ? checkpoint_sender.source_in_round : 0u,
        checkpoint_sender.valid ? checkpoint_sender.info.block_count : 0u,
        checkpoint_sender.valid ? checkpoint_sender.info.rounds : 0u,
        __atomic_load_n(&checkpoint_failures, __ATOMIC_RELAXED),
        stage == NULL ? checkpoint_stage : stage);
    (void)tb_live_display_checkpoint_status(&display, text);
    live_barrier();
#ifdef TB_LIVE_HOST_TEST
    tb_live_host_status(text);
#endif
}

enum checkpoint_kind {
    CHECKPOINT_SAMPLED,
    CHECKPOINT_IMMEDIATE,
    CHECKPOINT_PRIORITY,
    CHECKPOINT_TEXT,
};

/* Both boot checkpoints and the worker service the same carousel. A NULL
 * stage is a worker tick and retains the last actual checkpoint label.
 * A priority boundary before the worker can immediately publish one complete
 * frame. A text checkpoint only paints the captured tail and its footer. */
static __attribute__((noinline)) void checkpoint_publish(const char *stage, enum checkpoint_kind kind)
{
    if (!tb710fu_live_log_active()) return;
    int text_only = kind == CHECKPOINT_TEXT;
    if (!__atomic_load_n(&checkpoint_mode, __ATOMIC_ACQUIRE)) {
        if (!text_only) tb710fu_live_log_poll();
        return;
    }
    if (__atomic_exchange_n(&checkpoint_guard, 1u, __ATOMIC_ACQUIRE)) {
        (void)__atomic_fetch_add(&checkpoint_failures, 1u, __ATOMIC_RELAXED);
        return;
    }

    if (stage != NULL) checkpoint_set_stage(stage);
    int refresh = !text_only && __atomic_exchange_n(&checkpoint_bands_pending, 0u, __ATOMIC_ACQ_REL);
    uint64_t now = live_ticks();
    int text_due = refresh || kind != CHECKPOINT_SAMPLED || !checkpoint_text_attempted ||
        now - checkpoint_last_text_end >= frequency / 4u;
    int backup_frozen = __atomic_load_n(&early_frozen, __ATOMIC_ACQUIRE);
    int compact = !__atomic_load_n(&worker_started, __ATOMIC_ACQUIRE);
    int priority_frame = kind == CHECKPOINT_PRIORITY && compact && backup_frozen;
    /* A text checkpoint paints the scroll tail, and before the worker runs it is
     * the only publish the boot chain makes. If a text checkpoint could never
     * replace the frame, the QR would keep naming a stage the boot had already
     * passed, which is exactly the photograph disagreeing with the bottom lane.
     * A one second dwell keeps the rectangle stable for a photograph in progress
     * while letting every BSD stage reach the frame. */
    int qr_refresh = !text_only || (checkpoint_qr_attempted &&
        now - checkpoint_last_qr_attempt_end >= frequency);
    int qr_due = qr_refresh && (priority_frame || !checkpoint_qr_attempted ||
        now - checkpoint_last_qr_attempt_end >= frequency / 4u);
    int can_replace = qr_due && backup_frozen && (priority_frame || (checkpoint_compact ?
        tb710fu_live_log_can_snapshot_single(&checkpoint_sender) :
        tb710fu_live_log_can_snapshot(&checkpoint_sender)));
    if (refresh) {
        (void)tb_live_display_bands(&display);
        (void)tb_live_display_scroll_refresh(&display);
    }
    const char *status = NULL;
    if (!text_due && !can_replace) goto draw_qr;
    struct tb710fu_live_capture_info state = {0};
    int copied = 0;
    for (unsigned attempt = 0; attempt < 8u; ++attempt) {
        copied = tb710fu_live_capture_copy(&xnu_capture, snapshot, 3072u, &state);
        if (state.state != TB710FU_LIVE_CAPTURE_BUSY) break;
    }
    if ((state.state != TB710FU_LIVE_CAPTURE_EMPTY &&
        state.state != TB710FU_LIVE_CAPTURE_READY) || copied < 0) {
        (void)__atomic_fetch_add(&checkpoint_failures, 1u, __ATOMIC_RELAXED);
        status = state.state == TB710FU_LIVE_CAPTURE_BUSY ? "BUSY" : "CAPTURE-ERR";
        if (text_due) {
            checkpoint_text_attempted = 1u;
            checkpoint_last_text_end = live_ticks();
        }
        goto draw_qr;
    }
    if (text_due) {
        uint64_t log_start = state.end - (uint64_t)copied;
        (void)tb_live_display_scroll_log(&display, (const char *)snapshot,
                                         (size_t)copied, log_start, state.end);
        checkpoint_text_attempted = 1u;
        checkpoint_last_text_end = live_ticks();
    }

    /* A compact frame has room for one tail. CDC runs before BSD fills the
     * console, so its transcript is the part a shared tail would drop first.
     * Once the CDC ring holds anything, the frame carries that instead; the
     * header's current_start/current_end then name that ring's bytes. */
    const uint8_t *export_bytes = snapshot;
    size_t export_count = (size_t)copied;
    uint64_t export_end = state.end;
    uint64_t export_dropped = state.dropped;
    if (compact) {
        struct tb710fu_live_capture_info cdc_state = {0};
        int cdc_copied = 0;
        for (unsigned attempt = 0; attempt < 8u; ++attempt) {
            cdc_copied = tb710fu_live_capture_copy(&cdc_capture, cdc_tail,
                sizeof(cdc_tail), &cdc_state);
            if (cdc_state.state != TB710FU_LIVE_CAPTURE_BUSY) break;
        }
        if (cdc_copied > 0) {
            export_bytes = cdc_tail;
            export_count = (size_t)cdc_copied;
            export_end = cdc_state.end;
            export_dropped = cdc_state.dropped;
        }
    }

    int backup_ready = backup_frozen && early_state.state == TB710FU_LIVE_CAPTURE_READY;
    if (can_replace && (priority_frame || !checkpoint_sender.valid || compact != (int)checkpoint_compact ||
        export_end != checkpoint_sender.info.end || export_dropped != checkpoint_export_dropped ||
        checkpoint_stage_generation != checkpoint_export_stage_generation)) {
        size_t early_available = backup_ready && !compact ? early_state.copied : 0u;
        size_t current_count = compact && export_count > 512u ? 512u : export_count;
        ++checkpoint_file_generation;
        if (checkpoint_file_generation == 0) checkpoint_file_generation = 1u;
        for (;;) {
            uint64_t current_start = export_end - current_count;
            size_t early_count = 0u;
            if (early_available && current_start > early_state.start) {
                uint64_t distinct_early = current_start - early_state.start;
                early_count = distinct_early < early_available ? (size_t)distinct_early : early_available;
            }
            uint64_t early_end = early_state.start + early_count;
            uint64_t gap_start = !compact && early_end < current_start ? early_end : current_start;
            uint64_t gap_end = current_start;
            uint64_t gap_bytes = gap_end - gap_start;
            const char *backup_export = early_count ?
                (early_count == early_available ? "included_multi_frame" : "included_prefix_multi_frame") :
                (early_available ? "already_in_current" : "unavailable");
            const char *layout = early_count ? "earlybackup_then_current" : "current_only_multi_frame";
            if (compact) {
                backup_export = backup_ready ? "deferred_multi_frame" : "unavailable";
                layout = "current_only_single_frame";
            }
            int header = snprintf((char *)checkpoint_file, sizeof(checkpoint_file),
                "TB710FU XNU DEBUG EXPORT v1\nsource=xnu-debug\nbuild=%.64s\n"
                "checkpoint=%.48s\ncheckpoint_mode=%s\n"
                "earlybackup_state=%s\nearlybackup_start=%llu\nearlybackup_end=%llu\n"
                "earlybackup_bytes=%u\nearlybackup_export=%s\n"
                "current_start=%llu\ncurrent_end=%llu\ncurrent_bytes=%u\n"
                "raw_gap_start=%llu\nraw_gap_end=%llu\nraw_gap_bytes=%llu\n"
                "dropped=%llu\nraw_layout=%s\nraw_bytes_begin\n",
                TB_LIVE_BUILD_LABEL, checkpoint_stage,
                compact ? "synchronous_single_frame_loop" : "synchronous_multiframe_loop",
                state_name(early_state.state),
                (unsigned long long)early_state.start,
                (unsigned long long)(early_state.start + early_count),
                (unsigned)early_count, backup_export, (unsigned long long)current_start,
                (unsigned long long)export_end, (unsigned)current_count,
                (unsigned long long)gap_start, (unsigned long long)gap_end,
                (unsigned long long)gap_bytes,
                (unsigned long long)export_dropped, layout);
            size_t capacity = compact ? tb710fu_live_log_single_file_capacity(
                TB710FU_LIVE_LOG_XNU, checkpoint_file_generation, current_start, export_end) :
                sizeof(checkpoint_file);
            if (header >= 0 && (size_t)header + early_count + current_count <= capacity) {
                size_t offset = (size_t)header;
                for (size_t i = 0; i < early_count; ++i) checkpoint_file[offset++] = early_bytes[i];
                for (size_t i = 0; i < current_count; ++i)
                    checkpoint_file[offset++] = export_bytes[export_count - current_count + i];
                /* The compact sender has already completed its own dwell.
                 * Start a new multiframe session with its independent dwell. */
                if (!compact && checkpoint_compact) {
                    tb710fu_live_log_sender_init(&checkpoint_sender);
                    (void)tb710fu_live_log_set_source(&checkpoint_sender, TB710FU_LIVE_LOG_XNU);
                }
                enum tb710fu_live_log_result replaced;
                if (priority_frame) {
                    replaced = tb710fu_live_log_replace_single_file(&checkpoint_sender, checkpoint_file,
                        offset, checkpoint_file_generation, current_start, export_end);
                } else if (compact) {
                    replaced = tb710fu_live_log_snapshot_single_file(&checkpoint_sender, checkpoint_file,
                        offset, checkpoint_file_generation, current_start, export_end);
                } else {
                    replaced = tb710fu_live_log_snapshot_file(&checkpoint_sender, checkpoint_file,
                        offset, checkpoint_file_generation, current_start, export_end);
                }
                if (replaced == TB710FU_LIVE_LOG_READY) {
                    checkpoint_compact = (unsigned)compact;
                    checkpoint_export_dropped = export_dropped;
                    checkpoint_export_stage_generation = checkpoint_stage_generation;
                } else if (replaced != TB710FU_LIVE_LOG_BUSY) {
                    (void)__atomic_fetch_add(&checkpoint_failures, 1u, __ATOMIC_RELAXED);
                    checkpoint_status("FILE-ERR", NULL);
                    goto done;
                }
                break;
            }
            if (current_count == 0u) {
                (void)__atomic_fetch_add(&checkpoint_failures, 1u, __ATOMIC_RELAXED);
                checkpoint_status("SIZE-ERR", NULL);
                goto done;
            }
            size_t excess = header < 0 || (size_t)header + early_count > capacity ?
                current_count : (size_t)header + early_count + current_count - capacity;
            size_t step = excess > 16u ? excess : 16u;
            current_count = current_count > step ? current_count - step : 0u;
        }
    }
draw_qr:
    if (qr_due && checkpoint_sender.valid) {
        const char *qr_status = checkpoint_qr_advance_locked();
        if (status == NULL) status = qr_status;
    }
    if (status == NULL && text_due) {
        if (text_only) status = "TEXT";
        else status = checkpoint_sender.valid ? "OK" : (backup_frozen ? "QR-WAIT-CAPTURE" : "QR-WAIT-BACKUP");
    }
    if (status != NULL) checkpoint_status(status, text_only ? stage : NULL);

done:
    __atomic_store_n(&checkpoint_guard, 0u, __ATOMIC_RELEASE);
}

void tb710fu_live_log_checkpoint(const char *stage)
{
    checkpoint_publish(stage == NULL ? "unspecified" : stage, CHECKPOINT_IMMEDIATE);
}

void tb710fu_live_log_checkpoint_sampled(const char *stage)
{
    checkpoint_publish(stage == NULL ? "unspecified" : stage, CHECKPOINT_SAMPLED);
}

void tb710fu_live_log_checkpoint_priority(const char *stage)
{
    checkpoint_publish(stage == NULL ? "unspecified" : stage, CHECKPOINT_PRIORITY);
}

void tb710fu_live_log_checkpoint_text(const char *stage)
{
    checkpoint_publish(stage == NULL ? "unspecified" : stage, CHECKPOINT_TEXT);
}

/* Called from a scoped BSD scheduler diagnostic, including with spinlocks held
 * or IRQs masked. Dependencies are the initialized framebuffer and a barrier.
 */
void tb710fu_live_log_scheduler_mark(unsigned stage)
{
    if (stage == 0u || stage > 16u || !tb710fu_live_log_active() ||
        !__atomic_load_n(&checkpoint_mode, __ATOMIC_ACQUIRE)) return;
    const unsigned bit = 1u << (stage - 1u);
    unsigned visited = __atomic_fetch_or(&scheduler_marks, bit, __ATOMIC_RELAXED) | bit;
    tb_live_display_scheduler_mark(&display, stage, visited);
    live_barrier();
}

void tb710fu_live_log_boot_mark(unsigned lane, const char *stage)
{
    if (!tb710fu_live_log_active() || !__atomic_load_n(&checkpoint_mode, __ATOMIC_ACQUIRE))
        return;
    tb_live_display_boot_mark(&display, lane, stage);
    /* The QR header carries checkpoint_stage, and the worker only ever calls
     * checkpoint_publish(NULL, ...), so nothing refreshed that string after the
     * first explicit publish. The frame therefore kept naming an early stage
     * while the bottom lane moved on, and the QR stopped being evidence. Every
     * mark now moves the stage the next frame will carry. The header has one
     * stage field, so the most recent mark on any lane wins: a mark names what
     * just ran, which is exactly what the frame should say. */
    checkpoint_set_stage(stage);
    live_barrier();
}

/* The CDC transport keeps its own bounded log ring because the ordinary console
 * may be blocked while it runs. That ring is only readable by the CDC channel,
 * which is exactly the channel that is not up yet, so a CDC failure used to
 * leave no trace anywhere: the eight-character bottom lane was the only record.
 * The same bytes already reach the console ring through tb710fu_live_log_putc();
 * a second copy here keeps the CDC transcript selectable on its own, because
 * CDC runs before BSD floods the console and a compact frame's tail would
 * otherwise drop it. Nothing here waits, allocates or touches the display;
 * contention drops a byte like every other capture writer. */
void tb710fu_live_log_cdc_bytes(const char *bytes, unsigned long length)
{
    if (!bytes || !length) return;
    for (unsigned long i = 0; i < length; ++i)
        (void)tb710fu_live_capture_putc(&cdc_capture, bytes[i]);
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
            checkpoint_publish(NULL, CHECKPOINT_SAMPLED);
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
