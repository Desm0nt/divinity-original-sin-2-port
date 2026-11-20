// [DOS2] NR-ONCHIP host test (dm/gfx/nb/nb_onchip.h): the front end's chain marks (nb_onchip_plan) on synthetic Sky -> Light / PCF -> Sky ->
// Light frames and their refusals, on every phone IR dump (ra1 / rb1 / rc1: the analysis of NR-ONCHIP.md §1 counted 11 shadow frames with
// S3 and 23 with S2), the backend's subpass stepping (nb_oc_step / nb_oc_apply / nb_oc_steps_to_end) and the subpass-input scan of the
// light.s2 / light.s3 modules of a native-nr.xgspack built by tools/dos2_nr_sh_pack.py.
// Build (Windows clang):
//   clang -std=c11 -c ../../../nb/nb_plan.c -o nb_plan.o && clang++ -std=c++17 -Wall nb_onchip_test.cpp nb_plan.o -o nb_onchip_test
//   nb_onchip_test [<logs dir with ra1-dumps rb1-dumps rc1-dumps> [<native-nr.xgspack>]]   (each part skipped when missing)
#include "../../../nb/nb_onchip.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

static int fails, checks;
#define CHECK(c, ...) do { ++checks; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const uint64_t K_LIN = 0xf23c09d198b55dc5ull, K_SKY = 0x640a4c497bcc748bull, K_LIGHT = 0xd236b8d5b7ffad3cull, K_PCF = 0x1372db843749e1d3ull,
                      K_FOG = 0xd8aeec640d465275ull, K_GB = 0x1111222233334444ull, K_SHADOW = 0x5555666677778888ull;
enum : uint64_t { G0 = 0x1000, G1 = 0x2000, DS = 0x3000, LIN = 0x4000, ATLAS = 0x5000, SH = 0x6000, SP = 0x7000, DF = 0x8000, X = 0x9000 };

struct V { int s3, sky_draws2, light_no_sp, op_between, light_depth, pcf_draws2, hz_light, light_reads_df, small_sp, light_clear, pcf_reads_s;
           int s2f, fog_reads_c0, sky_reads_c0, op_before_fog, fog_draws2, fog_small; };   // S2F: Fog writes G0 (the G-buffer c0 Light samples)
