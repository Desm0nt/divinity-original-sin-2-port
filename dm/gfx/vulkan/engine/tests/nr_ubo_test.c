// NR-09U (DM_NR_UBO): host test of dm/gfx/nr/nr_ubo.h - the model of the engine's uniform-block functions 0x926f90 + 0x925740.
//   nr_ubo_test                    self-contained: random worlds - the =2 simulation stores nothing and predicts exactly what execution
//                                  does (blocks, head, ring bytes, calls); nru_check accepts it and catches every injected difference;
//                                  a foreign allocation in between is "raced", not bad; every refusal stores nothing; threads hammering
//                                  the ring lock (writers and readers, the engine's protocol) and r's lock: allocations stay disjoint,
//                                  the locks end free, contention paths taken; a PC timing (ns a draw).
//   nr_ubo_test --oracle in out    the model run on the cases nr_ubo_oracle.py made (it runs the game's own machine code on the same
//                                  states in Unicorn and compares the NVN calls with every argument and every byte left behind).
// Build: clang -std=c11 -Wall -Wextra -O2 nr_ubo_test.c -o nr_ubo_test
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#define NRU_YIELD_THREAD() SwitchToThread()
#else
#include <pthread.h>
#include <sched.h>
#define NRU_YIELD_THREAD() sched_yield()
#endif

// Guest address space of the test: A1 (renderer, shader record, arrays, global list / blocks, shadows), the device, the ring.
enum { GA = 0x10000000, A1 = 0x3000, DEVA = 0x20000000, DEVN = 0x26000, RINGA = 0x40000000 };
enum { O_R = 0, O_S = 0x1800, O_ARR = 0x1840, ARR_N = 6, O_LIST = 0x1cc0, O_GBLK = 0x1d00, GBLK_N = 8, O_SH = 0x1f00 };
#define RINGN (0x1380000u + 0x20000u)
static uint8_t *g_a1, *g_dev, *g_ring;
static uint8_t *ptr(uint64_t a) {
    if (a >= RINGA && a < RINGA + RINGN) return g_ring + (a - RINGA);
    if (a >= DEVA && a < DEVA + DEVN) return g_dev + (a - DEVA);
    if (a >= GA && a < GA + A1) return g_a1 + (a - GA);
    static uint8_t junk[0x20000]; return junk;   // never reached for a case the model runs (the oracle checks the engine's faults)
}
static int g_ident;   // the arenas sit at their guest addresses (VirtualAlloc / mmap at a fixed address): NRU_PTR is the identity, as in the HLE
#define NRU_PTR(a) (g_ident ? (uint8_t *)(uintptr_t)(a) : ptr((uint64_t)(a)))
#include "../../../nr/nr_ubo.h"

static unsigned long long g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { if (g_fail++ < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static uint64_t g_seed = 0x2545f4914f6cdd1dull;
static uint64_t rnd(void) { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17; return g_seed; }
static void w64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static void w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint64_t r64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t r32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint32_t crc32(const uint8_t *p, size_t n) {   // zlib's (IEEE), as the oracle's zlib.crc32
    static uint32_t t[256]; static int init;
    if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; t[i] = c; } init = 1; }
    uint32_t c = 0xffffffffu; for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xff] ^ (c >> 8); return c ^ 0xffffffffu;
}
enum { DEV_FIELDS = 40 };   // frame u64, ctx u64, head u32, lock u32, cpu u64, gpu u64
static void dev_get(uint8_t *o) { memcpy(o, g_dev + NRU_D_FRAME, 8); memcpy(o + 8, g_dev + NRU_D_CTX, 8); memcpy(o + 16, g_dev + NRU_D_HEAD, 4); memcpy(o + 20, g_dev + NRU_D_LOCK, 4); memcpy(o + 24, g_dev + NRU_D_CPU, 8); memcpy(o + 32, g_dev + NRU_D_GPU, 8); }
static void dev_put(const uint8_t *o) { memcpy(g_dev + NRU_D_FRAME, o, 8); memcpy(g_dev + NRU_D_CTX, o + 8, 8); memcpy(g_dev + NRU_D_HEAD, o + 16, 4); memcpy(g_dev + NRU_D_LOCK, o + 20, 4); memcpy(g_dev + NRU_D_CPU, o + 24, 8); memcpy(g_dev + NRU_D_GPU, o + 32, 8); }
// The two ring windows the oracle compares: [0, W) (where a wrap lands) and [head & ~0xff, + W) clipped to the mapping.
enum { WIN = 0x10000 };
static void ring_windows(uint32_t h0, uint32_t *c0, uint32_t *c1) {
    uint64_t s = h0 & 0xffffff00u; if (s > RINGN) s = RINGN;
    const uint64_t n = s + WIN <= RINGN ? WIN : RINGN - s;
    *c0 = crc32(g_ring, WIN); *c1 = crc32(g_ring + s, (size_t)n);
}
static void ring_clear(uint32_t h0) {
    uint64_t s = h0 & 0xffffff00u; if (s > RINGN) s = RINGN;
    memset(g_ring, 0, WIN); memset(g_ring + s, 0, (size_t)(s + WIN <= RINGN ? WIN : RINGN - s));
}

