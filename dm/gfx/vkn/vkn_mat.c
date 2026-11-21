// [DOS2] VK-NATIVE stage 5.2 B5: MAT + XLQ (vkn_mat.h has the rule, the exactness argument, the switches). One vkn_fx function
// (order 200). A draw into a colour pass costs one pointer compare while XLQ is off; a depth-only draw one table load per shader id
// (+ the gate terms of a gated shader: a few words of the ring). Ported from xbg_nr_mat.inc (namespace nrmat: on / open_pack / get /
// note / rd / pre / tick) and xbg_game.cpp's NR-32 (vsout_module, xlq_keep, the pipeline rule of draw()). Also installs the pre-pass
// hybrid (vkn_prepass.c) from vkn_b5_install, the library's one call. No game addresses.
#ifdef DM_GAME_DOS2
#include "vkn_mat.h"
#include "vkn_buf.h"
#include "vkn_draw.h"
#include "vkn_frame.h"
#include "vkn_pass.h"
#include "vkn_shader.h"
#include "vkn_state.h"
#include "../nr/nr_mat.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
#define VSOUT_ALL (~(uint64_t)0)
// the shader library's doors (vkn_shlib.cpp block "stage 5.2 B5", vkn_mat_lib.cpp; weak: host tests run without them)
extern int vkn_shlib_on(void) __attribute__((weak));
extern int vkn_shlib_b5_words(void *shader, uint64_t retype, uint32_t **words, size_t *nwords, uint8_t **raw, size_t *nraw) __attribute__((weak));
extern VkShaderModule vkn_shlib_b5_module(const uint32_t *words, size_t nwords, const char *key) __attribute__((weak));
extern int vkn_shlib_b5_record(const char *pack, const char *key, uint8_t **out, size_t *n) __attribute__((weak));
extern int vkn_shlib_named_entry(const char *fs_key, uint64_t *dref_mask) __attribute__((weak));
extern VkShaderModule vkn_shlib_alpha_module(void *shader, uint32_t func, float ref) __attribute__((weak));
extern void vkn_prepass_install(uint32_t vendor, const char *device_name) __attribute__((weak));   // vkn_prepass.c (stage 5.2 B4)
extern uint64_t vkn_mat_fs_read(const uint32_t *w, size_t n, uint64_t *declared) __attribute__((weak));
extern uint64_t vkn_mat_vs_written(const uint32_t *w, size_t n) __attribute__((weak));
extern int vkn_mat_vsout(const uint32_t *w, size_t n, uint64_t keep, uint64_t declared, uint32_t **out, size_t *out_n, uint32_t *stores, uint32_t *vars,
                         const char **why) __attribute__((weak));

enum { C_NONE = 0, C_PURE = 1, C_KILL = 2 };
// [stage 5 rest] variant B and XLQ no longer make effect-shader objects (896 of the 1024 at most, [fixpp]): their trimmed vertex
// modules ride on the engine shader object through vkn_fx_out.vin (vs + vs_id, vfmt 0: the PRESKIN door, vkn_fx.h), so the whole pack
// fits. One module per (vertex key, retype code, keep, declared) in a process-wide table (vin_module: the warm-up worker asks it too,
// vkn_mat_warm_module), at most MODULES of them.
enum { IDS = 16384, CODES = 4, XVARS = 6, MODULES = 8192 };
enum { R_CFG, R_GATE, R_TRIM, R_MODULE, R_TABLE, R_FACTS, R_N };
typedef struct vs_var { uint64_t code; VkShaderModule m; uint16_t vs_id; uint8_t done; } vs_var;   // variant B per retype code
typedef struct x_var { uint64_t code; VkShaderModule m; uint16_t vs_id; uint8_t done; } x_var;      // XLQ per retype code (the draw path picks
                                                                                                  // the fragment module, alpha variants included)
typedef struct mcls {
    const vkn_shader *s; uint64_t handle;
    uint8_t cls, why_none;                 // C_*; why_none: VKN_MAT_W_FACTS / _NOLIB when cls is C_NONE
    uint8_t xlq_ok;                        // 1 = the masks are known and a trim may exist
    nr_mat_gate *gate;                     // NULL = no record
    uint32_t aref; int8_t aapplied;        // the alpha test of reference bits aref: 1 the "#at" variant is drawn, 0 refused (base), -1 unknown
    uint16_t vfmt; uint64_t code; uint8_t code_ok;   // the retype code of the last vertex format
    uint64_t fs_read, fs_decl, vs_written;
    vs_var vs[CODES]; uint8_t nvs;
    x_var xv[XVARS]; uint8_t nxv;
} mcls;

static struct {
    int mode, xlq, inst, reg, retype;      // retype: the draw path's DM_GFX_VTX_RETYPE (-1 not read)
    vkn_mat_stats s, last;
    uint32_t reds[R_N];
    uint32_t own;                          // trimmed vertex modules made by this piece (vin_module)
    uint64_t next_ns;
    vkn_mat_test_fn test;
} g = {.retype = -1};
static mcls *g_tab[IDS];

