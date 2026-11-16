package com.xiaoboluo.d2r;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.SystemClock;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.Map;

// [TL2] Torchlight II touch layouts, modelled on D2R Mobile's touch controls (dm/ios/Sources/dm_touch_controls.c), not on D3's.
// D3PadView hands touches and drawing to this class while "d3_layout" is "tl2" (default) or "tl2_side"; the gear dialog, FrontMenu,
// opacity / size sliders stay D3PadView's. Output: the on-screen pad's controller channel (DmActivity.nativePadKey / nativePadAxes).
// TL2's Switch build reads the touch screen once at hid init (main 0x13ea04) and has no mouse path, so every touch becomes npad input.
// There is no menu mode: per-item menu touch waits for CEGUI mouse events (another branch); menus are driven with the buttons.
//
// Both layouts:
//   * move stick (left stick), D2R's floating base: a touch in the move zone (left 45% of the width below the top quarter, minus the
//     buttons) puts the base under the finger; axes from the finger relative to the touch point, full tilt at 60 dp, dead zone 6 dp;
//     nothing is drawn while no finger holds it (the stick appears under the finger and goes away on release). Diagonal pre-correction (D2R g_iso_fix): the vertical part times "tl2_iso" %, rescaled, so the screen angle of the
//     walk matches the finger's;
//   * no aim stick: TL2 on Switch uses the right stick only for the camera zoom (its Y axis), so the right side has a thin vertical zoom
//     slider instead (right edge above the fan; in tl2_side left of the cell columns): a finger on it drives the right stick's Y from
//     the drag relative to where it went down (up = stick up, full at 50 dp, dead 4 dp), held while the finger stays, back to 0 on
//     release - the game zooms continuously while the stick is held, like a held thumbstick;
//   * buttons win over both stick zones; a key held shorter than 35 ms is made up as a 100 ms pulse (the game samples once a frame).
// "tl2" (D2R fan): A (primary, large) with B X Y R around it and ZR above R (D2R's geometry), II = second skill set: the modifier
//   ("tl2_mod": ZL / ZR / L / R) goes down 170 ms before a skill key and is held only while a skill is in use (DM_TC_ZL_LEAD_S);
//   D2R's belt row at the bottom centre = d-pad left / up / down / right and "+"; "-" and R3 top right, ZL L L3 top left, gear on top.
// "tl2_side" (quick access on the sides, optional): every skill, potion and hotkey as its own cell along the screen edges; right edge:
//   A B X Y R ZR (set I) and next to it II+A .. II+ZR (modifier + key, the modifier leading by 170 ms); left edge: L ZL and the d-pad;
//   top centre: L3 - gear + R3.
// Optional, off by default (gear dialog): "tl2_tap" a quick tap on the world (outside the stick zones) = A (or turn toward the point,
//   then A); "tl2_hold_walk" touch and hold the world = walk toward the finger; "tl2_aim" dragging a fan skill aims with the right / left
//   stick (the aim stick does this better).
// Edit mode (DOOM's editor UX, frontend-common DoomPadView): toolbar − + Grouped/Separate Hide/Show Reset Cancel Save; drag a control,
//   a second finger resizes it, − / + with none chosen resize every group. Grouped (default): a control drags / resizes its whole group
//   (offset + scale "tl2_off_[side_]<group>" = "fx,fy" or "fx,fy,scale", fx / fy fractions of the view, the scale about the group's
//   default centre; a 2-field value from older builds still reads). Separate ("tl2_split" / "tl2_split_side"): every button on its own,
//   "tl2_btn_[side_]<name>" = "dx;dy;scale;hidden" (dx / dy fractions of the view inside its group, scale x the group's, hidden 0 / 1).
//   Hide / Show: the chosen button (Separate) or its group (Grouped) is neither drawn nor hit in game; the editor draws it dimmed and
//   struck through so it can be shown again (the gear never hides; the zoom slider hides as "tl2_btn_[side_]zoom" = "0;0;1;1").
//   Changes apply on Save; Cancel / Back drops them. The two stick zones are shown as outline there and drag like a group ("zmove")
//   from any spot inside it that is not a button; the zoom slider drags / resizes as "zoom". The editor itself is PadEditor (shared with
//   D3PadView's gamepad layout); Tl2Touch gives it the buttons plus the zoom slider and the move zone as zone items.
final class Tl2Touch {
    static final int K_A = KeyEvent.KEYCODE_ENTER, K_B = KeyEvent.KEYCODE_ESCAPE, K_X = KeyEvent.KEYCODE_R, K_Y = KeyEvent.KEYCODE_F,
        K_L = KeyEvent.KEYCODE_Q, K_R = KeyEvent.KEYCODE_E, K_ZL = KeyEvent.KEYCODE_SHIFT_LEFT, K_ZR = KeyEvent.KEYCODE_CTRL_LEFT,
        K_PLUS = KeyEvent.KEYCODE_TAB, K_MINUS = KeyEvent.KEYCODE_M, K_UP = KeyEvent.KEYCODE_DPAD_UP, K_DOWN = KeyEvent.KEYCODE_DPAD_DOWN,
        K_LEFT = KeyEvent.KEYCODE_DPAD_LEFT, K_RIGHT = KeyEvent.KEYCODE_DPAD_RIGHT, K_L3 = KeyEvent.KEYCODE_BUTTON_THUMBL, K_R3 = KeyEvent.KEYCODE_BUTTON_THUMBR;
    static final int[] MOD_KEYS = {K_ZL, K_ZR, K_L, K_R};
    static final String[] MOD_NAMES = {"ZL", "ZR", "L", "R"};
    static final int TAP_OFF = 0, TAP_A = 1, TAP_FACE_A = 2, AIM_OFF = 0, AIM_RIGHT = 1, AIM_LEFT = 2;
    static final int ISO_MIN = 100, ISO_MAX = 250, ISO_DEFAULT = 130, HERO_Y_MIN = 35, HERO_Y_MAX = 65, HERO_Y_DEFAULT = 50;

    // D2R timings (dm_touch_controls.h)
    static final long AIM_SETTLE_MS = 80, KEY_MIN_MS = 35, MOD_LEAD_MS = 170, TAP_MS = 350, HOLD_MS = 500, PULSE_MS = 100, FACE_MS = 70, STEP_GAP_MS = 40;
    static final float MOVE_DP = 10, AIM_DEAD_DP = 10, AIM_FULL_DP = 60, JOY_FULL_DP = 60, JOY_DEAD_DP = 6, WALK_DEAD_DP = 30, WALK_FULL_DP = 140, WALK_MIN_TILT = 0.4f;

