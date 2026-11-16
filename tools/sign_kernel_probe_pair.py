"""Sign a local kernel-probe vendor_boot and replace its vbmeta hash in place.

Requires the user's local-signing authorization. No device commands are run.
The existing avbtool is loaded unchanged with an explicitly selected Linux
OpenSSL backend. Every other raw descriptor and the vbmeta header must match.
"""
import argparse
from collections import Counter
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import sys


def digest(data):
    return hashlib.sha256(data).hexdigest()


SOURCE_IMAGE_BASELINES = {
    '836ca997d766dedfef0531f7283026b443d2227302148fdd107852e25354df15':
        '3992de82a3a03584b0e03c43bd66060ef93684122bb8867c893ed6c45ea85331',
    '15846226236b56bd3abb104ae57f45e23307f3ea52321fd03ed153fd2e4e0718':
        'e4cffaf3414a2c22d6c129ed62ac382424f67da8a0dd62aa9ae0ccb73fec63de',
}


def descriptors(blob, header):
    aux = 256 + header.authentication_data_block_size
    begin = aux + header.descriptors_offset
    end = begin + header.descriptors_size
    if end > len(blob):
        raise ValueError('Descriptor bounds')
    result = []
    while begin < end:
        if end - begin < 16:
            raise ValueError('Truncated descriptor')
        _, length = struct.unpack_from('>QQ', blob, begin)
        if length % 8 or 16 + length > end - begin:
            raise ValueError('Descriptor length')
        result.append(blob[begin:begin + 16 + length])
        begin += 16 + length
    return result


def hash_name(raw):
    if len(raw) < 132 or struct.unpack_from('>Q', raw)[0] != 2:
        return None
    length = struct.unpack_from('>I', raw, 56)[0]
    return raw[132:132 + length].decode('ascii')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--avbtool', type=Path, required=True)
    parser.add_argument('--key', type=Path, required=True)
    parser.add_argument('--unsigned-vendor', type=Path, required=True)
    parser.add_argument('--unsigned-sha256', required=True)
    parser.add_argument('--source-vendor', type=Path, required=True)
    parser.add_argument('--source-vbmeta', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    source_vendor = args.source_vendor.read_bytes()
    original = args.source_vbmeta.read_bytes()
    unsigned = args.unsigned_vendor.read_bytes()
    expected_vbmeta = SOURCE_IMAGE_BASELINES.get(digest(source_vendor))
    if expected_vbmeta is None:
        raise SystemExit('Source vendor_boot differs from every recorded signed baseline')
    if digest(original) != expected_vbmeta:
        raise SystemExit('Source vbmeta does not match its recorded vendor_boot baseline')
    if digest(unsigned) != args.unsigned_sha256:
        raise SystemExit('Unsigned diagnostic image identity mismatch')
    if shutil.which('openssl') != '/usr/bin/openssl':
        raise SystemExit('Run under WSL with system OpenSSL')
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location('local_avbtool', args.avbtool)
    avb = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(avb)
    avb.OPENSSL_EXECUTABLE = '/usr/bin/openssl'

    def verify(blob):
        header = avb.AvbVBMetaHeader(blob[:256])
        if header.algorithm_type != 2 or not avb.verify_vbmeta_signature(header, blob):
            raise ValueError('SHA256_RSA4096 signature verification failed')
        return header

    old_header = verify(original)
    raw = descriptors(original, old_header)
    indices = [i for i, item in enumerate(raw) if hash_name(item) == 'vendor_boot']
    if len(indices) != 1 or len(raw) != 23 or old_header.public_key_metadata_size:
        raise SystemExit('Unexpected source descriptor set or key metadata')
    index = indices[0]
    old_hash = raw[index]
    name_len, salt_len, _ = struct.unpack_from('>III', old_hash, 56)
    salt = old_hash[132 + name_len:132 + name_len + salt_len]
    _, old_offset, old_size = struct.unpack_from('>QQQ', source_vendor, len(source_vendor) - 64 + 12)
    vendor_header = verify(source_vendor[old_offset:old_offset + old_size])
    args.output_dir.mkdir(parents=True, exist_ok=True)
    vendor = args.output_dir / 'vendor_boot.img'
    vbmeta = args.output_dir / 'vbmeta.img'
    if vendor.exists() or vbmeta.exists():
        raise SystemExit('Output exists; preserve earlier evidence')
    with vendor.open('xb') as stream:
        stream.write(unsigned)

    def command(items):
        avb.AvbTool().run(['avbtool'] + list(map(str, items)))

    command(['add_hash_footer', '--image', vendor, '--partition_name', 'vendor_boot',
             '--partition_size', len(source_vendor), '--key', args.key,
             '--algorithm', 'SHA256_RSA4096', '--rollback_index', vendor_header.rollback_index,
             '--salt', salt.hex()])
    command(['verify_image', '--image', vendor])
    signed = vendor.read_bytes()
    original_size, offset, size = struct.unpack_from('>QQQ', signed, len(signed) - 64 + 12)
    if original_size != len(unsigned) or signed[:original_size] != unsigned:
        raise ValueError('Signing changed payload bytes')
    vendor_blob = signed[offset:offset + size]
    new_hashes = descriptors(vendor_blob, verify(vendor_blob))
    if len(new_hashes) != 1 or hash_name(new_hashes[0]) != 'vendor_boot':
        raise ValueError('Unexpected new vendor descriptor')
    merged = raw.copy()
    merged[index] = new_hashes[0]
    descriptor_file = args.output_dir / 'descriptors.json'
    descriptor_file.write_text(json.dumps([{'raw_hex': item.hex()} for item in merged], indent=2) + '\n')
    command(['make_vbmeta_image', '--output', vbmeta, '--key', args.key,
             '--algorithm', 'SHA256_RSA4096', '--rollback_index', old_header.rollback_index,
             '--rollback_index_location', old_header.rollback_index_location,
             '--flags', old_header.flags, '--include_descriptors_from_json', descriptor_file])
    rebuilt = vbmeta.read_bytes()
    new_header = verify(rebuilt)
    if rebuilt[:256] != original[:256] or descriptors(rebuilt, new_header) != merged:
        raise ValueError('vbmeta header or unrelated descriptors changed')
    aux = 256 + new_header.authentication_data_block_size
    key_start = aux + new_header.public_key_offset
    old_aux = 256 + old_header.authentication_data_block_size
    old_key = old_aux + old_header.public_key_offset
    if rebuilt[key_start:key_start + new_header.public_key_size] != original[old_key:old_key + old_header.public_key_size]:
        raise ValueError('Public key changed')
    report = {'stage': 'signed-kernel-probe-pair', 'vendor_boot_bytes': len(signed),
              'vendor_boot_sha256': digest(signed), 'vbmeta_bytes': len(rebuilt),
              'vbmeta_sha256': digest(rebuilt), 'source_vbmeta_sha256': digest(original),
              'unsigned_vendor_sha256': digest(unsigned), 'signature_checks': 'PASS',
              'unchanged_other_raw_descriptors': 22, 'identical_vbmeta_header': True,
              'descriptor_counts_by_tag': dict(Counter(struct.unpack_from('>Q', d)[0] for d in merged)),
              'public_key_unchanged': True, 'signing_payload_readback': 'PASS',
              'device_operations': False, 'hardware_verified': False}
    (args.output_dir / 'signing-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
