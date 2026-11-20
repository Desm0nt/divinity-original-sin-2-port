// [DOS2] NR-NATIVE step D: host test of dm/gfx/nr/nr_nrn_d.h (docs/dos2/nr/NR-NATIVE-D.md).
//   clang -O2 -Wall -Wextra -I dm/gfx/nr -I dm/gfx/nvn dm/gfx/vulkan/engine/tests/nr_nrn_d_test.c -o nr_nrn_d_test && ./nr_nrn_d_test
//   mutations: -DNRND_MUTATE=1..12 - each must FAIL.
// (1) the list: AllocCommand 0x941720 and the walker 0xac1f80 as the machine code does them (alignment byte, chunks); the scan of a
//     group finds every packet the walker executes, at the same addresses, over memory full of old bytes;
// (2) the note: a packet of unknown length / a nested note / two draws / DrawArrays end without FULL or DRAW; the pass of a context;
// (3) the chain: a model of the engine (renderer cache with the card's bytes and bookkeeping words that move at every draw, handlers
//     of packets 21 / 15 / 17 / 28 / the two-byte ones / targets / viewport, texture records that are re-made) builds lists the way
//     the engine's functions do, with the wrapper around them, and executes them; at EVERY draw the card the memo answers must be
//     nrn_card_of of the cache (the path without the note), and the note's pass the builder's; frames repeat, the order of the queue
//     changes, foreign packets, unwrapped classes, draws whose card is not taken, list changes and direct cache writes in between;
// (4) D-b: the record composed directly against nbd_pack + nrd_tail_put + nrn_tail_put + nrn_short_ext_put + nrd_tail_ext_put, byte
//     for byte.
#include <stdio.h>
#include <stdlib.h>
#include "../../../nr/nr_nrn_d.h"
#include "../../../nvn/nvn_nb_draw.h"

static uint64_t g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { if (g_fail++ < 12) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static uint64_t g_seed = 0x243f6a8885a308d3ull;
static uint64_t rnd(void) { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17; return g_seed; }
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }

// ---- the engine's list (0x941720 / 0xac1f80) ----
typedef struct chunk { uint8_t *data; uint32_t cap, used, count; struct chunk *next; } chunk;
typedef struct list { chunk *first, *cur; uint32_t min; uint8_t record; } list;
static chunk *chunk_new(uint32_t cap) {
    chunk *c = (chunk *)calloc(1, sizeof *c);
    c->cap = (cap + 0x3f) & ~0x3fu; { uint8_t *raw = (uint8_t *)malloc(c->cap + 0x40); c->data = raw + (0x40 - ((uintptr_t)raw & 0x3f)); }
    for (uint32_t i = 0; i < c->cap; i++) c->data[i] = (uint8_t)rnd();   // old bytes: the writers do not fill every byte
    return c;
}
static void list_init(list *l, uint32_t min) { memset(l, 0, sizeof *l); l->min = min; l->first = l->cur = chunk_new(min); l->record = 1; }
static void list_reset(list *l) { for (chunk *c = l->first; c; c = c->next) { c->used = 0; c->count = 0; for (uint32_t i = 0; i < c->cap; i++) c->data[i] = (uint8_t)rnd(); } l->cur = l->first; }
static uint8_t *list_alloc(list *l, uint32_t size, uint32_t align) {   // FUN_00941720
    chunk *c = l->cur; uint32_t used = c->used;
    uint32_t pad = ((align + used - 1) & ~(align - 1)) - used; if (!pad) pad = align;
    uint32_t end = used + size + pad;
    if (c->cap < end) {
        end = align + size;
        chunk *n = c->next;
        if (!n || n->cap < end) { chunk *x = chunk_new(end > l->min ? end : l->min); c->next = x; x->next = n; n = x; }
        l->cur = c = n; used = 0; pad = align;
    }
    c->data[used] = (uint8_t)align; c->used = end; c->count++;
    return c->data + used + pad;
}
typedef uint32_t (*exec_fn)(void *u, list *l, uint8_t *pkt);
static void list_walk(list *l, exec_fn ex, void *u) {   // FUN_00ac1f80
    for (chunk *c = l->first; c; c = c->next) {
        uint8_t *p = c->data;
        for (uint32_t k = 0; k < c->count; k++) {
            const uintptr_t a = *p, r = (uintptr_t)p % a;
            p += r ? a - r : a;
            p += ex(u, l, p);
        }
        if (c == l->cur) break;
    }
}

