# LLVM / clang toolchain patches

Apply in this order to `llvm-project@58c46bae21181fb4240cf964461bea738d96d50e`
(the commit pinned by `unixnu.lock` for the local Linux toolchain):

1. `UniXNU/scripts/linux/llvm-xnu-builtin-type.patch` — XNU builtin type
   signatures, `XnuUsageSemantics`, and the Linux host fixes. Owned by the
   pinned framework checkout, not by this directory.
2. `llvm-ptrauth-null-values.patch` — accepts Apple's fourth `__ptrauth`
   argument, `"authenticates-null-values"`, which macOS 27's `os/base.h` uses
   in `OS_PTRAUTH_SIGNED_PTR_AUTH_NULL` (also reached from `bsd/sys/ucred.h`).

Upstream LLVM rejects that form in three independent layers, and each layer
only becomes visible after the previous one is fixed:

| Layer | File | Failure |
|---|---|---|
| Parser | `clang/lib/Parse/ParseDecl.cpp` (`ParsePtrauthQualifier`) | caps the list at three and discards the qualifier before Sema sees it |
| Attribute spec | `clang/include/clang/Basic/Attr.td` (`PointerAuth`) | declares three args, so `SemaAttr.cpp`'s `checkAtMostNumArgs(getMaxArgs())` rejects the fourth |
| Sema | `clang/lib/Sema/SemaType.cpp` (`HandlePtrAuthQualifier`) | asserts `<= 3` and hard-codes `AuthenticatesNullValues` to `false` |

The diagnostic wording in `DiagnosticParseKinds.td` moves from "1 and 3" to
"1 and 4" so the message matches the accepted grammar. Downstream support
(`PointerAuthQualifier::AuthenticatesNullValues`, the DWARF attribute) already
exists; only the syntax was missing.

Only the exact string `"authenticates-null-values"` or the integers `0`/`1`
are accepted; other targets are rejected. One-, two- and three-argument forms
are unchanged.

## Pinning

The patch must be generated against `58c46bae…`, not `llvm-project/main`:
`HandlePtrAuthQualifier` sits at line 8659 in the pinned revision and at 8727
on main, so a main-based patch does not apply.

## Provenance

The first version was diffed against a working tree that already had
`llvm-xnu-builtin-type.patch` applied, which captured that patch's
`XnuUsageSemantics` hunk as if it were ours. Regeneration now uses
baseline = pristine tarball + that patch's `Attr.td` hunk only, and the result
is verified to reproduce the tree that built clang byte-for-byte
(`.work/regen-patch-v3.py`, `.work/verify-patch-v3.py`).

Verification performed on 2026-11-21:

- `patch -p1` and `git apply` both apply it cleanly on the pinned revision
  after the builtin-type `Attr.td` hunk;
- the four resulting files are SHA-256 identical to
  `~/tb710fu-llvm-project`, the tree whose clang compiled
  `firehose_buffer.c` into an arm64e `libfirehose_kernel.a`;
- the installed `clang-24` reports
  `'__ptrauth' qualifier must take between 1 and 4 arguments`.
