/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* SPMI arbiter v7 observer reads for the TB710FU PMIC keys.
 *
 * Windows and the EE come from sm8650.dtsi spmi@c400000 (see
 * Tb710fuInputHardware.h). The channel layout, the version gate and the
 * APID-by-PPID discovery are the arbiter v7 register model that
 * firmware/overlay/.../Tb710fuKeyInputDxe/Tb710fuSpmiKeys.h already runs on
 * this board; the Linux driver it derives from is
 * drivers/spmi/spmi-pmic-arb.c.
 *
 * Only reads are performed: one observer command per register byte, status
 * polled with a bounded count, no PMIC configuration register is written and
 * no interrupt is armed. A failed read reports "nothing pressed" rather than
 * a guessed state.
 */
#ifndef TB710FU_SPMI_H
#define TB710FU_SPMI_H

#include <stdint.h>

#define TB710FU_SPMI_VERSION_V7_MIN 0x70000000u
#define TB710FU_SPMI_VERSION_V8_MIN 0x80000000u
#define TB710FU_SPMI_FEATURES        0x04u
#define TB710FU_SPMI_FEATURES_APIDS  0x7FFu
#define TB710FU_SPMI_APID_MAP        0x2000u
#define TB710FU_SPMI_CNFG_OWNER      0x00u
#define TB710FU_SPMI_MAX_APIDS       1024u
#define TB710FU_SPMI_MAX_EE          7u
#define TB710FU_SPMI_OBS_EE_STRIDE   0x8000u
#define TB710FU_SPMI_OBS_APID_STRIDE 0x20u
#define TB710FU_SPMI_CMD             0x00u
#define TB710FU_SPMI_STATUS          0x08u
#define TB710FU_SPMI_RDATA0          0x18u
#define TB710FU_SPMI_OP_EXT_READL    1u
#define TB710FU_SPMI_STATUS_DONE     (1u << 0)
#define TB710FU_SPMI_STATUS_ERRORS   0x0Eu
#define TB710FU_SPMI_POLLS           100000u

struct tb710fu_spmi {
    void *core;   /* spmi@c400000 reg[0], 0x3000 bytes */
    void *obs;    /* reg[2], 0x80000 bytes, this EE's channels at the base */
    void *cnfg;   /* reg[4], 0x4000 bytes, ownership table */
    uint8_t ee;
    uint32_t (*read32)(void *ctx, void *window, uint32_t offset);
    void (*write32)(void *ctx, void *window, uint32_t offset, uint32_t value);
    void (*barrier)(void *ctx);
    void *ctx;
};

/* Returns 0 and stores the APID count when the arbiter reports a v7 layout
 * with a sane APID table; 1 for an unknown layout, 2 for a bad APID count and
 * 4 for invalid arguments. */
int tb710fu_spmi_layout(const struct tb710fu_spmi *spmi, uint32_t *apids);

/* Find the APID that maps `ppid` (SID << 8 | PID). Prefers the APID this EE
 * owns; falls back to the first mapping. 0 on success, 3 when absent. */
int tb710fu_spmi_find_apid(const struct tb710fu_spmi *spmi, uint32_t ppid, int *apid);

/* One register byte through the observer channel: 0 ok, 1 timeout, 2 hardware
 * error, 4 invalid arguments. `*value` is preserved on failure. */
int tb710fu_spmi_read8(const struct tb710fu_spmi *spmi, int apid, uint8_t reg, uint8_t *value);

#endif /* TB710FU_SPMI_H */
