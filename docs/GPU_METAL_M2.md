# GPU Metal M2 — SM8650 Adreno 750 staged power bring-up and the native ABI

M2 turns `package/Tb710fuGpu/` from the M1 discovery stub (a registered `IOAccelerator` whose
`newUserClient` always returned `kIOReturnUnsupported`) into a driver that can power the SM8650
GPU's CX domain and GMU clocks, inventory the GPU SMMU, verify the four Gen70900 firmware images,
and answer the `NativeAdrenoABI` v2 wire protocol on a `AGXDeviceUserClient` connection — while
`stage 7` (GMU cold start) and `stage 8` (GX) keep refusing, so no pixel is scanned out and
`WindowServer` still has no drawable.

Everything is gated behind `gpu-power=<1..9>`. With no `gpu-power=` on the command line the
translation units register nothing, print nothing, map nothing: the pl56c zero-wobble boot is
byte-for-byte the same path. `gpu-metal=1` alone keeps the M1 behaviour (service published,
opens refused).

## Boot arguments

| argument | meaning |
| --- | --- |
| `gpu-metal=1` | publish `Tb710fuGpuAccelerator` with `MetalPluginName`/`NativeSandboxGrantProcess`, install both metaclasses |
| `gpu-power=1` | ADT + `/chosen/memory-map` addon census only (read-only, no mapping) |
| `gpu-power=2` | map the GCC GPU page and its vote page, read the clock/power state (read-only) |
| `gpu-power=3` | additionally power the CX GDSC and enable `gpu_cc_ahb_clk` + `gpu_cc_hlos1_vote_gpu_smmu_clk` |
| `gpu-power=4` | additionally map the GPU SMMU and inventory global space 0 (read-only) |
| `gpu-power=5` | additionally run the seven GMU clock steps (GPLL0 votes, PLL1, HUB/GMU RCGs, branches) |
| `gpu-power=6` | additionally verify `gmu_gen70900.bin`, `gen70900_sqe.fw`, `gen70900_aqe.fw`, `gen70900_zap.mbn` by SHA-256 |
| `gpu-power=7..8` | stage 7 / stage 8 print why they refuse; nothing is written |
| `gpu-power=9` | publish the ABI: `newUserClient` accepts type `0x4147`, ring stays `ring=0` |

`gpu-addon=1` (already used by M1) keeps the addon payload visible; `gpu-bundle-system=1` is the
plugin-loading switch from M1 and is unchanged.

## Hardware facts this code uses

All numbers live in `Tb710fuGpuHw.h` with their source named per line:

- Windows: GPU `0x03d00000/0x40000`, GMU `0x03d68000/0x37000` (live ADT, agrees with the zorn
  K80 table), GPUCC `0x03d90000/0xa000` (`gpucc-sm8650.c`), GPU SMMU `0x03da0000` (GR0 only),
  GCC GPU branch page `0x00171000`, GCC GPLL0 vote page `0x00152000`; chip id `0x43051401`.
- GPUCC: `cx_gdsc.gdscr=0x9108`, `gx_gdsc.gdscr=0x905c`, `cx gds_hw_ctrl=0x9168`
  (**SM8650, not SM8550's 0x953c**), `ahb=0x911c`, `cx_gmu=0x913c`, `cxo=0x9144`,
  `cxo_aon=0x9004`, `demet=0x900c`, `hub_cx_int=0x9148`, `smmu_vote=0x7000`,
  `hub_rcgr=0x93ec`, `gmu_rcgr=0x9318`, PLL0 at 0x0 (`l=0x20 alpha=0x4aaa`), PLL1 at 0x1000
  (`l=0x1b alpha=0x1555`).
- RCG encoding is `clk-rcg2.c`'s half divider: `field = 2*divisor - 1`, so HUB is `sel=5, field=5`
  (÷3) and GMU is `sel=3, field=3` (÷2).
- GDSC bits: `PWR_ON=bit31`, `SW_COLLAPSE=bit0`, `RETAIN_MEM=bit14`, `RETAIN_PERIPH=bit13`,
  `RETAIN_FF=bit11`, poll 2000 µs — the SM8550 sequence that already booted on hardware, with
  the SM8650 offsets.
- The GPU SMMU is read as SMMUv2 global space 0 only: `SCR0/ID0/ID1/ID2/GFSR`, `SMR(n)` validity
  scanned up to `NUMSMRG` (cap 128). Global space 1 aborts on this board, so it is never touched.

## What is deliberately not done

- **GX is never powered and the GPU window is never written.** The GMU owns GX; poking it from
  the AP before HFI handshake is how boards lose their GPU rail.
- **No SMMU context is programmed.** That needs the pinned `ArmSmmu4K` kit's live/audit/streams
  tables, not a hand-rolled 4-level walk.
- **No GMU cold start.** Stage 7 needs the GMU's own SMMU context (bank 1) plus the SM8650
  RSC/PDC tables; the SM8550 values are not transferable, so `stage 7` prints the missing
  prerequisite and returns.
- **No DDRSS AXI write.** The SM8650 `gcc_ddrss_gpu_axi_clk` offset was not confirmed in the
  fetched GCC source, so that branch is read and reported, never enabled.
- **No firmware is written into GPU memory.** Stage 6 reads the images whole, hashes them and
  compares to the manifest pins; loading belongs to stage 7, after the GMU has memory.

## What the user client does

`Tb710fuGpuClient.cpp` defines `AGXDeviceUserClient : IOUserClient` — the class name is required
because the sandbox profiles admit it and because Apple's AGX kext never loads on this board.
`externalMethod` first honours the reference driver's tunnel (`{0x4e544e4c, method}` header,
`0xffffffff` probe), then dispatches the direct selectors 1..18 with exact record sizes copied
from `NativeAdrenoABI.h` (Submit is `32 + 24*128 = 3104` bytes, metadata 80, device info 64).

