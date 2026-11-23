// [DOS2] The window size decision of hle_dos2_wide.c (DM_DOS2_WIDE / DM_DOS2_RES / DM_DOS2_NARROW / DM_DOS2_FAKE_SCREEN), pure: no game
// memory, no globals, only the environment strings it is handed. Host-tested by tests/dos2_wide_size_test.c (which also prints the table the
// Java mirror Dos2Shape.java must reproduce: tools/dos2_wide_shape_check.sh compares both).
//
// Screens narrower than 16:9 (issue #9, Galaxy Z Fold7 inner 2184x1968 = 1.11:1; Fold6 2208x1768; 16:10 / 4:3 tablets). A window of the
// screen's shape makes the game turn on its own 16:9 letterbox (main+0xc0b4b0, controller UI only): the window rect +0xd8 (the UI) becomes the
// 16:9 band, every viewport is fitted to it (0x90fab0: relative height a / (16/9)) and the camera aspect follows (0x90fc50, still 16:9) - but
// the Switch build's 3D chain squeezes that band once more (its targets and the DRS object are sized from the whole window, 0xa6a770 ->
// 0xb21880; the exact second site is not pinned, the numbers are): the 3D lands in H x (a / (16/9))^2 rows (1200x1080: 0.625^2 x 1080 =
// 422 rows = 765 of the Fold's 1968 = the player's 2184x765 band, 2.86:1) while the camera still draws 16:9 -> everything 1.6x wide. The
// PC build has no DRS and the Switch never has a narrow window, so this path was never exercised on the console.
// DM_DOS2_WIDE_TALL (the letterbox off) fills the window, Hor-, but the UI is laid out by height for a 1920-wide design and is cut off on
// the right.
// The fix: a widescreen request narrower than 16:9 becomes the 16:9 window of the "16:9 with bars" shape (the front end sizes its
// SurfaceView 16:9, Dos2Shape.bars), so the 3D and the UI are exactly what that shape shows - never stretched, never cut. Screens at or
// wider than 16:9 are not touched.
//   DM_DOS2_NARROW unset / bars   that fallback (default)
//   DM_DOS2_NARROW=letterbox      the old way (0.3.0): the screen-shaped window and the game's own letterbox (reproduces issue #9)
//   DM_DOS2_NARROW=tall           the screen-shaped window, the game's letterbox off (= DM_DOS2_WIDE_TALL=1, still accepted): Hor-, UI cut
//   DM_DOS2_FAKE_SCREEN=<W>x<H> | <a>:<b>   debug: pretend the screen has this shape (the front end shows the picture in a rectangle of
//                                 that shape centred on the real screen, Dos2Shape.surfaceRect), e.g. 2184x1968 on a 20:9 phone = the
//                                 Fold7's inner screen; a widescreen request (DM_DOS2_WIDE=<W>x<H> not 16:9, or auto) takes this aspect
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DWS_NARROW_BARS = 0, DWS_NARROW_LETTERBOX = 1, DWS_NARROW_TALL = 2 };
static const char *const dws_narrow_name[] = {"bars", "letterbox", "tall"};

