// [DOS2] VK-NATIVE stage 5.1 A3: the static shadow cache and the far-cascade skip (vkn_shc.h has the scheme and the switches).
// One vkn_fx function (order 100) sees every graphics draw before anything of it is touched; a draw that is not into the shadow atlas
// costs one pointer compare (ctx->depth != the atlas) and two loads. Everything here runs on the recording thread (the fx chain, the
// vkn_pass watch, the close listener, the census); no locks. No game addresses.
// Engine facts used (docs/dos2/nr/NR-SHCACHE.md section 1, the old path's dumps): the sun shadow atlas is one 1536x1536 D16 depth target
// bound with no colour target, cleared whole once a frame, then one run of draws per cascade into its own 768x768 viewport tile of a 2x2
// grid (3 cascades: near, middle, far), all depth-only LESS / LEQUAL; the lighting pass then samples the atlas.
#ifdef DM_GAME_DOS2
#include "vkn_shc.h"
#include "vkn_fx.h"
#include "vkn_buf.h"
#include "vkn_draw.h"
#include "vkn_frame.h"
#include "vkn_pass.h"
#include "vkn_query.h"
#include "vkn_shader.h"
#include "vkn_state.h"
#include "vkn_tex.h"
#include "vkn_rt.h"     // [rt P4] vkn_rt_ehash
#include "../nr/nr_shc.h"
#include "../nr/nr_sort.h"
#include "../nr/nr_hacks.h"
#include "vkn_mat.h"   // [stage 5.2 B5 fixpp] vkn_mat_apply: the MAT decision for a replayed caster
#include "vkn_drv.h"   // [stage 5.3 V2] DM_GFX_CLEAR_REBIND after vkCmdClearAttachments
#include "vkn_shs.h"   // [VK-SHADOWS-MOVING] caster bounds, the scroll piece
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
_Static_assert((int)VKN_SHC_RF == (int)NRSHC_RF_N && (int)VKN_SHC_W == (int)NRSHC_W_N && (int)VKN_SHC_T == (int)NRSHC_T_N && (int)VKN_SHC_NRS == (int)NRS_W_N &&
               (int)VKN_SHC_CAS == (int)NRSHC_CAS,
               "vkn_shc.h reason counts follow nr_shc.h / nr_sort.h");

// ---- one kept draw (what a replay needs: the engine's draw as plain values; the slot arrays of vkn_draw_in point into the renderer's
// cache, so the slots the shader reads are copied; the constant blocks are ring offsets of this frame) ----
enum { SV_TEX = 8, SV_UBO = 12 };
typedef struct sdraw {
    vkn_draw_in in;
    uint32_t first, count, instances;
    uint8_t indexed, ntex, nubo, ok, vread;   // ok: complete (replayable); vread: bit st = vertex stream st is read
    struct { uint8_t st, slot; int32_t id, sid; } tx[SV_TEX];
    vkn_shs_ub ub[SV_UBO];
    uint64_t vver[3], iver;                   // buffer versions at the draw (a replay with other bytes would not be this draw)
    // [VK-SHADOWS-MOVING] tcls: a translation-invariant caster - inst its instance (nr_shs.h registry of the slot), mh the hash the model
    // got, lb its tile-local pixel bounds, z0 / z1 its clip z range; touch: a member that touches an exposed strip (drawn, kept for the layer)
    int32_t inst; uint8_t tcls, touch; nr_ss_rect lb; float z0, z1; uint64_t mh;
} sdraw;

enum { END_NEXT, END_CLEAR, END_READ, END_USE, END_CLOSE };   // why a cascade ended (END_NEXT: at a draw = the safe point)
enum { P_NONE, P_BUILD, P_RECOVER, P_VERIFY };                // work left for the next safe point
enum { RP_ALL, RP_DRAWN, RP_NONE };                           // replay: every kept draw / the ordinals the model marked drawn / none (a clear only)
enum { R_ALLOC, R_ATLAS, R_IMG, R_BUILD, R_RECOVER, R_LOST, R_STALE, R_VERIFY, R_UBO, R_SAVE, R_PRE, R_SCROLL, R_N };

static struct {
    // switches
    int mode, far, inst; nrshc_par par; uint32_t verify_every;
    // the atlas (a reference is held: the pointer cannot be reused while it is ours) and the depth target refused last
    vkn_tex *atlas, *rej; uint64_t atlas_gen, atlas_last;
    vkn_tex *S, *X; int img_fail;             // layer image (same format / size as the atlas), verify side image
    // the frame
    uint64_t frame; int frame_seen, arms, ntiles, dirty; uint32_t used; int32_t tiles[NRSHC_CAS][4];
    uint64_t clear_frame; uint32_t clear_bits; int clear_seen;
    int32_t slot_rect[NRSHC_CAS][4]; int slot_has[NRSHC_CAS], slot_real[NRSHC_CAS], slot_unrep[NRSHC_CAS]; uint64_t vnext[NRSHC_CAS];
    // slot_real: S holds the slot's layer (drawn by a replay); slot_unrep: the slot's last cascade had a draw that cannot be replayed
    nrh_tiles ftiles;
    // the model (lazy)
    nrshc_cas *cas[NRSHC_CAS]; nrshc_range *rg; sdraw *sv; int model; uint64_t salt_n;
    // the cascade being drawn
    int live; struct { int refused, k, save, verify, unrep, far, reuse; int32_t rect[4]; uint32_t clear_bits; } R;   // far / reuse: [gpu2 P8]
    // work for the next safe point
    int pend; struct { int kind, k; int32_t rect[4]; uint32_t bits; } P;
    // tiles whose layer was copied into the atlas at the frame's first cascade (precopy) and not yet taken by a hit: bit k = slot k,
    // its tile pre_rect[k]; pre_frame: the frame the precopy ran in
    uint32_t pre; int32_t pre_rect[NRSHC_CAS][4]; uint64_t pre_frame;
    int self;                                 // this module is calling vkn_pass itself (its watch events are ours)
    uint64_t cond_ref;                        // [stage 5.1 G] cascades refused for DM_VK_NATIVE_COND 1 / 3 (since the last census)
    const uint8_t *ring;
    struct { uint64_t frame; uint32_t off, n; uint64_t h; const void *mk; } um[2][VKN_UBO_SLOTS];   // the content hash of the last block of (stage, slot); [rt P3-GPU] mk: the read mask it was made with
    vkn_shc_stats s; uint8_t red[R_N];
    // [gpu2 P8] DM_VK_NATIVE_SHRATE (vkn_shc.h): F = the far tile as the last frame that DREW it left it (a private image like S);
    // f_ok / f_rect / f_frame / f_gen / f_bits: what F holds; f_pend: the far cascade of this frame ended drawn - copy its tile into F at the
    // next safe point (fp_rect); cur_far: the draw in hand is into the far tile (nrh_far)
    int rate; vkn_tex *F; int f_ok, f_pend, cur_far; int32_t f_rect[4], fp_rect[4]; uint64_t f_frame, f_gen; uint32_t f_bits;
} g;

#define SHC_RED(k, ...) do { if (g.red[k] < 16) { g.red[k]++; vkn_logf(RED "shcache: " __VA_ARGS__); } } while (0)

// [stage 5.1 G] DM_VK_NATIVE_COND (vkn_query.h) and this cache. The engine never puts a visibility condition on a draw of the sun
// cascade and opens no bracket there (its pass flags exclude the visibility query: docs/dos2/nr/NR-NATIVE-RE.md, the visibility-query
// row; the facts with addresses in dm/host/hle_dos2_vkn_exec.c), so no mode skips or predicates a caster by itself. Mode 2 (count)
// records every draw as asked: the cache stays on. Mode 4 (oracle) skips on the CPU (fx order 50, before this function) only draws
// of a bracket or conditional draws, neither of which reaches the atlas: the cascades this function sees are the ones mode 0 sees, so a hit's members all come and
// the RECOVER path ("a member did not come") does not run for that reason - on as well. The replays this module makes inside the chain
// do not enter the query module (vkn_draw.c g_fx_nest: a piece's nested draw), so they leave the brackets, the predicates and the
// counts alone. Modes 1 / 3 (GPU predicates) stay refused as before (a predicated caster would not be a pure function of its inputs;
// none is expected, but the phone has not shown it): counted by cond_ref, named in the census.
// [stage 5 technique A] modes 5 / 6 as 4: their CPU skips (vkn_occ.c, fx order 45) are pair draws and conditional draws only.
static int cond_ok(void) { const int m = vkn_query_mode(); return m == VKN_COND_OFF || m == VKN_COND_COUNT || m == VKN_COND_ORACLE || m == VKN_COND_A || m == VKN_COND_A_VERIFY ||
                                                              m == VKN_COND_B || m == VKN_COND_B_VERIFY || m == VKN_COND_AB; }   // [stage 5 technique B] predicates only on EQUAL draws without depth writes (never a caster), replays never in a shadow pass
static int stage_src(uint8_t st) { return st == 0 ? 0 : st == 1 ? 1 : -1; }   // engine stage (vkn_bind.stage) -> draw-path stage (graphics only)
static int fixed_depth(VkFormat f) { return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32; }
static uint32_t depth_bpp(VkFormat f) { return f == VK_FORMAT_D16_UNORM ? 2u : 4u; }
static int overlap(const int32_t a[4], const int32_t b[4]) { return a[0] < b[0] + b[2] && b[0] < a[0] + a[2] && a[1] < b[1] + b[3] && b[1] < a[1] + a[3]; }

// The shape rule of the old path (nr_shc.h nrshc_shape_why) on a backend texture.
static int shape_why(const vkn_tex *t, int colours, int arms) {
    nrshc_shape sh; memset(&sh, 0, sizeof sh);
    sh.has_depth = t != NULL; sh.same_image = 1;
    if (t) {
        sh.depth_aspect_only = t->aspect == VK_IMAGE_ASPECT_DEPTH_BIT; sh.fixed_point = fixed_depth(t->format);
        sh.levels = (int)t->mips; sh.layers = (int)(t->layers * (t->depth ? t->depth : 1)); sh.samples = (int)t->samples; sh.width = t->w; sh.height = t->h;
    }
    sh.colours = colours; sh.arms = arms;
    return nrshc_shape_why(&sh);
}

// ---- images ----
static uint64_t img_bytes(void) { return (g.S ? g.S->bytes : 0) + (g.X ? g.X->bytes : 0) + (g.F ? g.F->bytes : 0); }
static void img_free(void) {
    if (g.S) vkn_tex_private_free(g.S);
    if (g.X) vkn_tex_private_free(g.X);
    if (g.F) vkn_tex_private_free(g.F);
    g.S = g.X = g.F = NULL; g.f_ok = g.f_pend = 0;
}
static vkn_tex *img_make(const char *name) {
    vkn_tex *t = vkn_tex_private_depth(g.atlas->format, g.atlas->w, g.atlas->h, name);
    if (!t) { g.img_fail = 1; SHC_RED(R_IMG, "no %s image (%ux%u format %d): the cache stays off for this atlas", name, g.atlas->w, g.atlas->h, (int)g.atlas->format); return NULL; }
    g.self = 1; vkn_pass_clear_depth(t, 1, 1.0f, 0); g.self = 0;   // defined content (folded into its first pass): never "read before written"
    return t;
}

// ---- the model (allocated at the first cascade) ----
static int model_alloc(void) {
    if (g.model) return g.model > 0 ? 0 : -1;
    g.model = -1;
    for (int i = 0; i < NRSHC_CAS; i++) if (!(g.cas[i] = nrshc_cas_new())) goto fail;
    if (!(g.rg = (nrshc_range *)calloc(1, sizeof *g.rg)) || !(g.sv = (sdraw *)calloc(NRSHC_DRAWS, sizeof *g.sv))) goto fail;
    g.model = 1;
    g.s.model_bytes = NRSHC_CAS * sizeof(nrshc_cas) + sizeof(nrshc_range) + (uint64_t)NRSHC_DRAWS * sizeof(sdraw);
    return 0;
fail:
    SHC_RED(R_ALLOC, "no memory for the model (%zu bytes): the cache stays off", NRSHC_CAS * sizeof(nrshc_cas) + sizeof(nrshc_range) + NRSHC_DRAWS * sizeof(sdraw));
    return -1;
}
static void scroll_reset_all(void);   // [VK-SHADOWS-MOVING]
static void layers_drop(void) {
    scroll_reset_all();
    for (int i = 0; i < NRSHC_CAS; i++) { if (g.cas[i]) g.cas[i]->layer = 0; g.slot_has[i] = g.slot_real[i] = 0; }
}

// ---- own commands (outside the engine's pass; vkn_pass keeps the layouts) ----
static int copy_tile(vkn_tex *src, vkn_tex *dst, const int32_t r[4]) {
    VkCommandBuffer cb = vkn_cb();
    if (!cb || !src || !dst) return -1;
    g.self = 1;
    vkn_pass_layout(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);   // (ends an open pass; runs a remembered clear first)
    vkn_pass_layout(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    g.self = 0;
    if (src->layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || dst->layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) return -1;
    const VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .srcOffset = {r[0], r[1], 0},
                           .dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .dstOffset = {r[0], r[1], 0}, .extent = {(uint32_t)r[2], (uint32_t)r[3], 1}};
    vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    g.s.copies++;
    return 0;
}
static int read_tile(vkn_tex *t, const int32_t r[4], vkn_block *b) {
    VkCommandBuffer cb = vkn_cb();
    const uint64_t n = (uint64_t)r[2] * (uint64_t)r[3] * depth_bpp(t->format);
    if (!cb || vkn_block_alloc(VKN_ARENA_STAGING, n, 256, b)) return -1;
    g.self = 1; vkn_pass_layout(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); g.self = 0;
    if (t->layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) { vkn_block_free(b); return -1; }
    const VkBufferImageCopy c = {.bufferOffset = b->off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .imageOffset = {r[0], r[1], 0},
                                 .imageExtent = {(uint32_t)r[2], (uint32_t)r[3], 1}};
    vkCmdCopyImageToBuffer(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->buf, 1, &c);
    return 0;
}

