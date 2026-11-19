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
// (map, character, inventory, skills, crafting, journal) -> the id the PC hot bar's own button sends in hotbarBtnPressed (hotBarButton.setImage:
// 0 the sheet ("ico_menu"), 1 inventory, 2 skills, 3 crafting, 4 map, 5 journal; main+0x1230904.. jump table); -1 = not a panel
static inline int hu_side_panel(int code) {
    static const int8_t k[6] = {4, 0, 1, 2, 3, 5};
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
#endif
