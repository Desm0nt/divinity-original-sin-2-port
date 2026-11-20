// NR-31 host test: dm/gfx/nr/nr_seed.{h,c} (DM_NR_SEED, the portable pipeline warm-up seed).
// clang -std=c11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS [-DNRS_TEST_VK -I<ndk sysroot>/usr/include/vulkan]
//       dm/gfx/vulkan/engine/tests/nr_seed_test.c dm/gfx/nr/nr_seed.c dm/gfx/nr/nr_ubodyn.c -o nr_seed_test && ./nr_seed_test [list.bin ...]
// What it proves:
//  - parse / emit: every record (random, and every record of real lists given as arguments - e.g. the TL2 Mi 14 lists, same engine)
//    comes back byte for byte;
//  - the expander against an INDEPENDENT forward model of draw() / create_pass() / descriptors(): one random NVN draw state is turned
//    into the record each of 4 device policies would write (Turnip Mi 14, Mali, PowerVR, Xclipse-like: tile barrier, NOOUT, retype,
//    vertex fallbacks with conversion, D24 / D16 stand-ins, depth-bias x2, dynamic-UBO limit 8 / off, missing features); expanding
//    the source device's record to every destination must give that destination's record exactly, or a refusal of the kinds that are
//    provably not invertible (source NOOUT masks, ambiguous depth stand-ins, source conversions) - never a different record;
//  - fuzz: one word of a valid record changed at random: the expander never crashes and anything it accepts is a well-formed record
//    of the shape draw() writes; a mutation of a fixed field (pass ops, flags, dynamic states...) is refused;
//  - the seed file: header / id / damaged files; tools/dos2_pipe_seed.py's output (--check-seed) expands to the list it came from.
// Modes for tools/tests/dos2_pipe_seed_test.py: --write-list F N (a synthetic Turnip list) and --write-bundles DIR (bundle headers of the
// same shader universe), --check-seed SEED LIST (expand every seed record to its own policy, compare with LIST).
// [NR-31b] destinations with the NR-08 split (DM_NR_DESC_SPLIT: a layout picked at run time -> NSD_E_SPLIT, never a guess) and the NR-08b
// read-only rule (a dynamic block must be read by its stage), fused post keys ("F1A.<host>"), and --real-seed SEED LIST POLICY BDIR [BDIR2]:
// a seed built from a real phone list re-targeted to a policy, compared with the list's records of that policy (needs nr_ubodyn.c).
#include "../../../nr/nr_seed.h"
#include "../../../nr/nr_ubodyn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef NSD_TEST_VK
#include "vulkan_core.h"
#endif

static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; if (fails < 40) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static uint32_t rn(uint32_t n) { return (uint32_t)(rnd() % n); }

// ---------------- the shader universe ----------------
typedef struct { char key[17]; uint32_t stage; uint32_t nb; nsd_bind b[12]; uint8_t rd[12]; uint32_t nin, loc[8], cls[8]; uint32_t outputs, smask; } fshader;
enum { NVS = 14, NFS = 18, NGS = 2 };
static fshader U_vs[NVS], U_fs[NFS], U_gs[NGS], U_tc, U_te;
static void mk_key(char *k) { static const char h[] = "0123456789abcdef"; for (int i = 0; i < 16; i++) k[i] = h[rn(16)]; k[16] = 0; }
static void mk_binds(fshader *s, uint32_t base, int frag) {
    s->nb = 0; uint32_t bn = base;
    const uint32_t nubo = rn(frag ? 9 : 7);   // up to 8 / 6 uniform blocks: sets over 8 exist (Mali limit 8)
    for (uint32_t i = 0; i < nubo; i++) { static const uint32_t src[5] = {1, 2, 4, 5, 2}; s->rd[s->nb] = rn(6) != 0; s->b[s->nb++] = (nsd_bind){bn++, 6, 1, i == 0 ? 2 : src[rn(5)], i}; }
    if (frag) { const uint32_t nt = rn(3); for (uint32_t i = 0; i < nt; i++) { s->rd[s->nb] = 1; s->b[s->nb++] = (nsd_bind){bn++, 1, 1 + rn(2), 7, i}; } }
    if (rn(6) == 0) { s->rd[s->nb] = 1; s->b[s->nb++] = (nsd_bind){bn++, 7, 1, 3, 0}; }
}
static void universe(void) {
    for (int i = 0; i < NVS; i++) {
        fshader *s = &U_vs[i]; memset(s, 0, sizeof *s); mk_key(s->key); s->stage = 0; mk_binds(s, 0, 0);
        s->nin = 1 + rn(5); uint32_t used = 0;
        for (uint32_t k = 0; k < s->nin; k++) { uint32_t l; do l = rn(8); while (used >> l & 1); used |= 1u << l; s->loc[k] = l; s->cls[k] = rn(5) == 0 ? 1 + rn(2) : 0; }
    }
    for (int i = 0; i < NFS; i++) {
        fshader *s = &U_fs[i]; memset(s, 0, sizeof *s); mk_key(s->key); s->stage = 4; mk_binds(s, 32, 1);
        s->outputs = rn(5) == 0 ? 0 : 1 + rn(15); s->smask = rn(12) == 0;
    }
    for (int i = 0; i < NGS; i++) { fshader *s = &U_gs[i]; memset(s, 0, sizeof *s); mk_key(s->key); s->stage = 3; mk_binds(s, 64, 0); }
    memset(&U_tc, 0, sizeof U_tc); mk_key(U_tc.key); U_tc.stage = 1; mk_binds(&U_tc, 80, 0);
    memset(&U_te, 0, sizeof U_te); mk_key(U_te.key); U_te.stage = 2; mk_binds(&U_te, 96, 0);
}
static const fshader *find_shader(const char *base) {
    if (base[0] == 'F' && strchr(base, '.')) base = strchr(base, '.') + 1;   // a fused post stage: the host's bundle stands in for it here
    for (int i = 0; i < NVS; i++) if (!strcmp(U_vs[i].key, base)) return &U_vs[i];
    for (int i = 0; i < NFS; i++) if (!strcmp(U_fs[i].key, base)) return &U_fs[i];
    for (int i = 0; i < NGS; i++) if (!strcmp(U_gs[i].key, base)) return &U_gs[i];
    if (!strcmp(U_tc.key, base)) return &U_tc;
    if (!strcmp(U_te.key, base)) return &U_te;
    return NULL;
}

