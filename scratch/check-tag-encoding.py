"""
Check whether the openat/fcntl probe branches survived compilation.

The image contains the MMAP and MUNM tag immediates but not OATN or FNCT. Two
readings:

  a) the compiler dropped those switch cases
  b) it encoded them differently (e.g. via a literal pool, or folded into a
     different constant ordering)

If (a) is true the probe will silently miss the two calls that matter most:
fcntl(F_ADDFILESIGS_RETURN) is the one that tells us which fd is being signed,
and openat is the one that names the file.

This script looks for the constants in both endiannesses and at 1-byte offsets,
and reports where MMAP sits so the surrounding code can be inspected.
"""
import pathlib
import struct

IMG = pathlib.Path(r'D:\unixnu-tb710fu-clean\out\pl107\kernel.macho')
data = IMG.read_bytes()

tags = {
    'OATN': 0x4F41544E,
    'MMAP': 0x4D4D4150,
    'FNCT': 0x464E4354,
    'MUNM': 0x4D554E4D,
}


def find_all(needle, limit=8):
    out = []
    start = 0
    while len(out) < limit:
        i = data.find(needle, start)
        if i < 0:
            break
        out.append(i)
        start = i + 1
    return out


print('=== tag constants, all encodings ===')
for name, val in tags.items():
    le = struct.pack('<I', val)
    be = struct.pack('>I', val)
    ascii_b = name.encode()
    hits = {
        'LE': find_all(le),
        'BE': find_all(be),
        'ascii': find_all(ascii_b),
    }
    total = sum(len(v) for v in hits.values())
    print(f'\n{name} = 0x{val:08X}   total={total}')
    for kind, offs in hits.items():
        if offs:
            print(f'  {kind:5}: ' + ', '.join(f'0x{o:x}' for o in offs[:6]))

# MMAP hits tell us where the recorder lives; dump surrounding words
mmap_hits = find_all(struct.pack('>I', tags['MMAP']), limit=4)
print(f'\n=== context around MMAP BE hits ({len(mmap_hits)}) ===')
for h in mmap_hits[:2]:
    lo = max(0, h - 48)
    chunk = data[lo:h + 48]
    print(f'\n@0x{h:x}:')
    for i in range(0, len(chunk), 16):
        row = chunk[i:i+16]
        hexs = ' '.join(f'{b:02x}' for b in row)
        print(f'  {lo+i:08x}  {hexs}')

if not mmap_hits:
    print('no MMAP BE hits to inspect')
