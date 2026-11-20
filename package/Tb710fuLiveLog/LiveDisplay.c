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
#define DISPLAY_GAP 16U
#define CHECKPOINT_LOG_MIN_COLUMNS 8U
#define GLYPH_WIDTH 8U
#define GLYPH_HEIGHT 19U

/* The checkpoint pane has one owner. Keep its text ring and the last paint
 * in RAM so scrolling never reads from the live framebuffer. */
static uint8_t scroll_lines[TB_LIVE_SCROLL_MAX_ROWS][TB_LIVE_SCROLL_MAX_COLUMNS];
static uint8_t scroll_painted[TB_LIVE_SCROLL_MAX_ROWS][TB_LIVE_SCROLL_MAX_COLUMNS];

static int valid(const TB_LIVE_DISPLAY *d)
{
    size_t frame, band, first_row, status_y, checkpoint_size;
    uint32_t dual, horizontal, checkpoint_scale, checkpoint_x, checkpoint_y;
    uint32_t log_width, log_height, log_columns, log_rows, checkpoint_status_y;
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
        d->QrX != DISPLAY_MARGIN || d->QrY != first_row + 8U ||
        d->DualQr != dual ||
        d->QrSecondX != (dual ? d->Width - DISPLAY_MARGIN - TB_LIVE_QR_SIDE * d->QrScale : d->QrX) ||
        !d->QrScale || d->QrScale > 8U ||
        d->QrY >= d->Height ||
        d->QrScale > (d->Height - d->QrY - 1U) / TB_LIVE_QR_SIDE ||
        d->QrScale > horizontal)
        return 0;
    status_y = (size_t)d->QrY + TB_LIVE_QR_SIDE * d->QrScale + 8U;
    if (d->StatusY != status_y ||
        status_y + GLYPH_HEIGHT * d->TextScale > d->Height)
        return 0;
    checkpoint_scale = d->CheckpointQrScale;
    if (!checkpoint_scale) {
        return !d->CheckpointQrX && !d->CheckpointQrY && !d->LogX &&
               !d->LogY && !d->LogWidth && !d->LogHeight &&
               !d->LogColumns && !d->LogRows && !d->CheckpointStatusY;
    }
    if (checkpoint_scale > 8U)
        return 0;
    checkpoint_size = (size_t)TB_LIVE_QR_SIDE * checkpoint_scale;
    checkpoint_x = d->Width - DISPLAY_MARGIN - (uint32_t)checkpoint_size;
    checkpoint_y = d->Height - DISPLAY_MARGIN - (uint32_t)checkpoint_size;
    checkpoint_status_y = d->Height - DISPLAY_MARGIN - GLYPH_HEIGHT * d->TextScale;
    if (checkpoint_y < first_row + 8U ||
        checkpoint_x < DISPLAY_MARGIN + DISPLAY_GAP + GLYPH_WIDTH * d->TextScale ||
        d->CheckpointQrX != checkpoint_x || d->CheckpointQrY != checkpoint_y ||
        d->LogX != DISPLAY_MARGIN ||
        d->LogWidth != checkpoint_x - DISPLAY_MARGIN - DISPLAY_GAP ||
        d->LogY != first_row + 8U || checkpoint_status_y <= d->LogY + 8U ||
        d->CheckpointStatusY != checkpoint_status_y)
        return 0;
    log_width = d->LogWidth;
    log_height = checkpoint_status_y - d->LogY - 8U;
    log_columns = log_width / (GLYPH_WIDTH * d->TextScale);
    log_rows = log_height / (GLYPH_HEIGHT * d->TextScale);
    if (log_columns > TB_LIVE_SCROLL_MAX_COLUMNS)
        log_columns = TB_LIVE_SCROLL_MAX_COLUMNS;
    if (log_rows > TB_LIVE_SCROLL_MAX_ROWS)
        log_rows = TB_LIVE_SCROLL_MAX_ROWS;
    if (!log_columns || !log_rows || d->LogHeight != log_height ||
        d->LogColumns != log_columns || d->LogRows != log_rows)
        return 0;
    return 1;
}

