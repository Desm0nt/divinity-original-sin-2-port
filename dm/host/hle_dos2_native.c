// [DOS2] Native render path, first hooks (docs/dos2/NATIVE_PLAN.md 1.5 / 3.5). main = main.raw of update v786432, build 4979b200.
// The engine's render thread runs its own command lists through ExecuteCommandList (0xa49c30) -> ExecuteRenderCommand (0x93e5f0:
// x0 = list, w1 = opcode byte, x2 = packet; returns the packet length). The bind opcodes only write the renderer's state cache; the NVN
// calls are made at a draw: 0x928e80 (opcode 36, DrawElements) and 0x928fb0 (opcode 37, DrawElementsInstanced) flush the dirty uniform
// blocks (0x926f90 + 0x925740 -> BindUniformBuffer) and the dirty texture slots (0x924730 -> GetTextureHandle + BindTexture), then draw.
//   DM_NATIVE_RC=2     counting hook on ExecuteRenderCommand (the original runs): every 10 s the calls per opcode, the returned packet
//                      length against the table from the static analysis (k_len; 0 = variable, then min..max is reported), the threads
//                      that execute lists. 0 / unset = no hook.
//   DM_NATIVE_DRAW=1   the records of one engine draw (the binds its flush emits + the draw) go into the command stream as ONE host
//                      record OP_X_NATIVE_DRAW (hle_nvn.c nvn_native_draw_begin / _end; replayed in order by the translation thread).
//   DM_NATIVE_DRAW=2   check: the NVN path as it is + before each draw a record with the texture handles the renderer's state cache
//                      holds (renderer +0x1558 / +0x13c0 vertex, +0x15e0 / +0x1448 fragment: texture id / sampler id per slot, -1 = none);
//                      the translation thread compares them with what the NVN binds left (xbg_game.cpp OP_X_NATIVE_CHECK).
//   DM_NATIVE_DRAW=0 / unset: no hook (default until measured on the phone).
//   DM_GFX_PASS_GRAPH=1 the same ExecuteRenderCommand hook (with or without DM_NATIVE_RC) records, before the original runs, a marker
//                      OP_X_ENGINE_MARK {opcode, low 32 bits of the list} into the recording command buffer ([dev+0x5248]+0x38, dev =
//                      [[list+0x38]+0xb70]) for the opcodes that can end a Vulkan pass (k_mark); xbg_game.cpp's pass graph names each
//                      pass break by them and each pass by the engine command list that drew it (lists live in the pass objects, +0x18).
// Every site is checked word for word; another build changes nothing. The hooks are a 16-byte jump over the prologue (4 instructions,
// none PC-relative) and a trampoline with those 4 instructions + a jump to the rest, as hle_tl2_wide.c does (dn_hook, shared).
// ExecuteRenderCommand has ONE hook for everyone: DM_NATIVE_RC, DM_GFX_PASS_GRAPH and DM_NB (hle_dos2_nb.c) register handlers with
// dn_install_rc; the hook calls each one's pre before the original and post after it (docs/dos2/NATIVE_BACKEND_PLAN.md NB-03).
// The two draw functions have ONE hook each too: DM_NATIVE_DRAW (here) and DM_NB_OPS bits 36 / 37 (hle_dos2_nb.c, NB-09, dn_install_draw)
// share it; the hook opens the DM_NATIVE_DRAW bracket first, then the registered handler's pre, the original, its post, the DM_NATIVE_DRAW
// end (an OP_X_NB_DRAW record of NB-09 thus lands inside the OP_X_NATIVE_DRAW of the same draw when both are on).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum { DN_RC = 0x93e5f0, DN_DRAW = 0x928e80, DN_DRAWI = 0x928fb0 };
static const uint32_t k_dn_rc[4] = {0xd10503ff /* sub sp,sp,#0x140 */, 0xfd006be8 /* str d8,[sp,#0xd0] */, 0xa90e6ffc, 0xa90f67fa};
static const uint32_t k_dn_draw[4] = {0xa9bc5ff8 /* stp x24,x23,[sp,#-0x40]! */, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};   // both draw functions
// Packet length per opcode (x0 of each handler, NATIVE_PLAN.md 1.5); 0 = variable or branching
static const uint8_t k_len[64] = {
    0,    0,    0x20, 0x30, 0x40, 0x20, 0x18, 0x10, 0x18, 0x10, 0x10, 0x18, 0x10, 2,    0x18, 0x20,
    0x18, 0x18, 0x14, 0x14, 0x14, 0x70, 0,    0x98, 0x4c, 0x10, 0x10, 1,    0,    0x20, 0x20, 0x10,
    0x10, 1,    1,    0xc,  0x14, 0x1c, 0x50, 0x90, 0x40, 0x40, 0xa0, 0x10, 0,    0x18, 0,    0x18,
    0x10, 0,    0x10, 0x18, 0x18, 0x28, 0x14, 0,    0xc,  2,    2,    0xc,  0x18, 0x10, 0x10, 0x10};

