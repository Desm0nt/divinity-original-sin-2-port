// [DOS2] The iPad touch HUD in game, step 1 (DM_DOS2_TOUCHUI_HUD, default 0; docs/dos2/TOUCH_UI_PLAN.md TU-07). Static only (not run on a phone
// when written). main = main.raw of update v786432, build 4979b200, offsets = main.raw; iPad 3.6.72 addresses = Mach-O VA.
// The iPad's per-player UI set builder 0x1012dfea4 has three branches by the mode it stores in the player's record (+0x348): 0 = keyboard and
// mouse, 1 = controller, 2 = touch. Its touch branch (0x1012e0268, creations out of line from 0x1012e0b48) makes, with
// CreateUIObject(mgr, layer, type, flags | the builder's flags, 0x80, player):
//     statusConsole_t 0x98  layer 6     flags 0x201e0      hotBar_t 0x97          layer 0x10  flags 0x201e0
//     minimap_t       0x94  layer 2     flags 0x221e0      playerInfo_t 0x96      layer 0xf   flags 0x201e0
//     combatLog_t     0xce  layer 0xa   flags 0x201e0      rightSideButtons 0x99  layer 0x12  flags 0x201e0   journal_csp_t 0x95 layer 0xf
//     character switcher 0xa8 layer 0x16, magnifier 0xcc layer 0 flags 0x261e0, panels 0x9b..0x9e / 0xa7 layer 0xa (0x9b: 0xc), then the shared
//     tail with touch ids (gameMenu_t 0xca layer 0x1f, contextMenu_t 0x9f / 0xa0 layer 0x16, msgBox_t 0xae layer 0x22, examine_t 0xb4 layer 0xd,
//     tutorialBox_t 0xb9, textDisplay_t 0xaa layer 9, worldTooltip_t 0xbe layer 0, overhead_t 0xbb layer 1, notification_t 0xcd layer 0x16).
//   Its PC branch (0x1012e0118) for comparison: statusConsole 0x75 layer 6, hotBar 0x28 layer 0xf, minimap 0x1e layer 2, combatLog 7 layer 0xa,
//   playerInfo 0x26 layer 7, all flags 0x201e0. The touch objects have record slots of their own (+0x2b8..+0x338) that the Switch record lacks.
// The Switch builder 0x10c06d0 (record = hash node of [main+0x3298140]+0xf0; mode byte main+0x2bb8918 read at 0x10c07ac): mode 1 = the `_c`
//   block 0x10c07f4.. (bottomBar_c 0x3b layer 2 flags 0x261e0 -> record +0x58, 0x10c07fc..0x10c0818; minimap_c 0x3c -> +0x50; combatLog_c 0x41
//   -> +0x68 ...), any other mode = only the shared tail 0x10c0b30 (worldTooltip 0x30, overhead 5, 0xf, 0, notification 0x24, 0x25, 0x10, 9,
//   journal_c 0x46). A slot is only filled when it is 0, nothing else is done with a new object (no Show call): the objects run themselves.
//   No CreateUIObject call for the PC HUD types exists in main, but their classes are registered (hotBar 0x28 creator 0x10b60d0 ...).
// So the host makes the touch HUD itself, with the PC classes playing the iPad movies (TOUCH_UI_PLAN.md 3.1 group B):
//     type 0x28 hotBar -> GUI/hotBar_t.swf, 0x1e minimap -> minimap_t, 0x26 playerInfo -> playerInfo_t, 0x75 statusConsole -> statusConsole_t,
//     7 combatLog -> combatLog_t; layers and flags of the iPad's touch branch (table above), flags | 0x20000000 (what create-all passes).
//   The creator's path is rewritten the way hle_dos2_touchui.c does it (the tail after the last "GUI/", in place when the string's allocation
//   holds it, else pointer and length swapped for a host buffer); nobody else creates these types, so the change stays.
//   The handles are the host's (the Switch record has no slot for them); they are destroyed the way the game destroys a slot
//   (0x10bc7dc..0x10bc864: handle -> object through [mgr+0x10] / [mgr+0x1c] / [mgr+0x30], `obj->vt[0xf0 / 8](obj)`, DestroyUIObject
//   0xaeba10(mgr, &handle), then the message {vtable main+0x2999ec0, u32 1, obj} to 0xa47030([main+0x2bc0530], &msg)).
// When: the model "the set follows the last input" is DM_DOS2_TOUCH_LIVE (hle_dos2_touch.c pass 4: touch -> mode byte 0, pad input -> 1, each
//   with destroy-all 0x10c4730 + create-all 0x10c4580 on the game thread in the pad poll). This file hooks those two functions:
//     before destroy-all: the host's touch objects go first; after create-all: with the mode byte 0 they are made for every player record.
//   And it gates the switch (dm_dos2_touchui_hud_gate, one call in dt_live_switch): only in game (player records exist), only in game state
//   Running, and not while a dialog / loading screen / character creation is on screen:
//     game state = u32 [[[[main+0x3298150]+0x30]]+0x10] (the builder reads it at 0x10c0764..0x10c0784 and compares with 0x1e); the ids most
//     compared in main are 0x13 (14 sites), 7 (12), 0x1c (5), 0x11 (3) = Running, Menu, GameMasterPause, Paused of the PC enumeration
//     (ecl::GameState; the iPad's rebuild 0x1012f1f58 uses the same numbers). Allowed: 0x13 (DM_DOS2_TOUCHUI_HUD_STATES=<hex,..>; `any`).
//     blocking UI = a visible (flags +0x14 bit 3, as 0x10c0d30 tests) object of type dialog 0xe / dialog_c 0x42, 0x65, loadingScreen 0x17,
//     characterCreation 3 / 4 (DM_DOS2_TOUCHUI_HUD_BLOCK=<hex,..>).
//   A switch to touch that is not allowed is dropped (the next touch asks again); a switch back to the pad is retried on every poll until
//   allowed. Outside the game the mode byte is never 0 with this switch: menus and loading stay in the controller mode the menus were checked in
//   (run tu04); when the game destroys the set itself (unload) while the touch set is up, the byte goes back to 1.
// The pad is never turned off by the host; in the touch set the GAME drops the pad's records (its filter 0x844014), the host sees them in the
// poll and switches back (LIVE). A pad that also works under the touch HUD is TU-14.
// Switches (need DM_DOS2_TOUCHUI=1 with the package):
//   DM_DOS2_TOUCHUI_HUD=1            on (turns DM_DOS2_TOUCH_LIVE=1 on when it is unset)
//   DM_DOS2_TOUCHUI_HUD_SET=<hex,..> only these types of the table (default 28,1e,26,75,7): to find the one that breaks
//   DM_DOS2_TOUCHUI_HUD_PC=1         keep the PC movies (hotBar.swf ...): the PC classes with their own movies, for an A/B run
//   DM_DOS2_TOUCHUI_HUD_STATES / _BLOCK  see above
//   DM_DBG=1                         every switch asked / refused (state, blocking type), every object made or destroyed, timing
#ifdef DM_GAME_DOS2
#include "hle.h"
#include "hle_dos2_touchui_ei.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

