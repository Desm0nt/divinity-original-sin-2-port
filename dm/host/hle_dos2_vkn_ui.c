// [DOS2] VK-NATIVE stage 3, part "UI" (dm_vkn_ui_register; DM_VK_NATIVE=1 only): Iggy's GDraw backend for NVN (main+0x1bea840..
// 0x1bf4338) on the native backend (dm/gfx/vkn/vkn_ui.c), and the engine's texture updates after creation. main = main.raw of
// update v786432, build 4979b200. Mode 2 has no site here and stays exactly as it was.
//
// Approach (A): every GDraw function that records NVN commands is replaced by a host function that writes the same GDraw fields the
// original writes (the context's state caches, stacks, vectors, hash nodes, the GDrawStats counters) and records the same work through
// vkn_ui; functions without command-buffer calls stay the game's own code (texture / buffer creation and finalisation go through the
// HLE NVN object functions until stage 4, as the engine's own creation functions do) and get post-hooks that mirror their objects.
// Chosen over (B) ("keep GDraw, give it native objects") because GDraw issues every command through the NVN proc table that the
// command part of this stage redirects for the ENGINE's command buffer: GDraw's commands there would be drawn with the engine's state.
//
// GDraw context (ctx = [main+0x32c6a80], the functions get it in x0; function table main+0x32c6a88, filled by 0x1bf3c90):
//   +0x00..+0x3c  projection constants (16 f32, 0x1becfc0 / 0x1befd00 / 0x1bedb70 compute them); +0x40 custom 3D transform (+0x10c
//                 set: 0x1bf4300), +0x70 / +0x74 world scale (SetViewSizeAndWorldScale 0x1bf3e30)
//   +0x78         the engine's command buffer object (NVN cmdbuf = +0x38), set by FrameBegin 0x1bec540
//   +0x80 scissor-on cache, +0x84 blend index, +0x88 channel-mask-off, +0x8c depth-stencil key, +0x90..+0xaf texture / program caches
//                 (+0xa8 the fragment program record bound last)
//   +0xb0 / +0xb8 the UI colour target / depth target (NVN texture objects = engine texture objects: vkn_tex cookies), set by the
//                 engine's UI begin 0xd20d40; +0xc0 / +0xc4 frame size; +0xc8 / +0xcc / +0xd8 / +0xdc / +0xe0.. tile and view sizes;
//                 +0xf0 / +0xf4 tile origin; +0xf8 "full target" flag; +0xfc..+0x108 the scissor rect (x0, y0, x1, y1)
//   +0x110        base of the render-target stack (entries of 0x28: +0 GDraw handle (NULL at the base), +8..+0x14 region, +0x18 u8 has
//                 region); +0x1b0 the top; +0x1b8 / +0x1c0 colour object of the top / bound colour
//   +0x228        sampler id (clamp, linear, linear mips) of every texture unit; +0x508.. depth-stencil states, +0x548.. blend + colour
//                 states, +0x5a8 / +0x5ac channel masks; +0x5b0..+0x5d8 fragment program groups by tex0 mode (3 records: plain, cxf
//                 add, cxf add + mul); +0xca0 + 0x60 * i special-blend fragment programs; +0x2320 / +0x2380 / +0x23e0 vertex program
//                 records (v2 / v2aa / v2tc2); program record: +8 vertex program object, +0x10 fragment program object
//   +0x22c0 + 0x18 * vf vertex stream / attrib states; +0x24a0 antialias texture object, +0x24c0 the default texture object
//   +0x24e0 vertex-buffer counter; vectors {u64 data, u64 meta: bits 0..22 capacity, 23 flag, 24..47 count, 48..63 growth}:
//                 +0x24f0 vertex buffers, +0x2508 / +0x2520 buffers to finalise (this / next frame), +0x2538 every texture handle,
//                 +0x2550 / +0x2568 textures to finalise, +0x2580 free render-target handles, +0x2598 render-target handles used now;
//   +0x25a8       hash map (u32 count, u32 buckets +0x25ac, nodes +0x25b0: {next, handle, +0x10 x, y, +0x18 w, h}) of target regions
//   +0x25b8       the engine device (dev: +0x25a60 ring head, +0x25a98 / +0x25aa0 ring CPU / GPU base, +0x25aa8 ring lock)
// GDraw handle (0x20): +0 texture object, +8 unique id, +0x10 u8 owned (finalised by GDraw), +0x18 dev. Vertex buffer (0x70):
//   +0 CPU of the vertices, +8 CPU of the indices, +0x10 unique id, +0x38 NVN buffer, +0x68 GPU address.
// GDrawRenderState: +0 id, +4 flags (bit 0 test id, 1 set id, 2 world space, 3 scissor, 4 identical state), +8 u8 texgen, +9 tex0
//   mode, +0xc blend, +0xd special blend, +0xe filter, +0x10 stencil test, +0x11 stencil set, +0x18 s16 cxf add[4], +0x20 tex[3],
//   +0x38 f32* edge matrix, +0x40 f32* o2w, +0x48 colour, +0x58 scissor rect, +0x68 texgen s / t, +0x88 focal point.
// GDrawPrimitive: +0 vertices, +8 indices (offsets into the buffer when one is given), +0x10 vertex count, +0x14 index count,
//   +0x18 vertex format. GDrawStats: +0 u16 flags, +2 u16 batches, +0xa u16 textures made, +0xc / +0xe u16 render targets / clears,
//   +0x10 u32 indices, +0x14 u32 vertices, +0x1c u32 texture bytes, +0x28 u32 cleared pixels.
// Replaced (mode 1, none calls its original): 0x1bf0cd0 DrawIndexedTriangles (+ the state function 0x1bef7a0, reached only from it
//   and from FilterQuad), 0x1bedd60 FilterQuad, 0x1befd00 / 0x1bf0b20 TextureDrawBufferBegin / End, 0x1becd80 RenderingBegin,
//   RenderingEnd 0x1bf4200 (its 4th word is an adrp: the function TABLE slot +0xf8 is pointed at the host function instead, checked at
//   every RenderingBegin), 0x1becc20 custom-draw begin, 0x1becf00 viewport + scissor, 0x1bedb70 depth / stencil clear (ClearID,
//   ClearStencilBits, RenderTileBegin), 0x1bedc80 SetAntialiasTexture, 0x1bf1a10 MakeTextureEnd, 0x1bf24e0 UpdateTextureRect,
//   0x1bf1f60 MakeVertexBufferEnd, 0x1bed390 BlurPass. Post-hooks on the originals: 0x1beba70 program creation, 0x1bf2c60 / 0x1bf2860 texture / render-
//   target creation, 0x1bec540 FrameBegin (pre: GDraw's finalisation lists), 0x9410f0 engine texture update (immediate branch).
//   Packet 0x2c (the engine's texture rows) is mirrored by the executor's model slot (dn_install_rc_model) before the engine's handler.
// Filters (FilterQuad blend mode 4, type +0xe): 0 blur = the game's NVN-free drivers 0x1bf3a20 -> 0x1bf32e0 / 0x1bf30e0, which call
//   the hooked TextureDrawBufferBegin / End and BlurPass 0x1bed390 (hooked); 1 colour matrix and 2 / 3 bevel / drop shadow = the
//   composite steps of 0x1bee5d0 / 0x1beedc0 rewritten here (filter_composite: those two and the state function 0x1bef7a0 are reached
//   only from FilterQuad, so they are not hooked). Special blends (mode 5) that read the destination as unit 1 get the placeholder
//   (a feedback loop in Vulkan; counted).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gfx/vkn/vkn.h"
#include "../gfx/vkn/vkn_buf.h"
#include "../gfx/vkn/vkn_tex.h"
#include "../gfx/vkn/vkn_ui.h"
#include "../gfx/vkn/vkn_pass.h"   // [mgpu P7] the journal's UI share (vkn_pass_stats4)
#include "../gfx/vkn/vkn_rt.h"   // [stage 5.5] the recording thread: the GDraw calls of the backend are records (vkn_rt_ui_*)
#include "hle_dos2_vkn.h"
#include "hle_dos2_vkn_stack.h"
void dm_vkn_glue_flush_tex(void) __attribute__((weak));   // hle_dos2_vkn_glue.c: textures written through their mapped storage

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"

// DM_VK_NATIVE_UI=0 (read once at registration; phone debugging): every GDraw draw (triangles, filter quads, blur passes, filter
// composites) is counted and not recorded, everything else as with the UI on (GDraw's own fields, targets, viewport / scissor and
// the clears, so a UI target the engine composites later is transparent black, not undefined): the 3D scene alone.
static int g_ui_off; static uint64_t g_n_ui_off;
static int ui_draw(const vkn_ui_draw_in *d) { if (g_ui_off) { g_n_ui_off++; return 0; } return vkn_rt_ui_draw(d); }   // [stage 5.5] a record (its bytes copied)
int dn_install_rc_model(uint64_t mask, int (*model)(uint64_t list, unsigned op, uint64_t pkt, uint64_t *len));   // hle_dos2_native.c

static vkn_fn g_o_prog, g_o_tex, g_o_rt, g_o_fb, g_o_upd, g_o_draw, g_o_filter, g_o_tdb, g_o_tde, g_o_rb, g_o_cdb, g_o_vs, g_o_clr, g_o_aa,
              g_o_mte, g_o_utr, g_o_mve, g_o_blur;
// census
static uint64_t g_n_prog, g_n_prog_bad, g_n_tex, g_n_tex_fail, g_n_tex_free, g_n_vb, g_n_vb_fail, g_n_vb_free, g_n_draws, g_n_draw_drop, g_n_fq, g_n_fq_drop;
static uint64_t g_n_filter[5], g_n_special_dst, g_n_tdb, g_n_tdb_fail, g_n_tde, g_n_rb, g_n_re, g_n_clear, g_n_aa, g_n_mte, g_n_utr, g_n_upd_eng, g_n_pkt2c;
static uint64_t g_n_comp, g_n_comp_drop, g_n_blur, g_n_blur_drop, g_n_tgt_unknown, g_n_upd_miss, g_n_identical, g_n_tex0_null, g_n_no_vb, g_n_table, g_n_cdb;
static uint32_t g_red_tex, g_red_draw, g_red_misc;
#define RED16(c, ...) do { if ((c)++ < 16) vkn_logf(RED "ui: " __VA_ARGS__); } while (0)

// ---- guest memory and engine helpers (native code, called as it is: NVN-free) ----
static inline void wr32(uint64_t p, uint32_t v) { memcpy((void *)(uintptr_t)p, &v, 4); }
static inline void wr64(uint64_t p, uint64_t v) { memcpy((void *)(uintptr_t)p, &v, 8); }
static inline void wr16(uint64_t p, uint16_t v) { memcpy((void *)(uintptr_t)p, &v, 2); }
static inline float rdf(uint64_t p) { float v; memcpy(&v, (const void *)(uintptr_t)p, 4); return v; }
static inline void wrf(uint64_t p, float v) { memcpy((void *)(uintptr_t)p, &v, 4); }
static void *eng(uint32_t off) { return dm_vkn_base() + off; }
typedef uint64_t (*malloc_fn)(uint64_t size, uint64_t flags);   // 0x83cc00
typedef void (*free_fn)(uint64_t p);                            // 0x83d3e0
typedef void (*ctx_fn)(uint64_t ctx);                           // 0x1becfc0 projection constants
typedef uint64_t (*hash_put_fn)(uint64_t map, uint64_t key);    // 0x1bf0c40 find-or-insert -> node
typedef void (*vec_fn)(uint64_t vec);                           // 0x1bec940 vector shrink
typedef uint64_t (*tex_create_fn)(uint64_t ctx, uint64_t w, uint64_t h, uint64_t levels, uint64_t fmt, uint64_t flags, uint64_t a6);   // 0x1bf2c60
typedef uint64_t (*rt_create_fn)(uint64_t ctx, uint64_t w, uint64_t h, uint64_t a3, uint64_t fmt, uint64_t flags, uint64_t a6, uint64_t a7,
                                 uint64_t s0, uint64_t s1, uint64_t s2);   // 0x1bf2860 (stack: storage w, storage h, storage block or NULL)