// Opcodes marked for DM_GFX_PASS_GRAPH: 1 compute resources, 3/4 clear, 5 depth clear / RT, 21/22 SetRenderTargets, 35 UI draw, 40/42 full-state
// draws, 43 compute, 44/46/47/56/59 UI block, 52 RT copy, 55 barrier, 61 counter
static const uint64_t k_mark = 1ull << 1 | 1ull << 3 | 1ull << 4 | 1ull << 5 | 1ull << 21 | 1ull << 22 | 1ull << 35 | 1ull << 40 | 1ull << 42 | 1ull << 43 |
                               1ull << 44 | 1ull << 46 | 1ull << 47 | 1ull << 52 | 1ull << 55 | 1ull << 56 | 1ull << 59 | 1ull << 61;
static int g_rc_count, g_rc_mark;
// Shared ExecuteRenderCommand hook (hle_dos2_nb.c registers too): pre runs before the original handler, post after it with the packet length
// it returned; op = the opcode byte. Declared again in hle_dos2_nb.c.
typedef struct dn_rc_handler { void (*pre)(uint64_t list, unsigned op, uint64_t pkt); void (*post)(uint64_t list, unsigned op, uint64_t pkt, uint64_t len); } dn_rc_handler;
void nvn_engine_mark(void *cb, uint32_t op, uint32_t arg);   // hle_nvn.c
typedef uint64_t (*dn_fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static dn_fn g_rc_orig, g_draw_orig, g_drawi_orig;
static int g_dn_draw_mode;
// Shared draw hook (NB-09): op = the engine opcode the function serves (36 DrawElements 0x928e80, 37 DrawElementsInstanced 0x928fb0).
// Declared again in hle_dos2_nb.c.
typedef struct dn_draw_handler { void (*pre)(uint64_t renderer, unsigned op); void (*post)(uint64_t renderer, unsigned op); } dn_draw_handler;
static const dn_draw_handler *g_draw_h;
void nvn_native_draw_begin(const void *renderer, int mode);   // hle_nvn.c
void nvn_native_draw_end(void);
void nvn_native_draw_stats(uint64_t *draws, uint64_t *recs, uint64_t *bytes, uint64_t *split);

static uint64_t dn_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

// ---- DM_NATIVE_RC=2 ----
// Lists are expected to run on one thread; the counters are plain (a second thread only makes the numbers approximate, and is reported).
static struct { uint64_t n[64], bad[64], lmin[64], lmax[64], t0, calls; int tid[8], ntid; } g_rc;
static void rc_report(uint64_t now) {
    char line[1800]; int k = 0; uint64_t bad = 0;
    for (int i = 0; i < 64 && k < (int)sizeof line - 64; i++) if (g_rc.n[i]) {
        if (k_len[i]) k += snprintf(line + k, sizeof line - (size_t)k, " %d:%llu", i, (unsigned long long)g_rc.n[i]);
        else k += snprintf(line + k, sizeof line - (size_t)k, " %d:%llu(len %llu..%llu)", i, (unsigned long long)g_rc.n[i], (unsigned long long)g_rc.lmin[i], (unsigned long long)g_rc.lmax[i]);
        bad += g_rc.bad[i];
    }
    line[k] = 0;
    char tids[96]; int t = 0; tids[0] = 0;
    for (int i = 0; i < g_rc.ntid; i++) t += snprintf(tids + t, sizeof tids - (size_t)t, "%s%d", i ? "," : "", g_rc.tid[i]);
    LOG("[native] render commands (last %.1f s): %llu packets on %d thread(s) [%s]; length differs from the table in %llu; per opcode:%s",
        (now - g_rc.t0) / 1e9, (unsigned long long)g_rc.calls, g_rc.ntid, tids, (unsigned long long)bad, line);
    for (int i = 0; i < 64; i++) if (g_rc.bad[i]) LOG("【红】[native] opcode %d: %llu packets returned a length other than %#x (last seen %#llx..%#llx)",
        i, (unsigned long long)g_rc.bad[i], k_len[i], (unsigned long long)g_rc.lmin[i], (unsigned long long)g_rc.lmax[i]);
    memset(g_rc.n, 0, sizeof g_rc.n); memset(g_rc.bad, 0, sizeof g_rc.bad); memset(g_rc.lmin, 0, sizeof g_rc.lmin); memset(g_rc.lmax, 0, sizeof g_rc.lmax);
    g_rc.calls = 0; g_rc.t0 = now;
}
static void dn_count_pre(uint64_t list, unsigned c, uint64_t pkt) {   // the threads that execute lists
    (void)list; (void)c; (void)pkt;
    static __thread int seen;
    if (!seen) { seen = 1; int me = (int)gettid(), n = __atomic_load_n(&g_rc.ntid, __ATOMIC_RELAXED), have = 0;
        for (int i = 0; i < n && i < 8; i++) if (g_rc.tid[i] == me) have = 1;
        if (!have && n < 8) { g_rc.tid[n] = me; __atomic_store_n(&g_rc.ntid, n + 1, __ATOMIC_RELAXED); } }
}
static void dn_count_post(uint64_t list, unsigned c, uint64_t pkt, uint64_t len) {
    (void)list; (void)pkt;
    if (c < 64) {
        g_rc.n[c]++; g_rc.calls++;
        if (!g_rc.lmin[c] || len < g_rc.lmin[c]) g_rc.lmin[c] = len;
        if (len > g_rc.lmax[c]) g_rc.lmax[c] = len;
        if (k_len[c] && len != k_len[c]) g_rc.bad[c]++;
        if (!(g_rc.calls & 1023)) { uint64_t now = dn_now(); if (!g_rc.t0) g_rc.t0 = now; else if (now - g_rc.t0 >= 10000000000ull) rc_report(now); }
    }
}
static void dn_mark_pre(uint64_t list, unsigned c, uint64_t pkt) {   // DM_GFX_PASS_GRAPH: the marker goes before the opcode's NVN calls; opcode 64 = first packet of another list
    (void)pkt;
    if (!list) return;
    static __thread uint64_t last_list;
    const int m = c < 64 && (k_mark >> c & 1);
    if (m || list != last_list) {
        const uint8_t *r = *(const uint8_t *const *)(uintptr_t)(list + 0x38);
        const uint8_t *dev = r ? *(const uint8_t *const *)(r + 0xb70) : NULL;
        uint8_t *ctx = dev ? *(uint8_t *const *)(dev + 0x5248) : NULL;
        if (ctx) nvn_engine_mark(ctx + 0x38, m ? c : 64, (uint32_t)list);
        last_list = list;
    }
}
static const dn_rc_handler k_count = {dn_count_pre, dn_count_post}, k_mark_h = {dn_mark_pre, NULL};

// ---- the shared ExecuteRenderCommand hook: one jump at 0x93e5f0, the registered handlers around the original ----
// Handlers run in registration order (pre before the original, post after it), all on the thread that executes the list. Registration
// happens at install time only (before any guest code runs), so the list is read without locks.
enum { DN_RC_MAX = 4 };
static const dn_rc_handler *g_rc_h[DN_RC_MAX];
static int g_rc_nh;
static uint64_t dn_rc_hook(uint64_t list, uint64_t op, uint64_t pkt, uint64_t a3, uint64_t a4, uint64_t a5) {
    const unsigned c = (unsigned)op & 0xff; const int n = g_rc_nh;
    for (int i = 0; i < n; i++) if (g_rc_h[i]->pre) g_rc_h[i]->pre(list, c, pkt);
    uint64_t len = g_rc_orig(list, op, pkt, a3, a4, a5);
    for (int i = 0; i < n; i++) if (g_rc_h[i]->post) g_rc_h[i]->post(list, c, pkt, len);
    return len;
}

// ---- DM_NATIVE_DRAW=1 / 2 ----
static void dn_draw_report(void) {
    static uint64_t t0, n; if (++n & 255) return;
    uint64_t now = dn_now(); if (!t0) { t0 = now; return; } if (now - t0 < 10000000000ull) return;
    uint64_t d, r, b, s; nvn_native_draw_stats(&d, &r, &b, &s);
    LOG("[native] draws (DM_NATIVE_DRAW=%d, last %.1f s): %llu engine draws, %.2f host records and %.0f bytes a draw%s; %llu draw records split (buffer full / second command buffer)",
        g_dn_draw_mode, (now - t0) / 1e9, (unsigned long long)d, d ? (double)r / d : 0., d ? (double)b / d : 0.,
        g_dn_draw_mode == 1 ? ", one OP_X_NATIVE_DRAW each" : "", (unsigned long long)s);
    t0 = now;
}
static uint64_t dn_draw_common(dn_fn orig, unsigned op, uint64_t r, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    const int m = g_dn_draw_mode; const dn_draw_handler *h = g_draw_h;
    if (m) nvn_native_draw_begin((const void *)(uintptr_t)r, m);
    if (h) h->pre(r, op);
    uint64_t v = orig(r, a1, a2, a3, a4, a5);
    if (h) h->post(r, op);
    if (m) { nvn_native_draw_end(); dn_draw_report(); }
    return v;
}
static uint64_t dn_draw_hook(uint64_t r, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { return dn_draw_common(g_draw_orig, 36, r, a1, a2, a3, a4, a5); }
static uint64_t dn_drawi_hook(uint64_t r, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) { return dn_draw_common(g_drawi_orig, 37, r, a1, a2, a3, a4, a5); }
int dn_hook(module_t *m, uint32_t off, const uint32_t pro[4], void *hook, void **orig);   // below
// Both draw hooks in (once; the prologue words are checked by the caller). 0 = both in, -1 = none, -2 = DrawElements only.
static int dn_draw_hooks(module_t *m) {
    if (g_draw_orig && g_drawi_orig) return 0;
    if (g_draw_orig || g_drawi_orig) return -2;
    void *o1 = NULL, *o2 = NULL;
    if (dn_hook(m, DN_DRAW, k_dn_draw, (void *)dn_draw_hook, &o1)) return -1;
    g_draw_orig = (dn_fn)o1;
    if (dn_hook(m, DN_DRAWI, k_dn_draw, (void *)dn_drawi_hook, &o2)) return -2;
    g_drawi_orig = (dn_fn)o2;
    return 0;
}

static int dn_poke(module_t *m, uint32_t off, const uint32_t *w, int n) {
    uint32_t *at = (uint32_t *)(m->base + off);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF);
    size_t len = (size_t)(((uintptr_t)(at + n) - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);   // 16 KB = the loader's sealing granule
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    memcpy(at, w, 4 * (size_t)n);
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[native] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)(at + n));
    return 0;
}
static int dn_jump(module_t *m, uint32_t off, void *to) {   // ldr x16, #8; br x16; .quad to
    uint32_t w[4] = {0x58000050, 0xd61f0200, (uint32_t)(uintptr_t)to, (uint32_t)((uint64_t)(uintptr_t)to >> 32)};
    return dn_poke(m, off, w, 4);
}

