// [DOS2] NR-18: host test of xbg_ac_read_staged (xbg_astc_cache.h, production code; astccache::upload in xbg_game.cpp calls it).
//   1. Same bytes as the old upload() (pread loop straight into the map, zero-fill of a short read, xbg_ac_sanitize of the whole entry,
//      copied below as `old_way`) for the bounce path (DM_NR_ASTCREAD=1, 256 KB and odd chunk sizes) and the in-place path (=0):
//      sizes 0 / 15 / 16 / 17 / around the chunk / 3 MB, random blocks with all-zero blocks spread in, readers that return short
//      pieces, fail with EIO or hit the end of the file in the middle; bytes read, errno and the number of fixed blocks compared too.
//   2. Timing (the reason for NR-18): 33.9 MB (TL2's inventory case) into a write-combined map, in-place vs bounce. On Windows the map
//      is VirtualAlloc(PAGE_READWRITE | PAGE_WRITECOMBINE), what the stream arena's memory type 0x7 is on the phones; elsewhere plain
//      memory (the numbers then only show the bounce path's extra copy on cached memory). The "file" is memory (page cache, as pread of
//      a warm pack): the difference is the readback of the map, not storage.
// Build (Windows clang 21; vulkan.h of the NDK sysroot after the system headers), from this folder:
//   clang++ -std=c++17 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -idirafter <ndk>/toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/include astc_read_test.cpp -o astc_read_test.exe
// (Linux / WSL: g++ -std=c++17 -O2 -Wall -Wextra astc_read_test.cpp -o astc_read_test, with the Vulkan headers installed.)
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <vector>
#include "../xbg_astc_cache.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static int fails = 0, checks = 0;
#define CHECK(e, ...) do { ++checks; if (!(e)) { ++fails; if (fails <= 20) { printf("FAIL line %d: %s: ", __LINE__, #e); printf(__VA_ARGS__); printf("\n"); } } } while (0)

// A "file": the entry's bytes; reads return at most `piece` bytes, fail (-1, EIO) at `fail_at` or end (0) at `eof_at`.
struct File {
    const std::vector<uint8_t> *d; size_t piece; uint64_t fail_at, eof_at;
    int64_t operator()(void *p, size_t n, uint64_t off) const {
        if (off >= fail_at) { errno = EIO; return -1; }
        if (off >= eof_at || off >= d->size()) return 0;
        uint64_t lim = d->size(); if (eof_at < lim) lim = eof_at; if (fail_at < lim) lim = fail_at;
        size_t k = n; if (k > piece) k = piece; if (k > lim - off) k = size_t(lim - off);
        memcpy(p, d->data() + off, k); return int64_t(k);
    }
};
// The old astccache::upload read (as of 94d287d), with got / errno returned instead of logged.
static XbgAcRead old_way(const File &f, uint64_t bytes, uint8_t *dst) {
    uint64_t got = 0; int err = 0;
    while (got < bytes) { const int64_t r = f(dst + got, size_t(bytes - got), got); if (r <= 0) { err = r < 0 ? errno : 0; break; } got += uint64_t(r); }
    if (got < bytes) memset(dst + got, 0, size_t(bytes - got));
    XbgAcRead r{got, xbg_ac_sanitize(dst, bytes), err}; return r;
}
static uint64_t rng = 0x243F6A8885A308D3ull;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return uint32_t(rng >> 11); }
static std::vector<uint8_t> blocks(size_t n) {
    std::vector<uint8_t> v(n); for (auto &b : v) b = uint8_t(rnd());
    for (size_t o = 0; o + 16 <= n; o += 16) if (rnd() % 7 == 0) memset(&v[o], 0, 16);   // all-zero blocks (the error block)
    return v;
}
static double ms_since(std::chrono::steady_clock::time_point t) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); }

