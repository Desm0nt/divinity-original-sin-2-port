// [DOS2] VK-NATIVE stage 3, draws (vkn_draw.c): a draw of the engine as Vulkan commands in the open recording.
// The engine keeps the whole state of a draw in its renderer's cache (handles of shader / states / buffers, slot arrays of texture
// and sampler pool ids, viewport, scissor): dm/host/hle_dos2_vkn_draw.c reads that cache at the draw and passes it here as plain
// values (vkn_draw_in). Nothing is "bound" by events: the cache is the one truth, this module compares with what it bound last in
// this recording and redoes only what changed:
//   pipeline       key of small ids (vkn_state.h vkn_pipe_key) from shader, states, vertex format, primitive, stencil reference,
//                  alpha variant, the pass's compatibility id and write masks; vkn_pipe_get on a change;
//   descriptor set key = shader set layout + (view, sampler) of every texture binding of the shader's table (vkn_shader.h vkn_bind)
//                  + buffer identity of its uniform bindings; a cache of sets (pools of this module, cleared by generation when a
//                  view or sampler dies); uniform block offsets are DYNAMIC offsets (not in the key) when the shader's policy allows;
//   uniform blocks the constant flushes of the engine (their host replacements) upload a block into the ring (vkn_buf.h
//                  vkn_ring_alloc) and tell this module (vkn_draw_ubo): like the engine's NVN binds they persist across shader
//                  changes until the next upload of that (stage, slot);
//   vertex / index buffers (arena buffer, offset) of the engine handle (vkn_buf.h); binding 3 = 16 constant bytes {0, 0, 0, 1.0f}
//                  for shader inputs the vertex format lacks (vkn_state.h vkn_pipe_src.vs_inputs);
//   viewport / scissor dynamic state, set when changed.
// A draw whose shader has a missing / unusable stage, an unknown state object or a failed pipeline is dropped and counted (as the
// translator drops draws without a bundle). Single-threaded (the recording thread). No game addresses.
#ifndef DM_VKN_DRAW_H
#define DM_VKN_DRAW_H
#include "vkn.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_STAGE_SLOTS = 3, VKN_TEX_SLOTS = 32, VKN_UBO_SLOTS = 16 };   // stage order here: 0 vertex, 1 fragment, 2 compute (engine stage 5)
typedef struct vkn_draw_in {
    uint64_t shader, blend, ds, raster, vfmt;      // engine handles (the renderer's cache)
    uint64_t vb[3]; uint32_t vb_off[3];            // vertex buffers of streams 0..2 (0 = none) and their byte offsets
    uint64_t ib;                                   // index buffer (indexed draws)
    uint8_t prim;                                  // engine primitive 1..7
    uint8_t stencil_ref;
    float alpha_ref;                               // < 0: no alpha test; else the test GREATER than this reference (fragment variant)
    const int32_t *tex[VKN_STAGE_SLOTS];           // 32 texture-pool ids per stage (-1 = empty); NULL = stage has none
    const int32_t *smp[VKN_STAGE_SLOTS];           // 32 sampler-pool ids per stage (as the engine's flush pairs them)
    float viewport[4];                             // x, y, w, h (upper-left origin, depth 0..1: Vulkan's own conventions)
    int32_t scissor[4];                            // x, y, w, h
} vkn_draw_in;

// A constant block was uploaded: ring offset and size of (stage 0 / 1 / 2, slot = the block's binding index). Persists.
void vkn_draw_ubo(int stage, uint32_t slot, uint32_t ring_off, uint32_t size);
// Draws. first_index: in indices from the start of the index buffer; instances >= 1. Return 0 = recorded, 1 = dropped (counted).
int vkn_draw_indexed(const vkn_draw_in *in, uint32_t first_index, uint32_t count, uint32_t instances);
int vkn_draw_arrays(const vkn_draw_in *in, uint32_t first, uint32_t count);
// Compute: the shader's compute stage, textures / images / uniform blocks of stage slot 2. images: `nimg` (slot, texture handle).
typedef struct vkn_img_bind { uint32_t slot; uint64_t tex; } vkn_img_bind;
int vkn_draw_dispatch(const vkn_draw_in *in, const vkn_img_bind *img, uint32_t nimg, uint32_t x, uint32_t y, uint32_t z);
// Raw draw for callers with their own shaders and state (the UI part): everything explicit; pipeline and set are the caller's.
void vkn_draw_raw_begin(void);                    // the next vkn_draw_* must rebind everything (the raw caller bound its own objects)