enum { HU_CREATE_ALL = 0x10c4580, HU_DESTROY_ALL = 0x10c4730, HU_SLOT_DESTROY = 0x10bc7dc, HU_DESTROY_OBJ = 0xaeba10, HU_MSG_SEND = 0xa47030, HU_MSG_VT = 0x2999ec0,
       HU_MSG_TO = 0x2bc0530, HU_STATE_RD = 0x10c0764, HU_STATE = 0x3298150, HU_C_SITE = 0x10c07fc, HU_LOOKUP = 0xaebc10, HU_CREATE = 0xaebbf0, HU_UIMGR = 0x2bbb230,
       HU_UI_THIS = 0x3298140, HU_MODE = 0x2bb8918, HU_MODE_RD = 0x10c07ac, HU_VIS_RD = 0x10c0d30,
       HU_HASH_N = 0xd8, HU_HASH_B = 0xe0, HU_OBJS = 0x10, HU_OBJS_N = 0x1c, HU_SALTS = 0x30, HU_OBJ_FLAGS = 0x14, HU_OBJ_TYPE = 0xf0, HU_UI_HASH = 0xf0 };
static const uint32_t k_hu_create_all[4] = {0xd101c3ff /* sub sp,sp,#0x70 */, 0xa90457f6, 0xa9054ff4, 0xa9067bfd};
static const uint32_t k_hu_destroy_all[4] = {0xd10103ff /* sub sp,sp,#0x40 */, 0xa9024ff4, 0xa9037bfd, 0x9100c3fd};
static const uint32_t k_hu_slot_destroy[35] = {0xf940b689 /* ldr x9,[x20,#0x168]: a slot */, 0xb4000449, 0x9105a299, 0xf260553f /* tst x9,#0x3fffff00000000 */, 0x540003c0,
    0xb9401eca /* ldr w10,[x22,#0x1c]: objects */, 0x92407d28, 0xeb0a011f, 0x54000342, 0xf9401aca /* ldr x10,[x22,#0x30]: salts */, 0xb868794a, 0xd360d529, 0x6b0a013f, 0x540002a1,
    0xf9400ac9 /* ldr x9,[x22,#0x10] */, 0xf868793a /* the object */, 0xb400025a, 0xf9400348, 0xf9407908 /* ldr x8,[x8,#0xf0] */, 0xaa1a03e0, 0xd63f0100 /* blr x8 */,
    0xaa1603e0, 0xaa1903e1 /* x1 = &slot */, 0x97e8bc76 /* bl 0xaeba10 */, 0x320003e8, 0xb90013e8 /* u32 1 */, 0xb000c6e8, 0x913b0108 /* main+0x2999ec0 */, 0xf90007e8,
    0x9000d828, 0xf9429900 /* ldr x0,[main+0x2bc0530] */, 0x910023e1, 0xf9000ffa /* the object */, 0x97e629f4 /* bl 0xa47030 */, 0xf900033f /* slot = 0 */};
