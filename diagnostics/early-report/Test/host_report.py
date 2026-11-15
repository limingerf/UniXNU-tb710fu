# SPDX-License-Identifier: BSD-2-Clause-Patent
# Synthetic fixture only: verifies the reporter/OneSend transport, not a device log.
import ctypes,struct,json,zlib
from pathlib import Path
lib=ctypes.CDLL('.local/early-fault-qr/report-host.so')
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
text=ctypes.string_at(textptr,n);assert b'XNU EARLY EXCEPTION' in text and b'ESR=0000000096000004' in text
status=ctypes.c_size_t.in_dll(lib,'diag_test_qr_status').value;assert status==0,status
frame=ctypes.string_at(ctypes.c_void_p.in_dll(lib,'diag_test_frame').value,752)
assert frame[:3]==b'OS\x02' and struct.unpack_from('<I',frame,748)[0]==zlib.crc32(frame[:748])
size=struct.unpack_from('<I',frame,20)[0];env=frame[28:28+size];assert env[:4]==b'LMS1'
assert struct.unpack_from('<I',frame,24)[0]==zlib.crc32(env)
nl,ml=struct.unpack_from('<HH',env,6);length,crc=struct.unpack_from('<II',env,10)
body=env[26+nl+ml:];assert body==text and len(body)==length and zlib.crc32(body)==crc
bitmap=ctypes.c_void_p.in_dll(lib,'diag_test_bitmap').value
pixels=ctypes.string_at(bitmap,121*121*4)
root=Path('.local/early-fault-qr')
(root/'qr.rgb').write_bytes(b''.join(pixels[i+2:i+3]+pixels[i+1:i+2]+pixels[i:i+1] for i in range(0,len(pixels),4)))
(root/'frame.bin').write_bytes(frame);(root/'report.log').write_bytes(text)
(root/'host-check.json').write_text(json.dumps({'result':'PASS','report_bytes':n,'envelope_bytes':size,'frame_bytes':len(frame),'crc32_checks':'frame/envelope/body PASS','qr_status':status,'heap_bytes':ctypes.c_size_t.in_dll(lib,'diag_test_heap_used').value,'limits':'host C + unchanged QR encoder; not a real CPU fault'},indent=2)+'\n')
print(text.decode());print((root/'host-check.json').read_text())

# Repeated continuing snapshots must reset the bounded arena and encode fresh data.
lib.diag_prepare_live_frame.argtypes=[ctypes.POINTER(State),ctypes.c_char_p]
ctypes.c_uint64.in_dll(lib,'diag_report_phase').value=0
for stage in (27,29,47,27):
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
result['repeated_live_snapshot_frames']='PASS: 27/29/47/27; fresh body, bounded arena, CRCs, no invented exception'
(root/'host-check.json').write_text(json.dumps(result,indent=2)+'\n')
print(result['repeated_live_snapshot_frames'])
