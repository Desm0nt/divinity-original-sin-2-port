// [DOS2] Pure rules of the in-game UI mode switch (hle_dos2_uimode.c; docs/dos2/TOUCH_MODES_PLAN.md "Implemented: UI mode switch"), kept apart so
// a host test can check them without the game (dm/host/tests/dos2_uimode_test.c). Owner 19.11: three modes, chosen in the gear (front prefs
// "dos2_ui_mode", Java sets DM_DOS2_UI_MODE before the library loads; dm/env wins):
//   0 SWITCH  pure console UI: no `_d` movie (an empty path table: the stock `_c` movies), touches never become mouse records, the on-screen
//             pad in its own layout + a real controller; mode byte main+0x2bb8918 = 1
//   1 HYBRID  today's behaviour, the default (the `_d` table of hle_dos2_touchui.c, touches as the mouse where the gates let them); byte 1
//   2 TOUCH   the iPad-like UI: mode byte 0 (keyboard and mouse: PC movies, PC bindings), its own path table (`touch`, PC types -> `_d`), the
//             pad overlay hidden, a real pad press brings the controller UI back (DM_DOS2_TOUCH_LIVE's switch)
// A change has two parts. The MENU part (the path table, whether touches are mouse records) applies at once: windows made on demand (options,
// save / load, trade ...) pick the new movies. The HUD part (the mode byte, the bindings path, the live pad switch) applies where the player
// UI sets do not exist (the per-player hash [main+0x3298140]+0xf0 is empty: main menu, loading - the point DM_DOS2_TOUCH_LIVE=2 waits for), so
// the next load builds the HUD in the new mode; or at once (DM_DOS2_UI_LIVE=1) by the live rebuild when no modal window / dialog is open.
#ifndef HLE_DOS2_UIMODE_H
#define HLE_DOS2_UIMODE_H
#include <string.h>

enum { UI_SWITCH = 0, UI_HYBRID = 1, UI_TOUCH = 2, UI_NMODES = 3 };

// DM_DOS2_UI_MODE: "0" / "1" / "2" or "switch" / "hybrid" / "touch"; NULL, empty or anything else = HYBRID (today's behaviour).
// DmActivity.dos2UiModeParse is the Java twin (the pad overlay follows the mode the native side runs).
static inline int ui_mode_parse(const char *s) {
    if (!s) return UI_HYBRID;
    while (*s == ' ' || *s == '\t') s++;
    if ((s[0] == '0' || s[0] == '1' || s[0] == '2') && (!s[1] || s[1] == ' ' || s[1] == '\t' || s[1] == '\r' || s[1] == '\n')) return s[0] - '0';
    if (!strncmp(s, "switch", 6)) return UI_SWITCH;
    if (!strncmp(s, "touch", 5)) return UI_TOUCH;
    return UI_HYBRID;
}
static inline const char *ui_mode_name(int m) { return m == UI_SWITCH ? "switch" : m == UI_TOUCH ? "touch" : "hybrid"; }
static inline int ui_mode_valid(int m) { return m >= 0 && m < UI_NMODES; }
// the engine's input mode byte main+0x2bb8918 the mode wants: 1 = controller UI, 0 = keyboard-and-mouse UI
static inline int ui_mode_byte(int m) { return m == UI_TOUCH ? 0 : 1; }
// do touches become mouse records (hle_dos2_touch.c): never in SWITCH
static inline int ui_mode_touches(int m) { return m != UI_SWITCH; }
// the path table bit of a mode (hle_dos2_touchui.c tu_map.modes): SWITCH has none, so every substitution is given back there
static inline unsigned ui_mode_mask(int m) { return ui_mode_valid(m) ? 1u << m : 0u; }
// is a table entry of `modes` active in mode m
static inline int ui_mode_entry_on(unsigned modes, int m) { return (modes & ui_mode_mask(m)) != 0; }

// What one poll does with the HUD part. hud = the mode the HUD part runs now, want = the requested mode, records = player UI sets that exist
// (0 = not in game), live_b = DM_DOS2_UI_LIVE=1, first = this is the poll that took the request (B is tried once, at the request),
// busy = a modal window / dialog / loading screen is up or the host cannot tell (-1 counts as busy).
enum { UI_HUD_NONE = 0 /* nothing to do */, UI_HUD_WAIT /* in game: at the next load / main menu */, UI_HUD_APPLY /* no player UI set: now */,
       UI_HUD_REBUILD /* B: rebuild the player UI sets now */ };
static inline int ui_hud_step(int hud, int want, int records, int live_b, int first, int busy) {
    if (hud == want) return UI_HUD_NONE;
    if (records <= 0) return UI_HUD_APPLY;
    if (live_b && first && busy == 0) return UI_HUD_REBUILD;
    return UI_HUD_WAIT;
}
// The result the JNI call hands back to Java (DmActivity.dos2UiModeChanged picks the toast from it)
enum { UI_RES_OFF = 0 /* no live switch in this run (touch hooks off): next launch */, UI_RES_NEXT_LOAD = 1 /* menus now, HUD at the next load */,
       UI_RES_REBUILT = 2 /* everything now, the HUD was rebuilt */, UI_RES_PENDING = 3 /* the game thread did not answer in time: as 1 when it runs */ };
static inline int ui_result_of(int step) { return step == UI_HUD_REBUILD ? UI_RES_REBUILT : UI_RES_NEXT_LOAD; }
#endif
