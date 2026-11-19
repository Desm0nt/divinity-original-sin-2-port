// [DOS2] NR-ZERO Z3 "cheap key" host test (dm/gfx/nr/nr_z3k.h; docs/dos2/nr/NR-ZERO.md section 17): the front table of nr_z3.h's table -
// indexed by the record's key id and a few words in hand, compared in place against the live binding tables.
//   1  layout: an entry is 5 lines of 64 bytes, the words of the comparison come first
//   2  the fixed-words ids: exact interning (memo, table, a new id, a full table clears the front table and restarts the ids)
//   3  the table: find / put / replace, two ways with the older one replaced, the stamps clear it, what a store refuses
//   4  THE statement: with ONE entry stored for a state A and the live tables then holding a state B, the front table answers
//      if and only if nrz_key's words of B equal nrz_key's words of A (2 M random pairs over small value sets, so that equal keys,
//      keys that differ in one word and keys that differ only in a slot the shader set does not read all happen)
//   5  one input at a time, as nr_z3_test.c's part 3, through the front table in front of nr_z3.h's table in front of the model of the
//      three lookups: changing ONE input never gives the answer of the draw before; a slot the shader set does not read may change
//      freely (the entry still answers)
//   6  model against front table on a random stream of draws and events (4 x 1 M draws; tables of 2, 16, 256 and 4096 entries - the
//      table of 2 has one place to look, so only the comparison separates the keys): every answer of the front table is the model's,
//      and is the answer nr_z3.h's table gives for the same draw whenever that table holds the key too
// The mutation check: built with -DNRZK_MUTATE=k (k = 1..16: one compared word left out / one stamp not compared / the fixed-words id
// blind to a word / a store that hides a stage, nr_z3k.h) this test must FAIL.
// Build (Windows LLVM or any C11 compiler), from the repository root:
//   clang -std=c11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS dm/gfx/vulkan/engine/tests/nr_z3k_test.c -o nr_z3k_test && ./nr_z3k_test
//   for k in $(seq 1 16); do clang -std=c11 -O2 -D_CRT_SECURE_NO_WARNINGS -DNRZK_MUTATE=$k dm/gfx/vulkan/engine/tests/nr_z3k_test.c -o nr_z3k_mut && ! ./nr_z3k_mut >/dev/null || echo "mutation $k NOT caught"; done
#include "../../../nr/nr_z3k.h"
#include <stddef.h>
#include <stdio.h>

static long fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { if (fails < 20) printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static uint64_t rs = 0x243F6A8885A308D3ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }

// ---------------- the live state: the backend's binding tables ----------------
static uint64_t tex[NRZK_STAGES][NRZK_SLOTS], samp[NRZK_STAGES][NRZK_SLOTS];

// ---------------- the model: what the path without the tables resolves (as nr_z3_test.c) ----------------
enum { M_SETS = 26, M_TEX = 40, M_SAMP = 6, M_PASS = 6, M_KPS = 3, M_KEYS = M_SETS * M_KPS, M_D08 = 4096, M_D09 = 4096, M_ROWS = NRZ_IMG_MAX + 2, M_FIXED = 24 };
typedef struct { uint64_t sh[5]; unsigned nimg; nrzk_row row[M_ROWS]; uint64_t plan, layout_def; } mset;   // a shader set: its objects, the rows its plan reads, NR-08's plan
typedef struct { unsigned set; uint64_t fixed[6], th[2 * M_ROWS]; uint32_t key_id; uint64_t var4, var5; unsigned pass; } mdraw;   // what a draw binds
typedef struct { uint64_t w[5 + 6 + 2 * M_ROWS]; unsigned n; int32_t id; } m08;
typedef struct { uint64_t w[7]; uint64_t pipe; unsigned gen; } m09;
static mset sets[M_SETS];
static uint64_t pass_def[M_PASS];
static uint64_t fixed_pool[M_FIXED][6];   // the fixed-words sets of the scene (viewport sizes, the pools): few, some differ in one word only
static m08 d08[M_D08]; static unsigned n08;
static m09 d09[M_D09]; static unsigned n09, gen09 = 1;
static uint64_t stamp[NRZ_STAMPS] = {1, 1, 1}, lane_epoch = 1;
static uint64_t next_addr = 0x7000000000ull, next_pipe = 0x1000;
static uint64_t addr(void) { next_addr += 0x40 + 8 * rn(8); return next_addr; }

