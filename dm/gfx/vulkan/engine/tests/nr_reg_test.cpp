// [dos2-nrend] NR-01: host test of the engine object registry (dm/gfx/nr/nr_reg.h): put / kill / generation reuse, bounded tables,
// the checks of DM_NR=2 (wrong NVN format, wrong size, mips, target, packaged layout, sampler and state enums, shader programs),
// render-target sets, named transitions, the census, the one-line summary and the TSV writer read back field by field.
// Build (Windows clang + MSVC libs, or WSL g++):
//   clang -std=c11 -Wall -Wextra -c ../../../nr/nr_reg.c -D_CRT_SECURE_NO_WARNINGS && clang++ -std=c++17 -Wall -D_CRT_SECURE_NO_WARNINGS nr_reg_test.cpp nr_reg.o -o nr_reg_test
//   gcc -std=c11 -c ../../../nr/nr_reg.c && g++ -std=c++17 -Wall nr_reg_test.cpp nr_reg.o -o nr_reg_test
#include "../../../nr/nr_reg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

static int failures, checks;
#define CHECK(expression) do { ++checks; if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)

static uint64_t H(uint32_t idx, uint32_t gen) { return (uint64_t)gen << 32 | idx; }
static nr_tex_desc desc(int src, unsigned efmt, uint32_t w, uint32_t h, uint32_t d, unsigned mips, unsigned flags = 0) {
    nr_tex_desc e; memset(&e, 0, sizeof e);
    e.w = w; e.h = h; e.d = d; e.efmt = (uint8_t)efmt; e.mips = (uint8_t)mips; e.layers = 1; e.msaa = 1; e.src = (uint8_t)src; e.eflags = (uint16_t)flags; e.srgb = flags & 1;
    e.fmt_override = src == NR_SRC_IMAGE_LINEAR || src == NR_SRC_IMAGE_PACKAGED ? -1 : 0;
    return e;
}
static nr_nvn_tex nvn(uint64_t obj, int fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, int target = 1, int packaged = 0) {
    nr_nvn_tex n; memset(&n, 0, sizeof n);
    n.obj = obj; n.format = fmt; n.w = w; n.h = h; n.d = d; n.levels = levels; n.target = target; n.packaged = (uint8_t)packaged; n.size = (uint64_t)w * h * 4;
    return n;
}

static void test_formats() {
    CHECK(NR_FMT_N == 63);
    CHECK(!strcmp(nr_fmt_name(4), "RGBA8") && nr_formats[4].nvn == 0x25 && nr_formats[4].nvn_srgb == 0x38 && nr_formats[4].bytes == 4);
    CHECK(!strcmp(nr_fmt_name(53), "DXT1") && nr_formats[53].compressed && nr_formats[53].nvn == 0x41 && nr_formats[53].nvn_srgb == 0x45);
    CHECK(!strcmp(nr_fmt_name(61), "BGRA8") && nr_formats[61].nvn_srgb == 0x9c);
    CHECK(nr_formats[47].nvn == NR_NVN_TRAP && nr_formats[62].nvn == NR_NVN_TRAP);
    CHECK(!strcmp(nr_fmt_name(63), "?") && !strcmp(nr_fmt_name(1000), "?"));
    CHECK(nr_fmt_class(4) == NR_CLS_U8 && nr_fmt_class(20) == NR_CLS_HDR && nr_fmt_class(51) == NR_CLS_DEPTH && nr_fmt_class(59) == NR_CLS_BC && nr_fmt_class(0) == NR_CLS_NONE && nr_fmt_class(99) == NR_CLS_NONE);
    for (unsigned f = 0; f < NR_FMT_N; f++) { CHECK(nr_formats[f].name && nr_formats[f].name[0]); CHECK(f == 0 || nr_formats[f].bytes > 0); }
    // what the engine asks NVN for
    CHECK(nr_expect_nvn_format(4, 0, 0, 0) == 0x25 && nr_expect_nvn_format(4, 1, 0, 0) == 0x38);
    CHECK(nr_expect_nvn_format(4, 0, 0x31, 0) == 0x31);          // CreateTexture with an NVN format override
    CHECK(nr_expect_nvn_format(47, 0, 0, 0) == -1 && nr_expect_nvn_format(200, 0, 0, 0) == -1);   // trap / out of range: unknown
    CHECK(nr_expect_nvn_format(55, 1, -1, 1) == 0x48);           // image without its own NVN format: the table
    CHECK(nr_expect_nvn_format(55, 1, 0x44, 1) == 0x48 && nr_expect_nvn_format(55, 0, 0x48, 1) == 0x44);   // image pair swapped by the sRGB bit
    CHECK(nr_expect_nvn_format(4, 1, 0x25, 1) == 0x38 && nr_expect_nvn_format(4, 0, 0x38, 1) == 0x38);     // 0x38 itself is kept (not a case of the switch)
    CHECK(nr_expect_nvn_format(59, 0, 0x4d, 1) == 0x4d);         // not in a pair: as given
    CHECK(nr_h_idx(H(7, 3)) == 7 && nr_h_gen(H(7, 3)) == 3 && nr_h_gen(0xffc0000000000000ull | H(7, 3)) == 3);
}

