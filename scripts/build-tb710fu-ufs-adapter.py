#!/usr/bin/env python3
"""Compile the pinned ShengUfs source with the TB710FU SM8650 match adapter."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PIN = '99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0'
PATCH = ROOT / 'patches/ufs/0001-sm8650-compatible.patch'


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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', type=Path, help='resolved framework tree from the native kernel build')
    parser.add_argument('--framework', type=Path, help='pinned UniXNU checkout supplying unselected interface headers')
    args = parser.parse_args()
    if args.tree:
        tree = args.tree.resolve()
    else:
        resolved = ROOT / '.work/resolved-work.txt'
        if not resolved.is_file():
            parser.error('--tree is required unless .work/resolved-work.txt exists')
        tree = Path(resolved.read_text().strip()).resolve() / 'tree'
    upstream_source = tree / 'refer/ShengUfsPort'
    framework = (args.framework or ROOT / '.work/UniXNU').resolve()
    output_dir = tree / 'build/tb710fu-ufs-adapter'
    object_record = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    c_record_path = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'
    if not upstream_source.is_dir():
        raise SystemExit(f'{upstream_source} missing; the pinned ShengUfs feed was not fetched')
    revision = subprocess.check_output(['git', '-C', str(upstream_source), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != PIN:
        raise SystemExit(f'ShengUfs revision mismatch: expected {PIN}, found {revision}')
    for path in (object_record, c_record_path):
        if not path.is_file():
            raise SystemExit(f'{path} missing; build the native macOS 27 kernel first')
    if not (tree / 'build/storage-abi/include/IOKit/storage/IOBlockStorageDevice.h').is_file():
        raise SystemExit('official IOStorageFamily headers are missing from the native kernel build')
    verified_root = tree / 'kits/VerifiedRoot' if (tree / 'kits/VerifiedRoot').is_dir() else framework / 'kits/VerifiedRoot'
    virtio_block = tree / 'drivers/VirtioMmioBlock' if (tree / 'drivers/VirtioMmioBlock').is_dir() else framework / 'drivers/VirtioMmioBlock'
    output_dir.mkdir(parents=True, exist_ok=True)
    include_dirs = [output_dir, tree / 'build/storage-abi/include', verified_root / 'include',
                    virtio_block, tree / '.cache/xnu-work/osfmk']
    missing = [path for path in include_dirs if not path.is_dir()]
    if missing:
        raise SystemExit('Required pinned UFS interface headers are unavailable: ' + ', '.join(map(str, missing)))
    cpp_record = json.loads(object_record.read_text().strip().rstrip(','))
    with tempfile.TemporaryDirectory(prefix='tb710fu-ufs-') as temporary:
        stage = Path(temporary) / 'ShengUfs'
        shutil.copytree(upstream_source, stage, ignore=shutil.ignore_patterns('.git'))
        subprocess.run(['git', 'apply', '--check', str(PATCH)], cwd=stage, check=True)
        subprocess.run(['git', 'apply', str(PATCH)], cwd=stage, check=True)
        source_file = stage / 'NativeUfs.cpp'
        output = output_dir / 'NativeUfs.o'
        flags = ['-DNATIVE_UFS_SOURCE_API=1', '-DNATIVE_VERIFIED_BLOCK=1',
                 '-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1', '-DNATIVE_VERIFIED_POLICY_HANDOFF=1']
        target_path = framework / 'scripts/kernel_target.py'
        spec = importlib.util.spec_from_file_location('tb710fu_ufs_kernel_target', target_path)
        if spec is None or spec.loader is None:
            raise SystemExit(f'Pinned framework target verifier is unavailable: {target_path}')
        target = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(target)
        if os.environ.get('UNIXNU_XNU_TARGET') == 'macosx':
            osx = output_dir / 'macos-target.h'
            osx.write_text('#include <TargetConditionals.h>\n#undef TARGET_OS_OSX\n#define TARGET_OS_OSX 1\n')
            flags += ['-include', str(osx)]
        command = compile_command(cpp_record, source_file, output, include_dirs, flags)
        (output_dir / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
        result = subprocess.run(command, cwd=cpp_record['directory'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (output_dir / 'compile.log').write_text(result.stdout)
        if result.returncode:
            raise SystemExit(result.stdout[-6000:])
        target.check_object(output.read_bytes(), 'TB710FU NativeUfs')
        dependency_text = output.with_suffix('.d').read_text().replace('\\\n', ' ')
        dependencies = {}
        for name in shlex.split(dependency_text.split(':', 1)[1]):
            path = Path(name)
            if not path.is_absolute():
                path = Path(cpp_record['directory']) / path
            if not path.is_file():
                raise SystemExit(f'Compiler dependency disappeared before provenance capture: {path}')
            dependencies[str(path.resolve())] = sha(path)
        archive = output_dir / 'libTb710fuNativeUfs.a'
        archive.unlink(missing_ok=True)
        subprocess.run(['xcrun', 'ar', 'rcs', archive, output], check=True)
        report = {
            'upstream_revision': revision,
            'upstream_source_sha256': sha(upstream_source / 'NativeUfs.cpp'),
            'board_patch': str(PATCH.relative_to(ROOT)),
            'board_patch_sha256': sha(PATCH),
            'included_inputs_sha256': dependencies,
            'object_sha256': sha(output),
            'archive': str(archive),
            'archive_sha256': sha(archive),
            'xnu_compile_record_sha256': sha(object_record),
            'target': target.TRIPLE,
            'linked_into_kernel': False,
            'root_policy_ready': False,
            'runtime_qualified': False,
        }
        (output_dir / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(f'TB710FU NativeUfs compile passed: {archive}')


if __name__ == '__main__':
    main()
