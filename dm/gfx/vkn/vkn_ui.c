// [DOS2] VK-NATIVE stage 3: the UI (vkn_ui.h). No game addresses, no engine record offsets: dm/host/hle_dos2_vkn_ui.c decodes the
// GDraw calls and passes plain values.
// Objects of this module (all made on first use, kept for the process; pipelines and pairs are few: 3 vertex x ~75 fragment programs,
// GDraw's 6 blend x 8 depth-stencil states):
//   programs     GDraw program object -> stage, control copy, code pointer (vkn_ui_program_put; a small table under a mutex);
//   pairs        (vertex program, fragment program) -> vkn_shader created under the handle (1 << 32 | (0xff00 + pair index));
//   pipelines    key (pair, vertex format, blend, depth-stencil, channel mask, stencil masks, pass compatibility, write mask) -> VkPipeline;
//   sets         one descriptor set per draw from pools of the current recording (no vkResetDescriptorPool / vkFreeDescriptorSets in
//                the trampoline list): a new recording retires the pools of the last one (destroyed 3 frames later, vkn_retire);
//   constants and immediate vertices / indices go into the engine's constant ring (vkn_ring_alloc: the engine's rule, big enough);
//   statics      a 1x1 placeholder texture, the clamp sampler, the quad index pattern, the null vertex stream {0, 0, 0, 1}, and a
//                position-only vertex module (hand-written SPIR-V below) for masked stencil clears.
#ifdef DM_GAME_DOS2
#include "vkn_ui.h"
#include "vkn_buf.h"
#include "vkn_draw.h"
#include "vkn_frame.h"
#include "vkn_pass.h"
#include "vkn_shader.h"
#include "vkn_state.h"
#include "vkn_fx.h"            // [stage 5.2 B2] the fence before an engine draw
#include "vkn_drv.h"           // [stage 5.3 V2] DM_GFX_PIPE_SERIAL: the creation lock (PowerVR)
#include "../nr/nr_uix.h"     // [stage 5.2 B2] ordinal keys, DS decay (includes ../nb/nb_uiarea.h: rects, seed, history)
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
enum { PROGS = 256, PAIRS = 256, PIPES = 2048, POOL_SETS = 128, QUADS = 0x4000, PAIR_BASE = 0xff00, REDS = 16 };

static vkn_ui_stats g_st;
enum { RR_CB, RR_PASS, RR_SHADER, RR_BIND, RR_PIPE, RR_SET, RR_RING, RR_VB, RR_PROG, RR_STATIC, RR_N };
static uint32_t g_red[RR_N];
#define UI_RED(r, ...) do { if (g_red[r]++ < REDS) vkn_logf(RED "ui: " __VA_ARGS__); } while (0)

// ---- programs ----
typedef struct prog_e { uint64_t prog; int stage; uint8_t *ctl; const void *code; } prog_e;
static prog_e g_prog[PROGS]; static uint32_t g_nprog; static pthread_mutex_t g_prog_mu = PTHREAD_MUTEX_INITIALIZER;
int vkn_ui_program_put(uint64_t prog, int stage, const void *control, const void *code) {
    if (!prog || !control || !code || (stage != 0 && stage != 1)) return -1;
    pthread_mutex_lock(&g_prog_mu);
    prog_e *e = NULL;
    for (uint32_t i = 0; i < g_nprog; i++) if (g_prog[i].prog == prog) { e = &g_prog[i]; break; }
    if (!e && g_nprog < PROGS) { e = &g_prog[g_nprog++]; memset(e, 0, sizeof *e); }
    if (e) {
        if (!e->ctl) e->ctl = (uint8_t *)malloc(VKN_UI_CTL);
        if (e->ctl) { memcpy(e->ctl, control, VKN_UI_CTL); e->prog = prog; e->stage = stage; e->code = code; }
        else { e->prog = 0; e = NULL; }
    }
    pthread_mutex_unlock(&g_prog_mu);
    return e ? 0 : -1;
}
static int prog_find(uint64_t prog, prog_e *out) {
    int ok = 0;
    pthread_mutex_lock(&g_prog_mu);
    for (uint32_t i = 0; i < g_nprog; i++) if (g_prog[i].prog == prog) { *out = g_prog[i]; ok = 1; break; }
    pthread_mutex_unlock(&g_prog_mu);
    return ok;
}

// ---- pairs ----
typedef struct pair_e { uint64_t vs, fs, handle; const vkn_shader *s; uint8_t state; } pair_e;   // state 0 none, 1 queued, 2 ready, 3 failed
static pair_e g_pair[PAIRS]; static uint32_t g_npair;
static const vkn_shader *pair_get(uint64_t vs, uint64_t fs, uint32_t *idx) {
    pair_e *p = NULL;
    for (uint32_t i = 0; i < g_npair; i++) if (g_pair[i].vs == vs && g_pair[i].fs == fs) { p = &g_pair[i]; *idx = i; break; }
    if (!p) {
        if (g_npair >= PAIRS) { UI_RED(RR_SHADER, "more than %d program pairs: draw dropped", PAIRS); return NULL; }
        p = &g_pair[g_npair]; *idx = g_npair++; memset(p, 0, sizeof *p);
        p->vs = vs; p->fs = fs; p->handle = 1ull << 32 | (uint64_t)(PAIR_BASE + *idx);
        prog_e v, f;
        if (!prog_find(vs, &v) || !prog_find(fs, &f) || v.stage != 0 || f.stage != 1) {
            p->state = 3; g_st.pair_fail++;
            UI_RED(RR_PROG, "program pair %#llx / %#llx: unknown program or wrong stage (vertex %d, fragment %d)", (unsigned long long)vs, (unsigned long long)fs,
                   prog_find(vs, &v) ? v.stage : -1, prog_find(fs, &f) ? f.stage : -1);
            return NULL;
        }
        vkn_stage_src src[VKN_SRCS]; memset(src, 0, sizeof src);
        src[VKN_SRC_VS].control = v.ctl; src[VKN_SRC_VS].code = v.code;
        src[VKN_SRC_FS].control = f.ctl; src[VKN_SRC_FS].code = f.code;
        if (vkn_shader_create(p->handle, src)) { p->state = 3; g_st.pair_fail++; UI_RED(RR_SHADER, "vkn_shader_create of pair %u refused (no device / translator)", *idx); return NULL; }
        p->state = 1;
    }
    if (p->state == 1) {
        p->s = vkn_shader_get(p->handle);   // waits for the queued creation
        if (!p->s || p->s->missing || p->s->unusable || !p->s->module[VKN_SRC_VS] || !p->s->module[VKN_SRC_FS] || !p->s->layout) {
            p->state = 3; g_st.pair_fail++;
            UI_RED(RR_SHADER, "program pair %u (%#llx / %#llx): no usable modules (record %s, missing %#x, unusable %#x, keys %s / %s): its draws are dropped", *idx,
                   (unsigned long long)p->vs, (unsigned long long)p->fs, p->s ? "made" : "none", p->s ? p->s->missing : 0u, p->s ? p->s->unusable : 0u,
                   p->s ? p->s->key[VKN_SRC_VS] : "?", p->s ? p->s->key[VKN_SRC_FS] : "?");
        } else { p->state = 2; g_st.pairs++; }
    }
    return p->state == 2 ? p->s : NULL;
}