static const uint32_t k_hu_destroy_obj[8] = {0xf81c0ff7, 0xa90157f6, 0xa9024ff4, 0xa9037bfd, 0x9100c3fd, 0xf9400035 /* ldr x21,[x1]: the handle */, 0xf26056bf, 0x54000c80};
static const uint32_t k_hu_msg_send[4] = {0xd10103ff, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};
static const uint32_t k_hu_state_rd[10] = {0x90010edc /* adrp x28,main+0x3298000 */, 0xf940ab88 /* ldr x8,[x28,#0x150] */, 0xf9401908 /* ldr x8,[x8,#0x30] */, 0xf9400108 /* ldr x8,[x8] */,
    0xf000d7c9, 0xf9410929, 0x39404129, 0xb9401108 /* ldr w8,[x8,#0x10]: the state */, 0x7100791f /* cmp w8,#0x1e */, 0x1a9f17e8};
static const uint32_t k_hu_c_site[8] = {0x79401be5 /* ldrh w5,[sp,#0xc]: player */, 0x2a1b02a3 /* orr w3,w21,w27 */, 0x52800762 /* mov w2,#0x3b */, 0xaa1403e0, 0x321f03e1 /* mov w1,#2 */,
    0x321903e4 /* mov w4,#0x80 */, 0x97e8acf7 /* bl 0xaebbf0 */, 0xf9002ec0 /* str x0,[x22,#0x58] */};
static const uint32_t k_hu_lookup[6] = {0xb940d808, 0x1ac80849, 0x1b088928, 0xf9407009, 0xf8685928, 0xb4002aa8};   // CreateUIObject's type lookup (its prologue is hooked by hle_dos2_touchui.c)
static const uint32_t k_hu_mode_rd[2] = {0x39646118 /* ldrb w24,[x8,#0x918] */, 0xf9406ec8};
static const uint32_t k_hu_vis_rd[2] = {0x39405108 /* ldrb w8,[x8,#0x14] */, 0x361fd368 /* tbz w8,#3: not visible */};

