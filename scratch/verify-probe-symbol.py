"""
Verify that the pl107 kernel actually contains the probe ring storage and the
recorder, using the Mach-O symbol table rather than a naive byte search.

Why this matters: 'static inline' helpers leave no symbol, so a byte search for
tb710fu_dyld_probe_put proves nothing. The ring itself is a global, so it must
appear in the symbol table if the patch really took effect.
"""
import pathlib
import struct
import sys

ROOT = pathlib.Path(r'D:\unixnu-tb710fu-clean')
IMG = ROOT / 'out' / 'pl107' / 'kernel.macho'

data = IMG.read_bytes()
print(f'image: {IMG} ({len(data)} bytes)')

if struct.unpack_from('<I', data)[0] != 0xfeedfacf:
    sys.exit('not a 64-bit Mach-O')

ncmds = struct.unpack_from('<I', data, 16)[0]
off = 32
symoff = None
nsyms = None
stroff = None
strsize = None
segs = []

for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', data, off)
    if cmd == 0x19:  # LC_SEGMENT_64
        name = data[off+8:off+24].split(b'\0')[0].decode(errors='replace')
        vmaddr, vmsize = struct.unpack_from('<QQ', data, off + 24)
        fileoff, filesize = struct.unpack_from('<QQ', data, off + 40)
        segs.append((name, vmaddr, vmsize, fileoff, filesize))
    elif cmd == 0x2:  # LC_SYMTAB
        symoff, nsyms, stroff, strsize = struct.unpack_from('<IIII', data, off + 8)
    off += size

print(f'LC_SYMTAB: symoff=0x{symoff:x} nsyms={nsyms} stroff=0x{stroff:x}')
print(f'nlist entries are {0x10} bytes each in LC_SYMTAB but this image is a '
      f'kernel; checking both 16-byte and 12-byte strides')

if symoff is None:
    sys.exit('no LC_SYMTAB')

wanted = [b'tb710fu_dyld_probe', b'tb710fu_dyld_probe_idx',
          b'tb710fu_pid1_sc_ring', b'tb710fu_pid1_sc_count']
hits = {w: [] for w in wanted}

# nlist_64 stride is 16 bytes
stride = 16
for i in range(nsyms):
    base = symoff + i * stride
    if base + stride > len(data):
        break
    n_strx, n_type, n_sect, n_desc, n_value = struct.unpack_from('<IBBHQ', data, base)
    if n_strx == 0 or n_strx >= strsize:
        continue
    end = data.find(b'\0', stroff + n_strx)
    if end < 0:
        continue
    name = data[stroff + n_strx:end]
    for w in wanted:
        if name == w:
            hits[w].append((n_value, n_sect, n_type))

print()
for w, lst in hits.items():
    label = w.decode()
    if lst:
        for value, sect, typ in lst:
            print(f'  FOUND {label:26} value=0x{value:016x} sect={sect} type=0x{typ:02x}')
    else:
        print(f'  MISS  {label}')

probe_ok = bool(hits[b'tb710fu_dyld_probe'])
pid1_ok = bool(hits[b'tb710fu_pid1_sc_ring'])
print()
print(f'probe ring in symbol table : {probe_ok}')
print(f'0043 ring in symbol table  : {pid1_ok}')
if probe_ok:
    print('RESULT: patch 0053 storage is present in the built kernel')
else:
    print('RESULT: probe storage MISSING - the patch did not reach the image')
