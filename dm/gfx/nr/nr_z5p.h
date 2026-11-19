// [DOS2] NR-ZERO Z5, the game thread's values (DM_NR_Z5, docs/dos2/nr/NR-ZERO.md section 22): what the translation thread PUBLISHES
// for the draws it already resolved, and how the game thread turns it into the values of the draw it is writing. C++, no Vulkan /
// engine type (handles are words, viewport and scissor are the bytes the bind cache compares). The run protocol is nr_z5r.h, the
// engine side xbg_nr_z5.inc (namespace nrz5g); the host tests are tests/nr_z5p_test.cpp and tests/nr_z5r_test.cpp.
//
// The game thread writes an engine draw record long before the translation thread executes it. What it has in hand is the record:
// the engine's key id, the draw arguments, the index buffer address, the uniform ranges it bound. What the lane's tail binds - the
// pipeline, the descriptor set and its dynamic offsets, the vertex / index views, viewport and scissor - is resolved by the
// translation thread from its own state at the moment of execution. But the same draw comes back frame after frame, so:
//   site      the words of a draw record the game thread has in hand (nrz5_site). A SLOT of the table is chosen from them (and from
//             how many records of the same site this frame already had): the record carries the slot.
//   template  what the translation thread recorded for the record of that slot the last time it recorded one itself (nrz5_tmpl):
//             pipeline, layout, set, for each dynamic offset the uniform slot it comes from and (address - offset), the vertex /
//             index views as bound (folded), viewport, scissor. Published in the table, lock free: one writer (the translation
//             thread), readers validate with the slot's version (a seqlock: an odd version = being written; a read whose two
//             version loads differ is thrown away). Every word is an atomic: no data race by the letter of the standard either.
//   values    nrz5p_vals: the template + the record's own words (uniform addresses, counts) = the nrz5_vals of the draw, or "no
//             prediction" (another site in the slot, a template that says so, an unknown uniform address, an offset past 32 bits).
// A prediction is ONLY a prediction. Nothing here makes it right: the translation thread compares the predicted values of every
// draw with the values it resolves itself before it uses one packet written from them (nr_z5r.h).
#ifndef DM_NR_Z5P_H
#define DM_NR_Z5P_H
#include <atomic>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include "nr_z5.h"

// The mutation switch of the host test (never defined in the engine build): NRZ5P_MUTATE = k - 1 a read does not look at the version
// after the copy, 2 a read takes a slot that is being written, 3 the writer does not make the version odd before it writes, 4 the site
// words are not compared, 5 a template marked "no prediction" is used, 6 an unknown uniform address predicts offset 0, 7 the offset is
// not checked against 32 bits, 8 the base vertex fold k is not added, 9 the writer skips a template that differs only in its last word.
#ifndef NRZ5P_MUTATE
#define NRZ5P_MUTATE 0
#endif

enum { NRZ5T_MAX_DYN = 8, NRZ5T_MAX_VB = 8, NRZ5T_SITE_WORDS = 6,
       NRZ5T_F_OK = 1,          // the template predicts (else: the translation thread could not describe this draw - never predicted)
       NRZ5S_IDX = 1, NRZ5S_ARRAYS = 2 };

// What the game thread has in hand of an engine draw record (all of it is in the record, so the translation thread builds the same
// words when it publishes): the engine's key id, the draw kind and arguments, the index buffer, the address bound to vertex stream 0.
typedef struct nrz5_site { uint64_t w[NRZ5T_SITE_WORDS]; } nrz5_site;
static inline void nrz5_site_make(nrz5_site *s, uint32_t key, unsigned kind, int32_t prim, int32_t itype, int32_t count, int32_t base_vertex, int32_t base_instance,
                                  int32_t instances, int32_t first, uint64_t ibuf, uint64_t vb0) {
    s->w[0] = (uint64_t)key | (uint64_t)(uint32_t)prim << 32;
    s->w[1] = (uint64_t)(uint32_t)itype | (uint64_t)(uint32_t)count << 32;
    s->w[2] = (uint64_t)(uint32_t)base_vertex | (uint64_t)(uint32_t)base_instance << 32;
    s->w[3] = (uint64_t)(uint32_t)instances | (uint64_t)(uint32_t)first << 32;
    s->w[4] = ibuf;
    s->w[5] = vb0 ^ (uint64_t)kind << 62;   // (guest addresses stay below 2^48)
}
static inline uint64_t nrz5_site_hash(const nrz5_site *s, uint32_t occ) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ occ;
    for (int i = 0; i < NRZ5T_SITE_WORDS; i++) { h = (h ^ s->w[i]) * 0xBF58476D1CE4E5B9ull; h ^= h >> 29; }
    return h;
}

