/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Standard bands: UniXNU/UniXNU@0c1bac3d51a3009dca1708b15a5198f4b43d057a,
 * scripts/patch-xnu-entry-probe.py PAINT and docs/zh/guide/debugging.md.
 * This renderer restores those completed markers without a loader rebuild.
 */
#include "LiveDisplay.h"

/* Reuse the repository's unchanged licensed EDK II 8x19 ASCII glyph data.
 * The host verifier records this dependency's complete SHA-256 and copyright.
 */
typedef uint8_t UINT8;
#include "Font.h"

#define DISPLAY_MARGIN 16U
#define GLYPH_WIDTH 8U
#define GLYPH_HEIGHT 19U

static int valid(const TB_LIVE_DISPLAY *d)
{
    size_t frame, band, first_row;
    uint32_t dual, horizontal;
    if (!d || !d->Base || ((uintptr_t)d->Base & 3U) ||
        d->Width < TB_LIVE_QR_SIDE + DISPLAY_MARGIN * 2U ||
        d->Width > 8192U || d->Height < 240U || d->Height > 8192U ||
        d->StrideBytes < d->Width * 4U || (d->StrideBytes & 3U) ||
        d->StrideBytes > 65536U ||
        d->Height > SIZE_MAX / d->StrideBytes)
        return 0;
    frame = (size_t)d->StrideBytes * d->Height;
    band = (frame >> 4) & ~(size_t)7U;
    first_row = (band * 3U + d->StrideBytes - 1U) / d->StrideBytes;
    dual = d->Width >= TB_LIVE_QR_SIDE * 2U + DISPLAY_MARGIN * 3U;
    horizontal = dual ? (d->Width - DISPLAY_MARGIN * 3U) / (TB_LIVE_QR_SIDE * 2U) :
                        (d->Width - DISPLAY_MARGIN * 2U) / TB_LIVE_QR_SIDE;
    if (frame > d->Bytes || (uintptr_t)d->Base > UINTPTR_MAX - frame ||
        d->FrameBytes != frame || d->BandBytes != band ||
        d->TextScale != (d->Width >= 1600U ? 2U : 1U) ||
        d->StatusY != first_row + 8U || d->QrX != DISPLAY_MARGIN ||
        d->QrY != d->StatusY + GLYPH_HEIGHT * d->TextScale + 8U ||
        d->DualQr != dual ||
        d->QrSecondX != (dual ? d->Width - DISPLAY_MARGIN - TB_LIVE_QR_SIDE * d->QrScale : d->QrX) ||
        !d->QrScale || d->QrScale > 8U ||
        d->QrY >= d->Height ||
        d->QrScale > (d->Height - d->QrY - 1U) / TB_LIVE_QR_SIDE ||
        d->QrScale > horizontal)
        return 0;
    return 1;
}

int tb_live_display_bind(TB_LIVE_DISPLAY *d, void *base, size_t bytes,
                         uint32_t width, uint32_t height, uint32_t stride_bytes)
{
    TB_LIVE_DISPLAY empty = {0};
    size_t first_row;
    uint32_t horizontal;
    if (!d)
        return 0;
    *d = empty;
    if (!base || ((uintptr_t)base & 3U) ||
        width < TB_LIVE_QR_SIDE + DISPLAY_MARGIN * 2U || width > 8192U ||
        height < 240U || height > 8192U || stride_bytes < width * 4U ||
        (stride_bytes & 3U) || stride_bytes > 65536U ||
        height > SIZE_MAX / stride_bytes)
        return 0;
    d->Base = base;
    d->Bytes = bytes;
    d->Width = width;
    d->Height = height;
    d->StrideBytes = stride_bytes;
    d->FrameBytes = (size_t)stride_bytes * height;
    d->BandBytes = (d->FrameBytes >> 4) & ~(size_t)7U;
    first_row = (d->BandBytes * 3U + stride_bytes - 1U) / stride_bytes;
    d->TextScale = width >= 1600U ? 2U : 1U;
    d->StatusY = (uint32_t)first_row + 8U;
    d->QrX = DISPLAY_MARGIN;
    d->QrY = d->StatusY + GLYPH_HEIGHT * d->TextScale + 8U;
    if (d->QrY >= height) {
        *d = empty;
        return 0;
    }
    d->QrScale = (height - d->QrY - 1U) / TB_LIVE_QR_SIDE;
    d->DualQr = width >= TB_LIVE_QR_SIDE * 2U + DISPLAY_MARGIN * 3U;
    horizontal = d->DualQr ? (width - DISPLAY_MARGIN * 3U) / (TB_LIVE_QR_SIDE * 2U) :
                            (width - DISPLAY_MARGIN * 2U) / TB_LIVE_QR_SIDE;
    if (d->QrScale > horizontal)
        d->QrScale = horizontal;
    if (d->QrScale > 8U)
        d->QrScale = 8U;
    d->QrSecondX = d->DualQr ? width - DISPLAY_MARGIN - TB_LIVE_QR_SIDE * d->QrScale : d->QrX;
    if (!valid(d)) {
        *d = empty;
        return 0;
    }
    return 1;
}

