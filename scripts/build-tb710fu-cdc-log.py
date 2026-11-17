#!/usr/bin/env python3
"""Build the independent CDC transport using the actual XNU C compile record."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/tb710fu/Tb710fuUsbCdc'
OUT = ROOT / 'build/tb710fu-cdc-log'
RECORD = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json'
SOURCES = [PACKAGE / name for name in ('CdcKernelTap.c', 'CdcLogRing.c',
           'CdcAcmProtocol.c', 'CdcTransportGuard.c', 'CdcDwc3.c', 'CdcXnu.c')]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def link_input(identity):
    # EXTRA_LD_ARCHIVES is not an incremental prerequisite in pinned XNU.
    # Only invalidate the actual generated LD output, never a source/cache.
    records = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    if (ROOT / 'build/xnu').resolve() != ROOT / 'build/xnu' or records.resolve() != records:
        raise SystemExit('CDC generated XNU output is redirected')
    target = records / 'mach.development.bcm2837.unstripped.noctf'
    stamp = records / '.tb710fu-cdc-link-input.json'
    for path in (target, stamp):
        if path.is_symlink() or (path.exists() and not path.is_file()):
            raise SystemExit('CDC link target/input must be an ordinary generated file')
    source = ROOT / '.cache/xnu-work'
    revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != identity['xnu_revision']:
        raise SystemExit('CDC prepared XNU source differs from the device lock')
    makefile = source / 'makedefs/MakeInc.kernel'
    rule = '$(TARGET)/$(KERNEL_FILE_NAME).unstripped.noctf: $(addprefix $(TARGET)/,'
    command = ('$(LD) $(LDFLAGS_KERNEL) $(LDFLAGS_KERNEL_ONLY) -filelist link.filelist '
               '$(filter %.o,$+) -o $@ $(LD_KERNEL_LIBS) $(LD_KERNEL_ARCHIVES)')
    if makefile.read_text().count(rule) != 1 or command not in makefile.read_text():
        raise SystemExit('Pinned actual LD rule changed; re-audit CDC invalidation')
    identity['link_rule_sha256'] = sha(makefile)
    saved = json.dumps(identity, indent=2) + '\n'
    previous = stamp.read_text() if stamp.is_file() else ''
    changed = previous != saved
    existed = target.is_file()
    if changed:
        target.unlink(missing_ok=True)
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode='w', dir=records,
                                             prefix=stamp.name + '.', delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(saved)
            os.replace(temporary, stamp)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    return {**identity, 'input_changed': changed, 'link_target': str(target),
            'link_target_removed': changed and existed, 'kernel_link_completed': False}


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
    device = Path(os.environ['UNIXNU_DEVICE']).resolve()
    lock = json.loads((device / 'upstream.lock.json').read_text())
    flags = shlex.split(os.environ.get('XNU_EXTRA_CFLAGS', ''))
    archive = OUT / 'libTb710fuCdcLog.a'
    link_flags = shlex.split(os.environ.get('XNU_EXTRA_LD_ARCHIVES', ''))
    if '-DXNU_TB710FU_CDC=1' not in flags:
        raise SystemExit('CDC requires its explicit strong bsd_init hook macro')
    if link_flags.count('-Wl,-force_load,' + str(archive)) != 1:
        raise SystemExit('Select the exact CDC archive once in EXTRA_LD_ARCHIVES')
    OUT.mkdir(parents=True, exist_ok=True)
    for stale in (archive, OUT / 'build-report.json', OUT / 'archive-link-input.json'):
        stale.unlink(missing_ok=True)
    objects = []
    for source in SOURCES:
        if not source.is_file():
            raise SystemExit(f'{source} missing')
        output = OUT / (source.stem + '.o')
        command = compile_command(record, source, output)
        (OUT / (source.stem + '.command.json')).write_text(json.dumps(
            {'directory': record['directory'], 'arguments': command}, indent=2) + '\n')
        result = subprocess.run(command, cwd=record['directory'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (OUT / (source.stem + '.log')).write_text(result.stdout)
        if result.returncode:
            raise SystemExit(result.stdout[-5000:])
        target.check_object(output.read_bytes(), source.name)
        objects.append(output)
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
    report = {
        'archive': str(archive),
        'archive_sha256': sha(archive),
        'objects': {source.name: sha(obj)
                    for source, obj in zip(SOURCES, objects)},
        'objects_sha256': {obj.name: sha(obj) for obj in objects},
        'sources': {str(source.relative_to(ROOT)): sha(source)
                    for source in SOURCES + sorted(PACKAGE.glob('*.h'))},
        'compile_commands_sha256': {source.name: sha(OUT / (source.stem + '.command.json'))
                                    for source in SOURCES},
        'dependency_records_sha256': {source.name: sha(OUT / (source.stem + '.d'))
                                      for source in SOURCES},
        'compile_record_sha256': sha(RECORD),
        'framework_revision': lock['unixnu']['commit'],
        'xnu_revision': lock['xnu_commit'],
        'linked_into_kernel': False,
        'runtime_qualified': False,
        'transport_sources_compiled': True,
        'cdc_enumeration_verified': False,
    }
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    identity = {'archive': str(archive.relative_to(ROOT)), 'archive_sha256': sha(archive),
                'framework_revision': lock['unixnu']['commit'], 'xnu_revision': lock['xnu_commit'],
                'recipe_sha256': sha(Path(__file__)), 'selected_link_flags': link_flags,
                'startup_cflags': flags}
    (OUT / 'archive-link-input.json').write_text(json.dumps(
        {**link_input(identity), 'archive_report_sha256': sha(OUT / 'build-report.json')}, indent=2) + '\n')
    print(archive)


if __name__ == '__main__':
    main()
