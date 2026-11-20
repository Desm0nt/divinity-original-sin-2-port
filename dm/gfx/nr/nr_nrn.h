// [DOS2] NR-NATIVE (docs/dos2/nr/NR-NATIVE.md): the draw as the ENGINE holds it, above NVN. Pure C (C and C++, no NVN / Vulkan /
// engine type, no environment): the front end dm/host/hle_dos2_nrn.inc, the backend dm/gfx/vulkan/engine/xbg_nr_nrn.inc and the host
// test dm/gfx/vulkan/engine/tests/nr_nrn_test.c include it.
//
// The engine's renderer r ([main+0x2bbdc68]) keeps the folded state of the draw it is about to make - its own cache, the one the Switch
// build compares against before it calls NVN (r+0x16f0..0x1870: raster / blend / depth-stencil handles, stencil ref, primitive, vertex
// format, the vertex buffer handles, the index buffer handle, the shader handle, the colour targets and the depth target, alpha ref;
// r+0x13c0..0x1660: sampler and texture ids of the vertex and fragment slots). Those words ARE the engine's render objects of the draw:
// mesh (vertex format + vertex / index buffers), material (shader + state objects + texture set), pass (target set). The translation
// path learns the same thing by folding ~7 000 records a frame into a 17 KB State and looking the result up again.
//   card      one 64-bit word of those bytes (nrn_card_of). Two draws with the same card were asked for with the same engine objects.
//   pass      which pass of the frame the draw belongs to, from the NAMES the engine gives its targets (opcode 55, "DeferredBufferStage::
//             GBuffer1", ...): nrn_name_class / nrn_pass_of. The switches are per pass: DM_NRN_SHADOW / _PREPASS / _GBUF / _LIGHT / _POST /
//             _OTHER (0 off = the record stream byte for byte as before, 2 verify, 1 apply - refused until the replay is built).
//   the tail  16 bytes after the NR-09 tail of the draw record (NBD_F_NRN): card, pass, a sequence number.
//   the table what the translation path recorded for a card (pipeline, set layout, set, vertex / index buffers, viewport ...): the
//             verify of =2 - "the engine's objects decide the Vulkan draw" - is this table noting every recorded draw and classing
//             every difference (nrn_tab_note): a word that must be a function of the card (pipeline, layout, bindings, index type:
//             BAD when it differs), a word that follows a resource the backend may move (set, buffers: MOVED - what an epoch has to
//             cover in the replay), viewport / scissor (their packets are not in the card: counted apart).
#ifndef DM_NR_NRN_H
#define DM_NR_NRN_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The mutation switch of the host test (never defined in the engine build): NRN_MUTATE = k - 1 the card does not read the shader handle,
// 2 the card does not read the texture slots, 3 the table does not compare the pipeline, 4 a differing entry is not replaced, 5 a
// depth-only draw into a shadow target is called the pre-pass, 6 the tail is read 8 bytes early, 7 the volatile bytes of the renderer
// (conditional render, dirty masks) are read into the card, 8 a missing sequence number is not counted.
#ifndef NRN_MUTATE
#define NRN_MUTATE 0
#endif

// ---- passes ----
enum { NRN_P_SHADOW, NRN_P_PREPASS, NRN_P_GBUF, NRN_P_LIGHT, NRN_P_POST, NRN_P_OTHER, NRN_P_N };
static inline const char *nrn_pass_name(int p) {
    static const char *const k[NRN_P_N] = {"SHADOW", "PREPASS", "GBUF", "LIGHT", "POST", "OTHER"};
    return p >= 0 && p < NRN_P_N ? k[p] : "?";
}
// Modes of the passes from the environment: DM_NRN (all passes) then DM_NRN_<PASS>. get = getenv; def = the policy's default (0).
// A value outside 0..2 is 0 (the caller prints the red line: *bad gets a bit per pass, bit NRN_P_N for DM_NRN itself).
static inline unsigned nrn_env_modes(const char *(*get)(const char *), int def, uint8_t mode[NRN_P_N], unsigned *bad) {
    unsigned any = 0, b = 0;
    int all = def;
    const char *e = get("DM_NRN");
    if (e && *e) { all = atoi(e); if (all < 0 || all > 2) { all = 0; b |= 1u << NRN_P_N; } }
    for (int p = 0; p < NRN_P_N; p++) {
        char k[32] = "DM_NRN_"; const char *nm = nrn_pass_name(p); memcpy(k + 7, nm, strlen(nm) + 1);
        int m = all;
        e = get(k);
        if (e && *e) { m = atoi(e); if (m < 0 || m > 2) { m = 0; b |= 1u << p; } }
        mode[p] = (uint8_t)m; any |= (unsigned)m;
    }
    if (bad) *bad = b;
    return any;
}

