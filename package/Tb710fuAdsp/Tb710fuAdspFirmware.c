// SPDX-License-Identifier: BSD-2-Clause-Patent
// Firmware files for the audio DSP. Two sources, tried in this order unless the owner pins a directory:
//   1. the boot's addon: /chosen/memory-map GpuAddon (the NGPUADD2 container the payload's kernel.macho.gpuaddon carries),
//      entry EFI/UniXNU/firmware/Adsp/adsp.mbn and adsp_dtb.mbn, zlib streams inflated here with libkern's zlib. This is the
//      same channel UniXNU's own firmware loader uses (PianoFirmware.h: kext pins -> the boot addon -> the ESP), so the two
//      files travel inside vendor_boot and need no manual install.
//   2. the Data volume, /private/var/firmware/qcom/sm8650/xiaoxin/gt or the directory the adsp-fw boot argument names:
//      the files an owner installed once, for bring-up without rebuilding the payload.
// The ADSP bring-up retries until the Data volume is mounted (it only appears after the root mount, later than the services
// start). Plain reads on the ADSP thread: it runs no interrupt work, so a slow read only delays that bring-up.
//
// Provenance: the VFS read/close pair and the ENOENT/EFTYPE conventions are UniXNU-sheng's AdspFirmware.c
// (BSD-2-Clause-Patent). The addon parse is Tb710fuAddon.c (UniXNU-xiaomipad8pro's pfw_addon_find, BSD-2-Clause-Patent);
// the zlib allocator follows that project's PianoFirmwareZlib.c because this kernel's zlib is built with NO_ZCFUNCS and
// has no default allocator.
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

#include "Tb710fuAddon.h"

/* Only the two data-heap entry points from IOKit are needed here.  Including <IOKit/IOLib.h> pulls
 * <IOKit/system.h> and the whole mach_interface.h server surface, whose generated headers need the
 * private osfmk include tree; the BSD compile recipe deliberately does not have that, and adding it
 * makes <kern/ipc_tt.h> collide with the exported kern/thread.h enum. */
extern void *IOMallocData(vm_size_t size) __attribute__((alloc_size(1)));
extern void IOFreeData(void *address, vm_size_t size);

#define kAddonPrefix "EFI/UniXNU/firmware/Adsp/"
#define kZPrefix 16u
/* The two files the Linux port's archive ships (MD5SUMS-verified there): a wrong image is refused before the MDT parse. */
#define kAdspBytes 34793816ull
#define kAdspDtbBytes 65336ull

struct tb710fu_fw {
    vnode_t vnode;          /* the Data-volume file, or NULLVP for an addon image */
    const uint8_t *bytes;   /* the inflated addon image (owned), or NULL */
    uint64_t size;
    uint8_t owned;          /* bytes came from IOMallocData and is freed here */
};

int tb710fu_fw_open(const char *path, void **handle, uint64_t *size);
int tb710fu_fw_open_addon(const char *name, void **handle, uint64_t *size);
int tb710fu_fw_read(void *handle, uint64_t offset, void *to, size_t length);
void tb710fu_fw_close(void *handle);

/* NativeRamBlock.cpp publishes the loader's addon (already bounds checked against the physical window). Weak: a kernel built
 * without that driver has no addon and falls back to the Data volume. */
extern int native_ram_block_gpu_addon(const uint8_t **data, uint64_t *bytes) __attribute__((weak_import));

static int allocImage(uint64_t bytes, const uint8_t **image, void **handle)
{
    struct tb710fu_fw *fw = IOMallocData(sizeof *fw);
    if (!fw) return ENOMEM;
    bzero(fw, sizeof *fw);
    fw->vnode = NULLVP;
    fw->bytes = IOMallocData((vm_size_t)bytes);
    if (!fw->bytes) {IOFreeData(fw, sizeof *fw); return ENOMEM;}
    fw->size = bytes;
    fw->owned = 1;
    *image = fw->bytes;
    *handle = fw;
    return 0;
}

/* libkern's zlib has no allocator of its own: every block carries its size in a prefix the matching free reads back. */
static voidpf zblockAlloc(voidpf opaque, uInt items, uInt size)
{
    (void)opaque;
    const uint64_t bytes = (uint64_t)items * size;
    if (!bytes || bytes > TB710FU_ADDON_MAX_BYTES) return Z_NULL;
    uint8_t *p = IOMallocData((vm_size_t)bytes + kZPrefix);
    if (!p) return Z_NULL;
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(bytes >> (8 * i));
    return p + kZPrefix;
}

static void zblockFree(voidpf opaque, voidpf address)
{
    (void)opaque;
    if (!address) return;
    uint8_t *p = (uint8_t *)address - kZPrefix;
    uint64_t bytes = 0;
    for (unsigned i = 0; i < 8; ++i) bytes |= (uint64_t)p[i] << (8 * i);
    IOFreeData(p, (vm_size_t)bytes + kZPrefix);
}

