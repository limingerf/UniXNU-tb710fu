// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tb710fuPmk8550Reset.h - cold power-off, restart and reboot reasons.
//
// Board facts and their upstream origin:
//
//   PSCI function IDs, include/uapi/linux/psci.h:41-42,59-60,100-101
//     PSCI_VERSION 0x84000000, PSCI_FEATURES 0x8400000a,
//     SYSTEM_OFF 0x84000008, SYSTEM_RESET 0x84000009,
//     SYSTEM_RESET2 32-bit 0x84000012, 64-bit 0xC4000012,
//     reset type 0 = SYSTEM_WARM_RESET, vendor types start at 0x80000000.
//     drivers/firmware/psci/psci.c:558-563 is how SYSTEM_OFF is invoked
//     (psci_sys_poweroff), psci_sys_reset is its SYSTEM_RESET twin. On this
//     platform DT's psci node uses method = "smc", so the call is SMC #0.
//
//   Reboot reason, arch/arm64/boot/dts/qcom/pmk8550.dtsi:12-17,61-63
//     pmk8550_sdam_2 (SID 0 / PPID 0x071) declares reboot-reason@48 with
//     reg = <0x48 0x1> and bits = <1 7>, and nvmem-reboot-mode maps
//     mode-recovery = <0x01>, mode-bootloader = <0x02>.
//     So recovery is the raw byte 0x02 and bootloader 0x04; the seven-bit
//     field starts at bit 1 and bit 0 belongs to whoever else uses the cell.
//     drivers/nvmem/qcom-spmi-sdam.c:36-52 documents the same cell as plain
//     byte-addressed SDAM memory.
//
// Linux programs that cell through nvmem-reboot-mode only when a mode is
// requested. Nothing upstream says who clears it afterwards; the first live
// run has to read 0x7148 before and after a normal reboot to find out. This
// core therefore writes the cell only for an explicit recovery/bootloader
// request and never silently clears it on a normal restart.
//
// EDL (Emergency Download, the 9008 mode) is NOT one of that cell's modes.
// pmk8550.dtsi declares mode-recovery and mode-bootloader only, so no SDAM
// value stands for EDL on this board: writing a guessed byte into the cell
// would be an invented hardware fact. The path that is verified on this
// device is PSCI's vendor reset type instead:
//   - PSCI_1_1_RESET_TYPE_VENDOR_START is 0x80000000 (include/uapi/linux/
//     psci.h), and the vendor firmware on TB710FU maps that "reboot mode" to
//     EDL with cookie 1. The board's vendor reboot-mode node carries
//     mode-edl = <0x80000000 0x1>, and this repository's pl19-aes-edl run
//     reached 9008 (COM3) from COM5 with SYSTEM_RESET2(0x80000000, 1).
//   - 0x80000001 is the second vendor type the same vendor firmware accepts;
//     the ladder tries it next, then falls back to a plain SYSTEM_RESET, so a
//     future firmware that drops the vendor types still restarts instead of
//     hanging.
// Passing reset_type 0 (SYSTEM_WARM_RESET) instead of 0x80000000 ignores the
// cookie and performs an architectural warm reset - the trap this ladder
// avoids.
//
// Qualcomm's PON block is not written here. qcom-pon.c@v6.12 only registers
// reboot-mode and its pwrkey/resin children; it has no power-off or reset
// register, and the board DTS leaves pon@1300 disabled. Shutdown and restart
// are PSCI calls; the PMIC acts downstream of the secure monitor.
#ifndef TB710FU_PMK8550_RESET_H
#define TB710FU_PMK8550_RESET_H

#include "Tb710fuSpmiArbiter.h"