// ---------------- devices ----------------
static uint32_t vclass(uint32_t f) {   // xbg_vertex_format_class
    switch (f) {
    case 13: case 20: case 27: case 34: case 41: case 48: case 55: case 62: case 68: case 74: case 81: case 88: case 95: case 98: case 101: case 104: case 107: return 2;
    case 14: case 21: case 28: case 35: case 42: case 49: case 56: case 63: case 69: case 75: case 82: case 89: case 96: case 99: case 102: case 105: case 108: return 1;
    default: return 0;
    }
}
static uint32_t scaled_int(uint32_t f, uint32_t *kind) {   // xbg_vertex_scaled_int
    switch (f) {
    case 11: case 18: case 25: case 32: case 39: case 46: case 53: case 60: case 66: case 72: case 79: case 86: case 93: *kind = 2; return f + 2;
    case 12: case 19: case 26: case 33: case 40: case 47: case 54: case 61: case 67: case 73: case 80: case 87: case 94: *kind = 1; return f + 2;
    default: *kind = 0; return 0;
    }
}
static uint32_t widen(uint32_t f) {   // xbg_vertex_widen
    switch (f) {
    case 23: return 37; case 24: return 38; case 27: return 41; case 28: return 42; case 84: return 91; case 85: return 92; case 88: return 95; case 89: return 96;
    case 90: return 97; case 104: return 107; case 105: return 108; case 106: return 109; default: return 0;
    }
}
typedef struct { const char *name; nsd_policy pol; int no_rgb16, no_scaled, no_rgb8int; uint32_t ubo_lim; } tdev;
enum { NDEV = 6, NSRC = 4 };   // 0..3 sources and destinations; 4, 5 destinations only (layout rules a seed's source never has)
static tdev DEV[NDEV];
static int vsup(const tdev *d, uint32_t f) {
    if (d->no_rgb16 && f >= 84 && f <= 90) return 0;
    if (d->no_scaled) { uint32_t k; if (scaled_int(f, &k)) return 0; }
    if (d->no_rgb8int && (f == 27 || f == 28)) return 0;
    return f != 0;
}
// vertex_format_fallback + draw()'s checks; 0 = draw() stops
static uint32_t vfetch(const tdev *d, uint32_t f, uint32_t *conv) {
    *conv = 0;
    if (vsup(d, f)) return f;
    uint32_t kind; const uint32_t integer = scaled_int(f, &kind);
    if (integer) { uint32_t t = integer; if (!vsup(d, t)) t = widen(t); if (!vsup(d, t)) return 0; *conv = kind; return t; }
    const uint32_t w = widen(f);
    return w && vsup(d, w) ? w : 0;
}
static int cb_shader(void *u, const char *base, nsd_shader *o, unsigned want) {
    (void)u; (void)want;
    const fshader *s = find_shader(base);
    if (!s) return -1;
    o->stage = s->stage; o->nb = s->nb; memcpy(o->b, s->b, sizeof(nsd_bind) * s->nb);
    o->nin = s->nin; memcpy(o->in_loc, s->loc, 4 * s->nin); o->classes = 1; memcpy(o->in_class, s->cls, 4 * s->nin);
    o->facts = 1; o->outputs = s->outputs; o->sample_mask = s->smask;
    o->reads = 1; memcpy(o->read, s->rd, s->nb);
    return 0;
}
static uint32_t cb_vtx(void *u, uint32_t f, uint32_t *conv) { return vfetch((const tdev *)u, f, conv); }
static int cb_att(void *u, uint32_t f, int depth, int blend) {
    const tdev *d = (const tdev *)u; (void)blend;
    if (depth) return (f == 125 || f == 126 || f == 129) ? !(d->pol.x8d24_as_d32 && f == 125) && !(d->pol.d24s8_as_d32s8 && f == 129) : (f == 124 || f == 130 || f == 127);
    return f == 37 || f == 43 || f == 97 || f == 76 || f == 100 || f == 64;
}
static nsd_dev mkdev(tdev *d) {
    nsd_dev v; memset(&v, 0, sizeof v);
    v.u = d; v.shader = cb_shader; v.vtx_format = cb_vtx; v.vtx_class = vclass; v.att_ok = cb_att;
    v.max_color = 8; v.max_vi_bind = 16; v.max_vi_attr = 16; v.max_vi_offset = 2047; v.max_vi_stride = 2048; v.color_samples = 0x7f; v.depth_samples = 0x7f;
    v.set_ubo_dyn = d->pol.ubo_dyn_limit; v.set_ssbo_dyn = 8;
    return v;
}
static void devices(void) {
    nsd_policy t = {0}; t.ubo_dynamic = 1; t.ubo_dyn_limit = 16; t.dyn_partial = 1; t.alpha_test = 1; t.features = NSD_F_ALL;
    DEV[0] = (tdev){"turnip", t, 0, 0, 0, 16};
    nsd_policy m = t; m.tile_barrier = 1; m.noout = 1; m.retype = 1; m.ubo_dyn_limit = 8; m.features = NSD_F_ALL & ~(NSD_F_DBOUNDS | NSD_F_LOGICOP);
    DEV[1] = (tdev){"mali", m, 1, 0, 0, 8};
    nsd_policy p = m; p.ubo_dynamic = 0; p.depth16_as_d24 = 1; p.features = NSD_F_ALL & ~(NSD_F_NONSOLID | NSD_F_DCLAMP | NSD_F_DBOUNDS | NSD_F_BCLAMP);
    DEV[2] = (tdev){"powervr", p, 0, 0, 0, 8};
    nsd_policy x = t; x.noout = 1; x.retype = 1; x.d24s8_as_d32s8 = 1; x.x8d24_as_d32 = 1; x.bias_x2 = 1; x.ubo_dyn_limit = 8;
    DEV[3] = (tdev){"xclipse", x, 0, 1, 1, 8};
    nsd_policy ms = m; ms.split = 1;                                               // Mali with NR-08 DM_NR_DESC=1 (DOS2 default): split on
    DEV[4] = (tdev){"mali+split", ms, 1, 0, 0, 8};
    nsd_policy pr = p; pr.ubo_dynamic = 1; pr.ubo_ro = 1; pr.split = 1;            // PowerVR with NR-08b DM_NR_UBODYN=1 (default there) + NR-08
    DEV[5] = (tdev){"powervr+ro", pr, 0, 0, 0, 8};
    for (int d = 0; d < NDEV; d++) for (uint32_t f = 1; f < 256; f++) if (!vsup(&DEV[d], f)) DEV[d].pol.vtx_unsup[f >> 5] |= 1u << (f & 31);
}

