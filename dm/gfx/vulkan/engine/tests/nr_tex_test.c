// [dos2-nr05] NR-05: host test of the material-texture core (dm/gfx/nr/nr_tex.h, DM_NR_TEX).
//   1. nr_tex_layout = get_image_slow's DM_GFX_DESWZ_DIRECT staging layout (the loop below is a copy of xbg_game.cpp's, kept apart on
//      purpose) for 2D / mips / arrays / cubes / 3D / 1D, plain and BC / ASTC formats; refusals (multisample, linear, unknown format).
//   2. A prepared blob = nvn_deswizzle (the old, non-GOB function) of every subresource, byte for byte, on random storage.
//   3. The probe: deterministic, sees a change in a sampled range, covers every byte below 1 KB.
//   4. The table: create -> job -> READY -> take (probe-checked) -> drop; stale storage, a pending job cancelled by a take, gone with a
//      blob / during a job, the budget (a job waits, expire frees), format verdicts, the queue bound, slot reuse, the ASTC key.
//   5. [dos2-nr22] NR-22 (DM_NR_TEXSTREAM) with a fake engine sink: the engine is asked only for formats the translator took as it is
//      and never for an ASTC-cacheable texture while the cache is active; the deswizzle lands in the engine's staging (= nvn_deswizzle);
//      take_ts hands out object + staging once; every untaken end (stale, gone, WriteTexels, engine-dead, expired, cancelled while
//      working, a write during the job, slot reuse, NR-05's take, nr_tex_free) releases the object exactly once; ASTC objects hold
//      their bytes in the budget and leave the key served; a refusal falls back to the heap blob; no sink = NR-05 unchanged.
//   6. [dos2-nr22fix] DM_NR_TEXSTREAM=2 verify on the phone shapes of nr17_ts2 (820x820 BC1 10 levels etc., 214 of 709 red, "+8 B"):
//      the old path's DESWZ_DIRECT staging (a ring buffer with stale bytes, every subresource 16-byte aligned) against the worker's slice
//      (rounded up to 16, other stale bytes). Reproduces the red (one memcmp of the whole run differs: the alignment gaps after 8-byte
//      tail mips), shows every subresource equal (nr_tex_cmp_staged = PAD) and the old path's copy regions = the regions mode 1 records
//      (nr_tex_regions_match); a flipped subresource byte is DIFF at its offset, a flipped gap byte PAD, wrong lengths LAYOUT, a moved /
//      resized / pitched region no match. Plain, BC 16-byte, ASTC, cube, 3D and 1-D shapes too (no gaps = SAME).
// Build (Windows clang 21 or WSL gcc), from this folder:
//   clang -std=c11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -I../../../nvn/include nr_tex_test.c ../../../nr/nr_tex.c ../../../tex/nvn_deswizzle.c ../../../nvn/nvn_tex.c -o nr_tex_test
#include "../../../nr/nr_tex.h"
#include "../../../tex/nvn_deswizzle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(e) do { ++checks; if (!(e)) { printf("FAIL line %d: %s\n", __LINE__, #e); ++failures; } } while (0)

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static nvn_texdesc desc(int target, int fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels) {
    nvn_texdesc t; memset(&t, 0, sizeof t);
    t.target = target; t.format = fmt; t.width = w; t.height = h; t.depth = d; t.levels = levels; t.samples = 0;
    return t;
}
// xbg_game.cpp get_image_slow, DM_GFX_DESWZ_DIRECT=1 branch (Image fields from its creation code): the reference
typedef struct ref_sub { uint64_t begin, bytes; uint32_t pitch, level, layer, w, h, depth; } ref_sub;
static int ref_layout(const nvn_texdesc *d, const nvn_texlayout *L, ref_sub *out, uint64_t *total_out) {
    const uint32_t width = d->width, height = (d->target == 0 || d->target == 3) ? 1 : d->height, depthv = d->target == 2 ? d->depth : 1, levels = d->levels, layers = L->layers, bpp = L->bpb;
    int cbw = 1, cbh = 1; nvn_fmt_bpb(d->format, &cbw, &cbh); if (cbw < 1) cbw = 1; if (cbh < 1) cbh = 1;
    size_t total = 0; int n = 0;
    for (uint32_t layer = 0; layer < layers; layer++) for (uint32_t level = 0; level < levels; level++) {
        uint32_t w = width >> level ? width >> level : 1, h = height >> level ? height >> level : 1, depth = depthv >> level ? depthv >> level : 1;
        const uint32_t bx = (w + cbw - 1) / cbw, by = (h + cbh - 1) / cbh; const size_t begin = (total + 15) & ~(size_t)15, pitch = (size_t)bx * bpp; total = begin + pitch * by * depth;
        out[n++] = (ref_sub){begin, pitch * by * depth, (uint32_t)pitch, level, layer, w, h, depth};
    }
    *total_out = total;
    return n;
}

