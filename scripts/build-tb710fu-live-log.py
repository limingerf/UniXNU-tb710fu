#!/usr/bin/env python3
"""Build the native ramoops QR library from the actual XNU C compile record.

The pinned Mu sources are copied unchanged to generated output.  The UEFI
compatibility header is forced only into those sources and LiveQr.c; the XNU
adapter retains the real kernel headers and compilation environment.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import subprocess
import tempfile
import urllib.request


ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/tb710fu/Tb710fuLiveLog'
OUT = ROOT / 'build/tb710fu-live-log'
RECORDS = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
RECORD = RECORDS / 'osfmk/DEVELOPMENT/startup.o.json'
QR_REPOSITORY = 'microsoft/mu_plus'
QR_COMMIT = 'b8d46c71610318e36a661819e1e3c0b8bdda413a'
QR_BASE = 'MsGraphicsPkg/Library/QrEncoderLib/'
QR_PATHS = {
    QR_BASE + 'QrEncoderLib.inf', QR_BASE + 'QrEncoderLib.c',
    QR_BASE + 'QrEncoderTables.c', QR_BASE + 'QrEncoderTables.h',
    'MsGraphicsPkg/Include/Library/QrEncoderLib.h',
}
FONT_SHA256 = 'e3174a605a6cfb351e5fd613f5c0d29c509a873fc3218b79fc4e7115fe405071'
LOCAL_SOURCES = ('LiveProtocol.c', 'LiveDisplay.c', 'LiveQr.c', 'LiveXnu.c')
SHIMS = ('Uefi.h', 'Protocol/GraphicsOutput.h', 'Library/BaseMemoryLib.h',
         'Library/DebugLib.h', 'Library/MemoryAllocationLib.h',
         'Library/UefiBootServicesTableLib.h')
REQUIRED_FLAGS = ('-mgeneral-regs-only', '-mstrict-align', '-ffreestanding',
                  '-fno-builtin', '-DXNU_TB710FU_LIVE_LOG=1')
PINNED_QR_FLAGS = ('-Wno-sign-compare', '-Wno-unused-parameter', '-fwrapv',
                   '-Wno-shift-negative-value', '-Wno-unused-but-set-variable')
PINNED_QR_FLAGS_REASON = (
    'Preserve the pinned Mu source bytes: signed/UINTN indices, signed-bitmask '
    'wrapping semantics for bounded counts, and DEBUG-discarded variables/parameters.')
DUAL_SOURCE_LOG_CONTRACT = {
    'schema_version': 1,
    'left': {'slot': 0, 'source': 'xnu-console', 'capture_ring_bytes': 65536,
             'current_tail_bytes_max': 3072, 'early_backup_bytes_max': 512,
             'early_backup_separate_metadata_and_raw_section': True,
             'export_body_bytes_max': 4096},
    'right': {'slot': 1, 'source': 'ramoops', 'current_tail_bytes_max': 4096,
              'provider': 'tb710fu_ramoops_snapshot_copy', 'export_body_bytes_max': 4096},
    'frames_per_poll_max': 1, 'poll_interval_ms': 250,
    # Multi-frame transfers need a long immutable dwell so a slow camera can
    # collect every frame of one session.
    'complete_rounds_before_replacement_min': 16,
    # The pre-worker export is a single source block, fully described by one
    # photograph; its dwell is deliberately short so the newest boot text is
    # published before an early halt can freeze the display.
    'single_frame_rounds_before_replacement': 2,
    'checkpoint_mode': {
        'argument': 'tb710fu-live-checkpoints=1',
        'requires': 'tb710fu-live-log=1',
        'provider': 'tb710fu_live_log_checkpoint',
        'background_worker_disabled': True,
        'sampled_provider': 'tb710fu_live_log_checkpoint_sampled',
        'sampled_quiet_period_ms_after_attempt': 250,
        'explicit_stage_boundaries_immediate': True,
        'independent_single_frame_session_per_render': True,
        'raw_tail': 'largest current XNU suffix fitting one systematic frame',
        'display': {'checkpoint_qr_anchor': 'bottom-right',
                    'scroll_log_region': 'left-of-checkpoint-qr',
                    'scroll_source': 'latest-current-xnu-capture-tail',
                    'scroll_max_rows': 64, 'scroll_max_columns': 256,
                    'scroll_offset_model': 'absolute-range; append-unseen-suffix; mark-overwrite-gap',
                    'scroll_refresh': 'after-synchronous-checkpoint-snapshot',
                    'console_character_writer_draws': False,
                    'status_location': 'footer-left-of-checkpoint-qr'},
    },
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    ordinary_output(path)
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def ordinary_output(path):
    # This builder never writes through a generated symlink into a source cache.
    if path.resolve() != path or path.is_symlink():
        raise SystemExit(f'Live-log generated output is redirected: {path}')
    if path.exists() and not path.is_file():
        raise SystemExit(f'Live-log generated output is not an ordinary file: {path}')


def revision(path):
    return subprocess.check_output(
        ['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()


def stage_qr(device):
    manifest_path = device / 'diagnostics/early-report/qr-sources.json'
    manifest = json.loads(manifest_path.read_text())
    files = manifest.get('files', [])
    if (manifest.get('repository') != QR_REPOSITORY or
            manifest.get('commit') != QR_COMMIT or not isinstance(files, list) or
            {item.get('path') for item in files} != QR_PATHS or len(files) != len(QR_PATHS)):
        raise SystemExit('Live-log QR manifest differs from the audited Mu source selection')
    staged = OUT / 'upstream'
    result = {}
    caches = (device / '.local/early-fault-qr/upstream',
              ROOT / '.local/early-fault-qr/upstream')
    for item in files:
        relative = PurePosixPath(item['path'])
        expected = item.get('sha256', '')
        if not re.fullmatch('[0-9a-f]{64}', expected):
            raise SystemExit(f'Invalid QR source digest: {relative}')
        url = f'https://raw.githubusercontent.com/{QR_REPOSITORY}/{QR_COMMIT}/{relative}'
        origin = url
        data = None
        for cache in caches:
            candidate = cache / relative
            if candidate.is_file():
                data = candidate.read_bytes()
                origin = str(candidate)
                break
        if data is None:
            # Network retrieval is confined to a fixed commit/path and generated
            # output. No official fetched source or script is modified.
            with urllib.request.urlopen(url, timeout=60) as response:
                data = response.read(1024 * 1024 + 1)
            if len(data) > 1024 * 1024:
                raise SystemExit(f'Pinned QR source exceeds its expected bound: {relative}')
        if hashlib.sha256(data).hexdigest() != expected:
            raise SystemExit(f'Pinned QR source SHA-256 mismatch: {origin}')
        destination = staged / relative
        ordinary_output(destination)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        if sha(destination) != expected:
            raise SystemExit(f'Copied QR source SHA-256 mismatch: {relative}')
        result[str(relative)] = {'sha256': expected, 'origin': origin,
                                 'pinned_url': url, 'compiled_copy': str(destination)}
    return manifest_path, result


def stage_includes(device):
    include = OUT / 'include'
    for relative in SHIMS:
        path = include / relative
        ordinary_output(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        # Types, services and the symbol namespace come from the explicit
        # forced LiveQrCompat.h, never from UEFI runtime/boot services.
        path.write_text('/* LiveQrCompat.h supplies the freestanding interface. */\n',
                        encoding='utf-8')
    font = device / 'diagnostics/early-report/Font.h'
    if sha(font) != FONT_SHA256:
        raise SystemExit('Live-log licensed font differs from its audited input')
    destination = include / 'Font.h'
    ordinary_output(destination)
    destination.write_bytes(font.read_bytes())
    return font, {str(path.relative_to(OUT)): sha(path)
                  for path in sorted(include.rglob('*')) if path.is_file()}


def compile_command(record, source, output, qr=False, display_build_label=None):
    arguments = record.get('arguments')
    if (not isinstance(arguments, list) or not arguments or
            not all(isinstance(value, str) for value in arguments)):
        raise SystemExit('XNU C compile record has no argument vector')
    command = []
    index = 0
    replaced = outputs = 0
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
            replaced += 1
        elif value == '-o':
            if index + 1 >= len(arguments):
                raise SystemExit('Truncated XNU output argument')
            command.extend(['-o', str(output)])
            outputs += 1
            index += 1
        elif value in ('-MF', '-MT', '-MQ', '-MJ'):
            if index + 1 >= len(arguments):
                raise SystemExit('Truncated XNU dependency argument')
            index += 1
        elif value in ('-MD', '-MMD', '-MP'):
            pass
        elif (value == '-Xclang' and index + 1 < len(arguments) and
              arguments[index + 1].startswith('-W')):
            index += 1
        elif not value.startswith('-W'):
            command.append(value)
        index += 1
    if replaced != 1 or outputs != 1 or '-c' not in command:
        raise SystemExit('Require one actual XNU C compile source and output')
    command += ['-Wall', '-Wextra', '-Werror', *REQUIRED_FLAGS,
                '-I', str(PACKAGE), '-I', str(OUT / 'include')]
    if qr:
        command += ['-include', str(PACKAGE / 'LiveQrCompat.h'),
                    '-I', str(OUT / 'upstream/MsGraphicsPkg/Include')]
    if source.name in ('QrEncoderLib.c', 'QrEncoderTables.c'):
        # Preserve the fixed source bytes; these two old UEFI sources mix
        # signed indices and UINTN and have optional/debug-only parameters.
        command += list(PINNED_QR_FLAGS)
    if source.name == 'LiveXnu.c':
        if display_build_label is None or not re.fullmatch(r'src-[0-9a-f]{8}-ramoops', display_build_label):
            raise SystemExit('LiveXnu requires its exact device-commit display label')
        command += ['-DTB_LIVE_BUILD_LABEL="' + display_build_label + '"']
    return command + ['-MD', '-MF', str(output.with_suffix('.d'))]


def dependency_inputs(path, directory):
    text = path.read_text().replace('\\\n', ' ')
    if ':' not in text:
        raise SystemExit(f'Invalid compiler dependency record: {path}')
    dependencies = {}
    for name in shlex.split(text.split(':', 1)[1]):
        dependency = Path(name)
        if not dependency.is_absolute():
            dependency = Path(directory) / dependency
        if not dependency.is_file():
            raise SystemExit(f'Compiler dependency disappeared: {dependency}')
        dependencies[str(dependency.resolve())] = sha(dependency)
    if not dependencies:
        raise SystemExit(f'Empty compiler dependency record: {path}')
    return dependencies


def check_qr_symbols(path):
    text = subprocess.check_output(['xcrun', 'nm', '-g', str(path)], text=True)
    definitions = []
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 3 and re.fullmatch('[0-9a-fA-F]+', fields[0]):
            kind, name = fields[1:]
            if kind.upper() != 'U':
                if not name.startswith(('_tb_live_', '_tb710fu_')):
                    raise SystemExit(f'Unprefixed QR external definition in {path.name}: {name}')
                definitions.append(name)
        elif len(fields) == 2 and fields[0].upper() == 'U':
            # Compiler helpers may resolve from the real XNU environment;
            # UEFI/QR service imports must use the same private namespace.
            name = fields[1]
            forbidden = ('_Allocate', '_FreePool', '_CopyMem', '_SetMem32',
                         '_QrEncodeData', '_gBS', '_gST', '_memcpy', '_memset')
            if name.startswith(forbidden):
                raise SystemExit(f'Unprefixed QR service import in {path.name}: {name}')
    if not definitions:
        raise SystemExit(f'No global definitions found in QR object: {path}')
    return {'nm_output': text, 'external_definitions': definitions,
            'unprefixed_uefi_qr_definitions': False}


def link_input(identity):
    # The pinned EXTRA_LD_ARCHIVES addition is not an incremental prerequisite.
    # Invalidate only the verified generated LD output when actual inputs change.
    if ((ROOT / 'build/xnu').resolve() != ROOT / 'build/xnu' or
            RECORDS.resolve() != RECORDS):
        raise SystemExit('Live-log generated XNU output is redirected')
    target = RECORDS / 'mach.development.bcm2837.unstripped.noctf'
    stamp = RECORDS / '.tb710fu-live-log-link-input.json'
    for path in (target, stamp):
        ordinary_output(path)
    source = ROOT / '.cache/xnu-work'
    if revision(source) != identity['xnu_revision']:
        raise SystemExit('Live-log prepared XNU source differs from the device lock')
    makefile = source / 'makedefs/MakeInc.kernel'
    rule = '$(TARGET)/$(KERNEL_FILE_NAME).unstripped.noctf: $(addprefix $(TARGET)/,'
    command = ('$(LD) $(LDFLAGS_KERNEL) $(LDFLAGS_KERNEL_ONLY) -filelist link.filelist '
               '$(filter %.o,$+) -o $@ $(LD_KERNEL_LIBS) $(LD_KERNEL_ARCHIVES)')
    text = makefile.read_text()
    if text.count(rule) != 1 or command not in text:
        raise SystemExit('Pinned actual LD rule changed; re-audit live-log invalidation')
    identity = {**identity, 'link_rule_sha256': sha(makefile)}
    saved = json.dumps(identity, indent=2) + '\n'
    previous = stamp.read_text() if stamp.is_file() else ''
    changed = previous != saved
    existed = target.is_file()
    if changed:
        target.unlink(missing_ok=True)
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode='w', dir=RECORDS,
                                             prefix=stamp.name + '.', delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(saved)
            os.replace(temporary, stamp)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    return {**identity, 'input_changed': changed, 'link_target': str(target),
            'link_target_removed': changed and existed, 'kernel_link_completed': False}


def main():
    if not RECORD.is_file():
        raise SystemExit(f'{RECORD} missing; build the minimal XNU kernel first')
    if OUT.resolve() != OUT:
        raise SystemExit('Live-log output must remain inside the resolved tree/build directory')
    device = Path(os.environ['UNIXNU_DEVICE']).resolve()
    device_commit = revision(device)
    if not re.fullmatch('[0-9a-f]{40}', device_commit):
        raise SystemExit('Live-log requires the exact device Git commit')
    display_build_label = 'src-' + device_commit[:8] + '-ramoops'
    lock = json.loads((device / 'upstream.lock.json').read_text())
    record = json.loads(RECORD.read_text().strip().rstrip(','))
    flags = shlex.split(os.environ.get('XNU_EXTRA_CFLAGS', ''))
    link_flags = shlex.split(os.environ.get('XNU_EXTRA_LD_ARCHIVES', ''))
    archive = OUT / 'libTb710fuLiveLog.a'
    if '-DXNU_TB710FU_LIVE_LOG=1' not in flags:
        raise SystemExit('Live-log requires its explicit strong native hook macro')
    if link_flags.count('-Wl,-force_load,' + str(archive)) != 1:
        raise SystemExit('Select the exact live-log archive once in EXTRA_LD_ARCHIVES')
    target_path = ROOT / 'scripts/kernel_target.py'
    spec = importlib.util.spec_from_file_location('tb710fu_live_kernel_target', target_path)
    if spec is None or spec.loader is None:
        raise SystemExit(f'Pinned kernel object verifier unavailable: {target_path}')
    target = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(target)
    OUT.mkdir(parents=True, exist_ok=True)
    for stale in (archive, OUT / 'build-report.json', OUT / 'archive-link-input.json'):
        ordinary_output(stale)
        stale.unlink(missing_ok=True)
    local_sources = [PACKAGE / name for name in LOCAL_SOURCES]
    headers = sorted(PACKAGE.glob('*.h'))
    if PACKAGE / 'LiveQrCompat.h' not in headers:
        raise SystemExit('Live-log QR namespace compatibility header is missing')
    local_inputs = local_sources + headers
    original_inputs = {}
    for path in local_inputs:
        original = device / 'package/Tb710fuLiveLog' / path.name
        if not path.is_file() or not original.is_file() or sha(path) != sha(original):
            raise SystemExit(f'Resolved live-log source differs from device source: {path}')
        original_inputs[str(original)] = sha(original)
    manifest, upstream = stage_qr(device)
    font, includes = stage_includes(device)
    sources = local_sources + [OUT / 'upstream' / QR_BASE / name
                               for name in ('QrEncoderLib.c', 'QrEncoderTables.c')]
    objects, command_hashes, dependency_hashes, dependency_record_hashes = [], {}, {}, {}
    qr_symbols = {}
    for source in sources:
        output = OUT / (source.stem + '.o')
        ordinary_output(output)
        output.unlink(missing_ok=True)
        qr = source.name in ('LiveQr.c', 'QrEncoderLib.c', 'QrEncoderTables.c')
        command = compile_command(record, source, output, qr=qr,
                                  display_build_label=display_build_label)
        command_path = OUT / (source.stem + '.command.json')
        write_json(command_path, {'directory': record['directory'], 'arguments': command,
                                  'file': str(source), 'output': str(output)})
        result = subprocess.run(command, cwd=record['directory'], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log = OUT / (source.stem + '.log')
        ordinary_output(log)
        log.write_text(result.stdout, encoding='utf-8')
        if result.returncode:
            raise SystemExit(f'{source.name} compiler failed ({result.returncode}):\n{result.stdout[-6000:]}')
        target.check_object(output.read_bytes(), source.name)
        dependencies = dependency_inputs(output.with_suffix('.d'), record['directory'])
        dependency_path = OUT / (source.stem + '.dependencies.json')
        write_json(dependency_path, dependencies)
        command_hashes[source.name] = sha(command_path)
        dependency_hashes[source.name] = sha(output.with_suffix('.d'))
        dependency_record_hashes[source.name] = sha(dependency_path)
        if qr:
            symbols = check_qr_symbols(output)
            symbol_log = OUT / (source.stem + '.symbols.txt')
            ordinary_output(symbol_log)
            symbol_log.write_text(symbols.pop('nm_output'), encoding='utf-8')
            qr_symbols[source.name] = {**symbols, 'nm_output_sha256': sha(symbol_log)}
        objects.append(output)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
    # Some Darwin ar versions list the symbol-table member; it is not a
    # seventh compiled source object.
    members = [name for name in subprocess.check_output(
        ['xcrun', 'ar', 't', str(archive)], text=True).splitlines()
        if not name.startswith('__.SYMDEF')]
    if members != [path.name for path in objects]:
        raise SystemExit('Live-log archive members differ from the compiled objects')
    report = {
        'archive': str(archive), 'archive_sha256': sha(archive), 'archive_members': members,
        'objects': {source.name: sha(obj) for source, obj in zip(sources, objects)},
        'objects_sha256': {obj.name: sha(obj) for obj in objects},
        'sources': {str(path.relative_to(ROOT)): sha(path) for path in local_inputs},
        'device_sources': original_inputs,
        'qr_upstream': {'repository': QR_REPOSITORY, 'commit': QR_COMMIT,
                        'manifest': str(manifest), 'manifest_sha256': sha(manifest),
                        'files': upstream, 'unchanged_source_copies': True,
                        'source_specific_flags': list(PINNED_QR_FLAGS),
                        'source_specific_flags_reason': PINNED_QR_FLAGS_REASON},
        'font': {'source': str(font), 'sha256': sha(font), 'copy': str(OUT / 'include/Font.h')},
        'generated_include_sha256': includes, 'qr_symbol_isolation': qr_symbols,
        'compile_commands_sha256': command_hashes,
        'dependency_records_sha256': dependency_hashes,
        'included_inputs_records_sha256': dependency_record_hashes,
        'compile_record_sha256': sha(RECORD), 'compile_record': str(RECORD),
        'kernel_target_verifier_sha256': sha(target_path), 'target': target.TRIPLE,
        'recipe_sha256': sha(Path(__file__)), 'required_flags': list(REQUIRED_FLAGS),
        'device_commit': device_commit, 'display_build_label': display_build_label,
        'framework_revision': lock['unixnu']['commit'], 'xnu_revision': lock['xnu_commit'],
        'linked_into_kernel': False, 'runtime_qualified': False,
        'live_log_sources_compiled': True, 'qr_hardware_display_verified': False,
        'dual_source_log_contract': DUAL_SOURCE_LOG_CONTRACT,
    }
    report_path = OUT / 'build-report.json'
    write_json(report_path, report)
    identity = {'archive': str(archive.relative_to(ROOT)), 'archive_sha256': sha(archive),
                'framework_revision': lock['unixnu']['commit'], 'xnu_revision': lock['xnu_commit'],
                'recipe_sha256': sha(Path(__file__)), 'selected_link_flags': link_flags,
                'startup_cflags': flags, 'compile_record_sha256': sha(RECORD),
                'qr_manifest_sha256': sha(manifest), 'font_sha256': sha(font)}
    identity.update(device_commit=device_commit, display_build_label=display_build_label)
    write_json(OUT / 'archive-link-input.json',
               {**link_input(identity), 'archive_report_sha256': sha(report_path)})
    print(archive)


if __name__ == '__main__':
    main()
