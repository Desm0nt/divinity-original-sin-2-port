// [DOS2] NR-DRIFT host test (docs/dos2/nr/NR-DRIFT.md): dm/gfx/nr/nr_drift.h - load screens from the backend's draws a frame, the
// DM_DOS2_DRS_SETTLE hold window (apply / count / off), the GPU clock probe calibration and index, and a replay of the DRS
// controller model (hle_dos2_quality.c: step down to the size its model fits at once, step up 1.25 % a second) over the perf windows
// of the 19.11 phone run O_ab_a1 (load into Fort Joy) with and without the hold.
// Build: clang -std=c11 -O2 -Wall -Wextra nr_drift_test.c -lm   (no other file)
#include "../../../nr/nr_drift.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; if (fails < 30) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
#define S(x) ((uint64_t)((x) * 1e9))

static void test_load(void) {
    nrd_load_t l; nrd_load_init(&l);
    CHECK(l.in_load == 1, "boot counts as a load");
    CHECK(nrd_load_step(&l, 13.0, S(1)) == 0, "load screen stays a load");
    CHECK(nrd_load_step(&l, 1228.0, S(2)) == 1 && l.loads == 1 && l.load_end_ns == S(2), "3D menu ends the boot load");
    CHECK(nrd_load_step(&l, 1300.0, S(3)) == 0, "menu");
    CHECK(nrd_load_step(&l, 14.0, S(4)) == -1 && l.in_load, "load screen begins");
    CHECK(nrd_load_step(&l, 63.9, S(5)) == 0, "63.9 draws = still a load");
    CHECK(nrd_load_step(&l, 64.0, S(6)) == 1 && l.loads == 2, "64 draws ends it");
    CHECK(nrd_load_step(&l, -1.0, S(7)) == -1, "no window (-1) = a load");
}

static void test_settle(void) {
    nrd_settle_t s; int ev;
    nrd_settle_init(&s, NRD_SETTLE_OFF, S(6));
    CHECK(nrd_settle_step(&s, 1, S(0), 100, &ev) == 0 && ev == 0 && s.windows == 0, "off: never holds, never counts");
    nrd_settle_init(&s, NRD_SETTLE_APPLY, S(6));
    CHECK(nrd_settle_step(&s, 1, S(0), 100, &ev) == 1 && ev == 1 && s.windows == 1, "apply: holds while loading, window opens");
    CHECK(nrd_settle_step(&s, 1, S(5), 100, &ev) == 1 && ev == 0, "still loading");
    CHECK(nrd_settle_step(&s, 0, S(10), 100, &ev) == 1 && ev == 0 && s.until_ns == S(16), "load ended: hold 6 s from here");
    CHECK(nrd_settle_step(&s, 0, S(15.9), 100, &ev) == 1, "inside the 6 s");
    CHECK(nrd_settle_step(&s, 1, S(15.95), 100, &ev) == 1 && s.until_ns == 0 && s.windows == 1, "a new load inside the window extends it (same window)");
    CHECK(nrd_settle_step(&s, 0, S(16), 100, &ev) == 1 && s.until_ns == S(22), "and the 6 s restart at its end");
    CHECK(nrd_settle_step(&s, 0, S(22), 100, &ev) == 0 && ev == 2 && s.released == 1 && s.drops_inside == 0, "released at 22 s, no drop");
    CHECK(nrd_settle_step(&s, 0, S(40), 80, &ev) == 0 && ev == 0, "outside a window nothing happens (the controller is free)");
    // count mode: never holds, but sees the controller drop inside the window
    nrd_settle_init(&s, NRD_SETTLE_COUNT, S(6));
    CHECK(nrd_settle_step(&s, 1, S(0), 100, &ev) == 0 && ev == 1, "count: window opens, no hold");
    CHECK(nrd_settle_step(&s, 0, S(1), 100, &ev) == 0, "load ends");
    CHECK(nrd_settle_step(&s, 0, S(1.25), 75, &ev) == 0 && s.min_pct_inside == 75, "the spike drops it to 75 %%");
    CHECK(nrd_settle_step(&s, 0, S(7), 76.25f, &ev) == 0 && ev == 2 && s.drops_inside == 1, "window closes with one drop counted");
    CHECK(nrd_settle_step(&s, 1, S(8), -1, &ev) == 0 && ev == 1 && s.start_pct == 100, "unknown height = 100 at the start");
}

