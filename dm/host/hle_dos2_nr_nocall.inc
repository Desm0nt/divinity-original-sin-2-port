// [DOS2] NR-23: the engine's own NVN calls at a draw (DM_NR_NOCALL, default 0; docs/dos2/nr/NR-23.md). Included by hle_dos2_nr.c after
// hle_dos2_nr_key.inc (uses nk_draw, nk_site, g_o_tf, NR_HOOK / NR_A / NR_F, NR_INC / NR_GET). main = main.raw of update v786432.
//   DM_NR_NOCALL=0 / unset  nothing: the flush hook of NR-02 / NR-09 is the old function, the draw hook has no native handler.
//   DM_NR_NOCALL=2          count + compare, the engine's code runs. Per frame: the NVN calls of the threads that run engine packets by
//                           packet opcode and by part of the draw (before / in / after the texture flush); what =1 would no longer call
//                           through the thunks; the binds whose slot already held that handle; a cost model from a calibration on the
//                           recording thread (an NVN call through thunk + big stack, the body called directly, one stack switch).
//                           Compare: before the engine's texture flush runs, the model (nr_nocall.h) says which binds it will make;
//                           after the flush of a draw 36 / 37 it says the SetRenderEnable and the draw call; the calls that then reach the
//                           NVN entry points are compared with it (red lines, first 16), and so are the fields the code leaves behind.
//   DM_NR_NOCALL=1          the modelled part runs natively: the texture flush 0x924730 at every known call site (25) and the tail of
//                           the draw functions 0x928e80 / 0x928fb0 (flush + SetRenderEnable + the draw call + the flag byte) call the
//                           bodies of the NVN entry points directly on the big stack - one stack switch a draw instead of one
//                           thunk + wrapper round trip a call. The uniform-block functions 0x926f90 / 0x925740 stay the engine's (their
//                           BindUniformBuffer calls come through the thunks as before), and so does every other packet.
//                           DM_NR_NOCALL_PARTS: bit 0 the flush, bit 1 the draw tail (default 3).
//   There is no =3: the native path calls the same bodies with the same arguments, nothing is left stale that a poison could show.
// Needs DM_NR=1|2 and the flush hook (DM_NR_DRAW=1|2 or DM_NR_KEY=1|2).
// Proof, per run and per draw (before anything is skipped):
//   install   the three functions the model stands for hash to the analysed code (nrnc_code: every word after the hooked prologue);
//   first use the engine's five NVN pointers hold the thunks of the entry points whose bodies the direct entries call (nc_verify);
//   per draw  nrnc_draw_pre / nrnc_flush_plan: an index buffer record, a command buffer, dirty ranges inside the 32 slots - else the
//             engine's function runs, untouched. Nothing was written and no call made before that decision.
// The model itself is proven against the game's machine code in an emulator (tests/nr_nocall_oracle.py) and on the phone by =2.
#include "../gfx/nr/nr_nocall.h"
#include "../gfx/nvn/nvn_nr_nocall.h"

static int g_nr_nocall;        // DM_NR_NOCALL as armed (set once at install; 0 = off)
static int g_nc_want, g_nc_parts = 3;
static int g_nc_live;          // atomic: 0 = the engine's NVN pointers not looked at yet, 1 = verified, -1 = not ours (never native)
static uint64_t g_nc_base;     // main's load address
static const uint8_t *g_nc_isz;
typedef uint64_t (*nc_guest_fn)(uint64_t);
static nc_guest_fn g_nc_ubo_fill, g_nc_ubo_bind;
static double g_nc_cal[3]; static int g_nc_cal_done;   // atomic flag; the three numbers are written once before it is set

enum { NC_OPS = 65, NC_OUT = 64 };   // packet opcode 0..63, 64 = outside every packet
static struct {   // atomics
    // both modes
    uint64_t flushes, flush_binds, flush_in_draw, flush_with_binds_out, flush_noplan, flush_nocb, flush_site, flush_off;
    uint64_t draws[2], draw_norec, draw_flush_engine, draw_ren, not_live;
    // DM_NR_NOCALL=2
    uint64_t calls[NC_OPS][3];
    uint64_t same, f_cmp, f_bad, f_bad_n, f_bad_bind, f_bad_range, f_bad_calls, d_cmp, d_bad, d_bad_draw, d_bad_ren, d_bad_flag, bad_lines;
} g_nc;
#define NC_ADD(x, n) __atomic_fetch_add(&(x), (uint64_t)(n), __ATOMIC_RELAXED)
static __thread struct {
    int seen, depth, op[8], phase[8]; uint64_t last;                    // the class the thread's NVN calls go to (DM_NR_NOCALL=2)
    int armed, have, exp; nrnc_draw d; nrnc_expect e;                   // the draw 36 / 37 being compared
    uint64_t h_cb, h_frame; uint64_t h[NRNC_STAGES][NRNC_SLOTS]; uint32_t h_set[NRNC_STAGES];   // the last handle a flush bound per slot
} t_nc;

