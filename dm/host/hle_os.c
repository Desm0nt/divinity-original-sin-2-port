// 宿主公共杂项：客体对象旁表、单调时钟、modules 都没定义的 C 库导入。
// （原 nn::os/mem/lmem/C++ ABI 的 136 条 HLE 已删：SDK 自带 nn::os，装载时这些条目一条都没被解析用上。）
// Host common miscellany: guest object side table, monotonic clock, C library imports that none of the modules define.
// (The former 136 HLE entries for nn::os/mem/lmem/C++ ABI have been deleted: the SDK brings its own nn::os, and at load time not one of these entries was resolved and used.)
#include <pthread.h>
#include "hle.h"
#include <mach/mach_time.h>
#include <stdatomic.h>

// ---------- 旁表 ----------
// ---------- Side table ----------
#define OM_SIZE (1u << 18)
static struct { const void *k; void *v; } g_om[OM_SIZE];
static pthread_mutex_t g_om_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned om_h(const void *k) { uint64_t x = (uint64_t)k; x ^= x >> 29; x *= 0xbf58476d1ce4e5b9ull; return (unsigned)(x >> 40) & (OM_SIZE - 1); }
void *objmap_get(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    void *r = NULL;
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1)) {
        if (g_om[i].k == k) { r = g_om[i].v; break; }
        if (!g_om[i].k) break;
    }
    pthread_mutex_unlock(&g_om_lock);
    return r;
}
void objmap_put(const void *k, void *v) {
    pthread_mutex_lock(&g_om_lock);
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1))
        if (g_om[i].k == k || !g_om[i].k || g_om[i].k == (void *)1) { g_om[i].k = k; g_om[i].v = v; break; }
    pthread_mutex_unlock(&g_om_lock);
}
void *objmap_del(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    void *r = NULL;
    for (unsigned i = om_h(k);; i = (i + 1) & (OM_SIZE - 1)) {
        if (g_om[i].k == k) { r = g_om[i].v; g_om[i].k = (void *)1; g_om[i].v = NULL; break; }
        if (!g_om[i].k) break;
    }
    pthread_mutex_unlock(&g_om_lock);
    return r;
}
uint64_t now_ns(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}
// 客体时钟：客体能读到的系统计数器（mrs cntpct 门、svc GetSystemTick）都经这里。缺省 = now_ns()。
// 冻结（只供测试驱动 DM_TICK_HOLD 用）：冻结期间客体时钟不走 ⇒ 游戏不会到期下一拍；解冻后扣掉冻结时长，客体时钟连续不跳
// DM_DETCLOCK=<步长 ns>（回放 / 测试局用；产品局不设 = 墙钟）：首次出图起客体时钟改为「每次出图推进固定步长」，不跟墙钟——
// 客户端每帧 dt、服务端拍的补拍快照都由出图次数决定，同一回放两遍局内第 N 帧逐字节可比（画面逐字节证明的地基）。
// 首帧之前仍走墙钟（启动期有按时刻轮询的等待，冻住会卡）；切换时从当时的客体时钟值接续，不跳。冻结期间不推进。
// Guest clock: the system counters the guest can read (mrs cntpct gate, svc GetSystemTick) all go through here. Default = now_ns().
// Freeze (only for the test driver DM_TICK_HOLD): while frozen the guest clock does not advance ⇒ the game's next tick never comes due; after unfreezing the frozen duration is subtracted, the guest clock is continuous with no jump
// DM_DETCLOCK=<step ns> (for replay / test runs; product runs do not set it = wall clock): from the first present on the guest clock becomes "advance a fixed step per present", not following the wall clock --
// the client's per-frame dt and the server tick catch-up snapshots are both determined by the present count, so frame N in a run is byte-for-byte comparable between two passes of the same replay (the foundation of byte-for-byte picture proof).
// Before the first frame the wall clock is still used (the boot period has waits that poll by time, freezing would hang them); at the switch it continues from the guest clock value at that time, no jump. It does not advance while frozen.
static _Atomic uint64_t g_gfrozen, g_glag, g_gdet, g_guest_frame; static _Atomic int g_gfz, g_gdet_on; static uint64_t g_gdet_step = 1;
uint64_t dm_guest_frame(void) { return atomic_load(&g_guest_frame); }
static int detclock_cfg(void) {
    static int c = -1; if (c < 0) { const char *e = getenv("DM_DETCLOCK"); g_gdet_step = e ? strtoull(e, 0, 0) : 0; c = g_gdet_step > 0; }
    return c;
}
static uint64_t wall_guest_ns(void) { return atomic_load(&g_gfz) ? atomic_load(&g_gfrozen) : now_ns() - atomic_load(&g_glag); }
uint64_t guest_ns(void) { return atomic_load(&g_gdet_on) ? atomic_load(&g_gdet) : wall_guest_ns(); }
int dm_detclock_enabled(void) { return detclock_cfg() && atomic_load(&g_gdet_on); }
// 进局对齐：前端 / 选角阶段出图帧数随真实时间变（装载在后台线程按墙钟推进），首帧起累计的客体时钟值因此每局不同——
// 依赖绝对时刻的画面（如生命 / 法力球液面）在同拍仍不同（实测拍 E + 2 帧两遍差约 1.8 万像素，集中在两侧球位）。
// 见到 UI 消息 FrontEndNavigation:ToGame（建局、服务端拍时限初始化之前）即把客体时钟设为固定值 2^52 ns（只许前跳），
// 此后纯按出图推进：拍 E 落在第几帧、那一刻的绝对时刻都只由步长决定。
// Run entry alignment: the number of presented frames in the front end / character select phase varies with real time (loading advances on a background thread by wall clock), so the guest clock value accumulated since the first frame differs every run --
// pictures that depend on absolute time (such as the liquid level of the life / mana orbs) still differ at the same tick (measured: at tick E + 2 frames two passes differ by about 18,000 pixels, concentrated at the two orb positions).
// On seeing the UI message FrontEndNavigation:ToGame (before the run is created and the server tick deadline is initialized) the guest clock is set to the fixed value 2^52 ns (forward jumps only),
// from then on it advances purely by presents: which frame tick E lands on and the absolute time at that moment are determined by the step alone.
#define DET_TOGAME_NS (1ull << 52)
static void det_on_uimsg(const char *cat, const char *act, const char *arg) {
    (void)arg; if (strcmp(cat, "FrontEndNavigation") || strcmp(act, "ToGame")) return;
    uint64_t cur = atomic_load(&g_gdet);
    if (cur >= DET_TOGAME_NS) { LOG("【红】DM_DETCLOCK：进局时客体时钟 %llu ≥ 对齐值 2^52，不回拨（对齐未生效）", (unsigned long long)cur); return; }
    atomic_store(&g_gdet, DET_TOGAME_NS);
    LOG("DM_DETCLOCK：进局（ToGame）客体时钟对齐到 2^52 ns（前跳 %.3f s）", (DET_TOGAME_NS - cur) / 1e9);
}
void dm_guest_frame_tick(void) {
    crash_watch_frame();
    atomic_fetch_add(&g_guest_frame, 1);
#ifdef DM_GAME_DOOM
    // DM_DOOM_TICKLOG=1: once per wall second log present rate vs estimated game time (hle_doom_tick.c); a getenv-cached no-op otherwise
    void dm_doom_tick_diag(void); dm_doom_tick_diag();
#endif
    if (!detclock_cfg()) return;
    if (!atomic_load(&g_gdet_on)) {
        atomic_store(&g_gdet, wall_guest_ns()); atomic_store(&g_gdet_on, 1);
        void uimsg_subscribe(void (*)(const char *, const char *, const char *)); uimsg_subscribe(det_on_uimsg);
        LOG("DM_DETCLOCK：首次出图起客体时钟按出图推进，步长 %llu ns；进局时对齐到 2^52 ns", (unsigned long long)g_gdet_step); return;
    }
    if (!atomic_load(&g_gfz)) { atomic_fetch_add(&g_gdet, g_gdet_step); dm_guest_clock_advanced(); }
}
static void tick_freeze(int on);   // [NR-24] DM_NR_GATE_TICK: the guest tick domain freezes and thaws with the guest clock (below)
static void guest_clock_freeze_apply(int on) {
    if (on && !atomic_load(&g_gfz)) { atomic_store(&g_gfrozen, now_ns() - atomic_load(&g_glag)); atomic_store(&g_gfz, 1); tick_freeze(1); }
    else if (!on && atomic_load(&g_gfz)) { atomic_store(&g_glag, now_ns() - atomic_load(&g_gfrozen)); atomic_store(&g_gfz, 0); tick_freeze(0); }
}
// 冻结有几个互不相干的原因（0 测试用的冻结拍 / 读取画面，1 App 在后台，2 系统键盘弹着）：任一成立就冻结，全部解除才放行——
// 否则「键盘弹着时切后台再回来」会被回前台那次解冻提前放开
// Freezing has several unrelated reasons (0 freeze tick for testing / loading screen, 1 App in background, 2 system keyboard up): freeze if any holds, release only when all are lifted --
// otherwise "switch to background and back while the keyboard is up" would be released early by the unfreeze on returning to foreground
static pthread_mutex_t g_gfz_mu = PTHREAD_MUTEX_INITIALIZER; static unsigned g_gfz_reasons;
void guest_clock_freeze_for(int reason, int on) {
    pthread_mutex_lock(&g_gfz_mu);
    if (on) g_gfz_reasons |= 1u << reason; else g_gfz_reasons &= ~(1u << reason);
    guest_clock_freeze_apply(g_gfz_reasons != 0);
    void gfz_hold_sync_(void); gfz_hold_sync_();   // [front] pause-menu read wait
    pthread_mutex_unlock(&g_gfz_mu);
}
// [front] While the pause menu holds the game (reason 3, hle_hold.c) a guest read of the system counter (the cntpct gate, svc GetSystemTick) waits:
// Horizon suspends a game behind its HOME menu, and a guest thread reading the frozen clock twice would otherwise see no time pass and spin or
// divide by it (nx2android hle_os.c guest_clock_read_wait, Super Mario Galaxy 2). The hold suspends graphics before freezing, so no thread is inside
// an NVN call; host code reads guest_ns() directly and never waits here.
static pthread_cond_t g_gfz_cv = PTHREAD_COND_INITIALIZER; static _Atomic int g_gfz_hold;
void guest_clock_read_wait(void) {
    if (!atomic_load(&g_gfz_hold)) return;
    pthread_mutex_lock(&g_gfz_mu);
    while (g_gfz_reasons & (1u << 3)) pthread_cond_wait(&g_gfz_cv, &g_gfz_mu);
    pthread_mutex_unlock(&g_gfz_mu);
}
static void gfz_hold_sync(void) {   // under g_gfz_mu, after the clock state changed
    atomic_store(&g_gfz_hold, (g_gfz_reasons & (1u << 3)) != 0);
    if (!(g_gfz_reasons & (1u << 3))) pthread_cond_broadcast(&g_gfz_cv);
}
void gfz_hold_sync_(void) { gfz_hold_sync(); }
void guest_clock_freeze(int on) { guest_clock_freeze_for(0, on); }
// [hitch] Wall time the guest clock has stood frozen since start (pause menu, background, system keyboard, loading-screen freeze),
// including a freeze still running: the long-frame log (hle_nvn.c) takes it off the frame time, so the frame that spans the pause
// menu is not reported as a 6-26 s hitch (docs/dos2/HITCH_AUDIT.md, touch1.log / l1.log).
uint64_t dm_guest_frozen_ns(void) { return now_ns() - wall_guest_ns(); }

