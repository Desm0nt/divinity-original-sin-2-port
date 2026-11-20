// [DOS2] NR-TCOUNT host test (docs/dos2/nr/NR-TCOUNT.md): synthetic frames for each counter of dm/gfx/nr/nr_tcount.h.
// clang++ -std=c++17 -O2 -Wall -Wextra nr_tcount_test.cpp -o nr_tcount_test
#include "../../../nr/nr_tcount.h"
#include <stdio.h>
#include <stdlib.h>

static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define EQ(a, b) do { ++checks; const unsigned long long x_ = (unsigned long long)(a), y_ = (unsigned long long)(b); if (x_ != y_) { ++fails; printf("FAIL %s:%d: %s = %llu, want %llu\n", __FILE__, __LINE__, #a, x_, y_); } } while (0)

// a frame builder in pgraph's event order
struct B {
    nrtc_frame f;
    int img(uint32_t w, uint32_t h, uint32_t fmt = 0x25, bool depth = false) { f.imgs.push_back(nrtc_img{uint64_t(100 + f.imgs.size()), fmt, w, h, depth}); return int(f.imgs.size()) - 1; }
    // a whole pass: begin, draws, end
    nrtc_pass &pass(std::vector<int> att, uint32_t draws = 4, std::vector<nrtc_rd> in = {}, int sub = 0) {
        nrtc_pass p; p.att = att; p.w = f.imgs[size_t(att[0])].w; p.h = f.imgs[size_t(att[0])].h; p.draws = draws; p.in = in; p.sub = sub; p.fs = 0xabc00000u + uint32_t(f.passes.size());
        f.passes.push_back(p); const int i = int(f.passes.size()) - 1;
        f.evs.push_back(nrtc_ev{'P', i}); f.evs.push_back(nrtc_ev{'E', i});
        return f.passes.back();
    }
    // a pass whose end comes later (events inside it)
    int open(std::vector<int> att, uint32_t draws = 4) { pass(att, draws); f.evs.pop_back(); return int(f.passes.size()) - 1; }
    void close(int i) { f.evs.push_back(nrtc_ev{'E', i}); }
    nrtc_op &op(char k, unsigned host, std::vector<nrtc_rd> in, std::vector<int> out) {
        nrtc_op o; o.k = k; o.host_op = host; o.in = in; o.out = out; f.ops.push_back(o); f.evs.push_back(nrtc_ev{'O', int(f.ops.size()) - 1}); return f.ops.back();
    }
    void ev(char k, int op) { f.evs.push_back(nrtc_ev{k, op}); }
};
static const uint64_t FULL = 1920ull * 1080 * 4;   // 4 B/px of a 1920x1080 attachment

static void t3_vs_only() {
    B b; const int g0 = b.img(1920, 1080), g1 = b.img(1920, 1080), sk = b.img(64, 64);
    // G-buffer pass ending with a VS-only skinning draw; the host ends it at the reader's barrier, the next pass re-begins the same set
    nrtc_pass &p0 = b.pass({g0, g1}, 10); p0.vso = 1; p0.vs = 0x5c1a0001; p0.last_vso = p0.last_vsw = true;
    b.pass({g0, g1}, 6);
    // a pass made of VS-only draws only (palette writes into a tiny target)
    nrtc_pass &p2 = b.pass({sk}, 3); p2.vso = 3; p2.vs = 0x5c1a0002; p2.first_vso = p2.first_vsw = p2.last_vso = p2.last_vsw = true;
    // a VS storage writer that also draws, first in its pass after a WAR-barrier break
    b.pass({g0, g1}, 2);
    nrtc_pass &p4 = b.pass({g0, g1}, 2); p4.vsw = 1; p4.first_vsw = true;
    nrtc_acc a; nrtc_analyze(b.f, a);
    const nrtc_cls &c = a.c[NRTC_3D];
    EQ(c.vso, 4); EQ(c.vsw, 1); EQ(c.vso_passes, 2); EQ(c.vso_only, 1); EQ(c.vso_only_bytes, 64ull * 64 * 4 * 2);
    EQ(c.vsw_split, 2); EQ(c.vso_split, 1); EQ(c.vsw_split_bytes, 2 * 2 * 2 * FULL);   // two pairs, two attachments, store + load
    EQ(a.c[NRTC_UI].vso, 0);
    std::string l[4]; nrtc_lines(a, 1, 4, l);
    CHECK(l[0].find("[nr] tcount T3 DM_NR_TCOUNT=1: 1 frames sampled (1 in 4), 1 with 3D") == 0);
    CHECK(l[0].find("vs5c1a0002 3.00 in 64x64:25:fsabc00002") != std::string::npos);
    CHECK(l[0].find("VS-only draws 4.00 / 0.00 in passes 2.00 / 0.00 (only VS-only 1.00 / 0.00 = 0.0 / 0.0 MB)") != std::string::npos);
    CHECK(l[0].find("next to a VS storage write 2.00 / 0.00 (next to a VS-only draw 1.00 / 0.00) = 63.3 / 0.0 MB") != std::string::npos);
    printf("T3: VS-only 4 in 2 passes (1 only VS-only), 2 re-opens next to a VS storage write = %.1f MB\n", c.vsw_split_bytes / 1048576.);
}

static void t4_reopen() {
    B b; const int A = b.img(1920, 1080), Bt = b.img(960, 540), C = b.img(960, 540), E = b.img(960, 540), X = b.img(256, 256), G = b.img(480, 270);
    const int H = b.img(1920, 1080), I = b.img(1920, 1080), Z = b.img(1920, 1080, 0x3a, true), J = b.img(512, 512), K = b.img(128, 128);
    b.pass({A});                                   // p0
    b.pass({Bt});                                  // p1 independent
    b.pass({A});                                   // p2 -> (p0,p2) reorderable, exact
    b.pass({C}, 4, {{A, 's'}});                    // p3 samples A
    nrtc_pass &p4 = b.pass({A}); p4.in = {{X, 's'}};   // p4 -> (p2,p4) blocked: p3 uses the target; p4 samples X
    nrtc_pass &p5 = b.pass({E}); p5.xout = {X};    // p5 writes E and X (storage): p4 cannot move down
    b.pass({A}, 4, {{E, 's'}});                    // p6 samples E: cannot move up -> (p4,p6) other dependency
    b.pass({G});                                   // p7
    b.pass({A}, 4, {}, 1);                         // p8 in the next submit -> (p6,p8) submit
    b.pass({H, Z}); b.pass({I, Z});                // p9 p10: Z re-opened with another attachment set
    b.pass({J}); nrtc_pass &p12 = b.pass({K}); p12.buf_write = true; b.pass({J});   // (p11,p13) reorderable, not exact (a buffer write between)
    nrtc_acc a; nrtc_analyze(b.f, a);
    const nrtc_cls &c = a.c[NRTC_3D];
    EQ(c.aba, 5); EQ(c.aba_ok, 2); EQ(c.aba_exact, 1); EQ(c.aba_touch, 1); EQ(c.aba_dep, 1); EQ(c.aba_sub, 1); EQ(c.aba_cpu, 0);
    EQ(c.aba_ok_bytes, 2 * FULL + 512ull * 512 * 4 * 2);
    EQ(c.diff, 1); EQ(c.diff_bytes, 2 * FULL);
    EQ(c.img_n[2], 1);   // A: 5 opens
    EQ(c.img_n[1], 2);   // Z, J
    EQ(c.max_open, 5);
    EQ(c.reopen, 4 + 1 + 1); EQ(c.reopen_bytes, 4 * 2 * FULL + 2 * FULL + 512ull * 512 * 8);
    const nrtc_target &ta = a.tg[nrtc_tkey(b.f.imgs[size_t(A)])];
    EQ(ta.opens, 5); EQ(ta.pairs_between, 4); EQ(ta.pairs_ok, 1);
    CHECK(ta.between == "960x540:25:fsabc00001");
    std::string l[4]; nrtc_lines(a, 1, 4, l);
    CHECK(l[3].find("[nr] tcount top: re-opened targets by MB a frame: 1) #100 1920x1080 fmt 0x25 opened 5.0, re-opens 63.3 MB, pairs with passes between 4.00 (reorderable 1.00), between: 960x540:25:fsabc00001;") == 0);
    CHECK(l[1].find("images opened 1x 7.0 / 0.0, 2x 2.0 / 0.0, 3+x 1.0 / 0.0 (most 5 / 0)") != std::string::npos);
    CHECK(l[1].find("reorderable 2.00 / 0.00 (exact 1.00 / 0.00; the rest if buffer writes allow) = 17.8 / 0.0 MB") != std::string::npos);
    // a load DONT_CARE (NOLOAD) on the later open and a DONT_CARE store on the earlier one cost nothing
    B d; const int Q = d.img(1920, 1080); nrtc_pass &q0 = d.pass({Q}); q0.store = 1; nrtc_pass &q1 = d.pass({Q}); q1.kind = 'N';
    nrtc_acc ad; nrtc_analyze(d.f, ad); EQ(ad.c[0].reopen, 1); EQ(ad.c[0].reopen_bytes, 0); EQ(ad.c[0].aa, 1); EQ(ad.c[0].aa_bytes, 0);
    // the NB plan's load DONT_CARE (bit per attachment): only the store of the earlier open remains; depth under 'N' still loads
    B n; const int c0 = n.img(1920, 1080), z0 = n.img(1920, 1080, 0x3a, true); n.pass({c0, z0}); nrtc_pass &n1 = n.pass({c0, z0}); n1.dc = 1;
    nrtc_pass &n2 = n.pass({c0, z0}); n2.kind = 'N';
    nrtc_acc an; nrtc_analyze(n.f, an); EQ(an.c[0].reopen_bytes, (FULL + 2 * FULL) + (FULL + 2 * FULL)); EQ(an.c[0].open_bytes, 4 * FULL + 3 * FULL + 3 * FULL);
    if (getenv("NRTC_SHOW")) for (auto &x : l) puts(x.c_str());
    printf("T4: A opened 5x, 5 pairs with passes between: reorderable 2 (exact 1), touch 1, dependency 1, submit 1; another set 1\n");
}

static void t5_breaks() {
    B b; const int A = b.img(1920, 1080), S = b.img(256, 256), Bt = b.img(1920, 1080), X = b.img(64, 64), C = b.img(1920, 1080), Y = b.img(64, 64);
    const int D = b.img(1920, 1080), E = b.img(1920, 1080), F = b.img(1920, 1080), G = b.img(1920, 1080), U = b.img(2400, 1080), W = b.img(1920, 1080);
    b.pass({A}); b.op('x', 36, {{S, 't'}}, {A}); b.pass({A});                 // p0 COPY_T2T into A p1: drawable in the pass
    b.pass({Bt}); b.op('x', 38, {}, {X}); b.pass({Bt});                         // p2 CLEAR_TEX of another image p3: moves out
    b.pass({C}); b.op('x', 36, {{C, 't'}}, {Y}); b.pass({C});                   // p4 a copy out of C p5: needs the store
    b.pass({D}); nrtc_pass &k = b.pass({D}, 0); k.kind = 'K'; b.pass({D});      // p6 host clear pass on D p8: in-pass clear (K open cost added)
    nrtc_pass &p9 = b.pass({E}); p9.end_op = 9; p9.end_line = 1234; b.pass({E});   // p9 p10 back to back: mergeable
    nrtc_pass &p11 = b.pass({F}); p11.end_img = F; p11.end_why = 6; b.pass({F}, 4, {{F, 's'}});   // p11 p12: the second samples F (feedback)
    b.pass({G}); b.pass({G}, 4, {}, 1);                                         // p13 p14 across a submit
    nrtc_pass &u0 = b.pass({U}); u0.ui = true; nrtc_pass &u1 = b.pass({U}); u1.ui = true;   // UI back to back
    // a query / report inside the earlier pass does not block; one between the passes does
    const int w0 = b.open({W}); b.ev('C', 5); b.close(w0); b.pass({W});        // p17 p18 mergeable
    b.ev('C', 5); b.pass({W});                                                  // p18 p19: report between -> not mergeable
    nrtc_acc a; nrtc_analyze(b.f, a);
    const nrtc_cls &c = a.c[NRTC_3D], &u = a.c[NRTC_UI];
    EQ(c.ops, 4); EQ(c.ops_ok, 3); EQ(c.op_into, 2); EQ(c.op_indep, 1); EQ(c.op_out, 1); EQ(c.op_dep, 0);
    EQ(c.ops_bytes, 4 * 2 * FULL + FULL); EQ(c.ops_ok_bytes, 3 * 2 * FULL + FULL);   // the K pass stores D once more
    EQ(a.op_kind["COPY_T2T into the target"], 1); EQ(a.op_kind["CLEAR_TEX independent"], 1); EQ(a.op_kind["COPY_T2T reads the target"], 1); EQ(a.op_kind["clear pass into the target"], 1);
    EQ(c.aa, 5); EQ(c.aa_ok, 2); EQ(c.aa_fb, 1); EQ(c.aa_sub, 1); EQ(c.aa_bytes, 5 * 2 * FULL);
    EQ(a.aa_end["CLEAR_COLOR/L1234"], 1); EQ(a.aa_end["read break why=6"], 1);
    EQ(u.aa, 1); EQ(u.aa_ok, 1); EQ(u.aa_ok_bytes, 2400ull * 1080 * 4 * 2); EQ(u.passes, 2);
    std::string l[4]; nrtc_lines(a, 1, 4, l);
    CHECK(l[2].find("[nr] tcount T5: a frame 3D / UI: same target re-begun with only copies / clears / dispatches between 4.00 / 0.00 = 71.2 / 0.0 MB: removable 3.00 / 0.00 = 55.4 / 0.0 MB") == 0);
    CHECK(l[2].find("back-to-back passes into the same target 5.00 / 1.00 = 79.1 / 19.8 MB: mergeable 2.00 / 1.00") != std::string::npos);
    if (getenv("NRTC_SHOW")) for (auto &x : l) puts(x.c_str());
    printf("T5: 4 copy / clear breaks (3 removable: 2 drawable in the pass, 1 moves out), back to back 5 + 1 UI (mergeable 2 + 1)\n");
}

static void lines_fit() {
    nrtc_acc a; B b;
    for (int k = 0; k < 40; k++) { const int x = b.img(2400 - k, 1080, 0x25 + k), y = b.img(1200, 540); for (int r = 0; r < 4; r++) { b.pass({x}); b.pass({y}, 3, {{x, 's'}}); } }
    for (int k = 0; k < 20; k++) b.op('x', 30 + k, {}, {b.img(16, 16)});
    for (int f = 0; f < 3; f++) nrtc_analyze(b.f, a);
    for (int k = 0; k < 30; k++) a.aa_end["a rather long end reason number " + std::to_string(k)] = 5;
    std::string l[4]; nrtc_lines(a, 1, 600, l);
    for (auto &s : l) { CHECK(s.size() < 1024); CHECK(s.find("[nr] tcount ") == 0); }
    CHECK(a.frames == 3);
    nrtc_acc e; std::string m[4]; nrtc_lines(e, 1, 4, m); CHECK(m[3] == "[nr] tcount top: re-opened targets by MB a frame: none");
    printf("lines: %zu / %zu / %zu / %zu B (< 1024)\n", l[0].size(), l[1].size(), l[2].size(), l[3].size());
}

int main() {
    t3_vs_only(); t4_reopen(); t5_breaks(); lines_fit();
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "all passed", checks, fails);
    return fails ? 1 : 0;
}
