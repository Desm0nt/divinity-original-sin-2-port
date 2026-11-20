// [DOS2] NR-09S: the engine's "all state of a draw" packet as native code (DM_NR_STATE, default 0; docs/dos2/nr/NR-09S.md). Included by
// hle_dos2_nr.c after hle_dos2_nr_direct.inc. main = main.raw of update v786432, build 4979b200.
//   DM_NR_STATE=0 / unset  nothing: no handler, the ExecuteRenderCommand hook and the NVN entry points are byte for byte the old ones
//                          (19 NST lines: a load and a not-taken branch each).
//   DM_NR_STATE=1          opcode 21 (0x70 bytes; handler 0x93e6ac + SetShader 0x9283a0 + SetBlend 0x9284e0) does not run as machine code:
//                          the model of dm/gfx/nr/nr_state.h (proven against the machine code by tests/nr_state_oracle.py) reads the packet
//                          and the renderer, writes what the machine code writes (renderer fields, frame stamps) and hands the packet's NVN
//                          calls - the compact native state record - to the bodies of the entry points in ONE call on the big stack
//                          (nvn_nr_state.inc): no thunk, no big-stack wrapper per call, no engine code. The records the bodies write are
//                          the ones the engine's calls wrote (same bodies, same arguments, same order), so the NB-13a shadow, the NB tap, the
//                          backend and every lane downstream see the same stream.
//                          DM_NR_STATE_AB=1: packets alternate in blocks of 64 between the native path and the engine's (both timed in one
//                          run: the '[nr] state' line gives ns a packet for each).
//   DM_NR_STATE=2          the engine's handler runs; the model runs first on the same state and is compared with what the engine did:
//                          the NVN calls the entry points saw (every argument, in order), every renderer byte of r+0x16f0..0x1870 and
//                          r+0xb78, the frame stamps, the packet length (red lines, first 16 + totals every 10 s).
//   There is no =3: =1 makes the very calls the engine made; nothing is left stale that a poison could show.
//   DM_NR_STATE=4          NR-09S3 (docs/dos2/nr/NR-09S.md §10): as =1, but the calls are not made through the bodies: the packet becomes ONE
//                          OP_X_NR_STATE record (dm/gfx/nr/nr_state3.h; the bytes the bind bodies would have copied) that the backend applies
//                          straight into its State (xbg_nr_state3.inc) - no NVN state records, no NB-13a capture / dedup / pack for opcode 21,
//                          no OP_X_NB_STATE parse and nested execute() for it. The bodies' other effects are done natively (nvn_nr_state3.inc:
//                          the engine's blend / colour objects, the IR tap's programs, the NB-13a shadow's kinds made unknown = the lazy
//                          fallback for every packet the model does not take and every other opcode, whose calls keep writing plain records).
//                          A packet whose calls the record cannot represent goes through the bodies (=1's path). DM_NR_STATE_AB=1: blocks of
//                          64 packets alternate S3 / S1 (calls through the bodies), both timed.
//   DM_NR_STATE=5          the two-copy check of =4: the record (as CHECK: the backend applies it to a copy of its state) + the calls through the
//                          bodies (they write the real state, as =1) + CHECK_END (the backend compares every field the record can write: red
//                          lines '【红】[nr] state3 check', totals in '[nr] state3 (backend').
//   =4 / =5 need DM_NB_RELOC off (NB-14's key walk models plain state records only): otherwise =1 runs (red line).
// A packet the model cannot take (no renderer / device / recording context, a handle the machine code would turn into a bad pointer, a
// null table pointer, the engine's NVN pointers are not our thunks) runs the engine's handler, untouched: decided before anything is written.
#include "../gfx/nr/nr_state.h"
#include "../gfx/nvn/nvn_nr_state.h"
int dn_install_rc_take(uint64_t mask, int (*take)(uint64_t list, unsigned op, uint64_t pkt, uint64_t *len));
uint64_t dn_rc_call_orig(uint64_t list, unsigned op, uint64_t pkt);

