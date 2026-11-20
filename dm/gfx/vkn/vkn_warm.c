// [DOS2] VK-NATIVE stage 4, pipeline warm-up (vkn_warm.h): the driver cache file, the list of built pipelines, the replay worker.
// No game addresses. The worker is the only thread doing file I/O here; the render thread only queues (vkn_warm_note) and asks
// (vkn_warm_take). Lock order: none - g_mu is never held while calling into another module that could call back (vkn_state's
// table mutex is never taken under g_mu, and vkn_state never holds its table mutex while calling this file).
#ifdef DM_GAME_DOS2
#include "vkn_warm.h"
#include "vkn_shader.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

// The directory of the cache files (the parent of the shader directory): the backend's shader library in mode 1 with DM_VK_NATIVE_SHLIB on
// (vkn_shlib.h: the translator is not initialised), else the translator's door (xbg_game.cpp, [DOS2] VK-NATIVE export). Weak: host tests.
extern int xbg_vkn_cache_dir(char *out, size_t bytes) __attribute__((weak));
extern int vkn_shlib_on(void) __attribute__((weak));
extern int vkn_shlib_cache_dir(char *out, size_t bytes) __attribute__((weak));

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
enum { VEL_MAX = 16, NOTE_Q = 256, WBUCKETS = 1024, DEDUP = 8192, DIR_BYTES = 512, DIR_WAIT_MS = 60000, TICK_MS = 100, REDS = 16 };

// ---- the list record: everything a pipeline is made of, as content that is stable across runs (fixed 232 bytes) ----
enum { F_BLEND = 1, F_DS = 2, F_RASTER = 4, F_VFMT = 8, F_FS = 16 };
typedef struct wrec {
    char key[2][16];            // vertex / fragment program keys (xbg_shader_program_key, 16 hex chars, no NUL; fragment zero = no stage)
    uint64_t retype;            // vertex variant: the retype code ("#vi"), 0 = base module
    float alpha_ref;            // fragment variant: alpha test reference ("#at")
    uint32_t alpha_func;        // NVN compare func of the alpha test, 0 = base module
    uint32_t color[8], depth;   // render pass compatibility: VkFormat per colour slot (0 = not bound), depth VkFormat
    uint8_t prim, stencil_ref, flags, nvel;   // engine primitive (after the rewrite), stencil reference, F_*, vertex elements
    uint8_t blend[6], ds[8], raster[5];        // engine argument tuples (vkn_blend_args / vkn_ds_args / vkn_raster_args)
    uint8_t ncolor, samples, pad0;
    uint8_t write_mask[8];      // VkColorComponentFlags per colour slot
    uint16_t vs_inputs, pad1;   // locations the vertex module reads (checked against the shader at the replay)
    uint8_t vel[6 * VEL_MAX];   // vertex format elements (6 bytes each, the engine's)
    uint8_t pad2[6];
    uint64_t sum;               // fnv of everything above (a torn or foreign record is dropped)
} wrec;
_Static_assert(sizeof(wrec) == 232, "wrec layout");
typedef struct whdr { char magic[8]; uint32_t version, rec_size, vendor, device, driver, pad; uint8_t uuid[16]; } whdr;
static const char k_magic[8] = {'V', 'K', 'N', 'P', 'L', 'S', 'T', 0};
enum { LIST_VERSION = 1 };

// ---- the worker's pipeline table: content key + modules -> pipeline ----
// k[0..2] = vkn_pipe_key with the variant bytes cleared (the draw path numbers its variants itself), k[3] / k[4] = the vertex /
// fragment VkShaderModule (one module per variant: the translator caches them). The shader id is part of k[0], so an entry never
// serves another shader record's draws.
enum { W_BUILDING = 1, W_READY, W_FAILED, W_TAKEN };
typedef struct went { uint64_t k[5]; uint32_t shader; uint8_t state, dead; VkPipeline p; struct went *next; } went;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_go = PTHREAD_COND_INITIALIZER, g_built = PTHREAD_COND_INITIALIZER;
static int g_on = -1;              // -1 not decided, 0 off, 1 on (worker started)
static went *g_w[WBUCKETS];
// the list (worker owns the records; g_mu guards the states, because vkn_warm_forget resets them)
static wrec *g_list; static uint32_t g_nlist;
static uint32_t *g_list_shader;    // shader id the entry is built (or held by the render thread) for; 0 = pending
static uint8_t *g_list_state;      // 0 pending, 1 done for g_list_shader, 2 not buildable here (unusable stage / bad content)
static uint64_t g_dedup[DEDUP]; static uint32_t g_ndedup;
// render thread -> worker
static wrec g_q[NOTE_Q]; static uint32_t g_q_shader[NOTE_Q]; static uint32_t g_nq;
static vkn_warm_stats g_st;
static int g_reds;

