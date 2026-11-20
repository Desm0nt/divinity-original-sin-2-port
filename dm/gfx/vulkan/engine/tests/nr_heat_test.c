// [DOS2] NR-HEAT core test (dm/gfx/nr/nr_heat.c). Build: clang -std=c11 -O2 -Wall -Wextra nr_heat_test.c ../../../nr/nr_heat.c
// Optional argument: a real LSPK package (Origins.pak of the Switch RomFS) - its level table is read and checked as well.
// [NR-BN] Optional 2nd / 3rd: a decompressed meta.lsf of a real save and the level it must name (default TUT_Tutorial_A).
#include "../../../nr/nr_heat.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < 1e-6 * (1 + fabs((double)(b))))

static size_t lz4_literals(const uint8_t *s, size_t n, uint8_t *d) {   // a valid LZ4 block of one literal-only sequence
    size_t o = 0;
    if (n < 15) d[o++] = (uint8_t)(n << 4);
    else { d[o++] = 0xf0; size_t r = n - 15; while (r >= 255) { d[o++] = 255; r -= 255; } d[o++] = (uint8_t)r; }
    memcpy(d + o, s, n); return o + n;
}
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void test_lz4(void) {
    const uint8_t blk[] = {0x32, 'a', 'b', 'c', 3, 0, 0x30, 'x', 'y', 'z'};
    uint8_t out[64];
    CHECK(nrh_lz4(blk, sizeof blk, out, sizeof out) == 12 && !memcmp(out, "abcabcabcxyz", 12));
    CHECK(nrh_lz4(blk, sizeof blk, out, 11) == -1);                     // does not fit
    const uint8_t bad_off[] = {0x32, 'a', 'b', 'c', 9, 0, 0x30, 'x'};   // offset before the start
    CHECK(nrh_lz4(bad_off, sizeof bad_off, out, sizeof out) == -1);
    const uint8_t zero_off[] = {0x32, 'a', 'b', 'c', 0, 0, 0x30, 'x'};
    CHECK(nrh_lz4(zero_off, sizeof zero_off, out, sizeof out) == -1);
    const uint8_t trunc[] = {0x32, 'a', 'b', 'c', 3};                   // half an offset
    CHECK(nrh_lz4(trunc, sizeof trunc, out, sizeof out) == -1);
    const uint8_t run[] = {0x1f, 'z', 1, 0, 10, 0x00};                  // 1 literal, match off 1 len 15+10+4 = 29 -> 30 'z', then an empty end
    CHECK(nrh_lz4(run, sizeof run, out, sizeof out) == 30 && out[0] == 'z' && out[29] == 'z');
    static uint8_t big[1000], enc[1100], dec[1000];
    for (int i = 0; i < 1000; i++) big[i] = (uint8_t)(i * 7);
    const size_t en = lz4_literals(big, 1000, enc);
    CHECK(nrh_lz4(enc, en, dec, 1000) == 1000 && !memcmp(big, dec, 1000));
}

static void test_paths(void) {
    const char *lv; size_t ln;
    const char *a = "Mods/DivinityOrigins_1301db3d/Levels/FJ_FortJoy_Main/Terrains/x.data";
    CHECK(nrh_level_of_path(a, strlen(a), &lv, &ln) == 1 && ln == strlen("FJ_FortJoy_Main") && !strncmp(lv, "FJ_FortJoy_Main", ln));
    const char *b = "Mods/DivinityOrigins_1301db3d/Globals/TUT_Tutorial_A/Items/_merged.lsf";
    CHECK(nrh_level_of_path(b, strlen(b), &lv, &ln) == 1 && !strncmp(lv, "TUT_Tutorial_A", ln) && ln == 14);
    const char *c = "Public/Game/Assets/Materials/x.lsf";
    CHECK(nrh_level_of_path(c, strlen(c), &lv, &ln) == 0);
    const char *d = "Mods/X/Levels/OnlyAFolderName";   // no file under it
    CHECK(nrh_level_of_path(d, strlen(d), &lv, &ln) == 0);
    const char *e = "Mods/X/Levels//y";                // empty name
    CHECK(nrh_level_of_path(e, strlen(e), &lv, &ln) == 0);
    const char *f = "Mods/Shared/Levels/Arx_BlackhouseCellar Develop/a.lsf";
    CHECK(nrh_level_of_path(f, strlen(f), &lv, &ln) == 1 && ln == strlen("Arx_BlackhouseCellar Develop"));
    nrh_names *t = calloc(1, sizeof *t);
    CHECK(nrh_intern(t, "A", 1) == 0 && nrh_intern(t, "B", 1) == 1 && nrh_intern(t, "A", 1) == 0 && t->n == 2);
    CHECK(nrh_intern(t, "AB", 1) == 0);   // only the first n bytes
    CHECK(nrh_intern(t, "", 0) == -1);
    char tab[8] = "a\tb\nc"; nrh_clean(tab); CHECK(!strcmp(tab, "a b c"));
    free(t);
}

