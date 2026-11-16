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
    puts("PASS: dos2 touch gestures (tap / hold / swipe, rotation steps, placement task)");
    return 0;
}
