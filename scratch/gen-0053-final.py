"""
Generate patch 0053 against the file the build actually compiles.

The ground truth came from the run artifact (patched-sources/systemcalls.c),
extracted by scratch/get-truth-source.py. It differs from every earlier
assumption:

  * line endings are LF, not CRLF
  * patch 0043's blocks are present:
      lines 82-...   '#if DEVELOPMENT || DEBUG' + the declaration ring
      lines 238-241  sc_enter before the dispatch call
      lines 243-247  sc_exit right after it
  * the declaration anchor 'extern int mach_trap_count;' + '#endif' is followed
    by a blank line and then 0043's block, not by the function comment

So the two insertion points are:

  1. a fresh block AFTER 0043's declaration block (before its closing #endif
     is not safe -- use the whole '#if DEVELOPMENT || DEBUG' prelude boundary)
  2. the recorder after 0043's own 'sc_exit' block, so both hooks sit together
     before AUDIT_SYSCALL_EXIT

This script is intentionally defensive: it asserts each anchor is unique and
that the file looks like the artifact, then emits a patch that only inserts.
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

# ---- sanity: this really is the artifact ---------------------------------
for marker in ('tb710fu_pid1_sc_enter', 'tb710fu_pid1_sc_exit'):
    if marker not in truth_text:
        sys.exit(f'ground truth lacks {marker}; wrong file')
if '\r' in truth_text:
    sys.exit('ground truth contains CR -- it is LF here')

# ---- build a worktree whose systemcalls.c equals the truth ----------------
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

# overwrite the target with the exact bytes the build compiles, then commit it
# as the baseline so `git diff` shows only our two insertions
target = WT / TARGET_REL
target.write_text(truth_text, encoding='utf-8', newline='\n')
run(['git', '-C', str(WT), 'add', TARGET_REL])
run(['git', '-C', str(WT), '-c', 'user.name=probe', '-c', 'user.email=p@x',
     'commit', '-m', 'baseline: systemcalls.c as the build compiles it'])
print('baseline committed')

# ---- insertion 1: our declarations, right after 0043's block --------------
#
# 0043 emits, in order:
#   #if DEVELOPMENT || DEBUG
#   ... its volatile declarations ...
#   static inline void tb710fu_pid1_sc_enter(...)
#   ...
#   static inline void tb710fu_pid1_sc_exit(...)
#   ...
#   #endif
#
# Find the '#endif' that closes that block by scanning from the '#if' that
# follows 'extern int mach_trap_count;'.
decl_lines = truth_text.split('\n')
start = None
for i, line in enumerate(decl_lines):
    if line.strip() == 'extern int mach_trap_count;':
        for j in range(i, min(i + 5, len(decl_lines))):
            if decl_lines[j].strip() == '#endif':
                start = j
                break
        break
if start is None:
    sys.exit('decl anchor not found')

open_if = None
for j in range(start, len(decl_lines)):
    if decl_lines[j].strip() == '#if DEVELOPMENT || DEBUG':
        open_if = j
        break
if open_if is None:
    sys.exit('0043 declaration block not found')

close_endif = None
for j in range(open_if + 1, len(decl_lines)):
    if decl_lines[j].strip() == '#endif':
        close_endif = j
        break
if close_endif is None:
    sys.exit('0043 declaration block has no closing #endif')

print(f'0043 decl block: #if at line {open_if+1}, #endif at line {close_endif+1}')

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

new_lines = decl_lines[:close_endif + 1] + decl_block + decl_lines[close_endif + 1:]

# ---- insertion 2: the recorder, right after 0043's sc_exit block ---------
# 0043 puts:
#   error = (*(callp->sy_call))( ... );
#   #if DEVELOPMENT || DEBUG
#       if (pid == 1) { tb710fu_pid1_sc_exit(...); }
#   #endif
#   AUDIT_SYSCALL_EXIT(...)
#
# Insert after that '#endif', before AUDIT_SYSCALL_EXIT.
exit_endif = None
for i, line in enumerate(new_lines):
    if 'tb710fu_pid1_sc_exit(code, error' in line:
        for j in range(i, min(i + 4, len(new_lines))):
            if new_lines[j].strip() == '#endif':
                exit_endif = j
                break
        break
if exit_endif is None:
    sys.exit('0043 exit block not found')

print(f'0043 exit block: #endif at line {exit_endif+1}')

hook_block = '''#if DEVELOPMENT || DEBUG
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

new_lines = new_lines[:exit_endif + 1] + hook_block + new_lines[exit_endif + 1:]

result = '\n'.join(new_lines)
assert 'tb710fu_dyld_probe_put(0x4F41544Eu' in result
assert 'volatile uint64_t tb710fu_dyld_probe[64]' in result
assert result.count('tb710fu_pid1_sc_exit(code, error') == 1

target.write_text(result, encoding='utf-8', newline='\n')

# ---- emit ---------------------------------------------------------------
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

if minus != 0:
    sys.exit(f'REFUSING: deletes {minus} lines; must only insert')

# verify by reverting and re-applying against the same baseline
run(['git', '-C', str(WT), 'checkout', '--', TARGET_REL])
chk = subprocess.run(['git', '-C', str(WT), 'apply', '--check', str(OUT)],
                     capture_output=True, text=True)
print('APPLY_CHECK_PASS' if chk.returncode == 0 else 'APPLY_CHECK_FAIL')
if chk.returncode != 0:
    print(chk.stderr[:1200])
    sys.exit(1)
