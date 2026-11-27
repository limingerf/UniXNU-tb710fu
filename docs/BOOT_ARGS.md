# TB710FU macOS 27.3 Boot Arguments Reference

This document lists the kernel boot arguments used by the Lenovo TB710FU (`SM8650Q-AB`, Adreno 750, 3200×2000 panel, 8 GiB LPDDR5X) macOS 27.3 (`26C5035r`, XNU `12377.121.6`) port.

## 1. Current Verified Graphical Desktop Baseline (`pl93` / `pl94`, 5 GiB RAM)

```text
-v serial=3 debug=0x14e kt_fixed_zones=120 -no_slto_panic use_contiguous_hint=0
native-source-storage=1 native-ufs-root=1 tb710fu-cdc=1 kc-segment-release=0
tb710fu-root-partition=unixnu smp=1 armcpu-spi-ipi=1 armcpu-self-ipi=1
exec-log=1 sheng-data-persist=2 data-persist=2 apfs-boot-shim=1
disable-watchdogd=1 aks=1 aks-sls=3 no-lc=1 timebase-hz=24000000
kern.maxvnodes=65536 ml-timeout-sched-preemption=0
sched_preemption_disable_debug_mode=0
tb710fu-gop=1 offline-legacy-gop=1 offline-legacy-validate=1
offline-legacy-transfer=1 offline-legacy-pm=1 offline-legacy-phase=4
offline-legacy-register=1 offline-legacy-cpu-present=1
vsync=120 vsync-fixed=1
adsp=2 battery=1 usb-sdp-ma=2000
gpu-metal=1 gpu-power=9 gpu-addon=1 fb-half=2
text-fixup0=dyld_shared_cache_arm64e.03,5de1134,08060037,1f2003d5
tb710fu-touch=3 tb710fu-keys=1
```

> **Required ADT companion settings** (also recorded in `board.toml`):
> - `/chosen/darwinboot-memory-size = 0x140000000` (5 GiB usable RAM with the 5 GiB `RAM_High` UEFI `boot_b`, `out/uefi-5g-45995333298/boot.img`)
> - `/product/artwork-display-gamut = "sRGB"` and `/product/ideal-screen-colorspace = "sRGB"` (prevents `WindowServer` from selecting 64-bit wide-gamut `'&w4a'` surfaces)

---

## 2. Argument Reference by Subsystem

### Core Kernel, Storage, Memory & Debug
| Argument | Value | Purpose |
|---|---|---|
| `-v serial=3 debug=0x14e` | — | Verbose kernel log and serial output routed through `tb710fu-cdc=1` (`COM5`). |
| `tb710fu-cdc=1` | `1` | Enables the DWC3 USB CDC ACM serial console (`COM5`). Interactive commands: `disp`, `reboot`, `edl`. Always open `COM5` with `DtrEnable=true` and `RtsEnable=true`. |
| `native-source-storage=1 native-ufs-root=1` | `1` | Enables native Qcom UFS storage and mounts the APFS root container from `tb710fu-root-partition=unixnu`. |
| `sheng-data-persist=2 data-persist=2` | `2` | Mounts the persistent APFS Data volume on UFS without wiping it. Pass `data-reset=1` **only** when intentionally re-initializing Data from `data.dbr`. |
| `apfs-boot-shim=1` | `1` | Stages `/usr/libexec/apfs_boot_util` shim from the addon. |
| `disable-watchdogd=1` | `1` | Writes `launchd` override and arms the kernel guard suppressing `watchdogd` critical-exit panics. |
| `aks=1 aks-sls=3` | `1 / 3` | Enables `AppleKeyStore` software state provider. |
| `no-lc=1` | `1` | Skips launch constraints enforcement for unsigned/port daemons. |
| `smp=1 armcpu-spi-ipi=1 armcpu-self-ipi=1` | `1` | Enables 8-core SMP bring-up and SPI/self IPI delivery. |
| `timebase-hz=24000000` | `24000000` | Sets ARM generic timer frequency (24 MHz on SM8650). |
| `kern.maxvnodes=65536` | `65536` | Prevents `vnode: table is full` jetsam kills under full macOS desktop. |
| `ml-timeout-sched-preemption=0 sched_preemption_disable_debug_mode=0` | `0` | Disables 5 ms preemption-disable debug panic on DEVELOPMENT kernel. |
| `kt_fixed_zones=120 -no_slto_panic use_contiguous_hint=0 kc-segment-release=0` | — | XNU memory/prelink stability flags required on arm64e macOS 27.3. |
| `exec-log=1` | `1` | Logs process `exec`/`exit` lines to serial console for diagnostics. |

