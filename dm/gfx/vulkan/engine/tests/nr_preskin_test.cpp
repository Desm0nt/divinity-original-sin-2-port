// [DOS2] NR-PRESKIN (docs/dos2/nr/NR-PRESKIN.md): xbg_spirv_preskin on the player's pack.
//   nr_preskin_test <shaders.xgspack> <keys.txt> <out dir> [chain=nftcp]
// For every key (a vertex bundle): the module as the phone loads it (the chain of xbg_game.cpp tidy_rewrite, letters as tools/tu_stats/
// phone_chain.cpp: n NaN min/max + sat, f LDC fold, t forwarded temporaries, c LDC canon, p F2I no-trunc, u NR-27 UBOVEC as on Mali; r the
// Mali vertex input retype of Location 15 to uint after them), the
// plan, the consumer and the slice. Writes <key>.orig.spv / .cons.spv / .slice.spv / .plan.txt (tools/dos2_preskin_check.py reads them:
// spirv-val + the interpreter proof) and prints one line per key and a summary. Checks of its own (exit 1 on a failure):
//   - a plan that is ok yields both modules; the emitters are deterministic (two runs, same words);
//   - the consumer reads no bone block through a vertex-input index any more (xbg_spirv_preskin_candidate on it is false) and keeps the
//     original's stores; the slice has no Output store and stores m words a vertex;
//   - the plan of the consumer's own words is refused ("no uniform block read through a vertex-input index"): nothing is left to cut.
// Build (WSL): g++ -O2 -std=c++17 -I dm/gfx/vulkan/engine dm/gfx/vulkan/engine/tests/nr_preskin_test.cpp dm/gfx/vulkan/engine/xbg_spirv_preskin.cpp \
//   dm/gfx/vulkan/engine/xbg_spirv_{ldc,temps,peep,ubovec}.cpp -o nr_preskin_test
#include "xbg_spirv_ldc.h"
#include "xbg_spirv_peep.h"
#include "xbg_spirv_preskin.h"
#include "xbg_spirv_temps.h"
#include "xbg_spirv_ubovec.h"
#include "xbg_vertex_retype.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

