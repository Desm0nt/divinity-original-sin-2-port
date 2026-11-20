// [DOS2] NB-18b-4a host test: the post-tail fusion plan (nb_post_plan, dm/gfx/nb/nb_plan.c) on synthetic chains shaped like P2
// (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-18a findings" table: campaign f=2509) and on the P2 dump of that frame with the program facts
// (fs key + exact sampled reads) put in by hand for its five tail passes (the dumps predate the fs= lines).
// Build (Windows clang):
//   clang -std=c11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS nb_post_test.c ../../../nb/nb_plan.c -o nb_post_test
//   nb_post_test [<dir with nb_ir_04_f2509_p0.txt> [<dir with the ship deck dumps ra1-dumps>]]
//   (defaults: ../../../../../work/logs/p2b-nb and .../ra1-dumps, each skipped when missing)
// NB-18b-8 (route A): nb_post_aa_plan (dm/gfx/nb/nb_post_aa.h) on the same chains - F1A + the elided FXAA pass, its readers, refusals.
#include "../../../nb/nb_ir.h"
#include "../../../nb/nb_post_aa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { HDR = 0x2710, LUT3D = 0x2830, D = 0x2660, O1 = 0x1300, SIGHT = 0x1290, O2 = 0x940, WIN = 0x930, UI = 0x570, X = 0x7770,
       PH = 0x4440, PL = 0x4450, PO = 0x4460,
       DRT = 0x1181, FM = 0x3790 };   // ship deck: the pooled RGBA8 target that holds linear depth and later the FXAA output; a fog map   // the HUD portrait's mini-pipeline: its HDR, its LUT target, its FXAA target (80x100)
static const uint64_t K_LUT = 0x921728a5bbb1c71bull, K_SIGHT = 0x91eb2d97a304709aull, K_FXAA = 0x691768aa00000000ull, K_VIG = 0x43e8e8ed2b1df6c2ull,
                      K_COMB = 0x98186c22a7209861ull, K_OTHER = 0x1234567800000000ull, K_COPY = 0x7b3210191abd90cdull, K_FOG = 0xe94ea226e689801cull, K_OUTL = 0x010472595b342188ull;

// NB-18b-8 variants: copy_step (AntiAliasing 0, no Vignette: a PPDefault copy instead of FXAA), no_step (AntiAliasing 0 + Vignette: it
// reads Sight's target), outline (a step between Sight and FXAA: FXAA reads its target T), fxaa_inexact, op_reads (a copy from the
// FXAA target after the chain), s_written (a pass draws into Sight's target after FXAA), no_3d (a loading frame: only the combine).
typedef struct { int lut_inexact, lut_draws2, reader_after_sight, compute_between, vig_read_next, small_sight, sight_rt_size, no_vig;
                 int copy_step, no_step, outline, fxaa_inexact, op_reads, s_written, no_3d;
                 // NB-18b-9: portrait = the HUD portrait's own PPApplyLUT -> PPFXAA chain (80x100, nothing of the 3D tail): 1 after the
                 // combine (the phone's ship deck), 2 between Sight and the 3D FXAA pass, 3 before the 3D (P2); fxaa_twice = an FXAA pass
                 // over the combine's output (derived from Sight's target through the elided step, Vignette and the combine).
                 int portrait, fxaa_twice;
                 // NB-18b-10: fog = the ship deck's tail (dump ra1-dumps f5269): linear depth -> DRT ... LUT -> O1, Sight -> SIGHT, EoCFog
                 // (SIGHT + DRT + fog map) -> O1, FXAA (O1) -> DRT, combine (UI + DRT). fog_* = what is wrong with the fog pass;
                 // fog_no_aa = AntiAliasing 0 (no FXAA pass: the combine samples the fog's target).
                 int fog, fog_inexact, fog_draws2, fog_feedback, fog_small, fog_stale_depth, fog_rewrite, fog_no_aa;
                 // NR-33A: fj_outline = the Fort Joy tail (phone 18.11 frame 5240): Sight -> SIGHT, EoCOutlineCombine (SIGHT + mask) -> O1,
                 // EoCFog (O1 + DRT + fog map) -> SIGHT, FXAA (SIGHT) -> DRT; op_first = a copy from the FXAA target before anything writes it
                 int fj_outline, op_first; } variant;
