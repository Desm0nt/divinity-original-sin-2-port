// [DOS2][NR-32] DM_NR_XLQ: dead varying stores of translated vertex shaders (docs/dos2/nr/NR-32.md).
//
// Why: the Maxwell translator writes every attribute output the guest vertex program writes, as vec4 Location outputs; DOS2 pairs
// one vertex program with several fragment programs (G-buffer, depth pre-pass, shadow, forward), most of which read a few of those
// components (a depth pass reads the UV for its alpha test or nothing). Arm's compiler (malioc 2026.5) compiles the vertex stage alone,
// keeps every output and its whole computation: on Fort Joy's vertex shaders 40 % of the written components are never read and the
// varying stores are the load/store pipeline that bounds them. Turnip links the stages and removes such outputs itself.
//
// Exact by construction: only OpStore instructions to components no fragment input can see are removed (and output variables left
// with no reference at all leave the interface); every value the fragment shader can read is written by the same instruction as before.
#include "xbg_spirv_vsout.h"
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace {
enum : uint32_t {
    OpName = 5, OpMemberName = 6, OpEntryPoint = 15, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeMatrix = 24,
    OpTypeArray = 28, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpFunction = 54, OpVariable = 59, OpLoad = 61,
    OpStore = 62, OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71, OpMemberDecorate = 72,
    DecBuiltIn = 11, DecLocation = 30, DecComponent = 31, SCInput = 1, SCOutput = 3, ModelVertex = 0, ModelFragment = 4
};
struct Ins { size_t at; uint32_t op, n; };
struct Mod {
    const uint32_t *w = nullptr; size_t count = 0; bool ok = false;
    std::vector<Ins> ins; size_t entry = 0; uint32_t entries = 0, model = ~0u, entry_name_words = 0;
    std::unordered_map<uint32_t, uint32_t> loc, comp, cint;              // id -> Location / Component / 32-bit integer constant value
    std::unordered_set<uint32_t> builtin, block_types;
    struct Ty { uint32_t op = 0, a = 0, b = 0; };                        // vector: a = component type, b = count; array: a = elem, b = length id
    std::unordered_map<uint32_t, Ty> ty;
    std::unordered_map<uint32_t, uint32_t> ptr_pointee, ptr_sc;
    std::unordered_map<uint32_t, uint32_t> var_type, var_sc;             // global variables: pointer type, storage class
    bool parse(const uint32_t *words, size_t n) {
        w = words; count = n; if (n < 5 || words[0] != 0x07230203u) return false;
        for (size_t i = 5; i < n;) {
            const uint32_t op = w[i] & 0xffffu, c = w[i] >> 16; if (!c || i + c > n) return false;
            ins.push_back({i, op, c});
            switch (op) {
            case OpEntryPoint: if (c < 4) return false; entries++; entry = ins.size() - 1; model = w[i + 1];
                { size_t k = 3; while (i + k < i + c) { const uint32_t x = w[i + k]; k++; if (!((x & 0xff) && (x & 0xff00) && (x & 0xff0000) && (x & 0xff000000u))) break; } entry_name_words = uint32_t(k - 3); }
                break;
            case OpDecorate: if (c >= 3) { const uint32_t d = w[i + 2];
                    if (d == DecLocation && c >= 4) loc[w[i + 1]] = w[i + 3]; else if (d == DecComponent && c >= 4) comp[w[i + 1]] = w[i + 3];
                    else if (d == DecBuiltIn) builtin.insert(w[i + 1]); } break;
            case OpMemberDecorate: if (c >= 4 && w[i + 3] == DecBuiltIn) block_types.insert(w[i + 1]); break;
            case OpTypeInt: case OpTypeFloat: if (c >= 3) ty[w[i + 1]] = {op, w[i + 2], 0}; break;
            case OpTypeVector: case OpTypeMatrix: case OpTypeArray: if (c >= 4) ty[w[i + 1]] = {op, w[i + 2], w[i + 3]}; break;
            case OpTypeStruct: ty[w[i + 1]] = {op, 0, 0}; break;
            case OpTypePointer: if (c >= 4) { ptr_sc[w[i + 1]] = w[i + 2]; ptr_pointee[w[i + 1]] = w[i + 3]; } break;
            case OpConstant: if (c == 4) cint[w[i + 2]] = w[i + 3]; break;
            case OpVariable: if (c >= 4 && w[i + 3] != 7) { var_type[w[i + 2]] = w[i + 1]; var_sc[w[i + 2]] = w[i + 3]; } break;
            default: break;
            }
            i += c;
        }
        ok = entries == 1; return ok;
    }
    // Components of a 32-bit scalar / vector type; 0 = another type
    uint32_t comps(uint32_t t) const {
        auto it = ty.find(t); if (it == ty.end()) return 0;
        if ((it->second.op == OpTypeFloat || it->second.op == OpTypeInt) && it->second.a == 32) return 1;
        if (it->second.op == OpTypeVector && it->second.b >= 1 && it->second.b <= 4 && comps(it->second.a) == 1) return it->second.b;
        return 0;
    }
    // A user varying: a Location variable of the storage class; first = Location, width = components, alen = array length (0 = no array)
    struct Var { uint32_t loc = 0, first = 0, width = 0, alen = 0; };
    bool varying(uint32_t id, uint32_t sc, Var &v, bool &unsupported) const {
        unsupported = false;
        auto s = var_sc.find(id); if (s == var_sc.end() || s->second != sc) return false;
        if (builtin.count(id)) return false;
        const uint32_t pointee = ptr_pointee.count(var_type.at(id)) ? ptr_pointee.at(var_type.at(id)) : 0;
        if (block_types.count(pointee)) return false;                                   // gl_PerVertex
        auto l = loc.find(id); if (l == loc.end()) { unsupported = true; return true; }   // a Location-less user block / struct
        v.loc = l->second; v.first = comp.count(id) ? comp.at(id) : 0; v.width = comps(pointee); v.alen = 0;
        if (!v.width) {
            auto t = ty.find(pointee);
            if (t != ty.end() && t->second.op == OpTypeArray && comps(t->second.a) && cint.count(t->second.b)) { v.width = comps(t->second.a); v.alen = cint.at(t->second.b); }
            else { unsupported = true; return true; }
        }
        if (v.first + v.width > 4 || v.loc + (v.alen ? v.alen : 1) > 16) unsupported = true;
        return true;
    }
};
uint64_t bits(uint32_t loc, uint32_t first, uint32_t width) { return width ? (((uint64_t(1) << width) - 1) << first) << (4 * loc) : 0; }
uint64_t whole(const Mod::Var &v) { uint64_t m = 0; for (uint32_t k = 0; k < (v.alen ? v.alen : 1); k++) m |= bits(v.loc + k, v.first, v.width); return m; }
// Ids referenced by an instruction, as words after the opcode; literals may look like ids too, which only makes an answer
// more conservative (a variable "used" by a literal is pinned / read in full).
// Types and plain constants cannot refer to a variable (their literals, e.g. OpTypeFloat's width 32, would otherwise look like ids).
bool declares_only(uint32_t op) { return (op >= 19 && op <= 39) || (op >= 41 && op <= 44) || op == 46; }
// [NRVS] Word k (1 = the first after the opcode word) of an instruction that the SPIR-V grammar makes a literal, never an id. Without
// this, OpExtInst's instruction number (GLSL.std.450 InverseSqrt = 32, FMax = 40, ...) "mentioned" the output variables %32 / %40 and
// pinned them: on the player's pack about half of the vertex shaders kept Location stores a fragment shader that reads nothing never
// sees (tests/nrvs_equiv_test.cpp). Only literal positions are listed; anything else still counts (conservative).
bool literal_word(uint32_t op, size_t k) {
    switch (op) {
    case 3: return k != 3;                          // OpSource (word 3: the optional file id)
    case 8: case 16: return k >= 2;                 // OpLine, OpExecutionMode
    case 10: case 14: case 17: return true;         // OpExtension, OpMemoryModel, OpCapability
    case 11: return k >= 2;                         // OpExtInstImport name
    case 12: return k == 4;                         // OpExtInst instruction number
    case 79: return k >= 5;                         // OpVectorShuffle components
    case 81: return k >= 4;                         // OpCompositeExtract indexes
    case 82: return k >= 5;                         // OpCompositeInsert indexes
    case 246: return k >= 3;                        // OpLoopMerge control (+ parameters)
    case 247: return k == 2;                        // OpSelectionMerge control
    case 250: return k >= 4;                        // OpBranchConditional weights
    case 251: return k >= 3 && !((k - 3) & 1);      // OpSwitch case literals (32-bit selectors)
    default: return false;
    }
}
bool mentions(const uint32_t *w, const Ins &x, uint32_t id, size_t from = 1) {
    if (declares_only(x.op)) return false; for (size_t k = from; k < x.n; k++) if (w[x.at + k] == id && !literal_word(x.op, k)) return true; return false; }
}

