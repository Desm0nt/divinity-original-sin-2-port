// [DOS2] NR-POSONLY (docs/dos2/nr/NR-POSONLY.md) host test, Windows clang++ / any C++17:
//   1. nrpo_gather against a byte-by-byte model on random layouts (bit-exact: the same bytes, nothing converted), nrpo_nverts;
//   2. the arena: random alloc / free with a retire delay against an occupancy model - two live copies never overlap, a freed range is
//      never handed out again before serial > freed + delay, everything fits in [0, size);
//   3. (with a pack) xbg_spirv_posonly on every vertex shader of tools/dos2_posonly_check.py's table against its static verdicts (the C++
//      walk must agree with the Python one on every shader) and never "position-only" where the interpreter saw gl_Position move.
// usage: nr_posonly_test [pack.xgspack posonly.tsv]
#include "../../../nr/nr_posonly.h"
#include "../xbg_spirv_posonly.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
static int checks, fails;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; if (fails < 20) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static uint64_t rng = 0x1234abcd9876ull;
static uint64_t rnd() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static void test_gather() {
    for (int t = 0; t < 4000; t++) {
        const uint32_t psz = (uint32_t[]){4, 6, 8, 12, 16}[rnd() % 5], stride = psz + 1 + (uint32_t)(rnd() % 40), pos_off = (uint32_t)(rnd() % (stride - psz + 1));
        const uint64_t bytes = rnd() % 5000;
        std::vector<uint8_t> src(bytes + 64); for (auto &b : src) b = (uint8_t)rnd();
        const uint64_t nv = nrpo_nverts(bytes, stride, pos_off, psz);
        uint64_t model = 0; while (model * stride + pos_off + psz <= bytes) ++model;
        CHECK(nv == model, "nverts %llu vs %llu (bytes %llu stride %u off %u size %u)", (unsigned long long)nv, (unsigned long long)model, (unsigned long long)bytes, stride, pos_off, psz);
        std::vector<uint8_t> dst(nv * psz + 1, 0xee);
        nrpo_gather(dst.data(), src.data(), nv, stride, pos_off, psz);
        bool same = dst[nv * psz] == 0xee;
        for (uint64_t i = 0; i < nv && same; i++) for (uint32_t b = 0; b < psz; b++) if (dst[i * psz + b] != src[i * stride + pos_off + b]) { same = false; break; }
        CHECK(same, "gather differs (stride %u off %u size %u, %llu vertices) or wrote past the copy", stride, pos_off, psz, (unsigned long long)nv);
    }
    CHECK(nrpo_fmt_size(106) == 12 && nrpo_fmt_size(90) == 6 && nrpo_fmt_size(103) == 8 && nrpo_fmt_size(109) == 16 && nrpo_fmt_size(1234) == 0, "format sizes");
}

static void test_arena() {
    static nrpo_arena a; nrpo_arena_init(&a, 1 << 20);
    struct L { uint64_t off, n; }; std::vector<L> live; struct F { uint64_t off, n, serial; }; std::vector<F> freed;
    const uint64_t delay = 3; uint64_t bad_overlap = 0, bad_early = 0, bad_range = 0, allocs = 0;
    for (uint64_t serial = 1; serial < 4000; serial++) {
        const int ops = (int)(rnd() % 6);
        for (int o = 0; o < ops; o++) {
            if (live.empty() || rnd() % 3) {
                const uint64_t n = 1 + rnd() % 20000; const uint64_t off = nrpo_alloc(&a, n, serial, delay);
                if (off == ~0ull) continue;
                ++allocs; const uint64_t r = (n + 63) & ~63ull;
                if (off + r > a.size || off % 64) ++bad_range;
                for (auto &l : live) if (off < l.off + ((l.n + 63) & ~63ull) && l.off < off + r) ++bad_overlap;
                for (auto &f : freed) if (serial <= f.serial + delay && off < f.off + ((f.n + 63) & ~63ull) && f.off < off + r) ++bad_early;
                live.push_back({off, n});
            } else {
                const size_t i = (size_t)(rnd() % live.size()); nrpo_free(&a, live[i].off, live[i].n, serial);
                freed.push_back({live[i].off, live[i].n, serial}); live[i] = live.back(); live.pop_back();
            }
        }
        std::vector<F> keep; for (auto &f : freed) if (serial <= f.serial + delay + 1) keep.push_back(f); freed.swap(keep);   // older ones may be reused freely
    }
    CHECK(allocs > 1000, "the arena test ran (%llu allocations)", (unsigned long long)allocs);
    CHECK(!bad_range && !bad_overlap && !bad_early, "arena: out of range %llu, overlapping live copies %llu, reused before the retire delay %llu",
          (unsigned long long)bad_range, (unsigned long long)bad_overlap, (unsigned long long)bad_early);
    CHECK(a.reused > 100, "freed ranges are reused (%llu)", (unsigned long long)a.reused);
}

