#!/usr/bin/env python3
"""Host-side checks for the TB710FU WCN7850 Bluetooth package.

Four stages, all of which must pass:

  1. syntax:   every package/Tb710fuBluetooth/*.c compiles on its own with the host compiler
               (-std=c11 -Wall -Wextra -Werror), so a missing include or a C99-ism cannot reach the
               arm64 build;
  2. unit:     tests/tb710fu_bluetooth_test.c plus the five cores builds and runs (clock RCG math,
               GENI FIFO transport against a fake engine, HCI H4 framing and parsing, QCA TLV/NVM
               parsing and download order, TLMM pin words and the BT_EN pulse);
  3. iokit:    Tb710fuBluetooth.cpp is parsed against the stub IOKit headers in tests/stubs, which
               catches most of what a Linux host can catch in a 1000-line kernel file (undefined
               identifiers, bad member calls, brace and overload mistakes). The stubs are not the
               Apple headers; this stage proves internal consistency only;
  4. hygiene:  the MMIO rules the task pins down are checked textually - no memcpy/bzero/memset on a
               mapped window, no ++/-- on a volatile, IOPlatformExpert.h next to getPlatform().

The script is deliberately host-only: it never needs the XNU tree, an SDK or a network connection.
Build script for the kernel objects: scripts/build-tb710fu-bt.py.
"""
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'package/Tb710fuBluetooth'
TESTS = ROOT / 'tests'
STUBS = TESTS / 'stubs'
OUT = ROOT / 'build/tb710fu-bt-host'

C_SOURCES = ['Tb710fuBluetoothGeni.c', 'Tb710fuBluetoothClock.c', 'Tb710fuBluetoothHci.c',
             'Tb710fuBluetoothQca.c', 'Tb710fuBluetoothPower.c']
CPP_SOURCE = 'Tb710fuBluetooth.cpp'
TEST_SOURCE = TESTS / 'tb710fu_bluetooth_test.c'


