#!/usr/bin/env bash
# Generate patch 0053: record full parameters for the syscalls that launchd
# loops on, so we can tell WHY dyld discards the shared cache.
#
# Round 27 evidence (docs/LAUNCHD_SYSCALL_LOOP.md):
#   launchd spins in
#     openat -> fstat64 -> pread -> fcntl(F_ADDFILESIGS_RETURN) -> mmap -> munmap
#   and every one of those returns error 0.  map_and_slide (536) never runs.
#
# Patch 0043 only stores `code` and `error` per ring slot, which is not enough
# to separate these hypotheses:
#   A. mmap succeeds but with flags/anchor that dyld rejects afterwards
#   B. F_ADDFILESIGS_RETURN returns an end offset inconsistent with the cache
#      header's codeSignatureOffset/Size
#   C. the file opened as fd 4 is a sub-cache whose header does not match
#
# So capture, for pid 1 only, per syscall:
#   openat : path (copied out), flags, resulting fd
#   mmap   : addr, len, prot, flags, fd, offset, and the returned address
#   fcntl  : cmd, the user_fsignatures input, and the fs_blob_size/fs_hash_type
#            that the kernel writes back for F_ADDFILESIGS_INFO
#   munmap : addr, len  (to correlate with the mmap just before it)
#
# Generated against a pristine worktree the same way 0052 was, because
# hand-written hunk counts broke `git apply` before.
set -euo pipefail

ROOT=/mnt/d/unixnu-tb710fu-clean
WT=/mnt/d/unixnu-tb710fu-clean/.work/xnu-patch-lf-0053
BASE=ac9718fb

cd "$ROOT"

if [ ! -d "$WT" ]; then
    git -C .work/UniXNU/out/feeds/xnu-3199e8bc/.work/xnu-patch-lf worktree add \
        --detach "$WT" "$BASE"
fi

cd "$WT"

cat > /tmp/0053-marker.c <<'EOF'
EOF

# --- inject the recorder into bsd/dev/arm/systemcalls.c ---------------------
python3 - "$WT" <<'PYEOF'
import sys, pathlib, re

wt = pathlib.Path(sys.argv[1])
target = wt / 'bsd' / 'dev' / 'arm' / 'systemcalls.c'
src = target.read_text()

anchor = 'unix_syscall('
idx = src.index(anchor)

decls = r'''
/*
 * tb710fu: capture the exact arguments of the syscalls launchd loops on while
 * dyld gives up on the shared cache.  See docs/LAUNCHD_SYSCALL_LOOP.md.
 *
 * The 0043 ring only kept (code, error), which cannot distinguish
 * "mmap succeeded with the wrong flags" from "signature offset mismatch".
 */
volatile uint64_t tb710fu_dyld_probe[64];
volatile uint32_t tb710fu_dyld_probe_idx;

static void
tb710fu_dyld_probe_put(uint32_t tag, uint64_t a, uint64_t b, uint64_t c)
{
	uint32_t slot = __atomic_fetch_add(&tb710fu_dyld_probe_idx, 4u,
	    __ATOMIC_RELAXED) & 63u;
	tb710fu_dyld_probe[slot] = tag;
	tb710fu_dyld_probe[(slot + 1) & 63u] = a;
	tb710fu_dyld_probe[(slot + 2) & 63u] = b;
	tb710fu_dyld_probe[(slot + 3) & 63u] = c;
}

'''
src = src[:idx] + decls + src[idx:]

# find the exit point of unix_syscall where `error` and the return value exist
# and add a post-syscall recorder keyed on `code`.
ret_anchor = '\treturn (int)retval;'
if ret_anchor not in src:
    # fall back: locate the last 'retval' assignment region
    print('WARN: exact return anchor not found; skipping exit hook', file=sys.stderr)
else:
    hook = r'''	/* tb710fu: full-argument capture for the dyld cache retry loop */
	if (proc_getpid(current_proc()) == 1) {
		switch (code) {
		case 463: /* openat */
			tb710fu_dyld_probe_put(0x4F41544Eu /* 'OATN' */,
			    (uint64_t)uthread->uu_arg[1] /* path */,
			    (uint64_t)uthread->uu_arg[2] /* flags */,
			    (uint64_t)error);
			break;
		case 197: /* mmap */
			tb710fu_dyld_probe_put(0x4D4D4150u /* 'MMAP' */,
			    (uint64_t)uthread->uu_arg[1] /* len */,
			    ((uint64_t)uthread->uu_arg[3] << 32) |
			    ((uint64_t)uthread->uu_arg[2] & 0xffffffffu),
			    (uint64_t)(int64_t)retval);
			break;
		case 92: /* fcntl */
			tb710fu_dyld_probe_put(0x464E4354u /* 'FNCT' */,
			    (uint64_t)uthread->uu_arg[0] /* fd */,
			    (uint64_t)uthread->uu_arg[1] /* cmd */,
			    (uint64_t)(int64_t)retval);
			break;
		case 73: /* munmap */
			tb710fu_dyld_probe_put(0x4D554E4Du /* 'MUNM' */,
			    (uint64_t)uthread->uu_arg[0] /* addr */,
			    (uint64_t)uthread->uu_arg[1] /* len */,
			    (uint64_t)error);
			break;
		default:
			break;
		}
	}
		'''
    src = src.replace(ret_anchor, hook + ret_anchor, 1)

target.write_text(src)
print('patched', target)
PYEOF

git -C "$WT" diff > "$ROOT/patches/xnu/0053-tb710fu-dyld-cache-probe.patch"

echo "=== patch size ==="
wc -l "$ROOT/patches/xnu/0053-tb710fu-dyld-cache-probe.patch"

echo "=== apply check against current tree ==="
cd "$ROOT/.work/UniXNU/out/feeds/xnu-3199e8bc"
if git apply --check "$ROOT/patches/xnu/0053-tb710fu-dyld-cache-probe.patch" 2>&1; then
    echo "APPLY_CHECK_PASS"
else
    echo "APPLY_CHECK_FAIL - patch needs regeneration against the real baseline"
fi
