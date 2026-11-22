// [DOS2] VK-NATIVE stage 5.2 piece B3: the fused post tail through the draw-effect contract (vkn_fx.h). Design, switches, watches:
// vkn_post.h. Ported from dm/gfx/vulkan/engine/xbg_nb_post.inc (namespace nbpost: elide / fuse / step / redirect / carry), whose
// decisions the old path took from the NB plan of the whole submission (dm/gfx/nb/nb_plan.c nb_post_plan, dm/gfx/nb/nb_post_aa.h
// nb_post_aa_plan). Here every rule is applied draw by draw, in stream order; what the plan proved for the rest of the submission is
// proved by history (vkn_post_arm_note: VKN_POST_ARM good frames of the same shape, nb_post_plan's hist_frames) and kept true by the
// watches. The roles (by the fragment key's class, nb_post_class / nb_post_aa_class / nb_post_aa_carry2):
//   E1 PPApplyLUT -> O     skipped (stash: its draw, targets, constant blocks, the fused bundle's extras); F1 = LUT o Sight
//   H1 EoCSight O -> S     drawn with the fused shader F1 (or F1A when route A is armed) fed by the stash; O becomes STALE
//   C  EoCFog / EoCOutlineCombine cur -> T   carry: F1A's output goes on through T (route A; the outline from level 4)
//   A  PPFXAA cur -> X     the AA step: skipped after an F1A draw; X becomes an ALIAS of cur (its readers sample cur)
//   E2 PPVignette X -> V   skipped (level 3; X read through the alias); F2 = Vignette o CombineUI_NoFetch
//   H2 CombineUI_NoFetch V   drawn with F2; V becomes STALE
// No game addresses. Recording thread (the chain function, the close listener); the level reads and vkn_post_install: any thread.
#ifdef DM_GAME_DOS2
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkn_post.h"
#include "vkn_fx.h"
#include "vkn_buf.h"
#include "vkn_frame.h"
#include "vkn_shlib.h"
#include "vkn_state.h"
#include "vkn_fsr.h"   // [VK-LOWRES] route A at a stretch: the FSR pre pass left out (vkn_fsr_stretch_part / vkn_fsr_pre_skip)
#include "../nb/nb_fused.h"
#include "../nb/nb_post_aa.h"   // nb_post_aa_class / nb_post_aa_carry2 (and nb_ir.h: nb_post_class)

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
// vkn_post_pack.cpp / the library (weak: the stage-3 host test links neither; it installs vkn_post_test_loader)
int vkn_post_meta(const char *key, nb_fx *out, char *why, size_t why_bytes) __attribute__((weak));
int vkn_shlib_shader(const char *key, const void *control, const void *code, xbg_vkn_stage *out) __attribute__((weak));
static vkn_post_load_fn g_test_load;
void vkn_post_test_loader(vkn_post_load_fn fn) { g_test_load = fn; }

// ================================================================================================ switches
static int g_level = -1, g_aa = 1, g_luma = 0, g_verify = 0, g_part = 1;   // g_part: [VK-LOWRES] DM_VK_NATIVE_POST_PART
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int env_int(const char *n, int def, int lo, int hi) {
    const char *e = getenv(n);
    if (!e || !*e) return def;
    char *end = NULL; const long v = strtol(e, &end, 10);
    if (end == e || *end || v < lo || v > hi) { vkn_logf(RED "post: %s=%s is not %d..%d: %d used", n, e, lo, hi, def); return def; }
    return (int)v;
}
static void read_switches(void) {
    g_aa = env_int("DM_VK_NATIVE_POST_AA", 1, 0, 1);
    g_luma = env_int("DM_VK_NATIVE_POST_LUMA", 0, 0, 2);
    g_verify = env_int("DM_VK_NATIVE_POST_VERIFY", 0, 0, 2);
    g_part = env_int("DM_VK_NATIVE_POST_PART", 1, 0, 2);
    __atomic_store_n(&g_level, env_int("DM_VK_NATIVE_POST", VKN_POST_DEFAULT, 0, 4), __ATOMIC_RELEASE);
}
int vkn_post_level(void) { pthread_once(&g_once, read_switches); return __atomic_load_n(&g_level, __ATOMIC_ACQUIRE); }
int vkn_post_mobile(void) { return vkn_post_level() >= VKN_POST_MOBILE; }
// [VK-LOWRES] host tests (forked children: the switches were read once in the parent): DM_VK_NATIVE_POST_PART / _VERIFY set directly
void vkn_post_test_switches(int part, int verify) { pthread_once(&g_once, read_switches); g_part = part; g_verify = verify; }

// ================================================================================================ pure parts
int vkn_post_class_key(const char *k) {
    if (!k) return VKN_PC_OTHER;
    uint64_t fs = 0; int n = 0;
    for (; n < 16 && k[n]; n++) {
        const char c = k[n]; const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0) return VKN_PC_OTHER;
        fs = fs << 4 | (uint64_t)v;
    }
    if (n != 16) return VKN_PC_OTHER;
    switch (nb_post_class(fs)) {
    case NB_PSH_LUT: return VKN_PC_LUT;
    case NB_PSH_SIGHT: return VKN_PC_SIGHT;
    case NB_PSH_VIG: return VKN_PC_VIG;
    case NB_PSH_COMB: return VKN_PC_COMB;
    default: break;
    }
    switch (nb_post_aa_class(fs)) { case NB_PSA_FXAA: return VKN_PC_FXAA; case NB_PSA_COPY: return VKN_PC_COPY; default: break; }
    if (nb_post_aa_carry(fs)) return VKN_PC_FOG;
    if (nb_post_aa_carry2(fs, 1)) return VKN_PC_OUTLINE;
    return VKN_PC_OTHER;
}
void vkn_post_cover(const float vp[4], const int32_t sc[4], uint32_t w, uint32_t h, int32_t r[4]) {   // xbg_nb_post.inc cover()
    double x0 = vp[0], y0 = vp[1], x1 = (double)vp[0] + vp[2], y1 = (double)vp[1] + vp[3];
    if (vp[3] < 0) { y0 = (double)vp[1] + vp[3]; y1 = vp[1]; }   // (a flipped viewport covers the same rows)
    if (x0 < sc[0]) x0 = sc[0];
    if (y0 < sc[1]) y0 = sc[1];
    if (x1 > (double)sc[0] + sc[2]) x1 = (double)sc[0] + sc[2];
    if (y1 > (double)sc[1] + sc[3]) y1 = (double)sc[1] + sc[3];
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    r[0] = (int32_t)x0; r[1] = (int32_t)y0; r[2] = (int32_t)x1; r[3] = (int32_t)y1;
}
int vkn_post_arm_note(vkn_post_arm *a, uint64_t frame, int good) {
    if (!good) { a->good = 0; a->armed = 0; a->last = frame + 1; return 0; }
    if (a->last != frame + 1) {   // the first verdict of this frame (a bad one of this frame already stopped it: last == frame + 1)
        a->good = a->last == frame || a->good == 0 ? a->good + 1 : 1;   // consecutive frames only (a frame without a verdict breaks the run)
        a->last = frame + 1;
        if (a->good >= VKN_POST_ARM) a->armed = 1;
    }
    return a->armed;
}

// ================================================================================================ state
enum { FK_F1, FK_F1A, FK_F2, FK_N };
enum { PR_F1, PR_F2, PR_RA, PR_N };                       // armed shapes: the pairs and route A
enum { FD_NONE, FD_EXTRA, FD_DUMMY, FD_C1_VS, FD_C1_FS }; // feeds of a fused / twin shader's effect slots
enum { W_PROBE = 1, W_STALE, W_ALIAS };
// refusal / event reasons (census): one counter each
enum {
    R_BUNDLE, R_RT, R_ALPHA, R_EKEY, R_EXTRA, R_REQ_E, R_SIZE_E, R_NOHOST, R_HKEY, R_NOREAD, R_BASE, R_HSIZE, R_REQ_H, R_HCOVER,
    R_HOSTWRITE, R_STEP_SHAPE, R_STEP_INPUT, R_STEP_SIZE, R_STEP_COVER, R_STEP_SHARED, R_STEP_COPY, R_CARRY, R_OTHER_READER,
    R_NOSTEP, R_PART, R_TWIN, R_LATE_STEP, R_UNSEEN_USE, R_STALE_READ, R_OTHER_WRITE, R_BUILD, R_WATCH_FULL, R_SERIAL, R_REPLAY_FIRST,
    R_EDRAWS, R_NOVIG, R_STEP_STRETCH, R_STEP_PARTREAD, R_N
};
static const char *const k_why[R_N] = {
    "fused bundle not ready / unusable", "targets not one colour without depth", "alpha test", "elided key differs from the bundle's",
    "an extra input of the skipped draw is not bound", "requirement of the skipped draw", "input and target sizes differ",
    "no skipped draw before the host", "host key differs from the bundle's", "host does not sample the skipped target",
    "host samples the skipped target in another slot", "host target size differs", "requirement of the host", "host draws outside the skipped draw",
    "host writes the skipped target", "AA step shape", "AA step does not sample only the fused output", "AA step sizes / formats differ",
    "AA step coverage differs from the fused draw's", "AA step target has shared storage", "the step is the PPDefault copy (not replaced)",
    "carry pass refused", "another post draw reads the fused output before the AA step", "no AA step after the fused draw",
    "a part of the target is drawn and the AA step is not the FSR stretch with DM_VK_NATIVE_FSR_AA=1 (route A at a part needs it)", "reader of an alias cannot be redirected (copied)", "AA step refused after F1A (FXAA ran over it)",
    "a watched image was used by a draw / copy this module does not see", "a stale image was read and no stash could restore it",
    "a draw of another class writes a watched image", "fused / twin shader build failed", "more than 4 watched images",
    "a stash of another recording", "a replay before an arrays draw with another first vertex",
    "the skipped pass has more than one draw",
    "CombineUI without a Vignette draw before it (the scene draws no vignette: F2 has nothing to fuse)",   // [mgpu P4]
    "AA step after F1A at a part is not the FSR stretch of that part (module / mode / stretch / viewport / source)",   // [VK-LOWRES]
    "the FSR stretch reads beyond the part F1A and its carries drew",                                                 // [VK-LOWRES]
};

