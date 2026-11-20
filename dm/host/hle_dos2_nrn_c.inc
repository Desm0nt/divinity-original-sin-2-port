// [DOS2] NR-NATIVE step D, half c (DM_NRN_PKT, default 0; docs/dos2/nr/NR-NATIVE-D.md section 13): ONE packet instead of the group.
// Included by hle_dos2_nrn_d.inc (it extends the note of DM_NRN_LIST: nrnd_begin / nrnd_end at list construction, nrnd_exec_note at
// execution). The models and the walk are dm/gfx/nr/nr_pkt.h (pure C; host test tests/nr_pkt_test.c, machine-code oracle
// tests/nr_pkt_oracle.py). main = main.raw of update v786432, build 4979b200.
//   DM_NRN_PKT=0 / unset  nothing: the lists and their execution are those of DM_NRN_LIST alone.
//   DM_NRN_PKT=2          verify, nothing is executed differently:
//                         - list construction says for every noted group whether it would be merged (the chunk's packet count
//                           moved by exactly the group's packets, the group in the note's chunk);
//                         - execution: the note walks its group itself (alignment bytes + the length table) and the packets the
//                           engine's walker then delivers must be those, at those addresses, the last one ending where the merged
//                           packet would end ("walk" BAD);
//                         - every packet 0x0f / 0x1c / 0x11 / 0x39 / 0x3a the thread executes: the model lists what it would
//                           store (journal), the engine's handler runs, every listed byte must be what memory holds and the
//                           handler must return the model's length ("model" BAD).
//   DM_NRN_PKT=1          apply: the builder lowers the chunk's count by the group's packets and marks the note; at execution the
//                         note's handler runs the group's packets itself - through the same chain as the walker's call (every
//                         registered handler before and after, the native opcode-21 packet, the draw functions), minus the
//                         walker's trip through the gate per packet - and 0x0f / 0x1c / 0x11 / 0x39 / 0x3a by the models instead
//                         of the engine's handlers. One group in 16 is left unmerged as the yardstick of the 10 s line.
// Needs DM_NRN_LIST (1 or 2). The engine code the models and the merge stand for is hashed at install (NRPK_CODE_TABLE).
#include "../gfx/nr/nr_pkt.h"
#define NRPK_CODE_TABLE { \
    {0x9243d0, 0x924730, 0xc1c86c7ea6cbd1c0ull, "packet 0x0f handler 0x9243d0"}, \
    {0x924bb0, 0x925130, 0x70853fd8e44d6af3ull, "packet 0x1c handler 0x924bb0"}, \
    {0xa49c30, 0xa49ed4, 0xa88a8d099ea214a3ull, "the list walker 0xa49c30 (the only caller of ExecuteRenderCommand)"}, \
}
enum { NRPK_CODE_N = 3, NRNK_YARD = 16 };
uint64_t dn_rc_inner(uint64_t list, unsigned op, uint64_t pkt);   // hle_dos2_native.c: one packet through the handlers, as the hook does
int dn_install_rc_model(uint64_t mask, int (*model)(uint64_t list, unsigned op, uint64_t pkt, uint64_t *len));
static int g_nrnk;                               // DM_NRN_PKT as armed (nrnk_arm): 0 / 1 apply / 2 verify
static const uint8_t *g_nrnk_gdef;               // main + NRPK_GDEF
enum { NRNKC_MERGED, NRNKC_WOULD, NRNKC_NOT_FULL, NRNKC_CROSS, NRNKC_COUNT, NRNKC_YARD, NRNKC_EXEC_ONE, NRNKC_EXEC_PKTS, NRNKC_MODEL, NRNKC_RUN_BAD,
       NRNKC_WALK_CMP, NRNKC_WALK_BAD, NRNKC_M_CMP, NRNKC_M_OVER, NRNKC_M_BAD, NRNKC_T_ONE, NRNKC_NS_ONE, NRNKC_T_GRP, NRNKC_NS_GRP, NRNKC_P_ONE, NRNKC_P_GRP, NRNKC_N };
