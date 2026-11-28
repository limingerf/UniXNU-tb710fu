"""
Regenerate patch 0053 entirely from Windows, avoiding both the WSL path
mismatch and PowerShell's UTF-16 redirection.

Steps:
  1. reset the clean worktree's systemcalls.c
  2. inject the probe (import the injector logic inline so paths stay native)
  3. run `git diff` via subprocess and write the bytes with pathlib
  4. verify with `git apply --check` against the now-pristine file
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(r'D:\unixnu-tb710fu-clean')
WT = ROOT / '.work' / 'xnu-0053-clean'
OUT = ROOT / 'patches' / 'xnu' / '0053-tb710fu-dyld-cache-probe.patch'
TARGET_REL = 'bsd/dev/arm/systemcalls.c'
target = WT / 'bsd' / 'dev' / 'arm' / 'systemcalls.c'


def git(*args, check=True):
    r = subprocess.run(['git', '-C', str(WT), *args],
                       capture_output=True, text=True)
    if check and r.returncode != 0:
        sys.exit(f'git {args} failed: {r.stderr.strip()}')
    return r


# 1. pristine
git('checkout', '--', TARGET_REL)

src = target.read_text(encoding='utf-8')
if 'tb710fu_dyld_probe_put' in src:
    sys.exit('probe still present after checkout; aborting')

# 2. inject declarations before unix_syscall()
anchor = 'unix_syscall('
idx = src.index(anchor)
decls = r'''/*
 * tb710fu: capture the exact arguments of the syscalls launchd loops on while
 * dyld refuses the shared cache. See docs/LAUNCHD_SYSCALL_LOOP.md.
 *
 * Patch 0043 recorded only (code, error). That proved the six-call loop exists
 * and that every call succeeds, but it cannot explain why dyld munmaps after a
 * successful mmap. These records keep the arguments and the return value.
 *
 * Written round-robin as groups of four u64 in a 64-slot ring:
 *   [0] tag, [1] argA, [2] argB, [3] argC
 */
volatile uint64_t tb710fu_dyld_probe[64];
volatile uint32_t tb710fu_dyld_probe_idx;

static void
tb710fu_dyld_probe_put(uint32_t tag, uint64_t a, uint64_t b, uint64_t c)
{
	uint32_t slot = __atomic_fetch_add(&tb710fu_dyld_probe_idx, 4u,
	    __ATOMIC_RELAXED) & 63u;

	tb710fu_dyld_probe[slot] = tag;
	tb710fu_dyld_probe[(slot + 1u) & 63u] = a;
	tb710fu_dyld_probe[(slot + 2u) & 63u] = b;
	tb710fu_dyld_probe[(slot + 3u) & 63u] = c;
}

'''
src = src[:idx] + decls + src[idx:]

# 3. inject the recorder after the dispatch line
call_anchor = ('error = (*(callp->sy_call))(proc, &uthread->uu_arg[0], '
               '&(uthread->uu_rval[0]));')
if call_anchor not in src:
    sys.exit('dispatch anchor missing')

hook = r'''	/*
	 * tb710fu: full-argument capture for the dyld cache retry loop.
	 * pid 1 only, so nothing else is perturbed.
	 */
	if (pid == 1 && error == 0) {
		switch (code) {
		case 463: /* openat(dirfd, path, flags, mode) */
			tb710fu_dyld_probe_put(0x4F41544Eu,
			    (uint64_t)uthread->uu_arg[1],
			    (uint64_t)uthread->uu_arg[2],
			    (uint64_t)(int64_t)uthread->uu_rval[0]);
			break;
		case 197: /* mmap(addr, len, prot, flags, fd, off) */
			tb710fu_dyld_probe_put(0x4D4D4150u,
			    ((uint64_t)uthread->uu_arg[2] << 32) |
			    ((uint64_t)uthread->uu_arg[3] & 0xffffffffu),
			    (uint64_t)uthread->uu_arg[4],
			    (uint64_t)(int64_t)uthread->uu_rval[0]);
			break;
		case 92: /* fcntl(fd, cmd, arg) */
			tb710fu_dyld_probe_put(0x464E4354u,
			    (uint64_t)uthread->uu_arg[0],
			    (uint64_t)uthread->uu_arg[1],
			    (uint64_t)(int64_t)uthread->uu_rval[0]);
			break;
		case 73: /* munmap(addr, len) */
			tb710fu_dyld_probe_put(0x4D554E4Du,
			    (uint64_t)uthread->uu_arg[0],
			    (uint64_t)uthread->uu_arg[1],
			    0u);
			break;
		default:
			break;
		}
	}

'''
src = src.replace(call_anchor, call_anchor + '\n' + hook, 1)
target.write_text(src, encoding='utf-8')

# 4. emit patch bytes
diff = subprocess.run(['git', '-C', str(WT), 'diff', '--binary'],
                      capture_output=True).stdout
OUT.write_bytes(diff)
print(f'wrote {len(diff)} bytes to {OUT}')

# 5. verify it applies to a pristine tree
git('checkout', '--', TARGET_REL)
chk = subprocess.run(['git', '-C', str(WT), 'apply', '--check', str(OUT)],
                     capture_output=True, text=True)
if chk.returncode == 0:
    print('APPLY_CHECK_PASS')
else:
    print('APPLY_CHECK_FAIL')
    print(chk.stderr)
    sys.exit(1)
