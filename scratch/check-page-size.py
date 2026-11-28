"""
Determine whether the pl105 kernel is built for 4 KiB or 16 KiB pages.

Rather than inferring from patch presence, read it out of the prelinked image:

  * .local/prelink-kernel.macho is the prelink output used for the build
  * `_arm_hardware_page_size` is a data symbol whose initialiser is the
    hardware page size the kernel was compiled for
  * `_PAGE_SHIFT_CONST` likewise

We locate each symbol's address from kernel-symbols.txt, translate the
virtual address to a file offset using the Mach-O LC_SEGMENT_64 table, and
read the value.
"""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(r'D:\unixnu-tb710fu-clean')
SYMS = ROOT / '.local' / 'kernel-symbols.txt'
IMAGE = ROOT / '.local' / 'prelink-kernel.macho'

for p in (SYMS, IMAGE):
    if not p.exists():
        sys.exit(f'missing {p}')

# --- parse the symbol table ------------------------------------------------
wanted = {}
pat = re.compile(r'^([0-9a-f]{16})\s+\S+\s+_?(arm_hardware_page_size|PAGE_SHIFT_CONST|PAGE_MASK_CONST|arm_page_shift)$')
for line in SYMS.read_text(errors='replace').splitlines():
    m = pat.match(line.strip())
    if m:
        wanted[m.group(2)] = int(m.group(1), 16)

print('symbols found:')
for k, v in wanted.items():
    print(f'  {k:28} 0x{v:016x}')
if not wanted:
    sys.exit('no relevant symbols in the symbol table')

# --- parse the Mach-O segments --------------------------------------------
data = IMAGE.read_bytes()
magic = struct.unpack_from('<I', data)[0]
if magic != 0xfeedfacf:
    sys.exit(f'not a 64-bit Mach-O (magic 0x{magic:08x})')

ncmds = struct.unpack_from('<I', data, 16)[0]
off = 32
segs = []
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', data, off)
    if cmd == 0x19:  # LC_SEGMENT_64
        vmaddr, vmsize = struct.unpack_from('<QQ', data, off + 24)
        fileoff, filesize = struct.unpack_from('<QQ', data, off + 40)
        try:
            name = data[off + 8:off + 24].split(b'\0')[0].decode()
        except UnicodeDecodeError:
            name = '?'
        segs.append((name, vmaddr, vmsize, fileoff, filesize))
    off += size

print()
print('segments:')
for name, vmaddr, vmsize, fileoff, filesize in segs:
    print(f'  {name:20} vmaddr=0x{vmaddr:012x} vmsize=0x{vmsize:010x} fileoff=0x{fileoff:010x}')


def va_to_file(addr):
    for name, vmaddr, vmsize, fileoff, filesize in segs:
        if vmaddr <= addr < vmaddr + vmsize:
            delta = addr - vmaddr
            if delta >= filesize:
                return None, name
            return fileoff + delta, name
    return None, None


print()
for name, addr in sorted(wanted.items()):
    pos, segname = va_to_file(addr)
    if pos is None:
        print(f'{name}: at 0x{addr:x} -> not file-backed (segment {segname})')
        continue
    v32 = struct.unpack_from('<I', data, pos)[0]
    v64 = struct.unpack_from('<Q', data, pos)[0]
    note = ''
    if v32 == 4096:
        note = '   <== 4 KiB pages'
    elif v32 == 16384:
        note = '   <== 16 KiB pages'
    elif v32 in (12, 14):
        note = f'   <== PAGE_SHIFT {v32} -> {1 << v32} bytes'
    print(f'{name}: file 0x{pos:x} in {segname}  u32={v32} (0x{v32:x})  u64=0x{v64:x}{note}')
