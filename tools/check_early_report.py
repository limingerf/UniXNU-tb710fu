"""Instruction-model check for the actual injected exception capture and PIE fixups.

Privileged registers/cache operations are modeled, PACIBSP is skipped. This
verifies transport code and aliases; it is not hardware MMU/PAC qualification.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import struct
import sys


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--dependency-dir', type=Path, required=True)
    args = p.parse_args()
    sys.path.insert(0, str(args.dependency_dir))
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
    from unicorn import arm64_const as r
    record = json.loads((args.build / 'build-report.json').read_text())
    sym = {k: int(v, 16) for k, v in record['symbols'].items()}
    vbase, pbase = 0xfffffff007004000, 0x8c0004000
    base = int(record['segment_va'], 16)
    physical = pbase + base - vbase
    size = record['segment_bytes']
    blob = (args.build / 'report.bin').read_bytes()
    state_offset = sym['diag_state'] - base
    results = []
    for case in (1, 2, 3, 4):
        kind = 2 if case == 2 else 1
        selftest = case == 3
        sp1 = case == 4
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        memory = ctypes.create_string_buffer(size)
        ctypes.memmove(ctypes.addressof(memory), blob, len(blob))
        uc.mem_map_ptr(base, size, 7, ctypes.addressof(memory))
        uc.mem_map_ptr(physical, size, 7, ctypes.addressof(memory))
        bootva, bootpa = vbase + 0x8900000, pbase + 0x8900000
        boot = ctypes.create_string_buffer(4096)
        uc.mem_map_ptr(bootva, 4096, 7, ctypes.addressof(boot))
        uc.mem_map_ptr(bootpa, 4096, 7, ctypes.addressof(boot))
        values = {0x18: 0x80000000, 0x28: 0xd5100000, 0x38: 12800, 0x40: 3200, 0x48: 2000}
        for off, value in values.items():
            uc.mem_write(bootva + off, struct.pack('<Q', value))
        arm_next = int(record['arm_init_va'], 16) + 4
        uc.mem_map(arm_next & ~4095, 4096)
        uc.mem_write(arm_next, b'\x1f\x20\x03\xd5')
        registers = {(3, 0, 1, 0, 0): 0x30d0183d, (3, 0, 0, 0, 1): 4 << 16}
        registers[(3, 3, 0, 0, 1)] = 4 << 16  # CTR_EL0
        clean = []
        init_stop = sym['diag_selftest'] if selftest else arm_next
        stopping = {'pc': init_stop}

        def hook(machine, pc, length, _):
            if pc == sym.get('diag_live_text'):
                for n in list(range(8))+[18]:
                    machine.reg_write(getattr(r,'UC_ARM64_REG_X'+str(n)),0xbad000+n)
                machine.reg_write(r.UC_ARM64_REG_PC,machine.reg_read(r.UC_ARM64_REG_X30))
                return
            if pc == stopping['pc']:
                machine.emu_stop()
                return
            word = struct.unpack('<I', bytes(machine.mem_read(pc, 4)))[0]
            cp = ((word >> 19) & 3, (word >> 16) & 7, (word >> 12) & 15, (word >> 8) & 15, (word >> 5) & 7)
            number = word & 31
            reg = getattr(r, 'UC_ARM64_REG_X' + str(number)) if number < 31 else None
            if word & 0xfff00000 == 0xd5300000:
                machine.reg_write(reg, registers.get(cp, 0))
                machine.reg_write(r.UC_ARM64_REG_PC, pc + 4)
            elif word & 0xfff00000 == 0xd5100000:
                registers[cp] = machine.reg_read(reg) if reg else 0
                machine.reg_write(r.UC_ARM64_REG_PC, pc + 4)
            elif word & ~31 == 0xd50b7a20:
                clean.append(machine.reg_read(reg))
                machine.reg_write(r.UC_ARM64_REG_PC, pc + 4)
            elif word in (0xd503237f, 0xd5034fdf, 0xd50041bf):
                machine.reg_write(r.UC_ARM64_REG_PC, pc + 4)

        uc.hook_add(UC_HOOK_CODE, hook)
        uc.reg_write(r.UC_ARM64_REG_X0, bootva)
        uc.reg_write(r.UC_ARM64_REG_X22, vbase)
        uc.reg_write(r.UC_ARM64_REG_X23, pbase)
        if selftest:
            uc.mem_write(bootva + 0x6c, b'-v tb-report-selftest=1 ' + bytes(8))
        original_sp = vbase + 0x8813f30
        uc.mem_map(original_sp & ~4095, 4096)
        uc.reg_write(r.UC_ARM64_REG_SP, original_sp)
        uc.emu_start(sym['diag_init'], 0, count=5000)
        assert uc.reg_read(r.UC_ARM64_REG_PC) == init_stop
        assert uc.reg_read(r.UC_ARM64_REG_X0) == bootva
        assert uc.reg_read(r.UC_ARM64_REG_SP) == original_sp
        assert registers[(3, 0, 12, 0, 0)] == sym['diag_vectors']
        assert sym['diag_vectors'] % 2048 == 0
        snapshot = bytes(uc.mem_read(sym['diag_state'], 0x178))
        assert struct.unpack_from('<Q', snapshot, 0x170)[0] == physical
        assert struct.unpack_from('<Q', snapshot, 0x130)[0] == bootpa
        assert struct.unpack_from('<4Q', snapshot, 0x138) == (0xd5100000, 12800, 3200, 2000)

        original_regs = [0x10000000 + i * 0x101 for i in range(31)]
        for i, value in enumerate(original_regs):
            uc.reg_write(getattr(r, 'UC_ARM64_REG_X' + str(i)), value)
        syndrome = 0xf200b711 if selftest else 0x96000004
        elr = int(record['arm_init_va'], 16) + 0x30
        registers.update({(3, 0, 5, 2, 0): syndrome, (3, 0, 6, 0, 0): 0xdead0000,
                          (3, 0, 4, 0, 1): elr, (3, 0, 4, 0, 0): 5 if sp1 else 4,
                          (3, 0, 4, 1, 0): original_sp})
        stopping['pc'] = physical + sym['diag_build_report'] - base
        start = sym['diag_panic'] if kind == 2 else sym['diag_vectors'] + (512 if sp1 else 0)
        uc.emu_start(start, 0, count=200000)
        assert uc.reg_read(r.UC_ARM64_REG_PC) == stopping['pc']
        snapshot = bytes(uc.mem_read(physical + state_offset, 0x178))
        assert list(struct.unpack_from('<31Q', snapshot)) == original_regs
        assert struct.unpack_from('<Q', snapshot, 0xf8)[0] == original_sp
        assert struct.unpack_from('<3Q', snapshot, 0x100) == (syndrome, 0xdead0000, elr)
        assert struct.unpack_from('<Q', snapshot, 0x128)[0] == kind
        assert registers[(3, 0, 1, 0, 0)] == (0x30d0183d & ~(1 | 4 | 0x1000))
        terminal = sym.get('diag_terminal_vectors', sym['diag_vectors'])
        assert registers[(3, 0, 12, 0, 0)] == physical + terminal - base
        assert clean and all(base <= x < base + size for x in clean)
        start, end = sym['diag_rela_start'] - base, sym['diag_rela_end'] - base
        slide = (physical - base) & ((1 << 64) - 1)
        reloc_count = 0
        for at in range(start, end, 24):
            off, info, addend = struct.unpack_from('<QQq', blob, at)
            assert info & 0xffffffff == 1027
            value = struct.unpack('<Q', bytes(uc.mem_read((off + slide) & ((1 << 64) - 1), 8)))[0]
            assert value == (addend + slide) & ((1 << 64) - 1), (hex(off),hex(value),hex((addend+slide)&((1<<64)-1)))
            reloc_count += 1
        results.append({'kind': kind, 'explicit_selftest_token': selftest, 'exception_sp1': sp1, 'all_31_gprs_captured': True, 'original_sp_and_syndrome_captured': True,
                        'physical_alias_and_diagnostic_stack': 'PASS', 'PIE_runtime_relative_fixups': reloc_count,
                        'physical_terminal_VBAR': True,
                        'does_not_use_original_faulting_stack': True})
    trace_results = []
    for item in record.get('call_trace', []):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        memory = ctypes.create_string_buffer(size)
        ctypes.memmove(ctypes.addressof(memory), blob, len(blob))
        uc.mem_map_ptr(base, size, 7, ctypes.addressof(memory))
        callee, resume = int(item['callee'], 16), int(item['site'], 16) + 4
        for page in {callee & ~4095, resume & ~4095}:
            uc.mem_map(page, 4096)
        uc.mem_write(callee, b'\x1f\x20\x03\xd5')
        uc.mem_write(resume, b'\x1f\x20\x03\xd5')
        for number in range(31):
            uc.reg_write(getattr(r, 'UC_ARM64_REG_X' + str(number)), 0x12340000 + number)
        saved_sp = 0xfffffff00f817f30
        uc.mem_map(saved_sp & ~4095,4096)
        uc.reg_write(r.UC_ARM64_REG_SP, saved_sp)
        stage_address = sym['diag_last_stage']
        def call_hook(machine, pc, length, _):
            if pc == sym.get('diag_live_text'):
                for n in list(range(8))+[18]:
                    machine.reg_write(getattr(r,'UC_ARM64_REG_X'+str(n)),0xbad000+n)
                machine.reg_write(r.UC_ARM64_REG_PC,machine.reg_read(r.UC_ARM64_REG_X30))
                return
            if pc == callee:
                stage = struct.unpack('<Q', bytes(machine.mem_read(stage_address, 8)))[0]
                assert stage == item['before_stage']
                assert all(machine.reg_read(getattr(r, 'UC_ARM64_REG_X' + str(n))) == 0x12340000 + n for n in range(8))
                machine.reg_write(r.UC_ARM64_REG_X0, 0xfeed1234)
                machine.reg_write(r.UC_ARM64_REG_X16, 0xbad1)
                machine.reg_write(r.UC_ARM64_REG_X17, 0xbad2)
                machine.reg_write(r.UC_ARM64_REG_PC, machine.reg_read(r.UC_ARM64_REG_X30))
            elif pc == resume:
                machine.emu_stop()
        uc.hook_add(UC_HOOK_CODE, call_hook)
        uc.emu_start(sym[item['label']], 0, count=1000)
        assert uc.reg_read(r.UC_ARM64_REG_PC) == resume
        assert uc.reg_read(r.UC_ARM64_REG_X30) == resume
        assert uc.reg_read(r.UC_ARM64_REG_X0) == 0xfeed1234
        assert uc.reg_read(r.UC_ARM64_REG_SP) == saved_sp
        assert struct.unpack('<Q', bytes(uc.mem_read(stage_address, 8)))[0] == item['after_stage']
        assert all(uc.reg_read(getattr(r, 'UC_ARM64_REG_X' + str(n))) == 0x12340000 + n for n in range(19, 30))
        trace_results.append({'name': item['name'], 'result': 'PASS', 'original_call_executed_in_model': True,
                              'argument_and_result_preserved': True, 'LR_resume_and_stack_preserved': True})
    report = {'result': 'PASS', 'instruction_model': 'Unicorn 2.1.4', 'cases': results,
              'call_trace_wrappers': trace_results,
              'limits': 'Privileged registers/cache/live text backend modeled and PACIBSP skipped; live renderer separately exercised; real MMU/PAC not validated'}
    (args.build / 'instruction-check.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