// ---- the NVN calls of this thread by class (DM_NR_NOCALL=2) ----
static void nc_cls_flush(void) {
    const uint64_t now = nvn_nc_thread_calls();
    if (!t_nc.seen) { t_nc.seen = 1; t_nc.last = now; return; }
    const int d = t_nc.depth > 8 ? 8 : t_nc.depth;
    const int op = d ? t_nc.op[d - 1] : NC_OUT, ph = d ? t_nc.phase[d - 1] : t_nc.phase[0];
    if (now != t_nc.last) NC_ADD(g_nc.calls[op][ph], now - t_nc.last);
    t_nc.last = now;
}
static void nc_rc_pre(uint64_t list, unsigned op, uint64_t pkt) {
    (void)list; (void)pkt;
    nc_cls_flush();
    if (t_nc.depth < 8) { t_nc.op[t_nc.depth] = op < 64 ? (int)op : NC_OUT; t_nc.phase[t_nc.depth] = 0; }
    t_nc.depth++;
}
static void nc_rc_post(uint64_t list, unsigned op, uint64_t pkt, uint64_t len) {
    (void)list; (void)op; (void)pkt; (void)len;
    nc_cls_flush();
    if (t_nc.depth > 0) t_nc.depth--;
    if (!t_nc.depth) t_nc.phase[0] = 0;
}
static const dn_rc_handler k_nc_rc = {nc_rc_pre, nc_rc_post};
static void nc_phase(int ph) {   // the flush begins (1) / ended (2) inside the open packet (or outside every packet)
    nc_cls_flush();
    const int d = t_nc.depth > 8 ? 8 : t_nc.depth;
    t_nc.phase[d ? d - 1 : 0] = ph;
}

static void nc_red(const char *what, const char *detail) {
    if (__atomic_fetch_add(&g_nc.bad_lines, 1, __ATOMIC_RELAXED) < 16) LOG("【红】[nr] nocall check: %s - %s", what, detail);
}
// The engine's NVN pointers (filled by its loader before the first frame) against the thunks of the bodies the direct entries call.
static int nc_verify(void) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    int v = __atomic_load_n(&g_nc_live, __ATOMIC_RELAXED);
    if (!v) {
        static const uint32_t slot[NVN_NC_N] = {NRNC_SLOT_HANDLE, NRNC_SLOT_BIND, NRNC_SLOT_DRAW, NRNC_SLOT_DRAWI, NRNC_SLOT_REN};
        const void *th[NVN_NC_N];
        int ok = nvn_nc_ready(th), same = 1;
        uint64_t got[NVN_NC_N];
        for (int k = 0; k < NVN_NC_N; k++) { got[k] = nrnc_r64((const uint8_t *)(uintptr_t)(g_nc_base + slot[k])); if (!th[k] || got[k] != (uint64_t)(uintptr_t)th[k]) same = 0; }
        v = ok && same ? 1 : -1;
        if (v > 0) LOG("[nr] nocall: the engine's GetTextureHandle / BindTexture / DrawElements / DrawElementsInstanced / SetRenderEnable pointers are the thunks "
                       "of the bodies the direct entries call - the native paths are %s", g_nr_nocall == 1 ? "live" : "what =1 would run");
        else LOG("【红】[nr] nocall: %s (GetTextureHandle %#llx / %p, BindTexture %#llx / %p, DrawElements %#llx / %p, DrawElementsInstanced %#llx / %p, "
                 "SetRenderEnable %#llx / %p): the engine's code keeps running, nothing is skipped",
                 !ok ? "an NVN entry point is not implemented by the body the direct entry calls" : "an engine NVN pointer is not our thunk",
                 (unsigned long long)got[0], th[0], (unsigned long long)got[1], th[1], (unsigned long long)got[2], th[2], (unsigned long long)got[3], th[3],
                 (unsigned long long)got[4], th[4]);
        __atomic_store_n(&g_nc_live, v, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&mu);
    return v > 0;
}
static inline int nc_live(void) {
    const int v = __atomic_load_n(&g_nc_live, __ATOMIC_RELAXED);
    if (__builtin_expect(v != 0, 1)) return v > 0;
    return nc_verify();
}
static void nc_calibrate(void) {   // once, on the first thread that flushes after the pointers were verified (the recording thread)
    static int taken;
    if (__atomic_exchange_n(&taken, 1, __ATOMIC_RELAXED)) return;
    double c[3];
    if (!nvn_nc_calibrate(c)) return;
    g_nc_cal[0] = c[0]; g_nc_cal[1] = c[1]; g_nc_cal[2] = c[2];
    __atomic_store_n(&g_nc_cal_done, 1, __ATOMIC_RELEASE);
}

