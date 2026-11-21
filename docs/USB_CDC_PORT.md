# TB710FU USB CDC port · 2026-11-17

## PHY sampling checkpoint · 2026-11-21

The latest supplied photo has SHA-256
`3d17a8bdc7fa5c51b2eee4b52fc2fa25c801223e99a333951421a3299788bc24`. Its
independent footer shows BSD `autoconf` and CDC `phy-data`, while the ordinary
console remains at the DLIL wait. No complete build label is visible. Run
44897167991's actual CDC archive and kernel contain `cdcphyg`, `phy-sample`,
and `cdcphyr`, so the photo cannot be tied to that exact build.

The adapter now follows the same-device Linux order. It votes the four board
PHY rails through the apps RSC active TCS, enables the PM8550B eUSB2 repeater
through the SPMI v7 arbiter, optionally votes `xo.lvl`, enables the USB clocks,
pulses the QUSB2 reset and applies the 38.4 MHz PHY sequence. TCS 2 is held
under the existing Apps SMMU lock when that symbol is present. The RSC completion
latch is not a reliable CPU-readable success signal on this board, so each
one-command vote uses the same bounded 2 ms delay and cleanup sequence as the
same-device reference. No guessed `SN2` interconnect vote is issued.

The adapter rejects an invalid SPMI version/count, non-owned or ambiguous
repeater APID, failed SPMI transaction and repeater timeout. It also marks each
PHY reset, reference-clock and data-register access before the volatile read as
`phy-rst`, `phy-ref`, `phy-u0`, `phy-u5`, `phy-c0` or `phy-h2`, then saves each
returned value immediately. The source changes still require a native arm64e
cloud build and exact hardware test. They do not establish Windows enumeration.

## Device result on the first self-heal boot · 2026-11-21

Run `44874414139` (`ca5c7f8`) booted through the whole UFS bring-up and printed
`TB710FU bsd: cdc-start begin`, then froze: no `cdc-start done`, no
`TB710FU CDC:` line at all. Patch 0034 had moved the hook to the right place, so
the stall is inside `tb710fu_cdc_start()` — either the worker wedged on one
register access, or the worker's kprintf and the initiator's kprintf serialize
on the console lock and neither returns.

The follow-up build (`d2e0aa7`, run `44877673063`) removes every print from the
worker. The worker publishes `gStage` before each risky step (`gcc-reset`,
`gcc-gdsc`, `gcc-master`, `gcc-sleep`, `gcc-utmi`, `gcc-cfg-noc`,
`gcc-aggre-noc`, `gcc-sample`, `clock-check`, `phy-sample`, `smmu-route`,
`core-id`, `dma-alloc`, `dma-check`, `dma-preflight`, `vbus`, `dwc3-start`,
`ready`) plus the sampled GCC/PHY/core/SMMU words; the BSD thread waits at most
100 ms and prints five lines itself. A `stage=` naming one register with
`settled=0` therefore points at the access that never returned, while
`settled=1` turns the line into an ordinary refusal with `failure=`.

## Platform self-heal for the USB30_PRIM block · 2026-11-21

A quiet 0009 boot now prints the entire UFS bring-up and stops after
`TB710FU bsd: source-storage-start done`, so the CDC hook has never had its
verdict photographed: it used to run before `vfsinit`, dozens of screens earlier.
Two changes make the next boot report itself.

`CdcXnu.c` now replays the bounded GCC-side sequence Linux performs in
`dwc3-qcom` probe: deassert the block reset (`GCC 0x00100000 + 0x39000`), power
the USB30_PRIM GDSC (`0x39004` control, `0x39008` status bit 16) and enable the
five branch clocks (`0x39018`, `0x39024`, `0x39028`, `0x39088`, `0x3908c`),
setting the software enable while leaving the read-only halt bit alone and
clearing the hardware-gating vote. All of these registers sit in the always-on
GCC aperture, so writing them cannot fault the USB interconnect; the GDSC gates
only DWC3 and the PHY. The PHY, PMIC/repeater, SMMU and UFS sides stay adoption
only: a refused PHY control or SID 0x40 route is still reported, never programmed.

A loader may also hand the core over in host or OTG role. `GCTL.PrtCapDir` is
now reported and left to `tb710fu_dwc3_start()`, which halts the core and selects
device mode regardless; only the core ID and DMA width remain hard requirements.
Each attempt prints two compact register lines (`gcc ...`, `phy ...`) before any
verdict, and `tb710fu_cdc_start()` waits at most 20 ms for the worker so those
lines stay the tail of the log. Patch 0034 moves the hook to immediately after
the storage handoff for the same reason.

