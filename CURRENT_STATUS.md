# Current status · 2026-11-17

## CDC test bundle locally signed and independently verified · 2026-11-17

The user explicitly requested local signing for hardware testing after the
unsigned delivery. This authorization supersedes the earlier working-port
condition for this signing step; it does not authorize device commands.
The existing signer and avbtool were used unchanged with the existing local
key. No key material is published or committed.

The signed set is `out/usb-cdc-44088845841-signed/`:

| File | Bytes | SHA-256 |
|---|---:|---|
| `boot.img` | 100,663,296 | `a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83` |
| `vendor_boot.img` | 100,663,296 | `80fd313b2c349bb89fa3790dc6caf4fd577ad09f0767ec79c2de7c71f796b279` |
| `vbmeta.img` | 8,000 | `e8a4d1da9db5d60dedeade8ab3d12ae12f45d84be8b7bb036bcba101d2865323` |

Independent public-key RSA-4096/PKCS#1/SHA-256 recomputation passes for all
three AVB structures. Salted boot/vendor hashes, unchanged unsigned payload,
all four FAT files and decompressed kernel match. The top-level vbmeta changes
only the vendor_boot hash descriptor: its other 22 raw descriptors, complete
header, public key and rollback/flags remain unchanged. The boot chain key
matches the included existing signed handoff/LZMA loader; no firmware was rebuilt.

The delivery ZIP `out/tb710fu-usb-cdc-44088845841-signed.zip` is 35,549,927 B,
SHA-256 `a392526ed17436a02b1043f278cbf697d19a9c82da35ac966dd072db86b43df5`.
All ZIP members were read back and hashed; a Chinese README, SHA256SUMS and
signing/verification reports are included. Public evidence is
`docs/evidence/usb-cdc-signed-44088845841.json`.

This completes local signing for testing. No flash, reboot, partition or device
filesystem operation occurred. Actual TB710FU CDC enumeration and USB log
capture remain unverified, as do hardware UFS/root mount and macOS userspace.
The firmware-adoption, BSD/scheduler and active-DMA reset limits below remain.

Entries below retain their original stage and signing scope.

## Native USB CDC linked, prelinked and unsigned image verified · 2026-11-17

Run [44088845841](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44088845841)
at device `c0154c9d5e7048f7896614e1ca8d65be7d4d0e10`, job
`134058374678`, passed the supported macOS arm64e build, actual CDC/storage
integration checks, strict prelink and production DarwinBoot parser. All three
CDC host checks passed under ASan/UBSan on the runner. Artifact `14330634249`
is 38,483,204 B; its independently verified ZIP SHA-256 is
`346ec4a926a72bf6bc52768aaa50546858f966c629db20a820859ea8deef8369`.

The native kernel SHA-256 is
`3daf19507bdf3d7ca5943f165cde4611e710b58131e4dcb956fef8f635628215`.
The prelinked fileset is 142,802,944 B, SHA-256
`503599a026ee362e9523aba1808f8af01601ae1323c7404fe7ecd1ed9a5690e2`, with
79 executable entries, 86 closure components, no missing references or stubs.
The six-object CDC archive is 192,872 B, SHA-256
`97e3ff396c881f4aae0c3f6c33edc7ee69776d52c861fc1a937c50780ffce8df`.
Its bound integration report proves the actual BSD strong reference, final
transport definitions and force-load input. Archive-creation reports precede
the final link and must not be mistaken for its outcome. Local verification
matched all 12 source/header hashes against the exact Git objects, all six
command/dependency records and every archive member. The actual BSD object and
compile record are hashed by CI but not retained as separate artifact members.
See `docs/evidence/usb-cdc-integrated-44088845841.json` and
`docs/evidence/usb-cdc-local-verification-44088845841.json`.

The new storage archive SHA-256 is
`a87e8b55568242ccba39013ab8a1d0b439f5d4dc4580e2a61f1820ecabd628e5`.
Its repeated static ABI audit against the unchanged exact-profile historical
reference passes 326 PAC slots, 26 named methods, class size `0x90` and four
binary consumer dispatch sites. See `docs/evidence/storage-abi-44088845841.json`
and its separate provenance file; this is static evidence, not executed UFS.

The packager at the same device commit verified all 15 inputs and produced
`out/usb-cdc-44088845841/vendor_boot.unsigned.img`, 37,859,328 B, SHA-256
`49b216089db0cec46772cbba068178fb82e12d86af3ba0c5e925cf0a26ed416c`.
Manifest SHA-256 is
`f32068c96c0a635c11ffc088cd77df6463249ffebe54b4fd5bf2242cf64c98b2`.
The actual six CDC objects and 12 strong transport code definitions, three
existing RSA policy signatures, ADT pairing, recorded loader lineage,
production parser, LZMA roundtrip, four FAT readbacks and unchanged outer
vendor bytes all passed. Boot arguments are explicitly
`-v serial=3 native-source-storage=1 native-ufs-root=1 tb710fu-cdc=1`.
See `docs/evidence/usb-cdc-image-44088845841.json`.

This stage is implemented, target-compiled, prelinked and unsigned-packaged.
No TB710FU COM enumeration or captured USB bytes are established. Live
firmware role/clock/PHY/SMMU/DMA checks may refuse takeover; the worker requires
BSD startup and scheduling, so earlier output and final panic text are not
guaranteed. Active-DMA reset/disconnect/reconfiguration recovery is unsupported.
The fileset's `loadable=false` and runtime fields remain unqualified. Hardware
boot, UFS reads, policy handoff, root mount and macOS userspace remain separate
gates. No private key was read, no signing or device operation occurred, and
no firmware was rebuilt. The user's working-port condition for a signed image
is not yet met.

Entries below retain the stage and limitations known at their historical time.

## Native USB CDC transport implemented; target validation pending · 2026-11-17

The independent DWC3 core and XNU polling adapter are now implemented. Patch
0012 starts the opt-in worker before vfsinit; runtime requires `tb710fu-cdc=1`.
EP0 enumeration, line coding/DTR/SerialState, log IN, discard-only OUT, normal
SETUP-only bus reset and completion-before-consume are covered by the actual
production core host fixture with separate CPU/device DMA views. Protocol,
RAM ring/tap, hardware predicates and core tests passed ASan/UBSan locally.
The adapter checks live clocks/PHY controls, device role and a unique identity
SID40 route, then validates permanent contiguous DMA and cache geometry.
Source/hash provenance is in `docs/evidence/usb-cdc-sources.json`.

The device recipe now selects all six real sources. A separate CI gate will
check actual BSD compile/import, final strong definitions and force-load input.
The supported macOS native/prelink target has not yet validated these new inputs.
Run 44084514457 at `fbdd30ab98d16115c16ac7319b861e05af456623` passed the
macOS host tests but stopped at patch 0012 application, before any compiler
invocation. Its leading context was wrong. The corrected patch is actually
applied in an independent Git fixture with verified changed bytes and startup
order; see `docs/evidence/usb-cdc-startup-patch-check.json`. The initial nested
fixture had silently skipped patch paths, so its exit code was insufficient.
Run 44085996576 at `6f4f1ddd7aa3346be94183c3592c888874cadec9` then applied
the complete series and compiled/checked the five portable CDC objects. It
stopped compiling `CdcXnu.c`: BSD `sys/param.h`/`sys/systm.h` conflicted with
the replayed osfmk/MACH_KERNEL_PRIVATE declarations. The adapter now uses
the same `kern/misc_protos.h` declaration boundary as osfmk/startup.c.
The earlier bootstrap undefined providers are expected before archives;
the actual failing stage was archive-06/CdcXnu compilation. Neither final
native integration nor prelink passed in this attempt.
Queued run 44088500248 at `81a72b61c687845ee9980ea056e54eced075e9f0`
was cancelled before runner steps when a local check of a real Darwin archive
found that the new CDC verifier counted `__.SYMDEF SORTED` as an extra object.
The verifier now filters that linker metadata while checking every real object.
No actual TB710FU CDC enumeration or captured USB log is established. Unsupported
active-DMA reset/disconnect stops the session and retains DMA. The scheduled
worker does not promise final panic text after scheduling stops. No signing or
device operation occurred; the older unsigned image below contains no new
transport or enabling boot argument.

## Integrated UFS fileset and unsigned payload verified · 2026-11-17

Run [44075573981](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44075573981)
at device `9f01899957060ffbd81a49c793438ba152760eb9` passed native compilation,
the actual storage integration verifier, strict prelink and the production
DarwinBoot parser. Artifact `14324417743` is 38,354,358 B; its independently
downloaded ZIP SHA-256 is
`df2802dc432b0290e6b8db0c2a1ef27db8fc6469de2b9fb08351e9fd1d9b8016`.

The integrated native input SHA-256 is
`7a1bba0855ca9ea8d9ddc3e3f937209d5e874b1082415c24ac981c26e0231a45`.
The prelinked MH_FILESET is 142,802,944 B, SHA-256
`9e2be0f788f84fd6c582449afaace7784085e460aff5e959a8cfc59d43b886d3`, with
79 executable fileset entries, 86 closure components, no missing references
and no stubs. Its actual storage archive is 1,444,104 B, SHA-256
`e0da7e494ccd11908c3c861ec4a15765e977c45345d02465e3e89dc47142fc80`.
These are new integrated-run hashes, distinct from the earlier standalone run.
The compile report's `linked_into_kernel=false` describes archive creation;
the separate bound integration report proves its subsequent actual link.

Local checks independently matched all eight archive members, arm64e object
types, external late constructors, absence of eager registration/boot-observer
imports, bound report hashes and a single strong startup definition inside
the prelinked native image. CI also checked the actual BSD compile record,
strong BSD reference and saved archive force-load input. The BSD object and
compile record are hashed in CI but not separately retained in this artifact.
See `docs/evidence/ufs-integrated-44075573981.json`.
The new late NativeUfs object passes the fixed exact-profile static ABI audit:
326 PAC slots, 26 named APIs, class size `0x90` and four consumer dispatches.
The retained run 43844823907 ABI-reference fileset is unchanged. See
`docs/evidence/storage-abi-44075573981.json` and its exact provenance file.

The device-owned packager at `abc8503cb68004b4fe14f687a084a97445aab2b6`
then successfully produced
`out/source-storage-44075573981/vendor_boot.unsigned.img`, 37,859,328 B,
SHA-256 `32ed17f15fcf235183f141549b3ef07dc0b1fe4768a10faba8c5c3b197c86807`.
It uses the exact downloaded fileset, three-policy bundle, ADT-correct baseline
and recorded existing LZMA/handoff loader. All twelve input hashes, three real
RSA-3072 signatures, production parser, LZMA roundtrip, FAT geometry/four-file
readback and unchanged bytes outside the vendor ramdisk passed. The sole ADT
addition is zero16 `RootPolicies`; existing identity, UUIDs and seed are preserved.
Boot arguments are explicitly
`-v serial=3 native-source-storage=1 native-ufs-root=1`.
See `docs/evidence/source-storage-image-44075573981.json` and
`docs/SOURCE_STORAGE_PACKAGING.md` for exact inputs and limits.

