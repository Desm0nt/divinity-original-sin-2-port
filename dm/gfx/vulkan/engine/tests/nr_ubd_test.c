// NR-UBD (DM_NR_UBD): host test of dm/gfx/nr/nr_ubd.h - the mirror of the backend's uniform-buffer table that lets a BindUniformBuffer of
// a served direct draw stay unsent when its (stage, slot) already holds that range for the recording.
//   1. Random programs: 3 command buffers, 2 threads, BeginRecording / EndRecording, single and plural binds inside and outside draw
//      brackets and served direct draws (scope), NB-10 check brackets, draws, nested handles (CallCommands / CopyCommands of finished
//      handles). The front end runs the NVN hook's decision (nvn_nr_direct.inc nrub_bind_hook) on every call. A model of the backend
//      (state.ub[6][16], binds in record order, nested handles executed inline, the state kept across handles) then executes random
//      schedules of the finished handles - any order, replays, random start state - twice: every record, and without the binds =1 left
//      out. At every draw the two tables must be equal, and every left-out bind must find its range in the table as it would land (the
//      check DM_NR_UBD=2 makes on the device, xbg_nr_direct.inc).
//   2. Mutations of the mirror (no reset at BeginRecording, no forget at nested handles / another thread, size not compared, plural binds
//      not learned, adopting another command buffer without a reset): each must be caught by (1).
//   3. NR-09U's count (nr_ubo.h nru_same_count): dirty-in-the-same-frame rebuilds, shadow equal to the previous ring bytes.
//   4. PC timing: ns a bind of the hook's fast path (the owner's binds into the tracked recording).
// Build: clang -std=c11 -Wall -Wextra -O2 nr_ubd_test.c -o nr_ubd_test
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#define NRU_YIELD_THREAD() SwitchToThread()
#endif
#include "../../../nr/nr_ubd.h"
#include "../../../nr/nr_ubo.h"

