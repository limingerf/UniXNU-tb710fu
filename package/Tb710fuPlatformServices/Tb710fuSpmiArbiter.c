// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tb710fuSpmiArbiter.c - version-7 PMIC-arbiter core.
//
// The contract, the register facts and the write policy are documented in
// Tb710fuSpmiArbiter.h. Nothing here touches a real address: every window
// access goes through struct tb710fu_spmi_io, so this file is the same code
// that runs behind ml_io_map and on the host model in Test/.
#include "Tb710fuSpmiArbiter.h"

#include <stddef.h>

static int tb710fu_io_valid(const struct tb710fu_spmi_io *io)
{
    return io != NULL && io->read32 != NULL && io->write32 != NULL;
}

static uint32_t tb710fu_read(const struct tb710fu_spmi_io *io,
                             enum tb710fu_spmi_window window,
                             unsigned byte_offset)
{
    return io->read32(io->context, window, byte_offset);
}

static void tb710fu_write(const struct tb710fu_spmi_io *io,
                          enum tb710fu_spmi_window window,
                          unsigned byte_offset, uint32_t value)
{
    io->write32(io->context, window, byte_offset, value);
}

uint32_t tb710fu_spmi_command(unsigned operation, unsigned byte_addr,
                              unsigned bytes)
{
    /* bytes is 1..4; the command stores the count minus one in bits 2:0 and
     * the register byte in bits 11:4. No SID: the channel is already the
     * APID's, which is why mainline v7 uses pmic_arb_fmt_cmd_v2(). */
    return ((uint32_t)operation << 27) |
        (((uint32_t)byte_addr & 0xffu) << 4) | (((uint32_t)bytes - 1u) & 7u);
}

enum tb710fu_spmi_write_target tb710fu_spmi_write_target(unsigned usid,
                                                         uint8_t byte_addr,
                                                         unsigned bytes)
{
    if (usid == TB710FU_PMK8550_SDAM2_USID &&
        byte_addr == TB710FU_PMK8550_SDAM2_REBOOT_REASON && bytes == 1u)
        return TB710FU_SPMI_WRITE_TARGET_SDAM_REBOOT_REASON;
    if (usid == TB710FU_PMK8550_SDAM2_USID &&
        byte_addr == TB710FU_PMK8550_SDAM2_RTC_OFFSET &&
        bytes == 4u)
        return TB710FU_SPMI_WRITE_TARGET_SDAM_RTC_OFFSET;
    return TB710FU_SPMI_WRITE_TARGET_NONE;
}

enum tb710fu_spmi_write_denial tb710fu_spmi_write_denial(unsigned usid,
                                                         uint8_t byte_addr,
                                                         unsigned bytes)
{
    const unsigned sid = usid >> 8;
    const unsigned ppid = usid & 0xfffu;

    if (tb710fu_spmi_write_target(usid, byte_addr, bytes) !=
        TB710FU_SPMI_WRITE_TARGET_NONE)
        return TB710FU_SPMI_DENY_NONE;
    /* SID 7 is PM8550B, the board's charger/analog slave. Nothing in the
     * mainline pm8550b.dtsi names a charger register, so the whole slave is
     * refused by construction instead of trying to enumerate blocks. */
    if (sid == TB710FU_PM8550B_SID)
        return TB710FU_SPMI_DENY_CHARGER_SLAVE;
    if (sid != TB710FU_PMK8550_SID)
        return TB710FU_SPMI_DENY_FOREIGN_SLAVE;
    if (ppid == TB710FU_PMK8550_RTC_PPID ||
        ppid == TB710FU_PMK8550_RTC_ALARM_PPID)
        return TB710FU_SPMI_DENY_RTC_BLOCK;
    return TB710FU_SPMI_DENY_UNLISTED_CELL;
}

int tb710fu_spmi_probe(const struct tb710fu_spmi_io *io,
                       struct tb710fu_spmi_bus *bus)
{
    uint32_t version;
    uint32_t apids;