static variant g_v;
static int sz(void *ctx, uint64_t im, uint32_t *w, uint32_t *h) {
    (void)ctx;
    if (im == WIN || im == UI) { *w = 2400; *h = 1080; return 1; }
    if (im == LUT3D) return 0;
    if (im == PH || im == PL || im == PO) { *w = 80; *h = 100; return 1; }
    if (im == FM) return 0;
    if (im == O1 && g_v.fog_small) { *w = 1080; *h = 486; return 1; }
    if (im == SIGHT && g_v.sight_rt_size) { *w = 2000; *h = 900; return 1; }
    *w = 2160; *h = 972; return 1;
}
static void pass1(nb_frame *f, uint64_t rt, uint64_t fs, int exact, const uint64_t *sr, int n, const uint64_t *stale, int ns) {
    nb_att a = { rt, 0 };
    nb_pass_begin(f, &a, 1);
    nb_draw(f, 0);
    for (int i = 0; i < n; i++) nb_read(f, -1, sr[i]);
    for (int i = 0; i < ns; i++) nb_read(f, -1, stale[i]);     // stale bindings: IR reads the program does not sample
    nb_draw_prog(f, fs, exact, sr, n);
}
static void portrait(nb_frame *f) {   // drawn every frame; the UI samples PO (not modelled: the combine's Tex is the UI layer)
    { const uint64_t s[1] = { X }; pass1(f, PH, K_OTHER, 1, s, 1, NULL, 0); }
    { const uint64_t s[2] = { PH, LUT3D }; pass1(f, PL, K_LUT, 1, s, 2, NULL, 0); }
    { const uint64_t s[1] = { PL }; pass1(f, PO, K_FXAA, 1, s, 1, NULL, 0); }
}
static void build_deck(nb_frame *f, uint32_t no) {
    nb_frame_reset(f, no);
    const uint32_t z[4] = { 0, 0, 0, 0 };
    if (!g_v.fog_stale_depth || no == 1) {   // LinearizeDepth: clear + draw into the pooled target (every frame on the phone)
        nb_clear(f, DRT, NB_ASP_COLOR, NB_CLEAR_FULL, z, 0, 0);
        const uint64_t s[1] = { D }; pass1(f, DRT, K_OTHER, 1, s, 1, NULL, 0);
    }
    { const uint64_t s[2] = { DRT, X }; pass1(f, HDR, K_OTHER, 1, s, 2, NULL, 0); }   // lighting samples the linear depth
    { const uint64_t s[2] = { HDR, LUT3D }; pass1(f, O1, K_LUT, 1, s, 2, NULL, 0); }
    nb_barrier(f);
    { const uint64_t s[2] = { O1, DRT }; pass1(f, SIGHT, K_SIGHT, 1, s, 2, NULL, 0); }
    if (g_v.fj_outline) {
        { const uint64_t s[2] = { SIGHT, FM }; pass1(f, O1, K_OUTL, 1, s, 2, NULL, 0); }
        { const uint64_t s[3] = { O1, DRT, FM }; pass1(f, SIGHT, K_FOG, 1, s, 3, NULL, 0); }
        { const uint64_t s[1] = { SIGHT }; pass1(f, DRT, K_FXAA, 1, s, 1, NULL, 0); }
        { const uint64_t s[2] = { UI, DRT }; pass1(f, WIN, K_COMB, 1, s, 2, NULL, 0); }
        { const uint64_t w = WIN; nb_op(f, NB_OP_PRESENT, &w, 1, NULL, 0); }
        nb_submit(f, 1); return;
    }
    { const uint64_t s[4] = { SIGHT, DRT, FM, O1 }; pass1(f, O1, K_FOG, !g_v.fog_inexact, s, g_v.fog_feedback ? 4 : 3, NULL, 0); if (g_v.fog_draws2) nb_draw(f, 0); }
    if (g_v.fog_rewrite) { const uint64_t s[1] = { X }; pass1(f, O1, K_OTHER, 1, s, 1, NULL, 0); }   // something draws over the fog's output
    if (!g_v.fog_no_aa) { const uint64_t s[1] = { O1 }; pass1(f, DRT, K_FXAA, 1, s, 1, NULL, 0); }
    { const uint64_t s[2] = { UI, g_v.fog_no_aa ? (uint64_t)O1 : (uint64_t)DRT }; pass1(f, WIN, K_COMB, 1, s, 2, NULL, 0); }
    { const uint64_t w = WIN; nb_op(f, NB_OP_PRESENT, &w, 1, NULL, 0); }
    nb_submit(f, 1);
}
// One frame of the campaign tail: (full clear of O2 like P2's line 10) 3D -> LUT -> barrier -> Sight -> FXAA -> [Vignette] -> Combine -> present
static void build(nb_frame *f, uint32_t no) {
    if (g_v.fog) { build_deck(f, no); return; }
    nb_frame_reset(f, no);
    const uint32_t z[4] = { 0, 0, 0, 0 };
    if (g_v.no_3d) {   // a loading frame: the combine alone samples last frame's post output
        if (g_v.no_3d == 2) nb_clear(f, O1, NB_ASP_COLOR, NB_CLEAR_FULL, z, 0, 0);   // ... after the game cleared it: a real write, no alias
        const uint64_t s[2] = { UI, O1 }, w = WIN; pass1(f, WIN, K_COMB, 1, s, 2, NULL, 0); nb_op(f, NB_OP_PRESENT, &w, 1, NULL, 0); nb_submit(f, 1); return;
    }
    if (g_v.op_first) { const uint64_t src = O1, dst = X; nb_op(f, NB_OP_COPY, &src, 1, &dst, 1); }   // reads last frame's (aliased) FXAA output
    if (g_v.vig_read_next) { const uint64_t s[1] = { O2 }; pass1(f, X, K_OTHER, 1, s, 1, NULL, 0); }   // last frame's O2 sampled first
    if (g_v.portrait == 3) portrait(f);
    nb_clear(f, O2, NB_ASP_COLOR, NB_CLEAR_FULL, z, 0, 0);
    { const uint64_t s[1] = { X }; pass1(f, HDR, K_OTHER, 1, s, 1, NULL, 0); }
    { const uint64_t s[2] = { HDR, LUT3D }, st[1] = { O2 }; pass1(f, O1, K_LUT, !g_v.lut_inexact, s, 2, st, 1); if (g_v.lut_draws2) nb_draw(f, 0); }
    if (g_v.compute_between) nb_op(f, NB_OP_COMPUTE, NULL, 0, NULL, 0);
    nb_barrier(f);
    { const uint64_t s[2] = { O1, D }, st[1] = { O2 }; pass1(f, SIGHT, K_SIGHT, 1, s, 2, st, 1); }
    if (g_v.reader_after_sight) { const uint64_t s[1] = { O1 }; pass1(f, X, K_OTHER, 1, s, 1, NULL, 0); }
    if (g_v.outline) { const uint64_t s[1] = { SIGHT }; pass1(f, X, K_OTHER, 1, s, 1, NULL, 0); }
    if (g_v.portrait == 2) portrait(f);
    if (!g_v.no_step) { const uint64_t s[1] = { g_v.outline ? (uint64_t)X : (uint64_t)SIGHT }, st[1] = { O2 };
        pass1(f, O1, g_v.copy_step ? K_COPY : K_FXAA, !g_v.fxaa_inexact, s, 1, st, 1); }
    if (g_v.s_written) { const uint64_t s[1] = { X }; pass1(f, SIGHT, K_OTHER, 1, s, 1, NULL, 0); }
    const uint64_t post = g_v.no_step ? (uint64_t)SIGHT : (uint64_t)O1;   // what the step after the AA step reads
    if (!g_v.no_vig) { const uint64_t s[1] = { post }, st[1] = { O2 }; pass1(f, O2, K_VIG, 1, s, 1, st, 1); }   // stale self binding -> feedback
    { const uint64_t s[2] = { UI, g_v.no_vig ? post : (uint64_t)O2 }; pass1(f, WIN, K_COMB, 1, s, 2, NULL, 0); }
    if (g_v.portrait == 1) portrait(f);
    if (g_v.fxaa_twice) { const uint64_t s[1] = { WIN }; pass1(f, X, K_FXAA, 1, s, 1, NULL, 0); }
    if (g_v.op_reads) { const uint64_t src = O1, dst = X; nb_op(f, NB_OP_COPY, &src, 1, &dst, 1); }
    { const uint64_t w = WIN; nb_op(f, NB_OP_PRESENT, &w, 1, NULL, 0); }
    nb_submit(f, 1);
}
static nb_frame F; static nb_plan_out P; static nb_persist PS; static nb_post_hist H; static nb_post_out O;
static int run(int frames, int *last_f1, int *last_f2) {
    nb_persist_reset(&PS); memset(&H, 0, sizeof H);
    const nb_rules r = { NB_RULES_EXACT, 0, 0 };
    const nb_post_rules pr = { 0, 8, sz, NULL };
    int first_f2 = -1;
    for (int i = 0; i < frames; i++) {
        build(&F, (uint32_t)i + 1);
        if (nb_plan(&F, &PS, &r, &P)) { CHECK(0, "nb_plan failed"); return -1; }
        const int n = nb_post_plan(&F, &P, &pr, &H, &O);
        CHECK(n >= 0, "nb_post_plan bad input");
        *last_f1 = O.fused[NB_FXK_F1]; *last_f2 = O.fused[NB_FXK_F2];
        if (O.fused[NB_FXK_F2] && first_f2 < 0) first_f2 = i;
    }
    return first_f2;
}
static int why_of(int fx) { for (int w = 0; w < NB_PW_KINDS; w++) if (O.why[fx][w]) return w; return -1; }
static int find_flag(unsigned fl) { for (int i = 0; i < F.nnode; i++) if (P.node[i].flags & fl) return i; return -1; }

