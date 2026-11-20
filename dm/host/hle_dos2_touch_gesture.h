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
// Branch dos2-touch6 (owner 18.11: "Throw" from the inventory, then a tap on the ground made the character run there and cancelled the throw).
// The pad aims those modes with its selector + A, exactly as the item move; the throw's targeting is the input controller's attack / skill task
// (PC task type 2, made at main+0xe1de50, vtable main+0x29c9cf0: its code holds "Cast skill", "Target is too heavy", "Target is too far!"; the
// context menu entry "Throw Item" (hbce667b6...) is action 0x1b at main+0xfcd3f8). aim_mask = the task types that count as an aim mode
// (DM_DOS2_TOUCH_AIM_TYPES, default bit 2). The target mode of a controller: the item move (10) when its preview or running task is 10, else an
// aim type when the preview or the running task is one; -1 = neither (a tap stays the PC click).
#define DT_TASK_AIM 2
static inline int dt_target_task(int preview_type, int running_type, int place_on, uint32_t aim_mask) {
    if (place_on && (dt_is_placing(preview_type) || dt_is_placing(running_type))) return DT_TASK_ITEMMOVE;
    if (preview_type >= 0 && preview_type < 32 && (aim_mask >> preview_type & 1)) return preview_type;
    if (running_type >= 0 && running_type < 32 && (aim_mask >> running_type & 1)) return running_type;
    return -1;
}
// DM_DOS2_TOUCH_AIM_TYPES: "2" / "2,11" / "" (none) -> mask; bad items are skipped, types >= 32 ignored
static inline uint32_t dt_aim_mask(const char *s) {
    uint32_t m = 0; int v = -1;
    for (;; s++) {
        if (s && *s >= '0' && *s <= '9') v = (v < 0 ? 0 : v * 10) + (*s - '0');
        else { if (v >= 0 && v < 32) m |= 1u << v; v = -1; if (!s || !*s) break; }
        if (v > 1000) v = 1000;
    }
    return m;
}
// Branch dos2-touch7 (phone run 18.11 after touch6, logs C_base vs nrB_mat): every one-finger press in game was "TAKEN by a UI object (type
// 0x3b)" - world taps did not walk, swipes did not turn the camera, the column icons (dmColumn_d, below bottomBar_d in the UI walk) got the
// press as "not its event". Cause: tools/dos2_taps5_touch.py hangs a mouseDown listener on bottomBar_d's STAGE (the long-press tooltip, the hint
// row taps). A press that hits no element of the movie targets the stage, the listener runs, and Iggy answers "handled" for the whole 1920x1080
// stage (the same movie answered 0 at the same points before touch6; its stage mouseUp listener does the same to the release). The UI dispatch
// main+0xaea7b0 stores that answer in [x5] (main+0xaea8d4), so the objects below get vt+0xb0 "an upper object took it", the press captures the
// mouse ([uimgr+0x10d] / [uimgr+0x218], main+0xaea8ec) and the input never becomes the world's Action1.
// Fix (hle_dos2_touchui_route.c, DM_DOS2_TOUCH_STAGE_TYPES): for a movie with a stage listener its "handled" says nothing about the point, so the
// host checks the point against that movie's own HUD rects (tools/dos2_hudrect_touch.py: the hot bar = bottombar_mc with the hint row
// "(A) Move (X) Actions" in it, btn<code> / hold<code>, the grafted buttons, hint4). Inside one (+ the hint slop): the movie keeps the press.
// Outside all of them: the answer is cleared (the movie's listener still ran - a press anywhere still hides the long-press tooltip) and the
// press goes on to the objects below and to the world. No live rects of that movie: unknown, the movie's answer stands.
typedef struct { const void *player; uint64_t t; float x0, y0, x1, y1; char id[20]; } dt_hr;   // a HUD rect, window-normalised
enum { DT_HR_NONE = -1 /* no live rect of that movie */, DT_HR_OUT = 0, DT_HR_IN = 1 };
// nx, ny = the point / the window (0..1); sx, sy = the slop in the same units; ttl = how long a rect lives after its movie's last report
static inline int dt_hr_state(const dt_hr *r, unsigned n, const void *player, uint64_t now, uint64_t ttl, float nx, float ny, float sx, float sy) {
    int live = 0;
    for (unsigned i = 0; i < n; i++) {
        if (r[i].player != player || now - r[i].t > ttl) continue;
        live = 1;
        if (nx >= r[i].x0 - sx && nx <= r[i].x1 + sx && ny >= r[i].y0 - sy && ny <= r[i].y1 + sy) return DT_HR_IN;
    }
    return live ? DT_HR_OUT : DT_HR_NONE;
}
// Does a UI object's "handled" for a button press stand? ui = its answer, stage = the movie has a stage listener (DM_DOS2_TOUCH_STAGE_TYPES),
// hr = dt_hr_state of the point for that movie
static inline int dt_stage_press_keep(int ui, int stage, int hr) { return ui && (!stage || hr != DT_HR_OUT); }
// Branch dos2-touch7 (owner 18.11): the ZL / ZR wheels select with the RIGHT stick, which a phone player does not have. The two wheels are
// panelSelect_c.swf (UI type 0x53, "IE PanelSelect": inventory / skills / map / journal ...; stage 1200x1080, radialHolder_mc at 600,470) and
// partyManagement_c.swf (0x52, "IE PartyManagement": the party; stage 1920x1080, radialHolder_mc at 960,470). Both open while their pad button is
// HELD (onEventDown -> openMenu) and act on its RELEASE (onEventUp -> closeMenu(true): ExternalInterface openPanel(id) / setActiveChar(id)).
// Their onFrame turns stickHAxis / stickVAxis into a direction (atan2(V, H), screen axes: y down) and list.selectDirectionRad; the engine
// (main+0x1219020..0x1219130) writes those two every frame from the input events UIRadialLeft / Right / Up / Down (ids 0x126..0x129, registered
// at main+0x108cc40..0x108ce48): H = Right - Left, V = Down - Up, both 0 while |(H, V)| < 0.75. So the host feeds the wheel by the right stick:
// a finger on the wheel = the right stick pointing from the wheel's centre to the finger (full tilt past a dead radius); the left stick of the
// pad does the same while the wheel is open and the right stick rests; a finger lifted past the dead radius = choose: the held ZL / ZR is let go
// for the game (the release is what chooses) until the player really lets it go, so the wheel does not open again under the still-held button.
#define DT_RAD_FULL 32767
// The right stick for a finger at fx, fy (window px) on a wheel centred at cx, cy: 1 and *sx, *sy (npad: x right +, y UP +) past dead px, else 0
static inline int dt_radial_stick(float fx, float fy, float cx, float cy, float dead, int *sx, int *sy) {
    const float dx = fx - cx, dy = fy - cy, r = sqrtf(dx * dx + dy * dy);
    if (!(r > dead) || r < 1e-3f) { *sx = *sy = 0; return 0; }
    *sx = (int)lroundf(dx / r * DT_RAD_FULL); *sy = (int)lroundf(-dy / r * DT_RAD_FULL);
    return 1;
}
// The direction the wheel's onFrame makes of that stick (radians, screen axes: 0 = right, +pi/2 = down) - for the test: the engine's H = x,
// V = -y (npad y up -> the event "Down" is y < 0)
static inline float dt_radial_angle(int sx, int sy) { return atan2f((float)-sy, (float)sx); }
// The pad's left stick for an open wheel: while the right stick rests (inside dead, npad units) and the left one is past it, the left stick becomes
// the right one and the left one rests (the character does not walk under the wheel). Returns 1 when it moved them.
static inline int dt_radial_lstick(int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry, int dead) {
    const int64_t r2 = (int64_t)*rx * *rx + (int64_t)*ry * *ry, l2 = (int64_t)*lx * *lx + (int64_t)*ly * *ly, d2 = (int64_t)dead * dead;
    if (r2 > d2 || l2 <= d2) return 0;
    *rx = *lx; *ry = *ly; *lx = *ly = 0;
    return 1;
}
// The choose: from the moment the host asks (`ask` set), the triggers the player holds (phys & ZL|ZR mask `trig`) are held back from the game until
// the player lets them go. *sup = the held-back mask (state kept by the caller); returns the buttons the game sees.
static inline uint64_t dt_radial_release(uint64_t phys, int ask, uint64_t trig, uint64_t *sup) {
    if (ask) *sup = phys & trig;
    if (*sup && !(phys & *sup)) *sup = 0;   // the player let go: nothing to hold back any more
    return phys & ~*sup;
}
// The wheel's centre in window px: the movie's FlashPlayer rect (x, y, w, h: +0x10..+0x1c) fits its stage sw x sh uniformly and centred, the
// wheel sits at stage (wx, wy). 0 = no usable rect.
static inline int dt_radial_centre(const int32_t f[4], float sw, float sh, float wx, float wy, float *cx, float *cy) {
    if (f[2] < 16 || f[3] < 16 || sw < 16 || sh < 16) return 0;
    const float k = fminf((float)f[2] / sw, (float)f[3] / sh);
    *cx = (float)f[0] + ((float)f[2] - sw * k) / 2 + wx * k; *cy = (float)f[1] + ((float)f[3] - sh * k) / 2 + wy * k;
    return 1;
}
#endif