// ---------- [NR-24] DM_NR_GATE_TICK: the guest's system tick without the full gate (docs/dos2/nr/NR-24.md) ----------
// Off (0, default): every guest read of the system counter is guest_ns() * 12 / 625 through the full gate (entry.S _dm_gate: 800-byte
// frame, hash lookup, gate_dispatch), as before. 1: the guest tick domain is the host's virtual counter itself, cntvct_el0, mapped to
// 19.2 MHz (the Switch's CNTFRQ) as ticks = base + (cntvct - c0) [* 19.2 MHz / CNTFRQ when the phone's counter runs at another rate:
// MediaTek 13 MHz, Exynos 26 MHz, Tensor 24.576 MHz; Snapdragon's is 19.2 MHz and takes no multiply], read three ways that give the
// same numbers: (1) main's imports of nn::os::GetSystemTick / nnosGetSystemTick (105 call sites in DOS2's main; the SDK function is
// "mrs x0, cntpct_el0; ret") bind to dm_tick_guest_fn below, a plain C call; (2) the SDK's own 32 "mrs xN, cntpct_el0" sites go
// through entry.S _dm_gate_fast, which looks the gate up and computes the value with four scratch registers and no frame, and falls
// back to _dm_gate for every other gate; (3) the full gate and svc GetSystemTick call dm_tick_read(). A freeze of the guest clock
// (pause menu, background, system keyboard, loading-screen freeze) publishes a frozen block: readers take its value, the pause-menu
// hold waits as before (guest_clock_read_wait), and the thaw continues from the frozen value, so the guest sees one monotonic clock
// that stands still while frozen, as guest_ns() does. The host's own guest_ns() / now_ns() stay on CLOCK_MONOTONIC: the two differ
// only by the kernel's NTP slew (< 0.05 %), guest timeouts are relative. 2: the guest keeps the old values; the tick domain runs beside
// them and the full gate compares the two (red line past 1 ms, a summary every 10 s). Not with DM_DETCLOCK (the guest clock is then
// the present count, refused with a log line). Horizon: the Switch's counter is a free-running 19.2 MHz cntpct read by one
// instruction; the only Horizon-visible properties are monotonic ticks at 19.2 MHz, which both modes keep.
// Block layout (entry.S reads valid / c0 / base / mul at 0 / 8 / 16 / 24): the current block is published with a release store of
// dm_tick_cur and never written again while current; a frozen block (valid 0) gets its base after it is published, behind ready.
typedef struct dm_tick_blk { uint64_t valid, c0, base, mul; _Atomic uint64_t ready; uint64_t pad[3]; } dm_tick_blk;
_Static_assert(__builtin_offsetof(dm_tick_blk, valid) == 0 && __builtin_offsetof(dm_tick_blk, c0) == 8 && __builtin_offsetof(dm_tick_blk, base) == 16 &&
               __builtin_offsetof(dm_tick_blk, mul) == 24, "entry.S _dm_gate_fast reads these offsets");
