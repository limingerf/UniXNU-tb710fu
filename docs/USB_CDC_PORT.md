# TB710FU USB CDC port · 2026-11-17

## Implemented slice

`package/Tb710fuUsbCdc/` contains an independent, hardware-free CDC-ACM
protocol layer. It builds USB 2.0 device/configuration/string descriptors,
validates the standard and CDC class setup requests used by enumeration, and
provides a bounded log ring. A producer never waits for the ring lock: if a
writer finds the ring busy or full, its bytes are counted as dropped. The
single consumer copies pending bytes and only consumes them after a successful
transport write. `CdcTransportGuard` separately rejects transport startup
unless a caller supplies a verified device role, clocks/PHY, unique identity
SMMU route, and a contiguous, aligned DMA range.

`CdcKernelTap` provides a statically allocated 64 KiB RAM queue for XNU console
bytes. Patch `0010` adds nonblocking writes from `_cnputc` and the direct
`PE_kputc` UART paths, so the queue can retain text before a future USB worker
starts. It only captures bytes; no DWC3 endpoint, CDC enumeration or queue
drain over USB is connected yet. The target kernel build must validate the
archive and XNU patch together.

Run `python scripts/check_usb_cdc_protocol.py` to compile the host fixture with
warnings-as-errors and AddressSanitizer/UndefinedBehaviorSanitizer, then run the
descriptor, malformed-request, wraparound, back-pressure and fail-closed guard
checks. These tests cover protocol/ring/decision logic only; they do not read
TB710FU hardware state or emulate a DWC3 controller.

The code is a fresh implementation of the USB CDC-ACM contract. It does not
copy the Vermeer source. The host test's `0x1209:0x710f` values are fixtures,
not assigned or selected device identifiers. The reference pin in Ace 5's
[`ios16/sources.json`](https://github.com/tycykp/UniXNU-OnePlus-Ace5/blob/main/ios16/sources.json)
identifies Vermeer commit
`d68e05df683f863f054b0f9c063c54d2b1342b2a`; the Ace 5 hardware report is useful
for behavior and acceptance criteria, not as an XNU 12377 build input.

The Ace 5 report records a CDC-ACM device on Windows `COM21`, 8,802 captured
status bytes and no drops. Its log path mirrors XNU serial/kprintf output into
RAM and drains it over USB; it is not a general-purpose kernel TTY. A later
report records a CDC descriptor request failure, so enumeration itself also
needs repeat testing.

## TB710FU facts and limits

The local TWRP FDT source used for this audit has SHA-256
`399e9e656be6ed451a4c56b1ae2b77da96119dd10d5cf2fc01a90b44ded88103`; its DTB
has SHA-256 `57b2c3cdc08ff367e18e3b3854c853f1375d6f5e21f12c5f09b7e9831975a108`.
The pinned Linux SoC DTSI has SHA-256
`a1606dfe264a11411afbbdf7a0b9da6b28deaaa6254eae79a6aee45d7ab24d817`.
Those local source artifacts are ignored by Git; the repository's
[`TWRP_REFERENCE.md`](TWRP_REFERENCE.md) records their originating project.
The decoded FDT declares DWC3 core `0x00a600000`, `dr_mode = "otg"`, nested
SMMU stream ID `0x40`, atomic DMA and an eUSB2 PHY at `0x088e3000`. It lists
the controller clocks and resets but does not reveal which state XNU receives
at this boot.

The same FDT declares UFS host `0x01d84000`, PHY `0x01d80000`, stream ID
`0x60`, ICE and MCQ registers, plus `qcom,iommu-dma = "fastmap"`. The pinned
Linux DTS has the same host/PHY/SID declaration. These inputs improve the
resource comparison with Ace 5, but do not establish live XNU queue ownership,
SMMU context contents or DMA cache behavior.

## Read-only UFS sample

TWRP ADB identified `/dev/block/sda14` as `userdata`; the parent user-LUN
device `/dev/block/sda` reports a 4096-byte logical block size. A single read
of LBA 1 returned 4096 bytes beginning with `EFI PART`. The GPT header is 92
bytes; its stored and independently recomputed header CRC32 are both
`0d00f8ae`. CRC-32/ISO-HDLC (`zlib.crc32`) over the entire 4096-byte sample is
`44b9c49a`; SHA-256 is
`03f7bbae3cd21bdce06c3f21bf24254c4e297629be14533993c0ab5d46976617`.

The raw block and device serial were not retained. The machine-readable record
is [`ufs-gpt-lba1-twrp.json`](evidence/ufs-gpt-lba1-twrp.json). This establishes
a Linux/TWRP reference sample only. It does not test XNU's UFS transport,
queue handoff, SMMU mapping or DMA. GPT changes will change this sample.

## Next integration gates

The XNU 12377 DWC3 peripheral transport and `serial_console`/`kprintf` hook are
not implemented. The controller must perform its bounded role, clock, PHY and
SMMU checks during its own initialization, then keep the log producer path
nonblocking. Unknown state must refuse DMA takeover and let normal boot
continue. A separate register-snapshot build is not required before this work.

UFS remains a separate track. The TWRP GPT-header block is an expected-byte
baseline for the first read performed by the adapted NativeUfs driver. Its
initialization should verify power, link, SMMU, DMA and queue ownership before
issuing that command; a separate probe run is not required first. Do not reuse
Ace 5's CRC, partition LBAs, media policy, or its XNU 8796 class bindings. No
partition or filesystem was modified; the sample was read through the existing
TWRP/Linux UFS stack. The static hardware inventory and current adapter gaps
are collected in [UFS_PORT.md](UFS_PORT.md).