// ---------------- the forward model: one NVN draw state -> the record a device writes ----------------
typedef struct {
    int vs, fs, gs, tess;                     // shader indices (fs -1 = none)
    uint32_t ncol, col_fmt[8], col_on[8];     // NVN render targets (formats already the Vulkan table's)
    uint32_t depth_fmt;                       // 0 = none; 124 D16, 125 X8_D24, 126 D32, 129 D24S8, 130 D32S8 (format() table, before stand-ins)
    uint32_t samples;
    uint32_t attr_fmt[8], attr_stream[8], attr_off[8];   // per VS input (format 0 = constant)
    uint32_t stride[16], divisor[16];
    uint32_t topology, restart;
    uint32_t poly, cull, front, bias_en; float bias[3]; uint32_t dclamp, discard, a2c;
    uint32_t dt, dw, dcmp, dbounds, st_en; nsd_stencil sf, sb; uint32_t dbmin, dbmax;
    nsd_blend blend[8]; uint32_t logic_op; uint32_t bconst[4];
    uint32_t alpha_func, alpha_ref;
    uint32_t patch;
} dstate;
static const uint32_t VFMT[] = {37, 38, 39, 40, 41, 42, 79, 80, 81, 83, 84, 90, 103, 106, 109, 27, 25};
static void rand_state(dstate *s) {
    memset(s, 0, sizeof *s);
    s->vs = (int)rn(NVS); s->fs = rn(10) == 0 ? -1 : (int)rn(NFS); s->gs = rn(15) == 0 ? (int)rn(NGS) : -1; s->tess = rn(20) == 0;
    static const uint32_t CF[] = {37, 43, 97, 76, 100, 64};
    s->ncol = rn(5);
    for (uint32_t i = 0; i < s->ncol; i++) { s->col_on[i] = rn(6) != 0; s->col_fmt[i] = CF[rn(6)]; }
    static const uint32_t DF[] = {0, 124, 125, 126, 129, 130, 129, 124};
    s->depth_fmt = DF[rn(8)];
    if (s->fs < 0 && !s->depth_fmt) s->depth_fmt = 129;
    int any = s->depth_fmt != 0; for (uint32_t i = 0; i < s->ncol; i++) any |= (int)s->col_on[i];
    if (!any) { s->ncol = 1; s->col_on[0] = 1; s->col_fmt[0] = 37; }
    s->samples = rn(8) == 0 ? 4 : 1;
    const fshader *v = &U_vs[s->vs];
    for (uint32_t k = 0; k < v->nin; k++) {
        if (rn(6) == 0) { s->attr_fmt[k] = 0; continue; }
        s->attr_fmt[k] = VFMT[rn(sizeof VFMT / 4)]; s->attr_stream[k] = rn(3); s->attr_off[k] = 4 * rn(8);
    }
    for (int b = 0; b < 3; b++) { s->stride[b] = 4 * (1 + rn(16)); s->divisor[b] = rn(8) == 0; }
    s->topology = s->tess ? 10 : rn(6); s->restart = rn(4) == 0;
    s->poly = rn(10) == 0 ? rn(3) : 2; s->cull = rn(4); s->front = rn(2); s->bias_en = rn(3) == 0;
    s->bias[0] = (float)rn(5) * 0.5f; s->bias[1] = (float)(rn(200)) - 50.0f; s->bias[2] = rn(4) == 0 ? 0.25f : 0.0f;
    s->dclamp = rn(5) == 0; s->discard = rn(30) == 0; s->a2c = s->fs >= 0 && rn(10) == 0;
    if (s->depth_fmt) { s->dt = rn(3) != 0; s->dw = rn(2); s->dcmp = 1 + rn(8); s->dbounds = rn(6) == 0; s->st_en = rn(5) == 0; }
    nsd_stencil st = {rn(8), rn(8), rn(8), rn(8), 0xff, rn(256), rn(256)}; s->sf = st; s->sb = st; s->sb.ref = rn(256);
    s->dbmin = 0; s->dbmax = 0x3f800000;
    for (uint32_t i = 0; i < s->ncol; i++) {
        nsd_blend *b = &s->blend[i]; b->enable = rn(3) == 0; b->src_c = rn(19); b->dst_c = rn(19); b->op_c = rn(5); b->src_a = rn(19); b->dst_a = rn(19); b->op_a = rn(5);
        b->mask = rn(4) == 0 ? rn(16) : 15;
        if (s->fs < 0) b->mask = 0;   // xbg_graphics_stages: no fragment stage writes no colour
    }
    s->logic_op = rn(10) == 0 ? rn(16) : 3;
    s->bconst[0] = rn(2) ? 0x3f800000 : 0;
    s->alpha_func = rn(4) == 0 ? 1 + rn(7) : 8; s->alpha_ref = rn(2) ? 0x3f000000 : 0x3e800000;
    if (s->fs < 0) { s->a2c = 0; s->alpha_func = 8; }
    s->patch = 3;
}
typedef struct { uint32_t nk; char key[NSD_STAGES][NSD_KEY_MAX + 1]; uint32_t klen[NSD_STAGES]; uint32_t w[NSD_MAX_WORDS]; size_t nw; } trec;
static nsd_pipe MP;
// draw() under one device; 0 = the draw is drawn (rec filled), -1 = draw() would stop / skip it on this device, -2 = its layout is NR-08's
// run-time split (no record to compare: the expander must refuse it)
static int model(const dstate *s, const tdev *d, trec *out) {
    nsd_pipe *p = &MP; memset(p, 0, sizeof *p);
    const nsd_policy *pol = &d->pol;
    const fshader *sh[5]; uint32_t ns = 0;
    sh[ns++] = &U_vs[s->vs];
    if (s->tess) { sh[ns++] = &U_tc; sh[ns++] = &U_te; }
    if (s->gs >= 0) sh[ns++] = &U_gs[s->gs];
    if (s->fs >= 0) sh[ns++] = &U_fs[s->fs];
    // descriptors(): dyn_mode_for, UBO_DYN, the key loop
    uint32_t all = 0, vary = 0;
    for (uint32_t i = 0; i < ns; i++) for (uint32_t k = 0; k < sh[i]->nb; k++) if (sh[i]->b[k].type == 6) { all += sh[i]->b[k].count; if (sh[i]->b[k].source == 2 || sh[i]->b[k].source == 5 || sh[i]->b[k].source == 6) vary += sh[i]->b[k].count; }
    const uint32_t lim = pol->ubo_dyn_limit;
    if (pol->ubo_dynamic && pol->split && vary > lim && !(all && all <= lim)) {   // xbg_nr_desc.inc split_pick
        uint32_t n = 0; int ok = lim > 0;
        for (uint32_t i = 0; i < ns && ok; i++) for (uint32_t k = 0; k < sh[i]->nb && ok; k++) {
            const nsd_bind *b = &sh[i]->b[k];
            if (b->type != 6) continue;
            if (b->source == 5 || b->source == 6) { ok = 0; break; }
            if (b->source != 2) continue;
            if (b->count != 1 || b->slot >= 16 || n >= 16) { ok = 0; break; }
            n++;
        }
        if (ok && n > lim) return -2;   // dyn_mode_for 3: the dynamic blocks are picked by run-time change counts
    }
    unsigned mode = !pol->ubo_dynamic ? 0 : pol->ubo_ro ? (vary && vary <= lim && pol->dyn_partial ? 2 : 0)
                  : all && all <= lim ? 1 : vary && vary <= lim && pol->dyn_partial ? 2 : 0;
    for (uint32_t i = 0; i < ns; i++) for (uint32_t k = 0; k < sh[i]->nb; k++) {
        const nsd_bind *b = &sh[i]->b[k];
        const int dyn = b->type == 6 && (mode == 1 || (mode == 2 && (b->source == 2 || b->source == 5 || b->source == 6))) && !(pol->ubo_ro && !sh[i]->rd[k]);
        p->lb[p->nlb++] = (nsd_lb){b->binding, dyn ? 8u : b->type, b->count, 1u << sh[i]->stage};
    }
    p->sets = 1;
    // create_pass(): colour slots in order, then depth; formats with this device's stand-ins
    for (uint32_t i = 0; i < s->ncol; i++) {
        p->col[i].layout = 1;
        if (!s->col_on[i]) { p->col[i].att = 0xffffffffu; continue; }
        p->col[i].att = p->natt; p->att[p->natt++] = (nsd_att){0, s->col_fmt[i], s->samples, 0, 0, 0, 0, 1, 1};
    }
    p->ncol = s->ncol;
    if (s->depth_fmt) {
        uint32_t f = s->depth_fmt;
        if (f == 129 && pol->d24s8_as_d32s8) f = 130;
        if (f == 125 && pol->x8d24_as_d32) f = 126;
        if (f == 124 && pol->depth16_as_d24 && !pol->x8d24_as_d32) f = 125;
        p->has_depth = 1; p->depth = (nsd_ref){p->natt, 1}; p->att[p->natt++] = (nsd_att){0, f, s->samples, 0, 0, 0, 0, 1, 1};
    }
    p->ndep = 2;
    p->dep[0] = (nsd_dep){0xffffffffu, 0, 0x10000, 0x8000, 0x10000, 0x780, 0};
    p->dep[1] = (nsd_dep){0, 0xffffffffu, 0x8000, 0x10000, 0x500, 0x18000, 0};
    if (pol->tile_barrier) { p->dep[0].dst_stage = 0x780; p->dep[1].dst_stage = 0x1780; }
    // vertex input
    const fshader *v = &U_vs[s->vs]; uint32_t used = 0, conv = 0, code = 0; int zero = -1;
    uint32_t bstride[16] = {0}, brate[16] = {0};
    for (uint32_t k = 0; k < v->nin; k++) {
        const uint32_t loc = v->loc[k];
        if (!s->attr_fmt[k]) {
            if (zero < 0) { for (int b = 15; b >= 0; b--) { int taken = 0; for (uint32_t j = 0; j < v->nin; j++) if (s->attr_fmt[j] && s->attr_stream[j] == (uint32_t)b) taken = 1; if (!taken) { zero = b; break; } } }
            const uint32_t f = v->cls[k] == 0 ? 109 : v->cls[k] == 1 ? 108 : 107;
            p->va[p->nva][0] = loc; p->va[p->nva][1] = (uint32_t)zero; p->va[p->nva][2] = f; p->va[p->nva][3] = 0; p->nva++;
            used |= 1u << zero; bstride[zero] = 0; brate[zero] = 0; continue;
        }
        const uint32_t st = s->attr_stream[k];
        if (s->divisor[st] > 1) return -1;
        uint32_t kind = 0; const uint32_t f = vfetch(d, s->attr_fmt[k], &kind);
        if (!f) return -1;
        conv |= kind << (2 * loc);
        p->va[p->nva][0] = loc; p->va[p->nva][1] = st; p->va[p->nva][2] = f; p->va[p->nva][3] = s->attr_off[k]; p->nva++;
        used |= 1u << st; bstride[st] = s->stride[st]; brate[st] = s->divisor[st] ? 1 : 0;
    }
    for (uint32_t b = 0; b < 16; b++) if (used >> b & 1) { p->vb[p->nvb][0] = b; p->vb[p->nvb][1] = bstride[b]; p->vb[p->nvb][2] = brate[b]; p->nvb++; }
    if (pol->retype) for (uint32_t i = 0; i < p->nva; i++) for (uint32_t j = 0; j < v->nin; j++) if (v->loc[j] == p->va[i][0]) {
        const uint32_t loc = p->va[i][0];
        if ((conv >> (2 * loc)) & 3) break;
        const uint32_t want = vclass(p->va[i][2]);
        if (want != v->cls[j]) code |= (want + 1) << (2 * loc);
        break;
    }
    p->topology = s->topology; p->restart = s->restart && (s->topology == 2 || s->topology == 4 || s->topology == 5);
    p->has_tess = (uint32_t)s->tess; p->patch = s->patch;
    p->nvp = p->nsc = 1;
    p->poly = s->poly == 2 ? 0 : s->poly == 1 ? 1 : 2; if (p->poly && !(pol->features & NSD_F_NONSOLID)) p->poly = 0;
    p->cull = s->cull; p->front = s->front; p->line_width = 0x3f800000; p->bias_en = s->bias_en;
    { float c = s->bias[1] * (s->depth_fmt == 124 && pol->depth16_as_d24 && !pol->x8d24_as_d32 ? 256.0f : 1.0f) * (pol->bias_x2 ? 2.0f : 1.0f); memcpy(&p->bias_const, &c, 4); }
    if (s->dclamp && (pol->features & NSD_F_DCLAMP)) p->clamp = 1;
    p->discard = s->discard;
    memcpy(&p->bias_slope, &s->bias[0], 4);
    { float bc = s->bias[2]; if (bc != 0.0f && !(pol->features & NSD_F_BCLAMP)) bc = 0; memcpy(&p->bias_clamp, &bc, 4); }
    p->samples = s->samples; p->a2c = s->a2c;
    p->dt = s->dt; p->dw = s->dw; p->dcmp = s->dcmp ? s->dcmp - 1 : 0; p->st_en = s->st_en; p->sf = s->sf; p->sb = s->sb; p->db_min = s->dbmin; p->db_max = s->dbmax;
    p->dbounds = s->dbounds && (pol->features & NSD_F_DBOUNDS);
    p->nblend = s->ncol;
    for (uint32_t i = 0; i < s->ncol; i++) p->blend[i] = s->blend[i];
    if (pol->noout && p->nblend) { const fshader *f = s->fs >= 0 ? &U_fs[s->fs] : NULL; if (!f || (!f->outputs && !f->smask)) for (uint32_t i = 0; i < p->nblend; i++) p->blend[i].mask = 0; }
    memcpy(p->bconst, s->bconst, 16);
    if (s->logic_op != 3 && (pol->features & NSD_F_LOGICOP)) { p->logic_en = 1; p->logic_op = s->logic_op; }
    p->ndyn = 2; p->dyn[0] = 0; p->dyn[1] = 1;
    // stages + keys (vertex variant, alpha variant)
    out->nk = ns;
    for (uint32_t i = 0; i < ns; i++) {
        char *k = out->key[i];
        if (i == 0 && (code || conv)) { if (conv) snprintf(k, NSD_KEY_MAX + 1, "%s#vi%08xc%08x", sh[i]->key, code, conv); else snprintf(k, NSD_KEY_MAX + 1, "%s#vi%08x", sh[i]->key, code); }
        else if (sh[i]->stage == 4 && s->alpha_func != 8 && pol->alpha_test) snprintf(k, NSD_KEY_MAX + 1, "%s#at%u_%08x", sh[i]->key, s->alpha_func, s->alpha_ref);
        else snprintf(k, NSD_KEY_MAX + 1, "%s", sh[i]->key);
        out->klen[i] = (uint32_t)strlen(k);
        p->st[i].stage = 1u << sh[i]->stage; p->st[i].id = nsd_fnv(k, out->klen[i]);
    }
    p->nst = ns;
    out->nw = nsd_emit(p, out->w, NSD_MAX_WORDS);
    return out->nw ? 0 : -1;
}

