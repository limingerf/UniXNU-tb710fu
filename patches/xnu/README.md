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
