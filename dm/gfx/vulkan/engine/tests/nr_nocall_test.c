// NR-23 (DM_NR_NOCALL): host test of dm/gfx/nr/nr_nocall.h - the model of the engine's texture flush and of the tail of its two draw
// functions. Two modes:
//   nr_nocall_test                      self-contained: the model against an independent reference written from NATIVE_PLAN.md 1.7
//                                       (per stage / per slot rules), over random renderer states; eligibility; read-only planning.
//   nr_nocall_test --oracle in out      the model run on the cases nr_nocall_oracle.py made (it runs the game's own machine code on the
//                                       same states in an emulator and compares the NVN calls and every byte the code leaves behind).
// Build: clang -std=c11 -Wall -Wextra -O1 nr_nocall_test.c -o nr_nocall_test
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Guest memory = one arena at guest address GA (the oracle maps the same bytes at the same address in the emulator).
enum { GA = 0x10000000, ARENA = 0x10000, A_R = 0, A_DEV = 0x2000, A_CTX1 = 0x9000, A_CTX2 = 0x9800, A_PAGES = 0xa000, A_GENS = 0xa100, A_GENPG = 0xa200, A_RECPG = 0xb000 };
static uint8_t *g_arena;
#define NRNC_PTR(a) (g_arena + ((uint64_t)(a) - GA))
#include "../../../nr/nr_nocall.h"

