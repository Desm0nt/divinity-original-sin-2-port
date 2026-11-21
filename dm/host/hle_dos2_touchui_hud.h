// [DOS2] Pure rules of the touch HUD (hle_dos2_touchui_hud.c; docs/dos2/TOUCH_MODES_PLAN.md "Implemented: touch HUD"), kept apart so a host
// test can check them without the game (dm/host/tests/dos2_touchui_hud_test.c).
#ifndef HLE_DOS2_TOUCHUI_HUD_H
#define HLE_DOS2_TOUCHUI_HUD_H
#include <math.h>
#include <stdint.h>
#include "hle_dos2_uimode.h"

// Does the host make the PC HUD when a player UI set is built with the mode byte 0: UI mode TOUCH (requested or still running its HUD part), or
// the old switch DM_DOS2_TOUCHUI_HUD=1 in any mode; DM_DOS2_TOUCHUI_HUD=0 (env_hud 0) = never. env_hud: -1 unset, 0, 1.
static inline int hu_active(int env_hud, int ui_mode, int ui_hud_mode) {
    if (env_hud == 0) return 0;
    return env_hud > 0 || ui_mode == UI_TOUCH || ui_hud_mode == UI_TOUCH;
}
// The engine's "fixed" layout (main+0xae3be0, branch 0xae3c8c): k = c x S x h / D, c = the object's class scale (+0x94), S = the "UI scale"
// setting ([[main+0x2bb8a60]+0x34]), D = u32 [uimgr+0x110] (1080, the UI manager's constructor 0xae7438), h = the window height.
static inline float hu_fixed_k(float c, float s, float h, float d) { return d > 0 ? c * s * h / d : 0; }
// The class scale the host gives a touch HUD object so that one stage px of the movie is `tune` screen px on a D-high screen (x the iPad's own
// class scale of that movie: 1.1 for the portraits): the setting S is cancelled. An unusable S (not 0.2..4) counts as 1.
static inline float hu_class_scale(float tune, float ipad_c, float s) {
    if (!(s >= 0.2f && s <= 4.0f)) s = 1.0f;
    return tune * ipad_c / s;
}
// DM_DOS2_TOUCH_HUD_SCALE: the one size factor (default 1 = the iPad's px on a 1080-high screen: hot bar pitch 86, cell 74 x 78); 0 = the host
// leaves the class scales alone; else clamped to 0.5..2
static inline float hu_tune(float v, int given) {
    if (!given) return 1.0f;
    if (v == 0) return 0;
    return v < 0.5f ? 0.5f : v > 2.0f ? 2.0f : v;
}
// ExternalInterface dmSide(code) of dmSide_p.swf / minimap_p.swf: 1 = options (the pause menu), 2..7 = the iPad column's panels in its order
// (map, character, inventory, skills, crafting, journal) -> the id the PC hot bar's own button sends in hotbarBtnPressed (jump table
// main+0x1f52e1e -> main+0x1230904..: 0 = the GAME MENU (0x10c6360 toggles the record's +0x88 gameMenu: "ico_menu" is the menu, not the
// sheet - phone run 21.11: the armour icon opened the pause menu), 1 = the inventory AND the sheet together (0x10d6240: both up -> both
// closed, else both opened: the DE PC has no toggle of the sheet alone), 2 skills, 3 crafting, 4 map, 5 journal); -1 = not a panel
static inline int hu_side_panel(int code) {
    static const int8_t k[6] = {4, 1, 1, 2, 3, 5};
    return code >= 2 && code <= 7 ? k[code - 2] : -1;
}
// One window at a time, as on the iPad (owner 20.11): the panels the column opens are one group; opening one closes the others. The one pair is
// the EQUIPMENT VIEW (dmSide 8, from the doll button of partyInventory_p / characterSheet_p): the sheet movie shows only its doll panel and the
// inventory stands right of it (both in their state 1, tools/dos2_ppanel_inv.py). Bits of the group / the PC hot bar id that toggles each:
enum { HU_P_SHEET = 1, HU_P_INV = 2, HU_P_SKILLS = 4, HU_P_CRAFT = 8, HU_P_JOURNAL = 16, HU_P_ALL = 31, HU_VIEW_EQUIP = 8, HU_PAIR_GRACE_MS = 400 };
static inline int hu_panel_pc(int bit) { return bit == HU_P_SHEET ? 1 : bit == HU_P_INV ? 1 : bit == HU_P_SKILLS ? 2 : bit == HU_P_CRAFT ? 3 : bit == HU_P_JOURNAL ? 5 : -1; }
static inline int hu_view_want(int code) {
    return code == 3 ? HU_P_SHEET : code == 4 ? HU_P_INV : code == 5 ? HU_P_SKILLS : code == 6 ? HU_P_CRAFT : (code == 2 || code == 7) ? HU_P_JOURNAL : code == HU_VIEW_EQUIP ? HU_P_SHEET | HU_P_INV : 0;
}
// dmSide(code) with the panels `vis` on the screen and the host's last view (HU_VIEW_EQUIP while the pair is up, else 0): which panels get their
// toggle to close (*close), which to open (*open), whether the code's own PC button is still to be sent as before (*press: map / journal, one
// window with two pages - the engine switches or closes it itself), and the state the sheet / the inventory are told (*state: 1 = the pair,
// 0 = plain, -1 = nothing to tell). Returns the new view.
static inline int hu_view_plan(int code, int vis, int view, int *close, int *open, int *press, int *state) {
    const int want = hu_view_want(code);
    *close = *open = *press = 0; *state = -1;
    if (!want) return view;
    vis &= HU_P_ALL;
    if (want == HU_P_JOURNAL) { *close = vis & ~want; *press = 1; return 0; }
    const int pair = code == HU_VIEW_EQUIP;
    if (vis == want && (want & (HU_P_SHEET | HU_P_INV)) == want && (view == HU_VIEW_EQUIP) != pair) { *state = pair; return pair ? HU_VIEW_EQUIP : 0; }   // the same windows, the other look
    if (vis == want) { *close = want; if (want & (HU_P_SHEET | HU_P_INV)) *state = 0; return 0; }                                                       // the same view again: it closes
    *close = vis & ~want; *open = want & ~vis;
    if (want & (HU_P_SHEET | HU_P_INV)) *state = pair;
    return pair ? HU_VIEW_EQUIP : 0;
}
// The sheet and the inventory have ONE engine toggle (hot bar id 1 = main+0x10d6240: both up -> both closed; otherwise both opened) and one
// "hide" each (main+0x10c01d0 the sheet, main+0x10c7b30 the inventory: vt+0x58 of the record's +0x238 / +0x228 object when it is visible).
// From the halves on the screen (vis) to the halves wanted (want), both masked to HU_P_SHEET | HU_P_INV: *press = send the toggle once,
// *hide = the halves to hide after it. Run 21.11: "the bag opens the inventory AND the character stats" (the toggle opened both, the host
// believed id 1 was the inventory alone) and "the armour icon opens the pause menu" (id 0).
static inline void hu_pair_ops(int vis, int want, int *press, int *hide) {
    const int pair = HU_P_SHEET | HU_P_INV;
    vis &= pair; want &= pair; *press = 0; *hide = 0;
    if (vis == want) return;
    if (want & ~vis) { *press = 1; *hide = pair & ~want; return; }   // something to open: the toggle opens both (not both are up), the other half goes
    if (!want && vis == pair) { *press = 1; return; }                 // both up, both to go: the toggle closes them as the PC does
    *hide = vis & ~want;                                               // one half (of what is up) to go
}
// the pair lives and dies together: one of the two was closed by its own close button -> the other goes too (after the grace: an opening pair
// is not "one closed"). 1 = close what is left of it and forget the view.
static inline int hu_pair_broken(int view, int vis, uint64_t ms_since_plan) {
    return view == HU_VIEW_EQUIP && ms_since_plan >= HU_PAIR_GRACE_MS && (vis & (HU_P_SHEET | HU_P_INV)) != (HU_P_SHEET | HU_P_INV);
}
// Run 21.11 (owner: the inventory's close button at the left of its header, the journal's close missed with a finger): a tap in the
// TOP-RIGHT CORNER of an open panel closes it, whatever its movie did with its own close button. The windows are centred on the screen
// (design px of a 1080-high screen, sw = its width): journal / map 1197 wide (tools/dos2_ppanel_skills.py x0.88), skills 1211, crafting 612,
// all from y 8; the inventory 770 x min(cell / 50, (sw - 160) / 770) and the sheet 680 x min(1.7 cell / 76, (sw - 160) / 680, 1080 / 572)
// (tools/dos2_ppanel_inv.py inv_spec / cs_spec, top 0); the equipment view the doll panel (408.7) + the inventory as one block. The zone:
// HU_CORNER px left of the right edge .. 30 right of it, 10 above the top .. HU_CORNER below it. vis = HU_P_* bits; returns the dmSide code
// that closes the window hit (3 sheet, 4 inventory, 5 skills, 6 crafting, 8 the equipment view; 7 = the journal / map: the host sends the
// escape key there - its toggle switches the page instead when the other page is shown), 0 = not in a corner.
enum { HU_CORNER = 120 };
static inline int hu_corner_code(int vis, int view, float x, float y, float sw, int cell) {
    const float mx = sw - 160.0f, doll = 341.0f * 1080.0f / 901.0f;
    float w = 0, top = 8; int code = 0;
    const int pair = (vis & (HU_P_SHEET | HU_P_INV)) == (HU_P_SHEET | HU_P_INV);
    if (vis & HU_P_JOURNAL) { w = 1197; code = 7; }
    else if (vis & HU_P_SKILLS) { w = 1211; code = 5; }
    else if (vis & HU_P_CRAFT) { w = 612; code = 6; }
    else if (pair && view == HU_VIEW_EQUIP) { float s = cell / 50.0f, a = (mx - doll) / 770.0f; w = doll + 770.0f * (s < a ? s : a); top = 0; code = 8; }
    else if (vis & HU_P_INV) { float s = cell / 50.0f, a = mx / 770.0f; w = 770.0f * (s < a ? s : a); top = 0; code = 4; }
    else if (vis & HU_P_SHEET) { float s = 1.7f * cell / 76.0f, a = mx / 680.0f, b = 1080.0f / 572.0f; s = s < a ? s : a; s = s < b ? s : b; w = 680.0f * s; top = 0; code = 3; }
    if (!code) return 0;
    const float right = sw / 2 + w / 2;
    return x >= right - HU_CORNER && x <= right + 30 && y >= top - 10 && y <= top + HU_CORNER ? code : 0;
}
// the UI type of the window a dmSide panel code opens (the host's object in the player's record): map / journal 0x16, sheet 0x77,
// inventory 0x74, skills 0x29, crafting 0x66; -1 = not a panel
static inline int hu_side_type(int code) {
    static const int16_t k[6] = {0x16, 0x77, 0x74, 0x29, 0x66, 0x16};
    return code >= 2 && code <= 7 ? k[code - 2] : -1;
}
// The aimed point for CONFIRM / cancel: the last place the cursor RESTED before it went to where it is now (a tap on the button moves the cursor
// onto the button; the click must land where the player aimed). Fed with the cursor on every poll.
typedef struct { float x, y; uint64_t since; float rx, ry; int have, rest; } hu_aim;
enum { HU_AIM_SAME_PX = 48 /* moves inside this are the same spot (the buttons are 69 / 118 px) */, HU_AIM_REST_MS = 60 };
static inline void hu_aim_feed(hu_aim *a, float x, float y, uint64_t now_ms) {
    if (!a->have) { a->x = x; a->y = y; a->since = now_ms; a->have = 1; return; }
    if (fabsf(x - a->x) <= HU_AIM_SAME_PX && fabsf(y - a->y) <= HU_AIM_SAME_PX) return;
    if (now_ms - a->since >= HU_AIM_REST_MS) { a->rx = a->x; a->ry = a->y; a->rest = 1; }
    a->x = x; a->y = y; a->since = now_ms;
}
static inline void hu_aim_set(hu_aim *a, float x, float y) { a->rx = x; a->ry = y; a->rest = 1; }   // a gesture names the aimed point itself
static inline int hu_aim_point(const hu_aim *a, float *x, float *y) { if (!a->rest) return 0; *x = a->rx; *y = a->ry; return 1; }
// The action plate: who says "an object is under the finger" (DM_DOS2_TOUCH_T_PICK). Sources, in the order `auto` asks them:
//   PC     the pick hle_dos2_touch_world.c noted while a world finger was down (its "hover" flag; what the hybrid's long press uses)
//   HOVER  the engine's own PC pick right now: [PlayerPickingHelper+0x80] as the helper's ray set-up left it with the mode byte 0 - in the PC
//          input mode the helper casts from the UI cursor by itself (main+0xfa9fdc), this is what the PC's own hover (name, cursor) shows
//   LONG   no picking at all: a tap clicks as on the PC, the SHORT LONG PRESS (the tooltip hold) puts the plate up at the finger
// `hybrid` = PC then HOVER without the LONG fallback: in mode 0 the hybrid's trick (make the pad's picking read the cursor) IS the PC pick,
// there is no third picker to ask. `auto` falls to LONG once the picking is dead: the helper's ray set-up does not run at all (hover < 0)
// over HU_PICK_NOHOOK_TAPS world taps, or it runs and no source ever named an object in HU_PICK_DEAD_TAPS world taps; a later hit revives it.
enum { HU_PICK_AUTO = 0, HU_PICK_PC, HU_PICK_HOVER, HU_PICK_HYBRID, HU_PICK_LONG };
enum { HU_SRC_NONE = 0, HU_SRC_PC, HU_SRC_HOVER, HU_SRC_LONG };
enum { HU_PICK_NOHOOK_TAPS = 3, HU_PICK_DEAD_TAPS = 40 };
typedef struct { unsigned taps, hits; int dead; } hu_pick;
static inline int hu_pick_parse(const char *s) {
    if (!s || !*s) return HU_PICK_AUTO;
    return !strncmp(s, "pc", 2) ? HU_PICK_PC : !strncmp(s, "hover", 5) ? HU_PICK_HOVER : !strncmp(s, "hybrid", 6) ? HU_PICK_HYBRID : !strncmp(s, "long", 4) ? HU_PICK_LONG : HU_PICK_AUTO;
}
static inline const char *hu_pick_name(int m) { return m == HU_PICK_PC ? "pc" : m == HU_PICK_HOVER ? "hover" : m == HU_PICK_HYBRID ? "hybrid" : m == HU_PICK_LONG ? "long" : "auto"; }
static inline const char *hu_src_name(int s) { return s == HU_SRC_PC ? "pc" : s == HU_SRC_HOVER ? "hover" : s == HU_SRC_LONG ? "long" : "none"; }
// one world tap. pc: 1 / 0; hover: 1 / 0, -1 = the helper's ray set-up has not run lately. Returns the source that names an object (the plate
// goes up, no click) or HU_SRC_NONE (the tap clicks). *died = 1 when this tap is the one that declares the picking dead (log it once).
static inline int hu_pick_tap(hu_pick *p, int mode, int pc, int hover, int *died) {
    if (died) *died = 0;
    if (mode == HU_PICK_LONG) return HU_SRC_NONE;
    const int src = (mode != HU_PICK_HOVER && pc > 0) ? HU_SRC_PC : (mode != HU_PICK_PC && hover > 0) ? HU_SRC_HOVER : HU_SRC_NONE;
    p->taps++;
    if (src) { p->hits++; p->dead = 0; return src; }
    if (mode == HU_PICK_AUTO && !p->dead && !p->hits && p->taps >= (unsigned)(hover < 0 ? HU_PICK_NOHOOK_TAPS : HU_PICK_DEAD_TAPS)) { p->dead = 1; if (died) *died = 1; }
    return HU_SRC_NONE;
}
// does the short long press put the plate up now
static inline int hu_pick_long_on(const hu_pick *p, int mode) { return mode == HU_PICK_LONG || (mode == HU_PICK_AUTO && p->dead); }
#endif
