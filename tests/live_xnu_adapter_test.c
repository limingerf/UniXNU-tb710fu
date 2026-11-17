/* SPDX-License-Identifier: MIT
 * Run the actual adapter/protocol/display control flow with host XNU services.
 * Hardware MRS/DSB and XNU includes alone are isolated by TB_LIVE_HOST_TEST.
 * This fixture never maps a device or claims scheduler/MMU/cache validation. */
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
typedef struct {
    uint64_t v_baseAddr, v_depth, v_width, v_height, v_rowBytes;
    uint64_t v_length, v_offset;
} PE_Video;
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
static unsigned arg_present, arg_value, map_fail, reenter_encoder;
static unsigned map_calls, snapshot_calls, qr_calls, wait_calls, block_calls;
static unsigned create_calls, deallocate_calls, deadline_calls, reentries;
static uint64_t clock_now, clock_frequency;
static uint8_t log_bytes[8192], encoded_frame[TB710FU_LIVE_LOG_FRAME_BYTES];
static size_t log_length, last_capacity;
static unsigned log_state;
static uint32_t qr_pixels[TB_LIVE_QR_SIDE * TB_LIVE_QR_SIDE];
static uint8_t *frame_allocation;
static size_t frame_bytes;
static void (*created_worker)(void *, wait_result_t);
static jmp_buf worker_escape;

static uint64_t tb_live_host_ticks(void) { return clock_now; }
static uint64_t tb_live_host_frequency(void) { return clock_frequency; }
static int PE_parse_boot_argn(const char *name, void *output, int bytes)
{
    assert(strcmp(name, "tb710fu-live-log") == 0);
    assert(bytes == (int)sizeof(int));
    *(int *)output = (int)arg_value;
    return (int)arg_present;
}
static int PE_current_console(PE_Video *output)
{
    *output = video_input;
    return 0;
}
static vm_offset_t ml_io_map(uint64_t physical, vm_size_t bytes)
{
    ++map_calls;
    assert(physical == (video_input.v_baseAddr & ~UINT64_C(3)));
    assert(bytes <= frame_bytes);
    return map_fail ? 0 : (vm_offset_t)(frame_allocation + 64);
}
int tb710fu_ramoops_snapshot_copy(uint8_t *output, size_t capacity,
                                struct tb710fu_ramoops_snapshot *info)
{
    ++snapshot_calls;
    last_capacity = capacity;
    assert(capacity <= TB710FU_LIVE_LOG_MAX_BYTES);
    *info = (struct tb710fu_ramoops_snapshot){0};
    info->state = log_state;
    if (log_state != TB710FU_RAMOOPS_READY || log_length == 0) return 0;
    size_t n = log_length < capacity ? log_length : capacity;
    info->start = log_length - n;
    info->end = log_length;
    info->copied = (uint32_t)n;
    memcpy(output, log_bytes + log_length - n, n);
    return (int)n;
}
int tb_live_qr_encode(const uint8_t *input, size_t bytes, const uint32_t **pixels)
{
    ++qr_calls;
    assert(bytes == TB710FU_LIVE_LOG_FRAME_BYTES);
    memcpy(encoded_frame, input, bytes);
    for (size_t i = 0; i < sizeof(qr_pixels) / sizeof(qr_pixels[0]); ++i)
        qr_pixels[i] = (input[i % bytes] & 1u) ? 0xff000000u : 0xffffffffu;
    *pixels = qr_pixels;
    if (reenter_encoder) {
        unsigned before = snapshot_calls;
        assert(guard == 1u);
        ++reentries;
        tb710fu_live_log_poll();
        assert(snapshot_calls == before);
    }
    return 1;
}
static kern_return_t kernel_thread_start(void (*function)(void *, wait_result_t),
                                        void *argument, thread_t *thread)
{
    assert(argument == NULL);
    ++create_calls;
    created_worker = function;
    *thread = &sleep_channel;
    return KERN_SUCCESS;
}
static void thread_deallocate(thread_t thread)
{
    assert(thread == &sleep_channel);
    ++deallocate_calls;
}
static void clock_interval_to_deadline(unsigned interval, unsigned scale, uint64_t *deadline)
{
    ++deadline_calls;
    assert(interval == 250 && scale == NSEC_PER_MSEC);
    *deadline = clock_now + 250;
}
static int assert_wait_deadline(event_t event, int state, uint64_t deadline)
{
    ++wait_calls;
    assert(event == (event_t)&sleep_channel && state == THREAD_UNINT);
    assert(deadline == clock_now + 250);
    return 0;
}
static void thread_block(void *continuation)
{
    assert(continuation == THREAD_CONTINUE_NULL);
    ++block_calls;
    longjmp(worker_escape, 1);
}

