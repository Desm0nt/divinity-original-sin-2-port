// [NR-16] DM_NR_CPU (docs/dos2/nr/NR-16.md): the translator's side of the CPU attribution run. Included by xbg_game.cpp at file scope
// after xbg_present_thread.inc (it reads nbx:: / ptq:: / nbmt:: modes).
//   DM_NR_CPU  0 / unset  every hook below is one flag test (cpf::on false)
//              1          (a) the translation thread's own CPU time (CLOCK_THREAD_CPUTIME_ID) next to the wall time of every submission and
//                         present: wall - CPU = the time it waited (GPU fences, the DM_NB_MX worker, the queue lock); (b) the GPU time of
//                         the engine's submissions (their two timestamps) and the gaps between consecutive ones (present blits + GPU idle):
//                         in a GPU-bound frame fps = 1000 / (busy + gap); (c) render passes by signature: draws, pipeline binds, the
//                         distinct pipelines each pass binds, descriptor-set binds, and the order-free (depth-only) passes of NR-21
//                         (xbg_cpuprof.h); (d) with DM_NB_MX=1 the worker's CPU time (xbg_nb_mx.inc). Every 10 s: "[cpuprof] gfx",
//                         "[cpuprof] passes", "[cpuprof] passes by signature".
// Cost: two clock_gettime(THREAD_CPUTIME) per submission and per present, a few adds and a 512-slot probe per draw (~10-20 ns).
#include "xbg_cpuprof.h"
#include <time.h>
namespace {
namespace cpf {
bool on = false;
CpfPasses passes;
uint64_t rep_ns = 0, rep_frames = 0, subs = 0, sub_wall = 0, sub_cpu = 0, pres = 0, pres_wall = 0, pres_cpu = 0;
uint64_t gpu_busy = 0, gpu_gap = 0, gpu_n = 0, gpu_overlap = 0, gpu_idle_long = 0, last_end = 0; bool have_end = false;
uint64_t mx_cpu0 = 0;
inline uint64_t tcpu() { if (!on) return 0; struct timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec); }
void init() {
    const char *e = getenv("DM_NR_CPU"); on = e && atoi(e) > 0;
    nrcpu_on = on;   // [DOS2] NR-CPU: the NRCPU_SCOPE splits of "other" / "draw" (xbg_cpu_timing.h)
    if (on) xbg_logf("[cpuprof] DM_NR_CPU=1 (translator): thread CPU vs wall of submissions / presents, GPU busy and gaps, passes by signature with pipeline binds and the order-free ones; lines every 10 s");
}
inline void pass_open(uint32_t w, uint32_t h, uint32_t colors, bool depth) { if (on) passes.begin(w, h, colors, depth); }
// One draw at its pipeline bind: bound = a vkCmdBindPipeline is recorded for it; colors = the bound colour targets (bit per slot),
// mask = the channel masks (4 bits a target)
inline void draw(uint64_t pipe, bool bound, uint32_t colors, uint32_t mask, const ds_st &ds, bool counts) {
    if (!on) return;
    bool cw = false; for (int i = 0; i < 8; i++) if ((colors >> i & 1) && (mask >> (4 * i) & 0xF)) { cw = true; break; }
    passes.draw(pipe, bound, cw, ds.dt != 0, ds.dw != 0, uint32_t(ds.df), ds.st != 0, counts);
}
inline void set_bind() { if (on) passes.set_bind(); }
// The two timestamps of a retired submission (raw values, the timestamp mask and period)
inline void gpu_span(uint64_t t0, uint64_t t1, uint64_t mask, double period) {
    if (!on) return;
    gpu_busy += uint64_t(double((t1 - t0) & mask) * period); ++gpu_n;
    if (have_end) {
        const uint64_t d = (t0 - last_end) & mask;
        if (d > (mask >> 1)) ++gpu_overlap;                        // started before the previous one ended (or out of order)
        else { const uint64_t ns = uint64_t(double(d) * period); if (ns < 200000000ull) gpu_gap += ns; else ++gpu_idle_long; }   // a gap over 200 ms is a pause, not a bubble
    }
    last_end = t1; have_end = true;
}
void report(uint64_t now);
inline void presented(uint64_t wall, uint64_t c0) { if (!on) return; ++pres; pres_wall += wall; pres_cpu += tcpu() - c0; }
inline void submitted(uint64_t wall, uint64_t c0) {
    if (!on) return;
    ++subs; sub_wall += wall; sub_cpu += tcpu() - c0;
    const uint64_t now = timing_clock();
    if (!rep_ns) { rep_ns = now; rep_frames = stats.presents.load(); mx_cpu0 = nbx::cpu_total_ns; return; }
    if (now - rep_ns >= 10000000000ull) report(now);
}
void report(uint64_t now) {
    passes.end();   // the open pass counts in this window; the next draw of it starts a new record
    const uint64_t fr = stats.presents.load(); const double sec = double(now - rep_ns) / 1e9, frames = double(fr - rep_frames), f = frames > 0 ? frames : 1;
    const double mx_ms = double(nbx::cpu_total_ns - mx_cpu0) / 1e6 / f;
    xbg_logf("[cpuprof] gfx (%.1f s, %.0f frames = %.1f fps; DM_NB_MX=%d DM_GFX_PRESENT_THREAD=%d DM_NB_MT=%d): translation thread a frame: submit wall %.3f ms CPU %.3f ms "
             "(waited %.3f), present wall %.3f ms CPU %.3f ms (waited %.3f), %.2f submissions a frame; MX worker CPU %.3f ms a frame; GPU a frame: engine submissions %.3f ms, "
             "between them %.3f ms (present blits + idle; %llu overlapped, %llu pauses over 200 ms); %.2f retired submissions a frame",
             sec, frames, frames / sec, nbx::mode, ptq::mode, nbmt::mode, double(sub_wall) / 1e6 / f, double(sub_cpu) / 1e6 / f, double(sub_wall > sub_cpu ? sub_wall - sub_cpu : 0) / 1e6 / f,
             double(pres_wall) / 1e6 / f, double(pres_cpu) / 1e6 / f, double(pres_wall > pres_cpu ? pres_wall - pres_cpu : 0) / 1e6 / f, double(subs) / f, mx_ms,
             double(gpu_busy) / 1e6 / f, double(gpu_gap) / 1e6 / f, (unsigned long long)gpu_overlap, (unsigned long long)gpu_idle_long, double(gpu_n) / f);
    // passes: totals and the order-free ones (NR-21)
    CpfSig t{}, fr_{};
    for (int i = 0; i <= passes.nsig; i++) { const CpfSig &s = i < passes.nsig ? passes.sig[i] : passes.other;
        t.passes += s.passes; t.draws += s.draws; t.binds += s.binds; t.distinct += s.distinct; t.set_binds += s.set_binds; t.counting += s.counting;
        fr_.passes += s.free_passes; fr_.draws += s.free_draws; fr_.binds += s.free_binds; fr_.distinct += s.free_distinct; }
    xbg_logf("[cpuprof] passes (%.1f s, a frame): %.1f passes with draws, %.0f draws, %.0f pipeline binds (%.0f if every pass were sorted by pipeline), %.0f set binds, %.0f draws counting samples; "
             "order-free (depth only, one LESS / LEQUAL, no colour, no stencil): %.1f passes, %.0f draws, %.0f binds -> %.0f sorted = %.0f binds saved a frame",
             sec, double(t.passes) / f, double(t.draws) / f, double(t.binds) / f, double(t.distinct) / f, double(t.set_binds) / f, double(t.counting) / f,
             double(fr_.passes) / f, double(fr_.draws) / f, double(fr_.binds) / f, double(fr_.distinct) / f, double(fr_.binds - fr_.distinct) / f);
    // by signature, most draws first (w x h, colour targets, d = depth: passes / draws / binds / distinct / sets, F = order-free passes)
    char line[960]; size_t o = 0; line[0] = 0; bool used[CpfPasses::kSigs] = {};
    for (int k = 0; k < 10 && o < sizeof line - 96; k++) {
        int best = -1; for (int i = 0; i < passes.nsig; i++) if (!used[i] && passes.sig[i].draws && (best < 0 || passes.sig[i].draws > passes.sig[best].draws)) best = i;
        if (best < 0) break; used[best] = true; const CpfSig &s = passes.sig[best];
        o += size_t(snprintf(line + o, sizeof line - o, " %ux%u c%u%s %.1f/%.0f/%.0f/%.0f/%.0f F%.1f;", s.w, s.h, s.colors, s.depth ? "d" : "", double(s.passes) / f, double(s.draws) / f,
                             double(s.binds) / f, double(s.distinct) / f, double(s.set_binds) / f, double(s.free_passes) / f));
    }
    xbg_logf("[cpuprof] passes by signature (a frame: passes/draws/binds/distinct/sets, F = order-free passes):%s%s", line, passes.other.passes ? " (+ more signatures)" : "");
    passes.reset();
    subs = sub_wall = sub_cpu = pres = pres_wall = pres_cpu = gpu_busy = gpu_gap = gpu_n = gpu_overlap = gpu_idle_long = 0;
    rep_ns = now; rep_frames = fr; mx_cpu0 = nbx::cpu_total_ns;
}
}   // namespace cpf
}   // namespace