static void mred(int kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void mred(int kind, const char *fmt, ...) {
    g.s.reds++;
    if (kind < 0 || kind >= R_N || g.reds[kind]++ >= 16) return;
    char b[640]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    vkn_logf(RED "mat: %s", b);
}
const char *vkn_mat_reason_name(int kind, int i) {
    static const char *const w[VKN_MAT_W_N] = {"fragment output / side effect / interface", "no shader library", "gate fails", "alpha test fails .w",
                                               "kill without a gate record", "alpha verdict pending", "not an engine vertex + fragment shader"};
    static const char *const v[VKN_MAT_V_N] = {"nothing to drop", "trim check refused", "module table full", "module failed", "retype codes past 4",
                                               "words not loaded", "a binding a piece's shader cannot have", "the draw has a vertex module of its own (PRESKIN)"};
    static const char *const x[VKN_MAT_X_N] = {"fragment inputs not analysable", "NR-04 named fragment shader", "no depth target", "nothing to drop",
                                               "trim check refused", "module table full", "module failed", "retype codes past 6", "words not loaded",
                                               "a binding a piece's shader cannot have", "the draw has a vertex module of its own (PRESKIN)"};
    if (kind == 0) return i >= 0 && i < VKN_MAT_W_N ? w[i] : "?";
    if (kind == 1) return i >= 0 && i < VKN_MAT_V_N ? v[i] : "?";
    return i >= 0 && i < VKN_MAT_X_N ? x[i] : "?";
}

// ---------------------------------------------------------------- the GPU family (local: the library's init line prints family / policy / sim)
// xbg_vendor_policy.h xbg_gpu_family + xbg_effective_family: DM_GFX_SIM_GPU mali / powervr wins over the real one.
// 0 other, 1 Adreno, 2 Mali, 3 PowerVR, 5 Maleoon.
static int family(uint32_t v, const char *n) {
    const char *sim = getenv("DM_GFX_SIM_GPU");
    if (sim && (strstr(sim, "mali"))) return 2;
    if (sim && (strstr(sim, "pvr") || strstr(sim, "powervr"))) return 3;
    if (v == 0x5143) return 1;
    if (v == 0x13B5) return 2;
    if (v == 0x1010) return 3;
    if (v == 0x19E5) return 5;
    if (n) { if (strstr(n, "Mali")) return 2; if (strstr(n, "PowerVR") || strstr(n, "IMG ")) return 3; if (strstr(n, "Maleoon")) return 5; if (strstr(n, "Adreno")) return 1; }
    return 0;
}
static const char *family_name(int f) { return f == 1 ? "adreno" : f == 2 ? "mali" : f == 3 ? "powervr" : f == 5 ? "maleoon" : "other"; }

// ---------------------------------------------------------------- the library's steps on a thread with a big stack
typedef void (*job_fn)(void *);
typedef struct job { job_fn fn; void *arg; } job;
static void *job_run(void *a) { job *j = (job *)a; j->fn(j->arg); return NULL; }
static void run_big(job_fn fn, void *arg) {
    job j = {fn, arg}; pthread_attr_t at; pthread_t th;
    if (pthread_attr_init(&at)) { fn(arg); return; }
    pthread_attr_setstacksize(&at, 8u << 20);
    const int ok = !pthread_create(&th, &at, job_run, &j);
    pthread_attr_destroy(&at);
    if (ok) pthread_join(th, NULL); else fn(arg);
}
static int lib_on(void) { return vkn_shlib_on && vkn_shlib_on() && vkn_shlib_b5_words && vkn_shlib_b5_module; }

// ---------------------------------------------------------------- first sight of a shader: its facts
typedef struct learn_job { mcls *c; int stale, bad; } learn_job;
static void learn_lib(void *a) {
    learn_job *j = (learn_job *)a; mcls *c = j->c; const vkn_shader *s = c->s;
    uint32_t *w = NULL; size_t n = 0; uint8_t *raw = NULL; size_t nraw = 0;
    c->cls = C_NONE; c->why_none = VKN_MAT_W_FACTS;
    if (!s->xs[VKN_SRC_FS] || vkn_shlib_b5_words(s->xs[VKN_SRC_FS], 0, &w, &n, &raw, &nraw)) { free(w); free(raw); return; }
    nr_mat_facts f; memset(&f, 0, sizeof f);
    if (!nr_mat_scan(w, n, &f) && f.model == 4 && !(f.flags & (NR_MAT_FRAGOUT | NR_MAT_SIDE | NR_MAT_BAD | NR_MAT_COMPLEX_IO)))
        c->cls = (uint8_t)(f.flags & NR_MAT_KILL ? C_KILL : C_PURE);
    if (c->cls && vkn_shlib_b5_record) {   // the gate record "dg.<key>" (xbg_nr_mat.inc note)
        char key[24]; snprintf(key, sizeof key, "dg.%.16s", s->key[VKN_SRC_FS]);
        uint8_t *rec = NULL; size_t nrec = 0;
        const int r = vkn_shlib_b5_record("native-nr33b.xgspack", key, &rec, &nrec);
        g.s.pack = r == -2 ? 0 : r == -3 ? -1 : 1;
        if (r == 1) {
            nr_mat_gate *gt = (nr_mat_gate *)calloc(1, sizeof *gt);
            const char *why = gt ? nr_mat_gate_parse(rec, nrec, gt) : "out of memory";
            if (why) { j->bad = 1; mred(R_GATE, "gate record of %.16s refused: %s (run setup again)", s->key[VKN_SRC_FS], why); free(gt); }
            else if (gt->spirv_fnv != nr_mat_fnv64(raw, nraw)) {
                j->stale = 1; mred(R_GATE, "gate record of %.16s is for other SPIR-V (%016llx, the bundle %016llx): ignored (run setup again)", s->key[VKN_SRC_FS],
                                   (unsigned long long)gt->spirv_fnv, (unsigned long long)nr_mat_fnv64(raw, nraw));
                free(gt);
            } else c->gate = gt;
        } else if (r == -1) { j->bad = 1; mred(R_GATE, "gate record of %.16s unreadable (CRC): ignored", s->key[VKN_SRC_FS]); }
        free(rec);
    }
    if (g.xlq && vkn_mat_fs_read && vkn_mat_vs_written) {
        c->fs_read = vkn_mat_fs_read(w, n, &c->fs_decl);
        uint32_t *vw = NULL; size_t vn = 0;
        if (s->xs[VKN_SRC_VS] && !vkn_shlib_b5_words(s->xs[VKN_SRC_VS], 0, &vw, &vn, NULL, NULL)) c->vs_written = vkn_mat_vs_written(vw, vn);
        free(vw);
        c->xlq_ok = c->fs_read != VSOUT_ALL && c->vs_written && c->vs_written != VSOUT_ALL;
    }
    free(w); free(raw);
}
static mcls *cls_of(const vkn_shader *s) {
    const uint32_t id = s->id;
    mcls *c = id < IDS ? g_tab[id] : NULL;
    if (c && c->s == s && c->handle == s->handle) return c;
    if (id >= IDS) return NULL;
    if (!c && !(c = g_tab[id] = (mcls *)calloc(1, sizeof *c))) return NULL;
    free(c->gate);   // (a retired record's id came back: everything of the old one goes; its objects stay alive, as all pieces' shaders)
    memset(c, 0, sizeof *c); c->s = s; c->handle = s->handle; c->aapplied = -1;
    g.s.learned++;
    if (g.test) {
        vkn_mat_test_facts f; memset(&f, 0, sizeof f); f.fs_flags = -1; f.alpha_applied = -1;
        if (!g.test(s, &f) && f.fs_flags >= 0) {
            c->cls = (uint8_t)(f.fs_flags & (NR_MAT_FRAGOUT | NR_MAT_SIDE | NR_MAT_BAD | NR_MAT_COMPLEX_IO) ? C_NONE : f.fs_flags & NR_MAT_KILL ? C_KILL : C_PURE);
            c->why_none = VKN_MAT_W_FACTS;
            if (f.gate && c->cls) { c->gate = (nr_mat_gate *)malloc(sizeof *c->gate); if (c->gate) *c->gate = *f.gate; }
            c->fs_read = f.fs_read; c->fs_decl = f.fs_decl; c->vs_written = f.vs_written;
            c->xlq_ok = c->fs_read != VSOUT_ALL && c->vs_written && c->vs_written != VSOUT_ALL;
        } else c->why_none = VKN_MAT_W_NOLIB;
    } else if (!lib_on()) c->why_none = VKN_MAT_W_NOLIB;
    else { learn_job j = {c, 0, 0}; run_big(learn_lib, &j); g.s.gates_stale += (uint64_t)j.stale; g.s.gates_bad += (uint64_t)j.bad; }
    if (c->gate) g.s.gates_loaded++;
    return c;
}

// The draw path's retype code (vkn_draw.c retype_code / retype_on, DM_GFX_VTX_RETYPE: default on everywhere but Adreno)
static uint64_t retype_code(mcls *c, uint16_t vfmt) {
    if (c->code_ok && c->vfmt == vfmt) return c->code;
    if (g.retype < 0) { const char *e = getenv("DM_GFX_VTX_RETYPE"); const vkn_dev *d = vkn_device(); g.retype = e && *e ? atoi(e) != 0 : d ? d->props.vendorID != 0x5143 : 0; }
    const vkn_vfmt *vf = vfmt ? vkn_vfmt_info(vfmt) : NULL; const vkn_shader *s = c->s;
    uint32_t code = 0;
    for (uint32_t i = 0; vf && g.retype && i < s->ninputs && i < 16; i++) {
        const uint32_t l = s->inputs[i]; if (l >= VKN_VFMT_LOCS) continue;
        const uint32_t k = vf->loc_class[l];
        if (k && k - 1 != s->input_classes[i]) code |= k << (2 * l);
    }
    c->vfmt = vfmt; c->code = code; c->code_ok = 1;
    return code;
}

// ---------------------------------------------------------------- the alpha test: is the "#at" variant what the draw path draws?
// The draw path takes vkn_shader_module(s, FS, 5 = GREATER, ref): in mode 1 the library's alpha_module, which returns the base module
// when it refuses (no colour 0 .w, DM_GFX_ALPHA_TEST=0): the old path's `applied` (alpha_module(...) != fs->module).
typedef struct alpha_job { const vkn_shader *s; float ref; int applied; } alpha_job;
static void alpha_lib(void *a) {
    alpha_job *j = (alpha_job *)a;
    const VkShaderModule m = vkn_shlib_alpha_module(j->s->xs[VKN_SRC_FS], 5u, j->ref);   // built once by the library (its cache)
    j->applied = !m || m != j->s->module[VKN_SRC_FS];
}
static int alpha_applied(mcls *c, float ref) {
    uint32_t bits; memcpy(&bits, &ref, 4);
    if (c->aapplied >= 0 && c->aref == bits) return c->aapplied;
    int ap = 1;   // unknown = applied (the safe answer: a kill may happen)
    if (g.test) { vkn_mat_test_facts f; memset(&f, 0, sizeof f); f.alpha_applied = -1; if (!g.test(c->s, &f) && f.alpha_applied >= 0) ap = f.alpha_applied; }
    else if (lib_on() && vkn_shlib_alpha_module && c->s->xs[VKN_SRC_FS]) { alpha_job j = {c->s, ref, 1}; run_big(alpha_lib, &j); ap = j.applied; }
    c->aref = bits; c->aapplied = (int8_t)ap;
    return ap;
}

// ---------------------------------------------------------------- gate words: the fragment constant blocks as they lie in the ring now
// bank_word = Maxwell bank << 16 | word; NVN uniform slot = bank - 3 = the engine block's binding index (vkn_draw_ubo's slot).
static int rd(void *ctx, uint32_t bw, uint32_t *out) {
    const uint8_t *ring = (const uint8_t *)ctx;
    const uint32_t bank = bw >> 16, word = bw & 0xffffu;
    if (!ring || bank < 3 || bank - 3 >= VKN_UBO_SLOTS) return 0;
    uint32_t off = 0, size = 0;
    if (vkn_draw_ubo_get(1, bank - 3, &off, &size) || (uint64_t)word * 4 + 4 > size) return 0;
    memcpy(out, ring + off + (size_t)word * 4, 4);
    return 1;
}

// ---------------------------------------------------------------- trimmed vertex modules of this piece
// The trimmed vertex module of s for (retype code, keep, declared) as vsout_module made it; 0 ok, else a VKN_MAT_V_* reason + 1.
typedef struct trim_job { const vkn_shader *s; uint64_t code, keep, decl; VkShaderModule m; int why, make; uint32_t stores, vars; char bad[96]; } trim_job;
static void trim_lib(void *a) {
    trim_job *j = (trim_job *)a; const vkn_shader *s = j->s;
    uint32_t *w = NULL, *o = NULL; size_t n = 0, on = 0; const char *bad = "";
    if (!vkn_mat_vsout || !s->xs[VKN_SRC_VS] || vkn_shlib_b5_words(s->xs[VKN_SRC_VS], j->code, &w, &n, NULL, NULL)) { j->why = VKN_MAT_V_WORDS + 1; free(w); return; }
    const int r = vkn_mat_vsout(w, n, j->keep, j->decl, &o, &on, &j->stores, &j->vars, &bad);
    if (r == 1) j->why = VKN_MAT_V_NOTHING + 1;
    else if (r) { j->why = r == -1 ? VKN_MAT_V_CHECK + 1 : VKN_MAT_V_MODULE + 1; snprintf(j->bad, sizeof j->bad, "%s", bad ? bad : "?"); }
    else {
        char key[96], vi[24] = "";
        if (j->code) snprintf(vi, sizeof vi, "#vi%08x", (unsigned)j->code);
        snprintf(key, sizeof key, "%.16s%s#vo%016llx_%016llx", s->key[VKN_SRC_VS], vi, (unsigned long long)j->keep, (unsigned long long)j->decl);
        if (!j->make) j->m = (VkShaderModule)(uintptr_t)1;   // count mode: made and checked, no module (the caller never draws it)
        else if (!(j->m = vkn_shlib_b5_module(o, on, key))) j->why = VKN_MAT_V_MODULE + 1;
    }
    free(w); free(o);
}
static VkShaderModule trimmed(const vkn_shader *s, uint64_t code, uint64_t keep, uint64_t decl, int make, int *why) {
    *why = 0;
    if (g.test) {
        vkn_mat_test_facts f; memset(&f, 0, sizeof f);
        const VkShaderModule m = !g.test(s, &f) && f.vs_module ? f.vs_module(s, code, keep) : VK_NULL_HANDLE;
        if (!m) *why = VKN_MAT_V_NOTHING + 1;
        return m;
    }
    if (!lib_on() || !vkn_mat_vsout) { *why = VKN_MAT_V_WORDS + 1; return VK_NULL_HANDLE; }
    trim_job j; memset(&j, 0, sizeof j); j.s = s; j.code = code; j.keep = keep; j.decl = decl; j.make = make;
    run_big(trim_lib, &j);
    if (j.why == VKN_MAT_V_CHECK + 1) mred(R_TRIM, "vs %.16s code %#llx keep %016llx: %s: the engine's module is drawn", s->key[VKN_SRC_VS], (unsigned long long)code, (unsigned long long)keep, j.bad);
    if (j.why == VKN_MAT_V_MODULE + 1) mred(R_MODULE, "vs %.16s code %#llx keep %016llx: no module (%s)", s->key[VKN_SRC_VS], (unsigned long long)code, (unsigned long long)keep, j.bad[0] ? j.bad : "vkCreateShaderModule");
    if (!j.why) { g.s.stores += j.stores; g.s.outputs += j.vars; }
    *why = j.why;
    return j.m;
}
// [stage 5 rest] The process-wide module table: one trimmed module (and its vin id) per (vertex key, retype code, keep, declared). The
// recording thread (cut_vs / xlq_var, once per shader and code) and the warm-up worker (vkn_mat_warm_module) both come here, so a
// replayed pipeline has exactly the module and id the draw will ask for. The lock is held across a build (a few ms, once per module).
typedef struct vmod { char key[17]; uint64_t code, keep, decl; VkShaderModule m; uint16_t vs_id; uint8_t why; } vmod;
static pthread_mutex_t g_vm_mu = PTHREAD_MUTEX_INITIALIZER;
static vmod *g_vm; static uint32_t g_nvm, g_cvm;
extern void vkn_warm_recipe_vs(uint16_t vs_id, const char *piece, const void *eng_shader, const void *arg, uint32_t n) __attribute__((weak));
// 0 = *m / *vs_id set; else a VKN_MAT_V_* reason + 1 (kept: asked again, the same answer). make 0 (XLQ count mode): checked, no module.
static int vin_module(const vkn_shader *s, uint64_t code, uint64_t keep, uint64_t decl, int make, VkShaderModule *m, uint16_t *vs_id) {
    *m = VK_NULL_HANDLE; *vs_id = 0;
    if (!make) { int w = 0; *m = trimmed(s, code, keep, decl, 0, &w); return w; }
    pthread_mutex_lock(&g_vm_mu);
    for (uint32_t i = 0; i < g_nvm; i++) {
        const vmod *e = &g_vm[i];
        if (e->code == code && e->keep == keep && e->decl == decl && !strncmp(e->key, s->key[VKN_SRC_VS], 16)) {
            const int w = e->why; *m = e->m; *vs_id = e->vs_id; pthread_mutex_unlock(&g_vm_mu); return w;
        }
    }
    int w = 0;
    if (g_nvm >= MODULES) w = VKN_MAT_V_TABLE + 1;
    else if (g_nvm == g_cvm) {
        const uint32_t c = g_cvm ? g_cvm * 2 : 256; vmod *x = (vmod *)realloc(g_vm, c * sizeof *x);
        if (x) { g_vm = x; g_cvm = c; } else w = VKN_MAT_V_TABLE + 1;
    }
    if (w) { pthread_mutex_unlock(&g_vm_mu); return w; }
    vmod e; memset(&e, 0, sizeof e); snprintf(e.key, sizeof e.key, "%.16s", s->key[VKN_SRC_VS]); e.code = code; e.keep = keep; e.decl = decl;
    e.m = trimmed(s, code, keep, decl, 1, &w);
    if (!e.m && !w) w = VKN_MAT_V_MODULE + 1;
    if (e.m && !w && !(e.vs_id = vkn_fx_vs_id())) w = VKN_MAT_V_TABLE + 1;   // (65535 vertex-module ids used up)
    if (w) e.m = VK_NULL_HANDLE;
    e.why = (uint8_t)w;
    g_vm[g_nvm++] = e;
    if (!w) g.own++;
    pthread_mutex_unlock(&g_vm_mu);
    if (!w && vkn_warm_recipe_vs && !g.test) {   // the warm-up lists pipelines with this module by recipe (vkn_warm.h)
        const uint64_t a[3] = {code, keep, decl};
        vkn_warm_recipe_vs(e.vs_id, "mat", s, a, sizeof a);
    }
    *m = e.m; *vs_id = e.vs_id;
    return w;
}
// The warm-up worker's door (vkn_warm.c, recipe "mat": the three words code / keep / declared of vin_module): the module and vin id a
// draw of engine shader s with that recipe gets. 0 = ok. Any thread.
int vkn_mat_warm_module(const void *eng, const void *arg, uint32_t n, VkShaderModule *m, uint16_t *vs_id);
int vkn_mat_warm_module(const void *eng, const void *arg, uint32_t n, VkShaderModule *m, uint16_t *vs_id) {
    const vkn_shader *s = (const vkn_shader *)eng;
    if (!s || !arg || n != 3 * sizeof(uint64_t) || !m || !vs_id || !g.inst) return -1;
    uint64_t a[3]; memcpy(a, arg, sizeof a);
    return vin_module(s, a[0], a[1], a[2], 1, m, vs_id) ? -1 : 0;
}
// variant B: the cut-down vertex module of c for retype code `code` (made once; a failure is kept and counted at every draw)
static const vs_var *cut_vs(mcls *c, uint64_t code) {
    for (uint32_t i = 0; i < c->nvs; i++) if (c->vs[i].code == code) return &c->vs[i];
    if (c->nvs >= CODES) { g.s.vs_refused[VKN_MAT_V_CODES]++; return NULL; }
    vs_var *v = &c->vs[c->nvs++]; memset(v, 0, sizeof *v); v->code = code; v->done = 1;
    const vkn_shader *s = c->s;
    const int w = vin_module(s, code, 0, 0, 1, &v->m, &v->vs_id);
    if (w) { v->done = (uint8_t)(1 + w); v->m = VK_NULL_HANDLE; return v; }
    g.s.twins++;
    if (g.s.twins <= 4) vkn_logf("mat: vs %.16s code %#llx -> cut-down vertex module (no Location output; depth-only pipelines without a fragment stage, vin id %u)", s->key[VKN_SRC_VS], (unsigned long long)code, v->vs_id);
    return v;
}
static uint64_t xlq_keep(uint64_t read, int mode) {   // xbg_game.cpp xlq_keep: mode 3 drops the lowest read location too
    if (mode != 3 || !read) return read;
    uint32_t l = 0; while (!((read >> (4 * l)) & 0xF)) l++;
    return read & ~((uint64_t)0xF << (4 * l));
}
// XLQ: the trimmed vertex module of c for retype code `code`; the fragment module is the draw path's own (base / "#at" for the draw's
// reference: the engine object keeps its variants; the old effect object per (code, reference) is gone)
static const x_var *xlq_var(mcls *c, uint64_t code) {
    for (uint32_t i = 0; i < c->nxv; i++) if (c->xv[i].code == code) return &c->xv[i];
    if (c->nxv >= XVARS) { g.s.xlq_refused[VKN_MAT_X_VARIANTS]++; return NULL; }
    x_var *v = &c->xv[c->nxv++]; memset(v, 0, sizeof *v); v->code = code; v->done = 1;
    const vkn_shader *s = c->s;
    const uint64_t keep = xlq_keep(c->fs_read, g.xlq);
    const int w = vin_module(s, code, keep, c->fs_decl, g.xlq != 2, &v->m, &v->vs_id);
    if (w) {
        v->m = VK_NULL_HANDLE;
        v->done = (uint8_t)(2 + (w == VKN_MAT_V_NOTHING + 1 ? VKN_MAT_X_NOTHING : w == VKN_MAT_V_CHECK + 1 ? VKN_MAT_X_CHECK : w == VKN_MAT_V_WORDS + 1 ? VKN_MAT_X_WORDS :
                                 w == VKN_MAT_V_TABLE + 1 ? VKN_MAT_X_TABLE : VKN_MAT_X_MODULE));
        return v;
    }
    g.s.xlq_shaders++;
    if (g.xlq == 2) { v->m = VK_NULL_HANDLE; return v; }   // count: made and checked, not drawn
    if (g.s.xlq_shaders <= 8) vkn_logf("mat: xlq vs %.16s / fs %.16s code %#llx: vertex outputs %016llx, the fragment shader reads %016llx (vin id %u)", s->key[VKN_SRC_VS], s->key[VKN_SRC_FS],
                                       (unsigned long long)code, (unsigned long long)c->vs_written, (unsigned long long)keep, v->vs_id);
    return v;
}

// ---------------------------------------------------------------- the decision
static int engine_vf(const vkn_shader *s) {
    return s && !vkn_fx_shader_is(s) && (s->stages & 7u) == ((1u << VKN_SRC_VS) | (1u << VKN_SRC_FS)) && !s->missing && !s->unusable;
}
// MAT in a pass without colour targets: 1 = decided (out changed, or would be in verify mode)
static int mat_depth(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out) {
    const vkn_shader *s = ctx->shader;
    if (!engine_vf(s)) { g.s.refused[VKN_MAT_W_NOTENG]++; return 0; }
    mcls *c = cls_of(s);
    if (!c) { g.s.refused[VKN_MAT_W_NOTENG]++; return 0; }
    g.s.seen++;
    if (c->cls == C_NONE) { g.s.refused[c->why_none]++; return 0; }
    const int alpha = in->alpha_ref >= 0.0f;
    const int applied = alpha ? alpha_applied(c, in->alpha_ref) : 0;
    uint64_t *ok = NULL; int why = -1;
    if (!applied && c->cls == C_PURE) ok = &g.s.pure;
    else if (c->gate) {
        const int p = nr_mat_gate_eval(c->gate, rd, vkn_ring_cpu());
        if (p < 0) why = VKN_MAT_W_GATE;
        else if (!applied) ok = &g.s.gate_off;
        else if (c->gate->path[p].w_known && nr_mat_alpha_pass(5u, c->gate->path[p].w_bits, in->alpha_ref)) ok = &g.s.gate_alpha;
        else why = VKN_MAT_W_ALPHA;
    } else why = VKN_MAT_W_KILL;
    if (!ok) { g.s.refused[why]++; return 0; }
    (*ok)++;
    if (alpha && !applied) g.s.alpha_na++;
    if (g.mode == 3) { g.s.would++; return 1; }
    out->no_fs = 1; g.s.no_fs++;
    if (g.mode == 2) {
        // [stage 5 rest] the cut-down module rides on the engine object (vkn_fx_out.vin); a vertex module of an earlier function
        // (PRESKIN, order 150) stays: it is the draw's vertex stage, variant A only
        if (out->vin.vs) g.s.vs_refused[VKN_MAT_V_VIN]++;
        else {
            const vs_var *v = cut_vs(c, retype_code(c, ctx->vfmt));
            if (v && v->m) { out->vin.vs = v->m; out->vin.vs_id = v->vs_id; g.s.vs_swaps++; }
            else if (v && v->done >= 2) g.s.vs_refused[v->done - 2]++;
        }
    }
    return 1;
}
static void xlq_draw(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out) {
    const vkn_shader *s = ctx->shader;
    if (!engine_vf(s)) return;
    g.s.xlq_seen++;
    if (!ctx->depth) { g.s.xlq_refused[VKN_MAT_X_NODEPTH]++; return; }
    mcls *c = cls_of(s);
    if (!c) return;
    if (!c->xlq_ok) { g.s.xlq_refused[c->why_none == VKN_MAT_W_NOLIB ? VKN_MAT_X_WORDS : VKN_MAT_X_FS]++; return; }
    if (!(c->vs_written & ~xlq_keep(c->fs_read, g.xlq))) { g.s.xlq_refused[VKN_MAT_X_NOTHING]++; return; }
    if (vkn_shlib_named_entry && vkn_shlib_named_entry(s->key[VKN_SRC_FS], NULL) >= 0) { g.s.xlq_refused[VKN_MAT_X_NAMED]++; return; }
    if (out->vin.vs) { g.s.xlq_refused[VKN_MAT_X_VIN]++; return; }   // [stage 5 rest] an earlier function's vertex module (PRESKIN) is the stage
    const x_var *v = xlq_var(c, retype_code(c, ctx->vfmt));
    if (!v) return;
    if (v->done >= 2) { g.s.xlq_refused[v->done - 2]++; return; }
    if (g.xlq == 2) { g.s.xlq_would++; return; }
    if (!v->m) return;
    out->vin.vs = v->m; out->vin.vs_id = v->vs_id; g.s.xlq_swaps++;   // [stage 5 rest] (the engine's fragment module and variants stay)
}
static int decide(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out) {
    if (out->shader || out->discard || out->no_fs) return 0;   // an earlier technique changed the draw
    int nocolor = !ctx->color[0];
    for (uint32_t i = 1; nocolor && i < VKN_PASS_COLORS; i++) if (ctx->color[i]) nocolor = 0;
    if (nocolor && ctx->depth && g.mode && mat_depth(in, ctx, out)) return 1;
    if (g.xlq) { const VkShaderModule was = out->vin.vs; xlq_draw(in, ctx, out); return out->vin.vs != was; }
    return 0;
}
static void census_maybe(void);
static void mat_fx(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)user;
    if (!ctx->draw_no) census_maybe();     // (once a frame)
    if (ctx->color[0] && !g.xlq) return;   // THE cheap exit: a colour pass while XLQ is off
    if (!g.mode && !g.xlq) return;
    decide(in, ctx, out);
}
int vkn_mat_apply(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out) {
    if (!g.inst || !in || !ctx || !ctx->shader || !out) return 0;
    return decide(in, ctx, out);
}