static uint64_t g_nrnkc[NRNKC_N];                // atomics
static unsigned g_nrnk_red;
static __thread int t_nrnk_in;                   // inside the merged group of a note (its packets are run by nrnk_run)
static __thread struct { uint64_t t0; uint32_t left, npkt; int one; } t_nrnk_tm;   // a timed group (1 in 64 notes)
static __thread uint32_t t_nrnk_n;
// =2: the group the note walked, to be met by the walker's packets
enum { NRNK_V_MAX = 64 };
static __thread struct { uint32_t n, at; uint64_t list, end, pkt[NRNK_V_MAX]; uint8_t op[NRNK_V_MAX]; } t_nrnk_v;
static __thread struct { int on; uint64_t pkt; uint32_t len; nrpk_journal j; } t_nrnk_m;   // =2: the journal of the packet being executed

// ---- list construction (any thread; nrnd_end, after the scan) ----
// count_note: the chunk's packet count right after the note's AllocCommand; chunk: the note's chunk; nseg: pieces the group lies in.
static void nrnk_built(nrnd_note *n, uint64_t chunk, uint32_t count_note, unsigned nseg, uint32_t seq) {
    if (!(n->flags & NRND_F_FULL)) { NR_INC(g_nrnkc[NRNKC_NOT_FULL]); return; }
    const uint32_t count_now = nrnd_r32(chunk + 0x10);
    if (nseg != 1) { NR_INC(g_nrnkc[NRNKC_CROSS]); return; }
    if (!nrpk_merge_ok(1, nseg, n->npkt, count_note, count_now)) { NR_INC(g_nrnkc[NRNKC_COUNT]); return; }
    if (g_nrnk != 1) { NR_INC(g_nrnkc[NRNKC_WOULD]); return; }
    if (!(seq % NRNK_YARD)) { NR_INC(g_nrnkc[NRNKC_YARD]); return; }   // the yardstick: this group stays as the engine wrote it
    const uint32_t c = count_now - n->npkt;
    memcpy((void *)(uintptr_t)(chunk + 0x10), &c, 4);                  // the walker counts the note, not the group's packets
    n->flags |= NRND_F_ONE;
    NR_INC(g_nrnkc[NRNKC_MERGED]);
}

