// [DOS2] NR-09R: the engine draw recorded directly (DM_NR_DIRECT, default 0; docs/dos2/nr/NR-09R.md). Included by hle_dos2_nr.c after
// hle_dos2_nr_nocall.inc (uses the model of NR-23 - nr_nocall.h, NC_Z, dn_draw_native -, nk_ident / nk_site / t_nk / g_nk of NR-02 / NR-09,
// NR_INC / NR_GET). main = main.raw of update v786432, build 4979b200.
//   DM_NR_DIRECT=0 / unset  nothing: the hooks, the NVN entry points and the command stream are byte for byte the old ones.
//   DM_NR_DIRECT=1          needs DM_NR=1|2 and DM_NR_DRAW=1|2 (the identity and the OP_X_NR_DRAW record), DM_NB_OPS bits 36 / 37.
//       NRDP_DRAW  (1) the engine's draw functions 0x928e80 / 0x928fb0 do not run for a draw the recorder can take: their index buffer
//                      lookup, the texture flush 0x924730, SetRenderEnable, the draw call and the flag byte run natively (the model of
//                      NR-23, proven against the machine code by tests/nr_nocall_oracle.py) and go to the recorder in one call on the
//                      big stack (nvn_nr_direct.inc): no GetTextureHandle / BindTexture / DrawElements(Instanced) call of the engine,
//                      no thunk, no flush-hook entry, the identity handed over directly. The uniform-block functions 0x926f90 /
//                      0x925740 stay the engine's (5.9 KB with the ring allocator and its spin lock, NR-23 §1): their
//                      BindUniformBuffer calls still come through the thunks into the draw's shadow.
//       NRDP_TEXF  (2) a texture bind of an engine draw whose slot already holds that handle in the backend's table is not sent.
//       NRDP_TAP   (4) the frame-IR tap does not walk the bound slots of a draw whose tables and programs the open pass already holds.
//       NRDP_IDENT (8) the NR-09 identity without a lock or an atomic on this thread (only without DM_NR_KEY's census and with
//                      DM_NR_DRAW=1: the texture-set id, which nothing reads then, is not computed).
//       DM_NR_DIRECT_PARTS: the mask (default 15).
//   DM_NR_DIRECT=2          everything runs the old way and what =1 would do is computed next to it and compared (red lines, first 16):
//                      the engine's NVN calls of a draw against the model (NR-23's compare, armed with it: '[nr] nocall check'), the
//                      binds =1 would not send against the backend's table ('[nr] direct check', backend), the tap's walk at a draw
//                      =1 skips (it must change nothing), the lean identity against the old one.
//   There is no =3: =1 sends the same records minus binds that change nothing; nothing stale is left that a poison could show.
// A draw the recorder cannot take (no index buffer record, not inside the NB-09 bracket, a check mode of another step on it, the NVN
// pointers of the engine are not our thunks) runs the engine's function, untouched: the decision is made before anything is written.
#include "../gfx/nr/nr_direct.h"
#include "../gfx/nvn/nvn_nr_direct.h"

_Static_assert(sizeof(nrd_key) == sizeof(nr_dkey) && offsetof(nrd_key, blend_arg) == offsetof(nr_dkey, blend_arg) && offsetof(nrd_key, rtfmt) == offsetof(nr_dkey, rtfmt) &&
               offsetof(nrd_key, prim) == offsetof(nr_dkey, prim) && offsetof(nrd_key, alpha_test) == offsetof(nr_dkey, alpha_test) &&
               offsetof(nr_dkey, shader) == 0 && offsetof(nr_dkey, vformat) == 32, "nr_direct.h nrd_key = nr_key.h nr_dkey");
_Static_assert(NR_RT_COLORS == 8, "nrd_raw.rt = 8 colour targets + depth");

static int g_nrd_want, g_nrd_parts = NRDP_ALL;
static int g_nrd_live;         // atomic: 0 = the engine's NVN pointers not looked at yet, 1 = verified, -1 = not ours (never direct)
static uint64_t g_nrd_base;    // main's load address
static const uint8_t *g_nrd_isz;
static nc_guest_fn g_nrd_ubo_fill, g_nrd_ubo_bind;
enum { NRDC_D36, NRDC_D37, NRDC_NOREC, NRDC_NOCAN, NRDC_NOTLIVE, NRDC_FLUSH_ENGINE, NRDC_BINDS, NRDC_REN, NRDC_TIMED, NRDC_NS, NRDC_NS_UBO, NRDC_NS_ID, NRDC_NS_REC,
       NRDC_ID_CMP, NRDC_ID_BAD, NRDC_N };
