/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Firmware source for the WCN7850: one whole image in memory, from the boot addon or from the Data volume.
 * This subsystem deliberately carries its own small addon reader instead of linking Tb710fuAdsp's, so the archive can
 * be built alone and no symbol is shared with another driver. The container format and the VFS/zlib conventions are
 * the ones already proven in package/Tb710fuAdsp/Tb710fuAdspFirmware.c (BSD-2-Clause-Patent). */
#ifndef TB710FU_WIFI_FIRMWARE_H
#define TB710FU_WIFI_FIRMWARE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tb710fu_wifi_fw {
    uint8_t *bytes;
    uint64_t size;
};

/* dir == NULL means TB710FU_WIFI_FW_DIR. addon_name == NULL skips the boot addon. The image is checked against
 * [min_bytes, max_bytes] before it is allocated, so a wrong file cannot exhaust the heap. */
int tb710fu_wifi_fw_load(const char *dir, const char *name, const char *addon_name,
                         uint64_t min_bytes, uint64_t max_bytes, struct tb710fu_wifi_fw *fw);
void tb710fu_wifi_fw_release(struct tb710fu_wifi_fw *fw);

#ifdef __cplusplus
}
#endif

#endif /* TB710FU_WIFI_FIRMWARE_H */
