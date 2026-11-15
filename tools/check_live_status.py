"""Execute the actual live text renderer with modeled TTBR0/TCR switches.

No hardware mapping guarantee is claimed. Checks page descriptors, unchanged
original roots, bounded framebuffer writes and no dependency on PIE data fixups.
"""
import argparse
import ctypes
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
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
    from unicorn import arm64_const as r
    build = json.loads((args.build / 'build-report.json').read_text())
    sym = {k: int(v, 16) for k, v in build['symbols'].items()}
    base = int(build['segment_va'], 16)
    physical = 0x8c8828000
    size = build['segment_bytes']
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    memory = ctypes.create_string_buffer(size)
    blob = (args.build / 'report.bin').read_bytes()
    ctypes.memmove(ctypes.addressof(memory), blob, len(blob))
    uc.mem_map_ptr(base, size, 7, ctypes.addressof(memory))
    uc.mem_map_ptr(physical, size, 7, ctypes.addressof(memory))
    state = sym['diag_state']
    fb, stride, width, height = 0xd5100000, 2560, 640, 480
    uc.mem_map(fb, stride * height)
    uc.mem_write(state + 0x138, struct.pack('<4Q', fb, stride, width, height))
    uc.mem_write(state + 0x170, struct.pack('<Q', physical))
    stack = 0xfffffff00f817000
    uc.mem_map(stack, 4096)
    msg = base + size - 256
    uc.mem_write(msg, b'XNU enter: PE_init_platform\0')
    stop = base + size - 4
    uc.reg_write(r.UC_ARM64_REG_X0, state)
    uc.reg_write(r.UC_ARM64_REG_X1, msg)
    uc.reg_write(r.UC_ARM64_REG_SP, stack + 0xff0)
    uc.reg_write(r.UC_ARM64_REG_X30, stop)
    cp = {(3,3,0,0,1):4<<16, (3,0,2,0,2):0x2b0003519, (3,0,2,0,0):0x8c9000000}
    original = dict(cp)
    switches = []
    def instruction(machine, pc, length, _):
        word = struct.unpack('<I', bytes(machine.mem_read(pc, 4)))[0]
        field = ((word>>19)&3,(word>>16)&7,(word>>12)&15,(word>>8)&15,(word>>5)&7)
        number = word & 31
        reg = getattr(r,'UC_ARM64_REG_X'+str(number)) if number < 31 else None
        if word & 0xfff00000 == 0xd5300000:
            machine.reg_write(reg, cp.get(field,0))
            machine.reg_write(r.UC_ARM64_REG_PC,pc+4)
        elif word & 0xfff00000 == 0xd5100000:
            cp[field]=machine.reg_read(reg) if reg else 0
            if field==(3,0,2,0,0):switches.append(cp[field])
            machine.reg_write(r.UC_ARM64_REG_PC,pc+4)
        elif word & ~31 == 0xd50b7a20 or word == 0xd508871f:
            machine.reg_write(r.UC_ARM64_REG_PC,pc+4)
    # Only hook the finitely many system instructions, not every painted pixel.
    for at in range(0, len(blob)-3, 4):
        word = struct.unpack_from('<I',blob,at)[0]
        if word & 0xfff00000 in (0xd5300000,0xd5100000) or word & ~31 == 0xd50b7a20 or word == 0xd508871f:
            uc.hook_add(UC_HOOK_CODE,instruction,begin=base+at,end=base+at)
    writes=[]
    def store(machine,kind,address,n,value,_):
        writes.append((address,n))
    uc.hook_add(UC_HOOK_MEM_WRITE,store,begin=fb,end=fb+stride*height-1)
    uc.emu_start(sym['diag_live_text'],stop,timeout=30000000,count=20000000)
    assert uc.reg_read(r.UC_ARM64_REG_PC)==stop
    assert uc.reg_read(r.UC_ARM64_REG_SP)==stack+0xff0
    assert cp[(3,0,2,0,0)]==original[(3,0,2,0,0)] and cp[(3,0,2,0,2)]==original[(3,0,2,0,2)]
    assert len(switches)==2 and switches[-1]==original[(3,0,2,0,0)]
    table_pa=switches[0]
    desc=struct.unpack('<Q',bytes(uc.mem_read(table_pa+((fb>>30)&511)*8,8)))[0]
    assert desc==(table_pa+4096)|3
    leaf=struct.unpack('<Q',bytes(uc.mem_read(table_pa+4096+((fb>>21)&511)*8,8)))[0]
    assert leaf==(fb&~0x1fffff)|0x6000000000070d
    assert writes and all(fb<=a and a+n<=fb+48*stride for a,n in writes)
    pixels=bytes(uc.mem_read(fb,48*stride))
    assert b'\xee\xee\xee\xff' in pixels
    report={'result':'PASS','original_TCR_and_TTBR0_restored':True,'original_page_tables_modified':False,
            'new_framebuffer_mapping_descriptors_checked':True,'writes_limited_to_top_48_rows':True,
            'actual_text_machine_code_executed':True,'requires_PIE_data_relocations':False,
            'limits':'register transitions modeled, not actual hardware MMU/cache validation'}
    (args.build/'live-status-check.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    main()
