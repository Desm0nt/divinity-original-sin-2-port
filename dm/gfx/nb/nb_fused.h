// [DOS2] NB-18b-3: the "fused" native bundle (XGF1) - a fragment stage that runs in the pass of the LAST shader of a post chain (the
// host) and computes the elided passes inline (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-18b-3 findings"). Pure C, no backend types; host
// test dm/gfx/vulkan/engine/tests/nb_fused_test.c. Built offline by tools/dos2_post_fuse.py -> work/shaders-native-post/fused/
// <name>.<host key>.xgs, packed (xgs_pack v1) in fused.xgspack under the key "<name>.<host key>" (F1 / F2 / F3 / F3V ...; never equal
// to a translated key, so it can share native.xgspack).
//
// Why a new container: a fused stage has MORE descriptors than the translated module it replaces (the elided passes' textures /
// c-buffers / support buffer), so native::apply's rule "head equal byte for byte" cannot hold. The extra inputs are fed from the
// elided pass's recorded state (the plan does not execute that pass but keeps its binds: NB_NP_ELIDE, 18b-4a / 4b).
//
// Layout (little endian, all offsets from the start of the blob):
//   0   u32 magic 'XGF1' (0x31464758), u32 version 1, u32 flags (NB_FX_F_*), u32 n_elided (0..4), u32 n_extra (<= 64),
//       u32 n_req (<= 16), u32 bundle_size, u32 0
//   32  char host_key[16], char elided_key[4][16] (hex, no NUL; elided in chain order, unused = 0)
//   112 nb_fx_extra[n_extra]  (24 bytes each)
//       nb_fx_req[n_req]      (32 bytes each)
//       the XGS1 bundle (bundle_size bytes): the host's translated bundle with its binding table EXTENDED - header word 3 (binding
//       count) += n_extra and word 6 (SPIR-V bytes) = the fused SPIR-V; the host's own entries unchanged and first, then one entry per
//       extra in table order {vk binding, vk type, count, size, source NB_FX_SRC (0xF0), slot = extra index, sampler word, flags}; then
//       the host's input list and c1 bytes unchanged; then the fused SPIR-V.
// nb_fx_extra: the descriptor `vk_binding` (vk_type 1 combined image sampler / 6 uniform buffer, `size` bytes for a UBO) takes the
//   resource the ELIDED pass `src_pass` (1 = first elided pass of the chain) had bound at stage `src_stage` (0 vertex, 1 fragment),
//   kind `src_kind` = the translated bundle's source code (1 support buffer, 2 c-buffer slot, 7 texture slot) and slot `src_slot`;
//   `tr_flags` = the flags word of that binding in the elided shader's translated bundle (image dimension bits etc.). Flag
//   NB_FX_E_LINEAR: the fused code samples a texture the elided shader only fetched - bind a linear clamp sampler, not the pass's.
//   (NB-18b-8: used by F1A / F1AS / F1AR - FXAA's analysis taps on the pre-LUT HDR image the LUT pass only fetched; the backend takes a
//   linear clamp sampler of the game's bound sampler pool, xbg_nb_post.inc linear_sid.)
// nb_fx_req: preconditions the backend checks per frame before it elides (else: run the chain as recorded).
//   NB_FX_R_UBO_EQ    - pass `pass` (0 = host, 1.. elided), stage `stage`, c-buffer slot `slot`: the 4 floats at byte `offset` == v
//                       (VPtoRTScaleBias == (1,1,0,0): the elided pass's TexCoord is the texel centre the fused code assumes).
//   NB_FX_R_SAME_SIZE - render target 0 of pass `pass` has the size of the host pass's texture the chain feeds (1:1 reads).
//   NB_FX_R_VP_MAP    - (NB-18b-4 phone fix) pass `pass`, stage `stage`, c-buffer slot `slot`: the 4 floats s at byte `offset` map the
//                       pass's viewport onto the elided target O texel for texel: s.xy * size(O) == viewport (w, h) and s.zw * size(O)
//                       == viewport (x, y), within 1/256 px. The engine builds VPtoRTScaleBias = viewport / its target descriptor's
//                       size and sets that viewport (main+0xab5390, DRS path 0xab50e0, Sight 0x1424560 -> 0xab2050), and the post VS
//                       has TexCoord0 = inTexCoord0 * s.xy + s.zw with inTexCoord0 0..1 over the viewport: so TexCoord0 is the centre
//                       of the pixel's own texel exactly when the descriptor size is O's. UBO_EQ (1,1,0,0) is the special case of a
//                       full viewport; on the phone (wide 2400x1080 window, 3D 2160x972) it was refused every frame (16.11).
// flags: NB_FX_F_EXACT (equal to running the chain: interpreter-proven bit for bit, see the plan), NB_FX_F_Q8 (the elided RGBA8 target's
// store / load is emulated in the shader), NB_FX_F_APPROX (<= 1 LSB of the RGBA8 output; needs a visual A/B).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NB_FX_MAGIC 0x31464758u
#define NB_FX_XGS_MAGIC 0x31534758u
#define NB_FX_SRC 0xF0u
#define NB_FX_MAX_ELIDED 4
#define NB_FX_MAX_EXTRA 64
#define NB_FX_MAX_REQ 16
enum { NB_FX_F_EXACT = 1, NB_FX_F_Q8 = 2, NB_FX_F_APPROX = 4 };
enum { NB_FX_E_LINEAR = 1 };
enum { NB_FX_R_UBO_EQ = 1, NB_FX_R_SAME_SIZE = 2, NB_FX_R_VP_MAP = 3 };
enum { NB_FX_K_SUPPORT = 1, NB_FX_K_CBUF = 2, NB_FX_K_TEX = 7 };

