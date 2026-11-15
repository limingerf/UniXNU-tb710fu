/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Board diagnostic based on pinned UniXNU PAINT; one top strip, no accumulating bands.
 */
.text
.global _start
_start:
  ldr x1, [x0, #0x28]
  ldr x2, [x0, #0x38]
  ldr x3, [x0, #0x48]
  mul x2, x2, x3
  lsr x2, x2, #4
  mov x3, #5
  mul x2, x2, x3
  bic x2, x2, #7
  cbz x2, 3f
  mov w6, wzr
1:
  stp w6, w6, [x1], #8
  subs x2, x2, #8
  b.hi 1b
  dsb sy
3:
  ldr x1, [x0, #0x28]
  ldr x2, [x0, #0x38]
  ldr x3, [x0, #0x48]
  mov x4, #48
  cmp x3, x4
  csel x3, x3, x4, lo
  mul x2, x2, x3
  bic x2, x2, #7
  cbz x2, 2f
  movz w6, #0x00FF
  movk w6, #0xFF00, lsl #16
1:
  stp w6, w6, [x1], #8
  subs x2, x2, #8
  b.hi 1b
  dsb sy
2:
  b target
