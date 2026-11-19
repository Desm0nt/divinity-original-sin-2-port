// [DOS2] UI mode TOUCH: iPad functions whose ExternalInterface call the window's Switch engine class does not handle - the host does them
// (docs/dos2/TOUCH_MODES_PLAN.md "Implemented: iPad functions without a Switch EI - equivalents"; the movie half is tools/dos2_ptouch_gaps.py).
// main = main.raw of update v786432, build 4979b200, offsets = main.raw. Static only (not run on a phone when written).
// How a gap is told from a working call: an EI name works only when the OnFunctionCalled chain of the window's class compares it (vtable
// slot 0, its strcmp chain, the sub-handlers and base classes it branches to; tools/dos2_ptouch_gaps.py checks the facts below in main.raw).
// Taken here (hle_dos2_touchui_ei.c handlers: game thread, inside the engine's own EI drain):
//   openCharInventory(handle)  playerInfo_p, type 0x26 (the touch HUD's portraits). Its class (the creator main+0x10b5f00 builds it inline:
//                              vtable main+0x29eb768, slot 0 main+0x1237b10, then main+0x1251050) compares charSel, centerCamOnCharacter,
//                              piAdd* / piDetach*, stopDragging ... - not openCharInventory (only the GM possessionBar main+0x1258500 does).
//                              The PC's right click on a portrait = the iPad's long press (the touch gestures send a right click) = open
//                              the inventory: the touch column's inventory button route (hle_dos2_touchui_hud.c dm_dos2_touchui_hud_panel,
//                              "hotbarBtnPressed"(1) to the player's PC hot bar), only when the party inventory (0x74) is not open already
//                              (it is a toggle; the PC party inventory shows every character, so "open" is the whole function).
//   Everything else of the touch windows either works through a class that handles it (dialog's toggleInv: main+0x1237140; trade's
//   offerJunk: main+0x1349640 -> 0x13f5860) or is replaced in the movies (tools/dos2_ptouch_gaps.py: trade's long press, the container's
//   context menu).
// Switches: DM_DOS2_TOUCH_GAPS=0 = off (the calls go to the classes, which drop them, as before). Log: one line at start, the first 12 taken
// calls (with DM_DBG=1 all).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include "hle_dos2_touchui_ei.h"
#include "hle_dos2_touch_gaps.h"
#include <stdlib.h>
#include <string.h>

enum { TG_PI_VT = 0x29eb768 };   // its slot 0 must hold main+TG_PI_CALLED in memory (relocated) - checked at install
static module_t *g_main;
static int g_on, g_dbg;
static unsigned g_n;

int dm_dos2_touchui_hud_panel(void *ui, int code, int only_open, const char *who_asks);   // hle_dos2_touchui_hud.c

static int tg_ei(void *ctx, void *player, void *ui, const char *name, unsigned n, const dm_ei_val *a) {
    (void)ctx; (void)player; (void)n; (void)a;
    if (!g_on || !ui) return 0;
    const uint32_t type = *(const volatile uint32_t *)((const uint8_t *)ui + TG_UI_TYPE);
    void *const *vt = *(void *const *const *)ui;
    const int64_t called = vt ? (int64_t)((const uint8_t *)vt[0] - g_main->base) : -1;
    const int r = tg_rule(name, type, called);
    if (r == TG_PASS) return 0;
    const int sent = dm_dos2_touchui_hud_panel(ui, 4, 1, "openCharInventory (portrait long press)");
    if (g_dbg || g_n++ < 12) LOG("[gaps] %s from type %#x: %s", name, type, sent ? "the inventory opened (column route)" : "the inventory is open already / the touch HUD is not up: nothing");
    return 1;   // ours either way: the class has no handler for it
}

void dm_dos2_touch_gaps_install(void);   // main.c host_preload, after dm_dos2_touchui_hud_install
void dm_dos2_touch_gaps_install(void) {
    const char *e = getenv("DM_DOS2_TOUCH_GAPS"), *d = getenv("DM_DBG");
    g_dbg = d && *d && *d != '0';
    if (e && *e == '0') { LOG("[gaps] DM_DOS2_TOUCH_GAPS=0: the touch gap handlers are off"); return; }
    if (!dm_dos2_touchui_ei_on()) return;   // no EI hook (no touch UI): nothing to take
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_main = &g_mod[i];
    if (!g_main) return;
    void *const *slot = (void *const *)(void *)(g_main->base + TG_PI_VT);
    if (*slot != (void *)(g_main->base + TG_PI_CALLED)) {
        LOG("【红】[gaps] main+%#x (playerInfo vtable slot 0) is not main+%#x: not build 4979b200, the gap handlers are off", TG_PI_VT, TG_PI_CALLED); return; }
    g_on = dm_dos2_touchui_ei_register("openCharInventory", tg_ei, NULL);
    LOG("[gaps] UI mode touch gap handlers %s: openCharInventory of the portraits (type %#x, class main+%#x has no handler) = open the inventory "
        "(the column's route); DM_DOS2_TOUCH_GAPS=0 turns it off", g_on ? "on" : "NOT registered", TG_PI_TYPE, TG_PI_CALLED);
}
#endif
