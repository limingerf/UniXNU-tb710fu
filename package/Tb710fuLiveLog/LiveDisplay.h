/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef TB710FU_LIVE_DISPLAY_H
#define TB710FU_LIVE_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

#define TB_LIVE_QR_SIDE 121U
#define TB_LIVE_COLOR_GREEN UINT32_C(0xff00ff00)
#define TB_LIVE_COLOR_BLUE UINT32_C(0xff0000ff)
#define TB_LIVE_COLOR_MAGENTA UINT32_C(0xffff00ff)

/* Base must be a previously validated, writable, linear BGRA32 mapping.
 * No function maps hardware, changes translation controls or waits.
 * Bytes is the actual mapping length; row padding is included in StrideBytes.
 * Layout fields are filled by bind and must not be changed by the caller.
 */
typedef struct {
    volatile uint8_t *Base;
    size_t Bytes, FrameBytes, BandBytes;
    uint32_t Width, Height, StrideBytes;
    uint32_t QrX, QrSecondX, QrY, QrScale, StatusY, TextScale, DualQr;
} TB_LIVE_DISPLAY;

/* Return 1 on success, 0 for an invalid or too-small surface. bind never paints.
 * A failed bind clears the destination so subsequent drawing is rejected.
 */
int tb_live_display_bind(TB_LIVE_DISPLAY *display, void *base, size_t bytes,
                         uint32_t width, uint32_t height, uint32_t stride_bytes);

/* Restore the three upstream bands only after all three stages were reached.
 * This paints their exact byte ranges, including any row padding therein.
 * Later status/QR updates do not write those ranges.
 */
int tb_live_display_bands(TB_LIVE_DISPLAY *display);

/* One complete 121x121 BGRA32 QR bitmap, including its encoder's quiet zone.
 * Source alpha is ignored; scanout is always opaque. No allocation or encoding.
 */
int tb_live_display_qr(TB_LIVE_DISPLAY *display, const uint32_t *bgra,
                       size_t pixels, uint32_t side);

/* slot 0 is the fixed snapshot on the left, slot 1 the live transfer on the
 * right. When DualQr is zero both slots use the same rectangle; the caller
 * must alternate them. The renderer does not change either transfer session.
 */
int tb_live_display_qr_slot(TB_LIVE_DISPLAY *display, const uint32_t *bgra,
                            size_t pixels, uint32_t side, unsigned slot);

/* One bounded ASCII line, truncated at the right edge. Unsupported bytes use ?.
 * It is painted above the QR and below the three color bands.
 */
int tb_live_display_status(TB_LIVE_DISPLAY *display, const char *text);

#endif
