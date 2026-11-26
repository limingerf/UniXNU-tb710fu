// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tb710fuPmk8550Reset.c - power-off, restart and the SDAM2 reboot reason.
//
// The upstream origin of every register and every SMC function is documented
// in Tb710fuPmk8550Reset.h. This file only sequences them:
//
//   power off            -> PSCI SYSTEM_OFF, no SPMI access at all
//   restart              -> PSCI SYSTEM_RESET, no SPMI access at all
//   restart-recovery     -> SDAM2 0x48 = 0x02, read back, then SYSTEM_RESET
//   restart-bootloader   -> SDAM2 0x48 = 0x04, read back, then SYSTEM_RESET
//
// A refused reason write stops before the reset: leaving the previous reason
// in the cell is harmless, resetting with a half-written one is not.
#include "Tb710fuPmk8550Reset.h"

#include <stddef.h>
#include <string.h>

#define TB710FU_REASON_TRIES 4u

static int tb710fu_cell_from_spmi(int result)
{
    switch (result) {
    case TB710FU_SPMI_OK: return TB710FU_CELL_OK;
    case TB710FU_SPMI_FAILED: return TB710FU_CELL_FAILED;
    case TB710FU_SPMI_DENIED: return TB710FU_CELL_DENIED;
    case TB710FU_SPMI_DROPPED: return TB710FU_CELL_DROPPED;
    case TB710FU_SPMI_TIMEOUT: return TB710FU_CELL_TIMEOUT;
    case TB710FU_SPMI_NO_DEVICE: return TB710FU_CELL_NO_DEVICE;
    case TB710FU_SPMI_BAD_APID: return TB710FU_CELL_NO_DEVICE;
    case TB710FU_SPMI_NOT_OWNER: return TB710FU_CELL_NOT_OWNER;
    case TB710FU_SPMI_READ_ONLY: return TB710FU_CELL_READ_ONLY;
    case TB710FU_SPMI_ARGUMENT: return TB710FU_CELL_ARGUMENT;
    }
    return TB710FU_CELL_FAILED;
}

static int tb710fu_reason_read_byte(const struct tb710fu_spmi_io *io,
                                    unsigned ee, unsigned apid, uint8_t *value)
{
    uint32_t first = 0;
    uint32_t second = 0;

    for (unsigned attempt = 0; attempt < TB710FU_REASON_TRIES; ++attempt) {
        int rc = tb710fu_cell_from_spmi(tb710fu_spmi_read(
            io, ee, apid, (uint8_t)TB710FU_PMK8550_SDAM2_REBOOT_REASON, 1u,
            &first, NULL));
        if (rc != TB710FU_CELL_OK)
            return rc;
        rc = tb710fu_cell_from_spmi(tb710fu_spmi_read(
            io, ee, apid, (uint8_t)TB710FU_PMK8550_SDAM2_REBOOT_REASON, 1u,
            &second, NULL));
        if (rc != TB710FU_CELL_OK)
            return rc;
        if (first == second) {
            *value = (uint8_t)first;
            return TB710FU_CELL_OK;
        }
    }
    return TB710FU_CELL_UNSTABLE;
}

int tb710fu_psci_probe(const struct tb710fu_smc_io *smc,
                       struct tb710fu_psci_caps *caps)
{
    int64_t version;
    int64_t features;

    if (smc == NULL || caps == NULL || smc->call == NULL)
        return TB710FU_POWER_ARGUMENT;
    memset(caps, 0, sizeof(*caps));
    version = smc->call(smc->context, TB710FU_PSCI_VERSION, 0u, 0u, 0u);
    if (version < 0)
        return (int)version; /* no PSCI at all */
    caps->version = (uint32_t)version;
    features = smc->call(smc->context, TB710FU_PSCI_FEATURES,
                         TB710FU_PSCI_SYSTEM_OFF, 0u, 0u);
    caps->system_off = features == 0;
    features = smc->call(smc->context, TB710FU_PSCI_FEATURES,
                         TB710FU_PSCI_SYSTEM_RESET, 0u, 0u);
    caps->system_reset = features == 0;
    features = smc->call(smc->context, TB710FU_PSCI_FEATURES,
                         TB710FU_PSCI_SYSTEM_RESET2_64, 0u, 0u);
    caps->reset2_64 = features == 0;
    features = smc->call(smc->context, TB710FU_PSCI_FEATURES,
                         TB710FU_PSCI_SYSTEM_RESET2_32, 0u, 0u);
    caps->reset2_32 = features == 0;
    return TB710FU_POWER_SMC_RETURNED;
}