This is an unsigned candidate, not a newly signed flashable pair. No private key
was read, no signing or device operation occurred, and no new firmware was built.
The built source is XNU `12377.121.6`; Apple's original `26C5035r` KernelCache
uses `13432.60.57`. Firmware policy handoff, UFS DMA takeover/reads, root mount,
hardware boot and userspace remain unverified. USB has a target-compiled RAM
log tap, but still lacks the DWC3 transport/DMA/drain implementation and actual
CDC enumeration. Next implement and target-build that USB transport; hardware
storage and USB acceptance require their own device evidence. The user's
conditional request for a signed image after a working port is not yet met.

Entries below retain the stage and limitations known at their historical time.

## Complete archive passed; native integration selected · 2026-11-17

Run [44071685590](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44071685590)
at device `958a893b368befc145da5aad0c348889e7b31d6d` passed the complete
standalone source-storage build. Artifact `14323717445` is 8,970,853 B,
SHA-256 `4a59e9babc100c99b3df0b6ed7745605a6bad76c7efd56afb9f9eef30343cb74`.
Its eight-object archive is 1,444,104 B, SHA-256
`3d1842e4113d53da81d6e4ce208a4ec136c2dc95a31c646e04b4a27eb1bd2588`.
The downloaded ZIP, member hashes, external late constructors and absence of
eager class registration/boot-observer imports were independently checked.
The 1,712-byte RootPolicies bundle SHA-256 is
`49da45a7bbc6360f50bb3efabae7dc0842513b701bbb1d297cd3bd333923cb68`.
The late NativeUfs object also passes the same 326-slot/26-method/class-size
and four-consumer static ABI audit. See
`docs/evidence/ufs-bridges-44071685590.json` and
`docs/evidence/storage-abi-44071685590.json`.

The device now selects the reviewed integration inputs for the next bounded
`ufs-integrated` native/prelink build. `CONFIG_XNU_SOURCE_STORAGE=y`, while
`CONFIG_XNU_NATIVE_UFS=n` deliberately keeps automatic personalities absent.
An explicitly force-loaded archive and conditional strong `bsd_init` call
after `vfsinit()` replace that automatic startup route. The unique device
wrapper invokes the original device recipe using `UNIXNU_DEVICE`; it neither
replaces an official script nor fetches source during compilation. When the
archive changes, it invalidates only the verified generated actual LD output,
because the supported extra-archive flag alone lacks an incremental dependency.

The interface gitlinks are initialized before native compilation. A new verifier
will require the BSD hook's compile macro and strong reference, exactly one
final startup definition, the real storage/crypto symbols, archive/member
hashes and the actual saved `linkarchives` input. The successful standalone
run above does not establish these new native-link/prelink gates. Owner-root
audit/trust, Data/RAM, GPU, wireless and boot diagnostics remain off.
No new IMG has been signed, flashed or booted; USB CDC enumeration and the
runtime storage/root mount remain unqualified.

## Transactional UFS target compile and source-storage bridge · 2026-11-17

Historical entry before the complete archive result and integration selection above.

Run [44068063538](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44068063538)
at device commit `bf0be756b550558cf5a4097314c0bd1d703d1a89` completed
successfully. Both patched production host tests and the macOS 27 arm64e
NativeUfs object compile passed. Artifact `14322470471` is 8,902,951 B,
SHA-256 `bff7a9402c9a8726dc4a3b6360c7e04a54d58d8ef7bb7fdacfd90b11e453324b`.
The independently downloaded archive is 595,544 B, SHA-256
`bec56cbe2f42e76a0d162183f861da78d5a6336dcd78f36c645137125e6b561d`;
NativeUfs.o is `5ac860852c2f62b8fd1def8bfc6524d72cc4443dda4b067f9b11b04b88e81adb`.
The separate native-only kernel is 25,171,504 B, SHA-256
`d49c59876a11e3d3f8d46f0828dce6b401dec61bac3bac60931e2aa283ec4343`.
The UFS archive is not linked into that kernel; no prelink or hardware test was
performed. `docs/evidence/ufs-source-44068063538.json` records the exact inputs.

The device repository now supplies `package/SourceStorage.cpp`,
`SourceRootPolicies.cpp` and `SourceRootBinding.cpp`, adapted from exact
`UniXNU-sheng@1108068537d5396edb2c63b2f8c6f5b97b0a4bbc` blobs. The policy
bridge copies and protects the firmware handoff, authenticates all policies
before publishing, and releases its allocation on failure. Real RSA-3072
OS/SystemOS/App headers, a full App manifest and malformed bundle tests pass
on the host. The media bridge binds a unique BSD device and UUID through a
retained provider chain to the real NativeUfs authentication routine.

`scripts/build-tb710fu-source-storage.py` is an explicit device-owned minimal
adaptation of the pinned official source-storage recipe, not a replacement of
the fetched script. It compiles the public deferred base, NativeUfs, all three
bridges, the real VerifiedRoot core/XNU crypto adapter and the independent
public owner anchor into a separate archive. Optional RAM/Data storage,
benchmark, file logging, process diagnostics and wireless providers are omitted
coherently. Patch 0003 disables both declaration and call of the optional boot
observer, so no empty diagnostic/security provider is supplied. The new
`ufs-bridges` workflow stage is the next bounded target compile; its result is
not covered by the successful `bf0be75` run above.

A retained target fileset has been found locally, correcting the earlier
limited search. It records the exact profile KernelCache input
`d1003b5143ffd88439b4d45b6017f587171cfe6855660bed05370a2ea5db9fe9` and
contains IOStorageFamily/APFS. The reproducible static dispatch/PAC audit matches
all 326 slots, 26 block API identities/order, class size `0x90`, and four actual
binary consumer call sites; see `docs/SOURCE_STORAGE_ABI.md`. This establishes
static layout compatibility only. No startup hook is selected, both storage configuration switches
remain off, and the archive/link/prelink/runtime gates stay separate. No new
IMG has been packaged, signed or flashed.

Packaging audit correction: `tools/package_kernel_diagnostic.py` already
accepts the ADT-correct `out/kernel-log-region/vendor_boot.img` baseline,
SHA-256 `15846226236b56bd3abb104ae57f45e23307f3ea52321fd03ed153fd2e4e0718`.
Its absence from the allowlist was an earlier mistaken conclusion. The actual
remaining packaging inputs are an integrated/prelinked UFS kernel, a matching
RootPolicies sidecar and ADT handoff, explicit UFS boot arguments, and verified
loader provenance. Baseline allowlisting is not the blocker.

## UFS integration and image gate audit · 2026-11-17

Historical audit before run 44068063538; the current result is above.

The first board-specific UFS transaction patch is now in
`patches/ufs/0002-transactional-handoff.patch`, based on pinned
`ShengUfs@99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0`. It plans and snapshots
the apps-SMMU and firmware queue before writes, stops the idle request queue,
configures the stream transactionally, then installs and starts the new request
list. Failed configuration restores the saved SMMU state and original queue;
failed replacement startup first proves DMA is stopped before rollback. W1C
interrupt status is cleared only after the replacement queue is ready. An
unverifiable rollback panics while retaining DMA ownership. The patch also
checks firmware UFS power/clock state and refuses Sheng-only Wi-Fi/DPU SMMU
boot arguments on this board. It also refuses the legacy firmware-list
adoption probes, which zeroed the firmware UTRL before proving it idle.

Both device patches apply in order to a clean worktree at the pinned source
revision. The SMMU and production queue/command fault-injection tests pass with
WSL Debian `g++ 14.2.0` and `clang++ 19.1.7`
(`-Wall -Wextra -Werror`). Queue snapshot validation includes HCE/HCS ready
bits, interrupt mask, RUN_STOP and the original UTRL base. SMMUv2 sCR0 bit 0 is
CLIENTPD and must be clear; bit 1 is global fault reporting, not an enable bit.
The adapter requires IDR0.SMS and refuses EXIDENABLE because this path uses
SMR.VALID rather than extended-ID S2CR validity. A stale SMMU plan after queue
stop forbids resuming firmware DMA. Rollback rechecks the whole plan, including
stream aliases and context-bank ownership. This is host-only validation; the patched kernel object has not yet
been compiled on the pinned macOS runner or exercised on TB710FU hardware.

The official pinned arm64e `ShengUfs` recipe routes to
`build-sheng-source-storage.py`, which also requires device-owned
`SourceStorage.cpp`, `SourceRootPolicies.cpp`, and `SourceRootBinding.cpp`,
plus a separate macOS 27 IOStorageFamily/APFS ABI audit. The fixed DarwinBoot
payload path can carry the signed `RootPolicies` envelope and make its physical
range available under `/chosen/memory-map`; the target profile notes that this
handoff itself has not yet been exercised. The bridge inputs remain absent
from the clean device repository. The existing adapter workflow only compiles
one `NativeUfs.o` archive and explicitly reports
`linked_into_kernel=false`, `root_policy_ready=false`, and
`runtime_qualified=false`; the board keeps both storage config switches off.

No integrated kernel has been built or prelinked, so there is no new UFS kernel
payload to package. The existing generic kernel packager accepts the
ADT-correct `vendor_boot` baseline but does not provide a UFS root-argument or
RootPolicies sidecar input. The existing signed UEFI-UFS boot image is a
different artifact and does not contain the NativeUfs XNU driver. No new image
was packaged or signed. Next: run the bounded `ufs-source` workflow for the
macOS 27 object compile. Then implement the real source-storage bridges and
audit the IOStorageFamily/APFS binary ABI against the target macOS 27 kernel
before enabling the component, building a fresh integrated kernel and prelink,
or preparing a matching signed image pair.

The device adapter exports the exact pinned commit, applies both patches in a
temporary tree and runs the host tests there before the object compile. This
avoids using non-existent test paths in the device repository and ignores local
source-cache edits and line-ending differences. CI collects the host logs and
reports even if the adapter step fails.

## Official record and pinned UFS source review · 2026-11-17

Updated the existing root `porting.toml` storage area while preserving its
other device records. Storage remains `planned`; its findings and evidence now
record the standalone source archive without implying kernel integration or
hardware qualification. The pinned validator passed for all 15 areas:
`python3 .work/UniXNU/scripts/porting-record.py check --device .`.

Reviewed `ShengUfs@99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0`, the exact source
revision used by successful run
[44041029817](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44041029817).
The build report's upstream `NativeUfs.cpp` SHA-256 is
`8e99199aa87dc2ccebfe3366a0f6896fc8937d57670117af66c10ee52b1b28cd`; the
SM8650 compatible patch SHA-256 is
`8697b355a151c3cdb4aae8b595c58c05f760b4c3ddd6ce458ac5c8a1571b7993`. The
controller, GCC, and UFS clock register addresses align with the current board
inventory. The driver still assumes firmware initialized the controller/link,
requires an idle request queue, maps contiguous DMA through SID `0x60`, and does
not implement qualified reset, clock, or PHY initialization. The pinned
same-device Linux source at `4d04410798b538fbe4b50664e3a66d9500a2ed31` supplies
the static declarations: IRQ SPI 265, active-low reset GPIO 210, storage/PHY
rails, clock/GDSC/reset topology, SID `0x60`, two lanes, and the SM8650 QMP PHY
table. Linux cold-start resets and calibrates the PHY before selecting UniPro;
ShengUfs currently adopts the firmware-running link. Firmware's live clock,
reset, supply, link, queue, and SMMU states remain unknown. No kernel run was
dispatched because this source crosswalk changes no kernel inputs. The separate
takeover-order and integration audit is recorded above.

