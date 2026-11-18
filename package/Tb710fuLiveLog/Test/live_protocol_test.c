/* SPDX-License-Identifier: MIT */
#include "LiveProtocol.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct tb710fu_live_log_sender sender, saved;
static uint8_t bytes[TB710FU_LIVE_LOG_MAX_BYTES + 1u];
static struct tb710fu_live_capture capture;
static uint8_t early_backup[512];
static const char file_metadata[] =
    "TB710FU XNU debug export\ncurrent range and early backup are independent\n"
    "early-bytes=512; current-bytes=3072\n\n";

static void test_capture(void)
{
    uint8_t tail[TB710FU_LIVE_LOG_MAX_BYTES + 8u];
    struct tb710fu_live_capture_info info;
    memset(tail, 0xa5, sizeof(tail));
    assert(!tb710fu_live_capture_putc(NULL, 'x'));
    assert(!tb710fu_live_capture_copy(NULL, tail, sizeof(tail), &info) && info.state == 0);
    assert(!tb710fu_live_capture_copy(&capture, NULL, sizeof(tail), &info) && info.state == 0);
    assert(!tb710fu_live_capture_copy(&capture, tail, 0, &info) && info.state == 0);
    assert(!tb710fu_live_capture_copy(&capture, tail, sizeof(tail), NULL));
    assert(!tb710fu_live_capture_copy(&capture, tail, sizeof(tail), &info));
    assert(info.state == 2 && info.start == 0 && info.end == 0 && info.copied == 0 && info.dropped == 0);
    for (size_t i = 0; i < sizeof(tail); ++i) assert(tail[i] == 0xa5);
    assert(tb710fu_live_capture_putc(&capture, (char)0x80));
    assert(tb710fu_live_capture_copy(&capture, tail, sizeof(tail), &info) == 1);
    assert(info.state == 3 && info.start == 0 && info.end == 1 && info.copied == 1 && tail[0] == 0x80);
    /* Busy writers drop immediately; a busy reader neither writes output nor
     * counts a dropped source byte. The live writer owns the byte stream. */
    __atomic_store_n(&capture.guard, 1u, __ATOMIC_RELEASE);
    assert(!tb710fu_live_capture_putc(&capture, 'x'));
    memset(tail, 0xa5, sizeof(tail));
    assert(!tb710fu_live_capture_copy(&capture, tail, sizeof(tail), &info));
    assert(info.state == 1 && info.dropped == 1 && info.copied == 0);
    assert(capture.written == 1 && capture.guard == 1);
    for (size_t i = 0; i < sizeof(tail); ++i) assert(tail[i] == 0xa5);
    __atomic_store_n(&capture.guard, 0u, __ATOMIC_RELEASE);
    uint64_t total = TB710FU_LIVE_CAPTURE_BYTES * 2u + 1337u;
    for (uint64_t i = 1; i < total; ++i)
        assert(tb710fu_live_capture_putc(&capture, (char)(uint8_t)(i * 37u)));
    assert(tb710fu_live_capture_copy(&capture, tail, sizeof(tail), &info) == TB710FU_LIVE_LOG_MAX_BYTES);
    assert(info.state == 3 && info.start == total - TB710FU_LIVE_LOG_MAX_BYTES && info.end == total &&
           info.copied == TB710FU_LIVE_LOG_MAX_BYTES && info.dropped == 1);
    for (uint64_t i = info.start; i < info.end; ++i)
        assert(tail[i - info.start] == (uint8_t)(i * 37u));
    for (size_t i = TB710FU_LIVE_LOG_MAX_BYTES; i < sizeof(tail); ++i) assert(tail[i] == 0xa5);
    assert(tb710fu_live_capture_copy(&capture, tail, 17, &info) == 17);
    assert(info.start == total - 17 && info.end == total && info.dropped == 1);
    for (size_t i = 0; i < 17; ++i) assert(tail[i] == (uint8_t)((info.start + i) * 37u));
    /* Saturating the absolute position refuses another write rather than
     * wrapping the offset back into a different boot's apparent byte range. */
    capture.written = UINT64_MAX - 1u;
    assert(tb710fu_live_capture_putc(&capture, 'z'));
    assert(!tb710fu_live_capture_putc(&capture, 'x'));
    assert(capture.written == UINT64_MAX && capture.dropped == 2 && capture.guard == 0);
    assert(tb710fu_live_capture_copy(&capture, tail, 1, &info) == 1);
    assert(info.start == UINT64_MAX - 1u && info.end == UINT64_MAX && info.dropped == 2 && tail[0] == 'z');
}

