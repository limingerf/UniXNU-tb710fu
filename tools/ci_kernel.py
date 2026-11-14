"""Run the pinned official driver with explicit macOS selection and strict stage checks."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / 'upstream.lock.json').read_text())
PROFILE = LOCK['os_profile']


def command(arguments, cwd, logfile=None):
    print('+ ' + ' '.join(map(str, arguments)), flush=True)
    if logfile:
        with logfile.open('w') as stream:
            process = subprocess.Popen(list(map(str, arguments)), cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            for line in process.stdout:
                print(line, end='', flush=True)
                stream.write(line)
            returncode = process.wait()
            if returncode:
                raise SystemExit(returncode)
    else:
        subprocess.run(list(map(str, arguments)), cwd=cwd, check=True)


def status_snapshot(driver, common, source, destination):
    result = subprocess.run(driver + ['status', '--json'] + common, cwd=source, text=True, capture_output=True)
    if result.stderr:
        print(result.stderr, end='', file=sys.stderr)
    if result.returncode:
        print(result.stdout, end='')
        raise SystemExit(result.returncode)
    snapshot = json.loads(result.stdout)
    destination.write_text(json.dumps(snapshot, indent=2) + '\n')
    print('Resolved status written to ' + str(destination), flush=True)
    return snapshot


def verify_prelink_revision(snapshot):
    selected = next(module for module in snapshot['modules'] if module['name'] == 'XnuPrelink')
    if selected['revision'] != LOCK['xnu_prelink']['commit']:
        raise SystemExit('Resolved XnuPrelink differs from the official gitlink pinned by this project')


def import_native_run(run_id, ctx, output):
    import github_admin as gh
    token = gh.credential(None)
    repository = os.environ['GITHUB_REPOSITORY']
    base = 'repos/' + repository
    run = gh.api(token, 'GET', f'{base}/actions/runs/{run_id}')
    if run['conclusion'] != 'success' or run['path'] != '.github/workflows/macos-kernel.yml':
        raise SystemExit('Native input must be a successful run of this repository native-kernel workflow')
    artifacts = gh.api(token, 'GET', f'{base}/actions/runs/{run_id}/artifacts')['artifacts']
    artifact = next((item for item in artifacts if item['name'] == f'tb710fu-native-{run_id}' and not item['expired']), None)
    if not artifact:
        raise SystemExit('The exact native input artifact is missing or expired')
    for name in ('board.toml', 'configs/tb710fu_macos_defconfig'):
        saved = gh.api(token, 'GET', f'{base}/contents/{name}?ref={run["head_sha"]}')
        if base64.b64decode(saved['content']) != (ROOT / name).read_bytes():
            raise SystemExit(f'Native artifact configuration differs: {name}; rebuild native first')
    archive = ROOT / '.work/native-input.zip'
    gh.download(token, f'{base}/actions/artifacts/{artifact["id"]}/zip', archive)
    with zipfile.ZipFile(archive) as bundle:
        result = json.loads(bundle.read('build-result.json'))
        official = json.loads(bundle.read('kernel/kernel.json'))
        previous = json.loads(bundle.read('status.json'))
        if previous['board'] != ctx.board:
            raise SystemExit('Resolved native profile/board differs; rebuild native first')
        if result['stage'] != 'native-only' or result['source_commit'] != LOCK['unixnu']['commit'] or result['profile'] != PROFILE:
            raise SystemExit('Native artifact stage or source identity differs')
        member = bundle.getinfo('kernel/kernel.macho')
        if member.file_size > 512 * 1024 * 1024:
            raise SystemExit('Native artifact exceeds the expected kernel size range')
        data = bundle.read(member)
        digest = hashlib.sha256(data).hexdigest()
        if digest != result['sha256'] or digest != official['native_sha256'] or official['mode'] != 'native-only':
            raise SystemExit('Native artifact hash or official native-only record mismatch')
        from unixnu import patches
        expected = [hashlib.sha256(path.read_bytes()).hexdigest() for path in patches.plan(ctx, ctx.modules)]
        old = json.loads(bundle.read('xnu-inputs.json'))
        if old['commit'] != LOCK['xnu_commit'] or expected != [entry['sha256'] for entry in old['patches']]:
            raise SystemExit('Native artifact XNU patch inputs differ; rebuild native first')
    target = ctx.tree / ctx.board['kernel']['prelink']['kernel']
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    provenance = {'native_run_id': run_id, 'native_run_revision': run['head_sha'],
                  'native_artifact_id': artifact['id'], 'native_sha256': digest,
                  'configuration_and_patches_match': True, 'native_recompiled': False}
    (output / 'native-input.json').write_text(json.dumps(provenance, indent=2) + '\n')
    print('Restored exact native input ' + digest, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['configure', 'native', 'prelink', 'reuse-prelink', 'collect'])
    parser.add_argument('--native-run', type=int)
    parser.add_argument('--source', type=Path, default=ROOT / '.work/UniXNU')
    args = parser.parse_args()
    source = args.source.resolve()
    output = ROOT / '.work/artifacts'
    output.mkdir(parents=True, exist_ok=True)
    work = source / 'out/tb710fu' / PROFILE / 'tb710fu_macos'
    driver = [sys.executable, str(source / 'unixnu')]
    common = ['-D', str(ROOT), '--os', PROFILE]
    if args.mode == 'collect':
        resolved = ROOT / '.work/resolved-work.txt'
        if resolved.is_file():
            work = Path(resolved.read_text().strip())
        if (work / 'logs').is_dir():
            shutil.copytree(work / 'logs', output / 'logs', dirs_exist_ok=True)
        if (work / '.config').is_file():
            shutil.copy2(work / '.config', output / 'resolved.config')
        shutil.copy2(ROOT / 'upstream.lock.json', output / 'upstream.lock.json')
        for name in ('xnu-inputs.json', 'xnu-patches.list'):
            if (work / name).is_file():
                shutil.copy2(work / name, output / name)
        for name in ('unixnu.lock',):
            if (ROOT / name).is_file():
                shutil.copy2(ROOT / name, output / name)
        if (work / 'kernel').is_dir():
            shutil.copytree(work / 'kernel', output / 'kernel', dirs_exist_ok=True)
        return
    actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != LOCK['unixnu']['commit']:
        raise SystemExit('Official source revision differs from upstream.lock.json')
    command(driver + ['defconfig', 'tb710fu_macos_defconfig'] + common, source, output / 'defconfig.log')
    # Context is a documented public configuration entry point used by upstream CI.
    sys.path.insert(0, str(source / 'scripts'))
    from unixnu.cli import parser as cli_parser
    from unixnu.config import Context
    ctx = Context(cli_parser().parse_args(['status'] + common))
    if ctx.board['board']['os_profile'] != PROFILE or ctx.board['os']['arch'] != 'arm64e':
        raise SystemExit('macOS/arm64e profile resolution failed')
    if ctx.board['os']['xnu_revision'] != LOCK['xnu_commit']:
        raise SystemExit('Unexpected XNU source revision')
    work = ctx.work
    (ROOT / '.work/resolved-work.txt').write_text(str(work) + '\n')
    if args.mode == 'configure':
        status_snapshot(driver, common, source, output / 'status.json')
        print(f'Configured {PROFILE}; output {work}', flush=True)
        return
    if args.mode == 'native':
        command(driver + ['fetch'] + common, source, output / 'fetch.log')
        snapshot = status_snapshot(driver, common, source, output / 'status.json')
        verify_prelink_revision(snapshot)
        command(driver + ['build', 'kernel', '--skip-prelink'] + common, source, output / 'build-native.log')
    else:
        if args.mode == 'reuse-prelink':
            if not args.native_run:
                raise SystemExit('--native-run is required to reuse a successful native artifact')
            command(driver + ['fetch'] + common, source, output / 'fetch.log')
            command(driver + ['tree'] + common, source, output / 'tree.log')
            snapshot = status_snapshot(driver, common, source, output / 'status.json')
            verify_prelink_revision(snapshot)
            ctx = Context(cli_parser().parse_args(['status'] + common))
            from unixnu import feeds
            feeds.resolve(ctx)
            import_native_run(args.native_run, ctx, output)
        command(driver + ['build', 'prelink'] + common, source, output / 'build-prelink.log')
    kernel = work / 'kernel/kernel.macho'
    data = kernel.read_bytes()
    magic, cputype, subtype, filetype = struct.unpack_from('<IIII', data)
    if magic != 0xfeedfacf or cputype != 0x100000c or filetype != 2:
        raise SystemExit('Build did not produce an ARM64 MH_EXECUTE kernel')
    if subtype & 0x00ffffff != 2:
        raise SystemExit('Build did not produce the required arm64e subtype')
    native = args.mode == 'native'
    record = {'stage': 'native-only' if native else 'prelinked', 'profile': PROFILE,
              'source_commit': actual, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
              'cpu_subtype': hex(subtype), 'prelink_skipped': native, 'hardware_boot_verified': False}
    (output / 'build-result.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2), flush=True)
    shutil.copytree(work / 'kernel', output / 'kernel', dirs_exist_ok=True)
    summary = os.environ.get('GITHUB_STEP_SUMMARY')
    if summary:
        with open(summary, 'a') as stream:
            stream.write(f'### TB710FU {record["stage"]} kernel\n\nProfile: `{PROFILE}`\n\nSHA-256: `{record["sha256"]}`\n\nHardware boot is not yet qualified.\n')


if __name__ == '__main__':
    main()