typedef struct { uint32_t off, n; const uint32_t *want; } hu_site;
typedef struct { uint32_t cap1, len; char *ptr; } hu_str;
typedef struct { void *fn; hu_str path; } hu_creator;
typedef struct hu_node { struct hu_node *next; uint32_t id, pad; hu_creator *cr; } hu_node;
typedef uint64_t (*hu_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

enum { HU_NTYPES = 5, HU_MAXOBJ = 32 };
static struct { uint32_t type, layer, flags; const char *pc, *touch; int on, ready; char full[256]; } g_t[HU_NTYPES] = {
    {0x75, 6, 0x201e0, "GUI/statusConsole.swf", "GUI/statusConsole_t.swf", 1, 0, {0}},
    {0x1e, 2, 0x221e0, "GUI/minimap.swf", "GUI/minimap_t.swf", 1, 0, {0}},
    {0x26, 0xf, 0x201e0, "GUI/playerInfo.swf", "GUI/playerInfo_t.swf", 1, 0, {0}},
    {0x28, 0x10, 0x201e0, "GUI/hotBar.swf", "GUI/hotBar_t.swf", 1, 0, {0}},
    {7, 0xa, 0x201e0, "GUI/combatLog.swf", "GUI/combatLog_t.swf", 1, 0, {0}}};
static struct { uint64_t h; uint32_t type; uint16_t player; } g_obj[HU_MAXOBJ];
static int g_nobj, g_on, g_dbg, g_pc_movies, g_paths_done, g_switching, g_any_state;
static uint32_t g_states[8] = {0x13}, g_block[16] = {0xe, 0x42, 0x65, 0x17, 3, 4};
static int g_nstates = 1, g_nblock = 6;
static unsigned g_nswitch, g_nrefused;
static module_t *g_main;
static hu_fn g_create_all_orig, g_destroy_all_orig;

static uint64_t hu_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000ull; }
static uint8_t *hu_mgr(void) { return *(uint8_t *volatile *)(g_main->base + HU_UIMGR); }
static volatile uint8_t *hu_mode(void) { return (volatile uint8_t *)(g_main->base + HU_MODE); }
static int hu_state(void) {   // the game state id, -1 = no state machine yet
    uint8_t *p = *(uint8_t *volatile *)(g_main->base + HU_STATE); if (!p) return -1;
    uint8_t **q = *(uint8_t **volatile *)(p + 0x30); if (!q) return -1;
    uint8_t *r = *q; if (!r) return -1;
    return (int)*(volatile uint32_t *)(r + 0x10);
}
static uint8_t *hu_obj(uint8_t *mgr, uint64_t h) {   // handle -> UI object, the game's own checks (main+0x10bc7e8..0x10bc81c)
    if (!(h & 0x3fffff00000000ull)) return NULL;
    const uint64_t idx = h & 0xffffffffull;
    if (idx >= *(volatile uint32_t *)(mgr + HU_OBJS_N)) return NULL;
    const uint32_t *salt = *(const uint32_t *volatile *)(mgr + HU_SALTS); uint8_t **objs = *(uint8_t **volatile *)(mgr + HU_OBJS);
    if (!salt || !objs || salt[idx] != (uint32_t)((h >> 32) & 0x3fffff)) return NULL;
    return objs[idx];
}
static int hu_blocking(uint8_t *mgr) {   // type of a visible UI object that forbids a switch, -1 = none
    const uint32_t n = *(volatile uint32_t *)(mgr + HU_OBJS_N); uint8_t **objs = *(uint8_t **volatile *)(mgr + HU_OBJS);
    if (!objs || n > 4096) return -1;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *o = objs[i];
        if (!o || !(o[HU_OBJ_FLAGS] & 8)) continue;
        const uint32_t t = *(const volatile uint32_t *)(o + HU_OBJ_TYPE);
        for (int k = 0; k < g_nblock; k++) if (g_block[k] == t) return (int)t;
    }
    return -1;
}
static int hu_records(const uint8_t *ui, uint16_t *ids, int max) {   // the players that have a UI set (hash ui+0xf0: +4 buckets, +8 array; node +8 u16 id)
    const uint32_t nb = *(const volatile uint32_t *)(ui + HU_UI_HASH + 4);
    uint8_t *const volatile *bk = *(uint8_t *const volatile *const volatile *)(ui + HU_UI_HASH + 8);
    int n = 0;
    if (!bk || !nb || nb > 0x10000) return 0;
    for (uint32_t i = 0; i < nb; i++) for (const uint8_t *nd = bk[i]; nd && n < max; nd = *(const uint8_t *const *)(const void *)nd) { if (ids) ids[n] = *(const uint16_t *)(const void *)(nd + 8); n++; }
    return n;
}

