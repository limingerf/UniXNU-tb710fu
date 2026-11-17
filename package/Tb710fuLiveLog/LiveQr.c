/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "LiveQrCompat.h"
#include "LiveQr.h"
#include <Library/QrEncoderLib.h>

static uint8_t arena[524288] __attribute__((aligned(16)));
static size_t used;

VOID *memcpy(VOID *out, CONST VOID *in, UINTN bytes)
{
    uint8_t *d = out; const uint8_t *s = in;
    for (size_t i = 0; i < bytes; i++) d[i] = s[i];
    return out;
}
VOID *memset(VOID *out, int value, UINTN bytes)
{
    uint8_t *d = out;
    for (size_t i = 0; i < bytes; i++) d[i] = (uint8_t)value;
    return out;
}
VOID *CopyMem(VOID *out, CONST VOID *in, UINTN bytes) { return memcpy(out, in, bytes); }
VOID *SetMem32(VOID *out, UINTN bytes, UINT32 value)
{
    uint32_t *d = out;
    for (size_t i = 0; i < bytes / 4; i++) d[i] = value;
    return out;
}
VOID *AllocatePool(UINTN bytes)
{
    if (bytes > sizeof(arena) - 15u) return NULL;
    bytes = (bytes + 15u) & ~(UINTN)15u;
    if (bytes > sizeof(arena) - used) return NULL;
    void *result = arena + used; used += bytes;
    return result;
}
VOID *AllocateZeroPool(UINTN bytes)
{
    void *result = AllocatePool(bytes);
    if (result) memset(result, 0, bytes);
    return result;
}
VOID *AllocateCopyPool(UINTN bytes, CONST VOID *in)
{
    void *result = AllocatePool(bytes);
    if (result) memcpy(result, in, bytes);
    return result;
}
VOID FreePool(VOID *p) { (void)p; }

int tb_live_qr_encode(const uint8_t *frame, size_t bytes, const uint32_t **pixels)
{
    if (!pixels) return 0;
    *pixels = NULL;
    if (!frame || bytes != 752u) return 0;
    used = 0;
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL *bitmap = NULL;
    EFI_STATUS status = QrEncodeData(QrAutoVersion, QrECLevel_M, QrByteMode, 0,
        (uint8_t *)(uintptr_t)frame, (uint16_t)bytes, 121, &bitmap);
    if (EFI_ERROR(status) || !bitmap) return 0;
    *pixels = (const uint32_t *)(const void *)bitmap;
    return 1;
}
