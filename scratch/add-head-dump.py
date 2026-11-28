"""Add the head dump to BootWatchdogPolicy.c so the captured mapping bytes are
printed alongside the ring."""
import pathlib
import sys

F = pathlib.Path(r'D:\unixnu-tb710fu-clean\package\BootWatchdogPolicy.c')
src = F.read_text(encoding='utf-8')

if 'tb710fu_dyld_probe_head' in src:
    sys.exit('head dump already present')

# ---- weak declarations next to the existing probe ones -------------------
anchor = ('extern volatile uint32_t tb710fu_dyld_probe_idx '
          '__attribute__((weak_import));')
if anchor not in src:
    sys.exit('probe decl anchor not found')

decls = anchor + '''
extern volatile uint8_t tb710fu_dyld_probe_head[32] __attribute__((weak_import));
extern volatile uint32_t tb710fu_dyld_probe_head_ok __attribute__((weak_import));'''
src = src.replace(anchor, decls, 1)

# ---- dump block, just before the ring terminator -------------------------
term = '        printf("TB710FU DYLDPROBE: end\\n");'
if term not in src:
    sys.exit('ring terminator not found')

head_dump = '''        if (&tb710fu_dyld_probe_head_ok && tb710fu_dyld_probe_head_ok) {
            /* First 32 bytes of the most recent mmap. Compare with the file
             * header read via cat: 'dyld_v1  arm64e' should appear at offset
             * 0, mappingOffset at 0x10 and mappingCount at 0x14. */
            printf("TB710FU DYLDPROBE HEAD:");
            for (uint32_t k = 0; k < 32u; ++k) {
                printf(" %02x", (unsigned)tb710fu_dyld_probe_head[k]);
            }
            printf("\\n");
            printf("TB710FU DYLDPROBE HEADASCII: %c%c%c%c%c%c%c%c\\n",
                   (char)tb710fu_dyld_probe_head[0],
                   (char)tb710fu_dyld_probe_head[1],
                   (char)tb710fu_dyld_probe_head[2],
                   (char)tb710fu_dyld_probe_head[3],
                   (char)tb710fu_dyld_probe_head[4],
                   (char)tb710fu_dyld_probe_head[5],
                   (char)tb710fu_dyld_probe_head[6],
                   (char)tb710fu_dyld_probe_head[7]);
        } else {
            printf("TB710FU DYLDPROBE HEAD: none\\n");
        }
'''
src = src.replace(term, head_dump + term, 1)

F.write_text(src, encoding='utf-8')
print('head dump added to tb710fu_dump_procs()')

for i, line in enumerate(src.split('\n'), 1):
    s = line.strip()
    if 'DYLDPROBE HEAD' in s or 'tb710fu_dyld_probe_head' in s:
        print(f'{i:4}: {s[:78]}')
