# SPDX-License-Identifier: BSD-2-Clause
# Source-only effective gate=0 module list from reference 23235249d71604f8a6f743680ef8fd1003fe7189.
# Vendor hardware/display branches removed after audit; the board input driver is source-only.
APRIORI DXE {
  INF MdeModulePkg/Core/Dxe/DxeMain.inf
  INF MdeModulePkg/Universal/PCD/Dxe/Pcd.inf
  INF QcomPkg/Drivers/IortDtDxe/IortDtDxe.inf


  INF MdeModulePkg/Universal/ReportStatusCodeRouter/RuntimeDxe/ReportStatusCodeRouterRuntimeDxe.inf
  INF MdeModulePkg/Universal/StatusCodeHandler/RuntimeDxe/StatusCodeHandlerRuntimeDxe.inf
  INF MdeModulePkg/Core/RuntimeDxe/RuntimeDxe.inf
  INF ArmPkg/Drivers/CpuDxe/CpuDxe.inf
  INF ArmPkg/Drivers/ArmGicDxe/ArmGicDxe.inf
  INF EmbeddedPkg/MetronomeDxe/MetronomeDxe.inf
  INF ArmPkg/Drivers/TimerDxe/TimerDxe.inf


  INF MdeModulePkg/Universal/ResetSystemRuntimeDxe/ResetSystemRuntimeDxe.inf


  INF MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf
  INF MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf
  INF MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf


  INF FatPkg/EnhancedFatDxe/Fat.inf


  INF MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf


  INF MdeModulePkg/Universal/WatchdogTimerDxe/WatchdogTimer.inf
  INF MdeModulePkg/Universal/SecurityStubDxe/SecurityStubDxe.inf
  INF EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.inf
  INF EmbeddedPkg/RealTimeClockRuntimeDxe/RealTimeClockRuntimeDxe.inf
  INF MdeModulePkg/Universal/PrintDxe/PrintDxe.inf
  INF MdeModulePkg/Universal/DevicePathDxe/DevicePathDxe.inf
  INF MdeModulePkg/Universal/CapsuleRuntimeDxe/CapsuleRuntimeDxe.inf
  INF MdeModulePkg/Universal/HiiDatabaseDxe/HiiDatabaseDxe.inf
  # Timer and variable services are ready; register the physical-key ConIn first.
  INF tb710fuPkg/Drivers/Tb710fuKeyInputDxe/Tb710fuKeyInputDxe.inf
  INF tb710fuPkg/Drivers/Tb710fuRamoopsDxe/Tb710fuRamoopsDxe.inf
  INF MdeModulePkg/Universal/BdsDxe/BdsDxe.inf


  INF MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf
  INF MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf


  INF SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf


  INF EmbeddedPkg/SimpleTextInOutSerial/SimpleTextInOutSerial.inf
  INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf
  INF MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf
  INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf

!if $(TB710FU_ENABLE_UFS) == 1
  # UFS publication chain. DEPEX is TRUE everywhere, so this order is the
  # load order: IO accessors, clock services, then the host controller.
  # UFSDxe publishes the UFS BlockIo handles that PartitionDxe and FatDxe
  # above can then bind to when it arrives.
  INF Binaries/tb710fu/QcomPkg/Drivers/SmemDxe/SmemDxe.inf
  INF Binaries/tb710fu/QcomPkg/Drivers/HWIODxe/HWIODxe.inf
  INF Binaries/tb710fu/QcomPkg/Drivers/CmdDbDxe/CmdDbDxe.inf
  INF Binaries/tb710fu/QcomPkg/Drivers/PwrUtilsDxe/PwrUtilsDxe.inf
  INF Binaries/tb710fu/QcomPkg/Drivers/NpaDxe/NpaDxe.inf
  INF Binaries/tb710fu/QcomPkg/Drivers/RpmhDxe/RpmhDxe.inf
  INF Binaries/tb710fu/QcomPkg/Drivers/ClockDxe/ClockDxe.inf
  INF Binaries/tb710fu/QcomPkg/Drivers/UFSDxe/UFSDxe.inf
!endif

}
