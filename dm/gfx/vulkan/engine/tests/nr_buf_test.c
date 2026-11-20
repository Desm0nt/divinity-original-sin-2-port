// [DOS2] NR-06 host test: dm/gfx/nr/nr_buf.h (DM_NR_BUF) - the constant ring mirror bookkeeping against a model of the engine's ring and
// of the GPU (every bind the mirror served must read, when its slot retires, exactly the bytes the guest had at the bind), and the static
// buffer table against a one-by-one reference; plus a rough micro benchmark of the per-bind decisions.
// Build (Windows LLVM or any C11 compiler):
//   clang -std=c11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS dm/gfx/vulkan/engine/tests/nr_buf_test.c dm/gfx/nr/nr_buf.c -o nr_buf_test && ./nr_buf_test
#include "../../../nr/nr_buf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails = 0;
static long checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { if (++fails < 40) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static double now_ns(void) { struct timespec t; timespec_get(&t, TIME_UTC); return t.tv_sec * 1e9 + t.tv_nsec; }

// ---------------- ring: units ----------------
static void ring_units(void) {
    nr_ring r; nr_ring_work w;
    CHECK(nr_ring_init(&r, 0, 4096) == -1);
    CHECK(nr_ring_init(&r, 0x10000, 4096) == 0 && r.ngran == 16);
    CHECK(nr_ring_check(&r, 0x0ffff, 16, 1, 1, &w) == NRR_NOT);
    CHECK(nr_ring_check(&r, 0x11000, 16, 1, 1, &w) == NRR_NOT);
    CHECK(nr_ring_check(&r, 0x10ff0, 32, 1, 1, &w) == NRR_SPAN);
    // never used: one COPY run over the granules of the range
    CHECK(nr_ring_check(&r, 0x10100, 0x180, 1, 1, &w) == NRR_WORK && w.n == 1 && w.run[0].kind == NRR_COPY && w.run[0].g0 == 1 && w.run[0].g1 == 3);
    nr_ring_done(&r, 1, 3, 1, 1);
    CHECK(nr_ring_check(&r, 0x10100, 0x180, 1, 1, &w) == NRR_VALID && w.n == 0);
    // same serial, next epoch (a guest submission recorded into the same slot): readers pending -> COMPARE
    CHECK(nr_ring_check(&r, 0x10100, 0x100, 2, 1, &w) == NRR_WORK && w.n == 1 && w.run[0].kind == NRR_COMPARE);
    // serial 2: the slot of serial 1 may still fly -> COMPARE; serial 3: it retired -> COPY
    CHECK(nr_ring_check(&r, 0x10100, 0x100, 3, 2, &w) == NRR_WORK && w.run[0].kind == NRR_COMPARE);
    CHECK(nr_ring_check(&r, 0x10100, 0x100, 4, 3, &w) == NRR_WORK && w.run[0].kind == NRR_COPY);
    // mixed: granule 2 valid in epoch 5, granules 1 and 3 not -> two COPY runs around it
    nr_ring_done(&r, 2, 3, 5, 3);
    CHECK(nr_ring_check(&r, 0x10100, 0x300, 5, 5, &w) == NRR_WORK && w.n == 2 && w.run[0].g0 == 1 && w.run[0].g1 == 2 && w.run[1].g0 == 3 && w.run[1].g1 == 4);
    // race -> held until used + 2
    nr_ring_done(&r, 4, 5, 6, 6);
    nr_ring_race(&r, 4, 5, 7);
    CHECK(nr_ring_check(&r, 0x10400, 16, 7, 7, &w) == NRR_HOLD);
    CHECK(nr_ring_check(&r, 0x10400, 16, 8, 8, &w) == NRR_WORK && w.run[0].kind == NRR_COPY);
    // CPU write: a granule read by the serial being recorded is held two serials; one nobody reads only re-copies
    nr_ring_done(&r, 6, 8, 9, 9);
    CHECK(nr_ring_cpu_write(&r, 0x10600, 0x10, 9) == 1 && r.holds_cpu == 1);
    CHECK(nr_ring_check(&r, 0x10600, 16, 9, 9, &w) == NRR_HOLD && nr_ring_check(&r, 0x10600, 16, 10, 10, &w) == NRR_HOLD);
    CHECK(nr_ring_check(&r, 0x10600, 16, 11, 11, &w) == NRR_WORK && w.run[0].kind == NRR_COPY);
    CHECK(nr_ring_check(&r, 0x10700, 16, 9, 9, &w) == NRR_VALID);
    CHECK(nr_ring_cpu_write(&r, 0x10700, 0x10, 20) == 0 && nr_ring_check(&r, 0x10700, 16, 20, 20, &w) == NRR_WORK && w.run[0].kind == NRR_COPY);
    CHECK(nr_ring_cpu_write(&r, 0x0f000, 0x1000, 20) == 0 && nr_ring_cpu_write(&r, 0x11000, 0x10, 20) == 0);   // outside
    // runs: alternate valid / not granules - 8 runs fit, 9 do not
    for (uint32_t g = 0; g < 16; g += 2) nr_ring_done(&r, g, g + 1, 30, 30);
    for (uint32_t g = 1; g < 16; g += 2) { r.epoch[g] = 0; r.used[g] = 0; r.hold[g] = 0; }
    CHECK(nr_ring_check(&r, 0x10000, 4096, 30, 30, &w) == NRR_WORK && w.n == 8);
    CHECK(nr_ring_check(&r, 0x10000, 0x800, 30, 30, &w) == NRR_WORK && w.n == 4);
    nr_ring_free(&r); CHECK(nr_ring_init(&r, 0x10000, 8192) == 0);
    for (uint32_t g = 0; g < 32; g += 2) nr_ring_done(&r, g, g + 1, 30, 30);
    CHECK(nr_ring_check(&r, 0x10000, 8192, 30, 30, &w) == NRR_RUNS_OVER && w.n == 0);
    CHECK(!strcmp(nr_ring_result_name(NRR_HOLD), "hold") && !strcmp(nr_ring_result_name(99), "?"));
    nr_ring_free(&r);
}

// ---------------- ring: the engine model ----------------
// A ring of S bytes; the engine allocates 256-aligned blocks, writes them once, binds them (and rebinds recent ones: unchanged global
// blocks), wraps to 0. Submissions = slot serials; a serial has 1..3 epochs. Two slots: serial s retires when serial s + 2 begins.
// Optionally a CPU write in the stream rewrites a bound block. Small rings make the engine wrap within the in-flight window - the case
// the engine itself never allows - so COMPARE / race / hold are exercised: whatever happens, a bind served by the mirror must read, when
// its slot retires, the bytes the guest had when it was bound.
typedef struct { uint64_t off, n; uint64_t hash; } served_t;
enum { MAXSERVE = 1 << 14 };
static served_t g_srv[3][MAXSERVE]; static int g_nsrv[3];
static uint64_t hash_bytes(const uint8_t *p, size_t n) { return nr_fnv64(p, n); }
static void ring_model(uint64_t S, int serials, int binds_per_serial, int cpu_writes, uint64_t seed, uint64_t *out_served, uint64_t *out_old, uint64_t *out_races) {
    rng = seed;
    const uint64_t base = 0x7000000000ull;
    uint8_t *guest = (uint8_t *)calloc(S, 1), *mirror = (uint8_t *)calloc(S + 65536, 1);
    nr_ring r; CHECK(nr_ring_init(&r, base, S) == 0);
    uint64_t head = 0, served = 0, old = 0, races0 = r.holds_race;
    uint64_t recent[16][2]; int nrecent = 0;
    uint32_t epoch = 1; uint64_t epoch_bytes = 0;
    static uint64_t bound[4096][2]; int nbound = 0;   // ranges bound in the current epoch
    memset(g_nsrv, 0, sizeof g_nsrv);
    for (uint32_t serial = 1; serial <= (uint32_t)serials; serial++) {
        // serial - 2 retires now (recording_slot of this serial): its GPU reads happen at the latest here
        if (serial > 2) {
            const int k = (serial - 2) % 3;
            for (int i = 0; i < g_nsrv[k]; i++) CHECK(hash_bytes(mirror + g_srv[k][i].off, g_srv[k][i].n) == g_srv[k][i].hash);
        }
        g_nsrv[serial % 3] = 0;
        const int epochs = 1 + (int)(rnd() % 3);
        for (int ep = 0; ep < epochs; ep++, epoch++) {
            epoch_bytes = 0; nbound = 0;
            for (int b = 0; b < binds_per_serial / epochs; b++) {
                uint64_t off, n;
                // the engine never rewrites, inside one submission, a range it bound there (the GPU has not read it yet - on the Switch
                // either): an epoch allocates at most the ring minus a margin; past that it only rebinds
                const int may_alloc = epoch_bytes + 2048 + 512 < S - 4096;
                if (nrecent && (rnd() % 3 == 0 || !may_alloc)) { const int i = (int)(rnd() % (uint64_t)nrecent); off = recent[i][0]; n = recent[i][1]; }   // a rebind
                else {   // a new block: head aligned to 256, wrap when it does not fit
                    n = 16 + rnd() % 2048;
                    uint64_t h = (head + 255) & ~255ull;
                    if (h + n > S) h = 0;
                    int clash = 0;   // ... and never over a range this submission bound (a rebind of an older block included)
                    for (int i = 0; i < nbound && !clash; i++) clash = bound[i][0] < h + n + 16 && h < bound[i][0] + bound[i][1];
                    if (clash) continue;
                    head = h; off = head; head += n; epoch_bytes += n + 256;
                    for (uint64_t i = 0; i < n; i++) guest[off + i] = (uint8_t)rnd();
                    // the engine forgets older ranges it overwrote: drop recents that overlap
                    for (int i = 0; i < nrecent; i++) if (recent[i][0] < off + n && off < recent[i][0] + recent[i][1]) { recent[i][0] = recent[nrecent - 1][0]; recent[i][1] = recent[nrecent - 1][1]; nrecent--; i--; }
                    if (nrecent < 16) { recent[nrecent][0] = off; recent[nrecent][1] = n; nrecent++; } else { const int i = (int)(rnd() % 16); recent[i][0] = off; recent[i][1] = n; }
                }
                if (cpu_writes && rnd() % 97 == 0) {   // an in-stream CPU write over a bound block: invalidate first, then the write lands
                    nr_ring_cpu_write(&r, base + off, n, serial);
                    for (uint64_t i = 0; i < n; i += 7) guest[off + i] ^= 0x5a;
                }
                const uint64_t nn = (n + 15) & ~15ull;   // uniform_range
                if (nbound < 4096) { bound[nbound][0] = off; bound[nbound][1] = nn; nbound++; }
                if (off + nn > S) { old++; continue; }
                nr_ring_work w; const int res = nr_ring_check(&r, base + off, nn, epoch, serial, &w);
                int ok = res == NRR_VALID || res == NRR_WORK;
                for (int i = 0; ok && i < w.n; i++) {
                    const uint64_t lo = nr_ring_goff(w.run[i].g0), hi = nr_ring_goff(w.run[i].g1) < S ? nr_ring_goff(w.run[i].g1) : S;
                    if (w.run[i].kind == NRR_COPY) { memcpy(mirror + lo, guest + lo, hi - lo); nr_ring_done(&r, w.run[i].g0, w.run[i].g1, epoch, serial); }
                    else if (!memcmp(mirror + lo, guest + lo, hi - lo)) nr_ring_done(&r, w.run[i].g0, w.run[i].g1, epoch, serial);
                    else { nr_ring_race(&r, w.run[i].g0, w.run[i].g1, serial); ok = 0; }
                }
                if (!ok) { old++; continue; }
                CHECK(!memcmp(mirror + off, guest + off, nn));   // what the draw binds now
#ifdef NRB_DEBUG
                if (memcmp(mirror + off, guest + off, nn)) { static int said; if (said++ < 4) { printf("S=%llu serial %u epoch %u off %#llx n %llu res %d runs %d head %#llx\n", (unsigned long long)S, serial, epoch, (unsigned long long)off, (unsigned long long)nn, res, w.n, (unsigned long long)head); for (uint64_t g = off >> 8; g <= (off + nn - 1) >> 8; g++) printf("  g %llu epoch %u used %u hold %u diff %d\n", (unsigned long long)g, r.epoch[g], r.used[g], r.hold[g], memcmp(mirror + (g << 8), guest + (g << 8), 256) != 0); } }
#endif
                const int k = serial % 3;
                if (g_nsrv[k] < MAXSERVE) { g_srv[k][g_nsrv[k]].off = off; g_srv[k][g_nsrv[k]].n = nn; g_srv[k][g_nsrv[k]].hash = hash_bytes(guest + off, nn); g_nsrv[k]++; }
                served++;
            }
        }
    }
    *out_served = served; *out_old = old; *out_races = r.holds_race - races0;
    nr_ring_free(&r); free(guest); free(mirror);
}

// ---------------- static table ----------------
typedef struct { uint64_t lo, hi, handle; int kind, live, queued, dead_q; uint64_t seq; } ref_t;
enum { REFMAX = 20000 };
static ref_t R[REFMAX]; static int nR;      // applied entries (live == 1)
static ref_t Q[REFMAX]; static int nQ;      // queued adds (dead_q: killed / dropped, its overlaps still count)
static int g_gone_calls;
static int *g_slot_live; static int g_nslots;
static void on_gone(void *ctx, const nr_sbuf *e) {
    (void)ctx; g_gone_calls++;
    if (e->slot >= 0) { CHECK(e->slot < g_nslots && g_slot_live[e->slot] == 1); if (e->slot < g_nslots) g_slot_live[e->slot] = 0; }
}
static void ref_apply(void) {
    for (int i = 0; i < nQ; i++) {
        for (int j = 0; j < nR; j++) if (R[j].live && R[j].lo < Q[i].hi && Q[i].lo < R[j].hi) R[j].live = 0;
        for (int j = 0; j < i; j++) if (!Q[j].dead_q && Q[j].lo < Q[i].hi && Q[i].lo < Q[j].hi) Q[j].dead_q = 1;
    }
    for (int i = 0; i < nQ; i++) if (!Q[i].dead_q && nR < REFMAX) { R[nR] = Q[i]; R[nR].live = 1; nR++; }
    int k = 0; for (int j = 0; j < nR; j++) if (R[j].live) R[k++] = R[j]; nR = k;
    nQ = 0;
}
static const ref_t *ref_find(uint64_t a, uint64_t n) {
    for (int j = 0; j < nR; j++) if (R[j].live && a >= R[j].lo && a + n <= R[j].hi) return &R[j];
    return NULL;
}
static void stab_units(void) {
    nr_stab t; CHECK(nr_stab_init(&t) == 0);
    CHECK(nr_stab_add(&t, NRS_KIND_VB, 1, 0, 100, NRS_ELIGIBLE, 0) == -1 && nr_stab_add(&t, 5, 1, 100, 100, 0, 0) == -1 && nr_stab_add(&t, 0, 1, 100, 0, 0, 0) == -1);
    CHECK(nr_stab_add(&t, NRS_KIND_VB, 0x100000001ull, 0x1000, 0x100, NRS_ELIGIBLE, 7) == 0);
    CHECK(nr_stab_find(&t, 0x1000, 4) == NULL);   // queued: not visible before the apply
    CHECK(nr_stab_apply(&t, on_gone, NULL) == 1);
    nr_sbuf *e = nr_stab_find(&t, 0x1010, 0x10);
    CHECK(e && e->lo == 0x1000 && e->hi == 0x1100 && e->hash == 7 && e->slot == -1 && e->cls == NRS_ELIGIBLE);
    CHECK(!nr_stab_find(&t, 0x10f0, 0x20) && !nr_stab_find(&t, 0xfff, 2) && nr_stab_find(&t, 0x10ff, 1) && !nr_stab_find(&t, 0x1100, 1));
    // memory reused: a new buffer over half of it kills it (at the apply)
    CHECK(nr_stab_add(&t, NRS_KIND_IB, 0x200000002ull, 0x1080, 0x100, NRS_DYNAMIC, 0) == 0);
    CHECK(nr_stab_find(&t, 0x1010, 1) == NULL && nr_stab_find(&t, 0x1090, 1) == NULL);   // the old range is dead at once, the new one not merged yet
    g_gone_calls = 0; CHECK(nr_stab_apply(&t, on_gone, NULL) == 1 && g_gone_calls == 1);
    CHECK(!nr_stab_find(&t, 0x1010, 1) && nr_stab_find(&t, 0x1090, 1) && nr_stab_find(&t, 0x1090, 1)->kind == NRS_KIND_IB);
    // kill by handle: at once; a wrong generation misses
    CHECK(nr_stab_kill(&t, NRS_KIND_IB, 0x300000002ull) == 0 && t.st.kill_miss == 1);
    CHECK(nr_stab_kill(&t, NRS_KIND_IB, 0x200000002ull) == 1 && !nr_stab_find(&t, 0x1090, 1));
    CHECK(nr_stab_apply(&t, on_gone, NULL) == 0 && t.n == 0);
    CHECK(nr_stab_apply(&t, on_gone, NULL) == 0);   // nothing to do
    // drop: everything in the pool, queued or not
    nr_stab_add(&t, 0, 0x10, 0x10000, 0x1000, 0, 0); nr_stab_apply(&t, on_gone, NULL);
    nr_stab_add(&t, 0, 0x11, 0x12000, 0x1000, 0, 0);
    CHECK(nr_stab_drop(&t, 0x10800, 0x2000) == 2);
    CHECK(nr_stab_apply(&t, on_gone, NULL) == 0);
    nr_stab_free(&t);
}
static int g_each_n;
static void each_cb(void *ctx, nr_sbuf *e) { (void)ctx; (void)e; g_each_n++; }
static void stab_each_unit(void) {
    nr_stab t; nr_stab_init(&t);
    nr_stab_add(&t, 0, 0x20, 0x20000, 0x100, 0, 0); nr_stab_add(&t, 0, 0x21, 0x20100, 0x100, 0, 0); nr_stab_add(&t, 1, 0x22, 0x20300, 0x100, 0, 0); nr_stab_apply(&t, NULL, NULL);
    g_each_n = 0; CHECK(nr_stab_each(&t, 0x200f0, 0x20, each_cb, NULL) == 2 && g_each_n == 2);
    g_each_n = 0; CHECK(nr_stab_each(&t, 0x20200, 0x100, each_cb, NULL) == 0 && g_each_n == 0);
    g_each_n = 0; CHECK(nr_stab_each(&t, 0x1ff00, 0x1000, each_cb, NULL) == 3);
    nr_stab_free(&t);
}
static void stab_model(int steps, uint64_t seed) {
    rng = seed;
    nr_stab t; CHECK(nr_stab_init(&t) == 0);
    nR = nQ = 0; g_nslots = 0; g_slot_live = (int *)calloc(1 << 20, sizeof *g_slot_live);
    uint64_t handles = 0;
    for (int s = 0; s < steps; s++) {
        const uint64_t op = rnd() % 100;
        if (op < 45) {   // add (sometimes over live memory: reuse)
            const uint64_t lo = 0x1000 + (rnd() % 4096) * 64, n = 16 + rnd() % 2048;
            const int kind = (int)(rnd() % 2);
            const uint64_t h = (uint64_t)(1 + rnd() % 7) << 32 | (handles++ % 70000);   // some indices past NRS_HANDLES
            if (nQ < REFMAX && nr_stab_add(&t, kind, h, lo, n, NRS_ELIGIBLE, 0) == 0) {
                Q[nQ].lo = lo; Q[nQ].hi = lo + n; Q[nQ].handle = h; Q[nQ].kind = kind; Q[nQ].dead_q = 0; nQ++;
                for (int j = 0; j < nR; j++) if (R[j].live && R[j].lo < lo + n && lo < R[j].hi) R[j].live = 0;   // reuse kills the table range at once
            }
        } else if (op < 70) {   // kill: a live or queued handle, or a random one
            uint64_t h; int kind;
            if (nQ && rnd() % 2) { const int i = (int)(rnd() % (uint64_t)nQ); h = Q[i].handle; kind = Q[i].kind; }
            else if (nR && rnd() % 4) { const int i = (int)(rnd() % (uint64_t)nR); h = R[i].handle; kind = R[i].kind; }
            else { h = rnd(); kind = (int)(rnd() % 2); }
            // reference: newest live queued match first, then the table
            int found = 0;
            for (int i = nQ - 1; i >= 0 && !found; i--) if (!Q[i].dead_q && Q[i].kind == kind && Q[i].handle == h) { Q[i].dead_q = 1; found = 1; }
            for (int i = 0; i < nR && !found; i++) if (R[i].live && R[i].kind == kind && R[i].handle == h) { R[i].live = 0; found = 1; }
            CHECK(nr_stab_kill(&t, kind, h) == found);
        } else if (op < 73) {   // drop a pool
            const uint64_t lo = 0x1000 + (rnd() % 4096) * 64, n = 1 + rnd() % 16384;
            for (int i = 0; i < nQ; i++) if (Q[i].lo < lo + n && lo < Q[i].hi) Q[i].dead_q = 1;
            for (int i = 0; i < nR; i++) if (R[i].live && R[i].lo < lo + n && lo < R[i].hi) R[i].live = 0;
            nr_stab_drop(&t, lo, n);
        } else if (op < 85) {   // apply (a submission): the engine gives new live entries a copy slot
            ref_apply(); nr_stab_apply(&t, on_gone, NULL);
            CHECK(t.n == (uint32_t)nR);
            for (uint32_t i = 0; i < t.n; i++) {
                if (i) CHECK(t.e[i - 1].hi <= t.e[i].lo);
                if (t.e[i].slot < 0 && rnd() % 2 && g_nslots < (1 << 20)) { t.e[i].slot = g_nslots; g_slot_live[g_nslots++] = 1; }
            }
        } else {   // lookups
            for (int q = 0; q < 8; q++) {
                const uint64_t a = 0x1000 + rnd() % (4096 * 64 + 4096), n = 1 + rnd() % 512;
                const ref_t *x = ref_find(a, n); const nr_sbuf *y = nr_stab_find(&t, a, n);
                CHECK((x == NULL) == (y == NULL));
                if (x && y) CHECK(x->lo == y->lo && x->hi == y->hi && x->handle == y->handle && x->kind == y->kind);
            }
        }
    }
    ref_apply(); nr_stab_apply(&t, on_gone, NULL);
    int live_slots = 0; for (int i = 0; i < g_nslots; i++) live_slots += g_slot_live[i];
    int table_slots = 0; for (uint32_t i = 0; i < t.n; i++) table_slots += t.e[i].slot >= 0;
    CHECK(live_slots == table_slots);   // every slot that left was reported once, the rest are in the table
    nr_stab_free(&t); free(g_slot_live);
}

// ---------------- micro benchmark ----------------
static void bench(void) {
    nr_ring r; nr_ring_init(&r, 0x7000000000ull, NR_RING_SIZE);
    uint8_t *mirror = (uint8_t *)calloc(NR_RING_SIZE, 1), *guest = (uint8_t *)calloc(NR_RING_SIZE, 1);
    const int N = 6000;   // uniform binds of one campaign submission (~5 850 on the phone)
    uint64_t offs[6000]; uint64_t head = 0;
    for (int i = 0; i < N; i++) { if (i % 6 == 0) { head = (head + 255) & ~255ull; offs[i] = head; head += 96 + rnd() % 400; } else offs[i] = offs[i - i % 6]; }   // 1000 blocks, each bound 6 times
    double t0 = now_ns(); uint64_t copied = 0;
    for (int it = 0; it < 50; it++) for (int i = 0; i < N; i++) {
        nr_ring_work w; const uint32_t ep = (uint32_t)(it * 2 + 1), se = (uint32_t)(it + 1);
        const uint64_t a = 0x7000000000ull + offs[i] + (uint64_t)it * 0x60000 % (NR_RING_SIZE - 0x70000);
        if (nr_ring_check(&r, a, 400, ep, se, &w) == NRR_WORK) for (int k = 0; k < w.n; k++) {
            const uint64_t lo = nr_ring_goff(w.run[k].g0), hi = nr_ring_goff(w.run[k].g1);
            memcpy(mirror + lo, guest + lo, hi - lo); copied += hi - lo; nr_ring_done(&r, w.run[k].g0, w.run[k].g1, ep, se);
        }
    }
    const double ns = (now_ns() - t0) / (50.0 * N);
    printf("bench: ring decision + copy %.1f ns per bind (x86; %.2f MB copied per submission of %d binds)\n", ns, copied / 50.0 / 1048576.0, N);
    nr_stab t; nr_stab_init(&t);
    for (int i = 0; i < 30000; i++) nr_stab_add(&t, i & 1, (uint64_t)1 << 32 | (uint64_t)i, 0x10000000ull + (uint64_t)i * 0x2000, 0x1800, 0, 0);
    t0 = now_ns(); nr_stab_apply(&t, NULL, NULL); const double ta = (now_ns() - t0) / 1e6;
    t0 = now_ns(); uint64_t hit = 0;
    for (int i = 0; i < 1000000; i++) hit += nr_stab_find(&t, 0x10000000ull + (rnd() % 30000) * 0x2000 + 64, 256) != NULL;
    printf("bench: static table of %u: apply %.2f ms (30 000 adds), lookup %.1f ns (%llu hits)\n", t.n, ta, (now_ns() - t0) / 1e6, (unsigned long long)hit);
    {   // a frame: ~1 400 vertex / index binds over ~500 buffers, the same addresses every frame (the front)
        uint64_t addrs[1400]; for (int i = 0; i < 1400; i++) addrs[i] = 0x10000000ull + (rnd() % 500) * 0x2000 * 60 % (30000ull * 0x2000) / 0x2000 * 0x2000 + 64;
        t0 = now_ns(); hit = 0;
        for (int f = 0; f < 200; f++) for (int i = 0; i < 1400; i++) hit += nr_stab_find(&t, addrs[i], 256) != NULL;
        printf("bench: lookup with repeating addresses (1 400 a frame): %.1f ns (%llu hits)\n", (now_ns() - t0) / (200.0 * 1400), (unsigned long long)hit);
    }
    nr_stab_add(&t, 0, (uint64_t)1 << 32 | 40000, 0x9000000ull, 0x100, 0, 0);
    t0 = now_ns(); nr_stab_apply(&t, NULL, NULL); printf("bench: one add merged into 30 000: %.3f ms\n", (now_ns() - t0) / 1e6);
    nr_stab_free(&t); nr_ring_free(&r); free(mirror); free(guest);
}

int main(void) {
    ring_units();
    // a big ring (the engine's): never wraps inside the window -> no race, everything served
    uint64_t sv, od, rc;
    ring_model(NR_RING_SIZE, 300, 600, 0, 11, &sv, &od, &rc);
    printf("ring model 19.5 MB, 300 serials x 600 binds: served %llu, old path %llu, races %llu\n", (unsigned long long)sv, (unsigned long long)od, (unsigned long long)rc);
    CHECK(rc == 0 && od == 0 && sv > 150000);
    // the same with CPU writes in the stream
    ring_model(NR_RING_SIZE, 300, 600, 1, 12, &sv, &od, &rc);
    printf("ring model 19.5 MB + CPU writes: served %llu, old path %llu, races %llu\n", (unsigned long long)sv, (unsigned long long)od, (unsigned long long)rc);
    CHECK(rc == 0 && od > 0);
    // small rings: the engine wraps within the in-flight window, i.e. one to six submissions (it never does that: ~30 frames in the
    // campaign, ~10 in menus) - the mirror then compares, holds, falls back, and still never shows a wrong byte
    for (int i = 0; i < 12; i++) {
        const uint64_t S = (uint64_t)(320 + 128 * i) << 10;
        ring_model(S, 400, 300, i & 1, 100 + i, &sv, &od, &rc);
        if (i < 3) printf("ring model %llu KB (wraps under the GPU): served %llu, old path %llu, races %llu\n", (unsigned long long)(S >> 10), (unsigned long long)sv, (unsigned long long)od, (unsigned long long)rc);
    }
    stab_units();
    stab_each_unit();
    for (int i = 0; i < 20; i++) stab_model(20000, 1000 + i);
    bench();
    printf("%s: %ld checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
