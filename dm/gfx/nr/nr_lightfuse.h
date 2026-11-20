// [DOS2] NR-LIGHTFUSE DM_NR_LIGHTFUSE (docs/dos2/nr/NR-LIGHTFUSE.md): DeferredSky -> ClusteredLightingWithSSAO as one draw. Pure C, header
// only (host test tests/nr_lightfuse_test.c); the backend is dm/gfx/vulkan/engine/xbg_nr_lightfuse.inc, the shader
// tools/dos2_post/ClusteredLightingWithSSAO_mobile.frag -DLIGHTFUSE (+ lightfuse_sky.glsl), pack key "f.ClusteredLightingWithSSAO" of
// native-nr.xgspack (tools/dos2_nr_sh_pack.py, proof tools/dos2_lightfuse.py).
// The fused module declares the translated Light table's bindings plus NR_LF_NX extras fed from the stash of the Sky draw:
//   textures: fused binding 200 + 6 i  <- the Sky draw's binding nr_lf_x[i].sky, put on the fragment stage's spare texture slot 120 + i;
//   uniform buffers: 248 <- Sky's c8 (its binding 64), 254 <- Sky's c6 (binding 70), on Sky's own uniform slots (the Light table must not use them).
#ifndef DM_GFX_NR_LIGHTFUSE_H
#define DM_GFX_NR_LIGHTFUSE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NR_LF_TEX = 0, NR_LF_UBO = 1, NR_LF_NX = 10, NR_LF_NTEX = 8, NR_LF_SPARE_TEX = 120 };
typedef struct { uint32_t fused, sky; int kind; } nr_lf_extra;
static const nr_lf_extra nr_lf_x[NR_LF_NX] = {
    {200, 10, NR_LF_TEX}, {206, 16, NR_LF_TEX}, {212, 22, NR_LF_TEX}, {218, 28, NR_LF_TEX}, {224, 34, NR_LF_TEX}, {230, 40, NR_LF_TEX},
    {236, 46, NR_LF_TEX}, {242, 52, NR_LF_TEX}, {248, 64, NR_LF_UBO}, {254, 70, NR_LF_UBO}};

static inline uint32_t nr_lf_rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
// bytes before the SPIR-V of an XGS1 bundle (as nr_sh_head); 0 = malformed
static inline size_t nr_lf_head(const uint8_t *b, size_t n) {
    if (!b || n < 32 || nr_lf_rd32(b) != 0x31534758u || nr_lf_rd32(b + 4) != 1u) return 0;
    const uint64_t h = 32ull + 32ull * nr_lf_rd32(b + 12) + 4ull * nr_lf_rd32(b + 16) + nr_lf_rd32(b + 20);
    return h + nr_lf_rd32(b + 24) == n && (n - h) % 4 == 0 ? (size_t)h : 0;
}
// 1: every descriptor of the bundle's SPIR-V is in set 0 and is either in the bundle's binding table or one of the extras, and every extra
// is declared (the host's twin binds exactly them). 0 = no.
static inline int nr_lf_bindings_ok(const uint8_t *b, size_t n) {
    const size_t head = nr_lf_head(b, n);
    if (!head) return 0;
    const uint32_t nb = nr_lf_rd32(b + 12);
    const uint8_t *s = b + head; const size_t nw = (n - head) / 4;
    if (nw < 5 || nr_lf_rd32(s) != 0x07230203u) return 0;
    uint32_t seen = 0;
    for (size_t i = 5; i < nw;) {
        const uint32_t w0 = nr_lf_rd32(s + 4 * i), c = w0 >> 16, op = w0 & 0xffff;
        if (!c || i + c > nw) return 0;
        if (op == 71 && c >= 4) {
            const uint32_t dec = nr_lf_rd32(s + 4 * (i + 2)), val = nr_lf_rd32(s + 4 * (i + 3));
            if (dec == 34 && val != 0) return 0;
            if (dec == 33) {
                int found = 0;
                for (uint32_t k = 0; k < nb && !found; k++) found = nr_lf_rd32(b + 32 + 32 * (size_t)k) == val;
                for (int k = 0; k < NR_LF_NX && !found; k++) if (nr_lf_x[k].fused == val) { found = 1; seen |= 1u << k; }
                if (!found) return 0;
            }
        }
        i += c;
    }
    return seen == (1u << NR_LF_NX) - 1u;
}
// The blend of the Light draw the fused output relies on: colour 0 blended ONE * src + ONE * dst, ADD, R G B written (VkBlendFactor ONE = 1,
// VkBlendOp ADD = 0). The Sky pass cleared Df to 0, so the blender returns the fused value, as it returned q(Sky's diff) + Light before.
static inline int nr_lf_blend_ok(uint32_t enable, uint32_t src_color, uint32_t dst_color, uint32_t op_color, uint32_t write_mask) {
    return enable && src_color == 1u && dst_color == 1u && op_color == 0u && (write_mask & 7u) == 7u;
}
// DM_NR_LIGHTFUSE_FOG: 1 = a VkFormat (enum value) without an alpha channel, where a colour write mask without A writes exactly what R G B A
// writes. The game's DeferredFog draws R G B only (mask 7) into its B10G11R11_UFLOAT (122) target (pipeline list play-1118; the round 3
// model had assumed R G B A and refused every phone Fog draw). Unknown formats count as having alpha (refused).
static inline int nr_lf_vk_no_alpha(uint32_t f) {
    return f == 4u || f == 5u || (f >= 9u && f <= 36u) || (f >= 70u && f <= 90u) || (f >= 98u && f <= 106u) || (f >= 110u && f <= 118u) ||
           f == 122u || f == 123u;
}
// The Fog draw's write mask of colour 0 (R 1 G 2 B 4 A 8): R G B written, and A written too unless its target has no alpha channel
// (then the fused draw's out 1 into Sp, a target of the same format with the same mask, stores the same texels).
static inline int nr_lf_fog_mask_ok(uint32_t write_mask, uint32_t vk_format) {
    return (write_mask & 7u) == 7u && ((write_mask & 8u) || nr_lf_vk_no_alpha(vk_format));
}

