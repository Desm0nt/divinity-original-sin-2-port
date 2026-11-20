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
//             _OTHER (0 off = the record stream byte for byte as before, 2 verify, 1 apply - step B below).
//   the tail  16 bytes after the NR-09 tail of the draw record (NBD_F_NRN): card, pass, a sequence number; rsv bit 0 (NRN_T_READY): the
//             game thread found the card in the published table when it wrote the record.
//   the row   step B (nrn_rows): what the translation path RESOLVED for a card - NR-08's plan and entry, NR-09's pipeline entry - with
//             the generations it is valid under and the few live words the card does not hold (pass definition, shader objects,
//             NR-08's fixed words). A draw whose card has a live row is executed from the row (no descriptor key, no variant, no
//             table walk); =2 compares the row with what the path without it resolves.
//   the publication (nrn_pub)  which cards have a row, readable by the game thread without a lock: one word a slot, a generation that
//             retires every word at once.
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
// (conditional render, dirty masks) are read into the card, 8 a missing sequence number is not counted; step B - 9 a row answers although
// a generation of the backend moved, 10 the published word is not compared with the card, 11 a bump of the publication's generation
// leaves the old words ready, 12 the row's answer is compared without its NR-08 entry id, 13 a row of another pass answers; part 2 -
// 14 a full table of rows starts again (a card whose short records are in flight loses its image), 15 the engine mask of the image
// ignores the vertex-buffer slots of a dropped state record, 16 a dropped card stays published; eviction (series CN) - 17 retired rows
// are removed at once (no quarantine: a short record in flight loses its image), 18 a retired row stays published, 19 a row that was
// touched again is removed all the same, 20 the row's answer is compared without the lane word (NR-09's key id and variant words).
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
// The pass of a draw: class of colour target 0 (NRN_RC_NONE when the draw has no colour target), class of the depth target, and
// whether ANY colour target is a G-buffer. Phone series N_v1 (20.11): the engine's colour target 0 of the G-buffer pass and of the
// cascade draws is a handle no transition names, so by colour 0 alone both passes fell into OTHER - a shadow depth target decides
// first, then a G-buffer among the colour targets, then colour 0.
static inline int nrn_pass_of(int have_color, int c0, int ds, int any_gbuf) {
    if (ds == NRN_RC_SHADOW && NRN_MUTATE != 5) return NRN_P_SHADOW;
    if (!have_color) return NRN_P_PREPASS;
    if (any_gbuf) return NRN_P_GBUF;
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
    // four independent lanes over 32-byte steps (the multiplies of a lane do not wait for the other lanes: the card is taken on the
    // game thread at every engine draw - N_v1 measured 291-365 ns a card with one chain), then the lanes and the rest in one chain
    size_t i = 0;
    if (n >= 64) {
        uint64_t a = h, b = h ^ 0x243f6a8885a308d3ull, c = h ^ 0x13198a2e03707344ull, d = h ^ 0xa4093822299f31d0ull;
        for (; i + 32 <= n; i += 32) { uint64_t w[4]; memcpy(w, p + i, 32); a = nrn_mix(a, w[0]); b = nrn_mix(b, w[1]); c = nrn_mix(c, w[2]); d = nrn_mix(d, w[3]); }
        h = nrn_mix(nrn_mix(nrn_mix(a, b), c), d);
    }
    for (; i < n; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h = nrn_mix(h, w); }
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
    uint64_t c[8], ds; memcpy(c, R + NRN_R_RT, sizeof c); memcpy(&ds, R + NRN_R_DEPTH_RT, 8);
    int g = 0;
    for (int i = 0; i < 8; i++) g |= nrn_rt_get(m, c[i]) == NRN_RC_GBUF;
    return nrn_pass_of(!nrn_h_none(c[0]), nrn_rt_get(m, c[0]), nrn_rt_get(m, ds), g);
}

