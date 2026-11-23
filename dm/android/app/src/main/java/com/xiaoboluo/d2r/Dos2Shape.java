package com.xiaoboluo.d2r;

// [DOS2] The picture shape on the Java side, pure (no Android types: host-tested with javac, tools/dos2_wide_shape_check.sh, against the
// native dm/host/hle_dos2_wide_size.h, which holds the reasoning). Issue #9: on a screen narrower than 16:9 (Galaxy Z Fold7 inner 2184x1968,
// Fold6, 16:10 / 4:3 tablets) the widescreen shape used to give the game a window of the screen's shape; the game then letterboxed itself to
// 16:9 and the 3D came out squeezed into a 2.86:1 band (characters 1.6x wide). Now widescreen there = the "16:9 with bars" picture: the native
// side makes the window 16:9 (DM_DOS2_NARROW unset / bars) and the SurfaceView is sized 16:9 and centred (bars()). Screens at or wider
// than 16:9: nothing changes. DM_DOS2_NARROW=letterbox / tall (dm/env) bring the old ways back (the view fills the screen again).
// Debug DM_DOS2_FAKE_SCREEN=<W>x<H> | <a>:<b> (dm/env): the game acts as if the screen had that shape and the view is put in a rectangle
// of that shape centred on the real screen (surfaceRect) - a foldable's inner screen on a normal phone, without touching the system's
// display size (wm size stretches the whole screen, the pad too).
final class Dos2Shape {
    private Dos2Shape() {}
    static final int NARROW_BARS = 0, NARROW_LETTERBOX = 1, NARROW_TALL = 2;
    static final double WIDE = 16.0 / 9.0;

    // DM_DOS2_NARROW / DM_DOS2_WIDE_TALL as hle_dos2_wide_size.h dws_narrow_mode reads them
    static int narrowMode(String narrow, String tall) {
        if (narrow != null && !narrow.isEmpty()) {
            if (narrow.equals("letterbox") || narrow.equals("game")) return NARROW_LETTERBOX;
            if (narrow.equals("tall")) return NARROW_TALL;
            return NARROW_BARS;
        }
        return tall != null && tall.startsWith("1") ? NARROW_TALL : NARROW_BARS;
    }
    // "<W>x<H>" / "<a>:<b>" -> long side / short side; 0 = not set / not usable (dws_fake_aspect)
    static double fakeAspect(String e) {
        if (e == null) return 0;
        String[] p = e.trim().split("[x:]");
        if (p.length != 2) return 0;
        try {
            long x = Long.parseLong(p[0].trim()), y = Long.parseLong(p[1].trim());
            if (x <= 0 || y <= 0) return 0;
            double a = x > y ? (double) x / y : (double) y / x;
            return a >= 1.0 && a < 4.0 ? a : 0;
        } catch (NumberFormatException ex) { return 0; }
    }
    // the screen aspect (long / short); 0 = unknown
    static double aspect(int sw, int sh) { return sw > 0 && sh > 0 ? (double) Math.max(sw, sh) / Math.min(sw, sh) : 0; }
    // narrower than 16:9 the way the gear's sizes see it (wideSize: within 0.01 of 16:9 counts as 16:9)
    static boolean narrower(double a) { return a > 0 && a < WIDE - 0.01; }

    // The widescreen request for a screen of aspect a at the base height b (DM_DOS2_WIDE=<W>x<H>, Tl2Gear.aspectEnv; dws_java_wide):
    // wider than 16:9 = W x b, narrower = (b x 16/9) x H; null = 16:9 at 900 (the Switch's 1600x900)
    static int[] wideSize(int b, double a) {
        if (a <= 0) return null;
        int bw = b * 16 / 9;   // 720 -> 1280, 900 -> 1600, 1080 -> 1920
        if (Math.abs(a - WIDE) < 0.01) return b == 900 ? null : new int[] {bw, b};
        if (a > WIDE) return new int[] {(int) Math.max(bw + 16, Math.min(3200, Math.round(b * a / 16) * 16)), b};
        return new int[] {bw, (int) Math.max(b + 16, Math.min(bw, Math.round(bw / a / 16) * 16))};
    }
    // What the native side makes of a DM_DOS2_WIDE=<W>x<H> request (dws_from_size, docked): rescaled to the base height, multiples of 8
    static int[] nativeWindow(int b, int[] req) {
        if (req == null) return null;
        long w = req[0], h = req[1];
        if (h >= 360 && h != b && b >= 720) { w = w * b / h; h = b; }
        if (!(w >= 640 && h >= 360 && w <= 4096 && h <= 4096 && (w != 1600 || h != 900))) return null;
        int dw = (int) Math.max(640, Math.min(3200, (w + 4) / 8 * 8)), dh = (int) Math.max(368, Math.min(2400, (h + 4) / 8 * 8));
        return new int[] {dw, dh};
    }
    // The docked window the game ends up with for a picture shape (0 = 16:9 with bars, 1 = stretched, 2 = widescreen) on a screen of
    // aspect a (the fake one when set) at base b; null = the Switch's 1600x900
    static int[] window(int shape, int b, double a, int mode) {
        if (shape != 2) return b == 900 ? null : nativeWindow(b, new int[] {b * 16 / 9, b});
        if (narrower(a) && mode == NARROW_BARS) return window(0, b, a, mode);   // issue #9: the 16:9 window of the bars shape
        return nativeWindow(b, wideSize(b, a));
    }
    // The SurfaceView is 16:9 and centred (black bars) for this shape on a screen of aspect a
    static boolean bars(int shape, double a, int mode) { return shape == 0 || (shape == 2 && narrower(a) && mode == NARROW_BARS); }

    // The SurfaceView's rectangle {x, y, w, h} in a frame fw x fh: the screen (the whole frame, or with a fake aspect > 0 a rectangle of that
    // shape centred in it), then 16:9 centred inside that when bars (GearDisplay.fitSurface's arithmetic: fh * 16 / 9, fw * 9 / 16)
    static int[] surfaceRect(int fw, int fh, boolean bars, double fake) {
        int sw = fw, sh = fh;
        if (fake > 0 && fw > 0 && fh > 0) {
            if ((double) fw / fh > fake) sw = (int) Math.round(fh * fake); else sh = (int) Math.round(fw / fake);
        }
        int w = sw, h = sh;
        if (bars && sw > 0 && sh > 0 && (long) sw * 9 > (long) sh * 16) w = sh * 16 / 9;
        else if (bars && sw > 0 && sh > 0 && (long) sw * 9 < (long) sh * 16) h = sw * 9 / 16;
        return new int[] {(fw - w) / 2, (fh - h) / 2, w, h};
    }
}