int tb710fu_psci_poweroff(const struct tb710fu_smc_io *smc, int64_t *result)
{
    int64_t raw;

    if (smc == NULL || smc->call == NULL)
        return TB710FU_POWER_ARGUMENT;
    /* A real SYSTEM_OFF never comes back; reaching the next line means the
     * monitor refused it and the caller must keep the machine alive. */
    raw = smc->call(smc->context, TB710FU_PSCI_SYSTEM_OFF, 0u, 0u, 0u);
    if (result != NULL)
        *result = raw;
    return (int)raw;
}

int tb710fu_psci_restart(const struct tb710fu_smc_io *smc,
                         const struct tb710fu_psci_caps *caps,
                         struct tb710fu_reset_call *call)
{
    struct tb710fu_reset_call local;
    int64_t raw;

    if (smc == NULL || smc->call == NULL)
        return TB710FU_POWER_ARGUMENT;
    if (call == NULL)
        call = &local;
    memset(call, 0, sizeof(*call));
    call->function = TB710FU_PSCI_SYSTEM_RESET;
    raw = smc->call(smc->context, TB710FU_PSCI_SYSTEM_RESET, 0u, 0u, 0u);
    call->result = raw;
    if (raw != TB710FU_PSCI_NOT_SUPPORTED)
        return (int)raw;
    /* Only a monitor that says NOT_SUPPORTED for SYSTEM_RESET is asked for
     * SYSTEM_RESET2, 64-bit form first (UniXNU-sheng probes the same order). */
    if (caps == NULL || caps->reset2_64) {
        call->function = TB710FU_PSCI_SYSTEM_RESET2_64;
        call->reset_type = TB710FU_PSCI_RESET_TYPE_WARM;
        call->result = smc->call(smc->context, TB710FU_PSCI_SYSTEM_RESET2_64,
                                 TB710FU_PSCI_RESET_TYPE_WARM, 0u, 0u);
        if (call->result != TB710FU_PSCI_NOT_SUPPORTED) {
            call->used_reset2 = 1;
            return (int)call->result;
        }
    }
    if (caps == NULL || caps->reset2_32) {
        call->function = TB710FU_PSCI_SYSTEM_RESET2_32;
        call->reset_type = TB710FU_PSCI_RESET_TYPE_WARM;
        call->result = smc->call(smc->context, TB710FU_PSCI_SYSTEM_RESET2_32,
                                 TB710FU_PSCI_RESET_TYPE_WARM, 0u, 0u);
        if (call->result != TB710FU_PSCI_NOT_SUPPORTED) {
            call->used_reset2 = 1;
            return (int)call->result;
        }
    }
    return (int)call->result;
}

int tb710fu_psci_restart_edl(const struct tb710fu_smc_io *smc,
                             const struct tb710fu_psci_caps *caps,
                             struct tb710fu_reset_call *call)
{
    struct tb710fu_reset_call local;

    if (smc == NULL || smc->call == NULL)
        return TB710FU_POWER_ARGUMENT;
    if (call == NULL)
        call = &local;
    memset(call, 0, sizeof(*call));

    /* Both vendor types only exist in the 64-bit SYSTEM_RESET2 form; the
     * 32-bit form (0x84000012) cannot carry them on this platform, whose
     * psci node is a 64-bit SMC conduit. */
    const uint64_t types[] = {
        TB710FU_PSCI_RESET_TYPE_EDL, TB710FU_PSCI_RESET_TYPE_EDL_ALT
    };
    for (unsigned index = 0; index < 2u; ++index) {
        call->function = TB710FU_PSCI_SYSTEM_RESET2_64;
        call->reset_type = types[index];
        call->cookie = TB710FU_PSCI_RESET_EDL_COOKIE;
        call->result = smc->call(smc->context, TB710FU_PSCI_SYSTEM_RESET2_64,
                                 types[index], TB710FU_PSCI_RESET_EDL_COOKIE,
                                 0u);
        call->used_reset2 = 1;
        call->edl_vendor = (int)index + 1;
        if (call->result != TB710FU_PSCI_NOT_SUPPORTED)
            return (int)call->result;
    }
    /* Neither vendor type is implemented: restart rather than hang. */
    call->edl_vendor = 0;
    return tb710fu_psci_restart(smc, caps, call);
}

