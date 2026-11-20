// [DOS2] NR-POSONLY: see xbg_spirv_posonly.h. Pure C++, no Vulkan (host test tests/nr_posonly_test.cpp runs it on the pack against
// tools/dos2_posonly_check.py's interpreter verdicts).
#include "xbg_spirv_posonly.h"
#include <unordered_map>
#include <vector>

namespace {
enum : uint32_t {
    OpExtInst = 12, OpFunction = 54, OpFunctionCall = 57, OpVariable = 59, OpLoad = 61, OpStore = 62, OpCopyMemory = 63, OpCopyMemorySized = 64,
    OpAccessChain = 65, OpInBoundsAccessChain = 66, OpPtrAccessChain = 67, OpDecorate = 71, OpVectorShuffle = 79, OpCompositeExtract = 81,
    OpCompositeInsert = 82, OpImageWrite = 99, OpBranchConditional = 250, OpSwitch = 251,
    DecBuiltIn = 11, DecLocation = 30, ScInput = 1, ScOutput = 3, ScPrivate = 6, ScFunction = 7,
};
// instructions without a result id worth tagging (debug, annotations, modes, control flow, types / constants are skipped by opcode range)
bool no_result(uint32_t op) {
    switch (op) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 10: case 11: case 14: case 15: case 16: case 17:
    case 71: case 72: case 73: case 74: case 75: case 246: case 247: case 248: case 249: case 252: case 253: case 254: case 255:
        return true;
    default: return false;
    }
}
bool type_or_const(uint32_t op) { return (op >= 19 && op <= 39) || (op >= 41 && op <= 52); }
bool atomic(uint32_t op) { return (op >= 227 && op <= 242) || op == 4407; }
}

bool xbg_spirv_posonly(const uint32_t *w, size_t n, uint32_t *dep_out, const char **why) {
    const char *dummy; if (!why) why = &dummy; *why = "module"; if (dep_out) *dep_out = 0;
    if (!w || n < 5 || w[0] != 0x07230203u) return false;
    struct Inst { uint32_t op; const uint32_t *a; uint32_t na; };
    std::vector<Inst> insts; std::unordered_map<uint32_t, uint32_t> loc, sc;
    unsigned nfunc = 0;
    for (size_t i = 5; i < n;) {
        const uint32_t len = w[i] >> 16, op = w[i] & 0xffffu;
        if (!len || i + len > n) { *why = "malformed"; return false; }
        const uint32_t *a = w + i + 1; const uint32_t na = len - 1;
        if (op == OpDecorate && na >= 3 && a[1] == DecLocation) loc[a[0]] = a[2];
        if (op == OpFunction) ++nfunc;
        if (op == OpVariable && na >= 3) sc[a[1]] = a[2];
        insts.push_back({op, a, na}); i += len;
    }
    if (nfunc != 1) { *why = "not one function"; return false; }
    std::unordered_map<uint32_t, uint32_t> dep, root;
    for (auto &v : sc) { root[v.first] = v.first; if (v.second == ScInput) { auto l = loc.find(v.first); if (l != loc.end() && l->second < 32) dep[v.first] = 1u << l->second; } }
    auto D = [&](uint32_t id) -> uint32_t { auto f = dep.find(id); return f == dep.end() ? 0u : f->second; };
    auto R = [&](uint32_t id) -> uint32_t { auto f = root.find(id); return f == root.end() ? id : f->second; };
    uint32_t ctrl = 0;
    for (int round = 0;; ++round) {
        if (round >= 64) { *why = "no fixpoint"; return false; }
        bool changed = false;
        auto set = [&](uint32_t id, uint32_t d) { uint32_t &x = dep[id]; if ((x | d) != x) { x |= d; changed = true; } };
        for (const Inst &in : insts) {
            const uint32_t op = in.op, *a = in.a, na = in.na;
            if (op == OpImageWrite || atomic(op)) { *why = "writes memory"; return false; }
            if (op == OpFunctionCall) { *why = "call"; return false; }
            if (op == OpCopyMemory || op == OpCopyMemorySized) { *why = "copy memory"; return false; }
            if (op == OpAccessChain || op == OpInBoundsAccessChain || op == OpPtrAccessChain) {
                if (na < 3) continue;
                const uint32_t rid = a[1], r0 = R(a[2]);
                if (R(rid) != r0) { root[rid] = r0; changed = true; }
                uint32_t d = D(r0); for (uint32_t k = 3; k < na; k++) d |= D(a[k]);
                set(rid, d); continue;
            }
            if (op == OpLoad) { if (na < 3) continue; set(a[1], D(R(a[2])) | D(a[2])); continue; }
            if (op == OpStore) {
                if (na < 2) continue;
                const uint32_t r0 = R(a[0]); auto s = sc.find(r0);
                if (s == sc.end() || (s->second != ScOutput && s->second != ScPrivate && s->second != ScFunction)) { *why = "store to another storage class"; return false; }
                set(r0, D(a[1]) | D(a[0])); continue;
            }
            if (op == OpBranchConditional || op == OpSwitch) { if (na >= 1 && (ctrl | D(a[0])) != ctrl) { ctrl |= D(a[0]); changed = true; } continue; }
            if (no_result(op) || type_or_const(op) || op == OpVariable || na < 2) continue;
            uint32_t lo = 2, hi = na;
            if (op == OpVectorShuffle) hi = na < 4 ? na : 4;
            else if (op == OpCompositeExtract) hi = na < 3 ? na : 3;
            else if (op == OpCompositeInsert) hi = na < 4 ? na : 4;
            else if (op == OpExtInst) lo = 4;
            uint32_t d = 0; for (uint32_t k = lo; k < hi; k++) d |= D(a[k]);
            set(a[1], d);
        }
        if (!changed) break;
    }
    uint32_t out = 0; bool have = false;
    for (auto &v : sc) {
        if (v.second != ScOutput || loc.count(v.first)) continue;   // Location outputs: no fragment shader reads them in these draws
        out |= D(v.first) | ctrl; have = true;
    }
    if (dep_out) *dep_out = out;
    if (!have) { *why = "no built-in output"; return false; }
    if (out & ~1u) { *why = "gl_Position depends on other inputs"; return false; }
    *why = ""; return true;
}
