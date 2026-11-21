/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Adapted from UniXNU-sheng@516a780473a2ac59c8b438e132bb60204dda9e22. */
#ifndef TB710FU_CDC_QCOM_RPMH_H
#define TB710FU_CDC_QCOM_RPMH_H

#include <stddef.h>
#include <stdint.h>

#define TB710FU_CDC_CMD_DB_MAGIC UINT32_C(0x0c0330db)
#define TB710FU_CDC_RPMH_MAX_LEVELS 16u

enum tb710fu_cdc_rpmh_result {
    TB710FU_CDC_RPMH_OK = 0,
    TB710FU_CDC_RPMH_NO_DB = -1,
    TB710FU_CDC_RPMH_MISSING = -2,
    TB710FU_CDC_RPMH_CORRUPT = -3,
    TB710FU_CDC_RPMH_NO_LEVEL = -4,
    TB710FU_CDC_RPMH_ARGUMENT = -5
};

struct tb710fu_cdc_rpmh_resource {
    uint32_t address;
    uint16_t levels[TB710FU_CDC_RPMH_MAX_LEVELS];
    unsigned level_count;
};

struct tb710fu_cdc_rpmh_write {
    uint32_t offset, value;
};

struct tb710fu_cdc_rsc_layout {
    uint32_t tcs_stride, cmd_stride;
    uint32_t irq_enable, irq_status, irq_clear, wait_for_cmpl, control, status;
    uint32_t cmd_enable, cmd_msgid, cmd_addr, cmd_data;
};

static inline uint16_t tb710fu_cdc_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static inline uint32_t tb710fu_cdc_le32(const uint8_t *p)
{
    return (uint32_t)tb710fu_cdc_le16(p) |
           (uint32_t)tb710fu_cdc_le16(p + 2) << 16;
}

static inline int tb710fu_cdc_range_fits(size_t offset, size_t length, size_t size)
{
    return offset <= size && length <= size - offset;
}

static inline int tb710fu_cdc_cmd_db_find(const uint8_t *db, size_t bytes,
                                          const char *name,
                                          struct tb710fu_cdc_rpmh_resource *out)
{
    if (!db || !name || !out) return TB710FU_CDC_RPMH_ARGUMENT;
    if (bytes < 144u || tb710fu_cdc_le32(db + 4) != TB710FU_CDC_CMD_DB_MAGIC)
        return TB710FU_CDC_RPMH_NO_DB;

    char id[8] = {0};
    unsigned name_bytes = 0;
    while (name_bytes < sizeof(id) && name[name_bytes]) {
        id[name_bytes] = name[name_bytes];
        ++name_bytes;
    }
    if (name[name_bytes]) return TB710FU_CDC_RPMH_ARGUMENT;

    for (unsigned slave_index = 0; slave_index < 8; ++slave_index) {
        const uint8_t *slave = db + 8u + 16u * slave_index;
        if (!tb710fu_cdc_le16(slave)) break;

        const size_t headers = 144u + tb710fu_cdc_le16(slave + 2);
        const size_t data = 144u + tb710fu_cdc_le16(slave + 4);
        const size_t count = tb710fu_cdc_le16(slave + 6);
        if (!tb710fu_cdc_range_fits(headers, count * 24u, bytes))
            return TB710FU_CDC_RPMH_CORRUPT;

        for (size_t entry_index = 0; entry_index < count; ++entry_index) {
            const uint8_t *entry = db + headers + 24u * entry_index;
            unsigned same = 1;
            for (unsigned byte = 0; byte < sizeof(id) && same; ++byte)
                same = entry[byte] == (uint8_t)id[byte];
            if (!same) continue;

            struct tb710fu_cdc_rpmh_resource resource = {0};
            resource.address = tb710fu_cdc_le32(entry + 16);
            const size_t auxiliary_bytes = tb710fu_cdc_le16(entry + 20);
            const size_t auxiliary = data + tb710fu_cdc_le16(entry + 22);
            if (!resource.address ||
                !tb710fu_cdc_range_fits(auxiliary, auxiliary_bytes, bytes))
                return TB710FU_CDC_RPMH_CORRUPT;

            for (size_t offset = 0; offset + 1u < auxiliary_bytes &&
                 resource.level_count < TB710FU_CDC_RPMH_MAX_LEVELS; offset += 2u) {
                const uint16_t level = tb710fu_cdc_le16(db + auxiliary + offset);
                if (resource.level_count && !level) break;
                resource.levels[resource.level_count++] = level;
            }
            *out = resource;
            return TB710FU_CDC_RPMH_OK;
        }
    }
    return TB710FU_CDC_RPMH_MISSING;
}

