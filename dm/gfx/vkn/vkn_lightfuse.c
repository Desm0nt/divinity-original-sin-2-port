// [DOS2] VK-NATIVE stage 5.1 A4: LIGHTFUSE + FOG (vkn_lightfuse.h has the scheme, the checks, the switches). One vkn_fx function
// (order 300). A draw that is not DeferredSky / ClusteredLightingWithSSAO / DeferredFog costs one table load and two compares (the
// role of the engine shader, cached per shader id) while no skipped Sky waits and no Fog window is open. Everything runs on the
// recording thread (the fx chain); the census on the render thread after EndFrame (the same thread in mode 1). No game addresses.
// Ported from xbg_nr_lightfuse.inc (namespace nrlf: sky / light / fogdraw / redirect / tick); the model is nr_lightfuse.h as it is.
#ifdef DM_GAME_DOS2
#include "vkn_lightfuse.h"
#include "vkn_fx.h"
#include "vkn_buf.h"
#include "vkn_draw.h"
#include "vkn_frame.h"
#include "vkn_pass.h"
#include "vkn_shader.h"
#include "vkn_state.h"
#include "vkn_tex.h"
#include "vkn_shlib_named.h"
#include "../nr/nr_lightfuse.h"
#include "../nr/nr_sh.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
// the shader library's doors (vkn_shlib_named.cpp; weak: host tests provide their own or none)
extern int vkn_shlib_named_fused(const char *light_fs_key, char variant, xbg_vkn_stage *out, uint64_t declared[4], uint64_t *dref) __attribute__((weak));
extern int vkn_shlib_named_same_bundle(const char *key_a, const char *key_b) __attribute__((weak));

enum { LF_NONE = 0, LF_SKY = 1, LF_LIGHT = 2, LF_FOG = 3 };
// nr_lightfuse.h's Light / Fog binding numbers the old path read (xbg_nr_lightfuse.inc light / fogdraw): Light 22 linear depth, 52 Sp
// (its ambient), 82 c16, 100 c15; Fog 16 the scene (Df), 10 the linear depth, 22 c16, 28 c15.
enum { LB_DEPTH = 22, LB_SP = 52, LB_C16 = 82, LB_C15 = 100, FB_SCENE = 16, FB_DEPTH = 10, FB_C16 = 22, FB_C15 = 28 };
enum { ROLE_IDS = 16384, VSNAP = 1024, ISNAP = 512, FUSED = 8, TWINS = 8, PIECE_SHADERS = 48, PAIRS = 8 };
enum { R_CFG, R_SKY, R_LIGHT, R_RECOVER, R_LOST, R_FUSED, R_FOG, R_WINDOW, R_TWIN, R_N };

typedef struct tsnap { VkImageLayout layout; uint8_t nclr, dclr; uint64_t clr_ns, written; } tsnap;
typedef struct vsnap { uint8_t vb[VKN_VFMT_STREAMS][VSNAP]; uint32_t vlen[VKN_VFMT_STREAMS]; uint8_t ib[ISNAP]; uint32_t ilen; } vsnap;
typedef struct stash {
    int valid, skipped, light_done, light_ok, fog_fused;   // skipped: Sky was not drawn and waits for its Light (or a recovery); light_done:
                                                           // a Light draw was checked against this stash; light_ok: it passed and the Fog view is kept
    int df_zero, sp_zero;                         // at the Sky draw: Df / Sp carry a remembered clear to zero (what Sky's pass starts from)
    uint64_t frame; uint32_t serial;
    const vkn_shader *sky;
    vkn_draw_in in; int32_t tex[2][VKN_TEX_SLOTS], smp[2][VKN_TEX_SLOTS];
    uint32_t first, count, instances; uint8_t indexed, prim; uint16_t vfmt;
    struct { uint8_t st, slot; uint32_t off, size; } ub[24]; uint32_t nub;   // every uniform block Sky's table binds (the replay)
    uint32_t vub_ok, vub_off[VKN_UBO_SLOTS], vub_size[VKN_UBO_SLOTS];          // vertex-stage blocks as announced at the Sky draw
    vkn_fx_tex xt[NR_LF_NTEX]; vkn_fx_ubo xu[2];                                // the extras
    uint32_t tn, tl[VKN_PASS_COLORS], tm[VKN_PASS_COLORS]; vkn_tex *tc[VKN_PASS_COLORS], *td;   // Sky's target set
    vkn_tex *sp, *df; tsnap snap[2];
    vkn_tex *held[NR_LF_NTEX + 2]; uint32_t nheld;
    vsnap vs;
    vkn_tex *depth; uint8_t c16[48], c15[16];    // the Fog inputs as the Light saw them (light_ok)
} stash;
typedef struct fused { const vkn_shader *light; uint64_t handle; char v; const vkn_shader *fx; vkn_fx_ubo c1[2]; } fused;
typedef struct twin { const vkn_shader *s; uint64_t handle, cmask; VkShaderModule vs, fs; const vkn_shader *fx; vkn_fx_ubo c1[2]; uint8_t nt, tst[VKN_FX_TEX], tslot[VKN_FX_TEX]; } twin;

static struct {
    int mode, fog, inst, reg;
    char key[4][17];                               // fragment keys by role (nr_sh table)
    nr_lf_state ls; int fog_learned;
    uint64_t frame, frame_sky;
    vkn_lf_stats s, last;
    uint32_t reds[R_N];
    stash st;
    struct { int on; vkn_tex *c, *sp; tsnap snap; } rd;
    fused fu[FUSED]; uint32_t nfu;
    twin tw[TWINS]; uint32_t ntw;
    uint32_t pieces;                               // shader objects made (vkn_fx_shader_create)
    struct { char a[17], b[17]; int eq; } pair[PAIRS]; uint32_t npair;
    int retype;                                    // -1 not read yet
} g;
static const vkn_shader *g_role_s[ROLE_IDS]; static uint64_t g_role_h[ROLE_IDS]; static uint8_t g_role[ROLE_IDS];

