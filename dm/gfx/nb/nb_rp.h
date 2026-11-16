// [dos2-nb] NB-06: the planner's per-pass decision (OP_X_NB_PASS header, nvn_obj.h p_nb_pass) mapped onto the attachments of the host's
// Vulkan pass (docs/dos2/NATIVE_BACKEND_PLAN.md §4, "NB-06 findings"). Pure C, inline, no Vulkan: the backend (xbg_nb.inc, DM_NB_RP) and
// the host test tests/nb_rp_test.cpp call the same code.
//
// The header lists the IR pass's attachments (image id = texture storage address) with load / store by IR attachment; the host's pass has
// its own attachment order. nb_rp_map matches them by image id: the same number of attachments, every host one found exactly once in
// the header (else NB_RP_ATT: the caller falls back to its heuristics for that pass). The result is bit masks by HOST attachment index.
// nb_rp_twin turns a decision plus what the host can do into the host's twin render-pass mask (xbg_game.cpp lazy_twin):
//   bits 2k / 2k+1  loadOp / stencilLoadOp CLEAR of attachment k  - only where the host holds the clear value (its pending clear: a
//                   ClearTexture deferred or a clear of the lazily begun pass); a plan CLEAR without one = LOAD (the host has already
//                   executed that clear: same contents), counted as clear_missing; a host clear the plan did not fold is still folded
//                   (the clear must happen; as a load op it is the same contents), counted as clear_extra
//   bit 18+k        storeOp + stencilStoreOp DONT_CARE (plan store DONT_CARE)
//   bit 32+k        loadOp / stencilLoadOp DONT_CARE where not CLEAR (plan load DONT_CARE, or plan LOAD inside cover_dc when the host
//                   proved the first draw covers the pass: first_draw_covers)
#ifndef DM_GFX_NB_RP_H
#define DM_GFX_NB_RP_H
#include <stdint.h>
#include "nb_ir.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { NB_RP_OK = 0, NB_RP_NO_PLAN, NB_RP_ATT, NB_RP_KINDS };
typedef struct nb_rp_dec {
    uint32_t clear;      // bit k: plan load CLEAR (host attachment k)
    uint32_t dc;         // bit k: plan load DONT_CARE
    uint32_t cover;      // bit k: plan LOAD that becomes DONT_CARE with a coverage proof of the first draw (header cover_dc)
    uint32_t store_dc;   // bit k: plan store DONT_CARE
    uint8_t flags;       // NB_NP_* of the IR pass
    uint8_t n;           // host attachments
} nb_rp_dec;
typedef struct nb_rp_twin_out {
    uint64_t mask;       // twin mask (bits above); 0 = the pass as created
    uint32_t dc;         // attachments begun with load DONT_CARE (poison targets of DM_NB_RP=3)
    uint32_t clear_missing, clear_extra;   // by attachment (counts)
} nb_rp_twin_out;

// hn / hatt / hload / hstore / cover_dc / flags: the header (hn clamped to 9, never read past it); n / att: the host pass.
static inline int nb_rp_map(int plan_ok, unsigned hn, const uint64_t *hatt, const uint8_t *hload, const uint8_t *hstore, uint16_t cover_dc,
                            uint8_t flags, unsigned n, const uint64_t *att, nb_rp_dec *d) {
    d->clear = d->dc = d->cover = d->store_dc = 0; d->flags = flags; d->n = (uint8_t)(n > NB_MAX_ATT ? NB_MAX_ATT : n);
    if (!plan_ok) return NB_RP_NO_PLAN;
    if (hn > NB_MAX_ATT) hn = NB_MAX_ATT;
    if (n != hn || n > NB_MAX_ATT) return NB_RP_ATT;
    for (unsigned k = 0; k < n; k++) {
        int j = -1;
        if (!att[k]) return NB_RP_ATT;
        for (unsigned i = 0; i < hn; i++) if (hatt[i] == att[k]) { if (j >= 0) return NB_RP_ATT; j = (int)i; }   // duplicate = ambiguous
        if (j < 0) return NB_RP_ATT;
        for (unsigned i = 0; i < k; i++) if (att[i] == att[k]) return NB_RP_ATT;
        const unsigned b = 1u << k;
        if (hload[j] == NB_LOAD_CLEAR) d->clear |= b;
        else if (hload[j] == NB_LOAD_DONT_CARE) d->dc |= b;
        else if (hload[j] == NB_LOAD && (cover_dc >> j & 1u)) d->cover |= b;
        if (hstore[j] == NB_STORE_DONT_CARE) d->store_dc |= b;
    }
    return NB_RP_OK;
}
// have_clear: the host's pending clear values, 2 bits per attachment (2k colour / depth, 2k+1 stencil); covered: first_draw_covers said yes.
static inline nb_rp_twin_out nb_rp_twin(const nb_rp_dec *d, uint32_t have_clear, int covered) {
    nb_rp_twin_out o; o.mask = have_clear; o.dc = 0; o.clear_missing = o.clear_extra = 0;
    for (unsigned k = 0; k < d->n; k++) {
        const unsigned b = 1u << k; const int has = (have_clear >> (2 * k) & 3u) != 0;
        if ((d->clear & b) && !has) o.clear_missing++;
        if (!(d->clear & b) && has) o.clear_extra++;
        if (d->store_dc & b) o.mask |= 1ull << (18 + k);
        if (!has && ((d->dc & b) || (covered && (d->cover & b)))) { o.mask |= 1ull << (32 + k); o.dc |= b; }
    }
    return o;
}
// keep: host attachments whose store must stay STORE whatever the plan says (images the host banned after a store-hist violation,
// xbg_store_hist.h). Clears their store DONT_CARE bits (18+k) in t->mask; returns how many were cleared.
static inline unsigned nb_rp_keep_store(nb_rp_twin_out *t, uint32_t keep) {
    unsigned n = 0;
    for (unsigned k = 0; k < NB_MAX_ATT; k++)
        if ((keep >> k & 1u) && (t->mask >> (18 + k) & 1u)) { t->mask &= ~(1ull << (18 + k)); n++; }
    return n;
}
static inline char nb_rp_load_char(unsigned load) { return load == NB_LOAD_CLEAR ? 'C' : load == NB_LOAD_DONT_CARE ? 'D' : 'L'; }

#ifdef __cplusplus
}
#endif
#endif
