// [DOS2] VK-NATIVE stage 3, recording / submit / present (vkn_frame.h). No game addresses, no engine record offsets, no NVN.
// Model: one command pool, a ring of VKN_FR_RING command buffers + fences for presented recordings (a buffer is re-recorded only after
// its fence signalled: that wait, plus the host's pacing sleep and the presenter's own backpressure, is all the CPU ever waits for in
// a normal frame) and one command buffer + fence for off-frame recordings (submitted and waited for, as the engine's ctx+0xd8 path).
// GPU time: a timestamp pair per command buffer (top of the recording, bottom before vkEndCommandBuffer), read when the buffer's fence
// is seen signalled; the newest finished presented pair is vkn_frame_gpu_ns (the host's "GPU frame ms" of the engine).
// Present: the presenter's contract (android/vulkan_present.cpp dm_present_async): the image is in TRANSFER_SRC_OPTIMAL and the work
// that produced it is on the one VkQueue before the call; with DM_GFX_PRESENT_THREAD=1 the call only hands the present to the thread
// "dm-present", which blits later, so every later submission of ours first calls dm_present_wait_submitted() (the blit reads the image
// the next frames write again: same queue order as presenting inline, the translator's ptq::before_submit rule). The presenter takes
// the queue lock itself, so dm_present_async is never called with vkn_queue_lock held. A failed present (inline VkResult or
// dm_present_last_error of the thread) is counted and logged; the old path stopped the game there, stage 3 keeps running.
// Single-threaded (the engine's command-list thread, mode 1), like the header says.
#ifdef DM_GAME_DOS2
#include "vkn_frame.h"
#include "vkn_pass.h"
#include "vkn_prof.h"   // [stage 5 A0] GPU time by pass on a sampled frame (its query regions follow this ring)
#include <stdio.h>
#include <string.h>

// The presenter (android/vulkan_present.cpp). Weak: host tests and other front ends link without them (then nothing is presented).
extern int dm_present_async(VkImage source, VkFormat format, uint32_t width, uint32_t height, const int32_t crop[4], int32_t interval) __attribute__((weak));
extern uint64_t dm_present_wait_submitted(void) __attribute__((weak));
extern int dm_present_last_error(void) __attribute__((weak));
// [VK-DRV] the safe-mode ladder (gfx/vulkan/engine/xbg_safe_mode.inc; weak: host tests). A lost device or a fence that never signals is a
// graphics failure of the backend: counted once per process (the next start, counted, takes the next step; the old path stopped the game
// with a fatal there, the backend keeps running as before). The tick runs the DM_GFX_FAKE_GPU_CRASH lost / hang test (nothing unless armed).
extern int xbg_safe_mode_record(const char *why) __attribute__((weak));
extern void xbg_safe_mode_vkn_tick(int where) __attribute__((weak));
static void safe_count(const char *what, VkResult r) {
    if (!xbg_safe_mode_record || (r != VK_ERROR_DEVICE_LOST && r != VK_TIMEOUT)) return;
    char w[160]; snprintf(w, sizeof w, "%s %s in the native backend (vkn_frame)", r == VK_ERROR_DEVICE_LOST ? "VK_ERROR_DEVICE_LOST in" : "GPU hang: fence wait timed out in", what);
    if (xbg_safe_mode_record(w) > 0) vkn_logf("\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91" "frame: %s: counted for the safe-mode ladder (gfx-safe-mode)", w);
}

#define FR_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"   // the red-line mark, a literal of its own (the next letter may be a hex digit)

enum { VKN_FR_RING = 3, VKN_FR_OFF = VKN_FR_RING, VKN_FR_NCB = VKN_FR_RING + 1, VKN_FR_LISTEN = 8 };
enum { FENCE_WAIT_NS = 5000000000ull };   // a fence that takes longer is a lost / hung device: the buffer is never reused then
// Unexpected conditions (each prints its first 16 occurrences as red lines).
enum { RL_INIT, RL_WAIT, RL_BEGIN, RL_END, RL_SUBMIT, RL_PRESENT, RL_PRESENT_LATE, RL_OPEN, RL_LISTEN, RL_NONE, RL_N };