static void synthetic(void) {
    int f1, f2, first;
    memset(&g_v, 0, sizeof g_v);
    first = run(12, &f1, &f2);
    CHECK(f1 == 1, "base: F1 fused every frame (got %d)", f1);
    CHECK(first == 8, "base: F2 needs 8 frames of history, fused from frame index 8 (got %d)", first);
    CHECK(f2 == 1, "base: F2 fused after the history (got %d)", f2);
    {   // flags on the right nodes
        int e = -1, h = -1, n = 0;
        for (int i = 0; i < F.nnode; i++) { if (P.node[i].flags & NB_NP_ELIDE) { n++; if (e < 0) e = i; } if ((P.node[i].flags & NB_NP_FUSE) && h < 0) h = i; }
        CHECK(n == 2 && e >= 0 && nb_post_class(F.node[e].fs) == NB_PSH_LUT && h >= 0 && nb_post_class(F.node[h].fs) == NB_PSH_SIGHT && O.peer[e] == h,
              "base: ELIDE on LUT and Vignette, FUSE on Sight / Combine (e %d h %d n %d)", e, h, n);
        CHECK(O.assumed_cover >= 1 && O.later_cover >= 1, "base: FXAA reloads O1 by cover_dc, Vignette's feedback disproved (later %d assumed %d)", O.later_cover, O.assumed_cover);
    }
    {   // dump round trip keeps fs / sread and gives the same post plan
        static char buf[1 << 16]; static nb_frame G; static nb_plan_out Q; static nb_post_out O2b; nb_post_hist h2 = H;
        nb_dump_str(&F, &P, buf, sizeof buf);
        CHECK(strstr(buf, "fs=921728a5bbb1c71b exact") && strstr(buf, "# post ") && strstr(buf, "sread 0x2710"), "dump has fs= / sread / # post lines");
        CHECK(nb_parse(buf, &G) == 0, "dump parses");
        int same = G.nnode == F.nnode;
        for (int i = 0; same && i < F.nnode; i++) same = G.node[i].fs == F.node[i].fs && G.node[i].fsx == F.node[i].fsx && G.node[i].nsr == F.node[i].nsr &&
                                                         !memcmp(G.node[i].sread, F.node[i].sread, sizeof(uint64_t) * F.node[i].nsr);
        CHECK(same, "parsed fs / sread equal");
        nb_persist ps2 = PS; const nb_rules r = { NB_RULES_EXACT, 0, 0 }; const nb_post_rules pr = { 0, 8, sz, NULL };
        nb_plan(&G, &ps2, &r, &Q); nb_post_plan(&G, &Q, &pr, &h2, &O2b);
        CHECK(O2b.fused[1] == 1 && O2b.fused[2] == 1, "parsed frame: same fusion (%d %d)", O2b.fused[1], O2b.fused[2]);
    }
    struct { const char *name; variant v; int fx, why; } neg[] = {
        { "LUT reads not exact", { .lut_inexact = 1 }, NB_FXK_F1, NB_PW_INEXACT },
        { "LUT pass with 2 draws", { .lut_draws2 = 1 }, NB_FXK_F1, NB_PW_ELIDED_SHAPE },
        { "another pass samples O1 after Sight", { .reader_after_sight = 1 }, NB_FXK_F1, NB_PW_OTHER_READER },
        { "compute between LUT and Sight", { .compute_between = 1 }, NB_FXK_F1, NB_PW_NO_ELIDED },
        { "Sight target of another size", { .sight_rt_size = 1 }, NB_FXK_F1, NB_PW_SIZE },
        { "O2 sampled before Vignette writes it", { .vig_read_next = 1 }, NB_FXK_F2, NB_PW_PERSIST },
        { "no Vignette (menu): FXAA before the combine", { .no_vig = 1 }, NB_FXK_F2, NB_PW_NO_ELIDED },
    };
    for (unsigned i = 0; i < sizeof neg / sizeof neg[0]; i++) {
        g_v = neg[i].v;
        run(12, &f1, &f2);
        const int got = why_of(neg[i].fx), fused = neg[i].fx == NB_FXK_F1 ? f1 : f2;
        CHECK(!fused && got == neg[i].why, "%s: expected '%s', got fused %d / '%s'", neg[i].name, nb_post_why_name(neg[i].why), fused, nb_post_why_name(got));
        if (neg[i].fx == NB_FXK_F2 && neg[i].why != NB_PW_NO_ELIDED) CHECK(f1 == 1, "%s: F1 unaffected", neg[i].name);
        else if (neg[i].fx == NB_FXK_F1) CHECK(f2 == 1, "%s: F2 unaffected (%d)", neg[i].name, f2);
    }
    g_v = (variant){ 0 };
    {   // fx mask: F1 only
        nb_persist_reset(&PS); const nb_rules r = { NB_RULES_EXACT, 0, 0 }; const nb_post_rules pr = { 1u << NB_FXK_F1, 8, sz, NULL };
        build(&F, 1); nb_plan(&F, &PS, &r, &P); nb_post_plan(&F, &P, &pr, NULL, &O);
        CHECK(O.fused[1] == 1 && O.fused[2] == 0 && find_flag(NB_NP_FUSE) >= 0, "fx mask F1 only");
    }
}

