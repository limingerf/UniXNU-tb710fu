// SPDX-License-Identifier: BSD-2-Clause-Patent
// Whole-image firmware loader. Order: the boot addon (EFI/UniXNU/firmware/Wifi/<name>, inflated here), then the Data
// volume directory (wifi-fw or /private/var/firmware/ath12k/WCN7850/hw2.0). A file is read once, completely, into one
// IOMallocData buffer; MHI copies it into its physically contiguous BHI/BHIe buffer later.
//
// Provenance: the VFS read/close pair, the ENOENT/EFTYPE conventions and the addon container layout are UniXNU-sheng
// AdspFirmware.c and UniXNU-xiaomipad8pro's pfw_addon_find (package/common/PianoFirmwareCore.c), both
// BSD-2-Clause-Patent; zlib is libkern's, whose allocator is NO_ZCFUNCS and so needs the size-prefixed blocks below.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/errno.h>
#include <mach/vm_types.h>
#include <libkern/zlib.h>

#include "Tb710fuWifiFirmware.h"
#include "Tb710fuWifiRegs.h"

extern void *IOMallocData(vm_size_t size) __attribute__((alloc_size(1)));
extern void IOFreeData(void *address, vm_size_t size);

/* NativeRamBlock.cpp publishes the boot addon; weak so the archive links without that driver (Data volume only). */
extern int native_ram_block_gpu_addon(const uint8_t **data, uint64_t *bytes) __attribute__((weak_import));

#define kWifiAddonMagic "NGPUADD2"
#define kWifiAddonHeader 24u
#define kWifiAddonEntry 24u
#define kWifiAddonPathMax 160u
#define kWifiAddonMaxEntries 4096u
#define kWifiAddonMaxBytes (64ULL << 20)
#define kZPrefix 16u

struct wifiAddonEntry {
    const uint8_t *packed;
    uint64_t packedBytes;
    uint64_t size;
};

static uint32_t readU32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t readU64(const uint8_t *p)
{
    return (uint64_t)readU32(p) | (uint64_t)readU32(p + 4) << 32;
}

/* Walks the whole container and checks every length before use; a damaged one is refused even if the wanted entry is
 * first. Returns 0 and fills OUT, or ENOENT / EFTYPE. */
static int wifiAddonFind(const uint8_t *addon, uint64_t bytes, const char *path, struct wifiAddonEntry *out)
{
    if (!addon || bytes < kWifiAddonHeader || memcmp(addon, kWifiAddonMagic, 8) != 0) {
        return EFTYPE;
    }
    const uint32_t count = readU32(addon + 8);
    if (!count || count > kWifiAddonMaxEntries || readU64(addon + 16) != bytes) {
        return EFTYPE;
    }
    uint64_t offset = kWifiAddonHeader;
    for (uint32_t i = 0; i < count; ++i) {
        if (offset + kWifiAddonEntry > bytes) {
            return EFTYPE;
        }
        const uint8_t *entry = addon + offset;
        const uint32_t pathBytes = readU32(entry);
        const uint64_t size = readU64(entry + 8);
        const uint64_t packedBytes = readU64(entry + 16);
        if (!pathBytes || pathBytes > kWifiAddonPathMax) {
            return EFTYPE;
        }
        offset += kWifiAddonEntry;
        if (offset + pathBytes > bytes) {
            return EFTYPE;
        }
        const char *name = (const char *)(addon + offset);
        if (name[pathBytes - 1] != '\0') {
            return EFTYPE;
        }
        offset += (pathBytes + 7u) & ~7ull;
        if (offset > bytes || packedBytes > bytes - offset || packedBytes > kWifiAddonMaxBytes || size > kWifiAddonMaxBytes) {
            return EFTYPE;
        }
        if (strcmp(name, path) == 0) {
            out->packed = addon + offset;
            out->packedBytes = packedBytes;
            out->size = size;
            return 0;
        }
        offset += (packedBytes + 7u) & ~7ull;
        if (offset > bytes) {
            return EFTYPE;
        }
    }
    return ENOENT;
}

static voidpf zblockAlloc(voidpf opaque, uInt items, uInt size)
{
    (void)opaque;
    const uint64_t bytes = (uint64_t)items * size;
    if (!bytes || bytes > kWifiAddonMaxBytes) {
        return Z_NULL;
    }
    uint8_t *p = IOMallocData((vm_size_t)bytes + kZPrefix);
    if (!p) {
        return Z_NULL;
    }
    for (unsigned i = 0; i < 8; ++i) {
        p[i] = (uint8_t)(bytes >> (8 * i));
    }
    return p + kZPrefix;
}

static void zblockFree(voidpf opaque, voidpf address)
{
    (void)opaque;
    if (!address) {
        return;
    }
    uint8_t *p = (uint8_t *)address - kZPrefix;
    uint64_t bytes = 0;
    for (unsigned i = 0; i < 8; ++i) {
        bytes |= (uint64_t)p[i] << (8 * i);
    }
    IOFreeData(p, (vm_size_t)bytes + kZPrefix);
}

