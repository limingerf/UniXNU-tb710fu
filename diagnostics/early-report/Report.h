/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef TB_EARLY_REPORT_H
#define TB_EARLY_REPORT_H
#include "Compat.h"
/* Assembly offsets are checked by the producer and host tests. */
typedef struct {
  UINT64 Regs[31];                 /* 0x000..0x0f0 */
  UINT64 Sp, Esr, Far, Elr, Spsr; /* 0x0f8..0x118 */
  UINT64 Sctlr, Kind;             /* 0x120, 0x128: 1=exception, 2=panic */
  UINT64 BootArgsPa;              /* 0x130 */
  UINT64 Framebuffer, RowBytes, Width, Height; /* 0x138..0x150 */
  UINT64 VirtualBase, PhysicalBase, WindowBytes; /* 0x158..0x168 */
  UINT64 PayloadPa;               /* 0x170 */
} TB_REPORT;
typedef struct {
  UINT64 L1Descriptor, L2Descriptor, L3Descriptor, Physical;
  UINT32 Status;
  UINT8 Level, Writable, Executable, AccessFlag;
} DIAG_WALK_RESULT;
typedef BOOLEAN (*DIAG_READ_DESCRIPTOR)(VOID *context, UINT64 pa, UINT64 *value);
enum {
  DIAG_WALK_OK = 0,
  DIAG_WALK_UNSUPPORTED = 1,
  DIAG_WALK_READ_FAILED = 2,
  DIAG_WALK_INVALID_L1 = 3,
  DIAG_WALK_INVALID_L2 = 4,
  DIAG_WALK_INVALID_L3 = 5
};
BOOLEAN diag_walk_t1_4k(UINT64 root_pa, UINT64 tcr, UINT64 va,
                        DIAG_READ_DESCRIPTOR read_descriptor, VOID *context,
                        DIAG_WALK_RESULT *result);
_Static_assert(offsetof(TB_REPORT, Sp) == 0xf8, "SP offset");
_Static_assert(offsetof(TB_REPORT, PayloadPa) == 0x170, "payload offset");
_Static_assert(sizeof(TB_REPORT) == 0x178, "report size");
extern TB_REPORT diag_state;
VOID diag_build_report(TB_REPORT *state);
#endif
