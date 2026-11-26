/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Firmware reader contract used by the Bluetooth driver.
 *
 * The implementation lives in package/Tb710fuAdsp/Tb710fuAdspFirmware.c, which already serves the ADSP
 * blobs from the boot addon container and from a VFS directory, with libkern zlib for the addon's
 * compressed entries. This package links against those four functions as weak imports: a kernel built
 * without the ADSP archive still links, reports "no firmware reader in this kernel", and stops before the
 * firmware stage instead of failing obscurely later. Duplicating vnode plus zlib here would be a second
 * copy of a 400-line reader for no benefit; docs/BLUETOOTH_WCN7850.md records the merge requirement.
 *
 * Semantics copied from Tb710fuAdsp.cpp's own declarations:
 *   tb710fu_fw_open(path)        absolute VFS path
 *   tb710fu_fw_open_addon(name)  entry name inside the boot addon (EFI/UniXNU/firmware/Adsp/<name>)
 *   tb710fu_fw_read(handle, offset, to, length)  0 on success
 *   tb710fu_fw_close(handle)
 */
#ifndef TB710FU_BLUETOOTH_FIRMWARE_H
#define TB710FU_BLUETOOTH_FIRMWARE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Weak because the implementation is the ADSP package: a kernel without that archive must still link,
 * and the driver checks the pointers before the firmware stage. */
int tb710fu_fw_open(const char *path, void **handle, uint64_t *size) __attribute__((weak_import));
int tb710fu_fw_open_addon(const char *name, void **handle, uint64_t *size) __attribute__((weak_import));
int tb710fu_fw_read(void *handle, uint64_t offset, void *to, size_t length) __attribute__((weak_import));
void tb710fu_fw_close(void *handle) __attribute__((weak_import));

#ifdef __cplusplus
}
#endif
#endif /* TB710FU_BLUETOOTH_FIRMWARE_H */
