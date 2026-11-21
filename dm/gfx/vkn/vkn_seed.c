// [DOS2] VK-NATIVE stage 5 rest: the portable warm-up seed on the backend (vkn_seed.h has the scheme, the exactness argument, the
// switch). Runs on the warm-up worker only (vkn_warm.c calls vkn_seed_tick / _note / _census); the census line may come from the
// render thread (counters copied under the mutex). No game addresses.
#ifdef DM_GAME_DOS2
#include "vkn_seed.h"
#include "vkn.h"
#include "vkn_shader.h"
#include "vkn_state.h"
#include "../nr/nr_seed.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int vkn_shlib_shader_dir(char *out, size_t bytes) __attribute__((weak));
#define VKN_RED_MARK "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"

// One seed record as the translation needs it (the decoded create info, Vulkan values).
typedef struct srec {
    char key[2][17];                  // vertex / fragment base keys ("" = no fragment stage)
    uint32_t at_func, at_bits;        // fragment "#at" variant (0 = base)
    uint8_t state;                    // 0 waiting, 1 translated, 2 refused
    uint8_t ncolor, samples, has_depth, topo, stencil_ref, nva, nvb;
    uint32_t color[8], depth;         // VkFormats (0 = slot not bound)
    uint8_t mask[8];
    nsd_blend blend;                  // of the first bound colour slot (every bound slot blends alike: checked)
    uint32_t dt, dw, dcmp, st_en; nsd_stencil sf, sb;
    uint32_t poly, cull, front, bias_en;
    uint32_t va[16][4], vb[3][3];     // streams 0..2 only (location, binding, format, offset) / (binding, stride, rate)
} srec;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int mode;                         // -1 not read
    int loaded;
    srec *r; uint32_t n;
    uint64_t epoch; uint32_t sig;     // what the last pass saw (shader epoch, state counts)
    vkn_seed_stats s;
    uint64_t *had; uint32_t nhad, chad;    // verify / stats: sums of the translated records
    uint64_t *keys; uint32_t nkeys, ckeys; // fnv of the key pairs of the translated records
    uint8_t *img; size_t nimg;        // host test image
    int told_differ;
} g = {.mode = -1};

const char *vkn_seed_reason_name(int r) {
    static const char *const n[VKN_SEED_R_N] = {"not a seed record", "not pack keys", "a fused post stage (the backend's own)", "a vertex conversion variant (#vi..c)",
        "a create-info shape the old path never wrote", "a pass format this device cannot render to", "a topology the engine does not draw", "not one vertex + at most one fragment stage",
        "no blend state of this run matches", "no depth-stencil state of this run matches", "no raster state of this run matches", "no vertex format of this run matches"};
    return r >= 0 && r < VKN_SEED_R_N ? n[r] : "?";
}
static void refuse(srec *r, int why) { if (r) r->state = 2; if (why >= 0 && why < VKN_SEED_R_N) g.s.refused[why]++; }

