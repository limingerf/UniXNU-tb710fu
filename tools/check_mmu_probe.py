"""Emulate the assembled probe instructions to check aliases, registers and writes.

SCTLR reads/writes are explicitly modeled; this does not validate real MMU
translation, cache behavior or PAC. Only the original PACIBSP replay is skipped.
Unicorn 2.1.4 may be supplied from a task-local --dependency-dir.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel', type=Path, required=True)
    p.add_argument('--report', type=Path, required=True)
    p.add_argument('--dependency-dir', type=Path, required=True)
    args = p.parse_args()
    sys.path.insert(0, str(args.dependency_dir))
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
    from unicorn import arm64_const as reg
    data = args.kernel.read_bytes()
    report = json.loads(args.report.read_text())
    assert hashlib.sha256(data).hexdigest() == report['kernel_sha256']
    segments = []
    offset = 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, size = struct.unpack_from('<II', data, offset)
        if command == 0x19:
            _, va, vs, fileoff, fs = struct.unpack_from('<16sQQQQ', data, offset + 8)
            segments.append((va, min(vs, fs), fileoff))
        offset += size

    def image_bytes(va, size):
        for start, length, fileoff in segments:
            if start <= va and va + size <= start + length:
                at = fileoff + va - start
                return data[at:at + size]
        raise ValueError('Unmapped source VA')

    results = []
    for stage in (3, 4):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        vbase, pbase = 0xfffffff007004000, 0x8c0004000
        cave = int(report['caves'][str(stage)], 16)
        physical = pbase + cave - vbase
        size = report['code_bytes'][str(stage)]
        code = image_bytes(cave, size)
        site = int(report['sites'][str(stage)], 16)
        target = site + 4 if stage == 4 else pbase + site + 4 - vbase
        mapped = set()

        def load(address, blob):
            for page in range(address & ~4095, (address + len(blob) + 4095) & ~4095, 4096):
                if page not in mapped:
                    uc.mem_map(page, 4096)
                    mapped.add(page)
            uc.mem_write(address, blob)

        framebuffer, bootargs = 0xd5100000, pbase + 0x9000000
        load(cave, code)
        load(physical, code)
        load(target, b'\x1f\x20\x03\xd5')
        load(framebuffer, bytes(4096))
        load(bootargs, bytes(4096))
        uc.mem_write(bootargs + 0x28, struct.pack('<Q', framebuffer))
        uc.mem_write(bootargs + 0x38, struct.pack('<Q', 64))
        uc.mem_write(bootargs + 0x48, struct.pack('<Q', 16))
        x0 = vbase + bootargs - pbase if stage == 4 else 0x30d0183d
        for number, value in {0: x0, 20: bootargs, 22: vbase, 23: pbase, 30: 0xfffffff00c28f844}.items():
            uc.reg_write(getattr(reg, 'UC_ARM64_REG_X' + str(number)), value)
        uc.reg_write(reg.UC_ARM64_REG_SP, 0xfffffff00d000000)
        initial = 0x30d0183d
        control = {'value': initial, 'writes': []}
        trace, writes = [], []

        def instruction(machine, address, length, _):
            trace.append(address)
            if address == target:
                machine.emu_stop()
                return
            word = struct.unpack('<I', bytes(machine.mem_read(address, 4)))[0]
            r = getattr(reg, 'UC_ARM64_REG_X' + str(word & 31)) if word & 31 < 31 else None
            if word & ~31 == 0xd5381000:  # MRS SCTLR_EL1
                machine.reg_write(r, control['value'])
                machine.reg_write(reg.UC_ARM64_REG_PC, address + 4)
            elif word & ~31 == 0xd5181000:  # MSR SCTLR_EL1
                control['value'] = machine.reg_read(r)
                control['writes'].append(control['value'])
                machine.reg_write(reg.UC_ARM64_REG_PC, address + 4)
            elif word == 0xd503237f:  # Real PAC behavior is outside this model.
                assert stage == 4 and address == cave + size - 8
                machine.reg_write(reg.UC_ARM64_REG_PC, address + 4)

        def store(machine, access, address, length, value, _):
            writes.append((address, length))

        uc.hook_add(UC_HOOK_CODE, instruction)
        uc.hook_add(UC_HOOK_MEM_WRITE, store)
        uc.emu_start(cave if stage == 4 else physical, 0, count=10000)
        assert trace[-1] == target
        assert control['writes'] == [initial & ~(1 | 4 | 0x1000), initial]
        assert control['value'] == initial
        assert uc.reg_read(reg.UC_ARM64_REG_X0) == (x0 if stage == 4 else 0x300000)
        for number, value in {20: bootargs, 22: vbase, 23: pbase, 30: 0xfffffff00c28f844}.items():
            assert uc.reg_read(getattr(reg, 'UC_ARM64_REG_X' + str(number))) == value
        assert uc.reg_read(reg.UC_ARM64_REG_SP) == 0xfffffff00d000000
        band = stage  # post-MMU band 3, C-entry band 4
        begin, end = framebuffer + band * 64, framebuffer + (band + 1) * 64
        assert writes and all(begin <= a and a + n <= end for a, n in writes)
        color = 0xff00ffff if stage == 3 else 0xffffff00
        assert bytes(uc.mem_read(begin, 64)) == struct.pack('<I', color) * 16
        if stage == 4:
            assert physical + 16 in trace and cave + size - 8 in trace
        results.append({'stage': stage, 'registers_and_stack_preserved': True,
                        'sctlr_restored_in_model': True, 'only_expected_framebuffer_band_written': True,
                        'physical_virtual_alias_path': 'PASS', 'instructions': len(trace)})
    output = {'instruction_model': 'Unicorn 2.1.4', 'result': 'PASS', 'probes': results,
              'limits': 'SCTLR transitions modeled; real page tables, caches and PAC not validated'}
    args.report.with_suffix('.instruction-check.json').write_text(json.dumps(output, indent=2) + '\n')
    print(json.dumps(output, indent=2))


if __name__ == '__main__':
    main()