typedef struct fr_slot {
    VkCommandBuffer cb; VkFence fence;
    uint8_t pending;    // submitted, fence not yet seen signalled
    uint8_t ts;         // the timestamp pair of this buffer was written by its last recording
    uint64_t frame;     // [stage 5.5] vkn_frame() of its last presented recording (vkn_frame_gpu_done)
} fr_slot;

static struct {
    int init; uint32_t init_tries;
    VkDevice dev; VkQueue queue;
    VkCommandPool pool; fr_slot s[VKN_FR_NCB];
    VkQueryPool qp; double ts_period;                       // qp = VK_NULL_HANDLE: no timestamps
    uint32_t next;                                          // the next ring slot of a presented recording
    int cur, cur_presented;                                 // cur = open slot, -1 = none
    uint32_t serial;
    int want_presented;                                     // vkn_rec_begin(1) was asked since the last presented submit (even if it failed)
    int end_due;                                            // a presented recording was submitted: its vkn_frame_end waits for the next buffer (end_due_run)
    struct { int have; vkn_tex *tex; VkImage image; VkFormat fmt; uint32_t w, h; int32_t crop[4]; int has_crop, interval; } pres;
    void (*on_begin[VKN_FR_LISTEN])(void); int nbegin;
    void (*on_close[VKN_FR_LISTEN])(void); int nclose;
    vkn_frame_stats st; vkn_frame_xstats xs;
    uint64_t gpu_b, gpu_e; int gpu_ok;                       // newest finished presented timestamp pair (ns); [stage 5.5] seqlock gpu_seq
    uint32_t gpu_seq;
    uint64_t done_frame; int done_any;                       // [stage 5.5] newest presented recording seen finished (atomics)
    int last_err;                                           // last dm_present_last_error value seen
    uint32_t red[RL_N];
} g = {.cur = -1};

static int red_ok(int k) { return g.red[k]++ < 16; }

// ---- set-up (lazy: the device exists before the first guest instruction, but a failure is retried on later frames) ----
static void fr_drop(void) {
    if (g.dev) {
        for (int i = 0; i < VKN_FR_NCB; i++) if (g.s[i].fence) vkDestroyFence(g.dev, g.s[i].fence, NULL);
        if (g.pool) vkDestroyCommandPool(g.dev, g.pool, NULL);   // frees its command buffers
        if (g.qp) vkDestroyQueryPool(g.dev, g.qp, NULL);
    }
    memset(g.s, 0, sizeof g.s); g.pool = VK_NULL_HANDLE; g.qp = VK_NULL_HANDLE;
}
static int fr_init(void) {
    if (g.init) return 0;
    const vkn_dev *d = vkn_device();
    if (!d) return -1;   // no device yet: not an error of ours (vkn_rec_begin counts it)
    g.init_tries++;
    g.dev = d->dev; g.queue = d->queue;
    const VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = d->family};
    VkResult r = vkCreateCommandPool(g.dev, &pi, NULL, &g.pool);
    VkCommandBuffer cbs[VKN_FR_NCB] = {0};
    if (r == VK_SUCCESS) {
        const VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = VKN_FR_NCB};
        r = vkAllocateCommandBuffers(g.dev, &ai, cbs);
    }
    for (int i = 0; i < VKN_FR_NCB && r == VK_SUCCESS; i++) {
        const VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        g.s[i].cb = cbs[i];
        r = vkCreateFence(g.dev, &fi, NULL, &g.s[i].fence);
    }
    if (r != VK_SUCCESS) {
        if (red_ok(RL_INIT)) vkn_logf(FR_RED "frame: command pool / buffers / fences not created (VkResult %d, try %u): no native frame", (int)r, g.init_tries);
        fr_drop(); return -1;
    }
    // Timestamps: every graphics queue has them when timestampComputeAndGraphics is set; else no GPU time (the host falls back).
    if (d->props.limits.timestampComputeAndGraphics && d->props.limits.timestampPeriod > 0) {
        const VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * VKN_FR_NCB};
        if (vkCreateQueryPool(g.dev, &qi, NULL, &g.qp) != VK_SUCCESS) g.qp = VK_NULL_HANDLE;
        g.ts_period = d->props.limits.timestampPeriod;
    }
    g.init = 1;
    vkn_logf("frame: native recording ready: %d presented command buffers + 1 off-frame, GPU timestamps %s", VKN_FR_RING, g.qp ? "on" : "off");
    return 0;
}

