package com.xiaoboluo.d2r;

import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.system.ErrnoException;
import android.system.Os;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;

// [TL2] Torchlight II's own gear sections and their order, on the shared gear (frontend-common GearSettings / GearPages, docs/FRONTEND_UNIFIED.md
// "Gear"). This file belongs to the TL2 port (dm/android/app/src/main/java/com/xiaoboluo/d2r/Tl2Gear.java; a reference copy sits in
// frontend-common/migration/tl2/), not to frontend-common's java/: the sync never touches it. DmActivity.gearSections returns sections(this, g).
// The dialog has the rows DmActivity.showD3Settings had before the move (parity table frontend-common/migration/tl2-parity.md: same prefs,
// environment, defaults; the generic rows are GearPages' with GearText's 13 languages, the TL2 rows keep their string resources d3_* / tl2_*
// in the menu language). TL2 rows here:
//   mode            Switch mode = render size (pref handheld, DM_OPMODE 1 / 0; labels show the size for the picked picture shape)   next launch
//   picture shape   GearPages.pictureShape: 16:9 bars / stretched / widescreen (pref aspect, default 2; DM_TL2_WIDE=<W> from the display)
//   loading stretch widescreen loading screens stretched (pref tl2_stretch_loading, default off; DM_TL2_WIDE_LOADSTRETCH)            next launch
//   mods            Tl2Mods: the .MOD list, its own dialog with its own Save (pref tl2_mods; converted at the next start)
//   layout          D3PadView / Tl2Touch layouts tl2 / tl2_side / gamepad (pad d3_layout, applies when picked) + skill icons (tl2_icons);
//                   [DOS2] gamepad (the Switch pad, default) first, then tl2 / tl2_side
//   touch           Tl2Touch: diagonal correction, second-set modifier, the optional extras (pad tl2_iso / tl2_mod / tl2_tap / tl2_hold_walk /
//                   tl2_hero_y / tl2_aim)
//   start env       no row: DM_TL2_POINTER=1 (always on), the DOOM cvars unset, the non-Adreno text diagnostics
// A Diablo III build of this tree (DM_GAME=d3) gets D3's rows instead of the TL2 ones where they differ (touch / gamepad, D3 skill icons,
// hide sticks, walking to the finger, menu swipes).
final class Tl2Gear {
    private Tl2Gear() {}
    static final boolean TL2 = DmActivity.TL2;
    static final int[] FPS_LIMITS = {30, 60};   // above 60 is not offered: the game runs its frames at 60 like on the Switch (present interval 1)

    // The dialog, top to bottom (and the start environment in the same order)
    static List<GearSettings.Section> sections(DmActivity a, GearSettings g) {
        Mode mode = new Mode(a);
        List<GearSettings.Section> l = new ArrayList<>();
        l.add(startEnv(a));
        l.add(GearPages.menuLanguage());
        l.add(GearPages.gameLanguage(DmActivity.D3_LANG_CODES, DmActivity.D3_LANG_LABELS, true));
        l.add(note(R.string.d3_game_lang_note));
        if (DmActivity.DOS2) l.add(dos2Res());   // [DOS2] before the mode section: its labels show the size
        l.add(mode);
        l.add(GearPages.pictureShape(new String[] {g.str(R.string.d3_aspect_bars), g.str(R.string.d3_aspect_stretch), g.str(R.string.d3_aspect_wide)},
            DmActivity.D3_ASPECT_DEFAULT, g.str(R.string.d3_aspect_wide_note), a::applyAspectLayout, (s, v) -> aspectEnv(a, v), mode::relabel));
        if (!DmActivity.DOS2) l.add(stretchLoading());   // [DOS2] no loading-screen option: its widescreen has no pillarboxed loading art (hle_dos2_wide.c)
        // TL2 mods: their own row in the pause menu (DmActivity.portRows -> Tl2Gear.showMods), not a gear section
        l.add(GearPages.sharpen());
        if (DmActivity.DOS2) { l.add(dos2Fps()); l.add(note(R.string.dos2_fps_note)); }   // [DOS2] 30 / 60 / 90 / 120 / uncapped (DM_DOS2_FPS)
        else { l.add(GearPages.fpsLimit(FPS_LIMITS, 60)); l.add(note(R.string.d3_fps_note)); }
        if (DmActivity.DOS2) { l.add(dos2Drs()); l.add(dos2Thermal()); l.add(dos2Touch()); l.add(dos2Column()); l.add(dos2LStick()); l.add(dos2RStick()); }   // [DOS2] hle_dos2_quality.c: DRS on/off + target fps; thermal ladder
        if (a.drivers() != null) { l.add(GearPages.driver(a.drivers())); l.add(note(R.string.d3_driver_note)); }
        l.add(GearPages.rumble());
        if (!DmActivity.DOS2) l.add(localPlay());   // [DOS2] TL2's LAN play (its Pia over ldn) is not wired / tested for DOS2: no row
        l.add(GearPages.fpsCounter());
        if (!DmActivity.DOS2) l.add(new Layout());   // [DOS2] one layout (the Switch pad, edited in its own editor): no TL2 / D3 layout list
        l.add(GearPages.padControls());
        l.add(GearPages.deadzone());
        if (!DmActivity.DOS2) l.add(TL2 ? new TouchRows() : new D3Touch());   // [DOS2] no TL2 touch rows (tap world, zoom, ...)
        l.add(GearPages.gearButton());
        return l;
    }

