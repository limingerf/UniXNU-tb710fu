/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "LiveDisplay.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t UINT8;
#include "Font.h"

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

static void check_checkpoint_glyph(const uint8_t *frame, const TB_LIVE_DISPLAY *d,
                                   uint32_t row, uint32_t column, uint8_t character)
{
    uint32_t scale = d->TextScale;
    uint32_t y0 = d->LogY + row * 19U * scale;
    uint32_t x0 = d->LogX + column * 8U * scale;
    for (uint32_t y = 0; y < 19U; ++y)
        for (uint32_t x = 0; x < 8U; ++x) {
            uint32_t color = mEarlyFont[character - 32U][y] & (128U >> x) ?
                             UINT32_C(0xffeeeeee) : UINT32_C(0xff000000);
            for (uint32_t yy = 0; yy < scale; ++yy)
                for (uint32_t xx = 0; xx < scale; ++xx)
                    assert(read_pixel(frame, (size_t)(y0 + y * scale + yy) * d->StrideBytes +
                                      (size_t)(x0 + x * scale + xx) * 4U) == color);
        }
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
    char long_log[4096];
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

    if (d.CheckpointQrScale) {
        size_t length = 0;
        uint32_t row;
        uint32_t qr_size = TB_LIVE_QR_SIDE * d.CheckpointQrScale;
        assert(d.CheckpointQrX + qr_size == width - 16U);
        assert(d.CheckpointQrY + qr_size == height - 16U);
        assert(d.LogX + d.LogWidth + 16U <= d.CheckpointQrX);
        assert(d.LogY * stride >= d.BandBytes * 3U);
        assert(d.LogRows > 0 && d.LogColumns > 0);
        for (n = 0; n < TB_LIVE_QR_SIDE * TB_LIVE_QR_SIDE; ++n)
            bitmap[n] = (n % TB_LIVE_QR_SIDE + n / TB_LIVE_QR_SIDE) & 1U ?
                UINT32_C(0x12ffffff) : UINT32_C(0x34000000);
        for (row = 0; row < 80U; ++row) {
            int written = snprintf(long_log + length, sizeof(long_log) - length,
                                   row + 1U == 80U ? "ROW-%03u" : "ROW-%03u\n", row);
            assert(written > 0 && (size_t)written < sizeof(long_log) - length);
            length += (size_t)written;
        }
        assert(tb_live_display_checkpoint_qr(&d, bitmap,
            TB_LIVE_QR_SIDE * TB_LIVE_QR_SIDE, TB_LIVE_QR_SIDE));
        assert(tb_live_display_scroll_refresh(&d)); /* the pane has never been painted */
        assert(!d.LogInitialized);
        assert(tb_live_display_scroll_log(&d, long_log, length, 0, length));
        assert(tb_live_display_checkpoint_status(&d, "SCROLL LOG - LATEST ROW 79"));
        check_bands(frame, &d);
        for (row = 0; row < d.LogRows; ++row) {
            char line[8];
            assert(snprintf(line, sizeof(line), "ROW-%03u", 80U - d.LogRows + row) == 7);
            for (uint32_t column = 0; column < 7U; ++column)
                check_checkpoint_glyph(frame, &d, row, column, (uint8_t)line[column]);
        }
        {
            static const char followup[] = "ROW-079\nLATEST";
            uint64_t start = length - 7U;
            uint64_t end = start + sizeof(followup) - 1U;
            assert(tb_live_display_scroll_log(&d, followup, sizeof(followup) - 1U, start, end));
            assert(d.LogEnd == end);
            check_checkpoint_glyph(frame, &d, d.LogRows - 1U, 0, 'L');
            check_checkpoint_glyph(frame, &d, d.LogRows - 1U, 6, ' '); /* old glyph erased */
            uint8_t *painted = malloc(bytes);
            assert(painted);
            memcpy(painted, frame, bytes);
            assert(tb_live_display_scroll_log(&d, followup, sizeof(followup) - 1U, start, end));
            assert(tb_live_display_scroll_log(&d, long_log, length, 0, length));
            assert(d.LogEnd == end && memcmp(painted, frame, bytes) == 0);
            /* A console clear erases the paint, but an explicit refresh restores all rows. */
            for (y = d.LogY; y < d.LogY + d.LogHeight; ++y)
                memset(frame + (size_t)y * stride + d.LogX * 4U, SENTINEL, d.LogWidth * 4U);
            assert(tb_live_display_scroll_refresh(&d));
            assert(d.LogEnd == end && memcmp(painted, frame, bytes) == 0);
            free(painted);
            static const char after_gap[] = "NEXT";
            uint64_t gap_start = end + 5U;
            uint64_t gap_end = gap_start + sizeof(after_gap) - 1U;
            assert(tb_live_display_scroll_log(&d, after_gap, sizeof(after_gap) - 1U,
                                              gap_start, gap_end));
            assert(d.LogEnd == gap_end);
            check_checkpoint_glyph(frame, &d, d.LogRows - 1U, 0, 'N');
        }
        for (y = 0; y < qr_size; ++y)
            for (x = 0; x < qr_size; ++x) {
                uint32_t expected = ((x / d.CheckpointQrScale + y / d.CheckpointQrScale) & 1U) ?
                    UINT32_C(0xffffffff) : UINT32_C(0xff000000);
                assert(read_pixel(frame, (size_t)(d.CheckpointQrY + y) * stride +
                                  (size_t)(d.CheckpointQrX + x) * 4U) == expected);
            }
        for (n = d.BandBytes * 3U; n < bytes; ++n) {
            size_t pixel_row = n / stride, column = n % stride;
            int text = pixel_row >= d.StatusY && pixel_row < d.StatusY + 19U * d.TextScale &&
                       column < (size_t)width * 4U;
            int legacy_qr = pixel_row >= d.QrY &&
                pixel_row < d.QrY + TB_LIVE_QR_SIDE * d.QrScale &&
                ((column >= (size_t)d.QrX * 4U &&
                  column < (size_t)(d.QrX + TB_LIVE_QR_SIDE * d.QrScale) * 4U) ||
                 (d.DualQr && column >= (size_t)d.QrSecondX * 4U &&
                  column < (size_t)(d.QrSecondX + TB_LIVE_QR_SIDE * d.QrScale) * 4U));
            int log_text = pixel_row >= d.LogY && pixel_row < d.LogY + d.LogHeight &&
                           column >= (size_t)d.LogX * 4U &&
                           column < (size_t)(d.LogX + d.LogWidth) * 4U;
            int checkpoint_status = pixel_row >= d.CheckpointStatusY &&
                pixel_row < d.CheckpointStatusY + 19U * d.TextScale &&
                column >= (size_t)d.LogX * 4U &&
                column < (size_t)(d.LogX + d.LogWidth) * 4U;
            int checkpoint_qr = pixel_row >= d.CheckpointQrY &&
                pixel_row < d.CheckpointQrY + qr_size &&
                column >= (size_t)d.CheckpointQrX * 4U &&
                column < (size_t)(d.CheckpointQrX + qr_size) * 4U;
            if (!(text || legacy_qr || log_text || checkpoint_status || checkpoint_qr))
                assert(frame[n] == SENTINEL);
        }
        guards(memory, bytes);
    } else {
        assert(!tb_live_display_checkpoint_qr(&d, bitmap,
            TB_LIVE_QR_SIDE * TB_LIVE_QR_SIDE, TB_LIVE_QR_SIDE));
        assert(!tb_live_display_scroll_log(&d, "NO TEXT REGION", 14U, 0, 14U));
        assert(!tb_live_display_scroll_refresh(&d));
        assert(!tb_live_display_checkpoint_status(&d, "NO TEXT REGION"));
    }

    /* The scheduler marker must preserve the bands, log footer, QR and padding. */
    if (d.CheckpointQrScale && d.Width >= 16U + 17U * 8U) {
        uint8_t *copy = malloc(bytes);
        assert(copy);
        memcpy(copy, frame, bytes);
        tb_live_display_scheduler_mark(&d, 12U, 0x0f87U);
        assert(memcmp(copy, frame, bytes) != 0);
        const unsigned scale = d.Width >= 16U + 17U * 16U ? 2U : 1U;
        for (n = 0; n < bytes; ++n) {
            const size_t row = n / stride, column = n % stride;
            if (row < height - 16U || row >= height - 2U ||
                column < 16U * 4U || column >= (16U + 17U * 8U * scale) * 4U)
                assert(copy[n] == frame[n]);
        }
        const unsigned boot_x = 16U + 17U * 8U * scale + 16U;
        const unsigned minimum = boot_x + 2U * 12U * 8U + 16U;
        const unsigned boot_scale = d.Width >= minimum + 2U * 12U * 8U ? 2U : 1U;
        const char eight_bytes[] = {'p', 'u', 'b', 'l', 'i', 's', 'h', '1'};
        for (unsigned lane = 0; lane < 2; ++lane) {
            memcpy(copy, frame, bytes);
            tb_live_display_boot_mark(&d, lane, eight_bytes);
            if (d.Width >= minimum) {
                const unsigned lane_width = 12U * 8U * boot_scale;
                const unsigned x = boot_x + lane * (lane_width + 16U);
                assert(memcmp(copy, frame, bytes) != 0);
                for (n = 0; n < bytes; ++n) {
                    const size_t row = n / stride, column = n % stride;
                    if (row < height - 16U || row >= height - 2U ||
                        column < x * 4U || column >= (x + lane_width) * 4U)
                        assert(copy[n] == frame[n]);
                }
                tb_live_display_boot_mark(&d, lane, "\xff");
                memcpy(copy, frame, bytes);
                tb_live_display_boot_mark(&d, lane, "?");
                assert(memcmp(copy, frame, bytes) == 0);
                for (unsigned row = height - 16U; row < height - 2U; ++row)
                    for (unsigned col = x + 5U * 8U * boot_scale; col < x + lane_width; ++col)
                        assert(*(uint32_t *)(frame + (size_t)row * stride + col * 4U) == UINT32_C(0xff000000));
            } else {
                assert(memcmp(copy, frame, bytes) == 0);
            }
        }
        memcpy(copy, frame, bytes);
        tb_live_display_boot_mark(&d, 2U, "invalid");
        tb_live_display_boot_mark(&d, 0U, NULL);
        assert(memcmp(copy, frame, bytes) == 0);
        guards(memory, bytes);
        free(copy);
    }
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
        tb_live_display_boot_mark(&broken, 0U, "REJECT");
        tb_live_display_boot_mark(NULL, 0U, "REJECT");
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
    surface(240, 480, 960);    /* narrow checkpoint pane with a single bottom-right QR */
    surface(641, 481, 2576);   /* padding and a band ending inside a row */
    surface(512, 480, 2048);  /* both boot markers fit at one horizontal pixel per glyph pixel */
    surface(153, 240, 616);    /* smallest supported geometry, padded */
    surface(289, 480, 1160);   /* alternate two sessions in the shared QR rectangle */
    surface(290, 480, 1160);   /* first geometry wide enough for two QR slots */
    surface(1600, 900, 6412);  /* doubled glyph scale with row padding */
    rejected();
    puts("live display: exact upstream bands, BGRA QR, bounded ASCII and guard/padding checks passed");
    return 0;
}