static void test_probe(void) {
    nrd_probe_t p; nrd_probe_init(&p);
    CHECK(p.n == NRD_PROBE_N0, "start n");
    // a 900 MHz-like GPU: 5 ns / iteration -> n0 = 4096 takes 20.5 us: too short, calibrate to ~80 us
    double ns_iter = 5.0;
    CHECK(nrd_probe_alu(&p, p.n, p.n * ns_iter / 1000.0) == 0 && p.recal == 1 && p.n > 4096, "too short: re-calibrated (n %u)", p.n);
    const double us = p.n * ns_iter / 1000.0;
    CHECK(us >= 70 && us <= 90, "next run ~80 us (%.1f)", us);
    CHECK(nrd_probe_alu(&p, 4096, 20.0) == 0, "a run recorded before the re-calibration is ignored");
    CHECK(nrd_probe_alu(&p, p.n, us) == 1 && fabs(nrd_probe_index(&p) - 100.0) < 1e-9, "first stable run = index 100");
    CHECK(nrd_probe_alu(&p, p.n, us * 1.25) == 1 && fabs(nrd_probe_index(&p) - 80.0) < 1e-6, "25 %% longer = index 80 (%.3f)", nrd_probe_index(&p));
    CHECK(fabs(p.last_ns_iter - 6.25) < 1e-9, "ns / iteration of the last run");
    // a very slow GPU: re-calibrates down
    nrd_probe_t q; nrd_probe_init(&q);
    CHECK(nrd_probe_alu(&q, q.n, 2000.0) == 0 && q.n < 4096 && q.n >= 256, "too long: re-calibrated down (n %u)", q.n);
    CHECK(nrd_probe_alu(&q, q.n, q.n * (2000.0 / 4096) ) == 1, "then counts (%.1f us)", q.n * (2000.0 / 4096));
    CHECK(nrd_probe_index(&q) == 100.0, "index 100");
    // bad inputs
    CHECK(nrd_probe_alu(&q, q.n, 0) == 0 && nrd_probe_alu(&q, q.n, -5) == 0 && nrd_probe_alu(&q, 0, 50) == 0, "zero / negative ignored");
    nrd_probe_t z; nrd_probe_init(&z);
    CHECK(nrd_probe_index(&z) < 0, "no run yet: index unknown");
    CHECK(fabs(nrd_copy_gbs(4u << 20, 100.0) - 83.886) < 0.01, "4 MB in 100 us = 83.9 GB/s of traffic (%.3f)", nrd_copy_gbs(4u << 20, 100.0));
    CHECK(nrd_copy_gbs(1, 0) < 0, "no time");
    // the calibration converges for any clock from 50 MHz-like to 3 GHz-like within 3 steps
    for (double ns = 0.3; ns < 120; ns *= 1.37) {
        nrd_probe_t r; nrd_probe_init(&r); int steps = 0;
        while (!nrd_probe_alu(&r, r.n, r.n * ns / 1000.0) && steps < 10) steps++;
        const double t = r.n * ns / 1000.0;
        CHECK(steps <= 3 && t >= NRD_PROBE_MIN_US && t <= NRD_PROBE_MAX_US, "ns/iter %.2f: %d steps, %.1f us", ns, steps, t);
    }
}