static uint64_t g_nrdc[NRDC_N];                             // atomics, fed from the thread's counters once in 64 draws
static __thread struct { uint32_t n; uint64_t c[NRDC_N]; } t_nrd;
static __thread nrd_idc *t_nrdi;                            // the thread's key table (allocated at its first engine draw)
static nrd_idc *g_nrd_idc[8]; static int g_nrd_nidc;        // for the report (g_nk_mu)
static unsigned g_nrd_red;

static void nrd_count_flush(void) {
    for (int i = 0; i < NRDC_N; i++) if (t_nrd.c[i]) { __atomic_fetch_add(&g_nrdc[i], t_nrd.c[i], __ATOMIC_RELAXED); t_nrd.c[i] = 0; }
}

// ---- NRDP_IDENT: the identity of an engine draw (what nk_ident does, without its texture set, locks only for a key this thread has not seen) ----
static nrd_idc *nrd_idc_get(void) {
    if (t_nrdi) return t_nrdi;
    nrd_idc *c = (nrd_idc *)calloc(1, sizeof *c);
    if (!c) return NULL;
    pthread_mutex_lock(&g_nk_mu);
    if (g_nrd_nidc < 8) g_nrd_idc[g_nrd_nidc++] = c;
    pthread_mutex_unlock(&g_nk_mu);
    return t_nrdi = c;
}
static void nrd_raw_read(const uint8_t *R, nrd_raw *w) {
    memcpy(&w->shader, R + NK_R_SHADER, 8); memcpy(&w->blend, R + NK_R_BLEND, 8); memcpy(&w->depth, R + NK_R_DEPTH, 8);
    memcpy(&w->raster, R + NK_R_RASTER, 8); memcpy(&w->vformat, R + NK_R_VFORMAT, 8);
    memcpy(w->rt, R + NK_R_COLOR, 8 * NR_RT_COLORS); memcpy(&w->rt[NR_RT_COLORS], R + NK_R_DEPTH_RT, 8);
    memcpy(&w->blend_arg, R + NK_R_BLEND_ARG, 4);
    float aref; memcpy(&aref, R + NK_R_ALPHA_REF, 4);
    w->prim = R[NK_R_PRIM]; w->alpha_test = aref >= 0.f; w->pad[0] = w->pad[1] = 0;
}
// 1 = *id / *why are the key id and NR_NE_* mask of the draw the renderer r is about to make (as nk_ident computes them); 0 = no memory
// for the thread's table (the caller takes the old path).
static int nrd_ident_core(uint64_t r, int kind, uint32_t *id, uint32_t *why) {
    nrd_idc *c = nrd_idc_get();
    if (!c) return 0;
    nrd_raw w; nrd_raw_read((const uint8_t *)(uintptr_t)r, &w);
    int rt_same = 0;
    if (!nrd_ident_same(c, &w, id, why, &rt_same)) {
        uint16_t rtf = c->last_rtf;
        if (!rt_same || !rtf) {
            pthread_mutex_lock(&g_nk_mu); pthread_mutex_lock(&g_nr_mu);
            rtf = nr_rtfmt_note(g_nk, g_reg, w.rt, w.rt[NR_RT_COLORS]);
            pthread_mutex_unlock(&g_nr_mu); pthread_mutex_unlock(&g_nk_mu);
            c->rt++;
        }
        nrd_key k;
        if (!nrd_ident_find(c, &w, rtf, &k, id, why)) {
            nr_dkey dk; memcpy(&dk, &k, sizeof dk);
            int fresh = 0;
            pthread_mutex_lock(&g_nk_mu);
            const nr_kent *e = nr_key_note(g_nk, NULL, &dk, &fresh);
            if (!e && fresh < 0) { pthread_mutex_lock(&g_nr_mu); e = nr_key_note(g_nk, g_reg, &dk, &fresh); pthread_mutex_unlock(&g_nr_mu); }
            *id = e ? e->id : 0; *why = e ? e->why : NR_NE_LOST;
            pthread_mutex_unlock(&g_nk_mu);
            nrd_ident_put(c, &w, rtf, &k, *id, *why);
        }
    }
    if (kind == NR_DS_FULL) *why |= NR_NE_FULL;
    return 1;
}
// The identity of the engine draw at call site si of the texture flush (index into k_nk_site, < 0 = unknown), left for the NVN draw
// that follows (t_nk.idp, as nk_ident leaves it; the texture-set id is 0: nothing reads it with DM_NR_DRAW=1).
static void nrd_ident(uint64_t r, int si) {
    const int kind = si >= 0 ? k_nk_site[si].kind : NR_DS_UNKNOWN;
    if (!nr_dsite_is_draw(kind)) return;   // a flush alone / a dispatch: no NVN draw follows
    uint32_t id = 0, why = 0;
    if (!nrd_ident_core(r, kind, &id, &why)) { nk_ident(r, si >= 0 ? g_nk_base + k_nk_site[si].bl + 4 : 0); return; }
    t_nk.idp = 1; t_nk.id_key = id; t_nk.id_ts = 0; t_nk.id_why = why; t_nk.id_site = si >= 0 ? (uint32_t)si : 0xffu; NRN_NOTE(r);
}
static void nrd_ident_lr(uint64_t r, uint64_t lr) { nrd_ident(r, nk_site(lr)); }   // the flush hook (nk_draw) with g_nrd_lean == 1
// DM_NR_DIRECT=2, after nk_ident: the lean identity of the same draw against the one nk_ident left.
static void nrd_ident_cmp(uint64_t r, uint64_t lr) {
    const int si = nk_site(lr), kind = si >= 0 ? k_nk_site[si].kind : NR_DS_UNKNOWN;
    if (!nr_dsite_is_draw(kind) || !t_nk.idp) return;
    uint32_t id = 0, why = 0;
    if (!nrd_ident_core(r, kind, &id, &why)) return;
    NR_INC(g_nrdc[NRDC_ID_CMP]);
    if (id == t_nk.id_key && why == t_nk.id_why) return;
    NR_INC(g_nrdc[NRDC_ID_BAD]);
    if (__atomic_fetch_add(&g_nrd_red, 1, __ATOMIC_RELAXED) < 16)
        LOG("【红】[nr] direct check: identity - the draw at main+%#x: key id %u why %#x by the lean path, %u / %#x by nk_ident", si >= 0 ? k_nk_site[si].bl : 0u, id, why,
            t_nk.id_key, t_nk.id_why);
}

