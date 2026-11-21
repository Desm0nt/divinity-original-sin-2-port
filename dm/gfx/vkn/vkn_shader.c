// [DOS2] VK-NATIVE shaders (vkn_shader.h): engine shader handle -> stage modules of our packs + descriptor-set layout + pipeline layout +
// binding table. No game addresses (dm/host/hle_dos2_vkn_shader.c passes the control / code pointers of each stage).
// The modules and their variants come from a shader loader with the translator's contract (xbg_vkn_stage, xbg_game.h): in mode 1 with
// DM_VK_NATIVE_SHLIB on (the default there) the backend's own library (vkn_shlib_shader / _alpha_module / _retype_module, vkn_shlib.h:
// the translator is not initialised), otherwise the translator's doors (xbg_vkn_shader / xbg_vkn_alpha_module / xbg_vkn_retype_module).
// Both give the same bundle bytes, the same load-time SPIR-V rewrites, the same "#at" / "#vi" variants the old path draws with (host test
// vkn_shlib_test.cpp: word for word over the whole pack). The loader owns the modules (they live until the process ends), so this module
// never destroys a VkShaderModule.
// Set layouts are shared by content (a cache that only grows: distinct binding lists are a few hundred); a shader whose list does not
// fit the cache owns its layouts. Everything a record owns (binding table, c1 blocks, own layouts, its id) goes through vkn_retire.
// Threads: one mutex (g_mu) for the table, the id list, the layout cache and the counters. Every translator call runs on a private
// worker thread with an 8 MB stack: CreateShader runs on engine threads whose guest stacks are small, and a bundle load (SPIR-V rewrites)
// is not. Creations are queued (the engine thread computes the keys, copies the control sections and returns: it never waits for the
// translator's mutex, which its own thread holds for a whole submission); vkn_shader_get waits for a creation still queued, a destroy
// cancels one; the module variants are waited for.
#ifdef DM_GAME_DOS2
#include "vkn_shader.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "../vulkan/engine/xbg_game.h"   // xbg_vkn_stage, xbg_shader_binding, XBG_SOURCE_* (the door to the packs)
#include "vkn_fx.h"            // [stage 5 A1] VKN_BF_CMP, VKN_FX_SHADER_ID0
#include "vkn_shlib_named.h"   // [stage 5 A1] engine shaders by name (DM_VK_NATIVE_SH) + Dref
#include "vkn_drv.h"           // [stage 5.3 V2] the family's dynamic-uniform-buffer rules

// The translator and the key (weak: host tests link without them; then nothing is created).
extern int xbg_shader_program_key(const void *control, const void *code, char out_hex17[17]) __attribute__((weak));
extern int xbg_vkn_shader(const char *key, const void *control, const void *code, xbg_vkn_stage *out) __attribute__((weak));
extern VkShaderModule xbg_vkn_alpha_module(void *shader, uint32_t func, float ref) __attribute__((weak));
extern VkShaderModule xbg_vkn_retype_module(void *shader, uint64_t code) __attribute__((weak));
// The backend's shader library (vkn_shlib.cpp; weak: host tests mock it or leave it out)
extern int vkn_shlib_on(void) __attribute__((weak));
extern int vkn_shlib_shader(const char *key, const void *control, const void *code, xbg_vkn_stage *out) __attribute__((weak));
extern VkShaderModule vkn_shlib_alpha_module(void *shader, uint32_t func, float ref) __attribute__((weak));
extern VkShaderModule vkn_shlib_retype_module(void *shader, uint64_t code) __attribute__((weak));
// The loader in effect (chosen once: the switch is read once by vkn_shlib_on)
typedef struct sh_loader {
    int (*shader)(const char *key, const void *control, const void *code, xbg_vkn_stage *out);
    VkShaderModule (*alpha)(void *shader, uint32_t func, float ref);
    VkShaderModule (*retype)(void *shader, uint64_t code);
    const char *name;
} sh_loader;
static const sh_loader *loader(void) {
    static sh_loader l; static int picked;   // (a race picks the same values twice)
    if (!__atomic_load_n(&picked, __ATOMIC_ACQUIRE)) {
        if (vkn_shlib_on && vkn_shlib_on()) l = (sh_loader){vkn_shlib_shader, vkn_shlib_alpha_module, vkn_shlib_retype_module, "the backend's shader library"};
        else l = (sh_loader){xbg_vkn_shader, xbg_vkn_alpha_module, xbg_vkn_retype_module, "the translator"};
        __atomic_store_n(&picked, 1, __ATOMIC_RELEASE);
    }
    return &l;
}
extern void xbg_support_defaults(void *bytes1392, float width, float height) __attribute__((weak));   // xbg_vulkan.cpp (stateless)
// [stage 5 A1] named shaders (vkn_shlib_named.cpp) and the pass a pipeline is built for (vkn_tex.c / vkn_pass.c). Weak: the host tests
// link this file without them (then no shader is named).
extern int vkn_shlib_named_entry(const char *fs_key, uint64_t *dref_mask) __attribute__((weak));
extern int vkn_shlib_named_pass(int entry, int ncolor, const uint32_t *fmt, uint32_t depth_fmt, uint32_t samples, uint32_t w, uint32_t h) __attribute__((weak));
extern int vkn_shlib_named_alt(const char *fs_key, xbg_vkn_stage *out) __attribute__((weak));
extern void vkn_shlib_named_flagged(uint32_t bindings) __attribute__((weak));
extern void vkn_shlib_named_alt_result(int ok) __attribute__((weak));
extern void vkn_shlib_named_census(void) __attribute__((weak));
extern int vkn_tex_on_rec_thread(void) __attribute__((weak));
extern vkn_tex *vkn_pass_color(uint32_t i) __attribute__((weak));
extern vkn_tex *vkn_pass_depth(void) __attribute__((weak));

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
// [DRS 21.11] SUPPORT_CAP was 32 for the whole process: dynamic resolution makes new viewport sizes at every step (the 3D part and
// its half / quarter / eighth chain), so after a few steps every new size got no block, the draw path bound the zero block (1/w, 1/h, w,
// h = 0: every full-screen pass sampled one texel -> a flat grey-green screen under the HUD, the UI dropped its draws: S08). Now 256
// sizes, and when they are all taken the least recently used size idle for SUPPORT_IDLE frames is rewritten in place (the GPU is
// done with it: 3 frames in flight at most); vkn_shader_support_fill gives the bytes for a frame-local copy when even that fails.
enum { SH_TAB = 1 << 16, LAYOUT_CAP = 4096, SUPPORT_CAP = 256, SUPPORT_IDLE = 16, SUPPORT_BYTES = 1392, MISS_REDS = 16, BLOCK_REDS = 8 };

