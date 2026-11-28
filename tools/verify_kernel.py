"""Verify native MH_EXECUTE or strictly mapped prelinked MH_FILESET artifacts."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / 'upstream.lock.json').read_text())


def require(condition, message):
    if not condition:
        raise ValueError(message)


def verify_image(data, stage, report, lock=LOCK):
    require(len(data) >= 32, 'Truncated Mach-O header')
    require(len(data) <= 256 * 1024 * 1024, 'Kernel exceeds the production loader file limit')
    magic, cpu, subtype, kind, count, command_bytes, flags, reserved = struct.unpack_from('<8I', data)
    expected = 2 if stage == 'native-only' else 12
    require(magic == 0xfeedfacf and cpu == 0x100000c and subtype & 0xffffff == 2, 'Expected an ARM64e Mach-O image')
    require(kind == expected, f'Expected Mach-O file type {expected}, got {kind}')
    require(32 + command_bytes <= len(data), 'Mach-O load commands exceed image bounds')
    entries, position = {}, 32
    for _ in range(count):
        require(position + 8 <= 32 + command_bytes, 'Truncated Mach-O load command')
        command, size = struct.unpack_from('<II', data, position)
        require(size >= 8 and position + size <= 32 + command_bytes, 'Invalid Mach-O load command size')
        if command == 0x19:
            require(size >= 72, 'Truncated LC_SEGMENT_64')
            file_offset, file_size = struct.unpack_from('<QQ', data, position + 40)
            require(file_size == 0 or file_offset + file_size <= len(data), 'Mach-O segment exceeds file bounds')
        if command == 0x80000035:
            require(size >= 32, 'Truncated LC_FILESET_ENTRY')
            address, offset, name_offset, unused = struct.unpack_from('<QQII', data, position + 8)
            require(32 <= name_offset < size and offset + 32 <= len(data), 'Invalid fileset entry bounds')
            raw = data[position + name_offset:position + size]
            require(b'\0' in raw, 'Unterminated fileset identifier')
            identifier = raw.split(b'\0', 1)[0].decode('utf-8')
            require(identifier not in entries, 'Duplicate fileset identifier')
            entries[identifier] = offset
        position += size
    require(position == 32 + command_bytes, 'Inconsistent Mach-O command count')
    digest = hashlib.sha256(data).hexdigest()
    if stage == 'native-only':
        require(report.get('mode') == 'native-only' and report.get('prelink_skipped') is True, 'Missing official native-only record')
        require(report.get('native_sha256') == digest and report.get('candidate_sha256') == digest, 'Native record hash mismatch')
    else:
        require(report.get('format') == 'MH_FILESET', 'Missing official fileset record')
        require(report.get('mapping_complete') is True and report.get('candidate_written') is True, 'Prelink mapping is incomplete')
        require(report.get('allow_missing') is False and report.get('missing') == {} and report.get('stubbed') == {}, 'Unresolved or stubbed retained references are not accepted')
        require(report.get('stub_symbol') is None, 'A synthetic stub provider is not accepted')
        require(report.get('candidate_bytes') == len(data) and report.get('candidate_sha256') == digest, 'Fileset record size/hash mismatch')
        require(report['inputs']['kernelcache_sha256'] == lock['kernelcache_sha256'], 'Wrong macOS kernelcache input')
        require('com.apple.kernel' in entries, 'Missing native kernel fileset entry')
        # The dependency closure includes codeless bundles. The official emitter
        # filters existing executable entries by that closure; it does not create
        # Mach-O headers for bundles without an executable.
        require(set(entries) <= set(report['closure']) | {'com.apple.kernel'}, 'Unexpected executable fileset entry outside the retained closure')
        native_header = struct.unpack_from('<4I', data, entries['com.apple.kernel'])
        require(native_header == (magic, cpu, subtype, 2), 'Embedded native MH_EXECUTE header does not match the fileset architecture')
    return {'stage': stage, 'mach_o_filetype': kind, 'cpu_subtype': hex(subtype),
            'bytes': len(data), 'sha256': digest, 'fileset_entries': len(entries),
            'mapping_complete': report.get('mapping_complete') if stage != 'native-only' else None,
            'hardware_boot_verified': False, 'runtime_qualified': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-id', required=True, type=int)
    args = parser.parse_args()
    import github_admin as gh
    token = gh.credential(None)
    base = 'repos/' + os.environ['GITHUB_REPOSITORY']
    run = gh.api(token, 'GET', f'{base}/actions/runs/{args.run_id}')
    require(run['status'] == 'completed' and run['conclusion'] == 'success',
            'Source run is incomplete or unsuccessful')
    source_path = run['path']
    require(source_path in ('.github/workflows/macos-kernel.yml',
                            '.github/workflows/macos-prelink.yml'),
            'Wrong source workflow')
    artifacts = gh.api(token, 'GET', f'{base}/actions/runs/{args.run_id}/artifacts')['artifacts']
    if source_path == '.github/workflows/macos-kernel.yml':
        jobs = gh.api(token, 'GET', f'{base}/actions/runs/{args.run_id}/jobs')['jobs']
        require(any(step['name'] == 'Build and verify the native arm64e kernel' and
                    step['conclusion'] == 'success' for job in jobs for step in job['steps']),
                'Source run did not complete native compilation')
        expected_artifact_name = f'tb710fu-prelink-{args.run_id}'
    else:
        expected_artifact_name = f'tb710fu-strict-prelink-{args.run_id}'
    artifact = next((item for item in artifacts if item['name'] == expected_artifact_name and
                     not item['expired']), None)
    require(artifact is not None, 'Missing or expired source prelink artifact')
    archive = ROOT / '.work/prelinked-input.zip'
    gh.download(token, f'{base}/actions/artifacts/{artifact["id"]}/zip', archive)
    output = ROOT / '.work/verified-prelink'
    output.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as bundle:
        official = json.loads(bundle.read('kernel/kernel.json'))
        source = json.loads(bundle.read('build-result.json'))
        status = json.loads(bundle.read('status.json'))
        xnu = json.loads(bundle.read('xnu-inputs.json'))
        require(source['source_commit'] == LOCK['unixnu']['commit'] and source['profile'] == LOCK['os_profile'], 'Native source/profile identity mismatch')
        require(xnu['commit'] == LOCK['xnu_commit'], 'Unexpected XNU source input')
        require(source.get('stage') == 'prelinked' and
                source.get('sha256') == official.get('candidate_sha256') and
                source.get('bytes') == official.get('candidate_bytes'),
                'Build result and official prelinked candidate metadata differ')
        require(status['board']['board']['os_profile'] == LOCK['os_profile'] and status['board']['os']['arch'] == 'arm64e', 'Source status is not the pinned macOS arm64e profile')
        data = bundle.read('kernel/kernel.macho')
        verified = verify_image(data, 'prelinked', official)
        roots = status['board']['kernel']['prelink']['roots']
        require({root for root in roots if not root.startswith('!')} <= set(official['closure']), 'A required profile root is not retained')
        require(not {root[1:] for root in roots if root.startswith('!')} & set(official['closure']), 'An excluded profile root was retained')
        identifier, address = status['board']['kernel']['prelink']['early_security_init'].split('=', 1)
        require(official['early_security_init'].get(identifier) == address, 'AMFI early initializer differs from the profile')
        pairing = json.loads(bundle.read('kernel/kernel.map.json'))
        require(pairing.get('method') == 'symbol-names' and pairing.get('inputs') == official['inputs'], 'Wrong prelink map method or input hashes')
        require(pairing.get('matcher_version') == official['matcher_version'] and pairing.get('matcher_signature') == official['matcher_signature'], 'Prelink matcher identity differs between reports')
        require(pairing.get('conflicts') == [], 'Prelink symbol-map conflicts remain')
        require(pairing.get('stats', {}).get('conflicts') == {}, 'Prelink symbol-map conflict statistics remain')
        native_source_run_id = None
        if source_path == '.github/workflows/macos-prelink.yml':
            native_input = json.loads(bundle.read('native-input.json'))
            require(native_input.get('configuration_and_patches_match') is True and
                    native_input.get('native_recompiled') is False,
                    'Strict prelink did not preserve the verified native inputs')
            native_source_run_id = int(native_input['native_run_id'])
            native_run = gh.api(token, 'GET', f'{base}/actions/runs/{native_source_run_id}')
            require(native_run['status'] == 'completed' and native_run['conclusion'] == 'success' and
                    native_run['path'] == '.github/workflows/macos-kernel.yml' and
                    native_run['head_sha'] == native_input['native_run_revision'],
                    'Strict prelink native source run identity does not match')
            native_artifacts = gh.api(
                token, 'GET', f'{base}/actions/runs/{native_source_run_id}/artifacts')['artifacts']
            native_artifact = next((item for item in native_artifacts
                                    if item['id'] == native_input['native_artifact_id'] and
                                    item['name'] == f'tb710fu-native-{native_source_run_id}' and
                                    not item['expired']), None)
            require(native_artifact is not None,
                    'Strict prelink native artifact is missing, expired or mismatched')
            require(native_input['native_sha256'] == official['inputs']['native_sha256'],
                    'Strict prelink native image hash differs from the fileset input')
        (output / 'kernel.macho').write_bytes(data)
        (output / 'kernel.json').write_bytes(bundle.read('kernel/kernel.json'))
        (output / 'source-status.json').write_bytes(bundle.read('status.json'))
        if 'unixnu.lock' in bundle.namelist():
            (output / 'unixnu.lock').write_bytes(bundle.read('unixnu.lock'))
    verified.update({'source_run_id': args.run_id, 'source_run_revision': run['head_sha'],
                     'source_artifact_id': artifact['id'], 'retained_kexts': len(official['closure']),
                     'native_input_sha256': official['inputs']['native_sha256'],
                     'candidate_sha256': official['candidate_sha256'],
                     'missing': {}, 'stubbed': {}, 'allow_missing': False,
                     'native_recompiled': False})
    if native_source_run_id is not None:
        verified['native_source_run_id'] = native_source_run_id
    (output / 'verification.json').write_text(json.dumps(verified, indent=2) + '\n')
    print(json.dumps(verified, indent=2))
    summary = os.environ.get('GITHUB_STEP_SUMMARY')
    if summary:
        with open(summary, 'a') as stream:
            stream.write(f'### Strict macOS fileset verification passed\n\nSHA-256: `{verified["sha256"]}`\n\nMissing: 0; stubbed: 0; runtime/hardware qualification pending. No native recompilation.\n')


if __name__ == '__main__':
    main()
