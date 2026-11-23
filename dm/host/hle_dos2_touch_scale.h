// [DOS2] UI mode TOUCH: the size of the touch UI from the physical screen (docs/dos2/TOUCH_MODES_PLAN.md "Implemented: physical scale for
// fingers"). Pure rule, kept apart so a host test can check it (dm/host/tests/dos2_touch_scale_test.c, which also prints the per-device table
// tools/dos2_touch_scale.py draws).
// Input: mm = the millimetres that 1080 design px span on this device (the physical height of the game's UI area: Java DmActivity sets
// DM_DOS2_TOUCH_MM_PER_1080 from DisplayMetrics; a 16:9 picture letterboxed on a narrower screen is its own height), aspect = the UI area's
// width / height (hle_dos2_wide.c's window; narrower than 16:9 = the game's own 16:9 letterbox). Every size below is a design px of a 1080-high
// screen (the engine's "fixed" layout: k = c x S x h / 1080, hle_dos2_touchui_hud.h; one stage px of a host object = its factor px).
// Physics: the iPad's touch UI (cell 76..82) is ~11 mm on an 11" iPad; on a 6.7" 20:9 phone 1080 px are ~70 mm, so 82 px are ~5.3 mm. A finger
// wants >= ~7.5 mm (Android's 48 dp), comfortably 8. So the hot bar grows to an 8 mm cell while its row FITS between the left ~80 px strip
// (notch / punch hole; DmActivity draws into the cutout) and the right column. Every HUD object keeps the iPad's place and grows about its
// anchor (owner 20.11: the iPad's layout of work/uimodes/render/panels3/skills_p_mock_2400.png, nothing moves into the middle); the
// minimap does NOT grow (a phone needs bigger tap targets, not a bigger map: its small buttons get finger pads in minimap_p instead). The panels keep the hot bar at factor 1 while one is open (the host: hle_dos2_touchui_hud.c), get their cells from the
// same 8 mm target clamped to what still shows 5 inventory rows above that hot bar, and the small windows grow while they fit.
#ifndef HLE_DOS2_TOUCH_SCALE_H
#define HLE_DOS2_TOUCH_SCALE_H
#include <math.h>

// ---- the target
#define TS_TARGET_MM 8.0f          // a hot bar cell / inventory cell of 8 mm (Android's 48 dp = 7.6 mm is the floor a finger needs)
#define TS_MIN_MM 7.5f             // reported only: below this a tap target is "small"
#define TS_HB_CELL 82.0f           // the hot bar cell at factor 1 (tools/dos2_pchud_touch.py: iPad icon 76 in an 82 cell, pitch 86)
#define TS_CELL0 76                // the panels' cell at factor 1 (the iPad's)
#define TS_HUD_MAX 2.0f            // never more (a big tablet in a small window, a wrong DPI)
// ---- the screen
#define TS_MARGIN_X 80.0f          // design px kept free at each side (notch / punch hole; tools/dos2_ppanel_inv.py MARGIN)
#define TS_MARGIN_Y 8.0f
#define TS_GAP 10.0f               // between two HUD objects
// ---- the HUD movies (tools/dos2_pchud_touch.py; its `check` compares these): stage px at factor 1, each object scales about its anchor
#define TS_HB_HALF 683.0f          // hotBar_p (anchor bottom): the row (chat plate | bar: deck, 12 slots, actions | combat log plate) centred, +-683 px
#define TS_HB_TOP 125.0f           // its frame's top above the screen bottom (stage 875 of 1000)
#define TS_HB_TOP_R 125.0f         // ... at the right end (the combat log plate 888..972 is under the frame's top since 23.11)
#define TS_HB_TOP_L 125.0f         // ... at the left end (the chat plate, the same)
#define TS_MM_MAX 1.0f             // minimap_p (anchor topright): the iPad's ring (~24 % of the height) at every size (owner 20.11)
#define TS_SIDE_COL 92.0f          // dmSide_p (anchor topright, the iPad's places): the panel column's left edge from the screen's right edge
#define TS_SIDE_BOT 896.0f         // its stack's bottom (options 20..94 in the corner, the helmet 310..415 under the map, six panels from 422 every 80)
#define TS_SIDE_FOOT 62.0f         // screen px kept free under that stack (owner 23.11: the last button went into the screen's rounded corner; x1.196 -> x1.136 = 5 % smaller)
#define TS_PI_W 180.0f             // playerInfo_p (anchor topleft, class 1.1): the portraits' right edge (container_mc.x 25 + portrait + statuses)
#define TS_DIALOG_W 1160.0f        // dialog_p (anchor bottom, centred): the iPad's window 1160 x 400 (tools/dos2_ppanel_trade.py; the PC's was 1352 x 398)
#define TS_DIALOG_H 400.0f
#define TS_PI_BOT 720.0f          // the bottom of a party of four (estimate: 30 + 4 x ~170 x 1.1 ... measured on the phone, not static)
// ---- the panels (tools/dos2_ppanel_inv.py partyInventory_p: header INV_LIST_Y own px at its own scale <= TS_HEAD_S, then a bag header
// 56 + the rest 4 + rows x 56 own px at the cell's scale C / 50)
#define TS_INV_HEAD 202.0f
#define TS_INV_HEAD_S 1.19f        // the header's scale cap: 7 rows of 76 = the iPad's 968 px window (tools/dos2_ppanel_inv.py INV_HEAD_S; 1.52 before 21.11)
#define TS_INV_FIX 60.0f
#define TS_INV_PITCH 56.0f
#define TS_INV_BOT 27.0f           // the window's bottom edge under the list (may reach under the hot bar band)
#define TS_INV_W 770.0f
#define TS_INV_ROWS 5              // the least rows a sensible inventory shows
#define TS_CELL_MAX 160

