// [DOS2] NR-08b: see nr_ubodyn.h.
#include "nr_ubodyn.h"
#include <stdlib.h>
#include <string.h>

const char *nru_why_name(int why) {
    static const char *const n[NRU_N] = {"understood", "bad header", "truncated instruction", "not exactly one entry point", "no entry function", "no memory"};
    return why >= 0 && why < NRU_N ? n[why] : "?";
}

enum { OP_ENTRY_POINT = 15, OP_FUNCTION_CALL = 57, OP_FUNCTION = 54, OP_FUNCTION_END = 56, OP_VARIABLE = 59, OP_LOAD = 61,
       OP_COPY_MEMORY = 63, OP_COPY_MEMORY_SIZED = 64, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_PTR_ACCESS_CHAIN = 67,
       OP_ARRAY_LENGTH = 68, OP_IN_BOUNDS_PTR_ACCESS_CHAIN = 70, OP_DECORATE = 71, OP_COPY_OBJECT = 83, DEC_BINDING = 33 };
enum { MAX_BOUND = 1u << 22 };

static int header_ok(const uint32_t *w, size_t n) { return w && n >= 5 && w[0] == 0x07230203u && w[3] && w[3] <= MAX_BOUND; }

// The bindings the module reads (rule of nr_ubodyn.h): *out = malloc'd list (may repeat), *nout its length. Returns NRU_*.
static int live_reads(const uint32_t *w, size_t n, uint32_t **out, unsigned *nout) {
    *out = NULL; *nout = 0;
    if (!header_ok(w, n)) return NRU_BAD_HEADER;
    const uint32_t bound = w[3];
    int32_t *bind = (int32_t *)malloc(sizeof(int32_t) * bound), *ptr = (int32_t *)malloc(sizeof(int32_t) * bound);
    uint32_t *occ = (uint32_t *)calloc(bound, sizeof(uint32_t)); uint8_t *var = (uint8_t *)calloc(bound, 1);
    int why = NRU_OK;
    if (!bind || !ptr || !occ || !var) { why = NRU_NO_MEMORY; goto done; }
    for (uint32_t i = 0; i < bound; i++) bind[i] = ptr[i] = -1;
    // pass 1: entry point, bindings, variables, the entry function's word range
    unsigned entries = 0; uint32_t entry_fn = 0; size_t fbeg = 0, fend = 0, cur = 0; int in_entry = 0;
    for (size_t i = 5; i < n;) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if (!wc || i + wc > n) { why = NRU_TRUNCATED; goto done; }
        if (op == OP_ENTRY_POINT && wc >= 3) { entries++; entry_fn = w[i + 2]; }
        else if (op == OP_DECORATE && wc >= 4 && w[i + 2] == DEC_BINDING && w[i + 1] < bound) bind[w[i + 1]] = (int32_t)w[i + 3];
        else if (op == OP_VARIABLE && wc >= 4 && w[i + 2] < bound) var[w[i + 2]] = 1;
        else if (op == OP_FUNCTION && wc >= 3) { cur = i; in_entry = entries == 1 && w[i + 2] == entry_fn; }
        else if (op == OP_FUNCTION_END && in_entry) { fbeg = cur; fend = i + wc; in_entry = 0; }
        i += wc;
    }
    if (entries != 1) { why = NRU_ENTRY_POINTS; goto done; }
    if (!fend) { why = NRU_NO_ENTRY; goto done; }
    // pass 2 over the entry function: pointers into bound variables, uses of every id
    {
        uint32_t cap = 64, nc = 0; uint32_t (*cand)[2] = (uint32_t (*)[2])malloc(sizeof(uint32_t[2]) * cap);   // (result id or ~0 = read as such, binding)
        if (!cand) { why = NRU_NO_MEMORY; goto done; }
#define BASE(id) ((id) < bound ? (var[(id)] && bind[(id)] >= 0 ? bind[(id)] : ptr[(id)]) : -1)
#define CAND(res, b) do { if (nc == cap) { cap *= 2; void *g = realloc(cand, sizeof(uint32_t[2]) * cap); if (!g) { free(cand); why = NRU_NO_MEMORY; goto done; } cand = (uint32_t (*)[2])g; } \
                          cand[nc][0] = (res); cand[nc][1] = (uint32_t)(b); nc++; } while (0)
        for (size_t i = fbeg; i < fend;) {
            const uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
            for (uint32_t k = 1; k < wc; k++) if (w[i + k] < bound) occ[w[i + k]]++;
            int32_t b;
            switch (op) {
            case OP_ACCESS_CHAIN: case OP_IN_BOUNDS_ACCESS_CHAIN: case OP_PTR_ACCESS_CHAIN: case OP_IN_BOUNDS_PTR_ACCESS_CHAIN: case OP_COPY_OBJECT:
                if (wc >= 4 && (b = BASE(w[i + 3])) >= 0 && w[i + 2] < bound) ptr[w[i + 2]] = b;   // a pointer into the block: not a read by itself
                break;
            case OP_LOAD: case OP_ARRAY_LENGTH:
                if (wc >= 4 && (b = BASE(w[i + 3])) >= 0) CAND(w[i + 2], b);
                break;
            case OP_COPY_MEMORY: case OP_COPY_MEMORY_SIZED:
                if (wc >= 3 && (b = BASE(w[i + 2])) >= 0) CAND(~0u, b);   // writes memory: never dead
                break;
            case OP_FUNCTION_CALL:
                for (uint32_t k = 4; k < wc; k++) if ((b = BASE(w[i + k])) >= 0) CAND(~0u, b);
                break;
            default: break;
            }
            i += wc;
        }
