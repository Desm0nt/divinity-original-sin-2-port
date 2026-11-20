// [DOS2] NR-PRESKIN DM_NR_PRESKIN: see xbg_spirv_preskin.h and docs/dos2/nr/NR-PRESKIN.md.
//
// Plan: classify every value of the one basic block
//   dyn   depends on a vertex input (Location or built-in)
//   inv   a function of Location inputs, uniform blocks (set 0) and constants only (no built-in, image, push constant, Function variable)
//   bone  depends on a load from a uniform block through an index that is dyn (the bone palette reads)
// and solve a minimum cut (Dinic) on the value graph: S -> every dynamic bone load, every non-inv value and every stored value -> T, an inv
// value of a 32-bit scalar / vector type costs 64 per component (+1 when it depends on a uniform block other than the bone block, so the cut
// prefers values a pass-specific constant does not touch), everything else is uncuttable. The cut values are the record; each gets a
// structural hash (opcode, types, constants, Location of inputs, data key of uniform blocks), so two modules that compute the same value
// from the same leaves share it.
#include "xbg_spirv_preskin.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace {
enum : uint32_t {
    OpUndef = 1, OpSource = 3, OpName = 5, OpMemberName = 6, OpString = 7, OpLine = 8, OpExtension = 10, OpExtInstImport = 11, OpExtInst = 12,
    OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16, OpCapability = 17, OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21,
    OpTypeFloat = 22, OpTypeVector = 23, OpTypeMatrix = 24, OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32,
    OpTypeFunction = 33, OpConstantTrue = 41, OpConstantFalse = 42, OpConstant = 43, OpConstantComposite = 44, OpConstantNull = 46,
    OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66,
    OpDecorate = 71, OpMemberDecorate = 72, OpVectorShuffle = 79, OpCompositeConstruct = 80, OpCompositeExtract = 81, OpCompositeInsert = 82,
    OpCopyObject = 83, OpBitcast = 124, OpFNegate = 127, OpIAdd = 128, OpFAdd = 129, OpFSub = 131, OpIMul = 132, OpFMul = 133, OpFDiv = 136,
    OpFRem = 140, OpFMod = 141, OpVectorTimesScalar = 142, OpMatrixTimesScalar = 143, OpVectorTimesMatrix = 144, OpMatrixTimesVector = 145,
    OpMatrixTimesMatrix = 146, OpOuterProduct = 147, OpDot = 148, OpLabel = 248, OpReturn = 253, OpNoLine = 317, OpModuleProcessed = 330,
    DecBlock = 2, DecArrayStride = 6, DecBuiltIn = 11, DecNoContraction = 42, DecInvariant = 18, DecLocation = 30, DecBinding = 33,
    DecDescriptorSet = 34, DecOffset = 35, BuiltInVertexIndex = 42,
    SCUniformConstant = 0, SCInput = 1, SCUniform = 2, SCOutput = 3, SCPrivate = 6, SCFunction = 7, SCPushConstant = 9, SCStorageBuffer = 12
};
constexpr uint64_t INF = std::numeric_limits<uint64_t>::max() / 4;
inline uint64_t mix(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); h *= 0xff51afd7ed558ccdull; return h ^ (h >> 33); }

// the kinds of body instruction (ids read)
enum Kind { K_PURE, K_SHUF, K_EXTRACT, K_INSERT, K_EXT, K_LOAD, K_CHAIN, K_OPAQUE, K_BAD };
Kind kind_of(uint32_t op) {
    if (op == OpCopyObject || op == OpCompositeConstruct || op == 77 || op == 78 || op == 84) return K_PURE;
    if ((op >= 109 && op <= 116) || op == 118 || op == 119 || op == OpBitcast) return K_PURE;
    if (op >= 126 && op <= 148) return K_PURE;
    if (op >= 154 && op <= 191) return K_PURE;
    if (op >= 194 && op <= 205) return K_PURE;
    if (op == OpVectorShuffle) return K_SHUF;
    if (op == OpCompositeExtract) return K_EXTRACT;
    if (op == OpCompositeInsert) return K_INSERT;
    if (op == OpExtInst) return K_EXT;
    if (op == OpLoad) return K_LOAD;
    if (op == OpAccessChain || op == OpInBoundsAccessChain) return K_CHAIN;
    if (op == 86 || (op >= 87 && op <= 107)) return K_OPAQUE;   // images: kept as they are, never in the slice
    return K_BAD;
}
bool float_arith(uint32_t op) {
    return op == OpFNegate || op == OpFAdd || op == OpFSub || op == OpFMul || op == OpFDiv || op == OpFRem || op == OpFMod ||
           (op >= OpVectorTimesScalar && op <= OpDot);
}

