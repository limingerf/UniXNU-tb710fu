"""Append the mmap-head capture using anchors that match the generator's
literal backslash-t form, not real tabs."""
import pathlib
import sys

P = pathlib.Path(r'D:\unixnu-tb710fu-clean\scratch\gen-0053-v7.py')
src = P.read_text(encoding='utf-8')

if 'tb710fu_dyld_probe_head' in src:
    sys.exit('mapping-content capture already present')

# ---- 1. storage ----------------------------------------------------------
ring_decl = "volatile uint32_t tb710fu_dyld_probe_idx = 0;"
if ring_decl not in src:
    sys.exit('ring declaration anchor not found')

extra = ring_decl + """
/*
 * First 32 bytes of the most recent successful mmap, captured while the mapping
 * is still live (the matching munmap follows immediately). Compared against the
 * file header read with cat: a difference would prove the 4K-page mapping does
 * not expose the bytes the 16K-page cache layout describes.
 */
volatile uint8_t tb710fu_dyld_probe_head[32] = { 0 };
volatile uint32_t tb710fu_dyld_probe_head_ok = 0;"""
src = src.replace(ring_decl, extra, 1)

# ---- 2. capture inside the mmap case -------------------------------------
# anchors use the generator's literal backslash-t sequences
old = (r'\t\t\t    ((uint64_t)(uint32_t)uthread->uu_rval[0] | '
       r'((uint64_t)(uint32_t)uthread->uu_rval[1] << 32)));' + '\n'
       r'\t\t\tbreak;' + '\n'
       r'\t\tcase 92: /* fcntl(fd, cmd, arg) */')

if old not in src:
    sys.exit('mmap anchor (literal-backslash form) not found')

new = (r'\t\t\t    ((uint64_t)(uint32_t)uthread->uu_rval[0] | '
       r'((uint64_t)(uint32_t)uthread->uu_rval[1] << 32)));' + '\n'
       r'\t\t\t{' + '\n'
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
       r'\t\t\t}' + '\n'
       r'\t\t\tbreak;' + '\n'
       r'\t\tcase 92: /* fcntl(fd, cmd, arg) */')

src = src.replace(old, new, 1)
P.write_text(src, encoding='utf-8')
print('mapping-head capture added to the generator')

for i, line in enumerate(src.split('\n'), 1):
    s = line.strip()
    if 'probe_head' in s or 'copyin((user_addr_t)mp' in s:
        print(f'{i:4}: {s[:82]}')