typedef int32_t (*nvn_get_fn)(uint64_t tex);                    // nvnTextureGetWidth / GetHeight ([main+0x2bbf520] / [+0x2bbf528]): no command buffer
static int32_t tex_w(uint64_t obj) { return ((nvn_get_fn)(uintptr_t)vkn_main64(0x2bbf520))(obj); }
static int32_t tex_h(uint64_t obj) { return ((nvn_get_fn)(uintptr_t)vkn_main64(0x2bbf528))(obj); }
static void proj(uint64_t ctx) { ((ctx_fn)eng(0x1becfc0))(ctx); }
// GDraw's vector push (the inlined code of 0x1bf1f60 / 0x1bf1a10 / 0x1bf24e0: grow x2 below 0xfd1 entries, else + 0x200, or by the
// growth field; copy, free the old array; store at [count]; count + 1).
static void vec_push(uint64_t vec, uint64_t val) {
    uint64_t meta = vkn_rd64(vec + 8);
    const uint64_t cap = meta & 0x7fffff, cnt = meta >> 24 & 0xffffff;
    if (cnt == cap) {
        const uint64_t grow = meta >> 48;
        const uint64_t n = grow ? cap + grow : cap ? (cap < 0xfd1 ? cap << 1 : cap + 0x200) : 1;
        if (n != cap) {
            const uint64_t na = ((malloc_fn)eng(0x83cc00))(n << 3, 0), old = vkn_rd64(vec);
            for (uint64_t i = 0; i < cnt; i++) wr64(na + 8 * i, vkn_rd64(old + 8 * i));
            ((free_fn)eng(0x83d3e0))(old);
            wr64(vec, na);
            meta = vkn_rd64(vec + 8);
            meta = (meta & 0x800000) | (n & 0x7fffff) | (meta & 0xffffffffff000000ull);
            wr64(vec + 8, meta);
        }
    }
    meta = vkn_rd64(vec + 8);
    wr64(vkn_rd64(vec) + 8 * (meta >> 24 & 0xffffff), val);
    meta = vkn_rd64(vec + 8);
    wr64(vec + 8, (meta & 0xffff000000000000ull) | (meta & 0xffffff) | ((meta + 0x1000000) >> 24 & 0xffffff) << 24);
}
static vkn_tex *tex_of(uint64_t obj) { return obj ? vkn_tex_by_cookie(obj) : NULL; }
static vkn_tex *tex_of_handle(uint64_t h) { return h ? tex_of(vkn_rd64(h)) : NULL; }

// ---- GDraw vertex buffers: vertex-buffer object -> our arena block (made at MakeVertexBufferEnd) ----
enum { VB_TAB = 1 << 14 };
typedef struct vb_e { uint64_t vb; vkn_block blk; } vb_e;
static vb_e *g_vbt; static pthread_mutex_t g_vb_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t vb_slot(uint64_t p) { return (uint32_t)((p >> 4) * 0x9E3779B97F4A7C15ull >> 50) & (VB_TAB - 1); }
static int vb_put(uint64_t vb, const vkn_block *b) {   // under g_vb_mu; an older block of the same object is retired
    if (!g_vbt && !(g_vbt = (vb_e *)calloc(VB_TAB, sizeof *g_vbt))) return -1;
    for (uint32_t i = vb_slot(vb), n = 0; n < VB_TAB; i = (i + 1) & (VB_TAB - 1), n++)
        if (!g_vbt[i].vb || g_vbt[i].vb == vb) {
            if (g_vbt[i].vb == vb && g_vbt[i].blk.size) vkn_retire_block(&g_vbt[i].blk);
            g_vbt[i].vb = vb; g_vbt[i].blk = *b; return 0;
        }
    return -1;
}
static int vb_get(uint64_t vb, vkn_block *b) {
    int ok = 0;
    pthread_mutex_lock(&g_vb_mu);
    if (g_vbt)
        for (uint32_t i = vb_slot(vb), n = 0; n < VB_TAB && g_vbt[i].vb; i = (i + 1) & (VB_TAB - 1), n++)
            if (g_vbt[i].vb == vb) { *b = g_vbt[i].blk; ok = 1; break; }
    pthread_mutex_unlock(&g_vb_mu);
    return ok;
}
static void vb_kill(uint64_t vb) {   // linear probing with backward-shift deletion
    pthread_mutex_lock(&g_vb_mu);
    if (g_vbt) {
        uint32_t i = vb_slot(vb), n = 0;
        for (; n < VB_TAB && g_vbt[i].vb && g_vbt[i].vb != vb; i = (i + 1) & (VB_TAB - 1), n++) {}
        if (n < VB_TAB && g_vbt[i].vb == vb) {
            vkn_retire_block(&g_vbt[i].blk); g_n_vb_free++;
            for (uint32_t j = (i + 1) & (VB_TAB - 1);; j = (j + 1) & (VB_TAB - 1)) {
                if (!g_vbt[j].vb) break;
                const uint32_t h = vb_slot(g_vbt[j].vb);
                if (((j - h) & (VB_TAB - 1)) >= ((j - i) & (VB_TAB - 1))) { g_vbt[i] = g_vbt[j]; i = j; }
            }
            memset(&g_vbt[i], 0, sizeof g_vbt[i]);
        }
    }
    pthread_mutex_unlock(&g_vb_mu);
}

// ---- 0x1beba70 program creation (ctx, record, blob): post. The blob's sections (0x90 from +0x90, count +0x4c): type 0 = control +
// code (+4 data offset, +0x2c stage, +0x30 / +0x3c control offset / size, +0x34 / +0x38 code offset / size); the program object goes to
// record +8 (stage 0), +0x10 (1), +0x18 (5); the object's +0xc8 is the CPU pointer of its code copy, +0xe8 the address NVN got. ----
VKN_HOOK h_prog(VKN_A) {
    const uint64_t rc = g_o_prog(VKN_F);
    const uint64_t rec = a1, blob = a2;
    if (!rec || !blob || vkn_rd32(blob) != 0x19866891u) { g_n_prog_bad++; return rc; }
    const uint32_t n = vkn_rd32(blob + 0x4c);
    for (uint32_t i = 0; i < n && i < 16; i++) {
        const uint64_t sec = blob + 0x90 + 0x90ull * i;
        if (vkn_rd32(sec + 8) != 0) continue;
        const uint32_t stage = vkn_rd32(sec + 0x2c);
        if (stage != 0 && stage != 1) continue;   // GDraw has no compute programs
        const uint64_t prog = vkn_rd64(rec + (stage ? 0x10 : 8));
        if (!prog) { g_n_prog_bad++; continue; }
        const uint64_t data = blob + vkn_rd32(sec + 4);
        uint8_t ctl[VKN_UI_CTL]; memset(ctl, 0, sizeof ctl);
        uint32_t size = vkn_rd32(sec + 0x3c); if (size > VKN_UI_CTL) size = VKN_UI_CTL;
        memcpy(ctl, (const void *)(uintptr_t)(data + vkn_rd32(sec + 0x30)), size);
        uint64_t code = vkn_rd64(prog + 0xc8); if (!code) code = vkn_rd64(prog + 0xe8);
        if (!size || !code || vkn_ui_program_put(prog, (int)stage, ctl, (const void *)(uintptr_t)code)) { g_n_prog_bad++; continue; }
        g_n_prog++;
    }
    return rc;
}

// ---- textures ----
static void tex_mirror(uint64_t obj, uint32_t w, uint32_t h, uint32_t levels, uint32_t fmt, int rt) {
    if (!obj) return;
    vkn_tex_desc d; memset(&d, 0, sizeof d);
    d.nvn_fmt = fmt; d.nvn_target = 1; d.w = w; d.h = h; d.depth = 1; d.mips = levels ? levels : 1; d.layers = 1; d.samples = 1;
    d.cls = rt ? VKN_TC_COLOR : VKN_TC_SAMPLED;
    d.pool_id = vkn_rd32(obj + 0xc0);   // registered by the original in the engine's texture pool (0x1bf2c60: dev+0x25a3c allocator)
    for (int i = 0; i < VKN_TEX_LAYER_IDS; i++) d.layer_ids[i] = ~0u;
    d.sampler_id = ~0u; d.cookie = obj;
    static const uint8_t k_rrrr[4] = {2, 2, 2, 2};   // 0x1bf2c60: format 1 (R8) gets nvnTextureBuilderSetSwizzle(R, R, R, R)
    const char *why = NULL;
    // handle index 0xffffffff: beyond the handle table, so a GDraw texture is never linked under an engine handle (cookie / pool id only)
    vkn_tex *t = vkn_tex_create_swz(1ull << 32 | 0xffffffffull, &d, NULL, fmt == 1 && !rt ? k_rrrr : NULL, &why);
    if (t) g_n_tex++;
    else { g_n_tex_fail++; RED16(g_red_tex, "GDraw %s %ux%u NVN format %#x levels %u (object %#llx): %s", rt ? "render target" : "texture", w, h, fmt, levels,
                                 (unsigned long long)obj, why ? why : "?"); }
}
// 0x1bf2c60 CreateTexture(ctx, w, h, levels, NVN format, flags, ?) -> texture object (post)
VKN_HOOK h_tex(VKN_A) {
    const uint64_t obj = g_o_tex(VKN_F);
    tex_mirror(obj, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3, (uint32_t)a4, 0);
    return obj;
}
// 0x1bf2860 CreateRenderTarget(ctx, w, h, ?, NVN format, flags, ?, ?, storage w, storage h, storage) -> texture object (post)
VKN_HOOK h_rt(VKN_A) {
    const uint64_t obj = g_o_rt(VKN_F);
    tex_mirror(obj, (uint32_t)a1, (uint32_t)a2, 1, (uint32_t)a4, 1);
    return obj;
}
// 0x1bec540 FrameBegin(ctx, cmdbuf) (pre): the textures of +0x2568 (handles; owned ones are finalised by the original) and the buffers
// of +0x2520 are finalised in this call: their native objects go too. Then the original (NVN object finalisation, no command buffer).
static vkn_rt_site g_site_fb = VKN_RT_SITE("GDraw FrameBegin frees");
VKN_HOOK h_fb(VKN_A) {
    const uint64_t ctx = a0;
    if (ctx) {
        const uint64_t tv = vkn_rd64(ctx + 0x2568), tn = vkn_rd64(ctx + 0x2570) >> 24 & 0xffffff;
        const uint64_t bv = vkn_rd64(ctx + 0x2520), bn = vkn_rd64(ctx + 0x2528) >> 24 & 0xffffff;
        const int own = (tv && tn) || (bv && bn);   // [stage 5.5] a destroy / retirement: at the recording's point (synced only then)
        if (own) vkn_rt_own_begin(&g_site_fb);
        for (uint64_t i = 0; tv && i < tn; i++) {
            const uint64_t h = vkn_rd64(tv + 8 * i);
            if (h && vkn_rd8(h + 0x10) && vkn_rd64(h) && vkn_tex_destroy_cookie(vkn_rd64(h))) g_n_tex_free++;
        }
        for (uint64_t i = 0; bv && i < bn; i++) { const uint64_t b = vkn_rd64(bv + 8 * i); if (b) vb_kill(b); }
        if (own) vkn_rt_own_end();
    }
    return g_o_fb(VKN_F);
}
// 0x1bedc80 SetAntialiasTexture(ctx, width, RGBA8 texels): a new 1-row texture when the width changed, then its texels.
VKN_HOOK h_aa(VKN_A) {
    VKN_UNUSED;
    const uint64_t ctx = a0; const int32_t w = (int32_t)a1;
    const uint64_t cur = vkn_rd64(ctx + 0x24a0);
    if (!cur || tex_w(cur) != w) wr64(ctx + 0x24a0, ((tex_create_fn)eng(0x1bf2c60))(ctx, (uint64_t)(uint32_t)w, 1, 1, 0x25, 0, 1));
    vkn_tex *t = tex_of(vkn_rd64(ctx + 0x24a0));
    if (t && a2 && w > 0) vkn_tex_update(t, 0, 0, 0, 0, (uint32_t)w, 1, (const void *)(uintptr_t)a2, (uint32_t)w * 4u);
    else g_n_upd_miss++;
    g_n_aa++;
    return 0;
}
// 0x1bf1a10 MakeTextureEnd(ctx, ProcessingInfo p) -> handle: the staging rows (p+0x28 staging object, +0 its CPU) into level 0 whole,
// the staging object into the pending-finalise vector +0x2508.
VKN_HOOK h_mte(VKN_A) {
    VKN_UNUSED;
    const uint64_t ctx = a0, p = a1;
    const uint64_t h = vkn_rd64(p + 0x20), stg = vkn_rd64(p + 0x28), obj = h ? vkn_rd64(h) : 0;
    vkn_tex *t = tex_of(obj);
    if (t && stg && vkn_rd64(stg)) {
        const uint32_t bpp = t->nvn_fmt == 1 ? 1u : 4u;   // 0x1bf1700: format 1 (alpha) one byte, else RGBA8
        vkn_tex_update(t, 0, 0, 0, 0, t->w, t->h, (const void *)(uintptr_t)vkn_rd64(stg), t->w * bpp);
    } else g_n_upd_miss++;
    vec_push(ctx + 0x2508, stg);
    g_n_mte++;
    return h;
}
// 0x1bf24e0 UpdateTextureRect(ctx, handle, x, y, source pitch, w, h, source, stack: format (1 = one byte)): the rectangle.
VKN_HOOK h_utr(VKN_A) {
    VKN_UNUSED;
    vkn_tex *t = tex_of_handle(a1);
    const uint32_t bpp = (uint32_t)s0 == 1 ? 1u : 4u;
    if (t && a7) vkn_tex_update(t, 0, 0, (int32_t)a2, (int32_t)a3, (uint32_t)a5, (uint32_t)a6, (const void *)(uintptr_t)a7, (uint32_t)a4 ? (uint32_t)a4 : (uint32_t)a5 * bpp);
    else g_n_upd_miss++;
    g_n_utr++;
    return 0;
}
// 0x1bf1f60 MakeVertexBufferEnd(ctx, p) -> vertex buffer: the staging bytes (p+0x20 staging, +0 CPU; p+0x58 total) into our block,
// counter +0x24e0 -> p+0x74, staging into +0x2508, p+0x20 = 0.
VKN_HOOK h_mve(VKN_A) {
    VKN_UNUSED;
    const uint64_t ctx = a0, p = a1;
    const uint64_t vb = vkn_rd64(p + 0x18), stg = vkn_rd64(p + 0x20); const uint32_t n = vkn_rd32(p + 0x58);
    vkn_block b;
    if (vb && stg && vkn_rd64(stg) && n && !vkn_block_alloc(VKN_ARENA_STATIC, n, 16, &b)) {
        memcpy(b.cpu, (const void *)(uintptr_t)vkn_rd64(stg), n);
        pthread_mutex_lock(&g_vb_mu); const int r = vb_put(vb, &b); pthread_mutex_unlock(&g_vb_mu);
        if (r) { vkn_block_free(&b); g_n_vb_fail++; } else g_n_vb++;
    } else { g_n_vb_fail++; RED16(g_red_misc, "vertex buffer %#llx of %u bytes: no staging data or no arena block", (unsigned long long)vb, n); }
    const uint32_t c = vkn_rd32(ctx + 0x24e0) + 1;
    wr32(ctx + 0x24e0, c); wr32(p + 0x74, c);
    vec_push(ctx + 0x2508, stg);
    wr64(p + 0x20, 0);
    return vb;
}

