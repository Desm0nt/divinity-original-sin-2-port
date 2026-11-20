// [DOS2] Host test of the dmTouch kit rules (hle_dos2_touch_kit.h, docs/dos2/TOUCH_UNIFIED.md 3.4 / 6, step M1): the rect report parser, the
// target top-down with the modal rule (K1), the window rule for the world (K9), the one recogniser with the iPad timings (K3) - the same in
// both UI modes (it has no mode input at all) -, the verb -> event / mouse tables (K2 / K4), the accept guard with the host mark (K7), the
// modal rule for dmAct (K8), the dmSet value coding.
// Run with any C compiler: cc dos2_touch_kit_test.c -o <outside the repo> -lm; run it.
#include "../hle_dos2_touch_kit.h"
#include <assert.h>
#include <stdio.h>
#ifdef NDEBUG
#error "Build this test with assertions enabled"
#endif
static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }
int main(void) {
    // ---- names / ids / dmSet ----
    assert(DK_EV_N == 7 && DK_EV_TAP == 0x7f10 && DK_EV_SET == 0x7f16 && !strcmp(dk_ev_name[DK_EV_SCROLL - DK_EV_TAP], "IE dmScroll"));
    for (int i = 0; i < DK_EV_N; i++) assert(!strncmp(dk_ev_name[i], "IE dm", 5));
    assert(dk_set_key(dk_set_value(DK_SET_ACT, 1)) == 1 && dk_set_num(dk_set_value(DK_SET_ACT, 1)) == 1);
    assert(dk_set_key(dk_set_value(DK_SET_PAD, 64)) == 2 && dk_set_num(dk_set_value(DK_SET_PAD, 64)) == 64);
    assert(dk_set_num(dk_set_value(DK_SET_MODE, 2000000)) == 999999);   // clamped
    {   // the value travels as a float (+0x18 of the event): key 3, number 0..999999 must survive
        const float f = (float)dk_set_value(DK_SET_MODE, 0); assert(dk_set_key(f) == 3 && dk_set_num(f) == 0);
        const float g = (float)dk_set_value(DK_SET_PAD, 120); assert(dk_set_key(g) == 2 && dk_set_num(g) == 120);
    }
    // ---- the report parser: stage -> window-normalised through the player's fit ----
    const int32_t fr[4] = {240, 0, 1920, 1080};   // a 1920x1080 movie centred in a 2400x1080 window
    float k, ox, oy; assert(dk_fit(fr, 1920, 1080, &k, &ox, &oy) && near(k, 1) && near(ox, 240) && near(oy, 0));
    const int32_t bad[4] = {0, 0, 8, 8}; assert(!dk_fit(bad, 1920, 1080, &k, &ox, &oy));
    dk_rect r[16]; float sw, sh; const int P = 1; const void *pl = &P;
    int n = dk_parse("1920,1080|btn1,100,200,80,80,et;bg,0,0,1920,1080,w;sc,10,10,100,400,es;zero,1,1,0,5,e;huge,0,0,1910,1050,e;hold,500,500,50,50,eh", &sw, &sh, r, 16, pl, 1000, 1, 240, 0, 2400, 1080);
    assert(n == 4 && near(sw, 1920) && near(sh, 1080));
    assert(!strcmp(r[0].id, "btn1") && r[0].flags == (DK_F_EL | DK_F_TAP) && near(r[0].x0, 340.f / 2400) && near(r[0].y1, 280.f / 1080));
    assert(!strcmp(r[1].id, "bg") && r[1].flags == DK_F_WIN);                 // a window background may be the whole stage
    assert(!strcmp(r[2].id, "sc") && r[2].flags == (DK_F_EL | DK_F_SCROLL));
    assert(!strcmp(r[3].id, "hold") && r[3].flags == (DK_F_EL | DK_F_HOLD));  // "zero" (no size) and "huge" (a sentinel) skipped
    assert(dk_parse("garbage", &sw, &sh, r, 16, pl, 0, 1, 0, 0, 100, 100) == -1);
    assert(dk_parse("10,10|", &sw, &sh, r, 16, pl, 0, 1, 0, 0, 100, 100) == 0);
    assert(dk_parse("1920,1080|a,1,2,3,4", &sw, &sh, r, 16, pl, 0, 1, 0, 0, 2400, 1080) == 1 && r[0].flags == DK_F_EL);   // no flags = element
    assert(dk_parse("1920,1080|a,1,2,3,4,e;b,5,6,7,8,e", &sw, &sh, r, 1, pl, 0, 1, 0, 0, 2400, 1080) == 1);             // max respected
    // the least hit size grows a small element around its centre
    dk_rect s = {pl, 0, 0.50f, 0.50f, 0.51f, 0.51f, DK_F_EL, "s"};
    assert(!dk_in(&s, 0.52f, 0.505f, 0, 0) && dk_in(&s, 0.52f, 0.505f, 0.05f, 0.05f) && !dk_in(&s, 0.54f, 0.505f, 0.05f, 0.05f));
    // ---- K1: top-down, the first rect hit wins; a modal ends the walk ----
    const int A = 1, B = 2, C = 3; const void *pa = &A, *pb = &B, *pc = &C;
    dk_rect rr[6] = {{pa, 1000, 0.1f, 0.1f, 0.2f, 0.2f, DK_F_EL, "a1"}, {pa, 1000, 0.0f, 0.0f, 0.5f, 0.5f, DK_F_WIN, "awin"},
                     {pb, 1000, 0.15f, 0.15f, 0.3f, 0.3f, DK_F_EL, "b1"}, {pb, 1000, 0.6f, 0.6f, 0.7f, 0.7f, DK_F_EL, "b2"},
                     {pc, 1000, 0.8f, 0.8f, 0.9f, 0.9f, DK_F_EL | DK_F_MODAL, "c1"}, {pb, 10, 0.9f, 0.0f, 1.0f, 0.1f, DK_F_EL, "stale"}};
    dk_obj ob[3] = {{pa, 0x2 | 0x8, 0x8d}, {pb, 0x2 | 0x8, 0x74}, {pc, 0x2, 0x4b}};   // top first; c not visible
    int oi, ri;
    assert(dk_target(ob, 3, rr, 6, 1500, 1500, 0.16f, 0.16f, 0, 0, &oi, &ri) == DK_T_KIT && oi == 0 && ri == 0);   // a's element over b's
    assert(dk_target(ob, 3, rr, 6, 1500, 1500, 0.4f, 0.4f, 0, 0, &oi, &ri) == DK_T_KIT && oi == 0 && ri == 1);     // a's window background
    assert(dk_target(ob, 3, rr, 6, 1500, 1500, 0.65f, 0.65f, 0, 0, &oi, &ri) == DK_T_KIT && oi == 1 && ri == 3);   // b below a
    assert(dk_target(ob, 3, rr, 6, 1600, 1500, 0.95f, 0.05f, 0, 0, &oi, &ri) == DK_T_LEGACY);                      // stale rect: not a target
    assert(dk_target(ob, 3, rr, 6, 1500, 1500, 0.85f, 0.85f, 0, 0, &oi, &ri) == DK_T_LEGACY);                      // c hidden: nothing there
    assert(dk_target(ob, 3, rr, 6, 3000, 1500, 0.16f, 0.16f, 0, 0, &oi, &ri) == DK_T_LEGACY);                      // every rect past its life
    ob[1].flags |= 0x200;   // b is modal: a (above it) still wins its own point, below b nothing
    assert(dk_target(ob, 3, rr, 6, 1500, 1500, 0.16f, 0.16f, 0, 0, &oi, &ri) == DK_T_KIT && oi == 0);
    assert(dk_target(ob, 3, rr, 6, 1500, 1500, 0.65f, 0.65f, 0, 0, &oi, &ri) == DK_T_KIT && oi == 1);
    assert(dk_target(ob, 3, rr, 6, 1500, 1500, 0.9f, 0.5f, 0, 0, &oi, &ri) == DK_T_BLOCK && oi == 1);              // past the modal window: nobody
    ob[2].flags |= 0x8; ob[1].flags &= ~0x200u;   // c visible now, modal by its report flag, below b
    assert(dk_target(ob, 3, rr, 6, 1500, 1500, 0.85f, 0.85f, 0, 0, &oi, &ri) == DK_T_KIT && oi == 2);
    assert(dk_target(ob, 3, rr, 6, 1500, 1500, 0.9f, 0.5f, 0, 0, &oi, &ri) == DK_T_BLOCK && oi == 2);
    {   // an old modal (no rects) above the kit movies: the old path decides
        dk_obj o2[2] = {{pc + 1, 0x2 | 0x8 | 0x400, 0x4b}, {pa, 0x2 | 0x8, 0x8d}};
        assert(dk_target(o2, 2, rr, 6, 1500, 1500, 0.16f, 0.16f, 0, 0, &oi, &ri) == DK_T_LEGACY);
        o2[0].flags &= ~0x400u;   // not modal: the walk goes on to a
        assert(dk_target(o2, 2, rr, 6, 1500, 1500, 0.16f, 0.16f, 0, 0, &oi, &ri) == DK_T_KIT && oi == 1);
        o2[1].flags &= ~0x2u;     // no input bit: skipped
        assert(dk_target(o2, 2, rr, 6, 1500, 1500, 0.16f, 0.16f, 0, 0, &oi, &ri) == DK_T_LEGACY);
    }
    // ---- K9 ----
    assert(dk_window_up(ob, 3, rr, 6, 1500, 1500));        // a's window background
    { dk_obj o3[1] = {{pb, 0xa, 0x74}}; assert(!dk_window_up(o3, 1, rr, 6, 1500, 1500)); }   // b: elements only (HUD)
    { dk_obj o4[1] = {{pa, 0x2, 0x8d}}; assert(!dk_window_up(o4, 1, rr, 6, 1500, 1500)); }   // hidden
    assert(!dk_window_up(ob, 3, rr, 6, 9000, 1500));       // stale
    // ---- K3: one recogniser, iPad timings ----
    const dk_cfg c = dk_cfg_ipad(1080.f);
    assert(c.slong_ms == 350 && c.long_ms == 600 && c.dtap_ms == 300 && near(c.slop, 27.f) && near(c.dtap_r, 54.f));
    dk_rec g; memset(&g, 0, sizeof g); float px;
    dk_rec_down(&g, DK_F_EL, 100, 100, 1000);
    assert(dk_rec_time(&g, &c, 1200) == DK_V_NONE && dk_rec_move(&g, &c, 110, 105, &px) == DK_V_NONE);
    assert(dk_rec_up(&g, &c, 110, 105, 1250) == DK_V_TAP);
    dk_rec_down(&g, DK_F_EL, 120, 110, 1500);   // 250 ms after the release, 22 px away: the double tap
    assert(dk_rec_up(&g, &c, 120, 110, 1550) == DK_V_DTAP);
    dk_rec_down(&g, DK_F_EL, 120, 110, 1700);   // a third tap: a new first half
    assert(dk_rec_up(&g, &c, 120, 110, 1750) == DK_V_TAP);
    dk_rec_down(&g, DK_F_EL, 120, 110, 2100);   // 350 ms later: too late for a double
    assert(dk_rec_up(&g, &c, 120, 110, 2150) == DK_V_TAP);
    dk_rec_down(&g, DK_F_EL, 300, 110, 2300);   // in time but 180 px away: a tap
    assert(dk_rec_up(&g, &c, 300, 110, 2350) == DK_V_TAP);
    // still: short long press at 350, long press at 600, each once; the release then does nothing
    dk_rec_down(&g, DK_F_EL, 100, 100, 5000);
    assert(dk_rec_time(&g, &c, 5349) == DK_V_NONE && dk_rec_time(&g, &c, 5350) == DK_V_SLONG && dk_rec_time(&g, &c, 5400) == DK_V_NONE);
    assert(dk_rec_move(&g, &c, 200, 100, &px) == DK_V_NONE);   // after the tooltip a move does not start a drag
    assert(dk_rec_time(&g, &c, 5600) == DK_V_LONG && dk_rec_time(&g, &c, 6000) == DK_V_NONE && dk_rec_up(&g, &c, 100, 100, 6100) == DK_V_NONE);
    dk_rec_down(&g, DK_F_EL, 100, 100, 7000);   // up between 350 and 600: a tooltip only, no tap
    assert(dk_rec_time(&g, &c, 7400) == DK_V_SLONG && dk_rec_up(&g, &c, 100, 100, 7450) == DK_V_NONE);
    // a hold element: the long press is the hold, down until the release
    dk_rec_down(&g, DK_F_EL | DK_F_HOLD, 100, 100, 8000);
    assert(dk_rec_time(&g, &c, 8350) == DK_V_SLONG && dk_rec_time(&g, &c, 8600) == DK_V_HOLD_DOWN && dk_rec_time(&g, &c, 9000) == DK_V_NONE);
    assert(dk_rec_up(&g, &c, 100, 100, 9500) == DK_V_HOLD_UP);
    // a drag: past the slop before the short long press, once; the release ends it
    dk_rec_down(&g, DK_F_EL | DK_F_DRAG, 100, 100, 10000);
    assert(dk_rec_move(&g, &c, 120, 100, &px) == DK_V_NONE && dk_rec_move(&g, &c, 140, 100, &px) == DK_V_DRAG_START && dk_rec_move(&g, &c, 200, 100, &px) == DK_V_NONE);
    assert(dk_rec_time(&g, &c, 10700) == DK_V_NONE && dk_rec_up(&g, &c, 200, 100, 10800) == DK_V_DRAG_END);
    // a scroll element: the first step past the slop is a scroll by the whole motion, then steps of scroll_step px
    dk_rec_down(&g, DK_F_EL | DK_F_SCROLL, 100, 100, 11000);
    assert(dk_rec_move(&g, &c, 100, 130, &px) == DK_V_SCROLL && near(px, 30));
    assert(dk_rec_move(&g, &c, 100, 132, &px) == DK_V_NONE && dk_rec_move(&g, &c, 100, 135, &px) == DK_V_SCROLL && near(px, 5));
    assert(dk_rec_move(&g, &c, 100, 120, &px) == DK_V_SCROLL && near(px, -15));
    assert(dk_rec_time(&g, &c, 12000) == DK_V_NONE && dk_rec_up(&g, &c, 100, 120, 12100) == DK_V_NONE);
    // the recogniser has no mode: the same finger in "hybrid" and "touch" is the same verb (one rule set: nothing to switch)
    // ---- K2 / K4 tables ----
    assert(dk_verb_event(DK_V_TAP) == DK_EV_TAP && dk_verb_event(DK_V_DTAP) == DK_EV_DTAP && dk_verb_event(DK_V_SLONG) == DK_EV_SLONG && dk_verb_event(DK_V_LONG) == DK_EV_LONG);
    assert(dk_verb_event(DK_V_HOLD_DOWN) == DK_EV_HOLD && dk_verb_event(DK_V_HOLD_UP) == DK_EV_HOLD && dk_verb_event(DK_V_SCROLL) == DK_EV_SCROLL);
    assert(!dk_verb_event(DK_V_DRAG_START) && !dk_verb_event(DK_V_DRAG_END) && !dk_verb_event(DK_V_NONE));
    assert(dk_verb_mouse(DK_V_TAP, DK_F_EL) == DK_DO_MOVE);                                         // the kit's own element: no click
    assert(dk_verb_mouse(DK_V_TAP, DK_F_EL | DK_F_TAP) == (DK_DO_MOVE | DK_DO_PRESS | DK_DO_RELEASE)); // native: the click too
    assert(dk_verb_mouse(DK_V_DRAG_END, DK_F_EL | DK_F_DRAG) == (DK_DO_MOVE | DK_DO_RELEASE) && !dk_verb_mouse(DK_V_DRAG_END, DK_F_EL));
    assert(!dk_verb_mouse(DK_V_LONG, DK_F_EL | DK_F_TAP));                                          // no right click any more (Р9)
    // ---- K7 ----
    assert(dk_accept_drop(1500, 1000, 0, 0, 600));          // 500 ms after the touch press, no A, no host pulse: dropped
    assert(!dk_accept_drop(1700, 1000, 0, 0, 600));         // past the window
    assert(!dk_accept_drop(1500, 1000, 1400, 0, 600));      // the pad's A was down 100 ms ago
    assert(!dk_accept_drop(1500, 1000, 0, 1300, 600));      // the host pulsed a key / the pad 200 ms ago ("from the host")
    assert(dk_accept_drop(1500, 1000, 0, 1100, 600));       // that pulse is 400 ms old: not this accept
    assert(!dk_accept_drop(1500, 0, 0, 0, 600));            // no touch press at all
    // ---- K8 ----
    const int T = 7, U = 8;
    assert(dk_may_act(&T, &T, 1000, 900, 2000) && !dk_may_act(&U, &T, 1000, 900, 2000));
    assert(dk_may_act(&U, &T, 5000, 900, 2000) && dk_may_act(&U, NULL, 1000, 900, 2000));
    assert(DK_A_N == 7 && !strcmp(dk_act_name[DK_A_PANEL], "panel") && !strcmp(dk_act_name[DK_A_MENU], "menu"));
    printf("dos2_touch_kit_test: all passed\n");
    return 0;
}
