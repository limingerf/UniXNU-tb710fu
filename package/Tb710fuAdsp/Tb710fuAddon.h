/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The boot addon container ("NGPUADD2", UniXNU scripts/make-gpu-addon.py and scripts/addon-add-dir.py) as a portable,
 * bounds-checked reader. The loader hands the addon to the kernel as /chosen/memory-map GpuAddon; this parser finds one
 * entry by its full path, and Tb710fuAdspFirmware.c inflates it.
 *
 * Layout (little endian), all offsets relative to the container:
 *   24-byte header: "NGPUADD2", u32 entry count, u32 zero, u64 total bytes (= the file's size)
 *   per entry, 24 bytes: u32 path bytes with its NUL (<= 160), u32 mode, u64 size (inflated), u64 compressed size
 *   then the path padded to 8 bytes, then the zlib stream padded to 8 bytes.
 *
 * Provenance: the walk is UniXNU-xiaomipad8pro's pfw_addon_find (package/common/PianoFirmwareCore.c, BSD-2-Clause-Patent),
 * with the piano firmware type replaced by a plain result code. Every length is checked before use and the whole container
 * is walked to its exact end, so a damaged one is refused even when the wanted entry comes first. */
#ifndef TB710FU_ADDON_H
#define TB710FU_ADDON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TB710FU_ADDON_MAGIC "NGPUADD2"
#define TB710FU_ADDON_HEADER 24u
#define TB710FU_ADDON_ENTRY 24u
#define TB710FU_ADDON_PATH_MAX 160u
#define TB710FU_ADDON_MAX_ENTRIES 4096u
#define TB710FU_ADDON_MAX_BYTES (64ULL << 20)   /* DarwinBoot reads at most 64 MiB */

enum {
    TB710FU_ADDON_OK = 0,
    TB710FU_ADDON_NOT_FOUND = 1,
    TB710FU_ADDON_DAMAGED = 2,
};

typedef struct {
    const char *path;          /* inside the addon, NUL-terminated */
    uint32_t mode;
    uint64_t size;             /* inflated size */
    const uint8_t *packed;     /* the zlib stream */
    uint64_t packed_bytes;
} Tb710fuAddonEntry;

/* The entry named PATH (exactly, case included). TB710FU_ADDON_OK, TB710FU_ADDON_NOT_FOUND, TB710FU_ADDON_DAMAGED. */
int tb710fu_addon_find(const uint8_t *addon, uint64_t bytes, const char *path, Tb710fuAddonEntry *entry);

#ifdef __cplusplus
}
#endif

#endif