    static final int KIND_MAIN = 0, KIND_SKILL = 1, KIND_PLAIN = 2, KIND_SWAP = 3, KIND_GEAR = 4, KIND_COMBO = 5,
        KIND_PANEL = 6, KIND_TAB = 7;   // [TL2] pointer on (DM_TL2_POINTER): D2R-style panel buttons (key = panel type) and radial tab arrows (key = -1 / +1)
    // panel types = RadialMenu CreatePanel 0x13dfa0 numbering (hle_tl2_ui.c): 0 inventory / equipment, 7 skills, 9 character stats, 12 quests
    // [TL2] icons of the skills / items bound to the buttons (hle_tl2_icons.c: the HUD action bar's slots as the game drew them; pref "tl2_icons",
    // default on). Slot button codes: 1 A, 3 B, 2 X, 0 Y, 8 R, 9 ZR, 10 L, 11 ZL.
    static native int nativeTl2IconSerial();
    static native int nativeTl2Icons(int[] argb, int[] meta);
    private static final int ICON = 64, NSLOT = 8;
    private final int[] iconPx = new int[NSLOT * ICON * ICON], iconMeta = new int[NSLOT * 4];
    private final android.graphics.Bitmap[] iconOf = new android.graphics.Bitmap[16];
    private final int[] iconState = new int[16];
    private int iconSerial = -2; private long iconPoll; private boolean icons = true;
    private final RectF iconDst = new RectF(), iconSrc = new RectF(0, 0, ICON, ICON);
    private final android.graphics.BitmapShader[] iconSh = new android.graphics.BitmapShader[16];
    private final android.graphics.Matrix iconM = new android.graphics.Matrix();
    private final Paint iconPaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private static int codeOf(int key) {
        switch (key) { case K_A: return 1; case K_B: return 3; case K_X: return 2; case K_Y: return 0; case K_R: return 8; case K_ZR: return 9; case K_L: return 10; case K_ZL: return 11; default: return -1; }
    }
    // the icon serial is polled twice a second on its own timer; a redraw only when the game's icons changed
    private final Runnable iconTick = new Runnable() { public void run() { if (!icons || editMode) return; if (pollIcons()) view.invalidate(); view.postDelayed(this, 500); } };
    private boolean pollIcons() {
        long t = now(); if (!icons || t < iconPoll) return false; iconPoll = t + 450;
        int s; try { s = nativeTl2IconSerial(); } catch (UnsatisfiedLinkError e) { icons = false; return false; }
        if (s < 0 || s == iconSerial) return false;
        iconSerial = nativeTl2Icons(iconPx, iconMeta);
        java.util.Arrays.fill(iconState, 0);
        for (int i = 0; i < NSLOT; i++) {
            int code = iconMeta[i * 4], st = iconMeta[i * 4 + 1];
            if (code < 0 || code >= 16) continue;
            iconState[code] = st;
            iconOf[code] = st == 1 ? android.graphics.Bitmap.createBitmap(iconPx, i * ICON * ICON, ICON, ICON, ICON, android.graphics.Bitmap.Config.ARGB_8888) : null;
            // drawn as a shader-filled circle / rounded square: no clip path per button per frame
            iconSh[code] = iconOf[code] == null ? null : new android.graphics.BitmapShader(iconOf[code], android.graphics.Shader.TileMode.CLAMP, android.graphics.Shader.TileMode.CLAMP);
        }
        return true;
    }
    private static final String[] PANEL_LABELS = {"INV", "SKL", "CHR", "QST"};
    private static final int[] PANEL_TYPES = {0, 7, 9, 12};
    // where a button's (bx, by) is measured from (k units = dp x size); "belt" x is in D2R belt cell pitches (40 k) from the centre
    static final int A_BR = 0, A_TR = 1, A_TL = 2, A_TOP = 3, A_BELT = 4, A_LEFT = 5, A_RIGHT = 6;
    // PadEditor.Item: name / group, drawn centre / radius x y r (px), default centre x0 y0, the player's place dx dy, size ps, hide (editor)
    private static final class Btn extends PadEditor.Item {
        final String label; final int key, kind, anchor; final float bx, by, br;
        Btn(String name, String label, int key, int kind, String group, int anchor, float bx, float by, float br) {
            super(name, group); this.label = label; this.key = key; this.kind = kind; this.anchor = anchor; this.bx = bx; this.by = by; this.br = br;
            fixed = kind == KIND_GEAR;
            defLabel = label != null && !label.isEmpty() ? label : null;   // [front] its name in PadEditor's "Add…" list (Rename… stays the Switch pad's)
        }
    }
    private static final int T_NONE = -1, T_JOY = -2, T_ZOOM = -3, T_WORLD = -4, T_MENU = -5;   // T_MENU: a console menu is up (DmActivity.nativeTl2MenuOpen), tap or not yet decided
    private static final int T_DRAG = -6;   // [TL2] a T_MENU finger that moved vertically on a scrollable menu list: scrolls it (nativeTl2UiDrag), no tap
    private static final class Finger { int target = T_NONE, zone; float x, y, x0, y0; long t0, keyT; boolean keyDown, cancelled, moved, walking; long seq; }
    private static final class Pulse { final int key; final long start, end; final float ax, ay; final boolean mod;
        Pulse(int key, long start, long end, float ax, float ay, boolean mod) { this.key = key; this.start = start; this.end = end; this.ax = ax; this.ay = ay; this.mod = mod; } }
    private static final class Stick { int ptr = -1; float tx, ty, bx, by; final RectF zone = new RectF(); final String group, name; Stick(String g, String n) { group = g; name = n; } }

