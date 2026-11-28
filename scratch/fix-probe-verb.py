"""
Rewrite the probe verb in CdcXnu.c to use the same weak_import pattern
BootWatchdogPolicy.c uses, instead of an array-address test that forces a
strong reference.

The failing build showed:

    Undefined symbols for architecture arm64e.kernel:
      "_tb710fu_dyld_probe", referenced from:
          _cdc_exec_pending_cmd in libTb710fuCdcLog.a[7](CdcXnu.o)

BootWatchdogPolicy.c does this and links fine:

    extern volatile uint32_t tb710fu_pid1_sc_count __attribute__((weak_import));
    ...
    if (&tb710fu_pid1_sc_count) { printf(..., tb710fu_pid1_sc_count); }

The difference: it takes the address of a SCALAR, which the compiler turns into
a weak reference. Taking '&array[0]' on a declared-but-undefined array, then
indexing it, produced a strong reference instead.

So declare scalars/arrays weak and test them the same way, and keep the reads
inside the guarded block.
"""
import pathlib
import re
import sys

F = pathlib.Path(r'D:\unixnu-tb710fu-clean\package\Tb710fuUsbCdc\CdcXnu.c')
src = F.read_text(encoding='utf-8')

start = src.find('    } else if (cmd == 12u) {')
if start < 0:
    sys.exit('cmd 12 handler not found')
end = src.find('    } else if (cmd >= 7u && cmd <= 10u) {', start)
if end < 0:
    sys.exit('handler end not found')

new_handler = '''    } else if (cmd == 12u) {
        /* pl106: dump the dyld-cache probe (patch 0053).  launchd (pid 1)
         * spins in openat -> fstat64 -> pread -> fcntl(F_ADDFILESIGS_RETURN)
         * -> mmap -> munmap with every call returning 0, and never reaches
         * shared_region_map_and_slide_2_np.  The probe keeps the arguments
         * that patch 0043 discarded, which is what distinguishes "mmap came
         * back with an address dyld dislikes" from "the fd or flags are
         * wrong".  Sixteen groups of four u64, oldest first.
         *
         * The symbols are weak so this verb still answers on a kernel built
         * without patch 0053.  Test them the way BootWatchdogPolicy.c tests
         * its weak imports: address-of on the symbol itself, reads inside. */
        extern volatile uint64_t tb710fu_dyld_probe[64] __attribute__((weak_import));
        extern volatile uint32_t tb710fu_dyld_probe_idx __attribute__((weak_import));
        cdc_log("TB710FU CDC: cmd=probe\\n");
        if (&tb710fu_dyld_probe_idx) {
            uint32_t idx = tb710fu_dyld_probe_idx;
            cdc_log("TB710FU PROBE: idx=%u (groups written=%u)\\n",
                idx, idx / 4u);
            for (uint32_t g = 0; g < 16u; ++g) {
                uint32_t s = (idx + g * 4u) & 63u;
                uint64_t tag = tb710fu_dyld_probe[s];
                if (tag == 0) {
                    continue;
                }
                cdc_log("TB710FU PROBE: %c%c%c%c a=0x%llx b=0x%llx c=0x%llx\\n",
                    (char)(tag >> 24), (char)(tag >> 16),
                    (char)(tag >> 8), (char)tag,
                    (unsigned long long)tb710fu_dyld_probe[(s + 1u) & 63u],
                    (unsigned long long)tb710fu_dyld_probe[(s + 2u) & 63u],
                    (unsigned long long)tb710fu_dyld_probe[(s + 3u) & 63u]);
            }
            cdc_log("TB710FU PROBE: end\\n");
        } else {
            cdc_log("TB710FU PROBE: unavailable (patch 0053 not in this kernel)\\n");
        }
        return;
'''

src = src[:start] + new_handler + src[end:]
F.write_text(src, encoding='utf-8')
print('handler rewritten')

# sanity: no leftover runtime-lookup helper
assert 'cdc_lookup_kernel_symbol' not in src
print('no stale symbol-lookup helper')
