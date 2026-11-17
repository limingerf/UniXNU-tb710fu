#!/usr/bin/env python3
"""Apply real ramoops patches in isolation and test the production snapshot code.

The XNU source cache and official recipes are never changed. This host check is
not native target compilation, persistent-RAM retention or hardware acceptance.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
FILES = ('osfmk/kern/tb710fu_ramoops.c', 'osfmk/kern/tb710fu_ramoops.h')
PATCHES = (
    'patches/xnu/0004-tb710fu-ramoops-console.patch',
    'patches/xnu/0008-tb710fu-ramoops-in-window.patch',
    'patches/xnu/0013-tb710fu-ramoops-snapshot.patch',
)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, **kwargs):
    result = subprocess.run(command, text=True, capture_output=True, **kwargs)
    if result.stdout:
        print(result.stdout, end='')
    if result.stderr:
        print(result.stderr, end='')
    result.check_returncode()
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--output-dir', type=Path,
                        default=ROOT / '.work/ramoops-snapshot-check')
    args = parser.parse_args()
    compiler = shutil.which(args.cc)
    if not compiler:
        raise SystemExit('Missing host C compiler: ' + args.cc)
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='tb710fu-ramoops-') as temporary:
        scratch = Path(temporary)
        run(['git', 'init', '--quiet', str(scratch)])
        include = ['--include=' + path for path in FILES]
        hashes = {}
        for patch in PATCHES:
            # Applying the actual 0004 additions reconstructs the exact source;
            # include filters omit unrelated hunks that need a complete XNU tree.
            run(['git', '-C', str(scratch), 'apply', '--check', *include, str(ROOT / patch)])
            run(['git', '-C', str(scratch), 'apply', *include, str(ROOT / patch)])
            for name in FILES:
                hashes.setdefault(patch, {})[name] = sha(scratch / name)
        executable = scratch / 'ramoops-snapshot-test'
        command = [compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   '-fno-sanitize-recover=all', '-I', str(scratch),
                   '-I', str(scratch / 'osfmk'),
                   str(ROOT / 'tests/ramoops_snapshot_test.c'), '-o', str(executable)]
        run(command)
        result = run([str(executable)])
        report = {
            'schema_version': 1,
            'stage': 'production-ramoops-snapshot-host-check',
            'result': 'PASS',
            'cases': 10,
            'sanitizers': ['address', 'undefined'],
            'compiler': run([compiler, '--version']).stdout.splitlines()[0],
            'source_patch_sha256': {name: sha(ROOT / name) for name in PATCHES},
            'source_after_patch_sha256': hashes,
            'test_sha256': sha(ROOT / 'tests/ramoops_snapshot_test.c'),
            'checker_sha256': sha(Path(__file__)),
            'output': result.stdout.strip(),
            'limits': 'Host production-code check; native target, live framebuffer and retention unverified.',
            'device_operations': False,
        }
        (output / 'check-report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
