// [DOS2] VK-NATIVE stage 5 rest: the portable warm-up seed on the backend (vkn_seed.h has the scheme, the exactness argument, the
// switch). Runs on the warm-up worker only (vkn_warm.c calls vkn_seed_tick / _note / _census); the census line may come from the
// render thread (counters copied under the mutex). No game addresses.
#ifdef DM_GAME_DOS2
#include "vkn_seed.h"
#include "vkn.h"
#include "vkn_shader.h"
#include "vkn_state.h"
#include "vkn_tex.h"    // [VK-SEED] vkn_tex_rt_format
#include "../nr/nr_seed.h"
#include <stddef.h>
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
    // [VK-SEED] the new path's seed (vkn_seed.h): its records (portable form), their states, the lookup tables
    vkn_warm_rec *v; uint8_t *vst; uint32_t nv;
    uint64_t *vid; uint32_t vidcap;   // identities (+ the D32 form of an EITHER record): open addressing, 0 = empty
    uint64_t *vkp; uint32_t vkpcap;   // fnv of the key pairs (the "same shaders" answer)
    uint32_t *vhead, *vnext; uint32_t vhcap;   // key pair -> records (chains; UINT32_MAX = end)
} g = {.mode = -1};

const char *vkn_seed_reason_name(int r) {
    static const char *const n[VKN_SEED_R_N] = {"not a seed record", "not pack keys", "a fused post stage (the backend's own)", "a vertex conversion variant (#vi..c)",
        "a create-info shape the old path never wrote", "a pass format this device cannot render to", "a topology the engine does not draw", "not one vertex + at most one fragment stage",
        "no blend state of this run matches", "no depth-stencil state of this run matches", "no raster state of this run matches", "no vertex format of this run matches",
        "a torn / foreign record (checksum, shape)", "the vertex format table is full"};
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
// The seed file `name` from the shader directory (a test push) or files/ (the APK's copy). NULL = none.
static uint8_t *read_seed_file(const char *name, size_t *n, char *path, size_t pn) {
    char sd[512]; const char *cand[2] = {NULL, NULL}; char p0[600], p1[600]; uint8_t *data = NULL;
    *n = 0;
    if (vkn_shlib_shader_dir && !vkn_shlib_shader_dir(sd, sizeof sd) && sd[0]) { snprintf(p0, sizeof p0, "%s/%s", sd, name); cand[0] = p0; }
    const char *fd = getenv("DM_MARKER_DIR");
    if (fd && *fd) { snprintf(p1, sizeof p1, "%s/%s", fd, name); cand[1] = p1; }
    for (int i = 0; i < 2 && !data; i++) {
        if (!cand[i]) continue;
        FILE *f = fopen(cand[i], "rb"); if (!f) continue;
        if (!fseek(f, 0, SEEK_END)) {
            const long sz = ftell(f);
            if (sz > 32 && sz < (64l << 20) && !fseek(f, 0, SEEK_SET) && (data = (uint8_t *)malloc((size_t)sz))) {
                if (fread(data, 1, (size_t)sz, f) == (size_t)sz) { *n = (size_t)sz; snprintf(path, pn, "%s", cand[i]); }
                else { free(data); data = NULL; }
            }
        }
        fclose(f);
    }
    return data;
}
static int load2(const uint8_t *data, size_t n, const char *path);
static void load(const char *dir) {
    uint8_t *data = NULL; size_t n = 0; char path[640] = "";
    (void)dir;
    g.loaded = 1;
    if (g.img) { data = g.img; n = g.nimg; snprintf(path, sizeof path, "(test image)"); }
    else if ((data = read_seed_file(VKN_SEED2_FILE, &n, path, sizeof path))) {   // [VK-SEED] the new path's seed first
        const int ok = load2(data, n, path); free(data); data = NULL;
        if (ok) return;
        data = read_seed_file("dos2-pipe-seed.bin", &n, path, sizeof path);
    } else data = read_seed_file("dos2-pipe-seed.bin", &n, path, sizeof path);
    if (data && n >= 8 && !memcmp(data, "VKNSEED", 8)) {   // (a test image of the new seed)
        load2(data, n, path); if (data != g.img) free(data); return;
    }
    if (!data) { g.s.file = 0; vkn_logf("seed: DM_VK_NATIVE_SEED=%d: no " VKN_SEED2_FILE " / dos2-pipe-seed.bin (shader directory, files/): nothing to do", g.mode); return; }
    nsd_seed_hdr h;
    if (nsd_seed_open(data, n, &h)) { g.s.file = -1; vkn_logf(VKN_RED_MARK "seed: %s is not a version-%u seed or is damaged: ignored", path, NSD_SEED_VERSION); if (data != g.img) free(data); return; }
    g.s.file = 1; g.s.id = h.id; g.s.kind = 1;
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

// ---------------------------------------------------------------- [VK-SEED] the new path's seed (vkn_seed.h)
// vkn_warm_rec offsets the tool writes by number (tools/dos2_vkn_seed.py O_*): a layout change must change the tool too
_Static_assert(sizeof(vkn_warm_rec) == 296 && offsetof(vkn_warm_rec, retype) == 32 && offsetof(vkn_warm_rec, depth) == 80 && offsetof(vkn_warm_rec, nvel) == 87 &&
               offsetof(vkn_warm_rec, vel) == 122 && offsetof(vkn_warm_rec, pad3) == 227 && offsetof(vkn_warm_rec, piece) == 230 && offsetof(vkn_warm_rec, rarg) == 240 &&
               offsetof(vkn_warm_rec, sum) == 288, "vkn_warm_rec layout = tools/dos2_vkn_seed.py");
static uint64_t hs_key(uint64_t h) { return h ? h : 1; }
static void hs_put(uint64_t *t, uint32_t cap, uint64_t h) {   // cap: a power of two, never full (sized at load)
    h = hs_key(h);
    for (uint32_t i = (uint32_t)h & (cap - 1);; i = (i + 1) & (cap - 1)) { if (t[i] == h) return; if (!t[i]) { t[i] = h; return; } }
}
static int hs_has(const uint64_t *t, uint32_t cap, uint64_t h) {
    if (!t) return 0;
    h = hs_key(h);
    for (uint32_t i = (uint32_t)h & (cap - 1), k = 0; k < cap; i = (i + 1) & (cap - 1), k++) { if (t[i] == h) return 1; if (!t[i]) return 0; }
    return 0;
}
static uint32_t pow2_at_least(uint32_t n) { uint32_t c = 16; while (c < n) c <<= 1; return c; }
static int is_mat_recipe(const vkn_warm_rec *r) { return r->rkind == VKN_WREC_R_VS && r->rlen == 24 && !strncmp(r->piece, "mat", VKN_WARM_PIECE); }
static uint32_t d32_of(uint32_t f) { return f == VK_FORMAT_X8_D24_UNORM_PACK32 ? VK_FORMAT_D32_SFLOAT : f == VK_FORMAT_D24_UNORM_S8_UINT ? VK_FORMAT_D32_SFLOAT_S8_UINT : 0; }
static uint32_t d24_of(uint32_t f) { return f == VK_FORMAT_D32_SFLOAT ? VK_FORMAT_X8_D24_UNORM_PACK32 : f == VK_FORMAT_D32_SFLOAT_S8_UINT ? VK_FORMAT_D24_UNORM_S8_UINT : 0; }
// The portable identity (the tool's ident(canon(r))): retype and a "mat" recipe's code word cleared, the seed flags cleared.
static uint64_t ident_of(const vkn_warm_rec *r, uint32_t depth) {
    vkn_warm_rec x = *r;
    x.retype = 0; x.pad3 = 0; x.depth = depth;
    if (is_mat_recipe(&x)) memset(x.rarg, 0, 8);
    return vkn_fnv(&x, offsetof(vkn_warm_rec, sum));
}
// The key pair of a record / of an engine shader (vkn_warm.c keys_eq: a record without a fragment stage = a shader without one).
static uint64_t kp_rec(const vkn_warm_rec *r) { char b[32]; memset(b, 0, sizeof b); memcpy(b, r->key[0], 16); if (r->flags & VKN_WREC_F_FS) memcpy(b + 16, r->key[1], 16); return vkn_fnv(b, 32); }
static int kp_shader(const vkn_shader *s, uint64_t *h) {
    char b[32]; memset(b, 0, sizeof b);
    if (strlen(s->key[VKN_SRC_VS]) != 16) return -1;
    memcpy(b, s->key[VKN_SRC_VS], 16);
    if (s->key[VKN_SRC_FS][0]) { if (strlen(s->key[VKN_SRC_FS]) != 16) return -1; memcpy(b + 16, s->key[VKN_SRC_FS], 16); }
    *h = vkn_fnv(b, 32); return 0;
}
static int keys_match(const vkn_shader *s, const vkn_warm_rec *r) {
    if (memcmp(s->key[VKN_SRC_VS], r->key[0], 16)) return 0;
    if (r->flags & VKN_WREC_F_FS) return strlen(s->key[VKN_SRC_FS]) == 16 && !memcmp(s->key[VKN_SRC_FS], r->key[1], 16);
    return !s->key[VKN_SRC_FS][0];
}
static int shape_ok(const vkn_warm_rec *r) {
    if (r->nvel > VKN_WREC_VEL || r->ncolor > 8 || r->rlen > VKN_WARM_ARG || r->rkind > VKN_WREC_R_VS || (r->pad3 & ~VKN_SEED2_EITHER)) return 0;
    if ((r->pad3 & VKN_SEED2_EITHER) && !d32_of(r->depth)) return 0;
    return 1;
}
// 1 = read (g.s.kind 2), 0 = not a usable new seed (the caller falls back to the old file).
static int load2(const uint8_t *data, size_t n, const char *path) {
    uint32_t ver = 0, rs = 0, nrec = 0, nsrc = 0; uint64_t id = 0;
    if (n >= VKN_SEED2_HDR) { memcpy(&ver, data + 8, 4); memcpy(&rs, data + 12, 4); memcpy(&nrec, data + 16, 4); memcpy(&nsrc, data + 20, 4); memcpy(&id, data + 24, 8); }
    if (n < VKN_SEED2_HDR || memcmp(data, "VKNSEED", 8) || ver != VKN_SEED2_VERSION || rs != sizeof(vkn_warm_rec) || nrec > (1u << 20) ||
        n != VKN_SEED2_HDR + (size_t)nrec * rs || vkn_fnv(data + VKN_SEED2_HDR, n - VKN_SEED2_HDR) != id) {
        g.s.file = -1;
        vkn_logf(VKN_RED_MARK "seed: %s is not a version-%u " VKN_SEED2_FILE " or is damaged: ignored (the old seed is read if there is one)", path, VKN_SEED2_VERSION);
        return 0;
    }
    g.v = (vkn_warm_rec *)calloc(nrec ? nrec : 1, sizeof *g.v); g.vst = (uint8_t *)calloc(nrec ? nrec : 1, 1);
    g.vidcap = pow2_at_least(nrec * 4 + 16); g.vkpcap = pow2_at_least(nrec * 2 + 16); g.vhcap = g.vkpcap;
    g.vid = (uint64_t *)calloc(g.vidcap, 8); g.vkp = (uint64_t *)calloc(g.vkpcap, 8);
    g.vhead = (uint32_t *)malloc(g.vhcap * 4u); g.vnext = (uint32_t *)malloc((nrec ? nrec : 1) * 4u);
    if (!g.v || !g.vst || !g.vid || !g.vkp || !g.vhead || !g.vnext) { g.s.file = -1; g.nv = 0; vkn_logf(VKN_RED_MARK "seed: no memory for %u records: off", nrec); return 1; }
    memset(g.vhead, 0xff, g.vhcap * 4u);
    g.s.file = 1; g.s.kind = 2; g.s.id = id;
    for (uint32_t i = 0; i < nrec; i++) {
        vkn_warm_rec r; memcpy(&r, data + VKN_SEED2_HDR + (size_t)i * rs, sizeof r);
        if (r.sum != vkn_warm_rec_sum(&r) || !shape_ok(&r)) { g.s.refused[VKN_SEED_R_SUM]++; continue; }
        const uint32_t k = g.nv++;
        g.v[k] = r;
        hs_put(g.vid, g.vidcap, ident_of(&r, r.depth));
        if (r.pad3 & VKN_SEED2_EITHER) { hs_put(g.vid, g.vidcap, ident_of(&r, d32_of(r.depth))); g.s.either++; }
        const uint64_t kp = kp_rec(&r);
        hs_put(g.vkp, g.vkpcap, kp);
        const uint32_t b = (uint32_t)kp & (g.vhcap - 1); g.vnext[k] = g.vhead[b]; g.vhead[b] = k;
    }
    g.s.records = g.nv;
    char src[96] = ""; int k = 0;
    for (uint32_t i = 0; i < nsrc && i < 3 && k < (int)sizeof src - 24; i++) { uint32_t ven, dev; memcpy(&ven, data + 40 + 8 * i, 4); memcpy(&dev, data + 44 + 8 * i, 4); k += snprintf(src + k, sizeof src - (size_t)k, "%s%#x/%#x", i ? ", " : "", ven, dev); }
    vkn_logf("seed: DM_VK_NATIVE_SEED=%d (VK-SEED: %s): %s, the new path's seed %016llx, %u records (%u EITHER, %u refused while reading) from %u list(s) (%s); "
             "each gets this device's formats / retype code when its shader exists", g.mode, g.mode == 1 ? "translated + built by the warm-up worker" : "verify - translated and counted, nothing built",
             path, (unsigned long long)id, g.nv, g.s.either, g.s.refused[VKN_SEED_R_SUM], nsrc, src[0] ? src : "-");
    return 1;
}
// Record i for shader s on this device: 1 translated (list records made), -1 refused.
static int translate2(uint32_t i, const vkn_shader *s) {
    vkn_warm_rec w = g.v[i];
    const int either = (w.pad3 & VKN_SEED2_EITHER) != 0;
    w.pad3 = 0;
    if ((w.flags & VKN_WREC_F_FS) && !(s->stages & (1u << VKN_SRC_FS))) { refuse(NULL, VKN_SEED_R_STAGES); return -1; }
    for (int k = 0; k < 8; k++) if (w.color[k] && !(w.color[k] = vkn_tex_rt_format(w.color[k], 0))) { refuse(NULL, VKN_SEED_R_FORMAT); return -1; }
    uint32_t dv[2] = {0, 0}; int nd = 1;
    if (w.depth) {
        if (!(dv[0] = vkn_tex_rt_format(w.depth, 1))) { refuse(NULL, VKN_SEED_R_FORMAT); return -1; }
        if (either) { const uint32_t d1 = vkn_tex_rt_format(d32_of(w.depth), 1); if (d1 && d1 != dv[0]) dv[nd++] = d1; }
    }
    if (w.rkind != VKN_WREC_R_FX) {   // this device's retype code for the shader + vertex format (the draw path's rule); a piece object has none
        uint16_t vf = 0;
        if (w.flags & VKN_WREC_F_VFMT) { vf = vkn_vfmt_put(0, w.vel, w.nvel); if (!vf) { refuse(NULL, VKN_SEED_R_VTAB); return -1; } }
        w.retype = vkn_warm_retype_code(s, vf);
        if (is_mat_recipe(&w)) memcpy(w.rarg, &w.retype, 8);
    } else w.retype = 0;
    for (int k = 0; k < nd; k++) {
        w.depth = dv[k];
        w.sum = vkn_warm_rec_sum(&w);
        g.s.records_made++;
        set_add(&g.had, &g.nhad, &g.chad, w.sum);
        if (g.mode == 1) { if (vkn_warm_seed_add(&w)) g.s.added++; else g.s.known++; }
    }
    g.s.translated++;
    return 1;
}
static void pass2(void) {
    const uint64_t ep = vkn_shader_epoch();
    if (ep == g.epoch) return;   // no new shader
    g.epoch = ep;
    shv v = {NULL, 0, 0};
    vkn_shader_each(shv_add, &v);
    for (uint32_t j = 0; j < v.n; j++) {
        const vkn_shader *s = v.s[j]; uint64_t kp;
        if (!s->id || kp_shader(s, &kp)) continue;
        for (uint32_t i = g.vhead[(uint32_t)kp & (g.vhcap - 1)]; i != UINT32_MAX; i = g.vnext[i]) {
            if (g.vst[i] || kp_rec(&g.v[i]) != kp || !keys_match(s, &g.v[i])) continue;
            g.vst[i] = translate2(i, s) > 0 ? 1 : 2;
        }
    }
    uint32_t w = 0; for (uint32_t i = 0; i < g.nv; i++) w += !g.vst[i];
    g.s.waiting = w;
    free(v.s);
}
// Mode 1 / 2: an in-frame build of this device against the seed (portable identity; this device's D24 stand-ins also as the D24 form).
static void note2(const vkn_warm_rec *r) {
    const int tech = r->rkind || r->fx || r->fsalt;
    g.s.notes++; if (tech) g.s.tech_notes++;
    int hit = hs_has(g.vid, g.vidcap, ident_of(r, r->depth));
    if (!hit) { const uint32_t d24 = d24_of(r->depth); if (d24 && vkn_tex_rt_format(d24, 1) == r->depth) hit = hs_has(g.vid, g.vidcap, ident_of(r, d24)); }
    if (hit) { g.s.had++; if (tech) g.s.tech_had++; return; }
    if (hs_has(g.vkp, g.vkpcap, kp_rec(r))) {
        g.s.differ++;
        if (g.told_differ++ < 4)
            vkn_logf("seed: an in-frame build (%.16s / %.16s, prim %u, %u colours, depth %u, blend %u ds %u raster %u vertex elements %u, fx %u, piece %.10s) has a seed record of the same shaders with another content",
                     r->key[0], (r->flags & VKN_WREC_F_FS) ? r->key[1] : "-", r->prim, r->ncolor, r->depth, (r->flags & VKN_WREC_F_BLEND) != 0, (r->flags & VKN_WREC_F_DS) != 0,
                     (r->flags & VKN_WREC_F_RASTER) != 0, r->nvel, r->fx, r->rkind ? r->piece : "-");
    } else g.s.unknown++;
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
        if (g.s.kind == 2) { if (g.nv) pass2(); }   // [VK-SEED]
        else if (g.n) pass();
    }
    pthread_mutex_unlock(&g_mu);
}
void vkn_seed_note(const vkn_warm_rec *r) {
    if (!r) return;
    pthread_mutex_lock(&g_mu);
    if (g.mode > 0 && g.s.file == 1 && g.s.kind == 2) note2(r);   // [VK-SEED]
    else if (g.mode > 0 && g.s.file == 1) {
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
    if (s.kind == 2) {   // [VK-SEED] (two lines: the 1 024-byte log line)
        vkn_logf("seed: DM_VK_NATIVE_SEED=%d, new-path seed %016llx: %u records: translated %u (list records %u: new %u, the device's list had %u), waiting for their shader %u, "
                 "refused: %s", s.mode, (unsigned long long)s.id, s.records, s.translated, s.records_made, s.added, s.known, s.waiting, k ? by : "none");
        vkn_logf("seed: in-frame builds since start %llu (technique %llu): the seed had %llu (technique %llu)%s, a seed record of the same shaders differs %llu, "
                 "shaders the seed has no record of %llu; coverage %.1f %%", (unsigned long long)s.notes, (unsigned long long)s.tech_notes, (unsigned long long)s.had,
                 (unsigned long long)s.tech_had, s.mode == 1 ? " - built by the render thread anyway: the worker was late" : "", (unsigned long long)s.differ,
                 (unsigned long long)s.unknown, s.notes ? 100.0 * (double)s.had / (double)s.notes : 0.0);
        return;
    }
    vkn_logf("seed: DM_VK_NATIVE_SEED=%d, seed %016llx: %u records: translated %u (list records %u: new %u, the device's list had %u; combinations cut at %d: %u), "
             "waiting for their shader / states %u, refused: %s; in-frame builds since start %llu: the seed had %llu, a seed record of the same shaders differs %llu, "
             "shaders the seed has no record of %llu", s.mode, (unsigned long long)s.id, s.records, s.translated, s.records_made, s.added, s.known, VKN_SEED_COMBOS,
             s.combos_cut, s.waiting, k ? by : "none", (unsigned long long)s.notes, (unsigned long long)s.had, (unsigned long long)s.differ, (unsigned long long)s.unknown);
}
void vkn_seed_test_setup(int mode, const uint8_t *seed, size_t bytes) {
    pthread_mutex_lock(&g_mu);
    free(g.r); free(g.had); free(g.keys); free(g.img);
    free(g.v); free(g.vst); free(g.vid); free(g.vkp); free(g.vhead); free(g.vnext);
    memset(&g, 0, sizeof g);
    g.mode = mode; g.s.mode = mode; g.s.file = -2;
    if (seed && bytes && (g.img = (uint8_t *)malloc(bytes))) { memcpy(g.img, seed, bytes); g.nimg = bytes; }
    pthread_mutex_unlock(&g_mu);
}
#endif