// ---- the oracle mode ----
// in:  u32 'NRUB', u32 n, then n x { u8 a1[A1]; u8 dev[DEV_FIELDS] }
// out: u32 n, then n x { u32 ran, why, ncall; ncall x 5 u64 (cb stage slot addr size); u8 a1[A1]; u8 dev[DEV_FIELDS]; u32 crc0, crc1 }
static int oracle(const char *in, const char *out) {
    FILE *f = fopen(in, "rb"), *o = fopen(out, "wb");
    if (!f || !o) { printf("cannot open %s / %s\n", in, out); return 2; }
    uint32_t hd[2];
    if (fread(hd, 4, 2, f) != 2 || hd[0] != 0x4255524e) { printf("bad case file\n"); return 2; }
    fwrite(&hd[1], 4, 1, o);
    static nru_run s;
    uint32_t ran_n = 0;
    for (uint32_t i = 0; i < hd[1]; i++) {
        uint8_t dv[DEV_FIELDS];
        if (fread(g_a1, 1, A1, f) != A1 || fread(dv, 1, DEV_FIELDS, f) != DEV_FIELDS) { printf("short case file\n"); return 2; }
        dev_put(dv);
        const uint32_t h0 = r32(g_dev + NRU_D_HEAD);
        const uint32_t ran = (uint32_t)nru_draw(GA + O_R, &s, 0), why = (uint32_t)s.why, nc = ran ? (uint32_t)s.ncall : 0;
        ran_n += ran;
        fwrite(&ran, 4, 1, o); fwrite(&why, 4, 1, o); fwrite(&nc, 4, 1, o);
        for (uint32_t k = 0; k < nc; k++) { const uint64_t v[5] = {s.call[k].cb, s.call[k].stage, s.call[k].slot, s.call[k].addr, s.call[k].size}; fwrite(v, 8, 5, o); }
        fwrite(g_a1, 1, A1, o); dev_get(dv); fwrite(dv, 1, DEV_FIELDS, o);
        uint32_t c[2]; ring_windows(h0, &c[0], &c[1]); fwrite(c, 4, 2, o);
        ring_clear(h0);
    }
    fclose(f); fclose(o);
    printf("nr_ubo_test --oracle: %u cases, model ran %u (sizeof nru_call %zu)\n", hd[1], ran_n, sizeof(nru_call));
    return 0;
}

// ---- the self-contained mode: a random, mostly consistent world (the oracle's generator, smaller) ----
typedef struct { uint64_t frame; int bad; } world;
static void block_init(uint8_t *B, uint64_t frame, int kind) {
    for (int i = 0; i < NRU_BLOCK; i++) B[i] = (uint8_t)rnd();
    w64(B + NRU_B_STAMP, rnd() % 3 ? frame : frame - 1 - rnd() % 3);
    B[NRU_B_DIRTY] = rnd() % 3 ? 0 : (uint8_t)(1 + rnd() % 255);
    B[NRU_B_SLOT] = rnd() % 6 ? (uint8_t)(rnd() % 8) : (uint8_t)(8 + rnd() % 248);
    w32(B + NRU_B_SIZE, rnd() % 8 ? (uint32_t)(16 * (rnd() % 17)) : (uint32_t)(rnd() % 0x100));
    w64(B + NRU_B_SRC, GA + O_SH + rnd() % (A1 - O_SH - 0x110));
    if (kind && rnd() % 5 == 0) w64(B + NRU_B_CPU, 0);   // a shader block never built: not bound
}
static void world_make(world *w) {
    for (uint32_t i = 0; i < A1; i++) g_a1[i] = (uint8_t)rnd();
    uint8_t *R = g_a1 + O_R, *S = g_a1 + O_S;
    w->frame = rnd(); w->bad = 0;
    uint8_t dv[DEV_FIELDS]; w64(dv, w->frame); w64(dv + 8, 0x7100000000ull + (rnd() & 0xfff0));
    const uint32_t hk = (uint32_t)(rnd() % 8);
    w32(dv + 16, hk == 0 ? 0x137ff00u + (uint32_t)(rnd() % 0x100) : hk == 1 ? 0xffffff00u + (uint32_t)(rnd() % 0x100) : hk == 2 ? 0 : (uint32_t)(rnd() % 0x1370000));
    w32(dv + 20, 0); w64(dv + 24, RINGA); w64(dv + 32, 0x8000000000ull + (rnd() & ~0xffull));
    dev_put(dv);
    w64(R + NRU_R_DEV, DEVA);
    w32(R + NRU_R_GLOCK, (uint32_t)(rnd() % 4)); w32(R + NRU_R_GOWNER, 0xffffffffu);
    const uint32_t ng = (uint32_t)(rnd() % 5);
    w64(R + NRU_R_GCOUNT, (rnd() & ~(0xffffffull << 24)) | ((uint64_t)ng << 24));
    w64(R + NRU_R_GLIST, GA + O_LIST);
    for (uint32_t i = 0; i < GBLK_N; i++) block_init(g_a1 + O_GBLK + NRU_BLOCK * i, w->frame, 0);
    for (uint32_t i = 0; i < ng; i++) w64(g_a1 + O_LIST + 8 * i, GA + O_GBLK + NRU_BLOCK * (rnd() % GBLK_N));   // duplicates allowed
    w64(R + NRU_R_SHADER, rnd() % 10 ? GA + O_S : 0);
    for (int st = 0; st < 3; st++) {
        S[NRU_S_CNT + st] = (uint8_t)(rnd() % (ARR_N + 1));
        w64(S + NRU_S_ARR + 8 * st, GA + O_ARR + (uint64_t)st * ARR_N * NRU_BLOCK);
        for (int i = 0; i < ARR_N; i++) block_init(g_a1 + O_ARR + (st * ARR_N + i) * NRU_BLOCK, w->frame, 1);
    }
}