// nr_entry: [stage 5 A1] NR-04 table entry + 1 of a fragment stage the library replaced by name (0 = none); nr_alt: the translated
// module for a pass outside the verified one (vkn_shlib_named_alt, made on first need; state 0 not tried, 1 made, -1 refused), under g_mu.
typedef struct sh_rec { vkn_shader pub; uint8_t own_layout; uint8_t nr_entry; int8_t nr_alt_state; VkShaderModule nr_alt; } sh_rec;
static uint32_t g_cmp_reds; static uint64_t g_bit30;   // [stage 5 A1] red lines of the Dref flag; bundle flags that carried bit 30
typedef struct sh_layout { uint64_t hash; uint32_t nb; VkDescriptorSetLayoutBinding *b; VkDescriptorSetLayout set; VkPipelineLayout pl; } sh_layout;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static vkn_tab g_tab; static int g_tab_ok;          // index -> sh_rec *
static uint32_t *g_free_id, g_nfree, g_capfree, g_next_id = 1;
static sh_layout g_lay[LAYOUT_CAP]; static uint32_t g_nlay;
static struct { float w, h; vkn_block b; uint64_t used; } g_sup[SUPPORT_CAP]; static uint32_t g_nsup;   // used: vkn_frame() of the last lookup
static uint64_t g_sup_recycled, g_sup_full, g_sup_fills; static int g_sup_reds;
static vkn_shader_stats g_st;
static void (*g_dead)(uint32_t, uint64_t);
static int g_miss_reds, g_block_reds, g_odd_reds;

// ---- the worker: a private thread with an 8 MB stack running jobs in order (creations queued, variants waited for) ----
typedef struct wjob { void (*fn)(void *); void (*drop)(void *); void *arg; uint64_t handle, seq; int async; struct wjob *next; } wjob;
static pthread_mutex_t g_q_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_q_go = PTHREAD_COND_INITIALIZER, g_q_done = PTHREAD_COND_INITIALIZER;
static wjob *g_q_head, *g_q_tail; static uint64_t g_q_seq, g_q_fin, g_q_run_seq, g_q_run_handle; static uint32_t g_q_len; static int g_w_state;   // 0 not started, 1 up, -1 failed
static void *worker_main(void *unused) {
    (void)unused;
    pthread_mutex_lock(&g_q_mu);
    for (;;) {
        while (!g_q_head) pthread_cond_wait(&g_q_go, &g_q_mu);
        wjob *j = g_q_head; g_q_head = j->next; if (!g_q_head) g_q_tail = NULL; g_q_len--;
        const uint64_t seq = j->seq; const int async = j->async;
        g_q_run_seq = seq; g_q_run_handle = async ? j->handle : 0;
        pthread_mutex_unlock(&g_q_mu);
        if (j->fn) j->fn(j->arg);
        if (async) free(j);   // a waited-for job lives on its caller's stack: not touched after g_q_fin says it is done
        pthread_mutex_lock(&g_q_mu);
        g_q_run_seq = g_q_run_handle = 0; g_q_fin = seq; pthread_cond_broadcast(&g_q_done);
    }
    return NULL;
}
static int worker_up(void) {   // under g_q_mu
    if (!g_w_state) {
        pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8u << 20); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        g_w_state = pthread_create(&t, &a, worker_main, NULL) ? -1 : 1; pthread_attr_destroy(&a);
        if (g_w_state < 0) vkn_logf(RED "shaders: no worker thread: bundles load on the caller's own stack, synchronously");
    }
    return g_w_state > 0;
}
static void q_push(wjob *j) {   // under g_q_mu
    j->seq = ++g_q_seq; j->next = NULL;
    if (g_q_tail) g_q_tail->next = j; else g_q_head = j;
    g_q_tail = j; g_q_len++;
    pthread_cond_signal(&g_q_go);
}
static void run_big(void (*fn)(void *), void *arg) {   // fn(arg) on the worker, waited for (fn NULL: wait for everything queued)
    wjob j = {fn, NULL, arg, 0, 0, 0, NULL};
    pthread_mutex_lock(&g_q_mu);
    if (!worker_up()) { pthread_mutex_unlock(&g_q_mu); if (fn) fn(arg); return; }
    q_push(&j);
    while (g_q_fin < j.seq) pthread_cond_wait(&g_q_done, &g_q_mu);
    pthread_mutex_unlock(&g_q_mu);
}
static int run_async(void (*fn)(void *), void (*drop)(void *), void *arg, uint64_t handle) {   // 0 = queued (fn or drop owns arg from now on)
    wjob *j = (wjob *)malloc(sizeof *j); if (!j) return -1;
    *j = (wjob){fn, drop, arg, handle, 0, 1, NULL};
    pthread_mutex_lock(&g_q_mu);
    if (!worker_up()) { pthread_mutex_unlock(&g_q_mu); free(j); return -1; }
    q_push(j);
    pthread_mutex_unlock(&g_q_mu);
    return 0;
}
// The queued creations of this handle: cancelled (cancel = 1: the engine destroyed it) or waited for; a running one is waited for.
static void q_settle(uint64_t handle, int cancel) {
    wjob *gone = NULL; uint64_t wait = 0;
    pthread_mutex_lock(&g_q_mu);
    for (wjob *prev = NULL, *j = g_q_head; j;) {
        wjob *nx = j->next;
        if (j->async && j->handle == handle) {
            if (cancel) { if (prev) prev->next = nx; else g_q_head = nx; if (g_q_tail == j) g_q_tail = prev; g_q_len--; j->next = gone; gone = j; j = nx; continue; }
            wait = j->seq;
        }
        prev = j; j = nx;
    }
    if (g_q_run_handle == handle && g_q_run_seq > wait) wait = g_q_run_seq;
    while (g_q_fin < wait) pthread_cond_wait(&g_q_done, &g_q_mu);
    pthread_mutex_unlock(&g_q_mu);
    while (gone) { wjob *nx = gone->next; if (gone->drop) gone->drop(gone->arg); free(gone); gone = nx; }
}

// ---- small helpers ----
static int tab_ready(void) {   // under g_mu
    if (!g_tab_ok) g_tab_ok = vkn_tab_init(&g_tab, SH_TAB, sizeof(sh_rec *)) ? -1 : 1;
    return g_tab_ok > 0;
}
static uint32_t id_take(void) { return g_nfree ? g_free_id[--g_nfree] : g_next_id++; }   // under g_mu
static void id_give(uint32_t id) {   // under g_mu
    if (g_nfree == g_capfree) {
        const uint32_t cap = g_capfree ? g_capfree * 2 : 1024; uint32_t *n = (uint32_t *)realloc(g_free_id, cap * sizeof *n);
        if (!n) return;   // the id is simply never reused
        g_free_id = n; g_capfree = cap;
    }
    g_free_id[g_nfree++] = id;
}
static int src_engine_stage(int k) { return k == VKN_SRC_VS ? VKN_STAGE_VS : k == VKN_SRC_FS ? VKN_STAGE_FS : VKN_STAGE_CS; }
static uint32_t bundle_stage_bit(uint32_t st) {
    static const uint32_t bits[6] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                     VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT};
    return st < 6 ? bits[st] : VK_SHADER_STAGE_ALL;
}
static uint8_t kind_of(uint32_t source) {
    switch (source) {
    case XBG_SOURCE_UNIFORM: return VKN_BK_UNIFORM;
    case XBG_SOURCE_SUPPORT: return VKN_BK_SUPPORT;
    case XBG_SOURCE_C1: return VKN_BK_C1;
    case XBG_SOURCE_TEXTURE: return VKN_BK_TEXTURE;
    case XBG_SOURCE_IMAGE: return VKN_BK_IMAGE;
    case XBG_SOURCE_STORAGE: return VKN_BK_STORAGE;
    case XBG_SOURCE_DRIVER0: return VKN_BK_DRIVER0;
    case XBG_SOURCE_DRIVER2: return VKN_BK_DRIVER2;
    default: return VKN_BK_OTHER;
    }
}