static void test_layout_and_blob(void) {
    static const struct { int target, fmt; uint32_t w, h, d, lv; int pkg; } k[] = {
        {1, 0x25, 256, 256, 1, 9, 0}, {1, 0x25, 300, 77, 1, 4, 0}, {1, 0x42, 412, 412, 1, 9, 6}, {1, 0x46, 204, 204, 1, 8, 5}, {1, 0x48, 52, 104, 1, 7, 0},
        {1, 0x4d, 128, 64, 1, 3, 0}, {1, 0x79, 100, 60, 1, 2, 0}, {1, 0x01, 33, 17, 1, 1, 0}, {1, 0x29, 64, 64, 1, 7, 0}, {4, 0x25, 64, 64, 4, 5, 0},
        {8, 0x46, 128, 128, 6, 8, 0}, {2, 0x25, 32, 32, 16, 4, 0}, {0, 0x25, 128, 1, 1, 1, 0}, {1, 0x2e, 16, 16, 1, 5, 0}, {1, 0x44, 1024, 512, 1, 11, 0},
    };
    int shapes = 0;
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) {
        nvn_texdesc d = desc(k[i].target, k[i].fmt, k[i].w, k[i].h, k[i].d, k[i].lv); d.pkg_block_h = k[i].pkg;
        nvn_texlayout L; nvn_tex_layout(&d, &L);
        nrt_sub sub[NRT_SUBS]; ref_sub ref[NRT_SUBS]; uint64_t tot = 0, rtot = 0;
        const int n = nr_tex_layout(&d, &L, sub, NRT_SUBS, &tot), rn = ref_layout(&d, &L, ref, &rtot);
        CHECK(n == rn && n > 0 && tot == rtot);
        for (int j = 0; j < n && j < rn; j++)
            CHECK(sub[j].off == ref[j].begin && sub[j].bytes == ref[j].bytes && sub[j].pitch == ref[j].pitch && sub[j].level == ref[j].level && sub[j].layer == ref[j].layer &&
                  sub[j].w == ref[j].w && sub[j].h == ref[j].h && sub[j].depth == ref[j].depth);
        // the blob of a job against nvn_deswizzle
        uint8_t *mem = (uint8_t *)malloc((size_t)L.size);
        for (uint64_t b = 0; b < L.size; b++) mem[b] = (uint8_t)rnd();
        nr_tex_core *c = nr_tex_new(256u << 20, 0, 256u << 20);
        nrt_src s; memset(&s, 0, sizeof s); s.d = d; s.L = L; s.mem = mem; s.serial = 100 + i; s.src = 4;
        CHECK(nr_tex_create(c, &s, 0, 1, 1) == NRT_WHY_OK);
        static nrt_job j; CHECK(nr_tex_job_begin(c, &j) == 1); nr_tex_job_run(&j, NULL); CHECK(j.ok && j.blob); if (!j.ok) printf("  shape %u target %d fmt %#x: job why %s\n", i, k[i].target, k[i].fmt, nr_tex_why_name(j.why)); nr_tex_job_end(c, &j, 2);
        uint64_t bytes = 0, probe = 0; int why = -1;
        uint8_t *blob = nr_tex_take(c, s.serial, mem, L.size, &bytes, &probe, &why);
        CHECK(blob && why == NRT_WHY_OK && bytes == tot && probe == nr_tex_probe(mem, L.size));
        for (int q = 0; blob && q < n; q++) {
            uint8_t *want = (uint8_t *)malloc((size_t)sub[q].bytes);
            CHECK(nvn_deswizzle(&d, sub[q].level, sub[q].layer, mem, want, sub[q].pitch) == 0);
            CHECK(memcmp(want, blob + sub[q].off, (size_t)sub[q].bytes) == 0);
            free(want);
        }
        CHECK(c->st.live_bytes == bytes);
        nr_tex_drop(c, blob, bytes);
        CHECK(c->st.live_bytes == 0);
        nr_tex_free(c); free(mem); shapes++;
    }
    // refusals
    nvn_texdesc d = desc(1, 0x25, 64, 64, 1, 1); nvn_texlayout L; nvn_tex_layout(&d, &L); nrt_sub sub[NRT_SUBS]; uint64_t t;
    d.samples = 4; CHECK(nr_tex_layout(&d, &L, sub, NRT_SUBS, &t) == -1); d.samples = 0;
    d.target = 5; CHECK(nr_tex_layout(&d, &L, sub, NRT_SUBS, &t) == -1); d.target = 1;
    d.flags = 0x10; CHECK(nr_tex_layout(&d, &L, sub, NRT_SUBS, &t) == -1); d.flags = 0;
    d.format = 0xff; CHECK(nr_tex_layout(&d, &L, sub, NRT_SUBS, &t) == -1); d.format = 0x25;
    CHECK(nr_tex_layout(&d, &L, sub, 0, &t) == -1);
    printf("layout + blob: %d shapes match the DESWZ_DIRECT staging and nvn_deswizzle\n", shapes);
}

static void test_probe(void) {
    uint8_t *m = (uint8_t *)malloc(1 << 20);
    for (int i = 0; i < 1 << 20; i++) m[i] = (uint8_t)rnd();
    const uint64_t p = nr_tex_probe(m, 1 << 20);
    CHECK(p && p == nr_tex_probe(m, 1 << 20));
    m[0] ^= 1; CHECK(nr_tex_probe(m, 1 << 20) != p); m[0] ^= 1;
    m[(1 << 20) - 1] ^= 1; CHECK(nr_tex_probe(m, 1 << 20) != p); m[(1 << 20) - 1] ^= 1;
    const uint64_t mid = ((uint64_t)(1 << 20) - 32) * 16 / 32 + 5; m[mid] ^= 0x80; CHECK(nr_tex_probe(m, 1 << 20) != p); m[mid] ^= 0x80;
    CHECK(nr_tex_probe(m, 1 << 20) == p);
    for (int i = 0; i < 1000; i++) { const uint64_t q = nr_tex_probe(m, 1000); m[i] ^= 4; CHECK(nr_tex_probe(m, 1000) != q); m[i] ^= 4; }
    CHECK(nr_tex_probe(NULL, 10) == 0 && nr_tex_probe(m, 0) == 0);
    CHECK(nr_tex_hash(m, 1 << 20) != nr_tex_hash(m, (1 << 20) - 1));
    free(m);
}

static int fake_key(const void *mem, uint64_t size, uint32_t *crc, uint32_t *adler) { *crc = (uint32_t)nr_tex_hash(mem, size); *adler = (uint32_t)size; return 0; }

