// [DOS2] VK-NATIVE stage 5.5: the backend's own recording thread (vkn_rt.c). Design: docs/dos2/nr/VK-STAGE55-THREAD.md.
// The cut is at the backend's entry: the engine thread (the "encoder": the thread that runs BeginFrame) writes short records of what
// the hooks would have done into a stream; the recording thread (the "worker") executes them in order with the very functions the hooks
// called before (vkn_draw_*, vkn_pass_*, vkn_rec_*, the UI calls). For the rest of the backend the recording is still single-threaded:
// at any moment exactly one thread owns it (see "ownership"). No game addresses.
//
// DM_VK_NATIVE_RT (read by dm/host/hle_dos2_vkn_frame.c at install, passed to vkn_rt_install):
//   0  no stream: every call goes straight to the backend, exactly as after stage 5.1 (every function below is one branch);
//   1  the worker records; the engine thread is at most 1 frame ahead (at the end of frame N it waits until frame N-1 is recorded);
//   2  the same, at most 2 frames ahead;
//   9  verify: every record is encoded and at once executed on the engine thread (no worker: checks the encoding without races).
// Forced 0 with DM_VK_NATIVE_COND 1 / 3 (the engine reads the visibility verdicts the recording produces).
//
// Snapshot at encode (design section 2). What a record holds:
//   draws        vkn_draw_in by value; the texture / sampler slot arrays of stages 0 and 1 (32 + 32 numbers each) go into the stream
//                only when they changed (DM_VK_NATIVE_RT_SLOTS: 0 copy at every draw, 1 compare with the last copy (default), 2 the
//                engine's dirty ranges (vkn_rt_slots_touch) decide, 3 = 2 checked against the compare: differences counted, the
//                compare wins); the condition of vkn_query_cond; for an indexed draw of an engine primitive without a Vulkan topology
//                (fan / quads / loop) the engine's indices themselves (the rewrite reads them through the CPU pointer); dispatches carry
//                the compute slot arrays and the image list. Buffers are NOT resolved here: vkn_buf keeps two views (vkn_buf.h,
//                "[stage 5.5]"), so a record executed later finds exactly the block the engine's draw would have found, and the encoder
//                marks the buffers a draw reads at encode (the rename rule of vkn_buf_update runs on the encoder);
//   constants    vkn_rt_ubo: the announce goes into the stream in order with the draws (the bytes are in the ring already);
//                vkn_rt_ubo_gen is the encoder's own generation of the announces (what vkn_draw_ubo_gen would say for the engine's ones);
//   the rest     vkn_rt_call: a closure with its arguments copied (targets, clears, copies, the UI, queries, retirements).
// Sync point (section 3): vkn_rt_sync / vkn_rt_own_begin: the engine thread waits until the worker executed everything written and
// then owns the recording itself (the worker sleeps) until its next record; every hook entry that is not flagged "encodes"
// (dm/host/hle_dos2_vkn_stack.c) does that first, so the rare paths stay exact without edits. Calls from other threads (loaders) do
// not sync. Inside an owned section (vkn_rt_own_begin .. _end) every record is executed at once on the calling thread.
// Ownership: vkn_rt_rec_thread_is() tells the modules that decide by "am I the recording thread" (vkn_tex_update, vkn_read).
// Frames (section 4): vkn_rt_rec_begin / vkn_rt_rec_end / vkn_rt_frame_tail are records; the tail also runs the frame's census tick on
// the worker and holds the backpressure. Off-frame recordings (EndFrame without present: the engine expects the GPU work finished) end
// with a sync. The frame counter the encoder sees (vkn_rt_frame) is vkn_frame() as the recording will have it at that point.
#ifndef DM_VKN_RT_H
#define DM_VKN_RT_H
#include "vkn.h"
#include "vkn_draw.h"
#include "vkn_tex.h"
#define VKN_PASS_COLORS_RT 8                              // = VKN_PASS_COLORS (vkn_pass.h)
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_RT_OFF = 0, VKN_RT_AHEAD1 = 1, VKN_RT_AHEAD2 = 2, VKN_RT_VERIFY = 9 };
enum { VKN_RT_SLOTS_COPY = 0, VKN_RT_SLOTS_CMP = 1, VKN_RT_SLOTS_GEN = 2, VKN_RT_SLOTS_GEN_VERIFY = 3 };
// Install time (or a test between scenarios with nothing encoded): the mode (anything else than 1 / 2 / 9 = 0) and the slot policy.
void vkn_rt_install(int mode, int slots);
int vkn_rt_mode(void);
extern int vkn_rt_live_;                                  // 1 once the encoder is bound (the first vkn_rt_rec_begin of a mode != 0)
static inline int vkn_rt_live(void) { return vkn_rt_live_; }
int vkn_rt_threaded(void);                                // 1 = the worker records (modes 1 / 2, live)
int vkn_rt_rec_thread_is(void);                           // -1 not threaded (decide as before), 1 = this thread owns the recording, 0 = not
int vkn_rt_decoding(void);                                // 1 = this thread is executing a record right now (vkn_buf: the recording view)
int vkn_rt_encoder_here(void);                            // 1 = live and this is the encoder thread
int vkn_rt_tex_rec(void);                                 // 1 = a vkn_tex_update here is a record (vkn_tex.c: the engine thread, not owning)
uint64_t vkn_rt_frame(void);                              // the encoder: vkn_frame() of the recording at this point; elsewhere vkn_frame()
uint32_t vkn_rt_cb_serial(void);                          // vkn_cb_serial for the engine thread: changes with every vkn_rt_rec_begin