static int g_nrs_want, g_nrs_ab;
static int g_nrs_live;              // atomic: 0 = not looked at yet, 1 = the engine's 19 pointers are our thunks, -1 = not (never native)
static uint64_t g_nrs_base;
static const uint8_t *g_nrs_prim, *g_nrs_special;
enum { NRSC_PKTS, NRSC_NATIVE, NRSC_ENGINE_AB, NRSC_CALLS, NRSC_STAMPS, NRSC_SWITCH, NRSC_NOTLIVE, NRSC_NOR, NRSC_TIMED, NRSC_NS, NRSC_TIMED_E, NRSC_NS_E,
       NRSC_CMP, NRSC_BAD_MEM, NRSC_BAD_CALLS, NRSC_BAD_LEN, NRSC_S3, NRSC_S3_FELL, NRSC_S3_BYTES, NRSC_S1AB, NRSC_REFUSE, NRSC_FN = NRSC_REFUSE + NRST_WHY_N, NRSC_N = NRSC_FN + NRST_F_N };
static uint64_t g_nrsc[NRSC_N];   // atomics, fed from the thread's counters once in 64 packets
static __thread struct { uint32_t n, seq, got3; int m3; uint64_t c[NRSC_N]; nrst_run *run; } t_nrs;   // seq / got3 / m3: NR-09S3
static unsigned g_nrs_red;
static uint64_t g_nrs_tot[4];     // =2 totals: compared, memory bad, calls bad, length bad (atomics)

