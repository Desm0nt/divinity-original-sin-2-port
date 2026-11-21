// [DOS2] VK-NATIVE stage 5.1 piece A0, measurement (vkn_prof.c): where the frame goes, so that a failed fps gate is diagnosed from the
// log instead of by turning techniques off (docs/dos2/nr/VK-STAGE5-PLAN.md, stage 5.1 row A0). Switch DM_VK_NATIVE_PROF (read once at
// install by dm/host/hle_dos2_vkn_frame.c; it replaces the old path's DM_NR_HEAT / DM_NR_TCOUNT pass meters):
//   0  nothing (every probe below is one predictable branch, every pass / frame hook one test of a flag);
//   1  default in mode 1: the 10 s lines "draw by part" and "backend measured" (draw parts timed on 1 of every 128 draws), and one
//      GPU-sampled presented frame every 20 s ("gpu by pass", the top 24 kinds of pass + the rest summed);
//   2  as 1, a GPU-sampled frame every 2 s and the per-pass list not truncated;
//   3  [rt P0] as 2 + the split measurement (block "[rt P0]" below).
// GPU: on a sampled presented recording a timestamp (BOTTOM_OF_PIPE) at its top, before every vkCmdBeginRenderPass, after every
// vkCmdEndRenderPass and at its bottom, into this module's query pool (one region per command buffer of vkn_frame's ring: the region
// is reset at the top of the recording that uses it and read in vkn_frame's slot_done, i.e. once that buffer's fence was seen
// signalled: never a wait). A pass is named by its targets: vkn_tex.sname of the first colour target, "+" that of the depth target, the
// render area; passes of the same name and size are one "kind" (shadow cascades: xN). "outside passes" = frame - sum of passes: uploads,
// barriers, copies, clears outside a pass, dispatches. A queue family whose timestampValidBits is 0 gets one line and no GPU lines.
// CPU: the engine thread inside the backend's draw (dm/host/hle_dos2_vkn_draw.c hook entry .. its end), split by part with
// vkn_now_ns at each vkn_prof_mark: a mark attributes the time since the previous probe to its part (a part reached twice adds up);
// the cost of one timer read (calibrated at init) is subtracted per probe. Extrapolated to ms per frame with the exact number of
// probed draw calls.
// Single-threaded (the recording thread, like vkn_frame.h). No game addresses.
#ifndef DM_VKN_PROF_H
#define DM_VKN_PROF_H
#include "vkn.h"
#include "vkn_tex.h"
#ifdef __cplusplus
extern "C" {
#endif

// ---- set-up and census ----
void vkn_prof_init(int level);          // install time, mode 1: the DM_VK_NATIVE_PROF level (0 / 1 / 2); prints one line
int vkn_prof_level(void);
void vkn_prof_gpu_request(void);        // any thread: the next presented recording is GPU-sampled (on demand; level >= 1)
// The host's ms of the last presented frame on the engine thread (hle_dos2_vkn_frame.c g_ft: BeginFrame, BeginRender..EndFrame =
// the frame's recording, EndFrame): summed for the "backend measured" line. Call once a presented frame.
void vkn_prof_frame_cpu(uint64_t begin_ns, uint64_t record_ns, uint64_t end_ns);
void vkn_prof_census(void);             // the 10 s lines "draw by part" and "backend measured" (from the host's census)

// ---- GPU timestamps: called by vkn_frame.c / vkn_pass.c only ----
void vkn_prof_rec_begin_(VkCommandBuffer cb, int slot, int presented);
void vkn_prof_rec_close_(VkCommandBuffer cb, int slot);
void vkn_prof_slot_done_(int slot);
void vkn_prof_pass_begin_(VkCommandBuffer cb, vkn_tex *const *color, uint32_t ncolor, vkn_tex *depth, uint32_t w, uint32_t h);
void vkn_prof_pass_end_(VkCommandBuffer cb);
extern int vkn_prof_gpu_on;             // level >= 1 and timestamps usable (frame hooks) / 1 = the open recording is sampled (pass hooks)
extern int vkn_prof_gpu_cur;
// vkn_frame.c: after the top timestamp of a recording of ring slot `slot` / before its bottom timestamp / in slot_done (fence signalled).
static inline void vkn_prof_rec_begin(VkCommandBuffer cb, int slot, int presented) { if (vkn_prof_gpu_on) vkn_prof_rec_begin_(cb, slot, presented); }
static inline void vkn_prof_rec_close(VkCommandBuffer cb, int slot) { if (vkn_prof_gpu_cur) vkn_prof_rec_close_(cb, slot); }
static inline void vkn_prof_slot_done(int slot) { if (vkn_prof_gpu_on) vkn_prof_slot_done_(slot); }
// vkn_pass.c: right before vkCmdBeginRenderPass (targets of the pass, render area) / right after vkCmdEndRenderPass.
static inline void vkn_prof_pass_begin(VkCommandBuffer cb, vkn_tex *const *color, uint32_t ncolor, vkn_tex *depth, uint32_t w, uint32_t h) {
    if (vkn_prof_gpu_cur) vkn_prof_pass_begin_(cb, color, ncolor, depth, w, h);
}
static inline void vkn_prof_pass_end(VkCommandBuffer cb) { if (vkn_prof_gpu_cur) vkn_prof_pass_end_(cb); }

// ---- draw probes (the draw path: dm/host/hle_dos2_vkn_draw.c hooks and dm/gfx/vkn/vkn_draw.c) ----
// vkn_prof_draw_begin at the hook's entry, vkn_prof_mark(part) after each part, vkn_prof_draw_end at the hook's end (time since the
// last mark goes to VKN_PROF_TAIL). Unsampled draw: begin = one decrement and one branch, mark / end = one branch. A sampled draw
// that never reaches vkn_prof_draw_end (an early return before it) is closed by the next begin (counted as "unended").
enum {
    VKN_PROF_FLUSH,     // host: constant / texture flushes (dm_vkn_draw_tail_pre)
    VKN_PROF_FILL,      // host: the renderer cache read into vkn_draw_in (dm_vkn_draw_fill)
    VKN_PROF_STATE,     // vkn_draw: shader, state ids, primitive / viewport checks (prepare up to the fx call point)
    VKN_PROF_FX,        // vkn_draw: the effect chain (vkn_fx_run)
    VKN_PROF_BUF,       // vkn_draw: vertex / index buffer lookups and binds (variants included)
    VKN_PROF_TEX,       // vkn_draw: textures (bind_textures, vkn_pass_sample) and the descriptor set (build_set + bind)
    VKN_PROF_PASS,      // vkn_draw: vkn_pass_ready (a lazily opened pass: its barriers and vkCmdBeginRenderPass)
    VKN_PROF_PIPE,      // vkn_draw: pipeline key, vkn_pipe_get, vkCmdBindPipeline
    VKN_PROF_VIEW,      // vkn_draw: viewport / scissor (and the dump trace when on)
    VKN_PROF_CMD,       // vkn_draw: the draw command (with the stage-4 query brackets)
    VKN_PROF_TAIL,      // the rest up to vkn_prof_draw_end (dm_vkn_draw_tail_post, counters, drop paths)
    VKN_PROF_PARTS
};
typedef struct vkn_prof_hot { uint32_t left, on; uint64_t last; uint32_t on3, pad; } vkn_prof_hot;   // on3: [rt] a sampled draw at level 3
extern vkn_prof_hot vkn_prof_h;          // (written only by the recording thread)
void vkn_prof_draw_slow(void);
void vkn_prof_mark_slow(int part);
void vkn_prof_end_slow(void);
static inline void vkn_prof_draw_begin(void) { if (__builtin_expect((--vkn_prof_h.left == 0) | (vkn_prof_h.on != 0), 0)) vkn_prof_draw_slow(); }
static inline void vkn_prof_mark(int part) { if (__builtin_expect(vkn_prof_h.on != 0, 0)) vkn_prof_mark_slow(part); }
static inline void vkn_prof_draw_end(void) { if (__builtin_expect(vkn_prof_h.on != 0, 0)) vkn_prof_end_slow(); }

// ==== [rt P0] DM_VK_NATIVE_PROF=3: the split measurement (docs/dos2/nr/VK-RT-ANALYSIS.md section 2.4) ====
// Level 3 = level 2's lines plus, on the same sampled draws (1 of 128):
//   * fine parts inside the coarse ones (vkn_prof_mark_f): each fine mark adds the time since the previous probe to its fine part AND to
//     the coarse part it belongs to, so "draw by part" keeps its meaning; line "draw by part 3: ...; driver X ms a frame (calls N), ours Y";
//     the driver's share = the fine parts that are only a vkCmd* call (VKN_PROF_F_*_CMD);
//   * the effect chain by function (vkn_fx.c, line "fx by function");
//   * the recording thread's records by kind (vkn_rt.c exec, 1 of 64 records timed, line "rt by record");
//   * what differs between a new pipeline key and the previous one (vkn_draw.c, line "pipeline changes");
//   * contended locks of vkn_buf.c / vkn_tex.c lookups (trylock failed first, line "locks").
// Levels 0..2: every fine probe is one predictable branch (on3 stays 0), nothing else changes.
enum {
    VKN_PROF_F_TEX_LOOK,   // tex+set: texture-pool / sampler-pool lookups (bind_textures)
    VKN_PROF_F_TEX_SAMPLE, // tex+set: vkn_pass_sample of the bound textures
    VKN_PROF_F_TEX_REST,   // tex+set: the rest of the texture phase (placeholders, element writes, the reuse checks)
    VKN_PROF_F_SET_KEY,    // tex+set: uniform elements + the set key
    VKN_PROF_F_SET_FIND,   // tex+set: set cache lookup / build
    VKN_PROF_F_SET_CMD,    // tex+set: vkCmdBindDescriptorSets (driver)
    VKN_PROF_F_PIPE_KEY,   // pipeline: key packing
    VKN_PROF_F_PIPE_FIND,  // pipeline: front cache / vkn_pipe_get
    VKN_PROF_F_PIPE_CMD,   // pipeline: vkCmdBindPipeline (driver)
    VKN_PROF_F_BUF_LOOK,   // buffers: lookups (index / vertex buffers, folds)
    VKN_PROF_F_BUF_CMD,    // buffers: vkCmdBindVertexBuffers / vkCmdBindIndexBuffer (driver)
    VKN_PROF_F_VIEW_CMD,   // viewport: vkCmdSetViewport / vkCmdSetScissor (driver)
    VKN_PROF_F_Q_PRE,      // draw-cmd: the query module's pre bracket
    VKN_PROF_F_DRAW_CMD,   // draw-cmd: vkCmdDraw* (driver)
    VKN_PROF_FINE
};
double vkn_prof_timer_ns(void);           // [rt2 F1] the calibrated cost of one timer read (ns)
extern int vkn_prof_lvl3;                // 1 = DM_VK_NATIVE_PROF=3 (read anywhere; set once at init)
void vkn_prof_mark_f_slow(int fine);
static inline void vkn_prof_mark_f(int fine) { if (__builtin_expect(vkn_prof_h.on3 != 0, 0)) vkn_prof_mark_f_slow(fine); }
static inline int vkn_prof_sampled3(void) { return vkn_prof_h.on3 != 0; }
void vkn_prof_driver_calls(uint32_t n);  // driver calls made by the sampled draw (counted with the fine parts)

#ifdef __cplusplus
}
#endif
#endif
