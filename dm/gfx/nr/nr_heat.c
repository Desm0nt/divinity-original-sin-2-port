// [DOS2] NR-HEAT core (nr_heat.h; docs/dos2/nr/NR-HEAT.md). Pure C11.
#include "nr_heat.h"
#include <stdlib.h>
#include <string.h>

// ---------------- level names ----------------
int nrh_intern(nrh_names *t, const char *s, size_t n) {
    if (!n) return -1;
    if (n >= NRH_NAME) n = NRH_NAME - 1;
    for (uint32_t i = 0; i < t->n; i++) if (!strncmp(t->name[i], s, n) && t->name[i][n] == 0) return (int)i;
    if (t->n >= NRH_LEVELS) return -1;
    memcpy(t->name[t->n], s, n); t->name[t->n][n] = 0; nrh_clean(t->name[t->n]);
    return (int)t->n++;
}
static const char *find_sub(const char *s, size_t n, const char *sub) {
    const size_t m = strlen(sub);
    for (size_t i = 0; i + m <= n; i++) if (!memcmp(s + i, sub, m)) return s + i;
    return NULL;
}
int nrh_level_of_path(const char *path, size_t n, const char **lv, size_t *ln) {
    static const char *const tags[2] = {"/Levels/", "/Globals/"};
    for (int k = 0; k < 2; k++) {
        const char *p = find_sub(path, n, tags[k]);
        if (!p) continue;
        p += strlen(tags[k]);
        const char *end = path + n, *q = p;
        while (q < end && *q && *q != '/') q++;
        if (q == p || q >= end || *q != '/') continue;   // the level folder itself must hold the file
        *lv = p; *ln = (size_t)(q - p); return 1;
    }
    return 0;
}

// ---------------- LSPK ----------------
int nrh_lz4(const uint8_t *s, size_t sn, uint8_t *d, size_t dn) {
    size_t i = 0, o = 0;
    while (i < sn) {
        const uint8_t tok = s[i++];
        size_t lit = tok >> 4;
        if (lit == 15) { uint8_t b; do { if (i >= sn) return -1; b = s[i++]; lit += b; } while (b == 255); }
        if (lit > sn - i || lit > dn - o) return -1;
        memcpy(d + o, s + i, lit); i += lit; o += lit;
        if (i >= sn) break;   // the last sequence has literals only
        if (sn - i < 2) return -1;
        const size_t off = (size_t)s[i] | (size_t)s[i + 1] << 8; i += 2;
        if (!off || off > o) return -1;
        size_t ml = (size_t)(tok & 15) + 4;
        if ((tok & 15) == 15) { uint8_t b; do { if (i >= sn) return -1; b = s[i++]; ml += b; } while (b == 255); }
        if (ml > dn - o) return -1;
        for (size_t k = 0; k < ml; k++) { d[o] = d[o - off]; o++; }   // byte by byte: overlapping copies repeat the pattern
    }
    return o > (size_t)0x7fffffff ? -1 : (int)o;
}
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
int nrh_lspk_tail(const uint8_t t[8], uint32_t *hsize) {
    if (memcmp(t + 4, "LSPK", 4)) return -1;
    *hsize = rd32(t); return *hsize >= 12 && *hsize <= 4096 ? 0 : -1;
}
int nrh_lspk_header(const uint8_t *h, size_t n, uint32_t *list_off, uint32_t *list_size) {
    if (n < 12 || rd32(h) != 13) return -1;
    *list_off = rd32(h + 4); *list_size = rd32(h + 8);
    return *list_size >= 4 ? 0 : -1;
}
static int span_cmp(const void *a, const void *b) {
    const nrh_span *x = (const nrh_span *)a, *y = (const nrh_span *)b;
    return x->off < y->off ? -1 : x->off > y->off;
}
int nrh_lspk_levels(const uint8_t *list, size_t n, nrh_names *names, nrh_span **out) {
    *out = NULL;
    if (n < 4) return -1;
    const uint32_t count = rd32(list);
    if (!count) return 0;
    if (count > (1u << 20)) return -1;
    const size_t bytes = (size_t)count * 280;
    uint8_t *tab = (uint8_t *)malloc(bytes);
    if (!tab) return -1;
    if (nrh_lz4(list + 4, n - 4, tab, bytes) != (int)bytes) { free(tab); return -1; }
    nrh_span *v = (nrh_span *)malloc(sizeof *v * count);
    if (!v) { free(tab); return -1; }
    uint32_t k = 0; int bad = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = tab + (size_t)i * 280;
        size_t len = 0; while (len < 256 && e[len]) len++;
        const uint32_t off = rd32(e + 256), disk = rd32(e + 260), part = rd32(e + 268);
        const char *lv; size_t ln;
        if (part || !disk || !nrh_level_of_path((const char *)e, len, &lv, &ln)) continue;
        const int id = nrh_intern(names, lv, ln);
        if (id < 0) { bad = 1; break; }
        v[k].off = off; v[k].end = (uint64_t)off + disk; v[k].level = (uint32_t)id; k++;
    }
    free(tab);
    if (bad) { free(v); return -1; }
    if (!k) { free(v); return 0; }
    qsort(v, k, sizeof *v, span_cmp);
    *out = v; return (int)k;
}
int nrh_span_find(const nrh_span *v, uint32_t n, uint64_t off) {
    uint32_t lo = 0, hi = n;   // the first span with end > off
    while (lo < hi) { const uint32_t m = lo + (hi - lo) / 2; if (v[m].end <= off) lo = m + 1; else hi = m; }
    return lo < n && v[lo].off <= off ? (int)v[lo].level : -1;
}
uint64_t nrh_span_range(const nrh_span *v, uint32_t n, uint64_t off, uint64_t len, void (*add)(void *, uint32_t, uint64_t), void *ctx) {
    const uint64_t end = off + len < off ? UINT64_MAX : off + len;
    uint32_t lo = 0, hi = n;
    while (lo < hi) { const uint32_t m = lo + (hi - lo) / 2; if (v[m].end <= off) lo = m + 1; else hi = m; }
    uint64_t in = 0;
    for (uint32_t i = lo; i < n && v[i].off < end; i++) {   // sorted by offset, not overlapping (one file each)
        const uint64_t a = v[i].off > off ? v[i].off : off, b = v[i].end < end ? v[i].end : end;
        if (b > a) { in += b - a; if (add) add(ctx, v[i].level, b - a); }
    }
    return in;
}

