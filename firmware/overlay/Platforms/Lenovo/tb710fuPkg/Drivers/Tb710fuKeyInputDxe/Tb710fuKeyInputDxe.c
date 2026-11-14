/** @file
  The TB710FU power and volume keys as a SimpleTextIn device.

  The boot manager navigates with ordinary console keys. This driver reads the three hardware keys through the SPMI PMIC arbiter
  (Tb710fuSpmiKeys.h carries the register facts) and presents them as keys: volume up = Up, volume down = Down, power = Enter. It only
  reads; polling stops when boot services end, so nothing is left running for the kernel, which uses the same arbiter later.

  Derived from pinned uefi-sheng 66421cb7, Platforms/Xiaomi/shengPkg/Drivers/
  ShengKeyInputDxe. Board facts and limits: docs/UEFI_INPUT.md.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/UefiLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SimpleTextIn.h>
#include <Guid/EventGroup.h>
#include <Guid/GlobalVariable.h>

#define TB710FU_SPMI_READ32(Address)         MmioRead32 ((UINTN)(Address))
#define TB710FU_SPMI_WRITE32(Address, Value) MmioWrite32 ((UINTN)(Address), (Value))
#define TB710FU_SPMI_BARRIER()               MemoryFence ()
#include "Tb710fuSpmiKeys.h"

#define POLL_MS         20
#define REPEAT_DELAY    25     // polls (500 ms) before a held volume key repeats
#define REPEAT_PERIOD   7      // polls (140 ms) between repeats
#define QUEUE_SIZE      16
#define MAX_FAILURES    50     // consecutive failed samples before polling stops

#define SPMI_CORE_BASE    0x0C400000UL
#define SPMI_OBSRVR_BASE  0x0C440000UL
#define SPMI_CNFG_BASE    0x0C42D000UL

typedef struct {
  VENDOR_DEVICE_PATH          Vendor;
  EFI_DEVICE_PATH_PROTOCOL    End;
} TB710FU_KEY_DEVICE_PATH;

STATIC TB710FU_KEY_DEVICE_PATH  mPath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    { 0x84EF05F1, 0xCE1C, 0x4D4C, { 0x81, 0x7E, 0x79, 0xD5, 0x86, 0xEF, 0xDA, 0xEA } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC struct sk_arbiter  mArbiter;
STATIC struct sk_keys     mKeys;
STATIC struct sk_filter   mFilter;
STATIC EFI_INPUT_KEY      mQueue[QUEUE_SIZE];
STATIC UINTN              mHead, mCount;
STATIC UINTN              mFailures;
STATIC EFI_EVENT          mTimer, mExit;
STATIC EFI_HANDLE         mHandle;

STATIC
EFI_STATUS
EFIAPI
KeyReset (
  IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  IN BOOLEAN                         ExtendedVerification
  )
{
  EFI_TPL  Tpl = gBS->RaiseTPL (TPL_NOTIFY);

  mHead  = 0;
  mCount = 0;
  gBS->RestoreTPL (Tpl);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
KeyRead (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                   *Key
  )
{
  EFI_TPL  Tpl;

  if (Key == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Tpl = gBS->RaiseTPL (TPL_NOTIFY);

  if (mCount == 0) {
    gBS->RestoreTPL (Tpl);
    return EFI_NOT_READY;
  }

  *Key  = mQueue[mHead];
  mHead = (mHead + 1) % QUEUE_SIZE;
  mCount--;
  gBS->RestoreTPL (Tpl);
  return EFI_SUCCESS;
}

STATIC EFI_SIMPLE_TEXT_INPUT_PROTOCOL  mInput = { KeyReset, KeyRead, NULL };

// WaitForKey is checked by WaitForEvent/CheckEvent: it must be signaled again while keys remain queued.
STATIC
VOID
EFIAPI
KeyWait (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mCount != 0) {
    gBS->SignalEvent (Event);
  }
}

STATIC
VOID
Push (
  IN UINT16  Scan,
  IN CHAR16  Unicode
  )
{
  EFI_TPL Tpl = gBS->RaiseTPL (TPL_NOTIFY);

  if (mCount == QUEUE_SIZE) {
    gBS->RestoreTPL (Tpl);
    return;
  }

  mQueue[(mHead + mCount) % QUEUE_SIZE].ScanCode    = Scan;
  mQueue[(mHead + mCount) % QUEUE_SIZE].UnicodeChar = Unicode;
  mCount++;
  gBS->SignalEvent (mInput.WaitForKey);
  gBS->RestoreTPL (Tpl);
}

STATIC
VOID
EFIAPI
Poll (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  UINT8  Pressed;
  UINT8  Out;

  if (sk_sample (&mArbiter, &mKeys, &Pressed) != 0) {
    if (++mFailures >= MAX_FAILURES) {
      gBS->SetTimer (mTimer, TimerCancel, 0);
      DEBUG ((DEBUG_ERROR, "Tb710fuKeyInput: too many failed samples, polling stopped\n"));
    }

    return;
  }

  mFailures = 0;
  Out       = sk_step (&mFilter, Pressed, REPEAT_DELAY, REPEAT_PERIOD);
  if (Out & SK_KEY_VOLUP) {
    Push (SCAN_UP, 0);
  }

  if (Out & SK_KEY_VOLDOWN) {
    Push (SCAN_DOWN, 0);
  }

  if (Out & SK_KEY_POWER) {
    Push (SCAN_NULL, CHAR_CARRIAGE_RETURN);
  }
}

STATIC
VOID
EFIAPI
OnExitBootServices (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  gBS->SetTimer (mTimer, TimerCancel, 0);
}

// Build a validated multi-instance ConIn list; preserve the old value for rollback.
// An absent variable is created volatile. This does not alter any flash policy.
STATIC
EFI_STATUS
PrepareConIn (
  OUT UINT8                     **Old,
  OUT UINTN                     *OldSize,
  OUT UINT32                    *Attributes,
  OUT EFI_DEVICE_PATH_PROTOCOL  **New,
  OUT BOOLEAN                   *Changed
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *Walker, *Instance;
  UINTN                     Size;

  *Old = NULL;
  *OldSize = 0;
  *New = NULL;
  *Changed = FALSE;
  *Attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
  Status = gRT->GetVariable (EFI_CON_IN_VARIABLE_NAME, &gEfiGlobalVariableGuid, Attributes, OldSize, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    if (*OldSize < END_DEVICE_PATH_LENGTH || *OldSize > 0x10000) {
      return EFI_COMPROMISED_DATA;
    }
    *Old = AllocatePool (*OldSize);
    if (*Old == NULL) { return EFI_OUT_OF_RESOURCES; }
    Status = gRT->GetVariable (EFI_CON_IN_VARIABLE_NAME, &gEfiGlobalVariableGuid, Attributes, OldSize, *Old);
    if (EFI_ERROR (Status)) { return Status; }
    if (!IsDevicePathValid ((EFI_DEVICE_PATH_PROTOCOL *)*Old, *OldSize) ||
        GetDevicePathSize ((EFI_DEVICE_PATH_PROTOCOL *)*Old) != *OldSize) {
      return EFI_COMPROMISED_DATA;
    }
    Walker = (EFI_DEVICE_PATH_PROTOCOL *)*Old;
    while (Walker != NULL) {
      Instance = GetNextDevicePathInstance (&Walker, &Size);
      if (Instance == NULL) { return EFI_OUT_OF_RESOURCES; }
      if (Size == sizeof (mPath) && CompareMem (Instance, &mPath, Size) == 0) {
        FreePool (Instance);
        return EFI_SUCCESS;
      }
      FreePool (Instance);
    }
  } else if (Status != EFI_NOT_FOUND) {
    return EFI_ERROR (Status) ? Status : EFI_COMPROMISED_DATA;
  }

  *New = AppendDevicePathInstance ((EFI_DEVICE_PATH_PROTOCOL *)*Old, (EFI_DEVICE_PATH_PROTOCOL *)&mPath);
  if (*New == NULL) { return EFI_OUT_OF_RESOURCES; }
  *Changed = TRUE;
  return EFI_SUCCESS;
}

STATIC
VOID
CloseEvents (VOID)
{
  if (mTimer != NULL) {
    gBS->SetTimer (mTimer, TimerCancel, 0);
    gBS->CloseEvent (mTimer);
    mTimer = NULL;
  }
  if (mExit != NULL) { gBS->CloseEvent (mExit); mExit = NULL; }
  if (mInput.WaitForKey != NULL) {
    gBS->CloseEvent (mInput.WaitForKey);
    mInput.WaitForKey = NULL;
  }
}

EFI_STATUS
EFIAPI
Tb710fuKeyInputDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                Status, Rollback;
  UINT8                     *Old = NULL, Initial;
  UINTN                     OldSize = 0;
  UINT32                    Attributes;
  EFI_DEVICE_PATH_PROTOCOL  *New = NULL;
  BOOLEAN                   Changed = FALSE, Written = FALSE;
  int                       Rc;

  mArbiter.core = (volatile UINT32 *)SPMI_CORE_BASE;
  mArbiter.cnfg = (volatile UINT32 *)SPMI_CNFG_BASE;
  mArbiter.obs  = (volatile UINT32 *)SPMI_OBSRVR_BASE;
  mArbiter.ee   = 0;
  Rc = sk_open (&mArbiter, &mKeys);
  if (Rc == 0) { Rc = sk_sample (&mArbiter, &mKeys, &Initial); }
  if (Rc != 0) {
    DEBUG ((DEBUG_ERROR, "Tb710fuKeyInput: initialization failed (%d), no console installed\n", Rc));
    return EFI_DEVICE_ERROR;
  }
  // A power key held through boot must be released before it can confirm.
  mFilter.last = mFilter.stable = Initial;
  DEBUG ((DEBUG_INFO, "Tb710fuKeyInput: v%08x PON apid %d, GPIO6 apid %d\n",
          MmioRead32 ((UINTN)mArbiter.core), mKeys.pon, mKeys.gpio));

  Status = PrepareConIn (&Old, &OldSize, &Attributes, &New, &Changed);
  if (EFI_ERROR (Status)) { goto Done; }
  Status = gBS->CreateEvent (EVT_NOTIFY_WAIT, TPL_NOTIFY, KeyWait, NULL, &mInput.WaitForKey);
  if (EFI_ERROR (Status)) { goto Done; }
  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Poll, NULL, &mTimer);
  if (EFI_ERROR (Status)) { goto Done; }
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, OnExitBootServices, NULL,
                              &gEfiEventExitBootServicesGuid, &mExit);
  if (EFI_ERROR (Status)) { goto Done; }
  Status = gBS->SetTimer (mTimer, TimerPeriodic, EFI_TIMER_PERIOD_MILLISECONDS (POLL_MS));
  if (EFI_ERROR (Status)) { goto Done; }
  if (Changed) {
    Status = gRT->SetVariable (EFI_CON_IN_VARIABLE_NAME, &gEfiGlobalVariableGuid, Attributes, GetDevicePathSize (New), New);
    if (EFI_ERROR (Status)) { goto Done; }
    Written = TRUE;
  }
  // Final fallible operation: an error cannot leave published callbacks behind.
  Status = gBS->InstallMultipleProtocolInterfaces (&mHandle, &gEfiDevicePathProtocolGuid, &mPath,
                                                  &gEfiSimpleTextInProtocolGuid, &mInput, NULL);
  if (EFI_ERROR (Status)) { goto Done; }
  DEBUG ((DEBUG_INFO, "Tb710fuKeyInput: ConIn installed, volume +/- navigate, power confirms\n"));
  // ConPlatform/ConSplitter may dispatch later; BDS also connects this path.
  gBS->ConnectController (mHandle, NULL, NULL, TRUE);

Done:
  if (EFI_ERROR (Status)) {
    CloseEvents ();
    if (Written) {
      Rollback = gRT->SetVariable (EFI_CON_IN_VARIABLE_NAME, &gEfiGlobalVariableGuid, Attributes, OldSize, Old);
      if (EFI_ERROR (Rollback)) { DEBUG ((DEBUG_ERROR, "Tb710fuKeyInput: ConIn rollback failed: %r\n", Rollback)); }
    }
    DEBUG ((DEBUG_ERROR, "Tb710fuKeyInput: install failed: %r\n", Status));
  }
  if (Old != NULL) { FreePool (Old); }
  if (New != NULL) { FreePool (New); }
  return Status;
}
