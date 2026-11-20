// NR-09S (DM_NR_STATE): host test of dm/gfx/nr/nr_state.h - the model of the engine's opcode-21 packet (all state of a draw).
//   nr_state_test                     self-contained: random renderer states and packets: the model reads only, commit + check_mem agree,
//                                     the =2 comparator, refusals, mutations of the expected effect are caught; a PC timing of the model.
//   nr_state_test --oracle in out     the model run on the cases nr_state_oracle.py made (it runs the game's own machine code on the same
//                                     states in Unicorn and compares the NVN calls with every argument and every byte left behind).
// Build: clang -std=c11 -Wall -Wextra -O2 nr_state_test.c -o nr_state_test
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { GA = 0x10000000, ARENA = 0x18000, A_LIST = 0, A_R = 0x1000, A_PKT = 0x3000, A_CNT = 0x3100, A_DEV = 0x4000, A_CTX = 0x9400, A_TAB = 0xa000, TAB_STRIDE = 0x1400 };
static uint8_t *g_arena;
#define NRST_PTR(a) (g_arena + ((uint64_t)(a) - GA))
#include "../../../nr/nr_state.h"

static unsigned long long g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { if (g_fail++ < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static uint64_t g_seed = 0x2545f4914f6cdd1dull;
static uint64_t rnd(void) { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17; return g_seed; }
static void w64(uint32_t o, uint64_t v) { memcpy(g_arena + o, &v, 8); }
static void w32(uint32_t o, uint32_t v) { memcpy(g_arena + o, &v, 4); }
static uint64_t r64(uint32_t o) { uint64_t v; memcpy(&v, g_arena + o, 8); return v; }
static const int32_t k_prim[7] = {0, 1, 3, 2, 4, 6, 5};   // a stand-in for main + 0x1f58640 (the oracle uses the real table)

// ---- the oracle mode ----
// in:  u32 'NRST', u32 n, 28 bytes prim table, then n x { u64 special; u8 arena[ARENA] }
// out: u32 n, then n x { u32 ran, u32 why, u32 nops, nrst_op ops[nops], u8 arena[ARENA] }
static int oracle(const char *in, const char *out) {
    FILE *f = fopen(in, "rb"), *o = fopen(out, "wb");
    if (!f || !o) { printf("cannot open %s / %s\n", in, out); return 2; }
    uint32_t hd[2]; uint8_t prim[28];
    if (fread(hd, 4, 2, f) != 2 || hd[0] != 0x5453524e || fread(prim, 1, 28, f) != 28) { printf("bad case file\n"); return 2; }
    fwrite(&hd[1], 4, 1, o);
    g_arena = malloc(ARENA);
    static nrst_run s;
    for (uint32_t i = 0; i < hd[1]; i++) {
        uint64_t special;
        if (fread(&special, 8, 1, f) != 1 || fread(g_arena, 1, ARENA, f) != ARENA) { printf("short case file\n"); return 2; }
        const uint8_t *R = NRST_PTR(r64(A_LIST + 0x38));
        const uint32_t ran = (uint32_t)nrst_code21(R, g_arena + A_PKT, special, prim, &s);
        const uint32_t why = (uint32_t)s.why, n = ran ? (uint32_t)s.nops : 0;
        if (ran) nrst_commit((uint8_t *)R, &s);
        fwrite(&ran, 4, 1, o); fwrite(&why, 4, 1, o); fwrite(&n, 4, 1, o); fwrite(s.op, sizeof(nrst_op), n, o); fwrite(g_arena, 1, ARENA, o);
    }
    fclose(f); fclose(o);
    printf("nr_state_test --oracle: %u cases run (sizeof nrst_op %zu)\n", hd[1], sizeof(nrst_op));
    return 0;
}

// ---- the self-contained mode: a random but consistent world ----
enum { T_SHADER, T_VB, T_IB, T_VFMT, T_BLEND, T_DS, T_RASTER, T_N };
static const uint32_t k_tab[T_N] = {NRST_T_SHADER, NRST_T_VB, NRST_T_IB, NRST_T_VFMT, NRST_T_BLEND, NRST_T_DS, NRST_T_RASTER};
static const uint32_t k_esz[T_N] = {0x60, 0x68, 0x58, 0x80, 0x20, 0x20, 0x18};
static uint32_t tab_at(int t) { return A_TAB + (uint32_t)t * TAB_STRIDE; }
static uint64_t g_special;
// A handle of table t: valid (record of page / off), stale (generation), without generation bits, or 0.
static uint64_t handle(int t, int kind) {
    const uint32_t per = (uint32_t)(r64(A_R + k_tab[t] + 0x10) & 0xffffffff), page = (uint32_t)(rnd() % 4), off = per ? (uint32_t)(rnd() % per) : 0;
    const uint32_t idx = page * per + off, T = tab_at(t);
    uint32_t gen; memcpy(&gen, g_arena + T + 0x40 + 0x20 * page + 4 * off, 4);
    if (kind == 0) return 0;
    if (kind == 1) return (uint64_t)idx;                                         // no generation bits: invalid
    if (kind == 2) return (rnd() & 0x3ff) << 54 | (uint64_t)((gen ^ 5) & 0x3fffff) << 32 | idx;   // stale
    return (rnd() & 0x3ff) << 54 | (uint64_t)(gen & 0x3fffff) << 32 | idx;
}
static uint64_t pick(int t) { const int r = (int)(rnd() % 16); return handle(t, r == 0 ? 0 : r == 1 ? 1 : r == 2 ? 2 : 3); }
static void world(void) {
    for (uint32_t i = 0; i < ARENA; i += 8) w64(i, rnd());
    w64(A_LIST + 0x38, GA + A_R);
    w64(A_R + NRST_R_DEV, GA + A_DEV); w64(A_DEV + NRST_DEV_CTX, GA + A_CTX); w64(A_DEV + NRST_DEV_FRAME, rnd() & 0xffffff);
    for (int t = 0; t < T_N; t++) {
        const uint32_t T = tab_at(t), per = 1 + (uint32_t)(rnd() % 8);
        w64(A_R + k_tab[t], GA + T); w64(A_R + k_tab[t] + 8, GA + T + 0x20); w32(A_R + k_tab[t] + 0x10, per);
        for (int p = 0; p < 4; p++) {
            w64(T + 8 * p, GA + T + 0x100 + 0x400 * p); w64(T + 0x20 + 8 * p, GA + T + 0x40 + 0x20 * p);
            for (int k = 0; k < 8; k++) w32(T + 0x40 + 0x20 * p + 4 * k, 1 + (uint32_t)(rnd() % 0x3ffffe));
            for (uint32_t k = 0; k < 8; k++) {   // records: some fields zero (the branches)
                const uint32_t e = T + 0x100 + 0x400 * p + k * k_esz[t];
                if (t == T_SHADER && rnd() % 4 == 0) w64(e + 8 + 8 * (rnd() % 2), 0);
                if (t == T_VB && rnd() % 6 == 0) w64(e, 0);
                if (t == T_VFMT) { if (rnd() % 2) w32(e + 0x1c, 0); if (rnd() % 2) w32(e + 0x18, 0); }
            }
        }
    }
    g_special = rnd() % 3 ? 0x7fff00001234ull : handle(T_BLEND, 3);
}
static void packet(void) {
    uint8_t *R = g_arena + A_R, *P = g_arena + A_PKT;
    const uint64_t sh = pick(T_SHADER), ds = pick(T_DS), ra = pick(T_RASTER), bl = rnd() % 8 == 0 ? g_special : pick(T_BLEND), vf = pick(T_VFMT);
    w64(A_PKT + 8, sh); w64(A_PKT + 0x10, ds); w32(A_PKT + 0x18, (uint32_t)(rnd() % 4)); w64(A_PKT + 0x20, ra); w64(A_PKT + 0x28, bl); w64(A_PKT + 0x30, vf);
    for (int k = 0; k < 3; k++) w64(A_PKT + 0x38 + 8 * k, k && rnd() % 2 ? 0 : pick(T_VB));
    w64(A_PKT + 0x50, rnd() % 4 ? (rnd() & ~0xffffffffull) | (rnd() % (4 * (r64(A_R + NRST_T_IB + 0x10) & 0xffffffff))) : 0);   // any high bits: the IB lookup takes the index only
    P[0x58] = (uint8_t)(rnd() % 10);
    w64(A_PKT + 0x60, rnd() % 3 ? 0 : GA + A_CNT);
    static const float fa[6] = {-1.f, 0.f, 0.5f, -0.f, 0.25f, 1.f}; float a = fa[rnd() % 6]; memcpy(P + 0x68, &a, 4);
    // the renderer: each cached field the packet's value half the time
    static const uint32_t same[][2] = {{NRST_R_SHADER, 8}, {NRST_R_DS, 0x10}, {NRST_R_RASTER, 0x20}, {NRST_R_BLEND, 0x28}, {NRST_R_VFMT, 0x30},
                                       {NRST_R_VB, 0x38}, {NRST_R_VB + 8, 0x40}, {NRST_R_VB + 16, 0x48}, {NRST_R_IB, 0x50}, {NRST_R_REN_Q, 0x60}};
    for (unsigned i = 0; i < sizeof same / sizeof same[0]; i++) if (rnd() % 2) memcpy(R + same[i][0], P + same[i][1], 8);
    if (rnd() % 2) memcpy(R + NRST_R_REF, P + 0x18, 4);
    if (rnd() % 2) R[NRST_R_PRIM] = P[0x58];
    if (rnd() % 2) memcpy(R + NRST_R_ALPHA, P + 0x68, 4);
    R[NRST_R_REN_NEED] = (uint8_t)(rnd() % 3);
    for (int k = 0; k < 3; k++) w64(A_R + NRST_R_VB_DIRTY + 8 * k, rnd() % 3 ? 0 : rnd());
    w64(A_R + NRST_R_IB_FIRST, rnd() % 3 ? 0 : rnd());
    w32(A_R + NRST_R_BLEND_MASK, rnd() % 2 ? 0xffffffffu : (uint32_t)rnd());
    for (int i = 0; i < 8; i++) w64(A_R + NRST_R_RT + 8 * i, rnd() % 2 ? 0 : rnd());
}

// A recorder for the model's calls when they are made (=1's executor in the front end, here a log).
static void self_test(void) {
    static nrst_run s, s2;
    static uint8_t before[ARENA], after[ARENA];
    g_arena = malloc(ARENA);
    unsigned long long ran = 0, refused = 0, ops = 0, why[NRST_WHY_N] = {0}, fnc[NRST_F_N] = {0};
    for (int it = 0; it < 20000; it++) {
        if (it % 50 == 0) world();
        packet();
        uint8_t *R = g_arena + A_R;
        memcpy(before, g_arena, ARENA);
        const int ok = nrst_code21(R, g_arena + A_PKT, g_special, (const uint8_t *)k_prim, &s);
        CHECK(!memcmp(before, g_arena, ARENA), "nrst_code21 wrote guest memory (case %d)", it);
        if (!ok) { refused++; why[s.why < NRST_WHY_N ? s.why : 0]++; CHECK(s.why != NRST_OK, "refused without a reason"); continue; }
        ran++; ops += (unsigned long long)s.nops;
        for (int i = 0; i < s.nops; i++) fnc[s.op[i].fn < NRST_F_N ? s.op[i].fn : 0]++;
        CHECK(s.nops <= NRST_OPS_MAX && s.nstamps <= NRST_STAMPS_MAX, "bounds");
        // the region snapshot the front end takes for =2, then the commit as the engine's code would leave memory
        uint8_t pre[NRST_REGION_END - NRST_REGION]; memcpy(pre, R + NRST_REGION, sizeof pre);
        const uint32_t pre_cs = nrst_r32(R + NRST_R_CSFLAG);
        nrst_commit(R, &s);
        uint32_t where = 0;
        CHECK(nrst_check_mem(R, pre, pre_cs, &s, &where) == 0, "check_mem after the model's own commit (offset %#x)", where);
        // the packet again on the committed state: a second run must be the engine's filter at work (no shader / DS / raster / vfmt / VB call)
        memcpy(after, g_arena, ARENA);
        if (nrst_code21(R, g_arena + A_PKT, g_special, (const uint8_t *)k_prim, &s2))
            CHECK(s2.nops == 0 && s2.nstamps == 0, "the same packet again made %d calls / %d stamps (case %d): the engine's filter", s2.nops, s2.nstamps, it);
        // mutations of the effect the =2 check must catch: one stored field off by one, a stamp left out, a byte outside the fields
        {
            memcpy(g_arena, before, ARENA); nrst_commit(R, &s);
            const uint32_t m = (uint32_t)(rnd() % 5);
            if (m == 0) R[NRST_R_SHADER] ^= 1;
            else if (m == 1) R[NRST_R_VB + 8 * (rnd() % 3)] ^= 0x10;
            else if (m == 2) R[NRST_R_ALPHA + 3] ^= 0x80;
            else if (m == 3) R[NRST_REGION + 0x31] ^= 1;   // r + 0x1721: no field of the model
            else if (s.nstamps) NRST_PTR(s.stamp[0].a)[0] ^= 1;
            else R[NRST_R_REN_OFF] ^= 1;
            CHECK(nrst_check_mem(R, pre, pre_cs, &s, &where) != 0, "mutation %u not caught", m);
        }
        // the comparator: the list against itself (with a stack object standing for one pointer), and one changed argument
        {
            uint64_t loc[3] = {0, 0, 0}; int same = 1;
            for (int i = 0; i < s.nops; i++) {
                nrst_op g = s.op[i];
                if (g.p == NRST_LOCAL_BLEND) g.p = 0x7ffff000; else if (g.p == NRST_LOCAL_COLOR) g.p = 0x7ffff008;
                same &= nrst_same_op(&s.op[i], &g, loc);
            }
            CHECK(same, "the comparator rejects the model's own list");
            if (s.nops) {
                const int k = (int)(rnd() % (uint64_t)s.nops); nrst_op g = s.op[k]; uint64_t l2[3] = {0, 0, 0};
                switch (rnd() % 5) { case 0: g.a ^= 8; break; case 1: g.i ^= 1; break; case 2: g.j ^= 2; break; case 3: g.q ^= 4; break; default: g.fn ^= 1; break; }
                CHECK(!nrst_same_op(&s.op[k], &g, l2), "the comparator misses a changed argument (call %u)", s.op[k].fn);
            }
        }
        memcpy(g_arena, after, ARENA);
    }
    printf("model: %llu packets run, %llu left to the engine (", ran, refused);
    for (int i = 1; i < NRST_WHY_N; i++) printf("%s%d:%llu", i > 1 ? " " : "", i, why[i]);
    printf("), %.2f calls a packet; calls by kind:", ran ? (double)ops / (double)ran : 0.);
    for (int i = 1; i < NRST_F_N; i++) printf(" %d:%llu", i, fnc[i]);
    printf("\n");
    CHECK(ran > 10000 && refused > 100, "coverage of both outcomes");
    for (int i = 1; i < NRST_F_N; i++) CHECK(fnc[i] > 0, "call kind %d never made", i);
}

// PC timing: a frame-like mix (most packets change the shader half the time and VB 0 almost always, DS / blend / vfmt rarely).
static double now_s(void) { struct timespec t; timespec_get(&t, TIME_UTC); return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec; }
static void bench(void) {
    static nrst_run s;
    world();
    enum { N = 2000 };
    static uint8_t pk[N][0x70];
    uint8_t *R = g_arena + A_R, *P = g_arena + A_PKT;
    packet();
    uint64_t sh[8], vb[64]; for (int i = 0; i < 8; i++) sh[i] = handle(T_SHADER, 3); for (int i = 0; i < 64; i++) vb[i] = handle(T_VB, 3);
    for (int i = 0; i < N; i++) {
        memcpy(pk[i], P, 0x70);
        if (i % 2) memcpy(pk[i] + 8, &sh[rnd() % 8], 8);
        memcpy(pk[i] + 0x38, &vb[rnd() % 64], 8);
        memset(pk[i] + 0x40, 0, 16);
        if (i % 20 == 0) { const uint64_t d = handle(T_DS, 3); memcpy(pk[i] + 0x10, &d, 8); }
        const uint64_t ib = rnd() % (4 * (r64(A_R + NRST_T_IB + 0x10) & 0xffffffff)); memcpy(pk[i] + 0x50, &ib, 8);
    }
    unsigned long long calls = 0, runs = 0;
    const double t0 = now_s();
    for (int rep = 0; rep < 200; rep++)
        for (int i = 0; i < N; i++) {
            if (nrst_code21(R, pk[i], g_special, (const uint8_t *)k_prim, &s)) { nrst_commit(R, &s); calls += (unsigned long long)s.nops; runs++; }
        }
    const double t1 = now_s();
    printf("timing (PC, this build): %.1f ns a packet for the model + commit (%.2f calls a packet, %llu packets)\n", 1e9 * (t1 - t0) / (double)(200 * N),
           runs ? (double)calls / (double)runs : 0., runs);
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--oracle")) return oracle(argv[2], argv[3]);
    self_test();
    bench();
    printf("%s: %llu checks, %llu failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