// a package list: count + LZ4(count x 280)
static uint8_t *make_list(const char *const *names, const uint32_t *off, const uint32_t *disk, const uint32_t *part, int n, size_t *len) {
    uint8_t *tab = calloc((size_t)n, 280);
    for (int i = 0; i < n; i++) {
        uint8_t *e = tab + (size_t)i * 280;
        memcpy(e, names[i], strlen(names[i]));
        wr32(e + 256, off[i]); wr32(e + 260, disk[i]); wr32(e + 264, disk[i] * 2); wr32(e + 268, part[i]); wr32(e + 272, 2); wr32(e + 276, 0);
    }
    uint8_t *list = malloc((size_t)n * 280 + 64);
    wr32(list, (uint32_t)n);
    *len = 4 + lz4_literals(tab, (size_t)n * 280, list + 4);
    free(tab); return list;
}
static void range_add(void *c, uint32_t lv, uint64_t b) { if (lv < 2) ((uint64_t *)c)[lv] += b; }
static void test_lspk(void) {
    const char *const names[] = {"Mods/M/Levels/B_Level/a.data", "Mods/M/Public/x.lsf", "Mods/M/Levels/A_Level/b.data", "Mods/M/Globals/B_Level/c.lsf",
                                 "Mods/M/Levels/A_Level/part1.data", "Mods/M/Levels/C_Level/empty.data"};
    const uint32_t off[] = {5000, 0, 100, 9000, 100, 7000}, disk[] = {1000, 100, 400, 50, 400, 0}, part[] = {0, 0, 0, 0, 1, 0};
    size_t len; uint8_t *list = make_list(names, off, disk, part, 6, &len);
    nrh_names *t = calloc(1, sizeof *t); nrh_span *v = NULL;
    const int n = nrh_lspk_levels(list, len, t, &v);
    CHECK(n == 3);   // a.data, b.data, c.lsf (Public: not a level; part 1: another file; size 0: dropped)
    CHECK(t->n == 2 && !strcmp(t->name[0], "B_Level") && !strcmp(t->name[1], "A_Level"));
    if (n == 3) {
        CHECK(v[0].off == 100 && v[1].off == 5000 && v[2].off == 9000);   // sorted
        CHECK(nrh_span_find(v, 3, 99) == -1);
        CHECK(nrh_span_find(v, 3, 100) == 1 && nrh_span_find(v, 3, 499) == 1 && nrh_span_find(v, 3, 500) == -1);
        CHECK(nrh_span_find(v, 3, 5000) == 0 && nrh_span_find(v, 3, 5999) == 0 && nrh_span_find(v, 3, 6000) == -1);
        CHECK(nrh_span_find(v, 3, 9049) == 0 && nrh_span_find(v, 3, 9050) == -1 && nrh_span_find(v, 3, 1ull << 40) == -1);
    }
    if (n == 3) {   // [NR-BN] ranges: spans 100..500 (A), 5000..6000 (B), 9000..9050 (B)
        uint64_t got[2] = {0, 0};
        CHECK(nrh_span_range(v, 3, 0, 100, range_add, got) == 0 && !got[0] && !got[1]);
        CHECK(nrh_span_range(v, 3, 0, 1u << 20, range_add, got) == 1450 && got[0] == 1050 && got[1] == 400);   // one big read over all
        got[0] = got[1] = 0; CHECK(nrh_span_range(v, 3, 450, 4600, range_add, got) == 100 && got[1] == 50 && got[0] == 50);   // tail of A, head of B
        got[0] = got[1] = 0; CHECK(nrh_span_range(v, 3, 5500, 10, range_add, got) == 10 && got[0] == 10);
        CHECK(nrh_span_range(v, 3, UINT64_MAX - 5, 100, NULL, NULL) == 0);
    }
    CHECK(nrh_span_find(v, 0, 100) == -1);
    free(v);
    list[5] ^= 0xff;   // the token now promises other lengths: corrupt
    nrh_span *w = NULL; CHECK(nrh_lspk_levels(list, len - 7, t, &w) == -1 && !w);
    uint8_t zero[4] = {0}; CHECK(nrh_lspk_levels(zero, 4, t, &w) == 0 && !w);
    free(list); free(t);
    uint8_t tail[8]; wr32(tail, 40); memcpy(tail + 4, "LSPK", 4);
    uint32_t hs = 0; CHECK(nrh_lspk_tail(tail, &hs) == 0 && hs == 40);
    memcpy(tail + 4, "LSPX", 4); CHECK(nrh_lspk_tail(tail, &hs) == -1);
    uint8_t h[40] = {0}; wr32(h, 13); wr32(h + 4, 1234); wr32(h + 8, 99);
    uint32_t lo = 0, ls = 0; CHECK(nrh_lspk_header(h, sizeof h, &lo, &ls) == 0 && lo == 1234 && ls == 99);
    wr32(h, 18); CHECK(nrh_lspk_header(h, sizeof h, &lo, &ls) == -1);
}