int tb710fu_reboot_reason_write(const struct tb710fu_spmi_io *io, unsigned ee,
                                unsigned mode,
                                struct tb710fu_reboot_cell *cell)
{
    struct tb710fu_spmi_peripheral peripheral;
    struct tb710fu_reboot_cell local;
    uint8_t before = 0;
    uint8_t after = 0;
    uint8_t wanted;
    unsigned owner = 0;
    int rc;

    if (io == NULL || ee > TB710FU_SPMI_MAX_EE ||
        mode > TB710FU_REBOOT_REASON_MAX)
        return TB710FU_CELL_ARGUMENT;
    if (cell == NULL)
        cell = &local;
    memset(cell, 0, sizeof(*cell));
    rc = tb710fu_cell_from_spmi(
        tb710fu_spmi_find(io, ee, TB710FU_PMK8550_SDAM2_USID, &peripheral));
    if (rc != TB710FU_CELL_OK)
        return rc;
    rc = tb710fu_cell_from_spmi(
        tb710fu_spmi_ownership(io, peripheral.apid, &owner));
    if (rc != TB710FU_CELL_OK)
        return rc;
    if (owner != ee)
        return TB710FU_CELL_NOT_OWNER;
    rc = tb710fu_reason_read_byte(io, ee, peripheral.apid, &before);
    if (rc != TB710FU_CELL_OK)
        return rc;
    /* bits = <1 7>: keep bit 0, replace the seven-bit mode field. */
    wanted = (uint8_t)((before & (uint8_t)~TB710FU_REBOOT_REASON_MASK) |
                       (uint8_t)((mode << TB710FU_REBOOT_REASON_SHIFT) &
                                 TB710FU_REBOOT_REASON_MASK));
    cell->before = before;
    cell->wanted = wanted;
    if (wanted == before) {
        cell->after = before;
        return TB710FU_CELL_CURRENT;
    }
    rc = tb710fu_cell_from_spmi(tb710fu_spmi_write_apid(
        io, ee, peripheral.apid,
        (uint8_t)TB710FU_PMK8550_SDAM2_REBOOT_REASON, 1u, wanted, NULL));
    if (rc != TB710FU_CELL_OK)
        return rc;
    cell->written = 1;
    rc = tb710fu_reason_read_byte(io, ee, peripheral.apid, &after);
    if (rc != TB710FU_CELL_OK)
        return rc;
    cell->after = after;
    if (after != wanted)
        return TB710FU_CELL_MISMATCH;
    return TB710FU_CELL_OK;
}

int tb710fu_reboot_reason_read(const struct tb710fu_spmi_io *io, unsigned ee,
                               struct tb710fu_reboot_state *state)
{
    struct tb710fu_spmi_peripheral peripheral;
    struct tb710fu_reboot_state local;
    uint8_t raw = 0;
    unsigned owner = 0;
    int rc;

    if (io == NULL || ee > TB710FU_SPMI_MAX_EE)
        return TB710FU_CELL_ARGUMENT;
    if (state == NULL)
        state = &local;
    memset(state, 0, sizeof(*state));
    rc = tb710fu_cell_from_spmi(
        tb710fu_spmi_find(io, ee, TB710FU_PMK8550_SDAM2_USID, &peripheral));
    if (rc != TB710FU_CELL_OK)
        return rc;
    rc = tb710fu_cell_from_spmi(
        tb710fu_spmi_ownership(io, peripheral.apid, &owner));
    if (rc != TB710FU_CELL_OK)
        return rc;
    state->owner = owner;
    rc = tb710fu_reason_read_byte(io, ee, peripheral.apid, &raw);
    if (rc != TB710FU_CELL_OK)
        return rc;
    state->raw = raw;
    state->mode = (uint8_t)((raw & TB710FU_REBOOT_REASON_MASK) >>
                            TB710FU_REBOOT_REASON_SHIFT);
    return TB710FU_CELL_OK;
}

int tb710fu_power_action(const struct tb710fu_spmi_io *io, unsigned ee,
                         const struct tb710fu_smc_io *smc,
                         const struct tb710fu_psci_caps *caps,
                         enum tb710fu_power_action action,
                         struct tb710fu_power_report *report)
{
    struct tb710fu_power_report local;
    struct tb710fu_reboot_cell cell;
    struct tb710fu_reset_call call;
    unsigned mode = 0;
    int rc;

