// [DOS2] NR-11 host test: the frame rules of dm/gfx/nr/nr_present.h - the code the engine (xbg_nr_present.inc) runs at SetRenderTargets,
// at every pass / clear / read / write of the window texture and at the present.
// - prediction: two alternating window textures, one texture, a present of a third texture;
// - the phone's frame (IR dumps ra1 / rb1 / rc1: pending whole clear, combine pass, empty pass, present) -> direct, the passes and the
//   landed clear go to the swapchain image;
// - every start rule refused alone (no pending clear, written, partial view, crop, flip, query open, each presenter reason);
// - aborts: a read / a write after the start -> one ABORT, nothing substituted afterwards, the present falls back (no copy-back: X is the
//   presented texture); a mispredict -> fallback with a copy-back; crop / flip at the present -> fallback;
// - mode 2: the same scripts never substitute, abort or leave the old present, and count what mode 1 would do (red for broken assumptions);
// - random scripts (both modes): invariants above, the counters add up, one decision a frame.
// Build: clang -std=c11 -Wall -Wextra -I dm/gfx/nr dm/gfx/vulkan/engine/tests/nr_present_test.c -o nr_present_test && ./nr_present_test
#include "nr_present.h"
#include <stdio.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; if (fails < 30) printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static nrp_facts ok_facts(void) { nrp_facts x = {1, 0, 1, 1, 0, 0, NRP_P_OK}; return x; }
static void warm(nrp_frame *f, nrp_pres_stats *s, uint64_t a, uint64_t b) { int cb; nrp_present(f, s, a, 1, 0, &cb); nrp_present(f, s, b, 1, 0, &cb); }

static void test_prediction(void) {
    nrp_frame f; nrp_pres_stats s; memset(&f, 0, sizeof f); memset(&s, 0, sizeof s); int cb;
    CHECK(nrp_predict(&f) == 0);
    nrp_present(&f, &s, 10, 1, 0, &cb); CHECK(nrp_predict(&f) == 0); CHECK(!nrp_wants(&f, 10));
    nrp_present(&f, &s, 20, 1, 0, &cb); CHECK(nrp_predict(&f) == 10);
    nrp_present(&f, &s, 10, 1, 0, &cb); CHECK(nrp_predict(&f) == 20);
    nrp_present(&f, &s, 20, 1, 0, &cb); CHECK(nrp_predict(&f) == 10); CHECK(nrp_wants(&f, 10)); CHECK(!nrp_wants(&f, 20));
    memset(&f, 0, sizeof f);   // one texture
    nrp_present(&f, &s, 7, 1, 0, &cb); nrp_present(&f, &s, 7, 1, 0, &cb); CHECK(nrp_predict(&f) == 7);
    nrp_present(&f, &s, 7, 1, 0, &cb); CHECK(nrp_predict(&f) == 7);
    nrp_present(&f, &s, 9, 1, 0, &cb); CHECK(nrp_predict(&f) == 7);   // a third texture: the next guess is the one before it
    CHECK(s.started == 0 && s.direct == 0);
}

static void test_phone_frame(int mode) {
    nrp_frame f; nrp_pres_stats s; memset(&f, 0, sizeof f); memset(&s, 0, sizeof s); int cb;
    const int would = mode == NRP_MODE_CHECK;
    warm(&f, &s, 0x11822fc000ull, 0x1181870000ull);
    for (int frame = 0; frame < 40; frame++) {
        const uint64_t x = nrp_predict(&f);
        CHECK(x == (frame % 2 ? 0x1181870000ull : 0x11822fc000ull));
        // 3D and post into other targets: nothing for X
        CHECK(nrp_touch(&f, 0x10c944f000ull, NRP_T_PASS) == NRP_A_NONE);
        CHECK(nrp_wants(&f, x));
        nrp_facts ok = ok_facts();
        CHECK(nrp_decide(&f, &s, x, &ok, would) == NRP_R_OK);
        CHECK(!nrp_wants(&f, x));   // one decision a frame
        const int want = would ? NRP_A_NONE : NRP_A_SUBST;
        CHECK(nrp_touch(&f, x, NRP_T_PASS) == want);    // the combine (its clear folded into the load op)
        CHECK(nrp_touch(&f, x, NRP_T_CLEAR) == want);   // or landed before it
        CHECK(nrp_touch(&f, x, NRP_T_PASS) == want);    // the empty pass
        CHECK(nrp_touch(&f, 0x1181000000ull, NRP_T_READ) == NRP_A_NONE);   // other images: untouched
        const int v = nrp_present(&f, &s, x, 1, 0, &cb);
        CHECK(v == (would ? NRP_V_OLD : NRP_V_DIRECT)); CHECK(cb == 0);
    }
    CHECK(s.frames == 42);
    if (would) { CHECK(s.would == 40 && s.would_direct == 40 && s.started == 0 && s.direct == 0 && s.red == 0); }
    else { CHECK(s.started == 40 && s.direct == 40 && s.fallback == 0 && s.would == 0); }
    CHECK(s.passes == 80 && s.clears == 40 && s.why[NRP_R_OK] == 40);
}

