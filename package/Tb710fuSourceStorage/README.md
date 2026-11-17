# TB710FU minimal source-storage bridge

`../SourceStorage.cpp` adapts the same reference's SourceStorage.cpp, blob
`dd005e5d3405e97b9c2c7d70fe607a8848b8b19b`. Its startup sequence loads the
original IOStorageFamily, checks the public base size, authenticates RootPolicies,
loads APFS, registers the deferred public base and NativeUfs classes, then
attaches the real UFS service to IOPlatformExpertDevice. Startup requires
`native-source-storage=1`. Device patch 0011 selects a conditional strong
`bsd_init` hook after `vfsinit()` for the source-storage integration build.
Equal class size supplements a separate dispatch/PAC audit; it cannot replace it.

`../SourceRootBinding.cpp` adapts reference blob
`01ceb203de073170de257aaef1e8ce901f05dd52`. It validates UUID/BSD identities,
rejects ambiguous matches and follows at most 64 retained providers to the real
`native_ufs_block_authenticates` implementation. It stages the manifest digest
until the complete check succeeds and clears failed outputs. Rechecking the
indices reduces stale-snapshot exposure; it is not a registry lock. Mount
callers must reauthorize the actual mounted media.

`../SourceRootPolicies.cpp` adapts
[`package/SourceRootPolicies.cpp` at UniXNU-sheng commit
`1108068537d5396edb2c63b2f8c6f5b97b0a4bbc`](https://github.com/UniXNU/UniXNU-sheng/blob/1108068537d5396edb2c63b2f8c6f5b97b0a4bbc/package/SourceRootPolicies.cpp),
Git blob `38c066b99a52e4be976a65b7db91d19f29809447`. The reference was read
with `git show <exact-commit>:package/SourceRootPolicies.cpp` from a temporary
reference repository, independently of the fetched checkout's HEAD.

The preserved contract is the 16-byte physical address/length
`/chosen/memory-map/RootPolicies` property, the 128 MiB handoff limit,
`VROOTB01` version 1 with at most 16 contiguous policy records, and generation
floor 1. A 544-byte policy uses `vr_open_header`; a larger envelope uses
`vr_open`. Authentication uses the existing `verified_root_key` RootAnchor
symbol and `vr_xnu_crypto`, with no generated key or replacement crypto.
Header-only policies still need NativeUfs to read and authenticate the matching
userdata `.vroot` tables before block access is authorized.

`RootPolicyBundle.h` separates table validation from the XNU memory mapping.
It checks the complete table and exact final length before calling the supplied
authenticator. Bounds use subtraction before addition; the count is bounded
before table arithmetic. It exposes no result until every policy is accepted.

The device adaptation copies the firmware handoff into its own kernel
allocation, releases the firmware mapping, and removes current and maximum
write permission before parsing and authenticating. Failed protection, layout,
or signature validation frees that allocation. Policy entries are staged
locally, copied to the existing ABI globals only after complete success, and
the count is published last. The accepted read-only allocation remains alive
for the boot lifetime. Concurrent/reentrant loading attempts cannot replace
the published allocation. Consumers retain the upstream requirement to start
after this synchronous loader returns successfully.

The helper's host test uses the framework-pinned VerifiedRoot revision
`974f043d0c71ba2e60331483a41c6c6d35274ee3` and its unmodified
`tests/openssl_provider.c`. It verifies the existing public OS/SystemOS/App
headers under `scripts/policies/26C5035r`, a supplied full App manifest,
signature/table mutations and the independent trusted key. Layout cases cover
invalid magic/version/count, table truncation, gaps/overlap, 64-bit overflow,
short envelopes, inconsistent total lengths, trailing bytes and failure of
each authentication callback. These are host checks; XNU memory protection,
allocation cleanup and the runtime handoff still need a target compiler and
runtime verification. Debian `g++ 14.2.0` passed these cases with
`-Wall -Wextra -Werror`; the helper test also passed AddressSanitizer and
UndefinedBehaviorSanitizer with the same real policy inputs.

Example from the repository root on a Linux host with OpenSSL development
headers (use an ignored output directory):

```sh
storage_test_out=.work/root-policy-bundle-test
mkdir -p "$storage_test_out"
cc -std=c11 -Wall -Wextra -Werror \
  -I .work/UniXNU/kits/VerifiedRoot/include \
  -c .work/UniXNU/kits/VerifiedRoot/src/verified_root.c \
  -o "$storage_test_out/verified_root.o"
cc -std=c11 -Wall -Wextra -Werror \
  -I .work/UniXNU/kits/VerifiedRoot/include \
  -c .work/UniXNU/kits/VerifiedRoot/tests/openssl_provider.c \
  -o "$storage_test_out/openssl_provider.o"
c++ -std=c++17 -Wall -Wextra -Werror \
  -I .work/UniXNU/kits/VerifiedRoot/include \
  package/Tb710fuSourceStorage/RootPolicyBundleTest.cpp \
  "$storage_test_out/verified_root.o" "$storage_test_out/openssl_provider.o" \
  -lcrypto -o "$storage_test_out/check"
"$storage_test_out/check" scripts/policies/26C5035r \
  .work/UniXNU/out/tb710fu/macos-27.3-26C5035r/tb710fu_macos/tree/build/verified-root-App/manifest.bin
```

The device-owned `scripts/build-tb710fu-source-storage.py` explicitly adapts the
pinned framework's `build-sheng-source-storage.py`. It uses the exact public
IOStorageFamily source and the framework's VerifiedRoot/Virtio gitlinks, kernel
compiler records and macOS ABI flags. It compiles all three bridges, the
deferred public base, NativeUfs and real VerifiedRoot core/XNU crypto into a
standalone `libTb710fuSourceStorage.a`. The independent public key is linked;
the three signed 544-byte headers form a separate 1,712-byte VROOTB01 bundle.
The builder directly checks each header's format, UUID, image digest, byte/block
counts, generation and owner-key digest against the profile/verification record.
Runtime still authenticates the signatures and userdata hash tables.

RAM/Data storage, benchmarks, file logging, process diagnostics and wireless
providers are omitted as a unit. Patch 0003 disables the optional observer's
declaration and call together; no weak no-op or security/hardware substitute is
generated. Deferred objects are checked for the exported constructor and the
absence of an eager `__mod_init_func` section.

The optional `ufs-bridges` workflow compiles this standalone archive after the
native kernel. After the standalone archive passed in run 44071685590, the
board selected `SOURCE_STORAGE=y`, the explicit archive and strong hook for a
separate `ufs-integrated` build. `NATIVE_UFS=n` keeps automatic personalities
absent. The uniquely named wrapper follows the fixed archive protocol and
records precise generated-link invalidation; it does not replace an official
script. An archive compile supplies neither
an integrated/prelinked storage kernel, runtime APFS qualification nor a hardware
boot result. The fetched official script remains unchanged.