static void test_table(void) {
    nvn_texdesc d = desc(1, 0x25, 256, 256, 1, 1); nvn_texlayout L; nvn_tex_layout(&d, &L);   // 256 KB storage
    uint8_t *mem = (uint8_t *)calloc(8, (size_t)L.size);
    nr_tex_core *c = nr_tex_new(600u << 10, 64u << 10, 1u << 20);   // budget: two blobs
    nrt_src s; memset(&s, 0, sizeof s); s.d = d; s.L = L; s.src = 4;
    static nrt_job j; uint64_t bytes, probe; int why; nrt_info info;
    // 1: ready, taken
    s.serial = 1; s.mem = mem; CHECK(nr_tex_create(c, &s, 77, 10, 1) == NRT_WHY_OK);
    CHECK(nr_tex_find(c, 1, &info) && info.alive && info.state == NRT_ST_QUEUED && info.hash0 == 77);
    CHECK(nr_tex_job_begin(c, &j) == 1); nr_tex_job_run(&j, NULL); nr_tex_job_end(c, &j, 11);
    CHECK(nr_tex_find(c, 1, &info) && info.state == NRT_ST_READY);
    uint8_t *b = nr_tex_take(c, 1, mem, L.size, &bytes, &probe, &why); CHECK(b && why == NRT_WHY_OK);
    CHECK(!nr_tex_take(c, 1, mem, L.size, &bytes, &probe, &why) && why == NRT_WHY_NONE);   // only once
    nr_tex_drop(c, b, bytes);
    // 2: storage changed after the job -> stale, the blob is freed
    s.serial = 2; s.mem = mem + L.size; CHECK(nr_tex_create(c, &s, 0, 20, 1) == NRT_WHY_OK);
    CHECK(nr_tex_job_begin(c, &j) == 1); nr_tex_job_run(&j, NULL); nr_tex_job_end(c, &j, 21);
    mem[L.size] = 9;
    CHECK(!nr_tex_take(c, 2, mem + L.size, L.size, &bytes, &probe, &why) && why == NRT_WHY_STALE && c->st.live_bytes == 0);
    // 3: a take while queued cancels; the job is skipped
    s.serial = 3; s.mem = mem + 2 * L.size; CHECK(nr_tex_create(c, &s, 0, 30, 1) == NRT_WHY_OK);
    CHECK(!nr_tex_take(c, 3, s.mem, L.size, &bytes, &probe, &why) && why == NRT_WHY_PENDING);
    CHECK(nr_tex_job_begin(c, &j) == 0 && c->qhead == c->qtail);
    // 4: a take while working cancels; the job's blob is discarded at its end
    s.serial = 4; s.mem = mem + 3 * L.size; CHECK(nr_tex_create(c, &s, 0, 40, 1) == NRT_WHY_OK);
    CHECK(nr_tex_job_begin(c, &j) == 1); CHECK(!nr_tex_take(c, 4, s.mem, L.size, &bytes, &probe, &why) && why == NRT_WHY_PENDING);
    nr_tex_job_run(&j, NULL); nr_tex_job_end(c, &j, 41); CHECK(c->st.live_bytes == 0 && nr_tex_find(c, 4, &info) && info.state == NRT_ST_NONE);
    // 5: gone during the job and gone with a ready blob
    s.serial = 5; s.mem = mem + 4 * L.size; nr_tex_create(c, &s, 0, 50, 1);
    CHECK(nr_tex_job_begin(c, &j) == 1); nr_tex_gone(c, 5); nr_tex_job_run(&j, NULL); nr_tex_job_end(c, &j, 51); CHECK(c->st.live_bytes == 0);
    CHECK(nr_tex_find(c, 5, &info) && !info.alive && !nr_tex_take(c, 5, s.mem, L.size, &bytes, &probe, &why) && why == NRT_WHY_UNKNOWN);
    s.serial = 6; s.mem = mem + 5 * L.size; nr_tex_create(c, &s, 0, 60, 1); CHECK(nr_tex_job_begin(c, &j) == 1); nr_tex_job_run(&j, NULL); nr_tex_job_end(c, &j, 61);
    CHECK(c->st.live_bytes > 0); nr_tex_gone(c, 6); CHECK(c->st.live_bytes == 0 && c->st.gone_ready == 1);
    // 6: the budget: two blobs fit, the third job waits until one expires
    for (uint64_t k = 7; k <= 9; k++) { s.serial = k; s.mem = mem + (k - 7) * L.size; CHECK(nr_tex_create(c, &s, 0, 100 * k, 1) == NRT_WHY_OK); }
    for (int r = 0; r < 2; r++) { CHECK(nr_tex_job_begin(c, &j) == 1); nr_tex_job_run(&j, NULL); nr_tex_job_end(c, &j, 0); }
    CHECK(nr_tex_job_begin(c, &j) == 0);                       // waits for the budget
    nr_tex_expire(c, 750, 50);                                 // 7 (t 700) expired, 8 (t 800) not yet
    CHECK(c->st.expired == 1 && nr_tex_find(c, 7, &info) && info.state == NRT_ST_NONE && info.why == NRT_WHY_EXPIRED);
    CHECK(nr_tex_job_begin(c, &j) == 1); nr_tex_job_run(&j, NULL); nr_tex_job_end(c, &j, 0);
    CHECK(nr_tex_find(c, 9, &info) && info.state == NRT_ST_READY);
    nr_tex_gone(c, 8); nr_tex_gone(c, 9); CHECK(c->st.live_bytes == 0);
    // 7: size bounds and the format verdict
    nvn_texdesc ds = desc(1, 0x25, 16, 16, 1, 1); nvn_texlayout Ls; nvn_tex_layout(&ds, &Ls); nrt_src small = s; small.d = ds; small.L = Ls; small.serial = 20;
    CHECK(nr_tex_create(c, &small, 0, 1, 1) == NRT_WHY_SMALL);
    nvn_texdesc db = desc(1, 0x25, 1024, 1024, 1, 1); nvn_texlayout Lb; nvn_tex_layout(&db, &Lb); nrt_src big = s; big.d = db; big.L = Lb; big.serial = 21;
    CHECK(nr_tex_create(c, &big, 0, 1, 1) == NRT_WHY_BIG);
    nr_tex_verdict(c, 0x25, 0); s.serial = 22; s.mem = mem; CHECK(nr_tex_create(c, &s, 0, 1, 1) == NRT_WHY_FMT);
    CHECK(!nr_tex_take(c, 22, mem, L.size, &bytes, &probe, &why) && why == NRT_WHY_FMT);
    s.d.format = 0x26; s.serial = 23; CHECK(nr_tex_create(c, &s, 0, 1, 1) == NRT_WHY_OK);   // another format still prepares
    CHECK(nr_tex_job_begin(c, &j) == 1); nr_tex_job_run(&j, NULL); nr_tex_job_end(c, &j, 0); nr_tex_gone(c, 23);
    // 7b: WriteTexels into a live texture: a READY blob and a queued job are dropped, the count moves; unknown serials are ignored
    nr_tex_core *cw = nr_tex_new(8u << 20, 0, 8u << 20); nrt_src sw = s; sw.d.format = 0x25; sw.mem = mem;
    sw.serial = 60; nr_tex_create(cw, &sw, 0, 1, 1); CHECK(nr_tex_job_begin(cw, &j) == 1); nr_tex_job_run(&j, NULL); nr_tex_job_end(cw, &j, 2);
    CHECK(cw->st.live_bytes > 0 && nr_tex_written(cw, 60) == 1 && cw->st.live_bytes == 0 && nr_tex_find(cw, 60, &info) && info.writes == 1 && info.state == NRT_ST_NONE);
    CHECK(!nr_tex_take(cw, 60, mem, L.size, &bytes, &probe, &why) && why == NRT_WHY_STALE);
    sw.serial = 61; nr_tex_create(cw, &sw, 0, 1, 1); CHECK(nr_tex_written(cw, 61) == 1 && nr_tex_job_begin(cw, &j) == 0);   // the queued job is skipped
    sw.serial = 62; nr_tex_create(cw, &sw, 0, 1, 1); CHECK(nr_tex_job_begin(cw, &j) == 1); CHECK(nr_tex_written(cw, 62) == 1);
    nr_tex_job_run(&j, NULL); nr_tex_job_end(cw, &j, 3); CHECK(cw->st.live_bytes == 0 && nr_tex_find(cw, 62, &info) && info.state == NRT_ST_NONE);   // a running job is discarded
    CHECK(nr_tex_written(cw, 999) == 0 && cw->st.written == 3);
    nr_tex_gone(cw, 60); CHECK(nr_tex_written(cw, 60) == 0);
    // 7c: the engine's deferred destroy names the NVN object: the entry is engine-dead (blob dropped), once; other objects are ignored
    sw.serial = 70; sw.obj = 0x7000; nr_tex_create(cw, &sw, 0, 1, 1); CHECK(nr_tex_job_begin(cw, &j) == 1); nr_tex_job_run(&j, NULL); nr_tex_job_end(cw, &j, 2);
    CHECK(cw->st.live_bytes > 0 && nr_tex_engine_gone(cw, 0x7000) == 1 && cw->st.live_bytes == 0 && nr_tex_find(cw, 70, &info) && info.engine_dead && info.alive);
    CHECK(nr_tex_engine_gone(cw, 0x7000) == 0 && nr_tex_engine_gone(cw, 0x7008) == 0 && nr_tex_engine_gone(cw, 0) == 0 && cw->st.engine_gone == 1);
    sw.serial = 71; sw.obj = 0x7000; nr_tex_create(cw, &sw, 0, 1, 0);   // the engine reuses the object for a new texture: a new, live entry
    CHECK(nr_tex_find(cw, 71, &info) && !info.engine_dead && nr_tex_engine_gone(cw, 0x7000) == 1 && nr_tex_find(cw, 71, &info) && info.engine_dead);
    nr_tex_free(cw);
    // 8: queue bound, no job wanted
    s.serial = 30; CHECK(nr_tex_create(c, &s, 0, 1, 0) == NRT_WHY_NONE);
    c->qtail = c->qhead + NRT_QUEUE; s.serial = 31; CHECK(nr_tex_create(c, &s, 0, 1, 1) == NRT_WHY_QUEUE); c->qtail = c->qhead;
    // 9: slot reuse 32768 serials later
    s.serial = 40; nr_tex_create(c, &s, 0, 1, 1); CHECK(nr_tex_job_begin(c, &j) == 1); nr_tex_job_run(&j, NULL); nr_tex_job_end(c, &j, 0);
    s.serial = 40 + NRT_CAP; nr_tex_create(c, &s, 0, 1, 0);
    CHECK(c->st.wrapped >= 1 && c->st.live_bytes == 0 && !nr_tex_find(c, 40, &info) && nr_tex_find(c, 40 + NRT_CAP, &info));
    nr_tex_free(c);
    // 10: the ASTC key (BC1 2D, one layer): computed by the job, served while the probe holds, never for a changed storage
    c = nr_tex_new(8u << 20, 0, 8u << 20); c->want_key = 1;
    nvn_texdesc dk = desc(1, 0x42, 256, 256, 1, 1); nvn_texlayout Lk; nvn_tex_layout(&dk, &Lk); nrt_src sk = s; sk.d = dk; sk.L = Lk; sk.serial = 50; sk.mem = mem;
    nr_tex_verdict(c, 0x42, 0);                          // the cache decides BC1 on this device: no blob, but the key
    CHECK(nr_tex_create(c, &sk, 0, 1, 1) == NRT_WHY_OK);
    CHECK(nr_tex_job_begin(c, &j) == 1 && !j.want_blob && j.want_key); nr_tex_job_run(&j, fake_key); nr_tex_job_end(c, &j, 2);
    uint32_t crc = 0, ad = 0, ec, ea; fake_key(mem, Lk.size, &ec, &ea);
    CHECK(nr_tex_key(c, 50, mem, Lk.size, &crc, &ad) == 1 && crc == ec && ad == ea && c->st.live_bytes == 0);
    mem[0] ^= 0xff; CHECK(nr_tex_key(c, 50, mem, Lk.size, &crc, &ad) == 0 && c->st.key_stale == 1); mem[0] ^= 0xff;
    CHECK(nr_tex_key(c, 50, mem, Lk.size, &crc, &ad) == 0);   // dropped for good
    char line[1200]; CHECK(nr_tex_summary(c, line, sizeof line) > 0); printf("summary: %s\n", line);
    nr_tex_free(c); free(mem);
}