static unsigned long long g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { if (g_fail++ < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }

// ---- the mirror under test, with optional mutations ----
enum { MUT_NONE, MUT_NO_BEGIN, MUT_NO_NESTED, MUT_NO_FOREIGN, MUT_NO_SIZE, MUT_NO_PLURAL, MUT_ADOPT_NORESET, MUT_N };
static const char *const k_mut[MUT_N] = {"none", "no reset at BeginRecording", "no forget at a nested handle", "no forget at another thread's bind",
                                         "size not compared", "plural binds not learned", "another command buffer adopted without a reset"};
static int g_mut;
static int mbind(nrub_mirror *m, const void *cb, const void *me, int adopt, int st, int first, int n, const uint64_t *a, const uint64_t *sz) {
    if (g_mut == MUT_NONE) return nrub_bind(m, cb, me, adopt, st, first, n, a, sz, NULL);
    // the same function, written out, with one mutation
    if (adopt) { if (m->cb != cb || m->owner != me) { if (g_mut != MUT_ADOPT_NORESET) nrub_reset(m); m->cb = cb; m->owner = me; } }
    else if (cb != m->cb) return 0;
    else if (m->owner != me) { if (g_mut == MUT_NO_FOREIGN) return 0; nrub_forget(m); return 0; }
    int same = 0;
    for (int i = 0; i < n; i++) {
        const int s = first + i;
        if (!nrub_ok(st, s)) continue;
        if (n == 1 && m->at[st][s] == m->epoch && m->a[st][s] == a[0] && (g_mut == MUT_NO_SIZE || m->n[st][s] == sz[0])) same = 1;
        m->at[st][s] = m->epoch; m->a[st][s] = a[i]; m->n[st][s] = sz[i];
    }
    return same;
}
// = nrub_bind_hook (nvn_nr_direct.inc) without the lock and the counters. Returns 0 send, 2 do not send (=1); =2 flags exactly these.
static int hook(nrub_mirror *m, const void *cb, const void *me, int in, int chk, int scope, int st, int first, int n, const uint64_t *a, const uint64_t *sz, int filt) {
    const int adopt = in && !chk;
    if (nrub_slow(m, cb, me, adopt)) { (void)mbind(m, cb, me, adopt, st, first, n, a, sz); return 0; }
    if (m->cb != cb) return 0;
    if (!filt && g_mut == MUT_NO_PLURAL) return 0;
    const int same = mbind(m, cb, me, adopt, st, first, n, a, sz);
    if (!filt || !scope) return 0;
    return nrub_filter(same, adopt, scope, n) ? 2 : 0;
}

// ---- the recorded stream ----
enum { R_BIND, R_DRAW, R_CALL };
typedef struct { uint8_t kind, st, slot, skip; uint32_t h; uint64_t a, n; } rec;   // skip: a bind =1 leaves out; h: the handle of R_CALL
typedef struct { rec *r; int n, cap; } reclist;
enum { N_CB = 3, N_TH = 2, MAXH = 64 };
static reclist g_h[MAXH]; static int g_nh;         // finished handles
static reclist g_cur[N_CB]; static int g_rec[N_CB];   // recording now
static void put(reclist *l, rec r) { if (l->n == l->cap) { l->cap = l->cap ? 2 * l->cap : 64; l->r = (rec *)realloc(l->r, sizeof(rec) * (size_t)l->cap); } l->r[l->n++] = r; }
static uint64_t g_ranges[8][2];
static uint64_t g_skipped, g_binds, g_scope_binds;

static void gen_program(nrub_mirror *m, int steps) {
    for (int c = 0; c < N_CB; c++) { g_cur[c].n = 0; g_rec[c] = 0; }
    for (int i = 0; i < g_nh; i++) g_h[i].n = 0;
    g_nh = 0;
    static int me_tok[N_TH];
    // the "served direct draw" state per thread: bracket open on cb, scope, check bracket
    int in[N_TH] = {0}, scope[N_TH] = {0}, chk[N_TH] = {0}, bcb[N_TH] = {0};
    for (int k = 0; k < steps; k++) {
        const int th = rn(10) < 8 ? 0 : 1, c = (int)rn(N_CB);   // thread 0 does most of the work
        const void *cb = &g_cur[c], *me = &me_tok[th];
        const unsigned op = rn(100);
        if (!g_rec[c]) {   // begin
            g_rec[c] = 1; g_cur[c].n = 0;
            if (g_mut != MUT_NO_BEGIN) nrub_begin(m, cb);
            continue;
        }
        if (op < 3) {   // end -> a finished handle
            if (g_nh < MAXH) { reclist *h = &g_h[g_nh++]; h->n = 0; for (int i = 0; i < g_cur[c].n; i++) put(h, g_cur[c].r[i]); }
            g_rec[c] = 0; in[th] = scope[th] = 0;
            continue;
        }
        if (op < 10) {   // open / close a bracket of an engine draw on cb (scope: a served direct draw; chk: NB-10's check bracket)
            if (in[th]) { in[th] = scope[th] = chk[th] = 0; }
            else { in[th] = 1; bcb[th] = c; scope[th] = rn(4) != 0; chk[th] = rn(16) == 0; }
            continue;
        }
        if (op < 13 && g_nh) {   // a nested handle
            const uint32_t h = rn((unsigned)g_nh);
            if (g_mut != MUT_NO_NESTED) nrub_nested(m, cb);
            put(&g_cur[c], (rec){R_CALL, 0, 0, 0, h, 0, 0});
            continue;
        }
        if (op < 22) { put(&g_cur[c], (rec){R_DRAW, 0, 0, 0, 0, 0, 0}); continue; }
        const int inb = in[th] && bcb[th] == c;   // a bracket is the thread's, on its command buffer
        const int st = rn(8) == 0 ? (int)rn(6) : (int)(rn(2) ? 0 : 1), slot = rn(16) == 0 ? (int)(16 + rn(3)) : (int)rn(4);
        if (op < 30) {   // plural binds: learned, never left out
            const int n = 1 + (int)rn(3);
            uint64_t a[3], sz[3];
            for (int j = 0; j < n; j++) { const unsigned q = rn(8); a[j] = g_ranges[q][0]; sz[j] = g_ranges[q][1]; }
            (void)hook(m, cb, me, inb, chk[th], scope[th] && inb, st, slot, n, a, sz, 0);
            for (int j = 0; j < n; j++) put(&g_cur[c], (rec){R_BIND, (uint8_t)st, (uint8_t)(slot + j), 0, 0, a[j], sz[j]});
            g_binds += (uint64_t)n;
            continue;
        }
        const unsigned q = rn(8);
        uint64_t a = g_ranges[q][0], sz = g_ranges[q][1];
        const int d = hook(m, cb, me, inb, chk[th], scope[th] && inb, st, slot, 1, &a, &sz, 1);
        put(&g_cur[c], (rec){R_BIND, (uint8_t)st, (uint8_t)slot, (uint8_t)(d == 2), 0, a, sz});
        g_binds++; if (scope[th] && inb) g_scope_binds++; if (d == 2) g_skipped++;
    }
}

// ---- the backend model ----
typedef struct { uint64_t a, n; } range;
typedef struct { range ub[6][16]; } bstate;
static uint64_t g_draws_cmp, g_checked;
static int g_bad_draw, g_bad_check;
static uint64_t hash_state(const bstate *s) { uint64_t h = 1469598103934665603ull; const uint8_t *p = (const uint8_t *)s; for (size_t i = 0; i < sizeof *s; i++) h = (h ^ p[i]) * 1099511628211ull; return h; }
// filtered = 0: every record (and the =2 check of the left-out ones); 1: without the left-out binds. out: the table hash at every draw.
static void exec(bstate *s, const reclist *l, int filtered, uint64_t *out, int *no, int depth) {
    if (depth > 4) return;   // a handle may call itself through a later one: bounded (both runs cut at the same place)
    for (int i = 0; i < l->n; i++) {
        const rec *r = &l->r[i];
        if (r->kind == R_BIND) {
            const int ok = r->st < 6 && r->slot < 16;
            if (r->skip && !filtered) { g_checked++; if (!ok || s->ub[r->st][r->slot].a != r->a || s->ub[r->st][r->slot].n != r->n) g_bad_check++; }
            if (r->skip && filtered) continue;
            if (ok) { s->ub[r->st][r->slot].a = r->a; s->ub[r->st][r->slot].n = r->n; }
        } else if (r->kind == R_DRAW) { if (*no < 4096) out[(*no)++] = hash_state(s); }
        else if (r->h < (uint32_t)g_nh) exec(s, &g_h[r->h], filtered, out, no, depth + 1);
    }
}
static int run_world(int programs, int steps, int schedules) {
    static uint64_t o1[4096], o2[4096];
    g_bad_draw = g_bad_check = 0;
    nrub_mirror *m = (nrub_mirror *)calloc(1, sizeof *m);
    for (int w = 0; w < programs; w++) {
        for (int q = 0; q < 8; q++) { g_ranges[q][0] = 0x40000000ull + 0x100ull * rn(6); g_ranges[q][1] = 0x40u * (1 + rn(3)); }   // collisions on purpose
        nrub_forget(m);
        gen_program(m, steps);
        for (int c = 0; c < N_CB; c++) if (g_rec[c] && g_nh < MAXH) { reclist *h = &g_h[g_nh++]; h->n = 0; for (int i = 0; i < g_cur[c].n; i++) put(h, g_cur[c].r[i]); }
        for (int k = 0; k < schedules && g_nh; k++) {
            bstate s1, s2;
            for (int st = 0; st < 6; st++) for (int sl = 0; sl < 16; sl++) { s1.ub[st][sl].a = rnd(); s1.ub[st][sl].n = rn(2) ? rnd() : g_ranges[rn(8)][1]; }
            s2 = s1;
            int n1 = 0, n2 = 0;
            const int len = 1 + (int)rn(6);
            for (int j = 0; j < len; j++) {   // any order, replays, other handles in between; the state persists across handles
                const int h = (int)rn((unsigned)g_nh);
                if (rn(8) == 0) { const int st = (int)rn(6), sl = (int)rn(16); s1.ub[st][sl].a = s2.ub[st][sl].a = rnd(); }   // another handle's bind between two
                exec(&s1, &g_h[h], 0, o1, &n1, 0); exec(&s2, &g_h[h], 1, o2, &n2, 0);
            }
            if (n1 != n2 || memcmp(o1, o2, sizeof o1[0] * (size_t)n1)) g_bad_draw++;
            g_draws_cmp += (uint64_t)n1;
        }
    }
    free(m);
    return g_bad_draw + g_bad_check;
}

// ---- 3. NR-09U's count ----
static void test_same_count(void) {
    uint8_t *D = (uint8_t *)calloc(1, NRU_D_HI + 16), *ring = (uint8_t *)calloc(1, NRU_RING), B[NRU_BLOCK], shadow[256];
    nru_run *s = (nru_run *)calloc(1, sizeof *s);
    nru_w64(D + NRU_D_CPU, (uint64_t)(uintptr_t)ring); nru_w64(D + NRU_D_FRAME, 77);
    for (int i = 0; i < 256; i++) shadow[i] = (uint8_t)(i * 7 + 1);
    memcpy(ring + 0x1000, shadow, 192);
    const struct { int dirty; uint64_t stamp; uint64_t prev; uint32_t n; int flip; int want_dirty, want_same; } c[] = {
        {1, 77, 0x1000, 192, 0, 1, 1},          // dirty again in the same frame, the same bytes
        {1, 77, 0x1000, 192, 1, 1, 0},          // a byte differs
        {1, 76, 0x1000, 192, 0, 0, 0},          // first rebuild of the frame (stale stamp): never reused
        {0, 75, 0x1000, 192, 0, 0, 0},          // clean, stale
        {1, 77, NRU_RING - 64, 192, 0, 1, 0},   // the previous range would run past the ring
        {1, 77, ~0ull, 192, 0, 1, 0},           // no previous range in the ring (prev outside)
        {1, 77, 0x1000, 0, 0, 1, 0},            // size 0
    };
    for (unsigned k = 0; k < sizeof c / sizeof c[0]; k++) {
        memset(B, 0, sizeof B); B[NRU_B_DIRTY] = (uint8_t)c[k].dirty; nru_w64(B + NRU_B_STAMP, c[k].stamp);
        nru_w64(B + NRU_B_CPU, c[k].prev == ~0ull ? 8 : (uint64_t)(uintptr_t)ring + c[k].prev); nru_w32(B + NRU_B_SIZE, c[k].n);
        shadow[5] ^= (uint8_t)(c[k].flip ? 0x10 : 0);
        nru_w64(B + NRU_B_SRC, (uint64_t)(uintptr_t)shadow);
        s->reb_dirty = s->reb_same = 0;
        nru_same_count(s, B, D);
        shadow[5] ^= (uint8_t)(c[k].flip ? 0x10 : 0);
        CHECK((int)s->reb_dirty == c[k].want_dirty && (int)s->reb_same == c[k].want_same, "same count case %u: dirty %u same %u", k, s->reb_dirty, s->reb_same);
    }
    free(D); free(ring); free(s);
}

static double now_s(void) { struct timespec t; timespec_get(&t, TIME_UTC); return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec; }
int main(void) {
    // 1. the mirror against the backend model
    int bad = run_world(3000, 400, 12);
    CHECK(bad == 0, "mirror: %d draws differ, %d left-out binds the table did not hold", g_bad_draw, g_bad_check);
    printf("mirror vs backend model: %llu binds (%llu in served direct draws), %llu left out (%.1f%% of the served ones), %llu draws compared over random "
           "schedules (any order, replays, nested handles, random start state), %llu left-out binds checked as they land: bad %d / %d\n",
           (unsigned long long)g_binds, (unsigned long long)g_scope_binds, (unsigned long long)g_skipped, g_scope_binds ? 100. * (double)g_skipped / (double)g_scope_binds : 0.,
           (unsigned long long)g_draws_cmp, (unsigned long long)g_checked, g_bad_draw, g_bad_check);
    CHECK(g_skipped > 1000 && g_checked > 1000, "the test must leave binds out: %llu / %llu", (unsigned long long)g_skipped, (unsigned long long)g_checked);
    // 2. mutations
    int caught = 0;
    for (g_mut = 1; g_mut < MUT_N; g_mut++) {
        g_skipped = g_checked = 0;
        const int b = run_world(3000, 400, 12);
        printf("mutation '%s': %s (draws differ %d, left-out binds not held %d, left out %llu)\n", k_mut[g_mut], b ? "caught" : "NOT caught", g_bad_draw, g_bad_check,
               (unsigned long long)g_skipped);
        CHECK(b > 0, "mutation '%s' not caught", k_mut[g_mut]);
        caught += b > 0;
    }
    g_mut = MUT_NONE;
    // 3.
    test_same_count();
    // 4. timing: the owner's binds into its tracked recording, 4 a draw (2 repeated ranges, 2 new)
    {
        nrub_mirror *m = (nrub_mirror *)calloc(1, sizeof *m);
        static int me; const void *cb = &me + 1;
        uint64_t a = 0, z = 0x100;
        (void)nrub_bind(m, cb, &me, 1, 0, 0, 1, &a, &z, NULL);
        volatile uint64_t sink = 0;
        double best = 1e9;
        for (int rep = 0; rep < 5; rep++) {
            const int N = 4000000;
            const double t0 = now_s();
            for (int i = 0; i < N; i++) {
                uint64_t a0 = 0x40000000ull + 0x100ull * (uint64_t)(i & 7), a1 = 0x40001000ull, a2 = 0x40002000ull + 0x100ull * (uint64_t)i, n = 0x100;
                sink += (uint64_t)hook(m, cb, &me, 1, 0, 1, 0, 0, 1, &a1, &n, 1);
                sink += (uint64_t)hook(m, cb, &me, 1, 0, 1, 1, 0, 1, &a1, &n, 1);
                sink += (uint64_t)hook(m, cb, &me, 1, 0, 1, 0, 1, 1, &a2, &n, 1);
                sink += (uint64_t)hook(m, cb, &me, 1, 0, 1, 1, 1, 1, &a0, &n, 1);
            }
            const double ns = (now_s() - t0) * 1e9 / (4.0 * N);
            if (ns < best) best = ns;
        }
        printf("PC: the mirror's decision %.1f ns a bind (best of 5, 4 binds a draw, half repeated; sink %llu)\n", best, (unsigned long long)sink);
        free(m);
    }
    printf("%s: %llu checks, %llu failed; mutations caught %d of %d\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail, caught, MUT_N - 1);
    return g_fail ? 1 : 0;
}
