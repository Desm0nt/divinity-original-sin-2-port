// [DOS2] NR-33B DM_NR_MAT: see nr_mat.h; docs/dos2/nr/NR-33B.md.
#include "nr_mat.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { OpName = 5, OpEntryPoint = 15, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeArray = 28, OpTypePointer = 32,
       OpConstant = 43, OpFunction = 54, OpFunctionCall = 57, OpVariable = 59, OpImageTexelPointer = 60, OpLoad = 61, OpStore = 62,
       OpCopyMemory = 63, OpCopyMemorySized = 64, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpPtrAccessChain = 67,
       OpDecorate = 71, OpMemberDecorate = 72, OpInBoundsPtrAccessChain = 70, OpExtInst = 12, OpCopyObject = 83, OpImageWrite = 99,
       OpSelect = 169, OpPhi = 245, OpKill = 252, OpTerminateInvocation = 4416, OpDemote = 5380 };
enum { DecBuiltIn = 11, DecLocation = 30 };
enum { SC_Input = 1, SC_Output = 3, SC_Private = 6, SC_Function = 7 };

static uint32_t span_of(const uint32_t *ty, const uint32_t *tyop, const uint32_t *cval, const uint8_t *cset, uint32_t bound, uint32_t t, int *complex) {
    // locations a plain Location variable of pointee type t takes; *complex = 1 when it is not a 32/16-bit scalar / vector / array of them
    if (t >= bound) { *complex = 1; return 1; }
    uint32_t op = tyop[t];
    if (op == OpTypeArray) {
        const uint32_t e = ty[2 * t], len = ty[2 * t + 1];
        if (len >= bound || !cset[len] || cval[len] == 0 || cval[len] > 32) { *complex = 1; return 1; }
        int c = 0; const uint32_t s = span_of(ty, tyop, cval, cset, bound, e, &c);
        if (c || s != 1) { *complex = 1; return 1; }
        return cval[len];
    }
    if (op == OpTypeVector) { const uint32_t e = ty[2 * t]; if (e >= bound || (tyop[e] != OpTypeFloat && tyop[e] != OpTypeInt) || ty[2 * e] > 32) *complex = 1; return 1; }
    if (op == OpTypeFloat || op == OpTypeInt) { if (ty[2 * t] > 32) *complex = 1; return 1; }
    *complex = 1; return 1;
}

typedef struct mod {
    uint32_t bound;
    uint32_t *tyop, *ty, *cval, *ptr_sc, *ptr_pt, *var_sc, *var_pt, *loc, *root;   // per id
    uint8_t *cset, *has_loc, *builtin;
    size_t first_fn;   // word index of the first OpFunction
    uint32_t model;
} mod;

static void mod_free(mod *m) {
    free(m->tyop); free(m->ty); free(m->cval); free(m->ptr_sc); free(m->ptr_pt); free(m->var_sc); free(m->var_pt); free(m->loc); free(m->root);
    free(m->cset); free(m->has_loc); free(m->builtin);
}

