# TB710FU post-OS kernel patches

Base: UniXNU `0c1bac3d51a3009dca1708b15a5198f4b43d057a`, XNU `ac9718fb1af618d5ce8678d0dc6e8a58f252216f`, OS profile `macos-27.3-26C5035r`.

Apply these in the device patch series **after** the complete selected architecture, SoC and OS series:

1. `0001-tb710fu-gate-owner-authenticated-open.patch`
2. `0002-tb710fu-gate-optional-exit-trace.patch`

The native-only board has neither owner-verified storage nor the board exit-backtrace provider. Official macOS patches include their consumers independently of those providers. These gates disable the unselected consumers coherently. They add no replacement function, global, accepting authorization result or security stub. Other hunks of the official macOS patches remain in place.

## Owner-authenticated-open fallback

The upstream `macos-owner-root-authenticated-open.patch` includes a fallback for owner-verified, read-only media that lacks an Apple seal. It reads `native_owner_root_accepted` and `native_owner_root_mount_device`, whose definitions are normally compiled in `bsd_init.o` only with the real root audit.

`XNU_TB710FU_OWNER_AUTH_OPEN` is undefined by default. Both the fallback helper and its invocation are then omitted. The existing `openat_dprotected_internal` result, including `EAUTH`, is returned normally; this patch does not authorize an otherwise rejected volume.

Before enabling, supply **all** of the following:

- An actual owner-verified storage implementation and correctly bound root policies.
- The real `bsd_init.o` owner audit/state definitions, compiled with `XNU_OWNER_ROOT_AUDIT=1`, and the full source-storage or qualified verified-root dependency closure required by the upstream build script.
- Actual root acceptance only after its mounted-media binding, block verification and read-only checks. `XNU_OWNER_ROOT_TRUST=1` requires the corresponding real transport/storage path.
- `-DXNU_TB710FU_OWNER_AUTH_OPEN=1` in the **actual `vfs_syscalls.o` compile command**.

Do not define the macro solely to make linking succeed. Do not create fake state globals or a provider that returns success without verification.

## Optional exit backtrace

The upstream `macos-droidvm-k27-parity.patch` inserts an optional `native_exit_trace` weak import. In the old Linux arm64e build it generated an unresolved authenticated pointer; the chosen native archives do not include the provider.

`XNU_TB710FU_EXIT_TRACE` is undefined by default, compiling out that whole optional block. The separate upstream `macos-exec-log.patch` remains, so normal `exec-log=1` diagnostics do not require this backtrace hook.

Before enabling:

- Build and link the real target-compatible `native_exit_trace(int, const char *, int)` implementation. The official example is `UniXNU-sheng@68800685763d755c368775b848dd703fb3f8a3ce:package/BootUiTraceProbe.c:1231`. Review its dependencies and private ABI before porting it.
- Pass `-DXNU_TB710FU_EXIT_TRACE=1` in the **actual `kern_exit.o` compile command**.
- Confirm a real definition and the correct arm64e calling/pointer-authentication ABI. An empty weak definition is not a provider.

Neither macro is a boot argument. A new `CONFIG_XNU_TB710FU_*` line alone is insufficient unless a reviewed build integration actually forwards it as the required compiler define.

## Verification performed

Generated from the two pristine XNU source files after applying, in official order, **all ten selected upstream patches that affect those files**. Unrelated XNU files were not copied into the scratch fixture. Both device patches passed `git apply --check` and were then actually applied to those prepared files. The owner gate occurs twice; the exit gate once.

The local check record is `.local/kernel-patch-check.json`; the prepared-file fixture is `.local/kernel-patch-fixture/`. This is a patch-application check, not a compiler or device test. No fetched source tree was edited.

## Required SoC context before OS boot-progress

UniXNU `0c1bac3d` has no `patches/xnu-12377/sheng-platform-power.patch`; its older `patches/xnu/sheng-platform-power.patch` exists. The canonical XNU12377 patch is:

- Repository: `https://github.com/UniXNU/UniXNU-sm8550-common`
- Commit: `a9f45dc60c035084b95463781538380f4c3606c9` (current official sheng feed)
- Path: `patches/xnu-12377/sheng-platform-power.patch`

