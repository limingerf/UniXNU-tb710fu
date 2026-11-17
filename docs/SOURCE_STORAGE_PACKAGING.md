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
`serial=3`, `ramoops_size=0x40000`, `ramoops_ecc=0` or `16`,
`ramoops_panic_delay` from 0 to 300, and `ramoops_reset_after=0`. Omitting the
reset argument uses the reviewed patch's default zero. Duplicate arguments,
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
root boot is included. The next implementation work is the missing DWC3
transport/DMA/drain path; storage handoff, known reads and root mount retain
their separate hardware acceptance gates.
