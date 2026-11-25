// SPDX-License-Identifier: MIT
// Adapted from UniXNU/UniXNU-sheng@1108068537d5396edb2c63b2f8c6f5b97b0a4bbc,
// package/BootWatchdogPolicy.c. Runs after writable Data + firmlinks mount,
// before /sbin/launchd exec, to write disabled.plist when disable-watchdogd=1.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysproto.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include <sys/uio.h>
#include <sys/dirent.h>
#include <pexpert/pexpert.h>

/* vnode_if.h keeps this behind XNU_KERNEL_PRIVATE, which MakeInc.def defines
 * for the kernel build. A second identical prototype costs nothing and keeps
 * this file compiling if the record ever drops the define. */
extern errno_t VNOP_READDIR(vnode_t, struct uio *, int, int *, int *,
                            vfs_context_t);

static const char policy_head[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\"><dict>";
static const char policy_watchdogd[] = "<key>com.apple.watchdogd</key><true/>";
static const char policy_tail[] = "</dict></plist>\n";

volatile int tb710fu_watchdog_policy_status = -1;

static int
append_job(char *policy, size_t cap, const char **cursor)
{
    const char *p = *cursor;
    while (*p == ',') {
        ++p;
    }
    if (!*p) {
        *cursor = p;
        return 0;
    }
    const char *start = p;
    while (*p && *p != ',') {
        char ch = *p;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_')) {
            return EINVAL;
        }
        ++p;
    }
    size_t len = (size_t)(p - start);
    if (!len || len > 120) {
        return EINVAL;
    }
    char label[128];
    memcpy(label, start, len);
    label[len] = '\0';
    if (strlcat(policy, "<key>", cap) >= cap ||
        strlcat(policy, label, cap) >= cap ||
        strlcat(policy, "</key><true/>", cap) >= cap) {
        return ENAMETOOLONG;
    }
    *cursor = p;
    return 0;
}

static void
probe_path(proc_t p, user_addr_t scratch, const char *path)
{
    int32_t result = 0;
    if (copyout(path, scratch, strlen(path) + 1) != 0) {
        return;
    }
    struct open_args op = { .path = scratch, .flags = O_RDONLY, .mode = 0 };
    int err = open(p, &op, &result);
    if (err == 0 && result >= 0) {
        struct close_args cl = { .fd = result };
        (void)sys_close(p, &cl, &result);
    }
    printf("TB710FU boot-policy: open %s -> %d\n", path, err);
}