typedef struct {
    float mm, aspect, w;           // inputs (mm <= 0: unknown -> factor 1, the iPad's numbers) and the design width 1080 x aspect
    float want;                    // the HUD factor that makes the hot bar cell TS_TARGET_MM
    float cap_row;                 // the hot bar row's fit in the width (the left strip TS_MARGIN_X, the right column)
    float hud;                     // hot bar, status console: min(want, cap_row, TS_HUD_MAX), never < 1
    float mmap;                    // the minimap: hud, at most TS_MM_MAX
    float side;                    // the right column: hud, at most what keeps its stack on the screen
    float pi;                      // the portraits: hud, less when a party of four would reach the hot bar's left end
    int bottom;                    // the panels' bottom (screen px): the frame top of the hot bar at factor 1 (open panels shrink it back) - gap
    int cell_want, cell_cap, cell; // the panels' cell px (dmSetCell): 8 mm, the most that shows TS_INV_ROWS rows above `bottom`, the result
    float menu, examine, tooltip, dialog, reward, splitter, log;   // window factors over their iPad class scale (1 = as designed)
    float hb_mm, cell_mm;          // what a finger gets: the hot bar cell and the panel cell in mm
} ts_scale;

static inline float ts_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
// the factor (>= 1) a centred window of w x h px (at factor 1) may take: inside the width less the side margins, between the top margin and
// `bottom`, about the screen centre (anchor center / screen / center); `want` is what the 8 mm target asks; `hi` a sanity cap
static inline float ts_fit_center(float want, float w, float h, float sw, float bottom, float hi) {
    float f = want;
    if (w > 0) f = fminf(f, (sw - 2 * TS_MARGIN_X) / w);
    if (h > 0) f = fminf(f, fminf((540.0f - TS_MARGIN_Y) / (h / 2), (bottom - 540.0f) / (h / 2)));
    return ts_clampf(fminf(f, hi), 1.0f, hi);
}
// the hot bar's top (screen y) at factor f, at its middle / ends
static inline float ts_hb_top(float f, float at) { return 1080.0f - at * f; }
// rows the inventory shows at cell c above `bottom` (the movie's own rule: tools/dos2_ppanel_inv.py inv_spec)
static inline int ts_inv_rows(int c, float bottom, float sw) {
    float s = c / 50.0f, av = (sw - 2 * TS_MARGIN_X) / TS_INV_W;
    if (s > av) s = av;
    const float sh = s < TS_INV_HEAD_S ? s : TS_INV_HEAD_S;
    const float by_bottom = bottom - TS_MARGIN_Y - TS_INV_HEAD * sh - TS_INV_FIX * s, by_screen = 1080.0f - TS_MARGIN_Y - TS_INV_HEAD * sh - (TS_INV_FIX + TS_INV_BOT) * s;
    return (int)floorf(fminf(by_bottom, by_screen) / (TS_INV_PITCH * s));
}

