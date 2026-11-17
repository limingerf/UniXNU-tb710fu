/* SPDX-License-Identifier: MIT
 * This includes the actual source produced by applying 0004, 0008 and 0013;
 * check_ramoops_snapshot.py supplies that isolated source directory. */
#define TB710FU_RAMOOPS_HOSTTEST 1
#include "osfmk/kern/tb710fu_ramoops.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    uint8_t before[64];
    uint8_t bytes[RAMOOPS_MAX_SIZE];
    uint8_t after[64];
} guarded;
static uint8_t copy[RAMOOPS_SNAPSHOT_MAX + 128];

static void reset_fixture(void)
{
    memset(&guarded, 0x5a, sizeof(guarded));
    memset(guarded.bytes, 0, sizeof(guarded.bytes));
    memset(copy, 0xcc, sizeof(copy));
    zone = NULL;
    data_size = ecc_size = ecc_blocks = wpos = wsize = 0;
    cur_block = UINT32_MAX;
    snapshot_lock = 0;
    boot_written = snapshot_dropped = 0;
    current_boot = 0;
    early_len = 0;
}

static void guards(void)
{
    for (size_t i = 0; i < sizeof(guarded.before); ++i) {
        assert(guarded.before[i] == 0x5a);
        assert(guarded.after[i] == 0x5a);
    }
}

static struct tb710fu_ramoops_snapshot take(size_t capacity)
{
    struct tb710fu_ramoops_snapshot info;
    int n = tb710fu_ramoops_snapshot_copy(copy, capacity, &info);
    assert(n >= 0 && n <= (int)RAMOOPS_SNAPSHOT_MAX);
    assert((uint32_t)n == info.copied);
    assert(info.boot_start == 0);
    assert(info.end >= info.start);
    assert(info.end - info.start == info.copied);
    assert((n > 0) == (info.state == TB710FU_RAMOOPS_READY));
    assert(snapshot_lock == 0);
    guards();
    return info;
}

static void open_empty(uint32_t ecc)
{
    assert(ramoops_zone_open(guarded.bytes, sizeof(guarded.bytes), ecc) == 0);
    ramoops_begin_current_boot();
}

static void test_unmapped_and_empty(void)
{
    reset_fixture();
    struct tb710fu_ramoops_snapshot info = take(sizeof(copy));
    assert(info.state == TB710FU_RAMOOPS_UNMAPPED);
    tb710fu_ramoops_putc('e');
    assert(early_len == 1);
    info = take(sizeof(copy));
    assert(info.state == TB710FU_RAMOOPS_UNMAPPED);
    open_empty(0);
    info = take(sizeof(copy));
    assert(info.state == TB710FU_RAMOOPS_EMPTY);
    assert(info.end == 0 && info.dropped == 0);
}

static void test_partial_line_read_only(void)
{
    static const char text[] = "partial line without LF";
    reset_fixture();
    open_empty(16);
    for (size_t i = 0; i < sizeof(text) - 1; ++i) {
        tb710fu_ramoops_putc(text[i]);
    }
    /* No LF was written, so the persistent header is still at its old end.
     * The snapshot must use live write pointers without flushing it. */
    assert(get32(zone + 4) == 0 && get32(zone + 8) == 0);
    uint8_t *before = malloc(sizeof(guarded.bytes));
    assert(before != NULL);
    memcpy(before, guarded.bytes, sizeof(guarded.bytes));
    struct tb710fu_ramoops_snapshot info = take(sizeof(copy));
    assert(info.start == 0 && info.end == sizeof(text) - 1);
    assert(memcmp(copy, text, sizeof(text) - 1) == 0);
    assert(memcmp(before, guarded.bytes, sizeof(guarded.bytes)) == 0);
    info = take(7);
    assert(info.start == sizeof(text) - 1 - 7 && info.copied == 7);
    assert(memcmp(copy, text + sizeof(text) - 1 - 7, 7) == 0);
    assert(memcmp(before, guarded.bytes, sizeof(guarded.bytes)) == 0);
    free(before);
}

static uint8_t pattern(uint64_t pos)
{
    return (uint8_t)(0x21 + (pos % 89));
}

static void test_wrap_and_ecc(uint32_t ecc)
{
    reset_fixture();
    open_empty(ecc);
    uint32_t ring_bytes = data_size;
    uint64_t total = (uint64_t)ring_bytes * 2 + 37;
    for (uint64_t i = 0; i < total; ++i) {
        tb710fu_ramoops_putc((char)pattern(i));
    }
    assert(wpos == 37 && boot_written == total);
    assert(data_size < RAMOOPS_MAX_SIZE - HDR_BYTES || ecc == 0);
    struct tb710fu_ramoops_snapshot info = take(sizeof(copy));
    assert(info.copied == RAMOOPS_SNAPSHOT_MAX);
    assert(info.start == total - RAMOOPS_SNAPSHOT_MAX && info.end == total);
    for (uint32_t i = 0; i < info.copied; ++i) {
        assert(copy[i] == pattern(info.start + i));
    }
    /* Even a larger caller buffer cannot include header/ECC or exceed 4 KiB. */
    assert(copy[RAMOOPS_SNAPSHOT_MAX] == 0xcc);
    info = take(1);
    assert(info.copied == 1 && copy[0] == pattern(total - 1));
}

static void test_small_ring_retention(void)
{
    reset_fixture();
    assert(ramoops_zone_open(guarded.bytes, 512, 16) == 0);
    ramoops_begin_current_boot();
    uint32_t retained = data_size;
    uint64_t total = retained + 17;
    for (uint64_t i = 0; i < total; ++i) {
        tb710fu_ramoops_putc((char)pattern(i));
    }
    struct tb710fu_ramoops_snapshot info = take(sizeof(copy));
    assert(info.start == total - retained && info.copied == retained);
    for (uint32_t i = 0; i < retained; ++i) {
        assert(copy[i] == pattern(info.start + i));
    }
}

