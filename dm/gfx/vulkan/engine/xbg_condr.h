// [DOS2] DM_GFX_COND_RENDER: the decision logic of NVN conditional rendering (SetRenderEnable / SetRenderEnableConditional), pure C for C and
// C++ (no NVN / Vulkan types). Used by xbg_game.cpp namespace condr (port of DOOM Eternal PERF-E05, ETR src-cmp16 e24b901 / c812f46) and by the
// host test dm/gfx/vulkan/engine/tests/condr_test.c.
//
// Modes (DM_GFX_COND_RENDER, read once):
//   0 = off (default): nothing is recorded (hle_nvn.c writes no OP_X_RENDER_COND), every draw is made, no counter line.
//   1 = VK_EXT_conditional_rendering around the conditional draws, the predicate (judged as mode 2) in a small host-visible buffer; without the
//       extension the conditions are only counted. SetRenderEnable(false) is skipped by the host.
//   2 = the host skips the draws whose predicate fails; a report still pending on the GPU means "render" (stale, counted).
//   3 = as 2, but a pending report is judged by what guest memory holds for that address: the previous frame's counters (the game reuses its
//       report slots). An object that just became visible is drawn one frame late.
//   4 = count only: every draw is made (picture as 0), the predicate is judged both ways and counted (would_skip2 / would_skip3).
// Predicate (Maxwell render-enable modes, as yuzu reads them): the two 16-byte counter reports at addr and addr + 16 (NVNcounterData
// {counter, timestamp}) are compared by their counter words; RENDER_IF_EQUAL (0) renders when they are equal, RENDER_IF_NOT_EQUAL (1)
// when they differ (a report before and one after the occlusion test: equal = no sample passed).
#ifndef XBG_CONDR_H
#define XBG_CONDR_H
#include <stdint.h>

enum { CONDR_OFF = 0, CONDR_GPU = 1, CONDR_CPU = 2, CONDR_PREV = 3, CONDR_COUNT = 4, CONDR_MODES = 5 };
enum { CONDR_K_NONE = -1, CONDR_K_ON = 0, CONDR_K_OFF = 1, CONDR_K_COND = 2 };   // CONDR_K_ON..COND = p_render_cond.kind
enum { CONDR_NVN_IF_EQUAL = 0, CONDR_NVN_IF_NOT_EQUAL = 1 };                     // NVNconditionalRenderMode

static inline int condr_clamp_mode(long v) { return v < 0 || v >= CONDR_MODES ? CONDR_OFF : (int)v; }
// The predicate on the two counter words. Any NVN mode other than the two compare modes renders (never a false skip).
static inline int condr_predicate(int nvn_mode, uint64_t first, uint64_t second) {
    if (nvn_mode == CONDR_NVN_IF_EQUAL) return first == second;
    if (nvn_mode == CONDR_NVN_IF_NOT_EQUAL) return first != second;
    return 1;
}
// One condition judged for a mode. stale: a report of the pair (or a writeback over it) is still pending on the GPU, so guest memory holds an
// older value. ok_addr: the pair is usable (non-zero, 8-aligned address, both halves written by a counter report of this run - else guest
// memory holds whatever the game left there, e.g. zeros on the first frame, which would read as "no sample passed"). Returns 1 = render. *judged_stale = 1 when a stale value decided.
static inline int condr_judge(int mode, int nvn_mode, int ok_addr, int stale, uint64_t first, uint64_t second, int *judged_stale) {
    *judged_stale = 0;
    if (!ok_addr) return 1;
    if (stale && mode != CONDR_PREV) return 1;
    *judged_stale = stale;
    return condr_predicate(nvn_mode, first, second);
}
// Does the host skip a draw made under the condition kind (CONDR_K_*) whose judgement was render (1 / 0)?
static inline int condr_host_skips(int mode, int kind, int render) {
    if (mode == CONDR_OFF || mode == CONDR_COUNT || kind == CONDR_K_NONE || kind == CONDR_K_ON) return 0;
    if (kind == CONDR_K_OFF) return 1;                     // SetRenderEnable(false): nothing is drawn (modes 1..3)
    return (mode == CONDR_CPU || mode == CONDR_PREV) && !render;   // mode 1 leaves conditional draws to the GPU
}
// Is a draw under kind inside a "conditional range" for the counters (SetRenderEnable(true) is not)?
static inline int condr_inside(int kind) { return kind == CONDR_K_OFF || kind == CONDR_K_COND; }
#endif
