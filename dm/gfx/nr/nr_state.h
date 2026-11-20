// [DOS2] NR-09S (DM_NR_STATE, default 0; docs/dos2/nr/NR-09S.md): the engine's "all state of a draw" packet (ExecuteRenderCommand
// 0x93e5f0, opcode 21, 0x70 bytes; handler at 0x93e6ac with SetShader 0x9283a0 and SetBlend 0x9284e0) as native code.
// Pure C, no engine / host / NVN type: used by the front end (dm/host/hle_dos2_nr_state.inc) and by the tests
// (dm/gfx/vulkan/engine/tests/nr_state_test.c, nr_state_oracle.py: the same function against the game's own machine code in Unicorn).
// main = main.raw of update v786432, build 4979b200.
//
// What the packet does (machine code, in this order; every field compare is the engine's own filter):
//   SetShader(r, pkt+8)        shader handle -> record (table r+0xc0, 0x60 B, generation checked) -> BindProgram VS (1) + FS (2), or
//                              the record's compute program (0x20) with r+0xb78 = 1, or three NULL binds; the record gets the frame stamp
//   depth-stencil pkt+0x10 / stencil ref pkt+0x18   -> BindDepthStencilState + SetStencilMask + SetStencilValueMask + SetStencilRef (face 3)
//   raster pkt+0x20            -> BindPolygonState
//   SetBlend(r, pkt+0x28, -1)  blend handle -> per colour target bound (r+0x17e8 + 8 i): BlendStateSetBlendTarget + BindBlendState; both
//                              colour states of the record get ColorStateSetBlendEnable(i, bound); BindColorState (the alpha-test one
//                              when r+0x1868 >= 0). The special handle [main+0x2bbdb70]: default blend / colour states on the stack.
//   vertex format pkt+0x30     -> BindVertexAttribState(16) + BindVertexStreamState(1..3)
//   vertex buffers pkt+0x38/40/48 -> BindVertexBuffer(i, address, size) + frame stamp (table r+0x180, 0x68 B); 1 / 2 only when non-zero
//   index buffer pkt+0x50      -> no NVN call: r+0x17c8, the record's frame stamp (table r+0x240, 0x58 B, no generation check)
//   primitive pkt+0x58         -> r+0x1714 / r+0x1718 (table main+0x1f58640); conditional render pkt+0x60 -> SetRenderEnable(1) or
//                              SetRenderEnableConditional(1, [pkt+0x60]); r+0x185d = 0
//   alpha ref pkt+0x68         -> SetAlphaRef when >= 0, then SetBlend again with the blend forced (the colour state follows the test)
// The model is split in two so that one function serves every mode:
//   nrst_code21   reads guest memory only and returns the whole effect: the renderer fields as they end (nrst_rs, `stored` = the ones the
//                 machine code writes), the frame stamps it writes into table records, and the NVN calls in order (nrst_op: the compact
//                 native state record of the packet). 0 = a case the machine code would turn into a bad pointer / a fault: left to the engine.
//   nrst_commit   writes the stored fields and the stamps. The caller then makes the calls (front end: the HLE bodies on the big stack).
// Writes first, calls after: no NVN body reads the renderer fields or the stamps (they read the state objects the calls point at, which
// the packet never writes). Every load / store is a memcpy (no alignment or aliasing assumption); NRST_PTR(guest address) = host pointer.
#ifndef DM_NR_STATE_H
#define DM_NR_STATE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef NRST_PTR
#define NRST_PTR(a) ((uint8_t *)(uintptr_t)(a))
#endif

