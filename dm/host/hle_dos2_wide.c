// [DOS2] Widescreen (DM_DOS2_WIDE, default on; docs/dos2/DOS2_PORT.md "Широкий экран"): Divinity: Original Sin 2 renders at the phone's
// aspect with a Hor+ view. The game never asks the system for a display size (of oe:: / vi:: it imports only GetOperationMode /
// GetPerformanceMode / OpenDefaultDisplay / CreateLayer / GetNativeWindow). Its size comes from immediates in the renderer init
// (main = main.raw of update v786432, build 4979b200, offsets = main.raw):
//   0x93845c  mov w8,#1600 / 0x93846c mov w8,#900                       two display modes on the stack, {id 0, 1600, 900} at fp-0x70
//   0x938474  mov x8,#1 / 0x938478 movk x8,#1280,lsl#32 / 0x938480 mov w8,#720   and {id 1, 1280, 720} at fp-0x88 (id = the mode index
//            0x8a7758 picks: (GetOperationMode != 1) | (GetPerformanceMode != 1), i.e. 1 = handheld)
//   0x938b84  ldp w1,w2,[fp,#-0x6c]: the docked size goes into the render settings (+0x14 / +0x18, global main+0x2bb8a60) and into the
//            "Window" object (vtable main+0x2984d40: SetSize vt+0x48 = 0x93d200 -> +0x18 / +0x1c) the swap textures are made from
//            (0x939120 -> 0x93c260 -> TextureBuilderSetSize2D, nvnWindowBuilderSetTextures 0x939268); both modes are registered with it
//            (vt+0x60), and on an operation-mode change 0x8a7788 resizes the device to the picked mode's size (vt+0xf8).
// Nothing else holds the size: the only 1600 / 900 pair in main is the one above (no 1280 / 720 or 1920 / 1080 pair either).
// The 3D follows the window: Viewport::UpdateCameraAspect (0x90fc50) sets camera +0xa0 (aspect) = window width x viewport rel width /
// (window height x rel height); the projection (camera vtable main+0x29949b0, 0xa4b80c) is 1/tan(fov/2) with the FOV vertical and x
// divided by the aspect => a wider window = Hor+ (same vertical view, more at the sides).
// The window can constrain its picture to an aspect (+0xd1 flag, +0xd4 aspect, 0x93e350 -> rect 0x93d320, viewports fitted 0x90fab0):
// the only caller that turns it on is 0xc0b4b0, and only for a window NARROWER than 16:9 (0xc0b548: w / h < 16/9f at main+0x1f123bc)
// while the global byte main+0x2bb8918 is set (it also switches the UI scale 0x110e364): then the game letterboxes itself to 16:9 inside
// the window. Wider windows keep the whole window. UI (Iggy): each UI movie is scaled by height (0x110e32c: window rect height / the
// movie's height) and placed by the engine's anchors (setAnchor / registerAnchorId, the PC code), so the HUD sits at the screen edges;
// videos (Bink) are fitted to their own aspect (0xd1f4e0, centred).
//   DM_DOS2_WIDE unset / 1   auto from the screen (front_screen_px, long side first):
//                             wider than 16:9   docked W x 900, W = 900 x aspect rounded to 16 (max 3200); handheld 720 x aspect, x 720
//                             narrower          docked 1600 x H, H = 1600 / aspect rounded to 16 (max 1600); handheld 1280 x 1280 / aspect
//   DM_DOS2_WIDE=<W>          docked width W (1601..3200) x 900, handheld W x 4/5 x 720
//   DM_DOS2_WIDE=<W>x<H>      docked size (the Java gear passes this: the screen as the activity sees it), handheld both x 4/5
//   DM_DOS2_WIDE=<a>:<b>      as auto, for that aspect (e.g. 4:3)
//   DM_DOS2_WIDE=0            the Switch's 1600x900 / 1280x720 (the front end then bars or stretches it)
//   DM_DOS2_WIDE_TALL=1       narrower windows: the game's own 16:9 letterbox off (0xc0b550 b.mi -> nop): the whole 4:3 window, Hor-
//                             (same vertical view, less at the sides) and the 16:9 UI on it; experiment, default off = the game's letterbox
// Clustered lights (the fix of 14.11: "1080p" window = point lights, glows and the menu fresco highlight gone, 900p fine). The point
// lights go through ClusteredLightingWithSSAO.bshd with a light grid the CPU builds every frame (stage vtable main+0x299b4f0, ctor
// 0xb19820): SetSize vt+0x50 = 0xb19e50 (w, h = [[arg+0x20]+0x20/+0x24], from Init vt+0x18 = 0xb19ad0) makes tiles of 128 px,
// +0xe4/+0xe8 = ceil(w/128) x ceil(h/128), +0xe0 = tiles x 32 depth slices, and a CPU array +0xa8 of 8 bytes per cluster. The GPU
// copy ("ClusterInfoBuffer", +0x90) is a function-static buffer made ONCE with a fixed size: 0xb19d04 mov w1,#0x8700 = 34560 bytes =
// 4320 clusters = 15 x 9 tiles x 32 = exactly 1920x1080 (the Switch's ceiling, DRS max). Each frame the stage's Update (0xb19f20)
// copies the clusters only when clusters x 8 <= that size, otherwise it skips the copy: the buffer stays zero = no light in any
// cluster. 2000x896 -> 16 x 7 = 112 tiles (fits), 2400x1080 (3D 2160x972) -> 17 x 8 = 136 / 19 x 9 = 171 > 135: every point light
// dropped. The grid itself is dynamic (the shader gets the tile counts as floats, 0xb1a988..0xb1a9a4), so only the size is fixed:
// this file raises the immediate to ceil(W/128) x ceil(H/128) x 256 bytes for the docked window (the largest size the game renders),
// rounded up to 256 and at most 64 KB (the NVN / Vulkan uniform range); left alone when 0x8700 already fits.
//   DM_DOS2_CLUSTERS=0        keep the game's 0x8700 (A/B test: lights gone again on a 1080p window)
// All sites are checked word for word: another build changes nothing. The presenter shows the window at its own size (vulkan_present.cpp
// present_native: the buffer geometry follows the window, the compositor scales it uniformly to the full-screen SurfaceView), so a window
// of the screen's shape is shown 1:1. Touch needs nothing: DOS2 on the Switch has no touch input (no GetTouchScreenState import; the
// mouse is initialised but the host reports none), all taps are the front end's pad.
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

