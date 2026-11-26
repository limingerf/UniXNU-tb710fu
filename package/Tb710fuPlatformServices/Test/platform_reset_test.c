// SPDX-License-Identifier: BSD-2-Clause-Patent
// platform_reset_test.c - PSCI power-off/restart and the SDAM2 reboot reason.
//
// The fake secure monitor records every SMC and returns the values the test
// asks for, so the fallback order (SYSTEM_RESET -> RESET2 64-bit -> 32-bit)
// and the "refuse before reset" rule are observable without hardware.
#include "../Tb710fuPmk8550Reset.h"
#include "host_spmi_model.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct fake_smc {
    int64_t version;
    int64_t feature_off;
    int64_t feature_reset;
    int64_t feature_reset2_64;
    int64_t feature_reset2_32;
    int64_t result_off;
    int64_t result_reset;
    int64_t result_reset2_64;
    int64_t result_reset2_32;
    unsigned calls;
    uint64_t seen_function[8];
    uint64_t seen_arg0[8];
    uint64_t seen_arg1[8];
    uint64_t seen_arg2[8];
};

static struct host_spmi_model model;
static struct tb710fu_spmi_io g_io;

static int64_t fake_smc_call(void *context, uint64_t function, uint64_t arg0,
                             uint64_t arg1, uint64_t arg2)
{
    struct fake_smc *fake = context;
    const unsigned index = fake->calls;

    assert(index < sizeof(fake->seen_function) / sizeof(fake->seen_function[0]));
    fake->seen_function[index] = function;
    fake->seen_arg0[index] = arg0;
    fake->seen_arg1[index] = arg1;
    fake->seen_arg2[index] = arg2;
    ++fake->calls;

    switch (function) {
    case TB710FU_PSCI_VERSION:
        return fake->version;
    case TB710FU_PSCI_FEATURES:
        switch (arg0) {
        case TB710FU_PSCI_SYSTEM_OFF:
            return fake->feature_off;
        case TB710FU_PSCI_SYSTEM_RESET:
            return fake->feature_reset;
        case TB710FU_PSCI_SYSTEM_RESET2_64:
            return fake->feature_reset2_64;
        case TB710FU_PSCI_SYSTEM_RESET2_32:
            return fake->feature_reset2_32;
        default:
            return TB710FU_PSCI_NOT_SUPPORTED;
        }
    case TB710FU_PSCI_SYSTEM_OFF:
        return fake->result_off;
    case TB710FU_PSCI_SYSTEM_RESET:
        return fake->result_reset;
    case TB710FU_PSCI_SYSTEM_RESET2_64:
        return fake->result_reset2_64;
    case TB710FU_PSCI_SYSTEM_RESET2_32:
        return fake->result_reset2_32;
    default:
        return TB710FU_PSCI_NOT_SUPPORTED;
    }
}

static void fake_init(struct fake_smc *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->version = 0x00010001;
    fake->result_off = 0;
    fake->result_reset = 0;
    fake->result_reset2_64 = TB710FU_PSCI_NOT_SUPPORTED;
    fake->result_reset2_32 = TB710FU_PSCI_NOT_SUPPORTED;
}

static struct tb710fu_smc_io smc_io(struct fake_smc *fake)
{
    struct tb710fu_smc_io smc;

    smc.context = fake;
    smc.call = fake_smc_call;
    return smc;
}

static void fixture(void)
{
    host_spmi_init(&model);
    g_io = host_spmi_io(&model);
}

