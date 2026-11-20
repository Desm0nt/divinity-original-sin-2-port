// [DOS2] NR-06: ring mirror bookkeeping and the static buffer table (see nr_buf.h). Pure C, host-testable (tests/nr_buf_test.c).
#include "nr_buf.h"
#include <stdlib.h>
#include <string.h>

const char *nr_ring_result_name(int r) {
    static const char *const n[NRR_N] = {"valid", "work", "not-ring", "span", "hold", "runs"};
    return r >= 0 && r < NRR_N ? n[r] : "?";
}
const char *nr_sbuf_class_name(int c) { static const char *const n[NRS_CLASSES] = {"static", "dynamic", "no-init"}; return c >= 0 && c < NRS_CLASSES ? n[c] : "?"; }

// ---- ring ----
int nr_ring_init(nr_ring *r, uint64_t base, uint64_t size) {
    memset(r, 0, sizeof *r);
    if (!base || !size || size > (1ull << 32)) return -1;
    const uint32_t n = (uint32_t)((size + NR_RING_GRAN - 1) >> NR_RING_GSHIFT);
    r->epoch = (uint32_t *)calloc(n, 4); r->used = (uint32_t *)calloc(n, 4); r->hold = (uint32_t *)calloc(n, 4);
    if (!r->epoch || !r->used || !r->hold) { nr_ring_free(r); return -1; }
    r->base = base; r->size = size; r->ngran = n;
    return 0;
}
void nr_ring_free(nr_ring *r) { free(r->epoch); free(r->used); free(r->hold); memset(r, 0, sizeof *r); }
int nr_ring_check(const nr_ring *r, uint64_t addr, uint64_t n, uint32_t epoch, uint32_t serial, nr_ring_work *w) {
    w->n = 0;
    if (!nr_ring_has(r, addr)) return NRR_NOT;
    const uint64_t off = addr - r->base;
    if (!n) n = 1;
    if (n > r->size - off) return NRR_SPAN;
    const uint32_t g0 = (uint32_t)(off >> NR_RING_GSHIFT), g1 = (uint32_t)((off + n - 1) >> NR_RING_GSHIFT) + 1;
    int open = 0;   // kind of the run being built (0 = none)
    for (uint32_t g = g0; g < g1; g++) {
        if (r->hold[g] > serial) { w->n = 0; return NRR_HOLD; }
        int k = 0;
        if (r->epoch[g] != epoch) k = !r->used[g] || r->used[g] + 2 <= serial ? NRR_COPY : NRR_COMPARE;
        if (k == open) { if (k) w->run[w->n - 1].g1 = g + 1; continue; }
        open = k;
        if (!k) continue;
        if (w->n == NR_RING_RUNS) { w->n = 0; return NRR_RUNS_OVER; }
        w->run[w->n].g0 = g; w->run[w->n].g1 = g + 1; w->run[w->n].kind = (uint8_t)k; w->n++;
    }
    return w->n ? NRR_WORK : NRR_VALID;
}
void nr_ring_done(nr_ring *r, uint32_t g0, uint32_t g1, uint32_t epoch, uint32_t serial) {
    if (g1 > r->ngran) g1 = r->ngran;
    for (uint32_t g = g0; g < g1; g++) { r->epoch[g] = epoch; r->used[g] = serial; }
}
void nr_ring_race(nr_ring *r, uint32_t g0, uint32_t g1, uint32_t serial) {
    if (g1 > r->ngran) g1 = r->ngran;
    for (uint32_t g = g0; g < g1; g++) {
        const uint32_t until = (r->used[g] ? r->used[g] : serial) + 2;
        if (r->hold[g] < until) r->hold[g] = until;
        r->epoch[g] = 0; r->holds_race++;
    }
}
uint32_t nr_ring_cpu_write(nr_ring *r, uint64_t addr, uint64_t n, uint32_t serial) {
    if (!r->ngran || !n || addr >= r->base + r->size || addr + n <= r->base) return 0;
    const uint64_t lo = addr > r->base ? addr - r->base : 0, hi = addr + n - r->base < r->size ? addr + n - r->base : r->size;
    uint32_t held = 0;
    for (uint32_t g = (uint32_t)(lo >> NR_RING_GSHIFT), e = (uint32_t)((hi - 1) >> NR_RING_GSHIFT) + 1; g < e; g++) {
        r->epoch[g] = 0;   // the next bind copies / compares again
        if (r->used[g] && r->used[g] + 2 > serial) {   // a recorded or flying submission reads the old bytes from the mirror
            const uint32_t until = r->used[g] + 2;
            if (r->hold[g] < until) r->hold[g] = until;
            held++;
        }
    }
    r->holds_cpu += held;
    return held;
}

