// SPDX-License-Identifier: BSD-2-Clause-Patent
// platform_spmi_test.c - PMIC-arbiter v7 core, write whitelist and the MMIO
// transport. The RTC and reset tests use the same host model through the
// callback transport; this file also drives the real Tb710fuSpmiMmio.c over
// the model's register arrays.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../Tb710fuPmk8550Reset.h"
#include "../Tb710fuSpmiArbiter.h"
#include "../Tb710fuSpmiMmio.h"
#include "host_spmi_model.h"

#define TEST_COUNTER 0x65000000u
#define TEST_CELL 0x01234567u

static struct host_spmi_model model;
static struct host_mmio_bridge bridge;

static struct tb710fu_spmi_io reset_model(void)
{
    host_spmi_init(&model);
    return host_spmi_io(&model);
}

static void test_command_words(void)
{
    assert(tb710fu_spmi_command(TB710FU_SPMI_OP_EXT_READL, 0x48u, 4u) ==
           0x08000483u);
    assert(tb710fu_spmi_command(TB710FU_SPMI_OP_EXT_WRITEL, 0xbcu, 4u) ==
           0x00000bc3u);
    assert(tb710fu_spmi_command(TB710FU_SPMI_OP_EXT_READL, 0x46u, 1u) ==
           0x08000460u);
    /* The byte address is eight bits; a wider address cannot leak into the
     * opcode or byte-count fields. */
    assert(tb710fu_spmi_command(TB710FU_SPMI_OP_EXT_READL, 0x1ffu, 3u) ==
           0x08000ff2u);
    assert(tb710fu_spmi_command(31u, 0u, 4u) == 0xf8000003u);
}

static void test_names(void)
{
    const int results[] = {
        TB710FU_SPMI_OK,      TB710FU_SPMI_FAILED,   TB710FU_SPMI_DENIED,
        TB710FU_SPMI_DROPPED, TB710FU_SPMI_TIMEOUT,  TB710FU_SPMI_NO_DEVICE,
        TB710FU_SPMI_BAD_APID, TB710FU_SPMI_NOT_OWNER,
        TB710FU_SPMI_READ_ONLY, TB710FU_SPMI_ARGUMENT,
    };
    const int denials[] = {
        TB710FU_SPMI_DENY_NONE,        TB710FU_SPMI_DENY_CHARGER_SLAVE,
        TB710FU_SPMI_DENY_FOREIGN_SLAVE, TB710FU_SPMI_DENY_RTC_BLOCK,
        TB710FU_SPMI_DENY_UNLISTED_CELL,
    };
    size_t i;

    for (i = 0; i < sizeof(results) / sizeof(results[0]); ++i) {
        const char *name = tb710fu_spmi_result_name(results[i]);

        assert(name != NULL);
        assert(name[0] != '\0');
        assert(strcmp(name, tb710fu_spmi_result_name(results[i])) == 0);
    }
    assert(strcmp(tb710fu_spmi_result_name(TB710FU_SPMI_FAILED),
                  tb710fu_spmi_result_name(TB710FU_SPMI_DENIED)) != 0);
    assert(tb710fu_spmi_result_name(12345) != NULL);
    for (i = 0; i < sizeof(denials) / sizeof(denials[0]); ++i) {
        const char *name = tb710fu_spmi_denial_name(denials[i]);

        assert(name != NULL);
        assert(name[0] != '\0');
    }
    assert(strcmp(tb710fu_spmi_denial_name(TB710FU_SPMI_DENY_RTC_BLOCK),
                  tb710fu_spmi_denial_name(TB710FU_SPMI_DENY_UNLISTED_CELL)) !=
           0);
    assert(tb710fu_spmi_denial_name(999) != NULL);
}

