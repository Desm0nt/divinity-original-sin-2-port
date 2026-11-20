// [DOS2] NR-GMEM host test (docs/dos2/nr/NR-GMEM.md): the per-pass GMEM / sysmem rule of dm/gfx/nr/nr_gmem.h (DM_GFX_PASS_GMEM) on a
// model of a DOS2 campaign frame (passes as docs/dos2/GPU_BREAKDOWN.md lists them: sizes, attachments, draws), the hazard vetoes, the
// thresholds, Turnip's bandwidth model (by hand: tu_autotune.cc bandwidth_algo) and a random sweep of the rule's invariants.
// Build: clang -std=c11 -O2 -Wall -Wextra nr_gmem_test.c   (no other file)
#include "../../../nr/nr_gmem.h"
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; if (fails < 30) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static nrgm_pass mk(uint32_t w, uint32_t h, uint32_t atts, uint32_t draws, uint32_t ld, uint32_t st, uint32_t cl, uint32_t draw_bytes_each) {
    nrgm_pass p; memset(&p, 0, sizeof p);
    p.w = w; p.h = h; p.layers = 1; p.subpasses = 1; p.atts = atts; p.draws = draws; p.cpp_load = ld; p.cpp_store = st; p.cpp_clear = cl;
    p.draw_bytes = (uint64_t)draw_bytes_each * draws;
    return p;
}

static void test_frame(void) {
    const nrgm_cfg c = nrgm_cfg_default();
    struct { const char *name; nrgm_pass p; int want; } t[] = {
        // name                          w     h    atts draws  ld  st  cl  B/sample
        {"depth pre-pass (D24S8)",   mk(2160, 972, 1, 526,  4,  4,  0,  8), NRGM_W_DRAWS},
        {"G-buffer 4 MRT + D24S8",   mk(2160, 972, 5, 231, 20, 20,  0, 20), NRGM_W_DRAWS},
        {"shadow atlas D16",         mk(1536,1536, 1, 252,  0,  2,  2,  4), NRGM_W_DRAWS},
        {"DeferredSky 2 MRT",        mk(2160, 972, 2,   1,  0,  8,  0,  8), NRGM_W_GMEM},
        {"ClusteredLighting + DS",   mk(2160, 972, 2,   1,  8,  8,  0,  8), NRGM_W_GMEM},
        {"DeferredFog (blend)",      mk(2160, 972, 1,   1,  4,  4,  0,  8), NRGM_W_GMEM},
        {"PPApplyLUT",               mk(2160, 972, 1,   1,  0,  4,  0,  4), NRGM_W_GMEM},
        {"PPFXAA",                   mk(2160, 972, 1,   1,  0,  4,  0,  4), NRGM_W_GMEM},
        {"CombineUI (window)",       mk(2400,1080, 1,   2,  0,  4,  0,  4), NRGM_W_GMEM},
        {"particles forward 5 draws",mk(2160, 972, 2,   5,  8,  8,  0, 12), NRGM_W_GMEM},
        {"Iggy UI layer",            mk(2400,1080, 2,  80,  8,  8,  0,  8), NRGM_W_DRAWS},
        {"Iggy filter + DS",         mk(2400,1080, 2,   2,  8,  8,  0,  8), NRGM_W_GMEM},
        {"bloom quarter res",        mk( 540, 243, 1,   1,  0,  4,  0,  4), NRGM_W_SMALL},
        {"half-res particles 1080p", mk(1080, 486, 2,   3,  8,  8,  0, 12), NRGM_W_SMALL},
        {"portrait 80x100",          mk(  80, 100, 2,   3,  8,  8,  0,  8), NRGM_W_SMALL},
        {"brightness 256x256",       mk( 256, 256, 1,   1,  0,  4,  0,  4), NRGM_W_SMALL},
        {"clear-only pass",          mk(2160, 972, 2,   0,  0,  8,  8,  0), NRGM_W_NODRAW},
        {"6 draws",                  mk(2160, 972, 1,   6,  4,  4,  0,  4), NRGM_W_DRAWS},
        {"4 attachments",            mk(2160, 972, 4,   1,  0, 16,  0, 16), NRGM_W_ATTS},
    };
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++) {
        const int w = nrgm_decide(&c, &t[i].p);
        CHECK(w == t[i].want, "%s: %s, want %s", t[i].name, nrgm_why_name[w], nrgm_why_name[t[i].want]);
    }
}

