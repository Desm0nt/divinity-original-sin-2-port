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

typedef struct vkn_warm_stats {
    uint32_t listed, loaded, appended, dropped_full, dropped_bad, not_listable, queue_lost;   // the list
    uint32_t pending, replayed, built, failed, skipped, stale, mismatched;                    // the replay
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
