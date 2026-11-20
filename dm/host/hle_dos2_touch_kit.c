// [DOS2] dmTouch kit, host side (docs/dos2/TOUCH_UNIFIED.md: section 3.4 = the contract K1..K9, section 5 = steps M0 / M1; the pure rules are
// hle_dos2_touch_kit.h with the host test dm/host/tests/dos2_touch_kit_test.c). main = main.raw of update v786432, build 4979b200.
// Static only: nothing of this file was run on a phone when it was written.
//
// DM_DOS2_TOUCH_KIT=1 (default 0 = nothing below runs, the old paths are byte for byte the old ones in both UI modes):
//   - the kit movies (tools/dos2_touchkit.py: a movie with the dmTouch code) report their elements: ExternalInterface "dmKRects"("sw,sh|id,x,y,w,h,flags;...")
//     -> the rect store here (window-normalised through the movie's FlashPlayer rect, as hle_dos2_touch.c dmHudRects; life 1.5 s);
//   - a finger that lands: K1 = dk_target over the UI objects top-down (the UI manager's list [uimgr+0xf8], last = top, as main+0xae83e0
//     walks it). A kit rect hit = the kit's finger (hle_dos2_touch.c state G_KIT): no world probe, no allow list; K2 = the mouse goes to that
//     object only (hle_dos2_touchui_route.c g_kit_obj) and only as the native code needs it (move = hover; a click only on a 't' element, a
//     press at once only on a 'd' element); K3 = one recogniser (dk_rec_*: tap, double tap 300 ms, short long press 350, long press 600,
//     hold, drag, one-finger scroll) the same in both modes; K4 = each verb is ONE input event "IE dm*" to that movie only
//     (hle_dos2_touchui_ie.c dm_dos2_touchui_ie_send_to), DM_DOS2_TOUCHUI_IE_DELAY polls after the mouse of the touch (its stage mouse is the
//     finger by then); the first report of a movie gets "IE dmSet" act = 1 (with KIT=0 a kit movie only traces), the least hit size, the mode;
//   - K5 ExternalInterface "dmAct"(code, a, b): one host verb, the mode's way (dk_act below; the old names dmPanel / dmSide / dmPadBtn / dmKey stay
//     as they were and are what dmAct calls); K7 = one accept guard (route hook, both modes: UIAccept to the kit target within 600 ms of the
//     touch press, unless the pad's A or a host pulse made it); K8 = dmAct from an object that is not the target of the gesture in flight is
//     refused; K9 = no world finger while a kit movie reports a window background (or is modal): dm_dos2_kit_window_up.
//   No movie of the default packs has the kit (M1 = the core only): with KIT=1 and the default pack every finger stays on the old path.
//
// DM_DOS2_TOUCH_KIT_M0=<bits> (default 0; "all" = 7): the one-run experiment M0 of TOUCH_UNIFIED section 5, log only, independent of KIT:
//   1  addressed "IE dmTap" to our type 0x8d movie (dmColumn_d in hybrid, dmSide_p in touch) on every still release of a finger, both modes:
//      the M0 pack's 0x8d movies (tools/dos2_touchkit.py m0) list the kit's names in `events` and trace onEventDown / onEventUp with
//      ExternalInterface "dmKitTrace". Lines: "[kit] M0-1 sent: ..." (host: OnInputEvent's answer) and "[kit] M0-1 REACHED: ..." (the movie).
//   2  the UI manager's per-object dispatch main+0xaea7b0 called directly for ONE object - the top visible optionsSettings (types 0x58..0x5b)
//      3 s after it came up - with a built UICancel (down, then up; pass 1, then pass 2 as main+0xae83e0). Lines: "[kit] M0-2 sent: ..." and
//      "[kit] M0-2 after: ... CLOSED / still open".
//   4  the path of a host pad pulse (dmPadBtn): from the pulse every non-mouse input event that reaches a UI object for 800 ms is logged with
//      the object's answer (route hook); in touch the pulse is the keyboard twin (dt_pad_key: A return, B escape ...) and its key record is
//      logged too. Lines: "[kit] M0-3 pulse: ...", "[kit] M0-3 key ...", "[kit] M0-3 +N ms: input event ...".
// Other switches: DM_DOS2_TOUCH_KIT_LEAST=<mm> least hit size (default 8 mm through DM_DOS2_TOUCH_MM_PER_1080; 7 % of the height without it),
//   DM_DBG=1 every kit finger / verb / delivery / report.
#ifdef DM_GAME_DOS2
#include "hle.h"
#include "hle_dos2_touch_kit.h"
#include "hle_dos2_touchui_ei.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { DK_UIMGR = 0x2bbb230, DK_MODE_BYTE = 0x2bb8918, DK_MGR_LIST = 0xf8, DK_MGR_LISTN = 0x100, DK_UIM_PB = 0x158, DK_UIM_PB_STRIDE = 0x20,
       DK_OBJ_FLAGS = 0x14, DK_OBJ_PLAYER = 0x18, DK_OBJ_TYPE = 0xf0, DK_FP_RECT = 0x10,
       DK_EV_RD = 0xae8428, DK_LIST_RD = 0xae844c, DK_T_DMSIDE = 0x8d };
// main+0xae8428 (UIManager::OnInputEvent): the event's id / player / down and the per-player block x22 = uimgr + player * 0x20
static const uint32_t k_dk_ev_rd[4] = {0x294066b8 /* ldp w24,w25,[x21]: id, player */, 0x394082a8 /* ldrb w8,[x21,#0x20]: down */, 0x93407f39 /* sxtw x25,w25 */,
                                       0x8b191676 /* add x22,x19,x25,lsl #5 */};
