"""
Prove the probe recorder is present in the built kernel by locating its tag
immediates in the machine code.

Tags are 32-bit immediates passed to tb710fu_dyld_probe_put:

    0x4F41544E  'OATN'  openat
    0x4D4D4150  'MMAP'  mmap
    0x464E4354  'FNCT'  fcntl
    0x4D554E4D  'MUNM'  munmap

A naive ASCII search for "OATN" fails because the compiler materialises the
constant as a little-endian word (4E 54 41 4F) inside instructions, not as text.
So search for both the ASCII form and the little-endian immediate form.

Also search for the ring symbol name, which survives in the string table even
though the helper is 'static inline' and leaves no symbol.
"""
import pathlib
import struct
import sys

IMG = pathlib.Path(r'D:\unixnu-tb710fu-clean\out\pl107\kernel.macho')
data = IMG.read_bytes()
print(f'{IMG}  ({len(data)} bytes)')

tags = {
    'OATN': 0x4F41544E,
    'MMAP': 0x4D4D4150,
    'FNCT': 0x464E4354,
    'MUNM': 0x4D554E4D,
}

print('\n--- tag immediates (code) ---')
found_tags = 0
for name, val in tags.items():
    le = struct.pack('<I', val)
    be = struct.pack('>I', val)
    n_le = data.count(le)
    n_be = data.count(be)
    n_ascii = data.count(name.encode())
    hit = n_le > 0 or n_be > 0 or n_ascii > 0
    if hit:
        found_tags += 1
    print(f'  {name} 0x{val:08X}: LE={n_le} BE={n_be} ascii={n_ascii}  '
          f'{"HIT" if hit else "miss"}')

print('\n--- symbol / string table ---')
for s in (b'tb710fu_dyld_probe', b'tb710fu_dyld_probe_idx',
          b'tb710fu_pid1_sc_ring', b'tb710fu_pid1_sc_enter'):
    n = data.count(s)
    print(f'  {s.decode():28} occurrences={n}  {"HIT" if n else "miss"}')

print('\n--- existing 0052 marker (control) ---')
ctrl = b'TB710FU SRMAP'
print(f'  {ctrl.decode():28} occurrences={data.count(ctrl)}')

print()
if found_tags >= 3:
    print('RESULT: probe recorder present - at least 3 of the 4 tag immediates '
          'appear in the image.')
elif found_tags > 0:
    print('RESULT: partial - some tags present, inspect further.')
else:
    print('RESULT: probe recorder ABSENT - patch 0053 did not reach the image.')
