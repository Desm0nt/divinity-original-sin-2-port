// [DOS2] NR-TRAFFIC DM_NR_LINTEX host test (docs/dos2/nr/NR-TRAFFIC.md): the row runs of dm/gfx/nr/nr_lintex.h against a model of the
// page dirty tracker (dm/gfx/nvn/nvn_dirty.c semantics: a write to a protected page faults, which unprotects it and raises its
// generation above every earlier stamp; a write to a page that is not protected raises nothing), the verify helper, the give-up rule,
// and the whole mode-1 flow (protect + stamp BEFORE the read, rows copied into a model image) under random interleavings of game
// writes: the image must equal the storage after every quiet check. A negative control (protect AFTER the read, the order the old
// block-linear path uses at its upload) must be able to lose a write. Shapes: DOS2's fog texture 1424x1602 RGBA8 (pitch 5696 = row),
// padded pitches, odd sizes, storage not page aligned.
// Build: clang -std=c11 -O2 -Wall -Wextra nr_lintex_test.c   (no other file)
#include "../../../nr/nr_lintex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails = 0; static long long checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; if (fails < 30) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint64_t rs = 88172645463325252ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static uint64_t rn(uint64_t n) { return n ? rnd() % n : 0; }

// ---- tracker model over the texture storage (which starts base_off bytes into its first page) ----
enum { PG = 4096 };
typedef struct { uint64_t base_off, size, npages, now, faults, calls; uint64_t *gen; uint8_t *prot; } trk;
typedef struct { trk *t; uint64_t since; } q;
static void trk_init(trk *t, uint64_t base_off, uint64_t size) {
    memset(t, 0, sizeof *t); t->base_off = base_off; t->size = size; t->npages = (base_off + size + PG - 1) / PG;
    t->gen = calloc(t->npages, sizeof *t->gen); t->prot = calloc(t->npages, 1); t->now = 1;
}
static void trk_free(trk *t) { free(t->gen); free(t->prot); }
static void trk_protect(trk *t) { memset(t->prot, 1, t->npages); }
static void trk_fault(trk *t, uint64_t off, uint64_t len) {   // the CPU writes [off, off+len): every protected page it touches faults
    for (uint64_t p = (t->base_off + off) / PG; p <= (t->base_off + off + len - 1) / PG; p++)
        if (t->prot[p]) { t->prot[p] = 0; t->gen[p] = ++t->now; t->faults++; }
}
static int trk_since(void *c, uint64_t off, uint64_t len) {   // nvn_dirty_since: a page newer than the stamp
    q *x = c; trk *t = x->t; t->calls++;
    for (uint64_t p = (t->base_off + off) / PG; p <= (t->base_off + off + len - 1) / PG; p++) if (t->gen[p] > x->since) return 1;
    return 0;
}

// ---- 1. geometry ----
static void test_geom(void) {
    CHECK(nrlt_pitch(0, 1424, 4) == 5696, "fog pitch");
    CHECK(nrlt_pitch(0, 3, 4) == 32, "rounded to 32");
    CHECK(nrlt_pitch(8192, 1424, 4) == 8192, "stride wins");
    CHECK(nrlt_fits(9176256, 1602, 5696, 5696), "fog fits (log: size=9176256, 51264 bytes past the last row)");
    CHECK(nrlt_fits(9124992, 1602, 5696, 5696) && !nrlt_fits(9124991, 1602, 5696, 5696), "exactly the rows / one byte short");
    CHECK(nrlt_fits(1601ull * 8192 + 5696, 1602, 8192, 5696), "padded: last row needs only row bytes");
    CHECK(!nrlt_fits(100, 0, 10, 10) && !nrlt_fits(100, 2, 4, 8), "no rows / pitch < row");
}

