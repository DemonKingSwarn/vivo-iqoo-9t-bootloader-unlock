/*
 * rootcmd.c — trigger the armed kernel patch and exec a command as uid 0.
 *
 * The kernel patch (see tools/host/rootctl.py) turns __do_sys_capset into
 * commit_creds(&init_cred).  A plain capset(NULL, NULL) therefore installs
 * init_cred on the calling process.  Nothing is spawned until after the
 * credential swap, which is why this is an in-process trigger.
 *
 * Usage:
 *   rootcmd                     -> print identity, exit 0 if root
 *   rootcmd <cmd> [args...]     -> exec cmd as root
 *
 * Environment:
 *   ROOT_DOMAIN=u:r:shell:s0    -> switch the process to the shell SELinux
 *                                  domain before exec.  Required for binder
 *                                  clients (pm, am, cmd, ...) when running as
 *                                  kernel-domain root; uid stays 0.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv) {
    errno = 0;
    long r = syscall(SYS_capset, (void *)0, (void *)0);
    if (r != 0) {
        fprintf(stderr, "rootcmd: capset failed: %s\n", strerror(errno));
    }

    if (geteuid() != 0) {
        fprintf(stderr, "rootcmd: not root (uid=%d) — is the patch armed?\n", getuid());
        return 1;
    }

    if (argc < 2) {
        printf("uid=%d euid=%d gid=%d  *** ROOT ***\n", getuid(), geteuid(), getgid());
        return 0;
    }

    const char *domain = getenv("ROOT_DOMAIN");
    if (domain && *domain) {
        int fd = open("/proc/self/attr/current", O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            /* SELinux expects a NUL-terminated context. */
            if (write(fd, domain, strlen(domain) + 1) < 0)
                fprintf(stderr, "rootcmd: set domain %s: %s\n", domain, strerror(errno));
            close(fd);
        } else {
            fprintf(stderr, "rootcmd: open attr/current: %s\n", strerror(errno));
        }
    }

    execvp(argv[1], &argv[1]);
    fprintf(stderr, "rootcmd: exec %s: %s\n", argv[1], strerror(errno));
    return 127;
}