static uint64_t rec_sum(const wrec *r) { return vkn_fnv(r, offsetof(wrec, sum)); }
static void wkey(const vkn_pipe_key *k, VkShaderModule vs, VkShaderModule fs, uint64_t out[5]) {
    out[0] = k->w[0] & ~(0xffffull << 24); out[1] = k->w[1]; out[2] = k->w[2];
    out[3] = (uint64_t)(uintptr_t)vs; out[4] = (uint64_t)(uintptr_t)fs;
}
static uint32_t wbucket(const uint64_t k[5]) { return (uint32_t)vkn_fnv(k, 5 * sizeof(uint64_t)) & (WBUCKETS - 1); }
static went *wfind(const uint64_t k[5]) {   // under g_mu
    for (went *e = g_w[wbucket(k)]; e; e = e->next) if (!memcmp(e->k, k, sizeof e->k)) return e;
    return NULL;
}
static went *wadd(const uint64_t k[5], uint32_t shader, uint8_t state) {   // under g_mu; NULL = no memory
    went *e = (went *)calloc(1, sizeof *e);
    if (!e) return NULL;
    memcpy(e->k, k, sizeof e->k); e->shader = shader; e->state = state;
    const uint32_t b = wbucket(k); e->next = g_w[b]; g_w[b] = e;
    return e;
}
static void wremove(went *x) {   // under g_mu
    for (went **pp = &g_w[wbucket(x->k)]; *pp; pp = &(*pp)->next) if (*pp == x) { *pp = x->next; free(x); return; }
}
static void pipe_destroy(void *ctx) { const vkn_dev *d = vkn_device(); if (d) vkDestroyPipeline(d->dev, (VkPipeline)ctx, NULL); }
static int dedup_add(uint64_t h) {   // under g_mu; 1 = new
    if (!h) h = 1;
    for (uint32_t i = (uint32_t)h & (DEDUP - 1), n = 0; n < DEDUP; i = (i + 1) & (DEDUP - 1), n++) {
        if (g_dedup[i] == h) return 0;
        if (!g_dedup[i]) { if (g_ndedup >= DEDUP * 3 / 4) return 0; g_dedup[i] = h; g_ndedup++; return 1; }
    }
    return 0;
}

int vkn_warm_on(void) { return __atomic_load_n(&g_on, __ATOMIC_ACQUIRE) > 0; }
int vkn_warm_owns_cache(void) { return vkn_warm_on(); }