static size_t nan_minmax_swap(std::vector<uint32_t> &w, bool sat) {   // xbg_game.cpp nan_minmax_swap (as tools/tu_stats/phone_chain.cpp)
    uint32_t glsl = 0; size_t k = 0; std::unordered_map<uint32_t, uint32_t> fconst;
    for (size_t i = 5; i < w.size();) {
        uint32_t op = w[i] & 0xFFFFu, n = w[i] >> 16; if (!n || i + n > w.size()) break;
        if (sat && op == 43 && n == 4) fconst[w[i + 2]] = w[i + 3];
        if (op == 11 && n >= 5 && !memcmp(&w[i + 2], "GLSL.std.450", 13)) glsl = w[i + 1];
        else if (op == 12 && n >= 5 && glsl && w[i + 3] == glsl) {
            uint32_t &e = w[i + 4];
            if (e == 43 && sat && n >= 8) { auto lo = fconst.find(w[i + 6]), hi = fconst.find(w[i + 7]); if (lo != fconst.end() && hi != fconst.end() && lo->second == 0u && hi->second == 0x3f800000u) { i += n; continue; } }
            if (e == 37) { e = 79; ++k; } else if (e == 40) { e = 80; ++k; } else if (e == 43) { e = 81; ++k; }
        }
        i += n;
    }
    return k;
}
static void chain(std::vector<uint32_t> &w, const char *c) {
    std::vector<uint32_t> o;
    if (strchr(c, 'n')) nan_minmax_swap(w, true);
    if (strchr(c, 'f') && xbg_spirv_ldc_fold(w.data(), w.size(), o)) w.swap(o);
    if (strchr(c, 't') && xbg_spirv_forward_temps(w.data(), w.size(), o)) w.swap(o);
    if (strchr(c, 'c') && xbg_spirv_ldc_canon(w.data(), w.size(), o)) w.swap(o);
    if (strchr(c, 'p') && xbg_spirv_f2i_notrunc(w.data(), w.size(), o)) w.swap(o);
    if (strchr(c, 'u')) { XbgUbovecStats st; if (xbg_spirv_ubovec(w.data(), w.size(), o, &st, false, false)) w.swap(o); }
    // r: the pipeline's vertex input retype as on Mali / PowerVR (DM_GFX_VTX_RETYPE): Location 15 (bone indices, R8G8B8A8_UINT) declared uint
    if (strchr(c, 'r')) { const uint32_t loc = 15, want = XBG_VTX_WANT_UINT; std::string why; if (xbg_vertex_retype(w, &loc, 1, &want, o, &why)) w.swap(o); }
}
static bool put(const std::string &p, const std::vector<uint32_t> &w) { FILE *f = fopen(p.c_str(), "wb"); if (!f) return false; fwrite(w.data(), 4, w.size(), f); fclose(f); return true; }
// stores to Output variables (OpStore whose pointer roots at an Output variable)
static void facts(const std::vector<uint32_t> &w, int &out_stores, int &sb_stores) {
    std::unordered_map<uint32_t, uint32_t> sc, root; out_stores = sb_stores = 0;
    for (size_t i = 5; i < w.size();) { const uint32_t op = w[i] & 0xffff, n = w[i] >> 16; if (!n) break;
        if (op == 59 && n >= 4) sc[w[i + 2]] = w[i + 3];
        if ((op == 65 || op == 66) && n >= 4) root[w[i + 2]] = root.count(w[i + 3]) ? root[w[i + 3]] : w[i + 3];
        if (op == 62) { const uint32_t p = w[i + 1]; const uint32_t v = root.count(p) ? root[p] : p; if (sc[v] == 3) out_stores++; if (sc[v] == 12) sb_stores++; }
        i += n; }
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: nr_preskin_test <shaders.xgspack> <keys.txt> <out dir> [chain=nftcp]\n"); return 2; }
    const char *c = argc > 4 ? argv[4] : "nftcp";
    std::ifstream f(argv[1], std::ios::binary | std::ios::ate); if (!f) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    std::vector<uint8_t> d(size_t(f.tellg())); f.seekg(0); f.read(reinterpret_cast<char *>(d.data()), std::streamsize(d.size()));
    uint32_t h[4]; memcpy(h, d.data(), 16); if (h[0] != 0x50534758u || h[1] != 1) { fprintf(stderr, "not an xgspack v1\n"); return 2; }
    std::map<std::string, std::pair<uint64_t, uint32_t>> at;
    for (uint32_t i = 0; i < h[2]; i++) { const uint8_t *e = d.data() + 16 + size_t(i) * 56; char key[33] = {}; memcpy(key, e, 32); uint64_t off; uint32_t size; memcpy(&off, e + 32, 8); memcpy(&size, e + 40, 4); at[key] = {off, size}; }
    struct Planned { std::string key; std::vector<uint32_t> w; XbgPreskinPlan p; }; std::vector<Planned> plans;
    std::ifstream kf(argv[2]); std::string k; int n = 0, ok = 0, fail = 0, miss = 0; std::map<std::string, int> why; std::map<uint32_t, int> ms;
    while (kf >> k) {
        auto a = at.find(k); if (a == at.end()) { miss++; continue; }
        uint32_t b[8]; memcpy(b, d.data() + a->second.first, 32); const size_t so = 32 + size_t(b[3]) * 32 + size_t(b[4]) * 4 + b[5];
        if (b[0] != 0x31534758u || so + b[6] != a->second.second || b[2] != 0) { miss++; continue; }
        std::vector<XbgPreskinBind> binds;
        for (uint32_t i = 0; i < b[3]; i++) { uint32_t e[8]; memcpy(e, d.data() + a->second.first + 32 + size_t(i) * 32, 32); if (e[1] == 6) binds.push_back({e[0], e[4] << 16 | e[5]}); }
        std::vector<uint32_t> w(b[6] / 4); memcpy(w.data(), d.data() + a->second.first + so, b[6]); chain(w, c); n++;
        XbgPreskinPlan p; const bool pok = xbg_spirv_preskin_plan(w.data(), w.size(), binds.data(), binds.size(), p);
        if (!pok) { why[p.why]++; printf("%s refused: %s\n", k.c_str(), p.why); continue; }
        std::vector<uint32_t> cons, slice, cons2, slice2; const char *wc = "", *ws = "";
        const bool cok = xbg_spirv_preskin_consumer(w.data(), w.size(), p, nullptr, cons, nullptr, &wc), sok = xbg_spirv_preskin_slice(w.data(), w.size(), p, slice, &ws);
        bool bad = !cok || !sok;
        if (!bad) { xbg_spirv_preskin_consumer(w.data(), w.size(), p, nullptr, cons2); xbg_spirv_preskin_slice(w.data(), w.size(), p, slice2); bad = cons != cons2 || slice != slice2; if (bad) wc = "not deterministic"; }
        int o0 = 0, s0 = 0, o1 = 0, s1 = 0, o2 = 0, s2 = 0;
        if (!bad) { facts(w, o0, s0); facts(cons, o1, s1); facts(slice, o2, s2);
            if (o1 != o0) { bad = true; wc = "the consumer's Output stores differ"; }
            if (o2 || s2 != int(p.m)) { bad = true; ws = "the slice does not store m words alone"; }
            if (xbg_spirv_preskin_candidate(cons.data(), cons.size())) { bad = true; wc = "the consumer still indexes a bone block"; }
            XbgPreskinPlan q; if (xbg_spirv_preskin_plan(cons.data(), cons.size(), binds.data(), binds.size(), q)) { bad = true; wc = "the consumer could be cut again"; } }
        if (bad) { fail++; printf("%s FAIL: consumer %s / slice %s\n", k.c_str(), cok ? wc : wc, sok ? ws : ws); continue; }
        ok++; ms[p.m]++; plans.push_back({k, w, p});
        const std::string base = std::string(argv[3]) + "/" + k;
        put(base + ".orig.spv", w); put(base + ".cons.spv", cons); put(base + ".slice.spv", slice);
        if (FILE *pf = fopen((base + ".plan.txt").c_str(), "w")) {
            fprintf(pf, "m %u\nattrs %u\nlocs", p.m, p.attrs); for (uint32_t i = 0; i < p.attrs; i++) fprintf(pf, " %u", p.new_locs[i]);
            fprintf(pf, "\nsig %016llx\nslice_locs %08x\ncons_locs %08x\norig_locs %08x\nssbo %u\nkdep %u\nbone_loads %u\nops %u %u %u\nubos", (unsigned long long)p.sig, p.slice_locs, p.cons_locs, p.orig_locs, p.ssbo_binding, p.kdep, p.bone_loads, p.ops_orig, p.ops_cons, p.ops_slice);
            for (size_t i = 0; i < p.slice_ubos.size(); i++) fprintf(pf, " %u:%08x", p.slice_ubos[i], p.ubo_keys[i]);
            fprintf(pf, "\nvals"); for (auto &v : p.vals) fprintf(pf, " %016llx:%u:%u:%u:%x:%x", (unsigned long long)v.hash, v.comps, v.ctype, v.first, v.loc_mask, v.ubo_mask);
            fprintf(pf, "\nbone %u\n", p.bone_binding); fclose(pf); }
        printf("%s ok: m=%u attrs=%u sig=%016llx kdep=%u bone_loads=%u ops %u -> consumer %u + slice %u; locs orig %04x slice %04x consumer %04x new %u..; words %zu -> %zu / %zu\n",
               k.c_str(), p.m, p.attrs, (unsigned long long)p.sig, p.kdep, p.bone_loads, p.ops_orig, p.ops_cons, p.ops_slice, p.orig_locs, p.slice_locs, p.cons_locs, p.new_locs[0], w.size(), cons.size(), slice.size());
    }
    printf("nr_preskin_test %s: %d modules, planned %d, FAIL %d, missing %d; record sizes:", c, n, ok, fail, miss);
    for (auto &x : ms) printf(" m%u=%d", x.first, x.second);
    printf("; refusals:");
    for (auto &x : why) printf(" [%s] %d", x.first.c_str(), x.second);
    printf("\n");
    // superset layouts: P's consumer reading another plan's record
    int sup = 0, sup_fail = 0;
    for (auto &P : plans) {
        const Planned *Q = nullptr;
        for (auto &R : plans) if (R.p.m > P.p.m) { bool all = true;
            for (auto &v : P.p.vals) { bool f = false; for (auto &u : R.p.vals) if (u.hash == v.hash && u.comps == v.comps) f = true; if (!f) all = false; }
            if (all) { Q = &R; break; } }
        if (!Q) continue;
        std::vector<uint32_t> pos(P.p.m);
        for (auto &v : P.p.vals) for (auto &u : Q->p.vals) if (u.hash == v.hash) for (uint32_t j = 0; j < v.comps; j++) pos[v.first + j] = u.first + j;
        std::vector<uint32_t> cons; XbgPreskinLayout lay; const char *wc = "";
        if (!xbg_spirv_preskin_consumer(P.w.data(), P.w.size(), P.p, pos.data(), cons, &lay, &wc)) { sup_fail++; printf("%s FAIL superset consumer: %s\n", P.key.c_str(), wc); continue; }
        sup++; const std::string base = std::string(argv[3]) + "/" + P.key; put(base + ".sup.spv", cons);
        if (FILE *pf = fopen((base + ".sup.txt").c_str(), "w")) { fprintf(pf, "record %s %u\npos", Q->key.c_str(), Q->p.m); for (uint32_t x : pos) fprintf(pf, " %u", x);
            fprintf(pf, "\ngroups"); for (uint32_t g = 0; g < lay.groups; g++) fprintf(pf, " %u:%u:%u", lay.loc[g], lay.first[g], lay.width[g]); fprintf(pf, "\n"); fclose(pf); }
    }
    printf("superset consumers %d, FAIL %d\n", sup, sup_fail);
    return fail || sup_fail ? 1 : 0;
}