static void test_vetoes(void) {
    const nrgm_cfg c = nrgm_cfg_default();
    nrgm_pass p = mk(2160, 972, 1, 1, 0, 4, 0, 4);
    CHECK(nrgm_decide(&c, &p) == NRGM_W_GMEM, "base");
    nrgm_pass q = p; q.storage = 1; CHECK(nrgm_decide(&c, &q) == NRGM_W_STORAGE, "storage");
    q = p; q.depth_read = 1; CHECK(nrgm_decide(&c, &q) == NRGM_W_GMEM, "depth read without a write is exact in GMEM");
    q = p; q.depth_written = 1; CHECK(nrgm_decide(&c, &q) == NRGM_W_GMEM, "depth write alone is fine");
    q = p; q.depth_read = q.depth_written = 1; CHECK(nrgm_decide(&c, &q) == NRGM_W_FEEDBACK, "feedback");
    q = p; q.subpasses = 2; CHECK(nrgm_decide(&c, &q) == NRGM_W_OTHER, "two subpasses (NR-ONCHIP M)");
    {   // [NR-FRAME] DM_GFX_PASS_GMEM_M=1: M (2 subpasses: Sky 2 MRT + Light) passes the veto, every other rule still applies to its totals
        nrgm_cfg m = nrgm_cfg_default(); m.sub2 = 1;
        nrgm_pass mm = mk(2160, 972, 2, 2, 0, 8, 8, 8); mm.subpasses = 2;
        CHECK(nrgm_decide(&c, &mm) == NRGM_W_OTHER, "M vetoed by default");
        CHECK(nrgm_decide(&m, &mm) == NRGM_W_GMEM, "M with sub2: GMEM");
        nrgm_pass x = mm; x.subpasses = 3; x.atts = 3; x.draws = 3; CHECK(nrgm_decide(&m, &x) == NRGM_W_GMEM, "S2F M (3 subpasses, 3 attachments) with sub2: GMEM");
        x = mm; x.subpasses = 4; CHECK(nrgm_decide(&m, &x) == NRGM_W_OTHER, "four subpasses stay vetoed");
        x = mm; x.subpasses = 0; CHECK(nrgm_decide(&m, &x) == NRGM_W_OTHER, "creation unseen stays vetoed");
        x = mm; x.layers = 2; CHECK(nrgm_decide(&m, &x) == NRGM_W_OTHER, "layered stays vetoed");
        x = mm; x.storage = 1; CHECK(nrgm_decide(&m, &x) == NRGM_W_STORAGE, "storage still vetoes M");
        x = mm; x.atts = 4; CHECK(nrgm_decide(&m, &x) == NRGM_W_ATTS, "attachments still count");
        x = mm; x.w = 1080; x.h = 486; CHECK(nrgm_decide(&m, &x) == NRGM_W_SMALL, "size still counts");
        nrgm_pass one = p; CHECK(nrgm_decide(&m, &one) == nrgm_decide(&c, &one), "sub2 changes nothing for one-subpass passes");
        // the bandwidth picture of M (by hand): sysmem = Sky's clears (2 x 4 B) + Sky's 2 blended MRT (2 x 4 B x 2) + Light's blended Df (4 x 2)
        // = 8 + 16 + 8 = 32 B/px; gmem = 1.1 x stores (2 x 4) + 0.1 x draws (24) = 8.8 + 2.4 = 11.2 B/px (Light's Sp texture read, not in
        // Turnip's model, is a further 4 B/px sysmem only)
        nrgm_pass bw = mk(1000, 1000, 2, 2, 0, 8, 8, 0); bw.subpasses = 2; bw.draw_bytes = 16 + 8; uint64_t s, g; nrgm_bytes(&bw, &s, &g);
        CHECK(s == 32000000ull && g == 11200000ull, "M bandwidth: sys %llu (32e6) gmem %llu (11.2e6)", (unsigned long long)s, (unsigned long long)g);
        m.bw = 1; CHECK(nrgm_decide(&m, &bw) == NRGM_W_GMEM, "M with the bandwidth gate: GMEM");
    }
    q = p; q.subpasses = 0; CHECK(nrgm_decide(&c, &q) == NRGM_W_OTHER, "creation unseen");
    q = p; q.layers = 2; CHECK(nrgm_decide(&c, &q) == NRGM_W_OTHER, "layered");
    // vetoes win over the thresholds: a storage-writing draw-heavy pass says storage
    q = mk(2160, 972, 5, 300, 0, 0, 0, 4); q.storage = 1; CHECK(nrgm_decide(&c, &q) == NRGM_W_STORAGE, "veto first");
}

