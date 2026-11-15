"""Execute actual AARCH64 report/QR machine code at a physical alias.

Uses flat Unicorn memory and explicit ELF relative fixups, not hardware MMU or
device caches. Verifies runtime QR generation missed by host-native-only tests.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--dependency-dir', type=Path, required=True)
    p.add_argument('--check-alignment', action='store_true', help='Model natural alignment for reporter data/stack accesses with MMU off')
    p.add_argument('--expect-alignment-fault', action='store_true', help='Regression: old reporter must fail this check')
    args = p.parse_args()
    sys.path.insert(0, str(args.dependency_dir))
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
    from unicorn import arm64_const as r
    build = json.loads((args.build / 'build-report.json').read_text())
    sym = {k: int(v, 16) for k, v in build['symbols'].items()}
    base = int(build['segment_va'], 16)
    physical = 0x8c8828000
    delta = physical - base
    blob = bytearray((args.build / 'report.bin').read_bytes())
    for at in range(sym['diag_rela_start'] - base, sym['diag_rela_end'] - base, 24):
        offset, info, addend = struct.unpack_from('<QQq', blob, at)
        assert info & 0xffffffff == 1027
        struct.pack_into('<Q', blob, offset - base, (addend + delta) & ((1 << 64) - 1))
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(physical, build['segment_bytes'])
    uc.mem_write(physical, bytes(blob))
    framebuffer, width, height, stride = 0xd5100000, 3200, 2000, 12800
    uc.mem_map(framebuffer, (height * stride + 4095) & ~4095)
    state = physical + sym['diag_state'] - base
    syndrome = 0xf200b713 if build.get('checkpoint_installed', True) else 0x96000004
    uc.mem_write(state + 0x100, struct.pack('<Q', syndrome))
    uc.mem_write(state + 0x110, struct.pack('<Q', int(build['checkpoint_va'], 16)))
    uc.mem_write(state + 0x128, struct.pack('<Q', 1))
    uc.mem_write(state + 0x138, struct.pack('<4Q', framebuffer, stride, width, height))
    if 'diag_last_stage' in sym:
        uc.mem_write(physical + sym['diag_last_stage'] - base, struct.pack('<Q', 26))
    stop = physical + build['segment_bytes'] - 4
    uc.reg_write(r.UC_ARM64_REG_SP, physical + sym['diag_stack_end'] - base)
    uc.reg_write(r.UC_ARM64_REG_X0, state)
    uc.reg_write(r.UC_ARM64_REG_X30, stop)
    faults = []
    if args.check_alignment:
        def access(machine, kind, address, length, value, _):
            if address % min(length, 8):
                faults.append({'pc': hex(machine.reg_read(r.UC_ARM64_REG_PC)),
                               'address': hex(address), 'access_bytes': length})
                machine.emu_stop()
        uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, access,
                    begin=physical, end=physical + build['segment_bytes'] - 1)
    uc.emu_start(physical + sym['diag_build_report'] - base, stop, timeout=45000000, count=180000000)
    if faults:
        if not args.expect_alignment_fault:
            raise SystemExit('Unaligned reporter access: ' + json.dumps(faults))
        result = {'result': 'EXPECTED-FAIL', 'alignment_fault': faults[0],
                  'limits': 'Natural-alignment hook models MMU-off Device access; not a full hardware model'}
        (args.build / 'alignment-regression.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
        return
    if args.expect_alignment_fault:
        raise SystemExit('Expected old-reporter alignment failure did not occur')
    if uc.reg_read(r.UC_ARM64_REG_PC) != stop:
        raise SystemExit('Actual AARCH64 report did not return within bounded instruction/time budget')
    def value(name):
        return struct.unpack('<Q', bytes(uc.mem_read(physical + sym[name] - base, 8)))[0]
    if value('diag_test_qr_status') != 0 or value('diag_report_phase') != 6:
        raise SystemExit('AARCH64 QR did not reach READY')
    frame = bytes(uc.mem_read(value('diag_test_frame'), 752))
    bitmap = bytes(uc.mem_read(value('diag_test_bitmap'), 121 * 121 * 4))
    text = bytes(uc.mem_read(value('diag_test_text'), value('diag_test_text_length')))
    assert (b'before arm_vm_init' in text) if build.get('checkpoint_installed', True) else (b'XNU EARLY EXCEPTION' in text)
    assert frame[:3] == b'OS\x02'
    (args.build / 'runtime-frame.bin').write_bytes(frame)
    (args.build / 'runtime-qr.rgb').write_bytes(b''.join(bitmap[i+2:i+3] + bitmap[i+1:i+2] + bitmap[i:i+1] for i in range(0, len(bitmap), 4)))
    (args.build / 'runtime-text.log').write_bytes(text)
    report = {'result': 'PASS', 'actual_AARCH64_report_and_QrEncodeData_executed': True,
              'resolution': [width, height], 'phase': 'QR READY', 'text_bytes': len(text),
              'frame_sha256': hashlib.sha256(frame).hexdigest(),
              'heap_bytes': value('diag_test_heap_used'),
              'natural_alignment_checked': args.check_alignment,
              'limits': 'flat physical memory; real device/MMU/cache behavior not verified'}
    (args.build / 'runtime-check.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