// ---- statics ----
static VkSampler g_smp; static vkn_tex *g_ph; static vkn_block g_quad_ib, g_null_vb; static int g_static_ok;
static VkShaderModule g_clr_vs; static VkPipelineLayout g_clr_layout;
static uint8_t g_ph_cookie;   // the placeholder's cookie: this byte's address (unique, never an engine object)
// layout(location = 0) in vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }  (SPIR-V 1.0, hand-assembled)
static const uint32_t k_clr_vs[] = {
    0x07230203, 0x00010000, 0, 23, 0,
    2u << 16 | 17, 1,                                         // OpCapability Shader
    3u << 16 | 14, 0, 1,                                      // OpMemoryModel Logical GLSL450
    7u << 16 | 15, 0, 16, 0x6e69616d, 0, 10, 8,               // OpEntryPoint Vertex %16 "main" %10 %8
    4u << 16 | 71, 10, 30, 0,                                 // OpDecorate %10 Location 0
    5u << 16 | 72, 6, 0, 11, 0,                               // OpMemberDecorate %6 0 BuiltIn Position
    3u << 16 | 71, 6, 2,                                      // OpDecorate %6 Block
    2u << 16 | 19, 1,                                         // %1 void
    3u << 16 | 33, 2, 1,                                      // %2 fn void
    3u << 16 | 22, 3, 32,                                     // %3 f32
    4u << 16 | 23, 4, 3, 2,                                   // %4 vec2
    4u << 16 | 23, 5, 3, 4,                                   // %5 vec4
    3u << 16 | 30, 6, 5,                                      // %6 struct { vec4 }
    4u << 16 | 32, 7, 3, 6,                                   // %7 ptr Output %6
    4u << 16 | 59, 7, 8, 3,                                   // %8 var Output
    4u << 16 | 32, 9, 1, 4,                                   // %9 ptr Input vec2
    4u << 16 | 59, 9, 10, 1,                                  // %10 var Input
    4u << 16 | 21, 11, 32, 1,                                 // %11 i32
    4u << 16 | 43, 11, 12, 0,                                 // %12 = 0
    4u << 16 | 43, 3, 13, 0,                                  // %13 = 0.0
    4u << 16 | 43, 3, 14, 0x3f800000,                         // %14 = 1.0
    4u << 16 | 32, 15, 3, 5,                                  // %15 ptr Output vec4
    5u << 16 | 54, 1, 16, 0, 2,                               // %16 function
    2u << 16 | 248, 17,                                       // label
    4u << 16 | 61, 4, 18, 10,                                 // %18 = load %10
    5u << 16 | 81, 3, 19, 18, 0,                              // %19 = %18.x
    5u << 16 | 81, 3, 20, 18, 1,                              // %20 = %18.y
    7u << 16 | 80, 5, 21, 19, 20, 13, 14,                     // %21 = vec4(%19, %20, 0, 1)
    5u << 16 | 65, 15, 22, 8, 12,                             // %22 = &%8[0]
    3u << 16 | 62, 22, 21,                                    // store
    1u << 16 | 253,                                           // return
    1u << 16 | 56,                                            // end
};
static int statics(void) {
    if (g_static_ok) return g_static_ok > 0 ? 0 : -1;
    const vkn_dev *d = vkn_device(); if (!d) return -1;
    g_static_ok = -1;
    const VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 16.0f, .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    if (vkCreateSampler(d->dev, &si, NULL, &g_smp) != VK_SUCCESS) { UI_RED(RR_STATIC, "vkCreateSampler failed: the UI stays off"); return -1; }
    if (vkn_block_alloc(VKN_ARENA_STATIC, (uint64_t)QUADS * 6 * 4, 16, &g_quad_ib) || vkn_block_alloc(VKN_ARENA_STATIC, 16, 16, &g_null_vb)) {
        UI_RED(RR_STATIC, "no arena block for the quad pattern: the UI stays off"); return -1;
    }
    uint32_t *q = (uint32_t *)g_quad_ib.cpu;
    for (uint32_t i = 0; i < QUADS; i++) { const uint32_t v = 4 * i; q[6 * i] = v; q[6 * i + 1] = v + 1; q[6 * i + 2] = v + 2; q[6 * i + 3] = v; q[6 * i + 4] = v + 2; q[6 * i + 5] = v + 3; }
    const float one[4] = {0.0f, 0.0f, 0.0f, 1.0f}; memcpy(g_null_vb.cpu, one, sizeof one);
    // placeholder: 1x1 RGBA8 transparent black, never linked to an engine handle (index 0xffffffff is beyond every table)
    vkn_tex_desc pd; memset(&pd, 0, sizeof pd);
    pd.nvn_fmt = 0x25; pd.nvn_target = 1; pd.w = pd.h = pd.depth = pd.mips = pd.layers = pd.samples = 1; pd.cls = VKN_TC_SAMPLED;
    pd.pool_id = ~0u; for (int i = 0; i < VKN_TEX_LAYER_IDS; i++) pd.layer_ids[i] = ~0u; pd.sampler_id = ~0u; pd.cookie = (uint64_t)(uintptr_t)&g_ph_cookie;
    static const uint32_t zero = 0;
    const vkn_tex_region rg = {.level = 0, .layer = 0, .z = 0, .w = 1, .h = 1, .d = 1, .src = &zero};
    const vkn_tex_src ps = {.kind = VKN_SRC_REGIONS, .reg = &rg, .nreg = 1};
    const char *why = NULL;
    if (!(g_ph = vkn_tex_create(1ull << 32 | 0xffffffffull, &pd, &ps, &why))) { UI_RED(RR_STATIC, "placeholder texture failed (%s): the UI stays off", why ? why : "?"); return -1; }
    const VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_clr_vs, .pCode = k_clr_vs};
    { const int lk = vkn_drv_lock(); const VkResult r = vkCreateShaderModule(d->dev, &mi, NULL, &g_clr_vs); vkn_drv_unlock(lk);   // [stage 5.3 V2] the creation lock
      if (r != VK_SUCCESS) g_clr_vs = VK_NULL_HANDLE; }   // masked stencil clears off (counted)
    const VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (vkCreatePipelineLayout(d->dev, &li, NULL, &g_clr_layout) != VK_SUCCESS) g_clr_layout = VK_NULL_HANDLE;
    g_static_ok = 1;
    return 0;
}

