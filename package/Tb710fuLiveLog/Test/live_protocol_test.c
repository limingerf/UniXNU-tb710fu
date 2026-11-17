/* SPDX-License-Identifier: MIT */
#include "LiveProtocol.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct tb710fu_live_log_sender sender, saved;
static uint8_t bytes[TB710FU_LIVE_LOG_MAX_BYTES + 1u];

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

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *directory = argv[1];
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

    const size_t lengths[] = {17, 900, TB710FU_LIVE_LOG_MAX_BYTES, 512};
    FILE *manifest = output(directory, "cases.json");
    assert(fputs("{\"cases\":[", manifest) >= 0);
    for (unsigned test = 0; test < sizeof(lengths) / sizeof(lengths[0]); ++test) {
        size_t length = lengths[test];
        uint64_t generation = test == 2 ? UINT64_MAX : test == 3 ? 0 : test + 1u;
        uint64_t start = test >= 2 ? UINT64_MAX - length : 100000u + test * 10000u;
        uint64_t end = start + length;
        for (size_t index = 0; index < length; ++index)
            bytes[index] = (uint8_t)((index * 37u + test * 11u) & 255u);
        char name[128];
        int count = snprintf(name, sizeof(name), "case-%u.expected", test);
        assert(count > 0 && (size_t)count < sizeof(name));
        write_bytes(directory, name, bytes, length);
        tb710fu_live_log_sender_init(&sender);
        assert(tb710fu_live_log_snapshot(&sender, bytes, length, generation, start, end) == TB710FU_LIVE_LOG_READY);
        assert(tb710fu_live_log_get_info(&sender, &info));
        assert(info.generation == generation && info.start == start && info.end == end && info.bytes == length);
        assert(info.rounds == 0 && !tb710fu_live_log_can_snapshot(&sender));
        assert(info.block_count == (test == 1 ? 2u : test == 2 ? 6u : 1u));
        if (generation == 0) {
            assert(strncmp(info.filename, "TB710FU-ramoops-fixed-0-",
                           sizeof("TB710FU-ramoops-fixed-0-") - 1u) == 0);
            assert(info.envelope_bytes <= TB710FU_LIVE_LOG_BLOCK_BYTES);
        } else {
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
        for (uint32_t index = 0; index < info.block_count * 4u; ++index) {
            saved = sender;
            if (index < info.block_count * 2u) {
                assert(tb710fu_live_log_snapshot(&sender, bytes, length, generation + 1u, start, end) == TB710FU_LIVE_LOG_BUSY);
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
            assert(fwrite(frame, 1, sizeof(frame), frames) == sizeof(frame));
            assert(tb710fu_live_log_get_info(&sender, &info));
            assert(info.rounds == (index + 1u) / info.block_count);
            assert(tb710fu_live_log_can_snapshot(&sender) == (index + 1u >= info.block_count * 2u));
        }
        assert(fclose(frames) == 0);
        assert(fprintf(manifest,
            "%s{\"id\":%u,\"bytes\":%zu,\"generation\":\"%" PRIu64 "\","
            "\"start\":\"%" PRIu64 "\",\"end\":\"%" PRIu64 "\","
            "\"filename\":\"%s\",\"block_count\":%u,\"frames\":%u}",
            test ? "," : "", test, length, generation, start, end, info.filename,
            info.block_count, info.block_count * 4u) > 0);
        saved = sender;
        assert(tb710fu_live_log_snapshot(&sender, NULL, 0, 0, 0, 0) == TB710FU_LIVE_LOG_EMPTY);
        assert(memcmp(&sender, &saved, sizeof(sender)) == 0);
        assert(tb710fu_live_log_snapshot(&sender, bytes, length, 0, start, end) == TB710FU_LIVE_LOG_READY);
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
    puts("Live OneSend sender: immutable snapshots, offsets, empty/bounds, two-round gate and systematic frames PASS");
    return EXIT_SUCCESS;
}
