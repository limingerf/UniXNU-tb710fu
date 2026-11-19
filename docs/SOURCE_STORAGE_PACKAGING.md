# Unsigned source-storage payload packaging

`tools/package_source_storage.py` prepares a new unsigned `vendor_boot` from
the exact reviewed `kernel-log-region` baseline. It is an independent
device-owned tool; the generic diagnostic packager and pinned official scripts
are unchanged. It requires a complete integrated and prelinked kernel before
creating any output directory. A standalone archive, native-only kernel or
older fileset is insufficient.

The tool runs with Python 3.11 or later, `mtools`, `clang`, `openssl` and Git,
normally under WSL Debian. It uses local inputs only. It does not sign, access
a private key, issue device commands, contact GitHub or change a fetched source
checkout. Temporary payload/public-key files are removed at the end. Successful
output consists of `vendor_boot.unsigned.img` and `packaging-report.json`.

## Explicit manifest

Create a JSON manifest with the following structure, then independently hash
its exact bytes. Relative input paths resolve against the manifest directory.
All file hashes must be lowercase SHA-256 strings. Every listed input is
required; unknown manifest/input fields and duplicate JSON keys are rejected.

```json
{
  "schema_version": 1,
  "profile": "macos-27.3-26C5035r",
  "framework": "/mnt/d/unixnu-tb710fu-clean/.work/UniXNU",
  "boot_args": "-v serial=3 native-source-storage=1 native-ufs-root=1",
  "files": {
    "source_vendor": {"path": "vendor_boot.img", "sha256": "REVIEWED_SHA256"},
    "source_vbmeta": {"path": "vbmeta.img", "sha256": "REVIEWED_SHA256"},
    "kernel": {"path": "prelink/kernel/kernel.macho", "sha256": "REVIEWED_SHA256"},
    "kernel_report": {"path": "prelink/kernel/kernel.json", "sha256": "REVIEWED_SHA256"},
    "kernel_map": {"path": "prelink/kernel/kernel.map.json", "sha256": "REVIEWED_SHA256"},
    "build_report": {"path": "prelink/build-result.json", "sha256": "REVIEWED_SHA256"},
    "integration_report": {"path": "prelink/source-storage-integration.json", "sha256": "REVIEWED_SHA256"},
    "source_storage_archive": {"path": "prelink/source-storage/libTb710fuSourceStorage.a", "sha256": "REVIEWED_SHA256"},
    "source_storage_report": {"path": "prelink/source-storage/build-report.json", "sha256": "REVIEWED_SHA256"},
    "root_policies": {"path": "prelink/source-storage/root-policies.bin", "sha256": "REVIEWED_SHA256"},
    "loader_boot": {"path": "handoff/boot.img", "sha256": "REVIEWED_SHA256"},
    "loader_artifact": {"path": "handoff-stages-43729209898.zip", "sha256": "REVIEWED_SHA256"}
  }
}
```

The placeholders deliberately prevent this example from accepting a historical
kernel as integrated. Supply the actual completed build artifacts and reviewed
hashes. Run:

```sh
python3 tools/package_source_storage.py \
  --manifest /absolute/path/inputs.json \
  --manifest-sha256 REVIEWED_MANIFEST_SHA256 \
  --output-dir /absolute/path/new-output-directory
```

An existing output directory is refused. All input, parser, signature and FAT
readback checks finish before the directory is created. A failed validation
does not produce a candidate image.

When `boot_args` includes `tb710fu-cdc=1`, add these three exact inputs to
`files` (15 total), each with its actual `path` and `sha256`:

- `usb_cdc_integration_report`: `usb-cdc-integration.json` from the same build;
- `usb_cdc_archive`: `usb-cdc/libTb710fuCdcLog.a`;
- `usb_cdc_report`: `usb-cdc/build-report.json`.

Partial CDC inputs are refused. The original 12-input form remains valid for
images without the CDC argument. For a CDC-enabled image the tool binds the
prelinked build result to the exact CDC report, then to the archive/report and
the same native input/device revision as storage. It checks all six real
arm64e archive members and actual strong transport code definitions in the
retained `com.apple.kernel` fileset entry. Boolean report claims alone are
insufficient. Runtime, enumeration and hardware-boot fields remain false.

For the CDC candidate with the early QR reporter, add both `diagnostic_kernel`
and `diagnostic_report` (17 inputs total). `kernel` remains the original cloud
prelink. The two new fields name `prepare_early_report.py`'s actual derivative
and build report; each has an explicit SHA-256. Partial diagnostics inputs,
missing CDC evidence and a missing `tb710fu-cdc=1` argument are refused.
The original 12/15-input formats continue to describe kernels without this
post-link reporter; they must not be presented as carrying its QR or markers.

