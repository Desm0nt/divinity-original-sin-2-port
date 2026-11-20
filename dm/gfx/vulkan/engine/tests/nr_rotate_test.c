// [DOS2] NR-20 host test: the pre-rotation rules and geometry of dm/gfx/nr/nr_rotate.h - the code the presenter (android/vulkan_present.cpp,
// namespace nrr) runs when it makes a swapchain and at every present.
// - decisions: every reason alone (off, not turned, mirror, unsupported, failed, usage, RCAS, pipeline), mode 2 decides like 1;
// - the picture: for each turn (identity / 90 / 180 / 270), window sizes even and odd, and the old blit's shapes (1:1, letterbox, crop,
//   downscale), the turned draw (nrr_map_rect + nrr_uv_affine + nearest, as shaders/nr_rotate.* compute it) written into the image and
//   then read the way the compositor shows a pre-rotated image (nrr_window_point: Vulkan's "ROTATE_90 = content rotated 90 degrees
//   clockwise") must equal the old blit into the window pixel for pixel (scaled shapes: except pixels exactly on a texel edge);
// - the transform semantics against independent statements: Vulkan's clockwise corners, nx2android's rot.vert maps for 90 / 270;
// - a turn of the phone (90 -> 270) gives the same picture from another image (the 180-degree relation of the two images);
// - mode 2's probe: points inside the viewport, the CPU texel = the simulated draw's, format classes and the swizzle-aware compare;
// - RCAS in the turned draw (NR-20-rcas.md, shaders/nr_rotate_rcas.frag): the integer store map nrr_rcas_map / nrr_rcas_src_texel against
//   the turned draw's reference (nrr_src_texel) and the old blit, for 4 turns x the shapes; the sharpened picture (RCAS of the old path =
//   RCAS over the whole texture, then the nearest blit; new = the filter at the texel each image pixel shows, read as the compositor
//   shows the image) equal pixel for pixel; a phone turn 90 <-> 270 with RCAS; the push-constant layout of the shader; RCAS probes (+-1).
// Build: clang -std=c11 -Wall -Wextra -Werror -I dm/gfx/nr dm/gfx/vulkan/engine/tests/nr_rotate_test.c -o nr_rotate_test -lm && ./nr_rotate_test
#include "nr_rotate.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; if (fails < 40) printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static const uint32_t turns[4] = {NRR_T_IDENTITY, NRR_T_ROT90, NRR_T_ROT180, NRR_T_ROT270};

static void test_decide(void) {
    uint32_t t;
    const uint32_t all = 0x1ff;
    CHECK(nrr_decide(0, NRR_T_ROT90, all, 1, 0, 1, 0, &t) == NRR_R_OFF && t == 0);
    CHECK(nrr_decide(1, NRR_T_IDENTITY, all, 1, 0, 1, 0, &t) == NRR_R_IDENTITY && t == 0);
    CHECK(nrr_decide(1, 0x10, all, 1, 0, 1, 0, &t) == NRR_R_MIRROR && t == 0);
    CHECK(nrr_decide(1, 0x100, all, 1, 0, 1, 0, &t) == NRR_R_MIRROR);
    CHECK(nrr_decide(1, NRR_T_ROT90, NRR_T_IDENTITY, 1, 0, 1, 0, &t) == NRR_R_UNSUPPORTED && t == NRR_T_ROT90);
    CHECK(nrr_decide(1, NRR_T_ROT90, all, 1, 0, 1, 1, &t) == NRR_R_FAILED);
    CHECK(nrr_decide(1, NRR_T_ROT90, all, 0, 0, 1, 0, &t) == NRR_R_USAGE);
    CHECK(nrr_decide(1, NRR_T_ROT90, all, 1, 1, 1, 0, &t) == NRR_R_SHARPEN);   // RCAS on and its turned draw could not be made
    // RCAS on with its turned draw made is no reason (the presenter passes sharpen = 0 then): pre-rotated
    CHECK(nrr_decide(1, NRR_T_ROT90, all, 1, 0, 1, 0, &t) == NRR_R_OK && strcmp(nrr_reason_name(NRR_R_SHARPEN), "RCAS on, its turned draw failed") == 0);
    CHECK(nrr_decide(1, NRR_T_ROT90, all, 1, 0, 0, 0, &t) == NRR_R_PIPELINE);
    for (int m = 1; m <= 3; m++)
        for (int k = 1; k < 4; k++) { CHECK(nrr_decide(m, turns[k], all, 1, 0, 1, 0, &t) == NRR_R_OK && t == turns[k]); }
    // the phone's surface (rd1): currentTransform 2, then 8, supported 511
    CHECK(nrr_decide(1, 2, 511, 1, 0, 1, 0, &t) == NRR_R_OK && t == 2);
    CHECK(nrr_decide(1, 8, 511, 1, 0, 1, 0, &t) == NRR_R_OK && t == 8);
    CHECK(nrr_clamp_mode(4) == 0 && nrr_clamp_mode(-1) == 0 && nrr_clamp_mode(3) == 3);
#ifdef DM_GAME_DOS2
    CHECK(nrr_default_mode() == NRR_MODE_ON);
#else
    CHECK(nrr_default_mode() == NRR_MODE_OFF);
#endif
    for (int r = 0; r < NRR_R_N; r++) CHECK(strcmp(nrr_reason_name(r), "?") != 0);
    uint32_t iw, ih;
    nrr_image_extent(NRR_T_ROT90, 2400, 1080, &iw, &ih); CHECK(iw == 1080 && ih == 2400);
    nrr_image_extent(NRR_T_ROT270, 2400, 1080, &iw, &ih); CHECK(iw == 1080 && ih == 2400);
    nrr_image_extent(NRR_T_ROT180, 2400, 1080, &iw, &ih); CHECK(iw == 2400 && ih == 1080);
}

