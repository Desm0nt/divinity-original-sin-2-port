// [DOS2] VK-NATIVE stage 5.3 V1: PRESKIN (vkn_preskin.h has the rule, the exactness argument, the mechanism, the switches). One vkn_fx
// function (order 150). A draw whose vertex shader is not a skinning candidate costs one table load per shader id. Ported from
// xbg_nr_preskin.inc (namespace nrps: init / plan_for / cons_for / slice_pipe / slot_now / decide / apply_old / prologue / tick) onto the
// backend: the arena of a slot -> per-frame arena blocks retired with the frame; the prologue command buffer of a slot -> this module's
// own command buffer submitted in the frame's close listener; apply_old's pipeline rewrite -> vkn_fx_out.vin (vkn_fx.h). No game
// addresses.
#ifdef DM_GAME_DOS2
#include "vkn_preskin.h"
#include "vkn.h"
#include "vkn_buf.h"
#include "vkn_draw.h"
#include "vkn_frame.h"
#include "vkn_fx.h"
#include "vkn_policy.h"
#include "vkn_shader.h"
#include "vkn_state.h"
#include "../nr/nr_preskin.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
// the shader library's doors (vkn_shlib.cpp blocks "stage 5.2 B5" and "stage 5.3 V1"; vkn_preskin_lib.cpp) - weak: host tests run without
extern int vkn_shlib_on(void) __attribute__((weak));
extern int vkn_shlib_policy(void) __attribute__((weak));
extern int vkn_shlib_b5_words(void *shader, uint64_t retype, uint32_t **words, size_t *nwords, uint8_t **raw, size_t *nraw) __attribute__((weak));
extern VkShaderModule vkn_shlib_b5_module(const uint32_t *words, size_t nwords, const char *key) __attribute__((weak));
extern int vkn_shlib_preskin_candidate(const void *shader) __attribute__((weak));
extern int vkn_psk_plan_make(const uint32_t *w, size_t n, const uint32_t *bind, const uint32_t *key, uint32_t nbind, vkn_psk_plan *out) __attribute__((weak));
extern void vkn_psk_plan_free(vkn_psk_plan *p) __attribute__((weak));
extern int vkn_psk_slice(const vkn_psk_plan *p, uint32_t **out, size_t *n, const char **why) __attribute__((weak));
extern int vkn_psk_consumer(const vkn_psk_plan *p, const uint32_t *pos, uint32_t **out, size_t *n, vkn_psk_layout *lay, const char **why) __attribute__((weak));

enum { IDS = 16384, CODES = 4, CONS = 8, VFS = 8, PIPES = 8, MAXREC = 512, BUCKETS = 1024, RING = 4, POOLS = 8, POOL_SETS = 128,
       CHUNKS = 16, CHUNK = 1 << 20, VFMT_MAX = 192, REDS = 16 };
enum { R_CFG, R_PLAN, R_MODULE, R_PIPE, R_VK, R_VFMT, R_N };
static const char *const k_why[VKN_PSK_W_N] = {
    "not a skinning candidate", "no shader library", "no plan", "record too wide", "retype codes past 4", "off-frame recording", "attribute",
    "instanced input", "unbound", "stride / format", "mesh too big", "uniform block", "arena full", "descriptor pool", "slice pipeline",
    "consumer module", "no free stream", "vertex format", "per-draw module ids", "points / swapped / discarded",
    "a later object without the vertex bindings"};
const char *vkn_preskin_reason_name(int w) { return w >= 0 && w < VKN_PSK_W_N ? k_why[w] : "?"; }

// ---------------------------------------------------------------- per vertex shader: plans per retype code, consumers, formats, pipelines
typedef struct cons_e { uint64_t ph; uint8_t done; VkShaderModule mod; uint16_t vs_id; vkn_psk_layout lay; } cons_e;   // done 1 ok, 2 refused
typedef struct vf_e { uint16_t eng, cid, vfmt; uint8_t stream, ok; } vf_e;     // synthetic format of (engine format, consumer index)
typedef struct pipe_e { uint16_t vfmt; VkPipeline p; } pipe_e;                 // slice pipeline of an engine format
typedef struct plan_e {
    uint64_t code; uint8_t done;           // 1 = planned, 2 = refused (why)
    char why[96];
    vkn_psk_plan p; nrps_val want[NRPS_MAXV]; uint32_t nwant;
    uint8_t slot[VKN_PSK_UBOS]; uint32_t bytes[VKN_PSK_UBOS]; uint8_t bone; uint8_t cls;   // NVN slots of the slice's blocks; bone = index of the bone block
    VkShaderModule slice; VkDescriptorSetLayout dsl; VkPipelineLayout pl;
    cons_e cons[CONS]; uint32_t ncons;
    vf_e vfs[VFS]; uint32_t nvfs;
    pipe_e pipes[PIPES]; uint32_t npipes;
} plan_e;
typedef struct psh { const vkn_shader *s; uint64_t handle; int8_t cand; uint8_t nplan; plan_e *plan[CODES]; } psh;
static psh *g_tab[IDS];

// ---------------------------------------------------------------- the frame: records, arena blocks, the prologue command buffer
typedef struct ring_e { VkCommandBuffer cb; VkFence fence; uint8_t submitted; VkDescriptorPool pool[POOLS]; uint32_t npool; } ring_e;
static struct {
    int mode, inst, reg, retype, family, frame_open, pro_open;   // frame_open: a presented recording is open; pro_open: the prologue began
    uint32_t maxm, maxv; uint64_t budget;
    vkn_preskin_stats s, last;
    uint32_t reds[R_N], nvfmt;
    uint64_t next_ns, frame_no;
    VkRenderPass rp; VkFramebuffer fb; VkCommandPool cpool; ring_e ring[RING]; uint32_t cur;
    vkn_block chunk[CHUNKS]; uint64_t chunk_used[CHUNKS]; uint32_t nchunk; uint64_t frame_bytes;
    nrps_rec rec[MAXREC]; VkBuffer rbuf[MAXREC]; uint32_t nrec;
    uint16_t bucket[BUCKETS]; uint64_t bucket_key[BUCKETS];          // open addressing: bucket hash -> first record + 1 (chained by next)
    uint16_t next[MAXREC];
    vkn_block nullb; int null_ok;
    uint32_t ssbo_align, ubo_align, ubo_max;
    const vkn_preskin_test_lib *test;
    VkPipeline pipe_cur;
} g = {.retype = -1};

