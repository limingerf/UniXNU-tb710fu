# TB710FU UFS adaptation

Apply these device-owned patches in order to
`UniXNU/ShengUfs@99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0`.
The adapter script exports that exact commit and never edits fetched caches.

1. `0001-sm8650-compatible.patch` accepts the board's exact DT compatible.
2. `0002-transactional-handoff.patch` plans the SMMU before stopping the idle
   firmware queue, snapshots both owners' register state and verifies rollback
   before resuming firmware DMA. A stale plan or unverified restoration keeps
   the queue stopped and panics. W1C interrupt status is cleared after the new
   queue is ready. Legacy firmware-list adoption and Sheng Wi-Fi/DPU options
   are refused on this board.
3. `0004-bringup-stage-trace.patch` adds unconditional `IOLog` stage lines to
   `hostBringUp()` and the `startRoot()` entry. It is authored against the
   post-0002 source and ordered after 0002, before 0003. `gRootDiagnostic.stage`
   already tracked these stages but only with `native-ufs-diagnostics=1`, which
   the device does not pass. Every added statement is an `IOLog` call plus the
   braces needed to attach one to a previously braceless if-body; removing them
   reproduces the post-0002 file exactly, and every `write32`, wait constant,
   queue/SMMU call, `native_ufs_root_step` value, `*why` string and `panic`
   site is unchanged. It exists to localize a device halt inside the takeover.
4. `0003-minimal-source-runtime.patch` is selected only by the complete minimal
   source-storage archive builder. It disables the optional boot observer's
   declaration and call together when `TB710FU_UFS_NO_BOOT_OBSERVER=1`; it does
   not provide a no-op implementation. The isolated NativeUfs adapter still
   uses only patches 0001, 0002 and 0004.

The controller/GCC/SID facts come from the same-device Linux commit
`4d04410798b538fbe4b50664e3a66d9500a2ed31`, documented in
[`UFS_PORT.md`](../../docs/UFS_PORT.md). SMMUv2 register semantics follow ARM
IHI 0062D.c and the pinned official
[`ArmSmmu4KStreams.c`](https://github.com/UniXNU/ArmSmmu4K/blob/83969cd874503e65997ff7639f7dcf3d0a92cd17/ArmSmmu4KStreams.c):
sCR0.CLIENTPD is bit 0 (clear means clients enabled), IDR0.SMS is bit 27, and
EXIDENABLE is bit 3. This adapter refuses the extended-ID format. SMMUv3's
CR0.SMMUEN must not be used to interpret this SMMUv2 register.

The production SMMU and queue/command tests pass under Debian g++ 14.2.0 and
clang++ 19.1.7 with warnings treated as errors. Run the same patch/staging/test
path used by CI without requiring XNU compiler records:

```sh
CXX=clang++ python3 scripts/build-tb710fu-ufs-adapter.py \
  --host-tests-only --source /path/to/pinned/ShengUfs \
  --tree .work/ufs-host-validation
```

`build/tb710fu-ufs-adapter/host-tests.json` beneath that tree records compiler,
commands, patch hashes and input hashes. Tests use simulated MMIO and DMA;
they do not qualify TB710FU hardware. The separate static block-device ABI
audit is documented in `docs/SOURCE_STORAGE_ABI.md`. After the complete archive
build passed, the board selected `SOURCE_STORAGE=y` plus explicit archive and
startup integration; `NATIVE_UFS=n` keeps automatic personalities absent.
That integration requires its own native/prelink and runtime verification.
