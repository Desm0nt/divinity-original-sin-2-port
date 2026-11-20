// [DOS2] NR-20 DM_NR_ROTATE (docs/dos2/nr/NR-20.md): swapchain pre-rotation. Pure C (no Vulkan, no Android): the decisions and the
// geometry the presenter (android/vulkan_present.cpp, namespace nrr) uses, so tests/nr_rotate_test.c runs the same code.
//
// Why: the phone's surface reports currentTransform ROTATE_90 / ROTATE_270 (2 / 8: a landscape window on a portrait panel) and the
// swapchain was made with preTransform IDENTITY, so Android's compositor turns every frame. Whether that costs anything depends on the
// device: a display controller that rotates the layer itself ("DEVICE" composition) does it for free, otherwise SurfaceFlinger composes
// the layer on the GPU ("CLIENT"): a full-screen read + write every refresh, and on some devices a new frame only every second vsync
// (KB NXH-A02: nx2android, Retroid Pocket 5, 30 -> 60 fps on screen). An app cannot ask which one it gets (`adb shell dumpsys
// SurfaceFlinger`, layer table, column "Comp Type"), so the switch defaults to 0 and the policy is filled from that check per device.
//
// Rules:
//  * turn: the surface's currentTransform when it is one of ROTATE_90 / 180 / 270 and in supportedTransforms (mirror transforms and
//    INHERIT: no); the swapchain then takes preTransform = turn and, for 90 / 270, the image extent swapped (the panel's orientation);
//  * the picture: the frame is drawn into the swapchain image by one draw that samples the window texture turned (the blit cannot
//    turn); letterbox bars, crop and the TL2 pillarbox shapes are the old blit's, mapped into the image;
//  * NR-11 (DM_NR_PRESENT): a pre-rotated swapchain is refused there (reason "pre-rotated"): the game's combine pass is a translated
//    shader that cannot draw turned, so the rotating draw is the price of pre-rotation and NR-11 then saves nothing. Which of the two a
//    device wants is the dumpsys answer: DEVICE -> NR-11 (no blit, the display turns for free), CLIENT -> NR-20;
//  * a turn of the phone (ROTATE_90 <-> ROTATE_270, currentTransform 2 <-> 8, the window keeps its size): the next present sees the
//    surface's transform differ from the swapchain's (SUBOPTIMAL, or the poll every NRR_POLL presents) and makes a new swapchain;
//  * a failed rotated swapchain (creation, views, framebuffers, pipeline): an identity swapchain at once, rotation off until restart;
//  * RCAS (DM_GFX_SHARPEN, the gear's sharpening; docs/dos2/nr/NR-20-rcas.md): the turned draw itself sharpens (shaders/
//    nr_rotate_rcas.frag = rcas.comp's filter evaluated at the source texel the image pixel shows, nrr_rcas_map), so a sharpened frame is
//    still one pass (the old path: RCAS compute into its own image + blit). Only when that pipeline cannot be made: identity + RCAS +
//    blit as before (reason NRR_R_SHARPEN).
//
// Transform semantics (Vulkan spec, VkSurfaceTransformFlagBitsKHR): preTransform is the transform the application applied to the content;
// ROTATE_90 = the image content is rotated 90 degrees clockwise. A window point (u, v) of a W x H window lands in the image at
//   ROTATE_90 (H - v, u)   ROTATE_180 (W - u, H - v)   ROTATE_270 (v, W - u)
// so the image point (x, y) shows window point ROTATE_90 (y, H - x), ROTATE_180 (W - x, H - y), ROTATE_270 (W - y, x); normalized, the
// image point (a, b) shows (b, 1 - a) / (1 - a, 1 - b) / (1 - b, a) (nx2android rot.vert: the same for 90 / 270, confirmed on screen).
#pragma once
#include <stdint.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NRR_MODE_OFF = 0, NRR_MODE_ON = 1, NRR_MODE_CHECK = 2, NRR_MODE_POISON = 3 };
static inline int nrr_clamp_mode(long v) { return v >= 0 && v <= 3 ? (int)v : 0; }
// The policy default (DM_NR_ROTATE unset). DOS2: 1 on every GPU family (owner 17.11): on the Mi 14 Ultra the compositor turned the game's
// layer on the GPU (CLIENT, ROT_90); pre-rotated (RCAS in the turned draw) it became DEVICE / transform 0 and fps rose 84.6 -> 95.1
// (nr5_rot1 vs nr5_base). An unturned surface does nothing; a rotated swapchain that cannot be made falls back to identity + blit.
// Other games keep 0 until measured.
#ifdef DM_GAME_DOS2
static inline int nrr_default_mode(void) { return NRR_MODE_ON; }
#else
static inline int nrr_default_mode(void) { return NRR_MODE_OFF; }
#endif

