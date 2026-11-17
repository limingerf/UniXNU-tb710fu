/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Pinned UniXNU patch-xnu-entry-probe.py PAINT, stage 1: blue band 1.
 * Band 0 is the loader handoff marker. Do not clear other framebuffer data.
 */
.text
.global _start
_start:
  ldr x1, [x0, #0x28]
  ldr x2, [x0, #0x38]
  ldr x3, [x0, #0x48]
  mul x2, x2, x3
  lsr x2, x2, #4
  bic x2, x2, #7
  cbz x2, 2f
  mov x4, #1
  mul x5, x2, x4
  add x1, x1, x5
  movz w6, #0x00FF
  movk w6, #0xFF00, lsl #16
1:
  stp w6, w6, [x1], #8
  subs x2, x2, #8
  b.hi 1b
  dsb sy
2:
  b target
