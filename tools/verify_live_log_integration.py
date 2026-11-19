"""Bind native ramoops/QR sources, strong hook calls and the real XNU LD output.

This is static build evidence. It does not establish framebuffer visibility,
actual ramoops bytes, scheduling, QR capture or hardware boot.
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess

from verify_storage_integration import (ROOT, LOCK, digest, force_load_count,
                                        hook_macro_selected, hook_symbol,
                                        load_audit, require)


SOURCES = ('LiveProtocol', 'LiveDisplay', 'LiveQr', 'LiveXnu',
           'QrEncoderLib', 'QrEncoderTables')
QR_OBJECTS = ('LiveQr', 'QrEncoderLib', 'QrEncoderTables')
FINAL_SYMBOLS = (
    '_tb710fu_live_log_init', '_tb710fu_live_log_freeze',
    '_tb710fu_live_log_putc', '_tb710fu_live_log_checkpoint',
    '_tb710fu_live_log_checkpoint_sampled', '_tb710fu_live_capture_putc',
    '_tb710fu_live_capture_copy', '_tb710fu_live_log_set_source',
    '_tb710fu_live_log_snapshot_file',
    '_tb710fu_live_log_snapshot_single_file',
    '_tb710fu_live_log_poll', '_tb710fu_live_log_refresh',
    '_tb710fu_live_log_active', '_tb710fu_ramoops_snapshot_copy',
    '_tb710fu_live_log_sender_init', '_tb710fu_live_log_snapshot',
    '_tb710fu_live_log_next_frame', '_tb710fu_live_log_can_snapshot',
    '_tb710fu_live_log_can_snapshot_single',
    '_tb710fu_live_log_get_info', '_tb_live_display_bind',
    '_tb_live_display_bands', '_tb_live_display_qr_slot',
    '_tb_live_display_checkpoint_qr', '_tb_live_display_scroll_log',
    '_tb_live_display_checkpoint_status', '_tb_live_display_status', '_tb_live_qr_encode',
    '_tb_live_qr_QrEncodeData', '_tb_live_qr_InitializeLogTables',
)
HOOK_OBJECTS = {
    'osfmk/DEVELOPMENT/arm_init.o': ('_tb710fu_live_log_init', '_tb710fu_live_log_freeze'),
    'osfmk/DEVELOPMENT/arm_vm_init.o': ('_tb710fu_live_log_active',
                                        '_tb710fu_live_log_checkpoint',
                                        '_tb710fu_live_log_checkpoint_sampled'),
    'osfmk/DEVELOPMENT/machine_routines.o': ('_tb710fu_live_log_checkpoint',),
    'osfmk/DEVELOPMENT/startup.o': ('_tb710fu_live_log_poll', '_tb710fu_live_log_checkpoint',
                                    '_tb710fu_live_log_checkpoint_sampled'),
    'osfmk/DEVELOPMENT/kalloc.o': ('_tb710fu_live_log_checkpoint',
                                   '_tb710fu_live_log_checkpoint_sampled'),
    'osfmk/DEVELOPMENT/video_console.o': ('_tb710fu_live_log_active', '_tb710fu_live_log_refresh'),
    'osfmk/DEVELOPMENT/serial_console.o': (
        '_tb710fu_live_log_active', '_tb710fu_live_log_putc',
        '_tb710fu_ramoops_putc', '_tb710fu_cdc_log_tap_putc'),
    'pexpert/DEVELOPMENT/pe_kprintf.o': ('_tb710fu_ramoops_putc', '_tb710fu_live_log_putc'),
}
ADAPTER_SOURCE_IMPORTS = (
    '_tb710fu_ramoops_snapshot_copy', '_tb710fu_live_capture_putc',
    '_tb710fu_live_capture_copy', '_tb710fu_live_log_set_source',
    '_tb710fu_live_log_snapshot_file', '_tb710fu_live_log_snapshot_single_file',
)
CHECKPOINT_DISPLAY_IMPORTS = (
    '_tb_live_display_checkpoint_qr', '_tb_live_display_scroll_log',
    '_tb_live_display_checkpoint_status',
)
CHECKPOINT_WORKER_IMPORTS = (
    '_kernel_thread_start', '_clock_interval_to_deadline',
    '_assert_wait_deadline', '_thread_block',
)
DISABLED_CAPTURE_CALLERS = {'_tb710fu_live_capture_disabled', '_console_write',
                           '_console_write_unbuffered', '__cnputs',
                           '_console_ring_try_empty', '_console_ring_try_empty_locked'}
CAPTURE_SOURCE_FUNCTIONS = {
    'osfmk/DEVELOPMENT/serial_console.o': {
        '__cnputc': ('static inline void\n_cnputc(char c, bool poll)', 'c'),
        '_tb710fu_live_capture_disabled': (
            'static void\ntb710fu_live_capture_disabled(const char *text, int size)', r'text\[i\]')},
    'pexpert/DEVELOPMENT/pe_kprintf.o': {
        '_serial_putc': ('void\nserial_putc(char c)', 'c'),
        '_serial_putc_crlf': ('static void\nserial_putc_crlf(char c)', 'c')},
}


def load_builder():
    path = ROOT / 'scripts/build-tb710fu-live-log.py'
    spec = importlib.util.spec_from_file_location('tb710fu_live_build_recipe', path)
    require(spec is not None and spec.loader is not None, 'Live-log build recipe unavailable')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def strong_import(obj, name):
    symbols = hook_symbol(obj, name)
    require(len(symbols) == 1 and symbols[0]['kind'] & 14 == 0 and
            symbols[0]['kind'] & 1 and not symbols[0]['desc'] & 0x40,
            'Expected one strong native hook import: ' + name)
    return symbols[0]


def strong_definition(obj, name):
    symbols = hook_symbol(obj, name)
    require(len(symbols) == 1 and symbols[0]['kind'] & 14 == 14 and
            symbols[0]['kind'] & 1 and not symbols[0]['desc'] & 0x80,
            'Expected one strong native provider definition: ' + name)
    return symbols[0]


def branch_relocations(obj, symbol):
    """Require real ARM64_RELOC_BRANCH26 instructions, not unused declarations."""
    symbols = obj.nlists()
    wanted = [index for index, item in enumerate(symbols) if item['name'] == symbol]
    require(len(wanted) == 1, 'Ambiguous relocation symbol: ' + symbol)
    calls = []
    for number, section in enumerate(obj.sections, 1):
        if not section['flags'] & 0x80000000:
            continue
        functions = sorted((item['value'], item['name']) for item in symbols
                           if item['section'] == number and item['kind'] & 14 == 14 and
                           not item['kind'] & 0xe0 and item['name'].startswith('_'))
        for index in range(section['nreloc']):
            address, info = struct.unpack_from('<II', obj.data, section['reloff'] + index * 8)
            if info & 0xffffff != wanted[0] or not info & (1 << 27) or info >> 28 != 2:
                continue
            require(info & (1 << 24) and (info >> 25) & 3 == 2,
                    'Invalid ARM64 branch relocation: ' + symbol)
            require(address % 4 == 0 and address + 4 <= section['size'],
                    'Out-of-range hook branch relocation: ' + symbol)
            word = struct.unpack_from('<I', obj.data, section['fileoff'] + address)[0]
            require(word & 0x7c000000 == 0x14000000,
                    'Hook relocation is not a direct B/BL instruction: ' + symbol)
            va = section['va'] + address
            preceding = [item for item in functions if item[0] <= va]
            calls.append({'section': section['name'], 'section_offset': hex(address),
                          'instruction': hex(word), 'call': 'bl' if word >> 31 else 'b',
                          'caller': preceding[-1][1] if preceding else None})
    require(calls, 'Strong hook has no actual direct branch relocation: ' + symbol)
    return calls


def capture_source_evidence(command, functions):
    """Bind both source paths even when Clang inlines their calls together.

    Direct native branch relocations separately establish that the provider
    call was compiled. A nearest preceding symbol cannot distinguish inlined
    normal/disabled source paths, so never require a standalone _cnputc.
    """
    source = Path(command['file'])
    if not source.is_absolute():
        source = Path(command['directory']) / source
    text = source.read_text()
    bodies = {}
    for name, (signature, argument) in functions.items():
        marker = signature + '\n{'
        require(text.count(marker) == 1, 'Ambiguous actual console capture function: ' + name)
        start = text.index(marker)
        pos, depth = start + len(marker), 1
        while depth and pos < len(text):
            if text[pos] == '{': depth += 1
            elif text[pos] == '}': depth -= 1
            pos += 1
        require(depth == 0, 'Truncated actual console capture function: ' + name)
        body = text[start:pos]
        require(re.search(r'\btb710fu_live_log_putc\s*\(\s*' + argument + r'\s*\)\s*;', body),
                'Actual console path omits independent XNU capture: ' + name)
        bodies[name] = hashlib.sha256(body.encode()).hexdigest()
    return {'compiled_source': str(source), 'compiled_source_sha256': digest(source),
            'provider': '_tb710fu_live_log_putc', 'function_body_sha256': bodies}


def deployment(obj):
    values = [struct.unpack_from('<II', obj.data, offset + 8)
              for command, offset, _ in obj.commands if command == 0x32]
    require(len(values) == 1, 'Expected one object build-version command')
    return values[0]


def disassembler():
    selected = shutil.which('llvm-objdump')
    if selected:
        return selected
    for candidate in ('/opt/homebrew/opt/llvm/bin/llvm-objdump',
                      '/usr/local/opt/llvm/bin/llvm-objdump'):
        if Path(candidate).is_file():
            return candidate
    result = subprocess.run(['xcrun', '--find', 'llvm-objdump'], text=True, capture_output=True)
    require(result.returncode == 0 and result.stdout.strip(),
            'llvm-objdump is required for actual native live-log instruction checks')
    return result.stdout.strip()


def check_instructions(path, tool):
    command = [tool, '--disassemble', '--no-show-raw-insn', str(path)]
    text = subprocess.check_output(command, text=True)
    count = 0
    for line in text.splitlines():
        match = re.match(r'^\s*[0-9a-fA-F]+:\s+(.+)$', line)
        if match is None:
            continue
        instruction = match[1].split('//', 1)[0].split(';', 1)[0].strip()
        mnemonic = instruction.split(None, 1)[0].lower()
        require(mnemonic not in ('msr', 'tlbi', 'at', 'dc', 'ic', 'smstart', 'smstop', '<unknown>') and
                not mnemonic.startswith('.'),
                'Unexpected control/cache/unknown instruction in ' + path.name + ': ' + instruction)
        require(not mnemonic.startswith('f') and
                re.search(r'\b[vqdshbzp](?:[12]?[0-9]|3[01])(?:\.[0-9]*[bhsd])?\b', instruction) is None,
                'FP/SIMD instruction/register in ' + path.name + ': ' + instruction)
        count += 1
    require(count, 'No instructions decoded from live-log object: ' + path.name)
    return {'command': command, 'instruction_count': count,
            'disassembly_sha256': hashlib.sha256(text.encode()).hexdigest(),
            'fp_simd_found': False, 'direct_translation_control_writes_found': False}


def verify(tree, config, native_path):
    audit = load_audit()
    builder = load_builder()
    output = tree / 'build/tb710fu-live-log'
    package = tree / 'kits/tb710fu/Tb710fuLiveLog'
    archive = output / 'libTb710fuLiveLog.a'
    report_path = output / 'build-report.json'
    report = json.loads(report_path.read_text())
    device_commit = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
    display_build_label = 'src-' + device_commit[:8] + '-ramoops'
    require(report['device_commit'] == device_commit and
            report['display_build_label'] == display_build_label,
            'Live-log display label is not bound to the actual device commit')
    require(report['archive_sha256'] == digest(archive), 'Live-log archive/report mismatch')
    require(report['framework_revision'] == LOCK['unixnu']['commit'] and
            report['xnu_revision'] == LOCK['xnu_commit'], 'Wrong live-log source revisions')
    require(report['recipe_sha256'] == digest(ROOT / 'scripts/build-tb710fu-live-log.py'),
            'Live-log archive recipe changed')
    require(report.get('dual_source_log_contract') == builder.DUAL_SOURCE_LOG_CONTRACT,
            'Live-log dual-source capture/export contract differs')
    expected_objects = {name + '.o' for name in SOURCES}
    require(set(report['objects_sha256']) == expected_objects and
            report['archive_members'] == [name + '.o' for name in SOURCES],
            'Incomplete/reordered live-log source set')
    members_list = [(name, data) for name, data in audit.ar_members(archive.read_bytes())
                    if not name.startswith('__.SYMDEF')]
    require([name for name, _ in members_list] == [name + '.o' for name in SOURCES],
            'Unexpected/duplicate live-log archive members')
    members = dict(members_list)
    records = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    startup = records / 'osfmk/DEVELOPMENT/startup.o'
    startup_command_path = startup.with_suffix('.o.json')
    startup_command = json.loads(startup_command_path.read_text().strip().rstrip(','))
    require(report['compile_record_sha256'] == digest(startup_command_path),
            'Live-log compile replay is not bound to the actual startup record')
    require(report['kernel_target_verifier_sha256'] == digest(tree / 'scripts/kernel_target.py'),
            'Live-log target verifier changed')
    target_deployment = deployment(audit.MachO(startup.read_bytes()))
    expected_sources = {str((package / (name + '.c')).relative_to(tree))
                        for name in SOURCES[:4]}
    expected_sources.update(str(path.relative_to(tree)) for path in package.glob('*.h'))
    require(set(report['sources']) == expected_sources, 'Incomplete live-log source/header provenance')
    for relative, expected in report['sources'].items():
        path = tree / relative
        original = ROOT / 'package/Tb710fuLiveLog' / path.name
        require(digest(path) == expected == digest(original), 'Live-log source hash changed: ' + relative)
    manifest_path = ROOT / 'diagnostics/early-report/qr-sources.json'
    manifest = json.loads(manifest_path.read_text())
    qr = report['qr_upstream']
    require(qr['repository'] == manifest['repository'] == builder.QR_REPOSITORY and
            qr['commit'] == manifest['commit'] == builder.QR_COMMIT and
            qr['manifest_sha256'] == digest(manifest_path), 'Fixed Mu QR source identity changed')
    require(qr['source_specific_flags'] == list(builder.PINNED_QR_FLAGS) and
            qr['source_specific_flags_reason'] == builder.PINNED_QR_FLAGS_REASON,
            'Fixed Mu source-specific compile flags changed')
    expected_qr = {item['path']: item['sha256'] for item in manifest['files']}
    require(set(qr['files']) == set(expected_qr) == builder.QR_PATHS,
            'Incomplete fixed Mu QR sources')
    for relative, expected in expected_qr.items():
        require(qr['files'][relative]['sha256'] == expected == digest(output / 'upstream' / relative),
                'Fixed Mu source copy changed: ' + relative)
    font = ROOT / 'diagnostics/early-report/Font.h'
    require(report['font']['sha256'] == digest(font) == builder.FONT_SHA256 ==
            digest(output / 'include/Font.h'), 'Licensed live-log font changed')
    for relative, expected in report['generated_include_sha256'].items():
        require(digest(output / relative) == expected, 'Generated QR include changed: ' + relative)
    builder.PACKAGE, builder.OUT = package, output
    tool = disassembler()
    instruction_checks = {}
    for name in SOURCES:
        member = name + '.o'
        data = members[member]
        require(hashlib.sha256(data).hexdigest() == report['objects_sha256'][member] ==
                digest(output / member), 'Live-log archive member mismatch: ' + member)
        obj = audit.MachO(data)
        require(obj.kind == 1 and obj.subtype & 255 == 2 and deployment(obj) == target_deployment,
                'Live-log member target differs from actual XNU startup: ' + member)
        source = (package / (name + '.c') if name in SOURCES[:4] else
                  output / 'upstream' / builder.QR_BASE / (name + '.c'))
        require(report['objects'][name + '.c'] == report['objects_sha256'][member],
                'Live-log source/object provenance mismatch: ' + member)
        command_path = output / (name + '.command.json')
        require(report['compile_commands_sha256'][name + '.c'] == digest(command_path),
                'Live-log command record changed: ' + name)
        command = json.loads(command_path.read_text())
        require(command['directory'] == startup_command['directory'] and
                command['arguments'] == builder.compile_command(startup_command, source, output / member,
                                                                qr=name in QR_OBJECTS,
                                                                display_build_label=display_build_label),
                'Live-log command does not replay the actual XNU C command: ' + name)
        dep = output / (name + '.d')
        inputs_path = output / (name + '.dependencies.json')
        require(report['dependency_records_sha256'][name + '.c'] == digest(dep) and
                report['included_inputs_records_sha256'][name + '.c'] == digest(inputs_path),
                'Live-log dependency records changed: ' + name)
        inputs = builder.dependency_inputs(dep, command['directory'])
        require(inputs == json.loads(inputs_path.read_text()) and str(source.resolve()) in inputs,
                'Live-log actual included inputs changed: ' + name)
        if name in QR_OBJECTS:
            for symbol in obj.nlists():
                if symbol['kind'] & 1 and symbol['kind'] & 14 == 14 and not symbol['kind'] & 0xe0:
                    require(symbol['name'].startswith(('_tb_live_', '_tb710fu_')),
                            'Unprefixed QR global definition: ' + symbol['name'])
        instruction_checks[member] = check_instructions(output / member, tool)
    live_xnu = audit.MachO(members['LiveXnu.o'])
    strong_definition(live_xnu, '_tb710fu_live_log_putc')
    adapter_source_calls = {name: {'symbol': strong_import(live_xnu, name),
                                  'branches': branch_relocations(live_xnu, name)}
                            for name in ADAPTER_SOURCE_IMPORTS}
    checkpoint_display_calls = {name: {'symbol': strong_import(live_xnu, name),
                                       'branches': branch_relocations(live_xnu, name)}
                                for name in CHECKPOINT_DISPLAY_IMPORTS}
    require(any(call['caller'] == '_checkpoint_publish' for call in
                checkpoint_display_calls['_tb_live_display_scroll_log']['branches']),
            'Synchronous checkpoint renderer must draw the captured scrollback')
    require(any(call['caller'] == '_checkpoint_publish' for call in
                checkpoint_display_calls['_tb_live_display_checkpoint_qr']['branches']),
            'Synchronous checkpoint QR must use the bottom-right layout')
    require(any(call['caller'] in {'_checkpoint_status', '_checkpoint_publish'} for call in
                checkpoint_display_calls['_tb_live_display_checkpoint_status']['branches']),
            'Synchronous checkpoint status must stay in the text pane')
    display = audit.MachO(members['LiveDisplay.o'])
    for name in CHECKPOINT_DISPLAY_IMPORTS:
        strong_definition(display, name)
    checkpoint_worker_calls = {name: {'symbol': strong_import(live_xnu, name),
                                      'branches': branch_relocations(live_xnu, name)}
                               for name in CHECKPOINT_WORKER_IMPORTS}
    require(any(call['caller'] == '_live_start_worker' for call in
                checkpoint_worker_calls['_kernel_thread_start']['branches']) and
            all(any(call['caller'] == '_live_worker' for call in
                    checkpoint_worker_calls[name]['branches'])
                for name in CHECKPOINT_WORKER_IMPORTS[1:]),
            'Checkpoint QR carousel worker must be created and use the bounded timer wait path')
    adapter_snapshot_calls = adapter_source_calls['_tb710fu_ramoops_snapshot_copy']['branches']
    require(any(call['caller'] == '_tb710fu_live_log_putc' for call in
                adapter_source_calls['_tb710fu_live_capture_putc']['branches']),
            'XNU console provider has no actual independent-ring capture branch')
    protocol = audit.MachO(members['LiveProtocol.o'])
    for name in ADAPTER_SOURCE_IMPORTS[1:]:
        strong_definition(protocol, name)
    hooks = {}
    for relative, names in HOOK_OBJECTS.items():
        path = records / relative
        command_path = path.with_suffix('.o.json')
        command = json.loads(command_path.read_text().strip().rstrip(','))
        require(hook_macro_selected(command['arguments'], 'XNU_TB710FU_LIVE_LOG'),
                'Actual native hook macro was not selected: ' + relative)
        obj = audit.MachO(path.read_bytes())
        imports = {}
        for name in names:
            imports[name] = {'symbol': strong_import(obj, name),
                             'branches': branch_relocations(obj, name)}
        hooks[relative] = {'object_sha256': digest(path), 'compile_record_sha256': digest(command_path),
                           'strong_hook_imports': imports}
        if relative in CAPTURE_SOURCE_FUNCTIONS:
            hooks[relative]['capture_source'] = capture_source_evidence(command, CAPTURE_SOURCE_FUNCTIONS[relative])
    arm_calls = hooks['osfmk/DEVELOPMENT/arm_init.o']['strong_hook_imports']
    require(all(any(call['caller'] == '_arm_init' for call in arm_calls[name]['branches'])
                for name in ('_tb710fu_live_log_init', '_tb710fu_live_log_freeze')),
            'Live-log init/freeze branches must be in actual arm_init')
    startup_checkpoint_calls = hooks['osfmk/DEVELOPMENT/startup.o']['strong_hook_imports']
    kalloc_checkpoint_calls = hooks['osfmk/DEVELOPMENT/kalloc.o']['strong_hook_imports']
    sampled = '_tb710fu_live_log_checkpoint_sampled'
    immediate = '_tb710fu_live_log_checkpoint'
    require(any(call['caller'] == '_kernel_startup_initialize_upto'
                for call in startup_checkpoint_calls[sampled]['branches']) and
            any(call['caller'] == '_kalloc_type_distribute_zone_for_type'
                for call in kalloc_checkpoint_calls[sampled]['branches']),
            'Hot startup callback and kalloc helper need actual sampled branches')
    require(not any(call['caller'] == '_kalloc_type_distribute_zone_for_type'
                    for call in kalloc_checkpoint_calls[immediate]['branches']),
            'The fixed kalloc helper must not bypass checkpoint pacing')
    lockdown_checkpoint_sites = {}
    for relative, caller, minimum in (
        ('osfmk/DEVELOPMENT/arm_vm_init.o', '_arm_vm_prot_finalize', 8),
        ('osfmk/DEVELOPMENT/machine_routines.o', '_machine_lockdown', 4),
    ):
        imports = hooks[relative]['strong_hook_imports']
        count = sum(call['caller'] == caller for call in imports[immediate]['branches'])
        require(count >= minimum, 'Missing inner lockdown checkpoint branches: ' + caller)
        lockdown_checkpoint_sites[caller] = count
    arm_sampled = hooks['osfmk/DEVELOPMENT/arm_vm_init.o']['strong_hook_imports'][sampled]['branches']
    progress_callers = {'_arm_vm_prot_finalize', '_arm_vm_page_granular_prot',
                        '_arm_vm_page_granular_helper'}
    require(any(call['caller'] in progress_callers
                for call in arm_sampled),
            'PRELINKTEXT RNX progress needs an actual sampled checkpoint branch')
    arm_active = hooks['osfmk/DEVELOPMENT/arm_vm_init.o']['strong_hook_imports']['_tb710fu_live_log_active']['branches']
    require(any(call['caller'] in progress_callers for call in arm_active),
            'PRELINKTEXT RNX progress must retain the live-log runtime gate')
    arm_immediate = hooks['osfmk/DEVELOPMENT/arm_vm_init.o']['strong_hook_imports'][immediate]['branches']
    require(sum(call['caller'] in progress_callers for call in arm_immediate) >= 12,
            'PRELINKTEXT RNX edge path needs immediate table-selection and batch checkpoints')
    console_calls = hooks['osfmk/DEVELOPMENT/serial_console.o']['strong_hook_imports']
    # The normal _cnputc tap predates LiveLog and alone proves nothing about
    # acquisition's disableConsoleOutput branch. Require the live-active gate
    # in a real disabled-path provider or its actual inlined callers as well.
    require(any(call['caller'] in DISABLED_CAPTURE_CALLERS
                for call in console_calls['_tb710fu_live_log_active']['branches']),
            'Disabled-console capture has no actual live-active branch')
    xnu_calls = console_calls['_tb710fu_live_log_putc']['branches']
    require(any(call['caller'] in DISABLED_CAPTURE_CALLERS | {'__cnputc'} for call in xnu_calls),
            'XNU capture needs actual normal and disabled console branches')
    pe_calls = hooks['pexpert/DEVELOPMENT/pe_kprintf.o']['strong_hook_imports']
    require({'_serial_putc', '_serial_putc_crlf'} <= {
                call['caller'] for call in pe_calls['_tb710fu_live_log_putc']['branches']},
            'XNU capture needs both actual direct PE serial-provider branches')
    hooks['osfmk/DEVELOPMENT/serial_console.o']['disabled_capture_live_gate'] = True
    ramoops = records / 'osfmk/DEVELOPMENT/tb710fu_ramoops.o'
    ramoops_provider = strong_definition(audit.MachO(ramoops.read_bytes()), '_tb710fu_ramoops_snapshot_copy')
    image = audit.MachO(native_path.read_bytes())
    require(image.kind == 2 and image.subtype & 255 == 2,
            'Live-log integration requires actual native arm64e MH_EXECUTE')
    definitions = {name: strong_definition(image, name) for name in FINAL_SYMBOLS}
    link = records / 'mach.development.bcm2837.link/mach.development.bcm2837.linkarchives'
    link_text = link.read_text()
    selected = str(config.get('XNU_EXTRA_LD_ARCHIVES', '')).replace('{tree}', str(tree))
    require(force_load_count(link_text, archive) == force_load_count(selected, archive) == 1,
            'Live-log must be selected and actually force-loaded once')
    return {'schema_version': 1, 'profile': LOCK['os_profile'],
            'device_commit': device_commit, 'display_build_label': display_build_label,
            'framework_revision': LOCK['unixnu']['commit'], 'xnu_revision': LOCK['xnu_commit'],
            'native_kernel_sha256': digest(native_path), 'archive_sha256': digest(archive),
            'archive_compile_report_sha256': digest(report_path),
            'archive_objects_sha256': report['objects_sha256'],
            'source_inputs_sha256': report['sources'], 'qr_manifest_sha256': digest(manifest_path),
            'font_sha256': digest(font), 'hook_objects': hooks,
            'ramoops_snapshot_object_sha256': digest(ramoops),
            'ramoops_snapshot_provider': ramoops_provider,
            'adapter_snapshot_calls': adapter_snapshot_calls, 'native_definitions': definitions,
            'adapter_source_calls': adapter_source_calls,
            'checkpoint_display_calls': checkpoint_display_calls,
            'checkpoint_worker_calls': checkpoint_worker_calls,
            'dual_source_log_contract': builder.DUAL_SOURCE_LOG_CONTRACT,
            'object_instruction_checks': instruction_checks,
            'linkarchives_sha256': digest(link), 'linkarchives': link_text,
            'archive_force_load_count': 1, 'linked_into_kernel': True,
            'live_log_sources_compiled': True,
            'runtime_boot_argument': 'tb710fu-live-log=1 tb710fu-live-checkpoints=1',
            'synchronous_checkpoint_sites_verified': True,
            'lockdown_checkpoint_sites': lockdown_checkpoint_sites,
            'runtime_qualified': False, 'hardware_boot_verified': False,
            'qr_hardware_display_verified': False}