__attribute__((visibility("hidden"))) dm_tick_blk *_Atomic dm_tick_cur;   // entry.S _dm_gate_fast
int g_tick_mode;                                                            // 0 / 1 / 2 once dm_tick_init has run
static dm_tick_blk g_tb[256]; static unsigned g_tb_n;                        // ring of blocks (a freeze or thaw takes the next one)
#if defined(DM_TICK_TEST)                                                   // tests/t_nr24_tick.c: a counter it drives
uint64_t dm_test_cnt(void); uint64_t dm_test_frq(void);
static inline uint64_t tk_cnt(void) { return dm_test_cnt(); }
static inline uint64_t tk_frq(void) { return dm_test_frq(); }
static inline uint64_t tk_cnt_then(dm_tick_blk *_Atomic *pp, dm_tick_blk **cur) { uint64_t c = dm_test_cnt(); *cur = atomic_load_explicit(pp, memory_order_acquire); return c; }
#define TK_HAVE 1
#elif defined(__aarch64__) && defined(__linux__)
static inline uint64_t tk_cnt(void) { uint64_t v; __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v) : : "memory"); return v; }
static inline uint64_t tk_frq(void) { uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); return f; }
// The counter, then *pp loaded after it: an address dependency on the counter value orders the load after the read (Linux's
// arch_counter_enforce_ordering), so a reader whose re-check still sees its block read the counter before any later publish
static inline uint64_t tk_cnt_then(dm_tick_blk *_Atomic *pp, dm_tick_blk **cur) {
    uint64_t v, z; dm_tick_blk *p;
    __asm__ volatile("mrs %0, cntvct_el0\n\teor %1, %0, %0\n\tldr %2, [%3, %1]" : "=&r"(v), "=&r"(z), "=&r"(p) : "r"(pp) : "memory");
    *cur = p; return v;
}
#define TK_HAVE 1
#else
#define TK_HAVE 0
#endif
#if TK_HAVE
static inline uint64_t tick_calc(const dm_tick_blk *b, uint64_t c) {
    uint64_t d = c - b->c0; if ((int64_t)d < 0) d = 0;   // a counter read from before the block's anchor (speculated early): its base
    if (b->mul) d = (uint64_t)(((unsigned __int128)d * b->mul) >> 32);
    return b->base + d;
}
static dm_tick_blk *tb_next(void) { dm_tick_blk *b = &g_tb[g_tb_n++ & 255]; atomic_store_explicit(&b->ready, 0, memory_order_relaxed); b->valid = b->c0 = b->base = 0; return b; }
// Under g_gfz_mu, on a real transition (guest_clock_freeze_apply)
static void tick_freeze(int on) {
    if (!g_tick_mode) return;
    dm_tick_blk *old = atomic_load_explicit(&dm_tick_cur, memory_order_relaxed), *nb;
    if (on) {
        if (!old->valid) return;
        nb = tb_next(); nb->mul = old->mul;
        atomic_store_explicit(&dm_tick_cur, nb, memory_order_release);
        atomic_thread_fence(memory_order_seq_cst);          // the publish before the counter read below (dmb ish; tk_cnt has an isb)
        nb->base = tick_calc(old, tk_cnt());               // >= every value a reader of old returns (its counter read came first)
        atomic_store_explicit(&nb->ready, 1, memory_order_release);
    } else {
        if (old->valid) return;
        nb = tb_next(); nb->mul = old->mul; nb->base = old->base; nb->c0 = tk_cnt(); nb->valid = 1;
        atomic_store_explicit(&nb->ready, 1, memory_order_relaxed);
        atomic_store_explicit(&dm_tick_cur, nb, memory_order_release);   // a reader that sampled before c0 clamps to the frozen value
    }
}
uint64_t dm_tick_read(void) {
    for (;;) {
        dm_tick_blk *b = atomic_load_explicit(&dm_tick_cur, memory_order_acquire), *b2;
        if (b->valid) { uint64_t c = tk_cnt_then(&dm_tick_cur, &b2); if (b2 == b) return tick_calc(b, c); continue; }
        guest_clock_read_wait();   // [front] the pause-menu hold, as the old gate
        if (atomic_load_explicit(&dm_tick_cur, memory_order_acquire) != b) continue;
        while (!atomic_load_explicit(&b->ready, memory_order_acquire)) { }
        return b->base;
    }
}
// The same value without the pause-menu wait (DM_NR_GATE_TICK=2's comparison, host code)
static uint64_t tick_peek(void) {
    for (;;) {
        dm_tick_blk *b = atomic_load_explicit(&dm_tick_cur, memory_order_acquire), *b2;
        if (b->valid) { uint64_t c = tk_cnt_then(&dm_tick_cur, &b2); if (b2 == b) return tick_calc(b, c); continue; }
        if (atomic_load_explicit(&b->ready, memory_order_acquire)) return b->base;
    }
}
#else
static void tick_freeze(int on) { (void)on; }
uint64_t dm_tick_read(void) { guest_clock_read_wait(); return guest_ns() * 12 / 625; }
static uint64_t tick_peek(void) { return guest_ns() * 12 / 625; }
#endif
static pthread_once_t g_tick_once = PTHREAD_ONCE_INIT;
static void tick_init_once(void) {
    const char *e = getenv("DM_NR_GATE_TICK"); int m = e && *e ? atoi(e) : 0;
    if (m <= 0) return;
#if TK_HAVE
    if (detclock_cfg()) { LOG("[nr24] DM_NR_GATE_TICK=%d refused: DM_DETCLOCK drives the guest clock by presents (old gate kept)", m); return; }
    // the counter's rate as the kernel states it, checked against CLOCK_MONOTONIC_RAW over 5 ms (a firmware that programs CNTFRQ wrong)
    uint64_t f = tk_frq(); struct timespec a, b, sl = {0, 5000000};
    clock_gettime(CLOCK_MONOTONIC_RAW, &a); uint64_t c1 = tk_cnt(); nanosleep(&sl, NULL); uint64_t c2 = tk_cnt(); clock_gettime(CLOCK_MONOTONIC_RAW, &b);
    double dt = (double)(b.tv_sec - a.tv_sec) * 1e9 + (double)(b.tv_nsec - a.tv_nsec), rate = dt > 0 ? (double)(c2 - c1) * 1e9 / dt : 0;
    if (!f || rate <= 0 || rate < (double)f * 0.99 || rate > (double)f * 1.01) {
        LOG("【红】[nr24] DM_NR_GATE_TICK=%d refused: CNTFRQ %llu Hz but the counter ran at %.0f Hz against CLOCK_MONOTONIC_RAW (old gate kept)", m, (unsigned long long)f, rate); return; }
    const uint64_t mul = f == 19200000ull ? 0 : (uint64_t)((((unsigned __int128)19200000ull << 32) + f / 2) / f);
    pthread_mutex_lock(&g_gfz_mu);
    dm_tick_blk *nb = tb_next(); nb->mul = mul;
    if (atomic_load(&g_gfz)) nb->base = atomic_load(&g_gfrozen) * 12 / 625;
    else { nb->c0 = tk_cnt(); nb->base = guest_ns() * 12 / 625; nb->valid = 1; }
    atomic_store_explicit(&nb->ready, 1, memory_order_relaxed);
    atomic_store_explicit(&dm_tick_cur, nb, memory_order_release);
    g_tick_mode = m >= 2 ? 2 : 1;
    pthread_mutex_unlock(&g_gfz_mu);
    LOG("[nr24] DM_NR_GATE_TICK=%d: guest ticks = cntvct (CNTFRQ %llu Hz, measured %.0f Hz) %s; %s", g_tick_mode, (unsigned long long)f, rate,
        mul ? "scaled to 19.2 MHz" : "1:1 (19.2 MHz)", g_tick_mode == 1 ? "main's GetSystemTick bound to the host, the SDK's clock gates take the short entry"
                                                                         : "the guest keeps the old values, the full gate compares (red past 1 ms)");
#else
    LOG("[nr24] DM_NR_GATE_TICK=%d: not on this platform (needs arm64 Linux), old gate kept", m);
#endif
}
int dm_tick_init(void) { pthread_once(&g_tick_once, tick_init_once); return g_tick_mode; }
// main's imports of the SDK's GetSystemTick (loader.c relocate, DM_NR_GATE_TICK=1, main only; the SDK keeps its own)
static _Atomic uint64_t g_tick_fn_n;   // calls, only while dm_gate_prof is set (DM_NR_CPU=1)
extern void (*dm_gate_prof)(int kind, int svc, uint64_t ticks);   // hle.c
static uint64_t dm_tick_guest_fn(void) {
    if (__builtin_expect(dm_gate_prof != 0, 0)) atomic_fetch_add_explicit(&g_tick_fn_n, 1, memory_order_relaxed);
    return dm_tick_read();
}
uint64_t dm_tick_fn_calls(void) { return atomic_load_explicit(&g_tick_fn_n, memory_order_relaxed); }
void *dm_tick_bind(const char *mod, const char *name) {
    if (dm_tick_init() != 1 || strcmp(mod, "main")) return NULL;
    if (strcmp(name, "_ZN2nn2os13GetSystemTickEv") && strcmp(name, "nnosGetSystemTick")) return NULL;
    static int logged[2]; int k = name[0] == 'n'; if (!logged[k]++) LOG("[nr24] DM_NR_GATE_TICK=1: main's %s -> host dm_tick_guest_fn", name);
    return (void *)dm_tick_guest_fn;
}
// DM_NR_GATE_TICK=2: the full gate's old value against the tick domain (hle.c gate_dispatch)
static _Atomic uint64_t g_tc_n, g_tc_max, g_tc_red, g_tc_t0;
void dm_tick_compare(uint64_t old) {
    const uint64_t nw = tick_peek(), d = nw > old ? nw - old : old - nw, us = d * 625 / 12 / 1000;
    atomic_fetch_add(&g_tc_n, 1);
    uint64_t mx = atomic_load(&g_tc_max); while (us > mx && !atomic_compare_exchange_weak(&g_tc_max, &mx, us)) { }
    if (us > 1000 && atomic_fetch_add(&g_tc_red, 1) < 16) LOG("【红】[nr24] DM_NR_GATE_TICK=2: guest tick %llu (old) vs %llu (cntvct domain): %llu us apart", (unsigned long long)old, (unsigned long long)nw, (unsigned long long)us);
    const uint64_t now = now_ns(), t0 = atomic_load(&g_tc_t0);
    if (!t0) { atomic_store(&g_tc_t0, now); return; }
    if (now - t0 > 10000000000ull && atomic_compare_exchange_strong(&g_tc_t0, &(uint64_t){t0}, now))
        LOG("[nr24] DM_NR_GATE_TICK=2: %llu reads compared, largest gap %llu us, %llu past 1 ms", (unsigned long long)atomic_exchange(&g_tc_n, 0),
            (unsigned long long)atomic_exchange(&g_tc_max, 0), (unsigned long long)atomic_load(&g_tc_red));
}
// 熵源：缺省真随机（arc4random）；DM_DETCLOCK 开时改为固定种子的 splitmix64 流（回放画面逐字节可比用，营地蓝火粒子两遍不同）。
// 两处出口（libc getentropy、svc GetInfo RandomEntropy）共用一条流；调用次数在日志里报（首次与每 1000 次）
// Entropy source: true random by default (arc4random); with DM_DETCLOCK on it becomes a fixed-seed splitmix64 stream (so that replay pictures are byte-for-byte comparable, the camp blue fire particles differed between two passes).
// The two outlets (libc getentropy, svc GetInfo RandomEntropy) share one stream; the call count is reported in the log (first call and every 1000 calls)
static _Atomic uint64_t g_detrng = 0x5357444554524e47ull; static _Atomic uint64_t g_detrng_n;
uint64_t dm_entropy64(void) {
    if (!detclock_cfg()) return ((uint64_t)arc4random() << 32) | arc4random();
    uint64_t z = atomic_fetch_add(&g_detrng, 0x9E3779B97F4A7C15ull) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; z ^= z >> 31;
    uint64_t n = atomic_fetch_add(&g_detrng_n, 1) + 1; if (n == 1 || n % 1000 == 0) LOG("DM_DETCLOCK：确定熵第 %llu 次", (unsigned long long)n);
    return z;
}
HLE("getentropy", int, g_getentropy, void *p, size_t n) {
    if (!detclock_cfg()) { arc4random_buf(p, n); return 0; }
    for (size_t i = 0; i < n; i += 8) { uint64_t v = dm_entropy64(); memcpy((uint8_t *)p + i, &v, n - i < 8 ? n - i : 8); }
    return 0;
}