    private final View view; private final Runnable openSettings;
    private final float dp;
    private final List<Btn> btns = new ArrayList<>();
    private final Map<Integer, Finger> fingers = new HashMap<>();
    private final List<Pulse> pulses = new ArrayList<>();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF cell = new RectF();
    private float k = 1, op = 1, iso = ISO_DEFAULT / 100f, heroFy = HERO_Y_DEFAULT / 100f, joyR;
    private int tapMode = TAP_OFF, aimMode = AIM_OFF, modKey = K_ZL; private String modName = "ZL"; private boolean holdWalk;
    private boolean side, setII, editMode, controllerMode, pointer;
    private int lw, lh; private long seqN;
    private final Stick move = new Stick("zmove", "MOVE");
    static final float ZOOM_FULL_DP = 50, ZOOM_DEAD_DP = 4;
    private final RectF zoom = new RectF(); private int zoomPtr = -1; private float zoomY0;   // camera zoom slider (right stick Y)
    private static final String ZOOM_GROUP = "zoom";
    // the editor (PadEditor) sees the buttons plus two zone items: the zoom slider (hideable, sized) and the move zone (moves only)
    private final PadEditor ed; private final PadEditor.Item zoomItem = new PadEditor.Item(ZOOM_GROUP, ZOOM_GROUP), moveItem = new PadEditor.Item("zmove", "zmove");
    private final List<PadEditor.Item> edItems = new ArrayList<>();
    private float joyMinX, joyMaxY;
    private final HashSet<Integer> sent = new HashSet<>();
    private float sLx, sLy, sRx, sRy; private boolean axesSent;
    private long faceUntil; private float faceX, faceY;

    Tl2Touch(View v, Runnable openSettings) {
        view = v; this.openSettings = openSettings; dp = v.getResources().getDisplayMetrics().density;
        line.setStyle(Paint.Style.STROKE); line.setStrokeWidth(2 * dp); text.setTextAlign(Paint.Align.CENTER);
        zoomItem.zone = zoom; moveItem.zone = move.zone; moveItem.fixed = moveItem.noScale = true;
        ed = new PadEditor(v, new PadEditor.Host() {
            public PadEditor.Item editHit(float x, float y) { Btn b = hit(x, y); return b != null ? b : zoomHit(x, y) ? zoomItem : moveZone(x, y) ? moveItem : null; }
            public void editChanged() { lw = -1; }
            public void editClosed(boolean saved) { edClosed(saved); }
        }, "tl2");
    }
    // name: the button's own prefs id ("tl2_btn_[side_]<name>"), stable across the modifier choice and the label
    private Btn add(String name, String label, int key, int kind, String group, int anchor, float bx, float by, float br) { Btn b = new Btn(name, label, key, kind, group, anchor, bx, by, br); btns.add(b); return b; }
    private void build() {
        btns.clear();
        if (!side) {   // D2R dm_tc_layout_make: main W - 86 + 12, H - 88 - 24 + 28
            final float mx = -74, my = -84;
            add("a", "A", K_A, KIND_MAIN, "fan", A_BR, mx, my, 40);
            // all eight HUD action-bar slots (A B X Y R ZR L ZL) around A, D2R style: an inner arc of four at 88 dp and an outer arc
            // of three at 150 dp (angles from the right, counter-clockwise; the thumb sweeps from lower left to top), II at the outer top
            String[] l = {"B", "X", "Y", "R", "ZL", "L", "ZR"}; int[] kk = {K_B, K_X, K_Y, K_R, K_ZL, K_L, K_ZR};
            float[] ang = {200, 155, 110, 70, 190, 158, 126}, rad = {88, 88, 88, 88, 150, 150, 150};
            for (int i = 0; i < l.length; i++) {
                double a = Math.toRadians(ang[i]);
                add(l[i].toLowerCase(Locale.ROOT), l[i], kk[i], KIND_SKILL, "fan", A_BR, mx + rad[i] * (float) Math.cos(a), my - rad[i] * (float) Math.sin(a), 26);
            }
            double ia = Math.toRadians(95);
            add("ii", "II", 0, KIND_SWAP, "fan", A_BR, mx + 150 * (float) Math.cos(ia), my - 150 * (float) Math.sin(ia), 22);
            add("r3", "R3", K_R3, KIND_PLAIN, "tr", A_TR, -30, 30, 19);
            add("l3", "L3", K_L3, KIND_PLAIN, "tr", A_TR, -74, 30, 19);   // kept (not a HUD slot), next to R3 instead of the top-left row
            // weapon swap = d-pad left: the console action table 0x230d968 binds TOGGLE_WEAPONS_BINDINGS to code 4 = BUTTON_SWITCH_DPAD_LEFT
            add("swap", "⇄", K_LEFT, KIND_PLAIN, "tr", A_TR, -118, 30, 19);
            add("gear", "⚙", 0, KIND_GEAR, "top", A_TOP, 0, 30, 20);
            // bottom centre: only "-" and "+" (the owner: the d-pad arrows are not needed with menu taps always on)
            add("minus", "-", K_MINUS, KIND_PLAIN, "belt", A_BELT, -0.7f, 0, 20); add("plus", "+", K_PLUS, KIND_PLAIN, "belt", A_BELT, 0.7f, 0, 20);
        } else {   // quick access along the edges: cells 50 dp, pitch 56 dp, from 40 dp below the top
            String[] l = {"A", "B", "X", "Y", "R", "ZR"}; int[] kk = {K_A, K_B, K_X, K_Y, K_R, K_ZR};
            for (int i = 0; i < 6; i++) {
                String n = l[i].toLowerCase(Locale.ROOT);
                add(n, l[i], kk[i], KIND_PLAIN, "right", A_RIGHT, -30, 40 + 56 * i, 25).square = true;
                add("c_" + n, modName + "+" + l[i], kk[i], KIND_COMBO, "right", A_RIGHT, -88, 40 + 56 * i, 25).square = true;
            }
            String[] ll = {"L", "ZL", "↑", "←", "→", "↓"}, ln = {"l", "zl", "up", "left", "right", "down"}; int[] lk = {K_L, K_ZL, K_UP, K_LEFT, K_RIGHT, K_DOWN};
            for (int i = 0; i < 6; i++) add(ln[i], ll[i], lk[i], KIND_PLAIN, "left", A_LEFT, 30, 40 + 56 * i, 25).square = true;
            // L and ZL are HUD slots too: their set-II cells next to them, like the right edge's
            add("c_l", modName + "+L", K_L, KIND_COMBO, "left", A_LEFT, 88, 40, 25).square = true;
            add("c_zl", modName + "+ZL", K_ZL, KIND_COMBO, "left", A_LEFT, 88, 96, 25).square = true;
            add("l3", "L3", K_L3, KIND_PLAIN, "top", A_TOP, -110, 30, 19); add("minus", "-", K_MINUS, KIND_PLAIN, "top", A_TOP, -55, 30, 22);
            add("gear", "⚙", 0, KIND_GEAR, "top", A_TOP, 0, 30, 20);
            add("plus", "+", K_PLUS, KIND_PLAIN, "top", A_TOP, 55, 30, 22); add("r3", "R3", K_R3, KIND_PLAIN, "top", A_TOP, 110, 30, 19);
        }
        if (pointer) {   // [TL2] D2R Mobile's panel buttons along the top (a second row in tl2_side) and the radial menu's tab arrows next to the gear
            float py = side ? 86 : 30, ax = side ? 165 : 75;
            for (int i = 0; i < PANEL_LABELS.length; i++)
                add(PANEL_LABELS[i].toLowerCase(Locale.ROOT), PANEL_LABELS[i], PANEL_TYPES[i], KIND_PANEL, "panels", A_TOP, i < 2 ? -240 + 50 * i : 140 + 50 * (i - 2), py, 19);
            add("tab_l", "◀", -1, KIND_TAB, "panels", A_TOP, -ax, py, 18);
            add("tab_r", "▶", 1, KIND_TAB, "panels", A_TOP, ax, py, 18);
        }
        lw = -1;
    }