// ---- the tail of the draw record (after nrd_tail; the record's header carries NBD_F_NRN = 32) ----
enum { NRN_VER = 1, NRN_F_FLAG = 32, NRN_TAIL_BYTES = 16, NRN_T_READY = 1, NRN_T_SHORT = 2 };
// rsv bit 1 (NRN_T_SHORT, step B part 2): the record is SHORT - the state records of the packets of the draw and the texture binds of
// its flush (one handle, stage < NRN_TEX_STAGES, slot < NRN_TEX_SLOTS) were not written; an nrn_short_ext (NRN_SHORT_EXT_BYTES) follows
// the tail and names exactly the fields and slots left out; the backend writes those into its State from the image of the card before
// it executes the record.
enum { NRN_TEX_STAGES = 6, NRN_TEX_SLOTS = 32 };
typedef struct nrn_tail { uint64_t card; uint8_t ver, pass; uint16_t rsv; uint32_t seq; } nrn_tail;
// at = the byte right after the NR-09 tail; cap / n = bytes of the record's payload.
static inline int nrn_tail_put(uint8_t *dst, size_t cap, size_t at, uint64_t card, int pass, uint32_t seq, unsigned rsv) {
    if (!dst || at + sizeof(nrn_tail) > cap) return 0;
    nrn_tail t; memset(&t, 0, sizeof t);
    t.card = card; t.ver = NRN_VER; t.pass = (uint8_t)pass; t.seq = seq; t.rsv = (uint16_t)rsv;
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

// ---- step B: the row of a card ----
// What an answered draw takes instead of its lookups. plan / pent / did / frag: the resolution (nr_z3.h nrz_val: NR-08's plan of the
// shader set, NR-09's entry of the pipeline, NR-08's entry id, "no stage before the fragment shader reads an image"). passdef / sh / f:
// words the card does not hold and the resolution depends on, compared live before the row answers - the render pass definition of the
// open pass, the backend's five shader objects, NR-08's six fixed words (viewport size, texture / sampler pool).
enum { NRN_STAMPS = 3 };   // NR-08's id generation, NR-09's table generation, the backend's object epoch
// lane (series CN, 20.11: 16 red lines in one frame - the same card, plan and NR-08 entry, another pipeline entry): the words NR-09's
// table is asked with besides the state groups - the record's key id and the cheap words of the variant (primitive, samples, target
// count / mask, depth format, restart). The card does not hold the key id (it follows what the registry knows about the targets).
typedef struct nrn_ans { uint64_t plan, pent; int32_t did; uint8_t frag, pad[3]; uint64_t passdef, sh[5], f[6], lane; } nrn_ans;
static inline int nrn_ans_same(const nrn_ans *a, const nrn_ans *b) {
    return a->plan == b->plan && a->pent == b->pent && (a->did == b->did || NRN_MUTATE == 12) && a->frag == b->frag && a->passdef == b->passdef &&
           !memcmp(a->sh, b->sh, sizeof a->sh) && !memcmp(a->f, b->f, sizeof a->f) && (a->lane == b->lane || NRN_MUTATE == 20);
}
static inline uint64_t nrn_lane_word(uint32_t key, unsigned prim, unsigned samples, unsigned rt_n, unsigned colors, unsigned depth_fmt, unsigned restart) {
    return nrn_mix(nrn_mix((uint64_t)key << 32 | (prim & 0xffu) << 24 | (samples & 0xffu) << 16 | (rt_n & 0xffu) << 8 | (restart & 0xffu), colors), depth_fmt) | 1;
}
// The live words of a row against the state now: 0 = they stand; else which differ (NRN_L_*).
enum { NRN_L_PASSDEF = 1, NRN_L_SHADERS = 2, NRN_L_FIXED = 4 };
static inline unsigned nrn_ans_live(const nrn_ans *a, uint64_t passdef, const uint64_t sh[5], const uint64_t f[6]) {
    return (a->passdef != passdef ? NRN_L_PASSDEF : 0u) | (memcmp(a->sh, sh, sizeof a->sh) ? NRN_L_SHADERS : 0u) | (memcmp(a->f, f, sizeof a->f) ? NRN_L_FIXED : 0u);
}
// last: the tick (nrn_rows.now, the backend's frame count) the row was last asked / taught / given an image; retiring: dropped from the
// publication, removed by the next compaction unless it is touched again.
typedef struct nrn_row { uint64_t card; nrn_ans a; uint64_t stamp[NRN_STAMPS]; uint32_t answered, relearned; uint8_t pass, unstable, retiring, pad; uint32_t last; } nrn_row;   // stamp all 0: no answer yet (the row holds an image only)
typedef struct nrn_rows { nrn_row *e; uint32_t cap, n, now, retire_at, nretiring; uint64_t clears, retired, removed, revived; } nrn_rows;
static inline void nrn_row_touch(nrn_rows *t, nrn_row *e) { e->last = t->now; if (e->retiring && NRN_MUTATE != 19) { e->retiring = 0; t->nretiring--; t->revived++; } }
static inline int nrn_rows_init(nrn_rows *t, uint32_t cap) {   // cap: a power of two
    memset(t, 0, sizeof *t);
    if (cap < 64 || (cap & (cap - 1))) return -1;
    t->e = (nrn_row *)calloc(cap, sizeof *t->e);
    if (!t->e) return -1;
    t->cap = cap;
    return 0;
}
static inline void nrn_rows_free(nrn_rows *t) { free(t->e); memset(t, 0, sizeof *t); }
static inline void nrn_rows_clear(nrn_rows *t) { if (t->n) { memset(t->e, 0, sizeof *t->e * t->cap); t->n = 0; } t->clears++; }
enum { NRN_F_HIT, NRN_F_NONE, NRN_F_STALE, NRN_F_PASS };
// The row of a card for a draw of `pass` under the generations `stamp`. *why = NRN_F_*: no row; a row made under other generations
// (it does not answer: the path without it teaches the card anew); a row of another pass (one card in two passes: the caller counts
// it bad). NULL unless NRN_F_HIT.
static inline nrn_row *nrn_rows_find(nrn_rows *t, uint64_t card, int pass, const uint64_t stamp[NRN_STAMPS], int *why) {
    uint32_t i = (uint32_t)(card ^ card >> 32) & (t->cap - 1);
    for (;; i = (i + 1) & (t->cap - 1)) {
        nrn_row *e = &t->e[i];
        if (!e->card) { *why = NRN_F_NONE; return NULL; }
        if (e->card != card) continue;
        if (memcmp(e->stamp, stamp, sizeof e->stamp) && NRN_MUTATE != 9) { *why = NRN_F_STALE; return NULL; }
        if (e->pass != pass && NRN_MUTATE != 13) { *why = NRN_F_PASS; return NULL; }
        *why = NRN_F_HIT; nrn_row_touch(t, e);
        return e;
    }
}
// The row of a card, made when the card has none (no answer yet: stamps 0). NULL: the table is full - it never starts again (part 2:
// a row carries the image short records in flight are executed from), a new card simply gets no row and is never published.
static inline nrn_row *nrn_rows_get(nrn_rows *t, uint64_t card, int pass, int *made) {
    *made = 0;
    if (NRN_MUTATE == 14 && t->n * 4 >= t->cap * 3) nrn_rows_clear(t);
    uint32_t i = (uint32_t)(card ^ card >> 32) & (t->cap - 1);
    for (;; i = (i + 1) & (t->cap - 1)) {
        nrn_row *e = &t->e[i];
        if (e->card == card) { nrn_row_touch(t, e); return e; }
        if (e->card) continue;
        if (t->n * 4 >= t->cap * 3) return NULL;
        memset(e, 0, sizeof *e); e->card = card; e->pass = (uint8_t)pass; e->last = t->now; t->n++; *made = 1;
        return e;
    }
}
// The resolution the path without the row just made for the card. Returns 0 = the row's first answer, 1 = the row held the same answer
// under the same generations, 2 = the answer was replaced (another answer or other generations), -1 = no row (*full: the table is full).
static inline int nrn_rows_put(nrn_rows *t, uint64_t card, int pass, const nrn_ans *a, const uint64_t stamp[NRN_STAMPS], int *full) {
    int made = 0; static const uint64_t none[NRN_STAMPS] = {0, 0, 0};
    nrn_row *e = nrn_rows_get(t, card, pass, &made);
    *full = !e;
    if (!e) return -1;
    const int first = !memcmp(e->stamp, none, sizeof none);
    if (!first && nrn_ans_same(&e->a, a) && !memcmp(e->stamp, stamp, sizeof e->stamp) && e->pass == pass) return 1;
    e->a = *a; memcpy(e->stamp, stamp, sizeof e->stamp); e->pass = (uint8_t)pass;
    if (first) return 0;
    e->relearned++;
    return 2;
}

// ---- step B: the publication - which cards have a row, for the game thread ----
// One writer (the translation thread), any reader, no lock. A slot is one word: the card's upper 48 bits and the 16-bit generation it
// was published under; the slot is picked by the card's bits 16..29, so a reader that finds its 48 bits and the current generation
// has found its card (48 bits compared). nrn_pub_bump retires every word at once (the rows were cleared, a backend generation
// moved); when the 16-bit generation wraps the words are zeroed first, so an old word never becomes current again.
// The answer is a hint about the past: the row may be gone by the time the record is executed - the backend always asks its own table.
enum { NRN_PUB_SLOTS = 16384 };
typedef struct nrn_pub { uint64_t gen; uint64_t w[NRN_PUB_SLOTS]; } nrn_pub;   // gen: 1..65535 (0 = nothing was ever published)
static inline uint32_t nrn_pub_slot(uint64_t card) { return (uint32_t)(card >> 16) & (NRN_PUB_SLOTS - 1); }
static inline void nrn_pub_bump(nrn_pub *p) {
    uint64_t g = __atomic_load_n(&p->gen, __ATOMIC_RELAXED) + 1;
    if (g > 0xffffu) { for (unsigned i = 0; i < NRN_PUB_SLOTS; i++) __atomic_store_n(&p->w[i], 0, __ATOMIC_RELAXED); g = 1; }
    if (NRN_MUTATE == 11) return;
    __atomic_store_n(&p->gen, g, __ATOMIC_RELEASE);
}
static inline void nrn_pub_set(nrn_pub *p, uint64_t card) {
    uint64_t g = __atomic_load_n(&p->gen, __ATOMIC_RELAXED);
    if (!g) { g = 1; __atomic_store_n(&p->gen, g, __ATOMIC_RELEASE); }
    __atomic_store_n(&p->w[nrn_pub_slot(card)], (card & ~0xffffull) | g, __ATOMIC_RELEASE);
}
static inline void nrn_pub_drop(nrn_pub *p, uint64_t card) {   // this card is not to be written short any more (another card of the slot stays)
    const uint64_t g = __atomic_load_n(&p->gen, __ATOMIC_RELAXED);
    uint64_t *w = &p->w[nrn_pub_slot(card)];
    if (__atomic_load_n(w, __ATOMIC_RELAXED) == ((card & ~0xffffull) | g) && NRN_MUTATE != 16) __atomic_store_n(w, 0, __ATOMIC_RELEASE);
}
static inline int nrn_pub_ready(const nrn_pub *p, uint64_t card) {
    const uint64_t g = __atomic_load_n(&p->gen, __ATOMIC_ACQUIRE), w = __atomic_load_n(&p->w[nrn_pub_slot(card)], __ATOMIC_ACQUIRE);
    if (NRN_MUTATE == 10) return g && (w & 0xffffu) == g;
    return g && w == ((card & ~0xffffull) | g);
}

// ---- eviction (phone series CN_v2, 20.11: walking fills the table - 12 288 rows - and no new card ever got a row) ----
// A row cannot simply be removed: a short record written while its card was published is executed later and needs the image the row
// carries. So eviction has two steps. nrn_rows_retire: rows not touched for `idle` ticks leave the publication (the game thread
// writes their next record long) and are marked. nrn_rows_compact, at least NRN_QUARANTINE ticks later: the rows still marked are
// removed and the table is built again (move() tells the owner of the images where each kept row went). A row touched in between -
// a short record in flight, a long record of the card - is revived and stays. The quarantine is the longest a record may wait between
// the game thread writing it and the backend executing it, in backend frames; a short record that finds no image is a red line.
enum { NRN_QUARANTINE = 64 };
static inline uint32_t nrn_rows_retire(nrn_rows *t, nrn_pub *p, uint32_t idle) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < t->cap; i++) { nrn_row *e = &t->e[i];
        if (!e->card || e->retiring || t->now - e->last < idle) continue;
        e->retiring = 1; k++; if (NRN_MUTATE != 18) nrn_pub_drop(p, e->card); }
    t->nretiring += k; t->retired += k; t->retire_at = t->now;
    return k;
}
static inline int nrn_rows_compact_due(const nrn_rows *t) { return t->nretiring && (t->now - t->retire_at >= NRN_QUARANTINE || NRN_MUTATE == 17); }
// Removes the retiring rows. move(u, from, to): the row at index `from` is now at `to` (called once for every kept row; `from` indexes
// the old array, `to` the new one); drop(u, from): the row at `from` is gone. Returns the rows removed, -1 = no memory (nothing changed).
static inline int nrn_rows_compact(nrn_rows *t, void (*move)(void *, uint32_t, uint32_t), void (*drop)(void *, uint32_t), void *u) {
    nrn_row *ne = (nrn_row *)calloc(t->cap, sizeof *ne);
    if (!ne) return -1;
    uint32_t kept = 0, gone = 0;
    for (uint32_t i = 0; i < t->cap; i++) { const nrn_row *e = &t->e[i];
        if (!e->card) continue;
        if (e->retiring) { gone++; if (drop) drop(u, i); continue; }
        uint32_t j = (uint32_t)(e->card ^ e->card >> 32) & (t->cap - 1);
        while (ne[j].card) j = (j + 1) & (t->cap - 1);
        ne[j] = *e; kept++; if (move) move(u, i, j); }
    free(t->e); t->e = ne; t->n = kept; t->nretiring = 0; t->removed += gone;
    return (int)gone;
}
// The policy of the backend, once a tick: 1 = rows were retired, 2 = the table was compacted, 0 = nothing. A table more than half
// full retires what was idle for `idle` ticks; when that frees less than an eighth, *idle is halved (never below 2) for the next time.
static inline int nrn_rows_tick(nrn_rows *t, nrn_pub *p, uint32_t *idle, void (*move)(void *, uint32_t, uint32_t), void (*drop)(void *, uint32_t), void *u) {
    t->now++;
    if (nrn_rows_compact_due(t)) return nrn_rows_compact(t, move, drop, u) >= 0 ? 2 : 0;
    if (t->nretiring || t->n * 2 < t->cap) return 0;
    if (nrn_rows_retire(t, p, *idle) * 8 < t->cap && *idle > 2) *idle /= 2;
    return 1;
}