// Class of a target by the name the engine gave it in a resource transition (opcode 55: "<Stage>::<Resource>").
enum { NRN_RC_NONE, NRN_RC_SHADOW, NRN_RC_GBUF, NRN_RC_HDR, NRN_RC_LDR, NRN_RC_AUX, NRN_RC_NAMED, NRN_RC_N };
static inline int nrn_name_class(const char *s) {
    if (!s || !*s) return NRN_RC_NONE;
    const char *r = strstr(s, "::");
    r = r ? r + 2 : s;
    if (strstr(r, "ShadowDepth") || strstr(r, "LocalLightShadowBuffer")) return NRN_RC_SHADOW;   // DirectionalLightShadowDepth, LocalLightShadowDepth / Buffer / BufferCube
    if (!strncmp(r, "GBuffer", 7)) return NRN_RC_GBUF;
    if (!strncmp(r, "HDRFullBuffer", 13)) return NRN_RC_HDR;
    if (!strncmp(r, "LDRFullBuffer", 13)) return NRN_RC_LDR;
    if (strstr(r, "ShadowResult") || !strncmp(r, "m_RT_", 5)) return NRN_RC_AUX;                  // the PCF resolve, SSAO's own targets
    return NRN_RC_NAMED;
}
// An engine handle that names no target (the renderer holds 0 or all ones in an unused slot).
static inline int nrn_h_none(uint64_t h) { return !h || (uint32_t)h == 0xffffffffu; }
// The pass of a draw: class of colour target 0 (NRN_RC_NONE when the draw has no colour target), class of the depth target.
static inline int nrn_pass_of(int have_color, int c0, int ds) {
    if (!have_color) return ds == NRN_RC_SHADOW && NRN_MUTATE != 5 ? NRN_P_SHADOW : NRN_P_PREPASS;
    switch (c0) {
    case NRN_RC_SHADOW: return NRN_P_SHADOW;   // a local light's shadow buffer is a colour target
    case NRN_RC_GBUF: return NRN_P_GBUF;
    case NRN_RC_HDR: return NRN_P_LIGHT;       // lighting, sky, fog, the forward draws: everything the engine draws into the HDR buffers
    case NRN_RC_LDR: case NRN_RC_AUX: return NRN_P_POST;
    default: return NRN_P_OTHER;
    }
}
// handle -> class, written by the thread that executes the engine's command lists, read by the draw on the same thread (one word an
// entry: the handle's 54 bits and the class above them, so a reader never sees half an entry).
enum { NRN_RT_SLOTS = 1024 };
typedef struct nrn_rtmap { uint64_t w[NRN_RT_SLOTS]; } nrn_rtmap;
static inline void nrn_rt_put(nrn_rtmap *m, uint64_t h, int cls) {
    if (nrn_h_none(h)) return;
    const uint64_t v = (h & 0x003fffffffffffffull) | (uint64_t)cls << 56;
    __atomic_store_n(&m->w[(uint32_t)h & (NRN_RT_SLOTS - 1)], v, __ATOMIC_RELAXED);
}
static inline int nrn_rt_get(const nrn_rtmap *m, uint64_t h) {   // NRN_RC_NONE: never named (or pushed out by another handle of the slot)
    if (nrn_h_none(h)) return NRN_RC_NONE;
    const uint64_t v = __atomic_load_n(&m->w[(uint32_t)h & (NRN_RT_SLOTS - 1)], __ATOMIC_RELAXED);
    return (v & 0x003fffffffffffffull) == (h & 0x003fffffffffffffull) ? (int)(v >> 56) : NRN_RC_NONE;
}

// ---- the card ----
// Offsets in the renderer (main.raw of update v786432, build 4979b200; nr_state.h NRST_R_*, hle_dos2_nr_key.inc NK_R_*).
enum { NRN_R_SMP_VS = 0x13c0, NRN_R_SMP_FS = 0x1448, NRN_R_TEX_VS = 0x1558, NRN_R_TEX_FS = 0x15e0, NRN_SLOTS = 32,
       NRN_R_LO = 0x16f0, NRN_R_HI = 0x1870, NRN_R_VB = 0x1748, NRN_R_VB_DIRTY = 0x1788, NRN_R_IB = 0x17c8, NRN_R_IB_FIRST = 0x17d0,
       NRN_R_SHADER = 0x17d8, NRN_R_SHADER_REC = 0x17e0, NRN_R_RT = 0x17e8, NRN_R_DEPTH_RT = 0x1828, NRN_R_REN = 0x185c, NRN_R_ALPHA = 0x1868 };