// ---- the engine: renderer cache, texture records, handlers ----
enum { R_BYTES = 0x1900, NTEXREC = 64, NDESC = 24 };
static uint8_t R[R_BYTES];
static struct { uint32_t id; } g_texrec[NTEXREC];          // a texture record: the pool id the handler of packet 15 reads at execution
static struct { uint8_t stage, slot; } g_desc[NDESC];       // a shader's texture description: where the texture goes
static uint64_t g_epoch;
static void w64(uint32_t off, uint64_t v) { memcpy(R + off, &v, 8); }
static void w32(uint32_t off, uint32_t v) { memcpy(R + off, &v, 4); }
static uint64_t g_noise;
static void noise(void) {   // the words the card does not read move at every packet
    g_noise++;
    w64(NRN_R_VB_DIRTY, g_noise); w64(NRN_R_IB_FIRST, g_noise * 3); w64(NRN_R_SHADER_REC, g_noise * 5); w32(NRN_R_REN, (uint32_t)g_noise);
}
static uint32_t engine_exec(uint8_t *p) {   // every opcode but the note and the draws; returns the length
    const unsigned op = p[0]; uint64_t a, b; uint32_t x;
    noise();
    switch (op) {
    case NRND_PK_STATE:
        memcpy(&a, p + 8, 8); w64(NRN_R_SHADER, a); memcpy(&a, p + 0x10, 8); w64(0x16f0, a); memcpy(&x, p + 0x18, 4); w32(0x1730, x);
        memcpy(&a, p + 0x20, 8); w64(0x1708, a); memcpy(&a, p + 0x28, 8); w64(0x16f8, a); memcpy(&a, p + 0x30, 8); w64(0x1740, a);
        for (int k = 0; k < 3; k++) { memcpy(&a, p + 0x38 + 8 * k, 8); w64(NRN_R_VB + 8 * (uint32_t)k, a); }
        memcpy(&a, p + 0x50, 8); w64(NRN_R_IB, a); R[0x1734] = p[0x58]; memcpy(&a, p + 0x60, 8); w64(0x1860, a); memcpy(&x, p + 0x68, 4); w32(NRN_R_ALPHA, x);
        return 0x70;
    case NRND_PK_TEX: {
        memcpy(&a, p + 8, 8); memcpy(&b, p + 0x10, 8); memcpy(&x, p + 0x18, 4);
        const unsigned d = (unsigned)a % NDESC; const uint32_t id = g_texrec[b % NTEXREC].id + x;
        w32((g_desc[d].stage ? NRN_R_TEX_FS : NRN_R_TEX_VS) + 4u * g_desc[d].slot, id);
        return 0x20; }
    case NRND_PK_SAMP: memcpy(&a, p + 8, 8); memcpy(&b, p + 0x10, 8); w32(NRN_R_SMP_FS + 4u * (uint32_t)(a % 32), (uint32_t)b); return 0x18;
    case NRND_PK_PARAM: memcpy(&x, p + 0x28, 4); return x + 0x30;                          // shadow blocks only
    case 0x3a: R[0x1738] = p[1]; return 2;                                                  // (model: a two-byte packet that lands in the card's bytes)
    case 0x39: R[0x1739] = p[1]; return 2;
    case 0x3d: case 0x3e: return 0x10;                                                      // queries: NVN only
    case 0x12: memcpy(&x, p + 4, 4); w32(0x171c, x); return 0x14;                           // viewport (a pass-level packet: foreign)
    case 0x1b: return 1;
    case 0x16: memcpy(&x, p + 4, 4); memcpy(&a, p + 8, 8); w64(NRN_R_RT, a); return x;       // targets: variable length (the model keeps it at +4)
    default: printf("model: opcode %#x\n", op); exit(2);
    }
}