static inline unsigned dws_r16(double v) { return (unsigned)(llround(v / 16) * 16); }
static inline unsigned dws_clampu(unsigned v, unsigned lo, unsigned hi) { return v < lo ? lo : v > hi ? hi : v; }
// DM_DOS2_RES = 720 (default) / 900 / 1080: the window height the gear picked (Tl2Gear.dos2NativeRes)
static inline unsigned dws_base(const char *res) { int v = res ? atoi(res) : 720; return v == 900 || v == 1080 ? (unsigned)v : 720; }
static inline int dws_narrow_mode(const char *narrow, const char *tall) {
    if (narrow && *narrow) {
        if (!strcmp(narrow, "letterbox") || !strcmp(narrow, "game")) return DWS_NARROW_LETTERBOX;
        if (!strcmp(narrow, "tall")) return DWS_NARROW_TALL;
        return DWS_NARROW_BARS;
    }
    return tall && *tall == '1' ? DWS_NARROW_TALL : DWS_NARROW_BARS;
}
// "<W>x<H>" or "<a>:<b>" -> long side / short side (a portrait size counts as landscape); 0 = not set / not usable
static inline double dws_fake_aspect(const char *e) {
    unsigned x = 0, y = 0;
    if (!e || !*e || (sscanf(e, "%ux%u", &x, &y) != 2 && sscanf(e, "%u:%u", &x, &y) != 2) || !x || !y) return 0;
    double a = x > y ? (double)x / y : (double)y / x;
    return a >= 1.0 && a < 4.0 ? a : 0;
}
static inline int dws_narrower(unsigned w, unsigned h) { return (unsigned long long)w * 9 < (unsigned long long)h * 16; }
// The widescreen request the Java gear sends for a screen of aspect a (DmActivity / Dos2Shape.wideSize, the same arithmetic): 0 = none
// (16:9 at 900 = the Switch's 1600x900)
static inline int dws_java_wide(double a, unsigned b, unsigned *w, unsigned *h) {
    const unsigned bw = b * 16 / 9;
    if (fabs(a - 16.0 / 9.0) < 0.01) { if (b == 900) return 0; *w = bw; *h = b; return 1; }
    if (a > 16.0 / 9.0) { unsigned v = dws_r16(b * a); *w = v > 3200 ? 3200 : v; if (*w < bw + 16) *w = bw + 16; *h = b; }
    else { unsigned v = dws_r16(bw / a); *w = bw; *h = v > bw ? bw : v; if (*h < b + 16) *h = b + 16; }
    return 1;
}
// DM_DOS2_WIDE=<W>x<H>: the docked size, rescaled to DM_DOS2_RES's height when that is set (same shape), multiples of 8; handheld x 4/5
// (= docked at 720p). 0 = not usable (or the Switch's 1600x900)
static inline int dws_from_size(unsigned w, unsigned h, const char *res, unsigned s[4], int *rescaled) {
    const unsigned b = dws_base(res);
    if (rescaled) *rescaled = 0;
    if (res && *res && h >= 360 && h != b && b >= 720) { w = (unsigned)((unsigned long long)w * b / h); h = b; if (rescaled) *rescaled = 1; }
    if (!(w >= 640 && h >= 360 && w <= 4096 && h <= 4096 && (w != 1600 || h != 900))) return 0;
    s[0] = (w + 4) / 8 * 8; s[1] = (h + 4) / 8 * 8; s[2] = (w * 4 / 5 + 4) / 8 * 8; s[3] = (h * 4 / 5 + 4) / 8 * 8;   // 2400x1080 exact (16 made it 1088, 14.11)
    if (h <= 720 && w <= 2560) { s[2] = s[0]; s[3] = s[1]; }   // 720p (the default): handheld = docked
    s[0] = dws_clampu(s[0], 640, 3200); s[1] = dws_clampu(s[1], 368, 2400); s[2] = dws_clampu(s[2], 512, 2560); s[3] = dws_clampu(s[3], 288, 1920);
    return 1;
}
// auto / DM_DOS2_WIDE=<a>:<b>: the two modes for an aspect a at the base height (0 = 16:9 at 900: nothing to change)
static inline int dws_from_aspect(double a, unsigned b, unsigned s[4]) {
    if (!(a > 0.5 && a < 8)) return 0;
    if (fabs(a - 16.0 / 9.0) < 0.01) { if (b == 900) return 0; s[0] = s[2] = 1280; s[1] = s[3] = 720; return 1; }
    if (b == 720) {
        if (a > 16.0 / 9.0) { s[0] = s[2] = dws_clampu(dws_r16(720 * a), 1296, 2560); s[1] = s[3] = 720; }
        else { s[0] = s[2] = 1280; s[1] = s[3] = dws_clampu(dws_r16(1280 / a), 736, 1280); }
        return 1;
    }
    const unsigned bw = b * 16 / 9;   // 900 -> 1600, 1080 -> 1920; handheld 720
    if (a > 16.0 / 9.0) { s[0] = dws_clampu(dws_r16(b * a), bw + 16, 3200); s[1] = b; s[2] = dws_clampu(dws_r16(720 * a), 1296, 2560); s[3] = 720; }
    else { s[0] = bw; s[1] = dws_clampu(dws_r16(bw / a), b + 16, bw); s[2] = 1280; s[3] = dws_clampu(dws_r16(1280 / a), 736, 1280); }
    return 1;
}