// Renderer fields (r +) the packet reads or writes; tables (pages, generations, per page) at T, T + 8, T + 0x10 (u32).
enum {
    NRST_R_DEV = 0xb70, NRST_DEV_CTX = 0x5248, NRST_DEV_FRAME = 0x51f0, NRST_CTX_CB = 0x38,
    NRST_R_CSFLAG = 0xb78, NRST_R_RASTER = 0x16f0, NRST_R_BLEND = 0x16f8, NRST_R_BLEND_MASK = 0x1700, NRST_R_DS = 0x1708, NRST_R_REF = 0x1710,
    NRST_R_PRIM = 0x1714, NRST_R_NVN_PRIM = 0x1718, NRST_R_VFMT = 0x1740, NRST_R_VB = 0x1748, NRST_R_VB_DIRTY = 0x1788, NRST_R_IB = 0x17c8,
    NRST_R_IB_FIRST = 0x17d0, NRST_R_SHADER = 0x17d8, NRST_R_SHADER_REC = 0x17e0, NRST_R_RT = 0x17e8, NRST_R_REN_NEED = 0x185c,
    NRST_R_REN_OFF = 0x185d, NRST_R_REN_Q = 0x1860, NRST_R_ALPHA = 0x1868,
    NRST_T_SHADER = 0xc0, NRST_T_VB = 0x180, NRST_T_IB = 0x240, NRST_T_VFMT = 0x480, NRST_T_BLEND = 0x600, NRST_T_DS = 0x6c0, NRST_T_RASTER = 0x780,
    NRST_REGION = 0x16f0, NRST_REGION_END = 0x1870,   // the renderer bytes =2 snapshots (every field above but 0xb78 lies inside)
};
// The NVN calls of the packet (op.fn) and their arguments: a = command buffer, p = object / address, q = size, i / j = int args, f = float.
enum {
    NRST_F_PROGRAM = 1,   // BindProgram(a, p, i)
    NRST_F_DS,            // BindDepthStencilState(a, p)
    NRST_F_STENCIL_MASK,  // SetStencilMask(a, i, j)       (slot main+0x2bbfc08, called first)
    NRST_F_STENCIL_VMASK, // SetStencilValueMask(a, i, j)  (main+0x2bbfc00)
    NRST_F_STENCIL_REF,   // SetStencilRef(a, i, j)
    NRST_F_POLY,          // BindPolygonState(a, p)
    NRST_F_BLEND_DEF,     // BlendStateSetDefaults(p)
    NRST_F_BLEND,         // BindBlendState(a, p)
    NRST_F_COLOR_DEF,     // ColorStateSetDefaults(p)
    NRST_F_COLOR_ATEST,   // ColorStateSetAlphaTest(p, i)
    NRST_F_COLOR,         // BindColorState(a, p)
    NRST_F_BLEND_TARGET,  // BlendStateSetBlendTarget(p, i)
    NRST_F_COLOR_BLEND,   // ColorStateSetBlendEnable(p, i, j)
    NRST_F_VATTR,         // BindVertexAttribState(a, i, p)
    NRST_F_VSTREAM,       // BindVertexStreamState(a, i, p)
    NRST_F_VB,            // BindVertexBuffer(a, i, p, q)
    NRST_F_REN,           // SetRenderEnable(a, i)
    NRST_F_REN_COND,      // SetRenderEnableConditional(a, i, p)
    NRST_F_ALPHA_REF,     // SetAlphaRef(a, f)
    NRST_F_N
};
// p of the special blend path: the engine's stack objects (sp + 0x18 blend state, sp + 8 colour state); the caller supplies its own.
enum { NRST_LOCAL_BLEND = 1, NRST_LOCAL_COLOR = 2 };
typedef struct { uint32_t fn; int32_t i, j; float f; uint64_t a, p, q; } nrst_op;
enum { NRST_OPS_MAX = 96, NRST_STAMPS_MAX = 8 };   // worst case 3 + 4 + 1 + 33 + 2 + 3 + 1 + 1 + 33 = 81 calls; stamps 1 + 3 + 1
// Why a packet is left to the engine (the machine code would hand a bad pointer to an NVN call or read through a null table pointer).
enum { NRST_OK, NRST_NO_DEV, NRST_NO_CTX, NRST_BAD_DS, NRST_BAD_RASTER, NRST_BAD_BLEND, NRST_BAD_VFMT, NRST_NULL_TABLE, NRST_WHY_N };
// The renderer fields as the packet leaves them.
typedef struct {
    uint64_t shader, shader_rec, ds, raster, blend, vfmt, vb[3], vb_dirty[3], ib, ib_first, ren_q;
    uint32_t csflag, ref, blend_mask, nvn_prim; float alpha; uint8_t prim, ren_need, ren_off;
} nrst_rs;
enum {   // nrst_run.stored: the fields the machine code writes (a store of the same value counts)
    NRST_S_SHADER = 1u << 0, NRST_S_SHADER_REC = 1u << 1, NRST_S_CSFLAG = 1u << 2, NRST_S_DS = 1u << 3, NRST_S_REF = 1u << 4,
    NRST_S_RASTER = 1u << 5, NRST_S_BLEND = 1u << 6, NRST_S_BLEND_MASK = 1u << 7, NRST_S_VFMT = 1u << 8, NRST_S_VB0 = 1u << 9,
    NRST_S_VBD0 = 1u << 12, NRST_S_IB = 1u << 15, NRST_S_IB_FIRST = 1u << 16, NRST_S_PRIM = 1u << 17, NRST_S_NVN_PRIM = 1u << 18,
    NRST_S_REN_NEED = 1u << 19, NRST_S_REN_Q = 1u << 20, NRST_S_REN_OFF = 1u << 21, NRST_S_ALPHA = 1u << 22,
};
typedef struct { uint64_t a, v; } nrst_stamp;
typedef struct {
    int why, nops, nstamps;
    uint32_t stored;
    uint64_t cb;
    nrst_rs rs;
    nrst_stamp stamp[NRST_STAMPS_MAX];
    nrst_op op[NRST_OPS_MAX];
} nrst_run;

