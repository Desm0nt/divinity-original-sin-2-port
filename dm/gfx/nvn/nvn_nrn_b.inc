// [DOS2] NR-NATIVE step D, half b (DM_NRN_NOBODY, default 0; docs/dos2/nr/NR-NATIVE-D.md section 12): the binds of a CARD draw without
// the bodies of the NVN entry points. Included twice by hle_nvn.c (and by the host test tests/nr_nrn_b_test.c, which compiles it next
// to the real text of those bodies): NVN_NRNB_PART 1 ahead of bind_handles (the state and the capture line of the verify mode),
// NVN_NRNB_PART 2 at the end (the fold; it reads the state of nvn_nb_tap.inc, nvn_nr_direct.inc, nvn_nrn.inc).
//
// Today a direct draw (NR-09R) switches to the big stack and calls, per bind, the body of nvnCommandBufferBindUniformBuffer or
// nvnDeviceGetTextureHandle + bind_handles. With the preconditions of nrnb_can those bodies do exactly this and nothing else:
//   uniform-buffer bind   the mirror of the backend's table (nr_ubd.h nrub_bind; a range it holds is not sent), the tap's count
//                         (NBN_UBO), the bind into the shadow of the draw.
//   texture bind          the tap's table / dirty mask / signature (nbt_bind + NRD_SIG), the mirror of the backend's table
//                         (nr_direct.h nrdm_bind; a handle it holds is not sent and counted for the record's word), the texture-set
//                         generation, the left-out mask of a short draw, the bind into the shadow, the use mark.
// nrnb_fold does those steps for all the binds of the draw in one loop on the caller's stack - no stack switch, no body. What it
// leaves is what the bodies leave, word for word: the host test runs both on the same states (the bodies are the engine's own text)
// and DM_NRN_NOBODY=2 does it on the phone at every eligible draw. The draw itself - the tap's walk (nbt_draw) and the record
// (nbd_draw, rec / cmd_alloc) - stays the recorder's own code, called in place right after the fold (nvn_nrd_draw).
// A draw the fold cannot take (nrnb_can names why) runs the bodies as before; the decision is made before anything is written.
#ifdef DM_GAME_DOS2
#if NVN_NRNB_PART == 1
#include "nvn_nrn_b.h"
enum { NRNB_WT_MAX = 96, NRNB_ROOM = 16384 };   // use marks of one draw (3 stages x 32 slots); command memory the draw may need at most
static int g_nrnb;                               // DM_NRN_NOBODY as armed (nvn_nrnb_enable, install time)
static uint64_t g_nrnbs[NVN_NRNB_S_N];           // atomics
static unsigned g_nrnb_red;
static __thread struct { int on, n; uint64_t id[NRNB_WT_MAX]; } t_nrnb_cap;   // =2: the handles the bodies marked used, in order
static void nrnb_cap_wt(const uint64_t *h, int n) { for (int i = 0; i < n; i++) { if (t_nrnb_cap.n < NRNB_WT_MAX) t_nrnb_cap.id[t_nrnb_cap.n] = h[i]; t_nrnb_cap.n++; } }
// bind_handles, first line: every call with a texture / image op marks its handles used exactly once (wt_use_ids, on each of its paths).
#define NRNB_CAP_WT(op, h, n) do { if (__builtin_expect(t_nrnb_cap.on, 0) && ((op) == OP_BIND_TEX || (op) == OP_BIND_IMG)) nrnb_cap_wt((h), (n)); } while (0)
#else
#ifndef NRNB_MUTATE
#define NRNB_MUTATE 0
#endif
// What the fold reads and writes. live: the state itself (the use marks and the counters are applied as it goes); else copies (=2).
typedef struct {
    nrub_mirror *ubm; nrd_mirror *texm;
    uint32_t (*tex)[NBT_SLOTS]; uint64_t *tdirty, *tabsig;
    nbd_shadow *s; void **scb; nrn_short_ext *sx; uint32_t *nf;
    int tap, shrt, live;
    uint32_t texgen_k, nubo_note;      // out: binds that moved the texture-set generation; NBN_UBO notes
    uint64_t wt[NRNB_WT_MAX]; int nwt; // out (copies only): the handles marked used, in order
} nrnb_env;
// 0 = the fold may run for the binds of this draw into c; else NVN_NRNB_NO_*. Reads only (dm_nb_frame counts a call from a thread that
// is not the render thread - the bodies would call it too). *tap: the tap follows c.
static int nrnb_can(NVNcommandBuffer *c, const nru_call *ub, int nub, int ntb, int *tap) {
    const cmd_t *o = CB(c);
    *tap = 0;
    if (!nvn_nrd_can((uint64_t)(uintptr_t)c)) return NVN_NRNB_NO_MODE;        // DM_NR_DIRECT=1, inside the bracket, no check / capture mode, recording
    if (g_nru_cap || Z5G_ON() || g_nrub == 2 || g_nrd_texf == 2) return NVN_NRNB_NO_CAP;   // a capture / flag line of another step runs in the bodies
    if (NRNB_MUTATE != 9 && (t_nbd.s.h.nbind || t_nbd.s.h.flags || t_nbd.s.h.cond_mode || t_nbd.s.h.cond_addr || (t_nbd.cb && t_nbd.cb != o))) return NVN_NRNB_NO_SHADOW;
    if (g_dm_nb) {   // the tap: only when it already follows this command buffer (the first call of a recording claims it; another one is counted per call)
        if (!dm_nb_frame() || g_nbt.cb != (const void *)c) return NVN_NRNB_NO_TAP;
        *tap = 1;
    }
    // the mirrors: only when this thread owns them for c (the adoption takes their locks)
    if (NRNB_MUTATE != 10 && g_nrd_texf && ntb && (g_nrdm.cb != (const void *)c || g_nrdm.owner != (const void *)&t_nrd_wf)) return NVN_NRNB_NO_MIRROR;
    if (g_nrub && nub && (g_nrub_m.cb != (const void *)c || g_nrub_m.owner != (const void *)&t_nrub_scope)) return NVN_NRNB_NO_MIRROR;
    if (nub < 0 || ntb < 0 || nub + ntb > NBD_MAX_BINDS || ntb > NRNB_WT_MAX) return NVN_NRNB_NO_ARGS;
    for (int i = 0; i < nub; i++) if (ub[i].cb != (uint64_t)(uintptr_t)c || !nbd_bind_ok(NBD_B_UBO, (int)ub[i].stage, (int)ub[i].slot)) return NVN_NRNB_NO_ARGS;
    if (!o->cur || o->cur + NRNB_ROOM + OP_RESERVE > o->end) return NVN_NRNB_NO_ROOM;   // the game's callback for command memory is left to the big-stack path
    return 0;
}
// The binds, in call order. c = the command buffer (an address only).
static void nrnb_fold(nrnb_env *e, const void *c, const nru_call *ub, int nub, const nvn_nrnb_tex *tb, int ntb) {
    e->texgen_k = 0; e->nubo_note = 0; e->nwt = 0;
    for (int i = 0; i < nub; i++) {   // = nvnCommandBufferBindUniformBuffer(c, stage, slot, addr, size)
        const int st = (int)ub[i].stage, sl = (int)ub[i].slot; const uint64_t a = ub[i].addr, n = ub[i].size;
        if (g_nrub) {                                                        // NRUB_BIND1 -> nrub_bind_hook (the owner's bind, adopt = 1)
            const int same = nrub_bind(e->ubm, c, &t_nrub_scope, 1, st, sl, 1, &a, &n, NULL);
            if (e->live) { NRD_ADD(NVN_NRD_UB_SEEN, 1); NRD_TICK(); }
            if (t_nrub_scope) {
                if (e->live) NRD_ADD(NVN_NRD_UB_SCOPE, 1);
                if (nrub_filter(same, 1, t_nrub_scope, 1) && NRNB_MUTATE != 1) { if (e->live) NRD_ADD(NVN_NRD_UB_SKIP, 1); continue; }   // the backend holds the range: not sent
            }
        }
        if (e->tap) e->nubo_note++;                                           // NBT(nbt_note(c, NBN_UBO))
        *e->scb = (void *)(uintptr_t)c;                                       // nbd_take_bind: nbd_cb (the shadow is empty or already c's)
        nbd_add(e->s, NBD_B_UBO, st, sl, a, n);
    }
    for (int i = 0; i < ntb; i++) {   // = nvnCommandBufferBindTexture(c, stage, slot, nvnDeviceGetTextureHandle(tex, smp))
        const int st = tb[i].stage, sl = tb[i].slot;
        const uint64_t v = (uint64_t)tb[i].tex | (uint64_t)tb[i].smp << 20;
        if (e->live) NRD_ADD(NVN_NRD_BINDS, 1);
        if (e->tap && st < NBT_STAGES && sl < NBT_SLOTS) {                    // NBT(nbt_bind)
            const uint32_t id1 = ((uint32_t)v & (WT_IDS - 1)) + 1, was = e->tex[st][sl];
            if (was != id1) {
                if (g_nrd_tap && st < 5 && NRNB_MUTATE != 2) *e->tabsig ^= nrd_slot_sig(0, (unsigned)st, (unsigned)sl, was) ^ nrd_slot_sig(0, (unsigned)st, (unsigned)sl, id1);   // NRD_SIG
                e->tex[st][sl] = id1; if (NRNB_MUTATE != 3) e->tdirty[st] |= 1ull << sl;
            }
        }
        if (g_nrd_texf) {                                                     // NRD_BT -> nrdm_bind_hook (the owner's bind, adopt = 1)
            const int same = NRNB_MUTATE == 4 ? 0 : nrdm_bind(e->texm, c, &t_nrd_wf, 1, NRDM_TEX, st, sl, 1, &v);
            if (e->live) { NRD_ADD(NVN_NRD_BIND_SEEN, 1); NRD_TICK(); }
            if (nrdm_filter(same, 1, NRDM_TEX, 1) && nbd_bind_ok(NBD_B_TEX, st, sl)) {   // the backend holds the handle: not sent
                if (e->live) NRD_ADD(NVN_NRD_FILT, 1);
                if (*e->nf < 255 && NRNB_MUTATE != 5) (*e->nf)++;
                goto used;
            }
        }
        if (NRNB_MUTATE != 6) e->texgen_k++;                                  // NBD_TEXGEN
        if (e->shrt && NRNB_MUTATE != 7 && nrn_short_ext_tex(e->sx, st, sl)) {   // NRN_SHORT_BIND: the backend puts the slot back from the image of the card
            if (e->live) __atomic_fetch_add(&g_nrnps[NRNPS_TEX], 1, __ATOMIC_RELAXED);
            goto used;
        }
        *e->scb = (void *)(uintptr_t)c;
        nbd_add(e->s, NBD_B_TEX, st, sl, v, 0);                               // nbd_take_bind
    used:
        if (NRNB_MUTATE == 8 && i == 0) continue;
        if (e->live) wt_use_ids(&v, 1); else if (e->nwt < NRNB_WT_MAX) e->wt[e->nwt++] = v;
    }
}
void nvn_nrnb_enable(int mode) { g_nrnb = mode == 1 || mode == 2 ? mode : 0; }
void nvn_nrnb_stats(uint64_t out[NVN_NRNB_S_N]) { for (int i = 0; i < NVN_NRNB_S_N; i++) out[i] = __atomic_exchange_n(&g_nrnbs[i], 0, __ATOMIC_RELAXED); }
int nvn_nrnb_binds(uint64_t cb, const nru_call *ub, int nub, const nvn_nrnb_tex *tb, int ntb) {
    NVNcommandBuffer *c = (NVNcommandBuffer *)(uintptr_t)cb;
    nrnb_env e;
    const int why = g_nrnb == 1 ? nrnb_can(c, ub, nub, ntb, &e.tap) : NVN_NRNB_NO_MODE;
    if (why) { __atomic_fetch_add(&g_nrnbs[why], 1, __ATOMIC_RELAXED); return 0; }
    e.ubm = &g_nrub_m; e.texm = &g_nrdm; e.tex = g_nbt.tex; e.tdirty = g_nbt.tdirty; e.tabsig = &g_nrd_tabsig;
    e.s = &t_nbd.s; e.scb = (void **)&t_nbd.cb; e.sx = &t_nrn_sx; e.nf = &t_nrd_nf; e.shrt = t_nrn_short; e.live = 1;
    nrnb_fold(&e, c, ub, nub, tb, ntb);
    if (e.texgen_k) __atomic_add_fetch(&g_nbd_texgen, e.texgen_k, __ATOMIC_RELAXED);
    if (e.nubo_note) dm_nb_note(NBN_UBO, e.nubo_note);
    __atomic_fetch_add(&g_nrnbs[NVN_NRNB_FOLDS], 1, __ATOMIC_RELAXED); __atomic_fetch_add(&g_nrnbs[NVN_NRNB_UBO], (uint64_t)nub, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_nrnbs[NVN_NRNB_TEX], (uint64_t)ntb, __ATOMIC_RELAXED);
    return 1;
}
// ---- =2: the fold on copies against the bodies ----
typedef struct {
    int ok; const void *cb; nrnb_env e;
    nrub_mirror ubm; nrd_mirror texm;   // texm: the header and the texture rows only
    uint32_t tex[NBT_STAGES][NBT_SLOTS]; uint64_t tdirty[NBT_STAGES], tabsig;
    nbd_shadow s; void *scb; nrn_short_ext sx; uint32_t nf, texgen0;
} nrnb_snap_t;
static __thread nrnb_snap_t *t_nrnb_snap;
void nvn_nrnb_snap(uint64_t cb, const nru_call *ub, int nub, int ntb) {
    NVNcommandBuffer *c = (NVNcommandBuffer *)(uintptr_t)cb;
    nrnb_snap_t *z = t_nrnb_snap;
    if (!z) { z = t_nrnb_snap = (nrnb_snap_t *)calloc(1, sizeof *z); if (!z) return; }
    z->ok = 0; z->cb = c;
    const int why = g_nrnb == 2 ? nrnb_can(c, ub, nub, ntb, &z->e.tap) : NVN_NRNB_NO_MODE;
    if (why) { __atomic_fetch_add(&g_nrnbs[why], 1, __ATOMIC_RELAXED); return; }
    z->ubm = g_nrub_m;
    z->texm.cb = g_nrdm.cb; z->texm.owner = g_nrdm.owner; z->texm.epoch = g_nrdm.epoch;
    memcpy(z->texm.at[NRDM_TEX], g_nrdm.at[NRDM_TEX], sizeof z->texm.at[NRDM_TEX]); memcpy(z->texm.h[NRDM_TEX], g_nrdm.h[NRDM_TEX], sizeof z->texm.h[NRDM_TEX]);
    memcpy(z->tex, g_nbt.tex, sizeof z->tex); memcpy(z->tdirty, g_nbt.tdirty, sizeof z->tdirty); z->tabsig = g_nrd_tabsig;
    z->s.h = t_nbd.s.h; z->scb = t_nbd.cb; z->sx = t_nrn_sx; z->nf = t_nrd_nf; z->texgen0 = __atomic_load_n(&g_nbd_texgen, __ATOMIC_RELAXED);
    z->e.ubm = &z->ubm; z->e.texm = &z->texm; z->e.tex = z->tex; z->e.tdirty = z->tdirty; z->e.tabsig = &z->tabsig;
    z->e.s = &z->s; z->e.scb = &z->scb; z->e.sx = &z->sx; z->e.nf = &z->nf; z->e.shrt = t_nrn_short; z->e.live = 0;
    t_nrnb_cap.n = 0; t_nrnb_cap.on = 1;
    z->ok = 1;
}
void nvn_nrnb_check(uint64_t cb, const nru_call *ub, int nub, const nvn_nrnb_tex *tb, int ntb) {
    nrnb_snap_t *z = t_nrnb_snap;
    t_nrnb_cap.on = 0;
    if (!z || !z->ok || z->cb != (const void *)(uintptr_t)cb) return;
    z->ok = 0;
    nrnb_fold(&z->e, z->cb, ub, nub, tb, ntb);
    const char *what = NULL; unsigned long long a = 0, b = 0;
    const uint32_t moved = __atomic_load_n(&g_nbd_texgen, __ATOMIC_RELAXED) - z->texgen0;
#define NRNB_DIFF(cond, name, x, y) do { if (!what && (cond)) { what = (name); a = (unsigned long long)(x); b = (unsigned long long)(y); } } while (0)
    NRNB_DIFF(z->s.h.nbind != t_nbd.s.h.nbind || memcmp(&z->s.h, &t_nbd.s.h, sizeof z->s.h), "the header of the draw's shadow (binds)", z->s.h.nbind, t_nbd.s.h.nbind);
    for (unsigned i = 0; i < z->s.h.nbind && !what; i++) NRNB_DIFF(memcmp(&z->s.bind[i], &t_nbd.s.bind[i], sizeof z->s.bind[i]), "a bind of the draw's shadow (index, kind)", i, t_nbd.s.bind[i].kind);
    NRNB_DIFF(z->scb != (void *)t_nbd.cb, "the command buffer of the shadow", (uintptr_t)z->scb, (uintptr_t)t_nbd.cb);
    NRNB_DIFF(memcmp(&z->ubm, &g_nrub_m, sizeof z->ubm), "the mirror of the backend's uniform-buffer table", z->ubm.epoch, g_nrub_m.epoch);
    NRNB_DIFF(z->texm.cb != g_nrdm.cb || z->texm.owner != g_nrdm.owner || z->texm.epoch != g_nrdm.epoch, "the owner / epoch of the texture mirror", z->texm.epoch, g_nrdm.epoch);
    NRNB_DIFF(memcmp(z->texm.at[NRDM_TEX], g_nrdm.at[NRDM_TEX], sizeof z->texm.at[NRDM_TEX]) || memcmp(z->texm.h[NRDM_TEX], g_nrdm.h[NRDM_TEX], sizeof z->texm.h[NRDM_TEX]),
              "the texture rows of the mirror of the backend's binding tables", 0, 0);
    NRNB_DIFF(memcmp(z->tex, g_nbt.tex, sizeof z->tex), "the tap's texture table", 0, 0);
    NRNB_DIFF(memcmp(z->tdirty, g_nbt.tdirty, sizeof z->tdirty), "the tap's dirty masks", z->tdirty[1], g_nbt.tdirty[1]);
    NRNB_DIFF(z->tabsig != g_nrd_tabsig, "the signature of the tap's tables", z->tabsig, g_nrd_tabsig);
    NRNB_DIFF(memcmp(&z->sx, &t_nrn_sx, sizeof z->sx), "the left-out mask of the short draw", z->sx.tm[0], t_nrn_sx.tm[0]);
    NRNB_DIFF(z->nf != t_nrd_nf, "the count of texture binds not sent", z->nf, t_nrd_nf);
    NRNB_DIFF(z->e.nwt != t_nrnb_cap.n, "the number of use marks", z->e.nwt, t_nrnb_cap.n);
    for (int i = 0; i < z->e.nwt && i < NRNB_WT_MAX && !what; i++) NRNB_DIFF(z->e.wt[i] != t_nrnb_cap.id[i], "a use mark (fold, bodies)", z->e.wt[i], t_nrnb_cap.id[i]);
    NRNB_DIFF(moved < z->e.texgen_k, "the texture-set generation (fold, bodies)", z->e.texgen_k, moved);
#undef NRNB_DIFF
    __atomic_fetch_add(&g_nrnbs[NVN_NRNB_CMP], 1, __ATOMIC_RELAXED); __atomic_fetch_add(&g_nrnbs[NVN_NRNB_UBO], (uint64_t)nub, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_nrnbs[NVN_NRNB_TEX], (uint64_t)ntb, __ATOMIC_RELAXED);
    if (!what && moved != z->e.texgen_k) __atomic_fetch_add(&g_nrnbs[NVN_NRNB_TEXGEN_MOVED], 1, __ATOMIC_RELAXED);   // another thread bound a texture meanwhile: this thread's binds are inside the word
    if (!what) return;
    __atomic_fetch_add(&g_nrnbs[NVN_NRNB_BAD], 1, __ATOMIC_RELAXED);
    if (g_nrnb_red++ < 16) LOG("【红】[nr] native nobody: the fold is not what the NVN bodies left - %s: %#llx by the fold, %#llx by the bodies (%d uniform-buffer + %d texture binds, %s draw)",
                               what, a, b, nub, ntb, z->e.shrt ? "short" : "long");
}
#endif
#else
#if NVN_NRNB_PART == 1
#define NRNB_CAP_WT(op, h, n) ((void)0)
#endif
#endif
