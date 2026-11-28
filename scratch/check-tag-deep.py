"""
Test a different explanation for the missing OATN/FNCT immediates.

Earlier I concluded the openat and fcntl branches were optimised away. But the
case labels are compile-time constants 463 and 92; a compiler that lowers the
switch to a jump table does not need to keep 0x4F41544E in the instruction
stream as a literal -- it can materialise it from a constant pool, split it
across two instructions (movz/movk), or pass it in a register built by
arithmetic.

So before rewriting the probe, check for:
  * the case VALUES 463 (0x1CF) and 92 (0x5C) near the other tag stores
  * movz/movk sequences that would assemble 0x4F41544E
  * whether the ASCII tags appear at all as 4-byte little-endian words

A cleaner discriminator: the recorder writes tags through a single helper.
If the helper was inlined, all four call sites share the same surrounding code
shape. Find one known tag (MMAP is polluted by strings, so use nothing) --
instead locate the ring symbol's data references.
"""
import pathlib
import struct

IMG = pathlib.Path(r'D:\unixnu-tb710fu-clean\out\pl107\kernel.macho')
data = IMG.read_bytes()

# AArch64: movz Xd, imm16, lsl N  ->  0xD2800000 | (hw<<21) | (imm16<<5) | Rd
#          movk Xd, imm16, lsl N  ->  0xF2800000 | ...
def movz_movk_candidates(value):
    """Return the (imm16, shift) pairs needed to build value with movz+movk."""
    parts = []
    for shift in (0, 16, 32, 48):
        imm = (value >> shift) & 0xFFFF
        if imm:
            parts.append((imm, shift))
    return parts


print('=== how many instructions would be needed ===')
for name, val in (('OATN', 0x4F41544E), ('FNCT', 0x464E4354),
                  ('MMAP', 0x4D4D4150), ('MUNM', 0x4D554E4D)):
    parts = movz_movk_candidates(val)
    enc = []
    for imm, shift in parts:
        hw = shift // 16
        # movz for the lowest part, movk for the rest
        enc.append((imm, hw))
    print(f'  {name} 0x{val:08X}: {len(enc)} instruction(s) required '
          f'-> {[(hex(i), h) for i, h in enc]}')

# 463 and 92 as case values may survive as immediates in comparisons
print('\n=== case value immediates ===')
for label, v in (('463 openat', 463), ('197 mmap', 197),
                 ('92 fcntl', 92), ('73 munmap', 73)):
    le16 = struct.pack('<H', v)
    le32 = struct.pack('<I', v)
    print(f'  {label:12} u16_le count={data.count(le16):6}  '
          f'u32_le count={data.count(le32):6}')

# is the ring referenced at all from code? look for its symbol name in the
# string table and report the file offset, since __LINKEDIT follows __TEXT
print('\n=== ring symbol location ===')
for needle in (b'_tb710fu_dyld_probe\x00', b'_tb710fu_dyld_probe_idx\x00'):
    i = data.find(needle)
    print(f'  {needle[:-1].decode():26} @0x{i:x}' if i >= 0 else
          f'  {needle[:-1].decode():26} NOT FOUND')

# The probe helper is static inline; if it survived, the four tag constants
# appear as 4-byte words somewhere. Check every 4-byte alignment for OATN.
print('\n=== exhaustive 4-byte-aligned scan for OATN/FNCT ===')
for name, val in (('OATN', 0x4F41544E), ('FNCT', 0x464E4354)):
    le = struct.pack('<I', val)
    hits = []
    # scan word-aligned and at every offset
    for off in range(0, 4):
        start = off
        while True:
            i = data.find(le, start)
            if i < 0:
                break
            hits.append(i)
            start = i + 1
            if len(hits) > 20:
                break
        if len(hits) > 20:
            break
    print(f'  {name}: {len(hits)} hit(s)' +
          (f' at {[hex(h) for h in hits[:5]]}' if hits else ''))

print('\n=== conclusion ===')
print('If OATN/FNCT are absent at every offset, the branches are genuinely not')
print('in the image and the recorder must be made resilient (noinline + barrier).')
print('If they appear only at odd offsets, the earlier scan was misaligned and')
print('the branches are actually present.')
