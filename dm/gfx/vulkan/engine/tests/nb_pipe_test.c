// NB-13b host test: dm/gfx/nb/nb_pipe.h (DM_NB_PIPE state-group ids + the pipeline memo by them) + a rough micro benchmark.
// clang -std=c11 -O2 -Wall -Wextra dm/gfx/vulkan/engine/tests/nb_pipe_test.c -o nb_pipe_test && ./nb_pipe_test
// The model: a state struct whose pipeline-relevant bytes are six groups (two of them in two spans, like blend / colour / polygon /
// depth-stencil in State) + per-draw words (pass, shaders). The old memo is modelled as "full key bytes -> pipeline" (exact); the NB
// memo must only ever hit with the full key the entry was stored under, and return that entry's pipeline / bindings.
#include "../../../nb/nb_pipe.h"
#include <stdio.h>
#include <time.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static double now_ns(void) { struct timespec t; timespec_get(&t, TIME_UTC); return t.tv_sec * 1e9 + t.tv_nsec; }

// ---- model state: 6 groups over 9 fields (offsets fixed below), + bytes outside every group ----
typedef struct { uint8_t pre[16], a[40], b[24], gap[8], c[12], d[30], e[20], f[64], g[8], h[36], post[8]; } mstate;
#define MO(f) (uint16_t)offsetof(mstate, f)
#define ML(f) (uint16_t)sizeof(((mstate *)0)->f)
static nbp_span msp[NBP_GROUPS];
static void spans(void) {
    nbp_span z; memset(&z, 0, sizeof z);
    for (int k = 0; k < NBP_GROUPS; k++) msp[k] = z;
    msp[0].off[0] = MO(a); msp[0].len[0] = ML(a); msp[0].off[1] = MO(c); msp[0].len[1] = ML(c);   // two spans, like blend + blend_color
    msp[1].off[0] = MO(b); msp[1].len[0] = ML(b); msp[1].off[1] = MO(e); msp[1].len[1] = ML(e);
    msp[2].off[0] = MO(d); msp[2].len[0] = ML(d);
    msp[3].off[0] = MO(f); msp[3].len[0] = ML(f);
    msp[4].off[0] = MO(g); msp[4].len[0] = ML(g);                                                 // "vertex format": bindings depend on it
    msp[5].off[0] = MO(h); msp[5].len[0] = ML(h);
}
static const struct { uint16_t off, len; int g; } fields[] = {
    {MO(a), ML(a), 0}, {MO(c), ML(c), 0}, {MO(b), ML(b), 1}, {MO(e), ML(e), 1}, {MO(d), ML(d), 2}, {MO(f), ML(f), 3}, {MO(g), ML(g), 4}, {MO(h), ML(h), 5}};
enum { NFIELDS = sizeof fields / sizeof fields[0] };
enum { FULL = ML(a) + ML(c) + ML(b) + ML(e) + ML(d) + ML(f) + ML(g) + ML(h) + 8 };   // + 8: per-draw words
static void full_key(const mstate *s, uint32_t extra, uint8_t out[FULL]) {
    uint32_t at = 0; const uint8_t *b = (const uint8_t *)s;
    for (int i = 0; i < NFIELDS; i++) { memcpy(out + at, b + fields[i].off, fields[i].len); at += fields[i].len; }
    memset(out + at, 0, 8); memcpy(out + at, &extra, 4);
}
static void nb_key(const nbp_groups *g, uint32_t extra, nbp_key *k) {
    memset(k, 0, sizeof *k);
    k->w[0] = extra; k->w[1] = 0x55;
    for (int i = 0; i < NBP_GROUPS; i++) k->w[2 + i] = g->gid[i];
}