static void lf_red(int kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void lf_red(int kind, const char *fmt, ...) {
    __atomic_add_fetch(&g.s.reds, 1, __ATOMIC_RELAXED);   // ([stage 5 rest] the warm-up worker makes this piece's objects too)
    if (kind < 0 || kind >= R_N || __atomic_fetch_add(&g.reds[kind], 1, __ATOMIC_RELAXED) >= 16) return;
    char b[640]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    vkn_logf(RED "lightfuse: %s", b);
}
const char *vkn_lf_reason_name(int fog, int i) {
    static const char *w[VKN_LF_W_N] = {"Sky draw shape (2 colours, no depth, VS + FS, no alpha test / earlier effect)", "Sky resources not all bound",
        "Light without this frame's / recording's Sky stash", "Light stages / alpha test / an earlier effect", "Light's target is not Sky's Df (or depth bound)",
        "Light's colour 0 is not blended ONE, ONE, ADD (R G B written)", "Df has no pending clear to zero", "Light does not read Sky's Sp as its ambient",
        "another vertex shader bundle than Sky's", "vertex constants / streams / format differ from Sky's (another view ray)", "primitive / draw range / viewport / scissor differ from Sky's",
        "vertex retype variant needed", "Sp / Df used by something else since the Sky draw", "fused shader not available"};
    static const char *f[VKN_LF_F_N] = {"Fog without a checked Light of this frame", "Fog stages / alpha test / an earlier effect", "Fog targets (not one colour without depth)",
        "Fog colour 0 blended", "Fog write mask (R G B not all written, or A not written on a target with alpha)", "Fog bindings 16 / 10 / 22 / 28 missing or of another kind",
        "Fog's target not like Sp (format / size / one level and layer)", "Fog does not read Light's Df", "Fog's depth is not Light's", "Fog's c16[4..6] / c15[12] differ from Light's",
        "another vertex shader / constants / streams than Sky's", "draw range / viewport / scissor differ from Sky's", "the Fog draw does not cover its target",
        "Sp's pending clear is not zero (the fused output 1 is blended)", "fused Fog shader ('g') not available"};
    static const char *c[VKN_LF_CP_N] = {"C is a target of the next draw", "an earlier effect binds C", "a reader of C that cannot take a shader object of its own"};
    if (fog == 2) return i >= 0 && i < VKN_LF_CP_N ? c[i] : "?";
    if (fog) return i >= 0 && i < VKN_LF_F_N ? f[i] : "?";
    return i >= 0 && i < VKN_LF_W_N ? w[i] : "?";
}

// ---------------------------------------------------------------- small facts
static int role_of(const vkn_shader *s) {
    if (!s) return LF_NONE;
    const uint32_t id = s->id;
    if (id < ROLE_IDS && g_role_s[id] == s && g_role_h[id] == s->handle) return g_role[id];
    int r = LF_NONE;
    if (!vkn_fx_shader_is(s) && (s->stages & (1u << VKN_SRC_FS)))
        for (int k = LF_SKY; k <= LF_FOG; k++) if (g.key[k][0] && !memcmp(s->key[VKN_SRC_FS], g.key[k], 16)) r = k;
    if (id < ROLE_IDS) { g_role_s[id] = s; g_role_h[id] = s->handle; g_role[id] = (uint8_t)r; }
    return r;
}
static const vkn_bind *bind_of(const vkn_shader *s, uint32_t binding) {
    for (uint32_t i = 0; i < s->nbind; i++) if (s->bind[i].binding == binding) return &s->bind[i];
    return NULL;
}
static int src_of(uint8_t stage) { return stage == 0 ? 0 : stage == 1 ? 1 : -1; }   // engine stage -> vkn_draw_in stage index
static vkn_tex *tex_at(const vkn_draw_in *in, int st, uint32_t slot, VkImageView *view) {
    if (view) *view = VK_NULL_HANDLE;
    if (st < 0 || st > 1 || slot >= VKN_TEX_SLOTS || !in->tex[st] || in->tex[st][slot] < 0) return NULL;
    vkn_tex *t = NULL; const VkImageView v = vkn_tex_view_by_id((uint32_t)in->tex[st][slot], &t);
    if (view) *view = v;
    return v ? t : NULL;
}
static vkn_tex *tex_of(const vkn_shader *s, const vkn_draw_in *in, uint32_t binding) {
    const vkn_bind *b = bind_of(s, binding);
    return b && b->kind == VKN_BK_TEXTURE ? tex_at(in, src_of(b->stage), b->slot, NULL) : NULL;
}
static uint32_t ncolor(const vkn_fx_ctx *c) { uint32_t n = 0; for (uint32_t i = 0; i < VKN_PASS_COLORS; i++) if (c->color[i]) n = i + 1; return n; }
static void tsnap_take(const vkn_tex *t, tsnap *o) { o->layout = t->layout; o->nclr = t->pass_nclr; o->dclr = t->pass_dclr; o->clr_ns = t->pass_clr_ns; o->written = t->pass_written; }
static int tsnap_same(const vkn_tex *t, const tsnap *o) {
    return t->layout == o->layout && t->pass_nclr == o->nclr && t->pass_dclr == o->dclr && t->pass_clr_ns == o->clr_ns && t->pass_written == o->written;
}
// 1 = t has a remembered clear of layer 0 to zero in R, G and B (what the blender adds the fused output to; x + 0 = x, -0 too)
static int zero_clear(const vkn_tex *t) {
    for (uint32_t i = 0; i < t->pass_nclr && i < 4; i++)
        if (t->pass_clr[i].layer == 0) return t->pass_clr[i].rgba[0] == 0.0f && t->pass_clr[i].rgba[1] == 0.0f && t->pass_clr[i].rgba[2] == 0.0f;
    return 0;
}
// The draw path's vertex retype code (vkn_draw.c retype_code / retype_on, DM_GFX_VTX_RETYPE): 0 = the base vertex module is drawn.
static uint64_t retype_code(const vkn_shader *s, uint16_t vfmt) {
    if (g.retype < 0) { const char *e = getenv("DM_GFX_VTX_RETYPE"); const vkn_dev *d = vkn_device(); g.retype = e && *e ? atoi(e) != 0 : d ? d->props.vendorID != 0x5143 : 0; }
    const vkn_vfmt *vf = vfmt ? vkn_vfmt_info(vfmt) : NULL;
    if (!vf || !g.retype) return 0;
    uint64_t code = 0;
    for (uint32_t i = 0; i < s->ninputs && i < 16; i++) {
        const uint32_t l = s->inputs[i]; if (l >= VKN_VFMT_LOCS) continue;
        const uint32_t c = vf->loc_class[l];
        if (c && c - 1 != s->input_classes[i]) code |= (uint64_t)c << (2 * l);
    }
    return code;
}
// The bytes the draw reads of its index buffer and vertex streams (the old path's 1 KB / 256 B snapshots, here exactly the range
// drawn: arrays first + count vertices, indexed up to the largest index). -1 = not comparable (no CPU view, too large).
static int vsnap_take(const vkn_draw_in *in, uint16_t vfmt, int indexed, uint32_t first, uint32_t count, uint32_t instances, vsnap *o) {
    memset(o->vlen, 0, sizeof o->vlen); o->ilen = 0;
    uint64_t nv = (uint64_t)first + count;
    if (indexed) {
        vkn_buf_info ib; memset(&ib, 0, sizeof ib);
        if (vkn_buf_lookup(VKN_BUF_IB, in->ib, &ib) || !ib.blk.cpu) return -1;
        const uint32_t isz = ib.index_size == 4 ? 4 : 2; const uint64_t off = (uint64_t)first * isz, n = (uint64_t)count * isz;
        if (off + n > ib.size || n > ISNAP) return -1;
        memcpy(o->ib, (const uint8_t *)ib.blk.cpu + off, (size_t)n); o->ilen = (uint32_t)n;
        uint32_t mx = 0;
        for (uint32_t i = 0; i < count; i++) { uint32_t v = 0; memcpy(&v, o->ib + (size_t)i * isz, isz); if (v > mx) mx = v; }
        nv = (uint64_t)mx + 1;
    }
    const vkn_vfmt *vf = vfmt ? vkn_vfmt_info(vfmt) : NULL;
    for (uint32_t i = 0; vf && i < vf->nbind; i++) {
        const uint32_t st = vf->bind[i].binding; if (st >= VKN_VFMT_STREAMS) continue;
        vkn_buf_info b; memset(&b, 0, sizeof b);
        if (vkn_buf_lookup(VKN_BUF_VB, in->vb[st], &b) || !b.blk.cpu || in->vb_off[st] > b.size) return -1;
        const uint64_t stride = vf->bind[i].stride, avail = b.size - in->vb_off[st];
        uint64_t need = (vf->bind[i].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? (instances ? instances : 1) : nv) * stride;
        if (!stride) need = avail < 16 ? avail : 16;
        if (need > avail || need > VSNAP) return -1;
        memcpy(o->vb[st], (const uint8_t *)b.blk.cpu + in->vb_off[st], (size_t)need); o->vlen[st] = (uint32_t)need;
    }
    return 0;
}
static int vsnap_same(const vsnap *a, const vsnap *b) {
    if (a->ilen != b->ilen || memcmp(a->ib, b->ib, a->ilen)) return 0;
    for (int s = 0; s < VKN_VFMT_STREAMS; s++) if (a->vlen[s] != b->vlen[s] || memcmp(a->vb[s], b->vb[s], a->vlen[s])) return 0;
    return 1;
}
static int same_vs(const char *a, const char *b) {   // the old path's same_bundle (once per pair, logged)
    if (!strncmp(a, b, 16)) return 1;
    for (uint32_t i = 0; i < g.npair; i++) if (!strncmp(g.pair[i].a, a, 16) && !strncmp(g.pair[i].b, b, 16)) return g.pair[i].eq;
    const int eq = vkn_shlib_named_same_bundle ? vkn_shlib_named_same_bundle(a, b) == 1 : 0;
    vkn_logf("lightfuse: vertex shaders %.16s / %.16s: bundles %s", a, b, eq ? "equal byte for byte (one view ray)" : "DIFFER (or not readable): no fusion");
    if (g.npair < PAIRS) { snprintf(g.pair[g.npair].a, 17, "%.16s", a); snprintf(g.pair[g.npair].b, 17, "%.16s", b); g.pair[g.npair].eq = eq; g.npair++; }
    return eq;
}
// The vertex-stage constant blocks shader s binds now equal (bytes) those announced at the Sky draw.
static int vs_blocks_same(const vkn_shader *s) {
    const uint8_t *ring = (const uint8_t *)vkn_ring_cpu();
    for (uint32_t i = 0; i < s->nbind; i++) {
        const vkn_bind *b = &s->bind[i];
        if (b->kind != VKN_BK_UNIFORM || b->stage != 0 || b->slot >= VKN_UBO_SLOTS) continue;
        uint32_t off = 0, size = 0; const int now = !vkn_draw_ubo_get(0, b->slot, &off, &size), then = (g.st.vub_ok >> b->slot) & 1;
        if (now != then) return 0;
        if (!now || (off == g.st.vub_off[b->slot] && size == g.st.vub_size[b->slot])) continue;
        uint32_t n = size < g.st.vub_size[b->slot] ? size : g.st.vub_size[b->slot];
        if (b->bytes && b->bytes < n) n = b->bytes;
        if (!ring || (size != g.st.vub_size[b->slot] && (!b->bytes || b->bytes > n)) || memcmp(ring + off, ring + g.st.vub_off[b->slot], n)) return 0;
    }
    return 1;
}
static int vs_tex_same(const vkn_shader *s, const vkn_draw_in *in) {   // vertex-stage textures (none in a full-screen pass) read the same ids
    for (uint32_t i = 0; i < s->nbind; i++) {
        const vkn_bind *b = &s->bind[i];
        if (b->kind != VKN_BK_TEXTURE || b->stage != 0 || b->slot >= VKN_TEX_SLOTS) continue;
        const int32_t a = in->tex[0] ? in->tex[0][b->slot] : -1, c = g.st.tex[0][b->slot];
        if (a != c) return 0;
    }
    return 1;
}
static int draw_same(const vkn_draw_in *in, const vkn_fx_ctx *ctx) {
    const stash *t = &g.st;
    return ctx->prim == t->prim && ctx->indexed == t->indexed && ctx->first == t->first && ctx->count == t->count && ctx->instances == t->instances &&
           (!ctx->indexed || in->ib == t->in.ib) && !memcmp(in->viewport, t->in.viewport, sizeof in->viewport) && !memcmp(in->scissor, t->in.scissor, sizeof in->scissor);
}
static int ring_bytes(uint32_t slot, uint32_t at, uint32_t n, uint8_t *out) {   // fragment block `slot` bytes [at, at + n)
    uint32_t off = 0, size = 0; const uint8_t *ring = (const uint8_t *)vkn_ring_cpu();
    if (!ring || vkn_draw_ubo_get(1, slot, &off, &size) || size < at + n) return -1;
    memcpy(out, ring + off + at, n); return 0;
}
static int draw_again(const vkn_draw_in *in, int indexed, uint32_t first, uint32_t count, uint32_t instances) {
    return indexed ? vkn_draw_indexed(in, first, count, instances) : vkn_draw_arrays(in, first, count);
}

// ---------------------------------------------------------------- shader objects of this piece
typedef struct job { const char *key; char v; xbg_vkn_stage *st; uint64_t decl[4], dref; int rc; } job;
static void *job_run(void *a) { job *j = (job *)a; j->rc = vkn_shlib_named_fused(j->key, j->v, j->st, j->decl, &j->dref); return NULL; }
// The library's load-time rewrites run on its own threads with 8 MB stacks (vkn_shader.c); the recording thread's stack is the
// engine's: the one load of a fused module runs on a thread of its own (once per Light shader and variant).
static int load_fused(job *j) {
    if (!vkn_shlib_named_fused) return -1;
    pthread_attr_t at; pthread_t th;
    if (pthread_attr_init(&at)) return -1;
    pthread_attr_setstacksize(&at, 8u << 20);
    const int ok = !pthread_create(&th, &at, job_run, j);
    pthread_attr_destroy(&at);
    if (!ok) return -1;
    pthread_join(th, NULL);
    return j->rc;
}
// C1 (the shader's own constants) is not a source of a piece's shader: fed as an effect block from the engine object's static block.
static int c1_to_fx(vkn_bind *t, const vkn_shader *s, uint8_t fx_slot, vkn_fx_ubo c1[2]) {
    const int k = src_of(t->stage);
    if (k < 0 || !s->c1[k].size) return -1;
    c1[k].buf = s->c1[k].buf; c1[k].off = s->c1[k].off; c1[k].size = (uint32_t)s->c1[k].size;
    t->kind = VKN_BK_FX_UBO; t->slot = (uint8_t)(fx_slot + k);
    return 0;
}
static int cmp_binding(const void *a, const void *b) { return (int)((const vkn_bind *)a)->binding - (int)((const vkn_bind *)b)->binding; }
static const vkn_shader *make_fx(const char *name, VkShaderModule vs, VkShaderModule fs, const vkn_shader *eng, vkn_bind *tab, uint32_t n) {
    if (g.pieces >= PIECE_SHADERS) { lf_red(R_TWIN, "%s: more than %d shaders of this piece made: no more", name, PIECE_SHADERS); return NULL; }
    qsort(tab, n, sizeof *tab, cmp_binding);
    vkn_fx_shader_src src; memset(&src, 0, sizeof src);
    src.name = name; src.vs = vs; src.fs = fs; src.bind = tab; src.nbind = n;
    src.ninputs = eng->ninputs; memcpy(src.inputs, eng->inputs, sizeof src.inputs); memcpy(src.input_classes, eng->input_classes, sizeof src.input_classes);
    const vkn_shader *o = vkn_fx_shader_create(&src);
    if (o) g.pieces++;
    return o;
}
// The fused shader for engine Light shader `l` and variant v ('f' Sky, 'g' Sky + Fog): made once (a failure is kept too).
// [stage 5 rest] Both the recording thread and the warm-up worker (lf_make, the recipe "lightfuse" of vkn_warm.h) make this piece's objects:
// fused_for / twin_for run under g_mk_mu, so a pipeline the worker built for an object is the one the draw finds.
static pthread_mutex_t g_mk_mu = PTHREAD_MUTEX_INITIALIZER;
extern void vkn_warm_recipe_fx(const void *fx_shader, const char *piece, const void *eng_shader, const void *arg, uint32_t n) __attribute__((weak));
static const fused *fused_for_locked(const vkn_shader *l, char v);
static const fused *fused_for(const vkn_shader *l, char v) {
    pthread_mutex_lock(&g_mk_mu); const fused *f = fused_for_locked(l, v); pthread_mutex_unlock(&g_mk_mu);
    return f;
}
static const fused *fused_for_locked(const vkn_shader *l, char v) {
    for (uint32_t i = 0; i < g.nfu && i < FUSED; i++) if (g.fu[i].light == l && g.fu[i].handle == l->handle && g.fu[i].v == v) return g.fu[i].fx ? &g.fu[i] : NULL;
    fused *f = g.nfu < FUSED ? &g.fu[g.nfu++] : &g.fu[(g.nfu++) % FUSED];   // (a full cache forgets the oldest: rebuilt if it comes back)
    memset(f, 0, sizeof *f); f->light = l; f->handle = l->handle; f->v = v;
    xbg_vkn_stage *st = (xbg_vkn_stage *)calloc(1, sizeof *st);
    vkn_bind *tab = (vkn_bind *)calloc((size_t)l->nbind + NR_LF_NX, sizeof *tab);
    const char *why = NULL; job j; memset(&j, 0, sizeof j);
    if (!st || !tab) why = "out of memory";
    else { j.key = l->key[VKN_SRC_FS]; j.v = v; j.st = st; const int rc = load_fused(&j); if (rc) why = rc < 0 ? "the shader library is not up (or no thread)" : "no fused bundle that passes the checks (see the shlib named line)"; }
    uint32_t n = 0; int light_dyn = 0;
    for (uint32_t i = 0; !why && i < l->nbind; i++) {
        vkn_bind t = l->bind[i];
        if (t.binding >= 200) { why = "the Light table uses a binding of the extras (200..254)"; break; }
        if (t.count != 1) { why = "a binding with more than one descriptor"; break; }
        if (t.kind == VKN_BK_UNIFORM && t.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) light_dyn = 1;
        if (t.bundle_stage == 4) {   // fragment: the fused stage's entry (uniform extents of the fused words); undeclared: left out
            const xbg_shader_binding *fb = NULL;
            for (uint32_t k = 0; k < st->binding_count; k++) if (st->bindings[k].binding == t.binding) { fb = &st->bindings[k]; break; }
            if (!fb) { why = "the fused bundle's binding table differs from the Light shader's"; break; }
            if (t.binding < 256 && !(j.decl[t.binding >> 6] >> (t.binding & 63) & 1)) continue;
            t.bytes = fb->bytes; t.flags &= ~VKN_BF_CMP;
            if (t.kind == VKN_BK_TEXTURE && t.binding < 64 && (j.dref >> t.binding & 1)) t.flags |= VKN_BF_CMP;
        }
        if (t.kind == VKN_BK_C1) { if (c1_to_fx(&t, l, 2, f->c1)) { why = "C1 constants without their static block"; break; } }
        else if (t.kind != VKN_BK_UNIFORM && t.kind != VKN_BK_SUPPORT && t.kind != VKN_BK_TEXTURE) { why = "a binding of a kind a piece's shader cannot have"; break; }
        tab[n++] = t;
    }
    if (!why && st->binding_count != 0) for (uint32_t k = 0; k < st->binding_count && !why; k++) if (!bind_of(l, st->bindings[k].binding)) why = "the fused bundle's binding table differs from the Light shader's";
    if (!why && (j.decl[LB_SP >> 6] >> (LB_SP & 63) & 1)) why = "the fused module reads Sp (binding 52): its ambient must come from the Sky inside";
    for (int k = 0; !why && k < NR_LF_NX; k++) {
        vkn_bind t; memset(&t, 0, sizeof t);
        t.binding = (uint16_t)nr_lf_x[k].fused; t.stage = 1; t.bundle_stage = 4; t.count = 1; t.dyn = VKN_NO_DYN;
        if (nr_lf_x[k].kind == NR_LF_TEX) { t.kind = VKN_BK_FX_TEX; t.slot = (uint8_t)k; t.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; }
        else { t.kind = VKN_BK_FX_UBO; t.slot = (uint8_t)(k - NR_LF_NTEX); t.type = light_dyn ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; }
        tab[n++] = t;
    }
    if (!why) {   // the dynamic-offset limit (the Light table fits it; the extras may not): extras static first, then C1 / SUPPORT (as vkn_shader.c policy 2)
        const vkn_dev *d = vkn_device(); const uint32_t lim = d ? d->props.limits.maxDescriptorSetUniformBuffersDynamic : 8;
        for (int pass = 0; pass < 3; pass++) {
            uint32_t dyn = 0; for (uint32_t i = 0; i < n; i++) dyn += tab[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            if (dyn <= lim) break;
            for (uint32_t i = 0; i < n; i++)
                if (tab[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC && (pass == 2 || (pass == 0 ? tab[i].binding >= 200 : tab[i].kind != VKN_BK_UNIFORM))) tab[i].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        }
        if (j.dref) vkn_logf("lightfuse: the fused module samples bindings %#llx with a depth comparison: flagged (comparison sampler)", (unsigned long long)j.dref);
        const VkShaderModule vsm = vkn_shader_module(l, VKN_STAGE_VS, 0, 0.0f, 0);
        if (!vsm || !st->module) why = "no vertex / fragment module";
        else if (!(f->fx = make_fx(v == 'g' ? "lightfuse-g" : "lightfuse-f", vsm, st->module, l, tab, n))) why = "vkn_fx_shader_create refused the table";
    }
    if (why) lf_red(R_TWIN, "%c.ClusteredLightingWithSSAO for Light shader %#llx (%s): %s - no fusion with this variant", v, (unsigned long long)l->handle, l->key[VKN_SRC_FS], why);
    else if (vkn_warm_recipe_fx) vkn_warm_recipe_fx(f->fx, "lightfuse", l, &v, 1);   // [stage 5 rest] listed by recipe ('f' / 'g')
    if (!why) vkn_logf("lightfuse: Light shader %#llx (%s) -> %s: %u bindings (the Light table, %u fragment bindings the fused module declares, + Sky's 8 textures on 200..242 and c8 / c6 on 248 / 254)",
                  (unsigned long long)l->handle, l->key[VKN_SRC_FS], v == 'g' ? "lightfuse-g (+ DeferredFog on output 1)" : "lightfuse-f", n, st ? st->binding_count : 0u);
    free(st); free(tab);
    return f->fx ? f : NULL;
}

// [P10] the Light shader and the variant ('f' / 'g') behind a fused object of this piece (vkn_pass.c makes its input-attachment twin). 0 = found.
int vkn_lf_fused_of(const vkn_shader *fx, const vkn_shader **light, char *v) {
    int r = -1;
    pthread_mutex_lock(&g_mk_mu);
    for (uint32_t i = 0; i < g.nfu && i < FUSED; i++) if (g.fu[i].fx == fx && fx) { if (light) *light = g.fu[i].light; if (v) *v = g.fu[i].v; r = 0; break; }
    pthread_mutex_unlock(&g_mk_mu);
    return r;
}

// ---------------------------------------------------------------- the stash and the recovery
static void stash_drop(void) {
    for (uint32_t i = 0; i < g.st.nheld; i++) vkn_tex_unref(g.st.held[i]);
    memset(&g.st, 0, sizeof g.st);
}
static int hold(vkn_tex *t) {
    if (!t) return 0;
    for (uint32_t i = 0; i < g.st.nheld; i++) if (g.st.held[i] == t) return 1;
    if (g.st.nheld >= NR_LF_NTEX + 2 || !vkn_tex_ref(t)) return 0;
    g.st.held[g.st.nheld++] = t; return 1;
}
// The skipped Sky drawn now, as the engine asked for it (its target set, its blocks; the engine's put back). 0 = drawn.
static int replay_sky(void) {
    stash *t = &g.st;
    vsnap now;
    if (vsnap_take(&t->in, t->vfmt, t->indexed, t->first, t->count, t->instances, &now) || !vsnap_same(&now, &t->vs)) return -2;   // its vertex bytes changed
    uint32_t n0 = 0, lay[VKN_PASS_COLORS], mip[VKN_PASS_COLORS]; vkn_tex *col[VKN_PASS_COLORS], *dep = NULL;
    vkn_pass_cur_get(&n0, col, lay, mip, &dep);
    vkn_tex *held[VKN_PASS_COLORS + 1];
    for (uint32_t i = 0; i <= VKN_PASS_COLORS; i++) { vkn_tex *x = i < VKN_PASS_COLORS ? (i < n0 ? col[i] : NULL) : dep; held[i] = x && vkn_tex_ref(x) ? x : NULL; }
    struct { uint32_t off, size; int ok; } was[24];
    for (uint32_t i = 0; i < t->nub; i++) was[i].ok = !vkn_draw_ubo_get(t->ub[i].st, t->ub[i].slot, &was[i].off, &was[i].size);
    vkn_fx_bypass(1);
    vkn_pass_targets(t->tn, t->tc, t->tl, t->tm, t->td);
    for (uint32_t i = 0; i < t->nub; i++) vkn_draw_ubo(t->ub[i].st, t->ub[i].slot, t->ub[i].off, t->ub[i].size);
    vkn_draw_in in = t->in; in.tex[0] = t->tex[0]; in.tex[1] = t->tex[1]; in.tex[2] = NULL; in.smp[0] = t->smp[0]; in.smp[1] = t->smp[1]; in.smp[2] = NULL;
    const int rc = draw_again(&in, t->indexed, t->first, t->count, t->instances);
    for (uint32_t i = 0; i < t->nub; i++) if (was[i].ok) vkn_draw_ubo(t->ub[i].st, t->ub[i].slot, was[i].off, was[i].size);   // (a slot never announced before stays: the engine binds it only after announcing it)
    vkn_pass_targets(n0, col, lay, mip, dep);
    for (uint32_t i = 0; i <= VKN_PASS_COLORS; i++) if (held[i]) vkn_tex_unref(held[i]);
    vkn_fx_bypass(0);
    return rc ? -1 : 0;
}
// A skipped Sky whose Light did not come (why: what proved it): drawn now when nothing used Sp / Df since, else lost. Learning again.
static void recover(const char *why, const vkn_fx_ctx *ctx) {
    stash *t = &g.st;
    if (!t->valid || !t->skipped) return;
    t->skipped = 0;
    int rc = -3;
    if (t->frame == ctx->frame && tsnap_same(t->sp, &t->snap[0]) && tsnap_same(t->df, &t->snap[1])) rc = replay_sky();
    nr_lf_orphan(&g.ls); g.s.unlearn++;
    if (!rc) {
        g.s.recovered++;
        lf_red(R_RECOVER, "a skipped Sky (frame %llu) was drawn late, exactly, at %s (learning again)", (unsigned long long)t->frame, why);
    } else {
        g.s.lost++;
        lf_red(R_LOST, "a skipped Sky (frame %llu) could not be drawn late at %s: %s - its output is missing this frame", (unsigned long long)t->frame, why,
               rc == -3 ? (t->frame != ctx->frame ? "the frame ended" : "Sp / Df were used by something else meanwhile") : rc == -2 ? "its vertex / index bytes changed" : "the draw path dropped it");
    }
}

// ---------------------------------------------------------------- Sky
static int sky_stash(const vkn_draw_in *in, const vkn_fx_ctx *ctx, const vkn_fx_out *out) {
    stash *t = &g.st; const vkn_shader *s = ctx->shader;
    if ((s->stages & 7u) != ((1u << VKN_SRC_VS) | (1u << VKN_SRC_FS)) || s->missing || s->unusable || in->alpha_ref >= 0.0f || out->shader || out->discard || out->no_fs ||
        ncolor(ctx) != 2 || !ctx->color[0] || !ctx->color[1] || ctx->depth)
        return VKN_LF_W_SKY_SHAPE;
    t->valid = 1; t->frame = ctx->frame; t->serial = vkn_cb_serial(); t->sky = s;
    for (int k = 0; k < NR_LF_NX; k++) {
        const vkn_bind *b = bind_of(s, nr_lf_x[k].sky);
        if (!b || b->stage != 1 || (nr_lf_x[k].kind == NR_LF_TEX ? b->kind != VKN_BK_TEXTURE || b->slot >= VKN_TEX_SLOTS : b->kind != VKN_BK_UNIFORM || b->slot >= VKN_UBO_SLOTS))
            return VKN_LF_W_SKY_SHAPE;
        if (nr_lf_x[k].kind == NR_LF_TEX) {
            VkImageView v; vkn_tex *x = tex_at(in, 1, b->slot, &v);
            if (!x || !hold(x)) return VKN_LF_W_SKY_STASH;
            const int32_t sid = in->smp[1] ? in->smp[1][b->slot] : -1;
            t->xt[k].view = v; t->xt[k].tex = x; t->xt[k].sampler = sid >= 0 ? vkn_sampler_by_id((uint32_t)sid) : VK_NULL_HANDLE;
        } else {
            uint32_t off = 0, size = 0;
            if (vkn_draw_ubo_get(1, b->slot, &off, &size) || !size) return VKN_LF_W_SKY_STASH;
            t->xu[k - NR_LF_NTEX].buf = VK_NULL_HANDLE; t->xu[k - NR_LF_NTEX].off = off; t->xu[k - NR_LF_NTEX].size = size;
        }
    }
    // the whole draw (a recovery draws it again): slot arrays copied, every block the table binds, the target set, the bytes drawn
    t->in = *in;
    for (int st = 0; st < 2; st++) {
        for (uint32_t i = 0; i < VKN_TEX_SLOTS; i++) { t->tex[st][i] = in->tex[st] ? in->tex[st][i] : -1; t->smp[st][i] = in->smp[st] ? in->smp[st][i] : -1; }
    }
    t->first = ctx->first; t->count = ctx->count; t->instances = ctx->instances; t->indexed = ctx->indexed; t->prim = ctx->prim; t->vfmt = ctx->vfmt;
    t->nub = 0;
    for (uint32_t i = 0; i < s->nbind; i++) {
        const vkn_bind *b = &s->bind[i]; const int st = src_of(b->stage);
        if (b->kind != VKN_BK_UNIFORM || st < 0 || b->slot >= VKN_UBO_SLOTS) continue;
        if (t->nub >= 24 || vkn_draw_ubo_get(st, b->slot, &t->ub[t->nub].off, &t->ub[t->nub].size)) return VKN_LF_W_SKY_STASH;
        t->ub[t->nub].st = (uint8_t)st; t->ub[t->nub].slot = b->slot; t->nub++;
    }
    t->vub_ok = 0;
    for (uint32_t sl = 0; sl < VKN_UBO_SLOTS; sl++) if (!vkn_draw_ubo_get(0, sl, &t->vub_off[sl], &t->vub_size[sl])) t->vub_ok |= 1u << sl;
    vkn_pass_cur_get(&t->tn, t->tc, t->tl, t->tm, &t->td);
    t->sp = ctx->color[0]; t->df = ctx->color[1];
    if (!hold(t->sp) || !hold(t->df)) return VKN_LF_W_SKY_STASH;
    if (vsnap_take(in, ctx->vfmt, ctx->indexed, ctx->first, ctx->count, ctx->instances, &t->vs)) return VKN_LF_W_SKY_STASH;
    tsnap_take(t->sp, &t->snap[0]); tsnap_take(t->df, &t->snap[1]);
    t->df_zero = zero_clear(t->df); t->sp_zero = zero_clear(t->sp);
    return -1;
}
static void sky_draw(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out) {
    g.s.sky++;
    if (g.frame_sky != ctx->frame) { g.frame_sky = ctx->frame; g.s.frames++; }
    recover("the next Sky draw", ctx);   // a skipped Sky still waiting: drawn before this one
    stash_drop();
    const int w = sky_stash(in, ctx, out);
    if (w >= 0) {
        g.s.why[w]++;
        lf_red(R_SKY, "Sky draw refused: %s (colour targets %u, depth %d, shader %#llx)", vkn_lf_reason_name(0, w), ncolor(ctx), ctx->depth ? 1 : 0, (unsigned long long)ctx->shader->handle);
        stash_drop(); return;
    }
    // learned implies a fused shader was made for the Light that taught it (VKN_LF_W_TWIN is one of its checks)
    if (nr_lf_sky(&g.ls, g.mode, 1) == NR_LF_SKY_SKIP) { out->skip = 1; g.st.skipped = 1; g.s.sky_skipped++; }
}

// ---------------------------------------------------------------- Light
static int light_check(const vkn_draw_in *in, const vkn_fx_ctx *ctx, const vkn_fx_out *out, const fused **fu) {
    const stash *t = &g.st; const vkn_shader *s = ctx->shader;
    *fu = NULL;
    if (!t->valid || t->frame != ctx->frame || t->serial != vkn_cb_serial() || t->light_done) return VKN_LF_W_NO_STASH;
    if ((s->stages & 7u) != ((1u << VKN_SRC_VS) | (1u << VKN_SRC_FS)) || s->missing || s->unusable || in->alpha_ref >= 0.0f || out->shader || out->discard || out->no_fs)
        return VKN_LF_W_LIGHT_FS;
    if (ncolor(ctx) != 1 || ctx->color[0] != t->df || ctx->depth) return VKN_LF_W_LIGHT_RT;
    const VkPipelineColorBlendAttachmentState *b = ctx->blend ? vkn_blend_att(ctx->blend) : NULL;
    if (!b || !nr_lf_blend_ok(b->blendEnable, (uint32_t)b->srcColorBlendFactor, (uint32_t)b->dstColorBlendFactor, (uint32_t)b->colorBlendOp, (uint32_t)vkn_write_mask_nvn(t->df->nvn_fmt)))
        return VKN_LF_W_BLEND;
    if (t->skipped && (!tsnap_same(t->sp, &t->snap[0]) || !tsnap_same(t->df, &t->snap[1]))) return VKN_LF_W_TOUCHED;   // (drawn: Sky's own pass changed them)
    if (!t->df_zero) return VKN_LF_W_CLEAR;
    if (tex_of(s, in, LB_SP) != t->sp) return VKN_LF_W_AMBIENT;
    if (!same_vs(t->sky->key[VKN_SRC_VS], s->key[VKN_SRC_VS])) return VKN_LF_W_VS;
    vsnap now;
    if (ctx->vfmt != t->vfmt || !vs_blocks_same(s) || !vs_tex_same(s, in) || vsnap_take(in, ctx->vfmt, ctx->indexed, ctx->first, ctx->count, ctx->instances, &now) || !vsnap_same(&now, &t->vs))
        return VKN_LF_W_VS_DATA;
    if (!draw_same(in, ctx)) return VKN_LF_W_DRAW;
    if (retype_code(s, ctx->vfmt)) return VKN_LF_W_RETYPE;
    if (!(*fu = fused_for(s, 'f'))) return VKN_LF_W_TWIN;
    return -1;
}
// What the Light sees of the Fog's inputs (xbg_nr_lightfuse.inc light: bindings 22 / 82 / 100); 1 = kept.
static int light_fog_view(const vkn_draw_in *in, const vkn_shader *s) {
    stash *t = &g.st;
    const vkn_bind *b16 = bind_of(s, LB_C16), *b15 = bind_of(s, LB_C15);
    t->depth = tex_of(s, in, LB_DEPTH);
    if (!t->depth || !b16 || !b15 || b16->kind != VKN_BK_UNIFORM || b15->kind != VKN_BK_UNIFORM || b16->stage != 1 || b15->stage != 1) return 0;
    return !ring_bytes(b16->slot, 64, 48, t->c16) && !ring_bytes(b15->slot, 192, 16, t->c15);
}
static void fill_out(vkn_fx_out *o, const fused *fu) {
    o->shader = fu->fx;
    for (int k = 0; k < NR_LF_NTEX; k++) o->tex[k] = g.st.xt[k];
    o->ubo[0] = g.st.xu[0]; o->ubo[1] = g.st.xu[1]; o->ubo[2] = fu->c1[0]; o->ubo[3] = fu->c1[1];
}
static void light_draw(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out) {
    g.s.light++;
    stash *t = &g.st;
    const int skipped = t->valid && t->skipped;
    const fused *fu = NULL;
    const int w = light_check(in, ctx, out, &fu);
    if (w != VKN_LF_W_NO_STASH) t->light_done = 1;
    const int was = g.ls.learned;
    const int act = nr_lf_light(&g.ls, g.mode, skipped, w < 0);
    if (g.ls.learned && !was) g.s.learn++;
    if (!g.ls.learned && was && act != NR_LF_LIGHT_LOST) g.s.unlearn++;
    if (w >= 0) {
        g.s.why[w]++;
        if (w != VKN_LF_W_NO_STASH || skipped) lf_red(R_LIGHT, "Light draw refused: %s (Sky %s)", vkn_lf_reason_name(0, w), skipped ? "skipped: drawn now, then this Light as recorded" : "drawn");
    } else if (!skipped) g.s.would++;
    if (act == NR_LF_LIGHT_LOST) { recover("a Light draw that failed a check", ctx); return; }   // (recover counts the unlearn)
    if (w >= 0) return;
    t->light_ok = 1;
    const int fog_view = g.fog && light_fog_view(in, ctx->shader);
    if (!fog_view) t->depth = NULL;
    if (!fog_view) t->light_ok = 0;   // (the Fog checks need the Light's view of its inputs)
    if (act != NR_LF_LIGHT_FUSED) return;
    t->skipped = 0;
    // variant B: Sky + Fog in one draw into [Df, Sp] (after a Fog that passed every check)
    if (g.fog == 1 && g.fog_learned && fog_view) {
        const fused *fg = NULL; int fw = -1;
        if (!t->sp_zero || !zero_clear(t->sp)) fw = VKN_LF_F_SPCLEAR;
        else if (!(fg = fused_for(ctx->shader, 'g'))) fw = VKN_LF_F_TWIN;
        if (fw >= 0) { g.s.fog_why[fw]++; lf_red(R_FOG, "Fog not fused into this Light: %s", vkn_lf_reason_name(1, fw)); }
        else {
            vkn_fx_out o = *out; fill_out(&o, fg);
            uint32_t n0 = 0, lay[VKN_PASS_COLORS], mip[VKN_PASS_COLORS], l2[VKN_PASS_COLORS] = {0}, m2[VKN_PASS_COLORS] = {0}; vkn_tex *col[VKN_PASS_COLORS], *dep = NULL, *c2[VKN_PASS_COLORS] = {0};
            vkn_pass_cur_get(&n0, col, lay, mip, &dep);
            c2[0] = col[0]; l2[0] = lay[0]; m2[0] = mip[0]; c2[1] = t->sp; l2[1] = t->tl[0]; m2[1] = t->tm[0];   // (Sp as Sky's set had it)
            vkn_pass_targets(2, c2, l2, m2, NULL);
            vkn_fx_force(&o);
            const int rc = draw_again(in, ctx->indexed, ctx->first, ctx->count, ctx->instances);
            vkn_fx_force(NULL);
            vkn_pass_targets(n0, col, lay, mip, dep);
            if (!rc) { out->skip = 1; t->fog_fused = 1; g.s.fog_fused++; g.s.fused++; return; }
            lf_red(R_FUSED, "the fused Sky + Fog draw into [Df, Sp] was dropped by the draw path: the Sky-only fused draw instead");
        }
    }
    fill_out(out, fu);   // variant A (out may hold B's copy after a dropped B: rewritten whole here)
    out->skip = 0;
    if (g.mode == 3) { static const float mg[4] = {1, 0, 1, 1}; vkn_pass_clear_color(t->sp, 0, mg, NULL); g.s.poison++; }   // Sp's remembered clear: a reader of Sky's Sp shows magenta
    g.s.fused++;
}

// ---------------------------------------------------------------- Fog and the window on C
static int fog_check(const vkn_draw_in *in, const vkn_fx_ctx *ctx, const vkn_fx_out *out) {
    const stash *t = &g.st; const vkn_shader *s = ctx->shader;
    if (!t->valid || !t->light_ok || !t->depth || t->frame != ctx->frame) return VKN_LF_F_NOLIGHT;
    if ((s->stages & 7u) != ((1u << VKN_SRC_VS) | (1u << VKN_SRC_FS)) || s->missing || s->unusable || in->alpha_ref >= 0.0f || out->shader || out->discard || out->no_fs)
        return VKN_LF_F_STAGES;
    if (ncolor(ctx) != 1 || !ctx->color[0] || ctx->depth) return VKN_LF_F_RT;
    const VkPipelineColorBlendAttachmentState *b = ctx->blend ? vkn_blend_att(ctx->blend) : NULL;
    if (b && b->blendEnable) return VKN_LF_F_BLEND;
    vkn_tex *c = ctx->color[0];
    if (!nr_lf_fog_mask_ok((uint32_t)vkn_write_mask_nvn(c->nvn_fmt), (uint32_t)c->format)) return VKN_LF_F_MASK;
    const vkn_bind *bs = bind_of(s, FB_SCENE), *bd = bind_of(s, FB_DEPTH), *b16 = bind_of(s, FB_C16), *b15 = bind_of(s, FB_C15);
    if (!bs || !bd || !b16 || !b15 || bs->kind != VKN_BK_TEXTURE || bd->kind != VKN_BK_TEXTURE || b16->kind != VKN_BK_UNIFORM || b15->kind != VKN_BK_UNIFORM ||
        bs->stage != 1 || bd->stage != 1 || b16->stage != 1 || b15->stage != 1)
        return VKN_LF_F_BIND;
    if (c == t->sp || c == t->df || c->format != t->sp->format || c->nvn_fmt != t->sp->nvn_fmt || c->w != t->sp->w || c->h != t->sp->h || c->w != t->df->w || c->h != t->df->h ||
        c->mips != 1 || t->sp->mips != 1 || c->layers != 1 || t->sp->layers != 1 || c->samples != t->sp->samples || c->nvn_target != t->sp->nvn_target)
        return VKN_LF_F_TARGET;
    if (tex_at(in, 1, bs->slot, NULL) != t->df) return VKN_LF_F_SCENE;
    if (tex_at(in, 1, bd->slot, NULL) != t->depth) return VKN_LF_F_DEPTH;
    uint8_t c16[48], c15[16];
    if (ring_bytes(b16->slot, 64, 48, c16) || ring_bytes(b15->slot, 192, 16, c15) || memcmp(c16, t->c16, 48) || memcmp(c15, t->c15, 16)) return VKN_LF_F_CONST;
    vsnap now;
    if (!same_vs(t->sky->key[VKN_SRC_VS], s->key[VKN_SRC_VS]) || ctx->vfmt != t->vfmt || !vs_blocks_same(s) || !vs_tex_same(s, in) ||
        vsnap_take(in, ctx->vfmt, ctx->indexed, ctx->first, ctx->count, ctx->instances, &now) || !vsnap_same(&now, &t->vs))
        return VKN_LF_F_VS;
    if (!draw_same(in, ctx)) return VKN_LF_F_DRAW;
    if (in->viewport[0] != 0.0f || in->viewport[1] != 0.0f || in->viewport[2] != (float)c->w || in->viewport[3] != (float)c->h ||
        in->scissor[0] > 0 || in->scissor[1] > 0 || (int64_t)in->scissor[0] + in->scissor[2] < (int64_t)c->w || (int64_t)in->scissor[1] + in->scissor[3] < (int64_t)c->h)
        return VKN_LF_F_COVER;
    return -1;
}
static void rd_close(void) {
    if (!g.rd.on) return;
    vkn_tex_unref(g.rd.c); vkn_tex_unref(g.rd.sp);
    memset(&g.rd, 0, sizeof g.rd);
}
// C as the skipped Fog would have left it (exact: the Fog covered C; same format, one level / layer). [stage 5 rest] Not an error: the
// designed end of a window whose C is needed as C (a draw renders onto it, or a reader the twin path cannot serve). Counted by reason
// (census "Sp -> C copies"); the first 4 of a run logged as plain lines with the draw that asked for C. Was a red line every run (16 of
// ~58 at the main menu -> world transition, phone 21.11): the copy is a full-screen copy, but only in the frames of that fade.
static void rd_copy(int why, const vkn_fx_ctx *ctx) {
    vkn_pass_copy(g.rd.c, g.rd.sp);
    g.s.redir_copies++;
    if (why >= 0 && why < VKN_LF_CP_N) g.s.copy_by[why]++;
    static unsigned told;
    if (told < 4) {
        told++;
        vkn_logf("lightfuse fog: Sp copied into the skipped Fog's target %ux%u (%s; draw %u of frame %llu, shader %.16s / %.16s) - exact, counted from now on",
                 g.rd.c->w, g.rd.c->h, vkn_lf_reason_name(2, why), ctx->draw_no, (unsigned long long)ctx->frame,
                 ctx->shader ? ctx->shader->key[VKN_SRC_VS] : "?", ctx->shader ? ctx->shader->key[VKN_SRC_FS] : "?");
    }
    rd_close();
}
static void fog_draw(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out) {
    g.s.fog++;
    stash *t = &g.st;
    const int fused = t->fog_fused; t->fog_fused = 0;
    const int w = fog_check(in, ctx, out);
    t->light_ok = 0;
    if (w >= 0) {
        g.s.fog_why[w]++; g.fog_learned = 0;
        if (fused) g.s.fog_back++;   // drawn as recorded: exact (Df holds the Light's result; Sp got the fog, nobody reads it)
        // [gpu2 small] a partial Fog (dynamic resolution draws a part of the target, T06-T08) is the designed refusal: the Fog draws as
        // recorded (exact). Not an error: the first 4 of a run as plain lines, then only the census count (line "lightfuse fog ...", "refused: the Fog draw does not cover its target N")
        if (w == VKN_LF_F_COVER) {
            static unsigned told;
            if (told < 4) { told++; vkn_logf("lightfuse fog: Fog draw refused (designed): %s, viewport %gx%g of %ux%u%s - drawn as recorded, exact; counted from now on",
                                             vkn_lf_reason_name(1, w), in->viewport[2], in->viewport[3], ctx->color[0] ? ctx->color[0]->w : 0u, ctx->color[0] ? ctx->color[0]->h : 0u,
                                             fused ? " (after a fused Sky + Fog Light)" : ""); }
            return;
        }
        if (w != VKN_LF_F_NOLIGHT || fused) lf_red(R_FOG, "Fog draw refused: %s%s", vkn_lf_reason_name(1, w), fused ? " (after a fused Sky + Fog Light: the Fog draws as recorded)" : "");
        return;
    }
    g.s.fog_ok++;
    if (g.fog == 1 && (g.mode == 1 || g.mode == 3)) g.fog_learned = 1;
    if (!fused) return;
    vkn_tex *c = ctx->color[0];
    if (g.mode == 3) { static const float mg[4] = {1, 0, 1, 1}; vkn_pass_clear_color(c, 0, mg, NULL); g.s.poison++; }   // a reader the window misses shows magenta
    rd_close();
    if (vkn_tex_ref(c)) {
        if (vkn_tex_ref(t->sp)) { g.rd.on = 1; g.rd.c = c; g.rd.sp = t->sp; tsnap_take(c, &g.rd.snap); g.s.redir_windows++; }
        else vkn_tex_unref(c);
    }
    if (!g.rd.on) {   // no window possible: the Fog draws (exact: Df holds the Light's result)
        g.s.fog_back++; lf_red(R_WINDOW, "no reference on the Fog's target / Sp: the Fog draws as recorded"); return;
    }
    out->skip = 1; g.s.fog_skipped++;
}
// A reader of C inside the window: a shader object of its own whose texture bindings of C read Sp (made once per reader and pass).
static const twin *twin_for_locked(const vkn_shader *s, uint64_t cmask);
static const twin *twin_for(const vkn_shader *s, uint64_t cmask) {
    pthread_mutex_lock(&g_mk_mu); const twin *t = twin_for_locked(s, cmask); pthread_mutex_unlock(&g_mk_mu);
    return t;
}
static const twin *twin_for_locked(const vkn_shader *s, uint64_t cmask) {
    const VkShaderModule vs = vkn_shader_module(s, VKN_STAGE_VS, 0, 0.0f, 0), fs = vkn_shader_module(s, VKN_STAGE_FS, 0, 0.0f, 0);
    if (!vs || !fs) return NULL;
    for (uint32_t i = 0; i < g.ntw; i++) {
        const twin *x = &g.tw[i];
        if (x->s == s && x->handle == s->handle && x->cmask == cmask && x->vs == vs && x->fs == fs) return x->fx ? x : NULL;
    }
    if (g.ntw >= TWINS) return NULL;
    twin *x = &g.tw[g.ntw++]; memset(x, 0, sizeof *x);
    x->s = s; x->handle = s->handle; x->cmask = cmask; x->vs = vs; x->fs = fs;
    vkn_bind *tab = (vkn_bind *)calloc(s->nbind ? s->nbind : 1, sizeof *tab);
    const char *why = tab ? NULL : "out of memory";
    for (uint32_t i = 0; !why && i < s->nbind; i++) {
        vkn_bind b = s->bind[i]; const int st = src_of(b.stage);
        if (b.count != 1) why = "a binding with more than one descriptor";
        else if (b.kind == VKN_BK_TEXTURE && st >= 0 && b.slot < VKN_TEX_SLOTS && (cmask >> (st * 32 + b.slot) & 1)) {
            if (x->nt >= VKN_FX_TEX || (b.flags & VKN_BF_CMP)) why = "too many / comparison-sampled texture bindings of C";
            else { x->tst[x->nt] = (uint8_t)st; x->tslot[x->nt] = b.slot; b.kind = VKN_BK_FX_TEX; b.slot = x->nt++; }
        } else if (b.kind == VKN_BK_C1) { if (c1_to_fx(&b, s, 0, x->c1)) why = "C1 constants without their static block"; }
        else if (b.kind != VKN_BK_UNIFORM && b.kind != VKN_BK_SUPPORT && b.kind != VKN_BK_TEXTURE) why = "a binding of a kind a piece's shader cannot have";
        if (!why) tab[i] = b;
    }
    if (!why && !(x->fx = make_fx("lightfuse-rd", vs, fs, s, tab, s->nbind))) why = "vkn_fx_shader_create refused the table";
    free(tab);
    if (why) { lf_red(R_TWIN, "reader shader %#llx (%s) of the skipped Fog's target: %s - Sp is copied instead", (unsigned long long)s->handle, s->key[VKN_SRC_FS], why); return NULL; }
    g.s.twins++;
    if (vkn_warm_recipe_fx) { uint8_t a[9]; a[0] = 'r'; memcpy(a + 1, &cmask, 8); vkn_warm_recipe_fx(x->fx, "lightfuse", s, a, sizeof a); }   // [stage 5 rest]
    vkn_logf("lightfuse fog: reader shader %#llx (%s): %u texture binding(s) of C read Sp (shader object of its own)", (unsigned long long)s->handle, s->key[VKN_SRC_FS], x->nt);
    return x;
}
static void redirect(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out) {
    vkn_tex *c = g.rd.c;
    if (!tsnap_same(c, &g.rd.snap)) {   // C used by something that is not a draw (copy, clear, dispatch): the window ends
        if (c->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || c->layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || c->layout == VK_IMAGE_LAYOUT_GENERAL) {
            g.s.redir_lost++; lf_red(R_WINDOW, "the skipped Fog's target was read by something that is not a draw (layout %d): it lacked the fog there", (int)c->layout);
        }
        rd_close(); return;
    }
    if (ctx->frame != g.frame) { g.s.redir_open_end++; lf_red(R_WINDOW, "the window on the skipped Fog's target was still open at a frame change (C was not written since)"); rd_close(); return; }
    for (uint32_t i = 0; i < VKN_PASS_COLORS; i++) if (ctx->color[i] == c) { rd_copy(VKN_LF_CP_TARGET, ctx); return; }
    if (ctx->depth == c) { rd_copy(VKN_LF_CP_TARGET, ctx); return; }
    for (int k = 0; k < VKN_FX_TEX; k++) if (out->tex[k].tex == c) { rd_copy(VKN_LF_CP_EFFECT, ctx); return; }
    const vkn_shader *s = out->shader ? out->shader : ctx->shader;
    uint64_t cmask = 0;
    for (uint32_t i = 0; s && i < s->nbind; i++) {
        const vkn_bind *b = &s->bind[i]; const int st = src_of(b->stage);
        if (b->kind == VKN_BK_TEXTURE && st >= 0 && b->slot < VKN_TEX_SLOTS && tex_at(in, st, b->slot, NULL) == c) cmask |= 1ull << (st * 32 + b->slot);
    }
    if (!cmask) return;
    const twin *x = NULL;
    if (!out->shader && !out->discard && !out->no_fs && in->alpha_ref < 0.0f && !retype_code(s, ctx->vfmt) && (s->stages & 7u) == ((1u << VKN_SRC_VS) | (1u << VKN_SRC_FS)) &&
        !s->missing && !s->unusable)
        x = twin_for(s, cmask);
    if (!x) { rd_copy(VKN_LF_CP_READER, ctx); return; }   // (alpha test / retype / effect / kinds / cache full)
    out->shader = x->fx;
    for (uint32_t k = 0; k < x->nt; k++) {
        const int32_t sid = in->smp[x->tst[k]] ? in->smp[x->tst[k]][x->tslot[k]] : -1;
        out->tex[k].view = g.rd.sp->view; out->tex[k].tex = g.rd.sp; out->tex[k].sampler = sid >= 0 ? vkn_sampler_by_id((uint32_t)sid) : VK_NULL_HANDLE;
    }
    out->ubo[0] = x->c1[0]; out->ubo[1] = x->c1[1];
    g.s.redir_draws++;
}

// ---------------------------------------------------------------- the chain function
static void lf_fx(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)user;
    if (!g.mode) return;   // (host test: switched off again after its block; the chain cannot drop a function)
    const int r = role_of(ctx->shader);
    if (g.rd.on) redirect(in, ctx, out);
    if (ctx->frame != g.frame) {   // a new frame: a skipped Sky still waiting is lost (its blocks lived for its frame)
        if (g.st.valid && g.st.skipped) recover("the next frame", ctx);
        if (g.st.valid) stash_drop();
        g.frame = ctx->frame;
    }
    if (g.st.skipped && r != LF_LIGHT && r != LF_SKY) recover("a draw that is not the Light", ctx);
    if (r == LF_NONE) return;
    if (r == LF_SKY) sky_draw(in, ctx, out);
    else if (r == LF_LIGHT) light_draw(in, ctx, out);
    else if (g.fog) fog_draw(in, ctx, out);
}

// ---------------------------------------------------------------- install, census
static void keys(void) {
    static const char *nm[4] = {NULL, "DeferredSky", "ClusteredLightingWithSSAO", "DeferredFog"};
    for (int k = LF_SKY; k <= LF_FOG; k++) {
        const int i = nr_sh_find_name(nm[k]);
        if (i >= 0) snprintf(g.key[k], sizeof g.key[k], "%.16s", nr_sh_table[i].key);
        else { g.key[k][0] = 0; lf_red(R_CFG, "no %s in the NR-04 table: that role is never recognised", nm[k]); }
    }
}
static int reg_fn(void) {
    if (!g.reg) g.reg = !vkn_fx_register("lightfuse", lf_fx, NULL, 300) ? 1 : -1;
    return g.reg > 0;
}
// [stage 5 rest] the warm-up worker's maker (vkn_warm.h recipe "lightfuse"): 'f' / 'g' the fused shader of Light shader eng, 'r' + the
// 8-byte mask the Fog-window reader twin of eng. The object the draws get (made once, under g_mk_mu).
static int lf_make(const struct vkn_shader *eng, const void *arg, uint32_t n, const struct vkn_shader **fx, VkShaderModule *vs, uint16_t *vs_id) {
    (void)vs; (void)vs_id;
    const uint8_t *a = (const uint8_t *)arg;
    if (!g.inst || !eng || !a || !n || !fx) return -1;
    if ((a[0] == 'f' || a[0] == 'g') && n == 1) { if (a[0] == 'g' && !g.fog) return -1; const fused *f = fused_for(eng, (char)a[0]); *fx = f ? f->fx : NULL; }
    else if (a[0] == 'r' && n == 9 && g.fog) { uint64_t m; memcpy(&m, a + 1, 8); const twin *t = twin_for(eng, m); *fx = t ? t->fx : NULL; }
    else return -1;
    return *fx ? 0 : -1;
}
extern int vkn_warm_maker(const char *piece, int (*fn)(const struct vkn_shader *, const void *, uint32_t, const struct vkn_shader **, VkShaderModule *, uint16_t *)) __attribute__((weak));
int vkn_hacks_lightfuse_ok(void) __attribute__((weak));   // [stage 5.4 G1] vkn_hacks.h: DM_HACK_LIGHTS' cap module replaces the Light module
void vkn_lf_install(void) {
    static int done; if (done) return; done = 1;
    g.retype = -1;
    const char *e = getenv("DM_VK_NATIVE_LIGHTFUSE"); int m = e && *e ? atoi(e) : 1;   // xbg_nr_lightfuse_default: 1 on every family
    if (m < 0 || m > 3) { lf_red(R_CFG, "DM_VK_NATIVE_LIGHTFUSE=%s is not 0 / 1 / 2 / 3: off", e); m = 0; }
    if (m && vkn_hacks_lightfuse_ok && !vkn_hacks_lightfuse_ok()) m = 0;   // [stage 5.4 G1] the player's light cap wins (DM_VK_NATIVE_LCAP=1; one line there)
    const char *f = getenv("DM_VK_NATIVE_LIGHTFUSE_FOG"); int v = f && *f ? atoi(f) : 1;   // xbg_nr_lightfuse_fog_default: 1
    if (v < 0 || v > 2) { lf_red(R_CFG, "DM_VK_NATIVE_LIGHTFUSE_FOG=%s is not 0 / 1 / 2: off", f); v = 0; }
    if (v && !m && f && *f) vkn_logf("lightfuse: DM_VK_NATIVE_LIGHTFUSE_FOG=%d needs DM_VK_NATIVE_LIGHTFUSE: off", v);
    g.mode = m; g.fog = m ? v : 0;
    keys();
    vkn_logf("DM_VK_NATIVE_LIGHTFUSE=%d (stage 5.1 A4, replaces DM_NR_LIGHTFUSE: %s): DeferredSky -> ClusteredLightingWithSSAO as one draw (f.ClusteredLightingWithSSAO of "
             "native-nr.xgspack; the Sky draw is skipped after one checked chain, its clears stay remembered; a missing Light draws the stashed Sky late); "
             "DM_VK_NATIVE_LIGHTFUSE_FOG=%d (replaces DM_NR_LIGHTFUSE_FOG: %s)", m,
             m == 0 ? "off" : m == 1 ? "on" : m == 2 ? "verify - checked and counted, both draws as recorded" : "on + magenta poison of the skipped Sp / Fog target",
             g.fog, g.fog == 0 ? "off" : g.fog == 1 ? (m == 2 ? "no effect: verify" : "DeferredFog inside the fused draw (g., output 1 = Sp), the Fog draw skipped, its readers sample Sp") : "verify - Fog checks counted");
    if (!m) return;
    if (!reg_fn()) { lf_red(R_CFG, "the draw-effect chain refused the function: off"); g.mode = g.fog = 0; return; }
    g.inst = 1;
    if (vkn_warm_maker && g.mode != 2) vkn_warm_maker("lightfuse", lf_make);   // [stage 5 rest] (verify mode draws as recorded: nothing to warm)
}
int vkn_lf_active(void) { return g.inst; }
void vkn_lf_stats_get(vkn_lf_stats *s) { if (s) *s = g.s; }
void vkn_lf_test_setup(int mode, int fog) {
    stash_drop(); rd_close();
    const int reg = g.reg;
    memset(&g, 0, sizeof g); g.reg = reg; g.retype = -1;
    memset(g_role_s, 0, sizeof g_role_s);
    g.mode = mode; g.fog = mode ? fog : 0;
    keys();
    g.inst = mode ? reg_fn() : 0;
}

typedef struct txt { char b[1600]; size_t k; } txt;
static void put(txt *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(txt *t, const char *fmt, ...) {
    if (t->k >= sizeof t->b - 1) return;
    va_list a; va_start(a, fmt); const int w = vsnprintf(t->b + t->k, sizeof t->b - t->k, fmt, a); va_end(a);
    if (w > 0) t->k = t->k + (size_t)w < sizeof t->b - 1 ? t->k + (size_t)w : sizeof t->b - 1;
}
void vkn_lf_census(void) {
    if (!g.inst) return;
    typedef unsigned long long ull;
    const vkn_lf_stats *s = &g.s, *o = &g.last;
#define D(f) ((ull)(s->f - o->f))
    if (!D(sky) && !D(light) && !D(fog) && !D(reds)) return;
    txt a; a.k = 0; a.b[0] = 0;
    put(&a, "lightfuse (DM_VK_NATIVE_LIGHTFUSE=%d, last 10 s): frames with a Sky draw %llu; Sky draws %llu, skipped %llu; Light draws %llu: fused %llu, passed every check "
        "with Sky drawn (%s) %llu; skipped Sky drawn late (exact) %llu, lost %llu; learned %d (learn %llu, unlearn %llu); refused:", g.mode, D(frames), D(sky), D(sky_skipped),
        D(light), D(fused), g.mode == 2 ? "verify: would fuse" : "learning", D(would), D(recovered), D(lost), g.ls.learned, D(learn), D(unlearn));
    int any = 0;
    for (int i = 0; i < VKN_LF_W_N; i++) if (s->why[i] != o->why[i]) { put(&a, "%s %s %llu", any ? "," : "", vkn_lf_reason_name(0, i), (ull)(s->why[i] - o->why[i])); any = 1; }
    put(&a, "%s; poison clears %llu; fused shaders made %u; red lines %llu", any ? "" : " none", D(poison), g.pieces, D(reds));
    vkn_logf("%s", a.b);
    if (g.fog) {
        txt b; b.k = 0; b.b[0] = 0;
        put(&b, "lightfuse fog (DM_VK_NATIVE_LIGHTFUSE_FOG=%d): Fog draws %llu, passed every check %llu, fused into the Light %llu, skipped %llu, drawn as recorded after a fused Light %llu; "
            "learned %d; draws reading Sp for C %llu (windows %llu, Sp -> C copies %llu, reader shaders %llu so far), C read by a non-draw inside a window %llu, windows open at a frame change %llu; refused:",
            g.fog, D(fog), D(fog_ok), D(fog_fused), D(fog_skipped), D(fog_back), g.fog_learned, D(redir_draws), D(redir_windows), D(redir_copies), (ull)s->twins, D(redir_lost), D(redir_open_end));
        if (D(redir_copies)) {   // [stage 5 rest] the copies by reason (the designed end of a window, not an error)
            put(&b, " [copies:");
            for (int i = 0; i < VKN_LF_CP_N; i++) if (s->copy_by[i] != o->copy_by[i]) put(&b, " %s %llu", vkn_lf_reason_name(2, i), (ull)(s->copy_by[i] - o->copy_by[i]));
            put(&b, "]");
        }
        any = 0;
        for (int i = 0; i < VKN_LF_F_N; i++) if (s->fog_why[i] != o->fog_why[i]) { put(&b, "%s %s %llu", any ? "," : "", vkn_lf_reason_name(1, i), (ull)(s->fog_why[i] - o->fog_why[i])); any = 1; }
        put(&b, "%s", any ? "" : " none");
        vkn_logf("%s", b.b);
    }
#undef D
    g.last = g.s;
}
#endif