// The game's DRS controller as hle_dos2_quality.c describes it (0xc05da8..): evaluate each frame after 4 frames since a change; GPU >
// target - 0.5 -> the largest size the model (cost ~ gpu * (0.5 + 0.5 a) / (0.5 + 0.5 a_cur)) fits under the target, not below the
// minimum height 75 %; predicted cost of one step up (1.25 % of the height) under target x 30 / 31.5 for 1 s -> one step up.
typedef struct { double h; int since; double up_ms; } drs_t;
static double area(double h) { return (h / 100.0) * (h / 100.0); }
static uint32_t *g_spike_pg;   // DM_DOS2_DRS_SPIKE model page (NULL = today): the cave runs after the 4-frame gate, as in the game
static void drs_frame(drs_t *d, double gpu_at_full, double frame_ms, double target) {
    double gpu = gpu_at_full * (0.35 + 0.65 * area(d->h));   // the frame's GPU time at the current size (65 % scales with pixels)
    if (++d->since < 4) return;
    if (g_spike_pg) { nrd_spike_setf(g_spike_pg, NRD_PG_TARGET, (float)target); gpu = nrd_spike_step(g_spike_pg, (float)gpu); }
    if (target - gpu < 0.5) {
        double h = d->h;
        while (h > 75.0 && gpu * (0.5 + 0.5 * area(h)) / (0.5 + 0.5 * area(d->h)) > target - 0.5) h -= 2.5;
        if (h < 75.0) h = 75.0;
        if (h != d->h) { d->h = h; d->since = 0; d->up_ms = 0; }
        return;
    }
    if (d->h >= 100.0) return;
    const double up = gpu * (0.5 + 0.5 * area(d->h + 1.25)) / (0.5 + 0.5 * area(d->h));
    if (up < target * 30.0 / 31.5) { d->up_ms += frame_ms; if (d->up_ms >= 1000.0) { d->h += 1.25; if (d->h > 100) d->h = 100; d->since = 0; d->up_ms = 0; } }
    else d->up_ms = 0;
}
static void test_replay(void) {
    // O_ab_a1 (19.11, Fort Joy, uncapped, target 15.83 ms): the steady GPU at full size is 11.85 ms; the load ends with ~40 frames of
    // up to 29 ms ("max 29.12" in the 10-s line). Replay 60 s of frames with and without the hold.
    for (int hold = 0; hold <= 1; hold++) {
        drs_t d = {100.0, 4, 0}; nrd_settle_t s; nrd_settle_init(&s, hold ? NRD_SETTLE_APPLY : NRD_SETTLE_OFF, S(6));
        nrd_load_t l; nrd_load_init(&l);
        double t = 0, low_s = 0, min_h = 100; int ev;
        while (t < 60.0) {
            const int loading = t < 2.0;               // 2 s of load screen
            const double spike = (!loading && t < 2.7) ? 27.0 : 11.85;   // ~40 frames of spikes right after it
            const double gpu_full = loading ? 1.3 : spike;
            const int h = nrd_settle_step(&s, loading, S(t), (float)d.h, &ev);
            const double tgt = h ? 100.0 : 15.83;
            const double gpu = gpu_full * (0.35 + 0.65 * area(d.h));
            const double frame = gpu > 8.33 ? gpu : 8.33;
            drs_frame(&d, gpu_full, frame, tgt);
            if (d.h < 99.99) low_s += frame / 1000.0;
            if (d.h < min_h) min_h = d.h;
            t += frame / 1000.0;
        }
        if (!hold) CHECK(min_h <= 75.0 + 1e-9 && low_s > 15.0, "today: the spikes throw the DRS to 75 %% and it needs %.1f s to climb back (min %.1f)", low_s, min_h);
        else CHECK(min_h >= 99.99 && low_s == 0, "with the hold: full size all the time (min %.1f, %.1f s below 100 %%)", min_h, low_s);
        printf("replay %s: lowest height %.1f %%, %.1f s below 100 %%\n", hold ? "DM_DOS2_DRS_SETTLE=1" : "today", min_h, low_s);
    }
}

