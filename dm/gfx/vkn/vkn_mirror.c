// [DOS2] VK-NATIVE stage 4: changed rows of CPU-written textures (vkn_mirror.h). Plain C, no game addresses, no Vulkan.
#ifdef DM_GAME_DOS2
#include "vkn_mirror.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// ---- the hash: XXH64's round on 4 lanes, rows fed one after another (a row tail is zero-padded to 32 bytes and its length mixed in) ----
#define MP1 0x9E3779B185EBCA87ull
#define MP2 0xC2B2AE3D27D4EB4Full
#define MP3 0x165667B19E3779F9ull
#define MP4 0x85EBCA77C2B2AE63ull
static inline uint64_t rotl64(uint64_t x, int r) { return x << r | x >> (64 - r); }
static inline uint64_t rnd(uint64_t a, uint64_t x) { a += x * MP2; a = rotl64(a, 31); return a * MP1; }
static inline uint64_t ld64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
typedef struct { uint64_t v0, v1, v2, v3, n; } hs_t;
static void hs_init(hs_t *s, uint64_t seed) { s->v0 = seed + MP1 + MP2; s->v1 = seed + MP2; s->v2 = seed; s->v3 = seed - MP1; s->n = 0; }
static void hs_feed(hs_t *s, const uint8_t *p, size_t n) {
    uint64_t v0 = s->v0, v1 = s->v1, v2 = s->v2, v3 = s->v3;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) { v0 = rnd(v0, ld64(p + i)); v1 = rnd(v1, ld64(p + i + 8)); v2 = rnd(v2, ld64(p + i + 16)); v3 = rnd(v3, ld64(p + i + 24)); }
    if (i < n) {
        uint8_t t[32]; memset(t, 0, sizeof t); memcpy(t, p + i, n - i);
        v0 = rnd(v0, ld64(t) ^ (uint64_t)(n - i)); v1 = rnd(v1, ld64(t + 8)); v2 = rnd(v2, ld64(t + 16)); v3 = rnd(v3, ld64(t + 24));
    }
    s->v0 = v0; s->v1 = v1; s->v2 = v2; s->v3 = v3; s->n += n;
}
static uint64_t hs_end(const hs_t *s) {
    uint64_t h = rotl64(s->v0, 1) + rotl64(s->v1, 7) + rotl64(s->v2, 12) + rotl64(s->v3, 18);
    h = (h ^ rnd(0, s->v0)) * MP1 + MP4; h = (h ^ rnd(0, s->v1)) * MP1 + MP4;
    h = (h ^ rnd(0, s->v2)) * MP1 + MP4; h = (h ^ rnd(0, s->v3)) * MP1 + MP4;
    h += s->n;
    h ^= h >> 33; h *= MP2; h ^= h >> 29; h *= MP3; h ^= h >> 32;
    return h;
}
uint64_t vkn_mirror_hash(const void *p, size_t n, uint64_t seed) { hs_t s; hs_init(&s, seed); hs_feed(&s, (const uint8_t *)p, n); return hs_end(&s); }
static uint64_t band_hash(const uint8_t *src, uint32_t stride, uint32_t rowbytes, uint32_t y, uint32_t rows) {
    hs_t s; hs_init(&s, y);
    for (uint32_t r = 0; r < rows; r++) hs_feed(&s, src + (uint64_t)(y + r) * stride, rowbytes);
    return hs_end(&s);
}

// ---- the regions ----
typedef struct mk { uint64_t key, gen, use; uint32_t stride, rowbytes, rows, nb; uint64_t *h; } mk;
static mk g_k[VKN_MIRROR_KEYS];
static uint64_t g_use;
static vkn_mirror_stats g_st;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static mk *find_locked(uint64_t key) {
    for (int i = 0; i < VKN_MIRROR_KEYS; i++) if (g_k[i].h && g_k[i].key == key) return &g_k[i];
    return NULL;
}
static void drop_locked(mk *e) { free(e->h); memset(e, 0, sizeof *e); }

