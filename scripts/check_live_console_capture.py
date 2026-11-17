#!/usr/bin/env python3
"""Reconstruct actual native console patches and test their capture branches."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TARGET = 'osfmk/console/serial_console.c'


def run(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.stdout:
        print(result.stdout, end='')
    if result.stderr:
        print(result.stderr, end='')
    result.check_returncode()
    return result


def body(source, signature):
    if source.count(signature + '\n{') != 1:
        raise SystemExit('Ambiguous production function: ' + signature)
    start = source.index(signature + '\n{')
    pos = start + len(signature) + 2
    depth = 1
    while depth:
        if source[pos] == '{':
            depth += 1
        elif source[pos] == '}':
            depth -= 1
        pos += 1
    return source[start:pos] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--framework', type=Path, default=ROOT / '.work/UniXNU')
    parser.add_argument('--xnu', type=Path, default=ROOT / '.work/xnu-patch-lf-clean')
    parser.add_argument('--output-dir', type=Path, default=ROOT / '.work/live-console-capture-check')
    args = parser.parse_args()
    framework = args.framework.resolve()
    compiler = shutil.which(args.cc)
    if not compiler:
        raise SystemExit('Missing host compiler: ' + args.cc)
    source = subprocess.check_output(['git', '-C', str(args.xnu), 'show',
                                     'ac9718fb1af618d5ce8678d0dc6e8a58f252216f:' + TARGET])
    patches = [framework / 'patches/xnu-12377/zzz-boot-progress.patch',
               framework / 'profiles/os/macos-27.3-26C5035r/patches/xnu-12377/macos-verbose-text-console.patch',
               ROOT / 'patches/xnu/0004-tb710fu-ramoops-console.patch',
               ROOT / 'patches/xnu/0010-tb710fu-cdc-kernel-log-tap.patch',
               ROOT / 'patches/xnu/0014-tb710fu-native-live-log.patch']
    with tempfile.TemporaryDirectory(prefix='tb710fu-console-') as temporary:
        scratch = Path(temporary)
        path = scratch / TARGET
        path.parent.mkdir(parents=True)
        path.write_bytes(source)
        run(['git', 'init', '--quiet', str(scratch)])
        for patch in patches:
            run(['git', '-C', str(scratch), 'apply', '--check', '--include=' + TARGET, str(patch)])
            run(['git', '-C', str(scratch), 'apply', '--include=' + TARGET, str(patch)])
        actual = path.read_text()
        signatures = ['static inline void\n_cnputc(char c, bool poll)',
                      'static void\ntb710fu_live_capture_disabled(const char *text, int size)',
                      'static inline void\n_cnputs(char *c, int size, bool poll)',
                      'void\nconsole_write(char *str, int size)',
                      'void\nconsole_write_unbuffered(char c)']
        extracted = '\n'.join(body(actual, name) for name in signatures)
        (scratch / 'production-console.inc').write_text(extracted)
        binary = scratch / 'console-capture-test'
        command = [compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                   '-I', str(scratch), str(ROOT / 'tests/live_console_capture_test.c'), '-o', str(binary)]
        run(command)
        result = run([str(binary)])
        output = args.output_dir.resolve()
        output.mkdir(parents=True, exist_ok=True)
        report = {'result': 'PASS', 'cases': 10, 'source_xnu': 'ac9718fb1af618d5ce8678d0dc6e8a58f252216f',
                  'patched_source_sha256': hashlib.sha256(actual.encode()).hexdigest(),
                  'extracted_production_sha256': hashlib.sha256(extracted.encode()).hexdigest(),
                  'patches_sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in patches},
                  'test_sha256': hashlib.sha256((ROOT / 'tests/live_console_capture_test.c').read_bytes()).hexdigest(),
                  'sanitizers': ['address', 'undefined'], 'output': result.stdout.strip(),
                  'limits': 'Actual production function bodies with host console-service stubs; no native or hardware qualification.',
                  'device_operations': False}
        (output / 'capture-check.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