// ---------------- save games (NR-BN) ----------------
static int ieq(const char *a, size_t an, const char *b) {   // a[0..an) == the NUL-terminated b, ASCII case-insensitive
    size_t i = 0;
    for (; i < an && b[i]; i++) { char x = a[i], y = b[i]; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; if (x != y) return 0; }
    return i == an && !b[i];
}
int nrh_lspk_entry(const uint8_t *list, size_t n, const char *name, nrh_lspk_ent *e) {
    if (n < 4) return -1;
    const uint32_t count = rd32(list);
    if (!count) return 1;
    if (count > (1u << 16)) return -1;   // a save holds a handful of files
    const size_t bytes = (size_t)count * 280;
    uint8_t *tab = (uint8_t *)malloc(bytes);
    if (!tab) return -1;
    if (nrh_lz4(list + 4, n - 4, tab, bytes) != (int)bytes) { free(tab); return -1; }
    int r = 1;
    for (uint32_t i = 0; i < count && r == 1; i++) {
        const uint8_t *x = tab + (size_t)i * 280;
        size_t len = 0; while (len < 256 && x[len]) len++;
        if (rd32(x + 268) || !ieq((const char *)x, len, name)) continue;
        e->off = rd32(x + 256); e->disk = rd32(x + 260); e->size = rd32(x + 264); e->flags = rd32(x + 272); r = 0;
    }
    free(tab); return r;
}
// one LZ4 block appended at d[o0..dn): matches may reach back into the output before o0 (linked frame blocks)
static int lz4_at(const uint8_t *s, size_t sn, uint8_t *d, size_t o0, size_t dn) {
    size_t i = 0, o = o0;
    while (i < sn) {
        const uint8_t tok = s[i++];
        size_t lit = tok >> 4;
        if (lit == 15) { uint8_t b; do { if (i >= sn) return -1; b = s[i++]; lit += b; } while (b == 255); }
        if (lit > sn - i || lit > dn - o) return -1;
        memcpy(d + o, s + i, lit); i += lit; o += lit;
        if (i >= sn) break;
        if (sn - i < 2) return -1;
        const size_t off = (size_t)s[i] | (size_t)s[i + 1] << 8; i += 2;
        if (!off || off > o) return -1;
        size_t ml = (size_t)(tok & 15) + 4;
        if ((tok & 15) == 15) { uint8_t b; do { if (i >= sn) return -1; b = s[i++]; ml += b; } while (b == 255); }
        if (ml > dn - o) return -1;
        for (size_t k = 0; k < ml; k++) { d[o] = d[o - off]; o++; }
    }
    return (int)(o - o0);
}
int nrh_lz4f(const uint8_t *s, size_t sn, uint8_t *d, size_t dn) {
    if (sn < 7 || rd32(s) != 0x184D2204u) return -1;
    const uint8_t flg = s[4];
    if ((flg >> 6) != 1 || (flg & 1)) return -1;   // version 01; a dictionary id is not supported
    size_t i = 6 + ((flg & 8) ? 8 : 0) + 1;        // FLG BD [content size] HC
    if (i > sn) return -1;
    size_t o = 0;
    for (;;) {
        if (sn - i < 4) return -1;
        const uint32_t bs = rd32(s + i); i += 4;
        if (!bs) break;                             // end mark
        const uint32_t len = bs & 0x7fffffffu;
        if (len > sn - i) return -1;
        if (bs >> 31) { if (len > dn - o) return -1; memcpy(d + o, s + i, len); o += len; }
        else { const int w = lz4_at(s + i, len, d, o, dn); if (w < 0) return -1; o += (size_t)w; }
        i += len;
        if (flg & 0x10) { if (sn - i < 4) return -1; i += 4; }   // block checksum (not verified)
    }
    return o > (size_t)0x7fffffff ? -1 : (int)o;
}
static int word_ch(uint8_t c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
int nrh_lsf_level(const uint8_t *lsf, size_t n, const nrh_names *names) {
    if (n < 52 || memcmp(lsf, "LSOF", 4)) return -1;
    const uint32_t ver = rd32(lsf + 4);
    if (ver < 1 || ver > 5) return -1;              // v6 adds a keys section (BG3): not DOS2
    const size_t m = ver >= 5 ? 16 : 12;            // magic, version, engine version (u32; u64 from v5)
    uint32_t u[8]; for (int k = 0; k < 8; k++) u[k] = rd32(lsf + m + 4 * k);
    const size_t hdr = m + 32 + 8;                  // + compression flags u8, u8, u16, u32
    const uint32_t method = lsf[m + 32] & 0x0f;
    size_t p = hdr;
    for (int k = 0; k < 3; k++) { const size_t disk = u[2 * k + 1] ? u[2 * k + 1] : u[2 * k]; if (disk > n - p) return -1; p += disk; }
    const uint32_t vu = u[6], vd = u[7];
    if (!vu || vu > (4u << 20)) return -1;
    const size_t vdisk = vd ? vd : vu;
    if (vdisk > n - p) return -1;
    uint8_t *v = (uint8_t *)malloc(vu);
    if (!v) return -1;
    int got;
    if (!vd || method == 0) { memcpy(v, lsf + p, vu); got = (int)vu; }
    else if (method == 2) { got = nrh_lz4f(lsf + p, vdisk, v, vu); if (got < 0) got = nrh_lz4(lsf + p, vdisk, v, vu); }   // chunked (frame) from v2, a plain block before
    else got = -1;                                  // zlib values: not met in DOS2 saves
    int best = -1; uint32_t best_n = 0;
    if (got > 0 && names)
        for (uint32_t id = 0; id < names->n; id++) {
            const char *nm = names->name[id]; const size_t ln = strlen(nm);
            if (ln < 3) continue;
            uint32_t cnt = 0;
            for (size_t i = 0; i + ln <= (size_t)got; i++)
                if (ieq((const char *)v + i, ln, nm) && (!i || !word_ch(v[i - 1])) && (i + ln == (size_t)got || !word_ch(v[i + ln]))) cnt++;
            if (cnt > best_n) { best_n = cnt; best = (int)id; }
        }
    free(v);
    return best;
}

// ---------------- the location vote ----------------
void nrh_vote_init(nrh_vote *v) { memset(v, 0, sizeof *v); v->cur = -1; v->min_bytes = 256.0 * 1024; v->ratio = 3; v->keep = 0.5; }
int nrh_vote_step(nrh_vote *v, const uint64_t *delta, uint32_t n) {
    if (n > NRH_LEVELS) n = NRH_LEVELS;
    int best = -1;
    for (uint32_t i = 0; i < NRH_LEVELS; i++) {
        v->score[i] = v->score[i] * v->keep + (i < n ? (double)delta[i] : 0.0);
        if (v->score[i] < 1) v->score[i] = 0;
        if (v->score[i] > 0 && (best < 0 || v->score[i] > v->score[best])) best = (int)i;
    }
    if (best < 0 || best == v->cur || v->score[best] < v->min_bytes) return 0;
    if (v->cur >= 0 && v->score[best] < v->ratio * v->score[v->cur]) return 0;
    v->cur = best; return 1;
}

// ---------------- shader keys ----------------
uint64_t nrh_key_id(const char *key) {
    uint64_t v = 0; int i = 0;
    for (; i < 16; i++) {
        const char c = key[i]; int d;
        if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10; else if (c >= 'A' && c <= 'F') d = c - 'A' + 10; else break;
        v = v << 4 | (uint64_t)d;
    }
    if (i == 16 && !key[16]) return v ? v : 1;   // 0 means "none"
    uint64_t h = 1469598103934665603ull;
    for (const char *c = key; *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
    return h | 1ull << 63;
}
void nrh_key_hex(uint64_t id, char out[17]) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) { out[i] = hx[id & 15]; id >>= 4; }
    out[16] = 0;
}

