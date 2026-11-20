// NR-07 host test: dm/gfx/nr/nr_pipe.{h,c} (DM_NR_PIPE warm-up scheduler + the direct pipeline table) and a micro benchmark of the
// table against NB-13b's per-draw memo step (dm/gfx/nb/nb_pipe.h, DM_NB_PIPE, on by default) on a draw stream shaped like the phone's.
// clang -std=c11 -O2 -Wall -Wextra dm/gfx/vulkan/engine/tests/nr_pipe_test.c dm/gfx/nr/nr_pipe.c -o nr_pipe_test && ./nr_pipe_test
// Units: key parsing; hint order (a record is hinted only when every stage key was named; hinted first, FIFO, then list order);
// lazy (hinted only); shadow (DM_NR_PIPE=2: nothing reordered); claim / need states; lead buckets; the table (find / put / move to
// front / kill / revive / clear / full). Model: random hints, draws (need / took / claim) and worker steps against invariants - every
// record built at most once, a claimed record never built by the warm-up, lazy builds hinted records only, the warm-up picks a hinted
// FREE record whenever one exists, every record ends built / claimed / failed (not lazy). Mutation: a scheduler that ignores the
// queue must fail the order invariant (the test sees it).
#include "../../../nr/nr_pipe.h"
#include "../../../nb/nb_pipe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static double now_ns(void) { struct timespec t; timespec_get(&t, TIME_UTC); return t.tv_sec * 1e9 + t.tv_nsec; }

static void unit_keys(void) {
    CHECK(nrp_key_parse("69e0b8f6c34f058f", 16) == 0x69e0b8f6c34f058full);
    CHECK(nrp_key_parse("69E0B8F6C34F058F", 16) == 0x69e0b8f6c34f058full);
    CHECK(nrp_key_parse("69e0b8f6c34f058f#at3_3f000000", 29) == 0x69e0b8f6c34f058full);
    CHECK(nrp_key_parse("69e0b8f6c34f058f#vi12", 21) == 0x69e0b8f6c34f058full);
    CHECK(nrp_key_parse("69e0b8f6c34f058fx", 17) == 0);   // longer, not a variant
    CHECK(nrp_key_parse("69e0b8f6c34f058", 15) == 0);
    CHECK(nrp_key_parse("69e0b8f6c34f05g8", 16) == 0);
    CHECK(nrp_key_parse("0000000000000000", 16) == 1);
    CHECK(nrp_key_parse(NULL, 16) == 0);
    // NR-07 fix: the stage key of a record (nrp_stage_key) - our fused post bundles are hinted by the host shader they replace
    { int f = 9;
      CHECK(nrp_stage_key("69e0b8f6c34f058f", 16, &f) == 0x69e0b8f6c34f058full && f == 0);
      CHECK(nrp_stage_key("69e0b8f6c34f058f#at5_3f000000", 29, &f) == 0x69e0b8f6c34f058full && f == 0);
      CHECK(nrp_stage_key("F1A.91eb2d97a304709a", 20, &f) == 0x91eb2d97a304709aull && f == 1);   // phone list nr4_b2: records 1188 / 1223
      f = 9; CHECK(nrp_stage_key("F1.91eb2d97a304709a", 19, &f) == 0x91eb2d97a304709aull && f == 1);   // records 1183 / 1240
      f = 9; CHECK(nrp_stage_key("F2.0123456789abcdef", 19, &f) == 0x0123456789abcdefull && f == 1);
      f = 9; CHECK(nrp_stage_key("F1AS.0123456789abcdef", 21, &f) == 0x0123456789abcdefull && f == 1);
      f = 9; CHECK(nrp_stage_key("F1AR.0123456789abcdef#at5_3f000000", 34, &f) == 0x0123456789abcdefull && f == 1);
      f = 9; CHECK(nrp_stage_key("F3.0123456789abcdef", 19, &f) == 0 && f == 0);          // not ours
      CHECK(nrp_stage_key("F1ARX.0123456789abcdef", 22, &f) == 0 && f == 0);                 // three letters
      CHECK(nrp_stage_key("F1a.0123456789abcdef", 20, &f) == 0);                              // lower case: not ours
      CHECK(nrp_stage_key("G1.0123456789abcdef", 19, &f) == 0);
      CHECK(nrp_stage_key("F1.0123456789abcde", 18, &f) == 0);                                // 15 digits
      CHECK(nrp_stage_key("F1.0123456789abcdefx", 20, &f) == 0);                              // longer, not a variant
      CHECK(nrp_stage_key("F1.", 3, &f) == 0 && nrp_stage_key(NULL, 20, &f) == 0 && nrp_stage_key("F1A.91eb2d97a304709a", 20, NULL) == 0x91eb2d97a304709aull);
      CHECK(nrp_key_parse("F1A.91eb2d97a304709a", 20) == 0);                                  // hints stay plain keys
      // a record (VS, fused FS) is hinted once the game set up the host program (its VS and the host FS)
      const uint64_t K[2][NRP_STAGES] = {{0xd52cf8500f1e95d3ull, 0}, {0xd52cf8500f1e95d3ull, 0}};
      uint64_t k2[2][NRP_STAGES]; memcpy(k2, K, sizeof k2);
      k2[0][1] = nrp_stage_key("F1A.91eb2d97a304709a", 20, NULL); k2[1][1] = nrp_stage_key("F1.91eb2d97a304709a", 19, NULL);
      const uint8_t N[2] = {2, 2}; nrp_sched s; int fh;
      CHECK(nrp_init(&s, 2, &k2[0][0], N, 1) == 0);
      CHECK(nrp_hint(&s, 0xd52cf8500f1e95d3ull, 5) == 0 && nrp_next(&s, &fh) == -1);
      CHECK(nrp_hint(&s, 0x91eb2d97a304709aull, 6) == 2 && s.rec[0].t_hint == 6 && s.rec[1].t_hint == 6);
      CHECK(nrp_next(&s, &fh) == 0 && fh && nrp_next(&s, &fh) == 1 && fh);   // lazy builds them (before the fix: key 0, never hinted)
      nrp_free(&s);
      k2[0][1] = 0; CHECK(nrp_init(&s, 2, &k2[0][0], N, 1) == 0);   // the old parse (0): never hinted, never built when lazy
      nrp_hint(&s, 0xd52cf8500f1e95d3ull, 5); nrp_hint(&s, 0x91eb2d97a304709aull, 6);
      CHECK(!s.rec[0].t_hint && s.rec[1].t_hint == 6 && nrp_next(&s, &fh) == 1 && nrp_next(&s, &fh) == -1);
      nrp_free(&s); }
    CHECK(nrp_lead_bucket(0) == 0 && nrp_lead_bucket(49999999) == 0 && nrp_lead_bucket(50000000) == 1 && nrp_lead_bucket(999999999) == 2);
    CHECK(nrp_lead_bucket(4999999999ull) == 3 && nrp_lead_bucket(29999999999ull) == 4 && nrp_lead_bucket(30000000000ull) == 5);
}