typedef struct { uint32_t vk_binding, vk_type, size; uint8_t src_pass, src_stage, src_kind, flags; uint32_t src_slot, tr_flags; } nb_fx_extra;
typedef struct { uint8_t pass, stage, kind, pad; uint32_t slot, offset, arg; float v[4]; } nb_fx_req;
typedef struct {
    uint32_t flags, n_elided, n_extra, n_req;
    char host_key[17], elided_key[NB_FX_MAX_ELIDED][17];
    nb_fx_extra extra[NB_FX_MAX_EXTRA];
    nb_fx_req req[NB_FX_MAX_REQ];
    const uint8_t *bundle; uint32_t bundle_size;        // the embedded XGS1 bundle (points into the parsed blob)
    uint32_t host_bindings;                             // binding entries of the host's own table (= total - n_extra)
    uint32_t head_size, spirv_off, spirv_size;          // inside the bundle: bytes before the SPIR-V, its offset / size
} nb_fx;

static inline uint32_t nb_fx_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static inline int nb_fx_key_ok(const uint8_t *k, int allow_empty) {
    if (allow_empty && !k[0]) { for (int i = 0; i < 16; i++) if (k[i]) return 0; return 1; }
    for (int i = 0; i < 16; i++) if (!((k[i] >= '0' && k[i] <= '9') || (k[i] >= 'a' && k[i] <= 'f'))) return 0;
    return 1;
}

