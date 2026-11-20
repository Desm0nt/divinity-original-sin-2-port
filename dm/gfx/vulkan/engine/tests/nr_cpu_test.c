// [DOS2] NR-CPU (docs/dos2/nr/NR-CPU.md): host test + PC micro-benchmark of the per-draw CPU cuts in dm/gfx/nr/nr_cpu.h.
//   1 NR-TAPIMG  the tap's walk (a copy of nvn_nb_tap.inc nbt_reads + nbt_prog_draw over the real IR, nb_plan.c) with the slot image
//                cache gives the same IR as the walk through the 8 MB id table at every step - random binds, programs, passes, ops and
//                registrations of bound / unbound ids; the cache's answers equal the table's at every lookup; two mutations (no forget on
//                rebind, no generation bump on registration) are caught
//   2 NR-TAPSET  nb_read's membership test of the open pass through the hash set (nb_frame.rs_*, nb_plan.c) against the scan: three
//                IRs (scan, set, set + scan compared) from one random stream of passes, draws reading 1-12 images out of 3000, ops, clears,
//                barriers, resets and passes over NB_RS_MAX refs: equal node by node, ref by ref; rs_bad 0; a ref appended behind the
//                set is found (catch-up), and a mutation (a ref skipped by the catch-up) is caught by =2
//   3 bench      ns a walked draw, table against cache: 1 M-id table, the ids of a Fort Joy frame spread over it, and between two draws
//                the game's own work modelled as a walk over a buffer bigger than the L2 (it evicts what the tap touched, as the engine's
//                code does on the phone's main thread)
// Build: clang -std=c11 -O2 -Wall -Wextra nr_cpu_test.c ../../../nb/nb_plan.c -o nr_cpu_test
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../../../nb/nb_ir.h"
#include <stdatomic.h>
#include <string.h>

// ---- NR-TAPIMG (measured and REJECTED, NR-CPU.md: the walk is not the table lookups): the slot image cache, kept here only
// for the measurement. Per (kind, stage, slot) the image last read for the bound id and the registration generation of that read.
enum { NRC_KINDS = 2, NRC_STAGES = 6, NRC_SLOTS = 64 };
typedef struct {
    uint64_t im[NRC_KINDS][NRC_STAGES][NRC_SLOTS];   // the image last read for the slot's id
    uint32_t gen[NRC_KINDS][NRC_STAGES][NRC_SLOTS];  // registration generation of that read; 0 = nothing cached
    uint64_t hits, misses;                           // counters (owner thread)
} nrc_slotimg;

// The slot (kind w: 0 texture, 1 image; stage st; slot s) was rebound: forget its image.
static inline void nrc_slot_forget(nrc_slotimg *c, int w, int st, int s) { c->gen[w][st][s] = 0; }

// The image of the id held by the slot (id = pool id, the table index before masking). regen = the generation read once for this draw
// (never 0). table / mask: g_wt_id_mem and WT_IDS - 1.
static inline uint64_t nrc_slot_image(nrc_slotimg *c, int w, int st, int s, uint32_t id, uint32_t regen, const _Atomic uint64_t *table, uint32_t mask) {
    if (c->gen[w][st][s] == regen) { c->hits++; return c->im[w][st][s]; }
    const uint64_t v = atomic_load_explicit(&table[id & mask], memory_order_relaxed);
    c->im[w][st][s] = v; c->gen[w][st][s] = regen; c->misses++;
    return v;
}
// The generation after a registration: never 0 (0 = "nothing cached").
static inline uint32_t nrc_regen_next(uint32_t g) { return g + 1 ? g + 1 : 1; }

static unsigned long long g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { if (g_fail++ < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 16); }
static uint32_t rn(uint32_t n) { return rnd() % n; }

#define IDS (1u << 20)
static _Atomic uint64_t g_tab[IDS];        // = g_wt_id_mem
static uint32_t g_regen = 1;               // = g_nbt_regen, bumped by wt_register after its store