static inline uint64_t nrst_r64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint32_t nrst_r32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void nrst_w64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static inline void nrst_w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static inline void nrst_rs_read(const uint8_t *R, nrst_rs *x) {
    x->shader = nrst_r64(R + NRST_R_SHADER); x->shader_rec = nrst_r64(R + NRST_R_SHADER_REC); x->ds = nrst_r64(R + NRST_R_DS);
    x->raster = nrst_r64(R + NRST_R_RASTER); x->blend = nrst_r64(R + NRST_R_BLEND); x->vfmt = nrst_r64(R + NRST_R_VFMT);
    for (int k = 0; k < 3; k++) { x->vb[k] = nrst_r64(R + NRST_R_VB + 8 * k); x->vb_dirty[k] = nrst_r64(R + NRST_R_VB_DIRTY + 8 * k); }
    x->ib = nrst_r64(R + NRST_R_IB); x->ib_first = nrst_r64(R + NRST_R_IB_FIRST); x->ren_q = nrst_r64(R + NRST_R_REN_Q);
    x->csflag = nrst_r32(R + NRST_R_CSFLAG); x->ref = nrst_r32(R + NRST_R_REF); x->blend_mask = nrst_r32(R + NRST_R_BLEND_MASK);
    x->nvn_prim = nrst_r32(R + NRST_R_NVN_PRIM); memcpy(&x->alpha, R + NRST_R_ALPHA, 4);
    x->prim = R[NRST_R_PRIM]; x->ren_need = R[NRST_R_REN_NEED]; x->ren_off = R[NRST_R_REN_OFF];
}
static inline void nrst_op_put(nrst_run *s, uint32_t fn, uint64_t a, uint64_t p, uint64_t q, int32_t i, int32_t j, float f) {
    if (s->nops < NRST_OPS_MAX) { nrst_op *o = &s->op[s->nops]; o->fn = fn; o->a = a; o->p = p; o->q = q; o->i = i; o->j = j; o->f = f; }
    s->nops++;
}
#define NRST_OP(s, fn, a, p, q, i, j) nrst_op_put((s), (fn), (a), (p), (q), (i), (j), 0.f)

// Table lookup as the machine code does it (SetShader 0x9283c4..0x928400; the same shape for every table). 1 = *e is the record
// (0 = the handle names none: no generation bits or a stale generation); 0 = the machine code would read through a null pointer here.
// A null page base gives the record address off * esz as the machine code computes it (0 for off 0, else not a record: refused).
static inline int nrst_lookup(const uint8_t *R, uint32_t tab, uint64_t esz, uint64_t h, uint64_t *e) {
    *e = 0;
    if (!(h & 0x3fffff00000000ull)) return 1;
    const uint32_t per = nrst_r32(R + tab + 0x10), idx = (uint32_t)h;
    const uint32_t page = per ? idx / per : 0, off = idx - page * per;   // udiv by 0 = 0 on AArch64
    const uint64_t gp = nrst_r64(R + tab + 8);
    if (!gp) return 0;
    const uint64_t gens = nrst_r64(NRST_PTR(gp) + (uint64_t)page * 8);
    if (!gens) return 0;
    if ((uint32_t)(h >> 32 & 0x3fffff) != nrst_r32(NRST_PTR(gens) + (uint64_t)off * 4)) return 1;
    const uint64_t pp = nrst_r64(R + tab);
    if (!pp) return 0;
    const uint64_t base = nrst_r64(NRST_PTR(pp) + (uint64_t)page * 8);
    if (!base && off) return 0;
    *e = base + (uint64_t)off * esz;
    return 1;
}
static inline int nrst_refuse(nrst_run *s, int why) { s->why = why; return 0; }

