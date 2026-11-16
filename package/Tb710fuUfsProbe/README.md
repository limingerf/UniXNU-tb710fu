# TB710FU UFS read-path contract

This directory contains a hardware-independent, host-testable contract for a
future one-block UFS read. It implements only:

- SCSI READ(10) CDB construction for LUN 0, LBA 1, one 4096-byte block.
- GPT signature/current-LBA/header-CRC checks and CRC-32/ISO-HDLC verification.
- A refusal-first state machine. The caller must verify the powered/link state,
  idle queue and SID `0x60` route; separately verify the full DMA mapping;
  atomically recheck and take over the idle queue; confirm the controller is no
  longer DMA-active; then restore the original queue before releasing the DMA
  buffer. Unknown state refuses the operation. An unquiesced or unrestorable
  controller quarantines the buffer instead of freeing memory it may still DMA
  into.

The code does not map UFSHCI or SMMU registers, allocate XNU DMA memory, inspect
the real SID `0x60` route, take over the firmware request list, or add an XNU
boot hook. The enable parameter in its unit-test contract is not a separate
register probe or runtime XNU service. Those callback operations intentionally
have no fake implementation in this slice. This is not yet a usable hardware
driver and is not linked into the current kernel. The captured GPT bytes were
not retained, so host tests use a synthetic GPT block and test the CRC
algorithm separately; they do not reconstruct or simulate the captured sample
bytes.

The captured acceptance tuple is user LUN `/dev/block/sda`, logical block size
4096, LBA 1, sample CRC32 `44b9c49a`, and sample SHA-256
`03f7bbae3cd21bdce06c3f21bf24254c4e297629be14533993c0ab5d46976617`. The GPT
header CRC validates. Raw bytes were deliberately not retained. The host tests
construct a synthetic GPT sample; they cannot recreate the captured device
block from its CRC/hash. Any GPT change invalidates this expected sample.

## Source and license

This implementation is original code. Its public design reference is the
official [UniXNU/ShengUfs](https://github.com/UniXNU/ShengUfs/tree/99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0)
component at commit `99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0`, licensed
BSD-2-Clause-Patent. No ShengUfs or unlicensed Vermeer implementation source is
copied here. The local source files and host fixture carry SPDX
`BSD-2-Clause-Patent`.

## Host verification

Run from WSL/Linux:

```sh
CC=cc python3 package/Tb710fuUfsProbe/Test/run_host_tests.py
```

The test builds with C11, `-Wall -Wextra -Werror -pedantic`, ASan and UBSan.
It checks the standard CRC vector, exact READ(10) fields, valid and malformed
GPT samples, default-off/refusal behavior, mapping checks, queue restoration,
read failure, and timeout/restore quarantine. It does not emulate UFS hardware
or prove the callback implementation.