// Types, constants, global variables, Location / BuiltIn decorations, pointer roots (OpVariable / access chains / OpCopyObject).
static int mod_read(const uint32_t *w, size_t n, mod *m, unsigned *flags) {
    memset(m, 0, sizeof *m); m->model = ~0u;
    if (!w || n < 5 || w[0] != 0x07230203u || w[3] == 0 || w[3] > (1u << 22)) return -1;
    const uint32_t b = m->bound = w[3];
    m->tyop = calloc(b, 4); m->ty = calloc(2 * (size_t)b, 4); m->cval = calloc(b, 4); m->ptr_sc = calloc(b, 4); m->ptr_pt = calloc(b, 4);
    m->var_sc = calloc(b, 4); m->var_pt = calloc(b, 4); m->loc = calloc(b, 4); m->root = calloc(b, 4);
    m->cset = calloc(b, 1); m->has_loc = calloc(b, 1); m->builtin = calloc(b, 1);
    if (!m->tyop || !m->ty || !m->cval || !m->ptr_sc || !m->ptr_pt || !m->var_sc || !m->var_pt || !m->loc || !m->root || !m->cset || !m->has_loc || !m->builtin) return -1;
    for (uint32_t i = 0; i < b; i++) m->var_sc[i] = ~0u;
    int in_fn = 0;
    for (size_t i = 5; i < n;) {
        const uint32_t c = w[i] >> 16, op = w[i] & 0xffff;
        if (!c || i + c > n) return -1;
        const uint32_t *a = w + i + 1;
        if (op == OpFunction) { if (!m->first_fn) m->first_fn = i; in_fn = 1; }
        else if (op == 56) in_fn = 0;
        if (op == OpEntryPoint && c >= 3 && m->model == ~0u) m->model = a[0];
        else if ((op == OpTypeInt || op == OpTypeFloat) && c >= 3 && a[0] < b) { m->tyop[a[0]] = op; m->ty[2 * a[0]] = a[1]; }
        else if ((op == OpTypeVector || op == OpTypeArray) && c >= 4 && a[0] < b) { m->tyop[a[0]] = op; m->ty[2 * a[0]] = a[1]; m->ty[2 * a[0] + 1] = a[2]; }
        else if (op >= 19 && op <= 39 && op != OpTypePointer && c >= 2 && a[0] < b) m->tyop[a[0]] = op;   // other types (struct, matrix ...)
        else if (op == OpTypePointer && c >= 4 && a[0] < b) { m->tyop[a[0]] = op; m->ptr_sc[a[0]] = a[1]; m->ptr_pt[a[0]] = a[2]; }
        else if (op == OpConstant && c >= 4 && a[1] < b) { m->cval[a[1]] = a[2]; m->cset[a[1]] = 1; }
        else if (op == OpVariable && c >= 4 && a[1] < b && a[0] < b) {
            m->var_sc[a[1]] = a[2]; m->var_pt[a[1]] = m->ptr_pt[a[0]]; m->root[a[1]] = a[1];
        } else if (op == OpDecorate && c >= 4 && a[0] < b) {
            if (a[1] == DecLocation) { m->has_loc[a[0]] = 1; m->loc[a[0]] = a[2]; }
            else if (a[1] == DecBuiltIn) m->builtin[a[0]] = 1;
        } else if (op == OpMemberDecorate && c >= 5 && a[0] < b) {
            if (a[2] == DecLocation) *flags |= NR_MAT_COMPLEX_IO;          // Location on block members: not handled
            else if (a[2] == DecBuiltIn) m->builtin[a[0]] = 2;              // the struct type carries built-in members
        } else if (in_fn && (op == OpAccessChain || op == OpInBoundsAccessChain || op == OpPtrAccessChain || op == OpInBoundsPtrAccessChain) && c >= 4 && a[1] < b && a[2] < b) {
            m->root[a[1]] = m->root[a[2]];
        } else if (in_fn && op == OpCopyObject && c >= 4 && a[1] < b && a[2] < b && m->root[a[2]]) m->root[a[1]] = m->root[a[2]];
        i += c;
    }
    return 0;
}

int nr_mat_scan(const uint32_t *w, size_t n, nr_mat_facts *f) {
    memset(f, 0, sizeof *f); f->model = ~0u;
    mod m; unsigned fl = 0;
    if (mod_read(w, n, &m, &fl)) { mod_free(&m); f->flags = NR_MAT_BAD; return -1; }
    f->model = m.model;
    for (uint32_t v = 0; v < m.bound; v++) {
        if (m.var_sc[v] != SC_Input && m.var_sc[v] != SC_Output) continue;
        const uint32_t pt = m.var_pt[v];
        if (m.var_sc[v] == SC_Output && m.model == 4 && (m.builtin[v] || (pt < m.bound && m.builtin[pt] == 2))) fl |= NR_MAT_FRAGOUT;
        if (!m.has_loc[v]) continue;
        int cx = 0; const uint32_t s = span_of(m.ty, m.tyop, m.cval, m.cset, m.bound, pt, &cx);
        if (cx) fl |= NR_MAT_COMPLEX_IO;
        const uint32_t l = m.loc[v];
        for (uint32_t k = 0; k < s && l + k < 32; k++) { if (m.var_sc[v] == SC_Input) f->in_mask |= 1u << (l + k); else f->out_mask |= 1u << (l + k); }
        if (l + s > 32) fl |= NR_MAT_COMPLEX_IO;
    }
    for (size_t i = 5; i < n;) {
        const uint32_t c = w[i] >> 16, op = w[i] & 0xffff;
        const uint32_t *a = w + i + 1;
        if (op == OpKill || op == OpTerminateInvocation || op == OpDemote) fl |= NR_MAT_KILL;
        if (op == OpImageWrite || (op >= 227 && op <= 242) || op == 4417 || op == 5359 || op == 6035 || op == 6036 || op == OpImageTexelPointer) fl |= NR_MAT_SIDE;
        if (i >= m.first_fn && m.first_fn) {
            uint32_t ptr = 0;
            if (op == OpStore && c >= 3) ptr = a[0];
            else if ((op == OpCopyMemory || op == OpCopyMemorySized) && c >= 3) ptr = a[0];
            if (ptr) {
                const uint32_t r = ptr < m.bound ? m.root[ptr] : 0, sc = r ? m.var_sc[r] : ~0u;
                if (sc != SC_Function && sc != SC_Private && sc != SC_Output) fl |= NR_MAT_SIDE;   // storage / workgroup / unknown root
            }
        }
        i += c;
    }
    f->flags = fl; mod_free(&m);
    return 0;
}

