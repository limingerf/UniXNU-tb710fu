"""Inspect real FFS files in nested UEFI FVs, including standard LZMA guided sections."""
import hashlib
import lzma
import struct
import uuid

LZMA_GUID = uuid.UUID('ee4e5898-3914-4259-9d6e-dc7bd79403cf')


def inspect(data):
    volumes, files, unknown = {}, set(), set()

    def sections(blob, depth):
        offset = 0
        while offset + 4 <= len(blob):
            size = int.from_bytes(blob[offset:offset + 3], 'little')
            kind, header = blob[offset + 3], 4
            if size == 0xffffff:
                if offset + 8 > len(blob):
                    break
                size, header = struct.unpack_from('<I', blob, offset + 4)[0], 8
            if size < header or offset + size > len(blob):
                break
            part = blob[offset:offset + size]
            if kind == 2 and size >= header + 20:
                guid = uuid.UUID(bytes_le=part[header:header + 16])
                start = struct.unpack_from('<H', part, header + 16)[0]
                if not header + 20 <= start <= size:
                    raise ValueError('Invalid guided section offset')
                if guid == LZMA_GUID:
                    decoded = lzma.decompress(part[start:], format=lzma.FORMAT_ALONE, memlimit=256 * 1024 * 1024)
                    if len(decoded) > 128 * 1024 * 1024:
                        raise ValueError('Unexpectedly large decoded firmware section')
                    sections(decoded, depth + 1)
                    scan(decoded, depth + 1)
                else:
                    unknown.add(str(guid))
            elif kind == 0x17:
                scan(part[header:], depth + 1)
            offset = (offset + size + 3) & ~3

    def scan(blob, depth=0):
        if depth > 8:
            raise ValueError('Unexpected firmware nesting depth')
        position = 0
        while True:
            signature = blob.find(b'_FVH', position)
            if signature < 0:
                return
            position = signature + 4
            start = signature - 40
            if start < 0 or start + 56 > len(blob):
                continue
            size = struct.unpack_from('<Q', blob, start + 32)[0]
            header = struct.unpack_from('<H', blob, start + 48)[0]
            if header < 56 or header % 2 or size < header or start + size > len(blob):
                continue
            volume = blob[start:start + size]
            if sum(struct.unpack('<' + 'H' * (header // 2), volume[:header])) & 0xffff:
                continue
            digest = hashlib.sha256(volume).hexdigest()
            if digest in volumes:
                continue
            count, offset = 0, (header + 7) & ~7
            while offset + 24 <= len(volume):
                entry = volume[offset:offset + 24]
                if entry == b'\xff' * 24:
                    break
                length, fixed = int.from_bytes(entry[20:23], 'little'), 24
                if length == 0xffffff:
                    if offset + 32 > len(volume):
                        raise ValueError('Truncated large FFS file')
                    length, fixed = struct.unpack_from('<Q', volume, offset + 24)[0], 32
                if length < fixed or offset + length > len(volume):
                    raise ValueError('Invalid FFS file length')
                files.add(str(uuid.UUID(bytes_le=entry[:16])))
                count += 1
                if entry[18] != 0xf0:
                    sections(volume[offset + fixed:offset + length], depth + 1)
                offset = (offset + length + 7) & ~7
            volumes[digest] = {'sha256': digest, 'bytes': size, 'ffs_files': count, 'depth': depth}

    scan(data)
    if not volumes:
        raise ValueError('No checksum-valid firmware volume found')
    return {'volumes': list(volumes.values()), 'file_guids': sorted(files), 'unhandled_guided_sections': sorted(unknown)}
