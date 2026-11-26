#!/usr/bin/env python3
"""Host-side checks for the TB710FU input subsystem.

Compiles the portable C layer (transport, protocol, tracking, HID packing, key
policy) with the distribution compiler and runs the unit tests in
package/Tb710fuInput/host/Tb710fuInputHostTests.c. No IOKit, no XNU headers, no
cross compiler: the models drive the same callbacks the probe installs.

Optionally re-proves the generated IOHIDDevice vtable table against this board's
prelinked kernel (--kernel, defaulting to .local/prelink-kernel.macho when it
exists) and the reference firmware image (--firmware).

Exit status is 0 only when the compile is clean and every check passes.
"""
import argparse
import hashlib
import importlib.util
import json
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEVICE = Path(__import__('os').environ.get('UNIXNU_DEVICE', ROOT)).resolve()
PACKAGE = DEVICE / 'package/Tb710fuInput'
TESTS = PACKAGE / 'host/Tb710fuInputHostTests.c'
OUT = ROOT / 'build/tb710fu-input-host'

SOURCES = [PACKAGE / name for name in (
    'Tb710fuInputConfig.c',
    'Tb710fuGeni.c',
    'Tb710fuNvt.c',
    'Tb710fuNvtFirmware.c',
    'Tb710fuTracker.c',
    'Tb710fuHid.c',
    'Tb710fuSpmi.c',
    'Tb710fuKeys.c',
)]
HEADERS = [PACKAGE / name for name in (
    'Tb710fuInputHardware.h', 'Tb710fuInputConfig.h', 'Tb710fuInputHidSlots.h',
    'Tb710fuMem.h', 'Tb710fuGeni.h', 'Tb710fuNvt.h', 'Tb710fuNvtFirmware.h',
    'Tb710fuTracker.h', 'Tb710fuHid.h', 'Tb710fuSpmi.h', 'Tb710fuKeys.h',
)]
KERNEL_IMAGE = DEVICE / '.local/prelink-kernel.macho'
FIRMWARE_IMAGE = DEVICE / '.local/input-sources/firmware/novatek/novatek_nt36532_fw.bin'

