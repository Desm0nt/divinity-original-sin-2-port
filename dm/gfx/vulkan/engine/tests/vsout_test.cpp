// [NR-32] The production DM_NR_XLQ trim (xbg_spirv_vsout.cpp) on synthetic modules in the translator's form (vec4 Location outputs
// written one component at a time through OpAccessChain, or whole):
//   A  the fragment shader reads loc2.x and all of loc3: loc1's stores and the variable go, loc2's whole-vector store and loc3 stay;
//   B  the same, loc1 declared (not read) by the fragment shader: its stores go, the variable stays on the interface;
//   C  an output read back (OpLoad of its chain) is pinned: nothing of it is touched;
//   D  a dynamic component index pins the output;
//   E  fragment masks: constant chains count per component, a dynamic index / Interpolate* / whole load count in full, an array input
//      per element, a Location-less input struct gives XBG_VSOUT_ALL;
//   F  xbg_spirv_vsout_check accepts every trim above and rejects a tampered one (a kept store removed, an arithmetic op removed,
//      a kept output taken off the interface).
// The real pack is covered by tests/vsout_scan.cpp + tools/dos2_xlq_check.py (spirv-val, interpreter, malioc); this test needs no data.
//   clang++ / g++ -O2 -std=c++17 tests/vsout_test.cpp xbg_spirv_vsout.cpp -o vsout_test && ./vsout_test
#include "../xbg_spirv_vsout.h"
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
    void entry(uint32_t model, uint32_t fn, std::initializer_list<uint32_t> iface) {   // name "main"
        std::vector<uint32_t> a{model, fn, 0x6e69616du, 0}; a.insert(a.end(), iface); w.push_back(uint32_t((a.size() + 1) << 16) | 15u); w.insert(w.end(), a.begin(), a.end()); }
};
// ids: 1 void, 2 fn type, 3 float, 4 vec4, 5 vec2, 6 int, 7..10 int consts 0..3, 11 ptr Output vec4, 12 ptr Output float, 13 ptr Output vec2,
// 14 ptr Input vec4, 15 ptr Input float, 16 float const 1.0, 17 vec4 const, 18 vec2 const, 19 ptr Input arr, 20 arr[2] of vec4, 21 const 2u (len)
void types(B &b) {
    b.op(19, {1}); b.op(33, {2, 1}); b.op(22, {3, 32}); b.op(23, {4, 3, 4}); b.op(23, {5, 3, 2}); b.op(21, {6, 32, 1});
    for (uint32_t k = 0; k < 4; k++) b.op(43, {6, 7 + k, k});
    b.op(32, {11, 3, 4}); b.op(32, {12, 3, 3}); b.op(32, {13, 3, 5}); b.op(32, {14, 1, 4}); b.op(32, {15, 1, 3});
    b.op(43, {3, 16, 0x3f800000u}); b.op(44, {4, 17, 16, 16, 16, 16}); b.op(44, {5, 18, 16, 16});
}
// Vertex shader: outputs 30 (loc1 vec4), 31 (loc2 vec2), 32 (loc3 vec4); C: loc3 read back; D: loc1 with a dynamic index (int input 33)
std::vector<uint32_t> vs(char variant) {
    B b; b.op(17, {1}); b.op(14, {0, 1});
    b.entry(0, 40, {30, 31, 32});
    b.op(71, {30, 30, 1}); b.op(71, {31, 30, 2}); b.op(71, {32, 30, 3}); b.op(5, {30, 0x00316f});
    types(b);
    b.op(32, {34, 1, 6}); if (variant == 'D') b.op(59, {34, 33, 1});
    b.op(59, {11, 30, 3}); b.op(59, {13, 31, 3}); b.op(59, {11, 32, 3});
    b.op(54, {1, 40, 0, 2}); b.op(248, {41});
    // loc1: four component stores (or a dynamic one)
    if (variant == 'D') { b.op(61, {6, 42, 33}); b.op(65, {12, 43, 30, 42}); b.op(62, {43, 16}); }
    else for (uint32_t k = 0; k < 4; k++) { b.op(65, {12, 50 + k, 30, 7 + k}); b.op(62, {50 + k, 16}); }
    b.op(62, {31, 18});                                                 // loc2: whole vec2
    for (uint32_t k = 0; k < 4; k++) { b.op(65, {12, 60 + k, 32, 7 + k}); b.op(62, {60 + k, 16}); }
    if (variant == 'C') { b.op(61, {3, 70, 62}); b.op(129, {3, 71, 70, 16}); b.op(62, {63, 71}); }   // loc3.z read back into loc3.w
    b.op(253, {}); b.op(56, {});
    return b.w;
}
// Fragment shader inputs: 80 loc2 vec4 (reads .x by a constant chain), 81 loc3 vec4 (whole load), 82 loc1 (declared only, variant 'b'),
// variant 'e': 83 loc5 vec4 read with a dynamic index, 84 loc6.. array of 2 vec4 read [1].y, 85 loc9 Interpolate on a chain .z
std::vector<uint32_t> fs(char variant) {
    B b; b.op(17, {1}); b.op(11, {90, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0}); b.op(14, {0, 1});
    if (variant == 'b') b.entry(4, 40, {80, 81, 82}); else if (variant == 'e') b.entry(4, 40, {80, 81, 83, 84, 85, 87}); else if (variant == 's') b.entry(4, 40, {86}); else b.entry(4, 40, {80, 81});
    b.op(16, {40, 7});
    b.op(71, {80, 30, 2}); b.op(71, {81, 30, 3}); if (variant == 'b') b.op(71, {82, 30, 1});
    if (variant == 'e') { b.op(71, {83, 30, 5}); b.op(71, {84, 30, 6}); b.op(71, {85, 30, 9}); b.op(71, {87, 30, 10}); b.op(71, {87, 14}); }
    types(b);
    b.op(21, {22, 32, 0}); b.op(43, {22, 21, 2}); b.op(28, {20, 4, 21}); b.op(32, {19, 1, 20}); b.op(30, {23, 3, 3}); b.op(32, {24, 1, 23});
    b.op(32, {25, 1, 6});
    b.op(59, {14, 80, 1}); b.op(59, {14, 81, 1}); if (variant == 'b') b.op(59, {14, 82, 1});
    if (variant == 'e') { b.op(59, {14, 83, 1}); b.op(59, {19, 84, 1}); b.op(59, {14, 85, 1}); b.op(59, {25, 87, 1}); }
    if (variant == 's') b.op(59, {24, 86, 1});
    b.op(54, {1, 40, 0, 2}); b.op(248, {41});
    if (variant != 's') { b.op(65, {15, 100, 80, 7}); b.op(61, {3, 101, 100}); b.op(61, {4, 102, 81}); }
    if (variant == 'e') {
        b.op(61, {6, 103, 87}); b.op(65, {15, 104, 83, 103}); b.op(61, {3, 105, 104});
        b.op(65, {15, 106, 84, 8, 8}); b.op(61, {3, 107, 106});
        b.op(65, {15, 108, 85, 9}); b.op(12, {3, 109, 90, 76, 108});   // InterpolateAtCentroid
    }
    b.op(253, {}); b.op(56, {});
    return b.w;
}
uint64_t bit(uint32_t loc, uint32_t c) { return uint64_t(1) << (4 * loc + c); }
uint64_t vec(uint32_t loc, uint32_t n = 4) { return uint64_t((1u << n) - 1) << (4 * loc); }
size_t count_op(const std::vector<uint32_t> &w, uint32_t op) { size_t n = 0; for (size_t i = 5; i < w.size();) { uint32_t c = w[i] >> 16; if (!c) break; if ((w[i] & 0xffff) == op) n++; i += c; } return n; }
bool has_iface(const std::vector<uint32_t> &w, uint32_t id) { for (size_t i = 5; i < w.size();) { uint32_t c = w[i] >> 16; if (!c) break; if ((w[i] & 0xffff) == 15) { for (size_t k = 5; k < c; k++) if (w[i + k] == id) return true; return false; } i += c; } return false; }
}