    if (smc == NULL || smc->call == NULL)
        return TB710FU_POWER_ARGUMENT;
    if (report == NULL)
        report = &local;
    memset(report, 0, sizeof(*report));

    switch (action) {
    case TB710FU_POWER_OFF:
        break;
    case TB710FU_POWER_RESTART:
        break;
    case TB710FU_POWER_RESTART_RECOVERY:
        mode = TB710FU_REBOOT_MODE_RECOVERY;
        break;
    case TB710FU_POWER_RESTART_BOOTLOADER:
        mode = TB710FU_REBOOT_MODE_BOOTLOADER;
        break;
    case TB710FU_POWER_RESTART_EDL:
        mode = 0u; /* the board's cell has no EDL mode; see the header */
        break;
    default:
        return TB710FU_POWER_ARGUMENT;
    }

    if (mode != 0u) {
        report->stage = 1;
        rc = tb710fu_reboot_reason_write(io, ee, mode, &cell);
        report->reason_result = rc;
        report->reason_before = cell.before;
        report->reason_after = cell.after;
        if (rc == TB710FU_CELL_NOT_OWNER || rc == TB710FU_CELL_NO_DEVICE ||
            rc == TB710FU_CELL_ARGUMENT)
            return TB710FU_POWER_REFUSED_DEVICE;
        if (rc != TB710FU_CELL_OK && rc != TB710FU_CELL_CURRENT)
            return TB710FU_POWER_REFUSED_REASON;
    }
    report->stage = 2;
    report->smc_called = 1;
    if (action == TB710FU_POWER_OFF) {
        report->smc_function = TB710FU_PSCI_SYSTEM_OFF;
        report->smc_result = 0;
        return tb710fu_psci_poweroff(smc, &report->smc_result);
    }
    if (action == TB710FU_POWER_RESTART_EDL)
        rc = tb710fu_psci_restart_edl(smc, caps, &call);
    else
        rc = tb710fu_psci_restart(smc, caps, &call);
    report->smc_function = call.function;
    report->smc_reset_type = call.reset_type;
    report->smc_cookie = call.cookie;
    report->smc_result = call.result;
    report->used_reset2 = call.used_reset2;
    report->edl_vendor = call.edl_vendor;
    return rc;
}

const char *tb710fu_cell_result_name(int result)
{
    switch (result) {
    case TB710FU_CELL_OK: return "ok";
    case TB710FU_CELL_CURRENT: return "current";
    case TB710FU_CELL_UNSTABLE: return "unstable";
    case TB710FU_CELL_NOT_OWNER: return "not-owner";
    case TB710FU_CELL_DENIED: return "denied";
    case TB710FU_CELL_FAILED: return "failed";
    case TB710FU_CELL_DROPPED: return "dropped";
    case TB710FU_CELL_TIMEOUT: return "timeout";
    case TB710FU_CELL_NO_DEVICE: return "no-device";
    case TB710FU_CELL_MISMATCH: return "mismatch";
    case TB710FU_CELL_READ_ONLY: return "read-only";
    case TB710FU_CELL_ARGUMENT: return "argument";
    }
    return "unknown";
}

const char *tb710fu_power_result_name(int result)
{
    switch (result) {
    case TB710FU_POWER_SMC_RETURNED: return "smc-returned";
    case TB710FU_POWER_REFUSED_REASON: return "refused-reason";
    case TB710FU_POWER_REFUSED_DEVICE: return "refused-device";
    case TB710FU_POWER_ARGUMENT: return "argument";
    case TB710FU_PSCI_NOT_SUPPORTED: return "psci-not-supported";
    case TB710FU_PSCI_INVALID_PARAMETERS: return "psci-invalid-parameters";
    case TB710FU_PSCI_DENIED: return "psci-denied";
    case TB710FU_PSCI_ALREADY_ON: return "psci-already-on";
    case TB710FU_PSCI_ON_PENDING: return "psci-on-pending";
    case TB710FU_PSCI_INTERNAL_FAILURE: return "psci-internal-failure";
    case TB710FU_PSCI_NOT_PRESENT: return "psci-not-present";
    case TB710FU_PSCI_DISABLED: return "psci-disabled";
    case TB710FU_PSCI_INVALID_ADDRESS: return "psci-invalid-address";
    }
    return "unknown";
}