#undef BASE
#undef CAND
        // a load counts when its value is used: its id appears in some other instruction of the function (its own definition is one)
        uint32_t m = 0;
        for (uint32_t c = 0; c < nc; c++) if (cand[c][0] == ~0u || (cand[c][0] < bound && occ[cand[c][0]] >= 2)) cand[m++][1] = cand[c][1];
        uint32_t *o = (uint32_t *)malloc(sizeof(uint32_t) * (m ? m : 1));
        if (!o) { free(cand); why = NRU_NO_MEMORY; goto done; }
        for (uint32_t c = 0; c < m; c++) o[c] = cand[c][1];
        free(cand); *out = o; *nout = m;
    }
done:
    free(bind); free(ptr); free(occ); free(var);
    return why;
}

int nr_spv_binding_reads(const uint32_t *w, size_t n, const uint32_t *bindings, unsigned nb, uint8_t *read) {
    if (nb) memset(read, 0, nb);
    uint32_t *r = NULL; unsigned nr = 0;
    const int why = live_reads(w, n, &r, &nr);
    if (why == NRU_OK)
        for (unsigned i = 0; i < nb; i++) for (unsigned k = 0; k < nr; k++) if (r[k] == bindings[i]) { read[i] = 1; break; }
    free(r);
    return why;
}

static int cmp_u32(const void *a, const void *b) { const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }

int nr_spv_declared_bindings(const uint32_t *w, size_t n, uint32_t *out, unsigned max) {
    if (!header_ok(w, n)) return -1;
    const uint32_t bound = w[3];
    int32_t *bind = (int32_t *)malloc(sizeof(int32_t) * bound); uint8_t *var = (uint8_t *)calloc(bound, 1);
    if (!bind || !var) { free(bind); free(var); return -1; }
    for (uint32_t i = 0; i < bound; i++) bind[i] = -1;
    for (size_t i = 5; i < n;) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if (!wc || i + wc > n) break;
        if (op == OP_DECORATE && wc >= 4 && w[i + 2] == DEC_BINDING && w[i + 1] < bound) bind[w[i + 1]] = (int32_t)w[i + 3];
        else if (op == OP_VARIABLE && wc >= 4 && w[i + 2] < bound) var[w[i + 2]] = 1;
        i += wc;
    }
    uint32_t *all = (uint32_t *)malloc(sizeof(uint32_t) * bound); unsigned na = 0;
    if (!all) { free(bind); free(var); return -1; }
    for (uint32_t id = 0; id < bound; id++) if (var[id] && bind[id] >= 0) all[na++] = (uint32_t)bind[id];
    qsort(all, na, sizeof *all, cmp_u32);
    unsigned u = 0;
    for (unsigned i = 0; i < na; i++) if (!u || all[u - 1] != all[i]) all[u++] = all[i];
    for (unsigned i = 0; i < u && i < max; i++) out[i] = all[i];
    free(all); free(bind); free(var);
    return (int)u;
}

int nr_spv_reads_kept(const uint32_t *before, size_t nb, const uint32_t *after, size_t na) {
    const int nd = nr_spv_declared_bindings(before, nb, NULL, 0);
    if (nd <= 0) return 1;   // nothing declared (or not a module): nothing could have been made dynamic from it
    uint32_t *d = (uint32_t *)malloc(sizeof(uint32_t) * (unsigned)nd); uint8_t *rb = (uint8_t *)malloc((unsigned)nd), *ra = (uint8_t *)malloc((unsigned)nd);
    int kept = 0;
    if (d && rb && ra) {
        nr_spv_declared_bindings(before, nb, d, (unsigned)nd);
        if (nr_spv_binding_reads(before, nb, d, (unsigned)nd, rb) != NRU_OK) kept = 1;   // `before` not understood: nothing marked read
        else if (nr_spv_binding_reads(after, na, d, (unsigned)nd, ra) == NRU_OK) {
            kept = 1;
            for (int i = 0; i < nd; i++) if (rb[i] && !ra[i]) { kept = 0; break; }
        }
    }
    free(d); free(rb); free(ra);
    return kept;
}
