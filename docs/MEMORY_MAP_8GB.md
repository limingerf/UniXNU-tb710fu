# TB710FU 8 GiB memory map and the macOS 27.3 RAM ceiling

Board: Lenovo TB710FU, Qualcomm SM8650Q-AB, 8 GiB LPDDR5X.
Subject: the `darwinboot-memory-size` window, ADT `dram-base` / `dram-size`, and XNU `arm_vm_init`.
Branch: `doubao/ram-max-memorymap` · evidence date 2026-11-27.

## Answer first

| Question | Answer |
| --- | --- |
| Physical RAM | 8 GiB = **2 GiB bank 0** (`0x80000000`) + **6 GiB bank 1** (`0x880000000`); `4–34 GiB` is an MMIO hole, not DRAM |
| Largest contiguous System RAM | **5 GiB**, `0x8C0000000..0x9FFFFFFFF` |
| Chosen macOS window | **5 GiB** = `0x140000000`; `dram-base = 0x8C0000000`, `dram-size = 0x140000000` |
| Managed by XNU | 5 GiB minus a 16 KiB structural tail (`0xA0000000..0xA0004000`) |
| Why the old "5 GiB" gave only 3 GiB | UEFI `RAM_High` described only 3 GiB; XNU excluded `0x980000000..0xA0004000` (2 GiB + 16 KiB). This change expands the row to 5 GiB. |
| Next physical step | 6 GiB span → 5.90 GiB **if** the 922 MiB mid runs are declared conventional; 6.3 GiB adds the low bank through the sheng `outside` recipe. Documented, not enabled. |
| 6.5 / 7 / 8 GiB | Impossible: bank 1 ends at `0xA0000000`; a 6.5 GiB span already leaves the bank. |

The **maximum provable window with the firmware map as it exists today is 5 GiB**. The map proof is complete; the 5 GiB `RAM_High` expansion still needs one device boot (the 5 GiB ADT itself already booted 4/4, but with the old 3 GiB row).

## 1. Provenance

| Source | Size | SHA-256 (first 16) | Role |
| --- | --- | --- | --- |
| `.local/adt/live.dts` | 1,549,442 B | `7fabb6dc19c8d6d8` | live TB710FU FDT: DDR banks, `memory`, `/reserved-memory` |
| `.local/twrp-fdt/twrp.dts` | 1,549,444 B | `399e9e656be6ed45` | TWRP FDT: independent copy of the same map |
| `firmware/.../MemoryMapLib.c` | 5,844 B | `de9e5786d392b2a6` | 40-row UEFI `gMemoryDescriptor` table |
| `board.toml` | 5,596 B | `585bc5ed54ca5c59` | `[payload.adt]` request |
| `out/pl16-45571102967-pl46-desktop-unsigned/kernel.macho.adt` | 16,868 B | `ab6ea6a0d40a1408` | the proven 5 GiB sidecar used by the 4/4 boots |

`live.dts` and `twrp.dts` agree on all 2 DDR banks, all 35 fixed reserved-memory regions and all 12 merged System RAM runs. `sm8650.dtsi` carries only a size-0 placeholder (`memory@a0000000 { reg = <0 0xa0000000 0 0>; }`); the bootloader fills the real map, so the captured FDTs are the authority.

## 2. Physical DRAM banks

From the `ddr-regions` node of both FDTs:

| Bank | Start | End (inclusive) | Size | GiB |
| --- | --- | --- | --- | --- |
| 0 | `0x00080000000` | `0x000ffffffff` | 2048.0 MiB | 2.000 |
| 1 | `0x00880000000` | `0x009ffffffff` | 6144.0 MiB | 6.000 |
| **Total** | | | **8192.0 MiB** | **8.000** |

The `0x100000000..0x87FFFFFFF` range (4–34 GiB) is the SoC MMIO/PCIe aperture, not LPDDR. No `/memory` entry and no `/reserved-memory` entry lies in it.

## 3. System RAM runs

The `memory` node has 16 `reg` pairs (one zero-length placeholder at `0xD8800000`); adjacent pairs merge to 12 runs:

| Start | End | MiB |
| --- | --- | --- |
| `0x0816E0000` | `0x0819FFFFF` | 3.1 |
| `0x081CF5000` | `0x081CFFFFF` | 0.0 |
| `0x081F20000` | `0x08249FFFF` | 5.5 |
| `0x082800000` | `0x0847FFFFF` | 32.0 |
| `0x084A00000` | `0x09AFFFFFF` | 358.0 |
| `0x0A1F80000` | `0x0D4E22FFF` | 814.6 |
| `0x0D5100000` | `0x0D7FFFFFF` | 47.0 |
| `0x0E1BB0000` | `0x0FF7FFFFF` | 476.3 |
| `0x0FFE00000` | `0x0FFFFFFFF` | 2.0 |
| `0x880000000` | `0x8AFAFFFFF` | 763.0 |
| `0x8B0000000` | `0x8B9EFFFFF` | 159.0 |
| `0x8C0000000` | `0x9FFFFFFFF` | **5120.0** |

