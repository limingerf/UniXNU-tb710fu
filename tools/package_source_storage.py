"""Prepare only an unsigned, provenance-bound TB710FU source-storage payload.

Run with Python 3.11+, mtools, clang and OpenSSL (normally under WSL). Every
image/report input is named and hashed in a reviewed JSON manifest. No signing
key, device command, network request or fetched-source mutation is used.
"""
import argparse
import base64
from copy import deepcopy
import hashlib
import importlib
import json
import lzma
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import tomllib
import uuid
import xml.etree.ElementTree as ET
import zipfile
import zlib

sys.dont_write_bytecode = True

from check_adt_identity import inspect as inspect_adt, name_of, read_nodes
from check_payload_fat import geometry
from verify_kernel import verify_image
from verify_storage_integration import force_load_count, hook_symbol, load_audit

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / 'upstream.lock.json').read_text())
BASELINE = '15846226236b56bd3abb104ae57f45e23307f3ea52321fd03ed153fd2e4e0718'
BASE_VBMETA = 'e4cffaf3414a2c22d6c129ed62ac382424f67da8a0dd62aa9ae0ccb73fec63de'
BASE_ADT = 'f4161ff940b78e777ca2a6f4cb02191ff00f5046cc4dc3302d8fc4ce00f9c1a0'
ROOT_ADT = '61a7ef52d4375de26a4355b9531a29053c2b19f392dab4dd0d4d98f43ca443e7'
POLICIES = '49da45a7bbc6360f50bb3efabae7dc0842513b701bbb1d297cd3bd333923cb68'
OWNER_KEY = 'c2d8d462d0a790056bef2dddc30ae60ce210dbf0ee1cd4350a21e864db3a7d7e'
LOADER_BOOT = 'a3236accbde1c3b12f09c8cee8c561bed12542f95fbff50526b3e4e7bd817a83'
LOADER_ZIP = 'a76530c6d0a27fb9b428f7902817e551fa5927cfe935c0c80a821dc4ae49abfa'
LOADER_FD = '5722252b40e37d6c94e252c8daeddd9f3499b5483bca44f0cba93c795fb78a71'
LOADER_PE = '258fc45fcc1e1cd0bb982015e1e59cd0a48ec9cc9ac82c3b90dec78ae2d84079'
LOADER_HEAD = '0ea7095d5d99a8d66b4ff8b77bbdf9268e6cfd52'
LOADER_GUID = uuid.UUID('1b64af87-8415-4b57-9b16-680d70913b9f')
LZMA_GUID = uuid.UUID('ee4e5898-3914-4259-9d6e-dc7bd79403cf')
INPUTS = {'source_vendor', 'source_vbmeta', 'kernel', 'kernel_report', 'kernel_map',
          'build_report', 'integration_report', 'source_storage_archive',
          'source_storage_report', 'root_policies', 'loader_boot', 'loader_artifact'}
CDC_INPUTS = {'usb_cdc_integration_report', 'usb_cdc_archive', 'usb_cdc_report'}
CDC_SYMBOLS = {
    'CdcKernelTap': ('_tb710fu_cdc_log_tap_putc', '_tb710fu_cdc_log_tap_copy',
                     '_tb710fu_cdc_log_tap_consume'),
    'CdcLogRing': ('_tb710fu_cdc_log_ring_append', '_tb710fu_cdc_log_ring_copy',
                   '_tb710fu_cdc_log_ring_consume'),
    'CdcAcmProtocol': ('_tb710fu_cdc_classify_setup',),
    'CdcTransportGuard': ('_tb710fu_cdc_preflight',),
    'CdcDwc3': ('_tb710fu_dwc3_start', '_tb710fu_dwc3_poll', '_tb710fu_dwc3_stop'),
    'CdcXnu': ('_tb710fu_cdc_start',),
}
OBJECTS = {'IOBlockStorageDevice.late.o', 'NativeUfs.late.o', 'SourceStorage.o',
           'SourceRootPolicies.o', 'SourceRootBinding.o', 'verified_root.o',
           'xnu_crypto.o', 'RootAnchor.o'}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def json_bytes(data):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, f'Duplicate JSON key: {key}')
            result[key] = value
        return result
    return json.loads(data, object_pairs_hook=unique)


def validate_args(text):
    require(isinstance(text, str) and 0 < len(text.encode()) < 1024,
            'Supply the complete nonempty boot_args string, under 1024 bytes')
    require(all(32 <= ord(c) <= 126 for c in text) and ' '.join(text.split()) == text,
            'boot_args must be canonical ASCII tokens separated by one space')
    values = {}
    for token in text.split(' '):
        key, sep, value = token.partition('=')
        require(key not in values, f'Duplicate boot argument: {key}')
        values[key] = value if sep else None
    for key in ('native-source-storage', 'native-ufs-root'):
        require(values.get(key) == '1', f'Require exactly {key}=1')
    # This is deliberately a minimal reviewed set. No Sheng hardware/probe
    # parameters, root-device overrides, memory disk or old physical ramoops PA.
    allowed = {'-v', 'serial', 'native-source-storage', 'native-ufs-root',
               'tb710fu-cdc',
               'ramoops_size', 'ramoops_ecc', 'ramoops_panic_delay', 'ramoops_reset_after'}
    require(set(values) <= allowed, 'Unsupported boot arguments: ' + ', '.join(sorted(set(values) - allowed)))
    require('-v' not in values or values['-v'] is None, '-v takes no value')
    if 'tb710fu-cdc' in values:
        require(values['tb710fu-cdc'] == '1', 'tb710fu-cdc must be exactly 1 when supplied')
    for key, expected in (('serial', 3), ('ramoops_size', 0x40000), ('ramoops_reset_after', 0)):
        if key in values:
            require(values[key] is not None and int(values[key], 0) == expected,
                    f'{key} must be {expected:#x}; old reset/address overrides are refused')
    if 'ramoops_ecc' in values:
        require(values['ramoops_ecc'] is not None and int(values['ramoops_ecc'], 0) in (0, 16),
                'ramoops_ecc must be 0 or 16')
    if 'ramoops_panic_delay' in values:
        require(values['ramoops_panic_delay'] is not None and 0 <= int(values['ramoops_panic_delay'], 0) <= 300,
                'ramoops_panic_delay must be between 0 and 300 seconds')
    return values


