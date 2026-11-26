// SPDX-License-Identifier: BSD-2-Clause-Patent
// host_spmi_model.c - the PMK8550/arbiter register model behind the host
// tests. Every function here is test-only.
#include "host_spmi_model.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint32_t host_spmi_mask(unsigned bytes)
{
    return bytes >= 4u ? 0xffffffffu : (1u << (8u * bytes)) - 1u;
}

static uint32_t host_spmi_word(const uint32_t *words, unsigned bytes,
                               unsigned offset)
{
    assert((offset & 3u) == 0u);
    assert(offset < bytes);
    return words[offset / 4u];
}

static unsigned host_spmi_counter_index(unsigned reads, unsigned count)
{
    return reads < count ? reads : count - 1u;
}

static unsigned host_spmi_usid(const struct host_spmi_model *model,
                               unsigned apid)
{
    const uint32_t map =
        model->core[(TB710FU_SPMI_APID_MAP_BASE + 4u * apid) / 4u];

    return map != 0u ? (unsigned)((map >> 8) & 0xfffu) : 0u;
}

static uint32_t host_spmi_pmic_read(struct host_spmi_model *model,
                                    unsigned apid, unsigned address,
                                    unsigned bytes)
{
    const unsigned usid = host_spmi_usid(model, apid);
    uint32_t value = 0;

    ++model->pmic_reads;
    if (usid == TB710FU_PMK8550_RTC_USID) {
        if (address == TB710FU_PMK8550_RTC_CONTROL && bytes == 1u) {
            value = model->control;
        } else if (address == TB710FU_PMK8550_RTC_COUNTER) {
            if (bytes == TB710FU_PMK8550_RTC_COUNTER_BYTES) {
                if (model->counter_count != 0u) {
                    const unsigned index = host_spmi_counter_index(
                        model->counter_reads, model->counter_count);
                    value = model->counter_values[index];
                    model->last_counter = value;
                }
                ++model->counter_reads;
            } else if (bytes == 1u) {
                if (model->counter_lsb_count != 0u) {
                    const unsigned index = host_spmi_counter_index(
                        model->counter_lsb_reads,
                        model->counter_lsb_count);
                    value = model->counter_lsb_values[index];
                } else {
                    value = model->last_counter & 0xffu;
                }
                ++model->counter_lsb_reads;
            } else {
                value = model->last_counter;
            }
        }
    } else if (usid == TB710FU_PMK8550_SDAM2_USID) {
        if (address == TB710FU_PMK8550_SDAM2_RTC_OFFSET &&
            bytes == HOST_SPMI_WORD_BYTES) {
            if (model->cell_count != 0u) {
                const unsigned index = host_spmi_counter_index(
                    model->cell_reads, model->cell_count);
                value = model->cell_values[index];
            }
            ++model->cell_reads;
        } else if (address == TB710FU_PMK8550_SDAM2_REBOOT_REASON &&
                   bytes == 1u) {
            if (model->reason_count != 0u) {
                const unsigned index = host_spmi_counter_index(
                    model->reason_reads, model->reason_count);
                value = model->reason_values[index];
            }
            ++model->reason_reads;
        } else if (address == TB710FU_PMK8550_SDAM2_RTC_OFFSET &&
                   bytes < HOST_SPMI_WORD_BYTES) {
            value = model->cell_count != 0u ? model->cell_values[0] : 0u;
        }
    }
    return value & host_spmi_mask(bytes);
}

static void host_spmi_pmic_write(struct host_spmi_model *model, unsigned apid,
                                 unsigned address, unsigned bytes,
                                 uint32_t value)
{
    const unsigned usid = host_spmi_usid(model, apid);
    const uint32_t masked = value & host_spmi_mask(bytes);

    ++model->pmic_writes;
    if (usid == TB710FU_PMK8550_SDAM2_USID &&
        address == TB710FU_PMK8550_SDAM2_RTC_OFFSET &&
        bytes == HOST_SPMI_WORD_BYTES) {
        ++model->cell_writes;
        if (!model->drop_writes) {
            model->cell_values[0] = masked;
            model->cell_count = 1u;
            model->cell_reads = 0u;
        }
        return;
    }
    if (usid == TB710FU_PMK8550_SDAM2_USID &&
        address == TB710FU_PMK8550_SDAM2_REBOOT_REASON && bytes == 1u) {
        ++model->reason_writes;
        if (!model->drop_writes) {
            model->reason_values[0] = masked;
            model->reason_count = 1u;
            model->reason_reads = 0u;
        }
        return;
    }
    ++model->unexpected_writes;
}

