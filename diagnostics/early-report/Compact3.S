/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Board diagnostic based on pinned UniXNU PAINT; one top strip, no accumulating bands.
 */
.text
.global _start
_start:
  mrs x10, sctlr_el1
  bic x11, x10, #1
  bic x11, x11, #4
  bic x11, x11, #0x1000
  msr sctlr_el1, x11
  isb
  ldr x1, [x20, #0x28]
  ldr x2, [x20, #0x38]
  ldr x3, [x20, #0x48]
  mov x4, #48
  cmp x3, x4
  csel x3, x3, x4, lo
  mul x2, x2, x3
  bic x2, x2, #7
  cbz x2, 2f
  movz w6, #0xFFFF
  movk w6, #0xFF00, lsl #16
1:
  stp w6, w6, [x1], #8
  subs x2, x2, #8
  b.hi 1b
  dsb sy
2:
  msr sctlr_el1, x10
  isb
  mov x0, #0x300000
  b target