static void test_probe(void)
{
    struct tb710fu_spmi_bus bus;
    struct tb710fu_spmi_io io;

    io = reset_model();
    assert(tb710fu_spmi_probe(NULL, &bus) == TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_probe(&io, NULL) == TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_probe(&io, &bus) == TB710FU_SPMI_OK);
    assert(bus.version == TB710FU_SPMI_VERSION_V7_MIN);
    assert(bus.base_apid == 0u);
    assert(bus.apid_count == HOST_SPMI_APIDS);

    model.core[TB710FU_SPMI_VERSION / 4u] = 0x6fffffffu;
    assert(tb710fu_spmi_probe(&io, &bus) == TB710FU_SPMI_NO_DEVICE);

    io = reset_model();
    model.core[TB710FU_SPMI_VERSION / 4u] = TB710FU_SPMI_VERSION_V8_MIN;
    assert(tb710fu_spmi_probe(&io, &bus) == TB710FU_SPMI_NO_DEVICE);

    io = reset_model();
    model.core[TB710FU_SPMI_VERSION / 4u] = TB710FU_SPMI_VERSION_V7_MIN + 1u;
    assert(tb710fu_spmi_probe(&io, &bus) == TB710FU_SPMI_OK);

    io = reset_model();
    model.core[TB710FU_SPMI_FEATURES / 4u] = 0u;
    assert(tb710fu_spmi_probe(&io, &bus) == TB710FU_SPMI_NO_DEVICE);

    io = reset_model();
    model.core[TB710FU_SPMI_FEATURES / 4u] = 1024u;
    assert(tb710fu_spmi_probe(&io, &bus) == TB710FU_SPMI_OK);
    assert(bus.apid_count == 1024u);

    io = reset_model();
    model.core[TB710FU_SPMI_FEATURES / 4u] = 1025u;
    assert(tb710fu_spmi_probe(&io, &bus) == TB710FU_SPMI_NO_DEVICE);

    io = reset_model();
    model.core[TB710FU_SPMI_FEATURES / 4u] = 0x7ffu;
    assert(tb710fu_spmi_probe(&io, &bus) == TB710FU_SPMI_NO_DEVICE);

    /* A foreign layout is refused, not guessed at with a different base. */
    io = reset_model();
    model.core[TB710FU_SPMI_VERSION / 4u] = 0x50000000u;
    assert(tb710fu_spmi_probe(&io, &bus) == TB710FU_SPMI_NO_DEVICE);
}

static void test_apid_table(void)
{
    struct tb710fu_spmi_peripheral p;
    struct tb710fu_spmi_io io;
    unsigned usid = 0;
    unsigned owner = 0;

    io = reset_model();
    assert(tb710fu_spmi_lookup_apid(NULL, 2u, &usid) ==
           TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_lookup_apid(&io, 2u, NULL) == TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_lookup_apid(&io, 2000u, &usid) ==
           TB710FU_SPMI_BAD_APID);
    assert(tb710fu_spmi_lookup_apid(&io, 2u, &usid) == TB710FU_SPMI_OK);
    assert(usid == TB710FU_PMK8550_RTC_USID);
    assert(tb710fu_spmi_lookup_apid(&io, 3u, &usid) == TB710FU_SPMI_OK);
    assert(usid == TB710FU_PMK8550_SDAM2_USID);
    assert(tb710fu_spmi_lookup_apid(&io, 0u, &usid) ==
           TB710FU_SPMI_NO_DEVICE);
    assert(tb710fu_spmi_lookup_apid(&io, HOST_SPMI_APIDS, &usid) ==
           TB710FU_SPMI_BAD_APID);

    host_spmi_set_owner(&model, 2u, 5u);
    assert(tb710fu_spmi_ownership(&io, 2u, &owner) == TB710FU_SPMI_OK);
    assert(owner == 5u);
    model.config[2] = 0xfffffffdu; /* the gate only looks at bits 2:0 */
    assert(tb710fu_spmi_ownership(&io, 2u, &owner) == TB710FU_SPMI_OK);
    assert(owner == 5u);
    assert(tb710fu_spmi_ownership(&io, 2000u, &owner) ==
           TB710FU_SPMI_BAD_APID);
    assert(tb710fu_spmi_ownership(&io, HOST_SPMI_APIDS, &owner) ==
           TB710FU_SPMI_BAD_APID);
    assert(tb710fu_spmi_ownership(&io, 2u, NULL) == TB710FU_SPMI_ARGUMENT);

    /* Duplicate PPIDs are legal. A later mapping owned by this EE wins; the
     * first mapping wins otherwise. */
    io = reset_model();
    host_spmi_map(&model, 4u, TB710FU_PMK8550_RTC_USID);
    host_spmi_set_owner(&model, 4u, 1u);
    assert(tb710fu_spmi_find(&io, 0u, TB710FU_PMK8550_RTC_USID, &p) ==
           TB710FU_SPMI_OK);
    assert(p.apid == 2u && p.owner == 0u);
    assert(tb710fu_spmi_find(&io, 1u, TB710FU_PMK8550_RTC_USID, &p) ==
           TB710FU_SPMI_OK);
    assert(p.apid == 4u && p.owner == 1u);
    host_spmi_set_owner(&model, 2u, 2u);
    assert(tb710fu_spmi_find(&io, 1u, TB710FU_PMK8550_RTC_USID, &p) ==
           TB710FU_SPMI_OK);
    assert(p.apid == 4u);
    assert(tb710fu_spmi_find(&io, 2u, TB710FU_PMK8550_RTC_USID, &p) ==
           TB710FU_SPMI_OK);
    assert(p.apid == 2u);

    assert(tb710fu_spmi_find(&io, 0u, 0xfffu, &p) == TB710FU_SPMI_NO_DEVICE);
    assert(tb710fu_spmi_find(&io, 0u, 0x1000u, &p) ==
           TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_find(&io, 8u, TB710FU_PMK8550_RTC_USID, &p) ==
           TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_find(NULL, 0u, TB710FU_PMK8550_RTC_USID, &p) ==
           TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_find(&io, 0u, TB710FU_PMK8550_RTC_USID, NULL) ==
           TB710FU_SPMI_ARGUMENT);
}