int
native_boot_watchdog_policy_apply(proc_t p, user_addr_t scratch)
{
    unsigned enabled = 0;
    char jobs[256] = { 0 };
    PE_parse_boot_argn("disable-watchdogd", &enabled, sizeof(enabled));
    PE_parse_boot_arg_str("disable-jobs", jobs, sizeof(jobs));

    /* Non-fatal P0-5 diagnostic: verify Preboot & Cryptexes/OS visibility before launchd. */
    probe_path(p, scratch, "/System/Volumes/Preboot/Cryptexes/OS");
    probe_path(p, scratch, "/System/Volumes/Preboot/84581E99-85BE-4957-AC53-2947E887B3E4");

    if (!enabled && !jobs[0]) {
        tb710fu_watchdog_policy_status = 0;
        return 0;
    }

    char policy[2048];
    policy[0] = '\0';
    strlcat(policy, policy_head, sizeof(policy));
    if (enabled) {
        strlcat(policy, policy_watchdogd, sizeof(policy));
    }
    int error = 0;
    for (const char *c = jobs; *c;) {
        if ((error = append_job(policy, sizeof(policy), &c)) != 0) {
            tb710fu_watchdog_policy_status = error;
            printf("TB710FU boot-policy: invalid disable-jobs (%d)\n", error);
            return error;
        }
    }
    strlcat(policy, policy_tail, sizeof(policy));

    int32_t result = 0;
    int fd = -1;
    const char *directory = "/private/var/db/com.apple.xpc.launchd";
    const char *path = "/private/var/db/com.apple.xpc.launchd/disabled.plist";
    if ((error = copyout(directory, scratch, strlen(directory) + 1)) != 0) {
        goto done;
    }
    struct mkdir_args md = { .path = scratch, .mode = 0755 };
    error = mkdir(p, &md, &result);
    if (error && error != EEXIST) {
        goto done;
    }
    if ((error = copyout(path, scratch, strlen(path) + 1)) != 0) {
        goto done;
    }
    struct open_args op = {
        .path = scratch,
        .flags = O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
        .mode = 0644,
    };
    if ((error = open(p, &op, &result)) != 0) {
        goto done;
    }
    fd = result;
    for (size_t sent = 0, policy_len = strlen(policy); sent < policy_len;) {
        size_t piece = policy_len - sent < 0x1f0 ? policy_len - sent : 0x1f0;
        if ((error = copyout(policy + sent, scratch + 0x200, piece)) != 0) {
            goto done;
        }
        struct write_args wr = { .fd = fd, .cbuf = scratch + 0x200, .nbyte = piece };
        user_ssize_t written = 0;
        if ((error = write(p, &wr, &written)) != 0) {
            goto done;
        }
        if (written != (user_ssize_t)piece) {
            error = EIO;
            goto done;
        }
        sent += piece;
    }
    struct fchown_args owner = { .fd = fd, .uid = 0, .gid = 0 };
    if ((error = fchown(p, &owner, &result)) != 0) {
        goto done;
    }
    struct fchmod_args mode = { .fd = fd, .mode = 0644 };
    if ((error = fchmod(p, &mode, &result)) != 0) {
        goto done;
    }
    struct fsync_args sync = { .fd = fd };
    if ((error = fsync(p, &sync, &result)) != 0) {
        goto done;
    }
    struct fstat64_args sa = { .fd = fd, .ub = scratch + 0x800 };
    struct stat64 st = { 0 };
    if ((error = sys_fstat64(p, &sa, &result)) != 0 ||
        (error = copyin(sa.ub, &st, sizeof(st))) != 0) {
        goto done;
    }
    if (st.st_uid != 0 || st.st_gid != 0 || !S_ISREG(st.st_mode) ||
        (st.st_mode & 0777) != 0644 || st.st_size != (off_t)strlen(policy)) {
        error = EINVAL;
    }
done:
    if (fd >= 0) {
        struct close_args cl = { .fd = fd };
        int e = sys_close(p, &cl, &result);
        if (!error) {
            error = e;
        }
    }
    if (!error) {
        const char *marker = "/private/var/db/com.apple.xpc.launchd/disabled.migrated";
        error = copyout(marker, scratch, strlen(marker) + 1);
        if (!error) {
            struct open_args mark = {
                .path = scratch,
                .flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
                .mode = 0644,
            };
            error = open(p, &mark, &result);
            if (!error) {
                struct close_args cl = { .fd = result };
                error = sys_close(p, &cl, &result);
            } else if (error == EEXIST) {
                error = 0;
            }
        }
    }
    tb710fu_watchdog_policy_status = error;
    printf("TB710FU boot-policy: disable-watchdogd=%u status=%d\n", enabled, error);
    if (!error && enabled == 1u) {
        /* The plist only gates launchd's job import; pl45b showed the job still ran
         * (exec pid 84/239 watchdogd) and its brk #1 exit panicked the kernel. The
         * kernel side of the same boot argument (patch 0050) is what keeps that exit
         * from becoming a panic, so report both halves in one place. */
        printf("TB710FU boot-policy: launchd override written; kernel watchdogd exit guard armed\n");
    }
    return error;
}

extern task_t proc_task(proc_t);
extern int get_task_numacts(task_t);
extern uint64_t get_task_phys_footprint(task_t);
extern volatile uint32_t native_aes_kat_status __attribute__((weak_import));
extern volatile uint32_t native_aes_registered __attribute__((weak_import));
extern volatile uint32_t native_aes_completed __attribute__((weak_import));
extern volatile uint32_t native_aes_nonce_key_uses __attribute__((weak_import));
extern volatile uint32_t tb710fu_pid1_sc_count __attribute__((weak_import));
extern volatile uint32_t tb710fu_pid1_sc_in __attribute__((weak_import));
extern volatile uint32_t tb710fu_pid1_sc_cur __attribute__((weak_import));
extern volatile uint64_t tb710fu_pid1_sc_arg0 __attribute__((weak_import));
extern volatile uint64_t tb710fu_pid1_sc_arg1 __attribute__((weak_import));
extern volatile uint32_t tb710fu_pid1_sc_ring[16] __attribute__((weak_import));
extern volatile uint32_t tb710fu_pid1_sc_idx __attribute__((weak_import));