Bank 1 is therefore three runs with two holes:

| Piece | Start | End | Size |
| --- | --- | --- | --- |
| run A | `0x880000000` | `0x8AFAFFFFF` | 763 MiB |
| hole 1 | `0x8AFB0000` | `0x8AFFFFFFF` | 5 MiB |
| run B | `0x8B0000000` | `0x8B9EFFFFF` | 159 MiB |
| hole 2 | `0x8B9F0000` | `0x8BFFFFFFF` | 97 MiB |
| run C | `0x8C0000000` | `0x9FFFFFFFF` | 5120 MiB |

Run C is the largest contiguous run in the whole 8 GiB map; the low bank's largest free run is only ~462 MiB (`0xB8000000..0xD4E22FFF` after firmware reservations). The two high-bank holes are *not* named by any `/reserved-memory` child; they are the range the bootloader removed from the HLOS memory node. That is why they cannot be claimed without exclusion.

## 4. Complete `/reserved-memory` table

### 4.1 Fixed regions (35)

`size = end - start + 1`; entries below 0.05 MiB print as `0.0`.

| Node | Start | End | MiB | Owner / class | Flags |
| --- | --- | --- | --- | --- | --- |
| `gunyah_hyp_region@80000000` | `0x080000000` | `0x080DFFFFF` | 14.0 | HYP/TZ | no-map |
| `cpusys_vm_region@80e00000` | `0x080E00000` | `0x0811FFFFF` | 4.0 | HYP/TZ | no-map |
| `xbl_aop_merged_region@81a00000` | `0x081A00000` | `0x081C5FFFF` | 2.4 | firmware | no-map |
| `aop_cmd_db_region@81c60000` | `0x081C60000` | `0x081C7FFFF` | 0.1 | cmd-db | no-map |
| `aop_tme_uefi_merged_region@81c80000` | `0x081C80000` | `0x081CF3FFF` | 0.5 | firmware | no-map |
| `chipinfo_region@81cf4000` | `0x081CF4000` | `0x081CF4FFF` | 0.0 | firmware | no-map |
| `smem_region@81d00000` | `0x081D00000` | `0x081EFFFFF` | 2.0 | SMEM | no-map |
| `adsp_mhi_region@81f00000` | `0x081F00000` | `0x081F1FFFF` | 0.1 | ADSP/PIL | no-map |
| `pvmfw_region@0x824a0000` | `0x0824A0000` | `0x08259FFFF` | 1.0 | PVMFW | no-map |
| `global_sync_region@82600000` | `0x082600000` | `0x0826FFFFF` | 1.0 | firmware | no-map |
| `tz_stat_region@82700000` | `0x082700000` | `0x0827FFFFF` | 1.0 | TZ | no-map |
| `qdss_region@82800000` | `0x082800000` | `0x0847FFFFF` | 32.0 | debug | reusable |
| `qlink_logging_region@84800000` | `0x084800000` | `0x0849FFFFF` | 2.0 | debug | no-map |
| `q6_mpss_dtb_region@9b000000` | `0x09B000000` | `0x09B07FFFF` | 0.5 | MODEM/PIL | no-map |
| `ipa_fw_region@9b080000` | `0x09B080000` | `0x09B08FFFF` | 0.1 | firmware | no-map |
| `ipa_gsi_region@9b090000` | `0x09B090000` | `0x09B099FFF` | 0.0 | firmware | no-map |
| `gpu_microcode_region@9b09a000` | `0x09B09A000` | `0x09B09BFFF` | 0.0 | firmware | no-map |
| `lost_reg_mem` | `0x09B09C000` | `0x09B09FFFF` | 0.0 | firmware | no-map |
| `spss_region_region@9b0a0000` | `0x09B0A0000` | `0x09B27FFFF` | 1.9 | firmware | no-map |
| `spu_secure_shared_memory_region@9b280000` | `0x09B280000` | `0x09B2DFFFF` | 0.4 | firmware | no-map |
| `spu_secure_shared_memory_region@9b2e0000` | `0x09B2E0000` | `0x09B2FFFFF` | 0.1 | firmware | no-map |
| `camera_region@9b300000` | `0x09B300000` | `0x09BAFFFFF` | 8.0 | firmware | no-map |
| `video_region@9bb00000` | `0x09BB00000` | `0x09C2FFFFF` | 8.0 | firmware | no-map |
| `cvp_region@9c300000` | `0x09C300000` | `0x09C9FFFFF` | 7.0 | firmware | no-map |
| `cdsp_region@9ca00000` | `0x09CA00000` | `0x09DDFFFFF` | 20.0 | CDSP/PIL | no-map |
| `q6_cdsp_dtb_region@9de00000` | `0x09DE00000` | `0x09DE7FFFF` | 0.5 | CDSP/PIL | no-map |
| `q6_adsp_dtb_region@9de80000` | `0x09DE80000` | `0x09DEFFFFF` | 0.5 | ADSP/PIL | no-map |
| `adspslpi_region@9df00000` | `0x09DF00000` | `0x0A1F7FFFF` | 64.5 | ADSP/PIL | no-map |
| `ramoops_region` | `0x0AC300000` | `0x0AC3FFFFF` | 1.0 | ramoops | reusable |
| `hwfence-shmem` | `0x0D4E23000` | `0x0D50FFFFF` | 2.9 | firmware | no-map |
| `splash_region` | `0x0D5100000` | `0x0D7BFFFFF` | 43.0 | GOP/framebuffer | reusable |
| `tz_merged_region@d8000000` | `0x0D8000000` | `0x0D87FFFFF` | 8.0 | TZ | no-map |
| `trust_ui_vm_region@f3800000` | `0x0F3800000` | `0x0F7BFFFFF` | 68.0 | Gunyah VM | reusable |
| `oem_vm_region@f7c00000` | `0x0F7C00000` | `0x0FC7FFFFF` | 76.0 | Gunyah VM | reusable |
| `llcc_lpi_region@ff800000` | `0x0FF800000` | `0x0FFDFFFFF` | 6.0 | firmware | no-map |