#define TB710FU_PSCI_VERSION 0x84000000u
#define TB710FU_PSCI_FEATURES 0x8400000au
#define TB710FU_PSCI_SYSTEM_OFF 0x84000008u
#define TB710FU_PSCI_SYSTEM_RESET 0x84000009u
#define TB710FU_PSCI_SYSTEM_RESET2_32 0x84000012u
#define TB710FU_PSCI_SYSTEM_RESET2_64 0xc4000012u
#define TB710FU_PSCI_RESET_TYPE_WARM 0u
/* PSCI_1_1_RESET_TYPE_VENDOR_START; the board's mode-edl reset type. */
#define TB710FU_PSCI_RESET_TYPE_VENDOR_START 0x80000000u
#define TB710FU_PSCI_RESET_TYPE_EDL 0x80000000u
#define TB710FU_PSCI_RESET_TYPE_EDL_ALT 0x80000001u
#define TB710FU_PSCI_RESET_EDL_COOKIE 1u

#define TB710FU_PSCI_SUCCESS 0
#define TB710FU_PSCI_NOT_SUPPORTED (-1)
#define TB710FU_PSCI_INVALID_PARAMETERS (-2)
#define TB710FU_PSCI_DENIED (-3)
#define TB710FU_PSCI_ALREADY_ON (-4)
#define TB710FU_PSCI_ON_PENDING (-5)
#define TB710FU_PSCI_INTERNAL_FAILURE (-6)
#define TB710FU_PSCI_NOT_PRESENT (-7)
#define TB710FU_PSCI_DISABLED (-8)
#define TB710FU_PSCI_INVALID_ADDRESS (-9)

/* bits = <1 7>: bit 0 is not ours, bits 7:1 hold the mode. */
#define TB710FU_REBOOT_REASON_SHIFT 1u
#define TB710FU_REBOOT_REASON_MASK 0xfeu
#define TB710FU_REBOOT_REASON_MAX 0x7fu

enum tb710fu_reboot_mode {
    TB710FU_REBOOT_MODE_NONE = 0x00,
    TB710FU_REBOOT_MODE_RECOVERY = 0x01,
    TB710FU_REBOOT_MODE_BOOTLOADER = 0x02
};

/* The secure monitor call. x0 carries the function, x1..x3 the arguments and
 * x0 comes back as the result. The kernel glue implements this with `smc #0`;
 * the host model records the arguments. */
struct tb710fu_smc_io {
    void *context;
    int64_t (*call)(void *context, uint64_t function, uint64_t arg0,
                    uint64_t arg1, uint64_t arg2);
};

struct tb710fu_psci_caps {
    uint32_t version;
    int system_off;
    int system_reset;
    int reset2_32;
    int reset2_64;
};

struct tb710fu_reset_call {
    uint64_t function; /* the ID actually invoked */
    uint64_t reset_type;
    uint64_t cookie;
    int64_t result;
    int used_reset2; /* 1 when SYSTEM_RESET2 replaced SYSTEM_RESET */
    int edl_vendor;  /* 0 none, 1 type 0x80000000, 2 type 0x80000001 */
};

struct tb710fu_reboot_cell {
    uint8_t before;
    uint8_t wanted;
    uint8_t after;
    int written;
};

/* What the diagnose stage prints: the raw byte, the seven-bit mode field it
 * encodes (mode-recovery 1, mode-bootloader 2, 0 none) and the execution
 * environment the arbiter says owns the cell. */
struct tb710fu_reboot_state {
    uint8_t raw;
    uint8_t mode;
    unsigned owner;
};

enum tb710fu_cell_result {
    TB710FU_CELL_OK = 0,
    TB710FU_CELL_CURRENT,    /* the cell already held the requested mode */
    TB710FU_CELL_UNSTABLE,   /* the cell changed while it was read */
    TB710FU_CELL_NOT_OWNER,  /* the SDAM belongs to another execution environment */
    TB710FU_CELL_DENIED,     /* the arbiter reported a denied transfer */
    TB710FU_CELL_FAILED,     /* the arbiter reported a failed transfer */
    TB710FU_CELL_DROPPED,    /* the arbiter reported a dropped transfer */
    TB710FU_CELL_TIMEOUT,    /* the arbiter never reported DONE */
    TB710FU_CELL_NO_DEVICE,  /* SDAM2 is not mapped */
    TB710FU_CELL_MISMATCH,   /* the cell read back differs from what was written */
    TB710FU_CELL_READ_ONLY,  /* the write gate refused the cell */
    TB710FU_CELL_ARGUMENT
};