// ---- the NVN calls of the model = the bodies of the entry points ----
static void nc_op_bind(void *u, uint64_t cb, int stage, int slot, int32_t tex, int32_t smp) { (void)u; nvn_nc_bind(cb, stage, slot, tex, smp); }
static void nc_op_ren(void *u, uint64_t cb) { (void)u; nvn_nc_ren(cb); }
static void nc_op_draw(void *u, uint64_t cb, int32_t prim, uint32_t itype, int32_t count, uint64_t ib, int32_t instances) { (void)u; nvn_nc_draw(cb, prim, itype, count, ib, instances); }
static const nrnc_ops k_nc_ops = {nc_op_bind, nc_op_ren, nc_op_draw};
typedef struct { uint8_t *R; const nrnc_plan *p; uint64_t cb; const nrnc_draw *d; } nc_job;
static void *nc_job_flush(void *a) { const nc_job *j = (const nc_job *)a; nrnc_flush_run(j->R, j->p, j->cb, &k_nc_ops, NULL); return NULL; }
static void *nc_job_draw(void *a) {   // the tail of the draw function on the big stack: [flush], SetRenderEnable, the draw call, the flag byte
    const nc_job *j = (const nc_job *)a;
    if (j->p) nrnc_flush_run(j->R, j->p, j->cb, &k_nc_ops, NULL);
    nrnc_draw_tail(j->R, j->d, g_nc_isz, &k_nc_ops, NULL);
    return NULL;
}
#define NC_Z 0, 0, 0, 0, 0, 0, 0, 0., 0., 0., 0., 0., 0., 0., 0., 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0   // the flush takes x0 only