// Parse + validate. 0 = ok; else -1 and *why (static text).
static inline int nb_fx_parse(const void *data, size_t n, nb_fx *fx, const char **why) {
    const uint8_t *p = (const uint8_t *)data; const char *w = 0;
    size_t o = 112;   // declared before the first goto: the header is included from C++ too (xbg_nb_post.inc, NB-18b-4b)
    memset(fx, 0, sizeof *fx);
#define NB_FX_FAIL(s) do { w = (s); goto fail; } while (0)
    if (!p || n < 112) NB_FX_FAIL("short");
    if (nb_fx_u32(p) != NB_FX_MAGIC || nb_fx_u32(p + 4) != 1) NB_FX_FAIL("not XGF1 v1");
    fx->flags = nb_fx_u32(p + 8); fx->n_elided = nb_fx_u32(p + 12); fx->n_extra = nb_fx_u32(p + 16); fx->n_req = nb_fx_u32(p + 20);
    fx->bundle_size = nb_fx_u32(p + 24);
    if (fx->n_elided > NB_FX_MAX_ELIDED || fx->n_extra > NB_FX_MAX_EXTRA || fx->n_req > NB_FX_MAX_REQ) NB_FX_FAIL("counts");
    if ((fx->flags & NB_FX_F_EXACT) && (fx->flags & NB_FX_F_APPROX)) NB_FX_FAIL("exact and approx");
    if (!nb_fx_key_ok(p + 32, 0)) NB_FX_FAIL("host key");
    memcpy(fx->host_key, p + 32, 16);
    for (uint32_t i = 0; i < NB_FX_MAX_ELIDED; i++) {
        const uint8_t *k = p + 48 + 16 * i;
        if (!nb_fx_key_ok(k, 1) || (i < fx->n_elided) != (k[0] != 0)) NB_FX_FAIL("elided key");
        memcpy(fx->elided_key[i], k, 16);
    }
    if (n < o + fx->n_extra * 24ull + fx->n_req * 32ull + fx->bundle_size ||
        n != o + fx->n_extra * 24ull + fx->n_req * 32ull + fx->bundle_size) NB_FX_FAIL("size");
    for (uint32_t i = 0; i < fx->n_extra; i++, o += 24) {
        nb_fx_extra *e = &fx->extra[i];
        e->vk_binding = nb_fx_u32(p + o); e->vk_type = nb_fx_u32(p + o + 4); e->size = nb_fx_u32(p + o + 8);
        e->src_pass = p[o + 12]; e->src_stage = p[o + 13]; e->src_kind = p[o + 14]; e->flags = p[o + 15];
        e->src_slot = nb_fx_u32(p + o + 16); e->tr_flags = nb_fx_u32(p + o + 20);
        if (!e->src_pass || e->src_pass > fx->n_elided || e->src_stage > 1) NB_FX_FAIL("extra source pass / stage");
        if (e->src_kind != NB_FX_K_SUPPORT && e->src_kind != NB_FX_K_CBUF && e->src_kind != NB_FX_K_TEX) NB_FX_FAIL("extra source kind");
        if ((e->src_kind == NB_FX_K_TEX) != (e->vk_type == 1) || (e->vk_type == 6 && !e->size)) NB_FX_FAIL("extra type");
        for (uint32_t j = 0; j < i; j++) if (fx->extra[j].vk_binding == e->vk_binding) NB_FX_FAIL("extra binding twice");
    }
    for (uint32_t i = 0; i < fx->n_req; i++, o += 32) {
        nb_fx_req *r = &fx->req[i];
        r->pass = p[o]; r->stage = p[o + 1]; r->kind = p[o + 2]; r->pad = p[o + 3];
        r->slot = nb_fx_u32(p + o + 4); r->offset = nb_fx_u32(p + o + 8); r->arg = nb_fx_u32(p + o + 12); memcpy(r->v, p + o + 16, 16);
        if (r->pass > fx->n_elided || r->stage > 1 || (r->kind != NB_FX_R_UBO_EQ && r->kind != NB_FX_R_SAME_SIZE && r->kind != NB_FX_R_VP_MAP) || (r->offset & 3))
            NB_FX_FAIL("requirement");
    }
    {   // the embedded XGS1 bundle
        const uint8_t *b = p + o; uint32_t h[8];
        if (fx->bundle_size < 32) NB_FX_FAIL("bundle short");
        for (int i = 0; i < 8; i++) h[i] = nb_fx_u32(b + 4 * i);
        if (h[0] != NB_FX_XGS_MAGIC || h[1] != 1) NB_FX_FAIL("bundle not XGS1 v1");
        if (h[3] > 256 || h[4] > 16 || h[5] > 65536 || (h[6] & 3) || !h[6] || h[3] < fx->n_extra) NB_FX_FAIL("bundle header");
        const size_t hs = 32 + (size_t)h[3] * 32 + (size_t)h[4] * 4 + h[5];
        if (hs + h[6] != fx->bundle_size) NB_FX_FAIL("bundle size");
        fx->bundle = b; fx->host_bindings = h[3] - fx->n_extra; fx->head_size = (uint32_t)hs; fx->spirv_off = (uint32_t)hs; fx->spirv_size = h[6];
        if (nb_fx_u32(b + hs) != 0x07230203u) NB_FX_FAIL("no SPIR-V");
        for (uint32_t i = 0; i < h[3]; i++) {
            const uint8_t *e = b + 32 + 32 * i; const uint32_t bnd = nb_fx_u32(e), src = nb_fx_u32(e + 16), slot = nb_fx_u32(e + 20);
            for (uint32_t j = 0; j < i; j++) if (nb_fx_u32(b + 32 + 32 * j) == bnd) NB_FX_FAIL("bundle binding twice");
            if (i < fx->host_bindings) { if (src == NB_FX_SRC) NB_FX_FAIL("fused entry among the host's"); continue; }
            const nb_fx_extra *x = &fx->extra[i - fx->host_bindings];
            if (src != NB_FX_SRC || slot != i - fx->host_bindings || bnd != x->vk_binding || nb_fx_u32(e + 4) != x->vk_type)
                NB_FX_FAIL("bundle extra entry differs from the table");
        }
    }
#undef NB_FX_FAIL
    if (why) *why = 0;
    return 0;
fail:
    if (why) *why = w;
    fx->bundle = 0;
    return -1;
}