## USB/UFS adapter build prepared · 2026-11-17

The USB debug path now has a device-owned 64 KiB RAM tap. XNU `_cnputc` records
the console after newline normalization, and the direct `PE_kputc` serial
paths append without touching MMIO. The XNU serial callback under `_cnputc`
is not tapped separately, avoiding duplicate characters. A later worker can
drain the ring. Host tests cover the tap, but this is not a USB transport or
host-visible COM port: DWC3 endpoint/TRB handling, SID `0x40` DMA ownership,
startup and enumeration remain unimplemented. The official
`QcomDwc3` component is Apple XDCI/XHCI stack integration, not CDC-ACM, and its
PHY headers have a separate GPL-2.0-only license.

The `ufs-source` workflow runs the device-owned builder after the minimal
native kernel, using that build's XNU records and pinned framework interfaces.
Run [44027734178](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44027734178)
exposed stale XNU patch 0010 context after patches 0001-0009. The patch was
corrected and locally applied against the saved post-0009 snapshots in commit
`fa7b62f`.

After the user made the repository public, run
[44041029817](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44041029817)
completed and uploaded artifact `tb710fu-ufs-source-44041029817` (8,620,413
bytes, SHA-256
`61bb0852cc4d14a679ee2fd3bf88626376db2d16eeac535007b568a4990ab575`). The
artifact was downloaded to `.work/ufs-source-44041029817/`; the ZIP digest,
kernel record, and UFS archive digest all match their reported hashes.

The native-only kernel is 25,171,504 bytes with SHA-256
`0bb0d07e118ea4261613536b185a4dd80aaf90b97df75653c634c2e9bc54f948`.
Prelink was skipped and hardware boot was not verified. The standalone
`libTb710fuNativeUfs.a` is 541,448 bytes with SHA-256
`56a28627693c848f450614f293f3661f1632112ca1801cf2bdc004f341768c3c`; it uses
ShengUfs revision `99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0` and XNU compile
record SHA-256 `5b0cb4d43b31292ed6db2e73ea0cc8ea0c99a5dc9bc064f16b60797ee2e29d64`.
Its report marks `linked_into_kernel=false`, `root_policy_ready=false`, and
`runtime_qualified=false`. This is a successful source build, not a linked or
hardware-tested UFS driver.

The 14.1 GB userdata artifact was deleted after explicit authorization; its
byte-identical local ZIP remains. Runs 44038743847 and its second attempt had
failed before runner assignment. The current repository is public. The full
source-storage route is still not connected:
the board disables `CONFIG_XNU_SOURCE_STORAGE`, and the clean device repository
lacks the `SourceStorage.cpp`, `SourceRootPolicies.cpp`, and
`SourceRootBinding.cpp` bridge files required by that recipe.

## Owner policies and userdata `.vroot` rebuilt · 2026-11-17

Using the user-provided run `43724852936` artifact, OS, SystemOS and App were
extracted and matched to the pinned macOS 27.3 profile. The VerifiedRoot C host
verifier authenticated the RSA-3072/SHA-256 signatures and every 4 KiB block:
OS 3,538,432; SystemOS 712,192; App 13,312. All three policies share owner-key
SHA-256 `c2d8d462d0a790056bef2dddc30ae60ce210dbf0ee1cd4350a21e864db3a7d7e`.

The rebuilt `SHNGROOT v2` image contains the original windows plus raw
`OS.vroot`, `SystemOS.vroot` and `App.vroot` hash tables. The final 1 MiB object
padding, manifest checksum, all six row hashes, and zero-filled gaps passed a
full read-back. Image size is 17,603,493,888 B; SHA-256 is
`35b1c581c7074c626b4b3750af93901a988425e42671f1aaeafd7fd6e13de199`. Public
signed headers and trusted keys are in `scripts/policies/26C5035r/`; the private
key stays in the ignored local build tree with a restricted Windows ACL. This
is host-only validation: no flash, boot, or root mount has been tested.

## USB/UFS driver review · 2026-11-17

