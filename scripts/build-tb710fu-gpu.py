#!/usr/bin/env python3
"""Build the TB710FU GPU archive (Milestone 2: IOAccelerator + ABI user client + staged gpu-power).

Same schema as the board's other archives: every translation unit is compiled with the actual IOKit C++ record
(arm64e-apple-ios27.0), the metaclass constructor of each unit that has one is moved out of __mod_init_func so
Macos27Services.cpp can install it exactly once between preModLoad and postModLoad, the objects are PAC/vtable
checked with the framework's kernel_target helper, and the XNU link target is invalidated when the archive changes.

A translation unit without a C++ static initialiser (the MMIO and firmware helpers) simply has no __mod_init_func
and is not deferred; the two with metaclasses (accelerator and power domain) and the user client are.

The archive publishes nothing unless gpu-metal=1 or gpu-power=<n>: the default boot path stays identical to the
zero-wobble baseline. Host-side source checks run separately (scripts/check-tb710fu-gpu-host.py).
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
PACKAGE = DEVICE / 'package/Tb710fuGpu'
ADSP_PACKAGE = DEVICE / 'package/Tb710fuAdsp'
OUT = ROOT / 'build/tb710fu-gpu'

RECORD_CPP = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
CPP_SOURCES = [
    PACKAGE / 'Tb710fuGpuAccelerator.cpp',
    PACKAGE / 'Tb710fuGpuPower.cpp',
    PACKAGE / 'Tb710fuGpuMmio.cpp',
    PACKAGE / 'Tb710fuGpuFirmware.cpp',
    PACKAGE / 'Tb710fuGpuClient.cpp',
]
HEADERS = [PACKAGE / name for name in (
    'Tb710fuGpu.h', 'Tb710fuGpuABI.h', 'Tb710fuGpuHw.h', 'Tb710fuGpuMmio.h', 'Tb710fuGpuFirmware.h',
    'Tb710fuGpuClient.h', 'Tb710fuGpuNativeABI.h')] + [ADSP_PACKAGE / 'Tb710fuAddon.h']
TARGET = 'arm64e-apple-ios27.0'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def reported(path):
    for base in (DEVICE, ROOT):
        try:
            return str(path.relative_to(base))
        except ValueError:
            pass
    return str(path)


def helper(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def defer_constructor(obj, late, init):
    """Move the C++ static initialiser section to __mod_deferred and export the symbol.

    preModLoad then runs it on request; left in __mod_init_func it would run at
    kernel entry, before IOGraphicsFamily exists (the parent metaclass is null at
    that point and OSMetaClass would fault on the superclass pointer).

    A source with no dynamic initialiser (no metaclass, no runtime globals) has no
    __mod_init_func at all; it is copied unchanged instead of failing the build."""
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
        late.write_bytes(bytes(data))
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


def invalidate_link(digest):
    records = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    target = records / 'mach.development.bcm2837.unstripped.noctf'
    stamp = records / '.tb710fu-gpu-link-input.json'
    text = json.dumps({'archive': 'build/tb710fu-gpu/libTb710fuGpu.a', 'archive_sha256': digest}, indent=2) + '\n'
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
    if not RECORD_CPP.is_file():
        raise SystemExit(f'{RECORD_CPP} missing; build the minimal XNU kernel first')

    cpp_record = json.loads(RECORD_CPP.read_text().strip().rstrip(','))
    adapter = helper('tb710fu_ufs_adapter', device / 'scripts/build-tb710fu-ufs-adapter.py')
    target = helper('tb710fu_source_target', framework / 'scripts/kernel_target.py')

    OUT.mkdir(parents=True, exist_ok=True)
    archive = OUT / 'libTb710fuGpu.a'
    archive.unlink(missing_ok=True)
    objects, commands = [], {}
    includes = [PACKAGE, ADSP_PACKAGE, tree / '.cache/xnu-work/osfmk', tree / '.cache/xnu-work/libkern']

    for source in CPP_SOURCES:
        output = OUT / (source.stem + '.o')
        command = adapter.compile_command(cpp_record, source, output, includes, [])
        command = ['--target=' + TARGET if value.startswith('--target=') else value for value in command]
        commands[source.name] = command
        result = subprocess.run(command, cwd=cpp_record['directory'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (OUT / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(f'{source.name}:\n{result.stdout[-6000:]}')
        target.check_object(output.read_bytes(), source.name)
        late = output.with_suffix('.late.o')
        deferred = defer_constructor(output, late, '__GLOBAL__sub_I_' + source.name)
        target.check_object(late.read_bytes(), source.name + ' deferred')
        objects.append(late)
        if deferred:
            print(f'{source.name}: metaclass constructor deferred to preModLoad')

    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
    digest = sha(archive)
    report = {
        'archive': reported(archive),
        'archive_sha256': digest,
        'framework_revision': revision,
        'objects': {obj.name: sha(obj) for obj in objects},
        'sources': {reported(path): sha(path) for path in CPP_SOURCES + HEADERS},
        'compile_commands_sha256': {name: hashlib.sha256(json.dumps(cmd).encode()).hexdigest()
                                    for name, cmd in commands.items()},
        'host_checks': 'scripts/check-tb710fu-gpu-host.py',
        'boot_arguments': {'metal': 'gpu-metal=1', 'power': 'gpu-power=<1..9>', 'addon': 'gpu-addon=1'},
        'metal_device': False,  # discovery + user-client ABI; the device still needs the GMU cold start and GX
        'user_client': 'AGXDeviceUserClient (type 0x4147, NativeAdrenoABI v2)',
        'staged_power': [f'gpu-power={level}' for level in range(1, 10)],
    }
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    if invalidate_link(digest):
        print('TB710FU GPU archive changed: invalidated the XNU link target')
    print(f'TB710FU GPU archive passed: {archive} ({digest[:12]})')


if __name__ == '__main__':
    sys.exit(main())