    void reload(Context ui, SharedPreferences p, float opacity, float scale, boolean sideLayout) {
        releaseAll();
        op = opacity; k = dp * scale; side = sideLayout;
        iso = clamp(p.getInt("tl2_iso", ISO_DEFAULT), ISO_MIN, ISO_MAX) / 100f;
        heroFy = clamp(p.getInt("tl2_hero_y", HERO_Y_DEFAULT), HERO_Y_MIN, HERO_Y_MAX) / 100f;
        tapMode = clamp(p.getInt("tl2_tap", TAP_OFF), 0, 2);
        aimMode = clamp(p.getInt("tl2_aim", AIM_OFF), 0, 2);
        int m = clamp(p.getInt("tl2_mod", 0), 0, MOD_KEYS.length - 1); modKey = MOD_KEYS[m]; modName = MOD_NAMES[m];
        holdWalk = p.getBoolean("tl2_hold_walk", false);
        pointer = true;   // [TL2] menu taps are always on (the owner: convenient and works); the gear option was removed
        icons = p.getBoolean("tl2_icons", true); iconSerial = -2; iconPoll = 0;
        build();
        edItems.clear(); edItems.addAll(btns); edItems.add(moveItem); edItems.add(zoomItem); ed.setItems(edItems);
        ed.configure(ui, "tl2_off_" + (side ? "side_" : ""), "tl2_btn_" + (side ? "side_" : ""), side ? "tl2_split_side" : "tl2_split",
            R.string.pad_ed_hint_groups, R.string.pad_ed_hint_split);
        ed.load(p); lw = -1;   // group offsets "fx,fy[,scale]", every button's "dx;dy;scale;hidden", the zoom slider's hide, Separate
        view.invalidate();
    }
    private static int clamp(int v, int lo, int hi) { return Math.max(lo, Math.min(hi, v)); }

    // ---------------- layout ----------------
    private void layout() {
        int w = view.getWidth(), h = view.getHeight();
        if (w == lw && h == lh) return;
        lw = w; lh = h;
        for (Btn b : btns) {   // default centres; PadEditor.place adds the player's place, size and the group's scale / offset
            float x, y;
            switch (b.anchor) {
            case A_BR: x = w + b.bx * k; y = h + b.by * k; break;
            case A_TR: case A_RIGHT: x = w + b.bx * k; y = b.by * k; break;
            case A_TOP: x = w / 2f + b.bx * k; y = b.by * k; break;
            case A_BELT: x = w / 2f + b.bx * 40 * k; y = h - 22 * k; break;   // D2R: pitch 120 / 1320 of the height, 78.5 / 1320 above the bottom
            default: x = b.bx * k; y = b.by * k; break;
            }
            b.x0 = x; b.y0 = y; b.r0 = b.br * k;
        }
        ed.place(w, h);   // own place inside the group, then the group's scale about its default centre, then the group's offset
        joyR = 60 * k; joyMinX = joyR + 8 * k; joyMaxY = h - joyR - 8 * k;
        zone(move, 0, 0.25f, 0.45f, 1, w, h);   // floating sticks: where a finger that hits no button grabs them
        {   // zoom slider: tl2 = right edge between R3 / L3 and the fan's top (II at about h - 240 k); tl2_side = left of the cell columns
            float cx = side ? w - 140 * k : w - 18 * k, top = side ? 40 * k : 70 * k, bot = side ? 40 * k + 56 * 5 * k : Math.max(top + 90 * k, h - 260 * k);
            float[] o = ed.off(ZOOM_GROUP); float ox = o == null ? 0 : o[0] * w, oy = o == null ? 0 : o[1] * h, zs = o == null ? 1 : o[2];
            float mid = (top + bot) / 2, half = (bot - top) / 2 * zs;   // the editor's size: width and length about its default centre
            zoom.set(cx - 9 * k * zs + ox, mid - half + oy, cx + 9 * k * zs + ox, mid + half + oy);
        }
    }
    private void zone(Stick s, float l, float t, float r, float b, int w, int h) {
        float[] o = ed.off(s.group); float ox = o == null ? 0 : o[0] * w, oy = o == null ? 0 : o[1] * h;
        s.zone.set(l * w + ox, t * h + oy, r * w + ox, b * h + oy);
    }
    // hidden (editor's Hide): neither drawn nor hit in game; the editor shows (and hits) it dimmed so it can be shown again
    private boolean shown(Btn b) { return editMode || b.kind == KIND_GEAR || (!controllerMode && !b.hidden); }
    private Btn hit(float x, float y) {
        Btn best = null; float bd = Float.MAX_VALUE;
        for (Btn b : btns) {
            if (!shown(b)) continue;
            float dx = x - b.x, dy = y - b.y, r = b.r * (b.square ? 1.1f : 1.15f), d = dx * dx + dy * dy;
            boolean in = b.square ? Math.abs(dx) <= r && Math.abs(dy) <= r : d <= r * r;
            if (in && d < bd) { best = b; bd = d; }
        }
        return best;
    }
    private Btn btnOf(Finger f) { return f.target >= 0 && f.target < btns.size() ? btns.get(f.target) : null; }
    private boolean inMinus(float x, float y) { for (Btn b : btns) if (b.key == K_MINUS && !b.hidden) return Math.abs(x - b.x) <= b.r && Math.abs(y - b.y) <= b.r; return false; }
    private boolean moveZone(float x, float y) { return move.zone.contains(x, y); }
    private boolean zoomHit(float x, float y) { return !controllerMode && (editMode || !zoomItem.hidden) && x >= zoom.left - 14 * k && x <= zoom.right + 14 * k && y >= zoom.top - 10 * k && y <= zoom.bottom + 10 * k; }
    private float zoomAxis(Finger f) {   // drag up = right stick up (negative Y, Android axis convention as nativePadAxes gets it)
        float d = (f.y - zoomY0) / dp, a = Math.abs(d);
        if (a <= ZOOM_DEAD_DP) return 0;
        return Math.signum(d) * Math.min(1f, (a - ZOOM_DEAD_DP) / (ZOOM_FULL_DP - ZOOM_DEAD_DP));
    }