// ---- sync points ----
typedef struct vkn_rt_site {
    const char *name;
    uint64_t calls, syncs, wait_ns;                       // on the encoder: calls, syncs that found records to wait for, their wait
    uint64_t prev_syncs;                                  // (census window)
    struct vkn_rt_site *next; int reg;
} vkn_rt_site;
#define VKN_RT_SITE(n) {(n), 0, 0, 0, 0, NULL, 0}
void vkn_rt_sync(vkn_rt_site *site);                      // wait until everything written is executed; this thread owns the recording
void vkn_rt_own_begin(vkn_rt_site *site);                 // vkn_rt_sync + an owned section (nests; every record inside runs at once)
void vkn_rt_own_end(void);

// ---- records ----
void vkn_rt_call(void (*fn)(void *arg), const void *arg, uint32_t bytes);   // fn(copy of arg) at this point of the recording
void vkn_rt_ubo(int stage, uint32_t slot, uint32_t ring_off, uint32_t size);   // vkn_draw_ubo
uint32_t vkn_rt_ubo_gen(void);                                                // vkn_draw_ubo_gen
void vkn_rt_query_cond(uint64_t key);                                         // vkn_query_cond (carried by the next draw / dispatch)
int vkn_rt_draw_indexed(const vkn_draw_in *in, uint32_t first, uint32_t count, uint32_t instances);   // 0 = recorded / encoded
int vkn_rt_draw_arrays(const vkn_draw_in *in, uint32_t first, uint32_t count);
int vkn_rt_dispatch(const vkn_draw_in *in, const vkn_img_bind *img, uint32_t nimg, uint32_t x, uint32_t y, uint32_t z);   // + vkn_query_dispatch
void vkn_rt_slots_touch(void);                            // the engine's texture-slot dirty ranges were not clean (slot policy 2 / 3)
void vkn_rt_retire_block(const vkn_block *b);             // vkn_retire_block where the recording is (any thread)
void vkn_rt_retire(void (*fn)(void *ctx), void *ctx);     // vkn_retire likewise

