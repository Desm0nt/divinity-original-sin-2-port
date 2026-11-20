// [DOS2] NR-03 DM_NR_OCC: native conditional rendering, the decision core (docs/dos2/nr/NR-03.md). Pure C, header only, no Vulkan, no NVN
// types: dm/gfx/vulkan/engine/xbg_nr_occ.inc drives it from the backend (queries, predicate buffer, draws), tests/nr_occ_test.c tests it.
//
// What the game does (main 4979b200, static reading 17.11, NR-03.md section 1): BeginFrame 0x9215f0 resets an atomic counter (dev+0x25ac0)
// and the CPU fills the frame's half of the 128 KB counter buffer dev+0x25ae8 with {0, -1} pairs (an unwritten pair reads "not equal" =
// render). An object that wants an occlusion test takes the next 16-byte entry of the frame's half (0xb0c5c0: atomic fetch_add, max 2048,
// entry = {pair address = buffer + 32 * index, frame}), draws ITSELF between ReportCounter(pair) and ReportCounter(pair + 16) (codes 61 /
// 62, 0xb89330: the object's own index buffer and count, depth pre-pass state) and later in the SAME frame draws its real passes under
// SetRenderEnableConditional(RENDER_IF_NOT_EQUAL, pair) (code 63, 0xb23970 / 0x142c870, then 0xaab150 DrawElements). Both sides check
// that the entry was taken in this frame (obj+0x270 == frame counter), so the predicate always names a pair of the same frame, and a pair
// address names a different object every frame (allocation order; the halves alternate). Consequences:
//  - The Switch's predicate = "did any sample of this object's own pre-pass draw pass", evaluated on the GPU in stream order.
//  - Native replacement: the pair is ONE Vulkan occlusion query around the draws between the two reports (no samples counter from the
//    start, no reports into guest memory needed: nothing but SetRenderEnableConditional reads them).
//  - Judging a condition by the previous frame's guest memory at the same address (DM_GFX_COND_RENDER=3, its would_skip3 count) judges
//    another object; a previous-frame variant must key by the object (its draw), never by the address.
//
// Paths (DM_NR_OCC_PATH, default by GPU family in xbg_vendor_policy.h xbg_nr_occ_path_default):
//   GPU   the pair's query result is copied into a predicate word at the end of the pass that closed the pair, the consumer draws under
//         VK_EXT_conditional_rendering on that word. Exact (the Switch's own decision, same frame); only a consumer in the same pass as its
//         pair cannot be predicated (renders). Costs a copy + barrier between passes: the next pass waits for the previous pass's
//         fragments, which on a tiler (Mali / PowerVR) stops binning of pass N+1 from overlapping fragments of pass N - hence:
//   PREV  the host skips the draw on the CPU when the same object (key of its pair draw) had no visible sample in the last retired frames
//         (streak >= N), only for a key that occurred once in that frame (or, for repeated meshes, whose exact constants matched), and
//         by default only when its vertex constants did not change (camera and object still). A newly revealed object is drawn up to the
//         ring latency (2 submissions) late: that is the risk, counted against the truth at retire (false culls, red lines).
//   A pair with no draw inside is judged on the CPU on both paths: zero samples, exact.
#ifndef DM_NR_OCC_H
#define DM_NR_OCC_H
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NRO_OFF = 0, NRO_ON = 1, NRO_CHECK = 2, NRO_POISON = 3, NRO_MODES = 4 };
enum { NRO_PATH_AUTO = 0, NRO_PATH_GPU = 1, NRO_PATH_PREV = 2 };
enum { NRO_NVN_IF_EQUAL = 0, NRO_NVN_IF_NOT_EQUAL = 1 };   // NVNconditionalRenderMode
enum { NRO_MAX_Q = 4096, NRO_MAX_PAIRS = 4096, NRO_MAX_CONDS = 8192, NRO_MAP = 8192, NRO_HIST = 16384 };

static inline int nro_clamp_mode(long v) { return v < 0 || v >= NRO_MODES ? NRO_OFF : (int)v; }
static inline int nro_clamp_path(long v) { return v == NRO_PATH_GPU || v == NRO_PATH_PREV ? (int)v : NRO_PATH_AUTO; }

