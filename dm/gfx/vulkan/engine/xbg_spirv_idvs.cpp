// [DOS2] T2 DM_NR_IDVS (docs/dos2/nr/NR-IDVS.md): drop the translator's gl_PointSize store from vertex modules.
// No Vulkan, no engine state: host-testable (tests/idvs_test.cpp, tests/idvs_scan.cpp).
#include "xbg_spirv_idvs.h"
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace {
enum : uint32_t {
    OpName = 5, OpMemberName = 6, OpEntryPoint = 15, OpTypeInt = 21, OpTypePointer = 32, OpConstant = 43, OpFunction = 54,
    OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71, OpMemberDecorate = 72,
    OpDecorateId = 332, OpDecorateString = 5632, OpMemberDecorateString = 5633,
    DecBuiltIn = 11, BuiltInPointSize = 1, SCOutput = 3, ModelVertex = 0,
};
struct Ins { uint32_t op, n; size_t at; };

bool parse(const uint32_t *w, size_t count, std::vector<Ins> &ins) {
    if (!w || count < 5 || w[0] != 0x07230203u) return false;
    for (size_t i = 5; i < count;) {
        const uint32_t n = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (!n || i + n > count) return false;
        ins.push_back({op, n, i}); i += n;
    }
    return true;
}
// word k of an instruction (k = 0 is the opcode word) is a literal, never an id (as xbg_spirv_vsout.cpp literal_word)
bool literal_word(uint32_t op, size_t k) {
    switch (op) {
    case 3: return k != 3;
    case 8: case 16: return k >= 2;
    case 10: case 14: case 17: return true;
    case 11: return k >= 2;
    case 12: return k == 4;
    case 61: return k >= 4;                         // OpLoad memory operands
    case 62: case 63: return k >= 3;                // OpStore / OpCopyMemory memory operands
    case 79: return k >= 5;
    case 81: return k >= 4;
    case 82: return k >= 5;
    case 246: return k >= 3;
    case 247: return k == 2;
    case 250: return k >= 4;
    case 251: return k >= 3 && !((k - 3) & 1);
    default: return false;
    }
}
// declarations that may name a variable without using it; types / constants cannot reference variables (their literals are skipped)
bool declaration(uint32_t op) {
    return op == OpName || op == OpMemberName || op == OpEntryPoint || op == OpDecorate || op == OpMemberDecorate || op == OpDecorateId ||
           op == OpDecorateString || op == OpMemberDecorateString || op == OpVariable || (op >= 19 && op <= 52) || op == 1 || op == 3 ||
           op == 4 || op == 7 || op == 8 || op == 10 || op == 11 || op == 14 || op == 16 || op == 17;
}

// The PointSize pointers of a module: P = variables decorated PointSize + access chains selecting the PointSize member of an Output
// block. why != "" = the module cannot be handled (not a vertex module, PointSize reached in a way the strip does not handle).
struct Ps {
    std::unordered_set<uint32_t> ptr;      // P
    std::unordered_set<uint32_t> chains;   // the access chains in P
    std::vector<size_t> stores;            // instruction indexes of OpStore into P
    bool declared = false;                 // the module declares a PointSize output
    const char *why = "";
};
Ps analyse(const uint32_t *w, const std::vector<Ins> &ins) {
    Ps r; uint32_t entries = 0, model = ~0u;
    std::unordered_set<uint32_t> ints, ps_var; std::unordered_map<uint32_t, uint32_t> cint, ps_member, ptr_pointee, var_type; std::unordered_set<uint32_t> outputs;
    for (const Ins &x : ins) {
        const uint32_t *a = w + x.at;
        if (x.op == OpEntryPoint && x.n >= 3) { ++entries; model = a[1]; }
        else if (x.op == OpTypeInt && x.n >= 3) ints.insert(a[1]);
        else if (x.op == OpConstant && x.n == 4 && ints.count(a[1])) cint[a[2]] = a[3];
        else if (x.op == OpDecorate && x.n >= 4 && a[2] == DecBuiltIn && a[3] == BuiltInPointSize) ps_var.insert(a[1]);
        else if (x.op == OpMemberDecorate && x.n >= 5 && a[3] == DecBuiltIn && a[4] == BuiltInPointSize) ps_member[a[1]] = a[2];
        else if (x.op == OpTypePointer && x.n >= 4) ptr_pointee[a[1]] = a[3];
        else if (x.op == OpVariable && x.n >= 4 && a[3] == SCOutput) { outputs.insert(a[2]); var_type[a[2]] = a[1]; }
    }
    if (entries != 1 || model != ModelVertex) { r.why = "not a single-entry vertex module"; return r; }
    std::unordered_map<uint32_t, uint32_t> block;   // Output block variable -> its PointSize member index
    for (uint32_t v : outputs) {
        if (ps_var.count(v)) { r.ptr.insert(v); r.declared = true; continue; }
        auto p = ptr_pointee.find(var_type[v]); if (p == ptr_pointee.end()) continue;
        auto m = ps_member.find(p->second); if (m != ps_member.end()) { block[v] = m->second; r.declared = true; }
    }
    if (!r.declared) { r.why = "no PointSize output"; return r; }
    for (const Ins &x : ins) {
        if ((x.op != OpAccessChain && x.op != OpInBoundsAccessChain) || x.n < 4) continue;
        const uint32_t *a = w + x.at; const uint32_t base = a[3];
        if (r.ptr.count(base)) { r.why = "an access chain into PointSize"; return r; }
        auto b = block.find(base); if (b == block.end()) continue;
        if (x.n == 4) { r.why = "an access chain to the whole output block"; return r; }
        auto c = cint.find(a[4]); if (c == cint.end()) { r.why = "a non-constant output block member index"; return r; }
        if (c->second != b->second) continue;   // another member (gl_Position, ClipDistance ...)
        if (x.n != 5) { r.why = "an access chain past PointSize"; return r; }
        r.ptr.insert(a[2]); r.chains.insert(a[2]);
    }
    // every other mention of a PointSize pointer or of an output block variable (outside declarations and the chains above) refuses
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins &x = ins[i]; const uint32_t *a = w + x.at;
        if (declaration(x.op)) continue;
        if (x.op == OpStore && x.n >= 3 && r.ptr.count(a[1])) {
            if (r.ptr.count(a[2]) || block.count(a[2])) { r.why = "PointSize stored as a value"; return r; }
            r.stores.push_back(i); continue;
        }
        const bool chain = (x.op == OpAccessChain || x.op == OpInBoundsAccessChain) && x.n >= 4;
        for (size_t k = 1; k < x.n; k++) {
            if (literal_word(x.op, k)) continue;
            const uint32_t id = a[k];
            if (chain && (k == 1 || k == 2)) continue;              // result type / result id
            if (chain && k == 3 && block.count(id)) continue;       // the block as a chain base (handled above)
            if (r.ptr.count(id)) { r.why = "PointSize is read or passed on"; return r; }
            if (block.count(id)) { r.why = "the output block is used whole"; return r; }
        }
    }
    return r;
}
}  // namespace

