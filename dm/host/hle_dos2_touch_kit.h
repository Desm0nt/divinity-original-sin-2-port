// [DOS2] dmTouch kit, host side: the pure rules of the one touch handler for both UI modes (docs/dos2/TOUCH_UNIFIED.md section 3.4, contract
// K1..K9; step M1). Kept apart from hle_dos2_touch_kit.c so the host test dm/host/tests/dos2_touch_kit_test.c checks them without the game.
// Everything here is behind DM_DOS2_TOUCH_KIT=1 at run time (hle_dos2_touch_kit.c); with the default 0 none of it is called.
//   K1 target   dk_target: the UI objects top-down (the UI manager's list, last = top), visible ones with the input bit; a movie with dmTouch
//               reports its elements as rects (ExternalInterface "dmKRects", dk_parse). The first live rect under the finger wins; the first
//               visible modal object (flags 0x1e00) ends the walk: a kit modal = the target if the finger is in its rects, else nobody
//               (DK_T_BLOCK: a tap past a modal window does nothing); an old modal (no rects) = the old path (DK_T_LEGACY). No rect hit = the
//               old path too (the world probe, the allow lists) - in M1 no movie has the kit, so every finger stays on the old path.
//   K3 gestures dk_rec_*: one recogniser for the UI in both modes, the iPad's timings (tap; double tap 300 ms / 5 % of the height; short long
//               press 350; long press 600; hold = a long press on an element with a hold verb; drag past 2.5 % of the height; one-finger
//               scroll on a scroll element).
//   K4 delivery dk_ev_*: the input events "IE dm*" the kit movies list in `events` (host ids 0x7f10..; hle_dos2_touchui_ie.c gives them a node),
//               sent to the target only; dmScroll / dmSet carry a number in the event value (+0x18).
//   K7 accept   dk_accept_drop: UIAccept to the kit target within the window after a touch press is dropped unless the pad's A or a host key /
//               pad pulse made it (300 ms).
//   K8 modal    dk_may_act: a dmAct call from an object that is not the target of the gesture in flight is refused.
//   K9 world    dk_window_up: a live window-background rect or a visible modal object = a window is up: no world finger.
#ifndef HLE_DOS2_TOUCH_KIT_H
#define HLE_DOS2_TOUCH_KIT_H
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ---- the kit's input events (names in a movie's `events`; ids above every engine id and above hle_dos2_touchui_ie.c's 0x7f01..0x7f03) ----
enum { DK_EV_TAP = 0x7f10, DK_EV_DTAP, DK_EV_SLONG, DK_EV_LONG, DK_EV_HOLD, DK_EV_SCROLL, DK_EV_SET, DK_EV_END };
#define DK_EV_N (DK_EV_END - DK_EV_TAP)
static const char *const dk_ev_name[DK_EV_N] = {"IE dmTap", "IE dmDTap", "IE dmSLong", "IE dmLong", "IE dmHold", "IE dmScroll", "IE dmSet"};
// dmSet: value = key * 1000000 + number (number 0..999999); the kit movie keeps it (tools/dos2_touchkit.py dmKSet)
enum { DK_SET_ACT = 1 /* 1 = act on the gestures, 0 = trace only (M0) */, DK_SET_PAD = 2 /* least hit size, stage px */, DK_SET_MODE = 3 /* 1 hybrid, 0 touch */ };
static inline double dk_set_value(int key, int num) { return (double)key * 1000000.0 + (double)(num < 0 ? 0 : num > 999999 ? 999999 : num); }
static inline int dk_set_key(double v) { return v >= 0 ? (int)(v / 1000000.0) : -1; }
static inline int dk_set_num(double v) { const int k = dk_set_key(v); return k < 0 ? -1 : (int)(v - (double)k * 1000000.0 + 0.5); }