// [NR-BN] save games: the file list entry lookup, an LZ4 frame (linked blocks), the level an LSF meta names
static size_t lz4f_of(const uint8_t *const *blk, const size_t *bn, const int *raw, int nb, uint8_t *d) {   // frame: FLG 0x40 (linked, no checksums)
    size_t o = 0; wr32(d, 0x184D2204u); d[4] = 0x40; d[5] = 0x40; d[6] = 0; o = 7;
    for (int i = 0; i < nb; i++) { wr32(d + o, (uint32_t)bn[i] | (raw[i] ? 0x80000000u : 0)); o += 4; memcpy(d + o, blk[i], bn[i]); o += bn[i]; }
    wr32(d + o, 0); return o + 4;
}
static void test_save(void) {
    const char *const nm[] = {"globals.lsf", "LevelCache/TUT_Tutorial_A.lsf", "Meta.LSF"};
    const uint32_t off[] = {0, 5000, 9000}, disk[] = {4000, 300, 120}, part[] = {0, 0, 0};
    size_t len; uint8_t *list = make_list(nm, off, disk, part, 3, &len);
    nrh_lspk_ent e; memset(&e, 0, sizeof e);
    CHECK(nrh_lspk_entry(list, len, "meta.lsf", &e) == 0 && e.off == 9000 && e.disk == 120 && e.size == 240 && e.flags == 2);   // case-insensitive
    CHECK(nrh_lspk_entry(list, len, "globals.lsf", &e) == 0 && e.off == 0 && e.disk == 4000);
    CHECK(nrh_lspk_entry(list, len, "levelcache/x.lsf", &e) == 1);
    list[5] ^= 0xff; CHECK(nrh_lspk_entry(list, len - 7, "meta.lsf", &e) == -1);
    free(list);
    // frame: block 1 literals "ab_FJ_Fort", block 2 = a match back into block 1 (linked) + literals, block 3 stored
    const uint8_t b1[] = {0xa0, 'a', 'b', '_', 'F', 'J', '_', 'F', 'o', 'r', 't'};
    const uint8_t b2[] = {0x20, 'X', 'Y', 12, 0, 0x10, 'Z'};   // "XY" + 4 bytes from 12 back ("ab_F", block 1) + "Z"
    const uint8_t b3[] = {'s', 't'};
    const uint8_t *blk[] = {b1, b2, b3}; const size_t bn[] = {sizeof b1, sizeof b2, sizeof b3}; const int raw[] = {0, 0, 1};
    uint8_t fr[128], out[64]; const size_t fn = lz4f_of(blk, bn, raw, 3, fr);
    CHECK(nrh_lz4f(fr, fn, out, sizeof out) == 19 && !memcmp(out, "ab_FJ_FortXYab_FZst", 19));
    CHECK(nrh_lz4f(fr, fn, out, 18) == -1);
    fr[0] ^= 1; CHECK(nrh_lz4f(fr, fn, out, sizeof out) == -1); fr[0] ^= 1;
    CHECK(nrh_lz4f(fr, fn - 4, out, sizeof out) == -1);   // no end mark
    // an LSF v3 resource whose values name a level twice and another once (whole words only: "XFJ_FortJoy_Main" does not count)
    nrh_names *t = calloc(1, sizeof *t);
    nrh_intern(t, "TUT_Tutorial_A", 14); nrh_intern(t, "FJ_FortJoy_Main", 15); nrh_intern(t, "ARX_Main", 8);
    const char vals[] = "\x05Shared\0TUT_Tutorial_A\0\x09XFJ_FortJoy_Main\0fj_fortjoy_main\0\x02" "FJ_FortJoy_Main\0";
    const size_t vn = sizeof vals - 1;
    uint8_t vb[256]; const size_t vbn = lz4_literals((const uint8_t *)vals, vn, vb);
    const uint8_t *vblk[] = {vb}; const size_t vbl[] = {vbn}; const int vraw[] = {0};
    uint8_t vf[300]; const size_t vfn = lz4f_of(vblk, vbl, vraw, 1, vf);
    uint8_t lsf[600]; memset(lsf, 0, sizeof lsf); memcpy(lsf, "LSOF", 4); wr32(lsf + 4, 3); wr32(lsf + 8, 0x36090000u);
    const uint32_t sec[3] = {10, 7, 5};   // strings / nodes / attributes: stored (disk 0 = uncompressed size)
    wr32(lsf + 12, sec[0]); wr32(lsf + 16, 0); wr32(lsf + 20, sec[1]); wr32(lsf + 24, 0); wr32(lsf + 28, sec[2]); wr32(lsf + 32, 0);
    wr32(lsf + 36, (uint32_t)vn); wr32(lsf + 40, (uint32_t)vfn); lsf[44] = 0x22;
    size_t p = 52 + sec[0] + sec[1] + sec[2]; memcpy(lsf + p, vf, vfn); p += vfn;
    CHECK(nrh_lsf_level(lsf, p, t) == 1);   // FJ 2 (case-insensitive) > TUT 1
    CHECK(nrh_lsf_level(lsf, p - 3, t) == -1);   // values cut
    wr32(lsf + 4, 6); CHECK(nrh_lsf_level(lsf, p, t) == -1); wr32(lsf + 4, 3);
    lsf[0] = 'X'; CHECK(nrh_lsf_level(lsf, p, t) == -1); lsf[0] = 'L';
    nrh_names *none = calloc(1, sizeof *none); nrh_intern(none, "ARX_Main", 8);
    CHECK(nrh_lsf_level(lsf, p, none) == -1);
    free(none); free(t);
}
// a decompressed meta.lsf of a real save (tools: the scratch copy of a player's save) against the real level names
static void test_real_meta(const nrh_names *t, const char *path, const char *want) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("note: %s not readable, real save meta check skipped\n", path); return; }
    static uint8_t b[1 << 20]; const size_t n = fread(b, 1, sizeof b, f); fclose(f);
    const int lv = nrh_lsf_level(b, n, t);
    printf("real save meta %s: level %s\n", path, lv >= 0 ? t->name[lv] : "-");
    CHECK(lv >= 0 && !strcmp(t->name[lv], want));
}