// The fence of slot k signalled: the buffer is free; its timestamp pair (if any) is the newest finished GPU time of a presented one.
static void slot_done(int k) {
    fr_slot *s = &g.s[k];
    s->pending = 0;
    if (s->ts && g.qp && k < VKN_FR_RING) {
        uint64_t v[2] = {0, 0};
        if (vkGetQueryPoolResults(g.dev, g.qp, 2u * (uint32_t)k, 2, sizeof v, v, sizeof v[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && v[1] >= v[0]) {
            // [stage 5.5] a seqlock: the engine thread reads the pair (vkn_frame_gpu_ns) while the recording thread writes it
            __atomic_add_fetch(&g.gpu_seq, 1, __ATOMIC_ACQ_REL);
            __atomic_store_n(&g.gpu_b, (uint64_t)((double)v[0] * g.ts_period), __ATOMIC_RELAXED); __atomic_store_n(&g.gpu_e, (uint64_t)((double)v[1] * g.ts_period), __ATOMIC_RELAXED);
            __atomic_store_n(&g.gpu_ok, 1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&g.gpu_seq, 1, __ATOMIC_ACQ_REL);
        }
    }
    if (k < VKN_FR_RING && (!__atomic_load_n(&g.done_any, __ATOMIC_RELAXED) || s->frame > __atomic_load_n(&g.done_frame, __ATOMIC_RELAXED))) {   // [stage 5.5]
        __atomic_store_n(&g.done_frame, s->frame, __ATOMIC_RELEASE); __atomic_store_n(&g.done_any, 1, __ATOMIC_RELEASE);
    }
    s->ts = 0;
    vkn_prof_slot_done(k);   // [stage 5 A0] a sampled recording of this buffer: its timestamps are final now
}
// Waits until slot k is free. 0 = free; -1 = still in flight after FENCE_WAIT_NS or the device failed (never reuse it then).
static int slot_wait(int k) {
    fr_slot *s = &g.s[k];
    if (!s->pending) return 0;
    VkResult r = vkGetFenceStatus(g.dev, s->fence);
    if (r == VK_NOT_READY) {
        const uint64_t t0 = vkn_now_ns();
        r = vkWaitForFences(g.dev, 1, &s->fence, VK_TRUE, FENCE_WAIT_NS);
        g.st.waits++; g.st.wait_ns += vkn_now_ns() - t0;
    }
    if (r != VK_SUCCESS) {
        g.xs.fence_timeouts++;
        safe_count("vkWaitForFences", r);   // [VK-DRV]
        if (red_ok(RL_WAIT)) vkn_logf(FR_RED "frame: command buffer %d not finished by the GPU (VkResult %d after %.1f s): the recording is dropped", k, (int)r, FENCE_WAIT_NS / 1e9);
        return -1;
    }
    slot_done(k);
    return 0;
}

static int next_slot(int presented) { return presented ? (int)(g.next % VKN_FR_RING) : VKN_FR_OFF; }
// The frame end of the last presented submission (vkn.h: frame counter + due retirements), run once the next presented buffer is
// free. Why not at the submit: an object retired while recording N (staging of an upload recorded in N, a transient index block, a
// descriptor pool of a mid-recording flush) is destroyed at the 3rd frame end after it. Ended at the submits of N, N+1, N+2, it would
// go while N may still run (N+2's buffer wait proved only N-1 done). Ended here, the 3rd end is at N+3's begin, after the wait that
// proves N finished. Mode 1 only; the host's mode-2 frame ends are untouched.
static void end_due_run(void) { if (g.end_due) { g.end_due = 0; vkn_frame_end(); } }

void vkn_rec_prewait(int presented) {
    if (g.cur >= 0 || fr_init()) return;
    (void)slot_wait(next_slot(presented));
}

int vkn_rec_begin(int presented) {
    presented = presented ? 1 : 0;
    g.serial++;
    if (g.cur >= 0) {   // the engine began a recording without ending the last one: submit that one first (an off-frame one with its wait)
        g.xs.left_open++;
        if (red_ok(RL_OPEN)) vkn_logf(FR_RED "frame: a %s recording was still open at the begin of a %s one: submitted first", g.cur_presented ? "presented" : "off-frame", presented ? "presented" : "off-frame");
        (void)vkn_rec_submit(!g.cur_presented);
        g.serial++;
    }
    if (presented) g.want_presented = 1;
    if (fr_init()) { g.st.begin_fail++; return -1; }
    const int k = next_slot(presented);
    fr_slot *s = &g.s[k];
    if (slot_wait(k)) { g.st.begin_fail++; return -1; }
    if (presented) end_due_run();   // buffer k is free: the recording 3 before this one finished
    const VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VkResult r = vkResetFences(g.dev, 1, &s->fence);
    if (r == VK_SUCCESS) r = vkResetCommandBuffer(s->cb, 0);
    if (r == VK_SUCCESS) r = vkBeginCommandBuffer(s->cb, &bi);
    if (r != VK_SUCCESS) {
        g.st.begin_fail++;
        if (red_ok(RL_BEGIN)) vkn_logf(FR_RED "frame: command buffer %d could not begin (VkResult %d): the %s recording is dropped", k, (int)r, presented ? "presented" : "off-frame");
        return -1;
    }
    if (presented) { g.next++; s->frame = vkn_frame(); }   // [stage 5.5] (after the due frame end: the frame its draws see)
    g.cur = k; g.cur_presented = presented;
    if (g.qp) {
        vkCmdResetQueryPool(s->cb, g.qp, 2u * (uint32_t)k, 2);
        vkCmdWriteTimestamp(s->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.qp, 2u * (uint32_t)k);
    }
    vkn_prof_rec_begin(s->cb, k, presented);   // [stage 5 A0] (before the uploads: they count as "outside passes")
    g.st.uploads += (uint64_t)vkn_upload_run(s->cb);   // queued uploads first: the frame's commands may read what they fill
    for (int i = 0; i < g.nbegin; i++) g.on_begin[i]();
    return 0;
}

VkCommandBuffer vkn_cb(void) { return g.cur >= 0 ? g.s[g.cur].cb : VK_NULL_HANDLE; }
uint32_t vkn_cb_serial(void) { return g.serial; }
int vkn_rec_presented(void) { return g.cur >= 0 && g.cur_presented; }

void vkn_rec_on_begin(void (*fn)(void)) {
    if (!fn) return;
    for (int i = 0; i < g.nbegin; i++) if (g.on_begin[i] == fn) return;
    if (g.nbegin < VKN_FR_LISTEN) g.on_begin[g.nbegin++] = fn;
    else if (red_ok(RL_LISTEN)) vkn_logf(FR_RED "frame: more than %d begin listeners: one is not called", VKN_FR_LISTEN);
}
void vkn_rec_on_close(void (*fn)(void)) {
    if (!fn) return;
    for (int i = 0; i < g.nclose; i++) if (g.on_close[i] == fn) return;
    if (g.nclose < VKN_FR_LISTEN) g.on_close[g.nclose++] = fn;
    else if (red_ok(RL_LISTEN)) vkn_logf(FR_RED "frame: more than %d close listeners: one is not called", VKN_FR_LISTEN);
}

int vkn_rec_present(vkn_tex *win, const int32_t crop[4], int interval) {
    if (g.cur < 0 || !win || !win->image) {
        g.st.present_fail++;
        if (red_ok(RL_PRESENT)) vkn_logf(FR_RED "frame: present without %s: dropped", g.cur < 0 ? "an open recording" : "a window image");
        return -1;
    }
    if (win->samples > 1) {   // the presenter blits; a multisampled source cannot be blitted (the engine's window is 1 sample here)
        g.st.present_fail++;
        if (red_ok(RL_PRESENT)) vkn_logf(FR_RED "frame: present of a %u-sample window image %#llx: dropped", win->samples, (unsigned long long)win->handle);
        return -1;
    }
    if (g.pres.have && red_ok(RL_PRESENT)) vkn_logf(FR_RED "frame: second present in one recording (%#llx after %#llx): the later one is shown", (unsigned long long)win->handle, (unsigned long long)(g.pres.tex ? g.pres.tex->handle : 0));
    vkn_pass_layout(win, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    g.pres.have = 1; g.pres.tex = win; g.pres.image = win->image; g.pres.fmt = win->format; g.pres.w = win->w; g.pres.h = win->h; g.pres.interval = interval;
    g.pres.has_crop = 0;
    if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] >= 0 && crop[1] >= 0 && (uint32_t)crop[0] < win->w && (uint32_t)crop[1] < win->h) {
        g.pres.has_crop = 1; memcpy(g.pres.crop, crop, sizeof g.pres.crop);
        if ((uint32_t)g.pres.crop[2] > win->w - (uint32_t)crop[0]) g.pres.crop[2] = (int32_t)(win->w - (uint32_t)crop[0]);   // inside the image
        if ((uint32_t)g.pres.crop[3] > win->h - (uint32_t)crop[1]) g.pres.crop[3] = (int32_t)(win->h - (uint32_t)crop[1]);
    }
    return 0;
}

// The present that the open recording produced, right after its submission (no lock of ours held: the presenter takes the queue lock).
static void do_present(void) {
    if (!dm_present_async) { g.st.present_fail++; if (red_ok(RL_PRESENT)) vkn_logf(FR_RED "frame: no presenter in this build: frame not shown"); return; }
    const int r = dm_present_async(g.pres.image, g.pres.fmt, g.pres.w, g.pres.h, g.pres.has_crop ? g.pres.crop : NULL, g.pres.interval);
    if (r) {
        g.st.present_fail++;
        if (red_ok(RL_PRESENT)) vkn_logf(FR_RED "frame: present of %ux%u format %d failed: %d", g.pres.w, g.pres.h, (int)g.pres.fmt, r);
    } else g.xs.presents++;
}
// Before every submission: the presents handed to the present thread have their blits on the queue (DM_GFX_PRESENT_THREAD=1; a call
// returning 0 at once otherwise), and a failure the thread reported since is counted once.
static void before_submit(void) {
    if (dm_present_wait_submitted) { const uint64_t w = dm_present_wait_submitted(); if (w) { g.xs.present_waits++; g.xs.present_wait_ns += w; } }
    if (dm_present_last_error) {
        const int e = dm_present_last_error();
        if (e && e != g.last_err) { g.st.present_fail++; if (red_ok(RL_PRESENT_LATE)) vkn_logf(FR_RED "frame: the present thread reported a failed present: VkResult %d", e); }
        g.last_err = e;
    }
}
// A presented frame without an open recording (its begin failed): the frame still ends, once nothing of ours is in flight (deferred
// destruction must never overtake the GPU).
static void frame_end_idle(void) {
    for (int k = 0; k < VKN_FR_NCB; k++) if (g.init && slot_wait(k)) return;   // stuck: no frame end (retirements wait)
    end_due_run();   // everything finished: the due end of the last submitted frame, then this one's
    vkn_frame_end();
}

int vkn_rec_submit(int wait) {
    if (g.cur < 0) {
        g.pres.have = 0;
        if (g.want_presented && !wait) { g.want_presented = 0; frame_end_idle(); }
        if (red_ok(RL_NONE) && g.init) vkn_logf(FR_RED "frame: submit without an open recording (%s)", wait ? "off-frame" : "presented");
        return -1;
    }
    const int k = g.cur, presented = g.cur_presented;
    fr_slot *s = &g.s[k];
    for (int i = 0; i < g.nclose; i++) g.on_close[i]();
    // A close listener may have moved images to their resting layouts: the presented image must stay TRANSFER_SRC (the presenter's contract).
    if (g.pres.have && g.pres.tex && g.pres.tex->layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) vkn_pass_layout(g.pres.tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkn_prof_rec_close(s->cb, k);   // [stage 5 A0] after the close listeners (the last pass ended)
    if (g.qp) { vkCmdWriteTimestamp(s->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.qp, 2u * (uint32_t)k + 1); s->ts = 1; }
    g.cur = -1;   // from here on recorders see no command buffer
    int rc = 0;
    VkResult r = vkEndCommandBuffer(s->cb);
    if (r != VK_SUCCESS) {
        rc = -1; g.xs.submit_fail++; s->ts = 0;
        if (red_ok(RL_END)) vkn_logf(FR_RED "frame: vkEndCommandBuffer of buffer %d failed (VkResult %d): the recording is lost", k, (int)r);
    } else {
        before_submit();
        if (xbg_safe_mode_vkn_tick) xbg_safe_mode_vkn_tick(1);   // [VK-DRV] the lost / hang test, if armed
        const VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &s->cb};
        vkn_queue_lock(); r = vkQueueSubmit(g.queue, 1, &si, s->fence); vkn_queue_unlock();
        if (r == VK_SUCCESS) { s->pending = 1; g.st.submits++; }
        else {
            rc = -1; g.xs.submit_fail++; s->ts = 0;
            safe_count("vkQueueSubmit", r);   // [VK-DRV]
            if (red_ok(RL_SUBMIT)) vkn_logf(FR_RED "frame: vkQueueSubmit of buffer %d failed (VkResult %d): the recording is lost", k, (int)r);
        }
    }
    if (presented) g.st.frames++; else g.st.offframe++;
    if (g.pres.have) {
        if (rc == 0) do_present();
        else { g.st.present_fail++; if (red_ok(RL_PRESENT)) vkn_logf(FR_RED "frame: present dropped: its recording was not submitted"); }
        g.pres.have = 0; g.pres.tex = NULL;
    }
    if (rc == 0 && wait && slot_wait(k)) rc = -1;
    if (presented) { g.want_presented = 0; end_due_run(); g.end_due = 1; }   // (a still-due end runs first: one end per frame)
    return rc;
}

void vkn_frame_stats_get(vkn_frame_stats *s) { *s = g.st; }
void vkn_frame_xstats_get(vkn_frame_xstats *s) { *s = g.xs; }
int vkn_frame_gpu_ns(uint64_t *begin_ns, uint64_t *end_ns) {
    // [stage 5.5] any thread: the pair as one (seqlock against slot_done on the recording thread)
    for (int i = 0; i < 64; i++) {
        const uint32_t s0 = __atomic_load_n(&g.gpu_seq, __ATOMIC_ACQUIRE);
        if (s0 & 1) continue;
        const int ok = __atomic_load_n(&g.gpu_ok, __ATOMIC_RELAXED);
        const uint64_t b = __atomic_load_n(&g.gpu_b, __ATOMIC_RELAXED), e = __atomic_load_n(&g.gpu_e, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g.gpu_seq, __ATOMIC_RELAXED) != s0) continue;
        if (!ok) return 0;
        *begin_ns = b; *end_ns = e;
        return 1;
    }
    return 0;
}
// ---- [stage 5.5] the recording thread (vkn_rt.c) ----
int vkn_rec_end_due(void) { return g.end_due; }   // the recording's owner
uint64_t vkn_frame_gpu_done(int *any) {           // any thread
    const int a = __atomic_load_n(&g.done_any, __ATOMIC_ACQUIRE);
    if (any) *any = a;
    return a ? __atomic_load_n(&g.done_frame, __ATOMIC_ACQUIRE) : 0;
}
void vkn_frame_wait_frame(uint64_t f) {           // the recording's owner: every submitted presented buffer of frame <= f finished
    if (!g.init) return;
    for (int k = 0; k < VKN_FR_RING; k++) if (g.s[k].pending && g.s[k].frame <= f) (void)slot_wait(k);
}
#endif
