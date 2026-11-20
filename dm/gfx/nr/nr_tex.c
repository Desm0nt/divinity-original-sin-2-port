// [DOS2] NR-05: material textures by engine events, the pure core (see nr_tex.h). Host-testable: tests/nr_tex_test.c.
#include "nr_tex.h"
#include "../tex/nvn_deswizzle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- pure helpers ----
static uint64_t nrt_mix(uint64_t h, uint64_t w) { h ^= w; h *= 0xff51afd7ed558ccdull; return h ^ (h >> 32); }
uint64_t nr_tex_probe(const void *mem, uint64_t size) {
    const uint8_t *p = (const uint8_t *)mem;
    if (!p || !size) return 0;
    uint64_t h = 0x9e3779b97f4a7c15ull ^ size, w;
    if (size <= 1024) {
        uint64_t i = 0;
        for (; i + 8 <= size; i += 8) { memcpy(&w, p + i, 8); h = nrt_mix(h, w); }
        for (; i < size; i++) h = nrt_mix(h, p[i]);
    } else {
        for (uint64_t k = 0; k <= 32; k++) {   // 33 samples of 32 bytes: the first, 31 evenly spaced, the last
            const uint8_t *s = p + (size - 32) * k / 32;
            for (int j = 0; j < 4; j++) { memcpy(&w, s + 8 * j, 8); h = nrt_mix(h, w); }
        }
    }
    return h ? h : 1;
}
uint64_t nr_tex_hash(const void *mem, uint64_t n) {   // the 4-lane word mix of xbg_game.cpp hash_memory
    const uint8_t *p = (const uint8_t *)mem;
    uint64_t h[4] = {1469598103934665603ull, 0x9e3779b97f4a7c15ull, 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull};
    size_t i = 0;
    if (!p) return 0;
    for (; i + 32 <= n; i += 32) for (unsigned k = 0; k < 4; k++) { uint64_t w; memcpy(&w, p + i + 8 * k, 8); h[k] = (h[k] ^ w) * 0x100000001b3ull; h[k] ^= h[k] >> 29; }
    uint64_t hash = h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7) ^ n;
    for (; i < n; i++) { hash ^= p[i]; hash *= 1099511628211ull; }
    return hash;
}
int nr_tex_layout(const nvn_texdesc *d, const nvn_texlayout *L, nrt_sub *subs, int cap, uint64_t *total) {
    int bw = 1, bh = 1;
    if (!d || !L || (d->samples > 1) || (d->flags & 0x10) || d->target < 0 || d->target > 9 || d->target == 5 || d->target == 6) return -1;   // 5 / 6: multisample
    if (!nvn_fmt_bpb(d->format, &bw, &bh) || !L->bpb || !L->layers || !d->levels || d->levels > 16 || !d->width || !L->size) return -1;
    if (bw < 1) bw = 1;
    if (bh < 1) bh = 1;
    const uint32_t width = d->width, height = d->target == 0 || d->target == 3 ? 1 : d->height, depth = d->target == 2 ? d->depth : 1;
    if (!height || !depth) return -1;
    uint64_t t = 0; int n = 0;
    for (uint32_t layer = 0; layer < L->layers; layer++)
        for (uint32_t level = 0; level < d->levels; level++) {
            if (n >= cap) return -1;
            const uint32_t w = width >> level ? width >> level : 1, h = height >> level ? height >> level : 1, z = depth >> level ? depth >> level : 1;
            const uint64_t bx = (w + (uint32_t)bw - 1) / (uint32_t)bw, by = (h + (uint32_t)bh - 1) / (uint32_t)bh, begin = (t + 15) & ~(uint64_t)15, pitch = bx * L->bpb;
            subs[n] = (nrt_sub){.off = begin, .bytes = pitch * by * z, .pitch = (uint32_t)pitch, .level = level, .layer = layer, .w = w, .h = h, .depth = z};
            t = begin + pitch * by * z; n++;
        }
    if (total) *total = t;
    return n;
}
int nr_tex_cmp_staged(const nvn_texdesc *d, const nvn_texlayout *L, const uint8_t *a, uint64_t a_cap, const uint8_t *b, uint64_t bn, int *at, uint64_t *off) {
    nrt_sub sub[NRT_SUBS]; uint64_t total = 0;
    const int n = nr_tex_layout(d, L, sub, NRT_SUBS, &total);
    if (at) *at = -1;
    if (off) *off = 0;
    if (n <= 0 || !a || !b || bn != total || a_cap < total) return NRT_CMP_LAYOUT;
    for (int i = 0; i < n; i++)
        if (memcmp(a + sub[i].off, b + sub[i].off, (size_t)sub[i].bytes)) {
            uint64_t k = 0; while (k < sub[i].bytes && a[sub[i].off + k] == b[sub[i].off + k]) k++;
            if (at) *at = i;
            if (off) *off = sub[i].off + k;
            return NRT_CMP_DIFF;
        }
    return memcmp(a, b, (size_t)total) ? NRT_CMP_PAD : NRT_CMP_SAME;
}
int nr_tex_regions_match(const nvn_texdesc *d, const nvn_texlayout *L, const nrt_region *r, int n, int *at) {
    nrt_sub sub[NRT_SUBS]; uint64_t total = 0;
    const int k = nr_tex_layout(d, L, sub, NRT_SUBS, &total);
    if (at) *at = -1;
    if (k <= 0 || !r || n != k) { if (at) *at = n < k ? n : k; return 0; }
    for (int i = 0; i < n; i++)
        if (r[i].off != sub[i].off || r[i].row_len || r[i].img_h || r[i].level != sub[i].level || r[i].layer != sub[i].layer || r[i].layers != 1 || r[i].x || r[i].y || r[i].z ||
            r[i].w != sub[i].w || r[i].h != sub[i].h || r[i].depth != sub[i].depth) { if (at) *at = i; return 0; }
    return 1;
}
int nr_tex_astc_cacheable(const nvn_texdesc *d, const nvn_texlayout *L, const void *mem) {
    return d->format >= 0x41 && d->format <= 0x48 && d->target == 1 && mem && L->layers == 1 && L->size && L->size <= UINT32_MAX && !(d->flags & 0x10) &&
           d->width <= 65535 && d->height <= 65535;
}
const char *nr_tex_why_name(int why) {
    static const char *const n[NRT_WHY_N] = {"ok", "unknown", "pending", "none", "stale", "expired", "small", "big", "format", "queue", "layout", "failed", "changing", "nomem"};
    return why >= 0 && why < NRT_WHY_N ? n[why] : "?";
}