static void rows_random(mset *s) {   // distinct (stage, slot) per row, from a small pool: the sets share slots
    static const uint8_t stages[3] = {0, 1, 4};
    for (unsigned i = 0; i < s->nimg; i++) for (;;) {
        nrzk_row r; r.ns = stages[rn(3)]; r.slot = (uint8_t)rn(14); r.samp = rn(4) ? (uint8_t)rn(5) : (uint8_t)NRZK_NO_SAMP;
        unsigned k = 0; while (k < i && !(s->row[k].ns == r.ns && s->row[k].slot == r.slot)) k++;
        if (k == i) { s->row[i] = r; break; }
    }
}
static void model_init(void) {
    for (unsigned i = 0; i < M_SETS; i++) {
        memset(&sets[i], 0, sizeof sets[i]);
        sets[i].sh[0] = addr(); sets[i].sh[4] = rn(8) ? addr() : 0;
        sets[i].nimg = i < 3 ? i : i == 3 ? NRZ_IMG_MAX : i == 4 ? NRZ_IMG_MAX + 1 : rn(7);
        sets[i].plan = addr(); sets[i].layout_def = addr();
        rows_random(&sets[i]);
    }
    // two shader sets that differ in ONE object, with the same rows
    sets[6].sh[0] = sets[5].sh[0]; sets[5].sh[4] = addr(); sets[6].sh[4] = addr(); sets[6].nimg = sets[5].nimg; memcpy(sets[6].row, sets[5].row, sizeof sets[5].row);
    sets[8].sh[4] = sets[7].sh[4]; sets[8].nimg = sets[7].nimg; memcpy(sets[8].row, sets[7].row, sizeof sets[7].row);
    // a shader set with a geometry stage between the vertex and the fragment shader of set 9 (the engine never asks the front table for it)
    sets[25] = sets[9]; sets[25].sh[3] = addr(); sets[25].plan = addr(); sets[25].layout_def = addr();
    for (unsigned i = 0; i < M_PASS; i++) pass_def[i] = addr();
    for (unsigned i = 0; i < M_FIXED; i++) {
        uint64_t *f = fixed_pool[i];
        if (i >= 6 && i < 18) { memcpy(f, fixed_pool[i - 6], sizeof fixed_pool[0]); f[i % 6] += 1 + rn(3); continue; }   // one word of another set moved (every word twice)
        f[0] = (uint64_t)(rn(2) ? 1920 : 960) << 32 | (rn(2) ? 1080 : 540); f[1] = 0x5000 + 0x1000 * rn(2); f[2] = 256 * rn(2); f[3] = 1024 << rn(2); f[4] = 0x9000 + 0x1000 * rn(2); f[5] = 64 * rn(2);
    }
    n08 = n09 = 0; ++gen09; stamp[0] = stamp[1] = stamp[2] = 1; lane_epoch = 1;
    memset(tex, 0, sizeof tex); memset(samp, 0, sizeof samp);
}
static void ev_flush(void) { n08 = 0; stamp[NRZ_ST_DESC]++; }
static void ev_release(void) { n08 = 0; stamp[NRZ_ST_DESC]++; for (unsigned i = 0; i < M_SETS; i++) sets[i].plan = rn(2) ? addr() : sets[(i + 1) % M_SETS].plan; }
static void ev_clear09(void) { n09 = 0; ++gen09; stamp[NRZ_ST_PIPE]++; }
static void ev_relearn(void) { if (!n09) return; m09 *e = &d09[rn(n09)]; e->pipe = next_pipe++; }
static void ev_objects(void) { unsigned a = rn(M_PASS), b = rn(M_PASS); uint64_t t = pass_def[a]; pass_def[a] = pass_def[b]; pass_def[b] = t; stamp[NRZ_ST_OBJ]++; }

