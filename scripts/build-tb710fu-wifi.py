#!/usr/bin/env python3
"""Build the TB710FU Wi-Fi archive (SM8650 WCN7850 over PCIe0 + MHI, package/Tb710fuWifi).

Same pinned-schema2 archive pattern as scripts/build-tb710fu-adsp.py: the portable C layers (board/TLMM/iATU/BAR
planning, MHI state machine, firmware manifest, QRTR/QMI/HTC/WMI encoders, VFS firmware reader) compile with the XNU C
or BSD record, the kernel driver/PCIe/network files with the IOKit C++ record, and a changed archive invalidates the XNU
link target. Tb710fuWifiSlots.h is generated from the verified 26C5035r IONetworkingFamily fixture, exactly as
build-tb710fu-macos27-services.py does for Tb710fuEthernetSlots.h.
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
PACKAGE = DEVICE / 'package/Tb710fuWifi'
OUT = ROOT / 'build/tb710fu-wifi'


def reported(path):
    for base in (DEVICE, ROOT):
        try:
            return str(path.relative_to(base))
        except ValueError:
            pass
    return str(path)


RECORD_C = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'
RECORD_CPP = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
RECORD_BSD = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/crc32.o.json'
C_SOURCES = [PACKAGE / name for name in (
    'Tb710fuWifiBoard.c', 'Tb710fuWifiMhi.c', 'Tb710fuWifiManifest.c', 'Tb710fuWifiProto.c')]
BSD_SOURCES = [PACKAGE / 'Tb710fuWifiFirmware.c']
CPP_SOURCES = [PACKAGE / name for name in (
    'Tb710fuWifiPcie.cpp', 'Tb710fuWifiDriver.cpp', 'Tb710fuWifiNet.cpp')]
HEADERS = [PACKAGE / name for name in (
    'Tb710fuWifiRegs.h', 'Tb710fuWifiBoard.h', 'Tb710fuWifiMhi.h', 'Tb710fuWifiManifest.h',
    'Tb710fuWifiProto.h', 'Tb710fuWifiFirmware.h', 'Tb710fuWifiPcie.h', 'Tb710fuWifiPcieTables.h')]
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


def generate_slots(output):
    fixture = DEVICE / 'scripts/net-ether-26C5035r.json'
    if not fixture.is_file():
        raise SystemExit(f'Missing Ethernet ABI fixture: {fixture}')
    abi = json.loads(fixture.read_text())
    slots = abi['slots']
    bindings = abi['bindings']
    diversities = ', '.join(f'{int(d, 16):#06x}' for _, d in slots)
    path = output / 'Tb710fuWifiSlots.h'
    path.write_text(
        '#pragma once\n'
        '#include <stdint.h>\n'
        f'#define TB710FU_WIFI_METACLASS_VA {int(bindings["__ZN20IOEthernetController10gMetaClassE"], 16):#x}ULL\n'
        f'#define TB710FU_WIFI_VTABLE_VA {int(abi["vtable"], 16):#x}ULL\n'
        f'#define TB710FU_WIFI_C2_VA {int(bindings["__ZN20IOEthernetControllerC2EPK11OSMetaClass"], 16):#x}ULL\n'
        f'#define TB710FU_WIFI_INSTANCE_SIZE {int(abi["instance_size"], 16):#x}U\n'
        f'#define TB710FU_WIFI_SLOT_COUNT {len(slots)}U\n'
        f'static const uint16_t tb710fu_wifi_slot_diversity[{len(slots)}] = {{ {diversities} }};\n'
    )
    return path


def invalidate_link(digest):
    records = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    target = records / 'mach.development.bcm2837.unstripped.noctf'
    stamp = records / '.tb710fu-wifi-link-input.json'
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
    archive = OUT / 'libTb710fuWifi.a'
    archive.unlink(missing_ok=True)
    slots = generate_slots(OUT)
    objects, commands = [], {}
    includes = [PACKAGE, OUT]

    for source in C_SOURCES + BSD_SOURCES:
        output = OUT / (source.stem + '.o')
        record = bsd_record if source in BSD_SOURCES else c_record
        command = compile_command(record, source, output, includes,
                                  ['-mgeneral-regs-only', '-Wno-implicit-fallthrough'])
        commands[source.name] = command
        result = subprocess.run(command, cwd=record['directory'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (OUT / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(f'{source.name}:\nCMD: {" ".join(command)}\n'
                             f'RECORD: {record["arguments"]}\n{result.stdout[-6000:]}')
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
            raise SystemExit(f'{source.name}:\nCMD: {" ".join(command)}\n'
                             f'RECORD: {cpp_record["arguments"]}\n{result.stdout[-6000:]}')
        target.check_object(output.read_bytes(), source.name)
        objects.append(output)

    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
    digest = sha(archive)
    report = {
        'archive': str(archive),
        'archive_sha256': digest,
        'framework_revision': revision,
        'objects': {obj.name: sha(obj) for obj in objects},
        'sources': {reported(path): sha(path) for path in C_SOURCES + BSD_SOURCES + CPP_SOURCES + HEADERS + [slots]},
        'compile_commands_sha256': {name: hashlib.sha256(json.dumps(cmd).encode()).hexdigest()
                                    for name, cmd in commands.items()},
        'provenance': 'package/Tb710fuWifi/Tb710fuWifiRegs.h header; DTS sm8650-lenovo-tb710fu.dts/sm8650.dtsi; '
                      'UniXNU-sheng (BSD-2-Clause-Patent) for the PCIe/MHI sequence',
        'device_wifi': False,  # needs the firmware installed (addon or Data volume) and an on-device wifi=1..5 run
    }
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    if invalidate_link(digest):
        print('TB710FU Wi-Fi archive changed: invalidated the XNU link target')
    print(f'TB710FU Wi-Fi archive passed: {archive} ({digest[:12]})')


if __name__ == '__main__':
    sys.exit(main())
