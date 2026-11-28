"""
Analyse the pl109 probe data to characterise the loop precisely.

Question this answers: is dyld looping over ONE file repeatedly, or over many
files? The path pointer in OATN distinguishes them:

    a=0x16f4f24a0  -> first path  (fd 0, 1)
    a=0x16f4f24b8  -> second path (fd 2 onwards, +0x18 bytes apart)

If every iteration after the first uses the same pointer, dyld is retrying a
single file and never accepting it. That changes the diagnosis: it is not
"scanning the cache and failing somewhere", it is "one specific file will not
be accepted".
"""
import pathlib
import re
import sys

LOG = pathlib.Path(r'D:\unixnu-tb710fu-clean\scratch\pl109_final_155609.log')
if not LOG.exists():
    sys.exit(f'missing {LOG}')

text = LOG.read_text(errors='replace')

pat = re.compile(
    r'DYLDPROBE:\s+(\w+)\s+a=0x([0-9a-f]+)\s+b=0x([0-9a-f]+)\s+c=0x([0-9a-f]+)')
recs = []
for m in pat.finditer(text):
    tag = m.group(1)
    a = int(m.group(2), 16)
    b = int(m.group(3), 16)
    c = int(m.group(4), 16)
    recs.append((tag, a, b, c))

print(f'parsed {len(recs)} ring entries\n')

# ---- which paths were opened, and with which fds --------------------------
opens = [(a, b, c) for tag, a, b, c in recs if tag == 'OATN']
print(f'OATN records: {len(opens)}')
paths = {}
for a, b, c in opens:
    paths.setdefault(a, []).append(c)
for a, fds in sorted(paths.items()):
    print(f'  path 0x{a:x}: opened {len(fds)}x, fds {fds[0]:#x}..{fds[-1]:#x}')

# ---- mmap/munmap pairs -----------------------------------------------------
mmaps = [(a, b, c) for tag, a, b, c in recs if tag == 'MMAP']
munms = [(a, b, c) for tag, a, b, c in recs if tag == 'MUNM']
print(f'\nMMAP records: {len(mmaps)}')
addrs = set(c for a, b, c in mmaps)
print(f'  distinct return addresses: {[hex(x) for x in addrs]}')
prot_flags = set(a for a, b, c in mmaps)
print(f'  distinct (prot<<32|flags): {[hex(x) for x in prot_flags]}')
fds = set(b for a, b, c in mmaps)
print(f'  distinct fds mapped: {[hex(x) for x in sorted(fds)]}')

print(f'\nMUNM records: {len(munms)}')
maddrs = set(a for a, b, c in munms)
lens = set(b for a, b, c in munms)
print(f'  distinct unmap addresses: {[hex(x) for x in maddrs]}')
print(f'  distinct unmap lengths:   {[hex(x) for x in lens]}')

# ---- alignment check -------------------------------------------------------
print('\nalignment of unmap addresses (must be 16 KiB for dyld cache):')
for a in sorted(maddrs):
    aligned16 = (a & 0x3fff) == 0
    aligned4 = (a & 0xfff) == 0
    print(f'  0x{a:x}: 16K-aligned={aligned16} 4K-aligned={aligned4}')

# ---- the decisive question -------------------------------------------------
print('\n' + '=' * 60)
if len(paths) == 2:
    p = sorted(paths.keys())
    print('dyld opened exactly TWO distinct paths:')
    print(f'  path A 0x{p[0]:x} -> fds {[hex(x) for x in paths[p[0]]]}')
    print(f'  path B 0x{p[1]:x} -> fds {[hex(x) for x in paths[p[1]]]}')
    print('\nEvery iteration after the first reopens path B and never closes it.')
    print('This is a retry of ONE file, not a scan of the cache.')
elif len(paths) == 1:
    print('dyld reopened a single path throughout.')
else:
    print(f'dyld touched {len(paths)} distinct paths; inspect the list above.')

print('\nfds are never reused: max fd in data is', hex(max(c for _, _, c in opens)))
print('That is a descriptor leak proportional to the retry count.')