struct Inst { size_t at; uint32_t op, n; };
struct Mod {
    const uint32_t *w = nullptr; size_t count = 0; uint32_t bound = 0, version = 0;
    std::vector<Inst> ins;
    size_t ep = SIZE_MAX, fn = SIZE_MAX, label = SIZE_MAX, ret = SIZE_MAX, fn_end = SIZE_MAX, first_global = SIZE_MAX;
    uint32_t nfunc = 0, glsl = 0, entry_id = 0;
    std::unordered_map<uint32_t, size_t> def;                     // result id -> instruction index (globals and body)
    std::unordered_map<uint32_t, uint32_t> loc, builtin, dset, binding, cval;   // decorations; cval: 32-bit OpConstant values
    std::unordered_set<uint32_t> invariant_var, block, nocontract;
    std::unordered_map<uint64_t, uint32_t> member_builtin;        // (struct << 32 | member) -> BuiltIn
    std::unordered_set<uint64_t> member_invariant;
    std::unordered_map<uint32_t, uint32_t> arr_stride;
    std::unordered_map<uint64_t, uint32_t> member_offset;
    std::unordered_map<uint32_t, uint32_t> var_sc, var_ptype;     // global variables
    const uint32_t *at(size_t i) const { return w + ins[i].at; }
    bool parse(const uint32_t *words, size_t n, const char **why) {
        w = words; count = n;
        if (n < 5 || w[0] != 0x07230203u) { *why = "not SPIR-V"; return false; }
        version = w[1]; bound = w[3];
        for (size_t i = 5; i < n;) {
            const uint32_t op = w[i] & 0xffffu, c = w[i] >> 16; if (!c || i + c > n) { *why = "malformed"; return false; }
            const size_t k = ins.size(); ins.push_back({i, op, c}); const uint32_t *a = w + i;
            switch (op) {
            case OpExtInstImport: if (c >= 3 && !memcmp(a + 2, "GLSL.std.450", 13)) glsl = a[1]; def[a[1]] = k; break;
            case OpEntryPoint: if (ep != SIZE_MAX) { *why = "entry points"; return false; } ep = k; if (c < 3 || a[1] != 0) { *why = "not a vertex entry point"; return false; } entry_id = a[2]; break;
            case OpDecorate: if (c >= 3) { const uint32_t t = a[1], d = a[2];
                    if (d == DecLocation && c >= 4) loc[t] = a[3]; else if (d == DecBuiltIn && c >= 4) builtin[t] = a[3];
                    else if (d == DecDescriptorSet && c >= 4) dset[t] = a[3]; else if (d == DecBinding && c >= 4) binding[t] = a[3];
                    else if (d == DecInvariant) invariant_var.insert(t); else if (d == DecBlock) block.insert(t);
                    else if (d == DecArrayStride && c >= 4) arr_stride[t] = a[3]; else if (d == DecNoContraction) nocontract.insert(t); } break;
            case OpMemberDecorate: if (c >= 4) { const uint64_t key = uint64_t(a[1]) << 32 | a[2];
                    if (a[3] == DecBuiltIn && c >= 5) member_builtin[key] = a[4]; else if (a[3] == DecInvariant) member_invariant.insert(key);
                    else if (a[3] == DecOffset && c >= 5) member_offset[key] = a[4]; } break;
            case OpFunction: nfunc++; fn = k; def[a[2]] = k; break;
            case OpFunctionEnd: fn_end = k; break;
            case OpLabel: if (fn == SIZE_MAX) { *why = "label outside a function"; return false; } if (label != SIZE_MAX) { *why = "more than one block"; return false; } label = k; def[a[1]] = k; break;
            case OpReturn: ret = k; break;
            default:
                if (op >= OpTypeVoid && op <= 39) { if (first_global == SIZE_MAX) first_global = k; if (c >= 2) def[a[1]] = k; }
                else if ((op >= OpConstantTrue && op <= OpConstantNull) || (op >= 48 && op <= 52) || op == OpUndef) {
                    if (fn == SIZE_MAX && first_global == SIZE_MAX) first_global = k;
                    if (c >= 3) def[a[2]] = k; if (op == OpConstant && c == 4) cval[a[2]] = a[3]; }
                else if (op == OpVariable) { if (c < 4) { *why = "malformed variable"; return false; } def[a[2]] = k;
                    if (fn == SIZE_MAX) { if (first_global == SIZE_MAX) first_global = k; var_sc[a[2]] = a[3]; var_ptype[a[2]] = a[1]; } }
                else if (fn != SIZE_MAX && label != SIZE_MAX && c >= 3 && op != OpStore && op != OpLine && op != OpNoLine) def[a[2]] = k;
                break;
            }
            i += c;
        }
        if (ep == SIZE_MAX) { *why = "no vertex entry point"; return false; }
        if (nfunc != 1) { *why = "not one function"; return false; }
        if (label == SIZE_MAX || ret == SIZE_MAX || ret < label || fn_end != ret + 1) { *why = "not one block ending in OpReturn"; return false; }
        if (version > 0x10300u) { *why = "SPIR-V newer than 1.3"; return false; }
        if (first_global == SIZE_MAX) { *why = "no types"; return false; }
        return true;
    }
    uint32_t op_of(uint32_t id) const { auto f = def.find(id); return f == def.end() ? 0 : ins[f->second].op; }
    const uint32_t *def_words(uint32_t id) const { auto f = def.find(id); return f == def.end() ? nullptr : at(f->second); }
    // components of a 32-bit scalar / vector type; 0 = something else. ctype: 0 float, 1 sint, 2 uint
    uint32_t comps(uint32_t t, uint32_t *ctype = nullptr, uint32_t *comp_type = nullptr) const {
        const uint32_t *d = def_words(t); if (!d) return 0; const uint32_t op = d[0] & 0xffff;
        if (op == OpTypeFloat && d[2] == 32) { if (ctype) *ctype = 0; if (comp_type) *comp_type = t; return 1; }
        if (op == OpTypeInt && d[2] == 32) { if (ctype) *ctype = d[3] ? 1 : 2; if (comp_type) *comp_type = t; return 1; }
        if (op == OpTypeVector && d[3] >= 2 && d[3] <= 4 && comps(d[2], ctype, comp_type) == 1) return d[3];
        return 0;
    }
};

// ids an instruction reads (body): the words that are ids by its kind
void reads(const uint32_t *a, uint32_t op, uint32_t n, std::vector<uint32_t> &out) {
    out.clear();
    if (op == OpStore) { for (uint32_t k = 1; k < 3 && k < n; k++) out.push_back(a[k]); return; }
    switch (kind_of(op)) {
    case K_PURE: case K_CHAIN: case K_OPAQUE: for (uint32_t k = 3; k < n; k++) out.push_back(a[k]); break;
    case K_SHUF: case K_INSERT: for (uint32_t k = 3; k < 5 && k < n; k++) out.push_back(a[k]); break;
    case K_EXTRACT: case K_LOAD: if (n > 3) out.push_back(a[3]); break;
    case K_EXT: for (uint32_t k = 5; k < n; k++) out.push_back(a[k]); break;
    default: break;
    }
}

struct Dinic {
    struct E { uint32_t to; uint64_t cap; };
    std::vector<E> e; std::vector<std::vector<uint32_t>> g; std::vector<int> lvl; std::vector<size_t> it;
    explicit Dinic(size_t n) : g(n) {}
    void add(uint32_t a, uint32_t b, uint64_t c) { g[a].push_back(uint32_t(e.size())); e.push_back({b, c}); g[b].push_back(uint32_t(e.size())); e.push_back({a, 0}); }
    bool bfs(uint32_t s, uint32_t t) {
        lvl.assign(g.size(), -1); lvl[s] = 0; std::vector<uint32_t> q{s};
        for (size_t h = 0; h < q.size(); h++) for (uint32_t id : g[q[h]]) if (e[id].cap && lvl[e[id].to] < 0) { lvl[e[id].to] = lvl[q[h]] + 1; q.push_back(e[id].to); }
        return lvl[t] >= 0;
    }
    uint64_t dfs(uint32_t v, uint32_t t, uint64_t f) {   // iterative enough for these graphs: depth = path length (< nodes)
        if (v == t) return f;
        for (size_t &i = it[v]; i < g[v].size(); i++) { const uint32_t id = g[v][i]; E &x = e[id];
            if (!x.cap || lvl[x.to] != lvl[v] + 1) continue;
            const uint64_t d = dfs(x.to, t, std::min(f, x.cap));
            if (d) { x.cap -= d; e[id ^ 1].cap += d; return d; } }
        return 0;
    }
    uint64_t run(uint32_t s, uint32_t t, uint64_t limit) {
        uint64_t f = 0;
        while (f < limit && bfs(s, t)) { it.assign(g.size(), 0); while (uint64_t d = dfs(s, t, INF)) { f += d; if (f >= limit) return f; } }
        return f;
    }
    std::vector<char> reach(uint32_t s) const {
        std::vector<char> r(g.size(), 0); std::vector<uint32_t> q{s}; r[s] = 1;
        for (size_t h = 0; h < q.size(); h++) for (uint32_t id : g[q[h]]) if (e[id].cap && !r[e[id].to]) { r[e[id].to] = 1; q.push_back(e[id].to); }
        return r;
    }
};

