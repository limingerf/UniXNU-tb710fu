/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * The upstream post-MMU stage has no color bar. Continue the original code.
 */
.text
.global _start
_start:
  mov x0, #0x300000
  b target