It adds the guarded `extern void sheng_pmgr_run(void); sheng_pmgr_run();` block after `bsd_bufferinit()`. The OS `zzz-boot-progress.patch` expects that actual block as context. Keep the macro disabled on TB710FU: importing the guarded context does not qualify or enable SM8550 PMGR hardware. Place the canonical patch in the chosen SoC sequence before the OS sequence; this directory does not duplicate it.

## Actual stack-alignment export required by the 26C5035r reviewed site

`0003-tb710fu-kernel-stack-alignment.patch` extracts only the `osfmk/kern/stack.c` section, verbatim, from `UniXNU/UniXNU-sheng@68800685763d755c368775b848dd703fb3f8a3ce:patches/xnu-12377/zzzz-sheng-ipados27-bringup.patch`. Apply it after the complete official OS sequence and the two optional-provider gates. No board hardware code from the large source patch is included.

The initial strict prelink failed in `fileset_emit.reviewed_sites()` because the native artifact lacked `_unixnu_kernel_stack_shift`. The matching `fileset-26C5035r-symbols.json` fixture identifies AppleMobileFileIntegrity's GOT slot at `0xfffffe00081d4630`: its CoreEntitlements DER iterator uses the loaded shift to test remaining stack room. A page shift of 12 is not the guarded-stack allocation alignment on this 4 KiB kernel. Other users must retain the real `PAGE_SHIFT_CONST` for page arithmetic.

The official implementation aligns the whole allocation, including two lower guard pages and one upper guard page, via `roundup_pow2(kernel_stack_size + ptoa(3))`. It exports `unixnu_kernel_stack_shift` and initializes it from that actual alignment during `stack_init`. It does not use a fake constant or replacement stack-check function.

All fixtures matching the exact macOS kernelcache SHA256 `d1003b5143ffd88439b4d45b6017f587171cfe6855660bed05370a2ea5db9fe9` were inspected: there is one matching fixture, with **one required site, zero reviewed functions and zero reviewed data entries**. That site requires only `_unixnu_kernel_stack_shift`. `llvm-nm --defined-only` confirmed it is absent from the original native artifact SHA256 `4e65a8b08d5d70393b3c0f5d3f78b973bce1d15eb7f4d6f921c320b28124abce` (25,133,408 bytes). No second same-kind required site export is waiting behind it in this pinned fixture. This does not certify the 139 unpaired global name-map targets or the retained closure; later strict emitter checks still need actual reports.

Validation performed: start from pinned pristine XNU `stack.c`, apply every selected official arch/OS patch affecting it, then `git apply --check` and actual apply of this extracted patch. Verify exactly one real export declaration, one actual initialization and the correct allocation-mask formula in the resulting source. The native kernel **must be recompiled** and then strictly prelinked with the new native hash/map. The previous artifact cannot be fixed by prelink-only retry. No compile or device test was performed by this patch audit.


## Bootstrap capacity from upstream PR #249

`0005-native-4k-bootstrap-pool-9g.patch` adapts only the pool-size change from
`UniXNU/UniXNU` PR #249 head `9b9db9c9ff13a595492d241366426383c88bf236`
(open, not merged when checked on 2026-11-16). The PR edits the older `patches/xnu/`
recipe. This device selects `patches/xnu-12377/`, so the device patch applies after
the existing official 16-page declaration and changes that branch to 22 pages.
The official source/cache and its pinned patch file stay unchanged.

The extra six pages reserve 24 KiB for non-Apple 4 KiB builds. The ARM_LARGE_MEMORY
34-page branch and fallback 8-page branch are unchanged. The existing genassym
export, pool allocation, full clearing and exhaustion guard already consume
BOOTSTRAP_TABLE_SIZE. No independent constant is patched.

Actual patch application to pristine pinned pmap.h plus all selected patches
affecting it, and page-budget boundary checks through 9 GiB passed. This repairs
the known larger-window capacity issue; it is not claimed to fix the observed
post-MMU screen. Evidence: `docs/evidence/pr249-bootstrap-pool.json`.

## TTBR1 switch-report mapping