// ---- NRDP_DRAW: the engine's NVN pointers against the bodies the recorder stands for (once) ----
static int nrd_verify(void) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    int v = __atomic_load_n(&g_nrd_live, __ATOMIC_RELAXED);
    if (!v) {
        static const uint32_t slot[NVN_NC_N] = {NRNC_SLOT_HANDLE, NRNC_SLOT_BIND, NRNC_SLOT_DRAW, NRNC_SLOT_DRAWI, NRNC_SLOT_REN};
        const void *th[NVN_NC_N];
        const int ok = nvn_nrd_ready(th); int same = 1;
        uint64_t got[NVN_NC_N];
        for (int k = 0; k < NVN_NC_N; k++) { got[k] = nrnc_r64((const uint8_t *)(uintptr_t)(g_nrd_base + slot[k])); if (!th[k] || got[k] != (uint64_t)(uintptr_t)th[k]) same = 0; }
        v = ok && same ? 1 : -1;
        if (v > 0) LOG("[nr] direct: the engine's GetTextureHandle / BindTexture / DrawElements / DrawElementsInstanced / SetRenderEnable pointers are the thunks of the "
                       "bodies the direct recorder stands for - engine draws are recorded directly");
        else LOG("【红】[nr] direct: %s (GetTextureHandle %#llx / %p, BindTexture %#llx / %p, DrawElements %#llx / %p, DrawElementsInstanced %#llx / %p, SetRenderEnable %#llx / %p): "
                 "the engine's draw functions keep running, nothing is recorded directly",
                 !ok ? "an NVN entry point is not implemented by the body the recorder stands for" : "an engine NVN pointer is not our thunk",
                 (unsigned long long)got[0], th[0], (unsigned long long)got[1], th[1], (unsigned long long)got[2], th[2], (unsigned long long)got[3], th[3],
                 (unsigned long long)got[4], th[4]);
        __atomic_store_n(&g_nrd_live, v, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&mu);
    return v > 0;
}
static inline int nrd_live(void) {
    const int v = __atomic_load_n(&g_nrd_live, __ATOMIC_RELAXED);
    if (__builtin_expect(v != 0, 1)) return v > 0;
    return nrd_verify();
}