static void pixel(TB_LIVE_DISPLAY *d, uint32_t x, uint32_t y, uint32_t color)
{
    volatile uint32_t *at = (volatile uint32_t *)(d->Base +
        (size_t)y * d->StrideBytes + (size_t)x * 4U);
    *at = color;
}

int tb_live_display_bands(TB_LIVE_DISPLAY *d)
{
    static const uint32_t colors[3] = {
        TB_LIVE_COLOR_GREEN, TB_LIVE_COLOR_BLUE, TB_LIVE_COLOR_MAGENTA
    };
    size_t band, at;
    if (!valid(d))
        return 0;
    for (band = 0; band < 3U; ++band)
        for (at = 0; at < d->BandBytes; at += 4U)
            *(volatile uint32_t *)(d->Base + band * d->BandBytes + at) = colors[band];
    return 1;
}

int tb_live_display_qr(TB_LIVE_DISPLAY *d, const uint32_t *bgra,
                       size_t pixels, uint32_t side)
{
    return tb_live_display_qr_slot(d, bgra, pixels, side, 0);
}

int tb_live_display_qr_slot(TB_LIVE_DISPLAY *d, const uint32_t *bgra,
                            size_t pixels, uint32_t side, unsigned slot)
{
    uint32_t x, y, xx, yy, origin;
    if (!valid(d) || !bgra || ((uintptr_t)bgra & 3U) ||
        slot > 1U ||
        side != TB_LIVE_QR_SIDE || pixels < (size_t)TB_LIVE_QR_SIDE * TB_LIVE_QR_SIDE)
        return 0;
    origin = slot ? d->QrSecondX : d->QrX;
    for (y = 0; y < side; ++y)
        for (x = 0; x < side; ++x) {
            uint32_t color = UINT32_C(0xff000000) | (bgra[(size_t)y * side + x] & UINT32_C(0x00ffffff));
            for (yy = 0; yy < d->QrScale; ++yy)
                for (xx = 0; xx < d->QrScale; ++xx)
                    pixel(d, origin + x * d->QrScale + xx,
                          d->QrY + y * d->QrScale + yy, color);
        }
    return 1;
}

int tb_live_display_status(TB_LIVE_DISPLAY *d, const char *text)
{
    uint32_t x, y, index, maximum, scale;
    if (!valid(d) || !text)
        return 0;
    scale = d->TextScale;
    for (y = 0; y < GLYPH_HEIGHT * scale; ++y)
        for (x = 0; x < d->Width; ++x)
            pixel(d, x, d->StatusY + y, UINT32_C(0xff000000));
    maximum = (d->Width - DISPLAY_MARGIN * 2U) / (GLYPH_WIDTH * scale);
    for (index = 0; index < maximum && text[index]; ++index) {
        uint8_t c = (uint8_t)text[index];
        if (c < 32U || c > 126U)
            c = '?';
        for (y = 0; y < GLYPH_HEIGHT; ++y)
            for (x = 0; x < GLYPH_WIDTH; ++x) {
                uint32_t xx, yy;
                if (!(mEarlyFont[c - 32U][y] & (128U >> x)))
                    continue;
                for (yy = 0; yy < scale; ++yy)
                    for (xx = 0; xx < scale; ++xx)
                        pixel(d, DISPLAY_MARGIN + index * GLYPH_WIDTH * scale + x * scale + xx,
                              d->StatusY + y * scale + yy, UINT32_C(0xffeeeeee));
            }
    }
    return 1;
}
