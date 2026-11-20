// [DOS2] NR-DRIFT (docs/dos2/nr/NR-DRIFT.md): "after a location starts we lose 10-15 fps for good". Pure C (C and C++): no Vulkan,
// no game addresses. Used by the backend (xbg_nr_growth.inc, namespace nrgrowth: the 10-s "[nr] growth" line, the GPU clock probe)
// and by the DRS monitor (dm/host/hle_dos2_quality.c: DM_DOS2_DRS_SETTLE); dm/gfx/vulkan/engine/tests/nr_drift_test.c tests it on
// the PC.
//
// What the logs showed (NR-DRIFT.md section 1): the fps after a load is not the fps of the scene. The game's dynamic resolution
// (DRS, a Switch-build addition; hle_dos2_quality.c) reads a GPU frame time of up to 29-55 ms in the first frames after a load
// screen (pipelines, texture uploads, the first shadow pass; "[quality] GPU frame time ... max 29.12"), its model then jumps
// straight to the minimum (75 % of the window height = 56 % of the pixels), and it climbs back 1.25 % a second: ~20 s of 105-122
// fps at a lower resolution, then the real 77-82 fps at 100 %. With a fixed resolution (DM_DOS2_DRS=75:75, nr13_d3) the fps is flat
// from the first second; at 100 % it is flat for minutes after the climb (live-1118 pid 21243, the third load of the same save:
// 76.9 fps / 12.60 ms GPU for 3 min 8 s). Nothing in the port grows with play time (section 2).
//
//   nrd_load_*      a load screen as the backend sees it: fewer than NRD_LOAD_DRAWS draws a frame in a perf window (DOS2 load
//                   screens draw 13-15, the 3D menu ~1200, the campaign 700-2500); the first window at or above it ends the load.
//   nrd_settle_*    DM_DOS2_DRS_SETTLE: hold the DRS controller's GPU target at 100 ms (it never steps down; the game's own "DRS off"
//                   path) while a load screen is up and for `hold_ns` after it ends, so the load-time spikes cannot throw the
//                   resolution to the minimum. Mode 2 counts only: what the controller did inside the would-be hold windows.
//   nrd_probe_*     the GPU clock probe: a fixed ALU loop (xbg_nr_probe.comp) and a fixed buffer copy, each between two GPU stamps,
//                   once per period. Calibration keeps the loop at 40..400 us; the index = first stable ns / iteration over now
//                   (100 = the clock of the first sample, 80 = 20 % slower: DVFS / thermal). The copy gives GB/s of DRAM traffic.
#ifndef DM_NR_DRIFT_H
#define DM_NR_DRIFT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NRD_LOAD_DRAWS = 64 };

// ---- load screens, from the backend's per-window draws a frame ----
typedef struct {
    int in_load;            // 1 while the last window had fewer than NRD_LOAD_DRAWS draws a frame (and at start: nothing seen yet)
    uint32_t loads;         // load screens seen end (a window under the limit followed by one at or above it)
    uint64_t load_end_ns;   // when the last load ended (0 = none yet)
} nrd_load_t;
static inline void nrd_load_init(nrd_load_t *l) { l->in_load = 1; l->loads = 0; l->load_end_ns = 0; }
// returns 1 on the window that ends a load screen, -1 on the window that starts one, 0 otherwise
static inline int nrd_load_step(nrd_load_t *l, double draws_a_frame, uint64_t now_ns) {
    const int low = draws_a_frame < (double)NRD_LOAD_DRAWS;
    if (low && !l->in_load) { l->in_load = 1; return -1; }
    if (!low && l->in_load) { l->in_load = 0; l->loads++; l->load_end_ns = now_ns; return 1; }
    return 0;
}

