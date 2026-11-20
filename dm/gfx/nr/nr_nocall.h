// [DOS2] NR-23 (DM_NR_NOCALL, default 0; docs/dos2/nr/NR-23.md): the part of an engine draw that needs no NVN call of the engine.
// Pure C, no engine / host / NVN type: used by the front end (dm/host/hle_dos2_nr_nocall.inc) and by the tests
// (dm/gfx/vulkan/engine/tests/nr_nocall_test.c, nr_nocall_oracle.py: the same functions against the game's own machine code in an
// emulator). main = main.raw of update v786432, build 4979b200.
//
// What the engine does at a draw (NATIVE_PLAN.md 1.7), in NVN calls:
//   0x926f90 + 0x925740   uniform blocks -> BindUniformBuffer. Not modelled: 5.9 KB of code with the ring allocator and its spin lock;
//                         the engine's own code keeps running and its calls keep coming through the thunks.
//   0x924730              the texture flush: for every slot of a dirty range with a texture id, GetTextureHandle(dev + 8, id, sampler)
//                         and BindTexture(cb, stage, slot, handle); then the ranges are reset. Stages vertex (0), fragment (1),
//                         compute (5) in this order. MODELLED: nrnc_flush_plan / nrnc_flush_run.
//   0x928e80 / 0x928fb0   the draw: index buffer record of the bound handle, [uniform blocks], the flush, SetRenderEnable(cb, 1) when
//                         the engine had turned rendering off, DrawElements / DrawElementsInstanced, one flag byte. MODELLED:
//                         nrnc_draw_pre / nrnc_draw_tail (the two uniform functions are called by the front end in between).
// Every NVN call the model makes goes through nrnc_ops: the front end passes the HLE's own entry points (the bodies the engine's calls
// reach through the thunks), the tests a recorder. The model never calls anything else and writes only the fields the machine code writes.
//
// Guest memory is read in place: NRNC_PTR(address) is the host pointer of a guest address (the identity in the port; the tests map an
// arena). Every load / store is a memcpy (no alignment or aliasing assumption).
#ifndef DM_NR_NOCALL_H
#define DM_NR_NOCALL_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef NRNC_PTR
#define NRNC_PTR(a) ((uint8_t *)(uintptr_t)(a))
#endif

// The renderer's fields (r +): see NATIVE_PLAN.md 1.7; tools/dos2_nr_check.py checks the stores of the texture cache.
enum {
    NRNC_R_DEV = 0xb70,        // device; + 0x5248 = the recording context, + 0x38 of it = the NVN command buffer
    NRNC_DEV_CTX = 0x5248, NRNC_CTX_CB = 0x38,
    NRNC_R_IB_PAGES = 0x240, NRNC_R_IB_GENS = 0x248, NRNC_R_IB_PER = 0x250,   // index buffer table: pages of records, generations, records a page
    NRNC_IB_REC = 0x58, NRNC_IB_ADDR = 8, NRNC_IB_TYPE = 0x24,
    NRNC_R_IB_HANDLE = 0x17c8, NRNC_R_IB_FIRST = 0x17d0, NRNC_R_PRIM = 0x1718, NRNC_R_SHADER_REC = 0x17e0,
    NRNC_R_REN_NEED = 0x185c, NRNC_R_REN_OFF = 0x185d, NRNC_R_REN_Q = 0x1860,
    NRNC_STAGES = 3, NRNC_SLOTS = 32,
};
// Per stage, in the order the flush walks them: NVN stage, texture ids, sampler ids, the texture dirty range (i8 min, i8 max) and the
// sampler dirty range the flush resets with it.
typedef struct { uint8_t nvn_stage; uint16_t tex, smp, range, smp_range; } nrnc_stage_t;
static const nrnc_stage_t k_nrnc_stage[NRNC_STAGES] = {
    {0, 0x1558, 0x13c0, 0x15dc, 0x1444},
    {1, 0x15e0, 0x1448, 0x1664, 0x14cc},
    {5, 0x1668, 0x14d0, 0x16ec, 0x1554},
};
enum { NRNC_CLEAN = 0xff20 };   // a reset range: min 0x20, max 0xff (little-endian u16)

