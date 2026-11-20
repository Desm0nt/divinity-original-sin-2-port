// [NR-16] DM_NR_CPU: the pure part of the translator's counters (xbg_cpuprof.inc) - render passes by signature with their draws,
// pipeline binds, distinct pipelines and descriptor-set binds, and whether a pass is order-free (the NR-21 candidate test).
// No Vulkan, no globals: tests/cpuprof_pass_test.cpp runs it.
//
// Order-free pass ("depth-only"): a depth attachment, and every draw in it writes no colour (no colour target bound, or every bound
// target's channel mask is 0), tests depth with one function for the whole pass, LESS or LEQUAL (NVN 2 / 4), writes depth or counts
// samples, and has no stencil test. Its final depth is the minimum over its fragments whatever the order of the draws, so sorting the
// draws by pipeline cannot change it (the sample-count pairs of NR-03 travel with their draw). "distinct" = the pipelines a pass binds
// at least once = the binds left if its draws were sorted by pipeline; binds - distinct = what sorting saves.
#ifndef XBG_CPUPROF_H
#define XBG_CPUPROF_H
#include <stdint.h>
#include <string.h>

struct CpfSig {
    uint32_t w = 0, h = 0, colors = 0; bool depth = false;
    uint64_t passes = 0, draws = 0, binds = 0, distinct = 0, set_binds = 0, counting = 0;
    uint64_t free_passes = 0, free_draws = 0, free_binds = 0, free_distinct = 0;   // the order-free passes among them
};
struct CpfPasses {
    static constexpr int kSigs = 48, kSet = 512;
    CpfSig sig[kSigs]; int nsig = 0; CpfSig other;   // other: signatures past kSigs
    // the open pass
    bool open = false; int cur = -1; uint64_t draws = 0, binds = 0, distinct = 0, set_binds = 0, counting = 0; bool order_free = true; uint32_t df = 0;
    uint64_t set[kSet]; uint32_t set_gen[kSet]; uint32_t gen = 1; bool overflow = false;
    CpfPasses() { memset(set, 0, sizeof set); memset(set_gen, 0, sizeof set_gen); }
    void begin(uint32_t w, uint32_t h, uint32_t colors, bool depth) {
        end();
        cur = -1; for (int i = 0; i < nsig; i++) if (sig[i].w == w && sig[i].h == h && sig[i].colors == colors && sig[i].depth == depth) { cur = i; break; }
        if (cur < 0 && nsig < kSigs) { cur = nsig++; sig[cur] = CpfSig{}; sig[cur].w = w; sig[cur].h = h; sig[cur].colors = colors; sig[cur].depth = depth; }
        open = true; draws = binds = distinct = set_binds = counting = 0; order_free = depth; df = 0; overflow = false;
        if (++gen == 0) { memset(set_gen, 0, sizeof set_gen); gen = 1; }
    }
    // One draw: its pipeline, whether a bind was recorded for it, its colour writes, depth test / write / function, stencil test, counting
    void draw(uint64_t pipe, bool bound, bool color_written, bool dt, bool dw, uint32_t dfunc, bool st, bool counts) {
        if (!open) return;
        ++draws; if (bound) ++binds; if (counts) ++counting;
        if (color_written || st || !dt || !(dfunc == 2 || dfunc == 4) || (df && df != dfunc) || !(dw || counts)) order_free = false;
        df = dfunc;
        if (!overflow) {
            uint32_t i = uint32_t((pipe * 0x9E3779B97F4A7C15ull) >> 55) & (kSet - 1);
            for (int probe = 0;; probe++) {
                if (probe == kSet) { overflow = true; break; }
                if (set_gen[i] != gen) { set_gen[i] = gen; set[i] = pipe; ++distinct; break; }
                if (set[i] == pipe) break;
                i = (i + 1) & (kSet - 1);
            }
        }
    }
    void set_bind() { if (open) ++set_binds; }
    void end() {
        if (!open) return;
        open = false;
        CpfSig &s = cur >= 0 ? sig[cur] : other;
        ++s.passes; s.draws += draws; s.binds += binds; s.distinct += overflow ? binds : distinct; s.set_binds += set_binds; s.counting += counting;
        if (order_free && draws) { ++s.free_passes; s.free_draws += draws; s.free_binds += binds; s.free_distinct += overflow ? binds : distinct; }
    }
    void reset() { open = false; nsig = 0; other = CpfSig{}; }
};
#endif
