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
#include <pexpert/pexpert.h>

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
    return error;
}

extern task_t proc_task(proc_t);
extern int get_task_numacts(task_t);
extern uint64_t get_task_phys_footprint(task_t);
extern volatile uint32_t native_aes_kat_status __attribute__((weak_import));
extern volatile uint32_t native_aes_registered __attribute__((weak_import));
extern volatile uint32_t native_aes_completed __attribute__((weak_import));
extern volatile uint32_t native_aes_nonce_key_uses __attribute__((weak_import));

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
