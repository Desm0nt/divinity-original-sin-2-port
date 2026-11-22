// [DOS2] VK-SHADOWS-MOVING: caster bounds, the switches and the layer piece (vkn_shs.h has the rules). Recording thread only, except
// the install and vkn_shs_cam_block (install time). No game addresses.
#ifdef DM_GAME_DOS2
#include "vkn_shs.h"
#include "vkn_buf.h"
#include "vkn_draw.h"
#include "vkn_drv.h"
#include "vkn_frame.h"
#include "vkn_rt.h"
#include "vkn_shader.h"
#include "vkn_state.h"
#include "vkn_tex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
extern int vkn_shlib_on(void) __attribute__((weak));
extern const nr_sb_prog *vkn_shlib_vs_posl(const void *shader) __attribute__((weak));
extern const vkn_draw_in *vkn_draw_rt_in;

enum { BOX_N = 8192, BOX_WAYS = 4 };   // (Fort Joy draws ~1500 meshes into the atlas: a bucket of 4 overflows in < 0.1 % of the buckets)
typedef struct box_e { uint64_t k1, k2; nr_sb_box box; uint32_t ok, used; } box_e;   // k1 / k2: two hashes of the mesh key; used: the frame it was last read in
static struct {
    int inst, cull, scroll, scroll_z; double eps; uint32_t scan;
    int cam, cam_slot; uint32_t vp_off, view_off;
    box_e *box; uint64_t frame; uint32_t used;   // indices scanned in `frame`
    const nr_sb_prog *(*t_prog)(const vkn_shader *s);
    const vkn_shader *piece; int piece_bad; VkShaderModule vs, fs; uint16_t ds;
    vkn_shs_stats s;
} g;
static const char *const k_why[VKN_SHS_N] = {"known", "no affine position program", "primitive", "position format", "constant block", "w is not 1",
                                             "buffer", "version moved", "scan budget", "mesh too big"};
const char *vkn_shs_why_name(int w) { return w >= 0 && w < VKN_SHS_N ? k_why[w] : "?"; }
int vkn_shs_cull(void) { return g.cull; }
int vkn_shs_scroll(void) { return g.scroll; }
double vkn_shs_eps(void) { return g.eps; }
int vkn_shs_scroll_z(void) { return g.scroll_z; }
void vkn_shs_cam_block(uint32_t slot, uint32_t vp_off, uint32_t view_off) { g.cam = 1; g.cam_slot = (int)slot; g.vp_off = vp_off; g.view_off = view_off; }
int vkn_shs_cam(uint32_t *slot, uint32_t *vp_off, uint32_t *view_off) { if (!g.cam) return 0; *slot = (uint32_t)g.cam_slot; *vp_off = g.vp_off; *view_off = g.view_off; return 1; }
void vkn_shs_stats_get(vkn_shs_stats *s) { if (s) { *s = g.s; s->box_bytes = g.box ? (uint64_t)BOX_N * sizeof(box_e) : 0; } }
void vkn_shs_stats_reset(void) { memset(&g.s, 0, sizeof g.s); }
void vkn_shs_test_prog(const nr_sb_prog *(*fn)(const vkn_shader *s)) { g.t_prog = fn; }
void vkn_shs_test_modes(int cull, int scroll) { g.cull = cull; g.scroll = scroll; g.inst = 1; if (!(g.eps > 0)) g.eps = 1.0 / 64; if (!g.scan) g.scan = 400000; }