// The analysis both emitters repeat (deterministic on the same words): per body value, its facts; the cut and the slice set.
struct Ana {
    Mod m; const char *why = "";
    std::vector<size_t> body;                                      // instruction indexes of the block (after OpLabel, before OpReturn)
    std::unordered_map<uint32_t, uint8_t> dyn, inv, bone, kdep, ptr; // value facts
    std::unordered_map<uint32_t, uint32_t> root;                   // pointer value -> its variable
    std::unordered_set<uint32_t> U;                                // bone blocks: uniform variables indexed by a dyn value
    std::vector<uint32_t> cut;                                     // cut value ids, in body order
    std::unordered_set<uint32_t> cutset, slice;                    // slice: every value the cut values need (inv ancestors, the cut included)
    std::unordered_map<uint32_t, uint64_t> hash;                   // structural hashes
    std::unordered_map<uint32_t, uint32_t> ubo_key;                // uniform var -> data key
    uint32_t bone_loads = 0;

    bool is_loc_input(uint32_t var) const { auto s = m.var_sc.find(var); return s != m.var_sc.end() && s->second == SCInput && m.loc.count(var) && !m.builtin.count(var); }
    bool is_ubo(uint32_t var) const {
        auto s = m.var_sc.find(var); if (s == m.var_sc.end() || s->second != SCUniform) return false;
        auto d = m.dset.find(var); return d != m.dset.end() && d->second == 0 && m.binding.count(var);
    }
    uint64_t type_hash(uint32_t t, int depth = 0) {
        auto f = hash.find(t | 0x80000000u); if (f != hash.end()) return f->second;
        const uint32_t *d = m.def_words(t); uint64_t h = 0x51ED27;
        if (!d || depth > 16) return h;
        const uint32_t op = d[0] & 0xffff, n = d[0] >> 16; h = mix(h, op);
        for (uint32_t k = 2; k < n; k++) {
            const uint32_t x = d[k];
            if (op == OpTypeInt || op == OpTypeFloat || (op == OpTypeVector && k == 3) || (op == OpTypeMatrix && k == 3) || (op == OpTypePointer && k == 2)) h = mix(h, x);
            else if (op == OpTypeArray && k == 3) h = mix(h, m.cval.count(x) ? m.cval.at(x) : 0xFFFFFFFFu);
            else h = mix(h, type_hash(x, depth + 1));
        }
        if (m.arr_stride.count(t)) h = mix(h, 0xA0000000ull | m.arr_stride.at(t));
        if (op == OpTypeStruct) for (uint32_t k = 2; k < n; k++) { auto o = m.member_offset.find(uint64_t(t) << 32 | (k - 2)); h = mix(h, o == m.member_offset.end() ? 0xFFFFFFFFull : o->second); }
        hash[t | 0x80000000u] = h; return h;
    }
    uint64_t val_hash(uint32_t id) {
        auto f = hash.find(id); if (f != hash.end()) return f->second;
        const uint32_t *d = m.def_words(id); uint64_t h = 0xC0FFEE;
        if (!d) return h;
        const uint32_t op = d[0] & 0xffff, n = d[0] >> 16;
        if (op == OpVariable) {
            if (is_loc_input(id)) h = mix(mix(0x1A, m.loc.at(id)), type_hash(d[1]));
            else if (ubo_key.count(id)) h = mix(mix(0x2B, ubo_key.at(id)), type_hash(d[1]));
            else h = mix(0x3C, id);   // never in a slice
        } else if ((op >= OpConstantTrue && op <= OpConstantNull) || (op >= 48 && op <= 52)) {
            h = mix(mix(0x4D, op), type_hash(d[1]));
            for (uint32_t k = 3; k < n; k++) h = mix(h, op == OpConstantComposite ? val_hash(d[k]) : d[k]);
        } else {
            h = mix(mix(0x5E, op), type_hash(d[1]));
            std::vector<uint32_t> r; reads(d, op, n, r);
            const Kind kd = kind_of(op);
            for (uint32_t k = 3; k < n; k++) {
                bool is_id = false; for (uint32_t x : r) if (x == d[k]) is_id = true;
                if (kd == K_EXT && k == 3) { h = mix(h, 0x6F); continue; }   // the GLSL.std.450 set (checked)
                if (kd == K_EXTRACT || kd == K_SHUF || kd == K_INSERT || (kd == K_EXT && k == 4) || (kd == K_LOAD && k > 3)) {
                    // literal positions
                    const bool lit = (kd == K_EXTRACT && k > 3) || (kd == K_SHUF && k > 4) || (kd == K_INSERT && k > 4) || (kd == K_EXT && k == 4) || (kd == K_LOAD && k > 3);
                    if (lit) { h = mix(h, 0x70000000ull | d[k]); continue; }
                }
                h = mix(h, is_id ? val_hash(d[k]) : d[k]);
            }
            if (m.nocontract.count(id)) h = mix(h, 0x42);
        }
        hash[id] = h; return h;
    }