// ---- execution (the thread that executes lists) ----
static void nrnk_tm_done(void) {
    const uint64_t d = nr_now() - t_nrnk_tm.t0;
    const int k = t_nrnk_tm.one ? NRNKC_T_ONE : NRNKC_T_GRP;
    NR_INC(g_nrnkc[k]); __atomic_fetch_add(&g_nrnkc[k + 1], d, __ATOMIC_RELAXED); __atomic_fetch_add(&g_nrnkc[t_nrnk_tm.one ? NRNKC_P_ONE : NRNKC_P_GRP], t_nrnk_tm.npkt, __ATOMIC_RELAXED);
    t_nrnk_tm.t0 = 0;
}
// Every packet the hook delivers (nr_rc_pre's first lines), outside a merged group: the clock of a timed group stops at the first
// packet after it; =2 - the packet against the note's own walk of its group.
static void nrnk_pkt(uint64_t list, unsigned op, uint64_t pkt) {
    if (t_nrnk_in) return;
    if (t_nrnk_tm.t0) { if (t_nrnk_tm.left) t_nrnk_tm.left--; else nrnk_tm_done(); }
    if (t_nrnk_v.at < t_nrnk_v.n) {
        const uint32_t i = t_nrnk_v.at++;
        if (list == t_nrnk_v.list && pkt == t_nrnk_v.pkt[i] && op == t_nrnk_v.op[i]) { if (t_nrnk_v.at == t_nrnk_v.n) NR_INC(g_nrnkc[NRNKC_WALK_CMP]); return; }
        NR_INC(g_nrnkc[NRNKC_WALK_BAD]); t_nrnk_v.n = 0;
        if (__atomic_fetch_add(&g_nrnk_red, 1, __ATOMIC_RELAXED) < 16)
            LOG("【红】[nr] native pkt: the walker's packet %u of a noted group is opcode %#x at %#llx (list %#llx) - the note's own walk expected %#x at %#llx (list %#llx)", i, op,
                (unsigned long long)pkt, (unsigned long long)list, t_nrnk_v.op[i], (unsigned long long)t_nrnk_v.pkt[i], (unsigned long long)t_nrnk_v.list);
    }
}
#define NRNK_PKT(list, op, pkt) do { if (__builtin_expect(g_nrnk, 0)) nrnk_pkt((list), (op), (pkt)); } while (0)
static uint64_t nrnk_one(void *u, unsigned op, uint64_t pkt) { NR_INC(g_nrnkc[NRNKC_EXEC_PKTS]); return dn_rc_inner((uint64_t)(uintptr_t)u, op, pkt); }
static uint64_t nrnk_walk_only(void *u, unsigned op, uint64_t pkt) {   // =2: the note's own walk (lengths from the table, nothing executed)
    (void)u;
    if (t_nrnk_v.n < NRNK_V_MAX) { t_nrnk_v.pkt[t_nrnk_v.n] = pkt; t_nrnk_v.op[t_nrnk_v.n] = (uint8_t)op; }
    t_nrnk_v.n++;
    return nrnd_pkt_len((const uint8_t *)(uintptr_t)pkt);
}
// From nrnd_exec_note: the note at pkt (already given to the chain's run). Returns the length the walker advances by.
static uint64_t nrnk_note(uint64_t list, uint64_t pkt) {
    const nrnd_note *n = (const nrnd_note *)(uintptr_t)pkt;
    if (t_nrnk_tm.t0) { if (!t_nrnk_tm.left) nrnk_tm_done(); else t_nrnk_tm.t0 = 0; }   // the list ended inside the timed group's count: dropped
    const int one = (n->flags & NRND_F_ONE) != 0, whole = (n->flags & (NRND_F_FULL | NRND_F_DRAW)) == (NRND_F_FULL | NRND_F_DRAW);
    if (whole && !(t_nrnk_n++ & 63)) { t_nrnk_tm.t0 = nr_now(); t_nrnk_tm.left = one ? 0 : n->npkt; t_nrnk_tm.npkt = n->npkt; t_nrnk_tm.one = one; }
    if (g_nrnk == 2 && (n->flags & NRND_F_FULL) && n->npkt && n->npkt <= NRNK_V_MAX) {
        int bad = 0;
        t_nrnk_v.n = 0; t_nrnk_v.at = 0; t_nrnk_v.list = list;
        t_nrnk_v.end = pkt + nrpk_group_run(pkt, n->npkt, nrnk_walk_only, NULL, &bad);
        if (bad) t_nrnk_v.n = 0;
    }
    if (!one) return NRND_NOTE_BYTES;
    int bad = 0;
    t_nrnk_in = 1;
    const uint64_t len = nrpk_group_run(pkt, n->npkt, nrnk_one, (void *)(uintptr_t)list, &bad);
    t_nrnk_in = 0;
    NR_INC(g_nrnkc[NRNKC_EXEC_ONE]);
    if (bad) { NR_INC(g_nrnkc[NRNKC_RUN_BAD]);
        if (__atomic_fetch_add(&g_nrnk_red, 1, __ATOMIC_RELAXED) < 16) LOG("【红】[nr] native pkt: a packet of a merged group returned no length (note %#llx, %u packets) - the table's length was taken", (unsigned long long)pkt, n->npkt); }
    return len;
}
// The models (hle_dos2_native.c calls it for the opcodes of the mask before the engine's function). =1: inside a merged group the
// packet is executed here (1, *len); =2: the journal of what the model would store (0: the engine's handler runs, nrnk_post compares).
static int nrnk_model(uint64_t list, unsigned op, uint64_t pkt, uint64_t *len) {
    (void)op;
    const uint64_t r = nrnd_r64(list + 0x38);
    uint32_t gdef; memcpy(&gdef, g_nrnk_gdef, 4);
    if (g_nrnk == 1) {
        if (!t_nrnk_in) return 0;   // outside a merged group (the yardstick, a group across chunks, lists without notes): the engine's handler
        const uint32_t l = nrpk_exec(NULL, r, pkt, gdef);
        if (!l) return 0;
        *len = l; NR_INC(g_nrnkc[NRNKC_MODEL]);
        return 1;
    }
    t_nrnk_m.j.n = t_nrnk_m.j.used = t_nrnk_m.j.over = 0;
    t_nrnk_m.len = nrpk_exec(&t_nrnk_m.j, r, pkt, gdef); t_nrnk_m.pkt = pkt; t_nrnk_m.on = 1;
    return 0;
}
// =2, after the engine's handler (a post handler of the chain).
static void nrnk_post(uint64_t list, unsigned op, uint64_t pkt, uint64_t len) {
    (void)list;
    if (t_nrnk_v.n && t_nrnk_v.at == t_nrnk_v.n) {   // the last packet of a walked group: the merged packet would end here
        if (pkt == t_nrnk_v.pkt[t_nrnk_v.n - 1] && pkt + len != t_nrnk_v.end) { NR_INC(g_nrnkc[NRNKC_WALK_BAD]);
            if (__atomic_fetch_add(&g_nrnk_red, 1, __ATOMIC_RELAXED) < 16) LOG("【红】[nr] native pkt: the last packet of a noted group (opcode %#x at %#llx) returned %llu bytes - the merged packet would end at %#llx", op,
                (unsigned long long)pkt, (unsigned long long)len, (unsigned long long)t_nrnk_v.end); }
        t_nrnk_v.n = 0;
    }
    if (!t_nrnk_m.on || t_nrnk_m.pkt != pkt) return;
    t_nrnk_m.on = 0;
    if (t_nrnk_m.j.over || !t_nrnk_m.len) { NR_INC(g_nrnkc[NRNKC_M_OVER]); return; }
    NR_INC(g_nrnkc[NRNKC_M_CMP]);
    uint64_t at = 0;
    if (len == t_nrnk_m.len && nrpk_journal_holds(&t_nrnk_m.j, &at)) return;
    NR_INC(g_nrnkc[NRNKC_M_BAD]);
    if (__atomic_fetch_add(&g_nrnk_red, 1, __ATOMIC_RELAXED) < 16)
        LOG("【红】[nr] native pkt: the model of opcode %#x is not what the engine's handler did - length %u by the model, %llu returned; first different store at %#llx (%u stores)", op,
            t_nrnk_m.len, (unsigned long long)len, (unsigned long long)at, t_nrnk_m.j.n);
}
static const dn_rc_handler k_nrnk_rc = {NULL, nrnk_post};