// ---- the model's NVN calls = the recorder ----
typedef struct { uint8_t *R; const nrnc_plan *p; uint64_t cb; const nrnc_draw *d; int timed; const nru_run *ub; } nrd_job_t;   // ub: NR-09U's calls
static void nrd_op_bind(void *u, uint64_t cb, int stage, int slot, int32_t tex, int32_t smp) { (void)u; nvn_nrd_bind(cb, stage, slot, tex, smp); }
static void nrd_op_ren(void *u, uint64_t cb) { (void)u; nvn_nc_ren(cb); }   // SetRenderEnable(cb, 1): the body of the entry point (rare: after a culled draw)
static void nrd_op_draw(void *u, uint64_t cb, int32_t prim, uint32_t itype, int32_t count, uint64_t ib, int32_t instances) {
    dm_nr_tap_draw((const void *)(uintptr_t)cb);   // what the NR-02 tap line of the entry point does: the pending identity becomes this draw's
    nvn_nrd_draw(cb, prim, itype, count, ib, instances, ((const nrd_job_t *)u)->timed);
}
static const nrnc_ops k_nrd_ops = {nrd_op_bind, nrd_op_ren, nrd_op_draw};
static void *nrd_job(void *a) {   // on the big stack: [the flush], SetRenderEnable, the draw, the flag byte - in the machine code's order
    const nrd_job_t *j = (const nrd_job_t *)a;
    NRND_TJ(1);   // [DOS2] NR-NATIVE step D DM_NRN_REC=2: the parts of this call, timed (nothing without the switch)
    if (j->ub) nru_job_calls(j->ub);   // NR-09U DM_NR_UBO=1: the uniform blocks' BindUniformBuffer calls (the engine made them before the flush)
    NRND_TJ(2);   // [DOS2] NR-NATIVE step D
    if (j->p) nrnc_flush_run(j->R, j->p, j->cb, &k_nrd_ops, a);
    NRND_TJ(3);   // [DOS2] NR-NATIVE step D
    nrnc_draw_tail(j->R, j->d, g_nrd_isz, &k_nrd_ops, a);
    NRND_TJ(4);   // [DOS2] NR-NATIVE step D
    return NULL;
}
// The native handler of the shared draw hook (hle_dos2_native.c), inside the NB-09 bracket of the draw.
// op 36: 0x928e80(r, first index, count); op 37: 0x928fb0(r, -, instances, -, count). 1 = recorded (the engine's function must not run);
// 0 = nothing was touched, the engine's function runs.
static int nrd_draw_take(unsigned op, uint64_t r, uint64_t a1, uint64_t a2, uint64_t a4) {
    uint8_t *R = (uint8_t *)(uintptr_t)r;
    const int32_t first = op == 36 ? (int32_t)a1 : 0, count = op == 36 ? (int32_t)a2 : (int32_t)a4, inst = op == 36 ? -1 : (int32_t)a2;
    const int timed = !(t_nrd.n++ & 63);
    if (timed) nrd_count_flush();
    if (!nrd_live()) { t_nrd.c[NRDC_NOTLIVE]++; return 0; }
    nrnc_draw d;
    if (!nrnc_draw_pre(R, (int)op, first, count, inst, &d)) { t_nrd.c[NRDC_NOREC]++; return 0; }
    if (!nvn_nrd_can(d.cb)) { t_nrd.c[NRDC_NOCAN]++; return 0; }
    // from here on the draw is ours: the engine's function does not run
    if (g_nrd_ubd) nvn_nrub_scope(1);   // NR-UBD: its BindUniformBuffer calls (the engine's functions or NR-09U's model, in the job) are a served draw's
    const uint64_t t0 = timed ? nr_now() : 0;
    const nru_run *ub = NULL;
    if (d.ubo) { if (g_nru) ub = nru_take(r, g_nrd_ubo_fill, g_nrd_ubo_bind);           // NR-09U DM_NR_UBO: the model (its calls go to the job) or the engine's
                 else { g_nrd_ubo_fill(r); g_nrd_ubo_bind(r); } }                       // the engine's own uniform-block code
    const uint64_t t1 = timed ? nr_now() : 0;
    if (g_nrd_lean == 1) nrd_ident(r, op == 36 ? 0 : 1);                                 // k_nk_site[0] / [1]: the two flush calls of the draw functions
    else nk_draw(r, g_nrd_base + (op == 36 ? NRNC_LR_DRAW : NRNC_LR_DRAWI));            // what the flush hook does first (NR-02 / NR-09)
    NRN_PRE(R);   // NR-NATIVE step B part 2: the card of this draw, taken before its flush - a published card is written short
    const uint64_t t2 = timed ? nr_now() : 0;
    nrnc_plan p; nrd_job_t j = {R, NULL, 0, &d, timed, ub};
    const uint64_t fcb = nrnc_cb(R);
    if (fcb && nrnc_flush_plan(R, &p)) { j.p = &p; j.cb = fcb; t_nrd.c[NRDC_BINDS] += (uint64_t)p.n; }
    else { g_o_tf(r, NC_Z); t_nrd.c[NRDC_FLUSH_ENGINE]++; }                              // the engine's flush (the original, past the hook)
    if (R[NRNC_R_REN_OFF] && R[NRNC_R_REN_NEED]) t_nrd.c[NRDC_REN]++;
    NRND_TJ0(timed);   // [DOS2] NR-NATIVE step D DM_NRN_REC=2: the stack switch and the parts of the call
    nvn_on_bigstack(nrd_job, &j);
    NRND_TJ5();   // [DOS2] NR-NATIVE step D
    NRN_POST();   // NR-NATIVE step B part 2: the record of the draw took the short flag (red line if it did not)
    if (g_nrd_ubd) nvn_nrub_scope(0);   // NR-UBD
    t_nrd.c[op == 37 ? NRDC_D37 : NRDC_D36]++;
    if (timed) { const uint64_t t3 = nr_now(); t_nrd.c[NRDC_TIMED]++; t_nrd.c[NRDC_NS] += t3 - t0; t_nrd.c[NRDC_NS_UBO] += t1 - t0; t_nrd.c[NRDC_NS_ID] += t2 - t1; t_nrd.c[NRDC_NS_REC] += t3 - t2; }
    return 1;
}
static void nrd_draw_after(unsigned op, uint64_t r) { (void)op; (void)r; }
static const dn_draw_native k_nrd_draw = {nrd_draw_take, nrd_draw_after};

