// [DOS2] N5 (AUDIT-FINAL §2 #3, NR-27 x NR-10b): the NR-10b position program (xbg_spirv_pos_prog) of every vertex bundle of a pack, built
// from the words the phone gives the pipeline - the load-time clean-ups first (tidy_rewrite order: n NaN min / max, f LDC fold, t forwarded
// temporaries, c LDC canon, i F2I no-trunc), then NR-27's DM_NR_UBOVEC rewrite (xbg_spirv_ubovec, last, as in xbg_game.cpp tidy_rewrite) -
// against the same chain without the rewrite. The two programs must be equal for every shader: same accept / refuse, same reason, same
// instructions, inputs, uniform bytes, exactness. Exit 1 on any difference, on a failed xbg_spirv_ubovec_check, or on an empty pack.
//   g++ -std=c++17 -O2 -Wall -Wextra dm/gfx/vulkan/engine/tests/nr_cover_chain_test.cpp dm/gfx/vulkan/engine/xbg_spirv_pos.cpp
//       dm/gfx/vulkan/engine/xbg_spirv_ubovec.cpp dm/gfx/vulkan/engine/xbg_spirv_ldc.cpp dm/gfx/vulkan/engine/xbg_spirv_temps.cpp
//       dm/gfx/vulkan/engine/xbg_spirv_peep.cpp -o nr_cover_chain_test     (one command line)
//   ./nr_cover_chain_test <dir with *.xgs> [chain=nftci] [programs.txt] [spv dir]
//     chain: the clean-ups in front of the rewrite ("-" none). With programs.txt: the program of every accepted shader AFTER the rewrite
//     (the nr_cover_test --census format), and with spv dir its final module (<key>.spv) for
//       python tests/nr_cover_census.py <dir> programs.txt - <spv dir>      (interpreter run of the final words, 24 draws each)
//   Also counted: the raw bundle's program (what NR-10b's census proved) against the chain's, so a clean-up that changes coverage shows.
// The n step is the 10 lines of xbg_game.cpp nan_minmax_swap (sat = 0) copied here: that function lives in the engine's big file.
#include "../xbg_spirv_pos.h"
#include "../xbg_spirv_ubovec.h"
#include "../xbg_spirv_ldc.h"
#include "../xbg_spirv_temps.h"
#include "../xbg_spirv_peep.h"
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <map>
#include <string>
#include <vector>