// ---- shared with hle_dos2_nb.c ----
module_t *dn_main(void) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) return &g_mod[i];
    return NULL;
}
int dn_words_ok(module_t *m, uint32_t off, const uint32_t pro[4]) { return m && m->text_end >= off + 16 && !memcmp(m->base + off, pro, 16); }
// The 4 prologue words (checked by the caller with dn_words_ok, none PC-relative) go to a trampoline + a jump to off + 16; *orig = the
// trampoline; then off gets the 16-byte jump to hook. Install time only (before any guest code runs). 0 = in, -1 = nothing changed.
int dn_hook(module_t *m, uint32_t off, const uint32_t pro[4], void *hook, void **orig) {
    uint32_t *t = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) return -1;
    const uint64_t rest = (uint64_t)(uintptr_t)(m->base + off + 16);
    memcpy(t, pro, 16); t[4] = 0x58000050 /* ldr x16,#8 */; t[5] = 0xd61f0200 /* br x16 */; t[6] = (uint32_t)rest; t[7] = (uint32_t)(rest >> 32);
    if (mprotect(t, 0x4000, PROT_READ | PROT_EXEC)) { munmap(t, 0x4000); return -1; }
    __builtin___clear_cache((char *)t, (char *)(t + 8));
    *orig = (void *)t;
    if (dn_jump(m, off, hook)) { *orig = NULL; munmap(t, 0x4000); return -1; }
    return 0;
}
// Adds a handler to the one ExecuteRenderCommand hook (installs the hook on the first call). 0 = registered.
int dn_install_rc(const dn_rc_handler *h) {
    if (g_rc_nh >= DN_RC_MAX) { LOG("【红】[native] ExecuteRenderCommand: more than %d handlers", DN_RC_MAX); return -1; }
    if (!g_rc_orig) {
        module_t *m = dn_main();
        if (!m) { LOG("【红】[native] no main module: no ExecuteRenderCommand hook"); return -1; }
        if (!dn_words_ok(m, DN_RC, k_dn_rc)) { LOG("【红】[native] main+%#x differs: not DOS2 update v786432 main (build 4979b200): no ExecuteRenderCommand hook", DN_RC); return -1; }
        void *o = NULL;
        if (dn_hook(m, DN_RC, k_dn_rc, (void *)dn_rc_hook, &o)) { LOG("【红】[native] ExecuteRenderCommand main+%#x: mmap / mprotect failed, no hook", DN_RC); return -1; }
        g_rc_orig = (dn_fn)o;
        LOG("[native] ExecuteRenderCommand main+%#x hooked (one jump, a chain of handlers around the original)", DN_RC);
    }
    g_rc_h[g_rc_nh++] = h;
    return 0;
}

