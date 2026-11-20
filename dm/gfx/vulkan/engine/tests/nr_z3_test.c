// [DOS2] NR-ZERO Z3 host test (dm/gfx/nr/nr_z3.h; docs/dos2/nr/NR-ZERO.md §9): the key of a resolved engine draw, the table, the stamps,
// the classes of the DM_NR_Z3=2 comparison.
//   1  the key: every part is in it (changing one part alone changes the words), too many texture rows give no key, the hash is the
//      words' and never 0 in the table
//   2  the table: find / put / replace, two ways with the older one replaced, clear, a stale entry answers nothing and is replaced
//   3  invalidation by each input: a model of the three lookups a hit skips (NR-08's plan and entry ids - handed out again after a
//      flush -, NR-09's table - taught, re-taught, cleared -, objects destroyed with their addresses used again) answers every draw;
//      with the table in front of it, changing ONE input at a time never gives the answer of the draw before
//   4  model against table on a random stream of draws and events (4 M draws): every answer of the table is the model's
//   5  nrz_classify: the truth table of the phone comparison
// The mutation check: built with -DNRZ_MUTATE=k (k = 1..11: one key part left out / one stamp not compared, nr_z3.h) this test must FAIL.
// Build (Windows LLVM or any C11 compiler), from the repository root:
//   clang -std=c11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS dm/gfx/vulkan/engine/tests/nr_z3_test.c -o nr_z3_test && ./nr_z3_test
//   for k in 1 2 3 4 5 6 7 8 9 10 11; do clang -std=c11 -O2 -D_CRT_SECURE_NO_WARNINGS -DNRZ_MUTATE=$k dm/gfx/vulkan/engine/tests/nr_z3_test.c -o nr_z3_mut && ! ./nr_z3_mut >/dev/null || echo "mutation $k NOT caught"; done
#include "../../../nr/nr_z3.h"
#include <stdio.h>

static long fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { if (fails < 20) printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static uint64_t rs = 0x243F6A8885A308D3ull;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }

// ---------------- the model: what the path without the table resolves ----------------
enum { M_SETS = 24, M_TEX = 40, M_SAMP = 6, M_PASS = 6, M_KEYS = 48, M_D08 = 4096, M_D09 = 4096 };
typedef struct { uint64_t sh[5]; unsigned nimg; uint64_t plan, layout_def; } mset;          // a shader set: its objects, texture rows, NR-08's plan
typedef struct { unsigned set; uint64_t fixed[6], th[2 * (NRZ_IMG_MAX + 2)]; uint32_t key_id; uint64_t var4, var5; unsigned pass; } mdraw;   // what is bound at a draw
typedef struct { uint64_t w[5 + 6 + 2 * (NRZ_IMG_MAX + 2)]; unsigned n; int32_t id; } m08;   // NR-08: key -> entry id
typedef struct { uint64_t w[7]; uint64_t pipe; unsigned gen; } m09;                          // NR-09: (key id, vs, fs, layout, pass, var4, var5) -> an entry (its address is the answer; pipe: what it holds now)
static mset sets[M_SETS];
static uint64_t pass_def[M_PASS];
static m08 d08[M_D08]; static unsigned n08;
static m09 d09[M_D09]; static unsigned n09, gen09 = 1;   // gen09: the table's clears (an address of d09 is handed out again after one)
static uint64_t stamp[NRZ_STAMPS] = {1, 1, 1}, lane_epoch = 1;   // lane_epoch: the object epoch NR-09's table was last checked against (its lane clears it at its next draw)
static uint64_t next_addr = 0x7000000000ull, next_pipe = 0x1000;
static uint64_t addr(void) { next_addr += 0x40 + 8 * rn(8); return next_addr; }

