// [DOS2] Host test of the touch UI's physical scale (hle_dos2_touch_scale.h; docs/dos2/TOUCH_MODES_PLAN.md "Implemented: physical scale for
// fingers"). Run with any C compiler: cc dos2_touch_scale_test.c -lm -o <outside the repo>; run it. With `--table` it prints the factors of the
// representative devices as JSON lines (tools/dos2_touch_scale.py reads them: the pictures use the host's own numbers).
#include "../hle_dos2_touch_scale.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef NDEBUG
#error "Build this test with assertions enabled"
#endif
// a device: name, the screen in px (long side first), its diagonal in inches; the UI area = the 16:9 letterbox on narrower screens
typedef struct { const char *name; int w, h; float inch; } dev;
static const dev k_dev[] = {
    {"6.1in 19.5:9 2532x1170", 2532, 1170, 6.1f}, {"6.7in 20:9 2400x1080", 2400, 1080, 6.7f}, {"6.8in 19.3:9 3088x1440", 3088, 1440, 6.8f},
    {"6.5in 16:9 1920x1080", 1920, 1080, 6.5f}, {"8in 16:10 2560x1600", 2560, 1600, 8.0f}, {"11in 4:3 2388x1668", 2388, 1668, 11.0f}};
// what DmActivity computes: the physical height of the game's picture (min(screen height, screen width x 9 / 16)) and the UI's aspect
static void dev_ui(const dev *d, float *mm, float *aspect) {
    const float diag = d->inch * 25.4f, px = sqrtf((float)d->w * d->w + (float)d->h * d->h), wmm = diag * d->w / px, hmm = diag * d->h / px;
    *mm = fminf(hmm, wmm * 9.0f / 16.0f);
    *aspect = (float)d->w / d->h;
}
static int near(float a, float b, float e) { return fabsf(a - b) < e; }
int main(int argc, char **argv) {
    const int table = argc > 1 && !strcmp(argv[1], "--table");
    ts_scale s;
    // unknown size: the iPad's numbers (factor 1, cell 76), windows as designed
    ts_compute(0, 0, 0, 0, &s);
    assert(s.hud == 1 && s.side == 1 && s.pi == 1 && s.cell == TS_CELL0 && s.menu == 1 && s.dialog == 1 && near(s.aspect, 16.0f / 9, 1e-4f));
    // never below 1: a big tablet keeps the iPad's sizes
    ts_compute(150, 16.0f / 9, 0, 0, &s); assert(s.hud == 1 && s.cell == TS_CELL0 && s.examine == 1);
    // narrower than 16:9 = the 16:9 letterbox
    ts_compute(70, 4.0f / 3, 0, 0, &s); assert(near(s.aspect, 16.0f / 9, 1e-4f));
    // overrides win (testing)
    ts_compute(70, 20.0f / 9, 1.25f, 0, &s); assert(s.hud == 1.25f);
    ts_compute(70, 20.0f / 9, 9, 200, &s); assert(s.hud == 2 && s.cell == TS_CELL_MAX);
    ts_compute(70, 20.0f / 9, 0, 96, &s); assert(s.cell == 96);
    // monotonic: a smaller screen never gets a smaller UI; a wider one never a smaller hot bar
    { ts_scale a, b; for (float mm = 40; mm < 140; mm += 2) { ts_compute(mm, 2.2f, 0, 0, &a); ts_compute(mm + 2, 2.2f, 0, 0, &b); assert(a.hud >= b.hud && a.cell >= b.cell); }
      for (float r = 1.78f; r < 2.6f; r += 0.02f) { ts_compute(60, r, 0, 0, &a); ts_compute(60, r + 0.02f, 0, 0, &b); assert(b.hud >= a.hud - 1e-5f); } }
    // the fit, for every aspect between 16:9 and 21:9 and every phone-ish height: nothing in the side strips, nothing collides
    for (float r = 16.0f / 9; r <= 2.4f; r += 0.01f) for (float mm = 40; mm <= 140; mm += 1) {
        ts_compute(mm, r, 0, 0, &s);
        const float sw = s.w;
        assert(s.hud >= 1 && s.hud <= TS_HUD_MAX && s.side <= s.hud + 1e-5f && s.pi <= s.hud + 1e-5f);
        assert(sw / 2 + TS_HB_HALF * s.hud <= sw - TS_MARGIN_X + 0.01f && sw / 2 - TS_HB_HALF * s.hud >= TS_MARGIN_X - 0.01f);   // the hot bar row
        assert(TS_SIDE_BOT * s.side <= 1080 - TS_MARGIN_Y + 0.01f && s.mmap == 1);                       // the column on the screen, the map at the iPad's size
        assert(sw / 2 + TS_HB_HALF * s.hud <= sw - TS_SIDE_COL * s.side - TS_GAP + 0.01f);                 // the hot bar row left of the column
        if (sw / 2 - TS_HB_HALF * s.hud < TS_PI_W * s.pi + TS_GAP) assert(TS_PI_BOT * s.pi <= ts_hb_top(s.hud, TS_HB_TOP_L) - TS_GAP + 0.01f);
        assert(s.cell >= TS_CELL0 && s.cell <= s.cell_cap && ts_inv_rows(s.cell, (float)s.bottom, sw) >= TS_INV_ROWS);
        assert(s.examine >= 1 && s.reward >= 1 && s.splitter >= 1 && s.dialog >= 1 && TS_DIALOG_W * s.dialog <= sw - 2 * fmaxf(TS_MARGIN_X, TS_SIDE_COL * s.side + TS_GAP) + 0.5f);
        assert(s.hb_mm <= TS_TARGET_MM + 0.01f || s.hud == 1);   // never grown past the target
    }
    // the hot bar row of tools/dos2_pchud_touch.py: 16:9 holds x ~1.2 (the column at x ~1.2 beside it)
    ts_compute(80, 16.0f / 9, 0, 0, &s); assert(near(s.cap_row, (960 - 92 * s.side - 10) / 695.0f, 1e-3f));
    // the representative devices
    for (unsigned i = 0; i < sizeof k_dev / sizeof *k_dev; i++) {
        float mm, r; dev_ui(&k_dev[i], &mm, &r); ts_compute(mm, r, 0, 0, &s);
        const int rows = ts_inv_rows(s.cell, (float)s.bottom, s.w);
        if (table) printf("{\"device\": \"%s\", \"w\": %d, \"h\": %d, \"inch\": %.1f, \"mm\": %.2f, \"aspect\": %.4f, \"sw\": %.1f, \"want\": %.4f, \"cap_row\": %.4f, \"hud\": %.4f, \"mmap\": %.4f, "
                          "\"side\": %.4f, \"pi\": %.4f, \"bottom\": %d, \"cell_want\": %d, \"cell_cap\": %d, \"cell\": %d, \"rows\": %d, \"menu\": %.4f, \"examine\": %.4f, "
                          "\"tooltip\": %.4f, \"dialog\": %.4f, \"reward\": %.4f, \"splitter\": %.4f, \"log\": %.4f, \"hb_mm\": %.2f, \"cell_mm\": %.2f}\n",
                          k_dev[i].name, k_dev[i].w, k_dev[i].h, k_dev[i].inch, mm, r, s.w, s.want, s.cap_row, s.hud, s.mmap, s.side, s.pi, s.bottom, s.cell_want, s.cell_cap, s.cell, rows,
                          s.menu, s.examine, s.tooltip, s.dialog, s.reward, s.splitter, s.log, s.hb_mm, s.cell_mm);
        assert(rows >= TS_INV_ROWS && s.hud >= 1);
    }
    // the 6.7" 20:9 phone: the hot bar cell reaches the finger target, the HUD grows by half
    { float mm, r; dev_ui(&k_dev[1], &mm, &r); ts_compute(mm, r, 0, 0, &s); assert(s.hb_mm >= TS_MIN_MM && s.hud > 1.4f); }
    // the 11" tablet: the iPad's sizes
    { float mm, r; dev_ui(&k_dev[5], &mm, &r); ts_compute(mm, r, 0, 0, &s); assert(s.hud == 1 && s.cell == TS_CELL0); }
    if (!table) puts("dos2_touch_scale_test: PASS");
    return 0;
}
