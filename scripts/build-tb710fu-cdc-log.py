#!/usr/bin/env python3
"""Build the bounded console log ring for the XNU character-output hooks."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/tb710fu/Tb710fuUsbCdc'
OUT = ROOT / 'build/tb710fu-cdc-log'
RECORD = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'
SOURCES = [PACKAGE / 'CdcKernelTap.c', PACKAGE / 'CdcLogRing.c']


def compile_command(record, source, output):
    arguments = record['arguments']
    command = []
    index = 0
    replaced = False
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
            replaced = True
        elif value == '-o':
            command.extend(['-o', str(output)])
            index += 1
        elif value in ('-MF', '-MT', '-MQ', '-MJ'):
            index += 1
        elif value in ('-MD', '-MMD', '-MP'):
            pass
        elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    if not replaced:
        raise SystemExit('XNU C compilation record does not contain its source argument')
    command += ['-Wall', '-Wextra', '-Werror', '-mgeneral-regs-only',
                '-I', str(PACKAGE), '-MD', '-MF', str(output.with_suffix('.d'))]
    return command


def main():
    if not RECORD.is_file():
        raise SystemExit(f'{RECORD} missing; build the minimal XNU kernel first')
    record = json.loads(RECORD.read_text().strip().rstrip(','))
    target = __import__('kernel_target')
    OUT.mkdir(parents=True, exist_ok=True)
    objects = []
    for source in SOURCES:
        if not source.is_file():
            raise SystemExit(f'{source} missing')
        output = OUT / (source.stem + '.o')
        command = compile_command(record, source, output)
        result = subprocess.run(command, cwd=record['directory'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (OUT / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(result.stdout[-5000:])
        target.check_object(output.read_bytes(), source.name)
        objects.append(output)
    archive = OUT / 'libTb710fuCdcLog.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
    report = {
        'archive': str(archive),
        'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
        'objects': {source.name: hashlib.sha256(obj.read_bytes()).hexdigest()
                    for source, obj in zip(SOURCES, objects)},
        'sources': {str(source.relative_to(ROOT)): hashlib.sha256(source.read_bytes()).hexdigest()
                    for source in SOURCES},
        'runtime_qualified': False,
        'transport_connected': False,
    }
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(archive)


if __name__ == '__main__':
    main()
