// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tb710fuPlatformStage.h - the boot-argument ladder every TB710FU platform service is gated on.
//
// The rule this package follows (CURRENT_STATUS.md, "boot-arg staged bring-up"):
// nothing maps a register, publishes a service or replaces a platform hook until
// the board is explicitly asked to. A level is a small integer, never a boolean,
// so a new stage can be added between two existing ones without renumbering:
//
//   platform-svc  0  off, the default: no window is mapped, IORTC is not
//                     published and PE_halt_restart keeps the platform's own
//                     value. A default boot is byte-for-byte the previous one.
//                 1  diagnose: map the four spmi@c400000 windows, probe the v7
//                     arbiter, read the PMK8550 counter and the SDAM2
//                     reboot-reason cell, print all of it. Publishes nothing,
//                     writes nothing, installs no hook.
//                 2  serve: level 1 plus the IORTC service, the platform-expert
//                     clock registration and PE_halt_restart. SDAM2 writes stay
//                     behind their own opt-in (tb710fu-rtc-store=1).
//
//   backlight      0  off, the default.
//                 1  diagnose: map the QUP wrap-1 SE6 window, check the GENI
//                     I2C firmware, read the SY7758 device id, the brightness
//                     mode and the 12-bit level. No write, nothing published.
//                 2  serve: level 1 plus the backlight service CoreBrightness
//                     drives, which writes the 12-bit brightness registers.
//
//   thermal        0  off, the default. No TSENS window is mapped.
//                 1  serve: map the three TM and three SROT windows, enable the
//                     sensor block only when it is found disabled, and publish
//                     the one-shot sample on Tb710fuThermal.
//
// Levels above the maximum are clamped to it and reported, so a typo cannot
// silently select an unknown stage; a value that does not parse at all leaves
// the service off. The parsing lives here, in plain C, so the host test can
// prove the mapping for every argument the kernel will see.
#ifndef TB710FU_PLATFORM_STAGE_H
#define TB710FU_PLATFORM_STAGE_H

#include <stdint.h>

/* Compile-time maximum of each ladder; the boot argument is clamped to it. */
#define TB710FU_STAGE_PLATFORM_MAX 2u
#define TB710FU_STAGE_BACKLIGHT_MAX 2u
#define TB710FU_STAGE_THERMAL_MAX 1u

/* Boot arguments, as PE_parse_boot_argn() sees them in PE_boot_args(). */
#define TB710FU_BOOTARG_PLATFORM "platform-svc"
#define TB710FU_BOOTARG_BACKLIGHT "backlight"
#define TB710FU_BOOTARG_THERMAL "thermal"

/* What a stage asks of a subsystem, given what its probe found. A level that
 * would need a register the probe refused degrades to probe-only instead of
 * writing blind. */
enum tb710fu_stage_action {
    TB710FU_STAGE_NONE = 0,     /* do not touch the hardware at all */
    TB710FU_STAGE_DIAGNOSE,     /* read, log, publish nothing */
    TB710FU_STAGE_SERVE         /* read, write within policy, publish */
};

/* Clamp a requested level to 0..maximum. *clamped is set when the request was
 * out of range, so the caller can say so once in the boot log. Returns the
 * level to use. */
unsigned tb710fu_stage_clamp(unsigned requested, unsigned maximum, int *clamped);

/* The action a subsystem takes. probe_ok says whether every read the diagnose
 * stage needs succeeded:
 *   level 0                     -> NONE
 *   level 1                     -> DIAGNOSE (probe_ok or not: the log is the point)
 *   level 2, probe_ok           -> SERVE
 *   level 2, !probe_ok          -> NONE (a service that cannot read its device
 *                                  must not write it either) */
enum tb710fu_stage_action tb710fu_stage_action(unsigned level, int probe_ok);

/* Parses one "key=<unsigned>" token out of a boot-argument string the way
 * PE_parse_boot_argn() does, without depending on it: the token must be
 * delimited by a space, a tab or the ends of the string, the value is read in
 * decimal (0x.. hex is accepted too), and anything else after the digits makes
 * the token invalid. Returns 1 when found with a valid value, 0 when the key is
 * absent, -1 when the key is present but its value does not parse. */
int tb710fu_bootarg_value(const char *arguments, const char *key,
                          unsigned *value);

/* Convenience: parse, clamp and report in one call. absent or unparsable
 * values leave *level at 0. Returns the same codes as tb710fu_bootarg_value. */
int tb710fu_bootarg_stage(const char *arguments, const char *key,
                          unsigned maximum, unsigned *level);

/* The gate a driver's own entry point applies before it touches a register:
 * 1 when the boot argument selects at least `required`, 0 otherwise (absent,
 * unparsable, or a lower level). *level receives the clamped level that was
 * used, so the caller can name it in the log. This is the same decision the
 * platform's ladder makes; the check is repeated inside each driver so a
 * direct call from another subsystem cannot map a window the boot did not ask
 * for. */
int tb710fu_stage_allow(const char *arguments, const char *key,
                        unsigned maximum, unsigned required, unsigned *level);

#endif
