// [DOS2] UI mode TOUCH: "Description" / "Описание" in an item's context menu (owner 23.11, playing: "in the inventory a tap on an item gives
// the context menu; the description would be better shown by a separate item 'Описание' in the context menu"). The movie half is
// tools/dos2_pctxmenu.py (contextMenu_p.swf, type 0xb in k_touch); pure rules and the shared numbers: hle_dos2_touch_ctxdesc.h.
// main = main.raw of update v786432, build 4979b200, offsets = main.raw. Static only (not run on a phone when written).
//
// The iPad has no such entry: its contextMenu_t has the PC menu's code (buttonArr -> addButton, buttonPressed) and the description is its
// ShortLongPress (= our still finger / hover). Here the menu's own entries stay exactly the engine's (they come in buttonArr from the engine;
// nothing of them is changed or re-ordered); ours is appended at the bottom and never reaches the engine.
// Facts (tools/dos2_pctxmenu.py check proves them in main.raw):
//   - an item window's openContextMenu (partyInventory 0x74: main+0x13a7690 -> 0x13899a0, characterSheet 0x77, containers 9 / 0x25) fills
//     the item menu 0xb (main+0x10cc3d0 takes it from the player record's node +0x48 in mode 0; 0xa at +0x18 is not an item's): invokes
//     clearButtons, the buttonArr value, updateButtons, later open. The menu class registers its invokes on its FlashPlayer (vt+0x1a8, id, name)
//     at main+0x12ccf60..: setAnchor, clearButtons, close, open, setTitle (id 4, main+0x12ccfcc), addButton, updateButtons.
//   - showItemTooltip(handle, x, y, w, h, -1, anchor) is the game UI base's (main+0x10a5750, every item window falls through to it).
// What the host does (game thread: an ExternalInterface watcher, hle_dos2_touchui_ei.c dm_dos2_touchui_ei_watch, inside the engine's drain):
//   1. openContextMenu from an item window: remember the window (UI object + its FlashPlayer), the item handle and the point; if the player's
//      menu 0xb plays contextMenu_p.swf, invoke its setTitle(CD_SENTINEL, true) first - queued before the class's own clearButtons /
//      updateButtons, so contextMenu_p's next fill (within CD_WIN_MS) appends "Description" (Russian when the engine's entries have a Cyrillic
//      letter: the game's language). The call itself then goes to the game's class as before.
//   2. showItemTooltip from an item window: remembered (the last 8: window, item, the arguments; strings as short inline copies). From the
//      party inventory it is NOT passed on while the menu has the entry (the owner: the description via the menu; DM_DOS2_TOUCH_CTXDESC=2
//      keeps the hover); the hot bar, skills, sheet, containers and the world keep their hover tooltips.
//   3. dmCtxDesc(1) from contextMenu_p (it closed itself first: its menuClosed is drained before): the remembered window, if it is still in
//      the UI manager's table ([main+0x2bbb230] +0x10 / +0x1c) with the same FlashPlayer and type, gets showItemTooltip of the menu's item -
//      the hover's own arguments when it showed that item, else a slot-sized rect at the press point - through the touch HUD's handler first
//      (hle_dos2_touchui_hud.c: dropped when nothing could draw a tooltip) and then the window's vt[0] = OnFunctionCalled, as its movie would.
// Switches: DM_DOS2_TOUCH_CTXDESC=0 = off (no entry, the inventory hover as before), 2 = the entry and the inventory hover both. Log: one line
// at start, the first 12 of each kind (DM_DBG=1: all).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include "hle_dos2_touchui_ei.h"
#include "hle_dos2_touch_ctxdesc.h"
#include "hle_dos2_uimode.h"
#include <stdlib.h>
#include <string.h>

enum { CD_SITE_REG = 0x12ccfc4, CD_SITE_MENU = 0x10cc42c, CD_S_SETTITLE = 0x1e95097 };
// main+0x12ccfc4: ldr x8,[x0]; ldr x8,[x8,#0x1a8]; adrp x2 / add x2 = "setTitle"; mov w1,#4; blr x8 (the menu class registers invoke 4)
static const uint32_t k_cd_reg[6] = {0xf9400008, 0xf940d508, 0xb0005e42, 0x91025c42, 0x321e03e1, 0xd63f0100};
// main+0x10cc42c: cbnz w10 (mode byte != 0 -> the console menu at +0x40); ldr x10,[x8,#0x48]: the PC item menu 0xb; tst
static const uint32_t k_cd_menu[3] = {0x3500028a, 0xf940250a, 0xf260555f};