uint64_t xbg_spirv_fs_input_mask(const uint32_t *words, size_t count, uint64_t *declared) {
    if (declared) *declared = XBG_VSOUT_ALL;
    Mod m; if (!m.parse(words, count) || m.model != ModelFragment) return XBG_VSOUT_ALL;
    std::unordered_map<uint32_t, Mod::Var> vars;
    for (auto &kv : m.var_sc) { Mod::Var v; bool bad = false; if (m.varying(kv.first, SCInput, v, bad)) { if (bad) return XBG_VSOUT_ALL; vars[kv.first] = v; } }
    uint64_t mask = 0;
    if (declared) { *declared = 0; for (auto &kv : vars) *declared |= whole(kv.second); }
    for (const Ins &x : m.ins) {
        if (x.op == OpEntryPoint || x.op == OpDecorate || x.op == OpMemberDecorate || x.op == OpName || x.op == OpMemberName || x.op == OpVariable) continue;
        if ((x.op == OpAccessChain || x.op == OpInBoundsAccessChain) && x.n >= 4) {
            auto it = vars.find(words[x.at + 3]);
            if (it != vars.end()) {
                const Mod::Var &v = it->second; const size_t ni = x.n - 4; const uint32_t *idx = words + x.at + 4;
                uint64_t sel = whole(v);
                if (!v.alen && ni == 1 && m.cint.count(idx[0]) && m.cint.at(idx[0]) < v.width) sel = bits(v.loc, v.first + m.cint.at(idx[0]), 1);
                else if (v.alen && ni >= 1 && m.cint.count(idx[0]) && m.cint.at(idx[0]) < v.alen) {
                    const uint32_t l = v.loc + m.cint.at(idx[0]);
                    sel = bits(l, v.first, v.width);
                    if (ni == 2 && m.cint.count(idx[1]) && m.cint.at(idx[1]) < v.width) sel = bits(l, v.first + m.cint.at(idx[1]), 1);
                }
                mask |= sel;
                for (size_t k = 4; k < x.n; k++) if (vars.count(words[x.at + k])) mask |= whole(vars.at(words[x.at + k]));
                continue;
            }
        }
        for (auto &kv : vars) if (mentions(words, x, kv.first)) mask |= whole(kv.second);
    }
    return mask;
}