// records: 0 (A,B) 1 (A,C) 2 (D,B) 3 (E) 4 (A,B,F) 5 (no key: a module without a bundle key) 6 (C,C)
static nrp_sched mk(int lazy) {
    static const uint64_t K[7][NRP_STAGES] = {{0xA, 0xB}, {0xA, 0xC}, {0xD, 0xB}, {0xE}, {0xA, 0xB, 0xF}, {0}, {0xC, 0xC}};
    static const uint8_t N[7] = {2, 2, 2, 1, 3, 1, 2};
    nrp_sched s; CHECK(nrp_init(&s, 7, &K[0][0], N, lazy) == 0);
    return s;
}
static void unit_sched(void) {
    nrp_sched s = mk(0); int fh;
    CHECK(s.nkent == 6 && s.left == 7);
    CHECK(nrp_hint(&s, 0xA, 10) == 0);                    // A alone completes no record
    CHECK(nrp_hint(&s, 0x99, 11) == 0 && s.st.hint_keys_new == 1);
    CHECK(nrp_hint(&s, 0xB, 12) == 1);                    // (A,B) -> record 0
    CHECK(nrp_hint(&s, 0xB, 13) == 0 && s.st.hint_dup == 1);
    CHECK(nrp_hint(&s, 0xC, 14) == 2);                    // (A,C) and (C,C)
    CHECK(s.rec[0].t_hint == 12 && s.rec[1].t_hint == 14 && s.rec[6].t_hint == 14 && !s.rec[2].t_hint);
    CHECK(nrp_next(&s, &fh) == 0 && fh);
    CHECK(nrp_next(&s, &fh) == 1 && fh);
    CHECK(nrp_need(&s, 6, 20, 1) == NRP_FREE && s.rec[6].phase == NRP_CLAIMED && s.st.need_hinted == 1 && s.st.lead[0] == 1);   // the draw claims 6
    CHECK(nrp_next(&s, &fh) == 2 && !fh);                  // queue: 6 claimed -> skipped; list order from 0: 0,1 taken -> 2
    CHECK(nrp_need(&s, 2, 30, 1) == NRP_BUILDING && s.rec[2].phase == NRP_BUILDING && s.st.need_inflight == 1 && s.st.need_unhinted == 1);
    nrp_done(&s, 2, 1, 40); CHECK(s.rec[2].phase == NRP_READY && s.st.built_list == 1);
    nrp_took(&s, 2, 41); CHECK(s.rec[2].phase == NRP_TAKEN && s.st.took_unhinted == 1);
    nrp_done(&s, 0, 1, 50); nrp_done(&s, 1, 0, 51); CHECK(s.st.built_hint == 1 && s.st.failed == 1 && s.rec[1].phase == NRP_FAILED);
    nrp_took(&s, 0, 52); CHECK(s.st.took_hinted == 1 && s.st.took_lead[0] == 1);
    nrp_took(&s, 0, 53); CHECK(s.st.taken == 2);            // a second take of a taken record does nothing
    CHECK(nrp_next(&s, &fh) == 3);
    CHECK(nrp_next(&s, &fh) == 4);
    CHECK(nrp_next(&s, &fh) == 5);                          // no key: built in list order
    CHECK(nrp_next(&s, &fh) == -1 && s.st.dup_avoided == 1);   // 6 claimed: the warm-up skips it
    CHECK(s.left == 0);
    CHECK(nrp_hint(&s, 0xF, 60) == 0 && s.rec[4].t_hint == 60);   // (A,B,F) hinted, already building: timed, not queued
    nrp_free(&s);

    s = mk(1);   // lazy
    CHECK(nrp_next(&s, &fh) == -1);
    nrp_hint(&s, 0xE, 1); CHECK(nrp_next(&s, &fh) == 3 && fh); CHECK(nrp_next(&s, &fh) == -1);
    nrp_hint(&s, 0xD, 2); nrp_hint(&s, 0xB, 3); CHECK(nrp_next(&s, &fh) == 2); CHECK(nrp_next(&s, &fh) == -1);   // (A,B) not: A unnamed
    nrp_free(&s);

    s = mk(0); s.shadow = 1;   // DM_NR_PIPE=2: hints timed, order unchanged
    nrp_hint(&s, 0xD, 1); nrp_hint(&s, 0xB, 2); CHECK(s.rec[2].t_hint == 2 && s.qtail == 0);
    for (int i = 0; i < 7; i++) CHECK(nrp_next(&s, &fh) == i && !fh);
    nrp_free(&s);
    nrp_sched z; CHECK(nrp_init(&z, 0, NULL, NULL, 0) == 0); CHECK(nrp_next(&z, &fh) == -1); CHECK(nrp_hint(&z, 5, 1) == 0); nrp_free(&z);
}

