package com.xiaoboluo.d2r;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Picture;
import android.graphics.RectF;
import android.os.SystemClock;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

// [D3] On-screen controls of Diablo III (DmActivity creates this view instead of DoomPadView when D3). Two layouts, picked in the gear
// dialog ("Control layout", pref doom_controls "d3_layout"):
//   gamepad ("gamepad", default): the Joy-Con-like pad of commit abf0b6a, unchanged in behaviour: floating left stick + d-pad below,
//     A/B/X/Y + right stick (D3: evade toward the tilt) below them, L/ZL R/ZR in the top corners, "-" ⚙ "+" on top; a short tap on a
//     stick's knob is L3 / R3.
//   touch ("touch"): phase 1 of docs/d3/TOUCH_DESIGN.md (front end only, no game hooks):
//     * floating left stick in the lower left (tap on its home knob = L3);
//     * touch and hold the world (any free spot below the top strip) = walk toward that point: the left stick points from the hero's
//       on-screen spot (HERO_FX, "d3_hero_y") to the finger, the vertical part divided by the camera's depth squeeze ("d3_depth");
//       tilt grows with the distance (WORLD_*); the move stick wins over a world touch;
//     * double tap in the world = evade toward the point: a right-stick flick of EVADE_MS (D3 console "ControllerLayoutFlickEvade");
//     * six skill buttons for the right thumb (A primary, large, in the corner; B Y X R ZR on an arc around it) and the potion (L) left of
//       the arc; ZL and a small d-pad top left, "-" (inventory / map) ⚙ "+" (menu) on top, R3 and the MENU toggle top right;
//     * MENU toggle (manual menu mode; automatic detection is phase 2): skills, A/X/Y, R3 and the stick hide; a swipe anywhere drives the
//       left stick (or the d-pad, "d3_menu_swipe") toward the swipe, a tap = A, a long press = hold A; B, L/R, ZL/ZR, d-pad, -/+ stay.
// Both layouts: skill icons on the buttons (gear "Skill icons on buttons", "d3_icons", default on: D3SkillIcons copies each HUD slot's
// icon onto the button whose letter the HUD shows under it; the button's own label moves to a small badge in its lower right),
// opacity ("d3_opacity" %), size ("d3_scale" %: radii and cluster offsets), and an edit mode (gear dialog "Edit control
// positions") where each control (a d-pad or the A/B/X/Y diamond moves as one) is dragged to a new spot; positions are saved per layout
// as fractions of the view ("d3_pos_<layout>_<group>" = "fx,fy"), "Reset layout" drops them. The ⚙ button keeps its own look
// ("gear_mode" / "gear_alpha", as in DoomPadView). With a real controller in use only ⚙ is shown and nothing is sent.
// [DOS2] GAMEPAD_EDITOR (DOS2 builds; D3 keeps the editor above): the gamepad layout's edit mode is DOOM's / TL2's editor (PadEditor):
// toolbar − + Grouped/Separate Hide/Show Reset Cancel Save, drag with a finger, a second finger resizes; Grouped moves / resizes a group (the
// d-pad, the ABXY diamond, every other control on its own), Separate every button alone (the d-pad and the diamond split into 4); Hide
// takes a control out of the game (neither drawn nor hit; the gear never hides). Prefs "d3_off_gamepad_<group>" = "fx,fy[,scale]",
// "d3_btn_gamepad_<name>" = "dx;dy;scale;hidden", "d3_split_gamepad"; written on Save only. [front] Add… / Rename… (owner 15.11.2026): stand-alone
// L3 / R3 buttons ("l3" / "r3", BUTTON_THUMBL / THUMBR) hidden until added; a button's own label "d3_btn_gamepad_<name>_lbl" drawn instead of its letter. An old "d3_pos_gamepad_<group>" (absolute
// anchor) is read as the group's offset until the first Save / Reset, which drop it.
// Output: the native controller channel of the pad (DmActivity.nativePadKey / nativePadAxes, device PAD_DEVICE), keycodes as
// front_android.c maps them (Enter=A, Esc=B, R=X, F=Y, Q=L, E=R, Shift=ZL, Ctrl=ZR, Tab=+, M=-, d-pad, BUTTON_THUMBL/R = L3/R3).
public class D3PadView extends View implements FrontMenu.Pad, PadSurface.Painter {
    static final int PAD_DEVICE = DoomPadView.PAD_DEVICE;
    static final int SETTINGS = -1, MENU_TOGGLE = -2;   // not keys: open the gear dialog / switch menu mode
    static final String LAYOUT_TOUCH = "touch", LAYOUT_GAMEPAD = "gamepad", LAYOUT_TL2 = "tl2", LAYOUT_TL2_SIDE = "tl2_side",
        LAYOUT_DEFAULT = DmActivity.DOS2 ? LAYOUT_GAMEPAD : DmActivity.TL2 ? LAYOUT_TL2 : LAYOUT_GAMEPAD;   // [DOS2] the Switch pad (owner 14.11)   // [TL2] its own touch layout after D2R Mobile (Tl2Touch), not D3's
    static final boolean GAMEPAD_EDITOR = DmActivity.DOS2;   // [DOS2] the gamepad layout's editor = PadEditor (DOOM / TL2 UX); D3 keeps its own
    static final int OPACITY_MIN = 10, OPACITY_MAX = 100, OPACITY_DEFAULT = 100;   // % (gear dialog sliders)
    static final int SCALE_MIN = 60, SCALE_MAX = 160, SCALE_DEFAULT = 100;
    static final int MENU_SWIPE_STICK = 0, MENU_SWIPE_DPAD = 1;

    // ---- Touch walking tuning (touch layout) ----
    // Hero anchor: D3's camera keeps the hero in the middle of the picture. x = HERO_FX of the picture width; y = "d3_hero_y" % of the
    // picture height from its top (gear slider HERO_Y_MIN..MAX, default HERO_Y_DEFAULT; calibrate from a screenshot of a standing hero).
    static final float HERO_FX = 0.5f;
    static final int HERO_Y_MIN = 40, HERO_Y_MAX = 65, HERO_Y_DEFAULT = 53;
    // Depth squeeze s ("d3_depth" %): the camera looks down at a slant, so ground depth shows shorter on screen than ground width. The
    // stick gets (dx, dy / s). Calibration (TOUCH_DESIGN.md A.2): push the stick at 45 degrees, measure the screen angle t of the walk:
    // s = tan(t). Expected 0.6..0.8.
    static final int DEPTH_MIN = 40, DEPTH_MAX = 100, DEPTH_DEFAULT = 72;
    // Tilt by distance (dp from the hero, after the depth correction): nothing within WORLD_DEAD_DP (a touch on the hero), then
    // WORLD_MIN_TILT growing linearly to full tilt at WORLD_FULL_DP.
    static final float WORLD_DEAD_DP = 36, WORLD_FULL_DP = 160, WORLD_MIN_TILT = 0.35f;
    static final float TOP_STRIP_F = 0.14f;   // touches above this fraction of the height never walk (the top button row)
    // Double tap = evade: a second touch within DOUBLE_TAP_MS of a tap (shorter than TAP_MS, moved less than TAP_SLOP_DP) and within
    // DOUBLE_TAP_DIST_DP of it; the right stick is held at full tilt toward the point for EVADE_MS.
    static final long TAP_MS = 220, DOUBLE_TAP_MS = 300, EVADE_MS = 120;
    static final float TAP_SLOP_DP = 14, DOUBLE_TAP_DIST_DP = 90;
    // Menu mode gestures: a move past MENU_SWIPE_DP is a swipe (stick: full tilt at MENU_STICK_DP; held at least MENU_MIN_SWIPE_MS so a
    // quick flick is seen by the game), a touch held MENU_LONG_MS without a swipe holds A, a shorter one is an A press of PULSE_MS.
    static final float MENU_SWIPE_DP = 20, MENU_STICK_DP = 70;
    static final long MENU_LONG_MS = 450, MENU_MIN_SWIPE_MS = 90, PULSE_MS = 100;

    // ---- Touch layout geometry (dp, scaled by "d3_scale") ----
    // A sits ARC_A_GX / ARC_A_GY from the bottom right corner; B Y X R ZR on an arc of ARC_R around it from ARC_FROM_DEG (left, slightly
    // below) counter-clockwise in ARC_STEP_DEG steps (to just right of straight up).
    static final float ARC_A_GX = -90, ARC_A_GY = -85, ARC_A_R = 42, ARC_R = 135, ARC_SKILL_R = 28, ARC_FROM_DEG = 190, ARC_STEP_DEG = 26.25f;

    static final int K_A = KeyEvent.KEYCODE_ENTER, K_B = KeyEvent.KEYCODE_ESCAPE, K_X = KeyEvent.KEYCODE_R, K_Y = KeyEvent.KEYCODE_F,
        K_L = KeyEvent.KEYCODE_Q, K_R = KeyEvent.KEYCODE_E, K_ZL = KeyEvent.KEYCODE_SHIFT_LEFT, K_ZR = KeyEvent.KEYCODE_CTRL_LEFT,
        K_PLUS = KeyEvent.KEYCODE_TAB, K_MINUS = KeyEvent.KEYCODE_M, K_UP = KeyEvent.KEYCODE_DPAD_UP, K_DOWN = KeyEvent.KEYCODE_DPAD_DOWN,
        K_LEFT = KeyEvent.KEYCODE_DPAD_LEFT, K_RIGHT = KeyEvent.KEYCODE_DPAD_RIGHT, K_L3 = KeyEvent.KEYCODE_BUTTON_THUMBL, K_R3 = KeyEvent.KEYCODE_BUTTON_THUMBR;