int vkn_mirror_diff(uint64_t key, uint64_t gen, const uint8_t *src, uint32_t stride, uint32_t rowbytes, uint32_t rows, vkn_mirror_run *runs, int max_runs, int *full) {
    if (full) *full = 0;
    if (!src || !runs || !rows || !rowbytes || stride < rowbytes || max_runs < 1) return -1;
    if (max_runs > VKN_MIRROR_RUNS) max_runs = VKN_MIRROR_RUNS;
    const uint32_t nb = (rows + VKN_MIRROR_BAND - 1) / VKN_MIRROR_BAND;
    pthread_mutex_lock(&g_mu);
    g_st.diffs++;
    mk *e = find_locked(key);
    if (e && (e->gen != gen || e->stride != stride || e->rowbytes != rowbytes || e->rows != rows)) { drop_locked(e); e = NULL; }
    int fresh = 0;
    if (!e) {   // a free entry, else the least recently used one
        mk *v = &g_k[0];
        for (int i = 0; i < VKN_MIRROR_KEYS; i++) { if (!g_k[i].h) { v = &g_k[i]; break; } if (g_k[i].use < v->use) v = &g_k[i]; }
        if (v->h) { g_st.evicted++; drop_locked(v); }
        v->h = (uint64_t *)malloc(sizeof(uint64_t) * nb);
        if (v->h) { v->key = key; v->gen = gen; v->stride = stride; v->rowbytes = rowbytes; v->rows = rows; v->nb = nb; e = v; }
        else g_st.no_mem++;
        fresh = 1;
    }
    if (e) e->use = ++g_use;
    int n = 0;
    if (fresh) {
        // Nothing known: the whole region, and its hashes for the next time (when there is room for them).
        if (e) for (uint32_t b = 0; b < nb; b++) { const uint32_t y = b * VKN_MIRROR_BAND; e->h[b] = band_hash(src, stride, rowbytes, y, rows - y < VKN_MIRROR_BAND ? rows - y : VKN_MIRROR_BAND); }
        if (e) g_st.hashed_bytes += (uint64_t)rowbytes * rows;
        runs[0].y = 0; runs[0].rows = rows; n = 1;
        g_st.full++;
        if (full) *full = 1;
    } else {
        for (uint32_t b = 0; b < nb; b++) {
            const uint32_t y = b * VKN_MIRROR_BAND, cnt = rows - y < VKN_MIRROR_BAND ? rows - y : VKN_MIRROR_BAND;
            const uint64_t h = band_hash(src, stride, rowbytes, y, cnt);
            if (h == e->h[b]) continue;
            e->h[b] = h; g_st.dirty_bands++;
            if (n && runs[n - 1].y + runs[n - 1].rows == y) runs[n - 1].rows += cnt;            // adjacent: one run
            else if (n == max_runs) { runs[n - 1].rows = y + cnt - runs[n - 1].y; g_st.merged++; }   // over the limit: the last run grows
            else { runs[n].y = y; runs[n].rows = cnt; n++; }
        }
        g_st.bands += nb; g_st.hashed_bytes += (uint64_t)rowbytes * rows;
        if (!n) g_st.unchanged++;
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}

void vkn_mirror_forget(uint64_t key) {
    pthread_mutex_lock(&g_mu);
    mk *e = find_locked(key);
    if (e) { drop_locked(e); g_st.forgot++; }
    pthread_mutex_unlock(&g_mu);
}
void vkn_mirror_forget_all(void) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < VKN_MIRROR_KEYS; i++) if (g_k[i].h) drop_locked(&g_k[i]);
    pthread_mutex_unlock(&g_mu);
}
void vkn_mirror_stats_get(vkn_mirror_stats *s) { pthread_mutex_lock(&g_mu); *s = g_st; pthread_mutex_unlock(&g_mu); }
#endif