// Surface transform bits (the values of VkSurfaceTransformFlagBitsKHR).
enum { NRR_T_IDENTITY = 1, NRR_T_ROT90 = 2, NRR_T_ROT180 = 4, NRR_T_ROT270 = 8, NRR_T_MIRROR_ANY = 0x1f0 };
static inline const char *nrr_transform_name(uint32_t t) {
    switch (t) { case NRR_T_IDENTITY: return "identity"; case NRR_T_ROT90: return "rotate 90"; case NRR_T_ROT180: return "rotate 180";
                 case NRR_T_ROT270: return "rotate 270"; default: return t & NRR_T_MIRROR_ANY ? "mirror / inherit" : "?"; }
}
static inline int nrr_swaps_axes(uint32_t turn) { return turn == NRR_T_ROT90 || turn == NRR_T_ROT270; }

// Why a swapchain is not pre-rotated (the first that holds).
enum {
    NRR_R_OK = 0,      // pre-rotated (mode 2: would be)
    NRR_R_OFF,         // DM_NR_ROTATE 0 / unset
    NRR_R_IDENTITY,    // the surface is not turned (currentTransform IDENTITY): nothing to do, the old blit
    NRR_R_MIRROR,      // currentTransform is a mirror / INHERIT transform: not handled, the compositor turns it
    NRR_R_UNSUPPORTED, // supportedTransforms lacks currentTransform
    NRR_R_USAGE,       // the surface does not allow COLOR_ATTACHMENT swapchain images
    NRR_R_SHARPEN,     // DM_GFX_SHARPEN (RCAS) on and the sharpening turned draw's pipeline could not be made (the old RCAS + blit path)
    NRR_R_PIPELINE,    // render pass / pipeline / descriptor objects could not be made
    NRR_R_FAILED,      // a rotated swapchain failed earlier in this run (creation, views, framebuffers): off until restart
    NRR_R_N
};
static inline const char *nrr_reason_name(int r) {
    static const char *const n[NRR_R_N] = {"pre-rotated", "off", "surface not turned", "mirror / inherit transform", "transform not supported",
                                           "no colour-attachment usage", "RCAS on, its turned draw failed", "pipeline objects failed", "failed earlier"};
    return r >= 0 && r < NRR_R_N ? n[r] : "?";
}
// The decision for a new swapchain. mode 2 decides the same way (the presenter then makes the identity swapchain anyway). sharpen = RCAS is on
// and its turned draw could not be made (RCAS on with its pipeline made is no reason: the turned draw sharpens).
static inline int nrr_decide(int mode, uint32_t current, uint32_t supported, int usage_ok, int sharpen, int pipeline_ok, int failed, uint32_t *turn) {
    *turn = 0;
    if (mode == NRR_MODE_OFF) return NRR_R_OFF;
    if (current == NRR_T_IDENTITY || current == 0) return NRR_R_IDENTITY;
    if (current != NRR_T_ROT90 && current != NRR_T_ROT180 && current != NRR_T_ROT270) return NRR_R_MIRROR;
    *turn = current;
    if (!(supported & current)) return NRR_R_UNSUPPORTED;
    if (failed) return NRR_R_FAILED;
    if (!usage_ok) return NRR_R_USAGE;
    if (sharpen) return NRR_R_SHARPEN;
    if (!pipeline_ok) return NRR_R_PIPELINE;
    return NRR_R_OK;
}
// The swapchain images' extent for a window-oriented extent w x h.
static inline void nrr_image_extent(uint32_t turn, uint32_t w, uint32_t h, uint32_t *iw, uint32_t *ih) {
    if (nrr_swaps_axes(turn)) { *iw = h; *ih = w; } else { *iw = w; *ih = h; }
}

