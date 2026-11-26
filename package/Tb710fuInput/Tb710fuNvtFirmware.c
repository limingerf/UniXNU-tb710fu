/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* See Tb710fuNvtFirmware.h for the header words and the contract. */

#include "Tb710fuNvtFirmware.h"
#include "Tb710fuNvt.h"

#include <string.h>

static uint32_t read_le32(const uint8_t *image, size_t offset)
{
    return (uint32_t)image[offset] |
           ((uint32_t)image[offset + 1u] << 8) |
           ((uint32_t)image[offset + 2u] << 16) |
           ((uint32_t)image[offset + 3u] << 24);
}

uint32_t tb710fu_nvt_firmware_crc32(const uint8_t *data, size_t bytes)
{
    uint32_t checksum = 0xFFFFFFFFu;
    size_t words;
    size_t i;

    if (!data || bytes > SIZE_MAX - 4u) return 0u;
    words = (bytes + 4u) / 4u;
    for (i = 0u; i < words; ++i) {
        uint32_t word = 0u;
        unsigned byte;

        for (byte = 0u; byte < 4u; ++byte) {
            const size_t at = i * 4u + byte;

            if (at < bytes) word |= (uint32_t)data[at] << (8u * byte);
        }
        checksum ^= word;
        for (byte = 0u; byte < 32u; ++byte)
            checksum = (checksum & 0x80000000u) ? (checksum << 1u) ^ TB710FU_NVT_FW_CRC_POLY
                                                : (checksum << 1u);
    }
    return checksum;
}

int tb710fu_nvt_firmware_parse(const uint8_t *image, size_t bytes,
                               struct tb710fu_nvt_firmware *firmware)
{
    uint32_t words[TB710FU_NVT_FW_HEADER_BYTES / 4u];
    unsigned i;

    if (!image || !firmware) return TB710FU_NVT_ERR_PARAM;
    if (bytes < TB710FU_NVT_FW_MIN_BYTES || bytes > TB710FU_NVT_FW_MAX_BYTES ||
        (bytes & 3u) != 0u)
        return TB710FU_NVT_ERR_FW;

    memset(firmware, 0, sizeof(*firmware));
    for (i = 0u; i < TB710FU_NVT_FW_HEADER_BYTES / 4u; ++i)
        words[i] = read_le32(image, i * 4u);

    /* The ILM offset must be one of the vendor's page-aligned addresses, and
     * both described regions must lie inside the file. */
    if (words[0] < TB710FU_NVT_FW_ILM_MIN || words[0] > TB710FU_NVT_FW_ILM_MAX ||
        (words[0] & 0xFFu) != 0u)
        return TB710FU_NVT_ERR_FW;
    if (words[2] == 0u || words[5] == 0u) return TB710FU_NVT_ERR_FW;
    if ((uint64_t)words[0] + words[2] > bytes) return TB710FU_NVT_ERR_FW;
    if (words[3] < words[0] || (uint64_t)words[3] + words[5] > bytes)
        return TB710FU_NVT_ERR_FW;

    firmware->ilm_address = words[0];
    firmware->ilm_sram = words[1];
    firmware->ilm_bytes = words[2];
    firmware->dlm_address = words[3];
    firmware->dlm_sram = words[4];
    firmware->dlm_bytes = words[5];
    firmware->ilm_checksum = words[6];
    firmware->dlm_checksum = words[7];
    firmware->image_bytes = (uint32_t)bytes;
    /* The stored DLM word is the vendor CRC of the DLM payload on the pinned
     * blob; checking it binds the image without claiming the ILM checksum,
     * which covers the relocated copy. */
    firmware->dlm_crc_valid =
        tb710fu_nvt_firmware_crc32(image + words[3], words[5]) == words[7];
    return TB710FU_NVT_OK;
}
