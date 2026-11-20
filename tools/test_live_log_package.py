"""Adversarial packaging fixtures. These synthetic Mach-Os are never delivered."""
from copy import deepcopy
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
from test_package_source_storage import synthetic_archive, synthetic_fileset, synthetic_macho
import package_source_storage as pack
import verify_live_log_package as live


def call_evidence(*callers):
    return {'symbol': {'kind': 1, 'section': 0, 'desc': 0, 'value': '0x0'},
            'branches': [{'section': '__text', 'section_offset': hex(index * 4),
                          'instruction': '0x94000000', 'call': 'bl', 'caller': caller}
                         for index, caller in enumerate(callers)]}


def fixture():
    members = {name: synthetic_macho(live.OBJECT_SYMBOLS[name]) for name in live.ORDERED_OBJECTS}
    archive = synthetic_archive(members)
    archive_path = '/ci/tree/build/tb710fu-live-log/libTb710fuLiveLog.a'
    report = {
        'device_commit': 'a' * 40, 'display_build_label': 'src-aaaaaaaa-ramoops',
        'archive': archive_path, 'archive_sha256': live.sha(archive),
        'archive_members': list(live.ORDERED_OBJECTS),
        'framework_revision': live.LOCK['unixnu']['commit'], 'xnu_revision': live.LOCK['xnu_commit'],
        'runtime_qualified': False, 'qr_hardware_display_verified': False,
        'linked_into_kernel': False, 'live_log_sources_compiled': True,
        'objects_sha256': {name: live.sha(data) for name, data in members.items()},
        'sources': {'kits/tb710fu/Tb710fuLiveLog/' + p.name: live.sha(p.read_bytes())
                    for p in (live.ROOT / 'package/Tb710fuLiveLog').iterdir() if p.suffix in ('.c', '.h')},
        'recipe_sha256': live.sha((live.ROOT / 'scripts/build-tb710fu-live-log.py').read_bytes()),
        'font': {'sha256': live.sha((live.ROOT / 'diagnostics/early-report/Font.h').read_bytes())},
        'dual_source_log_contract': deepcopy(live.DUAL_SOURCE_LOG_CONTRACT),
    }
    manifest_bytes = (live.ROOT / 'diagnostics/early-report/qr-sources.json').read_bytes()
    manifest = json.loads(manifest_bytes)
    report['qr_upstream'] = {'repository': manifest['repository'], 'commit': manifest['commit'],
        'manifest_sha256': live.sha(manifest_bytes), 'unchanged_source_copies': True,
        'files': {item['path']: {'sha256': item['sha256']} for item in manifest['files']}}
    qr_flags, qr_flags_reason = live.qr_build_contract()
    report['qr_upstream']['source_specific_flags'] = qr_flags
    report['qr_upstream']['source_specific_flags_reason'] = qr_flags_reason
    for key in ('compile_commands_sha256', 'dependency_records_sha256', 'included_inputs_records_sha256'):
        report[key] = {name[:-2] + '.c': 'c' * 64 for name in live.OBJECTS}
    link = '-Wl,-force_load,' + archive_path + '\n'
    adapter_callers = {
        '_tb710fu_live_capture_putc': '_tb710fu_live_log_putc',
        '_tb710fu_live_log_snapshot_single_file': '_checkpoint_publish',
        '_tb710fu_live_log_snapshot_file': '_checkpoint_publish',
    }
    integration = {'schema_version': 1, 'profile': live.LOCK['os_profile'], 'device_commit': 'a' * 40,
        'display_build_label': 'src-aaaaaaaa-ramoops',
        'framework_revision': live.LOCK['unixnu']['commit'], 'xnu_revision': live.LOCK['xnu_commit'],
        'native_kernel_sha256': 'b' * 64, 'archive_sha256': live.sha(archive),
        'archive_force_load_count': 1, 'linked_into_kernel': True,
        'live_log_sources_compiled': True, 'synchronous_checkpoint_sites_verified': True,
        'runtime_boot_argument': 'tb710fu-live-log=1 tb710fu-live-checkpoints=1',
        'runtime_qualified': False, 'qr_hardware_display_verified': False, 'hardware_boot_verified': False,
        'prelink_rnx_pte_substeps': {
            'compiled_source': '/ci/tree/.cache/xnu-work/osfmk/arm64/arm_vm_init.c',
            'compiled_source_sha256': '6' * 64, 'compiled_checkpoint_label_in_object': True,
            'checkpoint_position': 'after-active-update-or-defer-before-next-PTE'},
        'linkarchives': link, 'linkarchives_sha256': live.sha(link.encode()),
        'dual_source_log_contract': deepcopy(live.DUAL_SOURCE_LOG_CONTRACT),
        'adapter_source_calls': {name: call_evidence(adapter_callers.get(name, '_tb710fu_live_log_poll'))
            for name in live.ADAPTER_SOURCE_IMPORTS},
        'checkpoint_display_calls': {
            name: call_evidence(
                '_checkpoint_status' if name == '_tb_live_display_checkpoint_status' else
                '_checkpoint_qr_advance_locked' if name == '_tb_live_display_checkpoint_qr' else
                '_checkpoint_publish')
            for name in live.CHECKPOINT_DISPLAY_IMPORTS},
        'checkpoint_worker_calls': {
            name: call_evidence('_live_start_worker' if name == '_kernel_thread_start'
                                else '_live_worker')
            for name in live.CHECKPOINT_WORKER_IMPORTS},
        'hook_objects': {
            'osfmk/DEVELOPMENT/serial_console.o': {
                'object_sha256': 'd' * 64, 'compile_record_sha256': 'e' * 64,
                'disabled_capture_live_gate': True,
                'strong_hook_imports': {
                    '_tb710fu_live_log_putc': call_evidence('__cnputc', '_tb710fu_live_capture_disabled'),
                    '_tb710fu_live_log_active': call_evidence('_tb710fu_live_capture_disabled'),
                    '_tb710fu_ramoops_putc': call_evidence('__cnputc', '_tb710fu_live_capture_disabled')}},
            'pexpert/DEVELOPMENT/pe_kprintf.o': {
                'object_sha256': 'f' * 64, 'compile_record_sha256': '1' * 64,
                'strong_hook_imports': {
                    '_tb710fu_live_log_putc': call_evidence('_serial_putc', '_serial_putc_crlf'),
                    '_tb710fu_ramoops_putc': call_evidence('_serial_putc', '_serial_putc_crlf')}}}}
    integration['adapter_snapshot_calls'] = deepcopy(
        integration['adapter_source_calls']['_tb710fu_ramoops_snapshot_copy']['branches'])
    for relative, names in live.CAPTURE_SOURCE_FUNCTIONS.items():
        integration['hook_objects'][relative]['capture_source'] = {
            'compiled_source': '/ci/tree/.cache/xnu-work/' + relative.replace('DEVELOPMENT/', '').replace('.o', '.c'),
            'compiled_source_sha256': '2' * 64, 'provider': '_tb710fu_live_log_putc',
            'function_body_sha256': {name: '3' * 64 for name in names}}
    return {'report': report, 'integration': integration, 'archive': archive,
        'kernel': synthetic_fileset(live.SYMBOLS),
        'official': {'inputs': {'native_sha256': 'b' * 64}},
        'build': {'live_log_linked': True}, 'storage': {'device_commit': 'a' * 40}}