// ---- model: random hints / worker steps / draws against invariants ----
typedef struct { uint64_t runs, recs, hints, built, claimed, need, waited, order_bad, dup_build, lazy_bad, claim_built, left_bad; } mstat;
static void model(int lazy, int mutate, mstat *ms) {
    enum { MAXR = 400, MAXK = 120 };
    const uint32_t n = 20 + (uint32_t)(rnd() % (MAXR - 20)), nk = 6 + (uint32_t)(rnd() % (MAXK - 6));
    static uint64_t keys[MAXR][NRP_STAGES]; static uint8_t nks[MAXR], builds[MAXR], claimedby[MAXR];
    for (uint32_t i = 0; i < n; i++) {
        nks[i] = (uint8_t)(1 + rnd() % 2 + (rnd() % 10 == 0));   // 1..3 stages, mostly 2 (VS + FS)
        for (int k = 0; k < NRP_STAGES; k++) keys[i][k] = k < nks[i] ? 0x1000 + rnd() % nk : 0;
        if (rnd() % 40 == 0) keys[i][0] = 0;                     // a module without a bundle key
        builds[i] = claimedby[i] = 0;
    }
    nrp_sched s; CHECK(nrp_init(&s, n, &keys[0][0], nks, lazy) == 0);
    uint8_t named[0x1000 + MAXK] = {0};
    int busy[3] = {-1, -1, -1}; uint64_t t = 1;
    for (int step = 0; step < 6000; step++, t += 1000) {
        const uint64_t x = rnd() % 100;
        if (x < 15) { const uint64_t k = 0x1000 + rnd() % nk; nrp_hint(&s, k, t); named[k] = 1; ms->hints++; }
        else if (x < 60) {   // a worker step
            const int w = (int)(rnd() % 3);
            if (busy[w] >= 0) { nrp_done(&s, (uint32_t)busy[w], rnd() % 20 != 0, t); busy[w] = -1; continue; }
            int any_hinted_free = 0;
            if (!mutate) for (uint32_t i = 0; i < n; i++) if (s.rec[i].phase == NRP_FREE && s.rec[i].queued) { any_hinted_free = 1; break; }
            int fh = 0, i;
            if (mutate) { i = -1; while (s.cursor < s.nrec) { const uint32_t j = s.cursor++; if (s.rec[j].phase == NRP_FREE) { s.rec[j].phase = NRP_BUILDING; s.left--; i = (int)j; break; } }   // ignores the queue
                          for (uint32_t j = 0; j < n; j++) if (s.rec[j].phase == NRP_FREE && s.rec[j].queued) any_hinted_free = 1; }
            else i = nrp_next(&s, &fh);
            if (i < 0) continue;
            if (any_hinted_free && !s.rec[i].queued) ms->order_bad++;
            if (lazy && !s.rec[i].t_hint) ms->lazy_bad++;
            if (claimedby[i]) ms->claim_built++;
            if (++builds[i] > 1) ms->dup_build++;
            busy[w] = i; ms->built++;
        } else {   // a draw needs a record (the ready set missed or hit)
            const uint32_t i = (uint32_t)(rnd() % n);
            const int st = s.rec[i].phase;
            if (st == NRP_READY) { nrp_took(&s, i, t); continue; }
            const int was = nrp_need(&s, i, t, 1); ms->need++;
            if (was == NRP_FREE) { claimedby[i] = 1; ms->claimed++; }
            if (was == NRP_BUILDING) ms->waited++;
        }
    }
    for (int w = 0; w < 3; w++) if (busy[w] >= 0) nrp_done(&s, (uint32_t)busy[w], 1, t);
    if (!mutate) {   // drain: everything ends built / claimed / failed (lazy: only hinted ones are built)
        int fh; for (int i; (i = nrp_next(&s, &fh)) >= 0;) { if (++builds[i] > 1) ms->dup_build++; if (claimedby[i]) ms->claim_built++; if (lazy && !s.rec[i].t_hint) ms->lazy_bad++; nrp_done(&s, (uint32_t)i, 1, t); }
        for (uint32_t i = 0; i < n; i++) {
            const int st = s.rec[i].phase;
            if (!lazy && (st == NRP_FREE || st == NRP_BUILDING)) ms->left_bad++;
            if (lazy && st == NRP_FREE && s.rec[i].t_hint) ms->left_bad++;
        }
        if (!lazy && s.left) ms->left_bad++;
    }
    (void)named;
    ms->runs++; ms->recs += n;
    nrp_free(&s);
}

