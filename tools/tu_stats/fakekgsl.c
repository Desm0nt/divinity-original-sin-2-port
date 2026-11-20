// N1 (docs/dos2/nr/NR-NSHADER.md): a fake /dev/kgsl-3d0 for running the host (x86_64 Linux) build of Turnip with NO GPU, only to compile
// pipelines and read the Adreno compiler's statistics (VK_KHR_pipeline_executable_properties). LD_PRELOAD this next to tu_stats.
// Nothing is ever executed: allocations are anonymous host memory, submissions are accepted and "retire" at once.
// Chip: FAKEKGSL_CHIP (hex, default 0x43051401 = FD750, the owner's Mi 14 Ultra Adreno 750); FAKEKGSL_GMEM bytes (default 3 MB).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/types.h>
#define __user
#include "msm_kgsl.h"

static int fake_fd = -1;
static unsigned next_id = 1;
static uint64_t next_iova = 0x100000000ull;
static unsigned ctx_ts = 0;

static int is_kgsl(const char *p){return p && (!strcmp(p, "/dev/kgsl-3d0"));}
static int (*real_open)(const char *, int, ...);
static int (*real_ioctl)(int, unsigned long, ...);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);

static void init_real(void){
    if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
    if (!real_ioctl) real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    if (!real_mmap) real_mmap = dlsym(RTLD_NEXT, "mmap");
}

static int open_fake(void){
    init_real();
    if (fake_fd < 0) fake_fd = real_open("/dev/null", O_RDWR);
    return fake_fd < 0 ? -1 : dup(fake_fd);
}
static int fds[64]; static int nfds;
static int is_fake(int fd){for (int i = 0; i < nfds; i++) if (fds[i] == fd) return 1; return 0;}

int open(const char *p, int fl, ...){
    init_real();
    if (is_kgsl(p)) {int fd = open_fake(); if (fd >= 0 && nfds < 64) fds[nfds++] = fd; return fd;}
    if (p && (!strcmp(p, "/dev/dma_heap/system") || !strcmp(p, "/dev/ion"))) {errno = ENOENT; return -1;}
    va_list a; va_start(a, fl); int m = (fl & O_CREAT) ? va_arg(a, int) : 0; va_end(a);
    return real_open(p, fl, m);
}
int open64(const char *p, int fl, ...){va_list a; va_start(a, fl); int m = (fl & O_CREAT) ? va_arg(a, int) : 0; va_end(a); return open(p, fl, m);}
int __open_2(const char *p, int fl){return open(p, fl);}
int __open64_2(const char *p, int fl){return open(p, fl);}

static int prop(struct kgsl_device_getproperty *g){
    switch (g->type) {
    case KGSL_PROP_DEVICE_INFO: {
        struct kgsl_devinfo d; memset(&d, 0, sizeof d);
        const char *c = getenv("FAKEKGSL_CHIP"); d.chip_id = c ? (unsigned)strtoul(c, 0, 16) : 0x43051401u;
        const char *gm = getenv("FAKEKGSL_GMEM"); d.gmem_sizebytes = gm ? strtoul(gm, 0, 0) : 3u * 1024 * 1024;
        d.gpu_id = 750; d.mmu_enabled = 1;
        memcpy(g->value, &d, g->sizebytes < sizeof d ? g->sizebytes : sizeof d); return 0;}
    case KGSL_PROP_UCHE_GMEM_VADDR: {uint64_t v = 0x100000; memcpy(g->value, &v, 8); return 0;}
    case KGSL_PROP_HIGHEST_BANK_BIT: {uint32_t v = 16; memcpy(g->value, &v, 4); return 0;}
    case KGSL_PROP_UBWC_MODE: {uint32_t v = KGSL_UBWC_4_0; memcpy(g->value, &v, 4); return 0;}
    case KGSL_PROP_GPU_VA64_SIZE: {uint64_t v = 1ull << 40; memcpy(g->value, &v, g->sizebytes < 8 ? g->sizebytes : 8); return 0;}
    default: errno = EINVAL; return -1;
    }
}

int ioctl(int fd, unsigned long req, ...){
    va_list a; va_start(a, req); void *arg = va_arg(a, void *); va_end(a);
    init_real();
    if (!is_fake(fd)) return real_ioctl(fd, req, arg);
    switch (req) {
    case IOCTL_KGSL_DEVICE_GETPROPERTY: return prop(arg);
    case IOCTL_KGSL_GPUMEM_ALLOC_ID: {
        struct kgsl_gpumem_alloc_id *r = arg; r->id = next_id++; r->mmapsize = (r->size + 4095) & ~(size_t)4095;
        r->gpuaddr = next_iova; next_iova += (r->mmapsize + 0xffff) & ~0xffffull; return 0;}
    case IOCTL_KGSL_GPUMEM_GET_INFO: {struct kgsl_gpumem_get_info *r = arg; r->gpuaddr = 0x200000000ull + ((uint64_t)r->id << 24); return 0;}
    case IOCTL_KGSL_GPUMEM_FREE_ID: case IOCTL_KGSL_GPUOBJ_FREE: case IOCTL_KGSL_DRAWCTXT_DESTROY: return 0;
    case IOCTL_KGSL_GPUOBJ_ALLOC: errno = EINVAL; return -1;       // no virtual BOs -> no sparse
    case IOCTL_KGSL_DRAWCTXT_CREATE: {struct kgsl_drawctxt_create *r = arg; r->drawctxt_id = 1; return 0;}
    case IOCTL_KGSL_GPU_COMMAND: {struct kgsl_gpu_command *r = arg; r->timestamp = ++ctx_ts; return 0;}
    case IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID: {struct kgsl_cmdstream_readtimestamp_ctxtid *r = arg; r->timestamp = ctx_ts; return 0;}
    case IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID: return 0;
    default:
        if (getenv("FAKEKGSL_LOG")) fprintf(stderr, "fakekgsl: ioctl 0x%lx refused\n", req);
        errno = EINVAL; return -1;
    }
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off){
    init_real();
    if (fd >= 0 && is_fake(fd)) return real_mmap(addr, len, prot, (flags & MAP_FIXED) | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return real_mmap(addr, len, prot, flags, fd, off);
}
void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off){return mmap(addr, len, prot, flags, fd, off);}
