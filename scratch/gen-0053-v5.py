"""
Regenerate patch 0053 by pure insertion into the CRLF file, verified to touch
only two spots.

History of failures:
  1. bare feed diff          -> hunk 179 rejected (0043 already edits there)
  2. anchor 'unix_syscall('   -> hunk 79 rejected (matched forward decl)
  3. anchor with '\\n'        -> anchor never matched CRLF, so only the recorder
                                 hunk was emitted against unmodified context
  4. line-join rewrite        -> 713+/584- full-file rewrite, which would have
                                 clobbered 0043's hooks

This version does byte-level splicing: read the file as bytes, locate the exact
CRLF-terminated anchor byte sequences, insert the new bytes, write back. No
line splitting, no join, no normalisation.
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
r = subprocess.run(['git', '-C', str(WT), 'apply',
                    str(PATCHDIR / '0043-tb710fu-pid1-syscall-ring.patch')],
                   capture_output=True, text=True)
if r.returncode != 0:
    sys.exit(f'0043 failed:\n{r.stderr[:1000]}')
print('applied 0043')

target = WT / TARGET_REL
data = target.read_bytes()

# ------------------------------------------------------------ anchor 1 ------
# the '#endif' that patch 0043 appends its block after
decl_anchor = b'extern int mach_trap_count;\r\n#endif\r\n'
if data.count(decl_anchor) != 1:
    sys.exit(f'decl anchor count {data.count(decl_anchor)}, expected 1')

decl_text = (
    '\r\n'
    '/*\r\n'
    ' * tb710fu: capture the exact arguments of the syscalls launchd loops on\r\n'
    ' * while dyld refuses the shared cache. See docs/LAUNCHD_SYSCALL_LOOP.md.\r\n'
    ' *\r\n'
    ' * Patch 0043 recorded only (code, error), which proved the six-call loop\r\n'
    ' * exists and that every call succeeds, but it cannot explain why dyld\r\n'
    ' * munmaps after a successful mmap. These records keep the arguments.\r\n'
    ' */\r\n'
    'volatile uint64_t tb710fu_dyld_probe[64] = { 0 };\r\n'
    'volatile uint32_t tb710fu_dyld_probe_idx = 0;\r\n'
    '\r\n'
    'static inline void\r\n'
    'tb710fu_dyld_probe_put(uint32_t tag, uint64_t a, uint64_t b, uint64_t c)\r\n'
    '{\r\n'
    '\tuint32_t slot = __atomic_fetch_add(&tb710fu_dyld_probe_idx, 4u,\r\n'
    '\t    __ATOMIC_RELAXED) & 63u;\r\n'
    '\r\n'
    '\ttb710fu_dyld_probe[slot] = tag;\r\n'
    '\ttb710fu_dyld_probe[(slot + 1u) & 63u] = a;\r\n'
    '\ttb710fu_dyld_probe[(slot + 2u) & 63u] = b;\r\n'
    '\ttb710fu_dyld_probe[(slot + 3u) & 63u] = c;\r\n'
    '}\r\n'
).encode('utf-8')

data = data.replace(decl_anchor, decl_anchor + decl_text, 1)

# ------------------------------------------------------------ anchor 2 ------
call_anchor = (b'error = (*(callp->sy_call))(proc, &uthread->uu_arg[0], '
               b'&(uthread->uu_rval[0]));\r\n')
if data.count(call_anchor) != 1:
    sys.exit(f'dispatch anchor count {data.count(call_anchor)}, expected 1')

hook_text = (
    '\t/*\r\n'
    '\t * tb710fu: full-argument capture for the dyld cache retry loop.\r\n'
    '\t * pid 1 only, so nothing else is perturbed.\r\n'
    '\t */\r\n'
    '\tif (pid == 1) {\r\n'
    '\t\tswitch (code) {\r\n'
    '\t\tcase 463: /* openat(dirfd, path, flags, mode) */\r\n'
    '\t\t\ttb710fu_dyld_probe_put(0x4F41544Eu,\r\n'
    '\t\t\t    (uint64_t)uthread->uu_arg[1],\r\n'
    '\t\t\t    (uint64_t)uthread->uu_arg[2],\r\n'
    '\t\t\t    (uint64_t)(long)uthread->uu_rval[0]);\r\n'
    '\t\t\tbreak;\r\n'
    '\t\tcase 197: /* mmap(addr, len, prot, flags, fd, off) */\r\n'
    '\t\t\ttb710fu_dyld_probe_put(0x4D4D4150u,\r\n'
    '\t\t\t    ((uint64_t)uthread->uu_arg[2] << 32) |\r\n'
    '\t\t\t    ((uint64_t)uthread->uu_arg[3] & 0xffffffffu),\r\n'
    '\t\t\t    (uint64_t)uthread->uu_arg[4],\r\n'
    '\t\t\t    (uint64_t)(long)uthread->uu_rval[0]);\r\n'
    '\t\t\tbreak;\r\n'
    '\t\tcase 92: /* fcntl(fd, cmd, arg) */\r\n'
    '\t\t\ttb710fu_dyld_probe_put(0x464E4354u,\r\n'
    '\t\t\t    (uint64_t)uthread->uu_arg[0],\r\n'
    '\t\t\t    (uint64_t)uthread->uu_arg[1],\r\n'
    '\t\t\t    (uint64_t)(long)uthread->uu_rval[0]);\r\n'
    '\t\t\tbreak;\r\n'
    '\t\tcase 73: /* munmap(addr, len) */\r\n'
    '\t\t\ttb710fu_dyld_probe_put(0x4D554E4Du,\r\n'
    '\t\t\t    (uint64_t)uthread->uu_arg[0],\r\n'
    '\t\t\t    (uint64_t)uthread->uu_arg[1],\r\n'
    '\t\t\t    (uint64_t)0);\r\n'
    '\t\t\tbreak;\r\n'
    '\t\tdefault:\r\n'
    '\t\t\tbreak;\r\n'
    '\t\t}\r\n'
    '\t}\r\n'
).encode('utf-8')

data = data.replace(call_anchor, call_anchor + hook_text, 1)
target.write_bytes(data)

# ------------------------------------------------------------- emit --------
diff = subprocess.run(['git', '-C', str(WT), 'diff', '--binary'],
                      capture_output=True).stdout
OUT.write_bytes(diff)

d = OUT.read_text(errors='replace').splitlines()
plus = sum(1 for l in d if l.startswith('+') and not l.startswith('+++'))
minus = sum(1 for l in d if l.startswith('-') and not l.startswith('---'))
print(f'patch: {len(diff)} bytes, {plus} added, {minus} removed')
for l in d:
    if l.startswith('@@'):
        print('  ' + l)

if minus != 0:
    sys.exit(f'REFUSING: patch deletes {minus} lines; it must only insert')

run(['git', '-C', str(WT), 'checkout', '--', TARGET_REL])
chk = subprocess.run(['git', '-C', str(WT), 'apply', '--check', str(OUT)],
                     capture_output=True, text=True)
print('APPLY_CHECK_PASS' if chk.returncode == 0 else 'APPLY_CHECK_FAIL')
if chk.returncode != 0:
    print(chk.stderr[:1200])
    sys.exit(1)