static nsd_work WK; static nsd_out OUT; static nsd_pipe PP;
static int cb_noreads(void *u, const char *base, nsd_shader *o, unsigned want) { const int r = cb_shader(u, base, o, want); o->reads = 0; return r; }
static int expand_rec(const trec *r, const nsd_policy *src, tdev *d) {
    const char *keys[NSD_STAGES]; for (uint32_t i = 0; i < r->nk; i++) keys[i] = r->key[i];
    nsd_dev v = mkdev(d);
    return nsd_expand(src, &d->pol, &v, keys, r->klen, r->nk, r->w, r->nw, &WK, &OUT);
}
static int same_out(const trec *r) {
    if (OUT.nk != r->nk || OUT.nw != r->nw || memcmp(OUT.words, r->w, r->nw * 4)) return 0;
    for (uint32_t i = 0; i < r->nk; i++) if (OUT.klen[i] != r->klen[i] || memcmp(OUT.key[i], r->key[i], r->klen[i])) return 0;
    return 1;
}
static void diff_print(const trec *want) {
    for (uint32_t i = 0; i < want->nk && i < OUT.nk; i++) if (strcmp(OUT.key[i], want->key[i])) printf("   key %u: got %s want %s\n", i, OUT.key[i], want->key[i]);
    const size_t n = OUT.nw < want->nw ? OUT.nw : want->nw;
    for (size_t i = 0; i < n; i++) if (OUT.words[i] != want->w[i]) { printf("   word %zu (%s): got %#x want %#x (lengths %zu / %zu)\n", i, nsd_field_at(want->w, want->nw, i), OUT.words[i], want->w[i], OUT.nw, want->nw); return; }
    if (OUT.nw != want->nw) printf("   lengths %zu / %zu\n", OUT.nw, want->nw);
}

// ---------------- tests ----------------
static void unit_keys(void) {
    nsd_key k;
    CHECK(nsd_key_parse("69e0b8f6c34f058f", 16, &k) == 0 && k.tag == NSD_TAG_NONE && !strcmp(k.base, "69e0b8f6c34f058f"));
    CHECK(nsd_key_parse("69e0b8f6c34f058f#at3_3f000000", 29, &k) == 0 && k.tag == NSD_TAG_AT && k.at_func == 3 && k.at_bits == 0x3f000000);
    CHECK(nsd_key_parse("69e0b8f6c34f058f#vi00000012", 27, &k) == 0 && k.tag == NSD_TAG_VI && k.vi_code == 0x12 && !k.vi_conv);
    CHECK(nsd_key_parse("69e0b8f6c34f058f#vi00000000c00000008", 36, &k) == 0 && k.vi_conv == 8);
    CHECK(nsd_key_parse("69e0b8f6c34f058f#vi12", 21, &k) == -1);         // not alpha_module / vtx_retype_module spelling
    CHECK(nsd_key_parse("69e0b8f6c34f058f#at03_3f000000", 30, &k) == -1);
    CHECK(nsd_key_parse("69e0b8f6c34f058f#xx", 19, &k) == -1);
    CHECK(nsd_key_parse("69e0b8f6c34f05g8", 16, &k) == -1);
    CHECK(nsd_key_parse("69e0b8f6c34f058", 15, &k) == -1);
    // [NR-31b] fused post stages (phone list 17.11: records with "F1.91eb2d97a304709a" / "F1A.91eb2d97a304709a")
    CHECK(nsd_key_parse("F1A.91eb2d97a304709a", 20, &k) == 0 && k.tag == NSD_TAG_FUSED && !strcmp(k.base, "F1A.91eb2d97a304709a"));
    CHECK(nsd_key_parse("F1.91eb2d97a304709a", 19, &k) == 0 && k.tag == NSD_TAG_FUSED && !strcmp(k.base, "F1.91eb2d97a304709a"));
    CHECK(nsd_key_parse("F1AS.91eb2d97a304709a", 21, &k) == 0 && nsd_key_parse("F2.98186c22a7209861", 19, &k) == 0);
    CHECK(nsd_key_parse("F3.98186c22a7209861", 19, &k) == -1);        // not a fused_name (F1 / F2 only)
    CHECK(nsd_key_parse("F1ABC.91eb2d97a304709a", 22, &k) == -1);     // the '.' within 2..4
    CHECK(nsd_key_parse("F1A.91EB2D97A304709A", 20, &k) == -1);       // the host's translated key: lower-case hex
    CHECK(nsd_key_parse("F1A.91eb2d97a304709", 19, &k) == -1 && nsd_key_parse("F1A.91eb2d97a304709a#at3_3f000000", 33, &k) == -1);
    CHECK(nsd_key_parse("F1a.91eb2d97a304709a", 20, &k) == -1 && nsd_key_parse("F191eb2d97a304709a00", 20, &k) == -1);
    CHECK(nsd_fnv("a", 1) == (((1469598103934665603ull ^ 'a') * 1099511628211ull) | (1ull << 63)));   // warm::fnv's constants
    for (int r = 0; r < NSD_E_COUNT; r++) CHECK(strcmp(nsd_reason_name(r), "?") != 0);
#ifdef NSD_TEST_VK
    CHECK(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC == 8 && VK_DESCRIPTOR_TYPE_STORAGE_BUFFER == 7 && VK_IMAGE_LAYOUT_GENERAL == 1);
    CHECK(VK_FORMAT_D16_UNORM == 124 && VK_FORMAT_X8_D24_UNORM_PACK32 == 125 && VK_FORMAT_D32_SFLOAT == 126 && VK_FORMAT_D24_UNORM_S8_UINT == 129 && VK_FORMAT_D32_SFLOAT_S8_UINT == 130);
    CHECK(VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT == 0x8000 && VK_PIPELINE_STAGE_ALL_COMMANDS_BIT == 0x10000 && VK_PIPELINE_STAGE_TRANSFER_BIT == 0x1000);
    CHECK((VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT) == 0x780);
    CHECK((VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) == 0x780);
    CHECK((VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT) == 0x18000 && VK_ACCESS_MEMORY_WRITE_BIT == 0x10000 && VK_SUBPASS_EXTERNAL == 0xffffffffu && VK_ATTACHMENT_UNUSED == 0xffffffffu);
    CHECK(VK_FORMAT_R32G32B32A32_SFLOAT == 109 && VK_DYNAMIC_STATE_VIEWPORT == 0 && VK_DYNAMIC_STATE_SCISSOR == 1 && VK_SHADER_STAGE_FRAGMENT_BIT == 0x10);
    printf("vulkan_core.h: the expander's numbers agree\n");
#endif
}