static void test_old_zone_and_early_replay(void)
{
    static const char old[] = "OLD SECRET FROM A PREVIOUS BOOT\n";
    static const char banner[] = "\n=== TB710FU XNU boot (UniXNU ramoops) ===\n";
    static const char initial[] = "early-before-map";
    reset_fixture();
    put32(guarded.bytes, RAMOOPS_SIG);
    put32(guarded.bytes + 4, sizeof(old) - 1);
    put32(guarded.bytes + 8, sizeof(old) - 1);
    memcpy(guarded.bytes + HDR_BYTES, old, sizeof(old) - 1);
    for (size_t i = 0; i < sizeof(initial) - 1; ++i) {
        tb710fu_ramoops_putc(initial[i]);
    }
    assert(ramoops_start_boot(guarded.bytes, sizeof(guarded.bytes), 0) == 0);
    struct tb710fu_ramoops_snapshot info = take(sizeof(copy));
    assert(info.start == 0);
    assert(info.end == sizeof(banner) + sizeof(initial) - 2);
    assert(memcmp(copy, banner, sizeof(banner) - 1) == 0);
    assert(memcmp(copy + sizeof(banner) - 1, initial, sizeof(initial) - 1) == 0);
    assert(memcmp(guarded.bytes + HDR_BYTES, old, sizeof(old) - 1) == 0);
    assert(info.dropped == 0);
}

static void test_early_bound_and_drops(void)
{
    reset_fixture();
    for (size_t i = 0; i < sizeof(early) + 3; ++i) {
        tb710fu_ramoops_putc('a');
    }
    assert(early_len == sizeof(early) && snapshot_dropped == 3);
    assert(ramoops_start_boot(guarded.bytes, sizeof(guarded.bytes), 16) == 0);
    struct tb710fu_ramoops_snapshot info = take(sizeof(copy));
    assert(info.copied == sizeof(early) && info.dropped == 3);
    for (size_t i = 0; i < sizeof(early); ++i) {
        assert(copy[i] == 'a');
    }
}

static void test_busy_is_bounded(void)
{
    reset_fixture();
    open_empty(0);
    tb710fu_ramoops_putc('a');
    assert(ramoops_try_lock());
    tb710fu_ramoops_putc('X');
    assert(snapshot_dropped == 1 && boot_written == 1);
    struct tb710fu_ramoops_snapshot info;
    assert(tb710fu_ramoops_snapshot_copy(copy, sizeof(copy), &info) == 0);
    assert(info.state == TB710FU_RAMOOPS_BUSY && info.dropped == 1);
    assert(snapshot_dropped == 1); /* A busy reader loses no text. */
    assert(!ramoops_flush_try());
    ramoops_unlock();
    tb710fu_ramoops_putc('b');
    assert(ramoops_flush_try());
    info = take(sizeof(copy));
    assert(info.dropped == 1 && info.copied == 2);
    assert(memcmp(copy, "ab", 2) == 0);
}

static void test_invalid_inputs_and_geometry(void)
{
    reset_fixture();
    assert(ramoops_zone_open(NULL, sizeof(guarded.bytes), 0) == -1);
    assert(ramoops_zone_open(guarded.bytes, RAMOOPS_MAX_SIZE + 1, 0) == -1);
    assert(ramoops_zone_open(guarded.bytes, 0, 0) == -1);
    assert(ramoops_zone_open(guarded.bytes, HDR_BYTES + 2 * RS_BLOCK, 0) == -1);
    assert(ramoops_zone_open(guarded.bytes, sizeof(guarded.bytes), RS_MAX_ROOTS + 1) == -1);
    assert(zone == NULL);
    open_empty(RS_MAX_ROOTS);
    tb710fu_ramoops_putc('z');
    struct tb710fu_ramoops_snapshot info;
    assert(tb710fu_ramoops_snapshot_copy(NULL, sizeof(copy), &info) == 0);
    assert(info.state == TB710FU_RAMOOPS_UNMAPPED && !info.copied);
    assert(tb710fu_ramoops_snapshot_copy(copy, 0, &info) == 0);
    assert(info.state == TB710FU_RAMOOPS_UNMAPPED && !info.copied);
    assert(tb710fu_ramoops_snapshot_copy(copy, sizeof(copy), NULL) == 0);
    assert(snapshot_lock == 0);
    info = take(sizeof(copy));
    assert(info.copied == 1 && copy[0] == 'z');
    guards();
}

static void test_historical_ring_end_normalization(void)
{
    reset_fixture();
    uint32_t length = sizeof(guarded.bytes) - HDR_BYTES;
    put32(guarded.bytes, RAMOOPS_SIG);
    put32(guarded.bytes + 4, length);
    put32(guarded.bytes + 8, length);
    assert(ramoops_zone_open(guarded.bytes, sizeof(guarded.bytes), 0) == 0);
    assert(wpos == 0);
    ramoops_begin_current_boot();
    tb710fu_ramoops_putc('n');
    struct tb710fu_ramoops_snapshot info = take(sizeof(copy));
    assert(info.copied == 1 && copy[0] == 'n');
}

int main(void)
{
    test_unmapped_and_empty();
    test_partial_line_read_only();
    test_wrap_and_ecc(0);
    test_wrap_and_ecc(16);
    test_small_ring_retention();
    test_old_zone_and_early_replay();
    test_early_bound_and_drops();
    test_busy_is_bounded();
    test_invalid_inputs_and_geometry();
    test_historical_ring_end_normalization();
    puts("PASS: 10 production ramoops snapshot cases");
    return 0;
}
