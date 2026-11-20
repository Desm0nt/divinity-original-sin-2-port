// [DOS2] NR-TRAFFIC round 2 DM_NR_ARENA (docs/dos2/nr/NR-TRAFFIC.md §10): how many standard chunks a frame slot's stream arena keeps
// between submissions. Pure C; the engine (xbg_game.cpp namespace narena) calls it at each slot retire, before Arena::trim;
// dm/gfx/vulkan/engine/tests/nr_arena_test.c tests it on the PC.
// Rule 1: keep the most chunks a submission of this slot touched in the last `hold` submissions (at least 1); a submission that needs more
// grows the target at once; the target shrinks only when a window of `hold` submissions ends (to that window's max); a memory-trim request
// (a new trim generation) shrinks it to the last submission's need once.
#ifndef DM_NR_ARENA_H
#define DM_NR_ARENA_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint32_t target, win_max, win_n, last_lim, trim_seen; } nrar_state;
// used: chunks the last submission touched (0 = none); hold: window length; trim_gen: the trim request generation.
// Returns the chunk limit (>= 1); *regrown = 1 when the last submission needed more chunks than the previous limit kept.
static inline uint32_t nrar_keep(nrar_state *s, uint32_t used, uint32_t hold, uint32_t trim_gen, int *regrown) {
    if (!hold) hold = 1;
    *regrown = s->last_lim && used > s->last_lim;
    if (used > s->win_max) s->win_max = used;
    if (used > s->target) s->target = used;
    if (++s->win_n >= hold) { s->target = s->win_max ? s->win_max : 1; s->win_max = 0; s->win_n = 0; }
    if (trim_gen != s->trim_seen) { s->trim_seen = trim_gen; s->target = used ? used : 1; s->win_max = 0; s->win_n = 0; }
    const uint32_t lim = s->target ? s->target : 1;
    s->last_lim = lim;
    return lim;
}
#ifdef __cplusplus
}
#endif
#endif