// ---- state ----
// The NVN state GDraw has bound, as far as draws read it beyond the context caches: textures of units 0, 1, 2, 7 (texture objects).
static uint64_t g_unit[VKN_UI_UNITS];
// The state reset at the top of 0x1becd80 / 0x1bf4200 / 0x1becc20 / 0x1bedb70 / 0x1befd00 / 0x1bedd60: depth-stencil 0x508, blend 0,
// channel mask on, the full scissor when the scissor cache is on; caches 0x80 (when on), 0x84, 0x88, 0x8c = 0.
static void state_reset(uint64_t ctx) {
    if (vkn_rd32(ctx + 0x80)) {
        wr32(ctx + 0x80, 0);
        const int32_t x0 = (int32_t)vkn_rd32(ctx + 0xfc), y0 = (int32_t)vkn_rd32(ctx + 0x100);
        vkn_rt_ui_scissor(x0, y0, (int32_t)vkn_rd32(ctx + 0x104) - x0, (int32_t)vkn_rd32(ctx + 0x108) - y0);
    }
    wr32(ctx + 0x8c, 0); wr64(ctx + 0x84, 0);
}
// 0x1becf00 viewport + scissor: the base of the stack uses the tile rect (+0xc8.. origin, +0xd8 / +0xdc size); a pushed target with a
// region its region size, else the view size +0xe0 / +0xe4 (also when +0xf8 says "full"). Scissor cache off.
static void vs_apply(uint64_t ctx) {
    int32_t x, y, w, h;
    const uint64_t top = vkn_rd64(ctx + 0x1b0);
    if (!vkn_rd32(ctx + 0xf8) && top == ctx + 0x110) {
        x = (int32_t)vkn_rd32(ctx + 0xc8); y = (int32_t)vkn_rd32(ctx + 0xcc); w = (int32_t)vkn_rd32(ctx + 0xd8); h = (int32_t)vkn_rd32(ctx + 0xdc);
        wr32(ctx + 0xfc, (uint32_t)x); wr32(ctx + 0x100, (uint32_t)y); wr32(ctx + 0x104, (uint32_t)(w + x)); wr32(ctx + 0x108, (uint32_t)(h + y));
    } else {
        if (!vkn_rd32(ctx + 0xf8) && vkn_rd8(top + 0x18)) { w = (int32_t)vkn_rd32(top + 0x10); h = (int32_t)vkn_rd32(top + 0x14); }
        else { w = (int32_t)vkn_rd32(ctx + 0xe0); h = (int32_t)vkn_rd32(ctx + 0xe4); }
        x = y = 0;
        wr64(ctx + 0xfc, 0); wr32(ctx + 0x104, (uint32_t)w); wr32(ctx + 0x108, (uint32_t)h);
    }
    vkn_rt_ui_viewport(x, y, w, h);
    wr32(ctx + 0x80, 0);
    const int32_t x0 = (int32_t)vkn_rd32(ctx + 0xfc), y0 = (int32_t)vkn_rd32(ctx + 0x100);
    vkn_rt_ui_scissor(x0, y0, (int32_t)vkn_rd32(ctx + 0x104) - x0, (int32_t)vkn_rd32(ctx + 0x108) - y0);
}
VKN_HOOK h_vs(VKN_A) { VKN_UNUSED; vs_apply(a0); return 0; }
// The colour of the stack top (NULL handle at the base: +0xb0, else the handle's object, also into +0x1b8) -> +0x1c0; the targets.
static void targets_apply(uint64_t ctx) {
    const uint64_t top = vkn_rd64(ctx + 0x1b0), th = top ? vkn_rd64(top) : 0;
    uint64_t color;
    if (!th) color = vkn_rd64(ctx + 0xb0);
    else { color = vkn_rd64(th); wr64(ctx + 0x1b8, color); }
    wr64(ctx + 0x1c0, color);
    vkn_tex *c = tex_of(color), *d = tex_of(vkn_rd64(ctx + 0xb8));
    if (!c || (!d && vkn_rd64(ctx + 0xb8))) { g_n_tgt_unknown++; RED16(g_red_misc, "GDraw target without a native texture: colour object %#llx (%s), depth object %#llx (%s), stack depth %llu",
                                                          (unsigned long long)color, c ? "known" : "unknown", (unsigned long long)vkn_rd64(ctx + 0xb8), d ? "known" : "unknown",
                                                          (unsigned long long)((top - (ctx + 0x110)) / 0x28)); }
    vkn_rt_ui_targets(c, d);
}
// RenderingEnd 0x1bf4200 (through the function table): the state reset. GDraw calls the table slot directly, so the slot gets this
// function's host-stack stub (g_re_stub, made at registration; NULL = no stub page: the slot keeps the game's function).
static void *g_re_stub;
static void ui_rendering_end(void) {
    const uint64_t ctx = vkn_main64(0x32c6a80);
    if (ctx) state_reset(ctx);
    vkn_rt_ui_pass_fence();   // [stage 5.2 B2] the engine may draw / clear with GDraw's targets bound after this: the NVN set, full area ([stage 5.5] a record)
    g_n_re++;
}
static void ui_layer_fn(void *a) { vkn_ui_layer(*(vkn_tex *const *)a); }   // [mgpu P7] (a record)
// 0x1becd80 RenderingBegin(ctx): caches +0x80..+0x87 = -1, +0x90..+0xaf = 0, the reset (with them on), the targets, viewport + scissor,
// projection.
VKN_HOOK h_rb(VKN_A) {
    VKN_UNUSED;
    const uint64_t ctx = a0;
    const uint64_t slot = (uint64_t)(uintptr_t)(dm_vkn_base() + 0x32c6a88 + 0xf8);   // GDraw table: RenderingEnd
    if (g_re_stub && vkn_rd64(slot) == (uint64_t)(uintptr_t)(dm_vkn_base() + 0x1bf4200)) { wr64(slot, (uint64_t)(uintptr_t)g_re_stub); g_n_table++; }
    wr64(ctx + 0x80, ~0ull); wr64(ctx + 0xa0, 0); wr64(ctx + 0xa8, 0); wr64(ctx + 0x90, 0); wr64(ctx + 0x98, 0);
    state_reset(ctx);
    { vkn_tex *L = tex_of(vkn_rd64(ctx + 0xb0)); vkn_rt_call(ui_layer_fn, &L, sizeof L); }   // [mgpu P7] GDraw's base colour target = the layer
    targets_apply(ctx);
    vs_apply(ctx);
    proj(ctx);
    g_n_rb++;
    return 0;
}
// 0x1becc20 custom-draw begin(ctx, render state, float out[16]): the reset, out = the projection x the object's 2x3 matrix (+0x50).
VKN_HOOK h_cdb(VKN_A) {
    VKN_UNUSED;
    const uint64_t ctx = a0, r = a1, o = a2;
    vkn_rt_ui_pass_fence();   // [stage 5.2 B2] the engine's custom draw renders into what NVN has bound ([stage 5.5] a record)
    state_reset(ctx);
    const uint64_t m = vkn_rd64(r + 0x50);
    const float c0 = rdf(ctx), c1 = rdf(ctx + 4), c2 = rdf(ctx + 8), c3 = rdf(ctx + 12);
    float out[16]; memset(out, 0, sizeof out);
    if (m) {
        out[0] = c0 * rdf(m); out[4] = c0 * rdf(m + 4); out[12] = c0 * rdf(m + 16) + c2;
        out[1] = c1 * rdf(m + 8); out[5] = c1 * rdf(m + 12); out[13] = c1 * rdf(m + 20) + c3;
    }
    out[15] = 1.0f;
    if (o) memcpy((void *)(uintptr_t)o, out, sizeof out);
    g_n_cdb++;
    return 0;
}
// 0x1bedb70 Clear(ctx, w1 bit 0 depth, w2 stencil mask): ClearDepthStencil(1.0, depth, 0, mask) on the bound depth target, the reset,
// viewport + scissor, projection (ClearID = (1, 0xff), ClearStencilBits(bits) = (0, bits), RenderTileBegin ends with ClearID).
VKN_HOOK h_clr(VKN_A) {
    VKN_UNUSED;
    const uint64_t ctx = a0;
    vkn_rt_ui_clear_ds((int)(a1 & 1), (uint8_t)a2);
    state_reset(ctx);
    vs_apply(ctx);
    proj(ctx);
    g_n_clear++;
    return 0;
}

