// [DOS2] Console touch, step 2..4 of docs/dos2/TOUCH_CONSOLE_PLAN.md: the mouse goes to the console movies that have mouse code, per UI object,
// instead of the one global filter byte [input manager + 0x170] (DM_DOS2_TOUCH_ROUTE, on with DM_DOS2_TOUCHUI; static only, not run on a phone).
// main = main.raw of update v786432, build 4979b200, offsets = main.raw.
//   UIManager::OnInputEvent 0xae83e0(uimgr, event) walks the UI objects ([uimgr+0xf8] pointers, count = bits 24..47 of [uimgr+0x100]) from the LAST
//   (top) to the first, twice, and calls 0xaea7b0(uimgr, object, event, w3 pass, x4, x5 = &handled) for each (0xae8468..0xae8488 pass 1 with w3 = 1,
//   0xae84a8..0xae84c8 pass 2 with w3 = 0); a walk ends when a call returns 1. In 0xaea7b0:
//     - object flags (+0x14) bit 1 clear: nothing, return 0 (0xaea7d4);
//     - w25 = (flags bit 9 & flags bit 3) | (object byte +0xe5 && event u16 +8 == 0) (0xaea820..0xaea87c): "takes the whole input". Pass 1 handles
//       only such objects (0xaea884: w3 bit 0 and w25 == 0 -> out) and returns 1 for them, which ends the walk - so NOTHING below a visible object
//       with flag 0x200 gets an input event, mouse included. That is the engine's own modal rule and it answers experiment E1 of the plan for boxes
//       created with 0x200 (the options from the main menu are: flags 0x1e21, 0x118032c);
//     - pass 2: every object gets the event through its vt+0xa8 (-> FlashPlayer 0xd23420 -> Iggy mouse / the movie's onEventDown / onEventUp); the
//       result goes into [x5] (0xaea8d4 strh w0,[x20]), and a visible object below one that handled it gets vt+0xb0(event, 1) instead (0xaea830..
//       0xaea854; a mouse move is not passed at all), i.e. "an upper object took it".
//   Event: +0 u32 id (1 / 2 / 3 FlashLeft / Right / MiddleMouse, 0x58 FlashMouseMove, 0x67 / 0x68 FlashScrollUp / Down: 0xd23800), +4 s32 player,
//   +0x20 u8 down. Input event names: the input manager [main+0x2bb8960] has a hash (+0x34 u32 buckets, +0x38 array; node +0 next, +0x10 u32 id,
//   +0x48 char *name; 0xd26c18..0xd26c80, the walk FlashPlayer uses for a movie's `events`).
// What the host does: one hook, the first four instructions of 0xaea7b0 (none PC-relative), trampoline as hle_dos2_touch.c.
//   1. PER-MOVIE MOUSE. With the hook in, hle_dos2_touch.c keeps [input manager + 0x170] = 1 (mouse records pass the listener filter 0x844014) and
//      this hook decides per object: a mouse event (ids above) is passed on only when the object is visible (flags bit 3) and its type ([obj+0xf0])
//      is in the allow list - the console movies with working mouse code (tools/dos2_abc.py listener tables, plan section 2):
//        hud     bottomBar_c 0x3b (hot bar slots), playerInfo 0x26 0x3d 0x73 (portraits), statusConsole 0x75 (end turn / flee / delay), combatTurn 0x08,
//                minimap 0x3c (minimap_d: a tap opens the map, tools/dos2_minimap_touch.py)
//        ctx     contextMenu_c 0x0c 0x60
//        panels  containerInventory 0x09 0x25, partyInventory_c 0x8e, equipmentPanel_c 0x40, statsPanel_c 0x3f, craftPanel_c 0x54, waypoints_c 0x4e,
//                itemSplitter_c 0x55, inventorySkillPanel_c 0x3e (the verdict-A list of docs/dos2/TOUCH_UI_AUDIT.md)
//                + journal_c 0x46, trade_c 0x49, reward_c 0x89, examine_c 0x43: their `_d` movies have the iPad button row (tools/dos2_panel_touch.py); journal_csp 0x8c (the journal the engine really creates, run tw4) too
//        dialog  dialog_c 0x42 0x65
//        menus   mainMenu 0x48 0x57 (`_d`), msgBox_c 0x4b 0x4c, saveLoad_c 0x4a, optionsSettings_c 0x58..0x5b, mods_c 0x67, characterCreation_c 0x04,
//                tutorialBox_c 0x5e, gameMenu_c 0x4d
//      Everything else (combatLog_c, overhead, enemyHealthBar, the PC tooltip path ...) never sees the mouse: for
//      those the hook returns 0 = "not handled, go on" without calling the game.
//   2. MODAL GUARD. Once a visible object with any modal bit (0x1e00; the engine itself only stops on 0x200) has been passed in a walk, objects
//      below it get no mouse event (the top modal still gets it, when its type is allowed). So a touch main menu under a message box cannot take
//      the click whatever flags the box was created with -> the `hints` set of menus is the default now (hle_dos2_touchui.c k_def).
//   3. A TAP IS NOT "ACCEPT". On the phone a left click also reached a console movie as the input event UIAccept (the pad's A; seen with the main
//      menu, the place in the engine that makes it is not found: UIAccept is bound to c:a only). The hook drops the event UIAccept (id by name,
//      checked: FlashLeftMouse must resolve to 1 and FlashMouseMove to 0x58) for an object when it comes within DM_DOS2_TOUCH_ROUTE_ACCEPT_MS (600)
//      of a touch-made left record while the npad's A has not been down in the last 300 ms (physical pad, on-screen pad and the host's own hint
//      pulses all set A in hle_hid.c fill) - in game for every object, out of game for the allow-listed movies and the console menus without touch code
//      (hle_dos2_touchui.c k_block), never for the main menu (its splash "press A" takes a tap as A, mainMenu_d has its own guard) or the loading
//      screen 0x17; other screens out of game (splash, videos, "press A") keep "a tap is A". DM_DBG lists every input event that reaches the UI within that window after
//      a tap (name, id), so one phone run names the real source.
//   4. For hle_dos2_touch.c's world gestures: which object took the last left press ([x5] after the call), and whether a menu / panel is up.
//   5. STAGE-LISTENER MOVIES (branch dos2-touch7, DM_DOS2_TOUCH_STAGE_TYPES, default "3b" = bottomBar_d): touch6 (tools/dos2_taps5_touch.py) hangs
//      a mouseDown / mouseUp listener on bottomBar_d's stage; a press that hits none of its elements targets the stage, the listener runs and
//      Iggy answers "handled" anywhere on the 1920x1080 stage (phone run 18.11, log C_base: every world tap / swipe "TAKEN by a UI object (type
//      0x3b)", the column icons of dmColumn_d below it got "not its event"). For such a movie, in game, a left / right / middle button event it
//      answered with 1 while [x5] was 0 and no mouse capture was set stays its own only when the UI cursor (the finger) is inside one of the
//      movie's live HUD rects (hle_dos2_touch.c dm_dos2_touch_stage_press, tools/dos2_hudrect_touch.py: hot bar with the hint row, btn / hold,
//      grafted buttons, hint4). Outside them the host clears [x5] and the capture the press made ([uimgr+0x10d] byte, [uimgr+0x218] handle,
//      main+0xaea8ec / 0xaea900) back to what they were: the movie's listener has run (a press anywhere still hides the long-press tooltip),
//      the walk goes on to the objects below (dmColumn_d) and the input stays the world's (Action1). No live rects of the movie: its answer
//      stands (the touch6 behaviour). Rule: hle_dos2_touch_gesture.h dt_hr_state / dt_stage_press_keep (dm/host/tests/dos2_touch_gesture_test.c).
// Switches (all need DM_DOS2_TOUCHUI=1; the hook is one site check away from "off, the old gate"):
//   DM_DOS2_TOUCH_ROUTE=0          no hook: hle_dos2_touch.c KBM=2 falls back to the window gate of pack 2v (dm_dos2_touchui_gate)
//   DM_DOS2_TOUCH_ROUTE_GAME=0     the hook routes in menus only; in game the filter byte follows the old gate (closed unless a modal touch window)
//   DM_DOS2_TOUCH_ROUTE_GROUPS=<l> groups allowed, default "hud,ctx,panels,dialog,menus" (e.g. "menus" = no mouse to the HUD at all)
//   DM_DOS2_TOUCH_ROUTE_ALLOW=<h>  extra hex type ids allowed ("3c,49"); DM_DOS2_TOUCH_ROUTE_DENY=<h> hex type ids taken out
//   DM_DOS2_TOUCH_ROUTE_MODAL=0    no modal guard (2.)
//   DM_DOS2_TOUCH_ROUTE_ACCEPT=0   no accept guard (3.); DM_DOS2_TOUCH_ROUTE_ACCEPT_MS=<ms> its window
//   DM_DOS2_TOUCH_ROUTE_HIDDEN=1   mouse also to allowed objects without the visible flag
//   DM_DOS2_TOUCH_STAGE_TYPES=<h>  hex type ids of movies with a stage listener (5.), default "3b"; "" or "0" = none (the touch6 behaviour)
//   DM_DOS2_TOUCH_ROUTE_BLOCK=0    UI mode TOUCH: an open PC window does not stop the mouse for what is below it (6., the run 21.11 behaviour)
//   DM_DBG=1                       every routing decision once per (type, decision), the events after a tap, who took a left press
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include "hle_dos2_touch_gesture.h"   // dt_stage_press_keep, DT_HR_* (5.)