// ---- DM_NR_NOCALL=1: the texture flush ----
// 1 = done natively (the engine's function must not run); 0 = nothing was touched, the engine's function runs.
static int nc_flush_take(uint64_t r, uint64_t lr) {
    if (!(g_nc_parts & 1)) { NR_INC(g_nc.flush_off); return 0; }
    if (!nc_live()) { NR_INC(g_nc.not_live); return 0; }
    if (!__atomic_load_n(&g_nc_cal_done, __ATOMIC_RELAXED)) nc_calibrate();
    if (nk_site(lr) < 0) { NR_INC(g_nc.flush_site); return 0; }
    uint8_t *R = (uint8_t *)(uintptr_t)r;
    const uint64_t cb = nrnc_cb(R);
    if (!cb) { NR_INC(g_nc.flush_nocb); return 0; }
    nrnc_plan p;
    if (!nrnc_flush_plan(R, &p)) { NR_INC(g_nc.flush_noplan); return 0; }
    if (p.n) { nc_job j = {R, &p, cb, NULL}; nvn_on_bigstack(nc_job_flush, &j); NR_INC(g_nc.flush_with_binds_out); NC_ADD(g_nc.flush_binds, p.n); }
    else for (int s = 0; s < NRNC_STAGES; s++) nrnc_flush_reset(R, s);
    NR_INC(g_nc.flushes);
    return 1;
}
// ---- DM_NR_NOCALL=2: the texture flush, before and after the engine's function ----
typedef struct { int ok; nrnc_plan p; uint64_t cb, calls0; } nc2_flush;
static void nc2_flush_pre(uint64_t r, nc2_flush *f) {
    const uint8_t *R = (const uint8_t *)(uintptr_t)r;
    if (nc_live() && !__atomic_load_n(&g_nc_cal_done, __ATOMIC_RELAXED)) nc_calibrate();
    f->cb = nrnc_cb(R);
    f->ok = f->cb && nrnc_flush_plan(R, &f->p);
    nc_phase(1);
    f->calls0 = nvn_nc_thread_calls();
    nvn_nc_cap_begin(1);
}
static void nc2_flush_post(uint64_t r, uint64_t lr, const nc2_flush *f) {
    const nvn_nc_cap *c = nvn_nc_cap_end(1);
    const uint64_t calls = nvn_nc_thread_calls() - f->calls0;
    nc_phase(2);
    const uint8_t *R = (const uint8_t *)(uintptr_t)r;
    const uint64_t site = lr - g_nc_base;
    const int in_draw = site == NRNC_LR_DRAW || site == NRNC_LR_DRAWI;
    NR_INC(g_nc.flushes); NC_ADD(g_nc.flush_binds, c->nbind);
    if (in_draw) NR_INC(g_nc.flush_in_draw); else if (c->nbind) NR_INC(g_nc.flush_with_binds_out);
    if (nk_site(lr) < 0) NR_INC(g_nc.flush_site);
    if (!f->cb) NR_INC(g_nc.flush_nocb); else if (!f->ok) NR_INC(g_nc.flush_noplan);
    // binds whose slot already held that handle (this thread, this command buffer, this frame): =1 does not skip them
    const uint64_t frame = dm_nvn_frame_count();
    if (t_nc.h_cb != f->cb || t_nc.h_frame != frame) { t_nc.h_cb = f->cb; t_nc.h_frame = frame; memset(t_nc.h_set, 0, sizeof t_nc.h_set); }
    for (int i = 0; i < c->nbind && i < NVN_NC_CAP_BINDS; i++) {
        const int s = c->b[i].stage == 0 ? 0 : c->b[i].stage == 1 ? 1 : c->b[i].stage == 5 ? 2 : -1, sl = c->b[i].slot;
        if (s < 0 || sl < 0 || sl >= NRNC_SLOTS) continue;
        if ((t_nc.h_set[s] >> sl & 1) && t_nc.h[s][sl] == c->b[i].handle) NR_INC(g_nc.same);
        t_nc.h[s][sl] = c->b[i].handle; t_nc.h_set[s] |= 1u << sl;
    }
    if (f->ok) {
        NR_INC(g_nc.f_cmp);
        int bad = 0; char d[256]; d[0] = 0;
        if (c->nbind != f->p.n) { bad = 1; NR_INC(g_nc.f_bad_n); snprintf(d, sizeof d, "the engine made %d BindTexture calls, the model %d", c->nbind, f->p.n); }
        else for (int i = 0; i < c->nbind && !bad; i++) {
            const nrnc_bind *b = &f->p.b[i];
            if (c->b[i].cb != f->cb || c->b[i].stage != b->stage || c->b[i].slot != b->slot || c->b[i].handle != nvn_nc_handle(b->tex, b->smp)) {
                bad = 1; NR_INC(g_nc.f_bad_bind);
                snprintf(d, sizeof d, "bind %d of %d: the engine (cb %#llx, stage %d, slot %d, handle %#llx), the model (cb %#llx, stage %d, slot %d, texture %d sampler %d)",
                         i, c->nbind, (unsigned long long)c->b[i].cb, c->b[i].stage, c->b[i].slot, (unsigned long long)c->b[i].handle, (unsigned long long)f->cb, b->stage, b->slot, b->tex, b->smp);
            }
        }
        if (!bad && !nrnc_flush_clean(R)) { bad = 1; NR_INC(g_nc.f_bad_range); snprintf(d, sizeof d, "a dirty range is not reset after the flush"); }
        if (!bad && nvn_nc_thread_calls() && calls != 2ull * (uint64_t)c->nbind) {   // (the counter is dead with DM_NVN_BIGSTACK=0)
            bad = 1; NR_INC(g_nc.f_bad_calls);
            snprintf(d, sizeof d, "%llu NVN calls inside the flush for %d binds (2 a bind expected: GetTextureHandle + BindTexture)", (unsigned long long)calls, c->nbind);
        }
        if (bad) { NR_INC(g_nc.f_bad); char w[64]; snprintf(w, sizeof w, "texture flush at main+%#llx", (unsigned long long)(site - 4)); nc_red(w, d); }
    }
    // the tail of a draw 36 / 37: what the engine's code is about to call
    if (in_draw && t_nc.armed && t_nc.have) { nrnc_draw_expect(R, &t_nc.d, g_nc_isz, &t_nc.e); t_nc.exp = 1; }
}
NR_HOOK nr_h_tf_nc(NR_A) {   // the flush hook while DM_NR_NOCALL is armed: nr_h_tf + the native / compared flush
    const uint64_t lr = (uint64_t)(uintptr_t)__builtin_return_address(0);   // the hook is jumped to: x30 is still the game's return address
    nk_draw(a0, lr);
    if (g_nr_nocall == 1) { if (nc_flush_take(a0, lr)) return 0; }
    else if (g_nr_nocall == 2) {
        nc2_flush f; nc2_flush_pre(a0, &f);
        const uint64_t v = g_o_tf(NR_F);
        nc2_flush_post(a0, lr, &f);
        return v;
    }
    return g_o_tf(NR_F);
}
static void *nc_tf_hook(void *dflt) { return g_nc_want ? (void *)nr_h_tf_nc : dflt; }

