// [dos2-nr01fix] NR-01 fix: host test of the guest read the engine object registry keys shaders with (dm/gfx/nr/nr_peek.h
// nr_pipe_read) and of the reader form of the shader pack key (xbg_shader_key.c xbg_shader_program_key_rd).
//   1. xbg_shader_program_key_rd == xbg_shader_program_key (the key the shader pack / xbg_game.cpp load_shader uses) for controls of
//      every own-length class and code sizes around the 4 KiB pieces, NULL code, ncode 0 and >= 16 MiB (the key then reads no code);
//      a reader that fails = -1 and an empty key; the reader is asked for exactly [code, code + ncode).
//   2. nr_pipe_read: plain, read-only and multi-page ranges read; a PROT_NONE page, an unmapped page and a range whose second page is
//      PROT_NONE are a 0 return with no signal (a SIGSEGV / SIGBUS handler fails the test); the pipe is empty and usable afterwards.
//   3. The phone's case: a dma-buf mapped MAP_SHARED | MAP_FIXED over an anonymous range (as xbg_game.cpp zc::map_chunk puts the AHB
//      of a zero-copy pool over guest memory), here a vgem dumb buffer exported through PRIME (/dev/dri/card0 in WSL2; skipped with a
//      note when there is none). Its VMA flags are printed from /proc/self/smaps; with VM_PFNMAP ("pf") process_vm_readv must refuse it
//      (EFAULT) - the NR-01 bug - and nr_pipe_read must read it byte-exact, and the pack key of code in it through the pipe must equal
//      the key of plain loads.
//   4. A VM_PFNMAP mapping that exists everywhere: the vDSO data pages [vvar] (WSL2's vgem maps dumb buffers without VM_PFNMAP, so 3.
//      shows no refusal there): process_vm_readv must refuse every page with EFAULT, nr_pipe_read must read the provided ones.
// Build + run (WSL / Linux):
//   gcc -std=gnu11 -O2 -Wall -Wextra nr_peek_test.c ../xbg_shader_key.c -o nr_peek_test && ./nr_peek_test
#define _GNU_SOURCE
#include "../../../nr/nr_peek.h"
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

int xbg_shader_program_key(const void *control, const void *code, char out[17]);
int xbg_shader_program_key_rd(const void *control, uint64_t code, int (*rd)(void *dst, uint64_t address, size_t n), char out[17]);

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static volatile sig_atomic_t g_signals;
static void on_signal(int s) { (void)s; g_signals++; _exit(3); }   // a fault in a probe = the test failed hard

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

// ---- readers ----
static uint64_t rd_lo, rd_hi, rd_bytes; static int rd_fail_at = -1, rd_calls;
static int rd_mem(void *dst, uint64_t a, size_t n) {
    if (rd_calls++ == rd_fail_at) return 0;
    if (a < rd_lo || a + n > rd_hi) { rd_lo = 1; return 0; }   // outside the code area: flagged by rd_lo = 1
    memcpy(dst, (const void *)(uintptr_t)a, n); rd_bytes += n; return 1;
}
static int g_pfd[2] = {-1, -1};
static int rd_pipe(void *dst, uint64_t a, size_t n) { return nr_pipe_read(g_pfd, dst, a, n) == 1; }
static int pipe_empty(void) { char c; return read(g_pfd[0], &c, 1) < 0 && errno == EAGAIN; }

static void make_control(uint8_t *ctl, uint32_t own, uint32_t ncode) {
    for (int i = 0; i < 0x880; i++) ctl[i] = (uint8_t)rnd();
    memcpy(ctl + 0x20, &own, 4); memcpy(ctl + 0x704, &ncode, 4);
}