// SetShader 0x9283a0 (r, pkt + 8).
static inline int nrst_set_shader(nrst_run *s, const uint8_t *R, uint64_t h, uint64_t frame) {
    nrst_rs *x = &s->rs;
    if (x->shader == h) return 1;
    x->shader = h; s->stored |= NRST_S_SHADER;
    uint64_t e;
    if (!nrst_lookup(R, NRST_T_SHADER, 0x60, h, &e)) return nrst_refuse(s, NRST_NULL_TABLE);
    x->shader_rec = e; s->stored |= NRST_S_SHADER_REC;
    const uint64_t cb = s->cb;
    if (!e) { for (int st = 1; st <= 2; st++) NRST_OP(s, NRST_F_PROGRAM, cb, 0, 0, st, 0); NRST_OP(s, NRST_F_PROGRAM, cb, 0, 0, 0x20, 0); return 1; }
    const uint8_t *E = NRST_PTR(e);
    s->stamp[s->nstamps].a = e; s->stamp[s->nstamps].v = frame; s->nstamps++;
    const uint64_t vs = nrst_r64(E + 8), fs = nrst_r64(E + 0x10);
    if (!vs || !fs) { x->csflag = 1; s->stored |= NRST_S_CSFLAG; NRST_OP(s, NRST_F_PROGRAM, cb, nrst_r64(E + 0x18), 0, 0x20, 0); return 1; }
    NRST_OP(s, NRST_F_PROGRAM, cb, vs, 0, 1, 0);
    NRST_OP(s, NRST_F_PROGRAM, cb, fs, 0, 2, 0);
    return 1;
}
// SetBlend 0x9284e0 (r, &b, mask). special = [main + 0x2bbdb70] (the engine's default-blend handle).
static inline int nrst_set_blend(nrst_run *s, const uint8_t *R, uint64_t b, uint32_t mask, uint64_t special) {
    nrst_rs *x = &s->rs;
    uint64_t v;
    if (!x->blend || b != x->blend) v = b;
    else if (x->blend_mask != mask) v = x->blend;
    else return 1;
    x->blend = v; x->blend_mask = mask; s->stored |= NRST_S_BLEND | NRST_S_BLEND_MASK;
    if (!b) return 1;
    const uint64_t cb = s->cb;
    if (b == special) {
        NRST_OP(s, NRST_F_BLEND_DEF, 0, NRST_LOCAL_BLEND, 0, 0, 0);
        NRST_OP(s, NRST_F_BLEND, cb, NRST_LOCAL_BLEND, 0, 0, 0);
        NRST_OP(s, NRST_F_COLOR_DEF, 0, NRST_LOCAL_COLOR, 0, 0, 0);
        if (x->alpha >= 0.f) NRST_OP(s, NRST_F_COLOR_ATEST, 0, NRST_LOCAL_COLOR, 0, 5, 0);
        NRST_OP(s, NRST_F_COLOR, cb, NRST_LOCAL_COLOR, 0, 0, 0);
        return 1;
    }
    uint64_t e;
    if (!nrst_lookup(R, NRST_T_BLEND, 0x20, b, &e)) return nrst_refuse(s, NRST_NULL_TABLE);
    if (!e) return nrst_refuse(s, NRST_BAD_BLEND);
    for (int i = 0; i < 8; i++) {
        const int on = nrst_r64(R + NRST_R_RT + 8 * i) != 0;
        if (on) { NRST_OP(s, NRST_F_BLEND_TARGET, 0, e + 8, 0, i, 0); NRST_OP(s, NRST_F_BLEND, cb, e + 8, 0, 0, 0); }
        NRST_OP(s, NRST_F_COLOR_BLEND, 0, e + 0x10, 0, i, on);
        NRST_OP(s, NRST_F_COLOR_BLEND, 0, e + 0x14, 0, i, on);
    }
    NRST_OP(s, NRST_F_COLOR, cb, x->alpha >= 0.f ? e + 0x14 : e + 0x10, 0, 0, 0);
    return 1;
}
// One vertex buffer slot k of the packet (handle h; the caller skipped what the machine code skips).
static inline int nrst_vb(nrst_run *s, const uint8_t *R, int k, uint64_t h, uint64_t frame) {
    nrst_rs *x = &s->rs;
    uint64_t e = 0;
    if (h & 0x3fffff00000000ull) {
        const uint32_t per = nrst_r32(R + NRST_T_VB + 0x10), idx = (uint32_t)h;
        const uint32_t page = per ? idx / per : 0, off = idx - page * per;
        const uint64_t gp = nrst_r64(R + NRST_T_VB + 8);
        if (!gp) return nrst_refuse(s, NRST_NULL_TABLE);
        const uint64_t gens = nrst_r64(NRST_PTR(gp) + (uint64_t)page * 8);
        if (!gens) return nrst_refuse(s, NRST_NULL_TABLE);
        if ((uint32_t)(h >> 32 & 0x3fffff) == nrst_r32(NRST_PTR(gens) + (uint64_t)off * 4)) {
            const uint64_t pp = nrst_r64(R + NRST_T_VB);
            if (!pp) return nrst_refuse(s, NRST_NULL_TABLE);
            const uint64_t base = nrst_r64(NRST_PTR(pp) + (uint64_t)page * 8);
            if (!base && off) return nrst_refuse(s, NRST_NULL_TABLE);     // [off * 0x68] would fault
            e = base + (uint64_t)off * 0x68;
            if (e && !nrst_r64(NRST_PTR(e))) e = 0;                       // a record without a buffer: as a stale handle
        }
    }
    if (!e) { x->vb[k] = 0; x->vb_dirty[k] = 0; s->stored |= (NRST_S_VB0 | NRST_S_VBD0) << k; return 1; }
    const uint8_t *E = NRST_PTR(e);
    NRST_OP(s, NRST_F_VB, s->cb, nrst_r64(E + 8), nrst_r64(E + 0x40), k, 0);
    s->stamp[s->nstamps].a = e + 0x38; s->stamp[s->nstamps].v = frame; s->nstamps++;
    x->vb[k] = h; x->vb_dirty[k] = 0; s->stored |= (NRST_S_VB0 | NRST_S_VBD0) << k;
    return 1;
}