int nr_mat_fs_pure(const nr_mat_facts *f) { return f && f->model == 4 && !(f->flags & (NR_MAT_KILL | NR_MAT_FRAGOUT | NR_MAT_SIDE | NR_MAT_BAD)); }

static const uint32_t k_empty_fs[] = {
    0x07230203u, 0x00010000u, 0u, 5u, 0u,
    (2u << 16) | 17u, 1u,                                   // OpCapability Shader
    (3u << 16) | 14u, 0u, 1u,                               // OpMemoryModel Logical GLSL450
    (5u << 16) | 15u, 4u, 1u, 0x6e69616du, 0u,              // OpEntryPoint Fragment %1 "main"
    (3u << 16) | 16u, 1u, 7u,                               // OpExecutionMode %1 OriginUpperLeft
    (2u << 16) | 19u, 2u,                                   // %2 = OpTypeVoid
    (3u << 16) | 33u, 3u, 2u,                               // %3 = OpTypeFunction %2
    (5u << 16) | 54u, 2u, 1u, 0u, 3u,                       // %1 = OpFunction %2 None %3
    (2u << 16) | 248u, 4u,                                  // %4 = OpLabel
    (1u << 16) | 253u,                                      // OpReturn
    (1u << 16) | 56u,                                       // OpFunctionEnd
};
const uint32_t *nr_mat_empty_fs(size_t *n) { if (n) *n = sizeof k_empty_fs / 4; return k_empty_fs; }

