// [DOS2] T2 DM_NR_IDVS: the production PointSize strip (xbg_spirv_idvs.cpp) on synthetic modules in the translator's form
// (gl_PerVertex output block, PointSize written through OpAccessChain member 1, gl_Position component stores, a Location output):
//   A  translator form: the PointSize store and its chain go; gl_Position / Location stores and everything else stay word for word;
//      xbg_spirv_idvs_stripped is false before, true after; a second strip finds nothing;
//   B  PointSize read back (OpLoad of its chain): refused;
//   C  a PointSize variable (OpDecorate BuiltIn PointSize) instead of a block member: its store goes, the variable stays;
//   D  the output block stored whole: refused;
//   E  a fragment module: refused; a vertex module without PointSize: "no PointSize output";
//   F  two PointSize stores (default + a later one): both go;
//   G  xbg_spirv_idvs_check accepts every strip above and rejects tampered ones (a gl_Position store removed, an arithmetic op
//      removed, a word changed, a kept chain dropped while a store still uses it).
// The real pack is covered by tests/idvs_scan.cpp + tools/dos2_idvs_check.py (spirv-val, interpreter, malioc); this test needs no data.
//   clang++ / g++ -O2 -std=c++17 tests/idvs_test.cpp xbg_spirv_idvs.cpp -o idvs_test && ./idvs_test
#include "../xbg_spirv_idvs.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace {
int failures = 0;
void expect(bool ok, const char *what) { if (!ok) { failures++; printf("FAIL: %s\n", what); } }
struct B {
    std::vector<uint32_t> w{0x07230203u, 0x00010000u, 0, 1000, 0};
    void op(uint32_t code, std::initializer_list<uint32_t> a) { w.push_back(uint32_t((a.size() + 1) << 16) | code); w.insert(w.end(), a); }
    void entry(uint32_t model, uint32_t fn, std::initializer_list<uint32_t> iface) {
        std::vector<uint32_t> a{model, fn, 0x6e69616du, 0}; a.insert(a.end(), iface); w.push_back(uint32_t((a.size() + 1) << 16) | 15u); w.insert(w.end(), a.begin(), a.end()); }
};
// ids: 1 void, 2 fn type, 3 float, 4 vec4, 6 int, 7..10 int 0..3, 11 ptr Output vec4, 12 ptr Output float, 16 float 1.0, 17 float 0.0,
// 21 int 8, 22 float[8], 24 float[1], 25 gl_PerVertex, 26 ptr Output gl_PerVertex, 27 ptr Input vec4, 28 input loc0, 30 the block, 31 loc0 output,
// 70 a PointSize variable (variant C)
std::vector<uint32_t> vs(char v, uint32_t model = 0) {
    B b; b.op(17, {1}); b.op(14, {0, 1});
    if (v == 'C') b.entry(model, 40, {28, 30, 31, 70}); else b.entry(model, 40, {28, 30, 31});
    b.op(16, {40, 7}); b.op(5, {30, 0x7265705fu, 0});
    b.op(71, {25, 2}); b.op(72, {25, 0, 11, 0});
    if (v != 'N') b.op(72, {25, 1, 11, 1});
    b.op(72, {25, 2, 11, 3}); b.op(72, {25, 3, 11, 4});
    b.op(71, {28, 30, 0}); b.op(71, {31, 30, 0}); if (v == 'C') b.op(71, {70, 11, 1});
    b.op(19, {1}); b.op(33, {2, 1}); b.op(22, {3, 32}); b.op(23, {4, 3, 4}); b.op(21, {6, 32, 1});
    for (uint32_t k = 0; k < 4; k++) b.op(43, {6, 7 + k, k});
    b.op(43, {6, 21, 8}); b.op(28, {22, 3, 21}); b.op(28, {24, 3, 8}); b.op(30, {25, 4, 3, 22, 24});
    b.op(32, {11, 3, 4}); b.op(32, {12, 3, 3}); b.op(32, {26, 3, 25}); b.op(32, {27, 1, 4});
    b.op(43, {3, 16, 0x3f800000u}); b.op(43, {3, 17, 0});
    b.op(59, {27, 28, 1}); b.op(59, {26, 30, 3}); b.op(59, {11, 31, 3}); if (v == 'C') b.op(59, {12, 70, 3});
    b.op(54, {1, 40, 0, 2}); b.op(248, {41});
    if (v == 'C') b.op(62, {70, 16});
    else if (v != 'N') { b.op(65, {12, 50, 30, 8}); b.op(62, {50, 16}); }                      // PointSize = 1.0 (translator default)
    for (uint32_t k = 0; k < 4; k++) { b.op(65, {12, 51 + k, 30, 7, 7 + k}); b.op(62, {51 + k, k == 3 ? 16u : 17u}); }   // gl_Position = (0,0,0,1)
    b.op(61, {4, 55, 28}); b.op(62, {31, 55});                                                // loc0 = input
    b.op(65, {11, 57, 30, 7}); b.op(142, {4, 59, 55, 16}); b.op(62, {57, 59});                // gl_Position = input * 1.0
    if (v == 'B') b.op(61, {3, 61, 50});                                                      // PointSize read back
    if (v == 'D') { b.op(61, {25, 62, 30}); b.op(62, {30, 62}); }                             // block loaded and stored whole
    if (v == 'F') { b.op(65, {12, 63, 30, 8}); b.op(62, {63, 17}); }                          // a second PointSize store
    b.op(253, {}); b.op(56, {});
    return b.w;
}
size_t count_op(const std::vector<uint32_t> &w, uint32_t op) { size_t n = 0; for (size_t i = 5; i < w.size(); i += w[i] >> 16) { if ((w[i] & 0xFFFF) == op) n++; if (!(w[i] >> 16)) break; } return n; }
// remove the instruction at word offset `at`
std::vector<uint32_t> cut(const std::vector<uint32_t> &w, size_t at) { std::vector<uint32_t> o(w.begin(), w.begin() + ptrdiff_t(at)); o.insert(o.end(), w.begin() + ptrdiff_t(at + (w[at] >> 16)), w.end()); return o; }
size_t find_op(const std::vector<uint32_t> &w, uint32_t op, uint32_t word1, size_t nth = 0) {
    for (size_t i = 5; i < w.size(); i += w[i] >> 16) { if ((w[i] & 0xFFFF) == op && w.size() > i + 1 && w[i + 1] == word1 && !nth--) return i; if (!(w[i] >> 16)) break; }
    return 0; }
}  // namespace