    void setControllerMode(boolean on) { controllerMode = on; if (on) releaseAll(); view.invalidate(); }

    // ---------------- output ----------------
    private static void key(int k, boolean down) { DmActivity.nativePadKey(D3PadView.PAD_DEVICE, k, down); }
    private final Runnable tick = this::update;
    private static long now() { return SystemClock.uptimeMillis(); }
    private void pulse(int key, long delay, float ax, float ay, boolean mod) {
        long t = now() + delay;
        for (Pulse p : pulses) if (p.key == key) t = Math.max(t, p.end + STEP_GAP_MS);   // the same key again: after a release the game sees
        pulses.add(new Pulse(key, t, t + PULSE_MS, ax, ay, mod));
    }
    private float[] stickAxes(Stick s, Finger f) {   // D2R update(): relative to the touch point, dead 6 dp, full at 60 dp, corrected diagonal
        float dx = f.x - s.tx, dy = f.y - s.ty, d = (float) Math.hypot(dx, dy), dead = JOY_DEAD_DP * dp, full = JOY_FULL_DP * dp;
        if (d <= dead) return new float[] {0, 0};
        return isoFix(dx / d, dy / d, Math.min(1, (d - dead) / (full - dead)));
    }
    private float[] isoFix(float ex, float ey, float s) { float n = (float) Math.hypot(ex, iso * ey); return new float[] {ex / n * s, iso * ey / n * s}; }
    private float[] walkAxes(float x, float y) {
        float hx = view.getWidth() * 0.5f, hy = view.getHeight() * heroFy, dx = x - hx, dy = y - hy, d = (float) Math.hypot(dx, dy) / dp;
        if (d < WALK_DEAD_DP) return new float[] {0, 0};
        float m = WALK_MIN_TILT + (1 - WALK_MIN_TILT) * Math.min(1f, (d - WALK_DEAD_DP) / (WALK_FULL_DP - WALK_DEAD_DP)), dd = d * dp;
        return isoFix(dx / dd, dy / dd, m);
    }
    private float dist(Finger f) { return (float) Math.hypot(f.x - f.x0, f.y - f.y0); }

    // Recomputes every held key and both sticks from the fingers and pulses, sends what changed, and keeps ticking while anything is timed.
    private void update() {
        view.removeCallbacks(tick);
        if (controllerMode || editMode) return;
        long t = now();
        HashSet<Integer> keys = new HashSet<>();
        boolean timed = false, modUse = false, lSet = false, rSet = false;
        float lx = 0, ly = 0, rx = 0, ry = 0;
        Finger dragAim = null;
        for (Finger f : fingers.values()) {
            Btn b = btnOf(f);
            if (b == null || f.cancelled) continue;
            if (b.kind == KIND_PLAIN) { keys.add(b.key); continue; }
            if (b.kind == KIND_COMBO) { modUse = true; if (t - f.t0 >= MOD_LEAD_MS) keys.add(b.key); else timed = true; continue; }
            if (b.kind != KIND_MAIN && b.kind != KIND_SKILL) continue;
            if (b.kind == KIND_SKILL && !f.keyDown) {
                if ((aimMode != AIM_OFF && dist(f) > AIM_DEAD_DP * dp) || t - f.t0 >= AIM_SETTLE_MS) { f.keyDown = true; f.keyT = t; } else timed = true;
            }
            if (setII) { modUse = true; if (t - f.t0 < MOD_LEAD_MS) { timed = true; continue; } }
            if (f.keyDown || b.kind == KIND_MAIN) keys.add(b.key);
            if (aimMode != AIM_OFF && dist(f) > AIM_DEAD_DP * dp && (dragAim == null || f.seq > dragAim.seq)) dragAim = f;
        }
        for (Iterator<Pulse> it = pulses.iterator(); it.hasNext();) {
            Pulse p = it.next();
            if (t >= p.end) { it.remove(); continue; }
            timed = true;
            if (p.mod) modUse = true;
            if (t >= p.start) { keys.add(p.key); if (p.ax != 0 || p.ay != 0) { if (aimMode == AIM_LEFT) { lx = p.ax; ly = p.ay; lSet = true; } else { rx = p.ax; ry = p.ay; rSet = true; } } }
        }
        if (modUse) keys.add(modKey);
        if (dragAim != null) {
            float ax = Math.max(-1, Math.min(1, (dragAim.x - dragAim.x0) / (AIM_FULL_DP * dp))), ay = Math.max(-1, Math.min(1, (dragAim.y - dragAim.y0) / (AIM_FULL_DP * dp)));
            if (aimMode == AIM_LEFT) { lx = ax; ly = ay; lSet = true; } else { rx = ax; ry = ay; rSet = true; }
        }
        Finger fz = zoomPtr >= 0 ? fingers.get(zoomPtr) : null;   // the zoom slider wins over a skill drag on the right stick
        if (fz != null) { rx = 0; ry = zoomAxis(fz); }
        Finger fm = move.ptr >= 0 ? fingers.get(move.ptr) : null;   // left stick: the move stick, then walking toward a held world finger, then a nudge
        if (fm != null) { float[] v = stickAxes(move, fm); lx = v[0]; ly = v[1]; lSet = true; }
        for (Finger f : fingers.values()) {
            if (f.target != T_WORLD || !holdWalk) continue;
            if (!f.walking && t - f.t0 >= HOLD_MS) f.walking = true;
            if (f.walking && !lSet) { float[] v = walkAxes(f.x, f.y); lx = v[0]; ly = v[1]; lSet = true; }
            if (!f.walking) timed = true;
        }
        if (!lSet && t < faceUntil) { lx = faceX; ly = faceY; timed = true; }
        for (Integer kc : new ArrayList<>(sent)) if (!keys.contains(kc)) { key(kc, false); sent.remove(kc); }
        for (Integer kc : keys) if (!sent.contains(kc)) { key(kc, true); sent.add(kc); }
        if (!axesSent || lx != sLx || ly != sLy || rx != sRx || ry != sRy) {
            sLx = lx; sLy = ly; sRx = rx; sRy = ry; axesSent = true;
            DmActivity.nativePadAxes(D3PadView.PAD_DEVICE, lx, ly, rx, ry, 0, 0, 0, 0);
        }
        // a held finger alone changes nothing between touch events: 16 ms only while something is timed, else a slow safety tick
        if (timed) view.postDelayed(tick, 16); else if (!fingers.isEmpty()) view.postDelayed(tick, 250);
        int sig = shownSig();   // redraw (the whole window is composed again) only when what is drawn changed
        if (sig != drawnSig) { drawnSig = sig; view.invalidate(); }
    }
    // What draw() shows of the fingers: the held buttons, set II, the stick knob, the zoom knob and the walk rings
    private int drawnSig;
    private int shownSig() {
        int s = setII ? 1 : 0;
        for (Map.Entry<Integer, Finger> en : fingers.entrySet()) {
            Finger f = en.getValue(); int id = en.getKey(), h = 0;
            if (f.target >= 0 && !f.cancelled) h = 31 * h + f.target + 7;
            if (id == move.ptr || id == zoomPtr || (f.target == T_WORLD && f.walking)) h = 31 * (31 * (31 * h + Math.round(f.x)) + Math.round(f.y)) + id + 1;
            s += h * 0x9e3779b1;
        }
        return s;
    }
    private void worldTap(float x, float y) {
        if (tapMode == TAP_OFF) return;
        long delay = 0;
        if (tapMode == TAP_FACE_A) {
            float[] v = walkAxes(x, y); float m = (float) Math.hypot(v[0], v[1]);
            if (m > 0) { faceX = v[0] / m * 0.35f; faceY = v[1] / m * 0.35f; faceUntil = now() + FACE_MS; delay = FACE_MS - 20; }
        }
        pulse(K_A, delay, 0, 0, false);
    }