static void test_read_path(void)
{
    struct tb710fu_spmi_io io;
    uint32_t value = 0;
    unsigned polls = 0;

    io = reset_model();
    host_spmi_set_owner(&model, 2u, 7u); /* reads ignore ownership */
    assert(tb710fu_spmi_read(&io, 0u, 2u, 0x46u, 1u, &value, &polls) ==
           TB710FU_SPMI_OK);
    assert(value == TB710FU_PMK8550_RTC_RUNNING);
    assert(polls == 1u);
    assert(model.read_commands == 1u);
    assert(model.pmic_reads == 1u);

    host_spmi_set_cell(&model, 0xa5c3f00fu);
    value = 0;
    assert(tb710fu_spmi_read(&io, 0u, 3u, 0xbcu, 1u, &value, NULL) ==
           TB710FU_SPMI_OK);
    assert(value == 0x0fu);
    assert(tb710fu_spmi_read(&io, 0u, 3u, 0xbcu, 2u, &value, NULL) ==
           TB710FU_SPMI_OK);
    assert(value == 0xf00fu);
    assert(tb710fu_spmi_read(&io, 0u, 3u, 0xbcu, 3u, &value, NULL) ==
           TB710FU_SPMI_OK);
    assert(value == 0xc3f00fu);
    assert(tb710fu_spmi_read(&io, 0u, 3u, 0xbcu, 4u, &value, NULL) ==
           TB710FU_SPMI_OK);
    assert(value == 0xa5c3f00fu);

    /* A non-owning EE still reads its own observer channel. */
    assert(tb710fu_spmi_read(&io, 7u, 2u, 0x46u, 1u, &value, NULL) ==
           TB710FU_SPMI_OK);

    /* Argument and range rejects happen before any transfer. */
    assert(tb710fu_spmi_read(NULL, 0u, 2u, 0x46u, 1u, &value, NULL) ==
           TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_read(&io, 0u, 2u, 0x46u, 1u, NULL, NULL) ==
           TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_read(&io, 0u, 2u, 0x46u, 0u, &value, NULL) ==
           TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_read(&io, 0u, 2u, 0x46u, 5u, &value, NULL) ==
           TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_read(&io, 8u, 2u, 0x46u, 1u, &value, NULL) ==
           TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_read(&io, 0u, 2000u, 0x46u, 1u, &value, NULL) ==
           TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_read(&io, 0u, HOST_SPMI_APIDS, 0x46u, 1u, &value,
                             NULL) == TB710FU_SPMI_BAD_APID);

    host_spmi_set_error(&model, HOST_SPMI_ERROR_FAILURE);
    assert(tb710fu_spmi_read(&io, 0u, 2u, 0x46u, 1u, &value, &polls) ==
           TB710FU_SPMI_FAILED);
    host_spmi_set_error(&model, HOST_SPMI_ERROR_DENIED);
    assert(tb710fu_spmi_read(&io, 0u, 2u, 0x46u, 1u, &value, NULL) ==
           TB710FU_SPMI_DENIED);
    host_spmi_set_error(&model, HOST_SPMI_ERROR_DROPPED);
    assert(tb710fu_spmi_read(&io, 0u, 2u, 0x46u, 1u, &value, NULL) ==
           TB710FU_SPMI_DROPPED);

    host_spmi_set_error(&model, HOST_SPMI_ERROR_SILENT);
    polls = 0;
    assert(tb710fu_spmi_read(&io, 0u, 2u, 0x46u, 1u, &value, &polls) ==
           TB710FU_SPMI_TIMEOUT);
    assert(polls == TB710FU_SPMI_POLL_LIMIT);
}