typedef struct { uint8_t stage, slot; int32_t tex, smp; } nrnc_bind;   // stage = NVN stage (0 / 1 / 5)
typedef struct { int n, per[NRNC_STAGES]; nrnc_bind b[NRNC_STAGES * NRNC_SLOTS]; } nrnc_plan;

typedef struct nrnc_ops {
    void (*bind)(void *u, uint64_t cb, int stage, int slot, int32_t tex, int32_t smp);   // GetTextureHandle(tex, smp) + BindTexture
    void (*ren)(void *u, uint64_t cb);                                                   // SetRenderEnable(cb, 1)
    // DrawElements(cb, prim, itype, count, ib) when instances < 0, else DrawElementsInstanced(cb, prim, itype, count, ib, 0, 0, instances)
    void (*draw)(void *u, uint64_t cb, int32_t prim, uint32_t itype, int32_t count, uint64_t ib, int32_t instances);
} nrnc_ops;

static inline uint64_t nrnc_r64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint32_t nrnc_r32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void nrnc_w16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

// The command buffer the engine records into (0 = the device or its context is not there: the caller lets the engine's own code run).
static inline uint64_t nrnc_cb(const uint8_t *R) {
    const uint64_t dev = nrnc_r64(R + NRNC_R_DEV);
    if (!dev) return 0;
    const uint64_t ctx = nrnc_r64(NRNC_PTR(dev) + NRNC_DEV_CTX);
    return ctx ? ctx + NRNC_CTX_CB : 0;
}

// ---- the texture flush 0x924730 ----
// The binds the flush would make for the renderer as it is now, in its order. Reads only. 1 = planned; 0 = a dirty range reaches
// outside slots 0..31 (the machine code would walk memory next to the slot tables: the caller lets it do that itself).
static inline int nrnc_flush_plan(const uint8_t *R, nrnc_plan *p) {
    p->n = 0;
    for (int s = 0; s < NRNC_STAGES; s++) {
        const nrnc_stage_t *st = &k_nrnc_stage[s];
        const int lo = (int8_t)R[st->range], hi = (int8_t)R[st->range + 1];
        p->per[s] = 0;
        if (lo > hi) continue;                       // clean
        if (lo < 0 || hi >= NRNC_SLOTS) return 0;
        for (int i = lo; i <= hi; i++) {
            const int32_t tex = (int32_t)nrnc_r32(R + st->tex + 4 * i);
            if (tex == -1) continue;
            nrnc_bind *b = &p->b[p->n++];
            b->stage = st->nvn_stage; b->slot = (uint8_t)i; b->tex = tex; b->smp = (int32_t)nrnc_r32(R + st->smp + 4 * i);
            p->per[s]++;
        }
    }
    return 1;
}
// The stores of the flush for stage s (after its binds, before the next stage's range is read - the machine code's order).
static inline void nrnc_flush_reset(uint8_t *R, int s) {
    nrnc_w16(R + k_nrnc_stage[s].range, NRNC_CLEAN);
    nrnc_w16(R + k_nrnc_stage[s].smp_range, NRNC_CLEAN);
}
// The flush itself from a plan made right before (nothing ran in between): the binds of a stage, its resets, the next stage.
// cb = nrnc_cb(R) read by the caller before (the machine code reads the context once at entry).
static inline void nrnc_flush_run(uint8_t *R, const nrnc_plan *p, uint64_t cb, const nrnc_ops *ops, void *u) {
    int k = 0;
    for (int s = 0; s < NRNC_STAGES; s++) {
        for (int e = k + p->per[s]; k < e; k++) ops->bind(u, cb, p->b[k].stage, p->b[k].slot, p->b[k].tex, p->b[k].smp);
        nrnc_flush_reset(R, s);
    }
}
// Every range clean (what the flush leaves): 1 / 0.
static inline int nrnc_flush_clean(const uint8_t *R) {
    for (int s = 0; s < NRNC_STAGES; s++) {
        uint16_t a, b; memcpy(&a, R + k_nrnc_stage[s].range, 2); memcpy(&b, R + k_nrnc_stage[s].smp_range, 2);
        if (a != NRNC_CLEAN || b != NRNC_CLEAN) return 0;
    }
    return 1;
}

