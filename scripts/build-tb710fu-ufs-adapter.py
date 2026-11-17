#!/usr/bin/env python3
"""Compile the pinned ShengUfs source with the TB710FU SM8650 match adapter."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PIN = '99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0'
SOURCE = ROOT / 'refer/ShengUfsPort'
PATCH = ROOT / 'patches/ufs/0001-sm8650-compatible.patch'
OUT = ROOT / 'build/tb710fu-ufs-adapter'
OBJECT_RECORD = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
RECORD = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compile_command(record, source, output, include_dirs, flags):
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
        elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    if not replaced:
        raise SystemExit('XNU C++ compile record does not contain its source argument')
    command += flags + ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                        '-Wno-c99-designator', '-Wno-reorder-init-list',
                        '-Wno-deprecated-enum-enum-conversion',
                        '-Wno-unnecessary-virtual-specifier', '-mgeneral-regs-only']
    for directory in include_dirs:
        command += ['-I', str(directory)]
    command += ['-MD', '-MF', str(output.with_suffix('.d'))]
    return command


def main():
    if not SOURCE.is_dir():
        raise SystemExit(f'{SOURCE} missing; the pinned ShengUfs feed was not fetched')
    revision = subprocess.check_output(['git', '-C', str(SOURCE), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != PIN:
        raise SystemExit(f'ShengUfs revision mismatch: expected {PIN}, found {revision}')
    for path in (OBJECT_RECORD, RECORD):
        if not path.is_file():
            raise SystemExit(f'{path} missing; build the native macOS 27 kernel first')
    if not (ROOT / 'build/storage-abi/include/IOKit/storage/IOBlockStorageDevice.h').is_file():
        raise SystemExit('official IOStorageFamily headers are missing from the native kernel build')

    OUT.mkdir(parents=True, exist_ok=True)
    cpp_record = json.loads(OBJECT_RECORD.read_text().strip().rstrip(','))
    with tempfile.TemporaryDirectory(prefix='tb710fu-ufs-') as temporary:
        stage = Path(temporary) / 'ShengUfs'
        shutil.copytree(SOURCE, stage, ignore=shutil.ignore_patterns('.git'))
        subprocess.run(['git', 'apply', '--check', str(PATCH)], cwd=stage, check=True)
        subprocess.run(['git', 'apply', str(PATCH)], cwd=stage, check=True)
        source = stage / 'NativeUfs.cpp'
        output = OUT / 'NativeUfs.o'
        flags = ['-DNATIVE_UFS_SOURCE_API=1', '-DNATIVE_VERIFIED_BLOCK=1',
                 '-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1', '-DNATIVE_VERIFIED_POLICY_HANDOFF=1']
        target = __import__('kernel_target')
        if os.environ.get('UNIXNU_XNU_TARGET') == 'macosx':
            osx = OUT / 'macos-target.h'
            osx.write_text('#include <TargetConditionals.h>\n#undef TARGET_OS_OSX\n#define TARGET_OS_OSX 1\n')
            flags += ['-include', str(osx)]
        include_dirs = [OUT, ROOT / 'build/storage-abi/include',
                        ROOT / 'kits/VerifiedRoot/include', ROOT / 'drivers/VirtioMmioBlock',
                        ROOT / '.cache/xnu-work/osfmk']
        command = compile_command(cpp_record, source, output, include_dirs, flags)
        (OUT / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
        result = subprocess.run(command, cwd=cpp_record['directory'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (OUT / 'compile.log').write_text(result.stdout)
        if result.returncode:
            raise SystemExit(result.stdout[-6000:])
        target.check_object(output.read_bytes(), 'TB710FU NativeUfs')
        archive = OUT / 'libTb710fuNativeUfs.a'
        archive.unlink(missing_ok=True)
        subprocess.run(['xcrun', 'ar', 'rcs', archive, output], check=True)
        report = {
            'upstream_revision': revision,
            'upstream_source_sha256': sha(SOURCE / 'NativeUfs.cpp'),
            'board_patch': str(PATCH.relative_to(ROOT)),
            'board_patch_sha256': sha(PATCH),
            'object_sha256': sha(output),
            'archive': str(archive),
            'archive_sha256': sha(archive),
            'xnu_compile_record_sha256': sha(OBJECT_RECORD),
            'target': target.TRIPLE,
            'linked_into_kernel': False,
            'root_policy_ready': False,
            'runtime_qualified': False,
        }
        (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(f'TB710FU NativeUfs compile passed: {archive}')


if __name__ == '__main__':
    main()
