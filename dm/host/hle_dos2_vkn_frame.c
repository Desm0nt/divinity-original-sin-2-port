// [DOS2] VK-NATIVE part "frame" (dm_vkn_frame_register; DM_VK_NATIVE=1 only): the engine's frame functions on the native backend
// (dm/gfx/vkn/vkn_frame.c recording / submit / present, vkn_pass.c clears and targets). main = main.raw of update v786432, build
// 4979b200; r = the renderer ([main+0x2bbdc68]); dev = [r+0xb70]; win = a window (base 0x93cf00, main window vtable 0x2984d00).
// Facts: docs/dos2/nr/VK-NATIVE-MAP-3.md section 3 and the disassembly / decompilations of 0x9215f0 0x93d9f0 0x93ccd0 0x93cef0 0x93cc20
// 0x93d200 0x912f70 0x928ab0 0x928be0 0x9291c0 0x9296d0 0x929880 0xa66bc0.
//
// REPLACED (mode 1), every engine-visible write kept, every NVN call dropped:
//   BeginFrame 0x9215f0(r, bit0)        bit0 1: nvnWindowAcquireTexture -> back-buffer index dev+0x31e0 = 0, 1, 0, ... (HLE NVN's rule:
//                                       next++ % 2 textures, SetTextures(2, dev+0x5238)), with the HLE's pacing wait (below);
//                                       nvnQueueWaitSync(queue, dev+0x31a0) dropped (one queue; the ring fences and the presenter order
//                                       the image); dev+0x5248 = dev+0x5250 + index * 0x1d8 (bit0 0: dev+0x5600); the frame's half of the
//                                       counter buffer [dev+0x25ac8] + index * 0x10000 reset to {0, -1} pairs, dev+0x25ac0 = 0;
//                                       AddCommandMemory / AddControlMemory / BeginRecording -> vkn_rec_begin(bit0); bit0 1: r+0x18f0 (f32
//                                       "GPU frame ms") = (ctx+0x120 - ctx+0x118) / 1e6 of this context's previous use, then ctx+0x118 /
//                                       ctx+0x120 = the backend's newest finished GPU timestamps (vkn_frame_gpu_ns; 0 / 0 without them);
//                                       ReportCounter(0x10) dropped. The global mutex 0x2bbee38 (only while byte 0x2bbee88 == 1) exactly as
//                                       the original, including its unlock tail 0x9217cc..0x9218b0: a countdown 0x2bbee58 (3 calls) after
//                                       which the memory pools listed in 0x2bbee70 / count 0x2bbee78 are freed (0x934910 + operator delete
//                                       0x83d3e0, engine code: pool bookkeeping served by HLE NVN until stage 4) and the two lists swap.
//   Window::BeginRender 0x93d9f0(win)   SetSamplerPool / SetTexturePool / SetShaderScratchMemory / SetRenderEnable(1) dropped; r+0x18c0 = 1;
//                                       0x91a040(r+0x1240) (a memset of 0x180 bytes: engine code); SetPolygonOffsetClamp(-2.5, -5.0, 0) ->
//                                       vkn_state_set_depth_bias once (constants); the staged buffer copies (hash map r+0x1898 count /
//                                       r+0x189c buckets / r+0x18a0 bucket array, 0x40-byte nodes {next, hash, descriptor 0x30}) each through
//                                       0x9218c0(r, &copy of node+0x10) (the buffer part's hook) in the original's order, then every node
//                                       freed and r+0x1898 = 0; the mip reductions (r+0x18b0 entries {handle, u32 levels}, count bits 24..47 of
//                                       r+0x18b8) through 0x920460 (the texture part's hook; its copy 0x920b00 is the pass part's), the list
//                                       emptied / shrunk as the original; mutex as the original. Then (after the unlock) the window clears
//                                       0x928ab0(r, win+0x60 or win+0x68, -1, (0,0,0,0)), the same for win+0x90 when [win+0x50] && its
//                                       f32 +0x2c != 1, and 0x928be0(r, win+0x80, -1, mode 2, stencil 0, depth 1.0) - as vkn_pass calls:
//                                       vkn_pass_clear_color (level 0, layer 0, whole), and for the depth: when win+0x80 != r+0x1828 (the
//                                       bound depth of the engine's cache, NOT updated by the original) vkn_pass_targets(0 colours, depth)
//                                       plus the original's viewport cache writes r+0x171c = w, r+0x1720 = h, r+0x1724 = r+0x1728 = 0 (only
//                                       if one differs), then vkn_pass_clear_depth(depth and stencil, 1.0, 0).
//   Window::EndFrame 0x93ccd0(win, b0)  ReportCounter dropped; b0 1: vkn_pass_end, vkn_rec_present(texture of win+0x68 = the one this
//                                       frame rendered: slot 10 swaps +0x68 / +0x70 in step with the index, crop of Resize, interval 2),
//                                       vkn_rec_submit(0); b0 0: vkn_rec_submit(1) (the original's FenceSync + Flush + SyncWait). Then as
//                                       the original: mutex drops between the steps, b0 1: 0x91abd0(r, 0) + 0x91ad20(r, 0) (deferred
//                                       destruction by dev+0x51f0: engine code), r+0x18c0 = 0, b0 1: the HLE's present-time pacing
//                                       bookkeeping, dev+0x51f0 += 1; window slot 10 (vtable +0x50, 0x93cef0 for the main window) with b0,
//                                       its result returned; b0 1: dm_vkn_tick().
//   Window::Resize 0x93cc20(win, w, h, flag)  0x93d200(win, w, h, flag & 1, w, h) and 0x912f70([win+0x50], w, h, f32 [win+0x44]) as the
//                                       original (engine code: sizes, listener targets); nvnWindowSetCrop(0, 0, w, h) -> the present crop.
//   ReadTexels users 0x9291c0 / 0x9296d0 / 0x929880: moved to the read-back part (hle_dos2_vkn_read.c, dm_vkn_read_register, called at
//                                       the end of this file's register until hle_dos2_vkn.c calls it itself; it is idempotent).
// Pacing (the HLE NVN path: hle_nvn.c nvnQueuePresentTexture computes the slot, nvnWindowAcquireTexture sleeps): present period =
// interval (1 in the display interval mode, the default with a known refresh; else the game's 2) * 1e9 / rate; rate = DM_DOS2_FPS's
// nvn_pace_target_hz(refresh, target) (0 = uncapped: no host pacing), else the old rule (power-saving 30, DM_PRESENT_MAX_HZ cap, 60
// tier of the refresh); DM_PRESENT_HZ pins a rate; at the present slot = now + nvn_pace(pacer, now, period) and the texture of this
// index is free again at slot + period; the next BeginFrame of that index sleeps until then. g_nvn_present_period_ns (DRS target of
// hle_dos2_quality.c) gets the period. Not reproduced: DM_PACE_ADAPT=1 (step-down tiers), DM_DETCLOCK pacing, the HLE's time-constraint
// thread registration, its per-frame stats rows (logged once when such a switch is set).
// Direct NVN calls left in the frame function 0xa66bc0 (screenshot / save-thumbnail branch, only while a read-back is requested):
// nvnCommandBufferBarrier(cb, 2) always; for a window that is not dev[10] also EndRecording + QueueSubmitCommands and FenceSync + Flush +
// SyncWait(ctx+0xd8). In mode 1 they reach HLE NVN on a command buffer that never began recording (this file never calls
// BeginRecording): without the sink of another part, Barrier / EndRecording append records after the context's last write position
// (5 MB, then "command memory exhausted"), the submit hands the translator a stale handle. The sink must swallow them.
// HLE NVN in mode 1 (device, queue, window still created through it; hle_nvn.c read 20.11): nvnWindowInitialize only stores the
// window; nvnWindowAcquireTexture / nvnQueuePresentTexture are never called, so no present ticket, no avail[] time and no acquire sync
// exist that anything could wait on (the acquire sync dev+0x31a0 was only waited on by the dropped QueueWaitSync; the contexts' ctx+0xd8
// syncs were fenced at creation). What the HLE present did besides presenting and is NOT done in mode 1: g_frame / dm_nvn_frame_count
// (hle_oe.c dynamic 3D scale needs >= 30 frames, hle_dos2_nr.c windows, diag.c perf file, hle_cpuprof.c rates), the translator's
// XJ_FRAME frame number, the per-frame stats rows and long-frame log, the DM_NB / Z5 frame marks: needs an export in hle_nvn.c, called
// here when it exists (dm_nvn_vkn_frame, weak). dm_cpu_place_self / dm_cpuprof_present are called here directly.
//
// [stage 5.5] DM_VK_NATIVE_RT (dm/gfx/vkn/vkn_rt.h; read here at registration, default 1; forced 0 with DM_VK_NATIVE_COND 1 / 3):
// the backend's calls of these hooks are records of the recording thread: BeginFrame's vkn_rec_begin (vkn_rt_rec_begin binds the engine
// thread as the encoder at the first one), BeginRender's clears and depth target (vkn_rt_pass_*), EndFrame's pass end / present / submit
// (vkn_rt_rec_end: an off-frame end syncs, the engine expects the GPU work finished) and the frame's tail (vkn_rt_frame_tail: the
// prof frame times and dm_vkn_tick run on the recording thread, then the backpressure). The engine-side work stays here, in the same
// order. vkn_rec_prewait is not called (the recording thread waits for its buffer in vkn_rec_begin); the pacing sleep stays on the engine
// thread. What the engine thread measures then: g_ft (begin / record / end) is the ENGINE thread's time (record = its encode time,
// end = EndFrame without the backpressure wait, which the "[vkn] rt:" line reports), the GPU ms r+0x18f0 = the newest finished pair as
// the recording thread saw it (vkn_frame_gpu_ns, read atomically). DM_VK_NATIVE_RT_SLOTS: the slot-array policy (default 1).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../gfx/vkn/vkn.h"
#include "../gfx/vkn/vkn_frame.h"
#include "../gfx/vkn/vkn_pass.h"
#include "../gfx/vkn/vkn_prof.h"   // [stage 5 A0] measurement
#include "../gfx/vkn/vkn_rt.h"     // [stage 5.5] the recording thread
#include "../gfx/vkn/vkn_state.h"
#include "../gfx/vkn/vkn_tex.h"
#include "../gfx/nvn/nvn_pace.h"
#include "hle_dos2_vkn.h"
#include "hle_dos2_vkn_stack.h"
void dm_vkn_glue_flush_tex(void) __attribute__((weak));   // hle_dos2_vkn_glue.c: textures written through their mapped storage