The original prelink/integration gates run unchanged. Then the independent
`verify_reporter_transform.py` checks exact source/report hashes, two new
segment commands, RX/RW-NX geometry, appended payload and a complete bounded
prefix delta. All original bytes outside the exact header/instruction/cave
allowlist remain unchanged. Original BL targets and branch resumes are
validated, and all 13 CDC/storage runtime functions retain their original
strong definitions and full code. The actual final derivative is parsed,
compressed and read back from FAT. `prelink` still records the original CI
input; `packaged_kernel` and `diagnostics` record the actual derivative.
No original CI report is edited to claim that a post-link hash was compiled.

## Kernel and integration proof

The standard official `kernel.json`, symbol map and device `build-result.json`
must describe the exact supplied arm64e MH_FILESET. Mapping must be complete,
with no missing references, stubs or map conflicts, and the pinned KernelCache
input/profile closure/security initializer must match. IOStorageFamily and APFS
must be retained. The pinned production DarwinBoot parser is compiled and run
against the actual file before packaging.

`build-result.json` must additionally state `source_storage_linked=true` and
bind the exact integration report bytes through
`storage_integration_report_sha256`. The integration report must use
`schema_version=1`, the pinned `profile` and `framework_revision`, and an exact
`device_commit`. It must contain:

- `native_kernel_sha256`, matching the official prelink's native input;
- `archive_sha256` and `policy_bundle_sha256`, matching the actual inputs;
- `archive_compile_report_sha256`, binding the exact compile report bytes;
- true `linked_into_kernel`, `source_storage_selected`,
  `startup_hook_selected`, `startup_reference_strong`,
  `startup_symbol_defined` and `late_metaclasses_verified`;
- `archive_force_load_count=1`, false `native_ufs_personalities_selected`,
  false `runtime_qualified` and false `hardware_boot_verified`.

These fields are build evidence produced by the integration workflow, not
manual overrides. The selected configuration is `CONFIG_XNU_SOURCE_STORAGE=y`
with `CONFIG_XNU_NATIVE_UFS=n`; the manual source-storage startup owns service
creation. The archive's actual eight member hashes are checked against its
compile report, including the deferred base/NativeUfs objects, three bridges,
real VerifiedRoot core/crypto adapter and public owner anchor.

## Root arguments and policy pairing

The full root-entry argument string is replaced with `boot_args`; no old
arguments are inherited. Require exactly one `native-source-storage=1` and one
`native-ufs-root=1`. The source references are
`package/SourceStorage.cpp:39`, pinned `ShengUfs@99d8bcd3` NativeUfs.cpp:2140-2150,
and official `UniXNU-sheng@11080685` board.toml:60. The official root path uses
ADT UUID matching and supplies neither `rd=md0` nor a board-specific root-device
number. The tool never invents such a number.

The currently reviewed optional argument set is deliberately small: `-v`,
`serial=3`, `tb710fu-cdc=1` with the evidence above,
`ramoops_size=0x40000`, `ramoops_ecc=0` or `16`,
`ramoops_panic_delay` from 0 to 300, and `ramoops_reset_after=0`. Omitting the
reset argument uses the reviewed patch's default zero. The checkpoint diagnostic
image may also carry the source-recorded `kc-segment-release=0`; other values are
refused. When reusing the rooted signed baseline, the packager still verifies the
exact ADT, kernel, RootPolicies and baseline configuration identity, then replaces
only the XNU entry's `args` field with this validated string; all other menu and
boot fields remain identical. Duplicate arguments,
`rd`/`rootdev`/other root overrides, physical `ramoops` addresses, nonzero
automatic reset, probes, switch/report markers, Sheng hardware arguments and
all other arguments are refused. Further options require a source-backed
extension of this list. This leaves optional hardware arguments unresolved
instead of guessing values. The sample minimal argument string is a packaging
input, not a hardware root-boot qualification.

The bundle must be exactly 1712 B with SHA-256
`49da45a7bbc6360f50bb3efabae7dc0842513b701bbb1d297cd3bd333923cb68`.
Its table must contain three contiguous 544-byte signed OS/SystemOS/App headers.
Each header's UUID, image hash, size, block count, generation and public anchor
metadata must match the exact profile, committed verification record and
archive report. All three real RSA-3072 signatures are rechecked with OpenSSL
using only the public VROOTKEY anchor, fingerprint
`c2d8d462d0a790056bef2dddc30ae60ce210dbf0ee1cd4350a21e864db3a7d7e`.
Full multi-gigabyte root images are not reread by this packager; their retained
verification records are checked, and runtime block authentication remains
mandatory.