// ---- the 10 s lines (from nk_report) ----
static void nrd_report(uint64_t frames) {
    uint64_t c[NRDC_N], n[NVN_NRD_S_N], ic[5] = {0, 0, 0, 0, 0};
    static uint64_t iw[5], id_cmp, id_bad;
    for (int i = 0; i < NRDC_N; i++) c[i] = __atomic_exchange_n(&g_nrdc[i], 0, __ATOMIC_RELAXED);
    nvn_nrd_stats(n);
    pthread_mutex_lock(&g_nk_mu);
    for (int i = 0; i < g_nrd_nidc; i++) { const nrd_idc *x = g_nrd_idc[i]; ic[0] += x->draws; ic[1] += x->same; ic[2] += x->hit; ic[3] += x->miss; ic[4] += x->rt; }
    pthread_mutex_unlock(&g_nk_mu);
    uint64_t id[5]; for (int i = 0; i < 5; i++) { id[i] = ic[i] - iw[i]; iw[i] = ic[i]; }
    id_cmp += c[NRDC_ID_CMP]; id_bad += c[NRDC_ID_BAD];
    const double f = frames ? (double)frames : 1., draws = (double)(c[NRDC_D36] + c[NRDC_D37]), idd = id[0] ? (double)id[0] : 1.;
    const double tap = n[NVN_NRD_TAP_DRAWS] ? (double)n[NVN_NRD_TAP_DRAWS] : 1., tm = c[NRDC_TIMED] ? (double)c[NRDC_TIMED] : 1., tn = n[NVN_NRD_TIMED] ? (double)n[NVN_NRD_TIMED] : 1.;
    if (g_nrd == 1) {
        LOG("[nr] direct (DM_NR_DIRECT=1, parts %#x, %llu frames): engine draws recorded directly %.1f a frame (36: %.1f, 37: %.1f; SetRenderEnable %.2f) with %.1f texture binds "
            "of their flush, %.1f of them not sent (the backend's table holds the handle; %.1f other binds of this thread noted); NVN calls of the engine not made %.0f a frame "
            "(DrawElements / DrawElementsInstanced %.0f, GetTextureHandle + BindTexture %.0f, SetRenderEnable %.2f) and %.0f flush-hook entries; left to the engine's function a "
            "frame: no index buffer record %.2f, no draw bracket / a check mode %.2f, NVN pointers not verified %.2f; the flush of a direct draw run by the engine %.2f; draws "
            "written as plain records %llu, binding tables forgotten %llu",
            g_nrd_parts, (unsigned long long)frames, draws / f, (double)c[NRDC_D36] / f, (double)c[NRDC_D37] / f, (double)c[NRDC_REN] / f, (double)c[NRDC_BINDS] / f,
            (double)n[NVN_NRD_FILT] / f, (double)(n[NVN_NRD_BIND_SEEN] > n[NVN_NRD_BINDS] ? n[NVN_NRD_BIND_SEEN] - n[NVN_NRD_BINDS] : 0) / f,
            (draws + 2. * (double)c[NRDC_BINDS] + (double)c[NRDC_REN]) / f, draws / f, 2. * (double)c[NRDC_BINDS] / f, (double)c[NRDC_REN] / f, draws / f,
            (double)c[NRDC_NOREC] / f, (double)c[NRDC_NOCAN] / f, (double)c[NRDC_NOTLIVE] / f, (double)c[NRDC_FLUSH_ENGINE] / f,
            (unsigned long long)n[NVN_NRD_PLAIN], (unsigned long long)n[NVN_NRD_FORGET]);
        LOG("[nr] direct, the tap and the identity (DM_NR_DIRECT=1): %.1f draws a frame reached the tap's walk, %.1f skipped (%.1f%%: the open pass already held their reads), "
            "%.1f walked and remembered, %.1f walked with a program whose masks / fs key are not published; identity (%s): %.1f engine draws a frame, state of the previous draw "
            "%.1f%%, thread table %.1f%%, shared table (lock) %.1f a frame, RT format set looked up %.1f a frame",
            (double)n[NVN_NRD_TAP_DRAWS] / f, (double)n[NVN_NRD_TAP_HIT] / f, 100. * (double)n[NVN_NRD_TAP_HIT] / tap, (double)n[NVN_NRD_TAP_LEARN] / f,
            (double)n[NVN_NRD_TAP_UNKNOWN] / f, g_nrd_lean == 1 ? "lean" : "nk_ident, as before", (double)id[0] / f, 100. * (double)id[1] / idd, 100. * (double)id[2] / idd,
            (double)id[3] / f, (double)id[4] / f);
        LOG("[nr] direct, ns a direct draw (1 in 64 timed, %llu): %.0f = the engine's uniform blocks %.0f + identity %.0f + flush and record %.0f (inside it: the tap %.0f, the "
            "state block + draw record %.0f (NR-CPU: state block %.0f, draw record %.0f), the flush's binds + stack switch %.0f)", (unsigned long long)c[NRDC_TIMED], (double)c[NRDC_NS] / tm, (double)c[NRDC_NS_UBO] / tm, (double)c[NRDC_NS_ID] / tm,
            (double)c[NRDC_NS_REC] / tm, (double)n[NVN_NRD_NS_TAP] / tn, (double)n[NVN_NRD_NS_REC] / tn, (double)n[NVN_NRD_NS_STATE] / tn,
            (double)(n[NVN_NRD_NS_REC] - n[NVN_NRD_NS_STATE]) / tn, (double)c[NRDC_NS_REC] / tm - (double)(n[NVN_NRD_NS_TAP] + n[NVN_NRD_NS_REC]) / tn);
    } else {
        LOG("[nr] direct check (DM_NR_DIRECT=2, parts %#x, %llu frames; the old path runs): texture binds of this thread noted %.1f a frame, %.1f of them in engine draws =1 "
            "would not send (flagged for the backend %.1f, past bind %d of their record %.1f; binding tables forgotten %llu); tap: %.1f draws a frame reached the walk, =1 would "
            "skip %.1f (%.1f%%), a skipped walk changed the pass: bad %llu; identity (totals): %llu compared, bad %llu; lean path: state of the previous draw %.1f%%, thread table "
            "%.1f%%, shared table (lock) %.1f a frame; the engine's own NVN calls against the model: %s",
            g_nrd_parts, (unsigned long long)frames, (double)n[NVN_NRD_BIND_SEEN] / f, (double)n[NVN_NRD_WOULD] / f, (double)n[NVN_NRD_WOULD_FLAG] / f, NRD_X_BITS,
            (double)n[NVN_NRD_WOULD_PAST] / f, (unsigned long long)n[NVN_NRD_FORGET], (double)n[NVN_NRD_TAP_DRAWS] / f, (double)n[NVN_NRD_TAP_HIT] / f,
            100. * (double)n[NVN_NRD_TAP_HIT] / tap, (unsigned long long)n[NVN_NRD_TAP_BAD], (unsigned long long)id_cmp, (unsigned long long)id_bad, 100. * (double)id[1] / idd,
            100. * (double)id[2] / idd, (double)id[3] / f, g_nr_nocall == 2 ? "the '[nr] nocall check' line (NR-23's compare, armed by this switch)" : "NOT compared (DM_NR_NOCALL is not 2)");
    }
    if (g_nrd_ubd == 1)
        LOG("[nr] ubd (DM_NR_UBD=1, NR-UBD, %llu frames): BindUniformBuffer calls of served direct draws %.1f a frame, not sent %.1f (%.1f%%: the backend's table holds that "
            "range for the recording); uniform binds of this thread into the tracked recording %.1f a frame; tables adopted %.2f, forgotten %llu (another thread / nested handles)",
            (unsigned long long)frames, (double)n[NVN_NRD_UB_SCOPE] / f, (double)n[NVN_NRD_UB_SKIP] / f,
            n[NVN_NRD_UB_SCOPE] ? 100. * (double)n[NVN_NRD_UB_SKIP] / (double)n[NVN_NRD_UB_SCOPE] : 0., (double)n[NVN_NRD_UB_SEEN] / f, (double)n[NVN_NRD_UB_ADOPT] / f,
            (unsigned long long)n[NVN_NRD_UB_FORGET]);
    else if (g_nrd_ubd == 2)
        LOG("[nr] ubd check (DM_NR_UBD=2, NR-UBD, %llu frames; every bind is sent): BindUniformBuffer calls of served direct draws %.1f a frame, =1 would not send %.1f (%.1f%%), "
            "flagged for the backend %.1f, past bind %d of their record %.1f; uniform binds of this thread into the tracked recording %.1f a frame; tables adopted %.2f, forgotten "
            "%llu; the backend's verdict: the '[nr] ubd check (backend' line",
            (unsigned long long)frames, (double)n[NVN_NRD_UB_SCOPE] / f, (double)n[NVN_NRD_UB_WOULD] / f,
            n[NVN_NRD_UB_SCOPE] ? 100. * (double)n[NVN_NRD_UB_WOULD] / (double)n[NVN_NRD_UB_SCOPE] : 0., (double)n[NVN_NRD_UB_FLAG] / f, NRD_X_BITS,
            (double)n[NVN_NRD_UB_PAST] / f, (double)n[NVN_NRD_UB_SEEN] / f, (double)n[NVN_NRD_UB_ADOPT] / f, (unsigned long long)n[NVN_NRD_UB_FORGET]);
    if (g_nru) nru_report(frames);   // NR-09U
}

