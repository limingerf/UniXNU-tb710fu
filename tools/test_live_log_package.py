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
    integration = {'schema_version': 1, 'profile': live.LOCK['os_profile'], 'device_commit': 'a' * 40,
        'display_build_label': 'src-aaaaaaaa-ramoops',
        'framework_revision': live.LOCK['unixnu']['commit'], 'xnu_revision': live.LOCK['xnu_commit'],
        'native_kernel_sha256': 'b' * 64, 'archive_sha256': live.sha(archive),
        'archive_force_load_count': 1, 'linked_into_kernel': True,
        'live_log_sources_compiled': True, 'runtime_boot_argument': 'tb710fu-live-log=1',
        'runtime_qualified': False, 'qr_hardware_display_verified': False, 'hardware_boot_verified': False,
        'linkarchives': link, 'linkarchives_sha256': live.sha(link.encode())}
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
        suffix = ' tb710fu-live-log=1 debug=0x8 -enable_kprintf_spam startup_debug=1'
        self.assertEqual(pack.validate_args(base + suffix)['debug'], '0x8')
        for bad in (suffix.replace('0x8', '0x9'), suffix.replace('startup_debug=1', 'startup_debug=2'),
                    suffix.replace('tb710fu-live-log=1 ', ''), suffix.replace('-enable_kprintf_spam ', '')):
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
                                     'tb710fu-live-log=1 debug=0x8 -enable_kprintf_spam startup_debug=1',
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