static void test_psci_probe(void)
{
    struct fake_smc fake;
    struct tb710fu_smc_io smc;
    struct tb710fu_psci_caps caps;
    int rc;

    fake_init(&fake);
    smc = smc_io(&fake);
    rc = tb710fu_psci_probe(&smc, &caps);
    assert(rc == TB710FU_POWER_SMC_RETURNED);
    assert(caps.version == 0x00010001u);
    assert(caps.system_off == 1);
    assert(caps.system_reset == 1);
    assert(caps.reset2_64 == 1);
    assert(caps.reset2_32 == 1);
    assert(fake.calls == 5u);
    assert(fake.seen_function[0] == TB710FU_PSCI_VERSION);
    assert(fake.seen_function[1] == TB710FU_PSCI_FEATURES);
    assert(fake.seen_arg0[1] == TB710FU_PSCI_SYSTEM_OFF);
    assert(fake.seen_arg0[2] == TB710FU_PSCI_SYSTEM_RESET);
    assert(fake.seen_arg0[3] == TB710FU_PSCI_SYSTEM_RESET2_64);
    assert(fake.seen_arg0[4] == TB710FU_PSCI_SYSTEM_RESET2_32);

    fake_init(&fake);
    fake.version = TB710FU_PSCI_NOT_SUPPORTED;
    assert(fake.seen_arg0[0] == 0u);
    smc = smc_io(&fake);
    memset(&caps, 0xa5, sizeof(caps));
    assert(tb710fu_psci_probe(&smc, &caps) == TB710FU_PSCI_NOT_SUPPORTED);
    assert(caps.version == 0u);
    assert(caps.system_off == 0);
    assert(fake.calls == 1u);

    fake_init(&fake);
    fake.feature_off = TB710FU_PSCI_NOT_SUPPORTED;
    fake.feature_reset2_64 = TB710FU_PSCI_NOT_SUPPORTED;
    smc = smc_io(&fake);
    assert(tb710fu_psci_probe(&smc, &caps) == TB710FU_POWER_SMC_RETURNED);
    assert(caps.system_off == 0);
    assert(caps.system_reset == 1);
    assert(caps.reset2_64 == 0);
    assert(caps.reset2_32 == 1);

    assert(tb710fu_psci_probe(NULL, &caps) == TB710FU_POWER_ARGUMENT);
    assert(tb710fu_psci_probe(&smc, NULL) == TB710FU_POWER_ARGUMENT);
    {
        struct tb710fu_smc_io broken = {NULL, NULL};

        assert(tb710fu_psci_probe(&broken, &caps) == TB710FU_POWER_ARGUMENT);
    }
}

static void test_poweroff(void)
{
    struct fake_smc fake;
    struct tb710fu_smc_io smc;
    int64_t result = 0;

    fake_init(&fake);
    fake.result_off = 0;
    smc = smc_io(&fake);
    assert(tb710fu_psci_poweroff(&smc, &result) == 0);
    assert(result == 0);
    assert(fake.calls == 1u);
    assert(fake.seen_function[0] == TB710FU_PSCI_SYSTEM_OFF);
    assert(fake.seen_arg0[0] == 0u);
    assert(fake.seen_arg1[0] == 0u);
    assert(fake.seen_arg2[0] == 0u);

    fake_init(&fake);
    fake.result_off = TB710FU_PSCI_DENIED;
    smc = smc_io(&fake);
    result = 1;
    assert(tb710fu_psci_poweroff(&smc, &result) == TB710FU_PSCI_DENIED);
    assert(result == TB710FU_PSCI_DENIED);

    /* The result pointer is optional. */
    assert(tb710fu_psci_poweroff(&smc, NULL) == TB710FU_PSCI_DENIED);
    assert(tb710fu_psci_poweroff(NULL, &result) == TB710FU_POWER_ARGUMENT);
}