typedef struct dcopy {
    vkn_draw_in in; int32_t tex[VKN_STAGE_SLOTS][VKN_TEX_SLOTS], smp[VKN_STAGE_SLOTS][VKN_TEX_SLOTS];
    uint8_t indexed; uint32_t first, count, inst;
} dcopy;
typedef struct stash {
    int valid; int pair; uint32_t serial; uint64_t frame; int skipped;
    dcopy d;
    uint32_t n; vkn_tex *color[VKN_PASS_COLORS]; uint32_t layer[VKN_PASS_COLORS], mip[VKN_PASS_COLORS]; vkn_tex *depth;   // referenced
    struct { uint8_t st, slot; uint32_t off, size; } ub[24]; uint32_t nub;
    vkn_tex *O; int32_t rect[4]; int whole;
    vkn_fx_tex xt[NB_FX_MAX_EXTRA]; vkn_fx_ubo xu[NB_FX_MAX_EXTRA]; int first_tex;
    const vkn_shader *eshader;
} stash;
typedef struct fused {
    int state; char key[40]; nb_fx fx;
    const vkn_shader *sh; const vkn_shader *host; uint32_t host_id; char host_vs[17], host_fs[17];
    int base_slot;
    uint8_t tfeed[VKN_FX_TEX], targ[VKN_FX_TEX], ufeed[VKN_FX_UBO], uarg[VKN_FX_UBO];
} fused;
typedef struct twin { const vkn_shader *r; uint32_t rid; uint64_t mask; const vkn_shader *sh; uint8_t tfeed[VKN_FX_TEX], targ[VKN_FX_TEX], ufeed[VKN_FX_UBO], uarg[VKN_FX_UBO]; int bad; } twin;
typedef struct watch { vkn_tex *t; uint8_t kind, pair, bad; vkn_tex *holder; stash *st; uint32_t serial; const vkn_tex *ht; uint64_t clr_ns;
                       int32_t rect[4]; } watch;   // rect: [VK-LOWRES] the texels whose content the watch is about (x0, y0, x1, y1): the skipped draw's cover
                                                   // (STALE / PROBE; the part at a lower 3D scale), the whole image (ALIAS)   // clr_ns: [mgpu P4]   // ht: a probe host target (pointer only)
typedef struct cls_ent { const vkn_shader *s; uint8_t cls; } cls_ent;

static struct {
    int installed, close_reg, level, level_ra;
    uint32_t serial;
    cls_ent *cls; uint32_t ncls;
    fused fz[FK_N];
    twin tw[16]; uint32_t ntw;
    stash st[2];                     // the last E stash of F1 / F2
    int pend;                        // pair whose stash waits for its host (-1)
    watch w[4]; uint32_t nw;
    vkn_post_arm arm[PR_N];
    const vkn_tex *armO[2];          // per pair: the skipped target the arming was earned on (pointer compare only); another E target
                                     // (the HUD portrait's own LUT -> FXAA) is never skipped and never disarms the pair
    struct { uint32_t serial; const vkn_tex *O; } lastE[2];
    struct { vkn_tex *O; int pair; const vkn_tex *ht; int32_t rect[4]; } probe;   // a probe to put after the current draw's own reads   // the last E of each pair: a second E into the same target = 2 draws
    struct { int on, real, part; uint32_t serial; vkn_tex *S, *cur; int32_t rect[4]; int ncarry; } la;   // part: [VK-LOWRES] F1A drawn on a part (the step must be the FSR stretch)   // F1A output (real) / its observation
    uint64_t why[R_N]; uint32_t reds[R_N];
    vkn_post_stats s;
    uint64_t next_ns;
} g = { .pend = -1 };