// NR-22 (DM_NR_TEXSTREAM): the core with a sink. A fake engine object = its own staging (malloc) + a magic; made / released are counted,
// a double release or a release of a taken object fails the test.
typedef struct fake_obj { uint32_t magic; int kind; uint64_t bytes; uint8_t *staging; int released; } fake_obj;
static int fk_made, fk_astc, fk_released, fk_refuse, fk_astc_have, fk_bad;
static void *fk_make(void *ctx, const nrt_src *s, uint64_t bytes, uint8_t **dst) {
    (void)ctx; (void)s;
    if (fk_refuse) return NULL;
    fake_obj *o = (fake_obj *)calloc(1, sizeof *o); o->magic = 0x51ea; o->kind = NRT_TS_DESW; o->bytes = bytes; o->staging = (uint8_t *)malloc((size_t)bytes);
    memset(o->staging, 0xcd, (size_t)bytes); *dst = o->staging; fk_made++; return o;
}
static void *fk_make_astc(void *ctx, const nrt_src *s, uint32_t crc, uint32_t adler, uint64_t *bytes) {
    (void)ctx; (void)crc; (void)adler;
    if (!fk_astc_have) return NULL;
    fake_obj *o = (fake_obj *)calloc(1, sizeof *o); o->magic = 0x51ea; o->kind = NRT_TS_ASTC; o->bytes = s->L.size * 2; o->staging = (uint8_t *)malloc((size_t)o->bytes);
    *bytes = o->bytes; fk_astc++; return o;
}
static void fk_release(void *ctx, void *p) {
    (void)ctx; fake_obj *o = (fake_obj *)p;
    if (!o || o->magic != 0x51ea || o->released) { fk_bad++; return; }
    o->released = 1; fk_released++; free(o->staging); o->staging = NULL;   // the struct leaks on purpose: a second release is caught
}
static int fk_key_write;   // the key function writes the storage (a write while the worker reads)
static int fk_key(const void *mem, uint64_t size, uint32_t *crc, uint32_t *adler) { if (fk_key_write) ((uint8_t *)mem)[size / 2] ^= 0x5a; return fake_key(mem, size, crc, adler); }