// ---- the draw functions (the shared draw hook of hle_dos2_native.c asks first) ----
// op 36: 0x928e80(r, first index, count); op 37: 0x928fb0(r, -, instances, -, count).
static int nc_draw_take(unsigned op, uint64_t r, uint64_t a1, uint64_t a2, uint64_t a4) {
    const int mode = g_nr_nocall;
    if (!mode) return 0;
    uint8_t *R = (uint8_t *)(uintptr_t)r;
    const int32_t first = op == 36 ? (int32_t)a1 : 0, count = op == 36 ? (int32_t)a2 : (int32_t)a4, inst = op == 36 ? -1 : (int32_t)a2;
    if (mode == 2) {
        t_nc.armed = 1; t_nc.exp = 0;
        t_nc.have = nrnc_draw_pre(R, (int)op, first, count, inst, &t_nc.d);
        NR_INC(g_nc.draws[op == 37]);
        if (!t_nc.have) NR_INC(g_nc.draw_norec);
        nvn_nc_cap_begin(2);
        return 0;
    }
    if (!(g_nc_parts & 2)) return 0;
    if (!nc_live()) { NR_INC(g_nc.not_live); return 0; }
    nrnc_draw d;
    if (!nrnc_draw_pre(R, (int)op, first, count, inst, &d)) { NR_INC(g_nc.draw_norec); return 0; }   // nothing touched: the engine's function runs
    if (d.ubo) { g_nc_ubo_fill(r); g_nc_ubo_bind(r); }                                              // the engine's own uniform-block code
    nk_draw(r, g_nc_base + (op == 36 ? NRNC_LR_DRAW : NRNC_LR_DRAWI));                              // what the flush hook does first (NR-02 / NR-09)
    nrnc_plan p; nc_job j = {R, NULL, 0, &d};
    const uint64_t fcb = (g_nc_parts & 1) ? nrnc_cb(R) : 0;
    if (fcb && nrnc_flush_plan(R, &p)) { j.p = &p; j.cb = fcb; NR_INC(g_nc.flushes); NR_INC(g_nc.flush_in_draw); NC_ADD(g_nc.flush_binds, p.n); }
    else { g_o_tf(r, NC_Z); NR_INC(g_nc.draw_flush_engine); }                                       // the engine's flush (the original, past the hook)
    if (R[NRNC_R_REN_OFF] && R[NRNC_R_REN_NEED]) NR_INC(g_nc.draw_ren);
    nvn_on_bigstack(nc_job_draw, &j);
    NR_INC(g_nc.draws[op == 37]);
    return 1;
}
static void nc_draw_after(unsigned op, uint64_t r) {   // DM_NR_NOCALL=2: the engine's draw function returned
    (void)op;
    if (g_nr_nocall != 2 || !t_nc.armed) return;
    const nvn_nc_cap *c = nvn_nc_cap_end(2);
    t_nc.armed = 0;
    if (!t_nc.have || !t_nc.exp) return;
    const uint8_t *R = (const uint8_t *)(uintptr_t)r;
    const nrnc_expect *e = &t_nc.e;
    NR_INC(g_nc.d_cmp);
    if (e->ren) NR_INC(g_nc.draw_ren);
    int bad = 0; char d[300]; d[0] = 0;
    if (c->ndraw != 1 || c->dcb != e->cb || c->prim != e->prim || (uint32_t)c->itype != e->itype || c->count != e->count || c->ib != e->ib || c->instances != e->instances) {
        bad = 1; NR_INC(g_nc.d_bad_draw);
        snprintf(d, sizeof d, "%d draw calls; the engine (cb %#llx, prim %d, type %d, count %d, indices %#llx, instances %d), the model (cb %#llx, prim %d, type %u, count %d, indices %#llx, instances %d)",
                 c->ndraw, (unsigned long long)c->dcb, c->prim, c->itype, c->count, (unsigned long long)c->ib, c->instances,
                 (unsigned long long)e->cb, e->prim, e->itype, e->count, (unsigned long long)e->ib, e->instances);
    } else if (c->nren != e->ren || (e->ren && (c->rcb != e->ren_cb || (c->ren_on & 0xff) != 1))) {
        bad = 1; NR_INC(g_nc.d_bad_ren);
        snprintf(d, sizeof d, "%d SetRenderEnable calls (cb %#llx, value %d), the model %d (cb %#llx, value 1)", c->nren, (unsigned long long)c->rcb, c->ren_on, e->ren, (unsigned long long)e->ren_cb);
    } else if (R[NRNC_R_REN_OFF] != R[NRNC_R_REN_NEED] || (e->ren && (R[NRNC_R_REN_NEED] || nrnc_r64(R + NRNC_R_REN_Q)))) {
        bad = 1; NR_INC(g_nc.d_bad_flag);
        snprintf(d, sizeof d, "render-enable bytes after the draw: need %d, off %d, +0x1860 %#llx (the model: off = need%s)", R[NRNC_R_REN_NEED], R[NRNC_R_REN_OFF],
                 (unsigned long long)nrnc_r64(R + NRNC_R_REN_Q), e->ren ? " = 0, +0x1860 = 0" : "");
    }
    if (bad) { NR_INC(g_nc.d_bad); nc_red(op == 36 ? "draw 36 (0x928e80)" : "draw 37 (0x928fb0)", d); }
}