// 0x1bef7a0 the render state of a draw (ctx, r, {vs gpu, vs cpu, fs gpu, fs cpu}): constants into vsc (32 f32) / fsc (12 f32), the
// context caches, the textures of units 0 / 7, scissor, depth-stencil key. Returns 0 = the whole function ran, 1 = an early return
// (identical state: nothing written; tex0 mode without a texture: after the program bind).
static int rs_apply(uint64_t ctx, uint64_t r, float *vsc, float *fsc) {
    const uint32_t fl = vkn_rd32(r + 4);
    if (fl >> 4 & 1) { g_n_identical++; return 1; }
    const float depth = (float)(int32_t)vkn_rd32(r) * rdf((uint64_t)(uintptr_t)eng(0x1f12324)) + rdf((uint64_t)(uintptr_t)eng(0x1f12634));
    if (!(fl >> 2 & 1)) {
        const uint64_t o = vkn_rd64(r + 0x40);
        vsc[0] = rdf(o); vsc[1] = rdf(o + 4); vsc[2] = depth; vsc[3] = rdf(o + 16); vsc[4] = rdf(o + 8); vsc[5] = rdf(o + 12); vsc[7] = rdf(o + 20);
    } else {
        vsc[0] = rdf(ctx + 0x70); vsc[1] = 0; vsc[2] = depth; vsc[3] = vsc[4] = 0; vsc[5] = rdf(ctx + 0x74); vsc[7] = 0;
    }
    vsc[6] = 0;
    const uint64_t em = vkn_rd64(r + 0x38);
    if (em) memcpy(vsc + 8, (const void *)(uintptr_t)em, 16); else memset(vsc + 8, 0, 16);
    if (!vkn_rd8(r + 8)) memset(vsc + 12, 0, 32); else memcpy(vsc + 12, (const void *)(uintptr_t)(r + 0x68), 32);
    memcpy(vsc + 20, (const void *)(uintptr_t)(ctx + (vkn_rd32(ctx + 0x10c) ? 0x40 : 0x10)), 48);
    const uint8_t blend = vkn_rd8(r + 0xc), mode = vkn_rd8(r + 9), sset = vkn_rd8(r + 0x11);
    if (vkn_rd32(ctx + 0x84) != blend) wr32(ctx + 0x84, blend);
    if (vkn_rd32(ctx + 0x88) != sset) wr32(ctx + 0x88, sset);
    uint64_t rec;
    if (vkn_rd32(ctx + 0x84) == 5) rec = ctx + 0xca0 + 0x60ull * vkn_rd8(r + 0xd);
    else {
        rec = vkn_rd64(ctx + 0x5b0 + 8ull * mode);
        const uint64_t cxf = vkn_rd64(r + 0x18);
        if (cxf) rec += (vkn_rd16(cxf + 6) == 0 ? 1 : 2) * 0x60ull;
    }
    if (vkn_rd64(ctx + 0xa8) != rec) wr64(ctx + 0xa8, rec);
    if (mode) {
        const uint64_t t0 = vkn_rd64(r + 0x20);
        if (!t0) { g_n_tex0_null++; return 1; }
        wr64(ctx + 0x90, t0);
        g_unit[0] = vkn_rd64(t0);
    }
    g_unit[3] = vkn_rd64(ctx + 0x24a0);
    memcpy(fsc, (const void *)(uintptr_t)(r + 0x48), 16);
    const uint64_t cxf = vkn_rd64(r + 0x18);
    const float k = rdf((uint64_t)(uintptr_t)eng(0x1f11fd8));
    if (!cxf) memset(fsc + 4, 0, 16);
    else for (int i = 0; i < 4; i++) fsc[4 + i] = (float)(int16_t)vkn_rd16(cxf + 2ull * i) * k;
    memcpy(fsc + 8, (const void *)(uintptr_t)(r + 0x88), 16);
    if (!(fl >> 3 & 1)) {
        if (vkn_rd32(ctx + 0x80)) {
            wr32(ctx + 0x80, 0);
            const int32_t x0 = (int32_t)vkn_rd32(ctx + 0xfc), y0 = (int32_t)vkn_rd32(ctx + 0x100);
            vkn_rt_ui_scissor(x0, y0, (int32_t)vkn_rd32(ctx + 0x104) - x0, (int32_t)vkn_rd32(ctx + 0x108) - y0);
        }
    } else {
        int32_t ox, oy;
        if (vkn_rd64(ctx + 0x1b0) == ctx + 0x110) { ox = (int32_t)vkn_rd32(ctx + 0xe8) - (int32_t)vkn_rd32(ctx + 0xc8); oy = (int32_t)vkn_rd32(ctx + 0xec) - (int32_t)vkn_rd32(ctx + 0xcc); }
        else { ox = (int32_t)vkn_rd32(ctx + 0xf0); oy = (int32_t)vkn_rd32(ctx + 0xf4); }
        const int32_t cx0 = (int32_t)vkn_rd32(ctx + 0xfc), cy0 = (int32_t)vkn_rd32(ctx + 0x100), cx1 = (int32_t)vkn_rd32(ctx + 0x104), cy1 = (int32_t)vkn_rd32(ctx + 0x108);
        int32_t x0 = (int32_t)vkn_rd32(r + 0x58) - ox, y0 = (int32_t)vkn_rd32(r + 0x5c) - oy, x1 = (int32_t)vkn_rd32(r + 0x60) - ox, y1 = (int32_t)vkn_rd32(r + 0x64) - oy;
        if (x0 < cx0) x0 = cx0; if (y0 < cy0) y0 = cy0; if (x1 > cx1) x1 = cx1; if (y1 > cy1) y1 = cy1;
        if (x0 < x1 && y0 < y1) vkn_rt_ui_scissor(x0, y0, x1 - x0, y1 - y0); else vkn_rt_ui_scissor(cx0, cy0, 0, 0);
        wr32(ctx + 0x80, 1);
    }
    const uint8_t stest = vkn_rd8(r + 0x10);
    const uint32_t key = (fl & 1u) << 1 | (fl >> 1 & 1u) | (uint32_t)stest << 16 | (uint32_t)sset << 24;
    if (key != vkn_rd32(ctx + 0x8c)) wr32(ctx + 0x8c, key);
    return 0;
}
// The draw's state from the caches (what NVN has bound after rs_apply / the resets).
static void draw_state(uint64_t ctx, vkn_ui_draw_in *d) {
    const uint32_t key = vkn_rd32(ctx + 0x8c), blend = vkn_rd32(ctx + 0x84);
    const uint8_t stest = (uint8_t)(key >> 16), sset = (uint8_t)(key >> 24);
    const uint32_t test = key >> 1 & 1, set = key & 1;   // 0x1bef7a0: + set id * 0x20 + test id * 0x10 (+ 8 with stencil)
    d->blend = (uint8_t)(blend < 6 ? blend : 0);
    d->color_off = vkn_rd32(ctx + 0x88) != 0;
    d->ds = (uint8_t)(set << 2 | test << 1 | (stest || sset ? 1u : 0u));
    d->stencil_cmp = stest; d->stencil_write = sset;
    const uint64_t rec = vkn_rd64(ctx + 0xa8);
    d->fs = rec ? vkn_rd64(rec + 0x10) : 0;
    for (int u = 0; u < VKN_UI_UNITS; u++) d->tex[u] = tex_of(g_unit[u]);
}

// 0x1bf0cd0 DrawIndexedTriangles(ctx, r, GDrawPrimitive p, vertex buffer or NULL, stats)
VKN_HOOK h_draw(VKN_A) {
    if (dm_vkn_glue_flush_tex) dm_vkn_glue_flush_tex();   // audit U1: storage-written textures before this use
    VKN_UNUSED;
    const uint64_t ctx = a0, r = a1, p = a2, buf = a3, st = a4;
    const uint32_t vf = vkn_rd32(p + 0x18), nv = vkn_rd32(p + 0x10), ni = vkn_rd32(p + 0x14);
    float vsc[32], fsc[12]; memset(vsc, 0, sizeof vsc); memset(fsc, 0, sizeof fsc);   // the original zeroes both ring blocks first
    vkn_ui_draw_in d; memset(&d, 0, sizeof d);
    if (vf < 3) d.vs = vkn_rd64(ctx + 0x2328 + 0x60ull * vf);
    rs_apply(ctx, r, vsc, fsc);
    for (int u = 0; u < 3; u++) { const uint64_t h = vkn_rd64(r + 0x20 + 8ull * u); g_unit[u] = h ? vkn_rd64(h) : vkn_rd64(ctx + 0x24c0); }
    g_unit[3] = vkn_rd64(ctx + 0x24a0) ? vkn_rd64(ctx + 0x24a0) : vkn_rd64(ctx + 0x24c0);
    draw_state(ctx, &d);
    d.vfmt = (uint8_t)(vf < 3 ? vf : 0); d.vsc = vsc; d.vsc_bytes = sizeof vsc; d.fsc = fsc; d.fsc_bytes = sizeof fsc;
    d.nverts = nv; d.nidx = ni;
    int drop = vf >= 3;
    if (buf) {
        vkn_block b;
        if (vb_get(buf, &b)) {
            d.vb = &b; d.vb_off = vkn_rd64(p); d.ib_off = vkn_rd64(buf + 8) - vkn_rd64(buf) + vkn_rd64(p + 8);
            if (!drop) drop = ui_draw(&d);
        } else { drop = 1; g_n_no_vb++; RED16(g_red_draw, "draw from vertex buffer %#llx that has no native block (%u vertices, %u indices)", (unsigned long long)buf, nv, ni); }
    } else {
        d.verts = (const void *)(uintptr_t)vkn_rd64(p); d.idx = (const uint16_t *)(uintptr_t)vkn_rd64(p + 8); d.quads = d.idx == NULL;
        if (!drop) drop = ui_draw(&d);
    }
    if (drop) g_n_draw_drop++; else g_n_draws++;
    if (st) {   // GDrawStats: flags |= 1, batches + 1, indices / vertices
        wr16(st, (uint16_t)(vkn_rd16(st) | 1)); wr16(st + 2, (uint16_t)(vkn_rd16(st + 2) + 1));
        wr32(st + 0x10, vkn_rd32(st + 0x10) + ni); wr32(st + 0x14, vkn_rd32(st + 0x14) + nv);
    }
    return 0;
}

static void filter_composite(uint64_t ctx, uint64_t r, const int32_t dst[4], const float uv[4], int kind, uint64_t st);
// 0x1bedd60 FilterQuad(ctx, r, x0, y0, x1, y1, stats): the rectangle clipped to the tile; the reset with +0x10c = 0 meanwhile; blend
// mode 4 = a filter (+0xe: 0 blur, 1 colour matrix, 2 / 3 bevel / drop shadow: NOT applied, see the top), else one v2tc2 quad with the
// render state, identity world, uv = the rectangle over the frame; special blends (5) read the destination as unit 1 (placeholder here).
VKN_HOOK h_filter(VKN_A) {
    if (dm_vkn_glue_flush_tex) dm_vkn_glue_flush_tex();   // audit U1: storage-written textures before this use
    VKN_UNUSED;
    const uint64_t ctx = a0, r = a1;
    const int32_t ox = (int32_t)vkn_rd32(ctx + 0xf0), oy = (int32_t)vkn_rd32(ctx + 0xf4);
    int32_t X0 = (int32_t)a2, Y0 = (int32_t)a3, X1 = (int32_t)a4, Y1 = (int32_t)a5;
    if (X0 < ox) X0 = ox; if (Y0 < oy) Y0 = oy;
    if (X1 > (int32_t)vkn_rd32(ctx + 0xe0) + ox) X1 = (int32_t)vkn_rd32(ctx + 0xe0) + ox;
    if (Y1 > (int32_t)vkn_rd32(ctx + 0xe4) + oy) Y1 = (int32_t)vkn_rd32(ctx + 0xe4) + oy;
    g_n_fq++;
    if (X1 < X0 || Y1 < Y0) return 0;
    const float fw = (float)(int32_t)vkn_rd32(ctx + 0xc0), fh = (float)(int32_t)vkn_rd32(ctx + 0xc4);
    const float u0 = (float)(X0 - ox) / fw, v0 = (float)(Y0 - oy) / fh, u1 = (float)(X1 - ox) / fw, v1 = (float)(Y1 - oy) / fh;
    const uint32_t saved = vkn_rd32(ctx + 0x10c);
    state_reset(ctx);
    wr32(ctx + 0x10c, 0);
    const uint8_t blend = vkn_rd8(r + 0xc);
    if (blend == 4) {
        const uint8_t f = vkn_rd8(r + 0xe);
        if (f == 0) {   // blur (0x1bee3e0..0x1bee488): the region node of tex[0] (find-or-insert), "full target" on, the game's blur driver
            const uint64_t node = ((hash_put_fn)eng(0x1bf0c40))(ctx + 0x25a8, vkn_rd64(r + 0x20));
            uint32_t reg[4] = {0, 0, 0, 0};
            if (node) memcpy(reg, (const void *)(uintptr_t)(node + 0x10), 16);
            const int32_t rect[4] = {X0 - ox, Y0 - oy, X1 - ox, Y1 - oy};
            uint32_t view[4]; memcpy(view, (const void *)(uintptr_t)(ctx + 0xe0), 8); view[2] = vkn_rd32(ctx + 0xc0); view[3] = vkn_rd32(ctx + 0xc4);
            wr32(ctx + 0xf8, 1);
            vs_apply(ctx); proj(ctx);
            typedef void (*blur_fn)(uint64_t ctx, const uint32_t *view, uint64_t r, const int32_t *rect, uint32_t *region, uint64_t stats);
            ((blur_fn)eng(0x1bf3a20))(ctx, view, r, rect, reg, a6);   // NVN-free: calls the hooked 0x1befd00 / 0x1bf0b20 / 0x1bed390
            wr32(ctx + 0xf8, 0);
            vs_apply(ctx); proj(ctx);
        }
        else if (f >= 1 && f <= 3) {   // 1 colour matrix (0x1bee5d0), 2 bevel (0x1beedc0 w4 = 1), 3 drop shadow (w4 = 0)
            const int32_t dst[4] = {X0, Y0, X1, Y1}; const float uv[4] = {u0, v0, u1, v1};
            filter_composite(ctx, r, dst, uv, f, a6);
        } else RED16(g_red_misc, "FilterQuad with filter type %u: the original aborts (0x80f560); skipped", f);
        g_n_filter[f < 4 ? f : 4]++;
        wr32(ctx + 0x10c, saved);
        return 0;
    }
    if (blend == 5) { g_unit[1] = 0; g_n_special_dst++; }   // the destination as unit 1: a feedback loop in Vulkan (placeholder)
    float vsc[32], fsc[12]; memset(vsc, 0, sizeof vsc); memset(fsc, 0, sizeof fsc);
    rs_apply(ctx, r, vsc, fsc);
    memset(vsc, 0, sizeof vsc); vsc[0] = 1.0f; vsc[5] = 1.0f;   // the original overwrites the vertex block after the state function
    memcpy(vsc + 20, (const void *)(uintptr_t)(ctx + 0x10), 48);
    const float q[16] = {(float)X0, (float)Y0, u0, v0, (float)X1, (float)Y0, u1, v0, (float)X1, (float)Y1, u1, v1, (float)X0, (float)Y1, u0, v1};
    vkn_ui_draw_in d; memset(&d, 0, sizeof d);
    draw_state(ctx, &d);
    d.vs = vkn_rd64(ctx + 0x23e8); d.vfmt = VKN_UI_VF_V2TC2; d.quads = 1; d.verts = q; d.nverts = 4;
    d.vsc = vsc; d.vsc_bytes = sizeof vsc; d.fsc = fsc; d.fsc_bytes = sizeof fsc;
    if (ui_draw(&d)) g_n_fq_drop++;
    wr32(ctx + 0x10c, saved);
    return 0;
}

