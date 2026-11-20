// [DOS2] NR-09R (DM_NR_DIRECT, default 0; docs/dos2/nr/NR-09R.md): the engine draw recorded directly instead of through its NVN calls.
// Pure C, no engine / host / NVN / Vulkan type: used by the NVN layer (dm/gfx/nvn/nvn_nr_direct.inc, inside hle_nvn.c), the front end
// (dm/host/hle_dos2_nr_direct.inc), the backend (dm/gfx/vulkan/engine/xbg_nr_direct.inc) and the host test
// dm/gfx/vulkan/engine/tests/nr_direct_test.c.
//
// Four parts (DM_NR_DIRECT_PARTS, a mask; default all):
//   NRDP_DRAW   the engine's draw functions 0x928e80 / 0x928fb0 run natively (the model of NR-23, nr_nocall.h) and hand the texture binds
//               of their flush and the draw to the recorder in ONE call: no GetTextureHandle / BindTexture / SetRenderEnable /
//               DrawElements call of the engine, no thunk, no stack switch per call, no texture-flush hook entry, the identity of
//               NR-09 passed along instead of through a tap.
//   NRDP_TEXF   a texture bind of an engine draw whose slot already holds that handle in the backend's table is not sent (nrd_mirror).
//   NRDP_TAP    the frame-IR tap of NB-04 / NB-07 / NB-18 does not walk the bound slots again at a draw whose binding tables and
//               programs were already folded into the open pass (nrd_tapmemo).
//   NRDP_IDENT  the identity of NR-09 without locks or atomics on the engine's thread: the state of the previous draw compared as one
//               block, then a per-thread table of keys (nrd_idc).
// Each part is exact by construction (its argument is next to its code) and is checked on the device by DM_NR_DIRECT=2.
#ifndef DM_NR_DIRECT_H
#define DM_NR_DIRECT_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { NRDP_DRAW = 1, NRDP_TEXF = 2, NRDP_TAP = 4, NRDP_IDENT = 8, NRDP_ALL = 15 };

// ---- the tail word of an OP_X_NR_DRAW (nrd_tail._pad, nvn_nb_draw.h; 0 without DM_NR_DIRECT, every old reader ignores it) ----
//   NRD_X_DIRECT  the record was written by the direct recorder (DM_NR_DIRECT=1, NRDP_DRAW)
//   NRD_X_FILT    bits 8..15: texture binds of this draw that were not sent (DM_NR_DIRECT=1, NRDP_TEXF; 255 = 255 or more)
//   NRD_X_TEXCHK  bits 8..31: bind i (i < 24) of this record is one DM_NR_DIRECT=1 would not send: the backend checks that its table
//                 holds that handle before the record's binds land (DM_NR_DIRECT=2)
enum { NRD_X_DIRECT = 1, NRD_X_TEXCHK = 2, NRD_X_FILT = 4, NRD_X_BITS = 24 };
static inline uint32_t nrd_x_filt(uint32_t flags, unsigned filtered) { return flags | (filtered ? NRD_X_FILT | (filtered > 255 ? 255u : filtered) << 8 : 0u); }
static inline uint32_t nrd_x_chk(uint32_t flags, uint32_t would) { return would ? flags | NRD_X_TEXCHK | would << 8 : flags; }
static inline unsigned nrd_x_filtered(uint32_t x) { return (x & NRD_X_FILT) ? (x >> 8) & 0xff : 0; }
static inline uint32_t nrd_x_would(uint32_t x) { return (x & NRD_X_TEXCHK) ? x >> 8 : 0; }