// The draw's binds land in the tables (the record's binds, applied before its draw runs).
static void bind(const mdraw *d) {
    const mset *s = &sets[d->set];
    for (unsigned i = 0; i < s->nimg; i++) { const nrzk_row r = s->row[i]; tex[r.ns][r.slot] = d->th[2 * i]; if (r.samp != NRZK_NO_SAMP) samp[r.ns][r.samp] = d->th[2 * i + 1]; }
}
// nrdesc::gather + nrz_key: the full key of the state as it is, read through the shader set's rows. 0 = too many rows.
static unsigned full_key(const mdraw *d, uint64_t *key) {
    const mset *s = &sets[d->set]; uint64_t th[2 * M_ROWS];
    for (unsigned i = 0; i < s->nimg; i++) { const nrzk_row r = s->row[i]; th[2 * i] = tex[r.ns][r.slot]; th[2 * i + 1] = r.samp != NRZK_NO_SAMP ? samp[r.ns][r.samp] : 0; }
    return nrz_key(key, s->sh, d->fixed, th, s->nimg, d->key_id, d->var4, d->var5, pass_def[d->pass]);
}
// The resolution of the draw whose full key is key[n] (always succeeds: an unknown key is added, as the old path teaches its tables).
static void model_resolve(const mdraw *d, const uint64_t *key, unsigned n, nrz_val *out) {
    const mset *s = &sets[d->set];
    const unsigned n8 = n - NRZ_KEY_TAIL;   // NR-08's words: the shader objects, the fixed words, the handles
    int32_t id = -1;
    for (unsigned i = 0; i < n08 && id < 0; i++) if (d08[i].n == n8 && !memcmp(d08[i].w, key, n8 * 8)) id = d08[i].id;
    if (id < 0) { if (n08 == M_D08) ev_flush(); m08 k; memset(&k, 0, sizeof k); memcpy(k.w, key, n8 * 8); k.n = n8; k.id = id = (int32_t)n08; d08[n08++] = k; }
    if (lane_epoch != stamp[NRZ_ST_OBJ]) { ev_clear09(); lane_epoch = stamp[NRZ_ST_OBJ]; }
    const uint64_t pw[7] = {d->key_id, s->sh[0], s->sh[4], s->layout_def, pass_def[d->pass], d->var4, d->var5};
    m09 *p = NULL;
    for (unsigned i = 0; i < n09 && !p; i++) if (!memcmp(d09[i].w, pw, sizeof pw)) p = &d09[i];
    if (!p) { if (n09 == M_D09) ev_clear09(); p = &d09[n09++]; memcpy(p->w, pw, sizeof pw); p->pipe = next_pipe++; p->gen = gen09; }
    memset(out, 0, sizeof *out);
    out->plan = s->plan; out->pent = (uint64_t)(uintptr_t)p; out->did = id; out->frag = (uint8_t)(d->set & 1);
}
static void draw_random(mdraw *d) {
    memset(d, 0, sizeof *d);
    d->set = rn(M_SETS);
    memcpy(d->fixed, fixed_pool[rn(M_FIXED)], sizeof d->fixed);
    for (unsigned i = 0; i < M_ROWS; i++) { d->th[2 * i] = 0x100 + rn(M_TEX); d->th[2 * i + 1] = 0x10 + rn(M_SAMP); }
    d->key_id = 1 + (rn(100) ? d->set : rn(M_SETS)) * M_KPS + rn(M_KPS); d->var4 = (uint64_t)(4 + rn(2)) | (uint64_t)1 << 8 | (uint64_t)(1 + rn(3)) << 16 | (uint64_t)rn(2) << 30 | (uint64_t)(50 + rn(3)) << 32;
    d->var5 = (uint64_t)rn(6) | (uint64_t)rn(5) << 10 | (uint64_t)rn(4) << 30; d->pass = rn(M_PASS);
}
static int special(const mset *s) { return s->sh[1] || s->sh[2] || s->sh[3]; }   // nrz3::draw's gate: such a draw never asks
// The front table asked for the state as it is, as nrz3::draw asks it: the stamps, the fixed-words id (never made by a lookup), the
// in-place comparison.
static const nrzk_ent *front_ask(nrzk_tab *F, const mdraw *d) {
    const mset *s = &sets[d->set];
    if (special(s)) return NULL;
    nrzk_sync(F, stamp);
    const uint32_t fid = nrzk_fixed(F, d->fixed, 0);
    return fid ? nrzk_find(F, d->key_id, s->sh[0], s->sh[4], d->var4, d->var5, pass_def[d->pass], fid, tex, samp) : NULL;
}
// One draw through the front table in front of nr_z3.h's table in front of the model, as xbg_nr_z3.inc does it. Returns 2 = the front
// table answered, 1 = nr_z3.h's table answered (the front table learns the entry), 0 = neither (both learn the model's answer under the
// stamps after the model ran).
static long fhits = 0, thits = 0, misses = 0, longs = 0, both = 0, front_only = 0;
static int through(nrzk_tab *F, nrz_tab *T, const mdraw *d, nrz_val *got) {
    const mset *s = &sets[d->set];
    bind(d);
    uint64_t key[NRZ_KEY_MAX + 8];
    const unsigned n = full_key(d, key);
    nrz_val truth;
    if (!n) { ++longs; return 0; }
    const uint64_t h = nrz_hash(key, n);
    const nrzk_ent *fe = front_ask(F, d);
    if (fe) {
        nrzk_val_of(fe, got); model_resolve(d, key, n, &truth); ++fhits; CHECK(nrz_val_same(got, &truth));
        const m09 *pe = (const m09 *)(uintptr_t)got->pent; CHECK(pe >= d09 && pe < d09 + n09 && pe->gen == gen09 && pe->w[0] == d->key_id && pe->w[5] == d->var4 && pe->w[6] == d->var5 && pe->w[4] == pass_def[d->pass]);
        CHECK(fe->nimg == s->nimg && fe->plan == s->plan);
        // DM_NR_Z3K=2: nr_z3.h's table asked for the same draw - when it holds the key, its answer is the front table's
        int stale = 0; const nrz_val *v = nrz_find(T, h, key, n, stamp, &stale);
        CHECK(!stale); if (v) { ++both; CHECK(nrz_val_same(v, got)); } else ++front_only;
        return 2;
    }
    int stale = 0;
    const nrz_val *v = nrz_find(T, h, key, n, stamp, &stale);
    if (v) { *got = *v; model_resolve(d, key, n, &truth); ++thits; CHECK(nrz_val_same(got, &truth));
        nrzk_put(F, stamp, key, n, s->nimg, s->row, v);
        return 1; }
    model_resolve(d, key, n, &truth); ++misses;
    nrz_put(T, h, key, n, stamp, &truth);
    CHECK(nrzk_put(F, stamp, key, n, s->nimg, s->row, &truth) == !special(s));
    return 0;
}