static uint32_t host_spmi_error_status(enum host_spmi_error error)
{
    switch (error) {
    case HOST_SPMI_ERROR_FAILURE:
        return TB710FU_SPMI_STATUS_DONE | TB710FU_SPMI_STATUS_FAILURE;
    case HOST_SPMI_ERROR_DENIED:
        return TB710FU_SPMI_STATUS_DONE | TB710FU_SPMI_STATUS_DENIED;
    case HOST_SPMI_ERROR_DROPPED:
        return TB710FU_SPMI_STATUS_DONE | TB710FU_SPMI_STATUS_DROPPED;
    case HOST_SPMI_ERROR_SILENT:
    case HOST_SPMI_ERROR_NONE:
        break;
    }
    return 0u;
}

void host_spmi_observer_command(struct host_spmi_model *model,
                                unsigned offset, uint32_t command)
{
    const unsigned ee = offset / TB710FU_SPMI_OBSERVER_EE_STRIDE;
    const unsigned apid =
        (offset % TB710FU_SPMI_OBSERVER_EE_STRIDE) /
        TB710FU_SPMI_OBSERVER_APID_STRIDE;
    const unsigned base = TB710FU_SPMI_OBSERVER_EE_STRIDE * ee +
        TB710FU_SPMI_OBSERVER_APID_STRIDE * apid;
    const unsigned operation = (command >> 27) & 0x1fu;
    const unsigned address = (command >> 4) & 0xffu;
    const unsigned bytes = (command & 7u) + 1u;

    assert(ee < HOST_SPMI_EES);
    assert(apid < HOST_SPMI_APIDS);
    assert((offset % TB710FU_SPMI_OBSERVER_APID_STRIDE) == TB710FU_SPMI_CMD);
    ++model->commands;
    ++model->read_commands;
    model->observer[(base + TB710FU_SPMI_STATUS) / 4u] = 0u;
    if (operation != TB710FU_SPMI_OP_EXT_READL ||
        bytes < 1u || bytes > TB710FU_SPMI_MAX_BYTES) {        ++model->unexpected_commands;
        return;
    }
    if (model->error == HOST_SPMI_ERROR_SILENT) {
        return;
    }
    if (model->error != HOST_SPMI_ERROR_NONE) {
        model->observer[(base + TB710FU_SPMI_STATUS) / 4u] =
            host_spmi_error_status(model->error);
        return;
    }
    model->observer[(base + TB710FU_SPMI_RDATA0) / 4u] =
        host_spmi_pmic_read(model, apid, address, bytes);
    model->observer[(base + TB710FU_SPMI_STATUS) / 4u] =
        TB710FU_SPMI_STATUS_DONE;
}

void host_spmi_channel_command(struct host_spmi_model *model, unsigned offset,
                               uint32_t command)
{
    const unsigned apid = offset / TB710FU_SPMI_CHANNEL_APID_STRIDE;
    const unsigned base = TB710FU_SPMI_CHANNEL_APID_STRIDE * apid;
    const unsigned operation = (command >> 27) & 0x1fu;
    const unsigned address = (command >> 4) & 0xffu;
    const unsigned bytes = (command & 7u) + 1u;
    uint32_t value;

    assert(apid < HOST_SPMI_APIDS);
    assert((offset % TB710FU_SPMI_CHANNEL_APID_STRIDE) == TB710FU_SPMI_CMD);
    ++model->commands;
    ++model->write_commands;
    model->channel[(base + TB710FU_SPMI_STATUS) / 4u] = 0u;
    value = model->channel[(base + TB710FU_SPMI_WDATA0) / 4u];
    if (operation != TB710FU_SPMI_OP_EXT_WRITEL ||
        bytes < 1u || bytes > TB710FU_SPMI_MAX_BYTES) {
        ++model->unexpected_commands;
        return;
    }
    if (model->error == HOST_SPMI_ERROR_SILENT) {
        return;
    }
    if (model->error != HOST_SPMI_ERROR_NONE) {
        model->channel[(base + TB710FU_SPMI_STATUS) / 4u] =
            host_spmi_error_status(model->error);
        return;
    }
    host_spmi_pmic_write(model, apid, address, bytes, value);
    model->channel[(base + TB710FU_SPMI_STATUS) / 4u] =
        TB710FU_SPMI_STATUS_DONE;
}

