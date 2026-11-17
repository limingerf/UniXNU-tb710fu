"""Verify the real source-storage archive, strong BSD hook and final native link."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import struct
import subprocess


ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / 'upstream.lock.json').read_text())


def require(value, message):
    if not value:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_audit():
    path = ROOT / 'scripts/audit-tb710fu-storage-abi.py'
    spec = importlib.util.spec_from_file_location('storage_link_macho', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def hook_symbol(macho, symbol='_native_source_storage_start'):
    table = next(pos for cmd, pos, _ in macho.commands if cmd == 2)
    symoff, count, stroff, _ = struct.unpack_from('<4I', macho.data, table + 8)
    matches = []
    for index in range(count):
        strx, kind, section, desc, value = struct.unpack_from('<IBBHQ', macho.data, symoff + index * 16)
        name = macho.data[stroff + strx:macho.data.index(b'\0', stroff + strx)].decode()
        if name == symbol and not kind & 0xe0:
            matches.append({'kind': kind, 'section': section, 'desc': desc, 'value': hex(value)})
    return matches


def force_load_count(text, archive):
    parts = shlex.split(text)
    count = parts.count('-Wl,-force_load,' + str(archive))
    for index, part in enumerate(parts[:-1]):
        if part in ('-force_load', '-Wl,-force_load'):
            count += parts[index + 1].removeprefix('-Wl,') == str(archive)
    return count


def hook_macro_selected(arguments, macro='XNU_TB710FU_SOURCE_STORAGE'):
    definitions = []
    for index, argument in enumerate(arguments):
        if argument in ('-D', '-U') and index + 1 < len(arguments):
            value = arguments[index + 1]
            operation = argument
        elif argument.startswith(('-D', '-U')):
            operation, value = argument[:2], argument[2:]
        else:
            continue
        if value.split('=', 1)[0] == macro:
            definitions.append((operation, value))
    return bool(definitions) and all(item == ('-D', macro + '=1') for item in definitions)


def verify(tree, config, native_path):
    require(config.get('XNU_SOURCE_STORAGE') is True, 'Source storage is not selected')
    require(config.get('XNU_NATIVE_UFS') is False, 'Automatic UFS personalities must be disabled for the source runtime')
    audit = load_audit()
    output = tree / 'build/tb710fu-source-storage'
    archive = output / 'libTb710fuSourceStorage.a'
    compile_report = output / 'build-report.json'
    report = json.loads(compile_report.read_text())
    require(report['archive_sha256'] == digest(archive), 'Source-storage archive/report mismatch')
    require(report['framework_revision'] == LOCK['unixnu']['commit'], 'Wrong framework source for storage')
    policies = output / 'root-policies.bin'
    require(digest(policies) == report['policies']['bundle_sha256'], 'RootPolicies bundle/report mismatch')
    members = dict(audit.ar_members(archive.read_bytes()))
    require(len(report['objects_sha256']) == 8, 'Unexpected minimal storage source set')
    for name, expected in report['objects_sha256'].items():
        data = members[name]
        require(hashlib.sha256(data).hexdigest() == expected, f'Archive member hash mismatch: {name}')
        obj = audit.MachO(data)
        require(obj.kind == 1 and obj.subtype & 255 == 2, f'Not an arm64e object: {name}')
        if name.endswith('.late.o'):
            require(not any(s['name'] == '__mod_init_func' for s in obj.sections), f'Eager class registration: {name}')
            constructor = '__GLOBAL__sub_I_' + name.split('.late.o')[0] + '.cpp'
            require(any(s['name'] == constructor and s['kind'] & 1 for s in obj.nlists()),
                    f'Late constructor is not external: {name}')
        require(not any(s['name'] == '_boot_progress_poll' and s['kind'] & 14 == 0 for s in obj.nlists()),
                'Minimal archive imports the disabled boot observer')
    records = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    bsd_path = records / 'bsd/DEVELOPMENT/bsd_init.o'
    command_path = bsd_path.with_suffix('.o.json')
    command = json.loads(command_path.read_text().strip().rstrip(','))
    require(hook_macro_selected(command['arguments']), 'BSD startup was not compiled with its explicit hook macro')
    imports = hook_symbol(audit.MachO(bsd_path.read_bytes()))
    require(len(imports) == 1 and imports[0]['kind'] & 14 == 0 and imports[0]['kind'] & 1 and
            not imports[0]['desc'] & 0x40, 'BSD startup must strongly import the real storage startup')
    native = native_path.read_bytes()
    image = audit.MachO(native)
    require(image.kind == 2, 'The native link verification input must be MH_EXECUTE')
    definitions = hook_symbol(image)
    require(len(definitions) == 1 and definitions[0]['kind'] & 14 == 14 and definitions[0]['kind'] & 1 and
            not definitions[0]['desc'] & 0x80,
            'Final native kernel must define the source-storage startup exactly once')
    symbols = image.nlists()
    for name in ('_native_source_root_policies_load', '_native_ufs_source_create',
                 '_native_ufs_block_authenticates', '_native_verified_block_root_authorized',
                 '_native_verified_block_manifest_sha384', '_native_verified_block_volume_device',
                 '_vr_open', '_vr_open_header', '_vr_xnu_crypto', '_verified_root_key'):
        require(any(s['name'] == name and s['kind'] & 14 == 14 and not s['kind'] & 0xe0 for s in symbols),
                f'Real storage/crypto definition missing from native kernel: {name}')
    link_path = records / 'mach.development.bcm2837.link/mach.development.bcm2837.linkarchives'
    link_text = link_path.read_text()
    count = force_load_count(link_text, archive)
    selected = str(config.get('XNU_EXTRA_LD_ARCHIVES', '')).replace('{tree}', str(tree))
    require(count == force_load_count(selected, archive) == 1, 'Select and actually link the source-storage archive exactly once')
    require('libTb710fuCdcLog.a' in link_text, 'The existing CDC console tap archive was lost')
    return {'schema_version': 1, 'profile': LOCK['os_profile'],
            'device_commit': subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip(),
            'framework_revision': LOCK['unixnu']['commit'],
            'native_kernel_sha256': hashlib.sha256(native).hexdigest(),
            'archive_sha256': digest(archive), 'policy_bundle_sha256': digest(policies),
            'archive_compile_report_sha256': digest(compile_report),
            'bsd_object_sha256': digest(bsd_path), 'bsd_compile_record_sha256': digest(command_path),
            'bsd_startup_symbol': imports[0], 'native_startup_symbol': definitions[0],
            'linkarchives_sha256': digest(link_path), 'linkarchives': link_text,
            'linked_into_kernel': True, 'source_storage_selected': True,
            'startup_hook_selected': True, 'startup_reference_strong': True, 'startup_symbol_defined': True,
            'archive_force_load_count': count, 'late_metaclasses_verified': True,
            'native_ufs_personalities_selected': False,
            'runtime_qualified': False, 'hardware_boot_verified': False,
            'root_policy_handoff_exercised': False}