static const char *g_meta, *g_meta_want;
static void test_real(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("note: %s not readable, real package check skipped\n", path); return; }
    fseek(f, 0, SEEK_END); const long size = ftell(f);
    uint8_t tail[8], hdr[64]; uint32_t hs, lo, ls;
    fseek(f, size - 8, SEEK_SET); CHECK(fread(tail, 1, 8, f) == 8);
    CHECK(nrh_lspk_tail(tail, &hs) == 0 && hs <= sizeof hdr);
    fseek(f, size - (long)hs, SEEK_SET); CHECK(fread(hdr, 1, hs, f) == hs);
    CHECK(nrh_lspk_header(hdr, hs, &lo, &ls) == 0);
    uint8_t *list = malloc(ls); fseek(f, (long)lo, SEEK_SET); CHECK(fread(list, 1, ls, f) == ls); fclose(f);
    nrh_names *t = calloc(1, sizeof *t); nrh_span *v = NULL;
    const int n = nrh_lspk_levels(list, ls, t, &v);
    printf("real package %s: %d level files, %u level names\n", path, n, t->n);
    CHECK(n > 1000 && t->n > 50);
    int fj = -1, tut = -1; for (uint32_t i = 0; i < t->n; i++) { if (!strcmp(t->name[i], "FJ_FortJoy_Main")) fj = (int)i; if (!strcmp(t->name[i], "TUT_Tutorial_A")) tut = (int)i; }
    CHECK(fj >= 0 && tut >= 0);
    int seen_fj = 0, ok = 1;
    for (int i = 0; i < n; i++) { if ((int)v[i].level == fj) seen_fj++; if (nrh_span_find(v, (uint32_t)n, v[i].off) != (int)v[i].level) ok = 0; if (i && v[i].off < v[i - 1].off) ok = 0; }
    CHECK(seen_fj > 100 && ok);
    if (g_meta) test_real_meta(t, g_meta, g_meta_want ? g_meta_want : "TUT_Tutorial_A");
    free(v); free(t); free(list);
}

