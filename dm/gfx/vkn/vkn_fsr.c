// [DOS2] VK-DRS-FSR: anti-aliasing at the render resolution before FSR 1 EASU - the contract and the facts are in vkn_fsr.h.
// One draw-effect function ("fsr-aa", order 450: after the post fusion, which never touches the AA step at a stretch < 1: "a part of
// the target is drawn"). Recording thread for everything but the install and the stats.
#ifdef DM_GAME_DOS2
#include "vkn_fsr.h"
#include "vkn.h"
#include "vkn_buf.h"
#include "vkn_drv.h"
#include "vkn_fx.h"
#include "vkn_pass.h"
#include "vkn_shader.h"
#include "vkn_shlib_named.h"
#include "vkn_tex.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
static const uint32_t k_pre_spv[] = {
#include "../../android/shaders/ppfxaa_fsr_pre.frag.spv.inc"
};
static const char k_key[] = "691768aa08545d95";   // PPFXAA (the fragment key the FSR module replaces, vkn_shlib_named.cpp)

// The library's hack state (weak: host tests link without the C++ library, there the module counts as swapped)
void vkn_shlib_named_hacks_get(vkn_shlib_named_hacks *h) __attribute__((weak));

enum { CACHE = 4 };
typedef struct fsr_sh {           // per engine shader object of the PPFXAA key
    const vkn_shader *eng; uint32_t id; uint64_t handle;
    int state;                    // 0 not built, 1 ready, -1 refused (counted at each draw)
    int why;                      // the refusal reason
    const vkn_shader *pre, *twin; // the pre pass's object (our fragment module) / the engine shader reading the effect texture
    vkn_fx_ubo c1[2];             // C1 of the fragment / vertex stage as effect blocks 0 / 1
    uint8_t tex_slot, c3_slot, has_c3;
} fsr_sh;
static struct {
    int mode, reg;                // DM_VK_NATIVE_FSR_AA; registered
    fsr_sh sh[CACHE]; uint32_t nsh;
    VkShaderModule pre_fs; int pre_fs_tried;
    vkn_tex *scratch;             // the copy the EASU draw reads (source size / format)
    vkn_fsr_stats st;
    uint32_t told;                // bit per first-time line
    pthread_mutex_t mu;
} g = {.mode = -1, .mu = PTHREAD_MUTEX_INITIALIZER};

static const char *const k_why[VKN_FSR_R_N] = {
    "another piece already swapped the draw's shader", "the engine shader's binding table is not the PPFXAA shape (one fragment texture, uniforms, support, C1)",
    "no vertex constants c3 announced (the stretch is unknown)", "the stretch c3[0].xy is not in (0, 1]", "no source texture, or its size differs from the target",
    "the viewport does not cover the target", "the copy (a private colour target) could not be made", "the pre pass / twin shader objects could not be made",
    "the draw path dropped the pre pass"};
const char *vkn_fsr_reason_name(int r) { return r >= 0 && r < VKN_FSR_R_N ? k_why[r] : "?"; }
const uint32_t *vkn_fsr_pre_words(uint32_t *n) { if (n) *n = (uint32_t)(sizeof k_pre_spv / 4); return k_pre_spv; }

static void refuse(int r, const char *detail) {
    g.st.refused[r]++;
    if (!(g.told >> r & 1)) { g.told |= 1u << r; vkn_logf(RED "fsr-aa: %s%s%s: this draw is EASU of the raw image (today's), counted from now on", k_why[r], detail ? ": " : "", detail ? detail : ""); }
}

