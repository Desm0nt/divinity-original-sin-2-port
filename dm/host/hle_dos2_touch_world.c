// [DOS2] DM_DOS2_TOUCH_WORLD=3: PC-style world clicks by touch while the game stays in controller mode (docs/dos2/TOUCH_CONSOLE_PLAN.md section 7).
// Static analysis only (capstone + Ghidra headless on a private copy; main 4979b200, offsets of main.raw); not run on a phone yet.
//
// What the engine does (PC path, all of it is in the Switch build):
//   - ecl::InputController (vtable main+0x29c9738; CharacterInputController main+0x29c9860 shares these methods) owns the character tasks:
//     [ic+0x38] task array (0x19 tasks made by main+0xe15b80, base ctor main+0xe19860(task, type, priority): type at task+0x10, flags at +0x14),
//     [ic+0x58] the running task, [ic+0x60] the preview task (what a click would do: path preview, AP cost, cursor), [ic+0x24] click start (px).
//   - OnInputEvent main+0xe13830(ic, event): event id at +0 (Action1 = 4, bound to mouse:left in Default/inputconfig.json), pressed at +0x20.
//     Every task gets the event first (task vt+0x88); Action1 down stores the cursor [main+0x2bb88e8] as the click start. There is NO test of
//     the controller mode byte here.
//   - Update main+0xe11f40(ic, time): each frame picks the preview task (main+0xe14dd0 -> each task's priority / CanEnter against the picking
//     results), enters / updates its preview and sets the cursor. Four reads of the mode byte, none of them stops the PC path.
//   - The difference between PC and controller mode is the PICKING: PlayerPickingHelper update main+0xfab260(helper) tests the mode byte at
//     main+0xfab2c4: controller mode = the pad's proximity selection (main+0x10a0d30, no screen point at all), PC mode = ray from a screen point.
//     The ray set-up main+0xfa9f50(helper) tests it again at main+0xfa9fdc: PC = [helper+0xe4] (float2) = the UI cursor (main+0xfaa494), controller
//     = the projection of the selection's world point. Results: [helper+0x80] the object under the point (handle; type index in the top bits:
//     character [main+0x32bb8d8], item [main+0x32bba10]), [helper+0x70] character, [helper+0x78] item, [helper+0x08] world position (float3),
//     [helper+0x50] the walk point with [helper+0xa0] = "walkable" (task 0 = move: main+0xe18790 takes the point, main+0xe18c80 arms it on the
//     release of Action1, the next Update starts it; main+0xe186d0 refuses it in controller mode in some character states).
//   So a mouse click in controller mode acted on what the PAD had selected (run tw1: chests near the character worked, the floor never did).
//
// What this file does (only while hle_dos2_touch.c says a world finger / world click is live; the pad is untouched otherwise):
//   1. The picking (main+0xfab260, main+0xfa9f50) and the input controller's Update (main+0xe11f40) run with the mode byte = 0 for the length of
//      the call and get it back on return: the picking follows the UI cursor = the finger, the preview task is the PC one (hover outline, path
//      preview, AP cost - whatever the PC shows under the mouse).
//   2. Action1 made by a touch left record: a PROBE press (the UI is asked who takes it) or any other UI gesture never reaches the input
//      controller (the hook answers "not handled"); the CLICK press / release runs OnInputEvent with the mode byte = 0 = the PC click:
//      ground = move, NPC = talk, enemy = attack, item = pick up / use, container = open.
//   The mode byte is global (670 readers): for the microseconds of those calls another thread could read 0. Known risk, hence the switch.
//   3. (run tw2, branch dos2-world2) The floor: in the normal, character-bound controller mode a tap picked the right point but the character
//      did not walk; in the "wisp" (free cursor) mode it did. Cause: the move task (type 0, vtable main+0x29c9a30) REFUSES in its priority
//      function main+0xe18790 when the input controller is the pad's ([ic+0x50] bit 0) and main+0xe151f0(ic) = "this controller targets with a
//      cursor now" (wisp mode main+0x109b500, a prepared skill, combat, [ic+0x20]) answers 0; the pad's own movement task (type 25, vtable
//      main+0x29cb7c8, made at main+0xe16b04, priority main+0xe40cb0) has the mirrored test and takes the preview with priority 10 instead.
//      That is the console design ([ic+0x50] = copy of the mode byte, set by main+0xe11c20: the pad walks by stick only, a point only where it
//      has a cursor). The iPad build has the same two tests (0x101503a54 / 0x1017eb660) but runs with the flag 0 and binds Action1 <- touchscr:tap
//      (touchscreen_1.json): a tap on the floor walks there in the normal camera. We mirror the iPad.
//      No other task has that test (talk = 4 and item = 6 worked in tw2). The start of a task (main+0xe14760) asks the same priority function
//      again, so the answer must hold for the Update after the click too; the running move task (update main+0xe1ab50, "go on" main+0xe18ea0)
//      has no such test. Fix: main+0xe151f0 answers 1 while it is called from inside one of OUR PC calls (the hooked Update in the window, the
//      click's OnInputEvent; same thread only) = exactly the state of the wisp mode in which the click was seen to work. Nothing is written into
//      the game's objects. DM_DOS2_TOUCH_WORLD_FLOOR=0: without it (as pack 2y); DM_DOS2_TOUCH_WORLD_KEEP=1: the answer also holds in every
//      Update of the clicked controller while its click-started move task runs (only if the walk stops when the window closes).
//   4. (owner 16.11, branch dos2-touch5) The item placement mode (Actions... -> Move: the object follows the cursor, pad A places it). That is the
//      input controller's item-move task running: PC task type 10 (made by main+0xe323d0, vtable main+0x29ca7a8; its cursor function
//      main+0xe32b20 sets CursorItemMove = cursor id 19 at main+0xe32cd8, its OnInputEvent main+0xe32d00 places on an Action1 RELEASE and
//      cancels on ActionCancel 0xdf; pad A = Action1 <- c:a in Default/inputconfig_controller.json). Our PC click there went through the
//      generic Action1 path with the mode byte 0: the preview task (move to the point) started on the release and the walk cancelled the move.
//      tw_update notes, from the call it already gets, whether a controller runs task type 10 (two loads; no extra hook, no polling of our
//      own); while it does, its Update runs untouched (mode byte as it is, no FLOOR answer) and a world tap is "pad A at the finger":
//      hle_dos2_touch.c keeps the PC picking window open (the picking - the point the move task reads - follows the finger) and presses the
//      pad's A, exactly what a pad player does with the cursor there. DM_DOS2_TOUCH_PLACE=0: off (the tap is the PC click, as before).
//   5. (phone 16.11, touch6.log, branch dos2-place) Point 4 was wrong about the point: opening the PC picking for the tap killed the item-move
//      preview (10 -> 0, the move task) and the PC ray then hit the moved barrel itself (pad A = "use the item", task 6). What the item move
//      really follows in pad mode (main.raw): the picking update main+0xfab260 in controller mode runs the proximity selection main+0x10a0d30
//      on the player's SELECTOR object [helper+0x108] (0x50 bytes, vtable main+0x29e1fa0, made in the helper's constructor at main+0xfa8e7c;
//      world position at +0x30 float3, written by the selector itself at main+0x10a0cbc / 0x10a115c / 0x10a1818) and then tail-calls the
//      ray set-up main+0xfa9f50, whose controller path projects [[helper+0x108]+0x30] to the screen (main+0x911120 -> helper+0xe4) and casts
//      the ray from there (main+0x9113a0): the walk point helper+0x50 is where the drag (WorldDragController main+0x1407dc0 -> main+0x1409da0,
//      the walk point read at main+0x140aa20) and the item-move task put the item; the left stick moves the selector (SelectorMove*).
//      So a placement tap now never opens the PC picking window: (a) ONE ray set-up call (two, the first may still be last frame's) runs from
//      the UI cursor = the finger to learn the ground point under it (helper+0x50, else +0x08), (b) from then on, before every ray set-up of
//      that helper, the selector's position is that point - the pad's own pipeline then sees "the player moved the cursor there": proximity
//      selection, preview 10, the ghost, (c) hle_dos2_touch.c presses pad A once the preview is the item-move task again, the selector is held
//      400 ms more. DM_DOS2_TOUCH_PLACE=2: the point 4 way (PC picking at the finger + A), for A/B only.
//   6. (owner 18.11, branch dos2-touch6) Inventory "Throw" (and any other aim mode of the same task): a tap on the ground made the character run
//      there and cancelled the throw; only the stick + A worked - the class of point 4 / 5. The throw's targeting is the attack / skill task
//      (PC task type 2, main+0xe1de50, vtable main+0x29c9cf0; "Throw Item" = context menu action 0x1b, main+0xfcd3f8). While a controller
//      previews or runs a task whose type is in DM_DOS2_TOUCH_AIM_TYPES (default "2") it is in the point-5 path: no PC picking window, no mouse
//      record for a world finger, a tap = the selector at the finger's ground point + pad A (the pad player's "aim there, A"). A prepared skill is
//      the same task, so its tap goes the pad's way too (the selector's proximity selection picks the character nearest to the point).
//      DM_DOS2_TOUCH_AIM=0: off (the PC click, as before). The type is a static finding; DM_DBG logs "aim mode ... previews task type N".
// DM_DBG=1: each change of the object under the finger, each Action1 with its kind, each change of the preview / running task (type numbers).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include "hle_dos2_touch_gesture.h"