// ---- the table ----
static void unit_tab(void) {
    nrp_tab t; CHECK(nrp_tab_init(&t) == 0);
    nrp_var a = {{1, 2, 3, 4, 5, 6}}, b = a; b.w[5] = 7;
    CHECK(!nrp_tab_find(&t, 3, &a) && t.misses == 1);
    nrp_tent *e = nrp_tab_put(&t, 3, &a); CHECK(e && e->zero == -1 && e->key == 3); e->pipe = 100;
    nrp_tent *f = nrp_tab_put(&t, 3, &b); CHECK(f && f != e); f->pipe = 200;
    CHECK(nrp_tab_put(&t, 3, &a) == e);
    const nrp_tent *g = nrp_tab_find(&t, 3, &a); CHECK(g && g->pipe == 100 && t.hits == 1);   // second in the row: moved to front
    CHECK(t.head[3] == (uint32_t)(e - t.e) + 1);
    g = nrp_tab_find(&t, 3, &a); CHECK(g && t.first_hits == 1);
    CHECK(!nrp_tab_find(&t, 4, &a));
    CHECK(!nrp_tab_find(&t, NRP_TAB_ROWS, &a) && !nrp_tab_put(&t, NRP_TAB_ROWS, &a));
    nrp_tab_kill(&t, 100); CHECK(!nrp_tab_find(&t, 3, &a) && t.killed == 1);
    e = nrp_tab_put(&t, 3, &a); CHECK(e && e->pipe == 0); e->pipe = 101; CHECK(nrp_tab_find(&t, 3, &a)->pipe == 101);   // revived
    g = nrp_tab_find(&t, 3, &b); CHECK(g && g->pipe == 200);
    nrp_tab_clear(&t); CHECK(!nrp_tab_find(&t, 3, &a) && t.n == 0);
    for (uint32_t i = 0; i < NRP_TAB_MAX; i++) { nrp_var v = a; v.w[0] = i; nrp_tab_put(&t, 1 + i % 100, &v)->pipe = 1000 + i; }
    CHECK(t.n == NRP_TAB_MAX && t.full == 0);
    { nrp_var v = a; v.w[0] = 77; CHECK(nrp_tab_find(&t, 1 + 77 % 100, &v)->pipe == 1077); }
    { nrp_var v = a; v.w[0] = 999999; nrp_tab_put(&t, 5, &v); CHECK(t.full == 1 && t.n == 1); }
    // random against a reference (key, variant) -> pipe
    enum { RK = 64, RV = 4 }; static uint64_t ref[RK][RV];
    memset(ref, 0, sizeof ref); nrp_tab_clear(&t); uint64_t bad = 0, pipe = 1;
    for (int i = 0; i < 200000; i++) {
        const uint32_t k = 1 + (uint32_t)(rnd() % RK), v = (uint32_t)(rnd() % RV); nrp_var var = a; var.w[2] = v;
        const uint64_t x = rnd() % 100;
        if (x < 70) { const nrp_tent *h = nrp_tab_find(&t, k, &var); if ((h ? h->pipe : 0) != ref[k - 1][v]) bad++; }
        else if (x < 95) { nrp_tent *p = nrp_tab_put(&t, k, &var); if (!p) { bad++; continue; } p->pipe = ref[k - 1][v] = ++pipe; }
        else { const uint32_t kk = 1 + (uint32_t)(rnd() % RK), vv = (uint32_t)(rnd() % RV); if (ref[kk - 1][vv]) { nrp_tab_kill(&t, ref[kk - 1][vv]); ref[kk - 1][vv] = 0; } }
    }
    CHECK(bad == 0);
    printf("table: 200000 random steps against a reference, bad %llu\n", (unsigned long long)bad);
    nrp_tab_free(&t);
}