// ---------------------------------------------------------------- shader objects
static VkShaderModule pre_module(void) {
    if (g.pre_fs_tried) return g.pre_fs;
    g.pre_fs_tried = 1;
    const vkn_dev *d = vkn_device(); if (!d) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; ci.codeSize = sizeof k_pre_spv; ci.pCode = k_pre_spv;
    const int lk = vkn_drv_lock(); const VkResult r = vkCreateShaderModule(d->dev, &ci, NULL, &g.pre_fs); vkn_drv_unlock(lk);
    if (r != VK_SUCCESS) { g.pre_fs = VK_NULL_HANDLE; vkn_logf(RED "fsr-aa: vkCreateShaderModule %d for the pre pass", (int)r); }
    return g.pre_fs;
}
static int cmp_binding(const void *a, const void *b) { return (int)((const vkn_bind *)a)->binding - (int)((const vkn_bind *)b)->binding; }
static const vkn_shader *make(const char *name, const vkn_shader *s, VkShaderModule fs, vkn_bind *tab, uint32_t n) {
    qsort(tab, n, sizeof *tab, cmp_binding);
    vkn_fx_shader_src src; memset(&src, 0, sizeof src);
    src.name = name; src.vs = vkn_shader_module(s, VKN_STAGE_VS, 0, 0.0f, 0); src.fs = fs; src.bind = tab; src.nbind = n;
    src.ninputs = s->ninputs; memcpy(src.inputs, s->inputs, sizeof src.inputs); memcpy(src.input_classes, s->input_classes, sizeof src.input_classes);
    return src.vs && src.fs ? vkn_fx_shader_create(&src) : NULL;
}
// The two objects of one engine shader: both = its table with C1 as effect blocks; the twin's source texture = effect texture 0.
static void build(fsr_sh *e, const vkn_shader *s) {
    e->state = -1; e->why = VKN_FSR_R_TABLE;
    if (s->nbind > 48) return;
    vkn_bind tp[48], tt[48]; int ntex = 0;
    for (uint32_t i = 0; i < s->nbind; i++) {
        vkn_bind b = s->bind[i];
        if (b.count != 1) return;
        if (b.kind == VKN_BK_C1) {
            const int k = b.bundle_stage == 4 ? 0 : b.bundle_stage == 0 ? 1 : -1, src = k == 0 ? VKN_SRC_FS : VKN_SRC_VS;
            if (k < 0 || !s->c1[src].size) return;
            e->c1[k].buf = s->c1[src].buf; e->c1[k].off = s->c1[src].off; e->c1[k].size = (uint32_t)s->c1[src].size;
            b.kind = VKN_BK_FX_UBO; b.slot = (uint8_t)k;
        } else if (b.kind == VKN_BK_TEXTURE) {
            if (b.stage != VKN_STAGE_FS || ntex++ || b.slot >= VKN_TEX_SLOTS) return;
            e->tex_slot = b.slot;
        } else if (b.kind == VKN_BK_UNIFORM) {
            if (b.stage == VKN_STAGE_VS && b.slot == 0) { e->has_c3 = 1; e->c3_slot = 0; }   // vp_c3 (engine VS block 0): TexCoord0 = uv x c3[0].xy + c3[0].zw
        } else if (b.kind != VKN_BK_SUPPORT) return;
        tp[i] = b; tt[i] = b;
        if (b.kind == VKN_BK_TEXTURE) { tt[i].kind = VKN_BK_FX_TEX; tt[i].slot = 0; tt[i].flags &= ~VKN_BF_CMP; }
    }
    if (ntex != 1) return;
    e->why = VKN_FSR_R_SHADER;
    const VkShaderModule fs = pre_module(), efs = vkn_shader_module(s, VKN_STAGE_FS, 0, 0.0f, 0);
    if (!fs || !efs) return;
    if (!(e->pre = make("fsr-aa-pre", s, fs, tp, s->nbind)) || !(e->twin = make("fsr-aa-easu", s, efs, tt, s->nbind))) return;
    e->state = 1;
    vkn_logf("fsr-aa: PPFXAA shader %#llx (%s / %s): the pre pass (FXAA 1:1 at the render resolution, ppfxaa_fsr_pre) and the EASU twin made "
             "(%u bindings, source = fragment texture slot %u, stretch from vertex block %u)", (unsigned long long)s->handle, s->key[VKN_SRC_VS],
             s->key[VKN_SRC_FS], s->nbind, e->tex_slot, e->c3_slot);
}
static fsr_sh *entry_of(const vkn_shader *s) {
    for (uint32_t i = 0; i < g.nsh && i < CACHE; i++) if (g.sh[i].eng == s && g.sh[i].id == s->id && g.sh[i].handle == s->handle) return &g.sh[i];
    fsr_sh *e = &g.sh[g.nsh++ % CACHE]; memset(e, 0, sizeof *e);   // (a full cache forgets the oldest: rebuilt if it comes back)
    e->eng = s; e->id = s->id; e->handle = s->handle;
    build(e, s);
    return e;
}
// The library swapped the FSR module into PPFXAA (state 1); without the library (host tests) it counts as swapped.
static int module_swapped(void) {
    if (!vkn_shlib_named_hacks_get) return 1;
    vkn_shlib_named_hacks h; memset(&h, 0, sizeof h); vkn_shlib_named_hacks_get(&h);
    return (h.fsr == 1 || h.fsr == 3) && h.state[0] == 1;
}

// ---------------------------------------------------------------- the copy
static vkn_tex *scratch_for(const vkn_tex *src) {
    vkn_tex *c = g.scratch;
    if (c && c->w == src->w && c->h == src->h && c->format == src->format) return c;
    if (c) { vkn_tex_private_free(c); g.scratch = NULL; }
    c = g.scratch = vkn_tex_private_color(src->format, src->w, src->h, "fsr-aa copy");
    if (c) {
        g.st.scratch_made++;
        vkn_logf("fsr-aa: the copy for EASU made: %ux%u, format %d (the source's), %.1f MB", src->w, src->h, (int)src->format, (double)c->bytes / 1048576.0);
    }
    return c;
}

