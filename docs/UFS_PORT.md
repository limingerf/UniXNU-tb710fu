# TB710FU UFS Port

## Board inputs

These source facts are enough to begin board-specific driver adaptation. The
official [UFS porting guide](https://unixnu.com/zh/porting/ufs) does not require
a separate register-probe build before implementation. It asks for the static
hardware inventory first, then has the driver check current controller and
firmware ownership during initialization.

| Resource | TB710FU input | Source and limit |
| --- | --- | --- |
| Controller | `qcom,sm8650-ufshc`, `0x01d84000/0x3000`; GIC SPI 265, level-high | Pinned [SM8650 DTSI](https://github.com/SpendyYT/tb710fu-linux/blob/4d04410798b538fbe4b50664e3a66d9500a2ed31/arch/arm64/boot/dts/qcom/sm8650.dtsi#L4002); XNU access and IRQ delivery are untested |
| PHY | `0x01d80000/0x2000`, two lanes; `vdda-phy` and `vdda-pll` board rails | [SM8650 DTSI](https://github.com/SpendyYT/tb710fu-linux/blob/4d04410798b538fbe4b50664e3a66d9500a2ed31/arch/arm64/boot/dts/qcom/sm8650.dtsi#L3980) and [TB710FU DTS](https://github.com/SpendyYT/tb710fu-linux/blob/4d04410798b538fbe4b50664e3a66d9500a2ed31/arch/arm64/boot/dts/qcom/sm8650-lenovo-tb710fu.dts#L1255); Linux's SM8650 QMP table is source evidence, not proof of firmware runtime state |
| Reset and rails | Host reset GPIO 210 active-low; `vcc` from `vreg_l17b_2p5` (max 1.3 A), `vccq` from `vreg_l1c_1p2` (max 1.2 A); PHY rails `vreg_l1d_0p88` and `vreg_l3i_1p2` | [TB710FU DTS](https://github.com/SpendyYT/tb710fu-linux/blob/4d04410798b538fbe4b50664e3a66d9500a2ed31/arch/arm64/boot/dts/qcom/sm8650-lenovo-tb710fu.dts#L1245); declared wiring only, not live GPIO/regulator state |
| UFS stream and DMA | Apps SMMU SID `0x60`, `dma-coherent`, two lanes | [SM8650 DTSI](https://github.com/SpendyYT/tb710fu-linux/blob/4d04410798b538fbe4b50664e3a66d9500a2ed31/arch/arm64/boot/dts/qcom/sm8650.dtsi#L4040); XNU's current mapping, cache behavior, and context-bank ownership remain runtime checks |
| GCC and reset controller | Base `0x00100000`; UFS PHY GDSCR `0x77004`, memory-PHY GDSCR `0x9e000`, UFS PHY BCR `0x77000` | Pinned [SM8650 GCC driver](https://github.com/SpendyYT/tb710fu-linux/blob/4d04410798b538fbe4b50664e3a66d9500a2ed31/drivers/clk/qcom/gcc-sm8650.c#L3728); no XNU clock/reset writes are authorized by this inventory |
| GCC branches | AXI `0x77018`, AHB `0x77024`, TX symbol `0x77028`, RX symbol 0 `0x7702c`, UniPro `0x77068`, ICE `0x77074`, PHY aux `0x770b0`, RX symbol 1 `0x770cc`, aggregation AXI `0x770e4` | Pinned SM8650 Linux GCC driver; read state during driver initialization, and account for hardware clock gating |
| Clock topology and OPP | SoC DTS names eight UFS host clocks plus PHY ref/ref_aux/qref; host and PHY use separate GDSCs; UFS OPP entries are 100, 201.5, and 403 MHz | [SM8650 DTSI](https://github.com/SpendyYT/tb710fu-linux/blob/4d04410798b538fbe4b50664e3a66d9500a2ed31/arch/arm64/boot/dts/qcom/sm8650.dtsi#L3984); parent/source selection and firmware-enabled state still need runtime observation |
| Interconnect | `ufs-ddr` uses AGGRE1 UFS-MEM to MC virtual EBI1; `cpu-ufs` uses GEM/CONFIG paths | [SM8650 DTSI](https://github.com/SpendyYT/tb710fu-linux/blob/4d04410798b538fbe4b50664e3a66d9500a2ed31/arch/arm64/boot/dts/qcom/sm8650.dtsi#L4028); Linux bandwidth votes are not part of the current XNU adapter |
| Storage geometry | User LUN `/dev/block/sda`, 4096-byte logical blocks | Read-only TWRP observation; `userdata` is `/dev/block/sda14` |
| Known block | LBA 1, 4096 bytes, CRC-32 `44b9c49a`; GPT header CRC validates | Read-only TWRP sample metadata in [`ufs-gpt-lba1-twrp.json`](evidence/ufs-gpt-lba1-twrp.json); raw block was not retained |

The decoded FDT includes ICE/MCQ resources and fastmap DMA. The XNU driver must
not infer that these are ready or owned by firmware from their presence in the
tree. It must read HCE/HCS, the link and queue state, inspect the SID mapping,
and prove its DMA allocation is addressable before taking ownership. These
checks belong in the NativeUfs initialization/bring-up path.

The pinned same-device Linux source now supplies static interrupt, reset, rail,
clock, power-domain, interconnect, and OPP declarations. Firmware's current
clock votes, reset level, supply state, controller/link state, queue ownership,
and apps-SMMU context remain runtime facts for the driver to check. No separate
standalone register-capture build is required before adaptation.

## Adapter and verification path

The pinned component is `UniXNU/ShengUfs@99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0`.
It contains real UFSHCI queue, SMMU and command-path code, but its component
metadata targets SM8550 and lists the iOS 17 ABI. The arm64e public-source path
for XNU 12377 has separate IOStorageFamily/APFS and signed root-policy inputs;
the upstream README marks that path unbuilt and unqualified. It is not a
drop-in for this TB710FU macOS 27 board.

The selected macOS profile provides IPSW build `26C5035r` and the OS, SystemOS,
and App image hashes and volume UUIDs. The official profile merge puts
`ipsw.build` into the resolved board, and `unixnu build kernel` exports it as
`UNIXNU_IPSW_BUILD`.

The matching owner policies are now prepared under `scripts/policies/26C5035r/`.
The VerifiedRoot C host verifier authenticated all 3,538,432 OS, 712,192
SystemOS, and 13,312 App blocks with one RSA-3072/SHA-256 owner key. The rebuilt
userdata image contains the raw 32-byte-per-block hash tables as `OS.vroot`,
`SystemOS.vroot`, and `App.vroot`; its final size and SHA-256 are recorded in
[`USERDATA_BUILD.md`](USERDATA_BUILD.md). This is host-only evidence and does not
establish that XNU can mount the root.

The board adaptation should preserve the official NativeUfs lifecycle while
replacing only board-specific resources and matching behavior. Driver bring-up
must verify the inherited state, safely take over an idle queue, allocate and
map DMA, issue a read-only command, and compare a fresh LBA 1 read against the
recorded sample. Root publication and persistence are later gates; no partition
or filesystem write is part of this porting step.

The board patch `patches/ufs/0001-sm8650-compatible.patch` adds only the exact
`qcom,sm8650-ufshc` compatible already declared by the TB710FU DT. The optional
`ufs-source` workflow first completes the minimal native kernel, then compiles
the pinned upstream source with this patch into an arm64e archive using that
kernel's compiler records. The archive remains an isolated source-build result:
the board still sets both `CONFIG_XNU_NATIVE_UFS=n` and
`CONFIG_XNU_SOURCE_STORAGE=n`. The clean device repo does not contain the
`SourceStorage.cpp`, `SourceRootPolicies.cpp`, and `SourceRootBinding.cpp`
bridge required by the separate full source-storage recipe. Those files are
not prerequisites for compiling the standalone NativeUfs archive. NativeUfs
kernel integration still needs its own component/archive/configuration and
matching ABI review. Neither route is linked into the current kernel or
consumes the new policy bundle.

## Pinned source review

The successful source build used `ShengUfs` revision
`99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0`. Its build report records upstream
`NativeUfs.cpp` SHA-256
`8e99199aa87dc2ccebfe3366a0f6896fc8937d57670117af66c10ee52b1b28cd` and board
patch SHA-256
`8697b355a151c3cdb4aae8b595c58c05f760b4c3ddd6ce458ac5c8a1571b7993`.

The source header identifies this as SM8550 bring-up. Its probe accepts only
`qcom,sm8550-ufshc`; the device-owned patch adds the TB710FU's
`qcom,sm8650-ufshc` string to both the `OSData` and `OSString` matching paths.
The source `GccUfs.h` controller, GCC, and UFS clock register addresses match
the values in the board inventory above. This static comparison found no
controller-address, interrupt, reset, rail, clock, or SID declaration mismatch.
The exact same-device Linux commit
`4d04410798b538fbe4b50664e3a66d9500a2ed31` declares host IRQ SPI 265, GPIO 210
active-low reset, the `vcc`/`vccq` and PHY rails, SID `0x60`, coherent DMA, two
lanes, interconnect paths, and OPPs. The SM8650 QMP driver selects a dedicated
two-lane PHY v6 table up to HS-G5 for `qcom,sm8650-qmp-ufs-phy`. These are
board/SoC declarations and Linux PHY data; they do not prove live macOS state.

`NativeUfs::start` stays inactive without its root or probe boot argument.
Root bring-up maps one physically contiguous DMA allocation, stops the current
request queue only when it can prove the queue is idle, then configures the
apps-SMMU mapping for SID `0x60`. The path assumes firmware already initialized
the controller and link. `GccUfs.h` explicitly leaves reset, RPMh/TCSR, and
QMP-PHY programming out of the driver; completion is polled with interrupts
masked. These are takeover assumptions to validate against TB710FU sources and
runtime observations, not settings to copy from the SM8550 board.

The pinned Linux driver cold-starts by resetting the host and PHY, initializing
and setting PHY mode, powering/calibrating it, then selecting UniPro mode. The
current ShengUfs path instead adopts firmware's running controller and link;
this board has no XNU reset/clock/PHY sequence, so takeover must remain
conditional on measured firmware state.

The board-owned
[`0002-transactional-handoff.patch`](../patches/ufs/0002-transactional-handoff.patch)
adapts this path without editing the fetched source cache. It plans the DMA
range and SMMU context before touching the host, snapshots the selected SMMU
and UTRL state, stops the idle firmware queue, then applies the new stream
mapping with readback. A configuration failure restores the saved context and
firmware queue. If replacement UTRL startup fails, the driver first confirms
the new queue and doorbells are stopped before restoring the SMMU and firmware
UTRL. Any uncertain rollback panics and retains DMA ownership. Since interrupt
status is write-one-to-clear, the old status is left untouched until the new
queue reaches ready state. The adapter also checks the inherited UFS PHY power
and bus-clock bits, rejects the Sheng-specific Wi-Fi/DPU boot arguments, and
disables legacy adoption probes that touched the firmware UTRL before checking
it idle.

The official arm64e component recipe does not accept the standalone archive as
a kernel integration. `build-arm-driver.py ShengUfs` selects the complete
source-storage recipe, which expects device-owned
`SourceStorage.cpp`, `SourceRootPolicies.cpp`, and `SourceRootBinding.cpp`.
Those files are absent here. The macOS 27 IOStorageFamily/APFS vtable and
linkage also need an explicit ABI audit; a successful object compile or equal
class size is not enough. Keep `CONFIG_XNU_NATIVE_UFS` and
`CONFIG_XNU_SOURCE_STORAGE` disabled until the bridge implementation, ABI
checks, and ordered ownership handoff pass their build gates.

## Verification status

The production SMMU and queue/command fault-injection tests now pass against the
patch-applied source tree with WSL Debian `g++ 14.2.0` and `clang++ 19.1.7`,
both with `-Wall -Wextra -Werror`. The queue snapshot rechecks HCE/HCS readiness,
interrupt mask, RUN_STOP and the original UTRL base after SMMU planning. The SMMU
tests require CLIENTPD clear, IDR0.SMS set and EXIDENABLE clear, and cover stale
planning, ignored writes, exact restoration, new aliases and failed restoration
detection. A stale plan after queue stop blocks firmware DMA resumption. The
queue tests cover idle refusal, stop/start
failure, UTRL address restoration, request validation and quarantine. They use
simulated register and DMA callbacks; they do not prove XNU MMIO behavior,
firmware races, device DMA or boot. No macOS 27 compiler run has yet tested this
new patch. The earlier host-only `UfsReadProbe` state-machine tests also pass,
but that code has no XNU MMIO, SMMU, DMA or queue callbacks. The first source build,
[run 44027734178](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44027734178),
failed while applying patch 0010. After correcting its context against the
saved XNU source following patches 0001-0009,
[run 44041029817](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44041029817)
completed successfully and uploaded a verifiable artifact.

The native-only kernel is 25,171,504 bytes, SHA-256
`0bb0d07e118ea4261613536b185a4dd80aaf90b97df75653c634c2e9bc54f948`; it was
not prelinked or hardware-tested. The standalone archive
`libTb710fuNativeUfs.a` is 541,448 bytes, SHA-256
`56a28627693c848f450614f293f3661f1632112ca1801cf2bdc004f341768c3c`. Its
build report confirms the pinned upstream revision and XNU compile record, and
marks `linked_into_kernel=false`, `root_policy_ready=false`, and
`runtime_qualified=false`. The downloaded artifact's SHA-256 matches GitHub's
digest. Local outputs are under `.work/ufs-source-44041029817/`.

There is still no linked TB710FU XNU UFS driver or hardware read. The full
source-storage bridge, macOS 27 storage ABI checks, and transactional
controller/SMMU ownership handoff must pass before enabling it in the kernel
configuration. Until then, there is no new UFS kernel payload to package.
The existing signed UEFI-UFS image is a firmware image and is not the NativeUfs
XNU driver. The tracked kernel packager also has no accepted ADT-correct source
baseline or UFS root-argument input, so it cannot yet produce a matching
flashable UFS kernel pair.
