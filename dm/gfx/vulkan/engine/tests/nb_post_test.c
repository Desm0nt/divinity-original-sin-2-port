// [DOS2] NB-18b-4a host test: the post-tail fusion plan (nb_post_plan, dm/gfx/nb/nb_plan.c) on synthetic chains shaped like P2
// (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-18a findings" table: campaign f=2509) and on the P2 dump of that frame with the program facts
// (fs key + exact sampled reads) put in by hand for its five tail passes (the dumps predate the fs= lines).
// Build (Windows clang):
//   clang -std=c11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS nb_post_test.c ../../../nb/nb_plan.c -o nb_post_test
//   nb_post_test [<dir with nb_ir_04_f2509_p0.txt>]   (default: ../../../../../work/logs/p2b-nb, skipped when missing)
// NB-18b-8 (route A): nb_post_aa_plan (dm/gfx/nb/nb_post_aa.h) on the same chains - F1A + the elided FXAA pass, its readers, refusals.
#include "../../../nb/nb_ir.h"
#include "../../../nb/nb_post_aa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { HDR = 0x2710, LUT3D = 0x2830, D = 0x2660, O1 = 0x1300, SIGHT = 0x1290, O2 = 0x940, WIN = 0x930, UI = 0x570, X = 0x7770 };
static const uint64_t K_LUT = 0x921728a5bbb1c71bull, K_SIGHT = 0x91eb2d97a304709aull, K_FXAA = 0x691768aa00000000ull, K_VIG = 0x43e8e8ed2b1df6c2ull,
                      K_COMB = 0x98186c22a7209861ull, K_OTHER = 0x1234567800000000ull, K_COPY = 0x7b3210191abd90cdull;

// NB-18b-8 variants: copy_step (AntiAliasing 0, no Vignette: a PPDefault copy instead of FXAA), no_step (AntiAliasing 0 + Vignette: it
// reads Sight's target), outline (a step between Sight and FXAA: FXAA reads its target T), fxaa_inexact, op_reads (a copy from the
// FXAA target after the chain), s_written (a pass draws into Sight's target after FXAA), no_3d (a loading frame: only the combine).
typedef struct { int lut_inexact, lut_draws2, reader_after_sight, compute_between, vig_read_next, small_sight, sight_rt_size, no_vig;
                 int copy_step, no_step, outline, fxaa_inexact, op_reads, s_written, no_3d; } variant;
