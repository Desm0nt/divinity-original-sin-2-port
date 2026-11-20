// [DOS2] NR-NATIVE step D, half b (DM_NRN_NOBODY, default 0; docs/dos2/nr/NR-NATIVE-D.md section 12): a CARD draw recorded without the
// bodies of the NVN entry points and without the big stack. Included by hle_dos2_nr_direct.inc right ahead of nrd_draw_take (it uses
// the job of the direct draw, k_nrd_ops and the uniform-block run of NR-09U).
//   DM_NRN_NOBODY=0 / unset  nothing: nrd_draw_take pays one load, the job runs on the big stack through the bodies as before.
//   DM_NRN_NOBODY=2          verify: the job runs as before; around the bodies of its binds the NVN half (nvn_nrn_b.inc) takes copies of
//                            the state the fold would start from, folds the same binds into the copies and compares them with what
//                            the bodies left - the two mirrors, the tap's table / dirty masks / signature, the shadow of the draw, the
//                            left-out mask of a short draw, the count of binds not sent, the use marks, the texture-set generation.
//                            A different word = BAD and a red line (first 16).
//   DM_NRN_NOBODY=1          apply: the binds of a card draw are folded in place (nvn_nrnb_binds), the stores of the flush and the
//                            tail of the draw function run here, the draw is recorded by the recorder's own nvn_nrd_draw (the tap's
//                            walk, nbd_draw, rec / cmd_alloc) on this stack. One timed card draw in 8 is kept on the bodies as the
//                            yardstick of the 10 s line (the two paths leave the same state, so mixing them is free).
// A draw stays on the bodies when: it carries no card (its pass is off) or no identity; the engine's own flush or uniform-block
// functions ran for it (their binds are already in the shadow); SetRenderEnable is due (rare, after a culled draw); the flush and the
// draw read different command buffers; the NVN half refuses (nvn_nrn_b.inc nrnb_can: another step's capture line, the tap or a mirror
// not yet following this recording, a shadow that is not empty, command memory about to run out).
#include "../gfx/nvn/nvn_nrn_b.h"
_Static_assert(sizeof(nvn_nrnb_tex) == sizeof(nrnc_bind) && offsetof(nvn_nrnb_tex, slot) == offsetof(nrnc_bind, slot) && offsetof(nvn_nrnb_tex, tex) == offsetof(nrnc_bind, tex) &&
               offsetof(nvn_nrnb_tex, smp) == offsetof(nrnc_bind, smp), "nvn_nrn_b.h nvn_nrnb_tex = nr_nocall.h nrnc_bind");
static int g_nrnb;                 // DM_NRN_NOBODY as armed (nrnb_arm): 0 / 1 apply / 2 verify
enum { NRNBC_FAST, NRNBC_NO_PLAN, NRNBC_NO_CARD, NRNBC_NO_REN, NRNBC_NO_UBO, NRNBC_NO_CB, NRNBC_NVN, NRNBC_YARD, NRNBC_VERIFY,
       NRNBC_T_FAST, NRNBC_NS_FAST, NRNBC_NSR_FAST, NRNBC_T_BODY, NRNBC_NS_BODY, NRNBC_NSR_BODY, NRNBC_N };