// 0x1bed390 BlurPass(ctx, r, taps, f32 weights[taps][4], int dest[4], f32 uv[4], f32 extra[4]) (from the blur drivers 0x1bf32e0 /
// 0x1bf30e0, which stay the game's code: NVN-free, they call the hooked TextureDrawBufferBegin / End / this): unit 0 = r's tex[0]
// (+0x90 = its handle), units 1, 2, 7 the default texture; fragment program record +0x1d80 + 0x60 * taps; vertex block identity +
// projection, fragment block 0 zero, block 1 (0xa0) = extra + the taps; one v2tc2 quad; Barrier.
VKN_HOOK h_blur(VKN_A) {
    VKN_UNUSED;
    const uint64_t ctx = a0, r = a1, wts = a3, dst = a4, uvp = a5, ex = a6;
    int32_t taps = (int32_t)a2; if (taps < 0) taps = 0; if (taps > 9) taps = 9;   // 0xa0 bytes hold 16 + 9 x 16
    float vsc[32], fsc[12], fsc1[40]; memset(vsc, 0, sizeof vsc); memset(fsc, 0, sizeof fsc); memset(fsc1, 0, sizeof fsc1);
    vsc[0] = 1.0f; vsc[5] = 1.0f; memcpy(vsc + 20, (const void *)(uintptr_t)(ctx + 0x10), 48);
    if (ex) memcpy(fsc1, (const void *)(uintptr_t)ex, 16);
    if (wts && taps) memcpy(fsc1 + 4, (const void *)(uintptr_t)wts, 16u * (uint32_t)taps);
    const uint64_t h0 = vkn_rd64(r + 0x20);
    wr64(ctx + 0x90, h0);
    g_unit[0] = h0 ? vkn_rd64(h0) : 0; g_unit[1] = g_unit[2] = g_unit[3] = vkn_rd64(ctx + 0x24c0);
    const uint64_t rec = ctx + 0x1d80 + 0x60ull * (uint32_t)(int32_t)a2;
    if (vkn_rd64(ctx + 0xa8) != rec) wr64(ctx + 0xa8, rec);
    const int32_t x0 = (int32_t)vkn_rd32(dst), y0 = (int32_t)vkn_rd32(dst + 4), x1 = (int32_t)vkn_rd32(dst + 8), y1 = (int32_t)vkn_rd32(dst + 12);
    const float u0 = rdf(uvp), v0 = rdf(uvp + 4), u1 = rdf(uvp + 8), v1 = rdf(uvp + 12);
    const float q[16] = {(float)x0, (float)y0, u0, v0, (float)x1, (float)y0, u1, v0, (float)x1, (float)y1, u1, v1, (float)x0, (float)y1, u0, v1};
    vkn_ui_draw_in d; memset(&d, 0, sizeof d);
    draw_state(ctx, &d);
    d.vs = vkn_rd64(ctx + 0x23e8); d.vfmt = VKN_UI_VF_V2TC2; d.quads = 1; d.verts = q; d.nverts = 4;
    d.vsc = vsc; d.vsc_bytes = sizeof vsc; d.fsc = fsc; d.fsc_bytes = sizeof fsc; d.fsc1 = fsc1; d.fsc1_bytes = 0xa0;
    if (ui_draw(&d)) g_n_blur_drop++; else g_n_blur++;
    return 0;
}

// 0x1befd00 TextureDrawBufferBegin(ctx, gswf_recti *region or NULL, format, flags, unique id, stats) -> 1 = pushed. A render-target
// handle from the free vector +0x2580 (re-made at the frame size when it differs) or a new one (handle + 0x1bf2860 + hash node + the
// texture vector); into the used vector +0x2598; the new stack entry; its hash node gets the region; the colour target; the state
// reset, full scissor and viewport of the frame, projection; the region (+2 px) cleared to transparent black; viewport, projection.
// [mgpu P1] a pushed target with a region: Iggy draws and reads only its cleared rect until TextureDrawBufferEnd (vkn_ui.h vkn_ui_region)
typedef struct ui_region_rec { vkn_tex *t; int32_t r[4]; } ui_region_rec;
static void ui_region_fn(void *a) { const ui_region_rec *r = (const ui_region_rec *)a; vkn_ui_region(r->t, r->r); }
static int tdb_impl(uint64_t ctx, uint64_t reg, uint64_t uid, uint64_t st) {
    const int32_t fw = (int32_t)vkn_rd32(ctx + 0xc0), fh = (int32_t)vkn_rd32(ctx + 0xc4);
    const uint64_t top = vkn_rd64(ctx + 0x1b0);
    if (!vkn_rd32(ctx + 0xd8) || !vkn_rd32(ctx + 0xdc) || top + 0x28 >= ctx + 0x1b0) { g_n_tdb_fail++; return 0; }   // the original asserts and returns 0
    uint64_t h;
    const uint64_t fmeta = vkn_rd64(ctx + 0x2588);
    if (fmeta & 0xffffff000000ull) {   // pop index 0 of the free vector (the last moves into its place)
        const uint64_t arr = vkn_rd64(ctx + 0x2580);
        const uint64_t n1 = ((fmeta >> 24) - 1) & 0xffffff;
        h = vkn_rd64(arr);
        wr64(ctx + 0x2588, (fmeta & 0xffff000000000000ull) | (fmeta & 0xffffff) | n1 << 24);
        if (n1) { const uint64_t last = vkn_rd64(arr + 8 * n1); wr64(arr + 8 * n1, h); wr64(arr, last); }
        wr64(vkn_rd64(ctx + 0x2580) + 8 * (vkn_rd64(ctx + 0x2588) >> 24 & 0xffffff), 0);
        ((vec_fn)eng(0x1bec940))(ctx + 0x2580);
        const uint64_t obj = vkn_rd64(h);
        if (tex_w(obj) != fw || tex_h(obj) != fh) {   // re-made at the frame size on the same storage (+0xe0 of the old object)
            const uint64_t nobj = ((rt_create_fn)eng(0x1bf2860))(ctx, (uint64_t)(uint32_t)fw, (uint64_t)(uint32_t)fh, 1, 0x25, 0, 1, 1, 0x780, 0x438, obj + 0xe0);
            wr64(h, nobj);
            const int32_t nw = tex_w(nobj), nh = tex_h(nobj);
            const uint32_t nb = vkn_rd32(ctx + 0x25ac);
            uint64_t node = vkn_rd64(vkn_rd64(ctx + 0x25b0) + 8 * (nb ? h % nb : 0));
            while (node && vkn_rd64(node + 8) != h) node = vkn_rd64(node);
            if (node) { wr64(node + 0x10, 0); wr32(node + 0x18, (uint32_t)nw); wr32(node + 0x1c, (uint32_t)nh); }
        }
    } else {
        h = ((malloc_fn)eng(0x83cc00))(0x20, 0);
        wr64(h + 0x18, vkn_rd64(ctx + 0x25b8)); wr64(h, 0); wr64(h + 8, 0);
        *(uint8_t *)(uintptr_t)(h + 0x10) = 1;
        const uint64_t obj = ((rt_create_fn)eng(0x1bf2860))(ctx, (uint64_t)(uint32_t)fw, (uint64_t)(uint32_t)fh, 1, 0x25, 0, 1, 1, 0x780, 0x438, 0);
        wr64(h, obj);
        const uint32_t w = (uint32_t)tex_w(obj), hh = (uint32_t)tex_h(obj);
        const uint64_t node = ((hash_put_fn)eng(0x1bf0c40))(ctx + 0x25a8, h);
        if (node) { wr64(node + 0x10, 0); wr64(node + 0x18, (uint64_t)w | (uint64_t)hh << 32); }
        vec_push(ctx + 0x2538, h);
    }
    vec_push(ctx + 0x2598, h);
    const uint64_t ne = top + 0x28;   // the new stack entry
    wr64(ne, h);
    *(uint8_t *)(uintptr_t)(ne + 0x18) = uid != 0;
    if (uid) {
        const int32_t r0 = (int32_t)vkn_rd32(reg), r1 = (int32_t)vkn_rd32(reg + 4);
        wr32(ne + 8, (uint32_t)r0); wr32(ne + 0xc, (uint32_t)r1); wr32(ne + 0x10, (uint32_t)((int32_t)vkn_rd32(reg + 8) - r0)); wr32(ne + 0x14, (uint32_t)((int32_t)vkn_rd32(reg + 12) - r1));
    }
    uint64_t node;
    {   // find-or-insert the handle's node (inline in the original, the same as 0x1bf0c40)
        node = ((hash_put_fn)eng(0x1bf0c40))(ctx + 0x25a8, h);
        if (!node) {
            const uint32_t nb = vkn_rd32(ctx + 0x25ac);
            node = vkn_rd64(vkn_rd64(ctx + 0x25b0) + 8 * (nb ? h % nb : 0));
            while (node && vkn_rd64(node + 8) != h) node = vkn_rd64(node);
        }
    }
    int32_t nx, ny, nw, nh, cx, cy, cw, chh;   // node region / clear rectangle
    if (!reg) { nx = ny = 0; nw = fw; nh = fh; cx = cy = 0; cw = fw; chh = fh; }
    else {
        int32_t ox = 0, oy = 0;
        if (!vkn_rd32(ctx + 0xf8)) { ox = (int32_t)vkn_rd32(ctx + 0xf0); oy = (int32_t)vkn_rd32(ctx + 0xf4); }
        int32_t t;
        t = (int32_t)vkn_rd32(reg) - ox; nx = t < 0 ? 0 : t;
        nw = (int32_t)vkn_rd32(ctx + 0xe0); t = (int32_t)vkn_rd32(reg + 8) - ox; if (t <= nw) nw = t;
        nh = (int32_t)vkn_rd32(ctx + 0xe4); t = (int32_t)vkn_rd32(reg + 12) - oy; if (t <= nh) nh = t;
        int32_t ex = fw; if (nw + 2 <= fw) ex = nw + 2;
        int32_t ey = fh; if (nh + 2 <= fh) ey = nh + 2;
        t = nx - 2; cx = t < 0 ? 0 : t;
        cw = ex - cx;
        if (cw < 1) return 0;   // 0x1beff48: nothing to draw into
        t = (int32_t)vkn_rd32(reg + 4) - oy; ny = t < 0 ? 0 : t;
        t = ny - 2; cy = t < 0 ? 0 : t;
        chh = ey - cy;
        if (chh < 1) return 0;
    }
    if (node) { wr32(node + 0x10, (uint32_t)nx); wr32(node + 0x14, (uint32_t)ny); wr32(node + 0x18, (uint32_t)nw); wr32(node + 0x1c, (uint32_t)nh); }
    if (st) { wr16(st, (uint16_t)(vkn_rd16(st) | 0x20)); wr16(st + 0xc, (uint16_t)(vkn_rd16(st + 0xc) + 1)); }
    // push, targets
    wr64(ctx + 0x1b0, ne);
    wr64(ctx + 0x1b8, vkn_rd64(vkn_rd64(ne)));
    targets_apply(ctx);
    state_reset(ctx);
    wr32(ctx + 0xfc, 0); wr32(ctx + 0x100, 0); wr32(ctx + 0x108, (uint32_t)fh); wr32(ctx + 0x104, (uint32_t)fw);
    vkn_rt_ui_viewport(0, 0, fw, fh);
    wr32(ctx + 0x80, 0);
    vkn_rt_ui_scissor(0, 0, fw, fh);
    {   // the projection of the whole target (0x1befd00, before its clearing quad)
        const float a = 2.0f / (float)fw, b = 2.0f / (float)-fh, c = (float)fw / (float)-fw, e = (float)fh / (float)fh;
        float m[16]; memset(m, 0, sizeof m);
        m[0] = a; m[1] = b; m[2] = c; m[3] = e; m[4] = a; m[9] = b; m[12] = c; m[13] = e; m[14] = 1.0f; m[15] = 1.0f;
        memcpy((void *)(uintptr_t)ctx, m, sizeof m);
    }
    const int32_t rect[4] = {cx, cy, cw, chh};
    if (reg) { const ui_region_rec rr = {tex_of(vkn_rd64(h)), {cx, cy, cw, chh}}; vkn_rt_call(ui_region_fn, &rr, sizeof rr); }   // [mgpu P1] the target's region
    vkn_rt_ui_clear_color(tex_of(vkn_rd64(h)), rect);
    if (st) { wr16(st, (uint16_t)(vkn_rd16(st) | 0x40)); wr16(st + 0xe, (uint16_t)(vkn_rd16(st + 0xe) + 1)); wr32(st + 0x28, vkn_rd32(st + 0x28) + (uint32_t)(chh * cw)); }
    vs_apply(ctx);
    proj(ctx);
    g_n_tdb++;
    return 1;
}
VKN_HOOK h_tdb(VKN_A) { VKN_UNUSED; return (uint64_t)tdb_impl(a0, a1, a4, a5); }
// 0x1bf0b20 TextureDrawBufferEnd(ctx, stats) -> the popped handle (0 = none): pop; with a frame, the colour of the new top as target,
// viewport + scissor, projection, stats.
static uint64_t tde_impl(uint64_t ctx, uint64_t st) {
    const uint64_t old = vkn_rd64(ctx + 0x1b0);
    wr64(ctx + 0x1b0, old - 0x28);
    if (!vkn_rd32(ctx + 0xd8)) return 0;
    if (old >= ctx + 0x1b0 || !vkn_rd32(ctx + 0xdc)) return 0;
    targets_apply(ctx);
    vs_apply(ctx);
    proj(ctx);
    if (st) { wr16(st, (uint16_t)(vkn_rd16(st) | 0x20)); wr16(st + 0xc, (uint16_t)(vkn_rd16(st + 0xc) + 1)); }
    g_n_tde++;
    return vkn_rd64(old);
}
VKN_HOOK h_tde(VKN_A) { VKN_UNUSED; return tde_impl(a0, a1); }

