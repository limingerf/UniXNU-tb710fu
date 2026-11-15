/** @file
  Read-only snapshot and UniXNU Tools viewer for the TB710FU ramoops console.
  No disk, variable, PMIC, USB or reset writes. Source: docs/UEFI_LOG_VIEWER.md.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/UniXNUMenuPlugin.h>
#include "Ramoops.h"
#include "Font.h"

#define RAMOOPS_CONSOLE_BASE 0xAC340000U
#define MAX_COLUMNS 160U

STATIC UINT8 *mSnapshot;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mBackground = { 15, 12, 11, 0 };
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mText = { 245, 243, 242, 0 };

STATIC
EFI_STATUS
DrawLine (
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixels,
  CONST UINT8 *Text, UINTN Length, UINTN Columns,
  UINTN X, UINTN Y, UINTN Scale
  )
{
  UINTN Width = Columns * 8 * Scale;
  UINTN Height = 19 * Scale;
  UINTN I, Row, Col, Sx, Sy, Base;
  CONST UINT8 *Glyph;

  for (I = 0; I < Width * Height; I++) {
    Pixels[I] = mBackground;
  }
  for (I = 0; I < Length; I++) {
    Glyph = mRamoopsFont[Text[I] - 32];
    for (Row = 0; Row < 19; Row++) {
      for (Col = 0; Col < 8; Col++) {
        if ((Glyph[Row] & (0x80 >> Col)) == 0) {
          continue;
        }
        Base = Row * Scale * Width + I * 8 * Scale + Col * Scale;
        for (Sy = 0; Sy < Scale; Sy++) {
          for (Sx = 0; Sx < Scale; Sx++) {
            Pixels[Base + Sy * Width + Sx] = mText;
          }
        }
      }
    }
  }
  return Gop->Blt (Gop, Pixels, EfiBltBufferToVideo, 0, 0,
                   X, Y, Width, Height, 0);
}

STATIC EFI_STATUS EFIAPI ViewLog (
  UNIXNU_MENU_PLUGIN_PROTOCOL *This,
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
  CONST UNIXNU_PLUGIN_RECT *Content,
  UNIXNU_MENU_PLUGIN_DRAW_STATUS DrawStatus
  );

STATIC UNIXNU_MENU_PLUGIN_PROTOCOL mPlugin0 = {
  UNIXNU_MENU_PLUGIN_PROTOCOL_SIGNATURE, UNIXNU_MENU_PLUGIN_PROTOCOL_VERSION,
  L"Last XNU log (ramoops ECC 0)", ViewLog
};
STATIC UNIXNU_MENU_PLUGIN_PROTOCOL mPlugin16 = {
  UNIXNU_MENU_PLUGIN_PROTOCOL_SIGNATURE, UNIXNU_MENU_PLUGIN_PROTOCOL_VERSION,
  L"Last XNU log (ramoops ECC 16)", ViewLog
};

STATIC
EFI_STATUS
EFIAPI
ViewLog (
  UNIXNU_MENU_PLUGIN_PROTOCOL *This,
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
  CONST UNIXNU_PLUGIN_RECT *Content,
  UNIXNU_MENU_PLUGIN_DRAW_STATUS DrawStatus
  )
{
  EFI_STATUS Status;
  EFI_INPUT_KEY Key;
  TR_VIEW View;
  UINT32 Ecc = This == &mPlugin16 ? 16 : 0;
  UINT32 *Lines;
  UINT32 Offset, Next, Length;
  UINTN Scale, Columns, Rows, LineCount, Top, Row, EventIndex, LastTop;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixels;
  CHAR16 Footer[160];
  UINT8 Text[MAX_COLUMNS];
  CONST CHAR8 *Message;
  BOOLEAN Valid;

  if (mSnapshot == NULL || Gop == NULL || Gop->Mode == NULL ||
      Gop->Mode->Info == NULL || Content == NULL || DrawStatus == NULL ||
      gST->ConIn == NULL) {
    return EFI_UNSUPPORTED;
  }
  if (Content->X >= Gop->Mode->Info->HorizontalResolution ||
      Content->Y >= Gop->Mode->Info->VerticalResolution ||
      Content->Width > Gop->Mode->Info->HorizontalResolution - Content->X ||
      Content->Height > Gop->Mode->Info->VerticalResolution - Content->Y ||
      Content->Width < 80 || Content->Height < 40) {
    return EFI_UNSUPPORTED;
  }

  Scale = MAX (1, MIN (3, Content->Width / 960));
  Columns = MIN (MAX_COLUMNS, (Content->Width - 16) / (8 * Scale));
  Rows = (Content->Height - 16) / (20 * Scale);
  if (Rows == 0 || Columns == 0) {
    return EFI_UNSUPPORTED;
  }
  Pixels = AllocatePool (Columns * 8 * Scale * 19 * Scale * sizeof (*Pixels));
  Lines = AllocatePool ((TR_ZONE_BYTES + 1) * sizeof (*Lines));
  if (Pixels == NULL || Lines == NULL) {
    if (Pixels != NULL) { FreePool (Pixels); }
    if (Lines != NULL) { FreePool (Lines); }
    return EFI_OUT_OF_RESOURCES;
  }

  Valid = (BOOLEAN)TrParse (mSnapshot, TR_ZONE_BYTES, Ecc, &View);
  LineCount = 0;
  Offset = 0;
  while (Valid && Offset < View.Size) {
    Lines[LineCount++] = Offset;
    Offset = TrLine (mSnapshot, &View, Offset, (UINT32)Columns, NULL, &Length);
  }
  LastTop = LineCount > Rows ? LineCount - Rows : 0;
  Top = LastTop; /* The last messages are usually the useful ones. */
  UnicodeSPrint (Footer, sizeof (Footer), L"0x%08x: sig=%08x start=%u size=%u capacity=%u ECC=%u",
                 RAMOOPS_CONSOLE_BASE, View.Signature, View.Start, View.Size, View.Capacity, Ecc);
  DrawStatus (This, 1, Footer);
  DrawStatus (This, 2, L"Volume +/-: page up/down   Power: back   Snapshot only; no ECC repair");

  /* Discard any queued press used to enter this tool. */
  while (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
  }
  for (;;) {
    Status = Gop->Blt (Gop, &mBackground, EfiBltVideoFill, 0, 0,
                       Content->X, Content->Y, Content->Width, Content->Height, 0);
    if (EFI_ERROR (Status)) { break; }
    if (!Valid || LineCount == 0) {
      Message = Valid ? "Valid empty console. No log bytes were retained." :
                        "Invalid console header. Original memory was not changed.";
      Length = (UINT32)MIN (AsciiStrLen (Message), Columns);
      CopyMem (Text, Message, Length);
      Status = DrawLine (Gop, Pixels, Text, Length, Columns,
                          Content->X + 8, Content->Y + 8, Scale);
    } else {
      for (Row = 0; Row < Rows && Top + Row < LineCount; Row++) {
        Next = TrLine (mSnapshot, &View, Lines[Top + Row], (UINT32)Columns, Text, &Length);
        (VOID)Next;
        Status = DrawLine (Gop, Pixels, Text, Length, Columns,
                            Content->X + 8, Content->Y + 8 + Row * 20 * Scale, Scale);
        if (EFI_ERROR (Status)) { break; }
      }
    }
    if (EFI_ERROR (Status)) { break; }
    UnicodeSPrint (Footer, sizeof (Footer), L"Retained console: lines %u-%u / %u (DDR survival unverified)",
                   (UINT32)(LineCount ? Top + 1 : 0),
                   (UINT32)MIN (Top + Rows, LineCount), (UINT32)LineCount);
    DrawStatus (This, 0, Footer);
    do {
      Status = gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &EventIndex);
      if (EFI_ERROR (Status)) { goto Done; }
      Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    } while (Status == EFI_NOT_READY);
    if (EFI_ERROR (Status)) { break; }
    if (Key.UnicodeChar == L'\r' || Key.ScanCode == SCAN_ESC) { break; }
    if (Key.ScanCode == SCAN_UP) {
      Top = Top > Rows ? Top - Rows : 0;
    } else if (Key.ScanCode == SCAN_DOWN) {
      Top = MIN (Top + Rows, LastTop);
    }
  }