static void test_textures(nr_reg *r) {
    const uint64_t h1 = H(10, 1);
    nr_tex_desc e = desc(NR_SRC_CREATE, 4, 256, 128, 0, 9);
    nr_nvn_tex n = nvn(0x1000, 0x25, 256, 128, 1, 9);
    nr_tex *t = nr_tex_put(r, h1, &e, &n, 1);
    CHECK(t && t->alive && t->creates == 1 && t->handle == h1 && t->nvn.obj == 0x1000 && r->st[NR_K_TEX].puts == 1 && r->st[NR_K_TEX].hi == 11);
    CHECK(nr_tex_get(r, h1) == t && !nr_tex_get(r, H(10, 2)) && !nr_tex_get(r, H(11, 1)) && !nr_tex_get(r, 10));
    CHECK(nr_check_tex(t) == 0 && nr_expect_target(t) == 1);
    // kill by handle: only the very handle; a stale generation is a miss
    CHECK(!nr_tex_kill(r, H(10, 2)) && r->st[NR_K_TEX].kill_miss == 1 && t->alive);
    CHECK(nr_tex_kill(r, h1) && !t->alive && t->destroys == 1 && !nr_tex_get(r, h1) && r->st[NR_K_TEX].kills == 1);
    CHECK(!nr_tex_kill(r, h1) && r->st[NR_K_TEX].kill_miss == 2);
    // generation reuse of the slot after a destroy: a fresh object, the counters of the slot carry on
    t = nr_tex_put(r, H(10, 2), &e, &n, 1);
    CHECK(t && t->alive && t->creates == 2 && t->destroys == 1 && t->replaced == 0 && nr_tex_get(r, H(10, 2)) && !nr_tex_get(r, h1));
    // reuse WITHOUT a destroy seen: the old object counts as replaced
    n.obj = 0x2000;
    t = nr_tex_put(r, H(10, 3), &e, &n, 1);
    CHECK(t && t->creates == 3 && t->replaced == 1 && r->st[NR_K_TEX].replaced == 1 && t->nvn.obj == 0x2000);
    // kill by NVN object (the engine's deferred destroy)
    CHECK(!nr_tex_kill_obj(r, 0x1000) && !nr_tex_kill_obj(r, 0));   // the replaced object is gone from the table; 0 is nothing
    CHECK(nr_tex_kill_obj(r, 0x2000) && !t->alive && t->destroys == 2);
    // bounds: index past the table, generation 0
    const uint64_t over0 = r->st[NR_K_TEX].overflow, bad0 = r->st[NR_K_TEX].bad_handle;
    CHECK(!nr_tex_put(r, H(NR_TEX_MAX, 1), &e, &n, 1) && !nr_tex_put(r, H(0xffffffffu, 1), &e, &n, 1) && r->st[NR_K_TEX].overflow == over0 + 2);
    CHECK(!nr_tex_put(r, 10, &e, &n, 1) && !nr_tex_put(r, 0, &e, &n, 1) && r->st[NR_K_TEX].bad_handle == bad0 + 2);
    CHECK(!nr_tex_kill(r, H(NR_TEX_MAX + 5, 1)) && !nr_tex_get(r, H(0xffffffffu, 0x3fffff)));
    CHECK(nr_tex_put(r, H(NR_TEX_MAX - 1, 1), &e, &n, 1) != NULL && r->st[NR_K_TEX].hi == NR_TEX_MAX);
    nr_tex_kill(r, H(NR_TEX_MAX - 1, 1));
    // no NVN texture inside the engine call
    t = nr_tex_put(r, H(12, 1), &e, NULL, 0);
    CHECK(t && t->nnvn == 0 && (nr_check_tex(t) & NR_BAD_NO_NVN));
    nr_tex_kill(r, H(12, 1));
}

