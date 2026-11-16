// [dos2-nb] NB-18b-2: host test of the UI / post-tail audit (dm/gfx/nb/nb_ui.h, used by xbg_nb_ui.inc under DM_NB_UIPASS=2 / DM_NB_POST=2).
// Synthetic frames shaped like P2 (src/work/logs/p2b.log): campaign f=2509 (portrait mini-pipeline, UI + 2 Iggy filter chains, 3D, LUT >
// Sight > FXAA > Vignette > CombineUI_NoFetch), menu f=835 (one UI pass, no Vignette), AA off (no FXAA / PPDefault copy), failure cases.
// Build, from this folder:  clang++ -std=c++17 -Wall -D_CRT_SECURE_NO_WARNINGS nb_ui_test.cpp -o nb_ui_test   (or g++)
#include "../../../nb/nb_ui.h"
#include <stdio.h>
#include <string.h>
#include <vector>

static int failures;
#define CHECK(expression) do { if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)

enum : uint64_t { PORT = 0x100, ATLAS = 0x200, UI = 0x5700, UIDS = 0x5800, F83 = 0x8300, F85 = 0x8500, HDR = 0x27100, LDEP = 0x26600,
                  LUT3 = 0x28300, P130 = 0x13000, P129 = 0x12900, P94 = 0x9400, FIN = 0x9300, CONE = 0x30000, DEP3 = 0x40000 };
// NVN blend: premultiplied over, additive keeping alpha, opaque
struct Bl { int en; unsigned sr, dr, sa, da, er, ea, mask; };
static const Bl OVER = {1, 2, 6, 2, 6, 1, 1, 15}, OPAQUE = {0, 2, 1, 2, 1, 1, 1, 15}, ADDK = {1, 2, 2, 1, 2, 1, 1, 15};

struct Frame {
    std::vector<nb_ui_pass> p;
    nb_ui_pass &pass(int sh, uint32_t w, uint32_t h, uint64_t c0, uint64_t ds, std::initializer_list<uint64_t> rd, uint32_t draws = 1,
                     const Bl &b = OPAQUE, int ds_used = 0, int32_t rx0 = 0, int32_t ry0 = 0, int32_t rx1 = -1, int32_t ry1 = -1) {
        nb_ui_pass q; nb_ui_pass_init(&q); q.color[0] = c0; q.ncolor = c0 ? 1 : 0; q.depth = ds; q.w = w; q.h = h; q.sh = sh; q.draws = draws;
        q.sh_mask = 1u << sh; q.blend[nb_ui_blend_class(b.en, b.sr, b.dr, b.sa, b.da, b.er, b.ea, b.mask)] = draws;
        if (ds_used) q.dt = q.st = 1;
        for (uint64_t r : rd) nb_ui_add_read(&q, r);
        nb_ui_add_rect(&q, rx0, ry0, rx1 < 0 ? (int32_t)w : rx1, ry1 < 0 ? (int32_t)h : ry1);
        p.push_back(q); return p.back();
    }
    void portrait() {   // p0..p19: 80x100 mini pipeline ending in LUT > FXAA into #0 (read by the UI)
        for (int i = 0; i < 18; i++) pass(NB_UISH_OTHER, 80, 100, PORT + 1 + (uint64_t)i, 0, {});
        pass(NB_UISH_LUT, 80, 100, PORT + 50, 0, {PORT + 17, LUT3});
        pass(NB_UISH_FXAA, 80, 100, PORT, 0, {PORT + 50});
    }
    void chain(int n) {   // a filter chain: fill without inputs, blurs ping-pong #83 / #85, DS #58 attached and unused
        pass(NB_UISH_IGGY_FILL, 2400, 1080, F83, UIDS, {}, 1, OPAQUE, 0, 1000, 500, 1100, 560);
        for (int i = 0; i < n; i++) pass(NB_UISH_IGGY_BLUR, 2400, 1080, i & 1 ? F83 : F85, UIDS, {i & 1 ? F85 : F83}, 1, OPAQUE, 0, 990, 490, 1110, 570);
    }
    void tail3d(bool vig, int aa) {   // 3D, then LUT > Sight > (FXAA | PPDefault | none) > (Vignette) ; returns the combine's Base
        pass(NB_UISH_OTHER, 2160, 972, LDEP, DEP3, {}, 300, OPAQUE, 1);
        pass(NB_UISH_OTHER, 2160, 972, HDR, DEP3, {LDEP}, 500, OPAQUE, 1);
        pass(NB_UISH_LUT, 2160, 972, P130, 0, {HDR, LUT3});
        pass(NB_UISH_SIGHT, 2160, 972, P129, 0, {P130, LDEP, CONE});
        uint64_t last = P129;
        if (aa == 1) { pass(NB_UISH_FXAA, 2160, 972, P130, 0, {P129}); last = P130; }
        if (aa == 2) { pass(NB_UISH_PPDEF, 2160, 972, P130, 0, {P129}); last = P130; }
        if (vig) { pass(NB_UISH_VIG, 2160, 972, P94, 0, {last}); last = P94; }
        base = last;
    }
    uint64_t base = 0;
    nb_ui_res combine(int sh = NB_UISH_COMB_NOFETCH, float py = 1.0f) {
        pass(sh, 2400, 1080, FIN, 0, {UI, base});
        nb_ui_comb c{}; c.ui = UI; c.base = base; c.have_c5 = 1; c.params[1] = py;
        nb_ui_res r; nb_ui_analyze(p.data(), (int)p.size(), &c, &r); return r;
    }
};