// ---- static buffer table ----
static int grow(void **p, uint32_t *cap, uint32_t need, size_t elem) {
    if (need <= *cap) return 0;
    uint32_t c = *cap ? *cap : 256;
    while (c < need) c *= 2;
    void *q = realloc(*p, (size_t)c * elem);
    if (!q) return -1;
    *p = q; *cap = c;
    return 0;
}
int nr_stab_init(nr_stab *t) {
    memset(t, 0, sizeof *t);
    t->ver = 1;   // front entries start at ver 0: never valid
    for (int k = 0; k < NRS_KINDS; k++) if (!(t->lo_of[k] = (uint64_t *)calloc(NRS_HANDLES, 8))) { nr_stab_free(t); return -1; }
    return 0;
}
void nr_stab_free(nr_stab *t) {
    free(t->e); free(t->b); free(t->ix);
    for (int k = 0; k < NRS_KINDS; k++) free(t->lo_of[k]);
    memset(t, 0, sizeof *t);
}
static uint32_t lower(const nr_sbuf *e, uint32_t n, uint64_t lo) {   // first entry with e.lo >= lo
    uint32_t a = 0, b = n;
    while (a < b) { const uint32_t m = a + (b - a) / 2; if (e[m].lo < lo) a = m + 1; else b = m; }
    return a;
}
static int kill_entry(nr_stab *t, nr_sbuf *x) { if (x->dead) return 0; x->dead = 1; if (x >= t->e && x < t->e + t->n) t->ndead++; return 1; }
int nr_stab_add(nr_stab *t, int kind, uint64_t handle, uint64_t lo, uint64_t size, int cls, uint64_t hash) {
    if (kind < 0 || kind >= NRS_KINDS || !lo || !size || lo + size < lo) return -1;
    if (grow((void **)&t->b, &t->capb, t->nb + 1, sizeof *t->b)) return -1;
    nr_sbuf *x = &t->b[t->nb++];
    memset(x, 0, sizeof *x);
    x->lo = lo; x->hi = lo + size; x->handle = handle; x->hash = hash; x->seq = ++t->seq; x->slot = -1; x->kind = (uint8_t)kind; x->cls = (uint8_t)cls;
    t->st.adds++;
    // memory reused: every live table range under the new one dies now (lookups must not find it while the add waits for its merge)
    uint32_t i = lower(t->e, t->n, lo);
    if (i > 0 && t->e[i - 1].hi > lo) i--;
    for (; i < t->n && t->e[i].lo < lo + size; i++) if (t->e[i].hi > lo && kill_entry(t, &t->e[i])) t->st.overlapped++;
    return 0;
}
int nr_stab_kill(nr_stab *t, int kind, uint64_t handle) {
    if (kind < 0 || kind >= NRS_KINDS) return 0;
    t->st.kills++;
    for (uint32_t i = t->nb; i-- > 0;) if (!t->b[i].dead && t->b[i].kind == kind && t->b[i].handle == handle) { kill_entry(t, &t->b[i]); return 1; }   // newest first
    const uint32_t idx = (uint32_t)handle;
    if (idx < NRS_HANDLES) {
        const uint64_t lo = t->lo_of[kind][idx];
        if (lo) {
            const uint32_t i = lower(t->e, t->n, lo);
            if (i < t->n && t->e[i].lo == lo && !t->e[i].dead && t->e[i].kind == kind && t->e[i].handle == handle) { kill_entry(t, &t->e[i]); t->lo_of[kind][idx] = 0; return 1; }
        }
    } else {
        t->st.no_index++;
        for (uint32_t i = 0; i < t->n; i++) if (!t->e[i].dead && t->e[i].kind == kind && t->e[i].handle == handle) { kill_entry(t, &t->e[i]); return 1; }
    }
    t->st.kill_miss++;
    return 0;
}
uint32_t nr_stab_drop(nr_stab *t, uint64_t lo, uint64_t n) {
    if (!n) return 0;
    const uint64_t hi = lo + n;
    uint32_t k = 0;
    for (uint32_t i = 0; i < t->nb; i++) if (t->b[i].lo < hi && lo < t->b[i].hi) k += (uint32_t)kill_entry(t, &t->b[i]);
    uint32_t i = lower(t->e, t->n, lo);
    if (i > 0 && t->e[i - 1].hi > lo) i--;   // the one before may reach into the range
    for (; i < t->n && t->e[i].lo < hi; i++) if (t->e[i].hi > lo) k += (uint32_t)kill_entry(t, &t->e[i]);
    t->st.drops += k;
    return k;
}
static const nr_sbuf *g_sort_b;   // qsort context (apply is single-threaded by contract)
static int by_lo(const void *a, const void *b) {
    const nr_sbuf *x = &g_sort_b[*(const uint32_t *)a], *y = &g_sort_b[*(const uint32_t *)b];
    if (x->lo != y->lo) return x->lo < y->lo ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}
