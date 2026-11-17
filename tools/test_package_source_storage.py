"""Small host checks for refusal boundaries and real public-key authentication."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
import package_source_storage as pack

ROOT = Path(__file__).resolve().parents[1]
SETTINGS = None


def synthetic_macho(names, kind=1, at=0, weak=None, subtype=0xc0000002):
    """Small executable-text/symbol-table fixtures, never production artifacts."""
    text_offset = 32 + 152 + 24
    text = b'\xc0\x03\x5f\xd6' * (len(names) + 1)
    symoff = (text_offset + len(text) + 7) & ~7
    stroff = symoff + 16 * len(names)
    strings, symbols = b'\0', bytearray()
    for index, name in enumerate(names):
        symbols += struct.pack('<IBBHQ', len(strings), 15, 1,
                               0x80 if name == weak else 0, 0x1000 + text_offset + index * 4)
        strings += name.encode() + b'\0'
    length = stroff + len(strings)
    header = struct.pack('<8I', 0xfeedfacf, 0x100000c, subtype, kind, 2, 176, 0, 0)
    segment = struct.pack('<II16s4Q4I', 0x19, 152, b'__TEXT', 0x1000, length,
                          at, length, 7, 5, 1, 0)
    section = struct.pack('<16s16sQQ8I', b'__text', b'__TEXT', 0x1000 + text_offset,
                          len(text), at + text_offset, 2, 0, 0, 0x80000400, 0, 0, 0)
    symtab = struct.pack('<6I', 2, 24, at + symoff, len(names), at + stroff, len(strings))
    return header + segment + section + symtab + text + bytes(symoff - text_offset - len(text)) + symbols + strings


def synthetic_fileset(names, weak=None):
    native_offset = 4096
    native = synthetic_macho(names, kind=2, at=native_offset, weak=weak)
    length = native_offset + len(native)
    header = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0xc0000002, 12, 2, 136, 0, 0)
    segment = struct.pack('<II16s4Q4I', 0x19, 72, b'__TEXT', 0x1000, length, 0, length, 7, 5, 0, 0)
    entry = struct.pack('<IIQQII', 0x80000035, 64, 0x1000, native_offset, 32, 0)
    entry += b'com.apple.kernel\0'.ljust(32, b'\0')
    return (header + segment + entry).ljust(native_offset, b'\0') + native


def synthetic_archive(members):
    result = bytearray(b'!<arch>\n')
    for name, data in members.items():
        body = name.encode() + data
        result += (f'{"#1/" + str(len(name)):<16}{0:<12}{0:<6}{0:<6}{100644:<8}{len(body):<10}`\n').encode()
        result += body + (b'\n' if len(body) % 2 else b'')
    return bytes(result)


def bind_cdc_fixture(fixture):
    fixture['source_bytes'] = json.dumps(fixture['source']).encode()
    fixture['integration']['archive_compile_report_sha256'] = pack.sha(fixture['source_bytes'])
    fixture['integration_bytes'] = json.dumps(fixture['integration']).encode()
    fixture['build']['usb_cdc_integration_report_sha256'] = pack.sha(fixture['integration_bytes'])
    fixture['build']['sha256'] = pack.sha(fixture['kernel'])


def cdc_fixture():
    names = [name for symbols in pack.CDC_SYMBOLS.values() for name in symbols]
    members = {name + '.o': synthetic_macho(symbols) for name, symbols in pack.CDC_SYMBOLS.items()}
    archive = synthetic_archive(members)
    archive_path = '/ci/tree/build/tb710fu-cdc-log/libTb710fuCdcLog.a'
    source = {
        'archive': archive_path, 'archive_sha256': pack.sha(archive),
        'framework_revision': pack.LOCK['unixnu']['commit'], 'xnu_revision': pack.LOCK['xnu_commit'],
        'linked_into_kernel': False, 'transport_sources_compiled': True,
        'runtime_qualified': False, 'cdc_enumeration_verified': False,
        'objects_sha256': {name: pack.sha(data) for name, data in members.items()},
        'objects': {name.removesuffix('.o') + '.c': pack.sha(data) for name, data in members.items()},
        'sources': {'kits/tb710fu/Tb710fuUsbCdc/' + name + '.c': pack.sha(name.encode())
                    for name in pack.CDC_SYMBOLS},
        'compile_commands_sha256': {name + '.c': 'c' * 64 for name in pack.CDC_SYMBOLS},
        'dependency_records_sha256': {name + '.c': 'd' * 64 for name in pack.CDC_SYMBOLS},
        'compile_record_sha256': 'e' * 64,
    }
    link = '-Wl,-force_load,' + archive_path + '\n'
    integration = {
        'schema_version': 1, 'profile': pack.LOCK['os_profile'], 'device_commit': 'b' * 40,
        'framework_revision': pack.LOCK['unixnu']['commit'], 'xnu_revision': pack.LOCK['xnu_commit'],
        'native_kernel_sha256': 'a' * 64, 'archive_sha256': pack.sha(archive),
        'bsd_object_sha256': '1' * 64, 'bsd_compile_record_sha256': '2' * 64,
        'bsd_startup_symbol': {'kind': 1, 'section': 0, 'desc': 0, 'value': '0x0'},
        'native_startup_symbol': {'kind': 15, 'section': 1, 'desc': 0, 'value': '0x10d0'},
        'linkarchives': link, 'linkarchives_sha256': pack.sha(link.encode()),
        'linked_into_kernel': True, 'transport_sources_compiled': True, 'archive_force_load_count': 1,
        'runtime_boot_argument': 'tb710fu-cdc=1', 'runtime_qualified': False,
        'cdc_enumeration_verified': False, 'hardware_boot_verified': False,
    }
    fixture = {
        'archive': archive, 'kernel': synthetic_fileset(names), 'members': members,
        'source': source, 'integration': integration,
        'official': {'inputs': {'native_sha256': 'a' * 64}},
        'storage': {'native_kernel_sha256': 'a' * 64, 'device_commit': 'b' * 40},
        'build': {'usb_cdc_transport_linked': True},
    }
    bind_cdc_fixture(fixture)
    return fixture


def verify_cdc_fixture(fixture):
    return pack.verify_usb_cdc(fixture['integration_bytes'], fixture['source_bytes'],
                               fixture['archive'], fixture['kernel'], fixture['official'],
                               fixture['build'], fixture['storage'])


def real_bundle():
    policies = ROOT / 'scripts/policies/26C5035r'
    parts = [(policies / name / 'manifest.header.bin').read_bytes() for name in ('OS', 'SystemOS', 'App')]
    return struct.pack('<8sQQQ', b'VROOTB01', 1, 3, 1712) + b''.join(
        struct.pack('<QQ', 80 + index * 544, 544) for index in range(3)) + b''.join(parts)


class PackagingBoundaries(unittest.TestCase):
    def test_reporter_manifest_preserves_original_and_requires_both_derivative_inputs(self):
        with tempfile.TemporaryDirectory(prefix='tb710fu-reporter-manifest-') as temporary:
            root = Path(temporary)
            files = {}
            for name in pack.INPUTS | pack.CDC_INPUTS | pack.DIAGNOSTIC_INPUTS:
                path = root / name
                path.write_bytes(name.encode())
                files[name] = {'path': path.name, 'sha256': pack.sha(path.read_bytes())}
            manifest = {'schema_version': 1, 'profile': pack.LOCK['os_profile'], 'framework': str(root),
                        'boot_args': 'native-source-storage=1 native-ufs-root=1 tb710fu-cdc=1',
                        'files': {name: files[name] for name in pack.INPUTS | pack.CDC_INPUTS}}
            path = root / 'inputs.json'

            def check():
                path.write_bytes(json.dumps(manifest).encode())
                return pack.checked_inputs(path, pack.sha(path.read_bytes()))

            with patch.object(pack, 'BASELINE', files['source_vendor']['sha256']), \
                    patch.object(pack, 'BASE_VBMETA', files['source_vbmeta']['sha256']):
                self.assertEqual(len(check()[1]), 15)
                manifest['files']['diagnostic_kernel'] = files['diagnostic_kernel']
                with self.assertRaisesRegex(ValueError, 'Manifest must name'):
                    check()
                manifest['files']['diagnostic_report'] = files['diagnostic_report']
                checked = check()[1]
                self.assertEqual(len(checked), 17)
                self.assertNotEqual(checked['kernel'], checked['diagnostic_kernel'])
                manifest['boot_args'] = 'native-source-storage=1 native-ufs-root=1'
                with self.assertRaisesRegex(ValueError, 'requires tb710fu-cdc'):
                    check()

    def test_cdc_manifest_requires_complete_transport_evidence(self):
        with tempfile.TemporaryDirectory(prefix='tb710fu-cdc-manifest-test-') as temporary:
            root = Path(temporary)
            files = {}
            for name in pack.INPUTS | pack.CDC_INPUTS:
                path = root / name
                path.write_bytes(name.encode())
                files[name] = {'path': path.name, 'sha256': pack.sha(path.read_bytes())}
            manifest = {'schema_version': 1, 'profile': pack.LOCK['os_profile'], 'framework': str(root),
                        'boot_args': 'native-source-storage=1 native-ufs-root=1',
                        'files': {name: files[name] for name in pack.INPUTS}}
            path = root / 'inputs.json'

            def check():
                path.write_text(json.dumps(manifest))
                return pack.checked_inputs(path, pack.sha(path.read_bytes()))

            with patch.object(pack, 'BASELINE', files['source_vendor']['sha256']), \
                    patch.object(pack, 'BASE_VBMETA', files['source_vbmeta']['sha256']):
                self.assertEqual(len(check()[1]), 12) # Existing manifests remain usable.
                manifest['boot_args'] += ' tb710fu-cdc=1'
                with self.assertRaisesRegex(ValueError, 'Manifest must name.*CDC'):
                    check() # The historical RAM tap alone is insufficient.
                for name in sorted(pack.CDC_INPUTS):
                    manifest['files'][name] = files[name]
                    if len(manifest['files']) != 15:
                        with self.assertRaisesRegex(ValueError, 'Manifest must name.*CDC'):
                            check()
                self.assertEqual(len(check()[1]), 15)
                manifest['files']['unknown_usb_override'] = files['usb_cdc_report']
                with self.assertRaisesRegex(ValueError, 'Manifest must name'):
                    check()

    def test_cdc_archive_and_actual_fileset_strong_definitions(self):
        record = verify_cdc_fixture(cdc_fixture())
        self.assertEqual(record['archive_objects_verified'], 6)
        self.assertFalse(record['cdc_enumeration_verified'])
        self.assertFalse(record['hardware_boot_verified'])
        self.assertEqual(record['strong_transport_definitions_verified'], 12)

    def test_cdc_bound_report_hashes_and_native_pairing_are_required(self):
        for case in ('build_flag', 'build_binding', 'compile_binding', 'prelink_hash',
                     'native_input', 'device_revision', 'archive_bytes'):
            fixture = cdc_fixture()
            if case == 'build_flag':
                fixture['build'].pop('usb_cdc_transport_linked')
            elif case == 'build_binding':
                fixture['integration_bytes'] += b' '
            elif case == 'compile_binding':
                fixture['source_bytes'] += b' '
            elif case == 'prelink_hash':
                fixture['build']['sha256'] = '0' * 64
            elif case == 'native_input':
                fixture['official']['inputs']['native_sha256'] = '0' * 64
            elif case == 'device_revision':
                fixture['storage']['device_commit'] = '0' * 40
            elif case == 'archive_bytes':
                fixture['archive'] += b'\0'
            with self.subTest(case=case), self.assertRaises(ValueError):
                verify_cdc_fixture(fixture)

    def test_cdc_old_tap_archive_cannot_claim_transport(self):
        fixture = cdc_fixture()
        fixture['members'].pop('CdcDwc3.o')
        fixture['members'].pop('CdcXnu.o')
        fixture['archive'] = synthetic_archive(fixture['members'])
        fixture['source']['archive_sha256'] = fixture['integration']['archive_sha256'] = pack.sha(fixture['archive'])
        fixture['source']['objects_sha256'] = {name: pack.sha(data) for name, data in fixture['members'].items()}
        fixture['source']['objects'] = {name.removesuffix('.o') + '.c': pack.sha(data)
                                        for name, data in fixture['members'].items()}
        bind_cdc_fixture(fixture)
        with self.assertRaisesRegex(ValueError, 'six compiled transport objects'):
            verify_cdc_fixture(fixture)

    def test_cdc_complete_claims_still_require_real_native_transport(self):
        names = [name for symbols in pack.CDC_SYMBOLS.values() for name in symbols]
        for case in ('missing_dwc3', 'weak_startup', 'duplicate_startup'):
            fixture = cdc_fixture()
            if case == 'missing_dwc3':
                fixture['kernel'] = synthetic_fileset([name for name in names if name != '_tb710fu_dwc3_poll'])
            elif case == 'weak_startup':
                fixture['kernel'] = synthetic_fileset(names, weak='_tb710fu_cdc_start')
            else:
                fixture['kernel'] = synthetic_fileset(names + ['_tb710fu_cdc_start'])
            bind_cdc_fixture(fixture)
            with self.subTest(case=case), self.assertRaisesRegex(ValueError, 'strong CDC definition'):
                verify_cdc_fixture(fixture)

    def test_cdc_archive_objects_need_real_target_code(self):
        for case in ('wrong_arch', 'missing_startup'):
            fixture = cdc_fixture()
            fixture['members']['CdcXnu.o'] = synthetic_macho(
                [] if case == 'missing_startup' else ['_tb710fu_cdc_start'],
                subtype=0 if case == 'wrong_arch' else 0xc0000002)
            fixture['archive'] = synthetic_archive(fixture['members'])
            fixture['source']['archive_sha256'] = fixture['integration']['archive_sha256'] = pack.sha(fixture['archive'])
            fixture['source']['objects_sha256']['CdcXnu.o'] = pack.sha(fixture['members']['CdcXnu.o'])
            fixture['source']['objects']['CdcXnu.c'] = pack.sha(fixture['members']['CdcXnu.o'])
            bind_cdc_fixture(fixture)
            with self.subTest(case=case), self.assertRaises(ValueError):
                verify_cdc_fixture(fixture)

    def test_cdc_runtime_claims_weak_hook_and_duplicate_load_are_refused(self):
        for case in ('enumeration', 'runtime', 'weak_import', 'duplicate_force_load'):
            fixture = cdc_fixture()
            if case == 'enumeration':
                fixture['integration']['cdc_enumeration_verified'] = True
            elif case == 'runtime':
                fixture['source']['runtime_qualified'] = True
            elif case == 'weak_import':
                fixture['integration']['bsd_startup_symbol']['desc'] = 0x40
            else:
                fixture['integration']['linkarchives'] *= 2
                fixture['integration']['linkarchives_sha256'] = pack.sha(fixture['integration']['linkarchives'].encode())
            bind_cdc_fixture(fixture)
            with self.subTest(case=case), self.assertRaises(ValueError):
                verify_cdc_fixture(fixture)

    def test_complete_args_do_not_inherit_diagnostics(self):
        args = '-v serial=3 native-source-storage=1 native-ufs-root=1 tb710fu-cdc=1 ramoops_reset_after=0'
        values = pack.validate_args(args)
        self.assertEqual(values['ramoops_reset_after'], '0')
        self.assertEqual(values['tb710fu-cdc'], '1')
        self.assertNotIn('rd', values)
        self.assertNotIn('TB710FU_SWITCH_MARKS', values)

    def test_ambiguous_legacy_or_hardware_args_are_refused(self):
        base = 'native-source-storage=1 native-ufs-root=1'
        for suffix in ('native-ufs-root=0', 'native-ufs-root=1', 'rd=md0', 'rootdev=disk0',
                       'ramoops_reset_after=120', 'native-ufs-probe=10', 'sheng-wifi=2',
                       'TB710FU_SWITCH_MARKS=1', 'ramoops=0xac340000', 'ramoops_ecc', '-v=1',
                       'tb710fu-cdc=0', 'tb710fu-cdc=2', 'tb710fu-cdc', 'tb710fu-cdc=0x1'):
            with self.subTest(suffix=suffix), self.assertRaises(ValueError):
                pack.validate_args(base + ' ' + suffix)
        with self.assertRaises(ValueError):
            pack.validate_args('native-ufs-root=1')

    def test_real_bundle_and_table_boundary_failures(self):
        data = real_bundle()
        self.assertEqual(pack.sha(data), pack.POLICIES)
        self.assertEqual([len(part) for part in pack.parse_bundle(data)], [544, 544, 544])
        for offset, value in ((16, 2), (24, 1711), (32, 79), (48, 80), (64, 1169)):
            bad = bytearray(data)
            struct.pack_into('<Q', bad, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                pack.parse_bundle(bad)
        with self.assertRaises(ValueError):
            pack.parse_bundle(data[:-1])

    def test_real_signatures_and_mutated_signed_content(self):
        policies = ROOT / 'scripts/policies/26C5035r'
        key = (policies / 'OS/trusted-key.bin').read_bytes()
        with tempfile.TemporaryDirectory(prefix='tb710fu-public-policy-test-') as temporary:
            work = Path(temporary)
            for name in ('OS', 'SystemOS', 'App'):
                header = (policies / name / 'manifest.header.bin').read_bytes()
                pack.verify_owner_signature(header, key, work)
                for offset in (40, 152, 160, 543):
                    bad = bytearray(header)
                    bad[offset] ^= 1
                    with self.subTest(policy=name, offset=offset), self.assertRaises(ValueError):
                        pack.verify_owner_signature(bad, key, work)
        for bad in (key[:-1], b'PRIVATE' + key[7:], key[:-3] + b'\0\0\3'):
            with self.assertRaises(ValueError):
                pack.public_pem(bad)

    def test_duplicate_json_keys_and_missing_inputs_are_refused(self):
        with self.assertRaises(ValueError):
            pack.json_bytes(b'{"schema_version":1,"schema_version":1}')
        with tempfile.TemporaryDirectory(prefix='tb710fu-missing-prelink-test-') as temporary:
            root = Path(temporary)
            manifest = root / 'inputs.json'
            manifest.write_text(json.dumps({'schema_version': 1, 'profile': pack.LOCK['os_profile'],
                                           'framework': str(SETTINGS.framework if SETTINGS else ROOT / '.work/UniXNU'),
                                           'boot_args': 'native-source-storage=1 native-ufs-root=1', 'files': {}}))
            output = root / 'output'
            with self.assertRaisesRegex(ValueError, 'Manifest must name'):
                pack.package(manifest, pack.sha(manifest.read_bytes()), output)
            self.assertFalse(output.exists())

    def test_retained_non_ufs_prelink_is_refused(self):
        if not SETTINGS or not SETTINGS.retained_prelink:
            self.skipTest('pass --retained-prelink for the real old-fileset refusal')
        directory = SETTINGS.retained_prelink
        official = json.loads((directory / 'kernel/kernel.json').read_text())
        pairing = json.loads((directory / 'kernel/kernel.map.json').read_text())
        build = json.loads((directory / 'build-result.json').read_text())
        data = (directory / 'kernel/kernel.macho').read_bytes()
        profile = pack.tomllib.loads((SETTINGS.framework / f'profiles/os/{pack.LOCK["os_profile"]}/profile.toml').read_text())
        pack.verify_image(data, 'prelinked', official)  # Structurally real, contains no asserted UFS integration.
        with self.assertRaisesRegex(ValueError, 'not built from the source-storage integrated'):
            pack.verify_prelink(data, official, pairing, build, {'native_kernel_sha256': '0' * 64}, profile)

    def test_fixed_adt_extension_and_explicit_args(self):
        if not SETTINGS or not SETTINGS.baseline:
            self.skipTest('pass --baseline for the actual ADT/TOML readback test')
        serializer, profile, _, _ = pack.pinned_tools(SETTINGS.framework)
        image = str(SETTINGS.baseline) + '@@4096'
        adt = subprocess.check_output(['mtype', '-i', image, '::/EFI/UniXNU/Payloads/kernel.macho.adt'])
        config = subprocess.check_output(['mtype', '-i', image, '::/EFI/UniXNU/unixnu.toml'])
        changed, report = pack.extend_adt(adt, serializer, profile)
        self.assertEqual(pack.sha(changed), pack.ROOT_ADT)
        self.assertTrue(report['all_original_properties_and_random_seed_preserved'])
        args = 'native-source-storage=1 native-ufs-root=1'
        parsed = pack.tomllib.loads(pack.update_config(config, args).decode())
        root = next(entry for entry in parsed['entry'] if entry['id'] == 'xnu')
        self.assertEqual(root['args'], args)
        with self.assertRaises(ValueError):
            pack.extend_adt(changed, serializer, profile)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--framework', type=Path, default=ROOT / '.work/UniXNU')
    parser.add_argument('--retained-prelink', type=Path)
    parser.add_argument('--baseline', type=Path)
    SETTINGS = parser.parse_args()
    unittest.main(argv=[sys.argv[0]], verbosity=2)