    if (!tb710fu_io_valid(io) || bus == NULL)
        return TB710FU_SPMI_ARGUMENT;
    version = tb710fu_read(io, TB710FU_SPMI_WINDOW_CORE,
                           TB710FU_SPMI_VERSION);
    /* Fails closed: a v8 base has different tables and another version has
     * none of these offsets. */
    if (version < TB710FU_SPMI_VERSION_V7_MIN ||
        version >= TB710FU_SPMI_VERSION_V8_MIN)
        return TB710FU_SPMI_NO_DEVICE;
    apids = tb710fu_read(io, TB710FU_SPMI_WINDOW_CORE,
                         TB710FU_SPMI_FEATURES) &
        TB710FU_SPMI_FEATURES_PERIPH_MASK;
    if (apids < 1u || apids > TB710FU_SPMI_MAX_APIDS)
        return TB710FU_SPMI_NO_DEVICE;
    bus->version = version;
    bus->base_apid = 0u; /* bus 0 starts at APID 0 (pmic_arb_init_apid_v7) */
    bus->apid_count = apids;
    return TB710FU_SPMI_OK;
}

int tb710fu_spmi_find(const struct tb710fu_spmi_io *io, unsigned ee,
                      unsigned usid, struct tb710fu_spmi_peripheral *out)
{
    struct tb710fu_spmi_bus bus;
    unsigned found = 0;
    unsigned owner = 0;
    int have = 0;
    int rc;

    if (!tb710fu_io_valid(io) || out == NULL || usid > 0xfffu ||
        ee > TB710FU_SPMI_MAX_EE)
        return TB710FU_SPMI_ARGUMENT;
    rc = tb710fu_spmi_probe(io, &bus);
    if (rc != TB710FU_SPMI_OK)
        return rc;
    for (unsigned i = 0; i < bus.apid_count; ++i) {
        const uint32_t map = tb710fu_read(io, TB710FU_SPMI_WINDOW_CORE,
                                          TB710FU_SPMI_APID_MAP_BASE +
                                              4u * i);
        unsigned candidate_owner;
        if (!map || ((map >> 8) & 0xfffu) != usid)
            continue;
        candidate_owner = tb710fu_read(io, TB710FU_SPMI_WINDOW_CONFIG,
                                       TB710FU_SPMI_APID_OWNER_BASE +
                                           4u * i) & 7u;
        if (!have || candidate_owner == ee) {
            found = i;
            owner = candidate_owner;
            have = 1;
        }
    }
    if (!have)
        return TB710FU_SPMI_NO_DEVICE;
    out->apid = found;
    out->owner = owner;
    return TB710FU_SPMI_OK;
}

int tb710fu_spmi_lookup_apid(const struct tb710fu_spmi_io *io, unsigned apid,
                             unsigned *usid)
{
    struct tb710fu_spmi_bus bus;
    uint32_t map;
    int rc;

    if (!tb710fu_io_valid(io) || usid == NULL)
        return TB710FU_SPMI_ARGUMENT;
    rc = tb710fu_spmi_probe(io, &bus);
    if (rc != TB710FU_SPMI_OK)
        return rc;
    if (apid >= bus.apid_count)
        return TB710FU_SPMI_BAD_APID;
    map = tb710fu_read(io, TB710FU_SPMI_WINDOW_CORE,
                       TB710FU_SPMI_APID_MAP_BASE + 4u * apid);
    if (!map)
        return TB710FU_SPMI_NO_DEVICE;
    *usid = (map >> 8) & 0xfffu;
    return TB710FU_SPMI_OK;
}

int tb710fu_spmi_ownership(const struct tb710fu_spmi_io *io, unsigned apid,
                           unsigned *owner)
{
    struct tb710fu_spmi_bus bus;
    int rc;

    if (!tb710fu_io_valid(io) || owner == NULL)
        return TB710FU_SPMI_ARGUMENT;
    rc = tb710fu_spmi_probe(io, &bus);
    if (rc != TB710FU_SPMI_OK)
        return rc;
    if (apid >= bus.apid_count)
        return TB710FU_SPMI_BAD_APID;
    *owner = tb710fu_read(io, TB710FU_SPMI_WINDOW_CONFIG,
                          TB710FU_SPMI_APID_OWNER_BASE + 4u * apid) & 7u;
    return TB710FU_SPMI_OK;
}