// The packet: R = renderer ([list + 0x38]), P = packet, special = [main + 0x2bbdb70], prim = main + 0x1f58640 (7 x i32).
// Reads only. 1 = s holds the effect; 0 = left to the engine (s->why).
static inline int nrst_code21(const uint8_t *R, const uint8_t *P, uint64_t special, const uint8_t *prim, nrst_run *s) {
    s->why = NRST_OK; s->nops = 0; s->nstamps = 0; s->stored = 0;
    const uint64_t dev = nrst_r64(R + NRST_R_DEV);
    if (!dev) return nrst_refuse(s, NRST_NO_DEV);
    const uint64_t ctx = nrst_r64(NRST_PTR(dev) + NRST_DEV_CTX);
    if (!ctx) return nrst_refuse(s, NRST_NO_CTX);
    const uint64_t frame = nrst_r64(NRST_PTR(dev) + NRST_DEV_FRAME), cb = s->cb = ctx + NRST_CTX_CB;
    nrst_rs *x = &s->rs;
    nrst_rs_read(R, x);
    if (!nrst_set_shader(s, R, nrst_r64(P + 8), frame)) return 0;
    // depth-stencil + stencil ref (0x93e6bc / 0x93ecf4)
    const uint64_t pds = nrst_r64(P + 0x10); const uint32_t ref = nrst_r32(P + 0x18);
    if (!x->ds || pds != x->ds || x->ref != ref) {
        x->ds = pds; x->ref = ref; s->stored |= NRST_S_DS | NRST_S_REF;
        if (pds) {
            uint64_t e;
            if (!nrst_lookup(R, NRST_T_DS, 0x20, pds, &e)) return nrst_refuse(s, NRST_NULL_TABLE);
            if (!e) return nrst_refuse(s, NRST_BAD_DS);
            const int32_t m = NRST_PTR(e)[0x10];
            NRST_OP(s, NRST_F_DS, cb, e + 8, 0, 0, 0);
            NRST_OP(s, NRST_F_STENCIL_MASK, cb, 0, 0, 3, m);
            NRST_OP(s, NRST_F_STENCIL_VMASK, cb, 0, 0, 3, m);
            NRST_OP(s, NRST_F_STENCIL_REF, cb, 0, 0, 3, (int32_t)ref);
        }
    }
    // raster (0x93e6e8 / 0x93e9a4)
    const uint64_t pr = nrst_r64(P + 0x20);
    if (!x->raster || pr != x->raster) {
        x->raster = pr; s->stored |= NRST_S_RASTER;
        if (pr) {
            uint64_t e;
            if (!nrst_lookup(R, NRST_T_RASTER, 0x18, pr, &e)) return nrst_refuse(s, NRST_NULL_TABLE);
            if (!e) return nrst_refuse(s, NRST_BAD_RASTER);
            NRST_OP(s, NRST_F_POLY, cb, e + 8, 0, 0, 0);
        }
    }
    if (!nrst_set_blend(s, R, nrst_r64(P + 0x28), 0xffffffffu, special)) return 0;
    // vertex format (0x93e70c / 0x93e908)
    const uint64_t pv = nrst_r64(P + 0x30);
    if (x->vfmt != pv) {
        x->vfmt = pv; s->stored |= NRST_S_VFMT;
        if (pv) {
            uint64_t e;
            if (!nrst_lookup(R, NRST_T_VFMT, 0x80, pv, &e)) return nrst_refuse(s, NRST_NULL_TABLE);
            if (!e) return nrst_refuse(s, NRST_BAD_VFMT);
            const uint8_t *E = NRST_PTR(e);
            NRST_OP(s, NRST_F_VATTR, cb, e + 0x20, 0, 16, 0);
            NRST_OP(s, NRST_F_VSTREAM, cb, e + 0x60, 0, nrst_r32(E + 0x1c) ? 3 : nrst_r32(E + 0x18) ? 2 : 1, 0);
        }
    }
    // vertex buffers (0x93e71c, 0x93ea0c, 0x93f00c): slot 0 always compared, 1 / 2 only when the packet names one
    for (int k = 0; k < 3; k++) {
        const uint64_t h = nrst_r64(P + 0x38 + 8 * k);
        if (k && !h) continue;
        if (x->vb[k] == h && !x->vb_dirty[k]) continue;
        if (!nrst_vb(s, R, k, h, frame)) return 0;
    }
    // index buffer (0x93e7cc): no NVN call
    const uint64_t pib = nrst_r64(P + 0x50);
    if (pib && (x->ib != pib || x->ib_first)) {
        const uint32_t per = nrst_r32(R + NRST_T_IB + 0x10), idx = (uint32_t)pib;
        const uint32_t page = per ? idx / per : 0, off = idx - page * per;
        const uint64_t pp = nrst_r64(R + NRST_T_IB);
        if (!pp) return nrst_refuse(s, NRST_NULL_TABLE);
        const uint64_t base = nrst_r64(NRST_PTR(pp) + (uint64_t)page * 8);
        if (!base) return nrst_refuse(s, NRST_NULL_TABLE);
        s->stamp[s->nstamps].a = base + (uint64_t)off * 0x58 + 0x18; s->stamp[s->nstamps].v = frame; s->nstamps++;
        x->ib = pib; x->ib_first = 0; s->stored |= NRST_S_IB | NRST_S_IB_FIRST;
    }
    // primitive (0x93e814 / 0x93f218)
    const uint8_t pp8 = P[0x58];
    if (pp8 != x->prim) {
        x->prim = pp8; const uint32_t t = (uint32_t)pp8 - 1u;
        x->nvn_prim = t > 6 ? 0u : nrst_r32(prim + 4 * t); s->stored |= NRST_S_PRIM | NRST_S_NVN_PRIM;
    }
    // conditional render (0x93e824 / 0x93e888)
    const uint64_t q = nrst_r64(P + 0x60); const uint8_t need = q != 0;
    if (x->ren_need != need || x->ren_q != q) {
        if (!q) NRST_OP(s, NRST_F_REN, cb, 0, 0, 1, 0);
        else NRST_OP(s, NRST_F_REN_COND, cb, nrst_r64(NRST_PTR(q)), 0, 1, 0);
        x->ren_need = need; x->ren_q = q; s->stored |= NRST_S_REN_NEED | NRST_S_REN_Q;
    }
    x->ren_off = 0; s->stored |= NRST_S_REN_OFF;
    // alpha ref (0x93e850 / 0x93f130)
    float a; memcpy(&a, P + 0x68, 4);
    if (!(x->alpha == a)) {
        if (a >= 0.f) nrst_op_put(s, NRST_F_ALPHA_REF, cb, 0, 0, 0, 0, a);
        const uint64_t saved = x->blend;
        x->alpha = a; x->blend = 0; s->stored |= NRST_S_ALPHA | NRST_S_BLEND;
        if (!nrst_set_blend(s, R, saved, x->blend_mask, special)) return 0;
    }
    return s->nops <= NRST_OPS_MAX && s->nstamps <= NRST_STAMPS_MAX ? 1 : nrst_refuse(s, NRST_NULL_TABLE);
}
// The stores of the packet (after nrst_code21 said 1 on this very state).
static inline void nrst_commit(uint8_t *R, const nrst_run *s) {
    const nrst_rs *x = &s->rs; const uint32_t m = s->stored;
    if (m & NRST_S_SHADER) nrst_w64(R + NRST_R_SHADER, x->shader);
    if (m & NRST_S_SHADER_REC) nrst_w64(R + NRST_R_SHADER_REC, x->shader_rec);
    if (m & NRST_S_CSFLAG) nrst_w32(R + NRST_R_CSFLAG, x->csflag);
    if (m & NRST_S_DS) nrst_w64(R + NRST_R_DS, x->ds);
    if (m & NRST_S_REF) nrst_w32(R + NRST_R_REF, x->ref);
    if (m & NRST_S_RASTER) nrst_w64(R + NRST_R_RASTER, x->raster);
    if (m & NRST_S_BLEND) nrst_w64(R + NRST_R_BLEND, x->blend);
    if (m & NRST_S_BLEND_MASK) nrst_w32(R + NRST_R_BLEND_MASK, x->blend_mask);
    if (m & NRST_S_VFMT) nrst_w64(R + NRST_R_VFMT, x->vfmt);
    for (int k = 0; k < 3; k++) {
        if (m & (NRST_S_VB0 << k)) nrst_w64(R + NRST_R_VB + 8 * k, x->vb[k]);
        if (m & (NRST_S_VBD0 << k)) nrst_w64(R + NRST_R_VB_DIRTY + 8 * k, x->vb_dirty[k]);
    }
    if (m & NRST_S_IB) nrst_w64(R + NRST_R_IB, x->ib);
    if (m & NRST_S_IB_FIRST) nrst_w64(R + NRST_R_IB_FIRST, x->ib_first);
    if (m & NRST_S_PRIM) R[NRST_R_PRIM] = x->prim;
    if (m & NRST_S_NVN_PRIM) nrst_w32(R + NRST_R_NVN_PRIM, x->nvn_prim);
    if (m & NRST_S_REN_NEED) R[NRST_R_REN_NEED] = x->ren_need;
    if (m & NRST_S_REN_Q) nrst_w64(R + NRST_R_REN_Q, x->ren_q);
    if (m & NRST_S_REN_OFF) R[NRST_R_REN_OFF] = x->ren_off;
    if (m & NRST_S_ALPHA) memcpy(R + NRST_R_ALPHA, &x->alpha, 4);
    for (int i = 0; i < s->nstamps; i++) nrst_w64(NRST_PTR(s->stamp[i].a), s->stamp[i].v);
}
// DM_NR_STATE=2, after the engine's own handler ran on the state nrst_code21 read: the renderer as the model says it ends (every field,
// stored or not) against memory, the bytes of the region the model does not know against the snapshot taken before (pre), and the
// stamps. Returns the number of differences (0 = the same); *where = the first differing renderer offset (or ~0u for a stamp).
static inline int nrst_check_mem(const uint8_t *R, const uint8_t *pre, uint32_t pre_csflag, const nrst_run *s, uint32_t *where) {
    uint8_t want[NRST_REGION_END - NRST_REGION]; memcpy(want, pre, sizeof want);
    const nrst_rs *x = &s->rs; uint8_t *W = want - NRST_REGION;   // W[off] = the byte at renderer offset off
    nrst_w64(W + NRST_R_SHADER, x->shader); nrst_w64(W + NRST_R_SHADER_REC, x->shader_rec); nrst_w64(W + NRST_R_DS, x->ds);
    nrst_w32(W + NRST_R_REF, x->ref); nrst_w64(W + NRST_R_RASTER, x->raster); nrst_w64(W + NRST_R_BLEND, x->blend);
    nrst_w32(W + NRST_R_BLEND_MASK, x->blend_mask); nrst_w64(W + NRST_R_VFMT, x->vfmt);
    for (int k = 0; k < 3; k++) { nrst_w64(W + NRST_R_VB + 8 * k, x->vb[k]); nrst_w64(W + NRST_R_VB_DIRTY + 8 * k, x->vb_dirty[k]); }
    nrst_w64(W + NRST_R_IB, x->ib); nrst_w64(W + NRST_R_IB_FIRST, x->ib_first); W[NRST_R_PRIM] = x->prim; nrst_w32(W + NRST_R_NVN_PRIM, x->nvn_prim);
    W[NRST_R_REN_NEED] = x->ren_need; nrst_w64(W + NRST_R_REN_Q, x->ren_q); W[NRST_R_REN_OFF] = x->ren_off; memcpy(W + NRST_R_ALPHA, &x->alpha, 4);
    int bad = 0; *where = 0;
    for (uint32_t o = NRST_REGION; o < NRST_REGION_END; o++) if (R[o] != W[o]) { if (!bad++) *where = o; }
    const uint32_t cs = nrst_r32(R + NRST_R_CSFLAG), wcs = (s->stored & NRST_S_CSFLAG) ? x->csflag : pre_csflag;
    if (cs != wcs) { if (!bad++) *where = NRST_R_CSFLAG; }
    for (int i = 0; i < s->nstamps; i++) if (nrst_r64(NRST_PTR(s->stamp[i].a)) != s->stamp[i].v) { if (!bad++) *where = ~0u; }
    return bad;
}
// Two call lists the same: every field of every call, a stack object of the engine (NRST_LOCAL_*) standing for one pointer per packet.
static inline int nrst_same_op(const nrst_op *m, const nrst_op *g, uint64_t loc[3]) {
    if (m->fn != g->fn || m->a != g->a || m->q != g->q || m->i != g->i || m->j != g->j || memcmp(&m->f, &g->f, 4)) return 0;
    if (m->p == NRST_LOCAL_BLEND || m->p == NRST_LOCAL_COLOR) { if (!loc[m->p]) loc[m->p] = g->p; return loc[m->p] == g->p; }
    return m->p == g->p;
}

