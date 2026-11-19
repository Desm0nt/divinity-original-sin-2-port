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
// The video skip (owner 20.11: the console's "B to skip" becomes a double tap). Videos play in the subtitles UI (type 0x20, subtitles.swf:
//   iggy_video + the hint hintHolder_mc); on any input during a video its class shows "(Press [1] to skip)" (localised handle
//   hf4e748begfdf9g4d52g8b3fg65483db31de1, [1] = the B glyph / the key; main+0x1194e90 -> setHintText / showHint). UI mode TOUCH:
//   subtitles_p shows "Double tap to skip" / "Двойной тап — пропустить" instead (tools/dos2_ptouch_gaps.py). The second tap of a TOUCH double
//   tap (hle_dos2_touch.c -> dm_dos2_touch_gaps_dtap) while a video plays = the class's own skip: its OnInputEvent main+0x1194d00 with the
//   input event SkipVideo (0xe7), on the game thread (dm_dos2_touch_gaps_poll from hle_dos2_uimode.c). Not escape: escape is SkipVideo AND
//   ToggleInGameMenu in keyboard_1.json, and whether the pause menu opens over a video is not decidable statically. Single taps stay plain
//   clicks (the class only shows its hint on them). "A video plays" = the movie name the skip itself resets is set (hle_dos2_touch_gaps.h).
// Everything else of the touch windows either works through a class that handles it (dialog's toggleInv: main+0x1237140; trade's offerJunk:
// main+0x1349640 -> 0x13f5860) or is replaced in the movies (tools/dos2_ptouch_gaps.py: trade's long press, the container's context menu).
// Switches: DM_DOS2_TOUCH_GAPS=0 = all off (the calls go to the classes, which drop them, as before; no double-tap skip). Log: lines at
// start, the first 12 taken calls / skips (with DM_DBG=1 all).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include "hle_dos2_touchui_ei.h"
#include "hle_dos2_touch_gaps.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { TG_PI_VT = 0x29eb768,   // its slot 0 must hold main+TG_PI_CALLED in memory (relocated) - checked at install
       TG_SITE_INPUT = 0x1194d00, TG_SITE_INPUT_SKIP = 0x1194d40, TG_SITE_SKIP_NAME = 0x11955b8, TG_SITE_SUB_CTOR = 0x1194778,
       TG_UIMGR = 0x2bbb230, TG_MGR_OBJS = 0x10, TG_MGR_NOBJ = 0x1c };
// the subtitles class code the skip relies on (main.raw words; tools/dos2_touchui_check.py compares them too)
static const uint32_t k_tg_input[4] = {0xa9be4ff4 /* stp x20,x19,[sp,#-0x20]! */, 0xa9017bfd, 0x910043fd, 0xb9400028 /* ldr w8,[x1]: the event id */};
static const uint32_t k_tg_input_skip[6] = {0x39460009 /* ldrb w9,[x0,#0x180]: skip allowed */, 0xaa0003f3, 0x34000489, 0x71039d1f /* cmp w8,#0xe7: SkipVideo */,
    0x54000301, 0xf000d129};
static const uint32_t k_tg_skip_name[6] = {0xaa1303f4, 0x90010949 /* adrp x9,main+0x32bd000 */, 0xf8418e88 /* ldr x8,[x20,#0x18]!: the movie name */,
    0xf940c135 /* ldr x21,[x9,#0x180]: the empty name */, 0xeb15011f, 0x54000140};
static const uint32_t k_tg_sub_ctor[4] = {0x91024108 /* add x8,x8,#0x90: the UIObject vtable main+0x29e6770 */, 0xa9167e8a, 0xf2c20009,
    0xa9032688 /* stp x8,x9,[x20,#0x30]: the UIObject at +0x30 */};
typedef struct { uint32_t off, n; const uint32_t *want; } tg_site;
static const tg_site k_tg_sites[] = {{TG_SITE_INPUT, 4, k_tg_input}, {TG_SITE_INPUT_SKIP, 6, k_tg_input_skip}, {TG_SITE_SKIP_NAME, 6, k_tg_skip_name},
    {TG_SITE_SUB_CTOR, 4, k_tg_sub_ctor}};

static module_t *g_main;
static int g_on, g_dbg, g_vid_ok;
static unsigned g_n, g_nv;
static _Atomic uint64_t g_skip_req;   // ms of the last double tap (0 = none): set by the touch thread, taken by the game thread
static uint64_t tg_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000ull + (uint64_t)t.tv_nsec / 1000000ull; }

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

// hle_dos2_touch.c, UI mode TOUCH: the second tap of a double tap (UI or world finger; the touch thread)
void dm_dos2_touch_gaps_dtap(void);
void dm_dos2_touch_gaps_dtap(void) { if (g_vid_ok) atomic_store(&g_skip_req, tg_ms()); }

