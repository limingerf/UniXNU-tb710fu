"""Prepare unsigned vendor_boot from the recorded baseline, changing its kernel only.

Run under WSL with mtools. Verifies the actual raw LZMA stream, preserved config
and ADT, FAT size and unchanged outer payload fields. No signing/device commands.
"""
import argparse
import hashlib
import json
import lzma
from pathlib import Path
import struct
import subprocess
import tomllib


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--lzma', type=Path, required=True)
    p.add_argument('--kernel-sha256', required=True)
    p.add_argument('--source-vendor', type=Path, required=True)
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--add-reporter-selftest', action='store_true', help='Append explicit report-only test menu; preserve original entries/default')
    args = p.parse_args()
    original = args.source_vendor.read_bytes()
    if sha(original) != '836ca997d766dedfef0531f7283026b443d2227302148fdd107852e25354df15':
        raise SystemExit('Require recorded ecc0 vendor_boot baseline')
    encoded = args.lzma.read_bytes()
    prop = encoded[0]
    lc, prop = prop % 9, prop // 9
    lp, pb = prop % 5, prop // 5
    plain = lzma.decompress(encoded[13:], format=lzma.FORMAT_RAW,
                            filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': int.from_bytes(encoded[1:5], 'little'),
                                      'lc': lc, 'lp': lp, 'pb': pb}])
    if sha(plain) != args.kernel_sha256 or len(plain) != int.from_bytes(encoded[5:13], 'little'):
        raise SystemExit('Diagnostic kernel identity or decoded length mismatch')
    page, length = struct.unpack_from('<I', original, 12)[0], struct.unpack_from('<I', original, 24)[0]
    body = struct.unpack_from('>Q', original, len(original) - 64 + 12)[0]
    args.output_dir.mkdir(parents=True, exist_ok=True)
    payload = args.output_dir / 'payload.img'
    unsigned = args.output_dir / 'vendor_boot.unsigned.img'
    if payload.exists() or unsigned.exists():
        raise SystemExit('Output exists; preserve earlier evidence')
    with payload.open('xb') as f:
        f.write(original[page:page + length])
    def read(name):
        return subprocess.check_output(['mtype', '-i', str(payload), '::/EFI/UniXNU/' + name])
    kept = {name: sha(read(name)) for name in ['unixnu.toml', 'Payloads/kernel.macho.adt']}
    expected = dict(kept)
    if args.add_reporter_selftest:
        config = read('unixnu.toml').decode('utf-8')
        original_config = tomllib.loads(config)
        matches = [entry for entry in original_config['entry'] if entry.get('id') == 'xnu' and entry.get('type') == 'xnu']
        if len(matches) != 1 or any(entry.get('id') == 'reporter-selftest' for entry in original_config['entry']):
            raise SystemExit('Unexpected base menu; refuse to guess a self-test entry')
        entry = matches[0]
        config += '\n[[entry]]\nid = "reporter-selftest"\ntitle = "Reporter self-test (intentional stop)"\ntype = "xnu"\n'
        config += 'path = ' + json.dumps(entry['path']) + '\nargs = ' + json.dumps(entry.get('args', '') + ' tb-report-selftest=1') + '\nenabled = true\n'
        parsed = tomllib.loads(config)
        if parsed['boot'] != original_config['boot'] or parsed['entry'][:-1] != original_config['entry']:
            raise SystemExit('Self-test altered existing menu/default')
        local = args.output_dir / 'unixnu.toml'
        local.write_text(config, encoding='utf-8')
        subprocess.run(['mcopy', '-o', '-i', str(payload), str(local), '::/EFI/UniXNU/unixnu.toml'], check=True)
        expected['unixnu.toml'] = sha(config.encode('utf-8'))
    subprocess.run(['mcopy', '-o', '-i', str(payload), str(args.lzma), '::/EFI/UniXNU/Payloads/kernel.macho'], check=True)
    if read('Payloads/kernel.macho') != encoded or expected != {name: sha(read(name)) for name in expected}:
        raise SystemExit('FAT kernel/config/ADT readback failed')
    fat = payload.read_bytes()
    if len(fat) != length:
        raise SystemExit('Payload size changed')
    image = bytearray(original[:body])
    image[page:page + length] = fat
    if image[:page] != original[:page] or image[page + length:] != original[page + length:body]:
        raise SystemExit('Unexpected vendor_boot change outside ramdisk')
    with unsigned.open('xb') as f:
        f.write(image)
    report = {'unsigned_bytes': len(image), 'unsigned_sha256': sha(image), 'payload_sha256': sha(fat),
              'kernel_sha256': sha(plain), 'kernel_bytes': len(plain), 'kernel_file_readback': 'PASS',
              'original_config_and_ADT_hashes': kept, 'output_config_and_ADT_hashes': expected,
              'reporter_selftest_appended': args.add_reporter_selftest,
              'original_entries_and_default_preserved': True,
              'source_vendor_sha256': sha(original), 'device_operations': False}
    (args.output_dir / 'packaging-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