def fixed_file(repository, revision, name):
    require(subprocess.check_output(['git', '-C', str(repository), 'rev-parse', 'HEAD'], text=True).strip() == revision,
            f'Unexpected fixed source revision: {repository}')
    expected = subprocess.check_output(['git', '-C', str(repository), 'show', revision + ':' + name])
    actual = (repository / name).read_bytes()
    require(actual.replace(b'\r\n', b'\n') == expected.replace(b'\r\n', b'\n'),
            f'Fixed source file has local changes: {repository / name}')
    return sha(expected)


def pinned_tools(framework):
    records = {}
    for name in ('scripts/unixnu/devicetree.py', 'scripts/unixnu/util.py',
                 f'profiles/os/{LOCK["os_profile"]}/profile.toml'):
        records[name] = fixed_file(framework, LOCK['unixnu']['commit'], name)
    loader = framework / 'drivers/DarwinBoot'
    for name in ('DarwinBootPkg/Driver/DarwinBoot.c', 'Test/test-kernel-parser.py'):
        records['DarwinBoot/' + name] = fixed_file(loader, LOCK['darwin_boot']['commit'], name)
    sys.path.insert(0, str(framework / 'scripts'))
    module = importlib.import_module('unixnu.devicetree')
    require(Path(module.__file__).resolve() == (framework / 'scripts/unixnu/devicetree.py').resolve(),
            'An unrelated unixnu.devicetree module was already loaded')
    profile = tomllib.loads((framework / f'profiles/os/{LOCK["os_profile"]}/profile.toml').read_text())
    return module.node, profile, loader / 'Test/test-kernel-parser.py', records


def parse_bundle(data):
    require(len(data) == 1712 and data[:8] == b'VROOTB01', 'Require the complete 1712-byte RootPolicies envelope')
    require(struct.unpack_from('<QQQ', data, 8) == (1, 3, len(data)), 'Wrong RootPolicies version/count/length')
    parts, cursor = [], 80
    for index in range(3):
        offset, size = struct.unpack_from('<QQ', data, 32 + index * 16)
        require(offset == cursor and size == 544 and offset + size <= len(data),
                'RootPolicies table is overlapping, gapped, out of order or out of bounds')
        parts.append(data[offset:offset + size])
        cursor += size
    require(cursor == len(data), 'RootPolicies trailing bytes')
    return parts