static void test_write_gate(void)
{
    struct tb710fu_spmi_io io;
    unsigned policy;

    io = reset_model();
    /* Policy is pure: no I/O at all. */
    assert(tb710fu_spmi_write_target(TB710FU_PMK8550_SDAM2_USID, 0x48u, 1u) ==
           TB710FU_SPMI_WRITE_TARGET_SDAM_REBOOT_REASON);
    assert(tb710fu_spmi_write_target(TB710FU_PMK8550_SDAM2_USID, 0xbcu, 4u) ==
           TB710FU_SPMI_WRITE_TARGET_SDAM_RTC_OFFSET);
    assert(tb710fu_spmi_write_target(TB710FU_PMK8550_SDAM2_USID, 0x48u, 2u) ==
           TB710FU_SPMI_WRITE_TARGET_NONE);
    assert(tb710fu_spmi_write_target(TB710FU_PMK8550_SDAM2_USID, 0xbcu, 1u) ==
           TB710FU_SPMI_WRITE_TARGET_NONE);
    assert(tb710fu_spmi_write_target(TB710FU_PMK8550_SDAM2_USID, 0xbcu, 8u) ==
           TB710FU_SPMI_WRITE_TARGET_NONE);
    assert(tb710fu_spmi_write_target(TB710FU_PMK8550_SDAM2_USID, 0x47u, 1u) ==
           TB710FU_SPMI_WRITE_TARGET_NONE);
    assert(tb710fu_spmi_write_target(TB710FU_PMK8550_RTC_USID, 0x46u, 1u) ==
           TB710FU_SPMI_WRITE_TARGET_NONE);
    assert(tb710fu_spmi_write_target(TB710FU_SPMI_USID(7u, 0x13u), 0x00u, 1u) ==
           TB710FU_SPMI_WRITE_TARGET_NONE);
    assert(tb710fu_spmi_write_target(0u, 0u, 1u) ==
           TB710FU_SPMI_WRITE_TARGET_NONE);

    assert(tb710fu_spmi_write_denial(TB710FU_SPMI_USID(7u, 0x13u), 0u, 1u) ==
           TB710FU_SPMI_DENY_CHARGER_SLAVE);
    assert(tb710fu_spmi_write_denial(TB710FU_SPMI_USID(1u, 0x13u), 0u, 1u) ==
           TB710FU_SPMI_DENY_FOREIGN_SLAVE);
    assert(tb710fu_spmi_write_denial(TB710FU_PMK8550_RTC_USID, 0x46u, 1u) ==
           TB710FU_SPMI_DENY_RTC_BLOCK);
    assert(tb710fu_spmi_write_denial(TB710FU_PMK8550_RTC_ALARM_PPID,
                                     0x00u, 1u) ==
           TB710FU_SPMI_DENY_RTC_BLOCK);
    assert(tb710fu_spmi_write_denial(TB710FU_PMK8550_SDAM2_USID, 0x47u, 1u) ==
           TB710FU_SPMI_DENY_UNLISTED_CELL);
    assert(tb710fu_spmi_write_denial(TB710FU_PMK8550_SDAM2_USID, 0x48u, 1u) ==
           TB710FU_SPMI_DENY_NONE);

    /* The USID path refuses the RTC block before any transfer. */
    assert(tb710fu_spmi_write(&io, 0u, TB710FU_PMK8550_RTC_USID, 0x46u, 1u,
                              0u, NULL) == TB710FU_SPMI_READ_ONLY);
    assert(model.commands == 0u);
    /* A mapped foreign USID reaches the same table and is refused there, not
     * silently accepted because the APID itself is writable-looking. */
    host_spmi_map(&model, 6u, TB710FU_SPMI_USID(7u, 0x13u));
    host_spmi_set_owner(&model, 6u, 0u);
    assert(tb710fu_spmi_write(&io, 0u, TB710FU_SPMI_USID(7u, 0x13u), 0x00u,
                              1u, 0u, NULL) == TB710FU_SPMI_READ_ONLY);
    assert(model.commands == 0u);
    host_spmi_map(&model, 7u, TB710FU_SPMI_USID(1u, 0x13u));
    host_spmi_set_owner(&model, 7u, 0u);
    assert(tb710fu_spmi_write(&io, 0u, TB710FU_SPMI_USID(1u, 0x13u), 0x00u,
                              1u, 0u, NULL) == TB710FU_SPMI_READ_ONLY);

    /* APID-level writes cannot bypass the same table. */
    assert(tb710fu_spmi_write_apid(&io, 0u, 2u, 0x46u, 1u, 0u, NULL) ==
           TB710FU_SPMI_READ_ONLY);
    assert(model.commands == 0u);
    assert(tb710fu_spmi_write_apid(&io, 0u, 3u, 0xbcu, 1u, 0u, NULL) ==
           TB710FU_SPMI_READ_ONLY);

    /* Unknown USIDs and APIDs fail without a transfer. */
    assert(tb710fu_spmi_write(&io, 0u, 0xffeu, 0x00u, 1u, 0u, NULL) ==
           TB710FU_SPMI_NO_DEVICE);
    assert(tb710fu_spmi_write_apid(&io, 0u, 1u, 0x48u, 1u, 0u, NULL) ==
           TB710FU_SPMI_NO_DEVICE);
    assert(model.commands == 0u);

    /* Ownership is checked on the APID the write resolves to. */
    host_spmi_set_owner(&model, 3u, 1u);
    assert(tb710fu_spmi_write(&io, 0u, TB710FU_PMK8550_SDAM2_USID, 0x48u, 1u,
                              2u, NULL) == TB710FU_SPMI_NOT_OWNER);
    assert(model.commands == 0u);

    /* A duplicate mapping owned by this EE is preferred, so a write still
     * goes to an APID this EE owns. */
    host_spmi_map(&model, 5u, TB710FU_PMK8550_SDAM2_USID);
    host_spmi_set_owner(&model, 5u, 0u);
    assert(tb710fu_spmi_write(&io, 0u, TB710FU_PMK8550_SDAM2_USID, 0x48u, 1u,
                              3u, NULL) == TB710FU_SPMI_OK);
    assert(model.reason_values[0] == 3u);

    policy = tb710fu_spmi_write_target(TB710FU_PMK8550_SDAM2_USID, 0xbcu, 4u);
    assert(policy == TB710FU_SPMI_WRITE_TARGET_SDAM_RTC_OFFSET);
}

