#!/usr/bin/env python3
"""Build the macOS 27 TB710FU GPU addon: Turnip, the Metal plugin bundle and the NGPUADD2 payload.

Run on the macOS CI runner after `.work/UniXNU` is checked out and the Adreno
driver headers are in place (the framework's plugin build reads them from
drivers/Adreno740; a board without that driver checks the pinned Adreno740
source out there, which is why this script checks the directory first).

The steps are the framework's own, unchanged:
  scripts/build-turnip-darwin.py --target macos27
  scripts/build-adreno-metal-plugin.py --target macos27 --no-replay
  scripts/make-gpu-addon.py --prefix GPUBundles --no-daemon [--publish] out.bin
  scripts/addon-add-dir.py (only when GPU firmware is staged)
  scripts/trust-append-module.py (only when --trust-cache is given)

What this script adds is the board policy the framework cannot know: the
GPUBundles prefix macOS reads (the kernel writes those entries to
/Library/GPUBundles with rootless protection), the firmware manifest check, the
NGPUADD2 verification and the CDHash list the owner trust cache must carry.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / 'scripts/data/tb710fu-gpu-firmware.json'
ADDON_MAGIC = b'NGPUADD2'
ENTRY_LIMIT = 160
# The payload is 78 MiB inside an 85,024,768-byte cap and already carries the
# ADSP addon; what is left for GPU entries is about 7.4 MiB. This is a warning,
# not a gate: the packaging step's own fat check is authoritative.
PAYLOAD_HEADROOM = 8 * 1024 * 1024


def run(command, **kwargs):
    print('+', ' '.join(str(part) for part in command))
    return subprocess.run([str(part) for part in command], check=True, **kwargs)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def addon_entries(data):
    if data[:8] != ADDON_MAGIC or len(data) < 24 or struct.unpack_from('<Q', data, 16)[0] != len(data):
        raise SystemExit('invalid GPU addon container')
    at = 24
    entries = []
    for _ in range(struct.unpack_from('<I', data, 8)[0]):
        length, mode, size, packed = struct.unpack_from('<IIQQ', data, at)
        at += 24
        name = data[at:at + length - 1].decode()
        if not 1 < length <= ENTRY_LIMIT or name.startswith('/') or '..' in Path(name).parts:
            raise SystemExit(f'invalid addon path: {name!r}')
        at += (length + 7) & ~7
        body = zlib.decompress(data[at:at + packed])
        at += (packed + 7) & ~7
        if len(body) != size:
            raise SystemExit(f'invalid addon entry size: {name}')
        entries.append({'name': name, 'mode': mode, 'size': size, 'body': body})
    if at != len(data):
        raise SystemExit('addon has trailing bytes')
    return entries


def cdhash(path):
    detail = subprocess.run(['codesign', '--display', '--verbose=4', str(path)],
                            check=True, capture_output=True, text=True).stderr
    match = re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M)
    if not match:
        raise SystemExit('no CDHash for ' + str(path))
    return match[1]


def verify_firmware(directory, manifest):
    staged = []
    for item in manifest['files']:
        path = directory / item['name']
        if not path.is_file():
            raise SystemExit(f'GPU firmware missing: {path} (manifest names {item["name"]})')
        if path.stat().st_size != item['bytes'] or sha256(path) != item['sha256']:
            raise SystemExit(f'GPU firmware differs from the pinned gen70900 image: {path}')
        staged.append(item['name'])
    return staged


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--framework', type=Path, default=Path('.work/UniXNU'),
                        help='checked-out UniXNU framework (default .work/UniXNU)')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/tb710fu-gpu-addon')
    parser.add_argument('--publish', action='store_true',
                        help='add GPUBundles/NativeAdrenoMetal.publish, which makes the plugin publish a real device')
    parser.add_argument('--firmware', type=Path, help='directory with the four gen70900 images to carry as Gpu/Firmware')
    parser.add_argument('--plugin-dir', type=Path, help='reuse an existing adreno-metal-plugin-macos27 build')
    parser.add_argument('--skip-build', action='store_true', help='only repack what --plugin-dir already holds')
    parser.add_argument('--trust-cache', type=Path, help='owner trust bundle to append the addon CDHashes to')
    parser.add_argument('--budget-bytes', type=int, default=PAYLOAD_HEADROOM)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()

    if platform.system() != 'Darwin':
        raise SystemExit('This script builds Mach-O bundles and needs the macOS runner (xcrun, codesign).')

    framework = args.framework.resolve()
    for script in ('build-turnip-darwin.py', 'build-adreno-metal-plugin.py', 'make-gpu-addon.py',
                   'addon-add-dir.py', 'trust-append-module.py'):
        if not (framework / 'scripts' / script).is_file():
            raise SystemExit(f'{framework}: scripts/{script} missing')
    abi = framework / 'drivers/Adreno740'
    if not abi.is_dir():
        raise SystemExit(f'{abi} missing: the Turnip and plugin builds read NativeAdrenoABI.h from there '
                         '(check the pinned Adreno740 source out at that path)')

    args.out.mkdir(parents=True, exist_ok=True)
    if not args.skip_build:
        run([sys.executable, framework / 'scripts/build-turnip-darwin.py', '--target', 'macos27'])
        run([sys.executable, framework / 'scripts/build-adreno-metal-plugin.py', '--target', 'macos27', '--no-replay'])
    plugin = args.plugin_dir or framework / 'build/adreno-metal-plugin-macos27'
    if not (plugin / 'NativeAdrenoMetal.bundle/Contents/MacOS/NativeAdrenoMetal').is_file():
        raise SystemExit(f'{plugin}: no NativeAdrenoMetal.bundle; build-adreno-metal-plugin.py --target macos27 writes it')

    manifest = json.loads(MANIFEST.read_text())
    work = Path(tempfile.mkdtemp(prefix='tb710fu-gpu-', dir=args.out))
    candidate = work / ('gpuaddon' + ('.publish' if args.publish else '') + '.bin')
    command = [sys.executable, framework / 'scripts/make-gpu-addon.py', '--prefix', 'GPUBundles', '--no-daemon']
    if args.publish:
        command.append('--publish')
    command.append(candidate)
    run(command, env={**os.environ, 'ADRENO_PLUGIN_DIR': str(plugin)})

    staged_firmware = []
    if args.firmware:
        staged = verify_firmware(args.firmware.resolve(), manifest)
        staged_firmware = staged
        combined = work / 'gpuaddon-firmware.bin'
        run([sys.executable, framework / 'scripts/addon-add-dir.py', candidate, combined,
             manifest['addon_prefix'], args.firmware.resolve()])
        candidate = combined

    addon = args.out / 'gpuaddon.bin'
    shutil.copyfile(candidate, addon)
    data = addon.read_bytes()
    entries = addon_entries(data)
    names = [entry['name'] for entry in entries]
    if 'GPUBundles/NativeAdrenoMetal.bundle/Contents/MacOS/NativeAdrenoMetal' not in names:
        raise SystemExit('the addon does not carry the plugin bundle at the macOS prefix')
    if args.publish != ('GPUBundles/NativeAdrenoMetal.publish' in names):
        raise SystemExit('publish marker presence does not match the requested mode')
    for item in manifest['files']:
        if args.firmware and f"{manifest['addon_prefix']}/{item['name']}" not in names:
            raise SystemExit('firmware entry missing from the addon: ' + item['name'])

    # The owner trust cache carries every executable the kernel will run or IOGPU
    # will load out of the addon; bundle CDHashes are the plugin executable and
    # the Turnip dylib inside it.
    executables = [entry for entry in entries
                   if entry['body'][:4] in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe')]
    hashes = []
    for entry in executables:
        path = work / entry['name']
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(entry['body'])
        path.chmod(entry['mode'] & 0o777)
        hashes.append(cdhash(path))
    hashes = sorted(set(hashes))

    trust = None
    if args.trust_cache:
        updated = args.out / 'gpu-owner-trust.bundle'
        run([sys.executable, framework / 'scripts/trust-append-module.py', args.trust_cache, updated, *hashes])
        trust = {'source': str(args.trust_cache), 'output': str(updated), 'sha256': sha256(updated)}

    size = len(data)
    report = {
        'schema': 'tb710fu-gpu-addon-v1',
        'addon': str(addon),
        'addon_sha256': sha256(addon),
        'addon_bytes': size,
        'budget_bytes': args.budget_bytes,
        'within_budget': size <= args.budget_bytes,
        'publish': args.publish,
        'plugin': str(plugin),
        'entries': [{'name': entry['name'], 'size': entry['size'], 'mode': oct(entry['mode'])} for entry in entries],
        'firmware': staged_firmware,
        'cdhashes': hashes,
        'trust': trust,
        'boot_arguments': ['gpu-addon=1', 'gpu-power=1', 'gpu-metal=1']
                     + (['gpu-bundle-system=1'] if args.publish else []),
    }
    text = json.dumps(report, indent=2) + '\n'
    (args.report or args.out / 'report.json').write_text(text)
    if not report['within_budget']:
        print(f'WARNING: addon is {size} bytes, above the {args.budget_bytes}-byte payload headroom')
    print(f'GPU addon: {addon} ({size} bytes, {len(entries)} entries, {len(hashes)} executables)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
