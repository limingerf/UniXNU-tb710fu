"""Execute actual AARCH64 text/replay code with modeled MMU roots and counter.

Checks drawing bounds, full GOP descriptors, restoration before every wait,
normal continuation and opt-in export pause. This is not a hardware MMU model.
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
    p.add_argument('--paused-export', action='store_true')
    args = p.parse_args()
    sys.path.insert(0, str(args.dependency_dir))
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
    from unicorn import arm64_const as r
    build = json.loads((args.build / 'build-report.json').read_text())
    sym = {k: int(v, 16) for k, v in build['symbols'].items()}
    base, physical = int(build['segment_va'], 16), 0x8c8828000
    size = build['segment_bytes']
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    memory = ctypes.create_string_buffer(size)
    blob = (args.build / 'report.bin').read_bytes()
    ctypes.memmove(ctypes.addressof(memory), blob, len(blob))
    uc.mem_map_ptr(base, size, 7, ctypes.addressof(memory))
    uc.mem_map_ptr(physical, size, 7, ctypes.addressof(memory))
    state = sym['diag_state']
    width, height = args.width, args.height
    fb, stride = 0xd5100000, width * 4
    qr = args.stage == 47
    scale = min(8, (height-128)//121, (width-32)//121)
    rows = 112+121*scale if qr else 48
    uc.mem_map(fb, (stride*height+4095)&~4095)
    uc.mem_write(sym['diag_last_stage'], struct.pack('<Q', args.stage))
    uc.mem_write(sym['diag_export_mode'], struct.pack('<Q', int(args.paused_export and args.stage!=0)))
    if args.stage==0:
        bootva,bootpa=base+size+0x10000,physical+size+0x10000
        uc.mem_map(bootva,4096)
        uc.mem_write(bootva+0x6c,b'-v tb-report-export=1 serial=3\0' if args.paused_export else b'-v serial=3\0')
        uc.mem_write(state+0x130,struct.pack('<Q',bootpa))
        uc.mem_write(state+0x158,struct.pack('<3Q',base,physical,0x80000000))
    uc.mem_write(state+0x138, struct.pack('<4Q', fb, stride, width, height))
    uc.mem_write(state+0x170, struct.pack('<Q', physical))
    stack = sym['diag_stack_end']-16384
    msg = 0xfffffff00fe00000
    uc.mem_map(msg, 4096)
    message = b'XNU enter: vm/set_mmu_ttb_alternate#1' if qr else b'XNU enter: arm_vm_init'
    uc.mem_write(msg, message+b'\0')
    stop = msg+4092
    uc.reg_write(r.UC_ARM64_REG_X0, state)
    uc.reg_write(r.UC_ARM64_REG_X1, msg)
    uc.reg_write(r.UC_ARM64_REG_SP, stack+16368)
    uc.reg_write(r.UC_ARM64_REG_X30, stop)
    tcr, ttbr0 = (3,0,2,0,2), (3,0,2,0,0)
    cp = {(3,3,0,0,1):4<<16, tcr:0x2b0003519, ttbr0:0x8c9000000,
          (3,0,2,0,1):0x8c9010000, (3,0,1,0,0):0xfc54799d,
          (3,3,14,0,0):1000}
    original = dict(cp)
    switches, waits, clock = [], [], [0]
    def value(name):
        return struct.unpack('<Q', bytes(uc.mem_read(sym[name],8)))[0]
    def instruction(machine, pc, length, _):
        word = struct.unpack('<I',bytes(machine.mem_read(pc,4)))[0]
        field = ((word>>19)&3,(word>>16)&7,(word>>12)&15,(word>>8)&15,(word>>5)&7)
        number = word&31
        reg = getattr(r,'UC_ARM64_REG_X'+str(number)) if number<31 else None
        if word&0xfff00000==0xd5300000:
            if field==(3,3,14,0,1):
                assert cp[ttbr0]==original[ttbr0] and cp[tcr]==original[tcr]
                waits.append(value('diag_replay_draws'))
                if args.paused_export and value('diag_replay_draws')>=20:
                    machine.emu_stop()
                    return
                clock[0]+=500
                result=clock[0]
            else:
                result=cp.get(field,0)
            machine.reg_write(reg,result)
            machine.reg_write(r.UC_ARM64_REG_PC,pc+4)
        elif word&0xfff00000==0xd5100000:
            cp[field]=machine.reg_read(reg) if reg else 0
            if field==ttbr0:switches.append(cp[field])
            machine.reg_write(r.UC_ARM64_REG_PC,pc+4)
        elif word&~31==0xd50b7a20 or word==0xd508871f:
            machine.reg_write(r.UC_ARM64_REG_PC,pc+4)
    for at in range(0,len(blob)-3,4):
        word=struct.unpack_from('<I',blob,at)[0]
        if word&0xfff00000 in (0xd5300000,0xd5100000) or word&~31==0xd50b7a20 or word==0xd508871f:
            uc.hook_add(UC_HOOK_CODE,instruction,begin=base+at,end=base+at)
    def encoding_root(machine,pc,length,_):
        assert cp[ttbr0]==original[ttbr0] and cp[tcr]==original[tcr]
    uc.hook_add(UC_HOOK_CODE,encoding_root,begin=sym['diag_prepare_replay'],end=sym['diag_prepare_replay'])
    writes=[0]
    def store(machine,kind,address,n,value,_):
        assert fb<=address and address+n<=fb+rows*stride
        writes[0]+=1
    uc.hook_add(UC_HOOK_MEM_WRITE,store,begin=fb,end=fb+stride*height-1)
    uc.emu_start(sym['diag_live_text'],stop,timeout=150000000,count=900000000)
    draws=value('diag_replay_draws')
    if args.stage==0:
        assert value('diag_export_mode')==int(args.paused_export)
    if args.paused_export and qr:
        assert qr and draws==20 and uc.reg_read(r.UC_ARM64_REG_PC)!=stop
        assert stack<=uc.reg_read(r.UC_ARM64_REG_SP)<stack+16384
    else:
        assert uc.reg_read(r.UC_ARM64_REG_PC)==stop
        assert uc.reg_read(r.UC_ARM64_REG_SP)==stack+16368
        assert draws==(16 if qr else 0)
    assert cp[ttbr0]==original[ttbr0] and cp[tcr]==original[tcr]
    assert len(switches)%2==0 and switches[-1]==original[ttbr0]
    table_pa=switches[0]
    assert all(v==table_pa if i%2==0 else v==original[ttbr0] for i,v in enumerate(switches))
    roots=struct.unpack('<512Q',bytes(uc.mem_read(table_pa,4096)))
    assert {i:v for i,v in enumerate(roots) if v}=={(fb>>30)&511:(table_pa+4096)|3}
    expected={}
    for pa in range(fb&~0x1fffff,fb+rows*stride,0x200000):expected[(pa>>21)&511]=pa|0x6000000000070d
    leaves=struct.unpack('<512Q',bytes(uc.mem_read(table_pa+4096,4096)))
    assert {i:v for i,v in enumerate(leaves) if v}==expected
    assert writes[0] and b'\xee\xee\xee\xff' in bytes(uc.mem_read(fb,rows*stride))
    mode=int(args.paused_export)
    if qr:
        assert value('diag_replay_ready')==4 and value('diag_test_qr_status')==0
        assert value('diag_report_phase')==0
        assert waits and max(waits)==draws
        text=bytes(uc.mem_read(value('diag_test_text'),value('diag_test_text_length')))
        assert (b'intentional export pause' in text)==args.paused_export
        assert message in text and b'ESR=' not in text
        assert b'OLD_TTBR1=00000008c9010000' in text
        stem='replay-'+str(mode)
        (args.build/(stem+'.log')).write_bytes(text)
        for i in range(4):
            frame=bytes(uc.mem_read(sym['diag_replay_frames']+i*752,752))
            assert frame[:3]==b'OS\x02' and struct.unpack_from('<I',frame,8)[0]==i
            assert struct.unpack_from('<I',frame,748)[0]==zlib.crc32(frame[:748])
            (args.build/(stem+'-frame-'+str(i)+'.bin')).write_bytes(frame)
            bitmap=bytes(uc.mem_read(sym['diag_replay_pixels']+i*121*121*4,121*121*4))
            (args.build/(stem+'-qr-'+str(i)+'.rgb')).write_bytes(b''.join(bitmap[j+2:j+3]+bitmap[j+1:j+2]+bitmap[j:j+1] for j in range(0,len(bitmap),4)))
        crop=bytes(uc.mem_read(fb,rows*stride))
        (args.build/(stem+'-screen.rgb')).write_bytes(b''.join(crop[i+2:i+3]+crop[i+1:i+2]+crop[i:i+1] for i in range(0,len(crop),4)))
    report={'result':'PASS','stage':args.stage,'paused_export':args.paused_export,
            'width':width,'rows':rows,'QR_draws':draws,'counter_waits_with_original_roots':len(waits),
            'normal_path_returns':not (args.paused_export and qr),
            'actual_boot_arg_parser_checked':args.stage==0,'original_TCR_and_TTBR0_restored':True,
            'all_GOP_mapping_descriptors_checked':True,'framebuffer_writes_bounded':True,
            'encoding_uses_original_roots':True,'no_QR_for_earlier_stage':not qr,
            'limits':'System registers and elapsed counter modeled; real hardware MMU/cache/camera not verified'}
    (args.build/('replay-check-'+str(args.stage)+'-'+str(mode)+'-'+str(width)+'.json')).write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    main()
