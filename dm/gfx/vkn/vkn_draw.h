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
#include "vkn_buf.h"   // [rt P2] vkn_buf_info (vkn_draw_pre)
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
uint64_t vkn_draw_stamp(void);   // [frame F1] draws recorded + every drop (one number: it moves when a prepared draw ends)

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

// ==== [stage 5] added by the draw-cost piece (A2, vkn_draw.c "draw cost"): not part of the original contract ====
// DM_VK_NATIVE_COST: a bit mask of exact shortcuts of the per-draw path (every one gives the same Vulkan state at every draw as the
// path without it; a bit off = that part of the stage-4 code exactly; 0 = everything as before). Set once at install (the host reads
// the switch; default VKN_COST_ALL in mode 1); this module's default is 0 (host tests and other callers keep the old path unless set).
//   IB      index buffers bound at (offset mod index size), firstIndex grows by offset / index size (nr_fold_index, dm/gfx/nr/nr_draw.h):
//           every engine index buffer is a 256-aligned block of one arena VkBuffer, so consecutive meshes bind the same (buffer, 0)
//   VB      vertex buffers of a draw looked up under one lock (vkn_buf_get_n); changed streams bound as one vkCmdBindVertexBuffers per
//           run of used bindings (nr_vb_runs)
//   VFOLD   [stage 5.1 G] (with VB only) per-vertex streams folded into vertexOffset / firstVertex (nr_fold_vertex) only for a vertex
//           shader KNOWN not to read gl_VertexIndex / gl_VertexID / gl_BaseVertex (vkn_shader_vindex; a piece's shader: never); the
//           fold changes the draw command's vertexOffset / firstVertex, so it has a bit of its own for the A/B run (0x7f = off)
//   SET     descriptor-set key hashed by words (not by bytes) and compared with the previous draw's key first (same key: the same
//           set, no probe); with vkn_tex_bind_gen (below) the texture bindings of the previous draw are reused when the shader, the
//           slot values, the effect output (all zero) and the generation are the same (no view / sampler lookups)
//   PIPE    a direct-mapped front cache key -> VkPipeline before vkn_pipe_get (valid while vkn_pipe_forget_gen() is unchanged and
//           until vkn_draw_forget_all)
//   LOOKUP  per-draw lookups of things that cannot change: the ring's buffer / base (fixed once created), SUPPORT blocks by viewport
//           size (immutable, never freed), shader objects by handle (valid while vkn_pipe_forget_gen() is unchanged: every shader
//           death calls vkn_pipe_forget_shader through vkn_shader_set_dead_fn), vertex-format ids (no destroy event exists); blend /
//           depth-stencil / raster ids only with vkn_state_handle_gen (below)
//   UBO     (host, hle_dos2_vkn_draw.c) a constant block whose bytes equal a ring copy made in this frame for the same slot is not
//           copied again: the record points at that copy
//   FLUSH   (host) the flushes skip work whose result is known: the texture-range reset when every range is already clean, the
//           shader blocks' re-announce when the same record was announced last with no upload and no announce since, the ring's CPU
//           pointer and the engine thread id are read once
enum { VKN_COST_IB = 1, VKN_COST_VB = 2, VKN_COST_SET = 4, VKN_COST_PIPE = 8, VKN_COST_LOOKUP = 16, VKN_COST_UBO = 32, VKN_COST_FLUSH = 64,
       VKN_COST_VFOLD = 128, VKN_COST_ALL = 255 };