// ---- the code the model stands for: FNV-1a 64 over ranges of main (computed from main.raw by nr_state_oracle.py, which asserts these) ----
typedef struct { uint32_t off, end; uint64_t fnv; const char *name; } nrst_code;
static inline uint64_t nrst_fnv(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}
enum { NRST_CODE_N = 4 };
#define NRST_CODE_TABLE { \
    {0x93e5f0, 0x93e648, 0x83ccc0aa3d45932bull, "ExecuteRenderCommand dispatch"}, \
    {0x93e6ac, 0x93fc88, 0x514a6b8e1a8e55dcull, "opcode 21 (all state of a draw)"}, \
    {0x9283a0, 0x9284e0, 0xecc74eb40723d894ull, "SetShader"}, \
    {0x9284e0, 0x928ab0, 0x9848b5c2a392cbb2ull, "SetBlend"} }
// The jump table entry of opcode 21 (u16 at main + 0x1f50716 + 2 * 21, base 0x93e648): 0x93e6ac.
enum { NRST_JT = 0x1f50716, NRST_JT_BASE = 0x93e648, NRST_CODE21 = 0x93e6ac, NRST_PKT_LEN = 0x70, NRST_SPECIAL_BLEND = 0x2bbdb70, NRST_PRIM_TABLE = 0x1f58640 };
// The engine's NVN pointer slots (main +; filled by its loader 0x934f00), in NRST_F_* order (index fn - 1).
#define NRST_SLOTS { \
    {0x2bbfa68, "nvnCommandBufferBindProgram"}, {0x2bbfa50, "nvnCommandBufferBindDepthStencilState"}, \
    {0x2bbfc08, "nvnCommandBufferSetStencilMask"}, {0x2bbfc00, "nvnCommandBufferSetStencilValueMask"}, \
    {0x2bbfc10, "nvnCommandBufferSetStencilRef"}, {0x2bbfa48, "nvnCommandBufferBindPolygonState"}, \
    {0x2bbf718, "nvnBlendStateSetDefaults"}, {0x2bbfa28, "nvnCommandBufferBindBlendState"}, \
    {0x2bbf790, "nvnColorStateSetDefaults"}, {0x2bbf7a8, "nvnColorStateSetAlphaTest"}, \
    {0x2bbfa38, "nvnCommandBufferBindColorState"}, {0x2bbf720, "nvnBlendStateSetBlendTarget"}, \
    {0x2bbf798, "nvnColorStateSetBlendEnable"}, {0x2bbfa58, "nvnCommandBufferBindVertexAttribState"}, \
    {0x2bbfa60, "nvnCommandBufferBindVertexStreamState"}, {0x2bbfa70, "nvnCommandBufferBindVertexBuffer"}, \
    {0x2bbfcd0, "nvnCommandBufferSetRenderEnable"}, {0x2bbfcd8, "nvnCommandBufferSetRenderEnableConditional"}, \
    {0x2bbfc38, "nvnCommandBufferSetAlphaRef"} }
#endif