// ---- dynamic state of the next draws ----
static VkViewport g_vp; static VkRect2D g_sc; static int g_vp_set, g_sc_set;
static vkn_tex *g_color, *g_depth;
void vkn_ui_viewport(int32_t x, int32_t y, int32_t w, int32_t h) { g_vp = (VkViewport){(float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f}; g_vp_set = 1; }
void vkn_ui_scissor(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    g_sc = (VkRect2D){{x, y}, {(uint32_t)(w > 0 ? w : 0), (uint32_t)(h > 0 ? h : 0)}}; g_sc_set = 1;
}
// ==== [stage 5.2 B2] the UI pass on tile GPUs (DM_VK_NATIVE_UIPASS: the rule is in vkn_ui.h) ====
// State (recording thread): g_up the switches, g_uph / g_upo the history of pass starts and this frame's ordinals (nb_uiarea.h /
// nr_uix.h as the old path uses them), g_ep the epoch of the current GDraw target set. Costs nothing at level 0 (g_ep never turns on).
int vkn_pass_area_hint(const VkRect2D *area) __attribute__((weak));
int vkn_pass_area_get(VkRect2D *out) __attribute__((weak));
enum { UP_DECAY = 120, UP_ALIGN = 32, UP_MARGIN = 4, UP_MIN_SAVE = 100, UP_REFRESH = 60 };   // xbg_nb_uiarea.inc k_align / k_margin / k_min_save, k_decay, refresh
enum { UP_KEY_CLEAR = 0xc1ea, UP_KEY_DS = 0xd5c1 };   // first-event kinds of the key (a draw uses its fragment program)
static struct { int req, verify, rule, sim_tiler, configured, resolved, mode, tile; } g_up;
static vkn_ui_pass_stats g_ups;
static nb_ua_hist g_uph; static nr_uix_ord g_upo;
static uint64_t g_up_frame = ~0ull; static uint32_t g_up_fno; static uint8_t g_up_fds, g_up_fds_prev;   // presented frame; rule 1: DS used this / last frame
typedef struct up_ep {
    uint8_t on, decided, shared, dref, readied, fenced;
    uint8_t detached, began_det, reattached, used;        // applied DS state; a DS-needing event came
    uint8_t hint, area_seen, narrowed, restarted;         // applied area state
    uint8_t wdetach, wmiss, warea, wrestart;              // verify: would detach / would have re-attached / would narrow / would restart
    vkn_tex *c, *d; uint32_t serial, w, h; uint64_t key2;
    nb_ua_rect uni, area;
} up_ep;
static up_ep g_ep;

static void up_fx(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user);
static void up_commit(void);
void vkn_ui_pass_config(int mode, int verify, int rule, int sim_tiler) {
    g_up.req = mode < -1 || mode > 3 ? 0 : mode; g_up.verify = verify ? 1 : 0; g_up.rule = rule == 1 ? 1 : 0; g_up.sim_tiler = sim_tiler ? 1 : 0;
    if (g_ep.on) up_commit();   // (its reference on the depth target)
    g_up.configured = 1; g_up.resolved = 0; g_up.mode = 0;
    nb_ua_hist_reset(&g_uph); nr_uix_ord_reset(&g_upo); memset(&g_ep, 0, sizeof g_ep);
    static int fx; if (!fx && g_up.req) fx = !vkn_fx_register("ui-pass-fence", up_fx, NULL, 1);   // first in the chain
}
// The switch in effect: the family default needs the device (vendor ARM 0x13b5 / Imagination 0x1010, or a DM_GFX_SIM_GPU tile model:
// xbg_vendor_policy.h xbg_tile_family through xbg_effective_family; this module reads the vendor itself, vkn_policy.h is not used).
static int up_on(void) {
    if (g_up.resolved) return g_up.mode;
    if (!g_up.configured) return 0;
    const vkn_dev *d = vkn_device();
    if (!d) return 0;
    g_up.resolved = 1;
    g_up.tile = d->props.vendorID == 0x13b5 || d->props.vendorID == 0x1010 || g_up.sim_tiler;
    g_up.mode = g_up.req < 0 ? (g_up.tile ? 2 : 0) : g_up.req;
    g_ups.mode = g_up.mode; g_ups.verify = g_up.verify; g_ups.rule = g_up.rule; g_ups.tile = g_up.tile; g_ups.api = vkn_pass_area_hint && vkn_pass_area_get;
    vkn_logf("ui pass (stage 5.2 B2): DM_VK_NATIVE_UIPASS=%d (%s; %s; replaces DM_NR_UIX_DS / DM_NR_UIX_AREA), verify %d, rule %d (%s), tile family %d (vendor %#x, sim %d), pass area functions %s",
             g_up.mode, g_up.req < 0 ? "family default" : "env", g_up.mode == 0 ? "off" : g_up.mode == 1 ? "depth-stencil detach" : g_up.mode == 2 ? "depth-stencil detach + render area" : "stress: every epoch detached, area = first rect",
             g_up.verify, g_up.rule, g_up.rule ? "per frame" : "per pass start", g_up.tile, d->props.vendorID, g_up.sim_tiler, g_ups.api ? "present" : "absent (level 2 = level 1)");
    return g_up.mode;
}
static void up_frame(void) {
    const uint64_t f = vkn_frame();
    if (f == g_up_frame) return;
    g_up_frame = f; g_up_fno++; g_up_fds_prev = g_up_fds; g_up_fds = 0;
    nr_uix_ord_frame(&g_upo); g_ups.ui_frames++;
}
static nb_ua_rect up_rect(int32_t x0, int32_t y0, int32_t x1, int32_t y1) { const nb_ua_rect r = {x0, y0, x1, y1}; return r; }
static VkRect2D up_vk(nb_ua_rect r) { const VkRect2D v = {{r.x0, r.y0}, {(uint32_t)(r.x1 - r.x0), (uint32_t)(r.y1 - r.y0)}}; return v; }
static int up_narrow(const VkRect2D *a) { return a->offset.x || a->offset.y || a->extent.width < g_ep.w || a->extent.height < g_ep.h; }
// 1 = the current set of vkn_pass is the epoch's (colour, depth or none when detached): nobody changed it since
static int up_mine(void) {
    uint32_t n = 0; vkn_tex *c[VKN_PASS_COLORS], *d = NULL;
    vkn_pass_cur_get(&n, c, NULL, NULL, &d);
    return n == 1 && c[0] == g_ep.c && d == (g_ep.detached ? NULL : g_ep.d);
}
static void up_set(vkn_tex *d) {   // the epoch's colour with d; the pass module drops an area hint when the set changes: given again
    vkn_tex *c[VKN_PASS_COLORS] = {g_ep.c}; const uint32_t z[VKN_PASS_COLORS] = {0};
    vkn_pass_targets(1, c, z, z, d);
    if (g_ep.hint) { VkRect2D a = up_vk(g_ep.area); if (vkn_pass_area_hint(&a)) g_ep.hint = 0; }
}
static void up_hint_drop(void) { if (g_ep.hint) { vkn_pass_area_hint(NULL); g_ep.hint = 0; } }
// 1 = a pass is open for the current set (the pass module's answer; without it: the epoch has opened one in this recording)
static int up_open(void) { VkRect2D a; return vkn_pass_area_get ? vkn_pass_area_get(&a) : g_ep.readied; }
// The epoch ends: the history learns what this pass start did (NR-UIX commit: its DS use, the union of its rects), stats.
static void up_commit(void) {
    if (!g_ep.on) return;
    if (g_ep.decided) {
        nb_ua_hent *f = nb_ua_hist_put(&g_uph, g_ep.key2);
        if (f) {
            if (g_ep.used) { f->ds = 1; f->frame_ds = g_up_fno; }
            if (nb_ua_empty(f->uni) || f->frame + UP_REFRESH < g_up_fno) f->uni = g_ep.uni; else f->uni = nb_ua_union(f->uni, g_ep.uni);
            f->frame = g_up_fno;
        }
        const uint64_t full = (uint64_t)g_ep.w * g_ep.h;
        const int off = g_ep.began_det && !g_ep.reattached, natt = 1 + (g_ep.d && !off);
        g_ups.epochs++; g_ups.epochs_ds += g_ep.d != NULL; g_ups.px_full += full;
        if (off) { g_ups.kept_off++; g_ups.ds_bytes += full * 8u; }   // D24S8: 4 B a pixel, load + store
        const uint64_t px = g_ep.narrowed && !g_ep.restarted ? nb_ua_px(g_ep.area) : full;
        g_ups.px_area += px; g_ups.area_bytes += (full - px) * 4u * (uint64_t)natt * 2u;
        if (g_ep.wdetach && !g_ep.wmiss) g_ups.w_ds_bytes += full * 8u;
        if (g_ep.warea && !g_ep.wrestart) g_ups.w_area_bytes += (full - nb_ua_px(g_ep.area)) * 4u * (uint64_t)(1 + (g_ep.d && !(g_ep.wdetach && !g_ep.wmiss))) * 2u;
    }
    if (g_ep.dref) vkn_tex_unref(g_ep.d);
    memset(&g_ep, 0, sizeof g_ep);
}
// A set the engine changed under the UI (nothing of the epoch is current): ends it without touching vkn_pass.
static void up_lost(void) { g_ups.set_lost++; g_ep.hint = 0; up_commit(); }
// The first pass-opening event of the epoch: rect r, `what` = its fragment program or kind, full_clear = a clear of the whole colour.
static void up_decide(nb_ua_rect r, uint64_t what, int full_clear) {
    g_ep.decided = 1;
    const int stress = g_up.mode == 3;
    // (object addresses: every GDraw texture has the same handle 1 << 32 | 0xffffffff, hle_dos2_vkn_ui.c tex_mirror)
    uint64_t k = nr_uix_mix(nr_uix_mix(0x55aa, (uint64_t)(uintptr_t)g_ep.c), (uint64_t)(uintptr_t)g_ep.d);
    k = nr_uix_mix(nr_uix_mix(k, what), (uint64_t)g_sc_set << 40 ^ (uint64_t)(uint32_t)g_sc.offset.x << 20 ^ (uint32_t)g_sc.offset.y);
    k = nr_uix_mix(nr_uix_mix(k, (uint64_t)g_sc.extent.width << 32 | g_sc.extent.height), (uint64_t)g_vp_set << 62 ^ (uint64_t)(int64_t)g_vp.x << 42 ^ (uint64_t)(int64_t)g_vp.y << 21 ^ (uint64_t)(int64_t)g_vp.width);
    k = nr_uix_mix(k, (uint64_t)(int64_t)g_vp.height);
    g_ep.key2 = nr_uix_key2(k, nr_uix_ord_take(&g_upo, k));
    const nb_ua_hent *e = nb_ua_hist_find(&g_uph, g_ep.key2);
    if (g_ep.d) {   // depth-stencil detach
        int why = -1, det = 0;
        if (!g_ep.dref) why = VKN_UIP_DS_DYING;
        else if (g_ep.d->w != g_ep.c->w || g_ep.d->h != g_ep.c->h || g_ep.d->samples != g_ep.c->samples) why = VKN_UIP_DS_SIZE;   // (the pass is the smallest attachment)
        else if (g_ep.shared && (!vkn_pass_area_get || up_open())) why = VKN_UIP_DS_SHARED;   // the set was current before GDraw took it, a pass may be open: detaching would break it
        else if (stress) det = 1;
        else if (g_up.rule == 1) { det = !g_up_fds_prev; if (!det) why = VKN_UIP_DS_FRAME; }
        else { det = !nr_uix_ds_keep(e, g_up_fno, UP_DECAY); if (!det) why = VKN_UIP_DS_HIST; }
        if (why >= 0) g_ups.ds_refused[why]++;
        if (det && g_up.verify) { g_ep.wdetach = 1; g_ups.w_detach++; }
        else if (det) { g_ep.detached = g_ep.began_det = 1; up_set(NULL); g_ups.detached++; }
    }
    if (g_up.mode >= 2) {   // render area
        int why = -1; nb_ua_rect s = nb_ua_none();
        const nb_ua_hent *h = !stress && e && !nb_ua_empty(e->uni) ? e : NULL;
        if (!g_up.verify && !g_ups.api) why = VKN_UIP_AR_API;
        else if (full_clear || g_ep.c->pass_nclr || (g_ep.d && !g_ep.detached && g_ep.d->pass_dclr)) why = VKN_UIP_AR_CLEAR;   // CLEAR load: clears only inside the area
        else if (g_ep.c->samples > 1) why = VKN_UIP_AR_MS;
        else if (!nb_ua_seed(r, h ? &h->uni : NULL, g_ep.w, g_ep.h, stress ? 1 : UP_ALIGN, UP_MIN_SAVE, &s)) why = VKN_UIP_AR_SAVE;
        if (why >= 0) g_ups.area_refused[why]++;
        else if (g_up.verify) { g_ep.area = s; g_ep.warea = 1; g_ups.w_area++; }
        else { g_ep.area = s; const VkRect2D a = up_vk(s); if (!vkn_pass_area_hint(&a)) { g_ep.hint = 1; g_ups.area_hinted++; } }
    }
}
// Before an event of the epoch that may open / use the pass: rect r it can touch, needs_ds = it tests / writes depth or stencil.
static void up_event(nb_ua_rect r, int needs_ds, uint64_t what, int full_clear) {
    if (!up_mine()) { up_lost(); return; }
    up_frame();
    if (!g_ep.decided) up_decide(r, what, full_clear);
    g_ep.uni = nb_ua_union(g_ep.uni, r);
    if (needs_ds) {
        g_ep.used = 1; g_up_fds = 1;
        if (g_ep.detached) { g_ep.detached = 0; g_ep.reattached = 1; up_set(g_ep.d); g_ups.reattach++; }   // the DS-less pass ends, the next LOADs both
        if (g_ep.wdetach && !g_ep.wmiss) { g_ep.wmiss = 1; g_ups.w_reattach++; }
    }
    if (g_ep.hint && !nb_ua_contains(g_ep.area, r)) {   // outside the area: the narrowed pass ends, the rest of the epoch is full area (LOAD)
        VkRect2D a;
        if (vkn_pass_area_get(&a) && up_narrow(&a)) { vkn_pass_end(); g_ups.restarts++; g_ep.restarted = 1; } else g_ups.hint_dropped++;
        up_hint_drop();
    }
    if (g_ep.warea && !g_ep.wrestart && !nb_ua_contains(g_ep.area, r)) { g_ep.wrestart = 1; g_ups.w_restart++; }
}
// After the pass of the epoch was made open (vkn_pass_ready ok): the area it really began with, once.
static void up_opened(void) {
    g_ep.readied = 1;
    if (!g_ep.hint || g_ep.area_seen) return;
    g_ep.area_seen = 1;
    VkRect2D a;
    if (vkn_pass_area_get(&a) && up_narrow(&a)) { g_ep.narrowed = 1; g_ups.area_applied++; }
    else g_ups.area_ignored++;   // (begun full: a CLEAR load op, a poison mode)
}
// The rect a GDraw draw can touch: viewport (+ margin) ∩ scissor ∩ target (as vkn_ui_draw sets them; unset = the whole target).
static nb_ua_rect up_draw_rect(void) {
    const int vp = g_vp_set && g_vp.width > 0.0f && g_vp.height > 0.0f;
    return nb_ua_draw_rect(vp ? g_vp.x : 0.0f, vp ? g_vp.y : 0.0f, vp ? g_vp.width : (float)g_ep.w, vp ? g_vp.height : (float)g_ep.h,
                           g_sc_set ? g_sc.offset.x : 0, g_sc_set ? g_sc.offset.y : 0, g_sc_set ? g_sc.extent.width : g_ep.w, g_sc_set ? g_sc.extent.height : g_ep.h,
                           g_ep.w, g_ep.h, UP_MARGIN);
}
// vkn_ui_targets at level >= 1. 1 = the epoch continues (the same set is still current: the stage-3 call would change nothing);
// 0 = a new epoch, *shared = the set (c, d) was current already and may have an open pass that is not the UI's.
static int up_targets(vkn_tex *c, vkn_tex *d, int *shared) {
    up_frame();
    const int same = g_ep.on && g_ep.c == c && g_ep.d == d, mine = g_ep.on && up_mine();
    if (same && mine && g_ep.serial == vkn_cb_serial() && !g_ep.fenced) return 1;
    up_commit();   // (a set that is not the epoch's any more: the pass module dropped the hint with the set change / the recording)
    uint32_t n = 0; vkn_tex *cc[VKN_PASS_COLORS], *cd = NULL;
    vkn_pass_cur_get(&n, cc, NULL, NULL, &cd);
    // (c, d) current already and not by this epoch (ours ended by a fence or a new recording: nothing of it is open): the engine's
    *shared = !(same && mine) && n == 1 && cc[0] == c && cd == d;
    return 0;
}
static void up_begin(vkn_tex *c, vkn_tex *d, int shared) {
    memset(&g_ep, 0, sizeof g_ep);
    if (!c || c->dead) return;
    g_ep.on = 1; g_ep.c = c; g_ep.d = d; g_ep.shared = (uint8_t)shared; g_ep.serial = vkn_cb_serial();
    g_ep.dref = d && vkn_tex_ref(d) ? 1 : 0;
    g_ep.w = d && d->w < c->w ? d->w : c->w; g_ep.h = d && d->h < c->h ? d->h : c->h;
    g_ep.uni = nb_ua_none(); g_ep.area = nb_ua_none();
}
void vkn_ui_pass_fence(void) {
    if (!g_ep.on) return;
    if (!up_mine()) { up_lost(); return; }
    int closed = 0;
    if (g_ep.hint) {
        VkRect2D a; const int open = vkn_pass_area_get(&a);
        if (open && up_narrow(&a)) { vkn_pass_end(); g_ups.fence_end++; }
        closed = !open || up_narrow(&a);
        up_hint_drop();
    }
    if (g_ep.detached) { g_ep.detached = 0; up_set(g_ep.d); g_ups.fence_reattach++; closed = 1; }   // (ends the DS-less pass)
    if (closed) g_ep.fenced = 1;   // nothing of the set is open: the next vkn_ui_targets of it begins a new epoch (a new decision)
}
// vkn_fx chain, every engine draw: the engine draws with what NVN has bound - GDraw's whole set, full area (a custom draw inside the UI)
static void up_fx(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)in; (void)ctx; (void)out; (void)user;
    if (g_ep.on && (g_ep.detached || g_ep.hint)) { vkn_ui_pass_fence(); g_ups.fence_draw++; }
}
void vkn_ui_pass_stats_get(vkn_ui_pass_stats *s) { if (s) { *s = g_ups; if (!g_up.resolved) s->mode = -1; } }
// ==== [/stage 5.2 B2] ====