The pinned official `devicetree.node()` serializer first round-trips the exact
baseline ADT. The tool adds only one zero-filled 16-byte
`/chosen/memory-map/RootPolicies` property, preserving every original property,
including UUIDs, identity, memory window and random seed. The result is 14264 B,
SHA-256 `61a7ef52d4375de26a4355b9531a29053c2b19f392dab4dd0d4d98f43ca443e7`.
It is paired with `Payloads/kernel.macho.vroot`; pinned DarwinBoot.c:941-954
rejects either half on its own. The actual FAT readback must match all four
expected kernel/config/ADT/policy files, retain its file list except the one
new sidecar, and preserve its mountable FAT geometry and every byte outside
the vendor ramdisk.

## Existing baseline and loader scope

The accepted signed baseline pair is:

| Input | SHA-256 |
|---|---|
| `kernel-log-region/vendor_boot.img` | `15846226236b56bd3abb104ae57f45e23307f3ea52321fd03ed153fd2e4e0718` |
| `kernel-log-region/vbmeta.img` | `e4cffaf3414a2c22d6c129ed62ac382424f67da8a0dd62aa9ae0ccb73fec63de` |

Loader support is limited to the already recorded handoff/LZMA candidate:
run `43729209898`, artifact `14167867119`, device head
`0ea7095d5d99a8d66b4ff8b77bbdf9268e6cfd52`, DarwinBoot revision
`5243c68ec332a227fe3680cd5da6d5a0e127035c`.

| Input or derived member | SHA-256 |
|---|---|
| Original artifact ZIP | `a76530c6d0a27fb9b428f7902817e551fa5927cfe935c0c80a821dc4ae49abfa` |
| Existing signed `handoff-stages/boot.img` | `a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83` |
| Embedded/archive FD | `5722252b40e37d6c94e252c8daeddd9f3499b5483bca44f0cba93c795fb78a71` |
| Unique DarwinBoot PE | `258fc45fcc1e1cd0bb982015e1e59cd0a48ec9cc9ac82c3b90dec78ae2d84079` |

The tool checks the original artifact's checkout/build/GUID records and derives
the PE through the real nested FVs. This is a recorded source/build lineage;
the artifact did not retain patched module source or a per-module source
attestation, and no independent firmware rebuild is claimed. The UFS-only
firmware is not accepted for an LZMA kernel. RootPolicies handoff on hardware,
firmware queue/SMMU state, transactional DMA takeover, known reads, root mount
and userspace all remain separate acceptance stages. The unsigned candidate
does not establish any of those stages.

## Host validation

```sh
python3 tools/test_package_source_storage.py \
  --framework /mnt/d/unixnu-tb710fu-clean/.work/UniXNU \
  --retained-prelink /mnt/d/unixnu-tb710fu-clean/.local/prelink-ramoops \
  --baseline /mnt/d/unixnu-tb710fu-clean/out/kernel-log-region/vendor_boot.img
```

The retained fileset is used solely to test rejection of a structurally valid
kernel whose native input lacks the new integration binding. It is never
reported as containing UFS. The host suite also checks actual policy signatures
and signature mutations, malformed bundle tables, argument conflicts, missing
manifest inputs and the real baseline's ADT/config transformation.

## Recorded positive packaging

Run 44075573981 at `9f01899957060ffbd81a49c793438ba152760eb9` completed the
native integration, strict prelink and production parser gates. The downloaded
artifact `14324417743` was independently checked before packaging. Its new
142,802,944-byte fileset SHA-256 is
`9e2be0f788f84fd6c582449afaace7784085e460aff5e959a8cfc59d43b886d3`.
The new archive also passed the repeated static block-device ABI audit.

The packager source at `abc8503cb68004b4fe14f687a084a97445aab2b6` then passed
the real positive path with twelve actual inputs. Its input manifest SHA-256 is
`c4c9538fce0d354468f8cf8f6af6a0b5bd7fdd88f8e7a9d979bda84017a9d960`.
The local result is
`out/source-storage-44075573981/vendor_boot.unsigned.img`, 37,859,328 B,
SHA-256 `32ed17f15fcf235183f141549b3ef07dc0b1fe4768a10faba8c5c3b197c86807`.
The actual output file was independently rehashed after packaging.
[`evidence/source-storage-image-44075573981.json`](evidence/source-storage-image-44075573981.json)
records exact inputs, tool revision/hash, report digest, loader scope, RSA
checks, four FAT file readbacks and the unsigned output hash. Large artifacts
and raw ADT/seed values remain local and ignored by Git.