// ---------------------------------------------------------------- the function
static uint8_t g_cls[4096];   // per engine shader id: 0 unknown, 1 PPFXAA, 2 other
static uint64_t g_cls_h[4096];
static void fsr_fx(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)user;
    const vkn_shader *s = ctx->shader;
    if (!s) return;
    const uint32_t id = s->id;
    if (id < 4096 && g_cls_h[id] == s->handle && g_cls[id]) { if (g_cls[id] != 1) return; }
    else {
        const int is = !strcmp(s->key[VKN_SRC_FS], k_key);
        if (id < 4096) { g_cls[id] = is ? 1 : 2; g_cls_h[id] = s->handle; }
        if (!is) return;
    }
    if (!module_swapped()) return;   // the translated PPFXAA (FSR refused): the console's own stretch, nothing to do
    g.st.draws++;
    if (out->skip || out->shader) { refuse(VKN_FSR_R_SWAPPED, NULL); return; }
    fsr_sh *e = entry_of(s);
    if (e->state < 0) { refuse(e->why, s->key[VKN_SRC_VS]); return; }
    // the stretch: vertex constants c3[0].xy of this draw
    uint32_t off = 0, size = 0; const uint8_t *ring = (const uint8_t *)vkn_ring_cpu(); float c3[4];
    if (!e->has_c3 || !ring || vkn_draw_ubo_get(0, e->c3_slot, &off, &size) || size < 16) { refuse(VKN_FSR_R_C3, NULL); return; }
    memcpy(c3, ring + off, sizeof c3);
    if (!(c3[0] > 0.0f && c3[0] <= 1.0f && c3[1] > 0.0f && c3[1] <= 1.0f)) {
        char d[64]; snprintf(d, sizeof d, "%g x %g", (double)c3[0], (double)c3[1]); refuse(VKN_FSR_R_SCALE, d); return; }
    if (fabsf(c3[0] - 1.0f) < 1.0f / 1024.0f) { g.st.one_to_one++; return; }   // the module's own rule (|ratio - 1| < 1/1024 -> FXAA)
    // the source (the one fragment texture) and the target: the same size, the viewport covers it (gl_FragCoord = texel position)
    vkn_tex *src = NULL; VkImageView sv = VK_NULL_HANDLE;
    if (in->tex[1] && in->tex[1][e->tex_slot] >= 0) sv = vkn_tex_view_by_id((uint32_t)in->tex[1][e->tex_slot], &src);
    vkn_tex *dst = ctx->color[0];
    if (!sv || !src || !dst || src->w != dst->w || src->h != dst->h || src->layers != 1) { refuse(VKN_FSR_R_SRC, NULL); return; }
    const float W = (float)dst->w, H = (float)dst->h, *vp = in->viewport;
    if (vp[0] != 0.0f || vp[2] != W || !((vp[1] == 0.0f && vp[3] == H) || (vp[1] == H && vp[3] == -H))) {
        char d[96]; snprintf(d, sizeof d, "viewport %g %g %g %g, target %ux%u", (double)vp[0], (double)vp[1], (double)vp[2], (double)vp[3], dst->w, dst->h);
        refuse(VKN_FSR_R_VIEWPORT, d); return;
    }
    // the pre pass's part: what EASU reads (its corners clamp to floor(s x size + 0.5)) + 1 texel
    uint32_t sw = (uint32_t)ceilf(c3[0] * W) + 1u, sh = (uint32_t)ceilf(c3[1] * H) + 1u;
    if (sw > dst->w) sw = dst->w;
    if (sh > dst->h) sh = dst->h;
    g.st.last_w = sw; g.st.last_h = sh; g.st.last_s[0] = c3[0]; g.st.last_s[1] = c3[1];
    if (g.mode == VKN_FSR_AA_COUNT) { g.st.would++; return; }
    vkn_tex *cp = scratch_for(src);
    if (!cp) { refuse(VKN_FSR_R_SCRATCH, NULL); return; }
    // the pre pass: the engine draw (its vertex shader, constants, textures) into the copy, clipped to the part
    uint32_t n0 = 0, lay[VKN_PASS_COLORS], mip[VKN_PASS_COLORS]; vkn_tex *col[VKN_PASS_COLORS], *dep = NULL;
    vkn_pass_cur_get(&n0, col, lay, mip, &dep);
    vkn_tex *held[VKN_PASS_COLORS + 1];
    for (uint32_t i = 0; i <= VKN_PASS_COLORS; i++) { vkn_tex *x = i < VKN_PASS_COLORS ? (i < n0 ? col[i] : NULL) : dep; held[i] = x && vkn_tex_ref(x) ? x : NULL; }
    vkn_tex *c2[VKN_PASS_COLORS] = {cp}; uint32_t l2[VKN_PASS_COLORS] = {0}, m2[VKN_PASS_COLORS] = {0};
    vkn_pass_targets(1, c2, l2, m2, NULL);
    vkn_draw_in pin = *in; pin.scissor[0] = 0; pin.scissor[1] = 0; pin.scissor[2] = (int32_t)sw; pin.scissor[3] = (int32_t)sh;
    vkn_fx_out po; memset(&po, 0, sizeof po); po.shader = e->pre; po.ubo[0] = e->c1[0]; po.ubo[1] = e->c1[1];
    vkn_fx_force(&po);
    const int rc = ctx->indexed ? vkn_draw_indexed(&pin, ctx->first, ctx->count, ctx->instances) : vkn_draw_arrays(&pin, ctx->first, ctx->count);
    vkn_fx_force(NULL);
    vkn_pass_targets(n0, col, lay, mip, dep);
    for (uint32_t i = 0; i <= VKN_PASS_COLORS; i++) if (held[i]) vkn_tex_unref(held[i]);
    if (rc) { refuse(VKN_FSR_R_DROPPED, NULL); return; }
    // the engine draw: EASU (the swapped module, unchanged) of the copy
    out->shader = e->twin;
    out->tex[0].view = cp->view; out->tex[0].sampler = VK_NULL_HANDLE; out->tex[0].tex = cp;   // (the backend's linear clamp sampler: EASU gathers)
    out->ubo[0] = e->c1[0]; out->ubo[1] = e->c1[1];
    g.st.pre++;
    if (!(g.told >> 30 & 1)) {
        g.told |= 1u << 30;
        vkn_logf("fsr-aa: first pre pass: stretch %.4f x %.4f, the part %ux%u of %ux%u FXAA'd into the copy, EASU reads it", (double)c3[0], (double)c3[1], sw, sh, dst->w, dst->h);
    }
}