// Vulkan: ROTATE_90 = the content rotated 90 degrees clockwise; so the window's top-left corner lands at the image's top-right, its
// top-right at the bottom-right. ROTATE_270 = counter-clockwise: top-left -> bottom-left. ROTATE_180: top-left -> bottom-right.
static void test_semantics(void) {
    const double W = 24, H = 10;
    double u, v;
    // ROTATE_90: image W'=H, H'=W; image top-right pixel centre (H-0.5, 0.5) shows window top-left (0.5, 0.5)
    nrr_window_point(NRR_T_ROT90, W, H, H - 0.5, 0.5, &u, &v); CHECK(u == 0.5 && v == 0.5);
    nrr_window_point(NRR_T_ROT90, W, H, H - 0.5, W - 0.5, &u, &v); CHECK(u == W - 0.5 && v == 0.5);   // bottom-right shows top-right
    nrr_window_point(NRR_T_ROT270, W, H, 0.5, W - 0.5, &u, &v); CHECK(u == 0.5 && v == 0.5);           // bottom-left shows top-left
    nrr_window_point(NRR_T_ROT180, W, H, W - 0.5, H - 0.5, &u, &v); CHECK(u == 0.5 && v == 0.5);
    // nx2android rot.vert (on screen on the Retroid Pocket 5): panel point (a, b) reads the window at (b, 1 - a) for ROTATE_90 and
    // (1 - b, a) for ROTATE_270 (normalized)
    for (int i = 0; i < 50; i++) {
        double a = (i * 7 % 50) / 50.0, b = (i * 13 % 50) / 50.0;
        nrr_window_point(NRR_T_ROT90, W, H, a * H, b * W, &u, &v); CHECK(fabs(u / W - b) < 1e-12 && fabs(v / H - (1 - a)) < 1e-12);
        nrr_window_point(NRR_T_ROT270, W, H, a * H, b * W, &u, &v); CHECK(fabs(u / W - (1 - b)) < 1e-12 && fabs(v / H - a) < 1e-12);
    }
    // nrr_map_rect is the forward map of nrr_window_point: the rectangle's mapped corners show its corners
    for (int k = 0; k < 4; k++) {
        nrr_rect r = {3, 2, 9, 5};
        nrr_rect o = nrr_map_rect(turns[k], (uint32_t)W, (uint32_t)H, r);
        CHECK(o.w * o.h == r.w * r.h && o.x >= 0 && o.y >= 0);
        uint32_t iw, ih; nrr_image_extent(turns[k], (uint32_t)W, (uint32_t)H, &iw, &ih);
        CHECK(o.x + o.w <= (int32_t)iw && o.y + o.h <= (int32_t)ih);
        for (int y = o.y; y < o.y + o.h; y++)
            for (int x = o.x; x < o.x + o.w; x++) { nrr_window_point(turns[k], W, H, x + 0.5, y + 0.5, &u, &v); CHECK(u > r.x && u < r.x + r.w && v > r.y && v < r.y + r.h); }
    }
}

// The old blit (nearest) of source rect (sx, sy, sbw, sbh) of an sw x sh texture into window rect d: the texel of window pixel (u, v),
// -1 = letterbox black. *edge = the pixel sits exactly on a texel edge (rounding may pick either neighbour).
static int32_t blit_texel(uint32_t sw, int32_t sx, int32_t sy, int32_t sbw, int32_t sbh, nrr_rect d, int u, int v, int *edge) {
    *edge = 0;
    if (u < d.x || u >= d.x + d.w || v < d.y || v >= d.y + d.h) return -1;
    const double fx = sx + (u + 0.5 - d.x) * sbw / d.w, fy = sy + (v + 0.5 - d.y) * sbh / d.h;
    if (fabs(fx - floor(fx + 0.5)) < 1e-6 || fabs(fy - floor(fy + 0.5)) < 1e-6) *edge = 1;
    return (int32_t)floor(fy) * (int32_t)sw + (int32_t)floor(fx);
}

