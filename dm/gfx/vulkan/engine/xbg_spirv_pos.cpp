// [DOS2] NR-10b DM_NR_COVER (see xbg_spirv_pos.h): symbolic walk of a one-block vertex shader. Every float value the walk understands is
// an instruction of a growing list (leaves are shared); Function / Private variables and the position output hold the value last stored
// per component. At the end the values the four position components depend on are renumbered into the nr_cv_prog.
#include "xbg_spirv_pos.h"
#include <map>
#include <vector>

namespace {
enum : uint32_t {
    OpExtInstImport = 11, OpExtInst = 12, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeMatrix = 24, OpTypeArray = 28,
    OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpConstantComposite = 44, OpConstantNull = 46, OpFunction = 54,
    OpFunctionCall = 57, OpVariable = 59, OpLoad = 61, OpStore = 62, OpCopyMemory = 63, OpCopyMemorySized = 64, OpAccessChain = 65,
    OpInBoundsAccessChain = 66, OpDecorate = 71, OpMemberDecorate = 72, OpVectorShuffle = 79, OpCompositeConstruct = 80,
    OpCompositeExtract = 81, OpCompositeInsert = 82, OpCopyObject = 83, OpFNegate = 127, OpFAdd = 129, OpFSub = 131, OpFMul = 133,
    OpFDiv = 136, OpVectorTimesScalar = 142, OpLabel = 248, OpKill = 252,
    DecBlock = 2, DecArrayStride = 6, DecBuiltIn = 11, DecLocation = 30, DecComponent = 31, DecBinding = 33, DecDescriptorSet = 34,
    DecOffset = 35, BuiltInPosition = 0, BuiltInClipDistance = 3, BuiltInCullDistance = 4,
    ScInput = 1, ScUniform = 2, ScOutput = 3, ScPrivate = 6, ScFunction = 7,
    GlslFAbs = 4, GlslFMin = 37, GlslFMax = 40, GlslFma = 50,
};
struct Type { uint32_t kind = 0, elem = 0, n = 0; std::vector<uint32_t> members; };   // kind: the OpType* opcode (float / int: 32 bit only)
struct Vec { int n = 0; int c[4] = {-1, -1, -1, -1}; };                               // a float scalar (n = 1) or vector value / cell
struct Chain { uint32_t base = 0; std::vector<uint32_t> idx; bool bad = false; };     // constant indices, base = a variable
}

