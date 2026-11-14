# TB710FU hardware evidence and reuse boundaries

This clean project targets Lenovo TB710FU / Xiaoxin Pad Pro GT, SM8650Q-AB,
with the official `macos-27.3-26C5035r` profile. The immediate task is a
native-only kernel build on a supported macOS runner. The selected components
are `ArmDTPlatform`, `ArmCpu` and `XnuPrelink`. No clean-project hardware boot,
prelink, root mount, macOS userspace or peripheral result is qualified.

`board.toml` defines configuration. `porting.toml` is the authoritative porting
record. `hardware.toml` is an evidence inventory and is not consumed by UniXNU.

## Fixed sources

Use the complete revisions in `upstream.lock.json`; do not combine current
branches from unrelated ports. At creation this inventory records:

| Input | Revision or identity | Role |
| --- | --- | --- |
| UniXNU/UniXNU | `0c1bac3d51a3009dca1708b15a5198f4b43d057a` | Official schema, recipes, documentation and OS profile |
| XNU | `ac9718fb1af618d5ce8678d0dc6e8a58f252216f` | Source selected by the target profile |
| OS profile | `macos-27.3-26C5035r` | macOS 27.3 build 26C5035r |
| Expected Apple kernelcache SHA-256 | `d1003b5143ffd88439b4d45b6017f587171cfe6855660bed05370a2ea5db9fe9` | Later prelink input, not a native-kernel hash |
| SpendyYT/tb710fu-linux | `4d04410798b538fbe4b50664e3a66d9500a2ed31` | Same-device hardware source reference |

Relevant fixed hardware sources:

