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
#include "vkn_policy.h"        // [mgpu P0] the render-pass tilers (vkn_policy_tile_pass)
#include "../nr/nr_uix.h"     // [stage 5.2 B2] ordinal keys, DS decay (includes ../nb/nb_uiarea.h: rects, seed, history)
#include <pthread.h>
#include <stdio.h>
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
// [mgpu P2] the filter cache (block "mgpu P2" below)
static int fc_holding(void); static void fc_put_vs(uint8_t kind, int32_t x, int32_t y, int32_t w, int32_t h); static void fc_begin(vkn_tex *t, const int32_t rect[4]);
static void fc_put_draw(const vkn_ui_draw_in *in); static void fc_flush(int why); static void fc_end(void); static int fc_hold_clear(vkn_tex *t, const int32_t rect[4]);
static int fc_same_set(const vkn_tex *c, const vkn_tex *d);
static int ui_draw_x(const vkn_ui_draw_in *in); static void ui_clear_color_x(vkn_tex *t, const int32_t rect[4]);
static int g_up_noarea;   // [mgpu P7] the hold's hoisted layer replay: the layer's epoch takes no render-area decision (one pass)
static void ui_viewport_x(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (fc_holding()) { fc_put_vs(1, x, y, w, h); return; }   // [mgpu P2]
    g_vp = (VkViewport){(float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f}; g_vp_set = 1;
}
static void ui_scissor_x(int32_t x, int32_t y, int32_t w, int32_t h) {
    if (fc_holding()) { fc_put_vs(2, x, y, w, h); return; }   // [mgpu P2]
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
static struct { int req, verify, rule, sim_tiler, configured, resolved, mode, tile, rreq, region; } g_up;   // rreq / region: [mgpu P1] DM_VK_NATIVE_UIREGION
static vkn_ui_pass_stats g_ups;
static nb_ua_hist g_uph; static nr_uix_ord g_upo;
static uint64_t g_up_frame = ~0ull; static uint32_t g_up_fno; static uint8_t g_up_fds, g_up_fds_prev;   // presented frame; rule 1: DS used this / last frame
typedef struct up_ep {
    uint8_t on, decided, shared, dref, readied, fenced;
    uint8_t detached, began_det, reattached, used;        // applied DS state; a DS-needing event came
    uint8_t hint, area_seen, narrowed, restarted;         // applied area state
    uint8_t wdetach, wmiss, warea, wrestart;              // verify: would detach / would have re-attached / would narrow / would restart
    uint8_t rq, region, wregion;                          // [mgpu P1] the host named the region (rq, rrect); the region area is applied / verify
    int32_t rrect[4];
    vkn_tex *c, *d; uint32_t serial, w, h; uint64_t key2;
    nb_ua_rect uni, area;
} up_ep;
static up_ep g_ep;

static void up_fx(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user);
static void up_commit(void);
void vkn_ui_region_config(int mode) { g_up.rreq = mode < -1 || mode > 3 ? 0 : mode; g_up.resolved = 0; }   // [mgpu P1] (before vkn_ui_pass_config's first use)
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
    // [mgpu P0] the render-pass tilers: Mali / PowerVR, + Maleoon and Adreno on Qualcomm's own driver (vkn_policy_tile_pass); the SIM models
    { const int f = vkn_policy_family_from(d->props.vendorID, d->props.deviceName, getenv("DM_GFX_SIM_GPU"));
      g_up.tile = vkn_policy_tile_pass(f, vkn_drv()->driver_id) || g_up.sim_tiler; }
    g_up.mode = g_up.req < 0 ? (g_up.tile ? 2 : 0) : g_up.req;
    g_up.region = g_up.mode ? (g_up.rreq < 0 ? (g_up.tile ? 1 : 0) : g_up.rreq) : 0;   // [mgpu P1] (needs the epochs: UIPASS >= 1)
    g_ups.region_mode = g_up.region;
    vkn_logf("ui region (mgpu P1): DM_VK_NATIVE_UIREGION=%d (%s; %s)", g_up.region, g_up.rreq < 0 ? "family default" : "env",
             !g_up.mode ? "off: DM_VK_NATIVE_UIPASS is 0" : g_up.region == 0 ? "off" : g_up.region == 1 ? "an Iggy filter target's pass = its region, colour draws clipped to it" :
             g_up.region == 2 ? "verify: counted, drawn full area" : "poison: 1 + the target outside its region magenta first");
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
    if (g_ep.rq && g_up.region && what == UP_KEY_CLEAR && !full_clear) {   // [mgpu P1] the region = the area of the whole epoch
        nb_ua_rect s = nb_ua_clip(up_rect(g_ep.rrect[0], g_ep.rrect[1], g_ep.rrect[0] + g_ep.rrect[2], g_ep.rrect[1] + g_ep.rrect[3]), nb_ua_full(g_ep.w, g_ep.h));
        int why = -1;
        if (nb_ua_empty(s)) why = VKN_UIP_AR_SAVE;
        else if (!g_ups.api) why = VKN_UIP_AR_API;
        else if (g_ep.c->pass_nclr || (g_ep.d && !g_ep.detached && g_ep.d->pass_dclr)) why = VKN_UIP_AR_CLEAR;
        else if (g_ep.c->samples > 1) why = VKN_UIP_AR_MS;
        if (why >= 0) { g_ups.area_refused[why]++; g_ups.region_refused++; }
        else if (g_up.region == 2) { g_ep.area = s; g_ep.wregion = 1; g_ups.w_region++; }
        else {
            if (g_up.region == 3) {   // poison: the texels outside the region magenta (a read of them shows), before the hint
                static const float mg[4] = {1, 0, 1, 1};
                const int32_t W = (int32_t)g_ep.w, H = (int32_t)g_ep.h;
                const int32_t bx[4][4] = {{0, 0, W, s.y0}, {0, s.y1, W, H - s.y1}, {0, s.y0, s.x0, s.y1 - s.y0}, {s.x1, s.y0, W - s.x1, s.y1 - s.y0}};
                for (int k = 0; k < 4; k++) if (bx[k][2] > 0 && bx[k][3] > 0) vkn_pass_clear_color(g_ep.c, 0, mg, bx[k]);
                vkn_pass_end(); g_ups.region_poison++;
            }
            g_ep.area = s; const VkRect2D a = up_vk(s);
            if (!vkn_pass_area_hint(&a)) { g_ep.hint = 1; g_ep.region = 1; g_ups.area_hinted++; g_ups.region_epochs++; g_ups.region_px += nb_ua_px(s); }
        }
        return;
    }
    if (g_up.mode >= 2 && !g_up_noarea) {   // render area ([mgpu P7] not for the hoisted layer: its pass is one)
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
    // [mgpu P1] a region epoch: a colour-only draw outside the region is clipped to it (vkn_ui_draw: its scissor); a depth-stencil use
    // (the DS is shared with the other epochs: its texels outside the region matter) or a clear outside ends the region rule (below)
    if (g_ep.region && (needs_ds || what == UP_KEY_DS || what == UP_KEY_CLEAR) && !nb_ua_contains(g_ep.area, r)) { g_ep.region = 0; g_ups.region_dropped++; }
    if (g_ep.region && !needs_ds && what != UP_KEY_DS && what != UP_KEY_CLEAR) { if (!nb_ua_contains(g_ep.area, r)) g_ups.region_clipped++; }
    else if (g_ep.wregion && !needs_ds && what != UP_KEY_DS && what != UP_KEY_CLEAR) { if (!nb_ua_contains(g_ep.area, r)) g_ups.w_region_clip++; }
    else if (g_ep.wregion && !nb_ua_contains(g_ep.area, r)) { g_ep.wregion = 0; g_ups.w_region_drop++; }
    if (g_ep.hint && !g_ep.region && !nb_ua_contains(g_ep.area, r)) {   // outside the area: the narrowed pass ends, the rest of the epoch is full area (LOAD)
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
static void ui_fence_x(void) {
    if (fc_holding()) fc_flush(1);   // [mgpu P2] (FCF_FENCE)
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
    if (g_ep.on && (g_ep.detached || g_ep.hint)) { ui_fence_x(); g_ups.fence_draw++; }
}
void vkn_ui_pass_stats_get(vkn_ui_pass_stats *s) { if (s) { *s = g_ups; if (!g_up.resolved) s->mode = -1; } }
// [mgpu P1] the host: the epoch of t beginning now is a pushed Iggy target whose meaningful texels are rect (x, y, w, h: its cleared
// region; Iggy draws and reads nothing else of it until TextureDrawBufferEnd)
static void ui_region_x(vkn_tex *t, const int32_t rect[4]) {
    if (!rect || rect[2] <= 0 || rect[3] <= 0) return;
    if (g_up.region && g_ep.on && g_ep.c == t && !g_ep.decided) { g_ep.rq = 1; memcpy(g_ep.rrect, rect, sizeof g_ep.rrect); }
    fc_begin(t, rect);   // [mgpu P2]
}
// ==== [/stage 5.2 B2] ====


// ==== [mgpu P2] the Iggy filter cache across frames (DM_VK_NATIVE_UIFC; vkn_ui.h "filter cache") ====
// The unit is a pushed Iggy target's epoch the host names (vkn_ui_region: TextureDrawBufferBegin with a region) from its region clear
// to the next target change. Its UI calls are HELD (copied, not recorded) while a key is hashed over everything its result depends on:
// the target's format, the region, and per call the viewport / scissor state and per draw its programs, states, constant bytes, vertex /
// index bytes and, per texture unit, the texture's content version (CPU textures: vkn_tex.shc_up, taken at every data upload; GPU-written
// ones: the key of the cached epoch that last wrote them, dropped at any other GPU write - vkn_pass_on_write; anything else: no version,
// not cacheable). At the clean end (vkn_ui_targets of another set):
//   hit   (the same key stored, the slot of the same size): the region is copied from the cache atlas into the target (one transfer) and
//         the held viewport / scissor state applied; no clear, no draw, no pass for that target;
//   miss  the held calls run as recorded (vkn_ui_* again, in order), then the region is copied into an atlas slot under the key.
// Any other end (a fence, the engine touching vkn_pass - the vkn_pass_before hook -, a depth-stencil clear, more than FC_MAXD draws) runs
// the held calls as recorded at that point, nothing stored. Exact when the key is complete (same inputs, same pipeline: the same texels);
// texels outside the region are never read (the region contract, vkn_ui_region). Modes: 0 off; 1 on, every 128th hit drawn and compared
// with its slot (a difference: red line, the cache off for the session); 2 verify: every hit drawn as a miss and compared (differ must
// stay 0); 3 poison: 1 + one frame in 64 a hit clears the region magenta instead (the cached path is visible). Default (-1): 1.
enum { FC_N = 128, FC_AW = 2560, FC_AH = 1024, FC_MAXD = 48, FC_MAXB = 256 * 1024, FC_CT = 256, FC_CMP = 4 };
enum { FCR_NOREG, FCR_DRAW, FCR_TEX, FCR_MANY, FCR_DS, FCR_FULL, FCR_OTHER };
enum { FCF_TARGETS, FCF_FENCE, FCF_ENGINE, FCF_CLOSE, FCF_OTHER };
enum { FCE_VP = 1, FCE_SC, FCE_CLR, FCE_DRAW };
typedef struct fc_ent { uint64_t key; uint16_t x, y, w, h; uint32_t frame; uint8_t live; } fc_ent;
typedef struct fc_ct { const vkn_tex *t; VkImage img; uint64_t key; uint8_t valid; } fc_ct;
typedef struct fc_cmp { int live; uint64_t frame, key; vkn_block b; uint32_t bytes; } fc_cmp;
typedef struct fc_hdr { uint32_t size; uint8_t kind, pad[3]; } fc_hdr;
typedef struct fc_draw { fc_hdr h; vkn_ui_draw_in in; vkn_block vb; uint32_t o_vsc, o_fsc, o_fsc1, o_verts, o_idx; uint8_t has_vb, pad[3]; } fc_draw;
static struct {
    int req, mode, init; uint8_t hold, replay, bad, nreg;
    vkn_tex *t, *d; int32_t rect[4]; uint64_t key; uint32_t ndraws, nents, first;
    uint8_t *buf; uint32_t n, cap;
    fc_ent e[FC_N]; vkn_tex *atlas; uint32_t ax, ay, arow, afmt;
    fc_ct ct[FC_CT];
    fc_cmp cmp[FC_CMP];
    uint64_t hits_all;
} g_fc;
static uint8_t g_fca_cookie;
static uint64_t fc_mix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); h *= 0xff51afd7ed558ccdull; return h ^ (h >> 33); }
static uint64_t fc_bytes(uint64_t h, const void *p, uint32_t n) {
    const uint8_t *b = (const uint8_t *)p; uint64_t w;
    h = fc_mix(h, n);
    for (; n >= 8; n -= 8, b += 8) { memcpy(&w, b, 8); h = fc_mix(h, w); }
    if (n) { w = 0; memcpy(&w, b, n); h = fc_mix(h, w); }
    return h;
}
void vkn_ui_fc_config(int mode) { g_fc.req = mode < -1 || mode > 3 ? 0 : mode; g_fc.init = 0; }
static void lh_written(vkn_tex *t);   // [mgpu P7]
static void fc_written(vkn_tex *t) {   // vkn_pass_on_write: a GPU write drops the content key of t
    if (!g_fc.mode) return;
    const uint32_t h = (uint32_t)(((uintptr_t)t >> 4) * 0x9E3779B1u) & (FC_CT - 1);
    for (uint32_t i = 0; i < 8; i++) { fc_ct *c = &g_fc.ct[(h + i) & (FC_CT - 1)]; if (c->t == t) { c->valid = 0; return; } }
}
static void ui_on_write(vkn_tex *t) { fc_written(t); lh_written(t); }   // [mgpu P7] vkn_pass_on_write
static fc_ct *fc_ct_get(const vkn_tex *t, int create) {
    const uint32_t h = (uint32_t)(((uintptr_t)t >> 4) * 0x9E3779B1u) & (FC_CT - 1);
    fc_ct *fr = NULL;
    for (uint32_t i = 0; i < 8; i++) { fc_ct *c = &g_fc.ct[(h + i) & (FC_CT - 1)]; if (c->t == t) return c; if (!fr && (!c->t || !c->valid)) fr = c; }
    if (!create) return NULL;
    if (!fr) fr = &g_fc.ct[h];
    memset(fr, 0, sizeof *fr); fr->t = t;
    return fr;
}
static int fc_on(void) {
    if (!g_fc.init) {
        const vkn_dev *d = vkn_device(); if (!d) return 0;
        g_fc.init = 1; g_fc.mode = g_fc.req < 0 ? 1 : g_fc.req; g_ups.fc_mode = g_fc.mode;
        vkn_pass_on_write(ui_on_write);   // [mgpu P7] (one listener for both pieces)
        vkn_logf("ui filter cache (mgpu P2): DM_VK_NATIVE_UIFC=%d (%s; %s); atlas %ux%u, %u entries, at most %u draws an epoch", g_fc.mode, g_fc.req < 0 ? "default" : "env",
                 g_fc.mode == 0 ? "off" : g_fc.mode == 1 ? "on, every 128th hit drawn and compared" : g_fc.mode == 2 ? "verify: every hit drawn and compared" : "poison: 1 + a hit magenta one frame in 64",
                 FC_AW, FC_AH, FC_N, FC_MAXD);
    }
    return g_fc.mode;
}
// the version of a texture unit's content (0 = ok, -1 = unknown)
static int fc_tex_ver(const vkn_tex *t, uint64_t *v) {
    if (!t || t->dead || !t->view || t == g_fc.t || t == g_fc.d) { *v = 0x51ace; return 0; }   // (the placeholder at draw time: unknown / feedback)
    if (!t->pass_ever) { *v = fc_mix((uint64_t)(uintptr_t)t, t->shc_up); return t->shc_up ? 0 : -1; }
    const fc_ct *c = fc_ct_get(t, 0);
    if (c && c->valid && c->img == t->image) { *v = fc_mix(0xc0ffee, c->key); return 0; }
    return -1;
}
static void *fc_put(uint8_t kind, uint32_t bytes) {
    bytes = (bytes + 15u) & ~15u;
    if (g_fc.n + bytes > g_fc.cap) {
        uint32_t c = g_fc.cap ? g_fc.cap : 16384; while (c < g_fc.n + bytes) c *= 2;
        uint8_t *nb = (uint8_t *)realloc(g_fc.buf, c); if (!nb) return NULL;
        g_fc.buf = nb; g_fc.cap = c;
    }
    fc_hdr *h = (fc_hdr *)(void *)(g_fc.buf + g_fc.n); memset(h, 0, bytes); h->size = bytes; h->kind = kind;
    g_fc.n += bytes; g_fc.nents++;
    return h;
}
static void fc_replay(void);
static void fc_stop(void) { g_fc.hold = 0; g_fc.n = 0; g_fc.nents = 0; g_fc.ndraws = 0; vkn_pass_before(NULL); }
// held calls run as recorded (an end that is not the clean one, or an overflow): nothing stored
static void fc_flush(int why) {
    if (!g_fc.hold) return;
    if (why >= 0 && why <= FCF_OTHER) g_ups.fc_flush[why]++;
    vkn_pass_before(NULL); g_fc.hold = 0;   // (before the replay: its own vkn_pass calls must not flush again)
    fc_replay(); fc_stop();
}
static void fc_before(void) { fc_flush(FCF_ENGINE); }   // (vkn_pass_before: the engine or the recording end touches vkn_pass)
static int fc_holding(void) { return g_fc.hold && !g_fc.replay; }
// vkn_ui_region: a pushed target whose region is named; the epoch's calls are held from now on
static void fc_begin(vkn_tex *t, const int32_t rect[4]) {
    if (g_fc.hold) fc_flush(FCF_OTHER);
    if (!fc_on() || !t || t != g_color || t->dead || t->samples > 1 || rect[2] <= 0 || rect[3] <= 0) return;
    g_ups.fc_epochs++;
    // [mgpu P7] wide strips too (W07: 1343x55 and 2400x157 regions were "other", 5 a frame): up to the atlas width when at most a quarter high
    if (!((rect[2] <= FC_AW / 2 && rect[3] <= FC_AH / 2) || (rect[2] <= FC_AW && rect[3] <= FC_AH / 4))) { g_ups.fc_refused[FCR_OTHER]++; return; }
    g_fc.hold = 1; g_fc.bad = 0; g_fc.t = t; g_fc.d = g_depth; memcpy(g_fc.rect, rect, sizeof g_fc.rect);
    g_fc.n = 0; g_fc.nents = 0; g_fc.ndraws = 0; g_fc.first = 1;
    g_fc.key = fc_mix(fc_mix(fc_mix(0xf11ce, (uint64_t)t->format), (uint64_t)(uint32_t)rect[0] << 32 | (uint32_t)rect[1]), (uint64_t)(uint32_t)rect[2] << 32 | (uint32_t)rect[3]);
    vkn_pass_before(fc_before);
}
static void fc_put_vs(uint8_t kind, int32_t x, int32_t y, int32_t w, int32_t h) {
    int32_t *p = (int32_t *)fc_put(kind, sizeof(fc_hdr) + 16);
    if (!p) { fc_flush(FCF_OTHER); return; }
    p += sizeof(fc_hdr) / 4; p[0] = x; p[1] = y; p[2] = w; p[3] = h;
    g_fc.key = fc_bytes(fc_mix(g_fc.key, kind), p, 16);
    g_fc.first = 0;
}
static uint32_t fc_copy(fc_draw **dp, const void *src, uint32_t n) {   // bytes appended to the current entry: offset from the entry (0 = none)
    if (!src || !n) return 0;
    const uint32_t at = (uint32_t)((uint8_t *)*dp - g_fc.buf), off = g_fc.n - at, pad = (n + 15u) & ~15u;
    if (g_fc.n + pad > g_fc.cap) {
        uint32_t c = g_fc.cap; while (c < g_fc.n + pad) c *= 2;
        uint8_t *nb = (uint8_t *)realloc(g_fc.buf, c); if (!nb) return ~0u;
        g_fc.buf = nb; g_fc.cap = c; *dp = (fc_draw *)(void *)(g_fc.buf + at);
    }
    memcpy(g_fc.buf + g_fc.n, src, n); g_fc.n += pad; (*dp)->h.size += pad;
    return off;
}
static void fc_put_draw(const vkn_ui_draw_in *in) {
    if (g_fc.ndraws >= FC_MAXD || g_fc.n > FC_MAXB) { g_ups.fc_refused[FCR_MANY]++; fc_flush(FCF_OTHER); ui_draw_x(in); return; }
    fc_draw *d = (fc_draw *)fc_put(FCE_DRAW, sizeof *d);
    if (!d) { fc_flush(FCF_OTHER); ui_draw_x(in); return; }
    d->in = *in;
    const uint32_t stride = in->vfmt == VKN_UI_VF_V2 ? 8u : 16u;
    if (in->vb) { d->vb = *in->vb; d->has_vb = 1; }
    uint32_t o;
    if ((o = fc_copy(&d, in->vsc, in->vsc_bytes)) == ~0u) goto oom; d->o_vsc = o;
    if ((o = fc_copy(&d, in->fsc, in->fsc_bytes)) == ~0u) goto oom; d->o_fsc = o;
    if ((o = fc_copy(&d, in->fsc1, in->fsc1 ? in->fsc1_bytes : 0)) == ~0u) goto oom; d->o_fsc1 = o;
    if (!in->vb) {
        if ((o = fc_copy(&d, in->verts, in->nverts * stride)) == ~0u) goto oom; d->o_verts = o;
        if ((o = fc_copy(&d, in->idx, in->quads ? 0 : in->nidx * 2u)) == ~0u) goto oom; d->o_idx = o;
    }
    g_fc.ndraws++;
    if (g_fc.first) g_fc.bad |= 1;   // (the first call must be the region clear)
    g_fc.first = 0;
    if (g_fc.bad) return;
    {   // the key
        uint64_t k = fc_mix(g_fc.key, FCE_DRAW);
        k = fc_mix(fc_mix(k, in->vs), in->fs);
        k = fc_mix(k, (uint64_t)in->vfmt | (uint64_t)in->quads << 8 | (uint64_t)in->blend << 16 | (uint64_t)in->ds << 24 | (uint64_t)in->color_off << 32 |
                      (uint64_t)in->stencil_cmp << 40 | (uint64_t)in->stencil_write << 48);
        if (in->ds) { g_fc.bad |= 2; return; }   // a depth / stencil test or write: the DS is shared by every epoch
        for (int u = 0; u < VKN_UI_UNITS; u++) { uint64_t v; if (fc_tex_ver(in->tex[u], &v)) { g_fc.bad |= 4; return; } k = fc_mix(k, v); }
        k = fc_bytes(k, in->vsc, in->vsc_bytes); k = fc_bytes(k, in->fsc, in->fsc_bytes); k = fc_bytes(k, in->fsc1, in->fsc1 ? in->fsc1_bytes : 0);
        if (in->vb) {
            if (!in->vb->cpu || in->vb_off + (uint64_t)in->nverts * stride > in->vb->size || (!in->quads && in->ib_off + 2ull * in->nidx > in->vb->size)) { g_fc.bad |= 2; return; }
            k = fc_bytes(k, (const uint8_t *)in->vb->cpu + in->vb_off, in->nverts * stride);
            if (!in->quads) k = fc_bytes(k, (const uint8_t *)in->vb->cpu + in->ib_off, in->nidx * 2u);
        } else { k = fc_bytes(k, in->verts, in->nverts * stride); if (!in->quads) k = fc_bytes(k, in->idx, in->nidx * 2u); }
        k = fc_mix(k, (uint64_t)in->nverts << 32 | in->nidx);
        g_fc.key = k;
    }
    return;
oom:
    g_fc.n = (uint32_t)((uint8_t *)d - g_fc.buf); g_fc.nents--;
    fc_flush(FCF_OTHER); ui_draw_x(in);
}
static void fc_replay(void) {
    g_fc.replay = 1;
    for (uint32_t at = 0; at < g_fc.n;) {
        fc_hdr *h = (fc_hdr *)(void *)(g_fc.buf + at);
        const int32_t *p = (const int32_t *)(const void *)(h + 1);
        switch (h->kind) {
        case FCE_VP: ui_viewport_x(p[0], p[1], p[2], p[3]); break;
        case FCE_SC: ui_scissor_x(p[0], p[1], p[2], p[3]); break;
        case FCE_CLR: ui_clear_color_x(g_fc.t, p); break;
        case FCE_DRAW: {
            fc_draw *d = (fc_draw *)(void *)h; vkn_ui_draw_in in = d->in; uint8_t *b = (uint8_t *)d;
            in.vsc = d->o_vsc ? b + d->o_vsc : NULL; in.fsc = d->o_fsc ? b + d->o_fsc : NULL; in.fsc1 = d->o_fsc1 ? b + d->o_fsc1 : NULL;
            if (d->has_vb) in.vb = &d->vb; else { in.verts = d->o_verts ? b + d->o_verts : NULL; in.idx = d->o_idx ? (const uint16_t *)(const void *)(b + d->o_idx) : NULL; }
            ui_draw_x(&in);
            break; }
        default: break;
        }
        at += h->size;
    }
    g_fc.replay = 0;
}
// the state calls only (a hit): viewport / scissor as the epoch left them
static void fc_state_only(void) {
    g_fc.replay = 1;
    for (uint32_t at = 0; at < g_fc.n;) {
        fc_hdr *h = (fc_hdr *)(void *)(g_fc.buf + at); const int32_t *p = (const int32_t *)(const void *)(h + 1);
        if (h->kind == FCE_VP) ui_viewport_x(p[0], p[1], p[2], p[3]); else if (h->kind == FCE_SC) ui_scissor_x(p[0], p[1], p[2], p[3]);
        at += h->size;
    }
    g_fc.replay = 0;
}
static int fc_atlas(const vkn_tex *t) {   // the atlas in t's format (one format: Iggy's RGBA8)
    if (g_fc.atlas) return g_fc.atlas->dead || g_fc.afmt != t->nvn_fmt ? -1 : 0;
    vkn_tex_desc d; memset(&d, 0, sizeof d);
    d.nvn_fmt = t->nvn_fmt; d.efmt = t->efmt; d.nvn_target = 1; d.w = FC_AW; d.h = FC_AH; d.depth = d.mips = d.layers = d.samples = 1; d.cls = VKN_TC_COLOR;
    d.pool_id = ~0u; for (int i = 0; i < VKN_TEX_LAYER_IDS; i++) d.layer_ids[i] = ~0u; d.sampler_id = ~0u; d.cookie = (uint64_t)(uintptr_t)&g_fca_cookie;
    const vkn_tex_src src = {.kind = VKN_SRC_NONE};
    const char *why = NULL;
    g_fc.atlas = vkn_tex_create(1ull << 32 | 0xffffffffull, &d, &src, &why);
    if (!g_fc.atlas || g_fc.atlas->format != t->format) { UI_RED(RR_STATIC, "filter cache: no atlas (%s): off", why ? why : "format"); g_fc.mode = 0; g_ups.fc_mode = 0; return -1; }
    snprintf(g_fc.atlas->sname, sizeof g_fc.atlas->sname, "ui-filter-cache"); g_fc.atlas->ui_tgt = 1;   // [mgpu P7]
    g_fc.afmt = t->nvn_fmt;
    return 0;
}
static fc_ent *fc_find(uint64_t key) { for (uint32_t i = 0; i < FC_N; i++) if (g_fc.e[i].live && g_fc.e[i].key == key) return &g_fc.e[i]; return NULL; }
static fc_ent *fc_alloc(uint64_t key, uint32_t w, uint32_t h) {
    fc_ent *e = NULL;
    for (uint32_t i = 0; i < FC_N && !e; i++) if (!g_fc.e[i].live) e = &g_fc.e[i];
    if (!e) { e = &g_fc.e[0]; for (uint32_t i = 1; i < FC_N; i++) if (g_fc.e[i].frame < e->frame) e = &g_fc.e[i]; }
    if (e->live) { e->live = 0; g_ups.fc_evicted++; }
    if (g_fc.ax + w > FC_AW) { g_fc.ax = 0; g_fc.ay += g_fc.arow; g_fc.arow = 0; }
    if (g_fc.ay + h > FC_AH) {   // the atlas is full: every entry goes (the shelves start over)
        for (uint32_t i = 0; i < FC_N; i++) if (g_fc.e[i].live) { g_fc.e[i].live = 0; g_ups.fc_evicted++; }
        g_fc.ax = g_fc.ay = g_fc.arow = 0;
    }
    e->live = 1; e->key = key; e->x = (uint16_t)g_fc.ax; e->y = (uint16_t)g_fc.ay; e->w = (uint16_t)w; e->h = (uint16_t)h; e->frame = (uint32_t)vkn_frame();
    g_fc.ax += (w + 7u) & ~7u; if (((h + 7u) & ~7u) > g_fc.arow) g_fc.arow = (h + 7u) & ~7u;
    return e;
}
// a compare of the target's region with an entry's slot: both read back now, checked 4 frames later (fc_cmp_check)
static void fc_cmp_put(const fc_ent *e) {
    fc_cmp *c = NULL; for (int i = 0; i < FC_CMP && !c; i++) if (!g_fc.cmp[i].live) c = &g_fc.cmp[i];
    VkCommandBuffer cb = vkn_cb();
    if (!c || !cb) return;
    const uint32_t bytes = (uint32_t)e->w * e->h * 4u;
    if (vkn_block_alloc(VKN_ARENA_STAGING, 2ull * bytes, 16, &c->b)) return;
    vkn_pass_layout(g_fc.t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); vkn_pass_layout(g_fc.atlas, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r = {.bufferOffset = c->b.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageOffset = {g_fc.rect[0], g_fc.rect[1], 0}, .imageExtent = {e->w, e->h, 1}};
    vkCmdCopyImageToBuffer(cb, g_fc.t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->b.buf, 1, &r);
    r.bufferOffset = c->b.off + bytes; r.imageOffset.x = e->x; r.imageOffset.y = e->y;
    vkCmdCopyImageToBuffer(cb, g_fc.atlas->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->b.buf, 1, &r);
    c->live = 1; c->frame = vkn_frame(); c->key = e->key; c->bytes = bytes; g_ups.fc_verify++;
}
static void fc_cmp_check(int all) {
    for (int i = 0; i < FC_CMP; i++) {
        fc_cmp *c = &g_fc.cmp[i];
        if (!c->live || (!all && vkn_frame() < c->frame + 4)) continue;
        const int differ = memcmp(c->b.cpu, (const uint8_t *)c->b.cpu + c->bytes, c->bytes) != 0;
        if (differ) {
            g_ups.fc_differ++;
            UI_RED(RR_STATIC, "filter cache: a cached region (key %016llx, %u bytes) differs from the drawn one%s", (unsigned long long)c->key, c->bytes,
                   g_fc.mode == 1 ? ": the cache is off for this session" : "");
            if (g_fc.mode == 1 || g_fc.mode == 3) { g_fc.mode = 0; g_ups.fc_mode = 0; }
            for (uint32_t k = 0; k < FC_N; k++) if (g_fc.e[k].live && g_fc.e[k].key == c->key) g_fc.e[k].live = 0;
        }
        vkn_block_free(&c->b); c->live = 0;
    }
}
// the clean end (vkn_ui_targets of another set): hit or miss
static void fc_end(void) {
    if (!g_fc.hold) return;
    vkn_pass_before(NULL);
    g_ups.fc_flush[FCF_TARGETS]++;
    fc_cmp_check(0);
    const uint32_t w = (uint32_t)g_fc.rect[2], h = (uint32_t)g_fc.rect[3];
    int why = -1;
    if (g_fc.bad & 1 || !g_fc.ndraws) why = FCR_NOREG;
    else if (g_fc.bad & 2) why = FCR_DRAW;
    else if (g_fc.bad & 4) why = FCR_TEX;
    else if (g_fc.t->dead || (uint64_t)g_fc.rect[0] + w > g_fc.t->w || (uint64_t)g_fc.rect[1] + h > g_fc.t->h || fc_atlas(g_fc.t)) why = FCR_FULL;
    const uint64_t key = g_fc.key;
    fc_ent *e = why < 0 ? fc_find(key) : NULL;
    if (e && (e->w != w || e->h != h)) { e->live = 0; e = NULL; }
    const int check = e && (g_fc.mode == 2 || (g_fc.mode == 1 && (++g_fc.hits_all & 127) == 0));
    vkn_tex *t = g_fc.t;
    if (e && !check) {
        g_ups.fc_hits++; g_ups.fc_draws_saved += g_fc.ndraws; g_ups.fc_px_copied += (uint64_t)w * h; e->frame = (uint32_t)vkn_frame();
        fc_state_only();
        if (g_fc.mode == 3 && (vkn_frame() & 63) == 0) {
            static const float mg[4] = {1, 0, 1, 1}; vkn_pass_clear_color(t, 0, mg, g_fc.rect); g_ups.fc_poison++;
            fc_ct *c = fc_ct_get(t, 1); if (c) c->valid = 0;
        } else if (!vkn_pass_copy_rect(t, g_fc.rect[0], g_fc.rect[1], g_fc.atlas, e->x, e->y, w, h)) {
            fc_ct *c = fc_ct_get(t, 1); if (c) { c->img = t->image; c->key = key; c->valid = 1; }
        } else { e->live = 0; fc_replay(); }   // (the copy failed: drawn as recorded)
        g_fc.n = 0; g_fc.nents = 0; g_fc.ndraws = 0; g_fc.hold = 0;
        return;
    }
    if (why >= 0) g_ups.fc_refused[why]++;
    fc_replay();
    if (why < 0 && check) {   // drawn; the drawn region is compared with the stored slot (the slot stays)
        fc_cmp_put(e);
        fc_ct *c = fc_ct_get(t, 1); if (c) { c->img = t->image; c->key = key; c->valid = 1; }
    } else if (why < 0) {
        g_ups.fc_misses++;
        e = fc_alloc(key, w, h);
        if (!vkn_pass_copy_rect(g_fc.atlas, e->x, e->y, t, g_fc.rect[0], g_fc.rect[1], w, h)) {
            g_ups.fc_stored++;
            fc_ct *c = fc_ct_get(t, 1); if (c) { c->img = t->image; c->key = key; c->valid = 1; }
        } else e->live = 0;
    }
    g_fc.n = 0; g_fc.nents = 0; g_fc.ndraws = 0; g_fc.hold = 0;
}
// ==== [/mgpu P2]
static int fc_same_set(const vkn_tex *c, const vkn_tex *d) { return c == g_fc.t && d == g_fc.d; }
static int fc_hold_clear(vkn_tex *t, const int32_t rect[4]) {   // the region clear as the epoch's first call: held (1)
    if (!g_fc.first || t != g_fc.t || !rect || memcmp(rect, g_fc.rect, sizeof g_fc.rect)) return 0;
    int32_t *p = (int32_t *)fc_put(FCE_CLR, sizeof(fc_hdr) + 16);
    if (!p) return 0;
    p += sizeof(fc_hdr) / 4; memcpy(p, rect, 16);
    g_fc.key = fc_bytes(fc_mix(g_fc.key, FCE_CLR), p, 16); g_fc.first = 0;
    return 1;
}

static void ui_targets_x(vkn_tex *color, vkn_tex *depth) {
    if (fc_holding()) { if (fc_same_set(color, depth)) { g_st.targets++; return; } fc_end(); }   // [mgpu P2] the clean end of a held epoch
    int shared = 0;
    if (up_on() && up_targets(color, depth, &shared)) { g_st.targets++; return; }   // [stage 5.2 B2]
    vkn_tex *c[VKN_PASS_COLORS] = {color};
    const uint32_t zero[VKN_PASS_COLORS] = {0};
    vkn_pass_targets(1, c, zero, zero, depth);
    g_color = color; g_depth = depth;
    if (color) color->ui_tgt = 1;
    if (depth) depth->ui_tgt = 1;   // [mgpu P7] (the journal's UI share)
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
// ---- [rt P6] DM_VK_NATIVE_UISET (vkn_ui.h): sets of this recording by content. A UI draw's uniform blocks are fresh copies in the
// recording ring at new offsets, so a set holding those offsets is never the same twice; with the cache on, a DYNAMIC uniform binding
// gets (ring buffer, ring base) in the set and the copy's offset from the base as its dynamic offset - the bytes read are the same (the
// descriptor's offset + the dynamic offset), the set content now repeats: (layout, per binding buffer / offset / range or view /
// sampler). Sets live in the pools of the recording (retired with them at the next recording), so the cache empties at the same point.
// Mode 2: the same dynamic offsets, the lookup made and counted (would hit), every draw a fresh set (the reuse alone is what 1 adds).
enum { USC = 512, USW = 2 + 3 * 32 };
static struct { uint64_t h; uint32_t nw; VkDescriptorSet set; uint64_t w[USW]; } g_usc[USC];
static uint32_t g_usc_serial; static int g_usc_valid, g_uiset; static uint64_t g_usc_hits, g_usc_miss, g_usc_would;
void vkn_ui_set_cache(int mode) { g_uiset = mode == 1 || mode == 2 ? mode : 0; g_usc_valid = 0; }
void vkn_ui_set_cache_stats(uint64_t *hits, uint64_t *miss, uint64_t *would) { if (hits) *hits = g_usc_hits; if (miss) *miss = g_usc_miss; if (would) *would = g_usc_would; }
static uint64_t usc_hash(const uint64_t *w, uint32_t n) { uint64_t h = 0x9e3779b97f4a7c15ull ^ n; for (uint32_t i = 0; i < n; i++) { h ^= w[i]; h *= 0xff51afd7ed558ccdull; h ^= h >> 32; } return h; }
static void usc_check_serial(void) {   // a new recording: its sets are new (the pools of the last one are being retired)
    if (!g_usc_valid || g_usc_serial != vkn_cb_serial()) { memset(g_usc, 0, sizeof g_usc); g_usc_serial = vkn_cb_serial(); g_usc_valid = 1; }
}
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

static int ui_draw_x(const vkn_ui_draw_in *in) {
    if (fc_holding()) { fc_put_draw(in); return 0; }   // [mgpu P2] held (drawn at the epoch's end, or replaced by the cached region)
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
                const float sw = vw > 0 ? vw : 1.0f, sh = vh > 0 ? vh : 1.0f;
                if (!vkn_shader_support(sw, sh, &sb)) { o->buffer = sb.buf; o->offset = sb.off; o->range = sb.size; }
                else {   // [DRS 21.11] no block for this size: a frame-local copy (vkn_shader.h vkn_shader_support_fill)
                    uint8_t tmp[1392];
                    if (vkn_shader_support_fill(sw, sh, tmp, sizeof tmp) || ring_put(tmp, sizeof tmp, 0, &o->buffer, &o->offset, &o->range)) {
                        g_st.drop_bind++; UI_RED(RR_BIND, "no SUPPORT block: draw dropped"); return 1; }
                }
            } else {
                const vkn_block *c1 = &s->c1[b->stage == VKN_STAGE_VS ? VKN_SRC_VS : VKN_SRC_FS];
                if (!c1->size) { g_st.drop_bind++; UI_RED(RR_BIND, "pair %u: C1 binding without data: draw dropped", pair); return 1; }
                o->buffer = c1->buf; o->offset = c1->off; o->range = c1->size;
            }
            if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
                uint32_t dv = 0;
                if (g_uiset && b->kind == VKN_BK_UNIFORM) { const uint64_t base = vkn_ring2_base(); dv = (uint32_t)(o->offset - base); o->offset = base; }   // [rt P6]
                if (ndyn < 32) dyn[ndyn++] = dv;
            }
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
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint64_t kw[USW]; uint32_t nk = 0; uint32_t ci = 0; uint64_t kh = 0;
    if (g_uiset) {   // [rt P6] the set's content as a key
        usc_check_serial();
        kw[nk++] = (uint64_t)(uintptr_t)s->set_layout; kw[nk++] = nw;
        for (uint32_t i = 0; i < nw && nk + 3 <= USW; i++) {
            if (wr[i].pBufferInfo) { kw[nk++] = (uint64_t)(uintptr_t)wr[i].pBufferInfo->buffer | (uint64_t)wr[i].dstBinding << 56; kw[nk++] = wr[i].pBufferInfo->offset; kw[nk++] = wr[i].pBufferInfo->range; }
            else { kw[nk++] = (uint64_t)(uintptr_t)wr[i].pImageInfo->imageView | (uint64_t)wr[i].dstBinding << 56; kw[nk++] = (uint64_t)(uintptr_t)wr[i].pImageInfo->sampler; kw[nk++] = wr[i].pImageInfo->imageLayout; }
        }
        kh = usc_hash(kw, nk); ci = (uint32_t)(kh >> 40) & (USC - 1);
        if (g_usc[ci].set && g_usc[ci].h == kh && g_usc[ci].nw == nk && !memcmp(g_usc[ci].w, kw, nk * sizeof kw[0])) {
            if (g_uiset == 1) { set = g_usc[ci].set; g_usc_hits++; } else g_usc_would++;
        } else g_usc_miss++;
    }
    if (!set) {
        set = set_alloc(s->set_layout);
        if (!set) { g_st.drop_set++; UI_RED(RR_SET, "descriptor set allocation failed: draw dropped"); return 1; }
        for (uint32_t i = 0; i < nw; i++) wr[i].dstSet = set;
        vkUpdateDescriptorSets(vkn_device()->dev, nw, wr, 0, NULL);
        if (g_uiset == 1 || (g_uiset == 2 && !g_usc[ci].set)) { g_usc[ci].set = set; g_usc[ci].h = kh; g_usc[ci].nw = nk; memcpy(g_usc[ci].w, kw, nk * sizeof kw[0]); }
    }
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
    if (g_ep.on && g_ep.region) {   // [mgpu P1] a region epoch's narrowed pass: the draw stays inside its render area (exact: the texels outside are never read)
        VkRect2D a;
        if (vkn_pass_area_get(&a) && up_narrow(&a)) {
            int64_t x0 = sc.offset.x > a.offset.x ? sc.offset.x : a.offset.x, y0 = sc.offset.y > a.offset.y ? sc.offset.y : a.offset.y;
            int64_t x1 = (int64_t)sc.offset.x + sc.extent.width, y1 = (int64_t)sc.offset.y + sc.extent.height;
            if (x1 > (int64_t)a.offset.x + a.extent.width) x1 = (int64_t)a.offset.x + a.extent.width;
            if (y1 > (int64_t)a.offset.y + a.extent.height) y1 = (int64_t)a.offset.y + a.extent.height;
            if (x1 <= x0 || y1 <= y0) { vkn_draw_raw_begin(); g_st.drop_empty++; return 0; }   // (nothing of it inside the region)
            sc.offset.x = (int32_t)x0; sc.offset.y = (int32_t)y0; sc.extent.width = (uint32_t)(x1 - x0); sc.extent.height = (uint32_t)(y1 - y0);
        }
    }
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

static void ui_clear_ds_x(int depth, uint8_t mask) {
    if (fc_holding()) { g_ups.fc_refused[4]++; fc_flush(4); }   // [mgpu P2] a depth-stencil use: drawn as recorded (FCR_DS, FCF_OTHER)
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

static void ui_clear_color_x(vkn_tex *t, const int32_t rect[4]) {
    if (fc_holding()) { if (fc_hold_clear(t, rect)) return; fc_flush(4); }   // [mgpu P2]
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
    // (a region clear of a current target opens its pass; [frame F1] not under DM_VK_NATIVE_LAZYCLR: the clear waits for the pass of the next
    // draw, whose up_opened then sees the area it really began with)
    if (region && g_ep.on && (!vkn_pass_area_get || vkn_pass_area_get(NULL))) up_opened();
    g_st.clears_color++;
}

void vkn_ui_stats_get(vkn_ui_stats *s) { if (s) *s = g_st; }
// ==== [mgpu P7] the UI layer hold: the layer in one pass, the whole-UI cache (DM_VK_NATIVE_UILAYER / DM_VK_NATIVE_UICACHE; vkn_ui.h) ====
// Every GDraw call of the backend (the vkn_ui_* entry points below) is HELD - copied into a buffer in order, not executed - from the
// first one of a run until something outside the UI needs what it would have drawn (vkn_pass_before_ev: an engine use that conflicts,
// below). Then the run is FLUSHED: executed through the stage-3 / P1 / P2 code as it was (the *_x functions), or skipped (the cache).
//   groups   an entry belongs to group 0 when the colour target current in the run is the layer L (the base of GDraw's stack, named by
//            the host at RenderingBegin: vkn_ui_layer), else to group 1 (a pushed target: Iggy's filter targets);
//   hoisted  (UILAYER 1) group 1 is executed first, then group 0: every filter block before the layer, so the layer is ONE pass
//            instead of one per filter (W07: 15 layer passes of 10-40 MB on the tile model). Exact when (checked while holding, else the
//            run is executed in its own order, counted by reason): no group-1 depth-stencil use (the DS is shared), no group-1 draw samples
//            the layer or the depth target, no pushed target is written after a group-0 draw sampled it (a reuse). A pushed target is
//            read only by the layer's later draws (its composite) and by later group-1 steps (blur ping-pong): both keep their order.
//            State calls (targets / viewport / scissor) are applied lazily per group: each executed entry sees the state it saw in the
//            run. Fences (RenderingEnd, custom-draw begin) only order engine work after them, which comes after the flush: dropped,
//            one at the end of the run when it had any. The layer's epoch takes no render-area decision (g_up_noarea: one pass).
//   conflict while holding (the engine's event is executed now, BEFORE the held run): the engine's own TARGETS never conflicts (it
//            marks that the bound set is the engine's again); READY / DEPTH (an engine draw / a bound-depth clear) conflict when the
//            set bound is GDraw's (the run's last binding) or the current set holds a texture the run writes or reads; SAMPLE of a
//            texture the run writes; CLEAR / STORAGE / LAYOUT of one it writes or reads; COPY into one it writes or reads, or out of
//            one it writes; END (a piece's pass end, the recording's close) always. A flush puts back the engine's set when the engine
//            had bound one after the run's last binding (the UI's set stays current otherwise, as NVN has it).
//   cache    (UICACHE 1) a key over the first run of a recording that has content: per entry its kind and values, per draw programs,
//            states, constant / vertex / index bytes, per texture unit the content version (a target of the run: the running hash of
//            what the run wrote into it; else vkn_tex.shc_up / the P2 key; a GPU-written texture outside the run has none: no key),
//            the targets by their order of appearance, and the value of the layer's remembered whole clear (the engine's frame clear,
//            vkn_pass_clear_take). The run is SKIPPED when the last recording's first run had the same key and value and no texture it
//            wrote (layer, depth, filter targets) was written since its flush (vkn_pass_on_write, our own later runs included): their
//            content is then exactly what this run would draw. Skip = the layer's remembered clear leaves the record, the run's final
//            state calls are applied, nothing else. Exact by construction; a missing key field is what the self-check catches.
// Switches: UILAYER 0 off (the hold only for the cache: runs in order) / 1 hoisted / 2 verify (hoisted and in order on alternate
// recordings; a recording whose run key equals the last one compares a band of the layer before (= the last recording's result) and
// after: "differ" must stay 0) / 3 poison (1 + every pushed target of a hoisted run magenta afterwards: a read out of order shows).
// UICACHE 0 off / 1 on (+ every 256th hit drawn and compared) / 2 verify (every would-hit drawn and compared) / 3 poison (1 + one hit in
// 64 clears the layer magenta instead). -1 = family default (render-pass tilers 1, else 0). Install time (vkn_ui_layer_config).
enum { LH_TGT = 1, LH_VP, LH_SC, LH_CDS, LH_CLR, LH_DRAW, LH_REG, LH_FENCE };
enum { LH_SET = 48, LH_TH = 64, LH_MAXB = 8u << 20, LH_MAXE = 16384, LH_CMP = 4, LH_BAND = 64 };
typedef struct lh_hdr { uint32_t size; uint8_t kind, grp, pad[10]; } lh_hdr;   // 16 bytes
typedef struct lh_ptr { lh_hdr h; vkn_tex *a, *b; int32_t v[4]; int32_t has, pad; } lh_ptr;   // TGT (a, b) / VP / SC (v) / CDS (v0, v1) / CLR (a, has, v) / REG (a, v)
typedef struct lh_draw_e { lh_hdr h; vkn_ui_draw_in in; vkn_block vb; uint32_t o_vsc, o_fsc, o_fsc1, o_verts, o_idx; uint8_t has_vb, pad[3]; } lh_draw_e;
typedef struct lh_cmp { int live; uint64_t frame, key; vkn_block b; uint32_t bytes; } lh_cmp;
static struct {
    int lreq, creq, init, lmode, cmode, tile;
    vkn_tex *L;                                       // the layer
    uint8_t hold, replay, owns, unsafe, content, fence, has_l, has_f, keyok, keywhy, seen_tgt, seen_vp, seen_sc, pre_done, pre_c0;
    uint32_t serial, runs;                            // the recording of the current runs; runs with content in it
    vkn_tex *c0, *d0, *cc, *cd;                       // live UI targets at the run's begin; the run's current (held) targets
    int32_t vp0[4], sc0[4]; uint8_t vp0_set, sc0_set; // live viewport / scissor at the run's begin
    uint8_t *buf; uint32_t n, cap, nents, ndraws;
    vkn_tex *w[LH_SET], *r[LH_SET], *r0[LH_SET]; uint32_t nw, nr, nr0; uint8_t wfull, rfull;   // written / read by the run; read by group 0
    struct { vkn_tex *t; uint64_t h; uint8_t wr, init; } th[LH_TH]; uint32_t nth;   // the run's targets by order of appearance, running content hashes
    uint64_t key;
    // the cache: the last candidate (the first run of the last recording with content), the textures it wrote, written since?
    struct { uint64_t key; float v[4]; uint8_t ok; } pv;
    vkn_tex *tw[LH_SET]; uint32_t ntw; uint8_t track, tdirty;
    uint64_t hits_all, alt; uint32_t band;
    lh_cmp cmp[LH_CMP];
    uint32_t tdepth; uint64_t t0;                     // CPU time of the entry points (nesting)
} g_lh;
void vkn_ui_layer_config(int layer, int cache) { g_lh.lreq = layer < -1 || layer > 3 ? 0 : layer; g_lh.creq = cache < -1 || cache > 3 ? 0 : cache; g_lh.init = 0; }
static void lh_ev(int ev, vkn_tex *a, vkn_tex *b);
static void lh_fx(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user);
static int lh_on(void) {
    if (g_lh.init) return g_lh.lmode || g_lh.cmode;
    const vkn_dev *d = vkn_device(); if (!d) return 0;
    g_lh.init = 1;
    const int f = vkn_policy_family_from(d->props.vendorID, d->props.deviceName, getenv("DM_GFX_SIM_GPU"));
    g_lh.tile = vkn_policy_tile_pass(f, vkn_drv()->driver_id) || g_up.sim_tiler;
    g_lh.lmode = g_lh.lreq < 0 ? (g_lh.tile ? 1 : 0) : g_lh.lreq;
    g_lh.cmode = g_lh.creq < 0 ? (g_lh.tile ? 1 : 0) : g_lh.creq;
    g_ups.layer_mode = g_lh.lmode; g_ups.cache_mode = g_lh.cmode;
    vkn_logf("ui layer (mgpu P7): DM_VK_NATIVE_UILAYER=%d (%s; %s), DM_VK_NATIVE_UICACHE=%d (%s; %s); tile pass family %d",
             g_lh.lmode, g_lh.lreq < 0 ? "family default" : "env", g_lh.lmode == 0 ? "off" : g_lh.lmode == 1 ? "filter blocks before the layer: one layer pass" :
             g_lh.lmode == 2 ? "verify: hoisted / in order on alternate recordings, equal runs compared" : "poison: 1 + pushed targets magenta after a hoisted run",
             g_lh.cmode, g_lh.creq < 0 ? "family default" : "env", g_lh.cmode == 0 ? "off" : g_lh.cmode == 1 ? "an unchanged UI is not drawn again (every 256th hit drawn and compared)" :
             g_lh.cmode == 2 ? "verify: every would-hit drawn and compared" : "poison: 1 + a hit in 64 clears the layer magenta", g_lh.tile);
    if (g_lh.lmode || g_lh.cmode) {
        vkn_pass_before_ev(lh_ev); vkn_pass_on_write(ui_on_write);
        static int fx; if (!fx) fx = !vkn_fx_register("ui-layer", lh_fx, NULL, 0);   // (before "ui-pass-fence" at 1)
    }
    return g_lh.lmode || g_lh.cmode;
}
static int lh_in(vkn_tex *const *set, uint32_t n, const vkn_tex *t) { for (uint32_t i = 0; i < n; i++) if (set[i] == t) return 1; return 0; }
static void lh_add_set(vkn_tex **set, uint32_t *n, uint8_t *full, vkn_tex *t) {
    if (!t || lh_in(set, *n, t)) return;
    if (*n < LH_SET) set[(*n)++] = t; else *full = 1;
}
static int lh_w(const vkn_tex *t) { return t && (g_lh.wfull || lh_in(g_lh.w, g_lh.nw, t)); }
static int lh_wr(const vkn_tex *t) { return t && (lh_w(t) || g_lh.rfull || lh_in(g_lh.r, g_lh.nr, t)); }
static void lh_written(vkn_tex *t) { if (g_lh.track && !g_lh.tdirty && lh_in(g_lh.tw, g_lh.ntw, t)) g_lh.tdirty = 1; }
// the run's target t: its order of appearance (the key names targets so) and its running content hash
static int lh_th(vkn_tex *t) {
    if (!t) return -1;
    for (uint32_t i = 0; i < g_lh.nth; i++) if (g_lh.th[i].t == t) return (int)i;
    if (g_lh.nth >= LH_TH) { if (g_lh.keyok) { g_lh.keyok = 0; g_lh.keywhy = VKN_UICR_MANY; } return -1; }
    g_lh.th[g_lh.nth].t = t; g_lh.th[g_lh.nth].h = fc_mix(0x7a6, g_lh.nth); g_lh.th[g_lh.nth].wr = g_lh.th[g_lh.nth].init = 0;
    return (int)g_lh.nth++;
}
static uint64_t lh_tag(vkn_tex *t) { if (!t) return 0x404; if (t == g_lh.L) return 0x1a4e; const int i = lh_th(t); return 0x7700u + (uint64_t)(uint32_t)i; }
static void lh_content(vkn_tex *t, uint64_t v) { const int i = lh_th(t); if (i >= 0) { g_lh.th[i].h = fc_mix(g_lh.th[i].h, v); g_lh.th[i].wr = 1; } }
// the key holds only what the run itself puts into a target: a pushed target / the depth must be cleared by the run before a draw lands
// in it (the layer begins with the engine's remembered clear, checked at the flush)
static void lh_init(vkn_tex *t) { const int i = lh_th(t); if (i >= 0) g_lh.th[i].init = 1; }
static void lh_need_init(vkn_tex *t) { if (!t || t == g_lh.L) return; const int i = lh_th(t); if (i >= 0 && !g_lh.th[i].init && g_lh.keyok) { g_lh.keyok = 0; g_lh.keywhy = VKN_UICR_INIT; } }
static void *lh_put(uint8_t kind, uint32_t bytes) {
    bytes = (bytes + 15u) & ~15u;
    if (g_lh.n + bytes > g_lh.cap) {
        uint32_t c = g_lh.cap ? g_lh.cap : 65536; while (c < g_lh.n + bytes) c *= 2;
        uint8_t *nb = (uint8_t *)realloc(g_lh.buf, c); if (!nb) return NULL;
        g_lh.buf = nb; g_lh.cap = c;
    }
    lh_hdr *h = (lh_hdr *)(void *)(g_lh.buf + g_lh.n); memset(h, 0, bytes); h->size = bytes; h->kind = kind;
    h->grp = (uint8_t)(g_lh.cc && g_lh.cc != g_lh.L ? 1 : 0);
    g_lh.n += bytes; g_lh.nents++; g_ups.lh_calls++;
    return h;
}
static uint32_t lh_copy(lh_draw_e **dp, const void *src, uint32_t n) {   // bytes appended to the current entry: offset from it (0 = none, ~0 = no memory)
    if (!src || !n) return 0;
    const uint32_t at = (uint32_t)((uint8_t *)*dp - g_lh.buf), off = g_lh.n - at, pad = (n + 15u) & ~15u;
    if (g_lh.n + pad > g_lh.cap) {
        uint32_t c = g_lh.cap; while (c < g_lh.n + pad) c *= 2;
        uint8_t *nb = (uint8_t *)realloc(g_lh.buf, c); if (!nb) return ~0u;
        g_lh.buf = nb; g_lh.cap = c; *dp = (lh_draw_e *)(void *)(g_lh.buf + at);
    }
    memcpy(g_lh.buf + g_lh.n, src, n); g_lh.n += pad; (*dp)->h.size += pad;
    return off;
}
static void lh_flush(int why);
static void lh_unsafe(int why) { if (!g_lh.unsafe) { g_lh.unsafe = 1; g_ups.lh_unsafe[why]++; } }
// the content version of a texture unit for the key (0 = ok)
static int lh_tex_ver(vkn_tex *t, uint64_t *v) {
    if (!t || t->dead || !t->view) { *v = 0x51ace; return 0; }   // (the placeholder at draw time)
    for (uint32_t i = 0; i < g_lh.nth; i++) if (g_lh.th[i].t == t && g_lh.th[i].wr) { *v = fc_mix(0xd1a, g_lh.th[i].h); return 0; }
    if (t == g_lh.L) return -1;   // (the layer read before the run wrote it: not this run's content)
    return fc_tex_ver(t, v);
}
// a run begins (the first held call): the live UI state is the run's starting state
static void lh_begin(void) {
    const uint32_t ser = vkn_cb_serial();
    if (ser != g_lh.serial) { g_lh.serial = ser; g_lh.runs = 0; }
    g_lh.hold = 1; g_lh.owns = 0; g_lh.unsafe = 0; g_lh.content = 0; g_lh.fence = 0; g_lh.has_l = 0; g_lh.has_f = 0; g_lh.keyok = 1; g_lh.keywhy = 0;
    g_lh.c0 = g_color; g_lh.d0 = g_depth; g_lh.cc = g_color; g_lh.cd = g_depth;
    g_lh.vp0_set = (uint8_t)g_vp_set; g_lh.vp0[0] = (int32_t)g_vp.x; g_lh.vp0[1] = (int32_t)g_vp.y; g_lh.vp0[2] = (int32_t)g_vp.width; g_lh.vp0[3] = (int32_t)g_vp.height;
    g_lh.sc0_set = (uint8_t)g_sc_set; g_lh.sc0[0] = g_sc.offset.x; g_lh.sc0[1] = g_sc.offset.y; g_lh.sc0[2] = (int32_t)g_sc.extent.width; g_lh.sc0[3] = (int32_t)g_sc.extent.height;
    g_lh.n = 0; g_lh.nents = 0; g_lh.ndraws = 0; g_lh.nw = g_lh.nr = g_lh.nr0 = 0; g_lh.wfull = g_lh.rfull = 0; g_lh.nth = 0;
    if (!g_lh.L) lh_unsafe(VKN_UILU_NOLAYER);
    g_lh.seen_tgt = g_lh.seen_vp = g_lh.seen_sc = g_lh.pre_done = g_lh.pre_c0 = 0;
    g_lh.key = 0x9a7e;
    g_ups.lh_runs++;
}
// the first content entry of the run: the starting state it sees (what no entry of the run set before it) goes into the key
// (clears and regions read the targets; draws also the viewport and the scissor)
static void lh_pre(int draw) {
    uint64_t k = g_lh.key;
    if (!(g_lh.pre_done & 1)) {
        g_lh.pre_done |= 1;
        if (!g_lh.seen_tgt) { k = fc_mix(fc_mix(fc_mix(k, 0x7e0), lh_tag(g_lh.c0)), lh_tag(g_lh.d0)); g_lh.pre_c0 = 1; }
    }
    if (draw && !(g_lh.pre_done & 2)) {
        g_lh.pre_done |= 2;
        if (!g_lh.seen_vp) k = fc_mix(fc_mix(k, (uint64_t)g_lh.vp0_set << 63 ^ (uint64_t)(uint32_t)g_lh.vp0[0] << 32 ^ (uint32_t)g_lh.vp0[1]), (uint64_t)(uint32_t)g_lh.vp0[2] << 32 | (uint32_t)g_lh.vp0[3]);
        if (!g_lh.seen_sc) k = fc_mix(fc_mix(k, (uint64_t)g_lh.sc0_set << 63 ^ (uint64_t)(uint32_t)g_lh.sc0[0] << 32 ^ (uint32_t)g_lh.sc0[1]), (uint64_t)(uint32_t)g_lh.sc0[2] << 32 | (uint32_t)g_lh.sc0[3]);
    }
    g_lh.key = k;
}
static int lh_want(void) {
    if (g_lh.replay || !lh_on()) return 0;
    if (g_lh.hold) {
        if (vkn_cb_serial() == g_lh.serial) return 1;
        lh_flush(VKN_UILF_OTHER);   // (a run left open across recordings: not expected - executed where we are now)
    }
    if (!vkn_cb()) return 0;
    lh_begin();
    return 1;
}
static void lh_check_size(void) { if (g_lh.n > LH_MAXB || g_lh.nents > LH_MAXE) lh_flush(VKN_UILF_SIZE); }
// ---- holding: one function per entry point ----
static void lh_targets(vkn_tex *c, vkn_tex *d) {
    lh_ptr *e = (lh_ptr *)lh_put(LH_TGT, sizeof *e);
    if (!e) { lh_flush(VKN_UILF_OTHER); ui_targets_x(c, d); return; }
    g_lh.cc = c; g_lh.cd = d; e->h.grp = (uint8_t)(c && c != g_lh.L ? 1 : 0); e->a = c; e->b = d; g_lh.owns = 0; g_lh.seen_tgt = 1;
    if (e->h.grp && lh_in(g_lh.r0, g_lh.nr0, c)) lh_unsafe(VKN_UILU_REUSE);   // a pushed target the layer read before: written again now
    g_lh.key = fc_mix(fc_mix(fc_mix(g_lh.key, LH_TGT), lh_tag(c)), lh_tag(d));
}
static void lh_vs(uint8_t kind, int32_t x, int32_t y, int32_t w, int32_t h) {
    lh_ptr *e = (lh_ptr *)lh_put(kind, sizeof *e);
    if (!e) { lh_flush(VKN_UILF_OTHER); if (kind == LH_VP) ui_viewport_x(x, y, w, h); else ui_scissor_x(x, y, w, h); return; }
    e->v[0] = x; e->v[1] = y; e->v[2] = w; e->v[3] = h;
    if (kind == LH_VP) g_lh.seen_vp = 1; else g_lh.seen_sc = 1;
    g_lh.key = fc_bytes(fc_mix(g_lh.key, kind), e->v, 16);
}
static void lh_note_w(vkn_tex *t) {   // content for t: written by the run
    lh_pre(0);
    if (!t) return;
    lh_add_set(g_lh.w, &g_lh.nw, &g_lh.wfull, t);
    if (t == g_lh.L) g_lh.has_l = 1; else if (!(t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) g_lh.has_f = 1;
    g_lh.content = 1;
}
static void lh_clear_ds(int depth, uint8_t mask) {
    lh_ptr *e = (lh_ptr *)lh_put(LH_CDS, sizeof *e);
    if (!e) { lh_flush(VKN_UILF_OTHER); ui_clear_ds_x(depth, mask); return; }
    e->v[0] = depth; e->v[1] = mask;
    if (e->h.grp) lh_unsafe(VKN_UILU_DS);
    lh_note_w(g_lh.cd); g_lh.content = 1;
    g_lh.key = fc_mix(fc_mix(g_lh.key, LH_CDS), (uint64_t)(uint32_t)depth << 8 | mask);
    if (depth && mask == 0xff) lh_init(g_lh.cd); else lh_need_init(g_lh.cd);
    lh_content(g_lh.cd, (uint64_t)(uint32_t)depth << 8 | mask);
}
static void lh_clear_color(vkn_tex *t, const int32_t rect[4]) {
    lh_ptr *e = (lh_ptr *)lh_put(LH_CLR, sizeof *e);
    if (!e) { lh_flush(VKN_UILF_OTHER); ui_clear_color_x(t, rect); return; }
    e->a = t; e->has = rect != NULL; if (rect) memcpy(e->v, rect, 16);
    e->h.grp = (uint8_t)(t && t != g_lh.L ? 1 : 0);
    if (e->h.grp && lh_in(g_lh.r0, g_lh.nr0, t)) lh_unsafe(VKN_UILU_REUSE);
    lh_note_w(t);
    g_lh.key = fc_bytes(fc_mix(fc_mix(g_lh.key, LH_CLR), lh_tag(t) | (uint64_t)e->has << 32), e->v, 16);
    lh_init(t);   // (a pushed target is read only inside its cleared region; the layer's whole clears are the engine's)
    lh_content(t, fc_bytes(0xc1, e->v, 16));
}
static void lh_region(vkn_tex *t, const int32_t rect[4]) {
    lh_ptr *e = (lh_ptr *)lh_put(LH_REG, sizeof *e);
    if (!e) { lh_flush(VKN_UILF_OTHER); ui_region_x(t, rect); return; }
    e->a = t; memcpy(e->v, rect, 16); e->h.grp = (uint8_t)(t && t != g_lh.L ? 1 : 0);
    lh_pre(0);
    g_lh.key = fc_bytes(fc_mix(fc_mix(g_lh.key, LH_REG), lh_tag(t)), e->v, 16);
}
static void lh_fence(void) {
    lh_ptr *e = (lh_ptr *)lh_put(LH_FENCE, sizeof *e);
    if (!e) { lh_flush(VKN_UILF_OTHER); ui_fence_x(); return; }
    e->h.grp = 0; g_lh.fence = 1;
    g_lh.key = fc_mix(g_lh.key, LH_FENCE);
}
static void lh_draw(const vkn_ui_draw_in *in) {
    lh_draw_e *d = (lh_draw_e *)lh_put(LH_DRAW, sizeof *d);
    if (!d) { lh_flush(VKN_UILF_OTHER); ui_draw_x(in); return; }
    d->in = *in;
    const uint32_t stride = in->vfmt == VKN_UI_VF_V2 ? 8u : 16u;
    if (in->vb) { d->vb = *in->vb; d->has_vb = 1; }
    uint32_t o;
    if ((o = lh_copy(&d, in->vsc, in->vsc_bytes)) == ~0u) goto oom; d->o_vsc = o;
    if ((o = lh_copy(&d, in->fsc, in->fsc_bytes)) == ~0u) goto oom; d->o_fsc = o;
    if ((o = lh_copy(&d, in->fsc1, in->fsc1 ? in->fsc1_bytes : 0)) == ~0u) goto oom; d->o_fsc1 = o;
    if (!in->vb) {
        if ((o = lh_copy(&d, in->verts, in->nverts * stride)) == ~0u) goto oom; d->o_verts = o;
        if ((o = lh_copy(&d, in->idx, in->quads ? 0 : in->nidx * 2u)) == ~0u) goto oom; d->o_idx = o;
    }
    g_lh.ndraws++; g_ups.lh_draws++;
    lh_pre(1);
    {
        const int grp = d->h.grp;
        lh_note_w(g_lh.cc);
        if (in->ds) { lh_note_w(g_lh.cd); if (grp) lh_unsafe(VKN_UILU_DS); }
        for (int u = 0; u < VKN_UI_UNITS; u++) {
            vkn_tex *t = in->tex[u]; if (!t) continue;
            lh_add_set(g_lh.r, &g_lh.nr, &g_lh.rfull, t);
            if (grp && (t == g_lh.L || t == g_lh.cd)) lh_unsafe(VKN_UILU_READL);
            if (!grp) { uint8_t f = 0; lh_add_set(g_lh.r0, &g_lh.nr0, &f, t); if (f) lh_unsafe(VKN_UILU_MANY); }
        }
    }
    {   // the key (fc_put_draw's fields; textures by the run's versions)
        uint64_t k = fc_mix(fc_mix(fc_mix(0xd7a, in->vs), in->fs), (uint64_t)in->vfmt | (uint64_t)in->quads << 8 | (uint64_t)in->blend << 16 | (uint64_t)in->ds << 24 |
                                                                  (uint64_t)in->color_off << 32 | (uint64_t)in->stencil_cmp << 40 | (uint64_t)in->stencil_write << 48);
        for (int u = 0; u < VKN_UI_UNITS; u++) { uint64_t v; if (lh_tex_ver(in->tex[u], &v)) { if (g_lh.keyok) { g_lh.keyok = 0; g_lh.keywhy = VKN_UICR_TEX; } v = 0; } k = fc_mix(k, v); }
        k = fc_bytes(k, in->vsc, in->vsc_bytes); k = fc_bytes(k, in->fsc, in->fsc_bytes); k = fc_bytes(k, in->fsc1, in->fsc1 ? in->fsc1_bytes : 0);
        if (in->vb) {
            if (!in->vb->cpu || in->vb_off + (uint64_t)in->nverts * stride > in->vb->size || (!in->quads && in->ib_off + 2ull * in->nidx > in->vb->size)) { if (g_lh.keyok) { g_lh.keyok = 0; g_lh.keywhy = VKN_UICR_VB; } }
            else { k = fc_bytes(k, (const uint8_t *)in->vb->cpu + in->vb_off, in->nverts * stride); if (!in->quads) k = fc_bytes(k, (const uint8_t *)in->vb->cpu + in->ib_off, in->nidx * 2u); }
        } else { k = fc_bytes(k, in->verts, in->nverts * stride); if (!in->quads) k = fc_bytes(k, in->idx, in->nidx * 2u); }
        k = fc_mix(k, (uint64_t)in->nverts << 32 | in->nidx);
        g_lh.key = fc_mix(fc_mix(g_lh.key, LH_DRAW), k);
        lh_need_init(g_lh.cc); if (in->ds) lh_need_init(g_lh.cd);
        lh_content(g_lh.cc, k); if (in->ds) lh_content(g_lh.cd, k);
    }
    lh_check_size();
    return;
oom:
    g_lh.n = (uint32_t)((uint8_t *)d - g_lh.buf); g_lh.nents--;
    lh_flush(VKN_UILF_OTHER); ui_draw_x(in);
}
// ---- executing a held run ----
static void lh_exec(const lh_hdr *h) {
    const lh_ptr *p = (const lh_ptr *)(const void *)h;
    switch (h->kind) {
    case LH_TGT: ui_targets_x(p->a, p->b); break;
    case LH_VP: ui_viewport_x(p->v[0], p->v[1], p->v[2], p->v[3]); break;
    case LH_SC: ui_scissor_x(p->v[0], p->v[1], p->v[2], p->v[3]); break;
    case LH_CDS: ui_clear_ds_x(p->v[0], (uint8_t)p->v[1]); break;
    case LH_CLR: ui_clear_color_x(p->a, p->has ? p->v : NULL); break;
    case LH_REG: ui_region_x(p->a, p->v); break;
    case LH_FENCE: ui_fence_x(); break;
    case LH_DRAW: {
        const lh_draw_e *d = (const lh_draw_e *)(const void *)h; vkn_ui_draw_in in = d->in; const uint8_t *b = (const uint8_t *)d;
        in.vsc = d->o_vsc ? b + d->o_vsc : NULL; in.fsc = d->o_fsc ? b + d->o_fsc : NULL; in.fsc1 = d->o_fsc1 ? b + d->o_fsc1 : NULL;
        if (d->has_vb) in.vb = &d->vb; else { in.verts = d->o_verts ? b + d->o_verts : NULL; in.idx = d->o_idx ? (const uint16_t *)(const void *)(b + d->o_idx) : NULL; }
        (void)ui_draw_x(&in);
        break; }
    default: break;
    }
}
// group g of the run, state applied lazily (each executed entry sees the targets / viewport / scissor it saw in the run)
typedef struct lh_st { vkn_tex *c, *d; int32_t vp[4], sc[4]; uint8_t vp_set, sc_set, tgt_set; } lh_st;
static void lh_state_apply(const lh_st *sv, lh_st *ap) {
    if ((sv->c || sv->d) && (!ap->tgt_set || sv->c != ap->c || sv->d != ap->d)) { ui_targets_x(sv->c, sv->d); ap->c = sv->c; ap->d = sv->d; ap->tgt_set = 1; }
    if (sv->vp_set && (!ap->vp_set || memcmp(sv->vp, ap->vp, 16))) { ui_viewport_x(sv->vp[0], sv->vp[1], sv->vp[2], sv->vp[3]); memcpy(ap->vp, sv->vp, 16); ap->vp_set = 1; }
    if (sv->sc_set && (!ap->sc_set || memcmp(sv->sc, ap->sc, 16))) { ui_scissor_x(sv->sc[0], sv->sc[1], sv->sc[2], sv->sc[3]); memcpy(ap->sc, sv->sc, 16); ap->sc_set = 1; }
}
static void lh_scan_state(const lh_hdr *h, lh_st *sv) {
    const lh_ptr *p = (const lh_ptr *)(const void *)h;
    if (h->kind == LH_TGT) { sv->c = p->a; sv->d = p->b; }
    else if (h->kind == LH_VP) { memcpy(sv->vp, p->v, 16); sv->vp_set = 1; }
    else if (h->kind == LH_SC) { memcpy(sv->sc, p->v, 16); sv->sc_set = 1; }
}
static void lh_group(int g, const lh_st *s0, lh_st *ap) {
    lh_st sv = *s0;
    for (uint32_t at = 0; at < g_lh.n;) {
        const lh_hdr *h = (const lh_hdr *)(const void *)(g_lh.buf + at);
        at += h->size;
        if (h->kind == LH_TGT || h->kind == LH_VP || h->kind == LH_SC) { lh_scan_state(h, &sv); continue; }
        if (h->grp != g || h->kind == LH_FENCE) continue;
        lh_state_apply(&sv, ap);
        lh_exec(h);
    }
}
// the run's final state (a hit, the end of a hoisted run): targets, viewport, scissor as the run left them
static void lh_final_state(const lh_st *s0, lh_st *ap) {
    lh_st sv = *s0;
    for (uint32_t at = 0; at < g_lh.n;) { const lh_hdr *h = (const lh_hdr *)(const void *)(g_lh.buf + at); at += h->size; lh_scan_state(h, &sv); }
    if (sv.c) lh_state_apply(&sv, ap);
}
// a band of the layer into a staging block (the compare of verify / self-check); a remembered clear of the layer stays remembered
static int lh_band(lh_cmp *c, uint32_t y0, uint32_t bh, int second) {
    vkn_tex *L = g_lh.L; VkCommandBuffer cb = vkn_cb();
    if (!L || L->dead || !cb) return -1;
    float v[4]; const int had = vkn_pass_clear_take(L, v, 1);   // (not run by the layout change: the band is what the layer HOLDS)
    vkn_pass_layout(L, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkBufferImageCopy r = {.bufferOffset = c->b.off + (second ? c->bytes : 0), .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageOffset = {0, (int32_t)y0, 0},
                                 .imageExtent = {L->w, bh, 1}};
    vkCmdCopyImageToBuffer(cb, L->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->b.buf, 1, &r);
    if (had) vkn_pass_clear_color(L, 0, v, NULL);   // (remembered again: no pass of the layer is open after the layout change)
    return 0;
}
static lh_cmp *lh_cmp_new(void) {
    vkn_tex *L = g_lh.L;
    if (!L || L->dead || L->samples > 1 || (L->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return NULL;
    lh_cmp *c = NULL; for (int i = 0; i < LH_CMP && !c; i++) if (!g_lh.cmp[i].live) c = &g_lh.cmp[i];
    if (!c) return NULL;
    const uint32_t bh = L->h < LH_BAND ? L->h : LH_BAND;
    c->bytes = L->w * bh * 4u;   // (Iggy's layer: RGBA8)
    if (vkn_block_alloc(VKN_ARENA_STAGING, 2ull * c->bytes, 16, &c->b)) return NULL;
    if (c->b.cpu) memset(c->b.cpu, 0, 2ull * c->bytes);   // (both halves are written by the copies; a copy that did not run compares equal zeros)
    return c;
}
static void lh_cmp_check(int all) {
    for (int i = 0; i < LH_CMP; i++) {
        lh_cmp *c = &g_lh.cmp[i];
        if (!c->live || (!all && vkn_frame() < c->frame + 4)) continue;
        const int differ = memcmp(c->b.cpu, (const uint8_t *)c->b.cpu + c->bytes, c->bytes) != 0;
        if (differ) {
            g_ups.uc_differ++;
            UI_RED(RR_STATIC, "ui layer / cache: a band of the layer (key %016llx, %u bytes) differs between two recordings of the same UI run%s", (unsigned long long)c->key, c->bytes,
                   g_lh.cmode == 1 || g_lh.cmode == 3 ? ": the UI cache is off for this session" : "");
            if (g_lh.cmode == 1 || g_lh.cmode == 3) { g_lh.cmode = 0; g_ups.cache_mode = 0; }
        }
        vkn_block_free(&c->b); c->live = 0;
    }
}
static void lh_flush(int why) {
    if (!g_lh.hold) return;
    g_lh.hold = 0;
    const uint64_t t0 = vkn_now_ns();
    if (why >= 0 && why < VKN_UILF_N) g_ups.lh_flush[why]++;
    lh_cmp_check(0);
    if (!g_lh.nents) return;
    // the engine's bound set now (put back after the run when the engine bound it after the run's last binding)
    uint32_t en = 0; vkn_tex *ec[VKN_PASS_COLORS], *ed = NULL; uint32_t el[VKN_PASS_COLORS], em[VKN_PASS_COLORS];
    vkn_pass_cur_get(&en, ec, el, em, &ed);
    const int restore = g_lh.owns;
    // the cache: the first run with content of this recording
    int skip = 0, check = 0, cand = 0, poison = 0; float v[4] = {0, 0, 0, 0}; uint64_t key = 0;
    const int first = g_lh.content && g_lh.runs == 0;
    if (g_lh.content) g_lh.runs++;
    if (first && (g_lh.cmode || g_lh.lmode == 2)) {
        int r = -1;
        if (!g_lh.L) r = VKN_UICR_NOLAYER;
        else if (!g_lh.keyok) r = g_lh.keywhy;
        else if (!g_lh.has_l) r = VKN_UICR_NOL;
        else if (g_lh.pre_c0 && g_lh.c0 && g_lh.c0 != g_lh.L) r = VKN_UICR_INIT;   // (drawn into a pushed target the run did not bind: its earlier content is not the run's)
        else if (!vkn_pass_clear_take(g_lh.L, v, 0)) r = VKN_UICR_NOCLEAR;
        if (r < 0) {
            cand = 1; key = g_lh.key; for (int i = 0; i < 4; i++) { uint32_t b; memcpy(&b, &v[i], 4); key = fc_mix(key, b); }
            g_ups.uc_cand++;
            if (!g_lh.pv.ok) r = VKN_UICR_FIRST;
            else if (g_lh.tdirty) r = VKN_UICR_DIRTY;
            else if (g_lh.pv.key != key) r = VKN_UICR_KEY;
        }
        if (r >= 0) g_ups.uc_refused[r]++;
        if (r < 0) {   // the same run as the last recording's, nothing it wrote touched since
            g_ups.uc_would++;
            check = g_lh.cmode == 2 || g_lh.lmode == 2 || (g_lh.cmode != 0 && (++g_lh.hits_all & 255) == 0);
            skip = g_lh.cmode && !check;
            poison = skip && g_lh.cmode == 3 && (vkn_frame() & 63) == 0;
        }
    }
    g_lh.replay = 1;
    lh_st s0; memset(&s0, 0, sizeof s0); s0.c = g_lh.c0; s0.d = g_lh.d0;
    s0.vp_set = g_lh.vp0_set; memcpy(s0.vp, g_lh.vp0, 16); s0.sc_set = g_lh.sc0_set; memcpy(s0.sc, g_lh.sc0, 16);
    lh_st ap = s0; ap.tgt_set = 0;   // (applied: the live state, except the set vkn_pass holds now)
    if (skip) {
        vkn_pass_clear_take(g_lh.L, NULL, 1);   // the layer keeps the last recording's UI, which is this run's
        if (poison) { static const float mg[4] = {1, 0, 1, 1}; vkn_pass_clear_color(g_lh.L, 0, mg, NULL); g_ups.uc_poison++; }
        lh_final_state(&s0, &ap);
        g_ups.uc_hits++; g_ups.uc_draws_saved += g_lh.ndraws; g_ups.uc_calls_saved += g_lh.nents;
    } else {
        lh_cmp *c = check ? lh_cmp_new() : NULL;
        const uint32_t bh = g_lh.L && g_lh.L->h < LH_BAND ? g_lh.L->h : LH_BAND;
        const uint32_t y0 = c && g_lh.L->h > bh ? (g_lh.band++ * bh) % (g_lh.L->h - bh + 1) : 0;
        if (c && lh_band(c, y0, bh, 0)) { vkn_block_free(&c->b); c = NULL; }
        int hoist = g_lh.lmode && !g_lh.unsafe && g_lh.has_f && g_lh.has_l;
        if (g_lh.lmode == 2 && hoist) hoist = (int)(g_lh.alt++ & 1);
        if (hoist) {
            lh_group(1, &s0, &ap);
            {   // the layer's first set (the clean end of a P2 epoch), then every texture the layer reads made sampleable: a layout change inside
                // the layer's pass would end it (the composites read the pushed targets just drawn)
                lh_st sv = s0;
                for (uint32_t at = 0; at < g_lh.n;) {
                    const lh_hdr *h = (const lh_hdr *)(const void *)(g_lh.buf + at); at += h->size;
                    if (h->kind == LH_TGT || h->kind == LH_VP || h->kind == LH_SC) { lh_scan_state(h, &sv); continue; }
                    if (h->grp == 0 && h->kind != LH_FENCE) break;
                }
                lh_state_apply(&sv, &ap);
                for (uint32_t i = 0; i < g_lh.nr0; i++) if (g_lh.r0[i] != sv.c && g_lh.r0[i] != sv.d && !g_lh.r0[i]->dead) (void)vkn_pass_sample(g_lh.r0[i]);
            }
            g_up_noarea = 1; lh_group(0, &s0, &ap); g_up_noarea = 0;
            lh_final_state(&s0, &ap);
            if (g_lh.fence) ui_fence_x();
            g_ups.lh_hoisted++;
            if (g_lh.lmode == 3) {   // poison: the pushed targets the run wrote, magenta now (their content is spent)
                static const float mg[4] = {1, 0, 1, 1};
                for (uint32_t i = 0; i < g_lh.nw; i++) if (g_lh.w[i] != g_lh.L && !(g_lh.w[i]->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) { vkn_pass_clear_color(g_lh.w[i], 0, mg, NULL); g_ups.lh_poison++; }
            }
        } else {
            if (g_lh.c0) ui_targets_x(g_lh.c0, g_lh.d0);   // (the run's starting set: vkn_pass may hold the engine's now)
            for (uint32_t at = 0; at < g_lh.n;) { const lh_hdr *h = (const lh_hdr *)(const void *)(g_lh.buf + at); at += h->size; lh_exec(h); }
            g_ups.lh_ordered++;
        }
        if (c && !lh_band(c, y0, bh, 1)) { c->live = 1; c->frame = vkn_frame(); c->key = key; g_ups.uc_verify++; } else if (c) vkn_block_free(&c->b);
    }
    if (restore) vkn_pass_targets(en, ec, el, em, ed);
    g_lh.replay = 0;
    // the candidate for the next recording: this run, the textures it wrote, tracked from now on
    if (first) {
        g_lh.pv.ok = (uint8_t)(cand && !poison); g_lh.pv.key = key; memcpy(g_lh.pv.v, v, sizeof v);
        g_lh.ntw = 0; for (uint32_t i = 0; i < g_lh.nw; i++) g_lh.tw[g_lh.ntw++] = g_lh.w[i];
        g_lh.track = (uint8_t)(cand && !g_lh.wfull); g_lh.tdirty = (uint8_t)(poison || g_lh.wfull);
    }
    g_lh.n = 0; g_lh.nents = 0; g_lh.ndraws = 0;
    g_ups.ui_ns += vkn_now_ns() - t0;
}
// vkn_pass_before_ev: an engine use while a run is held (the rule at the top)
static void lh_ev(int ev, vkn_tex *a, vkn_tex *b) {
    if (g_lh.replay || !g_lh.hold) return;
    int why = -1;
    switch (ev) {
    case VKN_PBE_TARGETS: g_lh.owns = 1; return;
    case VKN_PBE_READY: case VKN_PBE_DEPTH:
        if (!g_lh.owns) why = VKN_UILF_BOUND;
        else {
            uint32_t n = 0; vkn_tex *c[VKN_PASS_COLORS], *d = NULL; vkn_pass_cur_get(&n, c, NULL, NULL, &d);
            for (uint32_t i = 0; i < n && why < 0; i++) if (lh_wr(c[i])) why = VKN_UILF_CONFLICT;
            if (why < 0 && lh_wr(d)) why = VKN_UILF_CONFLICT;
        }
        break;
    case VKN_PBE_SAMPLE: if (lh_w(a)) why = a == g_lh.L ? VKN_UILF_LAYER : VKN_UILF_CONFLICT; break;
    case VKN_PBE_STORAGE: case VKN_PBE_LAYOUT: case VKN_PBE_CLEAR: if (lh_wr(a)) why = a == g_lh.L && ev == VKN_PBE_LAYOUT ? VKN_UILF_LAYER : VKN_UILF_CONFLICT; break;
    case VKN_PBE_COPY: if (lh_wr(a) || lh_w(b)) why = b == g_lh.L ? VKN_UILF_LAYER : VKN_UILF_CONFLICT; break;
    case VKN_PBE_END: why = VKN_UILF_END; break;
    default: why = VKN_UILF_OTHER; break;
    }
    if (why >= 0) lh_flush(why);
}
// the effect chain, first: an engine draw while the run's binding is GDraw's (a custom draw) - before any piece looks at the set
static void lh_fx(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)in; (void)ctx; (void)out; (void)user;
    if (g_lh.hold && !g_lh.replay && !g_lh.owns) lh_flush(VKN_UILF_BOUND);
}
void vkn_ui_layer(vkn_tex *L) {   // the host (RenderingBegin): GDraw's base colour target
    if (L == g_lh.L) return;
    if (g_lh.hold) lh_flush(VKN_UILF_OTHER);
    g_lh.L = L; g_lh.pv.ok = 0; g_lh.track = 0;
}
// ---- the entry points (vkn_ui.h; the recording thread) ----
#define LH_T0() do { if (!g_lh.tdepth++) g_lh.t0 = vkn_now_ns(); } while (0)
#define LH_T1() do { if (!--g_lh.tdepth) { g_ups.ui_ns += vkn_now_ns() - g_lh.t0; g_ups.ui_calls++; } } while (0)
void vkn_ui_targets(vkn_tex *color, vkn_tex *depth) { LH_T0(); if (lh_want()) lh_targets(color, depth); else ui_targets_x(color, depth); LH_T1(); }
void vkn_ui_viewport(int32_t x, int32_t y, int32_t w, int32_t h) { LH_T0(); if (lh_want()) lh_vs(LH_VP, x, y, w, h); else ui_viewport_x(x, y, w, h); LH_T1(); }
void vkn_ui_scissor(int32_t x, int32_t y, int32_t w, int32_t h) { LH_T0(); if (lh_want()) lh_vs(LH_SC, x, y, w, h); else ui_scissor_x(x, y, w, h); LH_T1(); }
void vkn_ui_clear_ds(int depth, uint8_t mask) { LH_T0(); if (lh_want()) lh_clear_ds(depth, mask); else ui_clear_ds_x(depth, mask); LH_T1(); }
void vkn_ui_clear_color(vkn_tex *t, const int32_t rect[4]) { LH_T0(); if (lh_want()) lh_clear_color(t, rect); else ui_clear_color_x(t, rect); LH_T1(); }
void vkn_ui_region(vkn_tex *t, const int32_t rect[4]) {
    if (!rect || rect[2] <= 0 || rect[3] <= 0) return;
    LH_T0(); if (lh_want()) lh_region(t, rect); else ui_region_x(t, rect); LH_T1();
}
void vkn_ui_pass_fence(void) { LH_T0(); if (g_lh.hold && !g_lh.replay) lh_fence(); else ui_fence_x(); LH_T1(); }
int vkn_ui_draw(const vkn_ui_draw_in *in) {
    int r = 0;
    LH_T0();
    if (in && lh_want()) lh_draw(in); else r = ui_draw_x(in);
    LH_T1();
    return r;
}
void vkn_ui_layer_flush(void) { if (g_lh.hold && !g_lh.replay) lh_flush(VKN_UILF_OTHER); }
// ==== [/mgpu P7]
#endif