uint64_t nr_mat_fnv64(const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data; uint64_t x = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { x ^= p[i]; x *= 0x100000001b3ull; }
    return x;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

const char *nr_mat_gate_parse(const uint8_t *b, size_t n, nr_mat_gate *g) {
    memset(g, 0, sizeof *g);
    if (!b || n < 24) return "short record";
    if (rd32(b) != 0x314d4758u) return "bad magic";
    if (rd32(b + 4) != 1) return "another version";
    memcpy(&g->spirv_fnv, b + 8, 8); g->paths = rd32(b + 16); g->terms = rd32(b + 20);
    if (!g->paths || g->paths > NR_MAT_GATE_MAX_PATHS || g->terms > NR_MAT_GATE_MAX_TERMS) return "too many paths / terms";
    if (n != 24 + 16 * (size_t)g->paths + 16 * (size_t)g->terms) return "size differs from its counts";
    const uint8_t *p = b + 24;
    for (uint32_t i = 0; i < g->paths; i++, p += 16) {
        nr_mat_path *x = &g->path[i]; x->w_bits = rd32(p); x->w_known = rd32(p + 4); x->first = rd32(p + 8); x->count = rd32(p + 12);
        if (x->w_known > 1 || x->first > g->terms || x->count > g->terms - x->first) return "path outside its terms";
    }
    for (uint32_t i = 0; i < g->terms; i++, p += 16) {
        nr_mat_term *t = &g->term[i]; t->bank_word = rd32(p); t->op = rd32(p + 4); t->cbits = rd32(p + 8); t->flags = rd32(p + 12);
        if (t->op < 170 || t->op > 191 || t->flags > 3) return "unknown comparison";
        if ((t->bank_word >> 16) < 3 || (t->bank_word >> 16) - 3 >= 16) return "uniform bank outside slots 0..15";
    }
    return 0;
}

static float bits_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

int nr_mat_cmp(uint32_t op, uint32_t a, uint32_t b) {
    if (op >= 180 && op <= 191) {
        const float x = bits_f(a), y = bits_f(b); const int nan = isnan(x) || isnan(y), unord = (op - 180) & 1;
        int r;
        switch ((op - 180) >> 1) { case 0: r = x == y; break; case 1: r = x != y; break; case 2: r = x < y; break; case 3: r = x > y; break;
                                   case 4: r = x <= y; break; default: r = x >= y; break; }
        return unord ? (r || nan) : (r && !nan);
    }
    const int32_t sa = (int32_t)a, sb = (int32_t)b;
    switch (op) {
    case 170: return a == b; case 171: return a != b; case 172: return a > b; case 173: return sa > sb; case 174: return a >= b;
    case 175: return sa >= sb; case 176: return a < b; case 177: return sa < sb; case 178: return a <= b; case 179: return sa <= sb;
    default: return -1;
    }
}

int nr_mat_gate_eval(const nr_mat_gate *g, nr_mat_read_fn read, void *ctx) {
    if (!g || !read) return -1;
    for (uint32_t p = 0; p < g->paths && p < NR_MAT_GATE_MAX_PATHS; p++) {
        const nr_mat_path *x = &g->path[p]; int ok = 1;
        for (uint32_t k = x->first; ok && k < x->first + x->count && k < NR_MAT_GATE_MAX_TERMS; k++) {
            const nr_mat_term *t = &g->term[k]; uint32_t u;
            if (!read(ctx, t->bank_word, &u)) { ok = 0; break; }
            const int r = (t->flags & 1) ? nr_mat_cmp(t->op, u, t->cbits) : nr_mat_cmp(t->op, t->cbits, u);
            if (r < 0 || r != !!(t->flags & 2)) ok = 0;
        }
        if (ok) return (int)p;
    }
    return -1;
}

int nr_mat_alpha_pass(uint32_t func, uint32_t a_bits, float ref) {
    static const uint32_t op[9] = {0, 0, 184, 180, 188, 186, 182, 190, 0};   // ordered, as the alpha rewrite emits them (NOTEQUAL ordered too)
    uint32_t r; memcpy(&r, &ref, 4);
    if (func == 8) return 1;
    if (func < 2 || func > 7) return 0;   // NEVER (and anything unknown): killed
    return nr_mat_cmp(op[func], a_bits, r) == 1;
}

int nr_mat_prune(const uint32_t *w, size_t n, uint32_t keep, uint32_t *out, size_t *out_n, uint32_t *pruned) {
    if (pruned) *pruned = 0;
    if (out_n) *out_n = 0;
    mod m; unsigned fl = 0;
    if (mod_read(w, n, &m, &fl)) { mod_free(&m); return -1; }
    if (fl & NR_MAT_COMPLEX_IO) { mod_free(&m); return -3; }
    uint8_t *cut = calloc(m.bound, 1);   // 1 = an Output variable whose stores go
    if (!cut) { mod_free(&m); return -1; }
    uint32_t pm = 0; int any = 0;
    for (uint32_t v = 0; v < m.bound; v++) {
        if (m.var_sc[v] != SC_Output || !m.has_loc[v] || m.builtin[v]) continue;
        int cx = 0; const uint32_t s = span_of(m.ty, m.tyop, m.cval, m.cset, m.bound, m.var_pt[v], &cx);
        if (cx || m.loc[v] + s > 32) { free(cut); mod_free(&m); return -3; }
        const uint32_t mask = (s >= 32 ? ~0u : ((1u << s) - 1u)) << m.loc[v];
        if (!(mask & keep)) { cut[v] = 1; pm |= mask; any = 1; }
    }
    if (!any) { free(cut); mod_free(&m); return -4; }
#define CUT(id) ((id) < m.bound && m.root[(id)] && cut[m.root[(id)]])
    int removed = 0; size_t o = 0;
    for (size_t i = 0; i < n;) {
        const uint32_t c = i < 5 ? 5 : w[i] >> 16, op = i < 5 ? 0 : w[i] & 0xffff;
        const uint32_t *a = w + i + 1;
        int drop = 0, bad = 0;
        if (i >= 5 && m.first_fn && i >= m.first_fn) {
            if (op == OpStore && c >= 3) { if (CUT(a[0])) drop = 1; if (CUT(a[1])) bad = 1; }
            else if (op == OpLoad && c >= 4) bad = CUT(a[2]);
            else if ((op == OpCopyMemory || op == OpCopyMemorySized) && c >= 3) bad = CUT(a[0]) || CUT(a[1]);
            else if (op == OpExtInst && c >= 5) { for (uint32_t k = 4; k < c - 1 && !bad; k++) bad = CUT(a[k]); }
            else if (op == OpFunctionCall && c >= 4) { for (uint32_t k = 3; k < c - 1 && !bad; k++) bad = CUT(a[k]); }
            else if (op == OpSelect && c >= 6) bad = CUT(a[3]) || CUT(a[4]);
            else if (op == OpPhi && c >= 3) { for (uint32_t k = 2; k < c - 1 && !bad; k += 2) bad = CUT(a[k]); }
            else if (op == OpImageTexelPointer && c >= 4) bad = CUT(a[2]);
        }
        if (bad) { free(cut); mod_free(&m); return -2; }
        if (drop) ++removed;
        else { memmove(out + o, w + i, (size_t)c * 4); o += c; }
        i += c;
    }
#undef CUT
    free(cut); mod_free(&m);
    if (!removed) return -4;
    if (out_n) *out_n = o;
    if (pruned) *pruned = pm;
    return removed;
}
