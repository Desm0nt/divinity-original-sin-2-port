// [DOS2][NR-27] DM_NR_UBOVEC: scalar uniform-buffer reads of one vec4 merged into one vec4 load.
//
// The translator turns every Maxwell LDC (one 32-bit word of a constant buffer) into its own scalar load:
//     %p = OpAccessChain %_ptr_Uniform_float %fp_c13 %int_0 %vec %comp      %x = OpLoad %float %p
// with %vec = (addr >> 2) >> 2 and %comp = (addr >> 2) & 3. A material's clustered-light loop reads 24 such words per light
// (addr = light * 48 + 0 / 4 / 8 ...). Turnip's NIR merges neighbouring loads itself (nir_opt_load_store_vectorize); the Mali
// compiler does not (Arm's offline compiler: 24-26 load/store cycles against 8.7-13.8 arithmetic on a Mali-G57 for the forward
// materials, docs/dos2/nr/NR-25.md), and it only removes loads that are word for word the same.
//
// This pass proves, per read, what the component is and which vec4 it is in, with exact facts about 32-bit integers
// (XbgUvFact: value == (S * mul + add) mod 2^bits), followed through the translator's Function variables as SSA construction
// would (dominator tree, values merged at the iterated dominance frontier of each variable's stores are new unknowns). The
// first read of a vec4 that dominates the others becomes one load of the vec4; every read becomes an OpCompositeExtract under
// its old result id. Nothing is hoisted, no index is recomputed (the leader's own index ids are reused), and a vec4 of which a
// leader's group reads one component only is left alone, so there is never a wider load without a merge.
// It needs none of DM_GFX_LDC_FOLD / FORWARD_TEMPS / LDC_CANON and works after any of them (it runs last in tidy_rewrite).
#include "xbg_spirv_ubovec.h"

#include <algorithm>
#include <map>
#include <string>

namespace xbg_ubovec {
namespace {
inline uint32_t mask_of(unsigned bits) { return bits >= 32 ? 0xffffffffu : ((1u << bits) - 1u); }
XbgUvFact norm(XbgUvFact f) {
    if (f.bits > 32) f.bits = 32;
    const uint32_t m = mask_of(f.bits);
    f.mul &= m; f.add &= m;
    if (!f.sym || !f.mul) { f.sym = 0; f.mul = 0; f.bits = 32; }
    return f;
}
}
XbgUvFact constant(uint32_t value) { XbgUvFact f; f.add = value; return f; }
XbgUvFact opaque(uint32_t sym) { XbgUvFact f; f.sym = sym; f.mul = 1; return f; }
bool add(const XbgUvFact &a, const XbgUvFact &b, XbgUvFact &r) {
    if (a.constant() && b.constant()) { r = constant(a.add + b.add); return true; }
    if (a.constant() || b.constant()) {
        const XbgUvFact &x = a.constant() ? b : a; const uint32_t k = a.constant() ? a.add : b.add;
        r = x;
        if (x.bits == 32) { r.add = x.add + k; r = norm(r); return true; }
        // value == q * g + (add mod g) with g the lowest set bit of mul, and value <= 2^bits - g + (add mod g): adding k with
        // (add mod g) + k < g carries into no bit at or above g, so the sum is still below 2^bits and equals the form with add + k.
        const uint32_t g = x.mul & (0u - x.mul), rem = x.add & (g - 1u);
        if (uint64_t(rem) + k >= g) return false;
        r.add = x.add + k; return true;
    }
    if (a.sym == b.sym && a.bits == 32 && b.bits == 32) { r = a; r.mul = a.mul + b.mul; r.add = a.add + b.add; r = norm(r); return true; }
    return false;
}
bool sub(const XbgUvFact &a, const XbgUvFact &b, XbgUvFact &r) {
    if (a.constant() && b.constant()) { r = constant(a.add - b.add); return true; }
    if (b.constant() && a.bits == 32) { r = a; r.add = a.add - b.add; r = norm(r); return true; }
    if (!a.constant() && a.sym == b.sym && a.bits == 32 && b.bits == 32) { r = a; r.mul = a.mul - b.mul; r.add = a.add - b.add; r = norm(r); return true; }
    return false;
}
bool mul(const XbgUvFact &a, const XbgUvFact &b, XbgUvFact &r) {
    if (a.constant() && b.constant()) { r = constant(a.add * b.add); return true; }
    if (!a.constant() && !b.constant()) return false;
    const XbgUvFact &x = a.constant() ? b : a; const uint32_t k = a.constant() ? a.add : b.add;
    if (x.bits != 32) return false;
    r = x; r.mul = x.mul * k; r.add = x.add * k; r = norm(r); return true;
}
bool shl(const XbgUvFact &a, uint32_t s, XbgUvFact &r) { return s < 32 && mul(a, constant(1u << s), r); }
bool shr(const XbgUvFact &a, uint32_t s, bool arithmetic, XbgUvFact &r) {
    if (s >= 32) return false;
    if (a.constant()) { r = constant(arithmetic ? uint32_t(int32_t(a.add) >> s) : a.add >> s); return true; }
    if (arithmetic && a.bits == 32) return false;      // below 32 known bits the sign bit is 0 and both shifts agree
    if (s >= a.bits) { r = constant(0); return true; }
    if (a.mul & mask_of(s)) return false;
    // (S * 2^s * m + 2^s * a' + rem) mod 2^bits, rem < 2^s: the low s bits are rem, the rest is (S * m + a') mod 2^(bits - s).
    r = a; r.mul = a.mul >> s; r.add = a.add >> s; r.bits = uint8_t(a.bits - s); r = norm(r); return true;
}
bool band(const XbgUvFact &a, uint32_t mask, XbgUvFact &r) {
    if (a.constant()) { r = constant(a.add & mask); return true; }
    if (mask & (mask + 1u)) return false;              // only masks 2^t - 1 (and 0, and all ones)
    unsigned t = 0; while (t < 32 && (mask >> t)) t++;
    if (t >= a.bits) { r = a; return true; }
    if (!t) { r = constant(0); return true; }
    r = a; r.bits = uint8_t(t); r = norm(r); return true;   // 2^t divides 2^bits: (x mod 2^bits) mod 2^t == x mod 2^t
}
bool add_aligned(const XbgUvFact &a, const XbgUvFact &b, uint32_t result, XbgUvFact &r) {
    if (a.constant() || b.constant() || a.bits != 32 || b.bits != 32) return false;
    const uint32_t ga = a.mul & (0u - a.mul), gb = b.mul & (0u - b.mul), g = ga < gb ? ga : gb;
    if (g < 2) return false;
    // the sum is a.add + b.add modulo g, so it is Q * g + rem with Q = (sum - rem) / g < 2^32 / g: no wrap in that form
    r.sym = result | 0x80000000u; r.mul = g; r.add = (a.add + b.add) & (g - 1u); r.bits = 32; return true;
}
}