// NB-09: one handler around both engine draw functions (installs the hooks if DM_NATIVE_DRAW did not). Install time only. 0 = registered.
int dn_install_draw(const dn_draw_handler *h);
int dn_install_draw(const dn_draw_handler *h) {
    if (g_draw_h) { LOG("【红】[native] engine draw: a second handler"); return -1; }
    module_t *m = dn_main();
    if (!m || !dn_words_ok(m, DN_DRAW, k_dn_draw) || !dn_words_ok(m, DN_DRAWI, k_dn_draw)) {
        LOG("【红】[native] main+%#x / +%#x differ: not DOS2 update v786432 main (build 4979b200): no draw hooks", DN_DRAW, DN_DRAWI); return -1; }
    const int rc = dn_draw_hooks(m);
    if (rc == -1) { LOG("【红】[native] mmap / mprotect failed: no draw hooks"); return -1; }
    if (rc == -2) { LOG("【红】[native] the draw hooks are half in (DrawElements only): no draw handler"); return -1; }
    g_draw_h = h;
    return 0;
}

void dm_dos2_native_install(void);   // main.c host_preload, after load_modules (before any guest code runs)
void dm_dos2_native_install(void) {
    const char *e = getenv("DM_NATIVE_RC"), *d = getenv("DM_NATIVE_DRAW"), *g = getenv("DM_GFX_PASS_GRAPH");
    g_rc_count = e && atoi(e) == 2; g_rc_mark = g && atoi(g) > 0;
    int rc = g_rc_count || g_rc_mark, dr = d ? atoi(d) : 0;
    if (dr < 0 || dr > 2) dr = 0;
    if (!rc && !dr) return;
    module_t *m = dn_main();
    if (!m) { LOG("【红】[native] no main module: no hooks"); return; }
    if (!dn_words_ok(m, DN_RC, k_dn_rc) || !dn_words_ok(m, DN_DRAW, k_dn_draw) || !dn_words_ok(m, DN_DRAWI, k_dn_draw)) {
        LOG("【红】[native] main+%#x / +%#x / +%#x differ: not DOS2 update v786432 main (build 4979b200): no hooks", DN_RC, DN_DRAW, DN_DRAWI); return; }
    if (rc) {
        int bad = 0;
        if (g_rc_count) bad |= dn_install_rc(&k_count);   // before the marker: the thread is noted first, as the single hook did
        if (g_rc_mark) bad |= dn_install_rc(&k_mark_h);
        if (bad) LOG("【红】[native] no render command census / marks (see above)");
        else LOG("[native] ExecuteRenderCommand main+%#x:%s%s, the original runs", DN_RC,
                 g_rc_count ? " DM_NATIVE_RC=2 counted (opcodes, packet lengths against the table, threads)" : "",
                 g_rc_mark ? " DM_GFX_PASS_GRAPH marks the pass-ending opcodes in the command stream" : "");
    }
    if (dr) {
        g_dn_draw_mode = dr;
        const int hk = dn_draw_hooks(m);
        if (hk == -1) LOG("【红】[native] mmap / mprotect failed: no draw hooks");
        else {
            if (hk == -2) LOG("【红】[native] mprotect failed: the draw hooks are half in (DrawElements only)");
            else {
                LOG("[native] DM_NATIVE_DRAW=%d: engine draws main+%#x / +%#x %s", dr, DN_DRAW, DN_DRAWI,
                    dr == 1 ? "recorded as one OP_X_NATIVE_DRAW each (the flushed binds + the draw)" : "on the NVN path, the renderer's texture cache checked against the bound handles before each draw"); }
        }
    }
}
#endif
