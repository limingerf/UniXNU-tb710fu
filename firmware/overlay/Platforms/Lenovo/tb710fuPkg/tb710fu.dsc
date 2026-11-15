##
# Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
# Copyright (c) 2014, Linaro Limited. All rights reserved.
# Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
# Copyright (c) 2018, Bingxing Wang. All rights reserved.
# Copyright (c) Microsoft Corporation.
# SPDX-License-Identifier: BSD-2-Clause-Patent
#
# TB710FU source-only policy, reviewed against uefi-sheng@66421cb7 and
# bmk-keyeast/UniXNU-tb710fu@23235249. See docs/UEFI_AUDIT.md.
##

[Defines]
  PLATFORM_NAME                  = tb710fu
  PLATFORM_GUID                  = 5B0C9E5A-7D1F-4E52-9A0B-71F0C0FEE710
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/tb710fuPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = tb710fuPkg/tb710fu.fdf

  # These gates are intentionally closed. The source-only volume contains
  # no vendor clock driver, PMIC service, display driver, button driver or
  # firmware blobs.
  USE_CUSTOM_DISPLAY_DRIVER      = 0
  TB710FU_VENDOR_HW              = 0
  # Minimal UFS publish path (the stock Qualcomm IO/clock/UFS DXE chain only).
  #
  # 0 = Mu drivers only (current audited baseline; ABL keeps scanout).
  # 1 = also load the smallest stock Qualcomm chain that lets the UFS host
  #     driver publish the UFS block device, so firmware can read/write devinfo
  #     and partitions. Deliberately excludes the display, graphics, DAL,
  #     SPI/I2C, SDCC and PIL/Tz modules: the 2026-11-11 all-vendor boot went
  #     black while those loaded (see docs/UEFI_AUDIT.md). This gate does NOT
  #     re-enable that set, and Tools/check-board.py enforces both the allow
  #     list and the exclusions.
  TB710FU_ENABLE_UFS             = 0
  SOC_TYPE                       = 3
  DEVICE_MODEL                   = 0
  ENABLE_SECUREBOOT              = 0

!if $(USE_CUSTOM_DISPLAY_DRIVER) != 0
  !error TB710FU source-only overlay does not implement vendor display initialization
!endif
!if $(TB710FU_VENDOR_HW) != 0
  !error TB710FU source-only overlay does not include qualified vendor hardware inputs
!endif
!if $(ENABLE_SECUREBOOT) != 0
  !error TB710FU overlay has no qualified UEFI Secure Boot key and policy inputs
!endif

# Lanai is the SM8650 platform. Do not inherit sheng's SM8550 board package.
!include LanaiPkg/LanaiPkg.dsc.inc

[PcdsFixedAtBuild]
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0xA760D000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  # Lanai defaults to 8; the open SMBIOS consumer publishes this count.
  # This override reports the cut six-core SKU and does not implement SMP.
  gArmPlatformTokenSpaceGuid.PcdCoreCount|6

  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Lenovo"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"TB710FU"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Xiaoxin Pad Pro GT"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"TB710FU"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"TB710FU"

  # Reference scanout geometry. The inherited SimpleFb implementation assumes
  # packed BGRA at this width; actual ABL stride/format still need device proof.
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|3200
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2000
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  # Vendor DXE is absent: do not scan leftover stock XBL HOBs for it.
  gQcomPkgTokenSpaceGuid.PcdBuildXblHobs|FALSE

  # Filename prefix for the OneSend QR log transfer (<model>-<date>-<time>.log).
  gUniXNUPkgTokenSpaceGuid.PcdUniXNUDeviceModel|"TB710FU"

[LibraryClasses]
  MemoryMapLib|tb710fuPkg/Library/MemoryMapLib/MemoryMapLib.inf
  ConfigurationMapLib|tb710fuPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf
  UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf
  FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf
  # OneSend QR exporter for the ramoops console snapshot (Tb710fuRamoopsDxe).
  QrEncoderLib|MsGraphicsPkg/Library/QrEncoderLib/QrEncoderLib.inf

[Components]
  # DiskIoDxe, PartitionDxe, FAT, Unicode collation and SimpleFb are built by
  # the pinned Silicium DSC inherited through Lanai/Qcom. Include/DXE.inc
  # explicitly puts them in this board's volume.
  UniXNUPkg/Application/UniXNU/UniXNU.inf
  UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.inf
  DarwinBootPkg/Driver/DarwinBoot.inf
  # Source-only PMIC status reads; no vendor initialization dependency chain.
  tb710fuPkg/Drivers/Tb710fuKeyInputDxe/Tb710fuKeyInputDxe.inf
  tb710fuPkg/Drivers/Tb710fuRamoopsDxe/Tb710fuRamoopsDxe.inf

!if $(TB710FU_ENABLE_UFS) == 1
  # Stock Qualcomm prebuilt DXE modules that publish UFS. These are PE32
  # binaries under the firmware binary set, not sources, so the board package
  # only declares them for the volume. See Include/DXE.inc for the rationale
  # and the deliberately excluded 2026-11-11 black-screen set.
  Binaries/tb710fu/QcomPkg/Drivers/SmemDxe/SmemDxe.inf
  Binaries/tb710fu/QcomPkg/Drivers/HWIODxe/HWIODxe.inf
  Binaries/tb710fu/QcomPkg/Drivers/CmdDbDxe/CmdDbDxe.inf
  Binaries/tb710fu/QcomPkg/Drivers/PwrUtilsDxe/PwrUtilsDxe.inf
  Binaries/tb710fu/QcomPkg/Drivers/NpaDxe/NpaDxe.inf
  Binaries/tb710fu/QcomPkg/Drivers/RpmhDxe/RpmhDxe.inf
  Binaries/tb710fu/QcomPkg/Drivers/ClockDxe/ClockDxe.inf
  Binaries/tb710fu/QcomPkg/Drivers/UFSDxe/UFSDxe.inf
!endif
