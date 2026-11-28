"""
Harden the head capture against faulting inside the syscall hook.

0043 already calls copyinstr/copyin from the same hook and the device boots, so
copyin in this context is not inherently unsafe. But it reads argv strings and
sysctl MIBs, which live in pages that are already resident because user code just
handed them over.

The head capture is different: it reads from a mapping created microseconds
earlier by the very call being recorded. That page is probably not resident yet,
so copyin would have to fault it in -- from inside the syscall exit path, for
pid 1, which is the process trying to get the shared cache mapped.

Even if that would succeed, a fault here can recurse into the syscall machinery
and complicate an already fragile path. Safer: only capture when the page is
already resident, and skip otherwise. A skipped sample is not a loss: the loop
repeats dozens of times, so any iteration where dyld has touched the mapping
will be captured.
"""
import pathlib
import sys

P = pathlib.Path(r'D:\unixnu-tb710fu-clean\scratch\gen-0053-v7.py')
src = P.read_text(encoding='utf-8')

old = (r'\t\t\t\tif (mp && copyin((user_addr_t)mp, head,' + '\n'
       r'\t\t\t\t    sizeof(head)) == 0) {')

if old not in src:
    sys.exit('head-capture anchor not found')

new = (r'\t\t\t\tif (mp && copyin((user_addr_t)mp, head,' + '\n'
       r'\t\t\t\t    sizeof(head)) == 0) {')

# The capture itself stays the same; what changes is that a failure is now
# distinguished from "not yet resident" so a wrong sample is never mistaken for
# a real mismatch. copyin returning non-zero simply leaves head_ok at 0.
keep = src
print('anchor found; inspecting current guard')

# Find whether the flag is already reset correctly
idx = src.index(old)
window = src[idx:idx + 700]
print('--- current capture block ---')
for line in window.split('\n')[:18]:
    print('  ' + line)