    // ---------------- touches ----------------
    boolean onTouch(MotionEvent e) {
        layout();
        int a = e.getActionMasked(), i = e.getActionIndex(), id = e.getPointerId(i);
        float x = e.getX(i), y = e.getY(i);
        if (editMode) { editTouch(e); return true; }
        if (controllerMode) {
            if (a == MotionEvent.ACTION_DOWN || a == MotionEvent.ACTION_POINTER_DOWN) { Btn b = hit(x, y); if (b != null && b.kind == KIND_GEAR) openSettings.run(); }
            return true;
        }
        switch (a) {
        case MotionEvent.ACTION_DOWN: case MotionEvent.ACTION_POINTER_DOWN: begin(id, x, y); break;
        case MotionEvent.ACTION_MOVE: for (int n = 0; n < e.getPointerCount(); n++) move(e.getPointerId(n), e.getX(n), e.getY(n)); break;
        case MotionEvent.ACTION_POINTER_UP: end(id, false); break;
        case MotionEvent.ACTION_UP: for (Integer p : new ArrayList<>(fingers.keySet())) end(p, false); break;
        case MotionEvent.ACTION_CANCEL: for (Integer p : new ArrayList<>(fingers.keySet())) end(p, true); break;
        }
        update();
        return true;
    }
    private void grab(Stick s, int id, float x, float y) {
        s.ptr = id; s.tx = x; s.ty = y; s.bx = Math.min(Math.max(x, joyMinX), view.getWidth() - joyMinX); s.by = Math.min(y, joyMaxY);
    }
    // [TL2] map tap (hle_tl2_map.c): a finger that lands on the shown minimap / big map waits like a menu tap (T_MENU above); a short still
    // tap then goes to nativeTl2UiTap, which with no menu up switches minimap <-> big map; moved, it becomes the stick / world finger as usual
    private boolean mapHit(float x, float y) { int w = view.getWidth(), h = view.getHeight(); return w > 0 && h > 0 && DmActivity.nativeTl2MapHit(x / w, y / h); }
    private boolean listHit(float x, float y) { int w = view.getWidth(), h = view.getHeight(); return w > 0 && h > 0 && DmActivity.nativeTl2UiListHit(x / w, y / h); }
    private void drag(Finger f, int phase) { int w = view.getWidth(), h = view.getHeight(); if (w > 0 && h > 0) DmActivity.nativeTl2UiDrag(f.x0 / w, f.y0 / h, (f.y - f.y0) / h, phase); }
    private void begin(int id, float x, float y) {
        if (fingers.containsKey(id)) return;
        Finger f = new Finger(); f.x = f.x0 = x; f.y = f.y0 = y; f.t0 = now();
        Btn b = hit(x, y);
        if (b != null && b.kind == KIND_GEAR) { openSettings.run(); return; }
        if (b != null && b.kind == KIND_SWAP) { setII = !setII; return; }
        if (b != null && b.kind == KIND_PANEL) { DmActivity.nativeTl2OpenPanel(b.key); return; }   // [TL2] hle_tl2_ui.c: the game's own + / tab / segment / Accept
        if (b != null && b.kind == KIND_TAB) { DmActivity.nativeTl2UiTab(b.key); return; }        // [TL2] the radial menu's own L / R tab code
        if (b != null) f.target = btns.indexOf(b);
        else if (zoomPtr < 0 && zoomHit(x, y)) { f.target = T_ZOOM; zoomPtr = id; zoomY0 = y; }
        // [TL2] pointer (hle_tl2_ui.c, gear "tap on menu items", DM_TL2_POINTER): while a console menu is up a finger off the overlay buttons
        // waits: a short still tap goes to the menu item under it; moving it first makes it the stick / world finger it would have been
        else if (DmActivity.nativeTl2MenuOpen() || mapHit(x, y)) { f.target = T_MENU; f.zone = move.ptr < 0 && moveZone(x, y) ? T_JOY
                                                  : y > 60 * k && !moveZone(x, y) ? T_WORLD : T_NONE; }
        else if (move.ptr < 0 && moveZone(x, y)) { f.target = T_JOY; grab(move, id, x, y); }
        else if (y > 60 * k && !moveZone(x, y)) f.target = T_WORLD;
        fingers.put(id, f);
    }
    private void move(int id, float x, float y) {
        Finger f = fingers.get(id); if (f == null) return;
        f.x = x; f.y = y;
        if (dist(f) > MOVE_DP * dp) f.moved = true;
        // [TL2] hle_tl2_ui.c list scroll: a mostly vertical move from a scrollable list scrolls it (rows follow the finger); release = no tap
        if (f.target == T_MENU && f.moved && Math.abs(f.y - f.y0) > Math.abs(f.x - f.x0) && listHit(f.x0, f.y0)) { f.target = T_DRAG; drag(f, 0); }
        if (f.target == T_DRAG) { drag(f, 1); return; }
        if (f.target == T_MENU && f.moved) {   // not a tap: the finger becomes what it would have been without the menu, from where it went down
            if (f.zone == T_JOY && move.ptr < 0) { f.target = T_JOY; grab(move, id, f.x0, f.y0); }
            else f.target = f.zone == T_WORLD ? T_WORLD : T_NONE;
        }
        Btn b = btnOf(f);
        if (b != null && aimMode != AIM_OFF && (b.kind == KIND_MAIN || b.kind == KIND_SKILL)) {
            if (inMinus(x, y)) f.cancelled = true;   // D2R: dragging a skill onto the map button cancels it
            if (!f.cancelled) f.seq = ++seqN;
        } else if (f.target == T_WORLD && f.moved && holdWalk) f.walking = true;
    }
    private void end(int id, boolean cancel) {
        Finger f = fingers.remove(id); if (f == null) return;
        if (id == move.ptr) move.ptr = -1;
        if (id == zoomPtr) zoomPtr = -1;
        if (f.target == T_DRAG) { drag(f, 2); return; }
        if (cancel) return;
        long t = now(), held = t - f.t0;
        Btn b = btnOf(f);
        if (b != null && !f.cancelled) {
            if (b.kind == KIND_SKILL && (!f.keyDown || t - f.keyT < KEY_MIN_MS || (setII && held < MOD_LEAD_MS))) {
                float ax = 0, ay = 0, d = dist(f);   // D2R dm_tc_end_at: a tap too short for the key becomes a pulse (with the drag's direction)
                if (d > AIM_DEAD_DP * dp && aimMode != AIM_OFF) { ax = (f.x - f.x0) / d; ay = (f.y - f.y0) / d; }
                pulse(b.key, setII ? Math.max(0, MOD_LEAD_MS - held) : 0, ax, ay, setII);
            } else if (b.kind == KIND_MAIN && (held < KEY_MIN_MS || (setII && held < MOD_LEAD_MS))) pulse(b.key, setII ? Math.max(0, MOD_LEAD_MS - held) : 0, 0, 0, setII);
            else if (b.kind == KIND_COMBO && held < MOD_LEAD_MS + KEY_MIN_MS) pulse(b.key, Math.max(0, MOD_LEAD_MS - held), 0, 0, true);
            else if (b.kind == KIND_PLAIN && held < KEY_MIN_MS) pulse(b.key, 0, 0, 0, false);
        }
        if (f.target == T_WORLD && held < TAP_MS && dist(f) <= MOVE_DP * dp) worldTap(f.x0, f.y0);
        if (f.target == T_MENU && held < TAP_MS && dist(f) <= MOVE_DP * dp) {
            // the view is the whole game picture (TL2 widescreen and 16:9 both fill it; menus are never on the pillarboxed loading screen)
            int w = view.getWidth(), h = view.getHeight();
            boolean took = w > 0 && h > 0 && DmActivity.nativeTl2UiTap(f.x0 / w, f.y0 / h);
            if (!took && f.zone == T_WORLD) worldTap(f.x0, f.y0);
        }
    }
    void releaseAll() {
        fingers.clear(); pulses.clear(); move.ptr = zoomPtr = -1; faceUntil = 0;
        view.removeCallbacks(tick);
        for (Integer kc : sent) key(kc, false);
        sent.clear();
        if (axesSent && (sLx != 0 || sLy != 0 || sRx != 0 || sRy != 0) && !controllerMode) DmActivity.nativePadAxes(D3PadView.PAD_DEVICE, 0, 0, 0, 0, 0, 0, 0, 0);
        sLx = sLy = sRx = sRy = 0;
        view.invalidate();
    }