// ---- the movies: the creators' paths of the PC HUD types -> the touch movies ----
static void hu_paths(uint8_t *mgr) {
    int dm_dos2_touchui_pak_has(const char *gui_path);
    g_paths_done = 1;
    const uint32_t nb = *(volatile uint32_t *)(mgr + HU_HASH_N); hu_node *volatile *bk = *(hu_node *volatile *volatile *)(mgr + HU_HASH_B);
    if (!bk || !nb || nb > 0x10000) { LOG("【红】[hud] the UI type table is not there: no touch HUD"); return; }
    for (int i = 0; i < HU_NTYPES; i++) {
        if (!g_t[i].on) continue;
        hu_node *nd = bk[g_t[i].type % nb]; int guard = 0;
        while (nd && nd->id != g_t[i].type && guard++ < 4096) nd = nd->next;
        hu_creator *cr = nd && nd->id == g_t[i].type ? nd->cr : NULL;
        if (!cr || !(*(const uint8_t *)&cr->path & 1) || !cr->path.ptr || cr->path.len < 9 || cr->path.len >= 200 || cr->path.len + 1 > (cr->path.cap1 & ~1u)) {
            LOG("【红】[hud] type %#x: no creator with a long-form path: not made", g_t[i].type); continue; }
        const char *p = cr->path.ptr; const uint32_t len = cr->path.len; int tail = -1;
        for (int k = (int)len - 8; k >= 0; k--) if (!memcmp(p + k, "GUI/", 4) && (k == 0 || p[k - 1] == '/')) { tail = k; break; }
        if (tail < 0) { LOG("【红】[hud] type %#x: path without GUI/: not made", g_t[i].type); continue; }
        const uint32_t tl = len - (uint32_t)tail;
        if (tl == strlen(g_t[i].touch) && !memcmp(p + tail, g_t[i].touch, tl)) { g_t[i].ready = 1; continue; }   // already the touch movie (DM_DOS2_TOUCHUI_MAP)
        if (tl != strlen(g_t[i].pc) || memcmp(p + tail, g_t[i].pc, tl)) { LOG("【红】[hud] type %#x has %.*s, expected %s: not made", g_t[i].type, (int)tl, p + tail, g_t[i].pc); continue; }
        if (g_pc_movies) { g_t[i].ready = 1; LOG("[hud] type %#x keeps %s (DM_DOS2_TOUCHUI_HUD_PC)", g_t[i].type, g_t[i].pc); continue; }
        if (!dm_dos2_touchui_pak_has(g_t[i].touch)) { LOG("【红】[hud] type %#x: the package has no Public/Game/%s: not made (build the `hud` set: tools/dos2_touchui_pak.py hud)", g_t[i].type, g_t[i].touch); continue; }
        const uint32_t pl = (uint32_t)strlen(g_t[i].touch), nl = (uint32_t)tail + pl;
        if (nl + 1 > sizeof g_t[i].full) continue;
        const int in_place = nl + 1 <= (cr->path.cap1 & ~1u);
        if (in_place) { memcpy(cr->path.ptr + tail, g_t[i].touch, pl + 1); cr->path.len = nl; }
        else { memcpy(g_t[i].full, p, (size_t)tail); memcpy(g_t[i].full + tail, g_t[i].touch, pl + 1); cr->path.ptr = g_t[i].full; cr->path.len = nl; }
        g_t[i].ready = 1;
        LOG("[hud] type %#x -> %s (was %s; %s)", g_t[i].type, g_t[i].touch, g_t[i].pc, in_place ? "written in place" : "pointer and length swapped for a host buffer");
    }
}