static void test_write_path(void)
{
    struct tb710fu_spmi_io io;
    unsigned polls = 0;

    io = reset_model();
    assert(tb710fu_spmi_write_apid(&io, 0u, 3u, 0x48u, 1u, 0x1ffu, &polls) ==
           TB710FU_SPMI_OK);
    assert(model.reason_values[0] == 0xffu); /* one byte, masked */
    assert(model.reason_writes == 1u);
    assert(polls == 1u);

    assert(tb710fu_spmi_write_apid(&io, 0u, 3u, 0xbcu, 4u, 0xdeadbeefu,
                                   NULL) == TB710FU_SPMI_OK);
    assert(model.cell_values[0] == 0xdeadbeefu);
    assert(model.cell_writes == 1u);

    host_spmi_set_error(&model, HOST_SPMI_ERROR_DENIED);
    assert(tb710fu_spmi_write_apid(&io, 0u, 3u, 0x48u, 1u, 1u, NULL) ==
           TB710FU_SPMI_DENIED);
    host_spmi_set_error(&model, HOST_SPMI_ERROR_FAILURE);
    assert(tb710fu_spmi_write_apid(&io, 0u, 3u, 0x48u, 1u, 1u, NULL) ==
           TB710FU_SPMI_FAILED);
    host_spmi_set_error(&model, HOST_SPMI_ERROR_DROPPED);
    assert(tb710fu_spmi_write_apid(&io, 0u, 3u, 0x48u, 1u, 1u, NULL) ==
           TB710FU_SPMI_DROPPED);

    /* A dropped write acknowledges before storing; the cell must not change. */
    host_spmi_set_error(&model, HOST_SPMI_ERROR_NONE);
    host_spmi_set_drop_writes(&model, 1);
    assert(tb710fu_spmi_write_apid(&io, 0u, 3u, 0x48u, 1u, 0x7fu, NULL) ==
           TB710FU_SPMI_OK);
    assert(model.reason_values[0] == 0xffu);
    assert(model.reason_writes == 2u);

    host_spmi_set_error(&model, HOST_SPMI_ERROR_SILENT);
    assert(tb710fu_spmi_write_apid(&io, 0u, 3u, 0x48u, 1u, 1u, &polls) ==
           TB710FU_SPMI_TIMEOUT);
    assert(polls == TB710FU_SPMI_POLL_LIMIT);
}