void vkn_ui_targets(vkn_tex *color, vkn_tex *depth) {
    int shared = 0;
    if (up_on() && up_targets(color, depth, &shared)) { g_st.targets++; return; }   // [stage 5.2 B2]
    vkn_tex *c[VKN_PASS_COLORS] = {color};
    const uint32_t zero[VKN_PASS_COLORS] = {0};
    vkn_pass_targets(1, c, zero, zero, depth);
    g_color = color; g_depth = depth;
    g_st.targets++;
    if (g_up.mode) up_begin(color, depth, shared);   // [stage 5.2 B2]
}

// ---- pipelines ----
typedef struct pipe_k { uint32_t pair; uint8_t vfmt, blend, ds, color_off, scmp, swr, wmask, samples; uint16_t compat, clear; } pipe_k;
typedef struct pipe_e { pipe_k k; VkPipeline p; } pipe_e;
static pipe_e g_pipe[PIPES]; static uint32_t g_npipe;
static void blend_of(uint8_t b, VkPipelineColorBlendAttachmentState *a) {
    memset(a, 0, sizeof *a);
    VkBlendFactor s = VK_BLEND_FACTOR_ONE, dfac = VK_BLEND_FACTOR_ZERO; int on = 0;
    switch (b) {
    case 1: on = 1; s = VK_BLEND_FACTOR_ONE; dfac = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; break;
    case 2: on = 1; s = VK_BLEND_FACTOR_DST_COLOR; dfac = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; break;
    case 3: on = 1; s = VK_BLEND_FACTOR_ONE; dfac = VK_BLEND_FACTOR_ONE; break;
    default: break;   // 0 none, 4 filter, 5 special: blending off
    }
    a->blendEnable = on ? VK_TRUE : VK_FALSE;
    a->srcColorBlendFactor = a->srcAlphaBlendFactor = s; a->dstColorBlendFactor = a->dstAlphaBlendFactor = dfac;
    a->colorBlendOp = a->alphaBlendOp = VK_BLEND_OP_ADD;
}
// The translator's vertex retype policy (vkn_draw.c retype_on: DM_GFX_VTX_RETYPE, default on except Adreno).
static int retype_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("DM_GFX_VTX_RETYPE"); const vkn_dev *d = vkn_device(); on = e && *e ? atoi(e) != 0 : d ? d->props.vendorID != 0x5143 : 0; }
    return on;
}
static VkPipeline pipe_get(const pipe_k *k, const vkn_shader *s, const vkn_pass_info *pi) {
    for (uint32_t i = 0; i < g_npipe; i++) if (!memcmp(&g_pipe[i].k, k, sizeof *k)) return g_pipe[i].p;
    const vkn_dev *d = vkn_device(); if (!d) return VK_NULL_HANDLE;
    if (g_npipe >= PIPES) { UI_RED(RR_PIPE, "more than %d UI pipelines: draw dropped", PIPES); return VK_NULL_HANDLE; }
    // vertex input: binding 0 by format, location 0 RG32F, location 1 by format; shader inputs the format lacks read the null stream
    VkVertexInputBindingDescription vb[2]; VkVertexInputAttributeDescription va[18]; uint32_t nb = 0, na = 0; uint32_t have = 0;
    VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
    if (k->clear) {
        vb[nb++] = (VkVertexInputBindingDescription){0, 8, VK_VERTEX_INPUT_RATE_VERTEX};
        va[na++] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
        vsm = g_clr_vs;
    } else {
        vb[nb++] = (VkVertexInputBindingDescription){0, k->vfmt == VKN_UI_VF_V2 ? 8u : 16u, VK_VERTEX_INPUT_RATE_VERTEX};
        va[na++] = (VkVertexInputAttributeDescription){0, 0, VK_FORMAT_R32G32_SFLOAT, 0}; have |= 1;
        uint64_t retype = 0;
        if (k->vfmt == VKN_UI_VF_V2AA) {
            va[na++] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R16G16B16A16_SINT, 8}; have |= 2;
            for (uint32_t i = 0; i < s->ninputs && i < 16; i++) if (s->inputs[i] == 1 && s->input_classes[i] != 1 && retype_on()) retype |= 2ull << 2;
        } else if (k->vfmt == VKN_UI_VF_V2TC2) { va[na++] = (VkVertexInputAttributeDescription){1, 0, VK_FORMAT_R32G32_SFLOAT, 8}; have |= 2; }
        int null_used = 0;
        for (uint32_t i = 0; i < s->ninputs && i < 16; i++) {
            const uint32_t l = s->inputs[i];
            if (l < 16 && !(have >> l & 1)) { va[na++] = (VkVertexInputAttributeDescription){l, VKN_VFMT_NULL_BINDING, VK_FORMAT_R32G32B32A32_SFLOAT, 0}; have |= 1u << l; null_used = 1; }
        }
        if (null_used) vb[nb++] = (VkVertexInputBindingDescription){VKN_VFMT_NULL_BINDING, 0, VK_VERTEX_INPUT_RATE_VERTEX};
        vsm = vkn_shader_module(s, VKN_STAGE_VS, 0, 0.0f, retype);
        fsm = vkn_shader_module(s, VKN_STAGE_FS, 0, 0.0f, 0);
        if (!vsm || !fsm) { UI_RED(RR_PIPE, "pair %u: no module (vertex %p, fragment %p, retype %#llx)", k->pair, (void *)vsm, (void *)fsm, (unsigned long long)retype); return VK_NULL_HANDLE; }
    }
    const VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main"}};
    const VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = nb,
        .pVertexBindingDescriptions = vb, .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    const VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    const VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.0f};
    const VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = (VkSampleCountFlagBits)(k->samples ? k->samples : 1)};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (k->clear) {   // stencil bits `swr` to 0 everywhere: always pass, REPLACE with reference 0 under the write mask
        ds.stencilTestEnable = VK_TRUE;
        ds.front = (VkStencilOpState){VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS, 0, k->swr, 0};
        ds.back = ds.front;
    } else {
        const int stencil = k->ds & 1, test = k->ds >> 1 & 1, write = k->ds >> 2 & 1;
        ds.depthTestEnable = test || write ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = write ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = test ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_ALWAYS;
        if (stencil) {
            ds.stencilTestEnable = VK_TRUE;
            ds.front = (VkStencilOpState){VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, k->scmp, k->swr, 0xff};
            ds.back = ds.front;
        }
    }
    VkPipelineColorBlendAttachmentState ba[VKN_PASS_COLORS];   // slot 0 is GDraw's target; further slots (an engine MRT set a clear runs in) write nothing
    for (uint32_t i = 0; i < VKN_PASS_COLORS; i++) { blend_of(k->clear ? 0 : k->blend, &ba[i]); ba[i].colorWriteMask = 0; }
    ba[0].colorWriteMask = k->color_off || k->clear ? 0 : k->wmask;
    const VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = pi->ncolor < VKN_PASS_COLORS ? pi->ncolor : VKN_PASS_COLORS, .pAttachments = ba};
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    const VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = k->clear ? 1u : 2u, .pStages = st,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy, .layout = k->clear ? g_clr_layout : s->layout, .renderPass = pi->pass, .subpass = 0};
    VkPipeline p = VK_NULL_HANDLE;
    const int lk = vkn_drv_lock(); const VkResult pr = vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gi, NULL, &p); vkn_drv_unlock(lk);   // [stage 5.3 V2]
    if (pr != VK_SUCCESS) {
        p = VK_NULL_HANDLE; g_st.pipe_fail++;
        UI_RED(RR_PIPE, "vkCreateGraphicsPipelines failed: pair %u, format %u, blend %u, depth-stencil %u, clear %u, samples %u", k->pair, k->vfmt, k->blend, k->ds, k->clear, k->samples);
    } else g_st.pipelines++;
    g_pipe[g_npipe].k = *k; g_pipe[g_npipe].p = p; g_npipe++;   // failures are cached too (no rebuild storm)
    return p;
}