    // ---------------- edit mode ----------------
    // DOOM's layout editor (PadEditor: frontend-common DoomPadView's gestures and toolbar look): drag a control; a second finger resizes it;
    // toolbar − + Grouped/Separate Hide/Show Reset Cancel Save. Grouped moves / resizes the chosen control's whole group, Separate the
    // chosen button alone (inside its group). − / + with nothing chosen resize every group. Nothing is written before Save.
    private Runnable editDone;
    boolean editing() { return editMode; }
    void startEdit(Runnable done) {
        releaseAll(); editDone = done; editMode = true; ed.start();
        android.util.Log.i("dm", "[java] tl2 layout editor open (" + (side ? "tl2_side" : "tl2") + ")");
        view.invalidate();
    }
    // save: write every place / size / hide (Save); else back to what it was when the editor opened (Cancel, Back)
    void endEdit(boolean save) { if (editMode) ed.close(save); }
    private void edClosed(boolean save) {
        editMode = false; lw = -1;
        android.util.Log.i("dm", "[java] tl2 layout editor " + (save ? "saved" : "cancelled"));
        Runnable d = editDone; editDone = null; view.invalidate(); if (d != null) d.run();
    }
    void resetLayout() {   // FrontMenu "Reset layout": every place, size and hide back to the defaults, saved at once
        ed.clear(); ed.save(); lw = -1;
        view.getContext().getSharedPreferences("doom_controls", Context.MODE_PRIVATE).edit().remove("tl2_off_" + (side ? "side_" : "") + "zaim").apply();
        view.invalidate();
    }
    private void editTouch(MotionEvent e) { ed.touch(e); }