typedef struct { void *ui, *fp; uint32_t type; uint16_t who; uint64_t item; double x, y; int set; } cd_menu;
typedef struct { void *ui; uint64_t item; unsigned n; dm_ei_val a[CD_MAXARGS]; } cd_tip;
static module_t *g_main;
static int g_on, g_keep_hover, g_dbg;
static cd_menu g_m;                 // the last item menu opened (game thread only, like everything here)
static cd_tip g_tip[8]; static unsigned g_ntip;
static unsigned g_nlog[4];          // 0 menus, 1 dropped hovers, 2 shown, 3 refused

int dm_dos2_ui_mode_hud(void);      // hle_dos2_uimode.c
int dm_dos2_touchui_ei_host(const char *name, void *player, void *ui, unsigned n, const dm_ei_val *args);   // hle_dos2_touchui_ei.c
#define CD_LOG(k, ...) do { if (g_dbg || g_nlog[k]++ < 12) LOG(__VA_ARGS__); } while (0)

static uint64_t cd_bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static uint8_t *cd_mgr(void) { return *(uint8_t *volatile *)(void *)(g_main->base + CD_UIMGR); }
// the manager's object table ([main+0x2bbb230] +0x10 objects, +0x1c count; free slots are NULL, as hle_dos2_touch_gaps.c reads it)
static uint8_t *const *cd_table(uint32_t *n) {
    uint8_t *mgr = cd_mgr();
    if (!mgr) return NULL;
    *n = *(volatile uint32_t *)(mgr + CD_MGR_NOBJ);
    uint8_t *const *objs = *(uint8_t *const *volatile *)(void *)(mgr + CD_MGR_OBJS);
    return objs && *n <= 4096 ? objs : NULL;
}
// is this UI object still alive: in the table (pointer compared before anything is read from it), same FlashPlayer, same type
static int cd_live(const void *ui, const void *fp, uint32_t type) {
    uint32_t n = 0; uint8_t *const *objs = cd_table(&n);
    for (uint32_t i = 0; objs && i < n; i++)
        if (objs[i] && objs[i] == ui) return *(void *const volatile *)(void *)(objs[i] + CD_UI_FP) == fp && *(volatile uint32_t *)(objs[i] + CD_UI_TYPE) == type;
    return 0;
}
// the object's movie (the engine string at +0x20) ends with `file`
static int cd_movie(const uint8_t *o, const char *file) {
    const uint8_t *s = o + CD_UI_PATH; const char *p; uint32_t len;
    if (s[0] & 1) { uint32_t cap; memcpy(&cap, s, 4); memcpy(&len, s + 4, 4); memcpy(&p, s + 8, 8);
                    if (!p || len > 300 || len + 1 > (cap & ~1u) || !((uintptr_t)p >> 12) || ((uintptr_t)p >> 48)) return 0; }
    else { p = (const char *)s + 1; len = s[0] >> 1; if (len > 14) return 0; }
    return cd_path_is(p, len, file);
}
// the player's item menu, when it plays our movie (NULL = none: an older TouchUI.pak, or another set)
static uint8_t *cd_menu_obj(uint16_t who) {
    uint32_t n = 0; uint8_t *const *objs = cd_table(&n);
    for (uint32_t i = 0; objs && i < n; i++) {
        uint8_t *o = objs[i];
        if (o && *(volatile uint32_t *)(o + CD_UI_TYPE) == CD_T_MENU && *(volatile uint16_t *)(o + CD_UI_PLAYER) == who
              && *(void *volatile *)(void *)(o + CD_UI_FP) && cd_movie(o, "contextMenu_p.swf")) return o;
    }
    return NULL;
}