    // A control's place: anchor = (fx, fy) fraction of the view + (gx, gy) dp, or the saved position of its group; centre = anchor +
    // (ox, oy) dp. Members of one group (d-pad, A/B/X/Y diamond) share the anchor and move together in the edit mode.
    // PadEditor.Item (name, group, placed x / y / r, the player's dx / dy / ps / hidden): used by the gamepad layout's editor (GAMEPAD_EDITOR).
    private static class Ctl extends PadEditor.Item {
        final float fx, fy, gx, gy, ox, oy, size; final boolean menu; int pointer = -1;   // size: radius in dp (x "d3_scale")
        Ctl(String name, String group, float fx, float fy, float gx, float gy, float ox, float oy, float size, boolean menu) {
            super(name, group); this.fx = fx; this.fy = fy; this.gx = gx; this.gy = gy; this.ox = ox; this.oy = oy; this.size = size; this.menu = menu;
        }
    }
    private static final class Btn extends Ctl {
        final String label; final int key;
        Btn(String label, int key, String group, float fx, float fy, float gx, float gy, float ox, float oy, float r, boolean menu) {
            super(nameOf(key, label), group, fx, fy, gx, gy, ox, oy, r, menu); this.label = label; this.key = key; fixed = key == SETTINGS;
            defLabel = label; canRename = key != SETTINGS && key != MENU_TOGGLE;   // [front] PadEditor "Rename…"
        }
    }
    // a button's prefs id in the editor ("d3_btn_<layout>_<name>")
    private static String nameOf(int key, String label) {
        switch (key) {
        case K_A: return "a"; case K_B: return "b"; case K_X: return "x"; case K_Y: return "y"; case K_L: return "l"; case K_R: return "r";
        case K_ZL: return "zl"; case K_ZR: return "zr"; case K_PLUS: return "plus"; case K_MINUS: return "minus"; case K_UP: return "up";
        case K_DOWN: return "down"; case K_LEFT: return "left"; case K_RIGHT: return "right"; case K_L3: return "l3"; case K_R3: return "r3";
        case SETTINGS: return "gear"; case MENU_TOGGLE: return "menu"; default: return label.toLowerCase(Locale.ROOT);
        }
    }
    // zoneW > 0: floating; a touch within zoneW (fraction of the width) left / right of the home and below (home - zoneUp of the height)
    // that hits nothing else puts the base under the finger. grab: touches within grab x r of the home take the stick.
    private static final class Stick extends Ctl {
        final int clickKey; final float grab, zoneW, zoneUp;
        float kx, ky, travel, baseX = Float.NaN, baseY; long down; boolean onKnob, spawned;   // spawned: [DOS2] made under the finger (a short tap = a mouse click); kx / ky: the knob's offset from the base (px)
        Stick(String group, float fx, float fy, float r, int clickKey, float grab, float zoneW, float zoneUp) {
            super(group, group, fx, fy, 0, 0, 0, 0, r, false); this.clickKey = clickKey; this.grab = grab; this.zoneW = zoneW; this.zoneUp = zoneUp;
        }
    }

    private final float dp;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private String layout = "";
    private final List<Btn> btns = new ArrayList<>();
    private Stick left, right;   // right: gamepad layout only
    private final Map<String, float[]> pos = new HashMap<>();   // group -> saved anchor (fractions of the view)
    private float deadzone, op = 1f, scale = 1f, heroFy = HERO_Y_DEFAULT / 100f, depth = DEPTH_DEFAULT / 100f;
    private boolean stretched;   // gear "Picture shape" 1: the 16:9 picture stretched to the view; 0 bars / 2 widescreen (game at the screen's aspect) keep square pixels and the full height
    private int menuSwipe = MENU_SWIPE_STICK;
    private String menuLabel = "MENU", editHint = "", resetLabel = "Reset", doneLabel = "Done";

    static final int GEAR_NORMAL = DoomPadView.GEAR_NORMAL, GEAR_FADED = DoomPadView.GEAR_FADED;
    private int gearMode = GEAR_NORMAL, gearAlpha = DoomPadView.GEAR_ALPHA_DEFAULT;
    private boolean controllerMode, menuMode, editMode;
    // Skill icons (D3SkillIcons): sampled only while the pad is visible in play mode, the option is on and the activity is resumed.
    private final D3SkillIcons icons;
    private boolean iconsOn = true, padVisible = true, hostResumed = true, hideSticks = true;
    private final Paint iconPaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final Path clip = new Path(); private final RectF iconDst = new RectF();
    // [DOS2] the gamepad layout's editor (GAMEPAD_EDITOR): DOOM's / TL2's UX, places from PadEditor
    private final PadEditor editor = new PadEditor(this, new PadEditor.Host() {
        public PadEditor.Item editHit(float x, float y) { return hitAny(x, y); }
        public void editChanged() { lw = -1;
            // [DOS2] owner 15.11: hiding the left stick in the editor shows the same hint as the gear's "left stick" switch (floor tap / pinch / two fingers)
            if (DmActivity.DOS2 && left != null && left.hidden && !lstickHintShown) { lstickHintShown = true;
                try { android.widget.Toast.makeText(getContext(), getContext().getString(R.string.dos2_lstick_hint), android.widget.Toast.LENGTH_LONG).show(); } catch (RuntimeException e) { } }
            if (left != null && !left.hidden) lstickHintShown = false; }
        public void editClosed(boolean saved) { if (saved) dropOldPlaces(); endEdit(); }
    }, "gamepad");
    private int lw = -1, lh;   // the size the editor's places were made for
    private boolean lstickHintShown;   // [DOS2] the "no left stick" hint was shown for this hide
    private boolean ed() { return GAMEPAD_EDITOR && LAYOUT_GAMEPAD.equals(layout); }
    private Tl2Touch tl2;   // [TL2] the "tl2" layout: touches and drawing are Tl2Touch's
    private boolean tl2() { return tl2 != null && (LAYOUT_TL2.equals(layout) || LAYOUT_TL2_SIDE.equals(layout)); }

    // ---------------- [DOS2] the pad on its own surface (PadSurface, docs/port-kb APP-01; DOOM 2016 BUGS_AND_FIXES 2.45, ETR anrfix b1b9a60) ----
    // Drawn on the window, every invalidate() (each touch event, the frame counter twice a second, Tl2Touch's ticks) made the UI thread run
    // Choreographer.doFrame -> ThreadedRenderer.draw -> nSyncAndDrawFrame, which waits for HWUI's RenderThread; while our Vulkan work holds the
    // GPU for seconds (first-run pipeline builds, heavy scenes) RenderThread waits on its last frame's fence, the UI thread on RenderThread, input
    // is not consumed and the system kills the app ("Input dispatching timed out"). With attachSurface() (DOS2 by default; DmActivity:
    // DM_PAD_SURFACE=0 in dm/env or --ez dmPadHwui true = the old drawing on the window; DM_PAD_SURFACE=1 = on for D3 / TL2 too):
    //   * touches are still handled here, on the UI thread, exactly as before: nativePadKey / nativePadAxes go out from onTouchEvent at once;
    //   * invalidate() (from this view, Tl2Touch, PadEditor, D3SkillIcons) only asks for a recording at the next vsync (postOnAnimation,
    //     coalesced); the recording runs the unchanged drawing code (drawPad: every layout, skins, icons, the editor's toolbar) on the UI
    //     thread into a Picture (a CPU display list: no HWUI, no GPU, no RenderThread), so every piece of pad state stays UI-thread only;
    //   * the finished Picture is handed over lock-free (AtomicReference) and PadSurface's draw thread "dm-pad-draw" plays it onto its CPU
    //     canvas. PadSurface's paint lock is the draw thread's own (DoomPadView shares its lock with the touch handler because it paints live
    //     state; here nothing the touch handler touches is read off the UI thread, so the UI thread never waits for the draw thread at all).
    // Without a surface (D3 / TL2 by default, or the switches above) the view draws on the window as before (onDraw -> drawPad).
    private PadSurface surface;
    private static final class Frame { final Picture pic; final long ver; Frame(Picture p, long v) { pic = p; ver = v; } }
    private final AtomicReference<Frame> frame = new AtomicReference<>();
    private final AtomicBoolean recordPosted = new AtomicBoolean();
    private final Object paintLock = new Object();   // PadSurface's draw thread only
    private long frameVer;                           // UI thread
    private Frame painting;                          // draw thread: the frame paintSignature() saw, painted by paint()
    // The pad's surface, to add to the game's frame right under this view (above the game's SurfaceView); the same one on every call.
    PadSurface attachSurface() {
        if (surface == null) { surface = new PadSurface(getContext(), this); surface.setVisibility(getVisibility()); setWillNotDraw(true); super.invalidate(); invalidate(); }
        return surface;
    }
    // Back to drawing on the window (UI thread): the surface is taken out of its parent and its thread ends.
    void detachSurface() {
        PadSurface s = surface; if (s == null) return;
        surface = null; frame.set(null);
        if (s.getParent() instanceof android.view.ViewGroup) ((android.view.ViewGroup) s.getParent()).removeView(s);
        setWillNotDraw(false); super.invalidate();
        android.util.Log.i("dm", "[java] pad drawn on the window again (HWUI)");
    }
    boolean drawsOnSurface() { return surface != null; }
    // Every redraw request of the pad: with a surface, a recording at the next vsync instead of a window frame. (The super constructor may call
    // this before the fields exist: surface is null then.)
    @Override public void invalidate() {
        if (surface == null) { super.invalidate(); return; }
        if (recordPosted.compareAndSet(false, true)) {   // [DOS2] at most DM_PAD_MAX_HZ recordings a second (PadSurface.maxHz, owner 20.11)
            int hz = PadSurface.maxHz(); long wait = hz > 0 ? lastRecord + 1000 / hz - android.os.SystemClock.uptimeMillis() : 0;
            if (wait > 0) postOnAnimationDelayed(record, wait); else postOnAnimation(record);
        }
    }
    private long lastRecord;   // UI thread: uptime of the last recording
    private final Runnable record = () -> {   // UI thread (Choreographer animation callback: draws nothing on the window)
        recordPosted.set(false); lastRecord = android.os.SystemClock.uptimeMillis();
        PadSurface s = surface; int w = getWidth(), h = getHeight();
        if (s == null || w <= 0 || h <= 0) return;
        Picture p = new Picture(); Canvas c = p.beginRecording(w, h);
        try { drawPad(c); } finally { p.endRecording(); }
        frame.set(new Frame(p, ++frameVer));
        s.request();
    };
    @Override public void setVisibility(int v) { super.setVisibility(v); PadSurface s = surface; if (s != null) s.setVisibility(v); }
    @Override protected void onSizeChanged(int w, int h, int ow, int oh) { super.onSizeChanged(w, h, ow, oh); if (surface != null) invalidate(); }
    // ---- PadSurface.Painter (draw thread, under paintLock)
    @Override public Object paintLock() { return paintLock; }
    @Override public long paintSignature() { Frame f = frame.get(); painting = f; return f == null ? PadSurface.NOT_READY : f.ver; }
    @Override public void paint(Canvas c) { Frame f = painting; if (f != null) c.drawPicture(f.pic); }