// ---- rects a kit movie reports: "sw,sh|id,x,y,w,h,flags;..." (stage px; flags = letters) ----
enum { DK_F_EL = 1 /* e: an element */, DK_F_WIN = 2 /* w: the window's background (a tap there is the window's, not the world's) */,
       DK_F_SCROLL = 4 /* s: one-finger drag = IE dmScroll */, DK_F_DRAG = 8 /* d: native drag (the mouse press goes at once) */,
       DK_F_TAP = 16 /* t: native tap (the host clicks there too) */, DK_F_HOLD = 32 /* h: a hold verb (dmHold down / up) */,
       DK_F_MODAL = 64 /* m: the movie is a modal window */ };
static inline uint32_t dk_flags(const char *s, const char *end) {
    uint32_t f = 0;
    for (; s < end && *s; s++) switch (*s) {
        case 'e': f |= DK_F_EL; break; case 'w': f |= DK_F_WIN; break; case 's': f |= DK_F_SCROLL; break; case 'd': f |= DK_F_DRAG; break;
        case 't': f |= DK_F_TAP; break; case 'h': f |= DK_F_HOLD; break; case 'm': f |= DK_F_MODAL; break; default: break; }
    return f;
}
typedef struct { const void *player; uint64_t t; float x0, y0, x1, y1; uint32_t flags; char id[24]; } dk_rect;   // window-normalised (0..1)
// The FlashPlayer rect f = {x, y, w, h} (window px, +0x10..+0x1c) fits a sw x sh stage uniformly and centred: window = origin + stage * k
static inline int dk_fit(const int32_t f[4], float sw, float sh, float *k, float *x0, float *y0) {
    if (sw < 16 || sh < 16 || f[2] < 16 || f[3] < 16 || f[2] > 16384 || f[3] > 16384 || f[0] < -16384 || f[0] > 16384 || f[1] < -16384 || f[1] > 16384) return 0;
    const float kk = fminf((float)f[2] / sw, (float)f[3] / sh);
    *k = kk; *x0 = (float)f[0] + ((float)f[2] - sw * kk) / 2; *y0 = (float)f[1] + ((float)f[3] - sh * kk) / 2;
    return 1;
}
// Parse one report into out[] (at most max); k / ox / oy: stage -> window px; ww / wh: the window. Returns the number of rects, -1 = bad head.
// A rect of no size, or one near the window's size without the window flag (a hidden layer / sentinel), is skipped.
static inline int dk_parse(const char *s, float *sw, float *sh, dk_rect *out, int max, const void *player, uint64_t now, float k, float ox, float oy, float ww, float wh) {
    if (!s || !(ww > 0) || !(wh > 0) || !(k > 0)) return -1;
    char *e1; const float a = strtof(s, &e1); if (e1 == s || *e1 != ',') return -1;
    char *e2; const float b = strtof(e1 + 1, &e2); if (e2 == e1 + 1 || *e2 != '|') return -1;
    *sw = a; *sh = b;
    const char *p = e2 + 1; int n = 0;
    while (*p && n < max) {
        const char *end = strchr(p, ';'); if (!end) end = p + strlen(p);
        const char *c = memchr(p, ',', (size_t)(end - p));
        if (c && c > p && (size_t)(c - p) < sizeof out[0].id) {
            float v[4]; const char *q = c + 1; int ok = 1;
            for (int i = 0; i < 4 && ok; i++) { char *qe; v[i] = strtof(q, &qe); if (qe == q || (*qe != ',' && qe != end && *qe != ';' && *qe)) ok = 0; else q = (*qe == ',') ? qe + 1 : qe; }
            const uint32_t fl = ok ? dk_flags(q, end) : 0;
            if (ok && v[2] > 0 && v[3] > 0 && fabsf(v[0]) < 1e5f && fabsf(v[1]) < 1e5f && ((fl & DK_F_WIN) || (v[2] * k < ww * 0.95f && v[3] * k < wh * 0.95f))) {
                dk_rect *r = &out[n++];
                r->player = player; r->t = now; r->flags = fl ? fl : DK_F_EL;
                memcpy(r->id, p, (size_t)(c - p)); r->id[c - p] = 0;
                r->x0 = (ox + v[0] * k) / ww; r->y0 = (oy + v[1] * k) / wh; r->x1 = (ox + (v[0] + v[2]) * k) / ww; r->y1 = (oy + (v[1] + v[3]) * k) / wh;
            }
        }
        p = *end ? end + 1 : end;
    }
    return n;
}
// A rect grown to at least `least` (window-normalised: the 8 mm the host asks for) around its centre
static inline int dk_in(const dk_rect *r, float nx, float ny, float least_x, float least_y) {
    float x0 = r->x0, x1 = r->x1, y0 = r->y0, y1 = r->y1;
    if (x1 - x0 < least_x) { const float c = (x0 + x1) / 2; x0 = c - least_x / 2; x1 = c + least_x / 2; }
    if (y1 - y0 < least_y) { const float c = (y0 + y1) / 2; y0 = c - least_y / 2; y1 = c + least_y / 2; }
    return nx >= x0 && nx <= x1 && ny >= y0 && ny <= y1;
}

