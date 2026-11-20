// [DOS2] VK-NATIVE stage 3, part "render targets, clears, copies" (dm_vkn_pass_register; core: hle_dos2_vkn.c). Mode 1 (native)
// only: the four engine functions below are REPLACED (the originals record NVN commands and are never called), with every
// engine-visible side effect of the originals kept. main = main.raw of update v786432, build 4979b200; r = [main+0x2bbdc68],
// dev = [r+0xb70], frame counter [dev+0x51f0]; texture table r+0 / +8 / +0x10, records 0x28 ([0] texture object, [1] frame of last use).
// Facts: docs/dos2/nr/VK-NATIVE-MAP-2.md section 4, MAP-3 section 6 (packets 0x03 / 0x04 / 0x05 / 0x16 / 0x34); decompilations
// 9279a0 928ab0 928be0 920b00 927310 (scratchpad vk/re/out).
//   0x9279a0 SetRenderTargets(r, w1 n, x2 u64 colour[], x3 s32 layers[] (NULL / -1 -> 0), x4 s32 levels[] (NULL -> 0), x5 depth):
//            for i < n: r+0x17e8+8i = colour[i]; a non-zero handle's record (generation checked: a stale one would make the
//            original write to address 8) gets [1] = frame. r+0x1828 = depth. Size (w, h) = level-0 size of colour 0 when
//            n && colour && colour[0], else of depth (both looked up by INDEX only, no generation check), else (0, 0): when
//            the viewport cache {w +0x171c, h +0x1720, x +0x1724, y +0x1728} differs from (w, h, 0, 0) it is rewritten (and NVN's
//            viewport AND scissor set to it; the scissor cache r+0x172c stays). Then: n == 0 and no depth -> return; else if
//            (w, h) differs from r+0x1854 / r+0x1858 they are written and 0x927310(r) runs (engine constants of the target
//            size; NVN-free: only stores, two libm imports at 0x1bf4710 / 0x1bf4640, no NVN pointer call - checked in the
//            disassembly); finally r+0x16f8 = 0 (u64), r+0x1700 = -1 (blend cache: the next ApplyBlend rebinds). Quirks kept:
//            n == 0 leaves the colour cache r+0x17e8.. stale; the depth handle is not generation checked.
//   0x928ab0 ClearColorTarget(r, handle, w2 layer (-1 -> 0), x3 float[4]): NVN ClearTexture of (level 0, layer) whole, mask 0xf,
//            independent of the bound targets; no engine field written. Callers pass -1 except packet 0x03 (its +0x10) and
//            0xac2de0 (a layer loop): layers other than 0 are real, so the backend remembers clears per layer.
//   0x928be0 ClearDepthStencil(s0 depth, r, x1 handle, x2 unused, w3 mode 0 depth / 1 stencil / 2 both / else trap, w4 stencil):
//            when handle != r+0x1828 NVN binds (0 colours, that depth) and the viewport cache is set as above, WITHOUT updating
//            r+0x1828 or r+0x17e8..: on NVN the following draws render into that depth-only set until the next SetRenderTargets,
//            and a later ClearDepthStencil naming r+0x1828 clears whatever depth NVN has bound. Here the backend's target set
//            (vkn_pass) is that NVN-bound set, kept apart from the engine cache; the clear goes to vkn_pass_depth().
//   0x920b00 CopyTexture(ctx, x1 dst texture object, x2 src texture object) (objects = vkn_tex cookies): vkn_pass_copy. The call
//            from ReduceTextureMips 0x920460 (return address 0x92076c) is skipped: the texture part's vkn_tex_swap_reduced queues
//            that very level-tail copy.
// Unknown / stale handles (where the original would crash or bind garbage) are dropped, counted and the first 16 of each kind logged.
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <string.h>
#include "../gfx/vkn/vkn.h"
#include "../gfx/vkn/vkn_tex.h"
#include "../gfx/vkn/vkn_pass.h"
#include "hle_dos2_vkn.h"
#include "hle_dos2_vkn_pass.h"

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
static vkn_fn g_o_srt, g_o_ccl, g_o_cds, g_o_cpy;   // (never called: the originals record NVN commands)

