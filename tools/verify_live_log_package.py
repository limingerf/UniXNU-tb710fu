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
           '_tb710fu_live_log_checkpoint',
           '_tb710fu_live_log_refresh', '_tb710fu_live_log_active',
           '_tb710fu_live_log_freeze', '_tb710fu_ramoops_snapshot_copy',
           '_tb710fu_live_log_putc', '_tb710fu_live_capture_putc',
           '_tb710fu_live_capture_copy', '_tb710fu_live_log_set_source',
           '_tb710fu_live_log_snapshot_file',
           '_tb710fu_live_log_snapshot_single_file',
           '_tb710fu_live_log_snapshot', '_tb710fu_live_log_next_frame',
           '_tb710fu_live_log_can_snapshot', '_tb710fu_live_log_can_snapshot_single',
           '_tb_live_display_qr_slot', '_tb_live_display_checkpoint_qr',
           '_tb_live_display_scroll_log', '_tb_live_display_checkpoint_status',
           '_tb_live_qr_encode',
           '_tb_live_qr_QrEncodeData')
OBJECT_SYMBOLS = {
    'LiveProtocol.o': ('_tb710fu_live_log_snapshot', '_tb710fu_live_log_next_frame',
                       '_tb710fu_live_capture_putc', '_tb710fu_live_capture_copy',
                       '_tb710fu_live_log_set_source', '_tb710fu_live_log_snapshot_file',
                       '_tb710fu_live_log_snapshot_single_file',
                       '_tb710fu_live_log_can_snapshot_single'),
    'LiveDisplay.o': ('_tb_live_display_qr_slot', '_tb_live_display_bands',
                      '_tb_live_display_checkpoint_qr', '_tb_live_display_scroll_log',
                      '_tb_live_display_checkpoint_status'),
    'LiveQr.o': ('_tb_live_qr_encode',),
    'LiveXnu.o': ('_tb710fu_live_log_init', '_tb710fu_live_log_poll',
                  '_tb710fu_live_log_checkpoint',
                  '_tb710fu_live_log_freeze', '_tb710fu_live_log_putc'),
    'QrEncoderLib.o': ('_tb_live_qr_QrEncodeData',),
    'QrEncoderTables.o': ('_tb_live_qr_InitializeLogTables',),
}
DUAL_SOURCE_LOG_CONTRACT = {
    'schema_version': 1,
    'left': {'slot': 0, 'source': 'xnu-console', 'capture_ring_bytes': 65536,
             'current_tail_bytes_max': 3072, 'early_backup_bytes_max': 512,
             'early_backup_separate_metadata_and_raw_section': True,
             'export_body_bytes_max': 4096},
    'right': {'slot': 1, 'source': 'ramoops', 'current_tail_bytes_max': 4096,
              'provider': 'tb710fu_ramoops_snapshot_copy', 'export_body_bytes_max': 4096},
    'frames_per_poll_max': 1, 'poll_interval_ms': 250,
    'complete_rounds_before_replacement_min': 16,
    'single_frame_rounds_before_replacement': 2,
    'checkpoint_mode': {
        'argument': 'tb710fu-live-checkpoints=1',
        'requires': 'tb710fu-live-log=1',
        'provider': 'tb710fu_live_log_checkpoint',
        'worker_role': 'QR-only immutable-snapshot carousel; no dual-source polling',
        'sampled_provider': 'tb710fu_live_log_checkpoint_sampled',
        'text_refresh': 'immediate at every accepted checkpoint snapshot',
        'qr_frame_interval_ms': 250,
        'multi_frame_file_body_max_bytes': 4096,
        'early_backup': 'include frozen ready 512-byte prefix in immutable file',
        'raw_layout': 'earlybackup_then_current',
        'pte_substep_interval': 16,
        'pte_checkpoint_position': 'after-completed-update-or-defer-then-report-next-index-and-va',
        'multiframe_raw_gap_range_metadata': True,
        'minimum_complete_rounds_before_replacement': 16,
        'sender_snapshot_api': 'tb710fu_live_log_snapshot_file',
        'sender_frame_api': 'tb710fu_live_log_next_frame',
        'qr_session_reset_required_on_generation_change': True,
        'halt_limit': 'carousel freezes if kernel execution or worker scheduling stops',
        'single_frame_after_early_freeze': False,
        'display': {'checkpoint_qr_anchor': 'bottom-right',
                    'scroll_log_region': 'left-of-checkpoint-qr',
                    'scroll_source': 'latest-current-xnu-capture-tail',
                    'scroll_max_rows': 64, 'scroll_max_columns': 256,
                    'scroll_offset_model': 'absolute-range; append-unseen-suffix; mark-overwrite-gap',
                    'scroll_refresh': 'after-synchronous-checkpoint-snapshot',
                    'console_character_writer_draws': False,
                    'status_location': 'footer-left-of-checkpoint-qr'},
    },
}
ADAPTER_SOURCE_IMPORTS = (
    '_tb710fu_ramoops_snapshot_copy', '_tb710fu_live_capture_putc',
    '_tb710fu_live_capture_copy', '_tb710fu_live_log_set_source',
    '_tb710fu_live_log_snapshot_file', '_tb710fu_live_log_snapshot_single_file',
)
CHECKPOINT_DISPLAY_IMPORTS = (
    '_tb_live_display_checkpoint_qr', '_tb_live_display_scroll_log',
    '_tb_live_display_checkpoint_status',
)
CHECKPOINT_WORKER_IMPORTS = (
    '_kernel_thread_start', '_clock_interval_to_deadline',
    '_assert_wait_deadline', '_thread_block',
)
DISABLED_CAPTURE_CALLERS = {'_tb710fu_live_capture_disabled', '_console_write',
                           '_console_write_unbuffered', '__cnputs',
                           '_console_ring_try_empty', '_console_ring_try_empty_locked'}
