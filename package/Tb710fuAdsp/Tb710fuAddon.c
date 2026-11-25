/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* See Tb710fuAddon.h. No C library and no allocation: only reads. */
#include "Tb710fuAddon.h"

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static uint64_t pad8(uint64_t n)
{
    return (n + 7) & ~(uint64_t)7;
}

static int same(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i) d |= (uint8_t)(x[i] ^ y[i]);
    return d == 0;
}

int tb710fu_addon_find(const uint8_t *addon, uint64_t bytes, const char *path, Tb710fuAddonEntry *entry)
{
    if (!addon || !path || !entry || bytes < TB710FU_ADDON_HEADER || bytes > TB710FU_ADDON_MAX_BYTES ||
        !same(addon, TB710FU_ADDON_MAGIC, 8) || le32(addon + 12) != 0 || le64(addon + 16) != bytes)
        return TB710FU_ADDON_DAMAGED;
    const uint32_t count = le32(addon + 8);
    if (count > TB710FU_ADDON_MAX_ENTRIES) return TB710FU_ADDON_DAMAGED;
    int found = 0;
    uint64_t at = TB710FU_ADDON_HEADER;
    for (uint32_t i = 0; i < count; ++i) {
        if (bytes - at < TB710FU_ADDON_ENTRY) return TB710FU_ADDON_DAMAGED;
        const uint8_t *e = addon + at;
        const uint32_t name_bytes = le32(e);
        const uint64_t size = le64(e + 8), packed = le64(e + 16);
        if (name_bytes < 2 || name_bytes > TB710FU_ADDON_PATH_MAX || packed > bytes) return TB710FU_ADDON_DAMAGED;
        const uint64_t name_at = at + TB710FU_ADDON_ENTRY, data_at = name_at + pad8(name_bytes);
        if (data_at > bytes || pad8(packed) > bytes - data_at) return TB710FU_ADDON_DAMAGED;
        const char *name = (const char *)(addon + name_at);
        for (uint32_t k = 0; k + 1 < name_bytes; ++k) {
            if (!name[k]) return TB710FU_ADDON_DAMAGED;
        }
        if (name[name_bytes - 1]) return TB710FU_ADDON_DAMAGED;
        if (!found) {
            uint32_t k = 0;
            while (k + 1 < name_bytes && path[k] && path[k] == name[k]) ++k;
            if (k + 1 == name_bytes && !path[k]) {
                found = 1;
                entry->path = name;
                entry->mode = le32(e + 4);
                entry->size = size;
                entry->packed = addon + data_at;
                entry->packed_bytes = packed;
            }
        }
        at = data_at + pad8(packed);
    }
    if (at != bytes) return TB710FU_ADDON_DAMAGED;   /* the total covers the entries exactly */
    return found ? TB710FU_ADDON_OK : TB710FU_ADDON_NOT_FOUND;
}