static void test_key_rd(void) {
    static uint8_t ctl[0x880];
    const uint32_t owns[] = {0x838, 0x850, 0x760, 0x87f, 0x880, 0, 0xe48};
    const uint32_t sizes[] = {1, 3, 4095, 4096, 4097, 8192, 12289, 70000};
    uint8_t *code = malloc(70000);
    for (int i = 0; i < 70000; i++) code[i] = (uint8_t)rnd();
    for (unsigned o = 0; o < sizeof owns / sizeof *owns; o++)
        for (unsigned z = 0; z < sizeof sizes / sizeof *sizes; z++) {
            make_control(ctl, owns[o], sizes[z]);
            char a[17], b[17], c[17];
            CHECK(!xbg_shader_program_key(ctl, code, a), "key own %#x size %u", owns[o], sizes[z]);
            rd_lo = (uint64_t)(uintptr_t)code; rd_hi = rd_lo + sizes[z]; rd_bytes = 0; rd_calls = 0; rd_fail_at = -1;
            CHECK(!xbg_shader_program_key_rd(ctl, (uint64_t)(uintptr_t)code, rd_mem, b) && !strcmp(a, b), "rd key own %#x size %u: '%s' vs '%s'", owns[o], sizes[z], b, a);
            CHECK(rd_lo != 1 && rd_bytes == sizes[z] && rd_calls == (int)((sizes[z] + 4095) / 4096), "reader range own %#x size %u: %llu bytes in %d calls",
                  owns[o], sizes[z], (unsigned long long)rd_bytes, rd_calls);
            CHECK(!xbg_shader_program_key_rd(ctl, (uint64_t)(uintptr_t)code, rd_pipe, c) && !strcmp(a, c) && pipe_empty(), "pipe key own %#x size %u", owns[o], sizes[z]);
            for (int f = 0; f < rd_calls; f++) {   // every piece failing in turn: no key
                rd_lo = (uint64_t)(uintptr_t)code; rd_calls = 0; rd_fail_at = f; strcpy(b, "x");
                CHECK(xbg_shader_program_key_rd(ctl, (uint64_t)(uintptr_t)code, rd_mem, b) == -1 && !b[0], "failing piece %d of size %u", f, sizes[z]);
            }
        }
    // no code read at all: NULL code, ncode 0, ncode >= 16 MiB (the key is the control's alone, like xbg_shader_program_key)
    const uint32_t none[] = {0, 1u << 24, 0xffffffffu};
    for (unsigned i = 0; i < 3; i++) {
        make_control(ctl, 0x850, none[i]);
        char a[17], b[17];
        xbg_shader_program_key(ctl, code, a); rd_calls = 0; rd_fail_at = 0;   // the first read would fail
        CHECK(!xbg_shader_program_key_rd(ctl, (uint64_t)(uintptr_t)code, rd_mem, b) && !strcmp(a, b) && rd_calls == 0, "ncode %#x reads no code", none[i]);
    }
    make_control(ctl, 0x850, 100);
    { char a[17], b[17]; xbg_shader_program_key(ctl, NULL, a); rd_calls = 0; rd_fail_at = 0;
      CHECK(!xbg_shader_program_key_rd(ctl, 0, rd_mem, b) && !strcmp(a, b) && rd_calls == 0, "NULL code reads no code"); }
    { char b[17] = "x"; CHECK(xbg_shader_program_key_rd(NULL, 1, rd_mem, b) == -1 && !b[0], "no control"); }
    { char b[17] = "x"; CHECK(xbg_shader_program_key_rd(ctl, 1, NULL, b) == -1 && !b[0], "no reader"); }
    free(code);
}

static void test_pipe_read(void) {
    const long pg = sysconf(_SC_PAGESIZE);
    uint8_t *m = mmap(NULL, 6 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED, "mmap");
    for (long i = 0; i < 6 * pg; i++) m[i] = (uint8_t)(i * 7 + 3);
    uint8_t *buf = malloc(6 * pg);
    // pages: 0-2 rw, 3 read-only, 4 PROT_NONE, 5 unmapped
    mprotect(m + 3 * pg, pg, PROT_READ); mprotect(m + 4 * pg, pg, PROT_NONE); munmap(m + 5 * pg, pg);
    const uint64_t a = (uint64_t)(uintptr_t)m;
    memset(buf, 0, 6 * pg);
    CHECK(nr_pipe_read(g_pfd, buf, a + 5, 3 * pg + 100) == 1 && !memcmp(buf, m + 5, 3 * pg + 100) && pipe_empty(), "rw + read-only range");
    CHECK(nr_pipe_read(g_pfd, buf, a + 3 * pg, pg) == 1 && !memcmp(buf, m + 3 * pg, pg), "read-only page");
    CHECK(nr_pipe_read(g_pfd, buf, a + 4 * pg, 16) == 0 && pipe_empty(), "PROT_NONE page: 0");
    CHECK(nr_pipe_read(g_pfd, buf, a + 5 * pg, 16) == 0 && pipe_empty(), "unmapped page: 0");
    CHECK(nr_pipe_read(g_pfd, buf, a + 3 * pg + 10, pg) == 0 && pipe_empty(), "read-only then PROT_NONE: 0");
    CHECK(nr_pipe_read(g_pfd, buf, a + 2 * pg, 3 * pg) == 0 && pipe_empty(), "a range ending in an unmapped page: 0");
    CHECK(nr_pipe_read(g_pfd, buf, 8, 8) == 0 && pipe_empty(), "address 8: 0");
    memset(buf, 0, 64);
    CHECK(nr_pipe_read(g_pfd, buf, a + 1, 64) == 1 && !memcmp(buf, m + 1, 64), "the pipe still works after the refusals");
    CHECK(nr_pipe_read(g_pfd, buf, a, 0) == 1, "0 bytes");
    // process_vm_readv on the same plain pages reads them too (the old route, for the record)
    struct iovec l = {buf, 64}, r = {m + 1, 64};
    CHECK(syscall(SYS_process_vm_readv, getpid(), &l, 1UL, &r, 1UL, 0UL) == 64, "process_vm_readv of anonymous pages");
    CHECK(g_signals == 0, "no signal");
    munmap(m, 5 * pg); free(buf);
}