typedef struct { uint32_t W, H, sw, sh; int32_t sx, sy, sbw, sbh; } shape;
// The present's rectangle math (vulkan_present.cpp present_body): the source rectangle scaled to fit the window, centred.
static nrr_rect fit(const shape *s) {
    double scale = fmin((double)s->W / s->sbw, (double)s->H / s->sbh);
    int dw = (int)(s->sbw * scale), dh = (int)(s->sbh * scale);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    nrr_rect d = {((int)s->W - dw) / 2, ((int)s->H - dh) / 2, dw, dh};
    return d;
}

static int32_t *render_turned(uint32_t turn, const shape *s, uint32_t *iw, uint32_t *ih) {
    nrr_image_extent(turn, s->W, s->H, iw, ih);
    int32_t *img = malloc(sizeof(int32_t) * (*iw) * (*ih));
    for (uint32_t i = 0; i < (*iw) * (*ih); i++) img[i] = -1;   // the render pass's clear (letterbox)
    const nrr_rect d = fit(s), vp = nrr_map_rect(turn, s->W, s->H, d);
    float m[4], t[2]; nrr_uv_affine(turn, s->sw, s->sh, s->sx, s->sy, s->sbw, s->sbh, m, t);
    CHECK(vp.x >= 0 && vp.y >= 0 && vp.w > 0 && vp.h > 0 && vp.x + vp.w <= (int32_t)*iw && vp.y + vp.h <= (int32_t)*ih);   // the viewport lies in the image
    if (!(vp.x >= 0 && vp.y >= 0 && vp.x + vp.w <= (int32_t)*iw && vp.y + vp.h <= (int32_t)*ih)) return img;
    for (int y = vp.y; y < vp.y + vp.h; y++)
        for (int x = vp.x; x < vp.x + vp.w; x++) { int32_t tx, ty; nrr_src_texel(m, t, vp, s->sw, s->sh, x, y, &tx, &ty); img[(uint32_t)y * (*iw) + (uint32_t)x] = ty * (int32_t)s->sw + tx; }
    return img;
}

static void check_shape(const shape *s, int exact) {
    const nrr_rect d = fit(s);
    for (int k = 0; k < 4; k++) {
        uint32_t iw, ih; int32_t *img = render_turned(turns[k], s, &iw, &ih);
        int mism = 0;
        for (uint32_t y = 0; y < ih; y++)
            for (uint32_t x = 0; x < iw; x++) {
                double u, v; nrr_window_point(turns[k], s->W, s->H, x + 0.5, y + 0.5, &u, &v);
                int edge; const int32_t want = blit_texel(s->sw, s->sx, s->sy, s->sbw, s->sbh, d, (int)floor(u), (int)floor(v), &edge);
                if (img[y * iw + x] != want && !(edge && !exact)) ++mism;
            }
        if (mism) printf("  shape %ux%u src %ux%u rect %d,%d %dx%d turn %u: %d pixels differ\n", s->W, s->H, s->sw, s->sh, s->sx, s->sy, s->sbw, s->sbh, turns[k], mism);
        CHECK(mism == 0);
        free(img);
    }
}

static void test_picture(void) {
    const shape shapes[] = {
        {24, 10, 24, 10, 0, 0, 24, 10},     // 1:1 (gfx-present-native: the window takes the game's output size)
        {25, 11, 25, 11, 0, 0, 25, 11},     // odd sizes
        {2400, 1080, 2400, 1080, 0, 0, 2400, 1080},   // the phone
        {24, 10, 16, 10, 0, 0, 16, 10},     // pillarbox (16:10 source in a wider window): bars left / right
        {24, 18, 24, 10, 0, 0, 24, 10},     // letterbox top / bottom
        {24, 10, 40, 20, 4, 3, 24, 10},     // crop (DRS sub-rectangle), 1:1
        {24, 10, 48, 20, 0, 0, 48, 20},     // downscale 2:1
        {30, 12, 20, 8, 2, 1, 15, 6},       // crop + upscale 2:1
    };
    for (size_t i = 0; i < sizeof shapes / sizeof shapes[0]; i++) {
        const shape *s = &shapes[i];
        const nrr_rect d = fit(s);
        const int exact = d.w == s->sbw && d.h == s->sbh;
        check_shape(s, exact);
    }
    // a scaled shape where texel edges fall on pixel centres (x 2/3): only those pixels may differ
    const shape odd = {36, 12, 24, 8, 0, 0, 24, 8};
    check_shape(&odd, 0);
}