// ---- benchmark: the per-draw pipeline step of NB-13b (as xbg_nb_pipe.inc look(): dirty groups re-read, 128-byte key, previous-entry
// compare, else hash + find) against NR's (key id from the record + 6 variant words -> row -> first entry). Stream shaped like the phone
// (h1def / g1all / rc1 [nb] pipe lines, campaign): ~150 live entries, 56 % of hits via the previous draw, 0.74 groups re-read a draw
// (0.64 changed to a known id, 0.10 unchanged), no new ids. Group sizes as State's (blend 72, colour 40, polygon 32, depth-stencil
// 56, vertex format 200, misc 20 bytes - a model, not sizeof(State)). The vertex-binding rebuild a hit does is the same for both: left out.
typedef struct { uint8_t blend[72], color[40], poly[32], ds[56], vf[200], misc[20]; } bstate;
static void bench(void) {
    enum { KEYS = 150, PRE = 12, N = 4000000 };
    static const uint16_t off[NBP_GROUPS] = {offsetof(bstate, blend), offsetof(bstate, color), offsetof(bstate, poly), offsetof(bstate, ds), offsetof(bstate, vf), offsetof(bstate, misc)};
    static const uint16_t len[NBP_GROUPS] = {72, 40, 32, 56, 200, 20};
    nbp_span sp[NBP_GROUPS]; memset(sp, 0, sizeof sp);
    for (int k = 0; k < NBP_GROUPS; k++) { sp[k].off[0] = off[k]; sp[k].len[0] = len[k]; }
    nbp_groups *g = (nbp_groups *)calloc(1, sizeof *g); nbp_memo m; CHECK(g && nbp_groups_init(g, sp) == 0 && nbp_memo_init(&m) == 0);
    static uint8_t pre[NBP_GROUPS][PRE][200]; for (int k = 0; k < NBP_GROUPS; k++) for (int p = 0; p < PRE; p++) for (int b = 0; b < len[k]; b++) pre[k][p][b] = (uint8_t)rnd();
    // keys share most state (a key change rewrites ~1.4 groups, as the phone's 0.64 changed groups per draw at 44 % key changes): each
    // group is the common preset 0 with probability 0.88, else one of PRE
    static uint8_t kp[KEYS][NBP_GROUPS]; for (int i = 0; i < KEYS; i++) for (int k = 0; k < NBP_GROUPS; k++) kp[i][k] = (uint8_t)(rnd() % 100 < 88 ? 0 : rnd() % PRE);
    // the stream: key ids with 56 % repeats; per draw the groups that differ from the previous key's are written (dirty), + 0.10 unchanged
    uint16_t *ks = (uint16_t *)malloc(N * sizeof *ks); uint8_t *extra = (uint8_t *)malloc(N);
    uint16_t cur = 0; for (int i = 0; i < N; i++) { if (rnd() % 100 >= 56) cur = (uint16_t)(rnd() % KEYS); ks[i] = cur; extra[i] = (uint8_t)(rnd() % 10 == 0); }
    bstate st; memset(&st, 0, sizeof st); uint8_t *base = (uint8_t *)&st;
    // fill both memos first (the steady state is ~100 % hits)
    nrp_tab t; CHECK(nrp_tab_init(&t) == 0);
    for (int i = 0; i < KEYS; i++) {
        for (int k = 0; k < NBP_GROUPS; k++) memcpy(base + off[k], pre[k][kp[i][k]], len[k]);
        g->dirty = (1u << NBP_GROUPS) - 1; nbp_refresh(g, base);
        nbp_key key; memset(&key, 0, sizeof key); key.w[0] = 0x1234; key.w[1] = (uint64_t)i;   // shaders + per-draw words
        for (int k = 0; k < NBP_GROUPS; k++) key.w[12 + k / 2] |= (uint64_t)g->gid[k] << (32 * (k & 1));
        int cl, fr; nbp_ent *e = nbp_put(&m, &key, nbp_key_hash(&key), &cl, &fr); e->pipe = 1000 + (uint64_t)i;
        nrp_var v = {{0x1234, 0x5678, 0x9abc, 1, 0, 0}}; nrp_tab_put(&t, 1 + (uint32_t)i, &v)->pipe = 1000 + (uint64_t)i;
    }
    uint64_t sink = 0, nb_hits = 0, nr_hits = 0; int prevk = -1; nbp_ent *prev = NULL;
    // baseline: only the state writes the records do (execute(); the same with or without NB-13b) - subtracted from the NB loop
    double t0 = now_ns();
    for (int i = 0; i < N; i++) {
        const int k = ks[i];
        if (k != prevk) { for (int q = 0; q < NBP_GROUPS; q++) if (prevk < 0 || kp[k][q] != kp[prevk][q]) { memcpy(base + off[q], pre[q][kp[k][q]], len[q]); g->dirty |= 1u << q; } }
        if (extra[i]) g->dirty |= 1u << (i % NBP_GROUPS);
        prevk = k; sink += base[i & 255] + g->dirty; g->dirty = 0;
    }
    const double writes = (now_ns() - t0) / N;
    prevk = -1; g->dirty = (1u << NBP_GROUPS) - 1; const uint64_t refresh0 = g->st[NBP_S_REFRESH];
    t0 = now_ns();
    for (int i = 0; i < N; i++) {
        const int k = ks[i];
        if (k != prevk) { for (int q = 0; q < NBP_GROUPS; q++) if (prevk < 0 || kp[k][q] != kp[prevk][q]) { memcpy(base + off[q], pre[q][kp[k][q]], len[q]); g->dirty |= 1u << q; } }
        if (extra[i]) g->dirty |= 1u << (i % NBP_GROUPS);
        prevk = k;
        if (g->dirty) nbp_refresh(g, base);
        nbp_key key; memset(&key, 0, sizeof key); key.w[0] = 0x1234; key.w[1] = (uint64_t)k;
        for (int q = 0; q < NBP_GROUPS; q++) key.w[12 + q / 2] |= (uint64_t)g->gid[q] << (32 * (q & 1));
        nbp_ent *e = prev && !memcmp(&prev->k, &key, sizeof key) ? prev : nbp_find(&m, &key, nbp_key_hash(&key));
        if (e && e->pipe) { nb_hits++; sink += e->pipe; prev = e; }
    }
    const double nb = (now_ns() - t0) / N - writes, reread = (double)(g->st[NBP_S_REFRESH] - refresh0) / N;
    t0 = now_ns();
    for (int i = 0; i < N; i++) {
        const uint32_t id = 1u + ks[i];                      // the record carries the NR-02 key id (NR-09)
        const nrp_var v = {{0x1234, 0x5678, 0x9abc, 1, 0, 0}};   // pass def, pass, layout, samples | colours, restart, variants
        const nrp_tent *e = nrp_tab_find(&t, id, &v);
        if (e) { nr_hits++; sink += e->pipe; }
    }
    const double nr = (now_ns() - t0) / N;
    CHECK(nb_hits == N && nr_hits == N);
    printf("bench (x86 host, rough; stream as the phone's campaign: 150 entries, 56 %% previous-draw hits, %.2f groups re-read a draw; the state writes, %.1f ns a draw, "
           "subtracted): NB-13b step %.1f ns a draw, NR direct table %.1f ns (%.1fx); hits %llu / %llu (sink %llu)\n", reread, writes, nb, nr, nr > 0 ? nb / nr : 0.,
           (unsigned long long)nb_hits, (unsigned long long)nr_hits, (unsigned long long)(sink & 1));
    nbp_groups_free(g); free(g); nbp_memo_free(&m); nrp_tab_free(&t); free(ks); free(extra);
}

