"""
Inject the dyld-cache probe into a clean xnu worktree (patch 0053).

Hook site: bsd/dev/arm/systemcalls.c, immediately after
    error = (*(callp->sy_call))(proc, &uthread->uu_arg[0], &(uthread->uu_rval[0]));
and before AUDIT_SYSCALL_EXIT. That is exactly where patch 0043 hooks its
(weaker) recorder, so the context is proven to compile.

What it adds, for pid 1 only, per syscall of interest:
  openat(463) path, flags, returned fd
  mmap(197)   (prot<<32|flags), fd, returned address
  fcntl(92)   fd, cmd, return value      <- F_ADDFILESIGS_RETURN == 0x61
  munmap(73)  addr, len, error

Round 27-32 (docs/LAUNCHD_SYSCALL_LOOP.md) proved launchd lives in
    openat -> fstat64 -> pread -> fcntl(0x61) -> mmap -> munmap -> repeat
with every call returning 0, and shared_region_map_and_slide_2_np never
being invoked. The unresolved question is why dyld munmaps after a
successful mmap; that needs the arguments, which 0043 threw away.
"""
import pathlib
import sys

WT = pathlib.Path(r'D:\unixnu-tb710fu-clean\.work\xnu-0053-clean')
target = WT / 'bsd' / 'dev' / 'arm' / 'systemcalls.c'

if not target.exists():
    sys.exit(f'missing {target}')

src = target.read_text()

if 'tb710fu_dyld_probe_put' in src:
    sys.exit('probe already present in this worktree')

# ---------------------------------------------------------------- 1. decls ---
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

# ---------------------------------------------------------------- 2. hook ----
call_anchor = 'error = (*(callp->sy_call))(proc, &uthread->uu_arg[0], &(uthread->uu_rval[0]));'
if call_anchor not in src:
    sys.exit('cannot find the sy_call dispatch line')

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
target.write_text(src)
print(f'patched {target}')
