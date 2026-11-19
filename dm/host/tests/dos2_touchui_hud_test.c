// [DOS2] Host test of the touch HUD's pure rules (hle_dos2_touchui_hud.h, branch dos2-uimodes): when the host makes the PC HUD, the class scale
// that makes one stage px = DM_DOS2_TOUCH_HUD_SCALE screen px on a 1080-high screen, the side column's button ids, the aimed point of CONFIRM.
// Run with any C compiler: cc dos2_touchui_hud_test.c -lm -o <outside the repo>; run it.
#include "../hle_dos2_touchui_hud.h"
#include <assert.h>
#include <stdio.h>
#ifdef NDEBUG
#error "Build this test with assertions enabled"
#endif
static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }
int main(void) {
    // active: UI mode touch (requested or its HUD part still running), or the old switch; =0 turns it off everywhere
    assert(!hu_active(-1, UI_HYBRID, UI_HYBRID) && !hu_active(-1, UI_SWITCH, UI_SWITCH));
    assert(hu_active(-1, UI_TOUCH, UI_HYBRID) && hu_active(-1, UI_HYBRID, UI_TOUCH) && hu_active(-1, UI_TOUCH, UI_TOUCH));
    assert(hu_active(1, UI_HYBRID, UI_HYBRID) && !hu_active(0, UI_TOUCH, UI_TOUCH));
    // the size: k = c x S x h / D; the host's c cancels S, so k = tune x iPad class scale x h / 1080
    assert(near(hu_fixed_k(1, 1, 1080, 1080), 1) && near(hu_fixed_k(1.3f, 1, 1080, 1080), 1.3f) && hu_fixed_k(1, 1, 1080, 0) == 0);
    for (float s = 0.5f; s < 1.6f; s += 0.1f) {
        assert(near(hu_fixed_k(hu_class_scale(1, 1, s), s, 1080, 1080), 1));
        assert(near(hu_fixed_k(hu_class_scale(1, 1.1f, s), s, 1080, 1080), 1.1f));         // portraits
        assert(near(hu_fixed_k(hu_class_scale(1.2f, 1, s), s, 864, 1080), 1.2f * 0.8f));  // handheld window, tuned up
    }
    assert(near(hu_class_scale(1, 1, 0), 1) && near(hu_class_scale(1, 1, -3), 1) && near(hu_class_scale(1, 1, 99), 1));   // an unusable setting counts as 1
    assert(hu_tune(0, 0) == 1 && hu_tune(0, 1) == 0 && hu_tune(1.25f, 1) == 1.25f && hu_tune(0.1f, 1) == 0.5f && hu_tune(9, 1) == 2);
    // hot bar numbers of tools/dos2_pchud_touch.py at k = 1: pitch 86 (iPad 86), slot 74 x 78 (iPad icon 76 in an 82 cell)
    { const float s = 1.562f, pitch = 58 * 0.9494f * s, w = 50 * 0.9494f * s, h = 50 * s; assert(fabsf(pitch - 86) < 0.2f && fabsf(w - 74.1f) < 0.2f && fabsf(h - 78.1f) < 0.2f); }
    // dmSide: the iPad column's order -> the PC hot bar button ids
    assert(hu_side_panel(1) == -1 && hu_side_panel(0) == -1 && hu_side_panel(8) == -1);
    assert(hu_side_panel(2) == 4 && hu_side_panel(3) == 0 && hu_side_panel(4) == 1 && hu_side_panel(5) == 2 && hu_side_panel(6) == 3 && hu_side_panel(7) == 5);
    { int seen = 0; for (int c = 2; c <= 7; c++) seen |= 1 << hu_side_panel(c); assert(seen == 0x3f); }   // every PC button exactly once
    // the aimed point: where the cursor rested before the finger went to the button
    hu_aim a = {0}; float x, y;
    assert(!hu_aim_point(&a, &x, &y));
    hu_aim_feed(&a, 800, 400, 1000); assert(!hu_aim_point(&a, &x, &y));          // the first position: nothing rested before it
    hu_aim_feed(&a, 810, 405, 1100);                                              // jitter inside the same spot
    hu_aim_feed(&a, 1200, 790, 1500);                                             // the finger lands on CONFIRM
    assert(hu_aim_point(&a, &x, &y) && x == 800 && y == 400);
    hu_aim_feed(&a, 1210, 800, 1600);                                             // it slides a little on the button: still the aimed point
    assert(hu_aim_point(&a, &x, &y) && x == 800 && y == 400);
    hu_aim_feed(&a, 300, 300, 1700); hu_aim_feed(&a, 900, 500, 1710);             // a fast pass (10 ms at 300,300) is not a rest: the button (200 ms) was
    assert(hu_aim_point(&a, &x, &y) && x == 1200 && y == 790);
    hu_aim_set(&a, 640, 360); assert(hu_aim_point(&a, &x, &y) && x == 640 && y == 360);   // the gestures name the point themselves
    // the action plate's pick chain (DM_DOS2_TOUCH_T_PICK)
    assert(hu_pick_parse(NULL) == HU_PICK_AUTO && hu_pick_parse("") == HU_PICK_AUTO && hu_pick_parse("pc") == HU_PICK_PC && hu_pick_parse("hover") == HU_PICK_HOVER);
    assert(hu_pick_parse("hybrid") == HU_PICK_HYBRID && hu_pick_parse("long") == HU_PICK_LONG && hu_pick_parse("x") == HU_PICK_AUTO);
    { hu_pick p = {0}; int died = 0;
      assert(hu_pick_tap(&p, HU_PICK_AUTO, 1, 1, &died) == HU_SRC_PC && hu_pick_tap(&p, HU_PICK_AUTO, 0, 1, &died) == HU_SRC_HOVER && hu_pick_tap(&p, HU_PICK_AUTO, 0, 0, &died) == HU_SRC_NONE);
      assert(hu_pick_tap(&p, HU_PICK_PC, 0, 1, &died) == HU_SRC_NONE && hu_pick_tap(&p, HU_PICK_HOVER, 1, 0, &died) == HU_SRC_NONE && hu_pick_tap(&p, HU_PICK_HOVER, 1, 1, &died) == HU_SRC_HOVER);
      assert(hu_pick_tap(&p, HU_PICK_HYBRID, 0, 1, &died) == HU_SRC_HOVER && hu_pick_tap(&p, HU_PICK_LONG, 1, 1, &died) == HU_SRC_NONE);
      for (int i = 0; i < 200; i++) hu_pick_tap(&p, HU_PICK_AUTO, 0, 0, &died);          // it has worked once: ground taps never kill it
      assert(!p.dead && !hu_pick_long_on(&p, HU_PICK_AUTO) && hu_pick_long_on(&p, HU_PICK_LONG) && !hu_pick_long_on(&p, HU_PICK_HYBRID)); }
    { hu_pick p = {0}; int died = 0, n = 0;                                                 // the helper's ray set-up does not run: dead after 3 taps, said once
      for (int i = 0; i < 10; i++) { hu_pick_tap(&p, HU_PICK_AUTO, 0, -1, &died); n += died; if (i == 1) assert(!p.dead); }
      assert(p.dead && n == 1 && hu_pick_long_on(&p, HU_PICK_AUTO));
      assert(hu_pick_tap(&p, HU_PICK_AUTO, 1, -1, &died) == HU_SRC_PC && !p.dead && !hu_pick_long_on(&p, HU_PICK_AUTO)); }   // a hit revives it
    { hu_pick p = {0}; int died = 0, n = 0;                                                 // it runs but never names an object: dead after 40 world taps
      for (int i = 0; i < HU_PICK_DEAD_TAPS - 1; i++) { hu_pick_tap(&p, HU_PICK_AUTO, 0, 0, &died); n += died; }
      assert(!p.dead && !n);
      hu_pick_tap(&p, HU_PICK_AUTO, 0, 0, &died); assert(p.dead && died);
      hu_pick p2 = {0}; for (int i = 0; i < 500; i++) hu_pick_tap(&p2, HU_PICK_HYBRID, 0, -1, &died);   // hybrid / pc / hover never fall to the long rule
      assert(!p2.dead && !hu_pick_long_on(&p2, HU_PICK_HYBRID)); }
    printf("dos2_touchui_hud_test: ok\n");
    return 0;
}