// [VK-DRV] docs/dos2/nr/VK-DRIVERS.md: a layout beyond the device's per-stage / per-set descriptor limits is invalid Vulkan (stock
// drivers fail the layout or the pipeline, some crash in vkCreateGraphicsPipelines; the spec minimums are 12 UBO / 16 samplers / 16
// sampled images / 4 SSBO / 4 storage images / 128 resources per stage, 8 dynamic UBO per set - PowerVR and old Mali report about that).
// Nothing can be split here: a red line names the limit (one per distinct overflow, 16 at most), the creation goes on as before. A limit
// the driver reports as 0 is unknown (host mocks): not checked.
static void limits_note(const VkDescriptorSetLayoutBinding *b, uint32_t nb) {
    const vkn_dev *d = vkn_device(); if (!d) return;
    const VkPhysicalDeviceLimits *L = &d->props.limits;
    uint32_t ubo[6] = {0}, smp[6] = {0}, img[6] = {0}, ssbo[6] = {0}, simg[6] = {0}, all[6] = {0}, dyn = 0, sdyn = 0;
    for (uint32_t i = 0; i < nb; i++) {
        const uint32_t c = b[i].descriptorCount; const VkDescriptorType t = b[i].descriptorType;
        if (t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) dyn += c;
        if (t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) sdyn += c;
        for (int s = 0; s < 6; s++) {
            if (!(b[i].stageFlags & (1u << s))) continue;
            if (t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) ubo[s] += c;
            else if (t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) { smp[s] += c; img[s] += c; }
            else if (t == VK_DESCRIPTOR_TYPE_SAMPLER) smp[s] += c;
            else if (t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || t == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) img[s] += c;
            else if (t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) ssbo[s] += c;
            else if (t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || t == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) simg[s] += c;
            all[s] += c;
        }
    }
    const char *what = NULL; uint32_t have = 0, lim = 0;
#define LIM_CHECK(v, l, name) do { if (!what && (l) && (v) > (l)) { what = name; have = (v); lim = (l); } } while (0)
    for (int s = 0; s < 6; s++) {
        LIM_CHECK(ubo[s], L->maxPerStageDescriptorUniformBuffers, "maxPerStageDescriptorUniformBuffers");
        LIM_CHECK(smp[s], L->maxPerStageDescriptorSamplers, "maxPerStageDescriptorSamplers");
        LIM_CHECK(img[s], L->maxPerStageDescriptorSampledImages, "maxPerStageDescriptorSampledImages");
        LIM_CHECK(ssbo[s], L->maxPerStageDescriptorStorageBuffers, "maxPerStageDescriptorStorageBuffers");
        LIM_CHECK(simg[s], L->maxPerStageDescriptorStorageImages, "maxPerStageDescriptorStorageImages");
        LIM_CHECK(all[s], L->maxPerStageResources, "maxPerStageResources");
    }
    LIM_CHECK(dyn, L->maxDescriptorSetUniformBuffersDynamic, "maxDescriptorSetUniformBuffersDynamic");
    LIM_CHECK(sdyn, L->maxDescriptorSetStorageBuffersDynamic, "maxDescriptorSetStorageBuffersDynamic");
#undef LIM_CHECK
    static int reds;
    if (what && __atomic_add_fetch(&reds, 1, __ATOMIC_RELAXED) <= 16)
        vkn_logf(RED "shaders: a descriptor layout (%u bindings) needs %u against %s %u of this device: invalid Vulkan, the driver may refuse or crash (created as before)", nb, have, what, lim);
}
// The set layout + pipeline layout of a binding list: shared by content; *own = 1 when the cache is full (the caller owns them).
static int layouts_get(const VkDescriptorSetLayoutBinding *b, uint32_t nb, VkDescriptorSetLayout *set, VkPipelineLayout *pl, uint8_t *own) {
    const vkn_dev *d = vkn_device(); if (!d) return -1;
    const uint64_t h = vkn_fnv(b, sizeof *b * nb) ^ nb;
    *own = 0;
    pthread_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < g_nlay; i++)
        if (g_lay[i].hash == h && g_lay[i].nb == nb && !memcmp(g_lay[i].b, b, sizeof *b * nb)) { *set = g_lay[i].set; *pl = g_lay[i].pl; pthread_mutex_unlock(&g_mu); return 0; }
    pthread_mutex_unlock(&g_mu);
    limits_note(b, nb);   // [VK-DRV] (a layout is made once per content)
    const VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nb, .pBindings = nb ? b : NULL};
    VkDescriptorSetLayout s = VK_NULL_HANDLE; VkPipelineLayout p = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(d->dev, &li, NULL, &s) != VK_SUCCESS) return -1;
    const VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &s};
    if (vkCreatePipelineLayout(d->dev, &pi, NULL, &p) != VK_SUCCESS) { vkDestroyDescriptorSetLayout(d->dev, s, NULL); return -1; }
    VkDescriptorSetLayoutBinding *copy = nb ? (VkDescriptorSetLayoutBinding *)malloc(sizeof *b * nb) : NULL;
    if (copy) memcpy(copy, b, sizeof *b * nb);
    pthread_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < g_nlay; i++)   // another thread made the same one meanwhile: take it, drop ours
        if (g_lay[i].hash == h && g_lay[i].nb == nb && !memcmp(g_lay[i].b, b, sizeof *b * nb)) {
            *set = g_lay[i].set; *pl = g_lay[i].pl; pthread_mutex_unlock(&g_mu);
            vkDestroyPipelineLayout(d->dev, p, NULL); vkDestroyDescriptorSetLayout(d->dev, s, NULL); free(copy); return 0;
        }
    if (g_nlay < LAYOUT_CAP && (copy || !nb)) { g_lay[g_nlay++] = (sh_layout){h, nb, copy, s, p}; g_st.layouts++; copy = NULL; }
    else { *own = 1; g_st.layouts_own++; }
    pthread_mutex_unlock(&g_mu);
    free(copy);
    *set = s; *pl = p;
    return 0;
}

