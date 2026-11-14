# TB710FU UEFI source overlay

This directory contains a source-only TB710FU board overlay for
`UniXNU/uefi-sheng@66421cb7bb5694843aec8de1ca3d1de4c20cca38`.
The same-device `bmk-keyeast/UniXNU-tb710fu` revision
`23235249d71604f8a6f743680ef8fd1003fe7189` is the board reference.

Copy `overlay/` over a fresh checkout of the pinned firmware base. Keep the
base's gitlinked Mu dependencies at their recorded revisions. The main
repository's lock and cloud workflow own checkout, package paths and the
actual build invocation. This overlay does not replace UniXNU's build driver.

The reviewed initial policy uses open-source Mu drivers, the shared UniXNU
RAM disk and FAT stack, and the existing ABL scanout. It does not import
`Binaries/`, proprietary panel XML, closed vendor DXE, keys, or stock images.
Enabling a vendor hardware/display gate is a build error: implementing a
vendor path requires a separate reviewed change with real, hash-checked
inputs. No empty binary or driver may satisfy a missing dependency.
Use numeric `DEVICE_MODEL=0` for compatibility with the pinned firmware
builder's host model-index contract: `build_uefi.py` declares an integer
model, defaults `--model` to 0, checks it against `status.model_count=1`,
and forwards it to the build. The string tb710fu is the device name, not
that integer model index. No native consumer of DEVICE_MODEL was found in
this TB710FU/Lanai/Silicium path, so 0 does not establish a native board
selector or CPU topology. A direct EDK II invocation merely receives the
macro; the canonical host-tool contract is the reason for retaining 0.
`ENABLE_SECUREBOOT=0` describes this UEFI image policy only; it does not
change Android ABL/AVB. Enabling UEFI Secure Boot requires a separate
qualified policy/key integration.

The expected FD is exactly `0x300000` bytes at `0xA7100000`. Its board DSC is
`Platforms/Lenovo/tb710fuPkg/tb710fu.dsc`; the expected output is
`Build/tb710fuPkg/{target}_CLANGPDB/FV/SILICIUM_UEFI.fd`. Android header v4 is
recorded separately in the board's firmware configuration; image assembly
still requires qualified stock DT and ramdisk inputs.

See [the audit](../docs/UEFI_AUDIT.md) for memory provenance, all retained or
rejected differences, inherited framework limitations and qualification
gates. A cloud compile proves source integration and FD structure only.
There is no clean-project device boot, GOP, input, UFS or XNU handoff claim.

No device command, signing, flashing, slot change or partition operation is
part of preparing or building these sources.