enum { C_SRT, C_SRT_ARGS, C_SRT_STALE, C_SRT_UNKNOWN, C_SRT_VIEW, C_SRT_DEPTH, C_SRT_SIZE, C_CCL, C_CCL_BAD, C_CDS, C_CDS_BIND, C_CDS_BAD,
       C_CDS_MODE, C_CDS_NONE, C_RECT, C_RECT_BAD, C_CPY, C_CPY_REDUCE, C_CPY_BAD, C_N };
static uint64_t g_c[C_N];
static uint32_t g_red[C_N];
static int32_t g_vpsc[4]; static uint32_t g_vpsc_serial;

static void wr64(uint64_t p, uint64_t v) { memcpy((void *)(uintptr_t)p, &v, 8); }
static void wr32(uint64_t p, uint32_t v) { memcpy((void *)(uintptr_t)p, &v, 4); }
static int red(int c) { return g_red[c]++ < 16; }

// The record of a texture handle by INDEX only (0x9279a0's depth and size lookups: page = idx / per, slot = idx % per, no
// generation check). 0 = no page.
static uint64_t rec_nogen(uint64_t r, uint64_t h) {
    const uint32_t idx = (uint32_t)h, per = vkn_rd32(r + 0x10);
    const uint64_t pages = vkn_rd64(r);
    if (!per || !pages) return 0;
    const uint64_t page = vkn_rd64(pages + 8ull * (idx / per));
    return page ? page + (uint64_t)(idx % per) * 0x28 : 0;
}
static vkn_tex *tex_of_rec(uint64_t rec) { const uint64_t obj = rec ? vkn_rd64(rec) : 0; return obj ? vkn_tex_by_cookie(obj) : NULL; }

uint32_t dm_vkn_pass_nvn_vpsc(int32_t xywh[4]) { if (xywh) memcpy(xywh, g_vpsc, sizeof g_vpsc); return g_vpsc_serial; }
// 0x9279a0 / 0x928be0: viewport cache = (w, h, 0, 0) when it differs (then NVN SetViewport + SetScissor(0, 0, w, h)).
static void vp_full(uint64_t r, int32_t w, int32_t h) {
    if ((int32_t)vkn_rd32(r + 0x171c) == w && (int32_t)vkn_rd32(r + 0x1720) == h && !vkn_rd32(r + 0x1724) && !vkn_rd32(r + 0x1728)) return;
    wr32(r + 0x171c, (uint32_t)w); wr32(r + 0x1720, (uint32_t)h); wr64(r + 0x1724, 0);
    g_vpsc[0] = 0; g_vpsc[1] = 0; g_vpsc[2] = w; g_vpsc[3] = h;
    if (!++g_vpsc_serial) g_vpsc_serial = 1;
    if (dm_vkn_on_nvn_viewport_scissor) dm_vkn_on_nvn_viewport_scissor(0, 0, w, h);
}