// ---- 2. runs against the tracker model: no written row missed, runs sorted / disjoint / in range / within cap; exact without gap ----
static void ref_rows(const trk *t, uint64_t since, uint32_t h, uint64_t pitch, uint64_t row, uint8_t *dirty) {
    for (uint32_t y = 0; y < h; y++) {
        dirty[y] = 0;
        for (uint64_t p = (t->base_off + y * pitch) / PG; p <= (t->base_off + y * pitch + row - 1) / PG; p++) if (t->gen[p] > since) dirty[y] = 1;
    }
}
static void test_runs_random(void) {
    nrlt_run runs[64];
    for (int it = 0; it < 4000; it++) {
        const uint32_t w = 1 + (uint32_t)rn(it < 200 ? 1424 : 700), bpp = (uint32_t[]){1, 2, 4, 8, 16}[rn(5)], h = 1 + (uint32_t)rn(it < 200 ? 1602 : 900);
        const uint64_t row = (uint64_t)w * bpp, pitch = rn(3) ? row : row + rn(4096);
        const uint64_t size = (uint64_t)(h - 1) * pitch + row + rn(64), base_off = rn(PG);
        trk t; trk_init(&t, base_off, size); trk_protect(&t);
        const uint64_t since = t.now;
        const int kind = (int)rn(4);   // 0 few rows, 1 a band, 2 dense, 3 bytes in the padding only
        if (kind == 0) for (int k = 0, n = 1 + (int)rn(12); k < n; k++) { uint64_t y = rn(h); trk_fault(&t, y * pitch + rn(row), 1 + rn(row - (row > 1 ? 1 : 0))); }
        else if (kind == 1) { uint64_t y0 = rn(h), n = 1 + rn(h - y0); trk_fault(&t, y0 * pitch, (n - 1) * pitch + row); }
        else if (kind == 2) trk_fault(&t, 0, size);
        else if (pitch > row) for (int k = 0; k < 6; k++) { uint64_t y = rn(h - 1 < 1 ? 1 : h - 1); trk_fault(&t, y * pitch + row, pitch - row); }
        uint8_t *ref = malloc(h);
        ref_rows(&t, since, h, pitch, row, ref);
        const uint32_t cap = it & 1 ? 64 : 1 + (uint32_t)rn(8), gap = it & 2 ? 0 : (uint32_t)rn(9), group = (uint32_t[]){1, 4, 16, 64}[rn(4)];
        uint32_t drows = 0; q x = {&t, since};
        const uint32_t n = nrlt_runs(h, pitch, row, trk_since, &x, group, gap, runs, cap, &drows);
        uint32_t want = 0; for (uint32_t y = 0; y < h; y++) want += ref[y];
        CHECK(drows == want, "dirty rows %u want %u (h %u pitch %llu row %llu group %u)", drows, want, h, (unsigned long long)pitch, (unsigned long long)row, group);
        CHECK(n <= cap, "runs %u > cap %u", n, cap);
        uint32_t prev_end = 0; int ok = 1;
        for (uint32_t i = 0; i < n; i++) { if (!runs[i].n || runs[i].y0 < prev_end || runs[i].y0 + runs[i].n > h) ok = 0; prev_end = runs[i].y0 + runs[i].n; }
        CHECK(ok, "runs sorted, disjoint, non-empty, in range");
        uint32_t k = 0, missed = 0, extra = 0;
        for (uint32_t y = 0; y < h; y++) {
            while (k < n && runs[k].y0 + runs[k].n <= y) k++;
            const int in = k < n && runs[k].y0 <= y;
            if (ref[y] && !in) missed++;
            if (!ref[y] && in) extra++;
        }
        CHECK(!missed, "%u written rows outside every run", missed);
        if (gap == 0 && cap >= 64 && n < cap) CHECK(!extra, "gap 0, no cap hit: exactly the written rows (%u extra)", extra);
        if (want == 0) CHECK(n == 0, "nothing written, no run");
        free(ref); trk_free(&t);
    }
}

// ---- 3. verify helper ----
static void test_missed(void) {
    enum { H = 40, ROW = 24, PITCH = 32 };
    uint8_t was[H * ROW], now[H * PITCH];
    for (int i = 0; i < H * PITCH; i++) now[i] = (uint8_t)i;
    for (int y = 0; y < H; y++) memcpy(was + y * ROW, now + y * PITCH, ROW);
    nrlt_run r[2] = {{3, 2}, {10, 1}};
    uint32_t ch = 9;
    CHECK(nrlt_missed(was, now, PITCH, H, ROW, r, 2, &ch) == 0 && ch == 0, "unchanged");
    now[4 * PITCH + 5] ^= 1; now[10 * PITCH] ^= 1;
    CHECK(nrlt_missed(was, now, PITCH, H, ROW, r, 2, &ch) == 0 && ch == 2, "changes inside the runs");
    now[7 * PITCH + 23] ^= 1; now[39 * PITCH] ^= 1;
    CHECK(nrlt_missed(was, now, PITCH, H, ROW, r, 2, &ch) == 2 && ch == 4, "two rows outside");
    now[8 * PITCH + ROW] ^= 1;   // the padding is not texel data
    CHECK(nrlt_missed(was, now, PITCH, H, ROW, r, 2, &ch) == 2 && ch == 4, "padding ignored");
}