enum { TW_INPUT = 0xe13830, TW_INPUT_EV = 0xe13860, TW_INPUT_DOWN = 0xe1386c, TW_INPUT_A1 = 0xe13908, TW_UPDATE = 0xe11f40, TW_UPDATE_MODE = 0xe121e4,
       TW_PICK = 0xfab260, TW_PICK_MODE = 0xfab2c4, TW_PICKC = 0xfa9f50, TW_PICKC_MODE = 0xfa9fd8, TW_PICKC_CUR = 0xfaa494, TW_TASK_CTOR = 0xe198c0,
       TW_CURSOR = 0xe151f0, TW_MOVE_GATE = 0xe187c8, TW_PAD_GATE = 0xe40cd8, TW_PAD_CTOR = 0xe16b04, TW_TASK_MOVE = 0, TW_TASK_PAD = 25,
       TW_MODE_INIT = 0x8a8eb8, TW_MODE = 0x2bb8918, TW_CURSOR_PTR = 0x2bb88e8 /* -> the UI cursor float2 (main+0xfaa494 reads it) */, TW_CHAR_TYPE = 0x32bb8d8, TW_ITEM_TYPE = 0x32bba10, TW_EV_ACTION1 = 4,
       TW_IC_PAD = 0x50, TW_IC_PRESS = 0x48, TW_IC_TASK = 0x58, TW_IC_PREVIEW = 0x60, TW_TASK_TYPE = 0x10, TW_TASK_FLAGS = 0x14,
       TW_H_POS = 0x08, TW_H_WALK = 0x50, TW_H_WALK_OK = 0xa0, TW_H_CHAR = 0x70, TW_H_ITEM = 0x78, TW_H_OBJ = 0x80, TW_H_FLAGS = 0xa0, TW_H_PLAYER = 0xb0, TW_H_SCREEN = 0xe4 };
static const uint32_t k_tw_input[6] = {0xd10183ff /* sub sp,sp,#0x60 */, 0xa9025ff8, 0xa90357f6, 0xa9044ff4, 0xa9057bfd, 0x910143fd};
static const uint32_t k_tw_input_ev[1] = {0xb9400036 /* ldr w22,[x1]: the event id */};
static const uint32_t k_tw_input_down[1] = {0x39408037 /* ldrb w23,[x1,#0x20]: pressed */};
static const uint32_t k_tw_input_a1[3] = {0x54ffff01, 0x710012df /* cmp w22,#4: Action1 */, 0x540006c0};
static const uint32_t k_tw_update[8] = {0xd10283ff /* sub sp,sp,#0xa0 */, 0xa9046ffc, 0xa90567fa, 0xa9065ff8, 0xa90757f6, 0xa9084ff4, 0xa9097bfd, 0x910243fd};
static const uint32_t k_tw_update_mode[3] = {0xd000ed28 /* adrp x8, main+0x2bb8000 */, 0x39646108 /* ldrb w8,[x8,#0x918]: the mode byte */, 0x34004a88};
static const uint32_t k_tw_pick[6] = {0xd10543ff /* sub sp,sp,#0x150 */, 0x6d0b3bef, 0x6d0c33ed, 0x6d0d2beb, 0x6d0e23e9, 0xa90f6ffc};
static const uint32_t k_tw_pick_mode[2] = {0x39646108 /* ldrb w8,[x8,#0x918] */, 0x340007a8 /* cbz: PC = ray from the cursor, else the pad's selection */};
static const uint32_t k_tw_pickc[6] = {0xd10183ff, 0xa9025ff8, 0xa90357f6, 0xa9044ff4, 0xa9057bfd, 0x910143fd};
static const uint32_t k_tw_pickc_mode[3] = {0xf000e078 /* adrp x24, main+0x2bb8000 */, 0x39646308 /* ldrb w8,[x24,#0x918] */, 0x340025a8 /* cbz -> main+0xfaa494 */};
static const uint32_t k_tw_pickc_cur[5] = {0xd000e068, 0xf9447508 /* ldr x8,[x8,#0x8e8]: the UI cursor */, 0xf9400108, 0xf80e4268 /* stur x8,[x19,#0xe4] */, 0x17fffed8};
static const uint32_t k_tw_task_ctor[1] = {0x29020801 /* stp w1,w2,[x0,#0x10]: task type, priority */};
static const uint32_t k_tw_mode_init[1] = {0x3900c113 /* strb w19,[x8,#0x30]: the mode byte = 1 (x8 = main+0x2bb88e8) */};

static const uint32_t k_tw_cursor[8] = {0xd10283ff /* sub sp,sp,#0xa0 */, 0xf90023fb, 0xa90567fa, 0xa9065ff8, 0xa90757f6, 0xa9084ff4, 0xa9097bfd, 0x910243fd};
static const uint32_t k_tw_move_gate[4] = {0xf9409520 /* ldr x0,[x9,#0x128]: the character's input controller */, 0x39414008 /* ldrb w8,[x0,#0x50] */,
                                           0x36000128 /* tbz w8,#0: not the pad's -> go on */, 0x97fff287 /* bl main+0xe151f0; 0 = the move task refuses */};
static const uint32_t k_tw_pad_gate[4] = {0xf9409500 /* ldr x0,[x8,#0x128] */, 0x39414008, 0x36000fc8 /* tbz: not the pad's -> 0 */, 0x97ff5143 /* bl main+0xe151f0; 1 = the pad task refuses */};
static const uint32_t k_tw_pad_ctor[2] = {0xd2800328 /* mov x8,#0x19: task type 25 */, 0xf2c0c288 /* movk x8,#0x614,lsl #32: its flags */};

typedef struct { uint32_t off, n; const uint32_t *want; } tw_site;
typedef uint64_t (*tw_fn2)(uint64_t, uint64_t);
static module_t *g_main;
static tw_fn2 g_o_input, g_o_update, g_o_pickc, g_o_cursor;
static volatile uint64_t g_live_obj, g_live_ms;   // UI mode TOUCH: [helper+0x80] as the ray set-up found it with the mode byte 0 (the PC's own hover pick), and when
static volatile uint64_t g_pick_obj, g_pick_ms;   // pack 3y: the last pick under the finger (tw_pickc) and when
static int g_on, g_dbg;
static volatile uint8_t g_hover;   // read by the generated stub in front of main+0xfab260 too
static volatile uint8_t *g_mode;
static int g_kind_dn, g_kind_up; static uint64_t g_kind_dn_ms, g_kind_up_ms, g_click_ms;
static unsigned g_n_swallow, g_n_click, g_n_pass, g_n_pick, g_n_upd, g_n_cur, g_n_walk, g_n_keep;
static int g_floor = 1, g_keep;                 // DM_DOS2_TOUCH_WORLD_FLOOR / _KEEP
static volatile int g_pc_in; static pthread_t g_pc_thr;   // inside one of our PC calls (the game thread): main+0xe151f0 answers "cursor targeting"
static const uint8_t *g_click_ic, *g_walk_ic;   // the controller that got the click / whose click-started move task runs
static int g_place_on = 1;                        // DM_DOS2_TOUCH_PLACE (item placement taps, point 4 above)
static int g_aim_on = 1; static uint32_t g_aim_mask = 1u << DT_TASK_AIM;   // DM_DOS2_TOUCH_AIM / _AIM_TYPES (point 6: throw / aim taps)
static volatile int g_place_type = -1;            // the task type of the target mode last seen (10 = item move, else an aim type)
static volatile uint64_t g_place_ms;              // last Update of a controller running the item-move task (0 = never)
static const uint8_t *volatile g_place_ic;
static volatile int g_run_type = -1; static volatile uint64_t g_run_ms;
// point 5: the placement target (game thread only: the pad poll and the picking run on it)
enum { TW_H_SEL = 0x108, TW_SEL_POS = 0x30 };
static int g_pl_probe;                 // ray set-ups still to run from the finger (2, 1; 0 = none)
static int g_pl_have;                  // 1 = g_pl_pt holds the finger's ground point
static float g_pl_pt[3], g_pl_probe1[3];
static uint64_t g_pl_until;            // the selector is held at g_pl_pt until then
static const uint8_t *g_pl_helper;     // the helper the probe ran on (player 1's)
static unsigned g_n_pl_hold;   // DM_DBG: the running task type of the last controller that had one

