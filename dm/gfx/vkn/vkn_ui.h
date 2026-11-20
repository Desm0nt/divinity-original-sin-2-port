// [DOS2] VK-NATIVE stage 3, the UI (vkn_ui.c): Iggy's GDraw backend (written by the game directly against NVN, main+0x1bea840..
// 0x1bf4338) drawn by the native backend into the same recording as the engine (vkn_frame.h) and through the same render-pass
// bookkeeping (vkn_pass.h). dm/host/hle_dos2_vkn_ui.c replaces every GDraw function that reaches an NVN command-buffer call, keeps
// GDraw's own records and caches as the originals write them, and hands this module plain values: program objects (opaque u64 keys)
// with their control / code, textures as vkn_tex, constants and vertices as CPU bytes or arena blocks, the GDraw state indices.
// GDraw's fixed objects (built once by 0x1bea840 and only ever selected by index):
//   blend 0..5    0 none, 1 alpha (ONE, ONE_MINUS_SRC_ALPHA), 2 multiply (DST_COLOR, ONE_MINUS_SRC_ALPHA), 3 add (ONE, ONE), 4 filter
//                 (none), 5 special (none: the special programs blend in the shader); equations add, alpha factors = colour factors;
//   depth-stencil 0..7 = (state address - 0x508) / 8: bit 0 stencil test, bit 1 depth test LESS, bit 2 depth write (ALWAYS when bit 1
//                 is clear); stencil: func EQUAL against reference 0xff under the value mask, pass REPLACE, fail / depth fail KEEP,
//                 both faces, value mask and write mask per draw (SetStencilValueMask / SetStencilMask);
//   channel mask  all on / all off (a draw that only writes stencil);
//   vertex formats v2 (stride 8: location 0 RG32F), v2aa (16: + location 1 RGBA16I at 8), v2tc2 (16: + location 1 RG32F at 8);
//   sampler       every texture unit uses GDraw's sampler +0x228: clamp to edge, linear, linear mipmaps;
//   programs      one NVN program object per stage (0x1beba70); a draw binds one vertex and one fragment program; uniform block 0
//                 of each stage (0x80 vertex / 0x30 fragment bytes from the engine's ring); texture units 0, 1, 2 and 7.
// A vertex + fragment program pair becomes one vkn_shader (vkn_shader.h) under a handle of this module's own (index 0xff00 + pair,
// generation 1: far above the engine's shader indices), which gives the modules, the set / pipeline layouts and the binding table.
// Pipelines and descriptor sets are this module's own (a draw binds its pipeline and set; vkn_draw_raw_begin() afterwards makes the
// engine's next draw rebind everything). Single-threaded: every call except vkn_ui_program_put comes from the recording thread.
// No game addresses, no engine record offsets.
#ifndef DM_VKN_UI_H
#define DM_VKN_UI_H
#include "vkn.h"
#include "vkn_tex.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_UI_VF_V2 = 0, VKN_UI_VF_V2AA = 1, VKN_UI_VF_V2TC2 = 2, VKN_UI_VFS = 3 };
enum { VKN_UI_UNITS = 4 };   // texture units 0, 1, 2, 7 of the GDraw programs, in this order
enum { VKN_UI_CTL = 0xe48 }; // control bytes vkn_shader reads (a shorter control section is zero-padded by the caller)

// A GDraw program object (0x1beba70): stage 0 vertex / 1 fragment, its control section (VKN_UI_CTL bytes, copied) and code (CPU
// pointer of the code the program got; read when its pair is created, kept by GDraw for its lifetime). Any thread. 0 = known.
int vkn_ui_program_put(uint64_t prog, int stage, const void *control, const void *code);

// The current GDraw target set: one colour target and the depth-stencil target (either may be NULL). vkn_pass_targets underneath.
void vkn_ui_targets(vkn_tex *color, vkn_tex *depth);
// Viewport and scissor of the next draws (pixels, upper-left origin as NVN's and Vulkan's).
void vkn_ui_viewport(int32_t x, int32_t y, int32_t w, int32_t h);
void vkn_ui_scissor(int32_t x, int32_t y, int32_t w, int32_t h);
// nvnCommandBufferClearDepthStencil(depth 1.0, depth mask, stencil 0, stencil mask) on the bound depth target (vkn_pass_depth): depth when
// `depth`; stencil bits `mask` to 0 (a full mask is a clear, a partial one a stencil-only draw over the whole target).
void vkn_ui_clear_ds(int depth, uint8_t mask);
// A rectangle of colour target t to transparent black (TextureDrawBufferBegin's clearing quad: blend none, no depth / stencil).
void vkn_ui_clear_color(vkn_tex *t, const int32_t rect[4]);

typedef struct vkn_ui_draw_in {
    uint64_t vs, fs;                    // program objects (vkn_ui_program_put)
    uint8_t vfmt;                       // VKN_UI_VF_*
    uint8_t quads;                      // 1: nverts vertices, 4 a quad (NVN QUADS, no indices); 0: nidx u16 indices, triangles
    uint8_t blend, ds, color_off;       // GDraw state indices (header)
    uint8_t stencil_cmp, stencil_write; // value mask / write mask (reference 0xff); used when ds bit 0
    vkn_tex *tex[VKN_UI_UNITS];         // units 0, 1, 2, 7; NULL = this module's placeholder (1x1 transparent black)
    const void *vsc, *fsc;              // uniform block 0 of the vertex / fragment stage (CPU bytes, copied into the ring)
    uint32_t vsc_bytes, fsc_bytes;
    const void *fsc1; uint32_t fsc1_bytes;   // uniform block 1 of the fragment stage (the blur pass: taps), NULL = zeros
    const void *verts; const uint16_t *idx;   // CPU vertices / indices (copied into the ring) when vb is NULL
    const vkn_block *vb; uint64_t vb_off, ib_off;   // or an arena block (GDraw vertex buffer): byte offsets of vertices / indices
    uint32_t nverts, nidx;
} vkn_ui_draw_in;
// Records the draw (pass opened by vkn_pass_ready, textures made sampleable by vkn_pass_sample first). 0 = drawn; 1 = dropped
// (counted by reason; the first 16 of each reason print a red line).
int vkn_ui_draw(const vkn_ui_draw_in *in);

typedef struct vkn_ui_stats {
    uint64_t draws, quads, indexed, vertices, ring_bytes, targets, clears_ds, clears_masked, clears_color;
    uint64_t pipelines, pipe_fail, pairs, pair_fail, sets, pools, feedback, placeholder;
    uint64_t drop_cb, drop_pass, drop_shader, drop_bind, drop_pipe, drop_set, drop_ring, drop_vb, drop_empty, drop_prog;
} vkn_ui_stats;
void vkn_ui_stats_get(vkn_ui_stats *s);

#ifdef __cplusplus
}
#endif
#endif
