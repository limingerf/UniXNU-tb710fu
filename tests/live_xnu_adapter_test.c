/* SPDX-License-Identifier: MIT
 * Production adapter/capture/protocol/display with isolated host XNU services.
 * Real OneSend interoperability and Mu encoding are verified separately. */
#define TB_LIVE_HOST_TEST 1
#define XNU_TB710FU_LIVE_LOG 1
#include <assert.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kern/tb710fu_ramoops.h>
#include <kern/tb710fu_live_log.h>
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
typedef int wait_result_t;
typedef int kern_return_t;
typedef void *thread_t;
typedef void *event_t;
typedef struct { uint64_t v_baseAddr, v_depth, v_width, v_height, v_rowBytes, v_length, v_offset; } PE_Video;
#define THREAD_NULL NULL
#define THREAD_CONTINUE_NULL NULL
#define THREAD_UNINT 0
#define KERN_SUCCESS 0
#define NSEC_PER_MSEC 1000000u
#define EARLY_BOOT 17
#define STARTUP_RANK_FIRST 0
#define STARTUP(subsystem, rank, function) \
    static void (*registered_startup)(void) = function; \
    enum { registered_subsystem = subsystem, registered_rank = rank }
static uint64_t tb_live_host_ticks(void);
static uint64_t tb_live_host_frequency(void);
static void tb_live_host_status(const char *);
static int PE_parse_boot_argn(const char *, void *, int);
static int PE_current_console(PE_Video *);
static vm_offset_t ml_io_map(uint64_t, vm_size_t);
static kern_return_t kernel_thread_start(void (*)(void *, wait_result_t), void *, thread_t *);
static void thread_deallocate(thread_t);
static void clock_interval_to_deadline(unsigned, unsigned, uint64_t *);
static int assert_wait_deadline(event_t, int, uint64_t);
static void thread_block(void *);
#include "package/Tb710fuLiveLog/LiveXnu.c"