// ---- hashing (keys of draws; not cryptographic, collisions only make a key "repeated" = rendered) ----
static inline uint64_t nro_mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h *= 0xff51afd7ed558ccdull; h ^= h >> 33;
    return h;
}
static inline uint64_t nro_hash_bytes(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    while (n >= 8) { uint64_t w; memcpy(&w, b, 8); h = nro_mix(h, w); b += 8; n -= 8; }
    if (n) { uint64_t w = 0; memcpy(&w, b, n); h = nro_mix(h, w ^ ((uint64_t)n << 56)); }
    return h;
}

// The Switch's decision for a pair whose draws let `samples` samples pass: 1 = render. Any NVN mode but the two compare modes renders.
static inline int nro_truth_render(int nvn_mode, uint64_t samples) {
    if (nvn_mode == NRO_NVN_IF_NOT_EQUAL) return samples != 0;
    if (nvn_mode == NRO_NVN_IF_EQUAL) return samples == 0;
    return 1;
}

// ---- one pair (two counter reports of one object in one submission) ----
enum { NRO_PS_OPEN = 1, NRO_PS_CLOSED = 2 };
enum { NRO_PF_BEGUN = 1, NRO_PF_ENDED = 2, NRO_PF_COPIED = 4 };
enum { NRO_INV_NONE = 0, NRO_INV_POOL = 1, NRO_INV_SPLIT = 2, NRO_INV_SKIPPED = 3, NRO_INV_NEST = 4, NRO_INV_OPEN = 5, NRO_INV_N = 6 };
typedef struct nro_pair {
    uint64_t addr;           // first report's address (the condition names it)
    uint64_t mesh, ubo;      // key of the draws inside (no constants) / hash of their vertex-stage constants
    int32_t q;               // query index in the slot's pool, -1 = none begun
    uint32_t seen, done;     // draws that entered the backend inside the pair / draws made with the query active
    uint32_t pass_close;     // pass serial when the pair closed
    uint32_t pass_endq;      // pass serial when its query ended: its result can be copied (and predicate a draw) once pass_serial is past it
    uint16_t occ;            // earlier pairs of this submission with the same mesh key
    uint8_t st, flags, inv;   // st: NRO_PS_* (not "state": the backend defines that name)
} nro_pair;

// A conditional draw, for the check at retire (mode 2, and the prev path's own accounting).
enum { NRO_CD_GPU_OK = 1, NRO_CD_PREV_STRICT = 2, NRO_CD_PREV_LOOSE = 4, NRO_CD_EXEC_SKIP = 8, NRO_CD_PREV_ADDR = 16 };
typedef struct nro_cond { int32_t pair; uint8_t nvn_mode, flags; uint16_t pad; uint64_t verts; } nro_cond;

typedef struct nro_mapent { uint64_t key; int32_t val; int32_t gen; } nro_mapent;

typedef struct nro_slot {
    nro_pair pair[NRO_MAX_PAIRS]; uint32_t npairs;
    int32_t open;            // open pair, -1 none
    int32_t qactive;         // pair whose query is active in the open pass, -1 none
    uint32_t qused;          // queries begun in this submission (indices 0..qused-1, each begun once and ended before any copy)
    uint32_t flush_lo;       // first query not copied by a pass-end flush yet
    uint32_t pass_serial;    // pass ends seen in this submission
    nro_cond cond[NRO_MAX_CONDS]; uint32_t nconds, conds_lost;
    nro_mapent amap[NRO_MAP];   // pair address -> latest pair index
    nro_mapent mmap[NRO_MAP];   // mesh key -> occurrences in this submission
    int32_t gen;
    uint32_t matched, closed_valid;   // prev path guard: closed pairs whose mesh key was in the last retired frame / all closed valid pairs
    uint64_t draws, verts;            // every draw / its vertices (index count x instances) of this submission
} nro_slot;

