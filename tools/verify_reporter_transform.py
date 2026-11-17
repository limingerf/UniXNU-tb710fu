"""Verify the bounded QR-reporter transformation after exact prelink checks.

This module never substitutes a post-link hash into a CI prelink report. Its
caller must first validate the original kernel and all archive/integration
inputs. No compiler, key, firmware, device or fetched checkout is touched.
"""
import hashlib
from pathlib import Path
import re
import struct

from verify_storage_integration import hook_symbol, load_audit

ROOT = Path(__file__).resolve().parents[1]
RUNTIME_SYMBOLS = (
    '_tb710fu_cdc_log_tap_putc', '_tb710fu_cdc_log_tap_copy', '_tb710fu_cdc_log_tap_consume',
    '_tb710fu_cdc_log_ring_append', '_tb710fu_cdc_log_ring_copy', '_tb710fu_cdc_log_ring_consume',
    '_tb710fu_cdc_classify_setup', '_tb710fu_cdc_preflight',
    '_tb710fu_dwc3_start', '_tb710fu_dwc3_poll', '_tb710fu_dwc3_stop',
    '_tb710fu_cdc_start', '_native_source_storage_start',
)
TRACE_SPECS = tuple(('_arm_init', name, 1) for name in (
    '_PE_init_platform', '_configure_timer_apple_regs', '_wfe_timeout_configure',
    '_wfe_timeout_init', '_configure_misc_apple_boot_args', '_configure_misc_apple_regs',
    '_ml_parse_cpu_topology', '_thread_bootstrap', '_cpu_bootstrap',
    '_rtclock_early_init', '_kernel_startup_bootstrap', '_cpu_init', '_processor_bootstrap',
    '_arm_vm_init',
)) + tuple(('_arm_vm_init', name, occurrence) for name, occurrence in (
    ('_early_random', 1), ('_SecureDTLookupEntry', 1), ('_SecureDTGetProperty', 1),
    ('_SecureDTGetProperty', 2), ('_arm_vm_outside_add', 1),
    ('_getsegdatafromheader', 1), ('_vm_set_page_size', 1),
    ('_arm_vm_prot_init', 1), ('_qsort', 1), ('_set_mmu_ttb_alternate_probed', 1),
    ('_set_mmu_ttb', 1), ('_pmap_bootstrap', 1), ('__enable_preemption', 1),
)) + (('_arm_init', '_tb710fu_ramoops_init', 1),)
PLATFORM_TRACE_SPECS = tuple(('_arm_init', name, occurrence) for name, occurrence in (
    ('_PE_consistent_debug_inherit', 1), ('_serial_init', 1),
    ('_kernel_startup_initialize_upto', 1), ('_PE_create_console', 1),
    ('_PE_init_printf', 1), ('_cpu_machine_idle_init', 1),
    ('_PE_init_platform', 2), ('_pe_arm_debug_init_early', 1),
    ('_ml_map_cpu_pio', 1), ('_cpu_timebase_init', 1),
    ('_kptimer_curcpu_up', 1), ('_PE_init_cpu', 1),
    ('_apt_msg_init', 1), ('_apt_msg_init_cpu', 1),
    ('_fiq_context_init', 1), ('_early_random', 1), ('_machine_startup', 1),
))