// One state, every (source, destination) pair: equal to the destination's own record, or a refusal of an allowed kind.
static uint64_t pairs = 0, equal = 0, refused[NSD_E_COUNT], wrong = 0, undrawable = 0, lossy = 0, from_full = 0, from_full_equal = 0, split_states = 0;
// A field a source without the optional feature recorded as "off": the game's value is lost there (best effort: kept as recorded).
static int lossy_field(const char *f, const nsd_policy *s, const nsd_policy *d) {
    const uint32_t lost = d->features & ~s->features;
    return (!strcmp(f, "ds.depth_bounds") && (lost & NSD_F_DBOUNDS)) || (!strcmp(f, "rs.polygon_mode") && (lost & NSD_F_NONSOLID)) ||
           (!strcmp(f, "rs.depth_clamp") && (lost & NSD_F_DCLAMP)) || (!strcmp(f, "rs.depth_bias_clamp") && (lost & NSD_F_BCLAMP)) ||
           ((!strcmp(f, "cb.logic_op_enable") || !strcmp(f, "cb.logic_op")) && (lost & NSD_F_LOGICOP));
}
// every word where the expansion differs from the destination's own record is a lossy field (keys equal)
static int only_lossy(const trec *want, const nsd_policy *s, const nsd_policy *d) {
    if (OUT.nk != want->nk || OUT.nw != want->nw) return 0;
    for (uint32_t i = 0; i < want->nk; i++) if (OUT.klen[i] != want->klen[i] || memcmp(OUT.key[i], want->key[i], want->klen[i])) return 0;
    for (size_t i = 0; i < want->nw; i++) if (OUT.words[i] != want->w[i] && !lossy_field(nsd_field_at(want->w, want->nw, i), s, d)) return 0;
    return 1;
}
static void check_pair(const dstate *s, int a, int b) {
    static trec ra, rb;
    const int mb = model(s, &DEV[b], &rb);
    if (model(s, &DEV[a], &ra) || mb == -1) { ++undrawable; return; }
    ++pairs;
    if (mb == -2) {   // the destination's layout is picked at run time: refused (split), or earlier for an allowed reason - never built
        ++split_states;
        const int e = expand_rec(&ra, &DEV[a].pol, &DEV[b]);
        ++refused[e];
        const int allowed = e == NSD_E_SPLIT || e == NSD_E_VARIANT || e == NSD_E_VTX_AMBIG;
        if (!allowed) { ++wrong; if (wrong <= 6) printf("WRONG %s -> %s: a split layout gave %s (%s)\n", DEV[a].name, DEV[b].name, nsd_reason_name(e), OUT.field); }
        return;
    }
    const int full = DEV[a].pol.features == NSD_F_ALL && !DEV[a].pol.noout && !DEV[a].pol.d24s8_as_d32s8 && !DEV[a].pol.x8d24_as_d32 && !DEV[a].pol.depth16_as_d24;
    from_full += full;
    const int e = expand_rec(&ra, &DEV[a].pol, &DEV[b]);
    if (e == NSD_OK) {
        if (same_out(&rb)) { ++equal; from_full_equal += full; return; }
        if (only_lossy(&rb, &DEV[a].pol, &DEV[b].pol)) { ++lossy; return; }
        ++wrong;
        if (wrong <= 6) { printf("WRONG %s -> %s:\n", DEV[a].name, DEV[b].name); diff_print(&rb); }
        return;
    }
    ++refused[e];
    // the refusals that are allowed: what the source device's record provably does not tell
    int allowed = 0;
    if (e == NSD_E_NOOUT) allowed = DEV[a].pol.noout && !DEV[b].pol.noout;
    if (e == NSD_E_DEPTH_AMBIG) allowed = DEV[a].pol.depth16_as_d24 || DEV[a].pol.x8d24_as_d32 || DEV[a].pol.d24s8_as_d32s8;
    if (e == NSD_E_VARIANT) allowed = strstr(ra.key[0], "#vi") != NULL && (ra.klen[0] == 36 || !DEV[a].pol.retype);
    if (e == NSD_E_VTX_AMBIG) { allowed = 0; for (uint32_t g = 1; g < 256; g++) if ((DEV[a].pol.vtx_unsup[g >> 5] >> (g & 31) & 1) && widen(g)) allowed = 1; }
    if (e == NSD_E_SPLIT) allowed = 0;   // only where the destination's own model says split (above)
    if (!allowed) { ++wrong; if (wrong <= 6) printf("WRONG refusal %s -> %s: %s (%s)\n", DEV[a].name, DEV[b].name, nsd_reason_name(e), OUT.field); }
}
static void prop_model(void) {
    dstate s;
    for (int i = 0; i < 30000; i++) {
        rand_state(&s);
        for (int a = 0; a < NSRC; a++) for (int b = 0; b < NDEV; b++) check_pair(&s, a, b);
    }
    printf("model: %llu (source, destination) pairs (%llu states x %d, %llu not drawable on one of them): %llu equal byte for byte, %llu differ only in fields a "
           "source without the feature cannot tell (best effort), %llu wrong; NR-08 split layouts on the destination %llu (all refused); from a full source "
           "(Turnip, Xclipse-like features): %llu of %llu equal; refused:",
           (unsigned long long)pairs, 30000ull, NSRC * NDEV, (unsigned long long)undrawable, (unsigned long long)equal, (unsigned long long)lossy, (unsigned long long)wrong,
           (unsigned long long)split_states, (unsigned long long)from_full_equal, (unsigned long long)from_full);
    for (int r = 1; r < NSD_E_COUNT; r++) if (refused[r]) printf(" %s %llu", nsd_reason_name(r), (unsigned long long)refused[r]);
    printf("\n");
    CHECK(wrong == 0);
    CHECK(from_full > 0 && from_full_equal == from_full);   // the Mi 14 (Turnip) as the source: every destination record exactly
    CHECK(split_states > 0 && refused[NSD_E_SPLIT] > 0);
    // the Mi 14 seed on every family: how much of it expands
    for (int b = 0; b < NDEV; b++) {
        uint64_t ok = 0, n = 0, split = 0; trec r, rd;
        for (int i = 0; i < 5000; i++) { rand_state(&s); if (model(&s, &DEV[0], &r)) continue; ++n; const int e = expand_rec(&r, &DEV[0].pol, &DEV[b]); if (e == NSD_OK) ++ok;
            if (e == NSD_E_SPLIT) { ++split; CHECK(model(&s, &DEV[b], &rd) == -2); } }
        printf("  turnip seed -> %-10s %llu of %llu records expand (%.1f %%), NR-08 split refused %llu\n", DEV[b].name, (unsigned long long)ok, (unsigned long long)n, 100.0 * ok / (n ? n : 1),
               (unsigned long long)split);
        CHECK(ok == n || b == 3 || b == 1 || ((b == 4 || b == 5) && ok + split <= n && split));   // Xclipse / Mali: vertex formats it cannot fetch; split
        if (b == 5) CHECK(ok + split == n);
    }
}

static void prop_roundtrip(void) {
    dstate s; trec r; uint64_t n = 0;
    for (int i = 0; i < 20000; i++) {
        rand_state(&s);
        if (model(&s, &DEV[rn(4)], &r)) continue;
        CHECK(nsd_parse(r.w, r.nw, &PP) == NSD_OK);
        uint32_t w2[NSD_MAX_WORDS]; const size_t m = nsd_emit(&PP, w2, NSD_MAX_WORDS);
        CHECK(m == r.nw && !memcmp(w2, r.w, m * 4));
        const char *f = NULL; CHECK(nsd_shape(&PP, r.nk, &f) == NSD_OK);
        for (size_t t = 0; t < r.nw; t++) CHECK(nsd_parse(r.w, t, &PP) != NSD_OK);   // every truncation refused
        ++n;
        if (i > 200) break;
    }
    printf("round trip: %llu records parse / emit byte for byte, every truncation refused\n", (unsigned long long)n);
}

// fuzz: change one word; never a crash; an accepted record re-parses with the shape draw() writes
static void prop_fuzz(void) {
    dstate s; trec r; uint64_t tried = 0, accepted = 0, fixed_hits = 0, fixed_refused = 0;
    static const char *const fixed[] = {"pass.att.ops", "pass.att.layout", "pass.flags", "ci.flags", "dy.state", "dy.states", "vp.static_viewports", "rs.line_width",
                                        "ms.sample_shading", "ms.alpha_to_one", "layout.sets", "layout.push", "stage.flags", "pass.subpass.flags", "pass.dep.flags",
                                        "pass.dep.src_stage", "pass.dep.dst_access", "pass.att.load", "pass.att.store", "pass.att.initial_layout", "pass.att.final_layout",
                                        "pass.att.stencil_load", "pass.att.stencil_store", "pass.att.flags", "vi.flags", "ia.flags", "rs.flags", "ms.flags", "ds.flags", "cb.flags"};
    for (int i = 0; i < 40000; i++) {
        rand_state(&s);
        if (model(&s, &DEV[0], &r)) continue;
        const size_t at = rn((uint32_t)r.nw); const uint32_t old = r.w[at];
        const char *field = nsd_field_at(r.w, r.nw, at);
        r.w[at] = rn(3) == 0 ? old ^ (1u << rn(32)) : rn(3) == 0 ? old + 1 : (uint32_t)rnd();
        ++tried;
        const int e = expand_rec(&r, &DEV[0].pol, &DEV[1 + rn(3)]);
        int is_fixed = 0; for (size_t k = 0; k < sizeof fixed / sizeof *fixed; k++) if (!strcmp(field, fixed[k])) is_fixed = 1;
        if (is_fixed) { ++fixed_hits; if (e != NSD_OK) ++fixed_refused; }
        if (e == NSD_OK) {
            ++accepted;
            CHECK(nsd_parse(OUT.words, OUT.nw, &PP) == NSD_OK);
            const char *f = NULL; CHECK(nsd_shape(&PP, OUT.nk, &f) == NSD_OK);
        }
    }
    printf("fuzz: %llu one-word mutations, %llu accepted (each re-parsed with the shape of draw()), mutations of fixed fields %llu, refused %llu\n",
           (unsigned long long)tried, (unsigned long long)accepted, (unsigned long long)fixed_hits, (unsigned long long)fixed_refused);
    CHECK(fixed_hits == fixed_refused);
}