static void test_restart_order(void)
{
    struct fake_smc fake;
    struct tb710fu_smc_io smc;
    struct tb710fu_psci_caps caps;
    struct tb710fu_reset_call call;
    int rc;

    fake_init(&fake);
    memset(&caps, 0, sizeof(caps));
    caps.system_reset = 1;
    caps.reset2_64 = 1;
    caps.reset2_32 = 1;
    smc = smc_io(&fake);
    rc = tb710fu_psci_restart(&smc, &caps, &call);
    assert(rc == 0);
    assert(call.function == TB710FU_PSCI_SYSTEM_RESET);
    assert(call.reset_type == 0u);
    assert(call.used_reset2 == 0);
    assert(call.result == 0);
    assert(fake.calls == 1u);

    /* SYSTEM_RESET unsupported: the 64-bit RESET2 form is the first fallback. */
    fake_init(&fake);
    fake.result_reset = TB710FU_PSCI_NOT_SUPPORTED;
    fake.result_reset2_64 = 0;
    smc = smc_io(&fake);
    memset(&call, 0xa5, sizeof(call));
    rc = tb710fu_psci_restart(&smc, &caps, &call);
    assert(rc == 0);
    assert(call.function == TB710FU_PSCI_SYSTEM_RESET2_64);
    assert(call.reset_type == TB710FU_PSCI_RESET_TYPE_WARM);
    assert(call.used_reset2 == 1);
    assert(fake.calls == 2u);
    assert(fake.seen_arg0[1] == TB710FU_PSCI_RESET_TYPE_WARM);

    /* Only the 32-bit form is supported. */
    fake_init(&fake);
    fake.result_reset = TB710FU_PSCI_NOT_SUPPORTED;
    fake.result_reset2_64 = TB710FU_PSCI_NOT_SUPPORTED;
    fake.result_reset2_32 = 0;
    caps.reset2_64 = 0;
    caps.reset2_32 = 1;
    smc = smc_io(&fake);
    rc = tb710fu_psci_restart(&smc, &caps, &call);
    assert(rc == 0);
    assert(call.function == TB710FU_PSCI_SYSTEM_RESET2_32);
    assert(call.used_reset2 == 1);
    assert(fake.calls == 2u);

    /* Neither fallback is supported: the last call is reported as failed. */
    fake_init(&fake);
    fake.result_reset = TB710FU_PSCI_NOT_SUPPORTED;
    fake.result_reset2_64 = TB710FU_PSCI_NOT_SUPPORTED;
    fake.result_reset2_32 = TB710FU_PSCI_NOT_SUPPORTED;
    smc = smc_io(&fake);
    rc = tb710fu_psci_restart(&smc, NULL, &call);
    assert(rc == TB710FU_PSCI_NOT_SUPPORTED);
    assert(call.function == TB710FU_PSCI_SYSTEM_RESET2_32);
    assert(call.used_reset2 == 0);
    assert(fake.calls == 3u);

    /* A monitor that denies the 64-bit form must not be asked for 32-bit. */
    fake_init(&fake);
    fake.result_reset = TB710FU_PSCI_NOT_SUPPORTED;
    fake.result_reset2_64 = TB710FU_PSCI_DENIED;
    caps.reset2_64 = 1;
    caps.reset2_32 = 1;
    smc = smc_io(&fake);
    rc = tb710fu_psci_restart(&smc, &caps, &call);
    assert(rc == TB710FU_PSCI_DENIED);
    assert(call.function == TB710FU_PSCI_SYSTEM_RESET2_64);
    assert(call.used_reset2 == 1);
    assert(fake.calls == 2u);

    /* call is optional, and the caps pointer gates each fallback. */
    fake_init(&fake);
    fake.result_reset = TB710FU_PSCI_NOT_SUPPORTED;
    fake.result_reset2_64 = TB710FU_PSCI_NOT_SUPPORTED;
    fake.result_reset2_32 = 0;
    smc = smc_io(&fake);
    caps.reset2_64 = 0;
    caps.reset2_32 = 1;
    assert(tb710fu_psci_restart(&smc, &caps, NULL) == 0);
    assert(tb710fu_psci_restart(NULL, &caps, &call) ==
           TB710FU_POWER_ARGUMENT);
}