// What the backend does at the Sky draw and at the Light draw of a marked chain.
enum { NR_LF_SKY_DRAW = 0, NR_LF_SKY_SKIP = 1 };
enum { NR_LF_LIGHT_RECORDED = 0, NR_LF_LIGHT_FUSED = 1, NR_LF_LIGHT_LOST = 2 };
typedef struct { int learned; uint64_t skips, fused, learn, lost, unlearn; } nr_lf_state;
// mode 1 / 3: Sky is skipped (drawn with rasterizer discard: its pass and its clears stay) only after an earlier chain passed every check at
// its Light draw (learned) and when both twins exist; mode 2 never skips.
static inline int nr_lf_sky(nr_lf_state *s, int mode, int twins_ok) {
    if ((mode == 1 || mode == 3) && twins_ok && s->learned) { s->skips++; return NR_LF_SKY_SKIP; }
    return NR_LF_SKY_DRAW;
}
// At the Light draw: checks_ok = every Light check passed. Skipped Sky + ok: fused draw. Sky drawn + ok: the recorded Light draws and the
// chain is learned (mode 1 / 3). Skipped + a failed check: the recorded Light draws over a Df that lacks Sky's diffuse this frame (lost),
// and learning starts again.
static inline int nr_lf_light(nr_lf_state *s, int mode, int sky_skipped, int checks_ok) {
    if (sky_skipped) {
        if (checks_ok) { s->fused++; return NR_LF_LIGHT_FUSED; }
        s->lost++; s->learned = 0; s->unlearn++; return NR_LF_LIGHT_LOST;
    }
    if (checks_ok && (mode == 1 || mode == 3) && !s->learned) { s->learned = 1; s->learn++; }
    else if (!checks_ok && s->learned) { s->learned = 0; s->unlearn++; }
    return NR_LF_LIGHT_RECORDED;
}
// The Sky draw was skipped but no Light draw consumed it before the next Sky draw / frame: Sky's output is missing there.
static inline void nr_lf_orphan(nr_lf_state *s) { s->lost++; s->learned = 0; s->unlearn++; }

#ifdef __cplusplus
}
#endif
#endif