static void test_mmio_init(void)
{
    struct tb710fu_spmi_mmio mmio;

    assert(tb710fu_spmi_mmio_init(NULL, model.core,
                                  TB710FU_SPMI_CORE_BYTES, model.config,
                                  TB710FU_SPMI_CONFIG_BYTES, model.observer,
                                  TB710FU_SPMI_OBSERVER_BYTES, model.channel,
                                  TB710FU_SPMI_CHANNEL_BYTES, 0u) ==
           TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_mmio_init(&mmio, NULL, TB710FU_SPMI_CORE_BYTES,
                                  model.config, TB710FU_SPMI_CONFIG_BYTES,
                                  model.observer, TB710FU_SPMI_OBSERVER_BYTES,
                                  model.channel, TB710FU_SPMI_CHANNEL_BYTES,
                                  0u) == TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_mmio_init(&mmio, model.core,
                                  TB710FU_SPMI_CORE_BYTES - 4u, model.config,
                                  TB710FU_SPMI_CONFIG_BYTES, model.observer,
                                  TB710FU_SPMI_OBSERVER_BYTES, model.channel,
                                  TB710FU_SPMI_CHANNEL_BYTES, 0u) ==
           TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_mmio_init(&mmio, model.core,
                                  TB710FU_SPMI_CORE_BYTES, model.config,
                                  0xffcu,
                                  model.observer, TB710FU_SPMI_OBSERVER_BYTES,
                                  model.channel, TB710FU_SPMI_CHANNEL_BYTES,
                                  0u) == TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_mmio_init(&mmio, model.core,
                                  TB710FU_SPMI_CORE_BYTES, model.config,
                                  TB710FU_SPMI_CONFIG_BYTES, model.observer,
                                  0x7ffcu,
                                  model.channel, TB710FU_SPMI_CHANNEL_BYTES,
                                  0u) == TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_mmio_init(&mmio, model.core,
                                  TB710FU_SPMI_CORE_BYTES, model.config,
                                  TB710FU_SPMI_CONFIG_BYTES, model.observer,
                                  TB710FU_SPMI_OBSERVER_BYTES, model.channel,
                                  TB710FU_SPMI_CHANNEL_BYTES - 4u, 0u) ==
           TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_mmio_init(&mmio, model.core,
                                  TB710FU_SPMI_CORE_BYTES, model.config,
                                  TB710FU_SPMI_CONFIG_BYTES, model.observer,
                                  TB710FU_SPMI_OBSERVER_BYTES, model.channel,
                                  TB710FU_SPMI_CHANNEL_BYTES, 8u) ==
           TB710FU_SPMI_ARGUMENT);
    assert(tb710fu_spmi_mmio_init(
               &mmio, (uint32_t *)(void *)((uint8_t *)model.core + 1u),
               TB710FU_SPMI_CORE_BYTES, model.config,
               TB710FU_SPMI_CONFIG_BYTES, model.observer,
               TB710FU_SPMI_OBSERVER_BYTES, model.channel,
               TB710FU_SPMI_CHANNEL_BYTES, 0u) == TB710FU_SPMI_ARGUMENT);

    host_spmi_init(&model);
    assert(tb710fu_spmi_mmio_init(
               &mmio, model.core, TB710FU_SPMI_CORE_BYTES, model.config,
               TB710FU_SPMI_CONFIG_BYTES, model.observer,
               TB710FU_SPMI_OBSERVER_BYTES, model.channel,
               TB710FU_SPMI_CHANNEL_BYTES, 0u) == TB710FU_SPMI_OK);
    assert(mmio.io.context == &mmio);
    assert(tb710fu_spmi_mmio_faults(&mmio) == 0u);
}