// ---------------------------------------------------------------- install, stats
void vkn_fsr_install(int mode) {
    pthread_mutex_lock(&g.mu);
    if (!g.reg) {
        if (mode < 0) { const char *e = getenv("DM_VK_NATIVE_FSR_AA"); mode = e && *e ? atoi(e) : VKN_FSR_AA_ON; }
        if (mode != VKN_FSR_AA_OFF && mode != VKN_FSR_AA_COUNT) mode = VKN_FSR_AA_ON;
        g.mode = mode; g.reg = 1;
        if (mode == VKN_FSR_AA_OFF) vkn_logf("fsr-aa: DM_VK_NATIVE_FSR_AA=0: EASU of the raw image below 100 %% (no anti-aliasing at all: the stair steps stay)");
        else if (vkn_fx_register("fsr-aa", fsr_fx, NULL, 450)) { vkn_logf(RED "fsr-aa: the draw-effect chain refused the function: EASU of the raw image"); g.mode = VKN_FSR_AA_OFF; }
        else vkn_logf("fsr-aa: DM_VK_NATIVE_FSR_AA=%d: %s", mode, mode == VKN_FSR_AA_ON ? "below 100 % the console's FXAA at the render resolution first (a pre pass into a copy), then EASU of it"
                                                                                       : "count only (decisions counted as 'would', EASU of the raw image drawn)");
    }
    pthread_mutex_unlock(&g.mu);
}
int vkn_fsr_mode(void) { return g.reg ? g.mode : -1; }
void vkn_fsr_stats_get(vkn_fsr_stats *s) { if (s) *s = g.st; }
void vkn_fsr_census(void) {
    if (!g.reg || !g.st.draws) return;
    char r[256]; int o = 0;
    for (int i = 0; i < VKN_FSR_R_N && o < (int)sizeof r - 48; i++) if (g.st.refused[i]) o += snprintf(r + o, sizeof r - (size_t)o, "%s%d:%llu", o ? " " : "", i, (unsigned long long)g.st.refused[i]);
    vkn_logf("fsr-aa (10 s): DM_VK_NATIVE_FSR_AA=%d; PPFXAA draws %llu, at 1:1 %llu, pre passes %llu (would %llu), copies made %llu; last part %ux%u at %.3f x %.3f; refused %s",
             g.mode, (unsigned long long)g.st.draws, (unsigned long long)g.st.one_to_one, (unsigned long long)g.st.pre, (unsigned long long)g.st.would,
             (unsigned long long)g.st.scratch_made, g.st.last_w, g.st.last_h, (double)g.st.last_s[0], (double)g.st.last_s[1], o ? r : "0");
}
#endif
