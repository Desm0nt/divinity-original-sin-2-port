package com.xiaoboluo.d2r;

// [DOS2] The Java side of the widescreen table (issue #9): the same cases and the same line format as dos2_wide_size_test.c --table, from
// the shipped Dos2Shape.java (no Android types). tools/dos2_wide_shape_check.sh compiles both and diffs the outputs: the window the native
// side makes and the view the front end lays out must agree for every screen / fake screen / shape / base height / DM_DOS2_NARROW.
public final class Dos2ShapeTable {
    private static final Object[][] DEV = {
        {"fold7-inner", 2184, 1968}, {"fold6-inner", 2208, 1768}, {"tab-16:10", 2560, 1600}, {"tab-4:3", 2048, 1536}, {"ipad11-4:3", 2388, 1668},
        {"tab-16:10-720", 1280, 800}, {"phone-16:9", 1920, 1080}, {"phone-18:9", 2160, 1080}, {"phone-20:9", 2400, 1080}, {"mi14", 2670, 1200},
        {"phone-19.5:9", 2532, 1170}, {"phone-21:9", 3200, 1440}, {"phone-900", 1600, 900}, {"phone-720-20:9", 1600, 720}};
    private static final String[] FAKE = {"", "2184x1968", "2208x1768", "16:10", "4:3"};
    private static final String[] MODE = {"", "bars", "letterbox", "tall"};
    private static final String[] NAME = {"bars", "letterbox", "tall"};

    public static void main(String[] args) {
        StringBuilder out = new StringBuilder();
        for (Object[] d : DEV) for (String fake : FAKE) for (int shape = 0; shape < 3; shape++) for (String mode : MODE) {
            String name = (String) d[0]; int sw = (Integer) d[1], sh = (Integer) d[2];
            if (!fake.isEmpty() && !name.equals("mi14") && !name.equals("phone-20:9")) continue;
            int b = sh >= 1080 ? 1080 : sh >= 900 ? 900 : 720;   // Tl2Gear.dos2NativeRes
            int nm = Dos2Shape.narrowMode(mode, null);
            double fa = Dos2Shape.fakeAspect(fake), a = fa > 0 ? fa : Dos2Shape.aspect(sw, sh);
            int[] win = Dos2Shape.window(shape, b, a, nm);
            boolean bars = Dos2Shape.bars(shape, a, nm);
            int[] r = Dos2Shape.surfaceRect(sw, sh, bars, fa);
            out.append(String.format(java.util.Locale.ROOT, "dev=%s screen=%dx%d fake=%s base=%d shape=%d narrow=%s window=%dx%d view=%s rect=%d,%d,%d,%d\n",
                name, sw, sh, fake.isEmpty() ? "-" : fake, b, shape, NAME[nm], win != null ? win[0] : 1600, win != null ? win[1] : 900,
                bars ? "16:9" : "full", r[0], r[1], r[2], r[3]));
        }
        System.out.print(out);
    }
}