static void model_init(void) {
    for (unsigned i = 0; i < M_SETS; i++) {
        memset(&sets[i], 0, sizeof sets[i]);
        sets[i].sh[0] = addr(); sets[i].sh[4] = rn(8) ? addr() : 0;
        sets[i].nimg = i < 3 ? i : i == 3 ? NRZ_IMG_MAX : i == 4 ? NRZ_IMG_MAX + 1 : rn(7);
        sets[i].plan = addr(); sets[i].layout_def = addr();
    }
    // two shader sets that differ in ONE object (the same vertex shader, another fragment shader; and a pair with the same fragment shader)
    sets[6].sh[0] = sets[5].sh[0]; sets[6].nimg = sets[5].nimg;
    sets[8].sh[4] = sets[7].sh[4]; sets[8].nimg = sets[7].nimg;
    for (unsigned i = 0; i < M_PASS; i++) pass_def[i] = addr();
    n08 = n09 = 0; ++gen09; stamp[0] = stamp[1] = stamp[2] = 1; lane_epoch = 1;
}
// NR-08's flush: every id is handed out again (the same id then names another key)
static void ev_flush(void) { n08 = 0; stamp[NRZ_ST_DESC]++; }
// NR-08's release: the plans are made again (at other addresses, or the same addresses for other shader sets)
static void ev_release(void) { n08 = 0; stamp[NRZ_ST_DESC]++; for (unsigned i = 0; i < M_SETS; i++) sets[i].plan = rn(2) ? addr() : sets[(i + 1) % M_SETS].plan; }
// NR-09: its table cleared (pipelines are made again: other handles), or one entry taught again by the old tail
static void ev_clear09(void) { n09 = 0; ++gen09; stamp[NRZ_ST_PIPE]++; }
static void ev_relearn(void) { if (!n09) return; m09 *e = &d09[rn(n09)]; e->pipe = next_pipe++; }   // in place: the entry is the same
// the backend's objects destroyed: a render pass definition's address now holds another definition; every pipeline is gone
static void ev_objects(void) { unsigned a = rn(M_PASS), b = rn(M_PASS); uint64_t t = pass_def[a]; pass_def[a] = pass_def[b]; pass_def[b] = t; stamp[NRZ_ST_OBJ]++; }