`0006-tb710fu-diag-segment-rx.patch` maps diagnostic segments only when both
`__KDIAG` and `__KDATRW` exist, are contiguous, and meet the 16 KiB alignment
used by the diagnostic Mach-O. It maps code RX and mutable state/stack RW-NX;
incomplete or malformed pairs remain unmapped. `0009-tb710fu-switch-helper-noinline.patch`
keeps the 0007 marked switch helper as a distinct call target for the stage QR
reporter. For fileset kernels, the segment lookup uses the primary kernel
collection header because post-link tooling appends these load commands to the
outer MH_FILESET header; non-fileset kernels use `_mh_execute_header`. Together
these patches enable diagnostics across TTBR1; they do not change the page-table
algorithm or claim a functional fix.

## Explicit source-storage startup

`0011-tb710fu-source-storage-start.patch` adapts the post-`vfsinit()` placement
from `UniXNU-sheng@1108068537d5396edb2c63b2f8c6f5b97b0a4bbc`,
`patches/xnu-12377/source-storage-runtime.patch`. The device uses a conditional
strong reference under `XNU_TB710FU_SOURCE_STORAGE=1`. Disabled configurations
emit no reference; enabled builds must link the actual implementation. It is
applied after the selected arch/macOS/device series through 0010 and has been
checked against that prepared source context. The separate native-link verifier
checks the actual BSD compile record, strong object reference, final definition
and saved `linkarchives` input. This does not prove runtime class registration
or root mounting, and adds no weak security or hardware provider.

## Explicit USB CDC startup

`0012-tb710fu-cdc-start.patch` adds the real `tb710fu_cdc_start()` call before
`vfsinit()`, under `XNU_TB710FU_CDC=1`. It applies to the selected post-0011
source. The worker separately requires `tb710fu-cdc=1` and performs live
hardware and DMA checks before takeover. Actual BSD strong imports, archive
force-loading and final definitions are checked by `tools/verify_cdc_integration.py`.
This startup placement can retain earlier buffered console output if boot
reaches BSD initialization; it cannot capture USB output before then or promise
the last panic bytes after scheduling stops. Source/build checks do not establish
CDC enumeration on TB710FU.

## Native current-boot ramoops export

`0013-tb710fu-ramoops-snapshot.patch` adds a bounded, consistent read of this
boot's text; `0014-tb710fu-native-live-log.patch` connects the separate real
LiveLog archive to native startup and console reacquisition. Two live sources
replace the old checkpoint waits: independent XNU capture on the left and
actual current-boot ramoops on the right. Every left file retains the same
pre-machine_startup backup with explicit raw-section lengths. Normal and
graphics-disabled console paths preserve their memory taps. See
`docs/LIVE_RAMOOPS.md` for byte limits and scheduling boundaries.

`0016-tb710fu-startup-callback-return-marker.patch` records a completion marker
after the KMEM rank-0 startup callback and polls the live log, distinguishing
callback return from the preceding trace. This is diagnostic instrumentation,
not a compressor sizing or runtime fix.

`0022-tb710fu-synchronous-live-checkpoints.patch` routes the existing startup
and kalloc markers through `tb710fu_live_log_checkpoint`, adds a callback-entry
marker before every startup callback, and retains callback-return markers. The
new `tb710fu-live-checkpoints=1` runtime option publishes an independent
single-frame XNU QR synchronously and skips the background renderer; without it,
the original throttled dual-stream polling behavior remains. The package gate
requires this option alongside `tb710fu-live-log=1`. This leaves the last
successfully rendered pre-operation frame visible if a later operation stops,
but cannot refresh after execution stops or establish the faulting PC.

`0023-tb710fu-bootstrap-suboperation-checkpoints.patch` splits the remaining
grouped direct operations in `kernel_bootstrap_thread`: each of its five
`early_random` calls, the kernel-list/post-test calls, and thread binding versus
cluster power resume now have their own begin/done frames. It preserves the broad
parent markers and does not reorder or alter the operations.

The first native attempt, run 44112832250 at `eb1f2bc7`, exposed a missing
internal-header export in `pe_kprintf.c`. The device patch now adds its two real
headers to `osfmk/kern/Makefile` XNU_ONLY_EXPORTS, using the same official
mechanism as `boot_progress.h`; no fetched source or official script is edited.
The first compiler error and later missing-filelist consequence are recorded
separately in `docs/evidence/live-ramoops-ci-44112832250.json`.