// ---- 4. give-up rule ----
static void test_policy(void) {
    nrlt_policy p = {0};
    CHECK(!nrlt_note(&p, 900, 1602, 50, 3) && !nrlt_note(&p, 1000, 1602, 50, 3), "two dense");
    CHECK(!nrlt_note(&p, 10, 1602, 50, 3), "a sparse one resets");
    CHECK(!nrlt_note(&p, 1602, 1602, 50, 3) && !nrlt_note(&p, 1602, 1602, 50, 3) && nrlt_note(&p, 1602, 1602, 50, 3), "three dense in a row give up");
    nrlt_policy z = {0}; CHECK(!nrlt_note(&z, 802, 1602, 50, 3) && !nrlt_note(&z, 802, 1602, 50, 3) && nrlt_note(&z, 802, 1602, 50, 3), "just over half");
    nrlt_policy hf = {0}; int a2 = 0; for (int i = 0; i < 9; i++) a2 |= nrlt_note(&hf, 801, 1602, 50, 3); CHECK(!a2, "exactly half is not dense");
    nrlt_policy u = {0}; int any = 0; for (int i = 0; i < 100; i++) any |= nrlt_note(&u, 801, 1602, 50, 1000); CHECK(!any, "streak not reached");
}

// ---- 5. the mode-1 flow: storage written by the game, image updated by checks; protect + stamp before the read ----
// Each round: [game writes] -> check: if any page newer than the stamp: protect, new stamp, copy the rows dirty since the OLD stamp
// (some game writes land between the protect and the read, some after the read). After a quiet check (no write since), image == storage.
static int flow(uint32_t w, uint32_t bpp, uint32_t h, uint64_t pad, uint64_t base_off, int bad_order, int rounds, uint64_t *rows_copied, uint64_t *rows_total) {
    const uint64_t row = (uint64_t)w * bpp, pitch = row + pad, size = (uint64_t)(h - 1) * pitch + row;
    uint8_t *st = malloc(size), *img = malloc(row * h);
    for (uint64_t i = 0; i < size; i++) st[i] = (uint8_t)rnd();
    trk t; trk_init(&t, base_off, size);
    nrlt_run runs[64]; int lost = 0;
    // first upload: protect + stamp, then read everything (good order); bad order: read, then protect + stamp
    uint64_t stamp;
    if (!bad_order) { trk_protect(&t); stamp = t.now; }
    for (uint32_t y = 0; y < h; y++) memcpy(img + y * row, st + y * pitch, row);
    if (bad_order) {
        // a game write right between the read and the protect: on an unprotected page, no fault
        uint64_t y = rn(h); st[y * pitch] ^= 0x5a;
        trk_protect(&t); stamp = t.now;
    }
    for (int r = 0; r < rounds; r++) {
        // game writes before the check (fault on protected pages)
        const int kind = (int)rn(10);
        int nw = kind < 7 ? (int)rn(6) : kind < 9 ? 20 + (int)rn(40) : 1;
        for (int k = 0; k < nw; k++) {
            uint64_t y = rn(h), off = y * pitch + rn(row), len = 1 + rn(kind == 9 ? size - off : (row - (off - y * pitch)) + (rn(4) ? 0 : pitch));
            if (off + len > size) len = size - off;
            trk_fault(&t, off, len); for (uint64_t i = 0; i < len; i++) st[off + i] = (uint8_t)rnd();
        }
        // the check (xbg_game.cpp tracked branch): pages newer than the stamp?
        q any = {&t, stamp};
        if (!trk_since(&any, 0, size)) continue;
        const uint64_t old = stamp;
        trk_protect(&t); stamp = t.now;   // protect, then stamp
        // a write between the protect and the read (faults: newer than the new stamp)
        if (rn(3) == 0) { uint64_t y = rn(h), off = y * pitch + rn(row); trk_fault(&t, off, 1); st[off] ^= 0x33; }
        q x = {&t, old}; uint32_t dr = 0;
        const uint32_t n = nrlt_runs(h, pitch, row, trk_since, &x, 16, 4, runs, 64, &dr);
        for (uint32_t i = 0; i < n; i++) for (uint32_t y = runs[i].y0; y < runs[i].y0 + runs[i].n; y++) memcpy(img + y * row, st + y * pitch, row);
        *rows_copied += nrlt_rows(runs, n); *rows_total += h;
        // a write after the read (faults: seen by the next check)
        if (rn(3) == 0) { uint64_t y = rn(h), off = y * pitch + rn(row); trk_fault(&t, off, 1); st[off] ^= 0x77; }
        // quiet check: nothing more written -> one more check settles every write, then the image must equal the storage
        if (rn(4) == 0) {
            q z = {&t, stamp};
            if (trk_since(&z, 0, size)) {
                const uint64_t o2 = stamp; trk_protect(&t); stamp = t.now;
                q x2 = {&t, o2}; uint32_t d2 = 0; const uint32_t n2 = nrlt_runs(h, pitch, row, trk_since, &x2, 16, 4, runs, 64, &d2);
                for (uint32_t i = 0; i < n2; i++) for (uint32_t y = runs[i].y0; y < runs[i].y0 + runs[i].n; y++) memcpy(img + y * row, st + y * pitch, row);
            }
            for (uint32_t y = 0; y < h; y++) if (memcmp(img + y * row, st + y * pitch, row)) { lost++; break; }
        }
    }
    free(st); free(img); trk_free(&t);
    return lost;
}
static void test_flow(void) {
    uint64_t cp = 0, tot = 0; int lost = 0;
    lost += flow(1424, 4, 1602, 0, 0, 0, 300, &cp, &tot);            // DOS2's fog texture, page-aligned storage
    lost += flow(1424, 4, 1602, 0, 1234, 0, 300, &cp, &tot);         // storage starting mid-page
    for (int i = 0; i < 60; i++) lost += flow(1 + (uint32_t)rn(600), (uint32_t[]){1, 2, 4, 8}[rn(4)], 1 + (uint32_t)rn(700), rn(2) ? 0 : rn(300), rn(PG), 0, 150, &cp, &tot);
    CHECK(lost == 0, "good order (protect + stamp before the read): %d quiet checks with stale rows", lost);
    printf("  flow: copied %.2f %% of the rows checked (sparse writes mostly)\n", tot ? 100.0 * cp / tot : 0.0);
    int bad = 0; uint64_t c2 = 0, t2 = 0;
    for (int i = 0; i < 40; i++) bad += flow(64, 4, 64, 0, 0, 1, 40, &c2, &t2) > 0;
    CHECK(bad > 0, "negative control: protect AFTER the read can keep a stale row (%d of 40 runs did)", bad);
    printf("  negative control (protect after the read): %d of 40 runs kept a stale row\n", bad);
}


