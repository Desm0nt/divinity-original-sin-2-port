// [DOS2] NB-11: DM_NB_DESC - an OP_X_NB_DRAW reuses the previous draw's descriptor set without building the memo key
// (docs/dos2/NATIVE_BACKEND_PLAN.md, "NB-11 findings"). Pure C (C and C++): the decision the backend makes (xbg_game.cpp namespace
// nbdesc, xbg_nb.inc nbdraw::apply) and the host test dm/gfx/vulkan/engine/tests/nb_desc_test.c.
//
// The memo key of descriptors() (xbg_game.cpp) is built from: the 5 graphics shaders, the texture pool (memory, offset, count), the
// sampler pool (memory, offset), and per binding of those shaders: uniform / storage buffer {address, size} (a dynamic uniform buffer
// with address and size != 0: only min(size, binding bytes) - its address moves the dynamic offset only), the support block (viewport
// size, alpha func), a non-dynamic driver block (a generation counter), texture / image / sampler handles. The reuse is sound when every
// one of those words is the one the previous descriptors() call keyed with, and the memo still holds that call's entry:
//   (1) nb_desc_pre, before the record's binds land: the backend's tex_gen[6] / buf_gen[6] equal the values at the previous
//       descriptors() call - no bind record (OP_BIND_*, a foreign record of the bracket, NB-18b-4 fused-draw swaps) and no other
//       OP_X_NB_DRAW touched the tables since (every writer of state.tex / samp / img / ub / sb bumps them: exec_bind_buf,
//       exec_bind_handles, nbdraw::apply, nbpost pre / post); then every bind of the record either writes the value already there
//       (texture / sampler / image: any changed handle blocks, read by the shaders or not), or is a uniform buffer the previous key did
//       not read, or a dynamic one whose new range keeps the key word (address != 0, same size), or a non-dynamic one with the same
//       {address, size}.
//   (2) nb_desc_post, after the binds: the stages whose counters the record bumped are not keyed by a counter (non-dynamic driver
//       blocks of that stage).
//   (3) nb_desc_guard, in descriptors(): the same shaders, pools, viewport / alpha func, the counters exactly as the record left them
//       (nothing ran between the record and descriptors() - a fused post draw bumps them), the same memo generation and slot.
// Not in the key and not needed: the render pass (sets bind images in GENERAL layout and no attachment), image layouts and barriers
// (prepare_read runs on the hit path for every image of the entry, reuse or not), uniform buffer contents (dynamic ones are
// re-snapshotted on every hit; the hit path is the same), registration revisions (checked by the hit path as before).
#ifndef DM_NB_DESC_H
#define DM_NB_DESC_H
#include <stdint.h>
#include <string.h>
#include "../nvn/nvn_nb_draw.h"

// Why a record / draw is not reused (counters of the "[nb] desc" line).
enum {
    NBDS_OK = 0,
    NBDS_PREV,     // the previous descriptors() call did not end on a memo entry (miss without memo, compute, no memo for its shaders)
    NBDS_GEN,      // tex_gen / buf_gen moved since the previous descriptors() call (bind records, another record, fused draw)
    NBDS_TEX,      // the record changes a texture / sampler / image handle
    NBDS_USTAT,    // the record changes a non-dynamic uniform buffer range the key reads
    NBDS_USIZE,    // the record changes the size (or zeroes the address) of a dynamic uniform buffer the key reads
    NBDS_LIMIT,    // a bind outside the limits (nbd_apply fails the record anyway)
    NBDS_DEP,      // the record bumped a counter the key reads (non-dynamic driver block of that stage)
    NBDS_SHADER,   // descriptors(): other shaders
    NBDS_POOL,     // other texture / sampler pool
    NBDS_SUPPORT,  // other viewport size / alpha func
    NBDS_LATE,     // the counters moved between the record and descriptors()
    NBDS_MEMO,     // the memo changed (insert / erase / epoch / submission reset) or another slot
    NBDS_GATE,     // the memo is off for this draw (capture, provenance, UI offset, pass graph)
    NBDS_N
};