// ---- the old memo: full key -> pipeline (exact, linear probing); pipelines get new handles after an eviction ----
enum { OLD_N = 1 << 17 };   // > distinct full keys of the longest run (the probe loop needs a free slot)
static uint8_t old_k[OLD_N][FULL]; static uint64_t old_p[OLD_N]; static uint8_t old_used[OLD_N]; static uint64_t next_pipe = 1;
static uint64_t old_lookup(const uint8_t *k) {
    uint32_t s = (uint32_t)nbp_hash(k, FULL) & (OLD_N - 1);
    for (;; s = (s + 1) & (OLD_N - 1)) {
        if (!old_used[s]) { old_used[s] = 1; memcpy(old_k[s], k, FULL); old_p[s] = next_pipe++; return old_p[s]; }
        if (!memcmp(old_k[s], k, FULL)) { if (!old_p[s]) old_p[s] = next_pipe++; return old_p[s]; }
    }
}
static uint64_t old_evict_random(void) {   // destroy one pipeline: the next lookup of its key creates a new one
    for (int t = 0; t < 64; t++) { uint32_t s = (uint32_t)rnd() & (OLD_N - 1); if (old_used[s] && old_p[s]) { uint64_t p = old_p[s]; old_p[s] = 0; return p; } }
    return 0;
}
static uint16_t bind_of(const uint8_t *full, uint32_t extra) {   // "vertex bindings" = a function of the vertex format group + the shader
    const uint8_t *g4 = full + ML(a) + ML(c) + ML(b) + ML(e) + ML(d) + ML(f);
    return (uint16_t)(nbp_hash(g4, ML(g)) ^ (extra >> 4) * 0x9E37u);
}

static void unit_dict(void) {
    nbp_dict d; memset(&d, 0, sizeof d); d.size = 24; d.arena = (uint8_t *)malloc((size_t)NBP_IDS * 24);
    uint8_t x[24] = {1, 2, 3}, y[24] = {1, 2, 4};
    int f = 0;
    CHECK(nbp_dict_get(&d, x, &f) == 0 && f == 1);
    CHECK(nbp_dict_get(&d, y, &f) == 1 && f == 1);
    CHECK(nbp_dict_get(&d, x, &f) == 0 && f == 0);
    for (int i = 2; i < NBP_IDS; i++) { uint8_t z[24] = {0}; memcpy(z, &i, 4); z[23] = 9; CHECK(nbp_dict_get(&d, z, &f) == i && f == 1); }
    uint8_t w[24] = {7, 7, 7};
    CHECK(nbp_dict_get(&d, w, &f) == -1);                        // full
    CHECK(nbp_dict_get(&d, y, &f) == 1 && f == 0);               // still finds what it has
    nbp_dict_reset(&d);
    CHECK(nbp_dict_get(&d, w, &f) == 0 && f == 1);               // restarted: ids reused
    free(d.arena);
}
static void unit_groups(void) {
    static nbp_groups g; mstate s; memset(&s, 0, sizeof s);
    CHECK(nbp_groups_init(&g, msp) == 0);
    CHECK(g.dirty == (1u << NBP_GROUPS) - 1);
    CHECK(nbp_refresh(&g, (const uint8_t *)&s) == 0 && g.dirty == 0);
    const uint32_t id0 = g.gid[0];
    s.c[3] = 5; g.dirty |= 1;                                     // second span of group 0
    nbp_refresh(&g, (const uint8_t *)&s);
    CHECK(g.gid[0] != id0);
    s.c[3] = 0; g.dirty |= 1;
    nbp_refresh(&g, (const uint8_t *)&s);
    CHECK(g.gid[0] == id0);                                       // back to the old bytes = the old id
    const uint64_t same = g.st[NBP_S_SAME]; g.dirty |= 2;
    nbp_refresh(&g, (const uint8_t *)&s);
    CHECK(g.st[NBP_S_SAME] == same + 1);                          // marked but unchanged: no new id, no hash
    s.gap[0] = 1; s.pre[0] = 1;                                   // bytes outside every group
    CHECK(nbp_stale(&g, (const uint8_t *)&s) == 0);
    s.f[10] = 3;                                                  // group 3 written without a mark
    CHECK(nbp_stale(&g, (const uint8_t *)&s) == 1u << 3);
    g.dirty |= 1u << 3; CHECK(nbp_stale(&g, (const uint8_t *)&s) == 0);   // a dirty group is not stale
    nbp_groups_free(&g);
}
static void unit_memo(void) {
    static nbp_memo m; CHECK(nbp_memo_init(&m) == 0);
    nbp_key a, b; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); a.w[3] = 1; b.w[3] = 2;
    int cl, fr;
    CHECK(!nbp_find(&m, &a, nbp_key_hash(&a)));
    nbp_ent *e = nbp_put(&m, &a, nbp_key_hash(&a), &cl, &fr); CHECK(e && fr == 1 && cl == 0 && e->pipe == 0 && e->zero == -1);
    e->pipe = 77;
    CHECK(nbp_find(&m, &a, nbp_key_hash(&a)) == e);
    CHECK(!nbp_find(&m, &b, nbp_key_hash(&b)));
    nbp_kill(&m, 77); CHECK(e->pipe == 0 && nbp_find(&m, &a, nbp_key_hash(&a)) == e);   // dead, still found (revived by a store)
    CHECK(nbp_put(&m, &a, nbp_key_hash(&a), &cl, &fr) == e && fr == 0);
    nbp_memo_clear(&m); CHECK(!nbp_find(&m, &a, nbp_key_hash(&a)) && m.n == 0);
    for (int i = 0; i < NBP_ENTRIES; i++) { nbp_key k; memset(&k, 0, sizeof k); k.w[0] = (uint64_t)i + 1; nbp_put(&m, &k, nbp_key_hash(&k), &cl, &fr)->pipe = 5; CHECK(!cl); }
    CHECK(m.n == NBP_ENTRIES);
    nbp_put(&m, &b, nbp_key_hash(&b), &cl, &fr); CHECK(cl == 1 && fr == 1 && m.n == 1);   // full: cleared, then the new key
    nbp_memo_free(&m);
}