static void test_reboot_reason(void)
{
    struct tb710fu_reboot_cell cell;

    fixture();
    assert(tb710fu_reboot_reason_write(&g_io, 0u,
                                       TB710FU_REBOOT_MODE_RECOVERY,
                                       &cell) == TB710FU_CELL_OK);
    assert(cell.before == 0x00u);
    assert(cell.wanted == 0x02u);
    assert(cell.after == 0x02u);
    assert(cell.written == 1);
    assert(model.reason_values[0] == 0x02u);
    assert(model.reason_writes == 1u);

    assert(tb710fu_reboot_reason_write(&g_io, 0u,
                                       TB710FU_REBOOT_MODE_RECOVERY,
                                       &cell) == TB710FU_CELL_CURRENT);
    assert(cell.before == 0x02u);
    assert(cell.wanted == 0x02u);
    assert(cell.after == 0x02u);
    assert(cell.written == 0);
    assert(model.reason_writes == 1u);

    assert(tb710fu_reboot_reason_write(&g_io, 0u,
                                       TB710FU_REBOOT_MODE_BOOTLOADER,
                                       &cell) == TB710FU_CELL_OK);
    assert(cell.before == 0x02u);
    assert(cell.wanted == 0x04u);
    assert(cell.after == 0x04u);

    /* bits = <1 7>: bit 0 belongs to whoever else uses the cell. */
    host_spmi_set_reason(&model, 0x01u);
    assert(tb710fu_reboot_reason_write(&g_io, 0u,
                                       TB710FU_REBOOT_MODE_RECOVERY,
                                       &cell) == TB710FU_CELL_OK);
    assert(cell.before == 0x01u);
    assert(cell.wanted == 0x03u);
    assert(cell.after == 0x03u);

    /* mode 0 clears the seven-bit field and keeps bit 0. */
    host_spmi_set_reason(&model, 0x04u);
    assert(tb710fu_reboot_reason_write(&g_io, 0u, 0u, &cell) ==
           TB710FU_CELL_OK);
    assert(cell.wanted == 0x00u);
    assert(cell.after == 0x00u);

    host_spmi_set_reason(&model, 0x00u);
    assert(tb710fu_reboot_reason_write(&g_io, 0u, 0x7fu, &cell) ==
           TB710FU_CELL_OK);
    assert(cell.wanted == 0xfeu);

    assert(tb710fu_reboot_reason_write(&g_io, 0u, 0x80u, &cell) ==
           TB710FU_CELL_ARGUMENT);
    assert(tb710fu_reboot_reason_write(&g_io, 8u,
                                       TB710FU_REBOOT_MODE_RECOVERY,
                                       &cell) == TB710FU_CELL_ARGUMENT);
    assert(tb710fu_reboot_reason_write(NULL, 0u,
                                       TB710FU_REBOOT_MODE_RECOVERY,
                                       &cell) == TB710FU_CELL_ARGUMENT);
    /* cell is optional. */
    assert(tb710fu_reboot_reason_write(&g_io, 0u,
                                       TB710FU_REBOOT_MODE_RECOVERY,
                                       NULL) == TB710FU_CELL_OK);
}

static void test_reboot_reason_guards(void)
{
    struct tb710fu_reboot_cell cell;

    fixture();
    host_spmi_unmap(&model, 3u);
    assert(tb710fu_reboot_reason_write(&g_io, 0u,
                                       TB710FU_REBOOT_MODE_RECOVERY,
                                       &cell) == TB710FU_CELL_NO_DEVICE);

    fixture();
    host_spmi_set_owner(&model, 3u, 1u);
    assert(tb710fu_reboot_reason_write(&g_io, 0u,
                                       TB710FU_REBOOT_MODE_RECOVERY,
                                       &cell) == TB710FU_CELL_NOT_OWNER);

    fixture();
    host_spmi_set_error(&model, HOST_SPMI_ERROR_DENIED);
    assert(tb710fu_reboot_reason_write(&g_io, 0u,
                                       TB710FU_REBOOT_MODE_RECOVERY,
                                       &cell) == TB710FU_CELL_DENIED);

    fixture();
    host_spmi_set_error(&model, HOST_SPMI_ERROR_FAILURE);
    assert(tb710fu_reboot_reason_write(&g_io, 0u,
                                       TB710FU_REBOOT_MODE_RECOVERY,
                                       &cell) == TB710FU_CELL_FAILED);

    fixture();
    host_spmi_set_error(&model, HOST_SPMI_ERROR_DROPPED);
    assert(tb710fu_reboot_reason_write(&g_io, 0u,
                                       TB710FU_REBOOT_MODE_RECOVERY,
                                       &cell) == TB710FU_CELL_DROPPED);

    fixture();
    host_spmi_set_drop_writes(&model, 1);
    assert(tb710fu_reboot_reason_write(&g_io, 0u,
                                       TB710FU_REBOOT_MODE_RECOVERY,
                                       &cell) == TB710FU_CELL_MISMATCH);
    assert(cell.written == 1);
    assert(cell.after == 0x00u);
    assert(model.reason_writes == 1u);

    fixture();
    {
        const uint32_t reasons[] = {0x00u, 0x01u, 0x02u, 0x03u,
                                    0x04u, 0x05u, 0x06u, 0x07u};

        host_spmi_set_reason_sequence(&model, reasons, 8u);
        assert(tb710fu_reboot_reason_write(&g_io, 0u,
                                           TB710FU_REBOOT_MODE_RECOVERY,
                                           &cell) == TB710FU_CELL_UNSTABLE);
    }
}