static nru_run g_sim, g_ex;
static unsigned long long g_raced;
static uint8_t g_a1_pre[A1], g_dev_pre[DEVN], g_a1_exec[A1];
static void same_calls(const nru_run *a, const nru_run *b, int *eq) {
    *eq = a->ncall == b->ncall;
    for (int i = 0; *eq && i < a->ncall; i++) *eq = !memcmp(&a->call[i], &b->call[i], sizeof a->call[i]);
}

static void test_sim_vs_exec(int cases) {
    unsigned long long ran = 0, calls = 0, reb = 0, stages[6] = {0}, slots_hi = 0, wraps = 0;
    for (int n = 0; n < cases; n++) {
        world w; world_make(&w);
        memcpy(g_a1_pre, g_a1, A1); memcpy(g_dev_pre, g_dev, DEVN);
        const uint32_t h0 = r32(g_dev + NRU_D_HEAD);
        const int rs = nru_draw(GA + O_R, &g_sim, 1);
        CHECK(!memcmp(g_a1_pre, g_a1, A1) && !memcmp(g_dev_pre, g_dev, DEVN), "case %d: the simulation stored something", n);
        const int re = nru_draw(GA + O_R, &g_ex, 0);
        CHECK(rs == re && g_sim.why == g_ex.why, "case %d: sim %d/%d exec %d/%d", n, rs, g_sim.why, re, g_ex.why);
        if (!re) { CHECK(!memcmp(g_a1_pre, g_a1, A1) && !memcmp(g_dev_pre, g_dev, DEVN), "case %d: a refusal (%d) stored something", n, g_ex.why); ring_clear(h0); continue; }
        ran++; calls += (unsigned long long)g_ex.ncall; reb += g_ex.reb_g + g_ex.reb_s;
        for (int i = 0; i < g_ex.ncall; i++) { stages[g_ex.call[i].stage]++; slots_hi += g_ex.call[i].slot > 7; }
        if (g_sim.head_set && g_sim.head < h0) wraps++;
        int eq; same_calls(&g_sim, &g_ex, &eq);
        CHECK(eq, "case %d: the simulation's calls (%d) are not execution's (%d)", n, g_sim.ncall, g_ex.ncall);
        for (int i = 0; i < g_sim.nvisit; i++) CHECK(!memcmp(ptr(g_sim.vaddr[i]), g_sim.vblk[i], NRU_BLOCK), "case %d: block %d predicted wrongly", n, i);
        if (g_sim.head_set) CHECK(r32(g_dev + NRU_D_HEAD) == g_sim.head, "case %d: head %#x predicted %#x", n, r32(g_dev + NRU_D_HEAD), g_sim.head);
        CHECK(r32(g_dev + NRU_D_LOCK) == 0 && r32(g_a1 + NRU_R_GLOCK) == r32(g_a1_pre + NRU_R_GLOCK), "case %d: a lock did not come back", n);
        int at, raced; const int bad = nru_check(&g_sim, GA + O_R, g_ex.call, g_ex.ncall, &at, &raced);
        CHECK(!bad && !raced, "case %d: nru_check says %#x at %d (raced %d)", n, bad, at, raced);
        // every byte outside the blocks / head / ring copies is the old one
        memcpy(g_a1_exec, g_a1, A1);
        for (int i = 0; i < g_sim.nvisit; i++) memcpy(g_a1_exec + (g_sim.vaddr[i] - GA), g_a1_pre + (g_sim.vaddr[i] - GA), NRU_BLOCK);
        CHECK(!memcmp(g_a1_exec, g_a1_pre, A1), "case %d: a store outside the visited blocks", n);
        // injected differences: a block byte, the head, a ring byte, a call argument
        if (g_sim.nvisit) {
            const int i = (int)(rnd() % (unsigned)g_sim.nvisit), b = (int)(rnd() % NRU_BLOCK);
            uint8_t *p = ptr(g_sim.vaddr[i]) + b; *p ^= 1;
            CHECK(nru_check(&g_sim, GA + O_R, g_ex.call, g_ex.ncall, &at, &raced) & NRU_BAD_BLOCK, "case %d: a flipped block byte not caught", n);
            *p ^= 1;
        }
        int nz = -1; for (int i = 0; i < g_sim.ncopy; i++) if (g_sim.copy[i].n) { nz = i; break; }
        if (nz >= 0) {
            uint8_t *p = ptr(g_sim.copy[nz].dst) + rnd() % g_sim.copy[nz].n; *p ^= 0x80;
            CHECK(nru_check(&g_sim, GA + O_R, g_ex.call, g_ex.ncall, &at, &raced) & NRU_BAD_RING, "case %d: a flipped ring byte not caught", n);
            *p ^= 0x80;
        }
        if (g_ex.ncall) {
            const int k = (int)(rnd() % (unsigned)g_ex.ncall); nru_call *p = &g_ex.call[k]; const nru_call old = *p;
            switch (rnd() % 4) { case 0: p->slot ^= 1; break; case 1: p->addr += 0x100; break; case 2: p->size ^= 4; break; default: p->stage = p->stage == 5 ? 1 : 5; }
            CHECK(nru_check(&g_sim, GA + O_R, g_ex.call, g_ex.ncall, &at, &raced) & NRU_BAD_CALLS, "case %d: a changed call argument not caught", n);
            *p = old;
            CHECK(nru_check(&g_sim, GA + O_R, g_ex.call, g_ex.ncall - 1, &at, &raced) & NRU_BAD_CALLS, "case %d: a missing call not caught", n);
        }
        if (g_sim.head_set) {   // a foreign allocation before the engine's run: raced (offsets not comparable), not bad
            memcpy(g_a1, g_a1_pre, A1); memcpy(g_dev, g_dev_pre, DEVN); ring_clear(h0);
            w32(g_dev + NRU_D_HEAD, (r32(g_dev + NRU_D_HEAD) + 0x400u) % 0x1000000u);
            nru_draw(GA + O_R, &g_ex, 0);
            CHECK(!nru_check(&g_sim, GA + O_R, g_ex.call, g_ex.ncall, &at, &raced), "case %d: a foreign allocation before the run reported as bad", n);
            g_raced += raced;
            ring_clear((r32(g_dev_pre + NRU_D_HEAD) + 0x400u) % 0x1000000u);
        }
        ring_clear(h0);
    }
    printf("sim vs exec: %d cases, ran %llu, %llu calls (stage 0 %llu, 1 %llu, 5 %llu; slot > 7 %llu), %llu rebuilds, %llu wraps; a foreign allocation in between: raced %llu, "
           "bad 0\n", cases, ran, calls, stages[0], stages[1], stages[5], slots_hi, reb, wraps, g_raced);
    CHECK(ran > (unsigned long long)cases / 3 && stages[0] && stages[1] && stages[5] && slots_hi && wraps && g_raced, "coverage");
}