enum { DW_MODES = 0x93844c, DW_LBOX = 0xc0b534, DW_CLUSTERS = 0xb19d00 };
// 0xb19d00 ldr x0,[x8,#0xc68] (renderer) / +4 mov w1,#0x8700 (ClusterInfoBuffer size) / +8 bl 0x915170 (make the buffer)
static const uint32_t k_dw_clusters[3] = {0xf9463500, 0x5290e001, 0x97f7ed1a};
static const uint32_t k_dw_modes[16] = {
    0x944af871 /* bl oe::GetPerformanceMode */, 0xaa1303fb, 0xf8418f68, 0xb9000100,
    0x5280c808 /* +0x10 mov w8,#1600 */, 0xb81a03bf, 0xa9397fbf, 0xb81943a8 /* stur w8,[fp,#-0x6c] */,
    0x52807088 /* +0x20 mov w8,#900 */, 0xb81983a8 /* stur w8,[fp,#-0x68] */, 0xd2800028 /* mov x8,#1 */,
    0xf2c0a008 /* +0x2c movk x8,#1280,lsl#32 */, 0xf81783a8 /* stur x8,[fp,#-0x88] */, 0x52805a08 /* +0x34 mov w8,#720 */,
    0xf81843bf, 0xb81803a8 /* stur w8,[fp,#-0x80] */};
