// [DOS2] NR-SHCACHE host test (docs/dos2/nr/NR-SHCACHE.md): the decisions of dm/gfx/nr/nr_shc.h driven exactly as the backend drives
// them (xbg_nr_shc.inc: arm at the first draw, nrshc_draw per draw, nrshc_end, execution 2 by ordinal), on a model of the depth test:
// a 64x64 D16 tile, each draw = a set of fragments (pixel, z) with LESS or LEQUAL + write. Every frame the tile the cache produces must
// equal the tile of drawing every draw into a cleared tile, bit for bit, in every scenario (static scene, moving casters, a static
// caster that disappears, a moved camera, a tainted draw, a changed clear value, the age limit, duplicates, shuffled draw order).
// Build: clang -std=c11 -O2 -Wall -Wextra nr_shc_test.c   (no other file)
#include "../../../nr/nr_shc.h"
#include <stdio.h>

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; if (fails < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

enum { W = 64, H = 64, MAXD = 600 };
typedef struct { uint64_t id; int x0, y0, x1, y1; uint16_t z0, dz; int op; int order_free; } Draw;   // op 1 LESS, 3 LEQUAL
static uint64_t rng = 0x1234567;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

static void raster(uint16_t *t, const Draw *d) {   // a sloped rectangle: z = z0 + dz * (x - x0) / 8 (deterministic per draw)
    for (int y = d->y0; y < d->y1; y++) for (int x = d->x0; x < d->x1; x++) {
        const uint16_t z = (uint16_t)(d->z0 + (uint16_t)(d->dz * (x - d->x0) / 8));
        uint16_t *s = &t[y * W + x];
        if (d->op == 1 ? z < *s : z <= *s) *s = z;
    }
}
static uint64_t dhash(const Draw *d) {   // what the backend hashes: every input of the draw (here: its parameters)
    uint64_t h = nrshc_mix(0x5348, d->id);
    const int32_t v[7] = {d->x0, d->y0, d->x1, d->y1, d->z0, d->dz, d->op};
    return nrshc_bytes(h, v, sizeof v);
}
static uint64_t shash(const Draw *d) {   // [shc2] the structure hash: everything but z0 (z0 stands for the uniform contents: the cascade matrix)
    uint64_t h = nrshc_mix(0x5348, d->id);
    const int32_t v[6] = {d->x0, d->y0, d->x1, d->y1, d->dz, d->op};
    return nrshc_bytes(h, v, sizeof v);
}
static Draw mk(uint64_t id) {
    Draw d; d.id = id; d.x0 = (int)(rnd() % (W - 4)); d.y0 = (int)(rnd() % (H - 4)); d.x1 = d.x0 + 1 + (int)(rnd() % (unsigned)(W - d.x0));
    d.y1 = d.y0 + 1 + (int)(rnd() % (unsigned)(H - d.y0)); d.z0 = (uint16_t)(rnd() & 0xffff); d.dz = (uint16_t)(rnd() % 64); d.op = (rnd() & 1) ? 1 : 3; d.order_free = 1;
    return d;
}

// the backend's execution of one range (the same calls in the same order as xbg_nr_shc.inc)
typedef struct { uint16_t S[W * H]; int have_s; } Layer;
typedef struct { uint64_t hits, builds, recovers, normals, skipped, drawn, frames, rests, blind, known, new_ubo, new_other, unc; } Tally;
static int run_range(nrshc_cas *c, const nrshc_par *p, nrshc_range *r, Layer *L, const Draw *ds, int n, uint16_t clear, int dirty, int poison,
                     uint16_t *tile, Tally *t) {
    for (int i = 0; i < W * H; i++) tile[i] = clear;   // the frame's full clear of the atlas
    const int32_t rect[4] = {0, 0, W, H};
    int why = 0;
    const int a = nrshc_arm(c, p, 0xA7A5, rect, 1, clear, dirty, 1, 1, &why);
    nrshc_begin(c, p, r, a, why);
    if (a == NRSHC_A_HIT) { CHECK(L->have_s, "HIT without an image"); if (!poison) memcpy(tile, L->S, sizeof L->S); ++t->hits; }
    else if (a == NRSHC_A_BUILD) ++t->builds; else ++t->normals;
    if (r->blind) { ++t->blind; for (int i = 0; i < n; i++) { raster(tile, &ds[i]); ++t->drawn; } }   // not hashed: nrshc_draw is not called
    else for (int i = 0; i < n; i++) { const int skip = nrshc_draw(c, p, r, dhash(&ds[i]), shash(&ds[i]), dhash(&ds[i]), 0, 0, ds[i].order_free); if (skip) ++t->skipped; else { raster(tile, &ds[i]); ++t->drawn; } }
    t->known += r->known; t->new_ubo += r->new_ubo; t->new_other += r->new_other; t->unc += r->unc;
    const int e = nrshc_end(c, p, r, 0xA7A5, rect, clear, 1);
    if (e == NRSHC_E_BUILD || e == NRSHC_E_REST) {
        if (e == NRSHC_E_BUILD) { memcpy(L->S, tile, sizeof L->S); L->have_s = 1; } else ++t->rests;
        for (int i = 0; i < n; i++) { int bad = 0; if (nrshc_again(r, NRSHC_R_REST, (uint32_t)i, dhash(&ds[i]), &bad)) { raster(tile, &ds[i]); ++t->drawn; } CHECK(!bad, "desync"); }
    } else if (e == NRSHC_E_RECOVER) {
        ++t->recovers; L->have_s = 0;
        for (int i = 0; i < W * H; i++) tile[i] = clear;
        for (int i = 0; i < n; i++) { int bad = 0; if (nrshc_again(r, NRSHC_R_ALL, (uint32_t)i, dhash(&ds[i]), &bad)) raster(tile, &ds[i]); CHECK(!bad, "desync (all)"); }
    }
    CHECK(e >= 0, "a range that cannot be executed again");
    ++t->frames;
    return a;
}
static void reference(const Draw *ds, int n, uint16_t clear, uint16_t *tile) { for (int i = 0; i < W * H; i++) tile[i] = clear; for (int i = 0; i < n; i++) raster(tile, &ds[i]); }
static void shuffle(Draw *ds, int n) { for (int i = n - 1; i > 0; i--) { int j = (int)(rnd() % (unsigned)(i + 1)); Draw t = ds[i]; ds[i] = ds[j]; ds[j] = t; } }

// A scenario: nstatic static casters + ndyn casters that change every frame, frames frames; events by frame.
typedef struct { const char *name; int nstatic, ndyn, frames, shuffle, vanish_at, camera_at, taint_at, clear_at, dup, move_until; } Scn;
static void scenario(const Scn *s, nrshc_par p, Tally *out) {
    nrshc_cas *c = nrshc_cas_new(); nrshc_range *r = (nrshc_range *)calloc(1, sizeof *r); Layer *L = (Layer *)calloc(1, sizeof *L);
    static Draw st[MAXD], ds[MAXD]; static uint16_t tile[W * H], ref[W * H];
    for (int i = 0; i < s->nstatic; i++) st[i] = mk(1000 + (uint64_t)i);
    if (s->dup) for (int i = 1; i < s->nstatic; i += 7) st[i] = st[i - 1];   // identical draws twice (same hash)
    int nst = s->nstatic; uint16_t clear = 0xffff; Tally t = {0};
    for (int f = 0; f < s->frames; f++) {
        if (f == s->vanish_at && nst > 3) { st[nst / 2] = st[nst - 1]; --nst; }   // a static caster leaves (culled / destroyed)
        if (f == s->camera_at) for (int i = 0; i < nst; i++) st[i].z0 = (uint16_t)(st[i].z0 + 37);   // the cascade matrix moved: every hash changes
        if (f < s->move_until) for (int i = 0; i < nst; i++) st[i].z0 = (uint16_t)(st[i].z0 + 1);   // a moving camera: new hashes every frame
        if (f == s->clear_at) clear = 0xfff0;
        int n = 0;
        for (int i = 0; i < nst; i++) ds[n++] = st[i];
        for (int i = 0; i < s->ndyn; i++) ds[n++] = mk(500000 + (uint64_t)f * 1000 + (uint64_t)i);   // animated: new every frame
        if (f == s->taint_at && n) ds[n / 3].order_free = 0;
        if (s->shuffle) shuffle(ds, n);
        run_range(c, &p, r, L, ds, n, clear, 0, 0, tile, &t);
        reference(ds, n, clear, ref);
        CHECK(!memcmp(tile, ref, sizeof tile), "%s frame %d: cached tile differs from the full render", s->name, f);
        if (f == s->taint_at) for (int i = 0; i < n; i++) ds[i].order_free = 1;
    }
    *out = t; free(c); free(r); free(L);
}

static void test_tab(void) {
    nrshc_cas *c = nrshc_cas_new(); nrshc_tab *t = &c->mem;
    nrshc_tab_clear(t);
    for (uint64_t k = 1; k <= 3000; k++) { int i = nrshc_tab_at(t, k * 0x10001, 1); CHECK(i >= 0, "insert %llu", (unsigned long long)k); t->val[i] += (uint32_t)(k % 3 + 1); }
    CHECK(t->n == 3000, "n %u", t->n);
    CHECK(nrshc_tab_get(t, 5 * 0x10001) == 5 % 3 + 1, "get");
    CHECK(nrshc_tab_get(t, 77) == 0, "missing");
    nrshc_tab_unuse(t);
    CHECK(nrshc_tab_take(t, 4 * 0x10001) && nrshc_tab_take(t, 4 * 0x10001) && !nrshc_tab_take(t, 4 * 0x10001), "take = count (2)");
    nrshc_tab_unuse(t);
    CHECK(nrshc_tab_take(t, 4 * 0x10001), "unuse resets");
    nrshc_tab_clear(t); CHECK(t->n == 0 && nrshc_tab_get(t, 5 * 0x10001) == 0, "clear");
    for (uint64_t k = 1; k <= NRSHC_TAB; k++) (void)nrshc_tab_at(t, k, 1);
    CHECK(t->n == NRSHC_TAB / 2, "half full at most (%u)", t->n);
    CHECK(nrshc_tab_at(t, 0xdeadbeefull, 1) < 0, "full table refuses");
    free(c);
    // the content hash: every byte and the length matter
    uint8_t b[300]; for (int i = 0; i < 300; i++) b[i] = (uint8_t)i;
    const uint64_t h0 = nrshc_bytes(1, b, 300);
    int all = 1; for (int i = 0; i < 300; i += 13) { b[i] ^= 1; if (nrshc_bytes(1, b, 300) == h0) all = 0; b[i] ^= 1; }
    CHECK(all, "a flipped byte changes the hash");
    CHECK(nrshc_bytes(1, b, 299) != h0 && nrshc_bytes(2, b, 300) != h0 && nrshc_bytes(1, b, 300) == h0, "length / seed / stable");
}

static void test_arm(void) {
    nrshc_cas *c = nrshc_cas_new(); nrshc_par p = nrshc_par_default(); int why;
    const int32_t rect[4] = {0, 0, 768, 768}, rect2[4] = {768, 0, 768, 768};
    CHECK(nrshc_arm(c, &p, 1, rect, 1, 7, 0, 1, 1, &why) == NRSHC_A_NORMAL && why == NRSHC_W_FIRST, "first");
    c->ranges = 5; c->prev_ok = 1; c->next_cand = 100;
    CHECK(nrshc_arm(c, &p, 1, rect, 1, 7, 0, 1, 0, &why) == NRSHC_A_NORMAL && why == NRSHC_W_OFF, "backend refused");
    CHECK(nrshc_arm(c, &p, 1, rect, 1, 7, 0, 0, 1, &why) == NRSHC_A_NORMAL && why == NRSHC_W_RECT, "rect");
    CHECK(nrshc_arm(c, &p, 1, rect, 1, 7, 1, 1, 1, &why) == NRSHC_A_NORMAL && why == NRSHC_W_DIRTY, "dirty");
    CHECK(nrshc_arm(c, &p, 1, rect, 0, 7, 0, 1, 1, &why) == NRSHC_A_NORMAL && why == NRSHC_W_CLEAR, "clear");
    c->prev_ok = 0; CHECK(nrshc_arm(c, &p, 1, rect, 1, 7, 0, 1, 1, &why) == NRSHC_A_NORMAL && why == NRSHC_W_PREV, "prev"); c->prev_ok = 1;
    c->next_cand = 10; CHECK(nrshc_arm(c, &p, 1, rect, 1, 7, 0, 1, 1, &why) == NRSHC_A_NORMAL && why == NRSHC_W_FEW, "few"); c->next_cand = 100;
    CHECK(nrshc_arm(c, &p, 1, rect, 1, 7, 0, 1, 1, &why) == NRSHC_A_BUILD, "build");
    c->layer = 1; c->atlas = 1; memcpy(c->rect, rect, sizeof rect); c->clear_bits = 7; c->layer_ranges = 0;
    CHECK(nrshc_arm(c, &p, 1, rect, 1, 7, 0, 1, 1, &why) == NRSHC_A_HIT, "hit");
    CHECK(nrshc_arm(c, &p, 1, rect2, 1, 7, 0, 1, 1, &why) == NRSHC_A_BUILD && why == NRSHC_W_LAYER && !c->layer, "other tile: layer dropped, build");
    c->layer = 1; CHECK(nrshc_arm(c, &p, 1, rect, 1, 8, 0, 1, 1, &why) == NRSHC_A_BUILD && why == NRSHC_W_LAYER, "other clear value");
    c->layer = 1; c->layer_ranges = p.max_age; CHECK(nrshc_arm(c, &p, 1, rect, 1, 7, 0, 1, 1, &why) == NRSHC_A_BUILD && why == NRSHC_W_AGE, "age");
    c->layer = 1; c->layer_ranges = 0; c->next_cand = 0; CHECK(nrshc_arm(c, &p, 2, rect, 1, 7, 0, 1, 1, &why) == NRSHC_A_NORMAL && why == NRSHC_W_LAYER, "other atlas, few");
    free(c);
}

// [shc2] the refusal at the first draw: DOS2's atlas (SetRenderTargets(1, {NULL}, D16 atlas): the caller counts bound colours = 0) is
// accepted; the scene pre-pass (D24S8, the occlusion pairs) is refused by its stencil aspect; every other reason in order.
static void test_shape(void) {
    nrshc_shape s = {1, 1, 1, 1, 1, 1, 1, 1536, 1536, 0, 0, 0};
    CHECK(nrshc_shape_why(&s) == NRSHC_RF_OK, "DOS2 atlas (rt.n 1, colour NULL -> 0 bound colours)");
    nrshc_shape t = s; t.has_depth = 0; CHECK(nrshc_shape_why(&t) == NRSHC_RF_NODEPTH, "no depth");
    t = s; t.same_image = 0; CHECK(nrshc_shape_why(&t) == NRSHC_RF_IMAGE, "image");
    t = s; t.depth_aspect_only = 0; t.width = 2160; t.height = 972; CHECK(nrshc_shape_why(&t) == NRSHC_RF_ASPECT, "D24S8 pre-pass: stencil aspect");
    t = s; t.fixed_point = 0; CHECK(nrshc_shape_why(&t) == NRSHC_RF_FORMAT, "float depth");
    t = s; t.levels = 2; CHECK(nrshc_shape_why(&t) == NRSHC_RF_SHAPE, "levels");
    t = s; t.layers = 6; CHECK(nrshc_shape_why(&t) == NRSHC_RF_SHAPE, "layers");
    t = s; t.samples = 4; CHECK(nrshc_shape_why(&t) == NRSHC_RF_SHAPE, "samples");
    t = s; t.width = 80; t.height = 100; CHECK(nrshc_shape_why(&t) == NRSHC_RF_SMALL, "portrait size");
    t = s; t.colours = 1; CHECK(nrshc_shape_why(&t) == NRSHC_RF_COLOUR, "a bound colour target");
    t = s; t.other_atlas = 1; CHECK(nrshc_shape_why(&t) == NRSHC_RF_OTHER, "other atlas");
    t = s; t.arms = NRSHC_CAS; CHECK(nrshc_shape_why(&t) == NRSHC_RF_SLOTS, "cascades");
    t = s; t.arms = NRSHC_CAS - 1; CHECK(nrshc_shape_why(&t) == NRSHC_RF_OK, "last slot");
    int names = 1; for (int i = 0; i < NRSHC_RF_N; i++) if (!strcmp(nrshc_rf_name(i), "?")) names = 0;
    for (int i = 0; i < NRSHC_T_N; i++) if (!strcmp(nrshc_t_name(i), "?")) names = 0;
    CHECK(names && !strcmp(nrshc_rf_name(NRSHC_RF_N), "?"), "every reason has a name");
    // the uniform row diff: which vec4s moved
    uint8_t a[1100], b[1100]; for (int i = 0; i < 1100; i++) a[i] = b[i] = (uint8_t)i;
    uint64_t m; uint32_t beyond;
    CHECK(nrshc_rows(a, b, 1100, &m, &beyond) == 0 && m == 0 && beyond == 0, "equal");
    b[64] ^= 1; b[79] ^= 2; b[80] ^= 1; b[1030] ^= 1; b[1099] ^= 1;
    CHECK(nrshc_rows(a, b, 1100, &m, &beyond) == 4 && m == (3ull << 4) && beyond == 2, "rows 4, 5 and two past 1 KB (%llx %u)", (unsigned long long)m, beyond);
    CHECK(nrshc_rows(a, b, 70, &m, &beyond) == 1 && m == (1ull << 4), "a short tail row");
    // the slot by tile: cascades 0..2 in slots 0..2; a frame where cascade 0 is no range keeps cascades 1 and 2 in their slots
    int32_t rects[NRSHC_CAS][4] = {{0}}; int has[NRSHC_CAS] = {0}; uint32_t used = 0;
    const int32_t t0[4] = {0, 0, 768, 768}, t1[4] = {768, 0, 768, 768}, t2[4] = {0, 768, 768, 768}, t9[4] = {768, 768, 768, 768};
    const int32_t *ts[3] = {t0, t1, t2};
    for (int i = 0; i < 3; i++) { const int k = nrshc_pick_slot(rects, has, used, ts[i]); CHECK(k == i, "first frame slot %d (%d)", i, k); used |= 1u << k; memcpy(rects[k], ts[i], sizeof rects[k]); has[k] = 1; }
    used = 0; CHECK(nrshc_pick_slot(rects, has, used, t1) == 1, "cascade 1 keeps slot 1 without cascade 0"); used |= 2;
    CHECK(nrshc_pick_slot(rects, has, used, t2) == 2, "cascade 2 keeps slot 2"); used |= 4;
    CHECK(nrshc_pick_slot(rects, has, used, t9) == 3, "a new tile takes the unused slot"); used |= 8; memcpy(rects[3], t9, sizeof rects[3]); has[3] = 1;
    CHECK(nrshc_pick_slot(rects, has, used, t0) == 0, "the last free slot"); used |= 1;
    CHECK(nrshc_pick_slot(rects, has, used, t0) == -1, "no free slot");
    used = 1 | 2 | 4 | 8; used &= ~4u; CHECK(nrshc_pick_slot(rects, has, used, t0) == 2, "another tile's slot when nothing else is free");
}
// [shc2] the stability counters: a still scene is all "known"; a moved cascade matrix (z0, the uniform bytes) shows as "new by uniform
// contents only" for every static draw; animated draws are "new otherwise"; the first range is not compared.
static void test_stability(nrshc_par p) {
    nrshc_cas *c = nrshc_cas_new(); nrshc_range *r = (nrshc_range *)calloc(1, sizeof *r); Layer *L = (Layer *)calloc(1, sizeof *L);
    static Draw st[MAXD], ds[MAXD]; static uint16_t tile[W * H], ref[W * H];
    for (int i = 0; i < 150; i++) st[i] = mk(9000 + (uint64_t)i);
    Tally t0 = {0}, t1 = {0}, t2 = {0};
    for (int f = 0; f < 12; f++) {
        if (f == 8) for (int i = 0; i < 150; i++) st[i].z0 = (uint16_t)(st[i].z0 + 5);   // the cascade moved
        int n = 0; for (int i = 0; i < 150; i++) ds[n++] = st[i];
        for (int i = 0; i < 20; i++) ds[n++] = mk(800000 + (uint64_t)f * 100 + (uint64_t)i);
        shuffle(ds, n);
        Tally *t = f == 0 ? &t0 : f == 8 ? &t2 : &t1;
        run_range(c, &p, r, L, ds, n, 0xffff, 0, 0, tile, t);
        reference(ds, n, 0xffff, ref);
        CHECK(!memcmp(tile, ref, sizeof tile), "stability frame %d: exact", f);
    }
    CHECK(t0.unc == 170 && t0.known == 0, "first range not compared (%llu)", (unsigned long long)t0.unc);
    CHECK(t1.known == 150 * 10 && t1.new_other == 20 * 10 && t1.new_ubo == 0, "still: known %llu other %llu ubo %llu", (unsigned long long)t1.known,
          (unsigned long long)t1.new_other, (unsigned long long)t1.new_ubo);
    CHECK(t2.new_ubo == 150 && t2.new_other == 20 && t2.known == 0, "moved: by uniform contents %llu, other %llu", (unsigned long long)t2.new_ubo, (unsigned long long)t2.new_other);
    free(c); free(r); free(L);
}

// [shc3] uniform updates inside a range (OP_UPDATE_UB writes memory when its record runs). A range is a list of records: an update writes
// a 2-byte value into slot s of a uniform memory U, a draw reads its z0 from U[slot] (its hash covers those bytes, as the backend's ubo()).
// Execution 2 without the undo reads U as execution 1 LEFT it: every draw before an update of its slot hashes differently (a desync) and
// rasterizes with the wrong z0. With the undo (old bytes noted before each update, written back in reverse before execution 2) every hash
// matches and the tile is exact every frame; memory after execution 2 equals memory after execution 1. A full log refuses execution 2.
typedef struct { int upd, slot; uint16_t val; Draw d; } Rec;
enum { USLOTS = 16 };
static uint64_t uhash(const Draw *d, const uint16_t *U, int slot) { Draw x = *d; x.z0 = U[slot]; return dhash(&x); }
static void uraster(uint16_t *tile, const Draw *d, const uint16_t *U, int slot) { Draw x = *d; x.z0 = U[slot]; raster(tile, &x); }
static void exec_recs(const Rec *rs, int n, uint16_t *U, nrshc_undo *u) {   // the updates of a range run (execution 1 notes them)
    for (int i = 0; i < n; i++) if (rs[i].upd) { if (u) (void)nrshc_undo_note(u, (uint64_t)rs[i].slot * 2, &U[rs[i].slot], 2); U[rs[i].slot] = rs[i].val; }
}
static void test_updates(nrshc_par p) {
    nrshc_cas *c = nrshc_cas_new(); nrshc_range *r = (nrshc_range *)calloc(1, sizeof *r); nrshc_undo *u = (nrshc_undo *)calloc(1, sizeof *u);
    static Draw st[MAXD]; static Rec rs[2 * MAXD]; static uint16_t tile[W * H], ref[W * H], S[W * H]; int have_s = 0;
    uint16_t U[USLOTS] = {0};
    for (int i = 0; i < 120; i++) st[i] = mk(30000 + (uint64_t)i);
    int hits = 0, builds = 0, again_runs = 0, bad_total = 0, exact = 1;
    for (int f = 0; f < 40; f++) {
        // before the range (outside it: not undone, not executed again) the engine sets every slot to X; inside the range draws 0-59 read X,
        // then every slot is updated to Y (twice: Y' then Y, a repeated address), and draws 60-119 read Y. Execution 1 leaves Y in memory.
        for (int s2 = 0; s2 < USLOTS; s2++) U[s2] = (uint16_t)(1000 + 37 * s2);
        int n = 0;
        for (int i = 0; i < 60; i++) { rs[n].upd = 0; rs[n].slot = i % USLOTS; rs[n].d = st[i]; ++n; }
        for (int s2 = 0; s2 < USLOTS; s2++) { rs[n].upd = 1; rs[n].slot = s2; rs[n].val = (uint16_t)(7 * s2); ++n; }
        for (int s2 = 0; s2 < USLOTS; s2++) { rs[n].upd = 1; rs[n].slot = s2; rs[n].val = (uint16_t)(20000 + 91 * s2); ++n; }
        for (int i = 60; i < 120; i++) { rs[n].upd = 0; rs[n].slot = i % USLOTS; rs[n].d = st[i]; ++n; }
        for (int i = 0; i < 10; i++) { rs[n].upd = 0; rs[n].slot = i % USLOTS; rs[n].d = mk(900000 + (uint64_t)f * 100 + (uint64_t)i); ++n; }   // dynamic casters
        for (int i = 0; i < W * H; i++) tile[i] = 0xffff;
        const int32_t rect[4] = {0, 0, W, H}; int why = 0;
        const int a = nrshc_arm(c, &p, 0xA7A5, rect, 1, 0xffff, 0, 1, 1, &why);
        nrshc_begin(c, &p, r, a, why); nrshc_undo_reset(u);
        if (a == NRSHC_A_HIT) { memcpy(tile, S, sizeof S); ++hits; } else if (a == NRSHC_A_BUILD) ++builds;
        // execution 1 (and the reference: the same records drawn in order into a cleared tile, from the same start memory)
        uint16_t U0[USLOTS]; memcpy(U0, U, sizeof U);
        { uint16_t Ur[USLOTS]; memcpy(Ur, U0, sizeof Ur); for (int i = 0; i < W * H; i++) ref[i] = 0xffff;
          for (int i = 0; i < n; i++) { if (rs[i].upd) Ur[rs[i].slot] = rs[i].val; else uraster(ref, &rs[i].d, Ur, rs[i].slot); } }
        for (int i = 0; i < n; i++) {
            if (rs[i].upd) { (void)nrshc_undo_note(u, (uint64_t)rs[i].slot * 2, &U[rs[i].slot], 2); U[rs[i].slot] = rs[i].val; continue; }
            const uint64_t h = uhash(&rs[i].d, U, rs[i].slot);
            if (!nrshc_draw(c, &p, r, h, h ^ 1, h, 0, 0, 1)) uraster(tile, &rs[i].d, U, rs[i].slot);
        }
        uint16_t U1[USLOTS]; memcpy(U1, U, sizeof U);
        const int e = nrshc_end(c, &p, r, 0xA7A5, rect, 0xffff, !u->over);
        CHECK(e >= 0, "updates: range executable again");
        if (e == NRSHC_E_BUILD || e == NRSHC_E_REST || e == NRSHC_E_RECOVER) {
            if (e == NRSHC_E_BUILD) { memcpy(S, tile, sizeof S); have_s = 1; }
            if (e == NRSHC_E_RECOVER) { for (int i = 0; i < W * H; i++) tile[i] = 0xffff; have_s = 0; }
            // without the undo: execution 2 would read the end of execution 1 (count the desyncs it would see; nothing drawn)
            { int bad_no = 0; uint32_t o = 0; uint16_t Ux[USLOTS]; memcpy(Ux, U, sizeof Ux);
              for (int i = 0; i < n; i++) { if (rs[i].upd) { Ux[rs[i].slot] = rs[i].val; continue; } int bad = 0; (void)nrshc_again(r, NRSHC_R_ALL, o++, uhash(&rs[i].d, Ux, rs[i].slot), &bad); bad_no += bad; }
              CHECK(bad_no > 0, "updates: without the undo execution 2 desyncs (%d)", bad_no); }
            // the undo, then execution 2 replays the updates in order
            for (uint32_t i = 0; i < u->n; i++) { uint64_t ad; uint32_t ln; const uint8_t *old = nrshc_undo_back(u, i, &ad, &ln); memcpy((uint8_t *)U + ad, old, ln); }
            CHECK(!memcmp(U, U0, sizeof U), "updates: the undo gives the range's start memory");
            ++again_runs; uint32_t o = 0;
            for (int i = 0; i < n; i++) {
                if (rs[i].upd) { U[rs[i].slot] = rs[i].val; continue; }
                int bad = 0; const int d = nrshc_again(r, e == NRSHC_E_RECOVER ? NRSHC_R_ALL : NRSHC_R_REST, o++, uhash(&rs[i].d, U, rs[i].slot), &bad);
                bad_total += bad; if (d) uraster(tile, &rs[i].d, U, rs[i].slot);
            }
            CHECK(o == r->ndraw, "updates: execution 2 met every draw (%u of %u)", o, r->ndraw);
            CHECK(!memcmp(U, U1, sizeof U), "updates: memory after execution 2 = after execution 1");
        }
        if (memcmp(tile, ref, sizeof tile)) exact = 0;
        CHECK(!memcmp(tile, ref, sizeof tile), "updates frame %d: cached tile differs from the full render", f);
        (void)have_s;
    }
    printf("  %-18s frames  40: hit %3d build %2d executions 2 %d, desyncs %d\n", "uniform updates", hits, builds, again_runs, bad_total);
    CHECK(bad_total == 0 && exact, "updates: no desync with the undo");
    CHECK(builds >= 1 && hits >= 25, "updates: still cached (builds %d hits %d)", builds, hits);
    // the log: reverse order, repeated addresses restore the oldest bytes, a full log is refused
    nrshc_undo_reset(u); uint16_t M[4] = {1, 2, 3, 4};
    uint16_t v = 10; CHECK(nrshc_undo_note(u, 2, &M[1], 2), "note"); M[1] = v;
    v = 11; CHECK(nrshc_undo_note(u, 2, &M[1], 2), "note again"); M[1] = v;
    CHECK(nrshc_undo_note(u, 0, M, 8), "note 8 B"); M[0] = 9; M[1] = 9; M[2] = 9; M[3] = 9;
    for (uint32_t i = 0; i < u->n; i++) { uint64_t ad; uint32_t ln; const uint8_t *old = nrshc_undo_back(u, i, &ad, &ln); memcpy((uint8_t *)M + ad, old, ln); }
    CHECK(M[0] == 1 && M[1] == 2 && M[2] == 3 && M[3] == 4, "reverse restore gives the oldest bytes (%u %u %u %u)", M[0], M[1], M[2], M[3]);
    nrshc_undo_reset(u); static uint8_t big[NRSHC_UNDO_BYTES];
    CHECK(nrshc_undo_note(u, 0, big, NRSHC_UNDO_BYTES) && !u->over, "a log exactly full");
    CHECK(!nrshc_undo_note(u, 0, big, 1) && u->over, "one byte more: full");
    CHECK(!nrshc_undo_note(u, 0, big, 0), "stays full");
    nrshc_undo_reset(u); for (int i = 0; i < NRSHC_UNDO_N; i++) (void)nrshc_undo_note(u, 0, big, 1);
    CHECK(!u->over && !nrshc_undo_note(u, 0, big, 1) && u->over, "entries full");
    (void)exec_recs;
    free(c); free(r); free(u);
}
// [shc3] the word diff of the uniform diagnostics: which 4-byte words moved, and "the same shift in every caster" vs per-object changes
static void test_words(void) {
    float a[48], b[48], d[64] = {0}, rd[64] = {0};
    for (int i = 0; i < 48; i++) a[i] = b[i] = (float)i * 0.5f;
    CHECK(nrshc_words(a, b, sizeof a, d) == 0, "equal: no word");
    b[3] += 2.f; b[7] -= 1.f; b[11] += 0.25f;   // a translation column of matrix 0 (rows 0-2, .w)
    const uint64_t m = nrshc_words(a, b, sizeof a, rd);
    CHECK(m == ((1ull << 3) | (1ull << 7) | (1ull << 11)) && rd[3] == 2.f && rd[7] == -1.f, "translation words (%llx)", (unsigned long long)m);
    float c2[48], e2[48]; for (int i = 0; i < 48; i++) c2[i] = e2[i] = (float)i * 3.f + 100.f;
    e2[3] += 2.f; e2[7] -= 1.f; e2[11] += 0.25f;   // another caster, the same shift
    const uint64_t m2 = nrshc_words(c2, e2, sizeof c2, d);
    CHECK(nrshc_same_delta(m2, d, m, rd), "the same shift in another caster");
    e2[7] -= 1.f; (void)nrshc_words(c2, e2, sizeof c2, d); CHECK(!nrshc_same_delta(m2, d, m, rd), "another amount: not the same");
    e2[7] += 1.f; e2[0] += 1.f; const uint64_t m3 = nrshc_words(c2, e2, sizeof c2, d); CHECK(!nrshc_same_delta(m3, d, m, rd), "another word: not the same");
    uint8_t big[300] = {0}, big2[300] = {0}; big2[260] = 1; CHECK(nrshc_words(big, big2, 300, d) == 0, "past 256 B: not counted in words");
}


// [shc4] the write journal (nr_shc.h nrshc_wj_*): against an exact model (the serial of the newest write touching each byte range), on random
// writes and queries over 8 MB with page / block / huge ranges: the version of a range is >= its exact newest serial, a range written since
// its last query always gets a NEW version, a range nothing touched keeps its version unless a write hit a page / block sharing its slot
// (counted: false changes, harmless - the draw is drawn).
static uint64_t wj_exact(const uint64_t *lw, uint64_t a, uint64_t n) { uint64_t v = 0; for (uint64_t p = a >> 12; p <= (a + n - 1) >> 12; p++) if (lw[p] > v) v = lw[p]; return v; }
static void test_journal(void) {
    nrshc_wj *j = (nrshc_wj *)calloc(1, sizeof *j); enum { PAGES = 2048, Q = 64 };
    static uint64_t lw[PAGES]; memset(lw, 0, sizeof lw);
    uint64_t qa[Q], qn[Q], qv[Q], qe[Q];
    for (int i = 0; i < Q; i++) { qa[i] = (uint64_t)(rnd() % (PAGES * 4096 - 300000)); qn[i] = 1 + rnd() % (i < 8 ? 300000 : 20000); qv[i] = nrshc_wj_ver(j, qa[i], qn[i]); qe[i] = 0; }
    int missed = 0, below = 0; uint64_t false_changes = 0, unchanged = 0;
    for (int step = 0; step < 60000; step++) {
        const uint32_t k = rnd() % 100; uint64_t a, n;
        if (k < 90) { a = (uint64_t)(rnd() % (PAGES * 4096 - 20000)); n = 1 + rnd() % 12000; }         // an upload (a few KB)
        else if (k < 99) { a = (uint64_t)(rnd() % (PAGES * 4096 - 300000)); n = 1 + rnd() % 300000; }   // a big copy / fill
        else { a = 0; n = (uint64_t)PAGES * 4096; }                                                     // all of it
        nrshc_wj_write(j, a, n); for (uint64_t p = a >> 12; p <= (a + n - 1) >> 12; p++) lw[p] = j->serial;
        for (int i = 0; i < Q; i++) {
            const uint64_t v = nrshc_wj_ver(j, qa[i], qn[i]), e = wj_exact(lw, qa[i], qn[i]);
            if (v < e) ++below;
            if (e != qe[i] && v == qv[i]) ++missed;          // written since the last query, same version: the bug class
            if (e == qe[i]) { if (v != qv[i]) ++false_changes; else ++unchanged; }
            qv[i] = v; qe[i] = e;
        }
    }
    CHECK(missed == 0 && below == 0, "journal: every write changes the version of the ranges it touches (missed %d, below %d)", missed, below);
    CHECK(unchanged > 0, "journal: an untouched range keeps its version (kept %llu, false changes %llu)", (unsigned long long)unchanged, (unsigned long long)false_changes);
    // a sparse case (the phone: ~20 uploads a frame into a large pool): an untouched range keeps its version almost always
    nrshc_wj *k3 = (nrshc_wj *)calloc(1, sizeof *k3); uint64_t keep = 0, flip = 0;
    for (int f = 0; f < 2000; f++) {
        uint64_t before[Q]; for (int i = 0; i < Q; i++) before[i] = nrshc_wj_ver(k3, 0x20000000ull + (uint64_t)i * 0x40000, 0x3000);
        for (int w = 0; w < 20; w++) nrshc_wj_write(k3, 0x40000000ull + (uint64_t)(rnd() % (256u << 20)), 1 + rnd() % 10000);   // elsewhere
        for (int i = 0; i < Q; i++) { if (nrshc_wj_ver(k3, 0x20000000ull + (uint64_t)i * 0x40000, 0x3000) == before[i]) ++keep; else ++flip; }
    }
    CHECK(flip * 50 < keep, "journal: writes elsewhere rarely change a version (kept %llu, false %llu)", (unsigned long long)keep, (unsigned long long)flip);
    // the edges: the last byte of a page, the last page of a full block, a range straddling two blocks, n = 0
    nrshc_wj *k2 = (nrshc_wj *)calloc(1, sizeof *k2);
    const uint64_t r0 = nrshc_wj_ver(k2, 0x40000, 1); nrshc_wj_write(k2, 0x40000 + 4095, 1); CHECK(nrshc_wj_ver(k2, 0x40000, 1) > r0, "a write at the end of the page");
    const uint64_t blk = nrshc_wj_ver(k2, 0x100000, 0x10000); nrshc_wj_write(k2, 0x10f000, 8); CHECK(nrshc_wj_ver(k2, 0x100000, 0x10000) > blk, "a full block sees its last page");
    const uint64_t st = nrshc_wj_ver(k2, 0x10f800, 0x1000); nrshc_wj_write(k2, 0x110000, 4); CHECK(nrshc_wj_ver(k2, 0x10f800, 0x1000) > st, "straddling two blocks");
    CHECK(nrshc_wj_ver(k2, 0x40000, 0) == 0, "n = 0");
    printf("  %-18s 60000 writes x %d ranges: missed %d below %d, false changes %llu of %llu; sparse: %llu of %llu kept\n", "write journal", Q, missed, below,
           (unsigned long long)false_changes, (unsigned long long)(false_changes + unchanged), (unsigned long long)keep, (unsigned long long)(keep + flip));
    free(j); free(k2); free(k3);
}

// [shc4] THE PHONE BUG CLASS (18.11, C_s4_shc2 / C_m4_shc2: "verify: cascade 2 ... the cached tile differs ... in ~1000 bytes" while every draw
// hashed as known): a caster's shape lives in memory the hash only knows by ADDRESS (a vertex buffer, an index range, a texture used for the
// alpha test). Casters: a static set; "cloth" casters whose bytes are rewritten under the same address every frame for a while (an upload
// into the same buffer); one caster rewritten once (a streamed LOD / mip); one whose "texture" is re-uploaded (its upload number moves).
// Without the content version the layer keeps the old shape: the cached tile differs from the full render (reproduced). With the version
// (nrshc_wj_ver of the bytes + the image's upload number mixed into the hash, as the backend's hash() does) it is exact every frame, the
// rewritten casters are drawn while they change and cached again once they stop, and the draws are counted as "new by content" (channel J /
// T). Execution 2 takes execution 1's version per ordinal (nrshc_again_v): no desync.
typedef struct { int32_t x0, y0, x1, y1; uint16_t z0, dz; int32_t op; } Geo;
enum { CN = 180 };
static Geo G[CN]; static uint32_t TEXUP[CN];   // the caster's bytes; its "texture" upload number (a mask that crops its rectangle)
static uint64_t gaddr(int id) { return 0x10000000ull + (uint64_t)id * 0x900; }   // 2.25 KB apart: casters share pages and blocks
static Draw gdraw(int id) {
    Draw d; d.id = (uint64_t)id; d.x0 = G[id].x0; d.y0 = G[id].y0; d.x1 = G[id].x1 - (int)(TEXUP[id] % 3); if (d.x1 <= d.x0) d.x1 = d.x0 + 1;
    d.y1 = G[id].y1; d.z0 = G[id].z0; d.dz = G[id].dz; d.op = G[id].op; d.order_free = 1; return d;
}
static uint64_t ghash(int id) { uint64_t h = nrshc_mix(0x5348, (uint64_t)id); return nrshc_mix(h, gaddr(id)); }   // by address (the old hash)
static uint64_t gver(const nrshc_wj *j, int id, uint64_t *vj, uint64_t *vt) {
    *vj = nrshc_mix(0x4A, nrshc_wj_ver(j, gaddr(id), sizeof(Geo))); *vt = nrshc_mix(0x54, TEXUP[id]); return nrshc_mix(*vj, *vt);
}
static void grewrite(nrshc_wj *j, int id) {
    const Draw d = mk(77); G[id].x0 = d.x0; G[id].y0 = d.y0; G[id].x1 = d.x1; G[id].y1 = d.y1; G[id].z0 = (uint16_t)(d.z0 >> 4); G[id].dz = d.dz;   // near: it shows
    nrshc_wj_write(j, gaddr(id), sizeof(Geo));
}
static int content_run(int versioned, nrshc_par p, Tally *t, uint64_t *ncj, uint64_t *nct) {
    nrshc_cas *c = nrshc_cas_new(); nrshc_range *r = (nrshc_range *)calloc(1, sizeof *r); nrshc_wj *j = (nrshc_wj *)calloc(1, sizeof *j);
    static uint16_t tile[W * H], ref[W * H], S[W * H]; int have_s = 0, wrong = 0; static Draw ds[CN]; static int ids[CN];
    rng = 0xC0FFEE;
    for (int i = 0; i < CN; i++) { const Draw d = mk(40000 + (uint64_t)i); G[i].x0 = d.x0; G[i].y0 = d.y0; G[i].x1 = d.x1; G[i].y1 = d.y1; G[i].z0 = d.z0; G[i].dz = d.dz; G[i].op = d.op; TEXUP[i] = 1; }
    G[11].z0 = 10; G[11].x0 = 0; G[11].x1 = W; G[11].y0 = 0; G[11].y1 = H;   // the alpha-tested caster: near and wide (its crop shows)
    *ncj = *nct = 0;
    for (int f = 0; f < 90; f++) {
        if (f >= 20 && f < 40) for (int id = 0; id < 3; id++) grewrite(j, id);   // cloth: rewritten every frame
        if (f == 55) grewrite(j, 7);                                           // one upload into a static caster's buffer
        if (f == 65) TEXUP[11] += 1;                                           // its texture re-uploaded (a mip arrived)
        int n = 0; for (int i = 0; i < CN; i++) { ids[n] = i; ds[n] = gdraw(i); ++n; }
        for (int i = n - 1; i > 0; i--) { const int k = (int)(rnd() % (unsigned)(i + 1)); const Draw td = ds[i]; ds[i] = ds[k]; ds[k] = td; const int ti = ids[i]; ids[i] = ids[k]; ids[k] = ti; }
        for (int i = 0; i < W * H; i++) tile[i] = 0xffff;
        const int32_t rect[4] = {0, 0, W, H}; int why = 0;
        const int a = nrshc_arm(c, &p, 0xA7A5, rect, 1, 0xffff, 0, 1, 1, &why); nrshc_begin(c, &p, r, a, why);
        if (a == NRSHC_A_HIT) { CHECK(have_s, "content: hit without a layer"); memcpy(tile, S, sizeof S); ++t->hits; } else if (a == NRSHC_A_BUILD) ++t->builds; else ++t->normals;
        for (int i = 0; i < n; i++) {
            uint64_t vj = 0, vt = 0; const uint64_t hn = ghash(ids[i]), v = versioned ? gver(j, ids[i], &vj, &vt) : 0, h = versioned ? nrshc_mix(hn, v) : hn;
            if (nrshc_draw(c, &p, r, h, hn ^ 0x55, hn, nrshc_vtag(vj, 0, vt), v, 1)) ++t->skipped; else { raster(tile, &ds[i]); ++t->drawn; }
        }
        *ncj += r->nc[NRSHC_VC_J]; *nct += r->nc[NRSHC_VC_T];
        const int e = nrshc_end(c, &p, r, 0xA7A5, rect, 0xffff, 1);
        if (e == NRSHC_E_BUILD) { memcpy(S, tile, sizeof S); have_s = 1; }
        if (e == NRSHC_E_RECOVER) { for (int i = 0; i < W * H; i++) tile[i] = 0xffff; have_s = 0; ++t->recovers; }
        if (e == NRSHC_E_BUILD || e == NRSHC_E_REST || e == NRSHC_E_RECOVER)
            for (int i = 0; i < n; i++) {
                int bad = 0; const uint64_t hn = ghash(ids[i]), h = versioned ? nrshc_mix(hn, nrshc_again_v(r, (uint32_t)i)) : hn;
                if (nrshc_again(r, e == NRSHC_E_RECOVER ? NRSHC_R_ALL : NRSHC_R_REST, (uint32_t)i, h, &bad)) raster(tile, &ds[i]);
                CHECK(!bad, "content: execution 2 desync");
            }
        reference(ds, n, 0xffff, ref);
        if (memcmp(tile, ref, sizeof tile)) ++wrong;
        if (versioned) CHECK(!memcmp(tile, ref, sizeof tile), "content (versioned) frame %d: cached tile differs from the full render", f);
        ++t->frames;
    }
    free(c); free(r); free(j);
    return wrong;
}
// [shc4] the write-protect version (nvn_dirty_version's argument, nvn_dirty.c) on a model of the tracker: pages PROT / NONE with a write
// generation; a write to a PROT page faults (generation = ++now, page NONE), a write to a NONE page is silent; protect: NONE -> PROT. The
// version of a range = (every page PROT) ? max generation : none. Claim: two observations of the same version see the same bytes.
static void test_protect_model(void) {
    enum { NP = 24 }; uint8_t prot[NP] = {0}; uint64_t gen[NP] = {0}, now = 1; uint32_t bytes[NP] = {0};
    typedef struct { uint64_t v; uint32_t content[4]; int have; } Seen; static Seen seen[NP][4]; memset(seen, 0, sizeof seen);
    int bad = 0, versions = 0, same = 0;
    for (int step = 0; step < 200000; step++) {
        const uint32_t k = rnd() % 10, pg = rnd() % NP;
        if (k < 4) { if (prot[pg]) { gen[pg] = ++now; prot[pg] = 0; } bytes[pg] = rnd(); }   // the game writes (a fault when protected)
        else if (k < 6) prot[pg] = 1;                                                       // someone protects
        else {                                                                               // a query of the range [pg, pg + len)
            const uint32_t len = 1 + rnd() % 4; if (pg + len > NP) continue;
            int all = 1; uint64_t g = 0; for (uint32_t i = 0; i < len; i++) { if (!prot[pg + i]) all = 0; if (gen[pg + i] > g) g = gen[pg + i]; }
            if (!all) continue;
            ++versions;
            Seen *sn = &seen[pg][len - 1]; uint32_t cur[4] = {0}; for (uint32_t i = 0; i < len; i++) cur[i] = bytes[pg + i];
            if (sn->have && sn->v == g) { ++same; if (memcmp(sn->content, cur, sizeof cur)) ++bad; }
            sn->have = 1; sn->v = g; memcpy(sn->content, cur, sizeof cur);
        }
    }
    CHECK(bad == 0 && versions > 1000 && same > 100, "protect model: one version = one content (%d bad of %d repeats, %d versions)", bad, same, versions);
}

// [NR-ZERO] DM_NR_SHC_MEMO: the (table, id) -> node memo against an exact model (a map that only grows, as the backend's std::map of
// texture registrations: nodes are never erased at run time). Every get must return either NULL or the model's node of that key under the
// current stamp; a stamp bump (the shutdown's images_epoch) forgets everything; NULL answers are never stored.
static void test_pmemo(void) {
    static nrshc_pmemo m; memset(&m, 0, sizeof m);
    enum { TABS = 5, IDS = 20000 };
    static int node[TABS][IDS];              // the model: node[t][id] exists (address = the node) once registered
    static unsigned char reg[TABS][IDS];
    const void *tab[TABS]; for (int t = 0; t < TABS; t++) tab[t] = (const void *)(uintptr_t)(0x10000 + 0x1000 * t);
    uint64_t gen = 1, rng = 0x1234567;
    long hits = 0, wrong = 0, gets = 0;
    for (int it = 0; it < 2000000; it++) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        const int t = (int)(rng % TABS), id = (int)((rng >> 8) % IDS), op = (int)((rng >> 40) % 100);
        if (op < 3) { reg[t][id] = 1; continue; }                     // a registration: the node appears (never erased)
        if (op == 3 && (rng >> 50) % 4000 == 0) { ++gen; continue; }  // the shutdown's stamp bump (rare)
        // the backend's tfind: memo, else the map (+ store a found node)
        const void *want = reg[t][id] ? (const void *)&node[t][id] : NULL;
        const void *c = nrshc_pm_get(&m, tab[t], id, gen); ++gets;
        if (c) { ++hits; if (c != want) ++wrong; }
        else nrshc_pm_put(&m, tab[t], id, gen, want);
    }
    CHECK(wrong == 0, "pmemo: %ld answers differ from the map", wrong);
    CHECK(hits > 0, "pmemo: some hits (%ld of %ld over 100000 random keys)", hits, gets);
    // a shadow pass's working set (a few hundred registered textures, used again every range): nearly every lookup is a memo answer
    { long h2 = 0, g2 = 0, w2 = 0; for (int r = 0; r < 200; r++) for (int k = 0; k < 300; k++) { const int id = (k * 37) % IDS; reg[1][id] = 1;
          const void *want = &node[1][id]; const void *c = nrshc_pm_get(&m, tab[1], id, gen); ++g2; if (c) { ++h2; if (c != want) ++w2; } else nrshc_pm_put(&m, tab[1], id, gen, want); }
      CHECK(w2 == 0 && h2 * 100 >= g2 * 95, "pmemo: working set of 300: %ld of %ld answered (wrong %ld)", h2, g2, w2); }
    // stamps and NULL
    static nrshc_pmemo z; memset(&z, 0, sizeof z); int a = 1, b = 2;
    nrshc_pm_put(&z, tab[0], 7, 5, NULL); CHECK(nrshc_pm_get(&z, tab[0], 7, 5) == NULL, "pmemo: NULL is not stored");
    nrshc_pm_put(&z, tab[0], 7, 5, &a); CHECK(nrshc_pm_get(&z, tab[0], 7, 5) == &a, "pmemo: stored");
    CHECK(nrshc_pm_get(&z, tab[0], 7, 6) == NULL, "pmemo: another stamp forgets");
    CHECK(nrshc_pm_get(&z, tab[1], 7, 5) == NULL && nrshc_pm_get(&z, tab[0], 8, 5) == NULL, "pmemo: another table / id");
    nrshc_pm_put(&z, tab[0], 7, 6, &b); CHECK(nrshc_pm_get(&z, tab[0], 7, 6) == &b && nrshc_pm_get(&z, tab[0], 7, 5) == NULL, "pmemo: restamped");
    CHECK(nrshc_pm_get(&z, NULL, 0, 0) == NULL, "pmemo: an empty slot never answers (map NULL, stamp 0)");
}
int main(void) {
    test_pmemo();
    test_tab();
    test_arm();
    test_shape();
    nrshc_par p = nrshc_par_default(); p.min_static = 16; p.max_age = 40;
    { nrshc_par q = p; q.probe = 0; test_stability(q); }
    test_updates(p);
    test_words();
    test_journal();
    test_protect_model();
    {   // [shc4] the phone bug class: bytes rewritten under an unchanged address
        Tally a = {0}, b = {0}; uint64_t ja = 0, ta = 0, jb = 0, tb = 0; nrshc_par q = p; q.probe = 0;
        const int wrong_old = content_run(0, q, &a, &jb, &tb), wrong_new = content_run(1, q, &b, &ja, &ta);
        printf("  %-18s frames  90: by address only %d frames wrong (hit %llu); versioned %d wrong, hit %llu build %llu recover %llu, new by content J %llu T %llu\n", "content rewrite",
               wrong_old, (unsigned long long)a.hits, wrong_new, (unsigned long long)b.hits, (unsigned long long)b.builds, (unsigned long long)b.recovers, (unsigned long long)ja, (unsigned long long)ta);
        CHECK(wrong_old > 0, "content: the bug is reproduced by the address-only hash (%d wrong frames)", wrong_old);
        CHECK(wrong_new == 0 && b.hits >= 50, "content: versioned exact and still cached (wrong %d, hits %llu)", wrong_new, (unsigned long long)b.hits);
        CHECK(ja >= 3 && ta >= 1, "content: counted new by content, J %llu T %llu", (unsigned long long)ja, (unsigned long long)ta);
    }
    const Scn sc[] = {
        {"static only", 200, 0, 60, 0, -1, -1, -1, -1, 0, 0},
        {"static + dynamic", 300, 40, 80, 1, -1, -1, -1, -1, 0, 0},
        {"vanishing caster", 300, 20, 60, 1, 30, -1, -1, -1, 0, 0},
        {"camera moves", 250, 30, 60, 1, -1, 25, -1, -1, 0, 0},
        {"tainted draw", 250, 30, 60, 1, -1, -1, 33, -1, 0, 0},
        {"clear value", 250, 30, 60, 1, -1, -1, -1, 20, 0, 0},
        {"duplicates", 280, 10, 60, 1, -1, -1, -1, -1, 1, 0},
        {"few static", 10, 50, 40, 1, -1, -1, -1, -1, 0, 0},
        {"all dynamic", 0, 120, 30, 1, -1, -1, -1, -1, 0, 0},
        {"walk, then stand", 250, 30, 100, 1, -1, -1, -1, -1, 0, 60},
    };
    Tally t[sizeof sc / sizeof sc[0]];
    for (unsigned i = 0; i < sizeof sc / sizeof sc[0]; i++) {
        scenario(&sc[i], p, &t[i]);
        printf("  %-18s frames %3llu: hit %3llu build %2llu normal %3llu recover %llu rest %llu blind %2llu; draws drawn %6llu skipped %6llu\n", sc[i].name,
               (unsigned long long)t[i].frames, (unsigned long long)t[i].hits, (unsigned long long)t[i].builds, (unsigned long long)t[i].normals,
               (unsigned long long)t[i].recovers, (unsigned long long)t[i].rests, (unsigned long long)t[i].blind, (unsigned long long)t[i].drawn, (unsigned long long)t[i].skipped);
    }
    // expectations on the decisions (exactness is checked every frame above)
    CHECK(t[0].hits >= 50 && t[0].recovers == 0, "static: mostly hits (%llu)", (unsigned long long)t[0].hits);
    CHECK(t[0].builds >= 2, "static: rebuilt by age (%llu builds)", (unsigned long long)t[0].builds);
    CHECK(t[1].hits >= 60 && t[1].skipped > t[1].drawn, "static + dynamic: hits skip the static part (%llu / %llu)", (unsigned long long)t[1].skipped, (unsigned long long)t[1].drawn);
    CHECK(t[2].recovers == 1, "vanishing caster: exactly one recover (%llu)", (unsigned long long)t[2].recovers);
    CHECK(t[3].recovers == 1, "camera move: one recover (%llu)", (unsigned long long)t[3].recovers);
    CHECK(t[4].recovers == 1, "tainted: one recover (%llu)", (unsigned long long)t[4].recovers);
    CHECK(t[6].hits >= 40, "duplicates: hits (%llu)", (unsigned long long)t[6].hits);
    CHECK(t[7].hits == 0 && t[7].builds == 0 && t[7].skipped == 0, "few static: never cached");
    CHECK(t[8].hits == 0 && t[8].skipped == 0, "all dynamic: never cached");
    CHECK(t[8].blind >= 15, "all dynamic: mostly not hashed (%llu)", (unsigned long long)t[8].blind);
    CHECK(t[9].blind >= 35 && t[9].hits >= 25 && t[9].hits <= 37, "walk then stand: probed while walking (%llu blind), hits within a probe period of standing (%llu)", (unsigned long long)t[9].blind, (unsigned long long)t[9].hits);
    // poison: a HIT without the layer copy must differ from the reference (shows the layer carries the static shadow)
    {
        nrshc_cas *c = nrshc_cas_new(); nrshc_range *r = (nrshc_range *)calloc(1, sizeof *r); Layer *L = (Layer *)calloc(1, sizeof *L);
        static Draw ds[MAXD]; static uint16_t tile[W * H], ref[W * H]; Tally tt = {0}; int poisoned_diff = 0;
        for (int i = 0; i < 100; i++) ds[i] = mk(7000 + (uint64_t)i);
        for (int f = 0; f < 10; f++) { const int a = run_range(c, &p, r, L, ds, 100, 0xffff, 0, f >= 6, tile, &tt); reference(ds, 100, 0xffff, ref);
            if (a == NRSHC_A_HIT && f >= 6 && memcmp(tile, ref, sizeof tile)) ++poisoned_diff; }
        CHECK(poisoned_diff >= 3, "poison: the static shadow is gone (%d)", poisoned_diff);
        free(c); free(r); free(L);
    }
    // execution 2 by ordinal: a desync is reported
    {
        nrshc_range *r = (nrshc_range *)calloc(1, sizeof *r); r->ndraw = 2; r->h[0] = 5; r->h[1] = 6; r->f[0] = NRSHC_F_DRAWN; r->f[1] = NRSHC_F_SKIP; int bad;
        CHECK(!nrshc_again(r, NRSHC_R_REST, 0, 5, &bad) && !bad, "rest: drawn one not again");
        CHECK(nrshc_again(r, NRSHC_R_REST, 1, 6, &bad) && !bad, "rest: deferred one");
        CHECK(nrshc_again(r, NRSHC_R_DRAWN, 0, 5, &bad) && !nrshc_again(r, NRSHC_R_DRAWN, 1, 6, &bad), "drawn");
        (void)nrshc_again(r, NRSHC_R_REST, 1, 7, &bad); CHECK(bad, "hash desync");
        (void)nrshc_again(r, NRSHC_R_REST, 2, 7, &bad); CHECK(bad, "ordinal past the range");
        free(r);
    }
    printf("nr_shc_test: %d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
