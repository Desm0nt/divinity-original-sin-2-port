// [DOS2] NR-ZERO Z5 (DM_NR_Z5, docs/dos2/nr/NR-ZERO.md section 14): the commands of a RUN of recorded engine draws, written apart from the
// translation thread's bind cache. No Vulkan / engine type, no environment (handles are words; viewport and scissor are the bytes the
// bind cache compares). The engine side is xbg_nr_z5.inc (namespace nrz5: the comparison of DM_NR_Z5=2), the packet side
// xbg_nb_mt.inc (Build, same_stream) and nb_mt.h (Queue::splice); the host test is tests/nr_z5_test.cpp.
//
// The lane's tail (nrdraw::lane_tail) records a draw as: the pipeline when it changed, the descriptor set when the set or a dynamic
// offset changed, viewport / scissor when they changed, one vkCmdBindVertexBuffers for each run of changed streams (nr_vb_runs), the
// index buffer when it changed, the draw. "Changed" is against the bind cache of the command buffer - the state of the translation
// thread at that moment. A writer that is not that thread (the game thread, Z5) does not have it. nrz5_emit is the tail's command
// section over a cache with a KNOWN mask:
//   a draw that reads only known fields      its commands do not depend on the real cache: they can be written ahead (the TAIL of a run)
//   a draw that reads a field not yet known   only the translation thread can record it (the HEAD of a run: its first draws; after a
//                                             draw every field it read is known and equal to its values, whatever the cache was)
// So a run is: head draws recorded by the translation thread as today (the first-draw fix-up is "the head is not written ahead"), then a
// chunk with the tail's packets, handed over at the position of the first tail draw. A draw that reads a new unknown field after the
// tail began ends the run (it is the head of the next one). After the chunk the real cache is the model's on every known field
// (nrz5_cache holds them; the engine copies them back).
#ifndef DM_NR_Z5_H
#define DM_NR_Z5_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "nr_draw.h"   // nr_vb_runs

// The mutation switch of the host test (never defined in the engine build): NRZ5_MUTATE = k - 1 the pipeline is not compared with the
// cache, 2 the set's dynamic offsets are not, 3 a set with more than 32 offsets is remembered as bound, 4 the viewport is not compared,
// 5 the scissor, 6 the vertex runs do not cover the used streams between two changed ones, 7 the index type is not compared, 8 the
// unknown test forgets the vertex streams, 9 forgets the index buffer, 10 the cache keeps the stream of a run that was not changed
// (it always does: the mutation drops the update of a changed one), 11 the base vertex is not folded into the draw.
#ifndef NRZ5_MUTATE
#define NRZ5_MUTATE 0
#endif

enum { NRZ5_VP_BYTES = 24, NRZ5_SC_BYTES = 16, NRZ5_MAX_OFF = 32,
       NRZ5_K_PIPE = 1, NRZ5_K_SET = 2, NRZ5_K_VP = 4, NRZ5_K_SC = 8, NRZ5_K_IB = 16 };

// One recorded draw: what the tail holds when it reaches its command section.
typedef struct nrz5_vals {
    uint64_t pipe, layout, set;
    uint32_t noff; const uint32_t *off;          // dynamic offsets of the set (any count; the cache holds at most NRZ5_MAX_OFF)
    uint8_t vp[NRZ5_VP_BYTES], sc[NRZ5_SC_BYTES];
    uint16_t used;                               // vertex bindings of the pipeline
    uint64_t vb[16], vbo[16];                    // buffer and (folded) offset of each used binding
    uint8_t indexed; uint32_t it; uint64_t ib, ibo;
    uint32_t count, instances, first, first_instance; int32_t vertex_offset;   // indexed: first = firstIndex, vertex_offset = base vertex + folded vertices; else first = first vertex + folded
} nrz5_vals;
// The bind cache as far as the tail reads it, with what the run knows of it.
typedef struct nrz5_cache {
    uint32_t known; uint16_t known_vb;           // NRZ5_K_* / bit i: binding i
    uint64_t pipe, layout, set; uint32_t noff, off[NRZ5_MAX_OFF];
    uint8_t has_vp, has_sc, vp[NRZ5_VP_BYTES], sc[NRZ5_SC_BYTES];
    uint64_t vb[16], vbo[16], ib, ibo; uint32_t it;
} nrz5_cache;
static inline void nrz5_cache_unknown(nrz5_cache *c) { memset(c, 0, sizeof *c); c->noff = 0xffffffffu; }