static void test_spike(void) {
    uint32_t pg[NRD_PG_WORDS] = {0};
    nrd_spike_setf(pg, NRD_PG_TARGET, 15.83f);
    nrd_spike_init(pg, 0);
    CHECK(nrd_spike_step(pg, 30.0f) == 30.0f && pg[NRD_PG_OVER] == 0, "mode 0: raw, nothing counted");
    // min of 2
    nrd_spike_init(pg, NRD_SPIKE_APPLY | NRD_SPIKE_COUNT);
    CHECK(nrd_spike_step(pg, 11.9f) == 11.9f, "first sample passes (no history)");
    CHECK(nrd_spike_step(pg, 29.0f) == 11.9f && pg[NRD_PG_OVER] == 1 && pg[NRD_PG_SUPP] == 1, "one spike: the size is kept, counted");
    CHECK(nrd_spike_step(pg, 29.0f) == 29.0f && pg[NRD_PG_OVER] == 2 && pg[NRD_PG_SUPP] == 1, "min of 2: the second slow sample in a row steps down");
    CHECK(nrd_spike_step(pg, 12.0f) == 12.0f, "back to normal");
    // min of 3
    nrd_spike_init(pg, NRD_SPIKE_APPLY | NRD_SPIKE_COUNT | NRD_SPIKE_MIN3);
    nrd_spike_step(pg, 11.9f); nrd_spike_step(pg, 12.1f);
    CHECK(nrd_spike_step(pg, 25.0f) == 11.9f && nrd_spike_step(pg, 26.0f) == 12.1f, "min of 3: a 2-frame burst is kept");
    CHECK(nrd_spike_step(pg, 27.0f) == 25.0f, "the 3rd slow sample in a row steps down (sustained overload)");
    CHECK(pg[NRD_PG_OVER] == 3 && pg[NRD_PG_SUPP] == 2, "counters %u / %u", pg[NRD_PG_OVER], pg[NRD_PG_SUPP]);
    for (int i = 0; i < 20; i++) nrd_spike_step(pg, 17.0f);
    CHECK(nrd_spike_step(pg, 17.0f) == 17.0f, "a constant overload passes (an un-updated sample cannot block a step down)");
    // count only
    nrd_spike_init(pg, NRD_SPIKE_COUNT | NRD_SPIKE_MIN3);
    nrd_spike_step(pg, 11.9f);
    CHECK(nrd_spike_step(pg, 30.0f) == 30.0f && pg[NRD_PG_OVER] == 1 && pg[NRD_PG_SUPP] == 1, "count: raw returned, the would-be save counted");
    const float nan = nanf("");
    CHECK(nrd_spike_step(pg, nan) != nrd_spike_step(pg, nan), "NaN in, NaN out (count mode), no counts");
}
static void test_walk(void) {
    // walking / zone entries: the steady 11.85 ms at full size, a 1-frame 30 ms hitch every 8 s, a 2-frame 24 ms burst every 13 s, then
    // from 40 s to 46 s a really heavy place (17.5 ms at full size). Today every hitch throws the size down; min of 3 keeps it, and the
    // heavy place still steps it down within a few frames.
    for (int mode = 0; mode < 2; mode++) {
        uint32_t pg[NRD_PG_WORDS] = {0}; nrd_spike_init(pg, NRD_SPIKE_APPLY | NRD_SPIKE_COUNT | NRD_SPIKE_MIN3);
        g_spike_pg = mode ? pg : NULL;
        drs_t d = {100.0, 4, 0}; double t = 0, low_before_heavy = 0, heavy_min = 100; int f, burst = 0, drops_before = 0; double last_h = 100;
        for (f = 0; t < 60.0; f++) {
            double g = 11.85;
            if (t >= 40 && t < 46) g = 17.5;
            else if (f % 960 == 500) g = 30.0;                      // a one-frame hitch every ~8 s
            else if (f % 1560 == 700) burst = 2;                    // a two-frame burst every ~13 s
            if (burst && !(t >= 40 && t < 46)) { g = 24.0; burst--; }
            const double gpu = g * (0.35 + 0.65 * area(d.h)), frame = gpu > 8.33 ? gpu : 8.33;
            drs_frame(&d, g, frame, 15.83);
            if (d.h < 99.99 && t < 40) low_before_heavy += frame / 1000.0;
            if (d.h < last_h - 0.01 && t < 40) drops_before++;
            if (t >= 40 && t < 47 && d.h < heavy_min) heavy_min = d.h;
            last_h = d.h; t += frame / 1000.0;
        }
        printf("walk %s: %d drops before the heavy place, %.1f s below 100 %% before it, lowest in it %.1f %%; over target %u, one-off %u\n",
               mode ? "DM_DOS2_DRS_SPIKE=1" : "today", drops_before, low_before_heavy, heavy_min, pg[NRD_PG_OVER], pg[NRD_PG_SUPP]);
        if (!mode) CHECK(drops_before >= 4 && low_before_heavy > 20.0, "today: hitches drop the size (%d drops, %.1f s low)", drops_before, low_before_heavy);
        else {
            CHECK(drops_before == 0 && low_before_heavy == 0, "filter: no drop before the heavy place (%d, %.1f s)", drops_before, low_before_heavy);
            CHECK(heavy_min < 95.0, "filter: the heavy place still steps the size down (%.1f %%)", heavy_min);
            CHECK(pg[NRD_PG_SUPP] > 0 && pg[NRD_PG_SUPP] < pg[NRD_PG_OVER], "counters: %u one-off of %u over", pg[NRD_PG_SUPP], pg[NRD_PG_OVER]);
        }
    }
    g_spike_pg = NULL;
}
// --spike <mode> <target>: the model on stdin (one 8-hex-digit float a line) -> stdout (filtered bits, over, suppressed): the Unicorn
// test of the cave (tests/nr_drift_cave_test.py) runs the same samples through the machine code and compares every line.
static int spike_vectors(uint32_t mode, float target) {
    uint32_t pg[NRD_PG_WORDS] = {0}; nrd_spike_setf(pg, NRD_PG_TARGET, target); nrd_spike_init(pg, mode);
    char line[64];
    while (fgets(line, sizeof line, stdin)) {
        union { uint32_t u; float f; } in, out; in.u = (uint32_t)strtoul(line, NULL, 16);
        out.f = nrd_spike_step(pg, in.f);
        printf("%08x %u %u\n", out.u, pg[NRD_PG_OVER], pg[NRD_PG_SUPP]);
    }
    return 0;
}

