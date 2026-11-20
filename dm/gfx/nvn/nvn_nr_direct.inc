// [DOS2] NR-09R (DM_NR_DIRECT, default 0; docs/dos2/nr/NR-09R.md): the NVN side of the direct engine draw. Included twice:
//   first (by nvn_nb_tap.inc, right after the tap's state g_nbt)   the switch, the counters, the tap memo (NRDP_TAP) and the hook
//                                       macros of the binding mirror (NRDP_TEXF). With DM_NR_DIRECT unset every hook is a load and a
//                                       not-taken branch and the command stream is byte for byte the old one.
//   second (NVN_NRD_ENTRIES, at the end of hle_nvn.c)               the mirror's bind hook and the direct recorder (NRDP_DRAW): the
//                                       texture binds of the engine's flush and its draw, recorded without the NVN calls.
// The recorder writes through the functions the entry points use (bind_handles, the tap, nbd_draw): the record is the OP_X_NB_DRAW +
// NR-09 tail the calls would have produced, minus the binds NRDP_TEXF does not send, plus the tail word of nr_direct.h (NRD_X_*).
#ifdef DM_GAME_DOS2
#ifndef NVN_NRD_ENTRIES
#include "../nr/nr_direct.h"
#include "../nr/nr_ubd.h"
#include "nvn_nr_direct.h"
static int g_nrd_mode;             // DM_NR_DIRECT as armed (nvn_nrd_enable, install time): 0 off, 1 on, 2 compare
static int g_nrd_tap, g_nrd_texf;  // the parts as armed: 0 off, else g_nrd_mode
// Counters: plain per thread, handed to the shared ones (atomics) once in 256 counted events of the thread - no atomic on the draw path.
static uint64_t g_nrdx[NVN_NRD_S_N];
static __thread uint64_t t_nrdx[NVN_NRD_S_N]; static __thread uint32_t t_nrdx_n;
#define NRD_ADD(k, n) (t_nrdx[k] += (uint64_t)(n))
static void nrd_cnt_flush(void) { for (int i = 0; i < NVN_NRD_S_N; i++) if (t_nrdx[i]) { __atomic_fetch_add(&g_nrdx[i], t_nrdx[i], __ATOMIC_RELAXED); t_nrdx[i] = 0; } }
#define NRD_TICK() do { if (__builtin_expect(!(++t_nrdx_n & 255), 0)) nrd_cnt_flush(); } while (0)
// ---- NRDP_TAP (the tap's thread only: nbt_f(c) != NULL) ----
static uint64_t g_nrd_tabsig;      // XOR of nrd_slot_sig over g_nbt.tex / img of stages 0..4 (kept by NRD_SIG in nbt_bind)
static uint32_t g_nrd_reggen;      // atomic: pool registrations (wt_register) - the image a bound id stands for may have changed
static nrd_tapmemo g_nrd_memo = { .node = -1 };
static unsigned g_nrd_tap_red;
#define NRD_SIG(w, st, s, was, now) do { if (__builtin_expect(g_nrd_tap, 0) && (st) < 5) g_nrd_tabsig ^= nrd_slot_sig((w), (unsigned)(st), (unsigned)(s), (was)) ^ nrd_slot_sig((w), (unsigned)(st), (unsigned)(s), (now)); } while (0)
#define NRD_REGGEN() do { if (__builtin_expect(g_nrd_tap, 0)) __atomic_fetch_add(&g_nrd_reggen, 1, __ATOMIC_RELAXED); } while (0)
typedef struct { int on, hit; uint64_t sig; uint32_t mw; int nref, pnref, nsr; uint8_t fsx; uint64_t fs; } nrd_tap_t;
static uint32_t g_nrd_mw;          // the slot-mask table's publish count the tap's cached masks (g_nbt.pknown) were looked up under
static uint32_t nrd_mask_writes(void) { const nb_sm_table *mt = xbg_nb_slotmask_table ? xbg_nb_slotmask_table() : NULL; return mt ? __atomic_load_n(&mt->writes, __ATOMIC_RELAXED) : 0u; }
// Before the walk of a draw of the open pass `node` (not its first draw: `all`; not a DrawTexture). 1 = skip the walk (=1 only).
static int nrd_tap_pre(nrd_tap_t *t, const nb_frame *f, int node, int all, uint64_t handle) {
    t->on = !handle; t->hit = 0;
    if (!t->on) return 0;
    NRD_ADD(NVN_NRD_TAP_DRAWS, 1); NRD_TICK();
    // A publish since the masks were cached: the masks of a bound program may be other ones now. They are looked up again (the cache
    // otherwise lasts until the next BindProgram), so the masks a walk uses are always the table's at the publish count in its signature.
    t->mw = nrd_mask_writes();
    if (t->mw != g_nrd_mw) { g_nrd_mw = t->mw; for (int st = 0; st < 5; st++) if (g_nbt.prog[st]) g_nbt.pknown[st] = 0; }
    t->sig = nrd_draw_sig(g_nrd_tabsig, g_nbt.prog, __atomic_load_n(&g_nrd_reggen, __ATOMIC_RELAXED), t->mw);
    uint8_t fl = 0;
    if (all || !nrd_tap_hit(&g_nrd_memo, node, g_nbt.gen, t->sig, &fl)) return 0;
    t->hit = 1; NRD_ADD(NVN_NRD_TAP_HIT, 1);
    if (g_nrd_tap == 1) { if (fl == 1) dm_nb_note(NBN_SM_KNOWN, 1); return 1; }   // the note the walk would have made (the '[nb] slotmask' line)
    const nb_node *p = &f->node[node];   // =2: the walk runs; it must change nothing (nrd_tap_post)
    t->nref = f->nref; t->pnref = p->nref; t->nsr = p->nsr; t->fsx = p->fsx; t->fs = p->fs;
    return 0;
}
// After the walk. known: nbt_mask_note's answer (1 every stage's masks known, 0 a stage unknown, 2 slot masks off); exact: nbt_prog_draw's
// (1 = the fs key is published and every sampled image was listed; always 1 without DM_NB_POST).
static void nrd_tap_post(const nrd_tap_t *t, const nb_frame *f, int node, int known, int exact) {
    if (!t->on) return;
    if (t->hit) {
        const nb_node *p = &f->node[node];
        if (f->nref != t->nref || p->nref != t->pnref || p->nsr != t->nsr || p->fsx != t->fsx || p->fs != t->fs) {
            NRD_ADD(NVN_NRD_TAP_BAD, 1);
            if (g_nrd_tap_red++ < 16) LOG("【红】[nr] direct check: tap - a draw =1 would not walk changed the open pass (node %d: reads %d -> %d, sampled %d -> %d, fs flags %#x -> %#x, "
                                          "fs %016llx -> %016llx; signature %016llx)", node, t->pnref, p->nref, t->nsr, p->nsr, t->fsx, p->fsx, (unsigned long long)t->fs, (unsigned long long)p->fs,
                                          (unsigned long long)t->sig);
        }
        return;
    }
    // a stage without published masks / fs key (its walk may add more later), or a publish while the walk ran: not remembered
    if (!known || !exact || nrd_mask_writes() != t->mw) { NRD_ADD(NVN_NRD_TAP_UNKNOWN, 1); return; }
    nrd_tap_learn(&g_nrd_memo, node, g_nbt.gen, t->sig, (uint8_t)known);
    NRD_ADD(NVN_NRD_TAP_LEARN, 1);
}
#define NRD_TAP_PRE(t, f, node, all, handle) (__builtin_expect(g_nrd_tap, 0) ? nrd_tap_pre(&(t), (f), (node), (all), (handle)) : ((t).on = 0))
#define NRD_TAP_POST(t, f, node, known, exact) do { if (__builtin_expect(g_nrd_tap, 0)) nrd_tap_post(&(t), (f), (node), (known), (exact)); } while (0)
// ---- NRDP_TEXF: the mirror of the backend's binding tables for the recording being written (nr_direct.h nrd_mirror) ----
static nrd_mirror g_nrdm;
static pthread_mutex_t g_nrdm_mu = PTHREAD_MUTEX_INITIALIZER;   // taken only off the fast path (a bind by a thread that does not own the mirror, Begin / nested)
static __thread uint32_t t_nrd_wf, t_nrd_nf;   // =2: binds of the shadow =1 would not send (bit = bind index < NRD_X_BITS); =1: binds not sent since the last draw record
static __thread int t_nrd_direct;              // the record being written comes from nvn_nrd_draw
static int nrdm_bind_hook(NVNcommandBuffer *c, int op, int st, int first, int n, const uint64_t *h);   // second part
static void nrdm_begin_hook(const void *cb);
static void nrdm_nested_hook(const void *cb);
// bind_handles: 0 = record the bind as before; 1 = the same, and it is one =1 would not send (=2, flagged by NRD_BT_FLAG once it sits in
// the shadow); 2 = do not send it (=1).
#define NRD_BT(c, op, st, first, n, h) (__builtin_expect(g_nrd_texf, 0) ? nrdm_bind_hook((c), (op), (st), (first), (n), (h)) : 0)
#define NRD_BT_FLAG(same) do { if (same) { const unsigned nrd_i_ = t_nbd.s.h.nbind - 1u; if (nrd_i_ < NRD_X_BITS) { t_nrd_wf |= 1u << nrd_i_; NRD_ADD(NVN_NRD_WOULD_FLAG, 1); } else NRD_ADD(NVN_NRD_WOULD_PAST, 1); } } while (0)
#define NRD_BT_BEGIN(o) do { if (__builtin_expect(g_nrd_texf, 0)) nrdm_begin_hook(o); } while (0)
#define NRD_BT_NESTED(o) do { if (__builtin_expect(g_nrd_texf, 0)) nrdm_nested_hook(o); } while (0)
// ---- NR-UBD (DM_NR_UBD): the mirror of the backend's uniform-buffer table for the recording being written (nr_ubd.h nrub_mirror) ----
static int g_nrub;                  // DM_NR_UBD as armed (nvn_nrub_enable, install time): 0 off, 1 on, 2 compare
static nrub_mirror g_nrub_m;
static pthread_mutex_t g_nrub_mu = PTHREAD_MUTEX_INITIALIZER;   // taken only off the fast path (adopt / another thread / Begin / nested)
static __thread int t_nrub_scope;   // this thread serves a direct draw now (nvn_nrub_scope: the front end, around nrd_draw_take's work)
static int nrub_bind_hook(NVNcommandBuffer *c, int st, int first, int n, const uint64_t *a, const uint64_t *sz, int filt);   // second part
static int nrub_bind1_hook(NVNcommandBuffer *c, int st, int slot, uint64_t a, uint64_t sz);
static void nrub_begin_hook(const void *cb);
static void nrub_nested_hook(const void *cb);
// BindUniformBuffer: 0 = record the bind as before; 1 = the same, and it is one =1 would not send (=2, flagged by NRUB_FLAG once it sits
// in the shadow); 2 = do not send it (=1). BindUniformBuffers: the mirror learns the ranges (never filtered).
#define NRUB_BIND1(c, st, i, a, n) (__builtin_expect(g_nrub, 0) ? nrub_bind1_hook((c), (int)(st), (i), (uint64_t)(a), (uint64_t)(n)) : 0)
#define NRUB_BINDS(c, st, first, n, rg) do { if (__builtin_expect(g_nrub, 0)) { uint64_t nrub_a_[32], nrub_n_[32]; const int nrub_k_ = (n) < 0 ? 0 : (n) > 32 ? 32 : (n); \
    for (int nrub_j_ = 0; nrub_j_ < nrub_k_; nrub_j_++) { nrub_a_[nrub_j_] = (uint64_t)(rg)[nrub_j_].address; nrub_n_[nrub_j_] = (uint64_t)(rg)[nrub_j_].size; } \
    (void)nrub_bind_hook((c), (int)(st), (first), nrub_k_, nrub_a_, nrub_n_, 0); } } while (0)