// ---- frames ----
void vkn_rt_rec_begin(int presented);                     // vkn_rec_begin(presented); the first call binds the encoder (this thread)
// pass end + vkn_rec_present(win, crop, interval) when present, then vkn_rec_submit(!present). Off-frame: a sync after it.
void vkn_rt_rec_end(int present, vkn_tex *win, const int32_t crop[4], int interval);
// The frame's tail: vkn_prof_frame_cpu(cpu[0..2]) and tick() on the recording thread, then (modes 1 / 2) the backpressure.
void vkn_rt_frame_tail(const uint64_t cpu_ns[3], void (*tick)(void));

// ---- the host's backend calls as records (each = the named function at this point of the recording; arguments copied) ----
void vkn_rt_pass_targets(uint32_t n, vkn_tex *const color[VKN_PASS_COLORS_RT], const uint32_t *layer, const uint32_t *mip, vkn_tex *depth);
void vkn_rt_pass_clear_color(vkn_tex *t, uint32_t layer, const float rgba[4], const int32_t rect[4]);
void vkn_rt_pass_clear_depth(vkn_tex *t, int what, float depth, uint32_t stencil);
void vkn_rt_pass_clear_bound_depth(int what, float depth, uint32_t stencil, uint64_t *none);   // the bound depth target then (none: + 1 when none)
void vkn_rt_pass_copy(vkn_tex *dst, vkn_tex *src);
void vkn_rt_query_begin(uint64_t key);
void vkn_rt_query_end(uint64_t key);
struct vkn_ui_draw_in;
void vkn_rt_ui_targets(vkn_tex *color, vkn_tex *depth);
void vkn_rt_ui_viewport(int32_t x, int32_t y, int32_t w, int32_t h);
void vkn_rt_ui_scissor(int32_t x, int32_t y, int32_t w, int32_t h);
void vkn_rt_ui_clear_ds(int depth, uint8_t mask);
void vkn_rt_ui_clear_color(vkn_tex *t, const int32_t rect[4]);
int vkn_rt_ui_draw(const struct vkn_ui_draw_in *in);       // the constants, vertices and indices copied (a vertex buffer block by value)
void vkn_rt_draw_raw_begin(void);
void vkn_rt_ui_pass_fence(void);                          // [stage 5.2 audit] vkn_ui_pass_fence (the UI pass piece)
// [stage 5.2 audit] the engine's CPU-written rings (immediate draws: 0 vertex, 1 index; 2.. tests): the encoder, before the bytes go in
enum { VKN_RT_CPU_RINGS = 4 };
void vkn_rt_cpu_ring(int id, uint64_t advance, uint64_t size);

// ---- census ----
void vkn_rt_census(void);                                 // the "[vkn] rt:" line (the 10 s census, recording thread)
typedef struct vkn_rt_stats {
    uint64_t ops[8], bytes, frames, syncs, sync_wait_ns, bp_wait_ns, idle_ns, busy_ns, ahead[4];
    uint64_t guard_ring, mispredict, foreign, slot_copies, slot_gen_miss, oom, inline_ops, guard_cpu;
} vkn_rt_stats;
enum { VKN_RT_OP_DRAW, VKN_RT_OP_DISPATCH, VKN_RT_OP_UBO, VKN_RT_OP_SLOTS, VKN_RT_OP_CALL, VKN_RT_OP_FRAME, VKN_RT_OP_SNAP, VKN_RT_OP_NEXT };
void vkn_rt_stats_get(vkn_rt_stats *s);

// ---- the constant ring's guard (vkn_buf.c calls it on the encoder, threaded modes): ring bytes handed out so far incl. this one ----
void vkn_rt_ring_guard(uint64_t cum_end);

// ---- tests ----
void vkn_rt_test_yield(void (*fn)(int where));            // called at encode / decode boundaries (where 0 encode, 1 decode)
void vkn_rt_test_ring_limit(uint64_t bytes);              // the guard's ring size (0 = VKN_RING_BYTES)
void vkn_rt_shutdown(void);                               // drain, stop the worker, back to "not live" (tests)

#ifdef __cplusplus
}
#endif
#endif
