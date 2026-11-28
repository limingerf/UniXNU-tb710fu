"""
Regenerate patch 0053 with the declaration block placed at the SAME anchor
patch 0043 uses, so the emitted hunks are stable and independent of where the
function happens to be declared.

Two earlier attempts failed for different reasons:

  1. diffed the bare feed -> hunk 179 failed, because 0043 already edits the
     dispatch line that 0053 appends to
  2. anchored the declarations on 'unix_syscall(' -> that string first occurs
     in the forward declaration 'extern void\\nunix_syscall(', so the block was
     spliced between the return type and the name, and hunk 79 failed

0043 anchors on this exact pair of lines:

    extern int mach_trap_count;
    #endif

and appends its declarations after them. Doing the same guarantees the context
matches whatever the build has when 0053 is applied, because 0043 is already in.
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(r'D:\unixnu-tb710fu-clean')
REPO = ROOT / '.work' / 'UniXNU' / 'out' / 'feeds' / 'xnu-3199e8bc'
WT = ROOT / '.work' / 'xnu-0053-post'
PATCHDIR = ROOT / 'patches' / 'xnu'
OUT = PATCHDIR / '0053-tb710fu-dyld-cache-probe.patch'
TARGET_REL = 'bsd/dev/arm/systemcalls.c'


def run(args, check=True):
    r = subprocess.run(args, capture_output=True, text=True)
    if check and r.returncode != 0:
        print(r.stdout[-1500:], r.stderr[-1500:])
        sys.exit(f'failed: {" ".join(map(str, args))}')
    return r


if WT.exists():
    run(['git', '-C', str(REPO), 'worktree', 'remove', '--force', str(WT)],
        check=False)
run(['git', '-C', str(REPO), 'worktree', 'add', '--detach', str(WT),
     'ac9718f'])

# only 0043 touches systemcalls.c before 0053
r = subprocess.run(['git', '-C', str(WT), 'apply',
                    str(PATCHDIR / '0043-tb710fu-pid1-syscall-ring.patch')],
                   capture_output=True, text=True)
if r.returncode != 0:
    sys.exit(f'0043 failed to apply:\n{r.stderr[:1200]}')
print('applied 0043')

target = WT / TARGET_REL
src = target.read_text(encoding='utf-8')

if 'tb710fu_dyld_probe_put' in src:
    sys.exit('probe already present')

# ---------------------------------------------------------- declarations ---
# same anchor 0043 uses, so the two blocks sit side by side and the hunk
# context is exactly what the build sees
decl_anchor = 'extern int mach_trap_count;\n#endif\n'
if src.count(decl_anchor) != 1:
    sys.exit(f'decl anchor count {src.count(decl_anchor)}')

decls = '''
/*
 * tb710fu: capture the exact arguments of the syscalls launchd loops on while
 * dyld refuses the shared cache. See docs/LAUNCHD_SYSCALL_LOOP.md.
 *
 * Patch 0043 recorded only (code, error), which proved the six-call loop exists
 * and that every call succeeds, but it cannot explain why dyld munmaps after a
 * successful mmap. These records keep the arguments and the return value.
 *
 * Written round-robin as groups of four u64 in a 64-slot ring:
 *   [0] tag, [1] argA, [2] argB, [3] argC
 */
volatile uint64_t tb710fu_dyld_probe[64] = { 0 };
volatile uint32_t tb710fu_dyld_probe_idx = 0;

static inline void
tb710fu_dyld_probe_put(uint32_t tag, uint64_t a, uint64_t b, uint64_t c)
{
\tuint32_t slot = __atomic_fetch_add(&tb710fu_dyld_probe_idx, 4u,
\t    __ATOMIC_RELAXED) & 63u;

\ttb710fu_dyld_probe[slot] = tag;
\ttb710fu_dyld_probe[(slot + 1u) & 63u] = a;
\ttb710fu_dyld_probe[(slot + 2u) & 63u] = b;
\ttb710fu_dyld_probe[(slot + 3u) & 63u] = c;
}
'''
src = src.replace(decl_anchor, decl_anchor + decls, 1)

# ---------------------------------------------------------------- recorder ---
call_anchor = ('error = (*(callp->sy_call))(proc, &uthread->uu_arg[0], '
               '&(uthread->uu_rval[0]));')
n = src.count(call_anchor)
if n != 1:
    sys.exit(f'dispatch anchor count {n}, expected 1')

hook = r'''	/*
	 * tb710fu: full-argument capture for the dyld cache retry loop.
	 * pid 1 only, so nothing else is perturbed.
	 */
	if (pid == 1) {
		switch (code) {
		case 463: /* openat(dirfd, path, flags, mode) */
			tb710fu_dyld_probe_put(0x4F41544Eu,
			    (uint64_t)uthread->uu_arg[1],
			    (uint64_t)uthread->uu_arg[2],
			    (uint64_t)(long)uthread->uu_rval[0]);
			break;
		case 197: /* mmap(addr, len, prot, flags, fd, off) */
			tb710fu_dyld_probe_put(0x4D4D4150u,
			    ((uint64_t)uthread->uu_arg[2] << 32) |
			    ((uint64_t)uthread->uu_arg[3] & 0xffffffffu),
			    (uint64_t)uthread->uu_arg[4],
			    (uint64_t)(long)uthread->uu_rval[0]);
			break;
		case 92: /* fcntl(fd, cmd, arg) */
			tb710fu_dyld_probe_put(0x464E4354u,
			    (uint64_t)uthread->uu_arg[0],
			    (uint64_t)uthread->uu_arg[1],
			    (uint64_t)(long)uthread->uu_rval[0]);
			break;
		case 73: /* munmap(addr, len) */
			tb710fu_dyld_probe_put(0x4D554E4Du,
			    (uint64_t)uthread->uu_arg[0],
			    (uint64_t)uthread->uu_arg[1],
			    (uint64_t)0);
			break;
		default:
			break;
		}
	}

'''
src = src.replace(call_anchor, call_anchor + '\n' + hook, 1)
target.write_text(src, encoding='utf-8')

diff = subprocess.run(['git', '-C', str(WT), 'diff', '--binary'],
                      capture_output=True).stdout
OUT.write_bytes(diff)
print(f'wrote {len(diff)} bytes')

# verify against the same post-0043 state
run(['git', '-C', str(WT), 'checkout', '--', TARGET_REL])
chk = subprocess.run(['git', '-C', str(WT), 'apply', '--check', str(OUT)],
                     capture_output=True, text=True)
print('APPLY_CHECK_PASS' if chk.returncode == 0 else 'APPLY_CHECK_FAIL')
if chk.returncode != 0:
    print(chk.stderr[:1500])
    sys.exit(1)