#define NRUB_FLAG(same) do { if (same) { const unsigned nrub_i_ = t_nbd.s.h.nbind - 1u; if (nrub_i_ < NRD_X_BITS) { t_nrd_wf |= 1u << nrub_i_; NRD_ADD(NVN_NRD_UB_FLAG, 1); } else NRD_ADD(NVN_NRD_UB_PAST, 1); } } while (0)
#define NRUB_BEGIN(o) do { if (__builtin_expect(g_nrub, 0)) nrub_begin_hook(o); } while (0)
#define NRUB_NESTED(o) do { if (__builtin_expect(g_nrub, 0)) nrub_nested_hook(o); } while (0)
// nbd_reset (the shadow starts again: after a draw record or a flush): the flags belong to the binds that just left.
#define NRD_WF_RESET() do { t_nrd_wf = 0; } while (0)
// nbd_draw: the tail word of the record being written (0 without DM_NR_DIRECT).
static inline uint32_t nrd_ext_take(void) {
    uint32_t x = t_nrd_direct ? NRD_X_DIRECT : 0u;
    if (g_nrd_texf == 2 || t_nrd_wf) { x = nrd_x_chk(x, t_nrd_wf); t_nrd_nf = 0; }   // t_nrd_wf without TEXF=2: NR-UBD=2's flags (this record's =1 count is dropped)
    else if (g_nrd_texf == 1) { x = nrd_x_filt(x, t_nrd_nf); t_nrd_nf = 0; }
    return x;
}
#define NRD_EXT_PUT(nr, p, size) do { if (__builtin_expect(g_nrd_mode, 0) && (nr)) { const uint32_t nrd_x_ = nrd_ext_take(); if (nrd_x_) nrd_tail_ext_put((p), (size), nrd_x_); } } while (0)
#else
// ======== second part: the mirror's hooks and the direct recorder ========
static int nrdm_bind_hook(NVNcommandBuffer *c, int op, int st, int first, int n, const uint64_t *h) {
    const unsigned kind = op == OP_BIND_TEX ? NRDM_TEX : op == OP_BIND_SAMP ? NRDM_SAMP : NRDM_IMG;
    const void *me = &t_nrd_wf;   // any address of this thread
    // Inside the bracket of an engine draw this thread is the one that records engine draws into c: the mirror follows it. NB-10's check
    // bracket and the replay of the shadow (emitting) record every bind as before and take no part.
    const int in = g_nbd_on && t_nbd.in && !t_nbd.emitting, adopt = in && !t_nbd_chk;
    if (__builtin_expect(!adopt && (g_nrdm.cb != (const void *)c || g_nrdm.owner != me), 0)) {
        if (g_nrdm.cb != (const void *)c) return 0;   // another command buffer: nothing known, nothing to forget
        pthread_mutex_lock(&g_nrdm_mu);                // another thread writes into the tracked recording
        if (g_nrdm.cb == (const void *)c && g_nrdm.owner != me) { nrdm_forget(&g_nrdm); NRD_ADD(NVN_NRD_FORGET, 1); nrd_cnt_flush(); }
        pthread_mutex_unlock(&g_nrdm_mu);
        return 0;
    }
    if (adopt && (g_nrdm.cb != (const void *)c || g_nrdm.owner != me)) {
        pthread_mutex_lock(&g_nrdm_mu);
        const int same0 = nrdm_bind(&g_nrdm, c, me, 1, kind, st, first, n, h); (void)same0;   // adopts: a fresh table, nothing is the same
        pthread_mutex_unlock(&g_nrdm_mu);
        NRD_ADD(NVN_NRD_BIND_SEEN, n);
        return 0;
    }
    const int same = nrdm_bind(&g_nrdm, c, me, adopt, kind, st, first, n, h);
    NRD_ADD(NVN_NRD_BIND_SEEN, n); NRD_TICK();
    // Only what DM_NR_DIRECT=2 can check on the backend is filtered: a single texture bind that goes into an engine draw's record.
    if (!nrdm_filter(same, adopt, kind, n) || !nbd_bind_ok(NBD_B_TEX, st, first)) return 0;
    if (g_nrd_texf == 1) { NRD_ADD(NVN_NRD_FILT, 1); if (t_nrd_nf < 255) t_nrd_nf++; return 2; }
    NRD_ADD(NVN_NRD_WOULD, 1);
    return 1;
}
static void nrdm_begin_hook(const void *cb) {
    if (g_nrdm.cb != cb) return;
    pthread_mutex_lock(&g_nrdm_mu); nrdm_begin(&g_nrdm, cb); pthread_mutex_unlock(&g_nrdm_mu);
}
static void nrdm_nested_hook(const void *cb) {
    if (g_nrdm.cb != cb) return;
    pthread_mutex_lock(&g_nrdm_mu); if (g_nrdm.cb == cb) { nrdm_nested(&g_nrdm, cb); NRD_ADD(NVN_NRD_FORGET, 1); nrd_cnt_flush(); } pthread_mutex_unlock(&g_nrdm_mu);
}
// NR-UBD. Every UBO bind of a thread passes here; only the owner's binds into the tracked recording touch the table without the lock.
static int nrub_bind_hook(NVNcommandBuffer *c, int st, int first, int n, const uint64_t *a, const uint64_t *sz, int filt) {
    const void *me = &t_nrub_scope;   // any address of this thread
    // Inside the bracket of an engine draw this thread records engine draws into c: the mirror follows it. NB-10's check bracket, the
    // replay of the shadow (emitting) and a DM_NATIVE_DRAW=1 draw record every bind as before and take no part.
    const int in = g_nbd_on && t_nbd.in && !t_nbd.emitting, adopt = in && !t_nbd_chk && !t_nd.mode;
    if (__builtin_expect(nrub_slow(&g_nrub_m, c, me, adopt), 0)) {   // adopt (a fresh table: nothing is the same) / another thread into the tracked recording
        int forgot = 0;
        pthread_mutex_lock(&g_nrub_mu);
        (void)nrub_bind(&g_nrub_m, c, me, adopt, st, first, n, a, sz, &forgot);
        pthread_mutex_unlock(&g_nrub_mu);
        if (forgot) { NRD_ADD(NVN_NRD_UB_FORGET, 1); nrd_cnt_flush(); } else if (adopt) NRD_ADD(NVN_NRD_UB_ADOPT, 1);
        return 0;
    }
    if (g_nrub_m.cb != (const void *)c) return 0;   // another command buffer, not adopted: its records do not touch the tracked recording
    const int same = nrub_bind(&g_nrub_m, c, me, adopt, st, first, n, a, sz, NULL);
    NRD_ADD(NVN_NRD_UB_SEEN, n); NRD_TICK();
    if (!filt || !t_nrub_scope) return 0;
    NRD_ADD(NVN_NRD_UB_SCOPE, 1);
    // Only what DM_NR_UBD=2 can check on the backend is left out: a single bind that goes into a served direct draw's record.
    if (!nrub_filter(same, adopt, t_nrub_scope, n)) return 0;
    if (g_nrub == 1) { NRD_ADD(NVN_NRD_UB_SKIP, 1); return 2; }
    NRD_ADD(NVN_NRD_UB_WOULD, 1);
    return 1;
}
static int nrub_bind1_hook(NVNcommandBuffer *c, int st, int slot, uint64_t a, uint64_t sz) { return nrub_bind_hook(c, st, slot, 1, &a, &sz, 1); }
static void nrub_begin_hook(const void *cb) {
    if (g_nrub_m.cb != cb) return;
    pthread_mutex_lock(&g_nrub_mu); nrub_begin(&g_nrub_m, cb); pthread_mutex_unlock(&g_nrub_mu);
}
static void nrub_nested_hook(const void *cb) {
    if (g_nrub_m.cb != cb) return;
    pthread_mutex_lock(&g_nrub_mu); if (g_nrub_m.cb == cb) { nrub_nested(&g_nrub_m, cb); NRD_ADD(NVN_NRD_UB_FORGET, 1); nrd_cnt_flush(); } pthread_mutex_unlock(&g_nrub_mu);
}
void nvn_nrub_enable(int mode) { g_nrub = mode == 1 || mode == 2 ? mode : 0; }
void nvn_nrub_scope(int on) { t_nrub_scope = on; }
void nvn_nrd_enable(int mode, int parts) {
    g_nrd_tap = (parts & NRDP_TAP) ? mode : 0; g_nrd_texf = (parts & NRDP_TEXF) ? mode : 0;
    g_nrd_mode = mode;
}
void nvn_nrd_stats(uint64_t out[NVN_NRD_S_N]) { for (int i = 0; i < NVN_NRD_S_N; i++) out[i] = __atomic_exchange_n(&g_nrdx[i], 0, __ATOMIC_RELAXED); }
int nvn_nrd_ready(const void *thunk[NVN_NC_N]) {
    static const char *const name[NVN_NC_N] = {"nvnDeviceGetTextureHandle", "nvnCommandBufferBindTexture", "nvnCommandBufferDrawElements",
                                               "nvnCommandBufferDrawElementsInstanced", "nvnCommandBufferSetRenderEnable"};
    void *const want[NVN_NC_N] = {(void *)h_nvnDeviceGetTextureHandle, (void *)h_nvnCommandBufferBindTexture, (void *)h_nvnCommandBufferDrawElements,
                                  (void *)h_nvnCommandBufferDrawElementsInstanced, (void *)h_nvnCommandBufferSetRenderEnable};
    void *const *tab = nvn_thunk_tab();
    int ok = 1;
    for (int k = 0; k < NVN_NC_N; k++) {
        const int i = nc_slot(name[k]);
        thunk[k] = i >= 0 ? tab[i] : NULL;
        if (i < 0 || nc_impl(i) != want[k]) ok = 0;
    }
    return ok;
}
int nvn_nrd_can(uint64_t cb) {
    const cmd_t *o = (const cmd_t *)(uintptr_t)cb;
    return g_nrd_mode == 1 && g_nbd_on && t_nbd.in && !t_nbd.emitting && !t_nbd_chk && !t_nd.mode && !g_nc_cap && o && o->magic == MAGIC('C', 'M', 'D', 'B') && o->recording;
}
// = h_nvnCommandBufferBindTexture(cb, stage, slot, h_nvnDeviceGetTextureHandle(tex, smp)) without NR-23's capture line (off: nvn_nrd_can).
void nvn_nrd_bind(uint64_t cb, int stage, int slot, int32_t tex, int32_t smp) {
    uint64_t v = (uint64_t)h_nvnDeviceGetTextureHandle(NULL, tex, smp);
    NRD_ADD(NVN_NRD_BINDS, 1);
    bind_handles((NVNcommandBuffer *)(uintptr_t)cb, OP_BIND_TEX, stage, slot, 1, &v);
}
// = h_nvnCommandBufferDrawElements / DrawElementsInstanced(cb, prim, itype, count, ib[, 0, 0, instances]) without the lines that are off
// here (NR-23's capture and DM_NATIVE_DRAW=2's check: nvn_nrd_can) and without the NR-02 tap: the front end has handed the identity over.
void nvn_nrd_draw(uint64_t cb, int32_t prim, uint32_t itype, int32_t count, uint64_t ib, int32_t instances, int timed) {
    NVNcommandBuffer *c = (NVNcommandBuffer *)(uintptr_t)cb;
    const int inst = instances < 0 ? 1 : instances;
    const uint64_t t0 = timed ? now_ns() : 0;
    NBT(nbt_draw(c, 0));
    const uint64_t t1 = timed ? now_ns() : 0;
    t_nrd_direct = 1;
#ifdef NBS_STATE_TIMED
    t_nbs_time = timed; t_nbs_ns = 0;   // [NR-CPU] the state block's share of the record time
#endif
    const int taken = NBD_TAKE(c) && nbd_draw(c, NBD_DRAW_IDX, prim, (int)itype, count, 0, 0, inst, ib, 0);
    t_nrd_direct = 0;
#ifdef NBS_STATE_TIMED
    t_nbs_time = 0; if (timed) NRD_ADD(NVN_NRD_NS_STATE, t_nbs_ns);
#endif
    if (!taken) {   // the bracket went away between nvn_nrd_can and here (it cannot: same thread, no call in between) - the plain record
        if (t_nrn_short) { LOG("【红】[nr] native: a short draw lost its bracket - its texture binds were not recorded"); t_nrn_short = 0; }
        *REC(c, OP_DRAW_IDX, p_draw_idx) = (p_draw_idx){prim, (int)itype, count, 0, 0, inst, ib};
        NRD_ADD(NVN_NRD_PLAIN, 1);
    }
    NRD_ADD(NVN_NRD_DRAWS, 1);
    if (timed) { const uint64_t t2 = now_ns(); NRD_ADD(NVN_NRD_TIMED, 1); NRD_ADD(NVN_NRD_NS_TAP, t1 - t0); NRD_ADD(NVN_NRD_NS_REC, t2 - t1); nrd_cnt_flush(); }
}
#endif
#else
#ifndef NRD_BT
#define NRD_SIG(w, st, s, was, now) ((void)0)
#define NRD_REGGEN() ((void)0)
#define NRD_BT(c, op, st, first, n, h) 0
#define NRD_BT_FLAG(same) ((void)0)
#define NRD_BT_BEGIN(o) ((void)0)
#define NRD_BT_NESTED(o) ((void)0)
#define NRD_WF_RESET() ((void)0)
#define NRD_EXT_PUT(nr, p, size) ((void)0)
#define NRUB_BIND1(c, st, i, a, n) 0
#define NRUB_BINDS(c, st, first, n, rg) ((void)0)
#define NRUB_FLAG(same) ((void)0)
#define NRUB_BEGIN(o) ((void)0)
#define NRUB_NESTED(o) ((void)0)
#endif
#endif