namespace {
// The trim's view of a vertex module: per output variable its stores and chains, or pinned.
struct Plan {
    std::unordered_map<uint32_t, Mod::Var> outs;          // Location outputs the pass handles
    std::unordered_set<uint32_t> pinned;                  // of them, left alone
    std::unordered_map<uint32_t, std::pair<uint32_t, uint64_t>> chain;   // chain id -> (variable, component bits)
    bool unsupported = false;
};
bool plan(const Mod &m, Plan &p) {
    for (auto &kv : m.var_sc) { Mod::Var v; bool bad = false; if (m.varying(kv.first, SCOutput, v, bad)) { if (bad || v.alen) p.pinned.insert(kv.first); p.outs[kv.first] = v; } }
    for (const Ins &x : m.ins) {
        if (x.op == OpEntryPoint || x.op == OpDecorate || x.op == OpMemberDecorate || x.op == OpName || x.op == OpMemberName || x.op == OpVariable) continue;
        const uint32_t *w = m.w + x.at;
        if ((x.op == OpAccessChain || x.op == OpInBoundsAccessChain) && x.n >= 4 && p.outs.count(w[3])) {
            const Mod::Var &v = p.outs.at(w[3]);
            if (x.n == 5 && m.cint.count(w[4]) && m.cint.at(w[4]) < v.width) p.chain[w[2]] = {w[3], bits(v.loc, v.first + m.cint.at(w[4]), 1)};
            else p.pinned.insert(w[3]);
            for (size_t k = 4; k < x.n; k++) if (p.outs.count(w[k])) p.pinned.insert(w[k]);
            continue;
        }
        if (x.op == OpStore && x.n >= 3) {
            // pointer operand: a variable or a chain; anything else mentioned (the value, memory operands) pins
            for (size_t k = 2; k < x.n; k++) { if (p.outs.count(w[k])) p.pinned.insert(w[k]); auto c = p.chain.find(w[k]); if (c != p.chain.end()) p.pinned.insert(c->second.first); }
            continue;
        }
        for (auto &kv : p.outs) if (mentions(m.w, x, kv.first)) p.pinned.insert(kv.first);
        for (auto &kv : p.chain) if (mentions(m.w, x, kv.first)) p.pinned.insert(kv.second.first);
    }
    return true;
}
}

