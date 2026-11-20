// [DOS2] UI mode TOUCH: loading screen tips without keyboard keys (owner 21.11: "why does the loading screen talk about left Shift and
// holding Ctrl?"). The PC UI the touch mode runs (mode byte 0) makes the engine pick its tips from the Keyboard set of Hints.lsx, whose
// keys it prints as the PC's ("Left Shift", "Ctrl"). The iPad's Hints.lsx has a third set (TouchScreen) without them plus touch-only
// tips; the Switch engine knows no such set. So, as the iPad: while the HUD part of the mode is TOUCH, the host clears the Keyboard byte
// of the keyboard-only entries (hle_dos2_touch_tips.h k_tt_off) in the engine's loaded table, and gives each its loaded value back when
// the mode leaves TOUCH. The touch-only tips come from the movie: loadingScreen_p (type 0x17 in k_touch, tools/dos2_ptouch_tips.py) puts
// one of our own touch tips (English / Russian by the engine text's language) in front of every second engine tip.
// Why not the console set: the Controller entries carry pad tokens too ([RightStickX], [IE_DestructionToggle], [IE_ToggleTacticalCamera])
// and the mode byte would have to change for the whole UI - the table fix touches 14 bytes and nothing else.
// main = main.raw of update v786432, build 4979b200, offsets = main.raw. Static only (not run on a phone when written).
// Runs on the game thread from the pad poll (hle_dos2_uimode.c dm_dos2_uimode_poll -> dm_dos2_touch_tips_poll): cheap compare of the
// table pointer / count / mode each poll, work only when one changed (the table is loaded once at client start, before the main menu).
// Switch: DM_DOS2_TOUCH_TIPS=0 = off (the engine's keyboard tips stay; the movie still adds the touch tips). Log: one line at install,
// one per apply ("[tips] ..."), with DM_DBG=1 the names of the entries changed.
#ifdef DM_GAME_DOS2
#include "hle.h"
#include "hle_dos2_touch_tips.h"
#include "hle_dos2_uimode.h"
#include <stdlib.h>
#include <string.h>

// the code the table layout comes from (main.raw words; tools/dos2_touchui_check.py compares them too)
enum { TT_SITE_PICK = 0x114fd6c, TT_SITE_MODE = 0x114fd88, TT_SITE_FLAGS = 0x114fdc4, TT_SITE_KB = 0x114fe58, TT_SITE_PARSE_KEY = 0x110ff6c,
       TT_SITE_PARSE_CTRL = 0x1110068, TT_SITE_PARSE_KB = 0x1110094, TT_SITE_LOAD = 0xd07190 };
static const uint32_t k_tt_pick[4] = {0xf0010a48 /* adrp x8,main+0x329a000 */, 0xf9419914 /* ldr x20,[x8,#0x330]: the manager */,
    0xf9400e88 /* ldr x8,[x20,#0x18] */, 0xd358bd15 /* ubfx x21,x8,#0x18,#0x18: the count */};
static const uint32_t k_tt_mode[4] = {0xb000d348 /* adrp x8,main+0x2bb8000 */, 0x39646116 /* ldrb w22,[x8,#0x918]: the mode byte */,
    0x942a9444 /* bl rand */, 0x321d0bf8 /* mov w24,#0x38: the entry size */};
static const uint32_t k_tt_flags[4] = {0xf9400a89 /* ldr x9,[x20,#0x10]: the entries */, 0x9b18250a /* madd x10,x8,x24,x9 */,
    0x34000476 /* cbz w22: mode 0 -> the Keyboard byte */, 0x3940c14a /* ldrb w10,[x10,#0x30]: Controller */};
static const uint32_t k_tt_kb[3] = {0x3940c54a /* ldrb w10,[x10,#0x31]: Keyboard */, 0x35fffbea /* cbnz w10: taken */, 0x17ffffd5 /* b: pick again */};
static const uint32_t k_tt_parse_key[2] = {0x321d0be8 /* mov w8,#0x38 */, 0x9b087379 /* madd x25,x27,x8,x28: HintKey at +0 */};
static const uint32_t k_tt_parse_ctrl[5] = {0x321d0be8 /* mov w8,#0x38 */, 0x9b087376 /* madd x22,x27,x8,x28 */, 0xf9400268, 0xf9406108,
    0x9100c2c2 /* add x2,x22,#0x30: Controller */};
static const uint32_t k_tt_parse_kb[3] = {0xf9400268, 0xf9406108, 0x9100c6c2 /* add x2,x22,#0x31: Keyboard */};
static const uint32_t k_tt_load[4] = {0xf0012c88 /* adrp x8,main+0x329a000 */, 0xf9419900 /* ldr x0,[x8,#0x330] */, 0x910043e1,
    0x941024f5 /* bl main+0x1110570: load Hints/Hints.lsx */};