// ---------------- aggregation ----------------
int nrh_agg_init(nrh_agg *a, uint32_t cap) {
    memset(a, 0, sizeof *a);
    uint32_t c = 64; while (c < cap && c < (1u << 20)) c <<= 1;
    a->rows = (nrh_row *)calloc(c, sizeof *a->rows);
    if (!a->rows) return -1;
    a->cap = c; return 0;
}
void nrh_agg_free(nrh_agg *a) { free(a->rows); memset(a, 0, sizeof *a); }
void nrh_agg_reset(nrh_agg *a) {
    if (a->rows) memset(a->rows, 0, sizeof *a->rows * a->cap);
    a->n = 0; a->dropped = 0; memset(a->lt, 0, sizeof a->lt);
}
static uint64_t key_hash(const nrh_rkey *k) {
    uint64_t h = 1469598103934665603ull;
    const uint64_t w[6] = {k->level, (uint64_t)k->w << 32 | k->h, (uint64_t)k->fmt << 16 | (uint64_t)k->kind << 8 | k->att, k->vs, k->fs, 0};
    for (int i = 0; i < 5; i++) { h ^= w[i]; h *= 1099511628211ull; h ^= h >> 29; }
    return h;
}
static int key_eq(const nrh_rkey *x, const nrh_rkey *y) {
    return x->level == y->level && x->w == y->w && x->h == y->h && x->fmt == y->fmt && x->kind == y->kind && x->att == y->att && x->vs == y->vs && x->fs == y->fs;
}
static nrh_row *row_get(nrh_agg *a, const nrh_rkey *k) {
    if (a->n * 4 >= a->cap * 3) {   // 75 % full: no new rows (counted), existing ones still found
        for (uint32_t i = (uint32_t)key_hash(k) & (a->cap - 1), s = 0; s < a->cap; s++, i = (i + 1) & (a->cap - 1)) {
            if (!a->rows[i].used) break;
            if (key_eq(&a->rows[i].k, k)) return &a->rows[i];
        }
        a->dropped++; return NULL;
    }
    for (uint32_t i = (uint32_t)key_hash(k) & (a->cap - 1);; i = (i + 1) & (a->cap - 1)) {
        nrh_row *r = &a->rows[i];
        if (!r->used) { r->used = 1; r->k = *k; r->last_frame = UINT64_MAX; a->n++; return r; }
        if (key_eq(&r->k, k)) return r;
    }
}
static void row_add(nrh_agg *a, nrh_rkey *k, uint64_t frame, uint32_t draws, uint64_t verts, double ud, double uv, double up, double uc) {
    nrh_row *r = row_get(a, k);
    if (!r) return;
    if (r->last_frame != frame) { r->last_frame = frame; r->frames++; }
    r->passes++; r->draws += draws; r->verts += verts; r->us_d += ud; r->us_v += uv; r->us_p += up; r->us_c += uc;
}
static double prog_cost(const nrh_prog *g) {   // -1 = unknown
    if (g->cfs < 0 && g->cvs < 0) return -1;
    return (g->cfs > 0 ? g->cfs : 0) + (g->cvs > 0 ? g->cvs : 0);
}
static nrh_ltot *lt_of(nrh_agg *a, uint32_t level, uint64_t frame) {
    nrh_ltot *t = &a->lt[level < NRH_LEVELS ? level : NRH_LEVELS - 1];
    if (t->frames == 0 || t->last_frame != frame) { t->last_frame = frame; t->frames++; }
    return t;
}
void nrh_agg_add(nrh_agg *a, uint32_t level, uint64_t frame, const nrh_pass *p, uint32_t n) {
    if (level >= NRH_LEVELS) level = NRH_LEVELS - 1;
    nrh_ltot *t = lt_of(a, level, frame);
    for (uint32_t i = 0; i < n; i++) {
        const nrh_pass *q = &p[i];
        const double us = q->us > 0 ? q->us : 0;
        nrh_rkey k; memset(&k, 0, sizeof k);
        k.level = level; k.kind = q->compute ? 1 : 0; k.w = q->w; k.h = q->h; k.fmt = q->fmt; k.att = (uint8_t)(q->att > 255 ? 255 : q->att);
        if (q->compute) { t->cpasses++; t->compute_us += us; } else { t->passes++; t->pass_us += us; t->draws += q->draws; }
        uint64_t sd = 0, sv = 0; double kd = 0, kc = 0;   // known costs: draws and draws x cost
        for (uint32_t j = 0; j < q->nprog; j++) {
            const nrh_prog *g = &q->prog[j]; const double c = prog_cost(g), d = q->compute ? 1.0 : (double)g->draws;
            sd += g->draws; sv += g->verts;
            if (c >= 0) { kd += d; kc += d * c; }
        }
        if (!q->nprog || (!sd && !q->compute)) { k.vs = k.fs = 0; row_add(a, &k, frame, q->draws, q->verts, us, us, us, us); continue; }
        const double mean = kd > 0 ? kc / kd : -1;
        double sw = 0;
        for (uint32_t j = 0; j < q->nprog; j++) {
            const nrh_prog *g = &q->prog[j]; double c = prog_cost(g); if (c < 0) c = mean;
            sw += (q->compute ? 1.0 : (double)g->draws) * (c > 0 ? c : 0);
        }
        for (uint32_t j = 0; j < q->nprog; j++) {
            const nrh_prog *g = &q->prog[j];
            const double fd = sd ? (double)g->draws / (double)sd : 1.0 / q->nprog;
            const double fv = sv ? (double)g->verts / (double)sv : fd;
            double c = prog_cost(g); if (c < 0) c = mean;
            const double fc = sw > 0 ? (q->compute ? 1.0 : (double)g->draws) * (c > 0 ? c : 0) / sw : fd;
            k.vs = g->vs; k.fs = g->fs;
            row_add(a, &k, frame, g->draws, g->verts, us * fd, us * fv, us, us * fc);
        }
    }
}
void nrh_agg_span(nrh_agg *a, uint32_t level, uint64_t frame, double span_us) {
    if (level >= NRH_LEVELS) level = NRH_LEVELS - 1;
    nrh_ltot *t = lt_of(a, level, frame);
    if (span_us > 0) t->span_us += span_us;
}
int nrh_agg_levels(const nrh_agg *a, uint32_t *out, int cap) {
    int k = 0;
    for (uint32_t i = 0; i < NRH_LEVELS && k < cap; i++) if (a->lt[i].frames) out[k++] = i;
    return k;
}
int nrh_agg_top(const nrh_agg *a, uint32_t level, uint32_t *idx, int k) {
    int m = 0;
    if (k <= 0) return 0;
    for (uint32_t i = 0; i < a->cap; i++) {
        const nrh_row *r = &a->rows[i];
        if (!r->used || r->k.level != level || !r->passes) continue;
        int j = m < k ? m++ : k;   // insertion into the top k (k is small)
        if (j == k) { if (r->us_d <= a->rows[idx[k - 1]].us_d) continue; j = k - 1; }
        while (j > 0 && a->rows[idx[j - 1]].us_d < r->us_d) { idx[j] = idx[j - 1]; j--; }
        idx[j] = i;
    }
    return m;
}