- [Board DTS](https://github.com/SpendyYT/tb710fu-linux/blob/4d04410798b538fbe4b50664e3a66d9500a2ed31/arch/arm64/boot/dts/qcom/sm8650-lenovo-tb710fu.dts).
- [SM8650 DTSI](https://github.com/SpendyYT/tb710fu-linux/blob/4d04410798b538fbe4b50664e3a66d9500a2ed31/arch/arm64/boot/dts/qcom/sm8650.dtsi).
- [Selected Linux configuration](https://github.com/SpendyYT/tb710fu-linux/blob/4d04410798b538fbe4b50664e3a66d9500a2ed31/arch/arm64/configs/sm8650_defconfig).
- [NT36532 panel implementation](https://github.com/SpendyYT/tb710fu-linux/blob/4d04410798b538fbe4b50664e3a66d9500a2ed31/drivers/gpu/drm/panel/panel-nt36532-wqhd-dual-dsi-dsc.c).

Linux source and its author's working-status reports do not qualify an XNU
driver. Old iPadOS or Darwin 22 object layouts and vtable slots are not macOS 27
ABI evidence.

## Prior local evidence

The previous workspace remains unchanged at `D:/unixnu-tb710fu-plan`. Its
initial skeletons and later experimental records have different dates and
qualification levels. Use an original input or a specific recorded observation
rather than treating every statement there as current.

| Previous input | What can be reused | Remaining qualification |
| --- | --- | --- |
| `device-facts.md` and `evidence/raw-*` | Historical board identity, six logical CPUs, 4 KiB pages and UFS block-size observations | Reconcile selected stock DT and firmware identity; no clean-project execution is proved |
| `backups/boot-chain/vendor_boot_a.img` and `vendor_boot_b.img` | Locally held stock-format images that may supply embedded DTB inputs | Parse offline and establish which image and DT entry match the selected firmware |
| `backups/boot-chain/dtbo_a.img` and `dtbo_b.img` | Locally held original overlay containers | Parse and match the active DTBO entry; do not assume the two slots are identical |
| `backups/boot-chain/uefi_a.img` and `uefi_b.img` | Local binary source candidates for required firmware extraction | Preserve provenance and verify hashes; do not commit full images or extracted proprietary blobs |
| `evidence/boot-uefi/` | Historical UEFI/menu/display observations and build notes | Match the precise source, image and device firmware before reuse |

These paths identify existing local evidence, not files distributed by this
repository. No personal serial, raw GPT table or partition geometry is copied
here. The reviewed old evidence establishes that an FDT file existed on the
device, but `evidence/raw-dtb_fdt.txt` is a text listing, not that binary. No
`live-fdt.dtb` was found in the previously inspected device/reference checkout
or its firmware cache. Do not claim a qualified live firmware DT is already
present. Offline extraction from the held images is the next source option;
device access is a separate task.

Previous Android properties identify SM8650Q/pineapple and report six logical
CPUs. Some product and SoC-looking properties originate from the installed
ColorOS port; they must not override the board identity. Previous observations
of a locked bootloader and a running test-key AVB chain are not authorization
to sign, flash, switch slots or modify storage in this task.

## Hardware inventory

| Area | Same-device source facts or prior observations | Clean-project limit |
| --- | --- | --- |
| CPU | The pinned DTSI disables physical CPU affinities `0x100` and `0x200`; enabled references are `0x000,0x300,0x400,0x500,0x600,0x700` | Confirm from the selected stock DT; Linux logical CPU indices are not MPIDRs |
| CPU features | Prior Linux EL0 feature strings include `paca` and `pacg` | Per-core EL1 PAC register access and instruction behavior remain unqualified |
| Interrupts | GICD `0x17100000/0x10000`, GICR `0x17180000/0x200000`, reference stride `0x40000`; reference PSCI method `smc` | Confirm GICR affinity, PSCI ownership, timer frequency and IPI behavior on this firmware |
| Memory | Mainline reserved ranges and the previous UEFI memory table are available as sources | Actual DDR banks and loader-owned ranges require qualified stock/live firmware input |
| UFS | Reference host `0x01d84000`, PHY `0x01d80000`; prior Android observations report 4096-byte logical/physical blocks | No selected native UFS driver, read-only known-block result, root mount or persistence |
| Display | NT36532, 3200 by 2000, dual DSI with DSC; SY7758 backlight and AW37504 panel supply | No ABL stride/format qualification, native framebuffer or userspace presentation |
| Touch | `novatek,NVT-ts-spi`, SPI4, source IRQ GPIO162; Linux selects `TOUCHSCREEN_NT36532_SPI` | Review `drivers/input/touchscreen/nt36532/`, not the distinct `novatek-nvt-ts.c`; no native HID result |
| GPU | Adreno 750 family and its board firmware names are source references | No native submission, fence, Metal or presentation result |
| Wireless | WCN7850; Wi-Fi on PCIe and Bluetooth on UART14 in the board DTS | No native transport, firmware boot, network traffic or Bluetooth result |
| Audio | WCD9395 and four AW88461 amplifiers with ADSP routing in the board DTS | No native DSP boot, audio ABI or playback/capture result |
| Power | Board PMIC/regulator and GPIO descriptions are available in the reference | No native clock, CPU power, suspend/resume or battery qualification |
| Cellular | No clean-project modem/SIM inventory has been established | Do not infer a usable modem from generic shared-SoC reserved nodes |

Clock, regulator and reset sequences are review inputs. Linux DRM, ath12k and
ASoC implementations cannot be linked into XNU as native drivers without the
required interface and ABI work. Firmware license and redistribution rights
must be checked before publishing any extracted material.

## Previous UEFI integration candidates

The prior actual UEFI checkout is `/opt/src/uefi-sheng` in WSL Debian. Its
`Platforms/Lenovo/tb710fuPkg/` board overlay and
`Resources/Configs/tb710fu.toml` are review candidates. They are not imported
by the clean native-only configuration. The previous working directory also
contains `Binaries/tb710fu/RawFiles/uefiplat.cfg`; tie any extracted input to
its original local image and hash before using it.

Historical settings requiring a clean import and a new hardware test:

- FD base `0xA7100000`, size `0x300000`, Android boot header version 4.
- `USE_CUSTOM_DISPLAY_DRIVER=0` and `TB710FU_VENDOR_HW=0`, preserving the ABL
  scanout instead of loading the entire vendor display/clock dependency chain.
- A 40-entry memory table with FD reservations, display demura and high DDR.
  These are board-specific inputs, not universal SM8650 memory constants.
- The previous ConfigurationMapLib retains `NumCpus=8` and
  `NumActiveCores=8`, as does the extracted configuration file. Review each
  consumer's semantics; those fields do not establish eight usable cores on
  the cut six-core TB710FU.

The old boot-chain plan put a payload ramdisk in `vendor_boot` and used a
ramdisk-free GKI `boot` image. It remains outside this native-only task; a
format-valid image or successful AVB check does not prove ABL will load it.

## Excluded previous implementations

- The early `t2/` tree is a documented offline skeleton. Its placeholder
  memory size, UUIDs, missing firmware integration and missing DTSI are not
  clean-project build inputs.
- The subsequent handwritten minimal board DTS has a zero-sized memory
  declaration and an overflowing/malformed ranges cell sequence. Rebuild a
  tree from qualified inputs; do not copy that DTS.
- The draft `research/sm8650-common/` contains a required-commit placeholder
  and eight-core assumptions. It is not a selected SoC implementation.
- The local old SoC DTSI embeds TB-specific six-core clipping; move such
  policy into a reviewed board tree rather than advertising it as common
  SM8650 topology.
- The old same-device reference's `package/ShengPlatformPower.cpp` is an
  iPadOS ABI qualification fixture. It uses fixed class sizes/slots and
  panics for unsupported hardware operations. It is not a functioning power
  driver and is not imported into macOS 27.
- No empty hardware, security or crypto provider is added to make a link
  succeed. Optional providers must be disabled coherently, following the
  pinned official profile and recipes.

## Evidence gates

1. Validate `porting.toml` with the pinned official checker. This checks the
   record schema and coverage, not the hardware.
2. Resolve the clean native-only configuration and complete one bounded
   macOS runner build. Preserve the actual native artifact hash and log.
3. Qualify prelink, Apple inputs, trust caches and Data seed separately.
4. Review firmware, selected DT, memory and a recoverable logging path before
   proposing any device test.
5. Report kernel entry, root mount, userspace exec, desktop and each peripheral
   as separate milestones, each with matching source/image identity.

No clean-project area is marked `device-verified`. Hardware tests, signing,
flashing, slot changes, storage layout changes and Android filesystem changes
are outside the clean cloud-build authorization.
