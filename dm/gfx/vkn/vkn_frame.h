// [DOS2] VK-NATIVE stage 3, recording / submit / present (vkn_frame.c). The engine's model (docs/dos2/nr/VK-NATIVE-MAP-3.md): one
// thread records; a presented frame records into one of two contexts (one per window texture), work outside a frame (texture
// read-backs) into a third; a normal frame has no CPU wait for the GPU; an off-frame recording is submitted and waited for.
// Here: a ring of command buffers with a fence each for presented frames (a buffer is reused only after its fence signalled: that
// wait is the only pacing wait besides the presenter's), one command buffer for off-frame recordings.
// Single-threaded: every call comes from the engine thread that executes the command lists (mode 1). No game addresses.
#ifndef DM_VKN_FRAME_H
#define DM_VKN_FRAME_H
#include "vkn.h"
#include "vkn_tex.h"
#ifdef __cplusplus
extern "C" {
#endif

// Starts a recording. presented 1: the next command buffer of the frame ring (waits for its previous submission), 0: the off-frame
// one. Queued uploads (vkn_upload_run) are recorded first thing. Runs the begin listeners. 0 = ok; -1 = no device / Vulkan failure
// (then vkn_cb() stays VK_NULL_HANDLE and every recorder must do nothing).
int vkn_rec_begin(int presented);
VkCommandBuffer vkn_cb(void);
VkCommandBuffer vkn_cb_peek(void);     // [rt2 R7] the same handle without emitting a waiting sort window (vkn_sort.h): checks, the draw path          // the open recording's command buffer, VK_NULL_HANDLE = none open
uint32_t vkn_cb_serial(void);          // changes with every vkn_rec_begin: "what I bound is gone" for the recorders' caches
int vkn_rec_presented(void);           // 1 = the open recording is a presented frame
// Listeners (registered once, at install): begin runs right after the command buffer began (state caches reset); close runs right
// before vkEndCommandBuffer (an open render pass must end, images go to their resting layouts). Up to 8 each.
void vkn_rec_on_begin(void (*fn)(void));
void vkn_rec_on_close(void (*fn)(void));
// Ends and submits the open recording (close listeners first). wait 1: returns after the GPU finished it (off-frame recordings and
// read-backs); 0: returns at once (presented frames). 0 = ok.
int vkn_rec_submit(int wait);
// Hands a finished image to the presenter (dm_present_async). Call between the close of the pass that wrote it and vkn_rec_submit:
// records the transition of `win` to TRANSFER_SRC_OPTIMAL into the open recording (win->layout updated), and remembers the present,
// which vkn_rec_submit(0) performs right after its vkQueueSubmit (the presenter's contract: the work producing the image is on the
// queue). crop: {x, y, w, h}, NULL = whole image. interval: the engine's present interval (1 = every vblank, 2 = every second).
int vkn_rec_present(vkn_tex *win, const int32_t crop[4], int interval);
// vkn_frame_end() (vkn.h: the frame counter, deferred destruction) is due with vkn_rec_submit of a presented recording and runs at the
// next vkn_rec_begin(1) once its buffer is free (or at a submit without a recording, after every buffer is free): so a retirement
// never overtakes the GPU (see end_due_run in vkn_frame.c).

typedef struct vkn_frame_stats { uint64_t frames, offframe, submits, waits, wait_ns, begin_fail, present_fail, uploads; } vkn_frame_stats;
void vkn_frame_stats_get(vkn_frame_stats *s);

// ---- additions of the frame piece (stage 3; vkn_frame.c) ----
// Waits (counted like vkn_rec_begin's wait) until the command buffer the next vkn_rec_begin(presented) will use is free, without
// beginning anything: the host calls it before it takes the engine's global mutex, where the NVN path blocked in AcquireTexture.
void vkn_rec_prewait(int presented);
// GPU timestamps (ns of the device's timestamp clock) at the top and the bottom of the newest presented recording the GPU finished.
// 0 = none known (no timestamp support, or nothing finished yet); 1 = *begin_ns / *end_ns written.
int vkn_frame_gpu_ns(uint64_t *begin_ns, uint64_t *end_ns);
// presents: handed to the presenter without error; submit_fail: recordings lost at vkEndCommandBuffer / vkQueueSubmit; left_open:
// recordings still open at the next vkn_rec_begin (submitted then); fence_timeouts: buffers the GPU did not finish in 5 s;
// present_waits / present_wait_ns: waits in dm_present_wait_submitted before a submission (present thread).
typedef struct vkn_frame_xstats { uint64_t presents, submit_fail, left_open, fence_timeouts, present_waits, present_wait_ns; } vkn_frame_xstats;
void vkn_frame_xstats_get(vkn_frame_xstats *s);

// ==== [stage 5.5] the recording thread (vkn_rt.h): "single-threaded" above means the recording's owner, which may change between the
// engine thread and the recording thread at a sync point; these may be called from the engine thread while the other thread records ====
// vkn_frame_gpu_ns: any thread (the pair is read as one). vkn_frame_gpu_done: the newest presented recording the GPU finished, as the
// vkn_frame() its draws saw (*any = 0: none yet). vkn_frame_wait_frame (the owner): waits until every submitted presented recording of
// frame <= f finished. vkn_rec_end_due (the owner): a presented submit's frame end is due (vkn_frame() + 1 at the next presented begin).
uint64_t vkn_frame_gpu_done(int *any);
void vkn_frame_wait_frame(uint64_t f);
int vkn_rec_end_due(void);

#ifdef __cplusplus
}
#endif
#endif