The positive host result establishes unsigned assembly only. No new AVB
signature, signed image pair, device operation, USB CDC enumeration or hardware
root boot is included. At this historical packaging point the DWC3 transport
was still absent. Storage handoff, known reads and root mount retain their
separate hardware acceptance gates.

## CDC-enabled positive packaging

Run [44088845841](https://github.com/limingerf/UniXNU-tb710fu/actions/runs/44088845841)
at `c0154c9d5e7048f7896614e1ca8d65be7d4d0e10` subsequently passed the full
six-source CDC target compile, actual integration, strict prelink and parser.
The packager at that same commit passed its positive path with all 15 inputs.
Manifest SHA-256 is
`f32068c96c0a635c11ffc088cd77df6463249ffebe54b4fd5bf2242cf64c98b2`.
The selected arguments are
`-v serial=3 native-source-storage=1 native-ufs-root=1 tb710fu-cdc=1`.

The result is `out/usb-cdc-44088845841/vendor_boot.unsigned.img`, 37,859,328 B,
SHA-256 `49b216089db0cec46772cbba068178fb82e12d86af3ba0c5e925cf0a26ed416c`.
All six actual CDC archive members and 12 strong transport code definitions
passed, alongside the existing signature/parser/loader/LZMA/ADT/FAT gates.
The file was independently rehashed after packaging. Exact hashes and scope
are in [the image record](evidence/usb-cdc-image-44088845841.json).

This is unsigned assembly with the existing recorded loader. Actual TB710FU
CDC enumeration and captured logs remain unverified; live adoption checks may
refuse takeover. The worker requires scheduling, and active-DMA reconnect/reset
recovery is unsupported. No signing, private-key read or device operation was
performed. Hardware storage and root boot remain separate acceptance gates.

## Extended platform diagnostic candidate

Following the user-reported stage 56/no-port observation, tool revision
`608c701ab5690f32bc3a36a571a7cba6fc1984c1` adds a bounded pre-scheduler trace.
It preserves the original 15 inputs and pairs the new diagnostic kernel/report,
manifest SHA-256 `5b91185df389ebec90751bcb24b698a903277ba2999dee8e4d5b63d411dce537`.
The original CI prelink remains 503599a0; the separately recorded derivative is
`23331f1d379f52c155dd4e479f72cf6786342d8d4ae1f87313f2b665a04aef19`.
The validator checks actual caller ranges and direct BL occurrence as well as
all reported prefix changes; 45 trace sites/52 spans preserve all 13 protected
CDC/storage definitions and 7,656 code bytes. The actual derivative passes the
production parser, compression and four FAT readbacks before output.

The 37,859,328-byte unsigned image SHA-256 is
`24132230a6771770d78b6065f0b157992f8f3937d51854ea9c22a571f4761aae`.
The existing separately authorized signer produces a matching vendor_boot/vbmeta
set with independent RSA/hash/payload verification, bundled with the unchanged
loader in `out/tb710fu-usb-cdc-platform-44088845841-signed.zip`.
See [image evidence](evidence/usb-cdc-platform-image-44088845841.json) and
[signed evidence](evidence/usb-cdc-platform-signed-44088845841.json).
No native rebuild, firmware rebuild, device operation or hardware qualification
is added by this diagnostic packaging step.

## Separately authorized local signing

The user subsequently requested a signed CDC candidate for hardware testing.
The unsigned packager remains unchanged and performs no signing. The existing
`tools/sign_kernel_probe_pair.py` then signed the exact verified unsigned
payload using the existing local key and unchanged avbtool. It generated a
matching vendor_boot/vbmeta pair, with the recorded signed handoff/LZMA boot
image copied into `out/usb-cdc-44088845841-signed/`.

Independent RSA and salted image-hash checks pass; the unsigned bytes, four FAT
files, decompressed kernel, all 22 unrelated vbmeta descriptors, complete
header, public key and rollback/flags are preserved. The existing boot's
embedded key matches the top-level boot chain descriptor. The exact delivery
and verification hashes are in
[the signed record](evidence/usb-cdc-signed-44088845841.json).
The signed ZIP includes the three images, checksums and Chinese test notes.
No firmware was rebuilt and no device operation occurred. Signing does not
establish CDC enumeration, USB output, hardware root boot or userspace.