Every fixed region is **below `0x100000000`**. None is inside the 5 GiB high-bank run. Owner counts: HYP/TZ 2, SMEM 1, cmd-db 1, ADSP/PIL 3, CDSP/PIL 2, MODEM/PIL 1, TZ 2, GOP 1, Gunyah VM 2, ramoops 1, PVMFW 1, debug 2, firmware 16.

The GOP reservation is `splash_region` 43 MiB at `0xD5100000`; a 3200×2000×32 framebuffer is 25.6 MB (24.4 MiB), so the rest is alignment / display plumbing. UEFI's `Display_Reserved` (43 MiB at the same address) agrees.

### 4.2 Dynamic regions (14, size only)

| Node | Size (MiB) | Owner |
| --- | --- | --- |
| `qseecom_region` | 20.0 | TZ |
| `qseecom_ta_region` | 16.0 | TZ |
| `qmc_dma_region` | 16.0 | firmware |
| `non_secure_display_region` | 164.0 | GOP/framebuffer |
| `vm_comm_mem_region` | 4.0 | firmware |
| `va_md_mem_region` | 16.0 | firmware |
| `sp_region` | 16.0 | firmware |
| `cdsp_eva_region` | 4.0 | CDSP/PIL |
| `adsp_heap_region` | 12.0 | ADSP/PIL |
| `linux,cma` | 32.0 | firmware |
| `secure_cdsp_region` | 72.0 | CDSP/PIL |
| `debug_kinfo_region` | 0.0 | firmware |
| `mem_dump_region` | 24.0 | firmware |
| `cnss_wlan_region` | 32.0 | firmware |

These have no `reg`; the bootloader places them. The live `memory` node already excludes their placement, so none can be inside the 5 GiB run — the run is listed whole.

## 5. UEFI `DeviceMemoryMap`

`MemoryMapLib.c` is the board's 40-descriptor table; `PcdBuildXblHobs=FALSE` means the final UEFI map is this table plus DXE runtime allocations, not an XBL HOB copy.

Critical high-bank row:

| Descriptor | Start | End | Size | Type |
| --- | --- | --- | --- | --- |
| `RAM_High` (before) | `0x8C0000000` | `0x97FFFFFFF` | 3 GiB | Conv |
| `RAM_High` (this change) | `0x8C0000000` | `0x9FFFFFFFF` | **5 GiB** | Conv |

Before the change, the largest `EfiConventionalMemory` block in the whole map was **3 GiB** (`0x8C0000000..0x980000000`); the 5 GiB System RAM run was only half described. The 5 GiB window therefore had `0x980000000..0xA0004000` (2 GiB + 16 KiB) excluded. That is exactly the `pmap: excluded memory 0x980000000..0xa00004000 (2097168 KB)` line from the old boot.

Low-bank rows that matter for any future multi-run plan:

