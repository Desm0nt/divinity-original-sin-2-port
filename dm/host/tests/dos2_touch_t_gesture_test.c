// [DOS2] Host test of the UI mode TOUCH gesture rules (hle_dos2_touch_gesture.h, branch dos2-uimodes): the iPad's touchscreen_1.json gestures -
// tap / short long press / long press / pan of one finger, the double tap, the two-finger pinch / rotation / pan components, the two- and
// three-finger taps, the three-finger swipe, the tactical camera toggle and the keyboard camera's hold times.
// Run with any C compiler: cc dos2_touch_t_gesture_test.c -o <outside the repo> -lm; run it.
#include "../hle_dos2_touch_gesture.h"
#include <assert.h>
#include <stdio.h>
#ifdef NDEBUG
#error "Build this test with assertions enabled"
#endif
static int near(float a, float b, float e) { return fabsf(a - b) <= e; }
int main(void) {
    const float H = 1080.f, slop = 0.025f * H;   // 27 px, the slop of the hybrid gestures
    const int slong = 350, lng = 600, dtap = 300;  // the iPad-like defaults (DM_DOS2_TOUCH_T_SLONG_MS / _LONG_MS / _DTAP_MS)
    // one finger: still -> wait / short long press / long press; moved before the long press -> pan
    assert(dt_t1_state(0, 0, slop, slong, lng) == DT_T1_WAIT);
    assert(dt_t1_state(slop, 349, slop, slong, lng) == DT_T1_WAIT);           // exactly the slop: still
    assert(dt_t1_state(10, 350, slop, slong, lng) == DT_T1_SLONG);
    assert(dt_t1_state(10, 599, slop, slong, lng) == DT_T1_SLONG);
    assert(dt_t1_state(10, 600, slop, slong, lng) == DT_T1_LONG);
    assert(dt_t1_state(28, 10, slop, slong, lng) == DT_T1_PAN);
    assert(dt_t1_state(28, 450, slop, slong, lng) == DT_T1_PAN);              // after the tooltip time a move still pans (the iPad's pan)
    assert(dt_t1_state(28, 600, slop, slong, lng) == DT_T1_LONG);             // the long press came first
    assert(dt_t1_state(10, 5000, slop, 0, 0) == DT_T1_WAIT);                  // times off: never
    // the release: a tap before the short long press, a tooltip only after it, nothing after a move
    assert(dt_t1_release(5, 100, slop, slong) == DT_REL_TAP);
    assert(dt_t1_release(5, 349, slop, slong) == DT_REL_TAP);
    assert(dt_t1_release(5, 350, slop, slong) == DT_REL_TIP);
    assert(dt_t1_release(40, 100, slop, slong) == DT_REL_NONE);
    assert(dt_t1_release(5, 900, slop, 0) == DT_REL_TAP);                     // DM_DOS2_TOUCH_T_SLONG_MS=0: every still release clicks
    // double tap: within 300 ms of the first tap's release and near it
    const float r = 2 * slop;
    assert(dt_is_dtap(1000, 500, 500, 1200, 510, 505, dtap, r));
    assert(dt_is_dtap(1000, 500, 500, 1300, 500, 500, dtap, r));
    assert(!dt_is_dtap(1000, 500, 500, 1301, 500, 500, dtap, r));             // too late
    assert(!dt_is_dtap(1000, 500, 500, 1100, 600, 500, dtap, r));             // too far
    assert(!dt_is_dtap(0, 500, 500, 100, 500, 500, dtap, r));                 // no previous tap
    assert(!dt_is_dtap(1000, 500, 500, 900, 500, 500, dtap, r));              // clock went back
    // two fingers: pair delta
    {
        const float a[4] = {800, 500, 1000, 500};           // 200 apart, horizontal
        float dd, da, mx, my;
        const float apart[4] = {750, 500, 1050, 500};
        dt_pair_delta(a, apart, &dd, &da, &mx, &my);
        assert(near(dd, 100, 0.01f) && near(da, 0, 1e-5f) && near(mx, 0, 0.01f) && near(my, 0, 0.01f));
        const float turned[4] = {900 - 100 * cosf(0.5f), 500 - 100 * sinf(0.5f), 900 + 100 * cosf(0.5f), 500 + 100 * sinf(0.5f)};   // clockwise 0.5 rad
        dt_pair_delta(a, turned, &dd, &da, &mx, &my);
        assert(near(dd, 0, 0.01f) && near(da, 0.5f, 1e-4f) && near(mx, 0, 0.01f) && near(my, 0, 0.01f));
        const float moved[4] = {830, 560, 1030, 560};
        dt_pair_delta(a, moved, &dd, &da, &mx, &my);
        assert(near(dd, 0, 0.01f) && near(da, 0, 1e-5f) && near(mx, 30, 0.01f) && near(my, 60, 0.01f));
        // the angle wraps: the pair turned across +-pi
        const float b[4] = {1000, 501, 800, 500}, c[4] = {1000, 499, 800, 500};
        dt_pair_delta(b, c, &dd, &da, &mx, &my);
        assert(fabsf(da) < 0.02f);
    }
    // components: first past its step, others past twice theirs
    {
        const float ps = 0.04f * H, rs = 0.03f * H, pn = 0.03f * H;   // 43.2 / 32.4 / 32.4 px
        assert(dt_two_mask(0, 10, 10, 10, ps, rs, pn) == 0);
        assert(dt_two_mask(0, 50, 10, 10, ps, rs, pn) == DT_2F_PINCH);
        assert(dt_two_mask(DT_2F_PINCH, 60, 10, 40, ps, rs, pn) == DT_2F_PINCH);                 // pinch drift: no pan yet
        assert(dt_two_mask(DT_2F_PINCH, 60, 10, 70, ps, rs, pn) == (DT_2F_PINCH | DT_2F_PAN));   // a real pan joins
        assert(dt_two_mask(0, 10, 10, 40, ps, rs, pn) == DT_2F_PAN);
        assert(dt_two_mask(0, -50, 40, 0, ps, rs, pn) == (DT_2F_PINCH | DT_2F_ROT));            // both at once from nothing
        assert(dt_two_mask(DT_2F_ROT, 0, 0, 0, ps, rs, pn) == DT_2F_ROT);                        // stays on
    }
    // multi-finger taps
    assert(dt_tapn(2, 200, 10, 0, slong, 2 * slop) == 2);
    assert(dt_tapn(3, 340, 20, 0, slong, 2 * slop) == 3);
    assert(dt_tapn(2, 400, 10, 0, slong, 2 * slop) == 0);      // too slow: a long press of two fingers, no tap
    assert(dt_tapn(2, 200, 60, 0, slong, 2 * slop) == 0);      // moved
    assert(dt_tapn(2, 200, 10, 1, slong, 2 * slop) == 0);      // a pinch / pan ran
    assert(dt_tapn(1, 100, 0, 0, slong, 2 * slop) == 0 && dt_tapn(4, 100, 0, 0, slong, 2 * slop) == 0);
    // three-finger swipe (8 % of the height)
    {
        const float md = 0.08f * H;
        assert(dt_swipe3(5, 100, md) == 1 && dt_swipe3(-10, -100, md) == -1);
        assert(dt_swipe3(0, 80, md) == 0);                  // not far enough
        assert(dt_swipe3(80, 100, md) == 0);                // not vertical enough (needs 1.5 x)
        assert(dt_swipe3(60, 100, md) == 1);
    }
    // the tactical camera: On / Off on the PC's one toggle key
    {
        int st = 0;
        assert(dt_tac_press(&st, 1) && st == 1);            // swipe down: on -> press 'o'
        assert(!dt_tac_press(&st, 1) && st == 1);           // again down: already on, nothing
        assert(dt_tac_press(&st, -1) && st == 0);           // swipe up: off -> press 'o'
        assert(!dt_tac_press(&st, -1) && st == 0);
        assert(!dt_tac_press(&st, 0) && st == 0);
    }
    // the keyboard camera: hold time owed per axis
    {
        float b = 0;
        dt_hold_add(&b, 108, H, 1200, 600);                  // a tenth of the height = 120 ms
        assert(near(b, 120, 0.01f));
        dt_hold_add(&b, 108, H, 1200, 600);
        assert(near(b, 240, 0.01f));
        dt_hold_add(&b, 1080, H, 1200, 600);                 // capped
        assert(near(b, 600, 0.01f));
        dt_hold_add(&b, -54, H, 1200, 600);                  // turned: the owed hold is dropped first
        assert(near(b, -60, 0.01f));
        dt_hold_tick(&b, 16);
        assert(near(b, -44, 0.01f));
        dt_hold_tick(&b, 100);
        assert(b == 0);
        dt_hold_tick(&b, 16);
        assert(b == 0);
        dt_hold_add(&b, 0.5f, 1.f, 700, 900);                // a rotation of 0.5 rad at 700 ms per radian
        assert(near(b, 350, 0.01f));
        dt_hold_add(&b, 1, 0, 700, 900);                     // a bad unit: unchanged
        assert(near(b, 350, 0.01f));
        assert(dt_hold_dir(10, 4) == 1 && dt_hold_dir(-10, 4) == -1 && dt_hold_dir(3, 4) == 0);
        // keys: the finger right + down = the view left + forward (a + w); inverted = d + s; rotation clockwise = end
        assert(dt_cam_keys(100, 100, 0, 0, 0, 4) == (DT_K_A | DT_K_W));
        assert(dt_cam_keys(-100, -100, 0, 0, 0, 4) == (DT_K_D | DT_K_S));
        assert(dt_cam_keys(100, 100, 0, 1, 0, 4) == (DT_K_D | DT_K_S));
        assert(dt_cam_keys(0, 0, 50, 0, 0, 4) == DT_K_ROTR && dt_cam_keys(0, 0, -50, 0, 0, 4) == DT_K_ROTL);
        assert(dt_cam_keys(0, 0, 50, 0, 1, 4) == DT_K_ROTL);
        assert(dt_cam_keys(2, -3, 1, 0, 0, 4) == 0);
    }
    {   // review of run 20.11: the keyboard twins of the host's pad pulses in TOUCH (raw ids of main+0x2974b10, index - 2)
        assert(dt_pad_key(1u << 0) == 0x00 && dt_pad_key(1u << 1) == 0x01);                 // A = return, B = escape
        assert(dt_pad_key(1u << 13) == 0x59 && dt_pad_key(1u << 15) == 0x58);               // up, down
        assert(dt_pad_key(1u << 12) == 0x57 && dt_pad_key(1u << 14) == 0x56);               // left, right
        assert(dt_pad_key(1u << 11) == 0x30);                                               // '-' = m (ToggleMap)
        assert(dt_pad_key(1u << 2) == DT_PADKEY_NONE && dt_pad_key(1u << 3) == DT_PADKEY_NONE && dt_pad_key(1u << 8) == DT_PADKEY_NONE && dt_pad_key(0) == DT_PADKEY_NONE);
        assert(dt_pad_key((1u << 0) | (1u << 11)) == 0x00);                                 // A wins over '-'
        // the TOUCH keys go while a menu is up or out of game; unknown state (bit 2 clear) keeps them (the old rule)
        assert(dt_t_away(4 | 2 | 1) && dt_t_away(4) && dt_t_away(4 | 1) && !dt_t_away(4 | 2) && !dt_t_away(0) && !dt_t_away(1) && !dt_t_away(2 | 1));
        // the PC windows that take the finger as a menu; the HUD, the tooltips and the tutorial overlay do not
        static const uint32_t menu[] = {0x74, 0x77, 0x29, 0x66, 0x16, 0x68, 0x2e, 0x2f, 0x0e, 0x88, 0x15, 0x0a, 0x0b};
        static const uint32_t hud[] = {0x28, 0x1e, 0x26, 0x75, 0x07, 0x8d, 0x2c, 0x30, 0x37, 0x05, 0x10, 0x2a, 0x1d, 0x13};
        for (unsigned i = 0; i < sizeof menu / sizeof *menu; i++) assert(dt_pc_menu_type(menu[i]));
        for (unsigned i = 0; i < sizeof hud / sizeof *hud; i++) assert(!dt_pc_menu_type(hud[i]));
        // run 21.11: the windows that stop the mouse below them: the panels and containers, not the small context menus, never the HUD
        for (unsigned i = 0; i < sizeof menu / sizeof *menu; i++) assert(dt_pc_blocker(menu[i]) == (menu[i] != 0x0a && menu[i] != 0x0b));
        for (unsigned i = 0; i < sizeof hud / sizeof *hud; i++) assert(!dt_pc_blocker(hud[i]));
        assert(dt_pc_blocker(0x09) && dt_pc_blocker(0x25));
        {   // the walk runs from the last entry (top) down: the LOWEST visible blocker is the one below which nothing gets the press
            const uint32_t ty[6] = {0x30, 0x1e, 0x10, 0x74, 0x77, 0x28}; const uint8_t vis[6] = {1, 1, 1, 1, 1, 1}, vis2[6] = {1, 1, 1, 0, 1, 1}, none[6] = {1, 1, 1, 0, 0, 1};
            assert(dt_lowest_blocker(ty, vis, 6) == 3 && dt_lowest_blocker(ty, vis2, 6) == 4 && dt_lowest_blocker(ty, none, 6) == -1);
        }
        // a HUD finger whose probe press was taken: tap < 350 ms clicks, 350..599 = tooltip only, >= 600 = the right click; no tooltip step
        // when the short long press is 0 or not shorter than the long press
        assert(dt_tui_state(0, 350, 600) == DT_TUI_CLICK && dt_tui_state(349, 350, 600) == DT_TUI_CLICK);
        assert(dt_tui_state(350, 350, 600) == DT_TUI_TIP && dt_tui_state(599, 350, 600) == DT_TUI_TIP);
        assert(dt_tui_state(600, 350, 600) == DT_TUI_LONG && dt_tui_state(5000, 350, 600) == DT_TUI_LONG);
        assert(dt_tui_state(400, 0, 600) == DT_TUI_CLICK && dt_tui_state(400, 600, 600) == DT_TUI_CLICK && dt_tui_state(600, 600, 600) == DT_TUI_LONG);
    }
    puts("PASS: dos2 TOUCH gestures (one-finger tap / tooltip / long press / pan, double tap, two-finger pinch / rotation / pan, 2f / 3f taps, "
         "3f swipe, tactical camera toggle, keyboard camera hold times; run 20.11: pad pulse keyboard twins, keys let go on menus, PC windows as menus)");
    return 0;
}
