// [DOS2] NR-14 DM_GFX_SAMPLES_TS policy (pure C, no Vulkan; tests/samplets_test.c; docs/dos2/nr/NR-14.md). The engine side is
// xbg_game.cpp namespace samplets (OP_REPORT_COUNTER, samples passed).
//  0 drop: a samples-passed report records nothing (no timestamp, no PendingReport, no write into guest memory). DOS2's only reader of
//    these reports is SetRenderEnableConditional on the GPU (NR-03.md §1: the counter buffer's CPU pointer dev+0x25ac8 is only ever
//    stored to, in BeginFrame's {0, -1} pre-fill; its GPU address dev+0x25b18 only builds the pair table), and the host's conditional
//    rendering is off (DM_GFX_COND_RENDER=0, no NR-03): every stamp was pure GPU cost (a WFI on Turnip) and, off Adreno with a
//    failed report_inpass self-test, a render-pass split. Guard: an in-stream access to the dropped reports' address range (a copy
//    source, a CPU read / write of the engine) switches the run to 4 (red line); GPU snapshots only count.
//  1 one bottom-of-pipe timestamp per report (the old behaviour, every other port).
//  2 top of pipe per report (no WFI, clamped monotonic at retire).
//  3 check: as 1, plus the guard's counters and red lines ("this run with 0 would have been unsafe").
//  4 one shared bottom-of-pipe stamp per pass (VERTEX.md §5.1).
// Unset: DOS2 = 0 when nothing consumes the reports, 4 when something does (DM_GFX_COND_RENDER != 0 or DM_NR_OCC != 0); other games 1.
#pragma once
#include <stdint.h>

enum { SAMPLETS_DROP = 0, SAMPLETS_BOP = 1, SAMPLETS_TOP = 2, SAMPLETS_CHECK = 3, SAMPLETS_SHARED = 4 };
enum { SAMPLETS_WHY_ENV = 0, SAMPLETS_WHY_NO_CONSUMER, SAMPLETS_WHY_CONDR, SAMPLETS_WHY_NR_OCC, SAMPLETS_WHY_OTHER_GAME, SAMPLETS_WHY_BAD_ENV };

static inline int samplets_env_set(const char *e) { return e && *e; }
static inline long samplets_atol(const char *e) { long v = 0; int neg = 0; if (!e) return 0; while (*e == ' ') e++; if (*e == '-') { neg = 1; e++; }
    while (*e >= '0' && *e <= '9') { v = v * 10 + (*e - '0'); e++; } return neg ? -v : v; }
// The mode for DM_GFX_SAMPLES_TS = ts (NULL / "" = unset), DM_GFX_COND_RENDER = condr, DM_NR_OCC = nr_occ; *why gets SAMPLETS_WHY_*.
// A value given in env always wins (0 too, even with a consumer: an A/B switch); a value that is not exactly one digit 0..4 is 1.
static inline int samplets_mode(int dos2, const char *ts, const char *condr, const char *nr_occ, int *why) {
    int w;
    int m;
    if (samplets_env_set(ts)) {
        if (ts[0] >= '0' && ts[0] <= '4' && !ts[1]) { m = ts[0] - '0'; w = SAMPLETS_WHY_ENV; }
        else { m = SAMPLETS_BOP; w = SAMPLETS_WHY_BAD_ENV; }
    } else if (!dos2) { m = SAMPLETS_BOP; w = SAMPLETS_WHY_OTHER_GAME; }
    else {
        const long c = samplets_env_set(condr) ? samplets_atol(condr) : 0, o = samplets_env_set(nr_occ) ? samplets_atol(nr_occ) : 0;
        if (c != 0) { m = SAMPLETS_SHARED; w = SAMPLETS_WHY_CONDR; }
        else if (o != 0) { m = SAMPLETS_SHARED; w = SAMPLETS_WHY_NR_OCC; }
        else { m = SAMPLETS_DROP; w = SAMPLETS_WHY_NO_CONSUMER; }
    }
    if (why) *why = w;
    return m;
}
static inline const char *samplets_why_name(int w) {
    return w == SAMPLETS_WHY_ENV ? "env" : w == SAMPLETS_WHY_NO_CONSUMER ? "auto: no consumer (DM_GFX_COND_RENDER=0, no DM_NR_OCC)"
         : w == SAMPLETS_WHY_CONDR ? "auto: DM_GFX_COND_RENDER reads the reports" : w == SAMPLETS_WHY_NR_OCC ? "auto: DM_NR_OCC is set"
         : w == SAMPLETS_WHY_OTHER_GAME ? "auto: not DOS2" : "env value not 0..4";
}
// The recording mode of one report: drop only while the host counts no samples (a counter reset turns counting on: then the report
// has a real occlusion run to close and is shared-stamped as 4); the check mode stamps as 1.
static inline int samplets_effective(int mode, int counting) {
    if (mode == SAMPLETS_DROP) return counting ? SAMPLETS_SHARED : SAMPLETS_DROP;
    if (mode == SAMPLETS_CHECK) return SAMPLETS_BOP;
    return mode;
}
// Address range of every samples report seen (bounding box; DOS2: one 128 KB buffer). Empty while hi == 0.
typedef struct { uint64_t lo, hi; } samplets_range;
static inline void samplets_range_add(samplets_range *r, uint64_t addr) {
    if (!r->hi) { r->lo = addr; r->hi = addr + 16; return; }
    if (addr < r->lo) r->lo = addr;
    if (addr + 16 > r->hi) r->hi = addr + 16;
}
static inline int samplets_range_hit(const samplets_range *r, uint64_t a, uint64_t n) { return r->hi && n && a < r->hi && a + n > r->lo; }
// cpu_access kinds (xbg_game.cpp): 1 / 2 are GPU snapshots of a range (they already see the last resolved report, cpu_access skips
// pending reports for them) -> counted only; any other kind, and 12 = an in-stream GPU buffer copy reading the range, is a reader.
static inline int samplets_touch_is_reader(unsigned why) { return why != 1 && why != 2; }
// What a touch does: 0 nothing, 1 count, 2 count + red line (check), 3 count + red line + switch the run from 0 to 4.
static inline int samplets_touch_action(int mode, unsigned why) {
    if (!samplets_touch_is_reader(why)) return 1;
    if (mode == SAMPLETS_DROP) return 3;
    if (mode == SAMPLETS_CHECK) return 2;
    return 1;
}