static const uint32_t k_dw_lbox[9] = {0x2d430680 /* ldp s0,s1,[x20,#0x18] */, 0xf0009828, 0x7e21d821, 0x7e21d800, 0x1e211801 /* fdiv */,
    0xbd43bd00 /* ldr s0,[16/9f] */, 0x1e202020 /* fcmp */, 0x54000804 /* +0x1c b.mi 0xc0b650: constrain to 16:9 */, 0x1e2e1000};

static int dw_poke(module_t *m, uint32_t off, uint32_t w) {
    uint32_t *at = (uint32_t *)(m->base + off);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF);
    size_t len = (size_t)(((uintptr_t)(at + 1) - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);   // 16 KB = the loader's sealing granule
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    *at = w;
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[wide] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)(at + 1));
    return 0;
}
static unsigned r16(double v) { return (unsigned)(llround(v / 16) * 16); }
static unsigned clampu(unsigned v, unsigned lo, unsigned hi) { return v < lo ? lo : v > hi ? hi : v; }

// The two modes for an aspect a (= width / height): 0 = 16:9 (nothing to change)
// [DOS2] DM_DOS2_RES = 720 (default, owner 14.11: the Switch's DRS range is 495-720p, widen the view instead of raising the
// resolution; handheld = docked) or 900 (the Switch's docked height; handheld 720)
static unsigned dw_base(void) { const char *e = getenv("DM_DOS2_RES"); int v = e ? atoi(e) : 720; return v == 900 || v == 1080 ? (unsigned)v : 720; }
static int dw_from_aspect(double a, unsigned s[4]) {
    if (!(a > 0.5 && a < 8)) return 0;
    const unsigned b = dw_base();
    if (fabs(a - 16.0 / 9.0) < 0.01) { if (b == 900) return 0; s[0] = s[2] = 1280; s[1] = s[3] = 720; return 1; }
    if (b == 720) {
        if (a > 16.0 / 9.0) { s[0] = s[2] = clampu(r16(720 * a), 1296, 2560); s[1] = s[3] = 720; }
        else { s[0] = s[2] = 1280; s[1] = s[3] = clampu(r16(1280 / a), 736, 1280); }
        return 1;
    }
    const unsigned bw = b * 16 / 9;   // 900 -> 1600, 1080 -> 1920; handheld 720
    if (a > 16.0 / 9.0) { s[0] = clampu(r16(b * a), bw + 16, 3200); s[1] = b; s[2] = clampu(r16(720 * a), 1296, 2560); s[3] = 720; }
    else { s[0] = bw; s[1] = clampu(r16(bw / a), b + 16, bw); s[2] = 1280; s[3] = clampu(r16(1280 / a), 736, 1280); }
    return 1;
}

