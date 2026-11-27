"""Read IOHIDDevice's live gMetaClass out of the prelinked kernel and report
its instanceSize, to check the hard-coded 0xD0 in Tb710fuInputHidSlots.h.

OSMetaClass layout (libkern/c++/OSMetaClass.h):
  the instance size is stored as an unsigned int; rather than guess the
  offset, scan the first 0x80 bytes of the object for the value 0xD0 and
  also print the raw words so the offset is identifiable.
"""
import struct
import sys
from pathlib import Path

IMAGE = Path(r'D:\unixnu-tb710fu-clean\.local\prelink-kernel.macho')
META_VA = 0xfffffff00cc24958   # __ZN11IOHIDDevice10gMetaClassE

data = IMAGE.read_bytes()
assert struct.unpack_from('<I', data)[0] == 0xfeedfacf, 'not a 64-bit mach-o'

ncmds = struct.unpack_from('<I', data, 16)[0]
offset = 32
segments = []
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<II', data, offset)
    if cmd == 0x19:  # LC_SEGMENT_64
        vmaddr, vmsize = struct.unpack_from('<QQ', data, offset + 24)
        fileoff, filesize = struct.unpack_from('<QQ', data, offset + 40)
        segments.append((vmaddr, vmsize, fileoff, filesize))
    offset += size


def va_to_file(addr):
    for vmaddr, vmsize, fileoff, filesize in segments:
        if vmaddr <= addr < vmaddr + vmsize:
            delta = addr - vmaddr
            if delta >= filesize:
                return None          # in zero-fill
            return fileoff + delta
    return None


pos = va_to_file(META_VA)
if pos is None:
    sys.exit(f'gMetaClass at 0x{META_VA:x} not file-backed in this image')

raw = data[pos:pos + 0x80]
print(f'gMetaClass @ 0x{META_VA:x}  file offset 0x{pos:x}')
print('first 0x80 bytes as 64-bit words:')
for i in range(0, 0x80, 8):
    w = struct.unpack_from('<Q', raw, i)[0]
    note = ''
    if w in (0xd0, 0xD0):
        note = '   <== 0xD0 (the hard-coded instance size)'
    # kernel text/data pointers look like 0xfffffff0xxxxxxxx
    if (w >> 32) == 0xfffffff0:
        note += '   (kernel pointer)'
    print(f'  +0x{i:02x}: 0x{w:016x}{note}')

print()
print('first 0x80 bytes as 32-bit words:')
for i in range(0, 0x80, 4):
    w = struct.unpack_from('<I', raw, i)[0]
    note = ''
    if w == 0xd0:
        note = '   <== 0xD0'
    if w and w < 0x10000:
        note += f'   (small: {w})'
    print(f'  +0x{i:02x}: 0x{w:08x}{note}')