CAPTURE_SOURCE_FUNCTIONS = {
    'osfmk/DEVELOPMENT/serial_console.o': {'__cnputc', '_tb710fu_live_capture_disabled'},
    'pexpert/DEVELOPMENT/pe_kprintf.o': {'_serial_putc', '_serial_putc_crlf'},
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


def import_branch_evidence(record, name):
    """Check the exact strong-import/direct-branch evidence produced by CI.

    The integration verifier reads these from the actual XNU and LiveXnu
    objects; the build and package bind that complete report by SHA-256.
    This schema check prevents incomplete or contradictory report readbacks.
    """
    symbol, branches = record['symbol'], record['branches']
    require(symbol['kind'] & 14 == 0 and symbol['kind'] & 1 and
            not symbol['desc'] & 0x40 and symbol['section'] == 0 and
            isinstance(branches, list) and branches,
            'Missing strong dual-source import/branch evidence: ' + name)
    for branch in branches:
        word = int(branch['instruction'], 16)
        offset = int(branch['section_offset'], 16)
        require(0 <= word <= 0xffffffff and word & 0x7c000000 == 0x14000000 and
                branch['call'] == ('bl' if word >> 31 else 'b') and
                0 <= offset and offset % 4 == 0 and branch['section'] == '__text' and
                isinstance(branch['caller'], str) and branch['caller'].startswith('_'),
                'Invalid dual-source direct-branch evidence: ' + name)
    return {branch['caller'] for branch in branches}


def dual_source_evidence(integration, report):
    for entry in (integration, report):
        require(entry.get('dual_source_log_contract') == DUAL_SOURCE_LOG_CONTRACT,
                'Live-log dual-source capture/export contract differs')
    adapter = integration.get('adapter_source_calls', {})
    require(set(adapter) == set(ADAPTER_SOURCE_IMPORTS),
            'Incomplete dual-source adapter-call evidence')
    callers = {name: import_branch_evidence(adapter[name], name) for name in ADAPTER_SOURCE_IMPORTS}
    require('_tb710fu_live_log_putc' in callers['_tb710fu_live_capture_putc'],
            'XNU console provider does not call its independent capture ring')
    require(integration.get('adapter_snapshot_calls') == adapter['_tb710fu_ramoops_snapshot_copy']['branches'],
            'Ramoops snapshot-call evidence differs from the actual provider import')
    hooks = integration.get('hook_objects', {})
    serial = hooks['osfmk/DEVELOPMENT/serial_console.o']
    pe = hooks['pexpert/DEVELOPMENT/pe_kprintf.o']
    for record in (serial, pe):
        require(re.fullmatch('[0-9a-f]{64}', record['object_sha256']) and
                re.fullmatch('[0-9a-f]{64}', record['compile_record_sha256']),
                'Incomplete dual-source console-object identity')
    for relative, names in CAPTURE_SOURCE_FUNCTIONS.items():
        capture = hooks[relative]['capture_source']
        bodies = capture['function_body_sha256']
        require(capture['provider'] == '_tb710fu_live_log_putc' and
                isinstance(capture['compiled_source'], str) and capture['compiled_source'] and
                re.fullmatch('[0-9a-f]{64}', capture['compiled_source_sha256']) and
                set(bodies) == names and all(re.fullmatch('[0-9a-f]{64}', value) for value in bodies.values()),
                'Incomplete actual normal/disabled console capture source evidence')
    serial_imports, pe_imports = serial['strong_hook_imports'], pe['strong_hook_imports']
    normal_and_disabled = import_branch_evidence(serial_imports['_tb710fu_live_log_putc'],
                                                '_tb710fu_live_log_putc')
    live_gate = import_branch_evidence(serial_imports['_tb710fu_live_log_active'],
                                      '_tb710fu_live_log_active')
    require(normal_and_disabled & (DISABLED_CAPTURE_CALLERS | {'__cnputc'}) and
            live_gate & DISABLED_CAPTURE_CALLERS and
            serial.get('disabled_capture_live_gate') is True,
            'Missing actual normal/disabled XNU console capture paths')
    require({'_serial_putc', '_serial_putc_crlf'} <=
            import_branch_evidence(pe_imports['_tb710fu_live_log_putc'], '_tb710fu_live_log_putc'),
            'Missing actual direct PE XNU console capture paths')
    # Both console outputs still feed the independent real ramoops provider.
    for imports in (serial_imports, pe_imports):
        import_branch_evidence(imports['_tb710fu_ramoops_putc'], '_tb710fu_ramoops_putc')


def checkpoint_display_evidence(integration):
    calls = integration.get('checkpoint_display_calls', {})
    require(set(calls) == set(CHECKPOINT_DISPLAY_IMPORTS),
            'Incomplete synchronous checkpoint display-call evidence')
    callers = {name: import_branch_evidence(calls[name], name) for name in CHECKPOINT_DISPLAY_IMPORTS}
    require('_checkpoint_publish' in callers['_tb_live_display_scroll_log'] and
            '_checkpoint_qr_advance_locked' in callers['_tb_live_display_checkpoint_qr'] and
            callers['_tb_live_display_checkpoint_status'] & {'_checkpoint_status', '_checkpoint_publish'},
            'Synchronous log, bottom-right QR and status do not have their real checkpoint callers')
    worker = integration.get('checkpoint_worker_calls', {})
    require(set(worker) == set(CHECKPOINT_WORKER_IMPORTS),
            'Incomplete immutable QR carousel worker-call evidence')
    worker_callers = {name: import_branch_evidence(worker[name], name)
                      for name in CHECKPOINT_WORKER_IMPORTS}
    require('_live_start_worker' in worker_callers['_kernel_thread_start'] and
            all('_live_worker' in worker_callers[name] for name in CHECKPOINT_WORKER_IMPORTS[1:]),
            'Immutable QR carousel worker lacks its real creation/timer/wait path')


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
            integration.get('runtime_boot_argument') ==
                'tb710fu-live-log=1 tb710fu-live-checkpoints=1' and
            integration.get('synchronous_checkpoint_sites_verified') is True and
            sha(integration.get('linkarchives', '').encode()) == integration.get('linkarchives_sha256') and
            force_load_count(integration['linkarchives'], report['archive']) == 1,
            'Live-log archive was not force-loaded exactly once')
    dual_source_evidence(integration, report)
    checkpoint_display_evidence(integration)
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
            'runtime_boot_argument': 'tb710fu-live-log=1 tb710fu-live-checkpoints=1',
            'dual_source_log_contract': DUAL_SOURCE_LOG_CONTRACT,
            'independent_xnu_capture_and_actual_ramoops_calls_verified': True,
            'runtime_qualified': False, 'qr_hardware_display_verified': False}


def verify(integration_bytes, report_bytes, archive, kernel, official, build, storage):
    try:
        return _verify(integration_bytes, report_bytes, archive, kernel, official, build, storage)
    except (AssertionError, AttributeError, KeyError, StopIteration, TypeError, IndexError, struct.error,
            UnicodeDecodeError, OverflowError) as error:
        raise ValueError('Malformed/incomplete live-log package evidence or Mach-O') from error