// The clustered-light buffer for the largest window of the two modes (see the header: 0x8700 = 1920x1080's 135 tiles)
static void dw_clusters(module_t *m, const unsigned s[4]) {
    const char *e = getenv("DM_DOS2_CLUSTERS");
    unsigned t0 = ((s[0] + 127) / 128) * ((s[1] + 127) / 128), t1 = ((s[2] + 127) / 128) * ((s[3] + 127) / 128), t = t0 > t1 ? t0 : t1;
    unsigned need = t * 32 * 8;
    if (need <= 0x8700) { LOG("[wide] clustered lights: %u tiles of 128 px fit the game's ClusterInfoBuffer (0x8700 = 135 tiles): unchanged", t); return; }
    if (e && *e == '0' && !e[1]) {
        LOG("【红】[wide] DM_DOS2_CLUSTERS=0: ClusterInfoBuffer stays 0x8700 (135 tiles) for %u tiles: the game skips the light grid upload, "
            "point lights / glows off", t); return; }
    if (m->text_end < DW_CLUSTERS + sizeof k_dw_clusters || memcmp(m->base + DW_CLUSTERS, k_dw_clusters, sizeof k_dw_clusters)) {
        LOG("【红】[wide] main+%#x differs: ClusterInfoBuffer stays 0x8700 (point lights off above 135 tiles)", DW_CLUSTERS); return; }
    unsigned sz = (need + 0xff) & ~0xffu;
    if (sz > 0x10000) {
        LOG("【红】[wide] clustered lights: %u tiles need %#x bytes > 64 KB (the uniform range): buffer 64 KB, the game skips the light "
            "grid upload if its 3D size (0.9 x window) needs more (use a smaller window)", t, sz);
        sz = 0x10000;
    }
    const uint32_t mov = sz == 0x10000 ? 0x52a00021u /* mov w1,#0x10000 */ : 0x52800001u | sz << 5 /* mov w1,#sz */;
    if (dw_poke(m, DW_CLUSTERS + 4, mov)) { LOG("【红】[wide] mprotect RW failed: ClusterInfoBuffer stays 0x8700"); return; }
    LOG("[wide] clustered lights: ClusterInfoBuffer 0x8700 (135 tiles = 1920x1080) -> %#x (%u tiles of 128 px for %ux%u / %ux%u; "
        "main+%#x): the light grid is uploaded again, point lights on", sz, t, s[0], s[1], s[2], s[3], DW_CLUSTERS + 4);
}
// [DOS2] the docked window size this install set (0 = left at the Switch's 1600x900): hle_dos2_touch.c maps touches with it
static unsigned g_dw_w, g_dw_h;
void dm_dos2_wide_size(unsigned *w, unsigned *h);
void dm_dos2_wide_size(unsigned *w, unsigned *h) { *w = g_dw_w; *h = g_dw_h; }