typedef struct { int32_t x, y, w, h; } nrr_rect;
// A window-space rectangle (W x H window) mapped into the image (pixel edges map to pixel edges: integer in, integer out).
static inline nrr_rect nrr_map_rect(uint32_t turn, uint32_t W, uint32_t H, nrr_rect r) {
    nrr_rect o = r;
    switch (turn) {
    case NRR_T_ROT90: o.x = (int32_t)H - (r.y + r.h); o.y = r.x; o.w = r.h; o.h = r.w; break;
    case NRR_T_ROT180: o.x = (int32_t)W - (r.x + r.w); o.y = (int32_t)H - (r.y + r.h); break;
    case NRR_T_ROT270: o.x = r.y; o.y = (int32_t)W - (r.x + r.w); o.w = r.h; o.h = r.w; break;
    default: break;
    }
    return o;
}
// Which window point an image point shows (continuous coordinates; the compositor's view of the pre-rotated image).
static inline void nrr_window_point(uint32_t turn, double W, double H, double x, double y, double *u, double *v) {
    switch (turn) {
    case NRR_T_ROT90: *u = y; *v = H - x; break;
    case NRR_T_ROT180: *u = W - x; *v = H - y; break;
    case NRR_T_ROT270: *u = W - y; *v = x; break;
    default: *u = x; *v = y; break;
    }
}
// The draw's texture-coordinate map: uv = (m[0] p.x + m[1] p.y + t[0], m[2] p.x + m[3] p.y + t[1]), p = the fragment's position normalized
// over the viewport (the destination rectangle in the image), uv normalized over the source (sw x sh) and spanning the source rectangle
// (sx, sy, sbw, sbh) the old blit read. The push constants of shaders/nr_rotate.vert.
static inline void nrr_uv_affine(uint32_t turn, uint32_t sw, uint32_t sh, int32_t sx, int32_t sy, int32_t sbw, int32_t sbh, float m[4], float t[2]) {
    // (s, t) inside the source rectangle from p: s = r0 . p + c0, t = r1 . p + c1
    double r00 = 1, r01 = 0, c0 = 0, r10 = 0, r11 = 1, c1 = 0;
    switch (turn) {
    case NRR_T_ROT90: r00 = 0; r01 = 1; c0 = 0; r10 = -1; r11 = 0; c1 = 1; break;
    case NRR_T_ROT180: r00 = -1; r01 = 0; c0 = 1; r10 = 0; r11 = -1; c1 = 1; break;
    case NRR_T_ROT270: r00 = 0; r01 = -1; c0 = 1; r10 = 1; r11 = 0; c1 = 0; break;
    default: break;
    }
    const double kx = (double)sbw / (double)sw, ky = (double)sbh / (double)sh, ox = (double)sx / (double)sw, oy = (double)sy / (double)sh;
    m[0] = (float)(kx * r00); m[1] = (float)(kx * r01); m[2] = (float)(ky * r10); m[3] = (float)(ky * r11);
    t[0] = (float)(ox + kx * c0); t[1] = (float)(oy + ky * c1);
}
// The source texel the draw reads for image pixel (x, y) inside the viewport vp (nearest filtering; the CPU reference of the shader).
static inline void nrr_src_texel(const float m[4], const float t[2], nrr_rect vp, uint32_t sw, uint32_t sh, int32_t x, int32_t y, int32_t *tx, int32_t *ty) {
    const double px = ((double)x + 0.5 - vp.x) / vp.w, py = ((double)y + 0.5 - vp.y) / vp.h;
    const double u = m[0] * px + m[1] * py + t[0], v = m[2] * px + m[3] * py + t[1];
    int32_t a = (int32_t)floor(u * sw), b = (int32_t)floor(v * sh);
    *tx = a < 0 ? 0 : a >= (int32_t)sw ? (int32_t)sw - 1 : a;
    *ty = b < 0 ? 0 : b >= (int32_t)sh ? (int32_t)sh - 1 : b;
}