static V g;
static void pass(nb_frame *f, const nb_att *a, int n, uint64_t fs, const uint64_t *sr, int ns, int draws = 1) {
    nb_pass_begin(f, a, n);
    for (int d = 0; d < draws; d++) { nb_draw(f, 0); for (int i = 0; i < ns; i++) nb_read(f, -1, sr[i]); nb_draw_prog(f, fs, 1, sr, ns); }
}
static void frame(nb_frame *f) {
    static const uint32_t z[4] = {0, 0, 0, 0};
    nb_frame_reset(f, 7);
    { const nb_att a[3] = {{G0, 0}, {G1, 1}, {DS, NB_SLOT_DS}}; pass(f, a, 3, K_GB, nullptr, 0); }
    { const nb_att a[1] = {{LIN, 0}}; const uint64_t s[1] = {DS}; pass(f, a, 1, K_LIN, s, 1); }
    if (g.s3) {
        { const nb_att a[1] = {{ATLAS, NB_SLOT_DS}}; pass(f, a, 1, K_SHADOW, nullptr, 0); }
        { const nb_att a[1] = {{SH, 0}}; const uint64_t s[3] = {LIN, ATLAS, SH}; pass(f, a, 1, K_PCF, s, g.pcf_reads_s ? 3 : 2, g.pcf_draws2 ? 2 : 1); }
    }
    nb_clear(f, SP, NB_ASP_COLOR, NB_CLEAR_FULL, z, 0, 0);   // ClearTexture: folded into Sky
    nb_clear(f, DF, NB_ASP_COLOR, NB_CLEAR_FULL, z, 0, 0);
    { const nb_att a[2] = {{SP, 0}, {DF, 1}}; const uint64_t s[3] = {LIN, G1, G0}; pass(f, a, 2, K_SKY, s, g.sky_reads_c0 ? 3 : 2, g.sky_draws2 ? 2 : 1); }
    if (g.op_between) { const uint64_t src = X, dst = G0; nb_op(f, NB_OP_COPY, &src, 1, &dst, 1); }
    {
        nb_att a[2] = {{DF, 0}, {DS, NB_SLOT_DS}};
        std::vector<uint64_t> s = {G0, G1, LIN};
        if (!g.light_no_sp) s.push_back(SP);
        if (g.s3) s.push_back(SH);
        if (g.light_reads_df) s.push_back(DF);
        nb_pass_begin(f, a, g.light_depth ? 2 : 1);
        if (g.light_clear) nb_clear(f, DF, NB_ASP_COLOR, NB_CLEAR_FULL | NB_CLEAR_RT, z, 0, 0);
        nb_draw(f, 0); for (uint64_t x : s) nb_read(f, -1, x); nb_draw_prog(f, K_LIGHT, 1, s.data(), (int)s.size());
        if (g.hz_light) nb_hazard(f, NB_HZ_UBO);
    }
    if (g.s2f) {   // the engine: ClearTexture of c0 (folded into Fog), Fog writes c0 from Df and the linear depth
        if (g.op_before_fog) { const uint64_t src = X, dst = SH; nb_op(f, NB_OP_COPY, &src, 1, &dst, 1); }
        nb_clear(f, G0, NB_ASP_COLOR, NB_CLEAR_FULL, z, 0, 0);
        std::vector<uint64_t> s = {LIN, DF}; if (g.fog_reads_c0) s.push_back(G0);
        const nb_att a[1] = {{G0, 0}}; pass(f, a, 1, K_FOG, s.data(), (int)s.size(), g.fog_draws2 ? 2 : 1);
    } else { const nb_att a[1] = {{DF, 0}}; const uint64_t s[1] = {LIN}; pass(f, a, 1, K_FOG, s, 1); }
    { const uint64_t w = DF; nb_op(f, NB_OP_PRESENT, &w, 1, nullptr, 0); }
    nb_submit(f, 1);
}
static int size_of(void *, uint64_t im, uint32_t *w, uint32_t *h) { *w = (g.small_sp && im == SP) ? 1080 : 2160; *h = (g.small_sp && im == SP) ? 486 : 972; return 1; }
static nb_frame F; static nb_plan_out P; static nb_oc_out O;
// Per-pass sizes (rules.att_size, the tap's table of the sizes each pass was bound with): by the pass's class and attachment index.
struct PS { uint32_t sp[2], df[2], ldf[2], sh[2]; int unknown_sky_sp; };
static PS ps;
static int pass_size(void *, int node, int a, uint32_t *w, uint32_t *h) {
    if (node < 0 || node >= F.nnode) return 0;
    const int c = nb_oc_class(F.node[node].fs); const uint32_t *v = nullptr;
    if (c == NB_OCR_SKY) { if (a == 0 && ps.unknown_sky_sp) return 0; v = a == 0 ? ps.sp : ps.df; }
    else if (c == NB_OCR_LIGHT) v = ps.ldf;
    else if (c == NB_OCR_PCF) v = ps.sh;
    else if (c == NB_OCR_FOG && g.fog_small) { *w = 1080; *h = 486; return 1; }
    else { *w = 2160; *h = 972; return 1; }
    *w = v[0]; *h = v[1]; return 1;
}
static int run(const V &v, unsigned allow = 0, bool by_pass = false) {
    g = v; frame(&F);
    const nb_rules rules = {NB_RULES_EXACT, 0, 0};
    if (nb_plan(&F, nullptr, &rules, &P)) { printf("FAIL nb_plan\n"); fails++; return -1; }
    const nb_oc_rules r = {size_of, nullptr, allow, by_pass ? pass_size : nullptr};
    return nb_onchip_plan(&F, &P, &r, &O);
}
// The phone 18.11 case and the size rule by pass: the image table says Sp 1080x486 (the bloom alias of its storage, bound later in the
// part), the passes were bound at 2160x972.
static void sizes_by_pass() {
    const PS same = {{2160, 972}, {2160, 972}, {2160, 972}, {2160, 972}, 0};
    { V v{}; v.small_sp = 1; ps = same;
      CHECK(run(v) == 0 && O.why[NB_OCW_SIZE] == 1, "image-keyed rule with the alias's size refuses (the phone's 0 chains)");
      CHECK(run(v, 0, true) == 1 && O.s2 == 1 && O.why[NB_OCW_SIZE] == 0 && O.table_differ == 1 && O.sp_larger == 0 && O.ex_why == 0,
            "per-pass sizes mark it (table_differ %d)", O.table_differ); }
    { V v{}; v.small_sp = 1; v.s3 = 1; ps = same; CHECK(run(v, 0, true) == 1 && O.s3 == 1, "per-pass sizes: S3 too"); }
    { PS q = same; q.sp[0] = 1080; q.sp[1] = 486; ps = q;
      CHECK(run(V{}, 0, true) == 0 && O.why[NB_OCW_SIZE] == 1 && O.ex_why == NB_OCW_SIZE && O.ex[0] == 1080 && O.ex[1] == 486 && O.ex[2] == 2160 &&
            O.ex[5] == 972, "Sp smaller than Df at the Sky pass: refused, example kept (%u %u %u %u %u %u)", O.ex[0], O.ex[1], O.ex[2], O.ex[3], O.ex[4], O.ex[5]); }
    { PS q = same; q.sp[0] = 2400; q.sp[1] = 1080; ps = q;
      CHECK(run(V{}, 0, true) == 1 && O.s2 == 1 && O.sp_larger == 1, "Sp larger than Df: marked (framebuffer = Df), counted"); }
    { PS q = same; q.sp[0] = 2400; q.sp[1] = 900; ps = q; CHECK(run(V{}, 0, true) == 0 && O.why[NB_OCW_SIZE] == 1, "Sp smaller in one dimension: refused"); }
    { PS q = same; q.ldf[0] = 2400; q.ldf[1] = 1080; ps = q; CHECK(run(V{}, 0, true) == 0 && O.why[NB_OCW_SIZE] == 1, "Df at Light != Df at Sky: refused"); }
    { PS q = same; q.unknown_sky_sp = 1; ps = q;
      CHECK(run(V{}, 0, true) == 0 && O.why[NB_OCW_SIZE_UNK] == 1 && O.why[NB_OCW_SIZE] == 0 && O.ex_why == NB_OCW_SIZE_UNK, "unknown size: refused as unknown"); }
    { V v{}; v.s3 = 1; PS q = same; q.sh[0] = 1080; q.sh[1] = 486; ps = q;
      CHECK(run(v, 0, true) == 1 && O.s2 == 1 && O.s3 == 0 && O.why[NB_OCW_PCF] == 1, "S smaller than Df: S3 refused on PCF, S2 marked"); }
}
static int find(uint64_t fs) { for (int i = 0; i < F.nnode; i++) if (F.node[i].kind == NB_NODE_PASS && F.node[i].fs == fs) return i; return -1; }
static void synthetic() {
    // S2
    CHECK(run(V{}) == 1 && O.s2 == 1 && O.s3 == 0, "S2 marked (%d %d)", O.s2, O.s3);
    const int sky = find(K_SKY), light = find(K_LIGHT), fog = find(K_FOG);
    CHECK(sky >= 0 && P.node[sky].oc == nb_oc_mark(2, 0) && nb_oc_role(P.node[sky].oc) == NB_OCR_SKY, "Sky oc %#x", sky >= 0 ? P.node[sky].oc : 0);
    CHECK(light >= 0 && P.node[light].oc == nb_oc_mark(2, 1) && nb_oc_role(P.node[light].oc) == NB_OCR_LIGHT, "Light oc %#x", light >= 0 ? P.node[light].oc : 0);
    CHECK(P.node[fog].oc == 0 && P.node[find(K_LIN)].oc == 0, "other passes unmarked");
    CHECK(P.node[sky].load[0] == NB_LOAD_CLEAR && P.node[sky].load[1] == NB_LOAD_CLEAR, "Sky's clears folded (loads %d %d)", P.node[sky].load[0], P.node[sky].load[1]);
    // S3
    { V v{}; v.s3 = 1; CHECK(run(v) == 1 && O.s3 == 1, "S3 marked");
      const int p = find(K_PCF), s = find(K_SKY), l = find(K_LIGHT);
      CHECK(P.node[p].oc == nb_oc_mark(3, 0) && nb_oc_role(P.node[p].oc) == NB_OCR_PCF && P.node[s].oc == nb_oc_mark(3, 1) && P.node[l].oc == nb_oc_mark(3, 2),
            "S3 marks %#x %#x %#x", P.node[p].oc, P.node[s].oc, P.node[l].oc);
      CHECK(nb_oc_role(P.node[s].oc) == NB_OCR_SKY && nb_oc_role(P.node[l].oc) == NB_OCR_LIGHT, "S3 roles"); }
    { V v{}; v.s3 = 1; CHECK(run(v, 1) == 1 && O.s2 == 1 && O.s3 == 0 && P.node[find(K_PCF)].oc == 0, "S3 with S2 only allowed -> S2"); }
    { V v{}; v.s3 = 1; v.pcf_draws2 = 1; CHECK(run(v) == 1 && O.s2 == 1 && O.why[NB_OCW_PCF] == 1 && P.node[find(K_PCF)].oc == 0, "PCF of 2 draws: S2, PCF counted"); }
    { V v{}; v.s3 = 1; v.pcf_reads_s = 1; CHECK(run(v) == 1 && O.s2 == 1 && O.why[NB_OCW_PCF] == 1, "PCF samples its own target: S2"); }
    // refusals
    { V v{}; v.sky_draws2 = 1; CHECK(run(v) == 0 && O.why[NB_OCW_SKY_SHAPE] == 1, "Sky of 2 draws refused"); }
    { V v{}; v.light_no_sp = 1; CHECK(run(v) == 0 && O.why[NB_OCW_EDGE] == 1, "Light not reading Sp refused"); }
    { V v{}; v.op_between = 1; CHECK(run(v) == 0 && O.why[NB_OCW_NO_SKY] == 1, "a copy between Sky and Light refused"); }
    { V v{}; v.light_depth = 1; CHECK(run(v) == 0 && O.why[NB_OCW_LIGHT_SHAPE] == 1, "Light with depth refused"); }
    { V v{}; v.hz_light = 1; CHECK(run(v) == 0 && O.why[NB_OCW_LIGHT_SHAPE] == 1, "Light with a hazard refused"); }
    { V v{}; v.light_reads_df = 1; CHECK(run(v) == 0 && O.why[NB_OCW_LIGHT_SHAPE] + O.why[NB_OCW_EDGE] == 1, "Light sampling Df refused"); }
    { V v{}; v.small_sp = 1; CHECK(run(v) == 0 && O.why[NB_OCW_SIZE] == 1, "sizes differ refused"); }
    { V v{}; v.light_clear = 1; CHECK(run(v) == 0 && O.why[NB_OCW_LIGHT_SHAPE] == 1, "a clear in Light refused"); }
    // a second run clears the old marks
    { V v{}; v.sky_draws2 = 1; run(V{}); run(v); int any = 0; for (int i = 0; i < F.nnode; i++) any |= P.node[i].oc; CHECK(!any, "marks cleared"); }
    CHECK(nb_onchip_plan(nullptr, &P, nullptr, &O) == -1, "bad input");
}
// S2F: Sky -> Light -> Fog (Fog writes the c0 Light samples); only with allow bit 2
static void s2f() {
    V v{}; v.s2f = 1;
    CHECK(run(v) == 1 && O.s2 == 1 && O.s2f == 0, "S2F frame without allow bit 2: S2 as before (tail never by default)");
    CHECK(P.node[find(K_FOG)].oc == 0, "Fog unmarked without the bit");
    CHECK(run(v, 7) == 1 && O.s2f == 1 && O.s2 == 0 && O.why[NB_OCW_S2F] == 1, "S2F marked (s2 %d s2f %d)", O.s2, O.s2f);
    const int sky = find(K_SKY), light = find(K_LIGHT), fog = find(K_FOG);
    CHECK(P.node[sky].oc == nb_oc_mark_tail(0) && P.node[light].oc == nb_oc_mark_tail(1) && P.node[fog].oc == nb_oc_mark_tail(2), "tail marks %#x %#x %#x",
          P.node[sky].oc, P.node[light].oc, P.node[fog].oc);
    CHECK(nb_oc_role(P.node[sky].oc) == NB_OCR_SKY && nb_oc_role(P.node[light].oc) == NB_OCR_LIGHT && nb_oc_role(P.node[fog].oc) == NB_OCR_FOG, "tail roles");
    CHECK(nb_oc_tail_image(&F, &P, sky) == G0 && nb_oc_tail_image(&F, &P, light) == 0 && nb_oc_tail_image(&F, &P, fog) == 0, "c0 for Sky's header");
    CHECK(O.nchain == 1 && O.fog[0] == fog, "chain records the Fog node");
    CHECK(P.node[fog].load[0] == NB_LOAD_CLEAR, "the c0 clear folded into Fog (load %d)", P.node[fog].load[0]);
    { V w = v; w.s3 = 1; CHECK(run(w, 7) == 1 && O.s2f == 1 && O.s3 == 0 && P.node[find(K_PCF)].oc == 0, "S3 + Fog: S2F, PCF unmarked"); }
    { V w = v; w.fog_reads_c0 = 1; const int m = run(w, 7); CHECK(m == 1 && O.s2 == 1 && O.s2f == 0 && O.why[NB_OCW_FOG] + O.why[NB_OCW_NO_FOG] == 1, "Fog sampling c0 (its own target): tail refused, S2 kept (%d: s2 %d s2f %d fog %d nofog %d)", m, O.s2, O.s2f, O.why[NB_OCW_FOG], O.why[NB_OCW_NO_FOG]); }
    { V w = v; w.sky_reads_c0 = 1; CHECK(run(w, 7) == 1 && O.s2 == 1 && O.why[NB_OCW_FOG] == 1, "Sky sampling c0: tail refused"); }
    { V w = v; w.fog_draws2 = 1; CHECK(run(w, 7) == 1 && O.s2 == 1 && O.why[NB_OCW_FOG] == 1, "Fog of 2 draws: tail refused"); }
    { V w = v; w.op_before_fog = 1; CHECK(run(w, 7) == 1 && O.s2 == 1 && O.why[NB_OCW_NO_FOG] == 1, "a copy between Light and Fog: no tail"); }
    { V w{}; CHECK(run(w, 7) == 1 && O.s2 == 1 && O.why[NB_OCW_FOG] == 1, "Fog writing Df itself (c0 == Df): tail refused"); }
    { V w = v; w.light_no_sp = 1; CHECK(run(w, 7) == 0, "no S2 under it: nothing"); }
    { const PS same = {{2160, 972}, {2160, 972}, {2160, 972}, {2160, 972}, 0}; ps = same;
      CHECK(run(v, 7, true) == 1 && O.s2f == 1, "per-pass sizes: S2F");
      V w = v; w.fog_small = 1; CHECK(run(w, 7, true) == 1 && O.s2 == 1 && O.why[NB_OCW_FOG] == 1, "c0 smaller than Df at Fog: tail refused"); }
    // stepping of a tail chain: Sky begins, Light steps to 1, Fog steps to 2, then anything else ends M
    nb_oc_run r{}; r.head = r.light = r.fog = -1;
    const uint8_t ts = nb_oc_mark_tail(0), tl = nb_oc_mark_tail(1), tf = nb_oc_mark_tail(2);
    CHECK(nb_oc_step(&r, 1, tf, 5, 9) == NB_OCS_NONE && nb_oc_step(&r, 1, tl, 5, 8) == NB_OCS_NONE, "Fog / Light alone: none");
    CHECK(nb_oc_step(&r, 1, ts, 5, 3) == NB_OCS_BEGIN, "tail Sky begins"); nb_oc_apply(&r, NB_OCS_BEGIN, 5, 3);
    CHECK(nb_oc_step(&r, 1, tf, 5, 9) == NB_OCS_BREAK, "Fog right after Sky (Light missing): break");
    CHECK(nb_oc_step(&r, 1, tl, 5, 5) == NB_OCS_NEXT, "tail Light: next"); nb_oc_apply(&r, NB_OCS_NEXT, 5, 5);
    CHECK(r.k == 1 && r.light == 5 && nb_oc_steps_to_end(3, r.k) == 1, "in Light of 3: one empty subpass left");
    CHECK(nb_oc_step(&r, 1, tl, 5, 5) == NB_OCS_SAME && nb_oc_step(&r, 1, tf, 5, 4) == NB_OCS_BREAK, "Light same; Fog before Light: break");
    CHECK(nb_oc_step(&r, 1, nb_oc_mark(3, 2), 5, 7) == NB_OCS_BREAK, "a non-tail third member is not a Fog");
    CHECK(nb_oc_step(&r, 1, tf, 5, 7) == NB_OCS_NEXT, "Fog after Light: next"); nb_oc_apply(&r, NB_OCS_NEXT, 5, 7);
    CHECK(r.k == 2 && r.fog == 7 && nb_oc_steps_to_end(3, r.k) == 0, "in Fog: nothing to step");
    CHECK(nb_oc_step(&r, 1, tf, 5, 7) == NB_OCS_SAME, "Fog continues");
    CHECK(nb_oc_step(&r, 1, tf, 5, 8) == NB_OCS_BREAK && nb_oc_step(&r, 1, 0, 5, 8) == NB_OCS_BREAK && nb_oc_step(&r, 1, tl, 5, 5) == NB_OCS_BREAK, "after Fog: break");
    nb_oc_apply(&r, NB_OCS_BREAK, 0, -1); CHECK(!r.active && r.fog == -1, "ended");
    // non-tail S2: a Fog role never follows
    nb_oc_run q{}; q.head = q.light = q.fog = -1; nb_oc_apply(&q, NB_OCS_BEGIN, 5, 3); nb_oc_apply(&q, NB_OCS_NEXT, 5, 5);
    CHECK(nb_oc_step(&q, 1, nb_oc_mark(2, 1), 5, 7) == NB_OCS_BREAK, "S2: after Light a second Light breaks");
    CHECK(nb_oc_role((uint8_t)(nb_oc_mark(3, 3) | NB_OC_TAIL)) == NB_OCR_NONE && nb_oc_role((uint8_t)(nb_oc_mark(3, 0) | NB_OC_TAIL | 0x20)) == NB_OCR_NONE, "malformed tail marks");
}
static void stepping() {
    nb_oc_run r{}; r.head = r.light = r.fog = -1;
    const uint8_t sky = nb_oc_mark(2, 0), light = nb_oc_mark(2, 1), pcf3 = nb_oc_mark(3, 0), sky3 = nb_oc_mark(3, 1), light3 = nb_oc_mark(3, 2);
    CHECK(nb_oc_step(&r, 1, 0, 5, 3) == NB_OCS_NONE && nb_oc_step(&r, 0, 0, 0, -1) == NB_OCS_NONE, "nothing open, unmarked: none");
    CHECK(nb_oc_step(&r, 1, light, 5, 4) == NB_OCS_NONE && nb_oc_step(&r, 1, pcf3, 5, 2) == NB_OCS_NONE, "Light / PCF alone: none (PCF is its own pass)");
    CHECK(nb_oc_step(&r, 1, sky, 5, 3) == NB_OCS_BEGIN && nb_oc_step(&r, 1, sky3, 5, 3) == NB_OCS_BEGIN, "Sky begins (S2 and S3)");
    nb_oc_apply(&r, NB_OCS_BEGIN, 5, 3);
    CHECK(r.active && r.k == 0 && r.head == 3, "begun");
    CHECK(nb_oc_step(&r, 1, sky, 5, 3) == NB_OCS_SAME, "Sky's clear / second host pass: same");
    CHECK(nb_oc_step(&r, 1, sky, 6, 3) == NB_OCS_BREAK && nb_oc_step(&r, 0, 0, 0, -1) == NB_OCS_BREAK && nb_oc_step(&r, 1, 0, 5, 4) == NB_OCS_BREAK,
          "another part / no header / an unmarked pass: break");
    CHECK(nb_oc_step(&r, 1, light, 5, 3) == NB_OCS_BREAK && nb_oc_step(&r, 1, light, 5, 2) == NB_OCS_BREAK, "Light at or before the head: break");
    CHECK(nb_oc_step(&r, 1, light, 5, 5) == NB_OCS_NEXT && nb_oc_step(&r, 1, light3, 5, 5) == NB_OCS_NEXT, "Light after Sky (inert nodes between): next");
    CHECK(nb_oc_steps_to_end(2, r.k) == 1, "ended after Sky: one empty subpass");
    nb_oc_apply(&r, NB_OCS_NEXT, 5, 5);
    CHECK(r.k == 1 && r.light == 5 && nb_oc_steps_to_end(2, r.k) == 0, "in Light: nothing to step");
    CHECK(nb_oc_step(&r, 1, light, 5, 5) == NB_OCS_SAME, "Light continues");
    CHECK(nb_oc_step(&r, 1, light, 5, 6) == NB_OCS_BREAK && nb_oc_step(&r, 1, sky, 5, 3) == NB_OCS_BREAK && nb_oc_step(&r, 1, 0, 5, 6) == NB_OCS_BREAK,
          "after Light anything else ends M");
    nb_oc_apply(&r, NB_OCS_BREAK, 0, -1);
    CHECK(!r.active && nb_oc_step(&r, 1, sky, 5, 3) == NB_OCS_BEGIN, "ended: a Sky begins again");
    // marks
    for (int n = 0; n < 4; n++) for (int k = 0; k < 4; k++) {
        const int role = nb_oc_role(nb_oc_mark(n, k));
        const int want = n == 3 ? (k == 0 ? NB_OCR_PCF : k == 1 ? NB_OCR_SKY : k == 2 ? NB_OCR_LIGHT : NB_OCR_NONE)
                       : n == 2 ? (k == 0 ? NB_OCR_SKY : k == 1 ? NB_OCR_LIGHT : NB_OCR_NONE) : NB_OCR_NONE;
        CHECK(role == want, "role of n=%d k=%d: %d, want %d", n, k, role, want);
    }
    CHECK(nb_oc_role(0) == NB_OCR_NONE && nb_oc_role(0x7f) == NB_OCR_NONE && nb_oc_role((uint8_t)(nb_oc_mark(2, 0) | 0x10)) == NB_OCR_NONE, "malformed marks");
}
static std::string slurp(const std::string &p) { FILE *f = fopen(p.c_str(), "rb"); if (!f) return ""; std::string s; char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f); return s; }
static std::map<uint64_t, std::pair<uint32_t, uint32_t>> d_img;            // "# image 0x.. WxH": last size per storage
static std::map<std::pair<int, int>, std::pair<uint32_t, uint32_t>> d_pass;   // "# passsize N a=WxH": per pass, as bound
static void dump_sizes(const std::string &t) {
    d_img.clear(); d_pass.clear();
    size_t at = 0;
    while (at < t.size()) {
        size_t e = t.find('\n', at); if (e == std::string::npos) e = t.size();
        const std::string l = t.substr(at, e - at); at = e + 1;
        unsigned long long im; unsigned w, h; int node, n;
        if (sscanf(l.c_str(), "# image %llx %ux%u", &im, &w, &h) == 3) d_img[(uint64_t)im] = {w, h};
        else if (sscanf(l.c_str(), "# passsize %d%n", &node, &n) == 1) {
            const char *p = l.c_str() + n; int a, k;
            while (sscanf(p, " %d=%ux%u%n", &a, &w, &h, &k) == 3) { d_pass[{node, a}] = {w, h}; p += k; }
        }
    }
}
static int dump_img_size(void *, uint64_t im, uint32_t *w, uint32_t *h) {
    auto it = d_img.find(im); if (it == d_img.end()) return 0; *w = it->second.first; *h = it->second.second; return 1;
}
static int dump_pass_size(void *, int node, int a, uint32_t *w, uint32_t *h) {
    auto it = d_pass.find({node, a}); if (it == d_pass.end()) return 0; *w = it->second.first; *h = it->second.second; return 1;
}
// [early clears] (NR-ONCHIP.md §11) the last thing before member node m that touched image im: 1 = a ClearTexture (NB_OP_CLEAR node; *at =
// its node), 2 = another use (sampled / ref / drawn attachment / in-pass clear), 0 = nothing in this part.
static int last_touch(const nb_frame *f, int m, uint64_t im, int *at) {
    for (int q = m - 1; q >= 0; q--) {
        const nb_node &n = f->node[q]; *at = q;
        for (int c = n.clear_first; c >= 0 && c < n.clear_first + n.nclear && c < f->nclear; c++) if (f->clr[c].image == im) return (n.kind & 0xff) == NB_OP_CLEAR ? 1 : 2;
        for (int r = n.ref_first; r >= 0 && r < n.ref_first + n.nref && r < f->nref; r++) if (f->ref[r].image == im) return 2;
        for (int i = 0; i < n.nsr && i < NB_MAX_SREAD; i++) if (n.sread[i] == im) return 2;
        if (n.kind == NB_NODE_PASS && n.draws) for (int a = 0; a < n.natt; a++) if (n.att[a].image == im) return 2;
    }
    return 0;
}
static int ec_chains, ec_before, ec_between, ec_fog_between;
static void early_clears(const char *d, const std::string &n) {
    for (int c = 0; c < O.nchain; c++) {
        const int sky = O.sky[c], mem[2] = {O.light[c], O.fog[c]}; ec_chains++;
        for (int k = 0; k < 2; k++) { const int m = mem[k]; if (m < 0) continue; const nb_node &x = F.node[m];
            for (int i = 0; i < x.nsr && i < NB_MAX_SREAD; i++) {
                const uint64_t im = x.sread[i]; bool own = false;
                for (int a = 0; a < F.node[sky].natt; a++) if (F.node[sky].att[a].image == im) own = true;
                if (own) continue;
                int at = -1; if (last_touch(&F, m, im, &at) != 1) continue;
                if (at < sky) { if (!k) ec_before++; }
                else { if (k) ec_fog_between++; else ec_between++; printf("  %s/%s: chain %d: %s reads %#llx cleared at node %d, after Sky %d\n", d, n.c_str(), c, k ? "Fog" : "Light", (unsigned long long)im, at, sky); }
            }
        }
    }
}
static void dumps(const std::string &logs) {
    int files = 0, s2 = 0, s3 = 0, light = 0, unmarked = 0, tab_size = 0, tab_marked = 0, by_pass_files = 0, by_pass_marked = 0, by_pass_light = 0, s2f = 0, s2f_c0 = 0;
    std::map<std::string, int> sp_sizes;
    for (const char *d : {"ra1-dumps", "rb1-dumps", "rc1-dumps"}) {
        std::vector<std::string> names;
        std::error_code ec;
        for (auto &e : std::filesystem::directory_iterator(logs + "/" + d, ec)) { const std::string n = e.path().filename().string(); if (n.rfind("nb_ir_", 0) == 0) names.push_back(n); }
        for (auto &n : names) {
            std::string t = slurp(logs + "/" + d + "/" + n);
            if (t.empty()) continue;
            if (nb_parse(t.c_str(), &F)) { CHECK(0, "%s/%s parses", d, n.c_str()); continue; }
            const nb_rules rules = {NB_RULES_EXACT, 0, 0};
            if (nb_plan(&F, nullptr, &rules, &P)) { CHECK(0, "%s/%s plans", d, n.c_str()); continue; }
            files++;
            if (nb_onchip_plan(&F, &P, nullptr, &O) < 0) { CHECK(0, "%s/%s onchip plan", d, n.c_str()); continue; }
            int lp = 0; for (int i = 0; i < F.nnode; i++) if (F.node[i].kind == NB_NODE_PASS && nb_oc_class(F.node[i].fs) == NB_OCR_LIGHT) lp++;
            light += lp; s2 += O.s2; s3 += O.s3; unmarked += lp - O.s2 - O.s3;
            if (lp && !(O.s2 + O.s3)) { printf("  %s/%s: Light pass not marked:", d, n.c_str()); for (int w = 2; w < NB_OCW_KINDS; w++) if (O.why[w]) printf(" %s %d", nb_oc_why_name(w), O.why[w]); printf("\n"); }
            // the rule the phone ran (DM_NR_ONCHIP 18.11): sizes from the tap's last-size-per-storage table = the "# image" lines
            dump_sizes(t);
            for (int c = 0; c < O.nchain; c++) {
                const uint64_t sp = F.node[O.sky[c]].att[0].image; auto it = d_img.find(sp);
                char b[64]; snprintf(b, sizeof b, "%ux%u", it == d_img.end() ? 0u : it->second.first, it == d_img.end() ? 0u : it->second.second);
                sp_sizes[b]++;
            }
            {   // S2F (allow bit 2): the Fog tail on the same parts, and c0 for Sky's header
                const nb_oc_rules rf = {nullptr, nullptr, 7, nullptr};
                if (nb_onchip_plan(&F, &P, &rf, &O) >= 0) {
                    s2f += O.s2f;
                    early_clears(d, n);
                    for (int c = 0; c < O.nchain; c++) if (O.fog[c] >= 0 && nb_oc_tail_image(&F, &P, O.sky[c]) == F.node[O.fog[c]].att[0].image) s2f_c0++;
                }
                if (nb_onchip_plan(&F, &P, nullptr, &O) < 0) CHECK(0, "replan");
            }
            const nb_oc_rules ri = {dump_img_size, nullptr, 0, nullptr};
            if (nb_onchip_plan(&F, &P, &ri, &O) >= 0) { tab_size += O.why[NB_OCW_SIZE]; tab_marked += O.s2 + O.s3; }
            if (!d_pass.empty()) {   // dumps of this build: the rule the front end runs now
                const nb_oc_rules rp = {dump_img_size, nullptr, 0, dump_pass_size};
                by_pass_files++; by_pass_light += lp;
                if (nb_onchip_plan(&F, &P, &rp, &O) >= 0) by_pass_marked += O.s2 + O.s3;
            }
        }
    }
    if (!files) { printf("dumps: none found under %s: skipped\n", logs.c_str()); return; }
    printf("dumps: %d parts, %d with a Light pass: S3 %d, S2 %d, not marked %d\n", files, light, s3, s2, unmarked);
    printf("dumps, the phone's rule (sizes from the last size per storage, \"# image\"): marked %d, refused \"sizes differ\" %d; Sp of the chains there:", tab_marked, tab_size);
    for (auto &e : sp_sizes) printf(" %s x%d", e.first.c_str(), e.second);
    printf("\n");
    CHECK(tab_marked == 0 && tab_size == light, "the image-keyed rule refuses every Light pass of the dumps, as on the phone (%d of %d)", tab_size, light);
    if (by_pass_files) {
        printf("dumps with per-pass sizes (\"# passsize\"): %d parts, %d Light passes, marked %d\n", by_pass_files, by_pass_light, by_pass_marked);
        CHECK(by_pass_marked == by_pass_light, "per-pass sizes mark every Light pass (%d of %d)", by_pass_marked, by_pass_light);
    } else printf("dumps with per-pass sizes: none yet (written from this build on with DM_NR_ONCHIP > 0)\n");
    CHECK(light > 0 && s2 + s3 == light, "every Light pass of the phone dumps is marked (%d of %d)", s2 + s3, light);
    CHECK(s3 > 0 && s2 > 0, "both kinds occur (S3 %d S2 %d)", s3, s2);
    printf("dumps, S2F (allow bit 2): %d of %d Light passes get the Fog tail, c0 found for Sky's header %d\n", s2f, light, s2f_c0);
    printf("dumps, early clears: %d chains; images Light samples whose last write is a ClearTexture: before Sky %d, between Sky and Light %d "
           "(Fog: between Sky and Fog %d)\n", ec_chains, ec_before, ec_between, ec_fog_between);
    CHECK(ec_before > 0 && ec_between == 0, "the pending clears Light reads are all recorded before Sky: landing them at M's begin moves no "
          "clear past a write (before %d, between %d)", ec_before, ec_between);
    CHECK(s2f == light && s2f_c0 == s2f, "every Light pass of the dumps gets the Fog tail (%d of %d; c0 %d) - the census of NR-FRAME.md §10", s2f, light, s2f_c0);
}
static void pack(const std::string &path) {
    const std::string b = slurp(path);
    if (b.size() < 16) { printf("pack: %s missing: skipped\n", path.c_str()); return; }
    uint32_t h[4]; memcpy(h, b.data(), 16);
    CHECK(h[0] == 0x50534758u && h[1] == 1u, "pack header");
    struct Ent { char key[32]; uint64_t off; uint32_t size, pad; uint64_t hash; };
    int seen = 0, s2f = 0;
    for (uint32_t i = 0; i < h[2] && 16 + (i + 1) * sizeof(Ent) <= b.size(); i++) {
        Ent e; memcpy(&e, b.data() + 16 + i * sizeof(Ent), sizeof e);
        const std::string k(e.key);
        const bool opt = k == "5.ClusteredLightingWithSSAO" || k == "5.DeferredFog";   // S2F (optional in the pack)
        if (k != "2.ClusteredLightingWithSSAO" && k != "3.ClusteredLightingWithSSAO" && k != "x.ClusteredLightingWithSSAO" && !opt) continue;
        CHECK(e.off + e.size <= b.size(), "%s in the file", k.c_str());
        const uint8_t *bun = (const uint8_t *)b.data() + e.off; uint32_t hw[8]; memcpy(hw, bun, 32);
        const size_t head = 32 + (size_t)hw[3] * 32 + (size_t)hw[4] * 4 + hw[5];
        std::vector<uint32_t> w(hw[6] / 4); memcpy(w.data(), bun + head, w.size() * 4);
        uint32_t bd[8], ix[8]; const int n = nb_oc_spv_inputs(w.data(), w.size(), bd, ix, 8);
        if (k[0] == '2') CHECK(n == 1 && bd[0] == 52 && ix[0] == 6, "light.s2: %d inputs (%u:%u)", n, bd[0], ix[0]);
        if (k[0] == '3') CHECK(n == 2 && ((bd[0] == 52 && ix[0] == 6 && bd[1] == 58 && ix[1] == 7) || (bd[1] == 52 && ix[1] == 6 && bd[0] == 58 && ix[0] == 7)), "light.s3: %d inputs", n);
        if (k[0] == 'x') CHECK(n == 0, "the NR-04 bundle has no subpass input (%d)", n);
        if (k == "5.ClusteredLightingWithSSAO") CHECK(n == 2 && ((bd[0] == 28 && ix[0] == 3 && bd[1] == 52 && ix[1] == 6) || (bd[1] == 28 && ix[1] == 3 && bd[0] == 52 && ix[0] == 6)), "light.s2f: %d inputs", n);
        if (k == "5.DeferredFog") CHECK(n == 1 && bd[0] == 16 && ix[0] == 1, "fog.s2f: %d inputs (%u:%u)", n, bd[0], ix[0]);
        if (opt) { s2f++; continue; }
        seen++;
    }
    CHECK(seen == 3, "pack holds x / 2 / 3 .ClusteredLightingWithSSAO (%d)", seen);
    printf("pack: %s: light.s2 / light.s3 subpass inputs checked; optional S2F bundles %d (light.s2f / fog.s2f inputs checked when present)\n", path.c_str(), s2f);
    const uint32_t bad[3] = {0x07230203u, 0, 0};
    CHECK(nb_oc_spv_inputs(bad, 3, nullptr, nullptr, 0) == -1, "short module");
}
// [early clears] the members' learned sampled images (nb_oc_learn_*): per member, replaced on every check, Fog's only with the tail
static void learned() {
    nb_oc_learned l{};
    nb_oc_learn_add(&l, 0, 29); nb_oc_learn_add(&l, 0, 29); nb_oc_learn_add(&l, 0, 7); nb_oc_learn_add(&l, 1, 41); nb_oc_learn_add(&l, 0, 0);
    CHECK(l.n[0] == 2 && l.n[1] == 1, "deduplicated, serial 0 ignored (%d %d)", l.n[0], l.n[1]);
    CHECK(nb_oc_learned_has(&l, 1, 29) && nb_oc_learned_has(&l, 1, 7) && !nb_oc_learned_has(&l, 1, 41), "M without the Fog subpass: Light's images only");
    CHECK(nb_oc_learned_has(&l, 2, 41) && !nb_oc_learned_has(&l, 2, 5) && !nb_oc_learned_has(&l, 2, 0), "S2F: Fog's images too, nothing else");
    nb_oc_learn_reset(&l, 0); nb_oc_learn_add(&l, 0, 30);
    CHECK(!nb_oc_learned_has(&l, 2, 29) && nb_oc_learned_has(&l, 2, 30) && nb_oc_learned_has(&l, 2, 41), "a new check of Light replaces only Light's set");
    for (uint64_t x = 100; x < 100 + 2 * NB_OC_LEARN; x++) nb_oc_learn_add(&l, 1, x);
    CHECK(l.n[1] == NB_OC_LEARN && nb_oc_learned_has(&l, 2, 41) && !nb_oc_learned_has(&l, 2, 100 + 2 * NB_OC_LEARN - 1), "capped (the rest not learned)");
    nb_oc_learn_add(&l, 2, 5); nb_oc_learn_reset(&l, -1);
    CHECK(!nb_oc_learned_has(&l, 2, 5) && l.n[0] == 1, "out-of-range member ignored");
}
int main(int argc, char **argv) {
    learned();
    synthetic();
    sizes_by_pass();
    stepping();
    s2f();
    dumps(argc > 1 ? argv[1] : "../../../../../work/logs");
    pack(argc > 2 ? argv[2] : "../../../../../../work/native-onchip/native-nr.xgspack");
    printf("nb_onchip_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
