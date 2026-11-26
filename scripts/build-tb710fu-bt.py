#!/usr/bin/env python3
"""Build the TB710FU Bluetooth archive (SM8650 GENI HSUART + WCN7850 Hamilton loader + HCI service).

Follows the pinned schema2 device archive pattern used by build-tb710fu-power.py: the five C translation
units (GENI transport, GCC clock plan, H4 framing, QCA firmware loader, TLMM/BT_EN sequencing) compile with
the actual XNU C compile record, the IOService/IOUserClient provider compiles with the actual IOKit C++
record (arm64e-apple-ios27.0, TARGET_OS_OSX=1), its metaclass constructor is moved to __mod_deferred so
Macos27Services.cpp can install it before postModLoad, and the XNU link target is invalidated when the
archive changes. Host checks run separately (scripts/check-tb710fu-bt-host.py).

Nothing here touches dts/firmware: the driver reads hmtbtfw20.tlv / hmtnv20.bin at runtime through the
Tb710fuAddon firmware reader (Tb710fuBluetoothFirmware.h, weak_import).
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
# Sources resolve from the device repository the framework hands every archive through UNIXNU_DEVICE, the
# same way build-tb710fu-macos27-services.py does; build outputs stay in the materialised tree (ROOT).
DEVICE = Path(os.environ['UNIXNU_DEVICE']).resolve() if os.environ.get('UNIXNU_DEVICE') else ROOT
PACKAGE = DEVICE / 'package/Tb710fuBluetooth'
OUT = ROOT / 'build/tb710fu-bt'

RECORD_C = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'
RECORD_CPP = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
C_SOURCES = [
    PACKAGE / 'Tb710fuBluetoothGeni.c',
    PACKAGE / 'Tb710fuBluetoothClock.c',
    PACKAGE / 'Tb710fuBluetoothHci.c',
    PACKAGE / 'Tb710fuBluetoothQca.c',
    PACKAGE / 'Tb710fuBluetoothPower.c',
]
CPP_SOURCES = [PACKAGE / 'Tb710fuBluetooth.cpp']
HEADERS = [
    PACKAGE / 'Tb710fuBluetoothBoard.h',
    PACKAGE / 'Tb710fuBluetoothGeni.h',
    PACKAGE / 'Tb710fuBluetoothClock.h',
    PACKAGE / 'Tb710fuBluetoothHci.h',
    PACKAGE / 'Tb710fuBluetoothQca.h',
    PACKAGE / 'Tb710fuBluetoothPower.h',
    PACKAGE / 'Tb710fuBluetoothFirmware.h',
]
TARGET = 'arm64e-apple-ios27.0'
ARCHIVE_NAME = 'libTb710fuBt.a'
LINK_STAMP = '.tb710fu-bt-link-input.json'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def reported(path):
    # Sources live in DEVICE, build outputs in ROOT (the materialised tree): report whichever contains them.
    for base in (DEVICE, ROOT):
        try:
            return str(path.relative_to(base))
        except ValueError:
            pass
    return str(path)


def helper(name, path):
    if not path.is_file():
        raise SystemExit(f'{path} missing')
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def compile_command(record, source, output, includes, extra):
    """Rewrite a recorded XNU compile command to name our source and output (same as the power script)."""
    arguments = record['arguments']
    command = []
    index = 0
    replaced = False
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
            replaced = True
        elif value == '-o':
            command.extend(['-o', str(output)])
            index += 1
        elif value in ('-MF', '-MT', '-MQ', '-MJ'):
            index += 1
        elif value in ('-MD', '-MMD', '-MP'):
            pass
        elif value == '-Xclang' and index + 1 < len(arguments) and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    if not replaced:
        raise SystemExit(f'{record["file"]}: compile record does not name its source')
    command = ['--target=' + TARGET if value.startswith('--target=') else value for value in command]
    command += ['-Wall', '-Wextra', '-Werror', '-Wno-overriding-option']
    for include in includes:
        command += ['-I', str(include)]
    command += extra
    return command


def defer_constructor(obj, late, init):
    """Move __mod_init_func entries to __mod_deferred and export the deferred initialiser symbol."""
    data = bytearray(obj.read_bytes())
    if struct.unpack_from('<I', data)[0] != 0xfeedfacf:
        raise SystemExit(f'{obj}: not a 64-bit Mach-O object')
    count = struct.unpack_from('<I', data, 16)[0]
    symtab, renamed, offset = None, 0, 32
    for _ in range(count):
        command, size = struct.unpack_from('<II', data, offset)
        if command == 0x19:
            sections = struct.unpack_from('<I', data, offset + 64)[0]
            for index in range(sections):
                section = offset + 72 + index * 80
                if data[section:section + 16].rstrip(b'\0') == b'__mod_init_func':
                    data[section:section + 16] = b'__mod_deferred\0\0'
                    flags = struct.unpack_from('<I', data, section + 64)[0]
                    struct.pack_into('<I', data, section + 64, flags & ~0xff)
                    renamed += 1
        elif command == 0x2:
            symtab = struct.unpack_from('<IIII', data, offset + 8)
        offset += size
    if not renamed:
        raise SystemExit(f'{obj}: expected eager metaclass constructor section missing')
    if symtab is None:
        raise SystemExit(f'{obj}: no symbol table')
    symoff, nsyms, stroff, _ = symtab
    external = 0
    for index in range(nsyms):
        entry = symoff + index * 16
        string, n_type = struct.unpack_from('<IB', data, entry)
        name = bytes(data[stroff + string:]).split(b'\0', 1)[0]
        if name == init.encode():
            struct.pack_into('<B', data, entry + 4, n_type | 0x01)
            external += 1
    if not external:
        raise SystemExit(f'{obj}: expected deferred constructor {init} missing from symbols')
    late.write_bytes(bytes(data))


def invalidate_link(digest):
    records = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    target = records / 'mach.development.bcm2837.unstripped.noctf'
    stamp = records / LINK_STAMP
    text = json.dumps({'archive_sha256': digest}, indent=2) + '\n'
    previous = stamp.read_text() if stamp.is_file() else ''
    if previous == text:
        return False
    target.unlink(missing_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', dir=records, prefix=stamp.name + '.', delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(text)
    os.replace(temporary, stamp)
    return True


def plan():
    """Print the archive contents and the record/toolchain the real build will use (no tree needed)."""
    missing = [str(path) for path in C_SOURCES + CPP_SOURCES + HEADERS if not path.is_file()]
    if missing:
        raise SystemExit('missing sources:\n  ' + '\n  '.join(missing))
    print(json.dumps({
        'archive': str(OUT / ARCHIVE_NAME),
        'c_sources': [reported(path) for path in C_SOURCES],
        'cpp_sources': [reported(path) for path in CPP_SOURCES],
        'headers': [reported(path) for path in HEADERS],
        'records': {'c': str(RECORD_C), 'cpp': str(RECORD_CPP)},
        'target': TARGET,
        'c_records_present': RECORD_C.is_file(),
        'cpp_records_present': RECORD_CPP.is_file(),
        'deferred_constructor': '__GLOBAL__sub_I_Tb710fuBluetooth.cpp',
        'boot_args': {'bt': '0..4 staged bring-up', 'tb710fu-bt-speed': 'override max-speed',
                      'tb710fu-bt-fw': 'firmware directory', 'tb710fu-bt-bdaddr': 'AA:BB:CC:DD:EE:FF',
                      'tb710fu-bt-log': '0 quiets the evidence lines'},
        'host_checks': 'scripts/check-tb710fu-bt-host.py',
    }, indent=2))


def main():
    device = Path(os.environ['UNIXNU_DEVICE']).resolve()
    tree = Path.cwd().resolve()
    catalog = tree / 'components.toml'
    if not catalog.is_symlink():
        raise SystemExit('Expected the fixed framework catalog link in the resolved tree')
    framework = catalog.resolve().parent
    lock = json.loads((device / 'upstream.lock.json').read_text())
    revision = subprocess.check_output(['git', '-C', str(framework), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != lock['unixnu']['commit']:
        raise SystemExit('Framework revision differs from upstream.lock.json')

    for record in (RECORD_C, RECORD_CPP):
        if not record.is_file():
            raise SystemExit(f'{record} missing; build the minimal XNU kernel first')
    c_record = json.loads(RECORD_C.read_text().strip().rstrip(','))
    cpp_record = json.loads(RECORD_CPP.read_text().strip().rstrip(','))

    adapter = helper('tb710fu_ufs_adapter', device / 'scripts/build-tb710fu-ufs-adapter.py')
    target = helper('tb710fu_source_target', framework / 'scripts/kernel_target.py')

    OUT.mkdir(parents=True, exist_ok=True)
    archive = OUT / ARCHIVE_NAME
    archive.unlink(missing_ok=True)
    objects, commands = [], {}
    includes = [PACKAGE, tree / '.cache/xnu-work/osfmk', tree / '.cache/xnu-work/libkern']

    for source in C_SOURCES:
        output = OUT / (source.stem + '.o')
        # -mgeneral-regs-only: a C translation unit that reaches the vector registers is a kernel bug.
        command = compile_command(c_record, source, output, includes, ['-mgeneral-regs-only'])
        commands[source.name] = command
        result = subprocess.run(command, cwd=c_record['directory'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (OUT / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(f'{source.name}:\nCMD: {" ".join(command)}\n{result.stdout[-6000:]}')
        target.check_object(output.read_bytes(), source.name)
        objects.append(output)

    for source in CPP_SOURCES:
        output = OUT / (source.stem + '.o')
        command = adapter.compile_command(cpp_record, source, output, includes, [])
        command = ['--target=' + TARGET if value.startswith('--target=') else value for value in command]
        commands[source.name] = command
        result = subprocess.run(command, cwd=cpp_record['directory'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (OUT / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(f'{source.name}:\nCMD: {" ".join(command)}\n{result.stdout[-6000:]}')
        target.check_object(output.read_bytes(), source.name)
        late = output.with_suffix('.late.o')
        defer_constructor(output, late, '__GLOBAL__sub_I_' + source.name)
        target.check_object(late.read_bytes(), source.name + ' deferred')
        objects.append(late)

    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
    digest = sha(archive)
    report = {
        'archive': str(archive),
        'archive_sha256': digest,
        'framework_revision': revision,
        'objects': {obj.name: sha(obj) for obj in objects},
        'sources': {reported(path): sha(path) for path in C_SOURCES + CPP_SOURCES + HEADERS},
        'compile_commands_sha256': {name: hashlib.sha256(json.dumps(cmd).encode()).hexdigest()
                                    for name, cmd in commands.items()},
        'host_checks': 'scripts/check-tb710fu-bt-host.py',
        'deferred_constructor': '__GLOBAL__sub_I_Tb710fuBluetooth.cpp',
        # Explicit stage statement: an archive (and a prelinked kernel) is not a driven controller,
        # a driven controller is not hardware bring-up, and a published IOService is not a BT stack.
        'stage': 'archive only: no HCI host controller is attached',
        'firmware': {
            'patch': 'qca/hmtbtfw20.tlv (274128 B, md5 bcaca7b2a1e02be62fbffaf746d52440)',
            'nvm': 'qca/hmtnv20.bin (9592 B, md5 c047013753ce0c3bec603a590b9fa7e4)',
            'reader': 'tb710fu_fw_* (package/Tb710fuAdsp, weak_import)',
        },
    }
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    if invalidate_link(digest):
        print('TB710FU Bluetooth archive changed: invalidated the XNU link target')
    print(f'TB710FU Bluetooth archive passed: {archive} ({digest[:12]})')


if __name__ == '__main__':
    if '--plan' in sys.argv[1:]:
        sys.exit(plan())
    sys.exit(main())