static void test_vote(void) {
    nrh_vote v; nrh_vote_init(&v);
    uint64_t d[NRH_LEVELS] = {0};
    d[3] = 100 * 1024; CHECK(nrh_vote_step(&v, d, NRH_LEVELS) == 0 && v.cur == -1);   // below 256 KB
    d[3] = 300 * 1024; CHECK(nrh_vote_step(&v, d, NRH_LEVELS) == 1 && v.cur == 3);     // 50 + 300 KB
    memset(d, 0, sizeof d); d[3] = 4u << 20; nrh_vote_step(&v, d, NRH_LEVELS);       // the level load
    memset(d, 0, sizeof d); d[7] = 400 * 1024; CHECK(nrh_vote_step(&v, d, NRH_LEVELS) == 0 && v.cur == 3);   // a stray read of another level
    memset(d, 0, sizeof d); d[7] = 8u << 20; CHECK(nrh_vote_step(&v, d, NRH_LEVELS) == 1 && v.cur == 7);    // a new level load
    memset(d, 0, sizeof d); for (int i = 0; i < 40; i++) CHECK(nrh_vote_step(&v, d, NRH_LEVELS) == 0 && v.cur == 7);   // quiet: stays
    CHECK(v.score[7] == 0);
    d[2] = 200 * 1024; CHECK(nrh_vote_step(&v, d, NRH_LEVELS) == 0 && v.cur == 7);   // decayed current, small read: still below min_bytes
}

static void test_keys(void) {
    CHECK(nrh_key_id("62d683cc49953ad6") == 0x62d683cc49953ad6ull);
    CHECK(nrh_key_id("62D683CC49953AD6") == 0x62d683cc49953ad6ull);
    CHECK(nrh_key_id("0000000000000000") == 1);   // 0 = none
    const uint64_t a = nrh_key_id("F1.62d683cc49953ad6"), b = nrh_key_id("x.FadeBlending"), c = nrh_key_id("62d683cc49953ad");
    CHECK(a >> 63 && b >> 63 && c >> 63 && a != b && b != c);
    char hx[17]; nrh_key_hex(0x00ab00000000cdefull, hx); CHECK(!strcmp(hx, "00ab00000000cdef"));
}