// The filters' composite step (0x1bee5d0 colour matrix, 0x1beedc0 bevel / drop shadow; reached only from FilterQuad, so not hooked:
// FilterQuad calls these): a new target over the destination rectangle (TextureDrawBufferBegin, uid 0), one v2tc2 quad with the
// filter program, then the popped handle becomes the render state's tex[0] (+0x20).
static float node_uv(uint64_t ctx, uint64_t h, float *o) {   // the region node of handle h (find-or-insert) as the uv clamp rect
    const uint64_t node = ((hash_put_fn)eng(0x1bf0c40))(ctx + 0x25a8, h);
    const float fw = (float)(int32_t)vkn_rd32(ctx + 0xc0), fh = (float)(int32_t)vkn_rd32(ctx + 0xc4);
    if (!node) { o[0] = o[1] = 0.0f; o[2] = o[3] = 1.0f; return 0.0f; }
    o[0] = ((float)(int32_t)vkn_rd32(node + 0x10) - 0.5f) / fw; o[1] = ((float)(int32_t)vkn_rd32(node + 0x14) - 0.5f) / fh;
    o[2] = ((float)(int32_t)vkn_rd32(node + 0x18) + 0.5f) / fw; o[3] = ((float)(int32_t)vkn_rd32(node + 0x1c) + 0.5f) / fh;
    return 1.0f;
}
static void filter_composite(uint64_t ctx, uint64_t r, const int32_t dst[4], const float uv[4], int kind /* 1 matrix, 2 bevel, 3 shadow */, uint64_t st) {
    if (!tdb_impl(ctx, (uint64_t)(uintptr_t)dst, 0, st)) return;
    float vsc[32], fsc[12], fsc1[20]; memset(vsc, 0, sizeof vsc); memset(fsc, 0, sizeof fsc); memset(fsc1, 0, sizeof fsc1);
    vsc[0] = 1.0f; vsc[5] = 1.0f; memcpy(vsc + 20, (const void *)(uintptr_t)(ctx + 0x10), 48);
    const uint64_t h0 = vkn_rd64(r + 0x20);
    wr64(ctx + 0x90, h0);
    g_unit[0] = h0 ? vkn_rd64(h0) : 0;
    uint64_t rec;
    if (kind == 1) {
        g_unit[1] = g_unit[2] = g_unit[3] = vkn_rd64(ctx + 0x24c0);
        rec = ctx + 0x2140;
        memcpy(fsc1, (const void *)(uintptr_t)(r + 0xa0), 0x50);   // the 4x5 colour matrix
    } else {
        const uint64_t h1 = vkn_rd64(r + 0x28), h2 = vkn_rd64(r + 0x30);
        wr64(ctx + 0x98, h1); g_unit[1] = h1 ? vkn_rd64(h1) : 0;
        if (h2) { wr64(ctx + 0xa0, h2); g_unit[2] = vkn_rd64(h2); } else g_unit[2] = vkn_rd64(ctx + 0x24c0);
        g_unit[3] = vkn_rd64(ctx + 0x24c0);
        rec = ctx + (kind == 2 ? 0x600 : 0) + 0x60ull * vkn_rd8(r + 0xf) + 0x1180;   // 0x1beedc0: + (w4 & 1) * 0x600 + filter mode * 0x60
        const float fw = (float)(int32_t)vkn_rd32(ctx + 0xc0), fh = (float)(int32_t)vkn_rd32(ctx + 0xc4);
        node_uv(ctx, h0, fsc1); node_uv(ctx, h1, fsc1 + 4);
        memcpy(fsc1 + 8, (const void *)(uintptr_t)(r + 0xa0), 16); memcpy(fsc1 + 12, (const void *)(uintptr_t)(r + 0xc0), 16);
        fsc1[16] = -rdf(r + 0xb0) / fw; fsc1[17] = -rdf(r + 0xb4) / fh; fsc1[18] = rdf(r + 0xb8); fsc1[19] = 0.0f;
    }
    if (vkn_rd64(ctx + 0xa8) != rec) wr64(ctx + 0xa8, rec);
    const float q[16] = {(float)dst[0], (float)dst[1], uv[0], uv[1], (float)dst[2], (float)dst[1], uv[2], uv[1],
                         (float)dst[2], (float)dst[3], uv[2], uv[3], (float)dst[0], (float)dst[3], uv[0], uv[3]};
    vkn_ui_draw_in d; memset(&d, 0, sizeof d);
    draw_state(ctx, &d);
    d.vs = vkn_rd64(ctx + 0x23e8); d.vfmt = VKN_UI_VF_V2TC2; d.quads = 1; d.verts = q; d.nverts = 4;
    d.vsc = vsc; d.vsc_bytes = sizeof vsc; d.fsc = fsc; d.fsc_bytes = sizeof fsc; d.fsc1 = fsc1; d.fsc1_bytes = 0x50;
    if (ui_draw(&d)) g_n_comp_drop++; else g_n_comp++;
    wr64(r + 0x20, tde_impl(ctx, st));
}

// ---- engine texture updates after creation ----
// 0x9410f0 UpdateTexture(list, handle, source, w, h): RGBA8 rows of 4 * w bytes from (0, 0) of level 0. An immediate list (+0x30 == 0)
// writes the texture's storage now (mirrored here, after the original); a recording list emits packet 0x2c (mirrored by the model).
VKN_HOOK h_upd(VKN_A) {
    const uint64_t rc = g_o_upd(VKN_F);
    if (a0 && !vkn_rd8(a0 + 0x30) && a2 && (int32_t)a3 > 0 && (int32_t)a4 > 0) {
        vkn_tex *t = vkn_tex_get(a1);
        if (t) vkn_tex_update(t, 0, 0, 0, 0, (uint32_t)a3, (uint32_t)a4, (const void *)(uintptr_t)a2, (uint32_t)a3 * 4u);
        else g_n_upd_miss++;
        g_n_upd_eng++;
    }
    return rc;
}
// Packet 0x2c (+8 handle, +0x14 bytes a row, +0x18 rows, +0x1c u8 4 bytes a texel, rows from (packet + 0x2f) & ~15): mirrored, then
// the engine's handler runs as it is (returns 0: not executed here).
// The ExecuteRenderCommand hook (hle_dos2_native.c) calls the model on the guest stack: the texture update (staging, copies) runs on
// the host stack (hle_dos2_vkn_stack.c).
typedef struct { vkn_tex *t; uint32_t w, rows; const void *src; uint32_t pitch; } ui_upd_job;
static uint64_t ui_upd_on_stack(void *p) { const ui_upd_job *j = p; vkn_tex_update(j->t, 0, 0, 0, 0, j->w, j->rows, j->src, j->pitch); return 0; }
static int model_2c(uint64_t list, unsigned op, uint64_t pkt, uint64_t *len) {
    (void)list; (void)len;
    if (op != 0x2c || !pkt) return 0;
    const uint32_t pitch = vkn_rd32(pkt + 0x14), rows = vkn_rd32(pkt + 0x18), bpp = vkn_rd8(pkt + 0x1c) ? vkn_rd8(pkt + 0x1c) : 4;
    vkn_tex *t = vkn_tex_get(vkn_rd64(pkt + 8));
    if (t && pitch && rows) { ui_upd_job j = {t, pitch / bpp, rows, (const void *)(uintptr_t)((pkt + 0x2f) & ~15ull), pitch}; dm_vkn_on_stack_x(ui_upd_on_stack, &j, NULL, DM_VKN_SF_ENC); }   // ([stage 5.5] the update is a record)
    else g_n_upd_miss++;
    g_n_pkt2c++;
    return 0;
}