/* The arbiter takes the payload first and the command that starts the transfer
 * second, so an interrupted sequence never sends stale data. STATUS is re-armed
 * by the command, which is why every transfer polls it from scratch. */
static int tb710fu_spmi_transfer(const struct tb710fu_spmi_io *io,
                                 enum tb710fu_spmi_window window,
                                 unsigned base, uint32_t command,
                                 const uint32_t *write_data,
                                 uint32_t *read_data, unsigned *polls)
{
    unsigned poll;

    if (write_data != NULL)
        tb710fu_write(io, window, base + TB710FU_SPMI_WDATA0, *write_data);
    tb710fu_write(io, window, base + TB710FU_SPMI_CMD, command);
    for (poll = 0; poll < TB710FU_SPMI_POLL_LIMIT; ++poll) {
        const uint32_t status = tb710fu_read(io, window,
                                             base + TB710FU_SPMI_STATUS);
        if (status & TB710FU_SPMI_STATUS_DONE) {
            if (polls != NULL)
                *polls = poll + 1u;
            if (status & TB710FU_SPMI_STATUS_ERRORS) {
                /* Mainline folds these into -EIO; the platform log wants to
                 * know whether the arbiter denied the EE or the bus dropped
                 * the frame, so keep them apart. */
                if (status & TB710FU_SPMI_STATUS_DENIED)
                    return TB710FU_SPMI_DENIED;
                if (status & TB710FU_SPMI_STATUS_FAILURE)
                    return TB710FU_SPMI_FAILED;
                return TB710FU_SPMI_DROPPED;
            }
            if (read_data != NULL)
                *read_data = tb710fu_read(io, window,
                                          base + TB710FU_SPMI_RDATA0);
            return TB710FU_SPMI_OK;
        }
    }
    if (polls != NULL)
        *polls = poll;
    return TB710FU_SPMI_TIMEOUT;
}

static int tb710fu_spmi_arguments(const struct tb710fu_spmi_io *io,
                                  unsigned ee, unsigned apid, unsigned bytes)
{
    if (!tb710fu_io_valid(io))
        return TB710FU_SPMI_ARGUMENT;
    if (bytes < 1u || bytes > TB710FU_SPMI_MAX_BYTES)
        return TB710FU_SPMI_ARGUMENT;
    if (ee > TB710FU_SPMI_MAX_EE || apid > TB710FU_SPMI_MAX_APID)
        return TB710FU_SPMI_ARGUMENT;
    return TB710FU_SPMI_OK;
}

int tb710fu_spmi_read(const struct tb710fu_spmi_io *io, unsigned ee,
                      unsigned apid, uint8_t byte_addr, unsigned bytes,
                      uint32_t *value, unsigned *polls)
{
    struct tb710fu_spmi_bus bus;
    uint32_t data = 0;
    uint32_t mask;
    unsigned base;
    int rc;

    if (value == NULL)
        return TB710FU_SPMI_ARGUMENT;
    rc = tb710fu_spmi_arguments(io, ee, apid, bytes);
    if (rc != TB710FU_SPMI_OK)
        return rc;
    rc = tb710fu_spmi_probe(io, &bus);
    if (rc != TB710FU_SPMI_OK)
        return rc;
    if (apid >= bus.apid_count)
        return TB710FU_SPMI_BAD_APID;
    /* An observer channel exists for every EE; reading cannot change the
     * device, so ownership does not gate a read (mainline does the same). */
    base = TB710FU_SPMI_OBSERVER_EE_STRIDE * ee +
        TB710FU_SPMI_OBSERVER_APID_STRIDE * apid;
    rc = tb710fu_spmi_transfer(
        io, TB710FU_SPMI_WINDOW_OBSERVER, base,
        tb710fu_spmi_command(TB710FU_SPMI_OP_EXT_READL, byte_addr, bytes),
        NULL, &data, polls);
    if (rc != TB710FU_SPMI_OK)
        return rc;
    mask = bytes == TB710FU_SPMI_MAX_BYTES ? 0xffffffffu
                                           : (1u << (8u * bytes)) - 1u;
    *value = data & mask; /* RDATA0 is little endian, as mainline reads it */
    return TB710FU_SPMI_OK;
}

