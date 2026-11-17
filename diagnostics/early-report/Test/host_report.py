# SPDX-License-Identifier: BSD-2-Clause-Patent
# Synthetic fixture only: verifies the reporter/OneSend transport, not a device log.
import argparse
import ctypes,struct,json,zlib
from pathlib import Path
parser=argparse.ArgumentParser()
parser.add_argument('--library',default='.local/early-fault-qr/report-host.so')
parser.add_argument('--output-dir',default='.local/early-fault-qr')
parser.add_argument('--build-label',default='source-unbound')
args=parser.parse_args()
root=Path(args.output_dir);root.mkdir(parents=True,exist_ok=True)
lib=ctypes.CDLL(args.library)
class State(ctypes.Structure):
 _fields_=[('Regs',ctypes.c_uint64*31)]+[(n,ctypes.c_uint64) for n in ['Sp','Esr','Far','Elr','Spsr','Sctlr','Kind','BootArgsPa','Framebuffer','RowBytes','Width','Height','VirtualBase','PhysicalBase','WindowBytes','PayloadPa']]
assert ctypes.sizeof(State)==0x178
fb=(ctypes.c_uint32*(3200*2000))()
s=State();s.Framebuffer=ctypes.addressof(fb);s.RowBytes=12800;s.Width=3200;s.Height=2000
s.Sp=0xfffffff00f817f30;s.Esr=0x96000004;s.Far=0xdead0000;s.Elr=0xfffffff00c28f874;s.Sctlr=0x30d0183d;s.Regs[30]=0xfffffff00bb95468;s.Kind=1
lib.diag_build_report.argtypes=[ctypes.POINTER(State)]
ctypes.c_uint64.in_dll(lib,'diag_report_phase').value=0
lib.diag_build_report(ctypes.byref(s))
textptr=ctypes.c_void_p.in_dll(lib,'diag_test_text').value;n=ctypes.c_size_t.in_dll(lib,'diag_test_text_length').value
text=ctypes.string_at(textptr,n);assert b'XNU EARLY EXCEPTION' in text and b'ESR=0000000096000004' in text and ('build '+args.build_label).encode() in text
status=ctypes.c_size_t.in_dll(lib,'diag_test_qr_status').value;assert status==0,status
frame=ctypes.string_at(ctypes.c_void_p.in_dll(lib,'diag_test_frame').value,752)
assert frame[:3]==b'OS\x02' and struct.unpack_from('<I',frame,748)[0]==zlib.crc32(frame[:748])
size=struct.unpack_from('<I',frame,20)[0];env=frame[28:28+size];assert env[:4]==b'LMS1'
assert struct.unpack_from('<I',frame,24)[0]==zlib.crc32(env)
nl,ml=struct.unpack_from('<HH',env,6);length,crc=struct.unpack_from('<II',env,10)
body=env[26+nl+ml:];assert body==text and len(body)==length and zlib.crc32(body)==crc
bitmap=ctypes.c_void_p.in_dll(lib,'diag_test_bitmap').value
pixels=ctypes.string_at(bitmap,121*121*4)
(root/'qr.rgb').write_bytes(b''.join(pixels[i+2:i+3]+pixels[i+1:i+2]+pixels[i:i+1] for i in range(0,len(pixels),4)))
(root/'frame.bin').write_bytes(frame);(root/'report.log').write_bytes(text)
(root/'host-check.json').write_text(json.dumps({'result':'PASS','report_bytes':n,'envelope_bytes':size,'frame_bytes':len(frame),'crc32_checks':'frame/envelope/body PASS','qr_status':status,'heap_bytes':ctypes.c_size_t.in_dll(lib,'diag_test_heap_used').value,'limits':'host C + unchanged QR encoder; not a real CPU fault'},indent=2)+'\n')
print(text.decode());print((root/'host-check.json').read_text())