// ---- K1: whose finger ----
typedef struct { const void *player; uint32_t flags, type; } dk_obj;   // a UI object: its FlashPlayer, flags (+0x14), type (+0xf0); top first
enum { DK_T_LEGACY = 0 /* the old path decides (probe / allow lists / world) */, DK_T_KIT = 1 /* a kit element / window: this object */,
       DK_T_BLOCK = 2 /* under a kit modal window, outside it: nobody */ };
enum { DK_UI_INPUT = 0x2, DK_UI_VISIBLE = 0x8, DK_UI_MODAL = 0x1e00 };
// *obj = the object index (KIT / BLOCK), *rect = the rect index (KIT); an element beats a window background of the same object
static inline int dk_target(const dk_obj *o, int n, const dk_rect *r, int nr, uint64_t now, uint64_t ttl, float nx, float ny, float lx, float ly, int *obj, int *rect) {
    *obj = -1; *rect = -1;
    for (int i = 0; i < n; i++) {
        if ((o[i].flags & (DK_UI_INPUT | DK_UI_VISIBLE)) != (DK_UI_INPUT | DK_UI_VISIBLE) || !o[i].player) continue;
        int live = 0, el = -1, win = -1, modal = (o[i].flags & DK_UI_MODAL) != 0;
        for (int j = 0; j < nr; j++) {
            if (r[j].player != o[i].player || now - r[j].t > ttl) continue;
            live = 1;
            if (r[j].flags & DK_F_MODAL) modal = 1;
            if (!dk_in(&r[j], nx, ny, (r[j].flags & DK_F_EL) ? lx : 0, (r[j].flags & DK_F_EL) ? ly : 0)) continue;
            if ((r[j].flags & DK_F_EL) && el < 0) el = j;
            if ((r[j].flags & DK_F_WIN) && win < 0) win = j;
        }
        if (live && (el >= 0 || win >= 0)) { *obj = i; *rect = el >= 0 ? el : win; return DK_T_KIT; }
        if (modal) { if (live) { *obj = i; return DK_T_BLOCK; } return DK_T_LEGACY; }
    }
    return DK_T_LEGACY;
}
// K9: a window is up (no world finger): a live window-background rect of a visible object, or a visible modal kit movie
static inline int dk_window_up(const dk_obj *o, int n, const dk_rect *r, int nr, uint64_t now, uint64_t ttl) {
    for (int i = 0; i < n; i++) {
        if (!(o[i].flags & DK_UI_VISIBLE) || !o[i].player) continue;
        for (int j = 0; j < nr; j++) if (r[j].player == o[i].player && now - r[j].t <= ttl && (r[j].flags & (DK_F_WIN | DK_F_MODAL))) return 1;
    }
    return 0;
}