// ---- step B: counters of one pass (the backend's, a window of the 10 s line) ----
enum { NRN_BW_NO_ROW, NRN_BW_STALE, NRN_BW_PASS, NRN_BW_GUARD, NRN_BW_FORM, NRN_BW_GATE, NRN_BW_PASSDEF, NRN_BW_SHADERS, NRN_BW_FIXED, NRN_BW_REFUSED, NRN_BW_LANE, NRN_BW_N };
static inline const char *nrn_bw_name(int w) {
    static const char *const k[NRN_BW_N] = {"no row yet", "row of other generations", "row of another pass", "the prologue changed the draw", "special draw form",
                                            "a gate of the short path", "another pass definition", "other shader objects", "viewport size / pools moved", "the answered half refused", "another NR-09 key id / variant word"};
    return w >= 0 && w < NRN_BW_N ? k[w] : "?";
}
typedef struct nrn_bstat {
    uint64_t asked, why[NRN_BW_N], answered[4], predicted, p_same, p_bad, p_lost, learned, relearned, kept,
             ready, ready_ans, ready_not,   // records the game thread flagged ready: answered (or predicted) at execution / not
             shorts, s_noimg, s_changed, img_new, img_same, img_bad, img_full, img_skip;   // part 2: short records (without an image: cannot be drawn right; that changed State), images captured / confirmed / differing / not kept (table full)
} nrn_bstat;

