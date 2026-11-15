# SPDX-License-Identifier: BSD-2-Clause
# Source-only effective gate=0 module list from reference 23235249d71604f8a6f743680ef8fd1003fe7189.
# Vendor hardware/display branches removed after audit; the board input driver is source-only.
  INF MdeModulePkg/Core/Dxe/DxeMain.inf
  INF MdeModulePkg/Universal/PCD/Dxe/Pcd.inf


  INF MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf
  INF MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
  INF ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  INF MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  INF MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf


  INF MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  INF MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  INF MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf
  INF EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  INF MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf
  INF EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  INF EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  INF MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  INF ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  INF ArmPkg/Drivers/TimerDxe/TimerDxe.inf


  INF MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf

  INF MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  INF MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf

  INF FatPkg/EnhancedFatDxe/Fat.inf
  INF MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf

  # UniXNU: the payload FAT arrives as the ABL initrd; DarwinBoot must live in the
  # firmware volume (a loader read from a volume is refused by image verification).
  INF UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.inf
  INF UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf
  INF DarwinBootPkg/Driver/DarwinBoot.inf
  INF UniXNUPkg/Application/UniXNU/UniXNU.inf


  # Publish the physical-key console before the boot manager connects ConIn.
  INF tb710fuPkg/Drivers/Tb710fuKeyInputDxe/Tb710fuKeyInputDxe.inf
  INF tb710fuPkg/Drivers/Tb710fuRamoopsDxe/Tb710fuRamoopsDxe.inf
  INF MdeModulePkg/Universal/BdsDxe/BdsDxe.inf


  INF EmbeddedPkg/SimpleTextInOutSerial/SimpleTextInOutSerial.inf
  INF MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
  INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  INF MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf


  INF SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf


  INF MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf

!if $(TB710FU_ENABLE_UFS) == 1
  # Minimal stock Qualcomm chain for UFS publication. DEPEX on every one of
  # these is TRUE, so APRIORI.inc decides the order; keeping the same relative
  # order here keeps DXE.inc and APRIORI.inc consistent.
  #
  # Coverage:
  #   HWIODxe      - HWIO protocol (UFS register accessors)
  #   NpaDxe       - NPA protocol (UFS clock votes)
  #   ClockDxe     - clock driver (UFS core/PHY clocks)
  #   RpmhDxe      - RPMh (clock/power resource requests)
  #   UFSDxe       - the UFS host controller driver itself
  #   CmdDbDxe     - command DB (RPMh dependency)
  #   PwrUtilsDxe  - power utilities
  #   SmemDxe      - shared memory (SoC info for UFS init)
  #
  # Excluded on purpose (2026-11-11 black-screen set):
  #   DALSYSDxe, VcsDxe, GpiDxe, SPIDxe, I2CDxe, SdccDxe, DisplayDxe,
  #   CPRDxe, PIL/PILProxy, Tz, FeatureEnabler, Rsc.
  INF Binaries/tb710fu/QcomPkg/Drivers/HWIODxe/HWIODxe.inf
  INF Binaries/tb710fu/QcomPkg/Drivers/NpaDxe/NpaDxe.inf
  INF Binaries/tb710fu/QcomPkg/Drivers/ClockDxe/ClockDxe.inf
  INF Binaries/tb710fu/QcomPkg/Drivers/RpmhDxe/RpmhDxe.inf
  INF Binaries/tb710fu/QcomPkg/Drivers/CmdDbDxe/CmdDbDxe.inf
  INF Binaries/tb710fu/QcomPkg/Drivers/PwrUtilsDxe/PwrUtilsDxe.inf
  INF Binaries/tb710fu/QcomPkg/Drivers/SmemDxe/SmemDxe.inf
  INF Binaries/tb710fu/QcomPkg/Drivers/UFSDxe/UFSDxe.inf
!endif
