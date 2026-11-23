// [DOS2] Host test of the touch HUD's pure rules (hle_dos2_touchui_hud.h, branch dos2-uimodes): when the host makes the PC HUD, the class scale
// that makes one stage px = DM_DOS2_TOUCH_HUD_SCALE screen px on a 1080-high screen, the side column's button ids, the aimed point of CONFIRM.
// Run with any C compiler: cc dos2_touchui_hud_test.c -lm -o <outside the repo>; run it.
#include "../hle_dos2_touchui_hud.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
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
    // run 21.11: hot bar id 0 is the GAME MENU (the armour icon opened the pause menu); the sheet and the inventory share id 1 (one engine toggle)
    assert(hu_side_panel(2) == 4 && hu_side_panel(3) == 1 && hu_side_panel(4) == 1 && hu_side_panel(5) == 2 && hu_side_panel(6) == 3 && hu_side_panel(7) == 5);
    { int seen = 0; for (int c = 2; c <= 7; c++) seen |= 1 << hu_side_panel(c); assert(seen == 0x3e); }   // every panel button, never the game menu (0)
    {   // the pair's engine ops: one toggle (opens both unless both are up, then closes both) + a hide per half
        const int S = HU_P_SHEET, I = HU_P_INV, B = S | I; int pr, hd;
        hu_pair_ops(0, I, &pr, &hd); assert(pr == 1 && hd == S);        // the bag: both open, the sheet goes
        hu_pair_ops(0, S, &pr, &hd); assert(pr == 1 && hd == I);        // the armour: both open, the inventory goes
        hu_pair_ops(0, B, &pr, &hd); assert(pr == 1 && hd == 0);        // the equipment view
        hu_pair_ops(I, B, &pr, &hd); assert(pr == 1 && hd == 0);        // the doll button from the inventory: the toggle opens both (not both were up)
        hu_pair_ops(I, S, &pr, &hd); assert(pr == 1 && hd == I);
        hu_pair_ops(I, 0, &pr, &hd); assert(pr == 0 && hd == I);        // the inventory alone closes by its hide (the toggle would OPEN both)
        hu_pair_ops(B, 0, &pr, &hd); assert(pr == 1 && hd == 0);        // both up: the PC's own close
        hu_pair_ops(B, I, &pr, &hd); assert(pr == 0 && hd == S);        // the equipment view back to the plain inventory
        hu_pair_ops(I, I | HU_P_SKILLS, &pr, &hd); assert(pr == 0 && hd == 0);   // other bits are not the pair's
    }
    {   // run 21.11: the corner of an open window closes it (2400 x 1080: sw 2400 design px; cell 93)
        const float sw = 2400;
        assert(hu_corner_code(HU_P_JOURNAL, 0, 1200 + 598 - 40, 40, sw, 93) == 7 && hu_corner_code(HU_P_JOURNAL, 0, 1200 + 598 - 200, 40, sw, 93) == 0);
        assert(hu_corner_code(HU_P_SKILLS, 0, 1200 + 605, 60, sw, 93) == 5 && hu_corner_code(HU_P_SKILLS, 0, 1200 + 605, 300, sw, 93) == 0);
        assert(hu_corner_code(HU_P_INV, 0, 1200 + 716 - 30, 30, sw, 93) == 4);                 // the inventory: 770 x 1.86 = 1432 wide
        assert(hu_corner_code(HU_P_INV, 0, 1200 - 716 + 30, 30, sw, 93) == 0);                 // its LEFT corner is not a close
        assert(hu_corner_code(HU_P_SHEET | HU_P_INV, HU_VIEW_EQUIP, 1200 + 920, 20, sw, 93) == 8);
        assert(hu_corner_code(0, 0, 2300, 20, sw, 93) == 0);
    }
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
    // UI mode TOUCH, prepared (owner 23.11): any source aims; the host shows CONFIRM / cancel only where the hot bar's slot mark does not
    assert(!hu_aim_any(0, 0, 0) && hu_aim_any(1, 0, 0) && hu_aim_any(0, 1, 0) && hu_aim_any(0, 0, 1));
    assert(!hu_aim_host_show(0, 0, 0) && !hu_aim_host_show(1, 0, 1) && !hu_aim_host_show(1, 1, 0));
    assert(hu_aim_host_show(0, 1, 0) && hu_aim_host_show(0, 0, 1));   // attack ground / a skill from the book / a throw: the host shows them
    assert(HU_AIM_SHOW < 1999999 && HU_AIM_HIDE < HU_AIM_SHOW && HU_AIM_HIDE > 1000000);   // below the plate's hide (tools/dos2_pchud_touch.py)
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
    // one window at a time + the equipment view (dmSide 8)
    { int c, o, pr, st;
      assert(hu_view_plan(4, 0, 0, &c, &o, &pr, &st) == 0 && c == 0 && o == HU_P_INV && !pr && st == 0);                        // nothing open: the inventory opens, plain
      assert(hu_view_plan(3, HU_P_INV, 0, &c, &o, &pr, &st) == 0 && c == HU_P_INV && o == HU_P_SHEET && st == 0);               // the sheet replaces the inventory
      assert(hu_view_plan(5, HU_P_SHEET | HU_P_CRAFT, 0, &c, &o, &pr, &st) == 0 && c == (HU_P_SHEET | HU_P_CRAFT) && o == HU_P_SKILLS && st == -1);
      assert(hu_view_plan(4, HU_P_INV, 0, &c, &o, &pr, &st) == 0 && c == HU_P_INV && o == 0 && st == 0);                        // the same button again closes it
      assert(hu_view_plan(8, HU_P_INV, 0, &c, &o, &pr, &st) == HU_VIEW_EQUIP && c == 0 && o == HU_P_SHEET && st == 1);          // the doll button of the inventory
      assert(hu_view_plan(8, HU_P_SKILLS, 0, &c, &o, &pr, &st) == HU_VIEW_EQUIP && c == HU_P_SKILLS && o == (HU_P_SHEET | HU_P_INV) && st == 1);
      assert(hu_view_plan(8, HU_P_SHEET | HU_P_INV, HU_VIEW_EQUIP, &c, &o, &pr, &st) == 0 && c == (HU_P_SHEET | HU_P_INV) && o == 0 && st == 0);   // the pair closes as one
      assert(hu_view_plan(4, HU_P_SHEET | HU_P_INV, HU_VIEW_EQUIP, &c, &o, &pr, &st) == 0 && c == HU_P_SHEET && o == 0 && st == 0);                // back to the plain inventory
      assert(hu_view_plan(3, HU_P_SHEET | HU_P_INV, HU_VIEW_EQUIP, &c, &o, &pr, &st) == 0 && c == HU_P_INV && o == 0 && st == 0);                  // back to the plain sheet
      assert(hu_view_plan(8, HU_P_SHEET | HU_P_INV, 0, &c, &o, &pr, &st) == HU_VIEW_EQUIP && c == 0 && o == 0 && st == 1);                         // both were up some other way: only the look
      assert(hu_view_plan(7, HU_P_INV, 0, &c, &o, &pr, &st) == 0 && c == HU_P_INV && o == 0 && pr == 1 && st == -1);            // journal / map: the engine's own toggle
      assert(hu_view_plan(2, HU_P_JOURNAL, 0, &c, &o, &pr, &st) == 0 && c == 0 && pr == 1);
      assert(hu_view_plan(1, HU_P_INV, HU_VIEW_EQUIP, &c, &o, &pr, &st) == HU_VIEW_EQUIP && !c && !o && !pr && st == -1);       // not a panel code
      for (int b = 1; b <= HU_P_JOURNAL; b <<= 1) assert(hu_panel_pc(b) >= 0 && hu_panel_pc(b) <= 5);
      assert(hu_panel_pc(HU_P_SHEET) == hu_side_panel(3) && hu_panel_pc(HU_P_INV) == hu_side_panel(4) && hu_panel_pc(HU_P_SKILLS) == hu_side_panel(5) &&
             hu_panel_pc(HU_P_CRAFT) == hu_side_panel(6) && hu_panel_pc(HU_P_JOURNAL) == hu_side_panel(7));
      assert(!hu_pair_broken(HU_VIEW_EQUIP, HU_P_SHEET | HU_P_INV, 5000) && !hu_pair_broken(HU_VIEW_EQUIP, HU_P_INV, 100) && hu_pair_broken(HU_VIEW_EQUIP, HU_P_INV, 400));
      assert(hu_pair_broken(HU_VIEW_EQUIP, 0, 1000) && !hu_pair_broken(0, HU_P_INV, 1000)); }
    {   // the PC tooltip window (phone run 23.11, 6.7" 2400 x 1080, window factor 1.3): the movie's size and the engine's placement keep it on the screen
        const hu_tip_fit t = hu_tip_numbers(1.3f, 1.0f, 2400, 1080);
        assert(near(t.f, 1.3f) && t.max_h == 1080 - 158 - 16 && t.max_w == 2400 - 160);
        double v[3]; hu_tip_cmds(&t, v);
        assert(v[0] == 1001300 && v[1] == 2000000 + 906 && v[2] == 3000000 + 2240);
        { const hu_tip_fit u = hu_tip_numbers(1.3f, 0.9f, 2400, 1080); assert(near(u.f, 1.3f / 0.9f) && u.max_h == (int)floorf(1080.0f / 0.9f - 174)); }   // S 0.9
        { const hu_tip_fit u = hu_tip_numbers(1.3f, 0, 0, 0); assert(near(u.f, 1.3f) && u.max_h == 906 && u.max_w == 1920 - 160); }             // no S / no screen yet
        // owner 23.11: skills x0.9, inventory x0.75 of the window factor; nonsense factors = 1
        assert(near(hu_tip_kind_f(1.3f, 0, HU_TIP_K_SKILL, HU_TIP_K_ITEM), 1.17f) && near(hu_tip_kind_f(1.3f, 1, HU_TIP_K_SKILL, HU_TIP_K_ITEM), 0.975f));
        assert(near(hu_tip_kind_f(1.3f, 1, 0.9f, 7.0f), 1.3f) && near(hu_tip_numbers(hu_tip_kind_f(1.3f, 1, 0.9f, 0.75f), 1.0f, 2400, 1080).f, 0.975f));
        { const hu_tip_fit u = hu_tip_numbers(99, 1, 2400, 1080); assert(near(u.f, 1)); }                                                      // a nonsense factor = 1
        // the run's two tooltips: an inventory item (~254 stage px at factor 1), a hot bar skill (~460); a long weapon ~800; huge ones
        const float hs[] = {120, 254, 460, 700, 800, 1200, 1800};
        for (unsigned i = 0; i < sizeof hs / sizeof *hs; i++) {
            const float h = hs[i], w = 413, f = hu_tip_movie_f(t.f, (float)t.max_h, (float)t.max_w, w, h);
            assert(f <= 1.3f + 1e-4f && f >= 0.5f && (h * f <= t.max_h + 1e-3f || near(f, 0.5f)));
            const float s2 = 1.0f, H = 1080;   // c = 1: the drawn tooltip IS the size the engine was told
            const float y = hu_tip_engine_top(1, h * f, s2, H, 0), y2 = hu_tip_engine_top(0, h * f, s2, H, 700);
            if (h * f <= t.max_h + 1e-3f) assert(y >= 0 && y + h * f * s2 <= H - HU_TIP_HB + 0.01f && y2 >= 0 && y2 + h * f * s2 <= H + 0.01f);
            // the bug of the run: the window drawn x1.3 by the class scale, the engine told the factor-1 size -> its bottom under the screen's edge
            // (clamped by the engine to y + h = H: always 0.3 h too low; the hot bar branch's 158 hid it up to h = 527)
            if (h >= 120) assert(hu_tip_engine_top(0, h, s2, H, 1000) + h * 1.3f > H);   // a slot near the bottom: clamped
            if (h >= 600) assert(hu_tip_engine_top(1, h, s2, H, 0) + h * 1.3f > H);
        }
        assert(near(hu_tip_movie_f(1.3f, 906, 2240, 413, 460), 1.3f) && near(hu_tip_movie_f(1.3f, 906, 2240, 413, 800), 906.0f / 800));
        assert(near(hu_tip_movie_f(0, 0, 0, 413, 460), 1) && near(hu_tip_movie_f(1.3f, 906, 300, 413, 100), 300.0f / 413));   // nothing told = 1; the width fit
    }
    {   // the tooltip stays after a HUD hold's lift until the next finger
        hu_pin p; memset(&p, 0, sizeof p); hu_pin_rec r[HU_PIN_MAX]; int hb, pi;
        dm_ei_val slot; memset(&slot, 0, sizeof slot); slot.type = 4; slot.d = 8;
        dm_ei_val str; memset(&str, 0, sizeof str); str.type = 5;
        assert(!hu_pin_take(&p, &hb, 0x28, "hideTooltip", 0, NULL));                    // not armed: the engine gets it at once
        hu_pin_arm(&p);
        assert(hu_pin_take(&p, &hb, 0x28, "SlotHoverOut", 1, &slot) && hu_pin_take(&p, &hb, 0x28, "hideTooltip", 0, NULL));
        assert(!hu_pin_take(&p, &hb, 0x28, "SlotHover", 1, &slot) && !hu_pin_take(&p, &hb, 0x28, "nextHotbar", 0, NULL));   // only the hover's end is kept
        assert(!hu_pin_take(&p, &hb, 0x28, "SlotHoverOut", 1, &str));                   // a string argument is not kept (an engine string)
        slot.d = 9; assert(hu_pin_take(&p, &hb, 0x28, "SlotHoverOut", 1, &slot) && p.n == 2);   // one per (object, name), the newest arguments
        assert(hu_pin_take(&p, &pi, 0x26, "hideTooltip", 0, NULL) && p.n == 3);
        const int n = hu_pin_release(&p, r);
        assert(n == 3 && !strcmp(r[0].name, "SlotHoverOut") && r[0].n == 1 && r[0].a[0].d == 9 && r[0].ui == &hb && r[0].type == 0x28);
        assert(!strcmp(r[1].name, "hideTooltip") && r[1].n == 0 && r[2].ui == &pi);
        assert(!p.on && hu_pin_release(&p, r) == 0 && !hu_pin_take(&p, &hb, 0x28, "hideTooltip", 0, NULL));   // the next finger ends it
        hu_pin_arm(&p); int objs[HU_PIN_MAX + 2];
        for (int i = 0; i < HU_PIN_MAX + 2; i++) assert(hu_pin_take(&p, &objs[i], 0x28, "hideTooltip", 0, NULL) == (i < HU_PIN_MAX));   // full: passed on
        hu_pin_arm(&p); assert(p.n == 0);                                                // a new hold starts empty
    }
    printf("dos2_touchui_hud_test: ok\n");
    return 0;
}