// ---- SetRenderTargets ----
VKN_HOOK vkn_h_srt(VKN_A) {
    VKN_UNUSED;
    const uint64_t r = a0, col = a2, lay = a3, lev = a4, depth = a5;
    uint32_t n = (uint32_t)a1;
    g_c[C_SRT]++;
    if (n && !col) {   // the original would read colour[0] through NULL
        g_c[C_SRT_ARGS]++; if (red(C_SRT_ARGS)) vkn_logf(RED "pass: SetRenderTargets(n %u) without a colour array: dropped", n);
        return 0;
    }
    if (n > VKN_PASS_COLORS) {   // (the original's arrays hold 8)
        g_c[C_SRT_ARGS]++; if (red(C_SRT_ARGS)) vkn_logf(RED "pass: SetRenderTargets with %u colour targets: the first 8 taken", n);
        n = VKN_PASS_COLORS;
    }
    const uint64_t dev = vkn_rd64(r + 0xb70), frame = dev ? vkn_rd64(dev + 0x51f0) : 0;
    vkn_tex *ct[VKN_PASS_COLORS]; uint32_t L[VKN_PASS_COLORS], M[VKN_PASS_COLORS];
    memset(ct, 0, sizeof ct); memset(L, 0, sizeof L); memset(M, 0, sizeof M);
    for (uint32_t i = 0; i < n; i++) {
        const uint64_t h = vkn_rd64(col + 8ull * i);
        wr64(r + 0x17e8 + 8ull * i, h);
        if (!h) continue;   // slot not bound
        const uint64_t rec = vkn_rec(r, 0x000, 0x28, h);
        if (!rec) { g_c[C_SRT_STALE]++; if (red(C_SRT_STALE)) vkn_logf(RED "pass: SetRenderTargets colour %u: stale handle %#llx (the original would crash)", i, (unsigned long long)h); continue; }
        wr64(rec + 8, frame);
        int32_t l = lay ? (int32_t)vkn_rd32(lay + 4ull * i) : -1, m = lev ? (int32_t)vkn_rd32(lev + 4ull * i) : 0;
        if (l == -1) l = 0;
        vkn_tex *t = tex_of_rec(rec);
        if (!t) { g_c[C_SRT_UNKNOWN]++; if (red(C_SRT_UNKNOWN)) vkn_logf(RED "pass: SetRenderTargets colour %u: texture %#llx (object %#llx) unknown to the backend", i, (unsigned long long)h, (unsigned long long)vkn_rd64(rec)); continue; }
        if (l < 0 || (uint32_t)l >= t->layers || m < 0 || (uint32_t)m >= t->mips) {
            g_c[C_SRT_VIEW]++; if (red(C_SRT_VIEW)) vkn_logf(RED "pass: SetRenderTargets colour %u: level %d / layer %d outside %#llx (%u levels, %u layers)", i, m, l, (unsigned long long)h, t->mips, t->layers);
            continue;
        }
        ct[i] = t; L[i] = (uint32_t)l; M[i] = (uint32_t)m;
    }
    wr64(r + 0x1828, depth);
    vkn_tex *dt = NULL;
    if (depth) {
        dt = tex_of_rec(rec_nogen(r, depth));
        if (!dt) { g_c[C_SRT_DEPTH]++; if (red(C_SRT_DEPTH)) vkn_logf(RED "pass: SetRenderTargets depth %#llx unknown to the backend", (unsigned long long)depth); }
    }
    vkn_pass_targets(n, ct, L, M, dt);
    // size, viewport cache, constants, blend cache: the original's tail, in its order
    int32_t w = 0, h = 0; int known = 1;
    const uint64_t c0 = n ? vkn_rd64(col) : 0;
    if (c0 || depth) {
        vkn_tex *st = c0 ? tex_of_rec(rec_nogen(r, c0)) : dt;
        if (st) { w = (int32_t)st->w; h = (int32_t)st->h; vp_full(r, w, h); }
        else { known = 0; g_c[C_SRT_SIZE]++; if (red(C_SRT_SIZE)) vkn_logf(RED "pass: SetRenderTargets: size of %#llx unknown: viewport cache and target size left as they were", (unsigned long long)(c0 ? c0 : depth)); }
    } else if (!n) return 0;   // n == 0 and no depth: the original returns here
    if (known && ((int32_t)vkn_rd32(r + 0x1854) != w || (int32_t)vkn_rd32(r + 0x1858) != h)) {
        wr32(r + 0x1854, (uint32_t)w); wr32(r + 0x1858, (uint32_t)h);
        ((void (*)(uint64_t))(void *)(dm_vkn_base() + 0x927310))(r);
    }
    wr64(r + 0x16f8, 0); wr32(r + 0x1700, 0xffffffffu);
    return 0;
}

// ---- ClearColorTarget ----
VKN_HOOK vkn_h_ccl(VKN_A) {
    VKN_UNUSED;
    g_c[C_CCL]++;
    const uint64_t rec = vkn_rec(a0, 0x000, 0x28, a1);
    vkn_tex *t = tex_of_rec(rec);
    int32_t l = (int32_t)a2; if (l == -1) l = 0;
    if (!t || !a3 || l < 0) {
        g_c[C_CCL_BAD]++;
        if (red(C_CCL_BAD)) vkn_logf(RED "pass: ClearColorTarget %#llx layer %d: %s", (unsigned long long)a1, (int)(int32_t)a2, !rec ? "stale handle (the original would crash)" : !t ? "texture unknown to the backend" : "bad arguments");
        return 0;
    }
    float rgba[4]; memcpy(rgba, (const void *)(uintptr_t)a3, sizeof rgba);
    vkn_pass_clear_color(t, (uint32_t)l, rgba, NULL);
    return 0;
}