static void guards(void)
{
    for (size_t i = 0; i < 64; ++i) {
        assert(frame_allocation[i] == 0xa5);
        assert(frame_allocation[64 + frame_bytes + i] == 0xa5);
    }
}
static void reset_fixture(void)
{
    free(frame_allocation);
    frame_bytes = 640u * 4u * 480u;
    frame_allocation = malloc(frame_bytes + 128);
    assert(frame_allocation != NULL);
    memset(frame_allocation, 0xa5, frame_bytes + 128);
    display = (TB_LIVE_DISPLAY){0};
    memset(&live_sender, 0, sizeof(live_sender));
    memset(&fixed_sender, 0, sizeof(fixed_sender));
    memset(fixed_pixels, 0, sizeof(fixed_pixels));
    enabled = guard = redraw = worker_started = sleep_channel = fixed_valid = 0;
    frequency = next_frame = generation = 0;
    small_screen_phase = 0;
    arg_present = arg_value = 1;
    map_fail = reenter_encoder = 0;
    map_calls = snapshot_calls = qr_calls = wait_calls = block_calls = 0;
    create_calls = deallocate_calls = deadline_calls = reentries = 0;
    clock_now = 0;
    clock_frequency = 1000;
    log_state = TB710FU_RAMOOPS_READY;
    log_length = 100;
    last_capacity = 0;
    created_worker = NULL;
    for (size_t i = 0; i < sizeof(log_bytes); ++i) log_bytes[i] = (uint8_t)('!' + i % 89);
    video_input = (PE_Video){
        .v_baseAddr = 0xd5100000, .v_depth = 32, .v_width = 640,
        .v_height = 480, .v_rowBytes = 640 * 4,
        .v_length = frame_bytes, .v_offset = 0
    };
}
static uint32_t get_le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t get_le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static void assert_body(const struct tb710fu_live_log_sender *sender, size_t count)
{
    size_t offset = 26u + get_le16(sender->envelope + 6) + get_le16(sender->envelope + 8);
    assert(sender->info.bytes == count);
    assert(memcmp(sender->envelope + offset, log_bytes + log_length - count, count) == 0);
}