static uint64_t g_nrnbc[NRNBC_N];  // atomics, fed from the thread's counters with the direct draw's (once in 64 draws)
static __thread uint64_t t_nrnbc[NRNBC_N];
static __thread uint32_t t_nrnb_ty;   // timed eligible draws of this thread (every 8th is the yardstick)
static void nrnb_count_flush(void) { for (int i = 0; i < NRNBC_N; i++) if (t_nrnbc[i]) { __atomic_fetch_add(&g_nrnbc[i], t_nrnbc[i], __ATOMIC_RELAXED); t_nrnbc[i] = 0; } }
// 0 = the front end's side of the draw is the one the fold stands for; else the counter of the reason. Reads only.
static int nrnb_front(const uint8_t *R, const nrd_job_t *j) {
    if (!j->p) return NRNBC_NO_PLAN;                                                   // the engine's own flush ran (its binds went through the thunks)
    if (!g_nr_draw || !g_nr_key || !t_nk.idp || !t_nrn_pre.valid || !t_nrn_r) return NRNBC_NO_CARD;   // the record would carry no identity / no card
    if (R[NRNC_R_REN_OFF] && R[NRNC_R_REN_NEED]) return NRNBC_NO_REN;                  // SetRenderEnable is due: its body stays
    if (j->d->ubo && !j->ub) return NRNBC_NO_UBO;                                      // the engine's uniform-block functions ran: their binds are in the shadow
    if (j->cb != j->d->cb) return NRNBC_NO_CB;                                         // the flush and the draw read different command buffers
    return 0;
}
// =1. 1 = the draw is recorded (the big-stack job must not run); 0 = nothing was touched.
static int nrnb_take(uint8_t *R, nrd_job_t *j) {
    if (!nvn_nrnb_binds(j->cb, j->ub ? j->ub->call : NULL, j->ub ? j->ub->ncall : 0, (const nvn_nrnb_tex *)j->p->b, j->p->n)) { t_nrnbc[NRNBC_NVN]++; return 0; }
    if (j->ub && t_nru.timed) { t_nru.c[NRUC_TIMED]++; t_nru.c[NRUC_NS] += t_nru.t_model; t_nru.timed = 0; }   // what nru_job_calls books (the calls cost nothing here)
    for (int s = 0; s < NRNC_STAGES; s++) nrnc_flush_reset(R, s);                      // the stores of nrnc_flush_run (its binds are folded)
    nrnc_draw_tail(R, j->d, g_nrd_isz, &k_nrd_ops, j);                                 // nrd_op_draw in place: dm_nr_tap_draw + nvn_nrd_draw; the flag byte
    t_nrnbc[NRNBC_FAST]++;
    return 1;
}
#define NRNB_V_SNAP(j) do { if (__builtin_expect((j)->nbv, 0)) nvn_nrnb_snap((j)->cb, (j)->ub ? (j)->ub->call : NULL, (j)->ub ? (j)->ub->ncall : 0, (j)->p->n); } while (0)
#define NRNB_V_CHECK(j) do { if (__builtin_expect((j)->nbv, 0)) nvn_nrnb_check((j)->cb, (j)->ub ? (j)->ub->call : NULL, (j)->ub ? (j)->ub->ncall : 0, (const nvn_nrnb_tex *)(j)->p->b, (j)->p->n); } while (0)