// ---- DM_DOS2_DRS_SETTLE ----
enum { NRD_SETTLE_OFF = 0, NRD_SETTLE_APPLY = 1, NRD_SETTLE_COUNT = 2 };
typedef struct {
    int mode;               // NRD_SETTLE_*
    uint64_t hold_ns;       // how long after a load ends the hold lasts
    int holding;            // 1 = inside a (would-be) hold window
    uint64_t until_ns;      // end of the window once the load ended (0 while the load screen is still up)
    uint32_t windows;       // windows opened (load screens seen start, incl. the boot)
    uint32_t released;      // windows closed
    // counts (both modes): the DRS height in % of the window seen by the monitor inside the window / at its end
    float min_pct_inside;   // lowest height inside the current window (100 when none seen)
    uint32_t drops_inside;  // windows in which the height went below the height at the window's start (mode 2: what the hold would stop)
    float start_pct;        // height when the window opened
} nrd_settle_t;
static inline void nrd_settle_init(nrd_settle_t *s, int mode, uint64_t hold_ns) {
    s->mode = mode; s->hold_ns = hold_ns; s->holding = 0; s->until_ns = 0; s->windows = s->released = 0;
    s->min_pct_inside = 100.0f; s->drops_inside = 0; s->start_pct = 100.0f;
}
// One monitor tick. loading = a load screen is up now (backend draws, or the DRS object's own "off" byte); pct = the DRS height now
// (in % of the window, <0 = unknown). Returns 1 when the target must be held at 100 ms this tick (mode 1 only), 0 otherwise; *event:
// 1 = a window opened, 2 = a window closed, 0 = none.
static inline int nrd_settle_step(nrd_settle_t *s, int loading, uint64_t now_ns, float pct, int *event) {
    int ev = 0;
    if (s->mode == NRD_SETTLE_OFF) { if (event) *event = 0; return 0; }
    if (loading) {
        if (!s->holding) { s->holding = 1; s->windows++; s->start_pct = pct >= 0 ? pct : 100.0f; s->min_pct_inside = s->start_pct; ev = 1; }
        s->until_ns = 0;
    } else if (s->holding) {
        if (!s->until_ns) s->until_ns = now_ns + s->hold_ns;
        if (now_ns >= s->until_ns) {
            s->holding = 0; s->until_ns = 0; s->released++; ev = 2;
            if (s->min_pct_inside < s->start_pct - 0.01f) s->drops_inside++;
        }
    }
    if (s->holding && pct >= 0 && pct < s->min_pct_inside) s->min_pct_inside = pct;
    if (event) *event = ev;
    return s->mode == NRD_SETTLE_APPLY && s->holding;
}

// ---- the GPU clock probe ----
enum { NRD_PROBE_MIN_US = 40, NRD_PROBE_MAX_US = 400, NRD_PROBE_N0 = 4096, NRD_PROBE_NMAX = 1u << 18 };   // review 19.11: was 1 << 24 (one GPU job of 512 threads x 16.7 M dependent FMAs on a weak GPU)
typedef struct {
    uint32_t n;             // loop iterations of the next ALU run
    double base_ns_iter;    // ns / iteration of the first stable run (0 = none yet)
    double last_ns_iter;    // of the last stable run
    uint32_t runs, recal;   // stable runs, calibration steps
} nrd_probe_t;
static inline void nrd_probe_init(nrd_probe_t *p) { p->n = NRD_PROBE_N0; p->base_ns_iter = p->last_ns_iter = 0; p->runs = p->recal = 0; }
// The result of one ALU run of n iterations that took us microseconds. Returns 1 when the run counts (the index is updated), 0 when
// it only re-calibrated (too short to trust the 52-ns stamp tick, or too long to stay cheap): the next run uses p->n.
static inline int nrd_probe_alu(nrd_probe_t *p, uint32_t n, double us) {
    if (!(us > 0) || n == 0) return 0;
    if (n != p->n) return 0;   // a run recorded before the last re-calibration
    if (us < NRD_PROBE_MIN_US && p->n < NRD_PROBE_NMAX) {
        double want = (NRD_PROBE_MIN_US * 2.0) / us * (double)p->n;
        if (want > (double)p->n * 8.0) want = (double)p->n * 8.0;   // review 19.11: at most x8 a step (a timer that reads ~0 must not explode the job)
        p->n = want > (double)NRD_PROBE_NMAX ? (uint32_t)NRD_PROBE_NMAX : (uint32_t)want; p->recal++; p->base_ns_iter = 0; return 0;
    }
    if (us > NRD_PROBE_MAX_US && p->n > 256) {
        double want = (NRD_PROBE_MAX_US / 4.0) / us * (double)p->n;
        p->n = want < 256 ? 256u : (uint32_t)want; p->recal++; p->base_ns_iter = 0; return 0;
    }
    p->last_ns_iter = us * 1000.0 / (double)n;
    if (p->base_ns_iter <= 0) p->base_ns_iter = p->last_ns_iter;
    p->runs++;
    return 1;
}
// 100 = the clock of the first stable run; < 100 = slower now (e.g. 80 = the loop takes 1.25x as long)
static inline double nrd_probe_index(const nrd_probe_t *p) {
    return p->base_ns_iter > 0 && p->last_ns_iter > 0 ? 100.0 * p->base_ns_iter / p->last_ns_iter : -1.0;
}
// GB/s of DRAM traffic of a copy of `bytes` (read + write) that took us microseconds
static inline double nrd_copy_gbs(uint64_t bytes, double us) { return us > 0 ? 2.0 * (double)bytes / (us * 1000.0) : -1.0; }