static void test_mmio_faults(void)
{
    struct tb710fu_spmi_mmio mmio;
    uint32_t value;

    host_spmi_init(&model);
    assert(tb710fu_spmi_mmio_init(
               &mmio, model.core, TB710FU_SPMI_CORE_BYTES, model.config,
               TB710FU_SPMI_CONFIG_BYTES, model.observer,
               TB710FU_SPMI_OBSERVER_BYTES, model.channel,
               TB710FU_SPMI_CHANNEL_BYTES, 0u) == TB710FU_SPMI_OK);

    value = mmio.io.read32(mmio.io.context, TB710FU_SPMI_WINDOW_CORE,
                           TB710FU_SPMI_CORE_BYTES);
    assert(value == 0u);
    assert(tb710fu_spmi_mmio_faults(&mmio) == 1u);
    value = mmio.io.read32(mmio.io.context, TB710FU_SPMI_WINDOW_CORE,
                           TB710FU_SPMI_CORE_BYTES - 2u);
    assert(value == 0u);
    assert(tb710fu_spmi_mmio_faults(&mmio) == 2u);
    mmio.io.write32(mmio.io.context, TB710FU_SPMI_WINDOW_CONFIG,
                    TB710FU_SPMI_CONFIG_BYTES, 1u);
    assert(tb710fu_spmi_mmio_faults(&mmio) == 3u);
    value = mmio.io.read32(mmio.io.context, TB710FU_SPMI_WINDOW_OBSERVER,
                           TB710FU_SPMI_OBSERVER_BYTES);
    assert(value == 0u);
    assert(tb710fu_spmi_mmio_faults(&mmio) == 4u);
    mmio.io.write32(mmio.io.context, TB710FU_SPMI_WINDOW_CHANNEL,
                    TB710FU_SPMI_CHANNEL_BYTES, 1u);
    assert(tb710fu_spmi_mmio_faults(&mmio) == 5u);
    value = mmio.io.read32(mmio.io.context, TB710FU_SPMI_WINDOW_OBSERVER,
                           0x1002u);
    assert(value == 0u);
    assert(tb710fu_spmi_mmio_faults(&mmio) == 6u);

    value = mmio.io.read32(mmio.io.context, TB710FU_SPMI_WINDOW_CORE,
                           TB710FU_SPMI_VERSION);
    assert(value == TB710FU_SPMI_VERSION_V7_MIN);
    assert(tb710fu_spmi_mmio_faults(&mmio) == 6u);

    /* A core request that starts inside the window but runs past it is a
     * fault too, not a partial read. */
    value = mmio.io.read32(mmio.io.context, TB710FU_SPMI_WINDOW_CORE,
                           TB710FU_SPMI_CORE_BYTES - 4u + 1u);
    assert(value == 0u);
    assert(tb710fu_spmi_mmio_faults(&mmio) == 7u);
}

