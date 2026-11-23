// [DOS2] Host test of the world-finger gesture rules (hle_dos2_touch_gesture.h, branch dos2-touch5): tap / hold / swipe and the swipe's
// rotation steps, the item placement test. Run with any C compiler: cc dos2_touch_gesture_test.c -o <outside the repo>; run it.
#include "../hle_dos2_touch_gesture.h"
#include <assert.h>
#include <stdio.h>
#ifdef NDEBUG
#error "Build this test with assertions enabled"
#endif
int main(void) {
    const float slop = 27.f;   // 2.5 % of 1080
    const int hold = 300;      // g_world_hold_ms with DM_DOS2_TOUCH_WORLD=3
    // still finger: undecided (a release = tap, the hold timer = walk / Actions...)
    assert(dt_gesture_move(0.f, 50, slop, hold, 1) == DT_GW_WAIT);
    assert(dt_gesture_move(slop, 50, slop, hold, 1) == DT_GW_WAIT);          // exactly the slop: still a tap
    assert(dt_gesture_move(26.f, 900, slop, hold, 1) == DT_GW_WAIT);
    // moved past the slop before the hold time = swipe; after it = the walk stick
    assert(dt_gesture_move(28.f, 0, slop, hold, 1) == DT_GW_SWIPE);
    assert(dt_gesture_move(200.f, 299, slop, hold, 1) == DT_GW_SWIPE);
    assert(dt_gesture_move(28.f, 300, slop, hold, 1) == DT_GW_STICK);
    assert(dt_gesture_move(500.f, 2000, slop, hold, 1) == DT_GW_STICK);
    // DM_DOS2_TOUCH_SWIPE_ROT=0: the old rule (any move past the slop = stick)
    assert(dt_gesture_move(28.f, 0, slop, hold, 0) == DT_GW_STICK);
    assert(dt_gesture_move(10.f, 0, slop, hold, 0) == DT_GW_WAIT);
    // rotation steps (step 2 % of 1080 = 21.6 px)
    int reset = 9;
    assert(dt_swipe_step(30.f, 5.f, 21.6f, &reset) == 1 && reset == 0);
    assert(dt_swipe_step(-30.f, 5.f, 21.6f, &reset) == -1 && reset == 0);
    assert(dt_swipe_step(10.f, 3.f, 21.6f, &reset) == 0 && reset == 0);          // below the step: nothing yet
    assert(dt_swipe_step(25.f, 40.f, 21.6f, &reset) == 0 && reset == 1);         // vertical dominant: restart the reference
    assert(dt_swipe_step(25.f, -25.f, 21.6f, &reset) == 0 && reset == 1);        // a tie is not horizontal
    assert(dt_swipe_step(30.f, 0.f, 21.6f, NULL) == 1);
    // the sign rule of the two-finger rotation (fingers right = +x, DM_DOS2_TOUCH_ROT_SWAP flips)
    assert(dt_swipe_rx(1, 0) == 32767 && dt_swipe_rx(-1, 0) == -32767);
    assert(dt_swipe_rx(1, 1) == -32767 && dt_swipe_rx(-1, 1) == 32767);
    assert(dt_swipe_rx(0, 0) == 0);
    // the item placement mode = the item-move task (type 10) runs; the move task (0), the pad's stick task (25), talk (4), item use (6) are not
    assert(dt_is_placing(10));
    assert(!dt_is_placing(0) && !dt_is_placing(25) && !dt_is_placing(4) && !dt_is_placing(6) && !dt_is_placing(-1));
    // dos2-touch6: the target modes (item move 10 by preview or running; aim types by the mask, default "2" = the throw / skill task)
    const uint32_t m2 = dt_aim_mask("2");
    assert(m2 == 1u << 2 && dt_aim_mask("2,11") == ((1u << 2) | (1u << 11)) && dt_aim_mask("") == 0 && dt_aim_mask("x,40,3") == 1u << 3);
    assert(dt_aim_mask(" 2 ; 5") == ((1u << 2) | (1u << 5)) && dt_aim_mask("99999999") == 0);
    assert(dt_target_task(10, -1, 1, m2) == 10 && dt_target_task(-1, 10, 1, m2) == 10);          // the item move as before
    assert(dt_target_task(10, -1, 0, m2) == -1);                                                    // DM_DOS2_TOUCH_PLACE=0
    assert(dt_target_task(2, -1, 1, m2) == 2 && dt_target_task(-1, 2, 1, m2) == 2 && dt_target_task(0, 2, 1, m2) == 2);   // throw / aim
    assert(dt_target_task(2, -1, 1, 0) == -1);                                                      // DM_DOS2_TOUCH_AIM=0
    assert(dt_target_task(0, -1, 1, m2) == -1 && dt_target_task(25, 0, 1, m2) == -1 && dt_target_task(4, 6, 1, m2) == -1 && dt_target_task(-1, -1, 1, m2) == -1);
    assert(dt_target_task(10, 2, 1, m2) == 10);                                                     // the move wins
    // UI mode TOUCH (owner 23.11): a prepared skill / target mode = a world tap only aims (dt_aim_latch / dt_aim_frame)
    {   int L = 0, blk = 0;
        // run 23.11 "ActionAttackGround": the attack / cast task (2) previewed over the bare ground = a target mode; it stays over a character
        L = dt_aim_latch(L, DT_TASK_AIM, 0); assert(L == 1);
        L = dt_aim_latch(L, DT_TASK_AIM, 1); assert(L == 1);           // aiming at an enemy: still the mode
        L = dt_aim_latch(L, -1, -1); assert(L == 1);                   // a transient frame without a preview keeps it
        L = dt_aim_latch(L, 0, 0); assert(L == 0);                     // the walk previewed over the ground: the mode is over
        // no mode: the attack preview of the hovered enemy is the plain PC attack (a tap attacks at once)
        L = dt_aim_latch(0, DT_TASK_AIM, 1); assert(L == 0);
        L = dt_aim_latch(0, DT_TASK_AIM, -1); assert(L == 0);          // unknown under the cursor: not a mode
        assert(dt_aim_latch(1, DT_TASK_SKILL, 0) == 1 && dt_aim_latch(1, 4, 1) == 0 && dt_aim_latch(1, 6, 1) == 0);   // talk / use end it
        // run 21.11: a hot bar skill = task 3 previewed and running (also while the cursor is on CONFIRM, and after the bar's own mark dropped)
        assert(dt_aim_frame(DT_TASK_SKILL, DT_TASK_SKILL, 0, &blk) == 1 && !blk);
        assert(dt_aim_frame(-1, DT_TASK_SKILL, 0, &blk) == 1);         // the finger landed on the world: preview -1, still running 3
        assert(dt_aim_frame(DT_TASK_AIM, -1, 1, &blk) == 1);           // the latched target mode
        assert(dt_aim_frame(DT_TASK_AIM, -1, 0, &blk) == 0);           // task 2 alone (the hover attack): nothing prepared
        assert(dt_aim_frame(0, -1, 0, &blk) == 0 && dt_aim_frame(-1, -1, 0, &blk) == 0);
        // CONFIRM / cancel sent: the lingering facts count again only after one frame without them
        blk = 1;
        assert(dt_aim_frame(DT_TASK_SKILL, DT_TASK_SKILL, 0, &blk) == 0 && blk == 1);
        assert(dt_aim_frame(-1, DT_TASK_AIM, 1, &blk) == 0 && blk == 1);   // the cast runs
        assert(dt_aim_frame(0, -1, 0, &blk) == 0 && blk == 0);             // over
        assert(dt_aim_frame(DT_TASK_SKILL, DT_TASK_SKILL, 0, &blk) == 1);  // the next prepared skill counts at once
    }
    // dos2-touch7: bottomBar_d (stage mouseDown / mouseUp listener since touch6) answers "handled" for every press; the host keeps it only
    // inside the movie's own HUD rects. Rects as the phone reported them (2400x1080 window, run C_base 18.11) + the hint row of touch6.
    {
        const float W = 2400.f, H = 1080.f, sy = 0.012f, sx = 0.012f * H / W;
        int bb_player, col_player;   // two movies: bottomBar_d and another one (its rects must not count for bottomBar_d)
#define R(p, x0, x1, y0, y1, nm) {(const void *)(p), 1000, (x0) / W, (y0) / H, (x1) / W, (y1) / H, nm}
        const dt_hr hr[] = {
            R(&bb_player, 588, 1772, 877, 1072, "hotbar"),     // bottombar_mc: slots + the hint row above them
            R(&bb_player, 600, 760, 880, 916, "btn2"),         // "(A) Move" of the hint row (bottombar_mc.btnList)
            R(&bb_player, 780, 960, 880, 916, "btn3"),         // "(X) Actions"
            R(&bb_player, 1720, 1794, 20, 94, "dmVisOpt_mc"),  // a grafted button at the top right
            R(&bb_player, 1384, 1759, 1026, 1062, "hint4"),
            R(&col_player, 2290, 2390, 300, 900, "col"),       // the column's own movie
        };
#undef R
        const unsigned n = sizeof hr / sizeof *hr;
        const uint64_t ttl = 5000, now = 2000;
#define AT(p, x, y) dt_hr_state(hr, n, (p), now, ttl, (x) / W, (y) / H, sx, sy)
        // the world taps / swipe starts of the run: empty sand, the hint row shown - NOT kept (the world's)
        assert(AT(&bb_player, 1340, 600) == DT_HR_OUT && !dt_stage_press_keep(1, 1, AT(&bb_player, 1340, 600)));
        assert(AT(&bb_player, 1060, 520) == DT_HR_OUT && !dt_stage_press_keep(1, 1, AT(&bb_player, 1060, 520)));
        assert(AT(&bb_player, 1500, 620) == DT_HR_OUT && AT(&bb_player, 900, 620) == DT_HR_OUT);
        assert(AT(&bb_player, 1340, 860) == DT_HR_OUT);                     // just above the bar, past the slop (877 - 0.012 * 1080 = 864)
        // a column icon (another movie's rect) is not bottomBar_d's: given back, so dmColumn_d below gets the press
        assert(AT(&bb_player, 2340, 600) == DT_HR_OUT && !dt_stage_press_keep(1, 1, AT(&bb_player, 2340, 600)));
        // the hint buttons, a hot bar slot, a grafted button, the Y hint: kept (the movie's own tap code runs)
        assert(AT(&bb_player, 680, 898) == DT_HR_IN && dt_stage_press_keep(1, 1, AT(&bb_player, 680, 898)));   // "(A) Move"
        assert(AT(&bb_player, 870, 898) == DT_HR_IN && dt_stage_press_keep(1, 1, AT(&bb_player, 870, 898)));   // "(X) Actions"
        assert(AT(&bb_player, 1200, 1000) == DT_HR_IN && AT(&bb_player, 1757, 57) == DT_HR_IN && AT(&bb_player, 1500, 1050) == DT_HR_IN);
        assert(AT(&bb_player, 1340, 870) == DT_HR_IN);                      // inside the slop above the bar
        // stale rects (movie stopped reporting) or none: unknown, the movie's answer stands (the touch6 behaviour)
        assert(dt_hr_state(hr, n, &bb_player, now + ttl + 1, ttl, 1340 / W, 600 / H, sx, sy) == DT_HR_NONE);
        assert(dt_hr_state(hr, 0, &bb_player, now, ttl, 1340 / W, 600 / H, sx, sy) == DT_HR_NONE && dt_stage_press_keep(1, 1, DT_HR_NONE));
        // a movie without a stage listener, or one that did not take the press: unchanged
        assert(dt_stage_press_keep(1, 0, DT_HR_OUT) && !dt_stage_press_keep(0, 1, DT_HR_IN) && !dt_stage_press_keep(0, 0, DT_HR_NONE));
#undef AT
    }
    // dos2-touch7: the ZL / ZR wheels. Centre: panelSelect_c 1200x1080 centred in a 2400x1080 window (FlashPlayer rect as the phone's
    // bottomBar_d: x 240 w 1920 would also give 1200), wheel at stage 600,470
    {
        float cx, cy;
        const int32_t f53[4] = {600, 0, 1200, 1080}, f52[4] = {240, 0, 1920, 1080}, fbig[4] = {0, 0, 2400, 1080}, bad[4] = {0, 0, 8, 8};
        assert(dt_radial_centre(f53, 1200, 1080, 600, 470, &cx, &cy) && fabsf(cx - 1200) < 0.01f && fabsf(cy - 470) < 0.01f);
        assert(dt_radial_centre(f52, 1920, 1080, 960, 470, &cx, &cy) && fabsf(cx - 1200) < 0.01f && fabsf(cy - 470) < 0.01f);
        assert(dt_radial_centre(fbig, 1200, 1080, 600, 470, &cx, &cy) && fabsf(cx - 1200) < 0.01f && fabsf(cy - 470) < 0.01f);   // a window-sized rect: centred fit
        assert(!dt_radial_centre(bad, 1200, 1080, 600, 470, &cx, &cy));
        // the finger's stick: npad y up; the wheel's angle (atan2(V, H), screen axes) points at the finger
        int sx, sy;
        const float dead = 0.07f * 1080;
        assert(!dt_radial_stick(1210, 480, 1200, 470, dead, &sx, &sy) && sx == 0 && sy == 0);                  // the centre: nothing
        assert(dt_radial_stick(1450, 470, 1200, 470, dead, &sx, &sy) && sx == DT_RAD_FULL && sy == 0);           // right
        assert(fabsf(dt_radial_angle(sx, sy)) < 1e-4f);
        assert(dt_radial_stick(1200, 220, 1200, 470, dead, &sx, &sy) && sx == 0 && sy == DT_RAD_FULL);           // up on screen = stick up
        assert(fabsf(dt_radial_angle(sx, sy) + 1.5707963f) < 1e-4f);                                             // the wheel: -pi/2 (up)
        assert(dt_radial_stick(1200, 720, 1200, 470, dead, &sx, &sy) && sy == -DT_RAD_FULL && fabsf(dt_radial_angle(sx, sy) - 1.5707963f) < 1e-4f);
        assert(dt_radial_stick(1000, 670, 1200, 470, dead, &sx, &sy) && sx < 0 && sy < 0 && fabsf(dt_radial_angle(sx, sy) - 2.3561945f) < 1e-3f);   // down left
        assert((int64_t)sx * sx + (int64_t)sy * sy > (int64_t)(0.75 * DT_RAD_FULL) * (int64_t)(0.75 * DT_RAD_FULL));   // past the engine's 0.75
        // the pad's left stick points an open wheel while the right one rests
        int32_t lx = 20000, ly = -20000, rx = 100, ry = -50;
        assert(dt_radial_lstick(&lx, &ly, &rx, &ry, 6000) && rx == 20000 && ry == -20000 && lx == 0 && ly == 0);
        lx = 20000; ly = 0; rx = 30000; ry = 0;
        assert(!dt_radial_lstick(&lx, &ly, &rx, &ry, 6000) && rx == 30000 && lx == 20000);                       // the pad's own right stick wins
        lx = 3000; ly = 0; rx = 0; ry = 0;
        assert(!dt_radial_lstick(&lx, &ly, &rx, &ry, 6000) && rx == 0 && lx == 3000);                             // a resting left stick: nothing
        // the choose: the held trigger is let go for the game until the player lets it go
        const uint64_t ZL = 1u << 8, ZR = 1u << 9, A = 1u << 0, T = ZL | ZR;
        uint64_t sup = 0;
        assert(dt_radial_release(ZL | A, 0, T, &sup) == (ZL | A) && sup == 0);   // before the choose: as the pad says
        assert(dt_radial_release(ZL | A, 1, T, &sup) == A && sup == ZL);         // the choose: ZL up for the game
        assert(dt_radial_release(ZL, 0, T, &sup) == 0 && sup == ZL);             // still held by the player: still up for the game
        assert(dt_radial_release(ZR, 0, T, &sup) == ZR && sup == 0);             // ZL let go: nothing held back (ZR passes)
        assert(dt_radial_release(ZL, 0, T, &sup) == ZL && sup == 0);             // a new ZL press opens the wheel again
        assert(dt_radial_release(A, 1, T, &sup) == A && sup == 0);               // a choose with no trigger held: nothing to let go
    }
    {   // phone run 23.11: a still hold on a hot bar slot flipped the deck - the no-click lift went to -64,-64 and hotBar_p's swipe took the jump
        // for one (x at the release - x at the press > 130 stage px). The park keeps the finger's x, so a hold measures no travel
        float px, py;
        dt_tui_park(1458, -64, &px, &py); assert(px == 1458 && py < 0);
        dt_tui_park(10, 0, &px, &py); assert(px == 10 && py < 0);                  // the park is always above the window: never an element
        const float swipe = 130, landed = 1458;
        dt_tui_park(landed, -64, &px, &py); assert(!(px - landed > swipe) && !(px - landed < -swipe));   // no travel from a hold
        assert(-64 - landed < -swipe);                                             // the old park: a 'swipe' of 1522 px (nextHotbar)
    }
    puts("PASS: dos2 touch gestures (tap / hold / swipe, rotation steps, placement task, aim / throw modes, TOUCH prepared skill / target mode, stage-listener press vs HUD rects, ZL / ZR wheels, the hold park keeps x)");
    return 0;
}