#define FH_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"   // the red-line mark, a literal of its own (the next letter may be a hex digit)

// Pacing inputs (gfx/vulkan/nvn_vk.c) and the DRS period (gfx/nvn/hle_nvn.c). Weak: absent in builds without them (then: 60 Hz, no DRS).
extern double mtl_display_refresh(void) __attribute__((weak));
extern int mtl_has_window(void) __attribute__((weak));
extern int mtl_fps_target(void) __attribute__((weak));
extern int dm_dos2_fps(void) __attribute__((weak));
extern _Atomic uint64_t g_nvn_present_period_ns __attribute__((weak));
// [proposal] hle_nvn.c: the frame-boundary bookkeeping of nvnQueuePresentTexture without its present (frame number g_frame /
// dm_nvn_frame_count, the XJ_FRAME job of the translator, per-frame stats). Called once a presented native frame when it exists.
extern void dm_nvn_vkn_frame(void) __attribute__((weak));
// [no-NVN] step D (hle_dos2_vkn_nonvn.c): the frame counter / on-screen stats of the port kept by the backend; 1 = done (hle_nvn.c is
// then not called per frame), 0 = the step is off (dm_nvn_vkn_frame as before).
extern int dm_vkn_nonvn_frame(void) __attribute__((weak));
// Host per-present calls of nvnQueuePresentTexture that guard themselves (cpu_topo.h / hle_cpuprof.c): kept at the native present.
extern void dm_cpu_place_self(void) __attribute__((weak));    // [NR-CPU] DM_CPU_SELF: the main thread re-asserts its own mask
extern void dm_cpuprof_present(void) __attribute__((weak));   // [NR-16] DM_NR_CPU: the first present starts the sampler