    public D3PadView(Context c) {
        super(c);
        dp = c.getResources().getDisplayMetrics().density;
        line.setStyle(Paint.Style.STROKE); line.setStrokeWidth(2 * dp);
        text.setTextAlign(Paint.Align.CENTER);
        icons = c instanceof DmActivity && !DmActivity.TL2 ? new D3SkillIcons((DmActivity) c, this::invalidate) : null;   // [TL2] D3SkillIcons reads D3's HUD slots: not TL2's
        reloadSettings();
    }

    // ---------------- layouts ----------------
    private void addBtn(String label, int key, String group, float fx, float fy, float gx, float gy, float ox, float oy, float r, boolean menu) {
        btns.add(new Btn(label, key, group, fx, fy, gx, gy, ox, oy, r, menu));
    }
    private void addDpad(String group, float fx, float fy, float gx, float gy, float gap, float r) {
        addBtn("↑", K_UP, group, fx, fy, gx, gy, 0, -gap, r, true); addBtn("→", K_RIGHT, group, fx, fy, gx, gy, gap, 0, r, true);
        addBtn("↓", K_DOWN, group, fx, fy, gx, gy, 0, gap, r, true); addBtn("←", K_LEFT, group, fx, fy, gx, gy, -gap, 0, r, true);
    }
    private void buildGamepad() {   // = DoomPadView's D3 pad (abf0b6a); 2400x1080 ~ 914x411 dp
        left = new Stick("lstick", 0.125f, 0.52f, 64, K_L3, 1.6f, 0.295f, 0.30f);   // floating zone: x < 0.42, y > 0.22 at home
        right = new Stick("rstick", 0.735f, 0.80f, 50, K_R3, 1.5f, 0.26f, 0.40f);   // floating too (owner: the fixed one reacted only near the minimap): x 0.475..0.995, y > 0.40 at home, buttons win
        final float ax = 0.875f, ay = 0.48f, g = 50;   // right Joy-Con: diamond X top, A right (largest), B bottom, Y left
        addBtn("X", K_X, "abxy", ax, ay, 0, 0, 0, -g, 27, true); addBtn("A", K_A, "abxy", ax, ay, 0, 0, g, 0, 30, true);
        addBtn("B", K_B, "abxy", ax, ay, 0, 0, 0, g, 27, true); addBtn("Y", K_Y, "abxy", ax, ay, 0, 0, -g, 0, 27, true);
        addBtn("R", K_R, "r", 0.81f, 0.09f, 0, 0, 0, 0, 26, true); addBtn("ZR", K_ZR, "zr", 0.935f, 0.09f, 0, 0, 0, 0, 28, true);
        addBtn("+", K_PLUS, "plus", 0.62f, 0.09f, 0, 0, 0, 0, 22, true);
        addDpad("dpad", 0.265f, 0.82f, 0, 0, 40, 21);   // left Joy-Con: d-pad below the stick
        addBtn("L", K_L, "l", 0.19f, 0.09f, 0, 0, 0, 0, 26, true); addBtn("ZL", K_ZL, "zl", 0.065f, 0.09f, 0, 0, 0, 0, 28, true);
        addBtn("-", K_MINUS, "minus", 0.38f, 0.09f, 0, 0, 0, 0, 22, true);
        addBtn("⚙", SETTINGS, "gear", 0.50f, 0.09f, 0, 0, 0, 0, 20, true);
        if (GAMEPAD_EDITOR) {   // [front] stand-alone L3 / R3 (owner 15.11.2026: a tap on a stick's knob is not always handy): hidden until the editor's "Add…"
            addBtn("L3", K_L3, "l3", 0.30f, 0.55f, 0, 0, 0, 0, 22, true); btns.get(btns.size() - 1).defHidden = true;
            addBtn("R3", K_R3, "r3", 0.70f, 0.55f, 0, 0, 0, 0, 22, true); btns.get(btns.size() - 1).defHidden = true;
            for (Btn b : btns) if (b.defHidden) b.hidden = true;
        }
    }
    private void buildTouch() {
        left = new Stick("lstick", 0.12f, 0.68f, 64, K_L3, 1.4f, 0.18f, 0.30f);   // floating zone: x < 0.30, y > 0.38 at home
        right = null;
        addBtn("A", K_A, "a", 1, 1, ARC_A_GX, ARC_A_GY, 0, 0, ARC_A_R, false);   // primary skill / interact
        String[] l = {"B", "Y", "X", "R", "ZR"}; int[] k = {K_B, K_Y, K_X, K_R, K_ZR}; boolean[] menu = {true, false, false, true, true};
        for (int i = 0; i < l.length; i++) {
            double t = Math.toRadians(ARC_FROM_DEG - i * ARC_STEP_DEG);
            addBtn(l[i], k[i], l[i].toLowerCase(Locale.ROOT), 1, 1, ARC_A_GX + ARC_R * (float) Math.cos(t), ARC_A_GY - ARC_R * (float) Math.sin(t), 0, 0, ARC_SKILL_R, menu[i]);
        }
        addBtn("L", K_L, "l", 1, 1, -300, -52, 0, 0, 30, true);   // potion, left of the arc
        addBtn("ZL", K_ZL, "zl", 0.065f, 0.09f, 0, 0, 0, 0, 26, true);
        addDpad("dpad", 0, 0, 165, 75, 30, 17);
        addBtn("-", K_MINUS, "minus", 0.38f, 0.09f, 0, 0, 0, 0, 22, true);
        addBtn("⚙", SETTINGS, "gear", 0.50f, 0.09f, 0, 0, 0, 0, 20, true);
        addBtn("+", K_PLUS, "plus", 0.62f, 0.09f, 0, 0, 0, 0, 22, true);
        addBtn(menuLabel, MENU_TOGGLE, "menu", 0.75f, 0.09f, 0, 0, 0, 0, 24, true);
        addBtn("R3", K_R3, "r3", 0.87f, 0.09f, 0, 0, 0, 0, 20, false);
    }
    private boolean touch() { return LAYOUT_TOUCH.equals(layout); }
    private void buildTl2() {   // [TL2] D3PadView only keeps a placeholder stick; Tl2Touch has the controls
        left = new Stick("lstick", 0.12f, 0.68f, 64, K_L3, 1.4f, 0.18f, 0.30f); right = null;
        if (tl2 == null) tl2 = new Tl2Touch(this, this::openSettings);
    }
    private List<Ctl> all() { List<Ctl> a = new ArrayList<>(btns); a.add(left); if (right != null) a.add(right); return a; }
    private String posKey(String group) { return "d3_pos_" + layout + "_" + group; }