// ---- the draw functions 0x928e80 (op 36: r, first index, count) and 0x928fb0 (op 37: r, -, instances, -, count) ----
typedef struct {
    int op;                       // 36 / 37
    int32_t first, count, instances;
    uint64_t rec, cb;             // the index buffer's record (0 = the handle names none), the command buffer read at entry
    int ubo;                      // r + 0x17e0 is set: the caller runs 0x926f90(r) and 0x925740(r) before the tail
} nrnc_draw;
// The index buffer record of the handle at r + 0x17c8 (generation checked), as both functions look it up at entry. 0 = none.
static inline uint64_t nrnc_ib_rec(const uint8_t *R) {
    const uint64_t h = nrnc_r64(R + NRNC_R_IB_HANDLE);
    if (!(h & 0x3fffff00000000ull)) return 0;
    const uint32_t per = nrnc_r32(R + NRNC_R_IB_PER), idx = (uint32_t)h;
    const uint32_t page = per ? idx / per : 0, off = idx - page * per;   // udiv by 0 = 0 on AArch64
    const uint64_t gens = nrnc_r64(NRNC_PTR(nrnc_r64(R + NRNC_R_IB_GENS)) + (uint64_t)page * 8);
    if ((uint32_t)(h >> 32 & 0x3fffff) != nrnc_r32(NRNC_PTR(gens) + (uint64_t)off * 4)) return 0;
    return nrnc_r64(NRNC_PTR(nrnc_r64(R + NRNC_R_IB_PAGES)) + (uint64_t)page * 8) + (uint64_t)off * NRNC_IB_REC;
}
// Entry of the draw function. 1 = the tail can run natively; 0 = not eligible (no index buffer record: the machine code would read
// address 0x24; no command buffer; a per-page count of 0) - nothing was written, the caller runs the engine's function.
static inline int nrnc_draw_pre(const uint8_t *R, int op, int32_t first, int32_t count, int32_t instances, nrnc_draw *d) {
    d->op = op; d->first = first; d->count = count; d->instances = instances;
    d->cb = nrnc_cb(R);
    if (!d->cb || !nrnc_r32(R + NRNC_R_IB_PER) || !(nrnc_r64(R + NRNC_R_IB_HANDLE) & 0x3fffff00000000ull)) return 0;
    if (!nrnc_r64(R + NRNC_R_IB_GENS) || !nrnc_r64(R + NRNC_R_IB_PAGES)) return 0;
    d->rec = nrnc_ib_rec(R);
    d->ubo = nrnc_r64(R + NRNC_R_SHADER_REC) != 0;
    return d->rec != 0;
}
// After the flush: SetRenderEnable(cb, 1) when rendering was turned off (r + 0x185d) and is needed (r + 0x185c) - on the command
// buffer as it is NOW, as the machine code re-reads it - then the draw on the command buffer read at entry, then the flag byte.
// isz = the engine's index size table (main + 0x1f5865c, i32 per index type), read in place as the machine code reads it.
enum { NRNC_ISZ_TABLE = 0x1f5865c };
static inline void nrnc_draw_tail(uint8_t *R, const nrnc_draw *d, const uint8_t *isz, const nrnc_ops *ops, void *u) {
    if (R[NRNC_R_REN_OFF] && R[NRNC_R_REN_NEED]) {
        ops->ren(u, nrnc_r64(NRNC_PTR(nrnc_r64(R + NRNC_R_DEV)) + NRNC_DEV_CTX) + NRNC_CTX_CB);
        nrnc_w16(R + NRNC_R_REN_NEED, 0);
        memset(R + NRNC_R_REN_Q, 0, 8);
    }
    const uint8_t *rec = NRNC_PTR(d->rec);
    const uint32_t itype = nrnc_r32(rec + NRNC_IB_TYPE);
    const int64_t size = (int32_t)nrnc_r32(isz + (uint64_t)itype * 4);
    const uint64_t base = nrnc_r64(rec + NRNC_IB_ADDR);
    const int32_t prim = (int32_t)nrnc_r32(R + NRNC_R_PRIM);
    if (d->op == 36) ops->draw(u, d->cb, prim, itype, d->count, (uint64_t)size * (uint64_t)(int64_t)d->first + base, -1);
    else ops->draw(u, d->cb, prim, itype, d->count, nrnc_r64(R + NRNC_R_IB_FIRST) * (uint64_t)size + base, d->instances);
    R[NRNC_R_REN_OFF] = R[NRNC_R_REN_NEED];
}
// The model of the tail without running it (DM_NR_NOCALL=2: what the engine's code is about to do). Reads only.
typedef struct { int ren; uint64_t ren_cb, cb, ib; int32_t prim, count, instances; uint32_t itype; } nrnc_expect;
static inline void nrnc_draw_expect(const uint8_t *R, const nrnc_draw *d, const uint8_t *isz, nrnc_expect *e) {
    memset(e, 0, sizeof *e);
    e->ren = R[NRNC_R_REN_OFF] && R[NRNC_R_REN_NEED];
    if (e->ren) e->ren_cb = nrnc_r64(NRNC_PTR(nrnc_r64(R + NRNC_R_DEV)) + NRNC_DEV_CTX) + NRNC_CTX_CB;
    const uint8_t *rec = NRNC_PTR(d->rec);
    e->itype = nrnc_r32(rec + NRNC_IB_TYPE);
    const int64_t size = (int32_t)nrnc_r32(isz + (uint64_t)e->itype * 4);
    const uint64_t base = nrnc_r64(rec + NRNC_IB_ADDR);
    e->prim = (int32_t)nrnc_r32(R + NRNC_R_PRIM); e->count = d->count; e->cb = d->cb;
    if (d->op == 36) { e->ib = (uint64_t)size * (uint64_t)(int64_t)d->first + base; e->instances = -1; }
    else { e->ib = nrnc_r64(R + NRNC_R_IB_FIRST) * (uint64_t)size + base; e->instances = d->instances; }
}

