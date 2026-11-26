/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* See Tb710fuSpmi.h for the register model and the read-only contract. */

#include "Tb710fuSpmi.h"

#include <stddef.h>

int tb710fu_spmi_layout(const struct tb710fu_spmi *spmi, uint32_t *apids)
{
    uint32_t version;
    uint32_t features;

    if (!spmi || !spmi->core || !spmi->obs || !spmi->cnfg || !apids ||
        !spmi->read32 || spmi->ee > TB710FU_SPMI_MAX_EE)
        return 4;
    version = spmi->read32(spmi->ctx, spmi->core, 0x00u);
    if (version < TB710FU_SPMI_VERSION_V7_MIN || version >= TB710FU_SPMI_VERSION_V8_MIN)
        return 1;
    features = spmi->read32(spmi->ctx, spmi->core, TB710FU_SPMI_FEATURES);
    *apids = features & TB710FU_SPMI_FEATURES_APIDS;
    if (*apids == 0u || *apids > TB710FU_SPMI_MAX_APIDS)
        return 2;
    return 0;
}

int tb710fu_spmi_find_apid(const struct tb710fu_spmi *spmi, uint32_t ppid, int *apid)
{
    uint32_t apids = 0u;
    uint32_t i;
    int result;

    if (!spmi || !apid) return 4;
    *apid = -1;
    if (ppid > 0xFFFu) return 4;
    result = tb710fu_spmi_layout(spmi, &apids);
    if (result != 0) return result;

    for (i = 0u; i < apids; ++i) {
        const uint32_t map = spmi->read32(spmi->ctx, spmi->core,
                                          TB710FU_SPMI_APID_MAP + 4u * i);
        if (map != 0u && ((map >> 8) & 0xFFFu) == ppid) {
            const uint32_t owner = spmi->read32(spmi->ctx, spmi->cnfg,
                                                TB710FU_SPMI_CNFG_OWNER + 4u * i) & 7u;

            if (*apid < 0 || owner == spmi->ee) *apid = (int)i;
        }
    }
    return *apid < 0 ? 3 : 0;
}

int tb710fu_spmi_read8(const struct tb710fu_spmi *spmi, int apid, uint8_t reg, uint8_t *value)
{
    uint32_t apids = 0u;
    uint32_t channel;
    unsigned poll;

    if (!spmi || !value || !spmi->read32 || !spmi->write32)
        return 4;
    if (tb710fu_spmi_layout(spmi, &apids) != 0) return 4;
    if (apid < 0 || (uint32_t)apid >= apids) return 4;

    channel = (TB710FU_SPMI_OBS_EE_STRIDE * (uint32_t)spmi->ee +
               TB710FU_SPMI_OBS_APID_STRIDE * (uint32_t)apid);
    spmi->write32(spmi->ctx, spmi->obs,
                  channel + TB710FU_SPMI_CMD,
                  ((uint32_t)TB710FU_SPMI_OP_EXT_READL << 27) | ((uint32_t)reg << 4));
    if (spmi->barrier) spmi->barrier(spmi->ctx);
    for (poll = 0u; poll < TB710FU_SPMI_POLLS; ++poll) {
        const uint32_t status = spmi->read32(spmi->ctx, spmi->obs,
                                             channel + TB710FU_SPMI_STATUS);

        if ((status & TB710FU_SPMI_STATUS_DONE) != 0u) {
            if ((status & TB710FU_SPMI_STATUS_ERRORS) != 0u) return 2;
            if (spmi->barrier) spmi->barrier(spmi->ctx);
            *value = (uint8_t)spmi->read32(spmi->ctx, spmi->obs,
                                           channel + TB710FU_SPMI_RDATA0);
            return 0;
        }
    }
    return 1;
}
