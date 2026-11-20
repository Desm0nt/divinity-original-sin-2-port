// [DOS2] NR-MDI DM_NR_MDI (docs/dos2/nr/NR-MDI.md): the batching model for depth-only draws - which consecutive (or, for order-free
// draws, sortable) draws one vkCmdDrawMultiIndexedEXT could carry if their per-draw uniform blocks were read by gl_DrawID instead of
// dynamic offsets. Pure C, no engine state (host test tests/nr_mdi_test.c); the backend (xbg_nr_mdi.inc) fills one nrmdi_draw per draw the
// NR-09 lane records and reads the counters every 10 s.
//
// Why a batch is "the same state except the per-draw blocks": on Turnip (turnip27, tu_cmd_buffer.cc tu_CmdDrawMultiIndexedEXT) a multi
// draw still emits VS params (draw id, vertex offset) and CP_DRAW_INDX_OFFSET per draw - what it saves on the GPU is only the state
// between the draws. In DOS2's depth passes that state is nearly always the descriptor set rebound with new dynamic offsets (the draw's
// world-matrix block in the engine ring): Turnip copies the dynamic descriptors to new memory and emits SP_UPDATE_CNTL(gfx_bindless=0xff),
// i.e. every bound set's descriptors are fetched again (DOOM Eternal GPUCOST 2026-11-05 cause #2). So a batch = draws with the same
// pipeline, layout, set, vertex bindings (buffer + binding offset after the NR-09 fold), index buffer + type, viewport, scissor, instance
// count and first instance, inside one render pass; firstIndex / indexCount / vertexOffset and the dynamic offsets may differ.
#ifndef NR_MDI_H
#define NR_MDI_H
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NRMDI_MAXOFF = 32, NRMDI_SET = 2048 };
typedef struct {
    uint64_t pass;                 // the render pass instance (the backend's pass counter)
    uint64_t pipe, layout, set;    // handles
    uint64_t vb[16], vbo[16];      // used bindings: buffer, binding offset (0 for unused)
    uint32_t used;                 // binding mask
    uint64_t ib, ibo; uint32_t it; // index buffer, offset, type (0 = not indexed)
    uint64_t vp, sc;               // viewport / scissor (hashes)
    uint32_t inst, first_inst;
    uint32_t noff; uint32_t off[NRMDI_MAXOFF];   // dynamic offsets (noff > NRMDI_MAXOFF: never batched)
    uint8_t depth_only, order_free;              // no colour written; NR-21's order-free test passed
} nrmdi_draw;

static inline uint64_t nrmdi_mix(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); h ^= h >> 31; h *= 0xBF58476D1CE4E5B9ull; return h ^ (h >> 29); }
// the batch key: everything but the dynamic offsets and the per-draw index parameters
static inline uint64_t nrmdi_key(const nrmdi_draw *d) {
    uint64_t h = nrmdi_mix(nrmdi_mix(nrmdi_mix(nrmdi_mix(0x4D4449, d->pass), d->pipe), d->layout), d->set);
    h = nrmdi_mix(h, d->used);
    for (uint32_t m = d->used; m; m &= m - 1) { const unsigned i = (unsigned)__builtin_ctz(m); h = nrmdi_mix(nrmdi_mix(h, d->vb[i]), d->vbo[i]); }
    h = nrmdi_mix(nrmdi_mix(nrmdi_mix(h, d->ib), d->ibo), d->it);
    h = nrmdi_mix(nrmdi_mix(h, d->vp), d->sc);
    h = nrmdi_mix(nrmdi_mix(nrmdi_mix(h, d->inst), d->first_inst), d->noff);
    return h | 1;
}
// exact: b may join the batch a stands for (all key fields equal)
static inline int nrmdi_same(const nrmdi_draw *a, const nrmdi_draw *b) {
    if (a->pass != b->pass || a->pipe != b->pipe || a->layout != b->layout || a->set != b->set || a->used != b->used) return 0;
    for (uint32_t m = a->used; m; m &= m - 1) { const unsigned i = (unsigned)__builtin_ctz(m); if (a->vb[i] != b->vb[i] || a->vbo[i] != b->vbo[i]) return 0; }
    if (a->ib != b->ib || a->ibo != b->ibo || a->it != b->it || a->vp != b->vp || a->sc != b->sc) return 0;
    if (a->inst != b->inst || a->first_inst != b->first_inst || a->noff != b->noff || a->noff > NRMDI_MAXOFF) return 0;
    return 1;
}
// what changes between two consecutive draws (bit per kind): the state a batch would not emit
enum { NRMDI_C_PIPE = 1, NRMDI_C_SET = 2, NRMDI_C_OFF = 4, NRMDI_C_VB = 8, NRMDI_C_IB = 16, NRMDI_C_VPSC = 32, NRMDI_C_INST = 64, NRMDI_C_PASS = 128 };
static inline unsigned nrmdi_changes(const nrmdi_draw *a, const nrmdi_draw *b, uint32_t *offmask) {
    unsigned c = 0; *offmask = 0;
    if (a->pass != b->pass) c |= NRMDI_C_PASS;
    if (a->pipe != b->pipe) c |= NRMDI_C_PIPE;
    if (a->layout != b->layout || a->set != b->set || a->noff != b->noff) c |= NRMDI_C_SET;
    else { const uint32_t n = a->noff < NRMDI_MAXOFF ? a->noff : NRMDI_MAXOFF; for (uint32_t i = 0; i < n; i++) if (a->off[i] != b->off[i]) *offmask |= 1u << i; if (*offmask) c |= NRMDI_C_OFF; }
    if (a->used != b->used) c |= NRMDI_C_VB;
    else for (uint32_t m = a->used; m; m &= m - 1) { const unsigned i = (unsigned)__builtin_ctz(m); if (a->vb[i] != b->vb[i] || a->vbo[i] != b->vbo[i]) { c |= NRMDI_C_VB; break; } }
    if (a->ib != b->ib || a->ibo != b->ibo || a->it != b->it) c |= NRMDI_C_IB;
    if (a->vp != b->vp || a->sc != b->sc) c |= NRMDI_C_VPSC;
    if (a->inst != b->inst || a->first_inst != b->first_inst) c |= NRMDI_C_INST;
    return c;
}