// ---- the host's touch objects ----
static void hu_destroy(const char *why) {
    uint8_t *mgr = hu_mgr();
    if (!g_nobj) return;
    const uint64_t t0 = hu_us(); int gone = 0;
    for (int i = g_nobj - 1; i >= 0 && mgr; i--) {
        uint8_t *o = hu_obj(mgr, g_obj[i].h);
        if (!o) { if (g_dbg) LOG("[hud] type %#x player %u handle %#llx: already gone", g_obj[i].type, g_obj[i].player, (unsigned long long)g_obj[i].h); continue; }
        void **vt = *(void ***)(void *)o;
        ((void (*)(void *))vt[0xf0 / 8])(o);                                                              // as main+0x10bc820
        uint64_t h = g_obj[i].h;
        ((void (*)(void *, uint64_t *))(void *)(g_main->base + HU_DESTROY_OBJ))(mgr, &h);                  // as main+0x10bc830
        struct { void *vt; uint32_t one, pad; void *obj; } msg = {g_main->base + HU_MSG_VT, 1, 0, o};     // as main+0x10bc83c..0x10bc860
        void *to = *(void *volatile *)(g_main->base + HU_MSG_TO);
        if (to) ((void (*)(void *, void *))(void *)(g_main->base + HU_MSG_SEND))(to, &msg);
        gone++;
        if (g_dbg) LOG("[hud] destroyed type %#x player %u handle %#llx", g_obj[i].type, g_obj[i].player, (unsigned long long)g_obj[i].h);
    }
    LOG("[hud] touch HUD down (%s): %d of %d object(s) destroyed, %llu us", why, gone, g_nobj, (unsigned long long)(hu_us() - t0));
    g_nobj = 0;
}
static void hu_create(uint8_t *ui) {
    uint8_t *mgr = hu_mgr();
    if (!mgr || !ui || g_nobj) return;
    if (!g_paths_done) hu_paths(mgr);
    uint16_t ids[8]; const int np = hu_records(ui, ids, 8);
    const uint64_t t0 = hu_us(); int made = 0, failed = 0;
    for (int p = 0; p < np; p++) for (int i = 0; i < HU_NTYPES && g_nobj < HU_MAXOBJ; i++) {
        if (!g_t[i].on || !g_t[i].ready) continue;
        const uint64_t h = ((hu_fn)(void *)(g_main->base + HU_CREATE))((uint64_t)(uintptr_t)mgr, g_t[i].layer, g_t[i].type, g_t[i].flags | 0x20000000u, 0x80, ids[p]);
        const int ok = (h & 0x3fffff00000000ull) != 0;
        if (ok) { g_obj[g_nobj].h = h; g_obj[g_nobj].type = g_t[i].type; g_obj[g_nobj].player = ids[p]; g_nobj++; made++; } else failed++;
        if (g_dbg || !ok) LOG("%s[hud] CreateUIObject type %#x = %s layer %#x flags %#x player %u -> handle %#llx%s", ok ? "" : "【红】", g_t[i].type,
                              g_pc_movies ? g_t[i].pc : g_t[i].touch, g_t[i].layer, g_t[i].flags | 0x20000000u, ids[p], (unsigned long long)h, ok ? "" : ": FAILED");
    }
    LOG("%s[hud] touch HUD up: %d object(s) for %d player(s)%s, %llu us (game state %#x)", failed ? "【红】" : "", made, np, failed ? ", some failed" : "",
        (unsigned long long)(hu_us() - t0), hu_state());
    dm_dos2_touchui_ei_dump("touch HUD up");
}
static uint64_t hu_destroy_all_hook(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    if (g_on) {
        hu_destroy(g_switching ? "switch" : "the game unloads the UI set");
        if (!g_switching && *hu_mode() == 0) { *hu_mode() = 1; LOG("[hud] the game destroys the player UI sets with the touch set up: mode byte main+%#x back to 1 (menus and loading stay controller)", HU_MODE); }
    }
    return g_destroy_all_orig(a0, a1, a2, a3, a4, a5);
}
static uint64_t hu_create_all_hook(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    const uint64_t v = g_create_all_orig(a0, a1, a2, a3, a4, a5);
    if (g_on && a0 && *hu_mode() == 0) hu_create((uint8_t *)(uintptr_t)a0);
    g_switching = 0;
    return v;
}
// hle_dos2_touch.c dt_live_switch, right before it changes the mode: 1 = go on, 0 = not now. to_touch: 1 = the touch set is wanted, 0 = the controller set
int dm_dos2_touchui_hud_gate(int to_touch, int records);
int dm_dos2_touchui_hud_gate(int to_touch, int records) {
    if (!g_on) return 1;
    uint8_t *mgr = hu_mgr();
    const int st = hu_state(), blk = mgr ? hu_blocking(mgr) : -1;
    int st_ok = g_any_state;
    for (int i = 0; i < g_nstates; i++) if ((int)g_states[i] == st) st_ok = 1;
    const char *why = !mgr ? "no UI manager" : !records ? "not in game (no player UI set)" : !st_ok ? "game state" : blk >= 0 ? "a blocking UI is on screen" : NULL;
    if (why && !(why[0] == 'n' && !to_touch && !records)) {   // back to the pad outside the game: only the byte changes, let it
        if (g_dbg && (g_nrefused < 40 || !(g_nrefused % 300)))
            LOG("[hud] switch to the %s set refused: %s (game state %#x, blocking type %#x, records %d)%s", to_touch ? "touch" : "controller", why, st, blk, records,
                to_touch ? "" : "; asked again on the next poll");
        g_nrefused++;
        return 0;
    }
    g_switching = records > 0; g_nswitch++;
    if (g_dbg || g_nswitch <= 4) LOG("[hud] switch #%u to the %s set (game state %#x, records %d)", g_nswitch, to_touch ? "touch" : "controller", st, records);
    return 1;
}