// ---------------------------------------------------------------- install, census
int vkn_mat_mode(void) { return g.inst ? g.mode : 0; }
int vkn_mat_xlq(void) { return g.inst ? g.xlq : 0; }
void vkn_mat_stats_get(vkn_mat_stats *s) { if (s) *s = g.s; }
static int reg_fn(void) {
    if (!g.reg) g.reg = !vkn_fx_register("mat", mat_fx, NULL, 200);
    return g.reg;
}
static int read_switch(const char *name, int def, int max) {
    const char *e = getenv(name); int v = e && *e ? atoi(e) : def;
    if (v < 0 || v > max) { mred(R_CFG, "%s=%s is not 0..%d: off", name, e, max); v = 0; }
    return v;
}
void vkn_mat_install(uint32_t vendor, const char *device_name) {
    static int done; if (done) return; done = 1;
    const int f = family(vendor, device_name), tile = f == 2 || f == 3 || f == 5;
    g.mode = read_switch("DM_VK_NATIVE_MAT", tile ? 2 : 1, 3);
    g.xlq = read_switch("DM_VK_NATIVE_XLQ", tile ? 1 : 0, 3);
    g.s.pack = -2;
    vkn_logf("DM_VK_NATIVE_MAT=%d (stage 5.2 B5, replaces DM_NR_MAT + DM_NR_MAT_VS: %s), DM_VK_NATIVE_XLQ=%d (replaces DM_NR_XLQ: %s); family %s (default MAT %d, XLQ %d); "
             "gate records <shaders>/native-nr33b.xgspack; shader library %s",
             g.mode, g.mode == 0 ? "off" : g.mode == 1 ? "no fragment stage in passes without colour targets when provably without effect" :
             g.mode == 2 ? "no fragment stage + the cut-down vertex shader (no Location output)" : "verify - decisions counted, the engine's shaders drawn",
             g.xlq, g.xlq == 0 ? "off" : g.xlq == 1 ? "vertex outputs the fragment shader never reads are not written" : g.xlq == 2 ? "count - trims made and checked, the engine's shaders drawn" : "POISON - one read location dropped too",
             family_name(f), tile ? 2 : 1, tile ? 1 : 0, lib_on() ? "on" : "OFF (nothing can be decided: every draw counted as 'no shader library')");
    if (!g.mode && !g.xlq) return;
    if (!reg_fn()) { mred(R_CFG, "the draw-effect chain refused the function: off"); g.mode = g.xlq = 0; return; }
    g.inst = 1;
}
void vkn_b5_install(uint32_t vendor, const char *device_name);   // vkn_shlib.cpp's one call (end of vkn_shlib_init: mode 1 with the library on)
void vkn_b5_install(uint32_t vendor, const char *device_name) { vkn_mat_install(vendor, device_name); if (vkn_prepass_install) vkn_prepass_install(vendor, device_name); }
void vkn_mat_test_setup(int mode, int xlq, vkn_mat_test_fn fn) {
    for (uint32_t i = 0; i < IDS; i++) if (g_tab[i]) { free(g_tab[i]->gate); free(g_tab[i]); g_tab[i] = NULL; }
    pthread_mutex_lock(&g_vm_mu); g_nvm = 0; pthread_mutex_unlock(&g_vm_mu);   // [stage 5 rest] (a test's modules are its own)
    const int reg = g.reg; const vkn_mat_stats keep = g.s, last = g.last;   // (the counters run on: the test reads differences)
    memset(&g, 0, sizeof g); g.reg = reg; g.retype = -1; g.s = keep; g.last = last;
    g.mode = mode; g.xlq = xlq; g.test = fn;
    g.inst = mode || xlq ? reg_fn() : 0;
}
static void census_maybe(void) {
    const uint64_t now = vkn_now_ns();
    if (now < g.next_ns) return;
    if (g.next_ns) vkn_mat_census();
    g.next_ns = now + 10000000000ull;
}
typedef struct txt { char b[2400]; size_t k; } txt;
static void put(txt *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(txt *t, const char *fmt, ...) {
    if (t->k >= sizeof t->b - 1) return;
    va_list a; va_start(a, fmt); const int w = vsnprintf(t->b + t->k, sizeof t->b - t->k, fmt, a); va_end(a);
    if (w > 0) t->k = t->k + (size_t)w < sizeof t->b - 1 ? t->k + (size_t)w : sizeof t->b - 1;
}
void vkn_mat_census(void) {
    if (!g.inst) return;
    typedef unsigned long long ull;
    const vkn_mat_stats *s = &g.s, *o = &g.last;
#define D(f) ((ull)(s->f - o->f))
    if (!D(seen) && !D(xlq_seen) && !D(reds) && !D(learned)) return;
    txt a; a.k = 0; a.b[0] = 0;
    put(&a, "mat: DM_VK_NATIVE_MAT=%d XLQ=%d (last 10 s): depth-only draws %llu: %s %llu (no kill + alpha off %llu, gate + alpha off %llu, gate + alpha passes .w %llu, "
        "alpha test not applied %llu), cut-down vertex shaders drawn %llu; kept:", g.mode, g.xlq, D(seen), g.mode == 3 ? "would drop the fragment stage" : "fragment stage dropped",
        g.mode == 3 ? D(would) : D(no_fs), D(pure), D(gate_off), D(gate_alpha), D(alpha_na), D(vs_swaps));
    int any = 0;
    for (int i = 0; i < VKN_MAT_W_N; i++) if (s->refused[i] != o->refused[i]) { put(&a, "%s %s %llu", any ? "," : "", vkn_mat_reason_name(0, i), (ull)(s->refused[i] - o->refused[i])); any = 1; }
    put(&a, "%s; cut-down vertex shader not drawn:", any ? "" : " none"); any = 0;
    for (int i = 0; i < VKN_MAT_V_N; i++) if (s->vs_refused[i] != o->vs_refused[i]) { put(&a, "%s %s %llu", any ? "," : "", vkn_mat_reason_name(1, i), (ull)(s->vs_refused[i] - o->vs_refused[i])); any = 1; }
    put(&a, "%s", any ? "" : " none");
    if (g.xlq) {
        put(&a, "; xlq: draws %llu, %s %llu, refused:", D(xlq_seen), g.xlq == 2 ? "would swap" : "swapped", g.xlq == 2 ? D(xlq_would) : D(xlq_swaps)); any = 0;
        for (int i = 0; i < VKN_MAT_X_N; i++) if (s->xlq_refused[i] != o->xlq_refused[i]) { put(&a, "%s %s %llu", any ? "," : "", vkn_mat_reason_name(2, i), (ull)(s->xlq_refused[i] - o->xlq_refused[i])); any = 1; }
        put(&a, "%s", any ? "" : " none");
    }
    put(&a, "; totals: shaders looked at %llu, gate records loaded %llu (stale %llu, bad %llu; pack %s), cut-down vertex shaders %llu, xlq objects %llu, stores dropped %llu, "
        "outputs dropped %llu, own vertex modules %u (vin); red lines %llu", (ull)s->learned, (ull)s->gates_loaded, (ull)s->gates_stale, (ull)s->gates_bad,
        s->pack == 1 ? "open" : s->pack == 0 ? "none" : s->pack == -1 ? "DAMAGED" : "not asked", (ull)s->twins, (ull)s->xlq_shaders, (ull)s->stores, (ull)s->outputs, g.own, D(reds));
#undef D
    vkn_logf("%s", a.b);
    g.last = g.s;
}
#endif