// the subtitles class object with a video on (full = its UIObject - 0x30), NULL = none
static uint8_t *tg_video(void) {
    uint8_t *mgr = *(uint8_t *volatile *)(g_main->base + TG_UIMGR);
    if (!mgr) return NULL;
    const uint32_t n = *(volatile uint32_t *)(mgr + TG_MGR_NOBJ); uint8_t **objs = *(uint8_t **volatile *)(mgr + TG_MGR_OBJS);
    if (!objs || n > 4096) return NULL;
    const uint64_t empty = *(volatile uint64_t *)(void *)(g_main->base + TG_EMPTY_NAME);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *o = objs[i];
        if (!o || *(volatile uint32_t *)(o + TG_UI_TYPE) != TG_SUB_TYPE) continue;
        if (*(void *volatile *)(void *)o != (void *)(g_main->base + TG_SUB_UIVT)) continue;   // the subtitles class's UIObject, nothing else
        uint8_t *full = o - TG_SUB_UI;
        if (tg_video_on(*(volatile uint64_t *)(void *)(full + TG_SUB_NAME), empty)) return full;
    }
    return NULL;
}
// hle_dos2_uimode.c, every pad poll (the game thread): a due double tap with a video on = the class's SkipVideo
void dm_dos2_touch_gaps_poll(void);
void dm_dos2_touch_gaps_poll(void) {
    if (!g_vid_ok) return;
    const uint64_t req = atomic_load(&g_skip_req);
    if (!req) return;
    atomic_store(&g_skip_req, 0);
    if (!tg_skip_due(req, tg_ms())) return;
    uint8_t *full = tg_video();
    if (!full) { if (g_dbg) LOG("[gaps] double tap: no video playing (subtitles UI): nothing to skip"); return; }
    const int allowed = full[TG_SUB_ALLOW] != 0;
    uint8_t ev[0x40]; memset(ev, 0, sizeof ev); *(uint32_t *)(void *)ev = TG_EV_SKIPVIDEO;   // id 0xe7; the kind at +0x24 = 0 (4 would be ignored)
    const uint64_t r = ((uint64_t (*)(void *, void *))(void *)(g_main->base + TG_SUB_INPUT))(full, ev);
    if (g_dbg || g_nv++ < 12) LOG("[gaps] double tap during a video: SkipVideo to the subtitles class (skip allowed %d): %s", allowed, r & 0xff ? "taken (skipped)" : "not taken");
}

void dm_dos2_touch_gaps_install(void);   // main.c host_preload, after dm_dos2_touchui_hud_install
void dm_dos2_touch_gaps_install(void) {
    const char *e = getenv("DM_DOS2_TOUCH_GAPS"), *d = getenv("DM_DBG");
    g_dbg = d && *d && *d != '0';
    if (e && *e == '0') { LOG("[gaps] DM_DOS2_TOUCH_GAPS=0: the touch gap handlers and the double-tap video skip are off"); return; }
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_main = &g_mod[i];
    if (!g_main) return;
    g_vid_ok = 1;
    for (unsigned i = 0; i < sizeof k_tg_sites / sizeof *k_tg_sites; i++)
        if ((uint64_t)k_tg_sites[i].off + 4ull * k_tg_sites[i].n > g_main->text_end || memcmp(g_main->base + k_tg_sites[i].off, k_tg_sites[i].want, 4ull * k_tg_sites[i].n)) {
            LOG("【红】[gaps] main+%#x differs (the subtitles class): no double-tap video skip", k_tg_sites[i].off); g_vid_ok = 0; break; }
    if (g_vid_ok && g_dbg) LOG("[gaps] video skip by double tap (UI mode TOUCH): SkipVideo to the subtitles class main+%#x while a video plays", TG_SUB_INPUT);
    if (!dm_dos2_touchui_ei_on()) return;   // the EI hook (touch UI): no handlers without it
    void *const *slot = (void *const *)(void *)(g_main->base + TG_PI_VT);
    if (*slot != (void *)(g_main->base + TG_PI_CALLED)) {
        LOG("【红】[gaps] main+%#x (playerInfo vtable slot 0) is not main+%#x: not build 4979b200, the gap handlers are off", TG_PI_VT, TG_PI_CALLED); return; }
    g_on = dm_dos2_touchui_ei_register("openCharInventory", tg_ei, NULL);
    LOG("[gaps] UI mode touch gap handlers %s: openCharInventory of the portraits (type %#x, class main+%#x has no handler) = open the inventory "
        "(the column's route); double-tap video skip %s; DM_DOS2_TOUCH_GAPS=0 turns both off", g_on ? "on" : "NOT registered", TG_PI_TYPE, TG_PI_CALLED,
        g_vid_ok ? "on" : "OFF (sites differ)");
}
#endif