typedef struct vkn_draw_stats {
    uint64_t draws, indexed, arrays, dispatches, dropped_shader, dropped_state, dropped_pipe, dropped_pass, dropped_prim;
    uint64_t pipe_binds, set_binds, set_builds, set_cache, vb_binds, ib_binds, viewport_sets, feedback_tex, unknown_tex, unknown_smp;
} vkn_draw_stats;
void vkn_draw_stats_get(vkn_draw_stats *s);

// ==== added by the draw piece (stage 3, vkn_draw.c): not part of the original contract ====
// Install time (mode 1, dm/host/hle_dos2_vkn_draw.c): registers this module's begin listener (vkn_frame.h) that forgets everything
// bound and runs the death check below. Without it the draw path still notices a new recording by vkn_cb_serial.
void vkn_draw_install(void);
// Every descriptor set of this module goes: its pools are retired (vkn_retire: destroying a pool frees its sets) and the set cache
// starts empty; the next draws build new sets. Any thread (a request, executed by the recording thread at its next draw). This
// module calls it itself at every recording begin when the death counter it reads through vkn_tex_get_stats (textures destroyed /
// swapped, samplers destroyed) and vkn_shader_stats_get (shaders destroyed / replaced) moved, and when the cache exceeds its budget:
// a dead view / sampler / layout handle can only be reused after its retirement (3 frames), so a check per recording is enough.
void vkn_draw_forget_all(void);
// Counters outside vkn_draw_stats (drop reasons and placeholder use the contract struct has no field for).
typedef struct vkn_draw_stats2 {
    uint64_t dropped_bind;       // a binding kind / descriptor type the draw path has no source for (storage, driver tables, images in graphics)
    uint64_t dropped_vb, dropped_ib;   // a stream the vertex format reads has no live vertex buffer / no live index buffer (or out of range)
    uint64_t dropped_viewport;   // viewport width <= 0 or height 0
    uint64_t dropped_variant;    // more than 255 distinct retype codes / alpha references (pipeline key variants)
    uint64_t dropped_set;        // descriptor set allocation failed / too many bindings / dynamic offsets
    uint64_t dropped_cb;         // no open recording
    uint64_t scissor_empty;      // scissor clamped to the pass is empty: nothing to draw (not recorded, not an error)
    uint64_t zero_ubo;           // uniform bindings fed with the 64 KiB zero block (slot never uploaded / no support / c1 block)
    uint64_t empty_tex;          // texture bindings whose engine slot is -1 (placeholder)
    uint64_t view_mismatch;      // the texture's view type differs from the binding's dimension (bound anyway; logged)
    uint64_t placeholder_fail;   // no placeholder for a binding shape (multisample, cube array without the feature, creation failure)
    uint64_t ubo_announce, ubo_bad;    // vkn_draw_ubo calls / calls with a stage or slot out of range
    uint64_t forget_all, pools, set_bytes_words;   // cache flushes, pools created, key words held now
} vkn_draw_stats2;
void vkn_draw_stats2_get(vkn_draw_stats2 *s);

// ==== added by the immediate-draw piece (stage 3 wave 2, vkn_draw.c "index rewrite"): not part of the original contract ====
// vkn_draw_arrays / vkn_draw_indexed draw engine primitives 2 (triangle fan), 4 (quads) and 7 (line loop) themselves: generated
// u16 / u32 indices in a transient index area of vkn_draw.c, drawn as vkn_prim_rewrite's primitive (no caller change needed).
enum { VKN_RW_FAN = 0, VKN_RW_QUADS = 1, VKN_RW_LOOP = 2, VKN_RW_KINDS = 3 };
typedef struct vkn_draw_rw_stats {
    uint64_t draws[2][VKN_RW_KINDS];   // [0 arrays / 1 indexed][fan / quads / loop] draws recorded with generated indices
    uint64_t indices, bytes;           // indices generated, transient bytes handed out (u32-aligned)
    uint64_t degenerate;               // nothing to draw (a fan under 3, quads under 4, a loop under 2 vertices / indices)
    uint64_t dropped;                  // no transient block / no CPU pointer of the index buffer / too many indices
    uint64_t blocks, retired;          // transient blocks taken / retired when full
} vkn_draw_rw_stats;
void vkn_draw_rw_stats_get(vkn_draw_rw_stats *s);

#ifdef __cplusplus
}
#endif
#endif
