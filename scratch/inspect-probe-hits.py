"""
Determine what the two 'tb710fu_dyld_probe' hits actually are.

Round 41 reported 2 occurrences of 'tb710fu_dyld_probe' and concluded the probe
storage reached the image. The tag-immediate check then showed OATN and FNCT
absent, and that the MMAP/MUNM "hits" were unrelated strings inside
'SKR_MODE_MMAPOK' and 'eSMART_MMAP_KERNEL_FLAGS_TRIALS'. So that conclusion needs
re-checking rather than trusting the count.

This prints the surrounding bytes of each hit so it is obvious whether the name
lives in a symbol/string table (patch applied) or somewhere unrelated.
"""
import pathlib

IMG = pathlib.Path(r'D:\unixnu-tb710fu-clean\out\pl107\kernel.macho')
data = IMG.read_bytes()

for needle in (b'tb710fu_dyld_probe', b'tb710fu_pid1_sc_ring'):
    print(f'\n=== {needle.decode()} ===')
    start = 0
    count = 0
    while True:
        i = data.find(needle, start)
        if i < 0:
            break
        count += 1
        lo = max(0, i - 40)
        hi = min(len(data), i + len(needle) + 40)
        chunk = data[lo:hi]
        printable = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
        print(f'  @0x{i:x}  ..{printable}..')
        start = i + 1
    print(f'  total: {count}')

# control: a symbol that must exist because 0043 is known-good
print('\n=== control: tb710fu_pid1_sc_count ===')
start = 0
c = 0
while True:
    i = data.find(b'tb710fu_pid1_sc_count', start)
    if i < 0:
        break
    c += 1
    lo = max(0, i - 40)
    hi = min(len(data), i + 20 + 40)
    chunk = data[lo:hi]
    printable = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
    print(f'  @0x{i:x}  ..{printable}..')
    start = i + 1
print(f'  total: {c}')
