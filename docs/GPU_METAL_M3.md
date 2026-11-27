# TB710FU GPU / Metal — Milestone 3 (ring=1)

M3 takes the hardware-verified M2 (`gpu-power=9`, `ring=0`, pl57d `50ccdb3`) to a
live CP ring and a real Metal device: the GMU cold start, GX power, GPU SMMU
context banks, CP SQE/AQE + RB0 and the NativeAdrenoABI v2 user client all run on
the TB710FU (SM8650Q-AB, Adreno 750 / gen70900).

Boot arguments (unchanged shape):

```
gpu-metal=1 gpu-power=9 gpu-addon=1 gpu-bundle-system=1
```

`gpu-power=7` stops after the GMU/HFI cold start, `8` stops after the CP fence
self-test, `9` publishes the ABI. `ring=1` is printed only when the GPU advanced
the fence word in RAM.

## What M3 adds

### Stage 7 — GMU cold start (gpu-power >= 7)

1. **GPU SMMU** at `0x03da0000` (4 KiB stage-1, SMMUv2, ArmSmmu4K encoding):
   context bank 1 for the GMU, SID 5 from `sm8650-lenovo-tb710fu.dts`; the GMU
   icache (4 MiB at IOVA `0x4000`, because gen70900 requests regions up to
   `0x324bf8`), dummy/log/HFI/debug buffers are mapped with write-back leaves and
   the bank is programmed and verified by read-back before `SCTLR.M`.
2. **SM8650 RPMh/RSC/PDC**: RSC `SEQ_MEM_0_DRV0` (generic `0x600` byte offset —
   A750 is `ADRENO_7XX_GEN3`, not a740-family), hidden TCS commands, override,
   `SEQ_START=0x4520`, match registers, then the AOP-side PDC (`SEQ_START=0`,
   `ENABLE_PDC=0x80000001`); the PDC TCS sequence itself stays in AOP, matching
   Linux's `pdc_in_aop` path.
3. **Firmware**: `gmu_gen70900.bin` (78,588 B, SHA-256 pinned) loaded block by
   block into ITCM/DTCM/icache through 32-bit MMIO writes (`mmio_copy32`, never
   `memcpy` on a device window), `dc cvac` + `dsb sy` before release.
4. **CM3 release**: FAL veto, cx_misc `TCM_RET_CNTL=1`, `FW_INIT_RESULT=0`,
   `BOOT_CONFIG=0x02`, QTBL address/info, AHB fence range, `CM3_CFG=0x4052`,
   `GENERAL_10=0x07090100`, `GENERAL_8=log`, `SYSRESET` toggle and a 100 ms poll
   for `FW_INIT_RESULT & 0x1ff == 0x100`.
5. **HFI**: 168-dword queue table, three 4 KiB queues, `HFI_CTRL_INIT`, then the
   A7xx **non-legacy** order `PERF_TABLE` (59 dwords, 11 GX + 2 GMU levels, ARC
   votes from the SM8650 cmd-db at `0x81c60000`), `BW_TABLE` (160 dwords, A740
   layout with CN0/SH0/MC0/ACV addresses), `CORE_FW_START`, `START`.

### Stage 8 — GX power, CP and the fence self-test (gpu-power >= 8)

1. `GX_BW_PERF_VOTE` (freq index 0) then the `GPU_SET` OOB handshake
   (`HOST2GMU_INTR_SET` bit 30, ack `GMU2HOST_INTR_INFO` bit 31). GX is judged by
   `SPTPRAC_PWR_CLK_STATUS` `(value & 3) == 0`, **not** by `POWER_STATE == 0xf`
   (0xf is the slumber state).
2. **GPU SMMU bank 0** (SID 0 and 1) with ring/fence/pwrup/SQE/AQE buffers at
   fixed IOVAs (`0x70000000`, `0x70010000`, `0x70011000`, `0x70020000`,
   `0x70040000`); the CP sees them through stage-1 translation.
3. UCHE GMEM range: base `0x100000`, 3 MiB (A750 is not a740-family, so Linux's
   `SZ_1M` base applies); `UCHE_CACHE_WAYS=BIT(23)`; the secure-video TSB door is
   cleared (a TZ rejection is reported, not hidden).
4. `gen70900_sqe.fw` (85,364 B) and `gen70900_aqe.fw` (33,176 B) copied into GPU
   memory at `CP_SQE_INSTR_BASE` / `CP_AQE_INSTR_BASE_0`; `CP_SQE_CNTL=1`.
5. RB0: `CP_RB_BASE=0x70000000`, `CP_RB_CNTL=0x20d` (64 KiB, 32 B blocks),
   `CP_RB_RPTR_ADDR=0x70010000`, WPTR=0.