void vkn_draw_cost_set(uint32_t mask);   // install time (or between recordings); a change of the SET bit starts a new set generation
uint32_t vkn_draw_cost(void);
// Generation of the uniform-block announces: + 1 at every vkn_draw_ubo that changes what a (stage, slot) holds.
uint32_t vkn_draw_ubo_gen(void);
// What the fast paths need from other modules, weak here (absent = that fast path stays off). [stage 5.1 G] all three exist now:
//   uint32_t vkn_tex_bind_gen(void)       vkn_tex.h: + 1 (atomic) whenever vkn_tex_view_by_id(id, &t) (view or t), vkn_sampler_by_id(id)
//                                         or vkn_sampler_cmp_by_id(id) may return something else for some id; a plain atomic load
//   uint32_t vkn_state_handle_gen(void)   vkn_state.h: + 1 (atomic) in hmap and hkill (every handle -> id change, incl. vkn_vfmt_kill)
//   int vkn_shader_vindex(const vkn_shader *s)   vkn_shader.h: 1 = the vertex module reads BuiltIn VertexIndex / VertexId / BaseVertex
//                                         (the translator's NR-09 scan, xbg_game.cpp "vi") or that is not known, 0 = it does not
//                                         (< 0 from a stand-in: unknown, no fold either: the fold needs exactly 0)
// Host tests may install stand-ins (NULL = the weak symbol):
void vkn_draw_cost_gens(uint32_t (*tex_gen)(void), uint32_t (*state_gen)(void), int (*vindex)(const void *shader));
typedef struct vkn_draw_cost_stats {
    uint64_t ib_calls_old, ib_calls, ib_folded;              // index binds the stage-4 path would make / made; draws drawn with a folded index offset
    uint64_t vb_calls_old, vb_calls, vb_folded, vb_batched;  // vertex binds (calls) old / made; draws with a vertex fold; draws looked up under one lock
    uint64_t vb_vindex;                                      // draws not folded: the shader reads gl_VertexIndex or is not known
    uint64_t set_key_same, set_tex_reuse, set_tex_lookups;   // set lookups answered by the previous key; texture bindings reused (draws) / looked up (draws)
    uint64_t pipe_front_hits, pipe_front_miss;               // pipeline lookups after a key change
    uint64_t shader_hits, support_hits, vfmt_hits, state_hits, ring_hits;   // lookups answered by the per-draw caches
} vkn_draw_cost_stats;
void vkn_draw_cost_stats_get(vkn_draw_cost_stats *s);
// [stage 5.1 G] A draw a piece makes INSIDE the effect chain (vkn_fx.h: between vkn_fx_bypass(1) / (0) in its function, e.g. the shadow
// cache's replays) is nested in the engine draw being prepared: it keeps that draw's state (call values, fold) intact and does not enter
// the query module (vkn_query.h brackets, conditions, predicates and counts are the engine's draws only).

// ==== [rt P0] DM_VK_NATIVE_PROF=3: what differs between a new pipeline key and the previous one of the recording (vkn_prof.c line) ====
// f[]: 0 shader, 1 module variants, 2 vertex format, 3 depth-stencil, 4 raster, 5 blend, 6 stencil reference, 7 write masks, 8 primitive,
// 9 pass, 10 effect bits, 11 own vertex module (vkn_fx_out.vin); dyn_only: changes where only ds / raster / stencil ref / blend differ
// (what dynamic state could remove), dyn_only_nb: the same without blend.
typedef struct vkn_draw_prof3 { uint64_t changes, f[12], dyn_only, dyn_only_nb; } vkn_draw_prof3;
void vkn_draw_prof3_get(vkn_draw_prof3 *s);