    bool run(const uint32_t *words, size_t count, const XbgPreskinBind *binds, size_t nb) {
        if (!m.parse(words, count, &why)) return false;
        for (size_t i = m.label + 1; i < m.ret; i++) body.push_back(i);
        // uniform blocks: data keys
        for (auto &kv : m.var_sc) if (is_ubo(kv.first)) {
            const uint32_t b = m.binding.at(kv.first); bool found = false;
            for (size_t k = 0; k < nb; k++) if (binds[k].binding == b) { ubo_key[kv.first] = binds[k].key; found = true; }
            if (!found) ubo_key[kv.first] = 0xFFFF0000u | b;   // unknown: hashed by binding; the runtime refuses it (no slot)
        }
        std::vector<uint32_t> r;
        // pass 1: dyn, pointer roots, U, the body's form
        for (size_t i : body) {
            const uint32_t *a = m.at(i); const uint32_t op = m.ins[i].op, n = m.ins[i].n;
            if (op == OpLine || op == OpNoLine) continue;
            if (op == OpVariable) { if (a[3] != SCFunction) { why = "non-Function variable in the block"; return false; } continue; }
            if (op == OpStore) {
                if (n < 3) { why = "malformed store"; return false; }
                auto rt = root.find(a[1]); const uint32_t var = rt != root.end() ? rt->second : a[1];
                auto s = m.var_sc.find(var); if (s == m.var_sc.end() || s->second != SCOutput) { why = "a store to a non-Output variable"; return false; }
                continue;
            }
            const Kind kd = kind_of(op); if (kd == K_BAD) { why = "an instruction the plan does not know"; return false; }
            if (kd == K_EXT && a[3] != m.glsl) { why = "an extended instruction set other than GLSL.std.450"; return false; }
            reads(a, op, n, r); const uint32_t id = a[2];
            bool d = false; for (uint32_t x : r) if (dyn.count(x) && dyn[x]) d = true;
            if (kd == K_CHAIN || kd == K_LOAD) {
                const uint32_t base = kd == K_CHAIN ? a[3] : a[3]; auto rt = root.find(base); const uint32_t var = rt != root.end() ? rt->second : base;
                auto s = m.var_sc.find(var);
                if (s == m.var_sc.end()) { why = "a pointer not rooted at a global variable"; return false; }
                if (s->second == SCFunction) { why = "a Function variable is read"; return false; }
                if (s->second == SCInput) d = true;
                if (kd == K_CHAIN) { root[id] = var; ptr[id] = 1;
                    if (s->second == SCUniform) for (uint32_t k = 4; k < n; k++) if (dyn.count(a[k]) && dyn[a[k]]) U.insert(var); }
            }
            dyn[id] = d;
        }
        if (U.empty()) { why = "no uniform block read through a vertex-input index"; return false; }
        for (uint32_t u : U) if (!is_ubo(u)) { why = "the bone block is not a set-0 uniform block"; return false; }
        // pass 2: inv / bone / kdep
        for (size_t i : body) {
            const uint32_t *a = m.at(i); const uint32_t op = m.ins[i].op, n = m.ins[i].n;
            if (op == OpLine || op == OpNoLine || op == OpVariable || op == OpStore) continue;
            const Kind kd = kind_of(op); reads(a, op, n, r); const uint32_t id = a[2];
            auto known = [&](uint32_t x) { return m.def.count(x) && (m.op_of(x) == OpConstant || m.op_of(x) == OpConstantComposite || m.op_of(x) == OpConstantTrue ||
                                                                     m.op_of(x) == OpConstantFalse || m.op_of(x) == OpConstantNull); };
            bool iv = true, bn = false, kd_ = false;
            for (uint32_t x : r) { if (known(x)) continue; if (m.var_sc.count(x)) continue;
                if (!inv.count(x) || !inv[x]) iv = false; if (bone.count(x) && bone[x]) bn = true; if (kdep.count(x) && kdep[x]) kd_ = true; }
            if (kd == K_CHAIN || kd == K_LOAD) {
                const uint32_t var = root.count(a[3]) ? root[a[3]] : a[3];
                const bool vin = is_loc_input(var), vub = is_ubo(var);
                if (!vin && !vub) iv = false;
                if (kd == K_CHAIN && vub) { bool dy = false; for (uint32_t k = 4; k < n; k++) if (dyn.count(a[k]) && dyn[a[k]]) dy = true;
                    if (U.count(var) && dy) bn = true; if (!U.count(var)) kd_ = true; }
            }
            if (kd == K_OPAQUE) iv = false;
            inv[id] = iv; bone[id] = bn; kdep[id] = kd_;
            if (kd == K_LOAD && bn) bone_loads++;
        }
        // the graph: node 2v = v_in, 2v + 1 = v_out over body values; S, T after
        std::unordered_map<uint32_t, uint32_t> node; std::vector<uint32_t> vals;
        for (size_t i : body) { const uint32_t op = m.ins[i].op; if (op == OpLine || op == OpNoLine || op == OpVariable || op == OpStore) continue;
            node[m.at(i)[2]] = uint32_t(vals.size()); vals.push_back(m.at(i)[2]); }
        const uint32_t S = uint32_t(2 * vals.size()), T = S + 1; Dinic g(T + 1);
        for (size_t i : body) {
            const uint32_t *a = m.at(i); const uint32_t op = m.ins[i].op, n = m.ins[i].n;
            if (op == OpLine || op == OpNoLine || op == OpVariable) continue;
            reads(a, op, n, r);
            if (op == OpStore) { if (node.count(a[2])) g.add(2 * node[a[2]] + 1, T, INF); continue; }
            const uint32_t id = a[2], v = node[id];
            uint32_t ct = 0; const uint32_t c = (inv[id] && !ptr.count(id)) ? m.comps(a[1], &ct) : 0;
            g.add(2 * v, 2 * v + 1, c ? uint64_t(c) * 64 + (kdep[id] ? 1 : 0) : INF);
            for (uint32_t x : r) if (node.count(x)) g.add(2 * node[x] + 1, 2 * v, INF);
            if (!inv[id]) g.add(2 * v, T, INF);
            if (kind_of(op) == K_LOAD && bone[id] && inv[id]) g.add(S, 2 * v, INF);
        }
        const uint64_t f = g.run(S, T, INF);
        if (f >= INF) { why = "no finite cut (a bone-dependent value reaches a non-inv use through an uncuttable value)"; return false; }
        if (!f) { why = "nothing to cut"; return false; }
        const std::vector<char> rs = g.reach(S);
        for (uint32_t id : vals) { const uint32_t v = node[id]; if (rs[2 * v] && !rs[2 * v + 1]) { cut.push_back(id); cutset.insert(id); } }
        // slice: inv ancestors of the cut values
        std::vector<uint32_t> st(cut.begin(), cut.end());
        while (!st.empty()) { const uint32_t id = st.back(); st.pop_back(); if (!slice.insert(id).second) continue;
            const uint32_t *d = m.def_words(id); if (!d) continue; reads(d, d[0] & 0xffff, d[0] >> 16, r);
            for (uint32_t x : r) if (node.count(x)) { if (!inv[x] && !ptr.count(x)) { why = "a slice value is not inv"; return false; } st.push_back(x); } }
        for (uint32_t id : slice) if (!inv[id]) { why = "a slice value is not inv"; return false; }
        return true;
    }
};

