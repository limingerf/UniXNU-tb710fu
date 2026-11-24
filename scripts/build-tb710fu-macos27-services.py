#!/usr/bin/env python3
"""Build TB710FU macOS 27 OS services archive (AppleKeyStoreState + board hooks).

Follows the pinned schema2 device archive pattern (build-tb710fu-source-storage):
verifies the pinned UniXNU gitlink for drivers/AppleKeyStoreState, compiles with
the macOS 27 ABI (arm64e-apple-ios27.0 + TARGET_OS_OSX=1), defers the IOKit
metaclass constructor, and invalidates the XNU link target when the archive changes.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def revision(path):
    top = Path(subprocess.check_output(
        ['git', '-C', str(path), 'rev-parse', '--show-toplevel'], text=True).strip()).resolve()
    if top != path.resolve():
        raise SystemExit(f'Initialize the pinned component before native build: {path}')
    return subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()


def helper(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


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


def main():
    device = Path(os.environ['UNIXNU_DEVICE']).resolve()
    tree = Path.cwd().resolve()
    catalog = tree / 'components.toml'
    if not catalog.is_symlink():
        raise SystemExit('Expected the fixed framework catalog link in the resolved tree')
    framework = catalog.resolve().parent
    lock = json.loads((device / 'upstream.lock.json').read_text())
    if revision(framework) != lock['unixnu']['commit']:
        raise SystemExit('Framework revision differs from upstream.lock.json')
    aks_dir = framework / 'drivers/AppleKeyStoreState'
    expected_aks = subprocess.check_output(
        ['git', '-C', str(framework), 'rev-parse', lock['unixnu']['commit'] + ':drivers/AppleKeyStoreState'],
        text=True).strip()
    if revision(aks_dir) != expected_aks:
        raise SystemExit('Initialize the pinned drivers/AppleKeyStoreState submodule before native build')

    records = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    cpp_record_path = records / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    if not cpp_record_path.is_file():
        raise SystemExit(f'Missing compile record: {cpp_record_path}')
    cpp_record = json.loads(cpp_record_path.read_text().strip().rstrip(','))

    adapter = helper('tb710fu_ufs_adapter', device / 'scripts/build-tb710fu-ufs-adapter.py')
    target = helper('tb710fu_source_target', framework / 'scripts/kernel_target.py')

    output = tree / 'build/tb710fu-macos27-services'
    output.mkdir(parents=True, exist_ok=True)
    archive = output / 'libTb710fuMacos27Services.a'
    archive.unlink(missing_ok=True)

    includes = [output, aks_dir, tree / '.cache/xnu-work/osfmk', tree / '.cache/xnu-work/libkern']
    flags = []
    sources = [
        aks_dir / 'NativeKeyStoreEvents.cpp',
        device / 'package/AppleKeyStoreHooks.cpp',
        device / 'package/Macos27Services.cpp',
    ]
    objects = []
    for path in sources:
        obj = output / (path.stem + '.o')
        command = adapter.compile_command(cpp_record, path, obj, includes, flags)
        command = ['--target=arm64e-apple-ios27.0' if v.startswith('--target=') else v for v in command]
        result = subprocess.run(command, cwd=cpp_record['directory'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (output / (path.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(f'{path.name}:\n{result.stdout[-6000:]}')
        target.check_object(obj.read_bytes(), path.name)
        if path.name == 'NativeKeyStoreEvents.cpp':
            init = '__GLOBAL__sub_I_' + path.name
            late = obj.with_suffix('.late.o')
            defer_constructor(obj, late, init)
            target.check_object(late.read_bytes(), path.name + ' deferred')
            obj = late
        objects.append(obj)

    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
    digest = sha(archive)
    (output / 'build-report.json').write_text(json.dumps({
        'archive': str(archive),
        'archive_sha256': digest,
        'aks_revision': expected_aks,
        'objects_sha256': {o.name: sha(o) for o in objects},
    }, indent=2) + '\n')

    # Invalidate final XNU link target when archive changes
    link_target = records / 'mach.development.bcm2837.unstripped.noctf'
    stamp = records / '.tb710fu-macos27-services-link-input.json'
    stamp_text = json.dumps({'archive_sha256': digest}, indent=2) + '\n'
    prev = stamp.read_text() if stamp.is_file() else ''
    if prev != stamp_text:
        link_target.unlink(missing_ok=True)
        with tempfile.NamedTemporaryFile(mode='w', dir=records, prefix=stamp.name + '.', delete=False) as tf:
            tmp = Path(tf.name)
            tf.write(stamp_text)
        os.replace(tmp, stamp)
        print(f'TB710FU macOS 27 services archive changed: invalidated {link_target}')
    print(f'TB710FU macOS 27 services archive passed: {archive} ({digest[:12]})')


if __name__ == '__main__':
    main()