namespace {
using xbg_ubovec::constant; using xbg_ubovec::opaque;
enum : uint32_t {
    OpTypeInt = 21, OpTypeVector = 23, OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32,
    OpConstant = 43, OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65,
    OpInBoundsAccessChain = 66, OpDecorate = 71, OpCompositeExtract = 81, OpCopyObject = 83, OpBitcast = 124, OpIAdd = 128, OpISub = 130,
    OpIMul = 132, OpShiftRightLogical = 194, OpShiftRightArithmetic = 195, OpShiftLeftLogical = 196, OpBitwiseAnd = 199, OpLabel = 248,
    OpBranch = 249, OpBranchConditional = 250, OpSwitch = 251, OpKill = 252, OpReturn = 253, OpReturnValue = 254, OpUnreachable = 255,
    OpTerminateInvocation = 4416,
    StorageUniform = 2, StorageFunction = 7, DecorationBufferBlock = 3,
};
inline uint32_t inst(uint32_t op, uint32_t words) { return (words << 16) | op; }
inline bool terminator(uint32_t op) { return (op >= OpBranch && op <= OpUnreachable) || op == OpTerminateInvocation; }

struct Block { size_t begin = 0, end = 0; uint32_t func = 0; int pre = -1, post = -1; };
struct Chain {
    size_t at = 0; uint32_t id = 0, op = 0, base = 0, vec_type = 0, comps = 0, comp = 0, loads = 0, rewritten = 0;
    bool constant_comp = false, computed = false;   // computed: some index is not a constant (an LDC with a computed address)
    std::vector<uint32_t> index;                    // every index id but the component
    std::vector<uint32_t> key;
};
struct Leader { size_t at = 0; uint32_t chain = 0, mask = 0; std::vector<size_t> members; };   // members: offsets of the OpLoads, the leader's first
struct Cur { XbgUvFact f; uint32_t rep = 0; uint8_t state = 0; };   // 0 nothing stored yet, 1 an integer with a fact, 2 anything else

struct Analysis {
    const uint32_t *in = nullptr; size_t n = 0; uint32_t bound = 0; size_t first_func = 0;
    std::vector<uint8_t> int32, has, buffer_block, global_uniform;
    std::vector<uint32_t> vec_comp, vec_n, ptr_sc, ptr_to, elem, var_type, uniform_ptr, rep, chain_of, chain_loads;
    std::vector<XbgUvFact> fact;
    std::map<uint32_t, std::vector<uint32_t>> members;
    std::vector<Block> blocks;                // every reachable or unreachable block, in binary order
    std::vector<Chain> chains; std::vector<Leader> leaders;
    std::map<size_t, uint32_t> load_leader;   // offset of a candidate OpLoad -> leader index
    XbgUbovecStats st; uint32_t fresh = 0x40000000u;   // unknowns that are not ids (merged variable values)
    bool ok = false;