// ---- replay: kept draws of the cascade drawn again into dst (the atlas, S or X), between vkn_fx_bypass(1) / (0) at a safe point.
// clear_rect: the tile is first cleared to clear_bits (vkCmdClearAttachments in dst's pass). The engine's target set, its textures and
// the announced constant blocks are put back afterwards. Returns the draws that could not be made (0 = every one). ----
// [stage 5.2 B5 fixpp] A replayed caster gets the MAT decision its engine draw got from the chain (vkn_mat.h vkn_mat_apply: no fragment
// stage / the cut-down vertex shader, both bit-equal depth), drawn under vkn_fx_force. A forced draw copies its output into the draw
// path's effect output, which inside the chain IS the `out` of the engine draw in hand (vkn_fx.h): shc_fx keeps a pointer to it and
// the replay puts its contents back before it returns. Weak: a build / test without vkn_mat.c replays with the engine's shaders.
extern int vkn_mat_apply(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out) __attribute__((weak));
static vkn_fx_out *g_chain_out;   // shc_fx's `out` while it runs (NULL: a replay from the close listener, outside the chain)
static int mat_replay(const vkn_draw_in *in, vkn_tex *dst, const struct sdraw *sd, vkn_fx_out *mo) {
    if (!vkn_mat_apply) return 0;
    const vkn_shader *sh = vkn_shader_get(in->shader); if (!sh) return 0;
    vkn_fx_ctx c; memset(&c, 0, sizeof c);
    c.shader = sh; c.depth = dst; c.blend = in->blend ? vkn_blend_id(in->blend) : 0; c.ds = in->ds ? vkn_ds_id(in->ds) : 0;
    c.raster = in->raster ? vkn_raster_id(in->raster) : 0; c.vfmt = in->vfmt ? vkn_vfmt_id(in->vfmt) : 0; c.prim = in->prim;
    c.indexed = sd->indexed; c.first = sd->first; c.count = sd->count; c.instances = sd->instances; c.frame = vkn_frame(); c.draw_no = 1;
    memset(mo, 0, sizeof *mo);
    return vkn_mat_apply(in, &c, mo);
}
// ================================================================================================ [VK-SHADOWS-MOVING]
// The scrolling layer and the caster culling (vkn_shc.h block "VK-SHADOWS-MOVING"; the rules and proofs: dm/gfx/nr/nr_shs.h; the bounds,
// the switches and the layer piece: vkn_shs.c). State of this block: gs. Everything on the recording thread.
enum { SJ_TV = 768 };
#define SJ_ZG 1e-4f   /* a member's clip z must stay this far inside (0, 1), in the frame's z frame and in the layer's */
typedef struct sslot {
    int anch;                     // the layer has an anchor: the cascade's reference rows at its build
    int plain;                    // never scrolled since the build (offset 0, the build's z frame): the cache as before
    int32_t O[2], sh[2];          // the layer's toroidal offset; the shift since the build (px)
    float lin[9], t[3];           // the anchor: linear part and translation of the reference rows
    uint32_t zw[2];               // the anchor's raw z-translation words (View, ViewProjection): the layer's z frame
    float last[12]; int last_ok;  // the reference of the slot's previous range
    uint32_t lzw[2], zrun;        // [fix 22.11] the z words of the slot's previous range; ranges in a row with the z words unchanged
    uint32_t nmem, fmem;          // members with an instance (mem / mh), other members (the layer cannot scroll while it has one)
    uint16_t mem[NRSHC_DRAWS]; uint64_t mh[NRSHC_DRAWS];
} sslot;
typedef struct sjob { int on; int32_t rect[4]; int ne; nr_ss_rect e[2]; int32_t O[2]; uint32_t t0, t1, cam_from, cam_to; } sjob;
static struct {
    int scroll, cull, cam; uint32_t cam_slot, vp_off, view_off; double eps;
    sslot *sl; nr_ss_reg *reg[NRSHC_CAS]; sdraw *tv; uint32_t ntv; uint64_t tv_frame;
    // the range: have - the reference rows are known; same_prev - they are the previous range's bit for bit; on - the scroll path
    struct { int have, same_prev, on, k, zmv; float ref[12]; uint32_t cam_off, cam_size, serial, fresh; int32_t n[2], O2[2]; nr_ss_rect e[2], keep; int ne; float dz; } R;
    struct { int on, cz; nr_ss_rect keep; int32_t O[2]; float dz; } V;   // the ended range, for its verify (cz: its layer moved in z)
    int zok;                                                           // [fix 22.11] DM_VK_NATIVE_SHSCROLL_Z=1
    sjob job[NRSHC_CAS]; int njob, rp_comp;
    const vkn_draw_in *cur_in;
    int (*t_nofs)(const vkn_shader *s);
    vkn_shc_scroll_stats c;
} gs;
extern int vkn_mat_peek(const vkn_draw_in *in, const vkn_fx_ctx *ctx) __attribute__((weak));
void vkn_shc_scroll_stats_get(vkn_shc_scroll_stats *s) { if (s) *s = gs.c; }
void vkn_shc_test_nofs(int (*fn)(const vkn_shader *s)) { gs.t_nofs = fn; }