// ---- K3: the recogniser of one kit finger ----
typedef struct { int slong_ms, long_ms, dtap_ms; float slop, dtap_r, scroll_step; } dk_cfg;
static inline dk_cfg dk_cfg_ipad(float win_h) { dk_cfg c = {350, 600, 300, win_h * 0.025f, win_h * 0.05f, 4.f}; return c; }
enum { DK_V_NONE = 0, DK_V_TAP, DK_V_DTAP, DK_V_SLONG, DK_V_LONG, DK_V_HOLD_DOWN, DK_V_HOLD_UP, DK_V_DRAG_START, DK_V_DRAG_END, DK_V_SCROLL };
enum { DK_R_STILL = 0, DK_R_DRAG = 1, DK_R_SCROLL = 2, DK_R_DONE = 3 /* slong / long / hold fired: the release does no tap */ };
typedef struct {
    int st, slong, lng, hold; uint32_t flags; float sx, sy, lx, ly, sacc; uint64_t t0;
    uint64_t tap_up; float tap_x, tap_y;   // the last TAP's release (the double tap's first half), kept across fingers
} dk_rec;
static inline void dk_rec_down(dk_rec *r, uint32_t flags, float x, float y, uint64_t t) {
    r->st = DK_R_STILL; r->slong = r->lng = r->hold = 0; r->flags = flags; r->sx = r->lx = x; r->sy = r->ly = y; r->sacc = 0; r->t0 = t;
}
// a move: DK_V_DRAG_START once (past the slop, not a scroll element), DK_V_SCROLL with *px = the motion since the last step (scroll element)
static inline int dk_rec_move(dk_rec *r, const dk_cfg *c, float x, float y, float *px) {
    *px = 0;
    const float dy = y - r->ly; r->lx = x; r->ly = y;
    if (r->st == DK_R_SCROLL) { r->sacc += dy; if (fabsf(r->sacc) >= c->scroll_step) { *px = r->sacc; r->sacc = 0; return DK_V_SCROLL; } return DK_V_NONE; }
    if (r->st != DK_R_STILL || r->hold) return DK_V_NONE;
    if (!(hypotf(x - r->sx, y - r->sy) > c->slop)) return DK_V_NONE;
    if (r->flags & DK_F_SCROLL) { r->st = DK_R_SCROLL; *px = y - r->sy; r->sacc = 0; return DK_V_SCROLL; }
    r->st = DK_R_DRAG; return DK_V_DRAG_START;
}
// time passes while the finger is still: the short long press, then the long press (or the hold on a hold element) - each once
static inline int dk_rec_time(dk_rec *r, const dk_cfg *c, uint64_t t) {
    if (r->st != DK_R_STILL && r->st != DK_R_DONE) return DK_V_NONE;
    const uint64_t held = t - r->t0;
    if (!r->lng && !r->hold && c->long_ms > 0 && held >= (uint64_t)c->long_ms) {
        r->st = DK_R_DONE;
        if (r->flags & DK_F_HOLD) { r->hold = 1; return DK_V_HOLD_DOWN; }
        r->lng = 1; return DK_V_LONG;
    }
    if (!r->slong && !r->lng && !r->hold && c->slong_ms > 0 && c->slong_ms < c->long_ms && held >= (uint64_t)c->slong_ms) { r->slong = 1; r->st = DK_R_DONE; return DK_V_SLONG; }
    return DK_V_NONE;
}
// the release: TAP / DTAP (a tap within dtap_ms of the last tap's release, near it) / HOLD_UP / DRAG_END / NONE (after a short long / long press, a scroll)
static inline int dk_rec_up(dk_rec *r, const dk_cfg *c, float x, float y, uint64_t t) {
    const int st = r->st; r->st = DK_R_DONE;
    if (r->hold) { r->hold = 0; return DK_V_HOLD_UP; }
    if (st == DK_R_DRAG) return DK_V_DRAG_END;
    if (st != DK_R_STILL) return DK_V_NONE;
    if (hypotf(x - r->sx, y - r->sy) > c->slop) return DK_V_NONE;
    if (r->tap_up && r->t0 >= r->tap_up && r->t0 - r->tap_up <= (uint64_t)(c->dtap_ms > 0 ? c->dtap_ms : 0) && hypotf(r->sx - r->tap_x, r->sy - r->tap_y) <= c->dtap_r) {
        r->tap_up = 0; return DK_V_DTAP; }
    r->tap_up = t ? t : 1; r->tap_x = r->sx; r->tap_y = r->sy;
    return DK_V_TAP;
}
// the event a verb is delivered as (0 = none)
static inline uint32_t dk_verb_event(int v) {
    switch (v) {
    case DK_V_TAP: return DK_EV_TAP; case DK_V_DTAP: return DK_EV_DTAP; case DK_V_SLONG: return DK_EV_SLONG; case DK_V_LONG: return DK_EV_LONG;
    case DK_V_HOLD_DOWN: case DK_V_HOLD_UP: return DK_EV_HOLD; case DK_V_SCROLL: return DK_EV_SCROLL; default: return 0;
    }
}
// the mouse the host sends the target with a verb (K2: the native code only): bits of DK_DO_*
enum { DK_DO_MOVE = 1, DK_DO_PRESS = 2, DK_DO_RELEASE = 4 };
static inline unsigned dk_verb_mouse(int v, uint32_t flags) {
    switch (v) {
    case DK_V_NONE: return 0;
    case DK_V_TAP: case DK_V_DTAP: return (flags & DK_F_TAP) ? DK_DO_MOVE | DK_DO_PRESS | DK_DO_RELEASE : DK_DO_MOVE;
    case DK_V_DRAG_END: return (flags & DK_F_DRAG) ? DK_DO_MOVE | DK_DO_RELEASE : 0;
    default: return 0;
    }
}

