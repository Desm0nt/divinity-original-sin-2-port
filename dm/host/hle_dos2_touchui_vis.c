// [DOS2] Visible touch elements of the console HUD, the host half (docs/dos2/TOUCH_VISIBLE.md; owner 15.11: where the iPad has a button on screen
// and the console movie only a pad button / the radial menu, the button is drawn into the console movie). Static only, not run on a phone.
// main = main.raw of update v786432, build 4979b200, offsets = main.raw.
// bottomBar_d.swf (tools/dos2_hud_touch.py) = bottomBar_c.swf + the iPad's panel column (rightSideButtons_t art) on its root timeline. A tap on a
// column button calls ExternalInterface "dmPanel"(id); the hot bar arrows / end turn call the movie's own names (prevHotbar, nextHotbar, endTurn:
// the bottomBar_c class handles them as it always did) and cancel calls "dmPadBtn"(1) (hle_dos2_touch.c).
// dmPanel(id) = what the radial menu does when A is pressed on a sector:
//   panelSelect_c.swf closeMenu(true) -> ExternalInterface "openPanel"(panel id) -> the class of UI type 0x53 (vtable main+0x29e9e98, created per
//   player by the set builder at 0x10c0a38..0x10c0a4c, handle in the player's record +0x140; the object lives hidden while the menu is closed):
//   vt[0] 0x1218580 OnFunctionCalled(this, name, count, args): strcmp "openPanel" -> id = (u32)args[0].d (0x12185e8), player = u16 [this+0xf4]
//   -> 0x121a140(this, id): ids 0..5 through the byte table main+0x1f52d87 -> 0x10c0360([main+0x3298140], player, kind, 1, character):
//       id 0 Inventory (kind 2)  1 Character (1)  2 Skills (3)  3 Crafting (5)  4 Journal (7)  5 Equipment (0)   (labels: main+0x1219bd4..0x1219d18)
//   The host finds the panelSelect_c object (UI manager [main+0x2bbb230]: the list +0xf8 / count bits 24..47 of +0x100, then the handle table
//   +0x10 / +0x1c) - the one of the tapping movie's player (u16 +0xf4) when there is one - checks its vtable and calls its vt[0] with
//   ("openPanel", 1, {type 4, d = id}): the same entry, same thread (the ExternalInterface drain of the game thread), same checks as the menu.
// Switches (need DM_DOS2_TOUCHUI=1 and the pack with bottomBar_d.swf):
//   DM_DOS2_TOUCHUI_VIS=0            no "dmPanel" handler (the buttons do nothing); the movie itself: DM_DOS2_TOUCHUI_DENY=3b (stock bottomBar_c)
//   DM_DBG=1                         every dmPanel call (id, the object it went to)
//   DM_DOS2_COLUMN=0|1|2             the column's state until the app sets it (shown / folded / hidden; the gear row "Колонка кнопок")
#ifdef DM_GAME_DOS2
#include "hle.h"
#include "hle_dos2_touchui_ei.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

enum { VS_OPENPANEL = 0x1218580, VS_OPENARG = 0x12185e8, VS_OPEN = 0x121a140, VS_PS_CREATE = 0x10c0a38, VS_PS_VT = 0x29e9e98, VS_UIMGR = 0x2bbb230,
       VS_MGR_OBJS = 0x10, VS_MGR_NOBJ = 0x1c, VS_MGR_LIST = 0xf8, VS_MGR_LISTN = 0x100, VS_OBJ_TYPE = 0xf0, VS_OBJ_PLAYER = 0xf4, VS_T_PANELSEL = 0x53, VS_PANELS = 6 };
static const uint32_t k_vs_openpanel[12] = {0xa9bd57f6 /* stp x22,x21,[sp,#-0x30]! */, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0xaa0103f6 /* x22 = name */, 0xaa0003f3 /* x19 = this */,
    0xd0006301 /* adrp x1,main+0x1e7a000 */, 0x910f3c21 /* add x1,x1,#0x3cf: "openPanel" */, 0xaa1603e0, 0xaa0303f4 /* x20 = args */, 0x2a0203f5, 0x94276f89 /* bl strcmp */};