static unsigned chk(nr_reg *r, const nr_tex_desc &e, const nr_nvn_tex &n) {
    nr_tex *t = nr_tex_put(r, H(20, 1), &e, &n, 1);
    const unsigned bad = t ? nr_check_tex(t) : ~0u;
    nr_tex_kill(r, H(20, 1));
    return bad;
}
static void test_checks(nr_reg *r) {
    // CreateTexture: RGBA8 256x128, 9 mips
    nr_tex_desc e = desc(NR_SRC_CREATE, 4, 256, 128, 0, 9);
    CHECK(chk(r, e, nvn(1, 0x25, 256, 128, 1, 9)) == 0);
    CHECK(chk(r, e, nvn(1, 0x38, 256, 128, 1, 9)) == NR_BAD_FORMAT);          // sRGB format without the sRGB flag
    CHECK(chk(r, e, nvn(1, 0x29, 256, 128, 1, 9)) == NR_BAD_FORMAT);          // wrong format
    CHECK(chk(r, e, nvn(1, 0x25, 128, 128, 1, 9)) == NR_BAD_DIMS);            // wrong width
    CHECK(chk(r, e, nvn(1, 0x25, 256, 128, 2, 9)) == NR_BAD_DIMS);            // wrong depth
    CHECK(chk(r, e, nvn(1, 0x25, 256, 128, 1, 8)) == NR_BAD_MIPS);
    CHECK(chk(r, e, nvn(1, 0x25, 256, 128, 1, 9, 2)) == NR_BAD_TARGET);       // 3D target for a 2D texture
    CHECK(chk(r, e, nvn(1, 0x25, 256, 128, 1, 9, 1, 1)) == NR_BAD_PACKAGED);  // packaged layout on a plain create
    CHECK(chk(r, e, nvn(1, 0x29, 64, 128, 1, 3)) == (NR_BAD_FORMAT | NR_BAD_DIMS | NR_BAD_MIPS));
    e = desc(NR_SRC_CREATE, 4, 256, 128, 0, 9, NR_TF_SRGB);
    CHECK(chk(r, e, nvn(1, 0x38, 256, 128, 1, 9)) == 0 && chk(r, e, nvn(1, 0x25, 256, 128, 1, 9)) == NR_BAD_FORMAT);
    e = desc(NR_SRC_CREATE, 4, 256, 128, 0, 9); e.fmt_override = 0x31;         // NVN format override of the call wins
    CHECK(chk(r, e, nvn(1, 0x31, 256, 128, 1, 9)) == 0 && chk(r, e, nvn(1, 0x25, 256, 128, 1, 9)) == NR_BAD_FORMAT);
    e = desc(NR_SRC_CREATE, 99, 16, 16, 0, 1);                                // engine format out of the enum
    CHECK(chk(r, e, nvn(1, 0x25, 16, 16, 1, 1)) == NR_BAD_EFMT);
    // mips 0: one level, except a colour RT (the engine computes the chain)
    e = desc(NR_SRC_CREATE, 4, 64, 64, 0, 0);
    CHECK(chk(r, e, nvn(1, 0x25, 64, 64, 1, 1)) == 0 && chk(r, e, nvn(1, 0x25, 64, 64, 1, 7)) == NR_BAD_MIPS);
    e = desc(NR_SRC_RT, 20, 1280, 720, 0, 0, 0x20);
    CHECK(chk(r, e, nvn(1, 0x29, 1280, 720, 1, 11)) == 0 && chk(r, e, nvn(1, 0x29, 1280, 720, 1, 1)) == 0);
    e = desc(NR_SRC_RT, 51, 1280, 720, 0, 1, 0x100);                          // depth-stencil RT
    CHECK(chk(r, e, nvn(1, 0x35, 1280, 720, 1, 1)) == 0 && chk(r, e, nvn(1, 0x34, 1280, 720, 1, 1)) == NR_BAD_FORMAT);
    // shapes: 3D, cube, 2D array, multisample
    e = desc(NR_SRC_CREATE, 4, 32, 32, 16, 1);
    CHECK(chk(r, e, nvn(1, 0x25, 32, 32, 16, 1, 2)) == 0 && chk(r, e, nvn(1, 0x25, 32, 32, 16, 1, 1)) == NR_BAD_TARGET && chk(r, e, nvn(1, 0x25, 32, 32, 1, 1, 2)) == NR_BAD_DIMS);
    e = desc(NR_SRC_CREATE, 4, 64, 64, 0, 1, NR_TF_CUBE);
    CHECK(chk(r, e, nvn(1, 0x25, 64, 64, 1, 1, 8)) == 0 && chk(r, e, nvn(1, 0x25, 64, 64, 1, 1, 1)) == NR_BAD_TARGET);
    e = desc(NR_SRC_CREATE, 4, 64, 64, 0, 1); e.layers = 4;
    CHECK(chk(r, e, nvn(1, 0x25, 64, 64, 1, 1, 4)) == 0);
    e = desc(NR_SRC_RT, 4, 64, 64, 0, 1, 0x20); e.msaa = 3;
    CHECK(chk(r, e, nvn(1, 0x25, 64, 64, 1, 1, 5)) == 0);
    e = desc(NR_SRC_RT, 4, 64, 64, 0, 1, 0x20);
    { nr_nvn_tex n = nvn(1, 0x25, 64, 64, 1, 1); n.samples = 4; CHECK(chk(r, e, n) == NR_BAD_SAMPLES); }
    // CreateTextureFromImage: packaged (layout already in pool memory) and linear (WriteTexels)
    e = desc(NR_SRC_IMAGE_PACKAGED, 55, 1024, 1024, 1, 11, NR_TF_SRGB); e.layers = 1;
    CHECK(chk(r, e, nvn(1, 0x48, 1024, 1024, 1, 11, 1, 1)) == 0);
    CHECK(chk(r, e, nvn(1, 0x48, 1024, 1024, 1, 11, 1, 0)) == NR_BAD_PACKAGED);   // packaged image, no SetPackagedTextureLayout seen
    CHECK(chk(r, e, nvn(1, 0x44, 1024, 1024, 1, 11, 1, 1)) == NR_BAD_FORMAT);
    CHECK(chk(r, e, nvn(1, 0x48, 1024, 512, 1, 11, 1, 1)) == NR_BAD_DIMS);
    CHECK(chk(r, e, nvn(1, 0x48, 1024, 1024, 1, 10, 1, 1)) == NR_BAD_MIPS);
    e = desc(NR_SRC_IMAGE_LINEAR, 4, 512, 512, 1, 1);
    CHECK(chk(r, e, nvn(1, 0x25, 512, 512, 1, 1)) == 0 && chk(r, e, nvn(1, 0x25, 512, 512, 1, 1, 1, 1)) == NR_BAD_PACKAGED);
    e.fmt_override = 0x25; e.srgb = 1;                                          // the image's own NVN format, swapped to its sRGB twin
    CHECK(chk(r, e, nvn(1, 0x38, 512, 512, 1, 1)) == 0 && chk(r, e, nvn(1, 0x25, 512, 512, 1, 1)) == NR_BAD_FORMAT);
    e = desc(NR_SRC_IMAGE_PACKAGED, 55, 256, 256, 1, 9, NR_TF_CUBE); e.layers = 6;
    CHECK(chk(r, e, nvn(1, 0x44, 256, 256, 6, 9, 8, 1)) == 0 && chk(r, e, nvn(1, 0x44, 256, 256, 1, 9, 8, 1)) == NR_BAD_DIMS);
    e = desc(NR_SRC_IMAGE_PACKAGED, 4, 32, 32, 32, 1);                           // volume image
    CHECK(chk(r, e, nvn(1, 0x25, 32, 32, 32, 1, 2, 1)) == 0);
    // window texture: format from the table, a sane size
    e = desc(NR_SRC_WINDOW, 4, 1280, 720, 0, 1);
    CHECK(chk(r, e, nvn(1, 0x25, 1280, 720, 1, 1)) == 0 && chk(r, e, nvn(1, 0x25, 0, 720, 1, 1)) == NR_BAD_DIMS);
    char text[96];
    CHECK(nr_bad_text(NR_BAD_FORMAT | NR_BAD_DIMS, text, sizeof text) == 11 && !strcmp(text, "format+dims"));
    CHECK(nr_bad_text(0, text, sizeof text) == 0 && !text[0]);
    CHECK(nr_bad_text(~0u, text, 8) < 8 && strlen(text) < 8);                    // bounded
    for (int b = 0; b < NR_BAD_BITS; b++) CHECK(strcmp(nr_bad_name(b), "?") != 0);
    CHECK(!strcmp(nr_bad_name(NR_BAD_BITS), "?"));
}

