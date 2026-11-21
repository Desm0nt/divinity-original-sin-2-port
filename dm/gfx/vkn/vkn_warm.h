// [DOS2] VK-NATIVE stage 4, pipeline warm-up (vkn_warm.c). Without it every pipeline is built at the first draw that needs it, on the
// render thread (first phone runs: 242-312 pipelines, 1.4 s of builds = hitches while walking), and nothing survives the run.
//   1. The driver cache: the VkPipelineCache of vkn_state.c is created from vkn-pipeline-cache.bin (header checked like the
//      translator's vk-pipeline-cache.bin: version 1, vendor, device, pipelineCacheUUID) and saved by the worker - every 300 presented
//      frames while new pipelines were compiled (a build of >= 2 ms), tmp file + rename, never on the render thread.
//   2. The list: every graphics pipeline the render thread builds is appended to vkn-pipeline-list.bin as content that is stable
//      across runs (the ids of vkn_state / vkn_shader are assigned at run time and are not): the stage program keys, the module
//      variant parameters (retype code, alpha func + reference), the engine argument tuples of blend / depth-stencil / raster, the
//      vertex format elements, primitive, stencil reference, the render pass compatibility content (VkFormats, samples), write masks,
//      vertex inputs. Versioned header (device bound), fixed records with a checksum, deduplicated, at most VKN_WARM_MAX records.
//   3. The replay: a low-priority worker thread with an 8 MB stack (translator calls, driver compiles) waits until the engine created
//      a shader with a listed key pair, then builds the listed pipelines of it into its own table, keyed by content + modules (the
//      draw path's variant numbering is its own: the key the draw asks for is matched by its content). A miss of vkn_pipe_get asks
//      vkn_warm_take first: a ready pipeline moves to the cache table, one being built right now is waited for (never built twice).
// Both files live next to the translator's vk-pipeline-cache.bin (xbg_vkn_cache_dir; DM_FILES_DIR as a fallback).
// Switch DM_VK_NATIVE_WARM: default 1 in mode 1 (DM_VK_NATIVE=1); 0 = the stage-3 behaviour (empty cache, no files, no worker).
// DM_GFX_PIPE_CACHE_LOAD=0 (the translator's PowerVR switch) skips loading the cache file here too (it is still saved).
// No game addresses. Thread safety: one mutex for the worker's table, its note queue and its counters; the render thread's hit path
// of vkn_pipe_get never touches it.
//
// [stage 5 rest] TECHNIQUE PIPELINES (list version 2; a version-1 file is read and rewritten). Stage 5's draw effects (vkn_fx.h) built
// their pipelines unlisted (the draw path passed no shader): the first sight of a scene built them in the frame. Now listed:
//   * fx bits (no fragment stage: MAT variant A, the shadow cache's depth-only draws; rasterizer discard): the record's `fx`, replayed
//     on the engine shader with the same bits;
//   * a named fragment stage drawn outside its verified pass (DM_VK_NATIVE_SH, the Dref / PCF bundles): `fsalt`, replayed with the
//     translated module (vkn_shader_named_alt) - off the recording thread vkn_shader_module gives the named one;
//   * a piece's vertex module (vkn_fx_out.vin: MAT variant B and XLQ) and a piece's shader object (vkn_fx_shader_create: LIGHTFUSE's
//     fused 'f' / 'g' shaders and its Fog-window readers) by RECIPE: the piece tells this file, when it makes the module / object, a
//     name and up to VKN_WARM_ARG bytes that make it again from the engine shader (vkn_warm_recipe_vs / _fx), and registers a maker
//     (vkn_warm_maker) that returns the SAME module + vin id / object for them on any thread (the piece's own table, under its lock).
//     The record keeps the engine shader's keys (the replay waits for that shader), the piece name and the bytes.
//   Not listed (counted as "not listable", by reason): a build of a piece without a recipe (PRESKIN's modules, post's fused shaders).
//   Exactness: a listed build is taken only by a draw asking for the same key (effect ids included) and the same modules - a recipe
//   that made something else is an unused build, never another picture.
// [stage 5 rest] PORTABLE SEED (vkn_seed.c, DM_VK_NATIVE_SEED): the records of the APK's dos2-pipe-seed.bin (the old path's NR-31 seed:
//   a phone's list as Vulkan create-info content) translated to this backend's records when their shader exists (vkn_seed.h).
// [VK-SEED] the new path's seed dos2-vkn-seed.bin (this list's records made device-independent, tools/dos2_vkn_seed.py) is read first;
//   THE WARM-UP AT LOAD: while the game loads (draws a frame < 64 or no frame in a 250 ms window) the worker runs at normal priority
//   with DM_VK_NATIVE_WARM_JOBS (default 3, max 7) helper threads on the pending entries; DM_VK_NATIVE_WARM_LOAD 0 off / 1 (default) /
//   2 always. Census line "warm: loads".
#ifndef DM_VKN_WARM_H
#define DM_VKN_WARM_H
#include "vkn.h"
#include "vkn_state.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_WARM_MAX = 4096, VKN_WARM_SAVE_FRAMES = 300 };

// Install time (dm/host/hle_dos2_vkn_warm.c, mode 1 only): reads DM_VK_NATIVE_WARM, starts the worker. 1 = on.
int vkn_warm_start(void);
int vkn_warm_on(void);