The official [UFS porting guide](https://unixnu.com/zh/porting/ufs) does not
require a separate register-probe build. It orders static hardware inventory,
implementation/adaptation and build integration first; the real driver then
checks controller, firmware, SMMU, DMA and queue ownership as part of
initialization. [`docs/UFS_PORT.md`](docs/UFS_PORT.md) records the known TB710FU
resources and remaining source-review gaps. The 4 KiB LBA 1 TWRP sample is a
later acceptance baseline, not a prerequisite for beginning the adapter.

No standalone XNU USB/UFS register-snapshot hook is selected. Host CDC/UFS
tests, `python3 tools/ci_kernel.py configure`, and the structured porting-record
check pass. There is still no DWC3 endpoint transport, DMA/SMMU ownership path,
CDC enumeration or XNU log hook.

There is no TB710FU XNU UFS build or hardware read. The pinned
`ShengUfs@99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0` contains real queue,
SMMU and command code, but its catalog metadata targets SM8550/iOS 17. The
macOS profile already supplies IPSW build `26C5035r` and the OS/SystemOS/App
image hashes and UUIDs; the official kernel context merges them and exports
`UNIXNU_IPSW_BUILD`. A direct component-script invocation omits that context.
The owner policies and matching `.vroot` userdata tables are now host-verified;
`docs/USERDATA_BUILD.md` records their hashes. The public-source storage route
still requires IOStorageFamily/APFS ABI inputs and remains unbuilt and
unqualified upstream, so it is not a drop-in macOS 27 driver. No standalone UFS
probe is required before adaptation.

## Mainline integration and Meizu21 audit · 2026-11-17

At the time of this earlier mainline audit, local `main` included the diagnostic
bring-up path, the Ace 5 audit, and the CDC protocol/log-ring host slice from
`1c6217ba46033419fd35f576f47b67ddc7ed9b98`. It was eleven commits ahead of
`origin/main` and had not been pushed. Those commits do not claim that the root
volume or macOS userspace boots.

The user-supplied [Meizu21 repository](https://github.com/XiaoChen8371/unixnu-meizu21)
targets SM8650 but is a Darwin 22 transition: `board.toml` selects only
`DarwinBoot`, the kernel provider is retained iOS 16.5/XNU, and
`evidence/kernel-snapshot.json` says `kernel_rebuilt=false`. Its
`meizu_ufs=1 root_apfs=ufs_super` arguments feed that retained Apple path; the
repository contains no native UniXNU UFS, DWC3, EUD, or CDC driver to transplant.
Its transition/rollback and protected-`super` workflow is useful process
reference, but its memory map, 16 KiB pages, ADT, and board addresses are not
TB710FU inputs.

## Ace 5 SM8650 USB/UFS reference audit · 2026-11-17

The [Ace 5 repository](https://github.com/tycykp/UniXNU-OnePlus-Ace5) is a much
closer reference than Meizu21. Its dated hardware record reports a real
Darwin 22 / XNU 8796.141.3 `fastboot boot`, USB CDC on Windows `COM21` (8,802
status bytes, zero drops), and an 8 KiB UFS read matching Android's CRC. A later
device record reports a read-write HFSX `iOSRoot` mount and launchd/system
service progress; SpringBoard/backboardd repeatedly crashed and no GUI appeared.
This is native XNU hardware integration, not an OEM kernel retained behind the
UniXNU menu.

Its CDC/UFS work derives from a pinned SM8550 Vermeer integration adapted for
Ace 5 SM8650. It is the best source for TB710FU USB/UFS bring-up, but controller
role, PHY, clocks, SMMU stream, memory layout, and XNU 8796 interfaces need
separate comparison with TB710FU/XNU 12377. The Ace 5 `board.toml` and
`hardware/sm8650-ufs.json` retain stale statements that no storage driver/DMA
was validated; the later run-bound read and root-mount records establish the
narrower tested behavior and should be cited with their image/kernel hashes.

## CDC protocol and log-ring host slice · 2026-11-17

Commit `1c6217ba46033419fd35f576f47b67ddc7ed9b98` adds the local device kit,
USB 2 CDC-ACM descriptor/request validation and a nonblocking bounded log ring.
The implementation is independent of the Vermeer source; test-only VID/PID
values are not device identifiers. The WSL host test
`CC=cc python scripts/check_usb_cdc_protocol.py` passed with warnings-as-errors
and AddressSanitizer/UndefinedBehaviorSanitizer. It covers descriptor layout,
malformed requests, ring wrap, full-buffer behavior and writer-lock contention.

The pinned UniXNU resolver also configured the `tb710fu_macos_defconfig` and
resolved the device-local package kit. This was a configuration check only;
there has been no new XNU native build for this portable source slice because
it is not yet linked into the kernel. No DWC3 register access, XNU log hook,
USB enumeration or UFSHCI/SMMU callback is implemented. TWRP FDT resource and
hash facts, the read-only UFS GPT sample and the host-only guard/state-machine
slices are in
[`USB_CDC_PORT.md`](docs/USB_CDC_PORT.md).

## Read-only UFS GPT sample · 2026-11-17

While the device was in TWRP, ADB confirmed `/dev/block/by-name/userdata` maps
to `/dev/block/sda14`; `/dev/block/sda` reports 4096-byte logical blocks. One
read-only 4096-byte block at user-LUN LBA 1 had the `EFI PART` signature and a
valid 92-byte GPT header CRC (`0d00f8ae`). CRC-32/ISO-HDLC over the full block
is `44b9c49a`; SHA-256 is
`03f7bbae3cd21bdce06c3f21bf24254c4e297629be14533993c0ab5d46976617`.

The raw block and device serial were not retained. This is a Linux/TWRP storage
baseline for a future read-only XNU probe, not evidence that XNU can access UFS;
queue ownership, SMMU and DMA remain untested. Full capture metadata is in
[`ufs-gpt-lba1-twrp.json`](docs/evidence/ufs-gpt-lba1-twrp.json).

## Host-only USB/UFS probe contracts · 2026-11-17

Commit `919b4bd` adds a CDC transport preflight guard and a one-block UFS
READ(10)/GPT sample state machine. Both are device-owned C slices; no
unlicensed Vermeer implementation was copied. WSL host tests passed with
AddressSanitizer/UndefinedBehaviorSanitizer:

- `CC=cc python scripts/check_usb_cdc_protocol.py` covers USB descriptors,
  setup requests, the nonblocking log ring and the fail-closed DWC3 guard.
- `CC=cc python package/Tb710fuUfsProbe/Test/run_host_tests.py` covers the
  READ(10) command, GPT/CRC logic, default-off behavior, SID/DMA refusal,
  queue restore and timeout quarantine.

These are preflight/transport-independent contracts only. No XNU 12377
hardware reader, DMA allocator, UFSHCI queue/SMMU adapter, DWC3 endpoint engine,
XNU log hook or startup personality is connected. No full XNU build or hardware
read/enumeration was performed. The current branch therefore does not yet
provide a USB port or an XNU UFS driver.

## Guard11 stage56 hardware checkpoint · 2026-11-17

The user supplied `XNU returned: tb710fu_ramoops_init` with
`build src-eff36305-guard11`, `STAGE=0x38` (56), and `ARG1=0x93ff00000`.
This confirms execution returned from the ramoops initialization call after
`arm_vm_init`; it is far beyond the early XNU entry/MMU markers. It does not
yet prove that the persistent zone header and console bytes were written, and
the device partition hash was not read back, so exact image attribution is not
confirmed.

There is still no live USB COM/XNU console for TB710FU. `serial=3` cannot create
a missing UART, and TB710FU EUD/CDC enumeration plus its XNU output driver
remain unverified. The portable CDC protocol layer has no controller or log
hook yet. The current
UEFI QR viewer reads the old `0xac340000` zone; guard11 writes to
`0x93ff00000`, so that viewer cannot yet export guard11's zone. A TWRP raw
memory read of `0x93ff00000..0x93ff40000` is the immediate post-reboot capture
path, subject to `/dev/mem` access and DDR retention. Do not treat stage56 as
proof that this read will contain a valid console until the zone header is
checked.

## Guard11 rebuilt and signed · 2026-11-17

The fileset-header correction is now in the signed diagnostic candidate. XNU
native arm64e build, macOS 27 prelink, and the production DarwinBoot fileset
parser passed in run [43917723572](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43917723572).
The exact 0006 patch SHA-256 is
`7be986c3744b7d9c9ca77f0d6b09e8e20b9ba74e587e527ac26281f9717a1a6c`.
Prelinked kernel SHA-256 is
`eff36305a28a6bf89c453734a07a9fe0e878e9041773864aeb205fc88483c3e3`; the
strict mapping report has `mapping_complete=true`, `missing={}`, and
`stubbed={}`.

The post-link reporter label is `src-eff36305-guard11`, with kernel SHA-256
`5d860a638dde58911cc35d66181f049689535f416c329bcbc27d93987ca8e09c`.
The signed pair is in `out/kernel-ttbr1-guard11/`: `vendor_boot.img` SHA-256
`10c3affb7b051b0b3f2ea1406c7246ee62bf257d128e8afc5db6a36dde63533d` and
`vbmeta.img` SHA-256
`d2f87a085be2319e3382d80678b6e3cccd8318a7b70eee6ea96fc449a406e4d2`.
Independent RSA verification and decoded kernel/ADT/TOML readback passed; the
vbmeta header, key and 22 unrelated descriptors are unchanged.

The exception/wrapper model, stage 47/48 3200x1080 rendering, marker-root
model, OneSend single-frame receive and rendered stage-47 QR decode passed. The
user later supplied a stage-56 report labelled `src-eff36305-guard11`, showing
`tb710fu_ramoops_init` returned at `0x93ff00000`. This confirms progress through
that reported checkpoint; no device partition hash binds it to the exact signed
pair, and the ramoops bytes, root-volume mount and macOS userspace remain
unverified. No device operation was performed by this task. Full provenance is
in [guard11 evidence](docs/evidence/kernel-ttbr1-guard11-build.json).

## Guard10 follow-up: switch returned; reporter mapping is missing · 2026-11-17

The `src-045bfc7c-guard10` screen shows `XNU returned:
vm/set_mmu_ttb_alternate_probed#1`, then `ESR=0x86000007` at
`ELR=FAR=0xfffffff00f82ba10`. This is a current-EL instruction abort with an
L3 translation fault. That PC is `diag_live_text+0x284`, inside `__KDIAG`; the
expected instruction is `CMP X22, X27`. Therefore the marked TTBR1 helper
returned. The following reporter instruction was not mapped by the active
TTBR1 root, so this report does not show a failure in the kernel's switch
helper.

The root cause is in diagnostic patch 0006: `getsegdatafromheader()` searched
`_mh_execute_header`, the embedded `com.apple.kernel` MH_EXECUTE. The post-link
reporter appends `__KDIAG` and `__KDATRW` load commands to the outer MH_FILESET
header, so those lookups returned no segments and silently skipped mapping.
The locally verified input and guard10 candidate both have this nested layout.
Patch 0006 now selects `PE_get_kc_header(KCKindPrimary)` for fileset kernels
and retains `_mh_execute_header` for standalone kernels. It was subsequently
built and signed as guard11; see the next status entry and
[guard11 evidence](docs/evidence/kernel-ttbr1-guard11-build.json).

CDP is not an available plug-and-play kernel log channel established by this
project. The live tree has a Qualcomm EUD node and the firmware contains EUD
serial code, but USB enumeration and an XNU EUD/CDC driver are unverified.
UniXNU's documented USB terminal is the giulia-specific protocol; changing USB
IDs does not add that protocol. See [log channel audit](docs/LOG_CHANNEL.md)
and [official debugging guide](https://unixnu.com/zh/guide/debugging).

The signed guard10 image remains at `out/kernel-ttbr1-guard10/` as the
superseded candidate. Guard11 contains the fileset-header correction. Device
partition hash readback is absent, so exact flashed-image attribution remains
unverified. No device action was performed by this task.

## Marker fault localized; guard10 signed · 2026-11-17

The new device report (`src-045bfc7c-qr8`) captured a real exception inside
`_set_mmu_ttb_alternate_probed`: `ESR=0x96000045` is a same-EL write data abort
with a level-1 translation fault, `FAR=0x93ffff000`, and `ELR=0xfffffff00bb8b374`.
The helper starts at `0xfffffff00bb8b360`; offset `0x14` is its first marker
`STRB`. `SWITCH_GUARD=2` only proved that the reporter/vector BRK self-test
worked. The then-tested guard9 map did not include the marker page, so this was
a diagnostic guard fault before the TTBR1 write, not evidence of an XNU
TTBR1-table failure. The report is consistent with guard9, but the device
partition hash was not read back, so exact flashed-image attribution remains
unverified.

guard10 adds and checks a writable, non-executable TTBR0 identity mapping for
`0x93ffff000`, and carries it through the temporary GOP root. The T1 walker also
now uses APTable bit 61 for EL1 write denial; bit 62 only restricts EL0. The
source is the same verified prelink from run [43844823907](https://github.com/limingerf/UniXNU/actions/runs/43844823907),
SHA-256 `045bfc7cfad9ed2539a586043dd43211263099734f4978abd8092c213b7d7d95`;
XNU was not rebuilt.

The signed pair is in `out/kernel-ttbr1-guard10/`: `vendor_boot.img`
SHA-256 `5e6f8b70369066b1b99ee5b4b69c2616449efc45df6394aaaf6be10365ac7b06`,
`vbmeta.img` SHA-256 `72e4807ec637f94d66195e0321f4e0c76dfb0960ed85fe11fd8f4099910da6df`.
Independent RSA and payload readback checks pass; the original ADT/TOML,
vbmeta header/public key and 22 unrelated descriptors are preserved. The
report label is `src-045bfc7c-guard10`.

The guard10 reporter build, exception/wrapper model, APTable host tests, stage
47/48 3200x1080 render model, marker stores under guard/GOP/restored roots,
OneSend stage-47/48 transfer, and stage-47 rendered QR decode pass. This remains
a diagnostic image: no flash or reboot was performed. The later stage-48
screen confirms the helper returned, but the reporter then faulted because its
segment was absent from the new TTBR1 root; later kernel progress remains
untested. Full machine record:
[guard10 evidence](docs/evidence/kernel-ttbr1-guard10-build.json).

## 内核停点再审计与二维码诊断修正 · 2026-11-16

用户此前看到 `XNU returned: vm/qsort#1`，它只说明 `_qsort` 已返回，不表示 qsort 崩溃。
用户随后提供 QR5 和 QR8 两份阶段 47 OneSend 报告。QR8 标签 `src-045bfc7c-qr8`
与最新版报告器一致；`ARG0=0x8c88fd000` 是新 TTBR1 根表参数，位于 2 GiB 管理窗口内。
报告明确在标记 helper 的 `BL` 前绘制，helper 尚未运行；旧 QR5 的
`marker helper entry` 文字已改为准确的 `before marker helper call`。
两份报告都不是故障 PC，不能证明 TTBR1 已写入或二维码之后内核停止。
用户尚未提供设备分区镜像哈希读回，因此不能确认具体签名包，底层停点仍未确诊。
源码审计确认标记 helper 保留了 XNU 原有 `DSB; MSR TTBR1_EL1; ISB; RET` 顺序；
目前没有可由 QR8 证明的 XNU 功能缺陷，也没有据此修改 TTBR1 切换代码。
此前 `0x8c88f9000` 的软件 walk 是模型根表，不是当前设备 `0x8c88fd000` 的映射读回。

## 单次页表快照与低地址异常捕获候选 · 2026-11-16

用户提供的 TCR `0x00080022a51a251a` 解析为 `T1SZ=26`、`TG1=2`，候选 TTBR1 根从 L1 开始，
使用 4 KiB 页。新 post-link 候选在阶段 47 读取本次 `ARG0` 根，逐级输出 L1/L2/L3 原始描述符，
检查 helper、返回 PC、内核栈、向量、报告器续点、`_set_mmu_ttb` 入口，并扫描全部 reporter RX 与 RW-NX 页。
读取前用 `AT S1E1R` 核对旧 TTBR1 下的物理别名，候选根本身只读。

同一候选为 `__KDIAG`/`__KDATRW` 建立独立的低地址 TTBR0 身份映射，描述符属性来自正在运行的旧 TTBR1 映射，
不依赖待测候选根；检查代码、状态区、栈和向量翻译，并在切换前执行可返回的 `BRK #0xb712` 自检。
阶段 47/48 的 GOP 临时表保留这份低地址映射；
helper 返回后先恢复 XNU VBAR，再恢复原 TTBR0/TCR。任何准备、翻译或自检失败都会恢复旧根并沿原向量路径继续。

输入仍是 run [43844823907](https://github.com/limingerf/UniXNU/actions/runs/43844823907) 的预链接内核，
SHA-256 `045bfc7cfad9ed2539a586043dd43211263099734f4978abd8092c213b7d7d95`；没有重编译 XNU。
guard9 post-link kernel SHA-256 `274917979bb503a18ce24e87ecaf8f5f186583ffb3bbf08cea1e0676f5863a6e`，
LZMA SHA-256 `cb19df3c4945d0bb92fd21c1d47567a16732162963b07401332f2cd98fca27d6`。
阶段 47 报告 591 字节，export-pause 报告 606 字节；阶段 47–56 的 OneSend 单帧接收和重复帧去重均通过。
AArch64 页表模型、4 种异常现场、低地址 BRK/ERET 与 TPIDR 恢复自检、28 个调用包装器、47–56 渲染模型均通过。

**边界：** 这是诊断版内核，不是 XNU 功能修复；设备实际页表结果、实机异常捕获和摄像头扫码仍未验收。
用户随后提供一份阶段 47 页表报告（`ROOT=0x8c8905000`）：helper、XNU 返回点、栈、向量和下一条 `_set_mmu_ttb` 均为 `+`；候选根缺 `__KDIAG` 12 页及 `__KDATRW` 200 页，报告续点 `C` 为无效 L3，分别显示 `X=0/12`、`D=0/200`。这发生在 helper 调用前，不证明 helper 已运行或 XNU 切换失败。
该报告中的 `G=3` 是当时 guard 错误地从待测候选根取 reporter 页造成的，不是硬件根因。guard9 改为从 `O1` 旧 TTBR1 读取 reporter 物理页，再建独立 TTBR0 映射；还把旧映射权限收紧到 RX/RW-NX。guard9 已在 `out/kernel-ttbr1-guard9` 生成并签名，独立 RSA、AVB 和 payload 读回检查通过。
`vendor_boot.img` SHA-256 `f9f185bec93bcb4e01f9da2fe5d489324c98a64a62a6d01cfd06b00e7f0a14f8`；
`vbmeta.img` SHA-256 `a0daf10e828b690bc0aa7162d4c007080113ab30715c81df3adc566f3d8657e3`。
本轮未刷写、重启或改分区。机器记录见
[TTBR1 快照与 guard 证据](docs/evidence/kernel-ttbr1-snapshot-build.json)，设计和字段说明见
[TTBR1 快照文档](docs/KERNEL_TTBR1_SNAPSHOT.md)。

已修正诊断路径：0006 成对映射连续对齐的 RX `__KDIAG` 与 RW-NX `__KDATRW`；
0009 保留标记 helper 为 noinline 符号。run [43844823907](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43844823907)
在提交 `e83bd00d94fc947ecdefd3fafdd471711561dff3` 上完成 native arm64e、严格 prelink
和固定 DarwinBoot 生产解析器。native 输入 SHA `670f2576…`，prelinked kernel
SHA `045bfc7c…`，86 个组件闭包，`mapping_complete=true`、`missing={}`、
`stubbed={}`、`allow_missing=false`。artifact SHA-256
`8063bc02fc5d405cbb7e20524a21e05a844c8937495b69b77e8dedc3597a33cd`。

最终报告器标签为 `src-045bfc7c-qr8`，可见阶段为 47/48、49/50、51/52、54、55/56。
分别覆盖 TTBR1 helper 调用前/返回后、切换后首个调用、`pmap_bootstrap`、
抢占重新开启后的映射完成点，以及 `arm_vm_init` 返回后的 ramoops 入口/返回。
抢占关闭期间不绘制 QR；普通检查点四帧轮播约 2 秒后继续。
九阶段 AArch64 模型、九张画面 ZXing 解码、固定 OneSend 接收器单帧文件还原均通过。

最终候选 `out/kernel-switch-qr8-label-signed/vendor_boot.img` SHA-256
`b5274a747ac23d31f9bb9648738076fe78d12f333ffc9852d9927592562f5280`，匹配
`vbmeta.img` SHA-256 `88dd4989330fad6bc9f0e6edeca38ee9996081bc2691c842ea2412e9c4af991b`。
独立 RSA 复算、payload/ADT/TOML 读回、vbmeta header/公钥和另外 22 个描述符检查通过。
用户已提供 QR8 阶段 47 报告文本（SHA-256 `b141c41e…`），但没有设备镜像哈希读回，
也没有阶段 48 及后续报告；不能确认刷入的 vendor_boot/vbmeta 是否为下方签名候选。
底层故障仍未确诊。
机器证据见 `docs/evidence/kernel-switch-qr-build.json`。
ARM64e per-core PAC 仍待硬件验证。详见
[TTBR1停点修正](docs/KERNEL_SWITCH_QR_FIX.md)和[官方流程对照](docs/UNIXNU_OFFICIAL_AUDIT.md)。

参考项目 `bmk-keyeast/UniXNU-tb710fu` HEAD `23235249d71604f8a6f743680ef8fd1003fe7189`
使用 iPadOS 17.7.11 / XNU 10063.141.1；没有当前 XNU 12377 的 TTBR1 路径。
只借鉴 ramoops 格式测试和分阶段探针经验，不复用其窗口外的 `0xac340000`
日志地址。当前状态入口中的旧 A/B 产物哈希与云 run 历史照常保留。

> 并行交付完成（2026-11-16）：两份可刷镜像均已签出并独立验签，均未上机验收。
>
> **(A) 切换内标记** `out/kernel-switch-markers`（`fafb1f96…`/`b64e33bc…`）。
> 云 run 43819437775，内核 `860834c5…`。0007 在 `set_mmu_ttb_alternate`
> 的 MSR 前后各写一个恒等映射字节（`0x93ffff000` / `+1`），走 TTBR0 V=P，
> 不依赖新页表；机器码逐条核对夹住 MSR。实机屏幕停在
> `XNU returned: vm/qsort#1`、无 QR，正是标记版预期（说明
> `TB710FU_SWITCH_MARKS=1` 生效、探针的 stage 47 那条 BL 未走）。
> 读标记需进 TWRP（Android `CONFIG_STRICT_DEVMEM` 拦 `/dev/mem`），
> `devmem 0x93ffff000 8`：01 00=MSR/ISB 没完成，01 02=切换完成。
> 见 [切换内标记](docs/KERNEL_SWITCH_MARKERS.md)。
>
> **(B) 窗口内日志区** `out/kernel-log-region`（`15846226…`/`e4cffaf3…`）。
> 云 run 43827454902，内核 `4ad08f1b…`。0008 把 ramoops 默认 PA 从不可达的
> `0xac340000`（比 physBase 低 32.3 GiB）改到 span 内 `0x93FF00000`+256KiB，
> 拒绝 span 外 override；机器码确认常量、区域几何 7/7 PASS。镜像同时含 0007。
> 注意 ramoops_init 在 arm_vm_init 返回之后，本次 stage 47 停点仍靠 0007 标记，
> 0008 只为越过切换后的启动提供持久日志。见 [日志区交付](docs/KERNEL_LOG_REGION.md)。
> 未刷写、未重启、未改分区。

> 待验交付（2026-11-16）：**段映射改为不依赖引导参数**。
> 上一版把映射挂在 `tb-diag-segment=` 上，反汇编证实编译器生成的守卫是对的
> （`cbz w0` 跳过整块），所以失败原因是**参数从未到达内核**，映射静默未执行——
> 与设备始终只见 `set_mmu_ttb_alternate#1` 完全吻合。
> 本版改为按段名无条件查找：不存在的段返回 NULL 且 size=0，代码要求两者非零，
> 因此**普通内核零映射**（5 情形矩阵全 PASS）。
> 云 run 43813385537 success（head `fbe75be`），内核 `6c71e39c` 中
> `tb-diag-segment` 字面量已为 **0**、`__KDIAG` 为 1。
> 交付 `out/kernel-diag-unconditional`（`67b19c55…` / `759bd40d…`），
> 引导参数已清理，ADT 身份从成品 payload 直接读出确认为 `Mac17,11`。
> 见 [无参数段映射交付](docs/KERNEL_DIAG_UNCONDITIONAL.md)。

> 上一交付（2026-11-16）：第一份内核自带 0006 的可刷镜像
> `out/kernel-diag-past-switch`（`2c35ccc4…` / `dc03f3b3…`）。云 run 43806716346
> success，内核 `6adc7e15…`，0006 在 `xnu-inputs.json` 第 132/132 项且哈希一致。
> 补丁**确已编入机器码**（`tb-diag-segment` 字符串与 `_arm_vm_prot_init` 对它的
> 引用各 1 处，基线均 0 处；切换点位移 `0x88`）。注意总字节数未变——增长被段内
> 填充吸收，但 7,297,495 字节不同、5,650 个符号位移，不是缓存复用。
> 探针追踪点 25 个，新增 `vm/set_mmu_ttb#1` 位于切换点之后，可把"切换失败"
> 与"切换后失败"彻底分开。签名经独立 RSA 复算 + 生产解析器 PASS。
> 并行线 (b) 已证**不可行**：ADT 窗口任何调整都无法让 ramoops 地址可达
> （在 span 下方 32.3 GiB）。见 [切换后探针交付](docs/KERNEL_DIAG_PAST_SWITCH.md)。

> 历史分析更正：T1SZ 软件遍历使用模型根表 `0x8c88f9000`，输入包括推定的
> `topOfKernelData=0x8c88f8000`；它不是设备表项读回，也不是后来 QR8 的
> `ARG0=0x8c88fd000`。该结果只说明模型路径，不否定当前实际根表可能缺少返回点映射。
> 参数门控版 0006 的 212 页空隙是旧 reporter 布局问题，后续已由无参数版修正。
> QR8 的阶段 47 在 helper 调用前，当前仍无阶段 48 或设备物理标记值；不能把旧模型
> “返回地址可执行”当成硬件切换已通过的证据。
> `patches/xnu/0006`：切换**前**把 `tb-diag-segment=` 指定的段映射为 RX。
> 默认启动零映射（行为矩阵 4/4 PASS，连段查找都不做），不需要改官方构建脚本
> （曾误改 `build-xnu-4k-baseline.sh`，已回退并验证与 HEAD 逐字节一致）。
> 见 [切换后探针](docs/DIAG_PAST_SWITCH.md)。

> 修复件（2026-11-16）：已用修补后的 `board.toml` 重新生成带 macOS 身份的 ADT
> （`Mac17,11` / `K114cAP` / `/product` 35 项 / `/chosen` osenvironment+
> boot-uuid+root-matching），只替换 payload 里的 `.adt`，内核 `9daf5064…` 与
> `unixnu.toml` 逐字节未变；签出 `out/kernel-adt-fix`（`eb09e141…`/`a7472fe5…`）。
> 签名经**独立 RSA 复算**（不调 avbtool）+ 4 例反篡改对照通过。
> **边界**：ADT 在 `arm_init:380` 即被读取，而停点在 `arm_vm_init:586`，
> 所以这是客观缺陷但**未证明是绿条成因**。见 [ADT修复交付](docs/KERNEL_ADT_FIX.md)。

> 上一审计（2026-11-16）：对 out/kernel-pr249-final 逐字节复核，交付本身自洽
> （vendor_boot `92232f89…`、内核 `9daf5064…`、PR249 的 22 页确在机器码中）。
> 但两点由此定量排除/确认：① PR249 与本机绿条卡点无关——2 GiB 窗口只需 8 页，
> 旧 16 页池已够，且实机已越过该检查到 set_mmu_ttb_alternate。② **真正的缺陷是
> 交付的 kernel.macho.adt 不是 macOS 设备树**：根 model 仍是
> `Qualcomm … Pineapple MTP`、`/product` 为空、`/chosen` 缺 osenvironment/
> boot-uuid/root-matching，内核匹配不到任何平台专家。已补 board.toml
> `[payload.adt]` 并新增身份门禁，重新生成 9 项全 PASS；未重编译/未刷机。
> 见 [绿条审计](docs/GREENBAR_AUDIT.md)。

> 上一交付（2026-11-16）：PR249 的16→22页容量修复已适配到本机
> xnu-12377补丁序列。完整native/严格prelink云run43763928457成功，
> 独立核对机器码22页、依赖闭包、生产解析器与artifact哈希通过。
> 原生启动候选已本地签名，out/kernel-pr249-final；无附加诊断段和
> 导出暂停入口。原卡启动尚未证实修复，实机待验。见 [内核交付](docs/KERNEL_PR249.md)。

这是当前工程的单一状态入口。编译、静态加载检查和实机运行分开记录。

| 检查点 | 结果 | 证据 |
| --- | --- | --- |
| macOS arm64e 原生内核编译/链接 | 通过 | 首轮43615291360；补齐真实stack导出后，原生输入SHA-256为57047aaca43bc2d907dab64fd54c0062a16d3f370e8c5c8220b2302e58622589 |
| 官方macOS严格预链接 | 通过 | mapping_complete=true，missing={}，stubbed={}，allow_missing=false |
| 最终fileset字节/结构/闭包 | 通过 | [验证run43620431387](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43620431387)：86个组件、79个可执行成员 |
| 同版本DarwinBoot生产解析器 | 通过 | 同一run对实际142,753,792字节产物执行官方Test/test-kernel-parser.py |
| UEFI输入修复云编译 | 通过 | [run43636909923](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43636909923)：AARCH64 DEBUG CLANGPDB，主机测试及10个必需模块通过 |
| FD本地独立复验 | 通过 | 3,145,728字节、两个真实FV、10个必需模块GUID及14个overlay文件哈希全部匹配 |
| UEFI实体按键修复候选 | 云编译及主机测试通过，实机待验 | Tb710fuKeyInputDxe：SPMI v7状态读取→ConIn；GCC/Clang和ASan/UBSan通过，实机未验 |
| 绿条交付逐字节复核 | 通过 | `92232f89…`/`9daf5064…`/22页机器码；见 [绿条审计](docs/GREENBAR_AUDIT.md) |
| ADT是否为macOS设备树 | **失败（已定根因并修复输入）** | 交付ADT根model仍为Qualcomm MTP、`/product`空、`/chosen`缺macOS键；补`[payload.adt]`后重新生成9项全PASS |
| PR249对绿条卡点的相关性 | 已排除 | 2 GiB窗口仅需8页，旧16页池已够；实机已越过该检查到set_mmu_ttb_alternate |
| 实机启动/PAC/GOP/UFS/用户态 | 未验收 | 当前任务未运行设备命令、刷写或更改分区 |

## 产物

- 预链接内核：验证run的artifact `tb710fu-verified-prelink-43620431387`。
  SHA-256：`c601ca5ceecfd1536f05634aa71ef700440d167cd2a4dbcd0a06e3a45400e131`。
- UEFI按键修复候选：run43636909923的artifact `tb710fu-uefi-43636909923`，文件 `SILICIUM_UEFI.fd`。
  SHA-256：`4fc321fb77ebad6f63970ae617228eef6be3bf98d98af5cfd4497ea51cd85b9b`。
- 私有Actions产物当前保留三天；源码、固定版本与哈希记录长期入库。
- 本机已保存首轮native、FD与报告至 `.local/`；完整预链接内核交付在上述私有artifact。

机器可读记录：`docs/evidence/native-build.json`、`prelinked-build.json`、`uefi-build.json`、`uefi-input-build.json`。先前UEFI run43618658596及其FD哈希保留于uefi-build.json，不覆盖历史证据。

## 历史红色run的原因

第一轮严格预链接发现真实缺失的 `unixnu_kernel_stack_shift`，已采用官方sheng的真实
栈/guard-page对齐实现修正。随后43618288536成功完成原生构建和官方预链接，但当时
包装检查器把合法 `MH_FILESET=12` 错判为 `MH_EXECUTE=2`，该历史run因此仍为红色。

检查器现已区分根fileset与嵌入原生内核，以及无代码组件和可执行成员。最终验证run
复用同一个产物，核验实际字节、严格报告、map冲突与同版本生产加载器；未为修检查器
重复编译内核，未启用allow-missing或加入桩函数。主构建workflow已接入正确检查及生产解析器。

## UEFI范围

固定官方base加版本化overlay，保留40项参考内存区域；移除旧外机RAW/DXE依赖，
修正SimpleFb查找、边界、清理和错误返回。模型索引0符合固定host工具的整数契约，
当前native板型路径不读取该宏。UEFI开发策略与Android ABL/AVB为不同层。

高位DDR来自同型号参考作者记录；当前设备live FDT、实际scanout stride/格式和所有权
仍需证据。源码检查和编译通过不构成物理内存可用或显示正常的证明。

## UEFI按键修复 · 2026-11-15

用户报告构建的UEFI实体按键无响应；此前overlay没有读取PMIC三键的输入提供者。
已在独立板级overlay加入Tb710fuKeyInputDxe，逐文件来源SHA-256见
[输入源码记录](docs/evidence/uefi-input-sources.json)，设计和验证边界见
[输入审查](docs/UEFI_INPUT.md)。音量加/减映射上/下，电源映射Enter。
新增驱动同时进入DSC/FDF，CI在编译前跑主机模型并核对真实压缩FV中的第10个必需模块。
本轮未访问设备；源码与主机验证不能证明实际按键恢复。云run43636909923及FD哈希已记录于`docs/evidence/uefi-input-build.json`。

## UFS 启用候选 · 2026-11-15

macOS 27 内核 137 MiB 无法放进 `vendor_boot` 的 81 MiB ramdisk 可用空间，
且本机 ABL 不支持 `fastboot boot`（实测 `FAILED (remote: 'unknown command')`）。
因此把内核改放到 `unixnu` 分区，由 UEFI 的 UFS 栈读取。

源码级确认（`Config.c:1547-1563`、`Config.c:1132`）：UniXNU 遍历所有已发布的
SimpleFileSystem 卷寻找 `unixnu.toml`，菜单项路径绑定配置所在卷，所以内核与
配置同卷即可。详见 [内核从UFS加载](docs/KERNEL_FROM_UFS.md)。

新增 `TB710FU_ENABLE_UFS`（默认 0）。为 1 时加入 8 个 stock Qualcomm DXE
（SmemDxe/HWIODxe/CmdDbDxe/PwrUtilsDxe/NpaDxe/RpmhDxe/ClockDxe/UFSDxe），
明确排除 2026-11-11 黑屏集（DALSYSDxe/VcsDxe/DisplayDxe/GpiDxe/SPIDxe/
I2CDxe/SdccDxe/PIL/Tz）。`check-board.py` 双向校验白名单与排除集，
默认构建经 `strip_ufs_gate` 剥离后仍报 PASS（40 区、10 模块、0 vendor 引用）。

云构建 run43692304879 success，FD `dfca596a…`，FVMAIN 由 51 增至 60 个 FFS
文件，8 个 UFS 模块在真实嵌套 FFS 中逐一核对 GUID。boot.img 与 vbmeta 已用
testkey 签名，vbmeta 描述符结构（3 hash + 4 hashtree + 3 chain + 13 prop）
与原厂一致。产物与判据见 [UFS构建记录](docs/UFS_BUILD.md)。

含内核的 `unixnu` 分区 FAT 镜像已预构建：224 MiB，内核读回 SHA-256
`c601ca5c…` 与输入一致，剩余 91 MiB。

**未验收**：UFS 块设备是否实际发布、`unixnu` 分区切分、内核能否从该卷启动，
均需实机证据。vendor DXE 来自 OnePlus Ace 5（giulia）替代集，TB710FU 自身
`uefi_a` 卷未能提取（FV 非可遍历 FFS 布局），板级 PHY/时钟存在风险。

## LZMA 压缩内核候选 · 2026-11-15

UFS 分区路线需要先切分区、再验 UFS 驱动。另一条更直接的路：把 137 MiB 内核
LZMA 压成 23 MiB，仍走 vendor_boot 的 payload ramdisk，由 DarwinBoot 解压后
交给 XNU。这是那台已上板 macOS 27 设备用的办法（用户提供线索）。

- 实测：`kernel-ufs.lzma` 24,073,889 B（8 MiB 字典），往返 SHA-256 与
  `c601ca5c…` 一致；大小字段已回填（`xz --format=lzma` 会写 0xFFFFFFFF，
  EDK II 需要真实值）。payload 25 MiB，余量 58 MiB。见
  [LZMA 内核](docs/LZMA_KERNEL.md)。
- 解码器：`MdeModulePkg LzmaCustomDecompressLib`（NULL 库，scratch 固定
  64 KiB）。**不能用 `EFI_DECOMPRESS_PROTOCOL`**——DXE core 用
  UefiDecompressLib（Tiano/EFI 压缩）实现它，不是 LZMA。
- `firmware/patches/0002-darwinboot-lzma-kernel.patch` 让 DarwinBoot 在
  `ReadFile` 后探测 0x5D 头，命中则 `LzmaUefiDecompress` 展开后交给
  `ParseKernel`；普通 Mach-O 不受影响。
- `tools/ci_firmware.py --lzma` + `.github/workflows/uefi-lzma.yml` 在云端
  构建该变体，并在 FD 里核对 DarwinBoot GUID。

**实机结果（2026-11-15）**：XNU 已在内核里执行，EL1。屏幕显示绿色交接横带、
`Current EL EL1`、非零 `TCR_EL1`、`ID_AA64MMFR0/ISAR1/ISAR2` 读取
（Parange 2、4 KiB granule、PAC 能力位）与 `Darwin Kernel Version 27.3.0`
横幅。整条链 UEFI → UniXNU → payload → LZMA 解压 → ADT → 窗口 → XNU 全通。
证据与阶段边界见 [XNU 实机进入内核](docs/KERNEL_BOOT.md)。

到达该阶段前修复了三个只有实机才暴露的问题：

| 症状 | 根因 | 修复 |
| --- | --- | --- |
| 菜单无 xnu | 25 MiB payload FAT32 仅 50,380 簇 < 65,525 下限，固件拒挂载 | 33 MiB FAT32（66,512 簇）；新增 `tools/check_payload_fat.py` 双向校验 |
| 选 xnu 卡死 `ASSERT [DxeCore] Pool.c(750)` | `AllocatePages` 分配、DarwinBoot 用 `FreePool` 释放 | 改用 `AllocatePool` + `FreePool` |
| 缺 `.adt` 直接失败 | clean board 无 `[payload]` 配置 | 从本机 live FDT 派生 ADT（补单范围 memory/timebase/psci，删 cpu-map） |

**未验证**：平台专家匹配、`/cpus` 与 `/arm-io` 是否被接受、根卷挂载、
用户态/launchd、显示服务、GPU、真实 UFS。

关键修正记录：DarwinBoot 是 UniXNU 的 gitlink，`defconfig` 不初始化子模块，
需 `--lzma` 显式 `git submodule update --init drivers/DarwinBoot`；`NULL|`
库链接只能写在 DSC 组件 override 块，写进 INF 会崩 BaseTools；且
`Binaries` 不能进 `[firmware.submodules]`，否则 build firmware 重复 init 与
materialised 目录冲突。

## 内核日志通道：ramoops · 2026-11-15

内核在 EL1 执行后屏幕无输出（交接后无 UEFI 显示驱动）。为拿到内核日志，
按 macOS 27 的真实补丁基线移植了参考移植的 ramoops console 补丁
（`patches/xnu/0004-tb710fu-ramoops-console.patch`）。

关键：macOS 27 profile 在设备补丁前先应用 55 个上游 xnu-12377 补丁，
补丁必须基于**已应用的树**（`xnu-work` @ `ac9718fb`）生成；基于裸 feed
生成的版本应用失败（实测）。已验证 0001–0004 整个设备序列按顺序干净应用。

云构建：native run 43709117408 success、严格 prelink run 43710067378
success。预链接 `kernel.macho` 142,753,792 B（SHA-256 `cc690c9a…`），
字节内确认含 `tb710fu_ramoops` 等符号。LZMA 压缩 24,054,955 B 往返一致。
启动参数带 `ramoops=0xac340000 ramoops_reset_after=120`。

交付三件套已重建，14 项刷入前审计全 PASS；只需刷 vendor_boot_b
（`72d8577c…`）与 vbmeta_b（`c210d711…`），boot_b（`6bcc95a3…`）未变。
详见 [ramoops 文档](docs/RAMOOPS.md)。

**未验证**：zone 能否跨复位存活、Android 侧 pstore 是否出现、内核实际停点。

补记：TWRP 实测 `pstore` 已挂载、`CONFIG_PSTORE_RAM/CONSOLE=y`、
`/sys/module/ramoops` 与 `/sys/module/pstore` 存在、`backend=ramoops`；
但 `/sys/module/ramoops/parameters/ecc = 0`，而旧 XNU 参数写
`ramoops_ecc=16`，GKI 按 ecc=0 解析带 RS 校验的 zone 会判无效并清零，
这就是首轮 pstore 为空的原因。已改 `ramoops_ecc=0` 重建并推送
（`fa0d3a2`），交付文件 `vendor_boot-ecc0.signed.img`（`836ca997…`）与
`vbmeta-ecc0.img`（`3992de82…`），14 项审计全 PASS。实机复读仍待验证。

## macOS 27 根卷（userdata）构建 · 2026-11-15

按官方 `unixnu build userdata` 落地根卷镜像，云 run **43724852936 success**。

- `board.toml` 补 `[userdata]` 实测 GPT 几何：`first_lba 5105064`、
  `last_lba 61390842`、`lba_bytes 4096`（214.71 GiB 容量上限，
  `sgdisk --print /dev/block/sda` 实测，userdata 为第 14 且最后一个分区）
- profile 自带 `[userdata] windows`（OS/SystemOS/App）与
  `[payload.adt] root_match_volume_uuid = 84581E99-…`
- 远程 IPSW 25.3 GiB，实测支持 HTTP range；只读三个成员约 13 GiB
  （OS 10.62 GiB AEA、SystemOS 2.31 GiB AEA、App 0.05 GiB）
- macOS runner 上 `ipsw 3.1.748` + `/usr/bin/aea` + `hdiutil` 完成 AEA 解密

**产出 `userdata-root.img`：17,466,130,432 B（16.27 GiB，远小于 214 GiB
分区）**，SHA-256 `221d18c76ccd6bcc0ec9141508ef109a254783d029b04e3401993450440ccc7a`，
三个窗口全在，报告校验通过。artifact `tb710fu-macos-userdata-43724852936`
（压缩 13.2 GiB，私有保留 3 天），本机未下载。详见
[根卷构建记录](docs/USERDATA_BUILD.md)。

**未解决**：设备 214.7 GiB userdata 装满 Android data、GPT 无空闲扇区、
data 多层加密；官方保留数据缩容工具会拒绝。用户表示"清空就行"，但擦除/
重分区/刷入**尚未执行且不可逆**。Data 卷 seed 是独立输入，未做。
根卷 UUID 与实际镜像的匹配、根卷挂载、launchd 均未验证。

## 下一阶段

先补本机DT与最小启动/日志输入，再做UEFI→DarwinBoot→单核EL1/PAC验证。真实UFS、
独立UnixNU存储、根策略、Data/cryptex与launchd随后接入；GPU和桌面后置。当前产物是
通过云构建与静态加载器检查的候选，并非已验收的双系统安装包。

## UEFI 只读日志查看器 · 2026-11-15

用户确认当前 A 槽能进 Android 和 TWRP；此前备份不是当前 A 槽状态。
新增 `Tb710fuRamoopsDxe`，在 BDS 前复制 `0xAC340000+0x40000`，
通过官方 Tools 协议提供 ECC 0/16 布局的只读日志翻页。显式框架补丁会在
旧 payload 配置缺少 Tools 时追加入口，保持默认项不变。
GCC/Clang 严格 C11、GCC ASan/UBSan 主机解码测试、40 项内存图和 11 模块预检通过；
云构建与实机结果分别记录，不把源代码/主机测试等同于 DDR 保留或屏显成功。
详见 [UEFI 日志查看器](docs/UEFI_LOG_VIEWER.md)。

本轮从已连接的 TWRP 只读核对 A 槽启动输入，未变更设备。A 槽现有 Linux
内核已带 AARCH64 EFI stub，无需重新编译 Linux；输入哈希见
`docs/evidence/android-oneshot-a-inputs.json`。USB/ADB/fastbootd 与一次性 A 槽引导
尚未实现；UFS 读取、DTB 和 ramdisk 组合是后续前置条件。普通 set-active 不满足下次回 UEFI 的要求。
空 pstore 曾被归因于 ECC 头校验失败；已更正为未证实，ECC=0 时参考实现跳过该校验。

日志查看器云构建 [run43722058056](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43722058056)
成功，实际构建源 `825a8560a237dc2de1426b207a3ed8ca00842338`；artifact
`tb710fu-uefi-lzma-43722058056`，FD 3,145,728 字节，SHA-256
`6a6ff288ddf48d1576cff991fb99b6feb90b4298c442f26eaf3442ee05a60f1e`。
本地独立复验：两个真实 FV、11 个必需模块 GUID、23 个 overlay 文件哈希全部匹配。
机器记录：`docs/evidence/uefi-log-viewer-build.json`。本版是保留 LZMA 加载路径的
可测试 FD，未签名、未刷入；日志查看器实机状态仍未验证。

## 绿条停屏复核与无重编译入口探针 · 2026-11-15

用户报告实际绿条、长按电源强制重启，未经过 Android/TWRP；日志工具原文
`sig 4107024 start1072 size6808`。此头不能作为有效 persistent_ram console 解码，
但无法据此区分未初始化与复位后数据丢失。当前证据不足以归因为根卷缺失。

已用固定官方入口探针生成现有 cc690c9a 预链接内核的诊断副本：蓝色标记
XNU 入口、紫色标记开启 MMU 前。不新增自动复位。仅 129 字节变化，同版生产
解析器通过，LZMA 往返和 payload FAT/读回通过。详见 `docs/XNU_ENTRY_PROBE.md`。
本地 unsigned vendor_boot 已准备；本轮未签名、未刷写，探针未上机。

## 绿条后加载器检查点 · 2026-11-15

针对原有绿条不能区分停点的问题，新增显式 DarwinBoot Entry.S 补丁：
同一条顶部色带依次绿→橙→黄→青→白，分别覆盖进入交接、数据缓存维护完成、
指令缓存维护完成、MMU 关闭后、TLB 维护后且即将跳转。保留 x0/x1，
不改变原缓存/MMU/TLB 操作顺序，未更改内核或根卷。

云 run43729209898 success，构建源 `0ea7095d5d99a8d66b4ff8b77bbdf9268e6cfd52`；
FD `5722252b40e37d6c94e252c8daeddd9f3499b5483bca44f0cba93c795fb78a71`。
本地 11 GUID、22 overlay 哈希、Entry.S 哈希和两个真实 FV 复验通过。
未签名 boot 载体已准备（113e0792…），现有 boot chain 密钥匹配及源载体
签名/hash 验证通过；尚未签名或刷入。见 `docs/HANDOFF_STAGES.md`。


用户已明确授权仅本地签名。检查点 boot 镜像已签出：
`out/handoff-stages/boot.img`，100,663,296 字节，SHA-256
`a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83`。
RSA 签名、内容 hash、FD 读回、原 boot chain 密钥匹配验证通过。
本轮未执行设备命令、刷写或重启，实机结果待用户测试；步骤见
`docs/HANDOFF_STAGES_FLASH.md`。

## 白条后进入核内诊断 · 2026-11-15

用户实测报告顶部白条，尚无核内标记。本轮沿用已有本地签名授权，将固定官方
蓝/紫入口探针副本 e2156f42… 装入 vendor_boot，产出已签名测试对：
`out/kernel-entry-test/vendor_boot.img`（93c81e7d…）及 `vbmeta.img`（58aad870…）。
保留现有已到白条的 boot_b，不涉及根卷。

签名、vendor_boot hash、匹配 vbmeta 引用均通过；vbmeta header、公钥和其余
22 个 raw descriptor 及次序逐字节不变。签名后读回内核并解压哈希正确，
配置/ADT 不变，FAT 几何通过。未新编译 native/prelink，原始内核文件未改，
未执行设备命令。实机入口停点待本次测试反馈，不能称为已修复内核。
步骤见 `docs/KERNEL_ENTRY_TEST.md`，机器记录 `docs/evidence/kernel-entry-test-build.json`。

## 核内蓝/紫已确认，继续 MMU / C 入口诊断

用户报告 out/kernel-entry-test 测试出现白＋蓝＋紫条。核内入口执行与
到达开启 MMU 前检查点已经获得用户实机反馈；设备镜像哈希未读回。
目前仍不能证明 MMU 开启失败或已进入 C 初始化，根卷未成为该停点的已证实原因。

新增青色（SCTLR/ISB 后）、黄色（arm_init KVA 入口）诊断，使用守卫的物理/KVA
代码别名及临时 SCTLR 切换写屏，恢复后继续，未修改页表。357 个变更字节限于
四处跳转和四段空填充区；生产解析器、LZMA、FAT、指令模型检查通过。
指令模型不模拟实际 MMU/缓存/PAC，探针不是永久修复。

已沿用仅本地签名授权签出 out/kernel-mmu-test/vendor_boot.img（1704e2c4…）
和匹配 vbmeta.img（30d6b6ad…）。签名与 hash 引用通过，其他 22 项描述符及
header/公钥不变；未操作设备、未新编 native/prelink。测试说明和边界见
`docs/KERNEL_MMU_TEST.md`，机器记录 `docs/evidence/kernel-mmu-test-build.json`。

## 核内异常文字与 OneSend · 2026-11-16

用户要求不再堆满色条，并希望屏显日志或OneSend。当前用异常快照捕获
ESR/FAR/ELR、原SP/LR和所有通用寄存器，显示关键字段及静态LMS1/OS二维码。
固定微软QrEncodeData原样编译，单帧720字节载荷/752字节帧，三个层级CRC32；
不是完整连续日志，未实现核内五档按键控制。旧五条清除，只覆盖顶部48像素。

诊断入口保留原PACIBSP，安装EL1向量，在异常或panic时用独立物理栈报告；
若到平台初始化前则主动BRK b710，明确标注intentional checkpoint。
追加Reporter self-test菜单，以BRK b711验证报告通道，原默认项/ADT不变。
报告器不返回内核，不是永久修复，不冒充完整内核验收。

out/kernel-report-final/vendor_boot.img（77af03cd…）及vbmeta.img（8edf1237…）
沿用已有仅本地签名授权签出。签名与引用通过，其余22描述符/header/公钥不变。
指令模型、同版生产解析器、变更范围、LZMA/FAT/签名读回通过；独立ZXing将
四类fixture二维码解回完整OS帧，CRC正确。实机报告和扫码待验，未操作设备。

独立诊断源云编译 run43744001120 success（源018a7d7a…），产物及源码身份已复核。
之前43743753427的AARCH64对象编译通过，失败于macOS主机测试的ELF段名；已仅修
host-test分支并证实freestanding对象逐字节不变，不是XNU编译失败。
本地签名镜像仍为77af03cd…/8edf1237…，没有新设备操作。

## 实机文字报告已出现，撤掉默认主动停点 · 2026-11-16

用户照片中的reporter1显示 XNU CHECKPOINT (intentional stop)，关键字段为
ESR=f200b710 / FAR=0 / ELR=fffffff00c28ff68。确认C序言与重定位路径走过，
不是原始故障。图片未见QR，原因未定；仅记录图片哈希和可信转录，未上传图片。

用户要求在实际卡住处报告。本版默认不注入主动BRK，原PE_init_platform及
arm_vm_init调用继续；14个直接调用包装只记进入/返回。早期同步异常/panic
才触发终止式报告，正常其他向量转发原处理器。自检是独立菜单，不作默认。

修正报告器关闭MMU后仍使用KVA向量的源码问题，增设物理终止向量、QR进度行和
二次异常报告，避免编码再异常时静默。此修正尚不等于已确认旧图缺QR原因。
实际AARCH64报告/QR机器码模型运行到READY、独立ZXing解码、捕获/包装/向量模型、
生产解析器和payload/签名校验通过；实机后续页表阶段和纯死循环抓取未验证。

交付 out/kernel-passive-report-final/vendor_boot.img（9e8920b0…）及匹配
vbmeta.img（c75712b8…），沿用仅本地签名授权，未操作设备/根卷。
详见 docs/KERNEL_PASSIVE_REPORT.md 和机器记录 kernel-passive-report-build.json。


独立诊断源云验证 [run43747184259](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43747184259) success，源提交2fd02c6d7f7b5760a40c8397a4625cfa2e984b9c。实际AARCH64对象和主机传输检查通过，未重新编译整个XNU；签名测试对保持9e8920b0…/c75712b8…，实机结果待验。

## 自检确认未对齐写入，修正并增加实时文字 · 2026-11-16

自检图REPORT_PHASE=2，FIRST_ESR=f200b711，第二次ESR=96000061；
ELR=8c8829fc0对应stur w8,[sp,#0x1e]，SP=8c88b1e90，加偏移正好
FAR=8c88b1eae，32位写入未按4字节对齐。报告器关闭MMU时发生该异常。
确认是报告器自身问题，不是原XNU故障。旧模型未检查此条件。

全部freestanding C增加-mstrict-align；旧机器码对齐回归在相同PC失败，
新机器码在自然对齐检查下完成报告与QR READY，独立解码正确。
14调用包装新增实时文字，只覆盖顶部48行：临时独立TTBR0设备映射写GOP，
代码/栈走TTBR1，保持MMU/缓存开启，恢复旧TTBR0/TCR，不改原页表。
指令模型验证描述符、参数/结果/X18和栈保留、只写顶部、原控制恢复。

out/kernel-live-report-final/vendor_boot.img（f8688874…）及vbmeta.img（eb126c61…）
签名/hash/匹配引用通过，原22描述符/header/公钥不变。未操作设备或根卷，
未新做native/prelink；内核原始停点及新版实机行为待反馈。


源编译云检查 [run43749763327](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/43749763327) success，提交7076a0036a87a633cba367292c3cc31c774287d3。AARCH64对象与主机传输检查通过，独立产物匹配；不是新native/prelink。实机QR和实时文字仍待验。


## 自检扫码通过，普通路径增加阶段 QR · 2026-11-16

用户已扫出 reporter 2 自检，ESR=f200b711 / ELR=fffffff00f8280bc。
因此自检的严格对齐修复和 OneSend 路径已有实机结果。普通启动最后报告
函数名 arm_vm_init（没有 enter/returned 前缀），没有 QR；此前普通路径
仅画文字，只有异常才生成 QR，不能把未出 QR 当作编码再次失败。

reporter 3 从进入 arm_vm_init 前开始，正常路径也先生成阶段 QR 再继续；
在其内部加入 10 个已验证直接调用的检查点，覆盖同一显示区，不累积色条。
独立 16 KiB 栈、有界堆重置、实际 GOP 范围的临时映射、TTBR0/TCR 恢复。
外层 arm_vm_init 和最终 set_mmu_ttb_alternate 使用原 LR 尾转移，最终
切换前恢复原 VBAR；保留原 panic 入口，避免页表切换后依赖附加诊断段。
到最后切换标记仅代表观察边界，不能认定该调用卡死。

实际 AARCH64 正常路径 QR 无异常运行通过；3200×2000/640×480 两种
实际绘制像素均被 ZXing 解回精确 752 字节帧。异常路径自然对齐回归、
24 调用/私有栈/原 LR 与 VBAR 模型、401 字节受限修改、同版生产解析器、
LZMA/FAT/签名读回通过。未重新 native/prelink，实机正常 QR 和原卡点待验。

交付 out/kernel-live-qr-final/vendor_boot.img（644cfe58…）与匹配
vbmeta.img（37de2e0f…）；沿用已有仅本地签名授权，其他22描述符/header/
公钥不变，无设备操作。见 docs/KERNEL_LIVE_QR.md 与构建证据
kernel-live-qr-build.json。


正常阶段 QR 的独立源码云检查 run43755493448 success，源提交
67893c542757b1b2e40c97c30903b63bb020b013；四份 AARCH64 ELF 对象与
重复帧主机测试通过。下载 artifact SHA-256 与 GitHub digest 完全一致，
源码提交/QR固定来源匹配。此检查不是重新 native/prelink 或实机验收。


## 实机到 VM 切换边界，固定文件轮播导出 · 2026-11-16

照片及用户补充确认最后阶段 vm/set_mmu_ttb_alternate#1，有静态码但
OneSend 收件不完成；该阶段是既有观察边界，不能认定真实故障位置。
固定 receiver 39e95ce5 的 OpticalReceiver 与 decodeTransferFile 对
旧最终帧独立接收成功；先收到前一阶段时最终会话被忽略。该复现解释设计
缺陷，尚不能认定它是用户此次相机接收失败的全部原因。

reporter 4 只在最终检查点发送同一报告，序号0/1/2/3的合法K=1帧
每500ms轮换；普通XNU项16次后继续，新增明确的 Export VM checkpoint
(animated; intentional pause) 持续轮播。提前缓存位图；编码/等待用
原页表，绘制短暂映射GOP即恢复；968像素M纠错QR置左侧。报告补充原
SCTLR/TCR/TTBR0/TTBR1，原始ARG0来自实际调用。没有后台硬件看门狗。

实际AARCH64普通路径16次轮播后返回、显式暂停路径20次仍持续的模型
通过，实际启动参数解析与映射恢复通过；实际像素被ZXing解回精确OS帧，
实际接收器从任意变体完成与源文本一致的文件。异常自然对齐、24调用
模型、403字节受限变更、生产解析器、LZMA/FAT/签名读回通过。

交付 out/kernel-animated-qr-final/vendor_boot.img（28ac87c3…）与
匹配 vbmeta.img（01982899…），沿用已有仅本地签名授权，其余22描述符/
header/公钥不变。未做设备操作或重新native/prelink；实机接收和原故障
待验。机器记录 docs/evidence/kernel-animated-qr-build.json。


固定报告轮播的独立源码云检查 run43759850376 success，源提交
b166a08f1718e5698a7524a8e6e5af3bda6f7bda。AARCH64对象、主机轮播帧
与未修改的固定OneSend实际收件/文件还原通过；下载artifact SHA-256与
GitHub digest一致，接收源与报告源身份匹配。非native/prelink或实机验收。


## 纳入 PR249 容量修复，回到完整内核构建 · 2026-11-16

用户授权纳入 PR249 并继续修内核。PR 当前仍 open，固定 head
9b9db9c9ff13a595492d241366426383c88bf236；其旧 xnu 补丁的16→22页
改动适配为设备0005，放在现有 xnu-12377 序列之后。没有改官方缓存。
多占24KiB；大型内存34页和其他8页分支不变。实际应用和9GiB边界检查通过。

对旧cc690c9a机器码执行arm_vm_init的有界模型，使用用户bootargs PA
8c88f4000与推定topOfKernelData=8c88f8000，生成root恰为8c88f9000。
切换函数/返回地址/向量可执行、真实SP可写、集合头可读；按新根重新映射
后模型到达pmap_bootstrap。DT/熵/特权指令/PAC有模型限制，不当作实机
切换成功，也未据此加入猜测性的MMU补丁。下一份完整构建不附加__KDIAG
或主动导出暂停；PR249容量修复与原故障修复分开验收。


完整native/prelink云run43763928457 success（源f95c34942e9317e97e8bba819e364e75a6a80b41）。
原生SHA 7b263aeee5f78af52ff6a9286d0a9e5f175212850114ba5696461fa5f537e9cf；
严格预链接SHA 9daf5064924a2a085b1e8ee4e0871df5b9fc86dac516c8d7de9a85484f2ca8ee。
mapping_complete=true、missing={}、stubbed={}、allow_missing=false；生产
DarwinBoot解析通过。实际启动清零机器码为22页，未含__KDIAG，arm_init和
panic原始PACIBSP保留。artifact SHA/digest匹配，LZMA原始流读回一致。

新交付 out/kernel-pr249-final/vendor_boot.img（92232f89…）与匹配
vbmeta.img（89243b22…）沿用已有本地签名授权生成；签名和引用通过，
其他22描述符/header/公钥不变。FAT32 66512簇，原ADT和默认菜单哈希
均未变，没有导出暂停参数或新探针。未操作设备，原故障实机待验。