static void test_thresholds(void) {
    nrgm_cfg c = nrgm_cfg_default();
    CHECK(c.min_draws == 1 && c.max_draws == 5 && c.max_atts == 3 && c.min_pixels == 600000 && c.bw == 0, "defaults");
    nrgm_pass p = mk(2160, 972, 3, 5, 0, 4, 0, 4);
    CHECK(nrgm_decide(&c, &p) == NRGM_W_GMEM, "5 draws / 3 atts are inside");
    c.max_draws = 4; CHECK(nrgm_decide(&c, &p) == NRGM_W_DRAWS, "max_draws 4");
    c = nrgm_cfg_default(); c.max_atts = 2; CHECK(nrgm_decide(&c, &p) == NRGM_W_ATTS, "max_atts 2");
    c = nrgm_cfg_default(); c.min_pixels = 2160u * 972u + 1u; CHECK(nrgm_decide(&c, &p) == NRGM_W_SMALL, "min_pixels above the pass");
    c = nrgm_cfg_default(); c.min_pixels = 0; p.w = 80; p.h = 100; CHECK(nrgm_decide(&c, &p) == NRGM_W_GMEM, "min_pixels 0: any size");
    c = nrgm_cfg_default(); c.min_draws = 0; p = mk(2160, 972, 1, 0, 0, 4, 4, 0); CHECK(nrgm_decide(&c, &p) == NRGM_W_GMEM, "min_draws 0 takes clear-only passes");
}

static void test_bandwidth(void) {
    // Turnip's model by hand, 1000 x 1000 = 1e6 px
    nrgm_pass p = mk(1000, 1000, 1, 1, 0, 4, 0, 4); uint64_t s, g;
    nrgm_bytes(&p, &s, &g);
    CHECK(s == 4000000ull && g == 4800000ull, "1 opaque full-screen draw: sys %llu (want 4e6) gmem %llu (want 1.1*4e6 + 0.4e6)", (unsigned long long)s, (unsigned long long)g);
    nrgm_cfg c = nrgm_cfg_default(); c.bw = 1; c.min_pixels = 0;
    CHECK(nrgm_decide(&c, &p) == NRGM_W_BW, "the model keeps a plain post pass in sysmem");
    // depth test + write with a DONT_CARE depth store, colour stored: sys 12 B/px, gmem 1.1*4 + 1.2 = 5.6
    p = mk(1000, 1000, 2, 1, 0, 4, 0, 12); nrgm_bytes(&p, &s, &g);
    CHECK(s == 12000000ull && g == 5600000ull, "transient depth: sys %llu gmem %llu", (unsigned long long)s, (unsigned long long)g);
    CHECK(nrgm_decide(&c, &p) == NRGM_W_GMEM, "transient depth favours GMEM");
    // overdraw: 5 blended draws over a loaded + stored target: sys 5*8 = 40, gmem 1.1*8 + 4 = 12.8
    p = mk(1000, 1000, 1, 5, 4, 4, 0, 8); nrgm_bytes(&p, &s, &g);
    CHECK(s == 40000000ull && g == 12800000ull, "overdraw: sys %llu gmem %llu", (unsigned long long)s, (unsigned long long)g);
    CHECK(nrgm_decide(&c, &p) == NRGM_W_GMEM, "overdraw favours GMEM");
    // clears count for sysmem: a cleared + stored target with one draw: sys 4 + 4, gmem 1.1*4 + 0.4
    p = mk(1000, 1000, 1, 1, 0, 4, 4, 4); nrgm_bytes(&p, &s, &g);
    CHECK(s == 8000000ull && g == 4800000ull, "clear: sys %llu gmem %llu", (unsigned long long)s, (unsigned long long)g);
    // no draws: no draw traffic
    p = mk(1000, 1000, 1, 0, 4, 4, 0, 0); p.draw_bytes = 99; nrgm_bytes(&p, &s, &g);
    CHECK(s == 0 && g == 8800000ull, "no draws: sys %llu gmem %llu", (unsigned long long)s, (unsigned long long)g);
    // layers multiply the area
    p = mk(100, 100, 1, 1, 0, 4, 0, 4); p.layers = 3; nrgm_bytes(&p, &s, &g); CHECK(s == 120000ull, "layers: %llu", (unsigned long long)s);
}