static inline uint32_t nro_slot_bucket(uint64_t k) { return (uint32_t)(nro_mix(0, k) & (NRO_MAP - 1)); }
// map get / put with a generation, so a reset is gen + 1 (no memset of 128 KB per submission)
static inline nro_mapent *nro_map_find(nro_mapent *m, int32_t gen, uint64_t key, int insert) {
    uint32_t b = nro_slot_bucket(key);
    for (uint32_t i = 0; i < 64; i++) {
        nro_mapent *e = &m[(b + i) & (NRO_MAP - 1)];
        if (e->gen != gen) { if (!insert) return 0; e->gen = gen; e->key = key; e->val = -1; return e; }
        if (e->key == key) return e;
    }
    return 0;
}
static inline void nro_slot_reset(nro_slot *s) {
    s->npairs = 0; s->open = -1; s->qactive = -1; s->qused = 0; s->flush_lo = 0; s->pass_serial = 0; s->nconds = 0; s->conds_lost = 0;
    s->matched = s->closed_valid = 0; s->draws = s->verts = 0;
    if (++s->gen <= 0) { memset(s->amap, 0, sizeof s->amap); memset(s->mmap, 0, sizeof s->mmap); s->gen = 1; }
}
static inline void nro_invalidate(nro_pair *p, int why) { if (!p->inv) p->inv = (uint8_t)why; }

// Report results of nro_report (bit set).
enum { NRO_R_OPENED = 1, NRO_R_CLOSED = 2, NRO_R_END_QUERY = 4, NRO_R_FULL = 8, NRO_R_NEST = 16 };
// A samples-passed ReportCounter(addr). *end_q: the query to end now (in the pass where it is active) when NRO_R_END_QUERY is set;
// *closed: the pair this report closed (NRO_R_CLOSED), -1 none.
static inline int nro_report(nro_slot *s, uint64_t addr, int32_t *end_q, int32_t *closed) {
    int r = 0;
    *end_q = -1; *closed = -1;
    if (s->open >= 0) {
        nro_pair *p = &s->pair[s->open];
        if (addr != p->addr + 16) { nro_invalidate(p, NRO_INV_NEST); r |= NRO_R_NEST; }   // another report inside: this pair is unusable
        if (s->qactive == s->open) { *end_q = p->q; p->flags |= NRO_PF_ENDED; p->pass_endq = s->pass_serial; s->qactive = -1; r |= NRO_R_END_QUERY; }
        if (p->seen != p->done) nro_invalidate(p, NRO_INV_SKIPPED);   // the backend left out a draw the Switch made: no judgement
        p->st = NRO_PS_CLOSED; p->pass_close = s->pass_serial;
        nro_mapent *e = nro_map_find(s->amap, s->gen, p->addr, 1); if (e) e->val = s->open;
        nro_mapent *m = nro_map_find(s->mmap, s->gen, p->mesh, 1); if (m) { p->occ = (uint16_t)(m->val < 0 ? 0 : m->val); m->val = (m->val < 0 ? 0 : m->val) + 1; }
        *closed = s->open; s->open = -1; r |= NRO_R_CLOSED;
        if (!(r & NRO_R_NEST)) return r;   // the closing report
    }
    if (s->npairs >= NRO_MAX_PAIRS) return r | NRO_R_FULL;
    nro_pair *p = &s->pair[s->npairs];
    memset(p, 0, sizeof *p); p->addr = addr; p->q = -1; p->st = NRO_PS_OPEN; p->mesh = 0x6e726f63ull; p->ubo = 0x75626f;
    s->open = (int32_t)s->npairs++;
    return r | NRO_R_OPENED;
}
// A draw entered the backend (before any of its own skips): inside an open pair it must reach the GPU with the query active.
static inline nro_pair *nro_draw_enter(nro_slot *s) {
    if (s->open < 0) return 0;
    nro_pair *p = &s->pair[s->open];
    ++p->seen;
    return p;
}
// Right before the draw command, inside its pass. 1 = begin query *q now (new index), 2 = the query is active already, 0 = no query
// (no open pair, an invalid one, or the pool is full).
static inline int nro_draw_query(nro_slot *s, int32_t *q) {
    *q = -1;
    if (s->open < 0) return 0;
    nro_pair *p = &s->pair[s->open];
    if (p->inv) return 0;
    if (s->qactive == s->open) { *q = p->q; return 2; }
    if (p->flags & NRO_PF_BEGUN) { nro_invalidate(p, NRO_INV_SPLIT); return 0; }   // a pass ended inside the pair and it drew again
    if (s->qused >= NRO_MAX_Q) { nro_invalidate(p, NRO_INV_POOL); return 0; }
    p->q = (int32_t)s->qused++; p->flags |= NRO_PF_BEGUN; s->qactive = s->open; *q = p->q;
    return 1;
}
static inline void nro_draw_done(nro_slot *s) { if (s->open >= 0 && s->qactive == s->open) ++s->pair[s->open].done; }
// The open pass ends: returns the query to end before vkCmdEndRenderPass (-1 none).
static inline int32_t nro_pass_end(nro_slot *s) {
    int32_t q = -1;
    if (s->qactive >= 0) { nro_pair *p = &s->pair[s->qactive]; q = p->q; p->flags |= NRO_PF_ENDED; p->pass_endq = s->pass_serial; s->qactive = -1; }
    ++s->pass_serial;
    return q;
}
// After the pass ended: the queries to copy into the predicate words now ([*lo, *lo + *n)); every one of them is ended. Pairs whose query
// is in the range are marked copied (a pair cannot draw after its query ended - that invalidates it - so the copied value is final).
static inline int nro_flush_range(nro_slot *s, uint32_t *lo, uint32_t *n) {
    *lo = s->flush_lo; *n = s->qused - s->flush_lo;
    if (!*n) return 0;
    for (uint32_t i = s->npairs; i-- > 0;) {
        nro_pair *p = &s->pair[i];
        if (p->q < 0) continue;
        if ((uint32_t)p->q < s->flush_lo) break;
        if (p->flags & NRO_PF_ENDED) p->flags |= NRO_PF_COPIED;
    }
    s->flush_lo = s->qused;
    return 1;
}
// Could the pair's result predicate a draw made now? (its query ended in a pass that has ended since: the pass-end copy ran)
static inline int nro_copy_ready(const nro_slot *s, const nro_pair *p) {
    return (p->flags & NRO_PF_BEGUN) && (p->flags & NRO_PF_ENDED) && s->pass_serial > p->pass_endq;
}
// The latest closed pair for a condition's address, -1 none.
static inline int32_t nro_lookup(nro_slot *s, uint64_t addr) {
    nro_mapent *e = nro_map_find(s->amap, s->gen, addr, 0);
    return e ? e->val : -1;
}