// ---- module writer ----
struct W {
    std::vector<uint32_t> o;
    void ins(uint32_t op, std::initializer_list<uint32_t> a) { o.push_back(uint32_t(a.size() + 1) << 16 | op); o.insert(o.end(), a.begin(), a.end()); }
    void ins(uint32_t op, const std::vector<uint32_t> &a) { o.push_back(uint32_t(a.size() + 1) << 16 | op); o.insert(o.end(), a.begin(), a.end()); }
    void copy(const uint32_t *a) { o.insert(o.end(), a, a + (a[0] >> 16)); }
};
// global ids found or made: types / pointers / constants (made ones go before the first function)
struct Globals {
    const Mod &m; uint32_t &bound; std::vector<std::vector<uint32_t>> made;   // each: op word then operands
    Globals(const Mod &mm, uint32_t &b) : m(mm), bound(b) {}
    uint32_t find(uint32_t op, std::initializer_list<uint32_t> a) {   // an existing global with these operands after the result id
        for (size_t i = m.first_global; i < m.fn && i < m.ins.size(); i++) { const Inst &x = m.ins[i]; if (x.op != op || x.n != a.size() + 2) continue;
            const uint32_t *w = m.at(i); size_t k = 2; bool eq = true; for (uint32_t v : a) if (w[k++] != v) { eq = false; break; } if (eq) return w[1]; }
        for (auto &v : made) if ((v[0] & 0xffff) == op && v.size() == a.size() + 2) { size_t k = 2; bool eq = true; for (uint32_t x : a) if (v[k++] != x) { eq = false; break; } if (eq) return v[1]; }
        return 0;
    }
    uint32_t type(uint32_t op, std::initializer_list<uint32_t> a, bool unique = true) {
        if (unique) if (uint32_t t = find(op, a)) return t;
        std::vector<uint32_t> v{uint32_t(a.size() + 2) << 16 | op, bound++}; v.insert(v.end(), a.begin(), a.end()); made.push_back(v); return v[1];
    }
    uint32_t konst(uint32_t t, uint32_t value) {   // OpConstant: type, result, value
        for (size_t i = m.first_global; i < m.fn && i < m.ins.size(); i++) { const Inst &x = m.ins[i]; if (x.op != OpConstant || x.n != 4) continue; const uint32_t *w = m.at(i); if (w[1] == t && w[3] == value) return w[2]; }
        for (auto &v : made) if ((v[0] & 0xffff) == OpConstant && v[1] == t && v[3] == value) return v[2];
        std::vector<uint32_t> v{4u << 16 | OpConstant, t, bound++, value}; made.push_back(v); return v[2];
    }
    uint32_t var(uint32_t ptype, uint32_t sc) { std::vector<uint32_t> v{4u << 16 | OpVariable, ptype, bound++, sc}; made.push_back(v); return v[2]; }
};
// the base words of an instruction minus ids in drop (OpEntryPoint interface)
void entry_point(W &o, const Mod &m, const std::unordered_set<uint32_t> &drop, const std::vector<uint32_t> &add) {
    const uint32_t *a = m.at(m.ep); const uint32_t n = m.ins[m.ep].n;
    size_t k = 3; while (k < n) { const uint32_t x = a[k++]; if (!((x & 0xff) && (x & 0xff00) && (x & 0xff0000) && (x & 0xff000000u))) break; }
    std::vector<uint32_t> v(a + 1, a + k);
    for (; k < n; k++) if (!drop.count(a[k])) v.push_back(a[k]);
    for (uint32_t x : add) v.push_back(x);
    o.ins(OpEntryPoint, v);
}
bool targets(const Mod &m, size_t i, const std::unordered_set<uint32_t> &gone) {
    const uint32_t op = m.ins[i].op; const uint32_t *a = m.at(i);
    if ((op == OpName || op == OpMemberName || op == OpDecorate || op == OpMemberDecorate) && m.ins[i].n >= 2) return gone.count(a[1]) != 0;
    return false;
}
}  // namespace

bool xbg_spirv_preskin_candidate(const uint32_t *words, size_t count) {
    Ana a; if (!a.m.parse(words, count, &a.why)) return false;
    // cheap: a uniform access chain whose index is not a constant and the module has Location 14 / 15 inputs or any Location input in an index
    std::unordered_set<uint32_t> dyn;
    for (auto &kv : a.m.var_sc) if (kv.second == SCInput) dyn.insert(kv.first);
    std::unordered_map<uint32_t, uint32_t> root; std::vector<uint32_t> r;
    for (size_t i = a.m.label + 1; i < a.m.ret; i++) {
        const uint32_t *w = a.m.at(i); const uint32_t op = a.m.ins[i].op, n = a.m.ins[i].n;
        if (op == OpStore || op == OpVariable || op == OpLine || op == OpNoLine || n < 3) continue;
        reads(w, op, n, r); bool d = false; for (uint32_t x : r) if (dyn.count(x)) d = true;
        if (op == OpAccessChain || op == OpInBoundsAccessChain) { const uint32_t var = root.count(w[3]) ? root[w[3]] : w[3]; root[w[2]] = var;
            auto s = a.m.var_sc.find(var); if (s != a.m.var_sc.end() && s->second == SCUniform) for (uint32_t k = 4; k < n; k++) if (dyn.count(w[k])) return true; }
        if (d) dyn.insert(w[2]);
    }
    return false;
}