    // Gear dialog options (Tl2Gear.java, the shared gear), all "doom_controls" except the picture shape.
    @Override public void reloadSettings() {
        Context c = getContext();
        SharedPreferences p = c.getSharedPreferences("doom_controls", Context.MODE_PRIVATE);
        Context ui = c instanceof DmActivity ? ((DmActivity) c).d3Ui() : c;
        menuLabel = ui.getString(R.string.d3_pad_menu); editHint = ui.getString(R.string.d3_pad_edit_hint);
        resetLabel = ui.getString(R.string.d3_pad_reset); doneLabel = ui.getString(R.string.d3_pad_done);
        deadzone = p.getFloat("move_dz", 0f);
        boolean fps = p.getBoolean("fps", true); if (fps && !showFps) postDelayed(fpsTick, 500); showFps = fps;
        gearMode = p.getInt("gear_mode", GEAR_NORMAL) == GEAR_NORMAL ? GEAR_NORMAL : GEAR_FADED;
        gearAlpha = clamp(p.getInt("gear_alpha", DoomPadView.GEAR_ALPHA_DEFAULT), 1, 100);
        op = clamp(p.getInt("d3_opacity", OPACITY_DEFAULT), OPACITY_MIN, OPACITY_MAX) / 100f;
        scale = clamp(p.getInt("d3_scale", SCALE_DEFAULT), SCALE_MIN, SCALE_MAX) / 100f;
        heroFy = clamp(p.getInt("d3_hero_y", HERO_Y_DEFAULT), HERO_Y_MIN, HERO_Y_MAX) / 100f;
        depth = clamp(p.getInt("d3_depth", DEPTH_DEFAULT), DEPTH_MIN, DEPTH_MAX) / 100f;
        menuSwipe = p.getInt("d3_menu_swipe", MENU_SWIPE_STICK) == MENU_SWIPE_DPAD ? MENU_SWIPE_DPAD : MENU_SWIPE_STICK;
        stretched = c.getSharedPreferences("doom_graphics", Context.MODE_PRIVATE).getInt("aspect", 0) == 1;
        iconsOn = p.getBoolean("d3_icons", true);
        hideSticks = p.getBoolean("d3_hide_sticks", true);
        if (DmActivity.DOS2) loadHudRects(p.getString("dos2_hud_rects", ""));   // [DOS2] pack 3d: the HUD's tappable areas (no floating stick there)
        skinPack = p.getString("skin", PadSkin.CLASSIC);   // [front] PadSkin pack or classic
        String saved = p.getString("d3_layout", LAYOUT_DEFAULT);
        String want = DmActivity.DOS2 ? (LAYOUT_TL2.equals(saved) || LAYOUT_TL2_SIDE.equals(saved) ? saved : LAYOUT_GAMEPAD)   // [DOS2] Switch pad unless tl2 picked
            : DmActivity.TL2 ? (LAYOUT_GAMEPAD.equals(saved) || LAYOUT_TL2_SIDE.equals(saved) ? saved : LAYOUT_TL2)   // [TL2] tl2 / tl2_side / gamepad only
            : LAYOUT_TOUCH.equals(saved) ? LAYOUT_TOUCH : LAYOUT_GAMEPAD;
        if (!want.equals(layout)) {
            releaseAll(); menuMode = false; layout = want; btns.clear();
            if (LAYOUT_TL2.equals(layout) || LAYOUT_TL2_SIDE.equals(layout)) buildTl2(); else if (touch()) buildTouch(); else buildGamepad();
        }
        if (tl2()) tl2.reload(ui, p, op, scale, LAYOUT_TL2_SIDE.equals(layout));
        pos.clear();
        for (Ctl k : all()) {
            String v = p.getString(posKey(k.group), null);
            if (v == null) continue;
            String[] f = v.split(",");
            try { if (f.length == 2) pos.put(k.group, new float[] {clampF(Float.parseFloat(f[0])), clampF(Float.parseFloat(f[1]))}); } catch (NumberFormatException e) { /* ignore */ }
        }
        if (ed() && !editor.open) {   // [DOS2] the editor's places; an old absolute anchor becomes the group's offset (gamepad anchors have no dp part)
            left.title = ui.getString(R.string.pad_lstick_title); if (right != null) right.title = ui.getString(R.string.pad_rstick_title);   // [front] the Add list
            for (Btn b : btns) if (b.key == K_L3 || b.key == K_R3) b.title = ui.getString(b.key == K_L3 ? R.string.pad_l3_title : R.string.pad_r3_title);
            editor.setItems(all());
            editor.configure(ui, "d3_off_" + layout + "_", "d3_btn_" + layout + "_", "d3_split_" + layout, R.string.pad_ed_hint_pad_groups, R.string.pad_ed_hint_pad_split);
            editor.load(p);
            for (Ctl k : all()) { float[] o = pos.get(k.group); if (o != null && editor.off(k.group) == null) editor.off.put(k.group, new float[] {o[0] - k.fx, o[1] - k.fy, 1}); }
        }
        if (ed()) pos.clear();
        lw = -1;
        iconGate(); invalidate();
    }

    // ---------------- skill icons ----------------
    private void iconGate() { if (icons != null && !tl2()) icons.setGate(iconsOn && padVisible && hostResumed && !menuMode && !controllerMode && !editMode); }
    // DmActivity.onResume / onPause: no read-back of a surface that is going away; a resume samples again soon.
    void setHostResumed(boolean on) { hostResumed = on; iconGate(); }
    @Override public void onVisibilityAggregated(boolean visible) { super.onVisibilityAggregated(visible); padVisible = visible; iconGate(); }
    @Override protected void onAttachedToWindow() { super.onAttachedToWindow(); if (icons != null) { icons.start(); iconGate(); } }
    @Override protected void onDetachedFromWindow() { if (icons != null) icons.stop(); super.onDetachedFromWindow(); }
    private static int clamp(int v, int lo, int hi) { return Math.max(lo, Math.min(hi, v)); }
    private static float clampF(float v) { return Math.max(0f, Math.min(1f, v)); }

    // A real controller is in use (DmActivity.updatePadVisibility): only ⚙ stays; everything held is released.
    void setControllerMode(boolean on) {
        if (on == controllerMode) return;
        controllerMode = on;   // first: no axes from releaseAll (they would make the touch pad the active controller in front_android.c)
        if (tl2 != null) tl2.setControllerMode(on);
        if (on) releaseAll();
        iconGate(); invalidate();
    }

    // ---------------- geometry ----------------
    private float u() { return dp * scale; }
    // [DOS2] ed(): the default centres / radii go to PadEditor.place, which adds the player's place, size and the group's scale / offset;
    // ax / ay are then the group's placed centre
    private void place() {
        int w = getWidth(), h = getHeight();
        if (w == lw && h == lh) return;
        lw = w; lh = h;
        for (Ctl c : all()) { c.x0 = c.fx * w + (c.gx + c.ox) * u(); c.y0 = c.fy * h + (c.gy + c.oy) * u(); c.r0 = c.size * u(); }
        editor.place(w, h);
    }
    private float ax(Ctl c) { if (ed()) { place(); float[] g = editor.centre(c.group); return g != null ? g[0] : c.x; } float[] o = pos.get(c.group); return o != null ? o[0] * getWidth() : c.fx * getWidth() + c.gx * u(); }
    private float ay(Ctl c) { if (ed()) { place(); float[] g = editor.centre(c.group); return g != null ? g[1] : c.y; } float[] o = pos.get(c.group); return o != null ? o[1] * getHeight() : c.fy * getHeight() + c.gy * u(); }
    private float cx(Ctl c) { if (ed()) { place(); return c.x; } return ax(c) + c.ox * u(); }
    private float cy(Ctl c) { if (ed()) { place(); return c.y; } return ay(c) + c.oy * u(); }
    private float rad(Ctl c) { if (ed()) { place(); return c.r; } return c.size * u(); }
    private boolean off(Ctl c) { return (ed() && c.hidden && !editMode) || (DmActivity.DOS2 && !editMode && ((DmActivity.dos2LStickOff && c == left) || (DmActivity.dos2RStickOff && c == right))); }   // [DOS2] hidden in the editor / the gear's "left stick off": neither drawn nor hit in play
    private float sx(Stick s) { return Float.isNaN(s.baseX) ? cx(s) : s.baseX; }
    private float sy(Stick s) { return Float.isNaN(s.baseX) ? cy(s) : s.baseY; }

    private boolean shown(Btn b) {
        if (editMode) return true;
        if (controllerMode) return b.key == SETTINGS;
        if (off(b)) return false;
        return !menuMode || b.menu;
    }
    private Btn hit(float x, float y) {
        Btn best = null; float bestD = Float.MAX_VALUE;
        for (Btn b : btns) {   // nearest shown button whose (enlarged) circle holds the touch
            if (!shown(b)) continue;
            float dx = x - cx(b), dy = y - cy(b), r = rad(b) * 1.25f, d = dx * dx + dy * dy;
            if (d <= r * r && d < bestD) { best = b; bestD = d; }
        }
        return best;
    }
    private boolean near(Stick s, float x, float y, float k) { float dx = x - sx(s), dy = y - sy(s), r = rad(s) * k; return dx * dx + dy * dy <= r * r; }
    // [DOS2] with touch as mouse the big D3 zones would take the game's UI (tap-to-walk, hotbar, panels, dialog answers): a stick's
    // floating zone shrinks to a disc of MOUSE_ZONE_K x its base radius around its home (scales with the pad size); outside -> the game
    static final float MOUSE_ZONE_K = 2.2f, ZONE_R_X0 = 0.728f;   // left: x its radius (~370 px on the 2400x1080 phone); right: strip from 0.728 W (x 1748)
    private boolean floatZone(Stick s, float x, float y) {
        if (s.zoneW <= 0) return false;
        if (dos2Mouse()) {   // owner 15.11: both sticks the same disc (~370 px on the 2400x1080 phone = the left stick's); the 3j outer-side strip is gone
            // owner 15.11 (pack 3v screenshot, a red frame): the right stick's zone is the strip from x ZONE_R_X0 of the width to the right edge, the
            // whole height (the pad buttons, the column, the minimap and the hot bar inside it win: hit() / hudHit first); the rest of the screen is
            // the floor's. The left stick keeps its disc.
            if (s == right) return x >= ZONE_R_X0 * getWidth();
            final float dx = x - cx(s), dy = y - cy(s), r = rad(left != null ? left : s) * MOUSE_ZONE_K;
            return dx * dx + dy * dy <= r * r; }
        return Math.abs(x - cx(s)) < s.zoneW * getWidth() && y > cy(s) - s.zoneUp * getHeight();
    }