static void test_refusals(void) {
    for (int r = 1; r < NRP_R_N; r++) {
        nrp_frame f; nrp_pres_stats s; memset(&f, 0, sizeof f); memset(&s, 0, sizeof s); int cb;
        warm(&f, &s, 1, 2);
        nrp_facts x = ok_facts();
        switch (r) {
        case NRP_R_NO_PREDICTION: continue;   // the engine does not ask without a prediction (nrp_wants)
        case NRP_R_NOT_CLEARED: x.pending_clear = 0; break;
        case NRP_R_WRITTEN: x.written = 1; break;
        case NRP_R_VIEW: x.whole_view = 0; break;
        case NRP_R_CROP: x.crop_full = 0; break;
        case NRP_R_FLIP: x.flip = 1; break;
        case NRP_R_COND: x.cond_gpu = 1; break;
        case NRP_R_PRESENTER: x.presenter = NRP_P_NO_IMAGE; break;
        }
        CHECK(nrp_decide(&f, &s, 1, &x, 0) == r);
        CHECK(s.why[r] == 1 && s.started == 0);
        CHECK(nrp_touch(&f, 1, NRP_T_PASS) == NRP_A_NONE);   // not started: X is X
        CHECK(nrp_present(&f, &s, 1, 1, 0, &cb) == NRP_V_OLD);
        if (r == NRP_R_PRESENTER) CHECK(s.presenter[NRP_P_NO_IMAGE] == 1);
    }
    // pending clear missing but written: counted as written
    { nrp_facts x = ok_facts(); x.pending_clear = 0; x.written = 1; CHECK(nrp_start_reason(&x) == NRP_R_WRITTEN); }
    for (int p = 1; p < NRP_P_N; p++) { nrp_facts x = ok_facts(); x.presenter = p; CHECK(nrp_start_reason(&x) == NRP_R_PRESENTER); CHECK(nrp_presenter_name(p)[0] != '?'); }
    for (int r = 0; r < NRP_R_N; r++) CHECK(nrp_reason_name(r)[0] != '?');
    for (int r = 0; r < NRP_F_N; r++) CHECK(nrp_fallback_name(r)[0] != '?');
    CHECK(nrp_clamp_mode(-1) == 0 && nrp_clamp_mode(4) == 0 && nrp_clamp_mode(3) == 3);
}

static void test_aborts(void) {
    const int kinds[2] = {NRP_T_READ, NRP_T_WRITE};
    for (int k = 0; k < 2; k++) for (int mode = 1; mode <= 2; mode++) {
        nrp_frame f; nrp_pres_stats s; memset(&f, 0, sizeof f); memset(&s, 0, sizeof s); int cb;
        warm(&f, &s, 1, 2);
        nrp_facts x = ok_facts();
        CHECK(nrp_decide(&f, &s, 1, &x, mode == 2) == NRP_R_OK);
        CHECK(nrp_touch(&f, 1, NRP_T_PASS) == (mode == 2 ? NRP_A_NONE : NRP_A_SUBST));
        CHECK(nrp_touch(&f, 1, kinds[k]) == (mode == 2 ? NRP_A_NONE : NRP_A_ABORT));
        CHECK(nrp_touch(&f, 1, kinds[k]) == NRP_A_NONE);   // once
        CHECK(nrp_touch(&f, 1, NRP_T_PASS) == NRP_A_NONE || mode == 2);
        if (mode == 1) CHECK(nrp_touch(&f, 1, NRP_T_CLEAR) == NRP_A_NONE);   // aborted: X's clear lands on X
        const int v = nrp_present(&f, &s, 1, 1, 0, &cb);
        const int fail = kinds[k] == NRP_T_READ ? NRP_F_READ : NRP_F_WRITE;
        if (mode == 1) { CHECK(v == NRP_V_FALLBACK && cb == 0 && s.fallback == 1 && s.fail[fail] == 1 && s.direct == 0); }
        else { CHECK(v == NRP_V_OLD && s.would_fallback == 1 && s.would_fail[fail] == 1 && s.red == 1); }
    }
    for (int mode = 1; mode <= 2; mode++) {   // mispredict
        nrp_frame f; nrp_pres_stats s; memset(&f, 0, sizeof f); memset(&s, 0, sizeof s); int cb;
        warm(&f, &s, 1, 2);
        nrp_facts x = ok_facts(); nrp_decide(&f, &s, 1, &x, mode == 2);
        nrp_touch(&f, 1, NRP_T_PASS);
        const int v = nrp_present(&f, &s, 3, 1, 0, &cb);
        if (mode == 1) { CHECK(v == NRP_V_FALLBACK && cb == 1 && s.fail[NRP_F_MISPREDICT] == 1); }
        else { CHECK(v == NRP_V_OLD && cb == 0 && s.would_fail[NRP_F_MISPREDICT] == 1 && s.red == 1); }
        CHECK(nrp_predict(&f) == 2);
    }
    for (int c = 0; c < 2; c++) {   // crop / flip at the present: fallback, no red (the engine refuses those at the start already)
        nrp_frame f; nrp_pres_stats s; memset(&f, 0, sizeof f); memset(&s, 0, sizeof s); int cb;
        warm(&f, &s, 1, 2);
        nrp_facts x = ok_facts(); nrp_decide(&f, &s, 1, &x, 0);
        const int v = nrp_present(&f, &s, 1, c == 0 ? 0 : 1, c == 1, &cb);
        CHECK(v == NRP_V_FALLBACK && cb == 0 && s.fail[c == 0 ? NRP_F_CROP : NRP_F_FLIP] == 1 && s.red == 0);
    }
}