static void test_power_action(void)
{
    struct fake_smc fake;
    struct tb710fu_smc_io smc;
    struct tb710fu_psci_caps caps;
    struct tb710fu_power_report report;
    int rc;

    fake_init(&fake);
    smc = smc_io(&fake);
    memset(&caps, 0, sizeof(caps));
    caps.system_reset = 1;
    caps.reset2_64 = 1;
    caps.reset2_32 = 1;

    /* Power-off and plain restart never touch SPMI. */
    fixture();
    rc = tb710fu_power_action(&g_io, 0u, &smc, &caps, TB710FU_POWER_OFF,
                              &report);
    assert(rc == 0);
    assert(report.stage == 2);
    assert(report.smc_called == 1);
    assert(report.smc_function == TB710FU_PSCI_SYSTEM_OFF);
    assert(report.smc_result == 0);
    assert(model.commands == 0u);

    fixture();
    rc = tb710fu_power_action(&g_io, 0u, &smc, &caps, TB710FU_POWER_RESTART,
                              &report);
    assert(rc == 0);
    assert(report.stage == 2);
    assert(report.smc_function == TB710FU_PSCI_SYSTEM_RESET);
    assert(report.smc_result == 0);
    assert(model.commands == 0u);

    /* Recovery: the reason cell is programmed before the reset call. */
    fixture();
    rc = tb710fu_power_action(&g_io, 0u, &smc, &caps,
                              TB710FU_POWER_RESTART_RECOVERY, &report);
    assert(rc == 0);
    assert(report.stage == 2);
    assert(report.reason_result == TB710FU_CELL_OK);
    assert(report.reason_before == 0x00u);
    assert(report.reason_after == 0x02u);
    assert(model.reason_values[0] == 0x02u);
    assert(report.smc_function == TB710FU_PSCI_SYSTEM_RESET);

    fixture();
    rc = tb710fu_power_action(&g_io, 0u, &smc, &caps,
                              TB710FU_POWER_RESTART_BOOTLOADER, &report);
    assert(rc == 0);
    assert(report.reason_after == 0x04u);

    /* An already-programmed cell is a success, not a second write. */
    fixture();
    host_spmi_set_reason(&model, 0x02u);
    rc = tb710fu_power_action(&g_io, 0u, &smc, &caps,
                              TB710FU_POWER_RESTART_RECOVERY, &report);
    assert(rc == 0);
    assert(report.reason_result == TB710FU_CELL_CURRENT);
    assert(model.reason_writes == 0u);

    /* Owning the SDAM is not optional for the reason path. */
    fixture();
    fake_init(&fake);
    smc = smc_io(&fake);
    host_spmi_set_owner(&model, 3u, 1u);
    rc = tb710fu_power_action(&g_io, 0u, &smc, &caps,
                              TB710FU_POWER_RESTART_RECOVERY, &report);
    assert(rc == TB710FU_POWER_REFUSED_DEVICE);
    assert(report.stage == 1);
    assert(report.reason_result == TB710FU_CELL_NOT_OWNER);
    assert(report.smc_called == 0);
    assert(fake.calls == 0u);

    fixture();
    host_spmi_unmap(&model, 3u);
    rc = tb710fu_power_action(&g_io, 0u, &smc, &caps,
                              TB710FU_POWER_RESTART_RECOVERY, &report);
    assert(rc == TB710FU_POWER_REFUSED_DEVICE);
    assert(report.reason_result == TB710FU_CELL_NO_DEVICE);

    fixture();
    host_spmi_set_error(&model, HOST_SPMI_ERROR_DENIED);
    rc = tb710fu_power_action(&g_io, 0u, &smc, &caps,
                              TB710FU_POWER_RESTART_RECOVERY, &report);
    assert(rc == TB710FU_POWER_REFUSED_REASON);
    assert(report.reason_result == TB710FU_CELL_DENIED);
    assert(report.smc_called == 0);

    fixture();
    host_spmi_set_drop_writes(&model, 1);
    rc = tb710fu_power_action(&g_io, 0u, &smc, &caps,
                              TB710FU_POWER_RESTART_RECOVERY, &report);
    assert(rc == TB710FU_POWER_REFUSED_REASON);
    assert(report.reason_result == TB710FU_CELL_MISMATCH);

    /* A plain action still works without the arbiter mapping. */
    fake_init(&fake);
    smc = smc_io(&fake);
    rc = tb710fu_power_action(NULL, 0u, &smc, &caps, TB710FU_POWER_OFF,
                              &report);
    assert(rc == 0);
    rc = tb710fu_power_action(NULL, 0u, &smc, &caps,
                              TB710FU_POWER_RESTART_RECOVERY, &report);
    assert(rc == TB710FU_POWER_REFUSED_DEVICE);

    assert(tb710fu_power_action(&g_io, 0u, &smc, &caps,
                                (enum tb710fu_power_action)99, &report) ==
           TB710FU_POWER_ARGUMENT);
    assert(tb710fu_power_action(&g_io, 0u, NULL, &caps, TB710FU_POWER_OFF,
                                &report) == TB710FU_POWER_ARGUMENT);
    /* report is optional. */
    assert(tb710fu_power_action(&g_io, 0u, &smc, &caps, TB710FU_POWER_OFF,
                                NULL) == 0);
}