    // [DOS2] pack 3d (owner, pack 3c: "the dynamic stick grabs a big area and does not let me tap the settings; a tap on interactive menu items
    // must take priority over the stick"). With touch as mouse the game's UI wins over a stick spawning under the finger:
    //   - the host says what is on screen (DmActivity.nativeDos2UiState, made on the game thread from hle_dos2_touchui_route.c's walk of the
    //     visible UI objects): a menu / modal / panel / dialog / context menu, or out of game (main menu, options, character creation, loading)
    //     -> no floating spawn at all; a stick takes a touch only on its own knob and only when an idle stick is drawn ("d3_hide_sticks" off);
    //   - in game with only the HUD -> the discs as before, except over the HUD's tappable areas (hotbar strip with arrows / cancel / end turn,
    //     the iPad button column, portraits, minimap): there, too, only a drawn knob;
    //   - state unknown (old host, route hook off) -> the rule of pack 3c.
    // The HUD areas: "name=A,x0,x1,y0,y1" per area, ';' between (pref "dos2_hud_rects" in doom_controls overrides an area by its name, "name="
    // drops it). A = L / C / R: x0..x1 are in view heights from the left edge / the centre / the right edge (the 1920x1080 HUD scene is fitted
    // by height on a wide screen, its parts anchored like that: docs/dos2/TOUCH_VISIBLE.md), y0..y1 are fractions of the view height.
    // Pack 3e (owner's screen recording 17:39, 2400x1080): the iPad column is drawn by bottomBar_d in the centred 16:9 HUD scene, NOT at the
    // screen's right edge (icons x 2010..2142 = centre + 0.75..0.87 h, y 0.39..0.90 h); the old R,-0.13..0 rect missed it and the stick spawned on
    // the icons. The menu button (top, centre + 0.52 h) is in that scene too; the minimap ring is anchored to the right edge (x -0.38..-0.06 h).
    static final String HUD_RECTS_DEFAULT = "hotbar=C,-0.62,0.70,0.76,1;column=C,0.72,0.91,0.35,0.94;menu=C,0.46,0.58,0,0.11;minimap=R,-0.40,0,0,0.43;portraits=L,0,0.18,0,0.56";
    private final java.util.LinkedHashMap<String, float[]> hudRects = new java.util.LinkedHashMap<>();
    private final java.util.LinkedHashMap<String, float[]> hudRectsUser = new java.util.LinkedHashMap<>();   // only the pref's own areas (also used with live rects)
    private void loadHudRects(String over) {
        hudRects.clear(); hudRectsUser.clear();
        for (String src : new String[] {HUD_RECTS_DEFAULT, over == null ? "" : over})
            for (String e : src.split(";")) {
                int q = e.indexOf('='); if (q <= 0) continue;
                String name = e.substring(0, q).trim(), v = e.substring(q + 1).trim();
                if (v.isEmpty()) { hudRects.remove(name); hudRectsUser.remove(name); continue; }
                String[] f = v.split(",");
                if (f.length != 5) continue;
                char a = Character.toUpperCase(f[0].trim().isEmpty() ? 'L' : f[0].trim().charAt(0));
                try { float[] r = new float[] {a == 'R' ? 1 : a == 'C' ? 0.5f : 0, Float.parseFloat(f[1]), Float.parseFloat(f[2]), Float.parseFloat(f[3]), Float.parseFloat(f[4])};
                      hudRects.put(name, r); if (src != HUD_RECTS_DEFAULT) hudRectsUser.put(name, r); }
                catch (NumberFormatException ex) { /* keep the default */ }
            }
    }
    // Pack 3f+ (owner: "binding to the screen edges is bad - 16:9? 4:3 tablet? get the real position of the objects in pixels"): the movies measure
    // their HUD elements (tools/dos2_hudrect_touch.py: getBounds of the hot bar, every column / menu / arrow / cancel button, minimap, portraits),
    // the host maps them to the window (hle_dos2_touch.c dt_ei_hudrects: FlashPlayer origin + scale) and hands them over normalised; a hidden
    // element is simply not reported (the folded / hidden column too). Read once per HUD_LIVE_MS; none known -> the fixed fractions above.
    static final long HUD_LIVE_MS = 100;
    static final float HUD_MARGIN = 0.015f;   // x view height around each live rect (finger size)
    private float[] hudLive; private long hudLiveAt;
    private float[] hudLive() {
        long now = SystemClock.uptimeMillis();
        if (now - hudLiveAt >= HUD_LIVE_MS || hudLive == null) {
            hudLiveAt = now;
            try { hudLive = DmActivity.nativeDos2HudRects(); } catch (UnsatisfiedLinkError e) { hudLive = null; }
        }
        return hudLive;
    }
    private boolean hudHit(float x, float y) {
        float w = getWidth(), h = getHeight(); if (w <= 0 || h <= 0) return false;
        float[] live = hudLive();
        boolean useLive = live != null && live.length >= 4;
        if (useLive) {
            float m = HUD_MARGIN * h;
            for (int i = 0; i + 3 < live.length; i += 4)
                if (x >= live[i] * w - m && x <= live[i + 2] * w + m && y >= live[i + 1] * h - m && y <= live[i + 3] * h + m) return true;
        }
        for (java.util.Map.Entry<String, float[]> en : (useLive ? hudRectsUser : hudRects).entrySet()) {
            if ("column".equals(en.getKey()) && DmActivity.dos2ColumnState() == 2) continue;   // the iPad column is hidden (gear / gamepad rule)
            float[] r = en.getValue(); float ax = r[0] * w;
            if (x >= ax + r[1] * h && x <= ax + r[2] * h && y >= r[3] * h && y <= r[4] * h) return true;
        }
        return false;
    }
    private int uiState;   // nativeDos2UiState at the last finger down
    private static int dos2UiState() { try { return DmActivity.nativeDos2UiState(); } catch (UnsatisfiedLinkError e) { return 0; } }
    // the touch is the game UI's: a menu is up (or out of game), or the finger is on a HUD area in game (only with a known host state)
    private boolean uiFirst(float x, float y) {
        if ((uiState & 4) == 0) return false;
        return (uiState & 1) != 0 || (uiState & 2) == 0 || hudHit(x, y);
    }
    // the stick's own home: near(grab) as before; when the UI comes first, only a drawn idle knob (hidden sticks are not drawn idle)
    private boolean homeGrab(Stick s, float x, float y, boolean uiFirst) {
        if (off(s) || s.pointer >= 0 || !padVisible) return false;
        if (!uiFirst) return near(s, x, y, s.grab);
        return !hideSticks && near(s, x, y, 1.0f);
    }
    private boolean floatSpawn(Stick s, float x, float y, boolean uiFirst) { return !uiFirst && padVisible && !off(s) && s.pointer < 0 && floatZone(s, x, y); }

    // ---------------- output ----------------
    private static void key(int k, boolean down) { DmActivity.nativePadKey(PAD_DEVICE, k, down); }
    private void pulse(final int k) { key(k, true); postDelayed(() -> key(k, false), PULSE_MS); }

    private void moveStick(Stick s, float x, float y) {
        float dx = x - sx(s), dy = y - sy(s), d = (float) Math.sqrt(dx * dx + dy * dy), r = rad(s);
        if (d > r) { dx *= r / d; dy *= r / d; }
        s.kx = dx; s.ky = dy; s.travel = Math.max(s.travel, d);
    }
    // A touch near the home also puts the base under the finger (D3 738d12e, issue #6): tilting from the home centre made a touch beside
    // the knob deflect the stick at its home. onKnob still tells a tap on the knob (L3 / R3) from a drag.
    private void grab(Stick s, int id, float x, float y) { boolean knob = near(s, x, y, 1.0f); grabFloating(s, id, x, y); s.onKnob = knob; s.spawned = false; }
    private void grabFloating(Stick s, int id, float x, float y) { s.baseX = x; s.baseY = y; s.onKnob = false; s.pointer = id; s.down = SystemClock.uptimeMillis(); s.kx = s.ky = 0; s.travel = 0; s.spawned = true; }
    // [DOS2] owner 15.11: wherever a stick can appear, a short tap on it is that stick's click (L3 / R3), like a tap on its home knob; the floor
    // there is reached by a tap only with a real gamepad (no on-screen sticks). A stick spawned under the finger and let go within FLOAT_TAP_MS
    // without moving = the click.
    static final long FLOAT_TAP_MS = 250;
    private void release(Stick s, boolean click) {   // a short tap on a stick's home knob presses L3 / R3
        if (click && s.onKnob && s.travel < 0.5f * rad(s) && SystemClock.uptimeMillis() - s.down < 300) pulse(s.clickKey);
        final boolean tap = click && s.spawned && dos2Mouse() && s.travel < 0.25f * rad(s) && SystemClock.uptimeMillis() - s.down < FLOAT_TAP_MS;
        s.pointer = -1; s.kx = s.ky = 0; s.baseX = Float.NaN; s.spawned = false;
        if (tap) pulse(s.clickKey);
    }

    // Radial dead-zone compensation (gear "dead-zone compensation"): a small deflection maps to just past the game's dead zone.
    private float[] shape(float x, float y) {
        float m = (float) Math.sqrt(x * x + y * y);
        if (m < 0.04f) return new float[] {0, 0};
        float o = Math.min(1f, deadzone + (1f - deadzone) * Math.min(1f, m));
        return new float[] {x / m * o, y / m * o};
    }