// ---- the table ----
static nrt_entry *slot(const nr_tex_core *c, uint64_t serial) { return &c->e[serial & (NRT_CAP - 1)]; }
static nrt_entry *entry(const nr_tex_core *c, uint64_t serial) { nrt_entry *e = slot(c, serial); return serial && e->serial == serial ? e : NULL; }
static void release(nr_tex_core *c, uint64_t bytes) { c->st.live_bytes = c->st.live_bytes >= bytes ? c->st.live_bytes - bytes : 0; }
static void free_blob(nr_tex_core *c, nrt_entry *e) {
    if (e->ts) {   // NR-22: the engine's prepared object (its staging is e->blob): back to the engine, which destroys it off this lock
        if (c->sink.release) c->sink.release(c->sink.ctx, e->ts);
        release(c, e->blob_bytes); c->st.ts_released++;
    } else if (e->blob) { free(e->blob); release(c, e->blob_bytes); }
    e->blob = NULL; e->blob_bytes = 0; e->ts = NULL; e->ts_kind = NRT_TS_NONE;
}
static void job_free(nrt_job *j) {   // a job's result that goes nowhere (any thread: the sink's release only queues)
    if (j->ts) { if (j->sink.release) j->sink.release(j->sink.ctx, j->ts); }
    else free(j->blob);
    j->ts = NULL; j->blob = NULL; j->ts_kind = NRT_TS_NONE;
}
static nrt_omap *oslot(const nr_tex_core *c, uint64_t obj) { uint64_t h = obj * 0x9e3779b97f4a7c15ull; return &c->omap[(h >> 40) & (NRT_OMAP - 1)]; }
static int bit(const uint8_t *m, unsigned f) { return f < 256 && (m[f >> 3] >> (f & 7) & 1); }