void vkn_shs_install(void) {
    if (g.inst) return;
    g.inst = 1;
    const char *c = getenv("DM_VK_NATIVE_SHCULL"), *s = getenv("DM_VK_NATIVE_SHSCROLL"), *e = getenv("DM_VK_NATIVE_SHSCROLL_EPS"), *b = getenv("DM_VK_NATIVE_SHS_SCAN");
    int cm = c && *c ? atoi(c) : 1, sm = s && *s ? atoi(s) : 1;   // [22.11] default 1 after the phone (series C: C3 verify, C4 / C6 not slower)
    if (cm < 0 || cm > 3) { vkn_logf(RED "shs: DM_VK_NATIVE_SHCULL=%s is not 0..3: 0", c); cm = 0; }
    if (sm < 0 || sm > 3) { vkn_logf(RED "shs: DM_VK_NATIVE_SHSCROLL=%s is not 0..3: 0", s); sm = 0; }
    { const char *z = getenv("DM_VK_NATIVE_SHSCROLL_Z"); g.scroll_z = z && *z == '1'; }   // [fix 22.11]
    int n = e && *e ? atoi(e) : 64;
    if (n < 4) n = 4;
    if (n > 4096) n = 4096;
    g.eps = 1.0 / n; g.scan = b && *b ? (uint32_t)atol(b) : 400000u;
    g.cull = cm; g.scroll = sm;
    vkn_logf("DM_VK_NATIVE_SHCULL=%d (shadows-moving: %s), DM_VK_NATIVE_SHSCROLL=%d (%s), DM_VK_NATIVE_SHSCROLL_Z=%d (%s), tolerance 1/%d px, box scan budget %u indices a frame, per-view block %s (vertex slot %d, ViewProjection at byte %u)",
             cm, cm == 0 ? "off" : cm == 1 ? "casters outside their cascade tile are skipped" : cm == 2 ? "counted only" : "POISON - the tile shrunk by 64 px",
             sm, sm == 0 ? "off" : sm == 1 ? "the static layer scrolls with the cascade, only the exposed strips are drawn again" : sm == 2 ? "verify - everything drawn, the would-be tile compared" :
             "POISON - the scrolled layer is not drawn", g.scroll_z, g.scroll_z ? "a layer follows the cascade's z too - NOT exact, for measurements" : "a layer whose cascade moved in z is dropped",
             n, g.scan, g.cam ? "given" : "NOT given (no scrolling)", g.cam_slot, g.vp_off);
}

static const nr_sb_prog *prog_of(const vkn_shader *sh) {
    if (!sh || vkn_fx_shader_is(sh)) return NULL;
    if (g.t_prog) return g.t_prog(sh);
    if (vkn_shlib_on && vkn_shlib_on() && vkn_shlib_vs_posl) return vkn_shlib_vs_posl(sh->xs[VKN_SRC_VS]);
    return NULL;
}
// The blocks the program reads: by the engine's announces (ub == NULL) or a kept draw's list.
static int prog_ubos(const nr_sb_prog *pr, const vkn_shader *sh, const vkn_shs_ub *ub, uint32_t nub, int cam_slot, uint32_t cam_from, uint32_t cam_to,
                     const uint8_t *p[NR_SB_MAX_UBO], size_t n[NR_SB_MAX_UBO]) {
    const uint8_t *ring = (const uint8_t *)vkn_ring_cpu();
    for (uint32_t u = 0; u < pr->nubo; u++) {
        const vkn_bind *b = NULL;
        for (uint32_t k = 0; k < sh->nbind; k++) if (sh->bind[k].binding == pr->ubo[u] && sh->bind[k].stage == 0) b = &sh->bind[k];
        if (!b) return -1;
        if (b->kind == VKN_BK_UNIFORM) {
            uint32_t off = 0, size = 0; int have = 0;
            if (ub) { for (uint32_t i = 0; i < nub; i++) if (ub[i].st == 0 && ub[i].slot == b->slot) { off = ub[i].off; size = ub[i].size; have = 1; } }
            else have = !vkn_draw_ubo_get(0, b->slot, &off, &size);
            if (!have || !size || !ring || (uint64_t)off + size > VKN_RING_BYTES) return -1;
            if (cam_slot >= 0 && (int)b->slot == cam_slot && off == cam_from) off = cam_to;
            p[u] = ring + off; n[u] = b->bytes && b->bytes < size ? b->bytes : size;
        } else if (b->kind == VKN_BK_C1 && sh->c1[VKN_SRC_VS].size && sh->c1[VKN_SRC_VS].cpu) {
            p[u] = (const uint8_t *)sh->c1[VKN_SRC_VS].cpu; n[u] = (size_t)sh->c1[VKN_SRC_VS].size;
        } else return -1;
        if (n[u] < pr->ubo_bytes[u]) return -1;
    }
    return 0;
}
int vkn_shs_forms_kept(const vkn_shader *sh, const vkn_shs_ub *ub, uint32_t nub, int cam_slot, uint32_t cam_from, uint32_t cam_to, nr_sb_form f[4]) {
    const nr_sb_prog *pr = prog_of(sh);
    const uint8_t *p[NR_SB_MAX_UBO]; size_t n[NR_SB_MAX_UBO];
    if (!pr || !ub || prog_ubos(pr, sh, ub, nub, cam_slot, cam_from, cam_to, p, n)) return 0;
    return nr_sb_eval(pr, p, n, 1.0f, f);
}