int tb710fu_spmi_write_apid(const struct tb710fu_spmi_io *io, unsigned ee,
                            unsigned apid, uint8_t byte_addr, unsigned bytes,
                            uint32_t value, unsigned *polls)
{
    struct tb710fu_spmi_bus bus;
    unsigned usid = 0;
    unsigned base;
    uint32_t mask;
    uint32_t data;
    int rc;

    rc = tb710fu_spmi_arguments(io, ee, apid, bytes);
    if (rc != TB710FU_SPMI_OK)
        return rc;
    rc = tb710fu_spmi_probe(io, &bus);
    if (rc != TB710FU_SPMI_OK)
        return rc;
    if (apid >= bus.apid_count)
        return TB710FU_SPMI_BAD_APID;
    /* Recover the USID from the mapping table rather than trusting the
     * caller, then apply the same whitelist the USID path applies. */
    rc = tb710fu_spmi_lookup_apid(io, apid, &usid);
    if (rc != TB710FU_SPMI_OK)
        return rc;
    if (tb710fu_spmi_write_target(usid, byte_addr, bytes) ==
        TB710FU_SPMI_WRITE_TARGET_NONE)
        return TB710FU_SPMI_READ_ONLY;
    if ((tb710fu_read(io, TB710FU_SPMI_WINDOW_CONFIG,
                      TB710FU_SPMI_APID_OWNER_BASE + 4u * apid) & 7u) != ee)
        return TB710FU_SPMI_NOT_OWNER;
    base = TB710FU_SPMI_CHANNEL_APID_STRIDE * apid;
    mask = bytes == TB710FU_SPMI_MAX_BYTES ? 0xffffffffu
                                           : (1u << (8u * bytes)) - 1u;
    data = value & mask;
    return tb710fu_spmi_transfer(
        io, TB710FU_SPMI_WINDOW_CHANNEL, base,
        tb710fu_spmi_command(TB710FU_SPMI_OP_EXT_WRITEL, byte_addr, bytes),
        &data, NULL, polls);
}

int tb710fu_spmi_write(const struct tb710fu_spmi_io *io, unsigned ee,
                       unsigned usid, uint8_t byte_addr, unsigned bytes,
                       uint32_t value, unsigned *polls)
{
    struct tb710fu_spmi_peripheral peripheral;
    int rc;

    rc = tb710fu_spmi_arguments(io, ee, 0u, bytes);
    if (rc != TB710FU_SPMI_OK)
        return rc;
    /* Resolve first so an unmapped or foreign USID never reaches a channel. */
    rc = tb710fu_spmi_find(io, ee, usid, &peripheral);
    if (rc != TB710FU_SPMI_OK)
        return rc;
    return tb710fu_spmi_write_apid(io, ee, peripheral.apid, byte_addr, bytes,
                                   value, polls);
}

const char *tb710fu_spmi_result_name(int result)
{
    switch (result) {
    case TB710FU_SPMI_OK: return "ok";
    case TB710FU_SPMI_FAILED: return "failed";
    case TB710FU_SPMI_DENIED: return "denied";
    case TB710FU_SPMI_DROPPED: return "dropped";
    case TB710FU_SPMI_TIMEOUT: return "timeout";
    case TB710FU_SPMI_NO_DEVICE: return "no-device";
    case TB710FU_SPMI_BAD_APID: return "bad-apid";
    case TB710FU_SPMI_NOT_OWNER: return "not-owner";
    case TB710FU_SPMI_READ_ONLY: return "read-only";
    case TB710FU_SPMI_ARGUMENT: return "argument";
    }
    return "unknown";
}

const char *tb710fu_spmi_denial_name(int denial)
{
    switch (denial) {
    case TB710FU_SPMI_DENY_NONE: return "allowed";
    case TB710FU_SPMI_DENY_CHARGER_SLAVE: return "charger-slave";
    case TB710FU_SPMI_DENY_FOREIGN_SLAVE: return "foreign-slave";
    case TB710FU_SPMI_DENY_RTC_BLOCK: return "rtc-block";
    case TB710FU_SPMI_DENY_UNLISTED_CELL: return "unlisted-cell";
    }
    return "unknown";
}