// ---- 5b. stores in flight: a store to a protected page faults (generation raised, page unprotected) and re-executes LATER - possibly
// after our read; re-executed on a page protected again meanwhile, it faults again. The check is three steps in one of two orders:
//   order 0 (this path, nrlin::protect_stamp): stamp, protect, read;   order 1 (the engine's tracked branch today): protect, stamp, read.
// Each in-flight store faults in slot fs and lands in slot ls >= fs (slot 0 = before the first step ... slot 3 = after the read).
// After each round a quiet check (no store in flight) runs; then image must equal storage. Order 1 can lose a store that faulted
// between its protect and its stamp (generation <= stamp) and landed after the read; order 0 never.
typedef struct { uint64_t off; uint8_t val; int fs, ls, pending; } inflight;
static void step_slot(trk *t, uint8_t *st, inflight *f, int nf, int slot) {
    for (int i = 0; i < nf; i++) {
        uint64_t pg = (t->base_off + f[i].off) / PG;
        if (f[i].fs == slot) {
            if (t->prot[pg]) { t->prot[pg] = 0; t->gen[pg] = ++t->now; t->faults++; f[i].pending = 1; }
            else st[f[i].off] = f[i].val;   // not protected: lands at once
        }
        if (f[i].pending && f[i].ls == slot) {
            if (t->prot[pg]) { t->prot[pg] = 0; t->gen[pg] = ++t->now; t->faults++; }   // protected again meanwhile: faults again
            st[f[i].off] = f[i].val; f[i].pending = 0;
        }
    }
}
static int flow2(int order, uint32_t w, uint32_t h, int rounds) {
    const uint64_t row = (uint64_t)w * 4, pitch = row, size = (uint64_t)h * pitch;
    uint8_t *st = malloc(size), *img = malloc(size);
    for (uint64_t i = 0; i < size; i++) st[i] = (uint8_t)rnd();
    trk t; trk_init(&t, 0, size);
    nrlt_run runs[64]; int lost = 0;
    uint64_t stamp = t.now; trk_protect(&t); memcpy(img, st, size);   // first upload, order 0
    for (int r = 0; r < rounds; r++) {
        for (int q2 = 0; q2 < 2; q2++) {   // q2 0: a round with stores in flight; 1: the quiet check right after
            inflight f[6]; int nf = q2 ? 0 : 1 + (int)rn(5);
            for (int i = 0; i < nf; i++) { f[i].off = rn(size); f[i].val = (uint8_t)rnd(); f[i].fs = (int)rn(4); f[i].ls = f[i].fs + (int)rn(4 - f[i].fs); f[i].pending = 0; }
            const uint64_t old = stamp; uint64_t ns = 0;
            for (int slot = 0; slot < 4; slot++) {
                step_slot(&t, st, f, nf, slot);
                if (slot == 3) break;
                const int op = order == 0 ? (int[]){0, 1, 2}[slot] : (int[]){1, 0, 2}[slot];   // 0 stamp, 1 protect, 2 read
                if (op == 0) ns = t.now;
                else if (op == 1) trk_protect(&t);
                else { q x = {&t, old}; uint32_t dr = 0; const uint32_t n = nrlt_runs(h, pitch, row, trk_since, &x, 16, 4, runs, 64, &dr);
                       for (uint32_t i = 0; i < n; i++) memcpy(img + (uint64_t)runs[i].y0 * row, st + (uint64_t)runs[i].y0 * pitch, (size_t)runs[i].n * row); }
            }
            stamp = ns;
        }
        if (memcmp(img, st, size)) { lost++; memcpy(img, st, size); }   // count once, resync the model
    }
    free(st); free(img); trk_free(&t);
    return lost;
}
static void test_order(void) {
    int lost0 = 0, lost1 = 0;
    for (int i = 0; i < 40; i++) { lost0 += flow2(0, 64 + (uint32_t)rn(256), 16 + (uint32_t)rn(64), 300); lost1 += flow2(1, 64 + (uint32_t)rn(256), 16 + (uint32_t)rn(64), 300); }
    CHECK(lost0 == 0, "stamp, protect, read: %d rounds left a stale byte after the quiet check", lost0);
    CHECK(lost1 > 0, "negative control: protect, stamp, read must be able to lose a store in flight (%d)", lost1);
    printf("  stores in flight, 40 x 300 rounds: stamp-protect-read lost %d, protect-stamp-read lost %d\n", lost0, lost1);
}