static void test_texstream(void) {
    const nrt_sink sink = {fk_make, fk_make_astc, fk_release, NULL};
    nvn_texdesc d = desc(1, 0x25, 128, 128, 1, 8); nvn_texlayout L; nvn_tex_layout(&d, &L);
    uint8_t *mem = (uint8_t *)malloc((size_t)L.size * 16); for (uint64_t b = 0; b < L.size * 16; b++) mem[b] = (uint8_t)rnd();
    nrt_sub sub[NRT_SUBS]; uint64_t tot = 0; const int nsub = nr_tex_layout(&d, &L, sub, NRT_SUBS, &tot); CHECK(nsub == 8);
    nr_tex_core *c = nr_tex_new(64u << 20, 0, 16u << 20); nr_tex_set_sink(c, &sink);
    nrt_src s; memset(&s, 0, sizeof s); s.d = d; s.L = L; s.src = 4;
    static nrt_job j; uint8_t *blob = NULL; void *ts = NULL; int kind = 0, why = 0; uint64_t bytes = 0, probe = 0; nrt_info info;
#define RUN(core, ts_at) do { CHECK(nr_tex_job_begin(core, &j) == 1); nr_tex_job_run(&j, fk_key); nr_tex_job_end(core, &j, ts_at); } while (0)
    // 1: the format is not proven yet (no verdict): a heap blob, the engine is not asked
    s.serial = 1; s.mem = mem; CHECK(nr_tex_create(c, &s, 0, 1, 1) == NRT_WHY_OK); RUN(c, 2);
    CHECK(fk_made == 0 && nr_tex_take_ts(c, 1, mem, L.size, &blob, &ts, &kind, &bytes, &probe, &why) == 1 && blob && !ts && kind == NRT_TS_NONE && bytes == tot);
    nr_tex_drop(c, blob, bytes); CHECK(c->st.live_bytes == 0);
    // 2: proven: the engine's object, the deswizzle lands in its staging = nvn_deswizzle, byte for byte; take hands out object + staging
    nr_tex_verdict(c, 0x25, 1);
    s.serial = 2; s.mem = mem + L.size; nr_tex_create(c, &s, 0, 3, 1); RUN(c, 4);
    CHECK(fk_made == 1 && nr_tex_find(c, 2, &info) && info.state == NRT_ST_READY && c->st.live_bytes == tot && c->st.ts_made == 1);
    CHECK(nr_tex_take_ts(c, 2, s.mem, L.size, &blob, &ts, &kind, &bytes, &probe, &why) == 1 && ts && kind == NRT_TS_DESW && blob == ((fake_obj *)ts)->staging && bytes == tot && probe == nr_tex_probe(s.mem, L.size));
    for (int q = 0; ts && q < nsub; q++) { uint8_t *want = (uint8_t *)malloc((size_t)sub[q].bytes); CHECK(nvn_deswizzle(&d, sub[q].level, sub[q].layer, s.mem, want, sub[q].pitch) == 0);
        CHECK(memcmp(want, blob + sub[q].off, (size_t)sub[q].bytes) == 0); free(want); }
    CHECK(c->st.live_bytes == tot && fk_released == 0); nr_tex_unhold(c, bytes); CHECK(c->st.live_bytes == 0 && c->st.ts_taken == 1);
    fk_release(NULL, ts);   // the engine's own disposal (counted here as a release of a taken object, once)
    CHECK(!nr_tex_take_ts(c, 2, s.mem, L.size, &blob, &ts, &kind, &bytes, &probe, &why) && why == NRT_WHY_NONE && !ts && !blob);   // only once
    // 3: every untaken end goes back to the engine exactly once: stale probe, gone, WriteTexels, engine-dead, expired, cancelled while working,
    //    a write during the job, a slot reuse, the old take (no ts), nr_tex_free
    int rel0 = fk_released;
    s.serial = 3; s.mem = mem + 2 * L.size; nr_tex_create(c, &s, 0, 5, 1); RUN(c, 6); mem[2 * L.size + 7] ^= 1;
    CHECK(!nr_tex_take_ts(c, 3, s.mem, L.size, &blob, &ts, &kind, &bytes, &probe, &why) && why == NRT_WHY_STALE && fk_released == rel0 + 1);
    s.serial = 4; s.mem = mem + 3 * L.size; nr_tex_create(c, &s, 0, 7, 1); RUN(c, 8); nr_tex_gone(c, 4); CHECK(fk_released == rel0 + 2);
    s.serial = 5; s.mem = mem + 4 * L.size; nr_tex_create(c, &s, 0, 9, 1); RUN(c, 10); CHECK(nr_tex_written(c, 5) == 1 && fk_released == rel0 + 3);
    s.serial = 6; s.mem = mem + 5 * L.size; s.obj = 0x600; nr_tex_create(c, &s, 0, 11, 1); RUN(c, 12); CHECK(nr_tex_engine_gone(c, 0x600) == 1 && fk_released == rel0 + 4); s.obj = 0;
    s.serial = 7; s.mem = mem + 6 * L.size; nr_tex_create(c, &s, 0, 100, 1); RUN(c, 0); nr_tex_expire(c, 1000, 50); CHECK(fk_released == rel0 + 5 && nr_tex_find(c, 7, &info) && info.why == NRT_WHY_EXPIRED);
    s.serial = 8; s.mem = mem + 7 * L.size; nr_tex_create(c, &s, 0, 13, 1); CHECK(nr_tex_job_begin(c, &j) == 1);
    CHECK(!nr_tex_take_ts(c, 8, s.mem, L.size, &blob, &ts, &kind, &bytes, &probe, &why) && why == NRT_WHY_PENDING);
    nr_tex_job_run(&j, fk_key); CHECK(j.ts != NULL); nr_tex_job_end(c, &j, 14); CHECK(fk_released == rel0 + 6);   // made, then discarded at the end
    c->want_key = 0;
    s.serial = 9; s.mem = mem + 8 * L.size; nr_tex_create(c, &s, 0, 15, 1);
    CHECK(c->st.live_bytes == 0);
    s.serial = 9 + NRT_CAP; nr_tex_create(c, &s, 0, 16, 0); CHECK(c->qhead != c->qtail);   // a newer serial in the slot of the queued 9
    CHECK(nr_tex_job_begin(c, &j) == 0);   // 9's job is skipped (not its slot any more)
    s.serial = 10; s.mem = mem + 9 * L.size; nr_tex_create(c, &s, 0, 17, 1); RUN(c, 18);
    s.serial = 10 + NRT_CAP; nr_tex_create(c, &s, 0, 19, 0); CHECK(fk_released == rel0 + 7);   // a READY object evicted by slot reuse
    s.serial = 11; s.mem = mem + 10 * L.size; nr_tex_create(c, &s, 0, 20, 1); RUN(c, 21);
    CHECK(!nr_tex_take(c, 11, s.mem, L.size, &bytes, &probe, &why) && why == NRT_WHY_NONE && fk_released == rel0 + 8);   // NR-05's take cannot hold an object
    // a write while the worker reads: the job's object goes back, nothing READY
    c->want_key = 1; nvn_texdesc dk = desc(1, 0x42, 128, 128, 1, 1); nvn_texlayout Lk; nvn_tex_layout(&dk, &Lk);
    nrt_src sk = s; sk.d = dk; sk.L = Lk; sk.serial = 12; sk.mem = mem + 11 * L.size; nr_tex_verdict(c, 0x42, 1); fk_key_write = 1;
    nr_tex_create(c, &sk, 0, 22, 1); CHECK(nr_tex_job_begin(c, &j) == 1 && j.want_key && j.want_blob); nr_tex_job_run(&j, fk_key);
    CHECK(!j.ok && j.why == NRT_WHY_CHANGING && !j.ts && !j.blob); nr_tex_job_end(c, &j, 23); fk_key_write = 0;
    CHECK(c->st.live_bytes == 0 && nr_tex_find(c, 12, &info) && info.state == NRT_ST_NONE);
    // 4: a cacheable texture with the cache active is never made as it is (the ASTC route); an entry -> the ASTC object, no blob, its bytes held
    int made0 = fk_made;
    sk.serial = 13; nr_tex_create(c, &sk, 0, 24, 1); RUN(c, 25);   // no entry: key only, heap blob of a format the verdict allows
    CHECK(fk_made == made0 && fk_astc == 0 && nr_tex_find(c, 13, &info) && info.have_key && info.state == NRT_ST_READY);
    CHECK(nr_tex_take_ts(c, 13, sk.mem, Lk.size, &blob, &ts, &kind, &bytes, &probe, &why) == 1 && blob && !ts); nr_tex_drop(c, blob, bytes);
    fk_astc_have = 1; sk.serial = 14; nr_tex_create(c, &sk, 0, 26, 1); RUN(c, 27);
    CHECK(fk_made == made0 && fk_astc == 1 && c->st.ts_astc == 1 && nr_tex_find(c, 14, &info) && info.have_key && info.state == NRT_ST_READY && c->st.live_bytes == Lk.size * 2);
    uint32_t crc, ad; CHECK(nr_tex_key(c, 14, sk.mem, Lk.size, &crc, &ad) == 1);   // the key is still served (the translator's lookup)
    CHECK(nr_tex_take_ts(c, 14, sk.mem, Lk.size, &blob, &ts, &kind, &bytes, &probe, &why) == 1 && ts && kind == NRT_TS_ASTC && !blob && bytes == Lk.size * 2);
    nr_tex_unhold(c, bytes); fk_release(NULL, ts); CHECK(c->st.live_bytes == 0);
    CHECK(nr_tex_key(c, 14, sk.mem, Lk.size, &crc, &ad) == 1);   // taking the object leaves the key
    fk_astc_have = 0; c->want_key = 0;
    // 5: the engine refuses (cap / not ready): the heap blob, counted
    fk_refuse = 1; s.serial = 15; s.mem = mem + 12 * L.size; nr_tex_create(c, &s, 0, 28, 1); RUN(c, 29); fk_refuse = 0;
    CHECK(c->st.ts_refused == 1 && nr_tex_take_ts(c, 15, s.mem, L.size, &blob, &ts, &kind, &bytes, &probe, &why) == 1 && blob && !ts); nr_tex_drop(c, blob, bytes);
    // 6: nr_tex_free gives back what is still READY
    s.serial = 16; s.mem = mem + 13 * L.size; nr_tex_create(c, &s, 0, 30, 1); RUN(c, 31);
    const int before_free = fk_released; char line[1400]; CHECK(nr_tex_summary(c, line, sizeof line) > 0 && strstr(line, "texstream: prepared images")); printf("summary: %s\n", line);
    nr_tex_free(c); CHECK(fk_released == before_free + 1);
    CHECK(fk_bad == 0);
    // 7: without a sink nothing of it (NR-05 exactly): heap blobs, no texstream words in the summary
    c = nr_tex_new(64u << 20, 0, 16u << 20); nr_tex_verdict(c, 0x25, 1); s.serial = 17; s.mem = mem; nr_tex_create(c, &s, 0, 1, 1); RUN(c, 2);
    CHECK(nr_tex_take_ts(c, 17, mem, L.size, &blob, &ts, &kind, &bytes, &probe, &why) == 1 && blob && !ts && c->st.ts_made == 0); nr_tex_drop(c, blob, bytes);
    CHECK(nr_tex_summary(c, line, sizeof line) > 0 && !strstr(line, "texstream"));
    nr_tex_free(c); free(mem);
#undef RUN
}