// ---- NB-18b-8: route A ----
static nb_post_aa_state AS; static nb_post_aa_out AO;
static int node_of(uint64_t key, uint64_t rt) { for (int i = 0; i < F.nnode; i++) if (F.node[i].kind == NB_NODE_PASS && F.node[i].fs == key && (!rt || F.node[i].att[0].image == rt)) return i; return -1; }
static void aa_frame(uint32_t no) {
    const nb_rules r = { NB_RULES_EXACT, 0, 0 }; const nb_post_rules pr = { 0, 8, sz, NULL };
    build(&F, no);
    CHECK(nb_plan(&F, &PS, &r, &P) == 0, "nb_plan");
    CHECK(nb_post_plan(&F, &P, &pr, &H, &O) >= 0, "nb_post_plan");
    CHECK(nb_post_aa_plan(&F, &P, &O, &pr, &AS, &AO) >= 0, "nb_post_aa_plan");
}
static void aa_reset(void) { nb_persist_reset(&PS); memset(&H, 0, sizeof H); memset(&AS, 0, sizeof AS); }
static int aa_why(void) { for (int w = NB_PAW_OTHER_FXAA; w < NB_PAW_KINDS; w++) if (AO.why[w]) return w; return -1; }
static unsigned fl(int n) { return n < 0 ? 0u : (unsigned)P.node[n].flags & (NB_NP_ELIDE | NB_NP_FUSE | NB_NP_AA); }
static void route_a(void) {
    memset(&g_v, 0, sizeof g_v); aa_reset();
    for (uint32_t i = 1; i <= 12; i++) aa_frame(i);
    {   // the campaign frame with Vignette: LUT elide aa, Sight fuse aa, FXAA elide aa, Vignette (F2's elided pass) reads the alias
        const int l = node_of(K_LUT, 0), s = node_of(K_SIGHT, 0), x = node_of(K_FXAA, 0), v = node_of(K_VIG, 0), c = node_of(K_COMB, 0);
        CHECK(AO.f1a == 1 && AO.f1a_fxaa == 1 && AO.plain == 0 && aa_why() < 0, "A base: F1A with the FXAA pass elided (f1a %d fxaa %d plain %d why %s)",
              AO.f1a, AO.f1a_fxaa, AO.plain, nb_post_aa_why_name(aa_why()));
        CHECK(fl(l) == (NB_NP_ELIDE | NB_NP_AA) && fl(s) == (NB_NP_FUSE | NB_NP_AA) && fl(x) == (NB_NP_ELIDE | NB_NP_AA), "A base: flags LUT %#x Sight %#x FXAA %#x", fl(l), fl(s), fl(x));
        CHECK(fl(v) == (NB_NP_ELIDE | NB_NP_AA) && fl(c) == NB_NP_FUSE && AO.readers == 1, "A base: Vignette = F2's elided pass + alias reader, combine = F2's host (%#x %#x readers %d)", fl(v), fl(c), AO.readers);
        CHECK(AO.step[x] == s && AS.n == 1 && AS.t[0].image == O1 && !AS.t[0].bad, "A base: the step belongs to Sight, its target is tracked");
        static char buf[1 << 16]; nb_dump_str(&F, &P, buf, sizeof buf);
        CHECK(strstr(buf, "elide aa") && strstr(buf, "fuse aa"), "A base: the dump marks the aa nodes");
    }
    g_v = (variant){ .no_3d = 1 }; aa_frame(13);   // loading frame after fused frames: the combine is told to sample Sight's target
    CHECK(AO.f1a == 0 && AO.readers == 1 && fl(node_of(K_COMB, 0)) == NB_NP_AA, "A loading frame: the lone combine reads the alias (readers %d flags %#x)", AO.readers, fl(node_of(K_COMB, 0)));
    g_v = (variant){ .no_3d = 2 }; aa_frame(14);
    CHECK(AO.readers == 0 && !(fl(node_of(K_COMB, 0)) & NB_NP_AA), "A loading frame after a clear of the target: the combine reads the cleared image itself (readers %d)", AO.readers);
    g_v = (variant){ .no_vig = 1 }; aa_reset();
    for (uint32_t i = 1; i <= 3; i++) aa_frame(i);
    CHECK(AO.f1a_fxaa == 1 && fl(node_of(K_COMB, 0)) == NB_NP_AA && AO.readers == 1, "A menu (no Vignette): FXAA elided, the combine reads the alias (%d, %#x)", AO.f1a_fxaa, fl(node_of(K_COMB, 0)));
    g_v = (variant){ .no_vig = 1, .copy_step = 1 }; aa_reset(); aa_frame(1);
    CHECK(AO.f1a == 1 && AO.f1a_copy == 1 && fl(node_of(K_COPY, 0)) == (NB_NP_ELIDE | NB_NP_AA) && fl(node_of(K_COMB, 0)) == NB_NP_AA, "A AntiAliasing 0, no Vignette: F1A + the PPDefault copy elided");
    g_v = (variant){ .no_step = 1 }; aa_reset();
    for (uint32_t i = 1; i <= 10; i++) aa_frame(i);   // the LUT target is not rewritten in the frame: F1 itself waits for 8 frames of history
    CHECK(AO.f1a == 1 && AO.f1a_nostep == 1 && AS.n == 0 && fl(node_of(K_SIGHT, 0)) == (NB_NP_FUSE | NB_NP_AA) && fl(node_of(K_LUT, 0)) == (NB_NP_ELIDE | NB_NP_AA) &&
          !(fl(node_of(K_VIG, 0)) & NB_NP_AA), "A AntiAliasing 0 + Vignette: F1A without a step, nothing aliased");
    struct { const char *name; variant v; int why; } neg[] = {
        { "a step between Sight and FXAA", { .outline = 1 }, NB_PAW_OTHER_FXAA },
        { "FXAA reads not exact", { .fxaa_inexact = 1 }, NB_PAW_INEXACT },
        { "a copy from the FXAA target", { .op_reads = 1 }, NB_PAW_READER },
        { "Sight's target drawn again", { .s_written = 1 }, NB_PAW_S_WRITTEN },
        { "an FXAA pass over the combine's output (fed by Sight's target)", { .fxaa_twice = 1 }, NB_PAW_OTHER_FXAA },
        { "a step between Sight and FXAA, with the portrait chain after the combine", { .outline = 1, .portrait = 1 }, NB_PAW_OTHER_FXAA },
    };
    for (unsigned i = 0; i < sizeof neg / sizeof neg[0]; i++) {
        g_v = neg[i].v; aa_reset(); aa_frame(1);
        int any = 0; for (int n = 0; n < F.nnode; n++) any |= P.node[n].flags & NB_NP_AA;
        CHECK(AO.f1a == 0 && AO.plain == 1 && aa_why() == neg[i].why && !any, "A %s: plain F1 + the console's FXAA, '%s' (got f1a %d plain %d '%s' aa flags %d)", neg[i].name,
              nb_post_aa_why_name(neg[i].why), AO.f1a, AO.plain, nb_post_aa_why_name(aa_why()), any);
        CHECK(O.fused[NB_FXK_F1] == 1 && fl(node_of(K_LUT, O1)) == NB_NP_ELIDE && fl(node_of(K_SIGHT, 0)) == NB_NP_FUSE, "A %s: F1 itself stays", neg[i].name);
        CHECK(AO.d_host == node_of(K_SIGHT, 0) && AO.d_why == neg[i].why, "A %s: the refusal names its Sight pass and reason (%d, %d)", neg[i].name, AO.d_host, AO.d_why);
        if (neg[i].why == NB_PAW_OTHER_FXAA) {
            const int want = neg[i].v.fxaa_twice ? node_of(K_FXAA, X) : node_of(K_FXAA, O1);
            CHECK(AO.d_other == want && AO.d_first >= 0, "A %s: the refusal names the FXAA pass (node %d, want %d; first reader %d)", neg[i].name, AO.d_other, want, AO.d_first);
        }
    }
    // NB-18b-9: the HUD portrait's own LUT -> FXAA chain (80x100) is not fed by Sight's target: wherever it is recorded, the 3D tail is
    // F1A with its FXAA pass elided, the portrait's passes carry no flag (before: "another FXAA pass follows" in every gameplay frame).
    for (int where = 1; where <= 3; where++) for (int vig = 0; vig < 2; vig++) {
        g_v = (variant){ .portrait = where, .no_vig = !vig }; aa_reset();
        for (uint32_t i = 1; i <= 12; i++) aa_frame(i);
        const int x = node_of(K_FXAA, O1), px = node_of(K_FXAA, PO), pl = node_of(K_LUT, PL), l = node_of(K_LUT, O1), sg = node_of(K_SIGHT, 0);
        CHECK(AO.f1a == 1 && AO.f1a_fxaa == 1 && AO.plain == 0 && aa_why() < 0, "A portrait %d vig %d: F1A with the 3D FXAA pass elided (f1a %d fxaa %d plain %d '%s')",
              where, vig, AO.f1a, AO.f1a_fxaa, AO.plain, nb_post_aa_why_name(aa_why()));
        CHECK(fl(l) == (NB_NP_ELIDE | NB_NP_AA) && fl(sg) == (NB_NP_FUSE | NB_NP_AA) && fl(x) == (NB_NP_ELIDE | NB_NP_AA), "A portrait %d vig %d: flags LUT %#x Sight %#x FXAA %#x", where, vig, fl(l), fl(sg), fl(x));
        CHECK(px >= 0 && pl >= 0 && fl(px) == 0 && fl(pl) == 0, "A portrait %d vig %d: the portrait's LUT / FXAA passes run as recorded (%#x %#x)", where, vig, fl(pl), fl(px));
        CHECK(AO.unrelated_fxaa == (where == 3 ? 0 : 1), "A portrait %d vig %d: FXAA passes of other chains after Sight %d", where, vig, AO.unrelated_fxaa);
        CHECK(AS.n == 1 && AS.t[0].image == O1 && !AS.t[0].bad && AO.readers == 1, "A portrait %d vig %d: only the 3D FXAA target is aliased, one reader (%d, readers %d)", where, vig, AS.n, AO.readers);
        CHECK(AO.d_host < 0, "A portrait %d vig %d: no refusal recorded", where, vig);
    }
    {   // NR-33A: an operation reads the tracked target after its real write in that submission (the save-game thumbnail, phone 18.11):
        // the AA pass runs in that submission only, the fusion goes on in the next one (before: bad for good)
        g_v = (variant){ 0 }; aa_reset(); aa_frame(1); aa_frame(2);
        CHECK(AO.f1a_fxaa == 1, "A op after the write: fused first");
        g_v = (variant){ .op_reads = 1 }; aa_frame(3);
        int any = 0; for (int n = 0; n < F.nnode; n++) any |= P.node[n].flags & NB_NP_AA;
        CHECK(AO.op_reads == 0 && AO.op_fresh == 1 && !AS.t[0].bad && AS.t[0].now && AO.plain == 1 && AO.f1a == 0 && aa_why() == NB_PAW_OPREAD && !any,
              "A op after the write: the AA pass runs in this submission, the target is not bad (op_reads %d fresh %d bad %d '%s')", AO.op_reads, AO.op_fresh, AS.t[0].bad, nb_post_aa_why_name(aa_why()));
        g_v = (variant){ 0 }; aa_frame(4);
        CHECK(AO.f1a_fxaa == 1 && AO.plain == 0 && !AS.t[0].now, "A op after the write: fused again in the next submission (fxaa %d plain %d)", AO.f1a_fxaa, AO.plain);
    }
    {   // an operation reads the aliased target before anything writes it in its submission (a stale read): bad for good
        g_v = (variant){ 0 }; aa_reset(); aa_frame(1); aa_frame(2);
        CHECK(AO.f1a_fxaa == 1, "A sticky: fused first");
        g_v = (variant){ .op_first = 1 }; aa_frame(3);
        CHECK(AO.op_reads == 1 && AS.t[0].bad && AO.plain == 1 && AO.f1a == 0, "A sticky: the copy marks the target bad (op_reads %d bad %d)", AO.op_reads, AS.t[0].bad);
        g_v = (variant){ 0 }; aa_frame(4);
        int any = 0; for (int n = 0; n < F.nnode; n++) any |= P.node[n].flags & NB_NP_AA;
        CHECK(AO.plain == 1 && aa_why() == NB_PAW_BAD && !any, "A sticky: never aliased again ('%s')", nb_post_aa_why_name(aa_why()));
    }
    // NB-18b-10: the ship deck. EoCFog between Sight and FXAA carries F1A's output into its own target: the FXAA pass is elided, the
    // combine samples the fog's target; the fog pass runs as recorded with NB_NP_AA (the backend follows F1A's output through it).
    g_v = (variant){ .fog = 1 }; aa_reset();
    for (uint32_t i = 1; i <= 12; i++) {
        aa_frame(i);
        const int l = node_of(K_LUT, 0), sg = node_of(K_SIGHT, 0), fg = node_of(K_FOG, 0), x = node_of(K_FXAA, 0), c = node_of(K_COMB, 0), lin = node_of(K_OTHER, DRT), lit = node_of(K_OTHER, HDR);
        CHECK(AO.f1a == 1 && AO.f1a_fxaa == 1 && AO.carried == 1 && AO.plain == 0 && aa_why() < 0, "A deck frame %u: F1A, FXAA elided, the fog carries (f1a %d fxaa %d carried %d plain %d '%s')",
              i, AO.f1a, AO.f1a_fxaa, AO.carried, AO.plain, nb_post_aa_why_name(aa_why()));
        CHECK(fl(l) == (NB_NP_ELIDE | NB_NP_AA) && fl(sg) == (NB_NP_FUSE | NB_NP_AA) && fl(fg) == NB_NP_AA && fl(x) == (NB_NP_ELIDE | NB_NP_AA) && fl(c) == NB_NP_AA,
              "A deck frame %u: flags LUT %#x Sight %#x Fog %#x FXAA %#x combine %#x", i, fl(l), fl(sg), fl(fg), fl(x), fl(c));
        CHECK(fl(lin) == 0 && fl(lit) == 0 && AO.readers == 1, "A deck frame %u: LinearizeDepth rewrites the pooled target, lighting reads it as it is (%#x %#x readers %d)", i, fl(lin), fl(lit), AO.readers);
        CHECK(AO.step[x] == sg && AS.n == 1 && AS.t[0].image == DRT && !AS.t[0].bad, "A deck frame %u: the aliased target is the FXAA pass's (the pooled depth target)", i);
    }
    // NR-33A: the Fort Joy tail Sight -> outline -> fog (into Sight's own target) -> FXAA. Without DM_NB_POST_AA_OUTLINE the outline is
    // no carry (plain F1 + the console's FXAA, as on the phone 18.11); with it both carry and the FXAA pass is elided.
    for (int ol = 0; ol < 2; ol++) {
        g_v = (variant){ .fog = 1, .fj_outline = 1 }; aa_reset(); AS.outline = (uint8_t)ol;
        for (uint32_t i = 1; i <= 12; i++) aa_frame(i);
        const int sg = node_of(K_SIGHT, 0), ou = node_of(K_OUTL, 0), fg = node_of(K_FOG, 0), x = node_of(K_FXAA, 0), c = node_of(K_COMB, 0);
        if (!ol) CHECK(AO.f1a == 0 && AO.plain == 1 && aa_why() == NB_PAW_OTHER_FXAA && AO.d_first == ou && AO.d_other == x, "A Fort Joy, outline carry off: plain F1 ('%s' first %d other %d)",
                       nb_post_aa_why_name(aa_why()), AO.d_first, AO.d_other);
        else {
            CHECK(AO.f1a == 1 && AO.f1a_fxaa == 1 && AO.carried == 2 && AO.plain == 0 && aa_why() < 0, "A Fort Joy, outline carry on: F1A, both carry, FXAA elided (f1a %d fxaa %d carried %d '%s')",
                  AO.f1a, AO.f1a_fxaa, AO.carried, nb_post_aa_why_name(aa_why()));
            CHECK(fl(sg) == (NB_NP_FUSE | NB_NP_AA) && fl(ou) == NB_NP_AA && fl(fg) == NB_NP_AA && fl(x) == (NB_NP_ELIDE | NB_NP_AA) && fl(c) == NB_NP_AA && AS.n == 1 && AS.t[0].image == DRT,
                  "A Fort Joy, outline carry on: flags Sight %#x outline %#x fog %#x FXAA %#x combine %#x", fl(sg), fl(ou), fl(fg), fl(x), fl(c));
        }
    }
    AS.outline = 0;
    g_v = (variant){ .fog = 1, .fog_no_aa = 1 }; aa_reset();
    for (uint32_t i = 1; i <= 10; i++) aa_frame(i);
    CHECK(AO.f1a == 1 && AO.f1a_nostep == 1 && AO.carried == 0 && AS.n == 0 && fl(node_of(K_FOG, 0)) == 0 && fl(node_of(K_COMB, 0)) == 0,
          "A deck, AntiAliasing 0: F1A without a step, the fog and the combine run as recorded (f1a %d nostep %d)", AO.f1a, AO.f1a_nostep);
    struct { const char *name; variant v; int why, frames; } fneg[] = {
        { "fog reads not exact (program not published yet)", { .fog = 1, .fog_inexact = 1 }, NB_PAW_CARRY, 1 },
        { "fog pass with 2 draws", { .fog = 1, .fog_draws2 = 1 }, NB_PAW_CARRY, 1 },
        { "fog samples its own target", { .fog = 1, .fog_feedback = 1 }, -1, 1 },   // F1 itself is refused: another reader of the LUT's target
        { "fog target of another size", { .fog = 1, .fog_small = 1 }, -1, 1 },   // F1 itself is refused by its size rule
        { "fog output drawn over before FXAA", { .fog = 1, .fog_rewrite = 1 }, NB_PAW_CARRY, 1 },
        { "fog samples the aliased depth target that nothing rewrote", { .fog = 1, .fog_stale_depth = 1 }, NB_PAW_CARRY, 3 },
    };
    for (unsigned i = 0; i < sizeof fneg / sizeof fneg[0]; i++) {
        g_v = fneg[i].v; aa_reset();
        for (int k = 1; k <= fneg[i].frames; k++) aa_frame((uint32_t)k);
        const int any = (fl(node_of(K_LUT, 0)) | fl(node_of(K_SIGHT, 0)) | (fneg[i].v.fog_stale_depth ? 0u : fl(node_of(K_FOG, 0))) | fl(node_of(K_FXAA, 0))) & NB_NP_AA;   // the tail (readers of a stale depth target, the fog among them, are told to sample its alias)
        if (fneg[i].why < 0) { CHECK(AO.f1a == 0 && !any, "A deck, %s: no F1A (f1a %d plain %d)", fneg[i].name, AO.f1a, AO.plain); continue; }
        CHECK(AO.f1a == 0 && AO.plain == 1 && aa_why() == fneg[i].why && !any, "A deck, %s: plain F1 + the console's FXAA, '%s' (got f1a %d plain %d '%s' aa flags %d)", fneg[i].name,
              nb_post_aa_why_name(fneg[i].why), AO.f1a, AO.plain, nb_post_aa_why_name(aa_why()), any);
        if (!fneg[i].v.fog_rewrite) CHECK(AO.d_first == node_of(K_FOG, 0) && AO.d_other == node_of(K_FXAA, 0), "A deck, %s: the refusal names the fog and the FXAA pass (%d %d)", fneg[i].name, AO.d_first, AO.d_other);
    }
    g_v = (variant){ 0 };
}

