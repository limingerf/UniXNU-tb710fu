"""Point the generator's four call sites and assertion at string tags."""
import pathlib

P = pathlib.Path(r'D:\unixnu-tb710fu-clean\scratch\gen-0053-v7.py')
t = P.read_text(encoding='utf-8')

pairs = [
    ('tb710fu_dyld_probe_put(0x4F41544Eu,', 'tb710fu_dyld_probe_put("OATN",'),
    ('tb710fu_dyld_probe_put(0x4D4D4150u,', 'tb710fu_dyld_probe_put("MMAP",'),
    ('tb710fu_dyld_probe_put(0x464E4354u,', 'tb710fu_dyld_probe_put("FNCT",'),
    ('tb710fu_dyld_probe_put(0x4D554E4Du,', 'tb710fu_dyld_probe_put("MUNM",'),
    ("assert 'tb710fu_dyld_probe_put(0x4F41544Eu' in result",
     'assert \'tb710fu_dyld_probe_put("OATN"\' in result'),
]

for old, new in pairs:
    if old not in t:
        print(f'  MISSING: {old}')
        continue
    t = t.replace(old, new)
    print(f'  replaced: {old[:50]}')

P.write_text(t, encoding='utf-8')

print('\n--- resulting generator markers ---')
for i, line in enumerate(t.split('\n'), 1):
    s = line.strip()
    if ('dyld_probe_put("' in s or 'noinline' in s or '__asm__' in s
            or "assert 'tb710fu_dyld_probe_put" in s):
        print(f'{i:4}: {s[:78]}')