static std::vector<uint8_t> read_file(const char *p) { std::vector<uint8_t> d; FILE *f = fopen(p, "rb"); if (!f) return d; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); d.resize((size_t)n); if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear(); fclose(f); return d; }
static void test_pack(const char *pack, const char *tsv) {
    std::vector<uint8_t> d = read_file(pack); if (d.size() < 16) { printf("nr_posonly_test: no pack at %s, part 3 skipped\n", pack); return; }
    uint32_t h[4]; memcpy(h, d.data(), 16);
    std::map<std::string, std::pair<uint64_t, uint32_t>> idx;
    for (uint32_t i = 0; i < h[2]; i++) { const uint8_t *e = d.data() + 16 + 56 * i; char k[33] = {0}; memcpy(k, e, 32); uint64_t o; uint32_t sz; memcpy(&o, e + 32, 8); memcpy(&sz, e + 40, 4); idx[k] = {o, sz}; }
    FILE *f = fopen(tsv, "r"); if (!f) { printf("nr_posonly_test: no table at %s, part 3 skipped\n", tsv); return; }
    char line[4096]; std::map<std::string, std::pair<int, std::string>> want; if (!fgets(line, sizeof line, f)) { fclose(f); return; }
    while (fgets(line, sizeof line, f)) { char *col[10] = {0}; int n = 0; char *p = line; col[n++] = p;
        while (*p && n < 10) { if (*p == '\t') { *p = 0; col[n++] = p + 1; } p++; }
        if (n < 9) continue; std::string interp = col[7]; want[col[0]] = {atoi(col[5]), interp}; }
    fclose(f);
    int n = 0, agree = 0, interp_bad = 0, ok = 0;
    for (auto &w : want) {
        auto it = idx.find(w.first); if (it == idx.end()) continue;
        const uint8_t *b = d.data() + it->second.first; uint32_t bh[8]; memcpy(bh, b, 32);
        const size_t o = 32 + (size_t)bh[3] * 32 + (size_t)bh[4] * 4 + bh[5]; std::vector<uint32_t> words(bh[6] / 4); memcpy(words.data(), b + o, words.size() * 4);
        const bool po = xbg_spirv_posonly(words.data(), words.size());
        ++n; agree += (int)po == w.second.first; ok += po;
        if (po && w.second.second == "False") ++interp_bad;
        if ((int)po != w.second.first && agree + 3 > n) printf("  differs: %s C++ %d python %d\n", w.first.c_str(), (int)po, w.second.first);
    }
    CHECK(n > 0 && agree == n, "C++ walk agrees with the Python walk on %d of %d vertex shaders", agree, n);
    CHECK(interp_bad == 0, "C++ says position-only where the interpreter saw gl_Position move: %d", interp_bad);
    printf("nr_posonly_test: pack: %d vertex shaders, %d position-only (C++ = Python on %d)\n", n, ok, agree);
}

int main(int argc, char **argv) {
    test_gather(); test_arena();
    if (argc >= 3) test_pack(argv[1], argv[2]);
    printf("nr_posonly_test: %d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