// ---- descriptor pools of the current recording ----
enum { MAX_POOLS = 64 };
static VkDescriptorPool g_pool[MAX_POOLS]; static uint32_t g_npool, g_pool_left, g_pool_serial; static int g_pool_valid;
static void pool_destroy(void *ctx) { const vkn_dev *d = vkn_device(); if (d) vkDestroyDescriptorPool(d->dev, (VkDescriptorPool)(uintptr_t)ctx, NULL); }
static VkDescriptorSet set_alloc(VkDescriptorSetLayout lay) {
    const vkn_dev *d = vkn_device(); if (!d) return VK_NULL_HANDLE;
    if (!g_pool_valid || g_pool_serial != vkn_cb_serial()) {   // a new recording: the last one's pools go after the frame delay
        for (uint32_t i = 0; i < g_npool; i++) vkn_retire(pool_destroy, (void *)(uintptr_t)g_pool[i]);
        g_npool = 0; g_pool_left = 0; g_pool_serial = vkn_cb_serial(); g_pool_valid = 1;
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!g_pool_left || !g_npool) {
            if (g_npool >= MAX_POOLS) return VK_NULL_HANDLE;
            const VkDescriptorPoolSize sz[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, POOL_SETS * 8}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, POOL_SETS * 8},
                                                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, POOL_SETS * 8}};
            const VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = POOL_SETS, .poolSizeCount = 3, .pPoolSizes = sz};
            VkDescriptorPool p = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(d->dev, &ci, NULL, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
            g_pool[g_npool++] = p; g_pool_left = POOL_SETS; g_st.pools++;
        }
        const VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_pool[g_npool - 1], .descriptorSetCount = 1, .pSetLayouts = &lay};
        VkDescriptorSet s = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(d->dev, &ai, &s) == VK_SUCCESS) { g_pool_left--; g_st.sets++; return s; }
        g_pool_left = 0;   // this pool is out of some descriptor kind: the next one
    }
    return VK_NULL_HANDLE;
}

