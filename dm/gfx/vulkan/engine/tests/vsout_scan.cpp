// [NR-32] Offline runner of the production DM_NR_XLQ trim (xbg_spirv_vsout.cpp) over (vertex, fragment) pairs of a shader pack,
// with the phone's load-time rewrites in front (DM_GFX_NAN_MINMAX + NAN_SAT, LDC_FOLD, FORWARD_TEMPS, LDC_CANON, F2I_NOTRUNC, and
// NR-27 UBOVEC with "u"), so "before" is what a Mali phone compiles today. No Vulkan; writes only into the output directory.
//   vsout_scan <shaders.xgspack> <pairs.tsv: vs<TAB>fs a line> <out dir> [chain=nftcpu] [write]
//     -> out/pairs.tsv (one line a pair: masks, stores dropped, check) and, with "write", <vs>.<keep>.before/after.vert.spv once a
//        distinct (vs, keep). Every trim is checked by xbg_spirv_vsout_check; exit 1 on any refusal.
#include "../xbg_spirv_ldc.h"
#include "../xbg_spirv_peep.h"
#include "../xbg_spirv_temps.h"
#include "../xbg_spirv_ubovec.h"
#include "../xbg_spirv_vsout.h"
#include <chrono>
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
static int pop(uint64_t m) { int n = 0; while (m) { m &= m - 1; n++; } return n; }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: see the head of tests/vsout_scan.cpp\n"); return 2; }
    const char *c = argc > 4 ? argv[4] : "nftcpu"; const bool write = argc > 5 && !strcmp(argv[5], "write");
    std::ifstream f(argv[1], std::ios::binary | std::ios::ate); if (!f) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    std::vector<uint8_t> d(size_t(f.tellg())); f.seekg(0); f.read(reinterpret_cast<char *>(d.data()), std::streamsize(d.size()));
    uint32_t h[4]; memcpy(h, d.data(), 16); if (h[0] != 0x50534758u || h[1] != 1) { fprintf(stderr, "not an xgspack v1\n"); return 2; }
    std::map<std::string, std::pair<uint64_t, uint32_t>> at;   // key -> (offset, size)
    for (uint32_t i = 0; i < h[2]; i++) { const uint8_t *e = d.data() + 16 + size_t(i) * 56; char key[33] = {}; memcpy(key, e, 32); uint64_t off; uint32_t size; memcpy(&off, e + 32, 8); memcpy(&size, e + 40, 4); at[key] = {off, size}; }
    std::unordered_map<std::string, std::vector<uint32_t>> cache;
    auto words = [&](const std::string &k, uint32_t want_stage, std::vector<uint32_t> *&w) -> bool {
        auto it = cache.find(k); if (it != cache.end()) { w = &it->second; return !w->empty(); }
        auto &v = cache[k]; w = &v; auto a = at.find(k); if (a == at.end()) return false;
        uint32_t b[8]; memcpy(b, d.data() + a->second.first, 32); const size_t so = 32 + size_t(b[3]) * 32 + size_t(b[4]) * 4 + b[5];
        if (b[0] != 0x31534758u || so + b[6] != a->second.second || b[2] != want_stage) return false;
        v.resize(b[6] / 4); memcpy(v.data(), d.data() + a->second.first + so, b[6]); chain(v, c); return true;
    };
    const std::string out = argv[3]; FILE *tsv = fopen((out + "/pairs.tsv").c_str(), "w");
    if (!tsv) { fprintf(stderr, "cannot write %s/pairs.tsv (make the directory first)\n", out.c_str()); return 2; }
    fprintf(tsv, "vs\tfs\twritten\tread\tdeclared\tkeep\twritten_comps\tkept_comps\tstores_dropped\tchains_dropped\tvars_dropped\tpinned\tus\tcheck\n");
    std::ifstream pf(argv[2]); std::string line; size_t pairs = 0, missing = 0, changed = 0, bad = 0, wc = 0, kc = 0, sd = 0, vd = 0, all_fs = 0;
    double us_sum = 0, us_max = 0; std::set<std::pair<std::string, uint64_t>> written;
    while (std::getline(pf, line)) {
        std::istringstream ls(line); std::string vs, fs; if (!(ls >> vs >> fs) || vs == "vs") continue;
        std::vector<uint32_t> *vw = nullptr, *fw = nullptr;
        if (!words(vs, 0, vw) || !words(fs, 4, fw)) { missing++; continue; }
        pairs++;
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t decl = 0; const uint64_t read = xbg_spirv_fs_input_mask(fw->data(), fw->size(), &decl);
        const uint64_t wr = xbg_spirv_vs_output_mask(vw->data(), vw->size());
        std::vector<uint32_t> o; XbgVsoutStats st; size_t k = 0;
        if (read != XBG_VSOUT_ALL && wr != XBG_VSOUT_ALL && (wr & ~read)) k = xbg_spirv_vsout_trim(vw->data(), vw->size(), read, o, &st, decl);
        if (read == XBG_VSOUT_ALL) all_fs++;
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count(); us_sum += us; if (us > us_max) us_max = us;
        const char *why = k ? xbg_spirv_vsout_check(vw->data(), vw->size(), o.data(), o.size(), read) : "";
        if (*why) { bad++; fprintf(stderr, "%s + %s: CHECK FAILED: %s\n", vs.c_str(), fs.c_str(), why); }
        const uint64_t keep = wr & read; wc += pop(wr == XBG_VSOUT_ALL ? 0 : wr); kc += pop(wr == XBG_VSOUT_ALL ? 0 : keep);
        if (k) { changed++; sd += st.stores_dropped; vd += st.vars_dropped; }
        fprintf(tsv, "%s\t%s\t%016llx\t%016llx\t%016llx\t%016llx\t%d\t%d\t%u\t%u\t%u\t%u\t%.0f\t%s\n", vs.c_str(), fs.c_str(), (unsigned long long)wr, (unsigned long long)read,
                (unsigned long long)decl, (unsigned long long)keep, pop(wr), pop(keep), st.stores_dropped, st.chains_dropped, st.vars_dropped, st.pinned, us, *why ? why : "ok");
        if (write && k && written.insert({vs, read}).second) {
            char tag[24]; snprintf(tag, sizeof tag, ".%016llx", (unsigned long long)read);
            if (!save(out + "/" + vs + tag + ".before.vert.spv", *vw) || !save(out + "/" + vs + tag + ".after.vert.spv", o)) { fprintf(stderr, "cannot write into %s\n", out.c_str()); return 2; }
        }
    }
    fclose(tsv);
    printf("pairs %zu (missing %zu) trimmed %zu fs_unanalysed %zu components written %zu kept %zu (%.1f%% dead) stores_dropped %zu vars_dropped %zu check_failed %zu us avg %.0f max %.0f (chain %s)\n",
           pairs, missing, changed, all_fs, wc, kc, wc ? 100.0 * double(wc - kc) / double(wc) : 0.0, sd, vd, bad, pairs ? us_sum / double(pairs) : 0.0, us_max, c);
    return bad ? 1 : 0;
}
