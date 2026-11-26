#!/usr/bin/env python3
"""Build the TB710FU audio archive (GENI I2C + AW88461 amps + GPR/AudioReach + the macOS service).

Follows the same pinned schema2 device-archive pattern as scripts/build-tb710fu-power.py: the C
modules compile with the actual XNU C compile record, the two C++ files with the actual IOKit C++
record (arm64e-apple-ios27.0, TARGET_OS_OSX=1), the metaclass constructor of the service file is
deferred so Macos27Services.cpp installs it before postModLoad, and the XNU link target is
invalidated when the archive changes.

After the archive is written the script asserts the two properties this subsystem is specified on:
no undefined IOAudioFamily symbol (the archive must not need the closed family), and the entry
points Macos27Services.cpp calls are present. The host checks run separately:
scripts/check-tb710fu-audio-host.py.
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
DEVICE = Path(os.environ['UNIXNU_DEVICE']).resolve() if os.environ.get('UNIXNU_DEVICE') else ROOT
PACKAGE = DEVICE / 'package/Tb710fuAudio'
OUT = ROOT / 'build/tb710fu-audio'

RECORD_C = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'
RECORD_CPP = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
C_SOURCES = [PACKAGE / name for name in (
    'Tb710fuAcf.c',
    'Tb710fuGeniI2c.c',
    'Tb710fuAw88461.c',
    'Tb710fuGpr.c',
    'Tb710fuAudioReach.c',
    'Tb710fuAudioGain.c',
    'Tb710fuAudioEngine.c',
)]
# The metaclasses live in the service file; the bring-up file has no eager constructor.
CPP_SOURCES = [(PACKAGE / 'Tb710fuAudio.cpp', False), (PACKAGE / 'Tb710fuAudioService.cpp', True)]
HEADERS = [PACKAGE / name for name in (
    'Tb710fuAcf.h', 'Tb710fuGeniI2c.h', 'Tb710fuAw88461.h', 'Tb710fuGpr.h', 'Tb710fuAudioReach.h',
    'Tb710fuAudioAbi.h', 'Tb710fuAudioEngine.h', 'Tb710fuAudioService.h')]
TARGET = 'arm64e-apple-ios27.0'
ENTRY_POINTS = ('_tb710fu_audio_start', '_tb710fu_audio_meta_init')
FAMILY = ('IOAudio',)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def helper(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def reported(path):
    for base in (DEVICE, ROOT):
        try:
            return str(path.relative_to(base))
        except ValueError:
            pass
    return str(path)


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
    command = ['--target=' + TARGET if value.startswith('--target=') else value for value in command]
    command += ['-Wall', '-Wextra', '-Werror', '-Wno-overriding-option']
    for include in includes:
        command += ['-I', str(include)]
    command += extra
    return command


def defer_constructor(obj, late, init, required):
    """Move the eager metaclass constructor into __mod_deferred and mark the symbol external."""
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
        if required:
            raise SystemExit(f'{obj}: expected eager metaclass constructor section missing')
        return False
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
    return True


def check_archive(archive):
    """Assert no undefined IOAudioFamily symbols and that the entry points are defined."""
    undefined = subprocess.check_output(['xcrun', 'nm', '-u', str(archive)], text=True)
    defined = subprocess.check_output(['xcrun', 'nm', '-gU', str(archive)], text=True)
    offenders = [line for line in undefined.splitlines() if any(family in line for family in FAMILY)]
    if offenders:
        raise SystemExit('the archive needs IOAudioFamily symbols:\n' + '\n'.join(offenders))
    missing = [name for name in ENTRY_POINTS if name not in defined]
    if missing:
        raise SystemExit(f'entry points missing from the archive: {", ".join(missing)}')
    return {'ioaudiofamily_undefined': 0, 'entry_points': list(ENTRY_POINTS)}


def invalidate_link(digest):
    records = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    target = records / 'mach.development.bcm2837.unstripped.noctf'
    stamp = records / '.tb710fu-audio-link-input.json'
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
    archive = OUT / 'libTb710fuAudio.a'
    archive.unlink(missing_ok=True)
    objects, commands, deferred = [], {}, []
    includes = [PACKAGE, tree / '.cache/xnu-work/osfmk', tree / '.cache/xnu-work/libkern']

    for source in C_SOURCES:
        output = OUT / (source.stem + '.o')
        command = compile_command(c_record, source, output, includes, ['-mgeneral-regs-only'])
        commands[source.name] = command
        result = subprocess.run(command, cwd=c_record['directory'], text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        (OUT / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(f'{source.name}:\nCMD: {" ".join(command)}\n{result.stdout[-6000:]}')
        target.check_object(output.read_bytes(), source.name)
        objects.append(output)

    for source, required in CPP_SOURCES:
        output = OUT / (source.stem + '.o')
        command = adapter.compile_command(cpp_record, source, output, includes, [])
        command = ['--target=' + TARGET if value.startswith('--target=') else value for value in command]
        commands[source.name] = command
        result = subprocess.run(command, cwd=cpp_record['directory'], text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        (OUT / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(f'{source.name}:\n{result.stdout[-6000:]}')
        target.check_object(output.read_bytes(), source.name)
        late = output.with_suffix('.late.o')
        if defer_constructor(output, late, '__GLOBAL__sub_I_' + source.name, required):
            target.check_object(late.read_bytes(), source.name + ' deferred')
            objects.append(late)
            deferred.append(late.name)
        else:
            objects.append(output)

    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
    digest = sha(archive)
    assertions = check_archive(archive)
    report = {
        'archive': str(archive),
        'archive_sha256': digest,
        'framework_revision': revision,
        'objects': {obj.name: sha(obj) for obj in objects},
        'deferred_constructors': deferred,
        'assertions': assertions,
        'sources': {reported(path): sha(path) for path in C_SOURCES + [p for p, _ in CPP_SOURCES] + HEADERS},
        'compile_commands_sha256': {name: hashlib.sha256(json.dumps(cmd).encode()).hexdigest()
                                    for name, cmd in commands.items()},
        'host_checks': 'scripts/check-tb710fu-audio-host.py',
        # What this build does not prove: no prelinked kernel, no boot, no tone. The sound reaching
        # the speakers is stage audio=4 on the device; this archive is its compile.
        'device_audio_verified': False,
        'needs_boot_argument': 'audio=1..4',
    }
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    if invalidate_link(digest):
        print('TB710FU audio archive changed: invalidated the XNU link target')
    print(f'TB710FU audio archive passed: {archive} ({digest[:12]})')


if __name__ == '__main__':
    sys.exit(main())