### Display (`GopFramebuffer` / `NativeLegacyDisplay`)
| Argument | Value | Purpose |
|---|---|---|
| `tb710fu-gop=1` | `1` | Starts TB710FU `IOMobileFramebuffer` GOP display adapter (also disables `vc-text=1` console overwrite). |
| `offline-legacy-gop=1 offline-legacy-validate=1 offline-legacy-transfer=1 offline-legacy-pm=1 offline-legacy-phase=4 offline-legacy-register=1 offline-legacy-cpu-present=1` | `1 / 4` | Enables the full `NativeLegacyDisplay` lifecycle (GOP mapping, swap validation, CPU scanout copy, power management, registration). |
| `vsync=120 vsync-fixed=1` | `120 / 1` | Publishes 120 Hz display timing and fixed refresh cadence. |
| `fb-half=2` | `2` | Encoded half-float LUT mode (fallback if any half-float `'RGhA'` frame is presented). |
| `text-fixup0=dyld_shared_cache_arm64e.03,5de1134,08060037,1f2003d5` | — | Signed-text fixup in `QuartzCore` `IOMFBDisplay::framebuffer_pixel_format` (`26C5035r`): NOPs the `vmm-present` -> `'RGhA'` branch so `WindowServer` presents 8-bit BGRA (`'&BGA'`, 25.6 MB instead of 51.2 MB). |

### GPU (`Tb710fuGpu` / Adreno 750 + `NativeAdrenoMetal`)
| Argument | Value | Purpose |
|---|---|---|
| `gpu-power=9` | `9` | Full Adreno 750 bring-up ladder: stages 1–9 (GCC/GPUCC clocks, CX/GX GDSCR, SMMU CB0 4 KiB tables, GMU firmware + HFI, ZAP secure exit, CP ring, NativeAdrenoABI v2 live). |
| `gpu-metal=1` | `1` | Publishes `AGXAccelerator` (`MetalPluginName=NativeAdrenoMetal`, `MetalPluginClassName=NativeAdrenoMetalDevice`) and serves `AGXDeviceUserClient`. |
| `gpu-addon=1` | `1` | Stages `/Library/GPUBundles/NativeAdrenoMetal.bundle` (13 files) and shader cache from the combined `GpuAddon` payload. |

### ADSP & Battery (`Tb710fuAdsp` + `Tb710fuPower`)
| Argument | Value | Purpose |
|---|---|---|
| `adsp=2` | `2` | Boots SM8650 ADSP (`adsp.mbn` + `adsp_dtb.mbn` from addon) and brings up SMEM/SMP2P/IPCC/PMIC GLINK (`adsp=1` is read-only probe). |
| `battery=1` | `1` | Publishes `IOPMPowerSource` from `qcom_battmgr` over PMIC GLINK. |
| `usb-sdp-ma=2000` | `2000` | Requests 2000 mA USB input current limit via battmgr. |

### Prelinked Peripheral Drivers (Opt-In)
| Subsystem | Boot Argument(s) | Status |
|---|---|---|
| **Touchscreen** (`Tb710fuInput`) | `tb710fu-touch=1..3` (`1`=snapshot, `2`=engine, `3`=full active) | Prelinked; enabled in `pl94-5g-srgb-input`. |
| **Side Keys** (`Tb710fuInput`) | `tb710fu-keys=1` (`0`=off, `1`=on) | Prelinked; enabled in `pl94-5g-srgb-input`. |
| **Backlight / Thermal / Platform** (`Tb710fuPlatformServices`) | `backlight=1` / `thermal=1` / `platform-svc=1` (see `package/Tb710fuPlatformServices/Tb710fuPlatformStage.h`) | Prelinked; default off, ready for staged hardware verification. |
| **Audio** (`Tb710fuAudio`) | `audio=1..12` (plus optional `audio-rate`, `audio-tdm-*`, `audio-gpr-*`) | Prelinked; default off, ready for staged hardware verification. |
| **Wi-Fi 7** (`Tb710fuWifi`) | `wifi=1..5` (optional `wifi-smmu-bank=N`) | Prelinked; default off, ready for staged hardware verification. |
| **Bluetooth** (`Tb710fuBluetooth`) | `bt=1..3` (optional `tb710fu-bt-log`, `tb710fu-bt-speed`, `tb710fu-bt-fw`, `tb710fu-bt-bdaddr`) | Prelinked; default off, ready for staged hardware verification. |
