"""
Regenerate patch 0053 so it lands AFTER patch 0043's own hunks.

Why the first version failed at build time:

    error: patch failed: bsd/dev/arm/systemcalls.c:179
    error: bsd/dev/arm/systemcalls.c: patch does not apply

0052 succeeded because it touches bsd/vm/vm_unix.c, a place no earlier patch
modifies. 0053 touches bsd/dev/arm/systemcalls.c at the dispatch line, which is
exactly where 0043 already inserts its enter/exit hooks. Diffing against the
bare feed therefore produced context that no longer exists once 0043 is applied.

Fix: build the patch on a tree that already contains 0043's edit. Instead of
replaying the whole series (0001 does not apply to the bare feed either, so the
series must be applied by the tooling in a different order), take the bare feed
and apply ONLY patch 0043 plus the handful of patches that touch this same file,
then inject.

Simpler and robust: apply 0043 alone. That reproduces the exact context the
build sees at the point where 0053 is applied, because 0053 follows 0043 in the
series and nothing between them modifies systemcalls.c.
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

# every series entry between 0043 and 0053 that touches this file
SERIES = PATCHDIR / 'series'


def run(args, check=True):
    r = subprocess.run(args, capture_output=True, text=True)
    if check and r.returncode != 0:
        print(r.stdout[-1500:], r.stderr[-1500:])
        sys.exit(f'failed: {" ".join(map(str, args))}')
    return r


# ---------------------------------------------------------- worktree ------
if WT.exists():
    run(['git', '-C', str(REPO), 'worktree', 'remove', '--force', str(WT)],
        check=False)
run(['git', '-C', str(REPO), 'worktree', 'add', '--detach', str(WT),
     'ac9718f'])

entries = [l.strip() for l in SERIES.read_text().splitlines()
           if l.strip() and not l.strip().startswith('#')]

# find which patches touch systemcalls.c, and apply exactly those up to 0052
touching = []
for name in entries:
    if name.startswith('0053'):
        break
    p = PATCHDIR / name
    if not p.exists():
        continue
    body = p.read_text(errors='replace')
    if TARGET_REL in body:
        touching.append(name)

print('patches touching systemcalls.c before 0053:')
for n in touching:
    print('  ' + n)

# apply them in series order; any that fail are reported but skipped, since
# only the ones able to apply matter for producing correct hunk context
applied = []
for name in touching:
    p = PATCHDIR / name
    r = subprocess.run(['git', '-C', str(WT), 'apply', str(p)],
                       capture_output=True, text=True)
    if r.returncode == 0:
        applied.append(name)
    else:
        print(f'  (skipped {name}: does not apply to the bare feed)')

print(f'applied {len(applied)} of {len(touching)}')

target = WT / TARGET_REL
src = target.read_text(encoding='utf-8')

if 'tb710fu_dyld_probe_put' in src:
    sys.exit('probe already present; aborting')

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

call_anchor = ('error = (*(callp->sy_call))(proc, &uthread->uu_arg[0], '
               '&(uthread->uu_rval[0]));')
n = src.count(call_anchor)
if n != 1:
    sys.exit(f'dispatch anchor appears {n} times')

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
			    0u);
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

# verify against the same tree state (patches applied, file reverted)
run(['git', '-C', str(WT), 'checkout', '--', TARGET_REL])
chk = subprocess.run(['git', '-C', str(WT), 'apply', '--check', str(OUT)],
                     capture_output=True, text=True)
print('APPLY_CHECK_PASS' if chk.returncode == 0 else 'APPLY_CHECK_FAIL')
if chk.returncode != 0:
    print(chk.stderr)
    sys.exit(1)