static size_t nan_swap(std::vector<uint32_t> &w) {   // = xbg_game.cpp nan_minmax_swap(w, false): FMin / FMax / FClamp -> NMin / NMax / NClamp
    uint32_t glsl = 0; size_t k = 0;
    for (size_t i = 5; i < w.size();) {
        const uint32_t op = w[i] & 0xFFFFu, n = w[i] >> 16; if (!n || i + n > w.size()) break;
        if (op == 11 && n >= 5 && !memcmp(&w[i + 2], "GLSL.std.450", 13)) glsl = w[i + 1];
        else if (op == 12 && n >= 5 && glsl && w[i + 3] == glsl) { uint32_t &e = w[i + 4];
            if (e == 37) { e = 79; ++k; } else if (e == 40) { e = 80; ++k; } else if (e == 43) { e = 81; ++k; } }
        i += n;
    }
    return k;
}
static void chain(std::vector<uint32_t> &w, const char *c) {
    std::vector<uint32_t> o;
    if (strchr(c, 'n')) nan_swap(w);
    if (strchr(c, 'f') && xbg_spirv_ldc_fold(w.data(), w.size(), o)) w.swap(o);
    if (strchr(c, 't') && xbg_spirv_forward_temps(w.data(), w.size(), o)) w.swap(o);
    if (strchr(c, 'c') && xbg_spirv_ldc_canon(w.data(), w.size(), o)) w.swap(o);
    if (strchr(c, 'i') && xbg_spirv_f2i_notrunc(w.data(), w.size(), o)) w.swap(o);
}
struct Prog { nr_cv_prog p{}; std::string why; };
static Prog prog(const std::vector<uint32_t> &w) { Prog r; const char *why = ""; xbg_spirv_pos_prog(w.data(), w.size(), &r.p, &why); r.why = why ? why : ""; return r; }
static bool same(const Prog &a, const Prog &b) {
    const nr_cv_prog &x = a.p, &y = b.p;
    if (x.ok != y.ok || a.why != b.why) return false;
    if (!x.ok) return true;
    if (x.n != y.n || x.exact != y.exact || x.nubo != y.nubo || x.in_mask != y.in_mask || memcmp(x.out, y.out, sizeof x.out)) return false;
    for (unsigned i = 0; i < x.nubo; i++) if (x.ubo[i] != y.ubo[i] || x.ubo_bytes[i] != y.ubo_bytes[i]) return false;
    for (unsigned i = 0; i < x.n; i++) { const nr_cv_ins &p = x.ins[i], &q = y.ins[i];
        if (p.op != q.op || p.a != q.a || p.b != q.b || p.c != q.c || memcmp(&p.k, &q.k, 4)) return false; }
    return true;
}
static const char *opname[NR_CV_OPS] = {"const", "input", "ubo", "add", "sub", "mul", "div", "neg", "fma", "abs", "min", "max"};

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: see the head of tests/nr_cover_chain_test.cpp\n"); return 2; }
    const char *dir = argv[1], *ch = argc > 2 ? argv[2] : "nftci";
    FILE *out = argc > 3 ? fopen(argv[3], "w") : nullptr; const std::string spv = argc > 4 ? argv[4] : "";
    DIR *d = opendir(dir); if (!d) { printf("cannot open %s\n", dir); return 2; }
    long vs = 0, acc_raw = 0, acc_chain = 0, acc_u = 0, exact_u = 0, u_changed = 0, u_changed_acc = 0, u_changed_acc_chain = 0, u_bad = 0, diff = 0, raw_diff = 0;
    std::map<std::string, long> why_u;
    while (dirent *e = readdir(d)) {
        const std::string name = e->d_name; if (name.size() < 5 || name.substr(name.size() - 4) != ".xgs") continue;
        FILE *f = fopen((std::string(dir) + "/" + name).c_str(), "rb"); if (!f) continue;
        std::vector<uint8_t> b; uint8_t buf[65536]; size_t got; while ((got = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + got); fclose(f);
        if (b.size() < 32) continue;
        uint32_t h[8]; memcpy(h, b.data(), 32);
        if (h[0] != 0x31534758u || h[2] != 0) continue;   // XGS1, stage 0 = vertex
        const size_t o = 32 + size_t(h[3]) * 32 + size_t(h[4]) * 4 + h[5]; if (o + h[6] > b.size()) continue;
        const std::string key = name.substr(0, name.size() - 4);
        std::vector<uint32_t> w(h[6] / 4); memcpy(w.data(), b.data() + o, w.size() * 4); ++vs;
        const Prog raw = prog(w); acc_raw += raw.p.ok;
        chain(w, ch); const Prog pc = prog(w); acc_chain += pc.p.ok;
        if (!same(raw, pc)) { if (++raw_diff <= 8) printf("note %s: the clean-ups %s change the program (raw ok=%u '%s', after ok=%u '%s')\n", key.c_str(), ch, raw.p.ok, raw.why.c_str(), pc.p.ok, pc.why.c_str()); }
        std::vector<uint32_t> u; XbgUbovecStats st; const size_t k = xbg_spirv_ubovec(w.data(), w.size(), u, &st);
        if (k) { const char *bad = xbg_spirv_ubovec_check(w.data(), w.size(), u.data(), u.size());
            if (*bad) { ++u_bad; printf("FAIL %s: xbg_spirv_ubovec_check: %s\n", key.c_str(), bad); continue; }
            ++u_changed; if (pc.p.ok) ++u_changed_acc; }
        const std::vector<uint32_t> &fin = k ? u : w;
        const Prog pu = prog(fin); acc_u += pu.p.ok; exact_u += pu.p.ok && pu.p.exact; ++why_u[pu.why];
        if (k && pc.p.ok) ++u_changed_acc_chain;
        if (!same(pc, pu)) { ++diff; printf("DIFF %s: DM_NR_UBOVEC=1 changes the position program (before ok=%u '%s' n=%u, after ok=%u '%s' n=%u; %zu reads rewritten)\n",
                                       key.c_str(), pc.p.ok, pc.why.c_str(), pc.p.n, pu.p.ok, pu.why.c_str(), pu.p.n, k); }
        if (pu.p.ok && out) { const nr_cv_prog &p = pu.p;
            fprintf(out, "%s n=%u exact=%u in=%#x out=%u,%u,%u,%u", key.c_str(), p.n, p.exact, p.in_mask, p.out[0], p.out[1], p.out[2], p.out[3]);
            for (unsigned i = 0; i < p.n; i++) { const nr_cv_ins &x = p.ins[i]; uint32_t kb; memcpy(&kb, &x.k, 4); fprintf(out, " %s:%u:%u:%u:%08x", opname[x.op], x.a, x.b, x.c, kb); }
            fputc('\n', out);
            if (!spv.empty()) { FILE *s = fopen((spv + "/" + key + ".spv").c_str(), "wb"); if (!s || fwrite(fin.data(), 4, fin.size(), s) != fin.size()) { printf("cannot write %s/%s.spv\n", spv.c_str(), key.c_str()); return 2; } fclose(s); } }
    }
    closedir(d); if (out) fclose(out);
    printf("nr_cover_chain_test %s (chain %s): %ld vertex modules; position program raw %ld, after the clean-ups %ld (%ld differ from raw), after DM_NR_UBOVEC %ld (exact %ld)\n",
           dir, ch, vs, acc_raw, acc_chain, raw_diff, acc_u, exact_u);
    printf("  DM_NR_UBOVEC rewrote %ld modules (check failed %ld), %ld of them have a position program before it; programs changed by it: %ld\n",
           u_changed, u_bad, u_changed_acc, diff);
    for (auto &k : why_u) printf("  %7ld  %s\n", k.second, k.first.c_str());
    const bool ok = vs > 0 && !diff && !u_bad;
    printf(ok ? "PASS: NR-27 leaves every NR-10b position program as it was\n" : "FAIL\n");
    return ok ? 0 : 1;
}
