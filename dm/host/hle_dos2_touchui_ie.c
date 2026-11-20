// [DOS2] The iPad's touch input events for the `_t` movies: "IE Tap", "IE LongPress", "IE ShortLongPress" (DM_DOS2_TOUCHUI_IE, on with
// DM_DOS2_TOUCHUI; docs/dos2/TOUCH_UI_PLAN.md section 10). main = main.raw of update v786432, build 4979b200, offsets = main.raw.
// Phone run tu05a (15.11, pack 2s): gameMenu_t / msgBox_t were shown, mouse moves and buttons reached Iggy, and no button did anything.
// Cause (tools/dos2_swf_abc.py + a per-method listing of gameMenu_t.swf): the iPad movies do not act on mouse clicks. A button
// (LS_Classes.touchSimpleButton) takes MOUSE_DOWN / MOUSE_UP only for its pressed look; its action hangs on the custom event
// touchCustomEventType.TAP (onTapHandle -> ExternalInterface "buttonPressed"), and that event is made by the movie's own input handler:
//   MainTimeline::onEventUp(index of "IE Tap") -> touchHelpers.dispatchCustomMouseEvent(root, TAP, true) = TAP to the object under
//   stage.mouseX / mouseY; onEventDown("IE Tap") notes a tap outside the menu (isFadeTapDowned -> the up closes the menu).
// "Tap" is an INPUT EVENT of the iPad engine (touchscreen_1.json binds the tap gesture to it). 38 of the 56 `_t` movies list "IE Tap" in
// their `events` array, 18 "IE ShortLongPress", 9 "IE LongPress", 7 "IE DoubleTap", 24 "IE Touch2Fingers"; none of these names is a
// string of the Switch main, so the Switch engine never sends them. mainMenu_t lists none (its buttons take the mouse): it worked in tu04.
// How the engine wires a movie's input events (FlashPlayer, vtable main+0x29c3ba0):
//   0xd26ae0 (vt+0x160): for index 0.. `[this+0x108]->vt[0xa8 / 8](obj, "events", 5 = string, &value, index)` (between the pair
//            [[main+0x2bbb238]+0x1e8]->vt[0xc0 / 8](0x198) / vt[0xb8 / 8](0x198)); the name after "IE " is looked up in the input
//            manager's event names ([main+0x2bb8960] +0x34 buckets, +0x38 array, node +0x48 name, +0x10 id; 0xd26c18..0xd26c80); a known
//            name gets a node {next, u32 event id, u32 index} (0x10 bytes, allocator 0x83cc00(0x10, 0)) in the player's hash (+0x110
//            count, +0x114 buckets, +0x118 array; 0xd26b50..0xd26b84); an unknown name is skipped.
//   0xd23420 (vt+0x30) OnInputEvent(this, event {+0 u32 id, +4 player, +0x18 float value, +0x20 u8 down, +0x24 u32}): vt+0x38 first
//            (0xd23800: ids 1..0x66, 0x67 / 0x68 only; anything else = not handled), then the hash: found -> the movie's
//            onEventDown / onEventUp(index, player, value) at once (IggyPlayerCallMethodRS, 0xd2368c).
//   UIObject::OnInputEvent 0xae38b0: needs the object's flags (+0x14) bit 5 + player and calls [object+0x18] = the player's vt+0x30.
// What the host does:
//   - after 0xd26ae0 (prologue hook): it reads the `events` array the same way and gives the names the engine does not know a node with
//     a host id (0x7f01 Tap, 0x7f02 LongPress, 0x7f03 ShortLongPress; ids above every engine id);
//   - hle_dos2_touch.c tells it about a tap (finger down and up in place) and a long press; DM_DOS2_TOUCHUI_IE_DELAY (2) polls later -
//     the mouse move and the button of that touch have reached Iggy by then, stage.mouseX / mouseY are the finger - every visible
//     UI object (flags bit 3) with player 0's input bit (bit 5) whose player has the node gets OnInputEvent(id, down) and (id, up).
// Not done: DoubleTap, Touch2Fingers; the order between overlapping `_t` windows (every one with the node gets the event).
// Switches: DM_DOS2_TOUCHUI_IE=0 off | tap = Tap only (default: Tap + LongPress + ShortLongPress); DM_DOS2_TOUCHUI_IE_DELAY=<polls>;
//   DM_DBG=1: every movie that got nodes (type, movie, its events), every event sent with the movies that got it and what they returned.
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