// ---- render thread: take / note ----
int vkn_warm_take(const vkn_pipe_key *k, const vkn_pipe_src *src, VkPipeline *out) {
    if (!vkn_warm_on() || !k || !src || !out) return 0;
    uint64_t wk[5]; wkey(k, src->vs, src->fs, wk);
    int rc = 0;
    pthread_mutex_lock(&g_mu);
    went *e = wfind(wk);
    if (e && e->state == W_BUILDING) {   // the worker is building exactly this one: wait for it rather than build a second copy
        const uint64_t t0 = vkn_now_ns();
        g_st.waits++;
        while ((e = wfind(wk)) && e->state == W_BUILDING) pthread_cond_wait(&g_built, &g_mu);
        g_st.wait_ns += vkn_now_ns() - t0;
    }
    if (e && e->state == W_READY && e->p) { *out = e->p; e->p = VK_NULL_HANDLE; e->state = W_TAKEN; g_st.taken++; rc = 1; }
    pthread_mutex_unlock(&g_mu);
    return rc;
}
// Fills a record from a build. 0 = listable.
static int rec_make(const vkn_pipe_desc *d, const vkn_pipe_src *src, wrec *r) {
    const vkn_shader *s = (const vkn_shader *)src->shader;
    if (!s || !s->key[VKN_SRC_VS][0]) return -1;
    memset(r, 0, sizeof *r);
    memcpy(r->key[0], s->key[VKN_SRC_VS], 16);
    if (src->fs && s->key[VKN_SRC_FS][0]) { memcpy(r->key[1], s->key[VKN_SRC_FS], 16); r->flags |= F_FS; }
    r->retype = src->retype; r->alpha_func = src->alpha_func; r->alpha_ref = src->alpha_func ? src->alpha_ref : 0.0f;
    if (d->blend) { if (vkn_blend_args(d->blend, r->blend)) return -1; r->flags |= F_BLEND; }
    if (d->ds) { if (vkn_ds_args(d->ds, r->ds)) return -1; r->flags |= F_DS; }
    if (d->raster) { if (vkn_raster_args(d->raster, r->raster)) return -1; r->flags |= F_RASTER; }
    if (d->vfmt) {
        uint32_t n = 0; const uint8_t *el = vkn_vfmt_elems(d->vfmt, &n);
        if (!el || n > VEL_MAX) return -1;
        memcpy(r->vel, el, 6u * n); r->nvel = (uint8_t)n; r->flags |= F_VFMT;
    }
    if (vkn_rp_compat_info(d->pass, r->color, &r->ncolor, &r->depth, &r->samples)) return -1;
    r->prim = d->prim; r->stencil_ref = d->stencil_ref; memcpy(r->write_mask, d->write_mask, 8); r->vs_inputs = src->vs_inputs;
    r->sum = rec_sum(r);
    return 0;
}
void vkn_warm_note(const vkn_pipe_desc *d, const vkn_pipe_src *src) {
    if (!vkn_warm_on() || !d || !src) return;
    wrec r; const int ok = !rec_make(d, src, &r);
    uint64_t wk[5]; const vkn_pipe_key k = vkn_pipe_key_pack(d); wkey(&k, src->vs, src->fs, wk);
    pthread_mutex_lock(&g_mu);
    // The cache table owns this content now: the worker must not build it again in this run.
    if (!wfind(wk)) wadd(wk, d->shader, W_TAKEN);
    if (!ok) g_st.not_listable++;
    else if (g_nq < NOTE_Q) { g_q[g_nq] = r; g_q_shader[g_nq] = d->shader; g_nq++; pthread_cond_signal(&g_go); }
    else g_st.queue_lost++;
    pthread_mutex_unlock(&g_mu);
}
void vkn_warm_forget(uint32_t shader) {
    if (!vkn_warm_on()) return;
    pthread_mutex_lock(&g_mu);
    for (uint32_t b = 0; b < WBUCKETS; b++)
        for (went **pp = &g_w[b]; *pp;) {
            went *e = *pp;
            if (e->shader != shader) { pp = &e->next; continue; }
            if (e->state == W_BUILDING) { e->dead = 1; pp = &e->next; continue; }   // the worker drops it when its build returns
            if (e->state == W_READY && e->p) vkn_retire(pipe_destroy, (void *)(uintptr_t)e->p);
            *pp = e->next; free(e);
        }
    for (uint32_t i = 0; i < g_nlist; i++)
        if (g_list_shader[i] == shader && g_list_state[i] == 1) { g_list_shader[i] = 0; g_list_state[i] = 0; }   // a new record of these keys gets it again
    pthread_cond_signal(&g_go);
    pthread_mutex_unlock(&g_mu);
}