def der(tag, value):
    length = len(value)
    encoded = bytes((length,)) if length < 128 else bytes((0x80 + (length.bit_length() + 7) // 8,)) + length.to_bytes((length.bit_length() + 7) // 8, 'big')
    return bytes((tag,)) + encoded + value


def public_pem(key):
    require(len(key) == 395 and key[:8] == b'VROOTKEY' and key[8] & 0x80 and key[391] & 1 and key[392:] == b'\1\0\1',
            'Require the complete RSA-3072 VROOTKEY public owner anchor, exponent 65537')
    rsa = der(0x30, der(2, b'\0' + key[8:392]) + der(2, key[392:]))
    spki = der(0x30, bytes.fromhex('300d06092a864886f70d0101010500') + der(3, b'\0' + rsa))
    body = base64.b64encode(spki).decode()
    return ('-----BEGIN PUBLIC KEY-----\n' + '\n'.join(body[i:i + 64] for i in range(0, len(body), 64)) + '\n-----END PUBLIC KEY-----\n').encode()


def verify_owner_signature(header, key, work):
    require(len(header) == 544, 'Require a complete signed policy header')
    key_file = work / 'owner-public.pem'
    message, signature = work / 'policy-header.bin', work / 'policy-signature.bin'
    key_file.write_bytes(public_pem(key))
    message.write_bytes(header[:160])
    signature.write_bytes(header[160:])
    result = subprocess.run(['openssl', 'dgst', '-sha256', '-verify', str(key_file),
                             '-signature', str(signature), str(message)], capture_output=True, text=True)
    require(result.returncode == 0, 'Real OpenSSL RSA-3072 signature verification failed')


def verify_policies(data, profile, archive_report, work):
    require(sha(data) == POLICIES, 'RootPolicies differs from the reviewed profile-bound bundle')
    parts = parse_bundle(data)
    policy_dir = ROOT / 'scripts/policies' / profile['ipsw']['build']
    key = (policy_dir / 'OS/trusted-key.bin').read_bytes()  # Public data only.
    require(sha(key) == OWNER_KEY, 'Owner public anchor fingerprint differs from the reviewed key')
    records = {}
    for name, header in zip(('OS', 'SystemOS', 'App'), parts):
        directory = policy_dir / name
        verification = json_bytes((directory / 'verification.json').read_bytes())
        require((directory / 'trusted-key.bin').read_bytes() == key and (directory / 'manifest.header.bin').read_bytes() == header,
                f'{name}: bundle differs from the committed policy and public anchor')
        image_bytes, blocks = struct.unpack_from('<QQ', header, 24)
        generation = struct.unpack_from('<Q', header, 152)[0]
        identity = str(uuid.UUID(bytes=header[40:56])).upper()
        digest = profile['root'][name.lower() + '_image_sha256']
        require(header[:8] == b'VROOT01\0' and struct.unpack_from('<4I', header, 8) == (1, 160, 4096, 0),
                f'{name}: signed header layout differs')
        require(image_bytes > 0 and blocks == (image_bytes + 4095) // 4096 and generation >= 1 and
                identity == profile['root'][name.lower() + '_volume_uuid'].upper() and
                header[56:88].hex() == digest and header[120:152].hex() == OWNER_KEY,
                f'{name}: signed policy does not match the exact OS profile/public anchor')
        require(verification.get('real_c_signature_and_block_verification') is True and
                verification['image_sha256'] == digest and verification['volume_uuid'].upper() == identity and
                verification['trusted_key_sha256'] == OWNER_KEY and verification['image_bytes'] == image_bytes and
                verification['blocks_verified'] == blocks and verification['generation'] == generation,
                f'{name}: retained full-image verification record differs')
        verify_owner_signature(header, key, work)
        records[name] = {'header_sha256': sha(header), 'verification_sha256': sha((directory / 'verification.json').read_bytes()),
                         'image_sha256': digest, 'volume_uuid': identity, 'image_bytes': image_bytes,
                         'block_count': blocks, 'generation': generation}
    policy_report = archive_report['policies']
    require(policy_report['bundle_sha256'] == sha(data) and policy_report['bundle_bytes'] == len(data) and
            policy_report['trusted_key_sha256'] == OWNER_KEY and policy_report['policies'] == records,
            'RootPolicies/public anchor metadata differs from the actual archive compile report')
    return {'bytes': len(data), 'sha256': sha(data), 'policies': records,
            'public_key': {'format': 'VROOTKEY', 'bytes': len(key), 'modulus_bits': 3072,
                           'exponent': 65537, 'sha256': sha(key)},
            'openssl_signature_verification': 'PASS', 'runtime_authentication_required': True,
            'current_full_root_image_bytes_rechecked': False, 'private_key_read': False}


def archive_objects(data):
    require(data[:8] == b'!<arch>\n', 'Source-storage input is not an ar archive')
    result, offset = {}, 8
    while offset < len(data):
        require(offset + 60 <= len(data) and data[offset + 58:offset + 60] == b'`\n', 'Truncated ar header')
        name = data[offset:offset + 16].decode('ascii').strip()
        length = int(data[offset + 48:offset + 58])
        body = data[offset + 60:offset + 60 + length]
        require(len(body) == length, 'Truncated ar member')
        if name.startswith('#1/'):
            size = int(name[3:])
            require(size <= len(body), 'Invalid BSD ar member name')
            name, body = body[:size].rstrip(b'\0').decode(), body[size:]
        else:
            name = name.rstrip('/')
        if not name.startswith('__.SYMDEF'):
            require(name not in result, 'Duplicate archive member: ' + name)
            result[name] = sha(body)
        offset += 60 + length + length % 2
    require(offset == len(data), 'ar trailing bytes')
    return result


def verify_integration(integration, source, archive, policies, native_sha):
    require(integration.get('schema_version') == 1 and integration.get('profile') == LOCK['os_profile'] and
            integration.get('framework_revision') == LOCK['unixnu']['commit'], 'Missing exact source-storage integration report')
    require(re.fullmatch(r'[0-9a-f]{40}', integration.get('device_commit', '')) is not None,
            'Integration report lacks an exact device commit')
    for key in ('linked_into_kernel', 'source_storage_selected', 'startup_hook_selected',
                'startup_reference_strong', 'startup_symbol_defined', 'late_metaclasses_verified'):
        require(integration.get(key) is True, 'Integration gate did not pass: ' + key)
    require(integration.get('archive_force_load_count') == 1 and integration.get('native_ufs_personalities_selected') is False,
            'Require one archive force-load and no automatic NativeUfs personalities')
    require(integration.get('runtime_qualified') is False and integration.get('hardware_boot_verified') is False,
            'This packaging stage must retain separate runtime/hardware qualification')
    require(integration.get('native_kernel_sha256') == native_sha and
            integration.get('archive_sha256') == sha(archive) == source.get('archive_sha256') and
            integration.get('policy_bundle_sha256') == sha(policies),
            'Prelink native/archive/policy hashes differ from the actual integration')
    require(source.get('framework_revision') == LOCK['unixnu']['commit'] and
            source.get('ufs_revision') == '99d8bcd3e43dd24fc3c8437c67b5d4cc71ab94d0' and
            source.get('storage_revision') == '7edb88fbae296fb7c8ce2f64e115e116e566d51c' and
            source.get('bridge_reference_revision') == '1108068537d5396edb2c63b2f8c6f5b97b0a4bbc' and
            source.get('target') == 'arm64e-apple-ios27.0' and source.get('archive_bytes') == len(archive),
            'Archive source/profile/architecture identity differs')
    members = archive_objects(archive)
    require(set(members) == OBJECTS and members == source['objects_sha256'],
            'Actual complete archive members/hashes differ from the compile record')
    return {'device_commit': integration['device_commit'], 'native_kernel_sha256': native_sha,
            'archive_sha256': sha(archive), 'archive_objects_verified': len(members),
            'startup_symbol': '_native_source_storage_start', 'strong_startup_and_late_metaclasses': True}


def verify_prelink(data, official, pairing, build, integration, profile):
    checked = verify_image(data, 'prelinked', official)
    require(build.get('stage') == 'prelinked' and build.get('profile') == LOCK['os_profile'] and
            build.get('source_commit') == LOCK['unixnu']['commit'] and build.get('prelink_skipped') is False and
            build.get('sha256') == sha(data) and build.get('bytes') == len(data),
            'Require the real complete prelinked build result for this exact kernel')
    require(official['inputs']['native_sha256'] == integration.get('native_kernel_sha256'),
            'Prelinked kernel was not built from the source-storage integrated native kernel')
    require({'com.apple.iokit.IOStorageFamily', 'com.apple.filesystems.apfs'} <= set(official['closure']),
            'Prelink does not retain the real IOStorageFamily and APFS')
    roots = profile['kernel']['prelink']['roots']
    require({r for r in roots if not r.startswith('!')} <= set(official['closure']) and
            not {r[1:] for r in roots if r.startswith('!')} & set(official['closure']), 'Prelink profile closure differs')
    identifier, address = profile['kernel']['prelink']['early_security_init'].split('=', 1)
    require(official.get('early_security_init', {}).get(identifier) == address, 'Prelink security initializer differs')
    require(pairing.get('method') == 'symbol-names' and pairing.get('inputs') == official['inputs'] and
            pairing.get('matcher_version') == official.get('matcher_version') and
            pairing.get('matcher_signature') == official.get('matcher_signature') and
            pairing.get('conflicts') == [] and pairing.get('stats', {}).get('conflicts') == {},
            'Prelink symbol mapping is incomplete, conflicted or has different inputs')
    return checked


def require_cdc_definition(image, name, context):
    symbols = hook_symbol(image, name)
    require(len(symbols) == 1 and symbols[0]['kind'] & 14 == 14 and
            symbols[0]['kind'] & 1 and not symbols[0]['desc'] & 0x80,
            f'{context}: require one real strong CDC definition: {name}')
    symbol = symbols[0]
    require(0 < symbol['section'] <= len(image.sections),
            f'{context}: CDC definition has no real section: {name}')
    section = image.sections[symbol['section'] - 1]
    address = int(symbol['value'], 16)
    require(section['name'] == '__text' and section['flags'] & 0x80000000 and
            section['va'] <= address and address + 4 <= section['va'] + section['size'],
            f'{context}: CDC definition is not backed by executable text: {name}')
    require(image.offset(address, 4) + 4 <= len(image.data),
            f'{context}: CDC instruction bytes are out of bounds: {name}')
    return symbol


def verify_usb_cdc(integration_bytes, source_bytes, archive, kernel, official, build, storage):
    """Bind optional CDC evidence and inspect the actual retained native code.

    The native input hash is the official prelink's input hash: emitting a
    fileset rewrites file offsets, so hashing its embedded slice is not an
    independent hash of the original native executable.
    """
    integration, source = json_bytes(integration_bytes), json_bytes(source_bytes)
    require(build.get('usb_cdc_transport_linked') is True and
            build.get('usb_cdc_integration_report_sha256') == sha(integration_bytes),
            'Build result does not bind this exact CDC transport integration report')
    require(build.get('sha256') == sha(kernel), 'CDC build result describes a different prelinked kernel')
    require(integration.get('schema_version') == 1 and integration.get('profile') == LOCK['os_profile'] and
            integration.get('framework_revision') == LOCK['unixnu']['commit'] and
            integration.get('xnu_revision') == LOCK['xnu_commit'],
            'Missing exact CDC transport integration source/profile identity')
    require(re.fullmatch(r'[0-9a-f]{40}', integration.get('device_commit', '')) is not None and
            integration['device_commit'] == storage.get('device_commit'),
            'CDC and source-storage integration reports describe different device revisions')
    require(integration.get('native_kernel_sha256') == official['inputs']['native_sha256'] ==
            storage.get('native_kernel_sha256'),
            'CDC report does not describe this exact prelink native input')
    for key in ('linked_into_kernel', 'transport_sources_compiled'):
        require(integration.get(key) is True, 'CDC integration gate did not pass: ' + key)
    require(integration.get('archive_force_load_count') == 1 and
            integration.get('runtime_boot_argument') == 'tb710fu-cdc=1',
            'CDC requires one real force-load and its exact opt-in argument')
    for report in (integration, source):
        for key in ('runtime_qualified', 'cdc_enumeration_verified'):
            require(report.get(key) is False, 'CDC packaging must retain unverified hardware/runtime qualification: ' + key)
    require(integration.get('hardware_boot_verified') is False,
            'CDC packaging must retain unverified hardware boot')
    require(source.get('framework_revision') == LOCK['unixnu']['commit'] and
            source.get('xnu_revision') == LOCK['xnu_commit'] and
            source.get('transport_sources_compiled') is True and source.get('linked_into_kernel') is False,
            'CDC compile record is not the complete pinned transport archive stage')
    require(integration.get('archive_compile_report_sha256') == sha(source_bytes),
            'CDC integration does not bind this exact archive compile report')
    require(integration.get('archive_sha256') == sha(archive) == source.get('archive_sha256'),
            'CDC actual archive differs from the bound integration/compile reports')
    members = archive_objects(archive)
    expected = {name + '.o' for name in CDC_SYMBOLS}
    require(set(members) == expected and members == source.get('objects_sha256') and
            source.get('objects') == {name + '.c': members[name + '.o'] for name in CDC_SYMBOLS},
            'CDC actual archive must match all six compiled transport objects')
    c_sources = {'kits/tb710fu/Tb710fuUsbCdc/' + name + '.c' for name in CDC_SYMBOLS}
    require(isinstance(source.get('sources'), dict) and c_sources <= set(source['sources']) and
            all(re.fullmatch(r'[0-9a-f]{64}', value) for value in source['sources'].values()),
            'CDC compile report lacks the complete transport source hashes')
    for key in ('compile_commands_sha256', 'dependency_records_sha256'):
        records = source.get(key, {})
        require(isinstance(records, dict) and set(records) == {name + '.c' for name in CDC_SYMBOLS} and
                all(re.fullmatch(r'[0-9a-f]{64}', value) for value in records.values()),
                'CDC compile report lacks exact target compile provenance: ' + key)
    require(re.fullmatch(r'[0-9a-f]{64}', source.get('compile_record_sha256', '')) is not None,
            'CDC compile report lacks the actual XNU C compile record hash')
    imported = integration.get('bsd_startup_symbol', {})
    require(set(imported) == {'kind', 'section', 'desc', 'value'} and
            imported.get('kind', 0) & 14 == 0 and imported.get('kind', 0) & 1 and
            not imported.get('desc', 0) & 0x40 and imported.get('section') == 0,
            'CDC BSD startup reference is not a strong real import')
    defined = integration.get('native_startup_symbol', {})
    require(set(defined) == {'kind', 'section', 'desc', 'value'} and
            defined.get('kind', 0) & 14 == 14 and defined.get('kind', 0) & 1 and
            not defined.get('desc', 0) & 0x80 and defined.get('section', 0) > 0,
            'CDC native integration startup is not a strong definition')
    for key in ('bsd_object_sha256', 'bsd_compile_record_sha256', 'linkarchives_sha256'):
        require(re.fullmatch(r'[0-9a-f]{64}', integration.get(key, '')) is not None,
                'CDC integration lacks actual link/startup provenance: ' + key)
    linkarchives = integration.get('linkarchives', '')
    require(sha(linkarchives.encode()) == integration['linkarchives_sha256'],
            'CDC integration link input text/hash differ')
    require(isinstance(source.get('archive'), str) and
            source['archive'].endswith('/build/tb710fu-cdc-log/libTb710fuCdcLog.a') and
            force_load_count(linkarchives, source['archive']) == 1,
            'CDC actual link input must force-load this exact source archive once')
    audit = load_audit()
    actual = {name.rstrip('/'): data for name, data in audit.ar_members(archive)
              if not name.startswith('__.SYMDEF')}
    try:
        for component, names in CDC_SYMBOLS.items():
            image = audit.MachO(actual[component + '.o'])
            require(image.kind == 1 and image.subtype & 0xffffff == 2,
                    'CDC archive member is not an arm64e target object: ' + component)
            for name in names:
                require_cdc_definition(image, name, component)
        fileset = audit.MachO(kernel)
        require(fileset.kind == 12 and 'com.apple.kernel' in fileset.entries,
                'CDC transport requires the real prelinked native fileset entry')
        native = audit.MachO(kernel, fileset.entries['com.apple.kernel'][1])
        require(native.kind == 2 and native.subtype & 0xffffff == 2,
                'CDC fileset native entry is not arm64e MH_EXECUTE')
        for names in CDC_SYMBOLS.values():
            for name in names:
                require_cdc_definition(native, name, 'Prelinked native kernel')
    except (AssertionError, KeyError, StopIteration, struct.error, UnicodeDecodeError) as error:
        raise ValueError('CDC archive/fileset Mach-O evidence is incomplete or invalid') from error
    return {'device_commit': integration['device_commit'],
            'native_kernel_sha256': integration['native_kernel_sha256'],
            'archive_sha256': sha(archive), 'archive_objects_verified': len(members),
            'integration_report_sha256': sha(integration_bytes),
            'archive_compile_report_sha256': sha(source_bytes),
            'strong_transport_definitions_verified': sum(map(len, CDC_SYMBOLS.values())),
            'runtime_boot_argument': integration['runtime_boot_argument'],
            'cdc_enumeration_verified': False, 'runtime_qualified': False,
            'hardware_boot_verified': False}


def unique_child(node, name):
    matches = [child for child in node[1] if name_of(child) == name]
    require(len(matches) == 1, 'Require unique ADT node: ' + name)
    return matches[0]


def extend_adt(data, serializer, profile):
    require(sha(data) == BASE_ADT, 'ADT is not the reviewed correct-identity baseline')
    tree = read_nodes(data)

    def encode(node):
        names = [key for key, _ in node[0]]
        require(len(names) == len(set(names)), 'Duplicate ADT property')
        return serializer(name_of(node), [(k, v) for k, v in node[0] if k != 'name'], [encode(c) for c in node[1]])

    require(encode(tree) == data, 'Pinned ADT serializer does not byte-roundtrip the baseline')
    chosen = unique_child(tree, 'chosen')
    props = dict(chosen[0])
    expected = profile['adt']['chosen']['string']
    for key in ('boot-uuid', 'apfs-preboot-uuid', 'osenvironment'):
        require(props[key].rstrip(b'\0').decode() == expected[key], 'ADT/profile chosen identity mismatch: ' + key)
    matching = ET.fromstring(props['root-matching'].rstrip(b'\0'))
    require([item.text for item in matching.iter('string')] == ['IOMedia', profile['root']['os_volume_uuid'].upper()],
            'ADT root-matching UUID differs from the OS policy')
    memory = unique_child(chosen, 'memory-map')
    require(all(key != 'RootPolicies' for key, _ in memory[0]), 'ADT already contains RootPolicies')
    memory[0].append(('RootPolicies', bytes(16)))
    output = encode(tree)
    require(len(output) == len(data) + 52 and sha(output) == ROOT_ADT, 'Unexpected ADT extension bytes')
    checked = inspect_adt(output, profile['ipsw']['product'])
    require(not any(item['result'] == 'FAIL' for item in checked['findings']), 'Extended ADT identity verification failed')
    return output, {'before_sha256': sha(data), 'sha256': sha(output), 'bytes': len(output),
                    'only_change': 'append zero16 /chosen/memory-map/RootPolicies',
                    'all_original_properties_and_random_seed_preserved': True, 'identity': checked}


def update_config(data, args):
    original = tomllib.loads(data.decode())
    matches = [entry for entry in original['entry'] if entry.get('id') == 'xnu' and entry.get('type') == 'xnu']
    require(len(matches) == 1 and matches[0].get('path') == 'Payloads/kernel.macho' and matches[0].get('enabled') is True and
            original['boot'].get('default') == 'xnu', 'Unexpected root entry/path/default in baseline')
    sections = re.split(r'(?m)^(?=\[\[entry\]\])', data.decode())
    count = 0
    for index, section in enumerate(sections):
        if section.startswith('[[entry]]') and tomllib.loads(section)['entry'][0].get('id') == 'xnu':
            sections[index], changed = re.subn(r'(?m)^args\s*=.*$', lambda _: 'args = ' + json.dumps(args), section)
            require(changed == 1, 'Require one explicit root args field')
            count += 1
    require(count == 1, 'Could not replace exactly one root entry')
    result = ''.join(sections).encode()
    expected = deepcopy(original)
    next(e for e in expected['entry'] if e.get('id') == 'xnu')['args'] = args
    require(tomllib.loads(result.decode()) == expected, 'Config change exceeded the explicit root args replacement')
    return result


def darwinboot_pe(fd):
    found, visited = [], set()

    def sections(blob, depth):
        offset = 0
        while offset + 4 <= len(blob):
            size, kind, header = int.from_bytes(blob[offset:offset + 3], 'little'), blob[offset + 3], 4
            if size == 0xffffff:
                require(offset + 8 <= len(blob), 'Truncated large firmware section')
                size, header = struct.unpack_from('<I', blob, offset + 4)[0], 8
            require(size >= header and offset + size <= len(blob), 'Invalid firmware section bounds')
            part = blob[offset:offset + size]
            if kind == 2 and size >= header + 20 and uuid.UUID(bytes_le=part[header:header + 16]) == LZMA_GUID:
                start = struct.unpack_from('<H', part, header + 16)[0]
                require(header + 20 <= start <= size, 'Invalid guided LZMA offset')
                decoded = lzma.decompress(part[start:], format=lzma.FORMAT_ALONE, memlimit=256 << 20)
                require(len(decoded) <= 128 << 20, 'Oversized firmware guided section')
                sections(decoded, depth + 1)
                scan(decoded, depth + 1)
            elif kind == 0x17:
                scan(part[header:], depth + 1)
            offset = (offset + size + 3) & ~3

    def scan(blob, depth=0):
        require(depth <= 8, 'Firmware recursion limit')
        position = 0
        while True:
            signature = blob.find(b'_FVH', position)
            if signature < 0:
                return
            position, start = signature + 4, signature - 40
            if start < 0 or start + 56 > len(blob):
                continue
            size, header = struct.unpack_from('<Q', blob, start + 32)[0], struct.unpack_from('<H', blob, start + 48)[0]
            if header < 56 or header % 2 or size < header or start + size > len(blob):
                continue
            volume = blob[start:start + size]
            if sum(struct.unpack('<' + 'H' * (header // 2), volume[:header])) & 65535 or sha(volume) in visited:
                continue
            visited.add(sha(volume))
            offset = (header + 7) & ~7
            while offset + 24 <= len(volume):
                entry = volume[offset:offset + 24]
                if entry == b'\xff' * 24:
                    break
                length, fixed = int.from_bytes(entry[20:23], 'little'), 24
                if length == 0xffffff:
                    length, fixed = struct.unpack_from('<Q', volume, offset + 24)[0], 32
                require(length >= fixed and offset + length <= len(volume), 'Invalid FFS bounds')
                body = volume[offset + fixed:offset + length]
                if uuid.UUID(bytes_le=entry[:16]) == LOADER_GUID:
                    found.append(body)
                if entry[18] != 0xf0:
                    sections(body, depth + 1)
                offset = (offset + length + 7) & ~7

    scan(fd)
    require(len(found) == 1, 'Require one actual DarwinBoot FFS module')
    blob, offset, images = found[0], 0, []
    while offset + 4 <= len(blob):
        size, kind = int.from_bytes(blob[offset:offset + 3], 'little'), blob[offset + 3]
        require(4 <= size <= len(blob) - offset, 'Invalid PE section bounds')
        if kind == 0x10:
            images.append(blob[offset + 4:offset + size])
        offset = (offset + size + 3) & ~3
    require(len(images) == 1 and sha(images[0]) == LOADER_PE, 'DarwinBoot PE differs from the reviewed LZMA/handoff build')
    return sha(images[0])


def verify_loader(boot, archive_path):
    require(sha(boot) == LOADER_BOOT and sha(archive_path.read_bytes()) == LOADER_ZIP, 'Require the exact recorded handoff/LZMA loader carrier and original artifact')
    require(boot[:8] == b'ANDROID!' and struct.unpack_from('<I', boot, 40)[0] == 4, 'Wrong loader boot header')
    size = struct.unpack_from('<I', boot, 8)[0]
    decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
    payload = decoder.decompress(boot[4096:4096 + size])
    require(decoder.eof and len(payload) == 192 + 0x300000, 'Invalid boot carrier gzip/FD layout')
    fd = payload[192:]
    require(sha(fd) == LOADER_FD, 'Signed carrier embedded FD differs')
    with zipfile.ZipFile(archive_path) as archive:
        require(archive.read('SILICIUM_UEFI.fd') == fd, 'Archived FD and signed carrier differ')
        report = json_bytes(archive.read('firmware-result.json'))
        require(report['device_source_commit'] == LOADER_HEAD and report['framework_commit'] == LOCK['unixnu']['commit'] and
                report['sha256'] == sha(fd) and report['lzma_kernel']['enabled'] is True and
                report['lzma_kernel']['darwinboot_patched'] is True, 'Loader build report/source identity differs')
        require(("checked out '" + LOCK['darwin_boot']['commit'] + "'") in archive.read('darwinboot-submodule.log').decode(),
                'Loader artifact lacks the exact DarwinBoot checkout record')
        log = archive.read('logs/firmware.log').decode()
        require(all(fragment in log for fragment in ('DarwinBoot.c', 'DarwinBoot.dll', 'DarwinBoot.efi', 'LzmaCustomDecompressLib')),
                'Loader compile/link/LZMA build trace is missing')
        require(str(LOADER_GUID).upper() + ' DarwinBoot' in archive.read('Guid.xref').decode(), 'Loader GUID map differs')
    return {'run_id': 43729209898, 'artifact_id': 14167867119, 'device_commit': LOADER_HEAD,
            'boot_sha256': sha(boot), 'fd_sha256': sha(fd), 'pe_sha256': darwinboot_pe(fd),
            'darwinboot_revision': LOCK['darwin_boot']['commit'], 'lzma_kernel_supported': True,
            'source_and_build_lineage_verified': True, 'independently_reproduced': False,
            'runtime_root_policy_handoff_tested': False, 'cloud_per_module_source_attestation': False}


def checked_inputs(manifest_path, expected):
    raw = manifest_path.read_bytes()
    require(re.fullmatch(r'[0-9a-f]{64}', expected) and sha(raw) == expected, 'Manifest SHA-256 mismatch')
    manifest = json_bytes(raw)
    require(set(manifest) == {'schema_version', 'profile', 'framework', 'boot_args', 'files'}, 'Unknown or missing manifest fields')
    require(manifest.get('schema_version') == 1 and manifest.get('profile') == LOCK['os_profile'], 'Wrong manifest schema/profile')
    arguments = validate_args(manifest.get('boot_args'))
    supplied = set(manifest.get('files', {}))
    expected_inputs = INPUTS | CDC_INPUTS if 'tb710fu-cdc' in arguments or supplied & CDC_INPUTS else INPUTS
    require(supplied == expected_inputs,
            'Manifest must name exactly these files' +
            (' for CDC transport' if expected_inputs != INPUTS else '') + ': ' + ', '.join(sorted(expected_inputs)))
    paths, records = {}, {}
    for name, item in manifest['files'].items():
        require(set(item) == {'path', 'sha256'} and re.fullmatch(r'[0-9a-f]{64}', item['sha256']), 'Missing explicit input path/hash: ' + name)
        path = Path(item['path'])
        path = path if path.is_absolute() else manifest_path.parent / path
        path = path.resolve(strict=True)
        require(path.is_file(), 'Input is not a regular file: ' + name)
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        require(digest == item['sha256'], 'Input SHA-256 mismatch: ' + name)
        paths[name] = path
        records[name] = {'path': str(path), 'sha256': digest, 'bytes': path.stat().st_size}
    require(records['source_vendor']['sha256'] == BASELINE and records['source_vbmeta']['sha256'] == BASE_VBMETA,
            'Require the exact reviewed ADT-correct signed vendor/vbmeta baseline pair')
    return manifest, paths, records


def package(manifest_path, manifest_sha, output):
    manifest, paths, inputs = checked_inputs(manifest_path, manifest_sha)
    framework = Path(manifest['framework'])
    framework = (framework if framework.is_absolute() else manifest_path.parent / framework).resolve(strict=True)
    serializer, profile, parser, source_hashes = pinned_tools(framework)
    report_names = ('kernel_report', 'kernel_map', 'build_report', 'integration_report', 'source_storage_report')
    raw_reports = {name: paths[name].read_bytes() for name in report_names}
    official, pairing, build, integration, archive_report = [json_bytes(raw_reports[name]) for name in report_names]
    require(build.get('source_storage_linked') is True and
            build.get('storage_integration_report_sha256') == sha(raw_reports['integration_report']),
            'Build result does not bind this exact source-storage integration report')
    require(integration.get('archive_compile_report_sha256') == sha(raw_reports['source_storage_report']),
            'Integration report does not bind this exact archive compile report')
    kernel, policies, storage = [paths[name].read_bytes() for name in ('kernel', 'root_policies', 'source_storage_archive')]
    verified = verify_prelink(kernel, official, pairing, build, integration, profile)
    integrated = verify_integration(integration, archive_report, storage, policies, official['inputs']['native_sha256'])
    cdc = None
    if CDC_INPUTS <= set(paths):
        cdc = verify_usb_cdc(paths['usb_cdc_integration_report'].read_bytes(),
                             paths['usb_cdc_report'].read_bytes(), paths['usb_cdc_archive'].read_bytes(),
                             kernel, official, build, integration)
    loader = verify_loader(paths['loader_boot'].read_bytes(), paths['loader_artifact'])
    require(not output.exists(), 'Output directory exists; preserve earlier evidence')
    original = paths['source_vendor'].read_bytes()
    require(original[:8] == b'VNDRBOOT' and original[-64:-60] == b'AVBf', 'Unexpected signed vendor_boot container')
    page, length = struct.unpack_from('<I', original, 12)[0], struct.unpack_from('<I', original, 24)[0]
    body = struct.unpack_from('>Q', original, len(original) - 64 + 12)[0]
    require(page == 4096 and page + length <= body < len(original), 'Invalid vendor_boot original image bounds')
    with tempfile.TemporaryDirectory(prefix='tb710fu-source-storage-') as temporary:
        work = Path(temporary)
        policy_report = verify_policies(policies, profile, archive_report, work)
        run = subprocess.run([sys.executable, str(parser), str(paths['kernel'])], capture_output=True, text=True)
        require(run.returncode == 0, 'Pinned production DarwinBoot parser rejected this exact prelinked kernel: ' + run.stderr[-2000:])
        payload = work / 'payload.img'
        payload.write_bytes(original[page:page + length])

        def read(name):
            return subprocess.check_output(['mtype', '-i', str(payload), '::/EFI/UniXNU/' + name])

        def listing():
            return set(subprocess.check_output(['mdir', '-/', '-b', '-i', str(payload), '::'], text=True).splitlines())

        expected_listing = {'::/EFI/', '::/EFI/UniXNU/', '::/EFI/UniXNU/Payloads/', '::/EFI/UniXNU/unixnu.toml',
                            '::/EFI/UniXNU/Payloads/kernel.macho', '::/EFI/UniXNU/Payloads/kernel.macho.adt'}
        require(listing() == expected_listing, 'Unexpected files in the fixed baseline payload')
        before_adt, before_config = read('Payloads/kernel.macho.adt'), read('unixnu.toml')
        tree, adt_report = extend_adt(before_adt, serializer, profile)
        config = update_config(before_config, manifest['boot_args'])
        options = {'id': lzma.FILTER_LZMA1, 'dict_size': 8 << 20, 'lc': 3, 'lp': 0, 'pb': 2, 'preset': 6}
        encoded = lzma.compress(kernel, format=lzma.FORMAT_ALONE, filters=[options])
        encoded = encoded[:5] + struct.pack('<Q', len(kernel)) + encoded[13:]
        decoder = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=[options])
        require(decoder.decompress(encoded[13:]) == kernel and decoder.eof and not decoder.unused_data,
                'LZMA raw-stream roundtrip/bounds failed')
        expected_files = {'Payloads/kernel.macho': encoded, 'Payloads/kernel.macho.adt': tree,
                          'Payloads/kernel.macho.vroot': policies, 'unixnu.toml': config}
        for index, (name, content) in enumerate(expected_files.items()):
            local = work / f'payload-input-{index}'
            local.write_bytes(content)
            subprocess.run(['mcopy', '-o', '-i', str(payload), str(local), '::/EFI/UniXNU/' + name], check=True)
        require(listing() == expected_listing | {'::/EFI/UniXNU/Payloads/kernel.macho.vroot'}, 'Unexpected output payload files')
        readbacks = {name: read(name) for name in expected_files}
        require(readbacks == expected_files, 'FAT kernel/TOML/ADT/policy byte readback failed')
        require(tomllib.loads(readbacks['unixnu.toml'].decode())['entry'][0]['args'] == manifest['boot_args'], 'Root args parsed readback failed')
        parse_bundle(readbacks['Payloads/kernel.macho.vroot'])
        require(dict(unique_child(unique_child(read_nodes(readbacks['Payloads/kernel.macho.adt']), 'chosen'), 'memory-map')[0])['RootPolicies'] == bytes(16),
                'Readback ADT RootPolicies handoff is not zero16')
        fat = payload.read_bytes()
        fat_geometry = geometry(fat)
        require(len(fat) == length and fat_geometry['mountable'] and
                fat_geometry == geometry(original[page:page + length]), 'Output payload FAT geometry/length changed or failed')
        image = bytearray(original[:body])
        image[page:page + length] = fat
        require(image[:page] == original[:page] and image[page + length:] == original[page + length:body],
                'Image changed outside vendor ramdisk')
        report = {'schema_version': 1, 'stage': 'unsigned-source-storage-vendor-boot', 'profile': LOCK['os_profile'],
                  'manifest_sha256': manifest_sha, 'inputs': inputs, 'fixed_source_sha256': source_hashes,
                  'integrated_native': integrated, 'prelink': verified, 'root_policies': policy_report,
                  'loader': loader, 'boot_args': manifest['boot_args'], 'adt': adt_report,
                  'kernel_lzma': {'bytes': len(encoded), 'sha256': sha(encoded), 'raw_roundtrip': 'PASS'},
                  'payload': {'bytes': len(fat), 'sha256': sha(fat), 'geometry': fat_geometry,
                              'file_readback_sha256': {name: sha(data) for name, data in readbacks.items()}},
                  'production_loader_parser': {'result': 'PASS', 'stdout': run.stdout.strip()},
                  'unsigned_bytes': len(image), 'unsigned_sha256': sha(image), 'outer_vendor_fields_preserved': True,
                  'signing_performed': False, 'private_key_read': False, 'device_operations': False,
                  'hardware_boot_verified': False, 'runtime_qualified': False}
        if cdc is not None:
            report['usb_cdc_transport'] = cdc
        # Validation, actual parser and readback all finish before any output
        # directory exists. Intermediates stay in the disposable local directory.
        output.mkdir(parents=True, exist_ok=False)
        target = output / 'vendor_boot.unsigned.img'
        with target.open('xb') as stream:
            stream.write(image)
        with target.open('rb') as stream:
            require(hashlib.file_digest(stream, 'sha256').hexdigest() == report['unsigned_sha256'], 'Final unsigned file readback failed')
        (output / 'packaging-report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--manifest-sha256', required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        report = package(args.manifest.resolve(), args.manifest_sha256, args.output_dir.resolve())
    except (ValueError, KeyError, OSError, struct.error, subprocess.CalledProcessError, lzma.LZMAError, zipfile.BadZipFile) as error:
        parser.exit(1, 'Refused source-storage packaging: ' + str(error) + '\n')
    print(json.dumps({key: report[key] for key in ('stage', 'unsigned_bytes', 'unsigned_sha256', 'signing_performed', 'device_operations')}, indent=2))


if __name__ == '__main__':
    main()
