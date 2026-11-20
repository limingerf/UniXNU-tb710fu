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
3. `0004-bringup-stage-trace.patch` writes stage lines from `start()`,
   `hostBringUp()` and `startRoot()` to the console capture ring using `printf`.
4. `0005-synchronous-bringup-checkpoints.patch` publishes 20 text checkpoints
   using the same optional live-log provider as SourceStorage. `ufsentry`
   appears in the footer before the first diagnostic print in `start()`.
   `ufsgate` precedes the provider gate. `ufsqown` covers firmware queue stop,
   SMMU programming and replacement queue start as one transition.
   The patch also removes the extra provider cast and four extra controller
   reads from the formatted probes. Run `tools/check_ufs_bringup_trace.py
   --source /path/to/pinned/ShengUfs` to verify placement, footer labels and
   the post-0002 behavior.
5. `0006-quiescent-sleep-replacement.patch` replaces the two `IOSleep` calls in
   `hostBringUp()` with `IODelay` loops after the post-wake stop.
6. `0007-quiescent-counter-spin.patch` replaces those loops and the command
   poll delay with a bounded `mach_absolute_time` spin: in a thread without a
   registered idle latency `IODelay` is itself a timer wait. `ufsspn`/`ufssetd`
   bracket the settle and `ufstur` precedes each LUN-ready attempt.
7. `0003-minimal-source-runtime.patch` is selected only by the complete minimal
   source-storage archive builder. It disables the optional boot observer's
   declaration and call together when `TB710FU_UFS_NO_BOOT_OBSERVER=1`; it does
   not provide a no-op implementation. The isolated NativeUfs adapter uses
   patches 0001, 0002, 0004, 0005, 0006 and 0007.

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