// ---- files ----
static char g_dir[DIR_BYTES]; static int g_files;   // worker only
static void path_of(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", g_dir, name); }
static int get_dir(void) {
    char d[DIR_BYTES];
    int (*door)(char *, size_t) = vkn_shlib_on && vkn_shlib_on() ? vkn_shlib_cache_dir : xbg_vkn_cache_dir;
    if (door && !door(d, sizeof d) && d[0]) { snprintf(g_dir, sizeof g_dir, "%s", d); return 0; }
    if (!door) { const char *e = getenv("DM_FILES_DIR"); if (e && *e) { snprintf(g_dir, sizeof g_dir, "%s", e); return 0; } }
    return -1;
}
static uint8_t *read_all(const char *path, size_t *n, size_t max) {
    *n = 0;
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    uint8_t *buf = NULL;
    if (!fseek(f, 0, SEEK_END)) {
        const long sz = ftell(f);
        if (sz > 0 && (size_t)sz <= max && !fseek(f, 0, SEEK_SET) && (buf = (uint8_t *)malloc((size_t)sz))) {
            if (fread(buf, 1, (size_t)sz, f) == (size_t)sz) *n = (size_t)sz; else { free(buf); buf = NULL; }
        }
    }
    fclose(f);
    return buf;
}
static int write_atomic(const char *path, const void *a, size_t na, const void *b, size_t nb) {
    char tmp[DIR_BYTES + 64]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); if (!f) return -1;
    int ok = (!na || fwrite(a, 1, na, f) == na) && (!nb || fwrite(b, 1, nb, f) == nb);
    ok = !fclose(f) && ok;
    if (ok) ok = !rename(tmp, path);
    if (!ok) remove(tmp);
    return ok ? 0 : -1;
}
// The driver cache, checked like the translator's load_pipeline_cache: header version 1, vendor, device, pipelineCacheUUID.
static void cache_load(void) {
    const vkn_dev *d = vkn_device();
    const char *e = getenv("DM_GFX_PIPE_CACHE_LOAD");
    size_t n = 0; uint8_t *data = NULL;
    if (g_files && !(e && *e == '0')) {
        char p[DIR_BYTES + 64]; path_of(p, sizeof p, "vkn-pipeline-cache.bin");
        data = read_all(p, &n, 256u << 20);
        if (data && n >= 32) {
            uint32_t h[4]; memcpy(h, data, 16);
            if (h[0] < 32 || h[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || h[2] != d->props.vendorID || h[3] != d->props.deviceID ||
                memcmp(data + 16, d->props.pipelineCacheUUID, VK_UUID_SIZE)) { vkn_logf("warm: %s is of another device or driver: not loaded", p); n = 0; }
        } else n = 0;
    }
    const int rc = vkn_pipe_cache_init(n ? data : NULL, n);
    free(data);
    pthread_mutex_lock(&g_mu);
    g_st.cache_loaded = rc == 0 ? n : 0; g_st.cache_state = rc == 0 && n ? 1 : 2;
    pthread_mutex_unlock(&g_mu);
    vkn_logf("warm: pipeline cache %s (%zu bytes)%s", rc == 0 && n ? "loaded" : "empty", rc == 0 ? n : 0, rc == 1 ? ", created before the worker (no file data)" : rc < 0 ? " - creation failed" : "");
}
static void hdr_fill(whdr *h) {
    const vkn_dev *d = vkn_device();
    memset(h, 0, sizeof *h); memcpy(h->magic, k_magic, 8); h->version = LIST_VERSION; h->rec_size = sizeof(wrec);
    h->vendor = d->props.vendorID; h->device = d->props.deviceID; h->driver = d->props.driverVersion; memcpy(h->uuid, d->props.pipelineCacheUUID, 16);
}
static int list_add(const wrec *r, uint32_t shader, uint8_t state) {   // under g_mu; 1 = new entry
    if (!dedup_add(rec_sum(r))) return 0;
    if (g_nlist >= VKN_WARM_MAX) { g_st.dropped_full++; return 0; }
    g_list[g_nlist] = *r; g_list_shader[g_nlist] = shader; g_list_state[g_nlist] = state; g_nlist++;
    return 1;
}
static void list_load(void) {
    g_list = (wrec *)calloc(VKN_WARM_MAX, sizeof *g_list); g_list_shader = (uint32_t *)calloc(VKN_WARM_MAX, 4); g_list_state = (uint8_t *)calloc(VKN_WARM_MAX, 1);
    if (!g_list || !g_list_shader || !g_list_state) { vkn_logf(RED "warm: no memory for the list: replay off"); g_files = 0; return; }
    if (!g_files) return;
    char p[DIR_BYTES + 64]; path_of(p, sizeof p, "vkn-pipeline-list.bin");
    size_t n = 0; uint8_t *data = read_all(p, &n, sizeof(whdr) + (size_t)VKN_WARM_MAX * sizeof(wrec) * 2);
    whdr want; hdr_fill(&want);
    int rewrite = 1; uint32_t bad = 0;
    if (data && n >= sizeof(whdr) && !memcmp(data, &want, sizeof want)) {
        const size_t nrec = (n - sizeof(whdr)) / sizeof(wrec);
        pthread_mutex_lock(&g_mu);
        for (size_t i = 0; i < nrec; i++) {
            wrec r; memcpy(&r, data + sizeof(whdr) + i * sizeof(wrec), sizeof r);
            if (r.sum != rec_sum(&r) || r.nvel > VEL_MAX || r.ncolor > 8) { bad++; continue; }
            if (list_add(&r, 0, 0)) g_st.loaded++;
        }
        g_st.dropped_bad += bad;
        pthread_mutex_unlock(&g_mu);
        rewrite = bad || (n - sizeof(whdr)) % sizeof(wrec);   // a torn tail would misalign every later append
    } else if (data && n) vkn_logf("warm: %s is of another device / driver / version: started anew", p);
    free(data);
    if (rewrite) {   // header + the good records, atomically
        pthread_mutex_lock(&g_mu);
        const uint32_t cnt = g_nlist; wrec *copy = cnt ? (wrec *)malloc(cnt * sizeof(wrec)) : NULL;
        if (copy) memcpy(copy, g_list, cnt * sizeof(wrec));
        pthread_mutex_unlock(&g_mu);
        if (write_atomic(p, &want, sizeof want, copy, copy ? cnt * sizeof(wrec) : 0)) { vkn_logf(RED "warm: cannot write %s: the list is not kept", p); g_files = 0; }
        free(copy);
    }
    vkn_logf("warm: pipeline list %s: %u entries loaded (%u bad)", p, g_st.loaded, bad);
}
// The render thread's notes: deduplicated, appended to the file, entered as "done" for their shader (the cache table has them).
static void notes_drain(void) {
    wrec q[NOTE_Q]; uint32_t n, add = 0;
    pthread_mutex_lock(&g_mu);
    n = g_nq;
    for (uint32_t i = 0; i < n; i++) if (list_add(&g_q[i], g_q_shader[i], 1)) q[add++] = g_q[i];
    g_nq = 0; g_st.appended += add;
    pthread_mutex_unlock(&g_mu);
    if (!add || !g_files) return;
    char p[DIR_BYTES + 64]; path_of(p, sizeof p, "vkn-pipeline-list.bin");
    FILE *f = fopen(p, "ab");
    const int ok = f && fwrite(q, sizeof(wrec), add, f) == add;
    if (f && fclose(f)) { if (ok) vkn_logf(RED "warm: closing %s failed", p); }
    if (!ok && g_reds++ < REDS) vkn_logf(RED "warm: appending %u entries to %s failed", add, p);
}
static uint64_t g_saved_compiles; static uint64_t g_save_frame;
static void cache_save(int force) {
    const uint64_t c = vkn_pipe_compiles(), fr = vkn_frame();
    if (c == g_saved_compiles || (!force && fr < g_save_frame + VKN_WARM_SAVE_FRAMES)) return;
    const VkPipelineCache pc = vkn_pipe_cache_get(); const vkn_dev *d = vkn_device();
    if (!pc || !d || !g_files) return;
    g_saved_compiles = c; g_save_frame = fr;
    const uint64_t t0 = vkn_now_ns();
    size_t n = 0; uint8_t *data = NULL; int ok = 0;
    if (vkGetPipelineCacheData(d->dev, pc, &n, NULL) == VK_SUCCESS && n && (data = (uint8_t *)malloc(n))) {
        if (vkGetPipelineCacheData(d->dev, pc, &n, data) == VK_SUCCESS) {   // VK_INCOMPLETE (it grew meanwhile): next time
            char p[DIR_BYTES + 64]; path_of(p, sizeof p, "vkn-pipeline-cache.bin");
            ok = !write_atomic(p, data, n, NULL, 0);
        }
    }
    free(data);
    pthread_mutex_lock(&g_mu);
    if (ok) { g_st.saves++; g_st.cache_saved = n; } else g_st.save_fail++;
    pthread_mutex_unlock(&g_mu);
    vkn_logf("warm: pipeline cache save %zu bytes, %s, %.1f ms (frame %llu, %llu compiles)", n, ok ? "ok" : "FAILED", (vkn_now_ns() - t0) / 1e6, (unsigned long long)fr, (unsigned long long)c);
}

// ---- the replay ----
typedef struct { const vkn_shader **s; uint32_t n, cap; } shv;
static int shv_add(const vkn_shader *s, void *ctx) {   // under vkn_shader's mutex: no calls out
    shv *v = (shv *)ctx;
    if (v->n == v->cap) { const uint32_t c = v->cap ? v->cap * 2 : 1024; const vkn_shader **x = (const vkn_shader **)realloc(v->s, c * sizeof *x); if (!x) return 1; v->s = x; v->cap = c; }
    v->s[v->n++] = s;
    return 0;
}
static int keys_eq(const vkn_shader *s, const wrec *r) {
    if (strlen(s->key[VKN_SRC_VS]) != 16 || memcmp(s->key[VKN_SRC_VS], r->key[0], 16)) return 0;
    if (r->flags & F_FS) return strlen(s->key[VKN_SRC_FS]) == 16 && !memcmp(s->key[VKN_SRC_FS], r->key[1], 16);
    return !s->key[VKN_SRC_FS][0];   // a vertex-only entry matches only a shader without a fragment stage (the draw's modules differ otherwise)
}
// Builds entry i for shader s (worker). 1 = built (or known), 0 = not now, -1 = never for this record.
static int replay_one(uint32_t i, const vkn_shader *s, uint32_t gen) {
    const wrec r = g_list[i];
    if ((r.flags & F_FS) && !(s->stages & (1u << VKN_SRC_FS))) return -1;
    vkn_pipe_desc d; memset(&d, 0, sizeof d);
    d.shader = s->id;
    if (r.flags & F_BLEND) d.blend = vkn_blend_put(0, r.blend[0], r.blend[1], r.blend[2], r.blend[3], r.blend[4], r.blend[5]);
    if (r.flags & F_DS) d.ds = vkn_ds_put(0, r.ds[0], r.ds[1], r.ds[2], r.ds[3], r.ds[4], r.ds[5], r.ds[6], r.ds[7]);
    if (r.flags & F_RASTER) d.raster = vkn_raster_put(0, r.raster[0], r.raster[1], r.raster[2], r.raster[3], r.raster[4]);
    if (r.flags & F_VFMT) d.vfmt = vkn_vfmt_put(0, r.vel, r.nvel);
    if (((r.flags & F_BLEND) && !d.blend) || ((r.flags & F_DS) && !d.ds) || ((r.flags & F_RASTER) && !d.raster) || ((r.flags & F_VFMT) && !d.vfmt)) return -1;
    d.prim = r.prim; d.stencil_ref = r.stencil_ref; memcpy(d.write_mask, r.write_mask, 8);
    d.pass = vkn_rp_compat_make(r.color, r.ncolor, r.depth, r.samples);
    if (!d.pass) return -1;
    vkn_pipe_src src; memset(&src, 0, sizeof src);
    // The same module calls as the draw path's miss (vkn_draw.c prepare): base / "#vi" vertex, base / "#at" fragment.
    src.vs = vkn_shader_module(s, VKN_STAGE_VS, 0, 0.0f, r.retype);
    src.fs = (r.flags & F_FS) ? vkn_shader_module(s, VKN_STAGE_FS, r.alpha_func, r.alpha_ref, 0) : VK_NULL_HANDLE;
    if (!src.vs || ((r.flags & F_FS) && !src.fs) || !s->layout) return -1;   // a missing / unusable stage: the draw is dropped anyway
    src.layout = s->layout;
    for (uint32_t k = 0; k < s->ninputs && k < 16; k++) if (s->inputs[k] < VKN_VFMT_LOCS) src.vs_inputs |= (uint16_t)(1u << s->inputs[k]);
    const vkn_pipe_key key = vkn_pipe_key_pack(&d);
    uint64_t wk[5]; wkey(&key, src.vs, src.fs, wk);
    pthread_mutex_lock(&g_mu);
    if (src.vs_inputs != r.vs_inputs) g_st.mismatched++;   // the record's shader read other inputs: built with the current ones
    went *e = wfind(wk);
    if (e) { pthread_mutex_unlock(&g_mu); return 1; }      // the render thread has it, or an equal entry was built
    e = wadd(wk, s->id, W_BUILDING);
    pthread_mutex_unlock(&g_mu);
    if (!e) return 0;
    uint64_t ns = 0;
    VkPipeline p = vkn_pipe_build(&d, &src, &ns);
    // A shader died while this ran: if it was this record (its id may be reused), the pipeline must not be offered.
    int stale = 0;
    if (vkn_pipe_forget_gen() != gen) { const vkn_shader *now = vkn_shader_find_keys(s->key[VKN_SRC_VS], s->key[VKN_SRC_FS]); stale = now != s || now->id != d.shader; }
    pthread_mutex_lock(&g_mu);
    g_st.build_ns += ns;
    if (e->dead || stale) {
        if (p) vkn_retire(pipe_destroy, (void *)(uintptr_t)p);
        wremove(e); g_st.stale++; p = VK_NULL_HANDLE;
        pthread_cond_broadcast(&g_built); pthread_mutex_unlock(&g_mu);
        return 0;
    }
    e->p = p; e->state = p ? W_READY : W_FAILED;
    if (p) g_st.built++; else g_st.failed++;
    pthread_cond_broadcast(&g_built);
    pthread_mutex_unlock(&g_mu);
    return 1;
}
static uint64_t g_epoch_seen = ~0ull; static uint32_t g_gen_seen = ~0u, g_nlist_seen;
static void replay(void) {
    const uint64_t ep = vkn_shader_epoch(); const uint32_t gen = vkn_pipe_forget_gen();
    pthread_mutex_lock(&g_mu);
    uint32_t pending = 0;
    for (uint32_t i = 0; i < g_nlist; i++) pending += g_list_state[i] == 0;
    g_st.pending = pending; const uint32_t nl = g_nlist;
    pthread_mutex_unlock(&g_mu);
    if (!pending || (ep == g_epoch_seen && gen == g_gen_seen && nl == g_nlist_seen)) return;   // nothing new to match
    g_epoch_seen = ep; g_gen_seen = gen; g_nlist_seen = nl;
    shv v = {NULL, 0, 0};
    vkn_shader_each(shv_add, &v);
    for (uint32_t i = 0; i < nl; i++) {
        pthread_mutex_lock(&g_mu); const int pend = g_list_state[i] == 0; pthread_mutex_unlock(&g_mu);
        if (!pend) continue;
        const vkn_shader *s = NULL;
        for (uint32_t j = 0; j < v.n && !s; j++) if (keys_eq(v.s[j], &g_list[i]) && v.s[j]->id) s = v.s[j];
        if (!s) continue;   // the engine has not created this shader (yet): another round
        const int rc = replay_one(i, s, gen);
        pthread_mutex_lock(&g_mu);
        if (rc > 0 && g_list_state[i] == 0) { g_list_state[i] = 1; g_list_shader[i] = s->id; g_st.replayed++; }
        else if (rc < 0) { g_list_state[i] = 2; g_st.skipped++; }
        pthread_mutex_unlock(&g_mu);
        if (rc == 0) g_gen_seen = ~0u;   // retry in the next round
        pthread_mutex_lock(&g_mu); const uint32_t nq = g_nq; pthread_mutex_unlock(&g_mu);
        if (nq) notes_drain();   // the render thread's notes first: they mark contents it already has
    }
    free(v.s);
}

static void *worker_main(void *unused) {
    (void)unused;
    setpriority(PRIO_PROCESS, 0, 10);   // Linux: this thread only (the translator's cache saver does the same)
    uint64_t waited = 0; int ready = 0;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        if (!g_nq) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += TICK_MS * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&g_go, &g_mu, &ts);
        }
        pthread_mutex_unlock(&g_mu);
        if (!ready) {   // the device and the translator's directory (both exist before the first frame; polled)
            if (!vkn_device()) continue;
            if (!get_dir()) g_files = 1;
            else if ((waited += TICK_MS) < DIR_WAIT_MS) continue;
            else vkn_logf(RED "warm: no directory for the pipeline files after %d s: an empty cache, no list", DIR_WAIT_MS / 1000);
            cache_load(); list_load(); ready = 1;
        }
        notes_drain();
        replay();
        cache_save(0);
    }
    return NULL;
}