bool xbg_spirv_preskin_plan(const uint32_t *words, size_t count, const XbgPreskinBind *binds, size_t nbinds, XbgPreskinPlan &plan) {
    plan = XbgPreskinPlan{};
    Ana a; if (!a.run(words, count, binds, nbinds)) { plan.why = a.why; return false; }
    // record: cut values sorted by hash
    std::vector<std::pair<uint64_t, uint32_t>> order;
    for (uint32_t id : a.cut) order.push_back({a.val_hash(id), id});
    std::sort(order.begin(), order.end());
    for (size_t i = 1; i < order.size(); i++) if (order[i].first == order[i - 1].first) { plan.why = "two cut values hash alike"; return false; }
    uint32_t first = 0; uint64_t sig = 0x5EC0;
    for (auto &p : order) { const uint32_t *d = a.m.def_words(p.second); uint32_t ct = 0; const uint32_t c = a.m.comps(d[1], &ct);
        plan.vals.push_back({p.first, c, ct, first, 0, 0}); first += c; sig = mix(mix(sig, p.first), c); if (a.kdep[p.second]) plan.kdep++; }
    plan.m = first; plan.sig = sig;
    if (plan.m == 0 || plan.m > 16) { plan.why = "the cut needs more than 16 components a vertex"; return false; }
    plan.attrs = (plan.m + 3) / 4;
    // Location masks; the consumer's inputs: every Location input still read after the cut values are replaced (dead code removed)
    std::vector<uint32_t> r;
    for (auto &kv : a.m.var_sc) if (a.is_loc_input(kv.first)) { const uint32_t l = a.m.loc.at(kv.first); if (l >= 32) { plan.why = "a Location past 31"; return false; } plan.orig_locs |= 1u << l; }
    for (uint32_t id : a.slice) { const uint32_t *d = a.m.def_words(id); const uint32_t op = d[0] & 0xffff;
        if (op == OpLoad || op == OpAccessChain || op == OpInBoundsAccessChain) { const uint32_t base = d[3]; const uint32_t var = a.root.count(base) ? a.root[base] : base;
            if (a.is_loc_input(var)) plan.slice_locs |= 1u << a.m.loc.at(var);
            else if (a.is_ubo(var) && std::find(plan.slice_ubos.begin(), plan.slice_ubos.end(), a.m.binding.at(var)) == plan.slice_ubos.end()) {
                plan.slice_ubos.push_back(a.m.binding.at(var)); plan.ubo_keys.push_back(a.ubo_key.at(var)); } } }
    // per value: the Location inputs and uniform blocks it is computed from (its leaves)
    {   std::vector<uint32_t> r2;
        for (size_t v = 0; v < order.size(); v++) {
            std::unordered_set<uint32_t> seen; std::vector<uint32_t> st2{order[v].second};
            while (!st2.empty()) { const uint32_t id = st2.back(); st2.pop_back(); if (!seen.insert(id).second) continue;
                const uint32_t *d = a.m.def_words(id); if (!d || !a.slice.count(id)) continue; const uint32_t op = d[0] & 0xffff;
                if (op == OpLoad || op == OpAccessChain || op == OpInBoundsAccessChain) { const uint32_t var = a.root.count(d[3]) ? a.root[d[3]] : d[3];
                    if (a.is_loc_input(var)) plan.vals[v].loc_mask |= 1u << a.m.loc.at(var);
                    else if (a.is_ubo(var)) for (size_t k = 0; k < plan.slice_ubos.size(); k++) if (plan.slice_ubos[k] == a.m.binding.at(var)) plan.vals[v].ubo_mask |= 1u << k; }
                reads(d, op, d[0] >> 16, r2); for (uint32_t x : r2) st2.push_back(x); }
        }
    }
    for (uint32_t u : a.U) { plan.bone_binding = a.m.binding.at(u); break; }
    // liveness of the consumer: backward from stores, cut values are leaves
    std::unordered_set<uint32_t> live; std::vector<uint32_t> st;
    for (size_t i : a.body) if (a.m.ins[i].op == OpStore) { const uint32_t *d = a.m.at(i); st.push_back(d[1]); st.push_back(d[2]); }
    while (!st.empty()) { const uint32_t id = st.back(); st.pop_back(); if (!live.insert(id).second) continue; if (a.cutset.count(id)) continue;
        const uint32_t *d = a.m.def_words(id); if (!d || !a.m.def.count(id) || a.m.def.at(id) <= a.m.label) continue;
        reads(d, d[0] & 0xffff, d[0] >> 16, r); for (uint32_t x : r) st.push_back(x); }
    uint32_t ops_body = 0;
    for (size_t i : a.body) { const uint32_t op = a.m.ins[i].op; if (op == OpLine || op == OpNoLine || op == OpVariable) continue; ops_body++;
        if (op == OpStore) continue; const uint32_t id = a.m.at(i)[2];
        if (live.count(id)) { plan.ops_cons++; const uint32_t *d = a.m.at(i);
            if (!a.cutset.count(id) && (op == OpLoad || op == OpAccessChain || op == OpInBoundsAccessChain)) { const uint32_t var = a.root.count(d[3]) ? a.root[d[3]] : d[3]; if (a.is_loc_input(var)) plan.cons_locs |= 1u << a.m.loc.at(var); } }
        if (a.slice.count(id)) plan.ops_slice++; }
    plan.ops_orig = ops_body;
    for (size_t i : a.body) if (a.m.ins[i].op == OpStore) plan.ops_cons++;
    // new locations: the lowest ones the consumer does not read, below 16
    uint32_t k = 0;
    for (uint32_t l = 0; l < 16; l++) if (!(plan.cons_locs & (1u << l))) { plan.free_locs |= 1u << l; if (k < plan.attrs) plan.new_locs[k++] = l; }
    if (k < plan.attrs) { plan.why = "no free vertex input locations below 16"; return false; }
    uint32_t maxb = 0; for (auto &kv : a.m.binding) maxb = std::max(maxb, kv.second);
    plan.ssbo_binding = maxb + 1; plan.bone_loads = a.bone_loads;
    for (uint32_t b : plan.ubo_keys) if ((b & 0xFFFF0000u) == 0xFFFF0000u) { plan.why = "the slice reads a uniform block without a data key"; return false; }
    plan.ok = true; return true;
}

namespace {
// cut values in record order (the plan's sort), re-derived from the words
bool record_order(Ana &a, const XbgPreskinPlan &plan, std::vector<uint32_t> &ids, const char **why) {
    ids.assign(plan.vals.size(), 0);
    for (uint32_t id : a.cut) { const uint64_t h = a.val_hash(id); bool f = false;
        for (size_t k = 0; k < plan.vals.size(); k++) if (plan.vals[k].hash == h) { ids[k] = id; f = true; }
        if (!f) { *why = "the plan is not of these words"; return false; } }
    for (uint32_t id : ids) if (!id) { *why = "the plan is not of these words"; return false; }
    return true;
}
// [i, end) of the instructions before the function, in the module's order, with drop / extra applied by the callers
}  // namespace

bool xbg_spirv_preskin_layout(const XbgPreskinPlan &plan, const uint32_t *pos, XbgPreskinLayout &lay) {
    lay = XbgPreskinLayout{}; if (!plan.ok || !plan.m || plan.m > 16) return false;
    uint32_t free = plan.free_locs, j = 0;
    while (j < plan.m) {
        const uint32_t p0 = pos ? pos[j] : j; uint32_t w = 1;
        while (j + w < plan.m && w < 4 && (pos ? pos[j + w] : j + w) == p0 + w) w++;
        if (!free || lay.groups >= 16) return false;
        const uint32_t l = uint32_t(__builtin_ctz(free)); free &= free - 1;
        lay.loc[lay.groups] = l; lay.first[lay.groups] = p0; lay.width[lay.groups] = w; lay.groups++; j += w;
    }
    return true;
}

