// [DOS2] Host test of the widescreen window decision (hle_dos2_wide_size.h; issue #9: screens narrower than 16:9). Any C compiler:
//   cc dos2_wide_size_test.c -lm -o <outside the repo>; run it (PASS or an assertion). `--table` prints one line per case, the same lines
//   the Java mirror prints (tools/dos2_wide_shape_check.sh diffs the two: Dos2Shape.java must make the same window and view).
// What it proves on the PC:
//   1. screens at or wider than 16:9 (16:9, 18:9, 19.5:9, 20:9, Mi 14 2670x1200, 21:9): every shape / base height / narrow mode gives the
//      window 0.3.0 gave (its code is copied below as old_*) - nothing changes there;
//   2. narrower screens (Fold7 inner 2184x1968, Fold6 2208x1768, 16:10 2560x1600, 4:3 2048x1536 / 2388x1668), widescreen, default: the
//      window is exactly the "16:9 with bars" one (same DRS range, clusters, touch window), the game's letterbox stays off (w/h >= 16/9 at
//      0xc0b548), the camera aspect (0x90fc50) = the view's aspect -> stretch 1.000, the 16:9 UI fully visible;
//   3. the old way (DM_DOS2_NARROW=letterbox) reproduces the player's numbers: Fold7 -> window 1200x1080 (their log), the 3D in a
//      2184 x ~765 band (2.86:1), 1.6x wide; DM_DOS2_NARROW=tall -> the UI 37.5 % cut off on the right;
//   4. DM_DOS2_FAKE_SCREEN on a 20:9 phone gives the foldable's windows (the owner's check on the Mi 14).
#include "../hle_dos2_wide_size.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef NDEBUG
#error "Build this test with assertions enabled"
#endif

// ---- 0.3.0's hle_dos2_wide.c (release v0.3.0, 2a065f51), the size part verbatim but for the names: the reference for "unchanged"
static unsigned old_base(const char *e) { int v = e ? atoi(e) : 720; return v == 900 || v == 1080 ? (unsigned)v : 720; }
static unsigned old_r16(double v) { return (unsigned)(llround(v / 16) * 16); }
static unsigned old_clampu(unsigned v, unsigned lo, unsigned hi) { return v < lo ? lo : v > hi ? hi : v; }
static int old_from_aspect(double a, const char *res, unsigned s[4]) {
    if (!(a > 0.5 && a < 8)) return 0;
    const unsigned b = old_base(res);
    if (fabs(a - 16.0 / 9.0) < 0.01) { if (b == 900) return 0; s[0] = s[2] = 1280; s[1] = s[3] = 720; return 1; }
    if (b == 720) {
        if (a > 16.0 / 9.0) { s[0] = s[2] = old_clampu(old_r16(720 * a), 1296, 2560); s[1] = s[3] = 720; }
        else { s[0] = s[2] = 1280; s[1] = s[3] = old_clampu(old_r16(1280 / a), 736, 1280); }
        return 1;
    }
    const unsigned bw = b * 16 / 9;
    if (a > 16.0 / 9.0) { s[0] = old_clampu(old_r16(b * a), bw + 16, 3200); s[1] = b; s[2] = old_clampu(old_r16(720 * a), 1296, 2560); s[3] = 720; }
    else { s[0] = bw; s[1] = old_clampu(old_r16(bw / a), b + 16, bw); s[2] = 1280; s[3] = old_clampu(old_r16(1280 / a), 736, 1280); }
    return 1;
}
static int old_decide(const char *e, const char *res, uint64_t screen, unsigned s[4]) {
    unsigned w = 0, h = 0, x = 0, y = 0;
    if (e && *e == '0' && !e[1]) return 0;
    if (e && sscanf(e, "%ux%u", &w, &h) == 2) {
        const unsigned b = old_base(res);
        if (res && *res && h >= 360 && h != b && b >= 720) { w = (unsigned)((unsigned long long)w * b / h); h = b; }
        if (w >= 640 && h >= 360 && w <= 4096 && h <= 4096 && (w != 1600 || h != 900)) {
            s[0] = (w + 4) / 8 * 8; s[1] = (h + 4) / 8 * 8; s[2] = (w * 4 / 5 + 4) / 8 * 8; s[3] = (h * 4 / 5 + 4) / 8 * 8;
            if (h <= 720 && w <= 2560) { s[2] = s[0]; s[3] = s[1]; }
            s[0] = old_clampu(s[0], 640, 3200); s[1] = old_clampu(s[1], 368, 2400); s[2] = old_clampu(s[2], 512, 2560); s[3] = old_clampu(s[3], 288, 1920);
            return 1;
        }
        return 0;
    }
    if (e && sscanf(e, "%u:%u", &x, &y) == 2 && x && y) return old_from_aspect((double)x / y, res, s);
    if (e && atol(e) > 1600) { w = old_clampu((unsigned)atol(e), 1616, 3200); s[0] = (w + 8) / 16 * 16; s[1] = 900; s[2] = (s[0] * 4 / 5 + 8) / 16 * 16; s[3] = 720; return 1; }
    w = (uint32_t)screen; h = (uint32_t)(screen >> 32);
    return w && h ? old_from_aspect((double)w / h, res, s) : 0;
}