static void test_file_bounds(void)
{
    uint8_t frame[TB710FU_LIVE_LOG_FRAME_BYTES];
    struct tb710fu_live_log_info info;
    tb710fu_live_log_sender_init(&sender);
    assert(sender.source == TB710FU_LIVE_LOG_RAMOOPS);
    saved = sender;
    assert(!tb710fu_live_log_set_source(NULL, TB710FU_LIVE_LOG_XNU));
    assert(!tb710fu_live_log_set_source(&sender, (enum tb710fu_live_log_source)-1));
    assert(!tb710fu_live_log_set_source(&sender, (enum tb710fu_live_log_source)2));
    assert(memcmp(&sender, &saved, sizeof(sender)) == 0);
    assert(tb710fu_live_log_set_source(&sender, TB710FU_LIVE_LOG_XNU));
    assert(sender.source == TB710FU_LIVE_LOG_XNU);
    saved = sender;
    assert(tb710fu_live_log_snapshot_file(&sender, bytes, sizeof(bytes), 1, 0, 1) == TB710FU_LIVE_LOG_INVALID);
    assert(tb710fu_live_log_snapshot_file(&sender, NULL, 1, 1, 0, 1) == TB710FU_LIVE_LOG_INVALID);
    assert(tb710fu_live_log_snapshot_file(&sender, bytes, 1, 1, 2, 1) == TB710FU_LIVE_LOG_INVALID);
    assert(tb710fu_live_log_snapshot_file(&sender, bytes, 1, 1, 0, 2) == TB710FU_LIVE_LOG_INVALID);
    assert(tb710fu_live_log_snapshot_file(&sender, NULL, 0, 1, 9, 9) == TB710FU_LIVE_LOG_EMPTY);
    assert(memcmp(&sender, &saved, sizeof(sender)) == 0);
    sender.source = (enum tb710fu_live_log_source)2;
    saved = sender;
    assert(tb710fu_live_log_snapshot_file(&sender, bytes, 1, 1, 0, 1) == TB710FU_LIVE_LOG_INVALID);
    assert(memcmp(&sender, &saved, sizeof(sender)) == 0);
    sender.source = TB710FU_LIVE_LOG_XNU;
    /* A backup-only file honestly names an empty current source range. */
    assert(tb710fu_live_log_snapshot_file(&sender, bytes, 17, 1, 900, 900) == TB710FU_LIVE_LOG_READY);
    assert(tb710fu_live_log_get_info(&sender, &info) && info.source == TB710FU_LIVE_LOG_XNU && info.bytes == 17);
    saved = sender;
    assert(tb710fu_live_log_set_source(&sender, TB710FU_LIVE_LOG_XNU));
    assert(!tb710fu_live_log_set_source(&sender, TB710FU_LIVE_LOG_RAMOOPS));
    assert(tb710fu_live_log_snapshot_file(&sender, bytes, 1, 2, 900, 900) == TB710FU_LIVE_LOG_BUSY);
    assert(memcmp(&sender, &saved, sizeof(sender)) == 0);
    for (unsigned round = 0; round < TB710FU_LIVE_LOG_MIN_ROUNDS; ++round) {
        assert(tb710fu_live_log_next_frame(&sender, frame));
        if (round + 1u < TB710FU_LIVE_LOG_MIN_ROUNDS)
            assert(!tb710fu_live_log_set_source(&sender, TB710FU_LIVE_LOG_RAMOOPS));
    }
    assert(tb710fu_live_log_set_source(&sender, TB710FU_LIVE_LOG_RAMOOPS));
    assert(!sender.valid && sender.source == TB710FU_LIVE_LOG_RAMOOPS && !tb710fu_live_log_get_info(&sender, &info));
    for (size_t i = 0; i < sizeof(sender.envelope); ++i) assert(sender.envelope[i] == 0);
}