| Descriptor | Start | End | Size | Type | Note |
| --- | --- | --- | --- | --- | --- |
| `RSRV0` | `0x816E0000` | `0x819FFFFF` | 3.1 MiB | Reserv | XBL/AOP |
| `SMEM` | `0x81D00000` | `0x81EFFFFF` | 2.0 MiB | Reserv | SMEM |
| `PIL_Reserved` | `0x8BC00000` | `0xA2B7FFFF` | 367.5 MiB | Reserv | ADSP/CDSP/modem/camera/video aggregate |
| `Kernel` | `0xA8000000` | `0xB7FFFFFF` | 256 MiB | Reserv | firmware kernel reserve |
| `DXE_Heap` | `0xB8000000` | `0xD2FFFFFF` | 432 MiB | Conv | the only large low-bank free run |
| `Display_Reserved` | `0xD5100000` | `0xD7BFFFFF` | 43 MiB | Reserv | GOP |
| `TZApps_Reserved` | `0xD8800000` | `0xF37FFFFF` | 432 MiB | Reserv | TZ/Gunyah |

Three rows are `MMAP_IO` but carry the `Conv` type: `DBI_Dump` (15 MiB at `0xA6100000`), `IMEM_Base` and `IMEM_Cookie_Base`. They are device windows, not DRAM; the verifier now treats `MMAP_IO` as non-RAM regardless of the type field. `DBI_Dump` matters only if the low bank is later adopted through `darwinboot-memory-outside`; it must be reserved explicitly.

The full 40-row table is in Appendix A.

## 6. DarwinBoot: one span, exclusions, one outside band

`DarwinBootPkg/Driver/MemoryWindow.h` and `DarwinBoot.c`:

- XNU gets **one contiguous span** `[PhysicalBase, PhysicalBase + MemorySize)`. `MemorySize` is `darwinboot-memory-size`; `PhysicalBase` is chosen by the loader.
- With `darwinboot-memory-excluded`, the span may cover **more than one free run**. The loader writes ascending `(base, size)` pairs for every range that is not `EfiConventionalMemory`, `EfiLoaderCode` or `EfiLoaderData`. Boot-services memory stays excluded even though ExitBootServices would free it.
- The first part of the span, the head, **is one allocation**: the loaded image plus the room XNU needs for its tables. `HeadSize = ALIGN(Layout.Used + DARWIN_WINDOW_RESERVE(WindowSize))`; `DARWIN_WINDOW_RESERVE(Size) = max(Size/64, 32 MiB)`. 5 GiB → 80 MiB; 6 GiB → 96 MiB.
- `AllocateSpan` allocates **only the head** (`gBS->AllocatePages(..., HeadSize, ...)`). The rest of the span is whatever the final map leaves usable, which is why holes are possible at all.
- The window start is `ALIGN_VALUE(free_start, 2 MiB) + (Kernel->Base & (2 MiB - 1))`. The prelinked kernel's `__TEXT vmaddr` is `0xFFFFFFF007004000`, so the offset is `0x4000`; on run C the start is `0x8C004000`, not `0x8C0000000`.
- `DarwinWindowExclusions` writes up to 64 pairs. If there are more ranges than slots, it joins the two neighbours with the least usable memory, giving up that memory but never leaving an excluded byte out.
- `DescribeWindow` refuses the window unless `DarwinRunEnd(Address, Address + HeadSize) == Address + HeadSize`, i.e. the head is one free run.
- `darwinboot-memory-outside` lists free runs **outside** the window, aligned inward to 64 KiB and at least 1 MiB, both below and above. XNU only adopts runs **below** the span.

The proven 5 GiB sidecar (`kernel.macho.adt`, 16,868 B) carries `dram-base=0x8C0000000`, `dram-size=0x140000000`, `darwinboot-memory-size=0x140000000`, a 64-slot excluded table and an empty 64-slot outside table.

## 7. XNU: one span, page-granular, no 16/64 MiB rule

`osfmk/arm64/arm_vm_init.c`, `osfmk/arm/arm_init.c`:

- `gPhysBase = args->physBase`, `gPhysSize = mem_size = ((gPhysBase + args->memSize) & ~PAGE_MASK) - gPhysBase`. `args->physBase` is the window start; `memSize` is the span size.
- `gDramBase` / `gDramSize` come from `/chosen/dram-base` and `/chosen/dram-size`; `pmap_first_pnum = atop(gDramBase)`. These describe the declared DRAM range, not necessarily the span.
- Excluded pairs are clamped to the span (`end > span_end` → `span_end`), the base is raised to `gPhysBase`, and a pair whose base reaches its end is dropped. The table must start after `topOfKernelData + ARM_VM_EXCLUDED_RESERVE(mem_size)`; `ARM_VM_EXCLUDED_RESERVE` is `max(span/64, 32 MiB)`.
- Page size is 4 KiB; `ARM_TT_TWIG_SIZE = ARM_TT_L2_SIZE` = 2 MiB with a 4 KiB granule. There is **no 16 MiB or 64 MiB span alignment rule**; the real constraints are 4 KiB page granularity, the 2 MiB block + kernel `0x4000` offset, and the head/reserve rule.
- `arm_vm_outside_init` accepts an outside pair only when `base >= gDramBase && end <= gPhysBase` (below the span, inside the declared DRAM), and the aperture may span at most 4 GiB of kernel VA. `ml_adopt_outside_memory` adopts pages one by one behind a probe load; a fault refuses the rest of the run, so another owner's memory is not touched.

