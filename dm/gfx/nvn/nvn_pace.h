// 出图节拍器（纯函数，Mac / iOS 共用；单测 tests/t_pace.c）。
// 语义同垂直同步：节拍点 = 上一节拍点 + 周期。早到 → 等到节拍点；迟到不足一个周期 → 立即出图、保持原相位；
// 落后一个周期以上（卡顿）→ 从当前时刻重新起拍，不连发追帧。周期 0 = 不等（节拍点跟随当前时刻）。
// Present pacer (pure function, shared by Mac / iOS; unit test tests/t_pace.c).
// Semantics as vsync: present slot = previous present slot + period. Early → wait until the present slot; late by less than one period → present immediately and keep the original phase;
// behind by one period or more (stutter) → restart the cadence from the current time, no burst of presents to catch up. Period 0 = no wait (the present slot follows the current time).
#pragma once
#include <stdint.h>

typedef struct { uint64_t target; } nvn_pacer;

// now：单调时钟（ns）；period：本帧周期（ns，0 = 不等）。返回需要等待的 ns（0 = 立即出图），并更新内部目标时刻。
// now: monotonic clock (ns); period: this frame's period (ns, 0 = no wait). Returns the ns to wait (0 = present immediately) and updates the internal target time.
static inline uint64_t nvn_pace(nvn_pacer *p, uint64_t now, uint64_t period) {
    if (!period || !p->target) { p->target = now; return 0; }
    uint64_t next = p->target + period;
    if (now <= next) { p->target = next; return next - now; }
    p->target = now - next < period ? next : now;
    return 0;
}

// 出图帧率 = 屏幕刷新率的整数分之一里不低于目标（60，省电模式 30）的最小一档：每帧在屏上停整数个刷新周期，没有交替抖动。
// 60 / 120 Hz 屏 → 60；144 → 72；165 → 82.5；100 → 100；240 → 60；省电 30：120 → 30、144 → 36。刷新率读不到（0）就按目标。
// 0.02 的余量让 119.88 / 59.94 这类 NTSC 档按 2 / 1 倍算
// Present frame rate = the smallest tier among integer fractions of the screen refresh rate that is not below the target (60, power saving mode 30): each frame stays on screen for an integer number of refresh cycles, with no alternating jitter.
// 60 / 120 Hz screen → 60; 144 → 72; 165 → 82.5; 100 → 100; 240 → 60; power saving 30: 120 → 30, 144 → 36. When the refresh rate cannot be read (0), the target is used.
// The 0.02 margin lets NTSC rates like 119.88 / 59.94 count as 2 / 1 times
static inline double nvn_pace_hz(double screen_hz, double floor_hz) {
    if (screen_hz <= 0 || floor_hz <= 0) return floor_hz;
    double k = (double)(long)(screen_hz / floor_hz + 0.02); return k >= 1 ? screen_hz / k : screen_hz;
}

// [DOOM / D3] Frame rate limit (DM_PRESENT_MAX_HZ, the gear's limit; ported from the DOOM 2016 port): the largest integer fraction
// of the screen refresh that is not above the cap, so the present never runs faster than the cap.
// 120 Hz screen: cap 60 -> 60, 30 -> 30; 90 Hz: cap 60 -> 45; 144: 60 -> 48; 60 Hz: any cap >= 60 -> 60. Same 0.02 margin for NTSC
// rates (119.88 / cap 60 -> 59.94). Refresh unknown (0): the cap.
static inline double nvn_pace_cap_hz(double screen_hz, double cap_hz) {
    if (screen_hz <= 0 || cap_hz <= 0) return cap_hz;
    double r = screen_hz / cap_hz - 0.02; long k = (long)r; if (k < r) k++; if (k < 1) k = 1;
    return screen_hz / (double)k;
}

// [DOS2] Frame rate target (DM_DOS2_FPS=30|60|90|120|0, the DOS2 gear's frame rate row; docs/dos2/DOS2_PORT.md "Лимит кадров 90 / 120 /
// без ограничения"). Parse: one of 0 / 30 / 60 / 90 / 120, else -1 (unset / not offered: the old rule, DM_PRESENT_MAX_HZ).
static inline int nvn_fps_parse(const char *s) {
    if (!s) return -1;
    while (*s == ' ' || *s == '	') s++;   // dm/env keeps the spaces of a line
    int v = 0, n = 0;
    for (; *s >= '0' && *s <= '9'; s++, n++) { if (v > 1000) return -1; v = v * 10 + (*s - '0'); }
    while (*s == ' ' || *s == '	') s++;
    if (!n || *s) return -1;
    return v == 0 || v == 30 || v == 60 || v == 90 || v == 120 ? v : -1;
}
// The present rate for a target: 0 = uncapped (no host pacing: the FIFO swapchain then holds the game to the screen refresh);
// 30 / 60 = nvn_pace_cap_hz, the rule the gear's limit used before (120 Hz: 30 / 60; 90 Hz: 30 / 45), so 60 stays exactly as it was;
// 90 / 120 = an integer fraction of the refresh within 2 % of the target when there is one (even: every frame stays the same number
// of refreshes; 120 on 120 / 240 Hz, 90 on 90 / 180 Hz, 119.88 -> 119.88), else at least the refresh (120 on a 60 / 90 Hz screen = the
// refresh), else the target itself, uneven (90 on a 120 Hz screen: frames alternate one and two refreshes, 90 on average; the Java side
// asks for a 90 Hz display mode first, so this happens only where the phone has none or ignores the request). Refresh unknown (0): the target.
static inline double nvn_pace_target_hz(double screen_hz, int target) {
    if (target <= 0) return 0;
    if (target <= 60) return nvn_pace_cap_hz(screen_hz, target);
    if (screen_hz <= 0) return target;
    if (screen_hz <= target * 1.02) return screen_hz;
    long k = (long)(screen_hz / target + 0.5); if (k < 1) k = 1;
    double even = screen_hz / (double)k, d = even > target ? even - target : target - even;
    return d <= target * 0.02 ? even : (double)target;
}
// The window's frame-rate vote (vulkan_present.cpp, marker gfx-frame-rate, default 120): 90 votes 90 (a 120 vote would keep a phone
// that has a 90 Hz mode at 120, and 90 fps on it is uneven); every other target keeps the marker's vote (120 for 120 / uncapped / 60 / 30).
static inline float nvn_fps_vote_hz(int target, float marker) { return target == 90 && marker > 0 ? 90.0f : marker; }
