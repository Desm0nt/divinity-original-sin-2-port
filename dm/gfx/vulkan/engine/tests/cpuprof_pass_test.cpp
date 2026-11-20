// [NR-16] host test of ../xbg_cpuprof.h (DM_NR_CPU, the translator's pass counters): a deck-like frame - a depth pre-pass whose draws
// alternate 6 pipelines (order-free: the binds sorting would save are binds - 6), a shadow atlas pass with LESS, a G-buffer pass with
// colour, a depth pass with a stencil test, one with two depth functions, one where a draw writes no depth and counts nothing, one
// with sample-counting draws (still order-free), colour targets bound with a zero channel mask (still depth-only); signatures merge
// across frames; more than 512 distinct pipelines in one pass (distinct = binds, flagged by overflow); more than 48 signatures
// (the rest in "other"); reset.
// Build: clang++ -std=c++17 -Wall -Wextra cpuprof_pass_test.cpp -o cpuprof_pass_test && ./cpuprof_pass_test   (or g++)
#include "../xbg_cpuprof.h"
#include <stdio.h>
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
enum { NEVER = 1, LESS = 2, EQUAL = 3, LEQUAL = 4, ALWAYS = 8 };
// one draw with the bind cache of the engine: bound when the pipeline differs from the last one bound in this pass
struct Feed { CpfPasses &p; uint64_t last = 0; void pass(uint32_t w, uint32_t h, uint32_t c, bool d) { p.begin(w, h, c, d); last = 0; }
    void draw(uint64_t pipe, bool cw, bool dt, bool dw, uint32_t df, bool st, bool counts) { p.draw(pipe, pipe != last, cw, dt, dw, df, st, counts); last = pipe; } };