// ---------------- static SPIR-V cost ----------------
static int in_range(uint32_t op, uint32_t lo, uint32_t hi) { return op >= lo && op <= hi; }
int nrh_spirv_cost(const uint32_t *w, size_t n, nrh_cost *c) {
    memset(c, 0, sizeof *c);
    if (n < 5 || w[0] != 0x07230203u || w[3] > (1u << 22)) return -1;
    const uint32_t bound = w[3] ? w[3] : 1;
    // per id: bit 0 constant, bit 1 GLSL.std.450 import, bit 2 dynamic index on the way, bits 3..7 storage class + 1 (classes 0..30)
    uint8_t *info = (uint8_t *)calloc(bound, 1);
    if (!info) return -1;
    int in_fn = 0;
    for (size_t i = 5; i < n;) {
        const uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (!len || i + len > n) break;
        const uint32_t *x = w + i;
        if (op == 11 && len > 4 && x[1] < bound && !memcmp(&x[2], "GLSL.std.450", 12)) info[x[1]] |= 2;
        else if ((op == 43 || op == 44 || op == 46 || op == 41 || op == 42 || in_range(op, 50, 52)) && len > 2 && x[2] < bound) info[x[2]] |= 1;
        else if (op == 59 && len > 3 && x[2] < bound && x[3] < 31) info[x[2]] = (uint8_t)((info[x[2]] & 7) | (x[3] + 1) << 3);
        else if ((op == 65 || op == 66) && len > 3 && x[2] < bound && x[3] < bound) {   // an access chain: its base's class (and dynamic flag)
            uint8_t v = (uint8_t)((info[x[3]] & 0xfc) | (info[x[2]] & 3));
            for (uint32_t k = 4; k < len; k++) if (x[k] >= bound || !(info[x[k]] & 1)) v |= 4;
            info[x[2]] = v;
        }
        if (op == 54) in_fn = 1;
        else if (op == 56) in_fn = 0;
        else if (in_fn) {
            if (op != 248 && op != 8 && op != 317 && op != 247 && op != 249 && op != 59) c->inst++;
            if (in_range(op, 109, 124) || in_range(op, 126, 152) || in_range(op, 154, 157) || in_range(op, 164, 191) || in_range(op, 194, 205) || in_range(op, 207, 215)) c->alu++;
            if (op == 124) c->bitcast++;
            if (in_range(op, 87, 98) || in_range(op, 305, 320)) c->tex++;
            if (op == 250 || op == 251) c->branch++;
            if (op == 246) c->loops++;
            if (op == 252 || op == 4416 || op == 5380) c->kill++;
            if (op == 12 && len > 4) {
                const uint32_t e = x[4];
                const int trans = in_range(e, 13, 32) || e == 66 || e == 67 || e == 69;
                if (x[3] < bound && (info[x[3]] & 2) && trans) c->trans++; else c->alu++;
            }
            if (op == 61 && len > 3 && x[3] < bound) {
                const unsigned sc = info[x[3]] >> 3;   // storage class + 1: Uniform 2, PushConstant 9, StorageBuffer 12
                if (sc == 3 || sc == 10 || sc == 13) { c->ubo++; if (info[x[3]] & 4) c->ubo_dyn++; }
            }
        }
        i += len;
    }
    free(info);
    c->cost = c->alu + 4 * c->trans + 4 * c->tex + 2 * c->ubo_dyn + 2 * c->branch;
    return 0;
}

