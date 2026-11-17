"""Bind native live logging evidence to the exact archive and prelinked code."""
import hashlib
import importlib.util
import json
import re
import struct
from pathlib import Path
from verify_storage_integration import load_audit, hook_symbol, force_load_count

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / 'upstream.lock.json').read_bytes())
OBJECTS = {'LiveProtocol.o', 'LiveDisplay.o', 'LiveQr.o', 'LiveXnu.o',
           'QrEncoderLib.o', 'QrEncoderTables.o'}
ORDERED_OBJECTS = ('LiveProtocol.o', 'LiveDisplay.o', 'LiveQr.o', 'LiveXnu.o',
                   'QrEncoderLib.o', 'QrEncoderTables.o')
SYMBOLS = ('_tb710fu_live_log_init', '_tb710fu_live_log_poll',
           '_tb710fu_live_log_refresh', '_tb710fu_live_log_active',
           '_tb710fu_live_log_freeze', '_tb710fu_ramoops_snapshot_copy',
           '_tb710fu_live_log_snapshot', '_tb710fu_live_log_next_frame',
           '_tb_live_display_qr_slot', '_tb_live_qr_encode',
           '_tb_live_qr_QrEncodeData')
OBJECT_SYMBOLS = {
    'LiveProtocol.o': ('_tb710fu_live_log_snapshot', '_tb710fu_live_log_next_frame'),
    'LiveDisplay.o': ('_tb_live_display_qr_slot', '_tb_live_display_bands'),
    'LiveQr.o': ('_tb_live_qr_encode',),
    'LiveXnu.o': ('_tb710fu_live_log_init', '_tb710fu_live_log_poll', '_tb710fu_live_log_freeze'),
    'QrEncoderLib.o': ('_tb_live_qr_QrEncodeData',),
    'QrEncoderTables.o': ('_tb_live_qr_InitializeLogTables',),
}


def require(value, message):
    if not value:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def qr_build_contract():
    path = ROOT / 'scripts/build-tb710fu-live-log.py'
    spec = importlib.util.spec_from_file_location('live_package_build_contract', path)
    require(spec is not None and spec.loader is not None, 'Live-log build contract is unavailable')
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    return list(builder.PINNED_QR_FLAGS), builder.PINNED_QR_FLAGS_REASON


def code_definition(image, name):
    symbols = hook_symbol(image, name)
    require(len(symbols) == 1 and symbols[0]['kind'] & 14 == 14 and
            symbols[0]['kind'] & 1 and not symbols[0]['desc'] & 0x80 and
            0 < symbols[0]['section'] <= len(image.sections),
            'Missing real strong live-log code definition: ' + name)
    section = image.sections[symbols[0]['section'] - 1]
    address = int(symbols[0]['value'], 16)
    require(section['name'] == '__text' and section['flags'] & 0x80000000 and
            section['va'] <= address and address + 4 <= section['va'] + section['size'] and
            image.offset(address, 4) + 4 <= len(image.data),
            'Live-log definition is not executable code: ' + name)


