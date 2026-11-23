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
// UI mode TOUCH (owner 23.11: "a tap should only pick the direction / target, the cast should happen on the confirm button"): is something
// prepared that waits for a target? Two task facts of the character's input controller (hle_dos2_touch_world.c tw_update reads them every frame):
//   - DT_TASK_SKILL (3, made at main+0xe23e50, vtable main+0x29c9e98): run 21.11 shows it previewed AND running from the hot bar's SlotPressed
//     until the cast, even while the cursor is on the UI (the CONFIRM circle) - a prepared skill, wherever it was taken from (the hot bar's own
//     mark showActiveSkill(slot) dropped to -1 the moment a finger landed on the world, run 21.11 27960, and the tap then cast at once);
//   - DT_TASK_AIM (2, the attack / skill-cast task) previewed while NOTHING is under the cursor: run 23.11 "ActionAttackGround" previews it over
//     the bare ground, as the inventory "Throw" does (dos2-touch6); over the ground an unprepared PC cursor previews the walk (0), so type 2
//     there means a target mode. Over a character it may be the plain attack of the hover, so it only keeps a mode seen before (the latch).
// dt_aim_latch: the target-mode latch after one frame. obj_under: 1 a character / item under the cursor, 0 nothing, -1 unknown (no fresh pick).
// The latch drops on any other known preview (the walk, talk, use ...: the mode ended); -1 / the skill task keep it.
#define DT_TASK_SKILL 3
static inline int dt_aim_latch(int latched, int preview_type, int obj_under) {
    if (preview_type == DT_TASK_AIM) return latched || obj_under == 0;
    if (preview_type >= 0 && preview_type != DT_TASK_SKILL) return 0;
    return latched;
}
// After the host sent CONFIRM / cancel for that controller the facts may linger a few frames while the cast starts (or stay while it runs):
// they count again only after one frame without either (blocked -> 0). 1 = this frame shows a prepared state (skill task or latch).
static inline int dt_aim_frame(int preview_type, int running_type, int latched, int *blocked) {
    const int seen = preview_type == DT_TASK_SKILL || running_type == DT_TASK_SKILL || preview_type == DT_TASK_AIM || running_type == DT_TASK_AIM;
    if (*blocked) { if (!seen) *blocked = 0; return 0; }
    return preview_type == DT_TASK_SKILL || running_type == DT_TASK_SKILL || latched;
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
// ---- UI mode TOUCH (branch dos2-uimodes, owner 19.11: "touch = the iPad's touchscreen_1.json exactly"; docs/dos2/TOUCH_MODES_PLAN.md section 2
// and "Implemented: touch gestures"). The iPad's gesture events (touchscr:tap / dtap / slongpress / longpress / pan / pan2f / rotate / pinch /
// tap2f / tap3f / swipeup3f / swipedown3f / longpress2f / 3f) do not exist in the Switch main (no "touchscr" device, no raw name for them, the
// event names DoubleTap / ShortLongPress / TacticalCameraOn / Off / LongPress2Fingers are not even strings of main). TOUCH runs the engine's input
// mode 0 with PlayerProfiles/Eoc/keyboard_1.json (the Switch Game.pak copy = the iPad's minus the mouse buttons, which Default/inputconfig.json
// binds), so every iPad event becomes its PC twin made of mouse / keyboard records. These rules decide WHICH gesture a set of fingers is; the
// host (hle_dos2_touch.c, "UI mode TOUCH") turns the result into records. Times / distances come from the host (env-tunable, iPad-like defaults).
//   one finger: tap (up before the short long press, moved <= slop) | short long press (still >= slong_ms: the tooltip = the PC hover, no click on
//   the release) | long press (still >= long_ms: the right click = ContextMenu) | pan (moved past the slop before the long press: CameraMove);
//   double tap = a second tap that lands within dtap_ms of the first one's release and within r px of it;
//   two fingers: pinch (distance) / rotate (angle) / pan (midpoint), any of them, plus the two-finger tap and long press;
//   three fingers: vertical swipe (TacticalCamera on = down / off = up), the three-finger tap and long press.
enum { DT_T1_WAIT = 0 /* still, not decided */, DT_T1_SLONG = 1 /* short long press reached */, DT_T1_LONG = 2 /* long press reached */,
       DT_T1_PAN = 3 /* moved: camera pan */ };
// The state of an undecided one-finger TOUCH world finger: moved = px from where it landed, held = ms since it landed. The pan wins over the
// time while the long press has not fired (the caller stops asking once it is PAN or LONG).
static inline int dt_t1_state(float moved, uint64_t held_ms, float slop, int slong_ms, int long_ms) {
    if (moved > slop && held_ms < (uint64_t)(long_ms > 0 ? long_ms : 0)) return DT_T1_PAN;
    if (long_ms > 0 && held_ms >= (uint64_t)long_ms) return DT_T1_LONG;
    if (slong_ms > 0 && held_ms >= (uint64_t)slong_ms) return DT_T1_SLONG;
    return DT_T1_WAIT;
}
// UI mode TOUCH (owner 23.11: "no Move for an item on the ground, no way to put it elsewhere"): a one-finger world move that would be the
// camera pan is the PC's own item drag (left held on the item, the cursor follows, the release places it) when the picking had an ITEM under
// the spot where the finger landed. The iPad's own tutorial (TUT_MoveItem on iPad: "Move an object by tapping it, and then dragging it") drags
// at once, so hold_ms is 0 by default (owner 23.11: "убирай" the delay); DM_DOS2_TOUCH_T_ITEMDRAG=2 keeps the 23.11 hold of the short long
// press first. 1 = drag the item, 0 = the pan as before.
static inline int dt_t1_item_drag(int state, uint64_t held_ms, int hold_ms, int item_at_landing) {
    return state == DT_T1_PAN && item_at_landing && hold_ms >= 0 && held_ms >= (uint64_t)hold_ms;
}
enum { DT_REL_TAP = 0 /* a click */, DT_REL_TIP = 1 /* a short long press ended: tooltip only, no click */, DT_REL_NONE = 2 /* moved: nothing */ };
// What the release of a still one-finger touch (no pan, no long press fired) is.
static inline int dt_t1_release(float moved, uint64_t held_ms, float slop, int slong_ms) {
    if (moved > slop) return DT_REL_NONE;
    return slong_ms > 0 && held_ms >= (uint64_t)slong_ms ? DT_REL_TIP : DT_REL_TAP;
}
// UI mode TOUCH, a finger on an open PC window (skills / inventory / crafting ...: no world probe, the press waits for the finger; owner 21.11:
// "not clear how to put a spell onto the hot bar" - a hold first, then a move, started no drag: the long press had fired the right click while
// the finger was still down). With the drag rule on (DM_DOS2_TOUCH_T_LIFT_LONG, default 1) a hold fires nothing while the finger is down (the
// cursor rests there: the PC hover tooltip); a move past the slop after ANY hold is a drag (the press at the landing point, then the motion);
// a still release decides: tap (click) / short long press (tooltip only) / long press (the right click = the PC context menu, on the lift).
enum { DT_PREL_TAP = 0, DT_PREL_TIP = 1, DT_PREL_RCLICK = 2, DT_PREL_NONE = 3 /* moved: the drag ended (or nothing) */ };
static inline int dt_panel_release(float moved, uint64_t held_ms, float slop, int slong_ms, int long_ms) {
    if (moved > slop) return DT_PREL_NONE;
    if (long_ms > 0 && held_ms >= (uint64_t)long_ms) return DT_PREL_RCLICK;
    return slong_ms > 0 && held_ms >= (uint64_t)slong_ms ? DT_PREL_TIP : DT_PREL_TAP;
}
// Is a new finger down the second tap of a double tap: prev_up = when the previous TAP ended (0 = none), px, py its point; t, x, y the new one.
static inline int dt_is_dtap(uint64_t prev_up, float px, float py, uint64_t t, float x, float y, int dtap_ms, float r) {
    if (!prev_up || t < prev_up || t - prev_up > (uint64_t)(dtap_ms > 0 ? dtap_ms : 0)) return 0;
    return hypotf(x - px, y - py) <= r;
}
// Two fingers: the change from the reference pair a (x0, y0, x1, y1) to the pair b: distance change dd (px, + = apart), angle change da (radians,
// wrapped to -pi..pi, + = clockwise on screen: y points down), midpoint motion mx, my (px).
static inline void dt_pair_delta(const float a[4], const float b[4], float *dd, float *da, float *mx, float *my) {
    const float da0 = atan2f(a[3] - a[1], a[2] - a[0]), db0 = atan2f(b[3] - b[1], b[2] - b[0]);
    float d = db0 - da0;
    while (d > 3.14159265f) d -= 6.28318531f;
    while (d < -3.14159265f) d += 6.28318531f;
    *dd = hypotf(b[2] - b[0], b[3] - b[1]) - hypotf(a[2] - a[0], a[3] - a[1]);
    *da = d;
    *mx = (b[0] + b[2]) / 2 - (a[0] + a[2]) / 2; *my = (b[1] + b[3]) / 2 - (a[1] + a[3]) / 2;
}
enum { DT_2F_PINCH = 1, DT_2F_ROT = 2, DT_2F_PAN = 4 };
// Which two-finger components run: on = the mask so far; dd / arc / dm = how far each has gone since the gesture began (distance change, the
// rotation as an arc = |angle| x half the finger distance, midpoint motion; px). A component starts past its step; once one runs, another one
// starts only past twice its step (the fingers of a pinch drift a little, a pan changes the distance a little). The iPad runs its pinch,
// rotation and pan recognizers at the same time - so does this, after the hysteresis.
static inline unsigned dt_two_mask(unsigned on, float dd, float arc, float dm, float pinch_step, float rot_step, float pan_step) {
    const float k = on ? 2.f : 1.f;
    unsigned m = on;
    if (!(m & DT_2F_PINCH) && fabsf(dd) > pinch_step * k) m |= DT_2F_PINCH;
    if (!(m & DT_2F_ROT) && fabsf(arc) > rot_step * k) m |= DT_2F_ROT;
    if (!(m & DT_2F_PAN) && fabsf(dm) > pan_step * k) m |= DT_2F_PAN;
    return m;
}
// A multi-finger tap: nmax = the most fingers that were down together, dur = ms from the first down to the last up, moved = the largest
// distance any finger went, started = a pinch / rotation / pan / swipe ran. Returns nmax (2 or 3) for a tap, else 0.
static inline int dt_tapn(int nmax, uint64_t dur_ms, float moved, int started, int tap_ms, float slop) {
    if (started || nmax < 2 || nmax > 3 || dur_ms > (uint64_t)(tap_ms > 0 ? tap_ms : 0) || moved > slop) return 0;
    return nmax;
}
// A three-finger swipe: dx, dy = the motion of the fingers' centre since three were down (px). +1 = down (TacticalCameraOn), -1 = up (Off).
static inline int dt_swipe3(float dx, float dy, float min_d) {
    if (!(fabsf(dy) > min_d) || fabsf(dy) < 1.5f * fabsf(dx)) return 0;
    return dy > 0 ? 1 : -1;
}
// The iPad's TacticalCameraOn / Off against the PC's one toggle key (keyboard_1.json ToggleTacticalCamera <- key:o): *state = what the host
// believes the camera is (0 off, 1 on); dir +1 = on wanted, -1 = off wanted. 1 = press the toggle now (and *state follows).
static inline int dt_tac_press(int *state, int dir) {
    const int want = dir > 0;
    if (dir == 0 || *state == want) return 0;
    *state = want;
    return 1;
}
// The keyboard camera (keyboard_1.json CameraForward / Backward / Left / Right <- w / s / a / d; CameraRotateLeft / Right <- delete / end): the
// finger's motion becomes a hold time of a key. *b = the signed hold time (ms) one axis still owes; d = the new motion (px for a pan, radians
// for a rotation), unit = px of a screen height (or 1 for radians), ms_per = ms of hold per unit, cap = most ms owed. A motion against the
// owed direction cancels it first (the camera stops at once when the finger turns).
static inline void dt_hold_add(float *b, float d, float unit, int ms_per, int cap_ms) {
    if (!(unit > 0) || ms_per <= 0) return;
    const float add = d / unit * (float)ms_per, cap = (float)(cap_ms > 0 ? cap_ms : 0);
    if ((*b > 0 && add < 0) || (*b < 0 && add > 0)) *b = 0;
    *b += add;
    if (*b > cap) *b = cap;
    if (*b < -cap) *b = -cap;
}
// time passes: the owed hold shrinks towards 0 by ms
static inline void dt_hold_tick(float *b, float ms) {
    if (*b > 0) { *b -= ms; if (*b < 0) *b = 0; }
    else if (*b < 0) { *b += ms; if (*b > 0) *b = 0; }
}
// -1 / 0 / +1: the key of the negative / no / positive direction is down (a hold shorter than eps ms is no hold)
static inline int dt_hold_dir(float b, float eps) { return b > eps ? 1 : b < -eps ? -1 : 0; }
enum { DT_K_W = 1, DT_K_S = 2, DT_K_A = 4, DT_K_D = 8, DT_K_ROTL = 16, DT_K_ROTR = 32 };
// The keys of a pan: bx / by = the owed hold of the finger's x / y motion (+ = the finger went right / down). The camera follows the finger as
// a grabbed map does: the finger right = the view goes left (a), the finger down = the view goes forward (w). invert flips both
// (DM_DOS2_TOUCH_T_PAN_INV). Rotation: br + = the fingers turned clockwise = CameraRotateRight (end), swap flips (DM_DOS2_TOUCH_ROT_SWAP).
static inline unsigned dt_cam_keys(float bx, float by, float br, int invert, int swap, float eps) {
    const int x = dt_hold_dir(bx, eps) * (invert ? -1 : 1), y = dt_hold_dir(by, eps) * (invert ? -1 : 1), r = dt_hold_dir(br, eps) * (swap ? -1 : 1);
    return (x > 0 ? DT_K_A : x < 0 ? DT_K_D : 0) | (y > 0 ? DT_K_W : y < 0 ? DT_K_S : 0) | (r > 0 ? DT_K_ROTR : r < 0 ? DT_K_ROTL : 0);
}
// ---- review of the phone run 20.11 (docs/dos2/TOUCH_MODES_PLAN.md "Прогон 20.11"; host test dm/host/tests/dos2_touch_t_gesture_test.c) ----
// UI mode TOUCH: the keyboard twin of a host pad pulse. The mode byte is 0 there (the pad's records are filtered for most listeners) and the
// bindings are Eoc/keyboard_1.json (no c:a / c:b), so a pad pulse did nothing; its key does what the pad's button means in a menu
// (keyboard_1.json of the iPad = the Switch's minus the mouse buttons). Raw key ids of the name table main+0x2974b10 (index - 2, from main.raw's
// .rela.dyn). Button bits = dm_input.h DM_BTN_* (nn::hid::NpadButton). DT_PADKEY_NONE = no sure twin: nothing is sent.
enum { DT_PADKEY_NONE = 0xffff, DT_KEY_RETURN = 0x00, DT_KEY_ESCAPE = 0x01, DT_KEY_M = 0x30, DT_KEY_RIGHT = 0x56, DT_KEY_LEFT = 0x57, DT_KEY_DOWN = 0x58, DT_KEY_UP = 0x59 };
static inline uint32_t dt_pad_key(uint64_t mask) {
    if (mask & (1u << 0)) return DT_KEY_RETURN;    // A     -> UIAccept <- key:return
    if (mask & (1u << 1)) return DT_KEY_ESCAPE;    // B     -> UICancel / ToggleInGameMenu <- key:escape
    if (mask & (1u << 13)) return DT_KEY_UP;       // up    -> UIUp <- key:up
    if (mask & (1u << 15)) return DT_KEY_DOWN;     // down  -> UIDown <- key:down
    if (mask & (1u << 12)) return DT_KEY_LEFT;     // left  -> UILeft <- key:left
    if (mask & (1u << 14)) return DT_KEY_RIGHT;    // right -> UIRight <- key:right
    if (mask & (1u << 11)) return DT_KEY_M;        // '-'   -> ToggleMap <- key:m
    return DT_PADKEY_NONE;                          // X / Y / L / R / ZL / ZR / + / sticks: no sure twin
}
// UI mode TOUCH, every pad poll: the held camera keys (pan w / a / s / d, rotation delete / end - delete is UIDelete too -, zoom pageup /
// pagedown, the alt / shift toggles) are let go and the action plate hidden while this is true. st = hle_dos2_touch.c g_ui_state: bit 2 known,
// bit 1 in game, bit 0 a menu / panel / dialog / modal window is up
static inline int dt_t_away(int st) { return (st & 4) && ((st & 1) || !(st & 2)); }
// UI mode TOUCH (mode byte 0): the PC windows that make a finger the UI's (a menu for the gestures; hle_dos2_touchui_route.c dm_dos2_route_world_ok):
// inventory 0x74, character sheet 0x77, skills 0x29, crafting 0x66, journal / map 0x16, examine 0x68, trade 0x2e, waypoints 0x2f, dialog 0xe,
// reward 0x88, item splitter 0x15, the PC context menus 0xa / 0xb. Not the HUD (hot bar 0x28, minimap 0x1e, portraits 0x26, status console
// 0x75, combat log 7, the right side 0x8d), not the tooltip 0x2c / worldTooltip 0x30, not the tutorial box 0x37 (an overlay, as 0x5e)
// UI mode TOUCH, a finger whose world probe press a HUD movie took (hot bar slot, portrait, minimap ...; the press is down already) and that has
// not moved past the slop: what the time held means. DT_TUI_CLICK = a tap (the release clicks there), DT_TUI_TIP = the short long press (the
// hover tooltip only: the release must not click - it is sent with the cursor off the element), DT_TUI_LONG = the long press (the left is let go
// off the element, then the right click at the finger: ContextMenu / the portrait's openCharInventory). long_ms <= slong_ms = no tooltip step.
enum { DT_TUI_CLICK = 0, DT_TUI_TIP = 1, DT_TUI_LONG = 2 };
static inline int dt_tui_state(uint64_t held_ms, int slong_ms, int long_ms) {
    if (held_ms >= (uint64_t)(long_ms > 0 ? long_ms : 0)) return DT_TUI_LONG;
    return slong_ms > 0 && slong_ms < long_ms && held_ms >= (uint64_t)slong_ms ? DT_TUI_TIP : DT_TUI_CLICK;
}
// Where the left is let go after a still HUD hold (the tooltip): above the window (off_y < 0: no element there, so no click), at the finger's
// OWN x. Phone run 23.11 ("a long press on a hot bar icon also flips the skill deck"): the park was (-64, -64) and hotBar_p's swipe over the
// slots (dmSwUp: the x at the release - the x at the press > 130 stage px = the next / previous deck) took that jump for a swipe - nextHotbar
// after every short long press on a slot. With x kept, a hold measures no travel; a real swipe moved the finger and is no hold (tui 0).
static inline void dt_tui_park(float finger_x, float off_y, float *px, float *py) { *px = finger_x; *py = off_y < 0 ? off_y : -64.0f; }
static inline int dt_pc_menu_type(uint32_t ty) {
    switch (ty) { case 0x74: case 0x77: case 0x29: case 0x66: case 0x16: case 0x68: case 0x2e: case 0x2f: case 0x0e: case 0x88: case 0x15: case 0x0a: case 0x0b: return 1; default: return 0; }
}
// Phone run 21.11 ("a button under the open window gets pressed, random windows open"): a PC window answers "not handled" for a press on its
// background (Iggy hits only elements with mouse code), so the UI manager's walk handed the press on, as THE TARGET, to everything below it in
// the walk (minimap, overhead, combat log, the world). The windows that stop the mouse for what lies below them (hle_dos2_touchui_route.c):
// the PC panels of dt_pc_menu_type and the containers 9 / 0x25 - not the small PC context menus 0xa / 0xb (a click beside them is the PC's
// own "close it and act there").
static inline int dt_pc_blocker(uint32_t ty) { return (dt_pc_menu_type(ty) && ty != 0x0a && ty != 0x0b) || ty == 0x09 || ty == 0x25; }
// The walk visits the object list from its LAST entry (the top) to the first. Which index is the lowest visible blocker (-1 = none): every
// object below it gets "an upper object took it" for a mouse event; the blockers above it (two halves of one view) still take their turn.
static inline int dt_lowest_blocker(const uint32_t *types, const uint8_t *visible, int n) {
    for (int i = 0; i < n; i++) if (visible[i] && dt_pc_blocker(types[i])) return i;
    return -1;
}
#endif