// The counters of one window. Depth-only draws only (others reset the recorded-order run).
typedef struct {
    uint64_t draws, depth, batches, in_multi, batched_rebinds, offmask_or[NRMDI_MAXOFF + 1];   // offmask_or[k]: batches whose varying offsets had popcount k
    uint64_t chg[8], chg_draws;                     // consecutive depth draws of one pass: how often each kind changed
    uint64_t of_draws, sorted_batches, of_passes;   // order-free draws, the batches they would form sorted by key (distinct keys per pass)
    uint64_t sizes[6];                              // batch sizes 1, 2, 3-4, 5-8, 9-32, > 32 (recorded order)
    uint64_t arith_batches, arith_draws;            // batches of 2+ whose varying offsets step by one constant delta each (the ring already
                                                    // holds their blocks as an array: one binding + gl_DrawID * delta, no copy)
} nrmdi_stats;
// the run in progress and the per-pass key set (generation stamped)
typedef struct {
    nrmdi_draw prev; int have; uint64_t run; uint32_t run_offs;
    int64_t delta[NRMDI_MAXOFF]; uint32_t dset, dbad;   // per offset slot: the first delta of the run, slots with a delta, slots whose delta changed
    uint64_t pass; uint32_t gen; uint32_t sgen[NRMDI_SET]; uint64_t skey[NRMDI_SET];
} nrmdi_state;
static inline void nrmdi_size(nrmdi_stats *s, uint64_t n) { s->sizes[n <= 1 ? 0 : n == 2 ? 1 : n <= 4 ? 2 : n <= 8 ? 3 : n <= 32 ? 4 : 5]++; }
static inline void nrmdi_end_run(nrmdi_state *t, nrmdi_stats *s) {
    if (!t->have || !t->run) return;
    ++s->batches; if (t->run >= 2) { s->in_multi += t->run; if (!t->dbad) { ++s->arith_batches; s->arith_draws += t->run; } }
    nrmdi_size(s, t->run); s->offmask_or[(unsigned)__builtin_popcount(t->run_offs)]++;
    t->run = 0; t->run_offs = 0; t->dset = 0; t->dbad = 0;
}
// A draw the lane recorded (after its commands). Returns 1 when it continues the batch of the previous one (recorded order).
static inline int nrmdi_note(nrmdi_state *t, nrmdi_stats *s, const nrmdi_draw *d) {
    ++s->draws;
    if (!d->depth_only) { nrmdi_end_run(t, s); t->have = 0; return 0; }
    ++s->depth;
    // the sorted estimate: distinct keys of the pass's order-free draws (one batch per key if the pass could be sorted freely)
    if (d->pass != t->pass) { t->pass = d->pass; if (++t->gen == 0) { memset(t->sgen, 0, sizeof t->sgen); t->gen = 1; } ++s->of_passes; }
    if (d->order_free) {
        ++s->of_draws; const uint64_t k = nrmdi_key(d); uint32_t i = (uint32_t)(k >> 40) & (NRMDI_SET - 1);
        unsigned probe = 0;
        for (; probe < NRMDI_SET; probe++, i = (i + 1) & (NRMDI_SET - 1)) {
            if (t->sgen[i] != t->gen) { t->sgen[i] = t->gen; t->skey[i] = k; ++s->sorted_batches; break; }
            if (t->skey[i] == k) break;
        }
        if (probe == NRMDI_SET) ++s->sorted_batches;   // the pass has more distinct keys than the set holds: a batch of its own (never fewer)
    } else ++s->sorted_batches;   // not order-free: stays where it is (its own batch at worst)
    int cont = 0;
    if (t->have) {
        uint32_t om = 0; const unsigned c = nrmdi_changes(&t->prev, d, &om);
        if (!(c & NRMDI_C_PASS)) { ++s->chg_draws; for (int b = 0; b < 8; b++) if (c >> b & 1) s->chg[b]++; }
        if (nrmdi_same(&t->prev, d)) { cont = 1; if (c & NRMDI_C_OFF) { ++s->batched_rebinds; t->run_offs |= om; }
            const uint32_t n = d->noff < NRMDI_MAXOFF ? d->noff : NRMDI_MAXOFF;
            for (uint32_t k = 0; k < n; k++) { const int64_t dl = (int64_t)d->off[k] - (int64_t)t->prev.off[k];
                if (!(t->dset >> k & 1)) { t->dset |= 1u << k; t->delta[k] = dl; } else if (t->delta[k] != dl) t->dbad |= 1u << k; } }
    }
    if (!cont) { nrmdi_end_run(t, s); }
    ++t->run; t->prev = *d; t->have = 1;
    return cont;
}
static inline void nrmdi_flush(nrmdi_state *t, nrmdi_stats *s) { nrmdi_end_run(t, s); t->have = 0; }

#ifdef __cplusplus
}
#endif
#endif