static void pred(int kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void pred(int kind, const char *fmt, ...) {
    g.s.reds++;
    if (kind < 0 || kind >= R_N || g.reds[kind]++ >= REDS) return;
    char b[640]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    vkn_logf(RED "preskin: %s", b);
}

// ---------------------------------------------------------------- the library's steps on a thread with a big stack (as vkn_mat.c)
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
static int lib_on(void) {
    if (g.test) return g.test->candidate && g.test->plan && g.test->slice && g.test->consumer;
    return vkn_shlib_on && vkn_shlib_on() && vkn_shlib_b5_words && vkn_shlib_b5_module && vkn_shlib_preskin_candidate && vkn_psk_plan_make && vkn_psk_slice &&
           vkn_psk_consumer;
}
static VkShaderModule make_module(const uint32_t *w, size_t n, const char *key) {
    if (!g.test && vkn_shlib_b5_module) return vkn_shlib_b5_module(w, n, key);
    const vkn_dev *d = vkn_device(); if (!d) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; ci.codeSize = n * 4; ci.pCode = w;
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(d->dev, &ci, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

// ---------------------------------------------------------------- the switch
int vkn_preskin_switch(int family) {
    static int v = -1;
    if (v < 0) v = vkn_policy_env("DM_VK_NATIVE_PRESKIN", vkn_preskin_default(family), 0, 3, NULL);
    return v;
}
static uint32_t env_u(const char *n, uint32_t d, uint32_t lo, uint32_t hi) {
    const char *e = getenv(n); if (!e || !*e) return d;
    long v = atol(e); return v < (long)lo ? lo : v > (long)hi ? hi : (uint32_t)v;
}

// ---------------------------------------------------------------- the draw path's retype code (vkn_draw.c retype_code / retype_on)
static uint64_t retype_code(const vkn_shader *s, uint16_t vfmt) {
    if (g.retype < 0) { const char *e = getenv("DM_GFX_VTX_RETYPE"); const vkn_dev *d = vkn_device(); g.retype = e && *e ? atoi(e) != 0 : d ? d->props.vendorID != 0x5143 : 0; }
    const vkn_vfmt *vf = vfmt ? vkn_vfmt_info(vfmt) : NULL;
    uint32_t code = 0;
    for (uint32_t i = 0; vf && g.retype && i < s->ninputs && i < 16; i++) {
        const uint32_t l = s->inputs[i]; if (l >= VKN_VFMT_LOCS) continue;
        const uint32_t k = vf->loc_class[l];
        if (k && k - 1 != s->input_classes[i]) code |= k << (2 * l);
    }
    return code;
}

// ---------------------------------------------------------------- plans (first sight of a (shader, retype code))
typedef struct plan_job { const vkn_shader *s; plan_e *pe; int make; } plan_job;
static void plan_lib(void *a) {
    plan_job *j = (plan_job *)a; const vkn_shader *s = j->s; plan_e *pe = j->pe;
    uint32_t *w = NULL; size_t n = 0;
    if (g.test) {
        if (g.test->plan(s, pe->code, &pe->p) || !pe->p.ok) { if (!pe->p.why[0]) snprintf(pe->why, sizeof pe->why, "the split refused"); else snprintf(pe->why, sizeof pe->why, "%s", pe->p.why); pe->p.ok = 0; return; }
        goto sliced;
    }
    if (vkn_shlib_b5_words(s->xs[VKN_SRC_VS], pe->code, &w, &n, NULL, NULL) || !w) { snprintf(pe->why, sizeof pe->why, "the library has no words for code %#llx", (unsigned long long)pe->code); free(w); return; }
    // the module's uniform bindings and their data keys (kind << 16 | engine stage << 8 | slot: the old path's source << 16 | NVN slot)
    uint32_t bind[64], key[64], nb = 0;
    for (uint32_t i = 0; i < s->nbind && nb < 64; i++) {
        const vkn_bind *b = &s->bind[i];
        if (b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) continue;
        bind[nb] = b->binding; key[nb] = (uint32_t)b->kind << 16 | (uint32_t)b->stage << 8 | b->slot; nb++;
    }
    const int pr = vkn_psk_plan_make(w, n, bind, key, nb, &pe->p);
    free(w);
    if (pr) { snprintf(pe->why, sizeof pe->why, "%s", pe->p.why[0] ? pe->p.why : "the split refused"); pe->p.ok = 0; return; }
sliced:;
    uint32_t *sw = NULL; size_t sn = 0; const char *why = "";
    if ((g.test ? g.test->slice : vkn_psk_slice)(&pe->p, &sw, &sn, &why)) { snprintf(pe->why, sizeof pe->why, "slice: %s", why); pe->p.ok = 0; free(sw); return; }
    if (j->make == 2) {
        char k[48]; snprintf(k, sizeof k, "%.16s#ps%llx.slice", s->key[VKN_SRC_VS], (unsigned long long)pe->code);
        if (!(pe->slice = make_module(sw, sn, k))) { snprintf(pe->why, sizeof pe->why, "slice module"); pe->p.ok = 0; }
    }
    free(sw);
}
// the plan's layout objects: set 0 = the slice's uniform blocks + the record's storage buffer (vertex stage)
static int plan_layout(plan_e *pe) {
    const vkn_dev *d = vkn_device(); if (!d) return -1;
    VkDescriptorSetLayoutBinding lb[VKN_PSK_UBOS + 1]; memset(lb, 0, sizeof lb); uint32_t n = 0;
    for (uint32_t i = 0; i < pe->p.nubo; i++) { lb[n].binding = pe->p.ubo_binding[i]; lb[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; lb[n].descriptorCount = 1; lb[n].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; n++; }
    lb[n].binding = pe->p.ssbo_binding; lb[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; lb[n].descriptorCount = 1; lb[n].stageFlags = VK_SHADER_STAGE_VERTEX_BIT; n++;
    VkDescriptorSetLayoutCreateInfo li; memset(&li, 0, sizeof li); li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; li.bindingCount = n; li.pBindings = lb;
    if (vkCreateDescriptorSetLayout(d->dev, &li, NULL, &pe->dsl) != VK_SUCCESS) { pe->dsl = VK_NULL_HANDLE; return -1; }
    VkPipelineLayoutCreateInfo pi; memset(&pi, 0, sizeof pi); pi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pi.setLayoutCount = 1; pi.pSetLayouts = &pe->dsl;
    if (vkCreatePipelineLayout(d->dev, &pi, NULL, &pe->pl) != VK_SUCCESS) { pe->pl = VK_NULL_HANDLE; return -1; }
    return 0;
}
static plan_e *plan_of(psh *h, uint64_t code, int *why) {
    for (uint32_t i = 0; i < h->nplan; i++) if (h->plan[i]->code == code) {
        plan_e *pe = h->plan[i];
        if (pe->done == 1) return pe;
        *why = VKN_PSK_W_PLAN; return NULL;
    }
    if (h->nplan >= CODES) { *why = VKN_PSK_W_CODES; return NULL; }
    plan_e *pe = (plan_e *)calloc(1, sizeof *pe);
    if (!pe) { *why = VKN_PSK_W_PLAN; return NULL; }
    h->plan[h->nplan++] = pe; pe->code = code; pe->done = 2;
    const vkn_shader *s = h->s;
    plan_job j = {s, pe, g.mode == 2 ? 1 : 2};
    if (g.test) plan_lib(&j); else run_big(plan_lib, &j);
    const char *bad = NULL;
    if (pe->p.ok) {
        // every block the slice reads must be one vertex-stage engine uniform slot (the old rule: one NVN uniform slot), the bone block too
        for (uint32_t i = 0; i < pe->p.nubo && !bad; i++) {
            const vkn_bind *b = NULL;
            for (uint32_t k = 0; k < s->nbind; k++) if (s->bind[k].binding == pe->p.ubo_binding[i]) b = &s->bind[k];
            if (!b || b->kind != VKN_BK_UNIFORM || b->stage != VKN_STAGE_VS || b->slot >= NRPS_SLOTS || b->count != 1) bad = "a slice uniform block is not one vertex uniform slot";
            else { pe->slot[i] = b->slot; pe->bytes[i] = b->bytes; if (b->binding == pe->p.bone_binding) pe->bone = (uint8_t)i; }
        }
        if (!bad && pe->p.nvals > NRPS_MAXV) bad = "too many values";
        if (!bad && g.mode != 2 && plan_layout(pe)) bad = "slice set / pipeline layout";
    } else bad = pe->why[0] ? pe->why : "refused";
    if (bad) {
        if (bad != pe->why) snprintf(pe->why, sizeof pe->why, "%s", bad);
        g.s.plans_refused++;
        if (g.s.plans_refused <= 8) vkn_logf("preskin: vs %.16s code %#llx not planned: %s", s->key[VKN_SRC_VS], (unsigned long long)code, pe->why);
        *why = VKN_PSK_W_PLAN; return NULL;
    }
    pe->nwant = pe->p.nvals;
    for (uint32_t i = 0; i < pe->nwant; i++) {
        uint32_t sm = 0;
        for (uint32_t k = 0; k < pe->p.nubo; k++) if (pe->p.val[i].ubo_mask >> k & 1) sm |= 1u << pe->slot[k];
        pe->want[i].hash = pe->p.val[i].hash; pe->want[i].first = pe->p.val[i].first; pe->want[i].comps = pe->p.val[i].comps;
        pe->want[i].loc_mask = pe->p.val[i].loc_mask; pe->want[i].slot_mask = sm;
    }
    pe->cls = pe->p.m <= 4 ? 0 : 1;
    pe->done = 1; g.s.plans++;
    if (g.s.plans <= 8 || g.mode == 2)
        vkn_logf("preskin: plan vs %.16s code %#llx: record %u words (%u values, %u on another uniform block), bone loads %u, body ops %u -> consumer %u + slice %u, "
                 "Locations orig %04x slice %04x consumer %04x", s->key[VKN_SRC_VS], (unsigned long long)code, pe->p.m, pe->nwant, pe->p.kdep, pe->p.bone_loads,
                 pe->p.ops_orig, pe->p.ops_cons, pe->p.ops_slice, pe->p.orig_locs, pe->p.slice_locs, pe->p.cons_locs);
    return pe;
}
// the consumer for record positions pos (rec_m words a vertex in the record it reads)
typedef struct cons_job { plan_e *pe; const uint32_t *pos; cons_e *c; const char *key; int make; char why[96]; } cons_job;
static void cons_lib(void *a) {
    cons_job *j = (cons_job *)a; uint32_t *w = NULL; size_t n = 0; const char *why = "";
    if ((g.test ? g.test->consumer : vkn_psk_consumer)(&j->pe->p, j->pos, &w, &n, &j->c->lay, &why)) { snprintf(j->why, sizeof j->why, "%s", why); free(w); return; }
    if (j->make && !(j->c->mod = make_module(w, n, j->key))) snprintf(j->why, sizeof j->why, "vkCreateShaderModule");
    free(w);
}
static cons_e *cons_of(const vkn_shader *s, plan_e *pe, const uint32_t *pos, uint64_t ph, uint32_t *ci) {
    for (uint32_t i = 0; i < pe->ncons; i++) if (pe->cons[i].ph == ph) { *ci = i; return pe->cons[i].done == 1 ? &pe->cons[i] : NULL; }
    if (pe->ncons >= CONS) return NULL;
    *ci = pe->ncons; cons_e *c = &pe->cons[pe->ncons++]; memset(c, 0, sizeof *c); c->ph = ph; c->done = 2;
    char key[64]; snprintf(key, sizeof key, "%.16s#vi%llx#ps%016llx", s->key[VKN_SRC_VS], (unsigned long long)pe->code, (unsigned long long)ph);
    cons_job j; memset(&j, 0, sizeof j); j.pe = pe; j.pos = pos; j.c = c; j.key = key; j.make = g.mode != 2;
    if (g.test) cons_lib(&j); else run_big(cons_lib, &j);
    if (j.why[0]) { pred(R_MODULE, "consumer of vs %.16s code %#llx: %s", s->key[VKN_SRC_VS], (unsigned long long)pe->code, j.why); return NULL; }
    if (g.mode != 2 && !(c->vs_id = vkn_fx_vs_id())) return NULL;
    c->done = 1; g.s.consumers++;
    return c;
}

// ---------------------------------------------------------------- vertex formats: the engine's + the record's groups on a free stream
// Engine element = {stream, usage, usage index, per-instance, type, count} (vkn_state.h); type 1 = R32*_UINT (4 bytes a component). The
// record's words go in order on the stream as elements (offset = the stream's running stride), a group as its location; words no group
// reads are filler elements at a spare location (one the consumer does not read: an attribute nothing reads, or none when count > 4).
static void loc_usage(uint32_t loc, uint8_t *usage, uint8_t *index) {   // the inverse of vkn_state.c usage_location
    *index = 0;
    if (loc == 0) *usage = 0; else if (loc == 1) *usage = 3; else if (loc == 2) *usage = 5; else if (loc == 3) *usage = 4;
    else if (loc <= 8) { *usage = 2; *index = (uint8_t)(loc - 4); } else if (loc <= 13) { *usage = 8; *index = (uint8_t)(loc - 9); }
    else if (loc == 14) *usage = 6; else *usage = 7;
}
static int vfmt_of(plan_e *pe, uint32_t ci, uint16_t eng, uint32_t rec_m, uint16_t *vfmt, uint8_t *stream) {
    for (uint32_t i = 0; i < pe->nvfs; i++) if (pe->vfs[i].eng == eng && pe->vfs[i].cid == ci) { *vfmt = pe->vfs[i].vfmt; *stream = pe->vfs[i].stream; return pe->vfs[i].ok ? 0 : -1; }
    if (pe->nvfs >= VFS) return -1;
    vf_e *v = &pe->vfs[pe->nvfs++]; v->eng = eng; v->cid = (uint16_t)ci; v->ok = 0;
    uint32_t ne = 0; const uint8_t *el = eng ? vkn_vfmt_elems(eng, &ne) : NULL;
    if (eng && !el) return -1;
    uint32_t used = 0; for (uint32_t i = 0; i < ne; i++) if (el[6 * i] < VKN_VFMT_STREAMS) used |= 1u << el[6 * i];
    int st = -1; for (int k = VKN_VFMT_STREAMS - 1; k >= 0; k--) if (!(used >> k & 1)) { st = k; break; }
    if (st < 0) { v->stream = 0xff; return -2; }
    const vkn_psk_layout *lay = &pe->cons[ci].lay;
    uint32_t glocs = 0; for (uint32_t g2 = 0; g2 < lay->groups; g2++) glocs |= 1u << lay->loc[g2];
    int spare = -1; for (uint32_t l = 0; l < 16; l++) if (!((pe->p.cons_locs | glocs) >> l & 1)) { spare = (int)l; break; }
    uint8_t e[6 * (255)]; uint32_t n = 0;
    if (ne > 200) return -1;
    memcpy(e, el, 6 * ne); n = ne;
    for (uint32_t w = 0; w < rec_m;) {
        int gi = -1; for (uint32_t g2 = 0; g2 < lay->groups; g2++) if (lay->first[g2] == w) { gi = (int)g2; break; }
        uint8_t u, x; uint32_t cnt;
        if (gi >= 0) { loc_usage(lay->loc[gi], &u, &x); cnt = lay->width[gi]; }
        else {   // filler: up to the next group's first word
            uint32_t nx = rec_m; for (uint32_t g2 = 0; g2 < lay->groups; g2++) if (lay->first[g2] > w && lay->first[g2] < nx) nx = lay->first[g2];
            cnt = nx - w; if (spare < 0 && cnt <= 4) return -1;
            loc_usage(spare < 0 ? 0 : (uint32_t)spare, &u, &x);
        }
        if (n >= 250 || cnt > 255) return -1;
        uint8_t *d = e + 6 * n++; d[0] = (uint8_t)st; d[1] = u; d[2] = x; d[3] = 0; d[4] = 1; d[5] = (uint8_t)cnt;
        w += cnt;
    }
    if (g.nvfmt >= VFMT_MAX) return -3;
    const uint16_t id = vkn_vfmt_put(0, e, n);
    if (!id) { pred(R_VFMT, "vertex format table refused a record format (%u elements)", n); return -3; }
    // the stream's stride must be the record's (every word an element): check what the state piece built
    const vkn_vfmt *f = vkn_vfmt_info(id);
    if (!f || f->stride[st] != rec_m * 4) { pred(R_VFMT, "record format of %u words built a stride of %u", rec_m, f ? f->stride[st] : 0); return -1; }
    v->vfmt = id; v->stream = (uint8_t)st; v->ok = 1; g.nvfmt++; g.s.vfmts++;
    *vfmt = id; *stream = (uint8_t)st;
    return 0;
}

// ---------------------------------------------------------------- the slice pipeline of an engine vertex format (inputs as the draw fetches them)
static VkPipeline slice_pipe(const vkn_shader *s, plan_e *pe, uint16_t eng) {
    for (uint32_t i = 0; i < pe->npipes; i++) if (pe->pipes[i].vfmt == eng) return pe->pipes[i].p;
    if (pe->npipes >= PIPES) return VK_NULL_HANDLE;
    pipe_e *pp = &pe->pipes[pe->npipes++]; pp->vfmt = eng; pp->p = VK_NULL_HANDLE;
    const vkn_dev *d = vkn_device(); const vkn_vfmt *vf = eng ? vkn_vfmt_info(eng) : NULL;
    if (!d || !pe->slice || !pe->pl || !g.rp) return VK_NULL_HANDLE;
    VkVertexInputAttributeDescription at[16]; VkVertexInputBindingDescription bd[VKN_VFMT_STREAMS + 1]; uint32_t nat = 0, nbd = 0, ub = 0;
    for (uint32_t l = 0; l < 16; l++) {
        if (!(pe->p.slice_locs >> l & 1)) continue;
        const VkVertexInputAttributeDescription *a = NULL;
        for (uint32_t i = 0; vf && i < vf->nattr; i++) if (vf->attr[i].location == l) a = &vf->attr[i];
        if (a) { at[nat++] = *a; ub |= 1u << a->binding; }
        else { at[nat].location = l; at[nat].binding = VKN_VFMT_NULL_BINDING; at[nat].format = VK_FORMAT_R32G32B32A32_SFLOAT; at[nat].offset = 0; nat++; ub |= 1u << VKN_VFMT_NULL_BINDING; }
    }
    for (uint32_t b = 0; b <= VKN_VFMT_NULL_BINDING; b++) if (ub >> b & 1) {
        bd[nbd].binding = b; bd[nbd].inputRate = VK_VERTEX_INPUT_RATE_VERTEX; bd[nbd].stride = 0;
        for (uint32_t i = 0; vf && b < VKN_VFMT_NULL_BINDING && i < vf->nbind; i++) if (vf->bind[i].binding == b) bd[nbd].stride = vf->bind[i].stride;
        nbd++;
    }
    VkPipelineShaderStageCreateInfo st; memset(&st, 0, sizeof st);
    st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st.stage = VK_SHADER_STAGE_VERTEX_BIT; st.module = pe->slice; st.pName = "main";
    VkPipelineVertexInputStateCreateInfo vi; memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO; vi.vertexBindingDescriptionCount = nbd; vi.pVertexBindingDescriptions = bd;
    vi.vertexAttributeDescriptionCount = nat; vi.pVertexAttributeDescriptions = at;
    VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.rasterizerDiscardEnable = VK_TRUE; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f;
    // (ignored with rasterizer discard; given valid all the same: a 1x1 viewport / scissor, one sample, no attachment)
    const VkViewport vpv = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f}; const VkRect2D scv = {{0, 0}, {1, 1}};
    VkPipelineViewportStateCreateInfo vp; memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vp.viewportCount = 1; vp.pViewports = &vpv; vp.scissorCount = 1; vp.pScissors = &scv;
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT; ms.minSampleShading = 1.0f;
    VkPipelineColorBlendStateCreateInfo cbs; memset(&cbs, 0, sizeof cbs); cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    VkGraphicsPipelineCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO; ci.stageCount = 1; ci.pStages = &st; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp; ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pColorBlendState = &cbs;
    ci.layout = pe->pl; ci.renderPass = g.rp; ci.subpass = 0; ci.basePipelineIndex = -1;
    const uint64_t t0 = vkn_now_ns();
    if (vkCreateGraphicsPipelines(d->dev, vkn_pipe_cache_get(), 1, &ci, NULL, &pp->p) != VK_SUCCESS) {
        pp->p = VK_NULL_HANDLE; pred(R_PIPE, "slice pipeline of vs %.16s (vertex format %u) failed", s->key[VKN_SRC_VS], eng);
    } else {
        g.s.slice_pipes++;
        if (g.s.slice_pipes <= 16) vkn_logf("preskin: slice pipeline vs %.16s vertex format %u, %u inputs: %.1f ms", s->key[VKN_SRC_VS], eng, nat, (vkn_now_ns() - t0) / 1e6);
    }
    return pp->p;
}

// ---------------------------------------------------------------- frame state
static uint32_t fmt_size(VkFormat f) {   // bytes of the vertex formats vkn_state.c k_vtx uses (+ the widened 4-component ones)
    switch (f) {
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: return 4;
    case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: return 8;
    case VK_FORMAT_R32G32B32_SFLOAT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: return 12;
    case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: return 16;
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_SINT: return 2;
    case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_SINT: return 4;
    case VK_FORMAT_R16G16B16_SFLOAT: case VK_FORMAT_R16G16B16_UNORM: case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16_SNORM: case VK_FORMAT_R16G16B16_SINT: return 6;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_SINT: return 8;
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_SINT: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_SINT: return 2;
    case VK_FORMAT_R8G8B8_UNORM: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8_SNORM: case VK_FORMAT_R8G8B8_SINT: return 3;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_SINT: return 4;
    default: return 0;
    }
}
static void pool_free(ring_e *r) {
    const vkn_dev *d = vkn_device();
    for (uint32_t i = 0; d && i < r->npool; i++) vkDestroyDescriptorPool(d->dev, r->pool[i], NULL);
    r->npool = 0;
}
// begin listener: a presented recording opens a new frame of records; its command buffer starts with the barrier that orders this
// frame's prologue (submitted before it, in the close listener) before every vertex fetch of the frame
static void on_begin(void) {
    g.frame_open = 0; g.pro_open = 0;
    for (uint32_t i = 0; i < g.nchunk; i++) vkn_retire_block(&g.chunk[i]);   // read by the previous frame's draws: freed 3 frames later
    g.nchunk = 0; g.frame_bytes = 0; g.nrec = 0; memset(g.bucket, 0, sizeof g.bucket);
    if (!vkn_rec_presented() || (g.mode != 1 && g.mode != 3 && g.mode != 2)) return;
    g.frame_open = 1; g.s.frames++; g.frame_no++;
    if (g.mode == 2) return;
    VkCommandBuffer cb = vkn_cb(); if (!cb) { g.frame_open = 0; return; }
    VkMemoryBarrier mb; memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER; mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
// the prologue of this frame: the ring entry's command buffer (its previous submission finished) with the no-attachment pass open
static int pro_begin(void) {
    if (g.pro_open) return 0;
    const vkn_dev *d = vkn_device(); if (!d || !g.cpool) return -1;
    ring_e *r = &g.ring[g.cur = (uint32_t)(g.frame_no % RING)];
    if (r->submitted) {
        if (vkWaitForFences(d->dev, 1, &r->fence, VK_TRUE, 5000000000ull) != VK_SUCCESS) { pred(R_VK, "prologue fence did not signal in 5 s: no slices this frame"); return -1; }
        r->submitted = 0;
    }
    pool_free(r);   // its sets were read by that finished prologue only
    if (vkResetFences(d->dev, 1, &r->fence) != VK_SUCCESS || vkResetCommandBuffer(r->cb, 0) != VK_SUCCESS) { pred(R_VK, "prologue reset failed"); return -1; }
    VkCommandBufferBeginInfo bi; memset(&bi, 0, sizeof bi); bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(r->cb, &bi) != VK_SUCCESS) { pred(R_VK, "prologue begin failed"); return -1; }
    VkRenderPassBeginInfo rb; memset(&rb, 0, sizeof rb);
    rb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO; rb.renderPass = g.rp; rb.framebuffer = g.fb; rb.renderArea.extent.width = rb.renderArea.extent.height = 1;
    vkCmdBeginRenderPass(r->cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    g.pro_open = 1; g.pipe_cur = VK_NULL_HANDLE;
    return 0;
}
// close listener: the prologue goes on the queue right before the frame's own submission
static void on_close(void) {
    const int open = g.pro_open; g.frame_open = 0; g.pro_open = 0;
    if (!open) return;
    const vkn_dev *d = vkn_device(); ring_e *r = &g.ring[g.cur];
    vkCmdEndRenderPass(r->cb);
    if (vkEndCommandBuffer(r->cb) != VK_SUCCESS) { pred(R_VK, "prologue end failed: this frame's records hold stale words"); return; }
    if (g.mode == 3) return;   // poison: never submitted
    VkSubmitInfo si; memset(&si, 0, sizeof si); si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &r->cb;
    vkn_queue_lock();
    const VkResult res = d ? vkQueueSubmit(d->queue, 1, &si, r->fence) : VK_ERROR_DEVICE_LOST;
    vkn_queue_unlock();
    if (res != VK_SUCCESS) { pred(R_VK, "prologue vkQueueSubmit %d: this frame's records hold stale words", (int)res); return; }
    r->submitted = 1; g.s.submits++;
}
static VkDescriptorSet set_alloc(VkDescriptorSetLayout l) {
    const vkn_dev *d = vkn_device(); ring_e *r = &g.ring[g.cur];
    VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorSetCount = 1; ai.pSetLayouts = &l;
    VkDescriptorSet s = VK_NULL_HANDLE;
    if (r->npool) { ai.descriptorPool = r->pool[r->npool - 1]; if (vkAllocateDescriptorSets(d->dev, &ai, &s) == VK_SUCCESS) return s; }
    if (r->npool >= POOLS) return VK_NULL_HANDLE;
    const VkDescriptorPoolSize sz[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, POOL_SETS * VKN_PSK_UBOS}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, POOL_SETS}};
    VkDescriptorPoolCreateInfo pi; memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pi.maxSets = POOL_SETS; pi.poolSizeCount = 2; pi.pPoolSizes = sz;
    if (vkCreateDescriptorPool(d->dev, &pi, NULL, &r->pool[r->npool]) != VK_SUCCESS) return VK_NULL_HANDLE;
    ai.descriptorPool = r->pool[r->npool++];
    return vkAllocateDescriptorSets(d->dev, &ai, &s) == VK_SUCCESS ? s : VK_NULL_HANDLE;
}
// record bytes of this frame: (buffer, offset) in a block of the static arena (host-visible, storage + vertex usage)
static int arena_take(uint64_t bytes, VkBuffer *buf, uint64_t *off) {
    const uint64_t al = g.ssbo_align;
    if (g.frame_bytes + bytes > g.budget) return -1;
    for (uint32_t i = 0; i < g.nchunk; i++) {
        const uint64_t at = (g.chunk_used[i] + al - 1) / al * al;
        if (at + bytes <= g.chunk[i].size) { g.chunk_used[i] = at + bytes; *buf = g.chunk[i].buf; *off = g.chunk[i].off + at; g.frame_bytes += bytes; return 0; }
    }
    if (g.nchunk >= CHUNKS) return -1;
    vkn_block *b = &g.chunk[g.nchunk];
    if (vkn_block_alloc(VKN_ARENA_STATIC, bytes > CHUNK ? bytes : CHUNK, (uint32_t)al, b)) return -1;
    g.chunk_used[g.nchunk++] = bytes; *buf = b->buf; *off = b->off; g.frame_bytes += bytes;
    return 0;
}

// ---------------------------------------------------------------- the decision
static psh *psh_of(const vkn_shader *s) {
    const uint32_t id = s->id;
    if (id >= IDS) return NULL;
    psh *h = g_tab[id];
    if (h && h->s == s && h->handle == s->handle) return h;
    if (!h && !(h = g_tab[id] = (psh *)calloc(1, sizeof *h))) return NULL;
    for (uint32_t i = 0; i < h->nplan; i++) { if (vkn_psk_plan_free && !g.test) vkn_psk_plan_free(&h->plan[i]->p); free(h->plan[i]); }   // (a retired record's id came back; its Vulkan objects stay alive)
    memset(h, 0, sizeof *h); h->s = s; h->handle = s->handle;
    int c = 0;
    if (!lib_on()) c = -1;
    else if (g.test) c = g.test->candidate(s);
    else c = s->xs[VKN_SRC_VS] ? vkn_shlib_preskin_candidate(s->xs[VKN_SRC_VS]) : 0;
    h->cand = (int8_t)(c < 0 ? -1 : c ? 1 : 0);
    return h;
}
static int decide(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out) {
    const vkn_shader *s = ctx->shader;
    if (!s || vkn_fx_shader_is(s) || !(s->stages & (1u << VKN_SRC_VS)) || (s->missing | s->unusable)) return 0;
    psh *h = psh_of(s);
    if (!h || h->cand == 0) return 0;   // THE cheap exit
    g.s.draws++;
    int nocolor = !ctx->color[0];
    for (uint32_t i = 1; nocolor && i < VKN_PASS_COLORS; i++) if (ctx->color[i]) nocolor = 0;
    const int pc = nocolor && ctx->depth ? 0 : 1;
    const uint64_t nidx = (uint64_t)ctx->count * (ctx->instances ? ctx->instances : 1);
    int why = -1;
    g.s.cand[pc]++; g.s.cand_idx[pc] += nidx;
#define FAIL(w) do { why = (w); goto fail; } while (0)
    if (h->cand < 0) FAIL(VKN_PSK_W_NOLIB);
    if (!g.frame_open) FAIL(VKN_PSK_W_OFFFRAME);
    {
    VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    if (out->shader || out->discard || (!vkn_prim_topology(ctx->prim, &topo) && topo == VK_PRIMITIVE_TOPOLOGY_POINT_LIST)) FAIL(VKN_PSK_W_PRIM);
    const uint64_t code = retype_code(s, ctx->vfmt);
    plan_e *pe = plan_of(h, code, &why);
    if (!pe) goto fail;
    if (pe->p.m > g.maxm) FAIL(VKN_PSK_W_WIDE);
    // the slice's inputs as the draw binds them (vkn_draw.c prepare: block of the engine handle + the draw's offset, the format's attributes)
    const vkn_vfmt *vf = ctx->vfmt ? vkn_vfmt_info(ctx->vfmt) : NULL;
    uint64_t cur_loc[16], cur_slot[NRPS_SLOTS]; memset(cur_loc, 0, sizeof cur_loc); memset(cur_slot, 0, sizeof cur_slot);
    uint32_t nverts = 0xffffffffu, streams = 0; vkn_block sblk[VKN_VFMT_STREAMS]; uint64_t ssize[VKN_VFMT_STREAMS];
    for (uint32_t l = 0; l < 16; l++) {
        if (!(pe->p.slice_locs >> l & 1)) continue;
        const VkVertexInputAttributeDescription *a = NULL;
        for (uint32_t i = 0; vf && i < vf->nattr; i++) if (vf->attr[i].location == l) a = &vf->attr[i];
        if (!a) { cur_loc[l] = 0x2E40ull; continue; }   // the null stream (a constant)
        const uint32_t st = a->binding; uint32_t stride = 0; int inst = 0;
        if (st >= VKN_VFMT_STREAMS) FAIL(VKN_PSK_W_ATTR);
        for (uint32_t i = 0; i < vf->nbind; i++) if (vf->bind[i].binding == st) { stride = vf->bind[i].stride; inst = vf->bind[i].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE; }
        if (inst) FAIL(VKN_PSK_W_INST);
        const uint32_t fsz = fmt_size(a->format);
        if (!fsz || !stride) FAIL(VKN_PSK_W_STRIDE);
        if (!(streams >> st & 1)) {
            vkn_buf_info bi; memset(&bi, 0, sizeof bi);
            if (!in->vb[st] || vkn_buf_lookup(VKN_BUF_VB, in->vb[st], &bi) || !bi.blk.buf || !bi.blk.size || in->vb_off[st] >= bi.size) FAIL(VKN_PSK_W_UNBOUND);
            sblk[st] = bi.blk; ssize[st] = bi.size - in->vb_off[st]; streams |= 1u << st;
        }
        const uint32_t nv = nrps_nverts(ssize[st], stride, a->offset, fsz); if (nv < nverts) nverts = nv;
        cur_loc[l] = nrps_id_attr((uint32_t)a->format, a->offset, stride, in->vb[st], ssize[st], (uint64_t)(uintptr_t)sblk[st].buf, sblk[st].off + in->vb_off[st]);
    }
    if (nverts == 0xffffffffu || !nverts) FAIL(VKN_PSK_W_STRIDE);
    if (nverts > g.maxv) FAIL(VKN_PSK_W_BIG);
    // the slice's uniform blocks as the draw binds them (vkn_draw.c build_set: ring offset of the announced slot, range by the binding's bytes)
    VkDescriptorBufferInfo ubi[VKN_PSK_UBOS]; memset(ubi, 0, sizeof ubi);
    const VkBuffer ring = vkn_ring_buffer(); const uint64_t ring_base = vkn_ring_base();
    for (uint32_t i = 0; i < pe->p.nubo; i++) {
        uint32_t off = 0, size = 0;
        if (vkn_draw_ubo_get(0, pe->slot[i], &off, &size) || !size || !ring) FAIL(VKN_PSK_W_UNBOUND);
        uint64_t range = size; if (pe->bytes[i] && pe->bytes[i] < range) range = pe->bytes[i];
        if (range > 65536) range = 65536;
        if (range > g.ubo_max) range = g.ubo_max;
        if ((ring_base + off) % g.ubo_align) FAIL(VKN_PSK_W_UBO);
        cur_slot[pe->slot[i]] = nrps_id_ubo(off, range, (uint64_t)(uintptr_t)ring, ring_base);
        ubi[i].buffer = ring; ubi[i].offset = ring_base + off; ubi[i].range = range;
    }
    // the record: own / another shader's that holds every value under the same leaves, else a new one (and its slice)
    const uint64_t bucket = nrps_mix(cur_slot[pe->slot[pe->bone]], 0xB0E5ull);
    uint32_t bi0 = (uint32_t)(bucket ^ bucket >> 29) & (BUCKETS - 1), pos[16]; int hit = -1;
    while (g.bucket[bi0] && g.bucket_key[bi0] != bucket) bi0 = (bi0 + 1) & (BUCKETS - 1);
    for (uint32_t r = g.bucket[bi0]; r && hit < 0; r = g.next[r - 1]) if (nrps_match(&g.rec[r - 1], pe->want, pe->nwant, cur_loc, cur_slot, nverts, pos)) hit = (int)r - 1;
    const uint32_t m = pe->p.m;
    if (hit >= 0) {
        g.rec[hit].uses++;
        if (g.rec[hit].m == m && nrps_pos_identity(pos, m)) g.s.own[pe->cls]++; else g.s.sup[pe->cls]++;
    } else {
        if (g.nrec >= MAXREC) FAIL(VKN_PSK_W_ARENA);
        nrps_rec *r = &g.rec[g.nrec]; memset(r, 0, sizeof *r);
        r->bucket = bucket; r->m = m; r->nverts = nverts; r->nvals = pe->nwant; memcpy(r->v, pe->want, sizeof(nrps_val) * pe->nwant); r->uses = 1;
        for (uint32_t l = 0; l < 16; l++) if (pe->p.slice_locs >> l & 1) r->loc_id[l] = cur_loc[l];
        for (uint32_t i = 0; i < pe->p.nubo; i++) r->slot_id[pe->slot[i]] = cur_slot[pe->slot[i]];
        const uint64_t bytes = (uint64_t)nverts * m * 4;
        VkBuffer rb = VK_NULL_HANDLE; uint64_t ro = 0;
        if (g.mode != 2) {
            if (arena_take(bytes, &rb, &ro)) FAIL(VKN_PSK_W_ARENA);
            const VkPipeline p = slice_pipe(s, pe, ctx->vfmt);
            if (!p) FAIL(VKN_PSK_W_PIPE);
            if (pro_begin()) FAIL(VKN_PSK_W_POOL);
            const VkDescriptorSet set = set_alloc(pe->dsl);
            if (!set) FAIL(VKN_PSK_W_POOL);
            VkWriteDescriptorSet wr[VKN_PSK_UBOS + 1]; memset(wr, 0, sizeof wr); uint32_t nw = 0;
            VkDescriptorBufferInfo sb; sb.buffer = rb; sb.offset = ro; sb.range = bytes;
            for (uint32_t i = 0; i < pe->p.nubo; i++) {
                VkWriteDescriptorSet *x = &wr[nw++]; x->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x->dstSet = set; x->dstBinding = pe->p.ubo_binding[i];
                x->descriptorCount = 1; x->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; x->pBufferInfo = &ubi[i];
            }
            { VkWriteDescriptorSet *x = &wr[nw++]; x->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; x->dstSet = set; x->dstBinding = pe->p.ssbo_binding;
              x->descriptorCount = 1; x->descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; x->pBufferInfo = &sb; }
            vkUpdateDescriptorSets(vkn_device()->dev, nw, wr, 0, NULL);
            const VkCommandBuffer cb = g.ring[g.cur].cb;
            if (p != g.pipe_cur) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); g.pipe_cur = p; }
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pe->pl, 0, 1, &set, 0, NULL);
            for (uint32_t st = 0; st < VKN_VFMT_STREAMS; st++) if (streams >> st & 1) { const VkDeviceSize o = sblk[st].off + in->vb_off[st]; vkCmdBindVertexBuffers(cb, st, 1, &sblk[st].buf, &o); }
            for (uint32_t l = 0; l < 16; l++) if ((pe->p.slice_locs >> l & 1) && cur_loc[l] == 0x2E40ull) {
                if (!g.null_ok) { g.null_ok = vkn_block_alloc(VKN_ARENA_STATIC, 16, 16, &g.nullb) ? -1 : 1; if (g.null_ok > 0) { const float v[4] = {0.0f, 0.0f, 0.0f, 1.0f}; memcpy(g.nullb.cpu, v, 16); } }
                if (g.null_ok < 0) FAIL(VKN_PSK_W_ATTR);   // (the slice draw below is then not recorded: the set is wasted, nothing else)
                const VkDeviceSize o = g.nullb.off; vkCmdBindVertexBuffers(cb, VKN_VFMT_NULL_BINDING, 1, &g.nullb.buf, &o);
                break;
            }
            vkCmdDraw(cb, nverts, 1, 0, 0);
            g.s.slices++;
        }
        r->off = ro; g.rbuf[g.nrec] = rb;
        g.next[g.nrec] = g.bucket[bi0]; g.bucket[bi0] = (uint16_t)(g.nrec + 1); g.bucket_key[bi0] = bucket;
        hit = (int)g.nrec++;
        g.s.recs[pe->cls]++; g.s.rec_verts[pe->cls] += nverts; g.s.rec_bytes += bytes; g.s.ops_slice += (uint64_t)nverts * pe->p.ops_slice;
        for (uint32_t j = 0; j < m; j++) pos[j] = j;
    }
    uint32_t ci = 0;
    const nrps_rec *rec = &g.rec[hit];
    const cons_e *c = cons_of(s, pe, pos, nrps_pos_hash(pos, m, rec->m), &ci);
    if (!c) FAIL(VKN_PSK_W_CONS);
    g.s.ops_saved += nidx * (pe->p.ops_orig - pe->p.ops_cons);
    if (g.mode == 2) { g.s.served[pc]++; g.s.served_idx[pc] += nidx; return 0; }
    uint16_t vfmt = 0; uint8_t stream = 0;
    const int vr = vfmt_of(pe, ci, ctx->vfmt, rec->m, &vfmt, &stream);
    if (vr == -2) FAIL(VKN_PSK_W_STREAM);
    if (vr) FAIL(VKN_PSK_W_VFMT);
    out->vin.vs = c->mod; out->vin.vs_id = c->vs_id; out->vin.vfmt = vfmt; out->vin.stream = stream; out->vin.buf = g.rbuf[hit]; out->vin.off = rec->off;
    g.s.served[pc]++; g.s.served_idx[pc] += nidx;
    return 1;
    }
fail:
#undef FAIL
    g.s.why[why]++; g.s.why_idx[why] += nidx;
    return 0;
}
static void census_maybe(void);
static void psk_fx(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)user;
    if (!g.inst) return;                   // (host tests: set up off again)
    if (!ctx->draw_no) census_maybe();     // (once a frame)
    decide(in, ctx, out);
}
int vkn_preskin_apply(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out) {
    if (!g.inst || !in || !ctx || !ctx->shader || !out) return 0;
    return decide(in, ctx, out);
}
// Order 260 (after MAT / XLQ at 200, the pre-pass at 220, GBUF_LE at 250): the consumer stays the vertex module of a later function's
// swapped object only when that object carries the engine vertex stage's bindings (same binding number, descriptor type and count: MAT's
// cut-down object and XLQ's object copy the engine's table): the consumer reads a subset of them. Otherwise the override goes (counted).
static void psk_late(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)in; (void)user;
    if (!g.inst || !out->vin.vs || !out->shader || !ctx->shader) return;
    const vkn_shader *e = ctx->shader, *o = out->shader;
    for (uint32_t i = 0; i < e->nbind; i++) {
        const vkn_bind *b = &e->bind[i]; if (b->bundle_stage != 0) continue;
        int found = 0;
        for (uint32_t k = 0; k < o->nbind && !found; k++) found = o->bind[k].binding == b->binding && o->bind[k].type == b->type && o->bind[k].count == b->count;
        if (!found) {
            memset(&out->vin, 0, sizeof out->vin);
            const uint64_t n = (uint64_t)ctx->count * (ctx->instances ? ctx->instances : 1);
            g.s.why[VKN_PSK_W_LATE]++; g.s.why_idx[VKN_PSK_W_LATE] += n;
            int nocolor = !ctx->color[0];
            for (uint32_t c = 1; nocolor && c < VKN_PASS_COLORS; c++) if (ctx->color[c]) nocolor = 0;
            const int pc = nocolor && ctx->depth ? 0 : 1;   // (decide's class of the draw: undo its "served")
            if (g.s.served[pc]) { g.s.served[pc]--; g.s.served_idx[pc] -= n; }
            return;
        }
    }
    g.s.composed++;
}