typedef struct { uint32_t off, n; const uint32_t *want; } tt_site;
static const tt_site k_tt_sites[] = {{TT_SITE_PICK, 4, k_tt_pick}, {TT_SITE_MODE, 4, k_tt_mode}, {TT_SITE_FLAGS, 4, k_tt_flags},
    {TT_SITE_KB, 3, k_tt_kb}, {TT_SITE_PARSE_KEY, 2, k_tt_parse_key}, {TT_SITE_PARSE_CTRL, 5, k_tt_parse_ctrl},
    {TT_SITE_PARSE_KB, 3, k_tt_parse_kb}, {TT_SITE_LOAD, 4, k_tt_load}};

static module_t *g_main;
static int g_ok, g_dbg;
static uint8_t *g_arr;              // the table this state belongs to
static uint32_t g_cnt;
static int g_applied = -1;          // 1 = the keyboard-only entries are cleared, 0 = as loaded, -1 = not looked at yet
static uint8_t g_hit[TT_MAX], g_kb[TT_MAX];

int dm_dos2_ui_mode_hud(void);      // hle_dos2_uimode.c

void dm_dos2_touch_tips_poll(void);   // hle_dos2_uimode.c, every pad poll (the game thread)
void dm_dos2_touch_tips_poll(void) {
    if (!g_ok) return;
    uint8_t *mgr = *(uint8_t *volatile *)(void *)(g_main->base + TT_HINTS);
    if (!mgr) return;   // before the client loaded Hints.lsx
    uint8_t *arr = *(uint8_t *volatile *)(void *)(mgr + TT_MGR_ARR);
    const uint32_t cnt = tt_count(*(volatile uint64_t *)(void *)(mgr + TT_MGR_CNT));
    const int want = dm_dos2_ui_mode_hud() == UI_TOUCH;
    if (arr == g_arr && cnt == g_cnt && want == g_applied) return;
    if (!arr || !cnt) return;   // the manager before its table is filled: wait
    if (cnt > TT_MAX) {
        if (arr != g_arr || cnt != g_cnt) LOG("【红】[tips] hint table at %p with %u entries: not the table this build has, the loading screen tips are not changed", (void *)arr, cnt);
        g_arr = arr; g_cnt = cnt; g_applied = want; return;
    }
    if (arr != g_arr || cnt != g_cnt) {   // a table not seen yet: which entries are ours, their Keyboard bytes as loaded
        for (uint32_t i = 0; i < cnt; i++) {
            uint8_t *e = arr + (size_t)i * TT_ENTRY;
            g_hit[i] = (uint8_t)tt_off_key(*(const char *volatile *)(void *)(e + TT_E_KEY));
            g_kb[i] = e[TT_E_KEYBOARD];
        }
        g_arr = arr; g_cnt = cnt; g_applied = 0;
    }
    uint32_t nhit = 0;
    for (uint32_t i = 0; i < cnt; i++) nhit += g_hit[i];
    const uint32_t keep = tt_plan(g_hit, g_kb, cnt);
    if (want && !keep) { LOG("【红】[tips] clearing the %u keyboard-only tips would leave the PC pick none of %u: not changed", nhit, cnt); g_applied = want; return; }
    for (uint32_t i = 0; i < cnt; i++) {
        if (!g_hit[i]) continue;
        uint8_t *e = arr + (size_t)i * TT_ENTRY;
        e[TT_E_KEYBOARD] = want ? 0 : g_kb[i];
        if (g_dbg) LOG("[tips] %s: Keyboard %d", *(const char *const *)(void *)(e + TT_E_KEY), e[TT_E_KEYBOARD]);
    }
    g_applied = want;
    LOG("[tips] UI mode %s: %u keyboard-only loading screen tip(s) of %u %s (%u keyboard tip(s) %s)", want ? "touch" : "not touch", nhit, cnt,
        want ? "taken out of the PC pick" : "as loaded", want ? keep : keep + nhit, want ? "left; the movie adds the touch tips" : "in the PC pick");
}

void dm_dos2_touch_tips_install(void);   // main.c host_preload, after dm_dos2_touch_gaps_install
void dm_dos2_touch_tips_install(void) {
    const char *e = getenv("DM_DOS2_TOUCH_TIPS"), *d = getenv("DM_DBG");
    g_dbg = d && *d && *d != '0';
    if (e && *e == '0') { LOG("[tips] DM_DOS2_TOUCH_TIPS=0: the loading screen keeps the keyboard tips in UI mode touch"); return; }
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_main = &g_mod[i];
    if (!g_main) return;
    for (unsigned i = 0; i < sizeof k_tt_sites / sizeof *k_tt_sites; i++)
        if ((uint64_t)k_tt_sites[i].off + 4ull * k_tt_sites[i].n > g_main->text_end || memcmp(g_main->base + k_tt_sites[i].off, k_tt_sites[i].want, 4ull * k_tt_sites[i].n)) {
            LOG("【红】[tips] main+%#x differs (the loading screen's hint pick / Hints.lsx parser): the tips are not changed", k_tt_sites[i].off); return; }
    g_ok = 1;
    LOG("[tips] UI mode touch: %d keyboard-only loading screen tips leave the PC pick (hint table [main+%#x], Keyboard byte +%#x); DM_DOS2_TOUCH_TIPS=0 keeps them",
        TT_NOFF, TT_HINTS, TT_E_KEYBOARD);
}
#endif
