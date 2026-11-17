/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "LiveQr.h"
#include "LiveProtocol.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* These are the production adapter's private arena services, not substitutes. */
extern void *tb_live_qr_AllocatePool(size_t bytes);
extern void *tb_live_qr_AllocateZeroPool(size_t bytes);
extern void *tb_live_qr_AllocateCopyPool(size_t bytes, const void *source);
extern void tb_live_qr_FreePool(void *pointer);

static uint8_t frame[TB710FU_LIVE_LOG_FRAME_BYTES];
static uint8_t body[TB710FU_LIVE_LOG_MAX_BYTES];
static struct tb710fu_live_log_sender sender;

static void write_file(const char *directory, const char *name,
                       const char *extension, const void *bytes, size_t length)
{
    char path[4096];
    FILE *output;
    int needed = snprintf(path, sizeof(path), "%s/%s.%s", directory, name, extension);
    assert(needed > 0 && (size_t)needed < sizeof(path));
    output = fopen(path, "wb");
    assert(output);
    assert(fwrite(bytes, 1, length, output) == length);
    assert(fclose(output) == 0);
}

static void encode(void)
{
    const uint32_t *pixels = NULL;
    assert(tb_live_qr_encode(frame, sizeof(frame), &pixels));
    assert(pixels && !((uintptr_t)pixels & 3U));
    for (size_t i = 0; i < 121U * 121U; ++i) {
        uint32_t color = pixels[i] & UINT32_C(0xffffff);
        assert(color == 0 || color == UINT32_C(0xffffff));
    }
}

static void fixture(const char *directory, const char *name, size_t length, uint64_t generation)
{
    struct tb710fu_live_log_info info;
    char filename[128];
    for (size_t i = 0; i < length; ++i)
        body[i] = i % 71U == 70U ? '\n' : (uint8_t)(' ' + (i * 37U + length) % 95U);
    tb710fu_live_log_sender_init(&sender);
    assert(tb710fu_live_log_snapshot(&sender, body, length, generation,
                                    UINT64_C(0x100000000), UINT64_C(0x100000000) + length) == TB710FU_LIVE_LOG_READY);
    assert(tb710fu_live_log_get_info(&sender, &info));
    if (!generation)
        assert(info.block_count == 1U);
    else
        assert(info.block_count == 6U);
    write_file(directory, name, "log", body, length);
    for (unsigned i = 0; i < info.block_count * 2U; ++i) {
        const uint32_t *pixels;
        assert(tb710fu_live_log_next_frame(&sender, frame));
        assert(tb_live_qr_encode(frame, sizeof(frame), &pixels));
        assert(snprintf(filename, sizeof(filename), "%s-%02u", name, i) > 0);
        write_file(directory, filename, "bin", frame, sizeof(frame));
        write_file(directory, filename, "bgra", pixels, 121U * 121U * sizeof(*pixels));
    }
}

int main(int argc, char **argv)
{
    const uint32_t *pixels = (const uint32_t *)(uintptr_t)1;
    uint32_t reference[121U * 121U];
    uint8_t source[31];
    size_t total = 0, allocations = 0;
    assert(argc == 2);
    assert(!tb_live_qr_encode(NULL, sizeof(frame), &pixels) && !pixels);
    assert(!tb_live_qr_encode(frame, sizeof(frame) - 1U, &pixels) && !pixels);
    assert(!tb_live_qr_encode(frame, sizeof(frame) + 1U, &pixels) && !pixels);
    assert(!tb_live_qr_encode(frame, sizeof(frame), NULL));
    assert(!tb_live_qr_encode(frame, SIZE_MAX, &pixels) && !pixels);

    for (size_t i = 0; i < sizeof(frame); ++i)
        frame[i] = (uint8_t)i;
    assert(tb_live_qr_encode(frame, sizeof(frame), &pixels));
    memcpy(reference, pixels, sizeof(reference));
    for (unsigned round = 0; round < 64U; ++round) {
        for (size_t i = 0; i < sizeof(frame); ++i)
            frame[i] = (uint8_t)(i * 73U + round * 19U);
        encode();
    }
    for (size_t i = 0; i < sizeof(frame); ++i)
        frame[i] = (uint8_t)i;
    assert(tb_live_qr_encode(frame, sizeof(frame), &pixels));
    assert(memcmp(reference, pixels, sizeof(reference)) == 0);

    /* Oversized requests refuse without arithmetic wrap or arena mutation. */
    assert(!tb_live_qr_AllocatePool(SIZE_MAX));
    assert(!tb_live_qr_AllocatePool(524288U));
    for (size_t i = 0; i < sizeof(source); ++i)
        source[i] = (uint8_t)(i + 17U);
    uint8_t *copy = tb_live_qr_AllocateCopyPool(sizeof(source), source);
    uint8_t *zero = tb_live_qr_AllocateZeroPool(sizeof(source));
    assert(copy && zero && !((uintptr_t)copy & 15U) && !((uintptr_t)zero & 15U));
    assert(memcmp(copy, source, sizeof(source)) == 0);
    for (size_t i = 0; i < sizeof(source); ++i)
        assert(!zero[i]);
    tb_live_qr_FreePool(copy);
    while ((copy = tb_live_qr_AllocatePool(16U)) != NULL) {
        assert(!((uintptr_t)copy & 15U));
        memset(copy, 0x5a, 16U);
        total += 16U;
        ++allocations;
        assert(total <= 524288U);
    }
    assert(allocations > 0);
    assert(!tb_live_qr_AllocatePool(16U));
    /* A fresh encode resets the exhausted private arena and preserves output. */
    assert(tb_live_qr_encode(frame, sizeof(frame), &pixels));
    assert(memcmp(reference, pixels, sizeof(reference)) == 0);
    fixture(argv[1], "fixed512", 512U, 0);
    fixture(argv[1], "live4096", 4096U, 1);
    puts("live QR: real pinned Mu encoder, 64 repeated frames, rejected inputs, arena exhaustion/reset and 14 transfer fixtures passed");
    return 0;
}
