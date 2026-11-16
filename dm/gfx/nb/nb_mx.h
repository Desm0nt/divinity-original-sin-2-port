// [DOS2] NB-20b step 2 base (DM_NB_MX): the recording context, the translation worker and the segment profile
// (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-20b-ctx findings"). No Vulkan and no engine here: the engine side is
// dm/gfx/vulkan/engine/xbg_nb_mx.inc, the host test is dm/gfx/vulkan/engine/tests/nb_mx_test.cpp.
//
// 1. Context variables. A global of the draw path's class C (NB-20a) keeps its one "main" instance, used by every thread as before
//    (the translation thread and the game threads that enter the engine under its lock). A worker thread owns a second instance of
//    every registered variable (a Ctx); the engine reads a variable through nbx::get(main, slot), which is the main instance unless
//    a window is open (`par`) AND the calling thread is a worker. ctx_load / ctx_store move the whole context main -> worker and back
//    (a worker that continues the translation thread's work, DM_NB_MX=1).
// 2. Worker: one thread that runs one posted job at a time; the poster waits on a condition variable (no spinning).
// 3. Segment profile: the vkCmd* packets of a segment (OP_X_NB_PASS .. the next one) by kind and position -> its class: can this
//    segment be translated apart from its neighbours (one pass, nothing between its begin and its end but in-pass commands)?
#ifndef DM_NB_MX_H
#define DM_NB_MX_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#if defined(_WIN32)
#include <thread>
#else
#include <pthread.h>
#endif