static void count(int r) { if (r >= 0 && r < R_N) { g.why[r]++; g.s.refused_total++; } }
static void red(int r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void red(int r, const char *fmt, ...) {
    if (r < 0 || r >= R_N || g.reds[r]++ >= 16) return;
    char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    vkn_logf(RED "post: %s: %s", k_why[r], b);
}
static int applying(void) { return g_verify != 1; }   // 0 = verify: the engine's draws are drawn as recorded

// ================================================================================================ classes (once per shader id)
static int cls_of(const vkn_shader *s) {
    if (!s || vkn_fx_shader_is(s)) return VKN_PC_OTHER;
    const uint32_t id = s->id;
    if (id < g.ncls && g.cls[id].s == s) return g.cls[id].cls;
    if (id >= g.ncls) {
        uint32_t n = g.ncls ? g.ncls : 1024; while (n <= id) n *= 2;
        if (n > (1u << 20)) return vkn_post_class_key(s->key[VKN_SRC_FS]);   // (ids are dense and small: never)
        cls_ent *c = (cls_ent *)realloc(g.cls, n * sizeof *c);
        if (!c) return vkn_post_class_key(s->key[VKN_SRC_FS]);
        memset(c + g.ncls, 0, (n - g.ncls) * sizeof *c); g.cls = c; g.ncls = n;
    }
    g.cls[id].s = s; g.cls[id].cls = (uint8_t)vkn_post_class_key(s->key[VKN_SRC_FS]);
    return g.cls[id].cls;
}

// ================================================================================================ small helpers
static int ncolor(const vkn_fx_ctx *c) { int n = 0; for (int i = 0; i < VKN_PASS_COLORS; i++) if (c->color[i]) n = i + 1; return n; }
static int one_target(const vkn_fx_ctx *c) { return c->color[0] && ncolor(c) == 1 && !c->depth; }
static int blend_off(const vkn_fx_ctx *c) { if (!c->blend) return 1; const VkPipelineColorBlendAttachmentState *b = vkn_blend_att(c->blend); return b && !b->blendEnable; }
// [VK-LOWRES] the draw replaces every channel of its colour target 0 (no blending, all four write-mask bits): what an overwrite needs
static int writes_all(const vkn_fx_ctx *c) {
    if (!c->blend) return 1;
    const VkPipelineColorBlendAttachmentState *b = vkn_blend_att(c->blend);
    return b && !b->blendEnable && (b->colorWriteMask & 0xfu) == 0xfu;
}
static vkn_tex *tex_at(const vkn_draw_in *in, int st, uint32_t slot, VkImageView *v) {
    if (v) *v = VK_NULL_HANDLE;
    if (st < 0 || st >= VKN_STAGE_SLOTS || !in->tex[st] || slot >= VKN_TEX_SLOTS || in->tex[st][slot] < 0) return NULL;
    vkn_tex *t = NULL; const VkImageView vw = vkn_tex_view_by_id((uint32_t)in->tex[st][slot], &t);
    if (v) *v = vw;
    return vw ? t : NULL;
}
static VkSampler smp_at(const vkn_draw_in *in, int st, uint32_t slot) {
    if (st < 0 || st >= VKN_STAGE_SLOTS || !in->smp[st] || slot >= VKN_TEX_SLOTS || in->smp[st][slot] < 0) return VK_NULL_HANDLE;
    return vkn_sampler_by_id((uint32_t)in->smp[st][slot]);
}
static int st_of(uint8_t engine_stage) { return engine_stage == 0 ? 0 : engine_stage == 1 ? 1 : -1; }
// The texture bindings of s that read t (bit = binding index in s->bind, first 64). sample slots of graphics stages only.
static uint64_t reads_of(const vkn_shader *s, const vkn_draw_in *in, const vkn_tex *t) {
    uint64_t m = 0;
    if (!t) return 0;
    for (uint32_t i = 0; i < s->nbind && i < 64; i++) {
        const vkn_bind *b = &s->bind[i];
        if (b->kind != VKN_BK_TEXTURE) continue;
        if (tex_at(in, st_of(b->stage), b->slot, NULL) == t) m |= 1ull << i;
    }
    return m;
}
static int same_shape(const vkn_tex *a, const vkn_tex *b) { return a && b && a->w == b->w && a->h == b->h && a->format == b->format && a->mips == b->mips && a->layers == b->layers && a->samples == b->samples; }
static void cover_of(const vkn_draw_in *in, const vkn_tex *t, int32_t r[4]) { vkn_post_cover(in->viewport, in->scissor, t->w, t->h, r); }
static int rect_whole(const int32_t r[4], const vkn_tex *t) { return r[0] <= 0 && r[1] <= 0 && r[2] >= (int32_t)t->w && r[3] >= (int32_t)t->h; }
// [VK-LOWRES] r (x0, y0, x1, y1) covers every texel of the non-empty rect a
static int rect_covers(const int32_t r[4], const int32_t a[4]) { return a[0] < a[2] && a[1] < a[3] && r[0] <= a[0] && r[1] <= a[1] && r[2] >= a[2] && r[3] >= a[3]; }
static void rect_whole_of(const vkn_tex *t, int32_t r[4]) { r[0] = 0; r[1] = 0; r[2] = (int32_t)t->w; r[3] = (int32_t)t->h; }
static const uint8_t *ring_at(int st, uint32_t slot, uint32_t *size) {
    uint32_t off = 0, sz = 0;
    if (vkn_draw_ubo_get(st, slot, &off, &sz)) return NULL;
    const uint8_t *base = (const uint8_t *)vkn_ring_cpu();
    if (size) *size = sz;
    return base ? base + off : NULL;
}
// A requirement of the bundle (pass 0 host / 1 skipped) against the current draw; O = the skipped draw's target. 1 = holds.
static int req_ok(const nb_fx_req *r, const vkn_draw_in *in, const vkn_tex *O) {
    if (r->kind == NB_FX_R_SAME_SIZE) return 1;   // checked by the callers (they know the textures)
    uint32_t n = 0; const uint8_t *u = ring_at(r->stage, r->slot, &n);
    if (r->kind == NB_FX_R_UBO_EQ) return nb_fx_req_ubo_ok(r, u, u ? n : 0);
    if (r->kind == NB_FX_R_VP_MAP) return nb_fx_req_vp_ok(r, u, u ? n : 0, in->viewport, O ? O->w : 0, O ? O->h : 0, NULL);
    return 1;
}

// ================================================================================================ replay (the skipped draw drawn now)
static int materialize_alias(uint32_t i);
static void stash_drop(stash *s) {
    if (!s->valid) return;
    for (uint32_t i = 0; i < s->n; i++) if (s->color[i]) vkn_tex_unref(s->color[i]);
    if (s->depth) vkn_tex_unref(s->depth);
    if (s->O) vkn_tex_unref(s->O);
    memset(s, 0, sizeof *s);
}
static int replay(stash *s, const vkn_fx_ctx *outer) {
    if (!s->valid || s->serial != vkn_cb_serial()) { count(R_SERIAL); return -1; }
    for (uint32_t i = 0; i < g.nw;) {   // an alias the skipped draw reads (Vignette's input) gets its content back first: the replay samples it
        if (g.w[i].kind == W_ALIAS && s->eshader && reads_of(s->eshader, &s->d.in, g.w[i].t) && !materialize_alias(i)) continue;
        i++;
    }
    // vkn_draw.c prepare reads its g_call (indexed ? 0 : first vertex) again after the chain (the vertex fold): the replay overwrites it
    // with its own; counted when the outer draw's value differs (the lead is asked to save / restore g_call around vkn_fx_run)
    if (outer && (outer->indexed ? 0u : outer->first) != (s->d.indexed ? 0u : s->d.first)) count(R_REPLAY_FIRST);
    uint32_t n0 = 0; vkn_tex *c0[VKN_PASS_COLORS]; uint32_t l0[VKN_PASS_COLORS], m0[VKN_PASS_COLORS]; vkn_tex *d0 = NULL;
    vkn_pass_cur_get(&n0, c0, l0, m0, &d0);
    int held[VKN_PASS_COLORS + 1] = {0};
    for (uint32_t i = 0; i < n0 && i < VKN_PASS_COLORS; i++) held[i] = c0[i] ? vkn_tex_ref(c0[i]) : 0;
    held[VKN_PASS_COLORS] = d0 ? vkn_tex_ref(d0) : 0;
    struct { uint32_t off, size; int valid; } saved[24];
    for (uint32_t i = 0; i < s->nub; i++) { saved[i].valid = !vkn_draw_ubo_get(s->ub[i].st, s->ub[i].slot, &saved[i].off, &saved[i].size); }
    vkn_fx_bypass(1);
    for (uint32_t i = 0; i < s->nub; i++) vkn_draw_ubo(s->ub[i].st, s->ub[i].slot, s->ub[i].off, s->ub[i].size);
    vkn_pass_targets(s->n, s->color, s->layer, s->mip, s->depth);
    dcopy *d = &s->d;
    const int rc = d->indexed ? vkn_draw_indexed(&d->in, d->first, d->count, d->inst) : vkn_draw_arrays(&d->in, d->first, d->count);
    vkn_pass_targets(n0, c0, l0, m0, d0);
    for (uint32_t i = 0; i < s->nub; i++) if (saved[i].valid) vkn_draw_ubo(s->ub[i].st, s->ub[i].slot, saved[i].off, saved[i].size);
    vkn_fx_bypass(0);
    for (uint32_t i = 0; i < n0 && i < VKN_PASS_COLORS; i++) if (held[i]) vkn_tex_unref(c0[i]);
    if (held[VKN_PASS_COLORS]) vkn_tex_unref(d0);
    s->skipped = 0;
    return rc;
}

// ================================================================================================ watches
static void watch_end(uint32_t i) {
    watch *w = &g.w[i];
    if (w->t) vkn_tex_unref(w->t);
    if (w->holder) vkn_tex_unref(w->holder);
    g.w[i] = g.w[--g.nw]; memset(&g.w[g.nw], 0, sizeof g.w[0]);
}
static int watch_find(const vkn_tex *t) { for (uint32_t i = 0; i < g.nw; i++) if (g.w[i].t == t) return (int)i; return -1; }
static void disarm(int pair, uint64_t frame) { if (pair >= 0 && pair < PR_N) vkn_post_arm_note(&g.arm[pair], frame, 0); }
// A failure on target O of pair p (an E target): it disarms only when O is the pair's tracked target (or none is tracked yet).
static int related(int p, const vkn_tex *O) { return p < 0 || p > 1 || !g.armO[p] || g.armO[p] == O; }
static void disarm_on(int p, const vkn_tex *O, uint64_t frame) { if (related(p, O)) { disarm(p, frame); if (p == PR_F1) disarm(PR_RA, frame); } }
// Puts t under watch (kind, owner pair); the layout sentinel is set (vkn_pass_layout: pending clears of t run first). -1 = no room.
static int watch_put(vkn_tex *t, int kind, int pair, vkn_tex *holder, stash *st, const int32_t rect[4]) {
    int i = watch_find(t);
    if (i < 0) {
        if (g.nw >= 4) { count(R_WATCH_FULL); return -1; }
        if (!vkn_tex_ref(t)) return -1;
        i = (int)g.nw++; memset(&g.w[i], 0, sizeof g.w[i]); g.w[i].t = t;
    } else if (g.w[i].holder) { vkn_tex_unref(g.w[i].holder); g.w[i].holder = NULL; }
    watch *w = &g.w[i];
    w->kind = (uint8_t)kind; w->pair = (uint8_t)pair; w->bad = 0; w->st = st; w->serial = vkn_cb_serial();
    if (rect) memcpy(w->rect, rect, sizeof w->rect); else rect_whole_of(t, w->rect);
    w->clr_ns = t->pass_nclr ? t->pass_clr_ns : 0;   // [mgpu P4] a clear remembered BEFORE the skip is not one after it
    if (holder && vkn_tex_ref(holder)) w->holder = holder;
    // (already GENERAL: a clear the engine asked for since stays remembered - the skipped draw would have overwritten it - and costs nothing)
    if (t->layout != VK_IMAGE_LAYOUT_GENERAL) vkn_pass_layout(t, VK_IMAGE_LAYOUT_GENERAL);
    return i;
}
static int materialize_alias(uint32_t i) {
    watch *w = &g.w[i];
    if (w->kind != W_ALIAS || !w->holder || !same_shape(w->t, w->holder)) return -1;
    vkn_tex *t = w->t, *h = w->holder;
    if (!vkn_tex_ref(t)) return -1;
    if (!vkn_tex_ref(h)) { vkn_tex_unref(t); return -1; }
    watch_end(i);
    vkn_pass_copy(t, h); g.s.materialized++;
    vkn_tex_unref(t); vkn_tex_unref(h);
    return 0;
}
// The content a watched image should hold is put into it now: ALIAS -> copy of the holder, STALE -> replay of its stash. 0 = done.
static int materialize(uint32_t i, const vkn_fx_ctx *outer) {
    watch *w = &g.w[i];
    if (w->kind == W_ALIAS) return materialize_alias(i);
    if (w->kind == W_STALE && w->st && w->st->valid && w->st->O == w->t && w->serial == vkn_cb_serial()) {
        stash *s = w->st; watch_end(i);
        const int rc = replay(s, outer); g.s.materialized++;
        return rc ? -1 : 0;
    }
    return -1;
}
static void violation(uint32_t i, int why, const vkn_fx_ctx *c, const char *what) {
    watch *w = &g.w[i];
    if (w->kind == W_PROBE) {   // probation: nothing was skipped, the shape of this window is bad (counted, not a violation)
        count(why);
        disarm(w->pair, c ? c->frame : vkn_frame()); if (w->pair == PR_F1) disarm(PR_RA, c ? c->frame : vkn_frame());
        watch_end(i); return;
    }
    g.s.violations++; count(why);
    red(why, "%s (image %s %ux%u, %s, layout %d) at frame %llu", what, w->t->sname[0] ? w->t->sname : "?", w->t->w, w->t->h,
        w->kind == W_ALIAS ? "alias" : "stale", (int)w->t->layout, (unsigned long long)(c ? c->frame : vkn_frame()));
    disarm(w->pair, c ? c->frame : vkn_frame());
    if (w->pair == PR_F1) disarm(PR_RA, c ? c->frame : vkn_frame());
    watch_end(i);
}
// [mgpu P4] the engine cleared the watched image whole AFTER its watch began (a remembered clear of its only level / layer, newer than the
// watch): what the skipped draw / the alias would have left there is overwritten by that clear anyway - nothing to materialise; the
// watch ends (was: the content was copied / replayed first, a full-screen copy a frame in U02h, and it landed after the flushed clear).
static int cleared_since(const watch *w) {
    const vkn_tex *t = w->t;
    if (w->kind == W_PROBE || t->mips != 1 || t->layers != 1 || !t->pass_nclr || t->pass_clr_ns <= w->clr_ns) return 0;
    for (uint32_t i = 0; i < t->pass_nclr; i++) if (t->pass_clr[i].layer == 0) return 1;
    return 0;
}
// Every draw while a watch is live: a use the module did not see (the layout moved) and a write into a watched image / an alias holder.
static void watch_cheap(const vkn_fx_ctx *c, int post) {
    for (uint32_t i = 0; i < g.nw;) {
        watch *w = &g.w[i];
        if (w->t->dead) { watch_end(i); continue; }
        const int marked = w->kind != W_PROBE || w->serial;   // a probe gets its sentinel only after its host drew
        if (marked && w->t->layout != VK_IMAGE_LAYOUT_GENERAL) { violation(i, R_UNSEEN_USE, c, "its layout changed"); continue; }
        int hit = 0, hold = 0;
        for (int k = 0; k < VKN_PASS_COLORS && c->color[k]; k++) { hit |= c->color[k] == w->t; hold |= w->holder && c->color[k] == w->holder; }
        hit |= c->depth == w->t;
        if ((hit || hold) && cleared_since(w)) { g.s.cleared++; watch_end(i); continue; }   // [mgpu P4]
        if (!post && (hit || hold)) {   // a draw of another class writes it: the content first, then the watch ends
            count(R_OTHER_WRITE);
            if (w->kind == W_PROBE || materialize(i, c)) violation(i, R_OTHER_WRITE, c, "written by a draw of another class");
            continue;
        }
        i++;
    }
}

// ================================================================================================ fused shaders and twins
static uint8_t kind_of(uint32_t src) {
    switch (src) { case XBG_SOURCE_UNIFORM: return VKN_BK_UNIFORM; case XBG_SOURCE_SUPPORT: return VKN_BK_SUPPORT; case XBG_SOURCE_C1: return VKN_BK_C1;
                   case XBG_SOURCE_TEXTURE: return VKN_BK_TEXTURE; default: return VKN_BK_OTHER; }
}
static int cmp_bind(const void *a, const void *b) { const vkn_bind *x = (const vkn_bind *)a, *y = (const vkn_bind *)b; return (int)x->binding - (int)y->binding; }
// The effect table: types fixed (engine UNIFORM blocks dynamic while the device allows), effect kinds checked. 0 = ok.
static int finish_table(vkn_bind *tb, uint32_t n) {
    const vkn_dev *d = vkn_device();
    uint32_t nu = 0; for (uint32_t i = 0; i < n; i++) nu += tb[i].kind == VKN_BK_UNIFORM;
    const int dyn = d && nu <= d->props.limits.maxDescriptorSetUniformBuffersDynamic;
    for (uint32_t i = 0; i < n; i++) {
        vkn_bind *t = &tb[i];
        if (t->count != 1) return -1;
        if (t->kind == VKN_BK_UNIFORM || t->kind == VKN_BK_SUPPORT || t->kind == VKN_BK_FX_UBO)
            t->type = t->kind == VKN_BK_UNIFORM && dyn ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else if (t->kind == VKN_BK_TEXTURE || t->kind == VKN_BK_FX_TEX) { if (t->type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) return -1; }
        else return -1;
        t->dyn = VKN_NO_DYN; t->flags &= ~VKN_BF_CMP;
    }
    qsort(tb, n, sizeof *tb, cmp_bind);
    for (uint32_t i = 1; i < n; i++) if (tb[i].binding == tb[i - 1].binding) return -1;
    return 0;
}
// A host binding of `s` (one of its own stages) into the effect table: C1 becomes an effect block fed from s->c1.
static int add_feed_ubo(uint8_t *ufeed, uint8_t *uarg, int *nu, int feed, int arg) {
    if (*nu >= VKN_FX_UBO) return -1;
    ufeed[*nu] = (uint8_t)feed; uarg[*nu] = (uint8_t)arg; return (*nu)++;
}
static const char *k_fz_name[FK_N] = { "F1", "F1A", "F2" };
static const char *k_luma_name[3] = { "F1A", "F1AS", "F1AR" };
static void fused_build(fused *f, int kind, const vkn_shader *host, int base_slot) {
    f->state = -1; f->host = host; f->host_id = host->id; f->base_slot = base_slot;
    memcpy(f->host_vs, host->key[VKN_SRC_VS], 17); memcpy(f->host_fs, host->key[VKN_SRC_FS], 17);
    snprintf(f->key, sizeof f->key, "%s.%s", kind == FK_F1A ? k_luma_name[g_luma] : k_fz_name[kind], host->key[VKN_SRC_FS]);
    xbg_vkn_stage *st = (xbg_vkn_stage *)calloc(1, sizeof *st);
    vkn_bind *tb = (vkn_bind *)calloc(320, sizeof *tb);
    char why[128] = "";
    const uint64_t t0 = vkn_now_ns();
    int rc = !st || !tb ? -1 : g_test_load ? g_test_load(f->key, st, &f->fx) : vkn_shlib_shader ? vkn_shlib_shader(f->key, NULL, NULL, st) : -1;
    if (rc || !st->module || st->stage != 4) { snprintf(why, sizeof why, "the library did not load it (rc %d%s)", rc, rc == 0 ? ", no module" : ""); goto fail; }
    if (!g_test_load && (!vkn_post_meta || vkn_post_meta(f->key, &f->fx, why, sizeof why))) { if (!why[0]) snprintf(why, sizeof why, "no bundle table"); goto fail; }
    if (strncmp(f->fx.host_key, host->key[VKN_SRC_FS], 16)) { snprintf(why, sizeof why, "host key differs"); goto fail; }
    uint32_t n = 0; int nt = 0, nu = 0;
    for (uint32_t i = 0; i < host->nbind && n < 320; i++) {   // the host's vertex stage, as its own table has it
        const vkn_bind *b = &host->bind[i];
        if (b->bundle_stage != 0) continue;
        vkn_bind t = *b;
        if (b->kind == VKN_BK_C1) { const int u = add_feed_ubo(f->ufeed, f->uarg, &nu, FD_C1_VS, VKN_SRC_VS); if (u < 0) { snprintf(why, sizeof why, "effect blocks"); goto fail; } t.kind = VKN_BK_FX_UBO; t.slot = (uint8_t)u; }
        tb[n++] = t;
    }
    for (uint32_t i = 0; i < st->binding_count && n < 320; i++) {   // the fused fragment stage (the library's table: extras on spare slots)
        const xbg_shader_binding *b = &st->bindings[i];
        vkn_bind t; memset(&t, 0, sizeof t);
        t.binding = (uint16_t)b->binding; t.stage = 1; t.bundle_stage = 4; t.count = (uint8_t)(b->count ? b->count : 1);
        t.type = (VkDescriptorType)b->type; t.bytes = b->bytes; t.flags = b->flags; t.slot = (uint8_t)b->slot;
        int ex = -1;
        for (uint32_t j = 0; j < f->fx.n_extra; j++) if (f->fx.extra[j].vk_binding == b->binding) ex = (int)j;
        if (ex >= 0) {
            if (f->fx.extra[ex].src_kind == NB_FX_K_TEX) {
                if (nt >= VKN_FX_TEX) { snprintf(why, sizeof why, "effect textures"); goto fail; }
                f->tfeed[nt] = FD_EXTRA; f->targ[nt] = (uint8_t)ex; t.kind = VKN_BK_FX_TEX; t.slot = (uint8_t)nt++;
            } else {
                const int u = add_feed_ubo(f->ufeed, f->uarg, &nu, FD_EXTRA, ex); if (u < 0) { snprintf(why, sizeof why, "effect blocks"); goto fail; }
                t.kind = VKN_BK_FX_UBO; t.slot = (uint8_t)u;
            }
        } else {
            t.kind = kind_of(b->source);
            if (t.kind == VKN_BK_C1) { const int u = add_feed_ubo(f->ufeed, f->uarg, &nu, FD_C1_FS, VKN_SRC_FS); if (u < 0) { snprintf(why, sizeof why, "effect blocks"); goto fail; } t.kind = VKN_BK_FX_UBO; t.slot = (uint8_t)u; }
            else if (t.kind == VKN_BK_TEXTURE && (int)b->slot == base_slot) {   // the skipped target's binding: the fused code does not read it
                if (nt >= VKN_FX_TEX) { snprintf(why, sizeof why, "effect textures"); goto fail; }
                f->tfeed[nt] = FD_DUMMY; f->targ[nt] = 0; t.kind = VKN_BK_FX_TEX; t.slot = (uint8_t)nt++;
            } else if (t.kind == VKN_BK_OTHER) { snprintf(why, sizeof why, "binding %u of source %u", b->binding, b->source); goto fail; }
        }
        tb[n++] = t;
    }
    if (finish_table(tb, n)) { snprintf(why, sizeof why, "binding table (count / type / duplicate)"); goto fail; }
    {
        vkn_fx_shader_src src; memset(&src, 0, sizeof src);
        src.name = f->key; src.vs = host->module[VKN_SRC_VS]; src.fs = st->module; src.bind = tb; src.nbind = n;
        src.ninputs = host->ninputs; memcpy(src.inputs, host->inputs, sizeof src.inputs); memcpy(src.input_classes, host->input_classes, sizeof src.input_classes);
        f->sh = vkn_fx_shader_create(&src);
    }
    if (!f->sh) { snprintf(why, sizeof why, "vkn_fx_shader_create refused"); goto fail; }
    f->state = 1;
    vkn_logf("post: fused shader %s ready: %u bindings (%d effect textures, %d effect blocks), %u extras, %u requirements, flags %#x (%s), %.1f ms",
             f->key, n, nt, nu, f->fx.n_extra, f->fx.n_req, f->fx.flags, f->fx.flags & NB_FX_F_EXACT ? "exact" : "approximate by design", (vkn_now_ns() - t0) / 1e6);
    free(st); free(tb); return;
fail:
    count(R_BUILD);
    vkn_logf(RED "post: fused shader %s unusable: %s (that fusion stays off)", f->key, why);
    free(st); free(tb);
}
// The fused object for kind on this host (built at its first host draw, kept while the host keys are the same).
static fused *fused_get(int kind, const vkn_shader *host, int base_slot) {
    fused *f = &g.fz[kind];
    if (f->state && (strcmp(f->host_fs, host->key[VKN_SRC_FS]) || strcmp(f->host_vs, host->key[VKN_SRC_VS]))) {
        if (f->state > 0) vkn_logf("post: %s: the host keys changed (%s / %s): rebuilt", f->key, host->key[VKN_SRC_VS], host->key[VKN_SRC_FS]);
        memset(f, 0, sizeof *f);
    }
    if (!f->state) fused_build(f, kind, host, base_slot);
    return f->state > 0 ? f : NULL;
}
// A twin of reader r: the texture bindings in mask (indices of r->bind) read the effect textures 0.. instead of their engine slots.
static twin *twin_get(const vkn_shader *r, uint64_t mask) {
    for (uint32_t i = 0; i < g.ntw; i++) if (g.tw[i].r == r && g.tw[i].rid == r->id && g.tw[i].mask == mask) return g.tw[i].bad ? NULL : &g.tw[i];
    if (g.ntw >= 16) return NULL;
    twin *t = &g.tw[g.ntw++]; memset(t, 0, sizeof *t); t->r = r; t->rid = r->id; t->mask = mask; t->bad = 1;
    vkn_bind tb[64]; uint32_t n = 0; int nt = 0, nu = 0;
    if (r->nbind > 64) { count(R_BUILD); return NULL; }
    for (uint32_t i = 0; i < r->nbind; i++) {
        vkn_bind b = r->bind[i];
        if (mask >> i & 1) { if (nt >= VKN_FX_TEX) return NULL; t->tfeed[nt] = FD_EXTRA; t->targ[nt] = (uint8_t)i; b.kind = VKN_BK_FX_TEX; b.slot = (uint8_t)nt++; }
        else if (b.kind == VKN_BK_C1) {
            const int u = add_feed_ubo(t->ufeed, t->uarg, &nu, b.bundle_stage == 0 ? FD_C1_VS : FD_C1_FS, b.bundle_stage == 0 ? VKN_SRC_VS : VKN_SRC_FS);
            if (u < 0) return NULL;
            b.kind = VKN_BK_FX_UBO; b.slot = (uint8_t)u;
        }
        tb[n++] = b;
    }
    if (finish_table(tb, n)) { count(R_BUILD); return NULL; }
    char nm[40]; snprintf(nm, sizeof nm, "alias.%.16s", r->key[VKN_SRC_FS]);
    vkn_fx_shader_src src; memset(&src, 0, sizeof src);
    src.name = nm; src.vs = vkn_shader_module(r, VKN_STAGE_VS, 0, 0.0f, 0); src.fs = vkn_shader_module(r, VKN_STAGE_FS, 0, 0.0f, 0);
    src.bind = tb; src.nbind = n; src.ninputs = r->ninputs; memcpy(src.inputs, r->inputs, sizeof src.inputs); memcpy(src.input_classes, r->input_classes, sizeof src.input_classes);
    if (!src.vs || !(r->stages & (1u << VKN_SRC_FS)) || !src.fs) { count(R_BUILD); return NULL; }
    t->sh = vkn_fx_shader_create(&src);
    if (!t->sh) { count(R_BUILD); return NULL; }
    t->bad = 0; g.s.twins++;
    return t;
}
// F1A is drawn from F1's stash: the same extras (kind, stage, slot) in the same order (xbg_nb_post.inc fuse "same")
static int extras_same(const nb_fx *a, const nb_fx *b) {
    if (a->n_extra != b->n_extra) return 0;
    for (uint32_t i = 0; i < a->n_extra; i++) {
        const nb_fx_extra *p = &a->extra[i], *q = &b->extra[i];
        if (p->src_kind != q->src_kind || p->src_stage != q->src_stage || p->src_slot != q->src_slot || p->src_pass != q->src_pass) return 0;
    }
    return 1;
}
static void feed_c1(vkn_fx_ubo *u, const vkn_shader *s, int src) { const vkn_block *b = &s->c1[src]; u->buf = b->buf; u->off = b->off; u->size = (uint32_t)b->size; }

// ================================================================================================ stash (E)
static void stash_draw(stash *s, const vkn_draw_in *in, const vkn_fx_ctx *c) {
    dcopy *d = &s->d; d->in = *in;
    for (int k = 0; k < VKN_STAGE_SLOTS; k++) {
        if (in->tex[k]) { memcpy(d->tex[k], in->tex[k], sizeof d->tex[k]); d->in.tex[k] = d->tex[k]; }
        if (in->smp[k]) { memcpy(d->smp[k], in->smp[k], sizeof d->smp[k]); d->in.smp[k] = d->smp[k]; }
    }
    d->indexed = c->indexed; d->first = c->first; d->count = c->count; d->inst = c->instances ? c->instances : 1;
    vkn_tex *col[VKN_PASS_COLORS]; vkn_tex *dep = NULL; uint32_t n = 0;
    vkn_pass_cur_get(&n, col, s->layer, s->mip, &dep);
    s->n = n > VKN_PASS_COLORS ? VKN_PASS_COLORS : n;
    for (uint32_t i = 0; i < s->n; i++) s->color[i] = col[i] && vkn_tex_ref(col[i]) ? col[i] : NULL;
    s->depth = dep && vkn_tex_ref(dep) ? dep : NULL;
    s->nub = 0;
    const vkn_shader *e = c->shader;
    for (uint32_t i = 0; i < e->nbind && s->nub < 24; i++) {
        const vkn_bind *b = &e->bind[i];
        if (b->kind != VKN_BK_UNIFORM) continue;
        const int st = st_of(b->stage); uint32_t off, size;
        if (st < 0 || vkn_draw_ubo_get(st, b->slot, &off, &size)) continue;
        s->ub[s->nub].st = (uint8_t)st; s->ub[s->nub].slot = b->slot; s->ub[s->nub].off = off; s->ub[s->nub].size = size; s->nub++;
    }
}
// The extras of the bundle from the skipped draw (xbg_nb_post.inc elide). why = R_* on failure.
static int stash_extras(stash *s, const nb_fx *fx, const vkn_draw_in *in, int *why) {
    s->first_tex = -1;
    for (uint32_t j = 0; j < fx->n_extra; j++) {
        const nb_fx_extra *e = &fx->extra[j]; const int st = e->src_stage;
        if (e->src_kind == NB_FX_K_TEX) {
            VkImageView v; vkn_tex *t = tex_at(in, st, e->src_slot, &v);
            if (!t) { *why = R_EXTRA; return -1; }
            const int a = watch_find(t);
            if (a >= 0 && g.w[a].kind == W_ALIAS && g.w[a].holder) { t = g.w[a].holder; v = t->view; g.s.redirected++; }   // Vignette's input is the alias: its content
            s->xt[j].view = v; s->xt[j].tex = t; s->xt[j].sampler = (e->flags & NB_FX_E_LINEAR) ? VK_NULL_HANDLE : smp_at(in, st, e->src_slot);
            if (s->first_tex < 0) s->first_tex = (int)j;
        } else if (e->src_kind == NB_FX_K_CBUF) {
            uint32_t off, size;
            if (vkn_draw_ubo_get(st, e->src_slot, &off, &size) || !size) { *why = R_EXTRA; return -1; }
            s->xu[j].buf = VK_NULL_HANDLE; s->xu[j].off = off; s->xu[j].size = size;
        } else {
            vkn_block b;
            if (vkn_shader_support(in->viewport[2], in->viewport[3] < 0 ? -in->viewport[3] : in->viewport[3], &b)) { *why = R_EXTRA; return -1; }
            s->xu[j].buf = b.buf; s->xu[j].off = b.off; s->xu[j].size = (uint32_t)b.size;
        }
    }
    if (s->first_tex < 0) { *why = R_EXTRA; return -1; }
    return 0;
}
// E of pair p (LUT / Vignette): stash and checks; skip when the pair is armed.
static void at_elided(int p, const vkn_draw_in *in, const vkn_fx_ctx *c, vkn_fx_out *out) {
    stash *s = &g.st[p]; stash_drop(s);
    // the window of the previous host: was the target used since (probation watch)?
    int win_ok = 1;
    if (c->color[0]) {
        const int i = watch_find(c->color[0]);
        if (i >= 0 && g.w[i].kind == W_PROBE) { win_ok = g.w[i].serial && c->color[0]->layout == VK_IMAGE_LAYOUT_GENERAL; watch_end((uint32_t)i); }
    }
    const fused *f = &g.fz[p == PR_F1 ? FK_F1 : FK_F2];
    vkn_tex *O = c->color[0];
    const uint32_t ser = vkn_cb_serial();
    int why = -1;
    if (in->alpha_ref >= 0) why = R_ALPHA;
    else if (!one_target(c)) why = R_RT;
    else if (g.lastE[p].serial == ser && g.lastE[p].O == O) why = R_EDRAWS;   // (the first one was drawn: recover_pending at this draw)
    else if (f->state <= 0) why = f->state < 0 ? R_BUNDLE : -1;   // not built yet: its first host draw builds it (probation)
    if (why < 0 && f->state > 0 && strncmp(c->shader->key[VKN_SRC_FS], f->fx.elided_key[0], 16)) why = R_EKEY;
    g.lastE[p].serial = ser; g.lastE[p].O = O;
    if (why >= 0) { count(why); disarm_on(p, O, c->frame); return; }
    s->valid = 1; s->pair = p; s->serial = vkn_cb_serial(); s->frame = c->frame; s->eshader = c->shader;
    s->O = vkn_tex_ref(O) ? O : NULL;
    if (!s->O) { s->valid = 0; return; }
    cover_of(in, O, s->rect); s->whole = rect_whole(s->rect, O);
    stash_draw(s, in, c);
    s->first_tex = -1;
    if (f->state > 0) {
        if (stash_extras(s, &f->fx, in, &why)) { count(why); disarm_on(p, O, c->frame); stash_drop(s); return; }
        for (uint32_t i = 0; i < f->fx.n_req; i++) {
            const nb_fx_req *r = &f->fx.req[i];
            if (r->pass != 1) continue;
            if (r->kind == NB_FX_R_SAME_SIZE) { const vkn_tex *t = s->xt[s->first_tex].tex; if (!t || t->w != O->w || t->h != O->h) why = R_SIZE_E; }
            else if (!req_ok(r, in, O)) why = R_REQ_E;
        }
        if (why >= 0) {
            count(why); disarm_on(p, O, c->frame);
            red(why, "%s: viewport %g %g %g %g, target %ux%u", f->key, in->viewport[0], in->viewport[1], in->viewport[2], in->viewport[3], O->w, O->h);
            stash_drop(s); return;
        }
    }
    s->skipped = 0;
    g.pend = p;
    if (!win_ok) disarm_on(p, O, c->frame);
    if (f->state > 0 && g.arm[p].armed && g.armO[p] == O && applying()) {
        if (watch_put(O, W_STALE, p, NULL, s, s->rect) < 0) { return; }
        if (g_verify == 2) {   // [VK-LOWRES] at a part only the part is poisoned: what lies outside it was never the skipped draw's
            static const float mg[4] = {1, 0, 1, 1};
            const int32_t pr[4] = {s->rect[0], s->rect[1], s->rect[2] - s->rect[0], s->rect[3] - s->rect[1]};
            vkn_pass_clear_color(O, 0, mg, s->whole ? NULL : pr); vkn_pass_layout(O, VK_IMAGE_LAYOUT_GENERAL);
        }
        out->skip = 1; s->skipped = 1; g.s.skipped++;
    } else if (f->state > 0 && g.arm[p].armed && g.armO[p] == O) g.s.would_skip++;
}
// H of pair p: the fused draw (or the observation in probation / verify).
static void at_host(int p, int cls, const vkn_draw_in *in, const vkn_fx_ctx *c, vkn_fx_out *out) {
    stash *s = &g.st[p];
    if (g.pend != p || !s->valid || s->serial != vkn_cb_serial()) {
        count(p == PR_F2 && g.pend < 0 && g.lastE[PR_F2].serial != vkn_cb_serial() ? R_NOVIG : R_NOHOST);   // [mgpu P4] F2's refusal by its true reason
        disarm_on(p, NULL, c->frame); return;
    }
    g.pend = -1;
    vkn_tex *O = s->O;
    // which binding samples O
    int base = -1, nread = 0;
    for (uint32_t i = 0; i < c->shader->nbind; i++) {
        const vkn_bind *b = &c->shader->bind[i];
        if (b->kind != VKN_BK_TEXTURE) continue;
        if (tex_at(in, st_of(b->stage), b->slot, NULL) == O) { nread++; if (b->stage == 1 && base < 0) base = b->slot; }
    }
    int why = -1;
    if (in->alpha_ref >= 0) why = R_ALPHA;
    else if (!nread || base < 0) why = R_NOREAD;
    else if (nread > 1) why = R_BASE;
    for (int k = 0; why < 0 && k < VKN_PASS_COLORS && c->color[k]; k++) if (c->color[k] == O) why = R_HOSTWRITE;
    if (why < 0 && p == PR_F1 && (!c->color[0] || c->color[0]->w != O->w || c->color[0]->h != O->h)) why = R_HSIZE;
    const int kind = p == PR_F1 ? FK_F1 : FK_F2;
    fused *f = why < 0 ? fused_get(kind, c->shader, base) : NULL;
    if (why < 0 && !f) why = R_BUNDLE;
    if (why < 0 && f->base_slot != base) why = R_BASE;
    if (why < 0 && strncmp(c->shader->key[VKN_SRC_FS], f->fx.host_key, 16)) why = R_HKEY;
    int first_build = 0;
    if (why < 0 && s->first_tex < 0) {   // the bundle was built only now (first host): the stash has no extras yet - the next frame
        why = R_BUNDLE; first_build = 1;   // [stage 5 rest] by design once per bundle: counted, not a red line
    }
    int32_t hr[4] = {0, 0, 0, 0};
    if (why < 0) {
        for (uint32_t i = 0; i < f->fx.n_req && why < 0; i++) { const nb_fx_req *r = &f->fx.req[i]; if (r->pass == 0 && !req_ok(r, in, O)) why = R_REQ_H; }
        vkn_post_cover(in->viewport, in->scissor, O->w, O->h, hr);
        if (why < 0 && hr[0] < hr[2] && hr[1] < hr[3] && (hr[0] < s->rect[0] || hr[1] < s->rect[1] || hr[2] > s->rect[2] || hr[3] > s->rect[3])) why = R_HCOVER;
    }
    // route A (F1A): armed shape, both draws cover their whole targets, the F1A bundle usable with the same extras
    fused *fa = NULL; int ra = 0, la_ok = 0, la_part = 0;
    if (why < 0 && p == PR_F1 && g_aa && g.level_ra) {
        const int whole = s->whole && c->color[0] && rect_whole(hr, c->color[0]);
        // [VK-LOWRES] a lower 3D scale: the chain draws the top-left part and the AA step stretches it over the whole target. Route A
        // can't skip that step (it is the upscale), but when it is the FSR stretch with its FXAA pre pass (DM_VK_NATIVE_FSR_AA=1) F1A
        // draws that FXAA on the part and the pre pass is left out (at_step). Any other AA step at a part: route A stays off (R_PART).
        const int part_ok = !whole && g_part && c->color[0] && hr[0] == 0 && hr[1] == 0 && hr[2] > hr[0] && hr[3] > hr[1] && vkn_fsr_route_ok();
        if (!whole && !part_ok) count(R_PART);
        else {
            la_ok = 1; la_part = !whole;
            if ((fa = fused_get(FK_F1A, c->shader, base)) && extras_same(&fa->fx, &f->fx)) ra = g.arm[PR_RA].armed && (whole || g_part == 1);
            else fa = NULL;
        }
    }
    if (why >= 0) {
        count(why);
        if ((why != R_BUNDLE || f) && !first_build) red(why, "%s host %s: viewport %g %g %g %g, skipped target %ux%u", k_fz_name[kind], c->shader->key[VKN_SRC_FS], in->viewport[0], in->viewport[1], in->viewport[2], in->viewport[3], O->w, O->h);
        disarm_on(p, O, c->frame);
        if (s->skipped) {   // the skipped draw is drawn now (recovery), then the host as recorded
            const int w = watch_find(O); if (w >= 0) watch_end((uint32_t)w);
            if (!replay(s, c)) g.s.recovered++;
        }
        stash_drop(s);
        return;
    }
    if (g.armO[p] != O) { g.arm[p].good = 0; g.arm[p].armed = 0; g.armO[p] = O; }   // the shape moved to another target: earn it again
    vkn_post_arm_note(&g.arm[p], c->frame, 1);
    if (p == PR_F1 && c->color[0] && la_ok) {   // the route-A observation starts at this host ([VK-LOWRES] only where route A can be)
        g.la.on = 1; g.la.real = 0; g.la.part = la_part; g.la.serial = vkn_cb_serial(); g.la.S = c->color[0]; g.la.cur = c->color[0]; memcpy(g.la.rect, hr, sizeof hr); g.la.ncarry = 0;
    }
    if (s->skipped) {
        const fused *use = ra && fa ? fa : f;
        out->shader = use->sh;
        for (int i = 0; i < VKN_FX_TEX; i++) {
            if (use->tfeed[i] == FD_EXTRA) { out->tex[i] = s->xt[use->targ[i]]; if (use->fx.extra[use->targ[i]].flags & NB_FX_E_LINEAR) out->tex[i].sampler = VK_NULL_HANDLE; }
            else if (use->tfeed[i] == FD_DUMMY) out->tex[i] = s->xt[s->first_tex];
        }
        for (int i = 0; i < VKN_FX_UBO; i++) {
            if (use->ufeed[i] == FD_EXTRA) out->ubo[i] = s->xu[use->uarg[i]];
            else if (use->ufeed[i] == FD_C1_VS || use->ufeed[i] == FD_C1_FS) feed_c1(&out->ubo[i], c->shader, use->uarg[i]);
        }
        g.s.fused[use == fa ? 1 : p == PR_F1 ? 0 : 2]++;
        if (use == fa) { g.la.real = 1; if (la_part) g.s.f1a_part++; }
        (void)cls;
    } else {
        g.s.would_fuse++;
        g.probe.O = O; g.probe.pair = p; g.probe.ht = c->color[0];   // probation: the window of O starts after this draw (post_fx puts the probe)
        memcpy(g.probe.rect, s->rect, sizeof g.probe.rect);          // [VK-LOWRES] what a skip would leave stale: the skipped draw's cover
    }
    if (!s->skipped) stash_drop(s);
}

// ================================================================================================ route A: carry and step
static void la_fail(int why, const vkn_fx_ctx *c) {
    if (!g.la.on) return;
    count(why);
    if (g.la.real) { g.s.late_aa++; count(R_LATE_STEP); red(why, "after F1A was drawn at frame %llu: the console's FXAA runs over it this frame", (unsigned long long)c->frame); }
    disarm(PR_RA, c->frame);
    g.la.on = 0;
}
static void at_carry(int cls, const vkn_draw_in *in, const vkn_fx_ctx *c) {
    if (!g.la.on || g.la.serial != vkn_cb_serial()) return;
    if (!reads_of(c->shader, in, g.la.cur)) return;
    if (cls == VKN_PC_OUTLINE && g.level < VKN_POST_OUTLINE) { la_fail(R_OTHER_READER, c); return; }
    vkn_tex *T = c->color[0]; int32_t r[4] = {0, 0, 0, 0};
    if (T) cover_of(in, T, r);
    if (in->alpha_ref >= 0 || !one_target(c) || T == g.la.cur || reads_of(c->shader, in, T) || !same_shape(T, g.la.cur) || !blend_off(c) || g.la.ncarry >= NBPA_CARRY ||
        memcmp(r, g.la.rect, sizeof r)) { la_fail(R_CARRY, c); return; }
    g.la.cur = T; g.la.ncarry++;
    if (g.la.real) g.s.carried++;
}
static void at_step(int cls, const vkn_draw_in *in, const vkn_fx_ctx *c, vkn_fx_out *out) {
    if (!g.la.on || g.la.serial != vkn_cb_serial()) return;
    const uint64_t rd = reads_of(c->shader, in, g.la.cur);
    if (!rd) return;   // an FXAA pass that reads nothing of the fused output (the HUD portrait's): not the step (NB-18b-9)
    int why = -1; vkn_tex *X = c->color[0]; int32_t r[4] = {0, 0, 0, 0};
    int ntex = 0; for (uint32_t i = 0; i < c->shader->nbind; i++) ntex += c->shader->bind[i].kind == VKN_BK_TEXTURE;
    if (cls == VKN_PC_COPY) why = R_STEP_COPY;
    else if (in->alpha_ref >= 0 || !one_target(c)) why = R_STEP_SHAPE;
    else if (ntex != 1 || X == g.la.cur) why = R_STEP_INPUT;
    else if (!same_shape(X, g.la.cur)) why = R_STEP_SIZE;
    else if (g.la.part) {   // [VK-LOWRES] F1A on a part: the step must be the FSR stretch over the whole target that reads only that part
        uint32_t lim[2] = {0, 0};
        cover_of(in, X, r);
        if (!rect_whole(r, X) || !vkn_fsr_stretch_part(in, c, lim)) why = R_STEP_STRETCH;
        else if (g.la.rect[0] > 0 || g.la.rect[1] > 0 || g.la.rect[2] < (int32_t)lim[0] || g.la.rect[3] < (int32_t)lim[1]) why = R_STEP_PARTREAD;
    }
    else if ((cover_of(in, X, r), memcmp(r, g.la.rect, sizeof r)) || !rect_whole(r, X)) why = R_STEP_COVER;
    else if (X->shared && X->nnames > 1) why = R_STEP_SHARED;
    if (why >= 0) { la_fail(why, c); return; }
    vkn_post_arm_note(&g.arm[PR_RA], c->frame, 1);
    if (g.la.part) {   // the step draws (EASU of the fused output, the upscale); its FXAA pre pass is what F1A already drew
        if (g.la.real && applying() && g_part == 1) { vkn_fsr_pre_skip(c); g.s.stretch_pre_skipped++; }
        else if (g.arm[PR_RA].armed || g_part == 2) g.s.stretch_would++;
        g.la.on = 0;
        return;
    }
    if (g.la.real && applying()) {
        if (watch_put(X, W_ALIAS, PR_RA, g.la.cur, NULL, NULL) < 0) { la_fail(R_WATCH_FULL, c); return; }
        if (g_verify == 2) { static const float mg[4] = {1, 0, 1, 1}; vkn_pass_clear_color(X, 0, mg, NULL); vkn_pass_layout(X, VK_IMAGE_LAYOUT_GENERAL); }
        out->skip = 1; g.s.step_skipped++;
    } else if (g.arm[PR_RA].armed) g.s.step_would++;
    g.la.on = 0;
}

// ================================================================================================ watched images at a post draw
// Reads of watched images by this (post-class, not skipped) draw: alias -> redirect / materialise; stale -> replay / violation.
static void watch_reads(const vkn_draw_in *in, const vkn_fx_ctx *c, vkn_fx_out *out) {
    for (uint32_t i = 0; i < g.nw;) {
        watch *w = &g.w[i];
        uint64_t m = reads_of(c->shader, in, w->t);
        if (m && out->shader && vkn_fx_shader_is(out->shader))   // a fused host: the skipped target's slot is fed by the stash already
            for (int p = 0; p < 2; p++) if (g.st[p].valid && g.st[p].skipped && g.st[p].O == w->t) m = 0;
        if (!m) { i++; continue; }
        if (cleared_since(w)) { g.s.cleared++; watch_end(i); continue; }   // [mgpu P4] the read sees the engine's clear (run before the sample)
        if (w->kind == W_PROBE) { violation(i, R_OTHER_READER, c, "read in the window"); continue; }
        if (w->kind == W_ALIAS && w->holder) {
            twin *t = !out->shader && in->alpha_ref < 0 ? twin_get(c->shader, m) : NULL;
            if (t && applying()) {
                out->shader = t->sh;
                for (int k = 0; k < VKN_FX_TEX; k++) if (t->tfeed[k] == FD_EXTRA) {
                    const vkn_bind *b = &c->shader->bind[t->targ[k]];
                    out->tex[k].view = w->holder->view; out->tex[k].tex = w->holder; out->tex[k].sampler = smp_at(in, st_of(b->stage), b->slot);
                }
                for (int k = 0; k < VKN_FX_UBO; k++) if (t->ufeed[k] == FD_C1_VS || t->ufeed[k] == FD_C1_FS) feed_c1(&out->ubo[k], c->shader, t->uarg[k]);
                g.s.redirected++; i++; continue;
            }
            count(R_TWIN);
        }
        if (materialize(i, c)) violation(i, R_STALE_READ, c, "read by a post draw");
    }
}
// Writes into watched images (or alias holders) by this post draw: a full overwrite ends the watch, anything else materialises first.
static void watch_writes(const vkn_draw_in *in, const vkn_fx_ctx *c) {
    for (uint32_t i = 0; i < g.nw;) {
        watch *w = &g.w[i]; int hit = 0, hold = 0;
        for (int k = 0; k < VKN_PASS_COLORS && c->color[k]; k++) { hit |= c->color[k] == w->t; hold |= w->holder && c->color[k] == w->holder; }
        hit |= c->depth == w->t;
        if (!hit && !hold) { i++; continue; }
        if (cleared_since(w)) { g.s.cleared++; watch_end(i); continue; }   // [mgpu P4]
        int32_t r[4] = {0, 0, 0, 0};
        if (hit && c->color[0] == w->t) cover_of(in, w->t, r);
        const int every = hit && !hold && one_target(c) && c->color[0] == w->t && writes_all(c) && !reads_of(c->shader, in, w->t);
        if (every && rect_whole(r, w->t)) { g.s.overwritten++; watch_end(i); continue; }
        // [VK-LOWRES] a lower 3D scale: the skipped draw wrote only its part (w->rect), the rest of the image holds what it held before in
        // both chains - a draw that replaces every channel of that part ends the watch just as a whole overwrite does
        if (every && rect_covers(r, w->rect)) {
            if (g_part == 1) { g.s.overwritten_part++; watch_end(i); continue; }
            if (g_part == 2) g.s.part_would++;
        }
        if (w->kind == W_PROBE || materialize(i, c)) violation(i, R_OTHER_WRITE, c, "partly written / its holder written by a post draw");
    }
}
// The probation marks: an image the probation host sampled gets the sentinel at the next draw of another target set.
static void probe_marks(const vkn_fx_ctx *c) {
    for (uint32_t i = 0; i < g.nw;) {
        watch *w = &g.w[i];
        if (w->kind != W_PROBE || w->serial) { i++; continue; }
        int busy = 0; for (int k = 0; k < VKN_PASS_COLORS && c && c->color[k]; k++) busy |= c->color[k] == w->ht || c->color[k] == w->t;
        if (busy) { i++; continue; }   // (the host's pass may still be open: the mark waits, it would end that pass)
        if (w->t->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && w->t->layout != VK_IMAGE_LAYOUT_GENERAL) { violation(i, R_OTHER_WRITE, c, "used before its mark"); continue; }
        vkn_pass_layout(w->t, VK_IMAGE_LAYOUT_GENERAL); w->serial = vkn_cb_serial() | 1u;
        i++;
    }
}

// ================================================================================================ the chain function
static void tick(void);
static void recover_pending(const vkn_fx_ctx *c) {
    if (g.pend < 0) return;
    stash *s = &g.st[g.pend]; const int p = g.pend; g.pend = -1;
    count(R_NOHOST); disarm_on(p, s->valid ? s->O : NULL, c ? c->frame : vkn_frame());
    if (s->valid && s->skipped) {
        const int w = watch_find(s->O); if (w >= 0) watch_end((uint32_t)w);
        if (!replay(s, c)) { if (c) g.s.recovered++; else g.s.recovered_close++; }
    }
    stash_drop(s);
}
static void post_close(void) {
    recover_pending(NULL);
    if (g.la.on && g.la.serial == vkn_cb_serial()) { vkn_fx_ctx c; memset(&c, 0, sizeof c); c.frame = vkn_frame(); la_fail(R_NOSTEP, &c); }
    if (vkn_cb()) probe_marks(NULL);
    // DM_VK_NATIVE_DUMP (vkn_pass.c dump_close, a close listener before this one) copies the frame's targets out: a known read of the
    // watched images, not a violation - their sentinel is put back (counted)
    if (vkn_cb() && vkn_pass_dump_active())
        for (uint32_t i = 0; i < g.nw; i++) {
            watch *w = &g.w[i];
            if ((w->kind != W_PROBE || w->serial) && w->t->layout != VK_IMAGE_LAYOUT_GENERAL) { vkn_pass_layout(w->t, VK_IMAGE_LAYOUT_GENERAL); g.s.dump_reads++; }
        }
    vkn_pass_end();   // (a replay or a mark may have opened a pass or ended none: the recording must close without one)
}
static void post_fx(const vkn_draw_in *in, const vkn_fx_ctx *c, vkn_fx_out *out, void *user) {
    (void)user;
    if (!g.close_reg) { g.close_reg = 1; vkn_rec_on_close(post_close); }
    const int cls = cls_of(c->shader);
    if (g.nw) watch_cheap(c, cls != VKN_PC_OTHER);
    if (cls == VKN_PC_OTHER) {
        if (g.pend >= 0) recover_pending(c);
        if (g.la.on && g.la.serial != vkn_cb_serial()) g.la.on = 0;
        return;
    }
    g.s.draws_post++;
    tick();
    if (g.pend >= 0 && !((g.pend == PR_F1 && cls == VKN_PC_SIGHT) || (g.pend == PR_F2 && cls == VKN_PC_COMB))) recover_pending(c);
    probe_marks(c);
    switch (cls) {
    case VKN_PC_LUT: at_elided(PR_F1, in, c, out); break;
    case VKN_PC_VIG: if (g.level >= VKN_POST_VIG) at_elided(PR_F2, in, c, out); break;
    case VKN_PC_SIGHT: at_host(PR_F1, cls, in, c, out); break;
    case VKN_PC_COMB: if (g.level >= VKN_POST_VIG) at_host(PR_F2, cls, in, c, out); break;
    case VKN_PC_FXAA: case VKN_PC_COPY: at_step(cls, in, c, out); break;
    case VKN_PC_FOG: case VKN_PC_OUTLINE: at_carry(cls, in, c); break;
    default: break;
    }
    if (out->skip) return;
    if (g.la.on && cls != VKN_PC_SIGHT && cls != VKN_PC_FOG && cls != VKN_PC_OUTLINE && cls != VKN_PC_FXAA && cls != VKN_PC_COPY && reads_of(c->shader, in, g.la.cur))
        la_fail(R_OTHER_READER, c);
    if (g.nw) { watch_reads(in, c, out); watch_writes(in, c); }
    if (g.probe.O) {   // the probation window of the host's input (unmarked: the sentinel is put at the next draw of another target set)
        vkn_tex *O = g.probe.O; g.probe.O = NULL;
        if (watch_find(O) < 0 && g.nw < 4 && vkn_tex_ref(O)) {
            watch *w = &g.w[g.nw++]; memset(w, 0, sizeof *w);
            w->t = O; w->kind = W_PROBE; w->pair = (uint8_t)g.probe.pair; w->ht = g.probe.ht; memcpy(w->rect, g.probe.rect, sizeof w->rect);
        }
    }
}

// ================================================================================================ install, census
static void tick(void) {
    const uint64_t now = vkn_now_ns();
    if (!g.next_ns) { g.next_ns = now + 10000000000ull; return; }
    if (now < g.next_ns) return;
    g.next_ns = now + 10000000000ull;
    vkn_post_census();
}
void vkn_post_install(void) {
    static int once;
    if (__atomic_exchange_n(&once, 1, __ATOMIC_ACQ_REL)) return;
    const int lv = vkn_post_level();
    g.level = lv; g.level_ra = lv >= VKN_POST_AA;
    vkn_logf("post: DM_VK_NATIVE_POST=%d (%s; replaces DM_NB_POST / _AA / _AA_OUTLINE and the post part of DM_GFX_NATIVE_SHADERS), AA route %d, luma %d (%s), verify %d%s, "
             "DM_VK_NATIVE_POST_PART=%d (%s)",
             lv, lv == 0 ? "the Switch post shaders, no fusion" : lv == 1 ? "mobile post shaders, no fusion" : lv == 2 ? "mobile + LUT o Sight (o FXAA)" :
             lv == 3 ? "mobile + LUT o Sight (o FXAA) + Vignette o Combine" : "mobile + LUT o Sight (o FXAA, fog and outline carry) + Vignette o Combine",
             g_aa, g_luma, k_luma_name[g_luma], g_verify, g_verify == 1 ? " (decisions counted, the chain drawn as recorded)" : g_verify == 2 ? " (poison: skipped targets cleared to magenta)" : "",
             g_part, g_part == 0 ? "below 100 % only whole overwrites end a watch, no route A" : g_part == 1 ? "below 100 % the drawn part is what counts; route A with the FSR stretch" :
             "below 100 % part decisions counted only");
    if (lv < VKN_POST_AA) return;
    g.installed = vkn_fx_register("post", post_fx, NULL, 400) == 0;
}
void vkn_post_stats_get(vkn_post_stats *s) {
    if (!s) return;
    *s = g.s; s->level = g.level; s->aa = g_aa; s->luma = g_luma; s->verify = g_verify; s->part = g_part;
    for (int i = 0; i < PR_N && i < 4; i++) s->armed_frames[i] = g.arm[i].armed;
}
void vkn_post_census(void) {
    static vkn_post_stats last; static uint64_t lwhy[R_N];
    if (!g.installed) return;
    vkn_post_stats s; vkn_post_stats_get(&s);
    char by[1400]; int k = 0; by[0] = 0;
    for (int i = 0; i < R_N && k < (int)sizeof by - 120; i++) if (g.why[i] != lwhy[i]) k += snprintf(by + k, sizeof by - (size_t)k, "%s%s %llu", k ? ", " : "", k_why[i], (unsigned long long)(g.why[i] - lwhy[i]));
#define D(f) (unsigned long long)(s.f - last.f)
    vkn_logf("post: level %d%s: post draws %llu; skipped %llu (would %llu), fused F1 %llu / F1A %llu / F2 %llu (would %llu); AA steps skipped %llu (would %llu), carried %llu; "
             "alias reads redirected %llu, twins %u; recovered %llu (at close %llu), materialised %llu, watches ended by an overwrite %llu, double AA frames %llu, "
             "violations %llu, dump reads %llu; armed F1 %d F2 %d route A %d; watches ended by an engine clear %llu; refusals %s",
             s.level, s.verify == 1 ? " verify" : s.verify == 2 ? " poison" : "", D(draws_post), D(skipped), D(would_skip), D(fused[0]), D(fused[1]), D(fused[2]), D(would_fuse),
             D(step_skipped), D(step_would), D(carried), D(redirected), (unsigned)s.twins, D(recovered), D(recovered_close), D(materialized), D(overwritten),
             D(late_aa), D(violations), D(dump_reads), g.arm[PR_F1].armed, g.arm[PR_F2].armed, g.arm[PR_RA].armed, D(cleared), k ? by : "none");
    // [VK-LOWRES] a line of its own (the one above is near the 1024-byte log line already)
    vkn_logf("post part (10 s): DM_VK_NATIVE_POST_PART=%d: watches ended by a part overwrite %llu (would %llu), F1A on a part %llu, "
             "FSR pre passes left out %llu (would %llu)", s.part, D(overwritten_part), D(part_would), D(f1a_part), D(stretch_pre_skipped), D(stretch_would));
#undef D
    last = s; memcpy(lwhy, g.why, sizeof lwhy);
}
#endif