uint64_t xbg_spirv_vs_output_mask(const uint32_t *words, size_t count) {
    Mod m; if (!m.parse(words, count) || m.model != ModelVertex) return 0;
    Plan p; plan(m, p); uint64_t mask = 0;
    for (auto &kv : p.outs) { if (p.pinned.count(kv.first)) { if (kv.second.width && !kv.second.alen && kv.second.loc < 16) mask |= whole(kv.second); else return XBG_VSOUT_ALL; } }
    for (const Ins &x : m.ins) {
        if (x.op != OpStore || x.n < 3) continue; const uint32_t ptr = words[x.at + 1];
        auto c = p.chain.find(ptr); if (c != p.chain.end()) { mask |= c->second.second; continue; }
        auto v = p.outs.find(ptr); if (v != p.outs.end()) mask |= whole(v->second);
    }
    return mask;
}

size_t xbg_spirv_vsout_trim(const uint32_t *words, size_t count, uint64_t keep, std::vector<uint32_t> &out, XbgVsoutStats *stats, uint64_t declared) {
    out.clear(); XbgVsoutStats st; Mod m; if (!m.parse(words, count) || m.model != ModelVertex) return 0;
    Plan p; plan(m, p); st.pinned = uint32_t(p.pinned.size());
    std::vector<char> drop(m.ins.size(), 0); std::unordered_map<uint32_t, uint32_t> refs;   // chain / variable -> references left
    for (size_t i = 0; i < m.ins.size(); i++) {
        const Ins &x = m.ins[i]; if (x.op != OpStore || x.n < 3) continue; const uint32_t ptr = words[x.at + 1];
        uint64_t sel = 0; uint32_t var = 0;
        auto c = p.chain.find(ptr); if (c != p.chain.end()) { sel = c->second.second; var = c->second.first; }
        else if (p.outs.count(ptr)) { var = ptr; sel = whole(p.outs.at(ptr)); }
        if (!var || p.pinned.count(var) || (sel & keep)) continue;
        drop[i] = 1; st.stores_dropped++;
    }
    if (!st.stores_dropped) return 0;
    // Chains / variables still referenced by a kept instruction
    for (size_t i = 0; i < m.ins.size(); i++) {
        const Ins &x = m.ins[i]; if (drop[i] || x.op == OpEntryPoint || x.op == OpDecorate || x.op == OpName || declares_only(x.op)) continue;
        const uint32_t *w = words + x.at;
        if ((x.op == OpAccessChain || x.op == OpInBoundsAccessChain) && x.n >= 4 && p.chain.count(w[2])) continue;   // counted through its uses
        for (size_t k = 1; k < x.n; k++) { if (x.op == OpVariable && k == 2) continue; refs[w[k]]++; }
    }
    std::unordered_set<uint32_t> live_vars, dead_vars;
    for (size_t i = 0; i < m.ins.size(); i++) {
        const Ins &x = m.ins[i]; const uint32_t *w = words + x.at;
        if ((x.op == OpAccessChain || x.op == OpInBoundsAccessChain) && x.n >= 4 && p.chain.count(w[2])) {
            if (refs.count(w[2])) live_vars.insert(w[3]); else { drop[i] = 1; st.chains_dropped++; }
        }
    }
    for (auto &kv : p.outs) if (!p.pinned.count(kv.first) && !live_vars.count(kv.first) && !refs.count(kv.first) && !(whole(kv.second) & declared)) dead_vars.insert(kv.first);
    for (size_t i = 0; i < m.ins.size(); i++) {
        const Ins &x = m.ins[i]; const uint32_t *w = words + x.at;
        if ((x.op == OpDecorate || x.op == OpName) && x.n >= 2 && dead_vars.count(w[1])) drop[i] = 1;
        if (x.op == OpVariable && x.n >= 4 && dead_vars.count(w[2])) { drop[i] = 1; st.vars_dropped++; }
    }
    out.assign(words, words + 5);
    for (size_t i = 0; i < m.ins.size(); i++) {
        const Ins &x = m.ins[i]; if (drop[i]) continue;
        if (x.op == OpEntryPoint && !dead_vars.empty()) {
            const size_t head = 3 + m.entry_name_words; std::vector<uint32_t> e(words + x.at, words + x.at + head);
            for (size_t k = head; k < x.n; k++) if (!dead_vars.count(words[x.at + k])) e.push_back(words[x.at + k]);
            e[0] = (uint32_t(e.size()) << 16) | OpEntryPoint; out.insert(out.end(), e.begin(), e.end()); continue;
        }
        out.insert(out.end(), words + x.at, words + x.at + x.n);
    }
    if (stats) *stats = st;
    return st.stores_dropped;
}