// ---------------------------------------------------------------- reading the file into srec
static int fmt_depth(uint32_t f) { return f >= VK_FORMAT_D16_UNORM && f <= VK_FORMAT_D32_SFLOAT_S8_UINT; }
static int decode(const nsd_rec *x, nsd_pipe *p, srec *o) {   // -1 = ok, else a refusal reason
    memset(o, 0, sizeof *o);
    if (nsd_parse(x->words, x->nw, p) != NSD_OK) return VKN_SEED_R_PARSE;
    if (x->nk < 1 || x->nk > 2 || p->nst != x->nk) return VKN_SEED_R_STAGES;
    int have_vs = 0;
    for (uint32_t i = 0; i < x->nk; i++) {
        nsd_key k;
        if (nsd_key_parse(x->key[i], x->klen[i], &k)) return VKN_SEED_R_KEYS;
        if (k.tag == NSD_TAG_FUSED) return VKN_SEED_R_FUSED;
        if (k.tag == NSD_TAG_VI && k.vi_conv) return VKN_SEED_R_CONV;
        if (p->st[i].stage == VK_SHADER_STAGE_VERTEX_BIT) { if (k.tag == NSD_TAG_AT || have_vs) return VKN_SEED_R_STAGES; memcpy(o->key[0], k.base, 17); have_vs = 1; }
        else if (p->st[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
            if (k.tag == NSD_TAG_VI || o->key[1][0]) return VKN_SEED_R_STAGES;
            memcpy(o->key[1], k.base, 17);
            if (k.tag == NSD_TAG_AT) { o->at_func = k.at_func; o->at_bits = k.at_bits; }
        } else return VKN_SEED_R_STAGES;
    }
    if (!have_vs || p->has_tess || p->subpass || p->ncol > 8) return VKN_SEED_R_STAGES;
    // the pass: colour slots (attachment references; VK_ATTACHMENT_UNUSED = not bound), depth
    o->ncolor = (uint8_t)p->ncol; o->samples = (uint8_t)(p->samples ? p->samples : 1);
    int first = -1;
    for (uint32_t i = 0; i < p->ncol; i++) {
        const uint32_t a = p->col[i].att;
        if (a == VK_ATTACHMENT_UNUSED) continue;
        if (a >= p->natt || fmt_depth(p->att[a].format)) return VKN_SEED_R_SHAPE;
        o->color[i] = p->att[a].format;
        if (i >= p->nblend) return VKN_SEED_R_SHAPE;
        o->mask[i] = (uint8_t)(p->blend[i].mask & 0xf);
        if (first < 0) { first = (int)i; o->blend = p->blend[i]; }
        else {   // the backend blends every bound slot alike (ApplyBlend): a record that does not cannot be one of its pipelines
            const nsd_blend *a0 = &o->blend, *b = &p->blend[i];
            if (a0->enable != b->enable || (a0->enable && (a0->src_c != b->src_c || a0->dst_c != b->dst_c || a0->op_c != b->op_c || a0->src_a != b->src_a ||
                                                            a0->dst_a != b->dst_a || a0->op_a != b->op_a))) return VKN_SEED_R_BLEND;
        }
    }
    if (p->has_depth) {
        const uint32_t a = p->depth.att;
        if (a >= p->natt || !fmt_depth(p->att[a].format)) return VKN_SEED_R_SHAPE;
        o->depth = p->att[a].format; o->has_depth = 1;
    }
    o->topo = (uint8_t)p->topology;
    o->dt = p->dt; o->dw = p->dw; o->dcmp = p->dcmp; o->st_en = p->st_en; o->sf = p->sf; o->sb = p->sb;
    o->stencil_ref = (uint8_t)p->sf.ref;
    o->poly = p->poly; o->cull = p->cull; o->front = p->front; o->bias_en = p->bias_en;
    for (uint32_t i = 0; i < p->nva; i++) {
        if (p->va[i][1] >= VKN_VFMT_STREAMS) continue;   // (the old path's own default-attribute stream: the backend has its null stream)
        if (o->nva >= 16) return VKN_SEED_R_SHAPE;
        memcpy(o->va[o->nva++], p->va[i], sizeof o->va[0]);
    }
    for (uint32_t i = 0; i < p->nvb; i++) {
        if (p->vb[i][0] >= VKN_VFMT_STREAMS) continue;
        if (o->nvb >= 3) return VKN_SEED_R_SHAPE;
        memcpy(o->vb[o->nvb++], p->vb[i], sizeof o->vb[0]);
    }
    return -1;
}
static void load(const char *dir) {
    uint8_t *data = NULL; size_t n = 0; char path[640] = "";
    if (g.img) { data = g.img; n = g.nimg; snprintf(path, sizeof path, "(test image)"); }
    else {
        char sd[512]; const char *cand[2] = {NULL, NULL}; char p0[600], p1[600];
        if (vkn_shlib_shader_dir && !vkn_shlib_shader_dir(sd, sizeof sd) && sd[0]) { snprintf(p0, sizeof p0, "%s/dos2-pipe-seed.bin", sd); cand[0] = p0; }
        const char *fd = getenv("DM_MARKER_DIR");
        if (fd && *fd) { snprintf(p1, sizeof p1, "%s/dos2-pipe-seed.bin", fd); cand[1] = p1; }
        (void)dir;
        for (int i = 0; i < 2 && !data; i++) {
            if (!cand[i]) continue;
            FILE *f = fopen(cand[i], "rb"); if (!f) continue;
            if (!fseek(f, 0, SEEK_END)) {
                const long sz = ftell(f);
                if (sz > 32 && sz < (64l << 20) && !fseek(f, 0, SEEK_SET) && (data = (uint8_t *)malloc((size_t)sz))) {
                    if (fread(data, 1, (size_t)sz, f) == (size_t)sz) { n = (size_t)sz; snprintf(path, sizeof path, "%s", cand[i]); }
                    else { free(data); data = NULL; }
                }
            }
            fclose(f);
        }
    }
    g.loaded = 1;
    if (!data) { g.s.file = 0; vkn_logf("seed: DM_VK_NATIVE_SEED=%d: no dos2-pipe-seed.bin (shader directory, files/): nothing to do", g.mode); return; }
    nsd_seed_hdr h;
    if (nsd_seed_open(data, n, &h)) { g.s.file = -1; vkn_logf(VKN_RED_MARK "seed: %s is not a version-%u seed or is damaged: ignored", path, NSD_SEED_VERSION); if (data != g.img) free(data); return; }
    g.s.file = 1; g.s.id = h.id;
    g.r = (srec *)calloc(h.nrec ? h.nrec : 1, sizeof *g.r);
    nsd_pipe *p = (nsd_pipe *)malloc(sizeof *p); nsd_rec *x = (nsd_rec *)malloc(sizeof *x);
    size_t off = h.body; int rc = 0;
    while (g.r && p && x && g.n < h.nrec && (rc = nsd_rec_next(data, n, &off, x)) == 1) {
        srec *o = &g.r[g.n++];
        const int why = decode(x, p, o);
        if (why >= 0) refuse(o, why);
    }
    if (rc < 0) g.s.refused[VKN_SEED_R_PARSE]++;
    g.s.records = g.n;
    free(p); free(x); if (data != g.img) free(data);
    uint32_t bad = 0; for (int i = 0; i < VKN_SEED_R_N; i++) bad += g.s.refused[i];
    vkn_logf("seed: DM_VK_NATIVE_SEED=%d (stage 5 rest, replaces DM_NR_SEED: %s): %s, seed %016llx, %u records (%u refused while reading), from a device with "
             "tile %u, depth stand-ins %u/%u/%u, retype %u; translated to this run's states when their shaders exist", g.mode,
             g.mode == 1 ? "translated + built by the warm-up worker" : "verify - translated and counted, nothing built",
             path, (unsigned long long)h.id, g.n, bad, h.src.tile_barrier, h.src.d24s8_as_d32s8, h.src.x8d24_as_d32, h.src.depth16_as_d24, h.src.retype);
}

// ---------------------------------------------------------------- translation
static uint32_t fmt_here(uint32_t f, int depth) {   // the VkFormat this device renders the recorded one as (vkn_tex: D24 -> D32F (+S8)); 0 = none
    const vkn_dev *d = vkn_device();
    if (!f || !d) return f;
    const VkFormatFeatureFlags need = depth ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    VkFormatProperties fp; memset(&fp, 0, sizeof fp);
    vkGetPhysicalDeviceFormatProperties(d->phys, (VkFormat)f, &fp);
    if (fp.optimalTilingFeatures & need) return f;
    uint32_t alt = 0;
    if (f == VK_FORMAT_D24_UNORM_S8_UINT) alt = VK_FORMAT_D32_SFLOAT_S8_UINT;
    else if (f == VK_FORMAT_X8_D24_UNORM_PACK32) alt = VK_FORMAT_D32_SFLOAT;
    if (!alt) return 0;
    memset(&fp, 0, sizeof fp); vkGetPhysicalDeviceFormatProperties(d->phys, (VkFormat)alt, &fp);
    return fp.optimalTilingFeatures & need ? alt : 0;
}
static int has_stencil(uint32_t f) { return f == VK_FORMAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }
static int has_depthf(uint32_t f) { return fmt_depth(f) && f != VK_FORMAT_S8_UINT; }
static int any_color(const srec *r) { for (int i = 0; i < 8; i++) if (r->color[i]) return 1; return 0; }
// Candidate ids of a kind: the matching content ids of this run (1..n); 0 ("no state", the build's defaults) only when none matches.
static int blend_ok(const srec *r, uint16_t id) {
    const VkPipelineColorBlendAttachmentState *a = id ? vkn_blend_att(id) : NULL;
    VkPipelineColorBlendAttachmentState z; memset(&z, 0, sizeof z); if (!a) { if (id) return 0; a = &z; }
    if (!any_color(r)) return !a->blendEnable;   // irrelevant to the pipeline: only the "off" states
    const int same = (uint32_t)a->srcColorBlendFactor == r->blend.src_c && (uint32_t)a->dstColorBlendFactor == r->blend.dst_c && (uint32_t)a->colorBlendOp == r->blend.op_c &&
                     (uint32_t)a->srcAlphaBlendFactor == r->blend.src_a && (uint32_t)a->dstAlphaBlendFactor == r->blend.dst_a && (uint32_t)a->alphaBlendOp == r->blend.op_a;
    // Off in the record: an "off" state, or the engine state with the same factors (the shipped seed: 242 of 931 records have blending
    // off with the engine's factors left in place - the old path's own rule; the draw's key names the engine's state)
    if (!r->blend.enable) return !a->blendEnable || same;
    return a->blendEnable && same && (uint32_t)a->srcColorBlendFactor == r->blend.src_c && (uint32_t)a->dstColorBlendFactor == r->blend.dst_c && (uint32_t)a->colorBlendOp == r->blend.op_c &&
           (uint32_t)a->srcAlphaBlendFactor == r->blend.src_a && (uint32_t)a->dstAlphaBlendFactor == r->blend.dst_a && (uint32_t)a->alphaBlendOp == r->blend.op_a;
}
static int stencil_same(const VkStencilOpState *a, const nsd_stencil *b) {
    return (uint32_t)a->failOp == b->fail && (uint32_t)a->passOp == b->pass && (uint32_t)a->depthFailOp == b->dfail && (uint32_t)a->compareOp == b->cmp && a->compareMask == b->cmask && a->writeMask == b->wmask;
}
static int ds_ok(const srec *r, uint32_t depth_here, uint16_t id) {
    VkPipelineDepthStencilStateCreateInfo z; memset(&z, 0, sizeof z); z.depthCompareOp = VK_COMPARE_OP_ALWAYS; z.maxDepthBounds = 1.0f;
    const VkPipelineDepthStencilStateCreateInfo *a = id ? vkn_ds_info(id) : &z;
    if (!a) return 0;
    const int hd = r->has_depth && has_depthf(depth_here), hs = r->has_depth && has_stencil(depth_here);
    const int dt = hd && a->depthTestEnable, st = hs && a->stencilTestEnable;
    const int rdt = hd && r->dt, rst = hs && r->st_en;
    if (dt != rdt || st != rst) return 0;
    if (dt && ((uint32_t)!!a->depthWriteEnable != !!r->dw || (uint32_t)a->depthCompareOp != r->dcmp)) return 0;
    if (st && (!stencil_same(&a->front, &r->sf) || !stencil_same(&a->back, &r->sb))) return 0;
    return 1;
}
static int raster_ok(const srec *r, uint16_t id) {
    VkPipelineRasterizationStateCreateInfo z; memset(&z, 0, sizeof z); z.polygonMode = VK_POLYGON_MODE_FILL; z.cullMode = VK_CULL_MODE_NONE; z.frontFace = VK_FRONT_FACE_CLOCKWISE;
    const VkPipelineRasterizationStateCreateInfo *a = id ? vkn_raster_info(id) : &z;
    if (!a) return 0;
    return (uint32_t)a->polygonMode == r->poly && (uint32_t)a->cullMode == r->cull && (uint32_t)a->frontFace == r->front && (uint32_t)!!a->depthBiasEnable == !!r->bias_en;
}
static uint32_t narrow(uint32_t f) {   // the 3-component format the backend widened (vkn_state.c vfmt_build) back
    switch (f) {
    case VK_FORMAT_R16G16B16A16_SFLOAT: return VK_FORMAT_R16G16B16_SFLOAT; case VK_FORMAT_R16G16B16A16_UNORM: return VK_FORMAT_R16G16B16_UNORM;
    case VK_FORMAT_R16G16B16A16_UINT: return VK_FORMAT_R16G16B16_UINT; case VK_FORMAT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16_SNORM;
    case VK_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16_SINT; case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8_UNORM;
    case VK_FORMAT_R8G8B8A8_UINT: return VK_FORMAT_R8G8B8_UINT; case VK_FORMAT_R8G8B8A8_SNORM: return VK_FORMAT_R8G8B8_SNORM;
    case VK_FORMAT_R8G8B8A8_SINT: return VK_FORMAT_R8G8B8_SINT; default: return f;
    }
}
static int vfmt_ok(const srec *r, uint16_t id) {
    if (!id) return r->nva == 0;
    const vkn_vfmt *v = vkn_vfmt_info(id);
    if (!v || v->nattr != r->nva || v->nbind != r->nvb) return 0;
    for (uint32_t i = 0; i < v->nattr; i++) {
        const VkVertexInputAttributeDescription *a = &v->attr[i]; int hit = 0;
        const uint32_t f = (v->widened >> a->location & 1) ? narrow(a->format) : (uint32_t)a->format;
        for (uint32_t k = 0; k < r->nva && !hit; k++)
            hit = r->va[k][0] == a->location && r->va[k][1] == a->binding && r->va[k][2] == f && r->va[k][3] == a->offset;
        if (!hit) return 0;
    }
    for (uint32_t i = 0; i < v->nbind; i++) {
        const VkVertexInputBindingDescription *b = &v->bind[i]; int hit = 0;
        for (uint32_t k = 0; k < r->nvb && !hit; k++) hit = r->vb[k][0] == b->binding && r->vb[k][1] == b->stride && r->vb[k][2] == (uint32_t)b->inputRate;
        if (!hit) return 0;
    }
    return 1;
}
enum { CANDS = 8 };
static int cands(int kind, const srec *r, uint32_t depth_here, uint32_t n, uint16_t *out) {
    int k = 0;
    for (uint32_t id = 1; id <= n && k < CANDS - 1; id++) {
        const int ok = kind == 0 ? blend_ok(r, (uint16_t)id) : kind == 1 ? ds_ok(r, depth_here, (uint16_t)id) : kind == 2 ? raster_ok(r, (uint16_t)id) : vfmt_ok(r, (uint16_t)id);
        if (ok) out[k++] = (uint16_t)id;
    }
    if (k) return k;   // an engine state of this run matches: id 0 ("no state") only when none does (the draw path's handle-0 case)
    const int ok0 = kind == 0 ? blend_ok(r, 0) : kind == 1 ? ds_ok(r, depth_here, 0) : kind == 2 ? raster_ok(r, 0) : vfmt_ok(r, 0);
    if (ok0) out[k++] = 0;
    return k;
}
static int set_add(uint64_t **a, uint32_t *n, uint32_t *cap, uint64_t v) {
    for (uint32_t i = 0; i < *n; i++) if ((*a)[i] == v) return 0;
    if (*n == *cap) { const uint32_t c = *cap ? *cap * 2 : 1024; uint64_t *x = (uint64_t *)realloc(*a, c * sizeof *x); if (!x) return 0; *a = x; *cap = c; }
    (*a)[(*n)++] = v; return 1;
}
static int set_has(const uint64_t *a, uint32_t n, uint64_t v) { for (uint32_t i = 0; i < n; i++) if (a[i] == v) return 1; return 0; }
static uint64_t keys_fnv(const char *vs, const char *fs) { char b[32]; memset(b, 0, sizeof b); memcpy(b, vs, 16); if (fs[0]) memcpy(b + 16, fs, 16); return vkn_fnv(b, sizeof b); }
// One record for shader s: 1 = translated (records made), 0 = a state not there yet (waits), -1 = refused (counted).
static int translate(srec *r, const vkn_shader *s, uint32_t nb, uint32_t nd, uint32_t nr, uint32_t nv) {
    if (r->key[1][0] && !(s->stages & (1u << VKN_SRC_FS))) { refuse(r, VKN_SEED_R_STAGES); return -1; }
    uint8_t prim;
    switch (r->topo) {
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: prim = 1; break; case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: prim = 3; break;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: prim = 5; break; case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: prim = 6; break;
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: prim = 0; break; default: refuse(r, VKN_SEED_R_PRIM); return -1;
    }
    uint32_t color[8]; memset(color, 0, sizeof color);
    for (int i = 0; i < 8; i++) if (r->color[i] && !(color[i] = fmt_here(r->color[i], 0))) { refuse(r, VKN_SEED_R_FORMAT); return -1; }
    const uint32_t depth = r->has_depth ? fmt_here(r->depth, 1) : 0;
    if (r->has_depth && !depth) { refuse(r, VKN_SEED_R_FORMAT); return -1; }
    uint16_t cb[CANDS], cd[CANDS], cr[CANDS], cv[CANDS];
    const int kb = cands(0, r, depth, nb, cb), kd = cands(1, r, depth, nd, cd), kr = cands(2, r, depth, nr, cr), kv = cands(3, r, depth, nv, cv);
    if (!kb || !kd || !kr || !kv) {   // a state of this record is not in this run (yet): wait - the engine makes its states at load
        g.s.waiting++;
        return 0;
    }
    vkn_warm_rec w; uint32_t made = 0;
    for (int ib = 0; ib < kb; ib++) for (int id = 0; id < kd; id++) for (int ir = 0; ir < kr; ir++) for (int iv = 0; iv < kv; iv++) {
        if (made >= VKN_SEED_COMBOS) { g.s.combos_cut++; goto done; }
        memset(&w, 0, sizeof w);
        memcpy(w.key[0], r->key[0], 16);
        if (r->key[1][0]) { memcpy(w.key[1], r->key[1], 16); w.flags |= VKN_WREC_F_FS; }
        if (r->at_func) { w.alpha_func = r->at_func; memcpy(&w.alpha_ref, &r->at_bits, 4); }
        if (cb[ib]) { if (vkn_blend_args(cb[ib], w.blend)) continue; w.flags |= VKN_WREC_F_BLEND; }
        if (cd[id]) { if (vkn_ds_args(cd[id], w.ds)) continue; w.flags |= VKN_WREC_F_DS; }
        if (cr[ir]) { if (vkn_raster_args(cr[ir], w.raster)) continue; w.flags |= VKN_WREC_F_RASTER; }
        if (cv[iv]) {
            uint32_t ne = 0; const uint8_t *el = vkn_vfmt_elems(cv[iv], &ne);
            if (!el || ne > VKN_WREC_VEL) continue;
            memcpy(w.vel, el, 6u * ne); w.nvel = (uint8_t)ne; w.flags |= VKN_WREC_F_VFMT;
        }
        w.retype = vkn_warm_retype_code(s, cv[iv]);   // this device's rule (the source's #vi, if any, is not this device's)
        memcpy(w.color, color, sizeof color); w.ncolor = r->ncolor; w.depth = depth; w.samples = r->samples;
        w.prim = prim; memcpy(w.write_mask, r->mask, 8);
        // the key's reference matters only with a stencil test on a stencil aspect (vkn_pipe_key_pack): the record's as recorded
        w.stencil_ref = r->stencil_ref;
        for (uint32_t k = 0; k < s->ninputs && k < 16; k++) if (s->inputs[k] < VKN_VFMT_LOCS) w.vs_inputs |= (uint16_t)(1u << s->inputs[k]);
        w.sum = vkn_warm_rec_sum(&w);
        made++; g.s.records_made++;
        set_add(&g.had, &g.nhad, &g.chad, w.sum);
        if (g.mode == 1) { if (vkn_warm_seed_add(&w)) g.s.added++; else g.s.known++; }
    }
done:
    set_add(&g.keys, &g.nkeys, &g.ckeys, keys_fnv(r->key[0], r->key[1]));
    r->state = 1; g.s.translated++;
    return 1;
}

typedef struct { const vkn_shader **s; uint32_t n, cap; } shv;
static int shv_add(const vkn_shader *s, void *ctx) {   // under vkn_shader's mutex: no calls out
    shv *v = (shv *)ctx;
    if (v->n == v->cap) { const uint32_t c = v->cap ? v->cap * 2 : 1024; const vkn_shader **x = (const vkn_shader **)realloc(v->s, c * sizeof *x); if (!x) return 1; v->s = x; v->cap = c; }
    v->s[v->n++] = s;
    return 0;
}
static int keyeq(const char *a, const char *b) { return !strncmp(a, b, 16) && strlen(a) == strlen(b); }
static void pass(void) {
    vkn_state_stats st; memset(&st, 0, sizeof st); vkn_state_stats_get(&st);
    const uint64_t ep = vkn_shader_epoch();
    const uint32_t sig = st.blend_ids * 7919u + st.ds_ids * 104729u + st.raster_ids * 1299709u + st.vfmt_ids;
    if (ep == g.epoch && sig == g.sig) return;   // nothing new to match
    g.epoch = ep; g.sig = sig;
    shv v = {NULL, 0, 0};
    vkn_shader_each(shv_add, &v);
    g.s.waiting = 0;
    for (uint32_t i = 0; i < g.n; i++) {
        srec *r = &g.r[i];
        if (r->state) continue;
        const vkn_shader *s = NULL;
        for (uint32_t j = 0; j < v.n && !s; j++)
            if (v.s[j]->id && keyeq(v.s[j]->key[VKN_SRC_VS], r->key[0]) && keyeq(v.s[j]->key[VKN_SRC_FS], r->key[1])) s = v.s[j];
        if (!s) { g.s.waiting++; continue; }
        translate(r, s, st.blend_ids, st.ds_ids, st.raster_ids, st.vfmt_ids);
    }
    free(v.s);
}

// ---------------------------------------------------------------- the worker's calls
static int mode_get(void) {
    if (g.mode < 0) {
        const char *e = getenv("DM_VK_NATIVE_SEED"); int m = e && *e ? atoi(e) : 1;   // 1 on every family: exact by construction (vkn_seed.h)
        if (m < 0 || m > 2) { vkn_logf(VKN_RED_MARK "seed: DM_VK_NATIVE_SEED=%s is not 0 / 1 / 2: off", e); m = 0; }
        g.mode = m; g.s.mode = m; g.s.file = -2;
        if (!m) vkn_logf("seed: DM_VK_NATIVE_SEED=0: the portable seed is not read");
    }
    return g.mode;
}
void vkn_seed_tick(const char *dir) {
    pthread_mutex_lock(&g_mu);
    if (mode_get() && (g.loaded || dir || g.img)) {
        if (!g.loaded) load(dir);
        if (g.n) pass();
    }
    pthread_mutex_unlock(&g_mu);
}
void vkn_seed_note(const vkn_warm_rec *r) {
    if (!r) return;
    pthread_mutex_lock(&g_mu);
    if (g.mode > 0 && g.s.file == 1) {
        g.s.notes++;
        char vs[17], fs[17]; memcpy(vs, r->key[0], 16); vs[16] = 0; memcpy(fs, r->key[1], 16); fs[16] = 0;
        if (r->rkind || r->fx || r->fsalt) { pthread_mutex_unlock(&g_mu); return; }   // (a technique pipeline: the old path had none of them)
        if (set_has(g.had, g.nhad, r->sum)) g.s.had++;
        else if (set_has(g.keys, g.nkeys, keys_fnv(vs, (r->flags & VKN_WREC_F_FS) ? fs : ""))) {
            g.s.differ++;
            if (g.told_differ++ < 4)
                vkn_logf("seed: an in-frame build (%s / %s, prim %u, %u colours, depth %u, blend %u ds %u raster %u vertex elements %u) has a seed record of the same shaders with another content",
                         vs, fs, r->prim, r->ncolor, r->depth, (r->flags & VKN_WREC_F_BLEND) != 0, (r->flags & VKN_WREC_F_DS) != 0, (r->flags & VKN_WREC_F_RASTER) != 0, r->nvel);
        } else g.s.unknown++;
    }
    pthread_mutex_unlock(&g_mu);
}
void vkn_seed_stats_get(vkn_seed_stats *s) { if (!s) return; pthread_mutex_lock(&g_mu); *s = g.s; pthread_mutex_unlock(&g_mu); }
void vkn_seed_census(void) {
    vkn_seed_stats s; vkn_seed_stats_get(&s);
    if (s.mode <= 0 || s.file != 1) return;
    char by[640]; int k = 0; by[0] = 0;
    for (int i = 0; i < VKN_SEED_R_N && k < (int)sizeof by - 96; i++) if (s.refused[i]) k += snprintf(by + k, sizeof by - (size_t)k, "%s%s %u", k ? ", " : "", vkn_seed_reason_name(i), s.refused[i]);
    vkn_logf("seed: DM_VK_NATIVE_SEED=%d, seed %016llx: %u records: translated %u (list records %u: new %u, the device's list had %u; combinations cut at %d: %u), "
             "waiting for their shader / states %u, refused: %s; in-frame builds since start %llu: the seed had %llu, a seed record of the same shaders differs %llu, "
             "shaders the seed has no record of %llu", s.mode, (unsigned long long)s.id, s.records, s.translated, s.records_made, s.added, s.known, VKN_SEED_COMBOS,
             s.combos_cut, s.waiting, k ? by : "none", (unsigned long long)s.notes, (unsigned long long)s.had, (unsigned long long)s.differ, (unsigned long long)s.unknown);
}
void vkn_seed_test_setup(int mode, const uint8_t *seed, size_t bytes) {
    pthread_mutex_lock(&g_mu);
    free(g.r); free(g.had); free(g.keys); free(g.img);
    memset(&g, 0, sizeof g);
    g.mode = mode; g.s.mode = mode; g.s.file = -2;
    if (seed && bytes && (g.img = (uint8_t *)malloc(bytes))) { memcpy(g.img, seed, bytes); g.nimg = bytes; }
    pthread_mutex_unlock(&g_mu);
}
#endif