static void test_small_kinds(nr_reg *r) {
    // samplers: CreateSampler(filter 5, wrap 0/0/0, aniso 16, compare 0) -> NVN min 5, mag 1, wrap 1/1/1
    nr_nvn_smp n; memset(&n, 0, sizeof n);
    n.obj = 0x500; n.minf = 5; n.magf = 1; n.ws = n.wt = n.wr = 1; n.aniso = 16.f;
    nr_sampler *s = nr_sampler_put(r, H(3, 1), 5, 0, 0, 0, 16, 0, &n);
    CHECK(s && s->alive && s->nvn.have && nr_check_sampler(s) == 0);
    n.ws = 7; s = nr_sampler_put(r, H(3, 1), 5, 0, 0, 0, 16, 0, &n);
    CHECK(nr_check_sampler(s) == NR_BAD_FILTER && r->st[NR_K_SAMPLER].replaced == 1);
    n.ws = 7; n.wt = 6; n.wr = 5; n.minf = 3; n.cmp_mode = 1; n.cmp_func = 2; n.aniso = 1.f;
    s = nr_sampler_put(r, H(4, 1), 2, 1, 2, 3, 1, 1, &n);
    CHECK(nr_check_sampler(s) == 0);
    n.cmp_func = 5; s = nr_sampler_put(r, H(4, 1), 2, 1, 2, 3, 1, 1, &n);
    CHECK(nr_check_sampler(s) == NR_BAD_FILTER);
    s = nr_sampler_put(r, H(5, 1), 9, 0, 0, 0, 1, 0, &n);
    CHECK(nr_check_sampler(s) == NR_BAD_ENUM);
    s = nr_sampler_put(r, H(6, 1), 0, 0, 0, 0, 1, 0, NULL);
    CHECK(nr_check_sampler(s) == NR_BAD_NO_NVN);
    memset(&n, 0, sizeof n); n.obj = 0x501; n.minf = 2; n.magf = 0; n.ws = n.wt = n.wr = 1; n.aniso = 1.f;
    s = nr_sampler_put(r, 7, 0, 0, 0, 0, 1, 0, &n);                 // a sampler handle without a generation is still stored (index only)
    CHECK(s && s->handle == 7 && nr_check_sampler(s) == 0 && r->st[NR_K_SAMPLER].bad_handle == 0);
    CHECK(!nr_sampler_put(r, H(NR_SAMPLER_MAX, 1), 0, 0, 0, 0, 1, 0, &n) && r->st[NR_K_SAMPLER].overflow == 1);
    // state objects: the creation arguments of the game's own defaults pass, values past the engine's tables do not
    const uint8_t b1[6] = {6, 7, 1, 7, 0, 0}, b2[6] = {11, 7, 1, 7, 0, 0}, b3[6] = {1, 0, 1, 1, 4, 1}, b4[6] = {1, 0, 1, 1, 5, 1};
    CHECK(nr_check_state(NR_ST_BLEND, b1, 6) == 0 && nr_check_state(NR_ST_BLEND, b3, 6) == 0);
    CHECK(nr_check_state(NR_ST_BLEND, b2, 6) == NR_BAD_ENUM && nr_check_state(NR_ST_BLEND, b4, 6) == NR_BAD_ENUM && nr_check_state(NR_ST_BLEND, b1, 5) == NR_BAD_ENUM);
    const uint8_t d1[8] = {1, 0, 2, 1, 0x7e, 4, 0, 2}, d2[8] = {1, 0, 8, 1, 0x7e, 4, 0, 2}, d3[8] = {2, 0, 2, 1, 0x7e, 4, 0, 2};
    CHECK(nr_check_state(NR_ST_DEPTH, d1, 8) == 0 && nr_check_state(NR_ST_DEPTH, d2, 8) == NR_BAD_ENUM && nr_check_state(NR_ST_DEPTH, d3, 8) == NR_BAD_ENUM);
    const uint8_t r1[5] = {2, 0, 1, 0, 1}, r2[5] = {3, 0, 1, 0, 0};
    CHECK(nr_check_state(NR_ST_RASTER, r1, 5) == 0 && nr_check_state(NR_ST_RASTER, r2, 5) == NR_BAD_ENUM && nr_check_state(7, r1, 5) == NR_BAD_ENUM && nr_check_state(NR_ST_RASTER, NULL, 5) == NR_BAD_ENUM);
    nr_state *st = nr_state_put(r, NR_ST_DEPTH, H(9, 1), d1, 8);
    CHECK(st && st->nargs == 8 && st->arg[4] == 0x7e && st->arg[7] == 2 && r->st[NR_K_DEPTH].puts == 1 && r->st[NR_K_BLEND].puts == 0);
    CHECK(nr_state_put(r, NR_ST_BLEND, H(9, 1), b1, 6) && nr_state_put(r, NR_ST_RASTER, H(2, 1), r1, 5) && !nr_state_put(r, 5, H(1, 1), r1, 5));
    CHECK(nr_state_put(r, NR_ST_RASTER, H(3, 1), r1, 40)->nargs == NR_ST_ARGS);       // more arguments than kept: clipped
    // vertex formats: the engine hands the same handle out for equal elements
    uint8_t el[40 * 6]; for (unsigned i = 0; i < sizeof el; i++) el[i] = (uint8_t)i;
    nr_vformat *v = nr_vformat_put(r, H(1, 1), el, 3);
    CHECK(v && v->count == 3 && v->kept == 3 && v->creates == 1 && v->elem[2][5] == 17);
    CHECK(nr_vformat_put(r, H(1, 1), el, 3) == v && v->creates == 2 && r->st[NR_K_VFORMAT].replaced == 0);
    v = nr_vformat_put(r, H(2, 1), el, 40);
    CHECK(v && v->count == 40 && v->kept == NR_VF_ELEMS);                              // more elements than kept: counted, clipped
    v = nr_vformat_put(r, H(3, 1), NULL, 4);
    CHECK(v && v->count == 4 && v->kept == 0);
    // buffers: static / dynamic bytes by kind, destroy by handle
    CHECK(nr_buffer_put(r, NR_BUF_VERTEX, H(1, 1), 1000, 0, 0, 0, 0xb0, 1000) && nr_buffer_put(r, NR_BUF_VERTEX, H(2, 1), 500, 0, 0, NR_USAGE_DYNAMIC, 0xb1, 500));
    nr_buffer *ib = nr_buffer_put(r, NR_BUF_INDEX, H(1, 1), 600, 300, 2, 1, 0xb2, 600);
    CHECK(ib && ib->index_size == 2 && ib->count == 300 && !nr_buffer_put(r, 2, H(1, 1), 1, 0, 0, 0, 0, 0) && !nr_buffer_put(r, NR_BUF_INDEX, H(NR_BUF_MAX, 1), 1, 0, 0, 0, 0, 0));
    nr_census c; nr_census_take(r, &c);
    CHECK(c.buf_n[NR_BUF_VERTEX][0] == 1 && c.buf_bytes[NR_BUF_VERTEX][0] == 1000 && c.buf_n[NR_BUF_VERTEX][1] == 1 && c.buf_bytes[NR_BUF_VERTEX][1] == 500);
    CHECK(c.buf_n[NR_BUF_INDEX][0] == 1 && c.buf_bytes[NR_BUF_INDEX][0] == 600 && c.alive[NR_K_VB] == 2 && c.alive[NR_K_IB] == 1);
    CHECK(!nr_buffer_kill(r, NR_BUF_VERTEX, H(1, 2)) && nr_buffer_kill(r, NR_BUF_VERTEX, H(1, 1)) && !nr_buffer_kill(r, NR_BUF_VERTEX, H(1, 1)) && r->st[NR_K_VB].kill_miss == 2);
    nr_buffer *vb = nr_buffer_put(r, NR_BUF_VERTEX, H(1, 2), 2000, 0, 0, 0, 0xb3, 2000);     // the slot again, next generation
    CHECK(vb && vb->creates == 2 && vb->destroys == 1 && vb->replaced == 0);
    vb = nr_buffer_put(r, NR_BUF_VERTEX, H(2, 2), 700, 0, 0, NR_USAGE_DYNAMIC, 0xb4, 700);   // reuse with no destroy seen
    CHECK(vb && vb->replaced == 1 && r->st[NR_K_VB].replaced == 1);
    nr_census_take(r, &c);
    CHECK(c.buf_bytes[NR_BUF_VERTEX][0] == 2000 && c.buf_bytes[NR_BUF_VERTEX][1] == 700);
}