// mm: <= 0 unknown; aspect <= 0: 16:9. hud_given / cell_given: DM_DOS2_TOUCH_HUD_SCALE / DM_DOS2_TOUCH_CELL (testing: they win, unclamped
// by the fit; hud 0.5..2, cell 40..TS_CELL_MAX); 0 = not given.
static inline void ts_compute(float mm, float aspect, float hud_given, int cell_given, ts_scale *o) {
    if (!(aspect >= 1.0f && aspect <= 4.0f)) aspect = 16.0f / 9.0f;
    if (aspect < 16.0f / 9.0f) aspect = 16.0f / 9.0f;           // narrower: the game letterboxes itself to 16:9 (hle_dos2_wide.c)
    if (!(mm >= 20.0f && mm <= 400.0f)) mm = 0;
    o->mm = mm; o->aspect = aspect; o->w = 1080.0f * aspect;
    const float sw = o->w;
    o->want = mm > 0 ? TS_TARGET_MM * 1080.0f / (TS_HB_CELL * mm) : 1.0f;
    float pre = ts_clampf(fminf(o->want, TS_HUD_MAX), 1.0f, TS_HUD_MAX);
    if (hud_given > 0) pre = ts_clampf(hud_given, 0.5f, 2.0f);
    // the right column (its places fixed, growing from the corner): as the HUD, while its stack stays on the screen
    float side = fminf(pre, (1080.0f - TS_SIDE_FOOT) / TS_SIDE_BOT);
    o->side = side < 0.5f ? 0.5f : side;
    // the hot bar row (centred): between the left strip and the right column (the column reaches down beside the row)
    o->cap_row = fminf((sw / 2 - TS_MARGIN_X) / TS_HB_HALF, (sw / 2 - TS_SIDE_COL * o->side - TS_GAP) / TS_HB_HALF);
    float hud = ts_clampf(fminf(pre, o->cap_row), 1.0f, TS_HUD_MAX);
    if (hud_given > 0) hud = pre;
    o->hud = hud;
    o->mmap = hud < TS_MM_MAX ? hud : TS_MM_MAX;
    // the portraits: the same at the left end
    float pi = hud;
    if (sw / 2 - TS_HB_HALF * hud < TS_PI_W * pi + TS_GAP) pi = fminf(pi, (ts_hb_top(hud, TS_HB_TOP_L) - TS_GAP) / TS_PI_BOT);
    o->pi = pi < 0.5f ? 0.5f : pi;
    // panels: the hot bar is back at factor 1 while one is open
    o->bottom = (int)(ts_hb_top(1.0f, TS_HB_TOP) - 6.0f);
    o->cell_want = mm > 0 ? (int)lroundf(TS_TARGET_MM * 1080.0f / mm) : TS_CELL0;
    int cap = TS_CELL0;
    for (int c = TS_CELL0; c <= TS_CELL_MAX; c++) if (ts_inv_rows(c, (float)o->bottom, sw) >= TS_INV_ROWS && c / 50.0f <= (sw - 2 * TS_MARGIN_X) / TS_INV_W) cap = c;
    o->cell_cap = cap;
    int cell = o->cell_want < TS_CELL0 ? TS_CELL0 : o->cell_want > cap ? cap : o->cell_want;
    if (cell_given > 0) cell = cell_given < 40 ? 40 : cell_given > TS_CELL_MAX ? TS_CELL_MAX : cell_given;
    o->cell = cell;
    // windows: the 8 mm target over a 76 px design cell, as much as each one fits (boxes at factor 1 incl. the iPad class scale, centred unless said)
    const float ww = mm > 0 ? (float)o->cell_want / TS_CELL0 : 1.0f, b = (float)o->bottom;
    o->menu = ts_clampf(ww, 1.0f, 1.375f);                                        // contextMenu x 1.6: rows 30 -> 48 .. 66 px; placed at the finger by the engine
    o->examine = ts_fit_center(ww, 620, 947, sw, b, 1.5f);                         // examine_p 484 x 740 x 1.28 (centred)
    o->tooltip = ts_clampf(ww, 1.0f, 1.3f);                                       // tooltip: <= 400 wide, long ones ~700 high; the engine keeps it in the screen
    const float side_x = fmaxf(TS_MARGIN_X, TS_SIDE_COL * o->side + TS_GAP);      // the right column reaches the bottom: a bottom window keeps clear of it (both sides: centred)
    o->dialog = ts_clampf(fminf(ww, fminf((sw - 2 * side_x) / TS_DIALOG_W, (1080.0f - 2 * TS_MARGIN_Y) / TS_DIALOG_H)), 1.0f, 1.7f);   // dialog_p at the bottom
    o->reward = ts_fit_center(ww, 651, 798, sw, b, 1.5f);                          // reward_p
    o->splitter = ts_fit_center(ww, 638, 478, sw, b, 1.6f);                        // itemSplitter_p
    o->log = ts_clampf(fminf(hud, 1.3f), 1.0f, 1.3f);                              // combatLog_p (622 px PC window, centred)
    o->hb_mm = mm > 0 ? TS_HB_CELL * hud * mm / 1080.0f : 0;
    o->cell_mm = mm > 0 ? cell * mm / 1080.0f : 0;
}
#endif
