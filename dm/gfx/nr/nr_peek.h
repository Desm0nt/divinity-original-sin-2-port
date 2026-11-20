// [DOS2] NR-01 fix (docs/dos2/nr/NR-01-fix.md): a read of this process's own memory that never faults and, unlike process_vm_readv,
// also reads VM_IO | VM_PFNMAP mappings.
// process_vm_readv pins the source pages (get_user_pages / pin_user_pages_remote), and the kernel refuses VM_IO | VM_PFNMAP VMAs there
// with EFAULT (mm/gup.c check_vma_flags). The zero-copy pools (xbg_game.cpp zc::map_chunk, DM_GFX_ZEROCOPY: default 1 on Adreno) mmap
// the dma-buf of an AHardwareBuffer MAP_SHARED | MAP_FIXED over the guest pool, and the dma-buf heaps map their pages with
// remap_pfn_range (= VM_IO | VM_PFNMAP), so every shader code area in such a pool read as "unreadable" through process_vm_readv.
// write() into a pipe reads its source with copy_from_user: ordinary loads with an exception-table fixup, so a mapped page reads
// whatever its VMA flags, and an unmapped / PROT_NONE page is an EFAULT return, never a signal (no signal handler of ours or the
// game's is entered). Linux / Android only; header-only so the host test (tests/nr_peek_test.c) runs the same code.
#pragma once
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

// A pipe for nr_pipe_read: both ends non-blocking and close-on-exec (pipe + fcntl: bionic declares pipe2 only under _GNU_SOURCE).
// 1 = open; 0 = no pipe (pfd = -1, -1).
static inline int nr_pipe_open(int pfd[2]) {
    if (pipe(pfd)) { pfd[0] = pfd[1] = -1; return 0; }
    for (int i = 0; i < 2; i++)
        if (fcntl(pfd[i], F_SETFL, fcntl(pfd[i], F_GETFL) | O_NONBLOCK) || fcntl(pfd[i], F_SETFD, FD_CLOEXEC)) {
            close(pfd[0]); close(pfd[1]); pfd[0] = pfd[1] = -1; return 0;
        }
    return 1;
}
// pfd = a pipe of nr_pipe_open, empty on entry and empty again on return; the caller serialises its use.
// Reads n bytes at addr into dst in writes of at most 4 KiB (PIPE_BUF: an empty pipe always takes them whole).
// 1 = all read; 0 = a source page is not readable (dst partly written); -1 = the pipe itself failed (it may hold bytes: reopen it).
static inline int nr_pipe_read(const int pfd[2], void *dst, uint64_t addr, size_t n) {
    size_t got = 0;
    while (got < n) {
        const size_t k = n - got < 4096 ? n - got : 4096;
        ssize_t w;
        do w = write(pfd[1], (const void *)(uintptr_t)(addr + got), k); while (w < 0 && errno == EINTR);
        if (w < 0) return errno == EFAULT ? 0 : -1;
        if (w == 0) return -1;
        for (ssize_t r = 0; r < w;) {   // everything written comes back out before the next write: the pipe never fills
            const ssize_t m = read(pfd[0], (uint8_t *)dst + got + r, (size_t)(w - r));
            if (m < 0 && errno == EINTR) continue;
            if (m <= 0) return -1;
            r += m;
        }
        got += (size_t)w;   // a short write: the page after it faulted, the next write says EFAULT
    }
    return 1;
}
