#!/usr/bin/env python3
"""Compile the minimal authenticated source-storage archive; never link or boot it.

Device-owned adaptation of the pinned official build-sheng-source-storage.py.
The source/headers, late metaclasses, real VerifiedRoot crypto and independent
owner anchor follow that recipe. Optional RAM/Data storage, benchmark, file log,
process diagnostics and wireless providers are omitted as a unit. No weak
defaults or substitute security implementations are generated.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import tempfile
import tomllib
import uuid


ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / 'upstream.lock.json').read_text())
STORAGE_PIN = '7edb88fbae296fb7c8ce2f64e115e116e566d51c'
BRIDGE_REFERENCE = '1108068537d5396edb2c63b2f8c6f5b97b0a4bbc'


def helper(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require_no_eager_constructors(data, name):
    # target.check_object has already checked the Mach-O header/architecture.
    count = struct.unpack_from('<I', data, 16)[0]
    offset = 32
    for _ in range(count):
        command, size = struct.unpack_from('<II', data, offset)
        if command == 0x19:
            sections = struct.unpack_from('<I', data, offset + 64)[0]
            for index in range(sections):
                section = offset + 72 + index * 80
                if data[section:section + 16].rstrip(b'\0') == b'__mod_init_func':
                    raise SystemExit(f'{name}: eager metaclass constructor section remains')
        offset += size


def policy_bundle(framework, output):
    profile = tomllib.loads((framework / 'profiles/os' / LOCK['os_profile'] / 'profile.toml').read_text())
    if profile['kernel']['prelink']['kernelcache_sha256'] != LOCK['kernelcache_sha256']:
        raise SystemExit('OS profile kernelcache differs from the device source lock')
    pins = {item['name']: item['commit'] for item in profile['source']}
    if pins['IOStorageFamily'] != STORAGE_PIN:
        raise SystemExit('OS profile storage headers differ from the device source lock')
    policies = ROOT / 'scripts/policies' / profile['ipsw']['build']
    names = ('OS', 'SystemOS', 'App')
    key = (policies / names[0] / 'trusted-key.bin').read_bytes()
    if (len(key) != 395 or key[:8] != b'VROOTKEY' or not key[8] & 0x80 or
            not key[391] & 1 or key[392:] != bytes((1, 0, 1))):
        raise SystemExit('The independent owner anchor must be the complete RSA-3072 VROOTKEY')
    key_digest = hashlib.sha256(key).hexdigest()
    manifests, records = [], {}
    for name in names:
        directory = policies / name
        path = directory / 'manifest.header.bin'
        data = path.read_bytes()
        verified = json.loads((directory / 'verification.json').read_text())
        prefix = name.lower()
        digest = profile['root'][prefix + '_image_sha256']
        identity = profile['root'][prefix + '_volume_uuid'].upper()
        if len(data) != 544:
            raise SystemExit(f'{name}: require exactly the 544-byte signed header')
        image_bytes, count = struct.unpack_from('<QQ', data, 24)
        generation = struct.unpack_from('<Q', data, 152)[0]
        if (data[:8] != b'VROOT01\0' or struct.unpack_from('<4I', data, 8) != (1, 160, 4096, 0) or
                not image_bytes or count != (image_bytes + 4095) // 4096 or generation < 1 or
                image_bytes != verified['image_bytes'] or count != verified['blocks_verified'] or
                generation != verified['generation'] or str(uuid.UUID(bytes=data[40:56])).upper() != identity or
                data[56:88].hex() != digest or data[120:152].hex() != key_digest or
                (directory / 'trusted-key.bin').read_bytes() != key or
                not verified['real_c_signature_and_block_verification'] or
                verified['image_sha256'] != digest or verified['volume_uuid'] != identity or
                verified['trusted_key_sha256'] != key_digest):
            raise SystemExit(f'{name}: policy/owner/verification record differs from the exact OS profile')
        manifests.append(data)
        records[name] = {'header_sha256': sha(path), 'verification_sha256': sha(directory / 'verification.json'),
                         'image_sha256': digest, 'volume_uuid': identity, 'image_bytes': image_bytes,
                         'block_count': count, 'generation': generation}
    cursor = 32 + 16 * len(manifests)
    table = bytearray()
    for data in manifests:
        table += struct.pack('<QQ', cursor, len(data))
        cursor += len(data)
    bundle = struct.pack('<8sQQQ', b'VROOTB01', 1, len(manifests), cursor) + table + b''.join(manifests)
    path = output / 'root-policies.bin'
    path.write_bytes(bundle)
    anchor = output / 'RootAnchor.c'
    anchor.write_text('#include <stdint.h>\nconst uint8_t verified_root_key[395] = {' +
                      ','.join(str(value) for value in key) + '};\n')
    return anchor, {'bundle_sha256': sha(path), 'bundle_bytes': len(bundle), 'policies': records,
                    'trusted_key_sha256': key_digest,
                    'runtime_authentication_required': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tree', required=True, type=Path)
    parser.add_argument('--framework', type=Path, default=ROOT / '.work/UniXNU')
    args = parser.parse_args()
    tree, framework = args.tree.resolve(), args.framework.resolve()
    revision = subprocess.check_output(['git', '-C', str(framework), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != LOCK['unixnu']['commit']:
        raise SystemExit('Framework revision differs from upstream.lock.json')
    if (os.environ.get('UNIXNU_XNU_ARCH'), os.environ.get('UNIXNU_IOS_VERSION'),
            os.environ.get('UNIXNU_XNU_TARGET')) != ('arm64e', '27.0', 'macosx'):
        raise SystemExit('Run with the pinned macOS 27 profile: arm64e, iOS deployment 27.0, macosx ABI')
    adapter = helper('tb710fu_ufs_adapter', ROOT / 'scripts/build-tb710fu-ufs-adapter.py')
    headers = helper('tb710fu_storage_headers', framework / 'scripts/prepare-storage-headers.py')
    target = helper('tb710fu_source_target', framework / 'scripts/kernel_target.py')
    source = tree / 'refer/ShengUfsPort'
    actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != adapter.PIN:
        raise SystemExit('ShengUfs source revision differs from the pinned adapter')
    output = tree / 'build/tb710fu-source-storage'
    output.mkdir(parents=True, exist_ok=True)
    # An unsuccessful retry must not leave an old success report/archive behind.
    report_path = output / 'build-report.json'
    archive = output / 'libTb710fuSourceStorage.a'
    report_path.unlink(missing_ok=True)
    archive.unlink(missing_ok=True)
    storage = tree / 'refer/IOStorageFamily'
    header_report = headers.prepare(storage, STORAGE_PIN, output / 'headers')
    anchor, policies = policy_bundle(framework, output)
    verified_root = framework / 'kits/VerifiedRoot'
    virtio_block = framework / 'drivers/VirtioMmioBlock'
    interface_revisions = {}
    for directory in (verified_root / 'include', virtio_block):
        if not directory.is_dir():
            raise SystemExit(f'Initialize the pinned interface component first: {directory}')
    for directory in (verified_root, virtio_block):
        relative = directory.relative_to(framework).as_posix()
        gitlink = subprocess.check_output(['git', '-C', str(framework), 'ls-tree', revision, relative], text=True).split()
        if len(gitlink) != 4 or gitlink[:2] != ['160000', 'commit']:
            raise SystemExit(f'Pinned interface is not an official gitlink: {relative}')
        interface_revisions[relative] = headers.pinned_source(directory, gitlink[2])
    original = (storage / 'IOBlockStorageDevice.cpp').read_text()
    macro = 'OSDefineMetaClassAndAbstractStructors(IOBlockStorageDevice,IOService)'
    if original.count(macro) != 1:
        raise SystemExit('Pinned public base registration macro changed')
    base = output / 'IOBlockStorageDevice.cpp'
    base.write_text(original.replace(macro, '''
extern "C" const OSMetaClass *native_source_storage_parent(void);
OSDefineMetaClassWithInit(IOBlockStorageDevice, IOService, )
IOBlockStorageDevice::MetaClass::MetaClass()
    : OSMetaClass("UniXNUBlockStorageDevice", native_source_storage_parent(), sizeof(IOBlockStorageDevice)) {}
OSDefineAbstractStructors(IOBlockStorageDevice, IOService)
OSDefineOperatorMethods(IOBlockStorageDevice)
'''))
    records = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    recipe_paths = {'cpp': records / 'iokit/DEVELOPMENT/IOPlatformExpert.cpo.json',
                    'c': records / 'osfmk/DEVELOPMENT/startup.o.json'}
    recipes = {name: json.loads(path.read_text().strip().rstrip(',')) for name, path in recipe_paths.items()}
    osx = output / 'macos-target.h'
    osx.write_text('#include <TargetConditionals.h>\n#undef TARGET_OS_OSX\n#define TARGET_OS_OSX 1\n')
    includes = [output, output / 'headers/include', verified_root / 'include', virtio_block,
                tree / '.cache/xnu-work/osfmk']
    flags = ['-DNATIVE_UFS_SOURCE_API=1', '-DNATIVE_VERIFIED_BLOCK=1',
             '-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1', '-DNATIVE_VERIFIED_POLICY_HANDOFF=1',
             '-DTB710FU_UFS_NO_BOOT_OBSERVER=1', '-include', str(osx)]
    patches = (*adapter.PATCHES, ROOT / 'patches/ufs/0003-minimal-source-runtime.patch')
    objcopy = shutil.which('llvm-objcopy') or '/opt/homebrew/opt/llvm/bin/llvm-objcopy'
    objects, inputs, deferred = [], {}, {}
    with tempfile.TemporaryDirectory(prefix='tb710fu-source-storage-') as temporary:
        stage = Path(temporary) / 'ShengUfs'
        adapter.stage_source(source, stage, patches=patches)
        host_tests = adapter.run_host_tests(stage, output, patches=patches)
        sources = [base, stage / 'NativeUfs.cpp', ROOT / 'package/SourceStorage.cpp',
                   ROOT / 'package/SourceRootPolicies.cpp', ROOT / 'package/SourceRootBinding.cpp',
                   verified_root / 'src/verified_root.c', verified_root / 'adapters/xnu_crypto.c', anchor]
        for path in sources:
            record = recipes['c' if path.suffix == '.c' else 'cpp']
            obj = output / (path.stem + '.o')
            command = adapter.compile_command(record, path, obj, includes, flags)
            # The public base retains upstream warning behavior, as in the
            # pinned official recipe. Warnings are preserved in its log.
            if path == base:
                command.append('-Wno-error')
            (output / (path.stem + '.command.json')).write_text(json.dumps(command, indent=2) + '\n')
            result = subprocess.run(command, cwd=record['directory'], text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (output / (path.stem + '.log')).write_text(result.stdout)
            if result.returncode:
                raise SystemExit(f'{path.name}:\n{result.stdout[-6000:]}')
            target.check_object(obj.read_bytes(), path.name)
            dependency_text = obj.with_suffix('.d').read_text().replace('\\\n', ' ')
            for name in shlex.split(dependency_text.split(':', 1)[1]):
                dependency = Path(name)
                if not dependency.is_absolute():
                    dependency = Path(record['directory']) / dependency
                inputs[str(dependency.resolve())] = sha(dependency)
            if path in (base, stage / 'NativeUfs.cpp'):
                init = '__GLOBAL__sub_I_' + path.name
                defined = subprocess.check_output(['xcrun', 'nm', '-j', '-U', str(obj)], text=True).splitlines()
                if init not in defined:
                    raise SystemExit(f'{path.name}: expected deferred constructor {init} missing')
                late = obj.with_suffix('.late.o')
                subprocess.run([objcopy, '--remove-section=__DATA,__mod_init_func',
                                '--globalize-symbol=' + init, str(obj), str(late)], check=True)
                target.check_object(late.read_bytes(), path.name + ' deferred')
                require_no_eager_constructors(late.read_bytes(), path.name)
                # The late constructor must be externally visible for the
                # runtime's strong reference; no eager class registration.
                external = subprocess.check_output(['xcrun', 'nm', '-g', '-j', '-U', str(late)], text=True).splitlines()
                if init not in external:
                    raise SystemExit(f'{path.name}: deferred constructor was not globalized')
                deferred[path.name] = {'symbol': init, 'object_sha256': sha(late)}
                obj = late
            objects.append(obj)
        subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
        undefined = subprocess.check_output(['xcrun', 'nm', '-u', '-j', str(archive)], text=True)
        if '_boot_progress_poll' in undefined.splitlines():
            raise SystemExit('Minimal source storage still imports the disabled boot observer')
        (output / 'undefined-symbols.log').write_text(undefined)
        report = {'framework_revision': revision, 'ufs_revision': actual, 'upstream_revision': actual,
                  'upstream_source_sha256': hashlib.sha256(subprocess.check_output(
                      ['git', '-C', str(source), 'show', f'{actual}:NativeUfs.cpp'])).hexdigest(),
                  'storage_revision': STORAGE_PIN,
                  'bridge_reference_revision': BRIDGE_REFERENCE, 'target': target.TRIPLE,
                  'official_recipe_sha256': sha(framework / 'scripts/build-sheng-source-storage.py'),
                  'device_recipe_sha256': sha(Path(__file__)),
                  'archive': str(archive), 'archive_sha256': sha(archive), 'archive_bytes': archive.stat().st_size,
                  'objects_sha256': {obj.name: sha(obj) for obj in objects}, 'deferred_metaclasses': deferred,
                  'included_inputs_sha256': inputs, 'headers': header_report, 'host_tests': host_tests,
                  'interface_revisions': interface_revisions,
                  'compile_records_sha256': {name: sha(path) for name, path in recipe_paths.items()},
                  'xnu_compile_record_sha256': sha(recipe_paths['cpp']),
                  'board_patches': [{'path': str(path.relative_to(ROOT)), 'sha256': sha(path)} for path in patches],
                  'policies': policies, 'linked_into_kernel': False, 'startup_hook_selected': False,
                  'root_policy_handoff_exercised': False, 'runtime_qualified': False}
        report_path.write_text(json.dumps(report, indent=2) + '\n')
    print(f'TB710FU standalone source-storage compile passed: {archive}')


if __name__ == '__main__':
    main()
