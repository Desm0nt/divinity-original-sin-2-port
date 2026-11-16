// [DOS2] NB-20b (ctx): DM_NB_MX - the recording context of the draw path, a translation worker and the segment profile
// (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-20b-ctx findings"; registry, worker and profile in dm/gfx/nb/nb_mx.h - the host test
// tests/nb_mx_test.cpp runs that header). Included by xbg_game.cpp at file scope right after xbg_nb_mt.inc.
//
// Context. The class C globals of the draw path (NB-20a) are no longer plain globals: each keeps its main instance `<name>_m0` and is
// read through `#define <name> nbx::get(<name>_m0, nbx::K_<name>)`. While no window is open (nbx::par false - always with DM_NB_MX=0)
// that is the main instance for every thread, as before: one load of a flag and a never-taken branch per access region. Inside a
// window a worker thread gets its own instance of every slot (nbx::tls), any other thread still the main one.
// tests/source_structure_test.py (block NB-20b ctx) keeps the slot list, the defines and the registrations in step.
//
//   DM_NB_MX       0  off
//                  1  continuation: every submission is translated by the worker thread dm-nbx0 (8 MB stack, the translation thread's
//                     CPU mask) while the translation thread waits: the whole context is copied main -> worker before and worker ->
//                     main after, so the worker continues exactly where the translation thread stood and the command stream is the
//                     one of 0. It proves the context mechanism and that nothing on the path depends on the thread it runs on; it is
//                     not faster. In-process A/B: gfx-ab-target = nbmx.
//   DM_NB_MX_PROF  1  segment profile (needs DM_NB=1 DM_NB_OWN=1/2 and DM_NB_MT=1/2/3: the packets are what is classified): every
//                     segment of a frame job by class (dm/gfx/nb/nb_mx.h: clean / pre-barrier / pre-xfer / tail / split / cont / ops /
//                     empty), by draws, with its translation time; "quiet" = clean or pre-barrier and no pipeline, image, layout,
//                     sampler, render pass or framebuffer created and no upload while it was translated; the model line gives the
//                     part's translation time if the quiet segments of DM_NB_MX_MIN (8) draws and more ran on 2 / 3 / 4 threads.
// Lines "[nb] mx ..." / "[nb] mx prof ..." every 10 s.
#include "../../nb/nb_mx.h"
#include <atomic>
// The pure headers xbg_game.cpp and its .inc files include further down are read here, ahead of every context define: their own
// identifiers (a parameter named tex_gen, a field named state) must not meet the macros. Each is guarded, so the later includes are empty.
#include "../../nb/nb_ir.h"
#include "../../nb/nb_slotmask.h"
#include "../../nb/nb_transient.h"
#include "../../nb/nb_pipe.h"
#include "../../nb/nb_hit.h"
#include "../../nb/nb_desc.h"
#include "../../nb/nb_order.h"
#include "../../nb/nb_own.h"
#include "../../nb/nb_fused.h"
#include "../../nb/nb_post.h"
#include "../../nb/nb_post_aa.h"
#include "../../nb/nb_ui.h"
#include "../../nb/nb_uiarea.h"
#include "xbg_store_hist.h"
namespace {
namespace nbx {
// One slot per context variable (NB-20a class C moved into the context). New entries go before K_COUNT; the variable's declaration in
// xbg_game.cpp carries the registration and the define.
enum Slot { K_g_ui_moff = 0, K_g_ui_moff_addr, K_storage_images_bound, K_snap_site, K_tex_gen, K_buf_gen, K_cur_op, K_draw_reads, K_draw_reads_overflow,
    K_img_front, K_pipe_memo_valid, K_pipe_memo_key, K_pipe_memo_next, K_pipe_memo_pipeline, K_desc_memo_local, K_memo_dyn_ok, K_memo_front_key,
    K_memo_front_it, K_memo_front_gen, K_memo_front_slot, K_desc_memo_images, K_desc_memo_dyn, K_desc_memo_recording, K_desc_memo_texel,
    K_in_descriptors, K_bind_cache, K_state, K_COUNT };
static_assert(int(K_COUNT) <= int(nbxq::MAX_VARS), "nb_mx.h: MAX_VARS");
nbxq::Registry registry;              // constant-initialised; filled by the initialisers next to the variables
std::atomic<bool> par{false};         // a window is open: worker threads read their own instances
thread_local void **tls = nullptr;    // a worker's slot table (null on every other thread)
// The window path is out of line and cold: with no window (always with DM_NB_MX=0) an access is a flag load and a never-taken branch.
__attribute__((noinline, cold)) void *window_instance(void *main, int slot) { void **t = tls; return t ? t[slot] : main; }
template <class T> inline T &get(T &main, int slot) {
    if (__builtin_expect(par.load(std::memory_order_relaxed), 0)) return *static_cast<T *>(window_instance(&main, slot));
    return main;
}
template <class T> inline bool reg(int slot, T *main, const char *name) { return nbxq::reg(registry, slot, main, name); }

int mode = 0, prof = 0, min_draws = 8;
bool window = false, told_bad = false;
nbxq::Worker worker;
nbxq::Ctx *wctx = nullptr;
struct Stats { uint64_t subs, direct_ab, direct_nested, direct_thread, move_ns, run_ns; } st{};
uint64_t report_ns = 0;

void worker_start(void *) {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "dm-nbx0");
#endif
    tls = wctx->p;
}
bool ensure_worker() {
    if (worker.running) return true;
    if (!nbxq::complete(registry, K_COUNT)) { if (!told_bad) { told_bad = true; xbg_logf("[nb] mx: the context registry is incomplete (%d of %d slots) - DM_NB_MX stays off", registry.n, int(K_COUNT)); } return false; }
    if (!wctx) wctx = nbxq::ctx_new(registry);
    worker.on_start = worker_start;
    return worker.start(8u << 20);   // the translation thread's stack size (hle_nvn.c)
}
void init() {
    auto num = [](const char *name, long def, long lo, long hi) { const char *e = getenv(name); long v = e && *e ? atol(e) : def; return v < lo ? lo : v > hi ? hi : v; };
    mode = int(num("DM_NB_MX", 0, 0, 1)); prof = int(num("DM_NB_MX_PROF", 0, 0, 1)); min_draws = int(num("DM_NB_MX_MIN", 8, 1, 4096));
    if (mode) xbg_logf("[nb] DM_NB_MX=%d: submissions are translated by the worker dm-nbx0 with the context moved to it and back (%d variables, %zu bytes; NB-20b ctx)",
        mode, registry.n, nbxq::ctx_bytes(registry));
}
void tick(uint64_t now) {
    if (!report_ns) { report_ns = now; return; }
    if (now - report_ns < 10000000000ull) return;
    const double n = st.subs ? double(st.subs) : 1.;
    xbg_logf("[nb] mx (DM_NB_MX=%d, last %.0f s): submissions on the worker %llu, on the translation thread %llu (a/b off %llu, nested %llu, no worker %llu); "
             "per submission: context moved both ways in %.3f ms (%d variables, %zu bytes), worker ran %.2f ms, the translation thread waited %.2f ms",
        mode, double(now - report_ns) * 1e-9, (unsigned long long)st.subs, (unsigned long long)(st.direct_ab + st.direct_nested + st.direct_thread),
        (unsigned long long)st.direct_ab, (unsigned long long)st.direct_nested, (unsigned long long)st.direct_thread,
        double(st.move_ns) * 1e-6 / n, registry.n, nbxq::ctx_bytes(registry), double(st.run_ns) * 1e-6 / n, double(worker.wait_ns) * 1e-6 / n);
    st = Stats{}; worker.wait_ns = 0; report_ns = now;
}
struct Job { int n; const uint64_t *h; uint64_t *budget; void (*run)(int, const uint64_t *, uint64_t &); uint64_t ns; };
void job_run(void *a) { Job &j = *static_cast<Job *>(a); const uint64_t t0 = nbxq::now_ns(); j.run(j.n, j.h, *j.budget); j.ns = nbxq::now_ns() - t0; }
// xbg_submit_now(): true = the handles were executed (by the worker); false = the caller executes them itself.
bool submit(int n, const uint64_t *h, uint64_t &budget, bool ab_is_off, void (*run)(int, const uint64_t *, uint64_t &)) {
    if (mode != 1) return false;
    if (window) { ++st.direct_nested; return false; }
    if (ab_is_off) { ++st.direct_ab; tick(nbxq::now_ns()); return false; }
    if (!ensure_worker()) { ++st.direct_thread; return false; }
    Job j{n, h, &budget, run, 0};
    const uint64_t t0 = nbxq::now_ns();
    nbxq::ctx_load(registry, *wctx);
    const uint64_t t1 = nbxq::now_ns();
    window = true; par.store(true, std::memory_order_release);
    worker.run(job_run, &j);
    par.store(false, std::memory_order_release); window = false;
    const uint64_t t2 = nbxq::now_ns();
    nbxq::ctx_store(registry, *wctx);
    const uint64_t t3 = nbxq::now_ns();
    ++st.subs; st.move_ns += (t1 - t0) + (t3 - t2); st.run_ns += j.ns;
    tick(t3);
    return true;
}
void stop() { worker.stop(); }

// ---------------- segment profile ----------------
nbxq::Seg seg; nbxq::Prof pf{};
bool seg_open = false, part_open = false;
uint64_t seg_t0 = 0, seg_noise0 = 0, prof_ns = 0, quiet_ns[1024], serial_ns = 0, model_ns[4] = {}, model_parts = 0;
int nquiet = 0, seg_node = -1;
int kind_of(uint16_t op) {
    switch (op) {
    case nbmt::OP_BeginRenderPass: return nbxq::NBX_E_BEGIN;
    case nbmt::OP_EndRenderPass: return nbxq::NBX_E_END;
    case nbmt::OP_Draw: case nbmt::OP_DrawIndexed: case nbmt::OP_DrawIndirect: case nbmt::OP_DrawIndexedIndirect: case nbmt::OP_DrawIndirectCount:
    case nbmt::OP_DrawIndexedIndirectCount: return nbxq::NBX_E_DRAW;
    case nbmt::OP_BindPipeline: case nbmt::OP_BindDescriptorSets: case nbmt::OP_BindVertexBuffers: case nbmt::OP_BindIndexBuffer: case nbmt::OP_PushConstants:
    case nbmt::OP_SetViewport: case nbmt::OP_SetScissor: case nbmt::OP_ClearAttachments: case nbmt::OP_BeginQuery: case nbmt::OP_EndQuery:
    case nbmt::OP_BeginConditionalRendering: case nbmt::OP_EndConditionalRendering: return nbxq::NBX_E_INPASS;
    case nbmt::OP_PipelineBarrier: return nbxq::NBX_E_BARRIER;
    case nbmt::OP_WriteTimestamp: case nbmt::OP_FillBuffer: return nbxq::NBX_E_NEUTRAL;   // pass profile stamps, crumbs
    default: return nbxq::NBX_E_XFER;   // dispatch, copies, blits, resolves, image clears, query pool reset / copy
    }
}
void on_packet(uint16_t op) { if (seg_open) nbxq::seg_event(&seg, kind_of(op)); }
void seg_close(uint64_t now, uint64_t noise) {
    if (!seg_open) return;
    const uint64_t ns = now - seg_t0; const bool noisy = noise != seg_noise0; const int c = nbxq::seg_class(&seg);
    if (seg_node < 0) { pf.ns_prefix += ns; pf.ns_total += ns; serial_ns += ns; }   // records before the first pass of the part
    else {
        nbxq::prof_add(&pf, &seg, ns, noisy);
        if (ns > pf.largest_ns) pf.largest_ns = ns;
        if ((c == nbxq::NBX_C_CLEAN || c == nbxq::NBX_C_PRE_BAR) && !noisy && int(seg.draws) >= min_draws && nquiet < 1024) quiet_ns[nquiet++] = ns; else serial_ns += ns;
    }
    seg_open = false;
}
void prof_report(uint64_t now) {
    if (!prof_ns) { prof_ns = now; return; }
    if (now - prof_ns < 10000000000ull) return;
    const double parts = pf.parts ? double(pf.parts) : 1., ms = 1e-6 / parts;
    char b[1400]; int k = 0; b[0] = 0;
    for (int c = 0; c < nbxq::NBX_C_KINDS; c++) { uint64_t n = 0, d = 0, t = 0; for (int z = 0; z < nbxq::NBX_SZ_KINDS; z++) { n += pf.n[c][z]; d += pf.draws[c][z]; t += pf.ns[c][z]; }
        if (n && k < int(sizeof b) - 80) k += snprintf(b + k, sizeof b - size_t(k), "%s%s %.1f / %.0f draws / %.2f ms", k ? "; " : "", nbxq::class_name(c), double(n) / parts, double(d) / parts, double(t) * ms); }
    static const char *const zn[nbxq::NBX_SZ_KINDS] = {"0-7", "8-31", "32-127", "128+"};
    char q[500]; int m = 0; q[0] = 0;
    for (int z = 0; z < nbxq::NBX_SZ_KINDS; z++) if (m < int(sizeof q) - 80) m += snprintf(q + m, sizeof q - size_t(m), "%s%s draws: %.1f / %.0f draws / %.2f ms", m ? "; " : "", zn[z],
        double(pf.quiet_n[z]) / parts, double(pf.quiet_draws[z]) / parts, double(pf.quiet_ns[z]) * ms);
    const double mp = model_parts ? 1e-6 / double(model_parts) : 0.;
    xbg_logf("[nb] mx prof (last %.0f s, per part): %llu parts, %.1f segments, translated in %.2f ms (before the first pass %.2f ms, longest segment %.2f ms); "
             "segments / draws / ms by class: %s; quiet (clean or pre-barrier, nothing created or uploaded) by size: %s; "
             "model, quiet segments of >= %d draws on T threads and the rest in order: T=1 %.2f ms, T=2 %.2f ms, T=3 %.2f ms, T=4 %.2f ms",
        double(now - prof_ns) * 1e-9, (unsigned long long)pf.parts, double(pf.segs) / parts, double(pf.ns_total) * ms, double(pf.ns_prefix) * ms, double(pf.largest_ns) * 1e-6,
        k ? b : "-", q, min_draws, double(model_ns[0]) * mp, double(model_ns[1]) * mp, double(model_ns[2]) * mp, double(model_ns[3]) * mp);
    pf = nbxq::Prof{}; model_ns[0] = model_ns[1] = model_ns[2] = model_ns[3] = 0; model_parts = 0; prof_ns = now;
}
// From xbg_nb_own.inc: what = 0 a frame job begins (records before its first pass follow), 1 a pass record (node), 2 the job ends.
// noise: a sum of counters that moves when a shared cache gets an entry or a texture is uploaded.
void mark(int what, int node, uint64_t noise) {
    const uint64_t now = nbxq::now_ns();
    seg_close(now, noise);
    if (what == 2) {
        if (part_open) { ++pf.parts; for (int t = 0; t < 4; t++) model_ns[t] += uint64_t(nbxq::prof_model(quiet_ns, nquiet, serial_ns, t + 1)); ++model_parts; part_open = false; }
        prof_report(now); return;
    }
    if (what == 0) { part_open = true; nquiet = 0; serial_ns = 0; }
    if (!part_open) return;
    nbxq::seg_reset(&seg); seg_open = true; seg_t0 = now; seg_noise0 = noise; seg_node = what == 1 ? node : -1;
}
void prof_init() { if (prof) { nbmt::on_packet = on_packet; xbg_logf("[nb] DM_NB_MX_PROF=1: segments of a frame job classified from their vkCmd* packets (needs DM_NB_OWN and DM_NB_MT%s)", nbmt::mode ? "" : " - DM_NB_MT IS 0: NO PACKETS, classes will all read empty"); } }
} // namespace nbx
} // namespace