// ---- step B part 2: what a short record leaves out ----
// The image of a card is the part of the backend State the records of an engine draw write: the fields of its OP_X_NR_STATE records
// (nr_state3.h) and the texture slots its flush binds. A short record names exactly what was left out of it - the union of the fields
// of the dropped state records (nrn_emask) and the dropped texture binds (at most two stages, a slot mask each) - and the backend
// writes those, and only those, from the image. Nothing the engine did not write at this very draw is touched.
//   have / vb / blend / nstream   as nr3_hdr; stages: the NVN stage bits of the programs (nr3_hdr pst); sten[w]: the faces of stencil
//   word w (0 value mask, 1 write mask, 2 ref).
typedef struct nrn_emask { uint16_t have, vb; uint8_t blend, stages, sten[3], nstream, pad[2]; } nrn_emask;
typedef struct nrn_short_ext { nrn_emask m; uint8_t st[2], pad[2]; uint32_t tm[2]; } nrn_short_ext;   // st: the two stages (0xff none), tm: their slot masks
enum { NRN_SHORT_EXT_BYTES = 24 };
static inline int nrn_emask_add(nrn_emask *m, unsigned have, unsigned vb, unsigned blend, unsigned stages, const uint8_t sten_faces[3], unsigned nstream) {
    nrn_emask o = *m;
    m->have |= (uint16_t)have; if (NRN_MUTATE != 15) m->vb |= (uint16_t)vb; m->blend |= (uint8_t)blend; m->stages |= (uint8_t)stages;
    for (int w = 0; w < 3; w++) m->sten[w] |= sten_faces[w];
    if (nstream > m->nstream) m->nstream = (uint8_t)nstream;
    return memcmp(&o, m, sizeof o) != 0;
}
static inline void nrn_emask_or(nrn_emask *m, const nrn_emask *a) { (void)nrn_emask_add(m, a->have, a->vb, a->blend, a->stages, a->sten, a->nstream); }
// The fields one OP_X_NR_STATE record payload writes (layout nr_state3.h: nr3_hdr {ver, flags, nprog, nstream, u16 have, u16 vb, u8 blend,
// nvattr, pst[3], _r, u16 seq}, then prog[nprog] x 8, ds (8) when have bit 0, then the stencil words {i32 face, i32 value} for have
// bits 1..3). 0 = too short to be a record (nothing added).
static inline int nrn_emask_of_state(nrn_emask *m, const uint8_t *p, size_t n) {
    if (!p || n < 16) return 0;
    const unsigned nprog = p[2], nstream = p[3]; uint16_t have, vb; memcpy(&have, p + 4, 2); memcpy(&vb, p + 6, 2);
    const unsigned blend = p[8];
    unsigned stages = 0; for (unsigned i = 0; i < nprog && i < 3; i++) stages |= p[10 + i];
    uint8_t faces[3] = {0, 0, 0};
    size_t at = 16 + 8u * nprog + ((have & 1u) ? 8u : 0u);
    for (int w = 0; w < 3; w++) if (have & (2u << w)) { int32_t f; if (at + 8 > n) return 0; memcpy(&f, p + at, 4); at += 8; faces[w] = (uint8_t)(f & 3); }
    (void)nrn_emask_add(m, have, vb, blend, stages, faces, nstream);
    return 1;
}
// A texture bind left out of a short record. 0 = it cannot be named (a third stage, a slot or stage outside the image): the caller
// records the bind as before.
static inline int nrn_short_ext_tex(nrn_short_ext *x, int st, int slot) {
    if (st < 0 || st >= NRN_TEX_STAGES || slot < 0 || slot >= NRN_TEX_SLOTS) return 0;
    for (int k = 0; k < 2; k++) {
        if (x->st[k] == 0xff) x->st[k] = (uint8_t)st;
        if (x->st[k] == st) { x->tm[k] |= 1u << slot; return 1; }
    }
    return 0;
}
// The ext of a short record: right after the nrn_tail (at = the byte after it). 1 = read / written.
static inline int nrn_short_ext_put(uint8_t *dst, size_t cap, size_t at, const nrn_short_ext *x) {
    if (!dst || at + sizeof *x > cap) return 0;
    memcpy(dst + at, x, sizeof *x);
    return 1;
}
static inline int nrn_short_ext_get(const uint8_t *p, size_t n, size_t at, nrn_short_ext *x) {
    if (!p || at + sizeof *x > n) return 0;
    memcpy(x, p + at, sizeof *x);
    return (x->st[0] == 0xff || x->st[0] < NRN_TEX_STAGES) && (x->st[1] == 0xff || x->st[1] < NRN_TEX_STAGES) && x->m.nstream <= 16;
}
#endif