// ---- retirement of a record (vkn_retire: 3 frames after it left the table) ----
static void rec_free(void *ctx) {
    sh_rec *r = (sh_rec *)ctx; vkn_shader *s = &r->pub;
    const vkn_dev *d = vkn_device();
    for (int k = 0; k < VKN_SRCS; k++) if (s->c1[k].size) vkn_block_free(&s->c1[k]);
    if (r->own_layout && d) { vkDestroyPipelineLayout(d->dev, s->layout, NULL); vkDestroyDescriptorSetLayout(d->dev, s->set_layout, NULL); }
    pthread_mutex_lock(&g_mu);
    if (r->own_layout && g_st.layouts_own) g_st.layouts_own--;
    g_st.table_bytes -= sizeof(vkn_bind) * s->nbind;
    for (int k = 0; k < VKN_SRCS; k++) g_st.c1_bytes -= s->c1[k].size;
    id_give(s->id);
    pthread_mutex_unlock(&g_mu);
    free((void *)s->bind); free(r);
}
static void rec_retire(sh_rec *r) {   // outside g_mu
    if (g_dead) g_dead(r->pub.id, r->pub.handle);
    vkn_retire(rec_free, r);
}

// ---- creation (on the worker) ----
enum { CTL_COPY = 0xe48, NAME_COPY = 96 };
// A queued creation: the keys (computed on the engine's thread), the control sections (copies: the engine frees its own), the code
// pointers (only for the missing-stage dump; a destroy cancels the job first, so the code is still there when it runs).
typedef struct sh_job {
    uint64_t handle, t_key; int rc; uint8_t has, keyed;   // t_key: ns the engine's thread spent on the keys and copies
    char key[VKN_SRCS][17]; uint8_t ctl[VKN_SRCS][CTL_COPY]; const void *code[VKN_SRCS]; char name[VKN_SRCS][NAME_COPY]; uint32_t mask[VKN_SRCS];
} sh_job;
static int cmp_bind(const void *a, const void *b) { const vkn_bind *x = (const vkn_bind *)a, *y = (const vkn_bind *)b; return (int)x->binding - (int)y->binding; }
static void create_job(void *arg) {
    sh_job *j = (sh_job *)arg; j->rc = -1;
    const vkn_dev *d = vkn_device();
    if (!d || !loader()->shader) return;
    xbg_vkn_stage *st = (xbg_vkn_stage *)calloc(VKN_SRCS, sizeof *st);
    sh_rec *r = (sh_rec *)calloc(1, sizeof *r);
    vkn_bind *tb = (vkn_bind *)calloc(VKN_SRCS * 256, sizeof *tb);
    VkDescriptorSetLayoutBinding *lb = (VkDescriptorSetLayoutBinding *)calloc(VKN_SRCS * 256, sizeof *lb);
    if (!st || !r || !tb || !lb) { free(st); free(r); free(tb); free(lb); return; }
    vkn_shader *s = &r->pub; s->handle = j->handle;
    uint32_t nb = 0; uint64_t n_stages = 0, n_bundle = 0, n_miss = 0, n_unus = 0, n_mism = 0, n_noblock = 0;
    uint32_t n_cmp = 0, n_bit30 = 0; uint64_t dref = 0; int named = -1;   // [stage 5 A1]
    for (int k = 0; k < VKN_SRCS; k++) {
        if (!(j->has >> k & 1)) continue;
        s->stages |= (uint8_t)(1u << k); n_stages++;
        if (!(j->keyed >> k & 1)) { s->missing |= (uint8_t)(1u << k); n_miss++; continue; }
        memcpy(s->key[k], j->key[k], 17);
        const int lr = loader()->shader(s->key[k], j->ctl[k], j->code[k], &st[k]);
        if (lr < 0) { free(st); free(r); free(tb); free(lb); return; }   // the loader is not up: nothing to build from
        if (lr > 0) {
            s->missing |= (uint8_t)(1u << k); n_miss++;
            pthread_mutex_lock(&g_mu); const int red = g_miss_reds < MISS_REDS ? ++g_miss_reds : 0; pthread_mutex_unlock(&g_mu);
            if (red) vkn_logf(RED "shaders: no bundle for the %s stage of engine shader %#llx: key %s, file '%s' (its draws must be skipped; %d of the first %d)",
                              k == VKN_SRC_VS ? "vertex" : k == VKN_SRC_FS ? "fragment" : "compute", (unsigned long long)j->handle, s->key[k],
                              j->name[k][0] ? j->name[k] : "?", red, MISS_REDS);
            continue;
        }
        n_bundle++;
        const uint32_t want = k == VKN_SRC_VS ? 0 : k == VKN_SRC_FS ? 4 : 5;
        if (st[k].stage != want) {
            n_mism++;
            pthread_mutex_lock(&g_mu); const int red = g_odd_reds < 8 ? ++g_odd_reds : 0; pthread_mutex_unlock(&g_mu);
            if (red) vkn_logf(RED "shaders: engine shader %#llx: the %s program's bundle %s is of stage %u", (unsigned long long)j->handle,
                              k == VKN_SRC_VS ? "vertex" : k == VKN_SRC_FS ? "fragment" : "compute", s->key[k], st[k].stage);
        }
        s->xs[k] = st[k].shader; s->module[k] = st[k].module;
        if (!st[k].module) { s->unusable |= (uint8_t)(1u << k); n_unus++; }
        // [stage 5 A1] a fragment bundle the library replaced by name (NR-04): its entry (the pass check at pipeline creation) and the
        // bindings its module samples with a depth comparison (Dref: they get VKN_BF_CMP below)
        if (k == VKN_SRC_FS && vkn_shlib_named_entry) named = vkn_shlib_named_entry(s->key[k], &dref);
        if (k == VKN_SRC_VS) {
            s->ninputs = (uint8_t)st[k].input_count;
            for (uint32_t i = 0; i < st[k].input_count; i++) { s->inputs[i] = (uint8_t)st[k].inputs[i]; s->input_classes[i] = (uint8_t)st[k].input_classes[i]; }
        }
        for (uint32_t i = 0; i < st[k].binding_count; i++) {
            const xbg_shader_binding *b = &st[k].bindings[i];
            vkn_bind *t = &tb[nb];
            t->binding = (uint16_t)b->binding; t->kind = kind_of(b->source); t->stage = (uint8_t)src_engine_stage(k); t->bundle_stage = (uint8_t)st[k].stage;
            t->slot = t->kind == VKN_BK_SUPPORT || t->kind == VKN_BK_C1 ? 0 : (uint8_t)b->slot; t->count = (uint8_t)(b->count ? b->count : 1);
            t->type = (VkDescriptorType)b->type; t->bytes = b->bytes; t->flags = b->flags & ~VKN_BF_CMP; t->dyn = VKN_NO_DYN;
            if (b->flags & VKN_BF_CMP) n_bit30++;   // [stage 5 A1] bit 30 is ours (vkn_fx.h): no bundle flag may carry it
            if (k == VKN_SRC_FS && dref && t->kind == VKN_BK_TEXTURE && b->binding < 64 && (dref >> b->binding & 1)) { t->flags |= VKN_BF_CMP; n_cmp++; }
            if (t->kind == VKN_BK_UNIFORM && b->slot < 8 && (j->mask[k] >> 31) && !(j->mask[k] >> b->slot & 1)) {
                n_noblock++;
                pthread_mutex_lock(&g_mu); const int red = g_block_reds < BLOCK_REDS ? ++g_block_reds : 0; pthread_mutex_unlock(&g_mu);
                if (red) vkn_logf(RED "shaders: engine shader %#llx (%s) binding %u reads uniform slot %u of stage %d: the engine record has no block with that binding index (mask %#x)",
                                  (unsigned long long)j->handle, s->key[k], b->binding, b->slot, t->stage, j->mask[k] & 0x7fffffffu);
            }
            nb++;
        }
    }
    if (dref && !n_cmp) {   // [stage 5 A1] a Dref module whose depth bindings are not texture bindings of the table
        pthread_mutex_lock(&g_mu); const int red = g_cmp_reds < 16 ? (int)++g_cmp_reds : 0; pthread_mutex_unlock(&g_mu);
        if (red) vkn_logf(RED "shaders: engine shader %#llx (%s): its named module samples bindings %#llx with a depth comparison, none is a texture binding of the table: no comparison sampler",
                          (unsigned long long)j->handle, s->key[VKN_SRC_FS], (unsigned long long)dref);
    }
    if (n_cmp && vkn_shlib_named_flagged) vkn_shlib_named_flagged(n_cmp);
    if (n_bit30) {
        pthread_mutex_lock(&g_mu); g_bit30 += n_bit30; const int red = g_cmp_reds < 16 ? (int)++g_cmp_reds : 0; pthread_mutex_unlock(&g_mu);
        if (red) vkn_logf(RED "shaders: engine shader %#llx: %u bundle binding(s) carry flag bit 30 (VKN_BF_CMP): cleared", (unsigned long long)j->handle, n_bit30);
    }
    r->nr_entry = (uint8_t)(named >= 0 && named < 255 ? named + 1 : 0);
    qsort(tb, nb, sizeof *tb, cmp_bind);
    uint32_t w = 0;   // a binding number twice (two stages cannot collide: x 6 + stage): keep the first
    for (uint32_t i = 0; i < nb; i++) if (!w || tb[i].binding != tb[w - 1].binding) tb[w++] = tb[i];
    nb = w;
    // the dynamic-offset policy (the translator's dyn_mode_for) and the dynamic indices in binding order
    uint32_t all = 0, vary = 0;
    for (uint32_t i = 0; i < nb; i++) if (tb[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) { all += tb[i].count; if (tb[i].kind == VKN_BK_UNIFORM || tb[i].kind == VKN_BK_DRIVER0 || tb[i].kind == VKN_BK_DRIVER2) vary += tb[i].count; }
    // [stage 5.3 V2] the family's rules (vkn_drv.h, xbg_game.cpp dyn_mode_for / UBO_DYN): DM_GFX_UBO_DYNAMIC off (PowerVR) = every block
    // static; the NR-08b read-only rule (DM_NR_UBODYN=1, PowerVR default) = per-draw blocks only (never mode 1), and a block is dynamic
    // only when the shader library scanned it and the stage reads it (an unread dynamic block is PowerVR's overrun); SIM ubdyn8: limit 8.
    const vkn_drv_cfg *dc = vkn_drv();
    uint32_t lim = d->props.limits.maxDescriptorSetUniformBuffersDynamic;
    if (dc->sim_ubdyn8 && lim > 8) lim = 8;
    s->dyn_policy = (uint8_t)(!dc->ubo_dyn ? 0 : dc->ubo_ro ? (vary && vary <= lim ? 2 : 0) : all && all <= lim ? 1 : vary && vary <= lim ? 2 : 0);
    uint32_t dyn = 0, ro_static = 0, ro_dyn = 0;
    enum { BIND_UNREAD = 1u << 29, BIND_SCANNED = 1u << 28 };   // xbg_nr_ubodyn.inc XBG_BIND_UNREAD / _SCANNED (the library's ubd_mark)
    for (uint32_t i = 0; i < nb; i++) {
        vkn_bind *t = &tb[i];
        if (t->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && (s->dyn_policy == 1 || (s->dyn_policy == 2 && (t->kind == VKN_BK_UNIFORM || t->kind == VKN_BK_DRIVER0 || t->kind == VKN_BK_DRIVER2)))) {
            if (dc->ubo_ro && (t->flags & (BIND_SCANNED | BIND_UNREAD)) != BIND_SCANNED) ro_static += t->count;   // [stage 5.3 V2] stays static
            else { if (dc->ubo_ro) ro_dyn += t->count; t->type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; t->dyn = (uint8_t)dyn; dyn += t->count; }
        }
        lb[i] = (VkDescriptorSetLayoutBinding){.binding = t->binding, .descriptorType = t->type, .descriptorCount = t->count, .stageFlags = bundle_stage_bit(t->bundle_stage)};
    }
    s->ndyn = (uint16_t)dyn;
    if ((!dc->ubo_dyn && all) || ro_static || ro_dyn) vkn_drv_ubo_note(!dc->ubo_dyn && all, ro_static, ro_dyn);   // [stage 5.3 V2] census
    if (layouts_get(lb, nb, &s->set_layout, &s->layout, &r->own_layout)) {
        vkn_logf(RED "shaders: engine shader %#llx: vkCreateDescriptorSetLayout / vkCreatePipelineLayout failed (%u bindings, %u dynamic)", (unsigned long long)j->handle, nb, dyn);
        free(st); free(r); free(tb); free(lb); return;
    }
    // c1: the shader's own constants, once, as the translator binds them (uniform_range: >= 16 bytes, a multiple of 16)
    uint64_t c1_bytes = 0;
    for (int k = 0; k < VKN_SRCS; k++) {
        int has = 0;
        for (uint32_t i = 0; i < nb; i++) has |= tb[i].kind == VKN_BK_C1 && tb[i].stage == src_engine_stage(k);
        if (!has) continue;
        const uint64_t n = st[k].c1_bytes > 16 ? (st[k].c1_bytes + 15ull) & ~15ull : 16;
        uint32_t al = (uint32_t)d->props.limits.minUniformBufferOffsetAlignment; if (al < 16) al = 16;
        if (vkn_block_alloc(VKN_ARENA_STATIC, n, al, &s->c1[k])) { memset(&s->c1[k], 0, sizeof s->c1[k]); vkn_logf(RED "shaders: engine shader %#llx: no arena block for its c1 constants", (unsigned long long)j->handle); continue; }
        memset(s->c1[k].cpu, 0, (size_t)s->c1[k].size);
        if (st[k].c1 && st[k].c1_bytes) memcpy(s->c1[k].cpu, st[k].c1, st[k].c1_bytes);
        c1_bytes += s->c1[k].size;
    }
    vkn_bind *tab = (vkn_bind *)malloc(sizeof *tab * (nb ? nb : 1));
    if (tab) memcpy(tab, tb, sizeof *tab * nb);
    s->bind = tab; s->nbind = tab ? (uint16_t)nb : 0;
    free(st); free(tb); free(lb);
    // into the table (an older record under the same index: retired)
    sh_rec *old = NULL;
    pthread_mutex_lock(&g_mu);
    s->id = id_take();
    sh_rec **slot = tab_ready() ? (sh_rec **)vkn_tab_put(&g_tab, j->handle, NULL) : NULL;
    if (slot) { old = *slot; *slot = r; }
    g_st.stages += n_stages; g_st.stages_bundle += n_bundle; g_st.stages_missing += n_miss; g_st.stages_unusable += n_unus; g_st.stage_mismatch += n_mism; g_st.ubo_no_block += n_noblock;
    if (slot) {
        g_st.created++; g_st.alive++; g_st.dyn_policy[s->dyn_policy]++; g_st.table_bytes += sizeof(vkn_bind) * s->nbind; g_st.c1_bytes += c1_bytes;
        for (uint32_t i = 0; i < s->nbind; i++) g_st.bind_kind[s->bind[i].kind <= VKN_BK_OTHER ? s->bind[i].kind : 0]++;
        if (old) { g_st.replaced++; g_st.alive--; g_st.dyn_policy[old->pub.dyn_policy]--; }
    }
    pthread_mutex_unlock(&g_mu);
    if (!slot) {   // handle index beyond the table (or no table): not kept; rc -1 counts it
        pthread_mutex_lock(&g_mu); g_st.table_bytes += sizeof(vkn_bind) * s->nbind; g_st.c1_bytes += c1_bytes; pthread_mutex_unlock(&g_mu);   // rec_free takes them off
        vkn_retire(rec_free, r); return;
    }
    if (old) rec_retire(old);
    j->rc = 0;
}

static void create_run(void *arg) {   // the job, then its time and outcome
    sh_job *j = (sh_job *)arg; const uint64_t t0 = vkn_now_ns();
    create_job(j);
    pthread_mutex_lock(&g_mu); g_st.ns += vkn_now_ns() - t0 + j->t_key; if (j->rc) g_st.failed++; pthread_mutex_unlock(&g_mu);
    free(j);
}
static void create_drop(void *arg) {   // cancelled before it ran (the engine destroyed the shader)
    pthread_mutex_lock(&g_mu); g_st.cancelled++; pthread_mutex_unlock(&g_mu);
    free(arg);
}
int vkn_shader_create(uint64_t handle, const vkn_stage_src src[VKN_SRCS]) {
    if (!handle || !src || !vkn_device() || !loader()->shader || !xbg_shader_program_key) return -1;
    sh_job *j = (sh_job *)calloc(1, sizeof *j); if (!j) return -1;
    const uint64_t t0 = vkn_now_ns(); j->handle = handle;
    for (int k = 0; k < VKN_SRCS; k++) {
        if (!src[k].control) continue;
        j->has |= (uint8_t)(1u << k);
        if (!xbg_shader_program_key(src[k].control, src[k].code, j->key[k])) j->keyed |= (uint8_t)(1u << k);
        memcpy(j->ctl[k], src[k].control, CTL_COPY); j->code[k] = src[k].code; j->mask[k] = src[k].engine_ubo_mask;
        if (src[k].name) { strncpy(j->name[k], src[k].name, NAME_COPY - 1); j->name[k][NAME_COPY - 1] = 0; }
    }
    j->t_key = vkn_now_ns() - t0;
    if (run_async(create_run, create_drop, j, handle)) create_run(j);   // no worker: here, now
    return 0;
}

static const vkn_shader *tab_find(uint64_t handle) {
    const vkn_shader *s = NULL;
    pthread_mutex_lock(&g_mu);
    if (g_tab_ok > 0) { sh_rec **p = (sh_rec **)vkn_tab_get(&g_tab, handle); if (p && *p) s = &(*p)->pub; }
    pthread_mutex_unlock(&g_mu);
    return s;
}
const vkn_shader *vkn_shader_get(uint64_t handle) {
    const vkn_shader *s = tab_find(handle);
    if (!s && handle) { q_settle(handle, 0); s = tab_find(handle); }   // still queued: wait for its creation
    return s;
}
const vkn_shader *vkn_shader_peek(uint64_t handle) { return tab_find(handle); }   // [stage 5 technique Z] never waits
void vkn_shader_sync(void) { run_big(NULL, NULL); }

void vkn_shader_destroy(uint64_t handle) {
    q_settle(handle, 1);   // a creation still queued is dropped, a running one finished first
    sh_rec *r = NULL;
    pthread_mutex_lock(&g_mu);
    if (g_tab_ok > 0) {
        sh_rec **p = (sh_rec **)vkn_tab_get(&g_tab, handle);
        if (p && *p) { r = *p; *p = NULL; vkn_tab_kill(&g_tab, handle); g_st.destroyed++; g_st.alive--; g_st.dyn_policy[r->pub.dyn_policy]--; }
    }
    pthread_mutex_unlock(&g_mu);
    if (r) rec_retire(r);
}

void vkn_shader_set_dead_fn(void (*fn)(uint32_t id, uint64_t handle)) { g_dead = fn; }

// ---- module variants (loader calls, on the big-stack thread) ----
typedef struct var_job { void *xs; int kind; uint32_t func; float ref; uint64_t code; VkShaderModule out; } var_job;
static void var_run(void *arg) {
    var_job *v = (var_job *)arg;
    const sh_loader *l = loader();
    if (v->kind == 1) v->out = l->alpha ? l->alpha(v->xs, v->func, v->ref) : VK_NULL_HANDLE;
    else v->out = l->retype ? l->retype(v->xs, v->code) : VK_NULL_HANDLE;
}
// [stage 5 A1] the translated module of a named fragment stage (vkn_shlib_named_alt, the library's "<key>#nrt"). Refused when it is not
// a usable fragment module or reads more of a uniform block than the named module's binding table binds (the table is the named one's:
// the heads are equal byte for byte, the uniform-extent trim follows each module's own reads).
typedef struct alt_job { const vkn_shader *s; VkShaderModule out; const char *why; } alt_job;
static void alt_run(void *arg) {
    alt_job *a = (alt_job *)arg; a->out = VK_NULL_HANDLE; a->why = "no library";
    xbg_vkn_stage *st = (xbg_vkn_stage *)calloc(1, sizeof *st);
    if (!st || !vkn_shlib_named_alt) { free(st); return; }
    const int rc = vkn_shlib_named_alt(a->s->key[VKN_SRC_FS], st);
    a->why = rc ? "no bundle" : st->stage != 4 ? "not a fragment bundle" : !st->module ? "unusable on this device" : NULL;
    for (uint32_t i = 0; !a->why && i < st->binding_count; i++) {
        const xbg_shader_binding *b = &st->bindings[i];
        if (b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) continue;
        const vkn_bind *t = NULL;
        for (uint32_t k = 0; k < a->s->nbind; k++) if (a->s->bind[k].binding == b->binding) { t = &a->s->bind[k]; break; }
        const uint32_t need = b->bytes ? b->bytes : 65536, have = t ? (t->bytes ? t->bytes : 65536) : 0;
        if (need > have) a->why = "it reads more of a uniform block than the named module's table binds";
    }
    if (!a->why) a->out = st->module;
    free(st);
}
// [stage 5 A1] The fragment module of a named shader for the pipeline being built: on the recording thread the current target set is
// that pipeline's pass (vkn_draw.c builds after vkn_pass_ready; the pipeline key holds the pass's compatibility class, so this runs once
// per shader and pass); off it (the warm-up worker) the pass is not known: the named module, counted. Outside the verified pass the
// translated module (vkn_shlib_named_pass decides; a Dref module stays).
static VkShaderModule named_fs(sh_rec *r) {
    const vkn_shader *s = &r->pub;
    if (!vkn_shlib_named_pass) return s->module[VKN_SRC_FS];
    uint32_t fmt[VKN_PASS_COLORS], depth = 0, samples = 1, w = 0, h = 0; int n = -1;
    if (vkn_tex_on_rec_thread && vkn_pass_color && vkn_tex_on_rec_thread()) {
        n = 0;
        for (uint32_t i = 0; i < VKN_PASS_COLORS; i++) {
            const vkn_tex *t = vkn_pass_color(i); if (!t) continue;
            if (!n) { samples = t->samples ? t->samples : 1; w = t->w; h = t->h; }
            fmt[n++] = t->nvn_fmt;
        }
        const vkn_tex *dt = vkn_pass_depth ? vkn_pass_depth() : NULL;
        if (dt) { depth = dt->nvn_fmt; if (!n) { samples = dt->samples ? dt->samples : 1; w = dt->w; h = dt->h; } }
    }
    if (!vkn_shlib_named_pass(r->nr_entry - 1, n, fmt, depth, samples, w, h)) return s->module[VKN_SRC_FS];
    pthread_mutex_lock(&g_mu); int state = r->nr_alt_state; pthread_mutex_unlock(&g_mu);
    if (!state) {
        alt_job a = {s, VK_NULL_HANDLE, NULL};
        run_big(alt_run, &a);
        pthread_mutex_lock(&g_mu);
        const int first = !r->nr_alt_state;
        if (first) { r->nr_alt = a.out; r->nr_alt_state = a.out ? 1 : -1; }
        state = r->nr_alt_state;
        pthread_mutex_unlock(&g_mu);
        if (first && vkn_shlib_named_alt_result) vkn_shlib_named_alt_result(a.out != VK_NULL_HANDLE);
        if (first && !a.out) vkn_logf(RED "shaders: engine shader %#llx (%s): no translated module for a pass outside the verified one (%s): the named module is used there",
                                      (unsigned long long)s->handle, s->key[VKN_SRC_FS], a.why ? a.why : "?");
    }
    return state > 0 ? r->nr_alt : s->module[VKN_SRC_FS];
}
VkShaderModule vkn_shader_module(const vkn_shader *s, int stage, uint32_t alpha_func, float alpha_ref, uint64_t retype) {
    if (!s) return VK_NULL_HANDLE;
    const int k = stage == VKN_STAGE_VS ? VKN_SRC_VS : stage == VKN_STAGE_FS ? VKN_SRC_FS : stage == VKN_STAGE_CS ? VKN_SRC_CS : -1;
    if (k < 0 || !s->module[k] || !s->xs[k]) return VK_NULL_HANDLE;
    var_job v = {s->xs[k], 0, alpha_func, alpha_ref, retype, s->module[k]};
    if (k == VKN_SRC_FS && alpha_func && alpha_func != 8) v.kind = 1;
    else if (k == VKN_SRC_VS && retype) v.kind = 2;
    // [stage 5 A1] a named fragment module: the pass check (an alpha-tested draw keeps the named bundle's "#at" variant, as before)
    if (!v.kind && k == VKN_SRC_FS && s->id < (uint32_t)VKN_FX_SHADER_ID0 && ((const sh_rec *)s)->nr_entry) return named_fs((sh_rec *)s);
    if (!v.kind) return s->module[k];
    run_big(var_run, &v);
    return v.out ? v.out : s->module[k];
}

// ---- the SUPPORT block per viewport size ----
int vkn_shader_support(float width, float height, vkn_block *out) {
    const vkn_dev *d = vkn_device();
    if (!out || !d || !xbg_support_defaults || !(width > 0) || !(height > 0)) return -1;
    int rc = -1, note = 0; const uint64_t now = vkn_frame();
    pthread_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < g_nsup; i++) if (g_sup[i].w == width && g_sup[i].h == height) { g_sup[i].used = now; *out = g_sup[i].b; rc = 0; break; }
    if (rc && g_nsup < SUPPORT_CAP) {
        uint32_t al = (uint32_t)d->props.limits.minUniformBufferOffsetAlignment; if (al < 16) al = 16;
        vkn_block b;
        if (!vkn_block_alloc(VKN_ARENA_STATIC, SUPPORT_BYTES, al, &b)) {
            xbg_support_defaults(b.cpu, width, height);
            g_sup[g_nsup].w = width; g_sup[g_nsup].h = height; g_sup[g_nsup].b = b; g_sup[g_nsup].used = now; g_nsup++;
            *out = b; rc = 0;
        }
    }
    if (rc && g_nsup) {   // [DRS 21.11] every size taken (or no arena memory): rewrite the least recently used idle one in place
        uint32_t k = UINT32_MAX;
        for (uint32_t i = 0; i < g_nsup; i++)
            if (g_sup[i].used + SUPPORT_IDLE <= now && (k == UINT32_MAX || g_sup[i].used < g_sup[k].used)) k = i;
        if (k != UINT32_MAX) {
            xbg_support_defaults(g_sup[k].b.cpu, width, height);   // (static arena: host-coherent, persistently mapped)
            g_sup[k].w = width; g_sup[k].h = height; g_sup[k].used = now;
            *out = g_sup[k].b; rc = 0; note = ++g_sup_recycled == 1;
        } else { g_sup_full++; note = g_sup_reds < BLOCK_REDS ? (g_sup_reds++, 2) : 0; }
    }
    const uint32_t n = g_nsup;
    pthread_mutex_unlock(&g_mu);
    if (note == 1) vkn_logf("shaders: SUPPORT blocks: all %u viewport sizes taken (dynamic resolution steps): the least recently used idle size is rewritten "
                            "in place from now on (%ux%u)", n, (unsigned)width, (unsigned)height);
    else if (note == 2) vkn_logf(RED "shaders: SUPPORT block for %gx%g: all %u sizes used in the last %d frames: a frame-local copy is bound", (double)width, (double)height, n, SUPPORT_IDLE);
    return rc;
}
// [DRS 21.11] the safety net: the block's bytes for a frame-local copy (the constant ring) when vkn_shader_support has none. 0 = ok.
int vkn_shader_support_fill(float width, float height, void *dst, uint32_t bytes) {
    if (!dst || bytes < SUPPORT_BYTES || !xbg_support_defaults || !(width > 0) || !(height > 0)) return -1;
    xbg_support_defaults(dst, width, height);
    pthread_mutex_lock(&g_mu); g_sup_fills++; pthread_mutex_unlock(&g_mu);
    return 0;
}
void vkn_shader_support_stats(uint32_t *sizes, uint64_t *recycled, uint64_t *full, uint64_t *fills) {
    pthread_mutex_lock(&g_mu);
    if (sizes) *sizes = g_nsup;
    if (recycled) *recycled = g_sup_recycled;
    if (full) *full = g_sup_full;
    if (fills) *fills = g_sup_fills;
    pthread_mutex_unlock(&g_mu);
}

