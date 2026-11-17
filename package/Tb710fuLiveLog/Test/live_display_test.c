/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "LiveDisplay.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUARD 64U
#define EXTRA 128U
#define SENTINEL 0xa5U

static uint32_t bitmap[TB_LIVE_QR_SIDE * TB_LIVE_QR_SIDE];

static uint32_t read_pixel(const uint8_t *frame, size_t at)
{
    uint32_t result;
    memcpy(&result, frame + at, sizeof(result));
    return result;
}

static void guards(const uint8_t *memory, size_t bytes)
{
    size_t n;
    for (n = 0; n < GUARD; ++n)
        assert(memory[n] == SENTINEL);
    for (n = GUARD + bytes; n < GUARD + bytes + GUARD + EXTRA; ++n)
        assert(memory[n] == SENTINEL);
}

static void check_bands(const uint8_t *frame, const TB_LIVE_DISPLAY *d)
{
    const uint32_t colors[] = {
        TB_LIVE_COLOR_GREEN, TB_LIVE_COLOR_BLUE, TB_LIVE_COLOR_MAGENTA
    };
    size_t n;
    for (n = 0; n < d->BandBytes * 3U; n += 4U)
        assert(read_pixel(frame, n) == colors[n / d->BandBytes]);
}

static void surface(uint32_t width, uint32_t height, uint32_t stride)
{
    TB_LIVE_DISPLAY d;
    size_t bytes = (size_t)stride * height;
    uint8_t *memory = malloc(bytes + GUARD * 2U + EXTRA);
    uint8_t *frame;
    size_t n;
    uint32_t x, y;
    char long_line[2048];
    assert(memory);
    memset(memory, SENTINEL, bytes + GUARD * 2U + EXTRA);
    frame = memory + GUARD;
    assert(tb_live_display_bind(&d, frame, bytes + EXTRA, width, height, stride));
    assert(d.BandBytes == ((bytes >> 4) & ~(size_t)7U));
    assert((size_t)d.QrY * stride >= d.BandBytes * 3U);
    assert((size_t)d.StatusY * stride >=
           ((size_t)d.QrY + TB_LIVE_QR_SIDE * d.QrScale) * stride);
    assert(d.QrX + TB_LIVE_QR_SIDE * d.QrScale <= width);
    assert(d.QrSecondX + TB_LIVE_QR_SIDE * d.QrScale <= width);
    if (d.DualQr)
        assert(d.QrX + TB_LIVE_QR_SIDE * d.QrScale + 16U <= d.QrSecondX);
    else
        assert(d.QrX == d.QrSecondX);
    assert(d.QrY + TB_LIVE_QR_SIDE * d.QrScale <= height);
    for (n = 0; n < bytes; ++n)
        assert(frame[n] == SENTINEL); /* bind is read-only */
    assert(tb_live_display_bands(&d));
    check_bands(frame, &d);
    for (n = d.BandBytes * 3U; n < bytes; ++n)
        assert(frame[n] == SENTINEL);
    guards(memory, bytes);

    memset(long_line, 'A', sizeof(long_line));
    long_line[sizeof(long_line) - 1U] = '\0';
    long_line[1] = '\n';
    long_line[2] = (char)0xff;
    assert(tb_live_display_status(&d, long_line));
    check_bands(frame, &d);
    for (y = (uint32_t)((d.BandBytes * 3U + stride - 1U) / stride); y < height; ++y)
        for (x = width * 4U; x < stride; ++x)
            assert(frame[(size_t)y * stride + x] == SENTINEL);
    guards(memory, bytes);

    for (y = 0; y < TB_LIVE_QR_SIDE; ++y)
        for (x = 0; x < TB_LIVE_QR_SIDE; ++x)
            bitmap[(size_t)y * TB_LIVE_QR_SIDE + x] = (x + y) & 1U ?
                UINT32_C(0x12ffffff) : UINT32_C(0x34000000);
    assert(tb_live_display_qr(&d, bitmap, TB_LIVE_QR_SIDE * TB_LIVE_QR_SIDE, TB_LIVE_QR_SIDE));
    /* A live update uses another rectangle on a wide display. */
    assert(tb_live_display_qr_slot(&d, bitmap, TB_LIVE_QR_SIDE * TB_LIVE_QR_SIDE, TB_LIVE_QR_SIDE, 1));
    check_bands(frame, &d);
    for (y = 0; y < TB_LIVE_QR_SIDE * d.QrScale; ++y)
        for (x = 0; x < TB_LIVE_QR_SIDE * d.QrScale; ++x) {
            uint32_t expected = ((x / d.QrScale + y / d.QrScale) & 1U) ?
                UINT32_C(0xffffffff) : UINT32_C(0xff000000);
            assert(read_pixel(frame, (size_t)(d.QrY + y) * stride +
                              (size_t)(d.QrX + x) * 4U) == expected);
            assert(read_pixel(frame, (size_t)(d.QrY + y) * stride +
                              (size_t)(d.QrSecondX + x) * 4U) == expected);
        }
    /* Overwrite live with a distinct bitmap and verify the frozen left code. */
    if (d.DualQr) {
        for (n = 0; n < TB_LIVE_QR_SIDE * TB_LIVE_QR_SIDE; ++n)
            bitmap[n] = UINT32_C(0xffffffff);
        assert(tb_live_display_qr_slot(&d, bitmap, TB_LIVE_QR_SIDE * TB_LIVE_QR_SIDE, TB_LIVE_QR_SIDE, 1));
        for (y = 0; y < TB_LIVE_QR_SIDE * d.QrScale; ++y)
            for (x = 0; x < TB_LIVE_QR_SIDE * d.QrScale; ++x) {
                uint32_t expected = ((x / d.QrScale + y / d.QrScale) & 1U) ?
                    UINT32_C(0xffffffff) : UINT32_C(0xff000000);
                assert(read_pixel(frame, (size_t)(d.QrY + y) * stride +
                                  (size_t)(d.QrX + x) * 4U) == expected);
                assert(read_pixel(frame, (size_t)(d.QrY + y) * stride +
                                  (size_t)(d.QrSecondX + x) * 4U) == UINT32_C(0xffffffff));
            }
    }
    /* Everything except exact bands, one text line and the QR rectangle stays untouched. */
    for (n = d.BandBytes * 3U; n < bytes; ++n) {
        size_t row = n / stride, column = n % stride;
        int text = row >= d.StatusY && row < d.StatusY + 19U * d.TextScale &&
                   column < (size_t)width * 4U;
        int qr = row >= d.QrY && row < d.QrY + TB_LIVE_QR_SIDE * d.QrScale &&
                 ((column >= (size_t)d.QrX * 4U &&
                   column < (size_t)(d.QrX + TB_LIVE_QR_SIDE * d.QrScale) * 4U) ||
                  (column >= (size_t)d.QrSecondX * 4U &&
                   column < (size_t)(d.QrSecondX + TB_LIVE_QR_SIDE * d.QrScale) * 4U));
        if (!text && !qr)
            assert(frame[n] == SENTINEL);
    }
    guards(memory, bytes);

    /* The high-contrast status line stays below both QR slots. */
    assert(tb_live_display_status(&d, ""));
    for (y = 0; y < 19U * d.TextScale; ++y)
        for (x = 0; x < width; ++x)
            assert(read_pixel(frame, (size_t)(d.StatusY + y) * stride + x * 4U) == UINT32_C(0xff000000));
    assert(tb_live_display_status(&d, "RAMOOPS LOG - FRAME 1/4"));
    check_bands(frame, &d);
    guards(memory, bytes);

    /* Invalid bitmaps and tampered layouts refuse to draw. */
    {
        uint8_t *copy = malloc(bytes);
        TB_LIVE_DISPLAY broken = d;
        assert(copy);
        memcpy(copy, frame, bytes);
        assert(!tb_live_display_qr(&d, NULL, 0, TB_LIVE_QR_SIDE));
        assert(!tb_live_display_qr(&d, bitmap, TB_LIVE_QR_SIDE * TB_LIVE_QR_SIDE - 1U, TB_LIVE_QR_SIDE));
        assert(!tb_live_display_qr(&d, bitmap, TB_LIVE_QR_SIDE * TB_LIVE_QR_SIDE, TB_LIVE_QR_SIDE - 1U));
        assert(!tb_live_display_qr_slot(&d, bitmap, TB_LIVE_QR_SIDE * TB_LIVE_QR_SIDE, TB_LIVE_QR_SIDE, 2));
        assert(!tb_live_display_status(&d, NULL));
        broken.QrY = 0;
        assert(!tb_live_display_qr(&broken, bitmap, TB_LIVE_QR_SIDE * TB_LIVE_QR_SIDE, TB_LIVE_QR_SIDE));
        assert(!tb_live_display_bands(&broken));
        assert(!tb_live_display_status(&broken, "REJECT"));
        assert(memcmp(copy, frame, bytes) == 0);
        free(copy);
    }
    free(memory);
}