# Repeated continuing snapshots must reset the bounded arena and encode fresh data.
lib.diag_prepare_live_frame.argtypes=[ctypes.POINTER(State),ctypes.c_char_p]
ctypes.c_uint64.in_dll(lib,'diag_report_phase').value=0
for stage in (27,29,47,48,49,27):
 ctypes.c_uint64.in_dll(lib,'diag_last_stage').value=stage
 message=('XNU stage '+str(stage)).encode()
 lib.diag_prepare_live_frame(ctypes.byref(s),message)
 assert ctypes.c_size_t.in_dll(lib,'diag_test_qr_status').value==0
 live=ctypes.string_at(ctypes.c_void_p.in_dll(lib,'diag_test_frame').value,752)
 size=struct.unpack_from('<I',live,20)[0];env=live[28:28+size]
 assert struct.unpack_from('<I',live,748)[0]==zlib.crc32(live[:748])
 assert struct.unpack_from('<I',live,24)[0]==zlib.crc32(env)
 nl,ml=struct.unpack_from('<HH',env,6);body=env[26+nl+ml:]
 assert env[26:26+nl]==b'TB710FU-live-stage.log'
 assert b'XNU LIVE CHECKPOINT (continues)' in body and message in body and b'ESR=' not in body
 assert struct.unpack_from('<I',env,14)[0]==zlib.crc32(body)
 assert ctypes.c_size_t.in_dll(lib,'diag_test_heap_used').value<524288
 assert ctypes.c_uint64.in_dll(lib,'diag_report_phase').value==0
result=json.loads((root/'host-check.json').read_text())
result['repeated_live_snapshot_frames']='PASS: 27/29/47/48/49/27; fresh body, bounded arena, CRCs, no invented exception'
(root/'host-check.json').write_text(json.dumps(result,indent=2)+'\n')
print(result['repeated_live_snapshot_frames'])

# Animated export keeps one file/session; changing QR sequences do not change data.
lib.diag_prepare_replay.argtypes=[ctypes.POINTER(State),ctypes.c_char_p]
lib.diag_export_option.argtypes=[ctypes.c_char_p];lib.diag_export_option.restype=ctypes.c_int
assert lib.diag_export_option(b'-v tb-report-export=1 serial=3')
assert not lib.diag_export_option(b'-v not-tb-report-export=1')
assert not lib.diag_export_option(b'tb-report-export=11')
assert not lib.diag_export_option(b'x'*1024)
replay_cases=(
 (47,b'XNU enter: vm/set_mmu_ttb_alternate_probed#1',b'before marker helper call'),
 (48,b'XNU returned: vm/set_mmu_ttb_alternate_probed#1',b'marked switch helper returned'),
 (49,b'XNU enter: vm/set_mmu_ttb#1',b'first post-switch call reached'),
 (50,b'XNU returned: vm/set_mmu_ttb#1',b'XNU returned: vm/set_mmu_ttb#1'),
 (51,b'XNU enter: vm/pmap_bootstrap#1',b'XNU enter: vm/pmap_bootstrap#1'),
 (52,b'XNU returned: vm/pmap_bootstrap#1',b'XNU returned: vm/pmap_bootstrap#1'),
 (54,b'XNU returned: vm/_enable_preemption#1',b'post-mapping checkpoint'),
 (55,b'XNU enter: tb710fu_ramoops_init',b'arm_vm_init returned'),
 (56,b'XNU returned: tb710fu_ramoops_init',b'ramoops initialization returned'))
platform_names=(
 'PE_consistent_debug_inherit','serial_init','kernel_startup_initialize_upto',
 'PE_create_console','PE_init_printf','cpu_machine_idle_init','PE_init_platform',
 'pe_arm_debug_init_early','ml_map_cpu_pio','cpu_timebase_init','kptimer_curcpu_up',
 'PE_init_cpu','apt_msg_init','apt_msg_init_cpu','fiq_context_init','early_random',
 'machine_startup')
for index,name in enumerate(platform_names):
 stage=57+index*2
 if name=='machine_startup':
  replay_cases+=((stage,('XNU enter: '+name).encode(),
                 b'handing control to machine_startup; BSD/CDC not yet observed'),)
 else:
  expected=b'pre-scheduler path; CDC worker has not been reached'
  replay_cases+=((stage,('XNU enter: '+name).encode(),expected),
                 (stage+1,('XNU returned: '+name).encode(),expected))