CFLAGS = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
          '-Wno-unused-parameter', '-Wshadow', '-Wstrict-prototypes',
          '-Wmissing-prototypes', '-Wvla', '-fno-strict-aliasing']


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def helper(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def compiler():
    for candidate in (['cc'], ['gcc'], ['clang']):
        if shutil.which(candidate[0]):
            return candidate
    raise SystemExit('no C compiler found (cc/gcc/clang)')


def parse_slot_array(text):
    """Read the uint16 diversity array out of the generated header."""
    marker = 'tb710fu_hid_slot_diversity[TB710FU_HID_VTABLE_SLOTS] = {'
    start = text.index(marker) + len(marker)
    end = text.index('};', start)
    values = []
    for token in text[start:end].replace('\n', ' ').split(','):
        token = token.strip()
        if not token:
            continue
        if not token.startswith('0x'):
            raise SystemExit(f'slot table has a non-numeric entry: {token}')
        values.append(int(token, 16))
    return values


def check_slots(kernel):
    build = helper('tb710fu_input_build', ROOT / 'scripts/build-tb710fu-input.py')
    rows = build.vtable_rows(kernel)
    build.verify_named_slots(rows)
    header = (PACKAGE / 'Tb710fuInputHidSlots.h').read_text()
    table = parse_slot_array(header)
    if len(table) != len(rows):
        raise SystemExit(f'header has {len(table)} slots, the kernel image has {len(rows)}')
    for index, (want, row) in enumerate(zip(table, rows)):
        if want != row['diversity']:
            raise SystemExit(f'slot {index}: header 0x{want:04x}, kernel 0x{row["diversity"]:04x}')
    for name in ('TB710FU_HID_VTABLE_SLOTS', 'TB710FU_HID_SLOT_NEW_REPORT_DESC',
                 'TB710FU_HID_SLOT_HANDLE_REPORT', 'TB710FU_HID_INSTANCE_BYTES'):
        if name not in header:
            raise SystemExit(f'header is missing {name}')
    if '0xD0u' not in header:
        raise SystemExit('header does not pin the IOHIDDevice instance size')
    return {'image': str(kernel), 'image_sha256': sha(kernel), 'slots': len(rows),
            'named': [{'slot': slot, 'diversity': f'0x{rows[slot]["diversity"]:04x}'}
                      for _, slot, _ in build.NAMED_SLOTS]}


FIRMWARE_PROBE = '''\
#include "Tb710fuNvtFirmware.h"
#include <stdio.h>

static unsigned char image[262144];

int main(int argc, char **argv)
{
    FILE *file;
    size_t bytes;
    struct tb710fu_nvt_firmware firmware;
    int result;

    if (argc < 2) return 2;
    file = fopen(argv[1], "rb");
    if (!file) return 2;
    bytes = fread(image, 1u, sizeof(image), file);
    fclose(file);
    result = tb710fu_nvt_firmware_parse(image, bytes, &firmware);
    printf("parse=%d bytes=%u ilm=0x%06x+%u@0x%06x dlm=0x%06x+%u@0x%06x dlm-crc=%s\\n",
           result, firmware.image_bytes, firmware.ilm_address, firmware.ilm_bytes,
           firmware.ilm_sram, firmware.dlm_address, firmware.dlm_bytes, firmware.dlm_sram,
           firmware.dlm_crc_valid ? "ok" : "MISMATCH");
    return result == 0 && firmware.dlm_crc_valid ? 0 : 1;
}
'''


def check_firmware(cc, image):
    """Run the real parser from Tb710fuNvtFirmware.c against the real blob."""
    probe = OUT / 'firmware-probe.c'
    probe.write_text(FIRMWARE_PROBE)
    binary = OUT / 'firmware-probe'
    command = [*cc, *CFLAGS, '-I', str(PACKAGE), str(probe),
               str(PACKAGE / 'Tb710fuNvtFirmware.c'), '-o', str(binary)]
    built = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if built.returncode:
        raise SystemExit(built.stdout[-4000:])
    run = subprocess.run([str(binary), str(image)], text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if run.returncode:
        raise SystemExit(f'{image}: parser rejected the reference blob:\n{run.stdout}')
    return {'image': str(image), 'bytes': image.stat().st_size, 'sha256': sha(image),
            'parser': run.stdout.strip()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=KERNEL_IMAGE,
                        help='prelinked kernel to re-prove the IOHIDDevice slot table against')
    parser.add_argument('--firmware', type=Path, default=FIRMWARE_IMAGE,
                        help='reference NT36532 firmware image to fingerprint')
    parser.add_argument('--keep', action='store_true', help='keep the built test binary')
    args = parser.parse_args()

    OUT.mkdir(parents=True, exist_ok=True)
    binary = OUT / 'tb710fu-input-host-tests'
    command = [*compiler(), *CFLAGS, '-I', str(PACKAGE), str(TESTS), *map(str, SOURCES),
               '-o', str(binary)]
    print('compile:', ' '.join(shlex.quote(part) for part in command))
    compiled = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (OUT / 'compile.log').write_text(compiled.stdout)
    if compiled.returncode:
        raise SystemExit(compiled.stdout[-6000:])

    tested = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (OUT / 'tests.log').write_text(tested.stdout)
    print(tested.stdout, end='')
    if tested.returncode:
        raise SystemExit(f'host tests failed ({tested.returncode})')

    report = {
        'compiler': compiled.args[0],
        'compiler_version': subprocess.check_output([compiled.args[0], '--version'], text=True).splitlines()[0],
        'command': command,
        'sources': {str(path.relative_to(DEVICE)): sha(path) for path in SOURCES + [TESTS] + HEADERS},
        'result': 'PASS',
        'checks': tested.stdout.strip(),
    }
    if args.kernel.is_file():
        report['hid_slots'] = check_slots(args.kernel)
        print(f'hid slot table re-proved against {args.kernel}')
    else:
        print(f'kernel image {args.kernel} absent: slot table not re-proved')
    if args.firmware.is_file():
        report['firmware'] = check_firmware(compiler(), args.firmware)
        print(f'firmware {report["firmware"]["parser"]}')
    else:
        print(f'firmware image {args.firmware} absent: parser checked on synthetic headers only')
    (OUT / 'host-report.json').write_text(json.dumps(report, indent=2) + '\n')
    if not args.keep:
        binary.unlink(missing_ok=True)
    print('PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