static void campaign(Frame &f, bool vig, int aa) {
    f.portrait();
    f.pass(NB_UISH_IGGY, 2400, 1080, UI, UIDS, {PORT, ATLAS}, 95, OVER, 1);   // p21
    f.chain(6);                                                                     // p22..p28
    f.pass(NB_UISH_IGGY, 2400, 1080, UI, UIDS, {F85}, 7, OVER);                    // p29
    f.chain(6);                                                                     // p30..p36
    f.pass(NB_UISH_IGGY, 2400, 1080, UI, UIDS, {F85}, 1, OVER);                    // p37
    f.tail3d(vig, aa);
}

int main() {
    // ---- blend classes
    CHECK(nb_ui_blend_class(1, 2, 6, 2, 6, 1, 1, 15) == NB_UIB_OVER);
    CHECK(nb_ui_blend_class(1, 5, 6, 2, 6, 1, 1, 15) == NB_UIB_OVER);          // straight-alpha source, same accumulation
    CHECK(nb_ui_blend_class(1, 5, 6, 5, 6, 1, 1, 15) == NB_UIB_OTHER);         // alpha SRC_ALPHA: a^2
    CHECK(nb_ui_blend_class(1, 2, 6, 2, 6, 1, 1, 7) == NB_UIB_NOALPHA);
    CHECK(nb_ui_blend_class(1, 2, 2, 1, 2, 1, 1, 15) == NB_UIB_ADD_KEEPA);
    CHECK(nb_ui_blend_class(1, 5, 2, 0, 0, 1, 1, 7) == NB_UIB_ADD_KEEPA);
    CHECK(nb_ui_blend_class(1, 2, 2, 2, 2, 1, 1, 15) == NB_UIB_ADD_A);
    CHECK(nb_ui_blend_class(1, 9, 1, 2, 6, 1, 1, 15) == NB_UIB_DST);           // multiply
    CHECK(nb_ui_blend_class(1, 2, 6, 8, 2, 1, 1, 15) == NB_UIB_DST);
    CHECK(nb_ui_blend_class(1, 2, 6, 8, 2, 1, 1, 7) == NB_UIB_NOALPHA);        // dst alpha factor, but alpha not written
    CHECK(nb_ui_blend_class(0, 2, 1, 2, 1, 1, 1, 15) == NB_UIB_OFF);
    CHECK(nb_ui_blend_class(0, 2, 1, 2, 1, 1, 1, 0) == NB_UIB_NOCOLOR);
    CHECK(nb_ui_blend_class(1, 0x61, 6, 2, 6, 1, 1, 15) == NB_UIB_OTHER);
    CHECK(nb_ui_blend_exact(NB_UIB_OVER) && nb_ui_blend_exact(NB_UIB_NOCOLOR) && !nb_ui_blend_exact(NB_UIB_ADD_KEEPA));
    // ---- shader classes
    CHECK(nb_ui_shader_class("98186c22deadbeef") == NB_UISH_COMB_NOFETCH);
    CHECK(nb_ui_shader_class("7b3210191abd0000") == NB_UISH_PPDEF);
    CHECK(nb_ui_shader_class("7b3210190000") == NB_UISH_OTHER);
    CHECK(nb_ui_shader_class(nullptr) == NB_UISH_OTHER);
    CHECK(nb_ui_is_combine(NB_UISH_COMB_HDR) && !nb_ui_is_combine(NB_UISH_VIG));

    {   // campaign f=2509
        Frame f; campaign(f, true, 1); nb_ui_res r = f.combine();
        CHECK(!r.ui_why); CHECK(r.first == 20); CHECK(r.last == 36);
        CHECK(r.ui_passes == 3); CHECK(r.ui_draws == 103); CHECK(r.flt_passes == 14); CHECK(r.chains == 2);
        CHECK(r.flt_rts == 2); CHECK(r.rt_reused == 2);   // #83 / #85 of chain 1 written again by chain 2
        CHECK(r.blend_ui[NB_UIB_OVER] == 103); CHECK(r.blend_flt[NB_UIB_OFF] == 14);
        CHECK(r.ds_att == 17); CHECK(r.ds_used == 1); CHECK(r.flt_ds_unused == 14);
        CHECK(r.flt_small == 14); CHECK(r.flt_noin == 2); CHECK(r.flt_area < r.flt_rt_area / 100);
        CHECK(r.ui_readers == 0); CHECK(r.between == 6); CHECK(r.params_one == 1);
        CHECK(!r.post_why); CHECK(!strcmp(r.chain, "LUT>Sight>FXAA>Vignette>CombineUI_NoFetch")); CHECK(r.tail_n == 5);
        if (r.post_why) printf("  post: %s (%s)\n", r.post_why, r.chain);
    }
    {   // menu f=835: one UI pass (44 draws), LUT > Sight > FXAA > CombineUI_NoFetch
        Frame f; f.pass(NB_UISH_IGGY, 2400, 1080, UI, UIDS, {ATLAS}, 44, OVER); f.tail3d(false, 1);
        nb_ui_res r = f.combine(NB_UISH_COMB_NOFETCH, 2.2f);
        CHECK(!r.ui_why); CHECK(r.chains == 0); CHECK(r.ui_passes == 1); CHECK(r.params_one == 0);
        CHECK(!r.post_why); CHECK(!strcmp(r.chain, "LUT>Sight>FXAA>CombineUI_NoFetch"));
    }
    {   // AA off (DM_DOS2_AA=0): Vignette frame -> no FXAA pass; no Vignette -> the PPDefault copy
        Frame f; campaign(f, true, 0); nb_ui_res r = f.combine();
        CHECK(!r.post_why); CHECK(!strcmp(r.chain, "LUT>Sight>Vignette>CombineUI_NoFetch"));
        Frame g; campaign(g, false, 2); r = g.combine();
        CHECK(!r.post_why); CHECK(!strcmp(r.chain, "LUT>Sight>PPDefault>CombineUI_NoFetch"));
        Frame h; campaign(h, false, 0); r = h.combine();
        CHECK(!r.post_why); CHECK(!strcmp(r.chain, "LUT>Sight>CombineUI_NoFetch"));
    }
    {   // an intermediate with a second reader (Sight's output read by a later pass before FXAA rewrites the pair)
        Frame f; campaign(f, true, 1);
        nb_ui_pass x; nb_ui_pass_init(&x); x.sh = NB_UISH_OTHER; x.color[0] = 0x777; x.ncolor = 1; x.w = 2160; x.h = 972; nb_ui_add_read(&x, P129);
        f.p.insert(f.p.end() - 2, x);   // between Sight and FXAA
        nb_ui_res r = f.combine(); CHECK(r.post_why && !strcmp(r.post_why, "intermediate read by another pass"));
    }
    {   // an unknown shader between Vignette and Sight
        Frame f; campaign(f, true, 1); f.p[f.p.size() - 2].sh = NB_UISH_OTHER;   // FXAA's slot
        nb_ui_res r = f.combine(); CHECK(r.post_why && !strcmp(r.post_why, "unknown shader in the tail")); CHECK(r.post_bad_sh == NB_UISH_OTHER);
    }
    {   // Tex read by another pass (a UI layer reused as a texture) / not drawn / blend outside "over"
        Frame f; campaign(f, true, 1); f.pass(NB_UISH_OTHER, 100, 100, 0x999, 0, {UI}); nb_ui_res r = f.combine();
        CHECK(r.ui_readers == 1); CHECK(r.between == 7);
        Frame g; g.tail3d(true, 1); r = g.combine(); CHECK(r.ui_why && !strcmp(r.ui_why, "Tex not drawn this frame")); CHECK(!r.post_why);
        Frame h; h.pass(NB_UISH_IGGY, 2400, 1080, UI, UIDS, {}, 10, ADDK); h.tail3d(false, 1); r = h.combine();
        CHECK(r.blend_ui[NB_UIB_ADD_KEEPA] == 10); CHECK(r.blend_ui[NB_UIB_OVER] == 0);
    }
    {   // tail pass sizes differ (FXAA at another size)
        Frame f; campaign(f, true, 1); f.p[f.p.size() - 2].w = 1920; nb_ui_res r = f.combine();
        CHECK(r.post_why && !strcmp(r.post_why, "tail sizes differ"));
    }
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("nb_ui_test: all passed\n"); return 0;
}
