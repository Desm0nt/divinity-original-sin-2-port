// [DOS2] NR-TRX (docs/dos2/nr/NR-TRX.md): translator hot-path helpers for the zero-copy chunk table. Pure C (C and C++, no Vulkan, no
// engine): used by xbg_game.cpp (namespace zc, DM_NR_TRX_ZC / DM_NR_TRX_STICKY) and the host test tests/nr_trx_test.cpp.
//
// (1) nrt_ix: the live chunks of zc::table sorted by base, + the slot found last. zc::holder() scanned every slot of the table (live and
//     retired, ~25-35 slots of 88 bytes in Fort Joy) on every vertex / index / uniform view; the index answers with one compare (the
//     last chunk) or a binary search. Exact: built only from live chunks that do not overlap (zc keeps at most one live chunk per
//     address; if two overlap, ok = 0 and the caller scans as before), so the one live chunk holding an address is THE answer the scan
//     gives (the newest holder).
// (2) nrt_wrset: the byte ranges of one chunk the replay ever wrote on the CPU (zc::mark: UpdateUniformBuffer, CPU buffer copies,
//     fills), merged, sorted. zc marks whole 4 KB pages "sticky" (never bound in place again); a vertex / index range on such a page
//     whose bytes were never written by the replay reads the same bytes in place as a never-written page does. Each range keeps the
//     submission serial of its last write.
// (3) the class of a sticky-page vertex / index view (nrt_sticky_class) and nrt_svring: the views served in place on sticky pages, by
//     submission, for the last NRT_SV_RING submissions. A replay CPU write (zc::mark) first asks nrt_sv_conflict whether it meets a
//     view served for a submission the GPU may not have finished; if so the engine waits for that submission (or submits and waits when
//     it is the one being recorded) BEFORE the write lands, and the written bytes go "hot" (never served in place again). So the GPU
//     never reads a byte the CPU wrote after the view was served (docs/dos2/nr/NR-TRX.md section 8 has the proof).
#ifndef DM_NR_TRX_H
#define DM_NR_TRX_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NRT_IX_MAX = 512 };   // = zc::MAX_CHUNKS
typedef struct { uint64_t lo, hi; int32_t slot; int32_t pad; } nrt_ix_ent;
typedef struct { nrt_ix_ent e[NRT_IX_MAX]; int n, ok, mru; } nrt_ix;

// Build from the table: slot i is live when live[i], covering [lo[i], lo[i] + n[i]). ok = 0 when two live chunks overlap.
static inline void nrt_ix_build(nrt_ix *x, const uint64_t *lo, const uint64_t *n, const uint8_t *live, int count) {
    x->n = 0; x->ok = 1; x->mru = -1;
    for (int i = 0; i < count && x->n < NRT_IX_MAX; i++) if (live[i] && n[i]) {
        nrt_ix_ent e; e.lo = lo[i]; e.hi = lo[i] + n[i]; e.slot = i; e.pad = 0;
        int j = x->n++; while (j > 0 && x->e[j - 1].lo > e.lo) { x->e[j] = x->e[j - 1]; --j; }   // insertion sort: <= 512, rebuilt only on a table change
        x->e[j] = e;
    }
    for (int i = 0; i + 1 < x->n; i++) if (x->e[i].hi > x->e[i + 1].lo) { x->ok = 0; break; }
}
// The slot of the live chunk holding a, -1 = none. Only when ok.
static inline int nrt_ix_find(nrt_ix *x, uint64_t a) {
    if (x->mru >= 0) { const nrt_ix_ent *m = &x->e[x->mru]; if (a >= m->lo && a < m->hi) return m->slot; }
    int lo = 0, hi = x->n;
    while (lo < hi) { const int mid = (lo + hi) >> 1; if (x->e[mid].lo <= a) lo = mid + 1; else hi = mid; }
    if (lo > 0 && a < x->e[lo - 1].hi) { x->mru = lo - 1; return x->e[lo - 1].slot; }
    return -1;
}