enum { ST = 5 };
typedef struct {
    nb_frame f;
    uint32_t tex[ST][NRC_SLOTS], img[ST][NRC_SLOTS];   // bound id + 1
    uint64_t tdirty[ST], idirty[ST];
    uint64_t mt[ST], mi[ST];                         // the bound program's sampled slots (NB-07 masks)
    int rd_node; uint32_t rd_gen, gen;
    int cache, mut;                                  // cache: 0 table, 1 nr_cpu.h; mut 1: no forget on rebind, 2: the generation is not looked at
    nrc_slotimg c;
    uint64_t lookups, bad;
} world;
static world *W[2];

static inline uint64_t look(world *w, int k, int st, int s, uint32_t v) {
    if (!w->cache) return atomic_load_explicit(&g_tab[(v - 1) & (IDS - 1)], memory_order_relaxed);
    return nrc_slot_image(&w->c, k, st, s, v - 1, w->mut == 2 ? 1u : g_regen, g_tab, IDS - 1);
}
static void w_draw(world *w, int checked) {   // nbt_draw from nb_draw on: the reads of the sampled rebound slots, then the sampled list
    nb_frame *f = &w->f;
    nb_draw(f, 0);
    if (!f->open) return;
    const int node = f->nnode - 1, all = node != w->rd_node || w->rd_gen != w->gen;
    for (int st = 0; st < ST; st++) for (int k = 0; k < 2; k++) {   // nbt_reads
        const uint32_t *tab = k ? w->img[st] : w->tex[st]; uint64_t *dirty = k ? &w->idirty[st] : &w->tdirty[st];
        const uint64_t mask = k ? w->mi[st] : w->mt[st];
        const uint64_t m0 = all ? ~(uint64_t)0 : *dirty; *dirty = m0 & ~mask;
        uint64_t m = m0 & mask;
        while (m) { const int s = __builtin_ctzll(m); m &= m - 1; if (!tab[s]) continue;
            const uint64_t im = look(w, k, st, s, tab[s]);
            if (checked) { w->lookups++; if (im != atomic_load_explicit(&g_tab[(tab[s] - 1) & (IDS - 1)], memory_order_relaxed)) w->bad++; }
            if (im) nb_read(f, -1, im); }
    }
    uint64_t im[NB_MAX_SREAD + 1]; int n = 0, exact = 1;   // nbt_prog_draw
    for (int st = 0; st < ST; st++) for (int k = 0; k < 2; k++) {
        uint64_t m = k ? w->mi[st] : w->mt[st]; const uint32_t *tab = k ? w->img[st] : w->tex[st];
        while (m) { const int s = __builtin_ctzll(m); m &= m - 1; if (!tab[s]) continue;
            const uint64_t x = look(w, k, st, s, tab[s]);
            if (checked) { w->lookups++; if (x != atomic_load_explicit(&g_tab[(tab[s] - 1) & (IDS - 1)], memory_order_relaxed)) w->bad++; }
            if (!x) { exact = 0; continue; } if (n < NB_MAX_SREAD) im[n++] = x; else exact = 0; }
    }
    nb_draw_prog(f, 0x1234, exact, im, n);
    w->rd_node = node; w->rd_gen = w->gen;
}
static void w_bind(world *w, int k, int st, int s, uint32_t id) {   // nbt_bind
    uint32_t *tab = k ? w->img[st] : w->tex[st]; const uint32_t v = id + 1;
    if (tab[s] == v) return;
    tab[s] = v; *(k ? &w->idirty[st] : &w->tdirty[st]) |= 1ull << s;
    if (w->cache && w->mut != 1) nrc_slot_forget(&w->c, k, st, s);
}
static void w_program(world *w, int st, uint64_t mt, uint64_t mi) { w->mt[st] = mt; w->mi[st] = mi; }
static void w_reset(world *w) { nb_frame_reset(&w->f, 1); w->gen++; }
static int ir_same(const nb_frame *a, const nb_frame *b) {
    if (a->nnode != b->nnode || a->open != b->open || a->nref != b->nref) return 0;
    for (int i = 0; i < a->nnode; i++) {
        const nb_node *p = &a->node[i], *q = &b->node[i];
        if (p->kind != q->kind || p->draws != q->draws || p->nref != q->nref || p->nsr != q->nsr || p->fsx != q->fsx || p->fs != q->fs) return 0;
        if (memcmp(p->sread, q->sread, sizeof(uint64_t) * (size_t)p->nsr)) return 0;
    }
    return !memcmp(a->ref, b->ref, sizeof(nb_ref) * (size_t)a->nref);
}
static void reg(uint32_t id, uint64_t mem) { atomic_store(&g_tab[id], mem); g_regen = nrc_regen_next(g_regen); }   // wt_register: store, then the generation