    const Block *block_at(size_t off) const {
        size_t lo = 0, hi = blocks.size();
        while (lo < hi) { size_t mid = (lo + hi) / 2; if (blocks[mid].end <= off) lo = mid + 1; else hi = mid; }
        return lo < blocks.size() && blocks[lo].begin <= off ? &blocks[lo] : nullptr;
    }
    // the instruction at a dominates the one at b (a before b in one block, or a's block strictly dominates b's)
    bool dominates(size_t a, size_t b) const {
        const Block *x = block_at(a), *y = block_at(b);
        if (!x || !y || x->func != y->func || x->pre < 0 || y->pre < 0) return false;
        if (x == y) return a < b;
        return x->pre < y->pre && y->post < x->post;
    }
    bool globals();
    bool function(size_t begin, size_t end, uint32_t func);
    bool run(const uint32_t *words, size_t count);
};

bool Analysis::globals() {
    if (!in || n < 5 || in[0] != 0x07230203u) return false;
    bound = in[3];
    if (!bound || bound > (1u << 22)) return false;
    for (auto *v : {&int32, &has, &buffer_block, &global_uniform}) v->assign(bound, 0);
    for (auto *v : {&vec_comp, &vec_n, &ptr_sc, &ptr_to, &elem, &var_type, &uniform_ptr, &rep, &chain_of, &chain_loads}) v->assign(bound, 0);
    fact.assign(bound, XbgUvFact{});
    for (size_t i = 5; i < n;) {
        const uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
        if (!wc || i + wc > n) return false;
        if (op == OpFunction) { first_func = i; break; }
        const uint32_t a = wc > 1 ? in[i + 1] : 0, b = wc > 2 ? in[i + 2] : 0;
        switch (op) {
        case OpTypeInt: if (wc == 4 && a < bound && in[i + 2] == 32) int32[a] = 1; break;
        case OpTypeVector: if (wc == 4 && a < bound) { vec_comp[a] = b; vec_n[a] = in[i + 3]; } break;
        case OpTypeArray: case OpTypeRuntimeArray: if (wc >= 3 && a < bound) elem[a] = b; break;   // not matrices: a column of a row-major one is not contiguous
        case OpTypeStruct: if (a < bound) members[a].assign(in + i + 2, in + i + wc); break;
        case OpTypePointer: if (wc == 4 && a < bound) { ptr_sc[a] = b; ptr_to[a] = in[i + 3]; if (b == StorageUniform && in[i + 3] < bound) uniform_ptr[in[i + 3]] = a; } break;
        case OpConstant: if (wc == 4 && a < bound && b < bound && int32[a]) { has[b] = 1; fact[b] = constant(in[i + 3]); rep[b] = b; } break;
        case OpVariable: if (wc >= 4 && a < bound && b < bound) { var_type[b] = a; if (in[i + 3] == StorageUniform) global_uniform[b] = 1; } break;
        case OpDecorate: if (wc == 3 && a < bound && b == DecorationBufferBlock) buffer_block[a] = 1; break;
        default: break;
        }
        i += wc;
    }
    return first_func != 0;
}

bool Analysis::function(size_t fbegin, size_t fend, uint32_t func) {
    // Blocks and the control-flow graph.
    const size_t b0 = blocks.size();
    std::vector<uint32_t> label_block(bound, 0);
    for (size_t i = fbegin; i < fend;) {
        const uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
        if (op == OpLabel && wc == 2 && in[i + 1] < bound) { Block b; b.begin = i; b.func = func; blocks.push_back(b); label_block[in[i + 1]] = uint32_t(blocks.size() - b0); }
        if (terminator(op) && blocks.size() > b0) blocks.back().end = i + wc;
        i += wc;
    }
    const size_t nb = blocks.size() - b0;
    if (!nb) return true;
    for (size_t k = 0; k < nb; k++) if (!blocks[b0 + k].end) return false;
    std::vector<std::vector<uint32_t>> succ(nb), pred(nb);
    for (size_t k = 0; k < nb; k++) {
        size_t i = blocks[b0 + k].begin, last = i;
        while (i < blocks[b0 + k].end) { last = i; i += in[i] >> 16; }
        const uint32_t wc = in[last] >> 16, op = in[last] & 0xffff;
        auto edge = [&](uint32_t label) { if (label >= bound || !label_block[label]) return false; succ[k].push_back(label_block[label] - 1); return true; };
        if (op == OpBranch) { if (wc != 2 || !edge(in[last + 1])) return false; }
        else if (op == OpBranchConditional) { if (wc < 4 || !edge(in[last + 2]) || !edge(in[last + 3])) return false; }
        else if (op == OpSwitch) {
            // the selector must be a 32-bit integer (one literal word a case); its type is the type of some earlier result
            if (wc < 3 || (wc - 3) % 2 || !edge(in[last + 2])) return false;
            for (uint32_t w = 4; w < wc; w += 2) if (!edge(in[last + w])) return false;
        }
    }
    // Reverse post-order, immediate dominators (Cooper, Harvey, Kennedy), dominance frontiers.
    std::vector<int> order, num(nb, -1), idom(nb, -1);
    {
        std::vector<std::pair<uint32_t, size_t>> stack{{0, 0}}; std::vector<uint8_t> seen(nb, 0); seen[0] = 1;
        while (!stack.empty()) {
            auto &top = stack.back();
            if (top.second < succ[top.first].size()) { uint32_t s = succ[top.first][top.second++]; if (!seen[s]) { seen[s] = 1; stack.push_back({s, 0}); } }
            else { order.push_back(int(top.first)); stack.pop_back(); }
        }
        std::reverse(order.begin(), order.end());
        for (size_t k = 0; k < order.size(); k++) num[order[k]] = int(k);
    }
    for (size_t k = 0; k < nb; k++) if (num[k] >= 0) for (uint32_t s : succ[k]) pred[s].push_back(uint32_t(k));
    idom[0] = 0;
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t o = 1; o < order.size(); o++) {
            const int b = order[o]; int best = -1;
            for (uint32_t p : pred[b]) {
                if (idom[p] < 0) continue;
                if (best < 0) { best = int(p); continue; }
                int x = int(p), y = best;
                while (x != y) { while (num[x] > num[y]) x = idom[x]; while (num[y] > num[x]) y = idom[y]; }
                best = x;
            }
            if (best >= 0 && idom[b] != best) { idom[b] = best; changed = true; }
        }
    }
    std::vector<std::vector<uint32_t>> kids(nb), df(nb);
    for (size_t o = 1; o < order.size(); o++) kids[idom[order[o]]].push_back(uint32_t(order[o]));
    for (size_t o = 0; o < order.size(); o++) {
        const int b = order[o];
        if (pred[b].size() < 2) continue;
        for (uint32_t p : pred[b]) for (int r = int(p); r != idom[b]; r = idom[r]) { if (df[r].empty() || df[r].back() != uint32_t(b)) df[r].push_back(uint32_t(b)); if (r == idom[r]) break; }
    }
    // Function variables that are only ever loaded and stored whole; access chains whose every use is a whole OpLoad.
    std::vector<uint8_t> fvar(bound, 0), escaped(bound, 0), is_chain(bound, 0), chain_bad(bound, 0), local(bound, 0);
    std::vector<uint32_t> init(bound, 0), first_block(bound, 0);   // first_block: block + 1 of the first access, ~0u once accessed in two blocks
    for (size_t i = fbegin; i < fend;) {
        const uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
        if (op == OpVariable && wc >= 4 && in[i + 2] < bound && in[i + 3] == StorageFunction) { fvar[in[i + 2]] = 1; if (wc >= 5) init[in[i + 2]] = in[i + 4]; }
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && wc >= 5 && in[i + 2] < bound) is_chain[in[i + 2]] = 1;
        i += wc;
    }
    std::vector<std::vector<uint32_t>> store_blocks_of;   // per tracked variable that needs merges: the blocks that store to it
    std::vector<uint32_t> merge_index(bound, 0);
    {
        uint32_t blk = 0;
        for (size_t i = fbegin; i < fend;) {
            const uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
            if (op == OpLabel) blk = label_block[in[i + 1]];
            for (uint32_t w = 1; w < wc; w++) {
                const uint32_t id = in[i + w];
                if (id >= bound) continue;
                if (fvar[id]) {
                    const bool load = op == OpLoad && w == 3 && wc == 4, store = op == OpStore && w == 1 && wc == 3;
                    if (load || store) {
                        if (!first_block[id]) { first_block[id] = blk; local[id] = store; }
                        else if (first_block[id] != blk) { first_block[id] = ~0u; local[id] = 0; }
                    } else if (!(op == OpVariable && w == 2)) escaped[id] = 1;
                }
                if (is_chain[id]) {
                    if (op == OpLoad && w == 3 && wc == 4) chain_loads[id]++;
                    else if (!((op == OpAccessChain || op == OpInBoundsAccessChain) && w == 2)) chain_bad[id] = 1;
                }
            }
            i += wc;
        }
        blk = 0;
        for (size_t i = fbegin; i < fend;) {
            const uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
            if (op == OpLabel) blk = label_block[in[i + 1]];
            if (op == OpStore && wc == 3 && in[i + 1] < bound && fvar[in[i + 1]] && !escaped[in[i + 1]] && !local[in[i + 1]] && blk && num[blk - 1] >= 0) {
                uint32_t &m = merge_index[in[i + 1]];
                if (!m) { store_blocks_of.emplace_back(); m = uint32_t(store_blocks_of.size()); }
                auto &v = store_blocks_of[m - 1]; if (v.empty() || v.back() != blk - 1) v.push_back(blk - 1);
            }
            i += wc;
        }
    }
    // Where two different stored values of a variable can meet (iterated dominance frontier of its stores; the entry counts as
    // a store of "nothing yet"), the value is a new unknown.
    std::vector<std::vector<uint32_t>> merges(nb);
    {
        std::vector<uint32_t> stamp(nb, 0), work; uint32_t tick = 0;
        for (uint32_t v = 0; v < bound; v++) {
            if (!merge_index[v]) continue;
            ++tick; work = store_blocks_of[merge_index[v] - 1];
            while (!work.empty()) {
                const uint32_t b = work.back(); work.pop_back();
                for (uint32_t d : df[b]) if (stamp[d] != tick) { stamp[d] = tick; merges[d].push_back(v); work.push_back(d); }
            }
        }
    }
    // Walk the dominator tree: facts of every 32-bit integer, the value each tracked variable holds, the uniform reads.
    std::vector<Cur> cur(bound);
    for (uint32_t v = 0; v < bound; v++) if (fvar[v] && init[v] && init[v] < bound && has[init[v]]) { cur[v].f = fact[init[v]]; cur[v].rep = rep[init[v]]; cur[v].state = 1; }
    else if (fvar[v] && init[v]) cur[v].state = 2;
    std::vector<std::pair<uint32_t, Cur>> undo; std::vector<std::vector<uint32_t>> scope_keys;
    std::map<std::vector<uint32_t>, uint32_t> table;   // key -> leader index
    struct Frame { uint32_t block; size_t kid, undo_mark; };
    std::vector<Frame> stack; int clock = 0;
    auto set_cur = [&](uint32_t v, const Cur &c) { undo.push_back({v, cur[v]}); cur[v] = c; };
    auto view = [&](uint32_t id, XbgUvFact &f) { if (id >= bound || !has[id]) return false; f = fact[id]; return true; };
    auto give = [&](uint32_t r, const XbgUvFact &f, uint32_t r_rep) { has[r] = 1; fact[r] = f; rep[r] = r_rep; };
    auto enter = [&](uint32_t k) {
        Block &B = blocks[b0 + k]; B.pre = clock++;
        for (uint32_t v : merges[k]) { Cur c; c.rep = fresh; c.f = opaque(fresh++); c.state = 1; set_cur(v, c); }
        for (size_t i = B.begin; i < B.end;) {
            const uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
            if (op == OpStore && wc == 3 && in[i + 1] < bound && fvar[in[i + 1]] && !escaped[in[i + 1]]) {
                Cur c; const uint32_t x = in[i + 2];
                if (x < bound && has[x]) { c.f = fact[x]; c.rep = rep[x]; c.state = 1; } else c.state = 2;
                set_cur(in[i + 1], c);
            } else if (wc >= 3 && in[i + 1] < bound && in[i + 2] < bound && int32[in[i + 1]] && !has[in[i + 2]]) {
                const uint32_t r = in[i + 2], a = wc > 3 ? in[i + 3] : 0, b = wc > 4 ? in[i + 4] : 0;
                XbgUvFact fa, fb, res; bool done = false; uint32_t res_rep = r;
                if (op == OpLoad && wc == 4 && a < bound && fvar[a] && !escaped[a]) { if (cur[a].state == 1) { res = cur[a].f; res_rep = cur[a].rep; done = true; } }
                else if ((op == OpBitcast || op == OpCopyObject) && wc == 4) { if (view(a, fa)) { res = fa; res_rep = rep[a]; done = true; } }
                else if (wc == 5 && view(a, fa) && view(b, fb)) {
                    const XbgUvFact ta = opaque(rep[a]), tb = opaque(rep[b]);
                    auto two = [&](bool (*f)(const XbgUvFact &, const XbgUvFact &, XbgUvFact &)) { return f(fa, fb, res) || f(ta, fb, res) || f(fa, tb, res) || f(ta, tb, res); };
                    switch (op) {
                    case OpIAdd: done = two(xbg_ubovec::add) || xbg_ubovec::add_aligned(fa, fb, r, res) || xbg_ubovec::add_aligned(ta, fb, r, res) || xbg_ubovec::add_aligned(fa, tb, r, res); break;
                    case OpISub: done = two(xbg_ubovec::sub); break;
                    case OpIMul: done = two(xbg_ubovec::mul); break;
                    case OpShiftLeftLogical: done = fb.constant() && (xbg_ubovec::shl(fa, fb.add, res) || xbg_ubovec::shl(ta, fb.add, res)); break;
                    case OpShiftRightLogical: case OpShiftRightArithmetic:
                        done = fb.constant() && (xbg_ubovec::shr(fa, fb.add, op == OpShiftRightArithmetic, res) || xbg_ubovec::shr(ta, fb.add, op == OpShiftRightArithmetic, res)); break;
                    case OpBitwiseAnd:
                        if (fb.constant()) done = xbg_ubovec::band(fa, fb.add, res) || xbg_ubovec::band(ta, fb.add, res);
                        else if (fa.constant()) done = xbg_ubovec::band(fb, fa.add, res) || xbg_ubovec::band(tb, fa.add, res);
                        break;
                    default: break;
                    }
                }
                if (done) give(r, res, res_rep); else give(r, opaque(r), r);
            }
            if ((op == OpAccessChain || op == OpInBoundsAccessChain) && wc >= 5 && in[i + 2] < bound && !chain_bad[in[i + 2]] && in[i + 1] < bound && in[i + 3] < bound
                && ptr_sc[in[i + 1]] == StorageUniform && global_uniform[in[i + 3]] && var_type[in[i + 3]] < bound) {
                // type walk: every index but the last selects down to a vector, the last one a component of it
                uint32_t t = ptr_to[var_type[in[i + 3]]]; bool good = t < bound && !buffer_block[t] && members.count(t);
                Chain c; c.at = i; c.id = in[i + 2]; c.op = op; c.base = in[i + 3];
                c.key = {c.base, op};
                for (uint32_t w = 4; good && w + 1 < wc; w++) {
                    const uint32_t ix = in[i + w]; XbgUvFact f;
                    if (!view(ix, f)) { good = false; break; }
                    auto m = members.find(t);
                    if (m != members.end()) { if (!f.constant() || f.add >= m->second.size()) { good = false; break; } t = m->second[f.add]; }
                    else if (t < bound && elem[t] && !vec_n[t]) t = elem[t];
                    else { good = false; break; }
                    if (t >= bound) { good = false; break; }
                    c.index.push_back(ix); c.key.insert(c.key.end(), {f.sym, f.mul, f.add, uint32_t(f.bits)}); c.computed = c.computed || !f.constant();
                }
                XbgUvFact last;
                if (good && t < bound && vec_n[t] >= 2 && vec_n[t] <= 4 && vec_comp[t] == ptr_to[in[i + 1]] && view(in[i + wc - 1], last)) {
                    c.vec_type = t; c.comps = vec_n[t]; c.key.push_back(t);
                    c.constant_comp = last.constant() && last.add < c.comps; c.comp = last.add;
                    chains.push_back(std::move(c)); chain_of[in[i + 2]] = uint32_t(chains.size());
                }
            }
            if (op == OpLoad && wc == 4 && in[i + 3] < bound && chain_of[in[i + 3]]) {
                const uint32_t ci = chain_of[in[i + 3]] - 1; const Chain &c = chains[ci];
                if (!c.constant_comp) st.dynamic++;
                else {
                    st.candidates++;
                    auto it = table.find(c.key); uint32_t li;
                    if (it == table.end()) { li = uint32_t(leaders.size()); Leader L; L.at = i; L.chain = ci; leaders.push_back(L); table[c.key] = li; scope_keys.back().push_back(li); }
                    else li = it->second;
                    leaders[li].mask |= 1u << c.comp; leaders[li].members.push_back(i); load_leader[i] = li;
                }
            }
            i += wc;
        }
    };
    scope_keys.emplace_back();
    stack.push_back({0, 0, undo.size()}); enter(0);
    while (!stack.empty()) {
        Frame &f = stack.back();
        if (f.kid < kids[f.block].size()) {
            const uint32_t k = kids[f.block][f.kid++];
            scope_keys.emplace_back(); stack.push_back({k, 0, undo.size()}); enter(k);
        } else {
            blocks[b0 + f.block].post = clock++;
            while (undo.size() > f.undo_mark) { cur[undo.back().first] = undo.back().second; undo.pop_back(); }
            for (uint32_t li : scope_keys.back()) table.erase(chains[leaders[li].chain].key);
            scope_keys.pop_back(); stack.pop_back();
        }
    }
    return true;
}