// RCAS in the turned draw (shaders/nr_rotate_rcas.frag). The fragment at image pixel (x, y) of the viewport vp shows window pixel
// d + q of the old blit's destination rectangle d, q = M (x - vp.x, y - vp.y) + c (an integer turn: exact, no interpolation), and so the
// source texel s = (sx, sy) + floor((q + 0.5) k), k = (sbw / d.w, sbh / d.h): the old blit's nearest map (Vulkan: (dst + 0.5 - dst0) *
// scale + src0, floored), so for every pixel RCAS runs on the texel the old blit would have copied from RCAS's own image. The filter's
// neighbours clamp at the texture's size (sw x sh), as rcas.comp's do (not at the source rectangle). The block from push-constant offset
// NRR_RCAS_PC_OFFSET (bytes 0..23: nr_rotate.vert's m, t); the layout matches the shader's (std430 push constants: ivec4, ivec4, ivec4,
// vec2, float).
enum { NRR_RCAS_PC_OFFSET = 32 };
typedef struct { int32_t m[4]; int32_t o[4]; int32_t s[4]; float k[2]; float sharpness; } nrr_rcas_pc;   // o: vp.x, vp.y, c.x, c.y; s: sx, sy, sw, sh
static inline void nrr_rcas_map(uint32_t turn, uint32_t W, uint32_t H, nrr_rect d, uint32_t sw, uint32_t sh, int32_t sx, int32_t sy, int32_t sbw, int32_t sbh, float sharpness, nrr_rcas_pc *pc) {
    const nrr_rect vp = nrr_map_rect(turn, W, H, d);
    int32_t m0 = 1, m1 = 0, m2 = 0, m3 = 1, c0 = 0, c1 = 0;
    switch (turn) {   // q from (a, b) = the pixel inside vp (nrr_window_point at the pixel's centre, minus d's origin)
    case NRR_T_ROT90: m0 = 0; m1 = 1; m2 = -1; m3 = 0; c0 = 0; c1 = vp.w - 1; break;            // q = (b, vp.w - 1 - a)
    case NRR_T_ROT180: m0 = -1; m1 = 0; m2 = 0; m3 = -1; c0 = vp.w - 1; c1 = vp.h - 1; break;   // q = (vp.w - 1 - a, vp.h - 1 - b)
    case NRR_T_ROT270: m0 = 0; m1 = -1; m2 = 1; m3 = 0; c0 = vp.h - 1; c1 = 0; break;           // q = (vp.h - 1 - b, a)
    default: break;
    }
    pc->m[0] = m0; pc->m[1] = m1; pc->m[2] = m2; pc->m[3] = m3;
    pc->o[0] = vp.x; pc->o[1] = vp.y; pc->o[2] = c0; pc->o[3] = c1;
    pc->s[0] = sx; pc->s[1] = sy; pc->s[2] = (int32_t)sw; pc->s[3] = (int32_t)sh;
    pc->k[0] = (float)sbw / (float)d.w; pc->k[1] = (float)sbh / (float)d.h; pc->sharpness = sharpness;
}
// The source texel the sharpening draw filters for image pixel (x, y) (the CPU reference of nr_rotate_rcas.frag, float math as the GPU's).
static inline void nrr_rcas_src_texel(const nrr_rcas_pc *pc, int32_t x, int32_t y, int32_t *tx, int32_t *ty) {
    const int32_t a = x - pc->o[0], b = y - pc->o[1];
    const int32_t qx = pc->m[0] * a + pc->m[1] * b + pc->o[2], qy = pc->m[2] * a + pc->m[3] * b + pc->o[3];
    int32_t u = pc->s[0] + (int32_t)floorf(((float)qx + 0.5f) * pc->k[0]), v = pc->s[1] + (int32_t)floorf(((float)qy + 0.5f) * pc->k[1]);
    *tx = u < 0 ? 0 : u >= pc->s[2] ? pc->s[2] - 1 : u;
    *ty = v < 0 ? 0 : v >= pc->s[3] ? pc->s[3] - 1 : v;
}