static float half_f(uint16_t h) {
    const uint32_t s = (uint32_t)(h >> 15) << 31, e = h >> 10 & 31, m = h & 1023; uint32_t u;
    if (e == 0) {
        if (!m) u = s;
        else { int k = 0; uint32_t mm = m; while (!(mm & 1024)) { mm <<= 1; k++; } u = s | (uint32_t)(113 - k) << 23 | (mm & 1023) << 13; }
    } else if (e == 31) u = s | 0x7f800000u | m << 13;
    else u = s | (e + 112) << 23 | m << 13;
    float f; memcpy(&f, &u, 4); return f;
}
// The bounding box of the vertices a draw fetches at Location 0. VKN_SHS_*.
static int mesh_box(const vkn_draw_in *in, const vkn_fx_ctx *ctx, const VkVertexInputAttributeDescription *a, uint32_t stride, const nr_sb_box **out) {
    const int half = a->format == VK_FORMAT_R16G16B16_SFLOAT || a->format == VK_FORMAT_R16G16B16A16_SFLOAT;
    const uint32_t st = a->binding, voff = in->vb_off[st] + a->offset, esz = half ? 6u : 12u;
    const vkn_draw_ver *pv = vkn_draw_rt_ver && vkn_draw_rt_in == in && vkn_draw_rt_ver->ok ? vkn_draw_rt_ver : NULL;
    int vnone = 1, inone = 0; uint64_t vver, iver = 0;
    if (pv) { vver = pv->vb[st]; vnone = pv->vnone >> st & 1; } else vver = vkn_buf_version(VKN_BUF_VB, in->vb[st], &vnone);
    if (ctx->indexed) { inone = 1; if (pv) { iver = pv->ib; inone = pv->inone; } else iver = vkn_buf_version(VKN_BUF_IB, in->ib, &inone); }
    if (vnone || inone) return VKN_SHS_VERSION;
    if (!g.box && !(g.box = (box_e *)calloc(BOX_N, sizeof *g.box))) return VKN_SHS_BUFFER;
    uint64_t h = nr_ss_mix(nr_ss_mix(in->vb[st], (uint64_t)voff | (uint64_t)stride << 32), ctx->indexed ? in->ib : 0);
    h = nr_ss_mix(h, (uint64_t)ctx->first | (uint64_t)ctx->count << 32);
    const uint64_t k1 = nr_ss_mix(nr_ss_mix(h, vver), iver) | 1u, k2 = nr_ss_mix(nr_ss_mix(nr_ss_mix(h ^ 0x9E3779B97F4A7C15ull, iver), vver), (uint64_t)a->format | (uint64_t)ctx->indexed << 32);
    box_e *bk = &g.box[((h >> 40) & (BOX_N / BOX_WAYS - 1)) * BOX_WAYS], *e = NULL;
    const uint64_t fr = vkn_frame();
    for (int w = 0; w < BOX_WAYS; w++) if (bk[w].ok && bk[w].k1 == k1 && bk[w].k2 == k2) { bk[w].used = (uint32_t)fr; g.s.box_hit++; *out = &bk[w].box; return VKN_SHS_OK; }
    for (int w = 0; w < BOX_WAYS; w++) if (!e || !bk[w].ok || (e->ok && (uint32_t)fr - bk[w].used > (uint32_t)fr - e->used)) { e = &bk[w]; if (!e->ok) break; }   // an empty way, else the one read longest ago
    if (ctx->count > (1u << 20)) return VKN_SHS_BIG;
    if (fr != g.frame) { g.frame = fr; g.used = 0; }
    if (g.used && g.used + ctx->count > g.scan) return VKN_SHS_BUDGET;   // (the frame's first scan always runs)
    vkn_buf_info vi, ii; memset(&vi, 0, sizeof vi); memset(&ii, 0, sizeof ii);
    if (vkn_buf_lookup(VKN_BUF_VB, in->vb[st], &vi) || !vi.blk.cpu) return VKN_SHS_BUFFER;
    uint32_t isz = 0;
    if (ctx->indexed) { if (vkn_buf_lookup(VKN_BUF_IB, in->ib, &ii) || !ii.blk.cpu) return VKN_SHS_BUFFER; isz = ii.index_size == 4 ? 4 : 2; }
    {   // the blocks just looked up hold the versions read AFTER the lookup (versions only grow): they must be the draw's
        int n1 = 1, n2 = 0; const uint64_t v1 = vkn_buf_version(VKN_BUF_VB, in->vb[st], &n1), v2 = ctx->indexed ? vkn_buf_version(VKN_BUF_IB, in->ib, &n2) : 0;
        if (n1 || n2 || v1 != vver || v2 != iver) return VKN_SHS_VERSION;
    }
    nr_sb_box bx = {{3.0e38f, 3.0e38f, 3.0e38f}, {-3.0e38f, -3.0e38f, -3.0e38f}};
    const uint8_t *vp = (const uint8_t *)vi.blk.cpu, *ip = (const uint8_t *)ii.blk.cpu;
    if (!ctx->count || (ctx->indexed && ((uint64_t)ctx->first + ctx->count) * isz > ii.size)) return VKN_SHS_BUFFER;
    for (uint32_t k = 0; k < ctx->count; k++) {
        uint64_t v;
        if (ctx->indexed) {
            const uint8_t *q = ip + ((uint64_t)ctx->first + k) * isz;
            if (isz == 2) { uint16_t x; memcpy(&x, q, 2); v = x; } else { uint32_t x; memcpy(&x, q, 4); v = x; }
        } else v = (uint64_t)ctx->first + k;
        const uint64_t at = (uint64_t)voff + v * stride;
        if (at + esz > vi.size) return VKN_SHS_BUFFER;
        float p[3];
        if (half) { uint16_t hh[3]; memcpy(hh, vp + at, 6); p[0] = half_f(hh[0]); p[1] = half_f(hh[1]); p[2] = half_f(hh[2]); } else memcpy(p, vp + at, 12);
        for (int c = 0; c < 3; c++) {
            if (!(p[c] == p[c])) return VKN_SHS_BUFFER;
            if (p[c] < bx.lo[c]) bx.lo[c] = p[c];
            if (p[c] > bx.hi[c]) bx.hi[c] = p[c];
        }
    }
    g.used += ctx->count; g.s.box_scan++; g.s.box_idx += ctx->count;
    e->k1 = k1; e->k2 = k2; e->box = bx; e->ok = 1; e->used = (uint32_t)fr;
    *out = &e->box;
    return VKN_SHS_OK;
}
static int bounds(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_shs_b *b) {
    const vkn_shader *sh = ctx->shader;
    const nr_sb_prog *pr = prog_of(sh);
    if (!pr || !pr->affine) return VKN_SHS_NOPROG;
    if (in->prim != 1 && in->prim != 3) return VKN_SHS_PRIM;
    const vkn_vfmt *vf = ctx->vfmt ? vkn_vfmt_info(ctx->vfmt) : NULL;
    const VkVertexInputAttributeDescription *a = NULL;
    for (uint32_t k = 0; vf && k < vf->nattr; k++) if (vf->attr[k].location == 0) a = &vf->attr[k];
    if (!a || a->binding >= 3) return VKN_SHS_FORMAT;
    const int c4 = a->format == VK_FORMAT_R32G32B32A32_SFLOAT || a->format == VK_FORMAT_R16G16B16A16_SFLOAT;
    if (!c4 && a->format != VK_FORMAT_R32G32B32_SFLOAT && a->format != VK_FORMAT_R16G16B16_SFLOAT) return VKN_SHS_FORMAT;
    if (c4 && pr->in_w) return VKN_SHS_FORMAT;   // (a fourth component from the stream is no constant)
    const VkVertexInputBindingDescription *bd = NULL;
    for (uint32_t k = 0; k < vf->nbind; k++) if (vf->bind[k].binding == a->binding) bd = &vf->bind[k];
    if (!bd || bd->inputRate != VK_VERTEX_INPUT_RATE_VERTEX || !bd->stride || !in->vb[a->binding]) return VKN_SHS_FORMAT;
    const uint8_t *up[NR_SB_MAX_UBO]; size_t un[NR_SB_MAX_UBO];
    if (prog_ubos(pr, sh, NULL, 0, -1, 0, 0, up, un)) return VKN_SHS_UBO;
    if (!nr_sb_eval(pr, up, un, 1.0f, b->f)) return VKN_SHS_UBO;
    if (b->f[3].c[0] != 1.0f || b->f[3].c[1] != 0.0f || b->f[3].c[2] != 0.0f || b->f[3].c[3] != 0.0f) return VKN_SHS_W;
    {   // every constant word the position reads; [fix 22.11] and those of the object's own blocks (all but the cascade's per-view block)
        int cam_u[NR_SB_MAX_UBO];
        for (unsigned u = 0; u < pr->nubo; u++) {
            cam_u[u] = 0;
            for (uint32_t k = 0; k < sh->nbind; k++) if (sh->bind[k].binding == pr->ubo[u] && sh->bind[k].stage == 0 && sh->bind[k].kind == VKN_BK_UNIFORM && g.cam && (int)sh->bind[k].slot == g.cam_slot) cam_u[u] = 1;
        }
        uint64_t ph = 0x504948, po = 0x50494F;
        for (unsigned i = 0; i < pr->n; i++) if (pr->ins[i].op == NR_CV_UBO) {
            unsigned u = 0; while (u < pr->nubo && pr->ubo[u] != pr->ins[i].a) u++;
            uint32_t w = 0; if (u < pr->nubo) memcpy(&w, up[u] + pr->ins[i].b, 4);
            ph = nr_ss_mix(ph, (uint64_t)w | (uint64_t)i << 32);
            if (u >= pr->nubo || !cam_u[u]) po = nr_ss_mix(po, (uint64_t)w | (uint64_t)i << 32);
        }
        b->pih = ph; b->pio = po;
    }
    const nr_sb_box *bx = NULL;
    const int w = mesh_box(in, ctx, a, bd->stride, &bx);
    if (w != VKN_SHS_OK) return w;
    if (!nr_sb_px(b->f, bx, in->viewport, &b->px, &b->z0, &b->z1)) return VKN_SHS_W;
    return VKN_SHS_OK;
}
int vkn_shs_bounds(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_shs_b *b) {
    const int w = bounds(in, ctx, b);
    g.s.asked++; g.s.why[w]++;
    return w;
}

