"""Run the pinned official driver with explicit macOS selection and strict stage checks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['configure', 'native', 'prelink', 'collect'])
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
        command(driver + ['status', '--json'] + common, source, output / 'status.json')
        print(f'Configured {PROFILE}; output {work}', flush=True)
        return
    if args.mode == 'native':
        command(driver + ['fetch'] + common, source, output / 'fetch.log')
        command(driver + ['status', '--json'] + common, source, output / 'status.json')
        command(driver + ['build', 'kernel', '--skip-prelink'] + common, source, output / 'build-native.log')
    else:
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