static uint32_t rng = 12345;
static uint32_t rnd(uint32_t n) { rng = rng * 1103515245u + 12345u; return (rng >> 8) % n; }

static void test_random(void) {
    for (int mode = 1; mode <= 2; mode++) for (int run = 0; run < 2000; run++) {
        nrp_frame f; nrp_pres_stats s; memset(&f, 0, sizeof f); memset(&s, 0, sizeof s); int cb;
        const uint64_t tex[3] = {100, 200, 300};
        uint64_t frames = 0, aborts = 0;
        for (int frame = 0; frame < 30; frame++) {
            const uint64_t x = nrp_predict(&f);
            int started = 0, touched_rw = 0, aborted = 0, decided = 0;
            const int ops = (int)rnd(8);
            for (int o = 0; o < ops + 2; o++) {
                const uint64_t t = tex[rnd(3)];
                const int what = (int)rnd(6);
                if (what == 0 && nrp_wants(&f, t)) {
                    CHECK(t == x && !decided); decided = 1;
                    nrp_facts fx = ok_facts(); if (rnd(4) == 0) fx.pending_clear = 0; if (rnd(8) == 0) fx.presenter = (int)(1 + rnd(NRP_P_N - 1));
                    started = nrp_decide(&f, &s, t, &fx, mode == 2) == NRP_R_OK;
                } else if (what >= 1) {
                    const int kind = what == 1 ? NRP_T_READ : what == 2 ? NRP_T_WRITE : what == 3 ? NRP_T_CLEAR : NRP_T_PASS;
                    const int a = nrp_touch(&f, t, kind);
                    if (mode == 2) CHECK(a == NRP_A_NONE);
                    if (!started || t != x) CHECK(a == NRP_A_NONE);
                    if (a == NRP_A_ABORT) { CHECK(!aborted); aborted = 1; ++aborts; }
                    if (a == NRP_A_SUBST) CHECK(!aborted && (kind == NRP_T_PASS || kind == NRP_T_CLEAR));
                    if (started && t == x && (kind == NRP_T_READ || kind == NRP_T_WRITE)) touched_rw = 1;
                }
            }
            const uint64_t shown = rnd(5) == 0 ? tex[rnd(3)] : (x ? x : tex[rnd(3)]);
            const int v = nrp_present(&f, &s, shown, 1, 0, &cb); ++frames;
            if (mode == 2 || !started) CHECK(v == NRP_V_OLD);
            if (v == NRP_V_DIRECT) CHECK(!touched_rw && shown == x);
            if (cb) CHECK(v == NRP_V_FALLBACK && shown != x && mode == 1);
            CHECK(f.phase == NRP_S_IDLE && f.target == 0 && !f.tried);
        }
        CHECK(s.frames == frames);
        if (mode == 1) { CHECK(s.started == s.direct + s.fallback); CHECK(aborts == s.fail[NRP_F_READ] + s.fail[NRP_F_WRITE]); }
        else { CHECK(s.would == s.would_direct + s.would_fallback && s.started == 0); }
        uint64_t decided = 0; for (int r = 0; r < NRP_R_N; r++) decided += s.why[r];
        CHECK(decided == s.started + s.would + (decided - s.why[NRP_R_OK]));
    }
}

int main(void) {
    test_prediction();
    test_phone_frame(NRP_MODE_ON);
    test_phone_frame(NRP_MODE_CHECK);
    test_refusals();
    test_aborts();
    test_random();
    if (fails) { printf("nr_present_test: %d FAILED\n", fails); return 1; }
    printf("nr_present_test: PASS\n");
    return 0;
}