// Packet 0x04's region clear (exec part).
void dm_vkn_pass_clear_rect(uint64_t tex_handle, int32_t layer, const float rgba[4], int32_t x, int32_t y, int32_t w, int32_t h) {
    if (vkn_mode() != VKN_NATIVE) return;
    g_c[C_RECT]++;
    const uint64_t r = vkn_renderer();
    vkn_tex *t = r ? tex_of_rec(vkn_rec(r, 0x000, 0x28, tex_handle)) : NULL;
    if (layer == -1) layer = 0;
    if (!t || !rgba || layer < 0) {
        g_c[C_RECT_BAD]++;
        if (red(C_RECT_BAD)) vkn_logf(RED "pass: region clear of %#llx layer %d (%d, %d, %dx%d): %s", (unsigned long long)tex_handle, layer, x, y, w, h, t ? "bad arguments" : "texture unknown");
        return;
    }
    if (w <= 0 || h <= 0) return;   // an empty region clears nothing
    const int32_t rect[4] = {x, y, w, h};
    vkn_pass_clear_color(t, (uint32_t)layer, rgba, rect);
}

// ---- ClearDepthStencil ----
VKN_HOOK vkn_h_cds(VKN_A) {
    VKN_UNUSED;
    g_c[C_CDS]++;
    const uint64_t r = a0, hd = a1; const uint8_t mode = (uint8_t)a3, stencil = (uint8_t)a4;
    uint64_t bits; memcpy(&bits, &d0, 8); const uint32_t lo = (uint32_t)bits; float depth; memcpy(&depth, &lo, 4);   // s0 = low half of v0
    if (vkn_rd64(r + 0x1828) != hd) {   // NVN binds (0 colours, this depth); the engine's caches stay
        vkn_tex *t = tex_of_rec(vkn_rec(r, 0x000, 0x28, hd));
        if (!t) {
            g_c[C_CDS_BAD]++;
            if (red(C_CDS_BAD)) vkn_logf(RED "pass: ClearDepthStencil %#llx (not the bound depth %#llx): texture unknown or stale (the original would crash)", (unsigned long long)hd, (unsigned long long)vkn_rd64(r + 0x1828));
            return 0;
        }
        g_c[C_CDS_BIND]++;
        vkn_tex *none[VKN_PASS_COLORS]; memset(none, 0, sizeof none);
        vkn_pass_targets(0, none, NULL, NULL, t);
        vp_full(r, (int32_t)t->w, (int32_t)t->h);
    }
    int what;
    switch (mode) {
    case 0: what = 1; break;   // depth only (stencil value / mask 0)
    case 1: what = 2; break;   // stencil only (depth 0, mask 0; stencil mask 0xffffffff)
    case 2: what = 3; break;   // both
    default:
        g_c[C_CDS_MODE]++; if (red(C_CDS_MODE)) vkn_logf(RED "pass: ClearDepthStencil mode %u (the original traps)", (unsigned)mode);
        return 0;
    }
    vkn_tex *bd = vkn_pass_depth();   // what NVN would clear: its bound depth target
    if (!bd) { g_c[C_CDS_NONE]++; return 0; }   // (NVN clears nothing)
    vkn_pass_clear_depth(bd, what, depth, stencil);
    return 0;
}

// ---- texture copy ----
VKN_HOOK vkn_h_cpy(VKN_A) {
    VKN_UNUSED;
    g_c[C_CPY]++;
    // The hook is entered by the patched entry's br (lr untouched): the return address is the engine caller's.
    if ((uint64_t)(uintptr_t)__builtin_return_address(0) == (uint64_t)(uintptr_t)(dm_vkn_base() + 0x92076c)) { g_c[C_CPY_REDUCE]++; return 0; }
    vkn_tex *dst = a1 ? vkn_tex_by_cookie(a1) : NULL, *src = a2 ? vkn_tex_by_cookie(a2) : NULL;
    if (!dst || !src) {
        g_c[C_CPY_BAD]++;
        if (red(C_CPY_BAD)) vkn_logf(RED "pass: CopyTexture %#llx -> %#llx: %s unknown to the backend", (unsigned long long)a2, (unsigned long long)a1, !src ? "source" : "destination");
        return 0;
    }
    vkn_pass_copy(dst, src);
    return 0;
}