// ---- the builders ----
typedef struct mat { uint64_t shader, raster, ds, blend, vfmt, vb[3], ib, samp[2], sname[2]; uint32_t rarg, alpha; uint8_t itype, ntex, nsamp, two, post; uint64_t tdesc[3], trec[3]; } mat;
enum { NMAT = 48 };
static mat g_mat[NMAT];
static void mats(void) {
    for (int i = 0; i < NMAT; i++) { mat *m = &g_mat[i];
        m->shader = 0x100000000ull + rn(9); m->raster = 0x200000000ull + rn(3); m->ds = 0x300000000ull + rn(3); m->blend = 0x400000000ull + rn(4); m->vfmt = 0x500000000ull + rn(4);
        for (int k = 0; k < 3; k++) m->vb[k] = k < 1 + (int)rn(2) ? 0x600000000ull + rn(20) : 0;
        m->ib = 0x700000000ull + rn(20); m->rarg = rn(2); m->alpha = rn(2) ? 0xbf800000u : 0x3f000000u; m->itype = (uint8_t)rn(2);
        m->ntex = (uint8_t)rn(4); m->nsamp = (uint8_t)rn(3); m->two = (uint8_t)rn(3); m->post = (uint8_t)(rn(5) == 0);
        for (int k = 0; k < 3; k++) { m->tdesc[k] = rn(NDESC); m->trec[k] = rn(NTEXREC); }
        for (int k = 0; k < 2; k++) { m->samp[k] = 0x800000000ull + rn(6); m->sname[k] = rn(32); } }
    for (int d = 0; d < NDESC; d++) { g_desc[d].stage = (uint8_t)rn(2); g_desc[d].slot = (uint8_t)rn(32); }
    for (int t = 0; t < NTEXREC; t++) g_texrec[t].id = 1 + rn(4000);
}
// what a draw-yourself function appends for material m; how: 0 one indexed draw, 1 DrawArrays, 2 no draw, 3 with a packet whose
// length the scan does not know (targets), 4 two draws
static uint8_t *group(list *l, const mat *m, int how, uint32_t obj_seed) {
    uint8_t *p, *draw = NULL;
    if (m->two == 1) { p = list_alloc(l, 2, 1); p[0] = 0x3a; p[1] = (uint8_t)(m->rarg & 1); }
    p = list_alloc(l, 0x70, 8); p[0] = NRND_PK_STATE;
    memcpy(p + 8, &m->shader, 8); memcpy(p + 0x10, &m->raster, 8); memcpy(p + 0x18, &m->rarg, 4); memcpy(p + 0x20, &m->ds, 8); memcpy(p + 0x28, &m->blend, 8);
    memcpy(p + 0x30, &m->vfmt, 8); memcpy(p + 0x38, m->vb, 24); memcpy(p + 0x50, &m->ib, 8); p[0x58] = m->itype; memset(p + 0x60, 0, 8); memcpy(p + 0x68, &m->alpha, 4);
    { const uint32_t n = 0x40; p = list_alloc(l, 0x30 + n, 8); p[0] = NRND_PK_PARAM; memcpy(p + 0x28, &n, 4); for (uint32_t i = 0; i < n; i++) p[0x30 + i] = (uint8_t)(obj_seed + i + rnd()); }   // the object matrix: other bytes every frame
    for (int k = 0; k < m->ntex; k++) { const uint32_t z = 0; p = list_alloc(l, 0x20, 8); p[0] = NRND_PK_TEX; memcpy(p + 8, &m->tdesc[k], 8); memcpy(p + 0x10, &m->trec[k], 8); memcpy(p + 0x18, &z, 4); }
    for (int k = 0; k < m->nsamp; k++) { p = list_alloc(l, 0x18, 8); p[0] = NRND_PK_SAMP; memcpy(p + 8, &m->sname[k], 8); memcpy(p + 0x10, &m->samp[k], 8); }
    if (how == 3) { const uint32_t n = 0x18; const uint64_t rt = 0x900000000ull + rn(3); p = list_alloc(l, n, 8); p[0] = 0x16; memcpy(p + 4, &n, 4); memcpy(p + 8, &rt, 8); }
    if (how == 1) { p = list_alloc(l, 0xc, 4); p[0] = NRND_PK_DRAW_ARRAYS; }
    else if (how != 2) { p = list_alloc(l, 0x14, 4); p[0] = NRND_PK_DRAW; draw = p; if (how == 4) { p = list_alloc(l, 0x14, 4); p[0] = NRND_PK_DRAW; } }
    if (m->post) { p = list_alloc(l, 2, 1); p[0] = 0x39; p[1] = (uint8_t)((m->shader & 3) + 1); }
    return draw;
}
// the wrapper of hle_dos2_nrn_d.inc (nrnd_begin / nrnd_end) on this list model
static uint8_t *wrapped(list *l, const mat *m, int how, uint32_t cflags, uint32_t stage, unsigned sub, uint32_t seed, nrnd_note **out) {
    uint8_t *np = list_alloc(l, NRND_NOTE_BYTES, NRND_NOTE_ALIGN);
    nrnd_note n; nrnd_note_init(&n, NRND_K_COMMON, cflags, 0x10, stage, sub, seed); memcpy(np, &n, sizeof n);
    chunk *c0 = l->cur; const uint32_t at0 = c0->used;
    uint8_t *draw = group(l, m, how, seed);
    nrnd_seg seg[8]; unsigned ns = 0;
    for (chunk *c = c0;; c = c->next) { seg[ns].base = c->data; seg[ns].at = c == c0 ? at0 : 0; seg[ns].end = c->used; ns++; if (c == l->cur) break; }
    memcpy(&n, np, sizeof n); nrnd_scan(&n, seg, ns); memcpy(np, &n, sizeof n);
    if (out) *out = (nrnd_note *)np;
    return draw;
}