static int inflateImage(uint8_t *to, uint64_t toBytes, const uint8_t *from, uint64_t fromBytes)
{
    if (!toBytes || toBytes > kWifiAddonMaxBytes || !fromBytes || fromBytes > kWifiAddonMaxBytes) {
        return EFTYPE;
    }
    z_stream s;
    uint8_t *raw = (uint8_t *)&s;
    for (vm_size_t i = 0; i < sizeof s; ++i) {
        raw[i] = 0;
    }
    s.next_in = (Bytef *)(uintptr_t)from;
    s.avail_in = (uInt)fromBytes;
    s.next_out = to;
    s.avail_out = (uInt)toBytes;
    s.zalloc = zblockAlloc;
    s.zfree = zblockFree;
    s.opaque = NULL;
    if (inflateInit(&s) != Z_OK) {
        return EIO;
    }
    const int rc = inflate(&s, Z_FINISH);
    const int ok = rc == Z_STREAM_END && s.total_out == toBytes && s.avail_in == 0;
    inflateEnd(&s);
    return ok ? 0 : EIO;
}

static int loadFromAddon(const char *addonName, uint64_t minBytes, uint64_t maxBytes, struct tb710fu_wifi_fw *fw)
{
    if (!native_ram_block_gpu_addon) {
        return ENOENT;
    }
    const uint8_t *addon = NULL;
    uint64_t bytes = 0;
    if (!native_ram_block_gpu_addon(&addon, &bytes) || !addon || bytes < kWifiAddonHeader) {
        return ENOENT;
    }
    char path[kWifiAddonPathMax];
    const int length = snprintf(path, sizeof path, "%s%s", TB710FU_WIFI_FW_ADDON_PREFIX, addonName);
    if (length <= 0 || (size_t)length >= sizeof path) {
        return ENAMETOOLONG;
    }
    struct wifiAddonEntry entry;
    const int found = wifiAddonFind(addon, bytes, path, &entry);
    if (found) {
        return found;
    }
    if (entry.size < minBytes || entry.size > maxBytes) {
        return EFTYPE;
    }
    uint8_t *image = IOMallocData((vm_size_t)entry.size);
    if (!image) {
        return ENOMEM;
    }
    const int error = inflateImage(image, entry.size, entry.packed, entry.packedBytes);
    if (error) {
        IOFreeData(image, (vm_size_t)entry.size);
        return error;
    }
    fw->bytes = image;
    fw->size = entry.size;
    return 0;
}

static int loadFromVolume(const char *dir, const char *name, uint64_t minBytes, uint64_t maxBytes,
                          struct tb710fu_wifi_fw *fw)
{
    char path[192];
    const int length = snprintf(path, sizeof path, "%s/%s", dir ? dir : TB710FU_WIFI_FW_DIR, name);
    if (length <= 0 || (size_t)length >= sizeof path) {
        return ENAMETOOLONG;
    }
    vnode_t vnode = NULLVP;
    vfs_context_t ctx = vfs_context_kernel();
    int error = vnode_open(path, FREAD, 0, 0, &vnode, ctx);
    if (error) {
        return error;
    }
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    error = vnode_getattr(vnode, &va, ctx);
    const uint64_t size = (error || !VATTR_IS_SUPPORTED(&va, va_data_size)) ? 0 : va.va_data_size;
    if (!error && (size < minBytes || size > maxBytes)) {
        error = EFTYPE;
    }
    uint8_t *image = NULL;
    if (!error) {
        image = IOMallocData((vm_size_t)size);
        if (!image) {
            error = ENOMEM;
        }
    }
    uint64_t offset = 0;
    while (!error && offset < size) {
        const uint32_t want = (size - offset) > (1u << 20) ? (1u << 20) : (uint32_t)(size - offset);
        int resid = (int)want;
        error = vn_rdwr(UIO_READ, vnode, (caddr_t)(image + offset), (int)want, (off_t)offset, UIO_SYSSPACE, IO_NOAUTH,
                        vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        if (!error && resid == (int)want) {
            error = EIO;
        }
        offset += want - (uint64_t)resid;
    }
    vnode_close(vnode, FREAD, ctx);
    if (error) {
        if (image) {
            IOFreeData(image, (vm_size_t)size);
        }
        return error;
    }
    fw->bytes = image;
    fw->size = size;
    return 0;
}

int tb710fu_wifi_fw_load(const char *dir, const char *name, const char *addonName,
                         uint64_t minBytes, uint64_t maxBytes, struct tb710fu_wifi_fw *fw)
{
    if (!name || !fw) {
        return EINVAL;
    }
    fw->bytes = NULL;
    fw->size = 0;
    int error = ENOENT;
    if (addonName) {
        error = loadFromAddon(addonName, minBytes, maxBytes, fw);
    }
    if (error) {
        error = loadFromVolume(dir, name, minBytes, maxBytes, fw);
    }
    return error;
}

void tb710fu_wifi_fw_release(struct tb710fu_wifi_fw *fw)
{
    if (!fw || !fw->bytes) {
        return;
    }
    IOFreeData(fw->bytes, (vm_size_t)fw->size);
    fw->bytes = NULL;
    fw->size = 0;
}