static void vkn_pass_census(void) {
    vkn_pass_stats s; vkn_pass_stats_get(&s); vkn_pass_stats2 x; vkn_pass_stats2_get(&x);
#define U(v) (unsigned long long)(v)
    vkn_logf("pass: %llu passes (attachments: load %llu [%llu not written this frame], clear %llu, dontcare %llu), clears folded %llu / command %llu / "
             "in pass %llu, region passes %llu, breaks for sampling %llu, feedback %llu, copies %llu (%llu scaled, %llu other formats: none), "
             "transitions %llu, sampled undefined %llu",
             U(s.passes), U(s.load), U(x.load_stale), U(s.clear), U(s.dontcare), U(s.clears_folded), U(s.clears_cmd), U(s.clears_att), U(x.region_pass),
             U(s.breaks_sample), U(s.feedback), U(s.copies), U(x.copy_blit), U(x.copy_skip_fmt), U(s.transitions), U(x.sampled_undefined));
    vkn_logf("pass: calls SetRenderTargets %llu, ClearColor %llu, region clears %llu, ClearDepthStencil %llu (%llu depth-only binds), copies %llu (%llu of mip "
             "reduction skipped); dropped: no recording %llu, no targets %llu, bad target %llu, destroyed target %llu, samples %llu, size mismatch %llu (drawn), "
             "render pass %llu, framebuffer %llu, clears %llu (+%llu lost), copies %llu; engine: bad args %llu, stale %llu, unknown colour %llu, bad view %llu, "
             "unknown depth %llu, unknown size %llu, clear %llu, region %llu, depth clear %llu, mode %llu, no depth bound %llu, copy %llu",
             U(g_c[C_SRT]), U(g_c[C_CCL]), U(g_c[C_RECT]), U(g_c[C_CDS]), U(g_c[C_CDS_BIND]), U(g_c[C_CPY]), U(g_c[C_CPY_REDUCE]), U(x.no_cb), U(x.no_targets),
             U(x.bad_target), U(x.dead_target), U(x.samples_mismatch), U(x.size_mismatch), U(x.rp_fail), U(x.fb_fail), U(x.clear_bad), U(x.clear_lost),
             U(x.copy_drop), U(g_c[C_SRT_ARGS]), U(g_c[C_SRT_STALE]), U(g_c[C_SRT_UNKNOWN]), U(g_c[C_SRT_VIEW]), U(g_c[C_SRT_DEPTH]), U(g_c[C_SRT_SIZE]),
             U(g_c[C_CCL_BAD]), U(g_c[C_RECT_BAD]), U(g_c[C_CDS_BAD]), U(g_c[C_CDS_MODE]), U(g_c[C_CDS_NONE]), U(g_c[C_CPY_BAD]));
#undef U
}

void dm_vkn_pass_register(void);
void dm_vkn_pass_register(void) {
    if (vkn_mode() != VKN_NATIVE) return;   // mode 2 stays exactly as it was: nothing of this part runs
    // First 4 instructions of each site (python <scratchpad>/vk/re/pro.py; none PC-relative, checked with capstone).
    static const uint32_t k_srt[4] = {0xa9ba6ffc /* stp x28,x27,[sp,#-0x60]! */, 0xa90167fa, 0xa9025ff8, 0xa90357f6};
    static const uint32_t k_ccl[4] = {0xd101c3ff /* sub sp,sp,#0x70 */, 0xa90457f6, 0xa9054ff4, 0xa9067bfd};
    static const uint32_t k_cds[4] = {0xfc1b0fe8 /* str d8,[sp,#-0x50]! */, 0xa9015ff8, 0xa90257f6, 0xa9034ff4};
    static const uint32_t k_cpy[4] = {0xd10383ff /* sub sp,sp,#0xe0 */, 0xa9086ffc, 0xa90967fa, 0xa90a5ff8};
    dm_vkn_site(0x9279a0, k_srt, "SetRenderTargets", (void *)vkn_h_srt, &g_o_srt, VKN_M_NATIVE);
    dm_vkn_site(0x928ab0, k_ccl, "ClearColorTarget", (void *)vkn_h_ccl, &g_o_ccl, VKN_M_NATIVE);
    dm_vkn_site(0x928be0, k_cds, "ClearDepthStencil", (void *)vkn_h_cds, &g_o_cds, VKN_M_NATIVE);
    dm_vkn_site(0x920b00, k_cpy, "CopyTexture", (void *)vkn_h_cpy, &g_o_cpy, VKN_M_NATIVE);
    vkn_pass_init();
    dm_vkn_census(vkn_pass_census);
}
#endif