// The resolution of a draw by the model (always succeeds: an unknown key is added, as the old path teaches its tables).
static void model_resolve(const mdraw *d, nrz_val *out) {
    const mset *s = &sets[d->set];
    m08 k; memset(&k, 0, sizeof k);
    for (unsigned i = 0; i < 5; i++) k.w[k.n++] = s->sh[i];
    for (unsigned i = 0; i < 6; i++) k.w[k.n++] = d->fixed[i];
    for (unsigned i = 0; i < 2 * s->nimg; i++) k.w[k.n++] = d->th[i];
    int32_t id = -1;
    for (unsigned i = 0; i < n08 && id < 0; i++) if (d08[i].n == k.n && !memcmp(d08[i].w, k.w, k.n * 8)) id = d08[i].id;
    if (id < 0) { if (n08 == M_D08) ev_flush(); k.id = id = (int32_t)n08; d08[n08++] = k; }
    if (lane_epoch != stamp[NRZ_ST_OBJ]) { ev_clear09(); lane_epoch = stamp[NRZ_ST_OBJ]; }   // the lane: its table is of another object epoch
    const uint64_t pw[7] = {d->key_id, s->sh[0], s->sh[4], s->layout_def, pass_def[d->pass], d->var4, d->var5};
    m09 *p = NULL;
    for (unsigned i = 0; i < n09 && !p; i++) if (!memcmp(d09[i].w, pw, sizeof pw)) p = &d09[i];
    if (!p) { if (n09 == M_D09) ev_clear09(); p = &d09[n09++]; memcpy(p->w, pw, sizeof pw); p->pipe = next_pipe++; p->gen = gen09; }   // the old tail taught the table (no generation moves)
    memset(out, 0, sizeof *out);
    out->plan = s->plan; out->pent = (uint64_t)(uintptr_t)p; out->did = id; out->frag = (uint8_t)(d->set & 1);
}
static void draw_random(mdraw *d) {
    memset(d, 0, sizeof *d);
    d->set = rn(M_SETS);
    d->fixed[0] = (uint64_t)(rn(2) ? 1920 : 960) << 32 | (rn(2) ? 1080 : 540); d->fixed[1] = 0x5000 + 0x1000 * rn(2); d->fixed[2] = 256 * rn(2); d->fixed[3] = 1024 << rn(2);
    d->fixed[4] = 0x9000 + 0x1000 * rn(2); d->fixed[5] = 64 * rn(2);
    for (unsigned i = 0; i < NRZ_IMG_MAX + 2; i++) { d->th[2 * i] = 0x100 + rn(M_TEX); d->th[2 * i + 1] = rn(3) ? 0x10 + rn(M_SAMP) : 0; }
    d->key_id = 1 + rn(M_KEYS); d->var4 = (uint64_t)(4 + rn(2)) | (uint64_t)1 << 8 | (uint64_t)(1 + rn(3)) << 16 | (uint64_t)rn(2) << 30 | (uint64_t)(50 + rn(3)) << 32;
    d->var5 = (uint64_t)rn(6) | (uint64_t)rn(5) << 10 | (uint64_t)rn(4) << 30; d->pass = rn(M_PASS);
}
// One draw through the table in front of the model, as xbg_nr_z3.inc does it: the key from what is bound, the table asked with the
// current stamps; a hit must be the model's answer; a miss stores the model's answer under the stamps after the model ran.
static long hits = 0, misses = 0, longs = 0;
static int through(nrz_tab *t, const mdraw *d, nrz_val *got) {   // 1 = the table answered (*got), 0 = not
    const mset *s = &sets[d->set];
    uint64_t key[NRZ_KEY_MAX];
    const unsigned n = nrz_key(key, s->sh, d->fixed, d->th, s->nimg, d->key_id, d->var4, d->var5, pass_def[d->pass]);
    nrz_val truth;
    if (!n) { ++longs; model_resolve(d, &truth); return 0; }
    const uint64_t h = nrz_hash(key, n);
    int stale = 0;
    const nrz_val *v = nrz_find(t, h, key, n, stamp, &stale);
    if (v) { *got = *v; model_resolve(d, &truth); ++hits; CHECK(nrz_val_same(got, &truth));
        const m09 *pe = (const m09 *)(uintptr_t)got->pent; CHECK(pe >= d09 && pe < d09 + n09 && pe->gen == gen09 && pe->w[0] == d->key_id && pe->w[5] == d->var4 && pe->w[6] == d->var5 && pe->w[4] == pass_def[d->pass]);
        return 1; }
    model_resolve(d, &truth); ++misses;
    nrz_put(t, h, key, n, stamp, &truth);
    return 0;
}