// A turn of the phone: ROTATE_90 -> ROTATE_270 with the same window. Both images show the same picture; they are each other turned by
// 180 degrees (so a swapchain kept across the turn would show the picture upside down: the presenter must make a new one).
static void test_turnover(void) {
    const shape s = {24, 10, 24, 10, 0, 0, 24, 10};
    uint32_t w1, h1, w2, h2;
    int32_t *a = render_turned(NRR_T_ROT90, &s, &w1, &h1), *b = render_turned(NRR_T_ROT270, &s, &w2, &h2);
    CHECK(w1 == w2 && h1 == h2);
    int same = 0, flipped = 1;
    for (uint32_t y = 0; y < h1; y++)
        for (uint32_t x = 0; x < w1; x++) { if (a[y * w1 + x] == b[y * w1 + x]) ++same; if (a[y * w1 + x] != b[(h1 - 1 - y) * w1 + (w1 - 1 - x)]) flipped = 0; }
    CHECK(flipped);
    CHECK(same < (int)(w1 * h1) / 10);
    free(a); free(b);
}

static void test_probe(void) {
    const nrr_rect vp = {5, 0, 10, 24};
    int32_t px[NRR_PROBES], py[NRR_PROBES];
    for (uint32_t seed = 0; seed < 200; seed++) {
        nrr_probe_points(vp, seed, px, py);
        for (int i = 0; i < NRR_PROBES; i++) CHECK(px[i] >= vp.x && px[i] < vp.x + vp.w && py[i] >= vp.y && py[i] < vp.y + vp.h);
    }
    nrr_probe_points(vp, 3, px, py);
    CHECK(px[0] == 5 && py[0] == 0 && px[3] == 14 && py[3] == 23);
    // the CPU texel of a probe = what the simulated draw wrote there (1:1, all turns)
    const shape s = {24, 10, 24, 10, 0, 0, 24, 10};
    for (int k = 0; k < 4; k++) {
        uint32_t iw, ih; int32_t *img = render_turned(turns[k], &s, &iw, &ih);
        const nrr_rect d = fit(&s), v = nrr_map_rect(turns[k], s.W, s.H, d);
        float m[4], t[2]; nrr_uv_affine(turns[k], s.sw, s.sh, s.sx, s.sy, s.sbw, s.sbh, m, t);
        nrr_probe_points(v, 7, px, py);
        for (int i = 0; i < NRR_PROBES; i++) { int32_t tx, ty; nrr_src_texel(m, t, v, s.sw, s.sh, px[i], py[i], &tx, &ty); CHECK(img[(uint32_t)py[i] * iw + (uint32_t)px[i]] == ty * (int32_t)s.sw + tx); }
        free(img);
    }
    // formats: 37 RGBA8 UNORM, 43 RGBA8 SRGB, 44 BGRA8 UNORM, 50 BGRA8 SRGB; others not compared
    CHECK(nrr_probe_format(37) == 4 && nrr_probe_format(44) == 5 && nrr_probe_format(43) == 6 && nrr_probe_format(50) == 7 && nrr_probe_format(97) == 0);
    const uint8_t rgba[4] = {10, 20, 30, 255}, bgra[4] = {30, 20, 10, 255}, off[4] = {11, 20, 30, 255};
    CHECK(nrr_probe_equal(rgba, nrr_probe_format(37), rgba, nrr_probe_format(37)));
    CHECK(nrr_probe_equal(bgra, nrr_probe_format(44), rgba, nrr_probe_format(37)));   // swapchain BGRA, window texture RGBA
    CHECK(!nrr_probe_equal(rgba, nrr_probe_format(44), rgba, nrr_probe_format(37)));
    CHECK(!nrr_probe_equal(off, nrr_probe_format(37), rgba, nrr_probe_format(37)));
    CHECK(nrr_probe_equal(off, nrr_probe_format(43), rgba, nrr_probe_format(43)));     // sRGB: +-1
}