static void nrs_count_flush(void) {
    for (int i = 0; i < NRSC_N; i++) if (t_nrs.c[i]) { __atomic_fetch_add(&g_nrsc[i], t_nrs.c[i], __ATOMIC_RELAXED); t_nrs.c[i] = 0; }
}
static int nrs_verify(void) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    int v = __atomic_load_n(&g_nrs_live, __ATOMIC_RELAXED);
    if (!v) {
        static const struct { uint32_t off; const char *name; } slot[NRST_F_N - 1] = NRST_SLOTS;
        const void *th[NRST_F_N - 1];
        const int ok = nvn_nrs_ready(th); int bad = -1;
        for (int k = 0; k < NRST_F_N - 1; k++) {
            const uint64_t got = nrst_r64((const uint8_t *)(uintptr_t)(g_nrs_base + slot[k].off));
            if ((!th[k] || got != (uint64_t)(uintptr_t)th[k]) && bad < 0) bad = k;
        }
        v = ok && bad < 0 ? 1 : -1;
        if (v > 0) LOG("[nr] state: the engine's 19 NVN pointers of opcode 21 are the thunks of the bodies the native packet calls");
        else LOG("【红】[nr] state: %s (first: %s at main+%#x): opcode 21 stays the engine's machine code", !ok ? "an NVN entry point is not implemented by the body the native packet calls"
                 : "an engine NVN pointer is not our thunk", bad >= 0 ? slot[bad].name : "-", bad >= 0 ? slot[bad].off : 0u);
        __atomic_store_n(&g_nrs_live, v, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&mu);
    return v > 0;
}
static inline int nrs_live(void) {
    const int v = __atomic_load_n(&g_nrs_live, __ATOMIC_RELAXED);
    if (__builtin_expect(v != 0, 1)) return v > 0;
    return nrs_verify();
}
static void *nrs_job(void *a) { const nrst_run *s = (const nrst_run *)a; nvn_nrs_exec(s->op, s->nops); return NULL; }
// NR-09S3: one record (m3 4 / 5); not representable or the S1 block of DM_NR_STATE_AB (m3 0): the calls through the bodies.
static void *nrs3_job(void *a) {
    const nrst_run *s = (const nrst_run *)a;
    const uint32_t got = t_nrs.m3 ? nvn_nrs3_exec(s->op, s->nops, s->cb, t_nrs.m3, t_nrs.seq) : 0;
    if (!got) nvn_nrs_exec(s->op, s->nops);
    t_nrs.got3 = got;
    return NULL;
}
static void nrs_red(const char *what, uint64_t pkt, const nrst_run *s, const nrst_op *g, int ng, int at, uint32_t where) {
    if (__atomic_fetch_add(&g_nrs_red, 1, __ATOMIC_RELAXED) >= 16) return;
    if (at >= 0 && at < NRST_OPS_MAX) {
        const nrst_op *m = at < s->nops ? &s->op[at] : NULL, *e = at < ng ? &g[at] : NULL;
        LOG("【红】[nr] state check: %s - packet %#llx, call %d of %d (model) / %d (engine): model fn %u a %#llx p %#llx q %#llx i %d j %d, engine fn %u a %#llx p %#llx q %#llx i %d j %d",
            what, (unsigned long long)pkt, at, s->nops, ng, m ? m->fn : 0u, m ? (unsigned long long)m->a : 0ull, m ? (unsigned long long)m->p : 0ull, m ? (unsigned long long)m->q : 0ull,
            m ? m->i : 0, m ? m->j : 0, e ? e->fn : 0u, e ? (unsigned long long)e->a : 0ull, e ? (unsigned long long)e->p : 0ull, e ? (unsigned long long)e->q : 0ull, e ? e->i : 0, e ? e->j : 0);
    } else if (!strcmp(what, "packet length")) LOG("【红】[nr] state check: the engine's handler returned length %#x for packet %#llx (the model: %#x)", where, (unsigned long long)pkt, NRST_PKT_LEN);
    else LOG("【红】[nr] state check: %s - packet %#llx: renderer offset %#x (%s)", what, (unsigned long long)pkt, where, where == ~0u ? "a frame stamp of a table record" : "r +");
}
// The native handler of opcode 21 (hle_dos2_native.c dn_rc_hook; the pre / post handlers - the NB-13a bracket - run around it as before).
// 1 = executed (*len = 0x70), 0 = the engine's handler runs.
static int nrs_take(uint64_t list, unsigned op, uint64_t pkt, uint64_t *len) {
    (void)op;
    nrst_run *s = t_nrs.run;
    if (__builtin_expect(!s, 0)) { s = t_nrs.run = (nrst_run *)calloc(1, sizeof *s); if (!s) return 0; }
    const uint32_t n = t_nrs.n++;
    const int timed = !(n & 63);
    if (timed) nrs_count_flush();
    t_nrs.c[NRSC_PKTS]++;
    if (!nrs_live()) { t_nrs.c[NRSC_NOTLIVE]++; return 0; }
    const uint8_t *R = (const uint8_t *)(uintptr_t)nrst_r64((const uint8_t *)(uintptr_t)list + 0x38);
    if (!R) { t_nrs.c[NRSC_NOR]++; return 0; }
    if (g_nrs == 1 && g_nrs_ab && (n >> 6 & 1)) {   // DM_NR_STATE_AB: this block of 64 is the engine's (timed the same way)
        t_nrs.c[NRSC_ENGINE_AB]++;
        const uint64_t t0 = timed ? nr_now() : 0;
        *len = dn_rc_call_orig(list, 21, pkt);
        if (timed) { t_nrs.c[NRSC_TIMED_E]++; t_nrs.c[NRSC_NS_E] += nr_now() - t0; }
        return 1;
    }
    const uint64_t t0 = timed ? nr_now() : 0;
    if (!nrst_code21(R, (const uint8_t *)(uintptr_t)pkt, nrst_r64(g_nrs_special), g_nrs_prim, s)) {
        t_nrs.c[NRSC_REFUSE + (s->why >= 0 && s->why < NRST_WHY_N ? s->why : 0)]++; return 0; }
    if (g_nrs == 1) {
        nrst_commit((uint8_t *)(uintptr_t)R, s);
        if (s->nops) { nvn_on_bigstack(nrs_job, s); t_nrs.c[NRSC_SWITCH]++; }
        *len = NRST_PKT_LEN;
        if (timed) { t_nrs.c[NRSC_TIMED]++; t_nrs.c[NRSC_NS] += nr_now() - t0; }
    } else if (g_nrs >= 4) {   // NR-09S3: =4 one OP_X_NR_STATE record, =5 it as CHECK + the calls through the bodies + CHECK_END
        nrst_commit((uint8_t *)(uintptr_t)R, s);
        const int s1 = g_nrs_ab && (n >> 6 & 1);   // DM_NR_STATE_AB (=4 only): this block of 64 makes the calls through the bodies (S1)
        t_nrs.m3 = s1 ? 0 : g_nrs; t_nrs.got3 = 0;
        if (s->nops) { t_nrs.seq++; nvn_on_bigstack(nrs3_job, s); t_nrs.c[NRSC_SWITCH]++; }
        *len = NRST_PKT_LEN;
        if (s1) t_nrs.c[NRSC_S1AB]++;
        else if (t_nrs.got3) { t_nrs.c[NRSC_S3]++; t_nrs.c[NRSC_S3_BYTES] += t_nrs.got3 + 8u; }
        else if (s->nops) t_nrs.c[NRSC_S3_FELL]++;
        if (timed) { const uint64_t d = nr_now() - t0; if (s1) { t_nrs.c[NRSC_TIMED_E]++; t_nrs.c[NRSC_NS_E] += d; } else { t_nrs.c[NRSC_TIMED]++; t_nrs.c[NRSC_NS] += d; } }
    } else {   // =2: the engine's handler on the state the model read; then the two are compared
        const uint64_t t1 = timed ? nr_now() : 0;
        uint8_t pre[NRST_REGION_END - NRST_REGION]; memcpy(pre, R + NRST_REGION, sizeof pre);
        const uint32_t pcs = nrst_r32(R + NRST_R_CSFLAG);
        nvn_nrs_cap_begin();
        const uint64_t t2 = timed ? nr_now() : 0;
        *len = dn_rc_call_orig(list, 21, pkt);
        const uint64_t t3 = timed ? nr_now() : 0;
        const nrst_op *g; const int ng = nvn_nrs_cap_end(&g);
        uint32_t where = 0;
        const int bm = nrst_check_mem(R, pre, pcs, s, &where);
        int at = -1; uint64_t loc[3] = {0, 0, 0};
        if (ng != s->nops) at = ng < s->nops ? ng : s->nops;
        for (int i = 0; at < 0 && i < ng && i < NRST_OPS_MAX; i++) if (!nrst_same_op(&s->op[i], &g[i], loc)) at = i;
        t_nrs.c[NRSC_CMP]++;
        if (bm) { t_nrs.c[NRSC_BAD_MEM]++; nrs_red("renderer memory", pkt, s, g, ng, -1, where); }
        if (at >= 0) { t_nrs.c[NRSC_BAD_CALLS]++; nrs_red("NVN calls", pkt, s, g, ng, at, 0); }
        if (*len != NRST_PKT_LEN) { t_nrs.c[NRSC_BAD_LEN]++; nrs_red("packet length", pkt, s, g, ng, -1, (uint32_t)*len); }
        if (timed) { t_nrs.c[NRSC_TIMED]++; t_nrs.c[NRSC_NS] += t1 - t0; t_nrs.c[NRSC_TIMED_E]++; t_nrs.c[NRSC_NS_E] += t3 - t2; }
    }
    t_nrs.c[NRSC_NATIVE]++; t_nrs.c[NRSC_CALLS] += (uint64_t)s->nops; t_nrs.c[NRSC_STAMPS] += (uint64_t)s->nstamps;
    for (int i = 0; i < s->nops && i < NRST_OPS_MAX; i++) if (s->op[i].fn < NRST_F_N) t_nrs.c[NRSC_FN + s->op[i].fn]++;
    return 1;
}