// What the card reads besides the fixed state words (DM_NRN_WORDS, an experiment's mask; the default is NRN_W_DEFAULT):
//   TEX / SMP  the texture / sampler ids of the 2 x 32 slots (the material's texture set)
//   VBDIRTY    r+0x1788..0x17c8 (per-stream words next to the vertex buffer handles; the Switch build's dirty bookkeeping)
//   IBFIRST    r+0x17d0 (first index of the bound index buffer: a draw argument, not an object)
//   REN        r+0x185c..0x1868 (conditional rendering: the query of this very draw)
//   SHREC      r+0x17e0 (pointer to the shader's record: a function of the shader handle)
enum { NRN_W_TEX = 1, NRN_W_SMP = 2, NRN_W_VBDIRTY = 4, NRN_W_IBFIRST = 8, NRN_W_REN = 16, NRN_W_SHREC = 32, NRN_W_DEFAULT = NRN_W_TEX | NRN_W_SMP };
static inline uint64_t nrn_mix(uint64_t h, uint64_t w) { h = (h ^ w) * 0x9e3779b97f4a7c15ull; return h ^ (h >> 29); }
static inline uint64_t nrn_mix_bytes(uint64_t h, const uint8_t *p, size_t n) {   // n: a multiple of 8
    for (size_t i = 0; i < n; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h = nrn_mix(h, w); }
    return h;
}
// R = the renderer. Never 0 (0 = "no card").
static inline uint64_t nrn_card_of(const uint8_t *R, unsigned words) {
    uint8_t st[NRN_R_HI - NRN_R_LO];
    memcpy(st, R + NRN_R_LO, sizeof st);
    if (NRN_MUTATE == 7) words |= NRN_W_VBDIRTY | NRN_W_REN | NRN_W_IBFIRST;
    if (!(words & NRN_W_VBDIRTY)) memset(st + (NRN_R_VB_DIRTY - NRN_R_LO), 0, NRN_R_IB - NRN_R_VB_DIRTY);
    if (!(words & NRN_W_IBFIRST)) memset(st + (NRN_R_IB_FIRST - NRN_R_LO), 0, NRN_R_SHADER - NRN_R_IB_FIRST);
    if (!(words & NRN_W_SHREC)) memset(st + (NRN_R_SHADER_REC - NRN_R_LO), 0, NRN_R_RT - NRN_R_SHADER_REC);
    if (!(words & NRN_W_REN)) memset(st + (NRN_R_REN - NRN_R_LO), 0, NRN_R_ALPHA - NRN_R_REN);
    if (NRN_MUTATE == 1) memset(st + (NRN_R_SHADER - NRN_R_LO), 0, 8);
    uint64_t h = nrn_mix_bytes(0x6e726e5f63617264ull, st, sizeof st);
    if ((words & NRN_W_TEX) && NRN_MUTATE != 2) { h = nrn_mix_bytes(h, R + NRN_R_TEX_VS, 4 * NRN_SLOTS); h = nrn_mix_bytes(h, R + NRN_R_TEX_FS, 4 * NRN_SLOTS); }
    if (words & NRN_W_SMP) { h = nrn_mix_bytes(h, R + NRN_R_SMP_VS, 4 * NRN_SLOTS); h = nrn_mix_bytes(h, R + NRN_R_SMP_FS, 4 * NRN_SLOTS); }
    h ^= h >> 32;
    return h ? h : 1;
}
// The pass of the draw the renderer is about to make.
static inline int nrn_pass_of_r(const uint8_t *R, const nrn_rtmap *m) {
    uint64_t c0, ds; memcpy(&c0, R + NRN_R_RT, 8); memcpy(&ds, R + NRN_R_DEPTH_RT, 8);
    return nrn_pass_of(!nrn_h_none(c0), nrn_rt_get(m, c0), nrn_rt_get(m, ds));
}