    // World point -> ground direction from the hero (screen-down = +y, as the sticks), in the 16:9 picture's own proportions.
    // Returns {x, y, distance in dp}.
    private float[] ground(float x, float y) {
        float w = getWidth(), h = getHeight(), pw = w, ph = h;
        if (!stretched) { if (w * 9 > h * 16) pw = h * 16 / 9; else ph = w * 9 / 16; }
        float xs = ph > 0 ? (pw / ph) / (16f / 9f) : 1f;   // stretched: screen x is wider than the game's
        float hx = w * 0.5f + (HERO_FX - 0.5f) * pw, hy = (h - ph) * 0.5f + heroFy * ph;
        float dx = (x - hx) / xs, dy = (y - hy) / depth;
        return new float[] {dx, dy, (float) Math.sqrt(dx * dx + dy * dy) / dp};
    }
    private float[] worldTilt(float x, float y) {
        float[] g = ground(x, y);
        if (g[2] < WORLD_DEAD_DP) return new float[] {0, 0};
        float m = WORLD_MIN_TILT + (1 - WORLD_MIN_TILT) * Math.min(1f, (g[2] - WORLD_DEAD_DP) / (WORLD_FULL_DP - WORLD_DEAD_DP)), d = g[2] * dp;
        return new float[] {g[0] / d * m, g[1] / d * m};
    }

    private long evadeUntil; private float evadeX, evadeY;
    private void evade(float x, float y) {
        float[] g = ground(x, y);
        if (g[2] < WORLD_DEAD_DP * 0.5f) return;   // on the hero: no direction
        float d = g[2] * dp; evadeX = g[0] / d; evadeY = g[1] / d; evadeUntil = SystemClock.uptimeMillis() + EVADE_MS;
        postDelayed(this::axes, EVADE_MS + 4);
    }

    private void axes() {
        if (controllerMode || editMode) return;   // a real controller / the edit mode: nothing from this pad
        float[] lv = {0, 0}, rv = {0, 0};
        if (left.pointer >= 0) lv = shape(left.kx / rad(left), left.ky / rad(left));
        else if (menuMode) lv = shape(menuSx, menuSy);
        else if (worldPtr >= 0) { float[] w = worldTilt(worldX, worldY); lv = shape(w[0], w[1]); }
        if (right != null && right.pointer >= 0) rv = shape(right.kx / rad(right), right.ky / rad(right));
        if (SystemClock.uptimeMillis() < evadeUntil) { rv[0] = evadeX; rv[1] = evadeY; }
        DmActivity.nativePadAxes(PAD_DEVICE, lv[0], lv[1], rv[0], rv[1], 0, 0, 0, 0);
    }

    // ---------------- world touch (touch layout, play mode) ----------------
    private int worldPtr = -1; private float worldX, worldY, worldX0, worldY0; private long worldT0, lastTapT; private float lastTapX, lastTapY; private boolean worldMoved, worldEvaded;
    private void worldStart(int id, float x, float y) {
        long now = SystemClock.uptimeMillis();
        worldEvaded = now - lastTapT < DOUBLE_TAP_MS && Math.hypot(x - lastTapX, y - lastTapY) < DOUBLE_TAP_DIST_DP * dp;
        if (worldEvaded) { evade(x, y); lastTapT = 0; }   // the finger may stay down: it then walks on toward the point
        worldPtr = id; worldX = worldX0 = x; worldY = worldY0 = y; worldT0 = now; worldMoved = false;
    }
    private void worldMove(float x, float y) { worldX = x; worldY = y; if (Math.hypot(x - worldX0, y - worldY0) > TAP_SLOP_DP * dp) worldMoved = true; }
    private void worldEnd(boolean cancel) {
        long now = SystemClock.uptimeMillis();
        if (!cancel && !worldMoved && !worldEvaded && now - worldT0 < TAP_MS) { lastTapT = now; lastTapX = worldX0; lastTapY = worldY0; }   // a tap: the first half of a double tap
        worldPtr = -1;
    }

    // ---------------- menu mode gestures ----------------
    private int menuPtr = -1, menuDpad; private float menuX0, menuY0, menuSx, menuSy; private long swipeT0; private boolean menuSwiping, menuHoldA;
    private int swipeKey; private boolean swipeStick;   // what the last swipe still holds until MENU_MIN_SWIPE_MS has passed
    private final Runnable swipeEnd = () -> { finishSwipe(); axes(); };
    private final Runnable menuLong = () -> { if (menuPtr >= 0 && !menuSwiping && !menuHoldA) { menuHoldA = true; key(K_A, true); invalidate(); } };
    private void finishSwipe() {
        removeCallbacks(swipeEnd);
        if (swipeKey != 0) key(swipeKey, false);
        if (swipeStick) menuSx = menuSy = 0;
        swipeKey = 0; swipeStick = false;
    }
    private void menuStart(int id, float x, float y) {
        finishSwipe();
        menuPtr = id; menuX0 = x; menuY0 = y; menuSwiping = menuHoldA = false; menuSx = menuSy = 0; menuDpad = 0;
        postDelayed(menuLong, MENU_LONG_MS);
    }
    private void menuMove(float x, float y) {
        float dx = x - menuX0, dy = y - menuY0, d = (float) Math.sqrt(dx * dx + dy * dy);
        if (!menuSwiping && !menuHoldA && d > MENU_SWIPE_DP * dp) { menuSwiping = true; swipeT0 = SystemClock.uptimeMillis(); removeCallbacks(menuLong); }
        if (!menuSwiping || d <= 0) return;
        if (menuSwipe == MENU_SWIPE_STICK) { float m = Math.min(1f, d / (MENU_STICK_DP * dp)); menuSx = dx / d * m; menuSy = dy / d * m; }
        else {
            int k = Math.abs(dx) > Math.abs(dy) ? (dx > 0 ? K_RIGHT : K_LEFT) : (dy > 0 ? K_DOWN : K_UP);
            if (k != menuDpad) { if (menuDpad != 0) key(menuDpad, false); key(k, true); menuDpad = k; }
        }
    }
    // cancel: the touch was taken away (focus lost, mode switch): release at once, no A.
    private void menuEnd(boolean cancel) {
        if (menuPtr < 0) return;
        removeCallbacks(menuLong); menuPtr = -1;
        if (menuHoldA) { key(K_A, false); menuHoldA = false; }
        else if (menuSwiping) {
            swipeKey = menuDpad; swipeStick = true; menuDpad = 0; menuSwiping = false;
            long wait = cancel ? 0 : MENU_MIN_SWIPE_MS - (SystemClock.uptimeMillis() - swipeT0);
            if (wait > 0) postDelayed(swipeEnd, wait); else finishSwipe();
        } else if (!cancel) pulse(K_A);
    }
    void setMenuMode(boolean on) {
        if (tl2()) return;   // [TL2] no menu mode (per-item menu touch waits for CEGUI mouse events)
        releaseAll(); menuMode = on; axes(); iconGate(); invalidate();
    }

    // Lets go of everything this pad holds (focus lost, layout / mode switch, a controller took over, edit mode).
    @Override public void releaseAll() {
        if (tl2 != null) tl2.releaseAll();
        boolean any = false;
        for (Btn b : btns) if (b.pointer >= 0) { if (b.key >= 0) key(b.key, false); b.pointer = -1; any = true; }
        if (left != null && left.pointer >= 0) { release(left, false); any = true; }
        if (right != null && right.pointer >= 0) { release(right, false); any = true; }
        if (worldPtr >= 0) { worldPtr = -1; any = true; }
        if (menuPtr >= 0) { menuEnd(true); any = true; }
        if (swipeKey != 0 || swipeStick) { finishSwipe(); any = true; }
        if (evadeUntil > SystemClock.uptimeMillis()) any = true;
        evadeUntil = 0;
        lastTapT = 0; editPtr = -1;
        if (any && !controllerMode && !editMode) DmActivity.nativePadAxes(PAD_DEVICE, 0, 0, 0, 0, 0, 0, 0, 0);
    }

    // [front] the gear opens the unified pause menu (FrontMenu: game settings, gamepad, saves, mode, quit)
    private void openSettings() { if (getContext() instanceof FrontMenu.Owner && ((FrontMenu.Owner) getContext()).frontMenu() != null) ((FrontMenu.Owner) getContext()).frontMenu().open(); else if (getContext() instanceof DmActivity) ((DmActivity) getContext()).showDoomSettings(); }
    // ---- [front] FrontMenu.Pad
    private Runnable editDone;
    @Override public void startEdit(Runnable done) { if (tl2()) { tl2.startEdit(done); return; } editDone = done; startEdit(); }
    @Override public boolean editing() { return tl2() ? tl2.editing() : editMode; }
    @Override public void endEdit(boolean save) { if (tl2()) { tl2.endEdit(save); return; } if (ed()) { if (editMode) editor.close(save); return; } endEdit(); }   // D3's editor keeps every drag at once ("Reset" / "Done" on its panel); [TL2] DOOM's Save / Cancel
    @Override public void resetLayout() {
        if (tl2()) tl2.resetLayout();
        else if (ed()) { editor.clear(); editor.save(); dropOldPlaces(); lw = -1; invalidate(); }   // [DOS2] every place, size and hide, saved at once
        else resetPlaces();
    }
    @Override public String prefsName() { return "doom_controls"; }
    @Override public String[] sliderKeys() { return new String[] {"d3_opacity", "d3_scale"}; }
    @Override public int[] sliderDefaults() { return new int[] {OPACITY_DEFAULT, SCALE_DEFAULT}; }
    @Override public String[] presets() { return null; }   // the touch / gamepad layouts stay in the gear dialog
    @Override public View view() { return this; }
    private PadSkin skin; private String skinPack = PadSkin.CLASSIC;
    private PadSkin skin() { if (skin == null) skin = new PadSkin(getContext()); skin.select(skinPack, PadSkin.layoutFor(getWidth(), getHeight())); return skin; }
    private static String skinName(Btn b) {
        switch (b.label) { case "A": return "a"; case "B": return "b"; case "X": return "x"; case "Y": return "y"; case "L": return "l"; case "R": return "r";
            case "ZL": return "zl"; case "ZR": return "zr"; case "+": return "plus"; case "-": return "minus"; case "\u2699": return "menu"; case "R3": return "rs"; case "L3": return "ls"; default: return null; }
    }