    // ---- [mp] local wireless play over the LAN (hle_ldn.c; pref "mp_lan", on by default -> DM_LDN): the game's own local multiplayer (Pia over
    // nn::ldn) between phones on one Wi-Fi / hotspot. No couch co-op row: TL2 on the Switch supports {Handheld, No1} only (main 0x13e9fc).
    // (frontend-common GearPages.localPlay is the same section with the shared texts; this one uses the port's resources until the gear texts sync.)
    static GearSettings.Section localPlay() {
        return new GearSettings.Section() {
            CheckBox lan;
            public void build(GearSettings g, LinearLayout box) {
                lan = g.check(box, g.when(g.str(R.string.d3_mp_lan), false), g.gamePrefs().getBoolean("mp_lan", true));
                g.note(box, g.str(R.string.tl2_mp_lan_note));
            }
            public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putBoolean("mp_lan", lan.isChecked()); }
            public void env(GearSettings g) throws ErrnoException { Os.setenv("DM_LDN", g.gamePrefs().getBoolean("mp_lan", true) ? "1" : "0", true); }
        };
    }

    // ---- a row-less note under the section before it
    static GearSettings.Section note(int res) {
        return new GearSettings.Section() { public void build(GearSettings g, LinearLayout box) { g.note(box, g.str(res)); } };
    }

    // ---- no row: the start environment that has no option
    static GearSettings.Section startEnv(DmActivity a) {
        return new GearSettings.Section() {
            public void build(GearSettings g, LinearLayout box) {}
            public void env(GearSettings g) throws ErrnoException {
                Os.unsetenv("DM_DOOM_DRS"); Os.unsetenv("DM_DOOM_RES");   // the DOOM cvars (rs_enable / rs_forceFraction*) do not apply
                if (TL2) Os.setenv("DM_TL2_POINTER", "1", true);   // hle_tl2_ui.c menu taps: always on (owner), no gear option; DM_TL2_UIDUMP only from dm/env
                // GPUs we could not test (no Adreno kernel driver: Mali, PowerVR, Xclipse): the existing text diagnostics, so one report shows what
                // the renderer did - the NVN calls of the first 120 frames, a per-pass GPU profile every 600 frames for the first ~2 minutes. Text
                // only; dm/env still overrides each value. Not DM_NVN_TRACE (its wrapper crashed the host's slot calls at boot, Mali 0.2.1).
                if (!VkDrivers.kgsl()) {
                    Os.setenv("DM_NVN_FRAMES", "120", false);
                    Os.setenv("DM_GFX_PASS_PROF", "60 600 7200", false);
                    a.crumb("diagnostics on (not an Adreno GPU: " + VkDrivers.hardwareSummary() + "): NVN calls of 120 frames, pass profile for ~2 min");
                }
            }
        };
    }

    // ---- the Switch operation mode = the game's render size (hle_oe.c GetOperationMode / GetPerformanceMode): docked 1920x1080 (D3 1600x900),
    // handheld 1280x720; the labels show the size the next launch renders at for the picked picture shape (widescreen: the screen's aspect).
    // Next launch here (DM_OPMODE); the pause menu's mode row switches it live (DmActivity.setDocked, the same pref).
    static final class Mode implements GearSettings.Section {
        final DmActivity a; RadioGroup r; GearSettings g;
        Mode(DmActivity a) { this.a = a; }
        public void build(GearSettings g, LinearLayout box) {
            this.g = g; SharedPreferences p = g.gamePrefs();
            g.heading(box, g.when(g.str(R.string.d3_mode), false));
            r = g.radios(box, new String[] {"", ""}, p.getBoolean("handheld", false) ? 1 : 0, false);
            relabel(p.getInt("aspect", DmActivity.D3_ASPECT_DEFAULT));
            g.note(box, g.str(R.string.d3_mode_note));
        }
        void relabel(int aspect) {
            if (r == null || g == null) return;
            int[] sc = a.screenSize(), dk = DmActivity.d3RenderSize(aspect == 2, false, sc[0], sc[1]), hh = DmActivity.d3RenderSize(aspect == 2, true, sc[0], sc[1]);
            ((RadioButton) r.getChildAt(0)).setText(g.str(R.string.d3_mode_docked, dk[0] + "×" + dk[1]));
            ((RadioButton) r.getChildAt(1)).setText(g.str(R.string.d3_mode_handheld, hh[0] + "×" + hh[1]));
        }
        public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putBoolean("handheld", GearSettings.picked(r) == 1); }
        public void env(GearSettings g) throws ErrnoException {
            a.startedHandheld = g.gamePrefs().getBoolean("handheld", false);
            Os.setenv("DM_OPMODE", a.startedHandheld ? "0" : "1", true);   // dm/env still wins (it is applied after this)
        }
    }
    // ---- picture shape 2 = widescreen (next launch): DOS2 hle_dos2_wide.c DM_DOS2_WIDE=<W>x<H> (Hor+; narrower screens 1600 x H, letterboxed
    // by the game itself); TL2 hle_tl2_wide.c W x 1080 Hor+, the width from the display, not the SurfaceView (the view
    // may still be laid out 16:9 before startedAspect is known, or portrait when started with the screen off); D3 hle_d3.c DM_D3_WIDE
    static void aspectEnv(DmActivity a, int v) throws ErrnoException {
        a.startedAspect = v;
        if (DmActivity.DOS2) {   // [DOS2] hle_dos2_wide.c: the docked window in the screen's shape (WxH, from the display: long side first), 0 = the Switch's 1600x900
            int[] sc = a.screenSize(), d = v == 2 ? DmActivity.dos2WideSize(sc[0], sc[1]) : DmActivity.dos2Base == 900 ? null : new int[] {1280, 720};
            Os.setenv("DM_DOS2_WIDE", d != null ? d[0] + "x" + d[1] : "0", true);
        } else if (TL2) {
            int[] sc = a.screenSize(); long ww = sc[1] > 0 ? Math.round(1080.0 * sc[0] / sc[1] / 16) * 16 : 0;
            Os.setenv("DM_TL2_WIDE", v != 2 ? "0" : ww > 1920 ? Long.toString(ww) : "1", true);
        } else if (v == 2) Os.setenv("DM_D3_WIDE", "1", true); else Os.unsetenv("DM_D3_WIDE");
    }
    // ---- widescreen loading screens: TL2 pillarboxed 16:9 by default, stretched when asked (hle_tl2_wide.c); D3 the other way round (ls_present_crop)
    // ---- [DOS2] render height: 720p by default (the Switch's DRS range is 495-720p; the widescreen widens the frame instead), 900p as an option
    // (next launch; heavier on the GPU). DmActivity.dos2Base feeds the docked / handheld labels and DM_DOS2_WIDE (aspectEnv)
    static final int[] DOS2_RES = {720, 900, 1080};
    static int dos2ResPref(GearSettings g) {   // "dos2_res" 720 / 900 / 1080; the old checkbox "dos2_res_900" is still read
        // owner 15.11 (Mi 13): the default is the screen's own height (1080 on a 2400x1080 phone; 900 / 720 on lower screens) - a player
        // lowers it if needed; DRS stays off by default (dos2Drs)
        SharedPreferences p = g.gamePrefs(); int r = p.getInt("dos2_res", p.getBoolean("dos2_res_900", false) ? 900 : dos2NativeRes(g));
        return r == 900 || r == 1080 ? r : 720;
    }
    static int dos2NativeRes(GearSettings g) {   // the largest of 720 / 900 / 1080 that the screen's short side holds (current display mode)
        try {
            android.util.DisplayMetrics m = new android.util.DisplayMetrics();
            ((android.view.WindowManager) g.a.getSystemService(android.content.Context.WINDOW_SERVICE)).getDefaultDisplay().getRealMetrics(m);
            int s = Math.min(m.widthPixels, m.heightPixels);
            return s >= 1080 ? 1080 : s >= 900 ? 900 : 720;
        } catch (RuntimeException e) { return 720; }
    }
    static GearSettings.Section dos2Res() {
        return new GearSettings.Section() {
            android.widget.RadioGroup r;
            public void build(GearSettings g, LinearLayout box) {
                int v = dos2ResPref(g); DmActivity.dos2Base = v;
                g.note(box, g.when(g.str(R.string.dos2_res), false));
                r = g.radios(box, new String[] {"720p", "900p", g.str(R.string.dos2_res_1080)}, v == 1080 ? 2 : v == 900 ? 1 : 0, true);
            }
            public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putInt("dos2_res", DOS2_RES[GearSettings.picked(r)]); }
            public void env(GearSettings g) throws ErrnoException { DmActivity.dos2Base = dos2ResPref(g); Os.setenv("DM_DOS2_RES", Integer.toString(DmActivity.dos2Base), true); }
        };
    }
    // ---- [DOS2] frame rate limit (next launch; docs/dos2/DOS2_PORT.md "Лимит кадров 90 / 120 / без ограничения"). Pref "fps_limit" as the
    // shared GearPages.fpsLimit row it replaces for DOS2 (a saved 30 / 60 stays), plus 90 / 120 / 0 = uncapped. 30 / 60 set DM_PRESENT_MAX_HZ
    // exactly as before (default 60: unchanged); 90 / 120 / uncapped set DM_DOS2_FPS (hle_nvn.c present_auto_hz -> nvn_pace.h
    // nvn_pace_target_hz). 90 / 120 are offered only when the display has a mode at a multiple of them (GearDisplay.modeFor); uncapped always
    // (no host pacing; the FIFO swapchain holds it to the refresh). dm/env DM_DOS2_FPS wins over the row (the env file is read later).
    static final int[] DOS2_FPS = {30, 60, 90, 120, 0};
    static boolean dos2FpsOffered(android.app.Activity a, int f) { return f == 0 || GearDisplay.fpsLimitAvailable(a, f); }
    // The saved choice, lowered to the highest offered one at or under it when its display mode is gone (another screen / refresh setting)
    static int dos2FpsPref(android.app.Activity a, SharedPreferences p) {
        int want = p.getInt("fps_limit", 60);
        if (want == 0) return 0;
        if (want <= 60) return want == 30 ? 30 : 60;
        int got = 60; for (int f : DOS2_FPS) if (f > got && f <= want && dos2FpsOffered(a, f)) got = f;
        return got;
    }
    static GearSettings.Section dos2Fps() {
        return new GearSettings.Section() {
            final ArrayList<Integer> fps = new ArrayList<>(); RadioGroup r;
            public void build(GearSettings g, LinearLayout box) {
                fps.clear(); for (int f : DOS2_FPS) if (dos2FpsOffered(g.a, f)) fps.add(f);
                String[] labels = new String[fps.size()]; int cur = dos2FpsPref(g.a, g.gamePrefs()), at = 1;
                for (int i = 0; i < labels.length; i++) { int f = fps.get(i); labels[i] = f == 0 ? g.str(R.string.dos2_fps_uncapped) : String.valueOf(f); if (f == cur) at = i; }
                g.heading(box, g.whenKey("gear_fps_limit", false));
                r = g.radios(box, labels, at, true);
            }
            public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) { if (!fps.isEmpty()) game.putInt("fps_limit", fps.get(GearSettings.picked(r))); }
            public void env(GearSettings g) throws ErrnoException {
                int f = dos2FpsPref(g.a, g.gamePrefs());
                if (f == 30 || f == 60) { Os.setenv("DM_PRESENT_MAX_HZ", String.valueOf(f), true); Os.unsetenv("DM_DOS2_FPS"); }
                else { Os.unsetenv("DM_PRESENT_MAX_HZ"); Os.setenv("DM_DOS2_FPS", String.valueOf(f), true); }
            }
        };
    }
    // After nativeStart (dm/env is in the environment by then): the effective target, one boot.log line with it and the display modes the phone
    // offers, and for 90 / 120 / uncapped the window's display mode (the lowest mode at a multiple of the target, the same resolution first;
    // uncapped: the fastest mode at the current resolution). 30 / 60 request nothing (as before: the surface's 120 Hz vote and the system decide).
    static void dos2FpsApply(DmActivity a) {
        String e = Os.getenv("DM_DOS2_FPS"), cap = Os.getenv("DM_PRESENT_MAX_HZ");
        int t = -1; if (e != null) for (int f : DOS2_FPS) if (e.trim().equals(String.valueOf(f))) t = f;
        String from = t >= 0 ? "DM_DOS2_FPS" : "DM_PRESENT_MAX_HZ " + (cap == null ? "unset" : cap);
        if (t < 0) t = "30".equals(cap) ? 30 : 60;
        android.view.Display d = GearDisplay.display(a);
        StringBuilder modes = new StringBuilder(); android.view.Display.Mode cur = d == null ? null : d.getMode(), pick = null;
        if (d != null) for (android.view.Display.Mode m : d.getSupportedModes()) {
            modes.append(' ').append(m.getModeId()).append('@').append(String.format(java.util.Locale.ROOT, "%.2f", m.getRefreshRate())).append('/').append(m.getPhysicalWidth()).append('x').append(m.getPhysicalHeight());
            if (t == 0 && cur != null && m.getPhysicalWidth() == cur.getPhysicalWidth() && m.getPhysicalHeight() == cur.getPhysicalHeight()
                && (pick == null || m.getRefreshRate() > pick.getRefreshRate())) pick = m;
        }
        if (t > 60) pick = GearDisplay.modeFor(a, t);
        String req = t > 60 || t == 0 ? (pick == null ? "none (no mode at a multiple: paced as is)" : pick.getModeId() + " @ " + pick.getRefreshRate() + " Hz") : "none (30 / 60)";
        if (pick != null && (t > 60 || t == 0)) {
            WindowManager.LayoutParams lp = a.getWindow().getAttributes(); lp.preferredDisplayModeId = pick.getModeId(); a.getWindow().setAttributes(lp);
        }
        a.crumb("fps target " + (t == 0 ? "uncapped" : String.valueOf(t)) + " (" + from + "): display modes" + (d == null ? " unknown" : modes.toString())
            + ", current " + (cur == null ? "?" : cur.getModeId() + " @ " + cur.getRefreshRate() + " Hz") + ", requested " + req);
    }
    // ---- [DOS2] dynamic resolution (next launch; hle_dos2_quality.c): off by default (the phone has the headroom the Switch lacked:
    // DM_DOS2_DRS=off = always the full window); on = the game's DRS with its target from the chosen frame rate (DM_DOS2_DRS_TARGET_MS =
    // 1000 / fps x 0.95, as DOOM 2016's DRS target)
    static final int[] DOS2_DRS_FPS = {30, 45, 60};
    static GearSettings.Section dos2Drs() {
        return new GearSettings.Section() {
            CheckBox c; android.widget.RadioGroup r;
            public void build(GearSettings g, LinearLayout box) {
                SharedPreferences p = g.gamePrefs();
                c = g.check(box, g.when(g.str(R.string.dos2_drs), true), p.getBoolean("dos2_drs", false));
                int f = p.getInt("dos2_drs_fps", 60); g.note(box, g.str(R.string.dos2_drs_target));
                r = g.radios(box, new String[] {"30", "45", "60"}, f == 30 ? 0 : f == 45 ? 1 : 2, true);
            }
            public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) {
                int fps = DOS2_DRS_FPS[GearSettings.picked(r)];
                game.putBoolean("dos2_drs", c.isChecked()); game.putInt("dos2_drs_fps", fps);
                // live, no restart (the game is running: its DRS target is our copy); a failed live call still applies at the next launch
                try { DmActivity.nativeSetDos2Drs(c.isChecked(), fps); } catch (Throwable t) { }
            }
            public void env(GearSettings g) throws ErrnoException {
                SharedPreferences p = g.gamePrefs();
                if (!p.getBoolean("dos2_drs", false)) { Os.setenv("DM_DOS2_DRS", "off", true); return; }
                int f = p.getInt("dos2_drs_fps", 60);
                Os.unsetenv("DM_DOS2_DRS"); Os.setenv("DM_DOS2_DRS_TARGET_MS", String.format(java.util.Locale.ROOT, "%.2f", 1000.0 / f * 0.95), true);
            }
        };
    }
    // ---- [DOS2] thermal mode (next launch): hle_dos2_quality.c DM_DOS2_THERMAL=1, the ladder on the AThermal headroom (DRS ceiling, then the
    // shadow quality, then bloom). Off by default; when off nothing is set, so an env-file DM_DOS2_THERMAL still works
    static GearSettings.Section dos2Thermal() {
        return new GearSettings.Section() {
            CheckBox c;
            public void build(GearSettings g, LinearLayout box) {
                c = g.check(box, g.when(g.str(R.string.dos2_thermal), false), g.gamePrefs().getBoolean("dos2_thermal", false));
            }
            public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putBoolean("dos2_thermal", c.isChecked()); }
            public void env(GearSettings g) throws ErrnoException { if (g.gamePrefs().getBoolean("dos2_thermal", false)) Os.setenv("DM_DOS2_THERMAL", "1", true); }
        };
    }
    // ---- [DOS2] touch as mouse (next launch; experiment): hle_dos2_touch.c DM_DOS2_TOUCH=1, touches off the pad's buttons and stick homes
    // become mouse records in the engine's own InjectInput (tap = left click, drag = left drag, hold = right click, pinch = zoom, two-finger
    // drag = wheel), the iPad way. On by default (owner 15.11, Mi 13); when off nothing is set, so an env-file DM_DOS2_TOUCH still works for the hook (the pad
    // then still keeps the touches)
    static GearSettings.Section dos2Touch() {
        return new GearSettings.Section() {
            CheckBox c;
            public void build(GearSettings g, LinearLayout box) {
                c = g.check(box, g.when(g.str(R.string.dos2_touch), false), g.gamePrefs().getBoolean("dos2_touch", true));
            }
            public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putBoolean("dos2_touch", c.isChecked()); }
            public void env(GearSettings g) throws ErrnoException {
                DmActivity.dos2Touch = g.gamePrefs().getBoolean("dos2_touch", true);
                if (DmActivity.dos2Touch) Os.setenv("DM_DOS2_TOUCH", "1", true);
            }
        };
    }
    // ---- [DOS2] the iPad button column of the console HUD (bottomBar_d, tools/dos2_hud_touch.py; owner 15.11): shown / folded (a small arrow at the
    // right edge unfolds it) / hidden, and what a connected gamepad does to it: hide / fold (default) / leave as is. Live (DmActivity.dos2ColumnApply ->
    // nativeSetDos2Column -> hle_dos2_touchui_vis.c; the movie asks twice a second); the start state also goes into DM_DOS2_COLUMN
    // [DOS2] owner 15.11: the left stick can be switched off - then a tap anywhere on the floor walks; zoom = pinch, camera = two fingers
    // left / right (hle_dos2_touch.c). The hint is the note under the switch. Live (D3PadView reads DmActivity.dos2LStickOff).
    static GearSettings.Section dos2LStick() {
        return new GearSettings.Section() {
            android.widget.RadioGroup r;
            public void build(GearSettings g, LinearLayout box) {
                g.note(box, g.str(R.string.dos2_lstick));
                r = g.radios(box, new String[] {g.str(R.string.dos2_lstick_on), g.str(R.string.dos2_lstick_off)}, g.gamePrefs().getBoolean("dos2_lstick_hide", false) ? 1 : 0, true);
                g.note(box, g.str(R.string.dos2_lstick_hint));
            }
            public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pd) {
                DmActivity.dos2LStickOff = GearSettings.picked(r) == 1; game.putBoolean("dos2_lstick_hide", DmActivity.dos2LStickOff);
            }
            public void env(GearSettings g) throws ErrnoException { DmActivity.dos2LStickOff = g.gamePrefs().getBoolean("dos2_lstick_hide", false); }
        };
    }
    // [DOS2] owner 16.11: the right stick hidden by default (one-finger swipe / two fingers turn the camera, pinch zooms); can be shown here. Live.
    static GearSettings.Section dos2RStick() {
        return new GearSettings.Section() {
            android.widget.RadioGroup r;
            public void build(GearSettings g, LinearLayout box) {
                g.note(box, g.str(R.string.dos2_rstick));
                r = g.radios(box, new String[] {g.str(R.string.dos2_lstick_on), g.str(R.string.dos2_rstick_off)}, g.gamePrefs().getBoolean("dos2_rstick_hide", true) ? 1 : 0, true);
            }
            public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pd) {
                DmActivity.dos2RStickOff = GearSettings.picked(r) == 1; game.putBoolean("dos2_rstick_hide", DmActivity.dos2RStickOff);
            }
            public void env(GearSettings g) throws ErrnoException { DmActivity.dos2RStickOff = g.gamePrefs().getBoolean("dos2_rstick_hide", true); }
        };
    }
    static GearSettings.Section dos2Column() {
        return new GearSettings.Section() {
            android.widget.RadioGroup base, pad;
            public void build(GearSettings g, LinearLayout box) {
                SharedPreferences p = g.gamePrefs();
                g.note(box, g.str(R.string.dos2_column));
                base = g.radios(box, new String[] {g.str(R.string.dos2_column_shown), g.str(R.string.dos2_column_folded), g.str(R.string.dos2_column_hidden)}, p.getInt("dos2_col", 0), true);
                g.note(box, g.str(R.string.dos2_column_pad));
                pad = g.radios(box, new String[] {g.str(R.string.dos2_column_pad_hide), g.str(R.string.dos2_column_pad_fold), g.str(R.string.dos2_column_pad_keep)}, p.getInt("dos2_col_pad", 1), true);
            }
            public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pd) {
                DmActivity.dos2ColBase = GearSettings.picked(base); DmActivity.dos2ColPad = GearSettings.picked(pad);
                game.putInt("dos2_col", DmActivity.dos2ColBase); game.putInt("dos2_col_pad", DmActivity.dos2ColPad);
                DmActivity.dos2ColumnApply();   // live
            }
            public void env(GearSettings g) throws ErrnoException {
                SharedPreferences p = g.gamePrefs();
                DmActivity.dos2ColBase = Math.max(0, Math.min(2, p.getInt("dos2_col", 0))); DmActivity.dos2ColPad = Math.max(0, Math.min(2, p.getInt("dos2_col_pad", 1)));
                Os.setenv("DM_DOS2_COLUMN", Integer.toString(DmActivity.dos2ColumnState()), true);
            }
        };
    }
    static GearSettings.Section stretchLoading() {
        return new GearSettings.Section() {
            CheckBox c;
            public void build(GearSettings g, LinearLayout box) { c = g.check(box, g.when(g.str(R.string.d3_stretch_loading), false), g.gamePrefs().getBoolean(DmActivity.STRETCH_LOADING_KEY, DmActivity.STRETCH_LOADING_DEFAULT)); }
            public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) { game.putBoolean(DmActivity.STRETCH_LOADING_KEY, c.isChecked()); }
            public void env(GearSettings g) throws ErrnoException {
                String on = g.gamePrefs().getBoolean(DmActivity.STRETCH_LOADING_KEY, DmActivity.STRETCH_LOADING_DEFAULT) ? "1" : "0";
                Os.setenv(TL2 ? "DM_TL2_WIDE_LOADSTRETCH" : "DM_D3_WIDE_STRETCH_LOADING", on, true);
            }
        };
    }

    // ---- the mod manager (Tl2Mods): every .MOD in Download/Torchlight2Mobile/mods with title, author, version from its header, a tick to enable
    // it and up / down for the load order (lower = loaded later, wins). Its own dialog, saved with its own "Save"; the next start converts and
    // loads them (DmActivity.prepareTl2Mods).
    static GearSettings.Section mods(DmActivity a) {
        return new GearSettings.Section() {
            public void build(GearSettings g, LinearLayout box) {
                g.heading(box, g.str(R.string.tl2_mods_title));
                Button b = g.button(box, g.str(R.string.tl2_mods_button, Tl2Mods.enabledCount(g.gamePrefs())));
                b.setOnClickListener(v -> showMods(g, b));
                g.note(box, g.str(R.string.tl2_mods_restart));
            }
        };
    }
    static void showMods(GearSettings g, Button from) {
        SharedPreferences p = g.gamePrefs(); final List<Tl2Mods.Mod> mods = Tl2Mods.list(p);
        LinearLayout box = g.vbox(null); box.setPadding(48, 24, 48, 0);
        g.note(box, g.str(R.string.tl2_mods_folder, Tl2Mods.modsDir().getAbsolutePath()));
        final LinearLayout rows = g.vbox(box);
        final Runnable[] fill = new Runnable[1];
        fill[0] = () -> {
            rows.removeAllViews();
            if (mods.isEmpty()) { TextView t = g.text(rows, g.str(R.string.tl2_mods_none)); t.setPadding(0, 24, 0, 24); }
            for (int i = 0; i < mods.size(); i++) {
                final int at = i; final Tl2Mods.Mod m = mods.get(i);
                LinearLayout row = new LinearLayout(g.a); row.setOrientation(LinearLayout.HORIZONTAL); row.setPadding(0, 12, 0, 12);
                CheckBox c = new CheckBox(g.a);
                String sub = m.info == null ? g.str(R.string.tl2_mods_bad, m.error)
                    : g.str(R.string.tl2_mods_line, (m.info.author.isEmpty() ? "" : m.info.author + " · ") + m.file.getName(), m.info.modver);
                c.setText(Tl2Mods.describe(m) + "\n" + sub); c.setEnabled(m.info != null); c.setChecked(m.on && m.info != null);
                c.setOnCheckedChangeListener((b, on) -> m.on = on);
                row.addView(c, new LinearLayout.LayoutParams(0, -2, 1f));
                Button up = new Button(g.a), down = new Button(g.a);
                up.setText("▲"); down.setText("▼"); up.setEnabled(i > 0); down.setEnabled(i < mods.size() - 1);
                up.setOnClickListener(v -> { java.util.Collections.swap(mods, at, at - 1); fill[0].run(); });
                down.setOnClickListener(v -> { java.util.Collections.swap(mods, at, at + 1); fill[0].run(); });
                row.addView(up, new LinearLayout.LayoutParams(-2, -2)); row.addView(down, new LinearLayout.LayoutParams(-2, -2));
                rows.addView(row);
            }
        };
        fill[0].run();
        g.note(box, g.str(R.string.tl2_mods_restart));
        android.widget.ScrollView scroll = new android.widget.ScrollView(g.a); scroll.addView(box);
        new AlertDialog.Builder(g.a).setTitle(g.str(R.string.tl2_mods_title)).setView(scroll)
            .setPositiveButton(g.s("gear_save"), (d, w) -> {
                String before = enabledList(p); Tl2Mods.save(p, mods); if (from != null) from.setText(g.str(R.string.tl2_mods_button, Tl2Mods.enabledCount(p)));
                if (!before.equals(enabledList(p))) askRestart(g, Tl2Mods.enabledCount(p) > 0);   // the enabled set or its order changed
            })
            .setNegativeButton(g.s("gear_cancel"), null).show();
    }

    // the enabled mods in load order (what the next start converts and loads)
    static String enabledList(SharedPreferences p) {
        StringBuilder b = new StringBuilder();
        for (String l : p.getString(Tl2Mods.PREF, "").split("\n")) if (l.endsWith("\t1")) b.append(l).append('\n');
        return b.toString();
    }
    // mods load only at the start: offer the graceful quit + reopen right away (DmActivity.restartGame)
    static void askRestart(GearSettings g, boolean anyOn) {
        new AlertDialog.Builder(g.a).setTitle(g.str(R.string.tl2_mods_restart_q))
            .setMessage(g.str(anyOn ? R.string.tl2_mods_restart_on : R.string.tl2_mods_restart_off))
            .setPositiveButton(g.str(R.string.tl2_mods_restart_now), (d, w) -> { if (g.a instanceof DmActivity) ((DmActivity) g.a).restartGame(); })
            .setNegativeButton(g.str(R.string.tl2_mods_restart_later), null).show();
    }

    // ---- the on-screen control layout (D3PadView "d3_layout"; applies when picked: the pad is visible behind the dialog). TL2: the D2R Mobile-style
    // layouts (Tl2Touch) "tl2" sticks + skill fan (default), "tl2_side" quick-access cells on the sides, or the gamepad; D3: touch / gamepad.
    // Below it the skill icons on the buttons (TL2: Tl2Touch "tl2_icons", hle_tl2_icons.c; D3: D3SkillIcons "d3_icons" + "d3_hide_sticks").
    static final class Layout implements GearSettings.Section {
        CheckBox icons, hide;
        public void build(GearSettings g, LinearLayout box) {
            SharedPreferences cp = g.padPrefs();
            g.heading(box, g.when(g.str(R.string.d3_layout), true));
            // [DOS2] the Switch pad first (its default, D3PadView.LAYOUT_DEFAULT), TL2's two touch layouts as alternatives
            final String[] ids = DmActivity.DOS2 ? new String[] {D3PadView.LAYOUT_GAMEPAD, D3PadView.LAYOUT_TL2, D3PadView.LAYOUT_TL2_SIDE}
                : TL2 ? new String[] {D3PadView.LAYOUT_TL2, D3PadView.LAYOUT_TL2_SIDE, D3PadView.LAYOUT_GAMEPAD} : new String[] {D3PadView.LAYOUT_TOUCH, D3PadView.LAYOUT_GAMEPAD};
            String now = cp.getString("d3_layout", D3PadView.LAYOUT_DEFAULT);
            int at = TL2 ? 0 : ids.length - 1;   // an unknown value: what D3PadView falls back to (DOS2 gamepad, TL2 tl2, D3 gamepad)
            for (int i = 0; i < ids.length; i++) if (ids[i].equals(now)) at = i;
            String[] labels = DmActivity.DOS2 ? new String[] {g.str(R.string.dos2_layout_switch), g.str(R.string.tl2_layout_touch), g.str(R.string.tl2_layout_side)}
                : TL2 ? new String[] {g.str(R.string.tl2_layout_touch), g.str(R.string.tl2_layout_side), g.str(R.string.d3_layout_gamepad)}
                      : new String[] {g.str(R.string.d3_layout_touch), g.str(R.string.d3_layout_gamepad)};
            RadioGroup r = g.radios(box, labels, at, false);
            r.setOnCheckedChangeListener((rg, id) -> { cp.edit().putString("d3_layout", ids[GearSettings.picked(rg)]).apply(); g.reloadPad(); });
            g.note(box, g.str(DmActivity.DOS2 ? R.string.dos2_layout_note : TL2 ? R.string.tl2_layout_note : R.string.d3_layout_note));
            if (TL2) icons = g.check(box, g.when(g.str(R.string.d3_skill_icons), true), cp.getBoolean("tl2_icons", true));
            else {
                icons = g.check(box, g.when(g.str(R.string.d3_skill_icons), true), cp.getBoolean("d3_icons", true));
                hide = g.check(box, g.when(g.str(R.string.d3_hide_sticks), true), cp.getBoolean("d3_hide_sticks", true));
                g.note(box, g.str(R.string.d3_skill_icons_note));
            }
        }
        public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) {
            pad.putBoolean(TL2 ? "tl2_icons" : "d3_icons", icons.isChecked());
            if (hide != null) pad.putBoolean("d3_hide_sticks", hide.isChecked());
        }
    }

    // ---- [TL2] the D2R-style touch layout (Tl2Touch, pad prefs tl2_*): sliders live, the rest on Save; the extras are off by default
    static final class TouchRows implements GearSettings.Section {
        RadioGroup mod, tap, aim; CheckBox walk;
        public void build(GearSettings g, LinearLayout box) {
            SharedPreferences cp = g.padPrefs();
            g.heading(box, g.when(g.str(R.string.tl2_touch), true));
            g.slider(box, cp, g.str(R.string.tl2_iso), "tl2_iso", Tl2Touch.ISO_MIN, Tl2Touch.ISO_MAX, Tl2Touch.ISO_DEFAULT, g::reloadPad);
            g.note(box, g.str(R.string.tl2_iso_note));
            g.heading(box, g.str(R.string.tl2_mod));
            mod = g.radios(box, Tl2Touch.MOD_NAMES, cp.getInt("tl2_mod", 0), true);
            g.note(box, g.str(R.string.tl2_mod_note));
            g.heading(box, g.str(R.string.tl2_extras));
            g.heading(box, g.str(R.string.tl2_tap));
            tap = g.radios(box, new String[] {g.str(R.string.d3_off), g.str(R.string.tl2_tap_a), g.str(R.string.tl2_tap_face)}, cp.getInt("tl2_tap", Tl2Touch.TAP_OFF), false);
            walk = g.check(box, g.str(R.string.tl2_hold_walk), cp.getBoolean("tl2_hold_walk", false));
            g.slider(box, cp, g.str(R.string.d3_hero_y), "tl2_hero_y", Tl2Touch.HERO_Y_MIN, Tl2Touch.HERO_Y_MAX, Tl2Touch.HERO_Y_DEFAULT, g::reloadPad);
            g.heading(box, g.str(R.string.tl2_aim));
            aim = g.radios(box, new String[] {g.str(R.string.d3_off), g.str(R.string.tl2_aim_right), g.str(R.string.tl2_aim_left)}, cp.getInt("tl2_aim", Tl2Touch.AIM_OFF), false);
        }
        public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) {
            pad.putInt("tl2_tap", GearSettings.picked(tap)).putBoolean("tl2_hold_walk", walk.isChecked()).putInt("tl2_aim", GearSettings.picked(aim))
               .putInt("tl2_mod", GearSettings.picked(mod));
        }
    }
    // ---- [D3] its touch layout: walking toward a held finger (D3PadView.HERO_* / DEPTH_*, live sliders) and what menu-mode swipes press
    static final class D3Touch implements GearSettings.Section {
        RadioGroup swipe;
        public void build(GearSettings g, LinearLayout box) {
            SharedPreferences cp = g.padPrefs();
            g.heading(box, g.when(g.str(R.string.d3_touch_walk), true));
            g.slider(box, cp, g.str(R.string.d3_hero_y), "d3_hero_y", D3PadView.HERO_Y_MIN, D3PadView.HERO_Y_MAX, D3PadView.HERO_Y_DEFAULT, g::reloadPad);
            g.slider(box, cp, g.str(R.string.d3_depth), "d3_depth", D3PadView.DEPTH_MIN, D3PadView.DEPTH_MAX, D3PadView.DEPTH_DEFAULT, g::reloadPad);
            g.note(box, g.str(R.string.d3_touch_walk_note));
            g.heading(box, g.when(g.str(R.string.d3_menu_swipe), true));
            swipe = g.radios(box, new String[] {g.str(R.string.d3_menu_swipe_stick), g.str(R.string.d3_menu_swipe_dpad)}, cp.getInt("d3_menu_swipe", D3PadView.MENU_SWIPE_STICK), true);
        }
        public void save(GearSettings g, SharedPreferences.Editor game, SharedPreferences.Editor pad) { pad.putInt("d3_menu_swipe", GearSettings.picked(swipe)); }
    }
}