// A ring copy of n bytes (zero-padded to `pad` bytes, 16-byte multiple); *buf / *off the descriptor / binding source. 0 = ok.
static int ring_put(const void *src, uint32_t n, uint32_t pad, VkBuffer *buf, uint64_t *off, uint64_t *size) {
    if (pad < n) pad = n;
    pad = (pad + 15u) & ~15u; if (!pad) pad = 16;
    // [stage 5.5] the recording ring (vkn_buf.h vkn_ring2_*): these copies are made where the recording is, the engine ring's offsets
    // are handed out by the engine thread (with the recording thread two timelines: each ring keeps one order)
    uint32_t ro = 0; uint8_t *cpu = (uint8_t *)vkn_ring2_cpu();
    if (!cpu || vkn_ring2_alloc(pad, &ro)) return -1;
    if (n && src) memcpy(cpu + ro, src, n);
    if (pad > n) memset(cpu + ro + n, 0, pad - n);
    *buf = vkn_ring2_buffer(); *off = vkn_ring2_base() + ro; if (size) *size = pad;
    g_st.ring_bytes += pad;
    return *buf ? 0 : -1;
}

static int unit_of_slot(uint32_t slot) { return slot <= 2 ? (int)slot : slot == 7 ? 3 : -1; }

int vkn_ui_draw(const vkn_ui_draw_in *in) {
    VkCommandBuffer cb = vkn_cb();
    if (!cb) { g_st.drop_cb++; UI_RED(RR_CB, "a draw outside any recording: dropped"); return 1; }
    if (statics()) { g_st.drop_shader++; return 1; }
    if (in->quads ? in->nverts < 4 : !in->nidx) { g_st.drop_empty++; return 1; }
    uint32_t pair = 0;
    const vkn_shader *s = pair_get(in->vs, in->fs, &pair);
    if (!s) { g_st.drop_shader++; return 1; }
    if (g_ep.on) up_event(up_draw_rect(), in->ds != 0, in->fs, 0);   // [stage 5.2 B2] decisions / re-attach / restart before the pass opens
    // textures first (layouts; a pass is ended where needed), then the pass
    vkn_tex *tex[VKN_UI_UNITS];
    for (int u = 0; u < VKN_UI_UNITS; u++) {
        vkn_tex *t = in->tex[u];
        if (t && (t->dead || !t->view)) { t = NULL; g_st.placeholder++; }
        if (t && g_ep.on && g_ep.detached && t == g_ep.d) { t = NULL; g_st.feedback++; }   // [stage 5.2 B2] NVN has it bound: as vkn_pass_sample answers
        if (t && vkn_pass_sample(t)) { t = NULL; g_st.feedback++; }
        tex[u] = t ? t : g_ph;
    }
    if (vkn_pass_sample(g_ph)) { g_st.drop_bind++; return 1; }
    vkn_pass_info pi;
    if (vkn_pass_ready(&pi)) { g_st.drop_pass++; UI_RED(RR_PASS, "no pass for the UI targets (colour %p, depth %p): draw dropped", (void *)g_color, (void *)g_depth); return 1; }
    if (g_ep.on) up_opened();   // [stage 5.2 B2]
    // descriptor set
    VkDescriptorBufferInfo bi[32]; VkDescriptorImageInfo ii[32]; VkWriteDescriptorSet wr[32]; uint32_t nw = 0, nbi = 0, nii = 0;
    uint32_t dyn[32]; uint32_t ndyn = 0;
    const float vw = g_vp_set ? g_vp.width : (float)pi.width, vh = g_vp_set ? g_vp.height : (float)pi.height;
    for (uint32_t i = 0; i < s->nbind; i++) {
        const vkn_bind *b = &s->bind[i];
        if (nw >= 32 || b->count != 1) { g_st.drop_bind++; UI_RED(RR_BIND, "pair %u binding %u: %u descriptors (or more than 32 bindings): draw dropped", pair, b->binding, b->count); return 1; }
        VkWriteDescriptorSet *w = &wr[nw];
        memset(w, 0, sizeof *w);
        w->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w->dstBinding = b->binding; w->descriptorCount = 1; w->descriptorType = b->type;
        if (b->kind == VKN_BK_UNIFORM || b->kind == VKN_BK_SUPPORT || b->kind == VKN_BK_C1) {
            VkDescriptorBufferInfo *o = &bi[nbi++];
            if (b->kind == VKN_BK_UNIFORM) {
                const void *src = NULL; uint32_t n = 0;
                if (b->slot == 0 && b->stage == VKN_STAGE_VS) { src = in->vsc; n = in->vsc_bytes; }
                else if (b->slot == 0 && b->stage == VKN_STAGE_FS) { src = in->fsc; n = in->fsc_bytes; }
                else if (b->slot == 1 && b->stage == VKN_STAGE_FS) { src = in->fsc1; n = src ? in->fsc1_bytes : 0; }
                uint32_t want = b->bytes ? b->bytes : 256; if (want > 4096) want = 4096;
                if (ring_put(src, n, want, &o->buffer, &o->offset, &o->range)) { g_st.drop_ring++; UI_RED(RR_RING, "no ring space for a uniform block: draw dropped"); return 1; }
            } else if (b->kind == VKN_BK_SUPPORT) {
                vkn_block sb;
                if (vkn_shader_support(vw > 0 ? vw : 1.0f, vh > 0 ? vh : 1.0f, &sb)) { g_st.drop_bind++; UI_RED(RR_BIND, "no SUPPORT block: draw dropped"); return 1; }
                o->buffer = sb.buf; o->offset = sb.off; o->range = sb.size;
            } else {
                const vkn_block *c1 = &s->c1[b->stage == VKN_STAGE_VS ? VKN_SRC_VS : VKN_SRC_FS];
                if (!c1->size) { g_st.drop_bind++; UI_RED(RR_BIND, "pair %u: C1 binding without data: draw dropped", pair); return 1; }
                o->buffer = c1->buf; o->offset = c1->off; o->range = c1->size;
            }
            if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) { if (ndyn < 32) dyn[ndyn++] = 0; }
            w->pBufferInfo = o;
        } else if (b->kind == VKN_BK_TEXTURE && b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            const int u = b->stage == VKN_STAGE_FS ? unit_of_slot(b->slot) : -1;
            vkn_tex *t = u >= 0 ? tex[u] : g_ph;
            VkDescriptorImageInfo *o = &ii[nii++];
            o->sampler = g_smp; o->imageView = t->view; o->imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            w->pImageInfo = o;
        } else {
            g_st.drop_bind++;
            UI_RED(RR_BIND, "pair %u (%s / %s) binding %u of kind %u type %d: no UI source: draw dropped", pair, s->key[VKN_SRC_VS], s->key[VKN_SRC_FS], b->binding, b->kind, (int)b->type);
            return 1;
        }
        nw++;
    }
    if (ndyn != s->ndyn) { g_st.drop_bind++; UI_RED(RR_BIND, "pair %u: %u dynamic offsets, the layout has %u: draw dropped", pair, ndyn, s->ndyn); return 1; }
    // pipeline
    pipe_k k; memset(&k, 0, sizeof k);
    k.pair = pair; k.vfmt = in->vfmt; k.blend = in->blend; k.ds = in->ds; k.color_off = in->color_off;
    if (k.ds & 1) { k.scmp = in->stencil_cmp; k.swr = in->stencil_write; }
    k.wmask = (uint8_t)pi.write_mask[0]; k.samples = (uint8_t)pi.samples; k.compat = pi.compat;
    if (!pi.has_depth && !pi.has_stencil) k.ds = 0;
    VkPipeline p = pipe_get(&k, s, &pi);
    if (!p) { g_st.drop_pipe++; return 1; }
    // vertices / indices
    const uint32_t stride = in->vfmt == VKN_UI_VF_V2 ? 8u : 16u;
    VkBuffer vbuf = VK_NULL_HANDLE, ibuf = VK_NULL_HANDLE; uint64_t voff = 0, ioff = 0; VkIndexType it = VK_INDEX_TYPE_UINT16;
    if (in->vb) {
        if (!in->vb->buf || in->vb_off + (uint64_t)in->nverts * stride > in->vb->size || (!in->quads && in->ib_off + 2ull * in->nidx > in->vb->size)) {
            g_st.drop_vb++; UI_RED(RR_VB, "vertex buffer block of %llu bytes: vertices at %llu x %u, indices at %llu x %u out of range: draw dropped", (unsigned long long)in->vb->size,
                                   (unsigned long long)in->vb_off, in->nverts, (unsigned long long)in->ib_off, in->nidx);
            return 1;
        }
        vbuf = in->vb->buf; voff = in->vb->off + in->vb_off; ibuf = in->vb->buf; ioff = in->vb->off + in->ib_off;
    } else {
        if (!in->verts || ring_put(in->verts, in->nverts * stride, 0, &vbuf, &voff, NULL)) { g_st.drop_ring++; UI_RED(RR_RING, "no ring space for %u vertices: draw dropped", in->nverts); return 1; }
        if (!in->quads && (!in->idx || ring_put(in->idx, in->nidx * 2u, 0, &ibuf, &ioff, NULL))) { g_st.drop_ring++; UI_RED(RR_RING, "no ring space for %u indices: draw dropped", in->nidx); return 1; }
    }
    if ((ioff & 1) && !in->quads) { g_st.drop_vb++; UI_RED(RR_VB, "odd index offset %llu: draw dropped", (unsigned long long)ioff); return 1; }
    VkDescriptorSet set = set_alloc(s->set_layout);
    if (!set) { g_st.drop_set++; UI_RED(RR_SET, "descriptor set allocation failed: draw dropped"); return 1; }
    for (uint32_t i = 0; i < nw; i++) wr[i].dstSet = set;
    vkUpdateDescriptorSets(vkn_device()->dev, nw, wr, 0, NULL);
    // record
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->layout, 0, 1, &set, ndyn, ndyn ? dyn : NULL);
    VkViewport vp = g_vp_set ? g_vp : (VkViewport){0.0f, 0.0f, (float)pi.width, (float)pi.height, 0.0f, 1.0f};
    if (!(vp.width > 0.0f) || !(vp.height > 0.0f)) vp = (VkViewport){0.0f, 0.0f, (float)pi.width, (float)pi.height, 0.0f, 1.0f};
    // flipped like vkn_draw.c's viewport: NVN's upper-left origin maps NDC y = +1 to row 0, Vulkan NDC y = -1 (the translator's
    // xbg_nvn_viewport does the same for these very shaders)
    vp.y += vp.height; vp.height = -vp.height;
    VkRect2D sc = g_sc_set ? g_sc : (VkRect2D){{0, 0}, {pi.width, pi.height}};
    if ((uint32_t)sc.offset.x >= pi.width || (uint32_t)sc.offset.y >= pi.height) { vkn_draw_raw_begin(); g_st.drop_empty++; return 0; }   // nothing inside the target
    if (sc.offset.x + sc.extent.width > pi.width) sc.extent.width = pi.width - (uint32_t)sc.offset.x;
    if (sc.offset.y + sc.extent.height > pi.height) sc.extent.height = pi.height - (uint32_t)sc.offset.y;
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    const VkBuffer bufs[2] = {vbuf, g_null_vb.buf}; const VkDeviceSize offs[2] = {voff, g_null_vb.off};
    vkCmdBindVertexBuffers(cb, 0, 1, &bufs[0], &offs[0]);
    vkCmdBindVertexBuffers(cb, VKN_VFMT_NULL_BINDING, 1, &bufs[1], &offs[1]);
    if (in->quads) {
        vkCmdBindIndexBuffer(cb, g_quad_ib.buf, g_quad_ib.off, VK_INDEX_TYPE_UINT32);
        for (uint32_t q0 = 0, nq = in->nverts / 4; q0 < nq; q0 += QUADS) {
            const uint32_t n = nq - q0 < QUADS ? nq - q0 : QUADS;
            vkCmdDrawIndexed(cb, n * 6, 1, 0, (int32_t)(q0 * 4), 0);
        }
        g_st.quads++;
    } else {
        vkCmdBindIndexBuffer(cb, ibuf, ioff, it);
        vkCmdDrawIndexed(cb, in->nidx, 1, 0, 0, 0);
        g_st.indexed++;
    }
    vkn_draw_raw_begin();   // the engine's next draw rebinds pipeline, set, buffers and dynamic state
    g_st.draws++; g_st.vertices += in->nverts;
    return 0;
}