assert [stage for stage,_,_ in replay_cases if stage>=57]==list(range(57,90))
largest_live_text=0
ctypes.c_uint64.in_dll(lib,'diag_guard_status').value=2
for stage,message,expected in replay_cases:
 ctypes.c_uint64.in_dll(lib,'diag_last_stage').value=stage
 for paused in (0,1):
  ctypes.c_uint64.in_dll(lib,'diag_export_mode').value=paused
  lib.diag_prepare_replay(ctypes.byref(s),message)
  assert ctypes.c_uint64.in_dll(lib,'diag_replay_ready').value==4
  assert ctypes.c_size_t.in_dll(lib,'diag_test_qr_status').value==0
  text=ctypes.string_at(ctypes.c_void_p.in_dll(lib,'diag_test_text').value,ctypes.c_size_t.in_dll(lib,'diag_test_text_length').value)
  assert expected in text, text.decode()
  assert b'SWITCH_GUARD=0000000000000002\n' in text
  assert len(text)<619 and b'ESR=' not in text
  if stage>=57:assert text.endswith(expected+b'\n'),text.decode()
  largest_live_text=max(largest_live_text,len(text))
  variants=bytes((ctypes.c_uint8*(4*752)).in_dll(lib,'diag_replay_frames'))
  bodies=[];sessions=[]
  for i in range(4):
   frame=variants[i*752:(i+1)*752]
   assert struct.unpack_from('<I',frame,8)[0]==i
   assert struct.unpack_from('<I',frame,748)[0]==zlib.crc32(frame[:748])
   size=struct.unpack_from('<I',frame,20)[0];env=frame[28:28+size]
   assert struct.unpack_from('<I',frame,24)[0]==zlib.crc32(env)
   nl,ml=struct.unpack_from('<HH',env,6);body=env[26+nl+ml:]
   assert body==text and struct.unpack_from('<II',env,10)==(len(body),zlib.crc32(body))
   sessions.append(frame[4:8]);bodies.append(frame[12:748])
   (root/('switch-'+str(stage)+'-'+str(paused)+'-frame-'+str(i)+'.bin')).write_bytes(frame)
  assert len(set(sessions))==1 and len(set(bodies))==1
  (root/('switch-'+str(stage)+'-'+str(paused)+'.log')).write_bytes(text)
result['four_distinct_QR_variants']='PASS: one unchanged envelope/session, sequence 0/1/2/3, CRCs'
result['switch_boundary_QR_frames']='PASS: distinct stage 47 entry, stage 48 return, and stage 49 post-switch frames'
result['platform_boundary_QR_frames']='PASS: stages 57..88 entry/return and stage 89 machine_startup entry; fresh QR, frame/envelope/body CRCs, full final explanatory line'
result['largest_live_report_bytes']=largest_live_text
result['export_mode_scope']='PASS: explicit token only, intentional-pause label retained'
(root/'host-check.json').write_text(json.dumps(result,indent=2)+'\n')

# The live TTBR1 walker is pure table geometry; hardware descriptor reads are
# supplied separately by the guarded old-TTBR1 alias in Report.c.
class Walk(ctypes.Structure):
 _fields_=[('L1Descriptor',ctypes.c_uint64),('L2Descriptor',ctypes.c_uint64),
           ('L3Descriptor',ctypes.c_uint64),
           ('Physical',ctypes.c_uint64),('Status',ctypes.c_uint32),
           ('Level',ctypes.c_uint8),('Writable',ctypes.c_uint8),
           ('Executable',ctypes.c_uint8),('AccessFlag',ctypes.c_uint8)]
entries={}
READ_DESCRIPTOR=ctypes.CFUNCTYPE(ctypes.c_int,ctypes.c_void_p,ctypes.c_uint64,ctypes.POINTER(ctypes.c_uint64))
@READ_DESCRIPTOR
def read_descriptor(_context,pa,out):
 value=entries.get(pa)
 if value is None:return 0
 out[0]=value
 return 1