// ---- the 10 s lines (from nr_report) ----
static void nrs_report(uint64_t frames) {
    uint64_t c[NRSC_N];
    for (int i = 0; i < NRSC_N; i++) c[i] = __atomic_exchange_n(&g_nrsc[i], 0, __ATOMIC_RELAXED);
    for (int i = 0; i < 4; i++) __atomic_fetch_add(&g_nrs_tot[i], c[NRSC_CMP + i], __ATOMIC_RELAXED);
    const double f = frames ? (double)frames : 1., tm = c[NRSC_TIMED] ? (double)c[NRSC_TIMED] : 1., te = c[NRSC_TIMED_E] ? (double)c[NRSC_TIMED_E] : 1.;
    const double nat = c[NRSC_NATIVE] ? (double)c[NRSC_NATIVE] : 1.;
    static const char *const fname[NRST_F_N] = {"-", "BindProgram", "BindDepthStencilState", "SetStencilMask", "SetStencilValueMask", "SetStencilRef",
        "BindPolygonState", "BlendStateSetDefaults", "BindBlendState", "ColorStateSetDefaults", "ColorStateSetAlphaTest", "BindColorState",
        "BlendStateSetBlendTarget", "ColorStateSetBlendEnable", "BindVertexAttribState", "BindVertexStreamState", "BindVertexBuffer", "SetRenderEnable",
        "SetRenderEnableConditional", "SetAlphaRef"};
    char kinds[700]; int k = 0; kinds[0] = 0;
    for (int i = 1; i < NRST_F_N && k < (int)sizeof kinds - 64; i++) if (c[NRSC_FN + i]) k += snprintf(kinds + k, sizeof kinds - (size_t)k, "%s%s %.1f", k ? ", " : "", fname[i], (double)c[NRSC_FN + i] / f);
    char why[300]; int w = 0; why[0] = 0;
    static const char *const wname[NRST_WHY_N] = {"-", "no device", "no recording context", "bad depth-stencil handle", "bad raster handle", "bad blend handle", "bad vertex format handle", "null table pointer"};
    for (int i = 1; i < NRST_WHY_N && w < (int)sizeof why - 48; i++) if (c[NRSC_REFUSE + i]) w += snprintf(why + w, sizeof why - (size_t)w, "%s%s %.2f", w ? ", " : "", wname[i], (double)c[NRSC_REFUSE + i] / f);
    if (g_nrs == 1)
        LOG("[nr] state (DM_NR_STATE=1, NR-09S, %llu frames): opcode-21 packets %.1f a frame, native %.1f%s with %.1f NVN calls a frame made through the bodies "
            "(%.2f a packet, %.1f stack switches a frame; none through a thunk), %.1f frame stamps; left to the engine a frame: NVN pointers not ours %.2f, no renderer %.2f%s%s; "
            "calls a frame: %s; ns a packet (1 in 64 timed): native %.0f (%llu)%s",
            (unsigned long long)frames, (double)c[NRSC_PKTS] / f, (double)c[NRSC_NATIVE] / f, g_nrs_ab ? " (DM_NR_STATE_AB: the other half the engine's)" : "",
            (double)c[NRSC_CALLS] / f, (double)c[NRSC_CALLS] / nat, (double)c[NRSC_SWITCH] / f, (double)c[NRSC_STAMPS] / f, (double)c[NRSC_NOTLIVE] / f, (double)c[NRSC_NOR] / f,
            w ? ", " : "", why, k ? kinds : "none", (double)c[NRSC_NS] / tm, (unsigned long long)c[NRSC_TIMED], "");
    else if (g_nrs >= 4)
        LOG("[nr] state S3 (DM_NR_STATE=%d, NR-09S3, %llu frames): opcode-21 packets %.1f a frame, native %.1f%s; %s %.1f a frame (%.0f bytes a record, "
            "%.1f KB a frame) in place of %.1f NVN state calls a frame (%.2f a packet); not representable (calls through the bodies) %.2f a frame; %.1f stack "
            "switches a frame; left to the engine a frame: NVN pointers not ours %.2f, no renderer %.2f%s%s; calls a frame: %s; ns a packet (1 in 64 timed): %s %.0f (%llu)",
            g_nrs, (unsigned long long)frames, (double)c[NRSC_PKTS] / f, (double)c[NRSC_NATIVE] / f, g_nrs_ab ? " (DM_NR_STATE_AB: every other block of 64 through the bodies)" : "",
            g_nrs == 4 ? "one OP_X_NR_STATE record a packet" : "CHECK + CHECK_END record pairs next to the calls through the bodies (the backend compares: '[nr] state3')",
            (double)c[NRSC_S3] / f, c[NRSC_S3] ? (double)c[NRSC_S3_BYTES] / (double)c[NRSC_S3] : 0., (double)c[NRSC_S3_BYTES] / f / 1024., (double)c[NRSC_CALLS] / f,
            (double)c[NRSC_CALLS] / nat, (double)c[NRSC_S3_FELL] / f, (double)c[NRSC_SWITCH] / f, (double)c[NRSC_NOTLIVE] / f, (double)c[NRSC_NOR] / f, w ? ", " : "", why,
            k ? kinds : "none", g_nrs == 4 ? "S3 (model + stores + one record)" : "model + stores + both copies", (double)c[NRSC_NS] / tm, (unsigned long long)c[NRSC_TIMED]);
    else
        LOG("[nr] state check (DM_NR_STATE=2, NR-09S, %llu frames; the engine's handler runs): opcode-21 packets %.1f a frame, compared %.1f (totals: %llu compared, renderer "
            "memory bad %llu, NVN calls bad %llu, packet length bad %llu); left to the engine a frame: NVN pointers not ours %.2f, no renderer %.2f%s%s; calls a frame: %s",
            (unsigned long long)frames, (double)c[NRSC_PKTS] / f, (double)c[NRSC_CMP] / f, (unsigned long long)__atomic_load_n(&g_nrs_tot[0], __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&g_nrs_tot[1], __ATOMIC_RELAXED), (unsigned long long)__atomic_load_n(&g_nrs_tot[2], __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&g_nrs_tot[3], __ATOMIC_RELAXED), (double)c[NRSC_NOTLIVE] / f, (double)c[NRSC_NOR] / f, w ? ", " : "", why, k ? kinds : "none");
    if (g_nrs >= 4 && c[NRSC_TIMED_E])   // NR-09S3 DM_NR_STATE_AB
        LOG("[nr] state S3, ns a packet (1 in 64 timed): S3 (model + stores + one OP_X_NR_STATE record) %.0f (%llu), S1 (model + stores + the calls through "
            "the bodies, each a record through the NB-13a shadow) %.0f (%llu): %.1f us a frame saved on this thread at %.1f packets a frame (the NB-13a pack the S1 "
            "calls cause later, before the next draw, is not in the S1 number)", (double)c[NRSC_NS] / tm, (unsigned long long)c[NRSC_TIMED], (double)c[NRSC_NS_E] / te,
            (unsigned long long)c[NRSC_TIMED_E], ((double)c[NRSC_NS_E] / te - (double)c[NRSC_NS] / tm) * (double)c[NRSC_PKTS] / f / 1000., (double)c[NRSC_PKTS] / f);
    else if (c[NRSC_TIMED_E] || g_nrs == 2)
        LOG("[nr] state, ns a packet (1 in 64 timed): %s %.0f (%llu), the engine's handler with its NVN calls through the thunks %.0f (%llu): %.1f us a frame saved at %.1f "
            "packets a frame", g_nrs == 1 ? "native (model + stores + calls through the bodies)" : "the model alone", (double)c[NRSC_NS] / tm, (unsigned long long)c[NRSC_TIMED],
            (double)c[NRSC_NS_E] / te, (unsigned long long)c[NRSC_TIMED_E], g_nrs == 1 ? ((double)c[NRSC_NS_E] / te - (double)c[NRSC_NS] / tm) * (double)c[NRSC_PKTS] / f / 1000. : 0.,
            (double)c[NRSC_PKTS] / f);
}