bool Analysis::run(const uint32_t *words, size_t count) {
    in = words; n = count;
    if (!globals()) return false;
    uint32_t func = 0; size_t begin = 0;
    for (size_t i = first_func; i < n;) {
        const uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
        if (!wc || i + wc > n) return false;
        if (op == OpFunction) begin = i;
        else if (op == OpFunctionEnd) { if (!begin || func >= 512 || !function(begin, i, func++)) return false; begin = 0; }
        i += wc;
    }
    return ok = true;
}
}

size_t xbg_spirv_ubovec(const uint32_t *in, size_t n, std::vector<uint32_t> &out, XbgUbovecStats *stats, bool poison, bool fixed_too) {
    out.clear();
    if (stats) *stats = XbgUbovecStats{};
    Analysis A;
    if (!A.run(in, n)) return 0;
    uint32_t bound = A.bound;
    // Leaders whose group reads at least two components of the vector.
    enum { DROP = 1, LEAD = 2, MEMBER = 3 };
    struct Edit { size_t at; int kind; uint32_t leader; };
    std::vector<Edit> edits; std::vector<uint32_t> vec_id(A.leaders.size(), 0), ptr_id(A.leaders.size(), 0), decl; std::map<uint32_t, uint32_t> ptr_type;
    for (size_t li = 0; li < A.leaders.size(); li++) {
        const Leader &L = A.leaders[li];
        const Chain &c = A.chains[L.chain];
        if ((L.mask & (L.mask - 1)) == 0 || (!c.computed && !fixed_too)) continue;
        if (!ptr_type.count(c.vec_type)) {
            uint32_t p = A.uniform_ptr[c.vec_type];
            if (!p) { p = bound++; decl.insert(decl.end(), {inst(OpTypePointer, 4), p, StorageUniform, c.vec_type}); }
            ptr_type[c.vec_type] = p;
        }
        ptr_id[li] = bound++; vec_id[li] = bound++;
        for (size_t m = 0; m < L.members.size(); m++) {
            edits.push_back({L.members[m], m ? MEMBER : LEAD, uint32_t(li)});
            A.chains[A.chain_of[in[L.members[m] + 3]] - 1].rewritten++;
        }
        A.st.vec_loads++; A.st.reads += uint32_t(L.members.size());
    }
    if (stats) *stats = A.st;
    if (edits.empty()) return 0;
    for (const Chain &c : A.chains) if (c.rewritten && c.rewritten == A.chain_loads[c.id]) { edits.push_back({c.at, DROP, 0}); if (stats) stats->chains_dropped++; }
    std::sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) { return a.at < b.at; });
    out.reserve(n + decl.size() + edits.size() * 8);
    out.insert(out.end(), in, in + A.first_func);
    out[3] = bound;
    out.insert(out.end(), decl.begin(), decl.end());
    size_t e = 0;
    for (size_t i = A.first_func; i < n;) {
        const uint32_t wc = in[i] >> 16;
        if (e < edits.size() && edits[e].at == i) {
            const Edit &E = edits[e++];
            if (E.kind != DROP) {
                const Leader &L = A.leaders[E.leader]; const Chain &lead = A.chains[L.chain], &mine = A.chains[A.chain_of[in[i + 3]] - 1];
                if (E.kind == LEAD) {
                    out.push_back(inst(lead.op, uint32_t(4 + lead.index.size()))); out.push_back(ptr_type[lead.vec_type]); out.push_back(ptr_id[E.leader]); out.push_back(lead.base);
                    out.insert(out.end(), lead.index.begin(), lead.index.end());
                    out.insert(out.end(), {inst(OpLoad, 4), lead.vec_type, vec_id[E.leader], ptr_id[E.leader]});
                }
                out.insert(out.end(), {inst(OpCompositeExtract, 5), in[i + 1], in[i + 2], vec_id[E.leader], poison ? (mine.comp + 1) % mine.comps : mine.comp});
            }
        } else out.insert(out.end(), in + i, in + i + wc);
        i += wc;
    }
    return A.st.reads;
}

