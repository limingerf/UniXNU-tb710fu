"""Repack a verified TB710FU boot carrier with a new 3 MiB FD, without signing.

Keeps its BootShim, appended DTB and Android header fields. The container format
follows pinned UniXNU scripts/unixnu/bootimg.py; no fetched script is modified.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct
import zlib


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--source-sha256', required=True)
    parser.add_argument('--source-fd-sha256', required=True)
    parser.add_argument('--fd', type=Path, required=True)
    parser.add_argument('--fd-sha256', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    data = args.source.read_bytes()
    fd = args.fd.read_bytes()
    if sha(data) != args.source_sha256 or sha(fd) != args.fd_sha256:
        raise SystemExit('Input hash mismatch')
    if len(fd) != 0x300000 or data[:8] != b'ANDROID!':
        raise SystemExit('Unexpected FD size or boot header')
    kernel_size, ramdisk_size = struct.unpack_from('<II', data, 8)
    if struct.unpack_from('<I', data, 40)[0] != 4 or ramdisk_size != 0:
        raise SystemExit('Requires the known ramdisk-free Android v4 boot carrier')
    signature_size = struct.unpack_from('<I', data, 1580)[0]
    signature_offset = (4096 + kernel_size + 4095) // 4096 * 4096
    signature = data[signature_offset:signature_offset + signature_size]
    if len(signature) != signature_size or any(signature):
        raise SystemExit('Refuse to preserve a stale non-empty GKI signature')
    decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
    payload = decoder.decompress(data[4096:4096 + kernel_size])
    dtb = decoder.unused_data
    if not decoder.eof or len(payload) != 192 + 0x300000:
        raise SystemExit('Unexpected shim/FD gzip layout')
    shim, old_fd = payload[:192], payload[192:]
    if sha(old_fd) != args.source_fd_sha256:
        raise SystemExit('Source embedded FD mismatch')
    if shim[56:60] != b'ARM\x64' or struct.unpack_from('<QQ', shim, 8) != (0xA7100000, len(payload)):
        raise SystemExit('Unexpected ARM64 BootShim header')
    if dtb[:4] != b'\xd0\x0d\xfe\xed' or struct.unpack_from('>I', dtb, 4)[0] != len(dtb):
        raise SystemExit('Invalid appended DTB')
    combined = shim + fd
    kernel = (data[4096:4096 + kernel_size] if fd == old_fd else
              gzip.compress(combined, mtime=0) + dtb)
    header = bytearray(data[:4096])
    struct.pack_into('<I', header, 8, len(kernel))
    image = bytes(header) + kernel
    image += bytes((-len(image)) % 4096)
    image += signature  # Preserve the official mkbootimg zero-filled v4 slot.
    image += bytes((-len(image)) % 4096)
    check = zlib.decompressobj(16 + zlib.MAX_WBITS)
    if check.decompress(image[4096:4096 + len(kernel)]) != combined or check.unused_data != dtb:
        raise SystemExit('Packed image readback failed')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('xb') as stream:
        stream.write(image)
    report = {'stage': 'unsigned-boot-carrier', 'source_sha256': sha(data),
              'bytes': len(image), 'sha256': sha(image), 'fd_sha256': sha(fd),
              'shim_sha256': sha(shim), 'dtb_sha256': sha(dtb),
              'preserved_zero_gki_signature_bytes': signature_size,
              'header_changes': ['kernel_size'], 'readback': 'PASS',
              'requires_boot_avb_signature': True, 'device_operations': False}
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
