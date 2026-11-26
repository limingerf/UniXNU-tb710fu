#!/usr/bin/env python3
"""Compile and run the host-side checks for package/Tb710fuWifi.

The portable C half (board/TLMM/iATU/BAR planning, MHI state machine and ring builders, firmware manifest, QRTR/QMI/
HTC/WMI/DP encoders) is compiled with the host's clang or gcc against package/Tb710fuWifi/tests/host_test.c. The
kernel-only files (Tb710fuWifiPcie.cpp, Tb710fuWifiDriver.cpp, Tb710fuWifiNet.cpp, Tb710fuWifiFirmware.c) need XNU/
IOKit context and are covered by scripts/build-tb710fu-wifi.py instead.

Usage:
    python3 scripts/check-tb710fu-wifi-host.py [--cc <compiler>] [--json]

Exit status 0 only when the test binary prints PASS (all checks), the strictly-warning build succeeds, and every
portable header compiles on its own. No compiler is a failure, not a skip: the point is that this ran.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'package/Tb710fuWifi'
TEST = PACKAGE / 'tests/host_test.c'
SOURCES = [PACKAGE / name for name in (
    'Tb710fuWifiBoard.c', 'Tb710fuWifiMhi.c', 'Tb710fuWifiManifest.c', 'Tb710fuWifiProto.c')]
HEADERS = [PACKAGE / name for name in (
    'Tb710fuWifiRegs.h', 'Tb710fuWifiBoard.h', 'Tb710fuWifiMhi.h', 'Tb710fuWifiManifest.h',
    'Tb710fuWifiProto.h', 'Tb710fuWifiFirmware.h', 'Tb710fuWifiPcieTables.h')]
CFLAGS = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror']


def find_compiler(explicit):
    candidates = []
    if explicit:
        candidates.append([explicit])
    if os.environ.get('CC'):
        candidates.append([os.environ['CC']])
    if shutil.which('xcrun'):
        candidates.append(['xcrun', 'clang'])
    for name in ('clang', 'gcc', 'cc'):
        found = shutil.which(name)
        if found:
            candidates.append([found])
    for candidate in candidates:
        try:
            probe = subprocess.run(candidate + ['--version'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True)
        except OSError:
            continue
        if probe.returncode == 0:
            return candidate
    return None


def run(command, **kwargs):
    return subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kwargs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', help='compiler command (default: $CC, xcrun clang, clang, gcc, cc)')
    parser.add_argument('--json', action='store_true', help='print a JSON report instead of the human summary')
    args = parser.parse_args()

    compiler = find_compiler(args.cc)
    if not compiler:
        print('FAIL: no C compiler found ($CC, xcrun clang, clang, gcc, cc)', file=sys.stderr)
        return 2
    work = Path(tempfile.mkdtemp(prefix='tb710fu-wifi-host-'))
    include = ['-I', str(PACKAGE)]
    report = {'compiler': ' '.join(compiler), 'checks': [], 'result': 'FAIL'}

    failures = []
    header_runs = []
    for header in HEADERS:
        source = work / (header.stem + '_tu.c')
        source.write_text(f'#include "{header.name}"\nint main(void) {{ return 0; }}\n')
        binary = work / (header.stem + '_tu')
        result = run(compiler + CFLAGS + include + [str(source), '-o', str(binary)])
        header_runs.append({'header': header.name, 'ok': result.returncode == 0})
        if result.returncode:
            failures.append(f'{header.name} is not self-contained:\n{result.stdout[-4000:]}')

    binary = work / 'host_test'
    command = compiler + CFLAGS + include + [str(source) for source in SOURCES] + [str(TEST), '-o', str(binary)]
    build = run(command)
    report['compile'] = {'command': command, 'ok': build.returncode == 0, 'log': build.stdout[-4000:]}
    if build.returncode:
        failures.append(f'compile failed:\n{build.stdout[-6000:]}')
    else:
        executed = run([str(binary)])
        report['run'] = {'command': [str(binary)], 'ok': executed.returncode == 0, 'log': executed.stdout}
        if executed.returncode != 0 or 'PASS' not in executed.stdout:
            failures.append(f'test binary failed:\n{executed.stdout[-6000:]}')

    report['headers'] = header_runs
    report['failures'] = failures
    report['result'] = 'PASS' if not failures else 'FAIL'
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        for header in header_runs:
            print(f'header {header["header"]}: {"ok" if header["ok"] else "FAIL"}')
        if failures:
            print('\n'.join(['FAIL'] + failures))
        else:
            print(report['run']['log'].strip())
            print(f'PASS ({report["compiler"]})')
    return 0 if not failures else 1


if __name__ == '__main__':
    sys.exit(main())