def check(value):
    report_bytes = json.dumps(value['report']).encode()
    value['integration']['archive_compile_report_sha256'] = live.sha(report_bytes)
    integration_bytes = json.dumps(value['integration']).encode()
    value['build']['live_log_integration_report_sha256'] = live.sha(integration_bytes)
    return live.verify(integration_bytes, report_bytes, value['archive'], value['kernel'],
                       value['official'], value['build'], value['storage'])


class LiveLogPackaging(unittest.TestCase):
    def test_complete_fixture(self):
        self.assertEqual(check(fixture())['strong_definitions_verified'], len(live.SYMBOLS))

    def test_dual_live_sources_and_early_backup_contract_are_required(self):
        for report_name in ('report', 'integration'):
            for mutation in ('missing', 'swapped_sources', 'shared_capture', 'lost_early_backup',
                             'oversize_tail', 'oversize_body', 'old_fixed_left', 'multiple_frames',
                             'one_round', 'long_single_frame', 'no_qr_carousel', 'short_qr_dwell',
                             'single_frame_checkpoint', 'no_early_backup', 'no_receiver_reset',
                             'oversize_multiframe_file', 'single_frame_file_api', 'wrong_raw_layout',
                             'missing_pte_substeps', 'wrong_pte_position', 'missing_raw_gap_metadata',
                             'missing_checkpoint_qr_driver', 'wrong_compact_dwell',
                             'missing_pte_source_evidence', 'wrong_pte_source_position'):
                with self.subTest(report=report_name, mutation=mutation):
                    value = fixture()
                    contract = value[report_name]['dual_source_log_contract']
                    if mutation == 'missing': value[report_name].pop('dual_source_log_contract')
                    elif mutation == 'swapped_sources':
                        contract['left']['source'], contract['right']['source'] = 'ramoops', 'xnu-console'
                    elif mutation == 'shared_capture': contract['left']['capture_ring_bytes'] = 0
                    elif mutation == 'lost_early_backup': contract['left']['early_backup_separate_metadata_and_raw_section'] = False
                    elif mutation == 'oversize_tail': contract['left']['current_tail_bytes_max'] = 4096
                    elif mutation == 'oversize_body': contract['left']['export_body_bytes_max'] = 8192
                    elif mutation == 'old_fixed_left': contract['left']['source'] = 'fixed-ramoops'
                    elif mutation == 'multiple_frames': contract['frames_per_poll_max'] = 2
                    elif mutation == 'long_single_frame':
                        contract['single_frame_rounds_before_replacement'] = 16
                    elif mutation == 'no_qr_carousel': contract['checkpoint_mode']['background_worker_role'] = 'disabled'
                    elif mutation == 'short_qr_dwell': contract['checkpoint_mode']['qr_frame_interval_ms'] = 1
                    elif mutation == 'single_frame_checkpoint': contract['checkpoint_mode']['minimum_complete_rounds_before_replacement'] = 1
                    elif mutation == 'no_early_backup': contract['checkpoint_mode']['early_backup'] = 'omitted'
                    elif mutation == 'no_receiver_reset': contract['checkpoint_mode']['qr_session_reset_required_on_generation_change'] = False
                    elif mutation == 'oversize_multiframe_file': contract['checkpoint_mode']['multi_frame_file_body_max_bytes'] = 8192
                    elif mutation == 'single_frame_file_api': contract['checkpoint_mode']['sender_snapshot_api'] = 'tb710fu_live_log_snapshot_single_file'
                    elif mutation == 'wrong_raw_layout': contract['checkpoint_mode']['raw_layout'] = 'current_only_single_frame'
                    elif mutation == 'missing_pte_substeps': contract['checkpoint_mode'].pop('pte_substep_interval')
                    elif mutation == 'wrong_pte_position': contract['checkpoint_mode']['pte_checkpoint_position'] = 'before-update'
                    elif mutation == 'missing_raw_gap_metadata': contract['checkpoint_mode']['multiframe_raw_gap_range_metadata'] = False
                    elif mutation == 'missing_checkpoint_qr_driver': contract['checkpoint_mode']['qr_frame_drivers'] = ['worker']
                    elif mutation == 'wrong_compact_dwell': contract['checkpoint_mode']['single_frame_successful_draws_before_replacement'] = 1
                    elif mutation == 'missing_pte_source_evidence': value['integration'].pop('prelink_rnx_pte_substeps')
                    elif mutation == 'wrong_pte_source_position': value['integration']['prelink_rnx_pte_substeps']['checkpoint_position'] = 'before-update'
                    else: contract['complete_rounds_before_replacement_min'] = 1
                    with self.assertRaisesRegex(ValueError, 'dual-source capture/export contract'):
                        check(value)

    def test_dual_source_capture_calls_must_be_real_strong_branches(self):
        for name in live.ADAPTER_SOURCE_IMPORTS:
            for mutation in ('missing', 'weak', 'no_branch', 'not_branch', 'wrong_call', 'unaligned', 'negative', 'data'):
                with self.subTest(symbol=name, mutation=mutation):
                    value = fixture()
                    calls = value['integration']['adapter_source_calls']
                    if mutation == 'missing': calls.pop(name)
                    elif mutation == 'weak': calls[name]['symbol']['desc'] = 0x40
                    elif mutation == 'no_branch': calls[name]['branches'] = []
                    else:
                        branch = calls[name]['branches'][0]
                        if mutation == 'not_branch': branch['instruction'] = '0xd65f03c0'
                        elif mutation == 'wrong_call': branch['call'] = 'b'
                        elif mutation == 'unaligned': branch['section_offset'] = '0x1'
                        elif mutation == 'negative': branch['section_offset'] = '-0x4'
                        else: branch['section'] = '__data'
                    with self.assertRaises(ValueError): check(value)

    def test_checkpoint_display_calls_must_use_live_scroll_qr_and_status_paths(self):
        for symbol in live.CHECKPOINT_DISPLAY_IMPORTS:
            for mutation in ('missing', 'no_branch', 'wrong_caller'):
                with self.subTest(symbol=symbol, mutation=mutation):
                    value = fixture()
                    calls = value['integration']['checkpoint_display_calls']
                    if mutation == 'missing': calls.pop(symbol)
                    elif mutation == 'no_branch': calls[symbol]['branches'] = []
                    else: calls[symbol]['branches'][0]['caller'] = '_other'
                    with self.assertRaises(ValueError): check(value)

    def test_checkpoint_uses_both_snapshot_providers(self):
        for symbol in ('_tb710fu_live_log_snapshot_single_file', '_tb710fu_live_log_snapshot_file'):
            with self.subTest(symbol=symbol):
                value = fixture()
                value['integration']['adapter_source_calls'][symbol]['branches'][0]['caller'] = '_snapshot_xnu'
                with self.assertRaisesRegex(ValueError, 'Checkpoint publisher lacks'):
                    check(value)

    def test_multiframe_qr_carousel_worker_must_be_real_and_timer_driven(self):
        for symbol in live.CHECKPOINT_WORKER_IMPORTS:
            with self.subTest(symbol=symbol):
                value = fixture()
                value['integration']['checkpoint_worker_calls'][symbol]['branches'][0]['caller'] = '_other'
                with self.assertRaises(ValueError): check(value)

    def test_each_console_path_and_both_source_providers_are_required(self):
        for mutation in ('normal', 'disabled', 'gate', 'pe_normal', 'pe_crlf', 'capture_provider',
                         'ramoops_alias', 'serial_ramoops', 'pe_ramoops', 'hook_identity'):
            with self.subTest(mutation=mutation):
                value = fixture()
                integration = value['integration']
                serial = integration['hook_objects']['osfmk/DEVELOPMENT/serial_console.o']
                pe = integration['hook_objects']['pexpert/DEVELOPMENT/pe_kprintf.o']
                calls = serial['strong_hook_imports']['_tb710fu_live_log_putc']['branches']
                if mutation == 'normal': serial['capture_source']['function_body_sha256'].pop('__cnputc')
                elif mutation == 'disabled': serial['capture_source']['function_body_sha256'].pop('_tb710fu_live_capture_disabled')
                elif mutation == 'gate': serial['strong_hook_imports']['_tb710fu_live_log_active']['branches'][0]['caller'] = '__cnputc'
                elif mutation == 'pe_normal': pe['strong_hook_imports']['_tb710fu_live_log_putc']['branches'].pop(0)
                elif mutation == 'pe_crlf': pe['strong_hook_imports']['_tb710fu_live_log_putc']['branches'].pop(1)
                elif mutation == 'capture_provider': integration['adapter_source_calls']['_tb710fu_live_capture_putc']['branches'][0]['caller'] = '_other_provider'
                elif mutation == 'ramoops_alias': integration['adapter_snapshot_calls'] = []
                elif mutation == 'serial_ramoops': serial['strong_hook_imports'].pop('_tb710fu_ramoops_putc')
                elif mutation == 'pe_ramoops': pe['strong_hook_imports'].pop('_tb710fu_ramoops_putc')
                else: serial['object_sha256'] = 'not a SHA-256'
                with self.assertRaises(ValueError): check(value)

    def test_normal_and_disabled_calls_may_both_be_inlined(self):
        value = fixture()
        imports = value['integration']['hook_objects']['osfmk/DEVELOPMENT/serial_console.o']['strong_hook_imports']
        for call in imports['_tb710fu_live_log_putc']['branches']:
            call['caller'] = '_console_write'
        self.assertTrue(check(value)['independent_xnu_capture_and_actual_ramoops_calls_verified'])

    def test_all_new_capture_exports_must_be_in_their_actual_objects_and_native_kernel(self):
        for member, names in (('LiveProtocol.o', live.OBJECT_SYMBOLS['LiveProtocol.o']),
                              ('LiveXnu.o', ('_tb710fu_live_log_putc',))):
            for name in names:
                for place in ('object', 'native'):
                    with self.subTest(member=member, symbol=name, place=place):
                        value = fixture()
                        if place == 'object':
                            members = {item: synthetic_macho(tuple(symbol for symbol in live.OBJECT_SYMBOLS[item]
                                                                  if item != member or symbol != name))
                                       for item in live.ORDERED_OBJECTS}
                            value['archive'] = synthetic_archive(members)
                            value['report']['objects_sha256'] = {item: live.sha(data) for item, data in members.items()}
                            value['report']['archive_sha256'] = value['integration']['archive_sha256'] = live.sha(value['archive'])
                        else: value['kernel'] = synthetic_fileset(tuple(symbol for symbol in live.SYMBOLS if symbol != name))
                        with self.assertRaisesRegex(ValueError, 'strong live-log code definition'):
                            check(value)

    def test_reject_false_provenance(self):
        for mutation in ('source', 'recipe', 'qr', 'native', 'force', 'qualify', 'dependency', 'archive', 'missing_code', 'weak_code'):
            with self.subTest(mutation=mutation):
                value = deepcopy(fixture())
                if mutation == 'source': value['report']['sources'].pop(next(iter(value['report']['sources'])))
                elif mutation == 'recipe': value['report']['recipe_sha256'] = '0' * 64
                elif mutation == 'qr': value['report']['qr_upstream']['commit'] = '0' * 40
                elif mutation == 'native': value['integration']['native_kernel_sha256'] = '0' * 64
                elif mutation == 'force': value['integration']['archive_force_load_count'] = 0
                elif mutation == 'qualify': value['report']['runtime_qualified'] = True
                elif mutation == 'dependency': value['report']['dependency_records_sha256'].pop('LiveXnu.c')
                elif mutation == 'archive': value['archive'] = value['archive'][:-4]
                elif mutation == 'missing_code': value['kernel'] = synthetic_fileset(live.SYMBOLS[:-1])
                elif mutation == 'weak_code': value['kernel'] = synthetic_fileset(live.SYMBOLS, weak=live.SYMBOLS[0])
                with self.assertRaises(ValueError): check(value)

    def test_verbose_args_are_bounded(self):
        base = '-v serial=3 native-source-storage=1 native-ufs-root=1 tb710fu-cdc=1'
        suffix = ' tb710fu-live-log=1 tb710fu-live-checkpoints=1 debug=0x8 -enable_kprintf_spam startup_debug=1'
        self.assertEqual(pack.validate_args(base + suffix)['debug'], '0x8')
        for bad in (suffix.replace('0x8', '0x9'), suffix.replace('startup_debug=1', 'startup_debug=2'),
                    suffix.replace('tb710fu-live-log=1 ', ''),
                    suffix.replace('tb710fu-live-checkpoints=1 ', ''),
                    suffix.replace('-enable_kprintf_spam ', '')):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError): pack.validate_args(base + bad)

    def test_archive_order_duplicates_metadata_and_actual_code(self):
        for mutation in ('duplicate', 'reorder', 'member_list', 'wrong_arch', 'truncated_object',
                         'missing_object_code', 'weak_object_code', 'unbacked_native_code',
                         'data_native_code', 'wrong_fileset_arch', 'wrong_native_arch'):
            with self.subTest(mutation=mutation):
                value = fixture()
                members = {name: synthetic_macho(live.OBJECT_SYMBOLS[name]) for name in live.ORDERED_OBJECTS}
                if mutation == 'duplicate':
                    value['archive'] += synthetic_archive({'LiveXnu.o': members['LiveXnu.o']})[8:]
                elif mutation == 'reorder':
                    value['archive'] = synthetic_archive(dict(reversed(list(members.items()))))
                elif mutation == 'member_list':
                    value['report']['archive_members'] = list(reversed(live.ORDERED_OBJECTS))
                elif mutation in ('wrong_arch', 'truncated_object', 'missing_object_code', 'weak_object_code'):
                    if mutation == 'wrong_arch':
                        blob = bytearray(members['LiveXnu.o'])
                        struct.pack_into('<I', blob, 8, 0)
                        members['LiveXnu.o'] = bytes(blob)
                    elif mutation == 'truncated_object':
                        members['LiveXnu.o'] = b'bad object'
                    elif mutation == 'missing_object_code':
                        members['LiveXnu.o'] = synthetic_macho(live.OBJECT_SYMBOLS['LiveXnu.o'][:-1])
                    else:
                        members['LiveXnu.o'] = synthetic_macho(live.OBJECT_SYMBOLS['LiveXnu.o'],
                                                               weak=live.OBJECT_SYMBOLS['LiveXnu.o'][0])
                    value['archive'] = synthetic_archive(members)
                    value['report']['objects_sha256'] = {name: live.sha(data) for name, data in members.items()}
                else:
                    blob = bytearray(value['kernel'])
                    audit = live.load_audit()
                    fileset = audit.MachO(blob)
                    native_at = fileset.entries['com.apple.kernel'][1]
                    native = audit.MachO(blob, native_at)
                    if mutation == 'wrong_fileset_arch':
                        struct.pack_into('<I', blob, 8, 0)
                    elif mutation == 'wrong_native_arch':
                        struct.pack_into('<I', blob, native_at + 8, 0)
                    elif mutation == 'data_native_code':
                        segment = next(pos for cmd, pos, _ in native.commands if cmd == 0x19)
                        # The actual section command is executable; stripping its
                        # instruction flags must invalidate the claimed provider.
                        struct.pack_into('<I', blob, segment + 72 + 64, 0)
                    else:
                        table = next(pos for cmd, pos, _ in native.commands if cmd == 2)
                        symoff = struct.unpack_from('<I', blob, table + 8)[0]
                        struct.pack_into('<Q', blob, symoff + 8, (1 << 64) - 1)
                    value['kernel'] = bytes(blob)
                value['report']['archive_sha256'] = value['integration']['archive_sha256'] = live.sha(value['archive'])
                with self.assertRaises(ValueError):
                    check(value)

    def test_qr_compile_contract_cannot_drift(self):
        for mutation in ('missing_flags', 'changed_flags', 'flags_order', 'reason'):
            with self.subTest(mutation=mutation):
                value = fixture()
                qr = value['report']['qr_upstream']
                if mutation == 'missing_flags': qr.pop('source_specific_flags')
                elif mutation == 'changed_flags': qr['source_specific_flags'].append('-fno-wrapv')
                elif mutation == 'flags_order': qr['source_specific_flags'].reverse()
                else: qr['source_specific_flags_reason'] = 'unrecorded exception'
                with self.assertRaisesRegex(ValueError, 'QR/font/recipe provenance'):
                    check(value)

    def test_live_manifest_requires_all_twenty_inputs_and_excludes_old_reporter(self):
        with tempfile.TemporaryDirectory(prefix='tb710fu-live-manifest-test-') as temporary:
            root = Path(temporary)
            all_names = pack.INPUTS | pack.CDC_INPUTS | pack.LIVE_INPUTS | pack.STANDARD_INPUTS
            files = {}
            for name in all_names | pack.DIAGNOSTIC_INPUTS:
                path = root / name
                path.write_bytes(name.encode())
                files[name] = {'path': path.name, 'sha256': pack.sha(path.read_bytes())}
            manifest = {'schema_version': 1, 'profile': pack.LOCK['os_profile'], 'framework': str(root),
                        'boot_args': 'native-source-storage=1 native-ufs-root=1 tb710fu-cdc=1 '
                                     'tb710fu-live-log=1 tb710fu-live-checkpoints=1 '
                                     'debug=0x8 -enable_kprintf_spam startup_debug=1',
                        'files': {name: files[name] for name in all_names}}
            path = root / 'inputs.json'

            def checked():
                path.write_bytes(json.dumps(manifest).encode())
                return pack.checked_inputs(path, pack.sha(path.read_bytes()))

            with patch.object(pack, 'BASELINE', files['source_vendor']['sha256']), \
                    patch.object(pack, 'BASE_VBMETA', files['source_vbmeta']['sha256']):
                self.assertEqual(len(checked()[1]), 20)
                for name in sorted(all_names):
                    with self.subTest(missing=name):
                        saved = manifest['files'].pop(name)
                        with self.assertRaises(ValueError): checked()
                        manifest['files'][name] = saved
                for name in sorted(pack.DIAGNOSTIC_INPUTS):
                    with self.subTest(old_reporter=name):
                        manifest['files'][name] = files[name]
                        with self.assertRaisesRegex(ValueError, 'only the standard-band transform'): checked()
                        manifest['files'].pop(name)
                for name in sorted(pack.LIVE_INPUTS | pack.STANDARD_INPUTS):
                    with self.subTest(lone_native_input=name):
                        saved_files = manifest['files']
                        saved_args = manifest['boot_args']
                        manifest['files'] = {key: files[key] for key in pack.INPUTS | pack.CDC_INPUTS | {name}}
                        manifest['boot_args'] = 'native-source-storage=1 native-ufs-root=1 tb710fu-cdc=1'
                        with self.assertRaisesRegex(ValueError, 'Native live log requires'): checked()
                        manifest['files'], manifest['boot_args'] = saved_files, saved_args
                manifest['boot_args'] = manifest['boot_args'].replace('tb710fu-cdc=1 ', '')
                with self.assertRaisesRegex(ValueError, 'complete CDC evidence'): checked()


if __name__ == '__main__':
    unittest.main()