// ---- [stage 5.2 B2] the UI pass on tile GPUs (DM_VK_NATIVE_UIPASS, vkn_ui.h): one line, values a presented frame over the window ----
static vkn_ui_pass_stats g_ups0; static uint64_t g_ups_frame0;
static void ui_pass_census(void) {
    vkn_ui_pass_stats a; vkn_ui_pass_stats_get(&a);
    if (a.mode <= 0 && !a.fc_mode && !a.layer_mode && !a.cache_mode && !a.ui_calls) return;   // (off, or no device yet; [mgpu P2] the filter cache runs without the UI pass)
    const vkn_ui_pass_stats *p = &g_ups0; const uint64_t fr = vkn_frame();
    const double f = fr > g_ups_frame0 ? (double)(fr - g_ups_frame0) : 1.0, mb = 1048576.0;
#define D(x) ((double)(a.x - p->x) / f)
    const double pf = (double)(a.px_full - p->px_full), pa = (double)(a.px_area - p->px_area);
    vkn_logf("ui pass: DM_VK_NATIVE_UIPASS=%d%s rule %d (tile %d, area functions %s): a frame: UI passes %.2f (with a DS %.2f), DS-less begun %.2f, stayed %.2f "
             "(%.1f MB load+store saved), mid-run reopens with DS %.3f; area hinted %.2f, applied %.2f, ignored by the pass %.2f, restarts %.3f, hints dropped %.3f, "
             "pixels %.2f of %.2f M (%.1f %%, %.1f MB saved); fences: re-attach %.3f, narrowed pass ended %.3f, before an engine draw %.3f; set changed under the UI %.3f; "
             "refused DS: history %.2f, last frame %.2f, shared set %.2f, size %.2f, dying %.2f; area: no pass functions %.2f, clear load %.2f, multisample %.2f, saving < 10%% %.2f",
             a.mode, a.verify ? " VERIFY (decisions counted, old targets drawn)" : "", a.rule, a.tile, a.api ? "present" : "absent", D(epochs), D(epochs_ds), D(detached), D(kept_off),
             D(ds_bytes) / mb, D(reattach), D(area_hinted), D(area_applied), D(area_ignored), D(restarts), D(hint_dropped), pa / f / 1e6, pf / f / 1e6, pf > 0 ? 100.0 * pa / pf : 100.0,
             D(area_bytes) / mb, D(fence_reattach), D(fence_end), D(fence_draw), D(set_lost), D(ds_refused[VKN_UIP_DS_HIST]), D(ds_refused[VKN_UIP_DS_FRAME]),
             D(ds_refused[VKN_UIP_DS_SHARED]), D(ds_refused[VKN_UIP_DS_SIZE]), D(ds_refused[VKN_UIP_DS_DYING]), D(area_refused[VKN_UIP_AR_API]), D(area_refused[VKN_UIP_AR_CLEAR]),
             D(area_refused[VKN_UIP_AR_MS]), D(area_refused[VKN_UIP_AR_SAVE]));
    if (a.verify) vkn_logf("ui pass verify: a frame: would detach %.2f, of them would re-attach %.3f (%.1f MB saved); would narrow %.2f, would restart %.3f (%.1f MB saved)",
                           D(w_detach), D(w_reattach), D(w_ds_bytes) / mb, D(w_area), D(w_restart), D(w_area_bytes) / mb);
    // [mgpu P1] / [mgpu P2]
    vkn_logf("ui region / filter cache (mgpu): DM_VK_NATIVE_UIREGION=%d: a frame: region epochs %.2f (%.3f M pixels), colour draws clipped %.2f, rules dropped %.3f, refused %.3f, poisoned %.2f; "
             "verify: would %.2f, would clip %.2f, would drop %.3f | DM_VK_NATIVE_UIFC=%d: epochs seen %.2f, hits %.2f, misses %.2f, stored %.2f, draws saved %.1f, pixels copied %.3f M, evicted %.3f; "
             "refused: not a region epoch %.2f, a draw not cacheable %.2f, a texture without a version %.2f, too many draws %.2f, DS use %.2f, cache full %.2f, other %.2f; "
             "verify compares %.3f, differ %llu (total); poison %.3f; held calls flushed by: targets %.2f, a fence %.2f, the engine %.2f, the recording end %.2f, other %.2f",
             a.region_mode, D(region_epochs), D(region_px) / 1e6, D(region_clipped), D(region_dropped), D(region_refused), D(region_poison), D(w_region), D(w_region_clip), D(w_region_drop),
             a.fc_mode, D(fc_epochs), D(fc_hits), D(fc_misses), D(fc_stored), D(fc_draws_saved), D(fc_px_copied) / 1e6, D(fc_evicted), D(fc_refused[0]), D(fc_refused[1]), D(fc_refused[2]),
             D(fc_refused[3]), D(fc_refused[4]), D(fc_refused[5]), D(fc_refused[6]), D(fc_verify), (unsigned long long)a.fc_differ, D(fc_poison),
             D(fc_flush[0]), D(fc_flush[1]), D(fc_flush[2]), D(fc_flush[3]), D(fc_flush[4]));
    {   // [mgpu P7] the UI's traffic (the pass journal's UI share), CPU and the layer hold / cache: one line to compare before / after
        vkn_pass_stats4 q; vkn_pass_stats4_get(&q);
        static vkn_pass_stats4 q0;
        const double qf = q.frames > q0.frames ? (double)(q.frames - q0.frames) : 1.0;
#define Q(x) ((double)(q.x - q0.x) / qf)
        vkn_logf("ui traffic (mgpu P7): a presented frame: UI passes %.2f (draws %.1f), model %.1f MB load + %.1f MB store, copies %.2f (%.1f MB); "
                 "UI CPU on the recording thread %.3f ms (%.0f calls) | DM_VK_NATIVE_UILAYER=%d: runs %.2f, calls held %.1f (draws %.1f), executed hoisted %.2f / in order %.2f, "
                 "not hoisted: no layer %.2f, pushed DS %.2f, pushed read of the layer %.2f, target reuse %.2f, many reads %.2f; poisoned %.2f; flushed by: an engine draw on GDraw's binding %.2f, "
                 "a conflict %.2f, the layer used %.2f, a pass end %.2f, size %.2f, other %.2f | DM_VK_NATIVE_UICACHE=%d: candidates %.2f, would hit %.2f, hits %.2f (draws not executed %.1f, calls %.1f); "
                 "refused: no layer %.2f, a texture without a version %.2f, vertex range %.2f, many targets %.2f, drawn before cleared %.2f, no layer content %.2f, no layer clear %.2f, "
                 "first %.2f, written since %.2f, another key %.2f; verify compares %.3f, differ %llu (total); poison %.3f",
                 Q(ui_passes), Q(ui_pass_draws), Q(ui_load) / mb, Q(ui_store) / mb, Q(ui_copies), Q(ui_copy) / mb, D(ui_ns) / 1e6, D(ui_calls),
                 a.layer_mode, D(lh_runs), D(lh_calls), D(lh_draws), D(lh_hoisted), D(lh_ordered), D(lh_unsafe[VKN_UILU_NOLAYER]), D(lh_unsafe[VKN_UILU_DS]), D(lh_unsafe[VKN_UILU_READL]),
                 D(lh_unsafe[VKN_UILU_REUSE]), D(lh_unsafe[VKN_UILU_MANY]), D(lh_poison), D(lh_flush[VKN_UILF_BOUND]), D(lh_flush[VKN_UILF_CONFLICT]), D(lh_flush[VKN_UILF_LAYER]),
                 D(lh_flush[VKN_UILF_END]), D(lh_flush[VKN_UILF_SIZE]), D(lh_flush[VKN_UILF_OTHER]), a.cache_mode, D(uc_cand), D(uc_would), D(uc_hits), D(uc_draws_saved), D(uc_calls_saved),
                 D(uc_refused[VKN_UICR_NOLAYER]), D(uc_refused[VKN_UICR_TEX]), D(uc_refused[VKN_UICR_VB]), D(uc_refused[VKN_UICR_MANY]), D(uc_refused[VKN_UICR_INIT]), D(uc_refused[VKN_UICR_NOL]),
                 D(uc_refused[VKN_UICR_NOCLEAR]), D(uc_refused[VKN_UICR_FIRST]), D(uc_refused[VKN_UICR_DIRTY]), D(uc_refused[VKN_UICR_KEY]), D(uc_verify), (unsigned long long)a.uc_differ, D(uc_poison));
#undef Q
        q0 = q;
    }
    // [mgpu P7b] what flushes the runs, how they run: renames, the pushed DS check, the layer pass's run-level DS / area
    vkn_logf("ui runs (mgpu P7b): a presented frame: flushed by the engine's draw %.2f, depth read %.2f, clear %.2f, sample %.2f, layout %.2f, copy %.2f, storage %.2f, "
             "pass end %.2f; touching the layer %.2f, a depth-stencil %.2f, a pushed target %.2f, a texture the run reads %.2f, nothing of the run %.2f | hoisted with the layer "
             "alone %.2f; pushed targets renamed %.2f (refused: no texture free %.3f), spares made %llu (%.1f MB alive), aliases at a recording end %.2f",
             D(lh_fev[VKN_PBE_READY]), D(lh_fev[VKN_PBE_DEPTH]), D(lh_fev[VKN_PBE_CLEAR]), D(lh_fev[VKN_PBE_SAMPLE]), D(lh_fev[VKN_PBE_LAYOUT]), D(lh_fev[VKN_PBE_COPY]),
             D(lh_fev[VKN_PBE_STORAGE]), D(lh_fev[VKN_PBE_END]), D(lh_ftex[VKN_UILT_LAYER]), D(lh_ftex[VKN_UILT_DS]), D(lh_ftex[VKN_UILT_PUSHED]), D(lh_ftex[VKN_UILT_READ]),
             D(lh_ftex[VKN_UILT_NONE]), D(lh_layer_only), D(lh_renamed), D(lh_rename_fail), (unsigned long long)a.lh_spares, (double)a.lh_spare_bytes / mb, D(lh_alias_live));
    vkn_logf("ui runs 2 (mgpu P7b): a presented frame: pushed DS use order-free %.2f, not %.2f; segments more %.2f (by the DS %.2f, a pushed read of the layer %.2f) | "
             "layer DS from the run: detached %.2f, kept %.2f; the engine draws into the layer next %.2f; run area: passes %.2f, pixels %.2f of %.2f M, restarts %.3f | "
             "DM_VK_NATIVE_UILAYER=2: runs drawn twice and compared %.2f, differ %llu (total)",
             D(lh_ds_ok), D(lh_ds_bad), D(lh_segs), D(lh_seg_ds), D(lh_seg_rl), D(lh_g0_det), D(lh_g0_keep), D(lh_cont), D(lh_area), D(lh_area_px) / 1e6, D(lh_area_full) / 1e6,
             D(lh_area_restart), D(lh_vdouble), (unsigned long long)a.lh_vdiffer);
    // [mgpu P7c] segments the hold started at a reuse; filter-cache hits answered by a page (no copy, no binding); pushed bindings never
    // made (never used) / made at first use; region epochs whose DS writes are dead after them (DS draws clipped)
    vkn_logf("ui runs 3 (mgpu P7c): a presented frame: segments started at a reuse %.2f | filter cache pages %llu (%.1f MB alive), hits by a page %.2f (verified ones then read from it %.2f), page regions made "
             "real %.3f, pages off by a self-check %llu (total) | pushed bindings never made %.2f, made at first use %.2f | region epochs with a DS use: dead after %.2f, "
             "not %.2f; DS draws clipped %.2f | regions grown by the guard band %.2f, self-check differences only at the edge %llu (total)",
             D(lh_seg_reuse), (unsigned long long)a.fc_pages, (double)a.fc_page_bytes / mb, D(fc_rd_hits), D(fc_rd_verify), D(fc_rd_real), (unsigned long long)a.fc_page_off, D(lh_lazy_skipped),
             D(lh_lazy_bound), D(lh_dsclip), D(lh_dsclip_no), D(region_dsclip), D(fc_guard), (unsigned long long)a.fc_diff_edge);
#undef D
    g_ups0 = a; g_ups_frame0 = fr;
}

// ---- census ----
static void ui_census(void) {
    vkn_ui_stats s; vkn_ui_stats_get(&s);
    vkn_tex_update_stats u; vkn_tex_update_stats_get(&u);
    vkn_logf("ui: draws %llu (dropped here %llu; vkn_ui: %llu drawn = quads %llu + indexed %llu, %llu vertices, ring %.1f MB), filter quads %llu (dropped %llu), "
             "filters (blur %llu: passes %llu, dropped %llu; colour matrix %llu, bevel %llu, drop shadow %llu: composites %llu, dropped %llu; other %llu), special blends without the destination %llu; "
             "targets %llu (unknown texture %llu; push %llu / fail %llu, pop %llu), RenderingBegin %llu / End %llu (table %llu), clears %llu (depth-stencil %llu, masked %llu, colour %llu), custom draws %llu",
             (unsigned long long)g_n_draws, (unsigned long long)g_n_draw_drop, (unsigned long long)s.draws, (unsigned long long)s.quads, (unsigned long long)s.indexed,
             (unsigned long long)s.vertices, s.ring_bytes / 1048576.0, (unsigned long long)g_n_fq, (unsigned long long)g_n_fq_drop, (unsigned long long)g_n_filter[0],
             (unsigned long long)g_n_blur, (unsigned long long)g_n_blur_drop, (unsigned long long)g_n_filter[1], (unsigned long long)g_n_filter[2], (unsigned long long)g_n_filter[3], (unsigned long long)g_n_comp, (unsigned long long)g_n_comp_drop,
             (unsigned long long)g_n_filter[4],
             (unsigned long long)g_n_special_dst, (unsigned long long)s.targets, (unsigned long long)g_n_tgt_unknown, (unsigned long long)g_n_tdb, (unsigned long long)g_n_tdb_fail, (unsigned long long)g_n_tde,
             (unsigned long long)g_n_rb, (unsigned long long)g_n_re, (unsigned long long)g_n_table, (unsigned long long)g_n_clear, (unsigned long long)s.clears_ds,
             (unsigned long long)s.clears_masked, (unsigned long long)s.clears_color, (unsigned long long)g_n_cdb);
    if (g_ui_off) vkn_logf("ui: DM_VK_NATIVE_UI=0: %llu GDraw draws not recorded", (unsigned long long)g_n_ui_off);
    ui_pass_census();
    vkn_logf("ui: dropped draws by reason: no recording %llu, no pass %llu, no shader %llu, binding %llu, pipeline %llu, set %llu, ring %llu, vertex range %llu, empty %llu, "
             "no native vertex buffer %llu; identical-state draws %llu, tex0 mode without texture %llu; pairs %llu (failed %llu), pipelines %llu (failed %llu), sets %llu in %llu pools, "
             "feedback %llu, placeholder %llu",
             (unsigned long long)s.drop_cb, (unsigned long long)s.drop_pass, (unsigned long long)s.drop_shader, (unsigned long long)s.drop_bind, (unsigned long long)s.drop_pipe,
             (unsigned long long)s.drop_set, (unsigned long long)s.drop_ring, (unsigned long long)s.drop_vb, (unsigned long long)s.drop_empty, (unsigned long long)g_n_no_vb,
             (unsigned long long)g_n_identical, (unsigned long long)g_n_tex0_null, (unsigned long long)s.pairs, (unsigned long long)s.pair_fail, (unsigned long long)s.pipelines,
             (unsigned long long)s.pipe_fail, (unsigned long long)s.sets, (unsigned long long)s.pools, (unsigned long long)s.feedback, (unsigned long long)s.placeholder);
    {   // [rt P6]
        uint64_t h = 0, m = 0, w = 0; vkn_ui_set_cache_stats(&h, &m, &w);
        static uint64_t oh, om, ow;
        vkn_logf("ui: set cache (DM_VK_NATIVE_UISET): reused %llu, new %llu, would reuse %llu (since the last line)", (unsigned long long)(h - oh), (unsigned long long)(m - om), (unsigned long long)(w - ow));
        oh = h; om = m; ow = w;
    }
    vkn_logf("ui: programs %llu (bad %llu); GDraw textures made %llu (failed %llu, freed %llu), vertex buffers %llu (failed %llu, freed %llu); updates: antialias %llu, "
             "MakeTextureEnd %llu, UpdateTextureRect %llu, engine immediate %llu, packet 0x2c %llu, without a native texture %llu; vkn_tex_update recorded %llu, queued %llu, "
             "%.1f MB, dropped (dead %llu, rectangle %llu, format %llu, memory %llu)",
             (unsigned long long)g_n_prog, (unsigned long long)g_n_prog_bad, (unsigned long long)g_n_tex, (unsigned long long)g_n_tex_fail, (unsigned long long)g_n_tex_free,
             (unsigned long long)g_n_vb, (unsigned long long)g_n_vb_fail, (unsigned long long)g_n_vb_free, (unsigned long long)g_n_aa, (unsigned long long)g_n_mte,
             (unsigned long long)g_n_utr, (unsigned long long)g_n_upd_eng, (unsigned long long)g_n_pkt2c, (unsigned long long)g_n_upd_miss, (unsigned long long)u.recorded,
             (unsigned long long)u.queued, u.bytes / 1048576.0, (unsigned long long)u.dropped_dead, (unsigned long long)u.dropped_rect, (unsigned long long)u.dropped_fmt,
             (unsigned long long)u.dropped_mem);
}