void
tb710fu_dump_procs(void)
{
    unsigned count = 0;
    printf("TB710FU PS: begin watchdog_policy=%d aes_kat=%u aes_reg=%u aes_done=%u aes_nonce=%u\n",
           tb710fu_watchdog_policy_status,
           &native_aes_kat_status ? native_aes_kat_status : 0u,
           &native_aes_registered ? native_aes_registered : 0u,
           &native_aes_completed ? native_aes_completed : 0u,
           &native_aes_nonce_key_uses ? native_aes_nonce_key_uses : 0u);
    if (&tb710fu_pid1_sc_count) {
        printf("TB710FU PID1: sc=%u in=%u cur=%u arg0=0x%llx arg1=0x%llx\n",
               tb710fu_pid1_sc_count, tb710fu_pid1_sc_in, tb710fu_pid1_sc_cur,
               (unsigned long long)tb710fu_pid1_sc_arg0,
               (unsigned long long)tb710fu_pid1_sc_arg1);
        uint32_t idx = tb710fu_pid1_sc_idx;
        printf("TB710FU PID1 RING:");
        for (uint32_t i = 0; i < 16u; ++i) {
            uint32_t v = tb710fu_pid1_sc_ring[(idx + i) & 15u];
            if (v) {
                printf(" %u:%u", v >> 16, v & 0xffffu);
            }
        }
        printf("\n");
    }
    for (int pid = 1; pid <= 256; ++pid) {
        proc_t p = proc_find(pid);
        if (p != PROC_NULL) {
            char name[32] = { 0 };
            proc_name(pid, name, (int)sizeof(name));
            task_t t = proc_task(p);
            int threads = t ? get_task_numacts(t) : -1;
            uint64_t fp = t ? get_task_phys_footprint(t) : 0;
            printf("TB710FU PS: pid=%d ppid=%d name=%s thr=%d fp=%llu\n",
                   pid, proc_ppid(p), name, threads,
                   (unsigned long long)fp);
            proc_rele(p);
            ++count;
        }
    }
    printf("TB710FU PS: end count=%u\n", count);
}

/* ---------------------------------------------------------------------------
 * COM5 debug shell: read-only VFS verbs.
 *
 * These run inside cdc_worker, a kernel thread created by
 * kernel_thread_start_priority whose current_proc() is kernproc. There is no
 * uthread behind it, so vfs_context_current() can resolve to a NULL BSD thread
 * info; every call here therefore passes vfs_context_kernel() explicitly, which
 * is the same context bsd_init.c uses for its own kernel-thread vnode lookup.
 *
 * Only one worker calls these, so the transfer buffers are plain file static
 * arrays instead of kalloc_data: fewer symbols to depend on, and no failure path
 * that could leave a page behind.
 * ------------------------------------------------------------------------- */
#define TB710FU_SHELL_PATH_MAX 256
#define TB710FU_SHELL_LS_BYTES 5120
#define TB710FU_SHELL_LS_ENTRIES 96
#define TB710FU_SHELL_CAT_BYTES 4096

static uint8_t gShellLsBuf[TB710FU_SHELL_LS_BYTES];
static char gShellCatBuf[TB710FU_SHELL_CAT_BYTES];

static const char
*shell_type(enum vtype type)
{
    switch (type) {
    case VREG: return "reg";
    case VDIR: return "dir";
    case VLNK: return "lnk";
    case VBLK: return "blk";
    case VCHR: return "chr";
    case VSOCK: return "sock";
    case VFIFO: return "fifo";
    case VSTR: return "str";
    case VCPLX: return "cplx";
    case VBAD: return "bad";
    default: return "non";
    }
}

/* Resolves an absolute path to a vnode with an iocount held. The caller owns
 * vnode_put() on success and nothing on failure. */