static const char *namefn(uint64_t id, void *ctx) { (void)ctx; return id == nrh_key_id("F1.abc") ? "F1.abc" : NULL; }
static void test_agg(void) {
    nrh_agg a; CHECK(nrh_agg_init(&a, 100) == 0 && a.cap == 128);
    const nrh_prog g1[] = {{0x11, 0xa1, 3, 300, -1, -1}, {0x12, 0xa2, 1, 100, -1, -1}};
    const nrh_prog g2[] = {{0x11, 0xa1, 2, 0, -1, -1}, {0x13, 0xa3, 2, 0, -1, -1}};   // no vertices: the vertex share falls back to draws
    const nrh_prog gc[] = {{0, 0xc1, 1, 0, -1, -1}};
    const nrh_pass p[] = {
        {0, 2400, 1080, 37, 2, 1000.0, 4, 400, 2, g1},
        {0, 2400, 1080, 37, 2, 400.0, 4, 0, 2, g2},
        {0, 512, 512, 6, 1, 50.0, 0, 0, 0, NULL},   // a clear-only pass
        {1, 0, 0, 0, 0, 80.0, 0, 0, 1, gc},
    };
    nrh_agg_add(&a, 5, 1000, p, 4);
    nrh_agg_span(&a, 5, 1000, 1700.0);
    nrh_agg_add(&a, 5, 1000, p, 1);   // a second submission of the same frame
    nrh_agg_add(&a, 6, 2000, p + 1, 1);
    // totals
    CHECK(a.lt[5].frames == 1 && a.lt[5].passes == 4 && a.lt[5].cpasses == 1 && a.lt[5].draws == 12);
    CHECK(NEAR(a.lt[5].pass_us, 2450) && NEAR(a.lt[5].compute_us, 80) && NEAR(a.lt[5].span_us, 1700));
    CHECK(a.lt[6].frames == 1 && a.lt[6].passes == 1);
    // the sum of us_d over a level's rows = its passes' time
    double sd = 0, sv = 0; int rows5 = 0;
    for (uint32_t i = 0; i < a.cap; i++) if (a.rows[i].used && a.rows[i].k.level == 5) { sd += a.rows[i].us_d; sv += a.rows[i].us_v; rows5++; }
    CHECK(NEAR(sd, 2450 + 80) && NEAR(sv, 2450 + 80));
    CHECK(rows5 == 5);   // (11,a1) of passes 1 and 2 (same key: merged), (12,a2), (13,a3), the clear row, the compute row
    // the (11, a1) row: pass 1 twice (draw share 3/4 of 1000 each) + pass 2 (2/4 of 400)
    const nrh_row *r = NULL;
    for (uint32_t i = 0; i < a.cap; i++) if (a.rows[i].used && a.rows[i].k.level == 5 && a.rows[i].k.vs == 0x11) r = &a.rows[i];
    CHECK(r && r->frames == 1 && r->passes == 3 && r->draws == 8 && r->verts == 600);
    if (r) { CHECK(NEAR(r->us_d, 750 + 750 + 200)); CHECK(NEAR(r->us_v, 750 + 750 + 200)); CHECK(NEAR(r->us_p, 1000 + 1000 + 400)); }
    const nrh_row *c = NULL, *z = NULL;
    for (uint32_t i = 0; i < a.cap; i++) if (a.rows[i].used && a.rows[i].k.level == 5) { if (a.rows[i].k.kind == 1) c = &a.rows[i]; if (!a.rows[i].k.fs) z = &a.rows[i]; }
    CHECK(c && c->k.fs == 0xc1 && NEAR(c->us_d, 80) && c->k.vs == 0);
    CHECK(z && z->k.w == 512 && NEAR(z->us_d, 50) && z->k.fmt == 6);
    // vertex share differs from draw share in pass 1 (300 / 400 vs 3 / 4 -> equal here); pass with 1 / 3 split:
    const nrh_prog g3[] = {{0x21, 0xb1, 1, 900, -1, -1}, {0x22, 0xb2, 3, 100, -1, -1}};
    const nrh_pass p3 = {0, 100, 100, 1, 1, 100.0, 4, 1000, 2, g3};
    nrh_agg_add(&a, 7, 3000, &p3, 1);
    for (uint32_t i = 0; i < a.cap; i++) if (a.rows[i].used && a.rows[i].k.level == 7 && a.rows[i].k.vs == 0x21) { CHECK(NEAR(a.rows[i].us_d, 25)); CHECK(NEAR(a.rows[i].us_v, 90)); }
    // levels / top
    uint32_t lv[8]; CHECK(nrh_agg_levels(&a, lv, 8) == 3 && lv[0] == 5 && lv[1] == 6 && lv[2] == 7);
    uint32_t top[3]; const int k = nrh_agg_top(&a, 5, top, 3);
    CHECK(k == 3 && a.rows[top[0]].k.vs == 0x11 && a.rows[top[0]].us_d >= a.rows[top[1]].us_d && a.rows[top[1]].us_d >= a.rows[top[2]].us_d);
    CHECK(nrh_agg_top(&a, 5, top, 0) == 0);
    // lines
    char line[512];
    int n = nrh_fmt_row(line, sizeof line, a.rows + top[0], NULL, NULL);
    CHECK(n > 0 && line[n - 1] == '\n' && !strncmp(line, "A\tp\t2400x1080\t37\t2\t0000000000000011\t00000000000000a1\t1\t3\t8\t600\t1700\t1700\t2400\t1700\n", (size_t)n));
    n = nrh_fmt_row(line, 10, a.rows + top[0], NULL, NULL); CHECK(n == -1);
    nrh_row hr; memset(&hr, 0, sizeof hr); hr.k.fs = nrh_key_id("F1.abc"); hr.k.kind = 0; hr.passes = 1;
    n = nrh_fmt_row(line, sizeof line, &hr, namefn, NULL); CHECK(n > 0 && strstr(line, "\t-\tF1.abc\t"));
    n = nrh_fmt_block(line, sizeof line, "FJ_FortJoy_Main", &a.lt[5], 1700000000ull, 12.4);
    CHECK(n > 0 && !strcmp(line, "B\t1700000000\t12\tFJ_FortJoy_Main\t1\t4\t12\t1700\t2450\t80\n"));
    n = nrh_fmt_pass(line, sizeof line, 1000, 0, &p[0], NULL, NULL);
    CHECK(n > 0 && !strcmp(line, "P\t1000\t0\tp\t1000\t2400x1080\t37\t2\t4\t400\t0000000000000011/00000000000000a1/3/300,0000000000000012/00000000000000a2/1/100\n"));
    n = nrh_fmt_pass(line, sizeof line, 1000, 2, &p[2], NULL, NULL); CHECK(n > 0 && strstr(line, "\t0\t0\t-\n"));
    // reset
    nrh_agg_reset(&a); CHECK(a.n == 0 && nrh_agg_levels(&a, lv, 8) == 0);
    // a full table: new rows dropped (counted), existing rows still added to
    nrh_agg_add(&a, 1, 1, p, 1);
    for (uint32_t i = 0; i < 200; i++) { nrh_prog g = {i + 100, i + 100, 1, 1, -1, -1}; nrh_pass q = {0, 1, 1, 1, 1, 1.0, 1, 1, 1, &g}; nrh_agg_add(&a, 1, 2 + i, &q, 1); }
    CHECK(a.n == 96 && a.dropped == 106);   // 75 % of 128 rows: 2 + 94 kept, 106 dropped
    const uint32_t before = a.lt[1].frames; nrh_agg_add(&a, 1, 999, p, 1); CHECK(a.lt[1].frames == before + 1);
    int found = 0; for (uint32_t i = 0; i < a.cap; i++) if (a.rows[i].used && a.rows[i].k.vs == 0x11 && a.rows[i].passes == 2) found = 1;
    CHECK(found);
    nrh_agg_free(&a);
}