// ---------------- lines ----------------
void nrh_clean(char *s) { for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' '; }
static const char *key_text(uint64_t id, char buf[17], nrh_key_name_fn name, void *ctx) {
    if (!id) return "-";
    if (id >> 63 && name) { const char *s = name(id, ctx); if (s && *s) return s; }
    nrh_key_hex(id, buf); return buf;
}
static int done(int r, size_t n) { return r < 0 || (size_t)r >= n ? -1 : r; }
int nrh_fmt_row(char *out, size_t n, const nrh_row *r, nrh_key_name_fn name, void *ctx) {
    char a[17], b[17];
    return done(snprintf(out, n, "A\t%c\t%ux%u\t%u\t%u\t%s\t%s\t%u\t%u\t%llu\t%llu\t%.0f\t%.0f\t%.0f\t%.0f\n", r->k.kind ? 'c' : 'p', r->k.w, r->k.h, r->k.fmt,
                         (unsigned)r->k.att, key_text(r->k.vs, a, name, ctx), key_text(r->k.fs, b, name, ctx), r->frames, r->passes,
                         (unsigned long long)r->draws, (unsigned long long)r->verts, r->us_d, r->us_v, r->us_p, r->us_c), n);
}
int nrh_fmt_block(char *out, size_t n, const char *level, const nrh_ltot *t, uint64_t unix_s, double t_s) {
    return done(snprintf(out, n, "B\t%llu\t%.0f\t%s\t%u\t%u\t%llu\t%.0f\t%.0f\t%.0f\n", (unsigned long long)unix_s, t_s, level, t->frames, t->passes,
                         (unsigned long long)t->draws, t->span_us, t->pass_us, t->compute_us), n);
}
int nrh_fmt_pass(char *out, size_t n, uint64_t frame, uint32_t i, const nrh_pass *p, nrh_key_name_fn name, void *ctx) {
    int o = snprintf(out, n, "P\t%llu\t%u\t%c\t%.0f\t%ux%u\t%u\t%u\t%u\t%llu\t", (unsigned long long)frame, i, p->compute ? 'c' : 'p', p->us, p->w, p->h,
                     p->fmt, p->att, p->draws, (unsigned long long)p->verts);
    if (o < 0 || (size_t)o >= n) return -1;
    if (!p->nprog) { int r = snprintf(out + o, n - (size_t)o, "-"); if (r < 0 || (size_t)(o + r) >= n) return -1; o += r; }
    for (uint32_t j = 0; j < p->nprog; j++) {
        char a[17], b[17];
        int r = snprintf(out + o, n - (size_t)o, "%s%s/%s/%u/%llu", j ? "," : "", key_text(p->prog[j].vs, a, name, ctx), key_text(p->prog[j].fs, b, name, ctx),
                         p->prog[j].draws, (unsigned long long)p->prog[j].verts);
        if (r < 0 || (size_t)(o + r) >= n) return -1;
        o += r;
    }
    if ((size_t)o + 2 > n) return -1;
    out[o++] = '\n'; out[o] = 0; return o;
}