// ---- the code the model stands for: FNV-1a 64 over the words of each function after its first four (the hooks patch those; they are
// compared word for word by the hook installers). Computed from main.raw by nr_nocall_oracle.py (it also asserts these constants). ----
typedef struct { uint32_t off, end; uint64_t fnv; const char *name; } nrnc_code;
static inline uint64_t nrnc_fnv(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}
enum { NRNC_CODE_FLUSH, NRNC_CODE_DRAW, NRNC_CODE_DRAWI, NRNC_CODE_N };
#define NRNC_CODE_TABLE { \
    {0x924730, 0x9249e4, 0xabf3b6f2ef915a55ull, "texture flush"}, \
    {0x928e80, 0x928fac, 0x26bc491360cf210cull, "DrawElements"}, \
    {0x928fb0, 0x9290e8, 0xe006f4c51f32cc50ull, "DrawElementsInstanced"} }
// The engine's NVN function pointer slots the modelled code calls through (main +; filled by the engine's loader 0x934f00).
enum { NRNC_SLOT_HANDLE = 0x2bbeee8, NRNC_SLOT_BIND = 0x2bbfab0, NRNC_SLOT_DRAW = 0x2bbfb20, NRNC_SLOT_DRAWI = 0x2bbfb38, NRNC_SLOT_REN = 0x2bbfcd0 };
// The two functions the draw calls before the flush when a shader record is bound (called as they are, never modelled).
enum { NRNC_FN_UBO_FILL = 0x926f90, NRNC_FN_UBO_BIND = 0x925740, NRNC_FN_FLUSH = 0x924730, NRNC_FN_DRAW = 0x928e80, NRNC_FN_DRAWI = 0x928fb0,
       NRNC_LR_DRAW = 0x928f10, NRNC_LR_DRAWI = 0x929040 };   // the return addresses of the two flush calls of the draw functions

// ---- DM_NR_NOCALL=2: the cost model of the 10 s line (ns; negative = it would cost) ----
// calls = NVN calls =1 no longer makes through the thunks, switches = stack switches it makes instead (one per native flush with a bind
// and per native draw), thunk_ns = an NVN call through the thunk and the big-stack wrapper with an empty body, direct_ns = the same
// body called directly, switch_ns = one nvn_on_bigstack round trip.
static inline double nrnc_saving_ns(double calls, double switches, double thunk_ns, double direct_ns, double switch_ns) {
    return calls * (thunk_ns - direct_ns) - switches * switch_ns;
}
#endif