// ---- the piece
static const uint32_t k_vs[] = {
#include "../../android/shaders/vkn_shs_comp.vert.spv.inc"
};
static const uint32_t k_fs[] = {
#include "../../android/shaders/vkn_shs_comp.frag.spv.inc"
};
static VkShaderModule make_module(const uint32_t *w, size_t bytes) {
    const vkn_dev *d = vkn_device(); VkShaderModule m = VK_NULL_HANDLE;
    if (!d) return VK_NULL_HANDLE;
    const VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = w};
    const int lk = vkn_drv_lock(); const VkResult r = vkCreateShaderModule(d->dev, &ci, NULL, &m); vkn_drv_unlock(lk);
    return r == VK_SUCCESS ? m : VK_NULL_HANDLE;
}
static const vkn_shader *piece(void) {
    if (g.piece || g.piece_bad) return g.piece;
    g.piece_bad = 1;
    g.vs = make_module(k_vs, sizeof k_vs); g.fs = make_module(k_fs, sizeof k_fs);
    g.ds = vkn_fx_ds(1, 1, 1 /* less */, 0, 0, 0, 0, 0);
    if (!g.vs || !g.fs || !g.ds) { vkn_logf(RED "shs: the layer piece's modules / depth state could not be made: no scrolling"); return NULL; }
    vkn_bind tb[2]; memset(tb, 0, sizeof tb);
    tb[0].binding = 0; tb[0].kind = VKN_BK_FX_UBO; tb[0].stage = 1; tb[0].bundle_stage = 4; tb[0].slot = 0; tb[0].count = 1; tb[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; tb[0].bytes = 48; tb[0].dyn = VKN_NO_DYN;
    tb[1].binding = 1; tb[1].kind = VKN_BK_FX_TEX; tb[1].stage = 1; tb[1].bundle_stage = 4; tb[1].slot = 0; tb[1].count = 1; tb[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; tb[1].dyn = VKN_NO_DYN;
    vkn_fx_shader_src src; memset(&src, 0, sizeof src);
    src.name = "shs-layer"; src.vs = g.vs; src.fs = g.fs; src.bind = tb; src.nbind = 2; src.ninputs = 0;
    g.piece = vkn_fx_shader_create(&src);
    if (!g.piece) { vkn_logf(RED "shs: the layer piece's shader object was refused: no scrolling"); return NULL; }
    g.piece_bad = 0;
    return g.piece;
}
int vkn_shs_composite(const vkn_draw_in *base, vkn_tex *layer, const int32_t tile[4], const nr_ss_rect *keep, const int32_t O[2], float dz, float clear) {
    const vkn_shader *p = piece();
    uint32_t off = 0;
    if (!p || !base || !layer || !layer->view || nr_ss_empty(keep) || vkn_ring_alloc(48, &off)) { g.s.comp_fail++; return -1; }
    uint8_t *u = (uint8_t *)vkn_ring_cpu() + off;
    const int32_t iv[8] = {tile[0], tile[1], tile[2], tile[3], O[0], O[1], 0, 0}; const float fv[4] = {dz, clear, 0.0f, 0.0f};
    memcpy(u, iv, 32); memcpy(u + 32, fv, 16);
    vkn_fx_out fo; memset(&fo, 0, sizeof fo);
    fo.shader = p; fo.ds = g.ds;
    fo.tex[0].view = layer->view; fo.tex[0].sampler = VK_NULL_HANDLE; fo.tex[0].tex = layer;
    fo.ubo[0].buf = VK_NULL_HANDLE; fo.ubo[0].off = off; fo.ubo[0].size = 48;
    vkn_draw_in in = *base;
    in.blend = 0; in.ds = 0; in.raster = 0; in.prim = 1; in.stencil_ref = 0; in.alpha_ref = -1.0f;
    in.viewport[0] = (float)tile[0]; in.viewport[1] = (float)tile[1]; in.viewport[2] = (float)tile[2]; in.viewport[3] = (float)tile[3];
    in.scissor[0] = keep->x0; in.scissor[1] = keep->y0; in.scissor[2] = keep->x1 - keep->x0; in.scissor[3] = keep->y1 - keep->y0;
    vkn_fx_bypass(1); vkn_fx_force(&fo);
    const int rc = vkn_draw_arrays(&in, 0, 3);
    vkn_fx_force(NULL); vkn_fx_bypass(0);
    if (rc) { g.s.comp_fail++; return -1; }
    g.s.comp++;
    return 0;
}
#endif