lib.diag_walk_t1_4k.argtypes=[ctypes.c_uint64,ctypes.c_uint64,ctypes.c_uint64,
                              READ_DESCRIPTOR,ctypes.c_void_p,ctypes.POINTER(Walk)]
lib.diag_walk_t1_4k.restype=ctypes.c_int
root_pa,l2_pa,l3_pa=0x100000,0x200000,0x300000
tcr=(26<<16)|(2<<30)
va=0xfffffff00f817d48
l1_slot=(va>>30)&0xff
l2_slot=(va>>21)&0x1ff;l3_slot=(va>>12)&0x1ff
entries={root_pa+l1_slot*8:l2_pa|3,
         l2_pa+l2_slot*8:l3_pa|3,
         l3_pa+l3_slot*8:0x12345000|(1<<10)|(3<<8)|(2<<6)|3}
walk=Walk()
assert lib.diag_walk_t1_4k(root_pa,tcr,va,read_descriptor,None,ctypes.byref(walk))
assert walk.Status==0 and walk.Level==3 and walk.Physical==(0x12345000|(va&0xfff))
assert not walk.Writable and walk.Executable and walk.AccessFlag
entries[root_pa+l1_slot*8]=l2_pa|3|(1<<59)
assert lib.diag_walk_t1_4k(root_pa,tcr,va,read_descriptor,None,ctypes.byref(walk))
assert not walk.Executable
entries[root_pa+l1_slot*8]=l2_pa|3|(1<<61)
assert lib.diag_walk_t1_4k(root_pa,tcr,va,read_descriptor,None,ctypes.byref(walk))
assert not walk.Writable
entries[l3_pa+l3_slot*8]=0x12345000|(1<<10)|(3<<8)|(1<<6)|3
entries[root_pa+l1_slot*8]=l2_pa|3|(1<<62)
assert lib.diag_walk_t1_4k(root_pa,tcr,va,read_descriptor,None,ctypes.byref(walk))
assert walk.Writable
entries[root_pa+l1_slot*8]=l2_pa|3
entries[l2_pa+l2_slot*8]=l3_pa|3|(1<<61)
assert lib.diag_walk_t1_4k(root_pa,tcr,va,read_descriptor,None,ctypes.byref(walk))
assert not walk.Writable
entries[l2_pa+l2_slot*8]=l3_pa|3|(1<<62)
assert lib.diag_walk_t1_4k(root_pa,tcr,va,read_descriptor,None,ctypes.byref(walk))
assert walk.Writable
entries[root_pa+l1_slot*8]=0x80000000|(1<<10)|(3<<8)|(1<<53)|1
assert lib.diag_walk_t1_4k(root_pa,tcr,va,read_descriptor,None,ctypes.byref(walk))
assert walk.Status==0 and walk.Level==1 and walk.Writable and not walk.Executable
assert walk.Physical==(0x80000000|(va&0x3fffffff))
entries[root_pa+l1_slot*8]=l2_pa|3
entries[l2_pa+l2_slot*8]=0x80000000|(1<<10)|(3<<8)|(1<<53)|1
assert lib.diag_walk_t1_4k(root_pa,tcr,va,read_descriptor,None,ctypes.byref(walk))
assert walk.Status==0 and walk.Level==2 and walk.Writable and not walk.Executable
assert walk.Physical==(0x80000000|(va&0x1fffff))
entries[root_pa+l1_slot*8]=0
assert not lib.diag_walk_t1_4k(root_pa,tcr,va,read_descriptor,None,ctypes.byref(walk))
assert walk.Status==3
assert not lib.diag_walk_t1_4k(root_pa,tcr^(1<<16),va,read_descriptor,None,ctypes.byref(walk))
assert walk.Status==1
assert not lib.diag_walk_t1_4k(root_pa,tcr,0x1000,read_descriptor,None,ctypes.byref(walk))
assert walk.Status==1
result['live_TTBR1_walk_model']='PASS: L1->L2->L3, L1/L2 blocks, APTable/PXNTable, invalid descriptor, unsupported TCR and non-TTBR1 VA'
(root/'host-check.json').write_text(json.dumps(result,indent=2)+'\n')
print(result['live_TTBR1_walk_model'])