// [P10] target < 0: gl_Position (four components). target >= 0: the Output variable decorated Location target (no BuiltIn, no Component
// decoration; a float vector of 2..4 components): out[0], out[1] = its x, y; out[2], out[3] = its z, w where the variable has them and
// they are followed, else a copy of out[0]. ClipDistance writes do not matter to a varying.
static void out_prog(const uint32_t *in, size_t n, int target, nr_cv_prog *pr, const char **why) {
    const char *dummy; if (!why) why = &dummy;
    memset(pr, 0, sizeof *pr); *why = "module";
    if (!in || n < 5 || in[0] != 0x07230203u) return;
    const uint32_t bound = in[3];
    if (bound > (1u << 22)) return;
    std::vector<Type> type(bound);
    std::vector<int> loc(bound, -1), builtin(bound, -1), binding(bound, -1), set(bound, 0), var_sc(bound, -1), stride(bound, -1);
    std::vector<uint8_t> block(bound, 0), has_comp(bound, 0), is_iconst(bound, 0), glsl(bound, 0);
    std::vector<uint32_t> var_type(bound, 0), iconst(bound, 0);
    std::map<uint64_t, uint32_t> member_off;                      // (struct id << 32 | member) -> Offset
    std::map<uint64_t, int> member_builtin;
    std::vector<Vec> val(bound);                                  // SSA values
    std::vector<nr_cv_ins> ins; std::map<uint64_t, int> leaf;
    auto mk_leaf = [&](uint8_t op, uint32_t a, uint32_t b, float k) -> int {
        uint32_t kb; memcpy(&kb, &k, 4);
        const uint64_t key = (uint64_t(op) << 56) | (op == NR_CV_CONST ? uint64_t(kb) : (uint64_t(a & 0xffffu) << 32 | uint64_t(b & 0xffffu) << 16));
        auto f = leaf.find(key); if (f != leaf.end()) return f->second;
        if (ins.size() >= 60000) return -1;
        nr_cv_ins x{}; x.op = op; x.a = uint16_t(a); x.b = uint16_t(b); x.k = k; ins.push_back(x); leaf[key] = int(ins.size() - 1); return int(ins.size() - 1);
    };
    auto mk = [&](uint8_t op, int a, int b = 0, int c = 0) -> int {
        if (a < 0 || b < 0 || c < 0 || ins.size() >= 60000) return -1;
        nr_cv_ins x{}; x.op = op; x.a = uint16_t(a); x.b = uint16_t(b); x.c = uint16_t(c); ins.push_back(x); return int(ins.size() - 1);
    };
    size_t first_func = 0, functions = 0, labels = 0;
    for (size_t i = 5; i < n;) {
        const uint32_t wc = in[i] >> 16, op = in[i] & 0xffff; const uint32_t *a = in + i + 1;
        if (!wc || i + wc > n) return;
        auto id_ok = [&](uint32_t x) { return x < bound; };
        switch (op) {
        case OpExtInstImport: if (wc >= 3 && id_ok(a[0])) glsl[a[0]] = !strncmp(reinterpret_cast<const char *>(a + 1), "GLSL.std.450", (wc - 2) * 4); break;
        case OpDecorate: if (wc >= 3 && id_ok(a[0])) {
            if (a[1] == DecBlock) block[a[0]] = 1;
            if (a[1] == DecComponent) has_comp[a[0]] = 1;
            if (wc >= 4) { if (a[1] == DecLocation) loc[a[0]] = int(a[2]); if (a[1] == DecBuiltIn) builtin[a[0]] = int(a[2]); if (a[1] == DecBinding) binding[a[0]] = int(a[2]);
                           if (a[1] == DecDescriptorSet) set[a[0]] = int(a[2]);
                           if (a[1] == DecArrayStride) stride[a[0]] = int(a[2]); } } break;
        case OpMemberDecorate: if (wc >= 5 && id_ok(a[0])) { const uint64_t k = uint64_t(a[0]) << 32 | a[1]; if (a[2] == DecOffset) member_off[k] = a[3]; if (a[2] == DecBuiltIn) member_builtin[k] = int(a[3]); } break;
        case OpTypeFloat: if (wc == 3 && id_ok(a[0]) && a[1] == 32) type[a[0]].kind = OpTypeFloat; break;
        case OpTypeInt: if (wc == 4 && id_ok(a[0]) && a[1] == 32) type[a[0]].kind = OpTypeInt; break;
        case OpTypeVector: if (wc == 4 && id_ok(a[0]) && id_ok(a[1])) { type[a[0]].kind = OpTypeVector; type[a[0]].elem = a[1]; type[a[0]].n = a[2]; } break;
        case OpTypeMatrix: if (wc == 4 && id_ok(a[0])) type[a[0]].kind = OpTypeMatrix; break;
        case OpTypeArray: if (wc == 4 && id_ok(a[0]) && id_ok(a[1]) && id_ok(a[2])) { type[a[0]].kind = OpTypeArray; type[a[0]].elem = a[1]; type[a[0]].n = is_iconst[a[2]] ? iconst[a[2]] : 0; } break;
        case OpTypeStruct: if (wc >= 2 && id_ok(a[0])) { type[a[0]].kind = OpTypeStruct; type[a[0]].members.assign(a + 1, a + wc - 1); } break;
        case OpTypePointer: if (wc == 4 && id_ok(a[0]) && id_ok(a[2])) { type[a[0]].kind = OpTypePointer; type[a[0]].elem = a[2]; } break;
        case OpConstant: if (wc == 4 && id_ok(a[0]) && id_ok(a[1])) {
            if (type[a[0]].kind == OpTypeInt) { is_iconst[a[1]] = 1; iconst[a[1]] = a[2]; }
            if (type[a[0]].kind == OpTypeFloat) { float k; memcpy(&k, &a[2], 4); val[a[1]].n = 1; val[a[1]].c[0] = mk_leaf(NR_CV_CONST, 0, 0, k); } } break;
        case OpConstantNull: if (wc == 3 && id_ok(a[0]) && id_ok(a[1])) {
            const Type &t = type[a[0]];
            if (t.kind == OpTypeInt) { is_iconst[a[1]] = 1; iconst[a[1]] = 0; }
            if (t.kind == OpTypeFloat) { val[a[1]].n = 1; val[a[1]].c[0] = mk_leaf(NR_CV_CONST, 0, 0, 0.f); }
            if (t.kind == OpTypeVector && t.n <= 4 && id_ok(t.elem) && type[t.elem].kind == OpTypeFloat) { val[a[1]].n = int(t.n); for (uint32_t c = 0; c < t.n; c++) val[a[1]].c[c] = mk_leaf(NR_CV_CONST, 0, 0, 0.f); } } break;
        case OpConstantComposite: if (wc >= 3 && id_ok(a[0]) && id_ok(a[1])) {
            const Type &t = type[a[0]];
            if (t.kind == OpTypeVector && t.n <= 4 && wc == 3 + t.n && id_ok(t.elem) && type[t.elem].kind == OpTypeFloat) { val[a[1]].n = int(t.n); for (uint32_t c = 0; c < t.n; c++) val[a[1]].c[c] = id_ok(a[2 + c]) && val[a[2 + c]].n == 1 ? val[a[2 + c]].c[0] : -1; } } break;
        case OpVariable: if (wc >= 4 && id_ok(a[0]) && id_ok(a[1])) { var_sc[a[1]] = int(a[2]); var_type[a[1]] = type[a[0]].elem; } break;
        case OpFunction: ++functions; if (!first_func) first_func = i; break;
        case OpLabel: ++labels; break;
        case OpFunctionCall: case OpCopyMemory: case OpCopyMemorySized: case OpKill: *why = "call / copy / kill"; return;
        default: break;
        }
        i += wc;
    }
    if (functions != 1 || labels != 1) { *why = "more than one block"; return; }
    // The position: an Output variable that is BuiltIn Position, or an Output block with such a member.
    int pvar = -1, pmem = -1; std::vector<uint8_t> clipvar(bound, 0);
    int pn = 4;   // components of the target
    if (target >= 0) {
        for (uint32_t v = 0; v < bound; ++v) if (var_sc[v] == int(ScOutput) && loc[v] == target && builtin[v] < 0) {
            if (pvar >= 0 || has_comp[v]) { *why = "two outputs at the location / a Component decoration"; return; }
            pvar = int(v);
        }
        if (pvar < 0) { *why = "no output at the location"; return; }
        const uint32_t t = var_type[pvar];
        if (t >= bound || type[t].kind != OpTypeVector || type[t].n < 2 || type[t].n > 4 || type[t].elem >= bound || type[type[t].elem].kind != OpTypeFloat) { *why = "the output is not a float vector"; return; }
        pn = int(type[t].n);
    } else
    for (uint32_t v = 0; v < bound; ++v) if (var_sc[v] == int(ScOutput)) {
        if (builtin[v] == int(BuiltInPosition)) { if (pvar >= 0) { *why = "two positions"; return; } pvar = int(v); pmem = -1; }
        if (builtin[v] == int(BuiltInClipDistance) || builtin[v] == int(BuiltInCullDistance)) clipvar[v] = 1;
        const uint32_t t = var_type[v];
        if (t < bound && type[t].kind == OpTypeStruct) for (uint32_t m = 0; m < type[t].members.size(); m++) {
            auto f = member_builtin.find(uint64_t(t) << 32 | m); if (f == member_builtin.end()) continue;
            if (f->second == int(BuiltInPosition)) { if (pvar >= 0) { *why = "two positions"; return; } pvar = int(v); pmem = int(m); }
        }
    }
    if (pvar < 0) { *why = "no position output"; return; }
    auto is_clip = [&](uint32_t v, const std::vector<uint32_t> &idx) {
        if (target >= 0) return false;
        if (clipvar[v]) return true;
        const uint32_t t = var_type[v];
        if (t < bound && type[t].kind == OpTypeStruct) {
            bool has = false;
            for (uint32_t m = 0; m < type[t].members.size(); m++) { auto f = member_builtin.find(uint64_t(t) << 32 | m);
                if (f != member_builtin.end() && (f->second == int(BuiltInClipDistance) || f->second == int(BuiltInCullDistance))) { if (idx.empty() || idx[0] == m) has = true; } }
            return has;   // a store to that member, or a whole-struct store of a struct that has one
        }
        return false;
    };
    std::vector<Chain> chain(bound); std::vector<uint8_t> is_chain(bound, 0);
    std::map<uint32_t, Vec> cell;        // Function / Private variables of float / float-vector type: the value stored last
    Vec pos; pos.n = pn;
    auto fvec = [&](uint32_t t) -> int { if (t >= bound) return 0; if (type[t].kind == OpTypeFloat) return 1;
        if (type[t].kind == OpTypeVector && type[t].n >= 2 && type[t].n <= 4 && type[t].elem < bound && type[type[t].elem].kind == OpTypeFloat) return int(type[t].n);
        return 0; };
    auto resolve = [&](uint32_t p, Chain &c) { if (p < bound && is_chain[p]) c = chain[p]; else { c = Chain{}; c.base = p; } };
    // Load through chain c: the value (n = 0: unknown).
    auto load = [&](const Chain &c) -> Vec {
        Vec r; if (c.bad || c.base >= bound) return r;
        const uint32_t v = c.base; const int sc = var_sc[v]; const uint32_t t = var_type[v];
        if (sc == int(ScFunction) || sc == int(ScPrivate)) {
            auto f = cell.find(v); if (f == cell.end()) return r;
            if (c.idx.empty()) return f->second;
            if (c.idx.size() == 1 && int(c.idx[0]) < f->second.n) { r.n = 1; r.c[0] = f->second.c[c.idx[0]]; }
            return r;
        }
        if (sc == int(ScInput)) {
            const int nc = fvec(t); if (!nc || loc[v] < 0 || loc[v] >= 32 || has_comp[v]) return r;
            if (c.idx.empty()) { r.n = nc; for (int k = 0; k < nc; k++) r.c[k] = mk_leaf(NR_CV_INPUT, uint32_t(loc[v]), uint32_t(k), 0); }
            else if (c.idx.size() == 1 && int(c.idx[0]) < nc && nc > 1) { r.n = 1; r.c[0] = mk_leaf(NR_CV_INPUT, uint32_t(loc[v]), c.idx[0], 0); }
            return r;
        }
        if (sc == int(ScOutput) && int(v) == pvar) {
            std::vector<uint32_t> idx = c.idx;
            if (pmem >= 0) { if (idx.empty() || int(idx[0]) != pmem) return r; idx.erase(idx.begin()); }
            if (idx.empty()) return pos;
            if (idx.size() == 1 && int(idx[0]) < pn) { r.n = 1; r.c[0] = pos.c[idx[0]]; }
            return r;
        }
        if (sc == int(ScUniform)) {
            if (t >= bound || type[t].kind != OpTypeStruct || !block[t] || binding[v] < 0 || binding[v] > 65535 || set[v] != 0) return r;
            uint32_t cur = t; uint64_t off = 0;
            for (uint32_t x : c.idx) {
                const Type &ty = type[cur];
                if (ty.kind == OpTypeStruct) { if (x >= ty.members.size()) return r; auto f = member_off.find(uint64_t(cur) << 32 | x); if (f == member_off.end()) return r; off += f->second; cur = ty.members[x]; }
                else if (ty.kind == OpTypeArray) { if (stride[cur] <= 0 || (ty.n && x >= ty.n)) return r; off += uint64_t(stride[cur]) * x; cur = ty.elem; }
                else if (ty.kind == OpTypeVector && ty.elem < bound && type[ty.elem].kind == OpTypeFloat) { if (x >= ty.n) return r; off += 4ull * x; cur = ty.elem; }
                else return r;   // matrices and anything else: not followed
                if (cur >= bound || off > 65536) return r;
            }
            const int nc = fvec(cur); if (!nc || off + 4ull * unsigned(nc) > 65536) return r;
            r.n = nc; for (int k = 0; k < nc; k++) r.c[k] = mk_leaf(NR_CV_UBO, uint32_t(binding[v]), uint32_t(off) + 4u * unsigned(k), 0);
            return r;
        }
        return r;
    };
    *why = "position write not followed";
    for (size_t i = first_func; i < n;) {
        const uint32_t wc = in[i] >> 16, op = in[i] & 0xffff; const uint32_t *a = in + i + 1;
        auto V = [&](uint32_t id) -> Vec { return id < bound ? val[id] : Vec{}; };
        auto S = [&](uint32_t id) -> int { return id < bound && val[id].n == 1 ? val[id].c[0] : -1; };
        auto rtype_n = [&]() { return fvec(a[0]); };   // the result type as a float scalar / vector (0: neither)
        switch (op) {
        case OpAccessChain: case OpInBoundsAccessChain: if (wc >= 4 && a[1] < bound) {
            Chain c; resolve(a[2], c);
            for (uint32_t k = 3; k + 1 < wc; k++) { if (a[k] < bound && is_iconst[a[k]]) c.idx.push_back(iconst[a[k]]); else c.bad = true; }
            chain[a[1]] = c; is_chain[a[1]] = 1; } break;
        case OpCopyObject: if (wc == 4 && a[1] < bound && a[2] < bound) { val[a[1]] = val[a[2]]; if (is_chain[a[2]] || var_sc[a[2]] >= 0) { resolve(a[2], chain[a[1]]); is_chain[a[1]] = 1; } } break;
        case OpLoad: if (wc >= 4 && a[1] < bound) { Chain c; resolve(a[2], c); Vec r = load(c); if (r.n && r.n == rtype_n()) val[a[1]] = r; } break;
        case OpStore: if (wc >= 3) {
            Chain c; resolve(a[0], c); if (c.base >= bound || var_sc[c.base] < 0) { *why = "store through a pointer that is not a variable"; return; }
            const uint32_t v = c.base; const int sc = var_sc[v]; const Vec x = V(a[1]);
            if (sc == int(ScOutput) && is_clip(v, c.idx)) { *why = "ClipDistance / CullDistance written"; return; }
            if (sc == int(ScFunction) || sc == int(ScPrivate)) {
                const int nc = fvec(var_type[v]);
                if (!nc) break;   // not a float cell: never loaded as one
                if (c.bad) { cell.erase(v); break; }
                if (c.idx.empty()) { if (x.n == nc) cell[v] = x; else cell.erase(v); }
                else if (c.idx.size() == 1 && int(c.idx[0]) < nc && nc > 1) { Vec &d = cell[v]; d.n = nc; d.c[c.idx[0]] = x.n == 1 ? x.c[0] : -1; }
                else cell.erase(v);
            } else if (sc == int(ScOutput) && int(v) == pvar) {
                if (c.bad) return;
                std::vector<uint32_t> idx = c.idx;
                if (pmem >= 0) { if (idx.empty()) return; if (int(idx[0]) != pmem) break; idx.erase(idx.begin()); }
                if (idx.empty()) { for (int k = 0; k < pn; k++) pos.c[k] = x.n == pn ? x.c[k] : -1; }
                else if (idx.size() == 1 && int(idx[0]) < pn) pos.c[idx[0]] = x.n == 1 ? x.c[0] : -1;
                else return;
            } } break;
        case OpFNegate: if (wc == 4 && a[1] < bound) { Vec x = V(a[2]), r; r.n = x.n && x.n == rtype_n() ? x.n : 0; for (int k = 0; k < r.n; k++) r.c[k] = mk(NR_CV_NEG, x.c[k]); val[a[1]] = r; } break;
        case OpFAdd: case OpFSub: case OpFMul: case OpFDiv: if (wc == 5 && a[1] < bound) {
            Vec x = V(a[2]), y = V(a[3]), r; r.n = x.n && x.n == y.n && x.n == rtype_n() ? x.n : 0;
            const uint8_t o = op == OpFAdd ? NR_CV_ADD : op == OpFSub ? NR_CV_SUB : op == OpFMul ? NR_CV_MUL : NR_CV_DIV;
            for (int k = 0; k < r.n; k++) r.c[k] = mk(o, x.c[k], y.c[k]);
            val[a[1]] = r; } break;
        case OpVectorTimesScalar: if (wc == 5 && a[1] < bound) { Vec x = V(a[2]), r; const int s = S(a[3]); r.n = x.n > 1 && x.n == rtype_n() ? x.n : 0; for (int k = 0; k < r.n; k++) r.c[k] = mk(NR_CV_MUL, x.c[k], s); val[a[1]] = r; } break;
        case OpExtInst: if (wc >= 5 && a[1] < bound && a[2] < bound && glsl[a[2]]) {
            const uint32_t f = a[3]; Vec r; const int rn = rtype_n();
            if (rn && ((f == GlslFAbs && wc == 6) || ((f == GlslFMin || f == GlslFMax) && wc == 7) || (f == GlslFma && wc == 8))) {
                Vec x = V(a[4]), y = wc >= 7 ? V(a[5]) : x, z = wc >= 8 ? V(a[6]) : x;
                r.n = x.n == rn && y.n == rn && z.n == rn ? rn : 0;
                for (int k = 0; k < r.n; k++) r.c[k] = f == GlslFAbs ? mk(NR_CV_ABS, x.c[k]) : f == GlslFMin ? mk(NR_CV_MIN, x.c[k], y.c[k]) : f == GlslFMax ? mk(NR_CV_MAX, x.c[k], y.c[k]) : mk(NR_CV_FMA, x.c[k], y.c[k], z.c[k]);
            }
            val[a[1]] = r; } break;
        case OpCompositeExtract: if (wc == 5 && a[1] < bound) { Vec x = V(a[2]), r; if (x.n > 1 && int(a[3]) < x.n && rtype_n() == 1) { r.n = 1; r.c[0] = x.c[a[3]]; } val[a[1]] = r; } break;
        case OpCompositeInsert: if (wc == 6 && a[1] < bound) { Vec x = V(a[3]), r; const int s = S(a[2]); if (x.n > 1 && int(a[4]) < x.n && rtype_n() == x.n) { r = x; r.c[a[4]] = s; } val[a[1]] = r; } break;
        case OpCompositeConstruct: if (wc >= 4 && a[1] < bound) {
            Vec r; const int rn = rtype_n(); int k = 0; bool good = rn > 1;
            for (uint32_t j = 2; j + 1 < wc && good; j++) { Vec x = V(a[j]); if (!x.n || k + x.n > rn) { good = false; break; } for (int q = 0; q < x.n; q++) r.c[k++] = x.c[q]; }
            if (good && k == rn) r.n = rn; else r = Vec{};
            val[a[1]] = r; } break;
        case OpVectorShuffle: if (wc >= 6 && a[1] < bound) {
            Vec x = V(a[2]), y = V(a[3]), r; const int rn = rtype_n(); bool good = rn > 1 && int(wc) - 5 == rn && x.n > 1 && y.n > 1;
            for (int k = 0; k < rn && good; k++) { const uint32_t s = a[4 + k]; if (s == 0xffffffffu) r.c[k] = -1; else if (int(s) < x.n) r.c[k] = x.c[s]; else if (int(s) < x.n + y.n) r.c[k] = y.c[s - x.n]; else good = false; }
            if (good) r.n = rn; else r = Vec{};
            val[a[1]] = r; } break;
        default: break;   // any other result id stays unknown (n = 0)
        }
        i += wc;
    }
    if (target >= 0) {   // x, y must be followed; z, w fall back to x
        for (int k = 0; k < 2; k++) if (pos.c[k] < 0) { *why = "x / y of the output is not a float expression of constants / inputs / uniforms"; return; }
        for (int k = 2; k < 4; k++) if (k >= pn || pos.c[k] < 0) pos.c[k] = pos.c[0];
    }
    for (int k = 0; k < 4; k++) if (pos.c[k] < 0) { *why = "a position component is not a float expression of constants / inputs / uniforms"; return; }
    // Renumber what the four components need (operands always precede their users).
    std::vector<int> need(ins.size(), 0), renum(ins.size(), -1);
    for (int k = 0; k < 4; k++) need[size_t(pos.c[k])] = 1;
    for (size_t i = ins.size(); i-- > 0;) if (need[i] && ins[i].op > NR_CV_UBO) {
        const nr_cv_ins &x = ins[i]; need[x.a] = 1;
        if (x.op != NR_CV_NEG && x.op != NR_CV_ABS) need[x.b] = 1;
        if (x.op == NR_CV_FMA) need[x.c] = 1;
    }
    unsigned cnt = 0; for (size_t i = 0; i < ins.size(); i++) if (need[i]) ++cnt;
    if (cnt > NR_CV_MAX_INS) { *why = "position expression too long"; return; }
    unsigned m = 0;
    for (size_t i = 0; i < ins.size(); i++) if (need[i]) {
        nr_cv_ins x = ins[i];
        if (x.op == NR_CV_INPUT) pr->in_mask |= 1u << x.a;
        else if (x.op == NR_CV_UBO) {
            unsigned u = 0; while (u < pr->nubo && pr->ubo[u] != x.a) u++;
            if (u == pr->nubo) { if (u >= NR_CV_MAX_UBO) { *why = "too many uniform blocks"; memset(pr, 0, sizeof *pr); return; } pr->ubo[u] = x.a; pr->nubo++; }
            if (uint32_t(x.b) + 4 > pr->ubo_bytes[u]) pr->ubo_bytes[u] = uint32_t(x.b) + 4;
        } else if (x.op != NR_CV_CONST) {
            x.a = uint16_t(renum[x.a]);
            x.b = x.op != NR_CV_NEG && x.op != NR_CV_ABS ? uint16_t(renum[x.b]) : 0;
            x.c = x.op == NR_CV_FMA ? uint16_t(renum[x.c]) : 0;
        }
        renum[i] = int(m); pr->ins[m++] = x;
    }
    pr->n = uint8_t(m); pr->exact = 1;
    for (int k = 0; k < 4; k++) { pr->out[k] = uint8_t(renum[size_t(pos.c[k])]); if (pr->ins[pr->out[k]].op > NR_CV_INPUT) pr->exact = 0; }
    pr->ok = 1; *why = "ok";
}

void xbg_spirv_pos_prog(const uint32_t *in, size_t n, nr_cv_prog *pr, const char **why) { out_prog(in, n, -1, pr, why); }
void xbg_spirv_out_prog(const uint32_t *in, size_t n, uint32_t location, nr_cv_prog *pr, const char **why) {
    if (location >= 32) { const char *d; if (!why) why = &d; memset(pr, 0, sizeof *pr); *why = "location"; return; }
    out_prog(in, n, int(location), pr, why);
}
