#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the real SourceRootBinding.cpp with a narrow host IOKit model."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1] / 'SourceRootBinding.cpp'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(output, compiler, sanitizers):
    report_path = output / 'report.json'
    report_path.unlink(missing_ok=True)
    # Preserve every non-include line, including blank lines and preprocessor
    # directives. No production function is copied into the checked-in fixture.
    source_bytes = SOURCE.read_bytes()
    source_sha256 = hashlib.sha256(source_bytes).hexdigest()
    text = source_bytes.decode().replace('\r\n', '\n')
    body = ''.join('\n' if re.match(r'^\s*#\s*include\b', line) else line
                   for line in text.splitlines(keepends=True))
    generated = output / 'SourceRootBinding.model.inc'
    generated.write_text('#line 1 ' + json.dumps(SOURCE.as_posix()) + '\n' + body)
    executable = output / 'binding-model'
    command = [*compiler, '-std=c++17', '-O1', '-g', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
               '-fno-omit-frame-pointer', '-I', str(output), str(HERE / 'BindingModel.cpp'),
               '-o', str(executable)]
    if sanitizers:
        command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    version = subprocess.check_output([*compiler, '--version'], text=True).strip()
    (output / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
    compiled = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (output / 'compile.log').write_text(compiled.stdout)
    if compiled.returncode:
        raise SystemExit(compiled.stdout or f'Compiler exited {compiled.returncode}')
    tested = subprocess.run([str(executable)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (output / 'test.log').write_text(tested.stdout)
    print(tested.stdout, end='')
    if tested.returncode:
        raise SystemExit(f'Binding model exited {tested.returncode}')
    if sha(SOURCE) != source_sha256:
        raise SystemExit('Production source changed during the test; no success report written')
    report = {'passed': True, 'compiler': version, 'sanitizers': sanitizers,
              'source_sha256': source_sha256, 'generated_sha256': sha(generated),
              'fixture_sha256': {name: sha(HERE / name) for name in
                                 ('BindingModel.h', 'BindingModel.cpp', Path(__file__).name)},
              'target_compile_verified': False, 'runtime_qualified': False}
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Production source SHA-256: {report["source_sha256"]}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--sanitizers', action='store_true', help='enable ASAN and UBSAN')
    parser.add_argument('--output', type=Path, help='retain generated input, commands, logs and report here')
    args = parser.parse_args()
    compiler = shlex.split(args.cxx)
    if args.output:
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=True)
        (output / 'report.json').unlink(missing_ok=True)
        run(output, compiler, args.sanitizers)
    else:
        with tempfile.TemporaryDirectory(prefix='tb710fu-binding-model-') as temporary:
            run(Path(temporary), compiler, args.sanitizers)


if __name__ == '__main__':
    main()