// ---- 6. cost of the run search on the fog texture (x86, model tracker) ----
static void bench(void) {
    trk t; const uint64_t pitch = 5696, size = 9176256; trk_init(&t, 0, size); trk_protect(&t);
    const uint64_t since = t.now;
    for (int k = 0; k < 8; k++) { uint64_t y = rn(1602); trk_fault(&t, y * pitch, pitch * 3); }
    nrlt_run runs[64]; uint32_t dr = 0, n = 0; q x = {&t, since};
    struct timespec a, b; timespec_get(&a, TIME_UTC);
    for (int i = 0; i < 2000; i++) n = nrlt_runs(1602, pitch, pitch, trk_since, &x, 16, 4, runs, 64, &dr);
    timespec_get(&b, TIME_UTC);
    const double us = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / 2000.0 / 1e3;
    printf("  bench: 1424x1602 RGBA8, 8 sparse writes: %u runs, %u rows, %.1f us a check, %.0f tracker calls a check\n", n, dr, us, t.calls / 2000.0);
    CHECK(n >= 1 && dr >= 8, "bench found the writes");
    trk_free(&t);
}

int main(void) {
    test_geom(); test_runs_random(); test_missed(); test_policy(); test_flow(); test_order(); bench();
    printf("%s: %lld checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