// arguments kept past the drain: numbers / booleans as they are, strings as short inline copies (the drain frees its own); 0 = not keepable
static int cd_keep(dm_ei_val *d, const dm_ei_val *s, unsigned n) {
    if (n > CD_MAXARGS) return 0;
    for (unsigned i = 0; i < n; i++) {
        memset(&d[i], 0, sizeof d[i]); d[i].type = s[i].type;
        if (s[i].type == 3) d[i].b = s[i].b;
        else if (s[i].type == 4) d[i].d = s[i].d;
        else if (s[i].type == 5) { char t[32]; dm_ei_str(&s[i], t, sizeof t); if (!cd_short_str(d[i].str, t)) return 0; }
        else return 0;
    }
    return 1;
}
static const cd_tip *cd_tip_of(const void *ui, uint64_t item, int any_item) {   // newest first
    for (unsigned k = 0; k < 8 && k < g_ntip; k++) {
        const cd_tip *t = &g_tip[(g_ntip - 1 - k) & 7];
        if (t->ui == ui && (any_item || t->item == item)) return t;
    }
    return NULL;
}

static void cd_show(void) {
    if (!g_m.set) { CD_LOG(3, "[ctxdesc] Description chosen, but no item menu is remembered: nothing shown"); return; }
    g_m.set = 0;
    if (!cd_live(g_m.ui, g_m.fp, g_m.type)) { CD_LOG(3, "[ctxdesc] Description chosen: the item's window (type %#x) is gone: nothing shown", g_m.type); return; }
    dm_ei_val a[CD_MAXARGS]; unsigned n; const char *how;
    const cd_tip *t = cd_tip_of(g_m.ui, g_m.item, 0);
    if (t) { memcpy(a, t->a, sizeof a); n = t->n; how = "the hover's own arguments"; }
    else {
        const cd_tip *any = cd_tip_of(g_m.ui, 0, 1);
        double x, y, w, h; cd_rect(g_m.x, g_m.y, any && any->n >= 5 && any->a[3].type == 4 ? any->a[3].d : 0, &x, &y, &w, &h);
        const double v[6] = {0, x, y, w, h, -1};
        memset(a, 0, sizeof a); n = 7;
        for (int i = 0; i < 6; i++) { a[i].type = 4; a[i].d = v[i]; }
        memcpy(&a[0].d, &g_m.item, 8);
        if (any && any->n == 7 && any->a[6].type == 5) a[6] = any->a[6]; else { a[6].type = 5; cd_short_str(a[6].str, "rightTop"); }
        how = "a slot-sized rect at the press point (the hover never showed it)";
    }
    if (dm_dos2_touchui_ei_host("showItemTooltip", g_m.fp, g_m.ui, n, a) == 1) {
        CD_LOG(3, "[ctxdesc] Description chosen: the touch HUD drops tooltips now (no tooltip window): nothing shown"); return; }
    void **vt = *(void ***)g_m.ui;
    ((void (*)(void *, const char *, uint32_t, const dm_ei_val *))vt[0])(g_m.ui, "showItemTooltip", n, a);
    CD_LOG(2, "[ctxdesc] Description chosen: showItemTooltip(%d args, %s) to the item's window type %#x %p", (int)n, how, g_m.type, g_m.ui);
}