static PE_Video video_input;
static unsigned arg_present, arg_value, checkpoint_arg_present, checkpoint_arg_value;
static unsigned map_fail, reenter_encoder;
static int qr_encode_fail_slot;
static unsigned map_calls, snapshot_calls, qr_calls, wait_calls, block_calls;
static unsigned create_calls, deallocate_calls, deadline_calls, reentries, encoded_by_source[2];
static uint64_t clock_now, clock_frequency, encoder_ticks, xnu_written;
static char last_status[256];
static uint8_t log_bytes[8192], encoded_frame[TB710FU_LIVE_LOG_FRAME_BYTES];
static size_t log_length, last_capacity;
static unsigned log_state;
static uint32_t qr_pixels[TB_LIVE_QR_SIDE * TB_LIVE_QR_SIDE];
static uint8_t *frame_allocation;
static size_t frame_bytes;
static void (*created_worker)(void *, wait_result_t);
static jmp_buf worker_escape;
static uint32_t le32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint8_t xnu_byte(uint64_t offset) { return (uint8_t)(offset * 17u + 3u); }
static uint64_t tb_live_host_ticks(void) { return clock_now; }
static uint64_t tb_live_host_frequency(void) { return clock_frequency; }
static void tb_live_host_status(const char *text)
{ snprintf(last_status, sizeof(last_status), "%s", text); }
static int PE_parse_boot_argn(const char *name, void *output, int bytes)
{
    assert(bytes == (int)sizeof(int));
    if (strcmp(name, "tb710fu-live-log") == 0) {
        *(int *)output = (int)arg_value; return (int)arg_present;
    }
    assert(strcmp(name, "tb710fu-live-checkpoints") == 0);
    *(int *)output = (int)checkpoint_arg_value;
    return (int)checkpoint_arg_present;
}
static int PE_current_console(PE_Video *output) { *output = video_input; return 0; }
static vm_offset_t ml_io_map(uint64_t physical, vm_size_t bytes)
{
    ++map_calls;
    assert(physical == (video_input.v_baseAddr & ~UINT64_C(3)) && bytes <= frame_bytes);
    return map_fail ? 0 : (vm_offset_t)(frame_allocation + 64);
}
int tb710fu_ramoops_snapshot_copy(uint8_t *output, size_t capacity, struct tb710fu_ramoops_snapshot *info)
{
    ++snapshot_calls; last_capacity = capacity;
    assert(capacity <= TB710FU_LIVE_LOG_MAX_BYTES);
    *info = (struct tb710fu_ramoops_snapshot){0}; info->state = log_state;
    if (log_state != TB710FU_RAMOOPS_READY || !log_length) return 0;
    size_t n = log_length < capacity ? log_length : capacity;
    info->start = log_length - n; info->end = log_length; info->copied = (uint32_t)n;
    memcpy(output, log_bytes + log_length - n, n); return (int)n;
}
int tb_live_qr_encode(const uint8_t *input, size_t bytes, const uint32_t **pixels)
{
    ++qr_calls; assert(bytes == TB710FU_LIVE_LOG_FRAME_BYTES);
    unsigned slot = checkpoint_mode ? 0u : next_slot ^ 1u;
    const struct tb710fu_live_log_sender *s = checkpoint_mode ? &checkpoint_sender :
        (slot == 0 ? &xnu_sender : &ramoops_sender);
    assert(s->valid && le32(input + 4) == s->info.session_id && le32(input + 24) == s->info.payload_checksum);
    assert(s->info.source == (slot == 0 ? TB710FU_LIVE_LOG_XNU : TB710FU_LIVE_LOG_RAMOOPS));
    /* Actual emitted payload must belong to this slot's immutable source file. */
    uint32_t sequence = le32(input + 8); assert(sequence % 5u != 4u);
    uint32_t block = (uint32_t)(((uint64_t)(sequence / 5u) * 4u + sequence % 5u) % s->info.block_count);
    size_t offset = (size_t)block * 720u, count = s->info.envelope_bytes - offset;
    if (count > 720u) count = 720u;
    assert(memcmp(input + 28, s->envelope + offset, count) == 0);
    for (size_t i = count; i < 720u; ++i) assert(input[28u + i] == 0);
    clock_now += encoder_ticks;
    if (qr_encode_fail_slot == (int)slot) return 0;
    ++encoded_by_source[slot]; memcpy(encoded_frame, input, bytes);
    for (size_t i = 0; i < sizeof(qr_pixels) / sizeof(qr_pixels[0]); ++i)
        qr_pixels[i] = slot == 0 ? 0xff112233u : 0xffaabbccu;
    *pixels = qr_pixels;
    if (reenter_encoder) {
        unsigned old_qr = qr_calls, old_snap = snapshot_calls;
        assert(checkpoint_mode ? checkpoint_guard == 1u : guard == 1u); ++reentries;
        if (checkpoint_mode) tb710fu_live_log_checkpoint("encoder-reentry");
        else tb710fu_live_log_poll();
        assert(snapshot_calls == old_snap && qr_calls == old_qr);
    }
    return 1;
}
static kern_return_t kernel_thread_start(void (*fn)(void *, wait_result_t), void *arg, thread_t *thread)
{ assert(arg == NULL); ++create_calls; created_worker = fn; *thread = &sleep_channel; return KERN_SUCCESS; }
static void thread_deallocate(thread_t thread) { assert(thread == &sleep_channel); ++deallocate_calls; }
static void clock_interval_to_deadline(unsigned interval, unsigned scale, uint64_t *deadline)
{ ++deadline_calls; assert(interval == 250 && scale == NSEC_PER_MSEC); *deadline = clock_now + 250; }
static int assert_wait_deadline(event_t event, int state, uint64_t deadline)
{
    ++wait_calls; assert(event == (event_t)&sleep_channel && state == THREAD_UNINT && deadline == clock_now + 250);
    return 0;
}
static void thread_block(void *continuation)
{ assert(continuation == THREAD_CONTINUE_NULL); ++block_calls; longjmp(worker_escape, 1); }
static void guards(void)
{
    for (size_t i = 0; i < 64; ++i) {
        assert(frame_allocation[i] == 0xa5 && frame_allocation[64 + frame_bytes + i] == 0xa5);
    }
}
static void reset_fixture(void)
{
    free(frame_allocation); frame_bytes = 640u * 4u * 480u;
    frame_allocation = malloc(frame_bytes + 128); assert(frame_allocation);
    memset(frame_allocation, 0xa5, frame_bytes + 128); display = (TB_LIVE_DISPLAY){0};
    memset(&xnu_sender, 0, sizeof(xnu_sender)); memset(&ramoops_sender, 0, sizeof(ramoops_sender));
    memset(&checkpoint_sender, 0, sizeof(checkpoint_sender));
    memset(&xnu_capture, 0, sizeof(xnu_capture));
    memset(&cdc_capture, 0, sizeof(cdc_capture));
    early_state = (struct tb710fu_live_capture_info){0}; xnu_state = early_state;
    ramoops_state = (struct tb710fu_ramoops_snapshot){0}; memset(early_bytes, 0, sizeof(early_bytes));
    enabled = guard = redraw = worker_started = sleep_channel = early_frozen = early_once = next_slot = 0;
    checkpoint_mode = checkpoint_guard = checkpoint_failures = checkpoint_bands_pending = 0;
    checkpoint_generation = checkpoint_last_qr_attempt_end = 0;
    checkpoint_file_generation = checkpoint_qr_attempted = checkpoint_compact = checkpoint_text_attempted = 0;
    checkpoint_last_text_end = checkpoint_export_dropped = checkpoint_drawn_end = 0;
    checkpoint_stage_generation = checkpoint_export_stage_generation = 0;
    checkpoint_stage[0] = '\0';
    qr_encoded[0] = qr_encoded[1] = qr_drawn[0] = qr_drawn[1] = 0;
    frequency = next_frame = xnu_generation = ramoops_generation = xnu_last_dropped = 0;
    arg_present = arg_value = 1; checkpoint_arg_present = checkpoint_arg_value = 0;
    map_fail = reenter_encoder = 0;
    qr_encode_fail_slot = -1;
    map_calls = snapshot_calls = qr_calls = wait_calls = block_calls = 0;
    create_calls = deallocate_calls = deadline_calls = reentries = 0;
    encoded_by_source[0] = encoded_by_source[1] = 0; clock_now = encoder_ticks = xnu_written = 0; clock_frequency = 1000;
    last_status[0] = '\0';
    log_state = TB710FU_RAMOOPS_READY; log_length = 5000; last_capacity = 0; created_worker = NULL;
    for (size_t i = 0; i < sizeof(log_bytes); ++i) log_bytes[i] = (uint8_t)('R' + i % 13u);
    video_input = (PE_Video){ .v_baseAddr = 0xd5100000, .v_depth = 32, .v_width = 640,
        .v_height = 480, .v_rowBytes = 640 * 4, .v_length = frame_bytes };
}
static void feed_xnu(size_t count)
{ for (size_t i = 0; i < count; ++i) tb710fu_live_log_putc((char)xnu_byte(xnu_written++)); }
static void tick(void)
{
    unsigned old_qr = qr_calls, old_wait = wait_calls; clock_now += 250; tb710fu_live_log_poll();
    assert(qr_calls <= old_qr + 1u && wait_calls == old_wait && guard == 0); guards();
}
static void carousel_tick(void)
{
    assert(created_worker && checkpoint_mode);
    clock_now += 250;
    if (!setjmp(worker_escape)) created_worker(NULL, 0);
    assert(worker_started && wait_calls > 0 && block_calls > 0);
}
static const uint8_t *body(const struct tb710fu_live_log_sender *s)
{ return s->envelope + 26u + le16(s->envelope + 6) + le16(s->envelope + 8); }
static void assert_ramoops_body(size_t count)
{
    assert(ramoops_sender.info.bytes == count && ramoops_sender.info.end == log_length);
    assert(ramoops_sender.info.start == log_length - count);
    assert(memcmp(body(&ramoops_sender), log_bytes + log_length - count, count) == 0);
    assert(strstr(ramoops_sender.info.filename, "TB710FU-ramoops-live-") == ramoops_sender.info.filename);
}
static void assert_xnu_body(size_t count, const uint8_t *backup, size_t backup_length)
{
    const uint8_t *file = body(&xnu_sender);
    const char *marker = strstr((const char *)file, "raw_bytes_begin\n");
    assert(marker && (const uint8_t *)marker - file < 512);
    size_t header_length = (size_t)((const uint8_t *)marker - file) + strlen("raw_bytes_begin\n");
    if (strstr((const char *)file, "raw_layout=current_only_single_frame\n")) {
        unsigned current = 0, retained = 0;
        assert(sscanf(strstr((const char *)file, "current_bytes="),
                      "current_bytes=%u", &current) == 1);
        assert(sscanf(strstr((const char *)file, "earlybackup_bytes="),
                      "earlybackup_bytes=%u", &retained) == 1);
        assert(retained == backup_length && current <= count);
        assert(xnu_sender.info.bytes == header_length + current);
        assert(xnu_sender.info.block_count == 1);
        assert(xnu_sender.info.start == xnu_written - current &&
               xnu_sender.info.end == xnu_written);
        for (size_t i = 0; i < current; ++i)
            assert(file[header_length + i] == xnu_byte(xnu_written - current + i));
        assert(strstr((const char *)file, "earlybackup_export=deferred_multi_frame\n"));
        assert(xnu_sender.info.source == TB710FU_LIVE_LOG_XNU);
        return;
    }
    assert(xnu_sender.info.bytes == header_length + backup_length + count);
    assert(xnu_sender.info.bytes <= 4096 && count <= 3072);
    assert(xnu_sender.info.start == xnu_written - count && xnu_sender.info.end == xnu_written);
    assert(memcmp(file + header_length, backup, backup_length) == 0);
    for (size_t i = 0; i < count; ++i)
        assert(file[header_length + backup_length + i] == xnu_byte(xnu_written - count + i));
    char expected[96]; snprintf(expected, sizeof(expected), "earlybackup_bytes=%u\n", (unsigned)backup_length);
    assert(strstr((const char *)file, expected));
    snprintf(expected, sizeof(expected), "current_bytes=%u\n", (unsigned)count);
    assert(strstr((const char *)file, expected));
    assert(strstr(xnu_sender.info.filename, "TB710FU-xnu-live-") == xnu_sender.info.filename);
    assert(xnu_sender.info.source == TB710FU_LIVE_LOG_XNU);
}
static void assert_checkpoint_body(const char *stage)
{
    const uint8_t *file = body(&checkpoint_sender);
    const char *marker = strstr((const char *)file, "raw_bytes_begin\n");
    assert(marker && (const uint8_t *)marker - file < 512);
    size_t header = (size_t)((const uint8_t *)marker - file) + strlen("raw_bytes_begin\n");
    unsigned current = 0;
    unsigned long long current_start = 0, current_end = 0;
    assert(sscanf(strstr((const char *)file, "current_bytes="),
                  "current_bytes=%u", &current) == 1);
    assert(sscanf(strstr((const char *)file, "current_start="),
                  "current_start=%llu", &current_start) == 1);
    assert(sscanf(strstr((const char *)file, "current_end="),
                  "current_end=%llu", &current_end) == 1);
    assert(strstr((const char *)file, "TB710FU XNU DEBUG EXPORT v1\n"));
    assert(strstr((const char *)file, "checkpoint_mode=synchronous_multiframe_loop\n"));
    char expected[96];
    snprintf(expected, sizeof(expected), "checkpoint=%s\n", stage);
    assert(strstr((const char *)file, expected));
    assert(checkpoint_sender.info.source == TB710FU_LIVE_LOG_XNU);
    unsigned early = 0;
    unsigned long long early_start = 0, early_end = 0;
    assert(sscanf(strstr((const char *)file, "earlybackup_bytes="),
                  "earlybackup_bytes=%u", &early) == 1);
    assert(sscanf(strstr((const char *)file, "earlybackup_start="),
                  "earlybackup_start=%llu", &early_start) == 1);
    assert(sscanf(strstr((const char *)file, "earlybackup_end="),
                  "earlybackup_end=%llu", &early_end) == 1);
    unsigned expected_early = early_state.state == TB710FU_LIVE_CAPTURE_READY &&
        current_start > early_state.start ?
        (current_start - early_state.start < early_state.copied ?
            (unsigned)(current_start - early_state.start) : early_state.copied) : 0u;
    assert(early == expected_early && early <= sizeof(early_bytes));
    assert(early_start == early_state.start && early_end == early_start + early);
    assert(strstr((const char *)file, early ? "earlybackup_export=included_multi_frame\n" :
        (early_state.state == TB710FU_LIVE_CAPTURE_READY ?
            "earlybackup_export=already_in_current\n" : "earlybackup_export=unavailable\n")));
    assert(strstr((const char *)file, early ? "raw_layout=earlybackup_then_current\n" :
                                               "raw_layout=current_only_multi_frame\n"));
    assert(checkpoint_sender.info.block_count > 1 &&
           checkpoint_sender.info.bytes == header + early + current);
    assert(checkpoint_sender.info.end == current_end && checkpoint_sender.info.start == current_start);
    assert(strstr(checkpoint_sender.info.filename, "TB710FU-xnu-live-") == checkpoint_sender.info.filename);
    assert(memcmp(file + header, early_bytes, early) == 0);
    for (size_t i = 0; i < current; ++i)
        assert(file[header + early + i] == xnu_byte((uint64_t)current_end - current + i));
}
static void test_log_tail_ranges_allow_early_backup_overlap(void)
{
    struct tb710fu_live_capture capture = {0};
    uint8_t bytes[32];
    struct tb710fu_live_capture_info info;
    for (unsigned i = 0; i < 32u; ++i)
        assert(tb710fu_live_capture_putc(&capture, (char)('A' + i)));
    assert(tb710fu_live_capture_copy(&capture, bytes, sizeof(bytes), &info) == 32);
    uint64_t backup_start = 0, backup_end = 32;
    uint64_t current_start = info.end - 20u;
    unsigned distinct_backup = current_start > backup_start ?
        (unsigned)((current_start - backup_start < backup_end - backup_start) ?
                   current_start - backup_start : backup_end - backup_start) : 0u;
    assert(distinct_backup == 12u);
    assert(memcmp(bytes, "ABCDEFGHIJKL", distinct_backup) == 0);
    assert(memcmp(bytes + (info.copied - 20u),
                  "MNOPQRSTUVWXYZ[\\]^_`abc", 20u) == 0);
}
static void test_disabled_and_preinit_capture(void)
{
    reset_fixture(); arg_present = 0; feed_xnu(700);
    tb710fu_live_log_init(); tb710fu_live_log_poll(); tb710fu_live_log_refresh();
    tb710fu_live_log_freeze(); registered_startup();
    assert(!enabled && !map_calls && !snapshot_calls && !qr_calls && !create_calls);
    arg_present = 1; arg_value = 2; tb710fu_live_log_init(); assert(!map_calls);
    arg_value = 1; tb710fu_live_log_init(); assert(xnu_capture.written == 700 && qr_calls == 0);
    assert(!xnu_sender.valid && !ramoops_sender.valid && !early_frozen);
    tb710fu_live_log_freeze(); assert(early_state.start == 188 && early_state.end == 700);
    tick(); assert_ramoops_body(512);
    tick(); assert_xnu_body(512, early_bytes, 512); assert(!wait_calls && !create_calls); guards();
}
static void test_invalid_video_is_bounded(void)
{
    for (unsigned which = 0; which < 15; ++which) {
        reset_fixture(); feed_xnu(20);
        switch (which) {
        case 0: video_input.v_depth = 24; break;
        case 1: video_input.v_baseAddr = 0; break;
        case 2: video_input.v_baseAddr = UINT64_C(1) << 48; break;
        case 3: video_input.v_width = 8193; break;
        case 4: video_input.v_height = 8193; break;
        case 5: video_input.v_height = 0; break;
        case 6: video_input.v_rowBytes = 65537; break;
        case 7: video_input.v_rowBytes = 0; break;
        case 8: video_input.v_length = 1; break;
        case 9: video_input.v_offset = frame_bytes + 1; break;
        case 10: video_input.v_baseAddr = (UINT64_C(1) << 48) - 4096; break;
        case 11: video_input.v_width = 1; break;
        case 12: video_input.v_rowBytes = 8; break;
        case 13: map_fail = 1; break;
        default: clock_frequency = 1; break;
        }
        tb710fu_live_log_init(); tb710fu_live_log_poll();
        assert(!enabled && !snapshot_calls && !qr_calls && !create_calls && xnu_capture.written == 20); guards();
    }
}
static void test_global_budget_and_stream_isolation(void)
{
    static const uint8_t banner[] = "\n=== TB710FU XNU boot (UniXNU ramoops) ===\n";
    static const uint8_t tail[] = "pmap_startup() 481638 page entries\n";
    reset_fixture(); feed_xnu(1500);
    memcpy(log_bytes, banner, sizeof(banner) - 1u); log_length = sizeof(banner) - 1u;
    tb710fu_live_log_init();
    assert(!ramoops_sender.valid && qr_calls == 0); /* Do not freeze the banner as generation 1. */
    memcpy(log_bytes + log_length, tail, sizeof(tail) - 1u); log_length += sizeof(tail) - 1u;
    tb710fu_live_log_freeze();
    assert(map_calls == 1 && snapshot_calls == 1 && qr_calls == 0);
    for (unsigned i = 0; i < 100; ++i) { tb710fu_live_log_refresh(); tb710fu_live_log_poll(); }
    assert(qr_calls == 0 && snapshot_calls == 1); /* No refresh throttle bypass. */
    tick(); assert(qr_calls == 1 && encoded_by_source[1] == 1 && last_capacity == 512);
    assert_ramoops_body(log_length);
    tick(); assert(qr_calls == 2 && encoded_by_source[0] == 1);
    assert_xnu_body(512, early_bytes, 512); assert_ramoops_body(log_length);
    assert(memcmp(body(&ramoops_sender) + sizeof(banner) - 1u, tail, sizeof(tail) - 1u) == 0);
    assert(strstr(last_status, "QR=X1/1 R1/1"));
    assert(xnu_sender.info.session_id != ramoops_sender.info.session_id);
    struct tb710fu_live_log_sender saved = xnu_sender;
    feed_xnu(123); memset(log_bytes, 'Q', sizeof(log_bytes)); log_length = 6000;
    tick(); tick();
    assert(xnu_sender.info.generation == saved.info.generation && xnu_sender.info.rounds <= 2);
    assert(memcmp(xnu_sender.envelope, saved.envelope, sizeof(saved.envelope)) == 0);
    for (unsigned i = 0; i < 200; ++i) tick();
    assert_xnu_body(512, early_bytes, 512); assert_ramoops_body(512);
    assert(xnu_sender.info.generation > saved.info.generation && encoded_by_source[0] > 2 && encoded_by_source[1] > 2);
    assert(!create_calls && !wait_calls && !block_calls);
}
static void test_backup_survives_wrap_in_every_left_file(void)
{
    reset_fixture(); feed_xnu(900); tb710fu_live_log_init(); tb710fu_live_log_freeze();
    uint8_t backup[512]; memcpy(backup, early_bytes, sizeof(backup));
    struct tb710fu_live_capture_info frozen = early_state;
    for (unsigned pass = 0; pass < 3; ++pass) {
        feed_xnu(70000); for (unsigned i = 0; i < 200; ++i) tick();
        assert_xnu_body(512, backup, sizeof(backup)); assert(xnu_sender.info.start > frozen.end);
        tb710fu_live_log_freeze(); assert(memcmp(early_bytes, backup, sizeof(backup)) == 0);
        assert(memcmp(&early_state, &frozen, sizeof(frozen)) == 0);
    }
}
static void test_capture_and_render_trylocks(void)
{
    reset_fixture(); feed_xnu(600); reenter_encoder = 1;
    tb710fu_live_log_init(); tb710fu_live_log_freeze(); tick(); tick(); assert(reentries == 2);
    unsigned old_qr = qr_calls, old_snap = snapshot_calls;
    guard = 1; feed_xnu(10); tb710fu_live_log_poll(); tb710fu_live_log_refresh();
    assert(qr_calls == old_qr && snapshot_calls == old_snap && xnu_capture.written == xnu_written); guard = 0;
    xnu_capture.guard = 1; tb710fu_live_log_putc('!');
    assert(xnu_capture.dropped == 1 && xnu_capture.written == xnu_written);
    tick(); tick(); assert(xnu_state.state == TB710FU_LIVE_CAPTURE_BUSY); xnu_capture.guard = 0;
    assert(!wait_calls && !block_calls && !create_calls);
}
static void test_worker_requires_actual_execution(void)
{
    reset_fixture(); feed_xnu(6000); tb710fu_live_log_init(); tb710fu_live_log_freeze(); tick(); tick();
    assert(registered_subsystem == EARLY_BOOT && registered_rank == STARTUP_RANK_FIRST);
    registered_startup(); assert(create_calls == 1 && deallocate_calls == 1 && created_worker && !worker_started);
    assert_xnu_body(512, early_bytes, 512); assert_ramoops_body(512); clock_now += 500;
    if (!setjmp(worker_escape)) created_worker(NULL, 0);
    assert(worker_started && wait_calls == 1 && block_calls == 1 && deadline_calls == 1);
    for (unsigned i = 0; i < 220; ++i) tick();
    assert(last_capacity == 4096); assert_xnu_body(3072, early_bytes, 512); assert_ramoops_body(4096);
    assert(strstr(last_status, "W=1") && strstr(last_status, "P="));
    assert(strstr(last_status, "R=ready:4096B "));
    assert(wait_calls == 1 && block_calls == 1 && create_calls == 1);
}
static uint32_t slot_pixel(unsigned slot);
static void test_early_checkpoint_repeats_complete_frames_without_worker(void)
{
    reset_fixture(); feed_xnu(1500);
    checkpoint_arg_present = checkpoint_arg_value = 1;
    tb710fu_live_log_init();
    assert(enabled && checkpoint_mode && !create_calls && !worker_started);
    assert(checkpoint_generation == 0 && qr_calls == 0 && !checkpoint_sender.valid);
    assert(!xnu_sender.valid && !ramoops_sender.valid && !snapshot_calls);

    tb710fu_live_log_freeze(); feed_xnu(37);
    tb710fu_live_log_checkpoint("phase16-begin");
    assert(qr_calls == 1 && checkpoint_generation == 1 && checkpoint_sender.valid);
    assert(checkpoint_sender.info.block_count == 1 && le32(encoded_frame + 12) == 1);
    assert(le32(encoded_frame + 20) <= 720 && memcmp(encoded_frame + 28, "LMS1", 4) == 0);
    assert(strstr((const char *)body(&checkpoint_sender), "checkpoint=phase16-begin\n"));
    assert(strstr((const char *)body(&checkpoint_sender), "raw_layout=current_only_single_frame\n"));
    for (uint64_t i = checkpoint_sender.info.start; i < checkpoint_sender.info.end; ++i) {
        size_t offset = checkpoint_sender.info.bytes - (size_t)(checkpoint_sender.info.end - i);
        assert(body(&checkpoint_sender)[offset] == xnu_byte(i));
    }
    uint64_t first_session = checkpoint_sender.info.session_id;
    unsigned old_snapshot = snapshot_calls;
    feed_xnu(37);
    clock_now += 250;
    tb710fu_live_log_checkpoint_sampled("text-without-worker");
    assert(qr_calls == 2 && checkpoint_generation == 2 && le32(encoded_frame + 8) == 1 &&
           display.LogEnd == xnu_capture.written && checkpoint_sender.info.session_id == first_session);
    clock_now += 250;
    tb710fu_live_log_checkpoint_sampled("next-early-log");
    assert(qr_calls == 3 && checkpoint_sender.info.session_id != first_session &&
           checkpoint_sender.info.end == xnu_capture.written && !wait_calls);
    registered_startup();
    assert(create_calls == 1 && created_worker && !worker_started);
    carousel_tick();
    assert(qr_calls == 4 && checkpoint_compact && slot_pixel(0) == 0xff112233u);
    carousel_tick();
    assert(qr_calls == 5 && !checkpoint_compact && checkpoint_sender.info.block_count > 1);
    assert_checkpoint_body("next-early-log");
    first_session = checkpoint_sender.info.session_id;
    unsigned blocks = checkpoint_sender.info.block_count;

    checkpoint_guard = 1;
    tb710fu_live_log_checkpoint("contended-renderer");
    checkpoint_guard = 0;
    assert(qr_calls == 5 && checkpoint_failures == 1);
    xnu_capture.guard = 1;
    tb710fu_live_log_checkpoint("contended-capture");
    xnu_capture.guard = 0;
    assert(qr_calls == 5 && checkpoint_failures == 2);

    while (checkpoint_sender.info.rounds < TB710FU_LIVE_LOG_MIN_ROUNDS)
        carousel_tick();
    assert(checkpoint_sender.info.rounds >= TB710FU_LIVE_LOG_MIN_ROUNDS);
    assert(checkpoint_sender.info.block_count == blocks &&
           checkpoint_sender.info.session_id == first_session);
    assert(snapshot_calls == old_snapshot);

    feed_xnu(70000);
    tb710fu_live_log_checkpoint("wrapped-log-tail");
    assert(checkpoint_sender.info.session_id == first_session);
    assert(qr_calls == 4 + blocks * TB710FU_LIVE_LOG_MIN_ROUNDS);
    old_snapshot = snapshot_calls;
    carousel_tick();
    assert(checkpoint_sender.info.session_id != first_session && checkpoint_sender.info.rounds == 0);
    assert(qr_calls == 5 + blocks * TB710FU_LIVE_LOG_MIN_ROUNDS);
    assert_checkpoint_body("wrapped-log-tail");
    assert(snapshot_calls == old_snapshot && worker_started);
    guards();
}
static void test_hot_checkpoints_leave_quiet_time_after_an_attempt(void)
{
    reset_fixture();
    checkpoint_arg_present = checkpoint_arg_value = 1;
    feed_xnu(500);
    tb710fu_live_log_init();
    tb710fu_live_log_freeze();
    assert(qr_calls == 0 && checkpoint_generation == 0);
    tb710fu_live_log_checkpoint("fixed-fallback-assign");
    assert(qr_calls == 1 && checkpoint_generation == 1 && checkpoint_sender.info.block_count == 1);
    registered_startup();
    carousel_tick();
    assert(qr_calls == 2 && checkpoint_generation == 2);

    clock_now += 20;
    for (unsigned i = 0; i < 1024u; ++i) {
        feed_xnu(1);
        tb710fu_live_log_checkpoint_sampled("fixed-fallback-assign");
    }
    assert(qr_calls == 2 && checkpoint_generation == 2 && !checkpoint_failures);
    assert(checkpoint_sender.info.end == 500 && xnu_capture.written == 1524);
    assert(display.LogEnd == 500);

    /* Checkpoints can service the carousel after its worker misses a tick. */
    clock_now += 1000;
    feed_xnu(1);
    tb710fu_live_log_checkpoint_sampled("worker-qr-stalled");
    assert(qr_calls == 3 && checkpoint_generation == 3 && !checkpoint_compact);
    assert(display.LogEnd == xnu_capture.written);

    /* An immediate checkpoint publishes text during the QR quiet interval. */
    feed_xnu(1);
    tb710fu_live_log_checkpoint("fixed-views");
    assert(qr_calls == 3 && checkpoint_generation == 3 && display.LogEnd == xnu_capture.written);
    assert_checkpoint_body("worker-qr-stalled");
    qr_encode_fail_slot = 0;
    encoder_ticks = 180;
    uint32_t sequence = checkpoint_sender.info.next_sequence;
    carousel_tick();
    assert(qr_calls == 4 && checkpoint_failures == 1 && checkpoint_generation == 3);
    assert(checkpoint_sender.info.next_sequence == sequence);
    assert(slot_pixel(0) == 0xff112233u);
    clock_now += 249;
    tb710fu_live_log_checkpoint_sampled("quiet-after-failed-attempt");
    assert(qr_calls == 4);
    qr_encode_fail_slot = -1;
    ++clock_now;
    tb710fu_live_log_checkpoint_sampled("retry-after-failed-attempt");
    assert(qr_calls == 5 && checkpoint_generation == 4);
    guards();
}
static void test_checkpoint_multiframe_qr_worker_loops_without_new_snapshot(void)
{
    reset_fixture(); feed_xnu(1800);
    checkpoint_arg_present = checkpoint_arg_value = 1;
    tb710fu_live_log_init();
    assert(!checkpoint_sender.valid && qr_calls == 0);
    tb710fu_live_log_freeze();
    live_start_worker();
    carousel_tick();
    assert(checkpoint_sender.valid && checkpoint_sender.info.block_count > 1);
    assert(qr_calls == 1); /* first frame is painted at the session boundary */
    uint32_t blocks = checkpoint_sender.info.block_count;
    uint64_t session = checkpoint_sender.info.session_id;
    uint64_t bytes = checkpoint_sender.info.bytes;
    unsigned snapshots = snapshot_calls;
    unsigned old_qr = qr_calls;

    assert(create_calls == 1 && created_worker && worker_started);
    tb710fu_live_log_checkpoint_priority("active-worker");
    assert(qr_calls == old_qr && checkpoint_sender.info.session_id == session);
    carousel_tick();
    assert(wait_calls == 2 && block_calls == 2);
    assert(qr_calls == old_qr + 1 && checkpoint_sender.info.next_sequence > 1);
    assert(checkpoint_sender.info.session_id == session &&
           checkpoint_sender.info.block_count == blocks && checkpoint_sender.info.bytes == bytes);
    assert(snapshot_calls == snapshots && !xnu_sender.valid && !ramoops_sender.valid);
    assert(slot_pixel(0) == 0xff112233u);
    guards();
}
static void test_priority_publishes_latest_single_frame_before_possible_stall(void)
{
    static const char boundary[] = "TB710FU boot: kperf-init-early begin\n";
    reset_fixture(); feed_xnu(1500);
    checkpoint_arg_present = checkpoint_arg_value = 1;
    tb710fu_live_log_init();
    tb710fu_live_log_checkpoint_priority("before-freeze");
    assert(qr_calls == 0 && checkpoint_drawn_end == 0);

    tb710fu_live_log_freeze();
    tb710fu_live_log_checkpoint("previous-phase");
    assert(qr_calls == 1 && checkpoint_sender.info.rounds == 1);
    assert(checkpoint_drawn_end == 1500 && display.LogEnd == 1500);
    uint32_t previous_session = checkpoint_sender.info.session_id;
    for (size_t i = 0; i < sizeof(boundary) - 1u; ++i)
        tb710fu_live_log_putc(boundary[i]);
    tb710fu_live_log_checkpoint("ordinary-checkpoint");
    assert(qr_calls == 1 && display.LogEnd == xnu_capture.written);
    assert(strstr(last_status, "LOG=1537 QR=1500"));

    qr_encode_fail_slot = 0;
    tb710fu_live_log_checkpoint_priority("kperf-init-early");
    assert(qr_calls == 2 && checkpoint_generation == 1 && checkpoint_failures == 1);
    assert(strstr(last_status, "ENCODE-ERR LOG=1537 QR=1500"));

    qr_encode_fail_slot = -1;
    tb710fu_live_log_checkpoint_priority("kperf-init-early");
    assert(qr_calls == 3 && checkpoint_generation == 2 && clock_now == 0 && !worker_started);
    assert(checkpoint_sender.info.session_id != previous_session);
    assert(checkpoint_sender.info.block_count == 1 && le32(encoded_frame + 12) == 1);
    assert(le32(encoded_frame + 20) <= 720 && memcmp(encoded_frame + 28, "LMS1", 4) == 0);
    const uint8_t *file = body(&checkpoint_sender);
    assert(strstr((const char *)file, "checkpoint=kperf-init-early\n"));
    assert(memcmp(file + checkpoint_sender.info.bytes - (sizeof(boundary) - 1u),
                  boundary, sizeof(boundary) - 1u) == 0);
    assert(checkpoint_drawn_end == xnu_capture.written && checkpoint_drawn_end == display.LogEnd);
    assert(strstr(last_status, "LOG=1537 QR=1537") && slot_pixel(0) == 0xff112233u);
    guards();
}
static void test_text_checkpoint_only_updates_scroll_and_footer(void)
{
    static const char boundary[] = "TB710FU BSD: throttle begin\n";
    reset_fixture(); feed_xnu(1800);
    tb710fu_live_log_init(); tb710fu_live_log_freeze();
    assert(enabled && !checkpoint_mode && !qr_calls);
    uint8_t *marker = frame_allocation + 64u + (display.Height - 16u) * display.StrideBytes + 304u * 4u;
    tb710fu_live_log_boot_mark(0u, "disabled");
    assert(*marker == 0xa5);
    tb710fu_live_log_checkpoint_text("throttle");
    assert(!qr_calls && display.LogEnd == 0);

    reset_fixture(); feed_xnu(1800);
    checkpoint_arg_present = checkpoint_arg_value = 1;
    tb710fu_live_log_init(); tb710fu_live_log_freeze();
    live_start_worker();
    carousel_tick();
    assert(worker_started && qr_calls == 1 && checkpoint_sender.info.block_count > 1);
    struct tb710fu_live_log_sender last_qr = checkpoint_sender;
    uint64_t last_qr_attempt = checkpoint_last_qr_attempt_end;
    unsigned last_file_generation = checkpoint_file_generation;
    assert(checkpoint_qr_attempted && checkpoint_generation == 1);
    assert(slot_pixel(0) == 0xff112233u);

    for (size_t i = 0; i < sizeof(boundary) - 1u; ++i)
        tb710fu_live_log_putc(boundary[i]);
    clock_now += 250;
    checkpoint_bands_pending = 1;
    tb710fu_live_log_checkpoint_text("throttle");
    assert(display.LogEnd == xnu_capture.written && checkpoint_last_text_end == clock_now);
    assert(strstr(last_status, "CKPT=TEXT LOG=1828 QR=1800") && strstr(last_status, "throttle"));
    assert(qr_calls == 1 && checkpoint_generation == 1 && checkpoint_drawn_end == 1800);
    assert(checkpoint_qr_attempted && checkpoint_last_qr_attempt_end == last_qr_attempt);
    assert(checkpoint_file_generation == last_file_generation && checkpoint_bands_pending == 1);
    assert(memcmp(&last_qr, &checkpoint_sender, sizeof(last_qr)) == 0);
    assert(strcmp(checkpoint_stage, "throttle") == 0 && slot_pixel(0) == 0xff112233u);

    marker = frame_allocation + 64u + (display.Height - 16u) * display.StrideBytes + 304u * 4u;
    const uint64_t status_end = display.LogEnd;
    const unsigned previous_failures = checkpoint_failures;
    checkpoint_guard = xnu_capture.guard = 1u;
    tb710fu_live_log_boot_mark(0u, "socket");
    tb710fu_live_log_boot_mark(1u, "ready");
    assert(*marker != 0xa5 && *(marker + 112u * 4u) != 0xa5);
    assert(checkpoint_guard == 1u && xnu_capture.guard == 1u);
    assert(checkpoint_failures == previous_failures && display.LogEnd == status_end);
    assert(qr_calls == 1 && strstr(last_status, "throttle"));
    checkpoint_guard = xnu_capture.guard = 0u;

    tb710fu_live_log_checkpoint_sampled("next-worker-tick");
    assert(qr_calls == 2 && checkpoint_generation == 2 && !checkpoint_bands_pending);
    assert(checkpoint_sender.info.session_id == last_qr.info.session_id);
    guards();
}
static uint32_t slot_pixel(unsigned slot)
{
    if (checkpoint_mode) {
        uint32_t x = display.CheckpointQrX;
        return *(uint32_t *)(frame_allocation + 64u +
            (size_t)display.CheckpointQrY * display.StrideBytes + x * 4u);
    }
    unsigned x = slot ? display.QrSecondX : display.QrX;
    return *(uint32_t *)(frame_allocation + 64u + (size_t)display.QrY * display.StrideBytes + x * 4u);
}
static void test_text_checkpoint_refreshes_stale_frame(void)
{
    reset_fixture(); feed_xnu(1800);
    checkpoint_arg_present = checkpoint_arg_value = 1;
    tb710fu_live_log_init(); tb710fu_live_log_freeze();
    assert(checkpoint_mode && !worker_started && early_frozen);
    tb710fu_live_log_checkpoint_priority("boot-one");
    assert(qr_calls == 1 && checkpoint_generation == 1 && checkpoint_compact == 1);
    assert(strstr((const char *)body(&checkpoint_sender), "checkpoint=boot-one"));

    /* Inside the dwell a text checkpoint only paints the scroll tail. */
    feed_xnu(40);
    clock_now += 250;
    tb710fu_live_log_checkpoint_text("boot-two");
    assert(checkpoint_generation == 1 && qr_calls == 1);
    assert(strstr(last_status, "CKPT=TEXT"));

    /* The first text checkpoint after the dwell completes the second round, so
     * this publish only advances the carousel; the file is still the priority one. */
    feed_xnu(40);
    clock_now += frequency;
    tb710fu_live_log_checkpoint_text("boot-three");
    assert(checkpoint_generation == 2 && qr_calls == 2);
    assert(strstr((const char *)body(&checkpoint_sender), "checkpoint=boot-one"));

    /* The next one may replace it, and the new header names the live stage. */
    feed_xnu(40);
    clock_now += frequency;
    tb710fu_live_log_checkpoint_text("boot-four");
    assert(checkpoint_generation == 3 && qr_calls == 3);
    assert(strcmp(checkpoint_stage, "boot-four") == 0);
    assert(strstr((const char *)body(&checkpoint_sender), "checkpoint=boot-four"));
    guards();
}
static void test_compact_frame_prefers_cdc_tail(void)
{
    static const char cdc[] = "TB710FU CDC: stage=smmu-route failure=sidamb\n";
    reset_fixture(); feed_xnu(1800);
    checkpoint_arg_present = checkpoint_arg_value = 1;
    tb710fu_live_log_init(); tb710fu_live_log_freeze();
    tb710fu_live_log_cdc_bytes(cdc, sizeof(cdc) - 1u);
    tb710fu_live_log_checkpoint_priority("cdc-one");
    const char *file = (const char *)body(&checkpoint_sender);
    const char *marker = strstr(file, "raw_bytes_begin\n");
    assert(marker);
    assert(memcmp(marker + strlen("raw_bytes_begin\n"), cdc, sizeof(cdc) - 1u) == 0);
    assert(strstr(file, "current_bytes=") && strstr(file, "checkpoint=cdc-one"));
    char field[64];
    snprintf(field, sizeof(field), "current_end=%u\n", (unsigned)(sizeof(cdc) - 1u));
    assert(strstr(file, "current_start=0\n") && strstr(file, field));
    /* The console tail is still longer than the CDC ring; the frame carries the
     * CDC transcript anyway, which is the whole point of the reserved ring. */
    assert(xnu_capture.written > cdc_capture.written);
    guards();
}
static void test_dual_slots_do_not_clear_each_other(void)
{
    reset_fixture(); feed_xnu(700); tb710fu_live_log_init(); tb710fu_live_log_freeze(); tick(); tick();
    assert(display.DualQr && slot_pixel(0) == 0xff112233u && slot_pixel(1) == 0xffaabbccu);
    for (unsigned i = 0; i < 8; ++i) { tick(); assert(slot_pixel(0) == 0xff112233u && slot_pixel(1) == 0xffaabbccu); }
}
static void test_small_screen_alternates_one_rectangle(void)
{
    reset_fixture(); video_input.v_width = 240; video_input.v_rowBytes = 960;
    video_input.v_length = frame_bytes = 960u * 480u; feed_xnu(700);
    tb710fu_live_log_init(); tb710fu_live_log_freeze();
    tick();
    assert(!display.DualQr && display.QrX == display.QrSecondX && slot_pixel(0) == 0xffaabbccu);
    for (unsigned i = 0; i < 10; ++i) { tick(); assert(slot_pixel(0) == (i % 2u ? 0xffaabbccu : 0xff112233u)); }
}
static void test_checkpoint_mode_rejects_surface_without_log_column(void)
{
    reset_fixture();
    video_input.v_width = 153; video_input.v_height = 240;
    video_input.v_rowBytes = 616; video_input.v_length = frame_bytes = 616u * 240u;
    checkpoint_arg_present = checkpoint_arg_value = 1;
    tb710fu_live_log_init();
    assert(map_calls == 1 && !enabled && !checkpoint_mode && qr_calls == 0);
}
static void test_ready_stream_reports_qr_encode_failure(void)
{
    reset_fixture(); qr_encode_fail_slot = 1;
    tb710fu_live_log_init(); tb710fu_live_log_freeze(); tick(); tick();
    assert(ramoops_sender.valid && ramoops_state.state == TB710FU_RAMOOPS_READY);
    assert(strstr(last_status, "R=ready:512B 1/1"));
    assert(strstr(last_status, "QR=X1/1 R0/0"));
    assert(slot_pixel(1) != 0xffaabbccu);
}
static void test_ready_stream_reports_qr_draw_failure(void)
{
    reset_fixture();
    tb710fu_live_log_init(); tb710fu_live_log_freeze(); tick(); tick();
    assert(qr_encoded[1] == 1 && qr_drawn[1] == 1);
    display.Bytes = 0;
    tick(); tick();
    assert(ramoops_sender.valid && ramoops_state.state == TB710FU_RAMOOPS_READY);
    assert(strstr(last_status, "QR=X2/1 R2/1"));
}
static void test_freeze_busy_and_empty_are_honest(void)
{
    for (unsigned busy = 0; busy < 2; ++busy) {
        reset_fixture(); tb710fu_live_log_init();
        xnu_capture.guard = busy; guard = 1; tb710fu_live_log_freeze(); guard = 0;
        assert(early_frozen && early_state.copied == 0);
        assert(early_state.state == (busy ? TB710FU_LIVE_CAPTURE_BUSY : TB710FU_LIVE_CAPTURE_EMPTY));
        xnu_capture.guard = 0; feed_xnu(1000); tb710fu_live_log_freeze();
        assert(early_state.copied == 0); tick(); tick(); assert_xnu_body(512, early_bytes, 0);
        assert(strstr((const char *)body(&xnu_sender), busy ? "earlybackup_state=busy\n" : "earlybackup_state=empty\n"));
    }
}
static void test_ramoops_failure_does_not_replace_xnu(void)
{
    for (unsigned state = TB710FU_RAMOOPS_UNMAPPED; state <= TB710FU_RAMOOPS_EMPTY; ++state) {
        reset_fixture(); log_state = state; feed_xnu(700);
        tb710fu_live_log_init(); tb710fu_live_log_freeze(); tick(); tick();
        assert(!ramoops_sender.valid && xnu_sender.valid && !guard);
        assert(encoded_by_source[0] == 1 && encoded_by_source[1] == 0); assert_xnu_body(512, early_bytes, 512);
    }
}
int main(void)
{
    test_disabled_and_preinit_capture(); test_log_tail_ranges_allow_early_backup_overlap();
    test_invalid_video_is_bounded();
    test_global_budget_and_stream_isolation(); test_backup_survives_wrap_in_every_left_file();
    test_capture_and_render_trylocks(); test_worker_requires_actual_execution();
    test_dual_slots_do_not_clear_each_other(); test_small_screen_alternates_one_rectangle();
    test_checkpoint_mode_rejects_surface_without_log_column();
    test_ready_stream_reports_qr_encode_failure(); test_ready_stream_reports_qr_draw_failure();
    test_freeze_busy_and_empty_are_honest(); test_ramoops_failure_does_not_replace_xnu();
    test_early_checkpoint_repeats_complete_frames_without_worker();
    test_hot_checkpoints_leave_quiet_time_after_an_attempt();
    test_checkpoint_multiframe_qr_worker_loops_without_new_snapshot();
    test_priority_publishes_latest_single_frame_before_possible_stall();
    test_text_checkpoint_only_updates_scroll_and_footer();
    test_text_checkpoint_refreshes_stale_frame();
    test_compact_frame_prefers_cdc_tail();
    free(frame_allocation); puts("PASS: 21 dual/live-checkpoint LiveXnu adapter groups (15 invalid video cases)"); return 0;
}