int tb_live_display_bind(TB_LIVE_DISPLAY *d, void *base, size_t bytes,
                         uint32_t width, uint32_t height, uint32_t stride_bytes)
{
    TB_LIVE_DISPLAY empty = {0};
    size_t first_row, log_min_width, checkpoint_vertical;
    uint32_t horizontal, checkpoint_scale, checkpoint_horizontal;
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
    d->QrX = DISPLAY_MARGIN;
    d->QrY = (uint32_t)first_row + 8U;
    if (d->QrY >= height) {
        *d = empty;
        return 0;
    }
    size_t bottom_ui = GLYPH_HEIGHT * d->TextScale + 8U;
    if (height - d->QrY <= bottom_ui) {
        *d = empty;
        return 0;
    }
    d->QrScale = (height - d->QrY - bottom_ui) / TB_LIVE_QR_SIDE;
    d->DualQr = width >= TB_LIVE_QR_SIDE * 2U + DISPLAY_MARGIN * 3U;
    horizontal = d->DualQr ? (width - DISPLAY_MARGIN * 3U) / (TB_LIVE_QR_SIDE * 2U) :
                            (width - DISPLAY_MARGIN * 2U) / TB_LIVE_QR_SIDE;
    if (d->QrScale > horizontal)
        d->QrScale = horizontal;
    if (d->QrScale > 8U)
        d->QrScale = 8U;
    d->QrSecondX = d->DualQr ? width - DISPLAY_MARGIN - TB_LIVE_QR_SIDE * d->QrScale : d->QrX;
    d->StatusY = d->QrY + TB_LIVE_QR_SIDE * d->QrScale + 8U;
    log_min_width = (size_t)GLYPH_WIDTH * d->TextScale * CHECKPOINT_LOG_MIN_COLUMNS;
    if (width > DISPLAY_MARGIN * 3U + DISPLAY_GAP + log_min_width) {
        checkpoint_horizontal = (uint32_t)((width - DISPLAY_MARGIN * 3U -
            log_min_width) / TB_LIVE_QR_SIDE);
        checkpoint_vertical = height - DISPLAY_MARGIN - (first_row + 8U);
        checkpoint_scale = (uint32_t)(checkpoint_vertical / TB_LIVE_QR_SIDE);
        if (checkpoint_scale > checkpoint_horizontal)
            checkpoint_scale = checkpoint_horizontal;
        if (checkpoint_scale > 8U)
            checkpoint_scale = 8U;
        if (checkpoint_scale) {
            uint32_t qr_size = TB_LIVE_QR_SIDE * checkpoint_scale;
            uint32_t checkpoint_status_y = height - DISPLAY_MARGIN - GLYPH_HEIGHT * d->TextScale;
            d->CheckpointQrScale = checkpoint_scale;
            d->CheckpointQrX = width - DISPLAY_MARGIN - qr_size;
            d->CheckpointQrY = height - DISPLAY_MARGIN - qr_size;
            d->LogX = DISPLAY_MARGIN;
            d->LogY = (uint32_t)first_row + 8U;
            d->LogWidth = d->CheckpointQrX - DISPLAY_MARGIN - DISPLAY_GAP;
            d->CheckpointStatusY = checkpoint_status_y;
            if (checkpoint_status_y > d->LogY + 8U) {
                d->LogHeight = checkpoint_status_y - d->LogY - 8U;
                d->LogColumns = d->LogWidth / (GLYPH_WIDTH * d->TextScale);
                d->LogRows = d->LogHeight / (GLYPH_HEIGHT * d->TextScale);
                if (d->LogColumns > TB_LIVE_SCROLL_MAX_COLUMNS)
                    d->LogColumns = TB_LIVE_SCROLL_MAX_COLUMNS;
                if (d->LogRows > TB_LIVE_SCROLL_MAX_ROWS)
                    d->LogRows = TB_LIVE_SCROLL_MAX_ROWS;
            } else {
                d->CheckpointQrScale = d->CheckpointQrX = d->CheckpointQrY = 0;
                d->LogX = d->LogY = d->LogWidth = d->LogHeight = 0;
                d->LogColumns = d->LogRows = d->CheckpointStatusY = 0;
            }
        }
    }
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

int tb_live_display_checkpoint_qr(TB_LIVE_DISPLAY *d, const uint32_t *bgra,
                                  size_t pixels, uint32_t side)
{
    uint32_t x, y, xx, yy;
    if (!valid(d) || !d->CheckpointQrScale || !bgra || ((uintptr_t)bgra & 3U) ||
        side != TB_LIVE_QR_SIDE || pixels < (size_t)TB_LIVE_QR_SIDE * TB_LIVE_QR_SIDE)
        return 0;
    for (y = 0; y < side; ++y)
        for (x = 0; x < side; ++x) {
            uint32_t color = UINT32_C(0xff000000) | (bgra[(size_t)y * side + x] & UINT32_C(0x00ffffff));
            for (yy = 0; yy < d->CheckpointQrScale; ++yy)
                for (xx = 0; xx < d->CheckpointQrScale; ++xx)
                    pixel(d, d->CheckpointQrX + x * d->CheckpointQrScale + xx,
                          d->CheckpointQrY + y * d->CheckpointQrScale + yy, color);
        }
    return 1;
}

static void draw_character(TB_LIVE_DISPLAY *d, uint32_t x, uint32_t y,
                           uint8_t previous, uint8_t c, uint32_t scale)
{
    uint32_t gx, gy, xx, yy;
    if (c < 32U || c > 126U)
        c = '?';
    for (gy = 0; gy < GLYPH_HEIGHT; ++gy) {
        uint8_t next_bits = mEarlyFont[c - 32U][gy];
        uint8_t changed = next_bits ^ mEarlyFont[previous - 32U][gy];
        for (gx = 0; gx < GLYPH_WIDTH; ++gx) {
            uint32_t mask = 128U >> gx;
            if (!(changed & mask))
                continue;
            uint32_t color = next_bits & mask ? UINT32_C(0xffeeeeee) : UINT32_C(0xff000000);
            for (yy = 0; yy < scale; ++yy)
                for (xx = 0; xx < scale; ++xx)
                    pixel(d, x + gx * scale + xx, y + gy * scale + yy, color);
        }
    }
}

static void clear_scroll_pane(TB_LIVE_DISPLAY *d)
{
    for (uint32_t y = d->LogY; y < d->LogY + d->LogHeight; ++y)
        for (uint32_t x = d->LogX; x < d->LogX + d->LogWidth; ++x)
            pixel(d, x, y, UINT32_C(0xff000000));
    for (uint32_t row = 0; row < d->LogRows; ++row)
        for (uint32_t column = 0; column < d->LogColumns; ++column)
            scroll_painted[row][column] = ' ';
}

static void paint_scroll_lines(TB_LIVE_DISPLAY *d)
{
    for (uint32_t row = 0; row < d->LogCount; ++row) {
        uint32_t current = (d->LogHead + row) % d->LogRows;
        uint32_t y = d->LogY + row * GLYPH_HEIGHT * d->TextScale;
        for (uint32_t column = 0; column < d->LogColumns; ++column) {
            uint8_t c = scroll_lines[current][column];
            if (c == scroll_painted[row][column])
                continue;
            uint32_t x = d->LogX + column * GLYPH_WIDTH * d->TextScale;
            draw_character(d, x, y, scroll_painted[row][column], c, d->TextScale);
            scroll_painted[row][column] = c;
        }
    }
}

static void scroll_next_line(TB_LIVE_DISPLAY *d)
{
    uint32_t current;
    if (d->LogCount < d->LogRows) {
        current = (d->LogHead + d->LogCount) % d->LogRows;
        ++d->LogCount;
    } else {
        d->LogHead = (d->LogHead + 1U) % d->LogRows;
        current = (d->LogHead + d->LogCount - 1U) % d->LogRows;
    }
    for (uint32_t i = 0; i < d->LogColumns; ++i)
        scroll_lines[current][i] = ' ';
    d->LogColumn = 0;
}

static void scroll_character(TB_LIVE_DISPLAY *d, uint8_t c)
{
    uint32_t current;
    if (d->LogColumn >= d->LogColumns)
        scroll_next_line(d);
    current = (d->LogHead + d->LogCount - 1U) % d->LogRows;
    scroll_lines[current][d->LogColumn] = c;
    ++d->LogColumn;
}

static void scroll_append(TB_LIVE_DISPLAY *d, uint8_t c)
{
    if (c == '\r')
        return;
    if (c == '\n') {
        scroll_next_line(d);
        return;
    }
    if (c == '\t') {
        uint32_t spaces = 4U - (d->LogColumn & 3U);
        while (spaces--)
            scroll_character(d, ' ');
        return;
    }
    if (c < 32U || c > 126U)
        c = '?';
    scroll_character(d, c);
}

int tb_live_display_scroll_log(TB_LIVE_DISPLAY *d, const char *text, size_t bytes,
                               uint64_t start, uint64_t end)
{
    if (!valid(d) || !d->CheckpointQrScale || (!text && bytes) || end < start ||
        end - start != bytes)
        return 0;
    if (!d->LogInitialized) {
        clear_scroll_pane(d);
        scroll_next_line(d);
        d->LogEnd = start;
        d->LogInitialized = 1;
    }
    if (end <= d->LogEnd)
        return 1;
    if (start > d->LogEnd) {
        static const char gap[] = "[LOG GAP]\n";
        for (size_t i = 0; i < sizeof(gap) - 1U; ++i)
            scroll_append(d, (uint8_t)gap[i]);
    }
    size_t offset = d->LogEnd > start ? (size_t)(d->LogEnd - start) : 0U;
    if (offset > bytes)
        return 0;
    for (size_t i = offset; i < bytes; ++i)
        scroll_append(d, (uint8_t)text[i]);
    paint_scroll_lines(d);
    d->LogEnd = end;
    return 1;
}

int tb_live_display_scroll_refresh(TB_LIVE_DISPLAY *d)
{
    if (!valid(d) || !d->CheckpointQrScale)
        return 0;
    if (d->LogInitialized) {
        clear_scroll_pane(d);
        paint_scroll_lines(d);
    }
    return 1;
}

int tb_live_display_checkpoint_status(TB_LIVE_DISPLAY *d, const char *text)
{
    uint32_t index, x, y, maximum;
    if (!valid(d) || !d->CheckpointQrScale || !text)
        return 0;
    for (y = 0; y < GLYPH_HEIGHT * d->TextScale; ++y)
        for (x = d->LogX; x < d->LogX + d->LogWidth; ++x)
            pixel(d, x, d->CheckpointStatusY + y, UINT32_C(0xff000000));
    maximum = d->LogColumns;
    for (index = 0; index < maximum && text[index]; ++index)
        draw_character(d, d->LogX + index * GLYPH_WIDTH * d->TextScale,
                       d->CheckpointStatusY, ' ', (uint8_t)text[index], d->TextScale);
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
        draw_character(d, DISPLAY_MARGIN + index * GLYPH_WIDTH * scale,
                       d->StatusY, ' ', c, scale);
    }
    return 1;
}