// ---- the 10 s lines (from nk_report) ----
static void nc_report(uint64_t frames) {
    static uint64_t w[sizeof g_nc / 8];
    uint64_t c[sizeof g_nc / 8], dlt[sizeof g_nc / 8];
    for (size_t i = 0; i < sizeof g_nc / 8; i++) { c[i] = __atomic_load_n(&((uint64_t *)&g_nc)[i], __ATOMIC_RELAXED); dlt[i] = c[i] - w[i]; w[i] = c[i]; }
#define NC_D(field) (dlt[offsetof(__typeof__(g_nc), field) / 8])
#define NC_T(field) (c[offsetof(__typeof__(g_nc), field) / 8])
    const double f = frames ? (double)frames : 1.;
    const int cal = __atomic_load_n(&g_nc_cal_done, __ATOMIC_ACQUIRE);
    const double draws = (double)(NC_D(draws[0]) + NC_D(draws[1]));
    if (g_nr_nocall == 2) {
        const uint64_t *calls = &NC_D(calls[0][0]);
        double total = 0, drw[3] = {0, 0, 0}, other = 0, out = 0;
        uint64_t per_op[NC_OPS];
        for (int op = 0; op < NC_OPS; op++) {
            per_op[op] = calls[op * 3] + calls[op * 3 + 1] + calls[op * 3 + 2]; total += (double)per_op[op];
            if (op == 36 || op == 37) for (int k = 0; k < 3; k++) drw[k] += (double)calls[op * 3 + k];
            else if (op == NC_OUT) out = (double)per_op[op];
            else other += (double)per_op[op];
        }
        char top[400]; int n = 0; top[0] = 0;
        for (int k = 0; k < 8 && n < (int)sizeof top - 40; k++) {
            int best = -1;
            for (int op = 0; op < 64; op++) if (op != 36 && op != 37 && per_op[op] && (best < 0 || per_op[op] > per_op[best])) best = op;
            if (best < 0) break;
            n += snprintf(top + n, sizeof top - (size_t)n, "%sop %d %.0f", n ? ", " : "", best, (double)per_op[best] / f);
            per_op[best] = 0;
        }
        LOG("[nr] nocall (DM_NR_NOCALL=2, %llu frames; count + compare, the engine's code runs): NVN calls of the threads that run engine packets %.0f a frame: "
            "engine draws 36 / 37 %.0f (uniform blocks before the flush %.0f, the texture flush %.0f, after it %.0f); other packets %.0f (%s); outside the packets %.0f",
            (unsigned long long)frames, total / f, (drw[0] + drw[1] + drw[2]) / f, drw[0] / f, drw[1] / f, drw[2] / f, other / f, n ? top : "-", out / f);
        const double binds = (double)NC_D(flush_binds), elig = draws - (double)NC_D(draw_norec);
        const double skip = 2. * binds + elig + (double)NC_D(draw_ren), sw = elig + (double)NC_D(flush_with_binds_out);
        LOG("[nr] nocall, what =1 would not call through the thunks: %.1f texture flushes a frame (%.1f in draws 36 / 37) with %.1f binds = %.0f calls "
            "(GetTextureHandle + BindTexture), %.1f draw calls, %.2f SetRenderEnable: %.0f NVN calls a frame (%.1f%% of the %.0f above) for %.0f stack switches; "
            "left to the engine: flushes with a range outside the slots %.2f, with no command buffer %.2f, from an unknown call site %.2f, draws without an "
            "index buffer record %.2f a frame; binds whose slot already held that handle this frame %.1f a frame (%.1f%% of the binds; =1 still makes them)",
            (double)NC_D(flushes) / f, (double)NC_D(flush_in_draw) / f, binds / f, 2. * binds / f, elig / f, (double)NC_D(draw_ren) / f, skip / f,
            total > 0 ? 100. * skip / total : 0., total / f, sw / f, (double)NC_D(flush_noplan) / f, (double)NC_D(flush_nocb) / f, (double)NC_D(flush_site) / f,
            (double)NC_D(draw_norec) / f, (double)NC_D(same) / f, binds > 0 ? 100. * (double)NC_D(same) / binds : 0.);
        if (cal) LOG("[nr] nocall cost model (calibrated on the recording thread): an NVN call through thunk + big stack %.1f ns, its body called directly %.1f ns, one stack "
                     "switch %.1f ns: =1 would save about %.3f ms a frame (%.0f calls x %.1f ns - %.0f switches x %.1f ns); all %.0f calls a frame cost %.3f ms in the "
                     "thunk + wrapper alone (the bodies: DM_NVN_PROF=2, [cpuprof] nvn top)",
                     g_nc_cal[0], g_nc_cal[1], g_nc_cal[2], nrnc_saving_ns(skip / f, sw / f, g_nc_cal[0], g_nc_cal[1], g_nc_cal[2]) / 1e6, skip / f, g_nc_cal[0] - g_nc_cal[1],
                     sw / f, g_nc_cal[2], total / f, total / f * (g_nc_cal[0] - g_nc_cal[1]) / 1e6);
        else LOG("[nr] nocall cost model: no calibration yet (%s)", __atomic_load_n(&g_nc_live, __ATOMIC_RELAXED) < 0 ? "the engine's NVN pointers are not our thunks" : "no flush seen / no no-op entry point");
        LOG("[nr] nocall check (DM_NR_NOCALL=2, totals): %llu texture flushes compared, bad %llu (bind count %llu, a bind %llu, ranges not reset %llu, other NVN calls inside %llu); "
            "%llu draws 36 / 37 compared, bad %llu (the draw call %llu, SetRenderEnable %llu, the flag bytes %llu)",
            (unsigned long long)NC_T(f_cmp), (unsigned long long)NC_T(f_bad), (unsigned long long)NC_T(f_bad_n), (unsigned long long)NC_T(f_bad_bind), (unsigned long long)NC_T(f_bad_range),
            (unsigned long long)NC_T(f_bad_calls), (unsigned long long)NC_T(d_cmp), (unsigned long long)NC_T(d_bad), (unsigned long long)NC_T(d_bad_draw), (unsigned long long)NC_T(d_bad_ren),
            (unsigned long long)NC_T(d_bad_flag));
    } else {
        const double binds = (double)NC_D(flush_binds), skip = 2. * binds + draws + (double)NC_D(draw_ren), sw = draws + (double)NC_D(flush_with_binds_out);
        LOG("[nr] nocall (DM_NR_NOCALL=1, parts %d, %llu frames): native texture flushes %.1f a frame (%.1f inside native draws) with %.1f binds, native draws %.1f a frame "
            "(36: %.1f, 37: %.1f; SetRenderEnable %.2f): %.0f NVN calls a frame not made through the thunks, %.0f stack switches; left to the engine a frame: flushes %.2f "
            "(range outside the slots %.2f, no command buffer %.2f, unknown call site %.2f, part off %.2f), the flush of a native draw %.2f, draws without an index buffer "
            "record %.2f, not verified %.2f",
            g_nc_parts, (unsigned long long)frames, (double)NC_D(flushes) / f, (double)NC_D(flush_in_draw) / f, binds / f, draws / f, (double)NC_D(draws[0]) / f, (double)NC_D(draws[1]) / f,
            (double)NC_D(draw_ren) / f, skip / f, sw / f, (double)(NC_D(flush_noplan) + NC_D(flush_nocb) + NC_D(flush_site) + NC_D(flush_off)) / f, (double)NC_D(flush_noplan) / f,
            (double)NC_D(flush_nocb) / f, (double)NC_D(flush_site) / f, (double)NC_D(flush_off) / f, (double)NC_D(draw_flush_engine) / f, (double)NC_D(draw_norec) / f, (double)NC_D(not_live) / f);
        if (cal) LOG("[nr] nocall cost model (calibrated on the recording thread): an NVN call through thunk + big stack %.1f ns, its body called directly %.1f ns, one stack "
                     "switch %.1f ns: about %.3f ms a frame saved (%.0f calls x %.1f ns - %.0f switches x %.1f ns)",
                     g_nc_cal[0], g_nc_cal[1], g_nc_cal[2], nrnc_saving_ns(skip / f, sw / f, g_nc_cal[0], g_nc_cal[1], g_nc_cal[2]) / 1e6, skip / f, g_nc_cal[0] - g_nc_cal[1], sw / f, g_nc_cal[2]);
    }
#undef NC_D
#undef NC_T
}

