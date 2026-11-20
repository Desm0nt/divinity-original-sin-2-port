// [DOS2] NRVS (docs/dos2/nr/NR-32.md / NR-33B.md "NRVS"): NR-33B's twin pipelines now use NR-32's vertex-output trim
// (xbg_spirv_vsout_trim with read = declared = 0) instead of nr_mat_prune. This test shows the replacement loses nothing: on every vertex
// shader the trim is accepted by xbg_spirv_vsout_check and leaves no more Location-output stores than nr_mat_prune(keep 0) did, and for
// the masks NR-33B used on non-twin pipelines (the fragment shader's Location inputs) NR-32's component mask keeps no more either.
//   clang -c ../../../nr/nr_mat.c && clang++ -std=c++17 nrvs_equiv_test.cpp ../xbg_spirv_vsout.cpp nr_mat.o -o nrvs_equiv_test
//   nrvs_equiv_test [shaders.xgspack [<key> <out prefix>]]   (with a pack: every vertex bundle of it at mask 0; key: write that one's modules)
#include "../../../nr/nr_mat.h"
#include "../xbg_spirv_vsout.h"
#include "nr_mat_test_spv.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define N(a) (sizeof(a) / sizeof((a)[0]))

// OpStore into a Location-decorated Output variable (directly or through an access chain of one)
static size_t loc_stores(const uint32_t *w, size_t n) {
    std::set<uint32_t> loc, outv, ptr;
    for (size_t i = 5; i < n;) { const uint32_t c = w[i] >> 16, op = w[i] & 0xffff; if (!c || i + c > n) break;
        if (op == 71 && c >= 4 && w[i + 2] == 30) loc.insert(w[i + 1]);
        if (op == 59 && c >= 4 && w[i + 3] == 3) outv.insert(w[i + 2]);
        i += c; }
    for (uint32_t v : outv) if (loc.count(v)) ptr.insert(v);
    size_t k = 0;
    for (size_t i = 5; i < n;) { const uint32_t c = w[i] >> 16, op = w[i] & 0xffff; if (!c || i + c > n) break;
        if ((op == 65 || op == 66) && c >= 4 && ptr.count(w[i + 3])) ptr.insert(w[i + 2]);
        if (op == 62 && c >= 3 && ptr.count(w[i + 1])) k++;
        i += c; }
    return k;
}

struct R { bool ok; size_t before, prune_after, vsout_after; };
// keep_loc: NR-33B's location mask; read / decl: NR-32's component masks of the same fragment shader (0 / 0 = the twin)
static R compare(const uint32_t *w, size_t n, uint32_t keep_loc, uint64_t read, uint64_t decl) {
    R r{true, loc_stores(w, n), 0, 0};
    std::vector<uint32_t> po(n + 1); size_t pn = 0; uint32_t pm = 0;
    const int pr = nr_mat_prune(w, n, keep_loc, po.data(), &pn, &pm);
    r.prune_after = pr >= 0 ? loc_stores(po.data(), pn) : r.before;
    std::vector<uint32_t> vo; XbgVsoutStats st;
    if (xbg_spirv_vsout_trim(w, n, read, vo, &st, decl)) {
        const char *bad = xbg_spirv_vsout_check(w, n, vo.data(), vo.size(), read);
        if (*bad) { printf("check refused: %s\n", bad); r.ok = false; }
        r.vsout_after = loc_stores(vo.data(), vo.size());
    } else r.vsout_after = r.before;
    if (r.vsout_after > r.prune_after) r.ok = false;
    return r;
}