enum { RT_MODE_BYTE = 0x2bb8918,   // the engine's input mode: 1 controller UI, 0 keyboard-and-mouse UI (hle_dos2_touch.c pass 3)
       RT_DISPATCH = 0xaea7b0, RT_CALL1 = 0xae8468, RT_CALL2 = 0xae84a8, RT_MODAL = 0xaea820, RT_MODAL2 = 0xaea87c, RT_HANDLED = 0xaea8d0, RT_UPPER = 0xaea830,
       RT_NAMES_LD = 0xd26c18, RT_NAMES = 0xd26c40, RT_NAME_ID = 0xd26c7c, RT_FLASH_IDS = 0xd23810,
       RT_UIMGR = 0x2bbb230, RT_INMGR = 0x2bb8960, RT_MGR_LIST = 0xf8, RT_MGR_LISTN = 0x100, RT_MGR_OBJS = 0x10, RT_MGR_NOBJ = 0x1c,
       RT_CAPTURE = 0xaea8e8, RT_UIM_CAPT = 0x10d, RT_UIM_CAPT_OBJ = 0x218,
       RT_OBJ_FLAGS = 0x14, RT_OBJ_PLAYER = 0x18, RT_OBJ_TYPE = 0xf0, RT_F_INPUT = 0x2, RT_F_VISIBLE = 0x8, RT_F_MODAL = 0x1e00,
       RT_EV_LEFT = 1, RT_EV_RIGHT = 2, RT_EV_MIDDLE = 3, RT_EV_MOVE = 0x58, RT_EV_SCROLL_UP = 0x67, RT_EV_SCROLL_DOWN = 0x68 };
static const uint32_t k_rt_dispatch[10] = {0xd101c3ff /* sub sp,sp,#0x70 */, 0xa9016ffc, 0xa90267fa, 0xa9035ff8, 0xa90457f6, 0xa9054ff4, 0xa9067bfd, 0x910183fd,
                                           0xb940142b /* ldr w11,[x1,#0x14]: object flags */, 0x3608042b /* tbz w11,#1: no input */};
static const uint32_t k_rt_call1[9] = {0xf9407e68 /* ldr x8,[x19,#0xf8]: the object list */, 0x8b1b0d08, 0xf85f8101 /* ldur x1,[x8,#-8]: from the top */, 0x910073e5 /* x5 = &handled */,
                                       0xaa1303e0, 0xaa1503e2 /* x2 = event */, 0x320003e3 /* mov w3,#1: pass 1 */, 0xaa1403e4, 0x940008ca /* bl 0xaea7b0 */};
static const uint32_t k_rt_call2[9] = {0xf9407e68, 0x8b170d08, 0xf85f8101, 0x910073e5, 0xaa1303e0, 0xaa1503e2, 0x2a1f03e3 /* mov w3,wzr: pass 2 */, 0xaa1403e4, 0x940008ba /* bl 0xaea7b0 */};
static const uint32_t k_rt_modal[7] = {0x5309256f /* ubfx w15,w11,#9,#1: flag 0x200 */, 0x530515cd, 0x0a4e0dee /* & visible (bit 3) */, 0x3618006b /* tbz w11,#3 */,
                                       0x3940028b /* ldrb w11,[x20]: handled by an upper object */, 0x3400024b, 0x7101615f /* cmp w10,#0x58 */};
