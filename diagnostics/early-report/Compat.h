/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Minimal freestanding services for unchanged pinned Microsoft QrEncoderLib.
 * No UEFI services are called. Allocation is a bounded fatal-report arena.
 */
#ifndef TB_EARLY_COMPAT_H
#define TB_EARLY_COMPAT_H
#include <stdint.h>
#include <stddef.h>
typedef uint8_t UINT8; typedef int8_t INT8;
typedef uint16_t UINT16; typedef int16_t INT16;
typedef uint32_t UINT32; typedef int32_t INT32;
typedef uint64_t UINT64; typedef int64_t INT64;
typedef uintptr_t UINTN; typedef intptr_t INTN;
typedef int BOOLEAN; typedef char CHAR8; typedef uint16_t CHAR16;
typedef void VOID; typedef UINTN EFI_STATUS;
#define IN
#define OUT
#define OPTIONAL
#define EFIAPI
#define CONST const
#define STATIC static
#define TRUE 1
#define FALSE 0
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER 2
#define EFI_OUT_OF_RESOURCES 9
#define EFI_NO_MEDIA 12
#define EFI_ERROR(s) ((s) != EFI_SUCCESS)
#define MAX_INTN INTPTR_MAX
#define DEBUG(x) ((void)0)
#define DEBUG_INFO 0
#define DEBUG_ERROR 0
#define ASSERT(x) ((void)0)
typedef struct { UINT8 Blue, Green, Red, Reserved; } EFI_GRAPHICS_OUTPUT_BLT_PIXEL;
VOID *AllocatePool(UINTN bytes);
VOID *AllocateZeroPool(UINTN bytes);
VOID *AllocateCopyPool(UINTN bytes, CONST VOID *data);
VOID FreePool(VOID *p);
VOID *CopyMem(VOID *out, CONST VOID *in, UINTN bytes);
VOID *SetMem32(VOID *out, UINTN bytes, UINT32 value);
VOID *memcpy(VOID *out, CONST VOID *in, UINTN bytes);
VOID *memset(VOID *out, int c, UINTN bytes);
#endif