6. **PM4 fence self-test** on the real ring: `CP_THREAD_CONTROL` (concurrent
   binning off), `CP_ME_INIT` (7 dwords, empty pwrup list), then
   `CP_SCRATCH(2)=1` + `CP_EVENT_WRITE7(CACHE_CLEAN|IRQ, fence_iova+4, seqno=1)`.
   The ring writes are `dc cvac`-cleaned before WPTR is pushed; the fence word is
   `dc ivac`-invalidated before each read. `ringReady` is set only when the word
   becomes 1.

### Stage 9 — live user client (gpu-power 9, ring=1)

`AGXDeviceUserClient` (type `0x4147`, NativeAdrenoABI v2) now serves:

* **BoMap** — maps a BO's pages at the caller's IOVA in SMMU bank 0 (read/write,
  WB, inner/outer shareable as requested), validates alignment/reserved fields.
* **Submit** — one `CP_INDIRECT_BUFFER` per `(handle, iova, bytes)`, rejects IBs
  outside the mapped BO, optional `wait_fence` first, then a monotonic fence
  packet; returns the fence sequence. Ring wrap pads with `CP_NOP`.
* **WaitFence** — bounded poll of the fence word (`timeout_ns=0` polls once),
  `kIOReturnTimeout` on expiry.
* **GetTimestamp** — `CP_ALWAYS_ON_COUNTER` (64-bit read of `0x2600`).
* **GetDeviceInfo** — chip `0x43051401`, GMEM 3 MiB at `0x100000`, VA
  `[0x10000000, 0x50000000)`, `ClientIova` (+`CachedCoherent` once ring-ready).
* **BoImport** — `IOMemoryDescriptor::withAddressRange(..., kIOMemoryPersistent,
  task)` + `prepare()`, read-only retry, released with `complete()` on BoFree.

BO free also unmaps the SMMU range (TLBIALL + TLBSYNC) before releasing pages.
The ABI is published only when `ringReady` is true; otherwise every GPU-address
method answers `kIOReturnNotReady`/`kIOReturnUnsupported` and `ring=0` is printed.

## Diagnostic surface

`Tb710fuGpuPower` publishes: `Tb710fuGpuStage7Step`, `Tb710fuGpuStage8Step`,
`Tb710fuGpuGmuBoot`, `Tb710fuGpuHfiError`, `Tb710fuGpuGxPower`,
`Tb710fuGpuCpFence`, `Tb710fuGpuRingReady`, `Tb710fuGpuSmmuId2`. COM5 lines:

```
TB710FU GPU: stage7 gmu step=12/12 boot=0x100 fwver=... chip=0x7090100 hfi_seq=4 hfi_err=0 ...
TB710FU GPU: stage8 gx step=7/7 gx=1 sptprac=0x0 rbbm_status=... cp_step=3 fence=1 ...
TB710FU GPU: stage8 ring=1 cp_step=3 fence=1
TB710FU GPU: stage9 abi live version=2 ring=1 chip=0x43051401 gmem=3145728 va=[0x10000000,0x50000000)
```

## Known limits (stated, not hidden)

* `gen70900_zap.mbn` is pinned and present but **not loaded**: SCM PAS is out of
  M3 scope. The driver clears `RBBM_SECVID_TSB_*` and reports whether TZ accepted
  it; without ZAP the secure/trusted path is not available.
* The A740 BW table is sent with SM8650 cmd-db addresses because Linux's
  `a750`-family path would otherwise fall back to A6xx hardcoded `0x50000`
  addresses, which do not exist on this SoC. The five uCode words and generic
  `SEQ_MEM` offset are GEN3 values, not a740 probes.
* UBWC facts (`highest_bank_bit`, `ubwc_swizzle`, `macrotile_mode`) are not
  pinned by the board sources; `GetDeviceInfo` reports 0/0/0 and UBWC 4.0, and
  the Metal plugin must not treat them as measured until a source is added.
* The CP `pwrup_reglist` is a 16-byte empty list (header only); the A740 driver's
  pwrup table is not pinned for SM8650 yet.
* M3 does not yet execute a full AGX firmware/command stream: Turnip's first
  submits exercise IB1/IB2 and fences, and Metal's WindowServer discovery reaches
  a real device, but a rendering pipeline needs the ZAP/AGX microcode work of M4.

## Verification

On the tablet (COM5), after installing the prelinked kernel and bundle:

```
ioreg -l | grep -E 'Tb710fuGpu(GmuBoot|HfiError|GxPower|CpFence|Stage7Step|Stage8Step|RingReady)'
```

All six stage facts must read 12, 7, `0x100`/`0x0`/`1`, and `RingReady=1`;
`ring=1` in the boot log is the authoritative check that the GPU, not the CPU,
wrote the fence. Host checks (`python3 scripts/check-tb710fu-gpu-host.py`) must
report 100% PASS before flashing.