// ---- engine offsets ----
enum {
    R_DEV = 0xb70, R_VP_W = 0x171c, R_VP_H = 0x1720, R_VP_X = 0x1724, R_VP_Y = 0x1728, R_DEPTH = 0x1828,
    R_STATE = 0x1240, R_SC_N = 0x1898, R_SC_NB = 0x189c, R_SC_B = 0x18a0, R_MIP = 0x18b0, R_MIP_META = 0x18b8, R_REC = 0x18c0, R_GPU_MS = 0x18f0,
    DEV_BACK = 0x31e0, DEV_FRAME = 0x51f0, DEV_CTX = 0x5248, DEV_CTX0 = 0x5250, CTX_BYTES = 0x1d8, DEV_CTX_OFF = 0x5600,
    DEV_CNT_USED = 0x25ac0, DEV_CNT_BUF = 0x25ac8, CTX_TS0 = 0x118, CTX_TS1 = 0x120,
    WIN_W = 0x18, WIN_H = 0x1c, WIN_SCALE = 0x44, WIN_R = 0x48, WIN_POOL = 0x50, WIN_OVR = 0x60, WIN_BACK = 0x68, WIN_DEPTH = 0x80, WIN_HALF = 0x90,
    POOL_SCALE = 0x2c,
    TEX_TAB = 0x000, TEX_REC = 0x28,                 // the engine's texture table r+0 (0x928ab0 / 0x928be0 look handles up there)
    G_MU = 0x2bbee38, G_MU_ON = 0x2bbee88, G_PD_CNT = 0x2bbee58, G_PD_A = 0x2bbee60, G_PD_B = 0x2bbee70,
    FN_LOCK = 0x1bf4560, FN_UNLOCK = 0x1bf4570, FN_POOL_FREE = 0x934910, FN_DELETE = 0x83d3e0, FN_STATE_RESET = 0x91a040, FN_STAGED = 0x9218c0,
    FN_MIP = 0x920460, FN_DEFER_A = 0x91abd0, FN_DEFER_B = 0x91ad20, FN_WIN_SIZE = 0x93d200, FN_TARGETS = 0x912f70,
    WIN_TEXTURES = 2, WIN_INTERVAL = 2,              // 0x939120: nvnWindowBuilderSetTextures(2, dev+0x5238), SetPresentInterval(2)
};
static inline void wr64(uint64_t p, uint64_t v) { memcpy((void *)(uintptr_t)p, &v, 8); }
static inline void wr32(uint64_t p, uint32_t v) { memcpy((void *)(uintptr_t)p, &v, 4); }
static inline void wr8(uint64_t p, uint8_t v) { *(uint8_t *)(uintptr_t)p = v; }
static inline float rdf(uint64_t p) { float f; memcpy(&f, (const void *)(uintptr_t)p, 4); return f; }
static inline uint64_t gaddr(uint32_t off) { return (uint64_t)(uintptr_t)(dm_vkn_base() + off); }
static inline void *eng(uint32_t off) { return dm_vkn_base() + off; }

typedef void (*fn_1)(uint64_t);
typedef uint64_t (*fn_2)(uint64_t, uint64_t);
typedef uint64_t (*fn_3)(uint64_t, uint64_t, uint64_t);
typedef uint64_t (*fn_6)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef void (*fn_targets)(uint64_t pool, uint32_t w, uint32_t h, float scale);   // 0x912f70: s0 = the window's scale

// ---- counters (census) and red lines ----
enum { RL_DEV, RL_WINTEX, RL_CLEAR, RL_DEPTH, RL_PACE, RL_N };
static struct {
    uint64_t begins, renders, ends_presented, ends_off, resizes, pace_waits, pace_ns, wintex_unknown, clear_unknown, depth_unknown, no_dev;
    uint64_t staged, mips;
    uint32_t red[RL_N];
} g_fs;
static int red_ok(int k) { return g_fs.red[k]++ < 16; }