// One random run. miss_mark: a write leaves its group unmarked with this probability (a writer without the hook): must be caught.
typedef struct { uint64_t draws, hits, prev_hits, bad, stale, stored, killed, clears, restarts; } run_st;
static run_st run(int steps, int miss_mark_pct, int presets, int wide_group, int check_stale) {
    static nbp_groups g; static nbp_memo m; static uint8_t fk[NBP_ENTRIES][FULL];
    static mstate ps[NBP_GROUPS][16];
    run_st r; memset(&r, 0, sizeof r);
    memset(old_used, 0, sizeof old_used); memset(old_p, 0, sizeof old_p);
    mstate s; memset(&s, 0, sizeof s);
    CHECK(nbp_groups_init(&g, msp) == 0); CHECK(nbp_memo_init(&m) == 0);
    for (int k = 0; k < NBP_GROUPS; k++) for (int i = 0; i < 16; i++) for (size_t b = 0; b < sizeof(mstate); b++) ((uint8_t *)&ps[k][i])[b] = (uint8_t)(rnd() % 3);
    nbp_ent *prev = NULL;
    for (int step = 0; step < steps; step++) {
        const uint64_t x = rnd() % 100;
        if (x < 45) {   // state records: a "material" (every group from preset i), one group from a preset, one byte, or (wide) a new group 3
            uint8_t *b = (uint8_t *)&s; const uint64_t y = rnd() % 10;
            uint32_t touched = 0;
            if (wide_group && y < 3) { const int fi = 5; for (int i = 0; i < 8; i++) b[fields[fi].off + rnd() % fields[fi].len] = (uint8_t)rnd(); touched = 1u << 3; }   // many distinct contents
            else if (y < 6) { const int pi = (int)(rnd() % (unsigned)presets); for (int i = 0; i < NFIELDS; i++) memcpy(b + fields[i].off, (uint8_t *)&ps[fields[i].g][pi] + fields[i].off, fields[i].len); touched = (1u << NBP_GROUPS) - 1; }
            else if (y < 9) { const int gk = (int)(rnd() % NBP_GROUPS), pi = (int)(rnd() % (unsigned)presets);
                for (int i = 0; i < NFIELDS; i++) if (fields[i].g == gk) memcpy(b + fields[i].off, (uint8_t *)&ps[gk][pi] + fields[i].off, fields[i].len); touched = 1u << gk; }
            else { const int fi = (int)(rnd() % NFIELDS); b[fields[fi].off + rnd() % fields[fi].len] = (uint8_t)(rnd() % 3); touched = 1u << fields[fi].g; }
            for (int k = 0; k < NBP_GROUPS; k++) if ((touched >> k & 1) && (int)(rnd() % 100) >= miss_mark_pct) g.dirty |= 1u << k;
        } else if (x < 47) { ((uint8_t *)&s)[MO(gap) + rnd() % 8] = (uint8_t)rnd(); ((uint8_t *)&s)[MO(post) + rnd() % 8] = (uint8_t)rnd(); }   // not a group: no mark
        else if (x < 48) { const uint64_t p = old_evict_random(); if (p) { nbp_kill(&m, p); r.killed++; } }
        else if (x < 48 + (rnd() % 1000 == 0)) { nbp_memo_clear(&m); prev = NULL; r.clears++; }
        else {          // a draw
            const uint32_t extra = (uint32_t)(rnd() % 4) | (uint32_t)(rnd() % 6) << 4;   // pass, shader
            r.draws++;
            if (check_stale && nbp_stale(&g, (const uint8_t *)&s)) r.stale++;
            if (g.dirty && nbp_refresh(&g, (const uint8_t *)&s)) { nbp_memo_clear(&m); prev = NULL; r.restarts++; }
            nbp_key k; nb_key(&g, extra, &k);
            uint8_t full[FULL]; full_key(&s, extra, full);
            nbp_ent *e = NULL; uint64_t h = nbp_key_hash(&k);
            if (prev && !memcmp(&prev->k, &k, sizeof k)) { e = prev; r.prev_hits += prev->pipe != 0; } else e = nbp_find(&m, &k, h);
            const uint64_t want = old_lookup(full);
            if (e && e->pipe) {
                r.hits++;
                const size_t i = (size_t)(e - m.e);
                if (memcmp(fk[i], full, FULL) || e->pipe != want || e->used != bind_of(full, extra)) r.bad++;
                prev = e;
            } else {
                int cl, fr; e = nbp_put(&m, &k, h, &cl, &fr); if (cl) prev = NULL;
                e->pipe = want; e->used = bind_of(full, extra); memcpy(fk[e - m.e], full, FULL); prev = e; r.stored++;
            }
        }
    }
    nbp_groups_free(&g); nbp_memo_free(&m);
    return r;
}