// Does the draw read a field of the cache the run has not set yet?
static inline int nrz5_reads_unknown(const nrz5_cache *c, const nrz5_vals *v) {
    const uint32_t need = NRZ5_K_PIPE | NRZ5_K_SET | NRZ5_K_VP | NRZ5_K_SC | (v->indexed && NRZ5_MUTATE != 9 ? (uint32_t)NRZ5_K_IB : 0u);
    if ((c->known & need) != need) return 1;
    if (NRZ5_MUTATE != 8 && (c->known_vb & v->used) != v->used) return 1;
    return 0;
}

// The tail's command section for one draw over the cache `c` (every field the draw reads must be meaningful: the real cache, or a model
// in which nrz5_reads_unknown answered 0). The sink gets the commands in the tail's order; the cache ends as the tail leaves it. Returns
// the number of commands. Sink (any type with these members; a false return is passed on as 0: the sink is full):
//   bool pipeline(uint64_t pipe)                                              vkCmdBindPipeline(GRAPHICS, pipe)
//   bool sets(uint64_t layout, uint64_t set, uint32_t n, const uint32_t *off) vkCmdBindDescriptorSets(GRAPHICS, layout, 0, 1, &set, n, off)
//   bool viewport(const uint8_t *vp)   bool scissor(const uint8_t *sc)        vkCmdSetViewport / vkCmdSetScissor(0, 1, ...)
//   bool vertex(uint32_t first, uint32_t n, const uint64_t *buf, const uint64_t *off)
//   bool index(uint64_t buf, uint64_t off, uint32_t type)
//   bool draw_indexed(count, instances, first_index, vertex_offset, first_instance) / bool draw(count, instances, first, first_instance)
template <class Sink> static inline unsigned nrz5_emit(Sink &s, nrz5_cache *c, const nrz5_vals *v) {
    unsigned n = 0;
    if (NRZ5_MUTATE == 1 || c->pipe != v->pipe) { if (!s.pipeline(v->pipe)) return 0; c->pipe = v->pipe; ++n; }
    {
        const uint32_t no = v->noff;
        if (c->layout != v->layout || c->set != v->set || c->noff != no || no > NRZ5_MAX_OFF || (NRZ5_MUTATE != 2 && no && memcmp(c->off, v->off, (size_t)no * 4))) {
            if (!s.sets(v->layout, v->set, no, v->off)) return 0; ++n;
            if (no <= NRZ5_MAX_OFF || NRZ5_MUTATE == 3) { c->layout = v->layout; c->set = v->set; c->noff = no; if (no && no <= NRZ5_MAX_OFF) memcpy(c->off, v->off, (size_t)no * 4); }
            else c->noff = 0xffffffffu;   // (more offsets than the cache holds: bound again at the next draw)
        }
    }
    if (!c->has_vp || (NRZ5_MUTATE != 4 && memcmp(c->vp, v->vp, NRZ5_VP_BYTES))) { if (!s.viewport(v->vp)) return 0; memcpy(c->vp, v->vp, NRZ5_VP_BYTES); c->has_vp = 1; ++n; }
    if (!c->has_sc || (NRZ5_MUTATE != 5 && memcmp(c->sc, v->sc, NRZ5_SC_BYTES))) { if (!s.scissor(v->sc)) return 0; memcpy(c->sc, v->sc, NRZ5_SC_BYTES); c->has_sc = 1; ++n; }
    {
        unsigned changed = 0;
        for (uint32_t m = v->used; m; m &= m - 1) { const uint32_t i = (uint32_t)__builtin_ctz(m); if (c->vb[i] != v->vb[i] || c->vbo[i] != v->vbo[i]) changed |= 1u << i; }
        if (changed) {
            uint8_t f[8], k[8]; const unsigned runs = NRZ5_MUTATE == 6 ? nr_vb_runs(changed, changed, f, k) : nr_vb_runs(changed, v->used, f, k);
            for (unsigned r = 0; r < runs; r++) {
                uint64_t o[16];
                for (unsigned j = 0; j < k[r]; j++) { o[j] = v->vbo[f[r] + j]; if (NRZ5_MUTATE != 10) { c->vb[f[r] + j] = v->vb[f[r] + j]; c->vbo[f[r] + j] = v->vbo[f[r] + j]; } }
                if (!s.vertex(f[r], k[r], &v->vb[f[r]], o)) return 0; ++n;
            }
        }
    }
    if (v->indexed) {
        if (c->ib != v->ib || c->ibo != v->ibo || (NRZ5_MUTATE != 7 && c->it != v->it)) { if (!s.index(v->ib, v->ibo, v->it)) return 0; c->ib = v->ib; c->ibo = v->ibo; c->it = v->it; ++n; }
        if (!s.draw_indexed(v->count, v->instances, v->first, NRZ5_MUTATE == 11 ? 0 : v->vertex_offset, v->first_instance)) return 0;
    } else if (!s.draw(v->count, v->instances, v->first, v->first_instance)) return 0;
    return n + 1;
}
// After a draw every field it read is the draw's (whatever the cache held before): what the run knows after a HEAD draw the translation
// thread recorded itself. (A set with more offsets than the cache holds leaves "bound again at the next draw": known, too.)
static inline void nrz5_after(nrz5_cache *c, const nrz5_vals *v) {
    c->pipe = v->pipe; c->known |= NRZ5_K_PIPE | NRZ5_K_SET | NRZ5_K_VP | NRZ5_K_SC;
    if (v->noff <= NRZ5_MAX_OFF) { c->layout = v->layout; c->set = v->set; c->noff = v->noff; if (v->noff) memcpy(c->off, v->off, (size_t)v->noff * 4); }
    else c->noff = 0xffffffffu;
    memcpy(c->vp, v->vp, NRZ5_VP_BYTES); c->has_vp = 1; memcpy(c->sc, v->sc, NRZ5_SC_BYTES); c->has_sc = 1;
    for (uint32_t m = v->used; m; m &= m - 1) { const uint32_t i = (uint32_t)__builtin_ctz(m); c->vb[i] = v->vb[i]; c->vbo[i] = v->vbo[i]; }
    c->known_vb = (uint16_t)(c->known_vb | v->used);
    if (v->indexed) { c->ib = v->ib; c->ibo = v->ibo; c->it = v->it; c->known |= NRZ5_K_IB; }
}
// Do the known fields of the model equal the same fields of another cache (the real one, converted)? 0 = same; else the first field that
// differs: 1 pipeline, 2 set / offsets, 3 viewport, 4 scissor, 5 a vertex stream, 6 the index buffer.
static inline int nrz5_cache_diff(const nrz5_cache *m, const nrz5_cache *r) {
    if ((m->known & NRZ5_K_PIPE) && m->pipe != r->pipe) return 1;
    if (m->known & NRZ5_K_SET) {
        if (m->noff != r->noff) return 2;
        if (m->noff != 0xffffffffu && (m->layout != r->layout || m->set != r->set || (m->noff && memcmp(m->off, r->off, (size_t)m->noff * 4)))) return 2;
    }
    if ((m->known & NRZ5_K_VP) && (!r->has_vp || memcmp(m->vp, r->vp, NRZ5_VP_BYTES))) return 3;
    if ((m->known & NRZ5_K_SC) && (!r->has_sc || memcmp(m->sc, r->sc, NRZ5_SC_BYTES))) return 4;
    for (uint32_t k = m->known_vb; k; k &= k - 1) { const uint32_t i = (uint32_t)__builtin_ctz(k); if (m->vb[i] != r->vb[i] || m->vbo[i] != r->vbo[i]) return 5; }
    if ((m->known & NRZ5_K_IB) && (m->ib != r->ib || m->ibo != r->ibo || m->it != r->it)) return 6;
    return 0;
}
#endif