// ---- execution: the render thread ----
typedef struct truth { uint8_t *draw; int pass; uint8_t noted, take; } truth;   // per draw packet: the builder's pass, wrapped, the card is taken (a direct draw)
static truth g_truth[8192]; static unsigned g_ntruth;
static nrnd_run g_run; static nrnd_memo *g_memo;
static uint64_t g_hits, g_keys, g_draws, g_passes;
static uint32_t run_exec(void *u, list *l, uint8_t *p) {
    (void)u;
    const unsigned op = p[0];
    if (op == NRND_OP) { nrnd_run_note(&g_run, (uint64_t)(uintptr_t)l, (const nrnd_note *)p); return NRND_NOTE_BYTES; }   // dn_rc_hook: before every handler
    nrnd_run_pkt(&g_run, op, (uint64_t)(uintptr_t)p);                                                                    // nr_rc_pre
    if (op == NRND_PK_DRAW_ARRAYS) { noise(); return 0xc; }
    if (op != NRND_PK_DRAW) return engine_exec(p);
    noise();
    const truth *t = NULL;
    for (unsigned i = 0; i < g_ntruth; i++) if (g_truth[i].draw == p) t = &g_truth[i];
    if (!t || !t->take) return 0x14;   // not a direct draw: nrn_pre does not run
    const nrnd_note *n = nrnd_run_take(&g_run);   // nrn_pre
    g_draws++;
    CHECK((n != NULL) == (t->noted != 0), "a draw %s its note", n ? "of an unwrapped function took a note" : "did not find");
    if (n) { const int pn = nrnd_pass_of(n->cflags, n->stage); if (pn >= 0) { g_passes++; CHECK(pn == t->pass, "pass %d by the note, %d by the builder", pn, t->pass); } }
    const uint64_t card = nrn_card_of(R, NRN_W_DEFAULT);
    const uint64_t key = nrnd_chain_key(&g_run, n, g_epoch);
    if (key) { g_keys++;
        const uint64_t pred = nrnd_memo_get(g_memo, key);
        if (pred) { g_hits++; CHECK(pred == card, "the memo's card %016llx is not the cache's %016llx (draw %llu)", (unsigned long long)pred, (unsigned long long)card, (unsigned long long)g_draws); }
        nrnd_memo_put(g_memo, key, card); }
    nrnd_run_drawn(&g_run, n, card);
    return 0x14;
}