static const uint32_t k_vs_openarg[8] = {0xfd401e80 /* ldr d0,[x20,#0x38]: args[0].d */, 0x1e790014 /* fcvtzu w20,d0: the panel id */, 0x90010408, 0xf940a100 /* ldr x0,[main+0x3298140] */,
    0x7941ea61 /* ldrh w1,[x19,#0xf4]: the player */, 0x340007a1, 0xb940f408, 0x93403c29};
static const uint32_t k_vs_open[10] = {0xf81d0ff5 /* str x21,[sp,#-0x30]! */, 0xa9014ff4, 0xa9027bfd, 0x910083fd, 0xd00103e8, 0xf940a113 /* ldr x19,[main+0x3298140] */, 0xb4000453,
    0x7941e814 /* ldrh w20,[x0,#0xf4] */, 0x2a0103f5 /* w21 = the id */, 0x34000474};
static const uint32_t k_vs_ps_create[6] = {0x52800a62 /* mov w2,#0x53: panelSelect_c */, 0xaa1403e0, 0x321f0be1 /* mov w1,#0xe: layer */, 0x321903e4, 0x97e8ac6a /* bl CreateUIObject */,
    0xf900a2c0 /* str x0,[x22,#0x140] */};
static const uint8_t k_vs_kinds[VS_PANELS] = {0x00, 0x0b, 0x1f, 0x13, 0x27, 0x2f};   // main+0x1f52d87: branch offsets / 4 from 0x121a260 for the ids 0..5
static const char *const k_vs_names[VS_PANELS] = {"Inventory", "Character", "Skills", "Crafting", "Journal", "Equipment"};

typedef struct { uint32_t off, n; const uint32_t *want; } vs_site;
typedef void (*vs_called)(void *self, const char *name, unsigned count, const dm_ei_val *args);
static module_t *g_main;
static int g_on, g_dbg;
static unsigned g_n, g_fail;

