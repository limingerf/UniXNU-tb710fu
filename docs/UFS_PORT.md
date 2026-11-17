# TB710FU UFS Port

## Board inputs

These source facts are enough to begin board-specific driver adaptation. The
official [UFS porting guide](https://unixnu.com/zh/porting/ufs) does not require
a separate register-probe build before implementation. It asks for the static
hardware inventory first, then has the driver check current controller and
firmware ownership during initialization.

| Resource | TB710FU input | Source and limit |
| --- | --- | --- |
| Controller | `qcom,sm8650-ufshc`, `0x01d84000/0x3000` | TWRP FDT and pinned same-device Linux DTS; XNU access is untested |
| PHY | `0x01d80000/0x2000`, two lanes | TWRP FDT/Linux DTS; firmware ownership and tuning are not yet qualified |
| UFS stream | Apps SMMU SID `0x60` | TWRP FDT/Linux DTS; XNU's current mapping and context-bank ownership are runtime checks |
| GCC | Base `0x00100000`; UFS PHY GDSCR `0x77004`, memory-PHY GDSCR `0x9e000` | Pinned SM8650 Linux GCC driver; no XNU clock writes are authorized by this inventory |
| GCC branches | AXI `0x77018`, AHB `0x77024`, TX symbol `0x77028`, RX symbol 0 `0x7702c`, UniPro `0x77068`, ICE `0x77074`, PHY aux `0x770b0`, RX symbol 1 `0x770cc`, aggregation AXI `0x770e4` | Pinned SM8650 Linux GCC driver; read state during driver initialization, and account for hardware clock gating |
| Storage geometry | User LUN `/dev/block/sda`, 4096-byte logical blocks | Read-only TWRP observation; `userdata` is `/dev/block/sda14` |
| Known block | LBA 1, 4096 bytes, CRC-32 `44b9c49a`; GPT header CRC validates | Read-only TWRP sample metadata in [`ufs-gpt-lba1-twrp.json`](evidence/ufs-gpt-lba1-twrp.json); raw block was not retained |

The decoded FDT includes ICE/MCQ resources and fastmap DMA. The XNU driver must
not infer that these are ready or owned by firmware from their presence in the
tree. It must read HCE/HCS, the link and queue state, inspect the SID mapping,
and prove its DMA allocation is addressable before taking ownership. These
checks belong in the NativeUfs initialization/bring-up path.

Remaining board inputs to reconcile against the pinned Linux driver and FDT
include interrupt routing, reset/power sequencing, PHY tuning ownership, and
which UFS clocks firmware leaves enabled. They are source-review tasks; a
standalone live-register capture is not a prerequisite.

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
the board still sets `CONFIG_XNU_SOURCE_STORAGE=n`, and the clean device repo
does not contain the `SourceStorage.cpp`, `SourceRootPolicies.cpp`, and
`SourceRootBinding.cpp` bridge required by the full source-storage recipe. It
is not linked into the kernel and does not consume the new policy bundle.

## Verification status

The host-only `UfsReadProbe` state-machine tests pass, but that code has no XNU
MMIO, SMMU, DMA or queue callbacks. The first optional source build,
[run 44027734178](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44027734178),
failed while applying XNU patch 0010, before native compilation or UFS archive
compilation. Its context has since been corrected against the saved XNU source
after device patches 0001-0009; the corrected patch applies locally and the
host-only tests pass. There is no linked TB710FU XNU UFS driver or hardware
read. The full source-storage bridge and controller ownership checks must pass
before enabling it in the kernel configuration.