// The embedded bundle's host part equals the host's translated bundle (what native::apply checks for a plain native bundle): header
// except word 3 (binding count) and 6 (SPIR-V bytes), the host's binding entries, input list and c1 bytes. 1 = same.
static inline int nb_fx_host_head_matches(const nb_fx *fx, const void *translated, size_t n) {
    const uint8_t *t = (const uint8_t *)translated, *b = fx->bundle;
    if (!b || !t || n < 32) return 0;
    uint32_t th[8], fh[8];
    for (int i = 0; i < 8; i++) { th[i] = nb_fx_u32(t + 4 * i); fh[i] = nb_fx_u32(b + 4 * i); }
    if (th[0] != fh[0] || th[1] != fh[1] || th[2] != fh[2] || th[4] != fh[4] || th[5] != fh[5] || th[7] != fh[7]) return 0;
    if (th[3] != fx->host_bindings) return 0;
    const size_t tb = 32 + (size_t)th[3] * 32, rest = (size_t)th[4] * 4 + th[5];
    if (n < tb + rest + th[6]) return 0;
    if (memcmp(t + 32, b + 32, tb - 32)) return 0;
    return !memcmp(t + tb, b + 32 + (size_t)fh[3] * 32, rest);
}

// NB_FX_R_UBO_EQ against a c-buffer snapshot (ubo, n bytes) of the pass / stage / slot the requirement names. 1 = holds.
static inline int nb_fx_req_ubo_ok(const nb_fx_req *r, const void *ubo, size_t n) {
    if (r->kind != NB_FX_R_UBO_EQ) return 1;
    if (!ubo || n < r->offset + 16ull) return 0;
    float v[4]; memcpy(v, (const uint8_t *)ubo + r->offset, 16);
    return v[0] == r->v[0] && v[1] == r->v[1] && v[2] == r->v[2] && v[3] == r->v[3];
}

// NB_FX_R_VP_MAP against a c-buffer snapshot, the draw's viewport vp = (x, y, w, h) in guest pixels and the size (ow, oh) of the elided
// target. 1 = holds. s (optional) gets the 4 floats read (zeros when the range is short).
static inline int nb_fx_req_vp_ok(const nb_fx_req *r, const void *ubo, size_t n, const float vp[4], uint32_t ow, uint32_t oh, float s[4]) {
    float v[4] = {0, 0, 0, 0};
    if (s) memcpy(s, v, 16);
    if (r->kind != NB_FX_R_VP_MAP) return 1;
    if (!ubo || n < r->offset + 16ull || !ow || !oh || !(vp[2] > 0) || !(vp[3] > 0)) return 0;
    memcpy(v, (const uint8_t *)ubo + r->offset, 16);
    if (s) memcpy(s, v, 16);
    const double e = 1.0 / 256, W = ow, H = oh;
    const double d[4] = {v[0] * W - vp[2], v[1] * H - vp[3], v[2] * W - vp[0], v[3] * H - vp[1]};
    for (int i = 0; i < 4; i++) if (!(d[i] <= e && d[i] >= -e)) return 0;   // NaN fails too
    return 1;
}

// The extra entry of a fused binding (NULL: a host binding).
static inline const nb_fx_extra *nb_fx_find(const nb_fx *fx, uint32_t vk_binding) {
    for (uint32_t i = 0; i < fx->n_extra; i++) if (fx->extra[i].vk_binding == vk_binding) return &fx->extra[i];
    return 0;
}
