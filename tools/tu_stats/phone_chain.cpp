// N1 (docs/dos2/nr/NR-NSHADER.md): write the SPIR-V the phone hands vkCreateShaderModule for given pack keys, i.e. the bundle's module after
// the load-time chain of xbg_game.cpp tidy_rewrite as the Mi 14 Ultra log shows it on (DM_GFX_NAN_MINMAX + NAN_SAT, LDC_FOLD,
// FORWARD_TEMPS, LDC_CANON, F2I_NOTRUNC; NR-27 UBOVEC with "u" as on Mali). The production C++ passes, no copies (nan_minmax_swap is the
// one of tests/vsout_scan.cpp, itself a copy of xbg_game.cpp's).
//   phone_chain <shaders.xgspack> <keys.txt> <out dir> [chain=nftcp]     -> <out>/<key>.spv a key (stage of the bundle kept)
// Build (WSL): g++ -O2 -std=c++17 tools/tu_stats/phone_chain.cpp dm/gfx/vulkan/engine/xbg_spirv_{ldc,temps,peep,ubovec}.cpp -Idm/gfx/vulkan/engine
#include "xbg_spirv_ldc.h"
#include "xbg_spirv_peep.h"
#include "xbg_spirv_temps.h"
#include "xbg_spirv_ubovec.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
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
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: phone_chain <shaders.xgspack> <keys.txt> <out dir> [chain=nftcp]\n"); return 2; }
    const char *c = argc > 4 ? argv[4] : "nftcp";
    std::ifstream f(argv[1], std::ios::binary | std::ios::ate); if (!f) { fprintf(stderr, "cannot read %s\n", argv[1]); return 2; }
    std::vector<uint8_t> d(size_t(f.tellg())); f.seekg(0); f.read(reinterpret_cast<char *>(d.data()), std::streamsize(d.size()));
    uint32_t h[4]; memcpy(h, d.data(), 16); if (h[0] != 0x50534758u || h[1] != 1) { fprintf(stderr, "not an xgspack v1\n"); return 2; }
    std::map<std::string, std::pair<uint64_t, uint32_t>> at;
    for (uint32_t i = 0; i < h[2]; i++) { const uint8_t *e = d.data() + 16 + size_t(i) * 56; char key[33] = {}; memcpy(key, e, 32); uint64_t off; uint32_t size; memcpy(&off, e + 32, 8); memcpy(&size, e + 40, 4); at[key] = {off, size}; }
    std::ifstream kf(argv[2]); std::string k; int n = 0, miss = 0;
    while (kf >> k) {
        auto a = at.find(k); if (a == at.end()) { miss++; continue; }
        uint32_t b[8]; memcpy(b, d.data() + a->second.first, 32); const size_t so = 32 + size_t(b[3]) * 32 + size_t(b[4]) * 4 + b[5];
        if (b[0] != 0x31534758u || so + b[6] != a->second.second) { miss++; continue; }
        std::vector<uint32_t> w(b[6] / 4); memcpy(w.data(), d.data() + a->second.first + so, b[6]); chain(w, c);
        FILE *o = fopen((std::string(argv[3]) + "/" + k + ".spv").c_str(), "wb"); if (!o) return 2; fwrite(w.data(), 4, w.size(), o); fclose(o); n++;
    }
    printf("phone_chain %s: %d modules written, %d keys missing\n", c, n, miss);
    return 0;
}