def _verify(integration_bytes, report_bytes, archive, kernel, official, build, storage):
    integration = json.loads(integration_bytes)
    report = json.loads(report_bytes)
    require(build.get('live_log_linked') is True and
            build.get('live_log_integration_report_sha256') == sha(integration_bytes),
            'Build does not bind the exact live-log integration report')
    require(integration.get('schema_version') == 1 and
            integration.get('profile') == LOCK['os_profile'] and
            integration.get('device_commit') == storage.get('device_commit') and
            integration.get('native_kernel_sha256') == official['inputs']['native_sha256'],
            'Live-log native/profile/device identity differs')
    label = 'src-' + integration['device_commit'][:8] + '-ramoops'
    require(report.get('device_commit') == integration['device_commit'] and
            report.get('display_build_label') == integration.get('display_build_label') == label,
            'Live-log display label is not bound to this device revision')
    for entry in (integration, report):
        require(entry.get('framework_revision') == LOCK['unixnu']['commit'] and
                entry.get('xnu_revision') == LOCK['xnu_commit'] and
                entry.get('runtime_qualified') is False and
                entry.get('qr_hardware_display_verified') is False and
                entry.get('live_log_sources_compiled') is True,
                'Live-log source identity or qualification differs')
    require(integration.get('linked_into_kernel') is True and
            integration.get('hardware_boot_verified') is False and
            report.get('linked_into_kernel') is False and
            integration.get('archive_compile_report_sha256') == sha(report_bytes) and
            integration.get('archive_sha256') == report.get('archive_sha256') == sha(archive),
            'Live-log archive/compile/integration evidence differs')
    require(integration.get('archive_force_load_count') == 1 and
            integration.get('runtime_boot_argument') == 'tb710fu-live-log=1' and
            sha(integration.get('linkarchives', '').encode()) == integration.get('linkarchives_sha256') and
            force_load_count(integration['linkarchives'], report['archive']) == 1,
            'Live-log archive was not force-loaded exactly once')
    audit = load_audit()
    member_list = [(name, data) for name, data in audit.ar_members(archive)
                   if not name.startswith('__.SYMDEF')]
    require([name for name, _ in member_list] == list(ORDERED_OBJECTS) == report.get('archive_members'),
            'Live-log archive has reordered, duplicate or unexpected members')
    members = dict(member_list)
    require(set(members) == OBJECTS and
            {name: sha(data) for name, data in members.items()} == report.get('objects_sha256'),
            'Live-log archive must contain all six actual source objects')
    for name, data in members.items():
        image = audit.MachO(data)
        require(image.kind == 1 and image.subtype & 0xffffff == 2,
                'Live-log member is not an actual arm64e object: ' + name)
        for symbol in OBJECT_SYMBOLS[name]:
            code_definition(image, symbol)
    package = ROOT / 'package/Tb710fuLiveLog'
    expected_sources = {'kits/tb710fu/Tb710fuLiveLog/' + path.name: sha(path.read_bytes())
                        for path in package.iterdir() if path.suffix in ('.c', '.h')}
    require(report.get('sources') == expected_sources,
            'Live-log compiled source/header hashes differ from reviewed device sources')
    manifest = json.loads((ROOT / 'diagnostics/early-report/qr-sources.json').read_bytes())
    qr = report.get('qr_upstream', {})
    qr_flags, qr_flags_reason = qr_build_contract()
    require(qr.get('repository') == manifest['repository'] and qr.get('commit') == manifest['commit'] and
            qr.get('manifest_sha256') == sha((ROOT / 'diagnostics/early-report/qr-sources.json').read_bytes()) and
            qr.get('unchanged_source_copies') is True and
            qr.get('source_specific_flags') == qr_flags and
            qr.get('source_specific_flags_reason') == qr_flags_reason and
            {name: item['sha256'] for name, item in qr.get('files', {}).items()} ==
            {item['path']: item['sha256'] for item in manifest['files']} and
            report.get('font', {}).get('sha256') == sha((ROOT / 'diagnostics/early-report/Font.h').read_bytes()) and
            report.get('recipe_sha256') == sha((ROOT / 'scripts/build-tb710fu-live-log.py').read_bytes()),
            'Live-log QR/font/recipe provenance differs')
    source_names = {name[:-2] + '.c' for name in OBJECTS}
    for key in ('compile_commands_sha256', 'dependency_records_sha256', 'included_inputs_records_sha256'):
        values = report.get(key, {})
        require(set(values) == source_names and all(re.fullmatch('[0-9a-f]{64}', value)
                for value in values.values()), 'Incomplete live-log target provenance: ' + key)
    fileset = audit.MachO(kernel)
    require(fileset.kind == 12 and fileset.subtype & 0xffffff == 2 and
            'com.apple.kernel' in fileset.entries,
            'Live-log needs the actual prelinked native kernel')
    native = audit.MachO(kernel, fileset.entries['com.apple.kernel'][1])
    require(native.kind == 2 and native.subtype & 0xffffff == 2,
            'Live-log fileset native entry is not an arm64e executable')
    for name in SYMBOLS:
        code_definition(native, name)
    return {'archive_sha256': sha(archive), 'archive_objects_verified': len(members),
            'integration_report_sha256': sha(integration_bytes),
            'compile_report_sha256': sha(report_bytes),
            'strong_definitions_verified': len(SYMBOLS),
            'runtime_boot_argument': 'tb710fu-live-log=1',
            'fixed_snapshot_bytes_max': 512, 'live_snapshot_bytes_max': 4096,
            'runtime_qualified': False, 'qr_hardware_display_verified': False}


def verify(integration_bytes, report_bytes, archive, kernel, official, build, storage):
    try:
        return _verify(integration_bytes, report_bytes, archive, kernel, official, build, storage)
    except (AssertionError, KeyError, StopIteration, TypeError, IndexError, struct.error,
            UnicodeDecodeError, OverflowError) as error:
        raise ValueError('Malformed/incomplete live-log package evidence or Mach-O') from error