static void test_cost(void) {
    // cost-weighted split: known 40 (1 draw) and 10 (2 draws), one unknown (1 draw) at the known mean 20 -> 40 : 20 : 20
    const nrh_prog g[] = {{2, 2, 1, 0, 10, 30}, {4, 4, 1, 0, -1, -1}, {6, 6, 2, 0, 0, 10}};
    const nrh_pass p = {0, 64, 64, 1, 1, 800.0, 4, 0, 3, g};
    nrh_agg a; nrh_agg_init(&a, 64); nrh_agg_add(&a, 0, 1, &p, 1);
    double uc[4] = {0}, ud[4] = {0};
    for (uint32_t i = 0; i < a.cap; i++) if (a.rows[i].used) { const int j = (int)(a.rows[i].k.vs / 2); if (j >= 1 && j <= 3) { uc[j] = a.rows[i].us_c; ud[j] = a.rows[i].us_d; } }
    CHECK(NEAR(uc[1], 400) && NEAR(uc[2], 200) && NEAR(uc[3], 200));
    CHECK(NEAR(ud[1], 200) && NEAR(ud[2], 200) && NEAR(ud[3], 400));
    // nothing known: the draw share
    const nrh_prog h[] = {{2, 2, 1, 0, -1, -1}, {4, 4, 3, 0, -1, -1}};
    const nrh_pass q = {0, 64, 64, 1, 1, 400.0, 4, 0, 2, h};
    nrh_agg_reset(&a); nrh_agg_add(&a, 0, 1, &q, 1);
    for (uint32_t i = 0; i < a.cap; i++) if (a.rows[i].used) CHECK(NEAR(a.rows[i].us_c, a.rows[i].us_d));
    nrh_agg_free(&a);
    // the SPIR-V counter on a hand-made module
    uint32_t w[256]; size_t n = 0;
#define INS(op, ...) do { const uint32_t a_[] = {0, __VA_ARGS__}; const size_t k_ = sizeof a_ / 4; w[n] = (uint32_t)(k_ << 16 | (op)); memcpy(w + n + 1, a_ + 1, (k_ - 1) * 4); n += k_; } while (0)
#define INS0(op) do { w[n++] = 1u << 16 | (op); } while (0)
    w[n++] = 0x07230203; w[n++] = 0x10000; w[n++] = 0; w[n++] = 100; w[n++] = 0;
    { char imp[16] = {0}; memcpy(imp, "GLSL.std.450", 12); w[n++] = 6u << 16 | 11; w[n++] = 20; memcpy(w + n, imp, 16); n += 4; }
    INS(59, 50, 51, 2);           // %51 Uniform variable
    INS(59, 50, 52, 7);           // %52 Function variable
    INS(43, 10, 60, 0);           // %60 constant
    INS(54, 1, 2, 0, 3);          // OpFunction
    INS(248, 4);
    INS(65, 52, 70, 51, 60);      // constant index
    INS(65, 52, 71, 51, 80);      // dynamic index
    INS(61, 10, 72, 70);
    INS(61, 10, 73, 71);
    INS(61, 10, 74, 52);          // a Function-class load: not a UBO load
    INS(129, 10, 75, 72, 73);     // FAdd
    INS(12, 10, 76, 20, 31, 75);  // GLSL Sqrt: transcendental
    INS(12, 10, 77, 20, 40, 75, 76);   // GLSL FMax: alu
    INS(87, 11, 78, 90, 74);      // sample
    INS(124, 10, 79, 78);         // bitcast
    INS(247, 81, 0); INS(250, 79, 82, 83);   // selection merge + conditional branch
    INS0(253);
    INS0(56);
#undef INS
#undef INS0
    nrh_cost c;
    CHECK(nrh_spirv_cost(w, n, &c) == 0);
    CHECK(c.tex == 1 && c.trans == 1 && c.ubo == 2 && c.ubo_dyn == 1 && c.bitcast == 1 && c.branch == 1);
    CHECK(c.alu == 3);    // FAdd, FMax, bitcast
    CHECK(c.inst == 12);  // 2 chains, 3 loads, FAdd, 2 ext, sample, bitcast, branch, return (+ the merge is not counted, the label neither)
    CHECK(c.cost == 3 + 4 + 4 + 2 + 2);
    CHECK(nrh_spirv_cost(w, 3, &c) == -1);
    w[0] = 0; CHECK(nrh_spirv_cost(w, n, &c) == -1);
}