static inline uint64_t nrd_mix(uint64_t x) {   // splitmix64 finaliser
    x += 0x9E3779B97F4A7C15ull; x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull; x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// ---- NRDP_TEXF: what the backend's binding tables hold while it executes the recording being written ----
// The backend applies the binds of a command handle in record order (exec_bind_handles / nbd_apply are the only writers of state.tex /
// samp / img; the post fusion swaps a slot and puts it back inside one draw). So for ONE recording of one command buffer, written by one
// thread: after this recording recorded a bind of handle h to (kind, stage, slot) and no later bind to that slot, the backend's table
// holds h there whenever it executes a later record of this recording - whatever ran before the recording and however often it is
// replayed. A bind of h to that slot is then a no-op for the tables and may stay unsent.
// Nothing is assumed across recordings (reset at BeginRecording), about another command buffer, or after a nested command handle
// (CopyCommands / CallCommands execute foreign records in the middle of this one): forget(). A bind by another thread into the tracked
// command buffer makes the order unknown: forget().
enum { NRDM_TEX = 0, NRDM_SAMP = 1, NRDM_IMG = 2, NRDM_KINDS = 3, NRDM_STAGES = 6, NRDM_SLOTS = 128 };
typedef struct nrd_mirror {
    const void *cb, *owner;                     // the command buffer / the thread (any per-thread address) the table describes; NULL = none
    uint32_t epoch;                             // at[] == epoch: h[] is what this recording bound there
    uint32_t at[NRDM_KINDS][NRDM_STAGES][NRDM_SLOTS];
    uint64_t h[NRDM_KINDS][NRDM_STAGES][NRDM_SLOTS];
} nrd_mirror;
static inline void nrdm_reset(nrd_mirror *m) { if (!++m->epoch) { memset(m->at, 0, sizeof m->at); m->epoch = 1; } }
static inline void nrdm_forget(nrd_mirror *m) { m->cb = NULL; m->owner = NULL; nrdm_reset(m); }
// BeginRecording of cb: a new recording knows nothing.
static inline void nrdm_begin(nrd_mirror *m, const void *cb) { if (cb == m->cb) nrdm_reset(m); }
// CopyCommands / CallCommands into cb.
static inline void nrdm_nested(nrd_mirror *m, const void *cb) { if (cb == m->cb) nrdm_forget(m); }
static inline int nrdm_ok(unsigned kind, int stage, int slot) { return kind < NRDM_KINDS && stage >= 0 && stage < NRDM_STAGES && slot >= 0 && slot < NRDM_SLOTS; }
// A bind call of `me` into cb, n handles from slot `first` (the call is about to be recorded - or, when 1 is returned and the caller
// filters, not sent). adopt: the caller is the thread that records engine draws into cb right now (inside an engine draw bracket) - the
// table follows it. Returns 1 when n == 1, the table describes (cb, me) and holds exactly this handle there; else 0. The table then
// holds the handles of the call (a no-op when 1 was returned).
static inline int nrdm_bind(nrd_mirror *m, const void *cb, const void *me, int adopt, unsigned kind, int stage, int first, int n, const uint64_t *h) {
    if (adopt) { if (m->cb != cb || m->owner != me) { nrdm_reset(m); m->cb = cb; m->owner = me; } }
    else if (cb != m->cb) return 0;                               // another command buffer: its records do not touch this recording
    else if (m->owner != me) { nrdm_forget(m); return 0; }        // another thread writes into the tracked recording: order unknown
    int same = 0;
    for (int i = 0; i < n; i++) {
        const int s = first + i;
        if (!nrdm_ok(kind, stage, s)) continue;                   // outside the table: never known, never filtered
        uint32_t *at = &m->at[kind][stage][s]; uint64_t *v = &m->h[kind][stage][s];
        if (n == 1 && *at == m->epoch && *v == h[0]) same = 1;
        *at = m->epoch; *v = h[i];
    }
    return same;
}
// What may stay unsent when nrdm_bind said "the table holds it": a single texture bind recorded inside an engine draw bracket (adopt) -
// the one class DM_NR_DIRECT=2 can check on the backend (the bind rides in the draw's record, flagged).
static inline int nrdm_filter(int same, int adopt, unsigned kind, int n) { return same && adopt && kind == NRDM_TEX && n == 1; }

// ---- NRDP_TAP: the read sets already folded into the open pass of the frame IR ----
// The tap (nvn_nb_tap.inc nbt_draw) adds, at a draw, the images of the bound slots its programs sample to the open pass (nb_read: a
// set, never removed) and the sampled list / fs key (nb_draw_prog: a set + flags that only move one way). Its bookkeeping keeps
//   (I)  after the first draw of a pass, a slot is "dirty" or its current image is a read of the pass.
// A draw D of pass N whose every stage program has known slot masks and a published fs key leaves: every slot its programs sample is
// not dirty, so its image is a read of N; the sampled list holds every image they sample; the fs key is folded. If a later draw D' of
// the SAME pass (node, IR generation) sees the SAME binding tables (stages 0..4, textures and images), the SAME programs per stage, the
// same pool registrations and the same published masks, then everything the full walk would add is already there:
//   reads      the slots it would walk are dirty ones the programs sample; the tables are equal to D's, so their images are reads of N;
//   sampled    the same images as at D (an overflow at D already cleared the exact flag, as it would again);
//   fs / exact the same key (no new "mixed"), the same exact (it only ever clears).
// The walk is skipped. It leaves dirty bits set that the walk would have cleared: by (I) that is the conservative side - the next full
// walk records those slots again, nb_read finds them (same binding: already a read of N) or adds what the old path adds too (rebound
// since). A new pass starts from "every slot" in both paths.
// The signature is a 64-bit hash (tables: XOR of a mix per bound slot, kept incrementally; programs, registration generation, mask table
// writes mixed in): two different states with one signature are not told apart (2^-64 a pair; nr_key.h takes the same odds).
enum { NRDT_N = 32 };   // signatures kept per pass (direct mapped)
typedef struct nrd_tapmemo { int node; uint32_t gen; uint64_t sig[NRDT_N]; uint8_t fl[NRDT_N]; } nrd_tapmemo;
// The signature part of one slot (w: 0 textures, 1 images; v = bound pool id + 1, 0 = nothing bound -> 0).
static inline uint64_t nrd_slot_sig(unsigned w, unsigned stage, unsigned slot, uint32_t v) {
    return v ? nrd_mix((uint64_t)(w << 16 | stage << 8 | slot) << 32 | v) : 0;
}
static inline uint64_t nrd_draw_sig(uint64_t tabsig, const uint64_t prog[5], uint32_t reggen, uint32_t maskgen) {
    uint64_t s = nrd_mix(tabsig ^ ((uint64_t)reggen << 32 | maskgen));
    for (int i = 0; i < 5; i++) s = nrd_mix(s ^ prog[i]) + (uint64_t)i;
    return s ? s : 1;
}
// 1 = a draw with this signature was folded into this very pass before (*fl = what nrd_tap_learn stored). A change of pass or IR
// generation empties the memo first.
static inline int nrd_tap_hit(nrd_tapmemo *m, int node, uint32_t gen, uint64_t sig, uint8_t *fl) {
    if (m->node != node || m->gen != gen) { m->node = node; m->gen = gen; memset(m->sig, 0, sizeof m->sig); return 0; }
    const unsigned k = (unsigned)(sig >> 40) & (NRDT_N - 1);
    if (m->sig[k] != sig) return 0;
    *fl = m->fl[k];
    return 1;
}
static inline void nrd_tap_learn(nrd_tapmemo *m, int node, uint32_t gen, uint64_t sig, uint8_t fl) {
    if (m->node != node || m->gen != gen) { m->node = node; m->gen = gen; memset(m->sig, 0, sizeof m->sig); }
    const unsigned k = (unsigned)(sig >> 40) & (NRDT_N - 1);
    m->sig[k] = sig; m->fl[k] = fl;
}

// ---- NRDP_IDENT: the key id of NR-02 / NR-09 per engine draw, on the engine's thread, without a lock ----
// raw = the fields of the renderer's state cache the key is made of (hle_dos2_nr_key.inc: NK_R_*), read in one go. The render-target
// handles stand for the RT format set id (the caller looks that up when they change, as before).
typedef struct nrd_raw {
    uint64_t shader, blend, depth, raster, vformat, rt[9];   // rt: colour 0..7, depth
    uint32_t blend_arg; uint8_t prim, alpha_test, pad[2];
} nrd_raw;
// The key bytes of nr_key.h's nr_dkey (the same layout: 5 handles, blend_arg, rtfmt, prim, alpha_test, pad) without including it here.
typedef struct nrd_key { uint64_t h[5]; uint32_t blend_arg; uint16_t rtfmt; uint8_t prim, alpha_test, pad[4]; } nrd_key;
enum { NRDI_N = 4096 };   // keys kept per thread (2 ways by the key's hash); ids are dense and never reused, `why` is fixed
typedef struct nrd_idc {
    int have; nrd_raw last; uint32_t last_id, last_why; uint16_t last_rtf;
    struct { nrd_key k; uint32_t id, why; } e[NRDI_N];
    uint64_t draws, same, hit, miss, rt;        // this thread's counters (read by the report through nrd_idc_take)
} nrd_idc;
static inline void nrd_key_of(const nrd_raw *w, uint16_t rtfmt, nrd_key *k) {
    memset(k, 0, sizeof *k);
    k->h[0] = w->shader; k->h[1] = w->blend; k->h[2] = w->depth; k->h[3] = w->raster; k->h[4] = w->vformat;
    k->blend_arg = w->blend_arg; k->rtfmt = rtfmt; k->prim = w->prim; k->alpha_test = w->alpha_test;
}
// The even slot of the key's pair of ways; *alt = which way a new key replaces when both are taken (a bit of the hash).
static inline unsigned nrd_key_slot(const nrd_key *k, unsigned *alt) {
    uint64_t s = nrd_mix(k->h[0] ^ nrd_mix(k->h[1] ^ nrd_mix(k->h[2])));
    s = nrd_mix(s ^ k->h[3] ^ nrd_mix(k->h[4] ^ ((uint64_t)k->blend_arg << 32 | (uint32_t)k->rtfmt << 16 | (uint32_t)k->prim << 8 | k->alpha_test)));
    *alt = (unsigned)(s >> 50) & 1;
    return (unsigned)(s >> 20) & (NRDI_N - 2);
}
// Step 1: 1 = the state is the previous draw's, byte for byte: *id / *why are that draw's (nothing else to do). 0 = it changed;
// *rt_same says whether the render-target handles are still the previous draw's (then the caller keeps c->last_rtf).
static inline int nrd_ident_same(nrd_idc *c, const nrd_raw *w, uint32_t *id, uint32_t *why, int *rt_same) {
    c->draws++;
    if (c->have && !memcmp(&c->last, w, sizeof *w)) { *id = c->last_id; *why = c->last_why; c->same++; return 1; }
    *rt_same = c->have && !memcmp(c->last.rt, w->rt, sizeof w->rt);
    return 0;
}
// Step 2 (rtfmt = the RT format set id of w->rt): 1 = the key is in the thread's table. 0 = the caller looks it up in the shared table
// (its lock) and calls nrd_ident_put.
static inline int nrd_ident_find(nrd_idc *c, const nrd_raw *w, uint16_t rtfmt, nrd_key *k, uint32_t *id, uint32_t *why) {
    nrd_key_of(w, rtfmt, k);
    unsigned alt; const unsigned s0 = nrd_key_slot(k, &alt);
    for (unsigned s = s0; s < s0 + 2; s++) if (c->e[s].id && !memcmp(&c->e[s].k, k, sizeof *k)) {
        *id = c->e[s].id; *why = c->e[s].why; c->hit++;
        c->last = *w; c->last_id = *id; c->last_why = *why; c->last_rtf = rtfmt; c->have = 1;
        return 1;
    }
    return 0;
}
// id 0 (the shared table is full / lost) is handed on but never kept: the next draw asks again, as the old path does.
static inline void nrd_ident_put(nrd_idc *c, const nrd_raw *w, uint16_t rtfmt, const nrd_key *k, uint32_t id, uint32_t why) {
    unsigned alt; const unsigned s0 = nrd_key_slot(k, &alt), s = !c->e[s0].id ? s0 : !c->e[s0 + 1].id ? s0 + 1 : s0 + alt;
    c->miss++;
    if (id) { c->e[s].k = *k; c->e[s].id = id; c->e[s].why = why; }
    c->last = *w; c->last_id = id; c->last_why = why; c->last_rtf = rtfmt; c->have = id != 0;
}
#endif