static void test_clock(void) {
    CHECK(nrd_clock_mhz("903000000\n", NRD_CLK_NUM) == 903, "kgsl Hz");
    CHECK(nrd_clock_mhz("750000\n", NRD_CLK_NUM) == 750, "kHz");
    CHECK(nrd_clock_mhz("680", NRD_CLK_NUM) == 680, "MHz");
    CHECK(nrd_clock_mhz("12 880000\n", NRD_CLK_LAST) == 880, "GED <opp> <kHz> -> the last number");
    CHECK(nrd_clock_mhz("[GPU] Freq: 600000, Volt: 65000\n", NRD_CLK_TAG_FREQ) == 600, "gpufreq v2 Freq:");
    CHECK(nrd_clock_mhz("g_cur_gpu_freq = 450000\ng_cur_gpu_volt = 1\n", NRD_CLK_TAG_CUR) == 450, "gpufreq v1 cur_gpu_freq");
    CHECK(nrd_clock_mhz("Permission denied", NRD_CLK_NUM) == -1 && nrd_clock_mhz("", NRD_CLK_LAST) == -1 && nrd_clock_mhz("x", NRD_CLK_TAG_FREQ) == -1, "no number");
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--spike")) return spike_vectors((uint32_t)strtoul(argv[2], NULL, 0), (float)atof(argv[3]));
    test_load(); test_settle(); test_probe(); test_replay(); test_spike(); test_walk(); test_clock();
    printf("nr_drift_test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