enum { IE_REG = 0xd26ae0, IE_LOCK = 0xd26bb4, IE_GETVAL = 0xd26bc4, IE_UNLOCK = 0xd26be8, IE_NAMES = 0xd26c40, IE_NAME_ID = 0xd26c7c, IE_INSERT = 0xd26b50, IE_FREE = 0xd26b28,
       IE_ONINPUT = 0xd23438, IE_ONINPUT_HASH = 0xd23458, IE_UI_INPUT = 0xae38b0, IE_UI_PLAYER = 0xae38e0, IE_ALLOC = 0x83cc00, IE_FREE_FN = 0x83d3e0,
       IE_LOCK_OBJ = 0x2bbb238, IE_UIMGR = 0x2bbb230,
       FP_VALUES = 0x108, FP_EV_N = 0x110, FP_EV_NB = 0x114, FP_EV_B = 0x118, FP_LISTENER = 0xf8, UI_FLAGS = 0x14, UI_PLAYER = 0x18, UI_PATH = 0x20, UI_TYPE = 0xf0,
       MGR_OBJS = 0x10, MGR_OBJS_N = 0x1c };
static const uint32_t k_ie_reg[4] = {0xd102c3ff /* sub sp,sp,#0xb0 */, 0xa9056ffc, 0xa90667fa, 0xa9075ff8};
static const uint32_t k_ie_lock[4] = {0xf940f500 /* ldr x0,[x8,#0x1e8] */, 0xf9400008, 0xf9406108 /* ldr x8,[x8,#0xc0] */, 0xd63f0100};
static const uint32_t k_ie_getval[9] = {0xf9408660 /* ldr x0,[x19,#0x108] */, 0xf9400008, 0xf9405508 /* ldr x8,[x8,#0xa8] */, 0x910043e3 /* x3 = &value */, 0x90008981,
    0x91347021 /* "events" */, 0x528000a2 /* mov w2,#5 */, 0x2a1403e4 /* mov w4,w20: index */, 0xd63f0100};
static const uint32_t k_ie_unlock[7] = {0xf9411ea8 /* ldr x8,[x21,#0x238] */, 0x2a0003f8 /* w24 = the result */, 0xf940f500, 0xf9400008, 0xf9405d08 /* ldr x8,[x8,#0xb8] */, 0x2a1603e1, 0xd63f0100};
static const uint32_t k_ie_names[4] = {0xf94026a0 /* ldr x0,[x21,#0x48]: a known event's name */, 0xaa1903e1 /* the movie's name + 3 */, 0x943b35e2 /* compare */, 0x34000180};
static const uint32_t k_ie_name_id[2] = {0xb94012b5 /* ldr w21,[x21,#0x10]: its id */, 0x34000175};
static const uint32_t k_ie_insert[14] = {0x321c03e0 /* mov w0,#0x10 */, 0xaa1f03e1, 0x97ec582a /* bl 0x83cc00 */, 0xa9007c1f, 0xd37df2c9, 0xb9000815 /* str w21,[x0,#8]: id */,
    0xf9408e68 /* ldr x8,[x19,#0x118] */, 0xf869690a, 0xf900000a /* next */, 0xf8296900 /* bucket = node */, 0xb9411268 /* ldr w8,[x19,#0x110] */, 0x11000508, 0xb9011268, 0x14000049 /* -> str w20,[x0,#0xc]: index */};
static const uint32_t k_ie_free[2] = {0xf9401be0 /* ldr x0,[sp,#0x30]: the value's long string */, 0x97ec5a2d /* bl 0x83d3e0 */};
static const uint32_t k_ie_oninput[8] = {0xf9400008, 0xf9401d08 /* ldr x8,[x8,#0x38] */, 0xb9400038 /* ldr w24,[x1]: id */, 0x39408036 /* ldrb w22,[x1,#0x20]: down */, 0x2a1f03e2, 0xaa0103f5, 0xaa0003f4, 0xd63f0100};
static const uint32_t k_ie_oninput_hash[5] = {0xb9411688 /* ldr w8,[x20,#0x114] */, 0x1ac80b09, 0x1b08e128, 0xf9408e89 /* ldr x9,[x20,#0x118] */, 0xf8685928};
static const uint32_t k_ie_ui_input[8] = {0xf81e0ff3, 0xa9017bfd, 0x910043fd, 0xb9400428 /* ldr w8,[x1,#4]: player */, 0x71000d1f, 0x540001c8, 0xb9401409 /* ldr w9,[x0,#0x14]: flags */, 0x11001508 /* + 5 */};
static const uint32_t k_ie_ui_player[3] = {0xf9400c00 /* ldr x0,[x0,#0x18]: the player */, 0xf9400008, 0xf9401908 /* ldr x8,[x8,#0x30] */};