// ---- K7 / K8 ----
// UIAccept to the target `now`: the touch press was at touch_ms, the pad's A last down at a_ms, the host's last key / pad pulse at host_ms
static inline int dk_accept_drop(uint64_t now, uint64_t touch_ms, uint64_t a_ms, uint64_t host_ms, int win_ms) {
    if (!touch_ms || now < touch_ms || now - touch_ms >= (uint64_t)(win_ms > 0 ? win_ms : 0)) return 0;
    if (a_ms && now >= a_ms && now - a_ms <= 300) return 0;
    if (host_ms && now >= host_ms && now - host_ms <= 300) return 0;
    return 1;
}
// a dmAct from `caller` (its UI object): refused while a gesture of another target is in flight (target set at target_t, ttl ms)
static inline int dk_may_act(const void *caller, const void *target, uint64_t now, uint64_t target_t, uint64_t ttl) {
    if (!target || !target_t || now < target_t || now - target_t > ttl) return 1;
    return caller == target;
}

// ---- K5: dmAct codes (one EI instead of dmPadBtn / dmPanel / dmSide / dmKey / dmAimBtn; the old names stay as aliases until M8) ----
// dmAct(code, a, b): the host picks the mode's way. The table of hle_dos2_touch_kit.c dk_act (tools/dos2_touchkit.py reads these names).
enum { DK_A_PANEL = 1 /* a = 0..5 panel id: hybrid openPanel / touch dmSide(2..7) */, DK_A_SIDE = 2 /* a = the touch right side code 1..8 */,
       DK_A_PAD = 3 /* a = LS_Classes.controllerHelper BTN code: hybrid npad pulse / touch key twin */, DK_A_KEY = 4 /* a = 1 quick save f5, 2 tactical camera o */,
       DK_A_MAP = 5 /* the map */, DK_A_MENU = 6 /* the pause menu (escape) */, DK_A_N = 7 };
static const char *const dk_act_name[DK_A_N] = {"?", "panel", "side", "pad", "key", "map", "menu"};
#endif
