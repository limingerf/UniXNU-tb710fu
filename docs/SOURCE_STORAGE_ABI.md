# Static macOS 27 block-storage ABI check

The retained prelink from run 43844823907 is a 142,753,792-byte fileset with
SHA-256 `045bfc7cfad9ed2539a586043dd43211263099734f4978abd8092c213b7d7d95`.
Its prelink record identifies the exact `26C5035r` profile KernelCache input
`d1003b5143ffd88439b4d45b6017f587171cfe6855660bed05370a2ea5db9fe9`.
The original unmodified Apple cache has not been independently rehashed locally;
the retained ZIP, kernel member and prelink provenance are checked instead.
This fileset contains the original IOStorageFamily and APFS binaries.

`scripts/audit-tb710fu-storage-abi.py` compares its IOBlockStorageDevice table
with the compiler-authored NativeUfsBlock table in run 44068063538's standalone
archive. It checks ZIP/member digests, the compile report and profile, decodes
collection chained fixups and object authenticated relocations, and compares
all 326 method slots' authentication, key, discriminator and address diversity.
There are zero differences. The 26 named block APIs at slots 272–297 also match
method identity/order; reserved methods remain opaque. The compiled DWARF size
and the original metaclass constructor both give `0x90`.

Four real IOStorageFamily call sites independently match their expected slot
offset and PAC discriminator: reportBlockSize (`0x8c8`, 40251),
reportMaxValidBlock (`0x8e0`, 32176), reportWriteProtection (`0x900`, 57027), and
doAsyncReadWrite (`0x918`, 58235). The audit follows the function and slot-address
modifier operands through straight-line instructions to the authenticated
branch. It does not execute those calls or establish arbitrary control-flow
equivalence. IOStorage/IOMedia tables and APFS symbols are inventoried; their
source model and runtime behavior are not qualified by this check.

The result and exact run/head/artifact provenance are in
[`evidence/storage-abi-44068063538.json`](evidence/storage-abi-44068063538.json)
and its companion provenance file. The full local JSON remains in the ignored
`.work/ufs-storage-abi-review/` directory with its digest recorded in the
committed result. No firmware, KernelCache, IPSW member or private key is added
to the repository.

Reproduce from the repository root (Python 3.11+; LLVM 19 tools in WSL Debian,
as named explicitly in the provenance JSON):

```powershell
python -B scripts/audit-tb710fu-storage-abi.py `
  --provenance docs/evidence/storage-abi-44068063538-provenance.json `
  --kernel .work/verified-prelink/kernel.macho `
  --kernel-report .work/verified-prelink/kernel.json `
  --kernel-zip .work/prelinked-input.zip `
  --native-archive .work/ufs-source-44068063538/extracted/libTb710fuNativeUfs.a `
  --compile-report .work/ufs-source-44068063538/extracted/ufs-adapter-build.json `
  --native-zip .work/ufs-source-44068063538/artifact.zip `
  --profile .work/UniXNU/profiles/os/macos-27.3-26C5035r/profile.toml `
  --output .work/ufs-storage-abi-review/reproduced.json
```

For a later complete source-storage archive, provide its own exact provenance,
report and ZIP, plus `--object-member NativeUfs.late.o` and
`--native-zip-member source-storage/libTb710fuSourceStorage.a`. Recheck after
source or flag changes; the `bf0be75` result does not cover later bridge code.

Static layout matching is one integration input. Late metaclass registration,
binary virtual calls, policy handoff, SMMU/queue/DMA ownership, media publication
and APFS root mount still require separate build and runtime verification. This
standalone audit does not cover the later selected strong startup hook and
native archive integration; those have a separate build report.

The actual archive from integrated run 44075573981 was audited again after the
startup/compiler-flag changes. Its archive SHA-256 is
`e0da7e494ccd11908c3c861ec4a15765e977c45345d02465e3e89dc47142fc80`;
`NativeUfs.late.o` is 591,792 B, SHA-256
`f7a4a794279b4b1ce61a40726c530cff93c1f3088d90b674d3947385287578c1`.
All 326 PAC slots, 26 named APIs, class size `0x90` and four binary dispatch
sites match the same fixed run 43844823907 reference. The new result is
[`evidence/storage-abi-44075573981.json`](evidence/storage-abi-44075573981.json),
with its own provenance file. To repeat it, use the complete-archive options
above and `.work/ufs-integrated-44075573981/{artifact.zip,extracted/source-storage/}`
as the native archive/report inputs. Do not replace the retained reference
kernel with this run's new kernel while reusing the historical provenance.

The separately bound
[`integration evidence`](evidence/ufs-integrated-44075573981.json) establishes
that the new archive was linked into a native kernel which passed strict
prelink and production parsing. The static comparison remains a build input;
it does not execute metaclass initialization, policy handoff, virtual calls,
DMA or APFS mounting.