Answered for real at `gpu-power=9`: `Query`, `BoAlloc`, `BoFree`, `BoCpuMap`, `BoCpuUnmap`
(shared `IOBufferMemoryDescriptor`, token returned from `clientMemoryForType`), `SetMetadata`,
`GetMetadata`, `CreateQueue`, `CloseQueue`, `SetPriority`, `Note`, `GetStatus`.

Refused with `kIOReturnUnsupported` and counted in `Tb710fuGpuAbiRefused`: `BoMap`, `Submit`,
`WaitFence` with a nonzero fence, `GetTimestamp`, `GetDeviceInfo` (its UBWC/GMEM fields are
board facts this port has not pinned), `BoImport`. A refused `GetStatus` is what Turnip expects
("counters not attached"); a zero-filled success for the others would be a lie that Metal
dereferences.

## Verification on the board (COM5)

Boot with `gpu-metal=1 gpu-power=1` first and confirm the census, then raise the level one stage
at a time; every stage is independent and stops on its own failure code.

```
gpu-metal=1 gpu-power=1
  TB710FU GPU: ADT kgsl-3d0 model=... chipid=43051401 compatible=... smmu=1 gmu=1 power=1
  TB710FU GPU: ADT windows gpu=0x3d00000/0x40000 gmu=0x3d68000/0x37000 gpucc=0x3d90000/0xa000 smmu=0x3da0000/0x40000
  TB710FU GPU: addon bundle=1 publish=1 firmware=4/4 stage=...
  TB710FU GPU: bringup stage=1 of 9 (gpu-power=1)

gpu-power=2
  TB710FU GPU: stage2 gcc cfg_ahb=0x... memnoc=0x... dvm=0x... gpll0_vote=0x...
  TB710FU GPU: stage2 gpucc cx=0x... gx=0x... ahb=0x... smmu_vote=0x... cx_gmu=0x...
  TB710FU GPU: bringup stage=2 of 9 (gpu-power=2)

gpu-power=3
  TB710FU GPU: stage3 cx on gdscr=0x... hw_ctrl=0x... ahb=0x... smmu_vote=0x... (writes=N)
  TB710FU GPU: bringup stage=3 of 9 (gpu-power=3)

gpu-power=4
  TB710FU GPU: stage4 smmu scr0=0x... id0=0x... id1=0x... id2=0x... oas=... gfsr=0x...
  TB710FU GPU: stage4 smmu programmed=none (contexts need the pinned ArmSmmu4K tables)
  TB710FU GPU: bringup stage=4 of 9 (gpu-power=4)

gpu-power=5
  TB710FU GPU: stage5 gmu_clocks step=7/7 failed=0 pll1_mode=0x... opmode=0x...
  TB710FU GPU: bringup stage=5 of 9 (gpu-power=5)

gpu-power=6
  TB710FU GPU: stage6 fw gmu_gen70900.bin=ok bytes=78588 source=addon digest=9685f62c
  TB710FU GPU: stage6 firmware=4/4 bytes=209152 addon=4
  TB710FU GPU: bringup stage=6 of 9 (gpu-power=6)

gpu-power=9
  TB710FU GPU: stage7 skipped: GMU cold start needs the GMU SMMU context (bank 1, ArmSmmu4K) ...
  TB710FU GPU: stage8 skipped: GX needs HFI startup votes from the AOP command DB tables ...
  TB710FU GPU: stage9 abi live version=2 ring=0 (Tb710fuGpuClient accepts type=0x4147)
  TB710FU GPU: bringup stage=9 of 9 ring=0 cx=1 gmu_clocks=1 firmware=4/4 (writes=N)
```

Then mount the plugin and ask IOKit for the accelerator: `ioreg -c Tb710fuGpuAccelerator` must
show `Tb710fuGpuMilestone=2`, `Tb710fuGpuAbiVersion=2`, `Tb710fuGpuRingReady=0`, and a
`NativeAdrenoMetal` load must reach `IOConnectCallStructMethod` with selector 1 (`Query`). With
`ring=0`, `BoMap`/`Submit` return `kIOReturnUnsupported`, which is the honest M2 state: the
connection exists, the ring does not.

## Failure semantics

Each stage records where it stopped in `TB710FU_GPU_BRINGUP` and returns; the next stage is
never entered on a failed predecessor. A stage 3 CX failure still leaves the ABI unpublished
(`gpu-power=9` alone cannot open the GPU), so the plugin sees `kIOReturnUnsupported` rather than
a half-live device.
