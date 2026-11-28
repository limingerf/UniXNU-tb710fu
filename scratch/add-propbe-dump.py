"""
Add the dyld-cache probe dump to BootWatchdogPolicy.c's tb710fu_dump_procs().

Why here and not CdcXnu.c: this file already references
'tb710fu_pid1_sc_ring[16]' through weak_import and links fine, so the pattern is
proven in this translation unit. Two attempts to do the same in CdcXnu.c both
failed with

    Undefined symbols for architecture arm64e.kernel: "_tb710fu_dyld_probe"

Riding on 'ps' also means no new verb and no new link surface: the data appears
in the same dump that already reports the PID1 syscall ring.
"""
import pathlib
import sys

F = pathlib.Path(r'D:\unixnu-tb710fu-clean\package\BootWatchdogPolicy.c')
src = F.read_text(encoding='utf-8')

if 'tb710fu_dyld_probe_idx' in src:
    sys.exit('probe dump already present')

# ---- 1. weak declarations, next to the existing ring declarations --------
decl_anchor = ('extern volatile uint32_t tb710fu_pid1_sc_ring[16] '
               '__attribute__((weak_import));\n')
if src.count(decl_anchor) != 1:
    sys.exit(f'decl anchor count {src.count(decl_anchor)}')

decls = (
    '/* patch 0053: full-argument ring for the dyld cache retry loop. */\n'
    'extern volatile uint64_t tb710fu_dyld_probe[64] '
    '__attribute__((weak_import));\n'
    'extern volatile uint32_t tb710fu_dyld_probe_idx '
    '__attribute__((weak_import));\n'
)
src = src.replace(decl_anchor, decl_anchor + decls, 1)

# ---- 2. dump block, after the existing PID1 RING printf ------------------
dump_anchor = (
    '        printf("\\n");\n'
    '    }\n'
    '    for (int pid = 1; pid <= 256; ++pid) {\n'
)
if src.count(dump_anchor) != 1:
    sys.exit(f'dump anchor count {src.count(dump_anchor)}')

dump = '''    if (&tb710fu_dyld_probe_idx) {
        /* Groups of four u64 written round-robin by patch 0053, oldest
         * first. Tag chars spell what the call was; the fields differ per
         * tag, so decode with the tag in hand:
         *   OATN  openat : a=path, b=flags, c=returned fd
         *   MMAP  mmap   : a=(prot<<32)|flags, b=fd, c=returned address
         *   FNCT  fcntl  : a=fd, b=cmd, c=return value
         *   MUNM  munmap : a=addr, b=len
         */
        uint32_t di = tb710fu_dyld_probe_idx;
        printf("TB710FU DYLDPROBE: idx=%u groups=%u\\n", di, di / 4u);
        for (uint32_t g = 0; g < 16u; ++g) {
            uint32_t s = (di + g * 4u) & 63u;
            uint64_t tag = tb710fu_dyld_probe[s];
            if (tag == 0) {
                continue;
            }
            printf("TB710FU DYLDPROBE: %c%c%c%c a=0x%llx b=0x%llx c=0x%llx\\n",
                   (char)(tag >> 24), (char)(tag >> 16),
                   (char)(tag >> 8), (char)tag,
                   (unsigned long long)tb710fu_dyld_probe[(s + 1u) & 63u],
                   (unsigned long long)tb710fu_dyld_probe[(s + 2u) & 63u],
                   (unsigned long long)tb710fu_dyld_probe[(s + 3u) & 63u]);
        }
        printf("TB710FU DYLDPROBE: end\\n");
    }
'''

src = src.replace(dump_anchor, dump + dump_anchor, 1)

F.write_text(src, encoding='utf-8')

# ---- structural checks ---------------------------------------------------
assert 'tb710fu_dyld_probe_idx' in src
assert src.count('TB710FU DYLDPROBE: end') == 1
print('probe dump added to tb710fu_dump_procs()')
print(f'file now {len(src)} chars')