static uint32_t host_spmi_read32(void *context, enum tb710fu_spmi_window window,
                                 unsigned offset)
{
    struct host_spmi_model *model = context;

    switch (window) {
    case TB710FU_SPMI_WINDOW_CORE:
        return host_spmi_word(model->core, (unsigned)sizeof(model->core),
                              offset);
    case TB710FU_SPMI_WINDOW_CONFIG:
        return host_spmi_word(model->config, (unsigned)sizeof(model->config),
                              offset);
    case TB710FU_SPMI_WINDOW_OBSERVER:
        return host_spmi_word(model->observer,
                              (unsigned)sizeof(model->observer), offset);
    case TB710FU_SPMI_WINDOW_CHANNEL:
        return host_spmi_word(model->channel, (unsigned)sizeof(model->channel),
                              offset);
    }
    assert(0);
    return 0u;
}

static void host_spmi_write32(void *context, enum tb710fu_spmi_window window,
                              unsigned offset, uint32_t value)
{
    struct host_spmi_model *model = context;

    switch (window) {
    case TB710FU_SPMI_WINDOW_CORE:
    case TB710FU_SPMI_WINDOW_CONFIG:
        ++model->unexpected_writes;
        break;
    case TB710FU_SPMI_WINDOW_OBSERVER: {
        const unsigned reg = offset % TB710FU_SPMI_OBSERVER_APID_STRIDE;

        assert(offset < (unsigned)sizeof(model->observer));
        if (reg == TB710FU_SPMI_CMD)
            host_spmi_observer_command(model, offset, value);
        else
            ++model->unexpected_writes;
        break;
    }
    case TB710FU_SPMI_WINDOW_CHANNEL: {
        const unsigned reg = offset % TB710FU_SPMI_CHANNEL_APID_STRIDE;
        const unsigned base = offset - reg;

        assert(offset < (unsigned)sizeof(model->channel));
        if (reg == TB710FU_SPMI_CMD) {
            host_spmi_channel_command(model, offset, value);
        } else if (reg == TB710FU_SPMI_WDATA0) {
            model->channel[(base + reg) / 4u] = value;
        } else {
            ++model->unexpected_writes;
        }
        break;
    }
    }
}

struct tb710fu_spmi_io host_spmi_io(struct host_spmi_model *model)
{
    struct tb710fu_spmi_io io;

    io.context = model;
    io.read32 = host_spmi_read32;
    io.write32 = host_spmi_write32;
    return io;
}

void host_spmi_init(struct host_spmi_model *model)
{
    memset(model, 0, sizeof(*model));
    model->apid_count = HOST_SPMI_APIDS;
    model->core[TB710FU_SPMI_VERSION / 4u] = TB710FU_SPMI_VERSION_V7_MIN;
    model->core[TB710FU_SPMI_FEATURES / 4u] = HOST_SPMI_APIDS;
    model->core[(TB710FU_SPMI_APID_MAP_BASE + 4u * 2u) / 4u] =
        (uint32_t)TB710FU_PMK8550_RTC_USID << 8;
    model->core[(TB710FU_SPMI_APID_MAP_BASE + 4u * 3u) / 4u] =
        (uint32_t)TB710FU_PMK8550_SDAM2_USID << 8;
    model->control = TB710FU_PMK8550_RTC_RUNNING;
    model->counter_values[0] = 0x65000000u; /* 2023-09-12, above EARLIEST */
    model->counter_count = 1u;
    model->cell_values[0] = 0u;
    model->cell_count = 1u;
    model->reason_values[0] = 0u;
    model->reason_count = 1u;
    model->error = HOST_SPMI_ERROR_NONE;
}

void host_spmi_map(struct host_spmi_model *model, unsigned apid, unsigned usid)
{
    assert(apid < HOST_SPMI_APIDS);
    model->core[(TB710FU_SPMI_APID_MAP_BASE + 4u * apid) / 4u] = usid << 8;
}

void host_spmi_unmap(struct host_spmi_model *model, unsigned apid)
{
    assert(apid < HOST_SPMI_APIDS);
    model->core[(TB710FU_SPMI_APID_MAP_BASE + 4u * apid) / 4u] = 0u;
}

void host_spmi_set_owner(struct host_spmi_model *model, unsigned apid,
                         unsigned owner)
{
    assert(apid < HOST_SPMI_APIDS);
    model->config[apid] = owner & 7u;
}

void host_spmi_set_error(struct host_spmi_model *model,
                         enum host_spmi_error error)
{
    model->error = error;
}

void host_spmi_set_drop_writes(struct host_spmi_model *model, int drop)
{
    model->drop_writes = drop != 0;
}

