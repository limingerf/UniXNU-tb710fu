# TB710FU log channels · 2026-11-17

## Current status

There is no verified live serial port for XNU on this device. The Windows host
currently exposes no `Win32_SerialPort` entry or Qualcomm EUD device. This only
describes the attached-device state at the time of the check; it does not prove
that EUD hardware is absent.

`serial=3` selects an XNU UART console. It cannot create a UART or route it over
USB-C. No external UART connector is available on the tablet. Qualcomm's
official EUD host library does include a COM peripheral API, but that is an EUD
USB protocol handled by a host client; it need not appear as a normal Windows
`COMx` port. The current XNU tree has no TB710FU EUD COM output driver.

The live device tree has a Qualcomm EUD node, and an older vendor
`UsbConfigDxe` binary contains EUD-related strings. These are implementation
clues, not proof that the host enumerates a COM port. The clean firmware
configuration keeps `TB710FU_VENDOR_HW=0`; its DXE list includes USB host
keyboard and mass-storage drivers, not a USB device-function or EUD driver.
The inherited `SimpleTextInOutSerial` module also does not establish a physical
UART.

## What each path can provide

| Path | Current state | XNU logs? |
| --- | --- | --- |
| External UART | No exposed connector or verified board UART | No |
| Qualcomm EUD COM over USB | Device-tree and old firmware clues exist; no EUD device is currently enumerated on the host, and the current firmware excludes the device-mode stack | Potentially, after enabling the channel and connecting XNU output; host access uses an EUD client, not necessarily `COMx` |
| DWC3 CDC-ACM | No TB710FU implementation yet; a community SM8150/XNU 17.7.11 port reports Windows `COM3` and live XNU output | Yes, after adapting and validating the controller, PHY, clocks, SMMU and XNU interfaces |
| `serial=3` | Boot argument only | Only if a real UART is configured and reachable |
| USB CDC / CDP | No TB710FU XNU driver or validated device protocol | No; giulia's protocol is board-specific |
| Screen QR checkpoints | Used for selected diagnostic reports | Checkpoint fields only, not a continuous console |
| Ramoops | Guard11 reported return from `tb710fu_ramoops_init` at stage 56; contents and cross-reset retention are not yet verified | Potentially, after reboot |

The guard11 ramoops writer uses physical range
`[0x93ff00000, 0x93ff40000)`. The existing UEFI QR viewer still reads the old
`0xac340000` address, so that viewer cannot export guard11's zone. A TWRP raw
memory read is the immediate verification path, subject to `/dev/mem` access and
memory retention. Stage 56 does not prove the zone header or log bytes are
valid, and the device partition hash has not been read back.

## Driver references

The closest working log-port reference is
[Lux12138/nabu-usb-cdc](https://github.com/Lux12138/nabu-usb-cdc). Its README
reports DWC3 CDC-ACM enumerating as Windows `COM3` and carrying live XNU output
on Xiaomi Pad 5 / SM8150 with XNU 17.7.11. It is a design and capture-tool
reference, not a drop-in driver: TB710FU is SM8650 with XNU 12377, so the
controller power, clocks, PHY, SMMU stream and XNU interfaces need review.
The repository has mixed licensing; audit the SPDX/license of each reused file
before copying code.

[UniXNU/QcomDwc3](https://github.com/UniXNU/QcomDwc3) is the closest official
driver for the Apple USB host/device stacks, but its documented hardware is
SM8550 and it is not a CDC console. Its board-specific PHY, power, UCSI and SMMU
work cannot be assumed to match TB710FU. The official
[VirtioMmioConsole](https://github.com/UniXNU/VirtioMmioConsole) is for QEMU and
does not match this physical device. The UniXNU driver catalog has no EUD XNU
console driver. Qualcomm's separate [quic/eud](https://github.com/quic/eud)
library does expose an EUD COM API to a host client, but EUD enumeration and an
XNU-side sender remain unverified here.

## Work required for a real port

1. Use `nabu-usb-cdc` for the CDC endpoint and host capture design, then map its
   assumptions against TB710FU's SM8650 DWC3/PHY/SMMU facts. Use `QcomDwc3` only
   for the newer Apple USB-stack integration points.
2. Build a minimal USB 2 CDC-ACM path with a test string and verify that the
   Windows host enumerates a COM port and captures bytes. Do this before
   enabling USB3, host mode, UCSI or other unrelated functions.
3. Connect early XNU console output and panic output to the validated CDC path.
   Keep EUD as a separate candidate until its device-mode enumeration and host
   COM API are proven on this board.

Until those checks pass, there is no direct XNU log channel to open. For the
current kernel, first verify the ramoops zone through TWRP. If it contains the
expected `DBGC` header and XNU banner, the UEFI viewer can later be updated,
with a separate check that firmware allocations do not overwrite the region.

Evidence and related details: [current status](../CURRENT_STATUS.md),
[UEFI audit](UEFI_AUDIT.md), [UEFI log viewer](UEFI_LOG_VIEWER.md), and
[official debugging guide](https://unixnu.com/zh/guide/debugging).