Consequence for TB710FU: with `dram-base = 0x8C0000000`, the low bank (`0x80000000..0xFFFFFFFF`) is **below `gDramBase`** and every outside pair is rejected. The observed `memory outside the span: 0 pages adopted, 0 refused` is the expected result, not a bug.

## 8. Why 2 GiB wobbled

Two measured boots, 4 KiB pages (`scratch/com5_endurance.log` and `scratch/com5_boot_fresh.log`):

| ADT span | page entries | excluded | free pages | wired pages | managed |
| --- | --- | --- | --- | --- | --- |
| 2 GiB (`0x80000000`) | 476,231 | **0** | 476,231 (1.817 GiB) | 48,057 (187.7 MiB) | 2.000 GiB |
| 5 GiB (`0x140000000`), old 3 GiB `RAM_High` | 1,250,899 | 524,292 (2 GiB + 16 KiB) | 726,607 (2.772 GiB) | 59,821 (233.7 MiB) | 3.0009 GiB |

The 2 GiB span had **zero excluded pages**: every page was conventional memory and no firmware/TZ/HYP/ADSP/CDSP/GOP region overlapped it. It is physically clean.

The 5–6× wobble was the packaging regression documented in `CURRENT_STATUS.md`: `out/charge-battery-*` reverted `darwinboot-memory-size` to `0x80000000` **and** enabled `tb710fu-live-log=1`, `tb710fu-live-checkpoints=1`, `-enable_kprintf_spam` and `startup_debug=1`. Restoring the 5 GiB ADT and turning those knobs off gave `pl51` / `pl52` / `pl53b` / `pl54c` 4/4 clean boots.

So the wobble was a **capacity + instrumentation** failure, not an address collision:

- 2 GiB leaves 1.817 GiB free and 187.7 MiB wired; macOS 27.3's dyld shared cache, launchd, WindowServer and 120+ daemons need roughly 3 GiB of managed memory. The 2 GiB boot logs `dyld[1]: result from check_np(): -1, errno 12` (`scratch/com5_endurance.log:460`) before it maps the shared cache; 1.817 GiB free is below the working set.
- The debug knobs multiply the working set and slow the boot, turning an out-of-memory failure into a watchdog-style wobble loop.

That is why the fix is **more memory**, not a different address.

## 9. The 5 GiB proof

Chosen window:

| Property | Value |
| --- | --- |
| `darwinboot-memory-size` | `0x140000000` (5120 MiB, 5.000 GiB) |
| `dram-base` | `0x8C0000000` |
| `dram-size` | `0x140000000` |
| Loader window | `0x8C004000..0xA0004000` (5 GiB) |
| Non-usable inside | `0xA0000000..0xA0004000` (16 KiB, 4 pages) |
| Excluded slots | 64 (1 used) |
| Head | 80 MiB, fits `RAM_High` |

Proof obligations, all checked by `scripts/check-tb710fu-memory-map.py`:

| Obligation | Result |
| --- | --- |
| (a) window inside one real DDR bank | `0x8C0000000..0xA0000000` ⊂ bank 1 `0x880000000+0x180000000` |
| (b) zero overlap with 35 fixed `/reserved-memory` regions | none |
| (b) zero overlap with HYP/TZ, SMEM, cmd-db, ADSP/PIL, CDSP/PIL, MODEM/PIL, TZ, GOP, Gunyah VM, ramoops, PVMFW, debug | none |
| (b) zero overlap with all 40 UEFI descriptors (Reserv/BsData/RtData/MmIO and `MMAP_IO` windows) | none |
| (b) UEFI coverage | 5120.0 MiB of 5120.0 MiB conventional |
| (c) 4 KiB and 2 MiB alignment of base and size | pass |
| (c) size within the ADT contract (256 MiB..8 GiB, 2 MiB step) | pass |
| (c) kernel link offset | window starts at `0x8C004000` |
| (c) 80 MiB head is one conventional run | `RAM_High` |
| (c) excluded slots cover every non-usable range | 1 range, 64 slots |
| largest-run claim | 5120.0 MiB is the largest contiguous free run |

With `RAM_High` at 5 GiB, the predicted XNU accounting is 1,250,895 free pages (4.771 GiB) + 59,821 wired pages (233.7 MiB) = 5 GiB − 16 KiB managed, with 4 excluded pages. That prediction is from the map; the old 3 GiB row's measured 726,607 free / 59,821 wired is the baseline it extends by exactly 2 GiB.