// From nrd_report (every 10 s).
static void nrnb_report(uint64_t frames) {
    if (!g_nrnb) return;
    uint64_t c[NRNBC_N], n[NVN_NRNB_S_N];
    for (int i = 0; i < NRNBC_N; i++) c[i] = __atomic_exchange_n(&g_nrnbc[i], 0, __ATOMIC_RELAXED);
    nvn_nrnb_stats(n);
    static uint64_t cmp, bad;
    cmp += n[NVN_NRNB_CMP]; bad += n[NVN_NRNB_BAD];
    const double f = frames ? (double)frames : 1., tf = c[NRNBC_T_FAST] ? (double)c[NRNBC_T_FAST] : 1., tb = c[NRNBC_T_BODY] ? (double)c[NRNBC_T_BODY] : 1.;
    const double took = (double)(g_nrnb == 1 ? c[NRNBC_FAST] : n[NVN_NRNB_CMP]), binds = took > 0 ? took : 1.;
    LOG("[nr] native nobody (DM_NRN_NOBODY=%d, step D half b, last 10 s): card draws %s %.0f a frame (%.2f uniform-buffer + %.2f texture binds a draw); kept on the NVN bodies a "
        "frame: no card / identity %.1f, the engine's own flush %.1f, the engine's own uniform blocks %.1f, SetRenderEnable due %.2f, two command buffers %.2f, the yardstick %.2f; "
        "refused by the NVN half %.1f (not a direct draw now %.1f, a capture line of another step %.1f, the tap does not follow the recording %.1f, the shadow is not empty %.1f, "
        "a mirror not adopted %.1f, arguments %.1f, command memory %.2f); verify (totals): compared %llu, BAD %llu; the texture-set generation moved by another thread meanwhile %llu",
        g_nrnb, g_nrnb == 1 ? "recorded without the NVN bodies and the big stack" : "whose binds were folded next to the bodies and compared", took / f,
        (double)n[NVN_NRNB_UBO] / binds, (double)n[NVN_NRNB_TEX] / binds, (double)c[NRNBC_NO_CARD] / f, (double)c[NRNBC_NO_PLAN] / f, (double)c[NRNBC_NO_UBO] / f,
        (double)c[NRNBC_NO_REN] / f, (double)c[NRNBC_NO_CB] / f, (double)c[NRNBC_YARD] / f, (double)(g_nrnb == 1 ? c[NRNBC_NVN] : 0) / f, (double)n[NVN_NRNB_NO_MODE] / f,
        (double)n[NVN_NRNB_NO_CAP] / f, (double)n[NVN_NRNB_NO_TAP] / f, (double)n[NVN_NRNB_NO_SHADOW] / f, (double)n[NVN_NRNB_NO_MIRROR] / f, (double)n[NVN_NRNB_NO_ARGS] / f,
        (double)n[NVN_NRNB_NO_ROOM] / f, (unsigned long long)cmp, (unsigned long long)bad, (unsigned long long)n[NVN_NRNB_TEXGEN_MOVED]);
    if (g_nrnb == 1) {
        const double a = (double)c[NRNBC_NS_FAST] / tf, b = (double)c[NRNBC_NS_BODY] / tb, ar = (double)c[NRNBC_NSR_FAST] / tf, br = (double)c[NRNBC_NSR_BODY] / tb;
        LOG("[nr] native nobody, game-thread ns a card draw (1 in 64 timed; uniform blocks + identity + card + flush and record): without the NVN bodies %.0f (flush and record %.0f; "
            "%llu timed), through the bodies %.0f (flush and record %.0f; %llu timed: every 8th timed eligible draw is kept on that path as the yardstick) - %s%.0f ns a card draw, "
            "%.3f ms a frame at %.0f such draws",
            a, ar, (unsigned long long)c[NRNBC_T_FAST], b, br, (unsigned long long)c[NRNBC_T_BODY], c[NRNBC_T_BODY] && c[NRNBC_T_FAST] ? (b >= a ? "saved " : "LOST ") : "no yardstick yet: ",
            c[NRNBC_T_BODY] && c[NRNBC_T_FAST] ? (b >= a ? b - a : a - b) : 0., c[NRNBC_T_BODY] && c[NRNBC_T_FAST] ? (b - a) * (took / f) * 1e-6 : 0., took / f);
    }
}
// From dm_dos2_nr_install, after nrd_install / nru_install / nrub_install (guest code is not running yet).
static void nrnb_arm(void) {
    const char *e = getenv("DM_NRN_NOBODY");
    const int v = e && *e ? atoi(e) : 0;   // 0 on every device unless asked (the backend's policy function xbg_nrn_nobody_default says the same)
    if (!v) return;
    if (v != 1 && v != 2) { LOG("【红】[nr] native nobody: DM_NRN_NOBODY=%s is not 0 / 1 / 2: off", e); return; }
    if (!nrn_on()) { LOG("【红】[nr] native nobody: DM_NRN_NOBODY=%d needs a DM_NRN_<PASS> switch at 1 or 2 (only a draw with a card is taken): off", v); return; }
    if (g_nrd != 1 || !(g_nrd_parts & NRDP_DRAW)) { LOG("【红】[nr] native nobody: DM_NRN_NOBODY=%d needs DM_NR_DIRECT=1 with its draw part (it is %d, parts %#x): off", v, g_nrd, g_nrd_parts); return; }
    if (g_nru != 1) LOG("[nr] native nobody: DM_NR_UBO is not 1 - the engine's own uniform-block functions bind through the thunks, so only draws without uniform blocks can be taken");
    nvn_nrnb_enable(v);
    g_nrnb = v;   // last: nrd_draw_take reads it
    LOG("[nr] native nobody (NR-NATIVE step D half b, DM_NRN_NOBODY=%d): %s", v, v == 1 ? "the uniform-buffer and texture binds of a card draw are folded in place (mirrors, the tap's "
        "tables, texture-set generation, use marks, the draw's shadow) without the bodies of the NVN entry points and without the big stack; the tap's walk and the record stay the "
        "recorder's own code; one timed draw in 8 stays on the bodies as the yardstick" : "verify - the bodies run as before; the fold runs next to them on copies of the state and "
        "every word it would leave is compared with what the bodies left");
}