static void test_edl(void)
{
    struct fake_smc fake;
    struct tb710fu_smc_io smc;
    struct tb710fu_psci_caps caps;
    struct tb710fu_reset_call call;
    struct tb710fu_power_report report;

    /* No SPMI at all: the EDL ladder never programs the board's reboot cell,
     * because pmk8550.dtsi gives it no EDL mode. */
    fake_init(&fake);
    fake.result_reset2_64 = 0; /* the vendor firmware accepts 0x80000000 */
    smc = smc_io(&fake);
    assert(tb710fu_psci_restart_edl(&smc, NULL, &call) == 0);
    assert(fake.calls == 1u);
    assert(fake.seen_function[0] == TB710FU_PSCI_SYSTEM_RESET2_64);
    assert(fake.seen_arg0[0] == 0x80000000u);
    assert(fake.seen_arg1[0] == 1u); /* the vendor cookie */
    assert(call.used_reset2 == 1);
    assert(call.edl_vendor == 1);
    assert(call.reset_type == TB710FU_PSCI_RESET_TYPE_EDL);
    assert(call.cookie == 1u);

    /* Neither vendor type exists: both rungs are tried, then the plain
     * restart's own ladder (64-bit, then 32-bit SYSTEM_RESET2), so a firmware
     * that drops the vendor types still restarts instead of hanging. */
    fake_init(&fake);
    fake.result_reset2_64 = TB710FU_PSCI_NOT_SUPPORTED;
    fake.result_reset = TB710FU_PSCI_NOT_SUPPORTED;
    fake.result_reset2_32 = 0;
    smc = smc_io(&fake);
    assert(tb710fu_psci_restart_edl(&smc, NULL, &call) == 0);
    assert(call.edl_vendor == 0);
    assert(call.function == TB710FU_PSCI_SYSTEM_RESET2_32);
    assert(call.reset_type == TB710FU_PSCI_RESET_TYPE_WARM);
    assert(call.cookie == 0u);
    assert(fake.calls == 5u); /* two vendor rungs, then SYSTEM_RESET, RESET2
                               * 64-bit and RESET2 32-bit */
    assert(fake.seen_function[0] == TB710FU_PSCI_SYSTEM_RESET2_64);
    assert(fake.seen_arg0[0] == 0x80000000u);
    assert(fake.seen_arg1[0] == 1u);
    assert(fake.seen_arg0[1] == 0x80000001u);
    assert(fake.seen_arg1[1] == 1u);
    assert(fake.seen_function[2] == TB710FU_PSCI_SYSTEM_RESET);
    assert(fake.seen_arg0[4] == TB710FU_PSCI_RESET_TYPE_WARM);

    /* The action path: EDL never touches SDAM2, so it works with no device
     * mapped (the platform expert must still be able to reach 9008 from a
     * machine whose PMIC arbiter did not map). */
    fake_init(&fake);
    fake.result_reset2_64 = 0;
    smc = smc_io(&fake);
    caps.version = 0x00010001;
    caps.system_off = 0;
    caps.system_reset = 1;
    caps.reset2_32 = 1;
    caps.reset2_64 = 1;
    assert(tb710fu_power_action(NULL, 0u, &smc, &caps,
                                TB710FU_POWER_RESTART_EDL, &report) == 0);
    assert(report.stage == 2);
    assert(report.reason_result == 0);
    assert(report.edl_vendor == 1);
    assert(report.smc_function == TB710FU_PSCI_SYSTEM_RESET2_64);
    assert(report.smc_reset_type == 0x80000000u);
    assert(report.smc_cookie == 1u);
}