bool xbg_spirv_preskin_consumer(const uint32_t *words, size_t count, const XbgPreskinPlan &plan, const uint32_t *pos, std::vector<uint32_t> &out,
                                XbgPreskinLayout *lay_out, const char **why) {
    const char *dummy = ""; if (!why) why = &dummy; *why = ""; out.clear();
    if (!plan.ok) { *why = "no plan"; return false; }
    XbgPreskinLayout lay; if (!xbg_spirv_preskin_layout(plan, pos, lay)) { *why = "no layout (locations)"; return false; }
    if (lay_out) *lay_out = lay;
    // the cut values' hashes need the data keys the plan saw for the blocks the slice reads (the only leaves they have); others get a stand-in
    std::vector<XbgPreskinBind> binds; for (size_t k = 0; k < plan.slice_ubos.size(); k++) binds.push_back({plan.slice_ubos[k], plan.ubo_keys[k]});
    Ana a;
    if (!a.run(words, count, binds.data(), binds.size())) { *why = a.why; return false; }
    std::vector<uint32_t> ids; if (!record_order(a, plan, ids, why)) return false;
    const Mod &m = a.m; uint32_t bound = m.bound; Globals G(m, bound);
    const uint32_t t_u = G.type(OpTypeInt, {32, 0});
    uint32_t t_vec[5] = {0, t_u, 0, 0, 0}, p_in[5] = {}, var[16] = {}, ld[16] = {}, grp[16] = {}, gidx[16] = {};
    for (uint32_t g = 0; g < lay.groups; g++) { const uint32_t w = lay.width[g]; if (w > 1 && !t_vec[w]) t_vec[w] = G.type(OpTypeVector, {t_u, w});
        if (!p_in[w]) p_in[w] = G.type(OpTypePointer, {SCInput, t_vec[w]}); var[g] = G.var(p_in[w], SCInput); ld[g] = bound++; }
    for (uint32_t g = 0, j = 0; g < lay.groups; g++) for (uint32_t q = 0; q < lay.width[g]; q++, j++) { grp[j] = g; gidx[j] = q; }   // plan component -> group, lane
    // replacements: cut id -> its words
    std::unordered_map<uint32_t, std::vector<uint32_t>> rep;
    for (size_t v = 0; v < ids.size(); v++) {
        const uint32_t id = ids[v]; const uint32_t *d = m.def_words(id); uint32_t ct = 0, ctid = 0; const uint32_t c = m.comps(d[1], &ct, &ctid);
        W o; std::vector<uint32_t> parts;
        // one component j of the value: its raw uint (an extract unless its group is a lone uint), then the component's type;
        // `to` = the id it must get (the cut value itself when c == 1)
        auto component = [&](uint32_t j, uint32_t to) {
            const uint32_t pc = plan.vals[v].first + j, g = grp[pc], q = gidx[pc], w = lay.width[g];
            if (ct == 2) {   // uint: the word itself
                if (w == 1) o.ins(OpCopyObject, {t_u, to, ld[g]}); else o.ins(OpCompositeExtract, {t_u, to, ld[g], q});
                return;
            }
            uint32_t x = ld[g];
            if (w > 1) { x = bound++; o.ins(OpCompositeExtract, {t_u, x, ld[g], q}); }
            o.ins(OpBitcast, {ctid, to, x});
        };
        if (c == 1) component(0, id);
        else { for (uint32_t j = 0; j < c; j++) { const uint32_t y = bound++; component(j, y); parts.push_back(y); }
            std::vector<uint32_t> v2{d[1], id}; v2.insert(v2.end(), parts.begin(), parts.end()); o.ins(OpCompositeConstruct, v2); }
        rep[id] = o.o;
    }
    // dead code: body values unused after the replacement (stores and the cut values' new words are the roots)
    std::unordered_set<uint32_t> live; std::vector<uint32_t> st, r;
    for (size_t i : a.body) if (m.ins[i].op == OpStore) { const uint32_t *d = m.at(i); st.push_back(d[1]); st.push_back(d[2]); }
    while (!st.empty()) { const uint32_t id = st.back(); st.pop_back(); if (!live.insert(id).second) continue; if (rep.count(id)) continue;
        auto f = m.def.find(id); if (f == m.def.end() || f->second <= m.label || f->second >= m.ret) continue;
        const uint32_t *d = m.at(f->second); reads(d, d[0] & 0xffff, d[0] >> 16, r); for (uint32_t x : r) st.push_back(x); }
    // globals the body no longer references: Input variables only (uniform blocks stay: the pipeline layout has them anyway)
    std::unordered_set<uint32_t> refd;
    for (size_t i : a.body) { const uint32_t op = m.ins[i].op; if (op == OpLine || op == OpNoLine || op == OpVariable) continue;
        if (op != OpStore && !live.count(m.at(i)[2])) continue; if (op != OpStore && rep.count(m.at(i)[2])) continue;
        const uint32_t *d = m.at(i); reads(d, op, m.ins[i].n, r); for (uint32_t x : r) refd.insert(x); }
    std::unordered_set<uint32_t> gone;
    for (auto &kv : m.var_sc) if (kv.second == SCInput && !refd.count(kv.first)) gone.insert(kv.first);
    // write
    W o; o.o.assign(words, words + 5);
    for (size_t i = 0; i < m.fn; i++) {
        const uint32_t op = m.ins[i].op;
        if (i == m.ep) { std::vector<uint32_t> add(var, var + lay.groups); entry_point(o, m, gone, add); continue; }
        if (targets(m, i, gone)) continue;
        if (op == OpVariable && gone.count(m.at(i)[2])) continue;
        if (i == m.first_global) {   // new decorations go right before the first type
            for (uint32_t g = 0; g < lay.groups; g++) o.ins(OpDecorate, {var[g], DecLocation, lay.loc[g]});
        }
        o.copy(m.at(i));
    }
    // the made globals right before the function (they only use earlier ids)
    for (auto &v : G.made) o.o.insert(o.o.end(), v.begin(), v.end());
    for (size_t i = m.fn; i <= m.label; i++) o.copy(m.at(i));
    bool loads_done = false;
    for (size_t i : a.body) {
        const uint32_t op = m.ins[i].op; const uint32_t *d = m.at(i);
        if (op == OpLine || op == OpNoLine) continue;
        if (op == OpVariable) { o.copy(d); continue; }   // Function variables (unused) stay at the top of the block
        if (!loads_done) { loads_done = true;
            for (uint32_t g = 0; g < lay.groups; g++) o.ins(OpLoad, {t_vec[lay.width[g]], ld[g], var[g]}); }
        if (op == OpStore) { o.copy(d); continue; }
        const uint32_t id = d[2];
        if (rep.count(id)) { const auto &v = rep[id]; o.o.insert(o.o.end(), v.begin(), v.end()); continue; }
        if (!live.count(id)) continue;
        o.copy(d);
    }
    if (!loads_done) for (uint32_t g = 0; g < lay.groups; g++) o.ins(OpLoad, {t_vec[lay.width[g]], ld[g], var[g]});
    for (size_t i = m.ret; i < m.ins.size(); i++) o.copy(m.at(i));
    o.o[3] = bound; out.swap(o.o); return true;
}