static int hu_poke(module_t *m, uint32_t off, const uint32_t *w, int n) {
    uint32_t *at = (uint32_t *)(void *)(m->base + off);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF);
    const size_t len = (size_t)(((uintptr_t)(at + n) - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    memcpy(at, w, 4 * (size_t)n);
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[hud] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)(at + n));
    return 0;
}
static int hu_list(const char *v, uint32_t *out, int max) {
    int n = 0; char *dup = strdup(v), *save = NULL;
    for (char *t = dup ? strtok_r(dup, ",; ", &save) : NULL; t && n < max; t = strtok_r(NULL, ",; ", &save)) { char *end; const unsigned long x = strtoul(t, &end, 16); if (end != t && !*end) out[n++] = (uint32_t)x; }
    free(dup); return n;
}
void dm_dos2_touchui_hud_install(void);   // main.c host_preload: after dm_dos2_touchui_install, BEFORE dm_dos2_touch_install (it turns DM_DOS2_TOUCH_LIVE on for it)
void dm_dos2_touchui_hud_install(void) {
    int dm_dos2_touchui_on(void);
    const char *e = getenv("DM_DOS2_TOUCHUI_HUD"), *v;
    if (!e || atoi(e) <= 0) return;
    if (!dm_dos2_touchui_on()) { LOG("【红】[hud] DM_DOS2_TOUCHUI_HUD=1 needs DM_DOS2_TOUCHUI=1 with its package (the CreateUIObject hook and the movies): touch HUD off"); return; }
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_main = &g_mod[i];
    module_t *m = g_main;
    if (!m) return;
    g_dbg = (v = getenv("DM_DBG")) && atoi(v) > 0;
    static const hu_site k_sites[] = {{HU_CREATE_ALL, 4, k_hu_create_all}, {HU_DESTROY_ALL, 4, k_hu_destroy_all}, {HU_SLOT_DESTROY, 35, k_hu_slot_destroy}, {HU_DESTROY_OBJ, 8, k_hu_destroy_obj},
        {HU_MSG_SEND, 4, k_hu_msg_send}, {HU_STATE_RD, 10, k_hu_state_rd}, {HU_C_SITE, 8, k_hu_c_site}, {HU_LOOKUP, 6, k_hu_lookup}, {HU_MODE_RD, 2, k_hu_mode_rd}, {HU_VIS_RD, 2, k_hu_vis_rd}};
    for (unsigned i = 0; i < sizeof k_sites / sizeof *k_sites; i++)
        if ((uint64_t)k_sites[i].off + 4ull * k_sites[i].n > m->text_end || memcmp(m->base + k_sites[i].off, k_sites[i].want, 4ull * k_sites[i].n)) {
            LOG("【红】[hud] main+%#x differs: not DOS2 update v786432 main (build 4979b200) at the player UI set: touch HUD off, nothing changed", k_sites[i].off); return; }
    g_pc_movies = (v = getenv("DM_DOS2_TOUCHUI_HUD_PC")) && atoi(v) > 0;
    if ((v = getenv("DM_DOS2_TOUCHUI_HUD_SET")) && *v) { uint32_t ids[HU_NTYPES]; const int n = hu_list(v, ids, HU_NTYPES);
        for (int i = 0; i < HU_NTYPES; i++) { g_t[i].on = 0; for (int k = 0; k < n; k++) if (ids[k] == g_t[i].type) g_t[i].on = 1; } }
    if ((v = getenv("DM_DOS2_TOUCHUI_HUD_STATES")) && *v) { if (!strcmp(v, "any")) g_any_state = 1; else g_nstates = hu_list(v, g_states, 8); }
    if ((v = getenv("DM_DOS2_TOUCHUI_HUD_BLOCK")) && *v) g_nblock = hu_list(v, g_block, 16);
    uint32_t *t = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) { LOG("【红】[hud] mmap failed: touch HUD off"); return; }
    const struct { uint32_t off; const uint32_t *pro; hu_fn host; hu_fn *orig; } hk[2] = {{HU_CREATE_ALL, k_hu_create_all, hu_create_all_hook, &g_create_all_orig},
                                                                                         {HU_DESTROY_ALL, k_hu_destroy_all, hu_destroy_all_hook, &g_destroy_all_orig}};
    for (int i = 0; i < 2; i++) { uint32_t *s = t + 8 * i; const uint64_t rest = (uint64_t)(uintptr_t)(m->base + hk[i].off + 16);
        memcpy(s, hk[i].pro, 16); s[4] = 0x58000050 /* ldr x16,#8 */; s[5] = 0xd61f0200 /* br x16 */; s[6] = (uint32_t)rest; s[7] = (uint32_t)(rest >> 32); }
    if (mprotect(t, 0x4000, PROT_READ | PROT_EXEC)) { LOG("【红】[hud] mprotect RX failed: touch HUD off"); munmap(t, 0x4000); return; }
    __builtin___clear_cache((char *)t, (char *)(t + 16));
    for (int i = 0; i < 2; i++) *hk[i].orig = (hu_fn)(void *)(t + 8 * i);
    for (int i = 0; i < 2; i++) {
        const uint64_t to = (uint64_t)(uintptr_t)(void *)hk[i].host;
        const uint32_t jump[4] = {0x58000050, 0xd61f0200, (uint32_t)to, (uint32_t)(to >> 32)};
        if (hu_poke(m, hk[i].off, jump, 4)) {
            LOG("【红】[hud] mprotect RW failed at main+%#x: touch HUD off", hk[i].off);
            if (i == 1) hu_poke(m, hk[0].off, hk[0].pro, 4);   // take the first hook back: never one without the other
            return; }
    }
    g_on = 1;
    const char *lv = getenv("DM_DOS2_TOUCH_LIVE");
    if (!lv || !*lv) setenv("DM_DOS2_TOUCH_LIVE", "1", 1);
    char list[160]; size_t o = 0; list[0] = 0;
    for (int i = 0; i < HU_NTYPES; i++) if (g_t[i].on) o += (size_t)snprintf(list + o, sizeof list - o, "%s%#x %s", o ? ", " : "", g_t[i].type, (g_pc_movies ? g_t[i].pc : g_t[i].touch) + 4);
    LOG("[hud] DM_DOS2_TOUCHUI_HUD=1: create-all main+%#x / destroy-all main+%#x hooked; in the touch set (mode byte 0) the host makes %s per player (layers / flags of the iPad's "
        "touch branch) and destroys them before the set goes; DM_DOS2_TOUCH_LIVE=%s%s; a switch needs game state %s%#x and no dialog / loading / character creation on screen",
        HU_CREATE_ALL, HU_DESTROY_ALL, list, getenv("DM_DOS2_TOUCH_LIVE"), lv && *lv ? "" : " (set here)", g_any_state ? "any, default " : "", g_states[0]);
    if (lv && atoi(lv) != 1) LOG("【红】[hud] DM_DOS2_TOUCH_LIVE=%s: the touch HUD needs 1 (the set rebuilt in game)", lv);
}
#endif
