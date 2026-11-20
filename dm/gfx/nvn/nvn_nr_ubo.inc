// [DOS2] NR-09U (DM_NR_UBO, default 0; docs/dos2/nr/NR-09U.md): the NVN side of the engine's uniform blocks as native code. Included twice
// by hle_nvn.c:
//   first (after nvn_nr_state.inc)       the capture of DM_NR_UBO=2 and its NUB(...) line at the top of nvnCommandBufferBindUniformBuffer.
//                                        g_nru_cap == 0: a load and a not-taken branch.
//   second (NVN_NRU_ENTRIES, at the end) the executor: the front end hands over one draw's BindUniformBuffer calls (nru_call) and they
//                                        are made through the body of the entry point, on the big stack the direct draw already runs on,
//                                        instead of the engine calling the thunk from its two functions. nvn_nru_ready proves per run
//                                        that the engine's pointer leads to exactly this body, so every effect of a call is the body's own.
#ifdef DM_GAME_DOS2
#ifndef NVN_NRU_ENTRIES
#include "nvn_nr_ubo.h"
static int g_nru_cap;   // DM_NR_UBO=2 (set once at install)
static __thread struct { int on, n; nru_call c[NRU_CALLS_MAX]; } t_nruc;
static void nru_cap(uint64_t cb, int st, int i, uint64_t a, uint64_t n) {
    if (!t_nruc.on) return;
    if (t_nruc.n < NRU_CALLS_MAX) { nru_call *c = &t_nruc.c[t_nruc.n]; c->cb = cb; c->stage = (uint32_t)st; c->slot = (uint32_t)i; c->addr = a; c->size = n; }
    t_nruc.n++;
}
#define NUB(c, st, i, a, n) do { if (__builtin_expect(g_nru_cap, 0)) nru_cap((uint64_t)(uintptr_t)(c), (int)(st), (i), (uint64_t)(a), (uint64_t)(n)); } while (0)
void nvn_nru_cap_begin(void) { t_nruc.n = 0; t_nruc.on = 1; }
int nvn_nru_cap_end(const nru_call **c) { t_nruc.on = 0; *c = t_nruc.c; return t_nruc.n; }
#else
void nvn_nru_enable(int mode) { g_nru_cap = mode == 2; }
// nc_slot / nc_impl: nvn_nr_nocall.inc (second part, included before this one).
int nvn_nru_ready(const void **thunk) {
    const int i = nc_slot("nvnCommandBufferBindUniformBuffer");
    void *const *tab = nvn_thunk_tab();
    *thunk = i >= 0 ? tab[i] : NULL;
    return i >= 0 && nc_impl(i) == (void *)h_nvnCommandBufferBindUniformBuffer;
}
void nvn_nru_exec(const nru_call *c, int n) {
    for (int k = 0; k < n; k++)
        h_nvnCommandBufferBindUniformBuffer((NVNcommandBuffer *)(uintptr_t)c[k].cb, (NVNshaderStage)c[k].stage, (int)c[k].slot, (NVNbufferAddress)c[k].addr, (size_t)c[k].size);
}
#endif
#else
#ifndef NUB
#define NUB(c, st, i, a, n) ((void)0)
#endif
#endif