// ---- vgem dma-buf (the zero-copy pool's AHB in miniature) ----
struct drm_mode_create_dumb_ { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct drm_prime_handle_ { uint32_t handle, flags; int32_t fd; };
#define DRM_IOCTL_MODE_CREATE_DUMB_ _IOWR('d', 0xB2, struct drm_mode_create_dumb_)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD_ _IOWR('d', 0x2d, struct drm_prime_handle_)
static void vma_flags(uint64_t a, char *out, size_t cap) {   // VmFlags of the VMA holding a (/proc/self/smaps)
    out[0] = 0; FILE *f = fopen("/proc/self/smaps", "r"); if (!f) return;
    char line[256]; int in = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        if (sscanf(line, "%lx-%lx ", &lo, &hi) == 2 && strchr(line, '-') == line + strspn(line, "0123456789abcdef")) { in = a >= lo && a < hi; continue; }
        if (in && !strncmp(line, "VmFlags:", 8)) { snprintf(out, cap, "%s", line + 9); out[strcspn(out, "\n")] = 0; break; }
    }
    fclose(f);
}
static void test_dmabuf(void) {
    int card = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (card < 0) { printf("note: no /dev/dri/card0 (%s): dma-buf case skipped\n", strerror(errno)); return; }
    struct drm_mode_create_dumb_ cd = {.height = 64, .width = 1024, .bpp = 32};   // 256 KiB
    if (ioctl(card, DRM_IOCTL_MODE_CREATE_DUMB_, &cd)) { printf("note: dumb buffers unavailable (%s): dma-buf case skipped\n", strerror(errno)); close(card); return; }
    struct drm_prime_handle_ ph = {.handle = cd.handle, .flags = O_CLOEXEC | O_RDWR, .fd = -1};
    if (ioctl(card, DRM_IOCTL_PRIME_HANDLE_TO_FD_, &ph)) { printf("note: PRIME export unavailable (%s): dma-buf case skipped\n", strerror(errno)); close(card); return; }
    const size_t n = (size_t)cd.size;
    // as zc::map_chunk: guest memory first, its contents copied into the dma-buf, the dma-buf MAP_FIXED over the guest range
    uint8_t *guest = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (size_t i = 0; i < n; i++) guest[i] = (uint8_t)rnd();
    uint8_t *tmp = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, ph.fd, 0);
    if (tmp == MAP_FAILED) { printf("note: dma-buf mmap failed (%s): dma-buf case skipped\n", strerror(errno)); close(ph.fd); close(card); return; }
    memcpy(tmp, guest, n);
    uint8_t *copy = malloc(n); memcpy(copy, guest, n);
    void *fx = mmap(guest, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, ph.fd, 0);
    munmap(tmp, n);
    CHECK(fx == guest, "MAP_FIXED of the dma-buf over the guest range");
    if (fx != guest) { close(ph.fd); close(card); free(copy); return; }
    char fl[512]; vma_flags((uint64_t)(uintptr_t)guest, fl, sizeof fl);
    const int pfn = strstr(fl, "pf") != NULL;
    uint8_t *buf = calloc(1, n);
    struct iovec l = {buf, 4096}, r = {guest + 4096, 4096};
    errno = 0; const long got = syscall(SYS_process_vm_readv, getpid(), &l, 1UL, &r, 1UL, 0UL); const int err = errno;
    printf("vgem dma-buf over guest memory: %zu bytes, VmFlags [%s] (VM_PFNMAP %s); process_vm_readv -> %ld errno %d (%s); plain loads read %s\n",
           n, fl, pfn ? "yes" : "no", got, err, got < 0 ? strerror(err) : "ok", !memcmp(guest, copy, n) ? "the contents" : "OTHER BYTES");
    if (pfn) CHECK(got == -1 && err == EFAULT, "process_vm_readv refuses a VM_PFNMAP dma-buf mapping with EFAULT (the NR-01 bug)");
    memset(buf, 0, n);
    CHECK(nr_pipe_read(g_pfd, buf, (uint64_t)(uintptr_t)guest, n) == 1 && !memcmp(buf, copy, n) && pipe_empty(), "nr_pipe_read reads the dma-buf mapping byte-exact");
    // a shader stage whose code lies in the mapping: the pack key through the pipe == the key of plain loads (the translator's)
    static uint8_t ctl[0x880]; make_control(ctl, 0x850, 50000);
    const uint8_t *code = guest + 4096 + 12;
    char a[17], b[17];
    xbg_shader_program_key(ctl, code, a);
    CHECK(!xbg_shader_program_key_rd(ctl, (uint64_t)(uintptr_t)code, rd_pipe, b) && !strcmp(a, b), "pack key of code in the dma-buf: pipe '%s' vs loads '%s'", b, a);
    printf("pack key of 50000 code bytes in the dma-buf: %s (pipe) = %s (loads)\n", b, a);
    CHECK(g_signals == 0, "no signal");
    munmap(guest, n); close(ph.fd); close(card); free(copy); free(buf);
}