static int scroll_alloc(void) {
    if (gs.sl) return 0;
    gs.sl = (sslot *)calloc(NRSHC_CAS, sizeof *gs.sl); gs.tv = (sdraw *)calloc(SJ_TV, sizeof *gs.tv);
    if (!gs.sl || !gs.tv) { free(gs.sl); free(gs.tv); gs.sl = NULL; gs.tv = NULL; gs.scroll = 0; SHC_RED(R_ALLOC, "no memory for the scroll state: DM_VK_NATIVE_SHSCROLL off"); return -1; }
    return 0;
}
static nr_ss_reg *scroll_reg(int k) {
    if (!gs.reg[k] && (gs.reg[k] = (nr_ss_reg *)malloc(sizeof(nr_ss_reg)))) nr_ss_reg_init(gs.reg[k], gs.eps, 4e-6);
    return gs.reg[k];
}
static uint64_t scroll_bytes(void) {
    uint64_t n = gs.sl ? NRSHC_CAS * sizeof(sslot) + (uint64_t)SJ_TV * sizeof(sdraw) : 0;
    for (int i = 0; i < NRSHC_CAS; i++) if (gs.reg[i]) n += sizeof(nr_ss_reg);
    return n;
}
// The slot's layer is gone (or about to be made again): no member, no anchor, no strip work.
static void scroll_slot_reset(int k) {
    if (!gs.sl) return;
    sslot *q = &gs.sl[k]; nr_ss_reg *rg = gs.reg[k];
    for (uint32_t i = 0; rg && i < q->nmem; i++) rg->in[q->mem[i]].mem = 0;
    q->nmem = q->fmem = 0; q->anch = 0; q->plain = 0; q->O[0] = q->O[1] = q->sh[0] = q->sh[1] = 0;
    if (gs.job[k].on) { gs.job[k].on = 0; gs.njob--; gs.c.job_lost++; }
}
static void scroll_drop(int k) { if (g.cas[k]) g.cas[k]->layer = 0; g.slot_real[k] = 0; scroll_slot_reset(k); }
static void scroll_reset_all(void) { for (int k = 0; gs.sl && k < NRSHC_CAS; k++) { scroll_slot_reset(k); gs.sl[k].last_ok = 0; } gs.R.on = 0; gs.V.on = 0; }
// The cascade's reference: rows 0..2 of the ViewProjection the announced per-view block holds (x y z coefficients + translation each).
static int ref_read(float ref[12], uint32_t *off, uint32_t *size) {
    uint32_t o = 0, z = 0;
    if (!gs.cam || vkn_draw_ubo_get(0, gs.cam_slot, &o, &z) || z < gs.vp_off + 48 || z < gs.view_off + 48) return 0;
    if (!g.ring) g.ring = (const uint8_t *)vkn_ring_cpu();
    if (!g.ring || (uint64_t)o + z > VKN_RING_BYTES) return 0;
    memcpy(ref, g.ring + o + gs.vp_off, 48);
    for (int i = 0; i < 12; i++) if (!(ref[i] == ref[i]) || ref[i] > 1e30f || ref[i] < -1e30f) return 0;
    *off = o; *size = z;
    return 1;
}
static int scroll_can(void) { return g.S && g.atlas && g.atlas->format == VK_FORMAT_D16_UNORM && g.clear_bits == 0x3f800000u; }
// At the first draw of a range, after the model's decision *act: the reference, and for a HIT whether the layer is taken as before
// (the cascade did not move since its build), scrolled, or dropped.
static void scroll_arm(int k, const int32_t rect[4], int *act, int *why) {
    memset(&gs.R, 0, sizeof gs.R); gs.R.k = k;
    if (!gs.scroll || scroll_alloc()) return;
    sslot *q = &gs.sl[k]; nrshc_cas *c = g.cas[k];
    gs.R.serial = (uint32_t)c->ranges + 1;
    gs.R.have = ref_read(gs.R.ref, &gs.R.cam_off, &gs.R.cam_size);
    gs.R.same_prev = gs.R.have && q->last_ok && !memcmp(gs.R.ref, q->last, sizeof q->last);
    // [fix 22.11] the z frame of the cascade: the raw z-translation words (View, ViewProjection); how long they stood
    uint32_t zw[2] = {0, 0};
    if (gs.R.have) { memcpy(&zw[0], g.ring + gs.R.cam_off + gs.view_off + 44, 4); memcpy(&zw[1], g.ring + gs.R.cam_off + gs.vp_off + 44, 4); }
    if (gs.R.have && q->last_ok) {
        gs.c.zcmp++;
        if (zw[0] != q->lzw[0] || zw[1] != q->lzw[1]) { gs.c.zstep++; q->zrun = 0; } else if (q->zrun < 100000) q->zrun++;
    } else q->zrun = 0;
    q->lzw[0] = zw[0]; q->lzw[1] = zw[1];
    q->last_ok = gs.R.have; if (gs.R.have) memcpy(q->last, gs.R.ref, sizeof q->last);
    // [fix 22.11] a layer built while the z frame moves would be dropped at its next hit (a replay of every member for nothing): held
    // until the z words stood `stable` ranges in a row (standing - the cache as before - builds as soon as the model wants)
    if (*act == NRSHC_A_BUILD && !gs.zok && gs.R.have && q->zrun + 1 < g.par.stable) { gs.c.build_held++; *act = NRSHC_A_NORMAL; *why = NRSHC_W_OFF; return; }
    if (*act != NRSHC_A_HIT || !q->anch) return;   // (a layer without an anchor: the model's hashes alone, as before)
    gs.R.zmv = zw[0] != q->zw[0] || zw[1] != q->zw[1];
    const float *r = gs.R.ref; uint64_t *drop = NULL;
    const float lin[9] = {r[0], r[1], r[2], r[4], r[5], r[6], r[8], r[9], r[10]}, t[3] = {r[3], r[7], r[11]};
    int32_t nx = 0, ny = 0; double err = 0;
    if (gs.job[k].on) drop = &gs.c.job_late;   // (the strips of the frame before were never drawn)
    else if (!gs.R.have) drop = &gs.c.drop_ref;
    else if (memcmp(lin, q->lin, sizeof lin)) drop = &gs.c.drop_lin;
    else if (q->plain && !memcmp(t, q->t, sizeof t)) { gs.c.plain_ranges++; return; }
    else if (gs.R.zmv && !gs.zok) drop = &gs.c.drop_z;   // [fix 22.11] the layer's depths cannot follow a z shift exactly
    else if (!nr_ss_shift((double)t[0] - (double)q->t[0], (double)t[1] - (double)q->t[1], rect[2], rect[3], gs.eps, &nx, &ny, &err)) drop = &gs.c.drop_shift;
    else if (abs(nx - q->sh[0]) >= rect[2] / 2 || abs(ny - q->sh[1]) >= rect[3] / 2) drop = &gs.c.drop_big;
    else if (q->fmem) drop = &gs.c.drop_fmem;
    else if (!scroll_can()) drop = &gs.c.drop_fmt;
    if (drop) { ++*drop; scroll_drop(k); *act = NRSHC_A_NORMAL; *why = NRSHC_W_LAYER; return; }
    const int32_t W = rect[2], H = rect[3];
    gs.R.on = 1; gs.R.n[0] = nx - q->sh[0]; gs.R.n[1] = ny - q->sh[1];
    gs.R.O2[0] = q->O[0]; gs.R.O2[1] = q->O[1]; nr_ss_scroll(W, H, gs.R.O2, gs.R.n[0], gs.R.n[1]);
    gs.R.ne = nr_ss_exposed(W, H, gs.R.n[0], gs.R.n[1], gs.R.e);
    gs.R.keep.x0 = gs.R.n[0] > 0 ? gs.R.n[0] : 0; gs.R.keep.x1 = gs.R.n[0] < 0 ? W + gs.R.n[0] : W;
    gs.R.keep.y0 = gs.R.n[1] > 0 ? gs.R.n[1] : 0; gs.R.keep.y1 = gs.R.n[1] < 0 ? H + gs.R.n[1] : H;
    gs.R.dz = t[2] - q->t[2];
    gs.c.scroll_ranges++;
    if (gs.R.n[0] | gs.R.n[1]) gs.c.moved_ranges++;
    if (gs.R.dz != 0.0f) gs.c.dz_ranges++;
    if (err > gs.c.err_max) gs.c.err_max = err;
    { const uint32_t m = (uint32_t)(abs(gs.R.n[0]) > abs(gs.R.n[1]) ? abs(gs.R.n[0]) : abs(gs.R.n[1])); if (m > gs.c.n_max) gs.c.n_max = m; }
}
// The scrolled layer into the tile of the current target set (the engine's atlas pass, or the verify image inside a replay).
static int scroll_comp(const int32_t rect[4], const nr_ss_rect *keep, const int32_t O[2], float dz) {
    const nr_ss_rect fb = {rect[0] + keep->x0, rect[1] + keep->y0, rect[0] + keep->x1, rect[1] + keep->y1};
    if (nr_ss_empty(&fb)) return 0;
    vkn_fx_out chain_keep; if (g_chain_out) chain_keep = *g_chain_out;   // (a forced draw overwrites the chain's output: vkn_fx.h)
    const int self = g.self; g.self = 1;
    const int rc = vkn_shs_composite(gs.cur_in, g.S, rect, &fb, O, dz + 0.25f / 65535.0f, 1.0f);
    g.self = self;
    if (g_chain_out) *g_chain_out = chain_keep;
    return rc;
}
// A draw of an armed range that has bounds: is it a translation-invariant caster ("T")? Then *hfin = its hash for the model (structure +
// instance id) and *inst its instance.
static int scroll_tdraw(const vkn_draw_in *in, const vkn_fx_ctx *ctx, const vkn_shs_b *b, uint64_t h0, uint64_t vj, uint64_t *hfin, int32_t *inst) {
    uint32_t o = 0, z = 0;
    if (vkn_draw_ubo_get(0, gs.cam_slot, &o, &z) || o != gs.R.cam_off) return 0;   // another per-view block than the range's
    const int32_t *rc = g.R.rect;
    if (in->scissor[0] > rc[0] || in->scissor[1] > rc[1] || in->scissor[0] + in->scissor[2] < rc[0] + rc[2] || in->scissor[1] + in->scissor[3] < rc[1] + rc[3]) return 0;
    if (!(gs.t_nofs ? gs.t_nofs(ctx->shader) : vkn_mat_peek ? vkn_mat_peek(in, ctx) : 0)) { gs.c.t_fs++; return 0; }   // (its fragment stage may kill / write depth)
    nr_ss_reg *rg = scroll_reg(g.R.k); if (!rg) return 0;
    const float *r = gs.R.ref;
    const double u[3] = {((double)b->f[0].c[0] - (double)r[3]) * 0.5 * rc[2], -((double)b->f[1].c[0] - (double)r[7]) * 0.5 * rc[3], (double)b->f[2].c[0] - (double)r[11]};
    uint64_t hs = nrshc_mix(nrshc_mix(h0, vj), 0x54434c53);
    for (int c = 0; c < 3; c++) hs = nrshc_bytes(hs, &b->f[c].c[1], 12);
    const int i = nr_ss_reg_get(rg, hs, u, gs.R.serial);
    if (i < 0) { gs.c.t_full++; return 0; }
    nr_ss_inst *I = &rg->in[i];
    if (I->bad) return 0;
    // the cascade did not move since the previous range, yet a constant this caster's position reads did (it moved by less than the
    // tolerance): another object for the cache - while the cascade stands a member is one only with bit-equal position inputs (exact:
    // the position of every vertex is a function of the mesh and those words, and no fragment stage acts)
    // [fix 22.11] standing or moving: an object whose own position words (World: every block but the per-view one) are not those of its
    // last draw is another object (phone M09: "changed while the cascade stood" 637 in 10 s - while the cascade moved such a caster kept
    // its id when it moved by less than the tolerance, and its old shadow stayed in the scrolled layer)
    if (I->ho_ok && I->ho != b->pio) { I->id = nr_ss_mix(I->id, 0x4F424A43) | 1u; gs.c.epoch_obj++; }
    else if (gs.R.same_prev && I->prev + 1 == gs.R.serial && I->hf != b->pih) { I->id = nr_ss_mix(I->id, 0x45504f43) | 1u; gs.c.epoch++; }
    I->hf = b->pih; I->ho = b->pio; I->ho_ok = 1;
    *hfin = nrshc_mix(hs, I->id); *inst = i;
    gs.c.t_draws++;
    return 1;
}
// A member of a HIT range came (the model wants it skipped): 1 = skip it, 0 = draw it (a strip toucher, or the range is broken).
static int scroll_member(sdraw *sd, const vkn_shs_b *b, nrshc_range *r) {
    if (!sd->tcls) {   // a member without an instance cannot have come under a moved projection: never trusted
        if (gs.R.on) { r->tainted = 1; gs.c.f_member++; return 0; }
        return 1;
    }
    nr_ss_inst *I = &gs.reg[g.R.k]->in[sd->inst];
    if (I->cstamp != gs.R.serial) { I->cstamp = gs.R.serial; I->cons = 0; }
    I->cons++;
    if (!gs.R.on) return 1;
    if (!(b->z0 > SJ_ZG && b->z1 < 1.0f - SJ_ZG && b->z0 - gs.R.dz > SJ_ZG && b->z1 - gs.R.dz < 1.0f - SJ_ZG)) { I->bad = 1; r->tainted = 1; gs.c.zguard++; return 0; }
    if (nr_ss_touch(&sd->lb, gs.R.e, gs.R.ne)) { sd->touch = 1; gs.c.touched++; return 0; }
    gs.c.skipped++;
    return 1;
}
// Before the model ends a scrolled HIT: every member that did not come must have left the tile (then it leaves the layer's members);
// one that did not come inside the tile breaks the range (RECOVER).
static void scroll_absent(int k, nrshc_range *r) {
    sslot *q = &gs.sl[k]; nr_ss_reg *rg = gs.reg[k]; nrshc_cas *c = g.cas[k];
    const int32_t sx = q->sh[0] + gs.R.n[0], sy = q->sh[1] + gs.R.n[1];
    const nr_ss_rect tile = {0, 0, g.R.rect[2], g.R.rect[3]};
    uint32_t w = 0;
    for (uint32_t i = 0; i < q->nmem; i++) {
        nr_ss_inst *I = &rg->in[q->mem[i]];
        int keep = 1;
        if (I->cstamp == gs.R.serial && I->cons) I->cons--;   // (one unit a member entry)
        else {
            const nr_ss_rect now = {I->bb.x0 + sx, I->bb.y0 + sy, I->bb.x1 + sx, I->bb.y1 + sy};
            if (r->tainted) { }
            else if (nr_ss_isect(&now, &tile)) { r->tainted = 1; gs.c.absent_in++; }
            else if (nrshc_mem_drop(c, q->mh[i], 1)) { keep = 0; if (I->mem) I->mem--; gs.c.absent_out++; }
            else { r->tainted = 1; gs.c.absent_in++; }
        }
        if (keep) { q->mem[w] = q->mem[i]; q->mh[w] = q->mh[i]; w++; }
    }
    q->nmem = w;
}
// A BUILD ended with a layer: its members, its anchor.
static void scroll_built(int k, const nrshc_range *r) {
    scroll_slot_reset(k);
    sslot *q = &gs.sl[k]; nr_ss_reg *rg = gs.reg[k];
    const uint32_t n = r->ndraw < NRSHC_DRAWS ? r->ndraw : NRSHC_DRAWS;
    for (uint32_t o = 0; o < n; o++) {
        if (r->f[o] != NRSHC_F_DRAWN) continue;
        const sdraw *sd = &g.sv[o];
        if (!sd->tcls || !rg || q->nmem >= NRSHC_DRAWS) { q->fmem++; continue; }
        nr_ss_inst *I = &rg->in[sd->inst];
        if (!(sd->z0 > SJ_ZG && sd->z1 < 1.0f - SJ_ZG)) { I->bad = 1; q->fmem++; gs.c.zguard++; continue; }
        q->mem[q->nmem] = (uint16_t)sd->inst; q->mh[q->nmem] = sd->mh; q->nmem++;
        if (!I->mem) I->bb = sd->lb;
        I->mem++;
    }
    if (gs.R.have) {
        const float *x = gs.R.ref;
        const float lin[9] = {x[0], x[1], x[2], x[4], x[5], x[6], x[8], x[9], x[10]};
        memcpy(q->lin, lin, sizeof lin); q->t[0] = x[3]; q->t[1] = x[7]; q->t[2] = x[11];
        memcpy(&q->zw[0], g.ring + gs.R.cam_off + gs.view_off + 44, 4); memcpy(&q->zw[1], g.ring + gs.R.cam_off + gs.vp_off + 44, 4);
        q->anch = 1; q->plain = 1;
    }
    if (rg) nr_ss_reg_sweep(rg, gs.R.serial, 600);
    gs.c.builds++; gs.c.build_t += q->nmem; gs.c.build_f += q->fmem;
}
// A scrolled HIT ended well: the layer's offset moves, its exposed strips are cleared and drawn (by the touchers) at the frame's safe
// point. 0 = ok; -1 = the layer cannot be kept (dropped).
static int scroll_commit(int k, const nrshc_range *r) {
    sslot *q = &gs.sl[k]; sjob *j = &gs.job[k];
    q->O[0] = gs.R.O2[0]; q->O[1] = gs.R.O2[1]; q->sh[0] += gs.R.n[0]; q->sh[1] += gs.R.n[1]; q->plain = 0;
    gs.V.on = 1; gs.V.keep = gs.R.keep; gs.V.O[0] = q->O[0]; gs.V.O[1] = q->O[1]; gs.V.dz = gs.R.dz; gs.V.cz = gs.R.zmv || gs.R.dz != 0.0f;
    if (j->on) { scroll_drop(k); return -1; }   // (the strips of the frame before were never drawn: its layer is not whole)
    if (!gs.R.ne) return 0;
    if (gs.tv_frame != g.frame) { gs.tv_frame = g.frame; gs.ntv = 0; }
    memset(j, 0, sizeof *j);
    memcpy(j->rect, g.R.rect, sizeof j->rect); j->ne = gs.R.ne; j->e[0] = gs.R.e[0]; j->e[1] = gs.R.e[1]; j->O[0] = q->O[0]; j->O[1] = q->O[1];
    j->t0 = gs.ntv; j->cam_from = j->cam_to = gs.R.cam_off;
    // the layer's z frame: the frame's per-view block with the anchor's z-translation words (checked by numbers for every toucher below)
    uint32_t zw[2]; memcpy(&zw[0], g.ring + gs.R.cam_off + gs.view_off + 44, 4); memcpy(&zw[1], g.ring + gs.R.cam_off + gs.vp_off + 44, 4);
    const int same_z = zw[0] == q->zw[0] && zw[1] == q->zw[1];
    const uint32_t n = r->ndraw < NRSHC_DRAWS ? r->ndraw : NRSHC_DRAWS;
    int bad = 0;
    for (uint32_t o = 0; o < n && !bad; o++) {
        const sdraw *sd = &g.sv[o];
        if (!sd->touch) continue;
        if (gs.ntv >= SJ_TV) { bad = 1; gs.c.job_full++; break; }
        if (!same_z) {
            if (j->cam_to == j->cam_from) {
                uint32_t off = 0;
                if (vkn_ring_alloc(gs.R.cam_size, &off)) { bad = 1; break; }
                uint8_t *p = (uint8_t *)vkn_ring_cpu() + off;
                memcpy(p, g.ring + gs.R.cam_off, gs.R.cam_size); memcpy(p + gs.view_off + 44, &q->zw[0], 4); memcpy(p + gs.vp_off + 44, &q->zw[1], 4);
                j->cam_to = off;
            }
            const vkn_shader *sh = vkn_shader_get(sd->in.shader); nr_sb_form a[4], b[4];
            if (!sh || !vkn_shs_forms_kept(sh, sd->ub, sd->nubo, -1, 0, 0, a) || !vkn_shs_forms_kept(sh, sd->ub, sd->nubo, (int)gs.cam_slot, j->cam_from, j->cam_to, b) ||
                memcmp(&a[0], &b[0], sizeof a[0]) || memcmp(&a[1], &b[1], sizeof a[1]) || memcmp(&a[2].c[1], &b[2].c[1], 12) || memcmp(&a[3], &b[3], sizeof a[3]) ||
                !(fabsf((a[2].c[0] - b[2].c[0]) - gs.R.dz) <= 2e-6f * (1.0f + fabsf(a[2].c[0])))) { bad = 1; gs.c.patch_bad++; break; }
        }
        gs.tv[gs.ntv++] = *sd;
    }
    if (bad) { gs.ntv = j->t0; scroll_drop(k); return -1; }
    j->t1 = gs.ntv; j->on = 1; gs.njob++;
    return 0;
}
// The frame's safe point (a draw outside the atlas): the strips of every scrolled layer, in one pass of the layer image.
static void scroll_jobs_run(void) {
    if (!gs.njob) return;
    vkn_fx_out chain_keep; if (g_chain_out) chain_keep = *g_chain_out;
    uint32_t n0 = 0, lay[VKN_PASS_COLORS], mip[VKN_PASS_COLORS]; vkn_tex *col[VKN_PASS_COLORS], *dep = NULL, *held[VKN_PASS_COLORS + 1];
    vkn_pass_cur_get(&n0, col, lay, mip, &dep);
    for (uint32_t i = 0; i <= VKN_PASS_COLORS; i++) { vkn_tex *t = i < VKN_PASS_COLORS ? col[i] : dep; held[i] = t && vkn_tex_ref(t) ? t : NULL; }
    struct { uint32_t off, size; int ok; } ub0[2][VKN_UBO_SLOTS];
    for (int st = 0; st < 2; st++) for (uint32_t sl = 0; sl < VKN_UBO_SLOTS; sl++) ub0[st][sl].ok = !vkn_draw_ubo_get(st, sl, &ub0[st][sl].off, &ub0[st][sl].size);
    g.self = 1; vkn_fx_bypass(1);
    if (g.S) vkn_pass_targets(0, NULL, NULL, NULL, g.S);
    vkn_pass_info pi; const int nopass = !g.S || vkn_pass_ready(&pi) || !vkn_cb();
    for (int k = 0; k < NRSHC_CAS; k++) {
        sjob *j = &gs.job[k];
        if (!j->on) continue;
        j->on = 0; gs.njob--;
        int bad = nopass || gs.tv_frame != g.frame;
        for (int ei = 0; ei < j->ne && !bad; ei++) {
            nr_ss_piece pc[4]; const int np = nr_ss_pieces(j->rect[2], j->rect[3], j->O, &j->e[ei], pc);
            for (int p = 0; p < np && !bad; p++) {
                const int32_t ox = j->rect[0] + pc[p].dx, oy = j->rect[1] + pc[p].dy;
                const VkClearAttachment ca = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .clearValue.depthStencil = {1.0f, 0}};
                const VkClearRect cr = {{{ox + pc[p].r.x0, oy + pc[p].r.y0}, {(uint32_t)(pc[p].r.x1 - pc[p].r.x0), (uint32_t)(pc[p].r.y1 - pc[p].r.y0)}}, 0, 1};
                if (vkn_pass_ready(&pi) || !vkn_cb()) { bad = 1; break; }
                vkCmdClearAttachments(vkn_cb(), 1, &ca, 1, &cr);
                vkn_drv_cleared(); g.s.tile_clears++; gs.c.job_pieces++;
                for (uint32_t t = j->t0; t < j->t1 && !bad; t++) {
                    sdraw *sd = &gs.tv[t];
                    if (!nr_ss_isect(&sd->lb, &pc[p].r)) continue;   // (its bounds do not reach this piece: no fragment there)
                    int stale = sd->indexed && vkn_buf_version(VKN_BUF_IB, sd->in.ib, NULL) != sd->iver;
                    for (int st = 0; st < 3; st++) if ((sd->vread >> st & 1) && vkn_buf_version(VKN_BUF_VB, sd->in.vb[st], NULL) != sd->vver[st]) stale = 1;
                    if (stale) { bad = 1; g.s.replay_stale++; break; }
                    int32_t tex[2][VKN_TEX_SLOTS], smp[2][VKN_TEX_SLOTS];
                    memset(tex, 0xff, sizeof tex); memset(smp, 0xff, sizeof smp);
                    for (uint32_t i = 0; i < sd->ntex; i++) { tex[sd->tx[i].st][sd->tx[i].slot] = sd->tx[i].id; smp[sd->tx[i].st][sd->tx[i].slot] = sd->tx[i].sid; }
                    vkn_draw_in in = sd->in;
                    in.tex[0] = tex[0]; in.tex[1] = tex[1]; in.tex[2] = NULL; in.smp[0] = smp[0]; in.smp[1] = smp[1]; in.smp[2] = NULL;
                    in.viewport[0] = (float)ox; in.viewport[1] = (float)oy;   // (the tile's size stays: the same texel grid, moved)
                    in.scissor[0] = ox + pc[p].r.x0; in.scissor[1] = oy + pc[p].r.y0; in.scissor[2] = pc[p].r.x1 - pc[p].r.x0; in.scissor[3] = pc[p].r.y1 - pc[p].r.y0;
                    for (uint32_t i = 0; i < sd->nubo; i++) {
                        const uint32_t off = sd->ub[i].st == 0 && sd->ub[i].slot == gs.cam_slot && sd->ub[i].off == j->cam_from ? j->cam_to : sd->ub[i].off;
                        vkn_draw_ubo(sd->ub[i].st, sd->ub[i].slot, off, sd->ub[i].size);
                    }
                    vkn_fx_out mo; const int forced = mat_replay(&in, g.S, sd, &mo);
                    if (forced) vkn_fx_force(&mo);
                    const int rc = sd->indexed ? vkn_draw_indexed(&in, sd->first, sd->count, sd->instances) : vkn_draw_arrays(&in, sd->first, sd->count);
                    if (forced) vkn_fx_force(NULL);
                    if (rc) { bad = 1; g.s.replay_drop++; } else { g.s.replayed++; gs.c.job_draws++; }
                }
            }
        }
        if (bad) { gs.c.job_fail++; scroll_drop(k); SHC_RED(R_SCROLL, "cascade %d: the scrolled layer's strips could not be drawn: the layer is dropped (drawn in full next frame)", k); }
        else gs.c.jobs++;
    }
    for (int st = 0; st < 2; st++) for (uint32_t sl = 0; sl < VKN_UBO_SLOTS; sl++) if (ub0[st][sl].ok) vkn_draw_ubo(st, sl, ub0[st][sl].off, ub0[st][sl].size);
    vkn_pass_targets(n0, col, lay, mip, dep);
    if (g.S) (void)vkn_pass_sample(g.S);   // readable for the tiles of the next frame now: no pass of the engine is open here, none is broken there
    for (uint32_t i = 0; i <= VKN_PASS_COLORS; i++) if (held[i]) vkn_tex_unref(held[i]);
    vkn_fx_bypass(0); g.self = 0;
    if (g_chain_out) *g_chain_out = chain_keep;
}
static void scroll_jobs_lost(void) { for (int k = 0; k < NRSHC_CAS; k++) if (gs.job[k].on) scroll_drop(k); }