int main(int argc, char **argv) {
    // synthetic: spv_vs3 (3 Location outputs), spv_vsrd (reads its own outputs back); fragment masks of nr_mat_test's shaders
    const struct { const uint32_t *w; size_t n; const char *name; } vs[] = {{spv_vs3, N(spv_vs3), "vs3"}, {spv_vsrd, N(spv_vsrd), "vsrd"}};
    const struct { const uint32_t *w; size_t n; } fs[] = {{spv_fsk, N(spv_fsk)}, {spv_fsd, N(spv_fsd)}, {spv_fsw, N(spv_fsw)}, {spv_fsp, N(spv_fsp)}};
    for (auto &v : vs) {
        R t = compare(v.w, v.n, 0, 0, 0);   // the twin
        printf("%s mask 0: Location stores %zu -> prune %zu, vsout %zu\n", v.name, t.before, t.prune_after, t.vsout_after);
        CHECK(t.ok);
        for (auto &f : fs) {
            nr_mat_facts ff; if (nr_mat_scan(f.w, f.n, &ff)) continue;
            uint64_t decl = 0; const uint64_t read = xbg_spirv_fs_input_mask(f.w, f.n, &decl); if (read == XBG_VSOUT_ALL) continue;
            R u = compare(v.w, v.n, ff.in_mask, read, decl);
            CHECK(u.ok);
        }
    }
    CHECK(compare(spv_vs3, N(spv_vs3), 0, 0, 0).vsout_after == 0);   // every Location output store goes under the twin
    if (argc > 1) {   // every vertex bundle of a pack, mask 0
        std::ifstream f(argv[1], std::ios::binary | std::ios::ate); if (!f) { printf("cannot read %s\n", argv[1]); return 2; }
        std::vector<uint8_t> d(size_t(f.tellg())); f.seekg(0); f.read(reinterpret_cast<char *>(d.data()), std::streamsize(d.size()));
        uint32_t h[4]; memcpy(h, d.data(), 16); if (h[0] != 0x50534758u || h[1] != 1) { printf("not an xgspack v1\n"); return 2; }
        size_t nvs = 0, worse = 0, better = 0, same = 0, st_before = 0, st_prune = 0, st_vsout = 0;
        for (uint32_t i = 0; i < h[2]; i++) {
            const uint8_t *e = d.data() + 16 + size_t(i) * 56; uint64_t off; uint32_t size; memcpy(&off, e + 32, 8); memcpy(&size, e + 40, 4);
            uint32_t b[8]; if (off + 32 > d.size()) continue; memcpy(b, d.data() + off, 32);
            const size_t so = 32 + size_t(b[3]) * 32 + size_t(b[4]) * 4 + b[5];
            if (b[0] != 0x31534758u || so + b[6] != size || b[2] != 0) continue;
            std::vector<uint32_t> w(b[6] / 4); memcpy(w.data(), d.data() + off + so, b[6]);
            if (!xbg_spirv_vs_output_mask(w.data(), w.size())) continue;
            if (argc > 3) { char k[33] = {}; memcpy(k, e, 32); if (!strcmp(k, argv[2])) {   // debug: <key> <out prefix> writes the modules
                std::vector<uint32_t> vo; xbg_spirv_vsout_trim(w.data(), w.size(), 0, vo, nullptr, 0);
                FILE *o = fopen((std::string(argv[3]) + ".in.spv").c_str(), "wb"); if (o) { fwrite(w.data(), 4, w.size(), o); fclose(o); }
                o = fopen((std::string(argv[3]) + ".vsout.spv").c_str(), "wb"); if (o) { fwrite(vo.data(), 4, vo.size(), o); fclose(o); } } }
            R t = compare(w.data(), w.size(), 0, 0, 0); nvs++;
            st_before += t.before; st_prune += t.prune_after; st_vsout += t.vsout_after;
            if (!t.ok) { if (++worse <= 8) { char k[33] = {}; memcpy(k, e, 32); printf("worse: %s %zu -> prune %zu vsout %zu\n", k, t.before, t.prune_after, t.vsout_after); } }
            else if (t.vsout_after < t.prune_after) better++; else same++;
        }
        printf("pack: %zu vertex shaders at mask 0: Location stores %zu -> nr_mat_prune %zu, xbg_spirv_vsout %zu; vsout fewer %zu, equal %zu, more / refused %zu\n",
               nvs, st_before, st_prune, st_vsout, better, same, worse);
        CHECK(worse == 0 && nvs > 0);
    }
    printf("%s: %d / %d checks\n", fails ? "FAILED" : "all passed", checks - fails, checks);
    return fails ? 1 : 0;
}