static int
shell_lookup(const char *path, vnode_t *vpp, vfs_context_t *ctxp)
{
    *vpp = NULL;
    if (path == NULL || path[0] != '/') {
        printf("TB710FU SHELL: an absolute path is required\n");
        return EINVAL;
    }
    if (strlen(path) >= TB710FU_SHELL_PATH_MAX) {
        printf("TB710FU SHELL: path too long (max %d)\n", TB710FU_SHELL_PATH_MAX - 1);
        return ENAMETOOLONG;
    }
    *ctxp = vfs_context_kernel();
    errno_t error = vnode_lookup(path, 0, vpp, *ctxp);
    if (error != 0 || *vpp == NULL) {
        *vpp = NULL;
        printf("TB710FU SHELL: lookup %s -> %d\n", path, (int)error);
        return error != 0 ? (int)error : ENOENT;
    }
    return 0;
}

/* VNOP_READDIR into one bounded transfer buffer. A directory with more entries
 * than fit (or than the display cap) reports itself truncated instead of
 * looping: the shell is a diagnostic, not a filesystem browser. */
int
tb710fu_shell_ls(const char *path)
{
    vnode_t vp = NULL;
    vfs_context_t ctx = NULL;
    int error = shell_lookup(path, &vp, &ctx);
    if (error) {
        return error;
    }
    if (!vnode_isdir(vp)) {
        printf("TB710FU LS: %s is not a directory (%s)\n", path, shell_type(vnode_vtype(vp)));
        vnode_put(vp);
        return ENOTDIR;
    }
    uio_t uio = uio_create(1, 0, UIO_SYSSPACE, UIO_READ);
    if (uio == NULL) {
        vnode_put(vp);
        printf("TB710FU LS: %s uio_create failed\n", path);
        return ENOMEM;
    }
    (void)uio_addiov(uio, (user_addr_t)(uintptr_t)gShellLsBuf, sizeof(gShellLsBuf));
    int eofflag = 0, numdirent = 0;
    error = (int)VNOP_READDIR(vp, uio, 0, &eofflag, &numdirent, ctx);
    user_ssize_t resid = uio_resid(uio);
    uio_free(uio);
    vnode_put(vp);
    if (error != 0 || resid < 0) {
        printf("TB710FU LS: %s readdir -> %d\n", path, error);
        return error != 0 ? error : EIO;
    }
    /* d_reclen/d_namlen are the only authority here: the FS packs records of
     * whatever size it likes into the buffer, and a zero-length record would
     * otherwise spin forever. */
    const unsigned header = (unsigned)(sizeof(struct dirent) -
                                       sizeof(((struct dirent *)0)->d_name));
    const unsigned used = (unsigned)(sizeof(gShellLsBuf) - (size_t)resid);
    unsigned offset = 0, count = 0;
    while (offset + header + 1u <= used) {
        struct dirent *entry = (struct dirent *)(void *)(gShellLsBuf + offset);
        if (entry->d_reclen < header + 1u || entry->d_reclen > used - offset ||
            entry->d_namlen + 1u > entry->d_reclen - header) {
            printf("TB710FU LS: %s malformed entry at %u (reclen=%u namlen=%u)\n",
                   path, offset, (unsigned)entry->d_reclen, (unsigned)entry->d_namlen);
            break;
        }
        if (count >= TB710FU_SHELL_LS_ENTRIES) {
            printf("TB710FU LS: %s truncated after %u entries\n", path,
                   (unsigned)TB710FU_SHELL_LS_ENTRIES);
            break;
        }
        char name[sizeof(((struct dirent *)0)->d_name)];
        memcpy(name, entry->d_name, entry->d_namlen);
        name[entry->d_namlen] = '\0';
        printf("TB710FU LS: %c %s\n",
               entry->d_type == DT_DIR ? 'd' :
               entry->d_type == DT_REG ? '-' :
               entry->d_type == DT_LNK ? 'l' : '?', name);
        ++count;
        offset += entry->d_reclen;
    }
    if (!eofflag && count < TB710FU_SHELL_LS_ENTRIES) {
        printf("TB710FU LS: %s more entries not shown\n", path);
    }
    printf("TB710FU LS: %s entries=%u eof=%d\n", path, count, eofflag);
    return 0;
}