// ==== [rt P2] DM_VK_NATIVE_RT_RES (vkn_rt.h): what the encoder resolved for a draw, carried in its record ====
// The engine thread looks the shader object, the four state ids and the buffers up at encode (as the engine's NVN draw read them at
// that point); the recording thread's draw takes them from here instead of looking up again (its locks and table walks). Exact:
//   shader     vkn_shader_get(in->shader) changes only by a destroy (DestroyShaderIfOld: a sync point, every record before it executed)
//              or a replacement on a loader thread (the old object stays valid 3 frames: vkn_retire; the engine thread is at most 2
//              frames ahead) - the record then holds what the engine's draw named at its call, which mode 0 uses too;
//   state ids  handle -> id changes only by hmap (a new handle) / hkill (the destroy hooks have the default sync: no record in flight);
//   buffers    the encoder view at encode = the recording view at execution (vkn_buf.h "[rt P2]");
// Verify mode (2): the recording thread looks everything up as before, compares, counts differences (vkn_draw_res_stats) and uses its
// own lookups. Never used for a piece's nested draw (only for the record's own vkn_draw_in, as vkn_draw_rt_snap).
typedef struct vkn_draw_pre {
    const struct vkn_shader *shader;               // NULL: none (the draw drops with the old red line)
    uint16_t blend, ds, raster, vfmt;              // ids (0: handle 0, or not known -> the draw drops)
    VkBuffer vb[3]; uint64_t vb_off[3];            // the block of in->vb[st] (buf VK_NULL_HANDLE: 0 / not live / no memory)
    vkn_buf_info ib; int32_t ib_rc;                // vkn_buf_lookup of in->ib (indexed draws; rc -1 = not live / 0 handle)
    uint32_t pad;
} vkn_draw_pre;
// The encoder side (vkn_rt.c draw_enc): fills *p (its own caches, keyed like the recording side's by vkn_pipe_forget_gen /
// vkn_state_handle_gen); marks the buffers as vkn_buf_mark / vkn_buf_lookup do. The IB only when indexed.
void vkn_draw_resolve(const vkn_draw_in *in, int indexed, vkn_draw_pre *p);
extern const vkn_draw_pre *vkn_draw_rt_pre;        // vkn_rt.c exec: the record's resolution for the draw vkn_draw_rt_in (NULL = none)
extern int vkn_draw_rt_res_verify;                 // 1 = compare instead of use (vkn_rt.c, DM_VK_NATIVE_RT_RES=2)
typedef struct vkn_draw_res_stats { uint64_t used, verified, differ, differ_shader, differ_state, differ_vb, differ_ib; } vkn_draw_res_stats;
void vkn_draw_res_stats_get(vkn_draw_res_stats *s);

// ==== [rt P4] DM_VK_NATIVE_SHC_EHASH (vkn_rt.h): an announce with the block's hash made on the engine thread ====
// vkn_draw_ubo + the hash of its `size` bytes; vkn_draw_ubo clears a slot's hash. vkn_draw_ubo_hash: 0 = the slot holds (off, size) with a
// hash given in this frame (vkn_frame()), *h = it; -1 = none (the caller hashes itself).
void vkn_draw_ubo_h(int stage, uint32_t slot, uint32_t ring_off, uint32_t size, uint64_t hash);
int vkn_draw_ubo_hash(int stage, uint32_t slot, uint32_t ring_off, uint32_t size, uint64_t *h);

// ==== [rt P3] DM_VK_NATIVE_COST bits 0x100 / 0x200 (textures and set without locks) ====
//   MIRROR (0x100)  this module's own table "texture-pool id -> (view, texture)", "sampler-pool id -> sampler" for bind_textures: an
//                   entry holds what the locked lookup answered under vkn_tex_bind_gen() G (read BEFORE that lookup); it answers while the
//                   generation is still G. Why the lock-free read is safe: every write that changes an answer bumps the generation
//                   under vkn_tex.c's mutex (release), and the reader loads it (acquire) before using an entry; a write in flight that
//                   this load does not see yet is ordered after the reader's use, exactly as a locked lookup that took the mutex first;
//                   the objects stay valid: a destroy is a sync point (no record in flight) and retires them 3 frames later. Off while
//                   DM_VK_NATIVE_TEXHACK=1 rules (vkn_sampler_by_id_pure() == 0).
//   SHMEM (0x200)   the texture bindings and the set of the last 2 draws of each of 64 shaders (by shader object; was: of the last draw
//                   only, VKN_COST_SET): reused under the conditions of the SET reuse (same shader object, shader generation, texture
//                   generation, all-zero effect output, every (texture id, sampler id), vkn_pass_sample asked again with the same
//                   answers); the set key of that shader's last draw is compared before the hash probe.
//   DM_VK_NATIVE_COST_CHECK=1: every reuse / mirror answer is compared with the full path (which is then used), differences counted.
enum { VKN_COST_MIRROR = 0x100, VKN_COST_SHMEM = 0x200, VKN_COST_ALL2 = 0x3ff };
void vkn_draw_cost_check(int on);
typedef struct vkn_draw_cost3_stats { uint64_t mirror_hits, mirror_miss, shmem_hits, shmem_miss, shmem_set_hits, check_runs, check_differ; } vkn_draw_cost3_stats;
void vkn_draw_cost3_stats_get(vkn_draw_cost3_stats *s);
#ifdef __cplusplus
}
#endif
#endif