// ---- written byte ranges of one chunk ----
enum { NRT_WR_MAX = 16384 };   // past it the chunk goes "coarse": pages only, as before
typedef struct { uint32_t lo, hi; uint64_t serial; } nrt_wr;          // [lo, hi), last write at submission serial
typedef struct { nrt_wr *v; uint32_t n, cap; int coarse; } nrt_wrset;
static inline void nrt_wr_free(nrt_wrset *s) { free(s->v); s->v = NULL; s->n = s->cap = 0; s->coarse = 0; }
static inline void nrt_wr_go_coarse(nrt_wrset *s) { free(s->v); s->v = NULL; s->n = s->cap = 0; s->coarse = 1; }
// first range with hi > a (ranges are disjoint and sorted, so hi is increasing too)
static inline uint32_t nrt_wr_first_after(const nrt_wrset *s, uint32_t a) {
    uint32_t lo = 0, hi = s->n;
    while (lo < hi) { const uint32_t mid = (lo + hi) >> 1; if (s->v[mid].hi <= a) lo = mid + 1; else hi = mid; }
    return lo;
}
// Add [a, b) written at `serial` (merged with every range it overlaps or touches). Returns 1 when the set went coarse.
static inline int nrt_wr_add(nrt_wrset *s, uint32_t a, uint32_t b, uint64_t serial) {
    if (s->coarse) return 0;
    if (b <= a) return 0;
    uint32_t i = 0; { uint32_t lo = 0, hi = s->n; while (lo < hi) { const uint32_t mid = (lo + hi) >> 1; if (s->v[mid].hi < a) lo = mid + 1; else hi = mid; } i = lo; }   // first with hi >= a
    uint32_t j = i, L = a, H = b; uint64_t S = serial;
    while (j < s->n && s->v[j].lo <= b) { if (s->v[j].lo < L) L = s->v[j].lo; if (s->v[j].hi > H) H = s->v[j].hi; if (s->v[j].serial > S) S = s->v[j].serial; ++j; }
    if (j > i) {   // merged into v[i], v[i+1 .. j) go
        s->v[i].lo = L; s->v[i].hi = H; s->v[i].serial = S;
        if (j > i + 1) { memmove(&s->v[i + 1], &s->v[j], (size_t)(s->n - j) * sizeof(nrt_wr)); s->n -= j - i - 1; }
        return 0;
    }
    if (s->n >= NRT_WR_MAX) { nrt_wr_go_coarse(s); return 1; }
    if (s->n == s->cap) {
        const uint32_t nc = s->cap ? s->cap * 2 : 16;
        nrt_wr *nv = (nrt_wr *)realloc(s->v, (size_t)nc * sizeof(nrt_wr));
        if (!nv) { nrt_wr_go_coarse(s); return 1; }
        s->v = nv; s->cap = nc;
    }
    memmove(&s->v[i + 1], &s->v[i], (size_t)(s->n - i) * sizeof(nrt_wr));
    s->v[i].lo = a; s->v[i].hi = b; s->v[i].serial = serial; ++s->n;
    return 0;
}
// Does [a, b) meet a written range? 1 = yes (*newest: the latest serial among those it meets), 0 = no. A coarse set always meets.
static inline int nrt_wr_meets(const nrt_wrset *s, uint32_t a, uint32_t b, uint64_t *newest) {
    if (s->coarse) { if (newest) *newest = UINT64_MAX; return 1; }
    uint32_t i = nrt_wr_first_after(s, a); int any = 0; uint64_t nw = 0;
    for (; i < s->n && s->v[i].lo < b; i++) { any = 1; if (s->v[i].serial > nw) nw = s->v[i].serial; if (!newest) break; }
    if (newest) *newest = nw;
    return any;
}

// ---- (3) sticky-page views: class, in-place serves by submission, the replay-write guard ----
// Class of a vertex / index view [a, b) on a sticky page of a chunk (wr: the replay's written ranges, hot: written ranges a write met
// while a view of them was in flight). now = the submission being recorded, age >= 1.
//   NEVER  none of its bytes was written by the replay (NR-TRX step 1)
//   STABLE every written range it meets was last written >= age submissions ago, and none of them is hot
//   RECENT a range it meets was written less than age submissions ago (the game's dynamic data: copy as before)
//   HOT    it meets a hot range;  COARSE  the chunk lost its byte ranges (> NRT_WR_MAX): pages only, as before
enum { NRT_CLS_RECENT = 0, NRT_CLS_NEVER = 1, NRT_CLS_STABLE = 2, NRT_CLS_HOT = 3, NRT_CLS_COARSE = 4, NRT_CLS_N = 5 };
static inline int nrt_sticky_class(const nrt_wrset *wr, const nrt_wrset *hot, uint32_t a, uint32_t b, uint64_t now, uint64_t age) {
    if (wr->coarse) return NRT_CLS_COARSE;
    uint64_t nw = 0;
    if (!nrt_wr_meets(wr, a, b, &nw)) return NRT_CLS_NEVER;
    if (nw + age > now) return NRT_CLS_RECENT;
    if (nrt_wr_meets(hot, a, b, NULL)) return NRT_CLS_HOT;   // a coarse hot set meets everything
    return NRT_CLS_STABLE;
}