void host_spmi_set_control(struct host_spmi_model *model, uint32_t control)
{
    model->control = control;
}

void host_spmi_set_counter(struct host_spmi_model *model, uint32_t counter)
{
    model->counter_values[0] = counter;
    model->counter_count = 1u;
    model->counter_reads = 0u;
    model->last_counter = counter;
}

void host_spmi_set_counter_sequence(struct host_spmi_model *model,
                                    const uint32_t *values, unsigned count)
{
    assert(count <= HOST_SPMI_COUNTER_VALUES);
    memcpy(model->counter_values, values, sizeof(values[0]) * count);
    model->counter_count = count;
    model->counter_reads = 0u;
}

void host_spmi_set_counter_lsb_sequence(struct host_spmi_model *model,
                                        const uint32_t *values,
                                        unsigned count)
{
    assert(count <= HOST_SPMI_COUNTER_VALUES);
    memcpy(model->counter_lsb_values, values, sizeof(values[0]) * count);
    model->counter_lsb_count = count;
    model->counter_lsb_reads = 0u;
}

void host_spmi_set_cell(struct host_spmi_model *model, uint32_t cell)
{
    model->cell_values[0] = cell;
    model->cell_count = 1u;
    model->cell_reads = 0u;
}

void host_spmi_set_cell_sequence(struct host_spmi_model *model,
                                 const uint32_t *values, unsigned count)
{
    assert(count <= HOST_SPMI_CELL_VALUES);
    memcpy(model->cell_values, values, sizeof(values[0]) * count);
    model->cell_count = count;
    model->cell_reads = 0u;
}

void host_spmi_set_reason(struct host_spmi_model *model, uint8_t reason)
{
    model->reason_values[0] = reason;
    model->reason_count = 1u;
    model->reason_reads = 0u;
}

void host_spmi_set_reason_sequence(struct host_spmi_model *model,
                                   const uint32_t *values, unsigned count)
{
    assert(count <= HOST_SPMI_REASON_VALUES);
    memcpy(model->reason_values, values, sizeof(values[0]) * count);
    model->reason_count = count;
    model->reason_reads = 0u;
}

/* ---- simulated MMIO transport ----------------------------------------- */

static uint32_t host_mmio_bridge_read32(void *context,
                                        enum tb710fu_spmi_window window,
                                        unsigned offset)
{
    struct host_mmio_bridge *bridge = context;

    return bridge->mmio.io.read32(bridge->mmio.io.context, window, offset);
}

static void host_mmio_bridge_write32(void *context,
                                     enum tb710fu_spmi_window window,
                                     unsigned offset, uint32_t value)
{
    struct host_mmio_bridge *bridge = context;
    const unsigned faults = bridge->mmio.faults;

    bridge->mmio.io.write32(bridge->mmio.io.context, window, offset, value);
    if (bridge->mmio.faults != faults) {
        return; /* the transport refused the access; no device saw it */
    }
    if (window == TB710FU_SPMI_WINDOW_OBSERVER) {
        const unsigned reg = offset % TB710FU_SPMI_OBSERVER_APID_STRIDE;

        if (reg == TB710FU_SPMI_CMD)
            host_spmi_observer_command(bridge->pmic, offset, value);
    } else if (window == TB710FU_SPMI_WINDOW_CHANNEL) {
        const unsigned reg = offset % TB710FU_SPMI_CHANNEL_APID_STRIDE;

        if (reg == TB710FU_SPMI_CMD)
            host_spmi_channel_command(bridge->pmic, offset, value);
    }
}

int host_mmio_bridge_init(struct host_mmio_bridge *bridge,
                          struct host_spmi_model *model)
{
    int result;

    memset(bridge, 0, sizeof(*bridge));
    bridge->pmic = model;
    result = tb710fu_spmi_mmio_init(
        &bridge->mmio, model->core, TB710FU_SPMI_CORE_BYTES, model->config,
        TB710FU_SPMI_CONFIG_BYTES, model->observer, TB710FU_SPMI_OBSERVER_BYTES,
        model->channel, TB710FU_SPMI_CHANNEL_BYTES, TB710FU_SPMI_EE);
    if (result != TB710FU_SPMI_OK) {
        return result;
    }
    bridge->bridge_io.context = bridge;
    bridge->bridge_io.read32 = host_mmio_bridge_read32;
    bridge->bridge_io.write32 = host_mmio_bridge_write32;
    return TB710FU_SPMI_OK;
}