static void test_out(const char *dir) {
    nrh_out o; char p[700], q[700];
    snprintf(p, sizeof p, "%s/nr_heat_test.tsv", dir); snprintf(q, sizeof q, "%s/nr_heat_test.1.tsv", dir); remove(p); remove(q);
    CHECK(nrh_out_open(&o, dir, "nr_heat_test.tsv", 100) == 0 && o.cap == 4096 && o.bytes == 0);
    char buf[3000]; memset(buf, 'x', sizeof buf); buf[sizeof buf - 1] = '\n';
    CHECK(nrh_out_full(&o, 3000) == 0);   // an empty file always takes the write
    CHECK(nrh_out_write(&o, buf, 3000) == 1 && o.bytes == 3000);
    CHECK(nrh_out_full(&o, 1000) == 0 && nrh_out_full(&o, 1200) == 1);
    CHECK(nrh_out_rotate(&o) == 0 && o.bytes == 0 && o.rotations == 1);
    CHECK(nrh_out_write(&o, "H\n", 2) == 1);
    nrh_out_close(&o);
    FILE *f = fopen(q, "rb"); long s1 = -1; if (f) { fseek(f, 0, SEEK_END); s1 = ftell(f); fclose(f); }
    f = fopen(p, "rb"); long s2 = -1; if (f) { fseek(f, 0, SEEK_END); s2 = ftell(f); fclose(f); }
    CHECK(s1 == 3000 && s2 == 2);
    CHECK(nrh_out_open(&o, dir, "nr_heat_test.tsv", 1 << 20) == 0 && o.bytes == 2);   // appends
    nrh_out_close(&o); CHECK(nrh_out_write(&o, "x", 1) == 0);
    remove(p); remove(q);
    CHECK(nrh_out_open(&o, "/nonexistent-dir-nr-heat", "x.tsv", 1 << 20) == -1);
}

int main(int argc, char **argv) {
    test_lz4(); test_paths(); test_lspk(); test_save(); test_vote(); test_keys(); test_agg(); test_cost();
    if (argc > 2) { g_meta = argv[2]; g_meta_want = argc > 3 ? argv[3] : NULL; }   // [NR-BN] a decompressed meta.lsf of a real save + its level
    const char *tmp = getenv("TEMP"); if (!tmp) tmp = getenv("TMPDIR"); if (!tmp) tmp = "/tmp";
    test_out(tmp);
    if (argc > 1) test_real(argv[1]);
    printf("nr_heat_test: %d checks, %d failed - %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