// The vertex shader's uv at its three vertices, interpolated at a fragment, equals nrr_src_texel's affine map (the shader is affine in p).
static void test_shader_affine(void) {
    for (int k = 0; k < 4; k++) {
        float m[4], t[2]; nrr_uv_affine(turns[k], 2400, 1080, 0, 0, 2400, 1080, m, t);
        const double P[3][2] = {{0, 0}, {2, 0}, {0, 2}};
        double uv[3][2];
        for (int i = 0; i < 3; i++) { uv[i][0] = m[0] * P[i][0] + m[1] * P[i][1] + t[0]; uv[i][1] = m[2] * P[i][0] + m[3] * P[i][1] + t[1]; }
        // barycentric interpolation at p = (0.3, 0.6)
        const double px = 0.3, py = 0.6, l1 = px / 2, l2 = py / 2, l0 = 1 - l1 - l2;
        const double iu = l0 * uv[0][0] + l1 * uv[1][0] + l2 * uv[2][0], iv = l0 * uv[0][1] + l1 * uv[1][1] + l2 * uv[2][1];
        CHECK(fabs(iu - (m[0] * px + m[1] * py + t[0])) < 1e-6 && fabs(iv - (m[2] * px + m[3] * py + t[1])) < 1e-6);
        // the viewport's corners read the source's corners (1:1): p = (0, 0) is the image rectangle's top-left
        CHECK(uv[0][0] >= -1e-6 && uv[0][0] <= 1 + 1e-6 && uv[0][1] >= -1e-6 && uv[0][1] <= 1 + 1e-6);
    }
    float m[4], t[2]; nrr_uv_affine(NRR_T_ROT90, 2400, 1080, 0, 0, 2400, 1080, m, t);
    CHECK(m[0] == 0 && m[1] == 1 && m[2] == -1 && m[3] == 0 && t[0] == 0 && t[1] == 1);   // = nx2android cw {0, 1, -1, 0, 0, 1}
    nrr_uv_affine(NRR_T_ROT270, 2400, 1080, 0, 0, 2400, 1080, m, t);
    CHECK(m[0] == 0 && m[1] == -1 && m[2] == 1 && m[3] == 0 && t[0] == 1 && t[1] == 0);   // = nx2android ccw {0, -1, 1, 0, 1, 0}
}

