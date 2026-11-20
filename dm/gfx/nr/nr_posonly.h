// [DOS2] NR-POSONLY DM_NR_POSONLY (docs/dos2/nr/NR-POSONLY.md): compact position-only copies of static vertex buffers for depth-only
// draws. Pure C (host test tests/nr_posonly_test.cpp): the copy (the position attribute's bytes of every vertex, the same bytes and format
// - bit-exact by construction), the arena (one buffer, first-fit with a retire delay: a freed range is reused only once every submission
// that could read it retired), the vertex count of a range.
#ifndef NR_POSONLY_H
#define NR_POSONLY_H
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif

// Bytes of a Vulkan vertex format the position may use (0 = not handled: the draw keeps the interleaved buffer).
static inline uint32_t nrpo_fmt_size(uint32_t vk_format) {
    switch (vk_format) {
    case 106: case 104: case 105: return 12;   // R32G32B32 SFLOAT / UINT / SINT
    case 109: case 107: case 108: return 16;   // R32G32B32A32
    case 103: case 101: case 102: return 8;    // R32G32
    case 90: case 84: case 85: case 86: case 87: case 88: case 89: return 6;   // R16G16B16 *
    case 97: case 91: case 92: case 93: case 94: case 95: case 96: return 8;   // R16G16B16A16 *
    case 83: case 77: case 78: case 79: case 80: case 81: case 82: return 4;   // R16G16 *
    case 37: case 38: case 39: case 40: case 41: case 42: case 43: case 44: return 4;   // R8G8B8A8 *
    default: return 0;
    }
}
// Vertices of a range of `bytes` whose vertex i has its position at i * stride + pos_off (pos_size bytes): the ones that fit.
static inline uint64_t nrpo_nverts(uint64_t bytes, uint32_t stride, uint32_t pos_off, uint32_t pos_size) {
    if (!stride || !pos_size || (uint64_t)pos_off + pos_size > bytes) return 0;
    return (bytes - pos_off - pos_size) / stride + 1;
}
// dst[i * pos_size ..] = src[i * stride + pos_off ..] for i < nverts (the attribute's bytes, nothing converted).
static inline void nrpo_gather(uint8_t *dst, const uint8_t *src, uint64_t nverts, uint32_t stride, uint32_t pos_off, uint32_t pos_size) {
    const uint8_t *s = src + pos_off;
    switch (pos_size) {
    case 6: for (uint64_t i = 0; i < nverts; i++, s += stride, dst += 6) memcpy(dst, s, 6); break;
    case 8: for (uint64_t i = 0; i < nverts; i++, s += stride, dst += 8) memcpy(dst, s, 8); break;
    case 12: for (uint64_t i = 0; i < nverts; i++, s += stride, dst += 12) memcpy(dst, s, 12); break;
    default: for (uint64_t i = 0; i < nverts; i++, s += stride, dst += pos_size) memcpy(dst, s, pos_size); break;
    }
}

// ---- the arena: [0, size) of one buffer. Ranges of 64-byte granules; a free range waits until serial > freed + delay ----
enum { NRPO_ALIGN = 64, NRPO_MAXFREE = 8192 };
typedef struct { uint64_t off, size, serial; } nrpo_free_t;
typedef struct {
    uint64_t size, top;                          // bump top (everything above is free and never used yet)
    nrpo_free_t fr[NRPO_MAXFREE]; uint32_t nfr;  // freed ranges (any order)
    uint64_t live, peak, allocs, fails, reused;
} nrpo_arena;
static inline void nrpo_arena_init(nrpo_arena *a, uint64_t size) { memset(a, 0, sizeof *a); a->size = size & ~(uint64_t)(NRPO_ALIGN - 1); }
// A range of n bytes (rounded up to the granule) for a copy recorded at `serial`; delay: submissions that may still read a freed range.
// Returns the offset, or ~0 when nothing fits.
static inline uint64_t nrpo_alloc(nrpo_arena *a, uint64_t n, uint64_t serial, uint64_t delay) {
    n = (n + NRPO_ALIGN - 1) & ~(uint64_t)(NRPO_ALIGN - 1);
    if (!n) return ~0ull;
    int best = -1;
    for (uint32_t i = 0; i < a->nfr; i++)
        if (a->fr[i].size >= n && serial > a->fr[i].serial + delay && (best < 0 || a->fr[i].size < a->fr[best].size)) best = (int)i;
    uint64_t off;
    if (best >= 0) {
        off = a->fr[best].off;
        if (a->fr[best].size == n) a->fr[best] = a->fr[--a->nfr]; else { a->fr[best].off += n; a->fr[best].size -= n; }
        ++a->reused;
    } else if (a->top + n <= a->size) { off = a->top; a->top += n; }
    else { ++a->fails; return ~0ull; }
    a->live += n; if (a->live > a->peak) a->peak = a->live; ++a->allocs;
    return off;
}
// The copy at off (n bytes as asked at alloc) is dropped at `serial`; its range is reusable after serial + delay. 0 = the free list is
// full (the range is lost until nrpo_arena_init: counted by the caller).
static inline int nrpo_free(nrpo_arena *a, uint64_t off, uint64_t n, uint64_t serial) {
    n = (n + NRPO_ALIGN - 1) & ~(uint64_t)(NRPO_ALIGN - 1);
    a->live -= n;
    for (uint32_t i = 0; i < a->nfr; i++) {   // merge with a neighbour freed at the same serial or earlier (keep the later serial)
        nrpo_free_t *f = &a->fr[i];
        if (f->off + f->size == off) { f->size += n; if (serial > f->serial) f->serial = serial; return 1; }
        if (off + n == f->off) { f->off = off; f->size += n; if (serial > f->serial) f->serial = serial; return 1; }
    }
    if (a->nfr >= NRPO_MAXFREE) return 0;
    a->fr[a->nfr].off = off; a->fr[a->nfr].size = n; a->fr[a->nfr].serial = serial; ++a->nfr;
    return 1;
}
#ifdef __cplusplus
}
#endif
#endif