Done:
  FreePool (Lines);
  FreePool (Pixels);
  return Status;
}

EFI_STATUS
EFIAPI
Tb710fuRamoopsEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  EFI_HANDLE Handle0 = NULL;
  EFI_HANDLE Handle16 = NULL;
  (VOID)ImageHandle;
  (VOID)SystemTable;

  /* This address is inside the board's Kernel Reserv region, not the heap.
   * Capture before BDS/payload loading. Never initialize/clear the old zone.
   */
  mSnapshot = AllocatePool (TR_ZONE_BYTES);
  if (mSnapshot == NULL) { return EFI_OUT_OF_RESOURCES; }
  MemoryFence ();
  CopyMem (mSnapshot, (CONST VOID *)(UINTN)RAMOOPS_CONSOLE_BASE, TR_ZONE_BYTES);
  MemoryFence ();
  Status = gBS->InstallProtocolInterface (&Handle0, &gUniXNUMenuPluginProtocolGuid,
                                         EFI_NATIVE_INTERFACE, &mPlugin0);
  if (EFI_ERROR (Status)) { goto Failure; }
  Status = gBS->InstallProtocolInterface (&Handle16, &gUniXNUMenuPluginProtocolGuid,
                                         EFI_NATIVE_INTERFACE, &mPlugin16);
  if (EFI_ERROR (Status)) {
    /* Do not free a snapshot still referenced by a published protocol. */
    if (EFI_ERROR (gBS->UninstallProtocolInterface (Handle0, &gUniXNUMenuPluginProtocolGuid, &mPlugin0))) {
      return EFI_SUCCESS;
    }
    goto Failure;
  }
  DEBUG ((DEBUG_INFO, "Tb710fuRamoops: read-only console snapshot and Tools installed\n"));
  return EFI_SUCCESS;
Failure:
  FreePool (mSnapshot);
  mSnapshot = NULL;
  return Status;
}