// ---- history of keys over retired submissions (prev path) ----
typedef struct nro_hent { uint64_t key, ubo; uint32_t last; uint16_t streak; uint8_t count, used; } nro_hent;
// serial: retired submissions with pairs folded in (1..); last = the serial a key was last seen in. Open addressing, rebuilt (keys seen in
// the last 8 serials kept) when half full; a probe chain past 48 makes the key "unknown" (rendered, counted in lost), never a wrong answer.
typedef struct nro_hist { nro_hent e[NRO_HIST]; nro_hent tmp[NRO_HIST]; uint32_t n, serial, rebuilds, lost; } nro_hist;
static inline nro_hent *nro_hist_slot(nro_hent *t, uint64_t key, int insert, uint32_t *n) {
    uint32_t b = (uint32_t)(nro_mix(1, key) & (NRO_HIST - 1));
    for (uint32_t i = 0; i < 48; i++) {
        nro_hent *e = &t[(b + i) & (NRO_HIST - 1)];
        if (!e->used) {
            if (!insert) return 0;
            memset(e, 0, sizeof *e); e->used = 1; e->key = key; ++*n; return e;
        }
        if (e->key == key) return e;
    }
    return 0;
}
static inline const nro_hent *nro_hist_get(const nro_hist *h, uint64_t key) { uint32_t n = 0; return nro_hist_slot((nro_hent *)h->e, key, 0, &n); }
static inline void nro_hist_rebuild(nro_hist *h, uint32_t keep) {
    memcpy(h->tmp, h->e, sizeof h->e); memset(h->e, 0, sizeof h->e); h->n = 0; ++h->rebuilds;
    for (uint32_t i = 0; i < NRO_HIST; i++) {
        const nro_hent *o = &h->tmp[i];
        if (!o->used || o->last + keep < h->serial) continue;
        nro_hent *e = nro_hist_slot(h->e, o->key, 1, &h->n);
        if (e) *e = *o;
    }
}
// Start folding in the next retired submission (call once per submission that had pairs).
static inline void nro_hist_begin(nro_hist *h) {
    if (h->n * 2 >= NRO_HIST) nro_hist_rebuild(h, 8);
    if (h->n * 2 >= NRO_HIST) nro_hist_rebuild(h, 0);   // that many keys a frame: keep the last frame only
    ++h->serial;
}
// One pair of the submission being folded in: hidden = no sample passed. A key seen twice in one submission is "repeated" (count > 1,
// streak 0: never culled by this key).
static inline void nro_hist_put(nro_hist *h, uint64_t key, uint64_t ubo, int hidden) {
    nro_hent *e = nro_hist_slot(h->e, key, 1, &h->n);
    if (!e) { ++h->lost; return; }
    if (e->last == h->serial) { e->count = (uint8_t)(e->count < 255 ? e->count + 1 : 255); e->streak = 0; e->ubo = ubo; return; }
    const int consecutive = e->last != 0 && e->last + 1 == h->serial;
    e->streak = (uint16_t)(!hidden ? 0 : consecutive ? (e->streak < 0xffff ? e->streak + 1 : 0xffff) : 1);
    e->count = 1; e->ubo = ubo; e->last = h->serial;
}
static inline uint64_t nro_strict_key(uint64_t mesh, uint64_t ubo) { return nro_mix(mesh ^ 0x5354524943540000ull, ubo); }
// Prev decision for a pair recorded now (h->serial = the last retired submission): 1 = cull. need: hidden streak; still: the vertex
// constants must equal the last retired frame's (object and camera still). A mesh key that occurred more than once (in that frame, or
// already in this submission: occ > 0) is judged by its strict key (mesh + constants), which implies still.
static inline int nro_prev_cull(const nro_hist *h, uint64_t mesh, uint64_t ubo, unsigned occ, unsigned need, int still) {
    if (!h->serial || need == 0) return 0;
    const nro_hent *e = nro_hist_get(h, mesh);
    if (!e || e->last != h->serial) return 0;
    if (e->count == 1 && occ == 0) return e->streak >= need && (!still || e->ubo == ubo);
    const nro_hent *s = nro_hist_get(h, nro_strict_key(mesh, ubo));
    return s && s->last == h->serial && s->count == 1 && s->streak >= need;
}
// The address-keyed judgement DM_GFX_COND_RENDER=3 makes (guest memory at the pair's address = whatever pair was written there last; the
// game's two halves alternate, so that is the submission before last), as a check-only variant (mode 2): right only while every object
// keeps its allocation index. Entries live in the same table under a tagged key; streak = 1 hidden / 0 visible.
static inline uint64_t nro_addr_key(uint64_t addr) { return nro_mix(addr ^ 0x4144445200000000ull, 0x61); }
static inline void nro_addr_put(nro_hist *h, uint64_t addr, int hidden) {
    nro_hent *e = nro_hist_slot(h->e, nro_addr_key(addr), 1, &h->n);
    if (!e) { ++h->lost; return; }
    e->streak = (uint16_t)(hidden ? 1 : 0); e->count = 1; e->last = h->serial;
}
static inline int nro_addr_cull(const nro_hist *h, uint64_t addr) {
    const nro_hent *e = nro_hist_get(h, nro_addr_key(addr));
    return e && h->serial && e->last + 2 >= h->serial && e->streak != 0;
}
// Guard of the prev path inside one submission: once 16 pairs closed, fewer than 3 of 4 found in the last retired frame = the scene changed
// (camera cut, load, fast pan): no prev culls for the rest of the submission.
static inline int nro_guard_scene(uint32_t matched, uint32_t closed) { return closed >= 16 && matched * 4 < closed * 3; }
// Reveal guard after a retire: keys that were hidden (streak >= need) and are visible now. Too many = the view opened up: hold off.
static inline int nro_guard_reveals(uint32_t reveals, uint32_t pairs) { return reveals > 4 && reveals * 50 > pairs; }

#ifdef __cplusplus
}
#endif
#endif