// NR-UBD (DM_NR_UBD): after nrd_install / nru_install. Needs the served direct draws of NR-09R (DM_NR_DIRECT=1 with its draw part, which
// needs DM_NR_DRAW: the OP_X_NR_DRAW tail carries the =2 flags) and the backend executing a recording in stream order.
static void nrub_install(int direct, int parts) {
    const char *e = getenv("DM_NR_UBD");
    const int v = e ? atoi(e) : 0;
    if (!v) return;
    if (v != 1 && v != 2) { LOG("【红】[nr] DM_NR_UBD=%s: only 0 (off), 1 (a repeated uniform-buffer range of a direct draw is not sent) and 2 (sent, flagged and compared) exist - off", e); return; }
    if (direct != 1 || !(parts & NRDP_DRAW)) {
        LOG("【红】[nr] DM_NR_UBD=%d: needs DM_NR_DIRECT=1 with its draw part (the served direct draws whose binds it leaves out; it is %d, parts %#x) - off", v, direct, parts);
        return;
    }
    static const char *const ord[3] = {"DM_NB_ORDER", "DM_NB_RELOC", "DM_NB_MX"};
    for (int i = 0; i < 3; i++) { const char *o = getenv(ord[i]); if (o && atoi(o)) {
        LOG("[nr] DM_NR_UBD=%d: %s=%s lets the backend execute parts of a recording in another order / from key frames - every uniform bind is sent (off)", v, ord[i], o); return; } }
    nvn_nrub_enable(v);
    g_nrd_ubd = v;   // last: nrd_draw_take reads it
    LOG("[nr] DM_NR_UBD=%d (NR-UBD): %s; the mirror resets at BeginRecording, forgets at CopyCommands / CallCommands and at another thread's bind%s", v,
        v == 1 ? "a BindUniformBuffer of a served direct draw whose stage / slot already holds that address and size in the backend's table for this recording is not sent"
               : "every BindUniformBuffer is sent; the ones =1 would not send are flagged in the draw's record and compared with the backend's table as they land",
        g_nru == 1 && v == 2 ? "; NR-09U: rebuilds of a block dirty in the same frame whose shadow equals its previous ring bytes are counted" : "");
}