size_t xbg_spirv_idvs_strip(const uint32_t *words, size_t count, std::vector<uint32_t> &out, XbgIdvsStats *stats) {
    out.clear(); XbgIdvsStats st;
    std::vector<Ins> ins; if (!parse(words, count, ins)) { st.why = "not SPIR-V"; if (stats) *stats = st; return 0; }
    Ps p = analyse(words, ins);
    if (*p.why) { st.why = p.why; if (stats) *stats = st; return 0; }
    if (p.stores.empty()) { st.why = "no PointSize store"; if (stats) *stats = st; return 0; }
    std::vector<char> drop(ins.size(), 0);
    for (size_t i : p.stores) drop[i] = 1;
    st.stores_dropped = uint32_t(p.stores.size());
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins &x = ins[i];
        if ((x.op == OpAccessChain || x.op == OpInBoundsAccessChain) && x.n >= 5 && p.chains.count(words[x.at + 2])) { drop[i] = 1; ++st.chains_dropped; }
    }
    out.assign(words, words + 5);
    for (size_t i = 0; i < ins.size(); i++) if (!drop[i]) out.insert(out.end(), words + ins[i].at, words + ins[i].at + ins[i].n);
    if (stats) *stats = st;
    return st.stores_dropped;
}

const char *xbg_spirv_idvs_check(const uint32_t *in, size_t in_count, const uint32_t *out, size_t out_count) {
    std::vector<Ins> a, b;
    if (!parse(in, in_count, a)) return "input is not SPIR-V";
    if (!parse(out, out_count, b)) return "output is not SPIR-V";
    if (memcmp(in, out, 5 * sizeof(uint32_t))) return "header changed";
    // PointSize pointers of the input, resolved here on their own: decorations -> Output variables / block members -> chains
    std::unordered_set<uint32_t> ints, psv, outs; std::unordered_map<uint32_t, uint32_t> k32, mem, pt, vt; uint32_t model = ~0u, entries = 0;
    for (const Ins &x : a) {
        const uint32_t *w = in + x.at;
        switch (x.op) {
        case OpEntryPoint: if (x.n >= 3) { ++entries; model = w[1]; } break;
        case OpTypeInt: if (x.n >= 3) ints.insert(w[1]); break;
        case OpConstant: if (x.n == 4 && ints.count(w[1])) k32[w[2]] = w[3]; break;
        case OpDecorate: if (x.n >= 4 && w[2] == DecBuiltIn && w[3] == BuiltInPointSize) psv.insert(w[1]); break;
        case OpMemberDecorate: if (x.n >= 5 && w[3] == DecBuiltIn && w[4] == BuiltInPointSize) mem[w[1]] = w[2]; break;
        case OpTypePointer: if (x.n >= 4) pt[w[1]] = w[3]; break;
        case OpVariable: if (x.n >= 4 && w[3] == SCOutput) { outs.insert(w[2]); vt[w[2]] = w[1]; } break;
        default: break;
        }
    }
    if (entries != 1 || model != ModelVertex) return "not a single-entry vertex module";
    auto ps_pointer = [&](uint32_t id, const std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> &chain) {
        if (outs.count(id) && psv.count(id)) return true;
        auto c = chain.find(id); if (c == chain.end()) return false;
        const uint32_t base = c->second.first; if (!outs.count(base)) return false;
        auto p = pt.find(vt[base]); if (p == pt.end()) return false;
        auto m = mem.find(p->second); return m != mem.end() && c->second.second == m->second;
    };
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> chain;   // single-index chains: id -> (base, constant index)
    for (const Ins &x : a)
        if ((x.op == OpAccessChain || x.op == OpInBoundsAccessChain) && x.n == 5 && k32.count(in[x.at + 4])) chain[in[x.at + 2]] = {in[x.at + 3], k32[in[x.at + 4]]};
    std::unordered_set<uint32_t> removed_chains; size_t j = 0, removed = 0;
    for (const Ins &x : a) {
        if (j < b.size() && b[j].n == x.n && !memcmp(in + x.at, out + b[j].at, x.n * sizeof(uint32_t))) { ++j; continue; }
        const uint32_t *w = in + x.at;
        if (x.op == OpStore && x.n >= 3 && ps_pointer(w[1], chain)) { ++removed; continue; }
        if ((x.op == OpAccessChain || x.op == OpInBoundsAccessChain) && x.n == 5 && ps_pointer(w[2], chain)) { removed_chains.insert(w[2]); ++removed; continue; }
        return "an instruction other than a PointSize store / chain is missing or changed";
    }
    if (j != b.size()) return "output has instructions the input lacks";
    if (!removed) return "nothing removed";
    for (const Ins &x : b) {
        if (declaration(x.op)) continue;
        for (size_t k = 1; k < x.n; k++) if (!literal_word(x.op, k) && removed_chains.count(out[x.at + k])) return "a removed PointSize chain is still used";
    }
    return "";
}

bool xbg_spirv_idvs_stripped(const uint32_t *words, size_t count) {
    std::vector<Ins> ins; if (!parse(words, count, ins)) return false;
    Ps p = analyse(words, ins);
    return !*p.why && p.declared && p.stores.empty();
}
