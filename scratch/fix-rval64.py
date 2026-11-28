"""
Fix a real bug the first hardware run exposed, then regenerate the patch.

Evidence (docs/DYLDPROBE_READING.md round 50, pl109 on hardware):

    MMAP a=0x500040002 b=0x1 c=0x9a0000
    MUNM a=0x1009a0000 b=0x4000 c=0x0

mmap's recorded return (0x9a0000) and the address dyld unmaps (0x1009a0000)
differ by exactly 0x100000000, and the gap is constant across all six groups.
That is not dyld behaviour, it is my recorder reading half of a 64-bit value.

bsd/sys/user.h declares:

    int uu_rval[2];

and bsd/dev/arm/systemcalls.c writes a 64-bit syscall result as two halves:

    ss64->x[0] = uthread->uu_rval[0];   /* low  32 */
    ss64->x[1] = uthread->uu_rval[1];   /* high 32 */

The recorder captured only uu_rval[0]. Reassembling 0x9a0000 | (0x1 << 32) gives
exactly 0x1009a0000 -- the address munmap received. So the data was right and the
reader was wrong.

This changes the recorder to pass the full 64-bit return by combining both
halves, and keeps the raw halves in argC's upper bits so nothing is lost if the
sign/size convention turns out to differ for some syscall.
"""
import pathlib
import sys

P = pathlib.Path(r'D:\unixnu-tb710fu-clean\scratch\gen-0053-v7.py')
src = P.read_text(encoding='utf-8')

if 'rval64' in src:
    sys.exit('rval64 already present in the generator')

# ---- update the four call sites -------------------------------------------
# each currently reads (uint64_t)(long)uthread->uu_rval[0]
old = '(uint64_t)(long)uthread->uu_rval[0]'
n = src.count(old)
# munmap passes a literal 0, so only openat/mmap/fcntl read uu_rval
if n != 3:
    sys.exit(f'expected 3 return-value reads, found {n}')

new = ('((uint64_t)(uint32_t)uthread->uu_rval[0] | '
       '((uint64_t)(uint32_t)uthread->uu_rval[1] << 32))')
src = src.replace(old, new)

# ---- document the encoding in the recorder comment -----------------------
marker = ' * Written round-robin as groups of four u64 in a 64-slot ring:'
if marker not in src:
    sys.exit('comment anchor not found')

note = ''' *
 * Return values are 64-bit and the kernel splits them across uu_rval[0] and
 * uu_rval[1] (see the ss64 save path in this same file), so both halves are
 * combined. Reading only uu_rval[0] reported mmap's 0x1009a0000 as 0x9a0000,
 * which looked like dyld unmapping a different address than it mapped.
'''
src = src.replace(marker, note + marker, 1)

P.write_text(src, encoding='utf-8')
print(f'rewrote {n} return-value reads to combine both halves')

for i, line in enumerate(src.split('\n'), 1):
    s = line.strip()
    if 'uu_rval[1] << 32' in s or 'both halves' in s:
        print(f'{i:4}: {s[:80]}')