static void t_list(void) {
    for (int round = 0; round < 60; round++) {
        list l; list_init(&l, 0x100 + rn(0x300));
        uint8_t *want[400]; unsigned nw = 0, npk = 0;
        uint8_t *np = list_alloc(&l, NRND_NOTE_BYTES, NRND_NOTE_ALIGN); (void)np;
        chunk *c0 = l.cur; const uint32_t at0 = c0->used;
        static const struct { uint8_t op, len, al; } k[] = {{0x15, 0x70, 8}, {0x0f, 0x20, 8}, {0x11, 0x18, 8}, {0x3a, 2, 1}, {0x3d, 0x10, 8}, {0x12, 0x14, 4}, {0x1b, 1, 1}, {0x24, 0x14, 4}};
        int drew = 0;
        for (int i = 0; i < 40 + (int)rn(60); i++) {
            unsigned j = rn(8); if (j == 7) { if (drew) j = 0; else drew = 1; }
            uint8_t *p = list_alloc(&l, k[j].len, k[j].al); p[0] = k[j].op; want[nw++] = p; npk++;
            if (rn(6) == 0) { const uint32_t n = 4 * rn(40); p = list_alloc(&l, 0x30 + n, 8); p[0] = NRND_PK_PARAM; memcpy(p + 0x28, &n, 4); want[nw++] = p; npk++; }
        }
        // the scan visits exactly the packets the allocator returned, in order
        unsigned got = 0;
        for (chunk *c = c0;; c = c->next) {
            uint32_t at = c == c0 ? at0 : 0;
            while (at < c->used) { const uint32_t p = nrnd_pkt_at(c->data, at, c->used); CHECK(p != 0, "the scan lost the packets");
                if (!p) break;
                CHECK(got < nw && c->data + p == want[got], "packet %u is not where AllocCommand put it", got); got++;
                const uint32_t len = nrnd_pkt_len(c->data + p); CHECK(len != 0, "length of %#x", c->data[p]); at = p + len; }
            if (c == l.cur) break;
        }
        CHECK(got == nw, "the scan saw %u of %u packets", got, nw);
        nrnd_seg seg[8]; unsigned ns = 0;
        for (chunk *c = c0;; c = c->next) { seg[ns].base = c->data; seg[ns].at = c == c0 ? at0 : 0; seg[ns].end = c->used; ns++; if (c == l.cur) break; }
        nrnd_note n; memset(&n, 0, sizeof n); nrnd_scan(&n, seg, ns);
        CHECK(n.npkt == npk && (n.flags & NRND_F_FULL), "npkt %u of %u, flags %#x", n.npkt, npk, n.flags);
        CHECK(((n.flags & NRND_F_DRAW) != 0) == (drew != 0), "the draw packet");
    }
}
static void t_note(void) {
    mats();
    list l; list_init(&l, 0x4000);
    nrnd_note *n = NULL;
    uint8_t *d = wrapped(&l, &g_mat[0], 0, 2, NRND_VT_CASCADE, 1, 7, &n);
    CHECK((n->flags & (NRND_F_DONE | NRND_F_FULL | NRND_F_DRAW)) == (NRND_F_DONE | NRND_F_FULL | NRND_F_DRAW) && n->draw == (uint64_t)(uintptr_t)d, "a plain group: flags %#x", n->flags);
    wrapped(&l, &g_mat[0], 1, 2, NRND_VT_CASCADE, 1, 7, &n); CHECK((n->flags & NRND_F_FULL) && !(n->flags & NRND_F_DRAW), "DrawArrays: flags %#x", n->flags);
    wrapped(&l, &g_mat[0], 2, 2, NRND_VT_CASCADE, 1, 7, &n); CHECK((n->flags & NRND_F_FULL) && !(n->flags & NRND_F_DRAW), "no draw: flags %#x", n->flags);
    wrapped(&l, &g_mat[0], 3, 2, NRND_VT_CASCADE, 1, 7, &n); CHECK(!(n->flags & (NRND_F_FULL | NRND_F_DRAW)) && (n->flags & NRND_F_DONE), "a packet of unknown length: flags %#x", n->flags);
    wrapped(&l, &g_mat[0], 4, 2, NRND_VT_CASCADE, 1, 7, &n); CHECK((n->flags & NRND_F_FULL) && !(n->flags & NRND_F_DRAW), "two draws: flags %#x", n->flags);
    {   // a nested call: the outer group holds the inner note
        uint8_t *np = list_alloc(&l, NRND_NOTE_BYTES, NRND_NOTE_ALIGN); nrnd_note o; nrnd_note_init(&o, 0, 1, 8, 0, 0, 1); memcpy(np, &o, sizeof o);
        chunk *c0 = l.cur; const uint32_t at0 = c0->used;
        wrapped(&l, &g_mat[1], 0, 1, 0, 0, 2, &n);
        nrnd_seg s = {c0->data, at0, c0->used}; nrnd_scan(&o, &s, 1);
        CHECK(!(o.flags & (NRND_F_FULL | NRND_F_DRAW)), "the outer note of a nested call: flags %#x", o.flags);
        CHECK((n->flags & NRND_F_DRAW) != 0, "the inner note"); }
    CHECK(nrnd_pass_of(2, NRND_VT_CASCADE) == NRN_P_SHADOW && nrnd_pass_of(0x40000, NRND_VT_GBUF_STAGE) == NRN_P_PREPASS && nrnd_pass_of(1, NRND_VT_GBUF_STAGE) == NRN_P_GBUF, "passes");
    CHECK(nrnd_pass_of(2, 0) < 0 && nrnd_pass_of(0x20002, NRND_VT_GBUF_STAGE) < 0 && nrnd_pass_of(1, NRND_VT_CASCADE) < 0 && nrnd_pass_of(0x40000, 0) < 0, "a context that names no pass");
    // two groups that differ in one field differ in lpre; a parameter packet's bytes do not
    static const int off[] = {8, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68};
    for (unsigned f = 0; f < sizeof off / sizeof off[0]; f++) {
        uint8_t a[0x70], b[0x70]; memset(a, 0, sizeof a); a[0] = NRND_PK_STATE; memcpy(b, a, sizeof b); b[off[f]] ^= 1;
        CHECK(nrnd_mix_pkt(1, a, 0x70) != nrnd_mix_pkt(1, b, 0x70), "packet 21 +%#x is not in the hash", off[f]);
    }
    { uint8_t a[0x70], b[0x70]; memset(a, 0, sizeof a); a[0] = NRND_PK_STATE; memcpy(b, a, sizeof b); b[3] = 9; b[0x1c] = 9; b[0x5b] = 9; b[0x6c] = 9;
      CHECK(nrnd_mix_pkt(1, a, 0x70) == nrnd_mix_pkt(1, b, 0x70), "bytes the writers leave unwritten are in the hash"); }
}

