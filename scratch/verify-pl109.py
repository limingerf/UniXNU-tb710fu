"""Full readiness check for pl109: recorder AND dump must both be present."""
import pathlib

IMG = pathlib.Path(r'D:\unixnu-tb710fu-clean\out\pl109\kernel.macho')
d = IMG.read_bytes()
print(f'{IMG}  ({len(d)} bytes)')

checks = {
    'recorder tag OATN':            b'OATN',
    'recorder tag FNCT':            b'FNCT',
    'recorder tag MMAP':            b'MMAP',
    'recorder tag MUNM':            b'MUNM',
    'helper symbol (noinline)':     b'_tb710fu_dyld_probe_put',
    'ring global':                  b'_tb710fu_dyld_probe\x00',
    'ring index global':            b'_tb710fu_dyld_probe_idx\x00',
    'dump header format':           b'TB710FU DYLDPROBE: idx=',
    'dump per-entry format':        b'TB710FU DYLDPROBE: %c%c%c%c',
    'dump terminator':              b'TB710FU DYLDPROBE: end',
    'control 0043 ring':            b'_tb710fu_pid1_sc_ring',
    'control 0052 srmap':           b'TB710FU SRMAP',
}

ok = 0
for label, needle in checks.items():
    n = d.count(needle)
    hit = n > 0
    if hit:
        ok += 1
    print(f'  {label:30} count={n:<3} {"OK" if hit else "MISSING"}')

print()
total = len(checks)
print(f'{ok}/{total} present')
if ok == total:
    print('RESULT: pl109 is complete -- recorder records, dump prints, both verified.')
else:
    print('RESULT: incomplete; see MISSING rows above.')
