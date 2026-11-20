// [DOS2] NR-23 (DM_NR_NOCALL, default 0; docs/dos2/nr/NR-23.md): the NVN side. Included twice by hle_nvn.c:
//   first (after nvn_nr_tap.inc)        the capture of DM_NR_NOCALL=2 and its NCT(...) lines: one in BindTexture, DrawElements,
//                                       DrawElementsInstanced and SetRenderEnable. g_nc_cap == 0: a load and a not-taken branch.
//   second (NVN_NC_ENTRIES, at the end) the direct entries: the front end calls the bodies of the five entry points the draw code of
//                                       the engine calls, on the big stack, instead of the engine calling them through the thunks.
//                                       They are the same functions (nvn_nc_ready proves it per run), so every effect of the call -
//                                       the NB tap, the NB-09 shadow, the texture-use marks, the record - is the body's own.
// The call counter t_nc_calls and g_nc_count live next to nvn_bigstack_top (the one place every implemented NVN call passes).
#ifdef DM_GAME_DOS2
#ifndef NVN_NC_ENTRIES
#include "nvn_nr_nocall.h"
static int g_nc_cap;                       // DM_NR_NOCALL=2 (set once at install)
static __thread struct { int on; nvn_nc_cap c; } t_ncc;
#define NCT(x) do { if (__builtin_expect(g_nc_cap, 0)) { x; } } while (0)
static void nc_cap_bind(const void *c, int st, int slot, uint64_t h) {
    if (!(t_ncc.on & 1)) return;
    nvn_nc_cap *k = &t_ncc.c;
    if (k->nbind < NVN_NC_CAP_BINDS) { k->b[k->nbind].cb = (uint64_t)(uintptr_t)c; k->b[k->nbind].handle = h; k->b[k->nbind].stage = st; k->b[k->nbind].slot = slot; }
    k->nbind++;
}
static void nc_cap_draw(const void *c, int prim, int itype, int count, uint64_t ib, int inst) {
    if (!(t_ncc.on & 2)) return;
    nvn_nc_cap *k = &t_ncc.c;
    k->ndraw++; k->dcb = (uint64_t)(uintptr_t)c; k->prim = prim; k->itype = itype; k->count = count; k->ib = ib; k->instances = inst;
}
static void nc_cap_ren(const void *c, uint32_t en) {
    if (!(t_ncc.on & 2)) return;
    nvn_nc_cap *k = &t_ncc.c;
    k->nren++; k->rcb = (uint64_t)(uintptr_t)c; k->ren_on = (int32_t)en;
}
void nvn_nc_cap_begin(int what) {
    nvn_nc_cap *k = &t_ncc.c;
    if (what & 1) k->nbind = 0;
    if (what & 2) { k->ndraw = 0; k->nren = 0; }
    t_ncc.on |= what;
}
const nvn_nc_cap *nvn_nc_cap_end(int what) { t_ncc.on &= ~what; return &t_ncc.c; }
uint64_t nvn_nc_thread_calls(void) { return t_nc_calls; }
#else
static int g_nc_mode;
void nvn_nc_enable(int mode) { g_nc_mode = mode; g_nc_cap = mode == 2; g_nc_count = mode == 2; }
static int nc_slot(const char *name) { for (unsigned i = 0; i < NVN_NFUNC; i++) if (!strcmp(nvn_names[i], name)) return (int)i; return -1; }
// The function a slot ends in, the wrappers this file can put in front of it peeled (big stack; under it timing or trace).
static void *nc_impl(int i) {
    void *f = nvn_slot[i].fn;
    if (NVN_BIG_FN(f)) f = nvn_real_big[i];   // the big-stack wrapper or NR-28's lean entry
    if (f == (void *)nvn_prof2_entry || f == (void *)nvn_trace_entry || f == (void *)nvn_prof_entry || f == (void *)nvn_prof_entry_v80) f = nvn_real[i];
    return f;
}
int nvn_nc_ready(const void *thunk[NVN_NC_N]) {
    static const char *const name[NVN_NC_N] = {"nvnDeviceGetTextureHandle", "nvnCommandBufferBindTexture", "nvnCommandBufferDrawElements",
                                               "nvnCommandBufferDrawElementsInstanced", "nvnCommandBufferSetRenderEnable"};
    void *const want[NVN_NC_N] = {(void *)h_nvnDeviceGetTextureHandle, (void *)h_nvnCommandBufferBindTexture, (void *)h_nvnCommandBufferDrawElements,
                                  (void *)h_nvnCommandBufferDrawElementsInstanced, (void *)h_nvnCommandBufferSetRenderEnable};
    void *const *tab = nvn_thunk_tab();   // as nvn_lookup picks it (NR-28: the plain set with DM_NR_LEANCALL=1)
    int ok = g_nc_mode != 0;
    for (int k = 0; k < NVN_NC_N; k++) {
        const int i = nc_slot(name[k]);
        thunk[k] = i >= 0 ? tab[i] : NULL;
        if (i < 0 || nc_impl(i) != want[k]) ok = 0;
    }
    return ok;
}
uint64_t nvn_nc_handle(int32_t tex, int32_t smp) { return (uint64_t)h_nvnDeviceGetTextureHandle(NULL, tex, smp); }
void nvn_nc_bind(uint64_t cb, int stage, int slot, int32_t tex, int32_t smp) {
    h_nvnCommandBufferBindTexture((NVNcommandBuffer *)(uintptr_t)cb, (NVNshaderStage)stage, slot, h_nvnDeviceGetTextureHandle(NULL, tex, smp));
}
void nvn_nc_ren(uint64_t cb) { h_nvnCommandBufferSetRenderEnable((NVNcommandBuffer *)(uintptr_t)cb, 1); }
void nvn_nc_draw(uint64_t cb, int32_t prim, uint32_t itype, int32_t count, uint64_t ib, int32_t instances) {
    if (instances < 0) h_nvnCommandBufferDrawElements((NVNcommandBuffer *)(uintptr_t)cb, (NVNdrawPrimitive)prim, (NVNindexType)itype, count, (NVNbufferAddress)ib);
    else h_nvnCommandBufferDrawElementsInstanced((NVNcommandBuffer *)(uintptr_t)cb, (NVNdrawPrimitive)prim, (NVNindexType)itype, count, (NVNbufferAddress)ib, 0, 0, instances);
}
static void *nc_nop_job(void *a) { return a; }
int nvn_nc_calibrate(double out[3]) {
    int nop = -1;
    for (unsigned i = 0; i < NVN_NFUNC && nop < 0; i++) if (nc_impl((int)i) == (void *)nvn_nop) nop = (int)i;
    out[0] = out[1] = out[2] = 0;
    if (nop < 0) return 0;
    enum { N = 4096 };
    void (*const f)(void) = (void (*)(void))nvn_thunk_tab()[nop];
    void (*volatile g)(void) = nvn_nop;
    void *(*volatile j)(void *) = nc_nop_job;
    const uint64_t calls0 = t_nc_calls;   // the calls of the calibration itself do not count
    for (int k = 0; k < 256; k++) { f(); g(); nvn_on_bigstack(j, NULL); }
    uint64_t a = now_ns(); for (int k = 0; k < N; k++) f(); out[0] = (double)(now_ns() - a) / N;
    a = now_ns(); for (int k = 0; k < N; k++) g(); out[1] = (double)(now_ns() - a) / N;
    a = now_ns(); for (int k = 0; k < N; k++) nvn_on_bigstack(j, NULL); out[2] = (double)(now_ns() - a) / N;
    t_nc_calls = calls0;
    return 1;
}
#endif
#else
#ifndef NCT
#define NCT(x) do { } while (0)
#endif
#endif