int main() {
    // E: fragment masks
    uint64_t decl = 0; const uint64_t ra = xbg_spirv_fs_input_mask(fs('a').data(), fs('a').size(), &decl);
    expect(ra == (bit(2, 0) | vec(3)), "E: reads loc2.x and all of loc3");
    expect(decl == (vec(2) | vec(3)), "E: declares loc2 and loc3");
    uint64_t declb = 0; const uint64_t rb = xbg_spirv_fs_input_mask(fs('b').data(), fs('b').size(), &declb);
    expect(rb == ra && declb == (vec(1) | vec(2) | vec(3)), "E: loc1 declared, not read");
    const uint64_t re = xbg_spirv_fs_input_mask(fs('e').data(), fs('e').size());
    expect(re == (bit(2, 0) | vec(3) | vec(5) | bit(7, 1) | bit(9, 2) | bit(10, 0)), "E: dynamic index -> whole loc5; array [1].y -> loc7.y; Interpolate chain -> loc9.z; flat int loc10");
    expect(xbg_spirv_fs_input_mask(fs('s').data(), fs('s').size()) == XBG_VSOUT_ALL, "E: a Location-less input struct disables the trim");
    // vertex write mask
    const std::vector<uint32_t> va = vs('A');
    expect(xbg_spirv_vs_output_mask(va.data(), va.size()) == (vec(1) | vec(2, 2) | vec(3)), "write mask: loc1 xyzw, loc2 xy, loc3 xyzw");
    // A
    std::vector<uint32_t> o; XbgVsoutStats st;
    size_t k = xbg_spirv_vsout_trim(va.data(), va.size(), ra, o, &st, decl);
    expect(k == 4 && st.stores_dropped == 4 && st.chains_dropped == 4 && st.vars_dropped == 1, "A: loc1's four stores, chains and variable go");
    expect(count_op(o, 62) == count_op(va, 62) - 4 && !has_iface(o, 30) && has_iface(o, 31) && has_iface(o, 32), "A: loc2 / loc3 stores and interface stay");
    expect(!*xbg_spirv_vsout_check(va.data(), va.size(), o.data(), o.size(), ra), "A: check accepts");
    // B: loc1 declared by the fragment shader -> its variable stays on the interface
    k = xbg_spirv_vsout_trim(va.data(), va.size(), rb, o, &st, declb);
    expect(k == 4 && st.vars_dropped == 0 && has_iface(o, 30), "B: stores go, the declared output stays");
    expect(!*xbg_spirv_vsout_check(va.data(), va.size(), o.data(), o.size(), rb), "B: check accepts");
    // keep = loc1.x only: loc2 whole store has no kept component -> goes; loc3 goes
    k = xbg_spirv_vsout_trim(va.data(), va.size(), bit(1, 0), o, &st, 0);
    expect(k == 3 + 1 + 4 && st.vars_dropped == 2 && has_iface(o, 30) && !has_iface(o, 31), "keep loc1.x: 3 + 1 + 4 stores, two variables");
    expect(!*xbg_spirv_vsout_check(va.data(), va.size(), o.data(), o.size(), bit(1, 0)), "keep loc1.x: check accepts");
    // C: loc3 read back -> pinned; loc1 still goes
    const std::vector<uint32_t> vc = vs('C');
    k = xbg_spirv_vsout_trim(vc.data(), vc.size(), bit(2, 0), o, &st, 0);
    expect(k == 4 && st.pinned == 1 && count_op(o, 62) == count_op(vc, 62) - 4, "C: loc1 goes, loc2 (x kept) stays, loc3 (read back) is pinned");
    expect(!*xbg_spirv_vsout_check(vc.data(), vc.size(), o.data(), o.size(), bit(2, 0)), "C: check accepts");
    expect(xbg_spirv_vs_output_mask(vc.data(), vc.size()) == (vec(1) | vec(2, 2) | vec(3)), "C: write mask counts the pinned output in full");
    // D: loc1 with a dynamic component index -> pinned
    const std::vector<uint32_t> vd = vs('D');
    k = xbg_spirv_vsout_trim(vd.data(), vd.size(), vec(3), o, &st, 0);
    expect(k == 1 && st.pinned == 1 && has_iface(o, 30) && !has_iface(o, 31), "D: dynamic-index loc1 pinned, loc2 goes");
    expect(!*xbg_spirv_vsout_check(vd.data(), vd.size(), o.data(), o.size(), vec(3)), "D: check accepts");
    // nothing to drop
    expect(!xbg_spirv_vsout_trim(va.data(), va.size(), XBG_VSOUT_ALL, o, &st) && o.empty(), "keep all: no change");
    expect(!xbg_spirv_vsout_trim(fs('a').data(), fs('a').size(), 0, o, &st), "fragment module: not a vertex trim");
    // F: tampering
    k = xbg_spirv_vsout_trim(va.data(), va.size(), ra, o, &st, decl);
    std::vector<uint32_t> t1 = va;   // remove a kept store (loc3.x): find "OpStore 60 16" and drop it
    for (size_t i = 5; i < t1.size();) { uint32_t c = t1[i] >> 16; if ((t1[i] & 0xffff) == 62 && t1[i + 1] == 60) { t1.erase(t1.begin() + i, t1.begin() + i + c); break; } i += c; }
    expect(*xbg_spirv_vsout_check(va.data(), va.size(), t1.data(), t1.size(), ra) != 0, "F: a removed kept store is refused");
    std::vector<uint32_t> t2 = o;    // remove an arithmetic-free but non-output instruction: the OpLabel... use the function's OpReturn
    for (size_t i = 5; i < t2.size();) { uint32_t c = t2[i] >> 16; if ((t2[i] & 0xffff) == 253) { t2.erase(t2.begin() + i, t2.begin() + i + c); break; } i += c; }
    expect(*xbg_spirv_vsout_check(va.data(), va.size(), t2.data(), t2.size(), ra) != 0, "F: another instruction removed is refused");
    k = xbg_spirv_vsout_trim(va.data(), va.size(), ra, o, &st, decl);
    std::vector<uint32_t> t3 = o;    // take loc3 (kept, 32) off the interface
    for (size_t i = 5; i < t3.size();) { uint32_t c = t3[i] >> 16; if ((t3[i] & 0xffff) == 15) { for (size_t q = 5; q < c; q++) if (t3[i + q] == 32) { t3.erase(t3.begin() + i + q); t3[i] -= 1u << 16; break; } break; } i += c; }
    expect(*xbg_spirv_vsout_check(va.data(), va.size(), t3.data(), t3.size(), ra) != 0, "F: a kept output off the interface is refused");
    expect(*xbg_spirv_vsout_check(va.data(), va.size(), o.data(), o.size(), bit(1, 0)) != 0, "F: a trim checked against a mask that keeps what it dropped is refused");
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("vsout_test: all passed\n");
    return 0;
}
