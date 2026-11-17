/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Pinned UniXNU patch-xnu-entry-probe.py PAINT, stage 2: magenta band 2.
 */
.text
.global _start
_start:
  ldr x1, [x20, #0x28]
  ldr x2, [x20, #0x38]
  ldr x3, [x20, #0x48]
  mul x2, x2, x3
  lsr x2, x2, #4
  bic x2, x2, #7
  cbz x2, 2f
  mov x4, #2
  mul x5, x2, x4
  add x1, x1, x5
  movz w6, #0x00FF
  movk w6, #0xFFFF, lsl #16
1:
  stp w6, w6, [x1], #8
  subs x2, x2, #8
  b.hi 1b
  dsb sy
2:
  msr SCTLR_EL1, x0
  b target
