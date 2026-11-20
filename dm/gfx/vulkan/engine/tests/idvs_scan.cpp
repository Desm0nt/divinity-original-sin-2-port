// [DOS2] T2 DM_NR_IDVS: offline runner of the production PointSize strip (xbg_spirv_idvs.cpp) over every vertex shader of a shader
// pack, with the phone's load-time rewrites in front (as tests/vsout_scan.cpp: NAN_MINMAX + NAN_SAT, LDC_FOLD, FORWARD_TEMPS, LDC_CANON,
// F2I_NOTRUNC, NR-27 UBOVEC), so "before" is what a Mali phone compiles today. Every strip is checked by xbg_spirv_idvs_check.
//   idvs_scan <shaders.xgspack> <out dir> [chain=nftcpu] [write] [pairs.tsv: vs<TAB>fs a line -> also the NR-32 trim per pair]
//     -> out/vs.tsv (one line a vertex shader) and, with "write", <vs>.before/after.vert.spv; with pairs, <vs>.<read>.before/after.vert.spv
//        = the NR-32 trimmed module without / with the strip (what a Mali phone compiles with DM_NR_XLQ=1 and DM_NR_IDVS 0 / 1).
//     exit 1 on any check failure.
#include "../xbg_spirv_idvs.h"
#include "../xbg_spirv_ldc.h"
#include "../xbg_spirv_peep.h"
#include "../xbg_spirv_temps.h"
#include "../xbg_spirv_ubovec.h"
#include "../xbg_spirv_vsout.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