// ---- the game's own aspect code (main.raw v786432; hle_dos2_wide.c header), as a model
typedef struct { int lbox; double rect_w, rect_h, vp_h, cam; } game_view;
static void game_model(unsigned W, unsigned H, int tall, game_view *g) {
    const double t = 16.0 / 9.0, a = (double)W / H;
    g->lbox = !tall && a < t;                                   // 0xc0b548 fcmp + b.mi (controller UI byte set), nop'd by tall
    double r = g->lbox ? a / t : 1;                              // 0x93d320 rect / 0x90fab0 viewport: relative height a / t
    g->rect_w = W; g->rect_h = llround(r * H); g->vp_h = llround(r * H);
    g->cam = W / g->vp_h;                                        // 0x90fc50: W x rel w / (H x rel h)
}
// the SurfaceView rect (Dos2Shape.surfaceRect / GearDisplay.fitSurface arithmetic)
static void view_rect(int fw, int fh, int bars, double fake, int r[4]) {
    int sw = fw, sh = fh;
    if (fake > 0) { if ((double)fw / fh > fake) sw = (int)llround(fh * fake); else sh = (int)llround(fw / fake); }
    int w = sw, h = sh;
    if (bars && (long long)sw * 9 > (long long)sh * 16) w = sh * 16 / 9;
    else if (bars && (long long)sw * 9 < (long long)sh * 16) h = sw * 9 / 16;
    r[0] = (fw - w) / 2; r[1] = (fh - h) / 2; r[2] = w; r[3] = h;
}
static unsigned native_res(unsigned shortside) { return shortside >= 1080 ? 1080 : shortside >= 900 ? 900 : 720; }   // Tl2Gear.dos2NativeRes
// Tl2Gear.aspectEnv: the request the gear sends for a shape (0 bars, 1 stretched, 2 widescreen) from the REAL screen
static void gear_request(int shape, unsigned b, double a, char *out, size_t n) {
    unsigned w, h;
    if (shape == 2) { if (dws_java_wide(a, b, &w, &h)) snprintf(out, n, "%ux%u", w, h); else snprintf(out, n, "0"); }
    else if (b == 900) snprintf(out, n, "0"); else snprintf(out, n, "1280x720");
}
typedef struct { const char *name; unsigned w, h; } dev;
static const dev k_dev[] = {
    {"fold7-inner", 2184, 1968}, {"fold6-inner", 2208, 1768}, {"tab-16:10", 2560, 1600}, {"tab-4:3", 2048, 1536}, {"ipad11-4:3", 2388, 1668},
    {"tab-16:10-720", 1280, 800}, {"phone-16:9", 1920, 1080}, {"phone-18:9", 2160, 1080}, {"phone-20:9", 2400, 1080}, {"mi14", 2670, 1200},
    {"phone-19.5:9", 2532, 1170}, {"phone-21:9", 3200, 1440}, {"phone-900", 1600, 900}, {"phone-720-20:9", 1600, 720}};
static const char *const k_fake[] = {"", "2184x1968", "2208x1768", "16:10", "4:3"};
static const char *const k_mode[] = {"", "bars", "letterbox", "tall"};