static int replay(vkn_tex *dst, const int32_t *clear_rect, uint32_t clear_bits, int filter) {
    vkn_fx_out chain_keep; if (g_chain_out) chain_keep = *g_chain_out;   // [fixpp]
    uint32_t n0 = 0, lay[VKN_PASS_COLORS], mip[VKN_PASS_COLORS]; vkn_tex *col[VKN_PASS_COLORS], *dep = NULL, *held[VKN_PASS_COLORS + 1];
    vkn_pass_cur_get(&n0, col, lay, mip, &dep);
    for (uint32_t i = 0; i <= VKN_PASS_COLORS; i++) { vkn_tex *t = i < VKN_PASS_COLORS ? col[i] : dep; held[i] = t && vkn_tex_ref(t) ? t : NULL; }
    struct { uint32_t off, size; int ok; } ub0[2][VKN_UBO_SLOTS];
    for (int st = 0; st < 2; st++) for (uint32_t sl = 0; sl < VKN_UBO_SLOTS; sl++) ub0[st][sl].ok = !vkn_draw_ubo_get(st, sl, &ub0[st][sl].off, &ub0[st][sl].size);
    g.self = 1; vkn_fx_bypass(1);
    vkn_pass_targets(0, NULL, NULL, NULL, dst);
    int bad = 0;
    if (clear_rect) {
        vkn_pass_info pi;
        if (vkn_pass_ready(&pi) || !vkn_cb()) bad = -1;
        else {
            float d; memcpy(&d, &clear_bits, 4);
            const VkClearAttachment ca = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .clearValue.depthStencil = {d, 0}};
            const VkClearRect cr = {{{clear_rect[0], clear_rect[1]}, {(uint32_t)clear_rect[2], (uint32_t)clear_rect[3]}}, 0, 1};
            vkCmdClearAttachments(vkn_cb(), 1, &ca, 1, &cr);
            vkn_drv_cleared();   // [stage 5.3 V2] DM_GFX_CLEAR_REBIND
            g.s.tile_clears++;
        }
    }
    if (bad >= 0 && gs.rp_comp && clear_rect && scroll_comp(clear_rect, &gs.V.keep, gs.V.O, gs.V.dz)) bad = -1;   // [VK-SHADOWS-MOVING] verify: the scrolled layer first
    const uint32_t n = g.rg->ndraw < NRSHC_DRAWS ? g.rg->ndraw : NRSHC_DRAWS;
    for (uint32_t o = 0; bad >= 0 && o < n; o++) {
        if (filter == RP_NONE) break;
        if (filter == RP_DRAWN && g.rg->f[o] != NRSHC_F_DRAWN) continue;
        sdraw *sd = &g.sv[o];
        if (!sd->ok) { bad++; g.s.replay_drop++; continue; }
        int stale = sd->indexed && vkn_buf_version(VKN_BUF_IB, sd->in.ib, NULL) != sd->iver;
        for (int st = 0; st < 3; st++) if ((sd->vread >> st & 1) && vkn_buf_version(VKN_BUF_VB, sd->in.vb[st], NULL) != sd->vver[st]) stale = 1;
        if (stale) { bad++; g.s.replay_stale++; SHC_RED(R_STALE, "replay: draw %u of cascade %d reads a vertex / index buffer written since it was drawn (not replayed)", o, g.R.k); continue; }
        int32_t tex[2][VKN_TEX_SLOTS], smp[2][VKN_TEX_SLOTS];
        memset(tex, 0xff, sizeof tex); memset(smp, 0xff, sizeof smp);
        for (uint32_t i = 0; i < sd->ntex; i++) { tex[sd->tx[i].st][sd->tx[i].slot] = sd->tx[i].id; smp[sd->tx[i].st][sd->tx[i].slot] = sd->tx[i].sid; }
        vkn_draw_in in = sd->in;
        in.tex[0] = tex[0]; in.tex[1] = tex[1]; in.tex[2] = NULL; in.smp[0] = smp[0]; in.smp[1] = smp[1]; in.smp[2] = NULL;
        for (uint32_t i = 0; i < sd->nubo; i++) vkn_draw_ubo(sd->ub[i].st, sd->ub[i].slot, sd->ub[i].off, sd->ub[i].size);
        vkn_fx_out mo; const int forced = mat_replay(&in, dst, sd, &mo);   // [fixpp] after the blocks are announced (the gate reads them)
        if (forced) vkn_fx_force(&mo);
        const int rc = sd->indexed ? vkn_draw_indexed(&in, sd->first, sd->count, sd->instances) : vkn_draw_arrays(&in, sd->first, sd->count);
        if (forced) vkn_fx_force(NULL);
        if (rc) { bad++; g.s.replay_drop++; } else g.s.replayed++;
    }
    for (int st = 0; st < 2; st++) for (uint32_t sl = 0; sl < VKN_UBO_SLOTS; sl++) {
        if (ub0[st][sl].ok) vkn_draw_ubo(st, sl, ub0[st][sl].off, ub0[st][sl].size);
        else if (!vkn_draw_ubo_get(st, sl, NULL, NULL)) { g.s.ubo_unrestored++; SHC_RED(R_UBO, "replay announced constant block %d.%u that the engine never had: it stays announced", st, sl); }
    }
    vkn_pass_targets(n0, col, lay, mip, dep);
    for (uint32_t i = 0; i <= VKN_PASS_COLORS; i++) if (held[i]) vkn_tex_unref(held[i]);
    vkn_fx_bypass(0); g.self = 0;
    if (g_chain_out) *g_chain_out = chain_keep;   // [fixpp] the engine draw's effect output as the chain had it
    return bad < 0 ? (int)n + 1 : bad;
}

