// [DOS2] NR-03 DM_NR_OCC host test (dm/gfx/nr/nr_occ.h): the Switch's predicate, the pair state machine the backend drives (reports,
// draws, pass ends, flushes, lookups), the reasons a pair becomes unusable, the key history and the prev-path decision, the guards, and a
// model of the game's frame (an occlusion pair per object in the pre-pass, conditional draws later in the frame, 2 submissions in flight)
// that measures what the prev path costs in false culls against what the GPU path does exactly.
// Build + run (from this directory):  clang -std=c11 -O2 -Wall -Wextra nr_occ_test.c -o nr_occ_test && ./nr_occ_test
#include "../../../nr/nr_occ.h"
#include <stdio.h>
#include <stdlib.h>

static int fails, checks;
#define CHECK(x) do { ++checks; if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

static nro_slot *S;
static nro_hist *H;

// one draw inside the backend: enter, query, done (as draw() does when nothing skips it); key bits as the engine mixes them
static void draw(nro_slot *s, uint64_t mesh, uint64_t ubo) {
    nro_pair *p = nro_draw_enter(s);
    if (p && !p->inv) { p->mesh = nro_mix(p->mesh, mesh); p->ubo = nro_mix(p->ubo, ubo); }
    int32_t q; int r = nro_draw_query(s, &q);
    (void)r;
    nro_draw_done(s);
}
static int report(nro_slot *s, uint64_t a, int32_t *eq, int32_t *closed) { return nro_report(s, a, eq, closed); }

static void test_basics(void) {
    CHECK(nro_truth_render(NRO_NVN_IF_NOT_EQUAL, 0) == 0 && nro_truth_render(NRO_NVN_IF_NOT_EQUAL, 3) == 1);
    CHECK(nro_truth_render(NRO_NVN_IF_EQUAL, 0) == 1 && nro_truth_render(NRO_NVN_IF_EQUAL, 3) == 0);
    CHECK(nro_truth_render(7, 0) == 1);
    CHECK(nro_clamp_mode(-1) == NRO_OFF && nro_clamp_mode(4) == NRO_OFF && nro_clamp_mode(2) == NRO_CHECK && nro_clamp_mode(3) == NRO_POISON);
    CHECK(nro_clamp_path(1) == NRO_PATH_GPU && nro_clamp_path(2) == NRO_PATH_PREV && nro_clamp_path(9) == NRO_PATH_AUTO);
    CHECK(nro_mix(0, 1) != nro_mix(0, 2) && nro_hash_bytes(0, "abcdefghi", 9) != nro_hash_bytes(0, "abcdefghj", 9));
    CHECK(nro_hash_bytes(5, "", 0) == 5);
}