// The views served in place (classes NEVER / STABLE) of the last NRT_SV_RING submissions: g[serial % NRT_SV_RING] holds submission
// `serial` (a slot whose serial differs is stale and is cleared on its next use). Per chunk table slot a bitmap of 64 KB blocks filters
// the writes that meet nothing (almost all) before the exact scan of the list. Entries carry the chunk's gen: a slot reused by a new
// chunk never matches the old chunk's views. `over`: an entry could not be stored (allocation failed) -> every write to a block the
// submission touched in that slot counts as a conflict (safe side).
enum { NRT_SV_RING = 4, NRT_SV_SHIFT = 16, NRT_SV_BLKS = 1024 };   // 1024 x 64 KB = a 64 MB chunk (zc::CHUNK); offsets past it share the last block
typedef struct { uint32_t lo, hi; int32_t slot; uint32_t pad; uint64_t gen; } nrt_sv_ent;
typedef struct {
    uint64_t serial;                                   // the submission (0 = empty)
    nrt_sv_ent *v; uint32_t n, cap; int over;
    uint32_t ntouched; uint16_t touched[NRT_IX_MAX]; uint8_t tmark[NRT_IX_MAX];
    uint64_t bits[NRT_IX_MAX][NRT_SV_BLKS / 64];
} nrt_svgen;
typedef struct { nrt_svgen g[NRT_SV_RING]; uint64_t notes, repeats, n_max, overs; } nrt_svring;
static inline uint32_t nrt_sv_blk(uint32_t off) { const uint32_t k = off >> NRT_SV_SHIFT; return k < NRT_SV_BLKS ? k : NRT_SV_BLKS - 1; }
static inline void nrt_sv_clear(nrt_svgen *g) {
    for (uint32_t i = 0; i < g->ntouched; i++) { const int t = g->touched[i]; memset(g->bits[t], 0, sizeof g->bits[t]); g->tmark[t] = 0; }
    g->ntouched = 0; g->n = 0; g->over = 0;
}
static inline void nrt_sv_free(nrt_svring *r) {
    for (int i = 0; i < NRT_SV_RING; i++) { nrt_svgen *g = &r->g[i]; nrt_sv_clear(g); free(g->v); g->v = NULL; g->cap = 0; g->serial = 0; }
}
// A view [lo, hi) of the chunk in table slot `slot` (gen) is served in place for submission `serial`.
static inline void nrt_sv_note(nrt_svring *r, uint64_t serial, int slot, uint64_t gen, uint32_t lo, uint32_t hi) {
    if (hi <= lo || slot < 0 || slot >= NRT_IX_MAX || !serial) return;
    nrt_svgen *g = &r->g[serial % NRT_SV_RING];
    if (g->serial != serial) { nrt_sv_clear(g); g->serial = serial; }
    ++r->notes;
    if (g->n) { const nrt_sv_ent *l = &g->v[g->n - 1]; if (l->lo == lo && l->hi == hi && l->slot == slot && l->gen == gen) { ++r->repeats; return; } }
    if (!g->tmark[slot]) { g->tmark[slot] = 1; g->touched[g->ntouched++] = (uint16_t)slot; }
    for (uint32_t k = nrt_sv_blk(lo), e = nrt_sv_blk(hi - 1); k <= e; k++) g->bits[slot][k >> 6] |= 1ull << (k & 63);
    if (g->over) return;
    if (g->n == g->cap) {
        const uint32_t nc = g->cap ? g->cap * 2 : 256;
        nrt_sv_ent *nv = (nrt_sv_ent *)realloc(g->v, (size_t)nc * sizeof(nrt_sv_ent));
        if (!nv) { g->over = 1; ++r->overs; return; }
        g->v = nv; g->cap = nc;
    }
    nrt_sv_ent *e = &g->v[g->n++]; e->lo = lo; e->hi = hi; e->slot = slot; e->pad = 0; e->gen = gen;
    if (g->n > r->n_max) r->n_max = g->n;
}
// Does a replay CPU write of [a, b) in the chunk (slot, gen) meet a view served in place for a submission in [floor, now]
// (= one the GPU may not have finished)? 1 = yes, *v = the latest such submission.
static inline int nrt_sv_conflict(const nrt_svring *r, uint64_t floor, uint64_t now, int slot, uint64_t gen, uint32_t a, uint32_t b, uint64_t *v) {
    int any = 0; uint64_t best = 0;
    if (b > a && slot >= 0 && slot < NRT_IX_MAX) {
        const uint32_t k0 = nrt_sv_blk(a), k1 = nrt_sv_blk(b - 1);
        for (int i = 0; i < NRT_SV_RING; i++) {
            const nrt_svgen *g = &r->g[i];
            if (!g->serial || g->serial < floor || g->serial > now || !g->tmark[slot]) continue;
            int hit = 0;
            for (uint32_t k = k0; k <= k1 && !hit; k++) hit = (int)((g->bits[slot][k >> 6] >> (k & 63)) & 1);
            if (!hit) continue;
            if (!g->over) {
                hit = 0;
                for (uint32_t j = 0; j < g->n; j++) { const nrt_sv_ent *e = &g->v[j]; if (e->slot == slot && e->gen == gen && e->lo < b && e->hi > a) { hit = 1; break; } }
            }
            if (hit) { any = 1; if (g->serial > best) best = g->serial; }
        }
    }
    if (v) *v = best;
    return any;
}
// What the engine does before the write lands: nothing; wait for the submissions up to v (an earlier one, already submitted); or
// submit the one being recorded and wait for everything (v == now: a draw of this submission, recorded before the write, reads it).
enum { NRT_GUARD_NONE = 0, NRT_GUARD_WAIT = 1, NRT_GUARD_SYNC = 2 };
static inline int nrt_guard_plan(int conflict, uint64_t v, uint64_t now) { return !conflict ? NRT_GUARD_NONE : v >= now ? NRT_GUARD_SYNC : NRT_GUARD_WAIT; }

#ifdef __cplusplus
}
#endif
#endif