static FILE *output(const char *directory, const char *filename)
{
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, filename);
    assert(length > 0 && (size_t)length < sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    return file;
}

static void write_bytes(const char *directory, const char *name,
    const uint8_t *data, size_t length)
{
    FILE *file = output(directory, name);
    assert(fwrite(data, 1, length, file) == length);
    assert(fclose(file) == 0);
}

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void test_single_frame_files(const char *directory)
{
    uint8_t frame[TB710FU_LIVE_LOG_FRAME_BYTES];
    assert(!tb710fu_live_log_single_file_capacity((enum tb710fu_live_log_source)2, 0, 0, 0));
    assert(!tb710fu_live_log_single_file_capacity(TB710FU_LIVE_LOG_XNU, 0, 2, 1));
    assert(tb710fu_live_log_snapshot_single_file(NULL, bytes, 1, 0, 0, 0) == TB710FU_LIVE_LOG_INVALID);
    for (unsigned source = 0; source < 2; ++source) {
        enum tb710fu_live_log_source kind = (enum tb710fu_live_log_source)source;
        size_t capacity = tb710fu_live_log_single_file_capacity(kind, 0, UINT64_MAX, UINT64_MAX);
        size_t worst = tb710fu_live_log_single_file_capacity(kind, UINT64_MAX, UINT64_MAX, UINT64_MAX);
        assert(worst >= 512 && capacity >= worst && capacity < TB710FU_LIVE_LOG_BLOCK_BYTES);
        for (size_t i = 0; i <= capacity; ++i) bytes[i] = (uint8_t)(i * 17u + source);
        tb710fu_live_log_sender_init(&sender);
        assert(tb710fu_live_log_set_source(&sender, kind));
        assert(tb710fu_live_log_can_snapshot_single(&sender));
        saved = sender;
        assert(tb710fu_live_log_snapshot_single_file(&sender, bytes, capacity + 1u,
            0, UINT64_MAX, UINT64_MAX) == TB710FU_LIVE_LOG_INVALID);
        assert(memcmp(&sender, &saved, sizeof(sender)) == 0);
        assert(tb710fu_live_log_snapshot_single_file(&sender, bytes, capacity,
            0, UINT64_MAX, UINT64_MAX) == TB710FU_LIVE_LOG_READY);
        assert(sender.info.block_count == 1 && sender.info.envelope_bytes == TB710FU_LIVE_LOG_BLOCK_BYTES);
        saved = sender;
        assert(tb710fu_live_log_snapshot_single_file(&sender, bytes, capacity,
            0, UINT64_MAX, UINT64_MAX) == TB710FU_LIVE_LOG_BUSY);
        assert(memcmp(&sender, &saved, sizeof(sender)) == 0);
        assert(!tb710fu_live_log_can_snapshot(&sender) &&
               !tb710fu_live_log_can_snapshot_single(&sender));
        char name[64];
        assert(snprintf(name, sizeof(name), "single-%u.expected", source) > 0);
        write_bytes(directory, name, bytes, capacity);
        assert(snprintf(name, sizeof(name), "single-%u.envelope", source) > 0);
        write_bytes(directory, name, sender.envelope, sender.info.envelope_bytes);
        assert(tb710fu_live_log_next_frame(&sender, frame));
        assert(snprintf(name, sizeof(name), "single-%u.frame", source) > 0);
        write_bytes(directory, name, frame, sizeof(frame));
        /* A one-block file is fully described by one photograph, so the
         * pre-worker publisher uses the short single-frame window while the
         * multi-frame gate keeps protecting the very same transfer. */
        assert(TB710FU_LIVE_LOG_SINGLE_FRAME_ROUNDS < TB710FU_LIVE_LOG_MIN_ROUNDS);
        assert(!tb710fu_live_log_can_snapshot_single(&sender) &&
               !tb710fu_live_log_can_snapshot(&sender));
        for (unsigned round = 1; round < TB710FU_LIVE_LOG_SINGLE_FRAME_ROUNDS; ++round) {
            assert(tb710fu_live_log_next_frame(&sender, frame));
            int open = round + 1u >= TB710FU_LIVE_LOG_SINGLE_FRAME_ROUNDS;
            assert(tb710fu_live_log_can_snapshot_single(&sender) == open);
            assert(!tb710fu_live_log_can_snapshot(&sender));
        }
        assert(tb710fu_live_log_can_snapshot_single(&sender));
        assert(tb710fu_live_log_snapshot_single_file(&sender, bytes, capacity,
            0, UINT64_MAX, UINT64_MAX) == TB710FU_LIVE_LOG_READY);
        assert(tb710fu_live_log_snapshot_file(&sender, bytes, capacity,
            0, UINT64_MAX, UINT64_MAX) == TB710FU_LIVE_LOG_BUSY);
    }
}