// ---------------- 1: layout ----------------
static void test_layout(void) {
    CHECK(sizeof(nrzk_ent) == 320);
    CHECK(offsetof(nrzk_ent, fixed_id) == 64 && offsetof(nrzk_ent, row) == 76 && offsetof(nrzk_ent, th) == 112);
    CHECK(sizeof(nrzk_row) == 3);
    nrzk_tab t; CHECK(nrzk_init(&t, 3, 64) == -1 && nrzk_init(&t, 0, 64) == -1 && nrzk_init(&t, 1, 64) == -1 && nrzk_init(&t, 8, 0) == -1 && nrzk_init(&t, 8, 48) == -1);
    CHECK(nrzk_init(&t, 8, 64) == 0 && t.sets == 4 && !((uintptr_t)t.e & 63));
    nrzk_free(&t); CHECK(!t.e && !t.kd);
    CHECK(nrzk_init(&t, 2, 64) == 0 && t.sets == 1); nrzk_free(&t);
}
// ---------------- 2: the fixed-words ids ----------------
static void test_fixed(void) {
    nrzk_tab t; CHECK(nrzk_init(&t, 8, 64) == 0);
    uint64_t a[6] = {1, 2, 3, 4, 5, 6}, b[6] = {1, 2, 3, 4, 5, 6};
    CHECK(nrzk_fixed(&t, a, 0) == 0 && t.nfixed == 0);                       // a lookup never adds
    CHECK(nrzk_fixed(&t, a, 1) == 1 && nrzk_fixed(&t, b, 0) == 1 && nrzk_fixed(&t, b, 1) == 1 && t.nfixed == 1);
    for (unsigned i = 0; i < 6; i++) { b[i] ^= 1ull << (7 * i); CHECK(nrzk_fixed(&t, b, 0) == 0); CHECK(nrzk_fixed(&t, a, 0) == 1); b[i] = a[i]; }   // every word, against the memo
    for (unsigned i = 0; i < 6; i++) { memcpy(b, a, sizeof b); b[i] += 100; CHECK(nrzk_fixed(&t, b, 1) == 2 + i); }
    for (unsigned i = 0; i < 6; i++) { memcpy(b, a, sizeof b); b[i] += 100; CHECK(nrzk_fixed(&t, b, 0) == 2 + i); CHECK(nrzk_fixed(&t, a, 0) == 1); }   // every word, against the table
    // a full id table: the front table is cleared with it (an entry never outlives its id)
    const uint64_t st[NRZ_STAMPS] = {1, 1, 1}; nrzk_sync(&t, st);
    uint64_t key[NRZ_KEY_MAX] = {0}; key[0] = 0x10; key[4] = 0x20; memcpy(key + 5, a, sizeof a); key[11] = 7; key[12] = 0x44; key[13] = 0x55; key[14] = 0x66;
    nrz_val v; memset(&v, 0, sizeof v); v.did = 3; v.plan = 0x99;
    CHECK(nrzk_put(&t, st, key, 15, 0, NULL, &v) == 1);
    CHECK(nrzk_find(&t, 7, 0x10, 0x20, 0x44, 0x55, 0x66, nrzk_fixed(&t, a, 0), tex, samp) != NULL);
    const uint64_t clears = t.clears;
    for (unsigned i = t.nfixed; i < NRZK_FIXED_MAX; i++) { memcpy(b, a, sizeof b); b[0] = 1000 + i; CHECK(nrzk_fixed(&t, b, 1) == i + 1); }
    CHECK(t.clears == clears && nrzk_fixed(&t, a, 0) == 1 && nrzk_find(&t, 7, 0x10, 0x20, 0x44, 0x55, 0x66, 1, tex, samp) != NULL);
    b[0] = 5000; CHECK(nrzk_fixed(&t, b, 1) == 1 && t.clears == clears + 1 && t.fixed_full == 1);   // ids restart
    CHECK(nrzk_fixed(&t, a, 0) == 0);
    CHECK(nrzk_find(&t, 7, 0x10, 0x20, 0x44, 0x55, 0x66, 1, tex, samp) == NULL);   // id 1 is another set of words now: the entry stored under the old id 1 is gone
    nrzk_free(&t);
}
// ---------------- 3: the table ----------------
static void test_table(void) {
    nrzk_tab t; CHECK(nrzk_init(&t, 2, 64) == 0);   // one place to look
    memset(tex, 0, sizeof tex); memset(samp, 0, sizeof samp);
    uint64_t st[NRZ_STAMPS] = {1, 1, 1};
    const uint64_t f[6] = {1, 2, 3, 4, 5, 6};
    const nrzk_row row[2] = {{1, 3, 2}, {0, 5, NRZK_NO_SAMP}};
    uint64_t k[NRZ_KEY_MAX] = {0};   // vs, -, -, -, fs, 6 fixed, (t0, s0), (t1, 0), key id, var4, var5, pass
    k[0] = 0x10; k[4] = 0x20; memcpy(k + 5, f, sizeof f); k[11] = 0x101; k[12] = 0x11; k[13] = 0x102; k[14] = 0; k[15] = 7; k[16] = 0x44; k[17] = 0x55; k[18] = 0x66;
    nrz_val v1, v2, v3, got; memset(&v1, 0, sizeof v1); v1.did = 1; v1.plan = 0x1000; v1.pent = 0x2000; v1.frag = 1; v2 = v1; v2.did = 2; v3 = v1; v3.did = 3;
    nrzk_sync(&t, st);
    CHECK(nrzk_fixed(&t, f, 0) == 0);
    CHECK(nrzk_put(&t, st, k, 19, 2, row, &v1) == 1 && t.puts == 1);
    const uint32_t fid = nrzk_fixed(&t, f, 0); CHECK(fid == 1);
    CHECK(!nrzk_find(&t, 7, 0x10, 0x20, 0x44, 0x55, 0x66, fid, tex, samp));               // the tables do not hold the handles
    tex[1][3] = 0x101; samp[1][2] = 0x11; tex[0][5] = 0x102;
    const nrzk_ent *e = nrzk_find(&t, 7, 0x10, 0x20, 0x44, 0x55, 0x66, fid, tex, samp); CHECK(e != NULL);
    if (e) { nrzk_val_of(e, &got); CHECK(nrz_val_same(&got, &v1)); }
    samp[0][5] = 0x77; tex[4][3] = 0x9; CHECK(nrzk_find(&t, 7, 0x10, 0x20, 0x44, 0x55, 0x66, fid, tex, samp) != NULL);   // slots the entry does not read
    CHECK(nrzk_put(&t, st, k, 19, 2, row, &v2) == 1 && t.replaced == 0);                    // the same key: replaced in place
    e = nrzk_find(&t, 7, 0x10, 0x20, 0x44, 0x55, 0x66, fid, tex, samp); CHECK(e && e->did == 2);
    uint64_t k2[NRZ_KEY_MAX]; memcpy(k2, k, sizeof k2); k2[11] = 0x201;                     // another texture: the other way
    CHECK(nrzk_put(&t, st, k2, 19, 2, row, &v3) == 1 && t.replaced == 0);
    e = nrzk_find(&t, 7, 0x10, 0x20, 0x44, 0x55, 0x66, fid, tex, samp); CHECK(e && e->did == 2);
    tex[1][3] = 0x201; e = nrzk_find(&t, 7, 0x10, 0x20, 0x44, 0x55, 0x66, fid, tex, samp); CHECK(e && e->did == 3);   // k2 used last
    uint64_t k3[NRZ_KEY_MAX]; memcpy(k3, k, sizeof k3); k3[11] = 0x301;
    CHECK(nrzk_put(&t, st, k3, 19, 2, row, &v1) == 1 && t.replaced == 1);                   // the older one (k) goes
    tex[1][3] = 0x101; CHECK(!nrzk_find(&t, 7, 0x10, 0x20, 0x44, 0x55, 0x66, fid, tex, samp));
    tex[1][3] = 0x201; CHECK(nrzk_find(&t, 7, 0x10, 0x20, 0x44, 0x55, 0x66, fid, tex, samp) != NULL);
    // what a store refuses
    uint64_t kb[NRZ_KEY_MAX]; memcpy(kb, k, sizeof kb); kb[15] = 0; CHECK(nrzk_put(&t, st, kb, 19, 2, row, &v1) == 0);          // no key id
    memcpy(kb, k, sizeof kb); kb[15] |= 1ull << 40; CHECK(nrzk_put(&t, st, kb, 19, 2, row, &v1) == 0);
    CHECK(nrzk_put(&t, st, k, 18, 2, row, &v1) == 0 && nrzk_put(&t, st, k, 19, NRZ_IMG_MAX + 1, row, &v1) == 0);                // not nrz_key's length
    { const nrzk_row bad[2] = {{6, 3, 2}, {0, 5, NRZK_NO_SAMP}}, bad2[2] = {{1, 128, 2}, {0, 5, NRZK_NO_SAMP}}, bad3[2] = {{1, 3, 128}, {0, 5, NRZK_NO_SAMP}};
      CHECK(nrzk_put(&t, st, k, 19, 2, bad, &v1) == 0 && nrzk_put(&t, st, k, 19, 2, bad2, &v1) == 0 && nrzk_put(&t, st, k, 19, 2, bad3, &v1) == 0); }
    // every stamp alone clears the table (and a store under new stamps is found under them)
    for (unsigned i = 0; i < NRZ_STAMPS; i++) {
        CHECK(nrzk_put(&t, st, k2, 19, 2, row, &v3) == 1);
        nrzk_sync(&t, st); CHECK(nrzk_find(&t, 7, 0x10, 0x20, 0x44, 0x55, 0x66, nrzk_fixed(&t, f, 0), tex, samp) != NULL);
        st[i]++; nrzk_sync(&t, st);
        CHECK(nrzk_fixed(&t, f, 0) == 0);                                                    // the ids went with the entries
        CHECK(nrzk_put(&t, st, k2, 19, 2, row, &v2) == 1);
        e = nrzk_find(&t, 7, 0x10, 0x20, 0x44, 0x55, 0x66, nrzk_fixed(&t, f, 0), tex, samp); CHECK(e && e->did == 2);
    }
    nrzk_free(&t);
}
// ---------------- 4: front hit <=> the full keys are equal ----------------
static void test_statement(void) {
    nrzk_tab F; CHECK(nrzk_init(&F, 2, 256) == 0);
    long eq = 0, ne = 0, one = 0, unread = 0;
    for (long it = 0; it < 2000000; it++) {
        if (!(it & 0xffff)) model_init();
        nrzk_clear(&F); F.stamped = 0;
        mdraw a, b; draw_random(&a);
        if (sets[a.set].nimg > NRZ_IMG_MAX || special(&sets[a.set])) continue;
        // random other binds before, so that the slots the set does not read hold anything
        for (unsigned i = 0; i < 6; i++) { tex[rn(2) ? 1 : rn(5)][rn(16)] = 0x100 + rn(M_TEX); samp[rn(2) ? 1 : rn(5)][rn(6)] = 0x10 + rn(M_SAMP); }
        bind(&a);
        uint64_t ka[NRZ_KEY_MAX + 8], kb[NRZ_KEY_MAX + 8];
        const unsigned na = full_key(&a, ka);
        nrz_val v; memset(&v, 0, sizeof v); v.did = (int32_t)(it & 0xffff); v.plan = sets[a.set].plan; v.pent = it;
        CHECK(nrzk_put(&F, stamp, ka, na, sets[a.set].nimg, sets[a.set].row, &v) == 1);
        // state B: the same draw, a draw that differs in one thing, or another draw
        b = a; const unsigned how = rn(16); int touched_unread = 0;
        switch (how) {
        case 0: case 1: break;                                                              // the same draw again
        case 2: b.set = rn(M_SETS); break;
        case 3: b.fixed[rn(6)] ^= 1ull << rn(12); break;
        case 4: if (sets[b.set].nimg) b.th[2 * rn(sets[b.set].nimg)] ^= 1ull << rn(4); break;
        case 5: if (sets[b.set].nimg) b.th[2 * rn(sets[b.set].nimg) + 1] ^= 1ull << rn(3); break;
        case 6: b.key_id = 1 + rn(M_KEYS); break;   // (any id: also another shader set's)
        case 7: b.var4 ^= 1ull << (rn(2) ? 28 + rn(3) : rn(40)); break;
        case 8: b.var5 ^= 1ull << rn(60); break;
        case 9: b.pass = rn(M_PASS); break;
        case 10: case 11: touched_unread = 1; break;                                        // binds to slots (below)
        default: draw_random(&b); break;
        }
        if (sets[b.set].nimg > NRZ_IMG_MAX || special(&sets[b.set])) continue;
        bind(&b);
        if (touched_unread) for (unsigned i = 0; i < 3; i++) { tex[rn(6)][rn(NRZK_SLOTS)] ^= 1 + rn(7); samp[rn(6)][rn(NRZK_SLOTS)] ^= 1 + rn(7); }   // any slot: read or not
        const unsigned nb = full_key(&b, kb);
        const int same = na == nb && !memcmp(ka, kb, na * 8);
        if (rn(2)) { nrzk_sync(&F, stamp); nrzk_fixed(&F, b.fixed, 1); }                     // B's fixed words are known to the table (another entry was stored with them)
        const nrzk_ent *e = front_ask(&F, &b);
        CHECK((e != NULL) == same);
        if (e) { nrz_val g; nrzk_val_of(e, &g); CHECK(nrz_val_same(&g, &v)); }
        if (same) { ++eq; if (touched_unread) ++unread; } else { ++ne; unsigned diff = 0; if (na == nb) for (unsigned i = 0; i < na; i++) diff += ka[i] != kb[i]; if (diff == 1) ++one; }
    }
    printf("  statement: %ld pairs with equal full keys (all answered; %ld of them after binds to other slots), %ld with different keys (none answered; %ld differ in exactly one word)\n", eq, unread, ne, one);
    CHECK(eq > 200000 && ne > 500000 && one > 150000 && unread > 20000);
    nrzk_free(&F);
}
// ---------------- 5: one input at a time ----------------
static void test_inputs(void) {
    nrzk_tab F; nrz_tab T; CHECK(nrzk_init(&F, 4096, 16384) == 0); CHECK(nrz_init(&T, 4096) == 0);
    enum { IN_SET_VS, IN_SET_FS, IN_SET_GS, IN_FIXED0, IN_FIXED5 = IN_FIXED0 + 5, IN_TEX, IN_SAMP, IN_TEX_LAST, IN_SAMP_LAST, IN_KEYID, IN_VAR4, IN_VAR5, IN_PASS,
           IN_UNREAD, IN_FLUSH, IN_RELEASE, IN_RELEARN, IN_CLEAR09, IN_OBJECTS, IN_N };
    for (int in = 0; in < IN_N; in++) for (int rep = 0; rep < 40; rep++) {
        model_init(); nrzk_clear(&F); F.stamped = 0; nrz_clear(&T);
        mdraw d; draw_random(&d);
        d.set = in == IN_SET_VS ? 7 : in == IN_SET_FS ? 5 : in == IN_SET_GS ? 9 : 10 + rn(M_SETS - 11); d.key_id = 1 + d.set * M_KPS + rn(M_KPS);
        mset *s = &sets[d.set];
        if (s->nimg < 2 || s->nimg > NRZ_IMG_MAX) { s->nimg = 2 + rn(4); rows_random(s); }
        if (in == IN_SAMP || in == IN_SAMP_LAST) { s->row[0].samp = 1; s->row[s->nimg - 1].samp = 3; for (unsigned i = 1; i + 1 < s->nimg; i++) s->row[i].samp = NRZK_NO_SAMP; if (s->nimg == 2 && in == IN_SAMP) s->row[1].samp = NRZK_NO_SAMP; }
        sets[8].nimg = sets[7].nimg; memcpy(sets[8].row, sets[7].row, sizeof sets[7].row); sets[6].nimg = sets[5].nimg; memcpy(sets[6].row, sets[5].row, sizeof sets[5].row);
        { const uint64_t gs = sets[25].sh[3], pl = sets[25].plan, ld = sets[25].layout_def; sets[25] = sets[9]; sets[25].sh[3] = gs; sets[25].plan = pl; sets[25].layout_def = ld; }
        nrz_val a, b, got;
        CHECK(through(&F, &T, &d, &got) == 0);
        mdraw o; draw_random(&o); o.set = 0; o.key_id = 1 + rn(M_KPS);
        if (nrzk_kd_slot(&F, o.key_id, sets[0].sh[4]) == nrzk_kd_slot(&F, d.key_id, s->sh[4])) o.key_id = o.key_id % M_KPS + 1;   // (not the hint slot of d: a moved hint is a miss by design, part 6 has them)
        through(&F, &T, &o, &got);      // another draw in between (no texture rows: it binds nothing)
        CHECK(through(&F, &T, &d, &got) == 2); a = got;                      // the front table answers
        mdraw e = d; int is_event = 0;
        switch (in) {
        case IN_SET_VS: e.set = 8; break;
        case IN_SET_FS: e.set = 6; break;
        case IN_SET_GS: e.set = 25; break;                                  // the same vertex and fragment shader + a geometry stage: never the front table's
        case IN_TEX: e.th[0] += 1; break;
        case IN_SAMP: e.th[1] += 1; break;
        case IN_TEX_LAST: e.th[2 * (s->nimg - 1)] += 1; break;
        case IN_SAMP_LAST: e.th[2 * (s->nimg - 1) + 1] += 1; break;
        case IN_KEYID: e.key_id = e.key_id % M_KEYS + 1; break;
        case IN_VAR4: e.var4 ^= 1ull << (rep % 2 ? 30 : 29); break;
        case IN_VAR5: e.var5 += 1ull << (10 * (rep % 6)); break;
        case IN_PASS: e.pass = (e.pass + 1) % M_PASS; break;
        case IN_UNREAD: {   // slots the shader set does not read: the entry still answers, with the same resolution
            for (unsigned st = 0; st < NRZK_STAGES; st++) for (unsigned sl = 0; sl < NRZK_SLOTS; sl++) {
                int rt = 0, rsm = 0; for (unsigned i = 0; i < s->nimg; i++) { if (s->row[i].ns == st && s->row[i].slot == sl) rt = 1; if (s->row[i].ns == st && s->row[i].samp == sl) rsm = 1; }
                if (!rt) tex[st][sl] += 1 + rn(9); if (!rsm) samp[st][sl] += 1 + rn(9); }
            const nrzk_ent *fe = front_ask(&F, &d); CHECK(fe != NULL);
            if (fe) { nrzk_val_of(fe, &got); CHECK(nrz_val_same(&got, &a)); }
            CHECK(through(&F, &T, &d, &got) == 2 && nrz_val_same(&got, &a)); continue; }
        case IN_FLUSH: ev_flush(); is_event = 1; break;
        case IN_RELEASE: ev_release(); is_event = 1; break;
        case IN_RELEARN: {
            const m09 *pe = (const m09 *)(uintptr_t)a.pent; const uint64_t p0 = pe->pipe; while (pe->pipe == p0) ev_relearn();
            CHECK(through(&F, &T, &d, &got) == 2 && nrz_val_same(&got, &a) && ((const m09 *)(uintptr_t)got.pent)->pipe != p0); continue; }
        case IN_CLEAR09: ev_clear09(); is_event = 1; break;
        case IN_OBJECTS: ev_objects(); is_event = 1; break;
        default: e.fixed[in - IN_FIXED0] += 1; break;
        }
        if (is_event && (in == IN_FLUSH || in == IN_RELEASE)) { mdraw q; draw_random(&q); q.set = 1; q.key_id = 1 + M_KPS + rn(M_KPS); bind(&q); uint64_t kq[NRZ_KEY_MAX + 8]; const unsigned nq = full_key(&q, kq); nrz_val x; model_resolve(&q, kq, nq, &x); }   // the id of the first draw is handed to another key first
        const int ans = through(&F, &T, &e, &got);                           // (every answer is checked against the model inside)
        CHECK(ans == 0);
        { uint64_t ke[NRZ_KEY_MAX + 8]; const unsigned ne = full_key(&e, ke); model_resolve(&e, ke, ne, &b); }
        CHECK(!nrz_val_same(&a, &b) || ((const m09 *)(uintptr_t)a.pent)->gen != gen09 - 0u || in == IN_CLEAR09 || in == IN_OBJECTS);   // the input matters: the first draw's resolution would be wrong
        if (!is_event) { CHECK(through(&F, &T, &d, &got) == 2 && nrz_val_same(&got, &a));
            const int r = through(&F, &T, &e, &got); CHECK(r == (in == IN_SET_GS ? 1 : 2) && nrz_val_same(&got, &b)); }   // both keys live side by side (the set with a geometry stage: only in nr_z3.h's table)
        else CHECK(through(&F, &T, &e, &got) == 2 && nrz_val_same(&got, &b));   // stored again under the new stamps
    }
    nrzk_free(&F); nrz_free(&T);
}
// ---------------- 6: random stream ----------------
static void test_random(void) {
    static const uint32_t fsize[4] = {2, 16, 256, 4096};
    for (int round = 0; round < 4; round++) {
        nrzk_tab F; nrz_tab T; CHECK(nrzk_init(&F, fsize[round], 256) == 0); CHECK(nrz_init(&T, round == 3 ? 256 : 4096) == 0);   // (round 3: the full table is the small one - entries it lost still answer from the front)
        model_init(); fhits = thits = misses = longs = both = front_only = 0;
        enum { W = 600 }; static mdraw w[W];
        for (int i = 0; i < W; i++) draw_random(&w[i]);
        long events = 0;
        for (long i = 0; i < 1000000; i++) {
            const unsigned r = rn(100000);
            if (r < 40) { ev_flush(); ++events; } else if (r < 44) { ev_release(); ++events; } else if (r < 90) { ev_relearn(); ++events; }
            else if (r < 96) { ev_clear09(); ++events; } else if (r < 102) { ev_objects(); ++events; }
            else if (r < 2000) draw_random(&w[rn(W)]);
            else if (r < 3000) { mdraw *d = &w[rn(W)]; switch (rn(7)) {
                case 0: d->th[2 * rn(4)] = 0x100 + rn(M_TEX); break; case 1: d->th[2 * rn(4) + 1] = 0x10 + rn(M_SAMP); break; case 2: if (rn(50)) memcpy(d->fixed, fixed_pool[rn(M_FIXED)], sizeof d->fixed); else d->fixed[rn(6)] ^= 1ull << rn(12); break;
                case 3: d->key_id = 1 + d->set * M_KPS + rn(M_KPS); break; case 4: d->var4 ^= 1ull << (28 + rn(3)); break; case 5: d->var5 ^= 1ull << rn(40); break; default: d->pass = rn(M_PASS); } }
            else if (r < 3400) { tex[rn(6)][rn(16)] = 0x100 + rn(M_TEX); samp[rn(6)][rn(6)] = 0x10 + rn(M_SAMP); }   // a bind of something else (a compute dispatch, the UI)
            nrz_val got; through(&F, &T, &w[rn(W)], &got);
        }
        printf("  random round %d (front %u entries, full %u): front answered %ld (the full table held the key too: %ld, had lost it: %ld), full table answered %ld, stored %ld, keys too long %ld, events %ld; "
               "front: replaced %llu, cleared %llu, fixed-words sets %llu (id table full %llu)\n", round, F.sets * NRZK_WAYS, T.sets * NRZ_WAYS, fhits, both, front_only, thits, misses, longs, events,
               (unsigned long long)F.replaced, (unsigned long long)F.clears, (unsigned long long)F.fixed_new, (unsigned long long)F.fixed_full);
        CHECK(fhits > (round == 0 ? 500 : round == 1 ? 5000 : 200000)); CHECK(longs > 0); CHECK(F.clears > 50);
        if (round == 3) CHECK(front_only > 1000);
        nrzk_free(&F); nrz_free(&T);
    }
}

int main(void) {
    test_layout(); test_fixed(); test_table();
    model_init(); test_statement();
    model_init(); test_inputs();
    test_random();
    printf("nr_z3k_test%s: %ld checks, %ld failed -> %s\n", NRZK_MUTATE ? " (MUTATED)" : "", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
