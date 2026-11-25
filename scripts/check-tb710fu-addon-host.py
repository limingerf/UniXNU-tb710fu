#!/usr/bin/env python3
"""Host check for the TB710FU addon reader (no IOKit, no hardware).

Compiles tests/tb710fu_addon_test.c and package/Tb710fuAdsp/Tb710fuAddon.c with the host C compiler,
AddressSanitizer and UndefinedBehaviorSanitizer, runs the binary, and writes build/tb710fu-addon-host/report.json.
Exits non-zero when the compile or any check fails.
"""
import hashlib
import json
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'package/Tb710fuAdsp'
TEST = ROOT / 'tests/tb710fu_addon_test.c'
OUT = ROOT / 'build/tb710fu-addon-host'


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
    binary = OUT / 'tb710fu_addon_test'
    sources = [TEST, PACKAGE / 'Tb710fuAddon.c']
    sanitizers = True
    command, compile_result = build(cc, sources, binary, sanitizers)
    log = compile_result.stdout
    if compile_result.returncode and any(word in log for word in ('libclang_rt', 'libasan', 'cannot find -lasan')):
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
        'sanitizers': sanitizers,
        'sources': {str(path.relative_to(ROOT)): sha(path) for path in sources},
        'checks': 'exact entry, fields, absent name, empty container, magic, zero field, total, count, truncation, trailing bytes, interior NUL, 160-byte path',
    }
    (OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if run.returncode == 0 else 1


if __name__ == '__main__':
    raise SystemExit(main())