    // ---------------- drawing ----------------
    void draw(Canvas c) {
        layout();
        pollIcons();
        if (icons && !editMode) { view.removeCallbacks(iconTick); view.postDelayed(iconTick, 500); }
        drawnSig = shownSig();
        if (editMode) c.drawColor(0x50000000);
        if (!controllerMode) {
            drawStick(c, move); drawZoom(c);
            for (Finger f : fingers.values()) if (f.target == T_WORLD && f.walking) { line.setColor(0xffffffff); line.setAlpha(Math.round(0x90 * op)); c.drawCircle(f.x, f.y, 22 * dp, line); }
        }
        for (Btn b : btns) if (shown(b)) drawBtn(c, b);
        if (editMode) ed.draw(c);
    }
    private void drawStick(Canvas c, Stick s) {
        Finger f = s.ptr >= 0 ? fingers.get(s.ptr) : null;
        if (f != null) {   // the base where the thumb landed, the knob = base + axes x radius
            float[] v = stickAxes(s, f);
            fill.setColor(0xffffffff); fill.setAlpha(Math.round(0x28 * op)); c.drawCircle(s.bx, s.by, joyR, fill);
            line.setColor(0xffffffff); line.setAlpha(Math.round(0x90 * op)); c.drawCircle(s.bx, s.by, joyR, line);
            fill.setAlpha(Math.round(0x80 * op)); c.drawCircle(s.bx + v[0] * joyR, s.by + v[1] / Math.max(1f, iso) * joyR, joyR * 0.42f, fill);
        } else if (editMode) {   // idle: nothing in play; the editor shows the zone a finger grabs the stick in
            line.setColor(0xffffd040); line.setAlpha(0xc0); c.drawRoundRect(s.zone, 12 * dp, 12 * dp, line);
            text.setTextSize(13 * dp); text.setColor(0xffffd040); text.setAlpha(0xc0);
            c.drawText(s.name, s.zone.centerX(), s.zone.centerY(), text);
        }
    }
    private void drawZoom(Canvas c) {   // thin track with + / - ends; while held a knob at the finger and the bar filled toward the drag
        if (zoomItem.hidden && !editMode) return;
        Finger f = zoomPtr >= 0 ? fingers.get(zoomPtr) : null;
        float kk = editMode ? ed.alpha(zoomItem, op) : op, rr = zoom.width() / 2;
        fill.setColor(0xffffffff); fill.setAlpha(Math.round((f != null ? 0x50 : 0x22) * kk)); c.drawRoundRect(zoom, rr, rr, fill);
        line.setColor(editMode ? 0xffffd040 : 0xffffffff); line.setAlpha(editMode ? 0xe0 : Math.round(0x80 * kk)); c.drawRoundRect(zoom, rr, rr, line);
        float ts = Math.max(10 * dp, 1.1f * rr * 2); text.setTextSize(ts); text.setColor(0xffffffff); text.setAlpha(Math.round(0xd0 * kk));
        c.drawText("+", zoom.centerX(), zoom.top + rr + ts * 0.35f, text); c.drawText("−", zoom.centerX(), zoom.bottom - rr + ts * 0.35f, text);
        if (f != null) {
            float ky = Math.max(zoom.top + rr, Math.min(zoom.bottom - rr, f.y));
            fill.setAlpha(Math.round(0xa0 * op)); c.drawCircle(zoom.centerX(), ky, rr * 1.6f, fill);
        }
        if (editMode && zoomItem.hidden) ed.strike(c, zoom.centerX(), zoom.centerY(), Math.max(rr * 2, 24 * dp));
    }
    private boolean held(Btn b) {
        if (b.kind == KIND_SWAP) return setII;
        for (Finger f : fingers.values()) if (f.target >= 0 && btns.get(f.target) == b && !f.cancelled) return true;
        return false;
    }
    private void drawBtn(Canvas c, Btn b) {
        drawBtnBody(c, b);
        if (editMode && b.hidden) ed.strike(c, b.x, b.y, b.r);
    }
    private void drawBtnBody(Canvas c, Btn b) {
        boolean on = held(b);
        float kk = b.kind == KIND_GEAR ? 1f : (editMode ? ed.alpha(b, op) : op);
        int tint = b.kind == KIND_SWAP && setII ? 0xffffb030 : b.kind == KIND_COMBO ? 0xffffd890 : 0xffffffff;
        fill.setColor(tint); fill.setAlpha(Math.round((on ? 0x80 : 0x30) * kk));
        line.setColor(editMode ? 0xffffd040 : tint); line.setAlpha(editMode ? 0xe0 : Math.round(0x90 * kk));
        if (b.square) { cell.set(b.x - b.r, b.y - b.r, b.x + b.r, b.y + b.r); c.drawRoundRect(cell, 8 * dp, 8 * dp, fill); c.drawRoundRect(cell, 8 * dp, 8 * dp, line); }
        else { c.drawCircle(b.x, b.y, b.r, fill); c.drawCircle(b.x, b.y, b.r, line); }
        int code = icons && b.kind != KIND_PANEL && b.kind != KIND_TAB && b.kind != KIND_GEAR && b.kind != KIND_SWAP ? codeOf(b.key) : -1;
        android.graphics.Bitmap ic = code >= 0 ? iconOf[code] : null;
        boolean empty = code >= 0 && iconState[code] == 2;
        if (ic != null) {   // the game's own icon inside the button, the button letter as a small badge
            float ir = b.r * 0.86f;
            iconDst.set(b.x - ir, b.y - ir, b.x + ir, b.y + ir);
            iconM.setRectToRect(iconSrc, iconDst, android.graphics.Matrix.ScaleToFit.FILL); iconSh[code].setLocalMatrix(iconM);
            iconPaint.setShader(iconSh[code]); iconPaint.setAlpha(on ? 0xff : Math.round(0xff * Math.max(kk, 0.85f)));   // the icon stays readable at low overlay opacity
            if (b.square) c.drawRoundRect(iconDst, 6 * dp, 6 * dp, iconPaint); else c.drawCircle(b.x, b.y, ir, iconPaint);
            float bs = Math.max(9 * dp, 0.32f * b.r); text.setTextSize(bs); text.setColor(0xffffffff); text.setAlpha(Math.round(0xe0 * kk));
            float bx = b.x + b.r * 0.55f, by = b.y + b.r * 0.62f;
            fill.setColor(0xa0000000); c.drawCircle(bx, by - bs * 0.35f, bs * 0.85f, fill);
            c.drawText(b.label.length() > 3 ? b.label.substring(b.label.length() - 2) : b.label, bx, by, text);
            return;
        }
        float ts = Math.max(11 * dp, 0.6f * b.r); text.setTextSize(ts);
        float w = text.measureText(b.label); if (w > 1.7f * b.r) { ts *= 1.7f * b.r / w; text.setTextSize(ts); }
        text.setColor(0xffffffff); text.setAlpha(Math.round((empty ? 0x70 : 0xd8) * kk));   // nothing bound to that button in the game: dim
        c.drawText(b.label, b.x, b.y + ts * 0.35f, text);
    }
}