static size_t nan_minmax_swap(std::vector<uint32_t> &w, bool sat) {   // xbg_game.cpp nan_minmax_swap (DM_GFX_NAN_MINMAX + NAN_SAT)
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
}
static bool save(const std::string &p, const std::vector<uint32_t> &w) {
    FILE *f = fopen(p.c_str(), "wb"); if (!f) return false;
    bool ok = fwrite(w.data(), 4, w.size(), f) == w.size(); fclose(f); return ok;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: see the head of tests/idvs_scan.cpp\n"); return 2; }
    const char *c = argc > 3 ? argv[3] : "nftcpu"; const bool write = argc > 4 && !strcmp(argv[4], "write"); const char *pairs = argc > 5 ? argv[5] : nullptr;
    std::ifstream f(argv[1], std::ios::binary | std::ios::ate); if (!f) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    std::vector<uint8_t> d(size_t(f.tellg())); f.seekg(0); f.read(reinterpret_cast<char *>(d.data()), std::streamsize(d.size()));
    uint32_t h[4]; memcpy(h, d.data(), 16); if (h[0] != 0x50534758u || h[1] != 1) { fprintf(stderr, "not an xgspack v1\n"); return 2; }
    std::map<std::string, std::pair<uint64_t, uint32_t>> at;
    for (uint32_t i = 0; i < h[2]; i++) { const uint8_t *e = d.data() + 16 + size_t(i) * 56; char key[33] = {}; memcpy(key, e, 32); uint64_t off; uint32_t size; memcpy(&off, e + 32, 8); memcpy(&size, e + 40, 4); at[key] = {off, size}; }
    auto words = [&](const std::string &k, uint32_t want_stage, std::vector<uint32_t> &v) -> bool {
        auto a = at.find(k); if (a == at.end()) return false;
        uint32_t b[8]; memcpy(b, d.data() + a->second.first, 32); const size_t so = 32 + size_t(b[3]) * 32 + size_t(b[4]) * 4 + b[5];
        if (b[0] != 0x31534758u || so + b[6] != a->second.second || b[2] != want_stage) return false;
        v.resize(b[6] / 4); memcpy(v.data(), d.data() + a->second.first + so, b[6]); chain(v, c); return true;
    };
    const std::string out = argv[2]; FILE *tsv = fopen((out + "/vs.tsv").c_str(), "w");
    if (!tsv) { fprintf(stderr, "cannot write %s/vs.tsv (make the directory first)\n", out.c_str()); return 2; }
    fprintf(tsv, "vs\twords\tstores_dropped\tchains_dropped\tresult\n");
    size_t vs = 0, stripped = 0, stores = 0, bad = 0; std::map<std::string, size_t> why;
    for (auto &kv : at) {
        std::vector<uint32_t> w; if (!words(kv.first, 0, w)) continue;
        vs++; std::vector<uint32_t> o; XbgIdvsStats st; const size_t k = xbg_spirv_idvs_strip(w.data(), w.size(), o, &st);
        const char *res = st.why;
        if (k) {
            const char *bad_why = xbg_spirv_idvs_check(w.data(), w.size(), o.data(), o.size());
            if (*bad_why) { bad++; res = bad_why; fprintf(stderr, "%s: CHECK FAILED: %s\n", kv.first.c_str(), bad_why); }
            else if (!xbg_spirv_idvs_stripped(o.data(), o.size())) { bad++; res = "stripped() disagrees"; fprintf(stderr, "%s: stripped() false after a strip\n", kv.first.c_str()); }
            else { stripped++; stores += k; res = "ok"; if (write && (!save(out + "/" + kv.first + ".before.vert.spv", w) || !save(out + "/" + kv.first + ".after.vert.spv", o))) { fprintf(stderr, "cannot write into %s\n", out.c_str()); return 2; } }
        } else why[st.why]++;
        fprintf(tsv, "%s\t%zu\t%u\t%u\t%s\n", kv.first.c_str(), w.size(), st.stores_dropped, st.chains_dropped, res);
    }
    fclose(tsv);
    printf("vertex shaders %zu stripped %zu stores_dropped %zu check_failed %zu (chain %s)\n", vs, stripped, stores, bad, c);
    for (auto &kv : why) printf("  not stripped: %zu  %s\n", kv.second, kv.first.c_str());
    if (pairs) {   // NR-32 trim per (vs, fs) pair, then the strip on the trimmed module: before = XLQ only, after = XLQ + IDVS
        std::ifstream pf(pairs); std::string line; std::set<std::pair<std::string, uint64_t>> done; size_t np = 0, pbad = 0;
        FILE *pr = fopen((out + "/pair_reads.tsv").c_str(), "w"); if (!pr) { fprintf(stderr, "cannot write into %s\n", out.c_str()); return 2; }
        while (std::getline(pf, line)) {
            std::istringstream ls(line); std::string v, fs; if (!(ls >> v >> fs) || v == "vs") continue;
            std::vector<uint32_t> vw, fw; if (!words(v, 0, vw) || !words(fs, 4, fw)) continue;
            uint64_t decl = 0; const uint64_t read = xbg_spirv_fs_input_mask(fw.data(), fw.size(), &decl);
            fprintf(pr, "%s\t%s\t%016llx\n", v.c_str(), fs.c_str(), (unsigned long long)read);
            if (!done.insert({v, read}).second) continue;
            const uint64_t wr = xbg_spirv_vs_output_mask(vw.data(), vw.size()); std::vector<uint32_t> t;
            if (read != XBG_VSOUT_ALL && wr != XBG_VSOUT_ALL && (wr & ~read) && xbg_spirv_vsout_trim(vw.data(), vw.size(), read, t, nullptr, decl)) vw.swap(t);
            std::vector<uint32_t> o; if (!xbg_spirv_idvs_strip(vw.data(), vw.size(), o)) continue;
            if (*xbg_spirv_idvs_check(vw.data(), vw.size(), o.data(), o.size())) { pbad++; continue; }
            np++; char tag[24]; snprintf(tag, sizeof tag, ".%016llx", (unsigned long long)read);
            if (!save(out + "/" + v + tag + ".before.vert.spv", vw) || !save(out + "/" + v + tag + ".after.vert.spv", o)) { fprintf(stderr, "cannot write into %s\n", out.c_str()); return 2; }
        }
        fclose(pr);
        printf("pairs: %zu distinct (vs, read) stripped after the NR-32 trim, check_failed %zu\n", np, pbad); bad += pbad;
    }
    return bad ? 1 : 0;
}