static unsigned long long g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { if (g_fail++ < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t g_seed = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17; return g_seed; }
static void w64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static void w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

// The recorder: every NVN call of the model, in order.
enum { K_BIND = 1, K_REN, K_DRAW, K_UBO1, K_UBO2 };
typedef struct { uint64_t v[8]; } call_t;
static call_t g_calls[256]; static int g_ncall;
static void put(uint64_t k, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g) {
    if (g_ncall < 256) { call_t *x = &g_calls[g_ncall]; x->v[0] = k; x->v[1] = a; x->v[2] = b; x->v[3] = c; x->v[4] = d; x->v[5] = e; x->v[6] = f; x->v[7] = g; }
    g_ncall++;
}
static void r_bind(void *u, uint64_t cb, int stage, int slot, int32_t tex, int32_t smp) { (void)u; put(K_BIND, cb, (uint64_t)stage, (uint64_t)slot, (uint64_t)(int64_t)tex, (uint64_t)(int64_t)smp, 0, 0); }
static void r_ren(void *u, uint64_t cb) { (void)u; put(K_REN, cb, 1, 0, 0, 0, 0, 0); }
static void r_draw(void *u, uint64_t cb, int32_t prim, uint32_t itype, int32_t count, uint64_t ib, int32_t inst) {
    (void)u; put(K_DRAW, cb, (uint64_t)(int64_t)prim, itype, (uint64_t)(int64_t)count, ib, (uint64_t)(int64_t)inst, 0);
}
static const nrnc_ops k_rec = {r_bind, r_ren, r_draw};

// One case on the arena as it is. kind 0 = the flush, 36 / 37 = the draw function (a1, a2, a4 = its w1, w2, w4); swap = the first
// uniform function replaces the device's recording context (the oracle's stub does the same): the draw must use the old one, a
// SetRenderEnable the new one. Returns 1 = ran, 0 = the model leaves the case to the engine (nothing written, no call).
static int run_case(int kind, int32_t a1, int32_t a2, int32_t a4, int swap, const uint8_t *isz) {
    uint8_t *R = g_arena + A_R;
    g_ncall = 0;
    nrnc_plan p;
    if (kind == 0) {
        const uint64_t cb = nrnc_cb(R);
        if (!cb || !nrnc_flush_plan(R, &p)) return 0;
        nrnc_flush_run(R, &p, cb, &k_rec, NULL);
        return 1;
    }
    nrnc_draw d;
    const int32_t first = kind == 36 ? a1 : 0, count = kind == 36 ? a2 : a4, inst = kind == 36 ? -1 : a2;
    if (!nrnc_draw_pre(R, kind, first, count, inst, &d)) return 0;
    // the front end would call the engine's flush when the plan is not eligible; the oracle only makes cases whose plan is
    {
        uint8_t save[ARENA]; memcpy(save, g_arena, ARENA);
        const int ok = nrnc_flush_plan(R, &p) && nrnc_cb(R);
        CHECK(!memcmp(save, g_arena, ARENA), "planning wrote guest memory");
        if (!ok) return 0;
    }
    if (d.ubo) {
        put(K_UBO1, GA + A_R, 0, 0, 0, 0, 0, 0);
        if (swap) w64(g_arena + A_DEV + NRNC_DEV_CTX, GA + A_CTX2);
        put(K_UBO2, GA + A_R, 0, 0, 0, 0, 0, 0);
    }
    const uint64_t fcb = nrnc_cb(R);
    if (!fcb || !nrnc_flush_plan(R, &p)) return 0;   // (cannot happen: the stub changes the context only)
    nrnc_flush_run(R, &p, fcb, &k_rec, NULL);
    nrnc_draw_tail(R, &d, isz, &k_rec, NULL);
    return 1;
}

// ---- the oracle mode ----
// in:  u32 magic 'NRNC', u32 n, 32 bytes index size table, then n x { i32 kind, a1, a2, a4, swap, pad[3]; u8 arena[ARENA] }
// out: u32 n, then n x { u32 ran, u32 ncalls, call_t calls[ncalls], u8 arena[ARENA] }
static int oracle(const char *in, const char *out) {
    FILE *f = fopen(in, "rb"), *o = fopen(out, "wb");
    if (!f || !o) { printf("cannot open %s / %s\n", in, out); return 2; }
    uint32_t hd[2]; uint8_t isz[32];
    if (fread(hd, 4, 2, f) != 2 || hd[0] != 0x434e524e || fread(isz, 1, 32, f) != 32) { printf("bad case file\n"); return 2; }
    fwrite(&hd[1], 4, 1, o);
    g_arena = malloc(ARENA);
    for (uint32_t i = 0; i < hd[1]; i++) {
        int32_t c[8];
        if (fread(c, 4, 8, f) != 8 || fread(g_arena, 1, ARENA, f) != ARENA) { printf("short case file\n"); return 2; }
        const uint32_t ran = (uint32_t)run_case(c[0], c[1], c[2], c[3], c[4], isz), n = ran ? (uint32_t)g_ncall : 0;
        fwrite(&ran, 4, 1, o); fwrite(&n, 4, 1, o); fwrite(g_calls, sizeof(call_t), n, o); fwrite(g_arena, 1, ARENA, o);
    }
    fclose(f); fclose(o);
    printf("nr_nocall_test --oracle: %u cases run\n", hd[1]);
    return 0;
}

// ---- the self-contained mode ----
static const uint8_t k_isz[32] = {1, 0, 0, 0, 2, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0};   // main + 0x1f5865c
static void random_state(int *per_out, uint32_t *idx_out, int valid_ib) {
    uint8_t *R = g_arena + A_R;
    for (int i = 0; i < ARENA; i += 8) w64(g_arena + i, rnd());
    w64(R + NRNC_R_DEV, GA + A_DEV);
    w64(g_arena + A_DEV + NRNC_DEV_CTX, GA + A_CTX1);
    for (int s = 0; s < NRNC_STAGES; s++) {
        const unsigned m = (unsigned)(rnd() % 4);
        int lo = (int)(rnd() % 32), hi = (int)(rnd() % 32);
        if (m == 0) { lo = 0x20; hi = -1; } else if (lo > hi) { const int t = lo; lo = hi; hi = t; }
        R[k_nrnc_stage[s].range] = (uint8_t)lo; R[k_nrnc_stage[s].range + 1] = (uint8_t)hi;
        for (int i = 0; i < 32; i++) if (rnd() % 3 == 0) w32(R + k_nrnc_stage[s].tex + 4 * i, 0xffffffffu);
    }
    const uint32_t per = 1 + (uint32_t)(rnd() % 16), page = (uint32_t)(rnd() % 4), off = (uint32_t)(rnd() % per), idx = page * per + off;
    const uint32_t gen = 1 + (uint32_t)(rnd() % 0x3ffffe);
    w32(R + NRNC_R_IB_PER, per); w64(R + NRNC_R_IB_PAGES, GA + A_PAGES); w64(R + NRNC_R_IB_GENS, GA + A_GENS);
    for (uint32_t pg = 0; pg < 4; pg++) { w64(g_arena + A_PAGES + 8 * pg, GA + A_RECPG + 0x800 * pg); w64(g_arena + A_GENS + 8 * pg, GA + A_GENPG + 0x100 * pg); }
    w32(g_arena + A_GENPG + 0x100 * page + 4 * off, valid_ib ? gen : gen ^ 1);
    w64(R + NRNC_R_IB_HANDLE, (rnd() << 54) | (uint64_t)gen << 32 | idx);
    w32(g_arena + A_RECPG + 0x800 * page + NRNC_IB_REC * off + NRNC_IB_TYPE, (uint32_t)(rnd() % 8));
    R[NRNC_R_REN_NEED] = (uint8_t)(rnd() % 3 ? rnd() % 2 : rnd()); R[NRNC_R_REN_OFF] = (uint8_t)(rnd() % 3 ? rnd() % 2 : rnd());
    if (rnd() & 1) w64(R + NRNC_R_SHADER_REC, 0);
    *per_out = (int)per; *idx_out = idx;
}
static int self_test(void) {
    g_arena = malloc(ARENA);
    uint8_t *save = malloc(ARENA), *R = g_arena + A_R;
    int per; uint32_t idx;
    // 1. the flush: the reference is the rule of NATIVE_PLAN.md 1.7 applied slot by slot
    for (int it = 0; it < 20000; it++) {
        random_state(&per, &idx, 1);
        const int wild = it % 9 == 0;
        if (wild) { const int s = (int)(rnd() % 3); R[k_nrnc_stage[s].range + 1] = (uint8_t)(32 + rnd() % 90); R[k_nrnc_stage[s].range] = (uint8_t)(rnd() % 32); }
        if (it % 13 == 0) { const int s = (int)(rnd() % 3); R[k_nrnc_stage[s].range] = (uint8_t)(0x80 + rnd() % 0x7f); R[k_nrnc_stage[s].range + 1] = (uint8_t)(rnd() % 32); }
        memcpy(save, g_arena, ARENA);
        nrnc_plan p; const int ok = nrnc_flush_plan(R, &p);
        CHECK(!memcmp(save, g_arena, ARENA), "nrnc_flush_plan wrote guest memory");
        int elig = 1;
        for (int s = 0; s < 3; s++) { const int lo = (int8_t)R[k_nrnc_stage[s].range], hi = (int8_t)R[k_nrnc_stage[s].range + 1]; if (lo <= hi && (lo < 0 || hi > 31)) elig = 0; }
        CHECK(ok == elig, "eligibility %d, expected %d", ok, elig);
        if (!ok) continue;
        static const int stage_no[3] = {0, 1, 5}; static const int tex[3] = {0x1558, 0x15e0, 0x1668}, smp[3] = {0x13c0, 0x1448, 0x14d0}, rng[3] = {0x15dc, 0x1664, 0x16ec}, srng[3] = {0x1444, 0x14cc, 0x1554};
        int n = 0;
        for (int s = 0; s < 3; s++) {
            const int lo = (int8_t)R[rng[s]], hi = (int8_t)R[rng[s] + 1];
            int cnt = 0;
            for (int i = lo; i <= hi; i++) {
                int32_t t, sm; memcpy(&t, R + tex[s] + 4 * i, 4); memcpy(&sm, R + smp[s] + 4 * i, 4);
                if (t == -1) continue;
                CHECK(n < p.n && p.b[n].stage == stage_no[s] && p.b[n].slot == i && p.b[n].tex == t && p.b[n].smp == sm, "bind %d of the plan", n);
                n++; cnt++;
            }
            CHECK(p.per[s] == cnt, "binds of stage %d", s);
        }
        CHECK(n == p.n, "plan has %d binds, reference %d", p.n, n);
        g_ncall = 0;
        const uint64_t cb = nrnc_cb(R);
        CHECK(cb == GA + A_CTX1 + 0x38, "command buffer");
        nrnc_flush_run(R, &p, cb, &k_rec, NULL);
        CHECK(g_ncall == p.n, "run made %d calls for %d binds", g_ncall, p.n);
        for (int i = 0; i < p.n && i < g_ncall; i++)
            CHECK(g_calls[i].v[0] == K_BIND && g_calls[i].v[1] == cb && g_calls[i].v[2] == p.b[i].stage && g_calls[i].v[3] == p.b[i].slot && (int32_t)g_calls[i].v[4] == p.b[i].tex && (int32_t)g_calls[i].v[5] == p.b[i].smp, "call %d", i);
        CHECK(nrnc_flush_clean(R), "ranges clean after the run");
        for (int s = 0; s < 3; s++) { save[rng[s]] = 0x20; save[rng[s] + 1] = 0xff; save[srng[s]] = 0x20; save[srng[s] + 1] = 0xff; }
        CHECK(!memcmp(save, g_arena, ARENA), "the flush wrote something besides the six ranges");
        nrnc_plan q; CHECK(nrnc_flush_plan(R, &q) && q.n == 0, "a second flush binds nothing");
    }
    // 2. no device / no context: not eligible
    random_state(&per, &idx, 1); w64(R + NRNC_R_DEV, 0); CHECK(nrnc_cb(R) == 0, "no device");
    random_state(&per, &idx, 1); w64(g_arena + A_DEV + NRNC_DEV_CTX, 0); CHECK(nrnc_cb(R) == 0, "no context");
    // 3. the draw functions
    for (int it = 0; it < 20000; it++) {
        const int valid = it % 7 != 0, op = it & 1 ? 37 : 36;
        random_state(&per, &idx, valid);
        const int kill = it % 11;   // other ways to have no record
        if (kill == 1) { uint64_t h; memcpy(&h, R + NRNC_R_IB_HANDLE, 8); h &= ~0x3fffff00000000ull; w64(R + NRNC_R_IB_HANDLE, h); }
        if (kill == 2) w32(R + NRNC_R_IB_PER, 0);
        if (kill == 3) w64(g_arena + A_DEV + NRNC_DEV_CTX, 0);
        const int32_t a1 = (int32_t)rnd(), a2 = (int32_t)rnd(), a4 = (int32_t)rnd();
        memcpy(save, g_arena, ARENA);
        nrnc_draw d;
        const int32_t first = op == 36 ? a1 : 0, count = op == 36 ? a2 : a4, inst = op == 36 ? -1 : a2;
        const int ok = nrnc_draw_pre(R, op, first, count, inst, &d);
        CHECK(!memcmp(save, g_arena, ARENA), "nrnc_draw_pre wrote guest memory");
        CHECK(ok == (valid && kill != 1 && kill != 2 && kill != 3), "draw eligibility %d (valid %d, kill %d)", ok, valid, kill);
        if (!ok) continue;
        const uint32_t page = idx / (uint32_t)per, off = idx % (uint32_t)per;
        const uint64_t rec = GA + A_RECPG + 0x800 * page + 0x58 * off;
        CHECK(d.rec == rec && d.cb == GA + A_CTX1 + 0x38, "record / command buffer");
        uint64_t sr; memcpy(&sr, R + NRNC_R_SHADER_REC, 8); CHECK(d.ubo == (sr != 0), "uniform flag");
        const int swap = (int)(rnd() & 1);
        if (swap) w64(g_arena + A_DEV + NRNC_DEV_CTX, GA + A_CTX2);   // the context changes after the entry
        const int need = R[NRNC_R_REN_NEED], offb = R[NRNC_R_REN_OFF], ren = need && offb;
        uint32_t itype, prim; uint64_t base, ibf; memcpy(&itype, NRNC_PTR(rec) + 0x24, 4); memcpy(&base, NRNC_PTR(rec) + 8, 8); memcpy(&prim, R + NRNC_R_PRIM, 4); memcpy(&ibf, R + NRNC_R_IB_FIRST, 8);
        const int64_t size = (int32_t)(k_isz[itype * 4] | 0);
        const uint64_t ib = op == 36 ? (uint64_t)((int64_t)a1 * size) + base : ibf * (uint64_t)size + base;
        nrnc_expect e; nrnc_draw_expect(R, &d, k_isz, &e);
        memcpy(save, g_arena, ARENA);
        g_ncall = 0;
        nrnc_draw_tail(R, &d, k_isz, &k_rec, NULL);
        CHECK(g_ncall == 1 + ren, "tail made %d calls, expected %d", g_ncall, 1 + ren);
        int k = 0;
        if (ren) { CHECK(g_calls[0].v[0] == K_REN && g_calls[0].v[1] == GA + (swap ? A_CTX2 : A_CTX1) + 0x38, "SetRenderEnable on the context of now"); k = 1; }
        CHECK(g_calls[k].v[0] == K_DRAW && g_calls[k].v[1] == GA + A_CTX1 + 0x38 && (uint32_t)g_calls[k].v[2] == prim && g_calls[k].v[3] == itype && (int32_t)g_calls[k].v[4] == count &&
              g_calls[k].v[5] == ib && (int32_t)g_calls[k].v[6] == inst, "the draw call (op %d)", op);
        CHECK(e.ren == ren && (!ren || e.ren_cb == g_calls[0].v[1]) && e.cb == g_calls[k].v[1] && (uint32_t)e.prim == prim && e.itype == itype && e.count == count && e.ib == ib && e.instances == inst, "nrnc_draw_expect = the tail");
        if (ren) { save[NRNC_R_REN_NEED] = 0; save[NRNC_R_REN_OFF] = 0; memset(save + NRNC_R_REN_Q, 0, 8); }
        save[NRNC_R_REN_OFF] = save[NRNC_R_REN_NEED];
        CHECK(!memcmp(save, g_arena, ARENA), "the tail wrote something besides the render-enable fields");
    }
    // 4. run_case = the order of the front end (uniform functions, flush, tail)
    for (int it = 0; it < 2000; it++) {
        random_state(&per, &idx, 1);
        w64(R + NRNC_R_SHADER_REC, 1);
        const int ran = run_case(it & 1 ? 37 : 36, 3, 5, 7, 1, k_isz);
        CHECK(ran && g_ncall >= 3 && g_calls[0].v[0] == K_UBO1 && g_calls[1].v[0] == K_UBO2 && g_calls[g_ncall - 1].v[0] == K_DRAW, "order of a draw");
        for (int i = 2; i < g_ncall - 1; i++) CHECK(g_calls[i].v[0] == K_BIND ? g_calls[i].v[1] == GA + A_CTX2 + 0x38 : g_calls[i].v[0] == K_REN && i == g_ncall - 2, "binds on the context of the flush, SetRenderEnable last before the draw");
        CHECK(g_calls[g_ncall - 1].v[1] == GA + A_CTX1 + 0x38, "the draw on the context of the entry");
    }
    // 5. hash and cost model
    CHECK(nrnc_fnv((const uint8_t *)"", 0) == 0xcbf29ce484222325ull && nrnc_fnv((const uint8_t *)"a", 1) == 0xaf63dc4c8601ec8cull, "FNV-1a 64");
    static const nrnc_code code[NRNC_CODE_N] = NRNC_CODE_TABLE;
    CHECK(code[NRNC_CODE_FLUSH].off == NRNC_FN_FLUSH && code[NRNC_CODE_DRAW].off == NRNC_FN_DRAW && code[NRNC_CODE_DRAWI].off == NRNC_FN_DRAWI, "code table");
    CHECK(NRNC_LR_DRAW > NRNC_FN_DRAW && NRNC_LR_DRAW < code[NRNC_CODE_DRAW].end && NRNC_LR_DRAWI > NRNC_FN_DRAWI && NRNC_LR_DRAWI < code[NRNC_CODE_DRAWI].end, "flush return addresses inside the draw functions");
    CHECK(nrnc_saving_ns(3000, 1000, 25, 2, 12) == 3000. * 23 - 12000 && nrnc_saving_ns(1000, 1000, 10, 2, 12) < 0, "cost model");
    if (g_fail) { printf("FAIL: nr_nocall_test: %llu of %llu checks failed\n", g_fail, g_checks); return 1; }
    printf("PASS: nr_nocall_test (%llu checks): flush plan / run against the per-slot rule (20000 states, eligibility, read-only planning, only the six ranges written), "
           "draw entry / tail (20000 states: record lookup, both index address forms, SetRenderEnable on the context of now, the draw on the context of the entry, "
           "only the render-enable fields written), order of a native draw, code table, cost model\n", g_checks);
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--oracle")) return oracle(argv[2], argv[3]);
    return self_test();
}