static uint64_t tw_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000ull + (uint64_t)t.tv_nsec / 1000000ull; }
static const char *tw_kind(uint64_t h) {   // a handle's type index (top 10 bits) against the two entity types the picking knows
    if (!h) return "nothing";
    const uint32_t ty = (uint32_t)(h >> 54);
    return ty == *(const volatile uint32_t *)(g_main->base + TW_CHAR_TYPE) ? "CHARACTER" : ty == *(const volatile uint32_t *)(g_main->base + TW_ITEM_TYPE) ? "ITEM" : "object";
}
// pack 3y (owner 15.11: "a long press walks the character after the finger instead of opening the menu"): is a character or an item under the
// finger (the picking ran in the last 400 ms)? hle_dos2_touch.c then gives the long press the pad's X = ControllerContextMenu ("Actions...")
// instead of the follow-the-finger walk; on the floor the walk stays.
int dm_dos2_touch_world_obj_under(void);
int dm_dos2_touch_world_obj_under(void) {
    const uint64_t h = g_pick_obj;
    if (!h || tw_ms() - g_pick_ms > 400) return 0;
    const char *k = tw_kind(h);
    return k[0] == 'C' || k[0] == 'I';   // CHARACTER / ITEM
}
// UI mode TOUCH, the item drag (hle_dos2_touch.c dt_t1_item_drag): an ITEM (not a character) under the finger in the last 400 ms
int dm_dos2_touch_world_item_under(void);
int dm_dos2_touch_world_item_under(void) {
    const uint64_t h = g_pick_obj;
    return h && tw_ms() - g_pick_ms <= 400 && tw_kind(h)[0] == 'I';
}
// the same, only from a pick that ran at or after t (ms, CLOCK_MONOTONIC as tw_ms): the landing spot of a finger, not a stale pick from before it
int dm_dos2_touch_world_item_since(uint64_t t);
int dm_dos2_touch_world_item_since(uint64_t t) {
    const uint64_t h = g_pick_obj, ms = g_pick_ms;
    return h && ms >= t && tw_kind(h)[0] == 'I';
}
// hle_dos2_touchui_hud.c, the action plate's second source: the engine's own PC pick under the cursor right now. 1 a character / item, 0 none,
// -1 = the helper's ray set-up has not run with the mode byte 0 in the last second (hooks off, or not the PC input mode)
int dm_dos2_touch_world_obj_live(void);
int dm_dos2_touch_world_obj_live(void) {
    if (!g_on || !g_live_ms || tw_ms() - g_live_ms > 1000) return -1;
    const uint64_t h = g_live_obj;
    if (!h) return 0;
    const char *k = tw_kind(h);
    return k[0] == 'C' || k[0] == 'I';
}
static int tw_task_type(const uint8_t *task) { return task ? *(const volatile int32_t *)(task + TW_TASK_TYPE) : -1; }
static const char *tw_task_name(int t) {   // what run tw2 showed for a type (the rest are numbers only)
    return t < 0 ? "none" : t == TW_TASK_MOVE ? "move to the point" : t == TW_TASK_PAD ? "the pad's stick movement" : t == 4 ? "character: talk" : t == 6 ? "item: use / pick up / open" : "?";
}
// ---- UI mode TOUCH (owner 23.11, "a tap only picks the target, the confirm button casts"): is a skill / target mode prepared, and the aim ----
// point the picking is held at. The rules are hle_dos2_touch_gesture.h dt_aim_latch / dt_aim_frame (the evidence is written there); tw_update
// feeds them one controller frame at a time (game thread). hle_dos2_touchui_hud.c asks dm_dos2_touch_world_aiming() and shows CONFIRM / cancel.
// The hold: once the player aimed (a world tap / drag while prepared) the engine's own preview (target outline, area, AP cost, path) must stay
// at that point while the finger goes to CONFIRM - but a UI touch moves the one UI cursor, and the PC picking ray starts at it (main+0xfaa494:
// [[main+0x2bb88e8]] -> helper+0xe4). So for the length of each ray set-up call the cursor reads the aim point and gets its own value back on
// return (the game thread does both; the movies keep the real cursor). DM_DOS2_TOUCH_T_AIM_ENG=0: none of this (only the hot bar's own marks).
static const uint8_t *volatile g_aim_ic;          // the controller whose frames are counted
static volatile int g_aim_latch, g_aim_block, g_aim_eng = 1;
static volatile uint64_t g_aim_ms;                // the last frame that showed a prepared state (0 = none)
static volatile int g_aim_hold; static volatile uint64_t g_aim_hxy;   // the held aim point (two floats, window px)
static unsigned g_n_aimhold, g_n_aimf;
static int tw_obj_now(void) {   // under the cursor now: 1 a character / item, 0 nothing, -1 no pick in the last 300 ms
    const uint64_t now = tw_ms(), pm = g_pick_ms, lm = g_live_ms;
    const uint64_t h = pm >= lm ? g_pick_obj : g_live_obj, ms = pm >= lm ? pm : lm;
    if (!ms || now - ms > 300) return -1;
    if (!h) return 0;
    const char *k = tw_kind(h);
    return k[0] == 'C' || k[0] == 'I';
}
static void tw_aim_note(const uint8_t *ic, int pt, int rt) {
    if (!g_aim_eng) return;
    const int seen = pt == DT_TASK_SKILL || rt == DT_TASK_SKILL || pt == DT_TASK_AIM || rt == DT_TASK_AIM;
    if (ic != g_aim_ic) { if (!seen) return; g_aim_ic = ic; g_aim_latch = 0; g_aim_block = 0; }   // another controller with the facts: count that one
    int blocked = g_aim_block;
    const int latch = blocked ? 0 : dt_aim_latch(g_aim_latch, pt, tw_obj_now());
    const int on = dt_aim_frame(pt, rt, latch, &blocked);
    const int was = g_aim_ms && tw_ms() - g_aim_ms < 250;
    g_aim_latch = latch; g_aim_block = blocked;
    if (on) g_aim_ms = tw_ms();
    if (on != was && (g_dbg || g_n_aimf++ < 40)) LOG("[world] TOUCH aim: input controller %p previews task type %d, runs %d (object under the cursor %d): %s", (const void *)ic, pt, rt, tw_obj_now(),
                                                    on ? (pt == DT_TASK_SKILL || rt == DT_TASK_SKILL ? "a SKILL is prepared (task 3): a world tap aims, CONFIRM casts" :
                                                          "a TARGET MODE (task 2 over the ground: attack ground / throw): a world tap aims, CONFIRM acts") : "nothing prepared any more");
}
int dm_dos2_touch_world_aiming(void);   // hle_dos2_touchui_hud.c (game thread): 1 = the engine shows a prepared skill / target mode (last 250 ms)
int dm_dos2_touch_world_aiming(void) { return g_on && g_aim_eng && g_aim_ms && tw_ms() - g_aim_ms < 250; }
void dm_dos2_touch_world_aim_used(void);   // the host sent CONFIRM / cancel: the facts count again only after a frame without them
void dm_dos2_touch_world_aim_used(void) { g_aim_block = 1; g_aim_latch = 0; g_aim_ms = 0; }
void dm_dos2_touch_world_aim_hold(int on, float x, float y);   // hle_dos2_touch.c (an aim tap / drag) and hle_dos2_touchui_hud.c (aim over: 0)
void dm_dos2_touch_world_aim_hold(int on, float x, float y) {
    if (on) { uint32_t a, b; memcpy(&a, &x, 4); memcpy(&b, &y, 4); g_aim_hxy = (uint64_t)a << 32 | b; }
    if (g_dbg && (on != 0) != (g_aim_hold != 0)) LOG("[world] TOUCH aim: the picking %s", on ? "is HELD at the aim point (the preview stays there while a finger is on the UI)" : "follows the cursor again");
    g_aim_hold = on && g_aim_eng;
}
static uint64_t tw_pickc_orig(uint64_t helper, uint64_t a1) {   // the ray set-up, with the cursor at the held aim point for the call
    float *c = g_aim_hold && g_main ? *(float *volatile *)(g_main->base + TW_CURSOR_PTR) : NULL;
    if (!c) return g_o_pickc(helper, a1);
    const uint64_t v = g_aim_hxy; const uint32_t a = (uint32_t)(v >> 32), b = (uint32_t)v;
    const float s0 = c[0], s1 = c[1];
    memcpy(&c[0], &a, 4); memcpy(&c[1], &b, 4);
    const uint64_t r = g_o_pickc(helper, a1);
    c[0] = s0; c[1] = s1; g_n_aimhold++;
    return r;
}
static void tw_pc_enter(void) { if (g_floor) { g_pc_thr = pthread_self(); g_pc_in++; } }
static void tw_pc_leave(void) { if (g_floor) g_pc_in--; }
static uint64_t tw_cursor(uint64_t ic, uint64_t a1) {   // main+0xe151f0: "this input controller targets with a cursor now" (wisp mode, skill, combat)
    if (g_pc_in > 0 && ic && pthread_equal(pthread_self(), g_pc_thr)) {
        if (g_dbg) { static const void *last; static unsigned n;
            if ((const void *)(uintptr_t)ic != last && n < 40) { n++; last = (const void *)(uintptr_t)ic;
                LOG("[world] FLOOR: main+%#x (cursor targeting?) answers 1 for input controller %p inside the PC call (its pad flag [+%#x] = %u): the move task takes the "
                    "tapped point, the pad's stick task steps back - as in the wisp mode", TW_CURSOR, (const void *)(uintptr_t)ic, TW_IC_PAD, (unsigned)(((const volatile uint8_t *)(uintptr_t)ic)[TW_IC_PAD] & 1)); } }
        g_n_cur++; return 1;
    }
    return g_o_cursor(ic, a1);
}