// The presenter polls the surface's transform every NRR_POLL presents while pre-rotated (a turn of the phone that the WSI does not report
// as SUBOPTIMAL, older Android); mode 2 checks the draw on the GPU every NRR_PROBE_EVERY presents with NRR_PROBES pixels.
enum { NRR_POLL = 60, NRR_PROBE_EVERY = 120, NRR_PROBES = 12 };
// The probe pixels: the viewport's corners, edge middles, centre, and 3 pseudo-random pixels from `seed`.
static inline void nrr_probe_points(nrr_rect vp, uint32_t seed, int32_t px[NRR_PROBES], int32_t py[NRR_PROBES]) {
    const int32_t x0 = vp.x, y0 = vp.y, x1 = vp.x + vp.w - 1, y1 = vp.y + vp.h - 1, xm = vp.x + vp.w / 2, ym = vp.y + vp.h / 2;
    const int32_t fx[9] = {x0, x1, x0, x1, xm, xm, x0, x1, xm}, fy[9] = {y0, y0, y1, y1, y0, y1, ym, ym, ym};
    for (int i = 0; i < 9; i++) { px[i] = fx[i]; py[i] = fy[i]; }
    uint32_t s = seed * 2654435761u + 12345u;
    for (int i = 9; i < NRR_PROBES; i++) { s = s * 1664525u + 1013904223u; px[i] = vp.x + (int32_t)((s >> 8) % (uint32_t)vp.w); s = s * 1664525u + 1013904223u; py[i] = vp.y + (int32_t)((s >> 8) % (uint32_t)vp.h); }
}
// 8-bit RGBA-class formats the probe compares (the VkFormat values: R8G8B8A8 / B8G8R8A8, UNORM and SRGB): 0 = not comparable,
// else bit 0 = BGRA order, bit 1 = sRGB.
static inline int nrr_probe_format(uint32_t vk_format) {
    switch (vk_format) { case 37: return 1 << 2; case 43: return (1 << 2) | 2; case 44: return (1 << 2) | 1; case 50: return (1 << 2) | 3; default: return 0; }
}
// Compare one probe: the drawn pixel (image format fi) against the source texel (format fs). Returns 1 = equal (sRGB: +-1 per channel; tol:
// more slack, 1 for the sharpening draw against RCAS's own image: the same filter in a fragment shader vs rcas.comp may round 1 apart).
static inline int nrr_probe_equal_tol(const uint8_t drawn[4], int fi, const uint8_t src[4], int fs, int tol);
static inline int nrr_probe_equal(const uint8_t drawn[4], int fi, const uint8_t src[4], int fs) { return nrr_probe_equal_tol(drawn, fi, src, fs, 0); }
static inline int nrr_probe_equal_tol(const uint8_t drawn[4], int fi, const uint8_t src[4], int fs, int tol_extra) {
    uint8_t a[4], b[4];
    for (int i = 0; i < 4; i++) { a[i] = drawn[i]; b[i] = src[i]; }
    if (fi & 1) { uint8_t t = a[0]; a[0] = a[2]; a[2] = t; }
    if (fs & 1) { uint8_t t = b[0]; b[0] = b[2]; b[2] = t; }
    const int tol = ((fi & 2) ? 1 : 0) + tol_extra;
    for (int i = 0; i < 4; i++) { int d = (int)a[i] - (int)b[i]; if (d < -tol || d > tol) return 0; }
    return 1;
}

#ifdef __cplusplus
}
#endif