uint32_t nr_stab_apply(nr_stab *t, void (*gone)(void *ctx, const nr_sbuf *e), void *ctx) {
    const uint32_t k = t->nb;
    if (!k && !t->ndead) return t->n;   // nothing changed since the last apply
    t->st.applies++;
    if (k && grow((void **)&t->ix, &t->capix, k, sizeof *t->ix)) return t->n;   // no memory: the queue waits for the next apply
    // 1. the queued adds among themselves, in event order: a later add kills every earlier overlapping one. Only when the queue has any
    //    overlap at all (sorted by lo, a running end past the next start) is the quadratic walk needed.
    uint32_t m = 0;
    for (uint32_t i = 0; i < k; i++) t->ix[m++] = i;
    g_sort_b = t->b; qsort(t->ix, m, sizeof *t->ix, by_lo);
    int overlap = 0; uint64_t end = 0;
    for (uint32_t i = 0; i < m; i++) { const nr_sbuf *x = &t->b[t->ix[i]]; if (i && x->lo < end) { overlap = 1; break; } if (x->hi > end) end = x->hi; }
    if (overlap)
        for (uint32_t i = 1; i < k; i++) for (uint32_t j = 0; j < i; j++)
            if (!t->b[j].dead && t->b[j].lo < t->b[i].hi && t->b[i].lo < t->b[j].hi) { t->b[j].dead = 1; t->st.overlapped++; }
    // 2. every table entry overlapping a queued add dies (dead or alive: applied one by one, that add killed it before it died itself)
    for (uint32_t q = 0; q < m; q++) {
        const nr_sbuf *x = &t->b[t->ix[q]];
        uint32_t i = lower(t->e, t->n, x->lo);
        if (i > 0 && t->e[i - 1].hi > x->lo) i--;
        for (; i < t->n && t->e[i].lo < x->hi; i++) if (t->e[i].hi > x->lo && kill_entry(t, &t->e[i])) t->st.overlapped++;
    }
    // 3. merge: live table entries + live queued adds by lo into a new array; the dead ones leave (gone)
    uint32_t live = 0;
    for (uint32_t i = 0; i < t->n; i++) live += !t->e[i].dead;
    for (uint32_t q = 0; q < m; q++) live += !t->b[t->ix[q]].dead;
    nr_sbuf *out = (nr_sbuf *)malloc((size_t)(live ? live : 1) * sizeof *out);
    if (!out) return t->n;   // no memory: the queue waits (its dead marks and kills stay)
    uint32_t a = 0, q = 0, o = 0;
    while (a < t->n || q < m) {
        nr_sbuf *x;
        if (q >= m || (a < t->n && t->e[a].lo <= t->b[t->ix[q]].lo)) x = &t->e[a++]; else x = &t->b[t->ix[q++]];
        const uint32_t idx = (uint32_t)x->handle;
        if (x->dead) {
            if (gone) gone(ctx, x);
            if (idx < NRS_HANDLES && t->lo_of[x->kind][idx] == x->lo) t->lo_of[x->kind][idx] = 0;
            continue;
        }
        out[o++] = *x;
        if (idx < NRS_HANDLES) t->lo_of[x->kind][idx] = x->lo;
    }
    free(t->e); t->e = out; t->cap = live ? live : 1; t->n = o; t->nb = 0; t->ndead = 0; t->ver++;
    return o;
}
nr_sbuf *nr_stab_find(nr_stab *t, uint64_t addr, uint64_t n) {
    if (!n) n = 1;
    const uint32_t f = (uint32_t)((addr * 0x9E3779B97F4A7C15ull) >> 54);
    nr_sbuf *x;
    if (t->front[f].ver == t->ver && t->front[f].addr == addr && t->front[f].idx < t->n) x = &t->e[t->front[f].idx];   // same address as before: same candidate
    else {
        const uint32_t i = lower(t->e, t->n, addr + 1);   // first entry starting past addr: the candidate is the one before
        if (!i) return NULL;
        x = &t->e[i - 1];
        t->front[f].addr = addr; t->front[f].idx = i - 1; t->front[f].ver = t->ver;
    }
    return !x->dead && addr >= x->lo && addr + n <= x->hi && addr + n > addr ? x : NULL;
}
uint32_t nr_stab_each(nr_stab *t, uint64_t lo, uint64_t n, void (*fn)(void *ctx, nr_sbuf *e), void *ctx) {
    if (!n || !t->n) return 0;
    const uint64_t hi = lo + n;
    uint32_t i = lower(t->e, t->n, lo), k = 0;
    if (i > 0 && t->e[i - 1].hi > lo) i--;
    for (; i < t->n && t->e[i].lo < hi; i++) if (t->e[i].hi > lo && !t->e[i].dead) { fn(ctx, &t->e[i]); k++; }
    return k;
}