static void test_mmio_probe_and_rtc_read(void)
{
    struct tb710fu_spmi_bus bus;
    struct tb710fu_pmk8550_rtc clock;
    struct tb710fu_rtc_reading reading;
    int rc;

    host_spmi_init(&model);
    host_spmi_set_counter(&model, TEST_COUNTER);
    host_spmi_set_cell(&model, TEST_CELL);
    assert(host_mmio_bridge_init(&bridge, &model) == TB710FU_SPMI_OK);

    assert(tb710fu_spmi_probe(&bridge.bridge_io, &bus) == TB710FU_SPMI_OK);
    assert(bus.version == TB710FU_SPMI_VERSION_V7_MIN);
    assert(bus.apid_count == HOST_SPMI_APIDS);

    rc = tb710fu_rtc_open(&clock, &bridge.bridge_io, NULL, NULL);
    assert(rc == TB710FU_RTC_OK);
    assert(clock.sdam_valid == 1);
    assert(clock.rtc_apid == 2u);
    assert(clock.sdam_apid == 3u);

    assert(tb710fu_rtc_read(&clock, &reading) == TB710FU_RTC_OK);
    assert(reading.seconds_valid == 1);
    assert(reading.seconds ==
           (uint64_t)TEST_COUNTER + (uint64_t)TEST_CELL);
    assert(reading.counter == TEST_COUNTER);
    assert(reading.offset == TEST_CELL);
    assert(reading.useconds_valid == 0);
    assert(bridge.mmio.faults == 0u);
    /* Two control reads, one counter word plus its LSB, two cell words. */
    assert(model.read_commands == 6u);
}

static void test_mmio_reboot_reason(void)
{
    struct tb710fu_reboot_cell cell;

    host_spmi_init(&model);
    assert(host_mmio_bridge_init(&bridge, &model) == TB710FU_SPMI_OK);

    assert(tb710fu_reboot_reason_write(&bridge.bridge_io, 0u,
                                       TB710FU_REBOOT_MODE_RECOVERY,
                                       &cell) == TB710FU_CELL_OK);
    assert(cell.before == 0u);
    assert(cell.after == 0x02u);
    assert(cell.written == 1u);
    assert(model.reason_values[0] == 0x02u);
    assert(model.reason_writes == 1u);
    assert(bridge.mmio.faults == 0u);

    /* The read back after the write must go through the same mapped window. */
    assert(tb710fu_reboot_reason_write(&bridge.bridge_io, 0u,
                                       TB710FU_REBOOT_MODE_RECOVERY,
                                       &cell) == TB710FU_CELL_CURRENT);
    assert(cell.written == 0u);
    assert(model.reason_writes == 1u);

    assert(tb710fu_reboot_reason_write(&bridge.bridge_io, 0u,
                                       TB710FU_REBOOT_MODE_BOOTLOADER,
                                       &cell) == TB710FU_CELL_OK);
    assert(cell.before == 0x02u);
    assert(cell.after == 0x04u);
    assert(model.reason_values[0] == 0x04u);
}

static void test_mmio_gate_has_no_transfer(void)
{
    host_spmi_init(&model);
    assert(host_mmio_bridge_init(&bridge, &model) == TB710FU_SPMI_OK);
    assert(tb710fu_spmi_write_apid(&bridge.bridge_io, 0u, 2u, 0x46u, 1u, 0u,
                                   NULL) == TB710FU_SPMI_READ_ONLY);
    assert(model.commands == 0u);
    assert(bridge.mmio.faults == 0u);
    assert(model.channel[(TB710FU_SPMI_CHANNEL_APID_STRIDE * 2u +
                          TB710FU_SPMI_WDATA0) / 4u] == 0u);
}

int main(void)
{
    printf("platform_spmi_test: PMIC-arbiter v7 / write gate / MMIO\n");
    test_command_words();
    printf("ok  command encoding\n");
    test_names();
    printf("ok  result and denial names\n");
    test_probe();
    printf("ok  probe and layout rejection\n");
    test_apid_table();
    printf("ok  APID map, ownership and duplicate PPIDs\n");
    test_read_path();
    printf("ok  observer reads, masks and status errors\n");
    test_write_gate();
    printf("ok  write whitelist and ownership gate\n");
    test_write_path();
    printf("ok  channel writes, masks and status errors\n");
    test_mmio_init();
    printf("ok  MMIO init validation\n");
    test_mmio_faults();
    printf("ok  MMIO bounds and fault counter\n");
    test_mmio_probe_and_rtc_read();
    printf("ok  simulated MMIO RTC read end to end\n");
    test_mmio_reboot_reason();
    printf("ok  simulated MMIO reboot-reason write\n");
    test_mmio_gate_has_no_transfer();
    printf("ok  simulated MMIO gate refuses before transfer\n");
    printf("platform_spmi_test: PASS\n");
    return 0;
}