static int tw_finite3(const float *v) { return v[0] == v[0] && v[1] == v[1] && v[2] == v[2] && fabsf(v[0]) < 1e6f && fabsf(v[1]) < 1e6f && fabsf(v[2]) < 1e6f; }
static float *tw_sel_pos(const uint8_t *h) { uint8_t *sel = *(uint8_t *volatile *)(h + TW_H_SEL); return sel ? (float *)(sel + TW_SEL_POS) : NULL; }
static uint64_t tw_place_pickc(uint64_t helper, uint64_t a1) {   // point 5 (only while a placement tap is in flight)
    uint8_t *h = (uint8_t *)(uintptr_t)helper;
    if (g_pl_probe > 0) {   // (a) the ray from the finger: the ground point under it
        const uint8_t old = *g_mode; *g_mode = 0;
        const uint64_t r = g_o_pickc(helper, a1);
        *g_mode = old;
        const float *w = (const float *)(h + TW_H_WALK), *p = (const float *)(h + TW_H_POS), *sc = (const float *)(h + TW_H_SCREEN);
        const float *src = h[TW_H_WALK_OK] && tw_finite3(w) && (w[0] != 0 || w[2] != 0) ? w : p;
        if (g_pl_probe == 2) memcpy(g_pl_probe1, src, sizeof g_pl_probe1);
        if (--g_pl_probe == 0) {
            if (tw_finite3(src) && (src[0] != 0 || src[2] != 0)) { memcpy(g_pl_pt, src, sizeof g_pl_pt); g_pl_have = 1; g_pl_helper = h; }
            { static unsigned n; if (g_dbg || n++ < 20) LOG("[world] placement: ray from the finger %.0f,%.0f px -> ground %.2f %.2f %.2f (%s; first call gave %.2f %.2f %.2f; under it %s %#llx)%s",
                (double)sc[0], (double)sc[1], (double)src[0], (double)src[1], (double)src[2], src == w ? "walk point" : "world point (not walkable)", (double)g_pl_probe1[0],
                (double)g_pl_probe1[1], (double)g_pl_probe1[2], tw_kind(*(const volatile uint64_t *)(h + TW_H_OBJ)), (unsigned long long)*(const volatile uint64_t *)(h + TW_H_OBJ),
                g_pl_have ? "" : " - NO usable point: placement tap dropped"); }
        }
        return r;
    }
    float *sel = tw_sel_pos(h);
    float before[3] = {0, 0, 0};
    if (sel) { memcpy(before, sel, sizeof before); memcpy(sel, g_pl_pt, sizeof g_pl_pt); }   // (b) the pad's cursor is at the finger's point
    const uint64_t r = g_o_pickc(helper, a1);
    g_n_pl_hold++;
    if (g_dbg) { static unsigned n; if (n < 60 && (g_n_pl_hold <= 3 || g_n_pl_hold % 10 == 0)) { n++; const float *w = (const float *)(h + TW_H_WALK), *sc = (const float *)(h + TW_H_SCREEN);
        LOG("[world] placement: selector %p %.2f %.2f %.2f -> %.2f %.2f %.2f (the finger's point), projected to %.0f,%.0f px, walk point now %.2f %.2f %.2f (%s), under it %s %#llx (hold #%u)",
            (void *)sel, (double)before[0], (double)before[1], (double)before[2], (double)g_pl_pt[0], (double)g_pl_pt[1], (double)g_pl_pt[2], (double)sc[0], (double)sc[1], (double)w[0], (double)w[1],
            (double)w[2], h[TW_H_WALK_OK] ? "walkable" : "not walkable", tw_kind(*(const volatile uint64_t *)(h + TW_H_OBJ)), (unsigned long long)*(const volatile uint64_t *)(h + TW_H_OBJ), g_n_pl_hold); } }
    return r;
}
static uint64_t tw_pickc(uint64_t helper, uint64_t a1) {   // main+0xfa9f50: the ray is set up from [helper+0xe4]; PC mode = the UI cursor
    if (helper && !g_hover && (g_pl_probe > 0 || (g_pl_have && (const uint8_t *)(uintptr_t)helper == g_pl_helper && tw_ms() < g_pl_until)))
        return tw_place_pickc(helper, a1);
    if (g_dbg && helper && g_place_ms && tw_ms() - g_place_ms < 300) {   // point 5, DM_DBG: the pad's own target in the placement mode (on change)
        static float last[3]; const uint8_t *h = (const uint8_t *)(uintptr_t)helper; static unsigned n;
        const float *sel = tw_sel_pos(h);
        if (sel && n < 200 && (fabsf(sel[0] - last[0]) + fabsf(sel[2] - last[2]) > 0.25f)) { n++; memcpy(last, sel, sizeof last);
            const float *w = (const float *)(h + TW_H_WALK);
            LOG("[world] placement mode, pad target: selector %.2f %.2f %.2f, walk point %.2f %.2f %.2f (%s), under it %s %#llx (before this frame's ray)", (double)sel[0], (double)sel[1], (double)sel[2],
                (double)w[0], (double)w[1], (double)w[2], h[TW_H_WALK_OK] ? "walkable" : "not walkable", tw_kind(*(const volatile uint64_t *)(h + TW_H_OBJ)),
                (unsigned long long)*(const volatile uint64_t *)(h + TW_H_OBJ)); }
    }
    if (!g_hover) {
        const uint64_t r0 = tw_pickc_orig(helper, a1);
        if (helper && *g_mode == 0) { g_live_obj = *(const volatile uint64_t *)((const uint8_t *)(uintptr_t)helper + TW_H_OBJ); g_live_ms = tw_ms(); }   // PC input mode: the engine's own pick under the UI cursor
        return r0;
    }
    const uint8_t old = *g_mode; *g_mode = 0;
    const uint64_t r = tw_pickc_orig(helper, a1);
    *g_mode = old; g_n_pick++;
    if (helper) { g_pick_obj = *(const volatile uint64_t *)((const uint8_t *)(uintptr_t)helper + TW_H_OBJ); g_pick_ms = tw_ms(); }   // pack 3y: what is under the finger
    if (g_dbg && helper) {
        const uint8_t *h = (const uint8_t *)(uintptr_t)helper; static uint64_t last_obj = ~0ull; static unsigned n;
        const uint64_t obj = *(const volatile uint64_t *)(h + TW_H_OBJ);
        if ((obj != last_obj || tw_ms() - g_click_ms < 40) && n < 600) { n++; last_obj = obj;
            const float *sc = (const float *)(h + TW_H_SCREEN), *p = (const float *)(h + TW_H_POS), *w = (const float *)(h + TW_H_WALK);
            LOG("[world] pick (player %d) at %.0f,%.0f px: under the finger %s %#llx (character %#llx, item %#llx), world point %.2f %.2f %.2f, walk point %.2f %.2f %.2f (%s), flags %#x",
                (int)*(const volatile int16_t *)(h + TW_H_PLAYER), (double)sc[0], (double)sc[1], tw_kind(obj), (unsigned long long)obj,
                (unsigned long long)*(const volatile uint64_t *)(h + TW_H_CHAR), (unsigned long long)*(const volatile uint64_t *)(h + TW_H_ITEM),
                (double)p[0], (double)p[1], (double)p[2], (double)w[0], (double)w[1], (double)w[2], h[TW_H_WALK_OK] ? "walkable: the move task takes it" : "not walkable",
                (unsigned)*(const volatile uint16_t *)(h + TW_H_FLAGS)); }
    }
    return r;
}
static void tw_tasks_say(const uint8_t *ic, const char *when) {   // DM_DBG: the preview / running task of an input controller, on change
    static const uint8_t *last_ic; static int last_pv = -2, last_run = -2; static unsigned n;
    const uint8_t *pv = *(const uint8_t *const volatile *)(ic + TW_IC_PREVIEW), *run = *(const uint8_t *const volatile *)(ic + TW_IC_TASK);
    const int tp = tw_task_type(pv), tr = tw_task_type(run);
    if ((ic == last_ic && tp == last_pv && tr == last_run) || n >= 600) return;
    if (ic != last_ic && tp < 0 && tr < 0) return;   // an idle party member's controller
    n++; last_ic = ic; last_pv = tp; last_run = tr;
    LOG("[world] %s: input controller %p: preview task type %d [%s] (flags %#x) = what a click does now, running task type %d [%s] (-1 = none)", when, (const void *)ic, tp,
        tw_task_name(tp), pv ? *(const volatile uint32_t *)(pv + TW_TASK_FLAGS) : 0u, tr, tw_task_name(tr));
}
static void tw_walk_watch(const uint8_t *ic) {   // after a click: did the move task start, and when does it end (the step the tw2 run was missing)
    const int tr = tw_task_type(*(const uint8_t *const volatile *)(ic + TW_IC_TASK));
    if (ic == g_click_ic && ic != g_walk_ic && tr == TW_TASK_MOVE && tw_ms() - g_click_ms < 1500) {
        g_walk_ic = ic; g_n_walk++;
        if (g_dbg) LOG("[world] WALK: the move task (type 0) of input controller %p STARTED %llu ms after the click (window %s, mode byte %u)", (const void *)ic,
                       (unsigned long long)(tw_ms() - g_click_ms), g_hover ? "open" : "closed", (unsigned)*g_mode);
    } else if (ic == g_walk_ic && tr != TW_TASK_MOVE) {
        g_walk_ic = NULL;
        if (g_dbg) LOG("[world] WALK: the move task of input controller %p ended %llu ms after the click (running task type now %d [%s]; window %s)", (const void *)ic,
                       (unsigned long long)(tw_ms() - g_click_ms), tr, tw_task_name(tr), g_hover ? "open" : "closed");
    }
}
static uint64_t tw_update(uint64_t ic, uint64_t tm) {   // main+0xe11f40: preview task / cursor of one character's input controller
    int placing = 0;
    if (ic) {   // point 4: is this controller in the item placement mode (its running task = the item-move task)?
        int rt = tw_task_type(*(const uint8_t *const volatile *)((const uint8_t *)(uintptr_t)ic + TW_IC_TASK));
        // phone 16.11 (touch5.log): Actions... -> Move shows the item-move task as the PREVIEW task (type 10, flags 0x100; running -1),
        // not as the running one; take either. Point 6 (dos2-touch6): the throw / aim task types the same way (dt_target_task).
        const int pt = tw_task_type(*(const uint8_t *const volatile *)((const uint8_t *)(uintptr_t)ic + TW_IC_PREVIEW));
        const int tt = dt_target_task(pt, rt, g_place_on, g_aim_on ? g_aim_mask : 0);
        tw_aim_note((const uint8_t *)(uintptr_t)ic, pt, rt);   // UI mode TOUCH: a prepared skill / target mode (above)
        if (dt_is_placing(pt) && !dt_is_placing(rt)) rt = pt;
        if (rt >= 0 || tt >= 0) {
            if (g_dbg && rt != g_run_type) { g_run_type = rt; g_run_ms = tw_ms(); } else if (g_dbg) g_run_ms = tw_ms();
            if (tt >= 0) {
                placing = 1;
                if (!g_place_ms || tw_ms() - g_place_ms > 1000 || g_place_ic != (const uint8_t *)(uintptr_t)ic || g_place_type != tt) { static unsigned n;
                    if (n++ < 40 || g_dbg) {
                        if (tt == DT_TASK_ITEMMOVE) LOG("[world] item placement mode: input controller %p runs / previews the item-move task (type %d, cursor ItemMove): a world tap = pad A with the "
                                                        "picking at the finger (DM_DOS2_TOUCH_PLACE=0 off)", (const void *)(uintptr_t)ic, tt);
                        else LOG("[world] aim mode (throw / prepared skill): input controller %p previews task type %d, runs %d: a world tap = the pad's selector at the finger's ground "
                                 "point + pad A, no PC click (DM_DOS2_TOUCH_AIM=0 off, DM_DOS2_TOUCH_AIM_TYPES=%#x)", (const void *)(uintptr_t)ic, pt, rt, g_aim_mask); } }
                g_place_ms = tw_ms(); g_place_ic = (const uint8_t *)(uintptr_t)ic; g_place_type = tt;
            }
        }
    }
    const int win = g_hover != 0 && !placing;   // placing: the Update stays the pad's (the picking alone follows the finger)
    if (!win) {
        const int keep = g_keep && ic && (const uint8_t *)(uintptr_t)ic == g_walk_ic;   // DM_DOS2_TOUCH_WORLD_KEEP=1: the walk keeps the wisp-mode answer (mode byte untouched)
        if (keep) { tw_pc_enter(); g_n_keep++; }
        const uint64_t r = g_o_update(ic, tm);
        if (keep) tw_pc_leave();
        if (ic) { tw_walk_watch((const uint8_t *)(uintptr_t)ic); if (g_dbg && tw_ms() - g_click_ms < 3000) tw_tasks_say((const uint8_t *)(uintptr_t)ic, "after the click");
                  else if (g_dbg && (placing || (g_pl_until && tw_ms() < g_pl_until + 1500))) tw_tasks_say((const uint8_t *)(uintptr_t)ic, "placement mode"); }
        return r; }
    const uint8_t old = *g_mode; *g_mode = 0; tw_pc_enter();
    const uint64_t r = g_o_update(ic, tm);
    tw_pc_leave(); *g_mode = old; g_n_upd++;
    if (ic) tw_walk_watch((const uint8_t *)(uintptr_t)ic);
    if (g_dbg && ic) tw_tasks_say((const uint8_t *)(uintptr_t)ic, "finger on the world");
    return r;
}
static volatile uint64_t g_ctx_ms;   // pack 3z: a long press on an object sent a right click (ContextMenu <- mouse:right, Default/inputconfig.json)
void dm_dos2_touch_world_ctx(void);   // hle_dos2_touch.c, right before it sends the right click records
void dm_dos2_touch_world_ctx(void) { g_ctx_ms = tw_ms(); }
// UI mode TOUCH, phone run 23.11 12:36 (build 66b3b2d6; owner: "a tap on the ground still casts the skill at once instead of only aiming it"):
// the host did aim (no click of its own: "world tap at 471,601: AIM"), yet the prepared skill (input controller task 3) was gone right after
// the finger's lift and the hot bar dropped its mark (dmAim(0)) at the probe press's release. The probe press (the left down / up every world
// finger sends first, to learn whether a UI object takes it) never reached the input controller: its Action1 counters (kept away 27, passed 0)
// did not move for that tap, while they did for every tap with nothing prepared. So with a skill prepared another input listener, earlier in
// the manager's list, takes Action1 (mouse:left) and casts. The listener dispatch: main+0x843f80(out, mgr, queue) - the events of one binding
// (queue +8 = array, stride 0x40; +0x10 bits 24..47 = count; event +0xc = the InputEvent the listeners get: +0 id, Action1 = 4; +0x20 pressed;
// +0x38 u16 device), listener by listener ([mgr+0x1a0], vt+0x38 OnInputEvent) until one takes one; out +0x40 = taken, +0x48 = the listener that
// captured the event (0 = no capture). Called from ProcessInputEvent (main+0x84590c / 0x84591c / 0x845934) and the manager's update (main+0xb07938 /
// 0xb07950); the callers empty the queue after it. While a skill / target mode is prepared (hle_dos2_touchui_hud.c dm_dos2_touchui_hud_aiming)
// the Action1 events of a probe press / UI gesture (kind 1 of dm_dos2_touch_world_left) are left out of the queue for the call (a copy without
// them; the array and the count are put back on return): no listener of the game sees them, the UI still gets FlashLeftMouse (event 1, the same
// mouse:left). The host's own world click (kind 2: CONFIRM, a tap with nothing prepared) passes as before. DM_DOS2_TOUCH_T_AIMPROBE=0: the probe's
// Action1 goes to the game as before. DM_DBG: one line per Action1 queue (who took it: out +0x40 / +0x48, the listener's vtable) - names the caster.
enum { TW_DISPATCH = 0x843f80, TW_Q_ARR = 8, TW_Q_CNT = 0x10, TW_Q_EV = 0x40, TW_Q_ID = 0xc, TW_Q_DOWN = 0x2c, TW_Q_MAX = 32 };
static const uint32_t k_tw_dispatch[4] = {0xd10443ff /* sub sp,sp,#0x110 */, 0xa90b6ffc, 0xa90c67fa, 0xa90d5ff8};
typedef uint64_t (*tw_fn3)(uint64_t, uint64_t, uint64_t);
static tw_fn3 g_o_disp;
static int g_aim_probe = 1; static unsigned g_n_aimdrop, g_n_a1q;
int dm_dos2_touchui_hud_aiming(void);   // hle_dos2_touchui_hud.c (game thread)
static uint64_t tw_dispatch(uint64_t out, uint64_t mgr, uint64_t queue) {
    uint8_t *q = (uint8_t *)(uintptr_t)queue;
    uint8_t *arr = q ? *(uint8_t *volatile *)(q + TW_Q_ARR) : NULL;
    const uint64_t cw = q ? *(volatile uint64_t *)(q + TW_Q_CNT) : 0; const unsigned n = (unsigned)((cw >> 24) & 0xffffffu);
    if (!g_on || !arr || !n || n > TW_Q_MAX) return g_o_disp(out, mgr, queue);
    unsigned a1 = 0, drop = 0, keep = 0; int down = 0, kind = 0;
    for (unsigned i = 0; i < n; i++) if (*(const uint32_t *)(arr + i * TW_Q_EV + TW_Q_ID) == TW_EV_ACTION1) {
        a1++; down = arr[i * TW_Q_EV + TW_Q_DOWN] != 0; kind = down ? g_kind_dn : g_kind_up;
        if (tw_ms() - (down ? g_kind_dn_ms : g_kind_up_ms) > 700) kind = 0; }
    if (!a1) return g_o_disp(out, mgr, queue);
    const int aiming = dm_dos2_touchui_hud_aiming();
    uint64_t r;
    if (g_aim_probe && kind == 1 && aiming) {
        uint8_t buf[TW_Q_MAX * TW_Q_EV];
        for (unsigned i = 0; i < n; i++) { const uint8_t *e = arr + i * TW_Q_EV;
            if (*(const uint32_t *)(e + TW_Q_ID) == TW_EV_ACTION1) { drop++; continue; }
            memcpy(buf + keep * TW_Q_EV, e, TW_Q_EV); keep++; }
        *(uint8_t *volatile *)(q + TW_Q_ARR) = buf;
        *(volatile uint64_t *)(q + TW_Q_CNT) = (cw & ~(0xffffffull << 24)) | ((uint64_t)keep << 24);
        r = g_o_disp(out, mgr, queue);
        *(uint8_t *volatile *)(q + TW_Q_ARR) = arr; *(volatile uint64_t *)(q + TW_Q_CNT) = cw;
        g_n_aimdrop++;
    } else r = g_o_disp(out, mgr, queue);
    if (g_dbg && g_n_a1q++ < 300) {
        const uint8_t *o = (const uint8_t *)(uintptr_t)out; const uint64_t L = o ? *(const volatile uint64_t *)(o + 0x48) : 0;
        const uint64_t vt = L ? *(const volatile uint64_t *)(uintptr_t)L : 0, b = (uint64_t)(uintptr_t)g_main->base;
        LOG("[world] Action1 %s (touch kind %d: 1 probe / UI gesture, 2 the host's world click, 0 not ours), prepared %d: %s; %u of %u event(s) left -> taken %d, "
            "captured by listener %#llx (vtable main+%#llx)", down ? "down" : "up", kind, aiming, drop ? "LEFT OUT of the dispatch (nothing of the game sees it)" : "dispatched",
            n - drop, n, o ? (int)o[0x40] : -1, (unsigned long long)L, vt >= b && vt - b < 0x4000000ull ? (unsigned long long)(vt - b) : 0ull);
    }
    return r;
}
static uint64_t tw_input(uint64_t ic, uint64_t ev) {   // main+0xe13830
    const uint8_t *e = (const uint8_t *)(uintptr_t)ev;
    if (e && *(const uint32_t *)e != TW_EV_ACTION1 && g_ctx_ms && tw_ms() - g_ctx_ms < 500) {   // pack 3z: the right click's events run as the PC does
        // (mode byte 0 for the call), so the context menu opens for the object under the finger (the PC picking of the hover window), not
        // for the pad's proximity selection (owner: "the long press opened Actions for the character, not for what is under my finger")
        const uint8_t old = *g_mode; *g_mode = 0; tw_pc_enter();
        const uint64_t r = g_o_input(ic, ev);
        tw_pc_leave(); *g_mode = old;
        static unsigned n; if (n++ < 40) LOG("[world] long-press right click: input event %u %s -> input controller %p as the PC does: answer %#llx",
                                             *(const uint32_t *)e, e[0x20] ? "down" : "up", (const void *)(uintptr_t)ic, (unsigned long long)r);
        return r;
    }
    if (!e || *(const uint32_t *)e != TW_EV_ACTION1) return g_o_input(ic, ev);
    const int down = e[0x20] != 0; const uint64_t now = tw_ms();
    int kind = down ? g_kind_dn : g_kind_up;
    if (now - (down ? g_kind_dn_ms : g_kind_up_ms) > 700) kind = 0;   // not one of ours (or far too late): the game's own way
    if (kind == 1) {   // a probing press / a UI gesture: not a world click
        g_n_swallow++;
        if (g_dbg) { static unsigned n; if (n++ < 200) LOG("[world] Action1 %s from a touch left record that is not a world click (probe / UI gesture): kept away from input controller %p",
                                                           down ? "down" : "up", (const void *)(uintptr_t)ic); }
        return 0;
    }
    if (kind != 2) { g_n_pass++; return g_o_input(ic, ev); }
    const uint8_t *c = (const uint8_t *)(uintptr_t)ic;
    const int before = tw_task_type(*(const uint8_t *const volatile *)(c + TW_IC_TASK));
    const uint8_t old = *g_mode; *g_mode = 0; tw_pc_enter();
    const uint64_t r = g_o_input(ic, ev);
    tw_pc_leave(); *g_mode = old; g_n_click++; g_click_ms = now;
    if (r || *(const uint8_t *const volatile *)(c + TW_IC_PREVIEW)) { g_click_ic = c; if (down) g_walk_ic = NULL; }   // the controller of the acting character
    if (g_dbg) { static unsigned n; if (n++ < 400) LOG("[world] CLICK: Action1 %s -> input controller %p as the PC does (mode byte 0 for the call): answer %#llx; preview task type %d [%s], "
                                                       "running task type %d -> %d, pressed-on object %#llx", down ? "down" : "up", (const void *)c, (unsigned long long)r,
                                                       tw_task_type(*(const uint8_t *const volatile *)(c + TW_IC_PREVIEW)), tw_task_name(tw_task_type(*(const uint8_t *const volatile *)(c + TW_IC_PREVIEW))), before, tw_task_type(*(const uint8_t *const volatile *)(c + TW_IC_TASK)),
                                                       (unsigned long long)*(const volatile uint64_t *)(c + TW_IC_PRESS)); }
    return r;
}
// ---- for hle_dos2_touch.c (game thread: the pad poll) ----
int dm_dos2_touch_world_on(void);
int dm_dos2_touch_world_on(void) { return g_on; }
void dm_dos2_touch_world_hover(int on);   // 1 = the picking and the preview follow the UI cursor (a world finger is down / a world click is in flight)
void dm_dos2_touch_world_hover(int on) {
    if (!g_on || (g_hover != 0) == (on != 0)) return;
    g_hover = on ? 1 : 0;
    if (g_dbg) { static unsigned n; if (n++ < 400) LOG("[world] PC picking window %s (mode byte now %u; picks %u, controller updates %u in windows so far)", on ? "OPEN: the cursor is the finger" : "closed: the pad's selection again",
                                                       (unsigned)*g_mode, g_n_pick, g_n_upd); }
}
void dm_dos2_touch_world_left(int kind, int down);   // a touch left record goes to InjectInput now: 1 = probe / UI gesture (not for the world), 2 = the world click
void dm_dos2_touch_world_left(int kind, int down) {
    if (!g_on) return;
    if (down) { g_kind_dn = kind; g_kind_dn_ms = tw_ms(); } else { g_kind_up = kind; g_kind_up_ms = tw_ms(); }
}
int dm_dos2_touch_world_placing(void);   // hle_dos2_touch.c, on a world tap: is the item placement mode on (seen in the last 300 ms)?
int dm_dos2_touch_world_placing(void) {
    if (!g_on || (!g_place_on && !g_aim_on)) return 0;
    const uint64_t t = g_place_ms;
    return t && tw_ms() - t < 300;
}
int dm_dos2_touch_world_place_mode(void);   // DM_DOS2_TOUCH_PLACE: 0 off, 1 the selector at the finger (point 5, default), 2 PC picking at the finger (point 4)
int dm_dos2_touch_world_place_mode(void) {   // point 6: an aim mode always takes the selector way (1); the item move keeps DM_DOS2_TOUCH_PLACE
    if (!g_on) return 0;
    const int t = g_place_type;
    return t >= 0 && t != DT_TASK_ITEMMOVE && g_place_ms && tw_ms() - g_place_ms < 1000 ? (g_aim_on ? 1 : 0) : g_place_on ? g_place_on : (g_aim_on ? 1 : 0);
}
int dm_dos2_touch_world_place_type(void);   // the target mode's task type (10 item move, 2 throw / skill aim; -1 none seen in the last second)
int dm_dos2_touch_world_place_type(void) { return g_on && g_place_ms && tw_ms() - g_place_ms < 1000 ? g_place_type : -1; }
void dm_dos2_touch_world_place_at(void);   // a placement tap: learn the finger's ground point (the UI cursor is at the finger), then hold the selector there
void dm_dos2_touch_world_place_at(void) { g_pl_probe = 2; g_pl_have = 0; g_pl_until = tw_ms() + 1500; g_n_pl_hold = 0; }
int dm_dos2_touch_world_place_ready(void);   // 1 = the point is known and the selector has been held there for a few ray set-ups
int dm_dos2_touch_world_place_ready(void) { return g_pl_have && g_n_pl_hold >= 3; }
int dm_dos2_touch_world_place_failed(void);   // the probe ran but found no usable point
int dm_dos2_touch_world_place_failed(void) { return !g_pl_probe && !g_pl_have && g_pl_until; }
void dm_dos2_touch_world_place_hold(int ms);   // keep the selector at the point ms more (after pad A), 0 = release now
void dm_dos2_touch_world_place_hold(int ms) { g_pl_until = ms > 0 ? tw_ms() + (uint64_t)ms : 0; if (ms <= 0) { g_pl_have = 0; g_pl_probe = 0; } }
int dm_dos2_touch_world_place_preview(void);   // the preview task type of the placing controller (-1 = unknown)
int dm_dos2_touch_world_place_preview(void) {
    const uint8_t *ic = g_place_ic;
    if (!g_on || !ic || !g_place_ms || tw_ms() - g_place_ms > 1000) return -1;   // seen in an Update less than 1 s ago: still alive
    return tw_task_type(*(const uint8_t *const volatile *)(ic + TW_IC_PREVIEW));
}
int dm_dos2_touch_world_run_type(void);   // DM_DBG log: the running task type of the last controller that had one (-1 = none / stale)
int dm_dos2_touch_world_run_type(void) { return g_on && g_run_ms && tw_ms() - g_run_ms < 500 ? g_run_type : -1; }
void dm_dos2_touch_world_kinds_clear(void);   // the pad's own Action1 (pad A of a placement tap) must not be taken for our probe press
void dm_dos2_touch_world_kinds_clear(void) { g_kind_dn = g_kind_up = 0; }
void dm_dos2_touch_world_summary(void);
void dm_dos2_touch_world_summary(void) {
    if (g_on && g_n_aimdrop) LOG("[world] TOUCH aim: %u Action1 queue(s) of a probe press left out while a skill was prepared (DM_DOS2_TOUCH_T_AIMPROBE=1)", g_n_aimdrop);
    if (g_on) LOG("[world] Action1: world clicks %u (down + up), kept away %u (probe / UI gesture), passed untouched %u; picks with the cursor %u, controller updates in a window %u; window %s; "
                  "floor: cursor-targeting answers forced %u, walks started by a click %u, kept updates %u (DM_DOS2_TOUCH_WORLD_FLOOR=%d _KEEP=%d); TOUCH aim: picks held at the aim point %u",
                  g_n_click, g_n_swallow, g_n_pass, g_n_pick, g_n_upd, g_hover ? "open" : "closed", g_n_cur, g_n_walk, g_n_keep, g_floor, g_keep, g_n_aimhold);
}