// ---- install (dm_dos2_nr_install; guest code is not running yet) ----
static void nrs_arm(module_t *m) {
    const char *e = getenv("DM_NR_STATE");
    const int v = e ? atoi(e) : 0;
    if (!v) return;
    if (v != 1 && v != 2 && v != 4 && v != 5) { LOG("【红】[nr] DM_NR_STATE=%s: only 0 (off), 1 (opcode 21 natively), 2 (the engine's handler + the model, compared), "
        "4 (NR-09S3: one native state record a packet) and 5 (4's two-copy check) exist - off", e); return; }
    static const nrst_code code[NRST_CODE_N] = NRST_CODE_TABLE;
    for (int i = 0; i < NRST_CODE_N; i++) {
        const uint64_t h = m->text_end >= code[i].end ? nrst_fnv(m->base + code[i].off + 16, code[i].end - code[i].off - 16) : 0;
        if (h == code[i].fnv) continue;
        LOG("【红】[nr] DM_NR_STATE=%d: main+%#x..%#x (%s) is not the analysed code (hash %016llx, expected %016llx): not DOS2 update v786432 main (build 4979b200) - off",
            v, code[i].off, code[i].end, code[i].name, (unsigned long long)h, (unsigned long long)code[i].fnv);
        return;
    }
    uint16_t jt; memcpy(&jt, m->base + NRST_JT + 2 * 21, 2);
    if (NRST_JT_BASE + 4u * jt != NRST_CODE21) { LOG("【红】[nr] DM_NR_STATE=%d: the jump table sends opcode 21 to main+%#x, not %#x - off", v, NRST_JT_BASE + 4u * jt, NRST_CODE21); return; }
    g_nrs_base = (uint64_t)(uintptr_t)m->base;
    g_nrs_prim = (const uint8_t *)m->base + NRST_PRIM_TABLE; g_nrs_special = (const uint8_t *)m->base + NRST_SPECIAL_BLEND;
    int w = v;
    if (v >= 4) {   // NR-09S3: NB-14's key walk (DM_NB_RELOC) models the plain state records only
        const char *r = getenv("DM_NB_RELOC");
        if (r && (atoi(r) == 1 || atoi(r) == 2)) { LOG("【红】[nr] DM_NR_STATE=%d (NR-09S3): DM_NB_RELOC=%s keys the plain state records of a pass, an OP_X_NR_STATE record is not one - DM_NR_STATE=1 instead", v, r); w = 1; }
    }
    { const char *a = getenv("DM_NR_STATE_AB"); g_nrs_ab = (w == 1 || w == 4) && a && atoi(a) == 1; }
    g_nrs_want = w;
}
static void nrs_install(void) {
    const int v = g_nrs_want;
    if (!v) return;
    nvn_nrs_enable(v);
    if (dn_install_rc_take(1ull << 21, nrs_take)) { nvn_nrs_enable(0); LOG("【红】[nr] DM_NR_STATE=%d: no native opcode handler on ExecuteRenderCommand (see above) - off", v); return; }
    g_nrs = v;   // last: the handler reads it
    LOG("[nr] DM_NR_STATE=%d (NR-09S): %s; SetShader main+%#x / SetBlend main+%#x inside it; %s", v,
        v == 1 ? "opcode 21 (all state of a draw, main+0x93e6ac) runs as native code: the model's stores + its NVN calls through the bodies in one call on the big stack"
        : v == 4 ? "opcode 21 runs as native code and its state is ONE OP_X_NR_STATE record a packet (NR-09S3): no NVN state records, no NB-13a capture for it; the backend applies it to its State"
        : v == 5 ? "NR-09S3 two-copy check: opcode 21 natively, each packet's state both as an OP_X_NR_STATE record (applied by the backend to a copy) and as the calls through the bodies; compared per packet"
               : "opcode 21 runs as the engine's machine code; the model runs first on the same state and the NVN calls, renderer bytes, stamps and length are compared",
        0x9283a0u, 0x9284e0u, g_nrs_ab ? (v == 4 ? "DM_NR_STATE_AB=1: blocks of 64 packets alternate S3 / S1 (the calls through the bodies), both timed" : "DM_NR_STATE_AB=1: blocks of 64 packets alternate native / engine, both timed") : "the other opcodes stay the engine's");
}