static uint8_t *vs_find(const uint8_t *from) {   // the panelSelect_c object: of the caller's player when the ids match, else the first one
    uint8_t *mgr = *(uint8_t *volatile *)(g_main->base + VS_UIMGR), *any = NULL;
    if (!mgr) return NULL;
    const uint16_t who = from ? *(const volatile uint16_t *)(from + VS_OBJ_PLAYER) : 0;
    const uint64_t vt = (uint64_t)(uintptr_t)(g_main->base + VS_PS_VT);
    for (int pass = 0; pass < 2; pass++) {
        uint8_t **list = *(uint8_t **volatile *)(mgr + (pass ? VS_MGR_OBJS : VS_MGR_LIST));
        const uint64_t n = pass ? *(volatile uint32_t *)(mgr + VS_MGR_NOBJ) : (*(volatile uint64_t *)(mgr + VS_MGR_LISTN) >> 24) & 0xffffff;
        if (!list || n > 4096) continue;
        for (uint64_t i = 0; i < n; i++) {
            uint8_t *o = list[i];
            if (!o || ((uintptr_t)o & 7) || !((uintptr_t)o >> 12) || ((uintptr_t)o >> 48)) continue;
            if (*(volatile uint32_t *)(o + VS_OBJ_TYPE) != VS_T_PANELSEL || *(volatile uint64_t *)o != vt) continue;
            if (*(volatile uint16_t *)(o + VS_OBJ_PLAYER) == who) return o;
            if (!any) any = o;
        }
        if (any) return any;
    }
    return NULL;
}
int dm_dos2_touchui_may_press(const void *ui);   // hle_dos2_touchui.c: not from a window under a modal menu
static int vs_ei_panel(void *ctx, void *player, void *ui, const char *name, unsigned n, const dm_ei_val *a) {   // ExternalInterface "dmPanel"(id)
    (void)ctx; (void)player; (void)name;
    const int id = n && a && a[0].type == 4 ? (int)a[0].d : -1;
    if (id < 0 || id >= VS_PANELS) { if (g_fail++ < 20) LOG("[vis] dmPanel(%d): no such panel (0..%d)", id, VS_PANELS - 1); return 1; }
    if (!dm_dos2_touchui_may_press(ui)) { if (g_fail++ < 20) LOG("[vis] dmPanel(%d %s): refused, the HUD is under a modal window", id, k_vs_names[id]); return 1; }
    uint8_t *ps = vs_find((const uint8_t *)ui);
    if (!ps) { if (g_fail++ < 20) LOG("【红】[vis] dmPanel(%d %s): no panelSelect_c object (UI type %#x) in the UI manager: nothing opened", id, k_vs_names[id], VS_T_PANELSEL); return 1; }
    dm_ei_val v; memset(&v, 0, sizeof v); v.type = 4; v.d = (double)id;
    g_n++;
    if (g_dbg || g_n <= 3) LOG("[vis] dmPanel(%d %s) #%u: \"openPanel\" to the panelSelect_c object %p (player %u), as the radial menu does", id, k_vs_names[id], g_n, (void *)ps,
                               (unsigned)*(volatile uint16_t *)(ps + VS_OBJ_PLAYER));
    ((vs_called *)(void *)*(uint8_t **)ps)[0](ps, "openPanel", 1, &v);   // vt[0] = main+0x1218580 (checked by vs_find)
    return 1;
}
// DM_DBG=1: the calls the tap fixes of tools/dos2_taps_touch.py end in, one line each (the game's class still gets them: 0 = not taken).
// SlotPressed = a hot bar slot, ButtonPressed = a message box prompt, charSel = a portrait, OkPressed / cancelNonModal = a tutorial.
static int vs_ei_tap(void *ctx, void *player, void *ui, const char *name, unsigned n, const dm_ei_val *a) {
    (void)ctx; (void)player;
    static unsigned cnt;
    if (cnt++ < 400)
        LOG("[taps] ExternalInterface %s(%s%.0f%s%.0f) from UI type %#x (#%u)", name, n > 0 && a && a[0].type == 4 ? "" : "?", n > 0 && a && a[0].type == 4 ? a[0].d : 0.0,
            n > 1 ? ", " : "", n > 1 && a ? (a[1].type == 3 ? (double)a[1].b : a[1].type == 4 ? a[1].d : -1.0) : 0.0, ui ? *(volatile uint32_t *)((uint8_t *)ui + VS_OBJ_TYPE) : 0, cnt);
    return 0;
}
// ---- the column's state (owner 15.11): shown / folded / hidden, from the gear (Java: DmActivity.nativeSetDos2Column, live) and the gamepad rule ----
// bottomBar_d asks every ~30 frames: ExternalInterface "dmColumnPoll"(the state it shows, -1 = none yet). When that differs from what the host wants
// the host invokes "setActiveInCombat"(100 + state) on that movie: an invoke needs a name the engine registered for the movie's class (the
// name table of the FlashPlayer, hle_dos2_touchui_ei.c dm_dos2_touchui_invoke) and bottomBar_d's wrapper of setActiveInCombat takes a Number
// >= 100 as dmColumn(n - 100) (the engine itself only ever sends a Boolean; tools/dos2_hud_touch.py). Same thread as the movie (the drain).
// Default before Java says anything: DM_DOS2_COLUMN=0 shown / 1 folded / 2 hidden (default 0).
static _Atomic int g_col_want = -1;
static int g_col_env;
static unsigned g_col_n, g_col_fail;
int dm_dos2_column_set(int state);   // jni_android.c nativeSetDos2Column: 0 shown, 1 folded, 2 hidden; 0 = not possible (no touch UI)
int dm_dos2_column_set(int state) {
    if (state < 0 || state > 2) return 0;
    const int was = atomic_exchange(&g_col_want, state);
    if (was != state) LOG("[vis] column of bottomBar_d: %s (from the app; applied at the movie's next poll)", state == 0 ? "shown" : state == 1 ? "folded" : "hidden");
    return g_main != NULL;
}
static int vs_ei_colpoll(void *ctx, void *player, void *ui, const char *name, unsigned n, const dm_ei_val *a) {   // ExternalInterface "dmColumnPoll"(state shown)
    (void)ctx; (void)ui; (void)name;
    const int have = n && a && a[0].type == 4 ? (int)a[0].d : -1;
    int want = atomic_load(&g_col_want); if (want < 0) want = g_col_env;
    { uint32_t ty = 0; char mv[48]; void dm_dos2_touchui_movie_of(const void *player, uint32_t *type, char *buf, size_t n); dm_dos2_touchui_movie_of(player, &ty, mv, sizeof mv);
      if (ty == 0x8d) {   // the column's own movie (dmColumn_d.swf, watermark class): its channel is that class's invoke name, no edge / right logic
          if (player && n >= 3) {   // pack 3s: placed by the minimap's real rect (dm_dos2_touch_col_place)
              int dm_dos2_touch_col_place(const void *col_player, int *rx, int *ty); int rx, ty2;
              if (dm_dos2_touch_col_place(player, &rx, &ty2)) {
                  const int sx = a[1].type == 4 ? (int)a[1].d : -1, sy = a[2].type == 4 ? (int)a[2].d : -1;
                  dm_ei_val e; memset(&e, 0, sizeof e); e.type = 4;
                  if (abs(rx - sx) > 1) { e.d = 10000.0 + rx; const int ok = dm_dos2_touchui_invoke(player, "addWatermarkText", &e, 1);
                      static unsigned nx; if (nx++ < 8 || g_dbg) LOG("[vis] column movie: right side at stage x %d (shown %d; the minimap ring's right side) -> %s", rx, sx, ok ? "ok" : "FAILED"); }
                  if (abs(ty2 - sy) > 1) { e.d = 20000.0 + ty2; const int ok = dm_dos2_touchui_invoke(player, "addWatermarkText", &e, 1);
                      static unsigned ny; if (ny++ < 8 || g_dbg) LOG("[vis] column movie: top at stage y %d (shown %d; under the minimap ring) -> %s", ty2, sy, ok ? "ok" : "FAILED"); }
              }
          }
          if (have == want || !player) return 1;
          dm_ei_val v; memset(&v, 0, sizeof v); v.type = 4; v.d = 100.0 + want;
          const int ok = dm_dos2_touchui_invoke(player, "addWatermarkText", &v, 1);
          if (ok) { g_col_n++; if (g_dbg || g_col_n <= 6) LOG("[vis] column state %d -> %d: invoke addWatermarkText(%d) on dmColumn_d (#%u)", have, want, 100 + want, g_col_n); }
          else if (g_col_fail++ < 5) LOG("【红】[vis] column state %d -> %d: dmColumn_d's player has no invoke name \"addWatermarkText\": the column stays as it is", have, want);
          return 1;
      } }
    if (player && n >= 2) {   // pack 3h: the right-anchored buttons follow the window's real right edge (dmColEdge)
        // pack 3i (run t3h: no "column edge" line, the column stayed put): before the first dmColEdge the movie's dmEdge is undefined, not a Number
        int dm_dos2_touch_stage_edge(const void *player); const int edge = dm_dos2_touch_stage_edge(player), shown = a[1].type == 4 ? (int)a[1].d : -1;
        if (edge >= 1920 && edge != shown) {
            dm_ei_val e; memset(&e, 0, sizeof e); e.type = 4; e.d = 10000.0 + edge;
            const int ok = dm_dos2_touchui_invoke(player, "setActiveInCombat", &e, 1);
            static unsigned ne; if (ne++ < 8 || g_dbg) LOG("[vis] column edge: stage x %d (shown %d) -> invoke setActiveInCombat(%d) on bottomBar_d: %s", edge, shown, 10000 + edge, ok ? "ok" : "FAILED");
        }
        if (n >= 4) {   // pack 3x: the options button just left of the minimap ring (poll args: state, edge, column right, options x = a[3])
            int dm_dos2_touch_opt_x(const void *bb_player); const int ox = dm_dos2_touch_opt_x(player), os = a[3].type == 4 ? (int)a[3].d : -1;
            if (ox > 0 && abs(ox - os) > 1) {
                dm_ei_val o; memset(&o, 0, sizeof o); o.type = 4; o.d = 20000.0 + ox;
                const int ok = dm_dos2_touchui_invoke(player, "setActiveInCombat", &o, 1);
                static unsigned no; if (no++ < 8 || g_dbg) LOG("[vis] options button: document x %d (left of the minimap ring; shown %d) -> setActiveInCombat(%d): %s", ox, os, 20000 + ox, ok ? "ok" : "FAILED");
            }
        }
        if (0) {}
        else if (edge >= 1920 && n >= 3) {   // pack 3o: the column's right side on the minimap ring's right side (once the edge is in place)
            int dm_dos2_touch_col_right(const void *player, int edge); const int c = dm_dos2_touch_col_right(player, edge), cs = a[2].type == 4 ? (int)a[2].d : -1;
            if (c > 0 && abs(c - cs) > 1) {
                dm_ei_val r; memset(&r, 0, sizeof r); r.type = 4; r.d = 30000.0 + c;
                const int ok = dm_dos2_touchui_invoke(player, "setActiveInCombat", &r, 1);
                static unsigned nr; if (nr++ < 8 || g_dbg) LOG("[vis] column right: document x %d (minimap ring's right side; shown %d) -> setActiveInCombat(%d) on bottomBar_d: %s", c, cs, 30000 + c, ok ? "ok" : "FAILED");
            }
        }
    }
    if (have == want || !player) return 1;
    dm_ei_val v; memset(&v, 0, sizeof v); v.type = 4; v.d = 100.0 + want;
    const int ok = dm_dos2_touchui_invoke(player, "setActiveInCombat", &v, 1);
    if (ok) { g_col_n++; if (g_dbg || g_col_n <= 6) LOG("[vis] column state %d -> %d: invoke setActiveInCombat(%d) on bottomBar_d (#%u)", have, want, 100 + want, g_col_n); }
    else if (g_col_fail++ < 5) LOG("【红】[vis] column state %d -> %d: the movie's player has no invoke name \"setActiveInCombat\": the column stays as it is", have, want);
    return 1;
}
void dm_dos2_touchui_vis_install(void);   // main.c host_preload, after dm_dos2_touchui_ei_install
void dm_dos2_touchui_vis_install(void) {
    int dm_dos2_touchui_on(void);
    const char *v;
    if (!dm_dos2_touchui_on()) return;
    if ((v = getenv("DM_DOS2_TOUCHUI_VIS")) && *v == '0') { LOG("[vis] DM_DOS2_TOUCHUI_VIS=0: no \"dmPanel\" handler (the panel column of bottomBar_d does nothing)"); return; }
    g_dbg = (v = getenv("DM_DBG")) && atoi(v) > 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_main = &g_mod[i];
    module_t *m = g_main;
    if (!m) return;
    static const vs_site k_sites[] = {{VS_OPENPANEL, 12, k_vs_openpanel}, {VS_OPENARG, 8, k_vs_openarg}, {VS_OPEN, 10, k_vs_open}, {VS_PS_CREATE, 6, k_vs_ps_create}};
    for (unsigned i = 0; i < sizeof k_sites / sizeof *k_sites; i++)
        if ((uint64_t)k_sites[i].off + 4ull * k_sites[i].n > m->text_end || memcmp(m->base + k_sites[i].off, k_sites[i].want, 4ull * k_sites[i].n)) {
            LOG("【红】[vis] main+%#x differs: not DOS2 update v786432 main (build 4979b200) at the radial menu's openPanel: the panel column of bottomBar_d does nothing", k_sites[i].off);
            g_main = NULL; return; }
    if (memcmp(m->base + 0x1f52d87, k_vs_kinds, sizeof k_vs_kinds)) { LOG("【红】[vis] main+0x1f52d87 differs (the panel id table): the panel column of bottomBar_d does nothing"); g_main = NULL; return; }
    g_on = dm_dos2_touchui_ei_register("dmPanel", vs_ei_panel, NULL);
    if ((v = getenv("DM_DOS2_COLUMN")) && *v >= '0' && *v <= '2') g_col_env = *v - '0';
    dm_dos2_touchui_ei_register("dmColumnPoll", vs_ei_colpoll, NULL);
    if (g_dbg) {
        static const char *const k_tap[] = {"SlotPressed", "ButtonPressed", "charSel", "centerCamOnCharacter", "OkPressed", "cancelNonModal"};
        for (unsigned i = 0; i < sizeof k_tap / sizeof *k_tap; i++) dm_dos2_touchui_ei_register(k_tap[i], vs_ei_tap, NULL);
        LOG("[taps] DM_DBG: ExternalInterface SlotPressed / ButtonPressed / charSel / centerCamOnCharacter / OkPressed / cancelNonModal are logged (tools/dos2_taps_touch.py)");
    }
    LOG("[vis] visible touch elements: ExternalInterface \"dmPanel\"(0 Inventory, 1 Character, 2 Skills, 3 Crafting, 4 Journal, 5 Equipment) of bottomBar_d = \"openPanel\" of the radial "
        "menu's class (main+%#x)%s", VS_OPENPANEL, g_on && dm_dos2_touchui_ei_on() ? "" : " - NOT active: the ExternalInterface hook is off (DM_DOS2_TOUCHUI_EI=0 or its sites differ)");
}
#endif