// From nrnd_report (every 10 s).
static void nrnk_report(uint64_t frames) {
    if (!g_nrnk) return;
    uint64_t c[NRNKC_N];
    for (int i = 0; i < NRNKC_N; i++) c[i] = __atomic_exchange_n(&g_nrnkc[i], 0, __ATOMIC_RELAXED);
    static uint64_t wc, wb, mc, mb, mo;
    wc += c[NRNKC_WALK_CMP]; wb += c[NRNKC_WALK_BAD]; mc += c[NRNKC_M_CMP]; mb += c[NRNKC_M_BAD]; mo += c[NRNKC_M_OVER];
    const double f = frames ? (double)frames : 1., t1 = c[NRNKC_T_ONE] ? (double)c[NRNKC_T_ONE] : 1., tg = c[NRNKC_T_GRP] ? (double)c[NRNKC_T_GRP] : 1.;
    LOG("[nr] native pkt (DM_NRN_PKT=%d, step D half c, last 10 s): list construction - groups %s %.0f a frame, left as they are: a packet of unknown length / a nested note %.1f, "
        "across two chunks %.1f, the chunk's count did not move by the group %.2f, the yardstick %.1f; execution - merged packets %.0f a frame with %.0f packets run by the note's "
        "handler, %.0f of them by the models (0x0f / 0x1c / 0x11 / 0x39 / 0x3a); a merged packet whose packet returned no length %llu; verify (totals): groups walked by the note "
        "and met by the walker %llu, BAD %llu; packets modelled next to the engine's handler %llu, BAD %llu, not comparable %llu",
        g_nrnk, g_nrnk == 1 ? "merged into their note" : "that would be merged", (double)(g_nrnk == 1 ? c[NRNKC_MERGED] : c[NRNKC_WOULD]) / f, (double)c[NRNKC_NOT_FULL] / f,
        (double)c[NRNKC_CROSS] / f, (double)c[NRNKC_COUNT] / f, (double)c[NRNKC_YARD] / f, (double)c[NRNKC_EXEC_ONE] / f, (double)c[NRNKC_EXEC_PKTS] / f, (double)c[NRNKC_MODEL] / f,
        (unsigned long long)c[NRNKC_RUN_BAD], (unsigned long long)wc, (unsigned long long)wb, (unsigned long long)mc, (unsigned long long)mb, (unsigned long long)mo);
    const double a = (double)c[NRNKC_NS_ONE] / t1, b = (double)c[NRNKC_NS_GRP] / tg;
    LOG("[nr] native pkt, game-thread ns a draw's group (note to the first packet after the group, the draw included; 1 note in 64 timed): one packet %.0f (%llu timed, %.1f "
        "packets a group), the group packet by packet %.0f (%llu timed, %.1f packets a group%s)%s%.0f ns a draw", a, (unsigned long long)c[NRNKC_T_ONE],
        (double)c[NRNKC_P_ONE] / t1, b, (unsigned long long)c[NRNKC_T_GRP], (double)c[NRNKC_P_GRP] / tg, g_nrnk == 1 ? ": the yardstick, one group in 16" : "",
        c[NRNKC_T_ONE] && c[NRNKC_T_GRP] ? (b >= a ? " - saved " : " - LOST ") : " - no pair yet: ", c[NRNKC_T_ONE] && c[NRNKC_T_GRP] ? (b >= a ? b - a : a - b) : 0.);
}
// From nrnd_arm, once DM_NRN_LIST is in (guest code is not running yet).
static void nrnk_arm(module_t *m, int list_mode) {
    const char *e = getenv("DM_NRN_PKT");
    const int v = e && *e ? atoi(e) : 0;   // 0 on every device unless asked (the backend's policy function xbg_nrn_pkt_default says the same)
    if (!v) return;
    if (v != 1 && v != 2) { LOG("【红】[nr] native pkt: DM_NRN_PKT=%s is not 0 / 1 / 2: off", e); return; }
    if (!list_mode) { LOG("【红】[nr] native pkt: DM_NRN_PKT=%d needs DM_NRN_LIST at 1 or 2 (the note in front of a group is the packet the group becomes): off", v); return; }
    static const nrpk_code code[NRPK_CODE_N] = NRPK_CODE_TABLE;
    for (int i = 0; i < NRPK_CODE_N; i++) {
        const uint64_t h = m->text_end >= code[i].end ? nrpk_fnv(m->base + code[i].off, code[i].end - code[i].off) : 0;
        if (h == code[i].fnv) continue;
        LOG("【红】[nr] native pkt: main+%#x..%#x (%s) is not the analysed code (hash %016llx, expected %016llx): not DOS2 update v786432 main (build 4979b200) - DM_NRN_PKT off",
            code[i].off, code[i].end, code[i].name, (unsigned long long)h, (unsigned long long)code[i].fnv);
        return;
    }
    g_nrnk_gdef = (const uint8_t *)m->base + NRPK_GDEF;
    if (dn_install_rc_model(1ull << NRPK_OP_TEX | 1ull << NRPK_OP_PARAM | 1ull << NRPK_OP_SAMP | 1ull << NRPK_OP_2A | 1ull << NRPK_OP_2B, nrnk_model)) {
        LOG("【红】[nr] native pkt: the packet models could not be registered with the ExecuteRenderCommand hook - DM_NRN_PKT off"); return; }
    if (v == 2 && dn_install_rc(&k_nrnk_rc)) { LOG("【红】[nr] native pkt: no room for the verify handler in the ExecuteRenderCommand chain - DM_NRN_PKT off"); return; }
    __atomic_store_n(&g_nrnk, v, __ATOMIC_RELEASE);
    LOG("[nr] native pkt (NR-NATIVE step D half c, DM_NRN_PKT=%d): %s", v, v == 1 ? "a noted group that lies in its note's chunk becomes the note's payload (the chunk's packet count is "
        "lowered by the group); the note's handler runs the group's packets through the handler chain itself and the texture / parameter / sampler / two-byte packets by the models "
        "(nr_pkt.h) instead of the engine's handlers; one group in 16 stays unmerged as the yardstick" : "verify - nothing is merged or modelled: the note's own walk of its group is "
        "compared with the packets the engine's walker delivers, and the model of every texture / parameter / sampler / two-byte packet with what the engine's handler stored");
}