static void unit_refusals(void) {
    dstate s; trec r;
    do { rand_state(&s); } while (model(&s, &DEV[0], &r));
    nsd_policy un = DEV[1].pol; un.unmodelled = 1; tdev d = DEV[1]; d.pol = un;
    CHECK(expand_rec(&r, &DEV[0].pol, &d) == NSD_E_POLICY);
    trec bad = r; snprintf(bad.key[0], sizeof bad.key[0], "zz%s", r.key[0] + 2);
    CHECK(expand_rec(&bad, &DEV[0].pol, &DEV[1]) == NSD_E_KEY);
    bad = r; bad.key[0][0] = bad.key[0][0] == 'a' ? 'b' : 'a';   // a valid key whose module id differs
    CHECK(expand_rec(&bad, &DEV[0].pol, &DEV[1]) == NSD_E_ID);
    // a key the device has no bundle for
    bad = r; memcpy(bad.key[0], "0000000000000000", 16);
    { nsd_pipe *p = &PP; nsd_parse(bad.w, bad.nw, p); p->st[0].id = nsd_fnv(bad.key[0], bad.klen[0]); bad.nw = nsd_emit(p, bad.w, NSD_MAX_WORDS); }
    CHECK(expand_rec(&bad, &DEV[0].pol, &DEV[1]) == NSD_E_SHADER);
    // the source policy is wrong about the layout (Mi 14 list read as if the limit were 4): model drift is refused, not guessed
    nsd_policy wrong = DEV[0].pol; wrong.ubo_dyn_limit = 0; wrong.ubo_dynamic = 0;
    int refused_layout = 0;
    for (int i = 0; i < 200; i++) { rand_state(&s); if (model(&s, &DEV[0], &r)) continue; if (expand_rec(&r, &wrong, &DEV[1]) == NSD_E_LAYOUT) refused_layout++; }
    CHECK(refused_layout > 50);
    // dependencies of the other tile policy: refused
    nsd_policy tile = DEV[0].pol; tile.tile_barrier = 1;
    do { rand_state(&s); } while (model(&s, &DEV[0], &r));
    CHECK(expand_rec(&r, &tile, &DEV[1]) == NSD_E_DEPS);
    printf("refusals: policy / key / module id / missing bundle / layout drift (%d of 200) / dependencies as expected\n", refused_layout);
    // [NR-31b] a fused post stage: re-targeted with its own bundle (here the host's), its key kept; never on another stage
    int fused = 0;
    for (int i = 0; i < 400 && fused < 40; i++) {
        rand_state(&s); s.alpha_func = 8;
        if (s.fs < 0 || model(&s, &DEV[0], &r)) continue;
        trec f = r; const uint32_t last = f.nk - 1; char k2[NSD_KEY_MAX + 1]; snprintf(k2, sizeof k2, "F1A.%s", r.key[last]);
        memcpy(f.key[last], k2, sizeof k2); f.klen[last] = (uint32_t)strlen(k2);
        nsd_pipe *p = &PP; nsd_parse(f.w, f.nw, p); p->st[last].id = nsd_fnv(k2, f.klen[last]); f.nw = nsd_emit(p, f.w, NSD_MAX_WORDS);
        CHECK(expand_rec(&f, &DEV[0].pol, &DEV[0]) == NSD_OK && same_out(&f));
        trec g = f; memcpy(g.key[0], k2, sizeof k2); g.klen[0] = f.klen[last];
        p = &PP; nsd_parse(g.w, g.nw, p); p->st[0].id = nsd_fnv(k2, g.klen[0]); g.nw = nsd_emit(p, g.w, NSD_MAX_WORDS);
        CHECK(expand_rec(&g, &DEV[0].pol, &DEV[0]) == NSD_E_VARIANT);
        fused++;
    }
    CHECK(fused == 40);
    // [NR-31b] the read-only rule without the stage's read flags: refused (facts), never a guess
    {   nsd_policy ro = DEV[5].pol; int facts = 0, n = 0;
        for (int i = 0; i < 300; i++) { rand_state(&s); if (model(&s, &DEV[0], &r)) continue; nsd_dev v = mkdev(&DEV[5]); v.shader = cb_noreads; ++n;
            const char *keys[NSD_STAGES]; for (uint32_t k = 0; k < r.nk; k++) keys[k] = r.key[k];
            facts += nsd_expand(&DEV[0].pol, &ro, &v, keys, r.klen, r.nk, r.w, r.nw, &WK, &OUT) == NSD_E_FACTS; }
        CHECK(facts == n);
        printf("fused post stages: %d re-targeted with their key, refused on the vertex stage; read-only rule without read flags: %d of %d refused (facts)\n", fused, facts, n); }
}

// ---------------- seed files ----------------
static size_t put32(uint8_t *b, size_t at, uint32_t v) { memcpy(b + at, &v, 4); return at + 4; }
static size_t put_rec(uint8_t *b, size_t at, const trec *r) {
    at = put32(b, at, NSD_REC_MAGIC); at = put32(b, at, r->nk);
    for (uint32_t i = 0; i < r->nk; i++) { at = put32(b, at, r->klen[i]); memcpy(b + at, r->key[i], r->klen[i]); at += r->klen[i]; }
    at = put32(b, at, (uint32_t)r->nw); memcpy(b + at, r->w, r->nw * 4); return at + r->nw * 4;
}
static size_t seed_header(uint8_t *b, uint32_t nrec, const nsd_policy *pol, size_t body_len) {
    uint32_t pw[NSD_POLICY_WORDS]; nsd_policy_words(pol, pw);
    const uint64_t id = nsd_fnv64(b + 4 * (8 + NSD_POLICY_WORDS), body_len);
    size_t at = 0; at = put32(b, at, NSD_SEED_MAGIC); at = put32(b, at, NSD_SEED_VERSION); at = put32(b, at, 8 + NSD_POLICY_WORDS); at = put32(b, at, nrec); at = put32(b, at, 0);
    at = put32(b, at, (uint32_t)id); at = put32(b, at, (uint32_t)(id >> 32)); at = put32(b, at, NSD_POLICY_WORDS);
    for (int i = 0; i < NSD_POLICY_WORDS; i++) at = put32(b, at, pw[i]);
    return at;
}
static void unit_seed_file(void) {
    static uint8_t buf[1 << 20]; dstate s; static trec recs[50]; uint32_t n = 0;
    const size_t h = 4 * (8 + NSD_POLICY_WORDS); size_t at = h;
    while (n < 50) { rand_state(&s); if (model(&s, &DEV[0], &recs[n])) continue; at = put_rec(buf, at, &recs[n]); n++; }
    seed_header(buf, n, &DEV[0].pol, at - h);
    nsd_seed_hdr hd; CHECK(nsd_seed_open(buf, at, &hd) == 0 && hd.nrec == n && hd.body == h && hd.src.ubo_dyn_limit == 16 && hd.src.alpha_test == 1);
    size_t off = hd.body; static nsd_rec rr; uint32_t got = 0;
    while (nsd_rec_next(buf, at, &off, &rr) == 1) {
        CHECK(rr.nk == recs[got].nk && rr.nw == recs[got].nw && !memcmp(rr.words, recs[got].w, rr.nw * 4));
        got++;
    }
    CHECK(got == n && off == at);
    buf[at - 3] ^= 1; CHECK(nsd_seed_open(buf, at, &hd) == -1); buf[at - 3] ^= 1;   // damaged body: the id differs
    buf[4] = 2; CHECK(nsd_seed_open(buf, at, &hd) == -1); buf[4] = 1;
    CHECK(nsd_seed_open(buf, 20, &hd) == -1);
    size_t o2 = h; CHECK(nsd_rec_next(buf, at - 5, &o2, &rr) == 1);
    printf("seed file: %u records read back, damaged body / version / short header refused\n", n);
}

// ---------------- real lists (other ports' devices: same engine and format) ----------------
static void real_list(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) { printf("%s: cannot open\n", path); return; }
    fseek(f, 0, SEEK_END); long len = ftell(f); rewind(f);
    uint8_t *b = (uint8_t *)malloc((size_t)len); if (fread(b, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(b); return; } fclose(f);
    size_t off = 0; static nsd_rec r; uint64_t n = 0, rt = 0, shape = 0; uint64_t why[64] = {0}; const char *names[64] = {0}; int nn = 0;
    while (nsd_rec_next(b, (size_t)len, &off, &r) == 1) {
        ++n;
        if (nsd_parse(r.words, r.nw, &PP) != NSD_OK) continue;
        uint32_t w2[NSD_MAX_WORDS]; const size_t m = nsd_emit(&PP, w2, NSD_MAX_WORDS);
        if (m == r.nw && !memcmp(w2, r.words, m * 4)) ++rt;
        const char *fl = NULL;
        if (nsd_shape(&PP, r.nk, &fl) == NSD_OK) { ++shape; continue; }
        int k = 0; for (; k < nn; k++) if (!strcmp(names[k], fl)) break;
        if (k == nn && nn < 64) names[nn++] = fl;
        if (k < 64) why[k]++;
    }
    printf("%s: %llu records, %llu parse + emit byte for byte, %llu of the shape draw() writes", path, (unsigned long long)n, (unsigned long long)rt, (unsigned long long)shape);
    for (int k = 0; k < nn; k++) printf("%s %s %llu", k ? "," : "; not:", names[k], (unsigned long long)why[k]);
    printf("%s\n", off == (size_t)len ? "" : " (stopped at a damaged record)");
    CHECK(rt == n);
    free(b);
}