// ================= 1 =================
// returns: IR mismatches, cache answers that differed from the table
static void run(int mut, int steps, uint64_t *ir_bad, uint64_t *look_bad) {
    for (int i = 0; i < 2; i++) { world *w = W[i]; memset(w, 0, sizeof *w); w->cache = i; w->mut = i ? mut : 0; w->rd_node = -1; nb_frame_reset(&w->f, 1); }
    memset(g_tab, 0, sizeof g_tab); g_regen = 1;
    enum { NID = 400 };
    uint32_t ids[NID]; for (int i = 0; i < NID; i++) { ids[i] = rnd() & (IDS - 1); atomic_store(&g_tab[ids[i]], 0x10000000ull + (uint64_t)i * 64); }
    *ir_bad = 0; int rt = 0;
    for (int step = 0; step < steps; step++) {
        const int ev = (int)rn(1000);
        if (ev < 500) { for (int i = 0; i < 2; i++) w_draw(W[i], i); if (!ir_same(&W[0]->f, &W[1]->f)) (*ir_bad)++; }
        else if (ev < 800) { const int k = rn(10) == 0, st = (int)rn(2), s = (int)rn(12); const uint32_t id = ids[rn(NID)]; for (int i = 0; i < 2; i++) w_bind(W[i], k, st, s, id); }
        else if (ev < 900) { const int st = (int)rn(2); const uint64_t mt = rn(8) ? (uint64_t)rnd() & 0xfff : 0, mi = rn(10) ? 0 : (uint64_t)rnd() & 0xf; for (int i = 0; i < 2; i++) w_program(W[i], st, mt, mi); }
        else if (ev < 940) { nb_att a[2] = {{0x9000000ull + (uint64_t)(++rt % 5) * 256, 0}, {0x9800000ull, NB_SLOT_DS}}; for (int i = 0; i < 2; i++) nb_pass_begin(&W[i]->f, a, 2); }
        else if (ev < 950) { uint64_t s = 0x9000000ull, d = 0x9100000ull; for (int i = 0; i < 2; i++) nb_op(&W[i]->f, NB_OP_COPY, &s, 1, &d, 1); }
        else if (ev < 955) { for (int i = 0; i < 2; i++) w_reset(W[i]); }
        else { const uint32_t id = ids[rn(NID)]; reg(id, rn(5) ? 0x20000000ull + (uint64_t)rn(4096) * 64 : 0); }   // a registration (bound or not)
    }
    *look_bad = W[1]->bad;
}
static void test_tapimg(void) {
    uint64_t irb, lb;
    for (int r = 0; r < 20; r++) { run(0, 60000, &irb, &lb); CHECK(irb == 0 && lb == 0, "NR-TAPIMG: cache vs table: IR mismatches %llu, lookups that differ %llu", (unsigned long long)irb, (unsigned long long)lb); }
    printf("  NR-TAPIMG: 20 x 60000 steps: IR equal at every draw, %llu cache answers checked, hits %llu / misses %llu\n", (unsigned long long)W[1]->lookups,
           (unsigned long long)W[1]->c.hits, (unsigned long long)W[1]->c.misses);
    run(1, 60000, &irb, &lb); CHECK(lb > 0, "mutation 1 (no forget on rebind) not caught");
    run(2, 60000, &irb, &lb); CHECK(lb > 0, "mutation 2 (generation not looked at) not caught");
    printf("  NR-TAPIMG mutations caught: no forget on rebind, registration generation ignored\n");
}