// The template of a slot: plain words (copied in and out of the table as a block of 64-bit words).
typedef struct nrz5_tmpl {
    uint64_t site[NRZ5T_SITE_WORDS];
    uint64_t pipe, layout, set;
    uint64_t delta[NRZ5T_MAX_DYN];                 // dynamic offset i = (address of uniform slot dyn_st[i] / dyn_sl[i]) - delta[i]
    uint64_t vb[NRZ5T_MAX_VB], vbo[NRZ5T_MAX_VB];  // the j-th used binding (vb_at[j]) as the tail binds it (folded)
    uint64_t ib, ibo;
    uint8_t vp[NRZ5_VP_BYTES], sc[NRZ5_SC_BYTES];
    uint32_t it, first_index, k, flags;            // k: vertices folded into the draw (base vertex / first vertex + k)
    uint16_t used; uint8_t noff, nvb, indexed, pad[3];
    uint8_t dyn_st[NRZ5T_MAX_DYN], dyn_sl[NRZ5T_MAX_DYN], vb_at[NRZ5T_MAX_VB];
} nrz5_tmpl;
enum { NRZ5T_WORDS = (int)(sizeof(nrz5_tmpl) / 8) };
static_assert(sizeof(nrz5_tmpl) % 8 == 0 && NRZ5T_MAX_VB <= 16, "nr_z5p.h: a template is a block of 64-bit words");

// The translation thread describes the draw it just recorded. v: the values its tail bound (nr_z5.h); first_index / k: the folds;
// ub(k, &st, &sl, &addr): the uniform slot dynamic offset k comes from and the address bound there (false: not known).
// Always fills *t (zeroed first, so equal draws give equal bytes); flags = 0 when the draw cannot be a template - more dynamic offsets
// or vertex bindings than a template holds, an offset whose slot is not known or lies above its address.
template <class Ub> static inline void nrz5_tmpl_make(nrz5_tmpl *t, const nrz5_site *s, const nrz5_vals *v, uint32_t first_index, uint32_t k, Ub &&ub) {
    memset(t, 0, sizeof *t); memcpy(t->site, s->w, sizeof t->site);
    unsigned nvb = 0; for (uint32_t m = v->used; m; m &= m - 1) ++nvb;
    if (v->noff > NRZ5T_MAX_DYN || nvb > NRZ5T_MAX_VB) return;
    t->pipe = v->pipe; t->layout = v->layout; t->set = v->set; t->noff = (uint8_t)v->noff;
    for (uint32_t i = 0; i < v->noff; i++) {
        unsigned st = 0, sl = 0; uint64_t a = 0;
        if (!ub(i, &st, &sl, &a) || !a || a < v->off[i] || st > 255 || sl > 255) { const nrz5_site keep = *s; memset(t, 0, sizeof *t); memcpy(t->site, keep.w, sizeof t->site); return; }
        t->dyn_st[i] = (uint8_t)st; t->dyn_sl[i] = (uint8_t)sl; t->delta[i] = a - v->off[i];
    }
    memcpy(t->vp, v->vp, sizeof t->vp); memcpy(t->sc, v->sc, sizeof t->sc);
    t->used = v->used; t->nvb = (uint8_t)nvb;
    { unsigned j = 0; for (uint32_t m = v->used; m; m &= m - 1, j++) { const uint32_t i = (uint32_t)__builtin_ctz(m); t->vb_at[j] = (uint8_t)i; t->vb[j] = v->vb[i]; t->vbo[j] = v->vbo[i]; } }
    t->indexed = v->indexed ? 1 : 0;
    if (v->indexed) { t->ib = v->ib; t->ibo = v->ibo; t->it = v->it; t->first_index = first_index; }
    t->k = k; t->flags = NRZ5T_F_OK;
}