// Every refusal: nothing stored, the result names the reason.
static void test_refusals(void) {
    struct { int why; const char *what; } k[] = {{NRU_W_NODEV, "no device"}, {NRU_W_NOCTX, "ctx 0"}, {NRU_W_NORING, "ring 0"}, {NRU_W_OWNER, "owner"},
                                                 {NRU_W_COUNT, "17 global blocks"}, {NRU_W_COUNT, "17 shader blocks"}, {NRU_W_NULL, "null array"},
                                                 {NRU_W_NULL, "null list"}, {NRU_W_NULL, "null block"}, {NRU_W_SIZE, "size"}, {NRU_W_NULL, "null shadow"},
                                                 {NRU_W_OVERLAP, "shadow in the ring"}, {NRU_W_OVERLAP, "array over the header"}, {NRU_W_OVERLAP, "global = shader block"},
                                                 {NRU_W_OVERLAP, "global blocks half over each other"}, {NRU_W_OVERLAP, "a shadow inside a global block"},
                                                 {NRU_W_OVERLAP, "a block over the list"}, {NRU_W_OVERLAP, "arrays over each other"}};
    for (unsigned t = 0; t < sizeof k / sizeof k[0]; t++) {
        for (int rep = 0; rep < 20; rep++) {
            world w; world_make(&w);
            uint8_t *R = g_a1 + O_R, *S = g_a1 + O_S;
            w64(R + NRU_R_SHADER, GA + O_S); S[NRU_S_CNT] = 2; S[NRU_S_CNT + 1] = 2;
            for (int i = 0; i < 2; i++) { uint8_t *B = g_a1 + O_ARR + i * NRU_BLOCK; B[NRU_B_SLOT] = 1; w32(B + NRU_B_SIZE, 64); }
            w64(R + NRU_R_GCOUNT, 2ull << 24); w64(g_a1 + O_LIST, GA + O_GBLK); w64(g_a1 + O_LIST + 8, GA + O_GBLK + NRU_BLOCK);
            switch (t) {
            case 0: w64(R + NRU_R_DEV, 0); break;
            case 1: w64(g_dev + NRU_D_CTX, 0); break;
            case 2: w64(g_dev + NRU_D_CPU, 0); break;
            case 3: w32(R + NRU_R_GOWNER, 0x1234); break;
            case 4: w64(R + NRU_R_GCOUNT, 17ull << 24); break;
            case 5: S[NRU_S_CNT + 2] = 17; break;
            case 6: w64(S + NRU_S_ARR + 8, 0); break;
            case 7: w64(R + NRU_R_GLIST, 0); break;
            case 8: w64(g_a1 + O_LIST + 8, 0); break;
            case 9: w32(g_a1 + O_ARR + NRU_B_SIZE, NRU_SIZE_MAX + 1); break;
            case 10: w64(g_a1 + O_ARR + NRU_BLOCK + NRU_B_SRC, 0); break;
            case 11: w64(g_a1 + O_GBLK + NRU_B_SRC, RINGA + 0x1000); w32(g_a1 + O_GBLK + NRU_B_SIZE, 64); break;
            case 12: w64(S + NRU_S_ARR, GA + O_S + 0x10); break;
            case 13: w64(g_a1 + O_LIST + 8, GA + O_ARR + NRU_BLOCK); break;
            case 14: w64(g_a1 + O_LIST + 8, GA + O_GBLK + 0x20); break;
            case 15: w64(g_a1 + O_ARR + NRU_B_SRC, GA + O_GBLK + 0x30); break;
            case 16: w64(g_a1 + O_LIST + 8, GA + O_LIST - 0x20); w32(g_a1 + O_LIST + 0x10, 64); break;   // its size = list entry 2 (unused)
            default: w64(S + NRU_S_ARR + 8, GA + O_ARR + NRU_BLOCK); break;
            }
            memcpy(g_a1_pre, g_a1, A1); memcpy(g_dev_pre, g_dev, DEVN);
            // the simulation (=2) refuses every kind; execution runs the two whose effect it reproduces as the machine code does (a shadow
            // over the ring: the engine's own copy; a shadow inside a global block: read when the engine reads it) - the oracle compares those
            const int sran = nru_draw(GA + O_R, &g_sim, 1), exec_runs = t == 11 || t == 15;
            CHECK(!sran && g_sim.why == k[t].why, "refusal '%s' (sim): ran %d why %d (%s)", k[t].what, sran, g_sim.why, k_nru_why[g_sim.why]);
            CHECK(!memcmp(g_a1_pre, g_a1, A1) && !memcmp(g_dev_pre, g_dev, DEVN), "refusal '%s' (sim) stored something", k[t].what);
            const int ran = nru_draw(GA + O_R, &g_ex, 0);
            if (exec_runs) { CHECK(ran, "'%s': execution refused (%d)", k[t].what, g_ex.why); ring_clear(r32(g_dev_pre + NRU_D_HEAD)); continue; }
            CHECK(!ran && g_ex.why == k[t].why, "refusal '%s': ran %d why %d (%s)", k[t].what, ran, g_ex.why, k_nru_why[g_ex.why]);
            CHECK(!memcmp(g_a1_pre, g_a1, A1) && !memcmp(g_dev_pre, g_dev, DEVN), "refusal '%s' stored something", k[t].what);
        }
    }
    printf("refusals: %zu kinds x 20 (the simulation refuses all, execution 16 - the other two it runs as the machine code does), nothing stored\n", sizeof k / sizeof k[0]);
}