`scripts/check-tb710fu-memory-map.py` run:

```
33/33 checks passed
```

With the built sidecar:

```
python scripts/check-tb710fu-memory-map.py --adt .../kernel.macho.adt
37/37 checks passed
```

`check-board.py`:

```
{"hardware_qualified": false, "memory_regions": 40,
 "memory_table_sha256": "8d30345696985c5c8b208ac586ffebdf82dc5d6b3dc9ab0f56c05b88eeeb92b6",
 "required_fv_modules": 11, "result": "PASS", "vendor_binary_references": 0}
```

`hardware_qualified: false` is the honest status: the table is structurally consistent, but the 5 GiB row has not been exercised on the tablet.

## 10. The ceiling: why 5 GiB and not 6.5 / 7 / 8

### 10.1 6 GiB is physically present but not in the firmware map

A 6 GiB span at `0x880000000` covers all of bank 1:

| Span | Free runs | Holes | Managed if the 922 MiB mid runs are `Conv` | Managed with today's map |
| --- | --- | --- | --- | --- |
| 6 GiB (`0x180000000`) | 763 + 159 + 5120 = 6042 MiB | 5 + 97 = 102 MiB | 6042 MiB − 16 KiB tail = **5.90 GiB** | 5120 MiB − 16 KiB (no gain) |

With today's map the two mid runs are **not described at all**, so `DarwinWindowExclusions` excludes them as well: the span would manage exactly the same 5 GiB as run C, with a larger 96 MiB head and two extra exclusions. It is not an improvement.

To get 5.90 GiB, the firmware table would need two more rows:

| New row | Start | Size | Type | Source |
| --- | --- | --- | --- | --- |
| `RAM_Mid0` | `0x880000000` | `0x2FB00000` | Conv | live/TWRP `memory` run A (763 MiB) |
| `RAM_Mid1` | `0x8B0000000` | `0x9F00000` | Conv | live/TWRP `memory` run B (159 MiB) |

Then `owned_memory_size=0x180000000`, `owned_memory_excluded_ranges=64`, and the 96 MiB head (`Layout.Used` + 96 MiB, roughly 316 MiB) fits inside run A. The two holes and the 16 KiB tail are excluded by the loader; 64 slots are ample.

Why this is documented but **not enabled**:

- It claims 922 MiB that the current UEFI map does not describe as RAM. The device tree says they are System RAM and no `/reserved-memory` child covers them, but the firmware map is the authority DarwinBoot reads; promoting them is a firmware-map change, not a pure XNU window change.
- It needs a device boot to prove that no XBL/UEFI agent used those runs before ExitBootServices. The map proof alone cannot establish that.
- With `RAM_Mid0/1` Conv, the DXE core may allocate from them; those allocations become excluded (not unsafe), but the yield is no longer exactly 6042 MiB.

### 10.2 6.3 GiB: the sheng recipe adds the low bank

The SM8650 `sheng` port in `.work/UniXNU-sheng` runs macOS 27.3 with exactly this 2 GiB + 6 GiB split:

- `dts/sheng.dts` declares `memory@80000000 { reg = <0 0x80000000 0 0x80000000>; }` — the low bank only.
- `board.toml` (macOS 27.3 profile): `owned_memory_size=0x180000000` (6 GiB), `owned_memory_excluded_ranges=64`, window at bank 1, one inaccessible range `0x8BB300000..0x8C0000000` (77 MiB).
- `owned_memory_outside_ranges=8` lists the low bank; `owned_memory_outside_reserved=["0xd0000000:0x10000"]` keeps the audio DSP buffer out.
- Hardware result: 458 MiB adopted, `hw.memsize 6.37 GiB`; expected range 5811–6067 MiB before the run.

For TB710FU the same recipe would need:

1. `dram-base=0x80000000`, `dram-size=0x80000000` (the low bank), because XNU only accepts outside runs with `base >= gDramBase`. That means changing the hardware DTS `memory` node from run C to the low bank, exactly as sheng does.
2. `owned_memory_size=0x180000000` plus `RAM_Mid0/1` from 10.1, so the span is 5.90 GiB.
3. `owned_memory_outside_ranges=8`; the only large low-bank `Conv` run is `DXE_Heap` 432 MiB at `0xB8000000`. `DBI_Dump` 15 MiB (`MMAP_IO` + `Conv`) must be added to `owned_memory_outside_reserved` (`0xa6100000:0xf00000`) or the probe could adopt a readable MMIO window.
4. A device boot. The probe refuses faulting pages, but it cannot detect readable MMIO.

