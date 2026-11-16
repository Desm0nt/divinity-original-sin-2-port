// [DOS2] Pure gesture rules of one world finger (hle_dos2_touch.c), kept apart so a host test can check them without the game
// (dm/host/tests/dos2_touch_gesture_test.c). Owner 16.11 (branch dos2-touch5): a one-finger swipe on the world rotates the camera.
//   - short tap (up quickly, moved less than the slop)                  = tap: walk / interact (as before);
//   - held still for the hold time                                       = hold: follow-the-finger walk on the floor / "Actions..." on an object;
//   - moved past the slop BEFORE the hold time                           = swipe: camera rotation (horizontal delta -> right stick x), no walk, no tap;
//   - moved past the slop AFTER the hold started (the walk is on)        = the walk goes on (stick follows the finger, as before).
// swipe_on = 0 (DM_DOS2_TOUCH_SWIPE_ROT=0) gives the old rule: any move past the slop = the walk stick.
#ifndef HLE_DOS2_TOUCH_GESTURE_H
#define HLE_DOS2_TOUCH_GESTURE_H
#include <math.h>
#include <stdint.h>

enum { DT_GW_WAIT = 0 /* not decided yet */, DT_GW_STICK = 1 /* walk stick */, DT_GW_SWIPE = 3 /* camera rotation */ };

// A move event of a still undecided world finger: moved = distance from where it landed (px), held_ms = time since it landed.
static inline int dt_gesture_move(float moved, uint64_t held_ms, float slop, int hold_ms, int swipe_on) {
    if (!(moved > slop)) return DT_GW_WAIT;
    return swipe_on && held_ms < (uint64_t)(hold_ms > 0 ? hold_ms : 0) ? DT_GW_SWIPE : DT_GW_STICK;
}
// One step of the swipe rotation: dx / dy = the finger's motion since the last step. -1 / +1 = a step left / right (horizontal dominates and
// is past `step` px), 0 = nothing yet; *reset = 1 when a vertical-dominant motion past `step` should restart the reference point.
static inline int dt_swipe_step(float dx, float dy, float step, int *reset) {
    if (reset) *reset = 0;
    if (fabsf(dx) > step && fabsf(dx) > fabsf(dy)) return dx > 0 ? 1 : -1;
    if (fabsf(dy) > step && reset) *reset = 1;
    return 0;
}
// The right stick x a swipe step asks for: the same sign rule as the two-finger rotation (fingers right = +x unless DM_DOS2_TOUCH_ROT_SWAP).
static inline int dt_swipe_rx(int dir, int swap) { return dir == 0 ? 0 : ((dir > 0) != (swap != 0)) ? 32767 : -32767; }
// The item placement mode (Actions... -> Move): the character's input controller runs the item-move task (PC task type 10, vtable
// main+0x29ca7a8: its cursor function sets CursorItemMove = 19 at main+0xe32cd8, its OnInputEvent main+0xe32d00 places on an Action1 release).
#define DT_TASK_ITEMMOVE 10
static inline int dt_is_placing(int running_task_type) { return running_task_type == DT_TASK_ITEMMOVE; }
#endif