static void test_shaders(nr_reg *r) {
    const uint64_t prog[NR_STAGES] = {0xa000, 0xa100, 0};
    const char key[NR_STAGES][17] = {"0123456789abcdef", "fedcba9876543210", ""};
    nr_shader *s = nr_shader_put(r, H(5, 1), "Deferred_Opaque.bshd", "Shaders/Deferred_Opaque.bshd", prog, key);
    CHECK(s && s->alive && !strcmp(s->name, "Deferred_Opaque.bshd") && !strcmp(s->path, "Shaders/Deferred_Opaque.bshd") && s->prog[1] == 0xa100 && !strcmp(s->key[0], "0123456789abcdef"));
    CHECK(nr_check_shader(s) == 0 && nr_shader_get(r, H(5, 1)) == s && !nr_shader_get(r, H(5, 2)));
    // a cache hit of ShaderManager::Get: counted; an unnamed shader gets the name; an unknown value is a miss
    CHECK(nr_shader_note_get(r, H(5, 1), "other") == 1 && s->gets == 1 && !strcmp(s->name, "Deferred_Opaque.bshd"));
    const uint64_t pc[NR_STAGES] = {0, 0, 0xa200};
    const char kc[NR_STAGES][17] = {"", "", "00000000000000aa"};
    nr_shader *c = nr_shader_put(r, H(6, 1), NULL, NULL, pc, kc);
    CHECK(c && !c->name[0] && !c->path[0] && nr_check_shader(c) == 0);
    CHECK(nr_shader_note_get(r, H(6, 1), "Compute.bshd") == 1 && !strcmp(c->name, "Compute.bshd"));
    CHECK(nr_shader_note_get(r, 0x7f0012345678ull, "x") == 0 && nr_shader_note_get(r, 0, NULL) == 0 && r->shader_gets == 4 && r->shader_get_miss == 2);
    // checks: no program, a program without a key, compute mixed with graphics
    const uint64_t p0[NR_STAGES] = {0, 0, 0};
    CHECK(nr_check_shader(nr_shader_put(r, H(7, 1), "a", "", p0, NULL)) == NR_BAD_NO_NVN);
    const char k1[NR_STAGES][17] = {"0123456789abcdef", "", ""};
    CHECK(nr_check_shader(nr_shader_put(r, H(7, 2), "a", "", prog, k1)) == NR_BAD_PROGRAM);
    const uint64_t p3[NR_STAGES] = {0xa000, 0, 0xa200}; const char k3[NR_STAGES][17] = {"0123456789abcdef", "", "00000000000000aa"};
    CHECK(nr_check_shader(nr_shader_put(r, H(7, 3), "a", "", p3, k3)) == NR_BAD_PROGRAM);
    CHECK(r->shader[7].replaced == 2 && r->st[NR_K_SHADER].replaced == 2);
    // names: bounded, tabs / control characters cannot break the TSV
    std::string big(400, 'n');
    s = nr_shader_put(r, H(8, 1), big.c_str(), "a\tb\nc", prog, key);
    CHECK(s && strlen(s->name) == NR_NAME - 1 && !strcmp(s->path, "a?b?c"));
    CHECK(!nr_shader_put(r, H(NR_SHADER_MAX, 1), "x", "", prog, key) && !nr_shader_put(r, 8, "x", "", prog, key));
}

static void test_sets(nr_reg *r) {
    const uint64_t a[3] = {H(1, 1), H(2, 1), H(3, 1)}, b[3] = {H(1, 1), H(2, 1), H(4, 1)};
    CHECK(nr_rtset_note(r, 3, a, H(9, 1)) == 1 && nr_rtset_note(r, 3, a, H(9, 1)) == 0 && nr_rtset_note(r, 3, b, H(9, 1)) == 1);
    CHECK(nr_rtset_note(r, 3, a, 0) == 1 && nr_rtset_note(r, 2, a, H(9, 1)) == 1 && nr_rtset_note(r, 0, NULL, H(9, 1)) == 1 && nr_rtset_note(r, 0, NULL, H(9, 1)) == 0);
    CHECK(r->nrtset == 5 && r->rtset_calls == 7 && r->rtset_lost == 0);
    CHECK(nr_rtset_note(r, 9, a, 0) == -1 && r->rtset_lost == 1);                    // more colour targets than the engine has: not stored
    uint64_t total = 0; for (unsigned i = 0; i < NR_RTSET_MAX; i++) total += r->rtset[i].count;
    CHECK(total == 7);
    // the table is bounded: past 3/4 full new sets are counted as lost, known ones still count
    for (uint32_t i = 0; i < NR_RTSET_MAX; i++) { const uint64_t c = H(1000 + i, 1); nr_rtset_note(r, 1, &c, 0); }
    CHECK(r->nrtset == NR_RTSET_MAX * 3 / 4 && r->rtset_lost == 1 + NR_RTSET_MAX - (NR_RTSET_MAX * 3 / 4 - 5));
    CHECK(nr_rtset_note(r, 3, a, H(9, 1)) == 0);
    // named transitions (engine opcode 55): key = (name pointer, state)
    CHECK(nr_trans_note(r, 0x1f00100, "DeferredRenderStage::GBuffer1", H(1, 1), 11) == 1 && nr_trans_note(r, 0x1f00100, NULL, H(1, 1), 11) == 0);
    CHECK(nr_trans_note(r, 0x1f00100, "DeferredRenderStage::GBuffer1", H(1, 1), 6) == 1 && nr_trans_note(r, 0, "", H(2, 1), 0) == 1);
    const nr_trans *t = nr_trans_find(r, 0x1f00100, 11);
    CHECK(t && t->count == 2 && t->handles == 1 && !strcmp(t->name, "DeferredRenderStage::GBuffer1") && !nr_trans_find(r, 0x1f00100, 7) && !nr_trans_find(r, 0x1f00200, 11));
    CHECK(nr_trans_note(r, 0x1f00100, NULL, H(1, 2), 11) == 0 && t->handles == 2 && t->handle == H(1, 2) && t->count == 3);   // the RT was recreated
    for (uint32_t i = 0; i < NR_TRANS_MAX; i++) nr_trans_note(r, 0x2000000 + i * 8, "n", H(1, 1), 1);
    CHECK(r->ntrans == NR_TRANS_MAX * 3 / 4 && r->trans_lost == NR_TRANS_MAX - (NR_TRANS_MAX * 3 / 4 - 3) && nr_trans_find(r, 0x1f00100, 11) == t);
    // shared RT pool reservations
    CHECK(nr_pool_note(r, 0xabc, 20, 1280, 720) == 1 && nr_pool_note(r, 0xabc, 20, 1280, 720) == 0 && nr_pool_note(r, 0xabc, 20, 640, 360) == 1 && r->npool == 2 && r->pool[0].count == 2);
    for (uint32_t i = 0; i < NR_POOL_MAX + 4; i++) nr_pool_note(r, 0x100000 + i, 4, 8, 8);
    CHECK(r->npool == NR_POOL_MAX && r->pool_lost == 6);
}