// ---- DM_DOS2_DRS_SPIKE: one bad frame must not drop the 3D resolution ----
// The game's controller (hle_dos2_quality.c, main+0xc05da8..) reads the GPU ms of a frame at main+0xc05dcc (ldr s16,[x9,#0x18f0]) and steps
// down at once - to the size its model fits, often the floor - when target - gpu < 0.5. A walk into a new zone, the first frame of a
// combat effect or a pipeline build makes one or two such frames, and the resolution then needs ~20 s to climb back. The cave below
// replaces that load (and the three instructions after it; no branch lands inside them) with: GPU ms = the minimum of this sample and the
// one or two before it (the samples the controller read on its previous evaluations). A sustained overload still steps down, 1-2
// evaluations later. Mode bits in the page word NRD_PG_MODE: bit 0 apply, bit 1 count, bit 2 min of 3 (else of 2). Both modes count:
// raw over target (the controller steps down on the raw value) and one-off (the filtered value would not).
// Assembled with the NDK clang (aarch64; the source is in the comments); uses x16, x17, s22..s25 and the flags only (none of them is read
// in the controller, and no flag is live across the site: tests/nr_drift_cave_test.py runs it in Unicorn against nrd_spike_step and
// checks every other register).
enum { NRD_PG_TARGET = 0, NRD_PG_MODE = 3, NRD_PG_H1 = 4, NRD_PG_H2 = 5, NRD_PG_SUPP = 7, NRD_PG_OVER = 8, NRD_PG_WORDS = 9 };
enum { NRD_SPIKE_APPLY = 1, NRD_SPIKE_COUNT = 2, NRD_SPIKE_MIN3 = 4 };
enum { NRD_SPIKE_SITE = 0xc05dcc, NRD_SPIKE_BACK = 0xc05ddc, NRD_SPIKE_CODE_WORDS = 34, NRD_SPIKE_LIT_PAGE = 34, NRD_SPIKE_LIT_BACK = 36,
       NRD_SPIKE_CAVE_WORDS = 38 };
// the four words the cave replaces (main.raw v786432), and the jump it puts there: ldr x16,#8; br x16; .quad cave
static const uint32_t nrd_spike_orig[4] = {0xbd58f130 /* ldr s16,[x9,#0x18f0] */, 0xaa1403e9 /* mov x9,x20 */, 0xbc428d20 /* ldr s0,[x9,#0x28]! */,
                                           0x297c312b /* ldp w11,w12,[x9,#-0x20] */};