// ---------------- modes for the Python test ----------------
static int write_list(const char *path, int count) {
    FILE *f = fopen(path, "wb"); if (!f) return 1;
    static uint8_t buf[1 << 16]; dstate s; trec r; int n = 0;
    while (n < count) { rand_state(&s); if (model(&s, &DEV[0], &r)) continue; const size_t m = put_rec(buf, 0, &r); fwrite(buf, 1, m, f); n++; }
    fclose(f); return 0;
}
static int write_bundles(const char *dir) {   // xgs headers (magic, version 1, stage, bindings, inputs, c1 0, spirv 0, reserved) + binding tables
    const fshader *all[NVS + NFS + NGS + 2]; int n = 0;
    for (int i = 0; i < NVS; i++) all[n++] = &U_vs[i];
    for (int i = 0; i < NFS; i++) all[n++] = &U_fs[i];
    for (int i = 0; i < NGS; i++) all[n++] = &U_gs[i];
    all[n++] = &U_tc; all[n++] = &U_te;
    for (int i = 0; i < n; i++) {
        char p[1024]; snprintf(p, sizeof p, "%s/%s.xgs", dir, all[i]->key);
        FILE *f = fopen(p, "wb"); if (!f) return 1;
        uint32_t h[8] = {0x31534758u, 1, all[i]->stage, all[i]->nb, all[i]->nin, 0, 0, 0}; fwrite(h, 4, 8, f);
        for (uint32_t k = 0; k < all[i]->nb; k++) { uint32_t b[8] = {all[i]->b[k].binding, all[i]->b[k].type, all[i]->b[k].count, 0, all[i]->b[k].source, all[i]->b[k].slot, 0, 0}; fwrite(b, 4, 8, f); }
        fwrite(all[i]->loc, 4, all[i]->nin, f); fclose(f);
    }
    return 0;
}
static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); rewind(f);
    uint8_t *b = (uint8_t *)malloc((size_t)len + 1); if (fread(b, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)len; return b;
}
static int check_seed(const char *seed, const char *list) {
    size_t ns = 0, nl = 0; uint8_t *s = slurp(seed, &ns), *l = slurp(list, &nl);
    if (!s || !l) { printf("cannot read %s / %s\n", seed, list); return 1; }
    nsd_seed_hdr h; if (nsd_seed_open(s, ns, &h)) { printf("%s: not a seed\n", seed); return 1; }
    size_t os = h.body, ol = 0; static nsd_rec a, b; uint64_t n = 0, eq = 0;
    for (;;) {
        const int ga = nsd_rec_next(s, ns, &os, &a); const int gb = nsd_rec_next(l, nl, &ol, &b);
        if (ga != 1 || gb != 1) { if (ga != gb) printf("record counts differ\n"); break; }
        ++n;
        tdev d = DEV[0]; d.pol = h.src; nsd_dev v = mkdev(&d);
        if (nsd_expand(&h.src, &h.src, &v, a.key, a.klen, a.nk, a.words, a.nw, &WK, &OUT) != NSD_OK) continue;
        int same = OUT.nk == b.nk && OUT.nw == b.nw && !memcmp(OUT.words, b.words, b.nw * 4);
        for (uint32_t i = 0; same && i < b.nk; i++) same = OUT.klen[i] == b.klen[i] && !memcmp(OUT.key[i], b.key[i], b.klen[i]);
        eq += same;
    }
    printf("check-seed: %llu records, %llu expand to the list's record byte for byte (seed id %016llx, %u records in the header)\n", (unsigned long long)n,
           (unsigned long long)eq, (unsigned long long)h.id, h.nrec);
    free(s); free(l);
    return !(n == h.nrec && eq == n && n > 0);
}


// ---------------- --real SRC DST BUNDLES PROFILE: a real list re-targeted, compared with a real list of the other policy ----------------
// (TL2 on the Mi 14: the Turnip list and the DM_GFX_SIM_GPU=mali list, same engine and record format.) Fragment outputs come from the
// bundle's own SPIR-V (Output variables' Location / BuiltIn SampleMask) - the runtime reads the final words.
static const char *g_bdir, *g_bdir2;
static int real_shader(void *u, const char *base, nsd_shader *o, unsigned want) {
    (void)u; char p[1024]; snprintf(p, sizeof p, "%s/%s.xgs", g_bdir, base);
    size_t n = 0; uint8_t *b = NULL; FILE *f = fopen(p, "rb");
    if (!f && g_bdir2) { snprintf(p, sizeof p, "%s/%s.xgs", g_bdir2, base); f = fopen(p, "rb"); }   // fused post bundles (composed by the tool test)
    if (!f) return -1;
    fseek(f, 0, SEEK_END); n = (size_t)ftell(f); rewind(f); b = (uint8_t *)malloc(n); if (fread(b, 1, n, f) != n) { fclose(f); free(b); return -1; } fclose(f);
    uint32_t h[8]; if (n < 32) { free(b); return -1; } memcpy(h, b, 32);
    if (h[0] != 0x31534758u || h[1] != 1 || h[3] > NSD_MAX_BIND || h[4] > NSD_MAX_IN) { free(b); return -1; }
    o->stage = h[2]; o->nb = h[3];
    for (uint32_t i = 0; i < h[3]; i++) { uint32_t x[8]; memcpy(x, b + 32 + 32 * i, 32); o->b[i] = (nsd_bind){x[0], x[1], x[2], x[4], x[5]}; }
    o->nin = h[4]; memcpy(o->in_loc, b + 32 + 32 * h[3], 4 * h[4]);
    if (want & NSD_W_READS) {   // the bundle's SPIR-V (the runtime scans the final words: the load-time rewrites add / drop no uniform reads)
        const size_t off = 32 + 32 * (size_t)h[3] + 4 * (size_t)h[4] + h[5];
        if (off + h[6] <= n) {
            uint32_t bn[NSD_MAX_BIND]; uint8_t rd[NSD_MAX_BIND] = {0}; unsigned at[NSD_MAX_BIND], k = 0;
            for (uint32_t i = 0; i < o->nb; i++) if (o->b[i].type == 6) { bn[k] = o->b[i].binding; at[k++] = i; }
            if (k) nr_spv_binding_reads((const uint32_t *)(b + off), h[6] / 4, bn, k, rd);
            memset(o->read, 0, sizeof o->read); for (unsigned i = 0; i < k; i++) o->read[at[i]] = rd[i]; o->reads = 1;
        }
    }
    if (want & NSD_W_FACTS) {
        const size_t off = 32 + 32 * (size_t)h[3] + 4 * (size_t)h[4] + h[5]; const uint32_t *w = (const uint32_t *)(b + off); const size_t nw = h[6] / 4;
        static uint32_t loc[1 << 16], sm[1 << 16]; uint32_t outv[256]; int no = 0;
        if (off + h[6] <= n) {
            for (size_t i = 5; i < nw;) { const uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > nw) break;
                if (op == 71 && len >= 4 && w[i + 1] < (1u << 16)) { if (w[i + 2] == 30) loc[w[i + 1]] = w[i + 3] + 1; if (w[i + 2] == 11 && w[i + 3] == 20) sm[w[i + 1]] = 1; }
                if (op == 59 && len >= 4 && w[i + 3] == 3 && no < 256) outv[no++] = w[i + 2];
                i += len; }
            o->outputs = 0; o->sample_mask = 0;
            for (int i = 0; i < no; i++) if (outv[i] < (1u << 16)) { if (loc[outv[i]]) o->outputs |= 1u << ((loc[outv[i]] - 1) & 31); if (sm[outv[i]]) o->sample_mask = 1; loc[outv[i]] = 0; sm[outv[i]] = 0; }
            o->facts = 1;
        }
    }
    free(b); return 0;
}
static int real_pair(const char *srcp, const char *dstp, const char *bdir, int retype) {
    g_bdir = bdir;
    size_t ns = 0, nd = 0; uint8_t *s = slurp(srcp, &ns), *d = slurp(dstp, &nd);
    if (!s || !d) return 1;
    tdev dst = DEV[0]; dst.pol.tile_barrier = 1; dst.pol.noout = 1; dst.pol.features = NSD_F_ALL & ~(NSD_F_DBOUNDS | NSD_F_NONSOLID); dst.pol.retype = (uint32_t)retype;   // XBG_SIM_PROFILE_MALI on Turnip
    nsd_dev v = mkdev(&dst); v.shader = real_shader; v.vtx_format = NULL; v.att_ok = NULL;
    static nsd_rec a, b; size_t off = 0; uint64_t n = 0, ok = 0, found = 0, near = 0; uint64_t why[NSD_E_COUNT] = {0};
    static char seen[4096][64]; (void)seen;
    // destination records by bytes (linear scan: a few hundred)
    typedef struct { uint32_t nw; uint32_t *w; char keys[200]; } drec; static drec D[4096]; int nD = 0;
    while (nD < 4096 && nsd_rec_next(d, nd, &off, &b) == 1) { D[nD].nw = (uint32_t)b.nw; D[nD].w = (uint32_t *)malloc(b.nw * 4); memcpy(D[nD].w, b.words, b.nw * 4);
        D[nD].keys[0] = 0; for (uint32_t i = 0; i < b.nk; i++) { strncat(D[nD].keys, b.key[i], b.klen[i] < 16 ? b.klen[i] : 16); strcat(D[nD].keys, "+"); } nD++; }
    uint64_t fld[64] = {0}; const char *fname[64] = {0}; int nf = 0;
    off = 0;
    while (nsd_rec_next(s, ns, &off, &a) == 1) {
        ++n;
        const int e = nsd_expand(&DEV[0].pol, &dst.pol, &v, a.key, a.klen, a.nk, a.words, a.nw, &WK, &OUT);
        if (e) { why[e]++; continue; }
        ++ok;
        char keys[200] = ""; for (uint32_t i = 0; i < a.nk; i++) { strncat(keys, a.key[i], 16); strcat(keys, "+"); }
        int hit = 0, best = -1; size_t bestd = (size_t)-1;
        for (int i = 0; i < nD && !hit; i++) {
            if (D[i].nw == OUT.nw && !memcmp(D[i].w, OUT.words, OUT.nw * 4)) hit = 1;
            else if (!strcmp(D[i].keys, keys) && D[i].nw == OUT.nw) { size_t dd = 0; for (size_t k = 0; k < OUT.nw; k++) dd += D[i].w[k] != OUT.words[k]; if (dd < bestd) { bestd = dd; best = i; } }
        }
        if (hit) { ++found; continue; }
        if (best >= 0 && bestd <= 2) { ++near;
            for (size_t k = 0; k < OUT.nw; k++) if (D[best].w[k] != OUT.words[k]) { const char *fl = nsd_field_at(OUT.words, OUT.nw, k); int j = 0; for (; j < nf; j++) if (!strcmp(fname[j], fl)) break; if (j == nf && nf < 64) fname[nf++] = fl; if (j < 64) fld[j]++; } }
    }
    printf("real: %s (%llu records) -> SIM mali (retype %d) -> found in %s (%d records): expanded %llu, of them exactly a record of the destination list %llu; "
           "within 2 words of a record with the same shaders %llu;", srcp, (unsigned long long)n, retype, dstp, nD, (unsigned long long)ok, (unsigned long long)found, (unsigned long long)near);
    for (int r = 1; r < NSD_E_COUNT; r++) if (why[r]) printf(" refused %s %llu", nsd_reason_name(r), (unsigned long long)why[r]);
    printf("\n  near misses differ in:"); for (int j = 0; j < nf; j++) printf(" %s %llu", fname[j], (unsigned long long)fld[j]); printf("\n");
    for (int i = 0; i < nD; i++) free(D[i].w);
    free(s); free(d); return 0;
}

