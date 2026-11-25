#!/usr/bin/env python3
"""Build the TB710FU ADSP archive (SM8650 ADSP bring-up, GLINK transport for the PMIC GLINK battery channel).

Same pinned-schema2 archive pattern as build-tb710fu-power.py: the portable C protocol layers (SMEM, SMP2P, IPCC,
GLINK, QMP, RPMH, MDT, SCM helpers, VFS firmware reader) compile with the actual XNU C record, the probe with the
actual IOKit C++ record, and a changed archive invalidates the XNU link target. The layers are UniXNU-sheng's
(BSD-2-Clause-Patent, hardware-verified on SM8550); see package/Tb710fuAdsp/Tb710fuAdsp.cpp for provenance.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DEVICE = Path(os.environ['UNIXNU_DEVICE']).resolve() if os.environ.get('UNIXNU_DEVICE') else ROOT
PACKAGE = DEVICE / 'package/Tb710fuAdsp'
POWER = DEVICE / 'package/Tb710fuPower'
OUT = ROOT / 'build/tb710fu-adsp'


def reported(path):
    # Sources live in DEVICE, build outputs in ROOT (the materialised tree); report whichever contains them.
    for base in (DEVICE, ROOT):
        try:
            return str(path.relative_to(base))
        except ValueError:
            pass
    return str(path)


RECORD_C = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'
RECORD_CPP = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
# The firmware reader uses VFS and BSD types; a BSD record (no MACH_KERNEL_PRIVATE) is the schema those headers expect.
RECORD_BSD = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/crc32.o.json'
C_SOURCES = [PACKAGE / name for name in (
    'QcomSmem.c', 'QcomSmp2p.c', 'QcomGlink.c', 'QcomQmp.c', 'QcomRpmh.c', 'QcomMdt.c', 'Tb710fuAddon.c',
    'Tb710fuAdspFirmware.c')]
CPP_SOURCES = [PACKAGE / 'Tb710fuAdsp.cpp']
HEADERS = [PACKAGE / name for name in (
    'QcomSmem.h', 'QcomSmp2p.h', 'QcomGlink.h', 'QcomQmp.h', 'QcomRpmh.h', 'QcomMdt.h', 'QcomScm.h',
    'Tb710fuAddon.h')]
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
    output_seen = False
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            if not replaced:
                command.append(str(source))
                replaced = True
        elif value == '-o':
            if not output_seen:
                command.extend(['-o', str(output)])
                output_seen = True
            index += 1
        elif value in ('-MF', '-MT', '-MQ', '-MJ'):
            index += 1
        elif value in ('-MD', '-MMD', '-MP'):
            pass
        elif value == '-Xclang' and index + 1 < len(arguments) and arguments[index + 1].startswith('-W'):
            index += 1
        elif value.startswith('-Wno-'):
            command.append(value)
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    if not replaced:
        raise SystemExit(f'{record["file"]}: compile record does not name its source')
    if not output_seen:
        command.extend(['-o', str(output)])
    command = ['--target=' + TARGET if value.startswith('--target=') else value for value in command]
    command += ['-Wall', '-Wextra', '-Werror', '-Wno-overriding-option', '-Wno-unused-parameter']
    for include in includes:
        command += ['-I', str(include)]
    command += extra
    return command


def invalidate_link(digest):
    records = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    target = records / 'mach.development.bcm2837.unstripped.noctf'
    stamp = records / '.tb710fu-adsp-link-input.json'
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

    for record in (RECORD_C, RECORD_CPP, RECORD_BSD):
        if not record.is_file():
            raise SystemExit(f'{record} missing; build the minimal XNU kernel first')
    c_record = json.loads(RECORD_C.read_text().strip().rstrip(','))
    cpp_record = json.loads(RECORD_CPP.read_text().strip().rstrip(','))
    bsd_record = json.loads(RECORD_BSD.read_text().strip().rstrip(','))

    adapter = helper('tb710fu_ufs_adapter', device / 'scripts/build-tb710fu-ufs-adapter.py')
    target = helper('tb710fu_source_target', framework / 'scripts/kernel_target.py')

    OUT.mkdir(parents=True, exist_ok=True)
    archive = OUT / 'libTb710fuAdsp.a'
    archive.unlink(missing_ok=True)
    objects, commands = [], {}
    # The compile records already carry the EXPORT_HDRS and patched-source directories.  Adding the
    # patched osfmk/libkern roots again makes <kern/ipc_tt.h> resolve twice (the source copy and the
    # exported copy), and that header has no include guard, so keep the record's own search order.
    includes = [PACKAGE, POWER]

    for source in C_SOURCES:
        output = OUT / (source.stem + '.o')
        record = bsd_record if source.name == 'Tb710fuAdspFirmware.c' else c_record
        command = compile_command(record, source, output, includes,
                                  ['-mgeneral-regs-only', '-Wno-implicit-fallthrough'])
        commands[source.name] = command
        result = subprocess.run(command, cwd=c_record['directory'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (OUT / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(f'{source.name}:\nCMD: {" ".join(command)}\nRECORD: {c_record["arguments"]}\n{result.stdout[-6000:]}')
        target.check_object(output.read_bytes(), source.name)
        objects.append(output)

    for source in CPP_SOURCES:
        output = OUT / (source.stem + '.o')
        command = adapter.compile_command(cpp_record, source, output, includes, [])
        command = ['--target=' + TARGET if value.startswith('--target=') else value for value in command]
        command += ['-Wno-unused-parameter', '-Wno-implicit-fallthrough',
                    '-Wno-missing-designated-field-initializers']
        commands[source.name] = command
        result = subprocess.run(command, cwd=cpp_record['directory'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (OUT / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(f'{source.name}:\nCMD: {" ".join(command)}\nRECORD: {cpp_record["arguments"]}\n{result.stdout[-6000:]}')
        target.check_object(output.read_bytes(), source.name)
        objects.append(output)

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
        'provenance': 'package/Tb710fuAdsp/Tb710fuAdsp.cpp header; UniXNU-sheng (BSD-2-Clause-Patent)',
        'device_battery_display': False,  # needs the firmware files installed and an on-device adsp=2 run
    }
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    if invalidate_link(digest):
        print('TB710FU ADSP archive changed: invalidated the XNU link target')
    print(f'TB710FU ADSP archive passed: {archive} ({digest[:12]})')


if __name__ == '__main__':
    sys.exit(main())