// ---- the tail of the draw record (after nrd_tail; the record's header carries NBD_F_NRN = 32) ----
enum { NRN_VER = 1, NRN_F_FLAG = 32, NRN_TAIL_BYTES = 16 };
typedef struct nrn_tail { uint64_t card; uint8_t ver, pass; uint16_t rsv; uint32_t seq; } nrn_tail;
// at = the byte right after the NR-09 tail; cap / n = bytes of the record's payload.
static inline int nrn_tail_put(uint8_t *dst, size_t cap, size_t at, uint64_t card, int pass, uint32_t seq) {
    if (!dst || at + sizeof(nrn_tail) > cap) return 0;
    nrn_tail t; memset(&t, 0, sizeof t);
    t.card = card; t.ver = NRN_VER; t.pass = (uint8_t)pass; t.seq = seq;
    memcpy(dst + at, &t, sizeof t);
    return 1;
}
static inline int nrn_tail_get(const uint8_t *p, size_t n, size_t at, unsigned flags, nrn_tail *out) {
    if (NRN_MUTATE == 6) at -= 8;
    if (!p || !(flags & NRN_F_FLAG) || at + sizeof(nrn_tail) > n) return 0;
    memcpy(out, p + at, sizeof *out);
    return out->ver == NRN_VER && out->card && out->pass < NRN_P_N;
}

// ---- what the translation path recorded for a card ----
enum { NRN_VB = 4 };
typedef struct nrn_vals {
    uint64_t pipe, layout, set;
    uint64_t vb[NRN_VB], vbo[NRN_VB], vb_rest;   // the first NRN_VB used bindings (buffer, folded offset); the others folded into one word
    uint64_t ib; int64_t ib_delta;               // index buffer; guest address of the index data - its offset in the buffer (0 when not indexed)
    uint32_t it, noff, k, ibo;                   // index type, dynamic offsets of the set, vertices folded into the draw, folded index offset
    uint16_t used; uint8_t indexed, pad;
    uint8_t vp[24], sc[16];
} nrn_vals;
// Differences of two recordings of one card.
enum { NRN_D_PIPE = 1, NRN_D_LAYOUT = 2, NRN_D_USED = 4, NRN_D_IT = 8, NRN_D_NOFF = 16,        // functions of the card: BAD
       NRN_D_SET = 32, NRN_D_VB = 64, NRN_D_VBO = 128, NRN_D_IB = 256, NRN_D_IBD = 512,       // follow a resource the backend may move: MOVED
       NRN_D_VP = 1024, NRN_D_SC = 2048,                                                      // viewport / scissor: not in the card
       NRN_D_PASS = 4096,                                                                     // the same card under two passes
       NRN_D_BITS = 13, NRN_D_BAD = NRN_D_PIPE | NRN_D_LAYOUT | NRN_D_USED | NRN_D_IT | NRN_D_NOFF,
       NRN_D_MOVED = NRN_D_SET | NRN_D_VB | NRN_D_VBO | NRN_D_IB | NRN_D_IBD, NRN_D_VIEW = NRN_D_VP | NRN_D_SC };
static inline const char *nrn_diff_name(int bit) {
    static const char *const k[NRN_D_BITS] = {"pipeline", "set layout", "vertex bindings", "index type", "dynamic offset count", "descriptor set", "vertex buffer",
                                              "vertex offset / fold", "index buffer", "index address", "viewport", "scissor", "pass"};
    return bit >= 0 && bit < NRN_D_BITS ? k[bit] : "?";
}
static inline unsigned nrn_vals_diff(const nrn_vals *a, const nrn_vals *b) {
    unsigned d = 0;
    if (a->pipe != b->pipe && NRN_MUTATE != 3) d |= NRN_D_PIPE;
    if (a->layout != b->layout) d |= NRN_D_LAYOUT;
    if (a->used != b->used || a->indexed != b->indexed) d |= NRN_D_USED;
    if (a->it != b->it) d |= NRN_D_IT;
    if (a->noff != b->noff) d |= NRN_D_NOFF;
    if (a->set != b->set) d |= NRN_D_SET;
    if (memcmp(a->vb, b->vb, sizeof a->vb) || a->vb_rest != b->vb_rest) d |= NRN_D_VB;
    if (memcmp(a->vbo, b->vbo, sizeof a->vbo) || a->k != b->k) d |= NRN_D_VBO;
    if (a->ib != b->ib) d |= NRN_D_IB;
    if (a->ib_delta != b->ib_delta || a->ibo != b->ibo) d |= NRN_D_IBD;
    if (memcmp(a->vp, b->vp, sizeof a->vp)) d |= NRN_D_VP;
    if (memcmp(a->sc, b->sc, sizeof a->sc)) d |= NRN_D_SC;
    return d;
}
// The values of a recorded draw from what the lane's tail holds. vb / vbo: 16 bindings, `used` their mask; ibuf: the guest address the
// record's draw read its indices from; first: the first index after the fold; isz: bytes an index.
static inline void nrn_vals_make(nrn_vals *v, uint64_t pipe, uint64_t layout, uint64_t set, uint32_t noff, unsigned used, const uint64_t *vb, const uint64_t *vbo,
                                 uint32_t k, int indexed, uint64_t ib, uint64_t ibo, uint32_t it, uint64_t ibuf, uint32_t first, uint32_t isz,
                                 const void *vp, const void *sc) {
    memset(v, 0, sizeof *v);
    v->pipe = pipe; v->layout = layout; v->set = set; v->noff = noff; v->used = (uint16_t)used; v->k = k;
    unsigned n = 0;
    for (unsigned i = 0; i < 16; i++) if (used >> i & 1u) {
        if (n < NRN_VB) { v->vb[n] = vb[i]; v->vbo[n] = vbo[i]; }
        else v->vb_rest = nrn_mix(nrn_mix(v->vb_rest, vb[i]), vbo[i]);
        n++;
    }
    v->indexed = indexed ? 1 : 0;
    if (indexed) { v->ib = ib; v->ibo = (uint32_t)ibo; v->it = it; v->ib_delta = (int64_t)(ibuf - ((uint64_t)first * isz + ibo)); }
    if (vp) memcpy(v->vp, vp, sizeof v->vp);
    if (sc) memcpy(v->sc, sc, sizeof v->sc);
}

