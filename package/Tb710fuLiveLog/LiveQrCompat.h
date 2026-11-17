/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Services for the unchanged, hash-pinned Mu encoder. Private to QR objects.
 */
#ifndef TB710FU_LIVE_QR_COMPAT_H
#define TB710FU_LIVE_QR_COMPAT_H
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
/* XNU's forced meta_features.h imports the object-like DEBUG configuration
 * macro. Only these standalone QR objects use the EDK2 function-like service;
 * leave native XNU objects and their compile flags unchanged. */
#ifdef DEBUG
#undef DEBUG
#endif
#define DEBUG(x) ((void)0)
#define DEBUG_INFO 0
#define DEBUG_ERROR 0
#define ASSERT(x) ((void)0)
typedef struct { UINT8 Blue, Green, Red, Reserved; } EFI_GRAPHICS_OUTPUT_BLT_PIXEL;
#define AllocatePool tb_live_qr_AllocatePool
#define AllocateZeroPool tb_live_qr_AllocateZeroPool
#define AllocateCopyPool tb_live_qr_AllocateCopyPool
#define FreePool tb_live_qr_FreePool
#define CopyMem tb_live_qr_CopyMem
#define SetMem32 tb_live_qr_SetMem32
#define memcpy tb_live_qr_memcpy
#define memset tb_live_qr_memset
#define QrEncodeData tb_live_qr_QrEncodeData
#define InitializeLogTables tb_live_qr_InitializeLogTables
#define CheckQrVersion tb_live_qr_CheckQrVersion
#define QR_modulo tb_live_qr_QR_modulo
#define Code7 tb_live_qr_Code7
#define Code10 tb_live_qr_Code10
#define Code13 tb_live_qr_Code13
#define Code15 tb_live_qr_Code15
#define Code16 tb_live_qr_Code16
#define Code17 tb_live_qr_Code17
#define Code18 tb_live_qr_Code18
#define Code20 tb_live_qr_Code20
#define Code22 tb_live_qr_Code22
#define Code24 tb_live_qr_Code24
#define Code26 tb_live_qr_Code26
#define Code28 tb_live_qr_Code28
#define Code30 tb_live_qr_Code30
#define gQrVersion tb_live_qr_gQrVersion
#define gQrSize tb_live_qr_gQrSize
#define gQrLevel tb_live_qr_gQrLevel
#define gQrMode tb_live_qr_gQrMode
#define gQrMask tb_live_qr_gQrMask
#define gQrBitmap tb_live_qr_gQrBitmap
#define gQrBitmapLen tb_live_qr_gQrBitmapLen
#define gCodeWords tb_live_qr_gCodeWords
#define gCodeWordCount tb_live_qr_gCodeWordCount
#define gECWords tb_live_qr_gECWords
#define gECWordCount tb_live_qr_gECWordCount
#define gBitStream tb_live_qr_gBitStream
#define gBitStreamCount tb_live_qr_gBitStreamCount
#define gQrT tb_live_qr_gQrT
#define gFlags tb_live_qr_gFlags
#define gBltBuffer tb_live_qr_gBltBuffer
#define gQrTable tb_live_qr_gQrTable
#define gAlphaNumerics tb_live_qr_gAlphaNumerics
#define gLengthBits tb_live_qr_gLengthBits
#define gFormatInfo tb_live_qr_gFormatInfo
#define gVersionInfo tb_live_qr_gVersionInfo
#define gAlignmentLocations tb_live_qr_gAlignmentLocations
#define gGeneratorPolynomials tb_live_qr_gGeneratorPolynomials
#define logTable tb_live_qr_logTable
#define alogTable tb_live_qr_alogTable
VOID *AllocatePool(UINTN bytes);
VOID *AllocateZeroPool(UINTN bytes);
VOID *AllocateCopyPool(UINTN bytes, CONST VOID *data);
VOID FreePool(VOID *p);
VOID *CopyMem(VOID *out, CONST VOID *in, UINTN bytes);
VOID *SetMem32(VOID *out, UINTN bytes, UINT32 value);
VOID *memcpy(VOID *out, CONST VOID *in, UINTN bytes);
VOID *memset(VOID *out, int value, UINTN bytes);
#endif