// ---- the P2 dump of campaign frame 2509 (nb_ir_04_f2509_p0.txt): nodes 132 LUT, 133 Sight, 134 FXAA, 135 Vignette, 136 Combine (131: an empty pass) ----
static struct { uint64_t im; uint32_t w, h; } g_img[256]; static int g_nimg;
static int dsz(void *ctx, uint64_t im, uint32_t *w, uint32_t *h) { (void)ctx; for (int i = 0; i < g_nimg; i++) if (g_img[i].im == im) { *w = g_img[i].w; *h = g_img[i].h; return 1; } return 0; }
static void p2(const char *dir) {
    char path[600]; snprintf(path, sizeof path, "%s/nb_ir_04_f2509_p0.txt", dir);
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("skip: no %s\n", path); return; }
    static char text[1 << 20]; const size_t n = fread(text, 1, sizeof text - 1, fp); fclose(fp); text[n] = 0;
    for (char *l = text; (l = strstr(l, "# image ")) != NULL; l++) {
        unsigned long long im; unsigned w, h;
        if (sscanf(l, "# image 0x%llx %ux%u", &im, &w, &h) == 3 && g_nimg < 256) { g_img[g_nimg].im = im; g_img[g_nimg].w = w; g_img[g_nimg].h = h; g_nimg++; }
    }
    CHECK(nb_parse(text, &F) == 0, "P2 dump parses");
    static const struct { int node; uint64_t fs, rt; uint64_t sr[3]; int nsr; } tail[] = {
        { 132, 0x921728a5bbb1c71bull, 0x10cba47000ull, { 0x10ccf5f000ull, 0x10d6f7dc00ull }, 2 },   // LUT: HDR #271 + the 3D LUT (id from the reads)
        { 133, 0x91eb2d97a304709aull, 0x10cafbb000ull, { 0x10cba47000ull, 0x10c4fef000ull, 0x10d7138480ull }, 3 },   // Sight: #130, LinearDepth, a cone
        { 134, 0x691768aa1234abcdull, 0x10cba47000ull, { 0x10cafbb000ull }, 1 },                 // FXAA
        { 135, 0x43e8e8ed2b1df6c2ull, 0x1181000000ull, { 0x10cba47000ull }, 1 },                 // Vignette
        { 136, 0x98186c22a7209861ull, 0x1181870000ull, { 0x1181000000ull, 0x1182d88000ull }, 2 },   // CombineUI_NoFetch: Base #94, Tex #57
    };
    for (unsigned i = 0; i < sizeof tail / sizeof tail[0]; i++) {
        nb_node *nd = &F.node[tail[i].node];
        CHECK(nd->kind == NB_NODE_PASS && nd->natt == 1 && nd->att[0].image == tail[i].rt, "P2 node %d is the expected tail pass", tail[i].node);
        nd->fs = tail[i].fs; nd->fsx = NB_FSX_ANY | NB_FSX_EXACT; nd->nsr = (uint8_t)tail[i].nsr; memcpy(nd->sread, tail[i].sr, sizeof tail[i].sr);
    }
    nb_persist_reset(&PS); memset(&H, 0, sizeof H);
    const nb_rules r = { NB_RULES_EXACT, 0, 0 }; const nb_post_rules pr = { 0, 8, dsz, NULL };
    for (int k = 0; k < 10; k++) {   // the same frame again and again: the history of #94 fills (its first access is the full clear)
        CHECK(nb_plan(&F, &PS, &r, &P) == 0, "P2 plan");
        nb_post_plan(&F, &P, &pr, &H, &O);
        if (k == 0) {
            CHECK(O.fused[1] == 1 && (P.node[132].flags & NB_NP_ELIDE) && (P.node[133].flags & NB_NP_FUSE), "P2 frame 1: F1 = nodes 132 -> 133 (why %s)", nb_post_why_name(why_of(1)));
            CHECK(O.fused[2] == 0 && why_of(2) == NB_PW_PERSIST, "P2 frame 1: F2 waits for history (why %s)", nb_post_why_name(why_of(2)));
        }
    }
    CHECK(O.fused[2] == 1 && (P.node[135].flags & NB_NP_ELIDE) && (P.node[136].flags & NB_NP_FUSE), "P2 frame 10: F2 = nodes 135 -> 136 (why %s)", nb_post_why_name(why_of(2)));
    memset(&AS, 0, sizeof AS);   // NB-18b-8: route A on the same frame
    CHECK(nb_post_aa_plan(&F, &P, &O, &pr, &AS, &AO) == 1 && AO.f1a_fxaa == 1, "P2 route A: F1A with the FXAA pass elided (why %s)", nb_post_aa_why_name(aa_why()));
    CHECK(fl(132) == (NB_NP_ELIDE | NB_NP_AA) && fl(133) == (NB_NP_FUSE | NB_NP_AA) && fl(134) == (NB_NP_ELIDE | NB_NP_AA) && fl(135) == (NB_NP_ELIDE | NB_NP_AA) && fl(136) == NB_NP_FUSE,
          "P2 route A: flags 132..136 = %#x %#x %#x %#x %#x", fl(132), fl(133), fl(134), fl(135), fl(136));
    { int extra = 0; for (int i = 0; i < F.nnode; i++) if (i < 132 && (P.node[i].flags & NB_NP_AA)) extra++;
      CHECK(extra == 0, "P2 route A: no pass before the tail is told to read the alias (%d)", extra); }
    printf("P2 f2509 route A: F1A %d (FXAA elided %d), alias readers %d\n", AO.f1a, AO.f1a_fxaa, AO.readers);
    printf("P2 f2509: F1 %d F2 %d, later cover_dc %d, assumed covering %d\n", O.fused[1], O.fused[2], O.later_cover, O.assumed_cover);
}