Real enumeration is still unverified: no `TB710FU CDC:` line has appeared in any
captured boot, so which predicate used to refuse remains open until the next
photo, and the two printed lines name it.

## Current PRELINKTEXT checkpoint · 2026-11-20

The latest user QR is from `src-45d347f6-ramoops`, run `44546733725`, and is still
inside `machine_lockdown()` / `arm_vm_prot_finalize()` while changing PRELINKTEXT
PTEs. This is before `kernel_bootstrap_thread()` calls `bsd_init()`. Patch 0012
starts `tb710fu_cdc_start()` in `bsd_init()` immediately before `vfsinit()`, so
the CDC worker has not reached its startup call at this checkpoint. Its absence
from USB logs here is expected and does not show a port fault. The current build
links the provider and its test boot arguments enable `tb710fu-cdc=1`, but the
runtime start has not occurred. Real TB710FU enumeration and captured bytes
remain unverified.

The pinned UniXNU [4 KiB page-table guide](/D:/unixnu-tb710fu-clean/.work/UniXNU/docs/zh/xnu/four-k-page-tables.md)
describes the native 4 KiB granule/PTE geometry. The pinned
[debugging guide](/D:/unixnu-tb710fu-clean/.work/UniXNU/docs/zh/guide/debugging.md)
recommends framebuffer or UART evidence during early kernel entry and requires
target-protocol compatibility before using a USB terminal. The `i=384` progress
marker is emitted before applying PTE 384; it does not identify a fault PC.