namespace nbxq {

// ---------------- 1. context variables ----------------
enum { MAX_VARS = 64 };
struct Var {
    const char *name;
    void *main;
    void *(*make)();                              // a value-initialised instance for a worker
    void (*kill)(void *);
    void (*assign)(void *dst, const void *src);   // dst = src (copy; element-wise for arrays)
    size_t bytes;
};
struct Registry { Var v[MAX_VARS]; int n; uint64_t mask; };   // slot = index; constant-initialised (static storage)
struct Ctx { void *p[MAX_VARS]; };

template <class T> struct Box { T v{}; };
template <class T> inline void assign_(T &d, const T &s) { d = s; }
template <class T, size_t N> inline void assign_(T (&d)[N], const T (&s)[N]) { for (size_t i = 0; i < N; i++) assign_(d[i], s[i]); }
// Registers the main instance of slot `slot` (a namespace-scope initialiser next to the variable). false: slot taken / out of range.
template <class T> inline bool reg(Registry &r, int slot, T *main, const char *name) {
    if (slot < 0 || slot >= MAX_VARS || (r.mask >> slot & 1)) { r.n = -1; return false; }
    Var &v = r.v[slot];
    v.name = name; v.main = main; v.bytes = sizeof(T);
    v.make = []() -> void * { return &(new Box<T>())->v; };
    v.kill = [](void *p) { delete reinterpret_cast<Box<T> *>(p); };   // v is the first (only) member of Box
    v.assign = [](void *d, const void *s) { assign_(*static_cast<T *>(d), *static_cast<const T *>(s)); };
    r.mask |= 1ull << slot; if (r.n >= 0 && slot >= r.n) r.n = slot + 1;
    return true;
}
inline bool complete(const Registry &r, int slots) { return r.n == slots && (slots >= 64 ? ~0ull : (1ull << slots) - 1) == r.mask; }
inline Ctx *ctx_new(const Registry &r) { Ctx *c = new Ctx(); for (int i = 0; i < r.n; i++) c->p[i] = (r.mask >> i & 1) ? r.v[i].make() : nullptr; return c; }
inline void ctx_free(const Registry &r, Ctx *c) { if (!c) return; for (int i = 0; i < r.n; i++) if (c->p[i]) r.v[i].kill(c->p[i]); delete c; }
inline void ctx_load(const Registry &r, Ctx &c) { for (int i = 0; i < r.n; i++) if (c.p[i]) r.v[i].assign(c.p[i], r.v[i].main); }     // main -> worker
inline void ctx_store(const Registry &r, const Ctx &c) { for (int i = 0; i < r.n; i++) if (c.p[i]) r.v[i].assign(r.v[i].main, c.p[i]); }   // worker -> main
inline size_t ctx_bytes(const Registry &r) { size_t n = 0; for (int i = 0; i < r.n; i++) if (r.mask >> i & 1) n += r.v[i].bytes; return n; }

// ---------------- 2. worker ----------------
inline uint64_t now_ns() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
struct Worker {
    std::mutex mu;
    std::condition_variable cv_job, cv_done;
    void (*fn)(void *) = nullptr; void *arg = nullptr;
    void (*on_start)(void *) = nullptr; void *start_arg = nullptr;   // on the worker thread, before its first job
    bool has = false, done = true, stop_flag = false, running = false;
    uint64_t jobs = 0, wait_ns = 0;   // poster side
#if defined(_WIN32)
    std::thread *th = nullptr;
#else
    pthread_t th{};
#endif
    void loop() {
        if (on_start) on_start(start_arg);
        std::unique_lock<std::mutex> l(mu);
        for (;;) {
            if (has) { void (*f)(void *) = fn; void *a = arg; has = false; l.unlock(); f(a); l.lock(); done = true; cv_done.notify_all(); continue; }
            if (stop_flag) break;
            cv_job.wait(l);
        }
    }
    static void *entry(void *w) { static_cast<Worker *>(w)->loop(); return nullptr; }
    bool start(size_t stack_bytes) {   // poster; idempotent
        { std::lock_guard<std::mutex> l(mu); if (running) return true; stop_flag = false; has = false; done = true; }
#if defined(_WIN32)
        (void)stack_bytes; th = new std::thread([this] { loop(); });
#else
        pthread_attr_t at; pthread_attr_init(&at); if (stack_bytes) pthread_attr_setstacksize(&at, stack_bytes);
        const int r = pthread_create(&th, &at, entry, this); pthread_attr_destroy(&at);
        if (r) return false;
#endif
        std::lock_guard<std::mutex> l(mu); running = true; return true;
    }
    // One job at a time: post, then wait. (Step 3 posts to several workers before it waits for any.)
    void post(void (*f)(void *), void *a) { { std::lock_guard<std::mutex> l(mu); fn = f; arg = a; has = true; done = false; } ++jobs; cv_job.notify_one(); }
    void wait() { const uint64_t t0 = now_ns(); { std::unique_lock<std::mutex> l(mu); cv_done.wait(l, [this] { return done; }); } wait_ns += now_ns() - t0; }
    void run(void (*f)(void *), void *a) { post(f, a); wait(); }
    void stop() {
        { std::unique_lock<std::mutex> l(mu); if (!running) return; cv_done.wait(l, [this] { return done; }); stop_flag = true; running = false; }
        cv_job.notify_all();
#if defined(_WIN32)
        if (th) { th->join(); delete th; th = nullptr; }
#else
        pthread_join(th, nullptr);
#endif
    }
};

// ---------------- 3. segment profile ----------------
enum { NBX_E_BEGIN = 0, NBX_E_END, NBX_E_DRAW, NBX_E_INPASS, NBX_E_BARRIER, NBX_E_XFER, NBX_E_NEUTRAL, NBX_E_KINDS };   // packet kinds
enum { NBX_C_EMPTY = 0,   // no command at all (state only)
       NBX_C_OPS,         // commands, but no pass begun and no draw (operations between passes)
       NBX_C_CONT,        // draws into the pass the previous segment left open (no begin of its own)
       NBX_C_CLEAN,       // one pass begun; before it at most the end of the previous pass
       NBX_C_PRE_BAR,     // the same with barriers before the begin
       NBX_C_PRE_XFER,    // the same with copies / clears / uploads before the begin
       NBX_C_TAIL,        // one pass, and commands after its end inside the segment
       NBX_C_SPLIT,       // a barrier / copy between its begin and end, or more than one pass
       NBX_C_KINDS };
static inline const char *class_name(int c) {
    static const char *const n[NBX_C_KINDS] = {"empty", "ops", "cont", "clean", "pre-barrier", "pre-xfer", "tail", "split"};
    return c >= 0 && c < NBX_C_KINDS ? n[c] : "?";
}
struct Seg {
    uint32_t begins, ends, draws, inpass, total;
    uint32_t bar_pre, bar_in, bar_post, xfer_pre, xfer_in, xfer_post, end_pre, draws_out;
    int phase;   // 0 before the first begin, 1 inside a pass, 2 after an end
};
static inline void seg_reset(Seg *s) { memset(s, 0, sizeof *s); }
static inline void seg_event(Seg *s, int kind) {
    ++s->total;
    switch (kind) {
    case NBX_E_BEGIN: ++s->begins; s->phase = 1; break;
    case NBX_E_END: if (s->phase == 0 && !s->begins) ++s->end_pre; else ++s->ends; if (s->phase == 1) s->phase = 2; break;
    case NBX_E_DRAW: ++s->draws; if (s->phase != 1) ++s->draws_out; break;
    case NBX_E_INPASS: ++s->inpass; break;
    case NBX_E_BARRIER: if (s->phase == 0) ++s->bar_pre; else if (s->phase == 1) ++s->bar_in; else ++s->bar_post; break;
    case NBX_E_XFER: if (s->phase == 0) ++s->xfer_pre; else if (s->phase == 1) ++s->xfer_in; else ++s->xfer_post; break;
    default: break;
    }
}
static inline int seg_class(const Seg *s) {
    if (!s->total) return NBX_C_EMPTY;
    if (!s->begins) return s->draws ? (s->bar_pre || s->xfer_pre ? NBX_C_SPLIT : NBX_C_CONT) : NBX_C_OPS;
    if (s->begins > 1 || s->bar_in || s->xfer_in || s->draws_out) return NBX_C_SPLIT;
    if (s->bar_post || s->xfer_post) return NBX_C_TAIL;
    if (s->xfer_pre) return NBX_C_PRE_XFER;
    if (s->bar_pre) return NBX_C_PRE_BAR;
    return NBX_C_CLEAN;
}
enum { NBX_SZ_1 = 0, NBX_SZ_8, NBX_SZ_32, NBX_SZ_MORE, NBX_SZ_KINDS };   // draws of a segment: 1..7, 8..31, 32..127, 128+ (0 draws counted with 1..7)
static inline int seg_size(uint32_t draws) { return draws < 8 ? NBX_SZ_1 : draws < 32 ? NBX_SZ_8 : draws < 128 ? NBX_SZ_32 : NBX_SZ_MORE; }
struct Prof {
    uint64_t parts, segs, n[NBX_C_KINDS][NBX_SZ_KINDS], draws[NBX_C_KINDS][NBX_SZ_KINDS], ns[NBX_C_KINDS][NBX_SZ_KINDS];
    uint64_t quiet_n[NBX_SZ_KINDS], quiet_draws[NBX_SZ_KINDS], quiet_ns[NBX_SZ_KINDS];   // clean / pre-barrier segments without a shared-cache miss
    uint64_t ns_total, ns_prefix, largest_ns;
};
// noisy: the segment created a pipeline / an image / uploaded a texture (a worker would have to give it back).
static inline void prof_add(Prof *p, const Seg *s, uint64_t ns, int noisy) {
    const int c = seg_class(s), z = seg_size(s->draws);
    ++p->segs; ++p->n[c][z]; p->draws[c][z] += s->draws; p->ns[c][z] += ns; p->ns_total += ns;
    if ((c == NBX_C_CLEAN || c == NBX_C_PRE_BAR) && !noisy) { ++p->quiet_n[z]; p->quiet_draws[z] += s->draws; p->quiet_ns[z] += ns; }
}
// The wall time of a part translated in two phases: the quiet segments (seg_ns) on `threads` threads at once (the translation thread
// is one of them; longest first onto the least loaded thread), then the rest (serial_ns) in order. A bound: no stitch cost.
static inline double prof_model(const uint64_t *seg_ns, int nseg, uint64_t serial_ns, int threads) {
    uint64_t load[16]; if (threads < 1) threads = 1; if (threads > 16) threads = 16;
    for (int i = 0; i < threads; i++) load[i] = 0;
    uint8_t done[1024]; const int n = nseg > 1024 ? 1024 : nseg; for (int i = 0; i < n; i++) done[i] = 0;
    for (int k = 0; k < n; k++) {
        int best = -1; for (int i = 0; i < n; i++) if (!done[i] && (best < 0 || seg_ns[i] > seg_ns[best])) best = i;
        done[best] = 1; int at = 0; for (int i = 1; i < threads; i++) if (load[i] < load[at]) at = i;
        load[at] += seg_ns[best];
    }
    uint64_t m = 0; for (int i = 0; i < threads; i++) if (load[i] > m) m = load[i];
    return double(m + serial_ns);
}

} // namespace nbxq
#endif