static std::vector<std::vector<std::string>> read_tsv(const nr_reg *r, int which, int *rows) {
    std::vector<std::vector<std::string>> out;
    FILE *o = tmpfile();
    if (!o) { printf("FAIL: tmpfile\n"); ++failures; return out; }
    *rows = nr_write_tsv(r, which, o);
    rewind(o);
    std::string line; int ch;
    auto flush = [&] { std::vector<std::string> f; size_t p = 0; for (;;) { const size_t q = line.find('\t', p); f.push_back(line.substr(p, q == std::string::npos ? q : q - p)); if (q == std::string::npos) break; p = q + 1; } out.push_back(f); line.clear(); };
    while ((ch = fgetc(o)) != EOF) { if (ch == '\n') flush(); else line.push_back((char)ch); }
    CHECK(line.empty());   // the file ends with a newline
    fclose(o);
    return out;
}
static std::map<std::string, size_t> columns(const std::vector<std::string> &head) { std::map<std::string, size_t> m; for (size_t i = 0; i < head.size(); i++) m[head[i]] = i; return m; }
static uint64_t num(const std::string &s) { return strtoull(s.c_str(), NULL, 0); }

static void test_summary_tsv() {
    nr_reg *r = nr_reg_new();
    CHECK(r != NULL);
    if (!r) return;
    char sum[900];
    int n = nr_summary(r, sum, sizeof sum);
    CHECK(n > 0 && (size_t)n == strlen(sum) && strstr(sum, "textures 0 alive") && !strchr(sum, '\n'));
    // a small world: 3 textures (one destroyed), 2 shaders, 1 sampler, states, a vertex format, buffers, sets
    nr_tex_desc e1 = desc(NR_SRC_IMAGE_PACKAGED, 55, 1024, 1024, 1, 11, NR_TF_SRGB), e2 = desc(NR_SRC_RT, 20, 1280, 720, 0, 1, 0x20), e3 = desc(NR_SRC_CREATE_DATA, 4, 16, 16, 0, 1);
    nr_nvn_tex n1 = nvn(0x7000001000ull, 0x48, 1024, 1024, 1, 11, 1, 1), n2 = nvn(0x7000002000ull, 0x29, 1280, 720, 1, 1), n3 = nvn(0x7000003000ull, 0x25, 16, 16, 1, 1);
    n1.size = 1398128; n2.size = 7372800; n3.size = 1024; n2.flags = 8; n1.samples = 0;
    nr_tex *t1 = nr_tex_put(r, H(100, 1), &e1, &n1, 1); nr_tex_put(r, H(101, 7), &e2, &n2, 1); nr_tex_put(r, H(5, 1), &e3, &n3, 1);
    t1->bad = nr_check_tex(t1);
    nr_tex_kill(r, H(5, 1));
    const uint64_t prog[NR_STAGES] = {0xa000, 0xa100, 0}; const char key[NR_STAGES][17] = {"0123456789abcdef", "fedcba9876543210", ""};
    nr_shader_put(r, H(1, 1), "Deferred_Opaque.bshd", "Shaders/Deferred_Opaque.bshd", prog, key);
    const uint64_t pc[NR_STAGES] = {0, 0, 0xa200}; const char kc[NR_STAGES][17] = {"", "", "00000000000000aa"};
    nr_shader_put(r, H(2, 1), NULL, NULL, pc, kc);
    nr_shader_note_get(r, H(1, 1), "Deferred_Opaque.bshd");
    nr_nvn_smp ns; memset(&ns, 0, sizeof ns); ns.obj = 0x500; ns.minf = 5; ns.magf = 1; ns.ws = ns.wt = ns.wr = 1; ns.aniso = 16.f;
    nr_sampler_put(r, H(3, 1), 5, 0, 0, 0, 16, 0, &ns);
    const uint8_t b1[6] = {6, 7, 1, 7, 0, 0}, d1[8] = {1, 0, 2, 1, 0x7e, 4, 0, 2}, r1[5] = {2, 0, 1, 0, 1};
    nr_state_put(r, NR_ST_BLEND, H(0, 1), b1, 6); nr_state_put(r, NR_ST_DEPTH, H(1, 1), d1, 8); nr_state_put(r, NR_ST_RASTER, H(2, 1), r1, 5);
    const uint8_t el[12] = {0, 1, 0, 0, 5, 3, 0, 2, 0, 0, 5, 2};
    nr_vformat_put(r, H(4, 1), el, 2);
    nr_buffer_put(r, NR_BUF_VERTEX, H(1, 1), 4096, 0, 0, 0, 0xb0, 4096); nr_buffer_put(r, NR_BUF_INDEX, H(1, 1), 600, 300, 2, NR_USAGE_DYNAMIC, 0xb2, 600);
    const uint64_t cols[2] = {H(101, 7), H(100, 1)};
    nr_rtset_note(r, 2, cols, H(9, 1)); nr_rtset_note(r, 2, cols, H(9, 1)); nr_rtset_note(r, 1, cols, 0);
    nr_trans_note(r, 0x1f00100, "DeferredRenderStage::GBuffer1", H(101, 7), 11); nr_trans_note(r, 0x1f00100, NULL, H(101, 7), 11); nr_trans_note(r, 0, NULL, H(100, 1), 0);
    nr_pool_note(r, 0xabc, 20, 1280, 720);

    nr_census c; nr_census_take(r, &c);
    CHECK(c.alive[NR_K_TEX] == 2 && c.tex_src[NR_SRC_IMAGE_PACKAGED] == 1 && c.tex_src[NR_SRC_RT] == 1 && c.tex_src[NR_SRC_CREATE_DATA] == 0);
    CHECK(c.tex_src_bytes[NR_SRC_IMAGE_PACKAGED] == 1398128 && c.tex_cls_bytes[NR_CLS_BC] == 1398128 && c.tex_cls_bytes[NR_CLS_HDR] == 7372800 && c.tex_bytes == 1398128 + 7372800);
    CHECK(c.alive[NR_K_SHADER] == 2 && c.shaders_named == 1 && c.programs == 3 && c.programs_keyed == 3 && c.alive[NR_K_SAMPLER] == 1 && c.alive[NR_K_VFORMAT] == 1);
    CHECK(c.alive[NR_K_BLEND] == 1 && c.alive[NR_K_DEPTH] == 1 && c.alive[NR_K_RASTER] == 1 && c.buf_bytes[NR_BUF_INDEX][1] == 600 && c.tex_no_nvn == 0);
    n = nr_summary(r, sum, sizeof sum);
    CHECK((size_t)n == strlen(sum) && strstr(sum, "textures 2 alive (3 created, 1 destroyed, 0 replaced, 8.4 MB)") && strstr(sum, "shaders 2 (1 named, 3 programs, 3 keyed)"));
    CHECK(strstr(sum, "samplers 1, vformats 1, vb 1, ib 1, blend 1, depth-stencil 1, raster 1, rt sets 2, transitions 2, pools 1; overflow 0, bad handles 0"));
    char tiny[24]; n = nr_summary(r, tiny, sizeof tiny);
    CHECK(n == (int)sizeof tiny - 1 && strlen(tiny) == sizeof tiny - 1);          // bounded

    // TSV read back: header + rows; every field of the objects above
    int rows = 0;
    auto f = read_tsv(r, NR_TSV_FORMATS, &rows);
    CHECK(rows == NR_FMT_N && f.size() == NR_FMT_N + 1u);
    if (f.size() == NR_FMT_N + 1u) {
        auto m = columns(f[0]);
        for (unsigned i = 0; i < NR_FMT_N; i++) {
            const auto &row = f[i + 1];
            CHECK(row.size() == f[0].size() && num(row[m["engine_fmt"]]) == i && row[m["name"]] == nr_formats[i].name && num(row[m["nvn"]]) == nr_formats[i].nvn &&
                  num(row[m["nvn_srgb"]]) == nr_formats[i].nvn_srgb && num(row[m["bytes"]]) == nr_formats[i].bytes && num(row[m["compressed"]]) == nr_formats[i].compressed);
        }
    }
    auto tx = read_tsv(r, NR_TSV_TEXTURES, &rows);
    CHECK(rows == 3 && tx.size() == 4u);
    if (tx.size() == 4u) {
        auto m = columns(tx[0]);
        CHECK(m.size() == tx[0].size() && m.count("handle") && m.count("nvn_fmt") && m.count("packaged") && m.count("bad"));
        for (size_t i = 1; i < tx.size(); i++) {
            const auto &row = tx[i];
            CHECK(row.size() == tx[0].size());
            const uint64_t h = num(row[m["handle"]]);
            const nr_tex *t = &r->tex[nr_h_idx(h)];
            CHECK(t->handle == h && num(row[m["alive"]]) == t->alive && row[m["source"]] == nr_src_name(t->src) && num(row[m["engine_fmt"]]) == t->efmt && row[m["fmt_name"]] == nr_fmt_name(t->efmt));
            CHECK(num(row[m["w"]]) == t->w && num(row[m["h"]]) == t->h && num(row[m["d"]]) == t->d && num(row[m["mips"]]) == t->mips && num(row[m["layers"]]) == t->layers && num(row[m["msaa"]]) == t->msaa);
            CHECK(num(row[m["flags"]]) == t->eflags && num(row[m["flags2"]]) == t->eflags2 && num(row[m["srgb"]]) == t->srgb && atoi(row[m["fmt_override"]].c_str()) == t->fmt_override);
            CHECK(num(row[m["nvn_obj"]]) == t->nvn.obj && num(row[m["nvn_fmt"]]) == (uint64_t)t->nvn.format && atoi(row[m["nvn_target"]].c_str()) == t->nvn.target && num(row[m["nvn_w"]]) == t->nvn.w &&
                  num(row[m["nvn_h"]]) == t->nvn.h && num(row[m["nvn_d"]]) == t->nvn.d && num(row[m["nvn_levels"]]) == t->nvn.levels && num(row[m["nvn_flags"]]) == (uint64_t)t->nvn.flags);
            CHECK(num(row[m["nvn_size"]]) == t->nvn.size && num(row[m["packaged"]]) == t->nvn.packaged && num(row[m["nvn_count"]]) == t->nnvn && num(row[m["creates"]]) == t->creates &&
                  num(row[m["destroys"]]) == t->destroys && num(row[m["replaced"]]) == t->replaced && num(row[m["bad"]]) == t->bad);
        }
        CHECK(num(tx[1][m["handle"]]) == H(5, 1) && num(tx[1][m["alive"]]) == 0 && tx[2][m["source"]] == "image-packaged" && tx[3][m["fmt_name"]] == "RGBA16F");   // by handle index; the destroyed one stays listed
    }
    auto sh = read_tsv(r, NR_TSV_SHADERS, &rows);
    CHECK(rows == 2 && sh.size() == 3u);
    if (sh.size() == 3u) {
        auto m = columns(sh[0]);
        CHECK(num(sh[1][m["handle"]]) == H(1, 1) && sh[1][m["name"]] == "Deferred_Opaque.bshd" && sh[1][m["path"]] == "Shaders/Deferred_Opaque.bshd" && num(sh[1][m["vs_prog"]]) == 0xa000 &&
              sh[1][m["vs_key"]] == "0123456789abcdef" && num(sh[1][m["fs_prog"]]) == 0xa100 && sh[1][m["fs_key"]] == "fedcba9876543210" && num(sh[1][m["cs_prog"]]) == 0 && sh[1][m["cs_key"]] == "-" &&
              num(sh[1][m["gets"]]) == 1 && num(sh[1][m["creates"]]) == 1 && sh[1].size() == sh[0].size());
        CHECK(sh[2][m["name"]] == "-" && sh[2][m["path"]] == "-" && sh[2][m["cs_key"]] == "00000000000000aa" && num(sh[2][m["cs_prog"]]) == 0xa200 && sh[2][m["vs_key"]] == "-" && sh[2].size() == sh[0].size());
    }
    auto sm = read_tsv(r, NR_TSV_SAMPLERS, &rows);
    CHECK(rows == 1 && sm.size() == 2u);
    if (sm.size() == 2u) { auto m = columns(sm[0]); CHECK(num(sm[1][m["handle"]]) == H(3, 1) && num(sm[1][m["filter"]]) == 5 && num(sm[1][m["aniso"]]) == 16 && num(sm[1][m["nvn_min"]]) == 5 && sm[1][m["nvn_aniso"]] == "16" && sm[1].size() == sm[0].size()); }
    auto st = read_tsv(r, NR_TSV_STATES, &rows);
    CHECK(rows == 3 && st.size() == 4u);
    if (st.size() == 4u) {
        auto m = columns(st[0]);
        CHECK(st[1][m["kind"]] == "blend" && num(st[1][m["nargs"]]) == 6 && num(st[1][m["a0"]]) == 6 && num(st[1][m["a3"]]) == 7 && st[1].size() == st[0].size());
        CHECK(st[2][m["kind"]] == "depth-stencil" && num(st[2][m["a4"]]) == 0x7e && num(st[2][m["a7"]]) == 2 && st[3][m["kind"]] == "raster" && num(st[3][m["handle"]]) == H(2, 1));
    }
    auto vf = read_tsv(r, NR_TSV_VFORMATS, &rows);
    CHECK(rows == 1 && vf.size() == 2u && vf[1].size() == vf[0].size() && vf[1][4] == "0.1.0.0.5.3 0.2.0.0.5.2" && num(vf[1][2]) == 2);
    auto bf = read_tsv(r, NR_TSV_BUFFERS, &rows);
    CHECK(rows == 2 && bf.size() == 3u);
    if (bf.size() == 3u) {
        auto m = columns(bf[0]);
        CHECK(bf[1][m["kind"]] == "vertex" && num(bf[1][m["size"]]) == 4096 && bf[1][m["class"]] == "static" && num(bf[1][m["nvn_obj"]]) == 0xb0 && bf[1].size() == bf[0].size());
        CHECK(bf[2][m["kind"]] == "index" && num(bf[2][m["count"]]) == 300 && num(bf[2][m["index_size"]]) == 2 && bf[2][m["class"]] == "dynamic" && num(bf[2][m["usage"]]) == NR_USAGE_DYNAMIC);
    }
    auto rs = read_tsv(r, NR_TSV_RTSETS, &rows);
    CHECK(rows == 2 && rs.size() == 3u);
    if (rs.size() == 3u) {
        auto m = columns(rs[0]);
        const auto &two = num(rs[1][m["n"]]) == 2 ? rs[1] : rs[2], &one = num(rs[1][m["n"]]) == 2 ? rs[2] : rs[1];
        CHECK(num(two[m["count"]]) == 2 && num(two[m["depth"]]) == H(9, 1) && num(two[m["c0"]]) == H(101, 7) && num(two[m["c1"]]) == H(100, 1) && num(two[m["c2"]]) == 0 && num(two[m["seq"]]) == 1);
        CHECK(num(one[m["count"]]) == 1 && num(one[m["n"]]) == 1 && num(one[m["depth"]]) == 0 && num(one[m["c0"]]) == H(101, 7) && num(one[m["c1"]]) == 0 && one.size() == rs[0].size());
    }
    auto tr = read_tsv(r, NR_TSV_TRANSITIONS, &rows);
    CHECK(rows == 2 && tr.size() == 3u);
    if (tr.size() == 3u) {
        auto m = columns(tr[0]);
        const auto &g = tr[1][m["name"]] == "-" ? tr[2] : tr[1], &u = tr[1][m["name"]] == "-" ? tr[1] : tr[2];
        CHECK(g[m["name"]] == "DeferredRenderStage::GBuffer1" && num(g[m["state"]]) == 11 && num(g[m["handle"]]) == H(101, 7) && num(g[m["count"]]) == 2 && num(g[m["name_key"]]) == 0x1f00100);
        CHECK(u[m["name"]] == "-" && num(u[m["state"]]) == 0 && num(u[m["count"]]) == 1 && u.size() == tr[0].size());
    }
    auto pl = read_tsv(r, NR_TSV_POOLS, &rows);
    CHECK(rows == 1 && pl.size() == 2u && num(pl[1][1]) == 0xabc && pl[1][3] == "RGBA16F" && num(pl[1][4]) == 1280 && num(pl[1][5]) == 720 && num(pl[1][6]) == 1);
    FILE *o = tmpfile();
    if (o) { CHECK(nr_write_tsv(r, NR_TSV_N, o) == -1 && nr_write_tsv(r, -1, o) == -1); fclose(o); }
    for (int w = 0; w < NR_TSV_N; w++) { const char *nm = nr_tsv_name(w); CHECK(!strncmp(nm, "nr_", 3) && strstr(nm, ".tsv") && strcmp(nm, "nr_unknown.tsv")); }
    CHECK(!strcmp(nr_tsv_name(NR_TSV_N), "nr_unknown.tsv") && !strcmp(nr_tsv_name(NR_TSV_TEXTURES), "nr_textures.tsv") && !strcmp(nr_tsv_name(NR_TSV_SHADERS), "nr_shaders.tsv"));
    nr_reg_free(r);
}

int main() {
    test_formats();
    nr_reg *r = nr_reg_new();
    CHECK(r != NULL);
    if (r) {
        test_textures(r);
        test_checks(r);
        test_small_kinds(r);
        test_shaders(r);
        test_sets(r);
        nr_reg_free(r);
    }
    nr_reg_free(NULL);
    test_summary_tsv();
    if (failures) { printf("%d of %d checks FAILED\n", failures, checks); return 1; }
    printf("PASS: nr_reg (%d checks): formats, textures put / kill / generation reuse, bounded tables, checks, samplers / states / vertex formats / buffers, shaders, "
           "render-target sets, transitions, pools, census, summary, TSV read back\n", checks);
    return 0;
}