static const uint32_t nrd_spike_cave[NRD_SPIKE_CODE_WORDS] = {
    0xbd58f130, /* ldr s16,[x9,#0x18f0]   raw                                  */ 0x58000430, /* ldr x16,lit_page                   */
    0xb9400e11, /* ldr w17,[x16,#12]      mode                                 */ 0x34000331, /* cbz w17,9f (off: raw)              */
    0xbd401217, /* ldr s23,[x16,#16]      h1                                   */ 0x1e377a16, /* fminnm s22,s16,s23                 */
    0x36100071, /* tbz w17,#2,2f                                               */ 0xbd401618, /* ldr s24,[x16,#20]  h2              */
    0x1e387ad6, /* fminnm s22,s22,s24     (min of 3)                           */ 0xbd001617, /* 2: str s23,[x16,#20]  h2 = h1      */
    0xbd001210, /* str s16,[x16,#16]      h1 = raw                             */ 0xbd400217, /* ldr s23,[x16]      target          */
    0x1e2c1019, /* fmov s25,#0.5                                               */ 0x1e303af8, /* fsub s24,s23,s16                   */
    0x1e392300, /* fcmp s24,s25                                                */ 0x54000145, /* b.pl 3f (raw does not step down)   */
    0xb9402211, /* ldr w17,[x16,#32]      over++                               */ 0x11000631, /* add w17,w17,#1                     */
    0xb9002211, /* str w17,[x16,#32]                                           */ 0x1e363af8, /* fsub s24,s23,s22                   */
    0x1e392300, /* fcmp s24,s25                                                */ 0x54000084, /* b.mi 3f (filtered steps down too)  */
    0xb9401e11, /* ldr w17,[x16,#28]      one-off++                            */ 0x11000631, /* add w17,w17,#1                     */
    0xb9001e11, /* str w17,[x16,#28]                                           */ 0xb9400e11, /* 3: ldr w17,[x16,#12]               */
    0x36000051, /* tbz w17,#0,9f          (count only: raw)                    */ 0x1e2042d0, /* fmov s16,s22       apply           */
    0xaa1403e9, /* 9: mov x9,x20          (the 3 moved instructions)           */ 0xbc428d20, /* ldr s0,[x9,#0x28]!                 */
    0x297c312b, /* ldp w11,w12,[x9,#-0x20]                                     */ 0x580000b0, /* ldr x16,lit_back                   */
    0xd61f0200, /* br x16 -> main+0xc05ddc                                     */ 0xd503201f, /* nop (literal alignment)            */
};   // + lit_page (.quad, words 34..35), lit_back (.quad, words 36..37)
// The C model of the cave (bit-exact: the same float operations in the same order). pg = the data page as 32-bit words.
static inline float nrd_spike_f(const uint32_t *pg, int i) { union { uint32_t u; float f; } v; v.u = pg[i]; return v.f; }
static inline void nrd_spike_setf(uint32_t *pg, int i, float f) { union { uint32_t u; float f; } v; v.f = f; pg[i] = v.u; }
static inline float nrd_fminnm(float a, float b) { if (a != a) return b; if (b != b) return a; return a < b ? a : b; }
static inline float nrd_spike_step(uint32_t *pg, float raw) {
    const uint32_t mode = pg[NRD_PG_MODE];
    if (!mode) return raw;
    const uint32_t h1 = pg[NRD_PG_H1];
    float f = nrd_fminnm(raw, nrd_spike_f(pg, NRD_PG_H1));
    if (mode & NRD_SPIKE_MIN3) f = nrd_fminnm(f, nrd_spike_f(pg, NRD_PG_H2));
    pg[NRD_PG_H2] = h1; nrd_spike_setf(pg, NRD_PG_H1, raw);
    const float t = nrd_spike_f(pg, NRD_PG_TARGET);
    if ((t - raw) < 0.5f) { pg[NRD_PG_OVER]++; if (!((t - f) < 0.5f)) pg[NRD_PG_SUPP]++; }
    return (mode & NRD_SPIKE_APPLY) ? f : raw;
}
// page words before the first frame: no history (quiet NaN: fminnm ignores it)
static inline void nrd_spike_init(uint32_t *pg, uint32_t mode) {
    pg[NRD_PG_MODE] = mode; pg[NRD_PG_H1] = pg[NRD_PG_H2] = 0x7fc00000u; pg[NRD_PG_SUPP] = pg[NRD_PG_OVER] = 0;
}

// ---- clock nodes (KB PERF-X09, the D3 perf_android.c gpu_parse rules) ----
// kind 0 a number (Hz / kHz / MHz by size), 1 the last number of the first line (MediaTek GED "<opp> <kHz>"), 2 the number after
// "Freq:" (gpufreq v2 status), 3 the number after "cur_gpu_freq" (gpufreq v1 dump). -> MHz, -1 = none.
enum { NRD_CLK_NUM = 0, NRD_CLK_LAST, NRD_CLK_TAG_FREQ, NRD_CLK_TAG_CUR };
static inline long nrd_clock_mhz(const char *b, int kind) {
    const char *p = b;
    if (kind == NRD_CLK_TAG_FREQ || kind == NRD_CLK_TAG_CUR) {
        const char *tag = kind == NRD_CLK_TAG_FREQ ? "Freq:" : "cur_gpu_freq"; p = 0;
        for (const char *q = b; *q; q++) { const char *a = q, *t = tag; while (*t && *a == *t) { a++; t++; } if (!*t) { p = q; break; } }
        if (!p) return -1;
    }
    long long v = -1;
    for (; *p && *p != '\n'; p++) if (*p >= '0' && *p <= '9') {
        v = 0; while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
        if (kind != NRD_CLK_LAST || !*p || *p == '\n') break;
        p--;
    }
    return v <= 0 ? -1 : v >= 10000000LL ? (long)(v / 1000000) : v >= 10000 ? (long)(v / 1000) : (long)v;
}

#ifdef __cplusplus
}
#endif
#endif
