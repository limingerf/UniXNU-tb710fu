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
they do not qualify TB710FU hardware or the macOS 27 storage ABI. Both kernel
storage switches remain disabled until separate integration and runtime gates
pass.