// ---------------- the output file ----------------
int nrh_out_open(nrh_out *o, const char *dir, const char *name, uint64_t cap) {
    memset(o, 0, sizeof *o);
    if (snprintf(o->path, sizeof o->path, "%s/%s", dir, name) >= (int)sizeof o->path) return -1;
    const size_t l = strlen(o->path);
    if (l > 4 && !strcmp(o->path + l - 4, ".tsv")) snprintf(o->old, sizeof o->old, "%.*s.1.tsv", (int)(l - 4), o->path);
    else snprintf(o->old, sizeof o->old, "%s.1", o->path);
    o->cap = cap < 4096 ? 4096 : cap;
    o->f = fopen(o->path, "ab");
    if (!o->f) return -1;
    if (fseek(o->f, 0, SEEK_END) == 0) { const long s = ftell(o->f); o->bytes = s > 0 ? (uint64_t)s : 0; }
    return 0;
}
int nrh_out_rotate(nrh_out *o) {
    if (o->f) { fclose(o->f); o->f = NULL; }
    remove(o->old);
    if (rename(o->path, o->old)) remove(o->path);
    o->f = fopen(o->path, "wb"); o->bytes = 0; o->rotations++;
    return o->f ? 0 : -1;
}
int nrh_out_full(const nrh_out *o, size_t more) { return o->f && o->bytes > 0 && o->bytes + more > o->cap; }
int nrh_out_write(nrh_out *o, const char *s, size_t n) {
    if (!o->f || !n) return 0;
    if (fwrite(s, 1, n, o->f) != n) return 0;
    o->bytes += n; return 1;
}
void nrh_out_close(nrh_out *o) { if (o->f) fclose(o->f); o->f = NULL; }