Estimated total: 5.90 GiB + up to 0.42 GiB = **~6.32 GiB**, close to sheng's 6.37 GiB. This is the physical ceiling of the board, not 7 GiB.

### 10.3 6.5 / 7 / 8 GiB are arithmetically impossible

| Requested span | End | Verdict |
| --- | --- | --- |
| 6.5 GiB from `0x880000000` | `0xA2000000` | 32 MiB past bank 1 (`0xA0000000`) into MMIO |
| 7 GiB from `0x880000000` | `0xA4000000` | 64 MiB past bank 1 |
| 8 GiB from `0x880000000` | `0xA8000000` | 128 MiB past bank 1 |
| 8 GiB from `0x80000000` | `0x280000000` | crosses the 30 GiB MMIO hole; not one DRAM range |

### 10.4 Multi-bank answer

DarwinBoot/XNU **cannot manage two arbitrary banks**. The supported shapes are:

- one span over multiple runs of the same bank, with `darwinboot-memory-excluded` holes;
- one span plus **one lower band** adopted through `darwinboot-memory-outside`, and only when the band lies inside `[gDramBase, gPhysBase)`.

The ADT's `dram-base` chooses which band that is. With `0x8C0000000` the low bank is below `gDramBase` and is refused (0 pages on hardware). With `0x80000000` (sheng) the low bank is below the high-bank span and is adopted. There is no third bank and no second span.

## 11. Changes in this repository

| File | Change | Reason |
| --- | --- | --- |
| `board.toml` | `owned_memory_size` `0x80000000` → `0x140000000`; add `owned_memory_excluded_ranges=64` | the 2 GiB default was the regression value; 5 GiB is the largest contiguous run |
| `firmware/.../MemoryMapLib.c` | `RAM_High` `0xC0000000` → `0x140000000` | make the whole 5 GiB run conventional, not a 3 GiB subset |
| `firmware/.../Tools/check-board.py` | expected `RAM_High` 5 GiB | keep the critical-region guard in sync |
| `scripts/check-tb710fu-memory-map.py` | new verifier | prove (a) bank containment, (b) zero overlap, (c) alignment; 33/33 and 37/37 with ADT |
| `docs/MEMORY_MAP_8GB.md` | this document | full map, mechanics, 2 GiB analysis, ceiling |

No DarwinBoot or ADT-script source change is needed for 5 GiB: `scripts/unixnu/devicetree.py` already accepts 256 MiB–8 GiB in 2 MiB steps, 64 excluded slots and 64 outside slots. The 5 GiB request is entirely within the schema.

The commit does **not** touch `package/Tb710fuGpu/`, `Tb710fuWifi/`, `Tb710fuAudio/`, `Tb710fuInput/`, `Tb710fuBluetooth/`, `Tb710fuPlatformServices/` or `.github/workflows/`.

## 12. Reproduction and risk notes

```powershell
python scripts/check-tb710fu-memory-map.py
python scripts/check-tb710fu-memory-map.py --adt D:\unixnu-tb710fu-clean\out\pl16-45571102967-pl46-desktop-unsigned\kernel.macho.adt
python firmware\overlay\Platforms\Lenovo\tb710fuPkg\Tools\check-board.py
```

Expected: `33/33 checks passed`, `37/37 checks passed`, `"result": "PASS"` with `hardware_qualified: false`.

Risks and limits:

- **Map-proven, not boot-proven.** The 5 GiB window, the zero-overlap property and the 64-slot table are proven from the captured FDTs and the board table. The expanded `RAM_High` row has not been booted on TB710FU; the old 3 GiB row's 5 GiB ADT did boot 4/4. A device run is the remaining verification.
- **The two high-bank holes are not named by any DT node.** They are excluded from the chosen 5 GiB window (the window starts after them). Any future 6 GiB span must exclude them; if their owner is ever identified, the exclusion remains correct but the reason should be recorded.
- **The low bank is firmware territory.** 35 fixed regions plus dynamic TZ/ADSP/CDSP/GOP allocations live there. It is not part of the 5 GiB window and is not adopted today. The 6.3 GiB plan must use the probe plus explicit `outside_reserved` entries.
- **`hardware_qualified: false`.** Do not present the 5 GiB row as hardware-qualified until a device boot confirms the predicted free-page count.

## Appendix A: UEFI `gMemoryDescriptor` (40 rows)

`size = end - start + 1`; `0.0` means below 0.05 MiB. `Type` is the UEFI memory type from the table. `DBI_Dump`, `IMEM_Base` and `IMEM_Cookie_Base` are `MMAP_IO` descriptors that carry `Conv`; they are device windows, not DRAM.