    // ---------------- edit mode ----------------
    private int editPtr = -1; private Ctl editCtl; private float editAx, editAy, editX0, editY0;
    private final RectF resetRect = new RectF(), doneRect = new RectF(), panelRect = new RectF();
    void startEdit() {
        if (tl2()) { tl2.startEdit(null); return; }
        releaseAll(); editMode = true; if (ed()) { editor.start(); android.util.Log.i("dm", "[java] gamepad layout editor open"); }
        iconGate(); invalidate();
    }
    private void endEdit() { editPtr = -1; editMode = false; lw = -1; iconGate(); invalidate(); Runnable d = editDone; editDone = null; if (d != null) d.run(); }
    // [DOS2] the old editor's absolute anchors: read as offsets until the new editor saves / resets
    private void dropOldPlaces() {
        SharedPreferences.Editor e = getContext().getSharedPreferences("doom_controls", Context.MODE_PRIVATE).edit();
        for (Ctl k : all()) e.remove(posKey(k.group));
        e.apply(); pos.clear();
    }
    private void resetPlaces() {
        SharedPreferences.Editor e = getContext().getSharedPreferences("doom_controls", Context.MODE_PRIVATE).edit();
        for (Ctl k : all()) e.remove(posKey(k.group));
        e.apply(); pos.clear(); invalidate();
    }
    private Ctl hitAny(float x, float y) {
        Ctl best = null; float bestD = Float.MAX_VALUE;
        for (Ctl c : all()) {
            float dx = x - cx(c), dy = y - cy(c), r = rad(c) * (c instanceof Stick ? 1.0f : 1.25f), d = dx * dx + dy * dy;
            if (d <= r * r && d < bestD) { best = c; bestD = d; }
        }
        return best;
    }
    private void editTouch(MotionEvent e, int a, int id, float x, float y) {
        switch (a) {
        case MotionEvent.ACTION_DOWN: case MotionEvent.ACTION_POINTER_DOWN:
            if (resetRect.contains(x, y)) { resetPlaces(); break; }
            if (doneRect.contains(x, y)) { endEdit(); break; }
            if (editPtr < 0) { Ctl c = hitAny(x, y); if (c != null) { editPtr = id; editCtl = c; editAx = ax(c); editAy = ay(c); editX0 = x; editY0 = y; } }
            break;
        case MotionEvent.ACTION_MOVE:
            for (int k = 0; k < e.getPointerCount(); k++) if (e.getPointerId(k) == editPtr && getWidth() > 0 && getHeight() > 0)
                pos.put(editCtl.group, new float[] {clampF((editAx + e.getX(k) - editX0) / getWidth()), clampF((editAy + e.getY(k) - editY0) / getHeight())});
            break;
        case MotionEvent.ACTION_UP: case MotionEvent.ACTION_POINTER_UP: case MotionEvent.ACTION_CANCEL:
            if (editPtr >= 0 && (id == editPtr || a != MotionEvent.ACTION_POINTER_UP)) {
                float[] o = pos.get(editCtl.group);
                if (o != null) getContext().getSharedPreferences("doom_controls", Context.MODE_PRIVATE).edit()
                    .putString(posKey(editCtl.group), String.format(Locale.ROOT, "%.4f,%.4f", o[0], o[1])).apply();
                editPtr = -1;
            }
            break;
        }
        invalidate();
    }

    // ---------------- touches ----------------
    // [DOS2] touch as mouse (DmActivity.dos2Touch, gear; hle_dos2_touch.c): a touch that is not on a shown button or a stick's home goes to
    // the game as the mouse (the sticks' floating zones, discs around their homes then, keep their touches: a stick spawns under the finger there); the native side makes clicks / drags / right click / zoom of it
    private final android.util.SparseBooleanArray mousePtr = new android.util.SparseBooleanArray();
    private boolean dos2Mouse() { return DmActivity.DOS2 && DmActivity.dos2Touch && !menuMode; }
    private void mouse(int phase, int id, float x, float y) {
        int w = getWidth(), h = getHeight(); if (w <= 0 || h <= 0) return;
        boolean taken = DmActivity.nativeDos2Touch(phase, id, x / w, y / h);
        if (phase == 0 && taken) mousePtr.put(id, true); else if (phase >= 2) mousePtr.delete(id);
    }
    @Override public boolean onTouchEvent(MotionEvent e) {
        if (tl2()) return tl2.onTouch(e);   // [TL2]
        int a = e.getActionMasked(), i = e.getActionIndex(), id = e.getPointerId(i);
        float x = e.getX(i), y = e.getY(i);
        if (editMode && ed()) { editor.touch(e); return true; }   // [DOS2]
        if (editMode) { editTouch(e, a, id, x, y); return true; }
        if (controllerMode) {   // a real controller drives the game: only the settings button, and no axes from this pad
            if (a == MotionEvent.ACTION_DOWN || a == MotionEvent.ACTION_POINTER_DOWN) { Btn b = hit(x, y); if (b != null && b.key == SETTINGS) openSettings(); }
            return true;
        }
        switch (a) {
        case MotionEvent.ACTION_DOWN: case MotionEvent.ACTION_POINTER_DOWN: {
            Btn b = hit(x, y);
            if (dos2Mouse()) uiState = dos2UiState();   // [DOS2] pack 3d: what the game shows now (one cheap JNI read, no game memory touched from here)
            final boolean ui = dos2Mouse() && uiFirst(x, y);
            if (b != null && b.key == SETTINGS) openSettings();
            else if (b != null && b.key == MENU_TOGGLE) setMenuMode(!menuMode);
            else if (b != null) { if (b.pointer < 0) { b.pointer = id; key(b.key, true); } }
            else if (menuMode) { if (menuPtr < 0) menuStart(id, x, y); }
            else if (homeGrab(left, x, y, ui)) grab(left, id, x, y);   // the stick takes touches around its home
            else if (floatSpawn(left, x, y, ui)) grabFloating(left, id, x, y);   // and spawns under the finger in its zone [DOS2] unless the game's UI comes first (pack 3d)
            else if (right != null && homeGrab(right, x, y, ui)) grab(right, id, x, y);
            else if (right != null && floatSpawn(right, x, y, ui)) grabFloating(right, id, x, y);
            else if (dos2Mouse()) mouse(0, id, x, y);   // [DOS2] touch as mouse: off the buttons and the stick zones = the game's cursor
            else if (right != null) { /* gamepad: other free taps do nothing */ }
            else if (worldPtr < 0 && y > TOP_STRIP_F * getHeight()) worldStart(id, x, y);
            break;
        }
        case MotionEvent.ACTION_MOVE:
            for (int k = 0; k < e.getPointerCount(); k++) {
                int pid = e.getPointerId(k); float px = e.getX(k), py = e.getY(k);
                if (pid == left.pointer) moveStick(left, px, py);
                if (right != null && pid == right.pointer) moveStick(right, px, py);
                if (pid == worldPtr) worldMove(px, py);
                if (pid == menuPtr) menuMove(px, py);
                if (mousePtr.get(pid)) mouse(1, pid, px, py);
            }
            break;
        case MotionEvent.ACTION_UP: case MotionEvent.ACTION_POINTER_UP: case MotionEvent.ACTION_CANCEL: {
            boolean all = a != MotionEvent.ACTION_POINTER_UP, cancel = a == MotionEvent.ACTION_CANCEL;
            for (Btn b : btns) if (b.pointer >= 0 && (b.pointer == id || all)) { key(b.key, false); b.pointer = -1; }
            if (left.pointer >= 0 && (id == left.pointer || all)) release(left, !cancel);
            if (right != null && right.pointer >= 0 && (id == right.pointer || all)) release(right, !cancel);
            if (worldPtr >= 0 && (id == worldPtr || all)) worldEnd(cancel);
            if (menuPtr >= 0 && (id == menuPtr || all)) menuEnd(cancel);
            for (int k = mousePtr.size() - 1; k >= 0; k--) { int pid = mousePtr.keyAt(k); if (pid == id || all) { int pi = e.findPointerIndex(pid);
                mouse(cancel ? 3 : 2, pid, pi >= 0 ? e.getX(pi) : x, pi >= 0 ? e.getY(pi) : y); } }
            break;
        }
        }
        axes(); invalidate();
        return true;
    }

    // ---------------- drawing ----------------
    private boolean showFps; private final float[] fstats = new float[4];
    private final Runnable fpsTick = new Runnable() { public void run() { if (!showFps) return; DmActivity.nativeFrameStats(fstats); invalidate(); postDelayed(this, 500); } };
    private void drawFps(Canvas c) {
        if (!showFps || fstats[0] <= 0) return;
        String t = String.format(Locale.ROOT, "%.0f fps  %.1f ms (max %.0f)%s", fstats[0], fstats[1], fstats[2],
            fstats[3] >= 0 ? String.format(Locale.ROOT, "  GPU %.1f", fstats[3]) : "");
        text.setTextSize(15 * dp); text.setColor(0xd0ffffff);
        float w = text.measureText(t), x = 18 * dp + w / 2, y = text.getTextSize();   // top left, clear of the rounded screen corner
        fill.setColor(0x80000000); c.drawRect(0, 0, x + w / 2 + 6 * dp, y + 5 * dp, fill);
        c.drawText(t, x, y, text);
    }