static void test_pair(void) {
    nro_slot_reset(S);
    int32_t eq, cl, q;
    // a pair with one draw, closed inside the pass: the closing report ends the query
    CHECK(report(S, 0x1000, &eq, &cl) == NRO_R_OPENED && S->open == 0 && cl == -1);
    CHECK(nro_draw_enter(S) != 0);
    CHECK(nro_draw_query(S, &q) == 1 && q == 0 && S->qused == 1 && S->qactive == 0);
    nro_draw_done(S);
    nro_draw_enter(S); CHECK(nro_draw_query(S, &q) == 2 && q == 0); nro_draw_done(S);   // second draw, same query
    int r = report(S, 0x1010, &eq, &cl);
    CHECK((r & NRO_R_CLOSED) && (r & NRO_R_END_QUERY) && eq == 0 && cl == 0 && S->open == -1 && S->qactive == -1);
    CHECK(S->pair[0].inv == 0 && S->pair[0].seen == 2 && S->pair[0].done == 2 && S->pair[0].st == NRO_PS_CLOSED);
    CHECK(nro_lookup(S, 0x1000) == 0 && nro_lookup(S, 0x1010) == -1);
    CHECK(!nro_copy_ready(S, &S->pair[0]));                // same pass: the copy has not run
    CHECK(nro_pass_end(S) == -1 && S->pass_serial == 1);  // no query active any more
    CHECK(nro_copy_ready(S, &S->pair[0]));
    uint32_t lo, n;
    CHECK(nro_flush_range(S, &lo, &n) == 1 && lo == 0 && n == 1 && (S->pair[0].flags & NRO_PF_COPIED));
    CHECK(nro_flush_range(S, &lo, &n) == 0);
    // an empty pair: no query at all, zero samples on the Switch
    report(S, 0x2000, &eq, &cl); r = report(S, 0x2010, &eq, &cl);
    CHECK((r & NRO_R_CLOSED) && !(r & NRO_R_END_QUERY) && cl == 1 && !(S->pair[1].flags & NRO_PF_BEGUN) && S->pair[1].inv == 0);
    // the query ends with a pass inside the pair, no draw after it: still usable, copied by that pass's flush
    report(S, 0x3000, &eq, &cl); draw(S, 1, 1);
    CHECK(nro_pass_end(S) == 1 && (S->pair[2].flags & NRO_PF_ENDED));
    CHECK(nro_flush_range(S, &lo, &n) == 1 && lo == 1 && n == 1 && (S->pair[2].flags & NRO_PF_COPIED));
    r = report(S, 0x3010, &eq, &cl);
    CHECK((r & NRO_R_CLOSED) && !(r & NRO_R_END_QUERY) && S->pair[2].inv == 0 && nro_copy_ready(S, &S->pair[2]));
    // a draw after the pass end inside the same pair: split, unusable
    report(S, 0x4000, &eq, &cl); draw(S, 1, 1); nro_pass_end(S);
    nro_draw_enter(S); CHECK(nro_draw_query(S, &q) == 0); nro_draw_done(S);
    report(S, 0x4010, &eq, &cl); CHECK(S->pair[3].inv == NRO_INV_SPLIT);
    // a draw the backend left out (entered, never made): unusable
    report(S, 0x5000, &eq, &cl); nro_draw_enter(S); report(S, 0x5010, &eq, &cl);
    CHECK(S->pair[4].inv == NRO_INV_SKIPPED);
    // another report inside an open pair: that pair is unusable, the new report opens the next one
    report(S, 0x6000, &eq, &cl); draw(S, 1, 1);
    r = report(S, 0x7000, &eq, &cl);
    CHECK((r & NRO_R_NEST) && (r & NRO_R_CLOSED) && (r & NRO_R_OPENED) && (r & NRO_R_END_QUERY) && cl == 5 && S->pair[5].inv == NRO_INV_NEST && S->open == 6);
    report(S, 0x7010, &eq, &cl); CHECK(cl == 6 && S->pair[6].inv == 0);
    // the latest pair for an address wins
    report(S, 0x1000, &eq, &cl); report(S, 0x1010, &eq, &cl); CHECK(nro_lookup(S, 0x1000) == 7);
    // a new submission forgets every address (generation)
    nro_slot_reset(S); CHECK(nro_lookup(S, 0x1000) == -1 && S->npairs == 0 && S->qused == 0);
    // pool full
    S->qused = NRO_MAX_Q; report(S, 0x8000, &eq, &cl); nro_draw_enter(S); CHECK(nro_draw_query(S, &q) == 0 && S->pair[0].inv == NRO_INV_POOL);
    nro_slot_reset(S);
    // repeated meshes: occurrence index
    for (int i = 0; i < 3; i++) { report(S, 0x9000 + 32 * i, &eq, &cl); draw(S, 77, (uint64_t)i); report(S, 0x9010 + 32 * i, &eq, &cl); }
    CHECK(S->pair[0].mesh == S->pair[2].mesh && S->pair[0].occ == 0 && S->pair[1].occ == 1 && S->pair[2].occ == 2);
    CHECK(S->pair[0].ubo != S->pair[1].ubo);
    // table full: no pair, the condition will find none (renders)
    nro_slot_reset(S); S->npairs = NRO_MAX_PAIRS; CHECK(report(S, 0xa000, &eq, &cl) & NRO_R_FULL); CHECK(nro_lookup(S, 0xa000) == -1);
    nro_slot_reset(S);
}

