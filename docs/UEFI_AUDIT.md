# TB710FU UEFI source audit

This audit covers the source-only board overlay in `firmware/overlay/`. It
does not qualify device execution. The clean native macOS kernel and strict
prelink have separate evidence; neither proves the UEFI memory or GOP handoff.

## Immutable inputs

- Firmware compatibility base: [UniXNU/uefi-sheng at
  66421cb7bb5694843aec8de1ca3d1de4c20cca38](https://github.com/UniXNU/uefi-sheng/tree/66421cb7bb5694843aec8de1ca3d1de4c20cca38).
- Board reference: [bmk-keyeast/UniXNU-tb710fu at
  23235249d71604f8a6f743680ef8fd1003fe7189](https://github.com/bmk-keyeast/UniXNU-tb710fu/tree/23235249d71604f8a6f743680ef8fd1003fe7189).
- Same-device Linux hardware reference:
  [SpendyYT/tb710fu-linux at
  4d04410798b538fbe4b50664e3a66d9500a2ed31](https://github.com/SpendyYT/tb710fu-linux/tree/4d04410798b538fbe4b50664e3a66d9500a2ed31).
- Official integration requirements: the clean project's pinned UniXNU
  `docs/zh/porting/uefi-firmware.md`, its component recipes and selected
  DarwinBoot interface. The main lock and workflow own their exact revisions.

Read-only Git inspection found that the previous firmware checkout's tracked
HEAD is exactly the base above, with no tracked framework diff. Its TB710FU
platform and resource TOML are untracked additions. Comparing those additions
against the fixed same-device board reference found only changes in
`Include/RAW.inc`: a panel XML filename substitution and quoted UI strings.
Those RAW files and every vendor branch are excluded from the new overlay.
No old working-tree framework modification is silently imported.

The base records these gitlinks (use the global lock for actual checkout):

| Submodule | Commit |
| --- | --- |
| Common/Mu | `b8d46c71610318e36a661819e1e3c0b8bdda413a` |
| Common/Mu_OEM_Sample | `b70b4b7bb02c43bf9db2172dfb3ed0457e2361d6` |
| Mu_Basecore | `cfb93724a06ff86ff5ba971413368a7809ee42a5` |
| Silicium-ACPI | `2d465ad77c25de908f0ec7bdfcb57f865b61233c` |

`Silicon/Qualcomm` and `Silicon/Silicium` are tracked base trees, not
submodules. No `Binaries` checkout is needed by this board's selected volume.
Do not initialize or copy that depot just to satisfy an old DeviceBuild script.

## File decisions and reviewable differences

All paths in this table are relative to the overlay.

| Path | Source and disposition |
| --- | --- |
| `Resources/Configs/tb710fu.toml` | Retains the reference FD base/size, ARM64 shim header, gzip/append-DTB policy and Android header v4. Adds explicit 4096 page size. Drops stale Android OS/patch metadata; the actual selected image inputs must supply that metadata later. |
| `Platforms/Lenovo/tb710fuPkg/Library/MemoryMapLib/*` | The reference's complete 40-descriptor table and INF are retained; only provenance comments are added to C. No region is silently shortened or removed. |
| `Platforms/Lenovo/tb710fuPkg/Library/ConfigurationMapLib/*` | The reference data and INF are retained with explicit count-semantics comments. No opaque vendor configuration key is mechanically rewritten. |
| `Platforms/Lenovo/tb710fuPkg/tb710fu.dsc` | Keeps the board GUID, Lanai SoC integration, stack, identity, framebuffer geometry and XBL-HOB policy. Closes vendor/display gates with build errors and adds the six-core SMBIOS override after the Lanai include. |
| `Platforms/Lenovo/tb710fuPkg/tb710fu.fdf` | Retains the reference FD/FV layout and LZMA guided section. Includes only the effective open-source branch, SMBIOS, IORT and the base's extra applications/resources. Omits vendor RAW and RamManager paths. |
| `Platforms/Lenovo/tb710fuPkg/Include/{DXE,APRIORI}.inc` | Generated from the fixed reference by evaluating only the two reviewed gates as zero. No binary path or dormant vendor branch remains. Existing open-source dispatch ordering is retained; the source-only TB710FU PMIC key input is dispatched before BDS. |
| `Platforms/Lenovo/tb710fuPkg/Drivers/Tb710fuKeyInputDxe/*` | Source-only SPMI v7 observer reader derived from the pinned Sheng implementation. It discovers the PON and PM8550 GPIO6 APIDs, maps physical keys to SimpleTextIn, and has a host arbiter model test. It does not replace the vendor PMIC stack or claim hardware qualification; see `docs/UEFI_INPUT.md`. |
| `Platforms/Lenovo/tb710fuPkg/Tools/check-board.py` | New board preflight checks range invariants, fixed FD settings, module closure, closed gates, no binary/key inputs and no absolute source paths. It does not mask errors or test hardware. |

The old empty DEC is unused and omitted. DeviceBuild.py is also omitted: its
Stuart setup requires the Binaries depot and is not the official UniXNU direct
DSC build entry used here. No new driver returns a fabricated success, and no
empty binary, security, crypto or hardware provider is created.

The volume retains the pinned base's real EDK II `SecurityStubDxe` module as
part of its existing open-source boot architecture. That upstream module is
not a new placeholder for a missing board driver. It is not evidence of UEFI
Secure Boot enforcement; Android ABL/AVB trust and UEFI image policy are
separate stages. No new signing or trust claim is made.
The earlier unqualified claim that DEVICE_MODEL selects this board is
replaced with the actual host-tool contract. In pinned `build_uefi.py`,
line 44 declares `BuildContext.device_model: int`; line 78 parses `--model`
with `type=int, default=0`; line 231 forwards `DEVICE_MODEL` to the build;
lines 450-457 read `status.model_count` and reject an index greater than or
equal to that count. For this resource TOML's `model_count=1`, the normal
nonnegative host model index is **0**. The device name tb710fu is not a valid
integer argument to that CLI.

The old TB Stuart DeviceBuild.py line 159 only forwards the variable through
`BLD_*_DEVICE_MODEL`. No native reader was found in the TB DSC/Lanai/Silicium
path. Other boards such as giulia do compare numeric model macros, but their
DSCs are not included by this board. Thus the recommendation is numeric 0
for canonical builder compatibility, not a claim that it selects a native
TB identity, changes affinities or starts cores. A direct EDK II call has no
observed TB runtime consumer of this macro at the reviewed base.
UEFI image Secure Boot defaults to 0 and enabling it fails this overlay's
policy until real key/policy inputs are reviewed; that does not change ABL/AVB.

## FD and memory policy

`0xA7100000` is the reviewed FD/shim destination and `0x300000` is its exact
length. The preceding `FD_Reserved` region is `0xA7000000 + 0x100000`. The
stock UEFI image's ELF entry and the appended FD destination are different
concepts; do not replace the destination with the ELF entry. FD block size
is 4096 and the builder must supply the matching FD macros.

The table preserves the same-device reference's 40 entries, including:

- `PIL_Reserved` ends at `0xA2B80000`; retain its source length
  `0x16F80000`, rather than the old experimental larger value.
- `Display_Demura` is `0xA3600000 + 0x2B00000` and
  `Display_Reserved` is `0xD5100000 + 0x2B00000`; neither is conventional RAM.
- `DXE_Heap` ends at `0xD3000000`, avoiding mainline rmtfs at that address.
  This is a deliberate reference policy, not an exact unmodified stock heap.
- `RAM_High` is `0x8C0000000 + 0xC0000000` (3 GiB conventional memory).
  The reference reports a larger contiguous live-FDT DDR run there and uses
  this subset for an aligned 2 GiB DarwinBoot owned window. The clean project
  has not independently qualified that live FDT or allocation.
  The high-DDR C comment now attributes the observation to the ref23235249
  author rather than claiming this project measured it.

The static checker confirms nonzero ranges, arithmetic bounds, pairwise
non-overlap and exclusion of the reviewed ramoops/rmtfs/hwfence/display ranges
from conventional allocations. It cannot prove the actual DDR hardware map.
An 8 GiB SKU label or Linux MemTotal is not a substitute for the selected live
firmware's banks and reserved holes. Do not enable hardware boot until these
inputs and the loader allocation map are reconciled.
The reviewed previous evidence contains no captured `/proc/iomem` System RAM
ranges that independently establish RAM_High. Its reference-detail text and
menu-achieved report trace the high region to the reference implementation.
The old round-2 report proposes reading `/proc/iomem` as a next step; that
proposal is not a captured measurement. No device access is performed here.

The complete retained address/size/type inventory appears below. Its source
fields, HOB options and attributes remain in the C table and are checked
without changing the previous source tree.

## CPU count semantics

The pinned Lanai DSC is the SM8650 SoC layer, not sheng's SM8550 board. Its
`PcdCoreCount=8` comment itself notes that Q variants have six cores. The
open `SmBiosTableDxe/SmBiosTable.c` consumer publishes that PCD as core,
enabled-core, thread and enabled-thread counts. The board now overrides it
to **6**, after including Lanai. This fixes the advertised SKU count; it
does not implement a six-core startup routine.

`ConfigurationMapLib` still retains source values `NumCpus=8`,
`NumActiveCores=8`, `EnableMultiThreading=1`, `EarlyInitCoreCnt=2` and
`EnableMultiCoreFvDecompression=1`. No reader of those count/multithreading
keys was found in the open Lanai implementation. `ShimLoader.c` exposes the
configuration lookup interface as UEFI Config Lib for vendor consumers. The
closed consumers' semantics are unknown, and those vendor DXE are excluded.
Changing 8 to 6 there would not establish correct affinity mapping or SMP.

Lanai `PlatformInitialize()` does not CPU_ON secondary cores. It detaches
the MDP SMMU streams `0x1C00,0x1C01,0x1C04,0x1C06`. This real inherited
hardware operation must be included in handoff review; source-only does not
mean hardware has no side effects. `PcdClusterCount=4` remains inherited
because the open Qualcomm SMBIOS code uses it to select a clock domain; it
must not be rewritten to match an unrelated XNU software-cluster model.

## GOP and display preservation

The overlay retains `3200x2000`, 32-bit geometry from the same-device panel
reference and the `Display_Reserved` name expected by SimpleFb. It does not
load vendor ClockDxe, SPMIDxe, DisplayDxe, CPRDxe or ButtonsDxe. Turning
either vendor gate on fails the build rather than substituting files. The
previous grey/black display followed the vendor dependency chain; the exact
register-level cause was not measured, so clock reprogramming is a suspected
mechanism rather than a fully proved root cause.

The inherited SimpleFb implementation uses `Display_Reserved` as the base,
assumes `PixelsPerScanLine=Width` and packed BGRA for 32-bit pixels, and does
not read the actual ABL stride/format. Its single-mode SetMode preserves
scanout; it is not a panel timing implementation. The framebuffer serial
library in DEBUG builds also relies on board geometry. These assumptions
need separate device evidence before any GOP or readable-screen claim.

Do not infer safe initrd lifetime merely from its address in a boot image.
The selected firmware FDT must provide correct `/chosen/linux,initrd-start`
and `linux,initrd-end`; the complete payload range must remain reserved
through UEFI allocations. The current static table does not dynamically
reserve an arbitrary ABL initrd. Qualified stock/live FDT and actual loader
allocation evidence remain prerequisites.

## DSC/FDF dependency closure

The DSC inherits `LanaiPkg -> QcomPkg -> SiliciumPkg`. The pinned Silicium
DSC supplies DiskIoDxe, PartitionDxe, FAT, English Unicode Collation,
SimpleFb, `HobLib`, `FdtLib`, `MemoryMapHelperLib` and `FrameBufferBltLib`.
The board explicitly adds the four UniXNU/DarwinBoot modules. Its effective
DXE list puts all nine required loader/filesystem/GOP modules in the FV:

1. UniXNU application.
2. UniXNUDxe registrar.
3. UniXNURamDiskDxe Block I/O provider.
4. DarwinBoot.
5. DiskIoDxe.
6. PartitionDxe.
7. EnhancedFatDxe.
8. English Unicode Collation.
9. SimpleFbDxe.

SEC in the pinned base preserves incoming x0 in the entry-argument GUID HOB
used by UniXNURamDiskDxe. This integration is a reason to keep the exact
uefi-sheng base instead of replacing it with unrelated Mu-Silicium sources.
The composed build still needs the pinned official UniXNU package and
selected DarwinBoot checkout in PACKAGES_PATH. Missing real providers must
fail checkout/preflight/build; no placeholders are supplied. The initial
native-only kernel component set does not itself establish that these
firmware inputs have been resolved.

## Shared framework findings kept outside the overlay

No common framework source is copied into the board overlay. The following
findings belong in separately reviewed patches against the pinned base:

| Source | Finding and required treatment |
| --- | --- |
| `SiliciumPkg/Drivers/SimpleFbDxe/SimpleFb.c`, GetFrameBufferInfos | A local descriptor is uninitialized and two region lookups ignore EFI_STATUS. MemoryMapHelperLib leaves output untouched on failure. A generic fix should initialize it, use status-aware fallback and validate geometry/required bytes against region length. |
| The same file, RegisterFrameBuffer | InstallMultipleProtocolInterfaces failure is logged/asserted but the function returns EFI_SUCCESS. Return the actual failure and handle owned allocations; do not hide it in board data. |
| The same file, mode functions | QueryMode/SetMode do not reject unsupported mode numbers. Any future generic cleanup should preserve one-mode scanout semantics and return a real unsupported-mode status. |
| SEC framebuffer paths | SEC also uses the display-region helper. Review its lookup/error policy with the same source contracts before hardware release; do not assume a GOP-only fix covers early serial drawing. |

The known-good board supplies the named display region, so the missing-region
path is not the expected normal input; that does not make the generic error
handling correct. A successful firmware build is not a remedy. These findings
are recorded as runtime qualification limits, not silently fixed by editing
old caches or introducing a private duplicate GOP driver.

## Verification and next gates

Run the board preflight without a device:

```sh
python3 firmware/overlay/Platforms/Lenovo/tb710fuPkg/Tools/check-board.py \
  --uefi-root /path/to/pinned/uefi-sheng
```

It can additionally check `--fd /path/to/SILICIUM_UEFI.fd` for exact length.
CI should preserve the actual EDK II build report, module map, source pins
and FD SHA-256. Validate actual FV module closure with the build report/FV
metadata; searching compressed FD strings is not sufficient proof.

### Local source validation result

The board preflight was run against the read-only base at the full revision
recorded above. It exited 0 with 40 memory regions, nine required FV modules,
zero vendor binary references and `hardware_qualified=false`. Its semantic
memory-table SHA-256 is
`3033991185c76994ca5d4cfe46aaad1bca255bb450c9a7e031a5b5057ea8acbe`.
This covers names, ranges, HOB/type and attributes, not compiled FD bytes.
No firmware build or device test is claimed by the local source check.

The reference comparison uses `git diff --no-index`; exit status 1 means
differences were found. The review removes vendor RAW, dormant vendor
branches, the empty unused DEC and the Stuart DeviceBuild entry. The two
library INF files, 40 memory rows and configuration values remain unchanged.
Board DSC/FDF policy and the preflight are deliberate clean changes.

Remaining device gates: reconcile live DT/DDR/initrd, check ABL geometry and
retained scanout, qualify the firmware log channel, obtain per-core EL1 PAC
and XNU handoff evidence, and test recovery separately if authorized. No
signing, flashing, boot-slot, GPT or filesystem change is included here.

## Complete retained memory table

| Name | Base | Length | HOB | Resource | UEFI type |
| --- | --- | --- | --- | --- | --- |
| `RSRV0` | `0x816e0000` | `0x320000` | `AddMem` | `SYS_MEM` | `Reserv` |
| `XBL_DT` | `0x81a00000` | `0x40000` | `AddMem` | `MEM_RES` | `Reserv` |
| `XBL_Ramdump` | `0x81a40000` | `0x1c0000` | `AddMem` | `MEM_RES` | `Reserv` |
| `AOP` | `0x81c00000` | `0xa0000` | `AddMem` | `MEM_RES` | `Reserv` |
| `UEFI_Log` | `0x81ce4000` | `0x10000` | `AddMem` | `MEM_RES` | `Reserv` |
| `SMEM` | `0x81d00000` | `0x200000` | `AddMem` | `MEM_RES` | `Reserv` |
| `PvmFw` | `0x824a0000` | `0x100000` | `AddMem` | `SYS_MEM` | `Reserv` |
| `PIL_Reserved` | `0x8bc00000` | `0x16f80000` | `AddMem` | `MEM_RES` | `Reserv` |
| `Display_Demura` | `0xa3600000` | `0x2b00000` | `AddMem` | `MEM_RES` | `Reserv` |
| `DBI_Dump` | `0xa6100000` | `0xf00000` | `NoHob` | `MMAP_IO` | `Conv` |
| `FD_Reserved` | `0xa7000000` | `0x100000` | `AddMem` | `SYS_MEM` | `BsData` |
| `UEFI_FD` | `0xa7100000` | `0x300000` | `AddMem` | `SYS_MEM` | `BsData` |
| `UEFI_FD_Reserved` | `0xa7400000` | `0x200000` | `AddMem` | `SYS_MEM` | `BsData` |
| `Info_Blk` | `0xa7601000` | `0x1000` | `AddMem` | `SYS_MEM` | `RtData` |
| `MMU_PageTables` | `0xa7602000` | `0x3000` | `AddMem` | `SYS_MEM` | `BsData` |
| `Log_Buffer` | `0xa7605000` | `0x8000` | `AddMem` | `SYS_MEM` | `RtData` |
| `UEFI_Stack` | `0xa760d000` | `0x40000` | `AddMem` | `SYS_MEM` | `BsData` |
| `SEC_Heap` | `0xa764d000` | `0x8c000` | `AddMem` | `SYS_MEM` | `BsData` |
| `Sched_Heap` | `0xa76d9000` | `0x400000` | `AddMem` | `SYS_MEM` | `BsData` |
| `FV_Region` | `0xa7ad9000` | `0x400000` | `AddMem` | `SYS_MEM` | `BsData` |
| `UEFI_RESV` | `0xa7ed9000` | `0x127000` | `AddMem` | `SYS_MEM` | `BsData` |
| `Kernel` | `0xa8000000` | `0x10000000` | `AddMem` | `SYS_MEM` | `Reserv` |
| `DXE_Heap` | `0xb8000000` | `0x1b000000` | `AddMem` | `SYS_MEM` | `Conv` |
| `Display_Reserved` | `0xd5100000` | `0x2b00000` | `AddMem` | `MEM_RES` | `Reserv` |
| `TZApps_Reserved` | `0xd8800000` | `0x1b000000` | `AddMem` | `MEM_RES` | `Reserv` |
| `RAM_High` | `0x8c0000000` | `0xc0000000` | `AddMem` | `SYS_MEM` | `Conv` |
| `IMEM_Base` | `0x14680000` | `0x2a000` | `NoHob` | `MMAP_IO` | `Conv` |
| `IMEM_Cookie_Base` | `0x146aa000` | `0x16000` | `AddDev` | `MMAP_IO` | `Conv` |
| `IPC_ROUTER_TOP` | `0x400000` | `0x100000` | `AddDev` | `MMAP_IO` | `MmIO` |
| `SECURITY_CONTROL` | `0x780000` | `0x7000` | `AddDev` | `MMAP_IO` | `MmIO` |
| `QUP` | `0x800000` | `0x300000` | `AddDev` | `MMAP_IO` | `MmIO` |
| `PRNG_CFG_PRNG` | `0x10c0000` | `0xc000` | `AddDev` | `MMAP_IO` | `MmIO` |
| `CRYPTO0_CRYPTO` | `0x1dc0000` | `0x40000` | `AddDev` | `MMAP_IO` | `MmIO` |
| `CLK_TCSR_TCSR_REGS` | `0x1f00000` | `0x100000` | `AddDev` | `MMAP_IO` | `MmIO` |
| `PERIPH_SS` | `0x8800000` | `0x100000` | `AddDev` | `MMAP_IO` | `MmIO` |
| `USB` | `0xa600000` | `0x200000` | `AddDev` | `MMAP_IO` | `MmIO` |
| `AOSS` | `0xb000000` | `0x4000000` | `AddDev` | `MMAP_IO` | `MmIO` |
| `TLMM` | `0xf000000` | `0x1000000` | `AddDev` | `MMAP_IO` | `MmIO` |
| `SMMU` | `0x15000000` | `0x200000` | `AddDev` | `MMAP_IO` | `MmIO` |
| `APSS_HM` | `0x17000000` | `0x2000000` | `AddDev` | `MMAP_IO` | `MmIO` |