typedef struct {
    unsigned s[4];        // docked w, h, handheld w, h (valid when ok)
    int ok;               // 1 = patch the modes to s; 0 = the Switch's 1600x900 / 1280x720
    int off;              // DM_DOS2_WIDE=0
    int no_screen;        // auto, but the screen size is not known yet
    const char *how;      // where the request came from (log)
    unsigned rw, rh;      // the request as given (WxH / screen / W), before any rescale
    int rescaled;         // WxH rescaled to DM_DOS2_RES's height
    double fake;          // DM_DOS2_FAKE_SCREEN's aspect (0 = none)
    int faked;            // the request was replaced by the fake screen's shape
    int narrow;           // the requested window was narrower than 16:9
    int mode;             // DWS_NARROW_* (what a narrow window does)
    int fallback;         // narrow + bars: s is the 16:9 window of the "16:9 with bars" shape
    unsigned nw, nh;      // the narrow docked window it replaced (log)
} dws_plan;

static inline void dws_decide(const char *wide, const char *res, const char *narrow, const char *tall, const char *fake, uint64_t screen_px, dws_plan *p) {
    memset(p, 0, sizeof *p);
    p->how = "the screen"; p->mode = dws_narrow_mode(narrow, tall); p->fake = dws_fake_aspect(fake);
    const unsigned b = dws_base(res);
    unsigned w = 0, h = 0, x = 0, y = 0;
    if (wide && *wide == '0' && !wide[1]) { p->off = 1; return; }
    if (wide && sscanf(wide, "%ux%u", &w, &h) == 2) {
        p->how = "DM_DOS2_WIDE=<W>x<H>"; p->rw = w; p->rh = h;
        if (p->fake > 0 && h && fabs((double)w / h - 16.0 / 9.0) >= 0.01) {   // a widescreen request (the gear's 16:9 shapes stay 16:9)
            p->faked = 1; p->how = "DM_DOS2_FAKE_SCREEN";
            if (!dws_java_wide(p->fake, b, &w, &h)) return;
        }
        p->ok = dws_from_size(w, h, res, p->s, &p->rescaled);
    } else if (wide && sscanf(wide, "%u:%u", &x, &y) == 2 && x && y) { p->how = "DM_DOS2_WIDE=<a>:<b>"; p->rw = x; p->rh = y; p->ok = dws_from_aspect((double)x / y, b, p->s); }
    else if (wide && atol(wide) > 1600) {
        p->how = "DM_DOS2_WIDE=<W>"; w = dws_clampu((unsigned)atol(wide), 1616, 3200); p->rw = w; p->rh = 900;
        p->s[0] = (w + 8) / 16 * 16; p->s[1] = 900; p->s[2] = (p->s[0] * 4 / 5 + 8) / 16 * 16; p->s[3] = 720; p->ok = 1;
    } else {
        w = (uint32_t)screen_px; h = (uint32_t)(screen_px >> 32); p->rw = w; p->rh = h;
        if (p->fake > 0) { p->faked = 1; p->how = "DM_DOS2_FAKE_SCREEN"; p->ok = dws_from_aspect(p->fake, b, p->s); }
        else if (w && h) p->ok = dws_from_aspect((double)w / h, b, p->s);
        else { p->no_screen = 1; return; }
    }
    if (!p->ok || !dws_narrower(p->s[0], p->s[1])) return;
    p->narrow = 1; p->nw = p->s[0]; p->nh = p->s[1];
    if (p->mode != DWS_NARROW_BARS) return;
    p->fallback = 1;   // the window of the "16:9 with bars" shape (Tl2Gear.aspectEnv: 1280x720 rescaled to the base height; 900 = the Switch's)
    p->ok = b == 900 ? 0 : dws_from_size(b * 16 / 9, b, res, p->s, NULL);
}