static int tw_patch(module_t *m, uint32_t off, const uint32_t *jump) {
    uint32_t *at = (uint32_t *)(void *)(m->base + off);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF);
    const size_t len = (size_t)(((uintptr_t)(at + 4) - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return 0;
    memcpy(at, jump, 16);
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[world] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)(at + 4));
    return 1;
}
void dm_dos2_touch_world_install(void);   // main.c host_preload, after dm_dos2_touchui_route_install and before dm_dos2_touch_install
void dm_dos2_touch_world_install(void) {
    const char *v = getenv("DM_DOS2_TOUCH_WORLD");
    if (!v || atoi(v) < 3) return;
    g_dbg = (v = getenv("DM_DBG")) && atoi(v) > 0;
    g_floor = !((v = getenv("DM_DOS2_TOUCH_WORLD_FLOOR")) && *v == '0'); g_keep = (v = getenv("DM_DOS2_TOUCH_WORLD_KEEP")) && atoi(v) > 0;
    g_place_on = (v = getenv("DM_DOS2_TOUCH_PLACE")) && *v ? (*v == '0' ? 0 : *v == '2' ? 2 : 1) : 1;
    g_aim_on = !((v = getenv("DM_DOS2_TOUCH_AIM")) && *v == '0');
    g_aim_eng = !((v = getenv("DM_DOS2_TOUCH_T_AIM_ENG")) && *v == '0');
    if ((v = getenv("DM_DOS2_TOUCH_AIM_TYPES"))) g_aim_mask = dt_aim_mask(v) & ~(1u << DT_TASK_ITEMMOVE) & ~(1u << TW_TASK_MOVE) & ~(1u << TW_TASK_PAD);   // never the walk / the stick
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_main = &g_mod[i];
    module_t *m = g_main;
    if (!m) return;
    static const tw_site k_sites[] = {{TW_INPUT, 6, k_tw_input}, {TW_INPUT_EV, 1, k_tw_input_ev}, {TW_INPUT_DOWN, 1, k_tw_input_down}, {TW_INPUT_A1, 3, k_tw_input_a1},
        {TW_UPDATE, 8, k_tw_update}, {TW_UPDATE_MODE, 3, k_tw_update_mode}, {TW_PICK, 6, k_tw_pick}, {TW_PICK_MODE, 2, k_tw_pick_mode}, {TW_PICKC, 6, k_tw_pickc},
        {TW_PICKC_MODE, 3, k_tw_pickc_mode}, {TW_PICKC_CUR, 5, k_tw_pickc_cur}, {TW_TASK_CTOR, 1, k_tw_task_ctor}, {TW_MODE_INIT, 1, k_tw_mode_init},
        {TW_CURSOR, 8, k_tw_cursor}, {TW_MOVE_GATE, 4, k_tw_move_gate}, {TW_PAD_GATE, 4, k_tw_pad_gate}, {TW_PAD_CTOR, 2, k_tw_pad_ctor}};
    for (unsigned i = 0; i < sizeof k_sites / sizeof *k_sites; i++)
        if ((uint64_t)k_sites[i].off + 4ull * k_sites[i].n > m->text_end || memcmp(m->base + k_sites[i].off, k_sites[i].want, 4ull * k_sites[i].n)) {
            LOG("【红】[world] main+%#x differs: not DOS2 update v786432 main (build 4979b200) at the input controller / the picking: DM_DOS2_TOUCH_WORLD=3 falls back to =1 "
                "(stick + walk-and-A tap)", k_sites[i].off);
            g_main = NULL; return; }
    g_mode = (volatile uint8_t *)(m->base + TW_MODE);
    // one page: four plain trampolines (C hooks; the fourth, main+0xe151f0, at words 24..31) + the stub in front of main+0xfab260 (its arguments may be in vector registers: no C in between)
    uint32_t *t = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) { LOG("【红】[world] mmap failed: DM_DOS2_TOUCH_WORLD=3 falls back to =1"); g_main = NULL; return; }
    static const struct { uint32_t off; const uint32_t *first; } k_c[4] = {{TW_INPUT, k_tw_input}, {TW_UPDATE, k_tw_update}, {TW_PICKC, k_tw_pickc}, {TW_CURSOR, k_tw_cursor}};
    for (int i = 0; i < 4; i++) {   // 8 words each: the first four instructions (sub sp / stp: position independent), then a jump to the fifth
        uint32_t *p = t + i * 8; const uint64_t rest = (uint64_t)(uintptr_t)(m->base + k_c[i].off + 16);
        memcpy(p, k_c[i].first, 16); p[4] = 0x58000050 /* ldr x16,#8 */; p[5] = 0xd61f0200 /* br x16 */; p[6] = (uint32_t)rest; p[7] = (uint32_t)(rest >> 32);
    }
    uint32_t *s = t + 32;   // the stub: window open -> mode byte = 0 around the call, old value kept on the stack; x16 / x17 only
    { const uint64_t rest = (uint64_t)(uintptr_t)(m->base + TW_PICK + 16), flag = (uint64_t)(uintptr_t)&g_hover, mode = (uint64_t)(uintptr_t)g_mode;
      s[0] = 0x58000010 | (24u << 5);   // ldr x16, =&g_hover
      s[1] = 0x39400210;                // ldrb w16,[x16]
      s[2] = 0x34000010 | (12u << 5);   // cbz w16, plain
      s[3] = 0x58000010 | (23u << 5);   // ldr x16, =mode byte
      s[4] = 0x39400211;                // ldrb w17,[x16]
      s[5] = 0xa9be7bfd;                // stp x29,x30,[sp,#-32]!
      s[6] = 0xf9000bf1;                // str x17,[sp,#16]
      s[7] = 0x3900021f;                // strb wzr,[x16]
      s[8] = 0x94000000 | 8u;           // bl tramp
      s[9] = 0x58000010 | (17u << 5);   // ldr x16, =mode byte
      s[10] = 0xf9400bf1;               // ldr x17,[sp,#16]
      s[11] = 0x39000211;               // strb w17,[x16]
      s[12] = 0xa8c27bfd;               // ldp x29,x30,[sp],#32
      s[13] = 0xd65f03c0;               // ret
      s[14] = 0x14000000 | 2u;          // plain: b tramp
      s[15] = 0xd503201f;               // nop
      memcpy(s + 16, k_tw_pick, 16); s[20] = 0x58000050; s[21] = 0xd61f0200; s[22] = (uint32_t)rest; s[23] = (uint32_t)(rest >> 32);
      s[24] = (uint32_t)flag; s[25] = (uint32_t)(flag >> 32); s[26] = (uint32_t)mode; s[27] = (uint32_t)(mode >> 32); }
    if (mprotect(t, 0x4000, PROT_READ | PROT_EXEC)) { LOG("【红】[world] mprotect RX failed: DM_DOS2_TOUCH_WORLD=3 falls back to =1"); munmap(t, 0x4000); g_main = NULL; return; }
    __builtin___clear_cache((char *)t, (char *)(t + 64));
    g_o_input = (tw_fn2)(void *)t; g_o_update = (tw_fn2)(void *)(t + 8); g_o_pickc = (tw_fn2)(void *)(t + 16); g_o_cursor = (tw_fn2)(void *)(t + 24);
    const uint64_t to[5] = {(uint64_t)(uintptr_t)(void *)tw_input, (uint64_t)(uintptr_t)(void *)tw_update, (uint64_t)(uintptr_t)(void *)tw_pickc, (uint64_t)(uintptr_t)(void *)s,
                            (uint64_t)(uintptr_t)(void *)tw_cursor};
    static const uint32_t tw_at[5] = {TW_INPUT, TW_UPDATE, TW_PICKC, TW_PICK, TW_CURSOR};
    for (int i = 0; i < (g_floor ? 5 : 4); i++) {
        const uint32_t jump[4] = {0x58000050, 0xd61f0200, (uint32_t)to[i], (uint32_t)(to[i] >> 32)};
        if (!tw_patch(m, tw_at[i], jump)) { LOG("【红】[world] mprotect RW failed at main+%#x (%d hook(s) already in: they only pass through): DM_DOS2_TOUCH_WORLD=3 falls back to =1", tw_at[i], i); return; }
    }
    g_on = 1;
    {   // the probe's Action1 while a skill is prepared (tw_dispatch above)
        int dn_words_ok(module_t *mm, uint32_t off, const uint32_t pro[4]); int dn_hook(module_t *mm, uint32_t off, const uint32_t pro[4], void *hook, void **orig);
        g_aim_probe = !((v = getenv("DM_DOS2_TOUCH_T_AIMPROBE")) && *v == '0') && !((v = getenv("DM_DOS2_TOUCH_T_AIMTAP")) && atoi(v) <= 0);
        if (!g_aim_probe) LOG("[world] DM_DOS2_TOUCH_T_AIMPROBE=0 (or T_AIMTAP=0): the probe press's Action1 reaches the game while a skill is prepared");
        else if (!dn_words_ok(m, TW_DISPATCH, k_tw_dispatch)) { g_aim_probe = 0; LOG("【红】[world] main+%#x differs (the input listener dispatch of build 4979b200): a probe press may cast a prepared skill", TW_DISPATCH); }
        else if (dn_hook(m, TW_DISPATCH, k_tw_dispatch, (void *)tw_dispatch, (void **)(void *)&g_o_disp)) { g_aim_probe = 0; LOG("【红】[world] main+%#x: no hook (mmap / mprotect): a probe press may cast a prepared skill", TW_DISPATCH); }
        else LOG("[world] input listener dispatch main+%#x hooked: while a skill / target mode is prepared the probe press's Action1 is left out (a world tap only aims, CONFIRM casts; DM_DOS2_TOUCH_T_AIMPROBE=0 off)", TW_DISPATCH);
    }
    LOG("[world] DM_DOS2_TOUCH_WORLD=3: PC world clicks by touch in controller mode: input controller OnInputEvent main+%#x and Update main+%#x, picking main+%#x / main+%#x "
        "hooked; while a world finger is down / a world click is in flight these run with the mode byte main+%#x = 0 (ray from the UI cursor = the finger), "
        "Action1 of a probe press / UI gesture is kept away from the world", TW_INPUT, TW_UPDATE, TW_PICK, TW_PICKC, TW_MODE);
    LOG("[world] floor taps in the normal (character-bound) controller mode: %s (DM_DOS2_TOUCH_WORLD_FLOOR=0 off; DM_DOS2_TOUCH_WORLD_KEEP=%d: 1 = the answer also holds "
        "while the click-started walk runs)", g_floor ? "main+0xe151f0 hooked: inside those PC calls the controller counts as cursor-targeting (as in the wisp mode), so the move "
        "task (type 0) takes the tapped point and the pad's stick task (type 25) steps back" : "NOT handled, as pack 2y", g_keep);
    LOG("[world] item placement (Actions... -> Move): %s", g_place_on == 1 ? "while a controller runs / previews the item-move task (type 10) a world tap = the pad's selector "
        "[helper+0x108]+0x30 at the finger's ground point, then pad A (DM_DOS2_TOUCH_PLACE=0 off, =2 the PC picking way)" : g_place_on == 2 ? "DM_DOS2_TOUCH_PLACE=2: PC picking at the finger + pad A (A/B)" :
        "OFF (DM_DOS2_TOUCH_PLACE=0): a tap is the PC click, as before");
    LOG("[world] throw / aim (inventory \"Throw\", a prepared skill): %s (task type mask %#x)", g_aim_on && g_aim_mask ? "while a controller previews / runs such a task a world tap = the pad's "
        "selector at the finger's ground point, then pad A once the selector has been there a few frames (point 6; DM_DOS2_TOUCH_AIM=0 off: the PC click)" :
        "OFF (DM_DOS2_TOUCH_AIM=0 or an empty DM_DOS2_TOUCH_AIM_TYPES): a tap is the PC click", g_aim_mask);
}
#endif