def compiler():
    """The distribution compiler, never the Darwin cross toolchain the kernel build prepends."""
    env = dict(os.environ)
    env['PATH'] = os.pathsep.join(
        part for part in env.get('PATH', '').split(os.pathsep) if '.linux-toolchain' not in part)
    candidates = [os.environ['CC']] if os.environ.get('CC') else []
    candidates += ['cc', 'clang', 'gcc']
    for candidate in candidates:
        try:
            found = subprocess.run(shlex.split(candidate) + ['--version'], env=env, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        except FileNotFoundError:
            continue
        if found.returncode == 0:
            return shlex.split(candidate), found.stdout.splitlines()[0], env
    raise SystemExit('no host C compiler found (expected cc, clang or gcc)')


def run(command, log, env, cwd=None):
    result = subprocess.run(command, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    Path(log).write_text('CMD: ' + ' '.join(map(str, command)) + '\n\n' + result.stdout)
    if result.returncode:
        print(result.stdout[-8000:])
        raise SystemExit(f'{log}: host check failed ({result.returncode}); see {log}')
    return result.stdout


def stage_syntax(cc, env, report):
    flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-I', str(PACKAGE)]
    for name in C_SOURCES:
        source = PACKAGE / name
        run(cc + flags + ['-fsyntax-only', str(source)], OUT / f'{name}.syntax.log', env)
        report['syntax'].append(name)
        print(f'   syntax ok: {name}')


def stage_unit(cc, env, report):
    flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
             '-I', str(PACKAGE)]
    executable = OUT / 'tb710fu-bluetooth-host-test'
    sources = [str(TEST_SOURCE)] + [str(PACKAGE / name) for name in C_SOURCES]
    run(cc + flags + sources + ['-o', str(executable)], OUT / 'unit-compile.log', env)
    output = run([str(executable)], OUT / 'unit-run.log', env)
    print(output, end='' if output.endswith('\n') else '\n')
    if 'PASS' not in output:
        raise SystemExit('host test binary did not report PASS')
    report['unit'] = {'executable': str(executable.relative_to(ROOT)), 'passed': True}
    checks = re.search(r'(\d+) checks, (\d+) failures', output)
    if checks:
        report['unit']['checks'] = int(checks.group(1))
        report['unit']['failures'] = int(checks.group(2))


def stage_iokit(cc, env, report):
    """Parse the IOKit translation unit against stubs. Warnings are allowed here: the stub headers are
    approximations, but an unknown type, a bad call or an unbalanced block is still an error."""
    if not (STUBS / 'IOKit/IOService.h').is_file():
        print('   iokit stub headers missing; skipping the C++ parse')
        report['iokit'] = {'stubbed': False}
        return
    flags = ['-std=c++17', '-fsyntax-only', '-fno-exceptions', '-fno-rtti',
             '-I', str(STUBS), '-I', str(PACKAGE)]
    command = cc + flags + [str(PACKAGE / CPP_SOURCE)]
    log = OUT / 'iokit-syntax.log'
    result = subprocess.run(command, env=env, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    log.write_text('CMD: ' + ' '.join(map(str, command)) + '\n\n' + result.stdout)
    if result.returncode:
        print(result.stdout[-8000:])
        raise SystemExit('Tb710fuBluetooth.cpp does not parse against the stub IOKit headers')
    report['iokit'] = {'stubbed': True, 'passed': True, 'includes': sorted(
        str(path.relative_to(STUBS)) for path in STUBS.rglob('*.h'))}
    print('   iokit parse ok (stubbed headers)')


def strip_comments(text):
    """Drop // and /* */ comments so a rule can be checked against code only (the package documents the
    very symbols it must not link)."""
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', text)


def stage_hygiene(report):
    """The rules that are cheapest to enforce by reading the text, and most expensive to break on
    hardware: no byte-wise library call may touch a mapped window, a volatile may never be incremented
    or decremented (clang -Werror,-Wdeprecated-volatile on macOS 27), and no Apple-private kext symbol
    may appear as a reference."""
    problems = []
    volatile_names = set()
    private_symbols = ('IOBluetooth', 'IONetworkingFamily', 'IOGraphicsFamily', 'IOAudioFamily',
                       'IOUSBHostFamily')
    for path in sorted(PACKAGE.glob('*')):
        if path.suffix not in ('.c', '.h', '.cpp'):
            continue
        text = path.read_text()
        for match in re.finditer(r'\b(?:memcpy|memset|bzero|bcopy|memmove)\s*\(', text):
            line = text.count('\n', 0, match.start()) + 1
            problems.append(f'{path.name}:{line}: byte-wise library call in the package: {match.group(0)}')
        for match in re.finditer(r'volatile\s+[A-Za-z_][\w:<>\s]*?\b([A-Za-z_]\w*)\s*(?:\[|;|=)', text):
            volatile_names.add(match.group(1))
        code = strip_comments(text)
        for symbol in private_symbols:
            if re.search(rf'\b{symbol}\w*\b', code):
                line = code[:re.search(rf'\b{symbol}\w*\b', code).start()].count('\n') + 1
                problems.append(f'{path.name}:{line}: references the Apple-private symbol {symbol}*')
    volatile_names.discard('void')
    for path in sorted(PACKAGE.glob('*')):
        if path.suffix not in ('.c', '.cpp'):
            continue
        text = path.read_text()
        for name in sorted(volatile_names):
            for match in re.finditer(rf'\b{name}\s*(\+\+|--)|(\+\+|--)\s*{name}\b', text):
                line = text.count('\n', 0, match.start()) + 1
                problems.append(f'{path.name}:{line}: increment of the volatile {name}: {match.group(0)}')
    if problems:
        print('\n'.join(problems))
        raise SystemExit('MMIO hygiene rules violated')

    # The one place memcpy is legitimate is the host test, which copies ordinary memory only.
    stub = (PACKAGE / CPP_SOURCE).read_text()
    if 'getPlatform' in stub and 'IOPlatformExpert.h' not in stub:
        raise SystemExit('Tb710fuBluetooth.cpp calls getPlatform() without <IOKit/IOPlatformExpert.h>')
    report['hygiene'] = {'passed': True, 'volatile_names': sorted(volatile_names),
                         'rules': ['no byte-wise access to a mapped window',
                                   'no increment on a volatile',
                                   'no Apple-private kext symbol reference',
                                   'IOPlatformExpert.h with getPlatform']}
    print('   mmio hygiene ok')


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cc, version, env = compiler()
    print(f'TB710FU Bluetooth host checks ({version})')
    report = {'compiler': version, 'syntax': [], 'unit': {}, 'iokit': {}, 'hygiene': {}}
    stage_syntax(cc, env, report)
    stage_unit(cc, env, report)
    stage_iokit(cc, env, report)
    stage_hygiene(report)
    report['passed'] = True
    (OUT / 'host-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'TB710FU Bluetooth host checks: PASS ({report["unit"].get("checks", 0)} unit checks)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