// The game thread: the values of the record it is writing. count / instances / first / base_vertex / base_instance: the record's
// arguments (an indexed draw: first is unused; a plain one: instances = 1, base_instance = 0 - as the backend's draw record).
// ub(st, sl): the address the thread last bound to that uniform slot, 0 = it does not know. off: room for NRZ5T_MAX_DYN offsets
// (v->off points at it). false = no prediction.
template <class Ub> static inline bool nrz5p_vals(const nrz5_tmpl *t, const nrz5_site *s, int32_t count, int32_t instances, int32_t first, int32_t base_vertex, int32_t base_instance,
                                                 Ub &&ub, nrz5_vals *v, uint32_t *off) {
    if (!(t->flags & NRZ5T_F_OK) && NRZ5P_MUTATE != 5) return false;
    if (NRZ5P_MUTATE != 4 && memcmp(t->site, s->w, sizeof t->site)) return false;
    if (t->noff > NRZ5T_MAX_DYN || t->nvb > NRZ5T_MAX_VB) return false;   // (never from a whole template)
    memset(v, 0, sizeof *v);
    v->pipe = t->pipe; v->layout = t->layout; v->set = t->set; v->noff = t->noff; v->off = off;
    for (unsigned i = 0; i < t->noff; i++) {
        const uint64_t a = ub(unsigned(t->dyn_st[i]), unsigned(t->dyn_sl[i]));
        if (!a) { if (NRZ5P_MUTATE == 6) { off[i] = 0; continue; } return false; }
        if (a < t->delta[i]) return false;
        const uint64_t o = a - t->delta[i];
        if (o > 0xffffffffull && NRZ5P_MUTATE != 7) return false;
        off[i] = (uint32_t)o;
    }
    memcpy(v->vp, t->vp, sizeof v->vp); memcpy(v->sc, t->sc, sizeof v->sc);
    v->used = t->used;
    for (unsigned j = 0; j < t->nvb; j++) { const unsigned i = t->vb_at[j] & 15u; v->vb[i] = t->vb[j]; v->vbo[i] = t->vbo[j]; }
    v->indexed = t->indexed; v->count = (uint32_t)count;
    const uint32_t k = NRZ5P_MUTATE == 8 ? 0u : t->k;
    if (t->indexed) { v->it = t->it; v->ib = t->ib; v->ibo = t->ibo; v->instances = (uint32_t)instances; v->first = t->first_index; v->vertex_offset = (int32_t)((uint32_t)base_vertex + k); v->first_instance = (uint32_t)base_instance; }
    else { v->instances = 1; v->first = (uint32_t)first + k; v->first_instance = 0; }
    return true;
}

namespace nrz5p {

// One writer (the translation thread), any number of readers. A slot's version: 0 = never written, odd = being written.
struct Slot { std::atomic<uint32_t> ver; std::atomic<uint64_t> w[NRZ5T_WORDS]; };
struct Table {
    Slot *s = nullptr; uint32_t n = 0;                                   // n: a power of two
    uint64_t puts = 0, same = 0;                                         // writer's counters
    bool init(uint32_t slots) {
        while (slots & (slots - 1)) slots &= slots - 1;
        if (!slots) return false;
        void *m = malloc(sizeof(Slot) * (size_t)slots); if (!m) return false;
        s = static_cast<Slot *>(m); n = slots;
        for (uint32_t i = 0; i < n; i++) { new (&s[i]) Slot; s[i].ver.store(0, std::memory_order_relaxed); for (int j = 0; j < NRZ5T_WORDS; j++) s[i].w[j].store(0, std::memory_order_relaxed); }
        return true;
    }
    // Never freed while a reader may be inside it (the engine keeps the table for the life of the process).
    void destroy() { free(static_cast<void *>(s)); s = nullptr; n = 0; }
    uint32_t slot_of(const nrz5_site *site, uint32_t occ) const { return (uint32_t)(nrz5_site_hash(site, occ) >> 20) & (n - 1); }
    // writer: publish t in slot i unless the slot already holds these words. true = written.
    bool put(uint32_t i, const nrz5_tmpl *t) {
        Slot &x = s[i & (n - 1)]; uint64_t w[NRZ5T_WORDS]; memcpy(w, t, sizeof w);
        const uint32_t v = x.ver.load(std::memory_order_relaxed);
        if (v) { bool eq = true; for (int j = 0; j < (NRZ5P_MUTATE == 9 ? NRZ5T_WORDS - 1 : NRZ5T_WORDS) && eq; j++) eq = x.w[j].load(std::memory_order_relaxed) == w[j]; if (eq) { ++same; return false; } }
        if (NRZ5P_MUTATE != 3) x.ver.store(v + 1, std::memory_order_release);
        for (int j = 0; j < NRZ5T_WORDS; j++) x.w[j].store(w[j], std::memory_order_release);
        x.ver.store(v + 2, std::memory_order_release); ++puts;
        return true;
    }
    // reader: the template of slot i as one consistent block, and its version. false = empty, being written, or written meanwhile.
    bool get(uint32_t i, nrz5_tmpl *t, uint32_t *ver) const {
        const Slot &x = s[i & (n - 1)];
        const uint32_t v0 = x.ver.load(std::memory_order_acquire);
        if (!v0 || ((v0 & 1) && NRZ5P_MUTATE != 2)) return false;
        uint64_t w[NRZ5T_WORDS]; for (int j = 0; j < NRZ5T_WORDS; j++) w[j] = x.w[j].load(std::memory_order_acquire);
        if (NRZ5P_MUTATE != 1 && x.ver.load(std::memory_order_acquire) != v0) return false;
        memcpy(t, w, sizeof w); if (ver) *ver = v0;
        return true;
    }
};

} // namespace nrz5p
#endif