int main() {
    std::vector<uint32_t> out; XbgIdvsStats st;
    {   // A
        auto in = vs('A');
        expect(!xbg_spirv_idvs_stripped(in.data(), in.size()), "A: stripped() false before");
        expect(xbg_spirv_idvs_strip(in.data(), in.size(), out, &st) == 1, "A: one store dropped");
        expect(st.stores_dropped == 1 && st.chains_dropped == 1 && !*st.why, "A: stats");
        expect(out.size() == in.size() - 5 - 3, "A: exactly the chain (5 words) and the store (3 words) removed");
        expect(count_op(out, 62) == count_op(in, 62) - 1 && count_op(out, 65) == count_op(in, 65) - 1, "A: one store, one chain fewer");
        expect(!*xbg_spirv_idvs_check(in.data(), in.size(), out.data(), out.size()), "A: check accepts");
        expect(xbg_spirv_idvs_stripped(out.data(), out.size()), "A: stripped() true after");
        std::vector<uint32_t> again; expect(xbg_spirv_idvs_strip(out.data(), out.size(), again, &st) == 0 && !strcmp(st.why, "no PointSize store"), "A: nothing left to strip");
        // G on A: tampering
        auto t1 = cut(out, find_op(out, 62, 57)); expect(*xbg_spirv_idvs_check(in.data(), in.size(), t1.data(), t1.size()) != 0, "G: a gl_Position store removed is rejected");
        auto t2 = cut(out, find_op(out, 142, 4)); expect(*xbg_spirv_idvs_check(in.data(), in.size(), t2.data(), t2.size()) != 0, "G: an arithmetic op removed is rejected");
        auto t3 = out; t3[find_op(t3, 43, 3) + 3] = 0x40000000u; expect(*xbg_spirv_idvs_check(in.data(), in.size(), t3.data(), t3.size()) != 0, "G: a changed constant is rejected");
        auto t4 = cut(out, find_op(out, 65, 11)); expect(*xbg_spirv_idvs_check(in.data(), in.size(), t4.data(), t4.size()) != 0, "G: a kept chain removed is rejected");
        expect(*xbg_spirv_idvs_check(in.data(), in.size(), in.data(), in.size()) != 0, "G: an unchanged module is not a strip");
        // the PointSize store alone removed (its chain left unused) is also a valid strip
        auto t5 = cut(in, find_op(in, 62, 50)); expect(!*xbg_spirv_idvs_check(in.data(), in.size(), t5.data(), t5.size()), "G: store-only removal accepted");
        // the chain removed but its store kept: the store now names a missing id -> rejected
        auto t6 = cut(in, find_op(in, 65, 12)); expect(*xbg_spirv_idvs_check(in.data(), in.size(), t6.data(), t6.size()) != 0, "G: chain removed under a live store rejected");
    }
    {   // B
        auto in = vs('B'); expect(xbg_spirv_idvs_strip(in.data(), in.size(), out, &st) == 0 && out.empty() && strstr(st.why, "read"), "B: PointSize read back refuses");
        expect(!xbg_spirv_idvs_stripped(in.data(), in.size()), "B: stripped() false");
    }
    {   // C
        auto in = vs('C'); expect(xbg_spirv_idvs_strip(in.data(), in.size(), out, &st) == 1 && st.chains_dropped == 0, "C: PointSize variable store dropped");
        expect(!*xbg_spirv_idvs_check(in.data(), in.size(), out.data(), out.size()), "C: check accepts");
        expect(count_op(out, 59) == count_op(in, 59) && find_op(out, 71, 70) != 0, "C: the variable and its decoration stay");
        expect(xbg_spirv_idvs_stripped(out.data(), out.size()), "C: stripped() true after");
    }
    {   // D
        auto in = vs('D'); expect(xbg_spirv_idvs_strip(in.data(), in.size(), out, &st) == 0 && strstr(st.why, "whole"), "D: whole block use refuses");
    }
    {   // E
        auto in = vs('A', 4); expect(xbg_spirv_idvs_strip(in.data(), in.size(), out, &st) == 0 && strstr(st.why, "vertex"), "E: fragment model refuses");
        auto n = vs('N'); expect(xbg_spirv_idvs_strip(n.data(), n.size(), out, &st) == 0 && !strcmp(st.why, "no PointSize output"), "E: no PointSize output");
        expect(!xbg_spirv_idvs_stripped(n.data(), n.size()), "E: stripped() false without PointSize");
        std::vector<uint32_t> junk{1, 2, 3}; expect(xbg_spirv_idvs_strip(junk.data(), junk.size(), out, &st) == 0, "E: not SPIR-V");
    }
    {   // F
        auto in = vs('F'); expect(xbg_spirv_idvs_strip(in.data(), in.size(), out, &st) == 2 && st.chains_dropped == 2, "F: both PointSize stores dropped");
        expect(!*xbg_spirv_idvs_check(in.data(), in.size(), out.data(), out.size()), "F: check accepts");
    }
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("idvs_test: all passed\n"); return 0;
}