// ---- the engine's global mutex 0x2bbee38 (taken only while the byte 0x2bbee88 is 1; the engine tests == 1 to lock, bit 0 to unlock
// in the middle of EndFrame and == 1 again at its end: kept as such) ----
static int mu_on(void) { return vkn_rd8(gaddr(G_MU_ON)) == 1; }
static void mu_lock(void) { ((fn_1)eng(FN_LOCK))(gaddr(G_MU)); }
static void mu_unlock(void) { ((fn_1)eng(FN_UNLOCK))(gaddr(G_MU)); }
// The unlock at the end of BeginFrame (0x9217cc..0x9218b0).
static void bf_unlock_tail(void) {
    if (!mu_on()) return;
    mu_unlock();
    if (!mu_on()) return;
    const uint64_t cnt = gaddr(G_PD_CNT), a = gaddr(G_PD_A), b = gaddr(G_PD_B);
    const uint32_t c = vkn_rd32(cnt);
    if (c) { wr32(cnt, c - 1); return; }
    mu_lock();
    for (uint32_t i = 0; i < vkn_rd32(b + 8); i++) {   // (count re-read after every free, as the original)
        const uint64_t p = vkn_rd64(vkn_rd64(b) + 8ull * i);
        if (p) { ((fn_1)eng(FN_POOL_FREE))(p); ((fn_1)eng(FN_DELETE))(p); }
    }
    const uint32_t a_cnt = vkn_rd32(a + 8), a_cap = vkn_rd32(a + 0xc), b_cap = vkn_rd32(b + 0xc);
    const uint64_t a_ptr = vkn_rd64(a), b_ptr = vkn_rd64(b);
    wr32(cnt, 3);
    wr32(b + 8, a_cnt); wr32(b + 0xc, a_cap); wr32(a + 8, 0); wr32(a + 0xc, b_cap); wr64(b, a_ptr); wr64(a, b_ptr);
    mu_unlock();
}