// What the previous descriptors() call keyed with, as far as a record can change it.
typedef struct {
    uint64_t tex_gen[6], buf_gen[6];   // backend counters at that call
    uint16_t ustat[6], udyn[6];        // uniform buffer slots read: {address, size} in the key / dynamic (size only)
    uint8_t dep;                       // stages whose counter is a key word (non-dynamic driver block)
} nb_desc_snap;
// The identity words compared in descriptors() (5 shaders, 5 pool words, viewport w << 32 | h, alpha func).
typedef struct { uint64_t sh[5], pool[5], vp, alpha; } nb_desc_ident;

// (1) Before nbd_apply. *texb = the record has a texture / sampler / image bind (for the FE generation census). Returns NBDS_*.
// The counters of a snapshot (a caller whose own names tex_gen / buf_gen are macros cannot name the fields).
static inline void nb_desc_snap_gens(nb_desc_snap *p, const uint64_t tg[6], const uint64_t bg[6]) { memcpy(p->tex_gen, tg, sizeof p->tex_gen); memcpy(p->buf_gen, bg, sizeof p->buf_gen); }
static inline int nb_desc_pre(const nb_desc_snap *p, const uint64_t tex_gen[6], const uint64_t buf_gen[6], const nbd_hdr *h,
                              const uint8_t *binds, const uint64_t (*tex)[128], const uint64_t (*samp)[128], const uint64_t (*img)[16],
                              const nbd_range (*ub)[16], int *texb) {
    *texb = 0;
    if (memcmp(tex_gen, p->tex_gen, sizeof p->tex_gen) || memcmp(buf_gen, p->buf_gen, sizeof p->buf_gen)) return NBDS_GEN;
    int why = NBDS_OK;
    for (unsigned i = 0; i < h->nbind && why == NBDS_OK; i++) {
        nbd_bind b; memcpy(&b, binds + sizeof b * i, sizeof b);
        const unsigned st = b.stage, sl = b.slot;
        if (st >= 6) return NBDS_LIMIT;
        switch (b.kind) {
        case NBD_B_TEX:  *texb = 1; if (sl >= 128) return NBDS_LIMIT; if (tex[st][sl] != b.a) why = NBDS_TEX; break;
        case NBD_B_SAMP: *texb = 1; if (sl >= 128) return NBDS_LIMIT; if (samp[st][sl] != b.a) why = NBDS_TEX; break;
        case NBD_B_IMG:  *texb = 1; if (sl >= 16) return NBDS_LIMIT; if (img[st][sl] != b.a) why = NBDS_TEX; break;
        case NBD_B_UBO: {
            if (sl >= 16) return NBDS_LIMIT;
            const nbd_range r = ub[st][sl];
            if ((p->ustat[st] >> sl & 1) && (r.addr != b.a || r.size != b.b)) why = NBDS_USTAT;
            else if ((p->udyn[st] >> sl & 1) && (!b.a || !r.addr || !r.size || r.size != b.b)) why = NBDS_USIZE;
            break; }
        default: return NBDS_LIMIT;
        }
    }
    for (unsigned i = 0; i < h->nbind && !*texb; i++) { nbd_bind b; memcpy(&b, binds + sizeof b * i, sizeof b); *texb = b.kind != NBD_B_UBO; }
    return why;
}
// (2) After nbd_apply: bumped = stages whose tex_gen or buf_gen the record bumped.
static inline int nb_desc_post(const nb_desc_snap *p, unsigned bumped) { return (bumped & p->dep) ? NBDS_DEP : NBDS_OK; }
// (3) descriptors(): prev / now identity, the counters now against the ones the record left (etex / ebuf), memo generation / slot.
static inline int nb_desc_guard(const nb_desc_ident *prev, const nb_desc_ident *now, const uint64_t tex_gen[6], const uint64_t buf_gen[6],
                                const uint64_t etex[6], const uint64_t ebuf[6], uint64_t memo_gen, uint64_t prev_memo_gen,
                                unsigned slot, unsigned prev_slot) {
    if (memo_gen != prev_memo_gen || slot != prev_slot) return NBDS_MEMO;
    if (memcmp(prev->sh, now->sh, sizeof now->sh)) return NBDS_SHADER;
    if (memcmp(prev->pool, now->pool, sizeof now->pool)) return NBDS_POOL;
    if (prev->vp != now->vp || prev->alpha != now->alpha) return NBDS_SUPPORT;
    if (memcmp(tex_gen, etex, 6 * sizeof *etex) || memcmp(buf_gen, ebuf, 6 * sizeof *ebuf)) return NBDS_LATE;
    return NBDS_OK;
}
#endif