static const uint32_t k_rt_modal2[4] = {0x2a0c01d9 /* orr w25,w14,w12: takes the whole input */, 0x35000059, 0x37001763 /* tbnz w3,#0: pass 1 leaves the others alone */, 0x350000d9};
static const uint32_t k_rt_handled[2] = {0x72001c1f /* tst w0,#0xff */, 0x79000280 /* strh w0,[x20]: the object's answer */};
static const uint32_t k_rt_capture[7] = {0x320003e8 /* mov w8,#1 */, 0x390436a8 /* strb w8,[x21,#0x10d]: mouse captured */, 0xf9400268, 0xf9401508, 0xaa1303e0, 0xd63f0100,
                                         0xf9010ea0 /* str x0,[x21,#0x218]: by this object's handle */};
static const uint32_t k_rt_upper[10] = {0x3940028b, 0x3400024b, 0x7101615f, 0x540000e0 /* a move is not passed on */, 0xf9400268, 0xf9405908 /* vt+0xb0 */, 0xaa1303e0, 0xaa0203e1,
                                        0x320003e2 /* mov w2,#1: not yours */, 0xd63f0100};
static const uint32_t k_rt_names_ld[4] = {0xb9403756 /* ldr w22,[x26,#0x34]: buckets */, 0x9a890109, 0x34000476, 0xf9401f57 /* ldr x23,[x26,#0x38]: bucket array */};
static const uint32_t k_rt_names[4] = {0xf94026a0 /* ldr x0,[x21,#0x48]: a known event's name */, 0xaa1903e1, 0x943b35e2 /* compare */, 0x34000180};
static const uint32_t k_rt_name_id[2] = {0xb94012b5 /* ldr w21,[x21,#0x10]: its id */, 0x34000175};
static const uint32_t k_rt_flash_ids[6] = {0x51019d48 /* sub w8,w10,#0x67: scroll up / down */, 0x2a0203e3, 0x7100051f, 0x540009a9, 0x7101615f /* cmp w10,#0x58: mouse move */, 0x540007e0};

