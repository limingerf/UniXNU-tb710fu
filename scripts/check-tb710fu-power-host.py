#!/usr/bin/env python3
"""Host check for the TB710FU battery codec (no IOKit, no hardware).

Compiles tests/tb710fu_battmgr_test.c and package/Tb710fuPower/Tb710fuBattmgr.c with the host C compiler,
AddressSanitizer and UndefinedBehaviorSanitizer, runs the binary, and writes build/tb710fu-power-host/report.json.
Exits non-zero when the compile or any check fails.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'package/Tb710fuPower'
TEST = ROOT / 'tests/tb710fu_battmgr_test.c'
OUT = ROOT / 'build/tb710fu-power-host'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compiler():
    for name in ('gcc', 'clang', 'cc'):
        found = shutil.which(name)
        if found:
            return found
    raise SystemExit('no host C compiler (gcc, clang or cc)')


def build(cc, sources, binary, sanitizers):
    command = [cc, '-std=c11', '-g', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(PACKAGE)]
    if sanitizers:
        command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    command += [*map(str, sources), '-o', str(binary)]
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return command, result


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cc = compiler()
    binary = OUT / 'tb710fu_battmgr_test'
    sources = [TEST, PACKAGE / 'Tb710fuBattmgr.c']
    sanitizers = True
    command, compile_result = build(cc, sources, binary, sanitizers)
    log = compile_result.stdout
    if compile_result.returncode and any(word in log for word in ('libclang_rt', 'libasan', 'cannot find -lasan')):
        # The host toolchain has no sanitizer runtime; the checks themselves do not depend on it.
        sanitizers = False
        command, compile_result = build(cc, sources, binary, sanitizers)
        log = 'sanitizer runtime missing, retried without sanitizers\n' + compile_result.stdout
    (OUT / 'compile.log').write_text(log)
    if compile_result.returncode:
        print(log[-4000:])
        print('compile failed')
        return 1
    run = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (OUT / 'run.log').write_text(run.stdout)
    print(run.stdout.strip())
    report = {
        'binary': str(binary.relative_to(ROOT)),
        'binary_sha256': sha(binary),
        'compiler': cc,
        'compile_command': ' '.join(command),
        'compile_ok': compile_result.returncode == 0,
        'run_exit': run.returncode,
        'run_ok': run.returncode == 0,
        'sources': {str(p.relative_to(ROOT)): sha(p) for p in sources + [PACKAGE / 'Tb710fuBattmgr.h']},
    }
    (OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    if run.returncode:
        print('host checks failed')
        return 1
    print(f'host checks passed: {binary}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
