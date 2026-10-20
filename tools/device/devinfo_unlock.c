/*
 * devinfo_unlock.c — flip the ABL's trusted unlock flags in the devinfo
 * partition from inside a temporary-root process.
 *
 * Background (see docs/findings.md §15):
 *   ABL is_device_unlocked()          = devinfo[0x0d] != 0
 *   ABL is_device_critical_unlocked() = devinfo[0x0e] != 0
 * and the valid-magic path performs no CRC/signature check.
 *
 * Requirements:
 *   - the __do_sys_capset patch is armed (tools/host/rootctl.py arm)
 *   - SELinux is Permissive (rootctl arm does this); kernel-domain root cannot
 *     write block devices or selinuxfs while Enforcing
 *
 * Usage:
 *   devinfo_unlock status      read and print the flags (no write)
 *   devinfo_unlock unlock      set is_unlocked=1, is_unlock_critical=1
 *   devinfo_unlock revert      set both back to 0
 *   devinfo_unlock --dry-run   same as status
 *
 * On `unlock` the original 4096 bytes are saved to
 *   /data/local/tmp/devinfo_before.img   (only if it does not already exist)
 *
 * NOTE: after a successful unlock the device expects a userdata wipe.  Booting
 * with modified devinfo but un-wiped userdata lands in vivo's "device is
 * corrupt" repair menu; choose "Clear all data" there (not "Repair system").
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define DEVINFO_SIZE 4096
#define OFF_UNLOCKED 0x0d
#define OFF_CRITICAL 0x0e
#define OFF_CHARGER 0x0f
#define MAGIC "ANDROID-BOOT!"
#define MAGIC_LEN 13

static const char *candidates[] = {
    "/dev/block/by-name/devinfo",
    "/dev/block/sde28",
    NULL,
};

static const char *find_devinfo(void) {
    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], F_OK) == 0)
            return candidates[i];
    }
    return NULL;
}

static int read_full(int fd, void *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, (char *)buf + done, len - done, (off_t)done);
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static int write_byte(int fd, off_t off, unsigned char v) {
    ssize_t n = pwrite(fd, &v, 1, off);
    if (n != 1)
        return -1;
    return 0;
}

static void print_flags(const unsigned char *b) {
    printf("magic        : %.13s\n", b);
    printf("is_unlocked  : 0x%02x  (0x0d)\n", b[OFF_UNLOCKED]);
    printf("is_critical  : 0x%02x  (0x0e)\n", b[OFF_CRITICAL]);
    printf("charger_scr  : 0x%02x  (0x0f)\n", b[OFF_CHARGER]);
    printf("byte 0x90    : 0x%02x\n", b[0x90]);
    printf("first 32     :");
    for (int i = 0; i < 32; i++)
        printf(" %02x", b[i]);
    printf("\n");
}

int main(int argc, char **argv) {
    int mode = 0; /* 0=status 1=unlock 2=revert */
    if (argc > 1) {
        if (!strcmp(argv[1], "unlock"))
            mode = 1;
        else if (!strcmp(argv[1], "revert"))
            mode = 2;
        else if (strcmp(argv[1], "status") && strcmp(argv[1], "--dry-run")) {
            fprintf(stderr, "usage: %s [status|unlock|revert]\n", argv[0]);
            return 2;
        }
    }

    /* Become uid 0 via the armed capset patch.  In-process only. */
    errno = 0;
    long r = syscall(SYS_capset, (void *)0, (void *)0);
    if (geteuid() != 0) {
        fprintf(stderr,
                "devinfo_unlock: capset -> %ld, uid=%d; patch not armed?\n",
                r, getuid());
        return 1;
    }

    /* Refuse early under Enforcing: kernel-domain root cannot touch the block
     * device and the process is likely to be killed. */
    {
        char enf[8] = {0};
        int fd = open("/sys/fs/selinux/enforce", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            if (read(fd, enf, sizeof(enf) - 1) <= 0)
                enf[0] = '?';
            close(fd);
            if (enf[0] == '1') {
                fprintf(stderr,
                        "devinfo_unlock: SELinux is Enforcing; run "
                        "`tools/host/selinux.py off` first\n");
                return 1;
            }
        }
    }

    const char *path = find_devinfo();
    if (!path) {
        fprintf(stderr, "devinfo_unlock: no devinfo block device found\n");
        return 1;
    }
    printf("devinfo      : %s\n", path);

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "devinfo_unlock: open %s: %s\n", path, strerror(errno));
        return 1;
    }

    unsigned char buf[DEVINFO_SIZE];
    if (read_full(fd, buf, sizeof(buf)) != 0) {
        fprintf(stderr, "devinfo_unlock: read: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    if (memcmp(buf, MAGIC, MAGIC_LEN) != 0) {
        fprintf(stderr, "devinfo_unlock: magic mismatch, refusing to write\n");
        print_flags(buf);
        close(fd);
        return 1;
    }

    print_flags(buf);

    if (mode == 0) {
        close(fd);
        return 0;
    }

    if (mode == 1) {
        /* Keep a pristine copy on-device the first time. */
        const char *backup = "/data/local/tmp/devinfo_before.img";
        if (access(backup, F_OK) != 0) {
            int bf = open(backup, O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (bf >= 0) {
                if (write(bf, buf, sizeof(buf)) == (ssize_t)sizeof(buf))
                    printf("backup       : %s\n", backup);
                else
                    fprintf(stderr, "devinfo_unlock: backup write failed\n");
                close(bf);
            } else {
                fprintf(stderr, "devinfo_unlock: backup open: %s\n", strerror(errno));
            }
        } else {
            printf("backup       : %s (already exists)\n", backup);
        }
    }

    unsigned char want_unlocked = (mode == 1) ? 1 : 0;
    unsigned char want_critical = (mode == 1) ? 1 : 0;

    if (write_byte(fd, OFF_UNLOCKED, want_unlocked) != 0 ||
        write_byte(fd, OFF_CRITICAL, want_critical) != 0) {
        fprintf(stderr, "devinfo_unlock: write: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    fsync(fd);
    close(fd);

    /* Verify by re-reading the partition. */
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "devinfo_unlock: reopen: %s\n", strerror(errno));
        return 1;
    }
    memset(buf, 0, sizeof(buf));
    if (read_full(fd, buf, sizeof(buf)) != 0) {
        fprintf(stderr, "devinfo_unlock: verify read: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);

    printf("--- after ---\n");
    print_flags(buf);

    if (buf[OFF_UNLOCKED] != want_unlocked ||
        buf[OFF_CRITICAL] != want_critical) {
        fprintf(stderr, "devinfo_unlock: verify FAILED\n");
        return 1;
    }

    printf("devinfo_unlock: %s OK\n", mode == 1 ? "unlock" : "revert");
    return 0;
}