static void test_history(void) {
    memset(H, 0, sizeof *H);
    CHECK(nro_prev_cull(H, 1, 1, 0, 2, 1) == 0);   // nothing retired yet
    for (int f = 0; f < 3; f++) { nro_hist_begin(H); nro_hist_put(H, 1, 10, 1); nro_hist_put(H, 2, 20, 0); }
    const nro_hent *e = nro_hist_get(H, 1);
    CHECK(e && e->streak == 3 && e->count == 1 && e->last == 3);
    CHECK(nro_hist_get(H, 2)->streak == 0);
    CHECK(nro_prev_cull(H, 1, 10, 0, 2, 1) == 1);   // hidden 3 frames, constants equal
    CHECK(nro_prev_cull(H, 1, 10, 0, 4, 1) == 0);   // needs 4
    CHECK(nro_prev_cull(H, 1, 11, 0, 2, 1) == 0);   // moved (or the camera did): strict renders
    CHECK(nro_prev_cull(H, 1, 11, 0, 2, 0) == 1);   // loose culls
    CHECK(nro_prev_cull(H, 2, 20, 0, 1, 0) == 0);   // visible
    CHECK(nro_prev_cull(H, 1, 10, 1, 2, 0) == 0);   // second occurrence in this submission, no strict key: render
    CHECK(nro_prev_cull(H, 3, 10, 0, 1, 0) == 0);   // unknown key
    // missing for one frame: the key is not in the last retired frame -> render, and the streak restarts
    nro_hist_begin(H); nro_hist_put(H, 2, 20, 0);
    CHECK(nro_prev_cull(H, 1, 10, 0, 1, 0) == 0);
    nro_hist_begin(H); nro_hist_put(H, 1, 10, 1); CHECK(nro_hist_get(H, 1)->streak == 1);
    // visible once resets the streak
    nro_hist_begin(H); nro_hist_put(H, 1, 10, 0); CHECK(nro_hist_get(H, 1)->streak == 0 && nro_prev_cull(H, 1, 10, 0, 1, 0) == 0);
    // a mesh repeated in a frame: mesh key count 2, judged by the strict key
    memset(H, 0, sizeof *H);
    for (int f = 0; f < 3; f++) {
        nro_hist_begin(H); nro_hist_put(H, 5, 100, 1); nro_hist_put(H, 5, 200, 0);
        nro_hist_put(H, nro_strict_key(5, 100), 100, 1); nro_hist_put(H, nro_strict_key(5, 200), 200, 0);
    }
    CHECK(nro_hist_get(H, 5)->count == 2 && nro_hist_get(H, 5)->streak == 0);
    CHECK(nro_prev_cull(H, 5, 100, 0, 2, 0) == 1);   // the hidden instance by its exact constants
    CHECK(nro_prev_cull(H, 5, 200, 1, 2, 0) == 0);   // the visible one
    CHECK(nro_prev_cull(H, 5, 300, 0, 1, 0) == 0);   // an instance with constants never seen
    // rebuild keeps recent keys, drops stale ones
    memset(H, 0, sizeof *H);
    for (int f = 0; f < 40; f++) { nro_hist_begin(H); for (int i = 0; i < 400; i++) nro_hist_put(H, (uint64_t)f * 1000 + (uint64_t)i, 0, 1); }
    printf("history: rebuilds %u keys %u lost %u\n", H->rebuilds, H->n, H->lost);
    CHECK(H->rebuilds >= 1 && H->n < NRO_HIST / 2 + 400 && H->lost == 0);
    CHECK(nro_hist_get(H, 39u * 1000 + 5) != 0 && nro_hist_get(H, 0u * 1000 + 5) == 0);
}

static void test_guards(void) {
    CHECK(!nro_guard_scene(0, 15) && nro_guard_scene(11, 16) && !nro_guard_scene(12, 16) && nro_guard_scene(0, 100));
    CHECK(!nro_guard_reveals(4, 10) && nro_guard_reveals(5, 200) && !nro_guard_reveals(5, 300));
}

