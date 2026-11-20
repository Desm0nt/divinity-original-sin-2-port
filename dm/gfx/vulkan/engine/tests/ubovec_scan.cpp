// [NR-27] Offline runner of the production DM_NR_UBOVEC rewrite (xbg_spirv_ubovec.cpp) over a shader pack or loose .spv files, with the
// load-time clean-ups of the phone in front of it (DM_GFX_LDC_FOLD, DM_GFX_FORWARD_TEMPS, DM_GFX_LDC_CANON: xbg_spirv_ldc.cpp,
// xbg_spirv_temps.cpp), so that "before" is what a phone compiles today. No Vulkan; nothing is written outside the output directory.
//   ubovec_scan pack <shaders.xgspack> <out dir> [chain=ftc] [stage=4|all] [keys.txt]
//        -> out/modules.tsv, and <key>.before.spv / <key>.after.spv of every changed module
//   ubovec_scan file <in.spv> <out.spv> [chain=ftc] [poison]
// chain: letters f (fold) t (temps) c (canon), "-" for none; with the letter x also vectors at a fixed place (fixed_too). Every rewrite is checked by xbg_spirv_ubovec_check.
#include "../xbg_spirv_ldc.h"
#include "../xbg_spirv_temps.h"
#include "../xbg_spirv_ubovec.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

static void chain(std::vector<uint32_t> &w, const char *c) {
    std::vector<uint32_t> o;
    if (strchr(c, 'f') && xbg_spirv_ldc_fold(w.data(), w.size(), o)) w.swap(o);
    if (strchr(c, 't') && xbg_spirv_forward_temps(w.data(), w.size(), o)) w.swap(o);
    if (strchr(c, 'c') && xbg_spirv_ldc_canon(w.data(), w.size(), o)) w.swap(o);
}
static bool save(const std::string &p, const std::vector<uint32_t> &w) {
    FILE *f = fopen(p.c_str(), "wb"); if (!f) return false;
    bool ok = fwrite(w.data(), 4, w.size(), f) == w.size(); fclose(f); return ok;
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "file")) {
        FILE *f = fopen(argv[2], "rb"); if (!f) { fprintf(stderr, "cannot read %s\n", argv[2]); return 2; }
        fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
        std::vector<uint32_t> w(size_t(n) / 4), o; if (fread(w.data(), 4, w.size(), f) != w.size()) return 2; fclose(f);
        const char *c = argc > 4 ? argv[4] : "ftc"; const bool poison = argc > 5 && !strcmp(argv[5], "poison");
        chain(w, c); XbgUbovecStats st; size_t k = xbg_spirv_ubovec(w.data(), w.size(), o, &st, poison, strchr(c, 'x') != nullptr);
        const char *bad = k ? xbg_spirv_ubovec_check(w.data(), w.size(), o.data(), o.size(), poison) : "";
        printf("reads %zu vec_loads %u candidates %u dynamic %u dropped_chains %u check %s\n", k, st.vec_loads, st.candidates, st.dynamic, st.chains_dropped, *bad ? bad : "ok");
        return save(argv[3], k ? o : w) && !*bad ? 0 : 1;
    }
    if (argc < 4 || strcmp(argv[1], "pack")) { fprintf(stderr, "usage: see the head of tests/ubovec_scan.cpp\n"); return 2; }
    const char *c = argc > 4 ? argv[4] : "ftc"; const char *stage = argc > 5 ? argv[5] : "4"; std::set<std::string> only;
    if (argc > 6) { std::ifstream k(argv[6]); std::string s; while (k >> s) only.insert(s); }
    std::ifstream f(argv[2], std::ios::binary | std::ios::ate); if (!f) { fprintf(stderr, "cannot read %s\n", argv[2]); return 2; }
    std::vector<uint8_t> d(size_t(f.tellg())); f.seekg(0); f.read(reinterpret_cast<char *>(d.data()), std::streamsize(d.size()));
    uint32_t h[4]; memcpy(h, d.data(), 16); if (h[0] != 0x50534758u || h[1] != 1) { fprintf(stderr, "not an xgspack v1\n"); return 2; }
    const std::string out = argv[3]; FILE *tsv = fopen((out + "/modules.tsv").c_str(), "w");
    if (!tsv) { fprintf(stderr, "cannot write %s/modules.tsv (make the directory first)\n", out.c_str()); return 2; }
    fprintf(tsv, "key\tstage\twords_before\twords_after\tcandidates\treads\tvec_loads\tdynamic\tdropped_chains\tpass_us\tcheck\n");
    size_t modules = 0, changed = 0, reads = 0, loads = 0, bad_n = 0, cand = 0, dyn = 0; double us_sum = 0, us_max = 0;
    for (uint32_t i = 0; i < h[2]; i++) {
        const uint8_t *e = d.data() + 16 + size_t(i) * 56; char key[33] = {}; memcpy(key, e, 32);
        uint64_t off; uint32_t size; memcpy(&off, e + 32, 8); memcpy(&size, e + 40, 4);
        uint32_t b[8]; memcpy(b, d.data() + off, 32); const size_t so = 32 + size_t(b[3]) * 32 + size_t(b[4]) * 4 + b[5];
        if (b[0] != 0x31534758u || so + b[6] != size) { fprintf(stderr, "%s: not an XGS1 bundle\n", key); return 1; }
        if (strcmp(stage, "all") && b[2] != uint32_t(atoi(stage))) continue;
        if (!only.empty() && !only.count(key)) continue;
        std::vector<uint32_t> w(b[6] / 4), o; memcpy(w.data(), d.data() + off + so, b[6]);
        chain(w, c); modules++;
        XbgUbovecStats st; const auto t0 = std::chrono::steady_clock::now();
        const size_t k = xbg_spirv_ubovec(w.data(), w.size(), o, &st, false, strchr(c, 'x') != nullptr);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count(); us_sum += us; if (us > us_max) us_max = us;
        const char *bad = k ? xbg_spirv_ubovec_check(w.data(), w.size(), o.data(), o.size()) : ""; cand += st.candidates; dyn += st.dynamic;
        if (k || st.candidates || st.dynamic)
            fprintf(tsv, "%s\t%u\t%zu\t%zu\t%u\t%zu\t%u\t%u\t%u\t%.0f\t%s\n", key, b[2], w.size(), k ? o.size() : w.size(), st.candidates, k, st.vec_loads, st.dynamic, st.chains_dropped, us, *bad ? bad : "ok");
        if (*bad) { bad_n++; fprintf(stderr, "%s: CHECK FAILED: %s\n", key, bad); }
        if (k) {
            changed++; reads += k; loads += st.vec_loads;
            if (!save(out + "/" + key + ".before.spv", w) || !save(out + "/" + key + ".after.spv", o)) { fprintf(stderr, "cannot write into %s\n", out.c_str()); return 2; }
        }
    }
    fclose(tsv);
    printf("modules %zu changed %zu candidate_reads %zu reads_rewritten %zu vec_loads %zu dynamic_left %zu check_failed %zu pass_us avg %.0f max %.0f (chain %s, stage %s)\n",
           modules, changed, cand, reads, loads, dyn, bad_n, modules ? us_sum / double(modules) : 0.0, us_max, c, stage);
    return bad_n ? 1 : 0;
}
