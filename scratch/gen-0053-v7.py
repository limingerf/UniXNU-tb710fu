"""
Generate patch 0053 v3: anchor the declaration block on 0043's LAST line, not
on a generic '#endif'.

Failure being fixed (7th build):

    systemcalls.c:224:1: error: function definition is not allowed here
    224 | {
    systemcalls.c:278:4: error: call to undeclared function 'tb710fu_dyld_probe_put'

The emitted hunk was

    @@ -204,6 +204,32 @@ unix_syscall(
            syscalls_log[code]++;
        }
    #endif
    +<our declarations + function definition>

which sits inside unix_syscall's body. My scan looked for the '#endif' that
closes 0043's declaration block, but 'extern int mach_trap_count;' is inside a
'#if CONFIG_DEBUG_SYSCALL_REJECTION' guard, and the first '#endif' after it was
that guard's -- not 0043's.

Now the anchor is 0043's own final declaration, which is unambiguous:

    char tb710fu_pid1_last_open[96] = { 0 };

Insert after that line. It appears once.

The recorder anchor stays 0043's sc_exit '#endif', which is also unambiguous
because it is the only '#endif' between that call and AUDIT_SYSCALL_EXIT.
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(r'D:\unixnu-tb710fu-clean')
TRUTH = ROOT / '.work' / 'build-truth' / 'systemcalls-46116666719.c'
WT = ROOT / '.work' / 'xnu-0053-final'
REPO = ROOT / '.work' / 'UniXNU' / 'out' / 'feeds' / 'xnu-3199e8bc'
PATCHDIR = ROOT / 'patches' / 'xnu'
OUT = PATCHDIR / '0053-tb710fu-dyld-cache-probe.patch'
TARGET_REL = 'bsd/dev/arm/systemcalls.c'

if not TRUTH.exists():
    sys.exit(f'missing ground truth: {TRUTH}')

truth_text = TRUTH.read_text(encoding='utf-8')
for marker in ('tb710fu_pid1_sc_enter', 'tb710fu_pid1_sc_exit'):
    if marker not in truth_text:
        sys.exit(f'ground truth lacks {marker}')
if '\r' in truth_text:
    sys.exit('ground truth contains CR')


def run(args, check=True):
    r = subprocess.run(args, capture_output=True, text=True)
    if check and r.returncode != 0:
        print(r.stdout[-1200:], r.stderr[-1200:])
        sys.exit(f'failed: {" ".join(map(str, args))}')
    return r


if WT.exists():
    run(['git', '-C', str(REPO), 'worktree', 'remove', '--force', str(WT)],
        check=False)
run(['git', '-C', str(REPO), 'worktree', 'add', '--detach', str(WT),
     'ac9718f'])

target = WT / TARGET_REL
target.write_text(truth_text, encoding='utf-8', newline='\n')
run(['git', '-C', str(WT), 'add', TARGET_REL])
run(['git', '-C', str(WT), '-c', 'user.name=probe', '-c', 'user.email=p@x',
     'commit', '-m', 'baseline: systemcalls.c as the build compiles it'])

lines = truth_text.split('\n')

# ---- anchor 1: 0043's last declaration, at file scope --------------------
ANCHOR_DECL = 'char tb710fu_pid1_last_open[96] = { 0 };'
idx = [i for i, l in enumerate(lines) if l.strip() == ANCHOR_DECL]
if len(idx) != 1:
    sys.exit(f'decl anchor count {len(idx)}, expected 1')
decl_at = idx[0]

# prove it is at file scope: the enclosing line must be #if DEVELOPMENT||DEBUG
# and there must be no 'unix_syscall(' definition between it and file start
before = '\n'.join(lines[:decl_at])
if 'unix_syscall(\n' in before and before.rindex('unix_syscall(') > \
        before.find('\n}') if '\n}' in before else True:
    pass  # heuristic only; the real check is the brace scan below
depth = 0
for l in lines[:decl_at]:
    depth += l.count('{') - l.count('}')
if depth != 0:
    sys.exit(f'decl anchor is not at file scope (brace depth {depth})')
print(f'decl anchor at line {decl_at+1}, brace depth 0 (file scope)')

decl_block = '''
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
}'''.split('\n')

lines = lines[:decl_at + 1] + decl_block + lines[decl_at + 1:]

# ---- anchor 2: the recorder, after 0043's sc_exit block ------------------
exit_idx = None
for i, l in enumerate(lines):
    if 'tb710fu_pid1_sc_exit(code, error' in l:
        for j in range(i, min(i + 4, len(lines))):
            if lines[j].strip() == '#endif':
                exit_idx = j
                break
        break
if exit_idx is None:
    sys.exit('sc_exit #endif not found')

# that #endif must be inside unix_syscall, so depth must be 1 there
depth = 0
for l in lines[:exit_idx]:
    depth += l.count('{') - l.count('}')
print(f'sc_exit #endif at line {exit_idx+1}, brace depth {depth} (1 = inside function)')
if depth != 1:
    sys.exit(f'expected brace depth 1 inside unix_syscall, got {depth}')

hook = '''#if DEVELOPMENT || DEBUG
\tif (pid == 1) {
\t\tswitch (code) {
\t\tcase 463: /* openat(dirfd, path, flags, mode) */
\t\t\ttb710fu_dyld_probe_put(0x4F41544Eu,
\t\t\t    (uint64_t)uthread->uu_arg[1],
\t\t\t    (uint64_t)uthread->uu_arg[2],
\t\t\t    (uint64_t)(long)uthread->uu_rval[0]);
\t\t\tbreak;
\t\tcase 197: /* mmap(addr, len, prot, flags, fd, off) */
\t\t\ttb710fu_dyld_probe_put(0x4D4D4150u,
\t\t\t    ((uint64_t)uthread->uu_arg[2] << 32) |
\t\t\t    ((uint64_t)uthread->uu_arg[3] & 0xffffffffu),
\t\t\t    (uint64_t)uthread->uu_arg[4],
\t\t\t    (uint64_t)(long)uthread->uu_rval[0]);
\t\t\tbreak;
\t\tcase 92: /* fcntl(fd, cmd, arg) */
\t\t\ttb710fu_dyld_probe_put(0x464E4354u,
\t\t\t    (uint64_t)uthread->uu_arg[0],
\t\t\t    (uint64_t)uthread->uu_arg[1],
\t\t\t    (uint64_t)(long)uthread->uu_rval[0]);
\t\t\tbreak;
\t\tcase 73: /* munmap(addr, len) */
\t\t\ttb710fu_dyld_probe_put(0x4D554E4Du,
\t\t\t    (uint64_t)uthread->uu_arg[0],
\t\t\t    (uint64_t)uthread->uu_arg[1],
\t\t\t    (uint64_t)0);
\t\t\tbreak;
\t\tdefault:
\t\t\tbreak;
\t\t}
\t}
#endif'''.split('\n')

lines = lines[:exit_idx + 1] + hook + lines[exit_idx + 1:]
result = '\n'.join(lines)

# ---- structural assertions ----------------------------------------------
assert 'volatile uint64_t tb710fu_dyld_probe[64]' in result
assert 'tb710fu_dyld_probe_put(0x4F41544Eu' in result

# the declaration block must be BEFORE the unix_syscall definition
def_pos = result.index('volatile uint64_t tb710fu_dyld_probe[64]')
defn_pos = result.index('unix_syscall(\n\tstruct arm_saved_state')
assert def_pos < defn_pos, 'declarations ended up after the function'
print('ordering ok: declarations precede the unix_syscall definition')

# brace depth of the decl block position must be 0
depth = 0
for l in result[:def_pos].split('\n'):
    depth += l.count('{') - l.count('}')
assert depth == 0, f'decl block depth {depth}, expected 0'
print('decl block at file scope')

target.write_text(result, encoding='utf-8', newline='\n')

diff = subprocess.run(['git', '-C', str(WT), 'diff', '--binary'],
                      capture_output=True).stdout
OUT.write_bytes(diff)

d = OUT.read_text(errors='replace').splitlines()
plus = sum(1 for l in d if l.startswith('+') and not l.startswith('+++'))
minus = sum(1 for l in d if l.startswith('-') and not l.startswith('---'))
print(f'\npatch: {len(diff)} bytes, {plus} added, {minus} removed')
for l in d:
    if l.startswith('@@'):
        print('  ' + l)
if minus:
    sys.exit(f'REFUSING: deletes {minus} lines')

run(['git', '-C', str(WT), 'checkout', '--', TARGET_REL])
chk = subprocess.run(['git', '-C', str(WT), 'apply', '--check', str(OUT)],
                     capture_output=True, text=True)
print('APPLY_CHECK_PASS' if chk.returncode == 0 else 'APPLY_CHECK_FAIL')
if chk.returncode != 0:
    print(chk.stderr[:1000])
    sys.exit(1)