// ---- mode 2: the would-be hit compared with the truth (at retire: the GPU is done) ----
typedef struct vjob { vkn_block a, b; uint64_t n; uint32_t w, bpp; int k; uint64_t frame; uint8_t x8, tol, cz; nr_ss_rect keep; } vjob;   // tol: [VK-SHADOWS-MOVING] a scrolled range (counted, not byte-equal)   // x8: an X8_D24 atlas ([VK-DRV])
static void verify_done(void *ctx) {
    vjob *j = (vjob *)ctx;
    const uint8_t *x = (const uint8_t *)j->a.cpu, *y = (const uint8_t *)j->b.cpu;
    // X8_D24: the top byte of each 32-bit texel of a buffer copy is not defined by Vulkan - compared without it
    if (x && y && j->x8) for (uint64_t q = 0; q + 4 <= j->n; q += 4) { ((uint8_t *)j->a.cpu)[q + 3] = 0; ((uint8_t *)j->b.cpu)[q + 3] = 0; }
    if (x && y && j->tol) {   // [VK-SHADOWS-MOVING] the scrolled tile against the drawn one: texels differing, the largest depth step
        uint64_t px = 0, k1 = 0, kn = 0, st = 0; uint32_t mx = 0;
        for (uint64_t q = 0; q + j->bpp <= j->n; q += j->bpp) {
            uint32_t u = 0, v = 0; memcpy(&u, x + q, j->bpp); memcpy(&v, y + q, j->bpp);
            if (u != v) {
                px++; const uint32_t d = u > v ? u - v : v - u; if (d > mx) mx = d;
                const uint64_t i = q / j->bpp; const int32_t tx = (int32_t)(i % j->w), ty = (int32_t)(i / j->w);   // [fix 22.11] by kind
                if (tx >= j->keep.x0 && tx < j->keep.x1 && ty >= j->keep.y0 && ty < j->keep.y1) { if (d == 1) k1++; else kn++; } else st++;
            }
        }
        gs.c.sv_cmp++; gs.c.sv_all += j->n / j->bpp; gs.c.sv_px += px; if (mx > gs.c.sv_max) gs.c.sv_max = mx;
        { const int c = j->cz ? 1 : 0; gs.c.svc_cmp[c]++; gs.c.svc_all[c] += j->n / j->bpp; gs.c.svc_k1[c] += k1; gs.c.svc_kn[c] += kn; gs.c.svc_st[c] += st; if (!px) gs.c.svc_eq[c]++; if (mx > gs.c.svc_max[c]) gs.c.svc_max[c] = mx; }
        if (!px) { g.s.verify_eq++; gs.c.sv_eq++; }
    } else if (x && y && !memcmp(x, y, j->n)) g.s.verify_eq++;
    else if (x && y) {
        g.s.verify_ne++;
        uint64_t i = 0, bad = 0; while (i < j->n && x[i] == y[i]) i++;
        for (uint64_t q = i; q < j->n; q++) bad += x[q] != y[q];
        const uint64_t px = i / j->bpp;
        SHC_RED(R_VERIFY, "verify: cascade %d (frame %llu): the cached tile differs from the drawn one in %llu bytes, first at pixel (%llu, %llu)", j->k,
                (unsigned long long)j->frame, (unsigned long long)bad, (unsigned long long)(px % j->w), (unsigned long long)(px / j->w));
    }
    vkn_block_free(&j->a); vkn_block_free(&j->b); free(j);
}
static void verify_run(int k, const int32_t r[4]) {
    g.vnext[k] = g.frame + g.verify_every; g.s.verify_runs++;
    const int scr = gs.V.on;   // [VK-SHADOWS-MOVING] a scrolled range: the tile cleared, the scrolled layer, then the drawn casters
    if (!g.X && !(g.X = img_make("shc verify"))) { g.s.verify_skip++; return; }
    if (scr) { gs.rp_comp = 1; const int b = replay(g.X, r, g.P.bits, RP_DRAWN); gs.rp_comp = 0; if (b) { g.s.verify_skip++; return; } }
    else if (copy_tile(g.S, g.X, r) || replay(g.X, NULL, 0, RP_DRAWN)) { g.s.verify_skip++; return; }
    vjob *j = (vjob *)calloc(1, sizeof *j);
    if (!j) { g.s.verify_skip++; return; }
    if (read_tile(g.atlas, r, &j->a)) { free(j); g.s.verify_skip++; return; }
    if (read_tile(g.X, r, &j->b)) { vkn_retire_block(&j->a); free(j); g.s.verify_skip++; return; }
    const VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(vkn_cb(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    j->bpp = depth_bpp(g.atlas->format); j->x8 = g.atlas->format == VK_FORMAT_X8_D24_UNORM_PACK32; j->n = (uint64_t)r[2] * (uint64_t)r[3] * j->bpp; j->w = (uint32_t)r[2]; j->k = k; j->frame = g.frame;
    j->tol = (uint8_t)scr; j->cz = (uint8_t)gs.V.cz; j->keep = gs.V.keep;
    vkn_retire(verify_done, j);
}

// ---- the work a cascade's end leaves (run at the safe point: in the fx function, before the draw in hand is looked at) ----
static void run_pending(void) {
    const int kind = g.P.kind, k = g.P.k;
    g.pend = 0; g.P.kind = P_NONE;
    nrshc_cas *c = g.cas[k];
    if (kind == P_BUILD) {
        if ((!g.S && !(g.S = img_make("shc layer"))) || replay(g.S, g.P.rect, g.P.bits, RP_DRAWN)) {
            c->layer = 0; g.slot_real[k] = 0; g.s.build_fail++;
            SHC_RED(R_BUILD, "cascade %d: the layer could not be drawn (image %s, %llu replayed draws dropped so far): no layer", k, g.S ? "ok" : "missing", (unsigned long long)g.s.replay_drop);
            return;
        }
        g.s.build_ok++; g.slot_real[k] = 1;
        for (int i = 0; i < NRSHC_CAS; i++)   // S holds one layer per tile: a slot whose layer covers this tile lost it
            if (i != k && g.cas[i]->layer && overlap(g.cas[i]->rect, g.P.rect)) { g.cas[i]->layer = 0; g.slot_real[i] = 0; g.s.evicted++; }
    } else if (kind == P_RECOVER) {
        g.s.recover_now++;
        const int bad = replay(g.atlas, g.P.rect, g.P.bits, RP_ALL);
        if (bad) { g.s.recover_bad++; SHC_RED(R_RECOVER, "cascade %d: repaired with %d of its draws missing (this frame's tile lacks their shadows)", k, bad); }
    } else if (kind == P_VERIFY) verify_run(k, g.P.rect);
}
static void pend(int kind) { g.pend = 1; g.P.kind = kind; g.P.k = g.R.k; memcpy(g.P.rect, g.R.rect, sizeof g.P.rect); g.P.bits = g.R.clear_bits; }
// Work that can no longer be done (the atlas is read / copied, or the recording ends, before the next draw): counted.
static void pend_lost(int why) {
    if (!g.pend) return;
    if (g.P.kind == P_BUILD && why == END_CLOSE) { g.cas[g.P.k]->layer = 0; g.slot_real[g.P.k] = 0; g.s.build_lost++; }
    else if (g.P.kind == P_BUILD) return;   // the layer does not depend on the atlas: still built at the next draw of this recording
    else if (g.P.kind == P_RECOVER && why == END_CLEAR) g.s.recover_cleared++;   // the engine cleared the tile anyway
    else if (g.P.kind == P_RECOVER) {
        g.s.recover_lost++;
        SHC_RED(R_LOST, "cascade %d: a failed hit could not be repaired before the atlas was %s (this frame's tile holds the old layer)", g.P.k,
                why == END_READ ? "read" : why == END_USE ? "copied / transitioned" : "submitted");
    } else g.s.verify_skip++;
    g.pend = 0; g.P.kind = P_NONE;
}

// ---- a cascade ends ----
static void end_range(int why) {
    g.live = 0;
    nrshc_range *r = g.rg;
    if (g.R.refused || g.R.k < 0 || !r || !r->live) return;
    const int k = g.R.k; nrshc_cas *c = g.cas[k];
    if (why == END_CLEAR) r->tainted = 1;   // a clear inside a cascade (as the old path: never cached)
    gs.V.on = 0;
    if (gs.R.on && r->action == NRSHC_A_HIT) scroll_absent(k, r);   // [VK-SHADOWS-MOVING] members that did not come: gone with the scroll, or the range is broken
    const int broke = r->tainted || r->overflow, act = r->action;
    if (broke) g.s.tranges++;
    g.slot_unrep[k] = g.R.unrep;
    g.s.cranges[k]++; g.s.known[k] += r->known; g.s.newu[k] += r->new_ubo; g.s.newo[k] += r->new_other; g.s.unc[k] += r->unc; g.s.ncont[k] += r->new_content;
    const int e = nrshc_end(c, &g.par, r, g.atlas_gen, g.R.rect, g.R.clear_bits, 1);
    if (broke) g.dirty = 1;   // later cascades of this frame are not cached
    if (act == NRSHC_A_HIT && e == NRSHC_E_NONE) g.s.hit_ok++;
    if (e == NRSHC_E_RECOVER) g.s.recover++;
    if (e == NRSHC_E_BUILD) g.s.build++;
    if (e == NRSHC_E_REST) g.s.rest++;
    if (e == NRSHC_E_RECOVER || e == NRSHC_E_BUILD) g.slot_real[k] = 0;
    if (gs.scroll && gs.sl) {   // [VK-SHADOWS-MOVING] the slot's members and anchor; a scrolled hit's strips; the refresh rule
        if (e == NRSHC_E_BUILD) scroll_built(k, r);
        else if (e == NRSHC_E_RECOVER || e == NRSHC_E_REST) scroll_slot_reset(k);
        else if (e == NRSHC_E_NONE && act == NRSHC_A_HIT && gs.R.on) {
            // casters that came into the cascade since the build and stayed are drawn every frame until they are members: when they are
            // a quarter of the layer (at least the model's minimum), the layer is built again
            const uint32_t lim = c->nmem / 4 > g.par.min_static ? c->nmem / 4 : g.par.min_static;
            if (!scroll_commit(k, r) && gs.R.fresh >= lim) {
                if (gs.job[k].on) { gs.job[k].on = 0; gs.njob--; }   // (its strips are not needed: the layer is made again)
                scroll_drop(k); gs.c.refresh++;
            }
        }
    }
    if (e == NRSHC_E_BUILD) {   // the members were drawn into the atlas in the engine's order: now into S
        pend(P_BUILD);
        if (why == END_CLOSE) pend_lost(END_CLOSE);
    } else if (e == NRSHC_E_RECOVER && act == NRSHC_A_HIT && g.mode != NRSHC_CHECK) {   // (a broken BUILD drew everything: nothing to do)
        pend(P_RECOVER);
        if (why != END_NEXT) pend_lost(why);
    } else if (e == NRSHC_E_NONE && g.R.verify) {
        if (why == END_NEXT) pend(P_VERIFY); else g.s.verify_skip++;
    }
    if (g.R.far && g.rate >= 2) {   // [gpu2 P8] a reused tile that held (no RECOVER) is last frame's; any other far tile is this frame's: into F
        if (g.R.reuse && e != NRSHC_E_RECOVER) g.s.rate_reused++;
        else if (why != END_CLEAR && why != END_USE) { g.f_pend = 1; memcpy(g.fp_rect, g.R.rect, sizeof g.fp_rect); }
        if (g.R.reuse && e == NRSHC_E_RECOVER) g.s.rate_recover++;
    }
}
// [gpu2 P8] mode 3 (verify): the tile F holds (the far tile of the frame before) against the tile this frame drew, at retire
typedef struct rjob { vkn_block a, b; uint64_t n; uint32_t bpp; uint8_t x8; } rjob;
static void rate_cmp_done(void *ctx) {
    rjob *j = (rjob *)ctx;
    const uint8_t *x = (const uint8_t *)j->a.cpu, *y = (const uint8_t *)j->b.cpu;
    if (x && y) {
        uint64_t px = 0; uint32_t mx = 0;
        for (uint64_t q = 0; q + j->bpp <= j->n; q += j->bpp) {
            uint32_t u = 0, v = 0; memcpy(&u, x + q, j->bpp); memcpy(&v, y + q, j->bpp);
            if (j->x8) { u &= 0xffffffu; v &= 0xffffffu; }   // (the top byte of an X8_D24 texel copy is not defined)
            if (u != v) { px++; const uint32_t d = u > v ? u - v : v - u; if (d > mx) mx = d; }
        }
        g.s.rate_cmp++; g.s.rate_cmp_px += px; if (mx > g.s.rate_cmp_max) g.s.rate_cmp_max = mx; g.s.rate_cmp_all += j->n / j->bpp;
    }
    vkn_block_free(&j->a); vkn_block_free(&j->b); free(j);
}
// At the safe point after a drawn far cascade: (mode 3: compare F with it every second frame), then the tile into F.
static void far_save(void) {
    g.f_pend = 0;
    if (!g.atlas || (!g.F && !(g.F = img_make("shc far")))) return;
    const int32_t *r = g.fp_rect;
    if (g.rate == 3 && g.f_ok && g.f_frame + 1 == g.frame && g.f_gen == g.atlas_gen && !memcmp(g.f_rect, r, sizeof g.f_rect) && (g.frame & 1)) {
        rjob *j = (rjob *)calloc(1, sizeof *j);
        if (j && !read_tile(g.F, r, &j->a)) {
            if (!read_tile(g.atlas, r, &j->b)) {
                const VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
                vkCmdPipelineBarrier(vkn_cb(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
                j->bpp = depth_bpp(g.atlas->format); j->x8 = g.atlas->format == VK_FORMAT_X8_D24_UNORM_PACK32; j->n = (uint64_t)r[2] * (uint64_t)r[3] * j->bpp;
                vkn_retire(rate_cmp_done, j); j = NULL;
            } else vkn_retire_block(&j->a);
        }
        free(j);
    }
    if (copy_tile(g.atlas, g.F, r)) { g.f_ok = 0; return; }
    g.f_ok = 1; memcpy(g.f_rect, r, sizeof g.f_rect); g.f_frame = g.frame; g.f_gen = g.atlas_gen; g.f_bits = g.clear_bits; g.s.rate_saved++;
}

// ---- precopy: one pass break a frame instead of one per hit. At the frame's first cascade (the atlas cleared this frame, the
// remembered clear not yet run) every slot whose layer nrshc_arm would take as a hit - layer drawn, same atlas / clear value, not too
// old, previous cascade clean, tile inside the atlas - gets its tile copied from S now, before the atlas pass opens. A hit of that
// slot then copies nothing. A precopied tile that is not taken by a hit is put back to the clear value before anything else uses it:
// at the start of a cascade that draws into it (vkCmdClearAttachments in the atlas pass) or at the next draw outside the atlas (the
// atlas pass reopened for the clear); a read / copy / submit of the atlas that comes first is counted as lost (red line). ----
static void clear_in_pass(const int32_t r[4]) {
    vkn_pass_info pi;
    if (vkn_pass_ready(&pi) || !vkn_cb()) { g.s.pre_lost++; SHC_RED(R_PRE, "a precopied tile (%d %d %d %d) could not be put back: no atlas pass", r[0], r[1], r[2], r[3]); return; }
    float d; memcpy(&d, &g.clear_bits, 4);
    const VkClearAttachment ca = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .clearValue.depthStencil = {d, 0}};
    const VkClearRect cr = {{{r[0], r[1]}, {(uint32_t)r[2], (uint32_t)r[3]}}, 0, 1};
    vkCmdClearAttachments(vkn_cb(), 1, &ca, 1, &cr);
    vkn_drv_cleared();   // [stage 5.3 V2] DM_GFX_CLEAR_REBIND
    g.s.tile_clears++;
}
static void precopy(void) {
    g.pre = 0; g.pre_frame = g.frame;
    if (g.mode != NRSHC_ON || !g.S || g.dirty || g.img_fail || !cond_ok() || !(g.clear_seen && g.clear_frame == g.frame)) return;
    uint32_t m = 0;
    for (int j = 0; j < NRSHC_CAS; j++) {
        const nrshc_cas *c = g.cas[j]; const int32_t *r = c->rect;
        if (!c->layer || !g.slot_real[j] || g.slot_unrep[j] || !c->ranges || !c->prev_ok || c->atlas != g.atlas_gen || c->clear_bits != g.clear_bits || c->layer_ranges >= g.par.max_age) continue;
        if (gs.sl && gs.sl[j].anch && !gs.sl[j].plain) continue;   // [VK-SHADOWS-MOVING] a scrolled layer is drawn by the piece, never copied
        if (r[0] < 0 || r[1] < 0 || r[2] <= 0 || r[3] <= 0 || (uint64_t)r[0] + (uint64_t)r[2] > g.atlas->w || (uint64_t)r[1] + (uint64_t)r[3] > g.atlas->h) continue;
        // [stage 5.1 A5 fix] the slot's last cascade was another tile (a cascade split by a clear / read took this slot for the rest of
        // another tile, keeping the layer): nrshc_pick_slot gives this tile to the slot that last drew it, so the copy would be put back
        // unused every frame (host test: frames after a split showed "put back" 1-2 a frame)
        if (memcmp(g.slot_rect[j], r, sizeof g.slot_rect[j])) continue;
        int ov = 0; for (int i = 0; i < j; i++) if ((m >> i & 1) && overlap(g.cas[i]->rect, r)) ov = 1;
        if (ov) continue;
        if (copy_tile(g.S, g.atlas, r)) break;
        m |= 1u << j; memcpy(g.pre_rect[j], r, sizeof g.pre_rect[j]); g.s.precopied++;
    }
    g.pre = m;
}
// Precopied tiles a cascade of rect draws into without taking them (in the engine's atlas pass, at the cascade's first draw).
static void pre_undo_overlapping(const int32_t rect[4]) {
    for (int j = 0; j < NRSHC_CAS; j++)
        if ((g.pre >> j & 1) && overlap(g.pre_rect[j], rect)) { clear_in_pass(g.pre_rect[j]); g.pre &= ~(1u << j); g.s.pre_undone++; }
}
// Every precopied tile left (at a draw outside the atlas: the safe point; the engine's targets are put back).
static void pre_undo_all(void) {
    for (int j = 0; j < NRSHC_CAS; j++)
        if (g.pre >> j & 1) {
            g.pre &= ~(1u << j); g.s.pre_undone++;
            if (replay(g.atlas, g.pre_rect[j], g.clear_bits, RP_NONE)) { g.s.pre_lost++; SHC_RED(R_PRE, "precopied tile of slot %d could not be put back", j); }
        }
}
static void pre_lost(int why) {
    if (!g.pre) return;
    if (why != END_CLEAR) {   // (a clear wipes them as the engine wants)
        g.s.pre_lost++;
        SHC_RED(R_PRE, "precopied tiles %#x were neither hit nor put back before the atlas was %s (this frame's tiles hold old layers)", g.pre,
                why == END_READ ? "read" : why == END_USE ? "copied / transitioned" : "submitted");
    }
    g.pre = 0;
}

// ---- a cascade begins: the old path's arm() ----
static void start_range(const int32_t rect[4], int exact) {
    g.s.ranges++; g.live = 1;
    memset(&g.R, 0, sizeof g.R); g.R.k = -1; memcpy(g.R.rect, rect, sizeof g.R.rect);
    if (model_alloc()) { g.R.refused = 1; return; }
    if (!g.arms && g.pre_frame != g.frame) precopy();   // the frame's first cascade
    const int rf = shape_why(g.atlas, 0, g.arms);
    if (rf != NRSHC_RF_OK) { g.R.refused = 1; g.s.refused++; g.s.rf[rf]++; return; }
    const int k = nrshc_pick_slot((const int32_t (*)[4])g.slot_rect, g.slot_has, g.used, rect);
    if (k < 0) { g.R.refused = 1; g.s.refused++; g.s.rf[NRSHC_RF_SLOTS]++; return; }
    g.R.k = k; g.used |= 1u << k; g.arms++; memcpy(g.slot_rect[k], rect, sizeof g.slot_rect[k]); g.slot_has[k] = 1;
    const vkn_tex *a = g.atlas;
    const int rect_ok = exact && rect[0] >= 0 && rect[1] >= 0 && rect[2] > 0 && rect[3] > 0 && (uint64_t)rect[0] + (uint64_t)rect[2] <= a->w && (uint64_t)rect[1] + (uint64_t)rect[3] <= a->h;
    int ov = 0;
    for (int i = 0; i < g.ntiles; i++) if (overlap(g.tiles[i], rect)) ov = 1;
    if (g.ntiles < NRSHC_CAS) memcpy(g.tiles[g.ntiles++], rect, sizeof g.tiles[0]);
    const int clear_ok = g.clear_seen && g.clear_frame == g.frame;
    g.R.clear_bits = g.clear_bits;
    // backend: the image could be made; no GPU predicates (DM_VK_NATIVE_COND 1 / 3: cond_ok above; 2 / 4 are fine)
    const int cok = cond_ok(), backend_ok = !g.img_fail && cok;
    if (!cok) g.cond_ref++;
    nrshc_cas *c = g.cas[k];
    int why = 0;
    int act = nrshc_arm(c, &g.par, g.atlas_gen, rect, clear_ok, g.R.clear_bits, g.dirty || ov, rect_ok, backend_ok, &why);
    // no layer drawn for it; or its last cascade had a draw this module cannot draw again (a RECOVER would lose it): no hit
    if (act == NRSHC_A_HIT && (!g.S || !g.slot_real[k] || g.slot_unrep[k])) { c->layer = 0; act = NRSHC_A_NORMAL; why = NRSHC_W_OFF; }
    scroll_arm(k, rect, &act, &why);   // [VK-SHADOWS-MOVING] the reference of the cascade; a hit as before / scrolled / dropped
    g.s.armed++; g.s.act[act]++; g.s.why[why]++;
    nrshc_begin(c, &g.par, g.rg, act, why);
    if (g.rg->blind) g.s.blind++;
    g.R.save = act == NRSHC_A_BUILD || (act == NRSHC_A_HIT && g.mode != NRSHC_CHECK);
    g.R.verify = act == NRSHC_A_HIT && g.mode == NRSHC_CHECK && g.verify_every && g.frame >= g.vnext[k];
    if (g.R.verify) g.R.save = 1;
    if (gs.R.on) g.R.save = 1;   // [VK-SHADOWS-MOVING] (the strip touchers are kept draws)
    const uint32_t kb = 1u << k;
    const int taken = act == NRSHC_A_HIT && g.mode == NRSHC_ON && (g.pre & kb) && !memcmp(g.pre_rect[k], rect, sizeof g.pre_rect[k]);
    if (taken) { g.pre &= ~kb; g.s.pre_hit++; }   // its layer is in the tile already
    if (g.pre) pre_undo_overlapping(rect);
    if (gs.R.on) {   // [VK-SHADOWS-MOVING] the scrolled layer is drawn into the tile inside the pass of the engine (no copy, no pass break)
        if (taken) clear_in_pass(rect);   // (the precopied tile holds the layer as it lay before this shift)
        if (g.mode == NRSHC_ON && gs.scroll == 1) {   // (mode 2: nothing is skipped; scroll 3, poison: the kept part stays clear)
            if (scroll_comp(rect, &gs.R.keep, gs.R.O2, gs.R.dz)) {
                g.rg->action = NRSHC_A_NORMAL; scroll_drop(k); g.R.save = 0; gs.R.on = 0; gs.c.quad_fail++;
                SHC_RED(R_SCROLL, "cascade %d: the scrolled layer could not be drawn into the tile: drawn as the engine asks", k);
            } else gs.c.quad++;
        }
    } else if (act == NRSHC_A_HIT && g.mode == NRSHC_ON && !taken && copy_tile(g.S, g.atlas, rect)) {   // mode 3 (poison): the cleared tile stays
        // no copy: nothing may be skipped (the frame stays exact); the model sees a normal range
        g.rg->action = NRSHC_A_NORMAL; c->layer = 0; g.slot_real[k] = 0; g.R.save = 0;
        SHC_RED(R_IMG, "cascade %d: the layer copy could not be recorded: drawn as the engine asks", k);
    }
    // [gpu2 P8] the far cascade every second frame: its static part is a hit (the members come with this frame's hashes - the light's
    // projection did not move, else the end is a RECOVER: every kept draw drawn again, exact), and F holds the far tile the frame before
    // drew: F's tile goes into the atlas and every draw of the cascade is skipped (kept: a RECOVER draws them all)
    g.R.far = g.cur_far;
    if (g.cur_far && g.rate >= 2) {
        const int can = g.rg->action == NRSHC_A_HIT && !gs.R.on && g.F && g.f_ok && g.f_frame + 1 == g.frame && g.f_gen == g.atlas_gen &&
                        g.f_bits == g.R.clear_bits && !memcmp(g.f_rect, rect, sizeof g.f_rect);
        if (can && g.rate == 2 && g.mode == NRSHC_ON && g.R.save) {
            if (!copy_tile(g.F, g.atlas, rect)) g.R.reuse = 1;
        } else if (can) g.s.rate_would++;
        g.s.rate_far++;
    }
}

// ---- one draw of an armed cascade: its hash (everything its depth output depends on), the model's decision, the copy for replays ----
// [rt P3-GPU] DM_VK_NATIVE_SHC_MASK (vkn_shc.h): the rows the caster's shader reads
extern int vkn_shlib_on(void) __attribute__((weak));
extern int vkn_shlib_read_mask(const void *shader, uint32_t binding, const uint64_t **bits, uint32_t *nwords) __attribute__((weak));
static int g_shm; static vkn_shc_mask_stats g_shms, g_shmo;
static int (*g_t_shmask)(const vkn_shader *s, int src, uint32_t binding, const uint64_t **bits, uint32_t *nwords);
int vkn_shc_mask(void) { return g_shm; }
void vkn_shc_mask_set(int mode) { g_shm = mode == 1 || mode == 2 ? mode : 0; }
void vkn_shc_mask_stats_get(vkn_shc_mask_stats *s) { if (s) *s = g_shms; }
void vkn_shc_test_mask(int (*fn)(const vkn_shader *s, int src, uint32_t binding, const uint64_t **bits, uint32_t *nwords)) { g_t_shmask = fn; }
static int shc_rmask(const vkn_shader *s, int src, uint32_t binding, const uint64_t **bits, uint32_t *nw) {   // 1 = bits, 0 / -1 = the whole block
    if (!g_shm || !s || vkn_fx_shader_is(s)) return -1;
    if (g_t_shmask) return g_t_shmask(s, src, binding, bits, nw);
    if (vkn_shlib_on && vkn_shlib_on() && vkn_shlib_read_mask) return vkn_shlib_read_mask(s->xs[src], binding, bits, nw);
    return -1;
}
// The hash of the rows of [0, n) set in bits (runs of consecutive rows in one nrshc_bytes); the run positions mixed in.
uint64_t vkn_shc_masked_hash(const uint8_t *p, uint32_t n, const uint64_t *bits, uint32_t nw, uint32_t *hashed) {
    uint64_t h = nrshc_mix(0x55424d31ull, n); uint32_t row = 0; const uint32_t rows = (n + 15u) / 16u; *hashed = 0;
    while (row < rows) {
        while (row < rows && !(row / 64u < nw && bits[row / 64u] >> (row & 63) & 1)) row++;
        if (row >= rows) break;
        const uint32_t r0 = row;
        while (row < rows && row / 64u < nw && bits[row / 64u] >> (row & 63) & 1) row++;
        const uint32_t at = r0 * 16u, end = row * 16u < n ? row * 16u : n;
        h = nrshc_bytes(nrshc_mix(h, r0), p + at, end - at); *hashed += end - at;
    }
    return h;
}
// [rt P4] DM_VK_NATIVE_SHC_EHASH (vkn_rt.h): the engine thread's hash of the whole block, when the shader reads all of it (n == size)
static struct { uint64_t used, verified, differ, na; } g_eh;
static uint64_t g_ehc[3];   // (totals: used, verified, differ)
void vkn_shc_ehash_stats(uint64_t *used, uint64_t *verified, uint64_t *differ) { *used = g_ehc[0]; *verified = g_ehc[1]; *differ = g_ehc[2]; }
static uint64_t ubo_hash(int st, uint32_t slot, uint32_t off, uint32_t n, uint32_t size, const vkn_shader *sh, const vkn_bind *b) {
    const uint64_t *mbits = NULL; uint32_t mnw = 0;
    const int mk = g_shm ? shc_rmask(sh, st == 0 ? VKN_SRC_VS : VKN_SRC_FS, b->binding, &mbits, &mnw) : -1;   // [rt P3-GPU]
    const void *mkey = g_shm == 1 && mk == 1 ? (const void *)mbits : NULL;
    if (slot < VKN_UBO_SLOTS) {
        const __typeof__(g.um[0][0]) *m = &g.um[st][slot];
        if (m->frame == g.frame + 1 && m->off == off && m->n == n && m->mk == mkey) { g.s.ubo_memo++; return m->h; }
    }
    if (g_shm) {   // [rt P3-GPU] the masked hash (mode 1: used; mode 2: counted only)
        if (!g.ring) g.ring = (const uint8_t *)vkn_ring_cpu();
        if (g.ring && (uint64_t)off + n <= VKN_RING_BYTES) {
            g_shms.blocks++; g_shms.bytes_whole += n;
            if (mk == 1 && mbits) {
                uint32_t hashed = 0; const uint64_t hm = vkn_shc_masked_hash(g.ring + off, n, mbits, mnw, &hashed);
                g_shms.bytes_masked += hashed; if (hashed < n) g_shms.narrowed++;
                if (g_shm == 1) {
                    g.s.ubo_bytes += hashed;
                    if (slot < VKN_UBO_SLOTS) { g.um[st][slot].frame = g.frame + 1; g.um[st][slot].off = off; g.um[st][slot].n = n; g.um[st][slot].h = hm; g.um[st][slot].mk = mkey; }
                    return hm;
                }
            } else g_shms.bytes_masked += n;
        }
    }
    const int em = vkn_rt_ehash();
    uint64_t eh = 0; const int have = em && n == size && !vkn_draw_ubo_hash(st, slot, off, size, &eh);
    if (em && !have) g_eh.na++;
    if (have && em == 1) {
        g_eh.used++; g_ehc[0]++;
        if (slot < VKN_UBO_SLOTS) { g.um[st][slot].frame = g.frame + 1; g.um[st][slot].off = off; g.um[st][slot].n = n; g.um[st][slot].h = eh; g.um[st][slot].mk = NULL; }
        return eh;
    }
    if (!g.ring) g.ring = (const uint8_t *)vkn_ring_cpu();
    if (!g.ring || (uint64_t)off + n > VKN_RING_BYTES) return 0x5542ffffull;
    const uint64_t h = nrshc_bytes(0x55424f, g.ring + off, n); g.s.ubo_bytes += n;
    if (have) { g_eh.verified++; g_ehc[1]++; if (h != eh) g_ehc[2]++; if (h != eh && g_eh.differ++ < 16) SHC_RED(R_UBO, "DM_VK_NATIVE_SHC_EHASH=2: the engine thread's hash of block %u+%u (stage %d slot %u) differs", off, n, st, slot); }
    if (slot < VKN_UBO_SLOTS) { g.um[st][slot].frame = g.frame + 1; g.um[st][slot].off = off; g.um[st][slot].n = n; g.um[st][slot].h = h; g.um[st][slot].mk = NULL; }
    return h;
}
extern const vkn_draw_in *vkn_draw_rt_in;   // [rt2 F1] vkn_draw.c (the draw the record executes now)
static struct { uint64_t used, checked, moved; } g_vs;   // [rt2 F1] versions from the record: used / checked (EHASH=2) / the table had moved on
static void draw_one(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, const vkn_shs_b *bp) {
    const vkn_shader *sh = ctx->shader; nrshc_range *r = g.rg; const int k = g.R.k;
    int tw = NRSHC_T_OK, nw = NRS_OK, salt = 0;
#define TAINT(w) do { if (!tw) tw = (w); } while (0)
    sdraw t; t.ntex = t.nubo = 0; t.ok = 1; t.vread = 0; t.iver = 0;
    uint64_t vj = 0x4A, vt = 0x54, hu = 0x55424f;
    uint32_t ar; memcpy(&ar, &in->alpha_ref, 4);
    uint64_t h = nrshc_mix(0x53484331, (uint64_t)sh->id | (uint64_t)vkn_h_idx(sh->handle) << 32);
    h = nrshc_mix(h, sh->handle);
    h = nrshc_mix(h, (uint64_t)ctx->blend | (uint64_t)ctx->ds << 16 | (uint64_t)ctx->raster << 32 | (uint64_t)ctx->vfmt << 48);
    h = nrshc_mix(h, (uint64_t)in->prim | (uint64_t)in->stencil_ref << 8 | (uint64_t)ctx->indexed << 16 | (uint64_t)ar << 32);
    h = nrshc_mix(h, (uint64_t)ctx->first | (uint64_t)ctx->count << 32); h = nrshc_mix(h, ctx->instances);
    h = nrshc_bytes(h, in->viewport, sizeof in->viewport); h = nrshc_bytes(h, in->scissor, sizeof in->scissor);
    // the vertex streams the format reads, the index buffer: handles, offsets and content versions. [rt2 F1] The versions the engine thread
    // read at encode when the record carries them (vkn_rt.h vkn_draw_ver: the numbers of the bytes this draw reads); else the table
    const vkn_draw_ver *pv = vkn_draw_rt_ver && vkn_draw_rt_in == in && vkn_draw_rt_ver->ok ? vkn_draw_rt_ver : NULL;
    const vkn_vfmt *vf = ctx->vfmt ? vkn_vfmt_info(ctx->vfmt) : NULL;
    for (uint32_t i = 0; vf && i < vf->nbind; i++) {
        const uint32_t st = vf->bind[i].binding; if (st >= 3) continue;
        int none = 1; uint64_t v;
        if (pv) {
            v = pv->vb[st]; none = pv->vnone >> st & 1; g_vs.used++;
            if (vkn_rt_ehash() == 2 || vkn_rt_res() == 2) { int n2 = 1; const uint64_t v2 = vkn_buf_version(VKN_BUF_VB, in->vb[st], &n2); g_vs.checked++; if (v2 != v || n2 != none) g_vs.moved++; }
        } else v = vkn_buf_version(VKN_BUF_VB, in->vb[st], &none);
        h = nrshc_mix(h, in->vb[st]); h = nrshc_mix(h, (uint64_t)in->vb_off[st] | (uint64_t)st << 32);
        vj = nrshc_mix(vj, v); salt |= none; t.vread |= (uint8_t)(1u << st); t.vver[st] = v;
    }
    if (ctx->indexed) {
        int none = 1;
        if (pv) {
            t.iver = pv->ib; none = pv->inone; g_vs.used++;
            if (vkn_rt_ehash() == 2 || vkn_rt_res() == 2) { int n2 = 1; const uint64_t v2 = vkn_buf_version(VKN_BUF_IB, in->ib, &n2); g_vs.checked++; if (v2 != t.iver || n2 != none) g_vs.moved++; }
        } else t.iver = vkn_buf_version(VKN_BUF_IB, in->ib, &none);
        h = nrshc_mix(h, in->ib); vj = nrshc_mix(vj, t.iver); salt |= none;
    }
    if (in->prim == 2 || in->prim == 4 || in->prim == 7) t.ok = 0;   // fan / quads / loop: the draw path rewrites its indices (not replayed)
    const uint64_t h0 = h;   // [VK-SHADOWS-MOVING] the structure without bindings: shader, states, draw arguments, viewport, scissor, streams
    // what the shader reads: constant blocks by content, textures by pool id + object + upload number, samplers by object
    int feedback = 0;
    for (uint32_t i = 0; i < sh->nbind; i++) {
        const vkn_bind *b = &sh->bind[i];
        for (uint32_t e = 0; e < b->count; e++) {
            const int st = stage_src(b->stage); const uint32_t slot = (uint32_t)b->slot + e;
            switch (b->kind) {
            case VKN_BK_UNIFORM: {
                if (st < 0 || slot >= VKN_UBO_SLOTS) { TAINT(NRSHC_T_BINDING); break; }
                uint32_t off = 0, size = 0;
                if (vkn_draw_ubo_get(st, slot, &off, &size)) { h = nrshc_mix(h, 0x55420000u + slot + ((uint64_t)st << 8)); break; }
                const uint32_t n = size < (b->bytes ? b->bytes : 65536u) ? size : (b->bytes ? b->bytes : 65536u);
                h = nrshc_mix(h, 0x55430000u + slot + ((uint64_t)st << 8) + ((uint64_t)n << 32));
                hu = nrshc_mix(hu, ubo_hash(st, slot, off, n, size, sh, b));
                if (t.nubo < SV_UBO) { t.ub[t.nubo].st = (uint8_t)st; t.ub[t.nubo].slot = (uint8_t)slot; t.ub[t.nubo].off = off; t.ub[t.nubo].size = size; t.nubo++; }
                else t.ok = 0;
                break;
            }
            case VKN_BK_SUPPORT: case VKN_BK_C1: break;   // the viewport size (hashed above) / the shader's own constants (its object)
            case VKN_BK_TEXTURE: {
                if (st < 0 || slot >= VKN_TEX_SLOTS) { TAINT(NRSHC_T_BINDING); break; }
                const int32_t id = in->tex[st] ? in->tex[st][slot] : -1, sid = in->smp[st] ? in->smp[st][slot] : -1;
                h = nrshc_mix(h, (uint64_t)(uint32_t)id | (uint64_t)(uint32_t)sid << 32);
                if (id >= 0) {
                    vkn_tex *tx = NULL; (void)vkn_tex_view_by_id((uint32_t)id, &tx);
                    h = nrshc_mix(h, (uint64_t)(uintptr_t)tx);
                    if (tx && tx == g.atlas) feedback = 1;
                    else if (!tx || tx->pass_ever) salt = 1;   // unknown, or written by the GPU (no upload number stands for its texels)
                    else vt = nrshc_mix(vt, __atomic_load_n(&tx->shc_up, __ATOMIC_ACQUIRE));
                }
                if (sid >= 0) h = nrshc_mix(h, (uint64_t)(uintptr_t)vkn_sampler_by_id((uint32_t)sid));
                if (t.ntex < SV_TEX) { t.tx[t.ntex].st = (uint8_t)st; t.tx[t.ntex].slot = (uint8_t)slot; t.tx[t.ntex].id = id; t.tx[t.ntex].sid = sid; t.ntex++; }
                else t.ok = 0;
                break;
            }
            case VKN_BK_IMAGE: case VKN_BK_STORAGE: TAINT(NRSHC_T_STORAGE); break;
            default: TAINT(NRSHC_T_BINDING); break;   // driver tables, bindless, effect kinds: what they read is not known here
            }
        }
    }
    // order-freedom (nr_sort.h nrs_why): depth only (no colour target: the caller checked), fixed point, min or no depth effect
    {
        nrs_in ni; memset(&ni, 0, sizeof ni);
        ni.depth_att = 1; ni.depth_fixed = 1; ni.feedback = (uint8_t)feedback;
        const VkPipelineDepthStencilStateCreateInfo *ds = ctx->ds ? vkn_ds_info(ctx->ds) : NULL;   // id 0 = depth-stencil off
        if (ds) { ni.dt = (uint8_t)!!ds->depthTestEnable; ni.dw = (uint8_t)!!ds->depthWriteEnable; ni.op = (uint8_t)ds->depthCompareOp; ni.st = (uint8_t)!!ds->stencilTestEnable; ni.bounds = (uint8_t)!!ds->depthBoundsTestEnable; }
        nw = nrs_why(&ni);
        if (nw != NRS_OK) TAINT(NRSHC_T_ORDER);
    }
#undef TAINT
    if (!t.ok) { salt = 1; g.R.unrep = 1; g.s.unrep++; }   // a draw that cannot be drawn again is never a member (and no hit next time)
    const uint64_t v0 = nrshc_mix(vj, vt), v = salt ? nrshc_mix(v0, nrshc_mix(0x5A17, ++g.salt_n)) : v0;
    const uint64_t hs = h, hn = nrshc_mix(hs, hu), hf = nrshc_mix(hn, v);
    if (salt) g.s.salted++;
    if (tw != NRSHC_T_OK) { g.s.tainted++; g.s.tw[tw]++; if (tw == NRSHC_T_ORDER) g.s.nrs[nw]++; }
    const uint32_t o = r->ndraw;
    // [VK-SHADOWS-MOVING] a caster whose depth depends only on its mesh and its affine clip transform, with no fragment stage that
    // matters: the model gets structure + instance instead of the bytes (the instance is the same under a moved projection)
    uint64_t hm = hf; t.inst = -1; t.tcls = t.touch = 0; t.mh = 0; memset(&t.lb, 0, sizeof t.lb); t.z0 = t.z1 = 0.0f;
    if (gs.scroll && bp && gs.R.have && t.ok && tw == NRSHC_T_OK && !salt && scroll_tdraw(in, ctx, bp, h0, vj, &hm, &t.inst)) {
        t.tcls = 1; t.mh = hm; t.z0 = bp->z0; t.z1 = bp->z1;
        t.lb.x0 = bp->px.x0 - g.R.rect[0]; t.lb.x1 = bp->px.x1 - g.R.rect[0]; t.lb.y0 = bp->px.y0 - g.R.rect[1]; t.lb.y1 = bp->px.y1 - g.R.rect[1];
    }
    const uint32_t streak0 = gs.R.on ? nrshc_tab_get(&g.cas[k]->hist[g.cas[k]->hc], hm) : 0;
    int skip = nrshc_draw(g.cas[k], &g.par, r, hm, hs, hn, nrshc_vtag(vj, 0, vt), v, tw == NRSHC_T_OK);
    g.s.draws++; g.s.cdraws[k]++;
    if (gs.R.on && !skip && tw == NRSHC_T_OK && streak0 + 1 >= g.par.stable) gs.R.fresh++;
    if (gs.scroll && r->action == NRSHC_A_HIT && skip && !scroll_member(&t, bp, r)) { skip = 0; if (o < NRSHC_DRAWS) r->f[o] = NRSHC_F_DRAWN; }
    if (g.R.save && o < NRSHC_DRAWS) {
        sdraw *sd = &g.sv[o];
        *sd = t; sd->in = *in; sd->first = ctx->first; sd->count = ctx->count; sd->instances = ctx->instances; sd->indexed = ctx->indexed;
        g.s.saved++;
        if (!t.ok) SHC_RED(R_SAVE, "cascade %d draw %u: not replayable (engine primitive %u, %u textures, %u constant blocks)", k, o, in->prim, t.ntex, t.nubo);
    }
    if (r->action == NRSHC_A_HIT && skip) { g.s.skipped++; g.s.cskip[k]++; if (g.mode != NRSHC_CHECK) out->skip = 1; }
    // [gpu2 P8] a reused far tile: a moving caster's shadow is last frame's - only a draw a RECOVER can draw again is skipped (kept, replayable);
    // another one is drawn over F's tile (its shadow this frame's, its old one stays for this frame: counted)
    else if (g.R.reuse && r->action == NRSHC_A_HIT && g.mode == NRSHC_ON) { if (t.ok && o < NRSHC_DRAWS) { out->skip = 1; g.s.rate_skipped++; } else g.s.rate_drawn++; }
}

// ---- the atlas ----
static void on_watch(vkn_tex *t, int ev, float depth);
static void atlas_drop(void) {
    if (g.live) end_range(END_USE);
    pend_lost(END_CLOSE); pre_lost(END_USE);
    vkn_pass_watch(NULL, NULL);
    if (g.atlas) vkn_tex_unref(g.atlas);
    g.atlas = NULL; layers_drop(); img_free(); g.img_fail = 0; g.clear_seen = 0;
}
static int atlas_take(vkn_tex *d, uint64_t frame) {
    if (!vkn_tex_ref(d)) return 0;
    const int same_shape = g.atlas && g.atlas->format == d->format && g.atlas->w == d->w && g.atlas->h == d->h;
    if (g.atlas) {
        vkn_logf("shcache: the shadow atlas changes from %ux%u (%s) to %ux%u (%s): every layer dropped", g.atlas->w, g.atlas->h, g.atlas->sname, d->w, d->h, d->sname);
        vkn_pass_watch(NULL, NULL); vkn_tex_unref(g.atlas);
    } else vkn_logf("shcache: shadow atlas %ux%u format %d (%s)", d->w, d->h, (int)d->format, d->sname);
    g.atlas = d; g.atlas_gen++; g.atlas_last = frame; g.s.atlas_changes++; g.clear_seen = 0;
    layers_drop();
    if (!same_shape) { img_free(); g.img_fail = 0; }
    vkn_pass_watch(d, on_watch);
    return 1;
}
// A depth target the chain has not judged yet (draws with colour targets, or into a refused image, never come here twice in a row).
static int adopt(const vkn_fx_ctx *ctx, vkn_tex *d) {
    for (uint32_t i = 0; i < VKN_PASS_COLORS; i++) if (ctx->color[i]) { g.rej = d; return 0; }
    const int rf = shape_why(d, 0, 0);
    if (rf != NRSHC_RF_OK || d->dead) { g.s.rf[rf]++; g.rej = d; return 0; }
    // one atlas at a time: another depth-only image takes over when it is bigger (the sun atlas is the biggest) or the current one went
    // unused for 60 frames (no thrashing of layers)
    if (g.atlas && !g.atlas->dead && ctx->frame - g.atlas_last < 60 && (uint64_t)d->w * d->h <= (uint64_t)g.atlas->w * g.atlas->h) {
        g.s.rf[NRSHC_RF_OTHER]++; g.rej = d; return 0;
    }
    if (!atlas_take(d, ctx->frame)) { g.rej = d; return 0; }
    return 1;
}
static void new_frame(uint64_t f) {
    g.frame = f; g.frame_seen = 1; g.arms = 0; g.used = 0; g.ntiles = 0; g.dirty = 0; g.ftiles.n = 0; g.s.frames++;
}
// The atlas was cleared / read / copied outside its pass (vkn_pass_watch): the cascade drawn now ends.
static void on_watch(vkn_tex *t, int ev, float depth) {
    if (g.self || t != g.atlas) return;
    if (ev == VKN_PASS_EV_CLEAR) {
        if (g.live) end_range(END_CLEAR);
        pend_lost(END_CLEAR); pre_lost(END_CLEAR);
        if (g.f_pend) { g.f_pend = 0; g.s.rate_lost++; }   // [gpu2 P8] the far tile was not saved before the next clear
        memcpy(&g.clear_bits, &depth, 4); g.clear_frame = vkn_frame(); g.clear_seen = 1;
        if (g.clear_frame != g.frame || !g.frame_seen) new_frame(g.clear_frame);
        g.ntiles = 0; g.dirty = 0;   // cleared whole: no tile of this frame is drawn yet
        return;
    }
    const int why = ev == VKN_PASS_EV_SAMPLE ? END_READ : END_USE;
    if (g.live) end_range(why);
    pend_lost(why); pre_lost(why);
    if (ev == VKN_PASS_EV_USE) { g.dirty = 1; if (g.f_pend) { g.f_pend = 0; g.s.rate_lost++; } }   // (a copy may write it; [gpu2 P8] not saved)
}
static void on_close(void) {
    if (g.live) end_range(END_CLOSE);
    pend_lost(END_CLOSE); pre_lost(END_CLOSE);
    scroll_jobs_lost();   // [VK-SHADOWS-MOVING] strips that were never drawn: those layers are not whole
}

// ---- the chain function ----
static void shc_fx_draw(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user);
static void shc_fx(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {   // [fixpp] g_chain_out around the body
    g_chain_out = out; gs.cur_in = in; shc_fx_draw(in, ctx, out, user); g_chain_out = NULL; gs.cur_in = NULL;
}
static void shc_fx_draw(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)user;
    vkn_tex *d = ctx->depth;
    if (d != g.atlas) {                       // every draw that is not into the atlas
        if (g.live | g.pend | (int)g.pre | g.f_pend | gs.njob) {  // the cascade drawn before ends here: the safe point for its work
            if (g.live) end_range(END_NEXT);
            if (g.pend) run_pending();
            if (g.pre) pre_undo_all();
            if (g.f_pend) far_save();   // [gpu2 P8] (after a RECOVER's replay: the repaired tile)
            if (gs.njob) scroll_jobs_run();   // [VK-SHADOWS-MOVING] the strips of the scrolled layers: one pass of the layer image a frame
        }
        if (d == g.rej || !d) return;
        if (!adopt(ctx, d)) return;
    } else if (!d) return;
    vkn_tex *a = g.atlas;
    if (a->dead) { atlas_drop(); return; }    // the engine destroyed it (our reference kept the object)
    if (ctx->frame != g.frame || !g.frame_seen) new_frame(ctx->frame);
    g.atlas_last = ctx->frame;
    for (uint32_t i = 0; i < VKN_PASS_COLORS; i++)
        if (ctx->color[i]) {                  // the atlas with a colour target: no shadow cascade; the atlas is written outside the tiles
            if (g.live) end_range(END_NEXT);
            if (g.pend) run_pending();
            g.dirty = 1; g.s.colour++;
            return;
        }
    const int32_t rect[4] = {(int32_t)in->viewport[0], (int32_t)in->viewport[1], (int32_t)in->viewport[2], (int32_t)in->viewport[3]};
    const int exact = (float)rect[0] == in->viewport[0] && (float)rect[1] == in->viewport[1] && (float)rect[2] == in->viewport[2] && (float)rect[3] == in->viewport[3];
    if (g.live && memcmp(rect, g.R.rect, sizeof rect)) end_range(END_NEXT);
    if (g.pend) run_pending();
    if (g.f_pend) far_save();   // [gpu2 P8]
    g.cur_far = 0;
    if (g.far || g.rate >= 2) {   // the far cascade (nrh_far: the frame's tiles in order of their first draw, the last one of 3 / of 2 for a 2048 atlas)
        // [stage 5.4 G2] only the sun atlas, as xbg_nr_hacks.inc far_draw (square, >= 1024): the cache adopts any depth-only image >= 256
        if (a->w != a->h || a->w < 1024) g.s.far_not_sun++;
        else if (nrh_far(&g.ftiles, rect, a->w) == 1) {
            g.cur_far = 1;
            if (g.far) g.s.far_draws++;
            if (g.far == 1) { out->skip = 1; g.s.far_skipped++; return; }
        }
    }
    // [VK-SHADOWS-MOVING] the bounds of the caster on the CPU: one that cannot touch viewport and scissor rasterises nothing (culled
    // before it is hashed or recorded); the cache uses the bounds for its scrolled layer
    vkn_shs_b bb; const vkn_shs_b *bp = NULL;
    if ((gs.cull || (gs.scroll && g.mode)) && exact && vkn_shs_bounds(in, ctx, &bb) == VKN_SHS_OK) bp = &bb;
    if (gs.cull && bp) {
        const int32_t m = gs.cull == 3 ? 64 : 0;   // (poison: the tile shrunk)
        nr_ss_rect vis = {rect[0] + m, rect[1] + m, rect[0] + rect[2] - m, rect[1] + rect[3] - m};
        if (vis.x0 < in->scissor[0]) vis.x0 = in->scissor[0];
        if (vis.y0 < in->scissor[1]) vis.y0 = in->scissor[1];
        if (vis.x1 > in->scissor[0] + in->scissor[2]) vis.x1 = in->scissor[0] + in->scissor[2];
        if (vis.y1 > in->scissor[1] + in->scissor[3]) vis.y1 = in->scissor[1] + in->scissor[3];
        gs.c.cull_seen++;
        if (!nr_ss_isect(&bp->px, &vis)) {
            if (gs.cull == 2) gs.c.cull_would++;
            else { gs.c.culled++; out->skip = 1; return; }
        }
    }
    if (!g.mode) return;
    if (!g.live) start_range(rect, exact);
    if (g.R.refused || g.rg->blind) return;
    draw_one(in, ctx, out, bp);
}

// ---- install, census ----
static uint32_t num(const char *n, uint32_t def, uint32_t lo, uint32_t hi) {
    const char *v = getenv(n); if (!v || !*v) return def;
    const long x = atol(v); return x < (long)lo ? lo : x > (long)hi ? hi : (uint32_t)x;
}
static int g_far_read;   // [stage 5.4 G2] vkn_shc_install read DM_VK_NATIVE_SHFAR
void vkn_shc_install(void) {
    static int done; if (done) return; done = 1;
    const char *e = getenv("DM_VK_NATIVE_SHCACHE");
    int m = e && *e ? atoi(e) : NRSHC_ON;
    if (m < 0 || m >= NRSHC_MODES) { vkn_logf(RED "shcache: DM_VK_NATIVE_SHCACHE=%d is not 0-3: off", m); m = 0; }
    g.par = nrshc_par_default();
    g.par.stable = num("DM_VK_NATIVE_SHCACHE_STABLE", 3, 1, 60); g.par.min_static = num("DM_VK_NATIVE_SHCACHE_MIN", 64, 1, NRSHC_DRAWS);
    g.par.max_age = num("DM_VK_NATIVE_SHCACHE_AGE", 120, 1, 100000); g.par.probe = num("DM_VK_NATIVE_SHCACHE_PROBE", 8, 0, 1000);
    g.verify_every = num("DM_VK_NATIVE_SHCACHE_VERIFY", 30, 0, 100000);
    g.par.diag = num("DM_VK_NATIVE_SHC_DIAG", 0, 0, 1);   // [rt2 F1] the diagnostics-only tables (nr_shc.h nrshc_par.diag): off by default
    char from = 0; const char *f = getenv("DM_VK_NATIVE_SHFAR");
    int fr = f && *f ? atoi(f) : nrh_value(NRH_SHFAR, &from);
    if (f && *f) from = 'v';
    if (fr < 0 || fr > 2) fr = 0;
    {   // [VK-SHADOWS-MOVING] DM_VK_NATIVE_SHCULL / DM_VK_NATIVE_SHSCROLL (vkn_shs.h)
        vkn_shs_install();
        gs.cull = vkn_shs_cull(); gs.scroll = vkn_shs_scroll(); gs.eps = vkn_shs_eps(); gs.cam = vkn_shs_cam(&gs.cam_slot, &gs.vp_off, &gs.view_off); gs.zok = vkn_shs_scroll_z();
        if (gs.scroll && m != NRSHC_ON && m != NRSHC_CHECK) { vkn_logf("shcache: DM_VK_NATIVE_SHSCROLL=%d needs DM_VK_NATIVE_SHCACHE=1 (it is %d): 0", gs.scroll, m); gs.scroll = 0; }
        if (gs.scroll && !gs.cam) {   // [22.11] default 1: red only when the switch was set explicitly (the default quietly falls back)
            const char *se = getenv("DM_VK_NATIVE_SHSCROLL");
            if (se && *se) vkn_logf(RED "shcache: DM_VK_NATIVE_SHSCROLL=%d without the per-view block (vkn_shs_cam_block): 0", gs.scroll);
            else vkn_logf("shcache: DM_VK_NATIVE_SHSCROLL default 1 without the per-view block (vkn_shs_cam_block): 0");
            gs.scroll = 0; }
        if (gs.scroll == 2 && m == NRSHC_ON) { m = NRSHC_CHECK; vkn_logf("shcache: DM_VK_NATIVE_SHSCROLL=2 (verify): DM_VK_NATIVE_SHCACHE=2 for this run"); }
    }
    g.mode = m; g.far = fr; g.inst = m || fr || gs.cull; g_far_read = 1;
    {   // [gpu2 P8] DM_VK_NATIVE_SHRATE (vkn_shc.h): 1 every frame (default), 2 the far cascade every second frame, 3 verify (drawn, compared)
        const char *q = getenv("DM_VK_NATIVE_SHRATE"); int rt = q && *q ? atoi(q) : 1;
        if (rt < 1 || rt > 3) { vkn_logf(RED "shcache: DM_VK_NATIVE_SHRATE=%s is not 1 / 2 / 3: 1", q); rt = 1; }
        if (rt >= 2 && m != NRSHC_ON && m != NRSHC_CHECK) { vkn_logf("shcache: DM_VK_NATIVE_SHRATE=%d needs DM_VK_NATIVE_SHCACHE=1 (it is %d): 1", rt, m); rt = 1; }
        if (rt == 2 && m == NRSHC_CHECK) rt = 3;   // (the cache's verify draws everything: the rate's verify)
        g.rate = rt;
        if (rt >= 2) vkn_logf("shcache: DM_VK_NATIVE_SHRATE=%d (gpu2 P8): %s", rt, rt == 2 ? "the far cascade every second frame: a frame whose far static casters hit the cache and "
                              "whose frame before drew the far tile takes that tile and skips every far draw (moving casters' far shadows one frame late)"
                              : "verify: drawn every frame, the frame before's far tile compared with this frame's every second frame (\"rate cmp\" in the census)");
    }
    { const char *k = getenv("DM_VK_NATIVE_SHC_MASK"); vkn_shc_mask_set(k && *k ? atoi(k) : 0);   // [rt P3-GPU] (default 0 until the phone's verify run)
      vkn_logf("shcache: DM_VK_NATIVE_SHC_MASK=%d (%s)", g_shm, g_shm == 1 ? "constant blocks hashed only over the rows the shader reads" : g_shm == 2 ? "whole blocks hashed, the masked hash counted" : "whole blocks"); }
    vkn_logf("DM_VK_NATIVE_SHCACHE=%d (stage 5 A3, the static shadow cache: %s) stable=%u min=%u age=%u probe=%u verify=%u diag=%u (DM_VK_NATIVE_SHC_DIAG); DM_VK_NATIVE_SHFAR=%d (%s, %s)", m,
             m == 0 ? "off" : m == 1 ? "on" : m == 2 ? "verify - decided and counted, drawn as the engine asks, would-be hits compared" : "poison - a hit keeps the cleared tile",
             g.par.stable, g.par.min_static, g.par.max_age, g.par.probe, g.verify_every, g.par.diag, fr, fr == 1 ? "the far cascade is not drawn" : fr == 2 ? "far cascade counted" : "off",
             from == 'v' ? "env" : from == 'e' ? "DM_HACK_SHFAR" : from == 't' ? "DM_HACK_TIER" : "default");
    if (!g.inst) return;
    if (vkn_fx_register("shcache", shc_fx, NULL, 100)) { vkn_logf(RED "shcache: the draw-effect chain refused the function: off"); g.inst = 0; return; }
    vkn_rec_on_close(on_close);
}
int vkn_shc_far(void) { return g_far_read ? g.far : -1; }
int vkn_shc_active(void) { return g.inst; }
void vkn_shc_stats_get(vkn_shc_stats *s) { if (s) { *s = g.s; s->img_bytes = img_bytes(); } }

typedef struct txt { char b[1400]; size_t k; } txt;
static void put(txt *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(txt *t, const char *fmt, ...) {
    if (t->k >= sizeof t->b - 1) return;
    va_list a; va_start(a, fmt); const int w = vsnprintf(t->b + t->k, sizeof t->b - t->k, fmt, a); va_end(a);
    if (w > 0) t->k = t->k + (size_t)w < sizeof t->b - 1 ? t->k + (size_t)w : sizeof t->b - 1;
}
void vkn_shc_census(void) {
    if (!g.inst) return;
    vkn_shc_stats *s = &g.s; const double fr = s->frames ? (double)s->frames : 1.0;
    s->img_bytes = img_bytes();
    typedef unsigned long long ull;
    txt a; a.k = 0; a.b[0] = 0;
    put(&a, "shcache (DM_VK_NATIVE_SHCACHE=%d, last 10 s, %llu frames with the atlas%s): cascades %llu armed %llu (not hashed: quiet %llu) refused %llu [", g.mode, (ull)s->frames,
        g.atlas ? "" : ", no atlas", (ull)s->ranges, (ull)s->armed, (ull)s->blind, (ull)s->refused);
    for (int i = 1, n = 0; i < NRSHC_RF_N; i++) if (s->rf[i]) put(&a, "%s%s %llu", n++ ? ", " : "", nrshc_rf_name(i), (ull)s->rf[i]);
    put(&a, "]; %s %llu (ok %llu, recover %llu: repaired %llu, cleared %llu, LOST %llu, with drops %llu), build %llu (layer %llu, failed %llu, lost %llu, evicted %llu), rest %llu, normal %llu [",
        g.mode == NRSHC_CHECK ? "would hit" : "hit", (ull)s->act[NRSHC_A_HIT], (ull)s->hit_ok, (ull)s->recover, (ull)s->recover_now, (ull)s->recover_cleared, (ull)s->recover_lost,
        (ull)s->recover_bad, (ull)s->build, (ull)s->build_ok, (ull)s->build_fail, (ull)s->build_lost, (ull)s->evicted, (ull)s->rest, (ull)s->act[NRSHC_A_NORMAL]);
    for (int i = 1, n = 0; i < NRSHC_W_N; i++) if (s->why[i]) put(&a, "%s%s %llu", n++ ? ", " : "", nrshc_why_name(i), (ull)s->why[i]);
    put(&a, "]; draws %.0f a frame, %s %.0f a frame (%.1f %%;", s->draws / fr, g.mode == NRSHC_CHECK ? "would skip" : "skipped", s->skipped / fr, s->draws ? 100.0 * (double)s->skipped / (double)s->draws : 0.0);
    for (int i = 0; i < NRSHC_CAS; i++) if (s->cdraws[i]) put(&a, " c%d %.0f/%.0f", i, s->cskip[i] / fr, s->cdraws[i] / fr);
    put(&a, " skipped/drawn); kept %.0f, replayed %.0f a frame (dropped %llu, stale %llu), layer copies %.1f a frame (precopied %llu: hit %llu, put back %llu, LOST %llu), tile clears %.1f a frame; verify equal %llu differ %llu (runs %llu, skipped %llu); "
        "far cascade (DM_VK_NATIVE_SHFAR=%d) %.0f draws a frame, %s %.0f; layer memory %.1f MB, model %.1f MB; red", s->saved / fr, s->replayed / fr, (ull)s->replay_drop, (ull)s->replay_stale,
        s->copies / fr, (ull)s->precopied, (ull)s->pre_hit, (ull)s->pre_undone, (ull)s->pre_lost, s->tile_clears / fr, (ull)s->verify_eq, (ull)s->verify_ne, (ull)s->verify_runs, (ull)s->verify_skip, g.far, s->far_draws / fr, g.far == 1 ? "skipped" : "would skip",
        g.far == 1 ? s->far_skipped / fr : s->far_draws / fr, s->img_bytes / 1048576.0, s->model_bytes / 1048576.0);
    { int r = 0; for (int i = 0; i < R_N; i++) r += g.red[i]; put(&a, " %d", r); }
    if (g.rate >= 2)   // [gpu2 P8]
        put(&a, "; far rate %d: far cascades %llu, reused %llu (would %llu), recovered %llu, draws skipped %.0f a frame, tiles saved %llu (lost %llu); rate cmp %llu tiles: "
            "texels differing %.3f %% (max %u steps); drawn over a reused tile %llu", g.rate, (ull)s->rate_far, (ull)s->rate_reused, (ull)s->rate_would, (ull)s->rate_recover, s->rate_skipped / fr,
            (ull)s->rate_saved, (ull)s->rate_lost, (ull)s->rate_cmp, s->rate_cmp_all ? 100.0 * (double)s->rate_cmp_px / (double)s->rate_cmp_all : 0.0, (unsigned)s->rate_cmp_max, (ull)s->rate_drawn);
    if (g.cond_ref) put(&a, "; refused for DM_VK_NATIVE_COND=%d (GPU predicates) %llu", vkn_query_mode(), (ull)g.cond_ref);   // (in "backend" above)
    g.cond_ref = 0;
    vkn_logf("%s", a.b);
    if (g.mode && s->armed) {   // the reasons: taints by draw kind (nrs_why for order), salted draws, per cascade how stable the draws were
        txt w; w.k = 0; w.b[0] = 0;
        put(&w, "shcache why (last 10 s): tainted cascades %llu, draws %llu [", (ull)s->tranges, (ull)s->tainted);
        for (int i = 1, n = 0; i < NRSHC_T_N; i++) if (s->tw[i]) {
            put(&w, "%s%s %llu", n++ ? ", " : "", nrshc_t_name(i), (ull)s->tw[i]);
            if (i == NRSHC_T_ORDER) { put(&w, " ("); for (int j = 1, m = 0; j < NRS_W_N; j++) if (s->nrs[j]) put(&w, "%s%s %llu", m++ ? ", " : "", nrs_why_name(j), (ull)s->nrs[j]); put(&w, ")"); }
        }
        put(&w, "]; not replayable draws %llu; draws without a content version (never members) %llu; constant blocks hashed %.1f KB a frame (memo %llu), unrestored %llu; "
            "engine-thread hashes (DM_VK_NATIVE_SHC_EHASH=%d) used %llu, verified %llu, differ %llu, not given %llu; read masks (DM_VK_NATIVE_SHC_MASK=%d): blocks %llu, "
            "narrowed %llu, %.1f of %.1f KB a frame; content versions from the record (rt2 F1) %llu, checked %llu, the table had moved on %llu; per armed cascade known / new by constants / new other / new by content / not compared%s:",
            (ull)s->unrep, (ull)s->salted, s->ubo_bytes / fr / 1024.0, (ull)s->ubo_memo, (ull)s->ubo_unrestored, vkn_rt_ehash(), (ull)g_eh.used, (ull)g_eh.verified, (ull)g_eh.differ, (ull)g_eh.na,
            g_shm, (ull)(g_shms.blocks - g_shmo.blocks), (ull)(g_shms.narrowed - g_shmo.narrowed), (g_shms.bytes_masked - g_shmo.bytes_masked) / fr / 1024.0,
            (g_shms.bytes_whole - g_shmo.bytes_whole) / fr / 1024.0, (ull)g_vs.used, (ull)g_vs.checked, (ull)g_vs.moved, g.par.diag ? "" : " (DM_VK_NATIVE_SHC_DIAG=0: every new draw counted as new other)");
        g_shmo = g_shms;
        memset(&g_eh, 0, sizeof g_eh); memset(&g_vs, 0, sizeof g_vs);
        for (int i = 0; i < NRSHC_CAS; i++) if (s->cranges[i]) {
            const double rr = (double)s->cranges[i];
            put(&w, " c%d (%llu) %.0f/%.0f/%.0f/%.0f/%.0f", i, (ull)s->cranges[i], s->known[i] / rr, s->newu[i] / rr, s->newo[i] / rr, s->ncont[i] / rr, s->unc[i] / rr);
        }
        vkn_logf("%s", w.b);
    }
    if (gs.scroll || gs.cull) {   // [VK-SHADOWS-MOVING]
        const vkn_shc_scroll_stats *q = &gs.c; vkn_shs_stats b; vkn_shs_stats_get(&b);
        txt m; m.k = 0; m.b[0] = 0;
        put(&m, "shcache moving (DM_VK_NATIVE_SHSCROLL=%d, DM_VK_NATIVE_SHCULL=%d, last 10 s): hits as before %llu, scrolled %llu (shifted %llu, z moved %llu; largest shift %u px, worst distance from whole texels %.4f px); "
            "dropped [reference %llu, projection turned %llu, z moved %llu, not whole texels %llu, shift too big %llu, members without an instance %llu, format / clear %llu, strips late %llu]; builds held (z moving) %llu; "
            "a scrolled hit: members %s %.0f, drawn into strips %.0f; members gone with the scroll %llu, missing inside the tile %llu, clip-range guard %llu, member without an instance met %llu; "
            "layers built %llu (members with an instance %.0f, without %.0f), refreshed %llu; strips: done %llu, pieces %llu, draws %llu, failed %llu, lost %llu, store full %llu, z patch refused %llu; layer drawn into tiles %llu (failed %llu)",
            gs.scroll, gs.cull, (ull)q->plain_ranges, (ull)q->scroll_ranges, (ull)q->moved_ranges, (ull)q->dz_ranges, (unsigned)q->n_max, q->err_max,
            (ull)q->drop_ref, (ull)q->drop_lin, (ull)q->drop_z, (ull)q->drop_shift, (ull)q->drop_big, (ull)q->drop_fmem, (ull)q->drop_fmt, (ull)q->job_late, (ull)q->build_held,
            g.mode == NRSHC_CHECK ? "would skip" : "skipped", q->scroll_ranges ? (double)q->skipped / (double)q->scroll_ranges : 0.0, q->scroll_ranges ? (double)q->touched / (double)q->scroll_ranges : 0.0,
            (ull)q->absent_out, (ull)q->absent_in, (ull)q->zguard, (ull)q->f_member,
            (ull)q->builds, q->builds ? (double)q->build_t / (double)q->builds : 0.0, q->builds ? (double)q->build_f / (double)q->builds : 0.0, (ull)q->refresh,
            (ull)q->jobs, (ull)q->job_pieces, (ull)q->job_draws, (ull)q->job_fail, (ull)q->job_lost, (ull)q->job_full, (ull)q->patch_bad, (ull)q->quad, (ull)q->quad_fail);
        vkn_logf("%s", m.b);
        m.k = 0; m.b[0] = 0;
        put(&m, "shcache moving why: casters with an instance %.0f a frame (fragment stage may matter %.0f, registry full %.0f, changed while the cascade stood %llu, own constants changed %llu); "
            "the cascade's z moved in %llu of %llu ranges; scroll verify: tiles %llu (equal %llu), texels differing %.4f %% (max %u steps)",
            q->t_draws / fr, q->t_fs / fr, q->t_full / fr, (ull)q->epoch, (ull)q->epoch_obj, (ull)q->zstep, (ull)q->zcmp, (ull)q->sv_cmp, (ull)q->sv_eq,
            q->sv_all ? 100.0 * (double)q->sv_px / (double)q->sv_all : 0.0, (unsigned)q->sv_max);
        put(&m, "; culling: casters with bounds %.0f a frame, %s %.0f; bounds asked %.0f a frame:", q->cull_seen / fr, gs.cull == 2 ? "would cull" : "culled", (gs.cull == 2 ? q->cull_would : q->culled) / fr, b.asked / fr);
        for (int i = 0; i < VKN_SHS_N; i++) if (b.why[i]) put(&m, " %s %.0f", vkn_shs_why_name(i), b.why[i] / fr);
        put(&m, "; boxes from the table %.0f, scanned %llu (%llu indices); memory: scroll state %.2f MB, box table %.2f MB", b.box_hit / fr, (ull)b.box_scan, (ull)b.box_idx, scroll_bytes() / 1048576.0, b.box_bytes / 1048576.0);
        vkn_logf("%s", m.b);
        if (gs.scroll == 2 || q->svc_cmp[0] || q->svc_cmp[1]) {   // [fix 22.11] the verify by case and kind (a line of its own: vkn_logf keeps 1024 bytes)
            m.k = 0; m.b[0] = 0;
            put(&m, "shcache moving verify (last 10 s; DM_VK_NATIVE_SHSCROLL_Z=%d):", gs.zok);
            for (int c = 0; c < 2; c++) {
                const double a = q->svc_all[c] ? 100.0 / (double)q->svc_all[c] : 0.0;
                put(&m, "%s %s: tiles %llu (equal %llu), texels differing - kept part 1 step %.4f %%, kept part more %.4f %%, exposed strips %.4f %% (max %u steps)", c ? ";" : "",
                    c ? "layer moved in z" : "layer moved in x / y only", (ull)q->svc_cmp[c], (ull)q->svc_eq[c], (double)q->svc_k1[c] * a, (double)q->svc_kn[c] * a, (double)q->svc_st[c] * a, (unsigned)q->svc_max[c]);
            }
            vkn_logf("%s", m.b);
        }
        memset(&gs.c, 0, sizeof gs.c); vkn_shs_stats_reset();
    }
    const uint64_t mb = s->model_bytes;
    memset(s, 0, sizeof *s); s->model_bytes = mb;
}
#endif