static void test_disabled_does_not_map(void)
{
    reset_fixture();
    arg_present = 0;
    tb710fu_live_log_init();
    tb710fu_live_log_poll();
    tb710fu_live_log_refresh();
    tb710fu_live_log_freeze();
    registered_startup();
    assert(!tb710fu_live_log_active());
    assert(!map_calls && !snapshot_calls && !qr_calls && !create_calls);
    arg_present = 1;
    arg_value = 2;
    tb710fu_live_log_init();
    assert(!map_calls);
    guards();
}
static void test_invalid_video_is_bounded(void)
{
    for (unsigned which = 0; which < 15; ++which) {
        reset_fixture();
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
        tb710fu_live_log_init();
        assert(!tb710fu_live_log_active());
        tb710fu_live_log_poll();
        assert(!snapshot_calls && !qr_calls && !create_calls);
        guards();
    }
}
static void test_poll_budget_and_no_scheduler_dependency(void)
{
    reset_fixture();
    log_length = 5000;
    tb710fu_live_log_init();
    assert(tb710fu_live_log_active() && map_calls == 1);
    assert(last_capacity == 512 && live_sender.info.bytes == 512);
    assert(snapshot_calls == 1 && qr_calls == 1);
    assert_body(&live_sender, 512);
    for (unsigned i = 0; i < 100; ++i) tb710fu_live_log_poll();
    assert(snapshot_calls == 1 && qr_calls == 1);
    clock_now = 500;
    tb710fu_live_log_poll();
    assert(snapshot_calls == 2 && qr_calls == 2);
    assert(!create_calls && !wait_calls && !block_calls && !deadline_calls);
    assert(guard == 0);
    guards();
}
static void test_fixed_k1_is_immutable(void)
{
    reset_fixture();
    log_length = 5000;
    tb710fu_live_log_init();
    tb710fu_live_log_freeze();
    assert(fixed_valid && fixed_sender.info.block_count == 1);
    assert(get_le32(encoded_frame + 12) == 1);
    assert(fixed_sender.info.generation == 0);
    assert(fixed_sender.info.start == log_length - 512 && fixed_sender.info.end == log_length);
    assert_body(&fixed_sender, 512);
    struct tb710fu_live_log_sender saved = fixed_sender;
    uint32_t *saved_pixels = malloc(sizeof(fixed_pixels));
    assert(saved_pixels);
    memcpy(saved_pixels, fixed_pixels, sizeof(fixed_pixels));
    memset(log_bytes, 'Z', sizeof(log_bytes));
    log_length = 7000;
    for (unsigned i = 0; i < 20; ++i) {
        clock_now += 1000;
        tb710fu_live_log_poll();
        tb710fu_live_log_freeze();
        assert(memcmp(&saved, &fixed_sender, sizeof(saved)) == 0);
        assert(memcmp(saved_pixels, fixed_pixels, sizeof(fixed_pixels)) == 0);
    }
    assert(live_sender.info.end == log_length);
    assert_body(&live_sender, 512);
    free(saved_pixels);
    guards();
}
static void test_render_try_lock_reentry(void)
{
    reset_fixture();
    reenter_encoder = 1;
    tb710fu_live_log_init();
    assert(reentries == 1 && snapshot_calls == 1 && qr_calls == 1 && !guard);
    guard = 1;
    tb710fu_live_log_poll();
    tb710fu_live_log_freeze();
    tb710fu_live_log_refresh();
    assert(snapshot_calls == 1 && qr_calls == 1 && guard == 1);
    guard = 0;
    tb710fu_live_log_poll(); /* refresh remains pending; the frame is bounded. */
    assert(snapshot_calls == 2 && qr_calls == 2 && reentries == 2 && !guard);
    assert(!wait_calls && !block_calls && !create_calls);
    guards();
}
static void test_worker_only_from_registered_early_boot(void)
{
    reset_fixture();
    log_length = 5000;
    tb710fu_live_log_init();
    assert(registered_subsystem == EARLY_BOOT && registered_rank == STARTUP_RANK_FIRST);
    assert(!worker_started && !create_calls);
    registered_startup();
    assert(create_calls == 1 && deallocate_calls == 1 && created_worker != NULL);
    assert(!worker_started); /* Creating a thread does not prove it ran. */
    clock_now = 1000;
    if (!setjmp(worker_escape)) created_worker(NULL, 0);
    assert(worker_started == 1 && last_capacity == 4096);
    assert(wait_calls == 1 && block_calls == 1 && deadline_calls == 1);
    for (unsigned i = 0; i < 3; ++i) {
        clock_now += 1000;
        tb710fu_live_log_poll();
    }
    assert(live_sender.info.bytes == 4096);
    assert_body(&live_sender, 4096);
    assert(wait_calls == 1 && block_calls == 1 && create_calls == 1);
    guards();
}
static void test_unmapped_busy_and_empty_remain_observable(void)
{
    for (unsigned state = TB710FU_RAMOOPS_UNMAPPED; state <= TB710FU_RAMOOPS_EMPTY; ++state) {
        reset_fixture();
        log_state = state;
        tb710fu_live_log_init();
        assert(tb710fu_live_log_active());
        assert(snapshot_calls == 1 && !qr_calls && !live_sender.valid);
        tb710fu_live_log_freeze();
        assert(!fixed_valid && !fixed_sender.valid && !guard);
        assert(!create_calls && !wait_calls);
        guards();
    }
}

int main(void)
{
    test_disabled_does_not_map();
    test_invalid_video_is_bounded();
    test_poll_budget_and_no_scheduler_dependency();
    test_fixed_k1_is_immutable();
    test_render_try_lock_reentry();
    test_worker_only_from_registered_early_boot();
    test_unmapped_busy_and_empty_remain_observable();
    free(frame_allocation);
    puts("PASS: 7 native LiveXnu adapter control-flow groups (15 invalid video cases)");
    return 0;
}