static void bench(void) {
    // NB step on a hit: one dirty group re-read (unchanged: memcmp only), key fill, previous-entry compare / hash + find.
    // Old step: pipe_memo_fill (memset + 3 copies of ~480 bytes), memcmp with the previous key, on a miss FNV over the key + map compare.
    static nbp_groups g; static nbp_memo m; mstate s; memset(&s, 0, sizeof s);
    nbp_groups_init(&g, msp); nbp_memo_init(&m);
    enum { N = 2000000, KEYS = 64, OLDK = 480 };
    nbp_key ks[KEYS]; int cl, fr;
    nbp_refresh(&g, (const uint8_t *)&s);
    for (int i = 0; i < KEYS; i++) { nb_key(&g, (uint32_t)i, &ks[i]); nbp_put(&m, &ks[i], nbp_key_hash(&ks[i]), &cl, &fr)->pipe = (uint64_t)i + 1; }
    volatile uint64_t sink = 0; nbp_ent *prev = NULL;
    double t0 = now_ns();
    for (int i = 0; i < N; i++) {
        g.dirty |= 1u << (i & 3);
        nbp_refresh(&g, (const uint8_t *)&s);
        nbp_key k; nb_key(&g, (uint32_t)((i >> 2) & (KEYS - 1)), &k);
        nbp_ent *e = prev && !memcmp(&prev->k, &k, sizeof k) ? prev : nbp_find(&m, &k, nbp_key_hash(&k));
        prev = e; sink += e ? e->pipe : 0;
    }
    const double nb = (now_ns() - t0) / N;
    static uint8_t st[1024], kprev[OLDK], knext[OLDK], pod[KEYS][OLDK];
    for (int i = 0; i < KEYS; i++) { memset(pod[i], 0, OLDK); pod[i][0] = (uint8_t)i; }
    t0 = now_ns();
    for (int i = 0; i < N; i++) {
        st[0] = (uint8_t)((i >> 2) & (KEYS - 1));
        memset(knext, 0, OLDK); memcpy(knext, st, 96); memcpy(knext + 96, st + 100, 288); memcpy(knext + 384, st + 400, 96);
        if (memcmp(knext, kprev, OLDK)) {
            uint64_t h = 1469598103934665603ull, w; for (int j = 0; j + 8 <= OLDK; j += 8) { memcpy(&w, knext + j, 8); h = (h ^ w) * 1099511628211ull; }
            sink += h; const int slot = knext[0] & (KEYS - 1); sink += !memcmp(pod[slot], knext, OLDK);
            memcpy(kprev, knext, OLDK);
        }
    }
    const double old = (now_ns() - t0) / N;
    printf("bench (x86 host, rough): NB step %.1f ns a draw (1 group re-read, A/B/A/B keys) vs old memo step %.1f ns (fill + compare + POD hash / compare on change)\n", nb, old);
    (void)sink;
    nbp_groups_free(&g); nbp_memo_free(&m);
}