// ---- threads: the engine's ring protocol from other threads (writers as 0x1bed3b8 allocate, readers as the low bits), r's writer ----
typedef struct { volatile int stop; uint64_t allocs; uint32_t mine[4096][2]; uint32_t nmine; } peer;
static peer g_peer[3];
static volatile long g_go;
#ifdef _WIN32
typedef HANDLE thr_t;
static DWORD WINAPI thr_tramp(LPVOID a);
#else
typedef pthread_t thr_t;
static void *thr_tramp(void *a);
#endif
static void (*g_thr_fn[4])(int);
typedef struct { int k; } thr_arg;
static thr_arg g_thr_arg[4];
#ifdef _WIN32
static DWORD WINAPI thr_tramp(LPVOID a) { const int k = ((thr_arg *)a)->k; g_thr_fn[k](k); return 0; }
static thr_t thr_start(int k) { return CreateThread(NULL, 0, thr_tramp, &g_thr_arg[k], 0, NULL); }
static void thr_join(thr_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
static void *thr_tramp(void *a) { const int k = ((thr_arg *)a)->k; g_thr_fn[k](k); return NULL; }
static thr_t thr_start(int k) { thr_t t; pthread_create(&t, NULL, thr_tramp, &g_thr_arg[k]); return t; }
static void thr_join(thr_t t) { pthread_join(t, NULL); }
#endif
static void peer_writer(int k) {   // another ring user: writer lock, 0x80 bytes, as main+0x1bed3b8 (no size in its wrap test)
    peer *p = &g_peer[k]; nru_run tmp; memset(&tmp, 0, sizeof tmp);
    while (!g_go) NRU_SPIN_HINT();
    while (!p->stop) {
        if (p->nmine >= 4096) { nru_wlock(&tmp, g_dev + NRU_D_LOCK); nru_wunlock(g_dev + NRU_D_LOCK); p->allocs++; continue; }   // the lock only
        nru_wlock(&tmp, g_dev + NRU_D_LOCK);
        uint32_t h = (r32(g_dev + NRU_D_HEAD) + 0xffu) & 0xffffff00u;
        if (((r32(g_dev + NRU_D_HEAD) + 0xffu) >> 19) > 0x26) h = 0;
        __atomic_store_n((uint32_t *)(void *)(g_dev + NRU_D_HEAD), h, __ATOMIC_RELAXED);
        const uint32_t old = __atomic_fetch_add((uint32_t *)(void *)(g_dev + NRU_D_HEAD), 0x80u, __ATOMIC_ACQ_REL);
        nru_wunlock(g_dev + NRU_D_LOCK);
        if (p->nmine < 4096) { p->mine[p->nmine][0] = old; p->mine[p->nmine][1] = 0x80; p->nmine++; }
        memset(g_ring + old, 0xa0 + k, 0x80);
        p->allocs++;
    }
}
static void peer_reader(int k) {   // readers of the ring lock (bits 0..19): the model's writer must wait for them
    peer *p = &g_peer[k];
    while (!g_go) NRU_SPIN_HINT();
    while (!p->stop) {
        uint32_t *lk = (uint32_t *)(void *)(g_dev + NRU_D_LOCK);
        for (;;) { while (__atomic_load_n(lk, __ATOMIC_ACQUIRE) >= NRU_LOCK_W) NRU_SPIN_HINT();
                   if (__atomic_add_fetch(lk, 1, __ATOMIC_ACQ_REL) < NRU_LOCK_W) break; __atomic_fetch_sub(lk, 1, __ATOMIC_ACQ_REL); }
        for (int i = 0; i < 50; i++) NRU_SPIN_HINT();
        __atomic_fetch_sub(lk, 1, __ATOMIC_ACQ_REL);
        p->allocs++;
    }
}
static void peer_rwriter(int k) {   // a writer of r's lock (the global list's owner side): the model's reader must wait for it
    peer *p = &g_peer[k]; nru_run tmp; memset(&tmp, 0, sizeof tmp);
    while (!g_go) NRU_SPIN_HINT();
    while (!p->stop) {
        nru_wlock(&tmp, g_a1 + O_R + NRU_R_GLOCK);   // the same writer protocol on r+0xbe0 (bit 20 up), readers drained
        for (int i = 0; i < 30; i++) NRU_SPIN_HINT();
        nru_wunlock(g_a1 + O_R + NRU_R_GLOCK);
        p->allocs++;
        for (int i = 0; i < 200; i++) NRU_SPIN_HINT();
    }
}
static int cmp_u32p(const void *a, const void *b) { const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b; return x[0] < y[0] ? -1 : x[0] > y[0]; }
static void test_threads(void) {
    world w; world_make(&w);
    uint8_t *R = g_a1 + O_R, *S = g_a1 + O_S;
    w64(g_dev + NRU_D_FRAME, 77); w32(g_dev + NRU_D_HEAD, 0); w32(g_dev + NRU_D_LOCK, 0); w32(R + NRU_R_GLOCK, 0);
    w64(R + NRU_R_SHADER, GA + O_S); S[NRU_S_CNT] = 3; S[NRU_S_CNT + 1] = 2; S[NRU_S_CNT + 2] = 0;
    w64(R + NRU_R_GCOUNT, 1ull << 24); w64(g_a1 + O_LIST, GA + O_GBLK);
    for (int i = 0; i < 5; i++) { uint8_t *B = g_a1 + O_ARR + (i < 3 ? i : ARR_N + i - 3) * NRU_BLOCK; B[NRU_B_SLOT] = (uint8_t)i; w32(B + NRU_B_SIZE, 0x60);
        w64(B + NRU_B_SRC, GA + O_SH + 0x100 * (uint64_t)i); }
    { uint8_t *B = g_a1 + O_GBLK; B[NRU_B_SLOT] = 7; w32(B + NRU_B_SIZE, 0x40); w64(B + NRU_B_SRC, GA + O_SH + 0x800); }
    static uint32_t got[60000][2]; uint32_t ng = 0;
    for (int k = 0; k < 3; k++) { memset(&g_peer[k], 0, sizeof g_peer[k]); g_thr_arg[k].k = k; }
    g_thr_fn[0] = peer_writer; g_thr_fn[1] = peer_reader; g_thr_fn[2] = peer_rwriter;
    thr_t t[3]; g_go = 0;
    for (int k = 0; k < 3; k++) t[k] = thr_start(k);
    g_go = 1;
    unsigned long long sw = 0, sd = 0, sr = 0, yl = 0, draws = 0;
    for (int it = 0; it < 3000 && ng + 8 < 60000; it++) {
        for (int i = 0; i < 5; i++) g_a1[O_ARR + (i < 3 ? i : ARR_N + i - 3) * NRU_BLOCK + NRU_B_DIRTY] = 1;
        if (it % 50 == 0) g_a1[O_GBLK + NRU_B_DIRTY] = 1;
        CHECK(nru_draw(GA + O_R, &g_ex, 0), "threads: refused (%d)", g_ex.why);
        draws++; sw += g_ex.spin_w; sd += g_ex.spin_d; sr += g_ex.spin_r; yl += g_ex.yields;
        for (int i = 0; i < 5; i++) { const uint8_t *B = g_a1 + O_ARR + (i < 3 ? i : ARR_N + i - 3) * NRU_BLOCK;
            got[ng][0] = (uint32_t)(r64(B + NRU_B_CPU) - RINGA); got[ng][1] = 0x60; ng++;
            CHECK(!memcmp(ptr(r64(B + NRU_B_CPU)), ptr(r64(B + NRU_B_SRC)), 0x60), "threads: a block's ring bytes are not its shadow (overwritten by a peer)"); }
        if (it % 50 == 0) { const uint8_t *B = g_a1 + O_GBLK; got[ng][0] = (uint32_t)(r64(B + NRU_B_CPU) - RINGA); got[ng][1] = 0x40; ng++; }
    }
    for (int k = 0; k < 3; k++) g_peer[k].stop = 1;
    for (int k = 0; k < 3; k++) thr_join(t[k]);
    // every allocation (the model's and the peer writer's) is disjoint: no wrap at these totals (checked)
    static uint32_t all[70000][2]; uint32_t na = 0;
    for (uint32_t i = 0; i < ng; i++) { all[na][0] = got[i][0]; all[na][1] = got[i][1]; na++; }
    for (uint32_t i = 0; i < g_peer[0].nmine && na < 70000; i++) { all[na][0] = g_peer[0].mine[i][0]; all[na][1] = g_peer[0].mine[i][1]; na++; }
    qsort(all, na, sizeof all[0], cmp_u32p);
    uint32_t over = 0; for (uint32_t i = 1; i < na; i++) over += all[i - 1][0] + all[i - 1][1] > all[i][0];
    CHECK(r32(g_dev + NRU_D_HEAD) < 0x1300000u, "threads: the test wrapped the ring (head %#x)", r32(g_dev + NRU_D_HEAD));
    CHECK(!over, "threads: %u allocations overlap", over);
    CHECK(r32(g_dev + NRU_D_LOCK) == 0 && r32(R + NRU_R_GLOCK) == 0, "threads: locks end %#x / %#x", r32(g_dev + NRU_D_LOCK), r32(R + NRU_R_GLOCK));
    printf("threads: %llu draws by the model with a peer ring writer (%llu allocations), ring readers (%llu holds), an r-lock writer (%llu holds): %u allocations disjoint, "
           "locks free; contention: waited for a ring writer %llu, for ring readers %llu, for r's writer %llu, YieldThread %llu\n",
           draws, (unsigned long long)g_peer[0].allocs, (unsigned long long)g_peer[1].allocs, (unsigned long long)g_peer[2].allocs, na, sw, sd, sr, yl);
    CHECK(sw + sd > 0 && sr > 0, "threads: the contention paths were not taken (writer %llu readers %llu r %llu)", sw, sd, sr);
}

static double now_s(void) { struct timespec t; timespec_get(&t, TIME_UTC); return (double)t.tv_sec + t.tv_nsec * 1e-9; }
// A Fort-Joy-like draw: one global block (rebuilt once a frame), stage 0: 2 blocks, stage 1: 2 blocks; one or two shader blocks dirty a
// draw (the object's constants, 64-256 B), the rest bound again with their range.
static void bench(void) {
    world w; world_make(&w);
    uint8_t *R = g_a1 + O_R, *S = g_a1 + O_S;
    w64(g_dev + NRU_D_FRAME, 5); w32(g_dev + NRU_D_HEAD, 0); w32(g_dev + NRU_D_LOCK, 0); w32(R + NRU_R_GLOCK, 0); w32(R + NRU_R_GOWNER, 0xffffffffu);
    w64(R + NRU_R_SHADER, GA + O_S); S[NRU_S_CNT] = 2; S[NRU_S_CNT + 1] = 2; S[NRU_S_CNT + 2] = 0;
    w64(R + NRU_R_GCOUNT, 1ull << 24); w64(g_a1 + O_LIST, GA + O_GBLK);
    const uint32_t sz[4] = {0xc0, 0x40, 0x100, 0x30};
    for (int i = 0; i < 4; i++) { uint8_t *B = g_a1 + O_ARR + (i < 2 ? i : ARR_N + i - 2) * NRU_BLOCK; B[NRU_B_SLOT] = (uint8_t)(i & 1); w32(B + NRU_B_SIZE, sz[i]);
        w64(B + NRU_B_SRC, GA + O_SH + 0x100 * (uint64_t)i); w64(B + NRU_B_STAMP, 5); B[NRU_B_DIRTY] = 1; }
    { uint8_t *B = g_a1 + O_GBLK; B[NRU_B_SLOT] = 2; w32(B + NRU_B_SIZE, 0x100); w64(B + NRU_B_SRC, GA + O_SH + 0x500); w64(B + NRU_B_STAMP, 5); B[NRU_B_DIRTY] = 0; }
    const int N = 2000000;
    double best = 1e9, bests = 1e9; unsigned long long sink = 0;
    for (int rep = 0; rep < 5; rep++) {
        const double t0 = now_s();
        for (int i = 0; i < N; i++) {
            g_a1[O_ARR + NRU_B_DIRTY] = 1; if (i & 1) g_a1[O_ARR + ARR_N * NRU_BLOCK + NRU_B_DIRTY] = 1;   // 1.5 dirty shader blocks a draw
            if ((i & 1023) == 0) { w64(g_dev + NRU_D_FRAME, 6 + (uint64_t)i); }                      // a new frame: everything rebuilt once
            if (r32(g_dev + NRU_D_HEAD) > 0x1000000u) w32(g_dev + NRU_D_HEAD, 0);
            sink += (unsigned long long)nru_draw(GA + O_R, &g_ex, 0) + (unsigned long long)g_ex.ncall;
        }
        const double t1 = now_s(), ns = (t1 - t0) * 1e9 / N; if (ns < best) best = ns;
        const double t2 = now_s();
        for (int i = 0; i < N / 4; i++) { g_a1[O_ARR + NRU_B_DIRTY] = 1; sink += (unsigned long long)nru_draw(GA + O_R, &g_sim, 1); }
        const double ns2 = (now_s() - t2) * 1e9 / (N / 4); if (ns2 < bests) bests = ns2;
    }
    printf("bench (PC, best of 5, guest addresses %s): the model executing one draw's 0x926f90 + 0x925740 (1 global block, 2 + 2 shader blocks, 1.5 rebuilt, %d BindUniformBuffer "
           "calls handed back): %.1f ns a draw; the =2 simulation %.1f ns (sink %llu)\n", g_ident ? "identity, as in the HLE" : "mapped (slower)",
           g_ex.ncall, best, bests, sink & 1);
}

int main(int argc, char **argv) {
#ifdef _WIN32
    g_a1 = (uint8_t *)VirtualAlloc((void *)(uintptr_t)GA, 0x4000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_dev = (uint8_t *)VirtualAlloc((void *)(uintptr_t)DEVA, DEVN, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_ring = (uint8_t *)VirtualAlloc((void *)(uintptr_t)RINGA, RINGN, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_ident = g_a1 == (uint8_t *)(uintptr_t)GA && g_dev == (uint8_t *)(uintptr_t)DEVA && g_ring == (uint8_t *)(uintptr_t)RINGA;
#endif
    if (!g_ident) { g_a1 = (uint8_t *)calloc(1, A1); g_dev = (uint8_t *)calloc(1, DEVN); g_ring = (uint8_t *)calloc(1, RINGN); }
    if (!g_a1 || !g_dev || !g_ring) { printf("no memory\n"); return 2; }
    if (argc == 4 && !strcmp(argv[1], "--oracle")) return oracle(argv[2], argv[3]);
    test_sim_vs_exec(argc > 1 ? atoi(argv[1]) : 20000);
    test_refusals();
    test_threads();
    bench();
    printf("%s: %llu checks, %llu failed\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