/* First 4096 bytes of a regular file. Printable bytes are passed through,
 * newlines/tabs are literal, everything else becomes \xNN so a binary or a
 * lossy console encoding cannot swallow the transcript. */
int
tb710fu_shell_cat(const char *path)
{
    static const char hex[] = "0123456789abcdef";
    vnode_t vp = NULL;
    vfs_context_t ctx = NULL;
    int error = shell_lookup(path, &vp, &ctx);
    if (error) {
        return error;
    }
    if (!vnode_isreg(vp)) {
        printf("TB710FU CAT: %s is not a regular file (%s)\n", path,
               shell_type(vnode_vtype(vp)));
        vnode_put(vp);
        return EINVAL;
    }
    int resid = 0;
    error = vn_rdwr(UIO_READ, vp, (caddr_t)gShellCatBuf, TB710FU_SHELL_CAT_BYTES, 0,
                    UIO_SYSSPACE, IO_NODELOCKED, vfs_context_ucred(ctx), &resid,
                    kernproc);
    vnode_put(vp);
    if (error != 0 || resid < 0) {
        printf("TB710FU CAT: %s read -> %d\n", path, error);
        return error != 0 ? error : EIO;
    }
    const int bytes = (int)TB710FU_SHELL_CAT_BYTES - resid;
    char text[128];
    unsigned used = 0;
    printf("TB710FU CAT: begin %s\n", path);
    for (int i = 0; i < bytes; ++i) {
        char piece[4];
        unsigned count = 0;
        const char ch = gShellCatBuf[i];
        if (ch == '\n' || ch == '\t') {
            piece[count++] = ch;
        } else if (ch == '\\') {
            piece[count++] = '\\';
            piece[count++] = '\\';
        } else if (ch >= 0x20 && ch < 0x7f) {
            piece[count++] = ch;
        } else {
            piece[count++] = '\\';
            piece[count++] = 'x';
            piece[count++] = hex[((unsigned)ch >> 4) & 0xfu];
            piece[count++] = hex[(unsigned)ch & 0xfu];
        }
        if (used + count >= sizeof(text)) {
            text[used] = '\0';
            printf("%s", text);
            used = 0;
        }
        memcpy(text + used, piece, count);
        used += count;
    }
    if (used) {
        text[used] = '\0';
        printf("%s", text);
    }
    if (bytes > 0 && gShellCatBuf[bytes - 1] == '\n') {
        printf("\n");
    }
    printf("TB710FU CAT: %s bytes=%d resid=%d\n", path, bytes, (int)resid);
    return bytes;
}

/* size/mode/uid/gid/type without opening the file: VNOP_GETATTR is allowed to
 * come without a preceding open, which keeps this usable on directories and on
 * paths the caller may not write. */
int
tb710fu_shell_stat(const char *path)
{
    vnode_t vp = NULL;
    vfs_context_t ctx = NULL;
    int error = shell_lookup(path, &vp, &ctx);
    if (error) {
        return error;
    }
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_type);
    VATTR_WANTED(&va, va_mode);
    VATTR_WANTED(&va, va_uid);
    VATTR_WANTED(&va, va_gid);
    VATTR_WANTED(&va, va_data_size);
    VATTR_WANTED(&va, va_nlink);
    VATTR_WANTED(&va, va_fsid);
    VATTR_WANTED(&va, va_fileid);
    error = vnode_getattr(vp, &va, ctx);
    vnode_put(vp);
    if (error != 0) {
        printf("TB710FU STAT: %s getattr -> %d\n", path, (int)error);
        return error;
    }
    printf("TB710FU STAT: %s type=%s mode=%04o uid=%u gid=%u size=%llu "
           "nlink=%llu fsid=%08x fileid=%llu\n",
           path, shell_type(va.va_type), (unsigned)(va.va_mode & 07777u),
           (unsigned)va.va_uid, (unsigned)va.va_gid,
           (unsigned long long)va.va_data_size, (unsigned long long)va.va_nlink,
           (unsigned)va.va_fsid, (unsigned long long)va.va_fileid);
    return 0;
}