void vkn_ui_clear_ds(int depth, uint8_t mask) {
    vkn_tex *t = vkn_pass_depth();   // NVN clears the bound depth target: the one of the current set (GDraw's or the engine's)
    if (g_ep.on && g_ep.detached && !t && up_mine()) t = g_ep.d;   // [stage 5.2 B2] detached: NVN still has GDraw's depth bound
    if (!t || t->dead) return;
    g_st.clears_ds++;
    const int full = mask == 0xff || !(t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    const int what = (depth ? 1 : 0) | (mask && full ? 2 : 0);
    if (g_ep.on && t == g_ep.d) {   // [stage 5.2 B2] a masked clear is a full-target stencil draw; a whole clear lands in an open pass with the DS (whole level)
        const int masked = mask && !full;
        const nb_ua_rect r = masked || (what && !g_ep.detached && up_open()) ? nb_ua_full(g_ep.w, g_ep.h) : nb_ua_none();
        up_event(r, masked, UP_KEY_DS, 0);
    }
    if (what) vkn_pass_clear_depth(t, what, 1.0f, 0);
    if (!mask || full) return;
    // a partial stencil mask: a full-target triangle that writes 0 into the masked bits (nothing else)
    VkCommandBuffer cb = vkn_cb();
    if (!cb || statics() || !g_clr_vs || !g_clr_layout) { g_st.drop_shader++; UI_RED(RR_SHADER, "masked stencil clear (mask %#x) without its module: dropped", mask); return; }
    vkn_pass_info pi;
    if (vkn_pass_ready(&pi)) { g_st.drop_pass++; return; }
    if (g_ep.on) up_opened();   // [stage 5.2 B2]
    pipe_k k; memset(&k, 0, sizeof k);
    k.pair = ~0u; k.clear = 1; k.swr = mask; k.samples = (uint8_t)pi.samples; k.compat = pi.compat;
    VkPipeline p = pipe_get(&k, NULL, &pi);
    if (!p) { g_st.drop_pipe++; return; }
    static const float tri[6] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
    VkBuffer vb; uint64_t vo;
    if (ring_put(tri, sizeof tri, 0, &vb, &vo, NULL)) { g_st.drop_ring++; return; }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    const VkViewport vp = {0.0f, 0.0f, (float)pi.width, (float)pi.height, 0.0f, 1.0f};
    const VkRect2D sc = {{0, 0}, {pi.width, pi.height}};
    vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindVertexBuffers(cb, 0, 1, &vb, &vo);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkn_draw_raw_begin();
    g_st.clears_masked++;
}

void vkn_ui_clear_color(vkn_tex *t, const int32_t rect[4]) {
    if (!t || t->dead) return;
    static const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int region = 0;
    if (g_ep.on && t == g_ep.c) {   // [stage 5.2 B2] the rect as vkn_pass_clear_color clips it; a whole clear is remembered or cleared in the pass
        nb_ua_rect r = nb_ua_full(t->w, t->h);
        if (rect) r = nb_ua_clip(up_rect(rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3]), r);
        const int whole = !nb_ua_empty(r) && r.x0 == 0 && r.y0 == 0 && r.x1 == (int32_t)t->w && r.y1 == (int32_t)t->h;
        region = !whole && !nb_ua_empty(r);
        if (!nb_ua_empty(r)) up_event(r, 0, UP_KEY_CLEAR, whole);
    }
    vkn_pass_clear_color(t, 0, zero, rect);
    if (region && g_ep.on) up_opened();   // (a region clear of a current target opens its pass)
    g_st.clears_color++;
}

void vkn_ui_stats_get(vkn_ui_stats *s) { if (s) *s = g_st; }
#endif