// A VM_IO | VM_PFNMAP mapping every Linux process has: the vDSO's data pages [vvar] (inserted with vmf_insert_pfn; the vDSO reads
// them with plain loads). process_vm_readv refuses it like the phone's dma-buf pages; nr_pipe_read reads each page the vDSO can read
// and returns 0 (no signal) for a page the kernel does not provide (its fault handler says SIGBUS, copy_from_user turns that into EFAULT).
static void test_pfnmap_vvar(void) {
    FILE *f = fopen("/proc/self/maps", "r"); char line[256]; unsigned long lo = 0, hi = 0;
    while (f && fgets(line, sizeof line, f)) if (strstr(line, "[vvar]")) { sscanf(line, "%lx-%lx", &lo, &hi); break; }
    if (f) fclose(f);
    if (!lo) { printf("note: no [vvar] mapping: VM_PFNMAP case skipped\n"); return; }
    char fl[512]; vma_flags(lo, fl, sizeof fl);
    const int pfn = strstr(fl, "pf") != NULL;
    const long pg = sysconf(_SC_PAGESIZE);
    int refused = 0, piped = 0, pages = 0;
    uint8_t *buf = malloc(pg);
    for (unsigned long a = lo; a < hi; a += pg, pages++) {
        struct iovec l = {buf, (size_t)pg}, r = {(void *)a, (size_t)pg};
        errno = 0; const long got = syscall(SYS_process_vm_readv, getpid(), &l, 1UL, &r, 1UL, 0UL);
        if (got < 0 && errno == EFAULT) refused++;
        const int p = nr_pipe_read(g_pfd, buf, a, pg);
        CHECK(p >= 0 && pipe_empty(), "[vvar] page %d: the pipe stays usable", pages);
        piped += p == 1;
    }
    printf("[vvar] %#lx-%#lx: VmFlags [%s] (VM_PFNMAP %s), %d pages: process_vm_readv refused %d (EFAULT), nr_pipe_read read %d\n",
           lo, hi, fl, pfn ? "yes" : "no", pages, refused, piped);
    if (pfn) {
        CHECK(refused == pages, "process_vm_readv refuses every page of a VM_PFNMAP mapping (%d of %d)", refused, pages);
        CHECK(piped >= 1, "nr_pipe_read reads the provided pages of a VM_PFNMAP mapping (%d of %d)", piped, pages);
    }
    CHECK(g_signals == 0, "no signal");
    free(buf);
}

int main(void) {
    signal(SIGSEGV, on_signal); signal(SIGBUS, on_signal);
    CHECK(nr_pipe_open(g_pfd) == 1 && pipe_empty(), "pipe open, non-blocking");
    test_key_rd();
    test_pipe_read();
    test_dmabuf();
    test_pfnmap_vvar();
    if (failures) { printf("FAIL: %d of %d checks\n", failures, checks); return 1; }
    printf("PASS: %d checks, 0 failed\n", checks);
    return 0;
}
