/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* NT36532 firmware image validation.
 *
 * The stock blob this board ships is novatek/novatek_nt36532_fw.bin, pinned by
 * .local/input-sources/firmware/MD5SUMS as
 *   bca72e403b2b4a521127dcc84fc9848f  novatek/novatek_nt36532_fw.bin
 * and 245760 bytes (0x3C000) in the pinned firmware tarball. It is NOT part of
 * this repository and is NOT uploaded by this driver: the controller is already
 * running its stock firmware after a warm handover, and a wrong upload leaves
 * the panel dark until the next power cycle. This module is the part that can
 * be verified off-target.
 *
 * Header layout, read off the pinned blob (all little endian, offsets in bytes):
 *
 *   0x00  ILM file offset    0x00000200
 *   0x04  ILM SRAM address   0x00000000
 *   0x08  ILM byte count     0x0001FE37   (ends at 0x00020037, inside the file)
 *   0x0C  DLM file offset    0x00022000
 *   0x10  DLM SRAM address   0x00100000
 *   0x14  DLM byte count     0x00014207   (ends at 0x00036207, inside the file)
 *   0x18  stored checksum    0xF97BA55A   (see below)
 *   0x1C  DLM checksum       0x8E59A050   (= vendor CRC-32 of the DLM region)
 *
 * The 0x18 word does not equal the same CRC over the ILM payload, so this
 * parser records it but does not claim to verify it: the reference implementation
 * checksums the ILM after writing it to SRAM, and the relocation is not
 * reproducible from the pinned sources. The DLM word was verified against the
 * blob, so it is checked.
 *
 * The two region descriptions are the vendor's own partition table as ported by
 * UniXNU/ShengNovatekTouch (NvtProtocol.c, revision 3b44b1b7), which is also
 * where the CRC-32 polynomial below comes from.
 */
#ifndef TB710FU_NVT_FIRMWARE_H
#define TB710FU_NVT_FIRMWARE_H

#include <stdint.h>
#include <stddef.h>

#define TB710FU_NVT_FW_HEADER_BYTES  0x20u
#define TB710FU_NVT_FW_MIN_BYTES     0x40u
#define TB710FU_NVT_FW_MAX_BYTES     0x100000u
#define TB710FU_NVT_FW_ILM_MIN       0x200u
#define TB710FU_NVT_FW_ILM_MAX       0x2000u
#define TB710FU_NVT_FW_CRC_POLY      0x04C11DB7u

struct tb710fu_nvt_firmware {
    uint32_t ilm_address;    /* file offset of the ILM payload */
    uint32_t ilm_sram;       /* SRAM address the bootloader loads it at */
    uint32_t ilm_bytes;
    uint32_t dlm_address;    /* file offset of the DLM payload */
    uint32_t dlm_sram;
    uint32_t dlm_bytes;
    uint32_t ilm_checksum;   /* stored at 0x18, recorded but not verified */
    uint32_t dlm_checksum;   /* stored at 0x1C, verified against the payload */
    uint32_t image_bytes;
    int dlm_crc_valid;
};

/* Vendor CRC-32 over little-endian 32-bit words, initial value 0xFFFFFFFF,
 * polynomial 0x04C11DB7, no final inversion. */
uint32_t tb710fu_nvt_firmware_crc32(const uint8_t *data, size_t bytes);

/* Returns TB710FU_NVT_OK and fills `firmware`, or TB710FU_NVT_ERR_FW when the
 * region descriptions do not fit inside `bytes`. Never reads past `bytes`. */
int tb710fu_nvt_firmware_parse(const uint8_t *image, size_t bytes,
                               struct tb710fu_nvt_firmware *firmware);

#endif /* TB710FU_NVT_FIRMWARE_H */
