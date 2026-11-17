"""Small host checks for refusal boundaries and real public-key authentication."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
import package_source_storage as pack

ROOT = Path(__file__).resolve().parents[1]
SETTINGS = None


def real_bundle():
    policies = ROOT / 'scripts/policies/26C5035r'
    parts = [(policies / name / 'manifest.header.bin').read_bytes() for name in ('OS', 'SystemOS', 'App')]
    return struct.pack('<8sQQQ', b'VROOTB01', 1, 3, 1712) + b''.join(
        struct.pack('<QQ', 80 + index * 544, 544) for index in range(3)) + b''.join(parts)


class PackagingBoundaries(unittest.TestCase):
    def test_complete_args_do_not_inherit_diagnostics(self):
        args = '-v serial=3 native-source-storage=1 native-ufs-root=1 ramoops_reset_after=0'
        values = pack.validate_args(args)
        self.assertEqual(values['ramoops_reset_after'], '0')
        self.assertNotIn('rd', values)
        self.assertNotIn('TB710FU_SWITCH_MARKS', values)

    def test_ambiguous_legacy_or_hardware_args_are_refused(self):
        base = 'native-source-storage=1 native-ufs-root=1'
        for suffix in ('native-ufs-root=0', 'native-ufs-root=1', 'rd=md0', 'rootdev=disk0',
                       'ramoops_reset_after=120', 'native-ufs-probe=10', 'sheng-wifi=2',
                       'TB710FU_SWITCH_MARKS=1', 'ramoops=0xac340000', 'ramoops_ecc', '-v=1'):
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