static int cd_watch(void *ctx, void *player, void *ui, const char *name, unsigned n, const dm_ei_val *a) {
    (void)ctx;
    if (!g_on || !ui || !name || (name[0] != 's' && name[0] != 'o' && name[0] != 'd')) return 0;
    const int desc = !strcmp(name, CD_EI_NAME), tip = !desc && !strcmp(name, "showItemTooltip"), menu = !desc && !tip && !strcmp(name, "openContextMenu");
    if (!desc && !tip && !menu) return 0;
    if (dm_dos2_ui_mode_hud() != UI_TOUCH) return desc;   // only contextMenu_p (touch set) sends dmCtxDesc; no class knows it
    const uint32_t type = *(const volatile uint32_t *)((const uint8_t *)ui + CD_UI_TYPE);
    if (desc) { if (type == CD_T_MENU && n >= 1 && a && a[0].type == 4 && a[0].d == 1) cd_show(); return 1; }
    if (!cd_source(type) || !a) return 0;
    const uint16_t who = *(const volatile uint16_t *)((const uint8_t *)ui + CD_UI_PLAYER);
    if (tip) {
        dm_ei_val kept[CD_MAXARGS] = {{0}};   // the ring's slot (the oldest record) is overwritten only by a whole, keepable call
        if (n >= 1 && a[0].type == 4 && cd_keep(kept, a, n)) {
            cd_tip *t = &g_tip[g_ntip & 7]; memcpy(t->a, kept, sizeof kept); t->ui = ui; t->item = cd_bits(a[0].d); t->n = n; g_ntip++; }
        if (cd_hover_dropped(type, cd_menu_obj(who) != NULL, g_keep_hover)) {
            CD_LOG(1, "[ctxdesc] hover showItemTooltip of the party inventory not shown (the item menu's Description shows it; DM_DOS2_TOUCH_CTXDESC=2 keeps it)");
            return 1; }
        return 0;
    }
    const int k = cd_item_arg(n);
    if (k < 0 || a[k].type != 4 || a[n - 2].type != 4 || a[n - 1].type != 4) return 0;
    g_m.ui = ui; g_m.fp = player; g_m.type = type; g_m.who = who; g_m.item = cd_bits(a[k].d); g_m.x = a[n - 2].d; g_m.y = a[n - 1].d; g_m.set = 1;
    uint8_t *m = cd_menu_obj(who);
    if (!m) { CD_LOG(0, "[ctxdesc] openContextMenu from type %#x: the item menu 0xb of player %u does not play contextMenu_p.swf: no Description entry", type, (unsigned)who); return 0; }
    dm_ei_val v[2]; memset(v, 0, sizeof v);
    v[0].type = 5; cd_short_str(v[0].str, CD_SENTINEL); v[1].type = 3; v[1].b = 1;
    const int sent = dm_dos2_touchui_invoke(*(void *volatile *)(void *)(m + CD_UI_FP), "setTitle", v, 2);
    CD_LOG(0, "[ctxdesc] openContextMenu from type %#x (item %#llx at %.0f,%.0f): setTitle(\"%s\", true) to contextMenu_p %s", type, (unsigned long long)g_m.item,
           g_m.x, g_m.y, CD_SENTINEL, sent ? "queued (the menu's fill gets Description)" : "NOT sent (the menu's player has no setTitle invoke)");
    return 0;   // the game's class opens the menu as before
}

void dm_dos2_touch_ctxdesc_install(void);   // main.c host_preload, after dm_dos2_touchui_ei_install
void dm_dos2_touch_ctxdesc_install(void) {
    const char *e = getenv("DM_DOS2_TOUCH_CTXDESC"), *d = getenv("DM_DBG");
    g_dbg = d && *d && *d != '0';
    if (e && *e == '0') { LOG("[ctxdesc] DM_DOS2_TOUCH_CTXDESC=0: no Description in the item menu; the inventory hover tooltip as before"); return; }
    g_keep_hover = e && *e == '2';
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_main = &g_mod[i];
    if (!g_main || !dm_dos2_touchui_ei_on()) return;   // no EI hook (the touch UI is off): nothing to do
    const struct { uint32_t off, n; const uint32_t *w; } k_sites[] = {{CD_SITE_REG, 6, k_cd_reg}, {CD_SITE_MENU, 3, k_cd_menu}};
    for (unsigned i = 0; i < sizeof k_sites / sizeof *k_sites; i++)
        if ((uint64_t)k_sites[i].off + 4ull * k_sites[i].n > g_main->text_end || memcmp(g_main->base + k_sites[i].off, k_sites[i].w, 4ull * k_sites[i].n)) {
            LOG("【红】[ctxdesc] main+%#x differs (the item menu class): not build 4979b200, no Description entry", k_sites[i].off); return; }
    if (memcmp(g_main->base + CD_S_SETTITLE, "setTitle", 9)) { LOG("【红】[ctxdesc] main+%#x is not \"setTitle\": no Description entry", CD_S_SETTITLE); return; }
    g_on = dm_dos2_touchui_ei_watch(cd_watch, NULL);
    LOG("[ctxdesc] UI mode touch: Description in the item menu (contextMenu_p, type %#x) %s: openContextMenu of types 0x74 / 0x77 / 9 / 0x25 -> setTitle(\"%s\"), "
        "dmCtxDesc(1) -> the item's showItemTooltip; the party inventory's hover tooltip %s (DM_DOS2_TOUCH_CTXDESC=0 off, 2 keeps the hover)", CD_T_MENU,
        g_on ? "on" : "NOT registered", CD_SENTINEL, g_keep_hover ? "kept" : "only through the menu");
}
#endif
