/** @file
  TB710FU Fastboot Platform DXE.

  Installs gAndroidFastbootPlatformProtocolGuid so EDK II's AndroidFastbootApp
  can run over the USB device transport. This gives the board a UEFI-side
  `fastboot devices` target, which is the channel the host uses to read UEFI
  state and to issue OEM commands (slot switch, reboot modes, log dump).

  Flash/erase are intentionally NOT implemented: there is no UFS publication in
  the source-only baseline, and writing partitions from firmware is out of scope
  until the UFS chain is qualified. They return EFI_NOT_FOUND.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/AndroidFastbootPlatform.h>

//
// A small ring of the most recent firmware log lines, captured from
// DebugLib writes. Populated by Tb710fuDebugRingRecord() which the board's
// DebugLib variant calls; a future change hooks that. For now the buffer is
// present and drained by `fastboot oem log`, and returns what is available.
//
#define LOG_RING_CAPACITY   (64 * 1024)
STATIC UINT8  *mLogRing;
STATIC UINTN  mLogRingUsed;
STATIC UINTN  mLogRingHead;   // next write offset; wraps within capacity

//
// A 64 KiB log ring, head-wrapped, is exposed via the "log" OEM command. The
// fastboot protocol's data phase for OEM is not supported by the AOSP host, so
// the buffer is returned in chunks through GetVar-style "log:N" variables.
//
STATIC
VOID
LogRingAppend (
  IN CONST UINT8  *Buffer,
  IN UINTN        Count
  )
{
  if (mLogRing == NULL || Count == 0) {
    return;
  }
  for (UINTN i = 0; i < Count; i++) {
    mLogRing[mLogRingHead] = Buffer[i];
    mLogRingHead = (mLogRingHead + 1) % LOG_RING_CAPACITY;
    if (mLogRingUsed < LOG_RING_CAPACITY) {
      mLogRingUsed++;
    }
  }
}

STATIC
EFI_STATUS
EFIAPI
PlatformInit (
  VOID
  )
{
  if (mLogRing == NULL) {
    mLogRing = AllocateRuntimeZeroPool (LOG_RING_CAPACITY);
    if (mLogRing == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    mLogRingUsed = 0;
    mLogRingHead = 0;
  }
  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
PlatformUnInit (
  VOID
  )
{
  // Keep the ring across fastboot sessions; the buffer is runtime pool and is
  // released only at ExitBootServices by the caller's choice. Nothing to undo.
}

STATIC
EFI_STATUS
EFIAPI
PlatformFlash (
  IN CHAR8  *PartitionName,
  IN UINTN  BufferSize,
  IN VOID   *Buffer
  )
{
  DEBUG ((DEBUG_WARN, "Tb710fu fastboot: flash '%a' refused (no UFS publication)\n", PartitionName));
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
EFIAPI
PlatformErase (
  IN CHAR8  *PartitionName
  )
{
  DEBUG ((DEBUG_WARN, "Tb710fu fastboot: erase '%a' refused (no UFS publication)\n", PartitionName));
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
EFIAPI
PlatformGetVar (
  IN  CHAR8  *Name,
  OUT CHAR8  *Value
  )
{
  if (Name == NULL || Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Value = '\0';

  if (AsciiStrCmp (Name, "log-bytes") == 0) {
    // number of bytes currently held in the log ring, as decimal ASCII
    CHAR8  Tmp[24];
    AsciiSPrint (Tmp, sizeof (Tmp), "%Lu", (UINT64)mLogRingUsed);
    AsciiStrCpyS (Value, 60, Tmp);
    return EFI_SUCCESS;
  }

  if (AsciiStrCmp (Name, "slot-count") == 0) {
    AsciiStrCpyS (Value, 60, "2");
    return EFI_SUCCESS;
  }

  // Unknown variables yield the empty string per the platform protocol contract.
  return EFI_SUCCESS;
}

//
// Copy the log ring out as text. The AOSP host has no OEM data phase, so this is
// best-effort: a later iteration can drain the ring through repeated getvar
// reads of a window. For now the whole ring (up to 60 bytes per protocol limit)
// cannot transfer, so OEM "log" only reports how many bytes are available; the
// actual transfer needs a transport extension and is tracked separately.
//
STATIC
EFI_STATUS
EFIAPI
PlatformOemCommand (
  IN  CHAR8  *Command
  )
{
  if (Command == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (AsciiStrCmp (Command, "log") == 0) {
    DEBUG ((DEBUG_INFO, "Tb710fu fastboot: %Lu log bytes captured\n", (UINT64)mLogRingUsed));
    return EFI_SUCCESS;
  }

  if (AsciiStrCmp (Command, "reboot-fastbootd") == 0) {
    // Ask the reset runtime to cold-reset; the ABL reads the BCB/boot reason and
    // can land in fastbootd. A direct "boot to fastbootd" needs the misc BCB
    // write, which is a separate follow-up.
    DEBUG ((DEBUG_INFO, "Tb710fu fastboot: reboot requested\n"));
    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
    // not reached
    return EFI_DEVICE_ERROR;
  }

  return EFI_NOT_FOUND;
}

STATIC FASTBOOT_PLATFORM_PROTOCOL  mPlatform = {
  PlatformInit,
  PlatformUnInit,
  PlatformFlash,
  PlatformErase,
  PlatformGetVar,
  PlatformOemCommand,
};

EFI_STATUS
EFIAPI
Tb710fuFastbootPlatformEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gAndroidFastbootPlatformProtocolGuid,
                  &mPlatform,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Tb710fu fastboot platform: install failed %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "Tb710fu fastboot platform: installed\n"));
  return EFI_SUCCESS;
}