void dm_dos2_wide_install(void);   // main.c host_preload, right after load_modules (before any guest code runs)
void dm_dos2_wide_install(void) {
    const char *e = getenv("DM_DOS2_WIDE");
    if (e && *e == '0' && !e[1]) { LOG("[wide] DM_DOS2_WIDE=0: window 1600x900 (handheld 1280x720), the 16:9 picture as the front end shows it"); return; }
    unsigned s[4] = {0}, w = 0, h = 0, x = 0, y = 0; int ok = 0; const char *how = "the screen";
    if (e && sscanf(e, "%ux%u", &w, &h) == 2) {
        how = "DM_DOS2_WIDE=<W>x<H>";
        {   // [DOS2] the gear computes W x H from its own resolution choice; DM_DOS2_RES (the gear's value, or dm/env, which is read later and wins) decides
            const char *r = getenv("DM_DOS2_RES"); const unsigned b = dw_base();
            if (r && *r && h >= 360 && h != b && b >= 720) {
                LOG("[wide] DM_DOS2_WIDE=%ux%u but DM_DOS2_RES=%u: window rescaled to the height %u (same shape)", w, h, b, b);
                w = (unsigned)((unsigned long long)w * b / h); h = b;
            }
        }
        if (w >= 640 && h >= 360 && w <= 4096 && h <= 4096 && (w != 1600 || h != 900)) {
            s[0] = (w + 4) / 8 * 8; s[1] = (h + 4) / 8 * 8; s[2] = (w * 4 / 5 + 4) / 8 * 8; s[3] = (h * 4 / 5 + 4) / 8 * 8; ok = 1;   // [DOS2] multiples of 8: 2400x1080 exact (16 made it 1088, 14.11)
            if (h <= 720 && w <= 2560) { s[2] = s[0]; s[3] = s[1]; }   // [DOS2] 720p (the default): handheld = docked
            s[0] = clampu(s[0], 640, 3200); s[1] = clampu(s[1], 368, 2400); s[2] = clampu(s[2], 512, 2560); s[3] = clampu(s[3], 288, 1920);
        }
    } else if (e && sscanf(e, "%u:%u", &x, &y) == 2 && x && y) { how = "DM_DOS2_WIDE=<a>:<b>"; ok = dw_from_aspect((double)x / y, s); }
    else if (e && atol(e) > 1600) {
        how = "DM_DOS2_WIDE=<W>"; w = clampu((unsigned)atol(e), 1616, 3200);
        s[0] = (w + 8) / 16 * 16; s[1] = 900; s[2] = (s[0] * 4 / 5 + 8) / 16 * 16; s[3] = 720; ok = 1;
    } else {
        uint64_t front_screen_px(void); uint64_t r = front_screen_px(); w = (uint32_t)r; h = (uint32_t)(r >> 32);
        if (w && h) ok = dw_from_aspect((double)w / h, s);
        else { LOG("[wide] DM_DOS2_WIDE=%s: the screen size is not known yet: window stays 1600x900", e ? e : "(default)"); return; }
    }
    if (!ok) { LOG("[wide] DM_DOS2_WIDE=%s (%s %ux%u): 16:9 or not usable: window stays 1600x900", e ? e : "(default)", how, w, h); return; }
    module_t *m = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) m = &g_mod[i];
    if (!m) { LOG("【红】[wide] no main module: window stays 1600x900"); return; }
    if (m->text_end < DW_LBOX + sizeof k_dw_lbox || memcmp(m->base + DW_MODES, k_dw_modes, sizeof k_dw_modes)) {
        LOG("【红】[wide] main+%#x differs: not DOS2 update v786432 main (build 4979b200): window stays 1600x900", DW_MODES); return; }
    const uint32_t dk_w = 0x52800008u | s[0] << 5 /* mov w8,#W */, dk_h = 0x52800008u | s[1] << 5 /* mov w8,#H */,
                   hh_w = 0xf2c00008u | s[2] << 5 /* movk x8,#Wh,lsl#32 */, hh_h = 0x52800008u | s[3] << 5 /* mov w8,#Hh */;
    if (dw_poke(m, DW_MODES + 0x10, dk_w) || dw_poke(m, DW_MODES + 0x20, dk_h) || dw_poke(m, DW_MODES + 0x2c, hh_w) || dw_poke(m, DW_MODES + 0x34, hh_h)) {
        LOG("【红】[wide] mprotect RW failed part-way: the game may mix sizes"); return; }
    g_dw_w = s[0]; g_dw_h = s[1];
    LOG("[wide] DOS2 window 1600x900 -> %ux%u, handheld 1280x720 -> %ux%u (%s %ux%u; display modes main+%#x); aspect %.4f: cameras take the "
        "window's aspect (Hor+, vertical FOV kept), UI scaled by height and anchored to the edges", s[0], s[1], s[2], s[3], how, w, h, DW_MODES, (double)s[0] / s[1]);
    dw_clusters(m, s);
    if (s[0] * 9 < s[1] * 16) {   // narrower than 16:9: the game's own 16:9 letterbox (0xc0b4b0) unless DM_DOS2_WIDE_TALL=1
        const char *t = getenv("DM_DOS2_WIDE_TALL"); int full = t && *t == '1';
        if (!full) LOG("[wide] narrower than 16:9: the game letterboxes the 3D and the UI to 16:9 inside the window (main+0xc0b4b0, as on PC); "
                       "DM_DOS2_WIDE_TALL=1 uses the whole window");
        else if (memcmp(m->base + DW_LBOX, k_dw_lbox, sizeof k_dw_lbox)) LOG("【红】[wide] main+%#x differs: the game's 16:9 letterbox stays", DW_LBOX);
        else if (dw_poke(m, DW_LBOX + 0x1c, 0xd503201f /* nop */)) LOG("【红】[wide] mprotect RW failed: the game's 16:9 letterbox stays");
        else LOG("[wide] DM_DOS2_WIDE_TALL=1: the game's 16:9 letterbox off (main+%#x b.mi -> nop): the whole window, Hor- (vertical FOV kept)", DW_LBOX + 0x1c);
    }
}
#endif