The user's later screen also shows `pe_arm_map_interrupt_controller: failed to
find the interrupt-controller` and `pe_arm_init_debug: failed to find
cpu-debug-interface`, followed by progress into `vm_mem_bootstrap` / phase 5.
In pinned XNU, the interrupt-controller lookup failure makes
`pe_arm_init_interrupts()` return 0 and skip its `pe_arm_init_timer()` call, but
`PE_init_platform()` ignores that return, so this message alone does not stop
bootstrap. The missing CPU debug interface is optional unless `panic_trace` or
`stop_clocks` is requested; this package does not request either. The current
host-generated ADT's `/arm-io` has zero hardware children (`docs/ADT_STATUS.md`),
so the interrupt-controller lookup failure is expected from that ADT and remains
a concrete platform integration gap. Its effect on the board's other GIC/timer
setup and worker scheduling is not yet qualified; no speculative ADT node was
added from Linux source facts alone.

## Stage-56 hardware feedback and extended platform trace

The user now reports a working QR labelled `src-503599a0-cdc-qr`, stage 56,
ramoops initialization returned, and no port. This is the last instrumented
checkpoint in that candidate; it does not locate a stall there or establish
that BSD/CDC executed. ARG1 is not an established error code. See the
[verbatim observation](evidence/usb-cdc-stage56-device-20261117.json),
[actual startup path](evidence/usb-cdc-stage56-path-audit.json) and
[packaged-ADT/CDC audit](evidence/usb-cdc-stage56-adoption-audit.json).

The next signed bundle is `out/tb710fu-usb-cdc-platform-44088845841-signed.zip`,
label `src-503599a0-plat-qr`, SHA-256
`5bbd067261f417a44826b43077404f6db92642147a439e9e21771993cbcba9e7`.
It adds 17 verified original BL wrappers, bringing the total to 45. Stages 57–88
bracket the remaining arm_init platform calls, and 89 only enters the no-return
machine_startup. All stay before scheduled bootstrap-thread creation/switching;
the shared-stack/temporary-GOP reporter is not used from the CDC worker.
The new checkpoints add up to 66 seconds of continuing QR replay. Save the final
label, stage and full decoded report after the screen settles. Stage 89 still
does not prove BSD or worker execution.

The original CDC/storage code and native/prelink reports remain unchanged.
Sixteen mutation tests, 45 actual wrapper instruction models, full host QR/CRC
coverage and AArch64 rendering at stages 61/89 pass. Production parsing,
17-input packaging, local signing and independent RSA/payload readback pass.
See [transform proof](evidence/usb-cdc-platform-transform-44088845841.json),
[image record](evidence/usb-cdc-platform-image-44088845841.json) and
[signed record](evidence/usb-cdc-platform-signed-44088845841.json).
No USB enumeration, guard rejection reason or captured bytes are established;
this extends localization without claiming the no-port issue is fixed.

Entries below preserve the results known when each earlier candidate was made.

## QR restoration after the white-band report

The first signed CDC delivery omitted the separate post-link reporter, so its
actual kernel contained no diagnostic segments or compact entry markers.
The linked fileset-header-fix branch was already merged: its 0006–0009 hashes
match the actual CDC build inputs. This explains missing QR; it does not
identify the cause of the hardware stall. The user observation has no device
partition hash readback. See [the regression record](evidence/usb-cdc-qr-regression-44088845841.json).

The replacement `out/tb710fu-usb-cdc-qr-44088845841-signed.zip` includes the
restored reporter, label `src-503599a0-cdc-qr`, plus the matching signed images.
It preserves the original CDC/UFS code and explicit enabling boot argument.
The 17-input packaging path proves original prelink/integration first, then
checks the bounded diagnostic derivative and runs the parser on that actual
final kernel. It adds no default stopping checkpoint. See
[transform proof](evidence/usb-cdc-qr-transform-44088845841.json) and
[signed replacement](evidence/usb-cdc-qr-signed-44088845841.json).
Screen reporting remains useful before BSD startup; actual QR display and
CDC log output still need a hardware result for this exact replacement.

## Signed test delivery

After an explicit user request for local signing, the same verified candidate
was AVB-signed using the existing local key and unchanged tools. The delivery
is `out/tb710fu-usb-cdc-44088845841-signed.zip`, SHA-256
`a392526ed17436a02b1043f278cbf697d19a9c82da35ac966dd072db86b43df5`.
It contains the matching `boot.img`, `vendor_boot.img`, `vbmeta.img`, checksums,
reports and a Chinese test README. The loader is the existing recorded signed
handoff/LZMA image, not a new firmware build.

Independent RSA recomputation, salted image hashes, unchanged payload, four
FAT readbacks, decompressed kernel and the boot chain key all pass. Only the
top-level vendor_boot descriptor changed; the other 22 raw descriptors,
header, key and rollback fields are preserved. See
[the signed record](evidence/usb-cdc-signed-44088845841.json).
No device commands were run; actual enumeration/log capture remains unverified.

## Target build and unsigned candidate

Run [44088845841](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44088845841)
at `c0154c9d5e7048f7896614e1ca8d65be7d4d0e10` passed all three sanitized
host checks, compilation of all six actual sources, the BSD/native integration
verifier, strict prelink and production DarwinBoot parser. The 192,872-byte
CDC archive SHA-256 is
`97e3ff396c881f4aae0c3f6c33edc7ee69776d52c861fc1a937c50780ffce8df`;
the 142,802,944-byte prelinked fileset SHA-256 is
`503599a026ee362e9523aba1808f8af01601ae1323c7404fe7ecd1ed9a5690e2`.
Local source/header hashes match exact Git objects at that commit; all six
compile commands, dependency records and archive members match their reports.
See [the integrated record](evidence/usb-cdc-integrated-44088845841.json).

The 15-input positive packaging path then verified the actual six CDC objects
and 12 strong code definitions in the final fileset, producing
`out/usb-cdc-44088845841/vendor_boot.unsigned.img`, 37,859,328 B, SHA-256
`49b216089db0cec46772cbba068178fb82e12d86af3ba0c5e925cf0a26ed416c`.
It selects `-v serial=3 native-source-storage=1 native-ufs-root=1 tb710fu-cdc=1`.
See [the image record](evidence/usb-cdc-image-44088845841.json).
This is an unsigned candidate paired with the recorded existing loader.
No signing, private-key read, device operation or actual COM enumeration was
performed. The implementation and build result do not prove live USB bytes.

## Native transport integration

The current device sources add an independent USB 2 CDC log transport in
`CdcDwc3.c`, a polling XNU worker in `CdcXnu.c`, and source-backed SM8650
register predicates in `CdcHardware.h`. Patch `0012` strongly starts the worker
before `vfsinit()` only when the CDC build macro is selected. Runtime startup
also requires the exact boot argument `tb710fu-cdc=1`. This is a log channel:
bulk OUT input is received and discarded, rather than executing commands.

The core handles EP0 SETUP/DATA/STATUS, full/high-speed and other-speed
descriptors, seven-byte line coding, host DTR, SerialState notifications,
bulk IN log bytes and terminating zero-length packets. It starts control DATA
immediately after SETUP. A normal reset with only SETUP armed preserves that
TRB, including the common Windows reset after the first eight descriptor bytes.
Other active transfers require proven cancellation; unsupported reset,
disconnect and controller failures halt the session and permanently retain DMA.
Reconnect/reconfiguration recovery is not implemented. A completed bulk IN is
committed to the RAM ring once; consumer lock contention retries the commit
without transmitting the same bytes again.

The worker reads the actual clock, PHY control, DWC3 role and unique SID `0x40`
SMMUv2 route before writes. It accepts only explicit identity/bypass DMA,
derives register geometry and address width, allocates a permanent contiguous
16 KiB arena and checks every physical page. It then rechecks ownership before
the QSCRATCH USB2 VBUS override and DWC3 halt/reset. It changes no clock, PHY,
PMIC or SMMU register. Cache maintenance uses ARM64 `dc cvac`/`dc ivac` with
barriers and separate event/TRB/buffer lines; cache lines over 64 bytes are
refused. PHY control bits do not prove analog readiness or powered rails.

Development enumeration uses shared test VID:PID `1209:0001` and synthetic
serial `TB710FUCDC001`; these are not production identifiers or the device's
private serial. The `1209:710f` values in protocol tests remain fixtures only.
The controller API bounds serial text to 30 ASCII characters, so the selected
control replies cannot require an aligned short DATA/ZLP chain. Empty-length
descriptor requests stall and rearm SETUP instead of stopping the session.
Before replacing firmware event addresses, the bounded halt loop acknowledges
old event counts; those counts can otherwise prevent `DEVCTLHLT`.

Run the three `scripts/check_usb_cdc_{protocol,hardware,transport}.py` checks.
The last executes the production core with modeled MMIO/DMA rather than a
second implementation. The archive recipe retains source/header, compile
command, dependency-record, object and archive hashes. A separate CI verifier
checks the actual BSD macro/strong import, final strong transport definitions,
archive members and saved force-load input. Changing archive bytes invalidates
only pinned XNU's generated actual LD output to prevent stale incremental links.

Target compilation, strict prelink and true host enumeration are separate gates.
There is currently no TB710FU COM-port or captured USB-byte evidence for this
transport. The worker requires scheduling: it cannot promise the final panic
bytes after the scheduler has stopped, or logs before this BSD startup point.
The early RAM tap retains earlier text only if boot reaches the worker.

## Sources and baseline

Programming facts are checked against Linux v6.1
`drivers/usb/dwc3/{core.h,ep0.c,gadget.c}` and the MIT
reference `FFT-infra/QcomXnuUsbDebug` at
`18bcc385bc0c97ff38343a83040f19711392867b`, identified through the pinned
official `docs/zh/devices/alioth-taishang1024.md` CDC evidence. It was reviewed for DWC3
behavior, not adopted wholesale: it targets another SoC and its recovery is
not proof of DMA cancellation here. No unlicensed Vermeer controller code is
copied. SM8650 clock/GDSC/eUSB2/TCSR/QSCRATCH facts come from the pinned
same-device Linux revision below. File hashes and URLs are recorded in
`docs/evidence/usb-cdc-sources.json`.

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
bytes. Patch `0010` taps `_cnputc` after newline normalization and the direct
`PE_kputc` serial path, so the queue can retain text before a future USB worker
starts. It deliberately does not tap `serial_putc_options`, which is called
under `_cnputc` for serial output and would record each byte twice. The tap
filters carriage returns to retain one newline per line. It only captures
bytes; no DWC3 endpoint, CDC enumeration or queue drain over USB is connected
in that historical baseline. Native-only kernel build run
[44041029817](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44041029817)
compiled the XNU tap patch and CDC archive together. This validates compilation
only; it does not provide a host-visible port or hardware USB result.

The same tap and archive also remain selected in the successful integrated
native/prelink run
[44075573981](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44075573981),
device commit `9f01899957060ffbd81a49c793438ba152760eb9`. That run's UFS
integration does not supply a USB transport or establish CDC enumeration.

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

## Hardware acceptance

The target build must include the new core, worker and startup hook together.
A device test must identify the exact image, enumerate the CDC ACM device,
open its COM/tty port with DTR asserted and capture known XNU log bytes. Test
initial enumeration, sustained output and the failure path independently.
Unknown firmware role/clock/PHY/SMMU state refuses takeover while normal boot
continues; it does not trigger guessed reinitialization. A separate register
snapshot build is not required. Opening at 115200 8N1 is conventional; the
line-coding setting does not change USB link speed.

UFS remains a separate track. The TWRP GPT-header block is an expected-byte
baseline for the first read performed by the adapted NativeUfs driver. Its
initialization should verify power, link, SMMU, DMA and queue ownership before
issuing that command; a separate probe run is not required first. Do not reuse
Ace 5's CRC, partition LBAs, media policy, or its XNU 8796 class bindings. No
partition or filesystem was modified; the sample was read through the existing
TWRP/Linux UFS stack. The static hardware inventory and current adapter gaps
are collected in [UFS_PORT.md](UFS_PORT.md).