static void test_names(void)
{
    const int cells[] = {
        TB710FU_CELL_OK,        TB710FU_CELL_CURRENT,
        TB710FU_CELL_UNSTABLE,  TB710FU_CELL_NOT_OWNER,
        TB710FU_CELL_DENIED,    TB710FU_CELL_FAILED,
        TB710FU_CELL_DROPPED,   TB710FU_CELL_TIMEOUT,
        TB710FU_CELL_NO_DEVICE, TB710FU_CELL_MISMATCH,
        TB710FU_CELL_READ_ONLY, TB710FU_CELL_ARGUMENT,
    };
    const int powers[] = {
        TB710FU_POWER_SMC_RETURNED,   TB710FU_POWER_REFUSED_REASON,
        TB710FU_POWER_REFUSED_DEVICE, TB710FU_POWER_ARGUMENT,
        TB710FU_PSCI_NOT_SUPPORTED,   TB710FU_PSCI_INVALID_PARAMETERS,
        TB710FU_PSCI_DENIED,          TB710FU_PSCI_ALREADY_ON,
        TB710FU_PSCI_ON_PENDING,      TB710FU_PSCI_INTERNAL_FAILURE,
        TB710FU_PSCI_NOT_PRESENT,     TB710FU_PSCI_DISABLED,
        TB710FU_PSCI_INVALID_ADDRESS,
    };
    size_t i;

    for (i = 0; i < sizeof(cells) / sizeof(cells[0]); ++i) {
        const char *name = tb710fu_cell_result_name(cells[i]);

        assert(name != NULL);
        assert(name[0] != '\0');
    }
    for (i = 0; i < sizeof(powers) / sizeof(powers[0]); ++i) {
        const char *name = tb710fu_power_result_name(powers[i]);

        assert(name != NULL);
        assert(name[0] != '\0');
    }
    assert(strcmp(tb710fu_cell_result_name(TB710FU_CELL_OK),
                  tb710fu_cell_result_name(TB710FU_CELL_MISMATCH)) != 0);
    assert(tb710fu_cell_result_name(9999) != NULL);
    assert(tb710fu_power_result_name(9999) != NULL);
}

int main(void)
{
    printf("platform_reset_test: PSCI and the SDAM2 reboot reason\n");
    test_psci_probe();
    printf("ok  PSCI probe and feature bits\n");
    test_poweroff();
    printf("ok  SYSTEM_OFF\n");
    test_restart_order();
    printf("ok  reset fallback order and caps\n");
    test_reboot_reason();
    printf("ok  reboot-reason encoding and current/clear\n");
    test_reboot_reason_guards();
    printf("ok  reboot-reason ownership, errors and instability\n");
    test_power_action();
    printf("ok  power action sequencing and refusals\n");
    test_edl();
    printf("ok  EDL ladder and the action path\n");
    test_names();
    printf("ok  result names\n");
    printf("platform_reset_test: PASS\n");
    return 0;
}
