/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Terminal report only. Installed at arm_init before the original PACIBSP.
 * Captures EL1 synchronous exceptions before original vector stack spills.
 */
.section .text.entry,"ax"
.global diag_init, diag_exception, diag_panic
.hidden arm_next
diag_init:
  adrp x9, diag_state
  add x9, x9, :lo12:diag_state
  adrp x10, diag_image_base
  add x10, x10, :lo12:diag_image_base
  sub x10, x10, x22
  add x10, x10, x23
  str x10, [x9, #0x170]
  sub x10, x0, x22
  add x10, x10, x23
  str x10, [x9, #0x130]
  ldr x10, [x0, #0x28]
  str x10, [x9, #0x138]
  ldr x10, [x0, #0x38]
  str x10, [x9, #0x140]
  ldr x10, [x0, #0x48]
  str x10, [x9, #0x150]
  ldr x10, [x0, #0x40]
  str x10, [x9, #0x148]
  stp x22, x23, [x9, #0x158]
  ldr x10, [x0, #0x18]
  str x10, [x9, #0x168]
  adrp x9, diag_vectors
  add x9, x9, :lo12:diag_vectors
  msr vbar_el1, x9
  isb
  // Optional explicit menu self-test, bounded by the boot_args command line.
  add x11, x0, #0x6c
  add x12, x11, #1024
diag_arg_scan:
  cmp x11, x12
  b.hs diag_resume
  ldrb w10, [x11]
  cbz w10, diag_resume
  mov x13, x11
  adr x14, diag_selftest_name
diag_arg_compare:
  ldrb w15, [x14], #1
  cbz w15, diag_selftest
  cmp x13, x12
  b.hs diag_resume
  ldrb w16, [x13], #1
  cmp w16, w15
  b.ne diag_arg_next
  b diag_arg_compare
diag_arg_next:
  add x11, x11, #1
  b diag_arg_scan
diag_selftest:
  brk #0xb711
diag_resume:
  .inst 0xd503237f /* Replay original arm_init PACIBSP at the same SP/LR. */
  b arm_next
diag_selftest_name:
  .asciz "tb-report-selftest=1"
.balign 4

diag_exception:
  msr tpidr_el1, x18
  msr tpidrro_el0, x17
  mov x17, #1
  b diag_capture
diag_panic:
  msr daifset, #15
  msr tpidr_el1, x18
  msr tpidrro_el0, x17
  mov x17, #2
diag_capture:
  adrp x18, diag_state
  add x18, x18, :lo12:diag_state
  stp x0,x1,[x18,#0]
  stp x2,x3,[x18,#16]
  stp x4,x5,[x18,#32]
  stp x6,x7,[x18,#48]
  stp x8,x9,[x18,#64]
  stp x10,x11,[x18,#80]
  stp x12,x13,[x18,#96]
  stp x14,x15,[x18,#112]
  str x16,[x18,#128]
  str x17,[x18,#0x128]
  mrs x0,tpidrro_el0
  str x0,[x18,#136]
  mrs x0,tpidr_el1
  str x0,[x18,#144]
  str x19,[x18,#152]
  stp x20,x21,[x18,#160]
  stp x22,x23,[x18,#176]
  stp x24,x25,[x18,#192]
  stp x26,x27,[x18,#208]
  stp x28,x29,[x18,#224]
  str x30,[x18,#240]
  mrs x1,spsr_el1
  mov x0,sp
  cmp x17,#2
  b.eq 1f
  tbnz x1,#0,1f
  mrs x0,sp_el0
1:
  str x0,[x18,#0xf8]
  mrs x0,esr_el1
  str x0,[x18,#0x100]
  mrs x0,far_el1
  str x0,[x18,#0x108]
  mrs x0,elr_el1
  str x0,[x18,#0x110]
  str x1,[x18,#0x118]
  mrs x0,sctlr_el1
  str x0,[x18,#0x120]
  msr daifset,#15
  // Clean only this diagnostic image/state before using its physical alias.
  adrp x4,diag_image_base
  add x4,x4,:lo12:diag_image_base
  adrp x5,diag_image_end
  add x5,x5,:lo12:diag_image_end
  mrs x2,ctr_el0
  ubfx x2,x2,#16,#4
  mov x3,#4
  lsl x2,x3,x2
2:
  dc cvac,x4
  add x4,x4,x2
  cmp x4,x5
  b.lo 2b
  dsb sy
  ldr x9,[x18,#0x170]
  adr x1,diag_physical
  adrp x2,diag_image_base
  add x2,x2,:lo12:diag_image_base
  sub x1,x1,x2
  add x1,x1,x9
  br x1
diag_physical:
  mrs x10,sctlr_el1
  bic x10,x10,#1
  bic x10,x10,#4
  bic x10,x10,#0x1000
  msr sctlr_el1,x10
  isb
  msr spsel,#1
  adrp x1,diag_stack_end
  add x1,x1,:lo12:diag_stack_end
  mov sp,x1
  adrp x0,diag_state
  add x0,x0,:lo12:diag_state
  b diag_fault_main

.balign 2048
.global diag_vectors
diag_vectors:
.rept 16
  b diag_exception
  .balign 128
.endr