typedef struct { uint32_t off, n; const uint32_t *want; } ie_site;
typedef struct ie_node { struct ie_node *next; uint32_t id, index; } ie_node;
typedef uint64_t (*ie_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
enum { IE_TAP, IE_LONG, IE_SHORTLONG, IE_NEV, IE_ID0 = 0x7f01 };
static const char *const k_ev[IE_NEV] = {"IE Tap", "IE LongPress", "IE ShortLongPress"};
static module_t *g_main;
static int g_on, g_dbg, g_delay = 2, g_use[IE_NEV] = {1, 1, 1};
static ie_fn g_reg_orig;
static struct { int ev, left; } g_pend[8]; static int g_npend;
static unsigned g_nmovies, g_nsent, g_nreached;
// the dmTouch kit's input events (hle_dos2_touch_kit.c: DM_DOS2_TOUCH_KIT / DM_DOS2_TOUCH_KIT_M0; docs/dos2/TOUCH_UNIFIED.md K4): names a kit movie
// lists in `events` ("IE dmTap" ...) get a node with the kit's id; sent to ONE player (dm_dos2_touchui_ie_send_to), never to all. Empty by default.
enum { IE_XMAX = 12 };
static const char *g_x_name[IE_XMAX]; static uint32_t g_x_id[IE_XMAX]; static int g_nx;
void dm_dos2_touchui_ie_extra(const char *const *names, int n, uint32_t id0);   // before the movies load (main.c host_preload order)
void dm_dos2_touchui_ie_extra(const char *const *names, int n, uint32_t id0) {
    for (int i = 0; i < n && g_nx < IE_XMAX; i++) { g_x_name[g_nx] = names[i]; g_x_id[g_nx] = id0 + (uint32_t)i; g_nx++; }
}
int dm_dos2_touchui_ie_on(void);

static const char *ie_movie(const uint8_t *fp, uint32_t *type, char *buf, size_t n) {
    const uint8_t *ui = *(const uint8_t *const volatile *)(fp + FP_LISTENER);
    buf[0] = 0; *type = 0xffff;
    if (!ui) return buf;
    *type = *(const volatile uint32_t *)(ui + UI_TYPE);
    if (ui[UI_PATH] & 1) { uint32_t len; const char *p; memcpy(&len, ui + UI_PATH + 4, 4); memcpy(&p, ui + UI_PATH + 8, 8);
        if (p && len >= 5 && len < 300 && !((uintptr_t)p >> 48)) { uint32_t s = len; while (s && p[s - 1] != '/') s--; snprintf(buf, n, "%.*s", (int)(len - s), p + s); } }
    return buf;
}
static ie_node *ie_find(const uint8_t *fp, uint32_t id) {
    const uint32_t nb = *(const volatile uint32_t *)(fp + FP_EV_NB); ie_node **bk = *(ie_node **const volatile *)(fp + FP_EV_B);
    if (!nb || !bk || nb > 0x10000) return NULL;
    int guard = 0;
    for (ie_node *nd = bk[id % nb]; nd && guard++ < 256; nd = nd->next) if (nd->id == id) return nd;
    return NULL;
}
void dm_dos2_touchui_movie_of(const void *player, uint32_t *type, char *buf, size_t n);   // hle_dos2_touch.c debug: which movie a FlashPlayer plays
void dm_dos2_touchui_movie_of(const void *player, uint32_t *type, char *buf, size_t n) { if (player && n) ie_movie(player, type, buf, n); else if (n) { buf[0] = 0; *type = 0xffff; } }
// after the engine has read the movie's `events`: the same walk again, for the names it does not know
static uint64_t ie_reg_hook(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    const uint64_t v = g_reg_orig(a0, a1, a2, a3, a4, a5);
    uint8_t *fp = (uint8_t *)(uintptr_t)a0;
    void *vals = fp ? *(void *volatile *)(fp + FP_VALUES) : NULL;
    uint8_t *lk0 = *(uint8_t *volatile *)(g_main->base + IE_LOCK_OBJ);
    void *lk = lk0 ? *(void *volatile *)(lk0 + 0x1e8) : NULL;
    const uint32_t nb = fp ? *(volatile uint32_t *)(fp + FP_EV_NB) : 0; ie_node **bk = fp ? *(ie_node **volatile *)(fp + FP_EV_B) : NULL;
    if (!g_on || !vals || !lk) return v;
    char all[256]; size_t ao = 0; int added = 0, want = 0; all[0] = 0;
    for (uint32_t idx = 0; idx < 48; idx++) {
        uint8_t val[0x40]; memset(val, 0, sizeof val); val[0] = 1;                                                    // as main+0xd26b88: type 1, the rest 0
        ((void (*)(void *, uint32_t))(*(void ***)lk)[0xc0 / 8])(lk, 0x198);                                              // main+0xd26bb4
        const uint64_t r = ((uint64_t (*)(void *, const char *, uint32_t, void *, uint32_t))(*(void ***)vals)[0xa8 / 8])(vals, "events", 5, val, idx);   // main+0xd26bc4
        ((void (*)(void *, uint32_t))(*(void ***)lk)[0xb8 / 8])(lk, 0x198);                                              // main+0xd26be8
        char name[40]; name[0] = 0;
        if (r & 1) {
            const char *p; uint32_t len;
            if (val[8] & 1) { memcpy(&len, val + 0xc, 4); memcpy(&p, val + 0x10, 8); } else { len = val[8] >> 1; p = (const char *)val + 9; }
            if (p && len < sizeof name) { memcpy(name, p, len); name[len] = 0; }
        }
        if (val[0x18] & 1) { void *q; memcpy(&q, val + 0x20, 8); if (q) ((void (*)(void *))(void *)(g_main->base + IE_FREE_FN))(q); }   // main+0xd26b28
        if (val[8] & 1) { void *q; memcpy(&q, val + 0x10, 8); if (q) ((void (*)(void *))(void *)(g_main->base + IE_FREE_FN))(q); }      // main+0xd26b38
        if (!(r & 1)) break;
        if (ao + 48 < sizeof all) ao += (size_t)snprintf(all + ao, sizeof all - ao, "%s%s", idx ? ", " : "", name[0] ? name + (strncmp(name, "IE ", 3) ? 0 : 3) : "?");
        for (int e = 0; e < IE_NEV + g_nx; e++) {   // the iPad's three, then the dmTouch kit's names (dm_dos2_touchui_ie_extra; none by default)
            const char *want_name = e < IE_NEV ? k_ev[e] : g_x_name[e - IE_NEV];
            const uint32_t want_id = e < IE_NEV ? IE_ID0 + (uint32_t)e : g_x_id[e - IE_NEV];
            if ((e < IE_NEV && !g_use[e]) || strcmp(name, want_name)) continue;
            want++;
            ie_node *nd = ie_find(fp, want_id);
            if (nd) { nd->index = idx; continue; }
            if (!nb || !bk || nb > 0x10000) continue;   // no table (the engine made none): nothing to hang the node on
            nd = ((ie_node *(*)(uint64_t, void *))(void *)(g_main->base + IE_ALLOC))(0x10, NULL);                         // main+0xd26b50..0xd26b84
            if (!nd) continue;
            nd->id = want_id; nd->index = idx; nd->next = bk[nd->id % nb]; bk[nd->id % nb] = nd;
            *(volatile uint32_t *)(fp + FP_EV_N) += 1; added++;
        }
    }
    if (want) {
        uint32_t type; char mv[48]; ie_movie(fp, &type, mv, sizeof mv); g_nmovies++;
        if (g_dbg || added < want) LOG("%s[ie] type %#x %s: events [%s]: %d touch event(s) wired%s", added < want && !nb ? "【红】" : "", type, mv, all, added,
                                       added < want ? (nb ? " (the rest were there already)" : " - the player has no event table") : "");
    }
    return v;
}
static void ie_send(int e, int down) {
    uint8_t *mgr = *(uint8_t *volatile *)(g_main->base + IE_UIMGR);
    if (!mgr) return;
    const uint32_t n = *(volatile uint32_t *)(mgr + MGR_OBJS_N); uint8_t **objs = *(uint8_t **volatile *)(mgr + MGR_OBJS);
    if (!objs || n > 4096) return;
    char who[400]; size_t wo = 0; int got = 0; who[0] = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *o = objs[i];
        if (!o || (o[UI_FLAGS] & 0x28) != 0x28) continue;   // visible (bit 3) and player 0's input (bit 5), as main+0xae38c8 / 0x10c0d34
        uint8_t *fp = *(uint8_t *volatile *)(o + UI_PLAYER);
        if (!fp || !ie_find(fp, IE_ID0 + (uint32_t)e)) continue;
        uint8_t ev[0x80]; memset(ev, 0, sizeof ev);
        const uint32_t id = IE_ID0 + (uint32_t)e; const float one = 1.0f;
        memcpy(ev, &id, 4); memcpy(ev + 0x18, &one, 4); ev[0x20] = (uint8_t)down;
        const uint64_t r = ((uint64_t (*)(void *, void *))(*(void ***)(void *)fp)[0x30 / 8])(fp, ev);
        got++;
        if (g_dbg && wo + 64 < sizeof who) { uint32_t type; char mv[40]; ie_movie(fp, &type, mv, sizeof mv); wo += (size_t)snprintf(who + wo, sizeof who - wo, " %s(%#x)=%u", mv, type, (unsigned)r & 0xff); }
    }
    g_nsent++; g_nreached += (unsigned)got;
    if (g_dbg) LOG("[ie] %s %s -> %d movie(s):%s", k_ev[e], down ? "down" : "up", got, got ? who : " none (no visible touch movie lists it)");
}
// hle_dos2_touch_kit.c (game thread, the pad poll): one input event to ONE movie (the kit's addressed delivery, K4). -1 = the player has no node
// for that id (its `events` lacks the name), -2 = the hook is off; else the low byte of FlashPlayer::OnInputEvent (1 = the movie's
// onEventDown / onEventUp ran through the node: main+0xd23458 found it)
int dm_dos2_touchui_ie_send_to(const void *player, uint32_t id, int down, float value);
int dm_dos2_touchui_ie_send_to(const void *player, uint32_t id, int down, float value) {
    if (!g_on) return -2;
    uint8_t *fp = (uint8_t *)(uintptr_t)player;
    if (!fp || !ie_find(fp, id)) return -1;
    uint8_t ev[0x80]; memset(ev, 0, sizeof ev);
    memcpy(ev, &id, 4); memcpy(ev + 0x18, &value, 4); ev[0x20] = (uint8_t)(down != 0);
    const uint64_t r = ((uint64_t (*)(void *, void *))(*(void ***)(void *)fp)[0x30 / 8])(fp, ev);
    return (int)(r & 0xff);
}
int dm_dos2_touchui_ie_on(void) { return g_on; }
// hle_dos2_touch.c: a gesture was recognised (game thread, in the pad poll). kind 0 = tap, 1 = long press
void dm_dos2_touchui_ie_gesture(int kind);
void dm_dos2_touchui_ie_gesture(int kind) {
    if (!g_on) return;
    const int evs[2] = {kind ? IE_SHORTLONG : IE_TAP, kind ? IE_LONG : -1};
    for (int i = 0; i < 2; i++) if (evs[i] >= 0 && g_use[evs[i]] && g_npend < 8) { g_pend[g_npend].ev = evs[i]; g_pend[g_npend].left = g_delay; g_npend++; }
}
// hle_dos2_touch.c: once per pad poll, before the new touches are turned into records
void dm_dos2_touchui_ie_poll(void);
void dm_dos2_touchui_ie_poll(void) {
    if (!g_on || !g_npend) return;
    // the mouse of that touch reached the movies only when the listener filter was open ([input manager +0x170], hle_dos2_touch.c KBM) or in the
    // PC / touch mode (mode byte 0): otherwise stage.mouseX / mouseY are stale and a Tap would press whatever lies there
    const uint8_t *im = *(const uint8_t *const volatile *)(g_main->base + 0x2bb8960);
    const int mouse = (im && im[0x170]) || !*(const volatile uint8_t *)(g_main->base + 0x2bb8918);
    int k = 0;
    for (int i = 0; i < g_npend; i++) {
        if (--g_pend[i].left > 0) { g_pend[k++] = g_pend[i]; continue; }
        if (!mouse) { if (g_dbg) LOG("[ie] %s not sent: mouse records do not reach the movies now ([mgr+0x170] 0, controller mode)", k_ev[g_pend[i].ev]); continue; }
        ie_send(g_pend[i].ev, 1); ie_send(g_pend[i].ev, 0);
    }
    g_npend = k;
}
static int ie_poke(module_t *m, uint32_t off, const uint32_t *w, int n) {
    uint32_t *at = (uint32_t *)(void *)(m->base + off);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF);
    const size_t len = (size_t)(((uintptr_t)(at + n) - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    memcpy(at, w, 4 * (size_t)n);
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[ie] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)(at + n));
    return 0;
}
void dm_dos2_touchui_ie_install(void);   // main.c host_preload, after dm_dos2_touchui_install
void dm_dos2_touchui_ie_install(void) {
    int dm_dos2_touchui_on(void);
    const char *e = getenv("DM_DOS2_TOUCHUI_IE"), *v;
    if (!dm_dos2_touchui_on() || (e && *e == '0')) return;
    if (e && !strcmp(e, "tap")) g_use[IE_LONG] = g_use[IE_SHORTLONG] = 0;
    if ((v = getenv("DM_DOS2_TOUCHUI_IE_DELAY")) && atoi(v) >= 1 && atoi(v) <= 30) g_delay = atoi(v);
    g_dbg = (v = getenv("DM_DBG")) && atoi(v) > 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_main = &g_mod[i];
    module_t *m = g_main;
    if (!m) return;
    static const ie_site k_sites[] = {{IE_REG, 4, k_ie_reg}, {IE_LOCK, 4, k_ie_lock}, {IE_GETVAL, 9, k_ie_getval}, {IE_UNLOCK, 7, k_ie_unlock}, {IE_NAMES, 4, k_ie_names}, {IE_NAME_ID, 2, k_ie_name_id},
        {IE_INSERT, 14, k_ie_insert}, {IE_FREE, 2, k_ie_free}, {IE_ONINPUT, 8, k_ie_oninput}, {IE_ONINPUT_HASH, 5, k_ie_oninput_hash}, {IE_UI_INPUT, 8, k_ie_ui_input}, {IE_UI_PLAYER, 3, k_ie_ui_player}};
    for (unsigned i = 0; i < sizeof k_sites / sizeof *k_sites; i++)
        if ((uint64_t)k_sites[i].off + 4ull * k_sites[i].n > m->text_end || memcmp(m->base + k_sites[i].off, k_sites[i].want, 4ull * k_sites[i].n)) {
            LOG("【红】[ie] main+%#x differs: not DOS2 update v786432 main (build 4979b200) at the Flash player's input events: the touch movies get no Tap (their buttons stay dead)", k_sites[i].off); return; }
    uint32_t *t = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) { LOG("【红】[ie] mmap failed: no touch input events"); return; }
    const uint64_t rest = (uint64_t)(uintptr_t)(m->base + IE_REG + 16);
    memcpy(t, k_ie_reg, 16); t[4] = 0x58000050 /* ldr x16,#8 */; t[5] = 0xd61f0200 /* br x16 */; t[6] = (uint32_t)rest; t[7] = (uint32_t)(rest >> 32);
    if (mprotect(t, 0x4000, PROT_READ | PROT_EXEC)) { LOG("【红】[ie] mprotect RX failed: no touch input events"); munmap(t, 0x4000); return; }
    __builtin___clear_cache((char *)t, (char *)(t + 8));
    g_reg_orig = (ie_fn)(void *)t;
    const uint64_t to = (uint64_t)(uintptr_t)(void *)ie_reg_hook;
    const uint32_t jump[4] = {0x58000050, 0xd61f0200, (uint32_t)to, (uint32_t)(to >> 32)};
    if (ie_poke(m, IE_REG, jump, 4)) { LOG("【红】[ie] mprotect RW failed: no touch input events"); g_reg_orig = NULL; return; }
    g_on = 1;
    LOG("[ie] touch input events for the iPad movies: main+%#x hooked (a movie's `events`); a tap sends \"IE Tap\"%s to every visible touch movie %d poll(s) after the click "
        "(FlashPlayer::OnInputEvent, host ids %#x..); DM_DOS2_TOUCHUI_IE=0 turns it off", IE_REG, g_use[IE_LONG] ? ", a long press \"IE ShortLongPress\" + \"IE LongPress\"" : "", g_delay, IE_ID0);
}
#endif