void dm_vkn_ui_register(void);
void dm_vkn_ui_register(void) {
    if (vkn_mode() != VKN_NATIVE) return;   // mode 2 stays exactly as it was
    { const char *e = getenv("DM_VK_NATIVE_UI"); g_ui_off = e && *e == '0'; }
    if (g_ui_off) vkn_logf("ui: DM_VK_NATIVE_UI=0: GDraw draws are counted, not recorded (the 3D scene alone; targets and clears as usual)");
    {   // [stage 5.2 B2] DM_VK_NATIVE_UIPASS (unset = the family default: 2 on Mali / PowerVR and their DM_GFX_SIM_GPU models, 0 elsewhere),
        // _VERIFY, _RULE (vkn_ui.h). The family needs the device: vkn_ui resolves it at its first use and prints its start line then.
        const char *e = getenv("DM_VK_NATIVE_UIPASS"), *v = getenv("DM_VK_NATIVE_UIPASS_VERIFY"), *r = getenv("DM_VK_NATIVE_UIPASS_RULE"), *sim = getenv("DM_GFX_SIM_GPU");
        int sim_tiler = 0;   // the tile models of xbg_sim_parse: mali, powervr, pvr, family-mali, family-powervr (tokens split by , ; + space)
        for (const char *q = sim; q && *q;) {
            while (*q == ',' || *q == ';' || *q == '+' || *q == ' ') q++;
            const char *t = q; while (*q && *q != ',' && *q != ';' && *q != '+' && *q != ' ') q++;
            const size_t n = (size_t)(q - t);
            static const char *const k_tile[] = {"mali", "powervr", "pvr", "family-mali", "family-powervr"};
            for (unsigned i = 0; i < sizeof k_tile / sizeof k_tile[0]; i++) if (n == strlen(k_tile[i]) && !strncmp(t, k_tile[i], n)) sim_tiler = 1;
        }
        { const char *rg = getenv("DM_VK_NATIVE_UIREGION"); vkn_ui_region_config(rg && *rg ? atoi(rg) : -1); }   // [mgpu P1] (-1 = render-pass tilers 1, else 0)
        { const char *fc = getenv("DM_VK_NATIVE_UIFC"); vkn_ui_fc_config(fc && *fc ? atoi(fc) : -1); }          // [mgpu P2]
        { const char *fg = getenv("DM_VK_NATIVE_UIFC_GUARD"); vkn_ui_fc_guard_config(fg && *fg ? atoi(fg) : -1); }   // [mgpu P7e] (-1 = tile families 1)
        { const char *l = getenv("DM_VK_NATIVE_UILAYER"), *c = getenv("DM_VK_NATIVE_UICACHE");                  // [mgpu P7] (-1 = render-pass tilers 1, else 0)
          vkn_ui_layer_config(l && *l ? atoi(l) : -1, c && *c ? atoi(c) : -1); }
        vkn_ui_pass_config(e && *e ? atoi(e) : -1, v && *v ? atoi(v) : 0, r && *r ? atoi(r) : 0, sim_tiler);
    }
    {   // [rt P6] DM_VK_NATIVE_UISET (vkn_ui.h): 0 a fresh set per UI draw, 1 (default) sets of the recording by content, 2 = 1 without reuse (counted)
        const char *e = getenv("DM_VK_NATIVE_UISET"); const int m = e && *e ? atoi(e) : 1;
        vkn_ui_set_cache(m);
        vkn_logf("ui: DM_VK_NATIVE_UISET=%d%s", m, e && *e ? "" : " (default)");
    }
    g_re_stub = dm_vkn_stack_stub_ex((void *)ui_rendering_end, "GDraw RenderingEnd", DM_VKN_SF_ENC);   // ([stage 5.5] its scissor is a record)
    if (!g_re_stub) vkn_logf(RED "ui: no host-stack stub for RenderingEnd: the GDraw table keeps the game's 0x1bf4200 (NVN)");
    // First 4 instructions of each site (python <scratchpad>/vk/re/pro.py; none PC-relative, checked with capstone).
    static const uint32_t k_prog[4] = {0xd10203ff /* sub sp,sp,#0x80 */, 0xa9026ffc, 0xa90367fa, 0xa9045ff8};
    static const uint32_t k_tex[4] = {0xd10403ff /* sub sp,sp,#0x100 */, 0xa90b67fa, 0xa90c5ff8, 0xa90d57f6};
    static const uint32_t k_rt[4] = {0xd10383ff /* sub sp,sp,#0xe0 */, 0xa9086ffc, 0xa90967fa, 0xa90a5ff8};
    static const uint32_t k_fb[4] = {0xa9ba6ffc /* stp x28,x27,[sp,#-0x60]! */, 0xa90167fa, 0xa9025ff8, 0xa90357f6};
    static const uint32_t k_aa[4] = {0xd101c3ff /* sub sp,sp,#0x70 */, 0xf90023f5, 0xa9054ff4, 0xa9067bfd};
    static const uint32_t k_mte[4] = {0xd10183ff /* sub sp,sp,#0x60 */, 0xa9025ff8, 0xa90357f6, 0xa9044ff4};
    static const uint32_t k_mve[4] = {0xf81b0ff9 /* str x25,[sp,#-0x50]! */, 0xa9015ff8, 0xa90257f6, 0xa9034ff4};
    static const uint32_t k_utr[4] = {0xd10443ff /* sub sp,sp,#0x110 */, 0xa90b6ffc, 0xa90c67fa, 0xa90d5ff8};
    static const uint32_t k_rb[4] = {0xd100c3ff /* sub sp,sp,#0x30 */, 0xa9014ff4, 0xa9027bfd, 0x910083fd};
    static const uint32_t k_cdb[4] = {0xa9bd57f6 /* stp x22,x21,[sp,#-0x30]! */, 0xa9014ff4, 0xa9027bfd, 0x910083fd};
    static const uint32_t k_vs[4] = {0xf81e0ff3 /* str x19,[sp,#-0x20]! */, 0xa9017bfd, 0x910043fd, 0xb940f808};
    static const uint32_t k_clr[4] = {0xa9be4ff4 /* stp x20,x19,[sp,#-0x20]! */, 0xa9017bfd, 0x910043fd, 0xf9403c08};
    static const uint32_t k_draw[4] = {0xd102c3ff /* sub sp,sp,#0xb0 */, 0xa9056ffc, 0xa90667fa, 0xa9075ff8};
    static const uint32_t k_filter[4] = {0xd10303ff /* sub sp,sp,#0xc0 */, 0xa9066ffc, 0xa90767fa, 0xa9085ff8};
    static const uint32_t k_blur[4] = {0xd10203ff /* sub sp,sp,#0x80 */, 0xa9026ffc, 0xa90367fa, 0xa9045ff8};
    static const uint32_t k_tdb[4] = {0xd10283ff /* sub sp,sp,#0xa0 */, 0xa9046ffc, 0xa90567fa, 0xa9065ff8};
    static const uint32_t k_tde[4] = {0xf81d0ff5 /* str x21,[sp,#-0x30]! */, 0xa9014ff4, 0xa9027bfd, 0x910083fd};
    static const uint32_t k_upd[4] = {0xd10203ff /* sub sp,sp,#0x80 */, 0xa9026ffc, 0xa90367fa, 0xa9045ff8};
    dm_vkn_site(0x1beba70, k_prog, "GDraw program", (void *)h_prog, &g_o_prog, VKN_M_NATIVE);
    dm_vkn_site(0x1bf2c60, k_tex, "GDraw CreateTexture", (void *)h_tex, &g_o_tex, VKN_M_NATIVE);
    dm_vkn_site(0x1bf2860, k_rt, "GDraw CreateRenderTarget", (void *)h_rt, &g_o_rt, VKN_M_NATIVE);
    dm_vkn_site(0x1bec540, k_fb, "GDraw FrameBegin", (void *)h_fb, &g_o_fb, VKN_M_NATIVE);
    dm_vkn_site(0x1bedc80, k_aa, "GDraw SetAntialiasTexture", (void *)h_aa, &g_o_aa, VKN_M_NATIVE);
    dm_vkn_site(0x1bf1a10, k_mte, "GDraw MakeTextureEnd", (void *)h_mte, &g_o_mte, VKN_M_NATIVE);
    dm_vkn_site(0x1bf1f60, k_mve, "GDraw MakeVertexBufferEnd", (void *)h_mve, &g_o_mve, VKN_M_NATIVE);
    dm_vkn_site(0x1bf24e0, k_utr, "GDraw UpdateTextureRect", (void *)h_utr, &g_o_utr, VKN_M_NATIVE);
    dm_vkn_site(0x1becd80, k_rb, "GDraw RenderingBegin", (void *)h_rb, &g_o_rb, VKN_M_NATIVE);
    dm_vkn_site(0x1becc20, k_cdb, "GDraw custom-draw begin", (void *)h_cdb, &g_o_cdb, VKN_M_NATIVE);
    dm_vkn_site(0x1becf00, k_vs, "GDraw viewport + scissor", (void *)h_vs, &g_o_vs, VKN_M_NATIVE);
    dm_vkn_site(0x1bedb70, k_clr, "GDraw ClearDepthStencil", (void *)h_clr, &g_o_clr, VKN_M_NATIVE);
    dm_vkn_site(0x1bf0cd0, k_draw, "GDraw DrawIndexedTriangles", (void *)h_draw, &g_o_draw, VKN_M_NATIVE);
    dm_vkn_site(0x1bedd60, k_filter, "GDraw FilterQuad", (void *)h_filter, &g_o_filter, VKN_M_NATIVE);
    dm_vkn_site(0x1bed390, k_blur, "GDraw BlurPass", (void *)h_blur, &g_o_blur, VKN_M_NATIVE);
    dm_vkn_site(0x1befd00, k_tdb, "GDraw TextureDrawBufferBegin", (void *)h_tdb, &g_o_tdb, VKN_M_NATIVE);
    dm_vkn_site(0x1bf0b20, k_tde, "GDraw TextureDrawBufferEnd", (void *)h_tde, &g_o_tde, VKN_M_NATIVE);
    dm_vkn_site(0x9410f0, k_upd, "UpdateTexture", (void *)h_upd, &g_o_upd, VKN_M_NATIVE);
    {   // [stage 5.5] the per-frame GDraw calls encode their backend work (vkn_rt_ui_*; FrameBegin owns the recording only to free): no
        // default sync. Creations and vertex buffers keep it (CreateTexture / RenderTarget, MakeVertexBufferEnd).
        // [stage 5.5] texture updates are records too (vkn_tex_update: the staging made on this thread): SetAntialiasTexture,
        // MakeTextureEnd, UpdateTextureRect, UpdateTexture encode
        void *const enc[] = {(void *)h_prog, (void *)h_fb, (void *)h_rb, (void *)h_cdb, (void *)h_vs, (void *)h_clr, (void *)h_draw, (void *)h_filter, (void *)h_blur,
                             (void *)h_tdb, (void *)h_tde, (void *)h_aa, (void *)h_mte, (void *)h_utr, (void *)h_upd};
        for (size_t i = 0; i < sizeof enc / sizeof enc[0]; i++) dm_vkn_stack_flag(enc[i], DM_VKN_SF_ENC);
    }
    vkn_tex_update_init();
    // packet 0x2c: the executor hook exists once the exec part installed its opcode handler (registered before this part)
    if (dn_install_rc_model(1ull << 0x2c, model_2c)) vkn_logf(RED "ui: no model slot for packet 0x2c (executor hook missing or slot taken): recorded texture rows stay on the old storage");
    dm_vkn_census(ui_census);
}
#endif