static inline int tb710fu_cdc_rpmh_level_index(
    const struct tb710fu_cdc_rpmh_resource *resource,
    uint32_t level, uint32_t *index)
{
    if (!resource || !index) return TB710FU_CDC_RPMH_ARGUMENT;
    if (!resource->level_count) return TB710FU_CDC_RPMH_NO_LEVEL;
    if (!level) {
        *index = 0;
        return TB710FU_CDC_RPMH_OK;
    }
    for (unsigned i = 0; i < resource->level_count; ++i) {
        if (resource->levels[i] >= level) {
            *index = i;
            return TB710FU_CDC_RPMH_OK;
        }
    }
    *index = resource->level_count - 1u;
    return TB710FU_CDC_RPMH_OK;
}

static inline struct tb710fu_cdc_rsc_layout tb710fu_cdc_rsc_layout(uint32_t driver_id)
{
    if (((driver_id >> 16) & 0xffu) >= 3u) {
        return (struct tb710fu_cdc_rsc_layout){
            672, 24, 0x00, 0x04, 0x08, 0x20, 0x24, 0x28, 0x2c, 0x34, 0x38, 0x3c
        };
    }
    return (struct tb710fu_cdc_rsc_layout){
        672, 20, 0x00, 0x04, 0x08, 0x10, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38
    };
}

static inline unsigned tb710fu_cdc_rpmh_active_request(
    const struct tb710fu_cdc_rsc_layout *layout, unsigned tcs,
    const uint32_t *addresses, const uint32_t *values, unsigned count,
    struct tb710fu_cdc_rpmh_write *writes, unsigned capacity)
{
    if (!layout || !layout->tcs_stride || !addresses || !values || !writes ||
        !count || count > 4u || tcs >= 32u ||
        capacity < 3u * count + 6u ||
        tcs > UINT32_MAX / layout->tcs_stride) return 0;

    const uint32_t base = layout->tcs_stride * tcs;
    unsigned n = 0;
    writes[n++] = (struct tb710fu_cdc_rpmh_write){base + layout->control, 0};
    writes[n++] = (struct tb710fu_cdc_rpmh_write){layout->irq_clear, 1u << tcs};
    for (unsigned command_index = 0; command_index < count; ++command_index) {
        const uint32_t command = base + layout->cmd_stride * command_index;
        writes[n++] = (struct tb710fu_cdc_rpmh_write){
            command + layout->cmd_msgid, 8u | (1u << 8) | (1u << 16)
        };
        writes[n++] = (struct tb710fu_cdc_rpmh_write){
            command + layout->cmd_addr, addresses[command_index]
        };
        writes[n++] = (struct tb710fu_cdc_rpmh_write){
            command + layout->cmd_data, values[command_index]
        };
    }
    const uint32_t mask = (1u << count) - 1u;
    writes[n++] = (struct tb710fu_cdc_rpmh_write){base + layout->wait_for_cmpl, mask};
    writes[n++] = (struct tb710fu_cdc_rpmh_write){base + layout->cmd_enable, mask};
    writes[n++] = (struct tb710fu_cdc_rpmh_write){base + layout->control, 1u << 16};
    writes[n++] = (struct tb710fu_cdc_rpmh_write){
        base + layout->control, (1u << 16) | (1u << 24)
    };
    return n;
}

static inline unsigned tb710fu_cdc_rpmh_finish(
    const struct tb710fu_cdc_rsc_layout *layout, unsigned tcs,
    struct tb710fu_cdc_rpmh_write *writes, unsigned capacity)
{
    if (!layout || !layout->tcs_stride || !writes || capacity < 3u || tcs >= 32u ||
        tcs > UINT32_MAX / layout->tcs_stride) return 0;
    const uint32_t base = layout->tcs_stride * tcs;
    writes[0] = (struct tb710fu_cdc_rpmh_write){layout->irq_clear, 1u << tcs};
    writes[1] = (struct tb710fu_cdc_rpmh_write){base + layout->control, 0};
    writes[2] = (struct tb710fu_cdc_rpmh_write){base + layout->cmd_enable, 0};
    return 3;
}

#endif