// Called by vkn_state.c (declared weak there):
// a miss of the render thread: the worker's pipeline of the same content and modules (waits while it is being built). 1 = *out set,
// the pipeline now belongs to the caller's table.
int vkn_warm_take(const vkn_pipe_key *k, const vkn_pipe_src *src, VkPipeline *out);
// the render thread built (d, src): queued for the list (src->shader set by the draw path; otherwise not listable, counted).
void vkn_warm_note(const vkn_pipe_desc *d, const vkn_pipe_src *src);
// a shader id died: the worker's pipelines of it are retired; listed entries built for it are replayed again for a new record.
void vkn_warm_forget(uint32_t shader);
// 1 = the worker creates the VkPipelineCache (from its file), vkn_state must not create an empty one.
int vkn_warm_owns_cache(void);

// ---- [stage 5 rest] technique pipelines: recipes and makers (any thread) ----
enum { VKN_WARM_ARG = 48, VKN_WARM_PIECE = 10 };
struct vkn_shader;
// The module / object of piece `piece` for engine shader `eng` and the recipe bytes: fx (shader object) or vs + vs_id (vertex module).
// 0 = made / found (the same one a draw gets).
typedef int (*vkn_warm_make_fn)(const struct vkn_shader *eng, const void *arg, uint32_t n, const struct vkn_shader **fx, VkShaderModule *vs, uint16_t *vs_id);
int vkn_warm_maker(const char *piece, vkn_warm_make_fn fn);   // 0 ok (install time)
// The piece made fx object `fx_shader` / vertex module id `vs_id` for engine shader `eng_shader` from these bytes (n <= VKN_WARM_ARG).
void vkn_warm_recipe_fx(const void *fx_shader, const char *piece, const void *eng_shader, const void *arg, uint32_t n);
void vkn_warm_recipe_vs(uint16_t vs_id, const char *piece, const void *eng_shader, const void *arg, uint32_t n);

// ---- the list record (version 2; vkn_seed.c makes them too) ----
enum { VKN_WREC_VEL = 16 };
enum { VKN_WREC_F_BLEND = 1, VKN_WREC_F_DS = 2, VKN_WREC_F_RASTER = 4, VKN_WREC_F_VFMT = 8, VKN_WREC_F_FS = 16 };
enum { VKN_WREC_R_NONE = 0, VKN_WREC_R_FX = 1, VKN_WREC_R_VS = 2 };   // rkind: a piece's shader object / vertex module by recipe
typedef struct vkn_warm_rec {
    char key[2][16];            // vertex / fragment program keys (xbg_shader_program_key, 16 hex chars, no NUL; fragment zero = no stage)
    uint64_t retype;            // vertex variant: the retype code ("#vi"), 0 = base module
    float alpha_ref;            // fragment variant: alpha test reference ("#at")
    uint32_t alpha_func;        // NVN compare func of the alpha test, 0 = base module
    uint32_t color[8], depth;   // render pass compatibility: VkFormat per colour slot (0 = not bound), depth VkFormat
    uint8_t prim, stencil_ref, flags, nvel;   // engine primitive (after the rewrite), stencil reference, VKN_WREC_F_*, vertex elements
    uint8_t blend[6], ds[8], raster[5];        // engine argument tuples (vkn_blend_args / vkn_ds_args / vkn_raster_args)
    uint8_t ncolor, samples, pad0;
    uint8_t write_mask[8];      // VkColorComponentFlags per colour slot
    uint16_t vs_inputs, pad1;   // locations the vertex module reads (checked against the shader at the replay)
    uint8_t vel[6 * VKN_WREC_VEL];   // vertex format elements (6 bytes each, the engine's)
    uint8_t pad2[6];
    // version 2
    uint8_t fx, rkind, fsalt, pad3;   // VKN_PIPE_FX_* bits; VKN_WREC_R_*; 1 = the named fragment stage's translated module
    uint16_t rlen; char piece[VKN_WARM_PIECE];
    uint8_t rarg[VKN_WARM_ARG];
    uint64_t sum;               // fnv of everything above (a torn or foreign record is dropped)
} vkn_warm_rec;
uint64_t vkn_warm_rec_sum(const vkn_warm_rec *r);
// vkn_seed.c (worker thread): a record made from the seed; it enters the list as pending (built when its shader exists), not the file.
// 1 = new, 0 = known already (the device's own list has it, or full).
int vkn_warm_seed_add(const vkn_warm_rec *r);
// The draw path's vertex retype code for shader s and vertex format content id vfmt (vkn_draw.c retype_code), any thread.
uint64_t vkn_warm_retype_code(const struct vkn_shader *s, uint16_t vfmt);

typedef struct vkn_warm_stats {
    uint32_t listed, loaded, appended, dropped_full, dropped_bad, not_listable, queue_lost;   // the list
    uint32_t nl_noshader, nl_fx, nl_vin, nl_content, upgraded;                             // [stage 5 rest] not listable by reason; v1 records read
    uint32_t tech_listed, tech_replayed, seed_added, seed_replayed;                         // technique records in the list / built; seed records
    uint32_t pending, replayed, built, failed, skipped, stale, mismatched;                    // the replay
    uint32_t loads, load_jobs, load_helper_builds; uint64_t load_ns;                          // [VK-SEED] the warm-up at load (vkn_warm.c)
    uint64_t taken, waits, wait_ns, build_ns;                                                 // the render thread's misses served
    uint64_t cache_loaded, cache_saved, saves, save_fail;                                     // the driver cache file (bytes)
    int cache_state;                                                                          // 0 waiting for the directory, 1 loaded, 2 empty, -1 off
} vkn_warm_stats;
void vkn_warm_stats_get(vkn_warm_stats *s);
void vkn_warm_census(void);   // one line through vkn_logf (+ the render-thread builds of vkn_state)

#ifdef __cplusplus
}
#endif
#endif