def require(condition, message):
    if not condition:
        raise ValueError('Reporter transform: ' + message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def number(value):
    require(isinstance(value, (int, str)) and not isinstance(value, bool), 'invalid integer')
    return int(value, 0) if isinstance(value, str) else value


def align16(value):
    return (value + 0x3fff) & ~0x3fff


def direct_branch(word, site, link=False):
    require(word >> 26 == (0b100101 if link else 0b000101), 'expected direct ' + ('BL' if link else 'B'))
    displacement = word & 0x3ffffff
    if displacement & (1 << 25):
        displacement -= 1 << 26
    return site + displacement * 4


def strong_code(image, name):
    symbols = hook_symbol(image, name)
    require(len(symbols) == 1, 'expected one definition of ' + name)
    symbol = symbols[0]
    require(symbol['kind'] & 14 == 14 and symbol['kind'] & 1 and not symbol['desc'] & 0x80,
            'expected strong external code: ' + name)
    require(0 < symbol['section'] <= len(image.sections), 'definition has no section: ' + name)
    section = image.sections[symbol['section'] - 1]
    address = number(symbol['value'])
    require(section['name'] == '__text' and section['flags'] & 0x80000000 and
            section['va'] <= address and address + 4 <= section['va'] + section['size'],
            'definition is not backed by executable text: ' + name)
    image.offset(address, 4)
    return symbol, section, address


def preserved_runtime_definitions(original, candidate):
    """Check actual original/final native symbols and code, independently of reports."""
    audit = load_audit()
    roots = [audit.MachO(data) for data in (original, candidate)]
    require(all(m.kind == 12 and 'com.apple.kernel' in m.entries for m in roots), 'missing native fileset entry')
    require(roots[0].entries == roots[1].entries, 'fileset entries changed')
    images = [audit.MachO(data, m.entries['com.apple.kernel'][1])
              for data, m in zip((original, candidate), roots)]
    require(all(m.kind == 2 and m.subtype & 0xffffff == 2 for m in images), 'native entry is not arm64e executable')
    all_addresses = sorted(images[0].symbols())
    verified, total = [], 0
    for name in RUNTIME_SYMBOLS:
        before, section, address = strong_code(images[0], name)
        after, after_section, after_address = strong_code(images[1], name)
        require(before == after and section == after_section and address == after_address,
                'runtime definition metadata changed: ' + name)
        end = min([v for v in all_addresses if address < v <= section['va'] + section['size']]
                  or [section['va'] + section['size']])
        length = end - address
        require(length >= 4, 'runtime definition has no complete instruction: ' + name)
        first, second = images[0].read(address, length), images[1].read(address, length)
        require(len(first) == len(second) == length and first == second,
                'runtime code changed: ' + name)
        verified.append({'name': name, 'address': hex(address), 'bytes': length, 'sha256': sha(first)})
        total += length
    return {'strong_runtime_definitions_verified': len(verified),
            'preserved_runtime_code_bytes': total, 'runtime_definitions': verified}


def _verify(original, candidate, report):
    require(isinstance(report, dict), 'missing transformation report')
    require(report.get('original_sha256') == sha(original), 'original kernel hash differs')
    require(report.get('kernel_sha256') == sha(candidate) and report.get('kernel_bytes') == len(candidate),
            'candidate kernel hash/size differs')
    require(report.get('checkpoint_installed') is False and report.get('panic_hook_installed') is False,
            'explicit stop/panic hooks are not accepted')
    require(report.get('reporter_tool_sha256') == sha((ROOT / 'tools/prepare_early_report.py').read_bytes()),
            'reporter tool source differs')
    audit = load_audit()
    old, new = audit.MachO(original), audit.MachO(candidate)
    require(old.kind == new.kind == 12 and old.subtype == new.subtype and old.subtype & 0xffffff == 2,
            'expected identical arm64e fileset architecture')
    require(not any(s['name'] in ('__KDIAG', '__KDATRW') for s in old.segments), 'input already has reporter segments')
    count, command_bytes = struct.unpack_from('<II', original, 16)
    command_at = 32 + command_bytes
    require(command_at + 144 <= len(original) and not any(original[command_at:command_at + 144]),
            'new segment commands do not occupy original zero padding')
    require(struct.unpack_from('<II', candidate, 16) == (count + 2, command_bytes + 144),
            'expected exactly two added load commands')
    require(len(new.commands) == len(old.commands) + 2 and new.commands[-2:] ==
            [(0x19, command_at, 72), (0x19, command_at + 72, 72)], 'added commands are not two terminal segments')
    base = align16(max(s['va'] + s['size'] for s in old.segments if s['filesize']))
    fileoff = align16(len(original))
    code, state = report['code_segment'], report['state_segment']
    code_size, state_size, state_file_size = number(code['bytes']), number(state['bytes']), number(state['filesize'])
    require(code_size > 0 and code_size % 0x4000 == 0 and 0 < state_file_size <= state_size and
            state_size == align16(state_file_size), 'invalid reporter segment sizes')
    require(code['name'] == '__KDIAG' and state['name'] == '__KDATRW' and
            number(code['va']) == base and number(state['va']) == base + code_size and
            number(code['fileoff']) == fileoff and number(state['fileoff']) == fileoff + code_size and
            number(code['filesize']) == code_size and code['protection'] == 'RX' and state['protection'] == 'RW-NX',
            'reporter segment address/file layout differs')
    require(number(report['segment_va']) == base and report['segment_fileoff'] == fileoff and
            report['segment_bytes'] == code_size + state_size and
            report['payload_bytes'] == code_size + state_file_size,
            'aggregate reporter geometry differs')
    commands = (struct.pack('<II16sQQQQiiII', 0x19, 72, b'__KDIAG', base, code_size, fileoff, code_size, 5, 5, 0, 0) +
                struct.pack('<II16sQQQQiiII', 0x19, 72, b'__KDATRW', base + code_size, state_size,
                            fileoff + code_size, state_file_size, 3, 3, 0, 0))
    require(candidate[command_at:command_at + 144] == commands, 'actual segment protections/layout differ')
    require(len(candidate) == fileoff + code_size + state_size, 'unexpected appended bytes')
    require(not any(candidate[len(original):fileoff]) and
            not any(candidate[fileoff + code_size + state_file_size:]), 'nonzero append padding')
    payload = candidate[fileoff:fileoff + code_size + state_file_size]
    require(re.fullmatch(r'[0-9a-f]{64}', report.get('payload_sha256', '')) and
            sha(payload) == report['payload_sha256'], 'reporter payload hash differs')
    symbols = {k: number(v) for k, v in report['symbols'].items()}
    require(symbols['diag_image_base'] == base and symbols['diag_data_start'] == base + code_size,
            'reporter symbol layout differs')

    def code_target(name):
        target = symbols[name]
        require(base <= target and target + 4 <= base + code_size and target % 4 == 0,
                'reporter branch target is outside RX: ' + name)
        return target

    require('com.apple.kernel' in old.entries, 'original has no native entry')
    native = audit.MachO(original, old.entries['com.apple.kernel'][1])
    names, definitions = {}, []
    for item in native.nlists():
        if not item['kind'] & 0xe0 and item['kind'] & 0x0e == 0x0e:
            names.setdefault(item['name'], set()).add(item['value'])
            definitions.append(item)

    def named(name):
        require(len(names.get(name, set())) == 1, 'missing/ambiguous original symbol: ' + name)
        return next(iter(names[name]))

    parent_ranges = {}

    def trace_parent_range(name):
        # Bind the claimed caller to executable bytes in the original native
        # image. The next distinct symbol in that same section bounds its code;
        # report metadata alone cannot establish the pre-scheduler boundary.
        if name not in parent_ranges:
            address = named(name)
            sections = {item['section'] for item in definitions
                        if item['name'] == name and item['value'] == address}
            require(len(sections) == 1, 'ambiguous trace parent section: ' + name)
            section_number = next(iter(sections))
            require(0 < section_number <= len(native.sections), 'invalid trace parent section: ' + name)
            section = native.sections[section_number - 1]
            require(section['name'] == '__text' and section['flags'] & 0x80000000 and
                    section['va'] <= address and address + 4 <= section['va'] + section['size'],
                    'trace parent is not executable text: ' + name)
            end = min([item['value'] for item in definitions
                       if item['section'] == section_number and address < item['value'] <= section['va'] + section['size']]
                      or [section['va'] + section['size']])
            require(address % 4 == end % 4 == 0 and end > address,
                    'invalid trace parent instruction range: ' + name)
            native.offset(address, end - address)
            parent_ranges[name] = address, end
        return parent_ranges[name]

    spans = report['patch_spans']
    require(isinstance(spans, list) and spans, 'missing patch spans')
    decoded, intervals = {}, [(16, 24), (command_at, command_at + 144)]
    rebuilt = bytearray(original)
    struct.pack_into('<II', rebuilt, 16, count + 2, command_bytes + 144)
    rebuilt[command_at:command_at + 144] = commands
    for span in spans:
        require(set(span) == {'offset', 'before_hex', 'after_hex', 'kind'}, 'unexpected patch span fields')
        kind = span['kind']
        require(kind in ('arm_init', 'call_trace', 'compact_branch', 'compact_code'), 'unknown patch span kind')
        offset = number(span['offset'])
        before, after = bytes.fromhex(span['before_hex']), bytes.fromhex(span['after_hex'])
        require(before.hex() == span['before_hex'] and after.hex() == span['after_hex'], 'noncanonical patch bytes')
        require(offset >= 0 and offset % 4 == 0 and len(before) == len(after) and len(after) % 4 == 0 and
                0 < len(after) <= (0x180 if kind == 'compact_code' else 4) and
                offset + len(after) <= len(original), 'invalid patch span bounds')
        require(all(offset + len(after) <= low or high <= offset for low, high in intervals), 'overlapping patch spans')
        require(original[offset:offset + len(before)] == before and candidate[offset:offset + len(after)] == after,
                'patch span before/after bytes differ')
        intervals.append((offset, offset + len(after)))
        decoded[(kind, offset)] = (before, after)
        rebuilt[offset:offset + len(after)] = after
    require(bytes(rebuilt) == candidate[:len(original)], 'unreported change in original kernel bytes')
    used = set()

    def patch(kind, address):
        offset = old.offset(address, 4)
        key = kind, offset
        require(key in decoded and key not in used, 'missing/duplicate derived patch span: ' + kind)
        used.add(key)
        return decoded[key]

    arm = number(report['arm_init_va'])
    require(arm == named('_arm_init'), 'arm_init address differs')
    before, after = patch('arm_init', arm)
    require(struct.unpack('<I', before)[0] == 0xd503237f and
            direct_branch(struct.unpack('<I', after)[0], arm) == code_target('diag_init'),
            'arm_init does not preserve the PACIBSP-entry contract')
    traces = report['call_trace']
    platform = report.get('platform_startup_trace', False)
    require(isinstance(platform, bool), 'invalid platform trace selector')
    expected_traces = TRACE_SPECS + (PLATFORM_TRACE_SPECS if platform else ())
    require(isinstance(traces, list) and len(traces) == len(expected_traces), 'expected the complete bounded trace list')
    for index, (item, expected) in enumerate(zip(traces, expected_traces)):
        require((item['parent'], item['name'], item['occurrence']) == expected and item['label'] == 'diag_trace_' + str(index),
                'unexpected trace function/order')
        site, callee = number(item['site']), number(item['callee'])
        require(callee == named(item['name']), 'trace callee is not the original function')
        parent_start, parent_end = trace_parent_range(item['parent'])
        require(parent_start <= site and site + 4 <= parent_end and site % 4 == 0,
                'trace site is outside original parent function: ' + item['parent'])
        calls = []
        for address in range(parent_start, parent_end, 4):
            word = struct.unpack('<I', native.read(address, 4))[0]
            if word >> 26 == 0b100101 and direct_branch(word, address, True) == callee:
                calls.append(address)
        occurrence = item['occurrence']
        require(0 < occurrence <= len(calls) and calls[occurrence - 1] == site,
                'trace site is not the original direct BL occurrence')
        before, after = patch('call_trace', site)
        require(direct_branch(struct.unpack('<I', before)[0], site, True) == callee and
                direct_branch(struct.unpack('<I', after)[0], site) == code_target(item['label']),
                'trace branch does not wrap the original BL')
        if platform:
            tail = item['name'] in ('_arm_vm_init', '_set_mmu_ttb_alternate', '_machine_startup')
            require(item.get('before_stage') == index * 2 + 1 and
                    item.get('after_stage') == (None if tail else index * 2 + 2) and
                    item.get('original_LR_tail_transfer') is tail,
                    'platform trace stage/continuation contract differs')
    compact = report['compact_checkpoints']
    require(isinstance(compact, list) and [item['stage'] for item in compact] == [1, 2, 3], 'expected exactly three compact checkpoints')
    for item in compact:
        stage, site, target, cave = item['stage'], number(item['site']), number(item['target']), number(item['cave'])
        before, after = patch('compact_branch', site)
        require(direct_branch(struct.unpack('<I', after)[0], site) == cave, 'compact entry branch misses its cave')
        cave_before, code_bytes = patch('compact_code', cave)
        require(not any(cave_before) and len(code_bytes) == item['code_bytes'] and sha(code_bytes) == item['code_sha256'],
                'compact code size/hash or original zero padding differs')
        require(len(code_bytes) >= 4 and direct_branch(struct.unpack_from('<I', code_bytes, len(code_bytes) - 4)[0],
                cave + len(code_bytes) - 4) == target, 'compact code does not resume original execution')
        original_word = struct.unpack('<I', before)[0]
        if stage == 1:
            require(target == named('_start_first_cpu') and direct_branch(original_word, site) == target,
                    'first compact checkpoint does not preserve start_first_cpu')
        else:
            replay = 0xd5181000 if stage == 2 else 0xd2a00600
            require(target == site + 4 and original_word == replay and len(code_bytes) >= 8 and
                    struct.unpack_from('<I', code_bytes, len(code_bytes) - 8)[0] == replay,
                    'compact checkpoint does not replay the original instruction')
    require(used == set(decoded), 'extra reported patch spans are not accepted')
    runtime = preserved_runtime_definitions(original, candidate)
    return {'stage': 'verified-postlink-early-qr-transform', 'original_prelink_sha256': sha(original),
            'diagnostic_kernel_sha256': sha(candidate), 'diagnostic_kernel_bytes': len(candidate),
            'patch_spans_verified': len(spans), 'trace_sites_verified': len(traces),
            'compact_checkpoints_verified': len(compact), 'prefix_changes_bounded': True,
            'reporter_payload_sha256': sha(payload), 'normal_kernel_execution_preserved': True,
            'hardware_verified': False, **runtime}


def verify_reporter_transform(original: bytes, candidate: bytes, report: dict) -> dict:
    """Validate one exact original -> diagnostic kernel transformation.

    Raises ValueError for malformed evidence or any unreported/unsafe delta.
    This is post-link proof, not a replacement for original CI prelink checks.
    """
    try:
        return _verify(original, candidate, report)
    except (AssertionError, KeyError, StopIteration, struct.error, UnicodeDecodeError,
            TypeError, IndexError, OverflowError) as error:
        raise ValueError('Reporter transform: malformed/incomplete Mach-O or report evidence') from error