const char *xbg_spirv_vsout_check(const uint32_t *in, size_t in_count, const uint32_t *out, size_t out_count, uint64_t keep) {
    Mod a, b; if (!a.parse(in, in_count) || a.model != ModelVertex) return "input is not a single-entry vertex module";
    if (!b.parse(out, out_count)) return "output does not parse";
    if (memcmp(in, out, 5 * 4)) return "header differs";
    // Output Location variables of the input and the component(s) each one-index chain selects
    std::unordered_map<uint32_t, Mod::Var> outs; for (auto &kv : a.var_sc) { Mod::Var v; bool bad = false; if (a.varying(kv.first, SCOutput, v, bad) && !bad && !v.alen) outs[kv.first] = v; }
    std::unordered_map<uint32_t, std::pair<uint32_t, uint64_t>> chains;
    for (const Ins &x : a.ins) { const uint32_t *w = in + x.at;
        if ((x.op == OpAccessChain || x.op == OpInBoundsAccessChain) && x.n == 5 && outs.count(w[3]) && a.cint.count(w[4]) && a.cint.at(w[4]) < outs.at(w[3]).width)
            chains[w[2]] = {w[3], bits(outs.at(w[3]).loc, outs.at(w[3]).first + a.cint.at(w[4]), 1)}; }
    // Every id the output mentions (outside names / decorations / the interface list)
    std::unordered_set<uint32_t> used;
    for (const Ins &x : b.ins) { if (x.op == OpEntryPoint || x.op == OpDecorate || x.op == OpName || declares_only(x.op)) continue; for (size_t k = 1; k < x.n; k++) if (!(x.op == OpVariable && k == 2)) used.insert(out[x.at + k]); }
    size_t j = 0, removed_vars = 0; std::unordered_set<uint32_t> gone_vars;
    for (size_t i = 0; i < a.ins.size(); i++) {
        const Ins &x = a.ins[i]; const uint32_t *w = in + x.at;
        if (j < b.ins.size()) { const Ins &y = b.ins[j];
            if (x.op == OpEntryPoint && y.op == OpEntryPoint) { j++; continue; }   // compared below
            if (x.n == y.n && !memcmp(w, out + y.at, x.n * 4)) { j++; continue; } }
        // x was removed: it must be one of the allowed forms
        if (x.op == OpStore && x.n >= 3) {
            uint64_t sel = 0; auto c = chains.find(w[1]); if (c != chains.end()) sel = c->second.second; else if (outs.count(w[1])) sel = whole(outs.at(w[1])); else return "a removed store does not write a Location output";
            if (sel & keep) return "a removed store writes a kept component";
            continue;
        }
        if ((x.op == OpAccessChain || x.op == OpInBoundsAccessChain) && x.n >= 4 && outs.count(w[3])) { if (used.count(w[2])) return "a removed chain is still used"; continue; }
        if ((x.op == OpVariable && x.n >= 4 && outs.count(w[2])) || ((x.op == OpDecorate || x.op == OpName) && x.n >= 2 && outs.count(w[1]))) {
            const uint32_t id = x.op == OpVariable ? w[2] : w[1]; if (used.count(id)) return "a removed output variable is still referenced";
            if (x.op == OpVariable) { removed_vars++; gone_vars.insert(id); } continue;
        }
        return "an instruction other than an output store / chain / variable was removed or changed";
    }
    if (j != b.ins.size()) return "the output has instructions the input does not";
    // The entry point: the same but for removed output variables
    const Ins &ea = a.ins[a.entry], &eb = b.ins[b.entry]; const size_t head = 3 + a.entry_name_words;
    if (eb.n < head || memcmp(in + ea.at + 1, out + eb.at + 1, (head - 1) * 4)) return "entry point head differs";
    size_t k2 = head;
    for (size_t k = head; k < ea.n; k++) { const uint32_t id = in[ea.at + k]; if (k2 < eb.n && out[eb.at + k2] == id) { k2++; continue; } if (!gone_vars.count(id)) return "an interface id other than a removed output left the entry point"; }
    if (k2 != eb.n) return "entry point interface has extra ids";
    for (uint32_t id : gone_vars) for (size_t k = head; k < eb.n; k++) if (out[eb.at + k] == id) return "a removed variable stays on the interface";
    (void)removed_vars; return "";
}