int main(int argc, char **argv) {
    const int table = argc > 1 && !strcmp(argv[1], "--table");
    // parsing
    assert(dws_narrow_mode(NULL, NULL) == DWS_NARROW_BARS && dws_narrow_mode("", "1") == DWS_NARROW_TALL && dws_narrow_mode("letterbox", "1") == DWS_NARROW_LETTERBOX);
    assert(dws_narrow_mode("tall", NULL) == DWS_NARROW_TALL && dws_narrow_mode("bars", "1") == DWS_NARROW_BARS && dws_narrow_mode("junk", NULL) == DWS_NARROW_BARS);
    assert(fabs(dws_fake_aspect("2184x1968") - 2184.0 / 1968) < 1e-9 && fabs(dws_fake_aspect("1968x2184") - 2184.0 / 1968) < 1e-9);
    assert(fabs(dws_fake_aspect("4:3") - 4.0 / 3) < 1e-9 && dws_fake_aspect("") == 0 && dws_fake_aspect("junk") == 0 && dws_fake_aspect("0x5") == 0 && dws_fake_aspect(NULL) == 0);
    int cases = 0, unchanged = 0, fixed = 0;
    for (size_t d = 0; d < sizeof k_dev / sizeof *k_dev; d++)
    for (size_t f = 0; f < sizeof k_fake / sizeof *k_fake; f++)
    for (int shape = 0; shape < 3; shape++)
    for (size_t mi = 0; mi < sizeof k_mode / sizeof *k_mode; mi++) {
        const dev *v = &k_dev[d]; const char *fake = k_fake[f], *mode = k_mode[mi];
        if (*fake && strcmp(v->name, "mi14") && strcmp(v->name, "phone-20:9")) continue;   // fakes: on the owner's kind of phone
        const unsigned b = native_res(v->h); char res[8], req[32]; snprintf(res, sizeof res, "%u", b);
        const double real = (double)v->w / v->h, fa = dws_fake_aspect(fake), a = fa > 0 ? fa : real;
        gear_request(shape, b, real, req, sizeof req);
        dws_plan p; dws_decide(req, res, *mode ? mode : NULL, NULL, fake, 0, &p);
        const unsigned W = p.ok ? p.s[0] : 1600, H = p.ok ? p.s[1] : 900;
        const int nm = dws_narrow_mode(*mode ? mode : NULL, NULL);
        const int narrow_screen = a < 16.0 / 9.0 - 0.01;   // Dos2Shape.narrower
        const int bars = shape == 0 || (shape == 2 && narrow_screen && nm == DWS_NARROW_BARS);
        // the front end's view: the screen (frame = the screen in landscape) or the fake rectangle, then 16:9 inside when bars
        int r[4]; view_rect((int)v->w, (int)v->h, bars, fa, r);
        // the same decision both sides: the native window is narrower than 16:9 exactly when the Java side shows widescreen with bars
        assert(!(shape == 2 && nm == DWS_NARROW_BARS) || p.fallback == narrow_screen);
        game_view g; game_model(W, H, nm == DWS_NARROW_TALL, &g);
        const double view_a = (double)r[2] / r[3];
        // what the eye sees: the presenter scales the window uniformly into the view, so the view must have the window's shape (bars, and
        // widescreen at or wider than 16:9 / narrower with the fallback); "stretched" stretches on purpose, the old narrow ways are case 3
        if (shape == 0 || (shape == 2 && (!narrow_screen || nm == DWS_NARROW_BARS))) assert(!g.lbox && fabs(view_a / ((double)W / H) - 1) < 0.01);
        if (!narrow_screen || shape != 2) {   // 1. at or wider than 16:9 (and the 16:9 shapes everywhere): the 0.3.0 window
            unsigned o[4] = {0}; int ook = old_decide(req, res, 0, o);
            if (!*fake || shape != 2) {   // (a fake screen replaces only a widescreen request)
                assert(ook == p.ok); if (ook) assert(!memcmp(o, p.s, sizeof o));
                unchanged++;
            }
        } else if (nm == DWS_NARROW_BARS) {   // 2. narrower, widescreen, default: the bars window, no game letterbox, no stretch, the UI whole
            dws_plan q; char rq[32]; gear_request(0, b, real, rq, sizeof rq); dws_decide(rq, res, NULL, NULL, fake, 0, &q);
            assert(q.ok == p.ok && (!p.ok || !memcmp(q.s, p.s, sizeof q.s)));       // same window as "16:9 with bars": DRS / clusters / touch the same
            assert(!dws_narrower(W, H) && !g.lbox && fabs(g.cam - 16.0 / 9.0) < 0.01);
            assert(fabs(view_a - 16.0 / 9.0) < 0.003);                               // the view 16:9 -> stretch = view aspect / camera aspect
            assert(fabs(view_a / g.cam - 1) < 0.005);
            assert(g.rect_w >= g.rect_h * 16.0 / 9.0 - 1);                            // UI rect fits a 1920x1080 design scaled by height: nothing cut
            fixed++;
        }
        cases++;
        if (table) printf("dev=%s screen=%ux%u fake=%s base=%u shape=%d narrow=%s window=%ux%u view=%s rect=%d,%d,%d,%d\n", v->name, v->w, v->h,
                          *fake ? fake : "-", b, shape, dws_narrow_name[nm], W, H, bars ? "16:9" : "full", r[0], r[1], r[2], r[3]);
    }
    // 3. the player's session (issue #9): Fold7 inner, base 1080, widescreen, the old way -> their log line and their band
    {
        char req[32]; gear_request(2, 1080, 2184.0 / 1968, req, sizeof req); assert(!strcmp(req, "1920x1728"));
        dws_plan p; dws_decide(req, "1080", "letterbox", NULL, NULL, 0, &p);
        assert(p.ok && p.s[0] == 1200 && p.s[1] == 1080 && p.narrow && !p.fallback);   // "DOS2 window 1600x900 -> 1200x1080"
        game_view g; game_model(1200, 1080, 0, &g);
        const double r = (1200.0 / 1080) / (16.0 / 9), band = 1080 * r * r, rows = band * 1968 / 1080, band_a = 2184 / rows;
        assert(g.lbox && fabs(g.vp_h - 675) < 1 && fabs(g.cam - 16.0 / 9.0) < 0.01);
        assert(fabs(rows - 765) < 6 && fabs(band_a - 2.86) < 0.03);                     // the player's "2184x765 (2.86:1)"
        assert(fabs(band_a / g.cam - 1.6) < 0.02);                                        // characters 1.6x wide
        dws_decide(req, "1080", "tall", NULL, NULL, 0, &p);
        game_model(p.s[0], p.s[1], 1, &g);
        assert(!g.lbox && fabs(g.rect_w / (g.rect_h * 16.0 / 9.0) - 0.625) < 0.001);    // the 16:9 UI 37.5 % cut off on the right
        dws_decide(req, "1080", NULL, NULL, NULL, 0, &p);
        assert(p.fallback && p.ok && p.s[0] == 1920 && p.s[1] == 1080 && p.s[2] == 1536 && p.s[3] == 864);   // = their bars session (boot.log)
        dws_decide(req, "1080", NULL, "1", NULL, 0, &p); assert(p.mode == DWS_NARROW_TALL && p.s[0] == 1200);  // DM_DOS2_WIDE_TALL=1 still the old tall
    }
    // 4. the owner's check: Mi 14 (2670x1200, base 1080) with DM_DOS2_FAKE_SCREEN=2184x1968
    {
        char req[32]; gear_request(2, 1080, 2670.0 / 1200, req, sizeof req); assert(!strcmp(req, "2400x1080"));
        dws_plan p; dws_decide(req, "1080", NULL, NULL, "2184x1968", 0, &p);
        assert(p.faked && p.fallback && p.s[0] == 1920 && p.s[1] == 1080);
        dws_decide(req, "1080", "letterbox", NULL, "2184x1968", 0, &p); assert(p.s[0] == 1200 && p.s[1] == 1080);   // the player's window, on the Mi 14
        int r[4]; view_rect(2670, 1200, 1, 2184.0 / 1968, r); assert(r[2] == 1332 && r[3] == 749 && r[0] == 669 && r[1] == 225);
        view_rect(2670, 1200, 0, 2184.0 / 1968, r); assert(r[2] == 1332 && r[3] == 1200 && r[0] == 669 && r[1] == 0);
        dws_decide("1280x720", "1080", NULL, NULL, "2184x1968", 0, &p); assert(!p.faked && p.s[0] == 1920 && p.s[1] == 1080);   // bars shape stays 16:9
    }
    // auto (no DM_DOS2_WIDE) and the other forms
    {
        dws_plan p;
        dws_decide(NULL, "1080", NULL, NULL, NULL, (uint64_t)1968 << 32 | 2184, &p); assert(p.fallback && p.s[0] == 1920 && p.s[1] == 1080);
        dws_decide(NULL, "1080", NULL, NULL, NULL, (uint64_t)1080 << 32 | 2400, &p); assert(!p.narrow && p.s[0] == 2400 && p.s[1] == 1080);
        dws_decide(NULL, "1080", NULL, NULL, NULL, 0, &p); assert(p.no_screen && !p.ok);
        dws_decide("0", "1080", NULL, NULL, "2184x1968", 0, &p); assert(p.off && !p.ok);
        dws_decide("4:3", "900", NULL, NULL, NULL, 0, &p); assert(p.fallback && !p.ok);    // 900: the Switch's 1600x900, as the bars shape there
        dws_decide("4:3", "720", NULL, NULL, NULL, 0, &p); assert(p.fallback && p.ok && p.s[0] == 1280 && p.s[1] == 720 && p.s[2] == 1280);
        dws_decide("2000", "900", NULL, NULL, NULL, 0, &p); assert(p.ok && p.s[0] == 2000 && p.s[1] == 900);
    }
    if (!table) printf("PASS: %d cases (%d unchanged vs 0.3.0, %d narrower-screen widescreen fixed)\n", cases, unchanged, fixed);
    return 0;
}