// the frame: three lists (cascade, pre-pass, G-buffer) built from a queue of materials, executed in order
static void frame(list L[3], const unsigned *queue, unsigned nq, int shuffle_at, int foreign_every, int unwrapped_every, int untaken_every) {
    g_ntruth = 0;
    static const struct { uint32_t cflags, stage; int pass; } P[3] = {{2, NRND_VT_CASCADE, NRN_P_SHADOW}, {0x40000, NRND_VT_GBUF_STAGE, NRN_P_PREPASS}, {1, NRND_VT_GBUF_STAGE, NRN_P_GBUF}};
    for (int li = 0; li < 3; li++) {
        list *l = &L[li]; list_reset(l);
        { const uint32_t n = 0x18; const uint64_t rt = 0x900000000ull + (uint64_t)li; uint8_t *p = list_alloc(l, n, 8); p[0] = 0x16; memcpy(p + 4, &n, 4); memcpy(p + 8, &rt, 8); }   // the pass's targets
        for (unsigned i = 0; i < nq; i++) {
            unsigned q = queue[i];
            if (shuffle_at >= 0 && (int)i >= shuffle_at) q = queue[shuffle_at + (i - (unsigned)shuffle_at) * 7 % (nq - (unsigned)shuffle_at)];
            const mat *m = &g_mat[(q + (unsigned)li) % NMAT];
            if (foreign_every && i % (unsigned)foreign_every == 3) { const uint32_t v = rn(3); uint8_t *p = list_alloc(l, 0x14, 4); p[0] = 0x12; memcpy(p + 4, &v, 4); }
            truth t; memset(&t, 0, sizeof t); t.pass = P[li].pass; t.take = !(untaken_every && i % (unsigned)untaken_every == 5);
            if (unwrapped_every && i % (unsigned)unwrapped_every == 1) { t.draw = group(l, m, 0, q); t.noted = 0; }
            else { t.draw = wrapped(l, m, 0, P[li].cflags, P[li].stage, 0, q, NULL); t.noted = 1; }
            if (g_ntruth < 8192) g_truth[g_ntruth++] = t;
        }
    }
    for (int li = 0; li < 3; li++) list_walk(&L[li], run_exec, NULL);
}
static void t_chain(void) {
    mats();
    g_memo = (nrnd_memo *)calloc(1, sizeof *g_memo); memset(&g_run, 0, sizeof g_run); memset(R, 0, sizeof R);
    list L[3]; for (int i = 0; i < 3; i++) list_init(&L[i], 0x2000);
    unsigned queue[300]; for (unsigned i = 0; i < 300; i++) queue[i] = rn(NMAT * 3);
    for (int f = 0; f < 400; f++) {
        const int mode = f / 40;
        if (f % 40 == 20) for (int k = 0; k < 6; k++) { queue[rn(300)] = rn(NMAT * 3); }   // objects come and go
        if (mode >= 5 && f % 7 == 3) {   // a texture is re-made under the same record (another pool id): the engine event moves the epoch
            g_texrec[rn(NTEXREC)].id = 1 + rn(4000); g_epoch++; }
        if (mode >= 7 && f % 5 == 2) { w32(0x173c, rn(250)); }   // a direct write between the frames' lists (an immediate-mode call): the lists start with a foreign packet
        frame(L, queue, 300, mode >= 2 && (f & 1) ? 100 + (int)rn(100) : -1, mode >= 3 ? 17 : 0, mode >= 4 ? 13 : 0, mode >= 6 ? 11 : 0);
    }
    CHECK(g_draws > 300000 && g_keys * 10 > g_draws * 6, "the chain held for %llu of %llu draws", (unsigned long long)g_keys, (unsigned long long)g_draws);
    CHECK(g_hits * 10 > g_draws * 4, "the memo answered %llu of %llu draws", (unsigned long long)g_hits, (unsigned long long)g_draws);
    CHECK(g_passes * 10 > g_draws * 8, "passes named by the note: %llu of %llu", (unsigned long long)g_passes, (unsigned long long)g_draws);

    // explicit: A, B after X - and A not taken in between (the previous card is not this cache's)
    memset(&g_run, 0, sizeof g_run);
    for (int rep = 0; rep < 6; rep++) for (int variant = 0; variant < 2; variant++) {
        list *l = &L[0]; list_reset(l); g_ntruth = 0;
        { const uint32_t n = 0x18; const uint64_t rt = 0x900000007ull; uint8_t *p = list_alloc(l, n, 8); p[0] = 0x16; memcpy(p + 4, &n, 4); memcpy(p + 8, &rt, 8); }
        const int seq[2][3] = {{3 + rep, 9 + 3 * rep, -1}, {3 + rep, 5 + 2 * rep, 9 + 3 * rep}};
        for (int i = 0; i < 3; i++) { if (seq[variant][i] < 0) continue;
            truth t; memset(&t, 0, sizeof t); t.pass = NRN_P_SHADOW; t.noted = 1; t.take = !(variant == 1 && i == 1);
            t.draw = wrapped(l, &g_mat[seq[variant][i]], 0, 2, NRND_VT_CASCADE, 0, 1, NULL); g_truth[g_ntruth++] = t; }
        list_walk(l, run_exec, NULL);
    }
    // explicit: X, A, B and X, A', B where A' is A without its packet after the draw (lpost of the previous group is in the key)
    for (int rep = 0; rep < 4; rep++) for (int variant = 0; variant < 2; variant++) {
        list *l = &L[0]; list_reset(l); g_ntruth = 0;
        { const uint32_t n = 0x18; const uint64_t rt = 0x900000008ull; uint8_t *p = list_alloc(l, n, 8); p[0] = 0x16; memcpy(p + 4, &n, 4); memcpy(p + 8, &rt, 8); }
        mat x = g_mat[30 + rep], am = g_mat[34 + rep], bm = g_mat[38 + rep];
        x.post = 1; x.shader = 0x100000000ull; am.post = (uint8_t)(variant == 0); am.shader = 0x100000001ull; bm.post = 0;
        const mat *q[3] = {&x, &am, &bm};
        for (int i = 0; i < 3; i++) { truth t; memset(&t, 0, sizeof t); t.pass = NRN_P_SHADOW; t.noted = 1; t.take = 1;
            t.draw = wrapped(l, q[i], 0, 2, NRND_VT_CASCADE, 0, 1, NULL); g_truth[g_ntruth++] = t; }
        list_walk(l, run_exec, NULL);
    }
    // explicit: X, T and X, T' where T' is T with another texture record in its texture packet
    for (int rep = 0; rep < 4; rep++) for (int variant = 0; variant < 2; variant++) {
        list *l = &L[0]; list_reset(l); g_ntruth = 0;
        { const uint32_t n = 0x18; const uint64_t rt = 0x900000009ull; uint8_t *p = list_alloc(l, n, 8); p[0] = 0x16; memcpy(p + 4, &n, 4); memcpy(p + 8, &rt, 8); }
        mat z = g_mat[18 + rep], x = g_mat[10 + rep], tm = g_mat[14 + rep];   // z puts the slot back, so the cache at x is the same in both variants
        tm.ntex = 1; tm.nsamp = 0; tm.trec[0] = (uint64_t)(variant ? 5 : 6); g_texrec[5].id = 4100; g_texrec[6].id = 4200; g_texrec[7].id = 4300;
        z.ntex = 1; z.nsamp = 0; z.tdesc[0] = tm.tdesc[0]; z.trec[0] = 7; z.post = 0; x.ntex = 0; x.nsamp = 0; x.post = 0; x.two = 0; z.two = 0; tm.two = 0;
        const mat *q[3] = {&z, &x, &tm};
        for (int i = 0; i < 3; i++) { truth t; memset(&t, 0, sizeof t); t.pass = NRN_P_SHADOW; t.noted = 1; t.take = 1;
            t.draw = wrapped(l, q[i], 0, 2, NRND_VT_CASCADE, 0, 1, NULL); g_truth[g_ntruth++] = t; }
        list_walk(l, run_exec, NULL);
    }
    // explicit: the same list twice with a direct cache write in between and no foreign packet at its start (another list in between)
    for (int rep = 0; rep < 6; rep++) {
        for (int li = 0; li < 2; li++) { list *l = &L[li]; list_reset(l); if (!li) g_ntruth = 0;
            for (int i = 0; i < 3; i++) { truth t; memset(&t, 0, sizeof t); t.pass = NRN_P_SHADOW; t.noted = 1; t.take = 1;
                t.draw = wrapped(l, &g_mat[20 + i], 0, 2, NRND_VT_CASCADE, 0, 1, NULL); g_truth[g_ntruth++] = t; } }
        list_walk(&L[0], run_exec, NULL);
        w32(0x173c, 1000u + (uint32_t)rep);   // between the two lists
        list_walk(&L[1], run_exec, NULL);
    }
    // explicit: a note whose draw is not taken, then a draw of an unwrapped function - it must not take that note (the pass of another draw)
    for (int rep = 0; rep < 3; rep++) {
        list *l = &L[0]; list_reset(l); g_ntruth = 0;
        truth t; memset(&t, 0, sizeof t); t.pass = NRN_P_SHADOW; t.noted = 1; t.take = 0; t.draw = wrapped(l, &g_mat[1], 0, 2, NRND_VT_CASCADE, 0, 1, NULL); g_truth[g_ntruth++] = t;
        memset(&t, 0, sizeof t); t.pass = NRN_P_GBUF; t.noted = 0; t.take = 1; t.draw = group(l, &g_mat[2], 0, 2); g_truth[g_ntruth++] = t;
        list_walk(l, run_exec, NULL);
    }
    // the memo holds a key or it does not: another key of the same slot is no answer
    { nrnd_memo *m = (nrnd_memo *)calloc(1, sizeof *m); nrnd_memo_put(m, 0x1234500000001ull, 77);
      CHECK(nrnd_memo_get(m, 0x1234500000001ull) == 77, "the memo lost its key");
      CHECK(nrnd_memo_slot(0x1234500000001ull ^ ((uint64_t)NRND_MEMO << 3)) != nrnd_memo_slot(0x1234500000001ull) || 1, "-");
      uint64_t other = 0; for (uint64_t k = 2; k < 1u << 22; k++) if (nrnd_memo_slot(k) == nrnd_memo_slot(0x1234500000001ull)) { other = k; break; }
      CHECK(other && nrnd_memo_get(m, other) == 0, "the memo answered a key it does not hold"); free(m); }
}