// ================= 2. NR-TAPSET =================
static double now_s(void) { struct timespec ts; timespec_get(&ts, TIME_UTC); return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9; }
static nb_frame *F[3];
static int frames_same(const nb_frame *a, const nb_frame *b) {
    if (a->nnode != b->nnode || a->open != b->open || a->nref != b->nref || a->overflow != b->overflow || a->stray != b->stray) return 0;
    for (int i = 0; i < a->nnode; i++) { const nb_node *p = &a->node[i], *q = &b->node[i];
        if (p->kind != q->kind || p->draws != q->draws || p->nref != q->nref || p->ref_first != q->ref_first || p->end != q->end) return 0; }
    return !memcmp(a->ref, b->ref, sizeof(nb_ref) * (size_t)a->nref);
}
static void test_tapset(void) {
    uint64_t steps = 0, bad = 0, reads = 0, big = 0;
    for (int run = 0; run < 12; run++) {
        for (int i = 0; i < 3; i++) { memset(F[i], 0, sizeof(nb_frame)); nb_frame_reset(F[i], 1); F[i]->rs_mode = (uint8_t)i; }
        const int wide = run % 4 == 3;   // passes of thousands of reads: over NB_RS_MAX, back to the scan
        for (int step = 0; step < 200000; step++, steps++) {
            const int ev = (int)rn(1000);
            if (ev < 600) { const int n = 1 + (int)rn(12); uint64_t im[12]; for (int k = 0; k < n; k++) im[k] = 0x10000000ull + (uint64_t)rn(wide ? 3000 : 900) * 64;
                for (int i = 0; i < 3; i++) { nb_draw(F[i], 0); for (int k = 0; k < n; k++) nb_read(F[i], -1, im[k]); } reads += (uint64_t)n; }
            else if (ev < 615) { nb_att a[2] = {{0x9000000ull + (uint64_t)rn(6) * 256, 0}, {0x9800000ull, NB_SLOT_DS}}; const int na = 1 + (int)rn(2); for (int i = 0; i < 3; i++) nb_pass_begin(F[i], a, na); }
            else if (ev < 620) { uint64_t s = 0x10000000ull + (uint64_t)rn(900) * 64, d = 0x9000000ull; for (int i = 0; i < 3; i++) nb_op(F[i], NB_OP_COPY, &s, 1, &d, 1); }
            else if (ev < 623) { const uint32_t c[4] = {0, 0, 0, 0}; for (int i = 0; i < 3; i++) nb_clear(F[i], 0x9000000ull, 1, 0, c, 0.f, 0); }
            else if (ev < 625) { for (int i = 0; i < 3; i++) nb_barrier(F[i]); }
            else if (ev < 627) { for (int i = 0; i < 3; i++) nb_frame_reset(F[i], 2); }
            else if (ev < 629) { for (int i = 0; i < 3; i++) nb_submit(F[i], 0); }
            if (!(step & 63) || ev >= 600) { if (!frames_same(F[0], F[1]) || !frames_same(F[0], F[2])) bad++; }
        }
        CHECK(F[2]->rs_bad == 0, "NR-TAPSET =2: the set and the scan differ %llu times (run %d)", (unsigned long long)F[2]->rs_bad, run);
        big += F[1]->rs_big;
    }
    CHECK(bad == 0, "NR-TAPSET: the IR with the set differs from the scan's at %llu steps", (unsigned long long)bad);
    CHECK(big > 0, "the passes over NB_RS_MAX were not exercised");
    printf("  NR-TAPSET: %llu steps (%llu reads), IR with the set = IR with the scan at every step, =2 differences 0, reads in passes over %d refs (scan) %llu\n",
           (unsigned long long)steps, (unsigned long long)reads, NB_RS_MAX, (unsigned long long)big);
    // catch-up and mutation
    memset(F[1], 0, sizeof(nb_frame)); nb_frame_reset(F[1], 1); F[1]->rs_mode = 2;
    nb_att a = {0x9000000ull, 0};
    nb_pass_begin(F[1], &a, 1); nb_draw(F[1], 0); nb_read(F[1], -1, 0x5000);
    nb_node *p = &F[1]->node[F[1]->nnode - 1];
    F[1]->ref[F[1]->nref].image = 0x6000; F[1]->ref[F[1]->nref].role = NB_REF_READ; F[1]->nref++; p->nref++;   // appended behind the set
    nb_read(F[1], -1, 0x6000);
    CHECK(F[1]->rs_bad == 0 && p->nref == 2, "catch-up: a ref appended by someone else is found by the set (bad %llu, refs %d)", (unsigned long long)F[1]->rs_bad, p->nref);
    F[1]->ref[F[1]->nref].image = 0x7000; F[1]->ref[F[1]->nref].role = NB_REF_READ; F[1]->nref++; p->nref++;
    F[1]->rs_n = p->nref;   // mutation: marked synced without inserting it
    nb_read(F[1], -1, 0x7000);
    CHECK(F[1]->rs_bad == 1 && p->nref == 3, "mutation (a ref skipped by the catch-up) not caught by =2 (bad %llu, refs %d)", (unsigned long long)F[1]->rs_bad, p->nref);
    printf("  NR-TAPSET: catch-up of refs appended behind the set; mutation caught (a ref skipped by the catch-up)\n");
}
static void bench_tapset(void) {
    // one frame of Fort Joy's heavy window (NR-BN): 40 passes - one of 600 draws, two of 300, three of 120, the rest short; 1-6 reads a
    // draw out of materials of 6 textures (400 materials); every draw walks here (on the phone 735 of 2157 a frame walk)
    double t[2]; uint64_t reads = 0, draws = 0;
    static uint64_t mat[400][6]; for (int m = 0; m < 400; m++) for (int s = 0; s < 6; s++) mat[m][s] = 0x10000000ull + (uint64_t)(s >= 4 ? s : m * 6 + s) * 64;
    static const int len[4] = {600, 300, 120, 8};
    for (int real = 0; real < 2; real++) {   // real: a big pass draws 40 materials (+ its own), small passes 3: ~600 refs a frame as play-1118's [nb] ir
        for (int v = 0; v < 2; v++) {
            memset(F[0], 0, sizeof(nb_frame)); nb_frame_reset(F[0], 1); F[0]->rs_mode = (uint8_t)v; g_rng = 11; reads = draws = 0;
            const double a = now_s();
            for (int frame = 0; frame < 300; frame++) {
                nb_frame_reset(F[0], (uint32_t)frame + 1);
                for (int pass = 0; pass < 40; pass++) {
                    nb_att at = {0x9000000ull + (uint64_t)(pass % 5) * 256, 0}; nb_pass_begin(F[0], &at, 1);
                    const int nd = len[pass < 1 ? 0 : pass < 3 ? 1 : pass < 6 ? 2 : 3];
                    for (int d = 0; d < nd; d++) { const int m = real ? (pass * 7 + (int)rn(pass < 6 ? 40 : 3)) % 400 : (int)rn(400), n = 1 + (int)rn(6);
                        nb_draw(F[0], 0); for (int k = 0; k < n; k++) nb_read(F[0], -1, mat[m][k]); reads += (uint64_t)n; draws++; }
                }
            }
            t[v] = now_s() - a;
        }
        printf("  bench NR-TAPSET (this PC; %s: %.0f draws a frame, %.1f reads a draw, %d refs a frame): %.0f ns a draw with the scan, %.0f ns with the set (%.0f%% less)\n",
               real ? "play-1118-like, 40 materials in a big pass" : "worst case, 400 materials in every pass", (double)draws / 300., (double)reads / (double)draws, F[0]->nref,
               t[0] / (double)draws * 1e9, t[1] / (double)draws * 1e9, 100. * (1. - t[1] / t[0]));
    }
}
// ================= 3. bench NR-TAPIMG =================
static volatile uint64_t g_sink;
static uint64_t *g_game; static size_t g_game_n;
static void game_work(unsigned lines) {   // the engine's own code between two draws: touches `lines` cache lines of a big buffer
    static size_t at = 0; uint64_t s = 0;
    for (unsigned i = 0; i < lines; i++) { at = (at + 8 * 37) % g_game_n; s += g_game[at]; g_game[at] = s; }
    g_sink += s;
}
static void bench(void) {
    g_game_n = (8u << 20) / 8; g_game = (uint64_t *)calloc(g_game_n, 8);
    for (int v = 0; v < 2; v++) { world *w = W[v]; memset(w, 0, sizeof *w); w->cache = v; w->rd_node = -1; nb_frame_reset(&w->f, 1); }
    memset(g_tab, 0, sizeof g_tab); g_regen = 1;
    enum { MATS = 300, PASSES = 12, DRAWS = 1600 };
    uint32_t id[MATS][6];   // a material: 6 textures (Base_PBR: albedo, normal, physical, ... + shared ones)
    for (int m = 0; m < MATS; m++) for (int s = 0; s < 6; s++) { id[m][s] = s >= 4 ? 7 + (uint32_t)s : rnd() & (IDS - 1); atomic_store(&g_tab[id[m][s]], 0x10000000ull + (uint64_t)(m * 6 + s) * 64); }
    for (unsigned lines = 0; lines <= 256; lines = lines ? lines * 4 : 64) {
        double t[2]; uint64_t walks = 0;
        for (int v = 0; v < 2; v++) {
            world *w = W[v]; g_rng = 7; walks = 0;
            for (int st = 0; st < ST; st++) { w->mt[st] = 0; w->mi[st] = 0; }
            w->mt[0] = 0x1; w->mt[1] = 0x3f;   // vs samples slot 0, fs slots 0-5
            const double a = now_s();
            for (int frame = 0; frame < 200; frame++) {
                w_reset(w);
                for (int d = 0; d < DRAWS; d++) {
                    if (d % (DRAWS / PASSES) == 0) { nb_att at[2] = {{0x9000000ull + (uint64_t)(d % 3) * 256, 0}, {0x9800000ull, NB_SLOT_DS}}; nb_pass_begin(&w->f, at, 2); }
                    const uint32_t mat = rn(3) ? (uint32_t)(d / 3 % MATS) : rn(MATS);
                    for (int s = 0; s < 6; s++) w_bind(w, 0, 1, s, id[mat][s]);
                    w_bind(w, 0, 0, 0, id[mat][0]);
                    if (lines) game_work(lines);
                    w_draw(w, 0); walks++;
                }
            }
            t[v] = now_s() - a;
        }
        // the game work alone, to take it out
        g_rng = 7; const double a = now_s(); for (int i = 0; i < 200 * DRAWS; i++) if (lines) game_work(lines); const double tg = now_s() - a;
        printf("  bench (this PC; game work %u cache lines a draw): walk %.0f ns a draw through the table, %.0f ns with the slot cache (%.0f%% less; cache hits %.0f%%)\n", lines,
               (t[0] - tg) / (double)walks * 1e9, (t[1] - tg) / (double)walks * 1e9, 100. * (1. - (t[1] - tg) / (t[0] - tg)),
               100. * (double)W[1]->c.hits / (double)(W[1]->c.hits + W[1]->c.misses ? W[1]->c.hits + W[1]->c.misses : 1));
    }
    free(g_game);
}

int main(int argc, char **argv) {
    printf("nr_cpu_test (NR-CPU)\n");
    W[0] = (world *)calloc(1, sizeof(world)); W[1] = (world *)calloc(1, sizeof(world));
    F[0] = (nb_frame *)calloc(1, sizeof(nb_frame)); F[1] = (nb_frame *)calloc(1, sizeof(nb_frame)); F[2] = (nb_frame *)calloc(1, sizeof(nb_frame));
    test_tapimg();
    test_tapset();
    if (!(argc > 1 && !strcmp(argv[1], "--no-bench"))) { bench_tapset(); bench(); }
    printf("%s: %llu checks, %llu failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