nr_tex_core *nr_tex_new(uint64_t budget, uint64_t min_bytes, uint64_t max_bytes) {
    nr_tex_core *c = (nr_tex_core *)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->e = (nrt_entry *)calloc(NRT_CAP, sizeof *c->e); c->q = (nrt_src *)calloc(NRT_QUEUE, sizeof *c->q); c->omap = (nrt_omap *)calloc(NRT_OMAP, sizeof *c->omap);
    if (!c->e || !c->q || !c->omap) { nr_tex_free(c); return NULL; }
    c->budget = budget; c->min_bytes = min_bytes; c->max_bytes = max_bytes;
    return c;
}
void nr_tex_free(nr_tex_core *c) {
    if (!c) return;
    if (c->e) for (int i = 0; i < NRT_CAP; i++) { if (c->e[i].ts) { if (c->sink.release) c->sink.release(c->sink.ctx, c->e[i].ts); } else free(c->e[i].blob); }
    free(c->e); free(c->q); free(c->omap); free(c);
}
int nr_tex_create(nr_tex_core *c, const nrt_src *s, uint64_t hash0, uint64_t now, int want_job) {
    if (!s->serial) return NRT_WHY_UNKNOWN;
    nrt_entry *e = slot(c, s->serial);
    if (e->serial && e->serial != s->serial && e->alive) c->st.wrapped++;   // a live older texture 32768 serials back loses its entry (old path)
    free_blob(c, e);   // a job still running for the old serial finds another serial at its end and is discarded
    if (s->obj) { nrt_omap *o = oslot(c, s->obj); if (o->obj && o->obj != s->obj) { nrt_entry *p = entry(c, o->serial); if (p && p->alive && p->obj == o->obj) c->st.omap_lost++; } o->obj = s->obj; o->serial = s->serial; }
    *e = (nrt_entry){.serial = s->serial, .handle = s->handle, .size = s->L.size, .t_create = now, .hash0 = hash0, .mem = s->mem, .alive = 1, .src = s->src, .efmt = s->efmt, .obj = s->obj,
                     .fmt = (uint16_t)(s->d.format & 0xffff), .state = NRT_ST_NONE};
    c->st.events++; c->st.events_src[s->src & 7]++;
    int why = NRT_WHY_OK;
    uint64_t total = 0; nrt_sub sub[NRT_SUBS];
    const int blob_ok = nr_tex_layout(&s->d, &s->L, sub, NRT_SUBS, &total) > 0 && s->mem;
    int want_blob = 0;
    if (!want_job) why = NRT_WHY_NONE;
    else if (!blob_ok) why = NRT_WHY_LAYOUT;
    else if (s->L.size < c->min_bytes) why = NRT_WHY_SMALL;
    else if (total > c->max_bytes || total > c->budget) why = NRT_WHY_BIG;
    else if (bit(c->deny, (unsigned)s->d.format)) why = NRT_WHY_FMT;
    else want_blob = 1;
    const int want_key = want_job && c->want_key && nr_tex_astc_cacheable(&s->d, &s->L, s->mem);
    if (!want_blob && !want_key) { e->why = (uint8_t)why; c->st.why[why]++; return why; }
    if (c->qtail - c->qhead >= NRT_QUEUE) { e->why = NRT_WHY_QUEUE; c->st.why[NRT_WHY_QUEUE]++; return NRT_WHY_QUEUE; }
    c->q[c->qtail++ % NRT_QUEUE] = *s;
    e->state = NRT_ST_QUEUED; e->why = (uint8_t)(want_blob ? NRT_WHY_OK : why);
    c->st.queued++;
    return NRT_WHY_OK;
}
void nr_tex_gone(nr_tex_core *c, uint64_t serial) {
    nrt_entry *e = entry(c, serial);
    if (!e || !e->alive) return;
    if (e->state == NRT_ST_READY) c->st.gone_ready++;
    free_blob(c, e);
    e->alive = 0; e->state = NRT_ST_NONE; e->have_key = 0;
    c->st.gone++;
}
int nr_tex_written(nr_tex_core *c, uint64_t serial) {
    nrt_entry *e = entry(c, serial);
    if (!e || !e->alive) return 0;
    free_blob(c, e);   // READY: the blob is older than the write; QUEUED / WORKING: the job is dropped (its end finds the state changed)
    e->state = NRT_ST_NONE; e->why = NRT_WHY_STALE; e->have_key = 0; e->writes++;
    c->st.written++;
    return 1;
}
int nr_tex_engine_gone(nr_tex_core *c, uint64_t obj) {
    const nrt_omap *o = obj ? oslot(c, obj) : NULL;
    nrt_entry *e = o && o->obj == obj ? entry(c, o->serial) : NULL;
    if (!e || !e->alive || e->obj != obj || e->engine_dead) return 0;
    free_blob(c, e);
    e->engine_dead = 1; e->state = NRT_ST_NONE; e->have_key = 0;
    c->st.engine_gone++;
    return 1;
}
int nr_tex_find(const nr_tex_core *c, uint64_t serial, nrt_info *out) {
    const nrt_entry *e = entry(c, serial);
    if (!e) return 0;
    if (out) *out = (nrt_info){.handle = e->handle, .hash0 = e->hash0, .t_create = e->t_create, .writes = e->writes, .engine_dead = e->engine_dead, .alive = e->alive, .src = e->src, .efmt = e->efmt, .state = e->state,
                               .have_key = e->have_key, .why = e->why};
    return 1;
}
void nr_tex_verdict(nr_tex_core *c, unsigned f, int ok) {
    if (f >= 256) return;
    uint8_t *m = ok ? c->allow : c->deny;
    m[f >> 3] |= (uint8_t)(1u << (f & 7));
}
int nr_tex_job_begin(nr_tex_core *c, nrt_job *j) {
    while (c->qhead != c->qtail) {
        const nrt_src *s = &c->q[c->qhead % NRT_QUEUE];
        nrt_entry *e = entry(c, s->serial);
        if (!e || !e->alive || e->state != NRT_ST_QUEUED) { c->qhead++; continue; }   // gone, replaced or cancelled meanwhile
        j->s = *s;
        j->want_blob = e->why == NRT_WHY_OK && !bit(c->deny, (unsigned)s->d.format);
        j->want_key = c->want_key && nr_tex_astc_cacheable(&s->d, &s->L, s->mem);
        j->nsub = 0; j->bytes = 0;
        if (j->want_blob) {
            j->nsub = nr_tex_layout(&s->d, &s->L, j->sub, NRT_SUBS, &j->bytes);
            if (j->nsub <= 0) j->want_blob = 0, j->bytes = 0;
        }
        if (!j->want_blob && !j->want_key) { e->state = NRT_ST_NONE; e->why = NRT_WHY_FMT; c->st.why[NRT_WHY_FMT]++; c->qhead++; continue; }
        if (j->want_blob && c->st.live_bytes + j->bytes > c->budget) return 0;   // wait for the translator to take / the worker to expire blobs
        c->qhead++;
        e->state = NRT_ST_WORKING;
        c->st.live_bytes += j->bytes;
        if (c->st.live_bytes > c->st.peak_bytes) c->st.peak_bytes = c->st.live_bytes;
        j->blob = NULL; j->ok = j->have_key = 0; j->why = NRT_WHY_OK; j->probe = 0; j->ns = 0;
        j->sink = c->sink; j->ts = NULL; j->ts_kind = NRT_TS_NONE; j->ts_bytes = 0; j->ts_refused = 0; j->fmt_ok = (uint8_t)bit(c->allow, (unsigned)s->d.format);
        return 1;
    }
    return 0;
}
void nr_tex_job_run(nrt_job *j, nrt_key_fn key) {
    const nrt_src *s = &j->s;
    const uint64_t p0 = nr_tex_probe(s->mem, s->L.size);
    if (j->want_blob) {
        // NR-22: a format the translator took as it is gets the engine's image + staging (the deswizzle goes straight into the staging);
        // refused (or no sink / format not proven yet) = the heap blob of NR-05; a texture the ASTC cache may hold goes the make_astc way below
        if (j->sink.make && j->fmt_ok && !j->want_key) { j->ts = j->sink.make(j->sink.ctx, s, j->bytes, &j->blob); if (j->ts && j->blob) j->ts_kind = NRT_TS_DESW; else { j->ts = NULL; j->blob = NULL; j->ts_refused = 1; } }
        if (!j->blob) j->blob = (uint8_t *)malloc(j->bytes ? j->bytes : 1);
        if (!j->blob) { j->why = NRT_WHY_NOMEM; return; }
        for (int i = 0; i < j->nsub; i++)
            if (nvn_deswizzle_gob(&s->d, j->sub[i].level, j->sub[i].layer, s->mem, j->blob + j->sub[i].off, j->sub[i].pitch)) { job_free(j); j->why = NRT_WHY_FAILED; return; }
    }
    if (j->want_key && key && !key(s->mem, s->L.size, &j->crc, &j->adler)) j->have_key = 1;
    if (!j->ts && j->have_key && j->sink.make_astc) {   // NR-22: the ASTC cache has it -> the ASTC image with its blocks (a heap blob of it is then useless)
        uint64_t n = 0; void *o = j->sink.make_astc(j->sink.ctx, s, j->crc, j->adler, &n);
        if (o) { free(j->blob); j->blob = NULL; j->ts = o; j->ts_kind = NRT_TS_ASTC; j->ts_bytes = n; }
    }
    if (nr_tex_probe(s->mem, s->L.size) != p0) { job_free(j); j->have_key = 0; j->why = NRT_WHY_CHANGING; return; }   // written while we read
    j->probe = p0; j->ok = 1;
}
void nr_tex_job_end(nr_tex_core *c, nrt_job *j, uint64_t now) {
    nrt_entry *e = entry(c, j->s.serial);
    const int mine = e && e->alive && e->state == NRT_ST_WORKING;
    (void)now;
    if (j->ok) c->st.prepared_ns += j->ns;
    if (j->ok && j->blob) { c->st.prepared++; c->st.prepared_bytes += j->bytes; }
    if (j->ok && j->have_key) c->st.keys++;
    if (j->ts_refused) c->st.ts_refused++;
    if (j->ts) { if (j->ts_kind == NRT_TS_ASTC) c->st.ts_astc++; else c->st.ts_made++; }
    if (!mine || !j->ok) {
        if (!j->ok) c->st.why[j->why]++;
        if (j->ts) c->st.ts_released++;
        job_free(j);
        release(c, j->bytes);
        if (mine) { e->state = NRT_ST_NONE; e->why = j->why; }
        return;
    }
    e->probe = j->probe;
    if (j->have_key) { e->have_key = 1; e->crc = j->crc; e->adler = j->adler; }
    if (j->ts && j->ts_kind == NRT_TS_ASTC) {   // NR-22: no blob; the budget holds the ASTC staging (a heap reservation of this job goes back)
        release(c, j->bytes); c->st.live_bytes += j->ts_bytes; if (c->st.live_bytes > c->st.peak_bytes) c->st.peak_bytes = c->st.live_bytes;
        e->ts = j->ts; e->ts_kind = NRT_TS_ASTC; e->blob = NULL; e->blob_bytes = j->ts_bytes; e->state = NRT_ST_READY;
    } else if (j->blob) { e->blob = j->blob; e->blob_bytes = j->bytes; e->ts = j->ts; e->ts_kind = j->ts ? NRT_TS_DESW : NRT_TS_NONE; e->state = NRT_ST_READY; }
    else { release(c, j->bytes); e->state = NRT_ST_NONE; }
    e->t_create = e->t_create ? e->t_create : now;
    j->blob = NULL; j->ts = NULL; j->ts_kind = NRT_TS_NONE;
}
int nr_tex_take_ts(nr_tex_core *c, uint64_t serial, const void *mem, uint64_t size, uint8_t **blob, void **ts, int *kind, uint64_t *bytes, uint64_t *probe, int *why) {
    nrt_entry *e = entry(c, serial);
    int w = NRT_WHY_OK;
    if (!e || !e->alive) w = NRT_WHY_UNKNOWN;
    else if (e->state == NRT_ST_QUEUED || e->state == NRT_ST_WORKING) { e->state = NRT_ST_NONE; e->why = NRT_WHY_PENDING; w = NRT_WHY_PENDING; }   // too late: cancelled
    else if (e->state != NRT_ST_READY) w = e->taken ? NRT_WHY_NONE : e->why ? e->why : NRT_WHY_NONE;
    else if ((const uint8_t *)mem != e->mem || size != e->size || nr_tex_probe(mem, size) != e->probe) { free_blob(c, e); e->state = NRT_ST_NONE; e->why = NRT_WHY_STALE; e->have_key = 0; w = NRT_WHY_STALE; }
    if (w != NRT_WHY_OK) { c->st.why[w]++; if (why) *why = w; if (blob) *blob = NULL; if (ts) *ts = NULL; if (kind) *kind = NRT_TS_NONE; return 0; }
    if (blob) *blob = e->blob;
    if (ts) *ts = e->ts;
    if (kind) *kind = e->ts_kind;
    if (bytes) *bytes = e->blob_bytes;
    if (probe) *probe = e->probe;
    c->st.taken++; c->st.taken_bytes += e->blob_bytes;
    if (e->ts) { c->st.ts_taken++; c->st.ts_taken_bytes += e->blob_bytes; }
    e->blob = NULL; e->blob_bytes = 0; e->ts = NULL; e->ts_kind = NRT_TS_NONE; e->state = NRT_ST_NONE; e->taken = 1;   // the budget is held until nr_tex_drop / nr_tex_unhold
    if (why) *why = NRT_WHY_OK;
    return 1;
}
uint8_t *nr_tex_take(nr_tex_core *c, uint64_t serial, const void *mem, uint64_t size, uint64_t *bytes, uint64_t *probe, int *why) {
    uint8_t *b = NULL; void *ts = NULL; int kind = 0; uint64_t n = 0;
    if (!nr_tex_take_ts(c, serial, mem, size, &b, &ts, &kind, &n, probe, why)) return NULL;
    if (ts) {   // a prepared object this caller cannot use: back to the engine, the old path uploads
        if (c->sink.release) c->sink.release(c->sink.ctx, ts);
        release(c, n); c->st.ts_released++; c->st.why[NRT_WHY_NONE]++;
        if (why) *why = NRT_WHY_NONE;
        return NULL;
    }
    if (bytes) *bytes = n;
    return b;
}
void nr_tex_drop(nr_tex_core *c, uint8_t *blob, uint64_t bytes) { if (!blob) return; free(blob); release(c, bytes); }
void nr_tex_unhold(nr_tex_core *c, uint64_t bytes) { release(c, bytes); }
void nr_tex_set_sink(nr_tex_core *c, const nrt_sink *s) { if (s) c->sink = *s; else memset(&c->sink, 0, sizeof c->sink); }
int nr_tex_key(nr_tex_core *c, uint64_t serial, const void *mem, uint64_t size, uint32_t *crc, uint32_t *adler) {
    nrt_entry *e = entry(c, serial);
    if (!e || !e->alive || !e->have_key) return 0;
    if ((const uint8_t *)mem != e->mem || size != e->size || nr_tex_probe(mem, size) != e->probe) { e->have_key = 0; c->st.key_stale++; return 0; }
    *crc = e->crc; *adler = e->adler; c->st.key_served++;
    return 1;
}
void nr_tex_expire(nr_tex_core *c, uint64_t now, uint64_t max_age) {
    for (int i = 0; i < NRT_CAP; i++) {
        nrt_entry *e = &c->e[i];
        if (e->state != NRT_ST_READY || now < e->t_create || now - e->t_create < max_age) continue;
        c->st.expired++; c->st.expired_bytes += e->blob_bytes;
        free_blob(c, e); e->state = NRT_ST_NONE; e->why = NRT_WHY_EXPIRED;
    }
}
int nr_tex_summary(const nr_tex_core *c, char *out, size_t cap) {
    const nrt_stats *s = &c->st; const double mb = 1048576.;
    int k = snprintf(out, cap, "events %llu (packaged %llu, linear %llu), jobs %llu, prepared %llu (%.1f MB, worker %.1f ms), keys %llu, taken %llu (%.1f MB), keys served %llu / stale %llu, "
                     "expired %llu (%.1f MB), engine destroys %llu, gone %llu (%llu with a blob), WriteTexels into live ones %llu, slot reuse %llu (object map %llu), blobs held %.1f MB (peak %.1f of %.0f MB); no blob:",
                     (unsigned long long)s->events, (unsigned long long)s->events_src[4], (unsigned long long)s->events_src[3], (unsigned long long)s->queued,
                     (unsigned long long)s->prepared, s->prepared_bytes / mb, s->prepared_ns / 1e6, (unsigned long long)s->keys, (unsigned long long)s->taken, s->taken_bytes / mb,
                     (unsigned long long)s->key_served, (unsigned long long)s->key_stale, (unsigned long long)s->expired, s->expired_bytes / mb, (unsigned long long)s->engine_gone, (unsigned long long)s->gone,
                     (unsigned long long)s->gone_ready, (unsigned long long)s->written, (unsigned long long)s->wrapped, (unsigned long long)s->omap_lost, s->live_bytes / mb, s->peak_bytes / mb, c->budget / mb);
    for (int w = 1; w < NRT_WHY_N && k > 0 && (size_t)k < cap; w++)
        if (s->why[w]) k += snprintf(out + k, cap - (size_t)k, " %s %llu", nr_tex_why_name(w), (unsigned long long)s->why[w]);
    if (c->sink.make && k > 0 && (size_t)k < cap)   // NR-22
        k += snprintf(out + k, cap - (size_t)k, "; texstream: prepared images %llu deswizzled + %llu ASTC, refused by the engine %llu, taken %llu (%.1f MB), released untaken %llu",
                      (unsigned long long)s->ts_made, (unsigned long long)s->ts_astc, (unsigned long long)s->ts_refused, (unsigned long long)s->ts_taken, s->ts_taken_bytes / mb, (unsigned long long)s->ts_released);
    return k;
}