static void test_cpp(void) {
    CHECK(nrgm_cpp(37) == 4 && nrgm_cpp(44) == 4 && nrgm_cpp(50) == 4, "RGBA8 / BGRA8 / BGRA8_SRGB");
    CHECK(nrgm_cpp(122) == 4 && nrgm_cpp(64) == 4, "B10G11R11 / A2B10G10R10");
    CHECK(nrgm_cpp(97) == 8 && nrgm_cpp(91) == 8, "RGBA16");
    CHECK(nrgm_cpp(83) == 4 && nrgm_cpp(100) == 4 && nrgm_cpp(76) == 2 && nrgm_cpp(9) == 1 && nrgm_cpp(16) == 2, "RG16F / R32F / R16F / R8 / RG8");
    CHECK(nrgm_cpp(103) == 8 && nrgm_cpp(106) == 12 && nrgm_cpp(109) == 16, "RG32F / RGB32F / RGBA32F");
    CHECK(nrgm_cpp(124) == 2 && nrgm_cpp(129) == 4 && nrgm_cpp(126) == 4 && nrgm_cpp(130) == 5 && nrgm_cpp(127) == 1 && nrgm_cpp(128) == 3, "depth formats");
    const uint8_t cc[8] = {4, 4, 8, 4, 0, 0, 0, 0};
    CHECK(nrgm_draw_bytes(cc, 0x1, 0x0, 4, 1, 0, 0, 0) == 4, "one opaque target");
    CHECK(nrgm_draw_bytes(cc, 0x1, 0x1, 4, 1, 0, 0, 0) == 8, "blended target reads the destination");
    CHECK(nrgm_draw_bytes(cc, 0xf, 0x4, 4, 1, 1, 1, 0) == 4 + 4 + 16 + 4 + 8, "G-buffer: 4 MRT (one blended) + depth test + write");
    CHECK(nrgm_draw_bytes(cc, 0x0, 0xff, 4, 1, 1, 0, 1) == 4 + 2, "depth test only + stencil");
    CHECK(nrgm_draw_bytes(cc, 0x2, 0x1, 0, 0, 1, 1, 1) == 4, "blend bit of an unwritten target ignored; no depth attachment");
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(uint32_t n) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng % n); }

static void test_sweep(void) {
    int seen[NRGM_W_N] = {0};
    for (int k = 0; k < 200000; k++) {
        nrgm_cfg c = {rnd(3), rnd(12), 1 + rnd(5), (uint64_t)rnd(3) * 300000u, (int)rnd(2), 0};
        nrgm_pass p = mk(1 + rnd(2600), 1 + rnd(1200), rnd(7), rnd(40), rnd(24), rnd(24), rnd(12), rnd(40));
        p.layers = rnd(8) ? 1 : 1 + rnd(3); p.subpasses = rnd(10) ? 1 : rnd(3); p.storage = !rnd(10); p.depth_read = !rnd(6); p.depth_written = rnd(2);
        const int w = nrgm_decide(&c, &p); ++seen[w];
        if (w == NRGM_W_GMEM) {
            uint64_t s, g; nrgm_bytes(&p, &s, &g);
            CHECK(p.subpasses == 1 && p.layers == 1 && !p.storage && !(p.depth_read && p.depth_written) && p.draws >= c.min_draws &&
                  p.draws <= c.max_draws && p.atts <= c.max_atts && (uint64_t)p.w * p.h >= c.min_pixels && (!c.bw || g < s),
                  "GMEM pick breaks a condition (k=%d)", k);
        }
        // more permissive thresholds never take a GMEM pick away
        if (w == NRGM_W_GMEM) {
            nrgm_cfg d = c; d.max_draws += 3; d.max_atts += 1; d.min_pixels /= 2; if (d.min_draws) d.min_draws--; d.bw = 0;
            CHECK(nrgm_decide(&d, &p) == NRGM_W_GMEM, "a looser rule dropped a pick (k=%d)", k);
        }
    }
    for (int i = 0; i < NRGM_W_N; i++) CHECK(seen[i] > 0, "the sweep never produced %s", nrgm_why_name[i]);
}

int main(void) {
    test_frame(); test_vetoes(); test_thresholds(); test_bandwidth(); test_cpp(); test_sweep();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
