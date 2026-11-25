#!/usr/bin/env python3
"""Build the TB710FU battery archive (PMIC GLINK battery manager codec + IOPMPowerSource provider).

Follows the pinned schema2 device archive pattern: compiles the C codec with the actual XNU C compile record and
the provider with the actual IOKit C++ record (arm64e-apple-ios27.0, TARGET_OS_OSX=1), defers the IOKit metaclass
constructor so Macos27Services.cpp can install it before postModLoad, and invalidates the XNU link target when the
archive changes. The host codec checks run separately (scripts/check-tb710fu-power-host.py).
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
# The archive runs inside the materialised tree; its package link is not
# guaranteed to carry device sources added after the tree was first linked
# (the CI tree is built from the framework checkout). Resolve the sources from
# the device repository the framework hands every archive through
# UNIXNU_DEVICE, exactly as build-tb710fu-macos27-services.py does.
DEVICE = Path(os.environ['UNIXNU_DEVICE']).resolve() if os.environ.get('UNIXNU_DEVICE') else ROOT
PACKAGE = DEVICE / 'package/Tb710fuPower'
OUT = ROOT / 'build/tb710fu-power'
RECORD_C = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'
RECORD_CPP = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
C_SOURCES = [PACKAGE / 'Tb710fuBattmgr.c']
CPP_SOURCES = [PACKAGE / 'Tb710fuBattery.cpp']
TARGET = 'arm64e-apple-ios27.0'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def helper(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def compile_command(record, source, output, includes, extra):
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
    command = [TARGET if value.startswith('--target=') else value for value in command]
    command += ['-Wall', '-Wextra', '-Werror', '-Wno-overriding-option']
    command += ['-I', *map(str, includes)]
    command += extra
    return command


def defer_constructor(obj, late, init):
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
    stamp = records / '.tb710fu-power-link-input.json'
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
    archive = OUT / 'libTb710fuPower.a'
    archive.unlink(missing_ok=True)
    objects, commands = [], {}
    includes = [PACKAGE, tree / '.cache/xnu-work/osfmk', tree / '.cache/xnu-work/libkern']

    for source in C_SOURCES:
        output = OUT / (source.stem + '.o')
        command = compile_command(c_record, source, output, includes, ['-mgeneral-regs-only'])
        commands[source.name] = command
        result = subprocess.run(command, cwd=c_record['directory'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (OUT / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(f'{source.name}:\n{result.stdout[-6000:]}')
        target.check_object(output.read_bytes(), source.name)
        objects.append(output)

    for source in CPP_SOURCES:
        output = OUT / (source.stem + '.o')
        command = adapter.compile_command(cpp_record, source, output, includes, [])
        command = [TARGET if value.startswith('--target=') else value for value in command]
        commands[source.name] = command
        result = subprocess.run(command, cwd=cpp_record['directory'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (OUT / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(f'{source.name}:\n{result.stdout[-6000:]}')
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
        'sources': {str(path.relative_to(ROOT)): sha(path) for path in C_SOURCES + CPP_SOURCES +
                    [PACKAGE / 'Tb710fuBattmgr.h', PACKAGE / 'Tb710fuPowerLink.h']},
        'compile_commands_sha256': {name: hashlib.sha256(json.dumps(cmd).encode()).hexdigest()
                                    for name, cmd in commands.items()},
        'codec_host_checks': 'scripts/check-tb710fu-power-host.py',
        'device_battery_display': False,  # needs the ADSP/PMIC GLINK transport, a separate subsystem
    }
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    if invalidate_link(digest):
        print('TB710FU battery archive changed: invalidated the XNU link target')
    print(f'TB710FU battery archive passed: {archive} ({digest[:12]})')


if __name__ == '__main__':
    sys.exit(main())