// Model of the game: K objects, each with an occlusion pair in the pre-pass (pass 0) and one conditional draw in the G-buffer pass (pass
// 1) - the 0xb89330 / 0xb23970 pattern. Visibility per object changes over frames (a door opens: a block of objects is revealed at once;
// a camera pan: a few at a time). The truth of frame t is known at its retire, 2 submissions later (the ring). The GPU path culls exactly
// the hidden ones (its copy runs at the end of pass 0); the prev path decides from history at record time.
typedef struct { unsigned culls_truth, gpu, strict_ok, strict_bad, loose_ok, loose_bad, addr_ok, addr_bad, conds; } model_out;
static int visible(int obj, int t, int scenario) {
    if (scenario == 0) return obj % 3 == 0;                         // static scene
    if (scenario == 1) return obj % 3 == 0 || (t >= 30 && obj < 20); // at t = 30 a door opens: 20 objects revealed at once
    return obj % 3 == 0 || ((obj + t / 4) % 7 == 0);               // slow pan: a few change every 4 frames
}
static model_out run_model(int scenario, unsigned need, int moving_camera, int shuffle) {
    enum { K = 120, T = 80, RING = 2 };
    static nro_slot slots[RING];
    static uint8_t truth[RING][K];
    model_out o; memset(&o, 0, sizeof o);
    memset(H, 0, sizeof *H);
    for (int k = 0; k < RING; k++) nro_slot_reset(&slots[k]);
    for (int t = 0; t < T; t++) {
        nro_slot *s = &slots[t % RING];
        if (t >= RING) {   // retire the submission of frame t - RING: fold its truth into the history
            nro_hist_begin(H);
            for (uint32_t i = 0; i < s->npairs; i++) { nro_hist_put(H, s->pair[i].mesh, s->pair[i].ubo, !truth[t % RING][i]); nro_addr_put(H, s->pair[i].addr, !truth[t % RING][i]); }
        }
        nro_slot_reset(s);
        int32_t eq, cl;
        for (int obj = 0; obj < K; obj++) {   // pass 0: the pairs
            report(s, 0x10000 + 32u * (uint32_t)(shuffle ? (obj * 7 + t) % K : obj), &eq, &cl);   // a different address every frame (allocation order)
            draw(s, 1000u + (unsigned)obj, moving_camera ? (uint64_t)t : 0);
            report(s, 0x10010 + 32u * (uint32_t)(shuffle ? (obj * 7 + t) % K : obj), &eq, &cl);
            truth[t % RING][obj] = (uint8_t)visible(obj, t, scenario);
        }
        nro_pass_end(s);
        for (int obj = 0; obj < K; obj++) {   // pass 1: the conditions
            const int32_t i = nro_lookup(s, 0x10000 + 32u * (uint32_t)(shuffle ? (obj * 7 + t) % K : obj));
            if (i < 0) continue;
            const nro_pair *p = &s->pair[i];
            ++o.conds;
            const int hidden = !truth[t % RING][obj];
            if (hidden) ++o.culls_truth;
            if (hidden && nro_copy_ready(s, p)) ++o.gpu;
            if (nro_prev_cull(H, p->mesh, p->ubo, p->occ, need, 1)) { if (hidden) ++o.strict_ok; else ++o.strict_bad; }
            if (nro_prev_cull(H, p->mesh, p->ubo, p->occ, need, 0)) { if (hidden) ++o.loose_ok; else ++o.loose_bad; }
            if (nro_addr_cull(H, p->addr)) { if (hidden) ++o.addr_ok; else ++o.addr_bad; }
        }
        nro_pass_end(s);
    }
    return o;
}
static void test_model(void) {
    model_out a = run_model(0, 2, 0, 1);
    printf("model static:      conds %u, Switch culls %u, gpu %u, prev strict %u ok / %u wrong, loose %u / %u\n", a.conds, a.culls_truth, a.gpu, a.strict_ok, a.strict_bad, a.loose_ok, a.loose_bad);
    CHECK(a.gpu == a.culls_truth && a.strict_bad == 0 && a.loose_bad == 0 && a.strict_ok > a.culls_truth * 9 / 10);
    model_out b = run_model(1, 2, 0, 1);
    printf("model door opens:  conds %u, Switch culls %u, gpu %u, prev strict %u ok / %u wrong, loose %u / %u\n", b.conds, b.culls_truth, b.gpu, b.strict_ok, b.strict_bad, b.loose_ok, b.loose_bad);
    CHECK(b.gpu == b.culls_truth);
    CHECK(b.strict_bad > 0 && b.strict_bad <= 20 * 2);   // the revealed block is culled for the ring latency (2 frames): the pop-in risk
    model_out c = run_model(2, 2, 0, 1);
    printf("model slow pan:    conds %u, Switch culls %u, gpu %u, prev strict %u ok / %u wrong, loose %u / %u\n", c.conds, c.culls_truth, c.gpu, c.strict_ok, c.strict_bad, c.loose_ok, c.loose_bad);
    model_out c4 = run_model(2, 4, 0, 1);
    printf("model slow pan N4: conds %u, Switch culls %u, gpu %u, prev strict %u ok / %u wrong\n", c4.conds, c4.culls_truth, c4.gpu, c4.strict_ok, c4.strict_bad);
    CHECK(c.gpu == c.culls_truth && c4.strict_bad <= c.strict_bad && c4.strict_ok <= c.strict_ok);
    model_out d = run_model(2, 2, 1, 1);
    printf("model moving cam:  conds %u, Switch culls %u, gpu %u, prev strict %u ok / %u wrong, loose %u / %u\n", d.conds, d.culls_truth, d.gpu, d.strict_ok, d.strict_bad, d.loose_ok, d.loose_bad);
    CHECK(d.strict_ok == 0 && d.strict_bad == 0 && d.gpu == d.culls_truth);   // constants change every frame: strict never culls
    // judged by address (DM_GFX_COND_RENDER=3): fine while every object keeps its allocation index, wrong once the order changes
    model_out e = run_model(0, 2, 0, 0), f = run_model(0, 2, 0, 1);
    printf("model by address:  stable indices %u ok / %u wrong; indices reshuffled every frame %u ok / %u wrong (Switch culls %u)\n", e.addr_ok, e.addr_bad, f.addr_ok, f.addr_bad, f.culls_truth);
    CHECK(e.addr_bad == 0 && e.addr_ok > 0 && f.addr_bad > f.culls_truth / 4);
}

int main(void) {
    S = calloc(1, sizeof *S); H = calloc(1, sizeof *H);
    if (!S || !H) return 2;
    test_basics(); test_pair(); test_history(); test_guards(); test_model();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    free(S); free(H);
    return fails != 0;
}