int vkn_warm_start(void) {
    if (__atomic_load_n(&g_on, __ATOMIC_ACQUIRE) >= 0) return vkn_warm_on();
    const char *e = getenv("DM_VK_NATIVE_WARM");
    int on = vkn_mode() == VKN_NATIVE && !(e && *e && atoi(e) == 0);
    if (on) {
        pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8u << 20); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        __atomic_store_n(&g_on, 1, __ATOMIC_RELEASE);   // before the thread: vkn_state must not create the cache meanwhile
        if (pthread_create(&t, &a, worker_main, NULL)) { on = 0; vkn_logf(RED "warm: no worker thread: warm-up off"); }
        pthread_attr_destroy(&a);
    }
    __atomic_store_n(&g_on, on, __ATOMIC_RELEASE);
    if (!on) { pthread_mutex_lock(&g_mu); g_st.cache_state = -1; pthread_mutex_unlock(&g_mu); }
    vkn_logf("warm: DM_VK_NATIVE_WARM=%d (%s)", on, on ? "driver cache file, pipeline list, replay worker" : "pipelines built at their first draw, nothing kept");
    return on;
}

void vkn_warm_stats_get(vkn_warm_stats *s) {
    pthread_mutex_lock(&g_mu); *s = g_st; s->listed = g_nlist; pthread_mutex_unlock(&g_mu);
}
void vkn_warm_census(void) {
    vkn_warm_stats w; vkn_warm_stats_get(&w);
    vkn_state_stats st; vkn_state_stats_get(&st);
    vkn_state_wstats ws; vkn_state_wstats_get(&ws);
    vkn_logf("warm: list %u (loaded %u, new %u, full %u, bad %u, not listable %u, notes lost %u); replay: %u replayed (%u built, %u failed, %.1f ms), %u waiting for "
             "their shader, %u skipped, %u stale, %u inputs differ; render thread: %llu from the worker (%llu waits, %.1f ms), %llu built itself (%.1f ms), %llu stale; "
             "cache %s: loaded %llu bytes, saved %llu bytes (%llu saves, %llu failed), %llu compiles",
             w.listed, w.loaded, w.appended, w.dropped_full, w.dropped_bad, w.not_listable, w.queue_lost, w.replayed, w.built, w.failed, w.build_ns / 1e6, w.pending,
             w.skipped, w.stale, w.mismatched, (unsigned long long)ws.warm_hits, (unsigned long long)w.waits, w.wait_ns / 1e6, (unsigned long long)st.misses, st.build_ns / 1e6,
             (unsigned long long)ws.stale_builds, w.cache_state == 1 ? "from file" : w.cache_state == 2 ? "new" : w.cache_state == 0 ? "not yet" : "off",
             (unsigned long long)w.cache_loaded, (unsigned long long)w.cache_saved, (unsigned long long)w.saves, (unsigned long long)w.save_fail, (unsigned long long)ws.compiles);
    if ((w.dropped_full || w.queue_lost || w.save_fail) && g_reds++ < REDS)
        vkn_logf(RED "warm: list full %u, notes lost %u, cache saves failed %llu", w.dropped_full, w.queue_lost, (unsigned long long)w.save_fail);
}
#endif