// ---------------- 1: the key ----------------
static void test_key(void) {
    uint64_t sh[5] = {11, 0, 0, 0, 15}, fx[6] = {21, 22, 23, 24, 25, 26}, th[2 * NRZ_IMG_MAX + 4], a[NRZ_KEY_MAX], b[NRZ_KEY_MAX];
    for (unsigned i = 0; i < 2 * NRZ_IMG_MAX + 4; i++) th[i] = 100 + i;
    const unsigned n = nrz_key(a, sh, fx, th, 3, 7, 0x44, 0x55, 0x66);
    CHECK(n == 5 + 6 + 6 + 4);
    CHECK(nrz_key(b, sh, fx, th, NRZ_IMG_MAX, 7, 0x44, 0x55, 0x66) == NRZ_KEY_MAX);
    CHECK(nrz_key(b, sh, fx, th, NRZ_IMG_MAX + 1, 7, 0x44, 0x55, 0x66) == 0);
    CHECK(nrz_key(b, sh, fx, th, 0, 7, 0x44, 0x55, 0x66) == 15);
    // every part alone moves the key (and the hash)
#define MOVED(stmt, undo) do { stmt; const unsigned m = nrz_key(b, sh, fx, th, 3, kid, v4, v5, pd); CHECK(m == n && memcmp(a, b, n * 8) != 0); CHECK(nrz_hash(a, n) != nrz_hash(b, m)); undo; } while (0)
    uint32_t kid = 7; uint64_t v4 = 0x44, v5 = 0x55, pd = 0x66;
    for (unsigned i = 0; i < 5; i++) MOVED(sh[i] ^= 0x1000, sh[i] ^= 0x1000);
    for (unsigned i = 0; i < 6; i++) MOVED(fx[i] += 1, fx[i] -= 1);
    for (unsigned i = 0; i < 6; i++) MOVED(th[i] += 1, th[i] -= 1);
    MOVED(kid = 8, kid = 7); MOVED(v4 ^= 1ull << 30, v4 = 0x44); MOVED(v5 += 1024, v5 = 0x55); MOVED(pd += 8, pd = 0x66);
#undef MOVED
    CHECK(nrz_key(b, sh, fx, th, 3, 7, 0x44, 0x55, 0x66) == n && !memcmp(a, b, n * 8));
    // a texture row more is another key even when the words before are the same; a row beyond the plan's count is not read
    CHECK(nrz_key(b, sh, fx, th, 4, 7, 0x44, 0x55, 0x66) == n + 2);
    th[6] = 999; CHECK(nrz_key(b, sh, fx, th, 3, 7, 0x44, 0x55, 0x66) == n && !memcmp(a, b, n * 8));
    // the length is in the hash (a prefix does not collide by construction)
    CHECK(nrz_hash(a, n) != nrz_hash(a, n - 1));
}
// ---------------- 2: the table ----------------
static void test_table(void) {
    nrz_tab t; int stale;
    CHECK(nrz_init(&t, 3) == -1 && nrz_init(&t, 0) == -1 && nrz_init(&t, 2) == -1 && nrz_init(&t, 48) == -1);
    CHECK(nrz_init(&t, 8) == 0 && t.sets == 4);
    uint64_t st[NRZ_STAMPS] = {5, 6, 7}, k1[4] = {1, 2, 3, 4}, k2[4] = {1, 2, 3, 5}, k3[4] = {9, 2, 3, 4};
    nrz_val v1, v2, v3; memset(&v1, 0, sizeof v1); v1.plan = 0xa; v1.pent = 0x1; v1.did = 1; v2 = v1; v2.did = 2; v3 = v1; v3.did = 3;
    CHECK(!nrz_find(&t, 8, k1, 4, st, &stale) && !stale);
    nrz_put(&t, 8, k1, 4, st, &v1);
    const nrz_val *g = nrz_find(&t, 8, k1, 4, st, &stale); CHECK(g && g->did == 1);
    CHECK(!nrz_find(&t, 8, k1, 3, st, &stale) && !stale);                    // another length
    CHECK(!nrz_find(&t, 8, k2, 4, st, &stale) && !stale);                    // the same hash, another key
    nrz_put(&t, 8, k2, 4, st, &v2);                                          // the set's second way
    g = nrz_find(&t, 8, k2, 4, st, &stale); CHECK(g && g->did == 2);
    g = nrz_find(&t, 8, k1, 4, st, &stale); CHECK(g && g->did == 1);         // k1 used last
    nrz_put(&t, 8, k3, 4, st, &v3);                                          // a third key of the set: the one used longest ago (k2) goes
    CHECK(t.replaced == 1);
    g = nrz_find(&t, 8, k3, 4, st, &stale); CHECK(g && g->did == 3);
    g = nrz_find(&t, 8, k1, 4, st, &stale); CHECK(g && g->did == 1);
    CHECK(!nrz_find(&t, 8, k2, 4, st, &stale) && !stale);
    nrz_put(&t, 8, k1, 4, st, &v2);                                          // the same key again: replaced in place
    g = nrz_find(&t, 8, k1, 4, st, &stale); CHECK(g && g->did == 2);
    g = nrz_find(&t, 8, k3, 4, st, &stale); CHECK(g && g->did == 3);
    // each stamp alone makes the entry stale; a put under the new stamps answers again
    for (unsigned i = 0; i < NRZ_STAMPS; i++) {
        uint64_t s2[NRZ_STAMPS] = {5, 6, 7}; s2[i]++;
        CHECK(!nrz_find(&t, 8, k1, 4, s2, &stale) && stale == 1);
        nrz_put(&t, 8, k1, 4, s2, &v3);
        g = nrz_find(&t, 8, k1, 4, s2, &stale); CHECK(g && g->did == 3);
        CHECK(!nrz_find(&t, 8, k1, 4, st, &stale) && stale == 1);            // and the old stamps do not
        nrz_put(&t, 8, k1, 4, st, &v2);
    }
    // hash 0 is stored as 1 (0 marks an empty entry)
    nrz_put(&t, 0, k3, 4, st, &v3); g = nrz_find(&t, 0, k3, 4, st, &stale); CHECK(g && g->did == 3);
    // nothing answers after a clear; a key longer than the table holds is neither stored nor found
    nrz_clear(&t);
    CHECK(!nrz_find(&t, 8, k1, 4, st, &stale) && !stale && !nrz_find(&t, 0, k3, 4, st, &stale));
    uint64_t big[NRZ_KEY_MAX + 1] = {0};
    nrz_put(&t, 3, big, NRZ_KEY_MAX + 1, st, &v1); CHECK(!nrz_find(&t, 3, big, NRZ_KEY_MAX + 1, st, &stale));
    nrz_put(&t, 3, big, NRZ_KEY_MAX, st, &v1); CHECK(nrz_find(&t, 3, big, NRZ_KEY_MAX, st, &stale) != NULL);
    nrz_free(&t); CHECK(!t.e && !nrz_find(&t, 8, k1, 4, st, &stale));
    CHECK(sizeof(nrz_ent) % 8 == 0 && sizeof(nrz_ent) <= 512);
}
// ---------------- 3: one input at a time ----------------
// The draw is drawn (stored), drawn again (answered), then ONE input changes and it is drawn: the table must not answer with the first
// draw's resolution unless the model resolves the same (through() checks every answer against the model); and the first draw's
// resolution must differ from the changed draw's, so a key without that input would have answered wrongly.
static void test_inputs(void) {
    nrz_tab t; CHECK(nrz_init(&t, 4096) == 0);
    enum { IN_SET_VS, IN_SET_FS, IN_FIXED0, IN_FIXED5 = IN_FIXED0 + 5, IN_TEX, IN_SAMP, IN_TEX_LAST, IN_KEYID, IN_VAR4, IN_VAR5, IN_PASS,
           IN_FLUSH, IN_RELEASE, IN_RELEARN, IN_CLEAR09, IN_OBJECTS, IN_N };
    for (int in = 0; in < IN_N; in++) for (int rep = 0; rep < 40; rep++) {
        model_init(); nrz_clear(&t);
        mdraw d; draw_random(&d);
        d.set = in == IN_SET_VS ? 7 : in == IN_SET_FS ? 5 : 9 + rn(M_SETS - 9); if (sets[d.set].nimg < 2) sets[d.set].nimg = 2;
        if (sets[d.set].nimg > NRZ_IMG_MAX) sets[d.set].nimg = NRZ_IMG_MAX;
        sets[8].nimg = sets[7].nimg; sets[6].nimg = sets[5].nimg;
        nrz_val a, b, got;
        CHECK(through(&t, &d, &got) == 0);
        // another draw in between (NR-08 and NR-09 learn it: no entry of the table goes stale by that)
        mdraw o; draw_random(&o); o.set = 0; through(&t, &o, &got);
        CHECK(through(&t, &d, &got) == 1); a = got;
        mdraw e = d; int is_event = 0;
        switch (in) {
        case IN_SET_VS: e.set = 8; break;                                   // another vertex shader, the same fragment shader
        case IN_SET_FS: e.set = 6; break;                                   // the same vertex shader, another fragment shader
        case IN_TEX: e.th[0] += 1; break;
        case IN_SAMP: e.th[1] += 1; break;
        case IN_TEX_LAST: e.th[2 * (sets[d.set].nimg - 1)] += 1; break;
        case IN_KEYID: e.key_id += 1; break;
        case IN_VAR4: e.var4 ^= 1ull << (rep % 2 ? 30 : 29); break;         // the pre-pass override / primitive restart
        case IN_VAR5: e.var5 += 1ull << (10 * (rep % 6)); break;            // one state-group id
        case IN_PASS: e.pass = (e.pass + 1) % M_PASS; break;
        case IN_FLUSH: ev_flush(); is_event = 1; break;
        case IN_RELEASE: ev_release(); is_event = 1; break;
        case IN_RELEARN: {   // the old tail teaches entries again in place: the same entry answers, and it holds the new pipeline
            const m09 *pe = (const m09 *)(uintptr_t)a.pent; const uint64_t p0 = pe->pipe; while (pe->pipe == p0) ev_relearn();
            CHECK(through(&t, &d, &got) == 1 && nrz_val_same(&got, &a) && ((const m09 *)(uintptr_t)got.pent)->pipe != p0); continue; }
        case IN_CLEAR09: ev_clear09(); is_event = 1; break;
        case IN_OBJECTS: ev_objects(); is_event = 1; break;
        default: e.fixed[in - IN_FIXED0] += 1; break;
        }
        if (is_event && (in == IN_FLUSH || in == IN_RELEASE)) { mdraw q; draw_random(&q); q.set = 1; nrz_val x; model_resolve(&q, &x); }   // the id of the first draw is handed to another key first
        const int ans = through(&t, &e, &got);                               // (every answer is checked against the model inside)
        CHECK(ans == 0);                                                     // nothing can answer: the key is new / the entry is stale
        model_resolve(&e, &b);
        CHECK(!nrz_val_same(&a, &b) || ((const m09 *)(uintptr_t)a.pent)->gen != gen09 - 0u || in == IN_CLEAR09 || in == IN_OBJECTS);   // and the input matters: the first draw's resolution would be wrong (after a clear the same address may be handed out again: the generation says so)
        if (!is_event) { CHECK(through(&t, &d, &got) == 1 && nrz_val_same(&got, &a)); CHECK(through(&t, &e, &got) == 1 && nrz_val_same(&got, &b)); }   // both keys live side by side
        else CHECK(through(&t, &e, &got) == 1 && nrz_val_same(&got, &b));    // stored again under the new stamps
    }
    nrz_free(&t);
}
// ---------------- 4: random stream ----------------
static void test_random(void) {
    for (int round = 0; round < 4; round++) {
        nrz_tab t; CHECK(nrz_init(&t, round == 0 ? 256 : round == 1 ? 1024 : 4096) == 0);
        model_init(); hits = misses = longs = 0;
        // a working set of materials (as a frame: the same draws again and again), slowly replaced
        enum { W = 600 }; static mdraw w[W];
        for (int i = 0; i < W; i++) draw_random(&w[i]);
        long events = 0;
        for (long i = 0; i < 1000000; i++) {
            const unsigned r = rn(100000);
            if (r < 40) { ev_flush(); ++events; } else if (r < 44) { ev_release(); ++events; } else if (r < 90) { ev_relearn(); ++events; }
            else if (r < 96) { ev_clear09(); ++events; } else if (r < 102) { ev_objects(); ++events; }
            else if (r < 2000) draw_random(&w[rn(W)]);
            else if (r < 3000) { mdraw *d = &w[rn(W)]; switch (rn(7)) {   // one input of a material moves
                case 0: d->th[2 * rn(4)] = 0x100 + rn(M_TEX); break; case 1: d->th[2 * rn(4) + 1] = 0x10 + rn(M_SAMP); break; case 2: d->fixed[rn(6)] ^= 1ull << rn(12); break;
                case 3: d->key_id = 1 + rn(M_KEYS); break; case 4: d->var4 ^= 1ull << (28 + rn(3)); break; case 5: d->var5 ^= 1ull << rn(40); break; default: d->pass = rn(M_PASS); } }
            nrz_val got; through(&t, &w[rn(W)], &got);
        }
        printf("  random round %d (%u entries): %ld answered, %ld stored, %ld keys too long, %ld events; table hits %llu stale %llu replaced %llu\n", round, t.sets * NRZ_WAYS, hits, misses, longs, events,
               (unsigned long long)t.hits, (unsigned long long)t.stale, (unsigned long long)t.replaced);
        CHECK(hits > 300000); CHECK(longs > 0); CHECK(t.stale > 0);
        nrz_free(&t);
    }
}
// ---------------- 5: the comparison's classes ----------------
static void test_classify(void) {
    for (int d = 0; d <= NRZD_FELL_GATE; d++) for (int img = 0; img < 2; img++) for (int pf = 0; pf < 2; pf++) for (int l = 0; l < 4; l++) for (int sv = 0; sv < 2; sv++)
        CHECK(nrz_classify(0, d, img, pf, l, sv) == NRZC_NOT_PREDICTED);
    CHECK(nrz_classify(1, NRZD_HIT, 0, 1, 0, 1) == NRZC_SAME);
    CHECK(nrz_classify(1, NRZD_HIT, 2, 1, 0, 1) == NRZC_SAME);              // images on the read path, the pass went on
    CHECK(nrz_classify(1, NRZD_HIT, 0, 1, 0, 0) == NRZC_BAD);               // another plan / entry / pipeline
    CHECK(nrz_classify(1, NRZD_HIT, 0, 1, 2, 0) == NRZC_LIVE);              // the lane's tail (the same code after the table's answer)
    CHECK(nrz_classify(1, NRZD_HIT, 0, 1, 1, 0) == NRZC_BAD);               // the lane left before its pipeline: a gate / id / table the key must cover
    CHECK(nrz_classify(1, NRZD_HIT, 0, 1, 3, 0) == NRZC_BAD);               // the lane was not asked
    CHECK(nrz_classify(1, NRZD_HIT, 0, 0, 0, 1) == NRZC_BAD);               // create_pass did not continue the pass and no image read can explain it
    CHECK(nrz_classify(1, NRZD_HIT, 1, 0, 0, 1) == NRZC_LIVE);              // an image read ended the pass
    CHECK(nrz_classify(1, NRZD_FELL_LIVE, 0, 1, 2, 0) == NRZC_LIVE);        // a block / a set
    CHECK(nrz_classify(1, NRZD_BUILT, 0, 1, 0, 1) == NRZC_BAD);             // NR-08 made / remade the entry the table called ready
    CHECK(nrz_classify(1, NRZD_FELL_GATE, 0, 1, 0, 1) == NRZC_BAD);
    CHECK(nrz_classify(1, NRZD_NONE, 0, 1, 0, 1) == NRZC_BAD);
    for (int w = 0; w < NRZW_N; w++) CHECK(nrz_why(w) && nrz_why(w)[0] && nrz_why(w)[0] != '?');
    CHECK(nrz_why(NRZW_N)[0] == '?');
}

int main(void) {
    test_key(); test_table(); test_classify();
    model_init(); test_inputs();
    test_random();
    printf("nr_z3_test%s: %ld checks, %ld failed -> %s\n", NRZ_MUTATE ? " (MUTATED)" : "", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