const char *xbg_spirv_ubovec_check(const uint32_t *in, size_t n, const uint32_t *out, size_t m, bool poison) {
    Analysis A;
    if (!A.run(in, n)) return "the original module could not be analysed";
    if (!out || m < 5 || out[0] != in[0] || out[1] != in[1] || out[2] != in[2] || out[3] < in[3] || out[4] != in[4]) return "header";
    const uint32_t bound = A.bound;
    // globals: the same words, then only new Uniform pointer types to a vector
    size_t j = 5;
    for (size_t i = 5; i < A.first_func; i++, j++) if (j >= m || out[j] != in[i]) return "globals differ";
    std::map<uint32_t, uint32_t> new_ptr;   // pointer type id -> vector type
    while (j < m && (out[j] & 0xffff) == OpTypePointer) {
        if ((out[j] >> 16) != 4 || j + 4 > m || out[j + 1] < bound || out[j + 1] >= out[3] || out[j + 2] != StorageUniform || out[j + 3] >= bound || !A.vec_n[out[j + 3]] || new_ptr.count(out[j + 1])) return "new pointer type";
        new_ptr[out[j + 1]] = out[j + 3]; j += 4;
    }
    struct Vec { size_t at; uint32_t base, op; std::vector<uint32_t> index; uint32_t type; };
    std::map<uint32_t, Vec> vecs;            // new vector value id -> the load it is (at = offset in `in` of the read it was emitted before)
    std::vector<uint32_t> dropped; std::map<uint32_t, uint32_t> left;   // chain id -> loads still in out
    std::vector<uint8_t> new_seen(out[3] - bound, 0);
    auto fresh = [&](uint32_t id) { if (id < bound || id >= out[3] || new_seen[id - bound]) return false; new_seen[id - bound] = 1; return true; };
    for (size_t i = A.first_func; i < n;) {
        const uint32_t wc = in[i] >> 16, op = in[i] & 0xffff;
        if (j < m && (out[j] >> 16) == wc && j + wc <= m && std::equal(in + i, in + i + wc, out + j)) {
            if (op == OpLoad && wc == 4 && in[i + 3] < bound && A.chain_of[in[i + 3]]) left[in[i + 3]]++;
            i += wc; j += wc; continue;
        }
        if ((op == OpAccessChain || op == OpInBoundsAccessChain) && in[i + 2] < bound && A.chain_of[in[i + 2]]) { dropped.push_back(in[i + 2]); i += wc; continue; }
        if (op != OpLoad || wc != 4 || in[i + 3] >= bound || !A.chain_of[in[i + 3]]) return "an instruction other than a uniform component load changed";
        const Chain &c = A.chains[A.chain_of[in[i + 3]] - 1];
        if (!c.constant_comp) return "a load with an unproven component was rewritten";
        if (j < m && (out[j] & 0xffff) == c.op) {     // a new vector access chain + load in front
            const uint32_t cw = out[j] >> 16;
            if (cw < 4 || j + cw + 4 > m || (!new_ptr.count(out[j + 1]) && A.uniform_ptr[c.vec_type] != out[j + 1])) return "new access chain";
            const uint32_t vt = new_ptr.count(out[j + 1]) ? new_ptr[out[j + 1]] : A.ptr_to[out[j + 1]];
            if (!fresh(out[j + 2]) || out[j + cw] != inst(OpLoad, 4) || out[j + cw + 1] != vt || !fresh(out[j + cw + 2]) || out[j + cw + 3] != out[j + 2]) return "new vector load";
            Vec v; v.at = i; v.base = out[j + 3]; v.op = c.op; v.index.assign(out + j + 4, out + j + cw); v.type = vt;
            for (uint32_t ix : v.index) if (ix >= bound || !A.has[ix]) return "new access chain index";
            vecs[out[j + cw + 2]] = v; j += cw + 4;
        }
        if (j + 5 > m || out[j] != inst(OpCompositeExtract, 5) || out[j + 1] != in[i + 1] || out[j + 2] != in[i + 2]) return "a rewritten load is not an extract under its old id";
        auto it = vecs.find(out[j + 3]);
        if (it == vecs.end()) return "extract from an unknown vector";
        const Vec &v = it->second;
        if (v.base != c.base || v.type != c.vec_type || v.op != c.op || v.index.size() != c.index.size()) return "extract from a vector of another block or shape";
        for (size_t k = 0; k < v.index.size(); k++) if (!(A.fact[v.index[k]] == A.fact[c.index[k]])) return "vector index not proven equal";
        if (out[j + 4] != (poison ? (c.comp + 1) % c.comps : c.comp)) return "wrong component";
        if (v.at != i && !A.dominates(v.at, i)) return "the vector load does not dominate the read";
        // the vector's own index ids must be defined where it was emitted: they are operands of the chain of the read at v.at
        if (v.at == i) for (size_t k = 0; k < v.index.size(); k++) if (v.index[k] != c.index[k]) return "the vector load uses indexes that are not the leader's";
        i += wc; j += 5;
    }
    if (j != m) return "trailing words";
    for (uint32_t id : dropped) if (left.count(id)) return "a dropped access chain is still loaded";
    return "";
}