static int inflateImage(uint8_t *to, uint64_t to_bytes, const uint8_t *from, uint64_t from_bytes)
{
    z_stream s;
    uint8_t *raw = (uint8_t *)&s;
    for (size_t i = 0; i < sizeof s; ++i) raw[i] = 0;
    if (!to_bytes || to_bytes > TB710FU_ADDON_MAX_BYTES || !from_bytes || from_bytes > TB710FU_ADDON_MAX_BYTES) return EFTYPE;
    s.next_in = (Bytef *)(uintptr_t)from;
    s.avail_in = (uInt)from_bytes;
    s.next_out = to;
    s.avail_out = (uInt)to_bytes;
    s.zalloc = zblockAlloc;
    s.zfree = zblockFree;
    s.opaque = NULL;
    if (inflateInit(&s) != Z_OK) return EIO;
    const int rc = inflate(&s, Z_FINISH);
    const int ok = rc == Z_STREAM_END && s.total_out == to_bytes && s.avail_in == 0;
    inflateEnd(&s);
    return ok ? 0 : EIO;
}

int tb710fu_fw_open_addon(const char *name, void **handle, uint64_t *size)
{
    if (!native_ram_block_gpu_addon) return ENOENT;
    const uint8_t *addon = NULL;
    uint64_t bytes = 0;
    if (!native_ram_block_gpu_addon(&addon, &bytes) || !addon || bytes < TB710FU_ADDON_HEADER) return ENOENT;
    char path[TB710FU_ADDON_PATH_MAX];
    const int length = snprintf(path, sizeof path, "%s%s", kAddonPrefix, name);
    if (length <= 0 || (size_t)length >= sizeof path) return ENAMETOOLONG;
    Tb710fuAddonEntry entry;
    const int found = tb710fu_addon_find(addon, bytes, path, &entry);
    if (found == TB710FU_ADDON_NOT_FOUND) return ENOENT;
    if (found != TB710FU_ADDON_OK || !entry.size || entry.size > TB710FU_ADDON_MAX_BYTES) return EFTYPE;
    const uint64_t expected = strcmp(name, "adsp.mbn") == 0 ? kAdspBytes
                            : strcmp(name, "adsp_dtb.mbn") == 0 ? kAdspDtbBytes : 0;
    if (expected && entry.size != expected) return EFTYPE;
    const uint8_t *image = NULL;
    int error = allocImage(entry.size, &image, handle);
    if (error) return error;
    if ((error = inflateImage((uint8_t *)image, entry.size, entry.packed, entry.packed_bytes))) {
        tb710fu_fw_close(*handle);
        *handle = NULL;
        return error;
    }
    *size = entry.size;
    return 0;
}

int tb710fu_fw_open(const char *path, void **handle, uint64_t *size)
{
    struct tb710fu_fw *fw = IOMallocData(sizeof *fw);
    if (!fw) return ENOMEM;
    bzero(fw, sizeof *fw);
    fw->vnode = NULLVP;
    vfs_context_t ctx = vfs_context_kernel();
    struct vnode_attr va;
    int error = vnode_open(path, FREAD, 0, 0, &fw->vnode, ctx);
    if (!error) {
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_data_size);
        if ((error = vnode_getattr(fw->vnode, &va, ctx)) || !VATTR_IS_SUPPORTED(&va, va_data_size) || !va.va_data_size) {
            vnode_close(fw->vnode, FREAD, ctx);
            fw->vnode = NULLVP;
            if (!error) error = EFTYPE;
        } else {
            *size = va.va_data_size;
        }
    }
    if (error) {IOFreeData(fw, sizeof *fw); return error;}
    *handle = fw;
    return 0;
}

int tb710fu_fw_read(void *handle, uint64_t offset, void *to, size_t length)
{
    struct tb710fu_fw *fw = handle;
    if (fw->bytes) {
        if (offset > fw->size || length > fw->size - offset) return EIO;
        memcpy(to, fw->bytes + offset, length);
        return 0;
    }
    vfs_context_t ctx = vfs_context_kernel();
    uint8_t *out = (uint8_t *)to;
    while (length) {
        const int want = length > (1u << 20) ? (1 << 20) : (int)length;
        int resid = want;
        const int error = vn_rdwr(UIO_READ, fw->vnode, (caddr_t)out, want, (off_t)offset, UIO_SYSSPACE, IO_NOAUTH,
                                  vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        if (error) return error;
        if (resid == want) return EIO;  /* end of file before the bytes asked for */
        out += want - resid;
        offset += (uint64_t)(want - resid);
        length -= (size_t)(want - resid);
    }
    return 0;
}

void tb710fu_fw_close(void *handle)
{
    struct tb710fu_fw *fw = handle;
    if (!fw) return;
    if (fw->vnode) vnode_close(fw->vnode, FREAD, vfs_context_kernel());
    if (fw->owned && fw->bytes) IOFreeData((uint8_t *)fw->bytes, (vm_size_t)fw->size);
    IOFreeData(fw, sizeof *fw);
}