static variant g_v;
static int sz(void *ctx, uint64_t im, uint32_t *w, uint32_t *h) {
    (void)ctx;
    if (im == WIN || im == UI) { *w = 2400; *h = 1080; return 1; }
    if (im == LUT3D) return 0;
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
// One frame of the campaign tail: (full clear of O2 like P2's line 10) 3D -> LUT -> barrier -> Sight -> FXAA -> [Vignette] -> Combine -> present
static void build(nb_frame *f, uint32_t no) {
    nb_frame_reset(f, no);
    const uint32_t z[4] = { 0, 0, 0, 0 };
    if (g_v.no_3d) {   // a loading frame: the combine alone samples last frame's post output
        if (g_v.no_3d == 2) nb_clear(f, O1, NB_ASP_COLOR, NB_CLEAR_FULL, z, 0, 0);   // ... after the game cleared it: a real write, no alias
        const uint64_t s[2] = { UI, O1 }, w = WIN; pass1(f, WIN, K_COMB, 1, s, 2, NULL, 0); nb_op(f, NB_OP_PRESENT, &w, 1, NULL, 0); nb_submit(f, 1); return;
    }
    if (g_v.vig_read_next) { const uint64_t s[1] = { O2 }; pass1(f, X, K_OTHER, 1, s, 1, NULL, 0); }   // last frame's O2 sampled first
    nb_clear(f, O2, NB_ASP_COLOR, NB_CLEAR_FULL, z, 0, 0);
    { const uint64_t s[1] = { X }; pass1(f, HDR, K_OTHER, 1, s, 1, NULL, 0); }
    { const uint64_t s[2] = { HDR, LUT3D }, st[1] = { O2 }; pass1(f, O1, K_LUT, !g_v.lut_inexact, s, 2, st, 1); if (g_v.lut_draws2) nb_draw(f, 0); }
    if (g_v.compute_between) nb_op(f, NB_OP_COMPUTE, NULL, 0, NULL, 0);
    nb_barrier(f);
    { const uint64_t s[2] = { O1, D }, st[1] = { O2 }; pass1(f, SIGHT, K_SIGHT, 1, s, 2, st, 1); }
    if (g_v.reader_after_sight) { const uint64_t s[1] = { O1 }; pass1(f, X, K_OTHER, 1, s, 1, NULL, 0); }
    if (g_v.outline) { const uint64_t s[1] = { SIGHT }; pass1(f, X, K_OTHER, 1, s, 1, NULL, 0); }
    if (!g_v.no_step) { const uint64_t s[1] = { g_v.outline ? (uint64_t)X : (uint64_t)SIGHT }, st[1] = { O2 };
        pass1(f, O1, g_v.copy_step ? K_COPY : K_FXAA, !g_v.fxaa_inexact, s, 1, st, 1); }
    if (g_v.s_written) { const uint64_t s[1] = { X }; pass1(f, SIGHT, K_OTHER, 1, s, 1, NULL, 0); }
    const uint64_t post = g_v.no_step ? (uint64_t)SIGHT : (uint64_t)O1;   // what the step after the AA step reads
    if (!g_v.no_vig) { const uint64_t s[1] = { post }, st[1] = { O2 }; pass1(f, O2, K_VIG, 1, s, 1, st, 1); }   // stale self binding -> feedback
    { const uint64_t s[2] = { UI, g_v.no_vig ? post : (uint64_t)O2 }; pass1(f, WIN, K_COMB, 1, s, 2, NULL, 0); }
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
    };
    for (unsigned i = 0; i < sizeof neg / sizeof neg[0]; i++) {
        g_v = neg[i].v; aa_reset(); aa_frame(1);
        int any = 0; for (int n = 0; n < F.nnode; n++) any |= P.node[n].flags & NB_NP_AA;
        CHECK(AO.f1a == 0 && AO.plain == 1 && aa_why() == neg[i].why && !any, "A %s: plain F1 + the console's FXAA, '%s' (got f1a %d plain %d '%s' aa flags %d)", neg[i].name,
              nb_post_aa_why_name(neg[i].why), AO.f1a, AO.plain, nb_post_aa_why_name(aa_why()), any);
        CHECK(O.fused[NB_FXK_F1] == 1 && fl(node_of(K_LUT, 0)) == NB_NP_ELIDE && fl(node_of(K_SIGHT, 0)) == NB_NP_FUSE, "A %s: F1 itself stays", neg[i].name);
    }
    {   // an operation reads the aliased target in a later frame: bad for good
        g_v = (variant){ 0 }; aa_reset(); aa_frame(1); aa_frame(2);
        CHECK(AO.f1a_fxaa == 1, "A sticky: fused first");
        g_v = (variant){ .op_reads = 1 }; aa_frame(3);
        CHECK(AO.op_reads == 1 && AS.t[0].bad && AO.plain == 1 && AO.f1a == 0, "A sticky: the copy marks the target bad (op_reads %d bad %d)", AO.op_reads, AS.t[0].bad);
        g_v = (variant){ 0 }; aa_frame(4);
        int any = 0; for (int n = 0; n < F.nnode; n++) any |= P.node[n].flags & NB_NP_AA;
        CHECK(AO.plain == 1 && aa_why() == NB_PAW_BAD && !any, "A sticky: never aliased again ('%s')", nb_post_aa_why_name(aa_why()));
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

int main(int argc, char **argv) {
    synthetic();
    route_a();
    p2(argc > 1 ? argv[1] : "../../../../../work/logs/p2b-nb");
    if (fails) { printf("nb_post_test: %d failed\n", fails); return 1; }
    printf("nb_post_test: all passed\n");
    return 0;
}
