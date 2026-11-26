/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The WCN7850 firmware manifest: the three files the TB710FU stock firmware tree ships, checked before any byte reaches
 * the device. Sizes and magic are measured from .local/input-sources/firmware/tb710fu-firmware-20261005.tar.gz
 * (MD5SUMS lists lib/firmware/ath12k/WCN7850/hw2.0/{amss.bin,board-2.bin,m3.bin}). The files are ELF32 little-endian
 * ARM images (amss, m3) and the ath12k board container ("QCA-ATH12K-BOARD\0"). No image content is interpreted here. */
#ifndef TB710FU_WIFI_MANIFEST_H
#define TB710FU_WIFI_MANIFEST_H

#include <stdint.h>

#include "Tb710fuWifiRegs.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TB710FU_WIFI_FW_OK = 0,
    TB710FU_WIFI_FW_UNKNOWN_NAME,
    TB710FU_WIFI_FW_TOO_SMALL,
    TB710FU_WIFI_FW_TOO_LARGE,
    TB710FU_WIFI_FW_BAD_MAGIC,
};

enum {
    TB710FU_WIFI_FW_BIT_AMSS = 1u << 0,
    TB710FU_WIFI_FW_BIT_M3 = 1u << 1,
    TB710FU_WIFI_FW_BIT_BOARD = 1u << 2,
    TB710FU_WIFI_FW_BIT_ALL = 7u,
};

struct tb710fu_wifi_fw_info {
    uint64_t bytes;
    uint32_t bits;      /* which manifest entry this is */
    uint32_t elf_class; /* e_ident[EI_CLASS] for the ELF images, 0 for board-2.bin */
    uint32_t elf_data;
    uint32_t elf_machine;
};

int tb710fu_wifi_fw_check(const char *name, const uint8_t *bytes, uint64_t size, struct tb710fu_wifi_fw_info *info);

/* Whole-manifest check: bitmap of the files seen, and the largest total that is accepted. */
struct tb710fu_wifi_manifest {
    uint32_t present;
    uint64_t amss_bytes, m3_bytes, board_bytes, total_bytes;
};

int tb710fu_wifi_manifest_note(struct tb710fu_wifi_manifest *manifest, const struct tb710fu_wifi_fw_info *info);
int tb710fu_wifi_manifest_complete(const struct tb710fu_wifi_manifest *manifest);

#ifdef __cplusplus
}
#endif

#endif /* TB710FU_WIFI_MANIFEST_H */