int main(void) {
    spans();
    unit_dict(); unit_groups(); unit_memo();
    uint64_t draws = 0, hits = 0;
    for (int t = 0; t < 40; t++) {
        run_st r = run(t % 5 == 4 ? 80000 : 20000, 0, 2 + t % 6, t % 5 == 4, t % 2);
        CHECK(r.bad == 0); CHECK(r.stale == 0);
        draws += r.draws; hits += r.hits;
        if (t < 3 || t % 5 == 4) printf("run %2d: %llu draws, %llu hits (%llu via the previous entry), stored %llu, killed %llu, clears %llu, dictionary restarts %llu, bad %llu\n", t,
            (unsigned long long)r.draws, (unsigned long long)r.hits, (unsigned long long)r.prev_hits, (unsigned long long)r.stored, (unsigned long long)r.killed,
            (unsigned long long)r.clears, (unsigned long long)r.restarts, (unsigned long long)r.bad);
        if (t % 5 == 4) CHECK(r.restarts > 0);   // the wide group overflows its dictionary: the memo is cleared, still no bad hit
    }
    printf("random runs: %llu draws, %llu NB hits (%.1f%%), 0 bad expected\n", (unsigned long long)draws, (unsigned long long)hits, 100.0 * (double)hits / (double)(draws ? draws : 1));
    // mutation: a writer that does not mark its group -> wrong hits, and the check mode's stale test sees it
    run_st mu = run(20000, 20, 3, 0, 1);
    printf("mutation (20%% of writes unmarked): bad hits %llu, stale groups seen %llu (both must be > 0)\n", (unsigned long long)mu.bad, (unsigned long long)mu.stale);
    CHECK(mu.bad > 0); CHECK(mu.stale > 0);
    bench();
    if (fails) { printf("FAILED: %d\n", fails); return 1; }
    printf("nb_pipe_test: all passed\n");
    return 0;
}