// main+0xae844c: the object count = bits 24..47 of [uimgr+0x100]; x20 = that block + 0x158 = the x4 of every 0xaea7b0 call
static const uint32_t k_dk_list_rd[5] = {0xf9408268 /* ldr x8,[x19,#0x100] */, 0x910562d4 /* add x20,x22,#0x158 */, 0xf2685d1f /* tst x8,#0xffffff000000 */, 0x54000420,
                                         0xd358bd17 /* ubfx x23,x8,#24,#24 */};
typedef struct { uint32_t off, n; const uint32_t *want; } dk_site;

static module_t *g_main;
static int g_on, g_m0, g_dbg, g_delay = 2;
static float g_least = 0.07f;   // least hit size, fraction of the window height
static uint64_t dk_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000ull + (uint64_t)t.tv_nsec / 1000000ull; }
static int dk_touch_mode(void) { return g_main && !*(const volatile uint8_t *)(g_main->base + DK_MODE_BYTE); }   // mode byte 0 = UI mode touch (PC classes)
void dm_dos2_touchui_movie_of(const void *player, uint32_t *type, char *buf, size_t n);   // hle_dos2_touchui_ie.c
static const char *dk_movie(const void *player, char *buf, size_t n) { uint32_t ty; dm_dos2_touchui_movie_of(player, &ty, buf, n); if (!buf[0]) snprintf(buf, n, "?"); return buf; }
int dm_dos2_touchui_ie_send_to(const void *player, uint32_t id, int down, float value);   // hle_dos2_touchui_ie.c
void dm_dos2_touchui_ie_extra(const char *const *names, int n, uint32_t id0);
int dm_dos2_touchui_ie_on(void);
void dm_dos2_route_kit_target(const void *obj);   // hle_dos2_touchui_route.c
void dm_dos2_route_kit_hooks(int (*accept)(const void *), void (*note)(const void *, uint32_t, uint32_t, int, int, unsigned, unsigned));
uint32_t dm_dos2_route_event_id(const char *name);
const char *dm_dos2_route_event_name(uint32_t id);
typedef uint64_t (*dm_route_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
dm_route_fn dm_dos2_route_dispatch_orig(void);
int dm_dos2_route_on(void);
uint64_t dm_dos2_touch_a_ms(void);       // hle_dos2_touch.c: the npad's A last down
uint64_t dm_dos2_touch_host_ms(void);    // hle_dos2_touch.c: the host's last key / pad pulse went down
float dm_dos2_touch_win_h(void); float dm_dos2_touch_win_w(void);
int dm_dos2_touchui_ei_host(const char *name, void *player, void *ui, unsigned n, const dm_ei_val *args);   // hle_dos2_touchui_ei.c

// ---- the UI objects, top first ----
enum { DK_OMAX = 160 };
static int dk_objects(dk_obj *o, uint8_t **ui, int max) {
    const uint8_t *m = g_main ? *(const uint8_t *const volatile *)(g_main->base + DK_UIMGR) : NULL;
    if (!m) return 0;
    uint8_t *const *list = *(uint8_t *const *const volatile *)(m + DK_MGR_LIST);
    const uint64_t n = (*(const volatile uint64_t *)(m + DK_MGR_LISTN) >> 24) & 0xffffff;
    if (!list || !n || n > 0x10000) return 0;
    int k = 0;
    for (uint64_t i = n; i-- > 0 && k < max;) {
        uint8_t *x = list[i]; if (!x) continue;
        o[k].flags = *(const volatile uint32_t *)(x + DK_OBJ_FLAGS); o[k].type = *(const volatile uint32_t *)(x + DK_OBJ_TYPE);
        o[k].player = *(const void *const volatile *)(x + DK_OBJ_PLAYER); ui[k] = x; k++;
    }
    return k;
}
static uint8_t *dk_ui_of(const void *player) {   // the UI object a FlashPlayer belongs to now (NULL = gone)
    dk_obj o[DK_OMAX]; uint8_t *ui[DK_OMAX]; const int n = dk_objects(o, ui, DK_OMAX);
    for (int i = 0; i < n; i++) if (o[i].player == player) return ui[i];
    return NULL;
}

// ---- the rect store (ExternalInterface "dmKRects") ----
enum { DK_RMAX = 160, DK_TTL = 1500, DK_PMAX = 16 };
static dk_rect g_r[DK_RMAX]; static int g_nr;
static struct { const void *player; float k; int told; uint64_t t; } g_pl[DK_PMAX];
static pthread_mutex_t g_rmu = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_n_rep;

// ---- the delivery queue (game thread: kit fingers, M0, dmSet) ----
typedef struct { const void *player; uint32_t id; float value; int dir /* 2 down + up, 1 down, 0 up */; int left, m0; char what[24]; } dk_q;
enum { DK_QMAX = 32 };
static dk_q g_q[DK_QMAX]; static int g_nq;
static unsigned g_n_sent, g_n_nonode;
static void dk_queue(const void *player, uint32_t id, float value, int dir, int delay, int m0, const char *what) {
    if (g_nq >= DK_QMAX || !player) return;
    dk_q *q = &g_q[g_nq++]; q->player = player; q->id = id; q->value = value; q->dir = dir; q->left = delay; q->m0 = m0;
    snprintf(q->what, sizeof q->what, "%s", what ? what : "");
}
static void dk_deliver(dk_q *q) {
    char mv[48]; uint32_t ty = 0; dm_dos2_touchui_movie_of(q->player, &ty, mv, sizeof mv);
    const char *nm = q->id >= DK_EV_TAP && q->id < DK_EV_END ? dk_ev_name[q->id - DK_EV_TAP] : "?";
    if (!dk_ui_of(q->player)) { LOG("[kit] %s%s not sent: the movie is gone", q->m0 ? "M0-1 " : "", nm); return; }
    const int rd = q->dir >= 1 ? dm_dos2_touchui_ie_send_to(q->player, q->id, 1, q->value) : 0;
    const int ru = q->dir != 1 ? dm_dos2_touchui_ie_send_to(q->player, q->id, 0, q->value) : 0;
    g_n_sent++;
    if (rd == -1 || ru == -1) g_n_nonode++;
    if (q->m0) {
        static unsigned n;
        if (n++ < 60) {
            if (rd == -1 || ru == -1) LOG("【红】[kit] M0-1 sent: \"%s\" -> type %#x %s (player %p): NO NODE - its `events` lacks the name (not the M0 / kit pack, or "
                                       "the movie registered its events before the host knew the name): the event cannot reach it", nm, ty, mv[0] ? mv : "?", q->player);
            else if (rd == -2) LOG("【红】[kit] M0-1 sent: the input event hook (hle_dos2_touchui_ie.c) is off: nothing sent");
            else LOG("[kit] M0-1 sent: \"%s\" (id %#x) -> type %#x %s (player %p), mode %s: FlashPlayer::OnInputEvent down -> %d, up -> %d "
                     "(the node was found; \"M0-1 REACHED\" from the movie = its onEventUp ran)", nm, q->id, ty, mv[0] ? mv : "?", q->player, dk_touch_mode() ? "touch" : "hybrid", rd, ru);
        }
    } else if (g_dbg) LOG("[kit] %s %s%s -> type %#x %s value %.1f: down %d, up %d", nm, q->dir == 2 ? "down + up" : q->dir ? "down" : "up", q->what[0] ? q->what : "", ty, mv, (double)q->value, rd, ru);
}

// ---- K1..K4: the kit's finger ----
static struct {
    int on, block; uint8_t *ui; const void *player; uint32_t type, flags; char id[24]; float k; uint64_t t, touch_ms; int pressed, clear_in;
    dk_rec rec; float x, y;
} g_f;
static dk_cfg g_cfg;
static unsigned g_n_fing, g_n_block, g_n_verb[DK_V_SCROLL + 1];
static const char *const k_verb[DK_V_SCROLL + 1] = {"-", "tap", "double tap", "short long press", "long press", "hold down", "hold up", "drag start", "drag end", "scroll"};
static void dk_target_clear(void) { dm_dos2_route_kit_target(NULL); g_f.clear_in = 0; }
static void dk_verb(int v, float px) {
    if (v <= DK_V_NONE || v > DK_V_SCROLL) return;
    g_n_verb[v]++;
    const uint32_t ev = dk_verb_event(v);
    if (g_dbg) { char mv[48]; LOG("[kit] %s on %s element %s (flags %#x)%s", k_verb[v], dk_movie(g_f.player, mv, sizeof mv), g_f.id, g_f.flags, ev ? "" : " (no event: the native mouse only)"); }
    if (!ev || g_f.block) return;
    float value = 1.f;
    if (v == DK_V_SCROLL) value = g_f.k > 0 ? px / g_f.k : px;   // stage px
    const int dir = v == DK_V_HOLD_DOWN ? 1 : v == DK_V_HOLD_UP ? 0 : 2;
    // tap / double tap wait for the mouse of this touch (the movie's stage mouse is the finger then); the timed verbs come with the mouse there already
    dk_queue(g_f.player, ev, value, dir, (v == DK_V_TAP || v == DK_V_DTAP) ? g_delay : 0, 0, g_f.id);
}
int dm_dos2_kit_on(void);
int dm_dos2_kit_on(void) { return g_on; }
// a finger lands (window px). 1 = the kit's (*mouse = DK_DO_* bits to do at x, y now), 0 = the old path
int dm_dos2_kit_down(float x, float y, uint64_t t, unsigned *mouse);
int dm_dos2_kit_down(float x, float y, uint64_t t, unsigned *mouse) {
    *mouse = 0;
    if (!g_on) return 0;
    const float ww = dm_dos2_touch_win_w(), wh = dm_dos2_touch_win_h();
    if (!(ww > 0) || !(wh > 0)) return 0;
    dk_obj o[DK_OMAX]; uint8_t *ui[DK_OMAX]; const int n = dk_objects(o, ui, DK_OMAX);
    int oi = -1, ri = -1, res;
    pthread_mutex_lock(&g_rmu);
    res = dk_target(o, n, g_r, g_nr, dk_ms(), DK_TTL, x / ww, y / wh, g_least * wh / ww, g_least, &oi, &ri);
    dk_rect hit; if (res == DK_T_KIT) hit = g_r[ri]; else memset(&hit, 0, sizeof hit);
    float k = 0; for (int i = 0; i < DK_PMAX; i++) if (oi >= 0 && g_pl[i].player == o[oi].player) k = g_pl[i].k;
    pthread_mutex_unlock(&g_rmu);
    if (res == DK_T_LEGACY) { if (g_f.clear_in) dk_target_clear(); g_f.on = 0; return 0; }
    memset(&g_f.id, 0, sizeof g_f.id);
    g_f.on = 1; g_f.block = res == DK_T_BLOCK; g_f.ui = ui[oi]; g_f.player = o[oi].player; g_f.type = o[oi].type; g_f.flags = hit.flags; g_f.k = k;
    snprintf(g_f.id, sizeof g_f.id, "%s", res == DK_T_KIT ? hit.id : "(outside)"); g_f.t = g_f.touch_ms = dk_ms(); g_f.pressed = 0; g_f.clear_in = 0; g_f.x = x; g_f.y = y;
    g_cfg = dk_cfg_ipad(wh);
    dk_rec_down(&g_f.rec, g_f.flags, x, y, t);
    g_n_fing++;
    if (g_f.block) {
        g_n_block++; dk_target_clear();
        { static unsigned nl; if (g_dbg || nl++ < 20) { char mv[48]; LOG("[kit] finger at %.0f,%.0f: outside the modal kit window type %#x %s: nobody gets it (not the world either)", (double)x, (double)y, g_f.type, dk_movie(g_f.player, mv, sizeof mv)); } }
        return 1;
    }
    dm_dos2_route_kit_target(g_f.ui);   // K2
    *mouse = DK_DO_MOVE;
    if (g_f.flags & DK_F_DRAG) { *mouse |= DK_DO_PRESS; g_f.pressed = 1; }   // a native drag starts at once (cells, sliders, the doll)
    { static unsigned nl; if (g_dbg || nl++ < 30) { char mv[48]; LOG("[kit] finger at %.0f,%.0f -> type %#x %s, %s %s (flags %#x)%s", (double)x, (double)y, g_f.type, dk_movie(g_f.player, mv, sizeof mv),
        (g_f.flags & DK_F_EL) ? "element" : "window background", g_f.id, g_f.flags, g_f.pressed ? ": native drag, pressed" : ""); } }
    return 1;
}
void dm_dos2_kit_move(float x, float y, unsigned *mouse);
void dm_dos2_kit_move(float x, float y, unsigned *mouse) {
    *mouse = 0;
    if (!g_f.on || g_f.block) return;
    float px = 0; const int v = dk_rec_move(&g_f.rec, &g_cfg, x, y, &px);
    g_f.x = x; g_f.y = y;
    if (v != DK_V_SCROLL) *mouse = DK_DO_MOVE;   // hover; a native drag follows (pressed at the landing)
    if (v != DK_V_NONE) dk_verb(v, px);
}
// the finger is lifted (cancel = the system took it): *mouse at x, y
void dm_dos2_kit_up(float x, float y, uint64_t t, int cancel, unsigned *mouse);
void dm_dos2_kit_up(float x, float y, uint64_t t, int cancel, unsigned *mouse) {
    *mouse = 0;
    if (!g_f.on) return;
    g_f.on = 0;
    if (g_f.block) return;
    const int v = cancel ? DK_V_NONE : dk_rec_up(&g_f.rec, &g_cfg, x, y, t);
    unsigned m = dk_verb_mouse(v, g_f.flags);
    if (g_f.pressed && !(m & DK_DO_RELEASE)) m |= DK_DO_MOVE | DK_DO_RELEASE;   // a native drag ends whatever the verb
    if (cancel && g_f.pressed) m = DK_DO_RELEASE;
    *mouse = m;
    if (v == DK_V_DRAG_END && !(g_f.flags & DK_F_DRAG)) {}   // a drag on a kit-only element: nothing (K2: no press was sent)
    else dk_verb(v, 0);
    g_f.clear_in = 4;   // the mouse records of this release are read by the engine's next input update: keep the target for a few polls
}
// a second finger lands on a kit finger: the gesture is the target's wheel (hle_dos2_touch.c G_TWO); the target keeps the mouse until all are up
void dm_dos2_kit_two(unsigned *mouse);
void dm_dos2_kit_two(unsigned *mouse) {
    *mouse = 0;
    if (!g_f.on) return;
    g_f.on = 0; g_f.clear_in = 0;   // the target keeps the mouse until every finger is up (dm_dos2_kit_end)
    if (g_f.pressed) { *mouse = DK_DO_RELEASE; g_f.pressed = 0; }
    if (g_dbg) LOG("[kit] a second finger: two-finger gesture on %s (the wheel, mouse to the target only)", g_f.id);
}
void dm_dos2_kit_end(void);
void dm_dos2_kit_end(void) { if (g_on) g_f.clear_in = 4; }
int dm_dos2_kit_window_up(void);   // K9 (hle_dos2_touch.c dt_world_now)
int dm_dos2_kit_window_up(void) {
    if (!g_on) return 0;
    dk_obj o[DK_OMAX]; uint8_t *ui[DK_OMAX]; const int n = dk_objects(o, ui, DK_OMAX);
    pthread_mutex_lock(&g_rmu);
    const int up = dk_window_up(o, n, g_r, g_nr, dk_ms(), DK_TTL);
    pthread_mutex_unlock(&g_rmu);
    return up;
}
// K7 (route hook, game thread): drop this UIAccept to obj?
static int dk_accept(const void *obj) {
    if (!g_on || !obj || obj != g_f.ui) return 0;
    const int drop = dk_accept_drop(dk_ms(), g_f.touch_ms, dm_dos2_touch_a_ms(), dm_dos2_touch_host_ms(), 600);
    if (drop) { static unsigned n; if (g_dbg || n++ < 20) LOG("[kit] UIAccept to the kit target type %#x dropped: it came with the touch (no A on the pad, no host pulse)", g_f.type); }
    return drop;
}

// ---- K5: dmAct(code, a, b) ----
static int dk_ei_host(const char *name, void *player, void *ui, double a) {
    dm_ei_val v; memset(&v, 0, sizeof v); v.type = 4; v.d = a;
    const int r = dm_dos2_touchui_ei_host(name, player, ui, 1, &v);
    if (r < 0) { static unsigned n; if (n++ < 20) LOG("【红】[kit] dmAct -> \"%s\"(%g): no host handler of that name in this mode (its module is off)", name, a); }
    return r;
}
static int dk_ei_act(void *ctx, void *player, void *ui, const char *name, unsigned n, const dm_ei_val *a) {
    (void)ctx; (void)name;
    const int code = n >= 1 && a[0].type == 4 ? (int)a[0].d : -1, x = n >= 2 && a[1].type == 4 ? (int)a[1].d : 0;
    const int touch = dk_touch_mode();
    if (!dk_may_act(ui, g_f.ui, dk_ms(), g_f.t, 2000)) {   // K8
        static unsigned nl; if (nl++ < 20) LOG("[kit] dmAct(%d, %d) from type %#x refused: the gesture in flight is another object's (type %#x)", code, x, ui ? *(const uint32_t *)((const uint8_t *)ui + DK_OBJ_TYPE) : 0, g_f.type);
        return 1;
    }
    { static unsigned nl; if (g_dbg || nl++ < 30) LOG("[kit] dmAct(%d %s, %d) in %s", code, code > 0 && code < DK_A_N ? dk_act_name[code] : "?", x, touch ? "touch" : "hybrid"); }
    // panel ids of dmPanel (hle_dos2_touchui_vis.c: Inventory, Character, Skills, Crafting, Journal, Equipment) <-> dmSide codes (hle_dos2_touchui_hud.c)
    static const int k_panel_side[6] = {4, 3, 5, 6, 7, 8};
    switch (code) {
    case DK_A_PANEL:
        if (x < 0 || x > 5) break;
        if (touch) dk_ei_host("dmSide", player, ui, k_panel_side[x]); else dk_ei_host("dmPanel", player, ui, x);
        return 1;
    case DK_A_SIDE:
        if (x < 1 || x > 8) break;
        if (touch) { dk_ei_host("dmSide", player, ui, x); return 1; }
        if (x == 1) dk_ei_host("dmPadBtn", player, ui, 28);        // options: the pause menu ('+')
        else if (x == 2) dk_ei_host("dmPadBtn", player, ui, 27);   // the map ('-')
        else for (int p = 0; p < 6; p++) if (k_panel_side[p] == x) dk_ei_host("dmPanel", player, ui, p);
        return 1;
    case DK_A_PAD: dk_ei_host("dmPadBtn", player, ui, x); return 1;
    case DK_A_KEY: if (x == 1 || x == 2) { dk_ei_host("dmKey", player, ui, x); return 1; } break;
    case DK_A_MAP: if (touch) dk_ei_host("dmSide", player, ui, 2); else dk_ei_host("dmPadBtn", player, ui, 27); return 1;
    case DK_A_MENU: if (touch) dk_ei_host("dmSide", player, ui, 1); else dk_ei_host("dmPadBtn", player, ui, 28); return 1;
    default: break;
    }
    { static unsigned nl; if (nl++ < 20) LOG("[kit] dmAct(%d, %d): no such verb / argument", code, x); }
    return 1;
}
// ---- the kit movie's report ----
static int dk_ei_rects(void *ctx, void *player, void *ui, const char *name, unsigned n, const dm_ei_val *a) {
    (void)ctx; (void)ui; (void)name;
    char buf[4096];
    const char *s = n && a && (a[0].type == 5 || a[0].type == 6) ? dm_ei_str(&a[0], buf, sizeof buf) : NULL;
    if (!s || !player) return 1;
    const float ww = dm_dos2_touch_win_w(), wh = dm_dos2_touch_win_h();
    float sw = 0, sh = 0, k, ox, oy;
    { char *e; sw = strtof(s, &e); sh = *e == ',' ? strtof(e + 1, NULL) : 0; }
    int32_t f[4]; memcpy(f, (const uint8_t *)player + DK_FP_RECT, sizeof f);
    if (!dk_fit(f, sw, sh, &k, &ox, &oy)) { static unsigned nl; if (nl++ < 10) LOG("[kit] dmKRects: the player's rect %d,%d %dx%d does not fit a %.0fx%.0f stage: report dropped", f[0], f[1], f[2], f[3], (double)sw, (double)sh); return 1; }
    dk_rect tmp[64]; const uint64_t now = dk_ms();
    const int got = dk_parse(s, &sw, &sh, tmp, 64, player, now, k, ox, oy, ww, wh);
    if (got < 0) return 1;
    int fresh = 0;
    pthread_mutex_lock(&g_rmu);
    int w = 0;
    for (int i = 0; i < g_nr; i++) if (g_r[i].player != player && now - g_r[i].t <= DK_TTL) g_r[w++] = g_r[i];   // this movie's old set and stale ones go
    g_nr = w;
    for (int i = 0; i < got && g_nr < DK_RMAX; i++) g_r[g_nr++] = tmp[i];
    int pi = -1;
    for (int i = 0; i < DK_PMAX; i++) if (g_pl[i].player == player) pi = i;
    if (pi < 0) { for (int i = 0; i < DK_PMAX; i++) if (!g_pl[i].player || now - g_pl[i].t > 10000) { pi = i; break; } if (pi >= 0) { g_pl[pi].player = player; g_pl[pi].told = 0; fresh = 1; } }
    else if (now - g_pl[pi].t > DK_TTL) g_pl[pi].told = 0;   // silent for longer than a rect lives: maybe a new movie at the same address - tell it again
    if (pi >= 0) { g_pl[pi].k = k; g_pl[pi].t = now; }
    const int tell = g_on && pi >= 0 && !g_pl[pi].told;
    if (tell) g_pl[pi].told = 1;
    pthread_mutex_unlock(&g_rmu);
    g_n_rep++;
    if (tell) {   // the movie acts on the gestures from now on; the least hit size in its stage px; the UI mode (its layout may differ)
        dk_queue(player, DK_EV_SET, (float)dk_set_value(DK_SET_ACT, 1), 2, 1, 0, "act");
        dk_queue(player, DK_EV_SET, (float)dk_set_value(DK_SET_PAD, (int)(g_least * wh / (k > 0 ? k : 1))), 2, 1, 0, "least");
        dk_queue(player, DK_EV_SET, (float)dk_set_value(DK_SET_MODE, dk_touch_mode() ? 0 : 1), 2, 1, 0, "mode");
    }
    { static unsigned nl; if (fresh || (g_dbg && nl++ < 40)) { char mv[48]; LOG("[kit] dmKRects from %s (player %p): %d rect(s), stage %.0fx%.0f, scale %.3f, origin %.0f,%.0f%s", dk_movie(player, mv, sizeof mv), player, got,
        (double)sw, (double)sh, (double)k, (double)ox, (double)oy, tell ? "; dmSet act / least / mode queued" : g_on ? "" : " (DM_DOS2_TOUCH_KIT=0: the movie only traces)"); } }
    return 1;
}
// a kit / M0 movie's trace: dmKitTrace(event name, down, value, stage mouse x, y, element under it, acted)
static int dk_ei_trace(void *ctx, void *player, void *ui, const char *name, unsigned n, const dm_ei_val *a) {
    (void)ctx; (void)ui; (void)name;
    char ev[48] = "?", hit[48] = "", mv[48];
    if (n >= 1 && (a[0].type == 5 || a[0].type == 6)) dm_ei_str(&a[0], ev, sizeof ev);
    if (n >= 6 && (a[5].type == 5 || a[5].type == 6)) dm_ei_str(&a[5], hit, sizeof hit);
    const double down = n >= 2 ? (a[1].type == 3 ? a[1].b : a[1].d) : -1, val = n >= 3 && a[2].type == 4 ? a[2].d : -1, mx = n >= 4 && a[3].type == 4 ? a[3].d : -1, my = n >= 5 && a[4].type == 4 ? a[4].d : -1;
    const double acted = n >= 7 && a[6].type == 4 ? a[6].d : -1;
    uint32_t ty = 0; dm_dos2_touchui_movie_of(player, &ty, mv, sizeof mv);
    static unsigned nl;
    if (nl++ < 200 || g_dbg) LOG("[kit] %s: %s (type %#x) onEvent%s(\"%s\", value %g): stage mouse %.0f,%.0f, element under it: %s%s", (g_m0 & 1) && !strcmp(ev, "IE dmTap") ? "M0-1 REACHED" : "trace",
        mv[0] ? mv : "?", ty, down > 0 ? "Down" : "Up", ev, val, mx, my, hit[0] ? hit : "none", acted > 0 ? " - acted" : acted == 0 ? " - not acted (trace only / no verb)" : "");
    return 1;
}

// ---- M0-1: the 0x8d movie gets an addressed "IE dmTap" ----
void dm_dos2_kit_m0_tap(float x, float y);   // hle_dos2_touch.c: a still finger was lifted (any path), window px
void dm_dos2_kit_m0_tap(float x, float y) {
    if (!(g_m0 & 1)) return;
    dk_obj o[DK_OMAX]; uint8_t *ui[DK_OMAX]; const int n = dk_objects(o, ui, DK_OMAX);
    for (int i = 0; i < n; i++) if (o[i].type == DK_T_DMSIDE && (o[i].flags & DK_UI_VISIBLE) && o[i].player) {
        dk_queue(o[i].player, DK_EV_TAP, 1.f, 2, g_delay, 1, "M0");
        static unsigned nl; if (nl++ < 60) LOG("[kit] M0-1: a tap at %.0f,%.0f: \"IE dmTap\" queued for type 0x8d (flags %#x) in %d poll(s)", (double)x, (double)y, o[i].flags, g_delay);
        return;
    }
    static unsigned nl; if (nl++ < 20) LOG("[kit] M0-1: a tap at %.0f,%.0f: no visible type 0x8d object (the column / right side is not up): nothing sent", (double)x, (double)y);
}
// ---- M0-2: a built UICancel straight into main+0xaea7b0 for the options window ----
static struct { const void *obj; uint64_t seen; int phase, polls, done; uint32_t type; } g_m2;
static void dk_m0_options(void) {
    dk_obj o[DK_OMAX]; uint8_t *ui[DK_OMAX]; const int n = dk_objects(o, ui, DK_OMAX);
    uint8_t *opt = NULL; uint32_t ty = 0;
    for (int i = 0; i < n && !opt; i++) if (o[i].type >= 0x58 && o[i].type <= 0x5b && (o[i].flags & DK_UI_VISIBLE)) { opt = ui[i]; ty = o[i].type; }
    const uint64_t now = dk_ms();
    if (g_m2.phase == 2) {   // after the call: is it still there?
        if (++g_m2.polls < 15) return;
        int there = 0, vis = 0;
        for (int i = 0; i < n; i++) if (ui[i] == g_m2.obj) { there = 1; vis = (o[i].flags & DK_UI_VISIBLE) != 0; }
        LOG("[kit] M0-2 after: 15 polls later the options object type %#x is %s", g_m2.type, !there ? "GONE from the UI manager: CLOSED (the class saw the UICancel)" :
            !vis ? "hidden: CLOSED (the class saw the UICancel)" : "still open: the built event did not close it (see the answers above)");
        g_m2.phase = 0; g_m2.obj = NULL; g_m2.done++;
        return;
    }
    if (!opt || g_m2.done >= 3) { g_m2.obj = NULL; g_m2.phase = 0; return; }
    if (opt != g_m2.obj) { g_m2.obj = opt; g_m2.seen = now; g_m2.phase = 1; g_m2.type = ty; LOG("[kit] M0-2: options window type %#x up: a built UICancel goes to it in 3 s (do not touch it)", ty); return; }
    if (g_m2.phase != 1 || now - g_m2.seen < 3000) return;
    const uint32_t id = dm_dos2_route_event_id("UICancel");
    dm_route_fn fn = dm_dos2_route_dispatch_orig();
    uint8_t *mgr = *(uint8_t *volatile *)(g_main->base + DK_UIMGR);
    if (!id || !fn || !mgr) { LOG("【红】[kit] M0-2: cannot call: UICancel id %#x, dispatch %s, UI manager %p", id, fn ? "ok" : "MISSING (the route hook is off)", (void *)mgr); g_m2.phase = 0; g_m2.done = 3; return; }
    const uint64_t vt_a8 = *(const uint64_t *)(*(const uint8_t *const *)opt + 0xa8);
    char mv[48]; dk_movie(*(const void *const *)(opt + DK_OBJ_PLAYER), mv, sizeof mv);   // before the call: a closed window may be freed by it
    unsigned r[2][2]; uint16_t h[2] = {0, 0};
    for (int d = 1; d >= 0; d--) {   // down, then up; for each pass 1 (w3 = 1) and, if that did not take it, pass 2 (w3 = 0), as main+0xae8468 / 0xae84a8
        uint8_t ev[0x80]; memset(ev, 0, sizeof ev); memcpy(ev, &id, 4); ev[0x20] = (uint8_t)d;   // +4 player 0
        uint16_t handled = 0;
        r[d][0] = (unsigned)(fn((uint64_t)(uintptr_t)mgr, (uint64_t)(uintptr_t)opt, (uint64_t)(uintptr_t)ev, 1, (uint64_t)(uintptr_t)(mgr + DK_UIM_PB), (uint64_t)(uintptr_t)&handled) & 0xff);
        r[d][1] = r[d][0] & 1 ? 0xff : (unsigned)(fn((uint64_t)(uintptr_t)mgr, (uint64_t)(uintptr_t)opt, (uint64_t)(uintptr_t)ev, 0, (uint64_t)(uintptr_t)(mgr + DK_UIM_PB), (uint64_t)(uintptr_t)&handled) & 0xff);
        h[d] = handled;
    }
    LOG("[kit] M0-2 sent: built UICancel (id %#x, player 0) -> main+0xaea7b0 for the options object %p type %#x %s (vt+0xa8 = main+%#llx), mode %s: "
        "down: pass 1 -> %u, pass 2 -> %s%u, handled %u; up: pass 1 -> %u, pass 2 -> %s%u, handled %u (1 = the object took it)", id, (void *)opt, ty,
        mv, (unsigned long long)(vt_a8 - (uint64_t)(uintptr_t)g_main->base), dk_touch_mode() ? "touch" : "hybrid",
        r[1][0], r[1][1] == 0xff ? "(not called) " : "", r[1][1] == 0xff ? 0 : r[1][1], h[1], r[0][0], r[0][1] == 0xff ? "(not called) " : "", r[0][1] == 0xff ? 0 : r[0][1], h[0]);
    g_m2.phase = 2; g_m2.polls = 0;
}
// ---- M0-3: the path of a host pad pulse ----
static _Atomic uint64_t g_m3_from, g_m3_to; static int g_m3_lines; static int g_m3_code;
void dm_dos2_kit_m0_pulse(int code, uint64_t mask, int delay_ms, int key);   // hle_dos2_touch.c dt_ei_padbtn / dm_dos2_touch_pad_pulse
void dm_dos2_kit_m0_pulse(int code, uint64_t mask, int delay_ms, int key) {
    if (!(g_m0 & 4)) return;
    const uint64_t at = dk_ms() + (uint64_t)(delay_ms > 0 ? delay_ms : 0);
    atomic_store(&g_m3_from, at); atomic_store(&g_m3_to, at + 800); g_m3_lines = 0; g_m3_code = code;
    LOG("[kit] M0-3 pulse: dmPadBtn(%d) = pad %#llx, mode %s: %s in %d ms; every input event the UI gets in the 800 ms after it follows", code, (unsigned long long)mask,
        dk_touch_mode() ? "touch" : "hybrid", key >= 0 ? "its keyboard twin" : "the npad pulse", delay_ms);
    if (key >= 0) LOG("[kit] M0-3 pulse: the twin is raw key %#x", key);
}
void dm_dos2_kit_m0_key(uint32_t key, int down);   // hle_dos2_touch.c dt_kp_poll: the host's key record went into InjectInput
void dm_dos2_kit_m0_key(uint32_t key, int down) {
    if (!(g_m0 & 4)) return;
    static unsigned nl; if (nl++ < 60) LOG("[kit] M0-3 key %#x %s record injected (mode byte %d)", key, down ? "DOWN" : "UP", g_main ? *(const volatile uint8_t *)(g_main->base + DK_MODE_BYTE) : -1);
}
static void dk_m3_note(const void *obj, uint32_t type, uint32_t id, int down, int pass1, unsigned ret, unsigned handled) {
    const uint64_t now = dk_ms(), from = atomic_load(&g_m3_from), to = atomic_load(&g_m3_to);
    if (!from || now < from || now > to || g_m3_lines >= 40) return;
    g_m3_lines++;
    char mv[48]; dk_movie(*(const void *const volatile *)((const uint8_t *)obj + DK_OBJ_PLAYER), mv, sizeof mv);
    LOG("[kit] M0-3 +%llu ms (dmPadBtn(%d)): input event %#x \"%s\" %s -> type %#x %s, pass %d: answered %u, handled %u%s", (unsigned long long)(now - from), g_m3_code, id,
        dm_dos2_route_event_name(id), down ? "down" : "up", type, mv, pass1 ? 1 : 2, ret, handled, ret & 1 ? " = TAKEN" : "");
}

// ---- every pad poll (hle_dos2_touch.c dt_drain, game thread) ----
void dm_dos2_kit_poll(void);
void dm_dos2_kit_poll(void) {
    if (!g_on && !g_m0) return;
    if (g_on && g_f.on && !g_f.block) {   // the timed verbs of a still finger
        const int v = dk_rec_time(&g_f.rec, &g_cfg, dk_ms());
        if (v != DK_V_NONE) dk_verb(v, 0);
    }
    if (g_f.clear_in && --g_f.clear_in == 0 && !g_f.on) dk_target_clear();
    int k = 0;
    for (int i = 0; i < g_nq; i++) {
        if (g_q[i].left-- > 0) { g_q[k++] = g_q[i]; continue; }
        dk_deliver(&g_q[i]);
    }
    g_nq = k;
    if (g_m0 & 2) dk_m0_options();
    static unsigned polls;
    if (g_dbg && g_on && ++polls % 1800 == 0)
        LOG("[kit] summary: fingers %u (%u outside a modal), taps %u, double %u, short long %u, long %u, holds %u, drags %u, scrolls %u; events sent %u (%u without a node); reports %u",
            g_n_fing, g_n_block, g_n_verb[DK_V_TAP], g_n_verb[DK_V_DTAP], g_n_verb[DK_V_SLONG], g_n_verb[DK_V_LONG], g_n_verb[DK_V_HOLD_DOWN], g_n_verb[DK_V_DRAG_START], g_n_verb[DK_V_SCROLL],
            g_n_sent, g_n_nonode, g_n_rep);
}

void dm_dos2_touch_kit_install(void);   // main.c host_preload, after dm_dos2_touchui_route_install (needs the EI, input event and route hooks)
void dm_dos2_touch_kit_install(void) {
    int dm_dos2_touchui_on(void);
    const char *v;
    const int want = (v = getenv("DM_DOS2_TOUCH_KIT")) && atoi(v) > 0;
    int m0 = 0;
    if ((v = getenv("DM_DOS2_TOUCH_KIT_M0")) && *v) m0 = !strcmp(v, "all") ? 7 : (atoi(v) & 7);
    if (!want && !m0) return;
    if (!dm_dos2_touchui_on()) { LOG("[kit] DM_DOS2_TOUCH_KIT / _M0 set but the touch UI is off (DM_DOS2_TOUCHUI): nothing"); return; }
    g_dbg = (v = getenv("DM_DBG")) && atoi(v) > 0;
    if ((v = getenv("DM_DOS2_TOUCHUI_IE_DELAY")) && atoi(v) >= 1 && atoi(v) <= 30) g_delay = atoi(v);
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_main = &g_mod[i];
    module_t *m = g_main;
    if (!m) return;
    static const dk_site k_sites[] = {{DK_EV_RD, 4, k_dk_ev_rd}, {DK_LIST_RD, 5, k_dk_list_rd}};
    for (unsigned i = 0; i < sizeof k_sites / sizeof *k_sites; i++)
        if ((uint64_t)k_sites[i].off + 4ull * k_sites[i].n > m->text_end || memcmp(m->base + k_sites[i].off, k_sites[i].want, 4ull * k_sites[i].n)) {
            LOG("【红】[kit] main+%#x differs: not DOS2 update v786432 main (build 4979b200) at the UI manager's input walk: no dmTouch kit, no M0", k_sites[i].off); g_main = NULL; return; }
    const int ei = dm_dos2_touchui_ei_on(), ie = dm_dos2_touchui_ie_on(), rt = dm_dos2_route_on();
    if (!ei || !ie || !rt) { LOG("【红】[kit] needs the ExternalInterface hook (%s), the input event hook (%s) and the route hook (%s): no dmTouch kit, no M0", ei ? "on" : "OFF", ie ? "on" : "OFF", rt ? "on" : "OFF"); g_main = NULL; return; }
    {   // least hit size: 8 mm (or DM_DOS2_TOUCH_KIT_LEAST mm) of the physical scale; 7 % of the height without one
        const char *mm = getenv("DM_DOS2_TOUCH_MM_PER_1080"), *le = getenv("DM_DOS2_TOUCH_KIT_LEAST");
        const float least_mm = le && atof(le) > 2 && atof(le) < 30 ? (float)atof(le) : 8.f;
        if (mm && atof(mm) > 20 && atof(mm) < 400) g_least = least_mm / (float)atof(mm);
    }
    dm_dos2_touchui_ie_extra(dk_ev_name, DK_EV_N, DK_EV_TAP);   // the kit's names get nodes in every movie that lists them
    dm_dos2_touchui_ei_register("dmKitTrace", dk_ei_trace, NULL);
    if (m0 & 4) dm_dos2_route_kit_hooks(want ? dk_accept : NULL, dk_m3_note); else if (want) dm_dos2_route_kit_hooks(dk_accept, NULL);
    if (want) { dm_dos2_touchui_ei_register("dmKRects", dk_ei_rects, NULL); dm_dos2_touchui_ei_register("dmAct", dk_ei_act, NULL); }
    g_m0 = m0; g_on = want;
    if (g_on) LOG("[kit] DM_DOS2_TOUCH_KIT=1: the dmTouch kit (TOUCH_UNIFIED K1..K9): fingers on a kit movie's rects (\"dmKRects\") are the kit's - target top-down "
                  "with the modal rule, mouse to the target only, one recogniser (tap / double 300 / short long 350 / long 600 / hold / drag / scroll), one \"IE dm*\" "
                  "event to the target, dmAct, accept guard in both modes; least hit %.3f of the height; the old paths for every other finger", (double)g_least);
    if (g_m0) LOG("[kit] DM_DOS2_TOUCH_KIT_M0=%d: experiment M0 (log only):%s%s%s", g_m0, (g_m0 & 1) ? " [1] addressed \"IE dmTap\" to type 0x8d on every tap;" : "",
                  (g_m0 & 2) ? " [2] built UICancel straight into main+0xaea7b0 for the options window 3 s after it opens;" : "",
                  (g_m0 & 4) ? " [4] the input events after every dmPadBtn pulse (800 ms)" : "");
}
#endif