typedef struct { uint32_t off, n; const uint32_t *want; } rt_site;
typedef uint64_t (*rt_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static module_t *g_main;
static rt_fn g_orig;
static int g_on, g_dbg, g_game = 1, g_modal = 1, g_accept = 1, g_accept_ms = 600, g_hidden;
static uint8_t g_allow[256], g_stage[256];   // g_stage: DM_DOS2_TOUCH_STAGE_TYPES (5.)
static unsigned g_n_stage, g_n_stage_kept, g_n_stage_norect;
static uint32_t g_accept_id; static int g_names_done;
// state of the walk / the last tap (game thread only: the input update and the pad poll run on it)
static int g_modal_seen; static uint32_t g_modal_type;
static uint64_t g_left_ms; static unsigned g_left_seq, g_seen_seq, g_taken_seq; static uint32_t g_taken_type;
static unsigned g_n_pass, g_n_drop, g_n_modal, g_n_accept;
// dmTouch kit (hle_dos2_touch_kit.c, DM_DOS2_TOUCH_KIT / DM_DOS2_TOUCH_KIT_M0): all NULL by default - the hook then behaves exactly as before
static const void *volatile g_kit_obj;   // K2: the UI object of the kit gesture in flight (game thread)
static int (*g_kit_accept)(const void *obj);   // K7: 1 = drop this UIAccept to obj
static void (*g_kit_note)(const void *obj, uint32_t type, uint32_t id, int down, int pass1, unsigned ret, unsigned handled);   // M0-3: a non-mouse input event reached obj

// 6. PC WINDOWS BLOCK WHAT IS UNDER THEM (phone run 21.11, UI mode TOUCH in game): every tap reached every movie below an open PC window as
//    "flag 0 (THE TARGET)": a window answers "not handled" for a press on its background (Iggy hits only elements with mouse code), so the
//    engine's walk handed the press on - the minimap / combat log / overhead under the inventory took it ("a button under the window got
//    pressed, random windows opened"). Rule (hle_dos2_touch_gesture.h dt_pc_blocker / dt_lowest_blocker): at the start of a walk the host
//    finds the lowest visible PC window in the walk order; once that object has had a mouse event in pass 2, the host sets [x5] = 1, so the
//    engine itself gives every object below "an upper object took it" (vt+0xb0(event, 1), main+0xaea830) - and the world gets nothing. The
//    HUD above the windows (column, hot bar, portraits) and the windows above the lowest one keep their own turn. DM_DOS2_TOUCH_ROUTE_BLOCK=0 off.
// 7. A HOST CLICK FOR THE WORLD (hle_dos2_touch.c dm_dos2_touch_world_click: CONFIRM / the action plate): the next left / right button down and
//    up go past every UI object (none sees them, [x5] stays 0), so the click lands in the world even when its point is under a HUD button
//    (run 21.11: CONFIRM's click at the CONFIRM circle came back to CONFIRM, again and again).
static const void *g_block_obj; static uint32_t g_block_type; static int g_block = 1; static unsigned g_n_block;
static unsigned g_skip_n; static uint64_t g_skip_until; static int g_walk_skip; static unsigned g_n_skip;
static uint64_t rt_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000ull + (uint64_t)t.tv_nsec / 1000000ull; }
static const struct { const char *name; uint16_t ids[12]; } k_groups[] = {
    {"hud", {0x3b, 0x26, 0x3d, 0x73, 0x75, 0x08, 0x3c, 0x8d}},   // 0x8d = the iPad column's own movie (dmColumn_d.swf, hle_dos2_touchui.c)
    // (pack 3s: in 3r that comment ran on over the next groups - panels / context menu / dialog got no taps)
    {"ctx", {0x0c, 0x60}}, {"panels", {0x09, 0x25, 0x8e, 0x40, 0x3f, 0x54, 0x4e, 0x55, 0x3e}}, {"dialog", {0x42, 0x65}},
    {"menus", {0x48, 0x57, 0x4b, 0x4c, 0x4a, 0x58, 0x59, 0x5a, 0x5b, 0x67, 0x04, 0x5e}}, {"menus", {0x4d}},
    {"panels", {0x46, 0x49, 0x89, 0x43}},   // journal_c, trade_c, reward_c, examine_c: no mouse code of their own, but their `_d` movies carry touch buttons (tools/dos2_panel_touch.py)
    {"panels", {0x8c}},   // journal_csp (the journal the engine really creates, run tw4: "kept away: not in the allow list"): journal_csp_d = button row + sub tabs by tap
};
static void rt_hexlist(const char *v, uint8_t val) {
    if (!v || !*v) return;
    char *dup = strdup(v), *save = NULL;
    for (char *t = dup ? strtok_r(dup, ",; ", &save) : NULL; t; t = strtok_r(NULL, ",; ", &save)) { char *end; const unsigned long id = strtoul(t, &end, 16); if (end != t && !*end && id < 256) g_allow[id] = val; }
    free(dup);
}
static const char *rt_event_name(uint32_t id) {   // the input manager's event names: id -> name (debug lines only)
    const uint8_t *im = *(const uint8_t *const volatile *)(g_main->base + RT_INMGR);
    if (!im) return "?";
    const uint32_t nb = *(const volatile uint32_t *)(im + 0x34); const uint8_t *const *bk = *(const uint8_t *const *const volatile *)(im + 0x38);
    if (!bk || !nb || nb > 0x10000) return "?";
    for (uint32_t b = 0; b < nb; b++) { int guard = 0;
        for (const uint8_t *nd = bk[b]; nd && guard++ < 512; nd = *(const uint8_t *const *)nd)
            if (*(const uint32_t *)(nd + 0x10) == id) { const char *nm = *(const char *const *)(nd + 0x48); return nm ? nm : "?"; } }
    return "?";
}
static uint32_t rt_event_id(const char *name) {
    const uint8_t *im = *(const uint8_t *const volatile *)(g_main->base + RT_INMGR);
    if (!im) return 0;
    const uint32_t nb = *(const volatile uint32_t *)(im + 0x34); const uint8_t *const *bk = *(const uint8_t *const *const volatile *)(im + 0x38);
    if (!bk || !nb || nb > 0x10000) return 0;
    for (uint32_t b = 0; b < nb; b++) { int guard = 0;
        for (const uint8_t *nd = bk[b]; nd && guard++ < 512; nd = *(const uint8_t *const *)nd) { const char *nm = *(const char *const *)(nd + 0x48); if (nm && !strcmp(nm, name)) return *(const uint32_t *)(nd + 0x10); } }
    return 0;
}
static void rt_names(void) {   // once, at the first event with the input manager up
    if (g_names_done || !*(void *const volatile *)(g_main->base + RT_INMGR)) return;
    g_names_done = 1;
    const uint32_t l = rt_event_id("FlashLeftMouse"), mv = rt_event_id("FlashMouseMove"), acc = rt_event_id("UIAccept");
    if (l != RT_EV_LEFT || mv != RT_EV_MOVE || !acc) {
        LOG("【红】[route] input event names: FlashLeftMouse = %#x (want 1), FlashMouseMove = %#x (want 0x58), UIAccept = %#x: the accept guard is off", l, mv, acc); g_accept = 0; return; }
    g_accept_id = acc;
    LOG("[route] input event ids by name: FlashLeftMouse %#x, FlashMouseMove %#x, UIAccept %#x, UICancel %#x, Action1 %#x, ContextMenu %#x%s", l, mv, acc, rt_event_id("UICancel"),
        rt_event_id("Action1"), rt_event_id("ContextMenu"), g_accept ? "; UIAccept right after a tap with no A on the pad is dropped" : "");
}
static const char *rt_movie(const uint8_t *o, char *buf, size_t n) {
    void dm_dos2_touchui_movie_of(const void *player, uint32_t *type, char *b, size_t bn);
    uint32_t ty; dm_dos2_touchui_movie_of(*(const void *const volatile *)(o + RT_OBJ_PLAYER), &ty, buf, n);
    return buf;
}
// one line per (type, decision): 0 delivered, 1 no mouse code, 2 under a modal, 3 hidden, 4 accept dropped
static void rt_say(const uint8_t *o, uint32_t type, uint32_t fl, int why, uint32_t id) {
    static uint8_t said[256];
    if (!g_dbg || type > 255 || (said[type] >> why) & 1) return;
    said[type] |= (uint8_t)(1u << why);
    char mv[48]; rt_movie(o, mv, sizeof mv);
    static const char *const k_why[5] = {"DELIVERED (a console movie with mouse code)", "kept away: not in the allow list (no mouse code / not wanted)", "kept away: under a modal window",
                                         "kept away: allowed but not visible", "UIAccept DROPPED: it came with a tap, A is not down on the pad"};
    if (why == 2) LOG("[route] type %#x %s (flags %#x), event %#x: %s (type %#x)", type, mv, fl, id, k_why[why], g_modal_type);
    else LOG("[route] type %#x %s (flags %#x), event %#x: %s", type, mv, fl, id, k_why[why]);
}
uint64_t dm_dos2_touch_a_ms(void);   // hle_dos2_touch.c: when the npad last had A down (ms, monotonic)
int dm_dos2_touch_in_game(void);     // hle_dos2_touch.c: a player UI set exists
int dm_dos2_touchui_is_block(uint32_t type);   // hle_dos2_touchui.c: a console menu that takes the whole input
static uint64_t rt_hook(uint64_t mgr, uint64_t obj, uint64_t ev, uint64_t pass, uint64_t a4, uint64_t a5) {
    const uint8_t *o = (const uint8_t *)(uintptr_t)obj, *e = (const uint8_t *)(uintptr_t)ev, *m = (const uint8_t *)(uintptr_t)mgr;
    if (!g_on || !o || !e || !m) return g_orig(mgr, obj, ev, pass, a4, a5);
    const uint32_t id = *(const uint32_t *)e, fl = *(const volatile uint32_t *)(o + RT_OBJ_FLAGS), type = *(const volatile uint32_t *)(o + RT_OBJ_TYPE);
    const int down = e[0x20] != 0;
    const int mouse = id == RT_EV_LEFT || id == RT_EV_RIGHT || id == RT_EV_MIDDLE || id == RT_EV_MOVE || id == RT_EV_SCROLL_UP || id == RT_EV_SCROLL_DOWN;
    { void *const *list = *(void *const *const volatile *)(m + RT_MGR_LIST); const uint64_t n = (*(const volatile uint64_t *)(m + RT_MGR_LISTN) >> 24) & 0xffffff;
      if (list && n && n < 0x10000 && list[n - 1] == (const void *)o) {   // the top object: a walk starts
          g_modal_seen = 0;
          if (!g_names_done) rt_names();
          // 6. the lowest visible PC window of this walk (UI mode TOUCH, in game)
          g_block_obj = NULL;
          if (g_block && mouse && *(const volatile uint8_t *)(g_main->base + RT_MODE_BYTE) == 0 && dm_dos2_touch_in_game()) {
              for (uint64_t i = 0; i < n; i++) {
                  const uint8_t *x = (const uint8_t *)list[i]; if (!x) continue;
                  const uint32_t xf = *(const volatile uint32_t *)(x + RT_OBJ_FLAGS), xt = *(const volatile uint32_t *)(x + RT_OBJ_TYPE);
                  const uint8_t vis = (xf & RT_F_VISIBLE) && (xf & RT_F_INPUT);
                  if (dt_lowest_blocker(&xt, &vis, 1) == 0) { g_block_obj = x; g_block_type = xt; break; }
              }
          }
          // 7. a host world click: decided at the start of pass 1 of a button event, kept for its pass 2
          const int btn = id == RT_EV_LEFT || id == RT_EV_RIGHT || id == RT_EV_MIDDLE;
          if (!btn) g_walk_skip = 0;
          else if (pass & 1) {
              g_walk_skip = 0;
              if (g_skip_n && rt_ms() < g_skip_until) { g_skip_n--; g_walk_skip = 1; g_n_skip++;
                  if (g_dbg) LOG("[route] %s %s: a host click for the world (CONFIRM / action plate): no UI object gets it", id == RT_EV_LEFT ? "left" : "right", down ? "down" : "up"); }
              else g_skip_n = 0;
          }
          if (g_dbg && !mouse && (pass & 1) && rt_ms() - g_left_ms < (uint64_t)g_accept_ms) {   // what else a tap makes: the answer to "where does the accept come from"
              static uint32_t seen[48]; static int ns; const uint32_t key = id << 1 | (uint32_t)down; int have = 0;
              for (int i = 0; i < ns; i++) if (seen[i] == key) have = 1;
              if (!have && ns < 48) { seen[ns++] = key; LOG("[route] within %d ms of a tap the UI gets input event %#x \"%s\" %s (player %d, npad A %s)", g_accept_ms, id, rt_event_name(id), down ? "down" : "up",
                                                           *(const int32_t *)(e + 4), rt_ms() - dm_dos2_touch_a_ms() < 300 ? "DOWN" : "not down"); }
          }
      } }
    if (g_walk_skip && (id == RT_EV_LEFT || id == RT_EV_RIGHT || id == RT_EV_MIDDLE)) return 0;   // 7. the host's world click: past every UI object
    if (!(fl & RT_F_INPUT)) return g_orig(mgr, obj, ev, pass, a4, a5);   // the game does nothing with it either
    const int in_game = dm_dos2_touch_in_game();
    // UI mode touch (mode byte main+0x2bb8918 = 0, hle_dos2_uimode.c): the PC movies are up and every one of them has the PC's own mouse code -
    // the engine's own routing stands (no allow list, no modal / accept guard: those are for the console movies); the bookkeeping below stays
    const int pcui = *(const volatile uint8_t *)(g_main->base + RT_MODE_BYTE) == 0;
    // run 20.11 review: TOUCH still plays console `_d` menus (the main menu and what it makes by its own type: options, mods, save / load,
    // message boxes, character creation, tutorial boxes) - an allow-listed type keeps the modal guard, the visibility test and the accept guard
    // at the mode byte 0 too; only the PC movies (not in the list) stand on the engine's own routing
    const int listed = type < 256 && g_allow[type];
    const int routed = (g_game || !in_game) && (!pcui || listed);
    // dmTouch kit (DM_DOS2_TOUCH_KIT=1, hle_dos2_touch_kit.c; TOUCH_UNIFIED K2): while a kit gesture is in flight the mouse goes to its target
    // only - the other objects do not see it (0 = not handled, the walk goes on), the target gets it past the allow list and the modal guard
    // (the kit chose it top-down with the modal rule already). g_kit_obj is NULL unless the kit is on and a kit finger is down.
    const void *const kit_obj = g_kit_obj;
    if (kit_obj && mouse && o != (const uint8_t *)kit_obj) { g_n_drop++; return 0; }
    int why = 0;
    if (kit_obj && mouse) {}
    else if (mouse && routed) {
        if (g_modal && g_modal_seen) why = 2;
        else if (!listed) why = 1;
        else if (!(fl & RT_F_VISIBLE) && !g_hidden) why = 3;
    } else if (!mouse && (!pcui || listed) && g_accept && g_accept_id && id == g_accept_id) {
        const uint64_t now = rt_ms();
        if (now - g_left_ms < (uint64_t)g_accept_ms && now - dm_dos2_touch_a_ms() > 300 && type != 0x48 && type != 0x57 && type != 0x17 &&
            (in_game || (type < 256 && g_allow[type]) || dm_dos2_touchui_is_block(type))) why = 4;
    }
    // the kit's one accept guard (K7), both modes: UIAccept to the kit target soon after a touch press, not made by the pad's A or a host pulse
    if (!why && !mouse && g_kit_accept && g_accept_id && id == g_accept_id && g_kit_accept(o)) why = 4;
    if ((fl & RT_F_VISIBLE) && (fl & RT_F_MODAL) && !g_modal_seen) { g_modal_seen = 1; g_modal_type = type; }   // for the objects below this one
    if (mouse || why) rt_say(o, type, fl, why, id);
    if (why) { if (why == 4) g_n_accept++; else if (why == 2) g_n_modal++; else g_n_drop++; return 0; }   // 0 = not handled, the walk goes on
    // 5. a stage-listener movie: its "handled" for a button says nothing about the point
    const int stage = (id == RT_EV_LEFT || id == RT_EV_RIGHT || id == RT_EV_MIDDLE) && !(pass & 1) && a5 && type < 256 && g_stage[type] && in_game;
    uint16_t pre_h = 0; uint8_t pre_c = 0; uint64_t pre_o = 0;
    if (stage) { pre_h = *(const volatile uint16_t *)(uintptr_t)a5; pre_c = m[RT_UIM_CAPT]; pre_o = *(const volatile uint64_t *)(m + RT_UIM_CAPT_OBJ); }
    const uint64_t v = g_orig(mgr, obj, ev, pass, a4, a5);
    if (stage && !pre_h && !pre_c && *(const volatile uint16_t *)(uintptr_t)a5) {
        uint8_t *mw = (uint8_t *)(uintptr_t)mgr;
        float cx = -1, cy = -1;
        int dm_dos2_touch_stage_press(const void *player, float *x, float *y);   // hle_dos2_touch.c
        const int hr = dm_dos2_touch_stage_press(*(const void *const volatile *)(o + RT_OBJ_PLAYER), &cx, &cy);
        if (!dt_stage_press_keep(1, 1, hr)) {
            *(volatile uint16_t *)(uintptr_t)a5 = 0;
            mw[RT_UIM_CAPT] = pre_c; *(volatile uint64_t *)(mw + RT_UIM_CAPT_OBJ) = pre_o;
            g_n_stage++;
            static unsigned n; if (g_dbg ? n++ < 200 : n++ < 6) { char mv[48]; LOG("[route] %s %s at %.0f,%.0f: %s (type %#x) answered it with its stage listener, outside its HUD rects: "
                "NOT taken (the objects below and the world get it)", id == RT_EV_LEFT ? "left" : id == RT_EV_RIGHT ? "right" : "middle", down ? "press" : "release",
                (double)cx, (double)cy, rt_movie(o, mv, sizeof mv), type); }
        } else {
            if (hr == DT_HR_NONE) g_n_stage_norect++; else g_n_stage_kept++;
            static unsigned n; if (g_dbg && n++ < 200) { char mv[48]; LOG("[route] %s %s at %.0f,%.0f: %s (type %#x) keeps it (%s)", id == RT_EV_LEFT ? "left" : id == RT_EV_RIGHT ? "right" : "middle",
                down ? "press" : "release", (double)cx, (double)cy, rt_movie(o, mv, sizeof mv), type, hr == DT_HR_IN ? "inside one of its HUD rects" : "no live HUD rects of it: its answer stands"); }
        }
    }
    // 6. the lowest open PC window has had its turn: nothing below it gets this mouse event (the engine's own "an upper object took it")
    if (mouse && !(pass & 1) && a5 && o == (const uint8_t *)g_block_obj && !*(const volatile uint16_t *)(uintptr_t)a5) {
        *(volatile uint16_t *)(uintptr_t)a5 = 1;
        if (id != RT_EV_MOVE) { g_n_block++;
            static uint8_t said[256]; if (g_dbg && type < 256 && !said[type]) { said[type] = 1; char mv[48];
                LOG("[route] type %#x %s is the lowest open PC window: a %s it did not take stops there (nothing below it, nor the world, gets it; DM_DOS2_TOUCH_ROUTE_BLOCK=0 off)",
                    type, rt_movie(o, mv, sizeof mv), id == RT_EV_LEFT ? "left button" : id == RT_EV_RIGHT ? "right button" : "mouse event"); } }
    }
    if (g_kit_note && !mouse) g_kit_note(o, type, id, down, (int)(pass & 1), (unsigned)(v & 0xff), a5 ? *(const volatile uint16_t *)(uintptr_t)a5 : 0xffff);   // M0-3 trace (kit)
    if (mouse) g_n_pass++;
    if (id == RT_EV_LEFT && down && !(pass & 1)) {   // pass 2 of a left press: [x5] = this object's answer (main+0xaea8d4)
        g_seen_seq = g_left_seq;
        if (a5 && *(const volatile uint8_t *)(uintptr_t)a5 && g_taken_seq != g_left_seq) {
            g_taken_seq = g_left_seq; g_taken_type = type;
            static unsigned n; if (g_dbg && n++ < 200) { char mv[48]; LOG("[route] left press #%u taken by type %#x %s", g_left_seq, type, rt_movie(o, mv, sizeof mv)); }
        }
    }
    return v;
}
// ---- for hle_dos2_touch_kit.c (game thread) ----
void dm_dos2_route_kit_target(const void *obj);   // K2: the kit gesture's target (NULL = none)
void dm_dos2_route_kit_target(const void *obj) { g_kit_obj = obj; }
void dm_dos2_route_kit_hooks(int (*accept)(const void *), void (*note)(const void *, uint32_t, uint32_t, int, int, unsigned, unsigned));
void dm_dos2_route_kit_hooks(int (*accept)(const void *), void (*note)(const void *, uint32_t, uint32_t, int, int, unsigned, unsigned)) { g_kit_accept = accept; g_kit_note = note; }
uint32_t dm_dos2_route_event_id(const char *name);   // an input event id by name (0 = unknown / the input manager not up yet)
uint32_t dm_dos2_route_event_id(const char *name) { return g_main ? rt_event_id(name) : 0; }
const char *dm_dos2_route_event_name(uint32_t id);
const char *dm_dos2_route_event_name(uint32_t id) { return g_main ? rt_event_name(id) : "?"; }
// M0 experiment 2: UIManager's per-object dispatch main+0xaea7b0 itself (the original, past this hook) for one object; x4 = the manager's
// per-player block (uimgr + 0x158 + player * 0x20, as main+0xae8450), x5 = the handled flag; NULL when the hook is not in (sites differ)
typedef uint64_t (*dm_route_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
dm_route_fn dm_dos2_route_dispatch_orig(void);
dm_route_fn dm_dos2_route_dispatch_orig(void) { return g_on ? g_orig : NULL; }
// ---- for hle_dos2_touch.c (game thread) ----
int dm_dos2_route_on(void);
int dm_dos2_route_on(void) { return g_on; }
int dm_dos2_route_game(void);
int dm_dos2_route_game(void) { return g_on && g_game; }
void dm_dos2_route_left(int down);   // a touch-made left record goes to InjectInput now
void dm_dos2_route_left(int down) { g_left_ms = rt_ms(); if (down) g_left_seq++; }
void dm_dos2_route_world_click(void);   // 7. the next button down + up (within 600 ms) go past every UI object: a host click meant for the world
void dm_dos2_route_world_click(void) { if (g_on) { g_skip_n = 2; g_skip_until = rt_ms() + 600; } }
int dm_dos2_route_taken(uint32_t *type);   // the last left press: 1 = a UI object took it, 0 = the UI saw it and none did, -1 = not seen by the UI yet
int dm_dos2_route_taken(uint32_t *type) {
    if (!g_on) return -1;
    if (g_taken_seq == g_left_seq && g_left_seq) { if (type) *type = g_taken_type; return 1; }
    return g_seen_seq == g_left_seq && g_left_seq ? 0 : -1;
}
int dm_dos2_route_world_ok(uint32_t *type);   // 0 = a modal window, a panel, a dialog or a menu is on screen (the finger is not for the world); *type = which
int dm_dos2_route_world_ok(uint32_t *type) {
    if (!g_main) return 1;
    const uint8_t *m = *(const uint8_t *const volatile *)(g_main->base + RT_UIMGR);
    if (!m) return 1;
    const uint8_t *const *objs = *(const uint8_t *const *const volatile *)(m + RT_MGR_OBJS); const uint32_t n = *(const volatile uint32_t *)(m + RT_MGR_NOBJ);
    if (!objs || n > 0x10000) return 1;
    // UI mode touch (the mode byte 0): the PC windows are menus for the gestures too (review of run 20.11). The lists above are the console's; a
    // visible PC panel was "no menu", so a finger on it took the world's probe path: taken by the panel = a plain mouse drag (G_DRAG), and that
    // path has no long press - a long press on an inventory item could never be the right click (ContextMenu) and the PC's item / skill context
    // menu was out of reach. As a menu the finger is the UI's (hle_dos2_touch.c G_PENDING: tap = click, hold = right click, the short hold =
    // the hover tooltip, a move = drag), the camera keys let go, and world taps / pans wait until the window is closed (the iPad's windows).
    // inventory 0x74, character sheet 0x77, skills 0x29, crafting 0x66, journal / map 0x16, examine 0x68, trade 0x2e, waypoints 0x2f, dialog 0xe,
    // reward 0x88, item splitter 0x15, the PC context menus 0xa / 0xb (containers 9 / 0x25 are in the console's list already): dt_pc_menu_type
    const int pcui = *(const volatile uint8_t *)(g_main->base + RT_MODE_BYTE) == 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *o = objs[i]; if (!o) continue;
        const uint32_t fl = *(const volatile uint32_t *)(o + RT_OBJ_FLAGS), ty = *(const volatile uint32_t *)(o + RT_OBJ_TYPE);
        if (!(fl & RT_F_VISIBLE)) continue;
        if (pcui && dt_pc_menu_type(ty)) { if (type) *type = ty; return 0; }
        // pack 3h (owner, run t3g: "after a save the controls died"): the pause menu's save left tutorialBox_d (0x5e, non-modal, flags 0x2600a)
        // flagged visible with nothing on screen; as a "menu" it turned the floating sticks off and kept every tap from the world. A non-modal
        // tutorial box is an overlay (its own hold-A prompt has tools/dos2_taps_touch.py taps): the world stays the finger's.
        if (ty == 0x5e && !(fl & RT_F_MODAL)) continue;
        int menu = (fl & RT_F_MODAL) != 0;
        for (unsigned g = 1; g < sizeof k_groups / sizeof *k_groups && !menu; g++) for (int k = 0; k < 12 && k_groups[g].ids[k]; k++) if (k_groups[g].ids[k] == ty) menu = 1;   // every group but the HUD
        if (ty == 0x46 || ty == 0x8c || ty == 0x49 || ty == 0x3e || ty == 0x53 || ty == 0x44 || ty == 0x43 || ty == 0x89) menu = 1;   // journal, trade, skills, panel select, area interact, examine, reward
        if (ty == 0x52) menu = 1;   // dos2-touch7: the party wheel (partyManagement_c, ZL / ZR): its fingers are the wheel's (hle_dos2_touch.c radial), no floating stick
        if (menu) { if (type) *type = ty; return 0; }
    }
    return 1;
}
int dm_dos2_route_visible(uint32_t want);   // hle_dos2_touch.c map gestures: is a visible UI object of that type on screen (1 / 0; -1 = no UI manager)
int dm_dos2_route_visible(uint32_t want) {
    if (!g_main) return -1;
    const uint8_t *m = *(const uint8_t *const volatile *)(g_main->base + RT_UIMGR);
    if (!m) return -1;
    const uint8_t *const *objs = *(const uint8_t *const *const volatile *)(m + RT_MGR_OBJS); const uint32_t n = *(const volatile uint32_t *)(m + RT_MGR_NOBJ);
    if (!objs || n > 0x10000) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *o = objs[i]; if (!o) continue;
        if (*(const volatile uint32_t *)(o + RT_OBJ_TYPE) == want && (*(const volatile uint32_t *)(o + RT_OBJ_FLAGS) & RT_F_VISIBLE)) return 1;
    }
    return 0;
}
const void *dm_dos2_route_player(uint32_t want);   // dos2-touch7: the FlashPlayer of a visible UI object of that type (NULL = none / no UI manager)
const void *dm_dos2_route_player(uint32_t want) {
    if (!g_main) return NULL;
    const uint8_t *m = *(const uint8_t *const volatile *)(g_main->base + RT_UIMGR);
    if (!m) return NULL;
    const uint8_t *const *objs = *(const uint8_t *const *const volatile *)(m + RT_MGR_OBJS); const uint32_t n = *(const volatile uint32_t *)(m + RT_MGR_NOBJ);
    if (!objs || n > 0x10000) return NULL;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *o = objs[i]; if (!o) continue;
        if (*(const volatile uint32_t *)(o + RT_OBJ_TYPE) == want && (*(const volatile uint32_t *)(o + RT_OBJ_FLAGS) & RT_F_VISIBLE)) return *(const void *const volatile *)(o + RT_OBJ_PLAYER);
    }
    return NULL;
}
void dm_dos2_route_summary(void);   // hle_dos2_touch.c's DM_DBG summary
void dm_dos2_route_summary(void) {
    if (g_on) LOG("[route] mouse events delivered %u, kept away %u (no mouse code / hidden) + %u (under a modal), UIAccept dropped %u; left presses %u, last taken #%u by type %#x; "
                  "stage-listener movies: button events given back %u, kept inside a HUD rect %u, kept with no rects %u; stopped below an open PC window %u (last %#x), "
                  "host world clicks past the UI %u",
                  g_n_pass, g_n_drop, g_n_modal, g_n_accept, g_left_seq, g_taken_seq, g_taken_type, g_n_stage, g_n_stage_kept, g_n_stage_norect, g_n_block, g_block_type, g_n_skip);
}
void dm_dos2_touchui_route_install(void);   // main.c host_preload, after dm_dos2_touchui_install and before dm_dos2_touch_install
void dm_dos2_touchui_route_install(void) {
    int dm_dos2_touchui_on(void);
    const char *e = getenv("DM_DOS2_TOUCH_ROUTE"), *v;
    if (!dm_dos2_touchui_on()) return;
    if (e && *e == '0') { LOG("[route] DM_DOS2_TOUCH_ROUTE=0: no per-movie mouse routing (the window gate of pack 2v decides the filter byte)"); return; }
    g_dbg = (v = getenv("DM_DBG")) && atoi(v) > 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_main = &g_mod[i];
    module_t *m = g_main;
    if (!m) return;
    static const rt_site k_sites[] = {{RT_DISPATCH, 10, k_rt_dispatch}, {RT_CALL1, 9, k_rt_call1}, {RT_CALL2, 9, k_rt_call2}, {RT_MODAL, 7, k_rt_modal}, {RT_MODAL2, 4, k_rt_modal2},
        {RT_HANDLED, 2, k_rt_handled}, {RT_CAPTURE, 7, k_rt_capture}, {RT_UPPER, 10, k_rt_upper}, {RT_NAMES_LD, 4, k_rt_names_ld}, {RT_NAMES, 4, k_rt_names}, {RT_NAME_ID, 2, k_rt_name_id}, {RT_FLASH_IDS, 6, k_rt_flash_ids}};
    for (unsigned i = 0; i < sizeof k_sites / sizeof *k_sites; i++)
        if ((uint64_t)k_sites[i].off + 4ull * k_sites[i].n > m->text_end || memcmp(m->base + k_sites[i].off, k_sites[i].want, 4ull * k_sites[i].n)) {
            LOG("【红】[route] main+%#x differs: not DOS2 update v786432 main (build 4979b200) at the UI manager's input dispatch: no per-movie mouse routing (the old window gate stays)", k_sites[i].off);
            g_main = NULL; return; }
    g_game = !((v = getenv("DM_DOS2_TOUCH_ROUTE_GAME")) && *v == '0');
    g_modal = !((v = getenv("DM_DOS2_TOUCH_ROUTE_MODAL")) && *v == '0');
    g_accept = !((v = getenv("DM_DOS2_TOUCH_ROUTE_ACCEPT")) && *v == '0');
    g_hidden = (v = getenv("DM_DOS2_TOUCH_ROUTE_HIDDEN")) && atoi(v) > 0;
    g_block = !((v = getenv("DM_DOS2_TOUCH_ROUTE_BLOCK")) && *v == '0');
    if ((v = getenv("DM_DOS2_TOUCH_ROUTE_ACCEPT_MS")) && atoi(v) >= 100 && atoi(v) <= 5000) g_accept_ms = atoi(v);
    const char *gr = getenv("DM_DOS2_TOUCH_ROUTE_GROUPS");
    for (unsigned g = 0; g < sizeof k_groups / sizeof *k_groups; g++) {
        int use = 1;
        if (gr && *gr) { const char *p = strstr(gr, k_groups[g].name); const size_t l = strlen(k_groups[g].name); use = p && (p == gr || p[-1] == ',') && (!p[l] || p[l] == ','); }
        for (int k = 0; use && k < 12 && k_groups[g].ids[k]; k++) g_allow[k_groups[g].ids[k]] = 1;
    }
    rt_hexlist(getenv("DM_DOS2_TOUCH_ROUTE_ALLOW"), 1); rt_hexlist(getenv("DM_DOS2_TOUCH_ROUTE_DENY"), 0);
    {   // 5. DM_DOS2_TOUCH_STAGE_TYPES: unset = "3b" (bottomBar_d); "" / "0" = none
        const char *st = getenv("DM_DOS2_TOUCH_STAGE_TYPES");
        memset(g_stage, 0, sizeof g_stage);
        if (!st) g_stage[0x3b] = 1;
        else {
            char *dup = strdup(st), *save = NULL;
            for (char *t = dup ? strtok_r(dup, ",; ", &save) : NULL; t; t = strtok_r(NULL, ",; ", &save)) { char *end; const unsigned long id = strtoul(t, &end, 16); if (end != t && !*end && id && id < 256) g_stage[id] = 1; }
            free(dup);
        }
    }
    uint32_t *t = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) { LOG("【红】[route] mmap failed: no per-movie mouse routing"); g_main = NULL; return; }
    const uint64_t rest = (uint64_t)(uintptr_t)(m->base + RT_DISPATCH + 16);
    memcpy(t, k_rt_dispatch, 16); t[4] = 0x58000050 /* ldr x16,#8 */; t[5] = 0xd61f0200 /* br x16 */; t[6] = (uint32_t)rest; t[7] = (uint32_t)(rest >> 32);
    if (mprotect(t, 0x4000, PROT_READ | PROT_EXEC)) { LOG("【红】[route] mprotect RX failed: no per-movie mouse routing"); munmap(t, 0x4000); g_main = NULL; return; }
    __builtin___clear_cache((char *)t, (char *)(t + 8));
    g_orig = (rt_fn)(void *)t;
    const uint64_t to = (uint64_t)(uintptr_t)(void *)rt_hook;
    const uint32_t jump[4] = {0x58000050, 0xd61f0200, (uint32_t)to, (uint32_t)(to >> 32)};
    uint32_t *at = (uint32_t *)(void *)(m->base + RT_DISPATCH);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF);
    const size_t len = (size_t)(((uintptr_t)(at + 4) - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) { LOG("【红】[route] mprotect RW failed: no per-movie mouse routing"); g_orig = NULL; g_main = NULL; return; }
    memcpy(at, jump, 16);
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[route] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)(at + 4));
    g_on = 1;
    char list[640]; size_t lo = 0; list[0] = 0;
    for (int i = 0; i < 256 && lo + 8 < sizeof list; i++) if (g_allow[i]) lo += (size_t)snprintf(list + lo, sizeof list - lo, "%s%x", lo ? " " : "", i);
    LOG("[route] DM_DOS2_TOUCH_ROUTE=1: UI input dispatch main+%#x hooked: mouse events go only to visible UI objects of types [%s] (%s), modal guard %s, "
        "tap-is-not-accept guard %s (%d ms)%s; UI mode TOUCH: an open PC window stops the mouse below it %s", RT_DISPATCH, list, g_game ? "menus and in game" : "menus only: DM_DOS2_TOUCH_ROUTE_GAME=0",
        g_modal ? "on" : "OFF", g_accept ? "on" : "OFF", g_accept_ms, g_hidden ? ", hidden objects too" : "", g_block ? "on" : "OFF (DM_DOS2_TOUCH_ROUTE_BLOCK=0)");
    {
        char sl[200]; size_t so = 0; sl[0] = 0;
        for (int i = 1; i < 256 && so + 8 < sizeof sl; i++) if (g_stage[i]) so += (size_t)snprintf(sl + so, sizeof sl - so, "%s%x", so ? " " : "", i);
        LOG("[route] stage-listener movies (DM_DOS2_TOUCH_STAGE_TYPES): [%s]: in game their \"handled\" for a button counts only inside their own HUD rects, "
            "outside it the press goes on to the objects below and the world%s", sl, sl[0] ? "" : " - NONE (the touch6 behaviour)");
    }
}
#endif