| Descriptor | Start | End | MiB | Type |
| --- | --- | --- | --- | --- |
| `IPC_ROUTER_TOP` | `0x000400000` | `0x0004FFFFF` | 1.0 | `MmIO` |
| `SECURITY_CONTROL` | `0x000780000` | `0x000786FFF` | 0.0 | `MmIO` |
| `QUP` | `0x000800000` | `0x000AFFFFF` | 3.0 | `MmIO` |
| `PRNG_CFG_PRNG` | `0x0010C0000` | `0x0010CBFFF` | 0.0 | `MmIO` |
| `CRYPTO0_CRYPTO` | `0x001DC0000` | `0x001DFFFFF` | 0.2 | `MmIO` |
| `CLK_TCSR_TCSR_REGS` | `0x001F00000` | `0x001FFFFFF` | 1.0 | `MmIO` |
| `PERIPH_SS` | `0x008800000` | `0x0088FFFFF` | 1.0 | `MmIO` |
| `USB` | `0x00A600000` | `0x00A7FFFFF` | 2.0 | `MmIO` |
| `AOSS` | `0x00B000000` | `0x00EFFFFFF` | 64.0 | `MmIO` |
| `TLMM` | `0x00F000000` | `0x00FFFFFFF` | 16.0 | `MmIO` |
| `IMEM_Base` | `0x014680000` | `0x0146A9FFF` | 0.2 | `Conv`* |
| `IMEM_Cookie_Base` | `0x0146AA000` | `0x0146BFFFF` | 0.1 | `Conv`* |
| `SMMU` | `0x015000000` | `0x0151FFFFF` | 2.0 | `MmIO` |
| `APSS_HM` | `0x017000000` | `0x018FFFFFF` | 32.0 | `MmIO` |
| `RSRV0` | `0x0816E0000` | `0x0819FFFFF` | 3.1 | `Reserv` |
| `XBL_DT` | `0x081A00000` | `0x081A3FFFF` | 0.2 | `Reserv` |
| `XBL_Ramdump` | `0x081A40000` | `0x081BFFFFF` | 1.8 | `Reserv` |
| `AOP` | `0x081C00000` | `0x081C9FFFF` | 0.6 | `Reserv` |
| `UEFI_Log` | `0x081CE4000` | `0x081CF3FFF` | 0.1 | `Reserv` |
| `SMEM` | `0x081D00000` | `0x081EFFFFF` | 2.0 | `Reserv` |
| `PvmFw` | `0x0824A0000` | `0x08259FFFF` | 1.0 | `Reserv` |
| `PIL_Reserved` | `0x08BC00000` | `0x0A2B7FFFF` | 367.5 | `Reserv` |
| `Display_Demura` | `0x0A3600000` | `0x0A60FFFFF` | 43.0 | `Reserv` |
| `DBI_Dump` | `0x0A6100000` | `0x0A6FFFFFF` | 15.0 | `Conv`* |
| `FD_Reserved` | `0x0A7000000` | `0x0A70FFFFF` | 1.0 | `BsData` |
| `UEFI_FD` | `0x0A7100000` | `0x0A73FFFFF` | 3.0 | `BsData` |
| `UEFI_FD_Reserved` | `0x0A7400000` | `0x0A75FFFFF` | 2.0 | `BsData` |
| `Info_Blk` | `0x0A7601000` | `0x0A7601FFF` | 0.0 | `RtData` |
| `MMU_PageTables` | `0x0A7602000` | `0x0A7604FFF` | 0.0 | `BsData` |
| `Log_Buffer` | `0x0A7605000` | `0x0A760CFFF` | 0.0 | `RtData` |
| `UEFI_Stack` | `0x0A760D000` | `0x0A764CFFF` | 0.2 | `BsData` |
| `SEC_Heap` | `0x0A764D000` | `0x0A76D8FFF` | 0.5 | `BsData` |
| `Sched_Heap` | `0x0A76D9000` | `0x0A7AD8FFF` | 4.0 | `BsData` |
| `FV_Region` | `0x0A7AD9000` | `0x0A7ED8FFF` | 4.0 | `BsData` |
| `UEFI_RESV` | `0x0A7ED9000` | `0x0A7FFFFFF` | 1.2 | `BsData` |
| `Kernel` | `0x0A8000000` | `0x0B7FFFFFF` | 256.0 | `Reserv` |
| `DXE_Heap` | `0x0B8000000` | `0x0D2FFFFFF` | 432.0 | `Conv` |
| `Display_Reserved` | `0x0D5100000` | `0x0D7BFFFFF` | 43.0 | `Reserv` |
| `TZApps_Reserved` | `0x0D8800000` | `0x0F37FFFFF` | 432.0 | `Reserv` |
| `RAM_High` | `0x8C0000000` | `0x9FFFFFFFF` | 5120.0 | `Conv` |

`*` `MMAP_IO` with a `Conv` type; the verifier classifies it as `MMIO:Conv`, not usable RAM.