// ---- NB-18b-10: the phone's ship deck dumps (work/logs/ra1-dumps, 17.11; fs= / sread lines as recorded): every nb_ir_*_p0.txt in argv[2]
// order is planned in sequence with one state, like the front end does. In the deck frames (a pass fs e94ea226 = EoCFog between Sight and
// FXAA) the tail must be: LUT elide aa, Sight fuse aa, Fog aa (carry), FXAA elide aa, combine reads aa.
static int find_fs(uint32_t hi, int from) { for (int i = from; i < F.nnode; i++) if (F.node[i].kind == NB_NODE_PASS && (uint32_t)(F.node[i].fs >> 32) == hi) return i; return -1; }
static void deck_dumps(const char *dir) {
    static const char *const names[] = { "nb_ir_03_f2330_p0.txt", "nb_ir_05_f3484_p0.txt", "nb_ir_07_f4668_p0.txt", "nb_ir_08_f5269_p0.txt", "nb_ir_09_f5869_p0.txt",
                                         "nb_ir_10_f6470_p0.txt", "nb_ir_11_f7071_p0.txt" };
    nb_persist_reset(&PS); memset(&H, 0, sizeof H); memset(&AS, 0, sizeof AS); g_nimg = 0;
    const nb_rules r = { NB_RULES_EXACT, 0, 0 }; const nb_post_rules pr = { 0, 8, dsz, NULL };
    int deck = 0, ok = 0;
    for (unsigned d = 0; d < sizeof names / sizeof names[0]; d++) {
        char path[600]; snprintf(path, sizeof path, "%s/%s", dir, names[d]);
        FILE *fp = fopen(path, "rb");
        if (!fp) { printf("skip: no %s\n", path); return; }
        static char text[1 << 20]; const size_t n = fread(text, 1, sizeof text - 1, fp); fclose(fp); text[n] = 0;
        for (char *l = text; (l = strstr(l, "# image ")) != NULL; l++) {
            unsigned long long im; unsigned w, h;
            if (sscanf(l, "# image 0x%llx %ux%u", &im, &w, &h) != 3) continue;
            int at = -1; for (int i = 0; i < g_nimg; i++) if (g_img[i].im == im) at = i;
            if (at < 0 && g_nimg < 256) at = g_nimg++;
            if (at >= 0) { g_img[at].im = im; g_img[at].w = w; g_img[at].h = h; }
        }
        CHECK(nb_parse(text, &F) == 0, "%s parses", names[d]);
        const int fog = find_fs(0xe94ea226u, 0);
        for (int rep = 0; rep < (fog >= 0 ? 3 : 1); rep++) {   // the same deck frame three times: the state of the frame before is the alias
            CHECK(nb_plan(&F, &PS, &r, &P) == 0, "%s plan", names[d]);
            CHECK(nb_post_plan(&F, &P, &pr, &H, &O) >= 0, "%s post plan", names[d]);
            CHECK(nb_post_aa_plan(&F, &P, &O, &pr, &AS, &AO) >= 0, "%s aa plan", names[d]);
            if (fog < 0) continue;
            deck++;
            const int l = find_fs(0x921728a5u, 0), sg = find_fs(0x91eb2d97u, 0), x = find_fs(0x691768aau, fog), c = find_fs(0x98186c22u, fog);
            const int good = AO.f1a == 1 && AO.f1a_fxaa == 1 && AO.carried == 1 && AO.plain == 0 && fl(l) == (NB_NP_ELIDE | NB_NP_AA) && fl(sg) == (NB_NP_FUSE | NB_NP_AA) &&
                             fl(fog) == NB_NP_AA && fl(x) == (NB_NP_ELIDE | NB_NP_AA) && fl(c) == NB_NP_AA && AO.readers == 1;
            CHECK(good, "%s rep %d: F1A through the fog (f1a %d fxaa %d carried %d plain %d '%s'; flags LUT %#x Sight %#x Fog %#x FXAA %#x combine %#x; readers %d)", names[d], rep,
                  AO.f1a, AO.f1a_fxaa, AO.carried, AO.plain, nb_post_aa_why_name(aa_why()), fl(l), fl(sg), fl(fog), fl(x), fl(c), AO.readers);
            int extra = 0; for (int i = 0; i < F.nnode; i++) if (i != l && i != sg && i != fog && i != x && i != c && (P.node[i].flags & NB_NP_AA)) extra++;
            CHECK(extra == 0, "%s rep %d: no other pass is told to read the alias (%d): the linear depth in the pooled target is read as it is", names[d], rep, extra);
            ok += good && !extra;
        }
    }
    { int bad = 0; for (int t = 0; t < AS.n; t++) bad += AS.t[t].bad; CHECK(bad == 0, "deck dumps: no aliased target went bad (%d of %d)", bad, AS.n); }
    printf("ship deck dumps: %d deck plans, %d with F1A through EoCFog and the FXAA pass elided; aliased targets tracked %d\n", deck, ok, AS.n);
}

int main(int argc, char **argv) {
    synthetic();
    route_a();
    p2(argc > 1 ? argv[1] : "../../../../../work/logs/p2b-nb");
    deck_dumps(argc > 2 ? argv[2] : "../../../../../work/logs/ra1-dumps");
    if (fails) { printf("nb_post_test: %d failed\n", fails); return 1; }
    printf("nb_post_test: all passed\n");
    return 0;
}
