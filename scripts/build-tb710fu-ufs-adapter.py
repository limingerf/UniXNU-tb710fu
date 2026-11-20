#!/usr/bin/env python3
"""Compile the pinned ShengUfs source with the TB710FU SM8650 match adapter."""
import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PIN = '99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0'
PATCHES = (
    ROOT / 'patches/ufs/0001-sm8650-compatible.patch',
    ROOT / 'patches/ufs/0002-transactional-handoff.patch',
    # Unconditional host bring-up and root-layout stage trace. Ordered after the
    # transactional handoff so it annotates the final code the device runs.
    ROOT / 'patches/ufs/0004-bringup-stage-trace.patch',
    ROOT / 'patches/ufs/0005-synchronous-bringup-checkpoints.patch',
    # hostBringUp drives the UFS command path with IODelay while interrupts are
    # masked, so its two IOSleeps can never be woken. The 03:52 device run
    # stopped inside IOSleep(50) after 'wake-settle-begin'. Behaviour fix, so it
    # is a separate patch from the trace-only 0004.
    ROOT / 'patches/ufs/0006-quiescent-sleep-replacement.patch',
    # 0006 still stopped at the same checkpoint: in a thread IODelay is a timer
    # wait, not a busy wait. 0007 replaces the settle and the command poll delay
    # with a bounded spin on mach_absolute_time. Behaviour fix, separate again.
    ROOT / 'patches/ufs/0007-quiescent-counter-spin.patch',
)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stage_source(upstream_source, stage, patches=PATCHES):
    # Export the pinned commit, so host checks and kernel compilation cannot
    # accidentally include local cache edits, untracked files or CRLF checkout differences.
    archive = subprocess.check_output(['git', '-C', str(upstream_source), 'archive', PIN])
    with tarfile.open(fileobj=io.BytesIO(archive)) as source_archive:
        source_archive.extractall(stage, filter='data')
    for patch in patches:
        subprocess.run(['git', 'apply', '--check', str(patch)], cwd=stage, check=True)
        subprocess.run(['git', 'apply', str(patch)], cwd=stage, check=True)


def run_host_tests(stage, output_dir, patches=PATCHES):
    compiler = shlex.split(os.environ.get('CXX', 'c++'))
    version = subprocess.check_output([*compiler, '--version'], text=True).strip()
    report = {'compiler': version, 'upstream_revision': PIN, 'tests': [],
              'board_patches': [{'path': str(p.relative_to(ROOT)), 'sha256': sha(p)} for p in patches]}
    for name in ('SmmuTakeover', 'RootCommands'):
        source = stage / 'tests' / f'{name}.cpp'
        executable = stage / f'{name}-host-test'
        command = [*compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-parameter', str(source), '-o', str(executable)]
        compiled = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (output_dir / f'{name}-host-compile.log').write_text(compiled.stdout)
        if compiled.returncode:
            raise SystemExit(compiled.stdout[-6000:])
        tested = subprocess.run([str(executable)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (output_dir / f'{name}-host-test.log').write_text(tested.stdout)
        if tested.returncode:
            raise SystemExit(f'{name} host test failed ({tested.returncode}):\n{tested.stdout[-6000:]}')
        print(tested.stdout, end='')
        report['tests'].append({'source': f'tests/{name}.cpp', 'command': command, 'passed': True})
    report['inputs_sha256'] = {
        str(path.relative_to(stage)): sha(path)
        for path in sorted(stage.rglob('*')) if path.suffix in ('.cpp', '.h')
    }
    (output_dir / 'host-tests.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


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
    parser.add_argument('--source', type=Path, help='explicit pinned ShengUfs checkout for host-only checks')
    parser.add_argument('--host-tests-only', action='store_true', help='apply device patches and run host tests without a kernel build')
    args = parser.parse_args()
    if args.tree:
        tree = args.tree.resolve()
    else:
        resolved = ROOT / '.work/resolved-work.txt'
        if not resolved.is_file():
            parser.error('--tree is required unless .work/resolved-work.txt exists')
        tree = Path(resolved.read_text().strip()).resolve() / 'tree'
    upstream_source = (args.source or tree / 'refer/ShengUfsPort').resolve()
    framework = (args.framework or ROOT / '.work/UniXNU').resolve()
    output_dir = tree / 'build/tb710fu-ufs-adapter'
    object_record = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    c_record_path = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'
    if not upstream_source.is_dir():
        raise SystemExit(f'{upstream_source} missing; the pinned ShengUfs feed was not fetched')
    revision = subprocess.check_output(['git', '-C', str(upstream_source), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != PIN:
        raise SystemExit(f'ShengUfs revision mismatch: expected {PIN}, found {revision}')
    output_dir.mkdir(parents=True, exist_ok=True)
    if args.host_tests_only:
        with tempfile.TemporaryDirectory(prefix='tb710fu-ufs-host-') as temporary:
            stage = Path(temporary) / 'ShengUfs'
            stage_source(upstream_source, stage)
            run_host_tests(stage, output_dir)
        return
    for path in (object_record, c_record_path):
        if not path.is_file():
            raise SystemExit(f'{path} missing; build the native macOS 27 kernel first')
    if not (tree / 'build/storage-abi/include/IOKit/storage/IOBlockStorageDevice.h').is_file():
        raise SystemExit('official IOStorageFamily headers are missing from the native kernel build')
    verified_root = tree / 'kits/VerifiedRoot' if (tree / 'kits/VerifiedRoot').is_dir() else framework / 'kits/VerifiedRoot'
    virtio_block = tree / 'drivers/VirtioMmioBlock' if (tree / 'drivers/VirtioMmioBlock').is_dir() else framework / 'drivers/VirtioMmioBlock'
    include_dirs = [output_dir, tree / 'build/storage-abi/include', verified_root / 'include',
                    virtio_block, tree / '.cache/xnu-work/osfmk']
    missing = [path for path in include_dirs if not path.is_dir()]
    if missing:
        raise SystemExit('Required pinned UFS interface headers are unavailable: ' + ', '.join(map(str, missing)))
    cpp_record = json.loads(object_record.read_text().strip().rstrip(','))
    with tempfile.TemporaryDirectory(prefix='tb710fu-ufs-') as temporary:
        stage = Path(temporary) / 'ShengUfs'
        stage_source(upstream_source, stage)
        host_tests = run_host_tests(stage, output_dir)
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
            'upstream_source_sha256': hashlib.sha256(subprocess.check_output(
                ['git', '-C', str(upstream_source), 'show', f'{PIN}:NativeUfs.cpp'])).hexdigest(),
            'board_patches': [
                {'path': str(patch.relative_to(ROOT)), 'sha256': sha(patch)}
                for patch in PATCHES
            ],
            'host_tests': host_tests,
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