int main() {
    // 1. same bytes as the old read
    const size_t K = XBG_AC_READ_CHUNK;
    const size_t sizes[] = {0, 15, 16, 17, 48, 1000, K - 16, K - 1, K, K + 1, K + 16, 2 * K + 48, 3u << 20 | 48, (3u << 20) + 7};
    const size_t chunks[] = {K, 16, 48, 1000 /* -> 992 */, 7 /* -> 16 */, 64u << 10};
    std::vector<uint8_t> bounce(K > 1000 ? K : 1000);
    unsigned cases = 0;
    for (size_t n : sizes) {
        const std::vector<uint8_t> d = blocks(n);
        const uint64_t cut[] = {UINT64_MAX, n / 2, n ? n - 1 : 0, 0, (n / 3) & ~uint64_t(15), K + 5};
        const size_t pieces[] = {SIZE_MAX, 4096, 13};
        for (uint64_t c : cut) for (int kind = 0; kind < 2; kind++) for (size_t piece : pieces) {
            File f{&d, piece, kind ? c : UINT64_MAX, kind ? UINT64_MAX : c};
            std::vector<uint8_t> ref(n + 32, 0xAB), a(n + 32, 0xCD), b(n + 32, 0xEF);
            const XbgAcRead r0 = old_way(f, n, ref.data());
            const XbgAcRead r1 = xbg_ac_read_staged(f, n, a.data(), nullptr, 0);   // DM_NR_ASTCREAD=0
            CHECK(!memcmp(ref.data(), a.data(), n) && r0.got == r1.got && r0.zero_fixed == r1.zero_fixed && r0.err == r1.err,
                  "in place n=%zu cut=%llu kind=%d piece=%zu", n, (unsigned long long)c, kind, piece);
            CHECK(a[n] == 0xCD, "in place wrote past the entry n=%zu", n);
            for (size_t ch : chunks) {
                memset(b.data(), 0xEF, b.size());
                const XbgAcRead r2 = xbg_ac_read_staged(f, n, b.data(), bounce.data(), ch);   // DM_NR_ASTCREAD=1
                CHECK(!memcmp(ref.data(), b.data(), n) && r0.got == r2.got && r0.zero_fixed == r2.zero_fixed && r0.err == r2.err,
                      "bounce n=%zu chunk=%zu cut=%llu kind=%d piece=%zu: got %llu/%llu fixed %u/%u err %d/%d", n, ch, (unsigned long long)c, kind, piece,
                      (unsigned long long)r2.got, (unsigned long long)r0.got, r2.zero_fixed, r0.zero_fixed, r2.err, r0.err);
                CHECK(b[n] == 0xEF, "bounce wrote past the entry n=%zu chunk=%zu", n, ch);
                ++cases;
            }
            if (kind && c < n) CHECK(r0.err == EIO && r0.got == c, "the failing reader n=%zu cut=%llu", n, (unsigned long long)c);
        }
    }
    {   // a lost tail is transparent black: every block after the cut is the void extent
        const std::vector<uint8_t> d = blocks(4096); File f{&d, SIZE_MAX, 1024, UINT64_MAX}; std::vector<uint8_t> o(4096);
        const XbgAcRead r = xbg_ac_read_staged(f, 4096, o.data(), bounce.data(), 256);
        bool black = true; for (size_t p = 1024; p < 4096; p += 16) black = black && o[p] == 0xFC && o[p + 1] == 0xFD && o[p + 15] == 0;
        CHECK(black && r.got == 1024 && r.err == EIO, "tail after a failed read");
    }
    printf("%s: %u bounce cases + in-place cases, %d checks, %d failed\n", fails ? "FAIL" : "PASS", cases, checks, fails);

    // 2. timing: 33.9 MB into a write-combined map
    const size_t N = size_t(33.9 * 1048576) & ~size_t(15);
    const std::vector<uint8_t> d = blocks(N); File f{&d, SIZE_MAX, UINT64_MAX, UINT64_MAX};
    uint8_t *wc = nullptr; const char *kind = "plain malloc (not write-combined)";
#ifdef _WIN32
    wc = static_cast<uint8_t *>(VirtualAlloc(nullptr, N, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE | PAGE_WRITECOMBINE));
    if (wc) kind = "VirtualAlloc PAGE_WRITECOMBINE";
#endif
    std::vector<uint8_t> plain;
    if (!wc) { plain.resize(N); wc = plain.data(); }
    std::vector<uint8_t> cached(N);
    memset(wc, 0, N); memset(cached.data(), 0, N);
    double best[4] = {1e30, 1e30, 1e30, 1e30}; XbgAcRead rr[4] = {};
    for (int rep = 0; rep < 3; rep++) {
        uint8_t *dsts[4] = {wc, wc, cached.data(), cached.data()};
        for (int m = 0; m < 4; m++) {
            const auto t0 = std::chrono::steady_clock::now();
            rr[m] = xbg_ac_read_staged(f, N, dsts[m], m & 1 ? bounce.data() : nullptr, K);
            const double ms = ms_since(t0); if (ms < best[m]) best[m] = ms;
        }
    }
    CHECK(rr[0].zero_fixed == rr[1].zero_fixed && rr[1].zero_fixed == rr[3].zero_fixed && !memcmp(wc, cached.data(), N), "timing runs give the same bytes");
    printf("timing %.1f MB, best of 3, map = %s:\n", N / 1048576.0, kind);
    printf("  map   in place (DM_NR_ASTCREAD=0, old) %8.1f ms  %6.2f ms/MB\n", best[0], best[0] / (N / 1048576.0));
    printf("  map   bounce   (DM_NR_ASTCREAD=1)      %8.1f ms  %6.2f ms/MB  (x%.1f)\n", best[1], best[1] / (N / 1048576.0), best[0] / best[1]);
    printf("  cached in place                        %8.1f ms\n", best[2]);
    printf("  cached bounce                          %8.1f ms  (the extra copy where the map is cached: %+.1f ms)\n", best[3], best[3] - best[2]);
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
