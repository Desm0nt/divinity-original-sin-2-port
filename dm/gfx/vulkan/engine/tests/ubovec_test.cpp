// [NR-27] The production DM_NR_UBOVEC rewrite (xbg_spirv_ubovec.cpp) on its own:
//  1. the integer facts (XbgUvFact) against plain 32-bit arithmetic on random values: every fact an operation returns must hold;
//  2. synthetic modules in the translator's LDC form (address = light * 48 + k through Function variables, word = addr >> 2,
//     vec4 = word >> 2, component = word & 3): what must merge merges, what must not (unknown alignment, a variable merged from two
//     branches, reads that do not dominate each other, a storage block, a lone component) stays, and xbg_spirv_ubovec_check accepts
//     the rewrite and rejects a tampered one.
// The values themselves are compared on the real pack by tools/dos2_ubovec_check.py (spv_interp); this test needs no data.
//   clang++ / g++ -O2 -std=c++17 tests/ubovec_test.cpp xbg_spirv_ubovec.cpp -o ubovec_test && ./ubovec_test
#include "../xbg_spirv_ubovec.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <random>
#include <vector>

namespace {
int failures = 0;
void expect(bool ok, const char *what) { if (!ok) { failures++; printf("FAIL: %s\n", what); } }

// ---------------------------------------------------------------------------------------------------------------- 1. facts
uint32_t eval(const XbgUvFact &f, uint32_t s) {
    if (f.constant()) return f.add;
    const uint32_t v = s * f.mul + f.add;
    return f.bits >= 32 ? v : v & ((1u << f.bits) - 1u);
}
void facts() {
    std::mt19937 rng(27);
    auto pick = [&]() -> uint32_t {
        switch (rng() % 6) {
        case 0: return rng() % 8;
        case 1: return 1u << (rng() % 32);
        case 2: return (1u << (rng() % 33 == 32 ? 31 : rng() % 32)) - 1u;
        case 3: return 48u * (rng() % 5);
        case 4: return 0u - (rng() % 64);
        default: return rng();
        }
    };
    size_t kept = 0, dropped = 0, bad = 0;
    for (int trial = 0; trial < 400000 && bad < 5; trial++) {
        uint32_t s = rng() % 4 ? rng() : pick(), v = s; XbgUvFact f = xbg_ubovec::opaque(1);
        for (int step = 0; step < 10; step++) {
            const uint32_t k = pick(); XbgUvFact r; bool ok = false; uint32_t nv = v; const int op = int(rng() % 8);
            switch (op) {
            case 0: nv = v + k; ok = xbg_ubovec::add(f, xbg_ubovec::constant(k), r); break;
            case 1: nv = k + v; ok = xbg_ubovec::add(xbg_ubovec::constant(k), f, r); break;
            case 2: nv = v - k; ok = xbg_ubovec::sub(f, xbg_ubovec::constant(k), r); break;
            case 3: nv = v * k; ok = xbg_ubovec::mul(f, xbg_ubovec::constant(k), r); break;
            case 4: { uint32_t sh = k % 32; nv = v << sh; ok = xbg_ubovec::shl(f, sh, r); break; }
            case 5: { uint32_t sh = k % 32; nv = v >> sh; ok = xbg_ubovec::shr(f, sh, false, r); break; }
            case 6: { uint32_t sh = k % 32; nv = uint32_t(int32_t(v) >> sh); ok = xbg_ubovec::shr(f, sh, true, r); break; }
            default: nv = v & k; ok = xbg_ubovec::band(f, k, r); break;
            }
            if (ok) {
                kept++;
                if (eval(r, s) != nv) { bad++; printf("FAIL: fact op %d k %#x: value %#x but fact gives %#x (sym %u mul %#x add %#x bits %u, s %#x)\n", op, k, nv, eval(r, s), r.sym, r.mul, r.add, r.bits, s); break; }
                f = r; v = nv;
            } else { dropped++; s = v = nv; f = xbg_ubovec::opaque(1); }   // the engine's fallback: the result is a new unknown
            if (step == 5) {   // two facts about one symbol: the sum and the difference
                XbgUvFact g, sum; const uint32_t m = pick();
                if (xbg_ubovec::mul(xbg_ubovec::opaque(1), xbg_ubovec::constant(m), g)) {
                    if (xbg_ubovec::add(f, g, sum) && eval(sum, s) != v + s * m) { bad++; printf("FAIL: fact + fact\n"); }
                    if (xbg_ubovec::sub(f, g, sum) && eval(sum, s) != v - s * m) { bad++; printf("FAIL: fact - fact\n"); }
                }
            }
        }
        // a + b of two different symbols: the claimed remainder modulo the claimed power of two
        XbgUvFact a = xbg_ubovec::opaque(1), b = xbg_ubovec::opaque(2), t, al; const uint32_t sa = rng(), sb = rng(), ma = pick(), mb = pick(), ka = pick(), kb = pick();
        if (xbg_ubovec::mul(a, xbg_ubovec::constant(ma), t) && xbg_ubovec::add(t, xbg_ubovec::constant(ka), a) && xbg_ubovec::mul(b, xbg_ubovec::constant(mb), t) && xbg_ubovec::add(t, xbg_ubovec::constant(kb), b)
            && !a.constant() && !b.constant() && xbg_ubovec::add_aligned(a, b, 77, al)) {
            const uint32_t sum = sa * ma + ka + sb * mb + kb;
            if (al.sym != (77u | 0x80000000u) || (al.mul & (al.mul - 1)) || al.mul < 2 || al.add >= al.mul || sum % al.mul != al.add) { bad++; printf("FAIL: aligned sum %#x mod %#x != %#x\n", sum, al.mul, al.add); }
            kept++;
        }
    }
    printf("facts: %zu results checked, %zu not expressible, %zu wrong\n", kept, dropped, bad);
    expect(!bad && kept > 100000, "integer facts hold");
}

// ---------------------------------------------------------------------------------------------------------------- 2. modules
using Module = std::vector<uint32_t>;
enum : uint32_t {
    tVoid = 1, tFn, tInt, tUint, tFloat, tV4, cLen, tArr, tBlock, tPtrBlock, vCbuf, tPtrUF, tPtrFnInt, tPtrInUint, vIn, tStore, tPtrStore, vSsbo, tBool,
    tPtrOutF, vOut, tPtrFnF, c0, c1, c2, c3, c4, c8, c16, c48, c20, fMain, FIRST
};
struct B {
    Module m; uint32_t next = FIRST;
    void op(uint32_t opcode, std::initializer_list<uint32_t> a) { m.push_back(uint32_t((a.size() + 1) << 16) | opcode); m.insert(m.end(), a); }
    uint32_t id() { return next++; }
    B() {
        m = {0x07230203, 0x00010300, 0, 0, 0};
        op(17, {1}); op(14, {0, 1}); op(15, {4, fMain, 0x6e69616d, 0, vIn, vOut}); op(16, {fMain, 7});
        op(71, {vIn, 30, 0}); op(71, {vIn, 14}); op(71, {vOut, 30, 0}); op(71, {tArr, 6, 16});
        op(72, {tBlock, 0, 35, 0}); op(71, {tBlock, 2}); op(71, {vCbuf, 34, 0}); op(71, {vCbuf, 33, 0});
        op(72, {tStore, 0, 35, 0}); op(71, {tStore, 3}); op(71, {vSsbo, 34, 0}); op(71, {vSsbo, 33, 1});
        op(19, {tVoid}); op(33, {tFn, tVoid}); op(21, {tInt, 32, 1}); op(21, {tUint, 32, 0}); op(22, {tFloat, 32}); op(23, {tV4, tFloat, 4}); op(20, {tBool});
        op(43, {tUint, cLen, 4096}); op(28, {tArr, tV4, cLen}); op(30, {tBlock, tArr}); op(32, {tPtrBlock, 2, tBlock}); op(59, {tPtrBlock, vCbuf, 2});
        op(30, {tStore, tArr}); op(32, {tPtrStore, 2, tStore}); op(59, {tPtrStore, vSsbo, 2});
        op(32, {tPtrUF, 2, tFloat}); op(32, {tPtrFnInt, 7, tInt}); op(32, {tPtrFnF, 7, tFloat}); op(32, {tPtrInUint, 1, tUint}); op(59, {tPtrInUint, vIn, 1});
        op(32, {tPtrOutF, 3, tFloat}); op(59, {tPtrOutF, vOut, 3});
        for (auto kv : {std::pair<uint32_t, uint32_t>{c0, 0}, {c1, 1}, {c2, 2}, {c3, 3}, {c4, 4}, {c8, 8}, {c16, 16}, {c48, 48}, {c20, 20}}) op(43, {tInt, kv.first, kv.second});
        op(54, {tVoid, fMain, 0, tFn});
    }
    // the translator's read of the 32-bit word at byte address (load of `reg`) + k: returns the float's id
    uint32_t ldc(uint32_t reg, uint32_t kconst, uint32_t base = vCbuf) {
        uint32_t a = id(), b = id(), w = id(), wu = id(), wi = id(), v = id(), wi2 = id(), c = id(), p = id(), x = id();
        op(61, {tInt, a, reg}); op(128, {tInt, b, a, kconst}); op(194, {tInt, w, b, c2}); op(124, {tUint, wu, w}); op(124, {tInt, wi, wu});
        op(194, {tInt, v, wi, c2}); op(124, {tInt, wi2, wu}); op(199, {tInt, c, wi2, c3});
        op(65, {tPtrUF, p, base, c0, v, c}); op(61, {tFloat, x, p}); return x;
    }
    Module done() { op(253, {}); op(56, {}); m[3] = next; return m; }
};
struct Result { size_t reads; XbgUbovecStats st; Module out; const char *check; };
Result run(const Module &m) {
    Result r; r.reads = xbg_spirv_ubovec(m.data(), m.size(), r.out, &r.st);
    r.check = r.reads ? xbg_spirv_ubovec_check(m.data(), m.size(), r.out.data(), r.out.size()) : "";
    return r;
}
// entry: reg = in * mul; then the reads
uint32_t head(B &b, uint32_t reg, uint32_t mul, uint32_t plus = 0) {
    uint32_t a = b.id(), i = b.id(), x = b.id();
    b.op(61, {tUint, a, vIn}); b.op(124, {tInt, i, a}); b.op(132, {tInt, x, i, mul});
    if (plus) { uint32_t y = b.id(); b.op(128, {tInt, y, x, plus}); x = y; }
    b.op(62, {reg, x}); return i;
}
void modules() {
    {   // A: a light loop; +0 +4 +8 of one vec4 merge, +16 (the next vec4, one component) stays
        B b; uint32_t L0 = b.id(), LH = b.id(), LB = b.id(), LC = b.id(), LM = b.id(), vN = b.id(), vA = b.id();
        b.op(248, {L0}); b.op(59, {tPtrFnInt, vN, 7}); b.op(59, {tPtrFnInt, vA, 7}); b.op(62, {vN, c0}); b.op(249, {LH});
        b.op(248, {LH}); b.op(246, {LM, LC, 0}); b.op(249, {LB});
        b.op(248, {LB}); uint32_t i = b.id(), a = b.id(); b.op(61, {tInt, i, vN}); b.op(132, {tInt, a, i, c48}); b.op(62, {vA, a});
        uint32_t x0 = b.ldc(vA, c0), x1 = b.ldc(vA, c4), x2 = b.ldc(vA, c8), x3 = b.ldc(vA, c16);
        uint32_t s0 = b.id(), s1 = b.id(), s2 = b.id(); b.op(129, {tFloat, s0, x0, x1}); b.op(129, {tFloat, s1, s0, x2}); b.op(129, {tFloat, s2, s1, x3}); b.op(62, {vOut, s2});
        uint32_t n = b.id(), c = b.id(); b.op(128, {tInt, n, i, c1}); b.op(62, {vN, n}); b.op(177, {tBool, c, n, c4}); b.op(249, {LC});
        b.op(248, {LC}); b.op(250, {c, LH, LM});
        b.op(248, {LM});
        Module m = b.done(); Result r = run(m);
        printf("A light loop: reads %zu vec4 loads %u candidates %u dynamic %u check '%s'\n", r.reads, r.st.vec_loads, r.st.candidates, r.st.dynamic, r.check);
        expect(r.reads == 3 && r.st.vec_loads == 1 && r.st.candidates == 4 && r.st.dynamic == 0 && !*r.check, "A: three words of one vec4 merge, the lone one stays");
        // poison passes its own check and fails the plain one; a tampered component and a vector moved after its use are rejected
        Module p; xbg_spirv_ubovec(m.data(), m.size(), p, nullptr, true);
        expect(!*xbg_spirv_ubovec_check(m.data(), m.size(), p.data(), p.size(), true) && *xbg_spirv_ubovec_check(m.data(), m.size(), p.data(), p.size(), false), "A: poison is seen by the check");
        Module t = r.out; bool hit = false;
        for (size_t k = 5; k < t.size(); k += t[k] >> 16) if ((t[k] & 0xffff) == 81 && (t[k] >> 16) == 5 && t[k + 4] == 2) { t[k + 4] = 3; hit = true; break; }
        expect(hit && *xbg_spirv_ubovec_check(m.data(), m.size(), t.data(), t.size()), "A: a wrong component is rejected");
        t = r.out; t.push_back(0x00010000);
        expect(*xbg_spirv_ubovec_check(m.data(), m.size(), t.data(), t.size()), "A: trailing words are rejected");
    }
    {   // B: address = in * 20: the low bits are unknown, nothing is proven
        B b; uint32_t L0 = b.id(), vA = b.id(); b.op(248, {L0}); b.op(59, {tPtrFnInt, vA, 7}); head(b, vA, c20);
        b.ldc(vA, c0); b.ldc(vA, c4); b.ldc(vA, c8);
        Result r = run(b.done());
        expect(r.reads == 0 && r.st.candidates == 0 && r.st.dynamic == 3, "B: unknown alignment is left alone");
    }
    {   // C: one component in each branch: neither read dominates the other; then with a read in front of the branch all three merge
        for (int front = 0; front < 2; front++) {
            B b; uint32_t L0 = b.id(), LT = b.id(), LF = b.id(), LM = b.id(), vA = b.id(), c = b.id();
            b.op(248, {L0}); b.op(59, {tPtrFnInt, vA, 7}); uint32_t i = head(b, vA, c48);
            if (front) b.ldc(vA, c0);
            b.op(177, {tBool, c, i, c4}); b.op(247, {LM, 0}); b.op(250, {c, LT, LF});
            b.op(248, {LT}); b.ldc(vA, c4); b.op(249, {LM});
            b.op(248, {LF}); b.ldc(vA, c8); b.op(249, {LM});
            b.op(248, {LM}); if (front) b.ldc(vA, c8);
            Result r = run(b.done());
            if (front) expect(r.reads == 4 && r.st.vec_loads == 1 && !*r.check, "C: reads dominated by the first one join it");
            else expect(r.reads == 0 && r.st.candidates == 2, "C: reads in sibling branches do not merge");
        }
    }
    {   // D: the address register is stored again in one branch: after the merge point it is a new unknown (alignment and value)
        B b; uint32_t L0 = b.id(), LT = b.id(), LM = b.id(), vA = b.id(), c = b.id();
        b.op(248, {L0}); b.op(59, {tPtrFnInt, vA, 7}); uint32_t i = head(b, vA, c48); b.ldc(vA, c0); b.ldc(vA, c4);
        b.op(177, {tBool, c, i, c4}); b.op(247, {LM, 0}); b.op(250, {c, LT, LM});
        b.op(248, {LT}); uint32_t y = b.id(), z = b.id(); b.op(61, {tInt, y, vA}); b.op(128, {tInt, z, y, c16}); b.op(62, {vA, z}); b.ldc(vA, c8); b.op(249, {LM});
        b.op(248, {LM}); b.ldc(vA, c8); b.ldc(vA, c4);
        Result r = run(b.done());
        printf("D merged register: reads %zu vec4 loads %u candidates %u dynamic %u check '%s'\n", r.reads, r.st.vec_loads, r.st.candidates, r.st.dynamic, r.check);
        // the two reads in front merge (1 load); the one in the branch is another vec4 (+16) alone; the two after the merge are unproven
        expect(r.reads == 2 && r.st.vec_loads == 1 && r.st.candidates == 3 && r.st.dynamic == 2 && !*r.check, "D: a register merged from two branches proves nothing");
    }
    {   // E: the same reads from a storage block (BufferBlock): its contents may change during the invocation
        B b; uint32_t L0 = b.id(), vA = b.id(); b.op(248, {L0}); b.op(59, {tPtrFnInt, vA, 7}); head(b, vA, c48);
        b.ldc(vA, c0, vSsbo); b.ldc(vA, c4, vSsbo); b.ldc(vA, c8, vSsbo);
        Result r = run(b.done());
        expect(r.reads == 0 && r.st.candidates == 0, "E: storage blocks are not touched");
    }
    {   // F: address = in * 48 + 8: +0 and +4 are components 2 and 3 of vec4 3*in, +8 is component 0 of the next one
        B b; uint32_t L0 = b.id(), vA = b.id(); b.op(248, {L0}); b.op(59, {tPtrFnInt, vA, 7}); head(b, vA, c48, c8);
        b.ldc(vA, c0); b.ldc(vA, c4); b.ldc(vA, c8);
        Result r = run(b.done());
        expect(r.reads == 2 && r.st.vec_loads == 1 && r.st.candidates == 3 && !*r.check, "F: a vec4 boundary inside a run of words is respected");
    }
    {   // G: fixed places (constant vec4 index) merge only when asked
        B b; uint32_t L0 = b.id(); b.op(248, {L0});
        for (uint32_t comp : {c0, c1, c2}) { uint32_t p = b.id(), x = b.id(); b.op(65, {tPtrUF, p, vCbuf, c0, c8, comp}); b.op(61, {tFloat, x, p}); }
        Module m = b.done(), o; XbgUbovecStats st;
        expect(xbg_spirv_ubovec(m.data(), m.size(), o, &st) == 0 && st.candidates == 3, "G: fixed places stay by default");
        expect(xbg_spirv_ubovec(m.data(), m.size(), o, &st, false, true) == 3 && st.vec_loads == 1 && !*xbg_spirv_ubovec_check(m.data(), m.size(), o.data(), o.size()), "G: fixed places merge with fixed_too");
    }
}
}

int main() {
    facts(); modules();
    printf(failures ? "ubovec_test: %d FAILED\n" : "ubovec_test: all passed\n", failures);
    return failures ? 1 : 0;
}