typedef struct nrn_ent { uint64_t card; nrn_vals v; uint32_t same, differs; uint8_t pass, pad[7]; } nrn_ent;
typedef struct nrn_tab { nrn_ent *e; uint32_t cap, n; uint64_t clears; } nrn_tab;
static inline int nrn_tab_init(nrn_tab *t, uint32_t cap) {   // cap: a power of two
    memset(t, 0, sizeof *t);
    if (cap < 64 || (cap & (cap - 1))) return -1;
    t->e = (nrn_ent *)calloc(cap, sizeof *t->e);
    if (!t->e) return -1;
    t->cap = cap;
    return 0;
}
static inline void nrn_tab_free(nrn_tab *t) { free(t->e); memset(t, 0, sizeof *t); }
// Notes a recorded draw. Returns 0 = the card's first recording, 1 = the same values as the last recording of the card, 2 = they
// differ (*diff = NRN_D_*; the entry now holds the new values: the next draw is compared with the latest recording).
static inline int nrn_tab_note(nrn_tab *t, uint64_t card, int pass, const nrn_vals *v, unsigned *diff) {
    *diff = 0;
    if (t->n * 4 >= t->cap * 3) { memset(t->e, 0, sizeof *t->e * t->cap); t->n = 0; t->clears++; }   // full: start again (every card is learnt anew)
    uint32_t i = (uint32_t)(card ^ card >> 32) & (t->cap - 1);
    for (;; i = (i + 1) & (t->cap - 1)) {
        nrn_ent *e = &t->e[i];
        if (!e->card) { e->card = card; e->v = *v; e->pass = (uint8_t)pass; e->same = e->differs = 0; t->n++; return 0; }
        if (e->card != card) continue;
        unsigned d = nrn_vals_diff(&e->v, v);
        if (e->pass != pass) d |= NRN_D_PASS;
        if (!d) { e->same++; return 1; }
        e->differs++;
        if (NRN_MUTATE != 4) { e->v = *v; e->pass = (uint8_t)pass; }
        *diff = d;
        return 2;
    }
}

// ---- counters of one pass (the backend's, a window of the 10 s line) ----
typedef struct nrn_pstat {
    uint64_t records, noted, first, same, differs, bad, moved_only, view_only, diff[NRN_D_BITS], seq_gaps;
} nrn_pstat;
// A record's sequence number against the last one of its pass (the front end counts the cards it wrote per pass): a gap = records the
// translation thread never saw (another command buffer, a dropped range) - counted, not an error.
static inline void nrn_seq_note(nrn_pstat *s, uint32_t *last, uint32_t seq) {
    if (*last && seq != *last + 1 && NRN_MUTATE != 8) s->seq_gaps++;
    *last = seq;
}
static inline void nrn_pstat_note(nrn_pstat *s, int r, unsigned diff) {
    s->noted++;
    if (r == 0) { s->first++; return; }
    if (r == 1) { s->same++; return; }
    s->differs++;
    for (int b = 0; b < NRN_D_BITS; b++) if (diff >> b & 1u) s->diff[b]++;
    if (diff & (NRN_D_BAD | NRN_D_PASS)) s->bad++;
    else if (diff & NRN_D_MOVED) s->moved_only++;
    else s->view_only++;
}
#endif