// --real-seed SEED LIST BDIR BDIR2|- POLICY [POLICY ...]: a seed built from a real phone list (tools/dos2_pipe_seed.py build) re-targeted to
// each POLICY (a file of 23 numbers: the 21 nsd_policy words, split, read-only - tools/dos2_pipe_seed.py policy --words), every result
// looked up in LIST (the list the seed came from, records of all those policies). Every list record the build turned into a seed record
// must come back under the policy that recorded it; one that does not is named (a layout-only difference = another layout rule / build).
static int load_pol(const char *path, nsd_policy *p) {
    FILE *f = fopen(path, "r"); if (!f) return -1;
    uint32_t w[NSD_POLICY_WORDS + 2] = {0}; int ok = 1;
    for (int i = 0; i < NSD_POLICY_WORDS + 2; i++) if (fscanf(f, "%u", &w[i]) != 1) ok = 0;
    fclose(f); if (!ok) return -1;
    nsd_policy_from(w, p); p->split = w[NSD_POLICY_WORDS]; p->ubo_ro = w[NSD_POLICY_WORDS + 1]; return 0;
}
typedef struct { uint32_t nw; uint32_t *w; char keys[200]; } xrec;
static void keys_of(const nsd_rec *r, char *k) { k[0] = 0; for (uint32_t i = 0; i < r->nk; i++) { strncat(k, r->key[i], r->klen[i] < 40 ? r->klen[i] : 40); strcat(k, "+"); } }
static int real_seed(int argc, char **argv) {
    enum { MAXP = 12, MAXR = 4096 };
    const char *seedp = argv[2], *listp = argv[3]; g_bdir = argv[4]; g_bdir2 = strcmp(argv[5], "-") ? argv[5] : NULL;
    const int np = argc - 6 < MAXP ? argc - 6 : MAXP; nsd_policy pol[MAXP];
    for (int j = 0; j < np; j++) if (load_pol(argv[6 + j], &pol[j])) { printf("%s: not 23 numbers\n", argv[6 + j]); return 1; }
    size_t ns = 0, nl = 0; uint8_t *s = slurp(seedp, &ns), *l = slurp(listp, &nl);
    if (!s || !l) { printf("cannot read %s / %s\n", seedp, listp); return 1; }
    nsd_seed_hdr h; if (nsd_seed_open(s, ns, &h)) { printf("%s: not a seed\n", seedp); return 1; }
    static xrec L[MAXR], E[MAXR][MAXP]; static int Lhit[MAXR]; int nL = 0, nS = 0; static nsd_rec a; size_t off = 0;
    while (nL < MAXR && nsd_rec_next(l, nl, &off, &a) == 1) { L[nL].nw = (uint32_t)a.nw; L[nL].w = (uint32_t *)malloc(a.nw * 4 + 4); memcpy(L[nL].w, a.words, a.nw * 4); keys_of(&a, L[nL].keys); Lhit[nL] = -1; nL++; }
    uint64_t why[MAXP][NSD_E_COUNT] = {{0}}, ok[MAXP] = {0}, found[MAXP] = {0};
    off = h.body;
    while (nS < MAXR && nsd_rec_next(s, ns, &off, &a) == 1) {
        for (int j = 0; j < np; j++) {
            tdev d = DEV[0]; d.pol = pol[j]; nsd_dev v = mkdev(&d); v.shader = real_shader; v.vtx_format = NULL; v.att_ok = NULL;
            xrec *x = &E[nS][j]; x->nw = 0; x->w = NULL;
            const int e = nsd_expand(&h.src, &pol[j], &v, a.key, a.klen, a.nk, a.words, a.nw, &WK, &OUT);
            if (e) { why[j][e]++; continue; }
            ok[j]++; x->nw = (uint32_t)OUT.nw; x->w = (uint32_t *)malloc(OUT.nw * 4 + 4); memcpy(x->w, OUT.words, OUT.nw * 4);
            x->keys[0] = 0; for (uint32_t i = 0; i < OUT.nk; i++) { strncat(x->keys, OUT.key[i], OUT.klen[i] < 40 ? OUT.klen[i] : 40); strcat(x->keys, "+"); }
            for (int i = 0; i < nL; i++) if (L[i].nw == x->nw && !strcmp(L[i].keys, x->keys) && !memcmp(L[i].w, x->w, x->nw * 4)) { if (Lhit[i] < 0) Lhit[i] = j; found[j]++; break; }
        }
        nS++;
    }
    printf("real-seed: %s, %d records (source policy of its header) -> %d policies; list %s, %d records\n", seedp, nS, np, listp, nL);
    for (int j = 0; j < np; j++) {
        printf("  %-28s split %u read-only %u tile %u limit %2u dynamic %u NOOUT %u features %#04x: expanded %llu, of them a record of the list %llu", argv[6 + j], pol[j].split,
               pol[j].ubo_ro, pol[j].tile_barrier, pol[j].ubo_dyn_limit, pol[j].ubo_dynamic, pol[j].noout, pol[j].features, (unsigned long long)ok[j], (unsigned long long)found[j]);
        for (int r = 1; r < NSD_E_COUNT; r++) if (why[j][r]) printf("; refused %s %llu", nsd_reason_name(r), (unsigned long long)why[j][r]);
        printf("\n");
    }
    // the list's records that no policy's expansion gives: same keys and every word after the layout equal to some expansion = layout only
    int back = 0, lay = 0, other = 0, told = 0;
    for (int i = 0; i < nL; i++) {
        if (Lhit[i] >= 0) { back++; continue; }
        const size_t le = L[i].nw > 2 ? 2 + L[i].w[1] : 0; int lo = 0;
        for (int k = 0; k < nS && !lo; k++) for (int j = 0; j < np && !lo; j++) { const xrec *x = &E[k][j];
            if (x->w && x->nw == L[i].nw && le < x->nw && !strcmp(x->keys, L[i].keys) && !memcmp(x->w + le, L[i].w + le, (x->nw - le) * 4)) lo = 1; }
        if (lo) lay++; else other++;
        if (getenv("NSD_REAL_VERBOSE") && told++ < atoi(getenv("NSD_REAL_VERBOSE"))) printf("    list record %d (%s) not given by any policy: %s\n", i, L[i].keys, lo ? "layout only" : "other words");
    }
    printf("  list records given back byte for byte by one of the policies: %d of %d; not given: layout only %d (another layout rule / an older build), other words %d\n", back, nL, lay, other);
    for (int i = 0; i < nL; i++) free(L[i].w);
    for (int k = 0; k < nS; k++) for (int j = 0; j < np; j++) free(E[k][j].w);
    free(s); free(l); return 0;
}

int main(int argc, char **argv) {
    universe(); devices();
    if (argc >= 4 && !strcmp(argv[1], "--write-list")) return write_list(argv[2], atoi(argv[3]));
    if (argc >= 3 && !strcmp(argv[1], "--write-bundles")) return write_bundles(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "--check-seed")) return check_seed(argv[2], argv[3]);
    if (argc >= 5 && !strcmp(argv[1], "--real")) return real_pair(argv[2], argv[3], argv[4], argc >= 6 ? atoi(argv[5]) : 0);
    if (argc >= 7 && !strcmp(argv[1], "--real-seed")) return real_seed(argc, argv);
    unit_keys();
    prop_roundtrip();
    prop_model();
    unit_refusals();
    prop_fuzz();
    unit_seed_file();
    for (int i = 1; i < argc; i++) real_list(argv[i]);
    printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