// ---- RCAS in the turned draw
// rcas.comp / nr_rotate_rcas.frag on the CPU (float, the shader's order of operations): the filter at texel (x, y) of an sw x sh texture.
static void rcas_at(const float *tex, int32_t sw, int32_t sh, int32_t x, int32_t y, float sharp, float out[3]) {
    float c[5][3];
    const int32_t dx[5] = {0, -1, 0, 1, 0}, dy[5] = {-1, 0, 0, 0, 1};   // b d e f h
    for (int i = 0; i < 5; i++) {
        int32_t u = x + dx[i], v = y + dy[i];
        u = u < 0 ? 0 : u > sw - 1 ? sw - 1 : u; v = v < 0 ? 0 : v > sh - 1 ? sh - 1 : v;
        for (int k = 0; k < 3; k++) c[i][k] = tex[((size_t)v * sw + u) * 3 + k];
    }
    const float limit = 0.25f - 1.0f / 16.0f;
    float lobe_rgb[3];
    for (int k = 0; k < 3; k++) {
        const float b = c[0][k], d = c[1][k], e = c[2][k], f = c[3][k], h = c[4][k];
        const float mn4 = fminf(fminf(b, d), fminf(f, h)), mx4 = fmaxf(fmaxf(b, d), fmaxf(f, h));
        const float hit_min = fminf(mn4, e) / fmaxf(4.0f * mx4, 1e-5f), hit_max = (1.0f - fmaxf(mx4, e)) / fminf(4.0f * mn4 - 4.0f, -1e-5f);
        lobe_rgb[k] = fmaxf(-hit_min, hit_max);
    }
    const float lobe = fmaxf(-limit, fminf(fmaxf(lobe_rgb[0], fmaxf(lobe_rgb[1], lobe_rgb[2])), 0.0f)) * sharp;
    for (int k = 0; k < 3; k++) {
        float v = (lobe * (c[0][k] + c[1][k] + c[3][k] + c[4][k]) + c[2][k]) / (4.0f * lobe + 1.0f);
        out[k] = v < 0 ? 0 : v > 1 ? 1 : v;
    }
}
static float *make_texture(uint32_t sw, uint32_t sh, uint32_t seed) {
    float *t = malloc(sizeof(float) * 3 * sw * sh);
    uint32_t s = seed * 2654435761u + 1u;
    for (uint32_t i = 0; i < 3 * sw * sh; i++) { s = s * 1664525u + 1013904223u; t[i] = (float)((s >> 16) & 255) / 255.0f; }
    return t;
}
// The image the sharpening turned draw writes (nrr_rcas_map + the filter; letterbox / bars: -1), as texel indices and as colours.
static int32_t *render_turned_rcas(uint32_t turn, const shape *s, const float *tex, float sharp, uint32_t *iw, uint32_t *ih, float **rgb) {
    nrr_image_extent(turn, s->W, s->H, iw, ih);
    int32_t *img = malloc(sizeof(int32_t) * (*iw) * (*ih));
    *rgb = calloc((size_t)(*iw) * (*ih) * 3, sizeof(float));
    for (uint32_t i = 0; i < (*iw) * (*ih); i++) img[i] = -1;
    const nrr_rect d = fit(s), vp = nrr_map_rect(turn, s->W, s->H, d);
    nrr_rcas_pc pc; nrr_rcas_map(turn, s->W, s->H, d, s->sw, s->sh, s->sx, s->sy, s->sbw, s->sbh, sharp, &pc);
    CHECK(pc.o[0] == vp.x && pc.o[1] == vp.y);
    for (int y = vp.y; y < vp.y + vp.h; y++)
        for (int x = vp.x; x < vp.x + vp.w; x++) {
            int32_t tx, ty; nrr_rcas_src_texel(&pc, x, y, &tx, &ty);
            const size_t at = (size_t)y * (*iw) + (size_t)x;
            img[at] = ty * (int32_t)s->sw + tx;
            rcas_at(tex, (int32_t)s->sw, (int32_t)s->sh, tx, ty, sharp, *rgb + at * 3);
        }
    return img;
}
static const shape rcas_shapes[] = {
    {24, 10, 24, 10, 0, 0, 24, 10},     // 1:1 (DOS2: gfx-present-native, the window = the game's output size)
    {25, 11, 25, 11, 0, 0, 25, 11},     // odd
    {240, 108, 240, 108, 0, 0, 240, 108},   // the phone's shape / 10
    {24, 10, 16, 10, 0, 0, 16, 10},     // pillarbox
    {24, 18, 24, 10, 0, 0, 24, 10},     // letterbox
    {24, 10, 40, 20, 4, 3, 24, 10},     // crop (DRS sub-rectangle): the filter's neighbours come from outside the rectangle, as in rcas.comp
    {24, 10, 48, 20, 0, 0, 48, 20},     // downscale 2:1
    {30, 12, 20, 8, 2, 1, 15, 6},       // crop + upscale 2:1
    {36, 12, 24, 8, 0, 0, 24, 8},       // x 1.5: texel edges on pixel centres
};
// The store map: every pixel of the viewport filters the texel the turned draw (nrr_src_texel) and the old blit show there.
static void test_rcas_map(void) {
    for (size_t i = 0; i < sizeof rcas_shapes / sizeof rcas_shapes[0]; i++) {
        const shape *s = &rcas_shapes[i];
        const nrr_rect d = fit(s);
        const int exact = d.w == s->sbw && d.h == s->sbh;
        for (int k = 0; k < 4; k++) {
            const nrr_rect vp = nrr_map_rect(turns[k], s->W, s->H, d);
            float m[4], t[2]; nrr_uv_affine(turns[k], s->sw, s->sh, s->sx, s->sy, s->sbw, s->sbh, m, t);
            nrr_rcas_pc pc; nrr_rcas_map(turns[k], s->W, s->H, d, s->sw, s->sh, s->sx, s->sy, s->sbw, s->sbh, 1.0f, &pc);
            int vs_draw = 0, vs_blit = 0;
            for (int y = vp.y; y < vp.y + vp.h; y++)
                for (int x = vp.x; x < vp.x + vp.w; x++) {
                    int32_t rx, ry, dx_, dy_; nrr_rcas_src_texel(&pc, x, y, &rx, &ry); nrr_src_texel(m, t, vp, s->sw, s->sh, x, y, &dx_, &dy_);
                    double u, v; nrr_window_point(turns[k], s->W, s->H, x + 0.5, y + 0.5, &u, &v);
                    int edge; const int32_t want = blit_texel(s->sw, s->sx, s->sy, s->sbw, s->sbh, d, (int)floor(u), (int)floor(v), &edge);
                    if ((rx != dx_ || ry != dy_) && (exact || !edge)) ++vs_draw;
                    if (ry * (int32_t)s->sw + rx != want && (exact || !edge)) ++vs_blit;
                }
            if (vs_draw || vs_blit) printf("  rcas map: shape %ux%u src %ux%u rect %d,%d %dx%d turn %u: %d pixels differ from the turned draw, %d from the blit\n", s->W, s->H, s->sw, s->sh, s->sx, s->sy, s->sbw, s->sbh, turns[k], vs_draw, vs_blit);
            CHECK(vs_draw == 0 && vs_blit == 0);
        }
    }
    // a non-uniform scale with no texel edge on any pixel centre (k = 13/20, 7/9: (2q + 1) k is never an integer): every pixel exact, so
    // the +0.5 of the blit's map and k per axis are checked (fit() keeps the aspect, and its scaled shapes put texel edges on pixel centres)
    {
        const shape s = {30, 14, 17, 10, 1, 1, 13, 7};
        const nrr_rect d = {3, 2, 20, 9};
        for (int k = 0; k < 4; k++) {
            const nrr_rect vp = nrr_map_rect(turns[k], s.W, s.H, d);
            float m[4], t[2]; nrr_uv_affine(turns[k], s.sw, s.sh, s.sx, s.sy, s.sbw, s.sbh, m, t);
            nrr_rcas_pc pc; nrr_rcas_map(turns[k], s.W, s.H, d, s.sw, s.sh, s.sx, s.sy, s.sbw, s.sbh, 1.0f, &pc);
            int bad = 0, edges = 0;
            for (int y = vp.y; y < vp.y + vp.h; y++)
                for (int x = vp.x; x < vp.x + vp.w; x++) {
                    int32_t rx, ry, dx_, dy_; nrr_rcas_src_texel(&pc, x, y, &rx, &ry); nrr_src_texel(m, t, vp, s.sw, s.sh, x, y, &dx_, &dy_);
                    double u, v; nrr_window_point(turns[k], s.W, s.H, x + 0.5, y + 0.5, &u, &v);
                    int edge; const int32_t want = blit_texel(s.sw, s.sx, s.sy, s.sbw, s.sbh, d, (int)floor(u), (int)floor(v), &edge);
                    edges += edge;
                    if (ry * (int32_t)s.sw + rx != want || rx != dx_ || ry != dy_) ++bad;
                }
            CHECK(bad == 0 && edges == 0);
        }
    }
    // the phone (2400 x 1080, 1:1): the map for 90 / 270 by hand. Image 1080 x 2400; ROTATE_90: image (x, y) shows window (y, 1079 - x),
    // ROTATE_270: (2399 - y, x) - the texel of the window texture at that point
    const nrr_rect d = {0, 0, 2400, 1080};
    nrr_rcas_pc p90, p270; int32_t tx, ty;
    nrr_rcas_map(NRR_T_ROT90, 2400, 1080, d, 2400, 1080, 0, 0, 2400, 1080, 0.7071f, &p90);
    nrr_rcas_map(NRR_T_ROT270, 2400, 1080, d, 2400, 1080, 0, 0, 2400, 1080, 0.7071f, &p270);
    CHECK(p90.m[0] == 0 && p90.m[1] == 1 && p90.m[2] == -1 && p90.m[3] == 0 && p90.o[2] == 0 && p90.o[3] == 1079 && p90.k[0] == 1 && p90.k[1] == 1);
    CHECK(p270.m[0] == 0 && p270.m[1] == -1 && p270.m[2] == 1 && p270.m[3] == 0 && p270.o[2] == 2399 && p270.o[3] == 0);
    const int32_t pts[6][2] = {{0, 0}, {1079, 0}, {0, 2399}, {1079, 2399}, {517, 1234}, {3, 2001}};
    for (int i = 0; i < 6; i++) {
        nrr_rcas_src_texel(&p90, pts[i][0], pts[i][1], &tx, &ty); CHECK(tx == pts[i][1] && ty == 1079 - pts[i][0]);
        nrr_rcas_src_texel(&p270, pts[i][0], pts[i][1], &tx, &ty); CHECK(tx == 2399 - pts[i][1] && ty == pts[i][0]);
    }
    // the shader's push-constant block (layout(offset = 32) ivec4 m, ivec4 o, ivec4 s, vec2 k, float sharpness: 32 48 64 80 88)
    CHECK(NRR_RCAS_PC_OFFSET + offsetof(nrr_rcas_pc, m) == 32 && NRR_RCAS_PC_OFFSET + offsetof(nrr_rcas_pc, o) == 48 && NRR_RCAS_PC_OFFSET + offsetof(nrr_rcas_pc, s) == 64);
    CHECK(NRR_RCAS_PC_OFFSET + offsetof(nrr_rcas_pc, k) == 80 && NRR_RCAS_PC_OFFSET + offsetof(nrr_rcas_pc, sharpness) == 88 && NRR_RCAS_PC_OFFSET + sizeof(nrr_rcas_pc) == 92);
}
// The sharpened picture: old path = RCAS over the whole texture (its own image), then the nearest blit into the window; new = the
// sharpening turned draw, read the way the compositor shows a pre-rotated image. Equal colours at every window pixel (scaled shapes:
// except pixels exactly on a texel edge, as for the plain draw).
static void test_rcas_picture(void) {
    for (size_t i = 0; i < sizeof rcas_shapes / sizeof rcas_shapes[0]; i++) {
        const shape *s = &rcas_shapes[i];
        const nrr_rect d = fit(s);
        const int exact = d.w == s->sbw && d.h == s->sbh;
        float *tex = make_texture(s->sw, s->sh, (uint32_t)i + 1u);
        float *old = malloc(sizeof(float) * 3 * s->sw * s->sh);   // RCAS's own image
        for (uint32_t y = 0; y < s->sh; y++)
            for (uint32_t x = 0; x < s->sw; x++) rcas_at(tex, (int32_t)s->sw, (int32_t)s->sh, (int32_t)x, (int32_t)y, 0.7071f, old + ((size_t)y * s->sw + x) * 3);
        for (int k = 0; k < 4; k++) {
            uint32_t iw, ih; float *rgb; int32_t *img = render_turned_rcas(turns[k], s, tex, 0.7071f, &iw, &ih, &rgb);
            int mism = 0;
            for (uint32_t y = 0; y < ih; y++)
                for (uint32_t x = 0; x < iw; x++) {
                    double u, v; nrr_window_point(turns[k], s->W, s->H, x + 0.5, y + 0.5, &u, &v);
                    int edge; const int32_t want = blit_texel(s->sw, s->sx, s->sy, s->sbw, s->sbh, d, (int)floor(u), (int)floor(v), &edge);
                    const size_t at = (size_t)y * iw + x;
                    if (want < 0) { if (img[at] != -1) ++mism; continue; }   // letterbox bars: the draw does not write them (the render pass clears)
                    if (img[at] < 0) { ++mism; continue; }
                    if (edge && !exact) continue;
                    if (memcmp(rgb + at * 3, old + (size_t)want * 3, sizeof(float) * 3) != 0) ++mism;
                }
            if (mism) printf("  rcas picture: shape %ux%u src %ux%u rect %d,%d %dx%d turn %u: %d pixels differ\n", s->W, s->H, s->sw, s->sh, s->sx, s->sy, s->sbw, s->sbh, turns[k], mism);
            CHECK(mism == 0);
            free(img); free(rgb);
        }
        free(tex); free(old);
    }
    // the sharpening is not a no-op on this texture (else the picture test would hold for a plain copy too)
    const shape s = {24, 10, 24, 10, 0, 0, 24, 10};
    float *tex = make_texture(24, 10, 99), out[3]; int differ = 0;
    for (int y = 0; y < 10; y++) for (int x = 0; x < 24; x++) { rcas_at(tex, 24, 10, x, y, 1.0f, out); if (fabsf(out[0] - tex[(y * 24 + x) * 3]) > 1.0f / 255) ++differ; }
    CHECK(differ > 24 * 10 / 4);
    // a phone turn (90 -> 270) with RCAS: the two images are each other turned by 180 degrees (the presenter makes a new swapchain)
    uint32_t w1, h1, w2, h2; float *r1, *r2;
    int32_t *a = render_turned_rcas(NRR_T_ROT90, &s, tex, 1.0f, &w1, &h1, &r1), *b = render_turned_rcas(NRR_T_ROT270, &s, tex, 1.0f, &w2, &h2, &r2);
    int flipped = w1 == w2 && h1 == h2;
    for (uint32_t y = 0; flipped && y < h1; y++)
        for (uint32_t x = 0; x < w1; x++) {
            const size_t p = (size_t)y * w1 + x, q = (size_t)(h1 - 1 - y) * w1 + (w1 - 1 - x);
            if (a[p] != b[q] || memcmp(r1 + p * 3, r2 + q * 3, sizeof(float) * 3) != 0) { flipped = 0; break; }
        }
    CHECK(flipped);
    free(a); free(b); free(r1); free(r2); free(tex);
    // mode 2's RCAS probes: the probe texel (nrr_rcas_src_texel) is the image pixel's; the compare allows +-1 (fragment vs compute rounding)
    for (int k = 0; k < 4; k++) {
        const nrr_rect dd = fit(&s), vp = nrr_map_rect(turns[k], s.W, s.H, dd);
        nrr_rcas_pc pc; nrr_rcas_map(turns[k], s.W, s.H, dd, s.sw, s.sh, s.sx, s.sy, s.sbw, s.sbh, 1.0f, &pc);
        float m[4], t[2]; nrr_uv_affine(turns[k], s.sw, s.sh, s.sx, s.sy, s.sbw, s.sbh, m, t);
        int32_t px[NRR_PROBES], py[NRR_PROBES];
        nrr_probe_points(vp, 11, px, py);
        for (int i = 0; i < NRR_PROBES; i++) { int32_t ax, ay, bx, by; nrr_rcas_src_texel(&pc, px[i], py[i], &ax, &ay); nrr_src_texel(m, t, vp, s.sw, s.sh, px[i], py[i], &bx, &by); CHECK(ax == bx && ay == by); }
    }
    const uint8_t base[4] = {100, 150, 200, 255}, one[4] = {101, 149, 200, 255}, two[4] = {102, 150, 200, 255};
    CHECK(nrr_probe_equal_tol(one, 4, base, 4, 1) && !nrr_probe_equal_tol(two, 4, base, 4, 1) && !nrr_probe_equal(one, 4, base, 4));
    CHECK(nrr_probe_equal_tol(two, 6, base, 6, 1));   // sRGB +-1 plus RCAS +-1
}

int main(void) {
    test_decide();
    test_semantics();
    test_picture();
    test_turnover();
    test_probe();
    test_shader_affine();
    test_rcas_map();
    test_rcas_picture();
    if (fails) { printf("nr_rotate_test: %d FAILED\n", fails); return 1; }
    printf("nr_rotate_test: PASS (decisions, transform semantics, picture = old blit for 4 turns x 9 shapes, turn-over, probes, shader map; RCAS: store map = turned draw = blit, sharpened picture = RCAS + blit for 4 turns x 9 shapes, 90 <-> 270, push layout, probes)\n");
    return 0;
}
