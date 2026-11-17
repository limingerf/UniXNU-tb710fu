"""Mutation tests for exact prelink -> QR post-link evidence boundaries."""
from copy import deepcopy
import hashlib
from pathlib import Path
import struct
import unittest

import verify_reporter_transform as verify


def sha(data):
    return hashlib.sha256(data).hexdigest()


def branch(site, target, link=False):
    return struct.pack('<I', (0x94000000 if link else 0x14000000) | (((target - site) // 4) & 0x3ffffff))


def fixture(platform=False):
    """Synthetic arm64e fileset with real sections/symbols and bounded hooks."""
    base, native_at, text, symoff, stroff = 0x100000, 0x1000, 0x2000, 0x5000, 0x6000
    original = bytearray(0x8000)
    header = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0xc0000002, 12, 2, 136, 0, 0)
    segment = struct.pack('<II16s4Q4I', 0x19, 72, b'__TEXT', base, len(original), 0, len(original), 7, 5, 0, 0)
    entry = struct.pack('<IIQQII', 0x80000035, 64, base + native_at, native_at, 32, 0)
    original[:168] = header + segment + entry + b'com.apple.kernel\0'.ljust(32, b'\0')
    names = {name: base + 0x2800 + i * 16 for i, name in enumerate(verify.RUNTIME_SYMBOLS)}
    specs = verify.TRACE_SPECS + (verify.PLATFORM_TRACE_SPECS if platform else ())
    names.update({name: base + 0x3000 + i * 16 for i, name in enumerate(dict.fromkeys(item[1] for item in specs))})
    names['_arm_init'] = base + 0x2040
    names['_fixture_after_arm_init'] = base + 0x2180
    names['_arm_vm_init'] = base + 0x2200
    names['_fixture_after_arm_vm_init'] = base + 0x2300
    names['_start_first_cpu'] = base + 0x3400
    names['_fixture_scheduled_path'] = base + 0x3500
    strings, table = b'\0', bytearray()
    for name, address in names.items():
        table += struct.pack('<IBBHQ', len(strings), 15, 1, 0, address)
        strings += name.encode() + b'\0'
        original[address - base:address - base + 16] = b'\x1f\x20\x03\xd5' * 3 + b'\xc0\x03\x5f\xd6'
    native_header = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0xc0000002, 2, 2, 176, 0, 0)
    native_segment = struct.pack('<II16s4Q4I', 0x19, 152, b'__TEXT_EXEC', base + native_at,
                                 0x7000, native_at, 0x7000, 5, 5, 1, 0)
    section = struct.pack('<16s16sQQ8I', b'__text', b'__TEXT_EXEC', base + text,
                          0x2000, text, 2, 0, 0, 0x80000400, 0, 0, 0)
    symtab = struct.pack('<6I', 2, 24, symoff, len(names), stroff, len(strings))
    original[native_at:native_at + 208] = native_header + native_segment + section + symtab
    original[symoff:symoff + len(table)] = table
    original[stroff:stroff + len(strings)] = strings
    arm = names['_arm_init']
    original[arm - base:arm - base + 4] = struct.pack('<I', 0xd503237f)
    trace_items = []
    next_site = {'_arm_init': base + 0x2080, '_arm_vm_init': base + 0x2240}
    for index, (parent, name, occurrence) in enumerate(specs):
        site = next_site[parent]
        next_site[parent] += 4
        original[site - base:site - base + 4] = branch(site, names[name], True)
        tail = name in ('_arm_vm_init', '_set_mmu_ttb_alternate', '_machine_startup')
        trace_items.append({'parent': parent, 'name': name, 'occurrence': occurrence,
                            'site': hex(site), 'callee': hex(names[name]), 'label': 'diag_trace_' + str(index),
                            'before_stage': index * 2 + 1, 'after_stage': None if tail else index * 2 + 2,
                            'original_LR_tail_transfer': tail})
    compact = []
    for stage in (1, 2, 3):
        site, cave = base + 0x2020 + (stage - 1) * 4, base + 0x2400 + (stage - 1) * 0x180
        target = names['_start_first_cpu'] if stage == 1 else site + 4
        original[site - base:site - base + 4] = branch(site, target) if stage == 1 else struct.pack('<I', 0xd5181000 if stage == 2 else 0xd2a00600)
        code = b'\x1f\x20\x03\xd5'
        if stage > 1:
            code += original[site - base:site - base + 4]
        code += branch(cave + len(code), target)
        compact.append({'stage': stage, 'site': hex(site), 'target': hex(target), 'cave': hex(cave),
                        'code_bytes': len(code), 'code_sha256': sha(code), '_code': code})
    candidate = bytearray(original)
    code_base, fileoff, code_size, state_file_size, state_size = base + 0x8000, 0x8000, 0x4000, 0x400, 0x4000
    payload = b'\x1f\x20\x03\xd5' * (code_size // 4) + bytes(state_file_size)
    candidate += payload + bytes(state_size - state_file_size)
    struct.pack_into('<II', candidate, 16, 4, 280)
    commands = struct.pack('<II16sQQQQiiII', 0x19, 72, b'__KDIAG', code_base, code_size, fileoff, code_size, 5, 5, 0, 0)
    commands += struct.pack('<II16sQQQQiiII', 0x19, 72, b'__KDATRW', code_base + code_size, state_size, fileoff + code_size, state_file_size, 3, 3, 0, 0)
    candidate[168:312] = commands
    symbols = {'diag_init': hex(code_base), 'diag_image_base': hex(code_base), 'diag_data_start': hex(code_base + code_size)}
    symbols.update({'diag_trace_' + str(i): hex(code_base + 0x100 + i * 4) for i in range(len(trace_items))})
    spans = []

    def patch(kind, address, content):
        offset = address - base
        spans.append({'kind': kind, 'offset': offset, 'before_hex': original[offset:offset + len(content)].hex(), 'after_hex': content.hex()})
        candidate[offset:offset + len(content)] = content

    patch('arm_init', arm, branch(arm, code_base))
    for item in trace_items:
        site = int(item['site'], 16)
        patch('call_trace', site, branch(site, int(symbols[item['label']], 16)))
    for item in compact:
        site, cave = int(item['site'], 16), int(item['cave'], 16)
        patch('compact_branch', site, branch(site, cave))
        patch('compact_code', cave, item.pop('_code'))
    report = {'original_sha256': sha(original), 'kernel_sha256': sha(candidate), 'kernel_bytes': len(candidate),
              'checkpoint_installed': False, 'panic_hook_installed': False,
              'reporter_tool_sha256': sha((Path(__file__).parent / 'prepare_early_report.py').read_bytes()),
              'code_segment': {'name': '__KDIAG', 'va': hex(code_base), 'bytes': code_size, 'fileoff': fileoff, 'filesize': code_size, 'protection': 'RX'},
              'state_segment': {'name': '__KDATRW', 'va': hex(code_base + code_size), 'bytes': state_size, 'fileoff': fileoff + code_size, 'filesize': state_file_size, 'protection': 'RW-NX'},
              'segment_va': hex(code_base), 'segment_fileoff': fileoff, 'segment_bytes': code_size + state_size,
              'payload_bytes': len(payload), 'payload_sha256': sha(payload), 'symbols': symbols,
              'patch_spans': spans, 'arm_init_va': hex(arm), 'call_trace': trace_items, 'compact_checkpoints': compact,
              'platform_startup_trace': platform}
    return bytes(original), bytes(candidate), report


class ReporterTransformTests(unittest.TestCase):
    def setUp(self):
        self.original, self.candidate, self.report = fixture()

    def check(self):
        return verify.verify_reporter_transform(self.original, self.candidate, self.report)

    def rebind(self):
        self.report['original_sha256'] = sha(self.original)
        self.report['kernel_sha256'] = sha(self.candidate)
        self.report['kernel_bytes'] = len(self.candidate)

    def reject(self, text=None):
        with self.assertRaisesRegex(ValueError, text or 'Reporter transform:'):
            self.check()

    def test_positive_bounded_transform_preserves_13_real_definitions(self):
        result = self.check()
        self.assertEqual(result['strong_runtime_definitions_verified'], 13)
        self.assertEqual(result['trace_sites_verified'], 28)
        self.assertGreaterEqual(result['preserved_runtime_code_bytes'], 13 * 4)

    def test_platform_extension_is_complete_and_stops_at_machine_startup_entry(self):
        self.original, self.candidate, self.report = fixture(platform=True)
        result = self.check()
        self.assertEqual(result['trace_sites_verified'], 45)
        self.assertEqual(self.report['call_trace'][-1]['name'], '_machine_startup')
        self.assertEqual(self.report['call_trace'][-1]['before_stage'], 89)
        self.assertIsNone(self.report['call_trace'][-1]['after_stage'])

    def test_platform_trace_cannot_be_downgraded_or_claim_a_machine_startup_return(self):
        self.original, self.candidate, self.report = fixture(platform=True)
        self.report['platform_startup_trace'] = False
        self.reject('complete bounded trace')
        self.report['platform_startup_trace'] = True
        self.report['call_trace'][-1]['after_stage'] = 90
        self.reject('continuation contract')

    def test_original_and_candidate_hashes_are_both_required(self):
        self.report['original_sha256'] = '0' * 64
        self.reject('original kernel hash')
        self.report['original_sha256'] = sha(self.original)
        self.report['kernel_sha256'] = '0' * 64
        self.reject('candidate kernel hash')

    def test_unreported_prefix_change_is_rejected_even_with_updated_hash(self):
        data = bytearray(self.candidate); data[0x4000] = 1
        self.candidate = bytes(data); self.rebind()
        self.reject('unreported change')

    def test_overlapping_or_extra_spans_are_rejected(self):
        self.report['patch_spans'].append(deepcopy(self.report['patch_spans'][0]))
        self.reject('overlapping')

    def test_forged_before_bytes_are_rejected(self):
        self.report['patch_spans'][0]['before_hex'] = '00000000'
        self.reject('before/after')

    def test_unreported_or_changed_executable_segment_is_rejected(self):
        data = bytearray(self.candidate); struct.pack_into('<I', data, 168 + 60, 7)
        self.candidate = bytes(data); self.rebind()
        self.reject('protections/layout')

    def test_append_padding_and_payload_hash_are_checked(self):
        data = bytearray(self.candidate); data[-1] = 1
        self.candidate = bytes(data); self.rebind()
        self.reject('padding')
        self.original, self.candidate, self.report = fixture()
        self.report['payload_sha256'] = '0' * 64
        self.reject('payload hash')

    def test_trace_callee_cannot_be_relabelled(self):
        self.report['call_trace'][0]['callee'] = self.report['call_trace'][1]['callee']
        self.reject('not the original function')

    def test_trace_branch_target_is_checked_after_valid_hash_and_delta(self):
        span = self.report['patch_spans'][1]
        offset = span['offset']; data = bytearray(self.candidate)
        data[offset:offset + 4] = branch(int(self.report['call_trace'][0]['site'], 16), 0x108000)
        span['after_hex'] = data[offset:offset + 4].hex()
        self.candidate = bytes(data); self.rebind()
        self.reject('wrap the original BL')

    def test_trace_cannot_claim_an_arm_init_parent_for_a_scheduled_path_bl(self):
        # Keep hashes, delta spans, original callee and wrapper target valid,
        # while moving a claimed arm_init trace into another real function.
        item = self.report['call_trace'][0]
        old_site, new_site = int(item['site'], 16), 0x103504
        original, candidate = bytearray(self.original), bytearray(self.candidate)
        candidate[old_site - 0x100000:old_site - 0x100000 + 4] = original[old_site - 0x100000:old_site - 0x100000 + 4]
        before = branch(new_site, int(item['callee'], 16), True)
        after = branch(new_site, int(self.report['symbols'][item['label']], 16))
        original[new_site - 0x100000:new_site - 0x100000 + 4] = before
        candidate[new_site - 0x100000:new_site - 0x100000 + 4] = after
        span = next(s for s in self.report['patch_spans'] if s['kind'] == 'call_trace' and s['offset'] == old_site - 0x100000)
        span.update(offset=new_site - 0x100000, before_hex=before.hex(), after_hex=after.hex())
        item['site'] = hex(new_site)
        self.original, self.candidate = bytes(original), bytes(candidate); self.rebind()
        self.reject('outside original parent function')

    def test_trace_actual_direct_bl_occurrence_is_recomputed(self):
        # An earlier unmodified call to the same target makes the still-bound
        # trace site occurrence 2. All report hashes/deltas remain consistent.
        item = self.report['call_trace'][0]
        original, candidate = bytearray(self.original), bytearray(self.candidate)
        earlier = 0x102044
        content = branch(earlier, int(item['callee'], 16), True)
        original[earlier - 0x100000:earlier - 0x100000 + 4] = content
        candidate[earlier - 0x100000:earlier - 0x100000 + 4] = content
        self.original, self.candidate = bytes(original), bytes(candidate); self.rebind()
        self.reject('not the original direct BL occurrence')

    def test_compact_code_must_return_and_replay_original_instruction(self):
        item = self.report['compact_checkpoints'][1]
        span = next(s for s in self.report['patch_spans'] if s['kind'] == 'compact_code' and s['offset'] == int(item['cave'], 16) - 0x100000)
        code = bytearray.fromhex(span['after_hex']); code[-8:-4] = b'\x1f\x20\x03\xd5'
        span['after_hex'] = code.hex(); item['code_sha256'] = sha(code)
        candidate = bytearray(self.candidate); candidate[span['offset']:span['offset'] + len(code)] = code
        self.candidate = bytes(candidate); self.rebind()
        self.reject('replay the original instruction')

    def test_explicit_stop_and_wrong_tool_source_are_rejected(self):
        self.report['checkpoint_installed'] = True
        self.reject('stop/panic')
        self.report['checkpoint_installed'] = False
        self.report['reporter_tool_sha256'] = '0' * 64
        self.reject('tool source')

    def test_weak_runtime_definition_is_rejected_even_when_prefix_is_preserved(self):
        original, candidate = bytearray(self.original), bytearray(self.candidate)
        struct.pack_into('<H', original, 0x5000 + 6, 0x80)
        struct.pack_into('<H', candidate, 0x5000 + 6, 0x80)
        self.original, self.candidate = bytes(original), bytes(candidate); self.rebind()
        self.reject('strong external code')


if __name__ == '__main__':
    unittest.main()
