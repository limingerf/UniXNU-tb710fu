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
    qr = args.stage in (47, 48, 49, 50, 51, 52, 54, 55, 56)
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
    message = {
        47: b'XNU enter: vm/set_mmu_ttb_alternate_probed#1',
        48: b'XNU returned: vm/set_mmu_ttb_alternate_probed#1',
        49: b'XNU enter: vm/set_mmu_ttb#1',
        50: b'XNU returned: vm/set_mmu_ttb#1',
        51: b'XNU enter: vm/pmap_bootstrap#1',
        52: b'XNU returned: vm/pmap_bootstrap#1',
        54: b'XNU returned: vm/_enable_preemption#1',
        55: b'XNU enter: tb710fu_ramoops_init',
        56: b'XNU returned: tb710fu_ramoops_init',
    }.get(args.stage, b'XNU enter: arm_vm_init')
    uc.mem_write(msg, message+b'\0')
    stop = msg+4092
    uc.reg_write(r.UC_ARM64_REG_X0, state)
    uc.reg_write(r.UC_ARM64_REG_X1, msg)
    uc.reg_write(r.UC_ARM64_REG_SP, stack+16368)
    uc.reg_write(r.UC_ARM64_REG_X30, stop)
    tcr, ttbr0 = (3,0,2,0,2), (3,0,2,0,0)
    guarded = args.stage==48
    guard_root,guard_page,guard_marker=0x8c8800000,0x8c8828000,0x93ffff000
    if guarded:
        guard_leaf=(guard_page|(1<<10)|(3<<8)|(1<<7)|(1<<54)|3)
        marker_leaf=(guard_marker|(1<<10)|(3<<8)|(1<<53)|(1<<54)|3)
        uc.mem_write(sym['diag_guard_active'],struct.pack('<I',1))
        uc.mem_write(sym['diag_guard_root_pa'],struct.pack('<Q',guard_root))
        uc.mem_write(sym['diag_guard_l3']+(((guard_page>>12)&0x1ff)*8),struct.pack('<Q',guard_leaf))
        uc.mem_write(sym['diag_guard_l3']+4096+(((guard_marker>>12)&0x1ff)*8),struct.pack('<Q',marker_leaf))
        marker_l1=(guard_marker>>30)&0xff;marker_l2=(guard_marker>>21)&0x1ff
        marker_l3=(guard_marker>>12)&0x1ff
        image_l1=(guard_page>>30)&0xff;image_l2=(guard_page>>21)&0x1ff
        image_l3=(guard_page>>12)&0x1ff
        uc.mem_map(guard_root,0x5000)
        uc.mem_write(guard_root+marker_l1*8,struct.pack('<Q',guard_root+0x1000|3))
        uc.mem_write(guard_root+0x1000+marker_l2*8,struct.pack('<Q',guard_root+0x2000|3))
        uc.mem_write(guard_root+0x2000+marker_l3*8,struct.pack('<Q',marker_leaf))
        uc.mem_write(guard_root+image_l1*8,struct.pack('<Q',guard_root+0x3000|3))
        uc.mem_write(guard_root+0x3000+image_l2*8,struct.pack('<Q',guard_root+0x4000|3))
        uc.mem_write(guard_root+0x4000+image_l3*8,struct.pack('<Q',guard_leaf))
        uc.mem_map(guard_marker,4096)
    cp = {(3,3,0,0,1):4<<16, tcr:0x00080022a51a251a, ttbr0:guard_root if guarded else 0x8c9000000,
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
        assert draws==(4 if qr else 0)
    assert cp[ttbr0]==original[ttbr0] and cp[tcr]==original[tcr]
    assert len(switches)%2==0 and switches[-1]==original[ttbr0]
    table_pa=switches[0]
    assert all(v==table_pa if i%2==0 else v==original[ttbr0] for i,v in enumerate(switches))
    roots=struct.unpack('<512Q',bytes(uc.mem_read(table_pa,4096)))
    expected_roots={(fb>>30)&511:(table_pa+4096)|3}
    if guarded:
        expected_roots[(guard_root>>30)&511]=(table_pa+8192)|3
        expected_roots[(guard_marker>>30)&511]=(table_pa+12288)|3
    assert {i:v for i,v in enumerate(roots) if v}==expected_roots
    expected={}
    for pa in range(fb&~0x1fffff,fb+rows*stride,0x200000):expected[(pa>>21)&511]=pa|0x6000000000070d
    leaves=struct.unpack('<512Q',bytes(uc.mem_read(table_pa+4096,4096)))
    assert {i:v for i,v in enumerate(leaves) if v}==expected
    if guarded:
        image_l2=struct.unpack('<512Q',bytes(uc.mem_read(table_pa+8192,4096)))
        marker_l2=struct.unpack('<512Q',bytes(uc.mem_read(table_pa+12288,4096)))
        image_l3=struct.unpack('<512Q',bytes(uc.mem_read(table_pa+16384,4096)))
        marker_l3=struct.unpack('<512Q',bytes(uc.mem_read(table_pa+20480,4096)))
        assert {i:v for i,v in enumerate(image_l2) if v}=={(guard_root>>21)&511:(table_pa+16384)|3}
        assert {i:v for i,v in enumerate(marker_l2) if v}=={(guard_marker>>21)&511:(table_pa+20480)|3}
        assert {i:v for i,v in enumerate(image_l3) if v}=={(guard_page>>12)&511:guard_leaf}
        assert {i:v for i,v in enumerate(marker_l3) if v}=={(guard_marker>>12)&511:marker_leaf}
        marker_code=msg+0x1000;marker_stop=marker_code+0x800
        uc.mem_map(marker_code,4096)
        marker_words=(0xd29e0008,0xf2a7ffe8,0xf2c00128,0x52800029,0x39000109,
                      0xd5033f9f,0xd5182020,0xd5033fdf,0x52800049,0x39000509,0xd65f03c0)
        uc.mem_write(marker_code,struct.pack('<11I',*marker_words))
        marker_state={'root':0,'t0sz':0,'stores':[],'ttbr1':[]}
        def marker_translate(va):
            root=marker_state['root'];t0sz=marker_state['t0sz'];input_bits=64-t0sz
            assert va>>input_bits==0
            l1_bits=input_bits-30;l1_index=(va>>30)&((1<<l1_bits)-1)
            l1=struct.unpack('<Q',bytes(uc.mem_read(root+l1_index*8,8)))[0]
            assert l1&3==3 and not l1&(1<<61)
            l2_pa=(l1&0x0000fffffffff000)+(((va>>21)&0x1ff)*8)
            l2=struct.unpack('<Q',bytes(uc.mem_read(l2_pa,8)))[0]
            assert l2&3==3 and not l2&(1<<61)
            l3_pa=(l2&0x0000fffffffff000)+(((va>>12)&0x1ff)*8)
            l3=struct.unpack('<Q',bytes(uc.mem_read(l3_pa,8)))[0]
            assert l3&3==3 and not l3&(1<<7) and l3&(1<<10)
            return (l3&0x0000fffffffff000)|(va&0xfff)
        def marker_store(machine,access,address,size,value,_):
            if address in (guard_marker,guard_marker+1):
                assert size==1 and marker_translate(address)==address
                marker_state['stores'].append((address,value&0xff))
        def marker_instruction(machine,pc,length,_):
            if pc==marker_stop:
                machine.emu_stop();return
            word=struct.unpack('<I',bytes(machine.mem_read(pc,4)))[0]
            if word in (0xd5033f9f,0xd5182020,0xd5033fdf):
                if word==0xd5182020:
                    marker_state['ttbr1'].append(machine.reg_read(r.UC_ARM64_REG_X0))
                machine.reg_write(r.UC_ARM64_REG_PC,pc+4)
        uc.hook_add(UC_HOOK_MEM_WRITE,marker_store,begin=guard_marker,end=guard_marker+1)
        uc.hook_add(UC_HOOK_CODE,marker_instruction,begin=marker_code,end=marker_stop)
        temporary=(original[tcr]&~0xffff)|0x3519
        assert (temporary&0x3f)==25
        for root,t0sz in ((guard_root,26),(table_pa,25),(guard_root,26)):
            marker_state.update(root=root,t0sz=t0sz,stores=[],ttbr1=[])
            uc.mem_write(guard_marker,bytes(2))
            uc.reg_write(r.UC_ARM64_REG_PC,marker_code)
            uc.reg_write(r.UC_ARM64_REG_SP,stack+16368)
            uc.reg_write(r.UC_ARM64_REG_X0,0x8c8910000)
            uc.reg_write(r.UC_ARM64_REG_X30,marker_stop)
            uc.emu_start(marker_code,marker_stop,count=128)
            assert marker_state['stores']==[(guard_marker,1),(guard_marker+1,2)]
            assert marker_state['ttbr1']==[0x8c8910000]
            assert bytes(uc.mem_read(guard_marker,2))==b'\x01\x02'
            assert cp[ttbr0]==guard_root
        assert cp[tcr]==original[tcr]
    assert writes[0] and b'\xee\xee\xee\xff' in bytes(uc.mem_read(fb,rows*stride))
    mode=int(args.paused_export)
    if qr:
        assert value('diag_replay_ready')==4 and value('diag_test_qr_status')==0
        assert value('diag_report_phase')==0
        assert waits and max(waits)==draws
        text=bytes(uc.mem_read(value('diag_test_text'),value('diag_test_text_length')))
        if args.stage==47:
            assert (b'export pause' in text)==args.paused_export
            assert b'XNU TTBR1 MAP SNAPSHOT' in text and b'STAGE=47' in text
            assert b'ROOT=' in text and b'X=' in text and b'D=' in text
            assert b'O0=0008c9000000' in text and b'O1=0008c9010000' in text
        else:
            assert (b'intentional export pause' in text)==args.paused_export
            assert message in text
            assert b'OLD_TTBR1=00000008c9010000' in text
        assert b'ESR=' not in text
        stem='replay-'+str(mode)+('-stage-'+str(args.stage) if qr else '')
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
            'low_exception_guard_preserved_in_GOP_temporary_root':guarded,
            'marker_helper_STRBs_checked_under_guard_GOP_and_restored_guard_roots':guarded,
            'limits':'System registers and elapsed counter modeled; real hardware MMU/cache/camera not verified'}
    (args.build/('replay-check-'+str(args.stage)+'-'+str(mode)+'-'+str(width)+'.json')).write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    main()
