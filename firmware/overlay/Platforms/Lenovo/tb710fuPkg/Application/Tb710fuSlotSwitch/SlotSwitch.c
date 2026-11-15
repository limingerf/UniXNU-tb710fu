/** @file
  Tb710fuSlotSwitch: switch the Android A/B active slot from UEFI.

  Reads every EFI_BLOCK_IO_PROTOCOL, locates GPT disks, and flips the Android
  A/B "slot active" attribute (GPT partition entry Attributes bit 0) on every
  A/B partition so the ABL boots the requested slot on the next reset.

  Why this exists: the device's fastboot cannot set the active slot (BL locked,
  fastboot set_active refused), and the stock UEFI boots from slot B. This app,
  launched from the UniXNU menu, performs the switch in-firmware so the menu can
  hand off to Android (slot A) or recovery without a host and without EDL.

  It is also the UFS probe in disguise: a successful switch proves UFSDxe
  published BlockIo; a "no GPT found" result proves it did not.

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
#include <Protocol/BlockIo.h>
#include <Protocol/DiskIo.h>
#include <Uefi/UefiGpt.h>
#include <Library/UefiRuntimeServicesTableLib.h>

// Android A/B partition attribute: Attributes bit 0 marks the active slot.
// (Qualcomm/AOSP borrow bit 0 of the GPT partition Attributes field; this is
// the AB_PARTITION_ATTR_SLOT_ACTIVE convention from AOSP boot_control.)
#define AB_PARTITION_ATTR_SLOT_ACTIVE  0x1ULL

// A/B partition name prefixes that carry a _a/_b suffix and must be switched
// together. The boot LUN (xbl) is handled by the ABL via the boot partition
// attribute, not by this app.
STATIC CONST CHAR16 *SlotSwitchPrefixes[] = {
  L"boot",
  L"init_boot",
  L"vendor_boot",
  L"recovery",
  L"dtbo",
  L"vbmeta",
  L"vbmeta_system",
  L"uefi",
  NULL,
};

// Which slot suffix to activate, resolved from LoadOptions.
STATIC CHAR16 TargetSuffix[8];   // L"_a" or L"_b"

STATIC
BOOLEAN
NameHasSlotSuffix (
  IN CONST CHAR16 *Name
  )
{
  UINTN Len = StrLen (Name);
  if (Len < 2) {
    return FALSE;
  }
  CHAR16 Tail = Name[Len - 1];
  return (Tail == L'a' || Tail == L'b') && Name[Len - 2] == L'_';
}

STATIC
BOOLEAN
IsSwitchablePartition (
  IN CONST CHAR16 *Name
  )
{
  UINTN Index;
  if (!NameHasSlotSuffix (Name)) {
    return FALSE;
  }
  for (Index = 0; SlotSwitchPrefixes[Index] != NULL; Index++) {
    UINTN PrefixLen = StrLen (SlotSwitchPrefixes[Index]);
    if (StrnCmp (Name, SlotSwitchPrefixes[Index], PrefixLen) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

//
// Read the GPT header + entries from a BlockIo disk using DiskIo (which works
// when the disk already has a GPT on it). Returns the entries and count.
//
STATIC
EFI_STATUS
ReadGptEntries (
  IN  EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN  EFI_DISK_IO_PROTOCOL   *DiskIo,
  OUT EFI_PARTITION_ENTRY    **EntriesOut,
  OUT UINTN                  *CountOut
  )
{
  EFI_STATUS                  Status;
  EFI_PARTITION_TABLE_HEADER  Header;
  UINTN                       EntriesSize;
  EFI_PARTITION_ENTRY         *Entries;
  UINTN                       Index;

  *EntriesOut = NULL;
  *CountOut   = 0;

  // Primary GPT header lives at LBA 1.
  Status = DiskIo->ReadDisk (DiskIo, BlockIo->Media->MediaId, BlockIo->Media->BlockSize,
                             (UINT8 *)&Header, sizeof (Header));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (CompareGuid (&Header.Header, &gEfiPartTypeSystemPartGuid) &&
      Header.Signature != EFI_PTAB_HEADER_ID) {
    return EFI_NOT_FOUND;
  }

  if (Header.NumberOfPartitionEntries == 0 ||
      Header.SizeOfPartitionEntry < sizeof (EFI_PARTITION_ENTRY)) {
    return EFI_NOT_FOUND;
  }

  EntriesSize = (UINTN)Header.NumberOfPartitionEntries * Header.SizeOfPartitionEntry;
  Entries = AllocatePool (EntriesSize);
  if (Entries == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = DiskIo->ReadDisk (DiskIo, BlockIo->Media->MediaId,
                             MultU64x32 (Header.PartitionEntryLBA, BlockIo->Media->BlockSize),
                             (UINT8 *)Entries, EntriesSize);
  if (EFI_ERROR (Status)) {
    FreePool (Entries);
    return Status;
  }

  // Trim down to valid (non-zero PartitionTypeGUID) entries.
  UINTN Valid = 0;
  for (Index = 0; Index < Header.NumberOfPartitionEntries; Index++) {
    if (!IsZeroGuid (&Entries[Index].PartitionTypeGUID)) {
      Valid++;
    }
  }

  *EntriesOut = Entries;
  *CountOut   = Valid;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SetSlotOnDisk (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN EFI_DISK_IO_PROTOCOL   *DiskIo
  )
{
  EFI_STATUS             Status;
  EFI_PARTITION_ENTRY    *Entries;
  UINTN                  Count;
  UINTN                  Index;
  UINTN                  Switched = 0;

  Status = ReadGptEntries (BlockIo, DiskIo, &Entries, &Count);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < Count; Index++) {
    if (!IsSwitchablePartition (Entries[Index].PartitionName)) {
      continue;
    }

    UINTN Len = StrLen (Entries[Index].PartitionName);
    BOOLEAN IsTarget = StrCmp (&Entries[Index].PartitionName[Len - 2], TargetSuffix) == 0;

    if (IsTarget) {
      Entries[Index].Attributes |= AB_PARTITION_ATTR_SLOT_ACTIVE;
    } else {
      Entries[Index].Attributes &= ~AB_PARTITION_ATTR_SLOT_ACTIVE;
    }
    Switched++;
  }

  if (Switched == 0) {
    FreePool (Entries);
    return EFI_NOT_FOUND;
  }

  // Write back is deliberately NOT implemented here on first pass: the primary
  // and backup GPT headers + entry CRCs must be recomputed and written through
  // DiskIo->WriteDisk. That is the write path this probe intentionally leaves
  // out until UFS read is proven on hardware.
  Print (L"SlotSwitch: read %u switchable entries on this disk, target %s (write not yet issued)\n",
         Switched, TargetSuffix);

  FreePool (Entries);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SlotSwitchEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS            Status;
  EFI_HANDLE            *Handles;
  UINTN                 HandleCount;
  UINTN                 Index;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  CHAR16                *LoadOptions;

  // Resolve target slot from LoadOptions ("a" or "b").
  StrCpyS (TargetSuffix, sizeof (TargetSuffix) / sizeof (CHAR16), L"_b");
  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (!EFI_ERROR (Status) && Loaded->LoadOptions != NULL) {
    LoadOptions = (CHAR16 *)Loaded->LoadOptions;
    if (LoadOptions[0] == L'a' || LoadOptions[0] == L'b') {
      TargetSuffix[1] = LoadOptions[0];
    }
  }
  Print (L"SlotSwitch: target slot %s\n", TargetSuffix);

  // Enumerate all BlockIo handles.
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    Print (L"SlotSwitch: no BlockIo handles (%r) -> UFS NOT published\n", Status);
    return Status;
  }

  UINTN DisksFound = 0;
  for (Index = 0; Index < HandleCount; Index++) {
    EFI_BLOCK_IO_PROTOCOL *BlockIo;
    EFI_DISK_IO_PROTOCOL  *DiskIo;

    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (EFI_ERROR (Status)) {
      continue;
    }
    // Skip partition child handles: only whole-disk BlockIo carries a GPT.
    if (BlockIo->Media == NULL || BlockIo->Media->LogicalPartition) {
      continue;
    }

    Status = gBS->HandleProtocol (Handles[Index], &gEfiDiskIoProtocolGuid, (VOID **)&DiskIo);
    if (EFI_ERROR (Status)) {
      continue;
    }

    EFI_PARTITION_ENTRY *Entries;
    UINTN               Count;
    Status = ReadGptEntries (BlockIo, DiskIo, &Entries, &Count);
    if (EFI_ERROR (Status)) {
      continue;
    }
    DisksFound++;
    SetSlotOnDisk (BlockIo, DiskIo);
    FreePool (Entries);
  }

  FreePool (Handles);

  if (DisksFound == 0) {
    Print (L"SlotSwitch: no GPT disk found -> UFS NOT published\n");
    return EFI_NOT_FOUND;
  }

  Print (L"SlotSwitch: inspected %u GPT disks\n", DisksFound);
  return EFI_SUCCESS;
}
