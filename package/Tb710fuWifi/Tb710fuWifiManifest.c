/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Tb710fuWifiManifest.h"

static int same(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}


int tb710fu_wifi_fw_check(const char *name, const uint8_t *bytes, uint64_t size, struct tb710fu_wifi_fw_info *info)
{
    if (!name || !bytes || !info) return TB710FU_WIFI_FW_BAD_MAGIC;
    for (unsigned i = 0; i < sizeof *info; ++i) ((uint8_t *)info)[i] = 0;
    info->bytes = size;
    if (same(name, TB710FU_WIFI_FW_AMSS_NAME)) {
        info->bits = TB710FU_WIFI_FW_BIT_AMSS;
        if (size < (1u << 20)) return TB710FU_WIFI_FW_TOO_SMALL;
        if (size > TB710FU_WIFI_FW_AMSS_MAX) return TB710FU_WIFI_FW_TOO_LARGE;
        if (size < 20 || bytes[0] != 0x7fu || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F')
            return TB710FU_WIFI_FW_BAD_MAGIC;
        info->elf_class = bytes[4];
        info->elf_data = bytes[5];
        info->elf_machine = (uint32_t)bytes[18] | ((uint32_t)bytes[19] << 8);
        if (bytes[4] != 1u || bytes[5] != 1u || info->elf_machine != 0x28u) return TB710FU_WIFI_FW_BAD_MAGIC;
        return TB710FU_WIFI_FW_OK;
    }
    if (same(name, TB710FU_WIFI_FW_M3_NAME)) {
        info->bits = TB710FU_WIFI_FW_BIT_M3;
        if (size < (64u << 10)) return TB710FU_WIFI_FW_TOO_SMALL;
        if (size > TB710FU_WIFI_FW_M3_MAX) return TB710FU_WIFI_FW_TOO_LARGE;
        if (size < 20 || bytes[0] != 0x7fu || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F')
            return TB710FU_WIFI_FW_BAD_MAGIC;
        info->elf_class = bytes[4];
        info->elf_data = bytes[5];
        info->elf_machine = (uint32_t)bytes[18] | ((uint32_t)bytes[19] << 8);
        if (bytes[4] != 1u || bytes[5] != 1u || info->elf_machine != 0x28u) return TB710FU_WIFI_FW_BAD_MAGIC;
        return TB710FU_WIFI_FW_OK;
    }
    if (same(name, TB710FU_WIFI_FW_BOARD_NAME)) {
        info->bits = TB710FU_WIFI_FW_BIT_BOARD;
        if (size < (64u << 10)) return TB710FU_WIFI_FW_TOO_SMALL;
        if (size > TB710FU_WIFI_FW_BOARD_MAX) return TB710FU_WIFI_FW_TOO_LARGE;
        static const char magic[] = "QCA-ATH12K-BOARD";
        if (size < sizeof magic || bytes[sizeof magic - 1] != 0) return TB710FU_WIFI_FW_BAD_MAGIC;
        for (unsigned i = 0; i < sizeof magic - 1; ++i) {
            if (bytes[i] != (uint8_t)magic[i]) return TB710FU_WIFI_FW_BAD_MAGIC;
        }
        return TB710FU_WIFI_FW_OK;
    }
    (void)name;
    return TB710FU_WIFI_FW_UNKNOWN_NAME;
}

int tb710fu_wifi_manifest_note(struct tb710fu_wifi_manifest *manifest, const struct tb710fu_wifi_fw_info *info)
{
    if (!manifest || !info || !info->bits) return 0;
    manifest->present |= info->bits;
    if (info->bits == TB710FU_WIFI_FW_BIT_AMSS) manifest->amss_bytes = info->bytes;
    if (info->bits == TB710FU_WIFI_FW_BIT_M3) manifest->m3_bytes = info->bytes;
    if (info->bits == TB710FU_WIFI_FW_BIT_BOARD) manifest->board_bytes = info->bytes;
    manifest->total_bytes = manifest->amss_bytes + manifest->m3_bytes + manifest->board_bytes;
    return manifest->total_bytes <= (TB710FU_WIFI_FW_AMSS_MAX + TB710FU_WIFI_FW_M3_MAX + TB710FU_WIFI_FW_BOARD_MAX);
}

int tb710fu_wifi_manifest_complete(const struct tb710fu_wifi_manifest *manifest)
{
    return manifest && manifest->present == TB710FU_WIFI_FW_BIT_ALL;
}