// ---- install (dm_dos2_nr_install; guest code is not running yet) ----
// Before the flush hook goes in: DM_NR_NOCALL is read and the analysed code is compared; g_nc_want picks nr_h_tf_nc as the hook.
static void nc_arm(module_t *m) {
    const char *e = getenv("DM_NR_NOCALL");
    const int v = e ? atoi(e) : 0;
    if (!v) return;
    if (v != 1 && v != 2) { LOG("【红】[nr] DM_NR_NOCALL=%s: only 0 (off), 1 (the modelled calls run natively) and 2 (count + compare, the engine's code runs) exist - off", e); return; }
    static const nrnc_code code[NRNC_CODE_N] = NRNC_CODE_TABLE;
    for (int i = 0; i < NRNC_CODE_N; i++) {
        const uint64_t h = m->text_end >= code[i].end ? nrnc_fnv(m->base + code[i].off + 16, code[i].end - code[i].off - 16) : 0;
        if (h != code[i].fnv) {
            LOG("【红】[nr] DM_NR_NOCALL=%d: main+%#x..%#x (%s) is not the analysed code (hash %016llx, expected %016llx): not DOS2 update v786432 main (build 4979b200) - off",
                v, code[i].off, code[i].end, code[i].name, (unsigned long long)h, (unsigned long long)code[i].fnv);
            return;
        }
    }
    if ((e = getenv("DM_NR_NOCALL_PARTS")) && *e) g_nc_parts = atoi(e) & 3;
    g_nc_base = (uint64_t)(uintptr_t)m->base;
    g_nc_isz = (const uint8_t *)m->base + NRNC_ISZ_TABLE;
    g_nc_ubo_fill = (nc_guest_fn)(uintptr_t)(m->base + NRNC_FN_UBO_FILL); g_nc_ubo_bind = (nc_guest_fn)(uintptr_t)(m->base + NRNC_FN_UBO_BIND);
    g_nc_want = v;
}
typedef struct dn_draw_native { int (*take)(unsigned op, uint64_t r, uint64_t a1, uint64_t a2, uint64_t a4); void (*after)(unsigned op, uint64_t r); } dn_draw_native;
int dn_install_draw_native(const dn_draw_native *h);   // hle_dos2_native.c
static const dn_draw_native k_nc_draw = {nc_draw_take, nc_draw_after};
// After the flush hook: the draw hook's native handler, the packet handler of the counting mode, the NVN side.
static void nc_install(void) {
    const int v = g_nc_want;
    if (!v) return;
    if (!g_nr_key) { g_nc_want = 0; LOG("【红】[nr] DM_NR_NOCALL=%d: the texture flush hook is not in (it comes with DM_NR_DRAW=1|2 or DM_NR_KEY=1|2) - off", v); return; }
    int draw = 1;
    if (v == 2 || (g_nc_parts & 2)) draw = !dn_install_draw_native(&k_nc_draw);
    if (v == 2 && dn_install_rc(&k_nc_rc)) LOG("【红】[nr] DM_NR_NOCALL=2: no ExecuteRenderCommand handler (see above): the NVN calls are not split by packet opcode");
    nvn_nc_enable(v);
    g_nr_nocall = v;   // last: the hooks read it
    LOG("[nr] DM_NR_NOCALL=%d (NR-23): %s; texture flush main+%#x (25 call sites)%s; draw functions main+%#x / +%#x %s; the three functions hash to the analysed code; "
        "uniform blocks (main+%#x / +%#x) and every other packet stay the engine's",
        v, v == 1 ? "the modelled NVN calls of an engine draw run natively (the bodies of the entry points, on the big stack)" : "count + compare, the engine's code runs",
        NRNC_FN_FLUSH, v == 1 && !(g_nc_parts & 1) ? " (part off)" : "", NRNC_FN_DRAW, NRNC_FN_DRAWI,
        !draw ? "NOT hooked (see above): the flush only" : v == 1 && !(g_nc_parts & 2) ? "(part off)" : v == 1 ? "taken over after their index buffer lookup" : "compared",
        NRNC_FN_UBO_FILL, NRNC_FN_UBO_BIND);
}
