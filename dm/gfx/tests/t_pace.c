// 节拍器单测：给定时钟序列，期望等待时长手算。周期 16.666 ms 记为 P = 16666667 ns。
// Pacer unit tests: given a clock sequence, the expected wait durations are hand-calculated. The period 16.666 ms is written as P = 16666667 ns.
#include "../nvn/nvn_pace.h"
#include <stdio.h>

int main(void) {
    const uint64_t P = 16666667, MS = 1000000;
    struct { uint64_t now, period, want; const char *why; } c[] = {
        {1000 * MS, P, 0, "首帧：起拍，不等"},
        {1005 * MS, P, 1000 * MS + P - 1005 * MS, "早到 5 ms：等到 1000+P"},
        {1000 * MS + 2 * P - 2 * MS, P, 2 * MS, "第三帧早 2 ms：等 2 ms"},
        {1000 * MS + 3 * P + 3 * MS, P, 0, "迟到 3 ms（不足一周期）：立即出图，节拍点仍为 1000+3P"},
        {1000 * MS + 4 * P - 1 * MS, P, 1 * MS, "下一帧早 1 ms：按原相位等到 1000+4P"},
        {1000 * MS + 7 * P, P, 0, "落后两个周期（卡顿）：重新起拍于当时，不连发追帧"},
        {1000 * MS + 7 * P + 4 * MS, P, P - 4 * MS, "重新起拍后的下一帧：等到新目标"},
        {1000 * MS + 9 * P, 0, 0, "周期 0（DM_PRESENT_HZ=0）：不等"},
        {1000 * MS + 9 * P + 1 * MS, 2 * P, 2 * P - 1 * MS, "切回间隔 2：节拍点 = 上帧时刻 9P + 2P"},
        {1000 * MS + 11 * P + 3 * MS, 2 * P, 2 * P - 3 * MS, "间隔 2 下一帧：节拍点 13P"},
        {1000 * MS + 13 * P + 2 * MS, P, P - 2 * MS, "30 to 60 fps: resume one-period cadence"},
        {1000 * MS + 14 * P + 1 * MS, 2 * P, 2 * P - 1 * MS, "60 to 30 fps: wait two periods"},
    };
    nvn_pacer p = {0}; int fail = 0, n = sizeof c / sizeof *c;
    for (int i = 0; i < n; i++) {
        uint64_t got = nvn_pace(&p, c[i].now, c[i].period);
        int ok = got == c[i].want;
        printf("%-52s 期望等 %8.3f ms → 真结果 %8.3f ms %s\n", c[i].why, c[i].want / 1e6, got / 1e6, ok ? "✓" : "✗");
        fail += !ok;
    }
    // 出图帧率：屏幕刷新率 ÷ k，取不低于目标的最小一档（期望值按 刷新率 ÷ floor(刷新率 ÷ 目标) 手算）
    // Present frame rate: screen refresh rate ÷ k, taking the smallest tier not below the target (expected values computed by hand as refresh rate ÷ floor(refresh rate ÷ target))
    struct { double scr, floor, want; const char *why; } h[] = {
        {60, 60, 60, "60 Hz 屏：60（与原先相同）"}, {120, 60, 60, "120 Hz 屏：60（与原先相同）"}, {144, 60, 72, "144 Hz 屏：72（144 ÷ 2）"},
        {165, 60, 82.5, "165 Hz 屏：82.5（165 ÷ 2）"}, {100, 60, 100, "100 Hz 屏：100（÷ 1）"}, {240, 60, 60, "240 Hz 屏：60（÷ 4）"},
        {119.88, 60, 59.94, "119.88 Hz：59.94（÷ 2，NTSC 档）"}, {59.94, 60, 59.94, "59.94 Hz：59.94（÷ 1）"}, {50, 60, 50, "50 Hz 屏：50（低于目标也只能 ÷ 1）"},
        {0, 60, 60, "读不到刷新率：按目标 60"}, {120, 30, 30, "省电 30，120 Hz 屏：30"}, {144, 30, 36, "省电 30，144 Hz 屏：36（÷ 4）"}, {60, 30, 30, "省电 30，60 Hz 屏：30"},
    };
    for (int i = 0; i < (int)(sizeof h / sizeof *h); i++) {
        double got = nvn_pace_hz(h[i].scr, h[i].floor); int ok = got > h[i].want - 1e-9 && got < h[i].want + 1e-9;
        printf("%-52s 期望 %7.3f Hz → 真结果 %7.3f Hz %s\n", h[i].why, h[i].want, got, ok ? "✓" : "✗"); fail += !ok; n++;
    }
    // [D3 / DOS2] the gear's limit (DM_PRESENT_MAX_HZ): the largest integer fraction of the refresh not above the cap
    struct { double scr, cap, want; const char *why; } cp[] = {
        {120, 60, 60, "cap 60 on 120 Hz: 60"}, {120, 30, 30, "cap 30 on 120 Hz: 30"}, {90, 60, 45, "cap 60 on 90 Hz: 45"},
        {144, 60, 48, "cap 60 on 144 Hz: 48"}, {60, 60, 60, "cap 60 on 60 Hz: 60"}, {60, 120, 60, "cap 120 on 60 Hz: 60"},
        {119.88, 60, 59.94, "cap 60 on 119.88 Hz: 59.94"}, {0, 60, 60, "refresh unknown: the cap"},
    };
    for (int i = 0; i < (int)(sizeof cp / sizeof *cp); i++) {
        double got = nvn_pace_cap_hz(cp[i].scr, cp[i].cap); int ok = got > cp[i].want - 1e-9 && got < cp[i].want + 1e-9;
        printf("%-52s want %7.3f Hz -> got %7.3f Hz %s\n", cp[i].why, cp[i].want, got, ok ? "OK" : "FAIL"); fail += !ok; n++;
    }
    // [DOS2] DM_DOS2_FPS targets (nvn_pace_target_hz): 30 / 60 = the cap rule above (60 unchanged), 90 / 120 = an even tier within 2 %, else
    // at most the refresh, else the target itself (uneven); 0 = uncapped (no pacing)
    struct { double scr; int t; double want; const char *why; } tg[] = {
        {120, 60, 60, "60 on 120 Hz: 60 (= cap 60, unchanged)"}, {90, 60, 45, "60 on 90 Hz: 45 (= cap 60, unchanged)"},
        {120, 30, 30, "30 on 120 Hz: 30"}, {120, 120, 120, "120 on 120 Hz: 120 (even, 1 refresh)"},
        {119.88, 120, 119.88, "120 on 119.88 Hz: 119.88"}, {240, 120, 120, "120 on 240 Hz: 120 (even, 2 refreshes)"},
        {60, 120, 60, "120 on 60 Hz: 60 (the refresh)"}, {90, 120, 90, "120 on 90 Hz: 90 (the refresh)"},
        {144, 120, 120, "120 on 144 Hz: 120 (uneven)"}, {90, 90, 90, "90 on 90 Hz: 90 (even)"}, {180, 90, 90, "90 on 180 Hz: 90 (even, 2)"},
        {120, 90, 90, "90 on 120 Hz: 90 (uneven: no 90 Hz mode)"}, {89.9, 90, 89.9, "90 on 89.9 Hz: 89.9"}, {60, 90, 60, "90 on 60 Hz: 60"},
        {0, 90, 90, "90, refresh unknown: 90"}, {0, 60, 60, "60, refresh unknown: 60"},
        {120, 0, 0, "uncapped on 120 Hz: 0 (no pacing)"}, {0, 0, 0, "uncapped, refresh unknown: 0"},
    };
    for (int i = 0; i < (int)(sizeof tg / sizeof *tg); i++) {
        double got = nvn_pace_target_hz(tg[i].scr, tg[i].t); int ok = got > tg[i].want - 1e-9 && got < tg[i].want + 1e-9;
        printf("%-52s want %7.3f Hz -> got %7.3f Hz %s\n", tg[i].why, tg[i].want, got, ok ? "OK" : "FAIL"); fail += !ok; n++;
    }
    // [DOS2] DM_DOS2_FPS parsing: only the offered values, else -1 (unset: the old rule)
    struct { const char *s; int want; } ps[] = {
        {"0", 0}, {"30", 30}, {"60", 60}, {"90", 90}, {"120", 120}, {NULL, -1}, {"", -1}, {"45", -1}, {"144", -1}, {"-1", -1}, {"60 ", 60}, {" 120	", 120}, {" ", -1}, {"6 0", -1},
        {"6O", -1}, {"99999999999", -1}, {"00", 0}, {"060", 60},
    };
    for (int i = 0; i < (int)(sizeof ps / sizeof *ps); i++) {
        int got = nvn_fps_parse(ps[i].s), ok = got == ps[i].want;
        printf("parse DM_DOS2_FPS=%-14s want %4d -> got %4d %s\n", ps[i].s ? ps[i].s : "(unset)", ps[i].want, got, ok ? "OK" : "FAIL"); fail += !ok; n++;
    }
    // [DOS2] window vote: 90 votes 90, everything else keeps the marker / default (0 = no vote stays 0)
    struct { int t; float marker, want; } vt[] = { {90, 120, 90}, {120, 120, 120}, {0, 120, 120}, {60, 120, 120}, {30, 120, 120}, {-1, 120, 120}, {90, 0, 0} };
    for (int i = 0; i < (int)(sizeof vt / sizeof *vt); i++) {
        float got = nvn_fps_vote_hz(vt[i].t, vt[i].marker); int ok = got == vt[i].want;
        printf("vote target %4d marker %5.0f: want %5.0f -> got %5.0f %s\n", vt[i].t, vt[i].marker, vt[i].want, got, ok ? "OK" : "FAIL"); fail += !ok; n++;
    }
    // [DOS2] the pacer at 120 / 90 / uncapped periods: 8.333 ms cadence keeps phase; an uneven 90 on a timer keeps 11.111 ms; period 0 never waits
    {
        nvn_pacer q = {0}; const uint64_t P120 = 8333333, P90 = 11111111; uint64_t t0 = 5000 * MS;
        struct { uint64_t now, period, want; const char *why; } pc[] = {
            {t0, P120, 0, "120: first frame starts the cadence"}, {t0 + 2 * MS, P120, P120 - 2 * MS, "120: early 2 ms -> wait to t0+P"},
            {t0 + 2 * P120 + 1 * MS, P120, 0, "120: late 1 ms -> present now, phase kept"}, {t0 + 3 * P120 - 3 * MS, P120, 3 * MS, "120: next early 3 ms"},
            {t0 + 3 * P120 + 1 * MS, P90, P90 - 1 * MS, "switch to 90: slot = last slot + P90"}, {t0 + 3 * P120 + P90 + 2 * MS, P90, P90 - 2 * MS, "90: next slot"},
            {t0 + 9 * P120, 0, 0, "uncapped: no wait"}, {t0 + 9 * P120 + 1 * MS, 0, 0, "uncapped: still no wait"},
        };
        for (int i = 0; i < (int)(sizeof pc / sizeof *pc); i++) {
            uint64_t got = nvn_pace(&q, pc[i].now, pc[i].period); int ok = got == pc[i].want;
            printf("%-52s want %8.3f ms -> got %8.3f ms %s\n", pc[i].why, pc[i].want / 1e6, got / 1e6, ok ? "OK" : "FAIL"); fail += !ok; n++;
        }
    }
    printf("== %d 项，失败 %d ==\n", n, fail);
    return fail != 0;
}
