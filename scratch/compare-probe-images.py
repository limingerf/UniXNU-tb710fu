"""Compare pl107 vs pl108 for the four probe tags and the noinline helper."""
import pathlib
import struct

ROOT = pathlib.Path(r'D:\unixnu-tb710fu-clean\out')

TAGS = ['OATN', 'MMAP', 'FNCT', 'MUNM']

def report(name):
    p = ROOT / name / 'kernel.macho'
    if not p.exists():
        print(f'{name}: image missing')
        return None
    d = p.read_bytes()
    print(f'\n=== {name} ({len(d)} bytes) ===')

    # string tags: with noinline + string literals these must appear as text
    print('  tag strings:')
    allok = True
    for t in TAGS:
        n = d.count(t.encode())
        mark = 'HIT' if n else 'MISS'
        if not n:
            allok = False
        print(f'    {t}  count={n}  {mark}')

    # helper symbol, only non-inline form leaves one
    for sym in (b'_tb710fu_dyld_probe_put', b'tb710fu_dyld_probe_put'):
        n = d.count(sym)
        print(f'  symbol {sym.decode():28} count={n}')

    # ring globals
    for sym in (b'_tb710fu_dyld_probe\x00', b'_tb710fu_dyld_probe_idx\x00'):
        n = d.count(sym)
        print(f'  global {sym[:-1].decode():28} count={n}')

    print(f'  RESULT: all four tags present = {allok}')
    return allok


r107 = report('pl107')
r108 = report('pl108')

print('\n=== verdict ===')
if r108:
    print('pl108 carries all four probe tags: the recorder survived compilation.')
    print('That is what pl107 lacked, and it is now fixed.')
else:
    print('pl108 still missing at least one tag.')
    print('If the helper symbol is also absent, noinline did not take effect and')
    print('the helper needs to move out of the header-style block entirely.')