static enum tb710fu_live_log_result snapshot_case(unsigned test,
    const uint8_t *body, size_t length, uint64_t generation,
    uint64_t start, uint64_t end)
{
    return test >= 5 ? tb710fu_live_log_snapshot_file(&sender, body, length, generation, start, end) :
        tb710fu_live_log_snapshot(&sender, body, length, generation, start, end);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *directory = argv[1];
    test_capture();
    test_file_bounds();
    test_single_frame_files(directory);
    FILE *capture_report = output(directory, "capture-check.json");
    assert(fputs("{\"result\":\"PASS\",\"capacity_bytes\":65536,\"tail_limit\":4096,"
        "\"zero_init_empty\":true,\"two_wraps_and_boundary_tail\":true,"
        "\"busy_writer_drops_without_wait\":true,\"busy_reader_preserves_output\":true,"
        "\"absolute_position_saturates\":true,\"invalid_outputs_preserved\":true,"
        "\"source_enum_and_16_round_gate\":true,\"file_body_range_bounds\":true}\n", capture_report) >= 0);
    assert(fclose(capture_report) == 0);
    uint8_t frame[TB710FU_LIVE_LOG_FRAME_BYTES];
    struct tb710fu_live_log_info info;
    tb710fu_live_log_sender_init(NULL);
    assert(!tb710fu_live_log_can_snapshot(NULL));
    assert(!tb710fu_live_log_next_frame(NULL, frame));
    assert(!tb710fu_live_log_get_info(NULL, &info));
    tb710fu_live_log_sender_init(&sender);
    assert(tb710fu_live_log_can_snapshot(&sender));
    assert(!tb710fu_live_log_get_info(&sender, &info));
    assert(!tb710fu_live_log_next_frame(&sender, frame));
    saved = sender;
    assert(tb710fu_live_log_snapshot(&sender, NULL, 0, 0, 10, 10) == TB710FU_LIVE_LOG_EMPTY);
    assert(tb710fu_live_log_snapshot(&sender, NULL, 1, 0, 0, 1) == TB710FU_LIVE_LOG_INVALID);
    assert(tb710fu_live_log_snapshot(&sender, bytes, sizeof(bytes), 0, 0, sizeof(bytes)) == TB710FU_LIVE_LOG_INVALID);
    assert(tb710fu_live_log_snapshot(&sender, bytes, 1, 0, 2, 1) == TB710FU_LIVE_LOG_INVALID);
    assert(tb710fu_live_log_snapshot(&sender, bytes, 1, 0, 0, 2) == TB710FU_LIVE_LOG_INVALID);
    assert(memcmp(&sender, &saved, sizeof(sender)) == 0);

    const size_t prefix_length = sizeof(file_metadata) - 1u;
    const size_t lengths[] = {17, 900, TB710FU_LIVE_LOG_MAX_BYTES, 512, 17,
                             sizeof(file_metadata) - 1u + 512u + 3072u,
                             sizeof(file_metadata) - 1u + 512u};
    const uint32_t blocks[] = {1, 2, 6, 1, 1, 6, 2};
    for (size_t i = 0; i < sizeof(early_backup); ++i) early_backup[i] = (uint8_t)(i * 13u + 7u);
    write_bytes(directory, "early-backup.expected", early_backup, sizeof(early_backup));
    FILE *manifest = output(directory, "cases.json");
    assert(fputs("{\"cases\":[", manifest) >= 0);
    for (unsigned test = 0; test < sizeof(lengths) / sizeof(lengths[0]); ++test) {
        size_t length = lengths[test];
        uint64_t generation = test == 2 ? UINT64_MAX : test == 3 ? 0 : test == 4 ? 1 : test + 1u;
        size_t source_length = test == 5 ? 3072u : test == 6 ? 0u : length;
        uint64_t start = test == 2 || test == 3 || test == 6 ? UINT64_MAX - source_length :
                         test == 4 ? 100000u : 100000u + test * 10000u;
        uint64_t end = start + source_length;
        for (size_t index = 0; index < length; ++index)
            bytes[index] = (uint8_t)((index * 37u + (test == 4 ? 0 : test) * 11u) & 255u);
        if (test >= 5) {
            memcpy(bytes, file_metadata, prefix_length);
            memcpy(bytes + prefix_length, early_backup, sizeof(early_backup));
        }
        char name[128];
        int count = snprintf(name, sizeof(name), "case-%u.expected", test);
        assert(count > 0 && (size_t)count < sizeof(name));
        write_bytes(directory, name, bytes, length);
        tb710fu_live_log_sender_init(&sender);
        if (test >= 4) assert(tb710fu_live_log_set_source(&sender, TB710FU_LIVE_LOG_XNU));
        assert(snapshot_case(test, bytes, length, generation, start, end) == TB710FU_LIVE_LOG_READY);
        assert(tb710fu_live_log_get_info(&sender, &info));
        assert(info.generation == generation && info.start == start && info.end == end && info.bytes == length);
        assert(info.source == (test >= 4 ? TB710FU_LIVE_LOG_XNU : TB710FU_LIVE_LOG_RAMOOPS));
        assert(info.rounds == 0 && !tb710fu_live_log_can_snapshot(&sender));
        assert(info.block_count == blocks[test]);
        if (generation == 0) {
            assert(strncmp(info.filename, "TB710FU-ramoops-fixed-0-",
                           sizeof("TB710FU-ramoops-fixed-0-") - 1u) == 0);
            assert(info.envelope_bytes <= TB710FU_LIVE_LOG_BLOCK_BYTES);
        } else if (test < 4) {
            assert(strncmp(info.filename, "TB710FU-ramoops-live-",
                           sizeof("TB710FU-ramoops-live-") - 1u) == 0);
        }
        count = snprintf(name, sizeof(name), "case-%u.envelope", test);
        assert(count > 0 && (size_t)count < sizeof(name));
        write_bytes(directory, name, sender.envelope, info.envelope_bytes);
        /* Caller storage may immediately change: only the captured bytes count. */
        memset(bytes, 0xa5, length);
        count = snprintf(name, sizeof(name), "case-%u.frames", test);
        assert(count > 0 && (size_t)count < sizeof(name));
        FILE *frames = output(directory, name);
        uint32_t receiver_frames = info.block_count * 4u;
        uint32_t stable_frames = info.block_count * TB710FU_LIVE_LOG_MIN_ROUNDS;
        for (uint32_t index = 0; index < stable_frames; ++index) {
            saved = sender;
            if (index + 1u < stable_frames) {
                assert(snapshot_case(test, bytes, length, generation + 1u, start, end) == TB710FU_LIVE_LOG_BUSY);
                assert(memcmp(&sender, &saved, sizeof(sender)) == 0);
            }
            assert(tb710fu_live_log_next_frame(&sender, frame));
            uint32_t sequence = read32(frame + 8);
            assert(sequence % 5u != 4u);
            uint32_t block = (((uint64_t)(sequence / 5u) * 4u + sequence % 5u) % info.block_count);
            assert(block == index % info.block_count);
            assert(read32(frame + 12) == info.block_count);
            assert(read32(frame + 20) == info.envelope_bytes);
            assert(read32(frame + 24) == info.payload_checksum);
            if (index < receiver_frames)
                assert(fwrite(frame, 1, sizeof(frame), frames) == sizeof(frame));
            assert(tb710fu_live_log_get_info(&sender, &info));
            assert(info.rounds == (index + 1u) / info.block_count);
            assert(tb710fu_live_log_can_snapshot(&sender) == (index + 1u >= stable_frames));
        }
        assert(fclose(frames) == 0);
        assert(fprintf(manifest,
            "%s{\"id\":%u,\"bytes\":%zu,\"generation\":\"%" PRIu64 "\","
            "\"start\":\"%" PRIu64 "\",\"end\":\"%" PRIu64 "\","
            "\"filename\":\"%s\",\"source\":\"%s\",\"source_bytes\":%zu,"
            "\"file_body\":%s,\"backup_offset\":%zu,\"backup_bytes\":%zu,"
            "\"block_count\":%u,\"frames\":%u}",
            test ? "," : "", test, length, generation, start, end, info.filename,
            test >= 4 ? "xnu" : "ramoops", source_length, test >= 5 ? "true" : "false",
            test >= 5 ? prefix_length : 0u, test >= 5 ? sizeof(early_backup) : 0u,
            info.block_count, info.block_count * 4u) > 0);
        saved = sender;
        assert(tb710fu_live_log_snapshot(&sender, NULL, 0, 0, 0, 0) == TB710FU_LIVE_LOG_EMPTY);
        assert(memcmp(&sender, &saved, sizeof(sender)) == 0);
        if (test >= 5) {
            /* Every exported replacement keeps the original early bytes,
             * independently of the changing live tail/caller storage. */
            memcpy(bytes, file_metadata, prefix_length);
            memcpy(bytes + prefix_length, early_backup, sizeof(early_backup));
        }
        assert(snapshot_case(test, bytes, length, 0, start, end) == TB710FU_LIVE_LOG_READY);
        assert(tb710fu_live_log_get_info(&sender, &info) && info.rounds == 0 && info.next_sequence == 0);
        assert(!tb710fu_live_log_next_frame(&sender, NULL));
        count = snprintf(name, sizeof(name), "case-%u.updated-expected", test);
        assert(count > 0 && (size_t)count < sizeof(name));
        write_bytes(directory, name, bytes, length);
        count = snprintf(name, sizeof(name), "case-%u.updated-frames", test);
        assert(count > 0 && (size_t)count < sizeof(name));
        frames = output(directory, name);
        for (uint32_t index = 0; index < info.block_count; ++index) {
            assert(tb710fu_live_log_next_frame(&sender, frame));
            assert(fwrite(frame, 1, sizeof(frame), frames) == sizeof(frame));
        }
        assert(fclose(frames) == 0);
    }
    assert(fputs("]}\n", manifest) >= 0);
    assert(fclose(manifest) == 0);
    puts("Live sender/capture: zero-init 64KiB ring, wrap/busy/drop, independent sources, exact backup files and OneSend frames PASS");
    return EXIT_SUCCESS;
}