bool xbg_spirv_preskin_slice(const uint32_t *words, size_t count, const XbgPreskinPlan &plan, std::vector<uint32_t> &out, const char **why) {
    const char *dummy = ""; if (!why) why = &dummy; *why = ""; out.clear();
    if (!plan.ok) { *why = "no plan"; return false; }
    std::vector<XbgPreskinBind> binds; for (size_t k = 0; k < plan.slice_ubos.size(); k++) binds.push_back({plan.slice_ubos[k], plan.ubo_keys[k]});
    Ana a; if (!a.run(words, count, binds.data(), binds.size())) { *why = a.why; return false; }
    if (a.m.version < 0x10300u) { *why = "SPIR-V older than 1.3 (StorageBuffer storage class)"; return false; }
    std::vector<uint32_t> ids; if (!record_order(a, plan, ids, why)) return false;
    const Mod &m = a.m; uint32_t bound = m.bound; Globals G(m, bound);
    // values that fed an Invariant output in the original: NoContraction on their float arithmetic in the slice
    std::unordered_set<uint32_t> inv_feed; std::vector<uint32_t> st, r;
    for (size_t i : a.body) if (m.ins[i].op == OpStore) {
        const uint32_t *d = m.at(i); const uint32_t p = d[1]; bool invar = false;
        const uint32_t var = a.root.count(p) ? a.root[p] : p;
        if (m.invariant_var.count(var)) invar = true;
        else if (a.root.count(p)) {   // a member of a built-in block: OpAccessChain %var %member ...
            const uint32_t *c = m.def_words(p); const uint32_t *vt = m.def_words(var);
            if (c && vt && (c[0] >> 16) >= 5) { const uint32_t *pt = m.def_words(vt[1]); const uint32_t st_ = pt ? pt[3] : 0; auto mc = m.cval.find(c[4]);
                if (mc != m.cval.end() && m.member_invariant.count(uint64_t(st_) << 32 | mc->second)) invar = true; }
        }
        if (invar) st.push_back(d[2]);
    }
    while (!st.empty()) { const uint32_t id = st.back(); st.pop_back(); if (!inv_feed.insert(id).second) continue;
        auto f = m.def.find(id); if (f == m.def.end() || f->second <= m.label || f->second >= m.ret) continue;
        const uint32_t *d = m.at(f->second); reads(d, d[0] & 0xffff, d[0] >> 16, r); for (uint32_t x : r) st.push_back(x); }
    // globals the slice keeps: Input / Uniform variables it reads; everything else goes (Outputs, images, unused inputs)
    std::unordered_set<uint32_t> refd;
    for (uint32_t id : a.slice) { const uint32_t *d = m.def_words(id); reads(d, d[0] & 0xffff, d[0] >> 16, r); for (uint32_t x : r) refd.insert(x); }
    std::unordered_set<uint32_t> gone;
    for (auto &kv : m.var_sc) if (!refd.count(kv.first)) gone.insert(kv.first);
    for (auto &kv : m.var_sc) if (refd.count(kv.first) && kv.second != SCInput && kv.second != SCUniform) { *why = "the slice reads a variable that is not an input or a uniform block"; return false; }
    // new globals
    const uint32_t t_u = G.type(OpTypeInt, {32, 0}), t_i = G.type(OpTypeInt, {32, 1});
    uint32_t vi = 0;
    for (auto &kv : m.var_sc) if (kv.second == SCInput && m.builtin.count(kv.first) && m.builtin.at(kv.first) == BuiltInVertexIndex) {
        const uint32_t *pt = m.def_words(m.var_ptype.at(kv.first)); if (pt && pt[3] == t_i) { vi = kv.first; gone.erase(vi); } }
    bool new_vi = false;
    if (!vi) { const uint32_t p_i = G.type(OpTypePointer, {SCInput, t_i}); vi = G.var(p_i, SCInput); new_vi = true; }
    const uint32_t t_rta = G.type(OpTypeRuntimeArray, {t_u}, false), t_sb = G.type(OpTypeStruct, {t_rta}, false);
    const uint32_t p_sb = G.type(OpTypePointer, {SCStorageBuffer, t_sb}), p_sbu = G.type(OpTypePointer, {SCStorageBuffer, t_u});
    const uint32_t ssbo = G.var(p_sb, SCStorageBuffer);
    const uint32_t c_m = G.konst(t_u, plan.m), c_i0 = G.konst(t_i, 0);
    std::vector<uint32_t> c_j(plan.m); for (uint32_t j = 0; j < plan.m; j++) c_j[j] = G.konst(t_u, j);
    // write
    W o; o.o.assign(words, words + 5);
    std::vector<uint32_t> iface; if (new_vi) iface.push_back(vi);
    uint32_t nocon = 0;
    for (size_t i = 0; i < m.fn; i++) {
        const uint32_t op = m.ins[i].op;
        if (i == m.ep) { entry_point(o, m, gone, iface); continue; }
        if (targets(m, i, gone)) continue;
        if (op == OpVariable && gone.count(m.at(i)[2])) continue;
        if (i == m.first_global) {
            if (new_vi) o.ins(OpDecorate, {vi, DecBuiltIn, BuiltInVertexIndex});
            o.ins(OpDecorate, {t_rta, DecArrayStride, 4}); o.ins(OpDecorate, {t_sb, DecBlock}); o.ins(OpMemberDecorate, {t_sb, 0, DecOffset, 0});
            o.ins(OpDecorate, {ssbo, DecDescriptorSet, 0}); o.ins(OpDecorate, {ssbo, DecBinding, plan.ssbo_binding});
            for (uint32_t id : a.slice) { const uint32_t *d = m.def_words(id); if (float_arith(d[0] & 0xffff) && inv_feed.count(id) && !m.nocontract.count(id)) { o.ins(OpDecorate, {id, DecNoContraction}); nocon++; } }
        }
        o.copy(m.at(i));
    }
    for (auto &v : G.made) o.o.insert(o.o.end(), v.begin(), v.end());
    for (size_t i = m.fn; i <= m.label; i++) o.copy(m.at(i));
    for (size_t i : a.body) { const uint32_t op = m.ins[i].op; if (op == OpLine || op == OpNoLine || op == OpVariable || op == OpStore) continue;
        if (a.slice.count(m.at(i)[2])) o.copy(m.at(i)); }
    // record: data[gl_VertexIndex * m + c] = the cut value's bits
    const uint32_t vl = bound++, vu = bound++, base = bound++;
    o.ins(OpLoad, {t_i, vl, vi}); o.ins(OpBitcast, {t_u, vu, vl}); o.ins(OpIMul, {t_u, base, vu, c_m});
    for (size_t v = 0; v < ids.size(); v++) {
        const uint32_t id = ids[v]; const uint32_t *d = m.def_words(id); uint32_t ct = 0, ctid = 0; const uint32_t c = m.comps(d[1], &ct, &ctid);
        for (uint32_t j = 0; j < c; j++) {
            const uint32_t rc = plan.vals[v].first + j; uint32_t x = id;
            if (c > 1) { x = bound++; o.ins(OpCompositeExtract, {ctid, x, id, j}); }
            if (ct != 2) { const uint32_t y = bound++; o.ins(OpBitcast, {t_u, y, x}); x = y; }
            uint32_t idx = base; if (rc) { idx = bound++; o.ins(OpIAdd, {t_u, idx, base, c_j[rc]}); }
            const uint32_t p = bound++; o.ins(OpAccessChain, {p_sbu, p, ssbo, c_i0, idx}); o.ins(OpStore, {p, x});
        }
    }
    for (size_t i = m.ret; i < m.ins.size(); i++) o.copy(m.at(i));
    o.o[3] = bound; out.swap(o.o);
    (void)nocon; return true;
}