void vkn_shader_stats_get(vkn_shader_stats *out) {
    if (!out) return;
    pthread_mutex_lock(&g_q_mu); const uint32_t q = g_q_len; pthread_mutex_unlock(&g_q_mu);
    pthread_mutex_lock(&g_mu); *out = g_st; pthread_mutex_unlock(&g_mu);
    out->queued = q;
}
void vkn_shader_census(void) {
    vkn_shader_stats s; vkn_shader_stats_get(&s);
    vkn_logf("shaders (from %s): %llu alive (created %llu, replaced %llu, destroyed %llu, failed %llu, queued %llu, cancelled %llu); stages %llu: with a bundle %llu, without %llu, unusable %llu; "
             "layouts %llu shared + %llu own; binding tables %.1f KB, c1 %.1f KB; dynamic all / engine blocks / none: %llu / %llu / %llu; "
             "bindings uniform %llu, support %llu, c1 %llu, texture %llu, image %llu; %.1f ms creating",
             loader()->name, (unsigned long long)s.alive, (unsigned long long)s.created, (unsigned long long)s.replaced, (unsigned long long)s.destroyed, (unsigned long long)s.failed, (unsigned long long)s.queued, (unsigned long long)s.cancelled,
             (unsigned long long)s.stages, (unsigned long long)s.stages_bundle, (unsigned long long)s.stages_missing, (unsigned long long)s.stages_unusable,
             (unsigned long long)s.layouts, (unsigned long long)s.layouts_own, s.table_bytes / 1024.0, s.c1_bytes / 1024.0,
             (unsigned long long)s.dyn_policy[1], (unsigned long long)s.dyn_policy[2], (unsigned long long)s.dyn_policy[0],
             (unsigned long long)s.bind_kind[VKN_BK_UNIFORM], (unsigned long long)s.bind_kind[VKN_BK_SUPPORT], (unsigned long long)s.bind_kind[VKN_BK_C1],
             (unsigned long long)s.bind_kind[VKN_BK_TEXTURE], (unsigned long long)s.bind_kind[VKN_BK_IMAGE], s.ns / 1e6);
    const uint64_t odd = s.bind_kind[VKN_BK_STORAGE] + s.bind_kind[VKN_BK_DRIVER0] + s.bind_kind[VKN_BK_DRIVER2] + s.bind_kind[VKN_BK_OTHER] + s.bind_kind[0];
    if (odd) vkn_logf(RED "shaders: bindings of kinds no DOS2 bundle was seen with: storage %llu, driver0 %llu, driver2 %llu, other %llu (the draw path has no source for them yet)",
                      (unsigned long long)s.bind_kind[VKN_BK_STORAGE], (unsigned long long)s.bind_kind[VKN_BK_DRIVER0], (unsigned long long)s.bind_kind[VKN_BK_DRIVER2],
                      (unsigned long long)(s.bind_kind[VKN_BK_OTHER] + s.bind_kind[0]));
    if (s.stage_mismatch || s.ubo_no_block) vkn_logf(RED "shaders: %llu stages whose bundle is of another stage, %llu uniform bindings without an engine block", (unsigned long long)s.stage_mismatch, (unsigned long long)s.ubo_no_block);
    {   // [DRS 21.11] SUPPORT blocks by viewport size (one line while dynamic resolution has made more than the first sizes)
        uint32_t n = 0; uint64_t rec = 0, full = 0, fills = 0; vkn_shader_support_stats(&n, &rec, &full, &fills);
        if (rec || full || fills) vkn_logf("shaders: SUPPORT blocks: %u viewport sizes (cap %d), rewritten in place %llu, none idle %llu, frame-local copies %llu",
                                           n, SUPPORT_CAP, (unsigned long long)rec, (unsigned long long)full, (unsigned long long)fills);
    }
    if (vkn_shlib_named_census) vkn_shlib_named_census();   // [stage 5 A1] engine shaders by name + Dref (one line)
}
// ==== [DOS2] VK-NATIVE stage 4 (pipeline warm-up, vkn_warm.c): lookups the replay of the pipeline list needs ====
uint32_t vkn_shader_each(int (*fn)(const vkn_shader *s, void *ctx), void *ctx) {
    uint32_t n = 0;
    if (!fn) return 0;
    pthread_mutex_lock(&g_mu);
    if (g_tab_ok > 0)
        for (uint32_t i = 0; i < g_tab.n; i++) {
            if (!g_tab.handle[i]) continue;
            sh_rec **p = (sh_rec **)vkn_tab_at(&g_tab, i);
            if (!p || !*p) continue;
            n++;
            if (fn(&(*p)->pub, ctx)) break;
        }
    pthread_mutex_unlock(&g_mu);
    return n;
}
typedef struct { const char *vs, *fs; const vkn_shader *hit; } keys_q;
static int keys_match(const vkn_shader *s, void *ctx) {
    keys_q *q = (keys_q *)ctx;
    if (strcmp(s->key[VKN_SRC_VS], q->vs) || strcmp(s->key[VKN_SRC_FS], q->fs)) return 0;
    q->hit = s; return 1;
}
const vkn_shader *vkn_shader_find_keys(const char *vs_key, const char *fs_key) {
    keys_q q = {vs_key ? vs_key : "", fs_key ? fs_key : "", NULL};
    vkn_shader_each(keys_match, &q);
    return q.hit;
}
uint64_t vkn_shader_epoch(void) {
    pthread_mutex_lock(&g_mu); const uint64_t e = g_st.created + g_st.replaced; pthread_mutex_unlock(&g_mu);
    return e;
}
// [stage 5.1 G] vkn_shader.h: the library's NR-09 scan of the vertex object's final words (fixed at load, no lock); every other case
// (a piece's shader of vkn_fx_shader_create, the translator's loader, no vertex object, a host test without the library) is "reads it".
extern int vkn_shlib_vs_vindex(const void *shader) __attribute__((weak));
int vkn_shader_vindex(const vkn_shader *s) {
    if (!s || vkn_fx_shader_is(s) || !vkn_shlib_vs_vindex || loader()->shader != vkn_shlib_shader || !s->xs[VKN_SRC_VS]) return 1;
    return vkn_shlib_vs_vindex(s->xs[VKN_SRC_VS]) == 0 ? 0 : 1;
}
#endif