// ---- 6: [dos2-nr22fix] the DM_NR_TEXSTREAM=2 compare (phone nr17_ts2: "the worker's deswizzled staging (450032 B) differs from the old path's (450024 B)")
static void *fv_make(void *ctx, const nrt_src *s, uint64_t bytes, uint8_t **dst) {   // xbg_game.cpp nrts::slice: the length rounded up to 16, stale chunk bytes
    (void)ctx; (void)s;
    fake_obj *o = (fake_obj *)calloc(1, sizeof *o); o->magic = 0x51ea; o->kind = NRT_TS_DESW; o->bytes = (bytes + 15) & ~(uint64_t)15; o->staging = (uint8_t *)malloc((size_t)o->bytes);
    memset(o->staging, 0x5a, (size_t)o->bytes); *dst = o->staging; return o;
}
static void fv_release(void *ctx, void *p) { (void)ctx; fake_obj *o = (fake_obj *)p; if (o) { free(o->staging); free(o); } }
static void test_texstream_verify(void) {
    static const struct { int target, fmt; uint32_t w, h, d, lv; uint64_t phone_bytes, phone_slice; } k[] = {
        {1, 0x42, 820, 820, 1, 10, 450024, 450032}, {1, 0x49, 820, 820, 1, 10, 450024, 450032}, {1, 0x42, 52, 52, 1, 6, 1944, 1952}, {1, 0x46, 52, 52, 1, 6, 1944, 1952},
        {1, 0x49, 204, 204, 1, 8, 28120, 28128}, {1, 0x42, 204, 204, 1, 7, 28104, 28112}, {1, 0x42, 412, 412, 1, 9, 113816, 113824}, {1, 0x49, 412, 412, 1, 9, 113816, 113824},
        {1, 0x42, 412, 412, 1, 8, 113800, 113808}, {1, 0x46, 104, 104, 1, 6, 7336, 7344}, {1, 0x42, 412, 1640, 1, 10, 451896, 451904},
        // not seen red on the phone: 16-byte blocks, plain, ASTC, cube / array / 3D / 1-D, odd plain tails
        {1, 0x44, 1024, 512, 1, 11, 0, 0}, {1, 0x48, 52, 104, 1, 7, 0, 0}, {1, 0x25, 256, 256, 1, 9, 0, 0}, {1, 0x25, 300, 77, 1, 4, 0, 0}, {1, 0x25, 8, 2, 1, 4, 0, 0},
        {1, 0x79, 100, 60, 1, 2, 0, 0}, {8, 0x46, 128, 128, 6, 8, 0, 0}, {4, 0x25, 64, 64, 4, 5, 0, 0}, {2, 0x25, 32, 32, 16, 4, 0, 0}, {0, 0x25, 128, 1, 1, 8, 0, 0},
        {1, 0x29, 64, 64, 1, 7, 0, 0}, {1, 0x01, 33, 17, 1, 1, 0, 0},
    };
    const nrt_sink sink = {fv_make, NULL, fv_release, NULL};
    int red_old = 0, red_new = 0, pads = 0, sames = 0, gapped = 0, phone = 0;
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) {
        nvn_texdesc d = desc(k[i].target, k[i].fmt, k[i].w, k[i].h, k[i].d, k[i].lv); nvn_texlayout L; nvn_tex_layout(&d, &L);
        ref_sub ref[NRT_SUBS]; uint64_t tot = 0; const int n = ref_layout(&d, &L, ref, &tot);
        if (k[i].phone_bytes) { CHECK(tot == k[i].phone_bytes); phone++; }   // the old path's length on the phone
        int gap = 0; for (int q = 0; q + 1 < n; q++) if (ref[q].begin + ref[q].bytes != ref[q + 1].begin) gap = 1;
        gapped += gap;
        uint8_t *mem = (uint8_t *)malloc((size_t)L.size); for (uint64_t b = 0; b < L.size; b++) mem[b] = (uint8_t)rnd();
        // the old path (get_image_slow DM_GFX_DESWZ_DIRECT=1): stream_buffer(total) with stale ring bytes, GOB-wise deswizzle per subresource
        uint8_t *old = (uint8_t *)malloc((size_t)tot); memset(old, 0xcd, (size_t)tot);
        for (int q = 0; q < n; q++) CHECK(nvn_deswizzle_gob(&d, ref[q].level, ref[q].layer, mem, old + ref[q].begin, ref[q].pitch) == 0);
        // the worker (NR-05 job with the NR-22 sink)
        nr_tex_core *c = nr_tex_new(256u << 20, 0, 256u << 20); nr_tex_set_sink(c, &sink); nr_tex_verdict(c, (unsigned)k[i].fmt, 1);
        nrt_src s; memset(&s, 0, sizeof s); s.d = d; s.L = L; s.mem = mem; s.serial = 600 + i; s.src = 4;
        static nrt_job j; CHECK(nr_tex_create(c, &s, 0, 1, 1) == NRT_WHY_OK); CHECK(nr_tex_job_begin(c, &j) == 1); nr_tex_job_run(&j, NULL); CHECK(j.ok && j.ts); nr_tex_job_end(c, &j, 2);
        uint8_t *blob = NULL; void *ts = NULL; int kind = 0, why = 0; uint64_t bytes = 0, probe = 0;
        CHECK(nr_tex_take_ts(c, s.serial, mem, L.size, &blob, &ts, &kind, &bytes, &probe, &why) == 1 && ts && kind == NRT_TS_DESW && blob);
        if (!ts || !blob) { printf("  shape %u: no object\n", i); nr_tex_free(c); free(old); free(mem); continue; }
        const uint64_t slice = ((fake_obj *)ts)->bytes;
        CHECK(bytes == tot && slice == ((tot + 15) & ~(uint64_t)15));
        if (k[i].phone_slice) CHECK(slice == k[i].phone_slice);   // "the worker's deswizzled staging (450032 B)"
        // the first check (xbg_game.cpp nrts::compare before the fix): one memcmp of min(n, slice) bytes -> red exactly when there are gaps
        const int old_red = tot > slice + 15 || memcmp(old, blob, (size_t)(tot < slice ? tot : slice)) != 0;
        CHECK(old_red == gap); red_old += old_red;
        if (k[i].phone_bytes) CHECK(old_red);   // every phone shape: reproduced
        // the fixed check: only the bytes the copies read
        int at = -2; uint64_t off = 1;
        const int r = nr_tex_cmp_staged(&d, &L, blob, slice, old, tot, &at, &off);
        CHECK(r == (gap ? NRT_CMP_PAD : NRT_CMP_SAME) && at == -1 && off == 0);
        red_new += r == NRT_CMP_DIFF || r == NRT_CMP_LAYOUT; pads += r == NRT_CMP_PAD; sames += r == NRT_CMP_SAME;
        // the old path's copy regions (its VkBufferImageCopy list: offset from the staging start, tight rows, whole subresource) = mode 1's
        nrt_region rg[NRT_SUBS];
        for (int q = 0; q < n; q++) rg[q] = (nrt_region){.off = ref[q].begin, .level = ref[q].level, .layer = ref[q].layer, .layers = 1, .w = ref[q].w, .h = ref[q].h, .depth = ref[q].depth};
        CHECK(nr_tex_regions_match(&d, &L, rg, n, &at) == 1 && at == -1);
        // negatives: a real byte difference is still red, at its place; a gap byte is not; lengths; regions moved / resized / pitched
        const int last = n - 1; const uint64_t pos = ref[last].begin + ref[last].bytes / 2;
        old[pos] ^= 0x01; CHECK(nr_tex_cmp_staged(&d, &L, blob, slice, old, tot, &at, &off) == NRT_CMP_DIFF && at == last && off == pos); old[pos] ^= 0x01;
        old[0] ^= 0x80; CHECK(nr_tex_cmp_staged(&d, &L, blob, slice, old, tot, &at, &off) == NRT_CMP_DIFF && at == 0 && off == 0); old[0] ^= 0x80;
        if (gap) { int q = 0; while (q + 1 < n && ref[q].begin + ref[q].bytes == ref[q + 1].begin) q++;
            const uint64_t gp = ref[q].begin + ref[q].bytes; old[gp] = blob[gp]; CHECK(nr_tex_cmp_staged(&d, &L, blob, slice, old, tot, &at, &off) == NRT_CMP_PAD);
            for (uint64_t b = 0; b < tot; b++) old[b] = blob[b]; CHECK(nr_tex_cmp_staged(&d, &L, blob, slice, old, tot, &at, &off) == NRT_CMP_SAME);
            blob[gp] ^= 0xff; CHECK(nr_tex_cmp_staged(&d, &L, blob, slice, old, tot, &at, &off) == NRT_CMP_PAD); blob[gp] ^= 0xff; }
        CHECK(nr_tex_cmp_staged(&d, &L, blob, slice, old, tot - 1, &at, &off) == NRT_CMP_LAYOUT);
        CHECK(nr_tex_cmp_staged(&d, &L, blob, tot - 1, old, tot, &at, &off) == NRT_CMP_LAYOUT);
        if (n > 1) { rg[1].off += 16; CHECK(nr_tex_regions_match(&d, &L, rg, n, &at) == 0 && at == 1); rg[1].off -= 16; }
        rg[last].w += 1; CHECK(nr_tex_regions_match(&d, &L, rg, n, &at) == 0 && at == last); rg[last].w -= 1;
        rg[0].row_len = rg[0].w + 4; CHECK(nr_tex_regions_match(&d, &L, rg, n, &at) == 0 && at == 0); rg[0].row_len = 0;
        rg[0].layers = 2; CHECK(nr_tex_regions_match(&d, &L, rg, n, &at) == 0); rg[0].layers = 1;
        CHECK(nr_tex_regions_match(&d, &L, rg, n - 1, &at) == 0);
        CHECK(nr_tex_regions_match(&d, &L, rg, n, &at) == 1);
        nr_tex_unhold(c, bytes); fv_release(NULL, ts); nr_tex_free(c); free(old); free(mem);
    }
    CHECK(red_new == 0 && red_old == gapped && phone == 11 && gapped >= phone);
    printf("texstream verify: %d shapes (%d from the phone), the old whole-run memcmp red on %d (exactly the ones with alignment gaps), fixed compare: %d SAME + %d gap-only, 0 red\n",
           (int)(sizeof k / sizeof *k), phone, red_old, sames, pads);
}

int main(void) {
    test_layout_and_blob();
    test_probe();
    test_table();
    test_texstream();
    test_texstream_verify();
    if (failures) { printf("FAIL: nr_tex %d of %d checks\n", failures, checks); return 1; }
    printf("PASS: nr_tex (%d checks): DESWZ_DIRECT layout, blobs = nvn_deswizzle, probe, create / job / take / drop, stale, cancel, gone, budget + expire, verdicts, queue, slot reuse, ASTC key; NR-22 sink: proven formats only, staging = nvn_deswizzle, every untaken end released once, ASTC objects, refusals; =2 verify: subresources + copy regions, gaps not red\n", checks);
    return 0;
}
