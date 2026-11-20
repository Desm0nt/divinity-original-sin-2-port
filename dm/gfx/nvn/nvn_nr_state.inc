// [DOS2] NR-09S (DM_NR_STATE, default 0; docs/dos2/nr/NR-09S.md): the NVN side of the native opcode-21 packet. Included twice by hle_nvn.c:
//   first (after nvn_nr_nocall.inc)      the capture of DM_NR_STATE=2 and its NST(...) lines: one at the top of each of the 19 entry points
//                                        the packet calls. g_nrs_cap == 0: a load and a not-taken branch.
//   second (NVN_NRS_ENTRIES, at the end) the executor: the front end hands over the packet's calls (nrst_op, the compact native state
//                                        record) and they are made through the bodies of the entry points, on the big stack, instead of
//                                        the engine calling them through the thunks. nvn_nrs_ready proves per run that the engine's
//                                        pointers lead to exactly these bodies, so every effect of a call is the body's own.
#ifdef DM_GAME_DOS2
#ifndef NVN_NRS_ENTRIES
#include "nvn_nr_state.h"
static int g_nrs_cap;   // DM_NR_STATE=2 (set once at install)
static __thread struct { int on, n; nrst_op op[NRST_OPS_MAX]; } t_nrsc;
static void nrs_cap(uint32_t fn, uint64_t a, uint64_t p, uint64_t q, int32_t i, int32_t j, float f) {
    if (!t_nrsc.on) return;
    if (t_nrsc.n < NRST_OPS_MAX) { nrst_op *o = &t_nrsc.op[t_nrsc.n]; o->fn = fn; o->a = a; o->p = p; o->q = q; o->i = i; o->j = j; o->f = f; }
    t_nrsc.n++;
}
#define NST(fn, a, p, q, i, j, f) do { if (__builtin_expect(g_nrs_cap, 0)) nrs_cap((fn), (uint64_t)(uintptr_t)(a), (uint64_t)(uintptr_t)(p), (uint64_t)(q), (int32_t)(i), (int32_t)(j), (f)); } while (0)
void nvn_nrs_cap_begin(void) { t_nrsc.n = 0; t_nrsc.on = 1; }
int nvn_nrs_cap_end(const nrst_op **ops) { t_nrsc.on = 0; *ops = t_nrsc.op; return t_nrsc.n; }
#else
static int g_nrs_mode;
void nvn_nrs_enable(int mode) { g_nrs_mode = mode; g_nrs_cap = mode == 2; }
// The bodies in NRST_SLOTS order (fn - 1); nc_slot / nc_impl: nvn_nr_nocall.inc (second part, included before this one).
int nvn_nrs_ready(const void *thunk[NRST_F_N - 1]) {
    static const struct { uint32_t off; const char *name; } slot[NRST_F_N - 1] = NRST_SLOTS;
    void *const want[NRST_F_N - 1] = {
        (void *)h_nvnCommandBufferBindProgram, (void *)h_nvnCommandBufferBindDepthStencilState, (void *)h_nvnCommandBufferSetStencilMask,
        (void *)h_nvnCommandBufferSetStencilValueMask, (void *)h_nvnCommandBufferSetStencilRef, (void *)h_nvnCommandBufferBindPolygonState,
        (void *)h_nvnBlendStateSetDefaults, (void *)h_nvnCommandBufferBindBlendState, (void *)h_nvnColorStateSetDefaults,
        (void *)h_nvnColorStateSetAlphaTest, (void *)h_nvnCommandBufferBindColorState, (void *)h_nvnBlendStateSetBlendTarget,
        (void *)h_nvnColorStateSetBlendEnable, (void *)h_nvnCommandBufferBindVertexAttribState, (void *)h_nvnCommandBufferBindVertexStreamState,
        (void *)h_nvnCommandBufferBindVertexBuffer, (void *)h_nvnCommandBufferSetRenderEnable, (void *)h_nvnCommandBufferSetRenderEnableConditional,
        (void *)h_nvnCommandBufferSetAlphaRef};
    void *const *tab = nvn_thunk_tab();
    int ok = g_nrs_mode != 0;
    for (int k = 0; k < NRST_F_N - 1; k++) {
        const int i = nc_slot(slot[k].name);
        thunk[k] = i >= 0 ? tab[i] : NULL;
        if (i < 0 || nc_impl(i) != want[k]) ok = 0;
    }
    return ok;
}
void nvn_nrs_exec(const nrst_op *op, int n) {
    NVNblendState lb; NVNcolorState lc;   // the engine's sp + 0x18 / sp + 8 of the default-blend path (SetDefaults writes all their bytes)
    memset(&lb, 0, sizeof lb); memset(&lc, 0, sizeof lc);
    for (int k = 0; k < n; k++) {
        const nrst_op *o = &op[k];
        NVNcommandBuffer *c = (NVNcommandBuffer *)(uintptr_t)o->a;
        void *p = o->p == NRST_LOCAL_BLEND ? (void *)&lb : o->p == NRST_LOCAL_COLOR ? (void *)&lc : (void *)(uintptr_t)o->p;
        switch (o->fn) {
        case NRST_F_PROGRAM: h_nvnCommandBufferBindProgram(c, (const NVNprogram *)p, o->i); break;
        case NRST_F_DS: h_nvnCommandBufferBindDepthStencilState(c, (const NVNdepthStencilState *)p); break;
        case NRST_F_STENCIL_MASK: h_nvnCommandBufferSetStencilMask(c, (NVNface)o->i, o->j); break;
        case NRST_F_STENCIL_VMASK: h_nvnCommandBufferSetStencilValueMask(c, (NVNface)o->i, o->j); break;
        case NRST_F_STENCIL_REF: h_nvnCommandBufferSetStencilRef(c, (NVNface)o->i, o->j); break;
        case NRST_F_POLY: h_nvnCommandBufferBindPolygonState(c, (const NVNpolygonState *)p); break;
        case NRST_F_BLEND_DEF: h_nvnBlendStateSetDefaults((NVNblendState *)p); break;
        case NRST_F_BLEND: h_nvnCommandBufferBindBlendState(c, (const NVNblendState *)p); break;
        case NRST_F_COLOR_DEF: h_nvnColorStateSetDefaults((NVNcolorState *)p); break;
        case NRST_F_COLOR_ATEST: h_nvnColorStateSetAlphaTest((NVNcolorState *)p, (NVNalphaFunc)o->i); break;
        case NRST_F_COLOR: h_nvnCommandBufferBindColorState(c, (const NVNcolorState *)p); break;
        case NRST_F_BLEND_TARGET: h_nvnBlendStateSetBlendTarget((NVNblendState *)p, o->i); break;
        case NRST_F_COLOR_BLEND: h_nvnColorStateSetBlendEnable((NVNcolorState *)p, o->i, (uint32_t)o->j); break;
        case NRST_F_VATTR: h_nvnCommandBufferBindVertexAttribState(c, o->i, (const NVNvertexAttribState *)p); break;
        case NRST_F_VSTREAM: h_nvnCommandBufferBindVertexStreamState(c, o->i, (const NVNvertexStreamState *)p); break;
        case NRST_F_VB: h_nvnCommandBufferBindVertexBuffer(c, o->i, (NVNbufferAddress)o->p, (size_t)o->q); break;
        case NRST_F_REN: h_nvnCommandBufferSetRenderEnable(c, (uint32_t)o->i); break;
        case NRST_F_REN_COND: h_nvnCommandBufferSetRenderEnableConditional(c, (NVNconditionalRenderMode)o->i, (NVNbufferAddress)o->p); break;
        case NRST_F_ALPHA_REF: h_nvnCommandBufferSetAlphaRef(c, o->f); break;
        default: break;
        }
    }
}
#endif
#else
#ifndef NST
#define NST(fn, a, p, q, i, j, f) ((void)0)
#endif
#endif