static const CpfSig *find(const CpfPasses &p, uint32_t w, uint32_t h, uint32_t c, bool d) { for (int i = 0; i < p.nsig; i++) if (p.sig[i].w == w && p.sig[i].h == h && p.sig[i].colors == c && p.sig[i].depth == d) return &p.sig[i]; return nullptr; }
static void frame(Feed &f) {
    f.pass(2160, 972, 0, true);   // pre-pass: 300 draws over 6 pipelines, round robin -> every draw binds
    for (int i = 0; i < 300; i++) f.draw(0x1000 + i % 6, false, true, true, LEQUAL, false, false);
    f.pass(4096, 4096, 0, true);  // shadow atlas: 120 draws, 3 pipelines in runs of 10
    for (int i = 0; i < 120; i++) f.draw(0x2000 + (i / 10) % 3, false, true, true, LESS, false, false);
    f.pass(2160, 972, 4, true);   // G-buffer with colour
    for (int i = 0; i < 200; i++) f.draw(0x3000 + i % 20, true, true, true, LEQUAL, false, false);
    f.pass(1080, 486, 0, true);   // stencil
    for (int i = 0; i < 10; i++) f.draw(0x4000 + i % 2, false, true, true, LEQUAL, true, false);
    f.pass(1080, 486, 0, true);   // two depth functions in one pass
    f.draw(0x5000, false, true, true, LESS, false, false); f.draw(0x5001, false, true, true, LEQUAL, false, false);
    f.pass(512, 512, 0, true);    // a draw that neither writes depth nor counts samples: not order-free
    f.draw(0x6000, false, true, true, LESS, false, false); f.draw(0x6001, false, true, false, LESS, false, false);
    f.pass(256, 256, 0, true);    // sample counting, no depth write: order-free (the query pair travels with its draw)
    for (int i = 0; i < 8; i++) f.draw(0x7000 + i % 4, false, true, false, LEQUAL, false, true);
    f.pass(128, 128, 2, false);   // no depth attachment: never order-free
    f.draw(0x8000, false, true, true, LESS, false, false);
}
int main() {
    static CpfPasses p; Feed f{p};
    frame(f); p.end();
    const CpfSig *pre = find(p, 2160, 972, 0, true), *sh = find(p, 4096, 4096, 0, true), *gb = find(p, 2160, 972, 4, true), *st = find(p, 1080, 486, 0, true),
                 *wd = find(p, 512, 512, 0, true), *cnt = find(p, 256, 256, 0, true), *nd = find(p, 128, 128, 2, false);
    CHECK(pre && sh && gb && st && wd && cnt && nd, "every signature recorded (%d)", p.nsig);
    if (!(pre && sh && gb && st && wd && cnt && nd)) { printf("FAIL\n"); return 1; }
    CHECK(pre->passes == 1 && pre->draws == 300 && pre->binds == 300 && pre->distinct == 6 && pre->free_passes == 1 && pre->free_binds == 300 && pre->free_distinct == 6, "pre-pass %llu/%llu/%llu free %llu",
          (unsigned long long)pre->draws, (unsigned long long)pre->binds, (unsigned long long)pre->distinct, (unsigned long long)pre->free_passes);
    CHECK(sh->binds == 12 && sh->distinct == 3 && sh->free_passes == 1, "shadow binds %llu distinct %llu", (unsigned long long)sh->binds, (unsigned long long)sh->distinct);
    CHECK(gb->binds == 200 && gb->distinct == 20 && gb->free_passes == 0, "G-buffer is not order-free");
    CHECK(st->passes == 2 && st->free_passes == 0 && st->draws == 12, "stencil / two functions: %llu passes %llu free", (unsigned long long)st->passes, (unsigned long long)st->free_passes);
    CHECK(wd->free_passes == 0, "a draw without depth write or counting breaks it");
    CHECK(cnt->free_passes == 1 && cnt->counting == 8 && cnt->distinct == 4, "counting pass order-free");
    CHECK(nd->free_passes == 0 && nd->draws == 1, "no depth attachment");
    // colour targets with a zero mask: the caller passes color_written = false -> still order-free
    f.pass(64, 64, 1, true); for (int i = 0; i < 4; i++) f.draw(0x9000 + i, false, true, true, LESS, false, false); p.end();
    CHECK(find(p, 64, 64, 1, true)->free_passes == 1, "masked colour target");
    // a second frame merges into the same signatures
    frame(f); p.end();
    CHECK(pre->passes == 2 && pre->draws == 600 && pre->distinct == 12 && pre->free_draws == 600, "merged: %llu passes", (unsigned long long)pre->passes);
    // an empty pass (begin, no draw): counted as a pass, not as an order-free one
    f.pass(32, 32, 0, true); p.end();
    CHECK(find(p, 32, 32, 0, true)->passes == 1 && find(p, 32, 32, 0, true)->free_passes == 0, "empty pass");
    // overflow of the distinct set: 700 different pipelines in one pass -> distinct falls back to binds
    f.pass(16, 16, 0, true); for (int i = 0; i < 700; i++) f.draw(0x100000 + uint64_t(i) * 7919, false, true, true, LESS, false, false); p.end();
    CHECK(find(p, 16, 16, 0, true)->distinct == 700 && find(p, 16, 16, 0, true)->binds == 700, "overflow distinct %llu", (unsigned long long)find(p, 16, 16, 0, true)->distinct);
    // the distinct set restarts every pass (generation) even after many passes
    for (int k = 0; k < 70000; k++) { f.pass(8, 8, 0, true); f.draw(0x42, false, true, true, LESS, false, false); f.draw(0x43, false, true, true, LESS, false, false); }
    p.end();
    CHECK(find(p, 8, 8, 0, true)->distinct == 140000 && find(p, 8, 8, 0, true)->passes == 70000, "generations: %llu", (unsigned long long)find(p, 8, 8, 0, true)->distinct);
    // more than kSigs signatures: the rest go to other
    p.reset();
    for (int k = 0; k < 60; k++) { f.pass(100 + k, 100, 0, true); f.draw(0x1, false, true, true, LESS, false, false); }
    p.end();
    CHECK(p.nsig == CpfPasses::kSigs && p.other.passes == 60 - CpfPasses::kSigs && p.other.draws == 60 - CpfPasses::kSigs, "other %llu", (unsigned long long)p.other.passes);
    // a draw outside any pass (begin not called after reset) is ignored
    p.reset(); p.draw(0x1, true, false, true, true, LESS, false, false); p.end();
    CHECK(p.nsig == 0 && p.other.passes == 0, "no pass, nothing counted");
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
