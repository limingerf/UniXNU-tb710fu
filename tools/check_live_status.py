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
import zlib


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--dependency-dir', type=Path, required=True)
    p.add_argument('--stage', type=int, default=0)
    p.add_argument('--width', type=int, default=640)
    p.add_argument('--height', type=int, default=480)
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
    width,height=args.width,args.height
    fb,stride=0xd5100000,width*4
    rows=96+121*min(5,(height-112)//121,(width-32)//121) if args.stage>=27 else 48
    uc.mem_map(fb, (stride * height+4095)&~4095)
    uc.mem_write(sym['diag_last_stage'],struct.pack('<Q',args.stage))
    uc.mem_write(state + 0x138, struct.pack('<4Q', fb, stride, width, height))
    uc.mem_write(state + 0x170, struct.pack('<Q', physical))
    stack=sym['diag_stack_end']-16384
    msg=0xfffffff00fe00000
    uc.mem_map(msg,4096)
    message=b'XNU enter: vm/set_mmu_ttb_alternate#1' if args.stage==47 else b'XNU enter: arm_vm_init'
    uc.mem_write(msg,message+b'\0')
    stop=msg+4092
    uc.reg_write(r.UC_ARM64_REG_X0, state)
    uc.reg_write(r.UC_ARM64_REG_X1, msg)
    uc.reg_write(r.UC_ARM64_REG_SP, stack + 16368)
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
    writes=[0]
    def store(machine,kind,address,n,value,_):
        assert fb<=address and address+n<=fb+rows*stride
        writes[0]+=1
    uc.hook_add(UC_HOOK_MEM_WRITE,store,begin=fb,end=fb+stride*height-1)
    uc.emu_start(sym['diag_live_text'],stop,timeout=55000000,count=180000000)
    assert uc.reg_read(r.UC_ARM64_REG_PC)==stop
    assert uc.reg_read(r.UC_ARM64_REG_SP)==stack+16368
    assert cp[(3,0,2,0,0)]==original[(3,0,2,0,0)] and cp[(3,0,2,0,2)]==original[(3,0,2,0,2)]
    assert len(switches)==2 and switches[-1]==original[(3,0,2,0,0)]
    table_pa=switches[0]
    desc=struct.unpack('<Q',bytes(uc.mem_read(table_pa+((fb>>30)&511)*8,8)))[0]
    assert desc==(table_pa+4096)|3
    leaf=struct.unpack('<Q',bytes(uc.mem_read(table_pa+4096+((fb>>21)&511)*8,8)))[0]
    assert leaf==(fb&~0x1fffff)|0x6000000000070d
    expected={}
    for pa in range(fb&~0x1fffff,fb+rows*stride,0x200000):
        expected[(pa>>21)&511]=pa|0x6000000000070d
    leaves=struct.unpack('<512Q',bytes(uc.mem_read(table_pa+4096,4096)))
    assert {i:v for i,v in enumerate(leaves) if v}==expected
    roots=struct.unpack('<512Q',bytes(uc.mem_read(table_pa,4096)))
    assert {i:v for i,v in enumerate(roots) if v}=={(fb>>30)&511:(table_pa+4096)|3}
    assert writes[0]
    pixels=bytes(uc.mem_read(fb,rows*stride))
    assert b'\xee\xee\xee\xff' in pixels
    if args.stage>=27:
        def value(name):return struct.unpack('<Q',bytes(uc.mem_read(sym[name],8)))[0]
        assert value('diag_test_qr_status')==0 and value('diag_live_phase')==0
        assert value('diag_report_phase')==0  # A checkpoint is not an exception.
        frame=bytes(uc.mem_read(value('diag_test_frame'),752))
        text=bytes(uc.mem_read(value('diag_test_text'),value('diag_test_text_length')))
        assert b'XNU LIVE CHECKPOINT (continues)' in text and message in text and b'ESR=' not in text
        assert frame[:3]==b'OS\x02' and struct.unpack_from('<I',frame,748)[0]==zlib.crc32(frame[:748])
        envelope=frame[28:28+struct.unpack_from('<I',frame,20)[0]]
        assert struct.unpack_from('<I',frame,24)[0]==zlib.crc32(envelope)
        nl,ml=struct.unpack_from('<HH',envelope,6)
        assert envelope[26:26+nl]==b'TB710FU-live-stage.log'
        assert envelope[26+nl+ml:]==text
        assert struct.unpack_from('<I',envelope,14)[0]==zlib.crc32(text)
        bitmap=bytes(uc.mem_read(value('diag_test_bitmap'),121*121*4))
        stem='live-stage-'+str(args.stage)+'-'+str(width)
        (args.build/(stem+'-frame.bin')).write_bytes(frame)
        (args.build/(stem+'.log')).write_bytes(text)
        (args.build/(stem+'-qr.rgb')).write_bytes(b''.join(bitmap[i+2:i+3]+bitmap[i+1:i+2]+bitmap[i:i+1] for i in range(0,len(bitmap),4)))
        # Save the actual painted crop for independent decode and visual QA.
        crop=bytes(uc.mem_read(fb,rows*stride))
        (args.build/(stem+'-screen.rgb')).write_bytes(b''.join(crop[i+2:i+3]+crop[i+1:i+2]+crop[i:i+1] for i in range(0,len(crop),4)))
    report={'result':'PASS','original_TCR_and_TTBR0_restored':True,'original_page_tables_modified':False,
            'new_framebuffer_mapping_descriptors_checked':True,'writes_limited_to_top_rows':rows,'width':width,'stage':args.stage,
            'normal_path_QR_with_no_exception':args.stage>=27,
            'actual_text_machine_code_executed':True,'relocations_not_preapplied_by_test':True,
            'limits':'register transitions modeled, not actual hardware MMU/cache validation'}
    (args.build/('live-status-'+str(args.stage)+'-'+str(width)+'.json')).write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    main()
