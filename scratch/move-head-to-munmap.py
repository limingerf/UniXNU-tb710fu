"""
Move the head capture from the mmap arm to the munmap arm.

Reason: in the mmap arm the page was created microseconds earlier and may not be
resident, so copyin would fault it in from inside the syscall exit path for pid 1.
In the munmap arm the mapping has been in use, so pages dyld touched are resident,
and a failure there simply means "nothing resident to show" rather than "we
disturbed the mapping we are measuring".

Also: munmap's first argument is the address, so the capture does not depend on
re-reading uu_rval.

This keeps the same globals and the same dump format; only the hook point moves.
"""
import pathlib
import sys

P = pathlib.Path(r'D:\unixnu-tb710fu-clean\scratch\gen-0053-v7.py')
src = P.read_text(encoding='utf-8')

# ---- remove the capture from the mmap arm --------------------------------
mmap_block = (r'\t\t\t{' + '\n'
              r'\t\t\t\tuint64_t mp = (uint64_t)(uint32_t)uthread->uu_rval[0] |' + '\n'
              r'\t\t\t\t    ((uint64_t)(uint32_t)uthread->uu_rval[1] << 32);' + '\n'
              r'\t\t\t\tuint8_t head[32];' + '\n'
              r'' + '\n'
              r'\t\t\t\tif (mp && copyin((user_addr_t)mp, head,' + '\n'
              r'\t\t\t\t    sizeof(head)) == 0) {' + '\n'
              r'\t\t\t\t\tfor (uint32_t k = 0; k < sizeof(head); ++k) {' + '\n'
              r'\t\t\t\t\t\ttb710fu_dyld_probe_head[k] = head[k];' + '\n'
              r'\t\t\t\t\t}' + '\n'
              r'\t\t\t\t\ttb710fu_dyld_probe_head_ok = 1;' + '\n'
              r'\t\t\t\t} else {' + '\n'
              r'\t\t\t\t\ttb710fu_dyld_probe_head_ok = 0;' + '\n'
              r'\t\t\t\t}' + '\n'
              r'\t\t\t}' + '\n')

if mmap_block not in src:
    sys.exit('mmap capture block not found verbatim')

src = src.replace(mmap_block, '', 1)
print('removed capture from the mmap arm')

# ---- add it to the munmap arm -------------------------------------------
# the munmap arm currently records the call then breaks
munmap_anchor = (r'\t\tcase 73: /* munmap(addr, len) */' + '\n'
                 r'\t\t\ttb710fu_dyld_probe_put("MUNM",' + '\n'
                 r'\t\t\t    (uint64_t)uthread->uu_arg[0],' + '\n'
                 r'\t\t\t    (uint64_t)uthread->uu_arg[1],' + '\n'
                 r'\t\t\t    (uint64_t)0);' + '\n'
                 r'\t\t\tbreak;')

if munmap_anchor not in src:
    sys.exit('munmap arm anchor not found')

# capture from arg0 (the address being unmapped) after recording, so the page
# has been in use and the call itself is already logged
munmap_new = (r'\t\tcase 73: /* munmap(addr, len) */' + '\n'
              r'\t\t\ttb710fu_dyld_probe_put("MUNM",' + '\n'
              r'\t\t\t    (uint64_t)uthread->uu_arg[0],' + '\n'
              r'\t\t\t    (uint64_t)uthread->uu_arg[1],' + '\n'
              r'\t\t\t    (uint64_t)0);' + '\n'
              r'\t\t\t{' + '\n'
              r'\t\t\t\tuint64_t ua = (uint64_t)uthread->uu_arg[0];' + '\n'
              r'\t\t\t\tuint8_t head[32];' + '\n'
              r'' + '\n'
              r'\t\t\t\t/* Read the mapping before it goes away. Sampling here' + '\n'
              r'\t\t\t\t * rather than in the mmap arm means the page has' + '\n'
              r'\t\t\t\t * usually been touched already, so copyin is unlikely' + '\n'
              r'\t\t\t\t * to fault inside the syscall-exit path. */' + '\n'
              r'\t\t\t\tif (ua && copyin((user_addr_t)ua, head,' + '\n'
              r'\t\t\t\t    sizeof(head)) == 0) {' + '\n'
              r'\t\t\t\t\tfor (uint32_t k = 0; k < sizeof(head); ++k) {' + '\n'
              r'\t\t\t\t\t\ttb710fu_dyld_probe_head[k] = head[k];' + '\n'
              r'\t\t\t\t\t}' + '\n'
              r'\t\t\t\t\ttb710fu_dyld_probe_head_ok = 1;' + '\n'
              r'\t\t\t\t} else {' + '\n'
              r'\t\t\t\t\ttb710fu_dyld_probe_head_ok = 0;' + '\n'
              r'\t\t\t\t}' + '\n'
              r'\t\t\t}' + '\n'
              r'\t\t\tbreak;')

src = src.replace(munmap_anchor, munmap_new, 1)
print('added capture to the munmap arm')

P.write_text(src, encoding='utf-8')

for i, line in enumerate(src.split('\n'), 1):
    s = line.strip()
    if 'probe_head' in s or 'copyin((user_addr_t)ua' in s:
        print(f'{i:4}: {s[:80]}')
