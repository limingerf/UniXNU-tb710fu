// SPDX-License-Identifier: BSD-2-Clause-Patent
// Firmware files for the audio DSP, read through VFS from the Data volume (the files are never part of the build):
//   /private/var/firmware/qcom/sm8650/xiaoxin/gt/adsp.mbn      the DSP image (a joined .mbn, as the firmware archive ships it)
//   /private/var/firmware/qcom/sm8650/xiaoxin/gt/adsp_dtb.mbn  the DSP's own device tree image
// The owner installs them once; the ADSP bring-up retries until the volume is mounted (the Data volume only appears after the root
// mount, which is later than the services start). Plain reads on the ADSP thread: it runs no interrupt work, so a slow read only
// delays that bring-up.
//
// Provenance: the read/close pair and the ENOENT/EFTYPE conventions are UniXNU-sheng's AdspFirmware.c (BSD-2-Clause-Patent). The
// split-image and FAT-partition halves are dropped: this board's firmware comes from the Linux port's archive as two joined .mbn
// files, and no firmware partition is read here.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/errno.h>
#include <IOKit/IOLib.h>

struct tb710fu_fw {
    vnode_t vnode;
};

int tb710fu_fw_open(const char *path, void **handle, uint64_t *size);
int tb710fu_fw_read(void *handle, uint64_t offset, void *to, size_t length);
void tb710fu_fw_close(void *handle);

int tb710fu_fw_open(const char *path, void **handle, uint64_t *size)
{
    struct tb710fu_fw *fw = IOMallocType(struct tb710fu_fw);
    if (!fw) return ENOMEM;
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
    if (error) {IOFreeType(fw, struct tb710fu_fw); return error;}
    *handle = fw;
    return 0;
}

int tb710fu_fw_read(void *handle, uint64_t offset, void *to, size_t length)
{
    struct tb710fu_fw *fw = handle;
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
    IOFreeType(fw, struct tb710fu_fw);
}