// ---- pacing (hle_nvn.c present_cfg / present_auto_hz / present_period / nvnWindowAcquireTexture / nvnQueuePresentTexture) ----
static struct { int cfg, hz_fixed, hz_auto, display; nvn_pacer pc; uint64_t avail[WIN_TEXTURES], period; uint32_t next, idx; } g_pace;
static double cap_hz(void) { const char *e = getenv("DM_PRESENT_MAX_HZ"); const double v = e && *e ? atof(e) : 0; return v >= 10 && v <= 240 ? v : 0; }
static double refresh(void) { return mtl_display_refresh ? mtl_display_refresh() : 0; }
static void pace_cfg(void) {
    if (g_pace.cfg) return;
    g_pace.cfg = 1;
    const int scr = (int)(refresh() + 0.5);
    const char *e = getenv("DM_PRESENT_HZ"), *m = getenv("DM_PRESENT_INTERVAL");
    g_pace.hz_fixed = e ? atoi(e) : 60; g_pace.hz_auto = !e && scr > 0;
    if (!e && !scr && dm_dos2_fps && dm_dos2_fps() >= 0 && mtl_has_window && mtl_has_window()) g_pace.hz_auto = 1;   // (refresh not reported yet)
    g_pace.display = m ? !strcmp(m, "display") : scr > 0;
    if (!m && g_pace.hz_auto && !scr) g_pace.display = 1;
    const char *pa = getenv("DM_PACE_ADAPT"), *dc = getenv("DM_DETCLOCK");
    if (((pa && *pa == '1') || (dc && *dc)) && red_ok(RL_PACE)) vkn_logf(FH_RED "frame: %s set: not reproduced by the native frame (plain pacing)", pa && *pa == '1' ? "DM_PACE_ADAPT=1" : "DM_DETCLOCK");
    vkn_logf("frame: pacing %s, interval mode %s (game interval %d), refresh %.2f Hz, DOS2 target %d", g_pace.hz_auto ? "by the refresh" : e ? "pinned by DM_PRESENT_HZ" : "fixed 60 Hz",
             g_pace.display ? "display" : "game", WIN_INTERVAL, refresh(), dm_dos2_fps ? dm_dos2_fps() : -1);
}
static double pace_hz(double screen_hz) {
    const int t = dm_dos2_fps ? dm_dos2_fps() : -1;
    if (t >= 0) return nvn_pace_target_hz(screen_hz, t);
    if (mtl_fps_target && mtl_fps_target() == 30) return nvn_pace_hz(screen_hz, 30);
    if (cap_hz() > 0) return nvn_pace_cap_hz(screen_hz, cap_hz());
    return nvn_pace_hz(screen_hz, 60);
}
static uint64_t pace_period(void) {
    pace_cfg();
    const int itv = g_pace.display ? 1 : WIN_INTERVAL;
    const double hz = g_pace.hz_auto ? pace_hz(refresh()) : g_pace.hz_fixed;
    const uint64_t p = hz > 0 ? (uint64_t)(itv * 1e9 / hz) : 0;
    if (p != g_pace.period) vkn_logf("frame: present period %.3f ms (%.2f Hz on a %.2f Hz screen)", p / 1e6, hz, refresh());
    g_pace.period = p;
    if (&g_nvn_present_period_ns) atomic_store(&g_nvn_present_period_ns, p);
    return p;
}
static void sleep_until(uint64_t t) {
    const struct timespec ts = {(time_t)(t / 1000000000ull), (long)(t % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}
// AcquireTexture: the next index; its texture is free at avail[] (set by the present two frames ago).
static uint32_t pace_acquire(void) {
    const uint32_t idx = g_pace.next++ % WIN_TEXTURES;
    const uint64_t at = g_pace.avail[idx], now = vkn_now_ns();
    if (at > now) { sleep_until(at); g_fs.pace_waits++; g_fs.pace_ns += at - now; }
    g_pace.idx = idx;
    return idx;
}
// PresentTexture: this frame's display slot by the pacer; the texture of this index can be acquired again one period after it.
static void pace_present(void) {
    const uint64_t period = pace_period(), now = vkn_now_ns();
    const uint64_t slot = now + nvn_pace(&g_pace.pc, now, period);
    g_pace.avail[g_pace.idx] = period ? slot + period : 0;
}

// ---- the present crop (Window::Resize -> nvnWindowSetCrop(0, 0, w, h)); none until the engine resizes = the whole image ----
static int32_t g_crop[4]; static int g_crop_set;

// ---- DM_VK_NATIVE_LOG (hle_dos2_vkn.c prints them): ms of the last presented frame in BeginFrame, from BeginRender to EndFrame
// (the frame's recording: every list the engine executes), in EndFrame (submit + present + the engine's tail, without the tick) ----
static struct { uint64_t begin, render0, render, end; } g_ft;
void dm_vkn_frame_times(double ms[3]);
void dm_vkn_frame_times(double ms[3]) { ms[0] = g_ft.begin / 1e6; ms[1] = g_ft.render / 1e6; ms[2] = g_ft.end / 1e6; }

// ---- hooks ----
static vkn_fn g_o_begin, g_o_render, g_o_end, g_o_resize;   // (originals: never called in mode 1)

VKN_HOOK fh_begin(VKN_A) {   // 0x9215f0 BeginFrame(r, bit0)
    VKN_UNUSED;
    const uint64_t r = a0; const int presented = (int)(a1 & 1);
    const uint64_t dev = r ? vkn_rd64(r + R_DEV) : 0, t0 = vkn_now_ns();
    g_fs.begins++;
    if (!dev) { g_fs.no_dev++; if (red_ok(RL_DEV)) vkn_logf(FH_RED "frame: BeginFrame(r %#llx, %d) without a device: nothing done", (unsigned long long)r, presented); return 0; }
    uint64_t ctx;
    if (presented) {
        const uint32_t idx = pace_acquire();   // nvnWindowAcquireTexture: before the mutex, as the original
        if (!vkn_rt_mode()) vkn_rec_prewait(1);   // the ring buffer's fence: also outside the mutex ([stage 5.5] the recording thread's own)
        wr32(dev + DEV_BACK, idx);
        if (mu_on()) mu_lock();
        ctx = dev + DEV_CTX0 + (uint64_t)idx * CTX_BYTES;
        wr64(dev + DEV_CTX, ctx);
        const uint64_t half = vkn_rd64(dev + DEV_CNT_BUF) + ((uint64_t)idx << 16);   // ((idx << 12) counters of 16 bytes)
        for (uint32_t k = 0; k < 0x1000; k++) wr64(half + 16ull * k, k & 1 ? ~0ull : 0);   // {0, -1} per query (begin, end counter value)
        wr32(dev + DEV_CNT_USED, 0);
    } else {
        if (!vkn_rt_mode()) vkn_rec_prewait(0);
        if (mu_on()) mu_lock();
        ctx = dev + DEV_CTX_OFF;
        wr64(dev + DEV_CTX, ctx);
    }
    vkn_rt_rec_begin(presented);   // (a failure is counted by vkn_frame; the recorders then see no command buffer) [stage 5.5] a record
    if (presented) {
        const float ms = (float)((double)(vkn_rd64(ctx + CTX_TS1) - vkn_rd64(ctx + CTX_TS0)) / 1000000.0);   // 0x92175c: from the previous use
        memcpy((void *)(uintptr_t)(r + R_GPU_MS), &ms, 4);
        uint64_t b = 0, e = 0;
        if (!vkn_frame_gpu_ns(&b, &e)) b = e = 0;
        wr64(ctx + CTX_TS0, b); wr64(ctx + CTX_TS1, e);
    }
    bf_unlock_tail();
    if (presented) g_ft.begin = vkn_now_ns() - t0;
    return 0;
}

// The window clears of 0x93d9f0 (after its unlock): colour 0x928ab0(r, h, -1, zeros), depth 0x928be0(r, h, -1, 2, 0, 1.0).
static void clear_color(uint64_t r, uint64_t h) {
    if (!vkn_rec(r, TEX_TAB, TEX_REC, h)) return;   // stale / empty handle: the original does nothing
    vkn_tex *t = vkn_tex_get(h);
    if (!t) { g_fs.clear_unknown++; if (red_ok(RL_CLEAR)) vkn_logf(FH_RED "frame: window colour clear of %#llx: texture unknown to the backend, dropped", (unsigned long long)h); return; }
    static const float zero[4] = {0, 0, 0, 0};
    vkn_rt_pass_clear_color(t, 0, zero, NULL);   // [stage 5.5] a record
}
static void clear_depth(uint64_t r, uint64_t h) {
    vkn_tex *t = vkn_rec(r, TEX_TAB, TEX_REC, h) ? vkn_tex_get(h) : NULL;
    if (!t) {   // (the original dereferences the record when it is not the bound depth: a stale handle never reaches it)
        g_fs.depth_unknown++;
        if (red_ok(RL_DEPTH)) vkn_logf(FH_RED "frame: window depth clear of %#llx (bound %#llx): texture %s, dropped", (unsigned long long)h, (unsigned long long)vkn_rd64(r + R_DEPTH),
                                       vkn_rec(r, TEX_TAB, TEX_REC, h) ? "unknown to the backend" : "handle stale");
        return;
    }
    if (vkn_rd64(r + R_DEPTH) != h) {   // nvnCommandBufferSetRenderTargets(0, NULL, NULL, depth, NULL) + full viewport / scissor
        vkn_tex *none[VKN_PASS_COLORS] = {0}; const uint32_t zero[VKN_PASS_COLORS] = {0};
        vkn_rt_pass_targets(0, none, zero, zero, t);   // [stage 5.5] a record
        if (vkn_rd32(r + R_VP_W) != t->w || vkn_rd32(r + R_VP_H) != t->h || vkn_rd32(r + R_VP_X) || vkn_rd32(r + R_VP_Y)) {
            wr32(r + R_VP_W, t->w); wr32(r + R_VP_H, t->h); wr64(r + R_VP_X, 0);   // (0x928cb0: one 8-byte store of zero over +0x1724 / +0x1728)
        }
    }
    vkn_rt_pass_clear_depth(t, 3, 1.0f, 0);   // mode 2: depth (mask 1) and stencil (value 0, mask 0xffffffff) ([stage 5.5] a record)
}

VKN_HOOK fh_render(VKN_A) {   // 0x93d9f0 Window::BeginRender(win)
    VKN_UNUSED;
    const uint64_t win = a0, r = win ? vkn_rd64(win + WIN_R) : 0;
    g_fs.renders++; g_ft.render0 = vkn_now_ns();
    if (!r) { g_fs.no_dev++; if (red_ok(RL_DEV)) vkn_logf(FH_RED "frame: BeginRender(win %#llx) without a renderer: nothing done", (unsigned long long)win); return 0; }
    if (mu_on()) mu_lock();
    wr8(r + R_REC, 1);
    ((fn_1)eng(FN_STATE_RESET))(r + R_STATE);
    static int bias; if (!bias) { bias = 1; vkn_state_set_depth_bias(-2.5f, -5.0f, 0.0f, 1.0f); }   // SetPolygonOffsetClamp(-2.5, -5.0, 0), the same every frame
    // Staged buffer copies (0x93dac8..0x93db58): bucket order, each chain to its end; the next bucket = (last node's hash % buckets) + 1.
    uint32_t nb = vkn_rd32(r + R_SC_NB);
    if (nb) {
        uint64_t node = 0; uint32_t i = 0;
        for (; i < nb && !(node = vkn_rd64(vkn_rd64(r + R_SC_B) + 8ull * i)); i++) {}
        while (node) {
            uint64_t last;
            do {
                uint8_t copy[0x40] __attribute__((aligned(16)));   // the original copies the whole node to its stack and passes +0x10
                last = node; memcpy(copy, (const void *)(uintptr_t)node, sizeof copy);
                ((fn_2)eng(FN_STAGED))(r, (uint64_t)(uintptr_t)(copy + 0x10)); g_fs.staged++;
                node = vkn_rd64(last);
            } while (node);
            nb = vkn_rd32(r + R_SC_NB);
            if (!nb) break;
            for (i = (uint32_t)(vkn_rd64(last + 8) % nb) + 1; i < nb && !(node = vkn_rd64(vkn_rd64(r + R_SC_B) + 8ull * i)); i++) {}
            if (i >= nb) node = 0;
        }
    }
    // Mip reductions (0x93db5c..0x93dbb8, 0x93dd5c..0x93dd84).
    uint64_t m = vkn_rd64(r + R_MIP_META);
    const uint32_t nm = (uint32_t)(m >> 24 & 0xffffff);
    if (nm) {
        const uint64_t p = vkn_rd64(r + R_MIP);
        for (uint32_t k = 0; k < nm; k++) { ((fn_3)eng(FN_MIP))(r, vkn_rd64(p + 16ull * k), vkn_rd32(p + 16ull * k + 8)); g_fs.mips++; }
        m = vkn_rd64(r + R_MIP_META);
    }
    uint64_t x8 = m & 0xffff000000ffffffull;
    wr64(r + R_MIP_META, x8);
    if (m >> 23 & 1) {   // owned storage of more than 32 entries: freed
        const uint32_t cap = (uint32_t)(m & 0x7fffff);
        if (cap * 6u >= 7 && cap >= 0x21) {
            const uint64_t keep = m & 0x800000;
            wr64(r + R_MIP_META, x8);
            const uint64_t p = vkn_rd64(r + R_MIP);
            if (p) { ((fn_1)eng(FN_DELETE))(p); x8 = vkn_rd64(r + R_MIP_META); }
            wr64(r + R_MIP, 0); wr64(r + R_MIP_META, keep | (x8 & 0xffffffffff000000ull));
        }
    }
    // Every node of the copy map freed, buckets emptied (0x93dbbc..0x93dc9c), then the count.
    nb = vkn_rd32(r + R_SC_NB);
    for (uint32_t i = 0; i < nb; i++) {
        for (uint64_t nd; (nd = vkn_rd64(vkn_rd64(r + R_SC_B) + 8ull * i));) { wr64(vkn_rd64(r + R_SC_B) + 8ull * i, vkn_rd64(nd)); ((fn_1)eng(FN_DELETE))(nd); }
        nb = vkn_rd32(r + R_SC_NB);
    }
    wr32(r + R_SC_N, 0);
    if (mu_on()) mu_unlock();
    // The window clears.
    const uint64_t ovr = vkn_rd64(win + WIN_OVR);
    clear_color(r, ovr ? ovr : vkn_rd64(win + WIN_BACK));
    const uint64_t pool = vkn_rd64(win + WIN_POOL);
    if (pool && rdf(pool + POOL_SCALE) != 1.0f) clear_color(r, vkn_rd64(win + WIN_HALF));
    clear_depth(r, vkn_rd64(win + WIN_DEPTH));
    return 0;
}

VKN_HOOK fh_end(VKN_A) {   // 0x93ccd0 Window::EndFrame(win, bit0 present) = window slot 8
    VKN_UNUSED;
    const uint64_t win = a0; const int present = (int)(a1 & 1);
    const uint64_t r = win ? vkn_rd64(win + WIN_R) : 0, dev = r ? vkn_rd64(r + R_DEV) : 0, t0 = vkn_now_ns();
    if (!dev) { g_fs.no_dev++; if (red_ok(RL_DEV)) vkn_logf(FH_RED "frame: EndFrame(win %#llx, %d) without a device: nothing done", (unsigned long long)win, present); return 0; }
    if (present && g_ft.render0) { g_ft.render = t0 - g_ft.render0; g_ft.render0 = 0; }
    if (present) g_fs.ends_presented++; else g_fs.ends_off++;
    if (mu_on()) mu_lock();
    if (dm_vkn_glue_flush_tex) dm_vkn_glue_flush_tex();   // audit U1: a texture written through its storage after the frame's last draw
    vkn_tex *pt = NULL;
    if (present) {
        const uint64_t h = vkn_rd64(win + WIN_BACK);
        pt = vkn_tex_get(h);
        if (!pt) { g_fs.wintex_unknown++; if (red_ok(RL_WINTEX)) vkn_logf(FH_RED "frame: present of window texture %#llx (index %u): unknown to the backend, frame not shown", (unsigned long long)h, g_pace.idx); }
    }
    // vkn_pass_end + vkn_rec_present when present, then EndRecording + SubmitCommands (+ for b0 0 the original's FenceSync / Flush /
    // SyncWait): vkn_rec_submit(!present). [stage 5.5] one record (an off-frame end syncs after it)
    vkn_rt_rec_end(present, pt, g_crop_set ? g_crop : NULL, WIN_INTERVAL);
    if (vkn_rd8(gaddr(G_MU_ON)) & 1) { mu_unlock(); if (mu_on()) mu_lock(); }
    if (present) { ((fn_2)eng(FN_DEFER_A))(r, 0); ((fn_2)eng(FN_DEFER_B))(r, 0); }
    if (vkn_rd8(gaddr(G_MU_ON)) & 1) { mu_unlock(); if (mu_on()) mu_lock(); }
    wr8(r + R_REC, 0);
    if (present) {   // nvnQueuePresentTexture's pacing, then the frame counter
        pace_present();
        wr64(dev + DEV_FRAME, vkn_rd64(dev + DEV_FRAME) + 1);
        if (!(dm_vkn_nonvn_frame && dm_vkn_nonvn_frame()) && dm_nvn_vkn_frame) dm_nvn_vkn_frame();   // [no-NVN] D first
        if (dm_cpu_place_self) dm_cpu_place_self();
        if (dm_cpuprof_present) dm_cpuprof_present();
    }
    if (mu_on()) mu_unlock();
    const uint64_t slot10 = vkn_rd64(vkn_rd64(win) + 0x50);   // (the original tail-branches to it)
    const uint64_t rc = ((fn_2)(uintptr_t)slot10)(win, (uint64_t)present);
    if (present) {   // [stage 5 A0] frame_cpu; [stage 5.5] with the tick a record of the recording thread (+ the backpressure)
        g_ft.end = vkn_now_ns() - t0;
        const uint64_t cpu[3] = {g_ft.begin, g_ft.render, g_ft.end};
        vkn_rt_frame_tail(cpu, dm_vkn_tick);
    }
    return rc;
}

VKN_HOOK fh_resize(VKN_A) {   // 0x93cc20 Window::Resize(win, w, h, flag) = window slot 4
    VKN_UNUSED;
    const uint64_t win = a0; const uint32_t w = (uint32_t)a1, h = (uint32_t)a2;
    g_fs.resizes++;
    const uint64_t rc = ((fn_6)eng(FN_WIN_SIZE))(win, w, h, a3 & 1, w, h);   // (0x93cc30..0x93cc48: w3 = flag & 1, w4 / w5 = w / h)
    ((fn_targets)eng(FN_TARGETS))(vkn_rd64(win + WIN_POOL), w, h, rdf(win + WIN_SCALE));
    const int32_t c[4] = {0, 0, (int32_t)w, (int32_t)h};
    if (!g_crop_set || memcmp(c, g_crop, sizeof c)) vkn_logf("frame: window %#llx resized to %ux%u: present crop 0,0 %ux%u", (unsigned long long)win, w, h, w, h);
    memcpy(g_crop, c, sizeof c); g_crop_set = 1;
    return rc & 1;
}

// ---- census ----
static void fh_census(void) {
    vkn_frame_stats s; vkn_frame_stats_get(&s);
    vkn_frame_xstats x; vkn_frame_xstats_get(&x);
    vkn_logf("frame: frames %llu, off-frame %llu, submits %llu (lost %llu), fence waits %llu / %.1f ms (timeouts %llu), uploads %llu, presents %llu (failed %llu), begin failed %llu, left open %llu, present-thread waits %llu / %.1f ms",
             (unsigned long long)s.frames, (unsigned long long)s.offframe, (unsigned long long)s.submits, (unsigned long long)x.submit_fail, (unsigned long long)s.waits, s.wait_ns / 1e6,
             (unsigned long long)x.fence_timeouts, (unsigned long long)s.uploads, (unsigned long long)x.presents, (unsigned long long)s.present_fail, (unsigned long long)s.begin_fail,
             (unsigned long long)x.left_open, (unsigned long long)x.present_waits, x.present_wait_ns / 1e6);
    vkn_logf("frame engine: BeginFrame %llu, BeginRender %llu, EndFrame %llu presented / %llu off, staged copies %llu, mip reductions %llu, resizes %llu; pacing %.3f ms period, %llu sleeps / %.1f ms; dropped: window texture %llu, colour clear %llu, depth clear %llu, no device %llu",
             (unsigned long long)g_fs.begins, (unsigned long long)g_fs.renders, (unsigned long long)g_fs.ends_presented, (unsigned long long)g_fs.ends_off, (unsigned long long)g_fs.staged,
             (unsigned long long)g_fs.mips, (unsigned long long)g_fs.resizes, g_pace.period / 1e6, (unsigned long long)g_fs.pace_waits, g_fs.pace_ns / 1e6,
             (unsigned long long)g_fs.wintex_unknown, (unsigned long long)g_fs.clear_unknown, (unsigned long long)g_fs.depth_unknown, (unsigned long long)g_fs.no_dev);
    vkn_prof_census();   // [stage 5 A0] "draw by part", "backend measured"
}

void dm_vkn_frame_register(void);   // hle_dos2_vkn.c (weak there)
void dm_vkn_read_register(void) __attribute__((weak));   // hle_dos2_vkn_read.c: the read-back sites moved there (idempotent)
void dm_vkn_frame_register(void) {
    // Mode 2 has nothing of this part (all sites VKN_M_NATIVE): no census either, so mode 2's 10 s lines stay those of stage 2.
    // (hle_dos2_vkn.c decides its own EndFrame hook by this function's presence, not by what it registers.)
    if (vkn_mode() != VKN_NATIVE) return;
    static const uint32_t k_begin[4] = {0xf81c0ff7 /* str x23,[sp,#-0x40]! */, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};
    static const uint32_t k_render[4] = {0xd10203ff /* sub sp,sp,#0x80 */, 0xa9045ff8, 0xa90557f6, 0xa9064ff4};
    static const uint32_t k_end[4] = {0xf81c0ff7 /* str x23,[sp,#-0x40]! */, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};
    static const uint32_t k_resize[4] = {0xa9bd57f6 /* stp x22,x21,[sp,#-0x30]! */, 0xa9014ff4, 0xa9027bfd, 0x910083fd};
    dm_vkn_site(0x9215f0, k_begin, "BeginFrame", (void *)fh_begin, &g_o_begin, VKN_M_NATIVE);
    dm_vkn_site(0x93d9f0, k_render, "Window::BeginRender", (void *)fh_render, &g_o_render, VKN_M_NATIVE);
    dm_vkn_site(0x93ccd0, k_end, "Window::EndFrame", (void *)fh_end, &g_o_end, VKN_M_NATIVE);
    dm_vkn_site(0x93cc20, k_resize, "Window::Resize", (void *)fh_resize, &g_o_resize, VKN_M_NATIVE);
    dm_vkn_census(fh_census);
    {   // [stage 5.5] DM_VK_NATIVE_RT / _SLOTS (vkn_rt.h): the recording thread, the slot-array policy
        const char *e = getenv("DM_VK_NATIVE_RT"), *sl = getenv("DM_VK_NATIVE_RT_SLOTS"), *qc = getenv("DM_VK_NATIVE_COND");
        int rt = e && *e ? atoi(e) : 1;
        const int cond = qc && *qc ? atoi(qc) : 0;
        if ((cond == 1 || cond == 3) && rt) { vkn_logf("rt: DM_VK_NATIVE_COND=%d: the engine reads the visibility verdicts the recording makes: DM_VK_NATIVE_RT=%d forced 0", cond, rt); rt = 0; }
        vkn_rt_install(rt, sl && *sl ? atoi(sl) : VKN_RT_SLOTS_CMP);
        {   // [rt P1] / [rt P2] DM_VK_NATIVE_RT_LEAN (0 / 1, default 1), DM_VK_NATIVE_RT_RES (0 / 1 / 2 verify, default 1): vkn_rt.h
            const char *ln = getenv("DM_VK_NATIVE_RT_LEAN"), *rs = getenv("DM_VK_NATIVE_RT_RES");
            vkn_rt_lean_set(ln && *ln ? atoi(ln) : 1); vkn_rt_res_set(rs && *rs ? atoi(rs) : 1);
            vkn_logf("rt: DM_VK_NATIVE_RT_LEAN=%d%s, DM_VK_NATIVE_RT_RES=%d%s", vkn_rt_lean(), ln && *ln ? "" : " (default)", vkn_rt_res(), rs && *rs ? "" : " (default)");
        }
        vkn_logf("rt: DM_VK_NATIVE_RT=%d%s, DM_VK_NATIVE_RT_SLOTS=%s (the recording thread starts at the first BeginFrame)", vkn_rt_mode(), e && *e ? "" : " (default)", sl && *sl ? sl : "1 (default)");
        if (vkn_rt_mode()) dm_vkn_census(vkn_rt_census);
        // the frame hooks encode their backend work themselves: no default sync at their entry (their nested engine calls may sync)
        dm_vkn_stack_flag((void *)fh_begin, DM_VKN_SF_ENC); dm_vkn_stack_flag((void *)fh_render, DM_VKN_SF_ENC);
        dm_vkn_stack_flag((void *)fh_end, DM_VKN_SF_ENC); dm_vkn_stack_flag((void *)fh_resize, DM_VKN_SF_ENC);
    }
    {   // [stage 5 A0] DM_VK_NATIVE_PROF (replaces the old path's DM_NR_HEAT / DM_NR_TCOUNT): 0 off, 1 (default) 10 s lines + a GPU sample every 20 s, 2 every 2 s
        const char *e = getenv("DM_VK_NATIVE_PROF");
        vkn_prof_init(e && *e ? atoi(e) : 1);
    }
    if (dm_vkn_read_register) dm_vkn_read_register();
}
#endif