// ---- install (dm_dos2_nr_install; guest code is not running yet) ----
// Before nc_arm: DM_NR_DIRECT is read; =2 arms NR-23's compare (the model against the engine's calls) unless DM_NR_NOCALL says otherwise.
static void nrd_arm(module_t *m) {
    const char *e = getenv("DM_NR_DIRECT");
    const int v = e ? atoi(e) : 0;
    if (!v) return;
    if (v != 1 && v != 2) { LOG("【红】[nr] DM_NR_DIRECT=%s: only 0 (off), 1 (engine draws recorded directly) and 2 (the old path + what 1 would do, compared) exist - off", e); return; }
    const char *n = getenv("DM_NR_NOCALL");
    if (v == 1 && n && atoi(n)) { LOG("【红】[nr] DM_NR_DIRECT=1 with DM_NR_NOCALL=%s: both stand in for the engine's draw functions - DM_NR_DIRECT off", n); return; }
    int parts = NRDP_ALL;
    if ((e = getenv("DM_NR_DIRECT_PARTS")) && *e) parts = (int)strtol(e, NULL, 0) & NRDP_ALL;
    if (parts & NRDP_DRAW) {
        static const nrnc_code code[NRNC_CODE_N] = NRNC_CODE_TABLE;
        for (int i = 0; i < NRNC_CODE_N; i++) {
            const uint64_t h = m->text_end >= code[i].end ? nrnc_fnv(m->base + code[i].off + 16, code[i].end - code[i].off - 16) : 0;
            if (h == code[i].fnv) continue;
            LOG("【红】[nr] DM_NR_DIRECT=%d: main+%#x..%#x (%s) is not the analysed code (hash %016llx, expected %016llx): not DOS2 update v786432 main (build 4979b200) - the "
                "draw part stays off", v, code[i].off, code[i].end, code[i].name, (unsigned long long)h, (unsigned long long)code[i].fnv);
            parts &= ~NRDP_DRAW; break;
        }
    }
    g_nrd_base = (uint64_t)(uintptr_t)m->base;
    g_nrd_isz = (const uint8_t *)m->base + NRNC_ISZ_TABLE;
    g_nrd_ubo_fill = (nc_guest_fn)(uintptr_t)(m->base + NRNC_FN_UBO_FILL); g_nrd_ubo_bind = (nc_guest_fn)(uintptr_t)(m->base + NRNC_FN_UBO_BIND);
    g_nrd_parts = parts; g_nrd_want = v;
    if (v == 2 && (parts & NRDP_DRAW)) setenv("DM_NR_NOCALL", "2", 0);   // nc_arm (next): NR-23's compare, unless DM_NR_NOCALL is set (install time, as jni_android.c sets its defaults)
}
// After the flush hook, DM_NR_DRAW and NR-23 are in.
static void nrd_install(void) {
    const int v = g_nrd_want;
    if (!v) return;
    if (!g_nr_draw) { LOG("【红】[nr] DM_NR_DIRECT=%d: DM_NR_DRAW is not armed (it brings the identity and the OP_X_NR_DRAW record the direct path writes) - off", v); return; }
    int parts = g_nrd_parts;
    if (v == 1 && (parts & NRDP_DRAW) && dn_install_draw_native(&k_nrd_draw)) { parts &= ~NRDP_DRAW; LOG("【红】[nr] DM_NR_DIRECT=1: no native handler on the engine's draw functions (see above) - the draw part stays off"); }
    if (parts & NRDP_TEXF) {   // the mirror stands on "the backend executes a recording's records in stream order from its own tables"
        static const char *const ord[3] = {"DM_NB_ORDER", "DM_NB_RELOC", "DM_NB_MX"};
        for (int i = 0; i < 3; i++) { const char *o = getenv(ord[i]); if (o && atoi(o)) { parts &= ~NRDP_TEXF;
            LOG("[nr] DM_NR_DIRECT=%d: %s=%s lets the backend execute parts of a recording in another order / from key frames - every texture bind is sent (part %d off)", v, ord[i], o, NRDP_TEXF); break; } }
    }
    g_nrd_lean = (parts & NRDP_IDENT) && g_nr_key == 3 && g_nr_draw == 1 ? v : 0;
    nvn_nrd_enable(v, parts);
    g_nrd_parts = parts;
    g_nrd = v;   // last: the hooks read it
    LOG("[nr] DM_NR_DIRECT=%d (NR-09R, parts %#x): %s; draw functions main+%#x / +%#x %s; texture binds whose slot holds the handle %s; the tap's walk at a repeated read set %s; "
        "identity %s; uniform blocks (main+%#x / +%#x) and every state packet stay the engine's NVN calls",
        v, parts, v == 1 ? "engine draws are recorded directly (one call on the big stack instead of the engine's GetTextureHandle / BindTexture / DrawElements calls)"
                         : "the old path runs, what =1 would do is computed and compared",
        NRNC_FN_DRAW, NRNC_FN_DRAWI, !(parts & NRDP_DRAW) ? "left to the engine (part off)" : v == 1 ? "taken over after their index buffer lookup" : g_nr_nocall == 2 ? "compared with the model (NR-23)" : "NOT compared (DM_NR_NOCALL is not 2)",
        !(parts & NRDP_TEXF) ? "sent (part off)" : v == 1 ? "not sent" : "flagged for the backend's check",
        !(parts & NRDP_TAP) ? "runs (part off)" : v == 1 ? "skipped" : "checked to change nothing",
        g_nrd_lean ? (v == 1 ? "lean (no lock, no atomic, no texture-set id)" : "lean path compared with nk_ident") : (parts & NRDP_IDENT) ? "as before (DM_NR_KEY's census or DM_NR_DRAW=2 need the old path)" : "as before (part off)",
        NRNC_FN_UBO_FILL, NRNC_FN_UBO_BIND);
}