enum tb710fu_power_action {
    TB710FU_POWER_OFF,               /* cold power down, no SPMI write */
    TB710FU_POWER_RESTART,           /* plain restart, no SPMI write */
    TB710FU_POWER_RESTART_RECOVERY,  /* 0x7148 = 0x02, then restart */
    TB710FU_POWER_RESTART_BOOTLOADER,/* 0x7148 = 0x04, then restart */
    TB710FU_POWER_RESTART_EDL        /* no SPMI write: SYSTEM_RESET2 vendor type */
};

enum tb710fu_power_result {
    TB710FU_POWER_SMC_RETURNED = 0, /* the SMC returned; see the report */
    TB710FU_POWER_REFUSED_REASON,   /* the cell write failed; nothing was called */
    TB710FU_POWER_REFUSED_DEVICE,   /* SDAM2 is not mapped; nothing was called */
    TB710FU_POWER_ARGUMENT
};

struct tb710fu_power_report {
    int stage; /* 0 none, 1 reason cell, 2 secure monitor */
    int reason_result;
    uint8_t reason_before;
    uint8_t reason_after;
    int smc_called;
    uint64_t smc_function;
    uint64_t smc_reset_type;
    uint64_t smc_cookie;
    int used_reset2;
    int edl_vendor;
    int64_t smc_result;
};

/* Probe PSCI. Features are queried exactly as UniXNU-sheng's BootReset.c does
 * before it trusts SYSTEM_RESET. */
int tb710fu_psci_probe(const struct tb710fu_smc_io *smc,
                       struct tb710fu_psci_caps *caps);

/* Cold power-off. Returns the SMC result; on real hardware SYSTEM_OFF does not
 * return. */
int tb710fu_psci_poweroff(const struct tb710fu_smc_io *smc, int64_t *result);

/* SYSTEM_RESET, and SYSTEM_RESET2 (64-bit then 32-bit) only when the first
 * call reports NOT_SUPPORTED. caps may be NULL. */
int tb710fu_psci_restart(const struct tb710fu_smc_io *smc,
                         const struct tb710fu_psci_caps *caps,
                         struct tb710fu_reset_call *call);

/* The EDL ladder: SYSTEM_RESET2(0x80000000, 1), then (0x80000001, 1), then a
 * plain restart. The vendor types are architectural, so a monitor that does
 * not implement them answers NOT_SUPPORTED and the next rung is tried; the
 * last rung is tb710fu_psci_restart(), which itself falls back to
 * SYSTEM_RESET. call is filled with the last attempt actually made. */
int tb710fu_psci_restart_edl(const struct tb710fu_smc_io *smc,
                             const struct tb710fu_psci_caps *caps,
                             struct tb710fu_reset_call *call);

/* Program the SDAM2 reboot-reason cell. mode is the nvmem value (recovery 1,
 * bootloader 2, 0 clears); the raw byte is (before & ~0xfe) | (mode << 1),
 * and the result is read back. */
int tb710fu_reboot_reason_write(const struct tb710fu_spmi_io *io, unsigned ee,
                                unsigned mode,
                                struct tb710fu_reboot_cell *cell);

/* Read the cell without writing it: the diagnose stage's "what did the last
 * boot ask for" row. Reads are not gated by ownership, but the owner is
 * reported so the log shows a cell another execution environment controls. */
int tb710fu_reboot_reason_read(const struct tb710fu_spmi_io *io, unsigned ee,
                               struct tb710fu_reboot_state *state);

/* One call for the platform expert: refuse-or-program the reason cell and then
 * hand over to PSCI. */
int tb710fu_power_action(const struct tb710fu_spmi_io *io, unsigned ee,
                         const struct tb710fu_smc_io *smc,
                         const struct tb710fu_psci_caps *caps,
                         enum tb710fu_power_action action,
                         struct tb710fu_power_report *report);

const char *tb710fu_cell_result_name(int result);
const char *tb710fu_power_result_name(int result);

#endif