// ---- D-b: the composed record against the functions the NVN half uses ----
static void t_rec(void) {
    for (int it = 0; it < 20000; it++) {
        nbd_shadow s; memset(&s, 0, sizeof s);
        const unsigned nb = rn(9);
        for (unsigned i = 0; i < nb; i++) nbd_add(&s, rn(2) ? NBD_B_UBO : NBD_B_TEX, (int)rn(6), (int)rn(16), rnd(), rnd() & 0xffff);
        s.h.engine_op = (uint8_t)(36 + rn(2)); s.h.draw = NBD_DRAW_IDX; s.h.texgen = (uint32_t)rnd(); s.h.flags = (uint8_t)(rn(2) ? NBD_F_REN | NBD_F_REN_ON : 0);
        s.h.prim = (int)rn(8); s.h.itype = (int)rn(3); s.h.count = (int)rn(90000); s.h.instances = 1 + (int)rn(30); s.h.ibuf = rnd(); s.h.cond_mode = 0;
        s.h._pad0 = 0x55; s.h._pad1 = 0x77777777u; s.h._pad2 = 0x66666666;   // nbd_pack zeroes the pads
        const uint32_t key = (uint32_t)rnd(), ts = (uint32_t)rnd(), ext = rn(3) ? (uint32_t)rnd() : 0, seq = (uint32_t)rnd(); const unsigned why = rn(65536), site = rn(256);
        const uint64_t card = rnd() | 1; const int pass = (int)rn(NRN_P_N); const int shrt = (int)rn(2); const unsigned rsv = (rn(2) ? NRN_T_READY : 0u) | (shrt ? (unsigned)NRN_T_SHORT : 0u);
        nrn_short_ext sx; memset(&sx, 0, sizeof sx); sx.st[0] = (uint8_t)rn(6); sx.st[1] = 0xff; sx.tm[0] = (uint32_t)rnd(); sx.m.have = (uint16_t)rnd(); sx.m.vb = (uint16_t)rnd();
        // the NVN half's way (nvn_nb_draw.inc nbd_draw)
        uint8_t a[4096], b[4096]; memset(a, 0, sizeof a); memset(b, 0, sizeof b);
        nbd_hdr *h = &s.h; h->flags |= NBD_F_NR; h->flags |= NRN_F_FLAG;
        const uint32_t size = nbd_size(&s) + (uint32_t)sizeof(nrd_tail) + (uint32_t)NRN_TAIL_BYTES + (shrt ? (uint32_t)NRN_SHORT_EXT_BYTES : 0u);
        nbd_pack(&s, a, size); nrd_tail_put(a, size, key, ts, why, site);
        nrn_tail_put(a, size, nrn_tail_off(&s.h), card, pass, seq, rsv);
        if (shrt) nrn_short_ext_put(a, size, nrn_tail_off(&s.h) + NRN_TAIL_BYTES, &sx);
        if (ext) nrd_tail_ext_put(a, size, ext);
        // composed
        nrnd_rec_in in; memset(&in, 0, sizeof in);
        in.engine_op = s.h.engine_op; in.draw = s.h.draw; in.flags = (uint8_t)(s.h.flags & ~(NBD_F_NR | NRN_F_FLAG)); in.nbind = s.h.nbind; in.texgen = s.h.texgen;
        in.prim = s.h.prim; in.itype = s.h.itype; in.count = s.h.count; in.instances = s.h.instances; in.ibuf = s.h.ibuf; in.binds = (const uint8_t *)s.bind;
        in.nr_key = key; in.nr_texset = ts; in.nr_why = (uint16_t)why; in.nr_site = (uint8_t)site; in.nr_ext = ext; in.card = card; in.pass = (uint8_t)pass; in.rsv = (uint16_t)rsv; in.seq = seq;
        in.sx = shrt ? &sx : NULL;
        const uint32_t n = nrnd_rec_put(b, sizeof b, &in);
        CHECK(n == size && nrnd_rec_size(&in) == size, "size %u / %u", n, size);
        CHECK(!memcmp(a, b, sizeof a), "the composed record is not the NVN half's, byte for byte (it %d, %u binds, short %d)", it, nb, shrt);
        CHECK(nrnd_rec_put(b, size - 1, &in) == 0, "a record that does not fit was written");
    }
}

int main(void) {
    for (int i = 0; i < 64; i++) CHECK(k_nrnd_len[i] == 0 || k_nrnd_len[i] <= 0xa0, "length table");
    t_list(); t_note(); t_chain(); t_rec();
    printf("%s nr_nrn_d_test: %llu checks, %llu failed (NRND_MUTATE=%d); chain: %llu draws, key for %llu, memo answered %llu, pass by the note %llu\n", g_fail ? "FAIL" : "PASS",
           (unsigned long long)g_checks, (unsigned long long)g_fail, NRND_MUTATE, (unsigned long long)g_draws, (unsigned long long)g_keys, (unsigned long long)g_hits, (unsigned long long)g_passes);
    return g_fail ? 1 : 0;
}