int main(void) {
    unit_keys(); unit_sched(); unit_tab();
    mstat a; memset(&a, 0, sizeof a); mstat l; memset(&l, 0, sizeof l); mstat mu; memset(&mu, 0, sizeof mu);
    for (int r = 0; r < 60; r++) model(0, 0, &a);
    for (int r = 0; r < 60; r++) model(1, 0, &l);
    for (int r = 0; r < 20; r++) model(0, 1, &mu);
    printf("model (ordered, 60 runs, %llu records): %llu hints, %llu builds, %llu claimed by draws, %llu needs (%llu met a build in flight); "
           "order bad %llu, double builds %llu, claimed then built %llu, left unbuilt %llu\n", (unsigned long long)a.recs, (unsigned long long)a.hints, (unsigned long long)a.built,
           (unsigned long long)a.claimed, (unsigned long long)a.need, (unsigned long long)a.waited, (unsigned long long)a.order_bad, (unsigned long long)a.dup_build,
           (unsigned long long)a.claim_built, (unsigned long long)a.left_bad);
    printf("model (lazy, 60 runs, %llu records): %llu builds; unhinted built %llu, double builds %llu, claimed then built %llu, hinted left unbuilt %llu\n", (unsigned long long)l.recs,
           (unsigned long long)l.built, (unsigned long long)l.lazy_bad, (unsigned long long)l.dup_build, (unsigned long long)l.claim_built, (unsigned long long)l.left_bad);
    printf("mutation (worker ignores the hint queue, 20 runs): order bad %llu (must be > 0)\n", (unsigned long long)mu.order_bad);
    CHECK(a.order_bad == 0 && a.dup_build == 0 && a.claim_built == 0 && a.left_bad == 0 && a.built > 0 && a.claimed > 0 && a.waited > 0);
    CHECK(l.lazy_bad == 0 && l.dup_build == 0 && l.claim_built == 0 && l.left_bad == 0 && l.built > 0);
    CHECK(mu.order_bad > 0);
    bench();
    if (fails) { printf("FAILED: %d\n", fails); return 1; }
    printf("nr_pipe_test: all passed\n");
    return 0;
}