static void rejected(void)
{
    uint32_t frame[640U * 480U];
    TB_LIVE_DISPLAY d;
    const uint32_t bad[][3] = {
        {0, 480, 2560}, {640, 0, 2560}, {8193, 480, 65536},
        {640, 8193, 2560}, {640, 480, 2556}, {640, 480, 2561},
        {640, 480, 65540}, {152, 480, 2560}, {640, 239, 2560},
        {UINT32_MAX, UINT32_MAX, UINT32_MAX}
    };
    size_t n;
    assert(!tb_live_display_bind(NULL, frame, sizeof(frame), 640, 480, 2560));
    assert(!tb_live_display_bind(&d, NULL, sizeof(frame), 640, 480, 2560));
    assert(!tb_live_display_bind(&d, (uint8_t *)frame + 1, sizeof(frame) - 1U, 640, 480, 2560));
    assert(!tb_live_display_bind(&d, frame, sizeof(frame) - 1U, 640, 480, 2560));
    assert(!tb_live_display_bind(&d, (void *)(UINTPTR_MAX & ~(uintptr_t)3U), SIZE_MAX, 640, 480, 2560));
    for (n = 0; n < sizeof(bad) / sizeof(bad[0]); ++n) {
        memset(&d, 0xa5, sizeof(d));
        assert(!tb_live_display_bind(&d, frame, sizeof(frame), bad[n][0], bad[n][1], bad[n][2]));
        assert(!d.Base);
        assert(!tb_live_display_bands(&d));
        assert(!tb_live_display_status(&d, "REJECT"));
    }
    assert(!tb_live_display_bands(NULL));
    assert(!tb_live_display_status(NULL, "REJECT"));
    assert(!tb_live_display_qr(NULL, bitmap, TB_LIVE_QR_SIDE * TB_LIVE_QR_SIDE, TB_LIVE_QR_SIDE));
}

int main(void)
{
    surface(3200, 2000, 12800); /* observed TB710FU geometry */
    surface(640, 480, 2560);
    surface(641, 481, 2576);   /* padding and a band ending inside a row */
    surface(153, 240, 616);    /* smallest supported geometry, padded */
    surface(289, 480, 1160);   /* alternate two sessions in the shared QR rectangle */
    surface(290, 480, 1160);   /* first geometry wide enough for two QR slots */
    surface(1600, 900, 6412);  /* doubled glyph scale with row padding */
    rejected();
    puts("live display: exact upstream bands, BGRA QR, bounded ASCII and guard/padding checks passed");
    return 0;
}