    @Override protected void onDraw(Canvas c) {
        super.onDraw(c);
        if (surface != null) return;   // [DOS2] drawn by PadSurface's thread (attachSurface)
        drawPad(c);
    }
    private void drawPad(Canvas c) {
        drawFps(c);
        if (tl2()) { tl2.draw(c); return; }   // [TL2]
        if (editMode) {
            c.drawColor(0x50000000);
            drawStick(c, left); if (right != null) drawStick(c, right);
            for (Btn b : btns) drawBtn(c, b);
            if (ed()) {   // [DOS2] hidden controls struck through, DOOM's toolbar
                for (Ctl k : all()) if (k.hidden) editor.strike(c, cx(k), cy(k), rad(k));
                editor.draw(c);
            } else drawEditPanel(c);
            return;
        }
        if (!controllerMode && !menuMode) {
            drawStick(c, left); if (right != null) drawStick(c, right);
            if (worldPtr >= 0) {   // where the hero is walking to
                line.setColor(0xffffffff); line.setAlpha(Math.round(0x90 * op)); c.drawCircle(worldX, worldY, 22 * dp, line);
            }
        }
        for (Btn b : btns) if (shown(b)) drawBtn(c, b);
    }
    private float k() { return editMode ? Math.max(op, 0.5f) : op; }
    private float k(Ctl c) { return editMode && ed() ? editor.alpha(c, op) : k(); }   // [DOS2] the editor: chosen 1, hidden dimmed
    // Hidden sticks ("d3_hide_sticks", default on, as D2R Mobile): an idle stick is not drawn at all; while a finger holds it, a faint
    // ring (half the layout opacity) shows where its base is. The edit mode always draws them so they can be placed.
    private void drawStick(Canvas c, Stick s) {
        if ((hideSticks && !editMode && s.pointer < 0) || off(s)) return;
        float x = sx(s), y = sy(s), r = rad(s), k = hideSticks && !editMode ? k() * 0.5f : k(s);
        if (skin().draw(c, s == right ? "stick_r" : "stick_l", x, y, r, r, s.pointer >= 0, 0, s.kx / r, s.ky / r, k * skin.packAlpha(), getHeight())) return;   // [front]
        fill.setColor(0xffffffff); fill.setAlpha(Math.round(0x20 * k)); c.drawCircle(x, y, r, fill);
        line.setColor(editMode ? 0xffffd040 : 0xffffffff); line.setAlpha(editMode ? 0xe0 : Math.round(0x90 * k)); c.drawCircle(x, y, r, line);
        fill.setAlpha(Math.round((s.pointer >= 0 ? 0x80 : 0x40) * k)); c.drawCircle(x + s.kx, y + s.ky, r * 0.42f, fill);
    }
    private void drawBtn(Canvas c, Btn b) {
        boolean gear = b.key == SETTINGS, on = b.pointer >= 0 || (b.key == MENU_TOGGLE && menuMode);
        float k = gear ? (gearMode == GEAR_FADED && !editMode ? gearAlpha / 100f : 1f) : k(b);
        float x = cx(b), y = cy(b), r = rad(b);
        fill.setColor(b.key == MENU_TOGGLE && menuMode ? 0xffffb030 : 0xffffffff); fill.setAlpha(Math.round((on ? 0x80 : 0x30) * k));
        line.setColor(editMode ? 0xffffd040 : 0xffffffff); line.setAlpha(editMode ? 0xe0 : Math.round(0x90 * k));
        Bitmap ic = iconsOn && icons != null && b.key >= 0 ? icons.icon(b.key) : null;
        if (ic != null) { drawIconBtn(c, b, ic, x, y, r, k, on); return; }
        if (drawSkinned(c, b, x, y, r, k, on)) return;   // [front] the pad's skin
        c.drawCircle(x, y, r, fill); c.drawCircle(x, y, r, line);
        float ts = touch() ? Math.max(12 * u(), 0.55f * r) : 15 * u();   // gamepad: the labels of DoomPadView's D3 pad
        String lab = lab(b);
        text.setTextSize(ts); float w = text.measureText(lab);
        if (w > 1.7f * r) { ts *= 1.7f * r / w; text.setTextSize(ts); }   // long labels (MENU / МЕНЮ, a player's own) fit the circle
        text.setColor(0xffffffff); text.setAlpha(Math.round(0xd0 * k));
        c.drawText(lab, x, y + ts * 0.35f, text);
    }
    // [front] the label drawn on a button: the player's own (editor "Rename…") or the default glyph / letter
    private String lab(Btn b) { return ed() && b.custom != null ? b.custom : b.label; }
    // A button with the HUD's icon of its skill: the icon fills the circle (at the layout opacity), pressed = lighter, the button's
    // label as a small badge in the lower right.
    private void drawIconBtn(Canvas c, Btn b, Bitmap ic, float x, float y, float r, float k, boolean on) {
        c.save();
        clip.reset(); clip.addCircle(x, y, r, Path.Direction.CW); c.clipPath(clip);
        iconPaint.setAlpha(Math.round(0xe0 * k)); iconDst.set(x - r, y - r, x + r, y + r); c.drawBitmap(ic, null, iconDst, iconPaint);
        if (on) { fill.setColor(0xffffffff); fill.setAlpha(Math.round(0x70 * k)); c.drawCircle(x, y, r, fill); }
        c.restore();
        c.drawCircle(x, y, r, line);
        float ts = Math.max(10 * dp, 0.34f * r); text.setTextSize(ts);
        String lab = lab(b); PadEdit.fit(text, lab, r * 1.2f); ts = text.getTextSize();   // [front] a player's label stays a badge
        float bx = x + r * 0.70f, by = y + r * 0.70f, br = Math.max(ts * 0.72f, text.measureText(lab) * 0.62f);
        fill.setColor(0xff000000); fill.setAlpha(Math.round(0xb0 * k)); c.drawCircle(bx, by, br, fill);
        text.setColor(0xffffffff); text.setAlpha(Math.round(0xe0 * k)); c.drawText(lab, bx, by + ts * 0.35f, text);
    }
    // [front] a button in the skin: by its label's name; the d-pad's four buttons are drawn as the skin's one d-pad, by the up button, at the group's centre
    private boolean drawSkinned(Canvas c, Btn b, float x, float y, float r, float k, boolean on) {
        PadSkin s = skin(); if (!s.on()) return false;
        if (ed() && b.custom != null) return false;   // [front] renamed: the classic circle with the player's label
        float sa = (b.key == SETTINGS ? 1 : s.packAlpha()) * k;
        if (b.key == K_UP || b.key == K_DOWN || b.key == K_LEFT || b.key == K_RIGHT) {
            if (!s.has("dpad")) return false;
            if (ed()) for (Btn d : btns) if (d.group.equals(b.group) && (d.dx != 0 || d.dy != 0 || d.ps != 1 || d.hidden || d.custom != null)) return false;   // [DOS2] split / moved / renamed: four buttons
            if (b.key != K_UP) return true;
            int bits = 0; float gap = 0;
            for (Btn d : btns) if (d.group.equals(b.group)) { gap = Math.max(gap, (float) Math.hypot(cx(d) - ax(d), cy(d) - ay(d)));
                if (d.pointer >= 0) bits |= d.key == K_UP ? PadTheme.UP : d.key == K_DOWN ? PadTheme.DOWN : d.key == K_LEFT ? PadTheme.LEFT : PadTheme.RIGHT; }
            return s.draw(c, "dpad", ax(b), ay(b), gap + r, gap + r, false, bits, 0, 0, sa, getHeight());
        }
        String n = skinName(b); if (n == null) return false;
        float w = n.equals("l") || n.equals("r") || n.equals("zl") || n.equals("zr") || n.equals("menu") ? r * 1.6f : r;
        return s.draw(c, n, x, y, w, r, on, 0, 0, 0, sa, getHeight());
    }
    private void drawEditPanel(Canvas c) {
        float w = getWidth(), h = getHeight(), pw = Math.min(w * 0.8f, 440 * dp), bh = 44 * dp, bw = (pw - 36 * dp) / 2, top = h * 0.30f;
        text.setTextSize(16 * dp);
        panelRect.set((w - pw) / 2, top, (w + pw) / 2, top + 28 * dp + bh + 24 * dp);
        fill.setColor(0xc0202020); c.drawRoundRect(panelRect, 12 * dp, 12 * dp, fill);
        text.setColor(0xffffffff); c.drawText(editHint, w / 2, top + 26 * dp, text);
        float by = top + 40 * dp;
        resetRect.set(panelRect.left + 12 * dp, by, panelRect.left + 12 * dp + bw, by + bh);
        doneRect.set(panelRect.right - 12 * dp - bw, by, panelRect.right - 12 * dp, by + bh);
        fill.setColor(0xff505050); c.drawRoundRect(resetRect, 8 * dp, 8 * dp, fill);
        fill.setColor(0xff2f7d32); c.drawRoundRect(doneRect, 8 * dp, 8 * dp, fill);
        c.drawText(resetLabel, resetRect.centerX(), resetRect.centerY() + 6 * dp, text);
        c.drawText(doneLabel, doneRect.centerX(), doneRect.centerY() + 6 * dp, text);
    }
}