// ---------------------------------------------------------------- install, census
int vkn_preskin_mode(void) { return g.inst ? g.mode : 0; }
void vkn_preskin_stats_get(vkn_preskin_stats *s) { if (s) *s = g.s; }
static int objects(void) {
    const vkn_dev *d = vkn_device(); if (!d) return -1;
    g.ssbo_align = (uint32_t)d->props.limits.minStorageBufferOffsetAlignment; if (g.ssbo_align < 64) g.ssbo_align = 64;
    g.ubo_align = (uint32_t)d->props.limits.minUniformBufferOffsetAlignment; if (!g.ubo_align) g.ubo_align = 256;
    g.ubo_max = d->props.limits.maxUniformBufferRange ? d->props.limits.maxUniformBufferRange : 16384;
    if (g.mode == 2) return 0;
    VkSubpassDescription sp; memset(&sp, 0, sizeof sp); sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    VkRenderPassCreateInfo rc; memset(&rc, 0, sizeof rc); rc.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO; rc.subpassCount = 1; rc.pSubpasses = &sp;
    if (vkCreateRenderPass(d->dev, &rc, NULL, &g.rp) != VK_SUCCESS) return -1;
    VkFramebufferCreateInfo fc; memset(&fc, 0, sizeof fc); fc.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; fc.renderPass = g.rp; fc.width = fc.height = fc.layers = 1;
    if (vkCreateFramebuffer(d->dev, &fc, NULL, &g.fb) != VK_SUCCESS) return -1;
    VkCommandPoolCreateInfo pc; memset(&pc, 0, sizeof pc); pc.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pc.queueFamilyIndex = d->family;
    if (vkCreateCommandPool(d->dev, &pc, NULL, &g.cpool) != VK_SUCCESS) return -1;
    for (int i = 0; i < RING; i++) {
        VkCommandBufferAllocateInfo ai; memset(&ai, 0, sizeof ai); ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ai.commandPool = g.cpool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VkFenceCreateInfo fi; memset(&fi, 0, sizeof fi); fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkAllocateCommandBuffers(d->dev, &ai, &g.ring[i].cb) != VK_SUCCESS || vkCreateFence(d->dev, &fi, NULL, &g.ring[i].fence) != VK_SUCCESS) return -1;
    }
    return 0;
}
static int reg_fn(void) {
    if (!g.reg) {
        g.reg = !vkn_fx_register("preskin", psk_fx, NULL, 150) && !vkn_fx_register("preskin-late", psk_late, NULL, 260);
        if (g.reg) { vkn_rec_on_begin(on_begin); vkn_rec_on_close(on_close); }
    }
    return g.reg;
}
static void install(int mode) {
    g.mode = mode;
    g.maxm = env_u("DM_VK_NATIVE_PRESKIN_MAXM", 16, 1, 16); g.maxv = env_u("DM_VK_NATIVE_PRESKIN_MAXV", 65536, 256, 1u << 22);
    g.budget = (uint64_t)env_u("DM_VK_NATIVE_PRESKIN_MB", 8, 1, 256) << 20;
    const vkn_dev *d = vkn_device();
    if ((g.mode == 1 || g.mode == 3) && d && !d->feat.vertexPipelineStoresAndAtomics) {
        pred(R_CFG, "vertexPipelineStoresAndAtomics is off on this device: verify only (=2)"); g.mode = 2;
    }
    if (g.mode && objects()) { pred(R_CFG, "no render pass / command buffers / fences: off"); g.mode = 0; }
    if (g.mode && !reg_fn()) { pred(R_CFG, "the draw-effect chain refused the function: off"); g.mode = 0; }
    g.inst = g.mode != 0;
}
void vkn_preskin_install(uint32_t vendor, const char *device_name) {
    static int done; if (done) return; done = 1;
    g.family = vkn_shlib_policy && vkn_shlib_policy() >= 0 ? vkn_shlib_policy() : vkn_policy_family_from(vendor, device_name, getenv("DM_GFX_SIM_GPU"));
    const int m = vkn_preskin_switch(g.family);
    install(m);
    vkn_logf("DM_VK_NATIVE_PRESKIN=%d (stage 5.3 V1, replaces DM_NR_PRESKIN: %s; family %s default %d): skinning vertex shaders cut, the slice once per mesh and frame in a "
             "prologue submitted ahead of the frame, consumers read the record; records up to %u words, meshes up to %u vertices, %llu MB of records a frame; shader library %s",
             g.mode, g.mode == 0 ? "off" : g.mode == 1 ? "on" : g.mode == 2 ? "verify - planned and counted, the engine's shaders drawn" : "POISON - slices never run",
             vkn_policy_family_name(g.family), vkn_preskin_default(g.family), g.maxm, g.maxv, (unsigned long long)(g.budget >> 20), lib_on() ? "on" : "OFF (every draw counted as 'no shader library')");
}
void vkn_preskin_test_setup(int mode, const vkn_preskin_test_lib *lib) {
    for (uint32_t i = 0; i < IDS; i++) if (g_tab[i]) { for (uint32_t k = 0; k < g_tab[i]->nplan; k++) free(g_tab[i]->plan[k]); free(g_tab[i]); g_tab[i] = NULL; }
    g.test = lib; g.retype = -1; g.nvfmt = 0;   // (the frame's records and prologue stay: a test may switch inside a frame)
    g.mode = mode; g.maxm = 16; g.maxv = 65536; g.budget = 8u << 20;
    if ((mode == 1 || mode == 3) && !g.cpool && objects()) { pred(R_CFG, "test: no objects"); g.mode = 0; }
    if (mode == 2 && !g.ubo_align) objects();
    g.inst = g.mode != 0 && reg_fn();
}
static void census_maybe(void) {
    const uint64_t now = vkn_now_ns();
    if (now < g.next_ns) return;
    if (g.next_ns) vkn_preskin_census();
    g.next_ns = now + 10000000000ull;
}
void vkn_preskin_census(void) {
    if (!g.inst) return;
    typedef unsigned long long ull;
    const vkn_preskin_stats *s = &g.s, *o = &g.last;
    const double fr = s->frames > o->frames ? (double)(s->frames - o->frames) : 1.0;
#define D(f) ((double)(s->f - o->f) / fr)
    char w[900]; int k = 0; w[0] = 0;
    for (int i = 0; i < VKN_PSK_W_N && k < (int)sizeof w - 80; i++) if (s->why[i] != o->why[i])
        k += snprintf(w + k, sizeof w - (size_t)k, "%s%s %.1f (%.3f M idx)", k ? ", " : "", k_why[i], D(why[i]), D(why_idx[i]) / 1e6);
    vkn_logf("preskin: DM_VK_NATIVE_PRESKIN=%d (last 10 s, %.0f frames, a frame): skinned draws depth-only %.1f (%.3f M idx) / colour %.1f (%.3f M); %s depth-only %.1f "
             "(%.3f M idx) / colour %.1f (%.3f M), with a MAT / XLQ object %.1f; meshes pre-skinned: position-class %.1f (%.1f k vertices) / wider %.1f (%.1f k), %.2f MB of records; "
             "reuse own %.1f / %.1f, another shader's record %.1f / %.1f; slices %.1f, prologue submits %.2f, pass breaks added 0, dispatches added 0; model (SPIR-V body ops) saved "
             "%.2f M - slice %.2f M; plans %llu (refused %llu), consumers %llu, slice pipelines %llu, record formats %llu; not: %s; red lines %llu",
             g.mode, fr, D(cand[0]), D(cand_idx[0]) / 1e6, D(cand[1]), D(cand_idx[1]) / 1e6, g.mode == 2 ? "would serve" : "served", D(served[0]), D(served_idx[0]) / 1e6,
             D(served[1]), D(served_idx[1]) / 1e6, D(composed), D(recs[0]), D(rec_verts[0]) / 1e3, D(recs[1]), D(rec_verts[1]) / 1e3, D(rec_bytes) / 1048576.0,
             D(own[0]), D(own[1]), D(sup[0]), D(sup[1]), D(slices), D(submits), D(ops_saved) / 1e6, D(ops_slice) / 1e6, (ull)s->plans, (ull)s->plans_refused,
             (ull)s->consumers, (ull)s->slice_pipes, (ull)s->vfmts, k ? w : "-", (ull)(s->reds - o->reds));
#undef D
    g.last = g.s;
}
#endif
