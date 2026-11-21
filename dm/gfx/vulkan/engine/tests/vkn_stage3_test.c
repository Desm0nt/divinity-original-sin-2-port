// [DOS2] VK-NATIVE stage 3 host test (docs/dos2/nr/VK-NATIVE.md §4.2-4.4; the contracts at the top of dm/gfx/vkn/vkn_frame.h,
// vkn_pass.h, vkn_draw.h are the specification): the recording / pass / draw modules (+ vkn_tex_update, a UI smoke draw) over the
// stage-2 resource modules, against the mock of vkn_mock_vk.h with its stage-3 part on (command log, fence hold mode, the submit-time
// validator: layouts per image, render pass pairing, commands legal inside / outside a pass, feedback, set / pipeline compatibility).
//   frame    no device, init retry, begin / submit order (uploads first, listeners, close before end), ring of 3 with fence waits,
//            prewait, off-frame wait, serials, begin while open, present after the submit outside the queue lock with the image in
//            TRANSFER_SRC (crop, interval, errors), failure paths, GPU timestamps, deferred destruction never overtakes the GPU;
//   pass     lazy begin, same / different sets, clear folding (colour layers, integer formats, depth, stencil, both), clears inside a
//            pass, region clears, remembered clears run before a sample / copy / present, LOAD vs DONT_CARE, feedback, sampling a
//            texture of an earlier pass (barrier old / new layout and aspect), copies (0x920b00: equal levels, level tail, arrays,
//            scaled, format mismatch), depth-only sets, (mip, layer) attachments, the close listener, a validator self-test;
//   draw     a full draw (pipeline, set with dynamic offsets in table order, used vertex streams, index buffer, viewport, scissor,
//            draw), nothing rebound for an identical draw, only the changed thing rebound, everything in a new recording, uniform
//            ranges, the zero block, the null stream, placeholders (unknown / empty / feedback), unknown sampler, drops (missing
//            stage, unknown state / shader / vertex buffer, index range, viewport), alpha variant, scissor clamp, arrays, index rewrite
//            of fans / quads / line loops (generated indices checked), the set cache and its retirement, compute dispatch;
//            [stage 5] draw effects (vkn_fx.h); draw cost (DM_VK_NATIVE_COST; [stage 5.1 G] the real texture / state generations, the
//            texture reuse after a remembered clear and a target change, bit VFOLD alone, a piece's draw nested in the chain); [stage 5.1 O2] DM_VK_NATIVE_GBUF_LE: the EQUAL -> LEQUAL swap of a conditional draw
//            whose pair holds the same draw drawn whole, and each refusal reason (no pair, other draw, alpha test, discard / unknown
//            fragment facts, pre-pass depth state, depth raised / clear pending, several draws, pre-pass draw skipped, wider scissor),
//            count-only mode, the tile-model gate;
//            [stage 5.1 A4] LIGHTFUSE (vkn_lightfuse.c): a learned chain skips Sky and draws the fused shader with Sky's stashed
//            textures / blocks; a skipped Sky whose Light does not come is drawn late (exact) at the next draw;
//   end to end  one synthetic frame (targets A with clears, 3 draws, targets B sampling A, 1 draw, present B) checked as a whole;
//   thumbnail  shared render-target storage (vkn_tex_create_shared): the save-game thumbnail of 0xa66bc0 - clear the override, draw
//            the scene into its alias, copy the override: one image, the clear folded into the scene pass, the copy reads the drawn
//            image; the same with separate images reproduces phone run V5_b (a cleared, never-drawn source);
//   queries  (stage 4, vkn_query.c) report pairs as occlusion queries inside a pass, a pair broken by a pass end, the copy + barrier
//            after the pass, consumers visible / hidden (GPU predicate) / empty pair (CPU skip) / unknown / same pass / dropped draw /
//            nesting / off-frame, slot reuse with the verdict readback, modes 0 / 2 / 3, a device without the extension.
//   [stage 5.1 A5] the test of the assembled stage 5.1 (the shadow cache against the mock's depth content model in forked children, more
//            of the effect chain, the occlusion oracle): the section after "END OF AREAS" (its own head comment).
// Ends with "vkn_stage3_test: N checks passed" (exit 0) or the failures and exit 1.
//
// Build + run (WSL gcc, as vkn_stage2_test.c; <vk> = a directory holding only vulkan/ and vk_video/ of the NDK sysroot include):
//   wsl gcc -std=c11 -D_GNU_SOURCE -DDM_GAME_DOOM=1 -DDM_GAME_D3=1 -DDM_GAME_TL2=1 -DDM_GAME_DOS2=1 -O1 -g -Wall -Wextra -Wno-unused-parameter -I<vk> -o /tmp/vkn_stage3_test
//       dm/gfx/vulkan/engine/tests/vkn_stage3_test.c dm/gfx/vkn/vkn_core.c dm/gfx/vkn/vkn_buf.c dm/gfx/vkn/vkn_tex.c dm/gfx/vkn/vkn_shader.c
//       dm/gfx/vkn/vkn_state.c dm/gfx/vkn/vkn_astc.c dm/gfx/vkn/vkn_bcgpu.c dm/gfx/vkn/vkn_frame.c dm/gfx/vkn/vkn_pass.c dm/gfx/vkn/vkn_draw.c dm/gfx/vkn/vkn_fx.c dm/gfx/vkn/vkn_ui.c dm/gfx/vkn/vkn_query.c dm/gfx/vkn/vkn_prof.c dm/gfx/vkn/vkn_shc.c dm/gfx/vkn/vkn_lightfuse.c
//       dm/gfx/vkn/vkn_post.c dm/gfx/vkn/vkn_rt.c dm/gfx/nb/nb_plan.c dm/gfx/vkn/vkn_mat.c dm/gfx/nr/nr_mat.c dm/gfx/vkn/vkn_prepass.c
//       dm/gfx/nvn/nvn_tex.c dm/gfx/vkn/vkn_occ.c dm/gfx/vkn/vkn_drv.c dm/gfx/vkn/vkn_preskin.c
//       dm/gfx/tex/nvn_deswizzle.c dm/gfx/tex/nvn_bcdec.c dm/gfx/nr/nr_reg.c dm/gfx/nr/nr_sh.c -lpthread -lm && wsl /tmp/vkn_stage3_test
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkn_mock_vk.h"
#include "../../../vkn/vkn.h"
#include "../../../vkn/vkn_buf.h"
#include "../../../vkn/vkn_tex.h"
#include "../../../vkn/vkn_shader.h"
#include "../../../vkn/vkn_state.h"
#include "../../../vkn/vkn_frame.h"
#include "../../../vkn/vkn_pass.h"
#include "../../../vkn/vkn_draw.h"
#include "../../../vkn/vkn_fx.h"
#include "../../../vkn/vkn_ui.h"
#include "../../../vkn/vkn_query.h"
#include "../../../vkn/vkn_lightfuse.h"   // [stage 5.1 A4]
#include "../../../vkn/vkn_post.h"     // [stage 5.2 B3] test_post (end of file)
#include "../../../vkn/vkn_rt.h"       // [stage 5.5] test_rt
#include <sched.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../../../nb/nb_fused.h"

static int g_checks, g_fails;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { const long long a_ = (long long)(a), b_ = (long long)(b); g_checks++; \
    if (a_ != b_) { g_fails++; printf("FAIL %s:%d: %s == %s (%lld / %#llx vs %lld / %#llx)\n", __FILE__, __LINE__, #a, #b, a_, (unsigned long long)a_, b_, (unsigned long long)b_); } } while (0)
#define CHECK_F(a, b) do { const double a_ = (a), b_ = (b); g_checks++; if (fabs(a_ - b_) > 1e-6) { g_fails++; printf("FAIL %s:%d: %s == %s (%g vs %g)\n", __FILE__, __LINE__, #a, #b, a_, b_); } } while (0)
#define H(idx, gen) ((uint64_t)(gen) << 32 | (uint32_t)(idx))
#define VH(x) ((uint64_t)(uintptr_t)(x))

// ================================================================================================================ presenter doors
// Strong definitions of the weak externs of vkn_frame.c (android/vulkan_present.cpp on the phone).
static int pr_rc, pr_err, pr_calls, pr_wait_calls; static uint64_t pr_wait_ret;
static struct { VkImage img; VkFormat fmt; uint32_t w, h; int has_crop; int32_t crop[4]; int interval, qlock; VkImageLayout gpu_layout; uint64_t seq; } pr_last;
int dm_present_async(VkImage src, VkFormat fmt, uint32_t w, uint32_t h, const int32_t crop[4], int32_t interval) {
    pthread_mutex_lock(&mk_mu);
    mk_ev *e = mk_log(VK_NULL_HANDLE, MK_E_PRESENT); e->a[0] = VH(src); e->qlock = mk_qlock_depth;
    mk_obj *o = mk_find(VH(src), MK_IMAGE);
    pr_last.img = src; pr_last.fmt = fmt; pr_last.w = w; pr_last.h = h; pr_last.has_crop = crop != NULL; if (crop) memcpy(pr_last.crop, crop, sizeof pr_last.crop);
    pr_last.interval = interval; pr_last.qlock = mk_qlock_depth; pr_last.gpu_layout = o ? o->gpu_layout : (VkImageLayout)-1; pr_last.seq = e->seq;
    pr_calls++;
    pthread_mutex_unlock(&mk_mu);
    return pr_rc;
}
uint64_t dm_present_wait_submitted(void) { pthread_mutex_lock(&mk_mu); mk_log(VK_NULL_HANDLE, MK_E_PRESENT_WAIT)->qlock = mk_qlock_depth; pr_wait_calls++; pthread_mutex_unlock(&mk_mu); return pr_wait_ret; }
int dm_present_last_error(void) { return pr_err; }

// ================================================================================================================ log helpers
static uint64_t mark(void) { return mk_seq; }
static int skip_op(int op) { return op == MK_E_RESET_QP || op == MK_E_TIMESTAMP; }
// Entries since `from` of command buffer cb (VK_NULL_HANDLE = every entry) and op (0 = any; timestamps skipped).
static int cnt(uint64_t from, VkCommandBuffer cb, int op) {
    int n = 0;
    for (uint64_t s = from; s < mk_seq; s++) { const mk_ev *e = mk_ev_at(s); if (e && (!cb || e->cb == cb) && (op ? e->op == op : !skip_op(e->op))) n++; }
    return n;
}
static const mk_ev *nth(uint64_t from, VkCommandBuffer cb, int op, int k) {
    for (uint64_t s = from; s < mk_seq; s++) { const mk_ev *e = mk_ev_at(s); if (e && (!cb || e->cb == cb) && e->op == op && !k--) return e; }
    return NULL;
}
static int ops(uint64_t from, VkCommandBuffer cb, int *o, int max) {   // the op sequence (timestamps skipped)
    int n = 0;
    for (uint64_t s = from; s < mk_seq && n < max; s++) { const mk_ev *e = mk_ev_at(s); if (e && (!cb || e->cb == cb) && !skip_op(e->op)) o[n++] = e->op; }
    return n;
}
static int ops_are(uint64_t from, VkCommandBuffer cb, const int *want, int n) {
    int got[64]; const int k = ops(from, cb, got, 64);
    int ok = k == n; for (int i = 0; ok && i < n; i++) ok = got[i] == want[i];
    if (!ok) { printf("  ops:"); for (int i = 0; i < k; i++) printf(" %s", mk_ev_name[got[i]]); printf("\n  want:"); for (int i = 0; i < n; i++) printf(" %s", mk_ev_name[want[i]]); printf("\n"); }
    return ok;
}
#define OPS(from, cb, ...) do { const int w_[] = {__VA_ARGS__}; CHECK(ops_are(from, cb, w_, (int)(sizeof w_ / sizeof w_[0]))); } while (0)
#define NO_OPS(from, cb) CHECK_EQ(cnt(from, cb, 0), 0)
static const mk_rp *rp_of(const mk_ev *e) { mk_obj *o = e ? mk_obj_of(e->a[0]) : NULL; return o && o->kind == MK_RP ? &o->rp : NULL; }
static MK_U mk_obj *fb_of(const mk_ev *e) { mk_obj *o = e ? mk_obj_of(e->a[1]) : NULL; return o && o->kind == MK_FB ? o : NULL; }
static MK_U mk_obj *vobj(VkImageView v) { mk_obj *o = mk_obj_of(VH(v)); return o && o->kind == MK_VIEW ? o : NULL; }
static const mk_ev *barrier_of(uint64_t from, VkImage img, int k) {   // the k-th image barrier of img since from
    for (uint64_t s = from; s < mk_seq; s++) { const mk_ev *e = mk_ev_at(s); if (e && e->op == MK_E_BARRIER && e->a[0] == VH(img) && !k--) return e; }
    return NULL;
}
static int pending(VkCommandBuffer cb) { mk_obj *o = mk_obj_of(VH(cb)); return o && o->pend_seq != 0; }

// ================================================================================================================ fixtures
static vkn_tex *mktex(uint32_t idx, uint32_t efmt, uint32_t cls, uint32_t tg, uint32_t w, uint32_t h, uint32_t mips, uint32_t layers, uint32_t pool, int storage) {
    vkn_tex_desc d; memset(&d, 0, sizeof d);
    d.efmt = efmt; d.nvn_target = tg; d.w = w; d.h = h; d.depth = 1; d.mips = mips; d.layers = layers; d.samples = 1; d.cls = cls; d.storage = (uint8_t)storage;
    d.pool_id = pool; for (int i = 0; i < VKN_TEX_LAYER_IDS; i++) d.layer_ids[i] = ~0u;
    d.sampler_id = ~0u; d.cookie = 0xC00000ull + idx;
    const char *why = NULL; vkn_tex *t = vkn_tex_create(H(idx, 1), &d, NULL, &why);
    if (!t) printf("texture %u: %s\n", idx, why ? why : "?");
    return t;
}
enum { EF_RGBA8 = 4, EF_RGBA16F = 20, EF_R32UI = 37, EF_D24S8 = 51, EF_D32F = 52 };
static void targets(vkn_tex *c0, vkn_tex *c1, vkn_tex *d) {
    vkn_tex *c[VKN_PASS_COLORS] = {c0, c1}; uint32_t l[VKN_PASS_COLORS] = {0}, m[VKN_PASS_COLORS] = {0};
    vkn_pass_targets(c1 ? 2 : c0 ? 1 : 0, c, l, m, d);
}
static MK_U void target_lm(vkn_tex *t, uint32_t layer, uint32_t mip) {
    vkn_tex *c[VKN_PASS_COLORS] = {t}; uint32_t l[VKN_PASS_COLORS] = {layer}, m[VKN_PASS_COLORS] = {mip};
    vkn_pass_targets(1, c, l, m, NULL);
}
static VkCommandBuffer fbegin(int presented) { const int r = vkn_rec_begin(presented); CHECK_EQ(r, 0); return vkn_cb(); }
static void fsubmit(int wait) { const int r = vkn_rec_submit(wait); CHECK_EQ(r, 0); }
static void frames_n(int n) { for (int i = 0; i < n; i++) { fbegin(1); fsubmit(0); } }

// listeners of the test (registered after the pass / draw ones, as the host registers its modules first)
static int g_lis, g_lb, g_lc; static VkCommandBuffer g_lb_cb, g_lc_cb;
static void marker(VkCommandBuffer cb, VkPipelineStageFlags s) { vkCmdPipelineBarrier(cb, s, s, 0, 0, NULL, 0, NULL, 0, NULL); }
static void lis_begin(void) { g_lb++; g_lb_cb = vkn_cb(); if (g_lis) marker(vkn_cb(), VK_PIPELINE_STAGE_HOST_BIT); }
static void lis_close(void) { g_lc++; g_lc_cb = vkn_cb(); if (g_lis) marker(vkn_cb(), VK_PIPELINE_STAGE_VERTEX_SHADER_BIT); }
static void up_marker(VkCommandBuffer cb, void *ctx) { (void)ctx; marker(cb, VK_PIPELINE_STAGE_TRANSFER_BIT); }

// ================================================================================================================ frame
static void test_no_device(void) {
    mk_dev_on = 0;
    vkn_frame_stats fs; vkn_draw_stats2 d2;
    CHECK_EQ(vkn_rec_begin(1), -1); CHECK(vkn_cb() == VK_NULL_HANDLE); CHECK_EQ(vkn_rec_presented(), 0);
    vkn_frame_stats_get(&fs); CHECK_EQ(fs.begin_fail, 1);
    const uint64_t f0 = vkn_frame();
    CHECK_EQ(vkn_rec_submit(0), -1);
    CHECK_EQ(vkn_frame(), f0 + 1);   // a presented frame without a recording still ends (nothing of ours in flight)
    vkn_pass_info pi; CHECK_EQ(vkn_pass_ready(&pi), -1);
    vkn_draw_in in; memset(&in, 0, sizeof in); in.prim = 1; in.viewport[2] = in.viewport[3] = 8;
    CHECK_EQ(vkn_draw_arrays(&in, 0, 3), 1);
    vkn_draw_stats2_get(&d2); CHECK_EQ(d2.dropped_cb, 1);
    CHECK_EQ(vkn_rec_present(NULL, NULL, 1), -1);
    CHECK_EQ(mk_submits, 0); CHECK_EQ(mk_created[MK_CB], 0); CHECK_EQ(pr_calls, 0);
    mk_dev_on = 1;
    // the device's timestamp support (read at the frame module's set-up)
    vkn_dev *d = (vkn_dev *)vkn_device(); CHECK(d != NULL);
    d->props.limits.timestampComputeAndGraphics = VK_TRUE; d->props.limits.timestampPeriod = 2.0f;
    // set-up failure: retried by the next begin
    mk_fail_cpool = 1;
    CHECK_EQ(vkn_rec_begin(1), -1); CHECK_EQ(vkn_rec_submit(0), -1);
    CHECK_EQ(mk_live(MK_POOL), 0);
    const VkCommandBuffer cb = fbegin(1);
    CHECK(cb != VK_NULL_HANDLE); CHECK_EQ(mk_live(MK_POOL), 1); CHECK_EQ(mk_live(MK_FENCE), 4); CHECK_EQ(mk_live(MK_QPOOL), 1);
    fsubmit(0);
    vkn_frame_stats_get(&fs); CHECK_EQ(fs.begin_fail, 2); CHECK_EQ(fs.frames, 1); CHECK_EQ(fs.submits, 1);
}

static void test_frame_order(void) {
    g_lis = 1; g_lb = g_lc = 0;
    vkn_upload_push(up_marker, NULL, 0);
    const uint32_t s0 = vkn_cb_serial();
    uint64_t m = mark();
    const uint64_t f0 = vkn_frame();
    const VkCommandBuffer cb = fbegin(1);
    CHECK(vkn_cb_serial() != s0); CHECK_EQ(vkn_rec_presented(), 1);
    CHECK_EQ(g_lb, 1); CHECK(g_lb_cb == cb);
    // begin, timestamps (skipped by ops), the queued upload first, then the begin listeners
    OPS(m, cb, MK_E_BEGIN_CB, MK_E_BARRIER, MK_E_BARRIER);
    CHECK_EQ(nth(m, cb, MK_E_BARRIER, 0)->u[0], VK_PIPELINE_STAGE_TRANSFER_BIT); CHECK_EQ(nth(m, cb, MK_E_BARRIER, 1)->u[0], VK_PIPELINE_STAGE_HOST_BIT);
    CHECK_EQ(cnt(m, cb, MK_E_RESET_QP), 1); CHECK_EQ(cnt(m, cb, MK_E_TIMESTAMP), 1);
    m = mark();
    fsubmit(0);
    CHECK(vkn_cb() == VK_NULL_HANDLE); CHECK_EQ(vkn_rec_presented(), 0);
    CHECK_EQ(g_lc, 1); CHECK(g_lc_cb == cb);   // the close listener still saw the recording
    OPS(m, VK_NULL_HANDLE, MK_E_BARRIER, MK_E_END_CB, MK_E_PRESENT_WAIT, MK_E_SUBMIT);
    CHECK_EQ(nth(m, cb, MK_E_BARRIER, 0)->u[0], VK_PIPELINE_STAGE_VERTEX_SHADER_BIT);
    CHECK_EQ(cnt(m, cb, MK_E_TIMESTAMP), 1);   // the bottom timestamp, before the end
    const mk_ev *sub = nth(m, cb, MK_E_SUBMIT, 0); CHECK(sub && sub->qlock == 1);
    CHECK(nth(m, VK_NULL_HANDLE, MK_E_PRESENT_WAIT, 0)->qlock == 0);
    // the frame counter: retirements of this frame wait until the GPU finished it (vkn_frame_end at the next presented begin's
    // buffer, see test_frame_retire)
    CHECK(vkn_frame() == f0 || vkn_frame() == f0 + 1);
    g_lis = 0;
    // off-frame: its own buffer, not presented, waited for, the frame counter unchanged
    const uint64_t f1 = vkn_frame();
    const VkCommandBuffer off = fbegin(0);
    CHECK(off && off != cb); CHECK_EQ(vkn_rec_presented(), 0);
    vkn_frame_stats fs0, fs1; vkn_frame_stats_get(&fs0);
    fsubmit(1);
    vkn_frame_stats_get(&fs1); CHECK_EQ(fs1.offframe, fs0.offframe + 1); CHECK_EQ(vkn_frame(), f1);
    // serials: every begin changes it; listeners registered twice run once
    vkn_rec_on_begin(lis_begin); vkn_rec_on_close(lis_close);
    g_lb = 0; const uint32_t s1 = vkn_cb_serial(); fbegin(1); CHECK(vkn_cb_serial() != s1); CHECK_EQ(g_lb, 1); fsubmit(0);
}

static void test_frame_ring(void) {
    mk_fence_hold = 1;
    const uint64_t w0 = mk_fence_waits;
    vkn_frame_stats fs0, fs1; vkn_frame_stats_get(&fs0);
    const uint64_t m = mark();
    VkCommandBuffer c[3];
    for (int i = 0; i < 3; i++) { c[i] = fbegin(1); fsubmit(0); }
    CHECK(c[0] != c[1] && c[1] != c[2] && c[0] != c[2]);
    CHECK_EQ(mk_fence_waits, w0);   // three buffers: no wait
    CHECK(pending(c[0]) && pending(c[1]) && pending(c[2]));
    // the 4th presented recording reuses the first buffer after its fence
    const VkCommandBuffer c3 = fbegin(1);
    CHECK(c3 == c[0]); CHECK_EQ(mk_fence_waits, w0 + 1);
    vkn_frame_stats_get(&fs1); CHECK_EQ(fs1.waits, fs0.waits + 1);
    const mk_ev *sub0 = nth(m, c[0], MK_E_SUBMIT, 0), *wt = nth(m, VK_NULL_HANDLE, MK_E_WAIT, 0);
    CHECK(sub0 && wt && wt->a[0] == sub0->a[0]);   // the wait was on that buffer's own fence
    CHECK(!pending(c[0]) && pending(c[1]) && pending(c[2]));   // queue order: only what was waited for is done
    // GPU time of the finished recording: its two timestamps x the period
    uint64_t b = 0, e = 0;
    CHECK_EQ(vkn_frame_gpu_ns(&b, &e), 1);
    const mk_ev *t0 = nth(m, c[0], MK_E_TIMESTAMP, 0), *t1 = nth(m, c[0], MK_E_TIMESTAMP, 1);
    CHECK(t0 && t1); if (t0 && t1) { CHECK_EQ(b, 2 * t0->a[1]); CHECK_EQ(e, 2 * t1->a[1]); CHECK(e > b); }
    fsubmit(0);
    // prewait: the next buffer is waited for without beginning; the begin then waits no more
    vkn_rec_prewait(1);
    CHECK_EQ(mk_fence_waits, w0 + 2); CHECK(vkn_cb() == VK_NULL_HANDLE); CHECK(!pending(c[1]));
    const VkCommandBuffer c4 = fbegin(1);
    CHECK(c4 == c[1]); CHECK_EQ(mk_fence_waits, w0 + 2);
    fsubmit(0);
    // off-frame: submitted and waited for (queue order: everything before it is done too)
    const VkCommandBuffer off = fbegin(0);
    fsubmit(1);
    CHECK_EQ(mk_fence_waits, w0 + 3); CHECK(!pending(off)); CHECK(!pending(c[2])); CHECK(!pending(c4));
    mk_gpu_finish_all(); mk_fence_hold = 0;
}

// Deferred destruction must never overtake the GPU: an object retired while recording N (a staging block of an upload recorded in
// N, a descriptor pool of a mid-recording cache flush, a full transient index block) may only be destroyed after N finished.
static VkCommandBuffer g_probe_cb; static int g_probe_ran, g_probe_inflight;
static void probe_fn(void *ctx) { (void)ctx; g_probe_ran++; g_probe_inflight = pending(g_probe_cb); }
static void test_frame_retire(void) {
    mk_fence_hold = 1; g_probe_ran = 0; g_probe_inflight = 0;
    g_probe_cb = fbegin(1);
    vkn_retire(probe_fn, NULL);
    fsubmit(0);
    for (int i = 0; i < 4 && !g_probe_ran; i++) { fbegin(1); fsubmit(0); }
    CHECK_EQ(g_probe_ran, 1);
    CHECK_EQ(g_probe_inflight, 0);   // destroyed only after the recording that used it was finished by the GPU
    mk_gpu_finish_all(); mk_fence_hold = 0;
}

static void test_frame_open_and_fail(void) {
    vkn_frame_stats fs0, fs1; vkn_frame_xstats x0, x1;
    vkn_frame_stats_get(&fs0); vkn_frame_xstats_get(&x0);
    // begin while open: the open one is submitted first (an off-frame one with its wait)
    uint64_t m = mark();
    const uint32_t s0 = vkn_cb_serial();
    const VkCommandBuffer a = fbegin(1);
    const VkCommandBuffer o = fbegin(0);
    CHECK(o != a); CHECK_EQ(vkn_rec_presented(), 0); CHECK_EQ(cnt(m, a, MK_E_SUBMIT), 1); CHECK_EQ(cnt(m, a, MK_E_END_CB), 1);
    CHECK_EQ(vkn_cb_serial(), s0 + 3);
    fsubmit(1);
    m = mark();
    const VkCommandBuffer o2 = fbegin(0);
    const VkCommandBuffer p = fbegin(1);
    CHECK(p != o2); CHECK_EQ(cnt(m, o2, MK_E_SUBMIT), 1); CHECK_EQ(vkn_rec_presented(), 1);
    fsubmit(0);
    vkn_frame_xstats_get(&x1); CHECK_EQ(x1.left_open, x0.left_open + 2);
    // a submit without a recording
    CHECK_EQ(vkn_rec_submit(1), -1);
    // vkBeginCommandBuffer fails: nothing open, the frame still ends
    mk_fail_begin = 1;
    CHECK_EQ(vkn_rec_begin(1), -1); CHECK(vkn_cb() == VK_NULL_HANDLE);
    uint64_t f = vkn_frame(); CHECK_EQ(vkn_rec_submit(0), -1); CHECK(vkn_frame() > f);
    // vkEndCommandBuffer fails: the recording is lost; its buffer is usable again
    fbegin(1); mk_fail_end = 1; CHECK_EQ(vkn_rec_submit(0), -1);
    // vkQueueSubmit fails
    fbegin(1); mk_fail_submit = 1; CHECK_EQ(vkn_rec_submit(0), -1);
    frames_n(4);   // every buffer of the ring again
    vkn_frame_stats_get(&fs1); vkn_frame_xstats_get(&x1);
    CHECK_EQ(x1.submit_fail, x0.submit_fail + 2); CHECK_EQ(fs1.begin_fail, fs0.begin_fail + 1);
    // a GPU that does not finish: begin fails (no reuse of a busy buffer), no frame end; later it recovers
    mk_fence_hold = 1;
    frames_n(3);
    mk_fence_stuck = 1;
    CHECK_EQ(vkn_rec_begin(1), -1);
    f = vkn_frame(); CHECK_EQ(vkn_rec_submit(0), -1); CHECK_EQ(vkn_frame(), f);
    vkn_frame_xstats_get(&x1); CHECK(x1.fence_timeouts >= x0.fence_timeouts + 1);
    mk_fence_stuck = 0;
    fbegin(1); fsubmit(0);
    mk_gpu_finish_all(); mk_fence_hold = 0;
    CHECK_EQ(mk_errors, 0);
}

static vkn_tex *g_win;
static void test_present(void) {
    vkn_tex *W = g_win = mktex(900, EF_RGBA8, VKN_TC_WINDOW, 1, 64, 32, 1, 1, ~0u, 0);
    CHECK(W != NULL); if (!W) return;
    vkn_frame_xstats x0, x1; vkn_frame_stats fs0, fs1; vkn_frame_xstats_get(&x0); vkn_frame_stats_get(&fs0);
    // 1: a cleared window never drawn: the remembered clear runs as its own command, then TRANSFER_SRC; presented after the submit
    pr_wait_ret = 1500;
    const VkCommandBuffer cb = fbegin(1);
    const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    vkn_pass_clear_color(W, 0, grey, NULL);
    uint64_t m = mark();
    const int pc = pr_calls;
    CHECK_EQ(vkn_rec_present(W, NULL, 1), 0);
    OPS(m, cb, MK_E_BARRIER, MK_E_CLEAR_COLOR_IMG, MK_E_BARRIER);
    const mk_ev *b0 = nth(m, cb, MK_E_BARRIER, 0), *b1 = nth(m, cb, MK_E_BARRIER, 1), *cl = nth(m, cb, MK_E_CLEAR_COLOR_IMG, 0);
    CHECK(b0->l0 == VK_IMAGE_LAYOUT_UNDEFINED && b0->l1 == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CHECK(b1->l0 == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && b1->l1 == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    CHECK_F(cl->cv[0].color.float32[0], 0.5); CHECK_F(cl->cv[0].color.float32[3], 1.0);
    CHECK_EQ(W->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    CHECK_EQ(pr_calls, pc);   // not yet: after the queue submit
    m = mark();
    fsubmit(0);
    CHECK_EQ(pr_calls, pc + 1);
    const mk_ev *sub = nth(m, cb, MK_E_SUBMIT, 0), *pres = nth(m, VK_NULL_HANDLE, MK_E_PRESENT, 0), *pw = nth(m, VK_NULL_HANDLE, MK_E_PRESENT_WAIT, 0);
    CHECK(sub && pres && pw && pw->seq < sub->seq && sub->seq < pres->seq);
    CHECK_EQ(pr_last.qlock, 0);   // never under the queue lock (the presenter takes it)
    CHECK(pr_last.img == W->image); CHECK_EQ(pr_last.fmt, W->format); CHECK_EQ(pr_last.w, 64); CHECK_EQ(pr_last.h, 32);
    CHECK_EQ(pr_last.has_crop, 0); CHECK_EQ(pr_last.interval, 1);
    CHECK_EQ(pr_last.gpu_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkn_frame_xstats_get(&x1); CHECK_EQ(x1.presents, x0.presents + 1); CHECK(x1.present_waits > x0.present_waits); CHECK(x1.present_wait_ns >= x0.present_wait_ns + 1500);
    pr_wait_ret = 0;
    // 2: the next frame draws into it again (TRANSFER_SRC -> attachment, LOAD: it has content); crop clamped, interval 2
    const VkCommandBuffer cb2 = fbegin(1);
    targets(W, NULL, NULL);
    m = mark();
    vkn_pass_info pi; CHECK_EQ(vkn_pass_ready(&pi), 0);
    const mk_ev *bw = barrier_of(m, W->image, 0), *rp = nth(m, cb2, MK_E_BEGIN_RP, 0);
    CHECK(bw && bw->l0 == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && bw->l1 == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    CHECK(rp && rp_of(rp) && rp_of(rp)->att[0].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && rp_of(rp)->att[0].initialLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    m = mark();
    const int32_t crop[4] = {8, 4, 100, 100};
    CHECK_EQ(vkn_rec_present(W, crop, 2), 0);
    OPS(m, cb2, MK_E_END_RP, MK_E_BARRIER);
    fsubmit(0);
    CHECK_EQ(pr_last.has_crop, 1); CHECK_EQ(pr_last.crop[0], 8); CHECK_EQ(pr_last.crop[1], 4); CHECK_EQ(pr_last.crop[2], 56); CHECK_EQ(pr_last.crop[3], 28);
    CHECK_EQ(pr_last.interval, 2);
    // a bad crop is no crop
    fbegin(1); const int32_t bad[4] = {0, 0, 0, 10}; vkn_rec_present(W, bad, 1); fsubmit(0); CHECK_EQ(pr_last.has_crop, 0);
    // 3: errors: inline, from the present thread (counted once), without a recording, with a failed submit
    vkn_frame_stats_get(&fs0); vkn_frame_xstats_get(&x0);
    pr_rc = -4; fbegin(1); vkn_rec_present(W, NULL, 1); fsubmit(0); pr_rc = 0;
    vkn_frame_stats_get(&fs1); vkn_frame_xstats_get(&x1); CHECK_EQ(fs1.present_fail, fs0.present_fail + 1); CHECK_EQ(x1.presents, x0.presents);
    pr_err = -1000000000;
    frames_n(2);
    vkn_frame_stats_get(&fs0); CHECK_EQ(fs0.present_fail, fs1.present_fail + 1);
    pr_err = 0; frames_n(1);
    CHECK_EQ(vkn_rec_present(W, NULL, 1), -1);
    fbegin(1); vkn_rec_present(W, NULL, 1); mk_fail_submit = 1; const int pc2 = pr_calls; CHECK_EQ(vkn_rec_submit(0), -1); CHECK_EQ(pr_calls, pc2);
    vkn_frame_stats_get(&fs1); CHECK_EQ(fs1.present_fail, fs0.present_fail + 2);
    frames_n(1);
}

// ================================================================================================================ pass
static const VkAttachmentDescription *att_of(const mk_ev *e, uint32_t i) { const mk_rp *r = rp_of(e); return r && i < r->natt ? &r->att[i] : NULL; }
static void test_pass(void) {
    vkn_tex *A = mktex(200, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 200, 0), *B = mktex(201, EF_RGBA16F, VKN_TC_COLOR, 1, 64, 32, 1, 1, 201, 0);
    vkn_tex *D = mktex(202, EF_D24S8, VKN_TC_DEPTH, 1, 64, 32, 1, 1, 202, 0), *D2 = mktex(203, EF_D24S8, VKN_TC_DEPTH, 1, 64, 32, 1, 1, 203, 0);
    vkn_tex *DZ = mktex(204, EF_D32F, VKN_TC_DEPTH, 1, 64, 32, 1, 1, 204, 0), *M = mktex(205, EF_RGBA8, VKN_TC_COLOR, 4, 64, 64, 3, 2, 205, 0);
    vkn_tex *U = mktex(206, EF_R32UI, VKN_TC_COLOR, 1, 16, 16, 1, 1, 206, 0), *S = mktex(207, EF_RGBA8, VKN_TC_SAMPLED, 1, 16, 16, 1, 1, 207, 0);
    vkn_tex *C = mktex(208, EF_RGBA8, VKN_TC_COLOR, 1, 32, 32, 1, 1, 208, 0), *E = mktex(209, EF_RGBA8, VKN_TC_COLOR, 1, 32, 32, 1, 1, 209, 0);
    vkn_tex *CS = mktex(210, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 210, 0), *CD = mktex(211, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 211, 0);
    vkn_tex *AS = mktex(212, EF_RGBA8, VKN_TC_SAMPLED, 4, 16, 16, 1, 3, 212, 0), *AD = mktex(213, EF_RGBA8, VKN_TC_SAMPLED, 4, 16, 16, 1, 2, 213, 0);
    vkn_tex *MS = mktex(214, EF_RGBA8, VKN_TC_SAMPLED, 1, 64, 64, 4, 1, 214, 0), *MD = mktex(215, EF_RGBA8, VKN_TC_SAMPLED, 1, 16, 16, 2, 1, 215, 0);
    vkn_tex *MF = mktex(216, EF_RGBA16F, VKN_TC_COLOR, 1, 64, 32, 1, 1, 216, 0), *MB = mktex(217, EF_RGBA8, VKN_TC_COLOR, 1, 32, 16, 1, 1, 217, 0);
    CHECK(A && B && D && D2 && DZ && M && U && S && C && E && CS && CD && AS && AD && MS && MD && MF && MB);
    if (!(A && B && D && D2 && DZ && M && U && S && C && E && CS && CD && AS && AD && MS && MD && MF && MB)) return;
    vkn_pass_stats p0, p1; vkn_pass_stats2 q0, q1; vkn_pass_stats_get(&p0); vkn_pass_stats2_get(&q0);
    const VkCommandBuffer cb = fbegin(1);
    // ---- lazy begin: nothing at the targets, the pass at the first ready; never-written targets are filled with zeros first
    // (fresh memory on the Switch; garbage in a history target never leaves it: phone run V3_b) and then LOADed
    uint64_t m = mark();
    targets(A, NULL, D);
    NO_OPS(m, cb);
    vkn_pass_info pi; memset(&pi, 0, sizeof pi);
    CHECK_EQ(vkn_pass_ready(&pi), 0);
    OPS(m, cb, MK_E_BARRIER, MK_E_CLEAR_COLOR_IMG, MK_E_BARRIER, MK_E_BARRIER, MK_E_CLEAR_DS_IMG, MK_E_BARRIER, MK_E_BEGIN_RP);
    const mk_ev *ba = barrier_of(m, A->image, 0), *ba2 = barrier_of(m, A->image, 1), *bd = barrier_of(m, D->image, 1), *rp = nth(m, cb, MK_E_BEGIN_RP, 0);
    CHECK(ba && ba->l0 == VK_IMAGE_LAYOUT_UNDEFINED && ba->l1 == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && ba->r.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
    CHECK(ba2 && ba2->l0 == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && ba2->l1 == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    CHECK(bd && bd->l1 == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL && bd->r.aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
    if (att_of(rp, 1)) {
        CHECK_EQ(rp_of(rp)->natt, 2);
        CHECK_EQ(att_of(rp, 0)->format, VK_FORMAT_R8G8B8A8_UNORM); CHECK_EQ(att_of(rp, 0)->loadOp, VK_ATTACHMENT_LOAD_OP_LOAD); CHECK_EQ(att_of(rp, 0)->storeOp, VK_ATTACHMENT_STORE_OP_STORE);
        CHECK_EQ(att_of(rp, 0)->initialLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        CHECK_EQ(att_of(rp, 1)->format, VK_FORMAT_D24_UNORM_S8_UINT); CHECK_EQ(att_of(rp, 1)->loadOp, VK_ATTACHMENT_LOAD_OP_LOAD); CHECK_EQ(att_of(rp, 1)->stencilLoadOp, VK_ATTACHMENT_LOAD_OP_LOAD);
    }
    CHECK_EQ(rp->rect.offset.x, 0); CHECK_EQ(rp->rect.extent.width, 64); CHECK_EQ(rp->rect.extent.height, 32);
    mk_obj *fb = fb_of(rp); CHECK(fb && fb->fb_n == 2 && fb->fb_view[0] == A->att && fb->fb_view[1] == D->att && fb->fb_w == 64 && fb->fb_h == 32 && fb->fb_layers == 1);
    CHECK(pi.pass == (VkRenderPass)(uintptr_t)rp->a[0]); CHECK(pi.compat != 0); CHECK_EQ(pi.ncolor, 1); CHECK_EQ(pi.width, 64); CHECK_EQ(pi.height, 32); CHECK_EQ(pi.samples, 1);
    CHECK_EQ(pi.has_depth, 1); CHECK_EQ(pi.has_stencil, 1); CHECK_EQ(pi.depth_format, VK_FORMAT_D24_UNORM_S8_UINT); CHECK_EQ(pi.write_mask[0], 0xf); CHECK_EQ(pi.write_mask[1], 0);
    CHECK(vkn_pass_depth() == D);
    // the same set keeps the pass; another set ends it at once and opens the next lazily
    m = mark();
    CHECK_EQ(vkn_pass_ready(&pi), 0); targets(A, NULL, D); CHECK_EQ(vkn_pass_ready(&pi), 0);
    NO_OPS(m, cb);
    targets(B, NULL, D);
    OPS(m, cb, MK_E_END_RP);
    m = mark();
    vkn_pass_ready(&pi);
    rp = nth(m, cb, MK_E_BEGIN_RP, 0);
    CHECK(att_of(rp, 1) && att_of(rp, 0)->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && att_of(rp, 1)->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD &&
          att_of(rp, 1)->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD && att_of(rp, 1)->initialLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    CHECK_EQ(pi.write_mask[0], 0xf);
    targets(A, NULL, D); m = mark(); vkn_pass_ready(&pi);
    rp = nth(m, cb, MK_E_BEGIN_RP, 0);
    CHECK(att_of(rp, 0) && att_of(rp, 0)->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && att_of(rp, 0)->initialLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    // ---- clears before the pass fold into CLEAR with their values (colour + depth + stencil)
    targets(A, B, D);
    const float c0[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    m = mark();
    vkn_pass_clear_color(A, 0, c0, NULL); vkn_pass_clear_depth(D, 3, 0.5f, 7);
    NO_OPS(m, cb);
    vkn_pass_ready(&pi);
    rp = nth(m, cb, MK_E_BEGIN_RP, 0);
    if (att_of(rp, 2)) {
        CHECK_EQ(att_of(rp, 0)->loadOp, VK_ATTACHMENT_LOAD_OP_CLEAR); CHECK_EQ(att_of(rp, 1)->loadOp, VK_ATTACHMENT_LOAD_OP_LOAD);
        CHECK_EQ(att_of(rp, 2)->loadOp, VK_ATTACHMENT_LOAD_OP_CLEAR); CHECK_EQ(att_of(rp, 2)->stencilLoadOp, VK_ATTACHMENT_LOAD_OP_CLEAR);
        CHECK_EQ(rp->u[1], 3);
        CHECK_F(rp->cv[0].color.float32[0], 0.25); CHECK_F(rp->cv[0].color.float32[2], 0.75); CHECK_F(rp->cv[2].depthStencil.depth, 0.5); CHECK_EQ(rp->cv[2].depthStencil.stencil, 7);
        CHECK_EQ(pi.ncolor, 2); CHECK_EQ(pi.write_mask[1], 0xf);
    }
    CHECK_EQ(A->pass_nclr, 0); CHECK_EQ(D->pass_dclr, 0);
    // depth only / stencil only
    targets(A, NULL, D); vkn_pass_clear_depth(D, 1, 0.25f, 0); m = mark(); vkn_pass_ready(&pi);
    rp = nth(m, cb, MK_E_BEGIN_RP, 0);
    CHECK(att_of(rp, 1) && att_of(rp, 1)->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && att_of(rp, 1)->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD && att_of(rp, 0)->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
    CHECK_F(rp->cv[1].depthStencil.depth, 0.25);
    targets(B, NULL, D); vkn_pass_clear_depth(D, 2, 0.0f, 9); m = mark(); vkn_pass_ready(&pi);
    rp = nth(m, cb, MK_E_BEGIN_RP, 0);
    CHECK(att_of(rp, 1) && att_of(rp, 1)->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && att_of(rp, 1)->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
    CHECK_EQ(rp->cv[1].depthStencil.stencil, 9);
    // a depth format without stencil: a stencil-only clear is nothing
    m = mark(); vkn_pass_clear_depth(DZ, 2, 0.0f, 3); CHECK_EQ(DZ->pass_dclr, 0); NO_OPS(m, cb);
    // colour layers: a clear of layer 1 folds into the pass of layer 1 only; layer 0's waits
    const float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1};
    vkn_pass_clear_color(M, 1, red, NULL); vkn_pass_clear_color(M, 0, blue, NULL);
    target_lm(M, 1, 0); m = mark(); vkn_pass_ready(&pi);
    rp = nth(m, cb, MK_E_BEGIN_RP, 0);
    CHECK(att_of(rp, 0) && att_of(rp, 0)->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR); CHECK_F(rp->cv[0].color.float32[0], 1.0); CHECK_F(rp->cv[0].color.float32[2], 0.0);
    CHECK_EQ(M->pass_nclr, 1); CHECK_EQ(M->pass_clr[0].layer, 0);
    // integer target: integer clear values
    targets(U, NULL, NULL); const float iv[4] = {3.7f, 2.0f, -1.0f, 1.0f}; vkn_pass_clear_color(U, 0, iv, NULL); m = mark(); vkn_pass_ready(&pi);
    rp = nth(m, cb, MK_E_BEGIN_RP, 0);
    CHECK(rp && rp->cv[0].color.uint32[0] == 3 && rp->cv[0].color.uint32[1] == 2 && rp->cv[0].color.uint32[2] == 0 && rp->cv[0].color.uint32[3] == 1);
    // ---- clears inside an open pass: vkCmdClearAttachments
    targets(A, NULL, D); vkn_pass_ready(&pi);
    const float green[4] = {0, 1, 0, 1};
    m = mark();
    vkn_pass_clear_color(A, 0, green, NULL); vkn_pass_clear_depth(D, 3, 1.0f, 0);
    OPS(m, cb, MK_E_CLEAR_ATT, MK_E_CLEAR_ATT);
    const mk_ev *ca = nth(m, cb, MK_E_CLEAR_ATT, 0), *cd = nth(m, cb, MK_E_CLEAR_ATT, 1);
    CHECK(ca->ca[0].aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && ca->ca[0].colorAttachment == 0 && ca->cr[0].rect.extent.width == 64 && ca->cr[0].rect.extent.height == 32);
    CHECK_F(ca->ca[0].clearValue.color.float32[1], 1.0);
    CHECK(cd->ca[0].aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)); CHECK_F(cd->ca[0].clearValue.depthStencil.depth, 1.0);
    // a region of a current target with the pass closed: the pass opens (it is the draw's anyway) and the region is cleared in it
    targets(B, NULL, D);
    const int32_t r8[4] = {8, 8, 16, 16};
    m = mark();
    vkn_pass_clear_color(B, 0, green, r8);
    OPS(m, cb, MK_E_BEGIN_RP, MK_E_CLEAR_ATT);
    ca = nth(m, cb, MK_E_CLEAR_ATT, 0);
    CHECK(ca->cr[0].rect.offset.x == 8 && ca->cr[0].rect.offset.y == 8 && ca->cr[0].rect.extent.width == 16 && ca->cr[0].rect.extent.height == 16);
    // a region of a texture that is no target: a one-attachment pass of its own; clipped to the texture
    const int32_t rneg[4] = {-4, -4, 8, 8};
    m = mark();
    vkn_pass_clear_color(C, 0, green, rneg);
    OPS(m, cb, MK_E_END_RP, MK_E_BARRIER, MK_E_CLEAR_COLOR_IMG, MK_E_BARRIER, MK_E_BEGIN_RP, MK_E_CLEAR_ATT, MK_E_END_RP);   // (C never written: zeros first)
    rp = nth(m, cb, MK_E_BEGIN_RP, 0); ca = nth(m, cb, MK_E_CLEAR_ATT, 0);
    CHECK(rp_of(rp) && rp_of(rp)->natt == 1 && rp_of(rp)->att[0].format == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(ca->cr[0].rect.offset.x == 0 && ca->cr[0].rect.offset.y == 0 && ca->cr[0].rect.extent.width == 4 && ca->cr[0].rect.extent.height == 4);
    m = mark(); const int32_t rout[4] = {100, 100, 4, 4}; vkn_pass_clear_color(C, 0, green, rout); NO_OPS(m, cb);   // nothing inside
    // ---- remembered clears run before another use: sampled (colour, depth), copied
    const float half[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    vkn_pass_clear_color(E, 0, half, NULL);
    m = mark();
    CHECK_EQ(vkn_pass_sample(E), 0);
    OPS(m, cb, MK_E_BARRIER, MK_E_CLEAR_COLOR_IMG, MK_E_BARRIER);
    const mk_ev *ci = nth(m, cb, MK_E_CLEAR_COLOR_IMG, 0), *b2 = barrier_of(m, E->image, 1);
    CHECK(ci->a[0] == VH(E->image) && ci->l1 == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && ci->r.baseMipLevel == 0 && ci->r.levelCount == 1 && ci->r.layerCount == 1);
    CHECK_F(ci->cv[0].color.float32[0], 0.5);
    CHECK(b2 && b2->l1 == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK_EQ(E->pass_nclr, 0); CHECK_EQ(E->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vkn_pass_clear_depth(DZ, 3, 0.75f, 5);
    m = mark(); vkn_pass_sample(DZ);
    OPS(m, cb, MK_E_BARRIER, MK_E_CLEAR_DS_IMG, MK_E_BARRIER);
    ci = nth(m, cb, MK_E_CLEAR_DS_IMG, 0);
    CHECK(ci->r.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT); CHECK_F(ci->cv[0].depthStencil.depth, 0.75);
    CHECK(barrier_of(m, DZ->image, 0)->r.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT);
    // the layer-0 clear of M left above runs when M is sampled (M is not in the current set any more)
    targets(A, NULL, D);
    m = mark(); vkn_pass_sample(M);
    ci = nth(m, cb, MK_E_CLEAR_COLOR_IMG, 0); CHECK(ci && ci->r.baseArrayLayer == 0 && ci->r.layerCount == 1); if (ci) CHECK_F(ci->cv[0].color.float32[2], 1.0);
    // ---- feedback: an attachment of the current set
    vkn_pass_ready(&pi);
    vkn_pass_stats_get(&p1);
    m = mark();
    CHECK_EQ(vkn_pass_sample(A), 1); CHECK_EQ(vkn_pass_sample(D), 1);
    NO_OPS(m, cb);
    { vkn_pass_stats p2; vkn_pass_stats_get(&p2); CHECK_EQ(p2.feedback, p1.feedback + 2); }
    // ---- sampling a texture written in an earlier pass: transitioned outside every pass; an open pass ends only when needed
    targets(A, NULL, D2); vkn_pass_ready(&pi);
    targets(B, NULL, D);
    m = mark();
    CHECK_EQ(vkn_pass_sample(A), 0);
    OPS(m, cb, MK_E_BARRIER);   // no pass is open (the set change ended it): just the barrier
    ba = barrier_of(m, A->image, 0);
    CHECK(ba && ba->l0 == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && ba->l1 == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && ba->r.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && ba->r.levelCount == 1 && ba->r.layerCount == 1);
    vkn_pass_ready(&pi);
    m = mark();
    CHECK_EQ(vkn_pass_sample(A), 0); CHECK_EQ(vkn_pass_sample(S), 0);   // already readable: the pass stays
    NO_OPS(m, cb);
    vkn_pass_stats_get(&p1);
    m = mark();
    CHECK_EQ(vkn_pass_sample(D2), 0);
    OPS(m, cb, MK_E_END_RP, MK_E_BARRIER);
    bd = barrier_of(m, D2->image, 0);
    CHECK(bd && bd->l0 == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL && bd->l1 == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && bd->r.aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
    { vkn_pass_stats p2; vkn_pass_stats_get(&p2); CHECK_EQ(p2.breaks_sample, p1.breaks_sample + 1); }
    m = mark(); vkn_pass_ready(&pi); rp = nth(m, cb, MK_E_BEGIN_RP, 0);
    CHECK(att_of(rp, 1) && att_of(rp, 0)->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && att_of(rp, 1)->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);   // reopened with LOAD
    // ---- copies (0x920b00)
    vkn_pass_clear_color(CS, 0, red, NULL);   // a remembered clear of the source runs first
    m = mark();
    vkn_pass_copy(CD, CS);
    OPS(m, cb, MK_E_END_RP, MK_E_BARRIER, MK_E_CLEAR_COLOR_IMG, MK_E_BARRIER, MK_E_BARRIER, MK_E_COPY_IMG);
    const mk_ev *cp = nth(m, cb, MK_E_COPY_IMG, 0);
    CHECK(cp && cp->a[0] == VH(CS->image) && cp->a[1] == VH(CD->image) && cp->l0 == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && cp->l1 == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && cp->n == 1);
    if (cp) { CHECK_EQ(cp->ic[0].extent.width, 64); CHECK_EQ(cp->ic[0].extent.height, 32); CHECK_EQ(cp->ic[0].extent.depth, 1); CHECK_EQ(cp->ic[0].srcSubresource.layerCount, 1); }
    m = mark(); vkn_pass_copy(AD, AS);   // arrays: min of the layers
    cp = nth(m, cb, MK_E_COPY_IMG, 0); CHECK(cp && cp->n == 1 && cp->ic[0].srcSubresource.layerCount == 2 && cp->ic[0].dstSubresource.layerCount == 2);
    m = mark(); vkn_pass_copy(MD, MS);   // the level tail: dst level i <- src level (4 - 2 + i)
    cp = nth(m, cb, MK_E_COPY_IMG, 0);
    CHECK(cp && cp->n == 2);
    if (cp && cp->n == 2) {
        CHECK_EQ(cp->ic[0].srcSubresource.mipLevel, 2); CHECK_EQ(cp->ic[0].dstSubresource.mipLevel, 0); CHECK_EQ(cp->ic[0].extent.width, 16);
        CHECK_EQ(cp->ic[1].srcSubresource.mipLevel, 3); CHECK_EQ(cp->ic[1].dstSubresource.mipLevel, 1); CHECK_EQ(cp->ic[1].extent.width, 8);
    }
    vkn_pass_stats2_get(&q1);
    m = mark();
    vkn_pass_copy(MF, CS);   // other format: nothing (the engine copies nothing)
    vkn_pass_copy(MS, MD);   // more destination levels than source levels: dropped
    vkn_pass_copy(CS, CS);
    NO_OPS(m, cb);
    { vkn_pass_stats2 q2; vkn_pass_stats2_get(&q2); CHECK_EQ(q2.copy_skip_fmt, q1.copy_skip_fmt + 1); CHECK_EQ(q2.copy_drop, q1.copy_drop + 2); }
    m = mark(); vkn_pass_copy(MB, CS);   // other size: a scaled copy, nearest
    const mk_ev *bl = nth(m, cb, MK_E_BLIT_IMG, 0);
    CHECK(bl && bl->filter == VK_FILTER_NEAREST && bl->ib[0].srcOffsets[1].x == 64 && bl->ib[0].srcOffsets[1].y == 32 && bl->ib[0].dstOffsets[1].x == 32 && bl->ib[0].dstOffsets[1].y == 16);
    // ---- depth-only set; (mip, layer) attachments
    vkn_pass_targets(0, NULL, NULL, NULL, DZ);
    m = mark(); CHECK_EQ(vkn_pass_ready(&pi), 0); rp = nth(m, cb, MK_E_BEGIN_RP, 0);
    CHECK(rp_of(rp) && rp_of(rp)->natt == 1 && rp_of(rp)->ncolor == 0 && rp_of(rp)->has_depth && rp_of(rp)->att[0].format == VK_FORMAT_D32_SFLOAT);
    CHECK_EQ(pi.ncolor, 0); CHECK_EQ(pi.has_depth, 1); CHECK_EQ(pi.has_stencil, 0); CHECK_EQ(pi.width, 64);
    target_lm(M, 1, 1);
    m = mark(); CHECK_EQ(vkn_pass_ready(&pi), 0); rp = nth(m, cb, MK_E_BEGIN_RP, 0); fb = fb_of(rp);
    CHECK(fb && fb->fb_w == 32 && fb->fb_h == 32); CHECK(rp && rp->rect.extent.width == 32 && rp->rect.extent.height == 32); CHECK_EQ(pi.width, 32);
    mk_obj *v = fb ? vobj(fb->fb_view[0]) : NULL;
    CHECK(v && v->vci.image == M->image && v->vci.subresourceRange.baseMipLevel == 1 && v->vci.subresourceRange.baseArrayLayer == 1 &&
          v->vci.subresourceRange.levelCount == 1 && v->vci.subresourceRange.layerCount == 1 && v->vci.viewType == VK_IMAGE_VIEW_TYPE_2D);
    // no targets / a dead target
    vkn_pass_targets(0, NULL, NULL, NULL, NULL); CHECK_EQ(vkn_pass_ready(&pi), -1);
    // ---- the close listener ends the open pass before the recording ends
    targets(A, NULL, D); vkn_pass_ready(&pi);
    m = mark();
    fsubmit(0);
    const mk_ev *er = nth(m, cb, MK_E_END_RP, 0), *ec = nth(m, cb, MK_E_END_CB, 0);
    CHECK(er && ec && er->seq < ec->seq);
    // stale targets of the last frame are LOADed (their content is kept), counted
    const VkCommandBuffer cb2 = fbegin(1);
    targets(A, NULL, D); m = mark(); vkn_pass_ready(&pi);
    rp = nth(m, cb2, MK_E_BEGIN_RP, 0); CHECK(att_of(rp, 0) && att_of(rp, 0)->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
    vkn_pass_stats_get(&p1); vkn_pass_stats2_get(&q1);
    CHECK(p1.passes > p0.passes); CHECK(q1.load_stale > q0.load_stale); CHECK(q1.region_pass == q0.region_pass + 1); CHECK(p1.clears_cmd >= p0.clears_cmd + 4);
    fsubmit(0);
    // ---- validator self-test: a hand-made bad recording is caught (and leaves the tracked layouts as they were)
    {
        uint32_t nl = 0; static VkImageLayout saved[MK_MAX_OBJ];
        pthread_mutex_lock(&mk_mu); nl = mk_nobj; for (uint32_t i = 0; i < nl; i++) saved[i] = mk_obj_tab[i]->gpu_layout; pthread_mutex_unlock(&mk_mu);
        const int v0 = mk_val_errors;
        printf("validator self-test: the VALIDATION lines below are expected\n");
        VkCommandBuffer t = mk_test_cb(); mk_obj_of(VH(t))->cb_state = 0;
        vkBeginCommandBuffer(t, NULL);
        VkRenderPassBeginInfo bi; memset(&bi, 0, sizeof bi);
        bi.renderPass = (VkRenderPass)(uintptr_t)rp->a[0]; bi.framebuffer = (VkFramebuffer)(uintptr_t)rp->a[1]; bi.renderArea = rp->rect;
        vkCmdBeginRenderPass(t, &bi, VK_SUBPASS_CONTENTS_INLINE);
        VkImageMemoryBarrier b; memset(&b, 0, sizeof b); b.image = S->image; b.oldLayout = VK_IMAGE_LAYOUT_GENERAL; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(t, 1, 1, 0, 0, NULL, 0, NULL, 1, &b);           // inside a pass; old layout wrong
        vkCmdEndRenderPass(t); vkCmdEndRenderPass(t);                          // unpaired
        vkCmdDraw(t, 3, 1, 0, 0);                                              // outside a pass
        vkEndCommandBuffer(t);
        pthread_mutex_lock(&mk_mu); mk_replay(mk_find(VH(t), MK_CB), t); pthread_mutex_unlock(&mk_mu);
        CHECK(mk_val_errors >= v0 + 4);
        mk_val_errors = v0;
        pthread_mutex_lock(&mk_mu); for (uint32_t i = 0; i < nl; i++) mk_obj_tab[i]->gpu_layout = saved[i]; pthread_mutex_unlock(&mk_mu);
    }
    CHECK_EQ(mk_val_errors, 0);
}

// ================================================================================================================ draw
enum { CTL = 0xe48 };
static uint8_t g_ctl[64][CTL], g_code[64][64]; static int g_nctl;   // (the whole suite with test_rt's extensions needs more than 32)
static vkn_stage_src stage_of(const char *key) {   // a stage whose control section carries its program key (the mock's rule)
    const int i = g_nctl++; if (i >= 64) { fprintf(stderr, "stage_of: g_ctl is full\n"); abort(); } memset(g_ctl[i], 0, CTL); memcpy(g_ctl[i], key, 16); memset(g_code[i], i, 64);
    return (vkn_stage_src){g_ctl[i], g_code[i], "shaders/t3.bshd", 0};
}
static const vkn_shader *mkshader(uint64_t h, const char *vs, const char *fs, const char *cs) {
    vkn_stage_src src[VKN_SRCS]; memset(src, 0, sizeof src);
    if (vs) src[VKN_SRC_VS] = stage_of(vs);
    if (fs) src[VKN_SRC_FS] = stage_of(fs);
    if (cs) src[VKN_SRC_CS] = stage_of(cs);
    CHECK_EQ(vkn_shader_create(h, src), 0);
    return vkn_shader_get(h);
}
static const void *buf_ptr(uint64_t buf, uint64_t off) { mk_obj *o = mk_obj_of(buf); return o && o->cpu ? (const uint8_t *)o->cpu + off : NULL; }
static const mk_dw *dw_of(const mk_ev *bs, uint32_t binding) { return bs ? mk_set_dw((VkDescriptorSet)(uintptr_t)bs->a[1], binding, 0) : NULL; }
static mk_obj *pipe_of(const mk_ev *bp) { mk_obj *o = bp ? mk_obj_of(bp->a[0]) : NULL; return o && o->kind == MK_PIPE ? o : NULL; }
// [stage 5] the index buffer bound when the indexed draw d ran (the last bind of cb before it: with the index fold a draw may bind
// nothing) and the CPU pointer of its first index (bound offset + firstIndex * isz)
static const mk_ev *ib_at(VkCommandBuffer cb, const mk_ev *d) {
    if (!d) return NULL;
    for (uint64_t q = d->seq; q-- > mk_ev_base;) { const mk_ev *e = mk_ev_at(q); if (e && e->cb == cb && e->op == MK_E_BIND_IB) return e; }
    return NULL;
}
static const void *idx_of(VkCommandBuffer cb, const mk_ev *d, uint32_t isz) { const mk_ev *b = ib_at(cb, d); return b ? buf_ptr(b->a[0], b->off[0] + (uint64_t)d->u[2] * isz) : NULL; }
static int vb_bound(uint64_t from, VkCommandBuffer cb) { int n = 0; for (int k = 0; nth(from, cb, MK_E_BIND_VB, k); k++) n += (int)nth(from, cb, MK_E_BIND_VB, k)->u[1]; return n; }   // bindings, not calls

static const uint8_t k_vf3[18] = {0, 0, 0, 0, 0, 3,  0, 3, 0, 0, 8, 4,  1, 2, 0, 0, 3, 2};   // loc 0 float3 / loc 1 (stream 0), loc 4 (stream 1)
static const uint8_t k_c1[32] = {1, 2, 3, 4};
static VkShaderModule g_vs1_mod, g_fs1_mod;
static int32_t g_ftex[VKN_TEX_SLOTS], g_fsmp[VKN_TEX_SLOTS], g_ctex[VKN_TEX_SLOTS], g_csmp[VKN_TEX_SLOTS];
static vkn_draw_in g_in;
static vkn_block g_vb0, g_vb1, g_vb2, g_ib16, g_ib32;
static uint32_t g_off2, g_off9, g_offf0;
static vkn_tex *g_t0, *g_t3, *g_t5, *g_ts, *g_a2, *g_d3, *g_b2;
static const vkn_shader *g_s1;

static void draw_setup(void) {
    const VkDescriptorType UB = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    xbg_vkn_stage *vs = mk_bundle_add("v300000000000001", 0);
    mk_bind(vs, 0, UB, XBG_SOURCE_SUPPORT, 0, 1392); mk_bind(vs, 1, UB, XBG_SOURCE_UNIFORM, 2, 256); mk_bind(vs, 2, UB, XBG_SOURCE_UNIFORM, 9, 0);
    mk_bind(vs, 3, UB, XBG_SOURCE_C1, 0, 32);
    vs->c1 = k_c1; vs->c1_bytes = 32; vs->input_count = 3; vs->inputs[0] = 0; vs->inputs[1] = 1; vs->inputs[2] = 4;
    xbg_vkn_stage *fs = mk_bundle_add("f300000000000001", 4);
    mk_bind(fs, 0, UB, XBG_SOURCE_SUPPORT, 0, 1392); mk_bind(fs, 1, UB, XBG_SOURCE_UNIFORM, 0, 64); mk_bind(fs, 3, CIS, XBG_SOURCE_TEXTURE, 0, 0);
    mk_bind(fs, 2, CIS, XBG_SOURCE_TEXTURE, 3, 0);
    xbg_vkn_stage *vs2 = mk_bundle_add("v300000000000002", 0);   // reads location 2 (no element feeds it) and a never-uploaded slot 7
    mk_bind(vs2, 0, UB, XBG_SOURCE_SUPPORT, 0, 1392); mk_bind(vs2, 4, UB, XBG_SOURCE_UNIFORM, 7, 128);
    vs2->input_count = 4; vs2->inputs[0] = 0; vs2->inputs[1] = 1; vs2->inputs[2] = 4; vs2->inputs[3] = 2;
    xbg_vkn_stage *cs = mk_bundle_add("c300000000000001", 5);
    mk_bind(cs, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, XBG_SOURCE_IMAGE, 1, 0); mk_bind(cs, 1, UB, XBG_SOURCE_UNIFORM, 1, 128);
    mk_bind(cs, 2, CIS, XBG_SOURCE_TEXTURE, 0, 0); mk_bind(cs, 3, UB, XBG_SOURCE_SUPPORT, 0, 1392);
    g_vs1_mod = vs->module; g_fs1_mod = fs->module;
    g_s1 = mkshader(H(30, 1), "v300000000000001", "f300000000000001", NULL);
    CHECK(mkshader(H(31, 1), "v300000000000002", "f300000000000001", NULL) != NULL);
    CHECK(mkshader(H(32, 1), "v300000000000001", "f30000000000dead", NULL) != NULL);   // fragment bundle missing
    CHECK(mkshader(H(33, 1), NULL, NULL, "c300000000000001") != NULL);
    CHECK(g_s1 != NULL); if (g_s1) { CHECK_EQ(g_s1->nbind, 8); CHECK_EQ(g_s1->ndyn, 6); }
    CHECK(vkn_blend_put(H(40, 1), 6, 7, 0, 0, 0, 0) != 0); CHECK(vkn_blend_put(H(41, 1), 1, 1, 0, 0, 0, 0) != 0);
    CHECK(vkn_ds_put(H(40, 1), 1, 1, 3, 1, 0xff, 7, 0, 2) != 0); CHECK(vkn_raster_put(H(40, 1), 1, 0, 0, 0, 0) != 0);
    CHECK(vkn_vfmt_put(H(40, 1), k_vf3, 3) != 0);
    CHECK_EQ(vkn_buf_create(VKN_BUF_VB, H(50, 1), 4096, 0, 0, NULL, &g_vb0), 0); CHECK_EQ(vkn_buf_create(VKN_BUF_VB, H(51, 1), 4096, 0, 0, NULL, &g_vb1), 0);
    CHECK_EQ(vkn_buf_create(VKN_BUF_VB, H(54, 1), 4096, 0, 0, NULL, &g_vb2), 0);
    CHECK_EQ(vkn_buf_create(VKN_BUF_IB, H(52, 1), 1024, 2, 0, NULL, &g_ib16), 0); CHECK_EQ(vkn_buf_create(VKN_BUF_IB, H(53, 1), 1024, 4, 0, NULL, &g_ib32), 0);
    CHECK_EQ(vkn_sampler_create(H(60, 1), 10, 1, 0, 0, 0, 1, 0), 0); CHECK_EQ(vkn_sampler_create(H(61, 1), 13, 0, 1, 1, 1, 1, 0), 0);
    g_t0 = mktex(300, EF_RGBA8, VKN_TC_SAMPLED, 1, 16, 16, 1, 1, 300, 0); g_t3 = mktex(303, EF_RGBA8, VKN_TC_SAMPLED, 1, 16, 16, 1, 1, 303, 0);
    g_t5 = mktex(305, EF_RGBA8, VKN_TC_SAMPLED, 1, 16, 16, 1, 1, 305, 0); g_ts = mktex(306, EF_RGBA8, VKN_TC_SAMPLED, 1, 32, 32, 1, 1, 306, 1);
    g_a2 = mktex(310, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 310, 0); g_d3 = mktex(311, EF_D24S8, VKN_TC_DEPTH, 1, 64, 32, 1, 1, 311, 0);
    g_b2 = mktex(312, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 312, 0);
    CHECK(g_t0 && g_t3 && g_t5 && g_ts && g_a2 && g_d3 && g_b2);
    CHECK_EQ(vkn_ring_alloc(512, &g_off2), 0); vkn_draw_ubo(0, 2, g_off2, 512);
    CHECK_EQ(vkn_ring_alloc(128, &g_off9), 0); vkn_draw_ubo(0, 9, g_off9, 128);
    CHECK_EQ(vkn_ring_alloc(32, &g_offf0), 0); vkn_draw_ubo(1, 0, g_offf0, 32);
    for (int i = 0; i < VKN_TEX_SLOTS; i++) g_ftex[i] = g_fsmp[i] = g_ctex[i] = g_csmp[i] = -1;
    g_ftex[0] = 300; g_fsmp[0] = 10; g_ftex[3] = 303; g_fsmp[3] = 13; g_ctex[0] = 300; g_csmp[0] = 10;
    memset(&g_in, 0, sizeof g_in);
    g_in.shader = H(30, 1); g_in.blend = H(40, 1); g_in.ds = H(40, 1); g_in.raster = H(40, 1); g_in.vfmt = H(40, 1);
    g_in.vb[0] = H(50, 1); g_in.vb_off[0] = 16; g_in.vb[1] = H(51, 1); g_in.vb[2] = H(54, 1); g_in.ib = H(52, 1);
    g_in.prim = 1; g_in.stencil_ref = 5; g_in.alpha_ref = -1.0f;
    g_in.tex[1] = g_ftex; g_in.smp[1] = g_fsmp; g_in.tex[2] = g_ctex; g_in.smp[2] = g_csmp;
    g_in.viewport[2] = 64; g_in.viewport[3] = 32; g_in.scissor[2] = 64; g_in.scissor[3] = 32;
}

// [stage 5] a draw effect of the test (vkn_fx.h): g_fxm picks what it asks for; the last context is kept.
static int g_fxm; static vkn_fx_ctx g_fxc; static const vkn_shader *g_fxsh; static uint16_t g_fxds; static uint32_t g_fxcalls;
static void fx_test(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)in; (void)user; g_fxc = *ctx; g_fxcalls++;
    switch (g_fxm) {
        case 1: out->skip = 1; break;
        case 2: out->discard = 1; break;
        case 3: out->ds = g_fxds; break;
        case 4: out->no_fs = 1; break;
        case 5: out->shader = g_fxsh; out->tex[2].view = g_t0 ? g_t0->view : VK_NULL_HANDLE; out->tex[2].tex = g_t0; out->ubo[1].off = 512; out->ubo[1].size = 64; break;
        case 6: { vkn_draw_in x = *in; vkn_fx_bypass(1); vkn_draw_indexed(&x, 0, 3, 1); vkn_fx_bypass(0); break; }   // [stage 5.1 G] a piece's nested draw
        default: break;
    }
}
// [stage 5] draw cost: stand-ins for the generations / vertex-index facts other modules are asked to provide (vkn_draw.h)
static uint32_t g_cost_tex_gen = 1;
static uint32_t cost_tex_gen(void) { return g_cost_tex_gen; }
static uint32_t cost_state_gen(void) { return 7; }
static int cost_vindex(const void *s) { (void)s; return 0; }   // the test's vertex modules read no gl_VertexIndex
// [stage 5.1 O2] DM_VK_NATIVE_GBUF_LE: the fragment facts the test gives every shader (vkn_query_le_test_facts), and the depth compare op
// of the pipeline bound since `from` (-1 = no pipeline bound).
static int g_lef;
static int le_facts_test(const vkn_shader *s) { (void)s; return g_lef; }
static int le_op_since(uint64_t from, VkCommandBuffer cb) {
    const mk_ev *bp = NULL;
    for (int k = 0; nth(from, cb, MK_E_BIND_PIPE, k); k++) bp = nth(from, cb, MK_E_BIND_PIPE, k);
    mk_obj *po = pipe_of(bp);
    return po && po->gp.has_ds ? (int)po->gp.ds.depthCompareOp : -1;
}
// [stage 5.1 A4] the shader library's fused-bundle door (vkn_shlib_named.h; weak in vkn_lightfuse.c): the Light bundle's table, a
// module of its own, every binding but 52 (Sp) declared, no depth comparison.
static int g_lf_fused_calls;
int vkn_shlib_named_fused(const char *light_fs_key, char variant, xbg_vkn_stage *out, uint64_t declared[4], uint64_t *dref) {
    g_lf_fused_calls++;
    for (int i = 0; i < mk_nbundles; i++) if (!strcmp(mk_bundles[i].key, light_fs_key)) {
        *out = mk_bundles[i].st; out->module = (VkShaderModule)(uintptr_t)(0x6f00000ull + (uint32_t)variant);
        for (int k = 0; k < 4; k++) declared[k] = ~0ull;
        declared[0] &= ~(1ull << 52);   // the fused module computes Sky's ambient: Sp (binding 52) is not read
        *dref = 0; return 0;
    }
    return 1;
}
static void test_draw(void) {
    draw_setup();
    if (!g_s1 || !g_t0 || !g_t3 || !g_t5 || !g_ts || !g_a2 || !g_d3 || !g_b2) return;
    vkn_draw_stats s0, s1; vkn_draw_stats2 t0, t1; vkn_draw_stats_get(&s0); vkn_draw_stats2_get(&t0);
    VkCommandBuffer cb = fbegin(1);   // (the sampled textures' uploads run here: SHADER_READ_ONLY)
    CHECK_EQ(g_t0->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    targets(g_a2, NULL, g_d3);
    vkn_draw_in in = g_in;
    // ---- the full draw
    uint64_t m = mark();
    CHECK_EQ(vkn_draw_indexed(&in, 6, 12, 2), 0);
    CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 1);
    CHECK_EQ(cnt(m, cb, MK_E_BIND_PIPE), 1); CHECK_EQ(cnt(m, cb, MK_E_BIND_SETS), 1); CHECK_EQ(cnt(m, cb, MK_E_BIND_VB), 1); CHECK_EQ(cnt(m, cb, MK_E_BIND_IB), 1);   // [stage 5] streams 0 + 1: one call (DM_VK_NATIVE_COST VB)
    CHECK_EQ(cnt(m, cb, MK_E_VIEWPORT), 1); CHECK_EQ(cnt(m, cb, MK_E_SCISSOR), 1); CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 1);
    const mk_ev *bp = nth(m, cb, MK_E_BIND_PIPE, 0), *bs = nth(m, cb, MK_E_BIND_SETS, 0), *dr = nth(m, cb, MK_E_DRAW_IDX, 0), *rp = nth(m, cb, MK_E_BEGIN_RP, 0);
    CHECK(rp->seq < bp->seq && bp->seq < bs->seq && bs->seq < dr->seq && dr->seq == mk_seq - 1);   // the draw last; the set after its pipeline
    CHECK(dr->u[0] == 12 && dr->u[1] == 2 && dr->u[2] == 6 + g_ib16.off / 2 && dr->u[3] == 0 && dr->u[4] == 0);   // [stage 5] index fold: + block offset / 2
    mk_obj *po = pipe_of(bp);
    CHECK(po && po->gp.nst == 2 && po->gp.module[0] == g_vs1_mod && po->gp.module[1] == g_fs1_mod && po->gp.layout == g_s1->layout && po->gp.topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    if (po) { CHECK_EQ(po->gp.nbind, 2); CHECK(po->gp.bind[0].binding == 0 && po->gp.bind[1].binding == 1); CHECK_EQ(po->gp.ds.front.reference, 5); }
    CHECK(bs->u[0] == VK_PIPELINE_BIND_POINT_GRAPHICS && bs->a[0] == VH(g_s1->layout) && bs->n == 6);
    const uint32_t want_dyn[6] = {0, 0, g_off2, g_offf0, g_off9, 0};   // table order: bindings 0, 4, 6, 10, 12, 18
    for (int i = 0; i < 6; i++) CHECK_EQ(bs->dyn[i], want_dyn[i]);
    const VkBuffer ring = vkn_ring_buffer(); const uint64_t rb = vkn_ring_base();
    const mk_dw *w6 = dw_of(bs, 6), *w10 = dw_of(bs, 10), *w12 = dw_of(bs, 12), *w0 = dw_of(bs, 0), *w18 = dw_of(bs, 18), *w16 = dw_of(bs, 16), *w22 = dw_of(bs, 22);
    CHECK(w6 && w6->buf == ring && w6->off == rb && w6->range == 256);        // min(512 uploaded, 256 the binding reads)
    CHECK(w10 && w10->buf == ring && w10->range == 32);                       // min(32, 64)
    CHECK(w12 && w12->range == 128);                                          // binding bytes 0 = the block's size
    CHECK(w0 && w0->range == 1392 && w0->buf != ring);
    CHECK(w18 && w18->buf == g_s1->c1[VKN_SRC_VS].buf && w18->off == g_s1->c1[VKN_SRC_VS].off && w18->range == 32);
    CHECK(w16 && w16->view == g_t3->view && w16->smp == vkn_sampler_by_id(13) && w16->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(w22 && w22->view == g_t0->view && w22->smp == vkn_sampler_by_id(10));
    const mk_ev *v0 = nth(m, cb, MK_E_BIND_VB, 0), *ib = nth(m, cb, MK_E_BIND_IB, 0);
    CHECK(v0->u[0] == 0 && v0->u[1] == 2 && v0->a[0] == VH(g_vb0.buf) && v0->off[0] == g_vb0.off + 16);
    CHECK(v0->a[1] == VH(g_vb1.buf) && v0->off[1] == g_vb1.off);   // stream 2 is not read by the format: not bound
    CHECK(ib->a[0] == VH(g_ib16.buf) && ib->off[0] == g_ib16.off % 2 && ib->u[0] == VK_INDEX_TYPE_UINT16);   // [stage 5] folded: bound at off mod 2
    const mk_ev *vp = nth(m, cb, MK_E_VIEWPORT, 0), *sc = nth(m, cb, MK_E_SCISSOR, 0);
    // flipped viewport (NVN upper-left origin: NDC y = +1 -> row 0), as the translator's xbg_nvn_viewport: y = y + h, height = -h
    CHECK(vp->vp.x == 0 && vp->vp.y == 32 && vp->vp.width == 64 && vp->vp.height == -32 && vp->vp.minDepth == 0 && vp->vp.maxDepth == 1);
    CHECK(sc->rect.offset.x == 0 && sc->rect.extent.width == 64 && sc->rect.extent.height == 32);
    const VkDescriptorSet set1 = (VkDescriptorSet)(uintptr_t)bs->a[1];
    // ---- an identical draw rebinds nothing; one change rebinds only that
    m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0); OPS(m, cb, MK_E_DRAW_IDX);
    in.viewport[0] = 4; m = mark(); vkn_draw_indexed(&in, 0, 3, 1); OPS(m, cb, MK_E_VIEWPORT, MK_E_DRAW_IDX); in.viewport[0] = 0; vkn_draw_indexed(&in, 0, 3, 1);
    in.scissor[2] = 32; m = mark(); vkn_draw_indexed(&in, 0, 3, 1); OPS(m, cb, MK_E_SCISSOR, MK_E_DRAW_IDX);
    CHECK_EQ(nth(m, cb, MK_E_SCISSOR, 0)->rect.extent.width, 32);
    in.vb_off[0] = 32; m = mark(); vkn_draw_indexed(&in, 0, 3, 1); OPS(m, cb, MK_E_BIND_VB, MK_E_DRAW_IDX);
    CHECK(nth(m, cb, MK_E_BIND_VB, 0)->u[0] == 0 && nth(m, cb, MK_E_BIND_VB, 0)->off[0] == g_vb0.off + 32);
    in.ib = H(53, 1); m = mark(); vkn_draw_indexed(&in, 0, 3, 1); OPS(m, cb, MK_E_BIND_IB, MK_E_DRAW_IDX);
    CHECK(nth(m, cb, MK_E_BIND_IB, 0)->u[0] == VK_INDEX_TYPE_UINT32 && nth(m, cb, MK_E_BIND_IB, 0)->a[0] == VH(g_ib32.buf));
    vkn_draw_stats_get(&s1);
    g_ftex[3] = 305; m = mark(); vkn_draw_indexed(&in, 0, 3, 1); OPS(m, cb, MK_E_BIND_SETS, MK_E_DRAW_IDX);
    { vkn_draw_stats s; vkn_draw_stats_get(&s); CHECK_EQ(s.set_builds, s1.set_builds + 1); CHECK(dw_of(nth(m, cb, MK_E_BIND_SETS, 0), 16)->view == g_t5->view); }
    g_ftex[3] = 303; m = mark(); vkn_draw_indexed(&in, 0, 3, 1); OPS(m, cb, MK_E_BIND_SETS, MK_E_DRAW_IDX);
    { vkn_draw_stats s; vkn_draw_stats_get(&s); CHECK_EQ(s.set_builds, s1.set_builds + 1); CHECK(s.set_cache > s1.set_cache); CHECK(nth(m, cb, MK_E_BIND_SETS, 0)->a[1] == VH(set1)); }
    uint32_t o2b; CHECK_EQ(vkn_ring_alloc(512, &o2b), 0); vkn_draw_ubo(0, 2, o2b, 512);   // a new upload: same set, another dynamic offset
    m = mark(); vkn_draw_indexed(&in, 0, 3, 1); OPS(m, cb, MK_E_BIND_SETS, MK_E_DRAW_IDX);
    CHECK(nth(m, cb, MK_E_BIND_SETS, 0)->a[1] == VH(set1)); CHECK_EQ(nth(m, cb, MK_E_BIND_SETS, 0)->dyn[2], o2b);
    in.blend = H(41, 1); m = mark(); vkn_draw_indexed(&in, 0, 3, 1); OPS(m, cb, MK_E_BIND_PIPE, MK_E_DRAW_IDX); in.blend = H(40, 1);
    in.stencil_ref = 6; m = mark(); vkn_draw_indexed(&in, 0, 3, 1); OPS(m, cb, MK_E_BIND_PIPE, MK_E_DRAW_IDX);
    CHECK_EQ(pipe_of(nth(m, cb, MK_E_BIND_PIPE, 0))->gp.ds.front.reference, 6);
    in.stencil_ref = 5; m = mark(); vkn_draw_indexed(&in, 0, 3, 1); OPS(m, cb, MK_E_BIND_PIPE, MK_E_DRAW_IDX);   // back: the cached pipeline
    CHECK(nth(m, cb, MK_E_BIND_PIPE, 0)->a[0] == bp->a[0]);
    // ---- alpha test variant
    in.alpha_ref = 0.5f; m = mark(); vkn_draw_indexed(&in, 0, 3, 1);
    po = pipe_of(nth(m, cb, MK_E_BIND_PIPE, 0));
    CHECK(po && po->gp.module[1] == (VkShaderModule)(uintptr_t)(0x7a00000ull + 5) && po->gp.module[0] == g_vs1_mod);
    CHECK_EQ(mk_alpha_func, 5); CHECK_F(mk_alpha_ref, 0.5);
    in.alpha_ref = -1.0f; m = mark(); vkn_draw_indexed(&in, 0, 3, 1); CHECK(nth(m, cb, MK_E_BIND_PIPE, 0) && nth(m, cb, MK_E_BIND_PIPE, 0)->a[0] == bp->a[0]);
    // ---- scissor clamped to the pass; empty after the clamp: nothing drawn
    in.scissor[0] = -8; in.scissor[1] = -8; in.scissor[2] = 200; in.scissor[3] = 200;
    m = mark(); vkn_draw_indexed(&in, 0, 3, 1); sc = nth(m, cb, MK_E_SCISSOR, 0);
    CHECK(sc && sc->rect.offset.x == 0 && sc->rect.offset.y == 0 && sc->rect.extent.width == 64 && sc->rect.extent.height == 32);
    in.scissor[0] = 100; in.scissor[1] = 100; in.scissor[2] = 10; in.scissor[3] = 10;
    m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0); CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 0);
    in.scissor[0] = in.scissor[1] = 0; in.scissor[2] = 64; in.scissor[3] = 32;
    // ---- arrays
    m = mark(); CHECK_EQ(vkn_draw_arrays(&in, 3, 6), 0);
    const mk_ev *da = nth(m, cb, MK_E_DRAW, 0); CHECK(da && da->u[0] == 6 && da->u[1] == 1 && da->u[2] == 3 && da->u[3] == 0); CHECK_EQ(cnt(m, cb, MK_E_BIND_IB), 0);
    // ---- index rewrite: quads / fans / line loops (arrays and indexed), generated indices checked
    in.prim = 4; m = mark(); CHECK_EQ(vkn_draw_arrays(&in, 10, 8), 0);
    {
        const mk_ev *bi = nth(m, cb, MK_E_BIND_IB, 0), *d = nth(m, cb, MK_E_DRAW_IDX, 0);
        CHECK(bi && d && bi->u[0] == VK_INDEX_TYPE_UINT16 && d->u[0] == 12 && d->u[3] == 10);   // [stage 5] firstIndex: the fold of the transient offset
        CHECK_EQ(cnt(m, cb, MK_E_BIND_PIPE), 0);   // quads draw as triangles: the triangle pipeline
        static const uint16_t want[12] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
        const uint16_t *got = (const uint16_t *)idx_of(cb, d, 2); CHECK(got && !memcmp(got, want, sizeof want));
    }
    in.prim = 2; m = mark(); vkn_draw_arrays(&in, 7, 5);
    {
        const mk_ev *bi = nth(m, cb, MK_E_BIND_IB, 0), *d = nth(m, cb, MK_E_DRAW_IDX, 0);
        CHECK(d && d->u[0] == 9 && d->u[3] == 7);
        static const uint16_t want[9] = {0, 1, 2, 0, 2, 3, 0, 3, 4};
        const uint16_t *got = (const uint16_t *)idx_of(cb, d, 2); CHECK(got && !memcmp(got, want, sizeof want));
    }
    in.prim = 7; m = mark(); vkn_draw_arrays(&in, 0, 4);
    {
        const mk_ev *bi = nth(m, cb, MK_E_BIND_IB, 0), *d = nth(m, cb, MK_E_DRAW_IDX, 0); po = pipe_of(nth(m, cb, MK_E_BIND_PIPE, 0));
        CHECK(po && po->gp.topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP); CHECK(d && d->u[0] == 5);
        static const uint16_t want[5] = {0, 1, 2, 3, 0};
        const uint16_t *got = (const uint16_t *)idx_of(cb, d, 2); CHECK(got && !memcmp(got, want, sizeof want));
    }
    {   // indexed: the engine's indices through the buffer's CPU pointer, from first_index on; u16 in, u16 out; u32 in, u32 out
        uint16_t *i16 = (uint16_t *)vkn_buf_get(VKN_BUF_IB, H(52, 1))->cpu; static const uint16_t q[8] = {10, 11, 12, 13, 20, 21, 22, 23}; memcpy(i16 + 4, q, sizeof q);
        uint32_t *i32 = (uint32_t *)vkn_buf_get(VKN_BUF_IB, H(53, 1))->cpu; static const uint32_t f[4] = {100000, 5, 6, 7}; memcpy(i32 + 2, f, sizeof f);
        in.prim = 4; in.ib = H(52, 1); m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 4, 8, 3), 0);
        const mk_ev *bi = nth(m, cb, MK_E_BIND_IB, 0), *d = nth(m, cb, MK_E_DRAW_IDX, 0);
        bi = ib_at(cb, d);   // [stage 5] the transient block may be bound already (index fold: (buffer, 0) for all its draws)
        CHECK(bi && bi->u[0] == VK_INDEX_TYPE_UINT16 && bi->a[0] != VH(g_ib16.buf)); CHECK(d && d->u[0] == 12 && d->u[1] == 3 && d->u[3] == 0);
        static const uint16_t wq[12] = {10, 11, 12, 10, 12, 13, 20, 21, 22, 20, 22, 23};
        const uint16_t *gq = (const uint16_t *)idx_of(cb, d, 2); CHECK(gq && !memcmp(gq, wq, sizeof wq));
        in.prim = 2; in.ib = H(53, 1); m = mark(); vkn_draw_indexed(&in, 2, 4, 1);
        d = nth(m, cb, MK_E_DRAW_IDX, 0); bi = ib_at(cb, d);
        CHECK(bi && bi->u[0] == VK_INDEX_TYPE_UINT32 && bi->off[0] % 4 == 0); CHECK(d && d->u[0] == 6);
        static const uint32_t wf[6] = {100000, 5, 6, 100000, 6, 7};
        const uint32_t *gf = (const uint32_t *)idx_of(cb, d, 4); CHECK(gf && !memcmp(gf, wf, sizeof wf));
        // nothing to draw: a fan of 2
        m = mark(); CHECK_EQ(vkn_draw_arrays(&in, 0, 2), 0); CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 0);
        // the engine's next plain indexed draw rebinds its own index buffer
        in.prim = 1; m = mark(); vkn_draw_indexed(&in, 0, 3, 1); CHECK(nth(m, cb, MK_E_BIND_IB, 0) && nth(m, cb, MK_E_BIND_IB, 0)->a[0] == VH(g_ib32.buf));
        vkn_draw_rw_stats rw; vkn_draw_rw_stats_get(&rw);
        CHECK_EQ(rw.draws[0][VKN_RW_QUADS], 1); CHECK_EQ(rw.draws[0][VKN_RW_FAN], 1); CHECK_EQ(rw.draws[0][VKN_RW_LOOP], 1);
        CHECK_EQ(rw.draws[1][VKN_RW_QUADS], 1); CHECK_EQ(rw.draws[1][VKN_RW_FAN], 1); CHECK_EQ(rw.degenerate, 1);
    }
    in.ib = H(52, 1); in.vb_off[0] = 16;
    // ---- the zero block (a slot never uploaded) and the null stream (an input the format lacks)
    in.shader = H(31, 1); m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
    {
        const mk_ev *b = nth(m, cb, MK_E_BIND_SETS, 0); const mk_dw *wz = dw_of(b, 24);
        CHECK(wz && wz->buf != ring && wz->range == 128);
        const float *z = wz ? (const float *)buf_ptr(VH(wz->buf), wz->off) : NULL; CHECK(z && z[0] == 0 && z[31] == 0);
        const mk_ev *vb3 = NULL; for (int k = 0; k < 4; k++) { const mk_ev *e = nth(m, cb, MK_E_BIND_VB, k); if (e && e->u[0] == 3) vb3 = e; }
        CHECK(vb3 != NULL);
        const float *nf = vb3 ? (const float *)buf_ptr(vb3->a[0], vb3->off[0]) : NULL; CHECK(nf && nf[0] == 0 && nf[1] == 0 && nf[2] == 0 && nf[3] == 1.0f);
        po = pipe_of(nth(m, cb, MK_E_BIND_PIPE, 0)); int has3 = 0; if (po) for (uint32_t i = 0; i < po->gp.nbind; i++) if (po->gp.bind[i].binding == 3 && po->gp.bind[i].stride == 0) has3 = 1;
        CHECK(has3);
    }
    in.shader = H(30, 1);
    // ---- placeholders: unknown texture id, empty slot, feedback attachment; unknown sampler id -> the default sampler
    vkn_draw_stats_get(&s1); vkn_draw_stats2_get(&t1);
    g_ftex[0] = 999; g_fsmp[3] = 77; m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
    {
        const mk_ev *b = nth(m, cb, MK_E_BIND_SETS, 0); const mk_dw *w = dw_of(b, 22), *w3 = dw_of(b, 16);
        mk_obj *pv = w ? vobj(w->view) : NULL; mk_obj *pim = pv ? mk_obj_of(VH(pv->vci.image)) : NULL;
        CHECK(w && w->view != g_t0->view && pim && pim->ici.extent.width == 1 && pim->ici.extent.height == 1);
        mk_obj *so = w3 ? mk_obj_of(VH(w3->smp)) : NULL; CHECK(so && so->kind == MK_SAMPLER && w3->smp != vkn_sampler_by_id(13) && so->sci.magFilter == VK_FILTER_LINEAR);
    }
    g_fsmp[3] = 13; g_ftex[0] = -1; vkn_draw_indexed(&in, 0, 3, 1);
    g_ftex[0] = 300; vkn_draw_indexed(&in, 0, 3, 1);
    g_ftex[0] = 310; m = mark(); vkn_draw_indexed(&in, 0, 3, 1);   // texture id of the bound colour target: placeholder, no feedback loop
    CHECK(dw_of(nth(m, cb, MK_E_BIND_SETS, 0), 22) && dw_of(nth(m, cb, MK_E_BIND_SETS, 0), 22)->view != g_a2->view); CHECK_EQ(cnt(m, cb, MK_E_END_RP), 0);
    g_ftex[0] = 300; g_fsmp[3] = 13;
    {
        vkn_draw_stats s; vkn_draw_stats2 t; vkn_draw_stats_get(&s); vkn_draw_stats2_get(&t);
        CHECK_EQ(s.unknown_tex, s1.unknown_tex + 1); CHECK_EQ(s.unknown_smp, s1.unknown_smp + 1); CHECK_EQ(t.empty_tex, t1.empty_tex + 1); CHECK_EQ(s.feedback_tex, s1.feedback_tex + 1);
    }
    // ---- drops: nothing recorded, not even a pass (a new target set is pending)
    targets(g_b2, NULL, g_d3);
    vkn_draw_stats_get(&s1); vkn_draw_stats2_get(&t1);
    m = mark();
    in.shader = H(32, 1); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 1);   // fragment stage missing
    in.shader = H(99, 1); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 1);   // unknown shader
    in.shader = H(30, 1); in.blend = H(49, 1); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 1); in.blend = H(40, 1);   // unknown state handle
    in.vb[0] = H(98, 1); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 1); in.vb[0] = H(50, 1);   // unknown vertex buffer
    CHECK_EQ(vkn_draw_indexed(&in, 500, 100, 1), 1);                                       // beyond the index buffer
    in.ib = H(97, 1); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 1); in.ib = H(52, 1);
    in.viewport[2] = 0; CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 1); in.viewport[2] = 64;
    in.prim = 9; CHECK_EQ(vkn_draw_arrays(&in, 0, 3), 0); in.prim = 1;   // out of range = NVN points: drawn (as the engine's table)
    CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 1); CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 0);
    {
        vkn_draw_stats s; vkn_draw_stats2 t; vkn_draw_stats_get(&s); vkn_draw_stats2_get(&t);
        CHECK_EQ(s.dropped_shader, s1.dropped_shader + 2); CHECK_EQ(s.dropped_state, s1.dropped_state + 1); CHECK_EQ(t.dropped_vb, t1.dropped_vb + 1);
        CHECK_EQ(t.dropped_ib, t1.dropped_ib + 2); CHECK_EQ(t.dropped_viewport, t1.dropped_viewport + 1);
    }
    // ---- compute: the pass ends, the storage image goes GENERAL, the compute pipeline and set (images in their layouts)
    uint32_t oc; CHECK_EQ(vkn_ring_alloc(128, &oc), 0); vkn_draw_ubo(2, 1, oc, 128);
    vkn_draw_in ci = g_in; ci.shader = H(33, 1);
    const vkn_img_bind ib1 = {1, H(306, 1)};
    m = mark(); CHECK_EQ(vkn_draw_dispatch(&ci, &ib1, 1, 4, 2, 1), 0);
    {
        CHECK_EQ(cnt(m, cb, MK_E_END_RP), 1); CHECK_EQ(cnt(m, cb, MK_E_DISPATCH), 1);
        const mk_ev *bt = barrier_of(m, g_ts->image, 0), *d = nth(m, cb, MK_E_DISPATCH, 0), *b = nth(m, cb, MK_E_BIND_SETS, 0), *p = nth(m, cb, MK_E_BIND_PIPE, 0);
        CHECK(bt && bt->l1 == VK_IMAGE_LAYOUT_GENERAL); CHECK(d && d->u[0] == 4 && d->u[1] == 2 && d->u[2] == 1);
        CHECK(p && p->u[0] == VK_PIPELINE_BIND_POINT_COMPUTE && b && b->u[0] == VK_PIPELINE_BIND_POINT_COMPUTE && b->n == 2);
        const mk_dw *wi = dw_of(b, 5), *wt = dw_of(b, 17), *wu = dw_of(b, 11);
        CHECK(wi && wi->view == g_ts->view && wi->layout == VK_IMAGE_LAYOUT_GENERAL); CHECK(wt && wt->view == g_t0->view); CHECK(wu && wu->range == 128);
        if (b) CHECK_EQ(b->dyn[0], oc);
    }
    m = mark(); vkn_draw_indexed(&in, 0, 3, 1); CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 1);   // the next draw reopens the pass
    // the storage image sampled by a draw: GENERAL -> SHADER_READ_ONLY first
    g_ftex[3] = 306; m = mark(); vkn_draw_indexed(&in, 0, 3, 1);
    { const mk_ev *bt = barrier_of(m, g_ts->image, 0); CHECK(bt && bt->l0 == VK_IMAGE_LAYOUT_GENERAL && bt->l1 == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); }
    g_ftex[3] = 303;
    fsubmit(0);
    // ---- a new recording rebinds everything
    cb = fbegin(1); targets(g_a2, NULL, g_d3);
    m = mark(); vkn_draw_indexed(&in, 0, 3, 1);
    CHECK_EQ(cnt(m, cb, MK_E_BIND_PIPE), 1); CHECK_EQ(cnt(m, cb, MK_E_BIND_SETS), 1); CHECK_EQ(vb_bound(m, cb), 2); CHECK_EQ(cnt(m, cb, MK_E_BIND_IB), 1);
    CHECK_EQ(cnt(m, cb, MK_E_VIEWPORT), 1); CHECK_EQ(cnt(m, cb, MK_E_SCISSOR), 1);
    CHECK(nth(m, cb, MK_E_BIND_SETS, 0)->a[1] == VH(set1));   // the set cache outlives the recording
    fsubmit(0);
    // ---- the set cache is flushed when a texture dies (at the next recording begin); its pools go after the GPU is done
    mk_fence_hold = 1;
    const int e0 = mk_errors;
    const uint64_t dp0 = mk_destroyed[MK_DPOOL];
    CHECK_EQ(vkn_tex_destroy_cookie(0xC00000ull + 305), 1);
    cb = fbegin(1); targets(g_a2, NULL, g_d3);
    vkn_draw_stats2_get(&t1); CHECK(t1.forget_all > t0.forget_all);
    vkn_draw_stats_get(&s1);
    m = mark(); vkn_draw_indexed(&in, 0, 3, 1);
    { vkn_draw_stats s; vkn_draw_stats_get(&s); CHECK_EQ(s.set_builds, s1.set_builds + 1); CHECK(nth(m, cb, MK_E_BIND_SETS, 0)->a[1] != VH(set1)); }
    // a flush in the middle of a recording (a request from another thread): its pools are still read by this recording
    vkn_draw_forget_all();
    m = mark(); vkn_draw_indexed(&in, 0, 3, 1); CHECK_EQ(cnt(m, cb, MK_E_BIND_SETS), 1);
    // ---- [stage 5] draw effects (vkn_fx.h): the chain, skip, discard, depth-state swap, no fragment stage, a piece's shader, bypass
    {
        vkn_fx_stats f0, f1; vkn_fx_stats_get(&f0);
        CHECK_EQ(vkn_fx_register("test", fx_test, NULL, 100), 0); CHECK_EQ(vkn_fx_register("test", fx_test, NULL, 100), -1);
        g_fxm = 0; vkn_draw_indexed(&in, 0, 3, 1);
        m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0); OPS(m, cb, MK_E_DRAW_IDX);   // a function that asks nothing changes nothing
        CHECK(g_fxc.shader && g_fxc.shader->handle == in.shader); CHECK(g_fxc.color[0] == g_a2); CHECK(g_fxc.depth == g_d3);
        CHECK_EQ(g_fxc.indexed, 1); CHECK_EQ(g_fxc.count, 3); CHECK_EQ(g_fxc.instances, 1); CHECK_EQ(g_fxc.prim, in.prim);
        g_fxm = 1; m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0); CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 0); CHECK_EQ(cnt(m, cb, MK_E_BIND_PIPE), 0);
        g_fxm = 2; m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0); CHECK_EQ(cnt(m, cb, MK_E_BIND_PIPE), 1); CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 1);
        const uint64_t p_disc = nth(m, cb, MK_E_BIND_PIPE, 0) ? nth(m, cb, MK_E_BIND_PIPE, 0)->a[0] : 0;
        g_fxm = 0; m = mark(); vkn_draw_indexed(&in, 0, 3, 1); CHECK_EQ(cnt(m, cb, MK_E_BIND_PIPE), 1);
        const uint64_t p_base = nth(m, cb, MK_E_BIND_PIPE, 0) ? nth(m, cb, MK_E_BIND_PIPE, 0)->a[0] : 0;
        CHECK(p_disc && p_base && p_disc != p_base);
        g_fxds = vkn_fx_ds(1, 0, 2, 0, 0, 0, 0, 0); CHECK(g_fxds != 0); CHECK_EQ(vkn_fx_ds(1, 0, 2, 0, 0, 0, 0, 0), g_fxds);
        g_fxm = 3; m = mark(); vkn_draw_indexed(&in, 0, 3, 1); CHECK_EQ(cnt(m, cb, MK_E_BIND_PIPE), 1);
        CHECK(nth(m, cb, MK_E_BIND_PIPE, 0) && nth(m, cb, MK_E_BIND_PIPE, 0)->a[0] != p_base && nth(m, cb, MK_E_BIND_PIPE, 0)->a[0] != p_disc);
        g_fxm = 4; m = mark(); vkn_draw_indexed(&in, 0, 3, 1); CHECK_EQ(cnt(m, cb, MK_E_BIND_PIPE), 1); CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 1);
        {   // a shader of a piece: an effect block (dynamic), the engine's fragment texture slot 3, an effect texture
            static const uint32_t spv[5] = {0x07230203u, 0x00010000u, 0, 1, 0};
            VkShaderModuleCreateInfo mi; memset(&mi, 0, sizeof mi); mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; mi.codeSize = sizeof spv; mi.pCode = spv;
            VkShaderModule mv = VK_NULL_HANDLE, mf = VK_NULL_HANDLE;
            vkCreateShaderModule(vkn_device()->dev, &mi, NULL, &mv); vkCreateShaderModule(vkn_device()->dev, &mi, NULL, &mf);
            vkn_bind tb[3]; memset(tb, 0, sizeof tb);
            tb[0].binding = 0; tb[0].kind = VKN_BK_FX_UBO; tb[0].slot = 1; tb[0].count = 1; tb[0].bundle_stage = 4; tb[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            tb[1].binding = 1; tb[1].kind = VKN_BK_TEXTURE; tb[1].stage = VKN_STAGE_FS; tb[1].slot = 3; tb[1].count = 1; tb[1].bundle_stage = 4; tb[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            tb[2].binding = 2; tb[2].kind = VKN_BK_FX_TEX; tb[2].slot = 2; tb[2].count = 1; tb[2].bundle_stage = 4; tb[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            vkn_fx_shader_src ss; memset(&ss, 0, sizeof ss); ss.name = "fxtest"; ss.vs = mv; ss.fs = mf; ss.bind = tb; ss.nbind = 3;
            g_fxsh = vkn_fx_shader_create(&ss); CHECK(g_fxsh != NULL);
            if (g_fxsh) {
                CHECK(vkn_fx_shader_is(g_fxsh)); CHECK_EQ(g_fxsh->ndyn, 1); CHECK_EQ(g_fxsh->bind[0].dyn, 0); CHECK_EQ(g_fxsh->bind[1].dyn, VKN_NO_DYN);
                g_fxm = 5; m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
                CHECK_EQ(cnt(m, cb, MK_E_BIND_PIPE), 1); CHECK_EQ(cnt(m, cb, MK_E_BIND_SETS), 1); CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 1);
                m = mark(); vkn_draw_indexed(&in, 0, 3, 1); OPS(m, cb, MK_E_DRAW_IDX);   // the same again: nothing rebound
            }
            tb[2].slot = VKN_FX_TEX; CHECK(vkn_fx_shader_create(&ss) == NULL);   // an effect slot out of range is refused
        }
        g_fxm = 1; vkn_fx_bypass(1); m = mark(); vkn_draw_indexed(&in, 0, 3, 1); CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 1); vkn_fx_bypass(0);   // a piece's own draw
        g_fxm = 0; vkn_draw_indexed(&in, 0, 3, 1);
        vkn_fx_stats_get(&f1);
        CHECK_EQ(f1.skipped, f0.skipped + 1); CHECK_EQ(f1.discards, f0.discards + 1); CHECK_EQ(f1.ds_swaps, f0.ds_swaps + 1); CHECK_EQ(f1.no_fs, f0.no_fs + 1);
        CHECK_EQ(f1.shader_swaps, f0.shader_swaps + 2); CHECK_EQ(f1.bypassed, f0.bypassed + 1); CHECK_EQ(f1.nfn, 1);
        vkn_fx_out oa, ob; memset(&oa, 0, sizeof oa); memset(&ob, 0, sizeof ob); CHECK(vkn_fx_out_same(&oa, NULL)); ob.ubo[3].size = 4; CHECK(!vkn_fx_out_same(&oa, &ob));
    }
    // ---- [stage 5] draw cost (DM_VK_NATIVE_COST, all bits on since main): the fast paths give the same state with fewer commands
    {
        vkn_draw_cost_stats c0, c1; vkn_draw_cost_stats_get(&c0);
        vkn_block ib2; CHECK_EQ(vkn_buf_create(VKN_BUF_IB, H(55, 1), 1024, 2, 0, NULL, &ib2), 0);
        in.ib = H(52, 1); vkn_draw_indexed(&in, 0, 3, 1);
        // index fold: another u16 index buffer of the same arena buffer needs no bind; firstIndex carries its block offset
        in.ib = H(55, 1); m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 3, 6, 1), 0);
        if (ib2.buf == g_ib16.buf) { OPS(m, cb, MK_E_DRAW_IDX); } else CHECK_EQ(cnt(m, cb, MK_E_BIND_IB), 1);
        { const mk_ev *d = nth(m, cb, MK_E_DRAW_IDX, 0), *b = ib_at(cb, d); CHECK(d && b && b->a[0] == VH(ib2.buf) && b->off[0] + (uint64_t)d->u[2] * 2 == ib2.off + 3 * 2); }
        in.ib = H(52, 1);
        // the pipeline front cache: back and forth between two keys
        in.blend = H(41, 1); vkn_draw_indexed(&in, 0, 3, 1); in.blend = H(40, 1); vkn_draw_indexed(&in, 0, 3, 1);
        vkn_draw_cost_stats_get(&c1); const uint64_t fh = c1.pipe_front_hits;
        in.blend = H(41, 1); m = mark(); vkn_draw_indexed(&in, 0, 3, 1); OPS(m, cb, MK_E_BIND_PIPE, MK_E_DRAW_IDX); in.blend = H(40, 1); vkn_draw_indexed(&in, 0, 3, 1);
        vkn_draw_cost_stats_get(&c1); CHECK_EQ(c1.pipe_front_hits, fh + 2);
        CHECK(c1.ib_calls < c1.ib_calls_old); CHECK(c1.set_key_same > c0.set_key_same); CHECK(c1.shader_hits > c0.shader_hits); CHECK(c1.vfmt_hits > c0.vfmt_hits);
        CHECK(c1.support_hits > c0.support_hits); CHECK(c1.ring_hits > c0.ring_hits);
        CHECK(c1.set_tex_reuse > c0.set_tex_reuse);   // [stage 5.1 G] vkn_tex_bind_gen is in the build: the same textures are reused
        CHECK_EQ(c1.vb_folded, c0.vb_folded); CHECK(c1.vb_vindex > c0.vb_vindex);   // no shader library here: gl_VertexIndex not known, no fold
        // with the generations of the other modules (stand-ins): texture bindings reused, state ids cached, the vertex fold
        vkn_draw_cost_gens(cost_tex_gen, cost_state_gen, cost_vindex);
        vkn_draw_indexed(&in, 0, 3, 1);
        vkn_draw_cost_stats_get(&c0);
        m = mark(); vkn_draw_indexed(&in, 0, 3, 1); OPS(m, cb, MK_E_DRAW_IDX);
        vkn_draw_cost_stats_get(&c1); CHECK_EQ(c1.set_tex_reuse, c0.set_tex_reuse + 1); CHECK(c1.state_hits >= c0.state_hits + 3);
        g_ftex[3] = 303; g_fsmp[3] = 13;   // (the same values written again: still reused)
        vkn_draw_indexed(&in, 0, 3, 1); vkn_draw_cost_stats_get(&c0); CHECK_EQ(c0.set_tex_reuse, c1.set_tex_reuse + 1);
        g_ftex[0] = -1; m = mark(); vkn_draw_indexed(&in, 0, 3, 1);   // a slot changed: looked up again, the placeholder bound
        vkn_draw_cost_stats_get(&c1); CHECK_EQ(c1.set_tex_lookups, c0.set_tex_lookups + 1);
        { const mk_dw *w = dw_of(nth(m, cb, MK_E_BIND_SETS, 0), 22); CHECK(w && w->view != g_t0->view); }
        g_ftex[0] = 300; m = mark(); vkn_draw_indexed(&in, 0, 3, 1);
        { const mk_dw *w = dw_of(nth(m, cb, MK_E_BIND_SETS, 0), 22); CHECK(w && w->view == g_t0->view); }
        g_cost_tex_gen++; vkn_draw_cost_stats_get(&c0); vkn_draw_indexed(&in, 0, 3, 1);   // a mapping may have changed: looked up again
        vkn_draw_cost_stats_get(&c1); CHECK_EQ(c1.set_tex_lookups, c0.set_tex_lookups + 1);
        // the vertex fold: stream s bound at (raw offset - k * stride), drawn with vertexOffset k - the same fetch address
        const vkn_vfmt *vf = vkn_vfmt_info(vkn_vfmt_id(H(40, 1)));
        in.vb_off[0] = 16; m = mark(); vkn_draw_indexed(&in, 0, 3, 1);
        uint64_t k = ~0ull;
        {
            const mk_ev *d = nth(m, cb, MK_E_DRAW_IDX, 0); CHECK(d && vf);
            uint64_t raw[2] = {g_vb0.off + 16, g_vb1.off}; const uint32_t st[2] = {vf ? vf->stride[0] : 0, vf ? vf->stride[1] : 0};
            for (int i = 0; i < 2; i++) if (st[i] && raw[i] / st[i] < k) k = raw[i] / st[i];
            if (d) CHECK_EQ(d->u[3], (uint32_t)k);
            uint64_t boff[2] = {0, 0}; int seen = 0;   // what the recording has bound for bindings 0 / 1 at the draw
            for (uint64_t q = mk_ev_base; d && q < d->seq; q++) { const mk_ev *e = mk_ev_at(q); if (e && e->cb == cb && e->op == MK_E_BIND_VB) for (uint32_t i = 0; i < e->u[1]; i++) if (e->u[0] + i < 2) { boff[e->u[0] + i] = e->off[i]; seen |= 1 << (e->u[0] + i); } }
            CHECK_EQ(seen, 3); for (int i = 0; i < 2; i++) CHECK_EQ(boff[i] + k * st[i], raw[i]);
        }
        vkn_draw_cost_stats_get(&c0); CHECK(c0.vb_folded > c1.vb_folded);
        m = mark(); CHECK_EQ(vkn_draw_arrays(&in, 2, 3), 0);   // arrays: firstVertex = first + k
        { const mk_ev *d = nth(m, cb, MK_E_DRAW, 0); CHECK(d && d->u[2] == 2 + k); }
        // [stage 5.1 G] a piece's draw nested in the chain (vkn_fx_bypass inside an effect function): both recorded, the nested one
        // first, the outer arrays draw keeps its own first vertex + fold
        g_fxm = 6; m = mark(); CHECK_EQ(vkn_draw_arrays(&in, 2, 3), 0); g_fxm = 0;
        { const mk_ev *dn = nth(m, cb, MK_E_DRAW_IDX, 0), *d = nth(m, cb, MK_E_DRAW, 0); CHECK(dn && d && dn->seq < d->seq && d->u[2] == 2 + k); }
        // [stage 5.1 G] bit VFOLD alone off (DM_VK_NATIVE_COST=0x7f, the A/B run): the same draw is not folded
        vkn_draw_cost_set(VKN_COST_ALL & ~VKN_COST_VFOLD);
        vkn_draw_cost_stats_get(&c0); m = mark(); CHECK_EQ(vkn_draw_arrays(&in, 2, 3), 0);
        { const mk_ev *d = nth(m, cb, MK_E_DRAW, 0); CHECK(d && d->u[2] == 2); }
        vkn_draw_cost_stats_get(&c1); CHECK_EQ(c1.vb_folded, c0.vb_folded); CHECK_EQ(c1.vb_vindex, c0.vb_vindex);
        vkn_draw_cost_set(VKN_COST_ALL);
        vkn_draw_cost_gens(NULL, NULL, NULL);
        vkn_draw_indexed(&in, 0, 3, 1); in.vb_off[0] = 16;
        // [stage 5.1 G] the real generations of this build (vkn_tex_bind_gen, vkn_state_handle_gen, vkn_shader_vindex)
        {
            const uint32_t tg0 = vkn_tex_bind_gen(), sg0 = vkn_state_handle_gen();
            vkn_tex *tq = NULL; CHECK(vkn_tex_view_by_id(300, &tq) == g_t0->view && tq == g_t0); CHECK(vkn_sampler_by_id(10) != VK_NULL_HANDLE); CHECK(vkn_blend_id(H(40, 1)) != 0);
            vkn_draw_indexed(&in, 0, 3, 1);
            CHECK_EQ(vkn_tex_bind_gen(), tg0); CHECK_EQ(vkn_state_handle_gen(), sg0);   // lookups and draws move neither
            CHECK_EQ(vkn_sampler_create(H(62, 1), 20, 1, 0, 0, 0, 1, 0), 0); const uint32_t tg1 = vkn_tex_bind_gen(); CHECK(tg1 != tg0);
            CHECK_EQ(vkn_sampler_destroy(H(62, 1)), 1); const uint32_t tg2 = vkn_tex_bind_gen(); CHECK(tg2 != tg1);
            vkn_tex *tn = mktex(321, EF_RGBA8, VKN_TC_SAMPLED, 1, 8, 8, 1, 1, 321, 0); CHECK(tn != NULL);
            const uint32_t tg3 = vkn_tex_bind_gen(); CHECK(tg3 != tg2);
            CHECK_EQ(vkn_tex_destroy_cookie(0xC00000ull + 321), 1); CHECK(vkn_tex_bind_gen() != tg3);
            CHECK(vkn_blend_put(H(42, 1), 2, 3, 0, 0, 0, 0) != 0); const uint32_t sg1 = vkn_state_handle_gen(); CHECK(sg1 != sg0);
            CHECK_EQ(vkn_blend_kill(H(42, 1)), 1); CHECK(vkn_state_handle_gen() != sg1);
            CHECK_EQ(vkn_shader_vindex(g_s1), 1); CHECK_EQ(vkn_shader_vindex(NULL), 1); if (g_fxsh) CHECK_EQ(vkn_shader_vindex(g_fxsh), 1);
            // the generation moved: the next draw looks its textures up once, the one after reuses them
            vkn_draw_cost_stats_get(&c0); vkn_draw_indexed(&in, 0, 3, 1); vkn_draw_indexed(&in, 0, 3, 1); vkn_draw_cost_stats_get(&c1);
            CHECK_EQ(c1.set_tex_lookups, c0.set_tex_lookups + 1); CHECK_EQ(c1.set_tex_reuse, c0.set_tex_reuse + 1);
            // a texture the record samples is written while the record holds (a remembered clear of colour target A2): the reused draw
            // still makes it readable - vkn_pass_sample runs the clear outside the pass and transitions it, as the full path does
            targets(g_b2, NULL, g_d3); g_ftex[0] = 310;
            vkn_draw_indexed(&in, 0, 3, 1); vkn_draw_cost_stats_get(&c0);
            const float rgba[4] = {1.0f, 0.0f, 0.0f, 1.0f}; vkn_pass_clear_color(g_a2, 0, rgba, NULL); CHECK(g_a2->pass_nclr > 0);
            CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0); vkn_draw_cost_stats_get(&c1);
            CHECK_EQ(c1.set_tex_reuse, c0.set_tex_reuse + 1); CHECK_EQ(g_a2->pass_nclr, 0); CHECK_EQ(g_a2->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            // the sampled texture becomes a target of the set: vkn_pass_sample answers otherwise, the full path binds the placeholder
            targets(g_a2, NULL, g_d3); vkn_draw_cost_stats_get(&c0); m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0); vkn_draw_cost_stats_get(&c1);
            CHECK_EQ(c1.set_tex_lookups, c0.set_tex_lookups + 1); CHECK_EQ(c1.set_tex_reuse, c0.set_tex_reuse);
            { const mk_dw *w = dw_of(nth(m, cb, MK_E_BIND_SETS, 0), 22); CHECK(w && w->view != g_a2->view); }
            g_ftex[0] = 300; vkn_draw_indexed(&in, 0, 3, 1);
        }
    }
    // ---- [stage 5.1 O2] DM_VK_NATIVE_GBUF_LE (vkn_query.h): a conditional EQUAL draw whose pair holds the same draw, drawn whole in the
    // pre-pass with LESS + writes, gets LEQUAL (only the compare op changes); every other case keeps EQUAL and is counted by its reason
    {
        enum { P = 0x52000 };
        CHECK(vkn_ds_put(H(42, 1), 1, 1, 1, 0, 0, 0, 0, 0) != 0);   // pre-pass: LESS with writes
        CHECK(vkn_ds_put(H(43, 1), 1, 0, 2, 1, 0xff, 7, 0, 2) != 0);   // G-buffer: EQUAL, no writes, stencil ALWAYS / replace
        CHECK(vkn_ds_put(H(44, 1), 1, 1, 7, 0, 0, 0, 0, 0) != 0);   // ALWAYS with writes: raises depth
        vkn_draw_in pre = in, gb = in; pre.ds = H(42, 1); gb.ds = H(43, 1);
        vkn_draw_in up = in; up.ds = H(44, 1);
        vkn_query_le_stats l0, l1; vkn_query_le_stats_get(&l0);
        g_fxm = 0; g_lef = 0; vkn_query_le_test_facts(le_facts_test);
        vkn_query_le_init(1); CHECK_EQ(vkn_query_le(), 1);
        uint64_t key = P;
#define LE_PAIR(pd) do { key += 16; vkn_query_begin(key); vkn_draw_indexed(&(pd), 0, 3, 1); vkn_query_end(key); } while (0)
#define LE_GB(cnt_) (vkn_query_cond(key), m = mark(), vkn_draw_indexed(&gb, 0, (cnt_), 1), le_op_since(m, cb))
        LE_PAIR(pre); CHECK_EQ(LE_GB(3), VK_COMPARE_OP_LESS_OR_EQUAL);                       // the swap
        if (le_op_since(m, cb) == VK_COMPARE_OP_LESS_OR_EQUAL) {
            const mk_obj *po = pipe_of(nth(m, cb, MK_E_BIND_PIPE, 0));
            CHECK(po->gp.ds.depthTestEnable && !po->gp.ds.depthWriteEnable && po->gp.ds.stencilTestEnable && po->gp.ds.front.passOp == VK_STENCIL_OP_REPLACE);
        }
        vkn_draw_indexed(&pre, 0, 3, 1); m = mark(); vkn_draw_indexed(&gb, 0, 3, 1); CHECK_EQ(le_op_since(m, cb), VK_COMPARE_OP_EQUAL);   // no condition
        LE_PAIR(pre); vkn_query_cond(key + 8); m = mark(); vkn_draw_indexed(&gb, 0, 3, 1); CHECK_EQ(le_op_since(m, cb), VK_COMPARE_OP_EQUAL);   // no such pair
        LE_PAIR(pre); CHECK_EQ(LE_GB(6), VK_COMPARE_OP_EQUAL);                               // not the pair's draw
        pre.alpha_ref = 0.5f; LE_PAIR(pre); pre.alpha_ref = -1.0f; CHECK_EQ(LE_GB(3), VK_COMPARE_OP_EQUAL);   // alpha-test variant in the pre-pass
        g_lef = 1 /* VKN_SHLIB_FS_KILL */; LE_PAIR(pre); CHECK_EQ(LE_GB(3), VK_COMPARE_OP_EQUAL);  // OpKill in the pre-pass fragment words
        g_lef = -1; LE_PAIR(pre); CHECK_EQ(LE_GB(3), VK_COMPARE_OP_EQUAL); g_lef = 0;        // facts unknown
        LE_PAIR(gb); vkn_draw_indexed(&pre, 0, 3, 1); CHECK_EQ(LE_GB(3), VK_COMPARE_OP_EQUAL);   // the pre-pass draw has no LESS + writes
        LE_PAIR(pre); vkn_draw_indexed(&up, 0, 3, 1); CHECK_EQ(LE_GB(3), VK_COMPARE_OP_EQUAL);   // depth raised in between
        key += 16; vkn_query_begin(key); vkn_draw_indexed(&pre, 0, 3, 1); vkn_draw_indexed(&pre, 0, 3, 1); vkn_query_end(key);
        CHECK_EQ(LE_GB(3), VK_COMPARE_OP_EQUAL);                                             // two draws in the pair
        g_fxm = 1; LE_PAIR(pre); g_fxm = 0; vkn_draw_indexed(&pre, 0, 3, 1); CHECK_EQ(LE_GB(3), VK_COMPARE_OP_EQUAL);   // an effect skipped the pre-pass draw
        LE_PAIR(pre); targets(g_a2, NULL, NULL); vkn_pass_clear_depth(g_d3, 1, 1.0f, 0); targets(g_a2, NULL, g_d3);
        CHECK_EQ(LE_GB(3), VK_COMPARE_OP_EQUAL);                                             // a depth clear pending at the G-buffer draw
        gb.scissor[2] = 65; LE_PAIR(pre); CHECK_EQ(LE_GB(3), VK_COMPARE_OP_EQUAL); gb.scissor[2] = in.scissor[2];   // scissor wider
        LE_PAIR(pre); CHECK_EQ(LE_GB(3), VK_COMPARE_OP_LESS_OR_EQUAL);                       // and the plain case again
        vkn_query_le_init(2); CHECK_EQ(vkn_query_le(), 2);
        LE_PAIR(pre); CHECK_EQ(LE_GB(3), VK_COMPARE_OP_EQUAL);                               // count only: decided, not applied
        setenv("DM_GFX_SIM_GPU", "mali", 1); vkn_query_le_init(1); CHECK_EQ(vkn_query_le(), 0); unsetenv("DM_GFX_SIM_GPU");   // tile model: off
        LE_PAIR(pre); CHECK_EQ(LE_GB(3), VK_COMPARE_OP_EQUAL);
#undef LE_PAIR
#undef LE_GB
        vkn_query_le_stats_get(&l1);
        CHECK_EQ(l1.swaps - l0.swaps, 3); CHECK_EQ(l1.twins - l0.twins, 1); CHECK_EQ(l1.ds_table_full, l0.ds_table_full);
        CHECK_EQ(l1.refused[VKN_LE_NO_PAIR] - l0.refused[VKN_LE_NO_PAIR], 1); CHECK_EQ(l1.refused[VKN_LE_DRAW] - l0.refused[VKN_LE_DRAW], 1);
        CHECK_EQ(l1.refused[VKN_LE_PRE_ALPHA] - l0.refused[VKN_LE_PRE_ALPHA], 1); CHECK_EQ(l1.refused[VKN_LE_PRE_FS] - l0.refused[VKN_LE_PRE_FS], 2);
        CHECK_EQ(l1.refused[VKN_LE_PRE_DS] - l0.refused[VKN_LE_PRE_DS], 1); CHECK_EQ(l1.refused[VKN_LE_DEPTH_EVENT] - l0.refused[VKN_LE_DEPTH_EVENT], 2);
        CHECK_EQ(l1.refused[VKN_LE_SEVERAL] - l0.refused[VKN_LE_SEVERAL], 1); CHECK_EQ(l1.refused[VKN_LE_LOST] - l0.refused[VKN_LE_LOST], 1);
        CHECK_EQ(l1.refused[VKN_LE_VIEW] - l0.refused[VKN_LE_VIEW], 1);
        vkn_query_census_occ();
        vkn_query_le_init(0); vkn_query_le_test_facts(NULL); CHECK_EQ(vkn_query_le(), 0);
    }
    // ---- [stage 5.1 A4] LIGHTFUSE (vkn_lightfuse.h): DeferredSky into [Sp, Df] then ClusteredLightingWithSSAO into [Df] (ONE, ONE). Frame 1 is
    // drawn as recorded and teaches the chain; frame 2 skips Sky and draws the fused shader with Sky's stashed textures (bindings
    // 200..242) and blocks (248 / 254); frame 3 skips Sky, then a draw that is not the Light comes: Sky is drawn first into [Sp, Df].
    {
        const VkDescriptorType UB = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        xbg_vkn_stage *lv = mk_bundle_add("a400000000000001", 0); mk_bind(lv, 0, UB, XBG_SOURCE_SUPPORT, 0, 1392);
        xbg_vkn_stage *sk = mk_bundle_add("640a4c497bcc748b", 4);   // the DeferredSky key (nr_sh table): textures 10..52 (slots 1..8), c8 / c6 at 64 / 70
        for (uint32_t k = 1; k <= 8; k++) mk_bind(sk, k, CIS, XBG_SOURCE_TEXTURE, k, 0);
        mk_bind(sk, 10, UB, XBG_SOURCE_UNIFORM, 10, 64); mk_bind(sk, 11, UB, XBG_SOURCE_UNIFORM, 11, 64);
        xbg_vkn_stage *li = mk_bundle_add("d236b8d5b7ffad3c", 4);   // ClusteredLightingWithSSAO: depth 22, Sp 52, c16 82, c15 100
        mk_bind(li, 0, UB, XBG_SOURCE_SUPPORT, 0, 1392); mk_bind(li, 3, CIS, XBG_SOURCE_TEXTURE, 9, 0); mk_bind(li, 8, CIS, XBG_SOURCE_TEXTURE, 8, 0);
        mk_bind(li, 13, UB, XBG_SOURCE_UNIFORM, 14, 128); mk_bind(li, 16, UB, XBG_SOURCE_UNIFORM, 15, 256);
        xbg_vkn_stage *fo = mk_bundle_add("d8aeec640d465275", 4);   // DeferredFog: depth 10, scene 16, c16 22, c15 28 (the Light's blocks)
        mk_bind(fo, 0, UB, XBG_SOURCE_SUPPORT, 0, 1392); mk_bind(fo, 1, CIS, XBG_SOURCE_TEXTURE, 0, 0); mk_bind(fo, 2, CIS, XBG_SOURCE_TEXTURE, 1, 0);
        mk_bind(fo, 3, UB, XBG_SOURCE_UNIFORM, 14, 128); mk_bind(fo, 4, UB, XBG_SOURCE_UNIFORM, 15, 256);
        const vkn_shader *ssky = mkshader(H(70, 1), "a400000000000001", "640a4c497bcc748b", NULL), *slt = mkshader(H(71, 1), "a400000000000001", "d236b8d5b7ffad3c", NULL);
        const vkn_shader *sfog = mkshader(H(72, 1), "a400000000000001", "d8aeec640d465275", NULL);
        vkn_tex *sp = mktex(320, EF_RGBA16F, VKN_TC_COLOR, 1, 64, 32, 1, 1, 320, 0), *df = mktex(321, EF_RGBA16F, VKN_TC_COLOR, 1, 64, 32, 1, 1, 321, 0);
        vkn_tex *cc = mktex(322, EF_RGBA16F, VKN_TC_COLOR, 1, 64, 32, 1, 1, 322, 0);   // the Fog's target C
        CHECK(ssky && slt && sfog && sp && df && cc);
        if (ssky && slt && sfog && sp && df && cc) {
            int32_t stex[VKN_TEX_SLOTS], ssmp[VKN_TEX_SLOTS], ltex[VKN_TEX_SLOTS], lsmp[VKN_TEX_SLOTS], ftx[VKN_TEX_SLOTS], fsm[VKN_TEX_SLOTS], rtx[VKN_TEX_SLOTS], rsm[VKN_TEX_SLOTS];
            for (int i = 0; i < VKN_TEX_SLOTS; i++) stex[i] = ssmp[i] = ltex[i] = lsmp[i] = ftx[i] = fsm[i] = rtx[i] = rsm[i] = -1;
            ftx[0] = 300; ftx[1] = 321; fsm[0] = fsm[1] = 10; rtx[0] = 322; rtx[3] = 303; rsm[0] = 10; rsm[3] = 13;   // Fog: depth, Df; reader g_s1: C at slot 0
            for (int k = 1; k <= 8; k++) { stex[k] = 300; ssmp[k] = 10; }
            stex[1] = 303;   // Sky's binding 10 reads g_t3: the fused draw's binding 200 must too
            ltex[8] = 320; ltex[9] = 300; lsmp[8] = lsmp[9] = 10;
            uint32_t o8 = 0, o6 = 0, o16 = 0, o15 = 0; CHECK_EQ(vkn_ring_alloc(64, &o8), 0); CHECK_EQ(vkn_ring_alloc(64, &o6), 0);
            CHECK_EQ(vkn_ring_alloc(128, &o16), 0); CHECK_EQ(vkn_ring_alloc(256, &o15), 0);
            vkn_draw_in sky = in, lt;
            sky.shader = H(70, 1); sky.blend = 0; sky.ds = 0; sky.raster = 0; sky.vfmt = 0; sky.tex[0] = NULL; sky.tex[1] = stex; sky.smp[1] = ssmp; sky.tex[2] = NULL;
            lt = sky; lt.shader = H(71, 1); lt.blend = H(41, 1); lt.tex[1] = ltex; lt.smp[1] = lsmp;
            vkn_draw_in fg = sky, rdr = in; fg.shader = H(72, 1); fg.tex[1] = ftx; fg.smp[1] = fsm; rdr.tex[1] = rtx; rdr.smp[1] = rsm;
            static const float z4[4] = {0, 0, 0, 0};
            vkn_lf_stats a0, a1; vkn_lf_test_setup(1, 1); vkn_lf_stats_get(&a0);
#define LF_SKY() do { targets(sp, df, NULL); vkn_pass_clear_color(sp, 0, z4, NULL); vkn_pass_clear_color(df, 0, z4, NULL); \
                      vkn_draw_ubo(1, 10, o8, 64); vkn_draw_ubo(1, 11, o6, 64); m = mark(); CHECK_EQ(vkn_draw_arrays(&sky, 0, 3), 0); } while (0)
            fsubmit(0); cb = fbegin(1);
            LF_SKY(); CHECK_EQ(cnt(m, cb, MK_E_DRAW), 1);                                     // frame 1: Sky drawn (learning)
            targets(df, NULL, NULL); m = mark(); CHECK_EQ(vkn_draw_arrays(&lt, 0, 3), 0); CHECK_EQ(cnt(m, cb, MK_E_DRAW), 1);
            vkn_lf_stats_get(&a1); CHECK_EQ(a1.would - a0.would, 1); CHECK_EQ(a1.learn - a0.learn, 1); CHECK_EQ(a1.fused, a0.fused); CHECK_EQ(g_lf_fused_calls, 1);
            fsubmit(0); cb = fbegin(1);
            LF_SKY(); CHECK_EQ(cnt(m, cb, MK_E_DRAW), 0); CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 0);    // frame 2: Sky skipped, no pass
            targets(df, NULL, NULL); m = mark(); CHECK_EQ(vkn_draw_arrays(&lt, 0, 3), 0);
            CHECK_EQ(cnt(m, cb, MK_E_DRAW), 1); CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 1);
            {
                const mk_ev *bs = nth(m, cb, MK_E_BIND_SETS, 0);
                const mk_dw *w200 = dw_of(bs, 200), *w206 = dw_of(bs, 206), *w248 = dw_of(bs, 248), *w52 = dw_of(bs, 52);
                CHECK(w200 && w200->view == g_t3->view); CHECK(w206 && w206->view == g_t0->view); CHECK(w248 != NULL); CHECK(w52 == NULL);   // Sp: left out (undeclared)
                const mk_rp *rp = rp_of(nth(m, cb, MK_E_BEGIN_RP, 0)); CHECK(rp && rp->att[0].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);   // Df's remembered clear folded
            }
            vkn_lf_stats_get(&a1); CHECK_EQ(a1.sky_skipped - a0.sky_skipped, 1); CHECK_EQ(a1.fused - a0.fused, 1); CHECK_EQ(a1.lost, a0.lost); CHECK_EQ(g_lf_fused_calls, 1);
            fsubmit(0); cb = fbegin(1);
            LF_SKY(); CHECK_EQ(cnt(m, cb, MK_E_DRAW), 0);                                     // frame 3: Sky skipped ...
            targets(g_a2, NULL, g_d3); m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);    // ... and another draw comes
            CHECK_EQ(cnt(m, cb, MK_E_DRAW), 1); CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 1); CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 2);
            {
                const mk_ev *d = nth(m, cb, MK_E_DRAW, 0), *di = nth(m, cb, MK_E_DRAW_IDX, 0); CHECK(d && di && d->seq < di->seq);   // Sky first
                const mk_rp *r0 = rp_of(nth(m, cb, MK_E_BEGIN_RP, 0)); CHECK(r0 && r0->ncolor == 2 && r0->att[0].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && r0->att[1].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
            }
            CHECK(vkn_pass_color(0) == g_a2 && vkn_pass_depth() == g_d3);                       // the engine's target set put back
            vkn_lf_stats_get(&a1); CHECK_EQ(a1.recovered - a0.recovered, 1); CHECK_EQ(a1.lost, a0.lost); CHECK_EQ(a1.sky_skipped - a0.sky_skipped, 2);
            fsubmit(0); cb = fbegin(1);
            LF_SKY(); CHECK_EQ(cnt(m, cb, MK_E_DRAW), 1);                                     // learning again: frame 4 draws Sky
            vkn_draw_ubo(1, 14, o16, 128); vkn_draw_ubo(1, 15, o15, 256);                    // c16 / c15: the Light's view of the Fog inputs
            targets(df, NULL, NULL); vkn_draw_arrays(&lt, 0, 3);
            targets(cc, NULL, NULL); vkn_pass_clear_color(cc, 0, z4, NULL); m = mark(); CHECK_EQ(vkn_draw_arrays(&fg, 0, 3), 0); CHECK_EQ(cnt(m, cb, MK_E_DRAW), 1);
            vkn_lf_stats_get(&a1); CHECK_EQ(a1.fog_ok - a0.fog_ok, 1); CHECK_EQ(a1.fog_skipped, a0.fog_skipped);   // the Fog passed: variant B learned
            // frame 5 (variant B): Sky skipped, the Light drawn as the 'g' module into [Df, Sp] (this module's own draw), the Fog skipped,
            // its reader samples Sp, a draw into C gets Sp copied into C first
            fsubmit(0); cb = fbegin(1);
            LF_SKY(); CHECK_EQ(cnt(m, cb, MK_E_DRAW), 0);
            vkn_draw_ubo(1, 14, o16, 128); vkn_draw_ubo(1, 15, o15, 256);
            targets(df, NULL, NULL); m = mark(); CHECK_EQ(vkn_draw_arrays(&lt, 0, 3), 0); CHECK_EQ(cnt(m, cb, MK_E_DRAW), 1);
            {
                const mk_rp *rp = rp_of(nth(m, cb, MK_E_BEGIN_RP, 0)); CHECK(rp && rp->ncolor == 2);
                const mk_dw *w = dw_of(nth(m, cb, MK_E_BIND_SETS, 0), 200); CHECK(w && w->view == g_t3->view);
            }
            CHECK(vkn_pass_color(0) == df && vkn_pass_color(1) == NULL);                        // the engine's set put back
            targets(cc, NULL, NULL); vkn_pass_clear_color(cc, 0, z4, NULL); m = mark(); CHECK_EQ(vkn_draw_arrays(&fg, 0, 3), 0); CHECK_EQ(cnt(m, cb, MK_E_DRAW), 0);
            targets(g_a2, NULL, g_d3); m = mark(); CHECK_EQ(vkn_draw_indexed(&rdr, 0, 3, 1), 0);
            { const mk_dw *w = dw_of(nth(m, cb, MK_E_BIND_SETS, 0), 22); CHECK(w && w->view == sp->view); }   // C's binding reads Sp
            rdr.ds = 0; targets(cc, NULL, NULL); m = mark(); CHECK_EQ(vkn_draw_indexed(&rdr, 0, 3, 1), 0); CHECK_EQ(cnt(m, cb, MK_E_COPY_IMG), 1);   // C drawn into: Sp -> C first
            vkn_lf_stats_get(&a1);
            CHECK_EQ(a1.fog_fused - a0.fog_fused, 1); CHECK_EQ(a1.fog_skipped - a0.fog_skipped, 1); CHECK_EQ(a1.redir_draws - a0.redir_draws, 1);
            CHECK_EQ(a1.redir_copies - a0.redir_copies, 1); CHECK_EQ(a1.twins - a0.twins, 1); CHECK_EQ(a1.lost, a0.lost); CHECK_EQ(a1.redir_lost, a0.redir_lost);
#undef LF_SKY
            vkn_lf_census();
            vkn_lf_test_setup(0, 0);
            targets(g_a2, NULL, g_d3);
        }
    }
    fsubmit(0);
    frames_n(4);
    CHECK(mk_destroyed[MK_DPOOL] > dp0);
    CHECK_EQ(mk_errors, e0);   // no pool destroyed while a submission using it was in flight
    mk_gpu_finish_all(); mk_fence_hold = 0;
    vkn_draw_stats_get(&s1); CHECK(s1.draws > s0.draws); CHECK(s1.dispatches == s0.dispatches + 1);
    CHECK_EQ(mk_val_errors, 0);
}

// ================================================================================================================ texture updates
static void test_tex_update(void) {
    vkn_tex *T = mktex(400, EF_RGBA8, VKN_TC_SAMPLED, 1, 16, 16, 1, 1, 400, 0), *X = mktex(401, EF_RGBA8, VKN_TC_COLOR, 1, 16, 16, 1, 1, 401, 0);
    vkn_tex *CT = mktex(402, EF_RGBA8, VKN_TC_COLOR, 1, 16, 16, 1, 1, 402, 0);
    CHECK(T && X && CT); if (!T || !X || !CT) return;
    uint8_t rows[8 * 16]; for (int i = 0; i < (int)sizeof rows; i++) rows[i] = (uint8_t)i;
    vkn_tex_update_stats u0, u1; vkn_tex_update_stats_get(&u0);
    // in a recording: the open pass ends, the copy is recorded in order, the texture ends SHADER_READ_ONLY
    VkCommandBuffer cb = fbegin(1);
    targets(X, NULL, NULL); vkn_pass_info pi; vkn_pass_ready(&pi);
    uint64_t m = mark(); mk_log_reset();
    CHECK_EQ(vkn_tex_update(T, 0, 0, 2, 3, 4, 2, rows, 16), 0);
    CHECK_EQ(cnt(m, cb, MK_E_END_RP), 1); CHECK_EQ(cnt(m, cb, MK_E_COPY_BUF_IMG), 1);
    const mk_ev *cp = nth(m, cb, MK_E_COPY_BUF_IMG, 0), *er = nth(m, cb, MK_E_END_RP, 0);
    CHECK(cp && er && er->seq < cp->seq && cp->a[0] == VH(T->image));
    CHECK_EQ(mk_ncopy, 1);
    if (mk_ncopy == 1) {
        const mk_copy_t *c = &mk_copy[0];
        CHECK(c->c.imageOffset.x == 2 && c->c.imageOffset.y == 3 && c->c.imageExtent.width == 4 && c->c.imageExtent.height == 2 && c->c.imageSubresource.mipLevel == 0);
        CHECK(c->src && !memcmp(c->src, rows, 16) && !memcmp(c->src + 16, rows + 16, 16));   // two rows of 16 bytes, tight
    }
    CHECK_EQ(T->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // clipped to the level; outside / dead: dropped
    mk_log_reset(); CHECK_EQ(vkn_tex_update(T, 0, 0, 14, 14, 8, 8, rows, 32), 0);
    CHECK(mk_ncopy == 1 && mk_copy[0].c.imageExtent.width == 2 && mk_copy[0].c.imageExtent.height == 2);
    CHECK_EQ(vkn_tex_update(T, 0, 0, 16, 0, 4, 4, rows, 16), -1); CHECK_EQ(vkn_tex_update(T, 1, 0, 0, 0, 4, 4, rows, 16), -1);
    // the pass of the current set reopens at the next ready
    m = mark(); vkn_pass_ready(&pi); CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 1);
    fsubmit(0);
    // no recording open: queued, recorded at the start of the next recording
    m = mark();
    CHECK_EQ(vkn_tex_update(T, 0, 0, 0, 0, 4, 4, rows, 16), 0);
    NO_OPS(m, VK_NULL_HANDLE);
    cb = fbegin(1);
    CHECK_EQ(cnt(m, cb, MK_E_COPY_BUF_IMG), 1);
    vkn_tex_update_stats_get(&u1); CHECK_EQ(u1.recorded, u0.recorded + 2); CHECK_EQ(u1.queued, u0.queued + 1); CHECK(u1.dropped_rect >= u0.dropped_rect + 2);
    // a held clear and a later queued update: the engine cleared first, so the update must survive (the clear runs before the copy)
    const float red[4] = {1, 0, 0, 1};
    vkn_pass_clear_color(CT, 0, red, NULL);
    fsubmit(0);
    m = mark();
    CHECK_EQ(vkn_tex_update(CT, 0, 0, 0, 0, 4, 4, rows, 16), 0);
    cb = fbegin(1);
    const mk_ev *cl = nth(m, cb, MK_E_CLEAR_COLOR_IMG, 0); cp = nth(m, cb, MK_E_COPY_BUF_IMG, 0);
    CHECK(cp != NULL);
    CHECK(cl && cp && cl->seq < cp->seq);   // the clear first, then the data
    m = mark(); CHECK_EQ(vkn_pass_sample(CT), 0); CHECK_EQ(cnt(m, cb, MK_E_CLEAR_COLOR_IMG), 0);   // and never again after it
    // the other order: an update recorded at the start, a clear later in the recording: the clear wins (runs at the sample)
    vkn_pass_clear_color(CT, 0, red, NULL);
    m = mark(); vkn_pass_sample(CT); CHECK_EQ(cnt(m, cb, MK_E_CLEAR_COLOR_IMG), 1);
    fsubmit(0);
    CHECK_EQ(mk_val_errors, 0);
}

// ================================================================================================================ end to end
// One synthetic frame: targets A (colour + depth) with clears, 3 draws, targets B (the window) sampling A, 1 draw, present B. The
// mock's validator replays it at the submit (layouts, pass pairing, legal commands, feedback, set / pipeline match); here the shape.
static void test_e2e(void) {
    vkn_tex *A = mktex(500, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 500, 0), *AD = mktex(501, EF_D24S8, VKN_TC_DEPTH, 1, 64, 32, 1, 1, 501, 0), *B = g_win;
    CHECK(A && AD && B && g_s1); if (!A || !AD || !B || !g_s1) return;
    const int val0 = mk_val_errors, err0 = mk_errors, pc0 = pr_calls;
    vkn_draw_in in = g_in; in.shader = H(30, 1); in.ib = H(52, 1); in.prim = 1;
    g_ftex[0] = 300; g_ftex[3] = 303; g_fsmp[3] = 13;
    const uint64_t m = mark();
    const VkCommandBuffer cb = fbegin(1);
    targets(A, NULL, AD);
    const float sky[4] = {0.1f, 0.2f, 0.3f, 1.0f};
    vkn_pass_clear_color(A, 0, sky, NULL); vkn_pass_clear_depth(AD, 3, 1.0f, 0);
    for (uint32_t i = 0; i < 3; i++) CHECK_EQ(vkn_draw_indexed(&in, 3 * i, 3, 1), 0);
    targets(B, NULL, NULL);
    g_ftex[0] = 500;   // A sampled through its pool id
    CHECK_EQ(vkn_draw_indexed(&in, 0, 6, 1), 0);
    g_ftex[0] = 300;
    CHECK_EQ(vkn_rec_present(B, NULL, 1), 0);
    fsubmit(0);
    // the shape of the recording
    CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 2); CHECK_EQ(cnt(m, cb, MK_E_END_RP), 2); CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 4);
    const mk_ev *r1 = nth(m, cb, MK_E_BEGIN_RP, 0), *r2 = nth(m, cb, MK_E_BEGIN_RP, 1), *e1 = nth(m, cb, MK_E_END_RP, 0), *e2 = nth(m, cb, MK_E_END_RP, 1);
    CHECK(att_of(r1, 1) && att_of(r1, 0)->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && att_of(r1, 1)->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && att_of(r1, 1)->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
    CHECK_F(r1->cv[0].color.float32[2], 0.3); CHECK_F(r1->cv[1].depthStencil.depth, 1.0);
    CHECK(att_of(r2, 0) && rp_of(r2)->natt == 1 && att_of(r2, 0)->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);   // the window keeps its content
    const mk_ev *ba = barrier_of(m, A->image, 1);   // [0] = UNDEFINED -> attachment, [1] = attachment -> sampled
    CHECK(ba && ba->l0 == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && ba->l1 == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && e1->seq < ba->seq && ba->seq < r2->seq);
    for (int k = 0; k < 3; k++) { const mk_ev *d = nth(m, cb, MK_E_DRAW_IDX, k); CHECK(d && r1->seq < d->seq && d->seq < e1->seq); }
    const mk_ev *d4 = nth(m, cb, MK_E_DRAW_IDX, 3); CHECK(d4 && r2->seq < d4->seq && d4->seq < e2->seq);
    const mk_ev *bs4 = NULL; for (int k = 0; nth(m, cb, MK_E_BIND_SETS, k); k++) if (nth(m, cb, MK_E_BIND_SETS, k)->seq < d4->seq) bs4 = nth(m, cb, MK_E_BIND_SETS, k);
    CHECK(dw_of(bs4, 22) && dw_of(bs4, 22)->view == A->view);
    const mk_ev *bw = barrier_of(e2->seq, B->image, 0), *ec = nth(m, cb, MK_E_END_CB, 0), *sub = nth(m, cb, MK_E_SUBMIT, 0), *pres = nth(m, VK_NULL_HANDLE, MK_E_PRESENT, 0);
    CHECK(bw && bw->l1 == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && ec && bw->seq < ec->seq);
    CHECK(sub && pres && ec->seq < sub->seq && sub->seq < pres->seq && pres->a[0] == VH(B->image) && pres->qlock == 0);
    CHECK_EQ(pr_calls, pc0 + 1); CHECK_EQ(pr_last.gpu_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    // nothing between a pass's begin and end that a pass forbids, and the validator found nothing
    for (uint64_t s = r1->seq; s < e2->seq; s++) {
        const mk_ev *e = mk_ev_at(s);
        if (e->cb == cb && ((s > r1->seq && s < e1->seq) || (s > r2->seq && s < e2->seq)))
            CHECK(e->op != MK_E_BARRIER && e->op != MK_E_COPY_IMG && e->op != MK_E_BLIT_IMG && e->op != MK_E_CLEAR_COLOR_IMG && e->op != MK_E_CLEAR_DS_IMG && e->op != MK_E_DISPATCH);
    }
    CHECK_EQ(mk_val_errors, val0); CHECK_EQ(mk_errors, err0);
    // the next frame: everything still consistent (A comes back as a target with LOAD, the window from TRANSFER_SRC)
    const VkCommandBuffer cb2 = fbegin(1);
    const uint64_t m2 = mark();
    targets(A, NULL, AD); vkn_draw_indexed(&in, 0, 3, 1);
    targets(B, NULL, NULL); vkn_draw_indexed(&in, 0, 3, 1);
    vkn_rec_present(B, NULL, 1); fsubmit(0);
    CHECK(barrier_of(m2, A->image, 0) && barrier_of(m2, A->image, 0)->l0 == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(barrier_of(m2, B->image, 0) && barrier_of(m2, B->image, 0)->l0 == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    CHECK_EQ(cnt(m2, cb2, MK_E_DRAW_IDX), 2);
    CHECK_EQ(mk_val_errors, val0);
}

// ================================================================================================================ [stage 5.5] the recording thread
// vkn_rt.h. One deterministic multi-frame scenario, driven only through the entry points the host hooks use (vkn_rt_* records, the
// constant ring + vkn_rt_ubo as the flushes do, vkn_buf_update between draws as the staged copy does, owned sections where a hook
// keeps the default sync: a texture update and a texture destroyed and re-created under the same pool id in the middle of a frame), run
// in a forked child per mode: DM_VK_NATIVE_RT 0 (the reference), 9, 1, 2, the slot policies 0 / 2 / 3, a tight ring guard, and a stress
// run with random yields at every encode / decode boundary. Every child records the mock's canonical journal (vkn_mock_vk.h mk_jr_on:
// one hash per submission over its commands, buffer binds replaced by the bytes each draw reads) and the validator / mock errors; the
// journals must equal the reference's submission by submission.
// Per frame: targets A + D with clears, 24 engine draws (indexed u16 / u32, arrays, quads (index rewrite of the engine's indices), a
// fan) with slot-array changes, viewport / scissor / vertex offset changes, constant announces, a VB rename and an IB rename between
// draws, an in-place update of a buffer no draw names, a texture update in an owned section, every 37 frames a texture destroyed and
// re-created under the same pool id; every 10 frames a dispatch; the window with 2 draws, the UI (targets, viewport, scissor, a quad
// draw and an indexed draw with their bytes copied); present; every 50 frames an off-frame recording (a sync at its end).
static uint8_t g_rtctl[2][VKN_UI_CTL], g_rtcode[2][64];
enum { RT_DYN = 768 };   // [stage 5.2 audit] the CPU-written vertex ring's size (2 frames of the scenario's 2 draws a frame)
static int32_t rt_ftex[VKN_TEX_SLOTS], rt_fsmp[VKN_TEX_SLOTS], rt_ctex[VKN_TEX_SLOTS], rt_csmp[VKN_TEX_SLOTS];
static vkn_rt_site g_rts_upd = VKN_RT_SITE("test: texture update"), g_rts_tex = VKN_RT_SITE("test: texture re-created");
static uint32_t rt_seed;
static uint32_t rt_rand(void) { rt_seed = rt_seed * 1664525u + 1013904223u; return rt_seed >> 8; }
static void rt_setup(void) {
    const VkDescriptorType UB = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    xbg_vkn_stage *vs = mk_bundle_add("u300000000000055", 0);
    mk_bind(vs, 0, UB, XBG_SOURCE_UNIFORM, 0, 128); vs->input_count = 2; vs->inputs[0] = 0; vs->inputs[1] = 1;
    xbg_vkn_stage *fs = mk_bundle_add("u300000000000056", 4);
    mk_bind(fs, 0, UB, XBG_SOURCE_UNIFORM, 0, 48); mk_bind(fs, 1, CIS, XBG_SOURCE_TEXTURE, 0, 0); mk_bind(fs, 2, CIS, XBG_SOURCE_TEXTURE, 7, 0);
    memcpy(g_rtctl[0], "u300000000000055", 16); memcpy(g_rtctl[1], "u300000000000056", 16);
    CHECK_EQ(vkn_ui_program_put(0xB1, 0, g_rtctl[0], g_rtcode[0]), 0); CHECK_EQ(vkn_ui_program_put(0xB2, 1, g_rtctl[1], g_rtcode[1]), 0);
    vkn_block b; CHECK_EQ(vkn_buf_create(VKN_BUF_VB, H(55, 1), 4096, 0, 0, NULL, &b), 0);   // never drawn: updated in place
    CHECK(mktex(330, EF_RGBA8, VKN_TC_SAMPLED, 1, 16, 16, 1, 1, 330, 0) != NULL);           // destroyed and re-created by the scenario
    CHECK_EQ(vkn_buf_create(VKN_BUF_VB, H(57, 1), RT_DYN, 0, 0, NULL, &b), 0);               // [stage 5.2 audit] the CPU-written vertex ring
    // deterministic buffer bytes: vertices (any), indices 0..15
    uint8_t *v0 = (uint8_t *)vkn_buf_get(VKN_BUF_VB, H(50, 1))->cpu, *v1 = (uint8_t *)vkn_buf_get(VKN_BUF_VB, H(51, 1))->cpu, *v2 = (uint8_t *)vkn_buf_get(VKN_BUF_VB, H(54, 1))->cpu;
    for (int i = 0; i < 4096; i++) { v0[i] = (uint8_t)(i * 7 + 1); v1[i] = (uint8_t)(i * 13 + 5); v2[i] = (uint8_t)(i * 3 + 9); }
    uint16_t *i16 = (uint16_t *)vkn_buf_get(VKN_BUF_IB, H(52, 1))->cpu; uint32_t *i32 = (uint32_t *)vkn_buf_get(VKN_BUF_IB, H(53, 1))->cpu;
    for (int i = 0; i < 512; i++) i16[i] = (uint16_t)(i * 5 % 16);
    for (int i = 0; i < 256; i++) i32[i] = (uint32_t)(i * 3 % 16);
    vkn_draw_cost_gens(NULL, NULL, NULL);   // the real texture / state generations (test_draw left stand-ins)
}
static void rt_announce(uint32_t f, uint32_t k) {   // what a constant flush does: copy into the ring, announce
    uint8_t *ring = (uint8_t *)vkn_ring_cpu(); uint32_t off = 0;
    if (!ring || vkn_ring_alloc(512, &off)) { CHECK(0); return; }
    for (int i = 0; i < 512; i++) ring[off + i] = (uint8_t)(f * 31 + k * 7 + i);
    vkn_rt_ubo(0, 2, off, 512);
    if (k % 8 == 0) { if (vkn_ring_alloc(64, &off)) { CHECK(0); return; } for (int i = 0; i < 64; i++) ring[off + i] = (uint8_t)(f + i); vkn_rt_ubo(1, 0, off, 64); }
    vkn_rt_ubo(0, 9, g_off9, 128);   // a re-announce of the same block (no generation change)
}
static void rt_targets(vkn_tex *c, vkn_tex *d) { vkn_tex *cc[VKN_PASS_COLORS] = {c}; const uint32_t z[VKN_PASS_COLORS] = {0}; vkn_rt_pass_targets(c ? 1 : 0, cc, z, z, d); }
// [stage 5.2 audit] the pieces merged after stage 5.5, as extensions of the scenario (end of the file: rtx_*): RTX_UI1 / RTX_UI3
// DM_VK_NATIVE_UIPASS 1 / 3 with a GDraw epoch on a target + DS (depth-tested draws, masked stencil clears, fences); RTX_POL the
// load / store policy LOAD=1 STORE=1 FRESH=2; RTX_MAT MAT variant B with a gate word changing between draws of a depth-only pass;
// RTX_SHC the shadow cache (an atlas pass of static casters + moving ones); RTX_LF LIGHTFUSE (Sky + Light every frame).
enum { RTX_UI1 = 1, RTX_UI3 = 2, RTX_POL = 4, RTX_MAT = 8, RTX_SHC = 16, RTX_LF = 32 };
static int g_rtx;
static void rtx_setup(void);
static void rtx_on(int ext);
static void rtx_off(void);
static void rtx_frame(uint32_t f);
static uint64_t rtx_metric(int ext);
// The CPU-written vertex ring (the immediate draws' r+0x8d0 in hle_dos2_vkn_imm.c): each draw's vertices written through the buffer's
// CPU pointer at the head, wrapping; vkn_rt_cpu_ring before each write (what the imm hooks do). The journal hashes the bytes a draw
// reads when it is recorded: with the recording thread an unguarded wrap would show later vertices in earlier draws.
static uint32_t rt_dyn_head;
static void rt_dyn_draws(uint32_t f) {
    const uint16_t vid = vkn_vfmt_id(g_in.vfmt); const vkn_vfmt *vf = vid ? vkn_vfmt_info(vid) : NULL;
    uint32_t stride = 32; for (uint32_t i = 0; vf && i < vf->nbind; i++) if (vf->bind[i].binding == 0 && vf->bind[i].stride) stride = vf->bind[i].stride;
    const uint32_t bytes = 6 * stride, ring = 3 * bytes <= RT_DYN ? 3 * bytes : RT_DYN;   // (three draws: a frame and a half - the engine thread can be a frame ahead)
    for (uint32_t d = 0; d < 2; d++) {
        uint32_t head = rt_dyn_head; const int wrap = head + bytes > ring;
        vkn_rt_cpu_ring(2, (wrap ? (uint64_t)ring - head : 0) + bytes, ring);
        if (wrap) head = 0;
        const vkn_block *blk = vkn_buf_get(VKN_BUF_VB, H(57, 1)); if (!blk) { CHECK(0); return; }
        uint8_t *p = (uint8_t *)blk->cpu + head; for (uint32_t i = 0; i < bytes; i++) p[i] = (uint8_t)(f * 7 + d * 61 + i);
        rt_dyn_head = head + bytes;
        vkn_draw_in in = g_in; in.tex[0] = NULL; in.smp[0] = NULL; in.tex[1] = rt_ftex; in.smp[1] = rt_fsmp; in.tex[2] = rt_ctex; in.smp[2] = rt_csmp;
        in.vb[0] = H(57, 1); in.vb_off[0] = head;
        vkn_rt_query_cond(0); vkn_rt_draw_arrays(&in, 0, 6);
    }
}
static void rt_frame(uint32_t f) {
    vkn_rt_rec_begin(1);
    rt_targets(g_a2, g_d3);
    const float rgba[4] = {(float)(f % 7) / 7.0f, 0.25f, 0.5f, 1.0f};
    vkn_rt_pass_clear_color(g_a2, 0, rgba, NULL); vkn_rt_pass_clear_depth(g_d3, 3, 1.0f, 0);
    for (uint32_t k = 0; k < 24; k++) {
        if (k % 4 == 0) rt_announce(f, k);
        // slot changes; vkn_rt_slots_touch = what the texture flush reports for a dirty range (slot policies 2 / 3); k % 10 == 5: a
        // flush with a dirty range and no change
        if (k % 5 == 0) { rt_ftex[0] = (f + k) % 3 == 0 ? 300 : (f + k) % 3 == 1 ? 303 : 330; vkn_rt_slots_touch(); }
        if (k == 7) { rt_ftex[3] = rt_ftex[3] == 303 ? 330 : 303; vkn_rt_slots_touch(); }
        if (k == 19) { rt_fsmp[3] = rt_fsmp[3] == 13 ? 10 : 13; vkn_rt_slots_touch(); }
        vkn_draw_in in = g_in;
        in.tex[0] = NULL; in.smp[0] = NULL; in.tex[1] = rt_ftex; in.smp[1] = rt_fsmp; in.tex[2] = rt_ctex; in.smp[2] = rt_csmp;
        in.viewport[2] = (float)(64 - (k % 3) * 8); in.scissor[2] = 64 - (int32_t)(k % 2) * 16; in.vb_off[0] = 16 * (k % 4);
        vkn_rt_query_cond(0);   // what dm_vkn_draw_fill does before every draw
        switch (k % 6) {
        case 0: case 1: in.ib = H(52, 1); vkn_rt_draw_indexed(&in, k % 8, 3 + 3 * (k % 2), 1); break;
        case 2: in.ib = H(53, 1); vkn_rt_draw_indexed(&in, 2, 6, 1 + (f % 2)); break;
        case 3: vkn_rt_draw_arrays(&in, k % 5, 6); break;
        case 4: in.prim = 4; in.ib = H(52, 1); vkn_rt_draw_indexed(&in, 4 + f % 4, 8, 1); break;   // quads: the rewrite reads the engine's indices
        default: in.prim = 2; vkn_rt_draw_arrays(&in, 1, 5); break;                                   // fan
        }
        uint8_t data[64]; void *cpu = NULL; uint64_t va = 0;
        if (k == 9) { for (int i = 0; i < 64; i++) data[i] = (uint8_t)(f * 3 + i); CHECK(vkn_buf_update(VKN_BUF_VB, H(50, 1), 64ull * (f % 8), data, 64, &cpu, &va) >= 0); }
        if (k == 13) { uint16_t q[8]; for (int i = 0; i < 8; i++) q[i] = (uint16_t)((f + 3 * i) % 16); CHECK(vkn_buf_update(VKN_BUF_IB, H(52, 1), 16, q, sizeof q, &cpu, &va) >= 0); }
        if (k == 17 && f % 7 == 0) { for (int i = 0; i < 64; i++) data[i] = (uint8_t)(f + i); CHECK(vkn_buf_update(VKN_BUF_VB, H(55, 1), 0, data, 64, &cpu, &va) >= 0); }
        if (k == 11 && f % 4 == 0) {   // a hook with the default sync: a texture update recorded in order (the recording's owner records it); the
                                       // other frames have no sync, so the engine can run a frame ahead
            static uint32_t px[16 * 16]; for (int i = 0; i < 256; i++) px[i] = f * 0x01010101u + (uint32_t)i;
            vkn_rt_own_begin(&g_rts_upd); CHECK_EQ(vkn_tex_update(g_t3, 0, 0, 0, 0, 16, 16, px, 64), 0); vkn_rt_own_end();
        }
        if (k == 21) {   // an encoded texture update (UpdateTextureRect & co. encode: the staging now, the copy where the recording is)
            static uint32_t px2[8 * 8]; for (int i = 0; i < 64; i++) px2[i] = f * 0x00010203u ^ (uint32_t)(i * 77);
            CHECK_EQ(vkn_tex_update(g_t0, 0, 0, (int32_t)(f % 2) * 8, 4, 8, 8, px2, 32), 0);
        }
        if (k == 15 && f % 37 == 5) {   // a destroy and a create under the same pool id between draws (the destroy hook syncs)
            vkn_rt_own_begin(&g_rts_tex); CHECK_EQ(vkn_tex_destroy_handle(H(330, 1)), 1); CHECK(mktex(330, EF_RGBA8, VKN_TC_SAMPLED, 1, 16, 16, 1, 1, 330, 0) != NULL); vkn_rt_own_end();
        }
    }
    rt_dyn_draws(f);       // [stage 5.2 audit]
    if (g_rtx) rtx_frame(f);
    if (f % 10 == 3) {   // a dispatch (the pass ends; the storage image)
        uint8_t *ring = (uint8_t *)vkn_ring_cpu(); uint32_t off = 0;
        CHECK_EQ(vkn_ring_alloc(128, &off), 0); for (int i = 0; i < 128; i++) ring[off + i] = (uint8_t)(f ^ i);
        vkn_rt_ubo(2, 1, off, 128);
        vkn_draw_in ci = g_in; ci.tex[0] = NULL; ci.smp[0] = NULL; ci.tex[1] = rt_ftex; ci.smp[1] = rt_fsmp; ci.tex[2] = rt_ctex; ci.smp[2] = rt_csmp; ci.shader = H(33, 1);
        const vkn_img_bind ib1 = {1, H(306, 1)};
        vkn_rt_query_cond(0);
        vkn_rt_dispatch(&ci, &ib1, 1, 4, 2, 1);
    }
    rt_targets(g_win, NULL);
    for (uint32_t k = 0; k < 2; k++) { vkn_draw_in in = g_in; in.tex[0] = NULL; in.smp[0] = NULL; in.tex[1] = rt_ftex; in.smp[1] = rt_fsmp; in.tex[2] = rt_ctex; in.smp[2] = rt_csmp;
                                       rt_ftex[0] = 310; vkn_rt_slots_touch(); in.ib = H(52, 1); vkn_rt_query_cond(0); vkn_rt_draw_indexed(&in, k * 3, 3, 1); rt_ftex[0] = 300; vkn_rt_slots_touch(); }
    {   // the UI
        vkn_rt_ui_targets(g_win, NULL); vkn_rt_ui_viewport(0, 0, 64, 32); vkn_rt_ui_scissor(0, (int32_t)(f % 4), 64, 28);
        float verts[16] = {0, 0, 0, 0,  64, 0, 1, 0,  64, 32, 1, 1,  0, 32, 0, 1}; verts[0] = (float)(f % 9);
        float vsc[32], fsc[12]; for (int i = 0; i < 32; i++) vsc[i] = (float)(f + i); for (int i = 0; i < 12; i++) fsc[i] = (float)(i * f);
        vkn_ui_draw_in d; memset(&d, 0, sizeof d);
        d.vs = 0xB1; d.fs = 0xB2; d.vfmt = VKN_UI_VF_V2TC2; d.quads = 1; d.blend = 1; d.tex[0] = g_t0; d.vsc = vsc; d.vsc_bytes = sizeof vsc;
        d.fsc = fsc; d.fsc_bytes = sizeof fsc; d.verts = verts; d.nverts = 4;
        vkn_rt_ui_draw(&d);
        verts[0] = 7.0f;   // (the record holds its own copy: this write must not reach it)
        static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
        d.quads = 0; d.idx = idx; d.nidx = 6; d.tex[0] = (f & 1) ? g_t3 : NULL; d.blend = 0;
        vkn_rt_ui_draw(&d);
    }
    vkn_rt_rec_end(1, g_win, NULL, 1);
    const uint64_t cpu3[3] = {0, 0, 0};
    vkn_rt_frame_tail(cpu3, NULL);
    if (f % 50 == 25) {   // an off-frame recording (the engine waits for its GPU work: a sync at its end)
        vkn_rt_rec_begin(0);
        rt_targets(g_b2, NULL);
        const float c2[4] = {0, 0, (float)f, 1};
        vkn_rt_pass_clear_color(g_b2, 0, c2, NULL);
        vkn_draw_in in = g_in; in.tex[0] = NULL; in.smp[0] = NULL; in.tex[1] = rt_ftex; in.smp[1] = rt_fsmp; in.tex[2] = rt_ctex; in.smp[2] = rt_csmp;
        vkn_rt_query_cond(0); vkn_rt_draw_indexed(&in, 0, 3, 1);
        vkn_rt_rec_end(0, NULL, NULL, 0);
    }
}
static __thread uint32_t t_yseed;
static void rt_yield(int where) {   // the stress run: random yields / short sleeps at every encode / decode boundary
    if (!t_yseed) t_yseed = (uint32_t)(uintptr_t)&t_yseed | 1u;
    t_yseed = t_yseed * 1103515245u + 12345u;
    const uint32_t r = t_yseed >> 16;
    if ((r & 15) == where) sched_yield();
    if ((r & 1023) == 7) { const struct timespec ts = {0, 20000}; nanosleep(&ts, NULL); }
    if (where == 1 && (r & 127) == 3) { const struct timespec ts = {0, 150000}; nanosleep(&ts, NULL); }   // a slow recording thread: the engine runs ahead
}
typedef struct rt_res {
    int ran, checks, fails, val_errors, mk_errors; uint32_t n, frames, mode;
    vkn_rt_stats st; uint64_t sync_upd, sync_tex, xm;
    mk_jr_rec rec[4600];
} rt_res;
typedef struct { int mode, slots, yield; uint32_t frames; uint64_t ring_limit; int ext; } rt_cfg;
static void rt_run(const rt_cfg *c, rt_res *r) {
    const int c0 = g_checks, f0 = g_fails, v0 = mk_val_errors, e0 = mk_errors;
    pthread_mutex_lock(&mk_mu); mk_jr_on = 1; mk_jr_n = 0; if (!mk_jr_recording()) mk_ev_base = mk_seq; pthread_mutex_unlock(&mk_mu);
    rt_seed = 12345; rt_dyn_head = 0;
    alarm(600);   // (a hang fails the child instead of the run)
    g_rtx = c->ext; if (c->ext) rtx_on(c->ext);
    vkn_rt_install(c->mode, c->slots);
    vkn_rt_test_yield(c->yield ? rt_yield : NULL);
    vkn_rt_test_ring_limit(c->ring_limit);
    for (uint32_t f = 0; f < c->frames; f++) rt_frame(f);
    vkn_rt_shutdown();
    pthread_mutex_lock(&mk_mu); mk_jr_on = 0; pthread_mutex_unlock(&mk_mu);
    r->n = mk_jr_n < 4600 ? mk_jr_n : 4600; if (mk_jr_n > 4600) g_fails++;
    memcpy(r->rec, mk_jr, r->n * sizeof r->rec[0]);
    vkn_rt_stats_get(&r->st); r->sync_upd = g_rts_upd.syncs; r->sync_tex = g_rts_tex.syncs; r->xm = c->ext ? rtx_metric(c->ext) : 0;
    r->frames = c->frames; r->mode = (uint32_t)c->mode;
    r->checks = g_checks - c0; r->fails = g_fails - f0; r->val_errors = mk_val_errors - v0; r->mk_errors = mk_errors - e0;
}
static int rt_child(const rt_cfg *c, rt_res *out) {
    memset(out, 0, sizeof *out);
    int pfd[2]; if (pipe(pfd)) return -1;
    fflush(stdout);
    const pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        close(pfd[0]);
        rt_res *r = (rt_res *)calloc(1, sizeof *r);
        rt_run(c, r); r->ran = 1;
        const uint8_t *p = (const uint8_t *)r; size_t left = sizeof *r;
        while (left) { const ssize_t w = write(pfd[1], p, left); if (w <= 0) break; p += w; left -= (size_t)w; }
        close(pfd[1]);
        _exit(0);
    }
    close(pfd[1]);
    uint8_t *p = (uint8_t *)out; size_t got = 0;
    while (got < sizeof *out) { const ssize_t n = read(pfd[0], p + got, sizeof *out - got); if (n <= 0) break; got += (size_t)n; }
    close(pfd[0]);
    int status = 0; waitpid(pid, &status, 0);
    const int ok = got == sizeof *out && WIFEXITED(status) && WEXITSTATUS(status) == 0 && out->ran;
    g_checks += out->checks + 1; g_fails += out->fails;
    if (!ok) { g_fails++; printf("FAIL %s:%d: rt child mode %d (status %#x, %zu of %zu result bytes)\n", __FILE__, __LINE__, c->mode, status, got, sizeof *out); }
    return ok ? 0 : -1;
}
// The journal of `r` equals the reference `ref` (same frames): submission by submission.
static int rt_same(const char *what, const rt_res *ref, const rt_res *r) {
    CHECK_EQ(r->val_errors, 0); CHECK_EQ(r->mk_errors, 0);
    int bad = r->n != ref->n, first = -1;
    for (uint32_t i = 0; i < r->n && i < ref->n; i++) if (memcmp(&r->rec[i], &ref->rec[i], sizeof r->rec[i])) { if (first < 0) first = (int)i; bad = 1; }
    g_checks++;
    if (bad) {
        g_fails++;
        printf("FAIL %s:%d: rt %s: journal differs from DM_VK_NATIVE_RT=0: %u vs %u submissions, first difference at %d", __FILE__, __LINE__, what, r->n, ref->n, first);
        if (first >= 0) printf(" (events %u / %u, draws %u / %u)", r->rec[first].nev, ref->rec[first].nev, r->rec[first].ndraw, ref->rec[first].ndraw);
        printf("\n");
    }
    uint64_t draws = 0; for (uint32_t i = 0; i < r->n; i++) draws += r->rec[i].ndraw;
    const double f = r->frames ? (double)r->frames : 1.0;
    printf("rt %-28s %s: %u submissions, %llu draws recorded; records/frame draw %.1f ubo %.1f slots %.1f call %.1f, syncs/frame %.2f (wait %.3f ms), "
           "engine wait at the end %.3f ms/frame, ahead 0:%llu 1:%llu 2:%llu 3+:%llu, ring guards %llu + CPU rings %llu, mispredicted %llu, at once %llu, slot-range misses %llu\n",
           what, bad ? "DIFFERS" : "journal equal", r->n, (unsigned long long)draws, r->st.ops[VKN_RT_OP_DRAW] / f, r->st.ops[VKN_RT_OP_UBO] / f, r->st.ops[VKN_RT_OP_SLOTS] / f,
           r->st.ops[VKN_RT_OP_CALL] / f, r->st.syncs / f, r->st.sync_wait_ns / 1e6 / f, r->st.bp_wait_ns / 1e6 / f, (unsigned long long)r->st.ahead[0], (unsigned long long)r->st.ahead[1],
           (unsigned long long)r->st.ahead[2], (unsigned long long)r->st.ahead[3], (unsigned long long)r->st.guard_ring, (unsigned long long)r->st.guard_cpu, (unsigned long long)r->st.mispredict,
           (unsigned long long)r->st.inline_ops, (unsigned long long)r->st.slot_gen_miss);
    return !bad;
}
static void rt_slots_init(void) {
    for (int i = 0; i < VKN_TEX_SLOTS; i++) rt_ftex[i] = rt_fsmp[i] = rt_ctex[i] = rt_csmp[i] = -1;
    rt_ftex[0] = 300; rt_fsmp[0] = 10; rt_ftex[3] = 303; rt_fsmp[3] = 13; rt_ctex[0] = 300; rt_csmp[0] = 10;
}
static void test_rt(void) {
    { static const char *const own[] = {"test", "a5-a", "a5-b", "b4-skipper"}; for (size_t i = 0; i < sizeof own / sizeof own[0]; i++) vkn_fx_drop(own[i]); }   // (earlier tests' own functions: not the game's)
    rt_setup();
    // Warm-up in this process (mode 0): every shader pair, module variant and pipeline the scenario needs exists before the forks (a
    // forked child has no vkn_shader worker thread: a module request there would wait for it forever), and every child starts from
    // the same state.
    vkn_rt_install(0, 1); rt_seed = 999;
    pthread_mutex_lock(&mk_mu); mk_jr_on = 1; if (!mk_jr_recording()) mk_ev_base = mk_seq; pthread_mutex_unlock(&mk_mu);   // (the log restarts after each submission; records not kept)
    for (uint32_t f = 0; f < 60; f++) rt_frame(f);
    rtx_setup();   // [stage 5.2 audit] and every extension's objects / pipelines (the shadow cache only in its children: installed once a process)
    { static const int wx[] = {RTX_UI1, RTX_UI3, RTX_POL, RTX_MAT, RTX_LF}; for (size_t i = 0; i < sizeof wx / sizeof wx[0]; i++) { rtx_on(wx[i]); g_rtx = wx[i]; for (uint32_t f = 0; f < 30; f++) rt_frame(f); rtx_off(); g_rtx = 0; } }
    pthread_mutex_lock(&mk_mu); mk_jr_on = 0; mk_jr_n = 0; pthread_mutex_unlock(&mk_mu);
    rt_dyn_head = 0;
    rt_slots_init();
    static rt_res ref, x, ref2;
    {   // VKN_RT_DIRECT=mode:slots:yield:frames:ring - one run in THIS process, its journal into the file VKN_RT_OUT (ThreadSanitizer
        // refuses threads in a child forked from a process with threads: work/rt_test.sh compares the files of separate processes)
        const char *d = getenv("VKN_RT_DIRECT");
        if (d && *d) {
            rt_cfg c = {0, 1, 0, 240, 0, 0}; unsigned long long rl = 0;
            sscanf(d, "%d:%d:%d:%u:%llu:%d", &c.mode, &c.slots, &c.yield, &c.frames, &rl, &c.ext); c.ring_limit = rl;
            rt_res *r = (rt_res *)calloc(1, sizeof *r);
            rt_run(&c, r);
            const char *o = getenv("VKN_RT_OUT");
            FILE *fo = o ? fopen(o, "w") : NULL;
            if (fo) { for (uint32_t i = 0; i < r->n; i++) fprintf(fo, "%016llx %u %u\n", (unsigned long long)r->rec[i].h, r->rec[i].nev, r->rec[i].ndraw); fclose(fo); }
            CHECK_EQ(r->val_errors, 0); CHECK_EQ(r->mk_errors, 0);
            if (c.mode == 1 || c.mode == 2) { CHECK(r->st.busy_ns > 0); CHECK_EQ(r->st.mispredict, 0); CHECK_EQ(r->st.foreign, 0); }
            printf("rt direct %s: %u submissions, syncs %llu, ahead 0:%llu 1:%llu 2:%llu 3+:%llu, ring guards %llu\n", d, r->n, (unsigned long long)r->st.syncs,
                   (unsigned long long)r->st.ahead[0], (unsigned long long)r->st.ahead[1], (unsigned long long)r->st.ahead[2], (unsigned long long)r->st.ahead[3], (unsigned long long)r->st.guard_ring);
            free(r);
            return;
        }
    }
    const uint32_t F = 240;
    const rt_cfg c0 = {0, 1, 0, F, 0};
    if (rt_child(&c0, &ref)) return;
    { uint64_t d = 0; for (uint32_t i = 0; i < ref.n; i++) d += ref.rec[i].ndraw; CHECK(ref.n >= F); CHECK(d >= (uint64_t)F * 8); printf("rt reference (DM_VK_NATIVE_RT=0): %u submissions, %llu draws, validator %d, mock %d\n", ref.n, (unsigned long long)d, ref.val_errors, ref.mk_errors); }
    CHECK_EQ(ref.val_errors, 0); CHECK_EQ(ref.mk_errors, 0);
    const struct { rt_cfg c; const char *what; } runs[] = {
        {{9, 1, 0, F, 0}, "RT=9 (verify)"}, {{1, 1, 0, F, 0}, "RT=1"}, {{2, 1, 0, F, 0}, "RT=2"},
        {{1, 0, 0, F, 0}, "RT=1 slots copied"}, {{1, 2, 0, F, 0}, "RT=1 slots by ranges"}, {{2, 3, 0, F, 0}, "RT=2 slots ranges verified"},
        {{1, 1, 0, F, 12 * 1024}, "RT=1 ring guard 12 KB"}, {{2, 1, 0, F, 12 * 1024}, "RT=2 ring guard 12 KB"},
        {{1, 1, 1, F, 0}, "RT=1 yields"}, {{2, 1, 1, F, 0}, "RT=2 yields"},
    };
    for (size_t i = 0; i < sizeof runs / sizeof runs[0]; i++) {
        if (rt_child(&runs[i].c, &x)) continue;
        rt_same(runs[i].what, &ref, &x);
        if (runs[i].c.mode == 1 || runs[i].c.mode == 2) { CHECK(x.st.busy_ns > 0); CHECK(x.st.syncs > 0); CHECK_EQ(x.st.mispredict, 0); CHECK_EQ(x.st.foreign, 0); CHECK_EQ(x.st.oom, 0); }
        if (runs[i].c.ring_limit) CHECK(x.st.guard_ring > 0);
        if (runs[i].c.slots == 3) CHECK_EQ(x.st.slot_gen_miss, 0);
        if (runs[i].c.mode == 2 && runs[i].c.yield) CHECK(x.st.guard_cpu > 0);   // the CPU-written ring wrapped onto frames in flight
    }
    {   // [stage 5.2 audit] the pieces merged after stage 5.5: a reference per extension, the modes against it
        static const struct { int ext; const char *what; } ex[] = {{RTX_UI1, "UIPASS=1"}, {RTX_UI3, "UIPASS=3"}, {RTX_POL, "LOAD=1 STORE=1 FRESH=2"},
                                                                   {RTX_MAT, "MAT=2 gate"}, {RTX_SHC, "SHCACHE=1"}, {RTX_LF, "LIGHTFUSE"}};
        static rt_res xr;
        for (size_t i = 0; i < sizeof ex / sizeof ex[0]; i++) {
            const uint32_t XF = 160;
            const rt_cfg r0 = {0, 1, 0, XF, 0, ex[i].ext};
            if (rt_child(&r0, &xr)) continue;
            CHECK_EQ(xr.val_errors, 0); CHECK_EQ(xr.mk_errors, 0);
            printf("rt %s reference: %u submissions, the piece acted %llu times\n", ex[i].what, xr.n, (unsigned long long)xr.xm);
            CHECK(xr.xm > 0);
            const rt_cfg rm[3] = {{9, 1, 0, XF, 0, ex[i].ext}, {1, 1, 1, XF, 0, ex[i].ext}, {2, 1, 1, XF, 0, ex[i].ext}};
            for (int m = 0; m < 3; m++) {
                if (rt_child(&rm[m], &x)) continue;
                char w[64]; snprintf(w, sizeof w, "%s RT=%d%s", ex[i].what, rm[m].mode, rm[m].yield ? " yields" : "");
                rt_same(w, &xr, &x);
                CHECK_EQ(x.xm, xr.xm);   // the piece decided the same in every mode
            }
        }
    }
    // the stress run: VKN_RT_STRESS=<repetitions>[:<frames>] (default 2 x 400; the night check ran 20 x 2000)
    uint32_t reps = 2, sf = 400;
    { const char *e = getenv("VKN_RT_STRESS"); if (e && *e) { reps = (uint32_t)atoi(e); const char *c = strchr(e, ':'); if (c) sf = (uint32_t)atoi(c + 1); } }
    if (!reps || !sf) return;
    if (sf > 4400) sf = 4400;
    const rt_cfg cr = {0, 1, 0, sf, 0};
    if (rt_child(&cr, &ref2)) return;
    uint32_t same = 0;
    for (uint32_t k = 0; k < reps; k++) for (int m = 1; m <= 2; m++) {
        const rt_cfg cs = {m, k % 3 == 2 ? 3 : 1, 1, sf, k & 1 ? 12 * 1024 : 0};
        if (rt_child(&cs, &x)) continue;
        char w[64]; snprintf(w, sizeof w, "stress %u x %u RT=%d%s%s", k + 1, sf, m, k & 1 ? " guard" : "", k % 3 == 2 ? " slots 3" : "");
        same += (uint32_t)rt_same(w, &ref2, &x);
    }
    printf("rt stress: %u of %u runs (%u frames each, random yields) with the journal of DM_VK_NATIVE_RT=0\n", same, 2 * reps, sf);
}

// ================================================================================================================ save-game thumbnail
// Shared render-target storage (vkn_tex_create_shared): the frame function 0xa66bc0 makes "ScreenshotBackBuffer" the window's override
// target, renders the scene off-frame and copies the override into a small read-back texture. With a render scale the scene's last
// pass draws into the scaled back buffer "HalfWidthBackBuffer" instead, which is the same storage key (flag 0x2000): the same memory on
// the Switch. Phone run V5_b (separate images): the copy read a never-drawn target whose remembered clear ran just before it - zeros.
// Checked: the two names are one image; the override's clear folds into the scene's pass (LOAD_OP_CLEAR, not a clear command after the
// draw); the copy (a scaled blit) reads the drawn image after the draw; the separate-image case shows the old failure; the override's
// destroy leaves the back buffer working.
static vkn_tex *shtex(uint32_t idx, uint32_t w, uint32_t h, uint32_t pool, uint64_t off, const char *name) {
    vkn_tex_desc d; memset(&d, 0, sizeof d);
    d.efmt = EF_RGBA8; d.nvn_target = 1; d.w = w; d.h = h; d.depth = 1; d.mips = 1; d.layers = 1; d.samples = 1; d.cls = VKN_TC_COLOR;
    d.pool_id = pool; for (int i = 0; i < VKN_TEX_LAYER_IDS; i++) d.layer_ids[i] = ~0u;
    d.sampler_id = ~0u; d.cookie = 0xC00000ull + idx;
    const vkn_tex_share sh = {0x7000, off, (uint64_t)w * h * 4, name, "HalfWidthBackBuffer"};
    return vkn_tex_create_shared(H(idx, 1), &d, NULL, &sh, NULL);
}
static int clears_of(uint64_t from, VkImage img) { int n = 0; for (int k = 0; nth(from, VK_NULL_HANDLE, MK_E_CLEAR_COLOR_IMG, k); k++) n += nth(from, VK_NULL_HANDLE, MK_E_CLEAR_COLOR_IMG, k)->a[0] == VH(img); return n; }
static const mk_ev *clear_of(uint64_t from, VkImage img) {   // the first colour clear command of img since from
    for (int k = 0; nth(from, VK_NULL_HANDLE, MK_E_CLEAR_COLOR_IMG, k); k++) if (nth(from, VK_NULL_HANDLE, MK_E_CLEAR_COLOR_IMG, k)->a[0] == VH(img)) return nth(from, VK_NULL_HANDLE, MK_E_CLEAR_COLOR_IMG, k);
    return NULL;
}
static void test_thumbnail(void) {
    vkn_tex *HW = shtex(640, 64, 32, 640, 0x1000, "HalfWidthBackBuffer"), *SS = shtex(641, 64, 32, 641, 0x1000, "ScreenshotBackBuffer");
    vkn_tex *RB = mktex(642, EF_RGBA8, VKN_TC_SAMPLED, 1, 32, 16, 1, 1, 642, 0);   // the read-back texture (480x270 on the phone)
    vkn_tex *SS2 = mktex(643, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 643, 0), *HW2 = mktex(644, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 644, 0);
    CHECK(HW && SS && RB && SS2 && HW2); CHECK(SS == HW);
    if (!HW || SS != HW || !RB || !SS2 || !HW2) return;
    CHECK(vkn_tex_by_cookie(0xC00000ull + 641) == HW); CHECK(vkn_tex_view_by_id(641, NULL) == HW->view);
    const int val0 = mk_val_errors, err0 = mk_errors;
    vkn_draw_in in = g_in; in.shader = H(30, 1); in.ib = H(52, 1); in.prim = 1;
    g_ftex[0] = 300; g_ftex[3] = 303; g_fsmp[3] = 13;
    const float black[4] = {0, 0, 0, 0};
    // ---- one image: clear the override, the scene into the scaled back buffer, copy the override
    uint64_t m = mark();
    VkCommandBuffer cb = fbegin(0);   // off-frame (0xa66bc0 renders the screenshot outside the presented frame)
    vkn_pass_clear_color(vkn_tex_by_cookie(0xC00000ull + 641), 0, black, NULL);
    targets(HW, NULL, NULL);
    CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
    vkn_pass_copy(RB, vkn_tex_by_cookie(0xC00000ull + 641));
    fsubmit(1);
    const mk_ev *rp = nth(m, cb, MK_E_BEGIN_RP, 0), *dr = nth(m, cb, MK_E_DRAW_IDX, 0), *er = nth(m, cb, MK_E_END_RP, 0), *bl = nth(m, cb, MK_E_BLIT_IMG, 0);
    CHECK(rp && dr && er && bl);
    if (rp && dr && er && bl) {
        CHECK(att_of(rp, 0) && att_of(rp, 0)->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);   // the override's clear is the scene pass's clear
        mk_obj *fb = fb_of(rp); CHECK(fb && fb->fb_view[0] == HW->att);
        CHECK(rp->seq < dr->seq && dr->seq < er->seq && er->seq < bl->seq);
        CHECK_EQ(bl->a[0], VH(HW->image)); CHECK_EQ(bl->a[1], VH(RB->image));   // the copy reads the image that was drawn
        CHECK_EQ(clears_of(dr->seq, HW->image), 0);                           // and nothing cleared it after the draw
    }
    CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 1);
    // ---- separate images (the old backend): the copy reads a target nothing was drawn into, its remembered clear run right before it
    m = mark();
    cb = fbegin(0);
    vkn_pass_clear_color(SS2, 0, black, NULL);
    targets(HW2, NULL, NULL);
    CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
    vkn_pass_copy(RB, SS2);
    fsubmit(1);
    bl = nth(m, cb, MK_E_BLIT_IMG, 0); const mk_ev *cc = clear_of(m, SS2->image); dr = nth(m, cb, MK_E_DRAW_IDX, 0);
    CHECK(bl && cc && dr && bl->a[0] == VH(SS2->image) && cc->a[0] == VH(SS2->image) && dr->seq < cc->seq && cc->seq < bl->seq);
    // (the audit question: a remembered clear of the copy source is recorded BEFORE the copy, also when the trace line of the copy
    // - printed at vkn_pass_copy's entry - comes first)
    // ---- the override goes (0x912700's reference drops to zero): the back buffer keeps its image and its state
    const VkImage hwi = HW->image;
    CHECK_EQ(vkn_tex_destroy_cookie(0xC00000ull + 641), 1);
    CHECK(!HW->dead); CHECK(vkn_tex_get(H(640, 1)) == HW); CHECK(vkn_tex_by_cookie(0xC00000ull + 641) == NULL); CHECK(vkn_tex_view_by_id(641, NULL) == VK_NULL_HANDLE);
    m = mark();
    cb = fbegin(1);
    targets(HW, NULL, NULL); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
    targets(NULL, NULL, NULL);
    fsubmit(0);
    rp = nth(m, cb, MK_E_BEGIN_RP, 0);
    CHECK(att_of(rp, 0) && att_of(rp, 0)->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD); CHECK(fb_of(rp) && fb_of(rp)->fb_view[0] == HW->att);
    CHECK(mk_is_live(VH(hwi), MK_IMAGE));
    CHECK_EQ(mk_val_errors, val0); CHECK_EQ(mk_errors, err0);
    vkn_tex_destroy_cookie(0xC00000ull + 640); vkn_tex_destroy_cookie(0xC00000ull + 642); vkn_tex_destroy_cookie(0xC00000ull + 643); vkn_tex_destroy_cookie(0xC00000ull + 644);
    frames_n(4);
    CHECK(!mk_is_live(VH(hwi), MK_IMAGE));
}

// ================================================================================================================ UI smoke
// GDraw through vkn_ui: targets, a full and a masked stencil clear, a textured quad draw, an indexed draw, then an engine draw (which
// must rebind everything after the UI's own binds). Checked: the calls draw, the validator finds nothing.
static uint8_t g_uictl[2][VKN_UI_CTL], g_uicode[2][64];
static void test_ui(void) {
    const VkDescriptorType UB = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    xbg_vkn_stage *vs = mk_bundle_add("u300000000000001", 0);
    mk_bind(vs, 0, UB, XBG_SOURCE_UNIFORM, 0, 128); vs->input_count = 2; vs->inputs[0] = 0; vs->inputs[1] = 1;
    xbg_vkn_stage *fs = mk_bundle_add("u300000000000002", 4);
    mk_bind(fs, 0, UB, XBG_SOURCE_UNIFORM, 0, 48); mk_bind(fs, 1, CIS, XBG_SOURCE_TEXTURE, 0, 0); mk_bind(fs, 2, CIS, XBG_SOURCE_TEXTURE, 7, 0);
    memcpy(g_uictl[0], "u300000000000001", 16); memcpy(g_uictl[1], "u300000000000002", 16);
    CHECK_EQ(vkn_ui_program_put(0xA1, 0, g_uictl[0], g_uicode[0]), 0); CHECK_EQ(vkn_ui_program_put(0xA2, 1, g_uictl[1], g_uicode[1]), 0);
    vkn_tex *W = g_win, *UD = mktex(600, EF_D24S8, VKN_TC_DEPTH, 1, 64, 32, 1, 1, 600, 0);
    CHECK(W && UD && g_t0); if (!W || !UD || !g_t0) return;
    const int val0 = mk_val_errors, err0 = mk_errors;
    vkn_ui_stats u0, u1; vkn_ui_stats_get(&u0);
    const VkCommandBuffer cb = fbegin(1);
    const uint64_t m = mark();
    vkn_ui_targets(W, UD); vkn_ui_viewport(0, 0, 64, 32); vkn_ui_scissor(0, 0, 64, 32);
    vkn_ui_clear_ds(1, 0xff);
    float verts[16] = {0, 0, 0, 0,  64, 0, 1, 0,  64, 32, 1, 1,  0, 32, 0, 1};
    float vsc[32] = {1}, fsc[12] = {1};
    vkn_ui_draw_in d; memset(&d, 0, sizeof d);
    d.vs = 0xA1; d.fs = 0xA2; d.vfmt = VKN_UI_VF_V2TC2; d.quads = 1; d.blend = 1; d.ds = 1; d.stencil_cmp = 0xff; d.stencil_write = 0;
    d.tex[0] = g_t0; d.vsc = vsc; d.vsc_bytes = sizeof vsc; d.fsc = fsc; d.fsc_bytes = sizeof fsc; d.verts = verts; d.nverts = 4;
    CHECK_EQ(vkn_ui_draw(&d), 0);
    vkn_ui_clear_ds(0, 0x0f);   // a partial stencil mask: a draw of its own
    static const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
    d.quads = 0; d.idx = idx; d.nidx = 6; d.tex[0] = NULL; d.blend = 0; d.ds = 0;
    CHECK_EQ(vkn_ui_draw(&d), 0);
    // the engine draws again after the UI: everything rebound
    vkn_draw_in in = g_in; targets(W, NULL, NULL);
    const uint64_t m2 = mark();
    CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
    CHECK_EQ(cnt(m2, cb, MK_E_BIND_PIPE), 1); CHECK_EQ(cnt(m2, cb, MK_E_BIND_SETS), 1); CHECK(vb_bound(m2, cb) >= 2); CHECK_EQ(cnt(m2, cb, MK_E_BIND_IB), 1);
    CHECK_EQ(cnt(m2, cb, MK_E_VIEWPORT), 1); CHECK_EQ(cnt(m2, cb, MK_E_SCISSOR), 1);
    vkn_rec_present(W, NULL, 1);
    fsubmit(0);
    vkn_ui_stats_get(&u1);
    CHECK_EQ(u1.draws, u0.draws + 2); CHECK_EQ(u1.clears_masked, u0.clears_masked + 1);
    CHECK(cnt(m, cb, MK_E_DRAW) + cnt(m, cb, MK_E_DRAW_IDX) >= 4);   // 2 UI draws, the masked clear, the engine draw
    const mk_ev *r = nth(m, cb, MK_E_BEGIN_RP, 0);
    CHECK(att_of(r, 1) && att_of(r, 1)->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && att_of(r, 1)->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);   // the full clear folded
    CHECK_EQ(mk_val_errors, val0); CHECK_EQ(mk_errors, err0);
}

// ================================================================================================================ queries (stage 4)
// vkn_query.h: a report pair = one occlusion query inside one pass instance, results copied after the pass end, consumers drawn under
// VK_EXT_conditional_rendering on their word; the exact CPU skip of an empty pair; unusable pairs (pass end inside, a draw dropped
// inside, nesting) and unknown / same-pass / off-frame conditions drawn; slot reuse across frames with the verdict readback; modes
// 0 / 2 / 3 and a device without the extension. The mock runs queries and predicates at record time (vkn_mock_vk.h [stage 4]).
int dm_vk_cond_render = 1;   // the front end's flag (android/vulkan_present.cpp): the extension is enabled on the device
static const mk_ev *q_reset_of(uint64_t from, VkCommandBuffer cb, uint64_t pool, uint32_t n) {   // the query reset of `pool` (any pool: 0) over n queries
    for (uint64_t s = from; s < mk_seq; s++) { const mk_ev *e = mk_ev_at(s); if (e && e->cb == cb && e->op == MK_E_RESET_QP && (!pool || e->a[0] == pool) && e->u[0] == 0 && e->u[1] == n) return e; }
    return NULL;
}
static int q_draw(const vkn_draw_in *in) { return vkn_draw_indexed(in, 0, 3, 1); }
static void test_query(void) {
    if (!g_s1 || !g_a2 || !g_b2 || !g_d3) return;
    enum { K1 = 0x51000, K2 = 0x51010, K3 = 0x51020, K4 = 0x51030, K5 = 0x51040, K6 = 0x51050, K7 = 0x51060, K8 = 0x51070, K99 = 0x5ff00 };
    const int val0 = mk_val_errors, err0 = mk_errors;
    mk_cond_ext = 1; vkn_query_init(VKN_COND_ON);
    CHECK_EQ(vkn_query_mode(), VKN_COND_ON);
    vkn_query_stats a, b; vkn_query_stats_get(&a);
    vkn_draw_in in = g_in;
    uint64_t m0 = mark();
    VkCommandBuffer cb = fbegin(1);
    // ---- a new slot: its whole pool is reset before anything else of the frame
    const mk_ev *rs = q_reset_of(m0, cb, 0, VKN_Q_CAP);
    CHECK(rs != NULL); const uint64_t pool1 = rs ? rs->a[0] : 0;
    CHECK(vkn_query_gpu());
    targets(g_a2, NULL, g_d3);
    // ---- 1. a pair inside one pass, 5 samples: begin lazily at the first draw (the pass is open then), end at the second report
    mk_draw_samples = 5;
    uint64_t m = mark();
    vkn_query_begin(K1); vkn_query_cond(0); CHECK_EQ(q_draw(&in), 0); vkn_query_end(K1);
    const mk_ev *rp = nth(m, cb, MK_E_BEGIN_RP, 0), *bq = nth(m, cb, MK_E_BEGIN_Q, 0), *dq = nth(m, cb, MK_E_DRAW_IDX, 0), *eq = nth(m, cb, MK_E_END_Q, 0);
    CHECK(rp && bq && dq && eq);
    if (rp && bq && dq && eq) {
        CHECK(rp->seq < bq->seq && bq->seq < dq->seq && dq->seq < eq->seq);
        CHECK_EQ(bq->a[0], pool1); CHECK_EQ(bq->u[0], 0); CHECK_EQ(bq->u[1], 0);   // query 0, not precise
        CHECK_EQ(eq->u[1], 5);
    }
    // ---- 2. a pair with two draws and no sample passing (query 1)
    mk_draw_samples = 0; m = mark();
    vkn_query_begin(K2); CHECK_EQ(q_draw(&in), 0); CHECK_EQ(q_draw(&in), 0); vkn_query_end(K2);
    OPS(m, cb, MK_E_BEGIN_Q, MK_E_DRAW_IDX, MK_E_DRAW_IDX, MK_E_END_Q);
    CHECK(nth(m, cb, MK_E_BEGIN_Q, 0) && nth(m, cb, MK_E_BEGIN_Q, 0)->u[0] == 1);
    // ---- 3. a pair without a draw: no query at all (zero samples, exact)
    m = mark(); vkn_query_begin(K3); vkn_query_end(K3); NO_OPS(m, cb);
    // ---- 4. a pair broken by a pass end (query 2): its query ends before the pass; the copy of queries 0..2 follows the pass end
    m = mark();
    vkn_query_begin(K4); CHECK_EQ(q_draw(&in), 0);
    targets(g_b2, NULL, g_d3);   // a different set: the pass ends now
    mk_draw_samples = 9; CHECK_EQ(q_draw(&in), 0); vkn_query_end(K4);
    const mk_ev *cp = nth(m, cb, MK_E_COPY_Q, 0), *cb_bar = cp ? nth(cp->seq, cb, MK_E_BARRIER, 0) : NULL, *er = nth(m, cb, MK_E_END_RP, 0);
    eq = nth(m, cb, MK_E_END_Q, 0); rp = nth(m, cb, MK_E_BEGIN_RP, 0); dq = nth(m, cb, MK_E_DRAW_IDX, 1);
    CHECK_EQ(cnt(m, cb, MK_E_BEGIN_Q), 1); CHECK_EQ(cnt(m, cb, MK_E_END_Q), 1); CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 2);
    uint64_t base = 0;
    CHECK(cp && cb_bar && er && eq && rp && dq);
    if (cp && cb_bar && er && eq && rp && dq) {
        CHECK(eq->seq < er->seq && er->seq < cp->seq && cp->seq + 1 == cb_bar->seq && cb_bar->seq < rp->seq && rp->seq < dq->seq);
        CHECK_EQ(cp->a[0], pool1); CHECK_EQ(cp->u[0], 0); CHECK_EQ(cp->u[1], 3); CHECK_EQ(cp->u[2], 4); CHECK_EQ(cp->u[3], VK_QUERY_RESULT_WAIT_BIT);
        base = cp->off[0]; CHECK_EQ(base % (4u * VKN_Q_CAP), 0);
        mk_obj *pb = mk_obj_of(cp->a[1]);
        CHECK(pb && (pb->usage & VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT));
        CHECK(cb_bar->a[0] == 0 && cb_bar->u[0] == VK_PIPELINE_STAGE_TRANSFER_BIT && (cb_bar->u[1] & VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT));
        CHECK(cb_bar->u[2] == VK_ACCESS_TRANSFER_WRITE_BIT && (cb_bar->u[3] & VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT));
        const uint32_t *w = (const uint32_t *)buf_ptr(cp->a[1], base);
        CHECK(w && w[0] == 5 && w[1] == 0 && w[2] == 0);   // the copied words: K1 5 samples, K2 none, K4 none before its pass ended
    }
    // ---- 5. consumers in the next pass
    m = mark(); vkn_query_cond(K1); CHECK_EQ(q_draw(&in), 0);   // visible: drawn under its predicate
    OPS(m, cb, MK_E_BEGIN_COND, MK_E_DRAW_IDX, MK_E_END_COND);
    const mk_ev *bc = nth(m, cb, MK_E_BEGIN_COND, 0), *d1 = nth(m, cb, MK_E_DRAW_IDX, 0);
    CHECK(bc && bc->off[0] == base + 0 && bc->u[0] == 0 && bc->u[1] == 5 && d1 && d1->u[7] == 0);
    m = mark(); vkn_query_cond(K2); CHECK_EQ(q_draw(&in), 0);   // hidden: recorded, the GPU discards it
    OPS(m, cb, MK_E_BEGIN_COND, MK_E_DRAW_IDX, MK_E_END_COND);
    bc = nth(m, cb, MK_E_BEGIN_COND, 0); d1 = nth(m, cb, MK_E_DRAW_IDX, 0);
    CHECK(bc && bc->off[0] == base + 4 && d1 && d1->u[7] == 1);
    m = mark(); CHECK_EQ(q_draw(&in), 0); OPS(m, cb, MK_E_DRAW_IDX);   // a condition covers one draw only
    m = mark(); vkn_query_cond(K3); CHECK_EQ(q_draw(&in), 0); NO_OPS(m, cb);   // the empty pair: skipped on the CPU, nothing recorded
    m = mark(); vkn_query_cond(K4); CHECK_EQ(q_draw(&in), 0); OPS(m, cb, MK_E_DRAW_IDX);   // broken pair: drawn
    m = mark(); vkn_query_cond(K99); CHECK_EQ(q_draw(&in), 0); OPS(m, cb, MK_E_DRAW_IDX);  // no pair of that name in this recording
    // ---- 6. a consumer in its pair's own pass (query 3 not copied yet): drawn
    mk_draw_samples = 0; m = mark();
    vkn_query_begin(K5); CHECK_EQ(q_draw(&in), 0); vkn_query_end(K5); vkn_query_cond(K5); CHECK_EQ(q_draw(&in), 0);
    OPS(m, cb, MK_E_BEGIN_Q, MK_E_DRAW_IDX, MK_E_END_Q, MK_E_DRAW_IDX);
    // ---- 7. a draw dropped inside a pair: the pair is unusable (the Switch may have drawn it), no query, its consumer drawn
    vkn_draw_in bad = in; bad.shader = H(99, 1);
    m = mark(); vkn_query_begin(K6); CHECK_EQ(vkn_draw_indexed(&bad, 0, 3, 1), 1); vkn_query_end(K6); NO_OPS(m, cb);
    m = mark(); vkn_query_cond(K6); CHECK_EQ(q_draw(&in), 0); OPS(m, cb, MK_E_DRAW_IDX);
    // ---- 8. nesting: the open pair's query (4) ends at the inner begin; the inner pair gets query 5; the outer end is unmatched
    m = mark();
    vkn_query_begin(K7); CHECK_EQ(q_draw(&in), 0); vkn_query_begin(K8); CHECK_EQ(q_draw(&in), 0); vkn_query_end(K8); vkn_query_end(K7);
    OPS(m, cb, MK_E_BEGIN_Q, MK_E_DRAW_IDX, MK_E_END_Q, MK_E_BEGIN_Q, MK_E_DRAW_IDX, MK_E_END_Q);
    // ---- the recording ends: the pass end copies queries 3..5
    m = mark(); fsubmit(0);
    cp = nth(m, cb, MK_E_COPY_Q, 0);
    CHECK(cp && cp->u[0] == 3 && cp->u[1] == 3 && cp->off[0] == base + 12);
    CHECK_EQ(cnt(m, cb, MK_E_COPY_Q), 1);
    vkn_query_stats_get(&b);
    CHECK_EQ(b.begins - a.begins, 8); CHECK_EQ(b.ends - a.ends, 8); CHECK_EQ(b.queries - a.queries, 6); CHECK_EQ(b.empty - a.empty, 1);
    CHECK_EQ(b.broken - a.broken, 1); CHECK_EQ(b.lost - a.lost, 1); CHECK_EQ(b.nested - a.nested, 1); CHECK_EQ(b.unmatched - a.unmatched, 1);
    CHECK_EQ(b.copies - a.copies, 2); CHECK_EQ(b.copied - a.copied, 6);
    CHECK_EQ(b.conds - a.conds, 7); CHECK_EQ(b.cond_gpu - a.cond_gpu, 2); CHECK_EQ(b.cond_cpu_skip - a.cond_cpu_skip, 1);
    CHECK_EQ(b.drawn_no_query - a.drawn_no_query, 1); CHECK_EQ(b.drawn_unusable - a.drawn_unusable, 2); CHECK_EQ(b.drawn_same_pass - a.drawn_same_pass, 1);
    CHECK_EQ(b.slots_taken - a.slots_taken, 1);
    // ---- 9. slot reuse: frame 1's slot comes back after VKN_Q_SLOTS presented recordings (its recording finished: retirement rule);
    // its 6 queries are reset, the verdicts on its 2 predicated draws are read (K1 drawn, K2 culled)
    for (int i = 0; i < VKN_Q_SLOTS - 1; i++) { fbegin(1); fsubmit(0); }
    vkn_query_stats_get(&a);
    m = mark(); cb = fbegin(1);
    CHECK(q_reset_of(m, cb, pool1, 6) != NULL);
    vkn_query_stats_get(&b);
    CHECK_EQ(b.results_read - a.results_read, 2); CHECK_EQ(b.gpu_culled - a.gpu_culled, 1); CHECK_EQ(b.gpu_drawn - a.gpu_drawn, 1);
    CHECK_EQ(b.slots_busy, 0); CHECK_EQ(b.slots_taken - a.slots_taken, 1);
    // the slot works again: a pair begins at query 0 of the same pool
    targets(g_a2, NULL, g_d3); mk_draw_samples = 1;
    m = mark(); vkn_query_begin(K1); CHECK_EQ(q_draw(&in), 0); vkn_query_end(K1);
    bq = nth(m, cb, MK_E_BEGIN_Q, 0); CHECK(bq && bq->a[0] == pool1 && bq->u[0] == 0);
    fsubmit(0);
    // ---- 10. an off-frame recording has no slot: the pair is unusable, its consumer drawn, no Vulkan query
    vkn_query_stats_get(&a);
    cb = fbegin(0); targets(g_a2, NULL, g_d3);
    m = mark(); vkn_query_begin(K1); CHECK_EQ(q_draw(&in), 0); vkn_query_end(K1); vkn_query_cond(K1); CHECK_EQ(q_draw(&in), 0);
    CHECK_EQ(cnt(m, cb, MK_E_BEGIN_Q), 0); CHECK_EQ(cnt(m, cb, MK_E_BEGIN_COND), 0); CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 2);
    fsubmit(1);
    vkn_query_stats_get(&b); CHECK_EQ(b.no_slot - a.no_slot, 1); CHECK_EQ(b.drawn_no_slot - a.drawn_no_slot, 1);
    // ---- 11. mode 2 (count): queries and copies, every draw recorded; mode 3 (poison): the predicate inverted
    for (int mode = VKN_COND_COUNT; mode <= VKN_COND_POISON; mode++) {
        vkn_query_init(mode); vkn_query_stats_get(&a);
        cb = fbegin(1); targets(g_a2, NULL, g_d3);
        mk_draw_samples = mode == VKN_COND_COUNT ? 0 : 5;
        vkn_query_begin(K1); CHECK_EQ(q_draw(&in), 0); vkn_query_end(K1);
        targets(g_b2, NULL, g_d3);
        m = mark(); vkn_query_cond(K1); CHECK_EQ(q_draw(&in), 0);
        CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 1); CHECK_EQ(cnt(m, cb, MK_E_BEGIN_COND), mode == VKN_COND_COUNT ? 0 : 1);
        if (mode == VKN_COND_COUNT) { d1 = nth(m, cb, MK_E_DRAW_IDX, 0); CHECK(d1 && d1->u[7] == 0); }
        else {
            bc = nth(m, cb, MK_E_BEGIN_COND, 0); d1 = nth(m, cb, MK_E_DRAW_IDX, 0);
            CHECK(bc && d1 && bc->seq < d1->seq && nth(m, cb, MK_E_END_COND, 0) && nth(m, cb, MK_E_END_COND, 0)->seq == d1->seq + 1);
            CHECK(bc && bc->u[0] == VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT && d1 && d1->u[7] == 1);   // visible, so the poison hides it
        }
        fsubmit(0);
        vkn_query_stats_get(&b);
        if (mode == VKN_COND_COUNT) CHECK_EQ(b.cond_count - a.cond_count, 1); else CHECK_EQ(b.cond_gpu - a.cond_gpu, 1);
    }
    // ---- 12. a device without the extension: no Vulkan query is made (nothing could use it), the pair's consumer is drawn, the empty
    // pair is still skipped
    mk_cond_ext = 0; vkn_query_init(VKN_COND_ON); CHECK_EQ(vkn_query_gpu(), 0);
    vkn_query_stats_get(&a);
    const uint64_t m12 = mark();
    cb = fbegin(1); targets(g_a2, NULL, g_d3); mk_draw_samples = 0;
    vkn_query_begin(K1); CHECK_EQ(q_draw(&in), 0); vkn_query_end(K1); vkn_query_begin(K3); vkn_query_end(K3);
    targets(g_b2, NULL, g_d3);
    m = mark();
    vkn_query_cond(K1); CHECK_EQ(q_draw(&in), 0); vkn_query_cond(K3); CHECK_EQ(q_draw(&in), 0);
    CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 1); CHECK_EQ(cnt(m, cb, MK_E_BEGIN_COND), 0);
    fsubmit(0);
    CHECK_EQ(cnt(m12, cb, MK_E_BEGIN_Q), 0); CHECK_EQ(cnt(m12, cb, MK_E_COPY_Q), 0);
    vkn_query_stats_get(&b); CHECK_EQ(b.drawn_no_ext - a.drawn_no_ext, 1); CHECK_EQ(b.cond_cpu_skip - a.cond_cpu_skip, 1);
    // ---- 13. mode 0 (the default): no Vulkan query work at all, conditions counted, every draw recorded
    vkn_query_init(VKN_COND_OFF); vkn_query_stats_get(&a);
    m0 = mark(); cb = fbegin(1); targets(g_a2, NULL, g_d3);
    vkn_query_begin(K1); CHECK_EQ(q_draw(&in), 0); vkn_query_end(K1); vkn_query_cond(K1); CHECK_EQ(q_draw(&in), 0);
    fsubmit(0);
    CHECK_EQ(cnt(m0, cb, MK_E_BEGIN_Q), 0); CHECK_EQ(cnt(m0, cb, MK_E_COPY_Q), 0); CHECK_EQ(cnt(m0, cb, MK_E_BEGIN_COND), 0); CHECK(q_reset_of(m0, cb, 0, VKN_Q_CAP) == NULL);
    CHECK_EQ(cnt(m0, cb, MK_E_DRAW_IDX), 2);
    vkn_query_stats_get(&b); CHECK_EQ(b.begins - a.begins, 1); CHECK_EQ(b.conds - a.conds, 1); CHECK_EQ(b.queries - a.queries, 0);
    CHECK_EQ(b.setup_fail, 0);
    mk_draw_samples = 1;
    CHECK_EQ(mk_val_errors, val0); CHECK_EQ(mk_errors, err0);
}

// ==== END OF AREAS ====

// ================================================================================================================ [stage 5.1 A5]
// The test of the assembled stage 5.1 (the owner's rule: one full test per stage), as new functions after the areas above (the blocks
// "[stage 5] draw cost" / "[stage 5.1 O2]" inside test_draw belong to other pieces). Areas:
//   shc      the static shadow cache (vkn_shc.c) against the mock's depth content model (vkn_mock_vk.h [stage 5.1 A5]): one scenario of
//            44 frames (3 cascades x 12 static + 2 moving casters into a D16 atlas, then a draw outside it and the lighting pass sampling
//            it) run in forked children with different switches; the atlas texels of every frame must equal those of the run without
//            the cache (the truth) - BUILD / HIT pairs, a member that does not come (RECOVER by replay), a precopied tile no hit takes,
//            a clear and a read of the atlas inside a cascade, a vertex-buffer write and a texture upload moving a member out of the
//            layer, GREATER and stencil draws (order rule), vkn_draw_forget_all inside a frame, the caster shader re-created, the
//            atlas destroyed and re-created at another size, a write behind vkn_buf_update; modes 2 (verify: equal, and a real
//            difference counted) and 3 (poison), DM_VK_NATIVE_SHFAR 1 / 2; the engine's targets and constant blocks after every own
//            draw of the cache; 3 recordings in flight (fence hold) for the verify read-backs; zero validation / mock errors.
//   fx       more of the draw-effect chain: chain order, a function that draws on its own under vkn_fx_bypass and changes targets,
//            skip of a rewritten primitive (fan / quad), effect constant blocks from an own buffer and from the ring, the
//            comparison twin of VKN_BF_CMP bindings and of cmp_mask (vkn_tex.c: cached, retired with its sampler, a comparing game
//            sampler kept), named fragment modules outside the verified pass (vkn_shader.c with test doubles of vkn_shlib_named),
//            warm-up listing of effect variants, pipeline key pack / unpack with the effect bits.
//   oracle   DM_VK_NATIVE_COND=4 (vkn_query.c): one oracle recording in VKN_ORACLE_PERIOD, CPU skips by the last verdict between,
//            object keys with occurrence numbers stable across frames, switching modes 4 -> 1 -> 0 -> 4 in a run.
#include <sys/wait.h>
#include <unistd.h>
#include "../../../vkn/vkn_shc.h"

// ---- forked children: a scenario runs in a copy of this process (the modules read their switches once), its result comes back
// through a pipe; its CHECKs are added to this process's counts
enum { A5_F = 44, A5_N = 12, A5_DYN = 2, A5_BEHIND = 42 };
typedef struct a5_res {
    int checks, fails, ran;
    uint64_t h[A5_F], th[A5_F][3]; uint8_t far_clear[A5_F];
    uint32_t sk[A5_F], cp[A5_F], rp[A5_F], bo[A5_F], rc[A5_F], pu[A5_F], pc[A5_F];
    vkn_shc_stats st;
    uint64_t model_draws;
} a5_res;
static int a5_child(const char *const env[][2], void (*fn)(a5_res *), a5_res *out) {
    memset(out, 0, sizeof *out);
    int pfd[2]; if (pipe(pfd)) return -1;
    fflush(stdout);
    const pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
    if (pid == 0) {
        close(pfd[0]);
        for (int i = 0; env && env[i][0]; i++) { if (env[i][1]) setenv(env[i][0], env[i][1], 1); else unsetenv(env[i][0]); }
        a5_res *r = (a5_res *)calloc(1, sizeof *r);
        const int c0 = g_checks, f0 = g_fails;
        fn(r);
        r->checks = g_checks - c0; r->fails = g_fails - f0; r->ran = 1;
        const uint8_t *p = (const uint8_t *)r; size_t left = sizeof *r;
        while (left) { const ssize_t w = write(pfd[1], p, left); if (w <= 0) break; p += w; left -= (size_t)w; }
        close(pfd[1]);
        _exit(0);
    }
    close(pfd[1]);
    uint8_t *p = (uint8_t *)out; size_t got = 0;
    while (got < sizeof *out) { const ssize_t n = read(pfd[0], p + got, sizeof *out - got); if (n <= 0) break; got += (size_t)n; }
    close(pfd[0]);
    int status = 0; waitpid(pid, &status, 0);
    const int ok = got == sizeof *out && WIFEXITED(status) && WEXITSTATUS(status) == 0 && out->ran;
    g_checks += out->checks + 1; g_fails += out->fails;
    if (!ok) { g_fails++; printf("FAIL %s:%d: child %s (status %#x, %zu of %zu result bytes)\n", __FILE__, __LINE__, env && env[0][0] ? env[0][0] : "?", status, got, sizeof *out); }
    return ok ? 0 : -1;
}

// ---- the shadow-cache scenario
typedef struct a5_cfg { int behind, hold; } a5_cfg;
static a5_cfg g_a5;
static int32_t g_a5_t0[VKN_TEX_SLOTS], g_a5_t1[VKN_TEX_SLOTS], g_a5_tl[VKN_TEX_SLOTS], g_a5_s[VKN_TEX_SLOTS];
static uint64_t a5_hash_rect(vkn_tex *t, int32_t x, int32_t y, int32_t w, int32_t h, int *all_clear) {
    uint32_t W = 0, Hh = 0; const uint16_t *p = t ? mk_depth_px(t->image, &W, &Hh) : NULL;
    uint64_t f = 0xcbf29ce484222325ull; int clr = 1;
    if (!p) { if (all_clear) *all_clear = 0; return 0; }
    for (int32_t j = y; j < y + h && j < (int32_t)Hh; j++) for (int32_t i = x; i < x + w && i < (int32_t)W; i++) {
        const uint16_t v = p[(size_t)j * W + i]; clr &= v == 0xffff;
        f = (f ^ (v & 0xff)) * 0x100000001b3ull; f = (f ^ (v >> 8)) * 0x100000001b3ull;
    }
    if (all_clear) *all_clear = clr;
    return f;
}
// One caster: its own constant block (vertex slot 2: cascade, index, salt), its own vertex range and indices; after the draw the
// engine's view must hold: its block announced, the atlas the depth target (whatever the cache recorded inside the call).
static void a5_cast(vkn_draw_in *in, vkn_tex *A, int k, int i, uint32_t salt, uint64_t ds) {
    uint32_t off = 0; CHECK_EQ(vkn_ring_alloc(64, &off), 0);
    uint32_t *u = (uint32_t *)((uint8_t *)vkn_ring_cpu() + off); memset(u, 0, 64);
    u[0] = (uint32_t)k; u[1] = (uint32_t)i; u[2] = salt; u[3] = 0x53484331u;
    vkn_draw_ubo(0, 2, off, 64);
    in->ds = ds; in->vb_off[0] = 16u * (uint32_t)(k * 20 + i);
    CHECK_EQ(vkn_draw_indexed(in, 3u * (uint32_t)(k * 20 + i), 3, 1), 0);
    uint32_t o = 0, z = 0; CHECK_EQ(vkn_draw_ubo_get(0, 2, &o, &z), 0); CHECK_EQ(o, off); CHECK_EQ(z, 64);
    CHECK(vkn_pass_depth() == A); CHECK(vkn_pass_color(0) == NULL);
}
static void a5_shc(a5_res *r) {
    mk_depth_on = 1; mk_fence_hold = g_a5.hold;
    const int v0 = mk_val_errors, e0 = mk_errors;
    vkn_shc_install();
    // the objects (created in the same order in every child: the content model never sees a handle, the cache's own images come later)
    vkn_tex *A = mktex(510, 49 /* NVN DEPTH16 */, VKN_TC_DEPTH, 1, 512, 512, 1, 1, 510, 0);
    vkn_tex *T = mktex(512, EF_RGBA8, VKN_TC_SAMPLED, 1, 16, 16, 1, 1, 512, 0);
    CHECK(A && T); if (!A || !T) return;
    CHECK(vkn_ds_put(H(70, 1), 1, 1, 1, 0, 0, 0, 0, 0) != 0);      // LESS with writes (the casters)
    CHECK(vkn_ds_put(H(71, 1), 1, 1, 4, 0, 0, 0, 0, 0) != 0);      // GREATER with writes: order-dependent
    CHECK(vkn_ds_put(H(72, 1), 1, 1, 1, 1, 0xff, 7, 0, 2) != 0);   // LESS + a stencil test
    uint64_t sh = H(80, 1);   // (made by the parent: a child has no shader worker thread - fork copies the calling thread only)
    { uint8_t pat[4096]; for (int i = 0; i < 4096; i++) pat[i] = (uint8_t)(i * 37 + (i >> 4) * 11 + 1);
      CHECK(vkn_buf_update(VKN_BUF_VB, H(50, 1), 0, pat, 4096, NULL, NULL) >= 0);   // (0 in place, 1 renamed)
      for (int i = 0; i < 4096; i++) pat[i] = (uint8_t)(i * 13 + (i >> 5) * 7 + 3);
      CHECK(vkn_buf_update(VKN_BUF_VB, H(51, 1), 0, pat, 4096, NULL, NULL) >= 0);
      for (int i = 0; i < 1024; i++) pat[i] = (uint8_t)((i & 1) ? 0 : (i / 2) % 7);
      CHECK(vkn_buf_update(VKN_BUF_IB, H(52, 1), 0, pat, 1024, NULL, NULL) >= 0); }
    for (int i = 0; i < VKN_TEX_SLOTS; i++) g_a5_t0[i] = g_a5_t1[i] = g_a5_tl[i] = g_a5_s[i] = -1;
    g_a5_t0[0] = 512; g_a5_t0[3] = 303; g_a5_t1[0] = 300; g_a5_t1[3] = 303; g_a5_tl[0] = 510; g_a5_tl[3] = 303; g_a5_s[0] = 10; g_a5_s[3] = 13;
    const uint64_t md0 = mk_depth_draws;
    for (int f = 0; f < A5_F; f++) {
        if (f == 35) { vkn_shader_destroy(sh); sh = H(81, 1); CHECK(vkn_shader_get(sh) != NULL); }   // the caster shader gone, another one of the same keys
        if (f == 38) {   // the atlas destroyed by the engine, a bigger one made
            CHECK_EQ(vkn_tex_destroy_cookie(0xC00000ull + 510), 1);
            A = mktex(511, 49, VKN_TC_DEPTH, 1, 1024, 1024, 1, 1, 511, 0); CHECK(A != NULL); if (!A) return;
            g_a5_tl[0] = 511;
        }
        if (f == 11) { uint8_t b[16]; memset(b, 0x77, sizeof b); CHECK(vkn_buf_update(VKN_BUF_VB, H(50, 1), 16u * 3, b, 16, NULL, NULL) >= 0); }   // caster (0, 3)
        if (f == 14) { uint8_t px[16 * 16 * 4]; memset(px, 0x31, sizeof px); CHECK_EQ(vkn_tex_update(T, 0, 0, 0, 0, 16, 16, px, 64), 0); }      // cascade 0's texture
        if (g_a5.behind && f == A5_BEHIND) {   // bytes of caster (2, 1) rewritten behind the buffer module (no version)
            vkn_buf_info bi; CHECK_EQ(vkn_buf_lookup(VKN_BUF_VB, H(50, 1), &bi), 0);
            if (bi.blk.cpu) memset((uint8_t *)bi.blk.cpu + 16u * (2 * 20 + 1), 0xee, 16);
        }
        vkn_shc_stats s0, s1; vkn_shc_stats_get(&s0);
        fbegin(1);
        targets(NULL, NULL, A); vkn_pass_clear_depth(A, 1, 1.0f, 0);
        const int32_t S = (int32_t)A->w, Q = S / 2, rect[3][2] = {{0, 0}, {Q, 0}, {0, Q}};
        for (int k = 0; k < 3; k++) {
            if (f == 17 && k == 2) continue;          // the engine skips a cascade: its precopied tile must go back to the clear value
            if (f == 32 && k == 1) vkn_draw_forget_all();
            vkn_draw_in in = g_in; in.shader = sh; in.tex[1] = k ? g_a5_t1 : g_a5_t0; in.smp[1] = g_a5_s; in.tex[2] = NULL; in.smp[2] = NULL;
            in.viewport[0] = (float)rect[k][0]; in.viewport[1] = (float)rect[k][1]; in.viewport[2] = in.viewport[3] = (float)Q;
            in.scissor[0] = rect[k][0]; in.scissor[1] = rect[k][1]; in.scissor[2] = in.scissor[3] = Q;
            for (int i = 0; i < A5_N; i++) {
                if (f == 8 && k == 1 && i == 5) continue;   // a member that does not come
                if (f == 20 && k == 0 && i == 6) vkn_pass_clear_depth(A, 1, 1.0f, 0);   // a clear inside a cascade
                if (f == 23 && k == 1 && i == 6) (void)vkn_pass_sample(A);           // a read inside a cascade (a dispatch binding it; 1 = feedback)
                if (f == 26 && k == 0 && i == 4) a5_cast(&in, A, k, 18, 0, H(71, 1));   // GREATER
                if (f == 29 && k == 1 && i == 4) a5_cast(&in, A, k, 19, 0, H(72, 1));   // stencil
                a5_cast(&in, A, k, i, 0, H(70, 1));
            }
            for (int j = 0; j < A5_DYN; j++) a5_cast(&in, A, k, A5_N + j, (uint32_t)f + 1, H(70, 1));   // moving casters
        }
        // the first draw outside the atlas (the cache's safe point): the engine's own targets and constant block must be bound for it;
        // frames 9 and 10: none - the lighting draw that samples the atlas is the safe point (cascade 1's layer is built there at 9)
        targets(g_a2, NULL, g_d3);
        if (f != 9 && f != 10) {
        uint32_t eo = 0; CHECK_EQ(vkn_ring_alloc(64, &eo), 0); memset((uint8_t *)vkn_ring_cpu() + eo, 0x45, 64); vkn_draw_ubo(0, 2, eo, 64);
        uint64_t m = mark();
        vkn_draw_in e = g_in; CHECK_EQ(vkn_draw_indexed(&e, 0, 3, 1), 0);
        { uint32_t o = 0, z = 0; CHECK_EQ(vkn_draw_ubo_get(0, 2, &o, &z), 0); CHECK_EQ(o, eo); CHECK_EQ(z, 64); }
        CHECK(vkn_pass_color(0) == g_a2); CHECK(vkn_pass_depth() == g_d3);
        {   // what the GPU saw for the engine's draw: the last pipeline / set bound before it, its slot-2 dynamic offset = the engine's block
            const mk_ev *d = NULL; for (int q = 0; nth(m, VK_NULL_HANDLE, MK_E_DRAW_IDX, q); q++) d = nth(m, VK_NULL_HANDLE, MK_E_DRAW_IDX, q);
            const mk_ev *bs = NULL; for (uint64_t q = m; d && q < d->seq; q++) { const mk_ev *x = mk_ev_at(q); if (x && x->op == MK_E_BIND_SETS && x->cb == d->cb) bs = x; }
            if (!bs) { for (uint64_t q = mk_ev_base; d && q < m; q++) { const mk_ev *x = mk_ev_at(q); if (x && x->op == MK_E_BIND_SETS && x->cb == d->cb) bs = x; } }
            CHECK(d && bs); if (bs) CHECK_EQ(bs->dyn[2], eo);   // g_s1 table order: bindings 0, 4 (SUPPORT), 6 (vertex slot 2), ...
        }
        }
        vkn_draw_in l = g_in; l.tex[1] = g_a5_tl; l.smp[1] = g_a5_s; CHECK_EQ(vkn_draw_indexed(&l, 0, 3, 1), 0);   // lighting samples the atlas
        fsubmit(0);
        vkn_shc_stats_get(&s1);
        int fc = 0;
        r->h[f] = a5_hash_rect(A, 0, 0, S, S, NULL);
        for (int k = 0; k < 3; k++) r->th[f][k] = a5_hash_rect(A, rect[k][0], rect[k][1], Q, Q, k == 2 ? &fc : NULL);
        r->far_clear[f] = (uint8_t)fc;
        r->sk[f] = (uint32_t)(s1.skipped - s0.skipped); r->cp[f] = (uint32_t)(s1.copies - s0.copies); r->rp[f] = (uint32_t)(s1.replayed - s0.replayed);
        r->bo[f] = (uint32_t)(s1.build_ok - s0.build_ok); r->rc[f] = (uint32_t)(s1.recover_now - s0.recover_now); r->pu[f] = (uint32_t)(s1.pre_undone - s0.pre_undone);
        r->pc[f] = (uint32_t)(s1.precopied - s0.precopied);
    }
    frames_n(4);   // the verify read-backs of the last frames retire
    mk_gpu_finish_all(); mk_fence_hold = 0; frames_n(1);
    vkn_shc_stats_get(&r->st);
    r->model_draws = mk_depth_draws - md0;
    vkn_shc_census(); vkn_fx_census();
    CHECK_EQ(mk_val_errors, v0); CHECK_EQ(mk_errors, e0);
}
static void test_a5_shc(void) {
#define A5_BASE {"DM_HACK_SHFAR", NULL}, {"DM_HACK_TIER", NULL}, {"DM_VK_NATIVE_SHCACHE_MIN", "4"}, {"DM_VK_NATIVE_SHCACHE_VERIFY", "1"}, {NULL, NULL}
    static const char *const e_truth[][2] = {{"DM_VK_NATIVE_SHCACHE", "0"}, {"DM_VK_NATIVE_SHFAR", "0"}, A5_BASE};
    static const char *const e_on[][2] = {{"DM_VK_NATIVE_SHCACHE", "1"}, {"DM_VK_NATIVE_SHFAR", "0"}, A5_BASE};
    static const char *const e_verify[][2] = {{"DM_VK_NATIVE_SHCACHE", "2"}, {"DM_VK_NATIVE_SHFAR", "0"}, A5_BASE};
    static const char *const e_poison[][2] = {{"DM_VK_NATIVE_SHCACHE", "3"}, {"DM_VK_NATIVE_SHFAR", "0"}, A5_BASE};
    static const char *const e_far1[][2] = {{"DM_VK_NATIVE_SHCACHE", "0"}, {"DM_VK_NATIVE_SHFAR", "1"}, A5_BASE};
    static const char *const e_far2[][2] = {{"DM_VK_NATIVE_SHCACHE", "1"}, {"DM_VK_NATIVE_SHFAR", "2"}, A5_BASE};
    static const char *const e_far1c[][2] = {{"DM_VK_NATIVE_SHCACHE", "1"}, {"DM_VK_NATIVE_SHFAR", "1"}, A5_BASE};
#undef A5_BASE
    static a5_res T, Tb, C1, C2, C2b, C3, C4, C5, C6;
    CHECK(mkshader(H(80, 1), "v300000000000001", "f300000000000001", NULL) != NULL); CHECK(mkshader(H(81, 1), "v300000000000001", "f300000000000001", NULL) != NULL);
    g_a5.behind = 0; g_a5.hold = 0; a5_child(e_truth, a5_shc, &T);
    g_a5.behind = 1; a5_child(e_truth, a5_shc, &Tb);
    g_a5.behind = 0; a5_child(e_on, a5_shc, &C1);
    g_a5.hold = 1; a5_child(e_verify, a5_shc, &C2);
    g_a5.behind = 1; a5_child(e_verify, a5_shc, &C2b);
    g_a5.behind = 0; g_a5.hold = 0; a5_child(e_poison, a5_shc, &C3);
    a5_child(e_far1, a5_shc, &C4);
    a5_child(e_far2, a5_shc, &C5);
    a5_child(e_far1c, a5_shc, &C6);
    if (!T.ran || !C1.ran) return;
    // the model draws something everywhere (else equality would prove nothing)
    CHECK(T.model_draws >= (uint64_t)A5_F * 3 * A5_N);
    { int differ = 0; for (int f = 1; f < A5_F; f++) differ += T.h[f] != T.h[f - 1]; CHECK(differ >= A5_F - 2); }   // the moving casters change every frame
    for (int k = 0; k < 3; k++) CHECK(T.th[5][k] != T.th[5][(k + 1) % 3]);
    CHECK(!T.far_clear[5]);
    // mode 1: every frame as the truth (before the write behind the buffer module)
    int bad = 0;
    for (int f = 0; f < A5_BEHIND; f++) if (C1.h[f] != T.h[f]) { bad++; if (bad <= 8) printf("  shc mode 1: frame %d atlas differs from the truth (tiles %d %d %d)\n", f, C1.th[f][0] != T.th[f][0], C1.th[f][1] != T.th[f][1], C1.th[f][2] != T.th[f][2]); }
    CHECK_EQ(bad, 0);
    printf("  shc mode 1 per frame (skipped / copies / replayed / built / repaired / put back / precopied):");
    for (int f = 0; f < A5_F; f++) printf(" %d:%u/%u/%u/%u/%u/%u/%u", f, C1.sk[f], C1.cp[f], C1.rp[f], C1.bo[f], C1.rc[f], C1.pu[f], C1.pc[f]);
    printf("\n");
    CHECK_EQ(C1.bo[2], 3);                                   // frame 2: every cascade builds its layer (stable 3)
    for (int f = 3; f <= 7; f++) { CHECK_EQ(C1.sk[f], 3 * A5_N); CHECK_EQ(C1.rp[f], 0); CHECK_EQ(C1.rc[f], 0); }   // pure hits: members skipped, nothing replayed
    for (int f = 4; f <= 7; f++) { CHECK_EQ(C1.pc[f], 3); CHECK_EQ(C1.cp[f], 3); }   // precopy: the three tiles copied once, at the first cascade
    CHECK_EQ(C1.rc[8], 1); CHECK_EQ(C1.sk[8], 3 * A5_N - 1);   // the missing member: cascade 1 repaired (tile cleared + its kept draws again)
    CHECK(C1.rp[8] >= A5_N - 1 + A5_DYN);
    CHECK_EQ(C1.pu[17], 1);                                  // the skipped cascade's precopied tile put back
    CHECK(C1.rc[11] >= 1); CHECK(C1.rc[14] >= 1);            // the vertex write / the texture upload: the old member did not come
    CHECK(C1.rc[35] >= 3);                                   // the caster shader re-created: no member matches
    CHECK_EQ(C1.sk[41], 3 * A5_N);                           // hits again on the new atlas
    CHECK_EQ(C1.st.atlas_changes, 2);
    CHECK_EQ(C1.st.replay_drop, 0); CHECK_EQ(C1.st.replay_stale, 0); CHECK_EQ(C1.st.ubo_unrestored, 0); CHECK_EQ(C1.st.build_fail, 0);
    CHECK_EQ(C1.st.recover_bad, 0);
    CHECK(C1.st.recover_lost >= 1);                          // frame 23: the read inside cascade 1 came before the repair could (red line)
    CHECK(C1.st.tranges >= 2);                               // frames 20 (clear), 26 (GREATER), 29 (stencil)
    CHECK(C1.st.tw[8 /* NRSHC_T_ORDER */] >= 2);              // the GREATER and the stencil draw (nrs_why)
    printf("  shc mode 1: recover %llu (repaired %llu, cleared %llu, lost %llu), tainted ranges %llu, precopied %llu (hit %llu, put back %llu, lost %llu), skipped %llu of %llu\n",
           (unsigned long long)C1.st.recover, (unsigned long long)C1.st.recover_now, (unsigned long long)C1.st.recover_cleared, (unsigned long long)C1.st.recover_lost,
           (unsigned long long)C1.st.tranges, (unsigned long long)C1.st.precopied, (unsigned long long)C1.st.pre_hit, (unsigned long long)C1.st.pre_undone,
           (unsigned long long)C1.st.pre_lost, (unsigned long long)C1.st.skipped, (unsigned long long)C1.st.draws);
    { int after = 0; for (int f = A5_BEHIND; f < A5_F; f++) after += C1.h[f] != T.h[f];   // (without the write in this child: still the truth)
      CHECK_EQ(after, 0); }
    // mode 2 (verify, 3 recordings in flight): drawn as the engine asks = the truth; would-be hits compared equal; a real difference
    // (bytes written behind the buffer module) is caught and counted
    if (C2.ran) {
        bad = 0; for (int f = 0; f < A5_F; f++) bad += C2.h[f] != T.h[f];
        CHECK_EQ(bad, 0); CHECK(C2.st.verify_eq > 0); CHECK_EQ(C2.st.verify_ne, 0); CHECK_EQ(C2.st.skipped > 0, 1);
        printf("  shc mode 2: verify equal %llu differ %llu (runs %llu, skipped %llu), would skip %llu\n", (unsigned long long)C2.st.verify_eq,
               (unsigned long long)C2.st.verify_ne, (unsigned long long)C2.st.verify_runs, (unsigned long long)C2.st.verify_skip, (unsigned long long)C2.st.skipped);
    }
    if (C2b.ran && Tb.ran) {
        bad = 0; for (int f = 0; f < A5_F; f++) bad += C2b.h[f] != Tb.h[f];
        CHECK_EQ(bad, 0); CHECK(C2b.st.verify_ne >= 1);
        printf("  shc mode 2 with a write behind the buffer module: verify equal %llu differ %llu\n", (unsigned long long)C2b.st.verify_eq, (unsigned long long)C2b.st.verify_ne);
    }
    // mode 3 (poison): a hit keeps the cleared tile - the static shadows vanish on hit frames, frames 0-2 (no hit yet) are the truth
    if (C3.ran) {
        for (int f = 0; f <= 2; f++) CHECK(C3.h[f] == T.h[f]);
        for (int f = 4; f <= 7; f++) { CHECK(C3.th[f][0] != T.th[f][0]); CHECK(C3.th[f][1] != T.th[f][1]); CHECK(C3.th[f][2] != T.th[f][2]); }
        CHECK_EQ(C3.st.copies, 0);
    }
    // SHFAR 1: the far cascade is not drawn (its tile keeps the clear value), the other two are the truth; 2: counted only
    if (C4.ran) {
        int clr = 0, eq = 0; for (int f = 0; f < A5_F; f++) { clr += C4.far_clear[f]; eq += C4.th[f][0] == T.th[f][0] && C4.th[f][1] == T.th[f][1]; }
        CHECK_EQ(clr, A5_F); CHECK_EQ(eq, A5_F);
        CHECK(C4.st.far_draws > 0); CHECK_EQ(C4.st.far_skipped, C4.st.far_draws); CHECK_EQ(C4.st.armed, 0);
    }
    if (C5.ran) {
        bad = 0; for (int f = 0; f < A5_BEHIND; f++) bad += C5.h[f] != T.h[f];
        CHECK_EQ(bad, 0); CHECK(C5.st.far_draws > 0); CHECK_EQ(C5.st.far_skipped, 0); CHECK(C5.st.skipped > 0);
    }
    if (C6.ran && C4.ran) {   // SHFAR 1 with the cache on: the truth of SHFAR 1 alone
        bad = 0; for (int f = 0; f < A5_BEHIND; f++) bad += C6.h[f] != C4.h[f];
        CHECK_EQ(bad, 0); CHECK(C6.st.far_skipped > 0); CHECK(C6.st.skipped > 0);
    }
}

// ---- fx: the draw-effect chain beyond the lead's block (two functions of this test, orders 110 / 120: after "test" 100)
static int g_a5fx, g_a5fx_tr[8], g_a5fx_nt; static vkn_fx_ctx g_a5fx_ctx; static uint16_t g_a5fx_ds_seen; static vkn_fx_ubo g_a5fx_ubo;
static void a5fx_a(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)in; (void)user;
    if (!g_a5fx) return;
    if (g_a5fx_nt < 8) g_a5fx_tr[g_a5fx_nt++] = 1;
    g_a5fx_ctx = *ctx;
    switch (g_a5fx) {
    case 1: out->ds = g_fxds; break;                                   // the next function sees it
    case 2: out->skip = 1; break;                                      // ends the chain
    case 3: {   // a piece's own draw into other targets, then the engine's target set put back
        uint32_t n = 0, lay[VKN_PASS_COLORS], mip[VKN_PASS_COLORS]; vkn_tex *col[VKN_PASS_COLORS], *dep = NULL;
        vkn_pass_cur_get(&n, col, lay, mip, &dep);
        targets(g_b2, NULL, g_d3);
        vkn_fx_bypass(1);
        vkn_draw_in o = g_in; o.vb_off[0] = 48;
        CHECK_EQ(vkn_draw_indexed(&o, 9, 6, 1), 0);
        vkn_fx_bypass(0);
        vkn_pass_targets(n, col, lay, mip, dep);
        break;
    }
    case 5: out->shader = g_fxsh; out->ubo[1] = g_a5fx_ubo; out->tex[2].view = g_t0->view; out->tex[2].tex = g_t0; break;
    case 6: out->cmp_mask[1] = 1u << 3; break;                         // fragment slot 3 (sampler-pool id 13) compares
    default: break;
    }
}
static void a5fx_b(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)in; (void)ctx; (void)user;
    if (!g_a5fx) return;
    if (g_a5fx_nt < 8) g_a5fx_tr[g_a5fx_nt++] = 2;
    g_a5fx_ds_seen = out->ds;
}
// Test doubles of the named-bundle piece (vkn_shlib_named.cpp is not in this program): vkn_shader.c takes them instead of its weak
// externs. Entry 0 = the fragment key g_nm_key (Dref binding mask g_nm_dref), entry 1 = g_nm_key2; the verified pass = one colour
// target of NVN format g_nm_fmt, 1 sample; the translated twins = the bundles g_nm_alt / g_nm_alt2.
static const char *g_nm_key, *g_nm_key2, *g_nm_alt, *g_nm_alt2; static uint64_t g_nm_dref; static uint32_t g_nm_fmt;
static int g_nm_pass_calls, g_nm_pass_last_n, g_nm_alt_calls;
int vkn_shlib_named_entry(const char *fs_key, uint64_t *dref_mask) {
    if (dref_mask) *dref_mask = 0;
    if (!fs_key || !g_nm_key) return -1;
    if (!strcmp(fs_key, g_nm_key)) { if (dref_mask) *dref_mask = g_nm_dref; return 0; }
    if (g_nm_key2 && !strcmp(fs_key, g_nm_key2)) return 1;
    return -1;
}
int vkn_shlib_named_pass(int entry, int ncolor, const uint32_t *fmt, uint32_t depth_fmt, uint32_t samples, uint32_t w, uint32_t h) {
    (void)entry; (void)depth_fmt; (void)w; (void)h;
    g_nm_pass_calls++; g_nm_pass_last_n = ncolor;
    if (ncolor < 0) return 0;   // (as the real one: the pass is not known off the recording thread)
    return !(ncolor == 1 && fmt && fmt[0] == g_nm_fmt && samples == 1);
}
int vkn_shlib_named_alt(const char *fs_key, xbg_vkn_stage *out) {
    g_nm_alt_calls++;
    const char *k = g_nm_key && fs_key && !strcmp(fs_key, g_nm_key) ? g_nm_alt : g_nm_alt2;
    return k ? vkn_shlib_shader(k, NULL, NULL, out) : 1;
}
// ... and of the warm-up (vkn_warm.c is not in this program): what the draw path offers for the next run's list
static int g_wn_calls, g_wn_fx_listable; static uint8_t g_wn_last_fx; static const void *g_wn_last_shader;
void vkn_warm_note(const vkn_pipe_desc *d, const vkn_pipe_src *src) {
    g_wn_calls++; g_wn_last_fx = d->fx; g_wn_last_shader = src->shader;
    if (d->fx && src->shader) g_wn_fx_listable++;   // rec_make lists every build with a shader object: an effect variant must not have one
}
int vkn_warm_take(const vkn_pipe_key *k, const vkn_pipe_src *src, VkPipeline *out) { (void)k; (void)src; (void)out; return 0; }
static void *a5_off_thread(void *arg) { const vkn_shader *s = (const vkn_shader *)arg; return (void *)vkn_shader_module(s, VKN_STAGE_FS, 0, 0.0f, 0); }
static const mk_ev *a5_last(uint64_t from, VkCommandBuffer cb, int op) { const mk_ev *r = NULL; for (int k = 0; nth(from, cb, op, k); k++) r = nth(from, cb, op, k); return r; }

static void test_a5_fx(void) {
    if (!g_s1 || !g_fxsh || !g_t0 || !g_a2 || !g_b2 || !g_d3) { CHECK(0); return; }
    const int v0 = mk_val_errors, e0 = mk_errors;
    CHECK_EQ(vkn_fx_register("a5-b", a5fx_b, NULL, 120), 0); CHECK_EQ(vkn_fx_register("a5-a", a5fx_a, NULL, 110), 0);   // registered in reverse order
    g_fxm = 0; g_a5fx = 0;
    if (!g_fxds) g_fxds = vkn_fx_ds(1, 0, 2, 0, 0, 0, 0, 0);
    VkCommandBuffer cb = fbegin(1); targets(g_a2, NULL, g_d3);
    vkn_draw_in in = g_in;
    vkn_fx_stats f0, f1; vkn_fx_stats_get(&f0);
    // ---- the chain's order and what a later function sees
    g_a5fx = 1; g_a5fx_nt = 0; g_a5fx_ds_seen = 0;
    uint64_t m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0); CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 1);
    CHECK_EQ(g_a5fx_nt, 2); CHECK_EQ(g_a5fx_tr[0], 1); CHECK_EQ(g_a5fx_tr[1], 2); CHECK_EQ(g_a5fx_ds_seen, g_fxds);
    g_a5fx = 2; g_a5fx_nt = 0;   // a skip ends the chain: the later function is not called, nothing recorded
    m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0); NO_OPS(m, cb); CHECK_EQ(g_a5fx_nt, 1);
    vkn_fx_stats_get(&f1); CHECK_EQ(f1.skipped, f0.skipped + 1); CHECK_EQ(f1.ds_swaps, f0.ds_swaps + 1);
    // ---- a function that draws on its own (bypass) into other targets and puts the engine's set back: the engine's arrays draw keeps
    // its own pass and firstVertex (the vertex fold of its own arguments)
    g_a5fx = 0; in.vb_off[0] = 32;
    m = mark(); CHECK_EQ(vkn_draw_arrays(&in, 5, 3), 0);
    const mk_ev *d0 = nth(m, cb, MK_E_DRAW, 0); const uint32_t fv0 = d0 ? d0->u[2] : ~0u;
    targets(g_b2, NULL, g_d3); vkn_draw_indexed(&in, 0, 3, 1); targets(g_a2, NULL, g_d3);   // (another pass in between: the next one begins anew)
    vkn_fx_stats_get(&f0);
    g_a5fx = 3;
    m = mark(); CHECK_EQ(vkn_draw_arrays(&in, 5, 3), 0);
    CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 1); CHECK_EQ(cnt(m, cb, MK_E_DRAW), 1);
    {
        const mk_ev *dn = nth(m, cb, MK_E_DRAW_IDX, 0), *dd = nth(m, cb, MK_E_DRAW, 0), *r1 = nth(m, cb, MK_E_BEGIN_RP, 0), *r2 = nth(m, cb, MK_E_BEGIN_RP, 1);
        CHECK(dn && dd && r1 && r2 && r1->seq < dn->seq && dn->seq < r2->seq && r2->seq < dd->seq);   // own draw in its pass, the engine's in a new one
        if (dd) CHECK_EQ(dd->u[2], fv0);                                                            // the engine's firstVertex as without the own draw
        const mk_obj *fb2 = fb_of(r2); CHECK(fb2 && fb2->fb_n >= 1 && vobj(fb2->fb_view[0]) && vobj(fb2->fb_view[0])->vci.image == g_a2->image);
    }
    CHECK(vkn_pass_color(0) == g_a2); CHECK(vkn_pass_depth() == g_d3);
    vkn_fx_stats_get(&f1); CHECK_EQ(f1.bypassed, f0.bypassed + 1); CHECK_EQ(f1.calls, f0.calls + 1);   // the own draw did not enter the chain
    in.vb_off[0] = 16;
    // ---- skip of a rewritten primitive: no transient indices are made for it; the context names the rewritten topology and the
    // engine's count
    {
        vkn_draw_rw_stats r0, r1; vkn_draw_in fan = in; fan.prim = 2; vkn_draw_in quad = in; quad.prim = 4;
        g_a5fx = 0; vkn_draw_rw_stats_get(&r0); m = mark(); CHECK_EQ(vkn_draw_indexed(&fan, 0, 5, 1), 0); vkn_draw_rw_stats_get(&r1);
        CHECK(r1.bytes > r0.bytes); CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 1);   // (control: drawn, rewritten)
        g_a5fx = 2;
        vkn_draw_rw_stats_get(&r0); m = mark();
        CHECK_EQ(vkn_draw_indexed(&fan, 0, 5, 1), 0); CHECK_EQ(g_a5fx_ctx.prim, vkn_prim_rewrite(2)); CHECK_EQ(g_a5fx_ctx.count, 5); CHECK_EQ(g_a5fx_ctx.indexed, 1);
        CHECK_EQ(vkn_draw_arrays(&fan, 2, 6), 0); CHECK_EQ(g_a5fx_ctx.indexed, 0); CHECK_EQ(g_a5fx_ctx.first, 2); CHECK_EQ(g_a5fx_ctx.count, 6);
        CHECK_EQ(vkn_draw_indexed(&quad, 0, 8, 1), 0); CHECK_EQ(g_a5fx_ctx.prim, vkn_prim_rewrite(4));
        vkn_draw_rw_stats_get(&r1);
        NO_OPS(m, cb); CHECK_EQ(r1.bytes, r0.bytes); CHECK_EQ(r1.blocks, r0.blocks);
        CHECK_EQ(r1.draws[0][VKN_RW_FAN] + r1.draws[1][VKN_RW_FAN] + r1.draws[1][VKN_RW_QUADS], r0.draws[0][VKN_RW_FAN] + r0.draws[1][VKN_RW_FAN] + r0.draws[1][VKN_RW_QUADS]);
    }
    // ---- a piece's shader with a dynamic effect block: from its own buffer, from the ring, none (the zero block)
    {
        vkn_block own; CHECK_EQ(vkn_block_alloc(VKN_ARENA_STAGING, 256, 256, &own), 0);
        uint32_t roff = 0; CHECK_EQ(vkn_ring_alloc(64, &roff), 0);
        g_a5fx = 5;
        g_a5fx_ubo.buf = own.buf; g_a5fx_ubo.off = own.off; g_a5fx_ubo.size = 64;
        m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
        const mk_ev *bs = a5_last(m, cb, MK_E_BIND_SETS); const mk_dw *w0 = dw_of(bs, 0);
        CHECK(bs && bs->n == 1 && bs->dyn[0] == (uint32_t)own.off); CHECK(w0 && w0->buf == own.buf && w0->off == 0 && w0->range == 64);
        g_a5fx_ubo.buf = VK_NULL_HANDLE; g_a5fx_ubo.off = roff; g_a5fx_ubo.size = 64;
        m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
        bs = a5_last(m, cb, MK_E_BIND_SETS); w0 = dw_of(bs, 0);
        CHECK(bs && bs->dyn[0] == roff); CHECK(w0 && w0->buf == vkn_ring_buffer() && w0->off == vkn_ring_base() && w0->range == 64);
        vkn_draw_stats2 z0, z1; vkn_draw_stats2_get(&z0);
        g_a5fx_ubo.size = 0;
        m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0); CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 1);
        vkn_draw_stats2_get(&z1); CHECK(z1.zero_ubo > z0.zero_ubo);
        bs = a5_last(m, cb, MK_E_BIND_SETS); CHECK(bs && bs->dyn[0] == 0);
        g_a5fx = 0;
        vkn_retire_block(&own);
    }
    // ---- comparison twins (vkn_tex.c): by the chain's mask; cached; retired with their sampler; a game sampler that compares is its own
    {
        uint64_t mk0 = 0, gc0 = 0, fl0 = 0, mk1 = 0, gc1 = 0, fl1 = 0; uint32_t al0 = 0, al1 = 0;
        vkn_sampler_cmp_stats(&mk0, &gc0, &fl0, &al0);
        g_a5fx = 6; m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
        const VkSampler tw = vkn_sampler_cmp_by_id(13);
        const mk_dw *w16 = dw_of(a5_last(m, cb, MK_E_BIND_SETS), 16), *w22 = dw_of(a5_last(m, cb, MK_E_BIND_SETS), 22);
        CHECK(tw && tw != vkn_sampler_by_id(13)); CHECK(w16 && w16->smp == tw); CHECK(w22 && w22->smp == vkn_sampler_by_id(10));
        CHECK(vkn_sampler_cmp_by_id(13) == tw);   // cached
        vkn_sampler_cmp_stats(&mk1, &gc1, &fl1, &al1); CHECK_EQ(mk1, mk0 + 1); CHECK_EQ(al1, al0 + 1);
        { mk_obj *o = mk_obj_of(VH(tw)); CHECK(o && o->sci.compareEnable && o->sci.compareOp == VK_COMPARE_OP_LESS && o->sci.minFilter == VK_FILTER_LINEAR && o->sci.magFilter == VK_FILTER_LINEAR);
          mk_obj *b = mk_obj_of(VH(vkn_sampler_by_id(13))); CHECK(o && b && o->sci.addressModeU == b->sci.addressModeU && o->sci.addressModeV == b->sci.addressModeV && !b->sci.compareEnable); }
        g_a5fx = 0; m = mark(); vkn_draw_indexed(&in, 0, 3, 1);
        { const mk_ev *bs = a5_last(m, cb, MK_E_BIND_SETS); if (bs) { const mk_dw *w = dw_of(bs, 16); CHECK(w && w->smp == vkn_sampler_by_id(13)); } }   // no mask: the engine's sampler
        fsubmit(0);
        // the engine replaces sampler 13: its twin is retired (destroyed after the GPU is done), a new one is made on demand
        CHECK_EQ(vkn_sampler_create(H(61, 1), 13, 0, 1, 1, 1, 1, 0), 0);
        vkn_sampler_cmp_stats(&mk1, &gc1, &fl1, &al1); CHECK_EQ(al1, al0);
        frames_n(4); CHECK(!mk_is_live(VH(tw), MK_SAMPLER));
        const VkSampler tw2 = vkn_sampler_cmp_by_id(13); CHECK(tw2 && tw2 != tw && mk_is_live(VH(tw2), MK_SAMPLER));
        // a comparing game sampler: kept as it is (not a second object), and nothing of ours dies with it
        CHECK_EQ(vkn_sampler_create(H(63, 1), 14, 1, 1, 1, 1, 1, 3), 0);
        vkn_sampler_cmp_stats(&mk0, &gc0, &fl0, &al0);
        CHECK(vkn_sampler_cmp_by_id(14) == vkn_sampler_by_id(14)); CHECK(vkn_sampler_cmp_by_id(14) == vkn_sampler_by_id(14));
        vkn_sampler_cmp_stats(&mk1, &gc1, &fl1, &al1); CHECK_EQ(gc1, gc0 + 1); CHECK_EQ(mk1, mk0); CHECK_EQ(al1, al0);
        CHECK_EQ(vkn_sampler_destroy(H(63, 1)), 1);
        CHECK(vkn_sampler_cmp_by_id(14) == VK_NULL_HANDLE);   // gone with the engine's sampler (counted as failed)
        CHECK(vkn_sampler_cmp_by_id(9999) == VK_NULL_HANDLE);
        vkn_sampler_cmp_stats(&mk0, &gc0, &fl0, &al0); CHECK_EQ(fl0, fl1 + 1);
        frames_n(4);
    }
    // ---- named fragment modules (vkn_shader.c with the test doubles above): a Dref binding gets the twin without any effect function;
    // the verified pass keeps the named module, another pass takes the translated twin (made once), a twin that reads more of a block
    // than the named table binds is refused (the named module stays), off the recording thread the pass is unknown (named module)
    {
        const VkDescriptorType UB = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        xbg_vkn_stage *nf = mk_bundle_add("f3000000000000a5", 4), *nt = mk_bundle_add("f3000000000000b5", 4), *nf2 = mk_bundle_add("f3000000000000a6", 4), *nt2 = mk_bundle_add("f3000000000000b6", 4);
        xbg_vkn_stage *L[4] = {nf, nt, nf2, nt2};
        for (int i = 0; i < 4; i++) { mk_bind(L[i], 0, UB, XBG_SOURCE_SUPPORT, 0, 1392); mk_bind(L[i], 1, UB, XBG_SOURCE_UNIFORM, 0, i == 3 ? 128 : 64); mk_bind(L[i], 3, CIS, XBG_SOURCE_TEXTURE, 0, 0); mk_bind(L[i], 2, CIS, XBG_SOURCE_TEXTURE, 3, 0); }
        g_nm_key = "f3000000000000a5"; g_nm_alt = "f3000000000000b5"; g_nm_key2 = "f3000000000000a6"; g_nm_alt2 = "f3000000000000b6";
        g_nm_dref = 1ull << 16; g_nm_fmt = g_a2->nvn_fmt;   // binding 16 = fragment texture slot 3
        const vkn_shader *ns = mkshader(H(85, 1), "v300000000000001", "f3000000000000a5", NULL), *ns2 = mkshader(H(86, 1), "v300000000000001", "f3000000000000a6", NULL);
        CHECK(ns && ns2);
        if (ns && ns2) {
            int flagged = 0, other = 0;
            for (uint32_t i = 0; i < ns->nbind; i++) { if (ns->bind[i].flags & VKN_BF_CMP) { flagged++; CHECK(ns->bind[i].kind == VKN_BK_TEXTURE && ns->bind[i].slot == 3); } }
            for (uint32_t i = 0; i < ns2->nbind; i++) other += (ns2->bind[i].flags & VKN_BF_CMP) != 0;
            CHECK_EQ(flagged, 1); CHECK_EQ(other, 0);
            cb = fbegin(1); targets(g_a2, NULL, g_d3);
            vkn_draw_in ni = g_in; ni.shader = H(85, 1);
            g_nm_pass_calls = 0; g_nm_alt_calls = 0;
            const uint64_t m0 = mark();   // (a set is bound only when it changes: the binding checks read the last bind since here)
            m = mark(); CHECK_EQ(vkn_draw_indexed(&ni, 0, 3, 1), 0);
            mk_obj *po = pipe_of(a5_last(m, cb, MK_E_BIND_PIPE));
            CHECK(po && po->gp.nst == 2 && po->gp.module[1] == nf->module); CHECK_EQ(g_nm_pass_calls, 1); CHECK_EQ(g_nm_pass_last_n, 1);
            { const mk_dw *w = dw_of(a5_last(m, cb, MK_E_BIND_SETS), 16); CHECK(w && w->smp == vkn_sampler_cmp_by_id(13)); }   // Dref: the twin, no function asked
            { const mk_dw *w = dw_of(a5_last(m, cb, MK_E_BIND_SETS), 22); CHECK(w && w->smp == vkn_sampler_by_id(10)); }
            m = mark(); vkn_draw_indexed(&ni, 0, 3, 1); CHECK_EQ(g_nm_pass_calls, 1);   // decided once per (shader, pass): the pipeline is cached
            targets(g_a2, g_b2, g_d3);   // two colours: outside the verified pass
            m = mark(); CHECK_EQ(vkn_draw_indexed(&ni, 0, 3, 1), 0);
            po = pipe_of(a5_last(m, cb, MK_E_BIND_PIPE)); CHECK(po && po->gp.module[1] == nt->module); CHECK_EQ(g_nm_alt_calls, 1);
            { const mk_dw *w = dw_of(a5_last(m0, cb, MK_E_BIND_SETS), 16); CHECK(w && w->smp == vkn_sampler_cmp_by_id(13)); }   // the flag is the binding's
            targets(g_b2, NULL, NULL);   // another outside pass (no depth): the twin module is made once
            m = mark(); CHECK_EQ(vkn_draw_indexed(&ni, 0, 3, 1), 0); po = pipe_of(a5_last(m, cb, MK_E_BIND_PIPE));
            if (g_b2->nvn_fmt != g_nm_fmt) CHECK(po && po->gp.module[1] == nt->module);
            CHECK_EQ(g_nm_alt_calls, 1);
            // a translated twin reading 128 bytes of a block the named table binds 64 of: refused, the named module stays outside too
            vkn_draw_in n2 = g_in; n2.shader = H(86, 1); targets(g_a2, g_b2, g_d3);
            m = mark(); CHECK_EQ(vkn_draw_indexed(&n2, 0, 3, 1), 0); po = pipe_of(a5_last(m, cb, MK_E_BIND_PIPE));
            CHECK(po && po->gp.module[1] == nf2->module); CHECK_EQ(g_nm_alt_calls, 2);
            m = mark(); vkn_draw_indexed(&n2, 0, 3, 1); CHECK_EQ(g_nm_alt_calls, 2);   // (refused once, remembered)
            // off the recording thread (the warm-up worker): the pass is unknown, the named module
            pthread_t th; void *mod = NULL; g_nm_pass_last_n = 0;
            CHECK_EQ(pthread_create(&th, NULL, a5_off_thread, (void *)ns), 0); pthread_join(th, &mod);
            CHECK(mod == (void *)nf->module); CHECK_EQ(g_nm_pass_last_n, -1);
            // ---- effect variants are never listed for the warm-up: the draw path builds them without a shader object
            targets(g_a2, NULL, g_d3);
            g_wn_calls = 0; g_wn_fx_listable = 0;
            g_fxm = 2; m = mark(); CHECK_EQ(vkn_draw_indexed(&ni, 0, 3, 1), 0);    // discard (a new pipeline for this shader)
            CHECK(g_wn_calls >= 1); CHECK_EQ(g_wn_last_fx, VKN_PIPE_FX_DISCARD); CHECK(g_wn_last_shader == NULL);
            g_fxm = 4; CHECK_EQ(vkn_draw_indexed(&ni, 0, 3, 1), 0);               // no fragment stage
            CHECK_EQ(g_wn_last_fx, VKN_PIPE_FX_NO_FS); CHECK(g_wn_last_shader == NULL);
            g_fxm = 0; vkn_draw_in n3 = ni; n3.blend = H(41, 1); CHECK_EQ(vkn_draw_indexed(&n3, 0, 3, 1), 0);   // the engine's own build: listed
            CHECK_EQ(g_wn_last_fx, 0); CHECK(g_wn_last_shader != NULL); CHECK_EQ(g_wn_fx_listable, 0);
            fsubmit(0);
        }
        g_nm_key = g_nm_key2 = NULL;
    }
    // ---- pipeline keys carry the effect bits through pack / unpack
    {
        vkn_pipe_desc d; memset(&d, 0, sizeof d); d.shader = g_s1->id; d.blend = 3; d.ds = 2; d.raster = 1; d.vfmt = 1; d.prim = 1; d.pass = 1; d.write_mask[0] = 0xf;
        vkn_pipe_key k[4];
        for (int fx = 0; fx < 4; fx++) {
            d.fx = (uint8_t)fx; k[fx] = vkn_pipe_key_pack(&d);
            vkn_pipe_desc u; vkn_pipe_key_unpack(&k[fx], &u);
            CHECK_EQ(u.fx, fx); CHECK_EQ(u.shader, d.shader); CHECK_EQ(u.ds, d.ds); CHECK_EQ(u.pass, d.pass); CHECK_EQ(u.prim, d.prim);
        }
        for (int a = 0; a < 4; a++) for (int b = a + 1; b < 4; b++) CHECK(memcmp(&k[a], &k[b], sizeof k[a]) != 0);
        d.fx = 0xff; k[0] = vkn_pipe_key_pack(&d); vkn_pipe_desc u; vkn_pipe_key_unpack(&k[0], &u); CHECK_EQ(u.fx, VKN_PIPE_FX_MASK);   // only the defined bits
    }
    // ---- an own draw of a piece inside the chain of a CONDITIONAL engine draw (COND=1, a hidden pair): the engine's draw keeps its predicate
    {
        mk_cond_ext = 1; vkn_query_init(VKN_COND_ON);
        cb = fbegin(1); targets(g_a2, NULL, g_d3);
        mk_draw_samples = 0; vkn_query_begin(0x5a500); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0); vkn_query_end(0x5a500);
        targets(g_b2, NULL, g_d3);   // the pass end copies the verdict
        mk_draw_samples = 1;
        g_a5fx = 0; vkn_query_cond(0x5a500); m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
        CHECK_EQ(cnt(m, cb, MK_E_BEGIN_COND), 1);   // (control: predicated)
        g_a5fx = 3; vkn_query_cond(0x5a500); m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
        const mk_ev *dd = a5_last(m, cb, MK_E_DRAW_IDX), *bc = a5_last(m, cb, MK_E_BEGIN_COND);
#if 1   // (was A5_FIXME_NESTED_PREDICATE: fixed in vkn_draw.c by the nesting counter g_fx_nest)
        // FIXME (vkn_draw.c, reported): the own draw runs vkn_query_draw_enter, which clears the predicate the engine's draw was given
        CHECK(dd && bc && bc->seq < dd->seq && dd->u[7] == 1);
#else
        if (!(dd && bc && bc->seq < dd->seq)) printf("  KNOWN (vkn_draw.c, reported; check off until fixed: -DA5_FIXME_NESTED_PREDICATE): an own draw of a piece inside a conditional draw's chain drops that draw's predicate\n");
#endif
        g_a5fx = 0; fsubmit(0);
        vkn_query_init(VKN_COND_OFF); mk_cond_ext = 0;
    }
    CHECK_EQ(mk_val_errors, v0); CHECK_EQ(mk_errors, e0);
}

// ---- oracle: DM_VK_NATIVE_COND=4 over 136 frames. 7 objects a frame: a report pair around the object's pre-pass draw (objects 4 and 5
// are the same draw: a repeated mesh, told apart by the occurrence number), object 6 an empty pair; then one conditional draw per object in
// the next pass. The pair addresses move every frame (the engine's query objects name other objects each frame): the object keys must not.
// Objects 1, 3, 5 are hidden; at frame 70 object 0 hides and object 1 shows (seen by the oracle of frame 128 only: INEXACT by design).
enum { A5_OF = 2 * VKN_ORACLE_PERIOD + 8 };
static void a5_or_frame(int f, const int vis[7], int *q, int *d, int *cond) {
    VkCommandBuffer cb = fbegin(1); targets(g_a2, NULL, g_d3);
    vkn_draw_in in = g_in;
    const uint64_t base = 0x5c0000 + (uint64_t)(f & 7) * 0x1000, m = mark();
    for (int i = 0; i < 6; i++) {
        const int obj = i == 5 ? 4 : i;
        vkn_query_begin(base + 16u * i); mk_draw_samples = vis[i] ? 5 : 0;
        in.vb_off[0] = 16u * (uint32_t)obj; CHECK_EQ(vkn_draw_indexed(&in, 3u * (uint32_t)obj, 3, 1), 0);
        vkn_query_end(base + 16u * i);
    }
    vkn_query_begin(base + 16u * 6); vkn_query_end(base + 16u * 6);   // an empty pair
    mk_draw_samples = 1; in.vb_off[0] = 16;
    targets(g_b2, NULL, g_d3);
    const uint64_t m2 = mark();
    for (int i = 0; i < 7; i++) { vkn_query_cond(base + 16u * i); CHECK_EQ(vkn_draw_indexed(&in, 3u * (uint32_t)(10 + i), 3, 1), 0); }
    *q = cnt(m, cb, MK_E_BEGIN_Q); *d = cnt(m, cb, MK_E_DRAW_IDX); *cond = cnt(m2, cb, MK_E_BEGIN_COND);
    fsubmit(0);
}
static void test_a5_oracle(void) {
    if (!g_s1 || !g_a2 || !g_b2 || !g_d3) { CHECK(0); return; }
    const int v0 = mk_val_errors, e0 = mk_errors;
    mk_cond_ext = 1; vkn_query_init(VKN_COND_ORACLE); CHECK_EQ(vkn_query_mode(), VKN_COND_ORACLE);
    int vis[7] = {1, 0, 1, 0, 1, 0, 1}, q[A5_OF], d[A5_OF], c[A5_OF];
    vkn_query_occ_stats o0, o1, oa; vkn_query_occ_stats_get(&o0);
    int reads_at[4] = {-1, -1, -1, -1}, nreads = 0;
    vkn_query_occ_stats ob[4];
    for (int f = 0; f < A5_OF; f++) {
        if (f == 70) { vis[0] = 0; vis[1] = 1; }
        vkn_query_occ_stats_get(&oa);
        a5_or_frame(f, vis, &q[f], &d[f], &c[f]);
        vkn_query_occ_stats_get(&o1);
        if (o1.read_frames != oa.read_frames && nreads < 4) { reads_at[nreads] = f; ob[nreads] = o1; nreads++; }
    }
    printf("  oracle per frame (queries / draws):"); for (int f = 0; f < A5_OF; f++) if (f < 8 || (f >= 62 && f <= 72) || f >= 126) printf(" %d:%d/%d", f, q[f], d[f]); printf("; verdicts read at frames");
    for (int i = 0; i < nreads; i++) printf(" %d", reads_at[i]); printf("\n");
    // oracle recordings: a query on every pair that drew, everything drawn (the empty pair's conditional draw too: mode 2's counting)
    for (int f = 0; f < A5_OF; f += VKN_ORACLE_PERIOD) { CHECK_EQ(q[f], 6); CHECK_EQ(d[f], 13); CHECK_EQ(c[f], 0); }
    // between them: no query at all; before the first verdict everything but the empty pair's conditional draw; after it, the hidden
    // objects' pre-pass and conditional draws are skipped on the CPU
    CHECK_EQ(nreads, 3); const int r0 = reads_at[0];   // each oracle's verdicts once it retired (3 recordings later), never an older slot's
    CHECK(r0 >= 1 && r0 <= 4); for (int i = 1; i < nreads; i++) CHECK_EQ(reads_at[i], r0 + i * VKN_ORACLE_PERIOD);
    for (int f = 1; f < A5_OF; f++) {
        if (f % VKN_ORACLE_PERIOD == 0) continue;
        CHECK_EQ(q[f], 0); CHECK_EQ(c[f], 0);
        CHECK_EQ(d[f], f < r0 ? 12 : 6);
    }
    vkn_query_occ_stats_get(&o1);
    CHECK_EQ(o1.oracle_frames - o0.oracle_frames, 3); CHECK_EQ(o1.oracle_lost - o0.oracle_lost, 0);
    CHECK_EQ(o1.keyed - o0.keyed, (uint64_t)6 * A5_OF);                    // every pair with a draw, every frame
    CHECK_EQ(o1.repeated - o0.repeated, (uint64_t)A5_OF);                  // object 5 = object 4's draw: occurrence 1
    if (nreads >= 2) {
        CHECK_EQ(ob[0].read_pairs - o0.read_pairs, 6); CHECK_EQ(ob[0].read_hidden - o0.read_hidden, 3);
        CHECK_EQ(ob[1].read_stable - ob[0].read_stable, 6);                // the same 6 keys again although every pair address moved
        CHECK_EQ(ob[1].read_changed - ob[0].read_changed, 0);
    }
    if (nreads >= 3) { CHECK_EQ(ob[2].read_stable - ob[1].read_stable, 6); CHECK_EQ(ob[2].read_changed - ob[1].read_changed, 2); }   // the frame-70 swap
    const uint64_t sk_pre = o1.skip_pre - o0.skip_pre, sk_cond = o1.skip_cond - o0.skip_cond;
    CHECK_EQ(sk_cond - (o1.skip_empty - o0.skip_empty), sk_pre);           // a hidden object: its pre-pass draw and its conditional draw
    // the frame-70 change: hidden object 0 is still drawn and shown object 1 still skipped until the third oracle's read (inexact, by
    // design); from then on object 0 is skipped and object 1 drawn - still 6 draws a frame
    CHECK_EQ(d[75], 6); CHECK_EQ(d[127], 6); CHECK_EQ(d[A5_OF - 1], 6);
    CHECK_EQ(o1.read_frames - o0.read_frames, 3);
    // ---- mode switches at recording boundaries: 4 -> 1 (no stale CPU skip: queries and predicates), 1 -> 4 (the oracle's schedule
    // goes on, no stale slot), 4 -> 0 (everything drawn, no query work), 0 -> 4
    vkn_query_census_occ();
    { int qq, dd, cc; vkn_query_init(VKN_COND_ON); a5_or_frame(1, vis, &qq, &dd, &cc); CHECK_EQ(qq, 6); CHECK_EQ(dd, 12); CHECK_EQ(cc, 6); }   // the empty pair: skipped exactly
    { int qq, dd, cc; vkn_query_init(VKN_COND_ORACLE); a5_or_frame(1, vis, &qq, &dd, &cc); CHECK(dd == 6 || dd == 13); CHECK_EQ(cc, 0); }
    { int qq, dd, cc; vkn_query_init(VKN_COND_OFF); a5_or_frame(1, vis, &qq, &dd, &cc); CHECK_EQ(qq, 0); CHECK_EQ(dd, 13); CHECK_EQ(cc, 0); }
    { int qq, dd, cc; vkn_query_init(VKN_COND_ORACLE); for (int k = 0; k < VKN_ORACLE_PERIOD + 4; k++) a5_or_frame(k, vis, &qq, &dd, &cc); CHECK(dd == 6 || dd == 13); }
    vkn_query_census_occ();
    vkn_query_init(VKN_COND_OFF); mk_cond_ext = 0; mk_draw_samples = 1;
    CHECK_EQ(mk_val_errors, v0); CHECK_EQ(mk_errors, e0);
}
// ================================================================================================================ [stage 5.2 B3] post
// vkn_post.c (DM_VK_NATIVE_POST=4): the pure parts (classes, coverage, the arming rule) and one synthetic tail LUT -> Sight -> FXAA ->
// Vignette -> Combine over the mock: 8 probation frames draw all five, then LUT and Vignette are skipped, Sight draws F1A with the
// LUT's stash, the FXAA step is skipped (its target an alias of Sight's), Combine draws F2 with Vignette's stash (whose input is the
// alias): 2 draws a frame; a LUT without its Sight is drawn from its stash (recovery); an engine copy of the alias image is counted.
static xbg_vkn_stage *g_pf[3]; static nb_fx g_pm[3]; static const char *const g_pk[3] = {"F1.91eb2d97a304709a", "F1A.91eb2d97a304709a", "F2.98186c22a7209861"};
static int post_load(const char *key, struct xbg_vkn_stage *out, void *meta) {
    for (int i = 0; i < 3; i++) if (g_pf[i] && !strcmp(key, g_pk[i])) { *out = *g_pf[i]; memcpy(meta, &g_pm[i], sizeof g_pm[i]); return 0; }
    return 1;
}
static void post_meta(nb_fx *m, const char *host, const char *elided, uint32_t flags) {
    memset(m, 0, sizeof *m); memcpy(m->host_key, host, 16); memcpy(m->elided_key[0], elided, 16); m->n_elided = 1; m->flags = flags;
}
static void post_extra(nb_fx *m, uint32_t vkb, uint32_t type, uint8_t kind, uint32_t slot, uint8_t flags) {
    nb_fx_extra *e = &m->extra[m->n_extra++]; e->vk_binding = vkb; e->vk_type = type; e->size = type == 6 ? 64 : 0; e->src_pass = 1; e->src_stage = 1; e->src_kind = kind; e->src_slot = slot; e->flags = flags;
}
static void test_post(void) {
    // ---- pure parts
    CHECK_EQ(vkn_post_class_key("921728a5bbb1c71b"), VKN_PC_LUT); CHECK_EQ(vkn_post_class_key("91eb2d97a304709a"), VKN_PC_SIGHT);
    CHECK_EQ(vkn_post_class_key("43e8e8ed2b1df6c2"), VKN_PC_VIG); CHECK_EQ(vkn_post_class_key("98186c22a7209861"), VKN_PC_COMB);
    CHECK_EQ(vkn_post_class_key("691768aa00000001"), VKN_PC_FXAA); CHECK_EQ(vkn_post_class_key("7b32101900000001"), VKN_PC_COPY);
    CHECK_EQ(vkn_post_class_key("e94ea226e689801c"), VKN_PC_FOG); CHECK_EQ(vkn_post_class_key("010472595b342188"), VKN_PC_OUTLINE);
    CHECK_EQ(vkn_post_class_key("f300000000000001"), VKN_PC_OTHER); CHECK_EQ(vkn_post_class_key("91eb2d97"), VKN_PC_OTHER); CHECK_EQ(vkn_post_class_key(NULL), VKN_PC_OTHER);
    {
        const float vp[4] = {0, 0, 1620, 729}; const int32_t sc[4] = {0, 0, 4096, 4096}; int32_t r[4];
        vkn_post_cover(vp, sc, 2160, 972, r); CHECK(r[0] == 0 && r[1] == 0 && r[2] == 1620 && r[3] == 729);
        const float vf[4] = {-10, -10, 3000, 3000}; const int32_t s2[4] = {8, 4, 100, 50};
        vkn_post_cover(vf, s2, 64, 32, r); CHECK(r[0] == 8 && r[1] == 4 && r[2] == 64 && r[3] == 32);
        vkn_post_arm a; memset(&a, 0, sizeof a);
        for (uint64_t f = 1; f <= 7; f++) CHECK_EQ(vkn_post_arm_note(&a, f, 1), 0);
        CHECK_EQ(vkn_post_arm_note(&a, 8, 1), 1); CHECK_EQ(vkn_post_arm_note(&a, 8, 1), 1); CHECK_EQ(a.good, 8);   // one verdict a frame
        CHECK_EQ(vkn_post_arm_note(&a, 9, 0), 0); CHECK_EQ(vkn_post_arm_note(&a, 9, 1), 0);                         // a bad frame stops it
        for (uint64_t f = 10; f <= 16; f++) vkn_post_arm_note(&a, f, 1);
        CHECK_EQ(a.armed, 0); vkn_post_arm_note(&a, 18, 1); CHECK_EQ(a.good, 1);                                    // a gap starts over
    }
    // ---- the synthetic tail
    setenv("DM_VK_NATIVE_POST", "4", 1);
    CHECK_EQ(vkn_post_level(), 4); CHECK_EQ(vkn_post_mobile(), 1);
    vkn_post_test_loader(post_load); vkn_post_install(); vkn_post_install();
    g_fxm = 0;
    const VkDescriptorType UB = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    xbg_vkn_stage *vs = mk_bundle_add("a0a0a0a000000001", 0); mk_bind(vs, 0, UB, XBG_SOURCE_SUPPORT, 0, 1392);
    xbg_vkn_stage *lut = mk_bundle_add("921728a5bbb1c71b", 4); mk_bind(lut, 0, CIS, XBG_SOURCE_TEXTURE, 0, 0); mk_bind(lut, 1, CIS, XBG_SOURCE_TEXTURE, 1, 0); mk_bind(lut, 2, UB, XBG_SOURCE_UNIFORM, 5, 64);
    xbg_vkn_stage *sig = mk_bundle_add("91eb2d97a304709a", 4); mk_bind(sig, 0, UB, XBG_SOURCE_SUPPORT, 0, 1392); mk_bind(sig, 1, CIS, XBG_SOURCE_TEXTURE, 0, 0); mk_bind(sig, 2, UB, XBG_SOURCE_UNIFORM, 5, 64);
    xbg_vkn_stage *fxa = mk_bundle_add("691768aa00000001", 4); mk_bind(fxa, 0, CIS, XBG_SOURCE_TEXTURE, 0, 0);
    xbg_vkn_stage *vig = mk_bundle_add("43e8e8ed2b1df6c2", 4); mk_bind(vig, 0, CIS, XBG_SOURCE_TEXTURE, 0, 0); mk_bind(vig, 1, UB, XBG_SOURCE_UNIFORM, 5, 64);
    xbg_vkn_stage *cmb = mk_bundle_add("98186c22a7209861", 4); mk_bind(cmb, 0, CIS, XBG_SOURCE_TEXTURE, 0, 0); mk_bind(cmb, 1, CIS, XBG_SOURCE_TEXTURE, 1, 0);
    for (int k = 0; k < 2; k++) {   // F1 / F1A: Sight's table + the LUT's two textures (spare slots 120, 121) and its block (spare 15)
        xbg_vkn_stage *f = g_pf[k] = mk_bundle_add(k ? "e1a0000000000001" : "e100000000000001", 4);
        mk_bind(f, 0, UB, XBG_SOURCE_SUPPORT, 0, 1392); mk_bind(f, 1, CIS, XBG_SOURCE_TEXTURE, 0, 0); mk_bind(f, 2, UB, XBG_SOURCE_UNIFORM, 5, 64);
        mk_bind(f, 3, CIS, XBG_SOURCE_TEXTURE, 120, 0); mk_bind(f, 4, CIS, XBG_SOURCE_TEXTURE, 121, 0); mk_bind(f, 5, UB, XBG_SOURCE_UNIFORM, 15, 64);
        post_meta(&g_pm[k], "91eb2d97a304709a", "921728a5bbb1c71b", k ? NB_FX_F_APPROX : NB_FX_F_EXACT);
        post_extra(&g_pm[k], 22, 1, NB_FX_K_TEX, 0, k ? NB_FX_E_LINEAR : 0); post_extra(&g_pm[k], 28, 1, NB_FX_K_TEX, 1, 0); post_extra(&g_pm[k], 34, 6, NB_FX_K_CBUF, 5, 0);
    }
    g_pf[2] = mk_bundle_add("e200000000000001", 4);   // F2: Combine's table + Vignette's input (spare 120) and block (spare 15)
    mk_bind(g_pf[2], 0, CIS, XBG_SOURCE_TEXTURE, 0, 0); mk_bind(g_pf[2], 1, CIS, XBG_SOURCE_TEXTURE, 1, 0);
    mk_bind(g_pf[2], 2, CIS, XBG_SOURCE_TEXTURE, 120, 0); mk_bind(g_pf[2], 3, UB, XBG_SOURCE_UNIFORM, 15, 64);
    post_meta(&g_pm[2], "98186c22a7209861", "43e8e8ed2b1df6c2", NB_FX_F_APPROX);
    post_extra(&g_pm[2], 16, 1, NB_FX_K_TEX, 0, 0); post_extra(&g_pm[2], 22, 6, NB_FX_K_CBUF, 5, 0);
    const vkn_shader *sl = mkshader(H(80, 1), "a0a0a0a000000001", "921728a5bbb1c71b", NULL), *ss = mkshader(H(81, 1), "a0a0a0a000000001", "91eb2d97a304709a", NULL);
    const vkn_shader *sf = mkshader(H(82, 1), "a0a0a0a000000001", "691768aa00000001", NULL), *sv = mkshader(H(83, 1), "a0a0a0a000000001", "43e8e8ed2b1df6c2", NULL);
    const vkn_shader *sc = mkshader(H(84, 1), "a0a0a0a000000001", "98186c22a7209861", NULL);
    vkn_tex *hdr = mktex(800, EF_RGBA16F, VKN_TC_COLOR, 1, 64, 32, 1, 1, 800, 0), *lt = mktex(801, EF_RGBA8, VKN_TC_SAMPLED, 1, 16, 16, 1, 1, 801, 0);
    vkn_tex *X = mktex(802, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 802, 0), *S = mktex(803, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 803, 0);
    vkn_tex *V = mktex(804, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 804, 0), *B = mktex(805, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 805, 0);
    vkn_tex *ui = mktex(806, EF_RGBA8, VKN_TC_SAMPLED, 1, 16, 16, 1, 1, 806, 0), *T = mktex(807, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 807, 0);
    CHECK(sl && ss && sf && sv && sc && hdr && lt && X && S && V && B && ui && T);
    if (!sl || !ss || !sf || !sv || !sc || !hdr || !lt || !X || !S || !V || !B || !ui || !T) return;
    static int32_t tl[VKN_TEX_SLOTS], ts[VKN_TEX_SLOTS], tf[VKN_TEX_SLOTS], tv[VKN_TEX_SLOTS], tc[VKN_TEX_SLOTS], sm[VKN_TEX_SLOTS];
    for (int i = 0; i < VKN_TEX_SLOTS; i++) tl[i] = ts[i] = tf[i] = tv[i] = tc[i] = sm[i] = -1;
    tl[0] = 800; tl[1] = 801; ts[0] = 802; tf[0] = 803; tv[0] = 802; tc[0] = 804; tc[1] = 806; sm[0] = sm[1] = 10;
    vkn_draw_in in[5]; const uint64_t sh[5] = {H(80, 1), H(81, 1), H(82, 1), H(83, 1), H(84, 1)}; int32_t *const tx[5] = {tl, ts, tf, tv, tc};
    for (int k = 0; k < 5; k++) {
        memset(&in[k], 0, sizeof in[k]); in[k].shader = sh[k]; in[k].prim = 1; in[k].alpha_ref = -1.0f; in[k].tex[1] = tx[k]; in[k].smp[1] = sm;
        in[k].viewport[2] = 64; in[k].viewport[3] = 32; in[k].scissor[2] = 64; in[k].scissor[3] = 32;
    }
    vkn_tex *const tgt[5] = {X, S, X, V, B};
    const uint64_t val0 = mk_val_errors, err0 = mk_errors;
    vkn_post_stats a, b; vkn_post_stats_get(&a);
    int draws[24]; uint64_t m = 0;
    for (int f = 0; f < 24; f++) {   // 0..19 the chain; 20 a LUT without its Sight; 21..23 the chain again (F1 earns its arming again: 8 frames later)
        VkCommandBuffer cb = fbegin(1); m = mark();
        uint32_t off; CHECK_EQ(vkn_ring_alloc(64, &off), 0); vkn_draw_ubo(1, 5, off, 64);
        static const float one[4] = {1, 1, 1, 1}; vkn_pass_clear_color(hdr, 0, one, NULL);
        if (f == 19) vkn_pass_copy(T, X);   // an engine copy of the alias image: seen at the next draw
        for (int k = 0; k < 5; k++) {
            if (f == 20 && k >= 1 && k <= 3) continue;
            targets(tgt[k], NULL, NULL); CHECK_EQ(vkn_draw_arrays(&in[k], 0, 3), 0);
        }
        fsubmit(0);
        draws[f] = cnt(m, cb, MK_E_DRAW);
    }
    vkn_post_stats_get(&b);
    for (int f = 0; f < 9; f++) CHECK_EQ(draws[f], 5);                                          // probation: everything drawn
    for (int f = 9; f < 19; f++) CHECK_EQ(draws[f], 2);                                         // LUT, FXAA, Vignette skipped
    CHECK_EQ(draws[19], 3);                                                                     // the copy broke route A: Sight F1, FXAA, Combine F2
    CHECK_EQ(draws[20], 2);                                                                     // LUT drawn from its stash at Combine + Combine
    CHECK_EQ(b.fused[1] - a.fused[1], 10); CHECK_EQ(b.fused[0] - a.fused[0], 1); CHECK_EQ(b.step_skipped - a.step_skipped, 10);
    CHECK_EQ(b.fused[2] - a.fused[2], 11); CHECK_EQ(b.recovered - a.recovered, 1);
    CHECK(b.redirected - a.redirected >= 10);                                                    // Vignette's input read through the alias
    CHECK_EQ(b.violations - a.violations, 2);                                                   // the copy of the alias, Combine reading the stale V
    CHECK(b.twins == a.twins);
    vkn_post_census();
    CHECK_EQ(mk_val_errors, val0); CHECK_EQ(mk_errors, err0);
}

static void test_b1_pass(void);   // [stage 5.2 B1] (end of file)
static void test_occ_a(void);     // [stage 5] technique A (end of file)
static void test_v2_drv(void);    // [stage 5.3 V2] driver corrections (end of file)
static void test_v1_preskin(void);   // [stage 5.3 V1] PRESKIN (end of file)
// ================================================================================================================ [stage 5.2 B2] UI pass
// DM_VK_NATIVE_UIPASS (vkn_ui.h "the UI pass on tile GPUs"): level 1 draws a GDraw epoch whose pass start never used its depth-stencil
// without it (render pass of one attachment), re-attaches it for the first draw that tests depth (LOAD of the colour drawn so far), keeps
// it attached the next frame (the history saw the use); a detached epoch's depth clear is remembered on the texture, a sample of the
// detached depth is still a feedback loop, a masked stencil clear re-attaches, the engine's draw inside a detached epoch gets GDraw's
// whole set (the fence in the vkn_fx chain); verify counts without changing the passes (would detach / re-attach / narrow / restart);
// level 2 hints the first rect's aligned area, a draw outside it ends the narrowed pass and drops the hint (the rest full area, LOAD).
// Exact = the validator is clean.
static void b2_quad(uint8_t ds, int32_t sx, int32_t sy, int32_t sw, int32_t sh, vkn_tex *t0) {
    static float verts[16] = {0, 0, 0, 0,  64, 0, 1, 0,  64, 32, 1, 1,  0, 32, 0, 1};
    static float vsc[32] = {1}, fsc[12] = {1};
    vkn_ui_scissor(sx, sy, sw, sh);
    vkn_ui_draw_in d; memset(&d, 0, sizeof d);
    d.vs = 0xA1; d.fs = 0xA2; d.vfmt = VKN_UI_VF_V2TC2; d.quads = 1; d.blend = 1; d.ds = ds; d.stencil_cmp = 0xff;
    d.tex[0] = t0; d.vsc = vsc; d.vsc_bytes = sizeof vsc; d.fsc = fsc; d.fsc_bytes = sizeof fsc; d.verts = verts; d.nverts = 4;
    CHECK_EQ(vkn_ui_draw(&d), 0);
}
// The pass module's area functions (vkn_ui.h) as weak stand-ins until the lead wires the real ones in vkn_pass.c (which then win):
// the hint is kept, a pass "is open" with exactly the hinted area while one is kept. Checks below hold for both.
static int g_b2_hint_on, g_b2_stub_used; static VkRect2D g_b2_hint;
int vkn_pass_area_hint(const VkRect2D *area) __attribute__((weak));
int vkn_pass_area_hint(const VkRect2D *area) { g_b2_stub_used = 1; g_b2_hint_on = area != NULL; if (area) g_b2_hint = *area; return 0; }
int vkn_pass_area_get(VkRect2D *out) __attribute__((weak));
int vkn_pass_area_get(VkRect2D *out) { if (!g_b2_hint_on) return 0; *out = g_b2_hint; return 1; }
static uint32_t b2_natt(uint64_t m, VkCommandBuffer cb, int k) { const mk_rp *r = rp_of(nth(m, cb, MK_E_BEGIN_RP, k)); return r ? r->natt : 0; }
static void test_b2_uipass(void) {
    vkn_tex *C = mktex(970, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 970, 0), *C2 = mktex(971, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 971, 0);
    vkn_tex *UD = mktex(972, EF_D24S8, VKN_TC_DEPTH, 1, 64, 32, 1, 1, 972, 0);
    CHECK(C && C2 && UD && g_t0); if (!C || !C2 || !UD || !g_t0) return;
    const int val0 = mk_val_errors, err0 = mk_errors;
    vkn_ui_pass_stats s0, s1;
    // ---- level 1, rule 0
    vkn_ui_pass_config(1, 0, 0, 0);
    vkn_ui_pass_stats_get(&s0);
    VkCommandBuffer cb = fbegin(1); uint64_t m = mark();
    vkn_ui_targets(C, UD); vkn_ui_viewport(0, 0, 64, 32);
    b2_quad(0, 0, 0, 64, 32, g_t0); b2_quad(0, 0, 0, 64, 32, NULL);
    b2_quad(2, 0, 0, 64, 32, NULL);   // depth test: re-attach
    b2_quad(0, 0, 0, 64, 32, NULL);
    vkn_ui_pass_fence();
    fsubmit(0);
    vkn_ui_pass_stats_get(&s1);
    CHECK_EQ(s1.mode, 1); CHECK_EQ(s1.api, 1);
    CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 2); CHECK_EQ(b2_natt(m, cb, 0), 1); CHECK_EQ(b2_natt(m, cb, 1), 2);
    { const mk_ev *r1 = nth(m, cb, MK_E_BEGIN_RP, 1); CHECK(att_of(r1, 0) && att_of(r1, 0)->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD); }
    CHECK_EQ(s1.detached - s0.detached, 1); CHECK_EQ(s1.reattach - s0.reattach, 1); CHECK_EQ(s1.kept_off - s0.kept_off, 0);
    CHECK(vkn_pass_depth() == UD);
    // next frame: the history keeps this pass start's DS (used 1 frame ago)
    s0 = s1; cb = fbegin(1); m = mark();
    vkn_ui_targets(C, UD); vkn_ui_viewport(0, 0, 64, 32);
    b2_quad(0, 0, 0, 64, 32, NULL); b2_quad(2, 0, 0, 64, 32, NULL);
    vkn_ui_pass_fence();
    fsubmit(0);
    vkn_ui_pass_stats_get(&s1);
    CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 1); CHECK_EQ(b2_natt(m, cb, 0), 2);
    CHECK_EQ(s1.detached - s0.detached, 0); CHECK_EQ(s1.ds_refused[VKN_UIP_DS_HIST] - s0.ds_refused[VKN_UIP_DS_HIST], 1); CHECK_EQ(s1.reattach - s0.reattach, 0);
    // a pass start that never needs its DS: detached frame after frame; a depth clear inside is remembered, a sample of the depth is a
    // feedback loop (placeholder), a masked stencil clear re-attaches
    for (int f = 0; f < 2; f++) {
        s0 = s1; cb = fbegin(1); m = mark();
        vkn_ui_targets(C2, UD); vkn_ui_viewport(0, 0, 64, 32);
        b2_quad(0, 0, 0, 64, 32, NULL);
        vkn_ui_clear_ds(1, 0xff);
        CHECK(UD->pass_dclr == 3); CHECK_EQ(cnt(m, cb, MK_E_CLEAR_ATT), 0);
        vkn_ui_stats u0, u1; vkn_ui_stats_get(&u0);
        b2_quad(0, 0, 0, 64, 32, UD);
        vkn_ui_stats_get(&u1); CHECK_EQ(u1.feedback + u1.placeholder - u0.feedback - u0.placeholder, 1); CHECK(UD->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkn_ui_pass_fence();
        fsubmit(0);
        vkn_ui_pass_stats_get(&s1);
        CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 1); CHECK_EQ(b2_natt(m, cb, 0), 1);
        CHECK_EQ(s1.detached - s0.detached, 1); CHECK_EQ(s1.fence_reattach - s0.fence_reattach, 1);
    }
    s0 = s1; cb = fbegin(1); m = mark();
    vkn_ui_targets(C2, UD); vkn_ui_viewport(0, 0, 64, 32);
    b2_quad(0, 0, 0, 64, 32, NULL);
    vkn_ui_clear_ds(0, 0x0f);   // masked: a stencil draw of its own
    vkn_ui_pass_fence();
    fsubmit(0);
    vkn_ui_pass_stats_get(&s1);
    CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 2); CHECK_EQ(b2_natt(m, cb, 0), 1); CHECK_EQ(b2_natt(m, cb, 1), 2);
    { const mk_ev *r1 = nth(m, cb, MK_E_BEGIN_RP, 1); CHECK(att_of(r1, 1) && att_of(r1, 1)->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR); }   // the remembered clear folded
    CHECK_EQ(s1.reattach - s0.reattach, 1); CHECK_EQ(s1.kept_off - s0.kept_off, 1);   // (the second loop frame's epoch, committed here)
    // the engine draws while a detached epoch is current (a custom draw): GDraw's whole set, the UI's pass ends
    s0 = s1; cb = fbegin(1); m = mark();
    vkn_ui_targets(C, UD);
    for (int i = 0; i < 2; i++) { vkn_ui_pass_fence(); vkn_ui_targets(C2, UD); }   // (fences without a pass: nothing changes)
    vkn_ui_viewport(0, 0, 64, 30);   // (another pass start than C2's above, which used its DS: no history)
    b2_quad(0, 0, 0, 64, 32, NULL);
    CHECK(vkn_pass_depth() == NULL);
    { vkn_draw_in in = g_in; CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0); }
    CHECK(vkn_pass_depth() == UD);
    fsubmit(0);
    vkn_ui_pass_stats_get(&s1);
    CHECK_EQ(s1.fence_draw - s0.fence_draw, 1); CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 2); CHECK_EQ(b2_natt(m, cb, 1), 2);
    // ---- verify (level 2): nothing changes, the decisions are counted
    vkn_ui_pass_config(2, 1, 0, 0);
    targets(NULL, NULL, NULL);   // (the engine's set in between: GDraw's set left current by a forgotten epoch counts as the engine's - shared)
    vkn_ui_pass_stats_get(&s0);
    cb = fbegin(1); m = mark();
    vkn_ui_targets(C2, UD); vkn_ui_viewport(0, 0, 64, 32);
    b2_quad(0, 0, 0, 16, 8, NULL);    // area seed: 16x8 (+4 px of the viewport is cut by the scissor) aligned to 32 = 32x32 of 64x32
    b2_quad(0, 40, 0, 16, 8, NULL);   // outside: would restart
    b2_quad(1, 0, 0, 16, 8, NULL);    // stencil: would re-attach
    vkn_ui_pass_fence();
    fsubmit(0);
    vkn_ui_pass_stats_get(&s1);
    CHECK_EQ(s1.verify, 1); CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 1); CHECK_EQ(b2_natt(m, cb, 0), 2);
    { const mk_ev *r0 = nth(m, cb, MK_E_BEGIN_RP, 0); CHECK(r0 && r0->rect.extent.width == 64 && r0->rect.extent.height == 32); }
    CHECK_EQ(s1.w_detach - s0.w_detach, 1); CHECK_EQ(s1.w_reattach - s0.w_reattach, 1); CHECK_EQ(s1.w_area - s0.w_area, 1); CHECK_EQ(s1.w_restart - s0.w_restart, 1);
    CHECK_EQ(s1.detached - s0.detached, 0); CHECK_EQ(s1.area_refused[VKN_UIP_AR_API] - s0.area_refused[VKN_UIP_AR_API], 0);
    // ---- level 2: area hinted, one restart by a draw outside it (DS detached too)
    vkn_ui_pass_config(2, 0, 0, 0);
    targets(NULL, NULL, NULL);   // (the engine's set in between: GDraw's set left current by a forgotten epoch counts as the engine's - shared)
    vkn_ui_pass_stats_get(&s0);
    cb = fbegin(1); m = mark();
    vkn_ui_targets(C2, UD); vkn_ui_viewport(0, 0, 64, 32);
    b2_quad(0, 0, 0, 16, 8, NULL); b2_quad(0, 8, 8, 16, 8, NULL);   // inside 32x32
    { VkRect2D a; CHECK_EQ(vkn_pass_area_get(&a), 1); CHECK(a.offset.x == 0 && a.offset.y == 0 && a.extent.width == 32 && a.extent.height == 32); }
    b2_quad(0, 40, 0, 16, 8, NULL);   // outside: the narrowed pass ends, the next one is full
    { VkRect2D a; const int open = vkn_pass_area_get(&a); CHECK(!open || (a.offset.x == 0 && a.extent.width == 64)); }
    vkn_ui_pass_fence();
    fsubmit(0);
    vkn_ui_pass_stats_get(&s1);
    CHECK_EQ(s1.area_hinted - s0.area_hinted, 1); CHECK_EQ(s1.area_applied - s0.area_applied, 1); CHECK_EQ(s1.restarts - s0.restarts, 1);
    CHECK_EQ(s1.detached - s0.detached, 1); CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 2); CHECK_EQ(b2_natt(m, cb, 0), 1); CHECK_EQ(b2_natt(m, cb, 1), 1);
    if (!g_b2_stub_used) {   // the real pass functions: the first pass began narrowed, the second full
        const mk_ev *r0 = nth(m, cb, MK_E_BEGIN_RP, 0), *r1 = nth(m, cb, MK_E_BEGIN_RP, 1);
        CHECK(r0 && r0->rect.extent.width == 32 && r0->rect.extent.height == 32); CHECK(r1 && r1->rect.extent.width == 64);
    } else CHECK_EQ(g_b2_hint_on, 0);
    // ---- off again: the stage-3 path (the set as GDraw gives it)
    vkn_ui_pass_config(0, 0, 0, 0);
    cb = fbegin(1); m = mark();
    vkn_ui_targets(C2, UD); vkn_ui_viewport(0, 0, 64, 32);
    b2_quad(0, 0, 0, 64, 32, NULL);
    fsubmit(0);
    CHECK_EQ(b2_natt(m, cb, 0), 2);
    targets(NULL, NULL, NULL);
    CHECK_EQ(mk_val_errors, val0); CHECK_EQ(mk_errors, err0);
    frames_n(4);
}

// ================================================================================================================ [stage 5.2 B5] MAT + XLQ
// vkn_mat.c with the facts a test function gives (vkn_mat_test_setup): in a set without colour targets a pure fragment shader is
// dropped (a vertex-only pipeline), a killing one only when the gate words in the ring pass (and the alpha test passes .w), every
// refusal counted by its reason; verify mode changes nothing; variant B swaps in the cut-down vertex module with C1 as effect block 0;
// XLQ swaps the vertex module of a colour draw on a depth target and keeps the fragment module; count mode changes nothing.
#include "../../../vkn/vkn_mat.h"
static struct { int flags, applied; const nr_mat_gate *gate; uint64_t read, decl, written; VkShaderModule vsm; int vsm_calls; uint64_t last_keep; } g_b5;
static VkShaderModule b5_vs_module(const vkn_shader *s, uint64_t code, uint64_t keep) { (void)s; (void)code; g_b5.vsm_calls++; g_b5.last_keep = keep; return g_b5.vsm; }
static int b5_facts(const vkn_shader *s, vkn_mat_test_facts *f) {
    (void)s;
    f->fs_flags = g_b5.flags; f->gate = g_b5.gate; f->alpha_applied = g_b5.applied;
    f->fs_read = g_b5.read; f->fs_decl = g_b5.decl; f->vs_written = g_b5.written; f->vs_module = b5_vs_module;
    return 0;
}
static const mk_gp *b5_pipe(VkCommandBuffer cb) {   // the pipeline bound last in cb (the draw-cost path binds only what changed)
    for (uint64_t q = mk_seq; q-- > mk_ev_base;) {
        const mk_ev *e = mk_ev_at(q);
        if (e && e->cb == cb && e->op == MK_E_BIND_PIPE) { const mk_obj *o = pipe_of(e); return o ? &o->gp : NULL; }
    }
    return NULL;
}
static uint32_t b5_nst(const mk_gp *p) { return p ? p->nst : 99u; }
static void test_b5_mat(void) {
    if (!g_s1 || !g_d3 || !g_a2) { CHECK(0); return; }
    const int v0 = mk_val_errors, e0 = mk_errors;
    static const uint32_t spv[5] = {0x07230203u, 0x00010000u, 0, 1, 0};
    VkShaderModuleCreateInfo mi; memset(&mi, 0, sizeof mi); mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; mi.codeSize = sizeof spv; mi.pCode = spv;
    VkShaderModule cutm = VK_NULL_HANDLE; vkCreateShaderModule(vkn_device()->dev, &mi, NULL, &cutm); CHECK(cutm != VK_NULL_HANDLE);
    // the gate: fragment block slot 0 (bank 3) word 1 == 1.0f, Location 0 .w = 1.0 on that path (Base_PBR's "fade off" path)
    static nr_mat_gate gate; memset(&gate, 0, sizeof gate);
    gate.paths = 1; gate.terms = 1; gate.path[0].first = 0; gate.path[0].count = 1; gate.path[0].w_known = 1; gate.path[0].w_bits = 0x3f800000u;
    gate.term[0].bank_word = 3u << 16 | 1u; gate.term[0].op = 180 /* FOrdEqual */; gate.term[0].cbits = 0x3f800000u; gate.term[0].flags = 1 | 2;
    float *fb = (float *)((uint8_t *)vkn_ring_cpu() + g_offf0); CHECK(fb != NULL);
    VkCommandBuffer cb = fbegin(1);
    vkn_draw_in in = g_in; in.alpha_ref = -1.0f;
    vkn_draw_ubo(1, 0, g_offf0, 32);
    vkn_mat_stats s0, s1; vkn_mat_stats_get(&s0);
    vkn_fx_stats f0, f1; vkn_fx_stats_get(&f0);
#define B5_DRAW() (vkn_draw_indexed(&in, 0, 3, 1), b5_pipe(cb))
#define B5_NST() b5_nst(B5_DRAW())
    // ---- variant A
    memset(&g_b5, 0, sizeof g_b5); g_b5.applied = 1;
    vkn_mat_test_setup(1, 0, b5_facts); CHECK_EQ(vkn_mat_mode(), 1); CHECK_EQ(vkn_mat_xlq(), 0);
    targets(NULL, NULL, g_d3);
    CHECK_EQ(B5_NST(), 1u);                                                 // pure, alpha off: no fragment stage
    { const mk_gp *p = b5_pipe(cb); CHECK(p && p->module[0] == g_vs1_mod && p->stage[0] == VK_SHADER_STAGE_VERTEX_BIT); }
    targets(g_a2, NULL, g_d3); CHECK_EQ(B5_NST(), 2u);                    // a colour target: the engine's pipeline
    targets(NULL, NULL, g_d3);
    in.alpha_ref = 0.5f; CHECK_EQ(B5_NST(), 2u);                           // alpha test applied, no gate record: kept (it may kill)
    vkn_mat_test_setup(1, 0, b5_facts); g_b5.applied = 0; CHECK_EQ(B5_NST(), 1u);   // the library refuses the alpha test: drawn without it
    in.alpha_ref = -1.0f;
    vkn_mat_test_setup(1, 0, b5_facts); g_b5.flags = NR_MAT_KILL; CHECK_EQ(B5_NST(), 2u);          // a kill without a gate
    vkn_mat_test_setup(1, 0, b5_facts); g_b5.gate = &gate; g_b5.applied = 1;
    fb[1] = 1.0f; CHECK_EQ(B5_NST(), 1u);                                                           // the gate passes
    fb[1] = 0.25f; CHECK_EQ(B5_NST(), 2u);                                                          // the gate fails (the ring now)
    fb[1] = 1.0f; in.alpha_ref = 0.5f; CHECK_EQ(B5_NST(), 1u);                                      // gate + alpha passes .w = 1.0 > 0.5
    in.alpha_ref = 1.0f; vkn_mat_test_setup(1, 0, b5_facts); CHECK_EQ(B5_NST(), 2u);              // .w = 1.0 is not GREATER than 1.0
    in.alpha_ref = -1.0f;
    gate.term[0].bank_word = 3u << 16 | 8u; vkn_mat_test_setup(1, 0, b5_facts); CHECK_EQ(B5_NST(), 2u);   // a word past the block: fails
    gate.term[0].bank_word = 3u << 16 | 1u;
    vkn_mat_test_setup(1, 0, b5_facts); g_b5.gate = NULL; g_b5.flags = NR_MAT_SIDE; CHECK_EQ(B5_NST(), 2u);   // a storage write
    vkn_mat_test_setup(1, 0, b5_facts); g_b5.flags = NR_MAT_FRAGOUT; CHECK_EQ(B5_NST(), 2u);                  // writes depth
    vkn_mat_test_setup(1, 0, NULL); g_b5.flags = 0; CHECK_EQ(B5_NST(), 2u);                                   // no library: nothing known
    vkn_mat_stats_get(&s1);
    CHECK_EQ(s1.no_fs - s0.no_fs, 4); CHECK_EQ(s1.pure - s0.pure, 2); CHECK_EQ(s1.gate_off - s0.gate_off, 1); CHECK_EQ(s1.gate_alpha - s0.gate_alpha, 1);
    CHECK_EQ(s1.alpha_na - s0.alpha_na, 1);
    CHECK_EQ(s1.refused[VKN_MAT_W_KILL] - s0.refused[VKN_MAT_W_KILL], 2); CHECK_EQ(s1.refused[VKN_MAT_W_GATE] - s0.refused[VKN_MAT_W_GATE], 2);
    CHECK_EQ(s1.refused[VKN_MAT_W_ALPHA] - s0.refused[VKN_MAT_W_ALPHA], 1); CHECK_EQ(s1.refused[VKN_MAT_W_FACTS] - s0.refused[VKN_MAT_W_FACTS], 2);
    CHECK_EQ(s1.refused[VKN_MAT_W_NOLIB] - s0.refused[VKN_MAT_W_NOLIB], 1);
    // ---- verify mode: decided and counted, the engine's shaders drawn
    vkn_mat_test_setup(3, 0, b5_facts); vkn_mat_stats_get(&s0);
    CHECK_EQ(B5_NST(), 2u); vkn_mat_stats_get(&s1); CHECK_EQ(s1.would - s0.would, 1); CHECK_EQ(s1.no_fs, s0.no_fs);
    // ---- variant B: the cut-down vertex module (trim keep 0), the vertex-stage table with C1 as effect block 0 (binding 18)
    vkn_mat_test_setup(2, 0, b5_facts); g_b5.vsm = cutm; vkn_mat_stats_get(&s0);
    {
        const uint64_t m = mark(); const mk_gp *p = B5_DRAW();
        CHECK(p && p->nst == 1 && p->module[0] == cutm); CHECK_EQ(g_b5.last_keep, 0);
        const mk_dw *w3 = dw_of(nth(m, cb, MK_E_BIND_SETS, 0), 18), *w1 = dw_of(nth(m, cb, MK_E_BIND_SETS, 0), 6);
        CHECK(w3 && w3->buf == g_s1->c1[VKN_SRC_VS].buf && w3->range == g_s1->c1[VKN_SRC_VS].size); CHECK(w1 != NULL);
        CHECK(dw_of(nth(m, cb, MK_E_BIND_SETS, 0), 22) == NULL);   // (the fragment stage's texture binding is not in the table)
        p = B5_DRAW(); CHECK(p && p->module[0] == cutm); CHECK_EQ(g_b5.vsm_calls, 1);   // made once
    }
    vkn_mat_stats_get(&s1); CHECK_EQ(s1.vs_swaps - s0.vs_swaps, 2); CHECK_EQ(s1.twins - s0.twins, 1);
    vkn_mat_test_setup(2, 0, b5_facts); g_b5.vsm = VK_NULL_HANDLE; vkn_mat_stats_get(&s0);   // the trim has nothing to drop: variant A
    { const mk_gp *p = B5_DRAW(); CHECK(p && p->nst == 1 && p->module[0] == g_vs1_mod); }
    vkn_mat_stats_get(&s1); CHECK_EQ(s1.vs_refused[VKN_MAT_V_NOTHING] - s0.vs_refused[VKN_MAT_V_NOTHING], 1);
    // ---- the replay door: the same decision for a draw that does not pass the chain
    {
        vkn_mat_test_setup(2, 0, b5_facts); g_b5.vsm = cutm;
        vkn_fx_ctx c; memset(&c, 0, sizeof c); c.shader = g_s1; c.depth = g_d3; c.vfmt = vkn_vfmt_id(g_in.vfmt);
        vkn_fx_out o; memset(&o, 0, sizeof o);
        CHECK_EQ(vkn_mat_apply(&in, &c, &o), 1); CHECK(o.no_fs == 1 && o.shader && vkn_fx_shader_is(o.shader) && o.ubo[0].size == g_s1->c1[VKN_SRC_VS].size);
        memset(&o, 0, sizeof o); c.color[0] = g_a2; CHECK_EQ(vkn_mat_apply(&in, &c, &o), 0); CHECK(!o.no_fs && !o.shader);
    }
    // ---- XLQ: a colour draw on a depth target: the trimmed vertex module (keep = what the fragment shader reads) + the engine's fragment module
    targets(g_a2, NULL, g_d3);
    memset(&g_b5, 0, sizeof g_b5); g_b5.applied = 1; g_b5.read = 0xF; g_b5.decl = 0xFF; g_b5.written = 0xFFF; g_b5.vsm = cutm;
    vkn_mat_test_setup(0, 1, b5_facts); vkn_mat_stats_get(&s0);
    { const mk_gp *p = B5_DRAW(); CHECK(p && p->nst == 2 && p->module[0] == cutm && p->module[1] == g_s1->module[VKN_SRC_FS]); CHECK_EQ(g_b5.last_keep, 0xF); }
    in.alpha_ref = 0.5f; B5_DRAW(); in.alpha_ref = -1.0f;                                  // another variant (alpha reference)
    targets(g_a2, NULL, NULL); B5_DRAW(); targets(g_a2, NULL, g_d3);                         // no depth target: refused
    g_b5.written = 0xF; vkn_mat_test_setup(0, 1, b5_facts);
    { const mk_gp *p = B5_DRAW(); CHECK(p && p->module[0] == g_vs1_mod); }                 // nothing to drop
    g_b5.written = 0xFFF; g_b5.read = ~0ull; vkn_mat_test_setup(0, 1, b5_facts); B5_DRAW(); // fragment inputs not analysable
    g_b5.read = 0xF; vkn_mat_test_setup(0, 2, b5_facts);
    { const mk_gp *p = B5_DRAW(); CHECK(p && p->module[0] == g_vs1_mod && p->nst == 2); } // count mode
    vkn_mat_stats_get(&s1);
    CHECK_EQ(s1.xlq_swaps - s0.xlq_swaps, 2); CHECK_EQ(s1.xlq_would - s0.xlq_would, 1);
    CHECK_EQ(s1.xlq_refused[VKN_MAT_X_NODEPTH] - s0.xlq_refused[VKN_MAT_X_NODEPTH], 1); CHECK_EQ(s1.xlq_refused[VKN_MAT_X_NOTHING] - s0.xlq_refused[VKN_MAT_X_NOTHING], 1);
    CHECK_EQ(s1.xlq_refused[VKN_MAT_X_FS] - s0.xlq_refused[VKN_MAT_X_FS], 1);
    // ---- MAT and XLQ together in a depth-only set: MAT decides first; a killing shader MAT keeps gets the XLQ trim
    targets(NULL, NULL, g_d3);
    vkn_mat_test_setup(1, 1, b5_facts); g_b5.flags = 0; CHECK_EQ(B5_NST(), 1u);
    vkn_mat_test_setup(1, 1, b5_facts); g_b5.flags = NR_MAT_KILL;
    { const mk_gp *p = B5_DRAW(); CHECK(p && p->nst == 2 && p->module[0] == cutm); }
#undef B5_DRAW
#undef B5_NST
    vkn_mat_census();
    vkn_mat_test_setup(0, 0, NULL); CHECK_EQ(vkn_mat_mode(), 0);
    targets(g_a2, NULL, g_d3); vkn_draw_indexed(&in, 0, 3, 1);
    fsubmit(0); frames_n(4);
    vkn_fx_stats_get(&f1); CHECK(f1.no_fs > f0.no_fs);
    CHECK_EQ(mk_val_errors, v0); CHECK_EQ(mk_errors, e0);
    CHECK_EQ(vkn_mat_reason_name(0, VKN_MAT_W_GATE)[0] != '?', 1);
}

// ================================================================================================================ [stage 5.2 B4] the pre-pass hybrid
// vkn_prepass.c on the xbg_prepass.h model: frame 1 learns key A (kept: new), frame 2 admits it (skipped in the pre-pass, its G-buffer
// draw gets LEQUAL + write), frame 3 skips it as established; a frame whose G-buffer does not draw A is bad (red line) and the next
// frame draws A again; kill / alpha / non-indexed / the shadow-atlas-like square depth are kept; compare mode changes nothing; poison
// skips without the override; an override a later technique skips is lost; an open report pair keeps the draw while COND != 0.
#include "../../../vkn/vkn_prepass.h"
#include "../xbg_prepass.h"
static int g_b4_cls = 1, g_b4_skip;
static int b4_facts(const vkn_shader *s) { (void)s; return g_b4_cls; }
static void b4_skipper(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) { (void)in; (void)ctx; (void)user; if (g_b4_skip) out->skip = 1; }
static void test_b4_prepass(void) {
    if (!g_s1) { CHECK(0); return; }
    const int v0 = mk_val_errors, e0 = mk_errors;
    vkn_tex *D = mktex(330, EF_D24S8, VKN_TC_DEPTH, 1, 1280, 720, 1, 1, 330, 0), *C = mktex(331, EF_RGBA8, VKN_TC_COLOR, 1, 1280, 720, 1, 1, 331, 0);
    vkn_tex *Q = mktex(332, EF_D24S8, VKN_TC_DEPTH, 1, 1024, 1024, 1, 1, 332, 0);   // square: the shadow atlas's shape
    CHECK(D && C && Q); if (!D || !C || !Q) return;
    CHECK_EQ(vkn_write_mask_nvn(C->nvn_fmt), 0xF);
    CHECK(vkn_ds_put(H(70, 1), 1, 1, 1, 0, 0, 0, 0, 0) != 0);   // pre-pass: LESS + write
    CHECK(vkn_ds_put(H(71, 1), 1, 0, 2, 0, 0, 0, 0, 0) != 0);   // G-buffer: EQUAL, no write
    CHECK_EQ(vkn_fx_register("b4-skipper", b4_skipper, NULL, 600), 0);
    vkn_draw_in pre = g_in, gb = g_in; pre.ds = H(70, 1); gb.ds = H(71, 1); pre.blend = gb.blend = 0; pre.alpha_ref = gb.alpha_ref = -1.0f;
    vkn_prepass_stats s0, s1; vkn_prepass_stats_get(&s0);
    g_b4_cls = 1; vkn_prepass_test_setup(1, b4_facts); CHECK_EQ(vkn_prepass_mode(), 1);
    int predraws[8], gbwrite[8];
    // one frame: pre-pass X (first: kept) and A, G-buffer A (or B when `gbkey` is 0); returns the pre-pass draws recorded
#define B4_FRAME(i, gbA) do { VkCommandBuffer cb_ = fbegin(1); targets(NULL, NULL, D); uint64_t m_ = mark();                                   \
        vkn_draw_indexed(&pre, 3, 3, 1); vkn_draw_indexed(&pre, 0, 3, 1); predraws[i] = cnt(m_, cb_, MK_E_DRAW_IDX);                        \
        targets(C, NULL, D); vkn_draw_indexed(&gb, (gbA) ? 0 : 6, 3, 1); { const mk_gp *p_ = b5_pipe(cb_);                               \
        gbwrite[i] = p_ && p_->has_ds ? (int)p_->ds.depthWriteEnable * 10 + (int)p_->ds.depthCompareOp : -1; } fsubmit(0); } while (0)
    B4_FRAME(0, 1); B4_FRAME(1, 1); B4_FRAME(2, 1);
    CHECK_EQ(predraws[0], 2); CHECK_EQ(gbwrite[0], VK_COMPARE_OP_EQUAL);                       // learned (new)
    CHECK_EQ(predraws[1], 1); CHECK_EQ(gbwrite[1], 10 + VK_COMPARE_OP_LESS_OR_EQUAL);          // admitted: skipped, LEQUAL + write
    CHECK_EQ(predraws[2], 1); CHECK_EQ(gbwrite[2], 10 + VK_COMPARE_OP_LESS_OR_EQUAL);          // established
    B4_FRAME(3, 0);                                                                            // skipped, but the G-buffer draws B: bad
    CHECK_EQ(predraws[3], 1); CHECK_EQ(gbwrite[3], VK_COMPARE_OP_EQUAL);
    B4_FRAME(4, 1); CHECK_EQ(predraws[4], 2); CHECK_EQ(gbwrite[4], VK_COMPARE_OP_EQUAL);       // the history dropped A: drawn
    { const prepass_result *r = (const prepass_result *)vkn_prepass_test_last(); CHECK(r && r->bad_keys == 1 && r->unmatched == 1); }
    B4_FRAME(5, 1); B4_FRAME(6, 1); CHECK_EQ(predraws[6], 1);                                    // clean again, then admitted
    // a later technique skips the overridden G-buffer draw: the override is lost, the key refused
    g_b4_skip = 0; { VkCommandBuffer cb = fbegin(1); targets(NULL, NULL, D); vkn_draw_indexed(&pre, 3, 3, 1); vkn_draw_indexed(&pre, 0, 3, 1);
      targets(C, NULL, D); g_b4_skip = 1; vkn_draw_indexed(&gb, 0, 3, 1); g_b4_skip = 0; vkn_draw_indexed(&gb, 6, 3, 1); (void)cb; fsubmit(0); }
    vkn_prepass_stats_get(&s1); CHECK_EQ(s1.lost - s0.lost, 1);
    // kept by nature: a killing fragment shader, the alpha test, an arrays draw; the square depth target is not the pre-pass
    vkn_prepass_test_setup(1, b4_facts); B4_FRAME(0, 1); B4_FRAME(1, 1); CHECK_EQ(predraws[1], 1);
    g_b4_cls = 2; vkn_prepass_test_setup(1, b4_facts); B4_FRAME(0, 1); B4_FRAME(1, 1); B4_FRAME(2, 1); CHECK_EQ(predraws[2], 2);
    { const prepass_result *r = (const prepass_result *)vkn_prepass_test_last(); CHECK(r && r->keep[PREPASS_K_KILL] == 2 && r->pre_skip == 0); }
    g_b4_cls = 1; vkn_prepass_test_setup(1, b4_facts); pre.alpha_ref = 0.5f; B4_FRAME(0, 1); B4_FRAME(1, 1); B4_FRAME(2, 1); CHECK_EQ(predraws[2], 2); pre.alpha_ref = -1.0f;
    { const prepass_result *r = (const prepass_result *)vkn_prepass_test_last(); CHECK(r && r->keep[PREPASS_K_ALPHA] == 2); }
    g_b4_cls = -1; vkn_prepass_test_setup(1, b4_facts); B4_FRAME(0, 1); B4_FRAME(1, 1); B4_FRAME(2, 1); CHECK_EQ(predraws[2], 2);   // facts unknown
    g_b4_cls = 1; vkn_prepass_test_setup(1, b4_facts);
    { VkCommandBuffer cb = fbegin(1); for (int k = 0; k < 3; k++) { targets(NULL, NULL, Q); uint64_t m = mark(); vkn_draw_indexed(&pre, 0, 3, 1); CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 1); }
      fsubmit(0); fbegin(1); fsubmit(0); CHECK(vkn_prepass_test_last() == NULL); }   // no table: never the pre-pass
    // compare mode: decided, nothing changes; poison: skipped without the override
    vkn_prepass_test_setup(2, b4_facts); B4_FRAME(0, 1); B4_FRAME(1, 1); B4_FRAME(2, 1);
    CHECK_EQ(predraws[1], 2); CHECK_EQ(gbwrite[1], VK_COMPARE_OP_EQUAL); CHECK_EQ(predraws[2], 2);
    vkn_prepass_test_setup(3, b4_facts); B4_FRAME(0, 1); B4_FRAME(1, 1); CHECK_EQ(predraws[1], 1); CHECK_EQ(gbwrite[1], VK_COMPARE_OP_EQUAL);
    // COND != 0: a pre-pass draw inside an open report pair is kept (a skipped one would leave the pair without samples)
    vkn_prepass_test_setup(1, b4_facts); B4_FRAME(0, 1);
    {
        vkn_query_init(2); VkCommandBuffer cb = fbegin(1); targets(NULL, NULL, D); uint64_t m = mark();
        vkn_draw_indexed(&pre, 3, 3, 1); vkn_query_begin(0x7b400); vkn_draw_indexed(&pre, 0, 3, 1); vkn_query_end(0x7b400);
        CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 2);
        targets(C, NULL, D); vkn_draw_indexed(&gb, 0, 3, 1); fsubmit(0); vkn_query_init(0);
        vkn_prepass_stats s2; vkn_prepass_stats_get(&s2); CHECK(s2.counting_open > s1.counting_open);
    }
#undef B4_FRAME
    vkn_prepass_census();
    vkn_prepass_stats_get(&s1);
    CHECK(s1.skipped - s0.skipped >= 7); CHECK(s1.overrides - s0.overrides >= 5); CHECK_EQ(s1.ds_full, s0.ds_full);
    vkn_prepass_test_setup(0, NULL); CHECK_EQ(vkn_prepass_mode(), 0);
    frames_n(4);
    CHECK_EQ(mk_val_errors, v0); CHECK_EQ(mk_errors, e0);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    mk_main_thread = pthread_self();
    mk_validate_on = 1; mk_blit = 1;
    vkn_set_mode(VKN_NATIVE);
    // install order of the host: the modules' listeners first, then the test's own
    vkn_pass_init(); vkn_draw_install(); vkn_tex_update_init();
    vkn_draw_cost_set(VKN_COST_ALL);   // [stage 5] the draw-cost shortcuts as the host sets them in mode 1 (DM_VK_NATIVE_COST default)
    vkn_rec_on_begin(lis_begin); vkn_rec_on_close(lis_close);
    {   // [stage 5.5] VKN_TEST_ONLY=rt: the recording thread's test alone (the ThreadSanitizer build)
        const char *only = getenv("VKN_TEST_ONLY");
        if (only && !strcmp(only, "rt")) {
            g_win = mktex(900, EF_RGBA8, VKN_TC_WINDOW, 1, 64, 32, 1, 1, ~0u, 0);
            draw_setup(); frames_n(4);
            test_rt();
            CHECK_EQ(mk_val_errors, 0); CHECK_EQ(mk_errors, 0);
            if (g_fails) { printf("vkn_stage3_test (rt only): %d of %d checks FAILED\n", g_fails, g_checks); return 1; }
            printf("vkn_stage3_test (rt only): %d checks passed\n", g_checks);
            return 0;
        }
    }
    test_no_device();
    test_frame_order();
    test_frame_ring();
    test_frame_retire();
    test_frame_open_and_fail();
    test_present();
    test_pass();
    test_draw();
    test_tex_update();
    test_e2e();
    test_thumbnail();
    test_ui();
    test_query();
    test_occ_a();   // [stage 5] technique A (vkn_occ.c, DM_VK_NATIVE_COND 5 / 6; forked children: here, while the effect chain has room)
    test_a5_shc();      // [stage 5.1 A5] (forked children: the cache's switches are read once per process)
    test_a5_fx();
    test_a5_oracle();
    test_post();   // [stage 5.2 B3] it installs the post effect for the rest of the process
    test_b1_pass();   // [stage 5.2 B1] render-pass load / store policy (forked child)
    test_b2_uipass();   // [stage 5.2 B2] DM_VK_NATIVE_UIPASS
    test_b5_mat();   // [stage 5.2 B5] MAT + XLQ (vkn_mat.c)
    test_b4_prepass();   // [stage 5.2 B4] the pre-pass hybrid (vkn_prepass.c)
    test_v2_drv();   // [stage 5.3 V2] driver corrections (vkn_drv.c)
    test_v1_preskin();   // [stage 5.3 V1] PRESKIN (vkn_preskin.c)
    test_rt();     // [stage 5.5] last (forked children: the recording thread's modes; its warm-up changes this process's state)
    CHECK_EQ(mk_val_errors, 0);
    CHECK_EQ(mk_errors, 0);
    CHECK_EQ(mk_qlock_depth, 0);
    if (g_fails) { printf("vkn_stage3_test: %d of %d checks FAILED\n", g_fails, g_checks); return 1; }
    printf("vkn_stage3_test: %d checks passed\n", g_checks);
    return 0;
}

// ================================================================================================================ [stage 5.2 B1]
// The render-pass load / store policy (vkn_pass.c "stage 5.2 B1"), in a forked child (the policy and its effect function stay there):
//   cover    the first draw's proof: whole target (triangle list, two-triangle quad, fan and quad lists of the arrays rewrite) ->
//            loadOp DONT_CARE; each refusal reason (no facts, stale facts, effect, shader kill, no program, output, blend,
//            mask, depth test, raster, primitive, input, buffer, uniform, w, empty, geometry, culled vs the other winding, part too
//            small, part with a clear); positions from a uniform block (and refused with the sub-switch off); the area pass (render
//            area = proven rect, a draw inside keeps it, a draw / clear outside / a draw the chain did not see restarts it full with
//            LOAD); depth DONT_CARE (ALWAYS + write, no stencil) and not for D24S8; verify (=2: LOAD kept, counted); poison (=3:
//            magenta after the begin);
//   fresh    1: a never-written target begins with CLEAR to zeros (no clear command), several layers keep the zero fill; 0: the
//            zero fill; 2: an alias of shared storage written by the other image begins DONT_CARE (and LOAD when it was not), 3 poisons;
//   store    a target cleared every frame and never read: STORE for N frames, then DONT_CARE; a read of the discarded content is a
//            violation (counted, banned: STORE again); verify (=2) counts the would-be violation; poison (=3) clears magenta before
//            the pass ends; a shared-storage target is never discarded;
//   tilebar  a fragment-only draw's read barrier waits in the fragment stages; a later compute read of the same texture gets a full
//            barrier first.
//   area     [B1 + B2] the UI's hint (vkn_pass_area_hint / _get): a narrowed LOAD pass, kept for the UI's next draw; clears inside / outside;
//            engine draws (outside: full; inside: DONT_CARE only when the proof covers the hint); clear / poison = full; same-set
//            targets end it, another set / NULL / a recording drop it; never-written = zero fill; a detached depth and narrowed
//            passes never discard a store, the same depth pattern attached does.
#include "../../../vkn/vkn_policy.h"
#include "../../../nr/nr_cover.h"
static nr_cv_prog g_b1_prog; static int g_b1_noprog; static uint32_t g_b1_out = 1, g_b1_ff;
static int b1_facts(const struct vkn_shader *s, vkn_pass_cover_sh *o) { (void)s; o->pos = g_b1_noprog ? NULL : (const struct nr_cv_prog *)&g_b1_prog; o->fs_outputs = g_b1_out; o->fs_facts = g_b1_ff; return 0; }
// position = (input 0 .x * [ubo] , input 0 .y, 0, w)
static void b1_prog(float w, int ubo_binding) {
    nr_cv_prog *p = &g_b1_prog; memset(p, 0, sizeof *p);
    p->ok = 1; p->in_mask = 1;
    p->ins[0] = (nr_cv_ins){NR_CV_INPUT, 0, 0, 0, 0, 0.0f}; p->ins[1] = (nr_cv_ins){NR_CV_INPUT, 0, 0, 1, 0, 0.0f};
    p->ins[2] = (nr_cv_ins){NR_CV_CONST, 0, 0, 0, 0, 0.0f}; p->ins[3] = (nr_cv_ins){NR_CV_CONST, 0, 0, 0, 0, w};
    p->n = 4; p->out[0] = 0; p->out[1] = 1; p->out[2] = 2; p->out[3] = 3; p->exact = 1;
    if (ubo_binding >= 0) {
        p->ins[4] = (nr_cv_ins){NR_CV_UBO, 0, (uint16_t)ubo_binding, 0, 0, 0.0f}; p->ins[5] = (nr_cv_ins){NR_CV_MUL, 0, 0, 4, 0, 0.0f};
        p->n = 6; p->out[0] = 5; p->exact = 0; p->nubo = 1; p->ubo[0] = (uint32_t)ubo_binding; p->ubo_bytes[0] = 4;
    }
}
static void b1_verts(const float *xy, int n) { float *v = (float *)((uint8_t *)g_vb0.cpu + 1024); memcpy(v, xy, sizeof(float) * 2 * (size_t)n); }
static const float k_b1_full[6] = {-1, -1, 3, -1, -1, 3}, k_b1_rev[6] = {-1, -1, -1, 3, 3, -1}, k_b1_small[6] = {-0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f};
static const float k_b1_quad[12] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1}, k_b1_q4[8] = {-1, -1, 1, -1, 1, 1, -1, 1};
static vkn_draw_in g_b1;
static vkn_draw_in b1_base(void) {
    vkn_draw_in in = g_in; in.blend = 0; in.ds = 0; in.raster = 0; in.vfmt = H(90, 1); in.vb[0] = H(50, 1); in.vb_off[0] = 1024; in.prim = 1; in.alpha_ref = -1.0f;
    in.viewport[0] = in.viewport[1] = 0; in.viewport[2] = 64; in.viewport[3] = 32; in.scissor[0] = in.scissor[1] = 0; in.scissor[2] = 64; in.scissor[3] = 32;
    return in;
}
// A pass begun by one draw of `in` (count vertices, arrays): the proof's outcome (-1 = none counted), the begin event (NULL = none).
static int b1_draw(vkn_draw_in *in, uint32_t count, const mk_ev **begin) {
    vkn_pass_stats3 a, b; vkn_pass_stats3_get(&a);
    const uint64_t m = mark();
    vkn_draw_arrays(in, 0, count);
    vkn_pass_stats3_get(&b);
    if (begin) *begin = nth(m, VK_NULL_HANDLE, MK_E_BEGIN_RP, 0);
    int why = -1;
    for (int i = 0; i < VKN_PCV_N; i++) if (b.cover_why[i] != a.cover_why[i]) why = why < 0 ? i : 99;
    return why;
}
static int b1_why(vkn_draw_in *in, uint32_t count) { vkn_pass_end(); return b1_draw(in, count, NULL); }
static int b1_load0(const mk_ev *e) { const mk_rp *r = rp_of(e); return r && r->natt ? (int)r->att[0].loadOp : -1; }
static int b1_store(const mk_ev *e, uint32_t k) { const mk_rp *r = rp_of(e); return r && r->natt > k ? (int)r->att[k].storeOp : -1; }
static int b1_load(const mk_ev *e, uint32_t k) { const mk_rp *r = rp_of(e); return r && r->natt > k ? (int)r->att[k].loadOp : -1; }
static void b1_pol(int load, int store, int fresh, int tilebar) {
    const vkn_pass_policy p = {(int8_t)load, (int8_t)store, (int8_t)fresh, (int8_t)tilebar, 1, 1, 3};
    vkn_pass_policy_set(&p);
}
static void b1_child(a5_res *r) {
    (void)r;
    const int v0 = mk_val_errors, e0 = mk_errors;
    vkn_pass_cover_facts_fn(b1_facts);
    static const uint8_t vf2[6] = {0, 0, 0, 0, 0, 2}, vf8[6] = {0, 0, 0, 0, 8, 2}, vfs1[12] = {0, 0, 0, 0, 0, 2, 1, 3, 0, 0, 0, 2};
    CHECK(vkn_vfmt_put(H(90, 1), vf2, 1) != 0); CHECK(vkn_vfmt_put(H(92, 1), vf8, 1) != 0); CHECK(vkn_vfmt_put(H(93, 1), vfs1, 2) != 0);
    CHECK(vkn_raster_put(H(91, 1), 0, 1, 0, 0, 0) != 0);                       // lines
    CHECK(vkn_ds_put(H(94, 1), 1, 1, 7, 0, 0, 0, 0, 0) != 0);                  // ALWAYS + write
    vkn_tex *D32 = mktex(700, EF_D32F, VKN_TC_DEPTH, 1, 64, 32, 1, 1, 700, 0), *SN = mktex(701, 9 /* R8SN: write mask 0 */, VKN_TC_COLOR, 1, 64, 32, 1, 1, 701, 0);
    CHECK(D32 && SN); if (!D32 || !SN) return;
    b1_pol(1, 0, 1, 0); b1_prog(1.0f, -1);
    vkn_pass_policy pg; int fam = -1; vkn_pass_policy_get(&pg, &fam); CHECK_EQ(pg.load, 1); CHECK_EQ(pg.store_frames, 3);
    VkCommandBuffer cb = fbegin(1);
    // ---- the whole target
    targets(g_a2, NULL, NULL); g_b1 = b1_base(); b1_verts(k_b1_full, 3);
    { vkn_pass_end(); const mk_ev *b = NULL; vkn_pass_stats3 a, c; vkn_pass_stats3_get(&a);
      CHECK_EQ(b1_draw(&g_b1, 3, &b), VKN_PCV_FULL); CHECK_EQ(b1_load0(b), VK_ATTACHMENT_LOAD_OP_DONT_CARE); CHECK(b && b->rect.extent.width == 64 && b->rect.extent.height == 32);
      vkn_pass_stats3_get(&c); CHECK_EQ(c.att_nload[VKN_PDC_COVER] - a.att_nload[VKN_PDC_COVER], 1); CHECK_EQ(c.cover_exact - a.cover_exact, 1);
      const uint64_t m = mark(); vkn_draw_arrays(&g_b1, 0, 3); CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 0); }   // the next draw: the same pass
    b1_verts(k_b1_quad, 6); CHECK_EQ(b1_why(&g_b1, 6), VKN_PCV_FULL);                                  // two triangles
    b1_verts(k_b1_q4, 4); g_b1.prim = 4; CHECK_EQ(b1_why(&g_b1, 4), VKN_PCV_FULL);                     // a quad (rewritten)
    g_b1.prim = 2; CHECK_EQ(b1_why(&g_b1, 4), VKN_PCV_FULL);                                            // a fan of the same corners
    g_b1.prim = 5; CHECK_EQ(b1_why(&g_b1, 4), VKN_PCV_PRIM);                                            // lines
    g_b1 = b1_base(); b1_verts(k_b1_full, 3);
    // ---- refusals
    b1_verts(k_b1_small, 3); CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_GEOMETRY); b1_verts(k_b1_full, 3);
    b1_prog(2.0f, -1); CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_VERTEX); b1_prog(1.0f, -1);
    g_b1_noprog = 1; CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_NOPROG); g_b1_noprog = 0;
    g_b1_out = 0; CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_OUTPUT); g_b1_out = 1;
    g_b1_ff = 1; CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_SHADER); g_b1_ff = 0;
    // (an alpha-tested draw refuses in the same check; not drawn here: its module variant needs the shader worker a forked child lacks)
    g_b1.blend = H(40, 1); CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_BLEND); g_b1.blend = H(41, 1); CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_BLEND); g_b1.blend = 0;
    g_b1.raster = H(91, 1); CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_RASTER); g_b1.raster = 0;
    g_b1.vfmt = H(92, 1); CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_INPUT); g_b1.vfmt = H(90, 1);
    g_b1.vb_off[0] = 4090; CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_BUFFER); g_b1.vb_off[0] = 1024;
    g_fxm = 2; CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_FX); g_fxm = 0;                                       // the test effect asks for discard
    g_b1.raster = H(40, 1); CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_CULLED);                                // back faces culled ...
    b1_verts(k_b1_rev, 3); CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_FULL); g_b1.raster = 0; b1_verts(k_b1_full, 3);   // ... the other winding is drawn
    g_b1.scissor[2] = 0; CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_EMPTY); g_b1.scissor[2] = 64;
    targets(SN, NULL, NULL); CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_MASK);
    targets(g_a2, NULL, g_d3); g_b1.ds = H(40, 1); CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_DS); g_b1.ds = 0;
    {   // depth: D24S8 keeps its load (stencil), a D32F written with ALWAYS begins DONT_CARE too
        const mk_ev *b = NULL; vkn_pass_end(); g_b1.ds = H(94, 1);
        CHECK_EQ(b1_draw(&g_b1, 3, &b), VKN_PCV_FULL); CHECK_EQ(b1_load(b, 0), VK_ATTACHMENT_LOAD_OP_DONT_CARE); CHECK_EQ(b1_load(b, 1), VK_ATTACHMENT_LOAD_OP_LOAD);
        targets(g_a2, NULL, D32); vkn_pass_clear_depth(D32, 1, 1.0f, 0); CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_FULL);   // (the clear folds: CLEAR, not DONT_CARE)
        vkn_pass_end(); vkn_pass_stats3 a, c; vkn_pass_stats3_get(&a);
        CHECK_EQ(b1_draw(&g_b1, 3, &b), VKN_PCV_FULL); CHECK_EQ(b1_load(b, 0), VK_ATTACHMENT_LOAD_OP_DONT_CARE); CHECK_EQ(b1_load(b, 1), VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        vkn_pass_stats3_get(&c); CHECK_EQ(c.cover_depth - a.cover_depth, 1);
        g_b1.ds = 0; targets(g_a2, NULL, NULL);
    }
    {   // positions read from a uniform block (vertex slot 2 = Vulkan binding 6 of the test's vertex shader); refused with the sub-switch off
        uint32_t off = 0; CHECK_EQ(vkn_ring_alloc(64, &off), 0); float *u = (float *)((uint8_t *)vkn_ring_cpu() + off); memset(u, 0, 64); u[0] = 1.0f; vkn_draw_ubo(0, 2, off, 64);
        b1_prog(1.0f, 6); CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_FULL);
        u[0] = 0.25f; CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_GEOMETRY);                                    // x scaled down: not covered
        b1_prog(1.0f, 7); CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_UBO);                                     // a binding the shader does not have
        u[0] = 1.0f; b1_prog(1.0f, 6); { vkn_pass_policy q = {1, 0, 1, 0, 1, 0, 3}; vkn_pass_policy_set(&q); } CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_UBO);
        b1_pol(1, 0, 1, 0); b1_prog(1.0f, -1);
    }
    // no facts (a pass the effect chain did not see) / stale facts (the draw was dropped after the chain)
    vkn_pass_end(); { vkn_pass_stats3 a, c; vkn_pass_stats3_get(&a); CHECK_EQ(vkn_pass_ready(NULL), 0); vkn_pass_stats3_get(&c); CHECK_EQ(c.cover_why[VKN_PCV_NOFACTS] - a.cover_why[VKN_PCV_NOFACTS], 1); }
    vkn_pass_end(); { vkn_draw_in x = g_b1; x.vfmt = H(93, 1); x.vb[1] = 0; vkn_draw_arrays(&x, 0, 3);
      vkn_pass_stats3 a, c; vkn_pass_stats3_get(&a); CHECK_EQ(vkn_pass_ready(NULL), 0); vkn_pass_stats3_get(&c); CHECK_EQ(c.cover_why[VKN_PCV_STALE] - a.cover_why[VKN_PCV_STALE], 1); }
    // ---- a part of the target (dynamic resolution): render area + DONT_CARE; what must stay inside it
    {
        vkn_pass_end(); const mk_ev *b = NULL; g_b1.viewport[2] = 32; g_b1.scissor[2] = 32;
        CHECK_EQ(b1_draw(&g_b1, 3, &b), VKN_PCV_AREA); CHECK_EQ(b1_load0(b), VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        CHECK(b && b->rect.offset.x == 0 && b->rect.extent.width == 32 && b->rect.extent.height == 32);
        uint64_t m = mark(); vkn_draw_arrays(&g_b1, 0, 3); CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 0);       // inside: the same pass
        vkn_pass_stats3 a, c; vkn_pass_stats3_get(&a);
        vkn_draw_in o = g_b1; o.viewport[2] = 64; o.scissor[2] = 64; b1_verts(k_b1_small, 3);         // a draw outside: the pass begins again, full, LOAD
        m = mark(); CHECK_EQ(b1_draw(&o, 3, &b), VKN_PCV_GEOMETRY); CHECK_EQ(cnt(m, cb, MK_E_END_RP), 1); CHECK_EQ(b1_load0(b), VK_ATTACHMENT_LOAD_OP_LOAD);
        CHECK(b && b->rect.extent.width == 64);
        vkn_pass_stats3_get(&c); CHECK_EQ(c.area_restart_draw - a.area_restart_draw, 1);
        b1_verts(k_b1_full, 3); vkn_pass_end(); CHECK_EQ(b1_draw(&g_b1, 3, NULL), VKN_PCV_AREA);
        const float k0[4] = {0, 0, 0, 0}; const int32_t rr[4] = {40, 0, 8, 8};                       // a clear outside the area
        m = mark(); vkn_pass_clear_color(g_a2, 0, k0, rr); CHECK_EQ(cnt(m, cb, MK_E_END_RP), 1); CHECK_EQ(cnt(m, cb, MK_E_CLEAR_ATT), 1);
        const mk_ev *ca = nth(m, cb, MK_E_CLEAR_ATT, 0); CHECK(ca && ca->cr[0].rect.offset.x == 40 && ca->cr[0].rect.extent.width == 8);
        vkn_pass_stats3_get(&c); CHECK_EQ(c.area_restart_clear - a.area_restart_clear, 1);
        vkn_pass_end(); CHECK_EQ(b1_draw(&g_b1, 3, NULL), VKN_PCV_AREA);
        m = mark(); CHECK_EQ(vkn_pass_ready(NULL), 0); CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 1);         // a draw the chain did not see
        vkn_pass_stats3_get(&c); CHECK_EQ(c.area_restart_other - a.area_restart_other, 1);
        g_b1.viewport[2] = 62; g_b1.scissor[2] = 62; CHECK_EQ(b1_why(&g_b1, 3), VKN_PCV_SMALL);       // saves < 10 %
        g_b1.viewport[2] = 32; g_b1.scissor[2] = 32; vkn_pass_end(); vkn_pass_clear_color(g_a2, 0, k0, NULL);
        CHECK_EQ(b1_draw(&g_b1, 3, &b), VKN_PCV_AREA_CLEAR); CHECK_EQ(b1_load0(b), VK_ATTACHMENT_LOAD_OP_CLEAR);
        g_b1.viewport[2] = 64; g_b1.scissor[2] = 64;
    }
    // ---- verify and poison
    b1_pol(2, 0, 1, 0);
    { vkn_pass_end(); const mk_ev *b = NULL; vkn_pass_stats3 a, c; vkn_pass_stats3_get(&a);
      CHECK_EQ(b1_draw(&g_b1, 3, &b), VKN_PCV_FULL); CHECK_EQ(b1_load0(b), VK_ATTACHMENT_LOAD_OP_LOAD);
      vkn_pass_stats3_get(&c); CHECK_EQ(c.would_dc - a.would_dc, 1); }
    b1_pol(3, 0, 1, 0);
    { vkn_pass_end(); const mk_ev *b = NULL; const uint64_t m = mark();
      CHECK_EQ(b1_draw(&g_b1, 3, &b), VKN_PCV_FULL); CHECK_EQ(b1_load0(b), VK_ATTACHMENT_LOAD_OP_DONT_CARE);
      const mk_ev *ca = nth(m, cb, MK_E_CLEAR_ATT, 0), *d = nth(m, cb, MK_E_DRAW, 0);
      CHECK(ca && b && d && ca->seq > b->seq && ca->seq < d->seq); if (ca) { CHECK_F(ca->ca[0].clearValue.color.float32[0], 1.0); CHECK_F(ca->ca[0].clearValue.color.float32[1], 0.0); } }
    // ---- fresh targets
    b1_pol(0, 0, 1, 0);
    {
        vkn_tex *F1 = mktex(702, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 702, 0), *F2 = mktex(703, EF_RGBA8, VKN_TC_COLOR, 4, 64, 32, 1, 2, 703, 0);
        vkn_tex *F0 = mktex(704, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 704, 0);
        CHECK(F1 && F2 && F0);
        if (F1 && F2 && F0) {
            g_b1.blend = H(41, 1); targets(F1, NULL, NULL); uint64_t m = mark(); const mk_ev *b = NULL; b1_draw(&g_b1, 3, &b);
            CHECK_EQ(b1_load0(b), VK_ATTACHMENT_LOAD_OP_CLEAR); CHECK_EQ(clears_of(m, F1->image), 0); if (b) CHECK_F(b->cv[0].color.float32[3], 0.0);
            target_lm(F2, 1, 0); m = mark(); b1_draw(&g_b1, 3, &b); CHECK_EQ(b1_load0(b), VK_ATTACHMENT_LOAD_OP_LOAD); CHECK_EQ(clears_of(m, F2->image), 1);   // two layers: the zero fill
            b1_pol(0, 0, 0, 0); targets(F0, NULL, NULL); m = mark(); b1_draw(&g_b1, 3, &b); CHECK_EQ(b1_load0(b), VK_ATTACHMENT_LOAD_OP_LOAD); CHECK_EQ(clears_of(m, F0->image), 1);
            g_b1.blend = 0;
        }
        // aliases of shared storage with different shapes (separate images, one identity): the other one written since -> DONT_CARE
        b1_pol(0, 0, 2, 0);
        vkn_tex *SA = shtex(705, 64, 32, 705, 0x9000, "AliasA"), *SB = shtex(706, 32, 32, 706, 0x9000, "AliasB");
        CHECK(SA && SB && SA != SB);
        if (SA && SB && SA != SB) {
            const mk_ev *b = NULL;
            targets(SA, NULL, NULL); b1_draw(&g_b1, 3, NULL); vkn_pass_end();
            vkn_draw_in hb = g_b1; hb.viewport[2] = 32; hb.scissor[2] = 32; targets(SB, NULL, NULL); b1_draw(&hb, 3, NULL); vkn_pass_end();
            vkn_pass_stats3 a, c; vkn_pass_stats3_get(&a);
            targets(SA, NULL, NULL); b1_draw(&g_b1, 3, &b); CHECK_EQ(b1_load0(b), VK_ATTACHMENT_LOAD_OP_DONT_CARE); vkn_pass_end();
            vkn_pass_stats3_get(&c); CHECK_EQ(c.att_nload[VKN_PDC_ALIAS] - a.att_nload[VKN_PDC_ALIAS], 1);
            b1_draw(&g_b1, 3, &b); CHECK_EQ(b1_load0(b), VK_ATTACHMENT_LOAD_OP_LOAD); vkn_pass_end();            // not written by the other since
            b1_pol(0, 0, 3, 0); targets(SB, NULL, NULL); b1_draw(&hb, 3, NULL); vkn_pass_end();
            targets(SA, NULL, NULL); const uint64_t m = mark(); b1_draw(&g_b1, 3, &b); CHECK_EQ(b1_load0(b), VK_ATTACHMENT_LOAD_OP_DONT_CARE);
            CHECK_EQ(cnt(m, cb, MK_E_CLEAR_ATT), 1);                                                          // poisoned
        }
        vkn_pass_end();
    }
    fsubmit(0);
    // ---- store history: S cleared every frame and drawn, never read; then read once
    {
        vkn_tex *S1 = mktex(710, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 710, 0), *S2 = mktex(711, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 711, 0);
        vkn_tex *S3 = mktex(712, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 712, 0), *SH = shtex(713, 64, 32, 713, 0xa000, "SharedStore");
        CHECK(S1 && S2 && S3 && SH); if (!S1 || !S2 || !S3 || !SH) return;
        vkn_tex *const S[4] = {S1, S2, S3, SH}; const int mode[4] = {1, 2, 3, 1};
        static int32_t rd[VKN_TEX_SLOTS]; for (int i = 0; i < VKN_TEX_SLOTS; i++) rd[i] = -1;
        for (int k = 0; k < 4; k++) {
            b1_pol(0, mode[k], 1, 0);
            int st[8]; uint64_t clr[8]; vkn_pass_stats3 a, c; vkn_pass_stats3_get(&a);
            for (int f = 0; f < 8; f++) {
                cb = fbegin(1);
                const float k0[4] = {0, 0, 0, 0};
                targets(S[k], NULL, NULL); vkn_pass_clear_color(S[k], 0, k0, NULL);
                const mk_ev *b = NULL; const uint64_t m = mark(); b1_draw(&g_b1, 3, &b); st[f] = b1_store(b, 0);
                vkn_pass_end(); clr[f] = (uint64_t)cnt(m, cb, MK_E_CLEAR_ATT);
                targets(g_b2, NULL, NULL);
                vkn_draw_in rr = g_b1; rd[0] = (int32_t)S[k]->pool_id; rr.tex[1] = f == 5 ? rd : g_ftex;   // frame 5 samples S
                b1_draw(&rr, 3, NULL); vkn_pass_end();
                fsubmit(0);
            }
            vkn_pass_stats3_get(&c);
            const int D = VK_ATTACHMENT_STORE_OP_DONT_CARE, T = VK_ATTACHMENT_STORE_OP_STORE;
            for (int f = 0; f < 3; f++) CHECK_EQ(st[f], T);                                  // the streak grows: 0, 1, 2 frames "not read"
            if (k == 0) { CHECK_EQ(st[3], D); CHECK_EQ(st[4], D); CHECK_EQ(st[5], D); CHECK_EQ(st[6], T); CHECK_EQ(st[7], T);   // frame 5's store is read: banned
                          CHECK_EQ(c.violations - a.violations, 1); CHECK_EQ(c.bans - a.bans, 1); CHECK_EQ(c.store_dc - a.store_dc, 3); }
            if (k == 1) { for (int f = 3; f < 8; f++) CHECK_EQ(st[f], T); CHECK_EQ(c.store_would - a.store_would, 3); CHECK_EQ(c.would_violations - a.would_violations, 1); CHECK_EQ(c.violations - a.violations, 0); }
            if (k == 2) { for (int f = 3; f < 8; f++) CHECK_EQ(st[f], T); CHECK_EQ(c.store_poison - a.store_poison, 3); CHECK_EQ(clr[3], 1); CHECK_EQ(clr[2], 0); CHECK_EQ(c.violations - a.violations, 1); }
            if (k == 3) { for (int f = 0; f < 8; f++) CHECK_EQ(st[f], T); CHECK(c.store_noelig - a.store_noelig >= 8); }   // shared storage: never
        }
    }
    // ---- tile barrier: a fragment-only draw's read barrier; a compute read of the same texture afterwards gets a full one
    {
        b1_pol(0, 0, 1, 1);
        vkn_tex *R = mktex(720, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 720, 0); CHECK(R != NULL); if (!R) return;
        cb = fbegin(1);
        targets(R, NULL, NULL); b1_draw(&g_b1, 3, NULL);
        targets(g_b2, NULL, NULL);
        static int32_t rt[VKN_TEX_SLOTS], ct[VKN_TEX_SLOTS]; for (int i = 0; i < VKN_TEX_SLOTS; i++) rt[i] = ct[i] = -1;
        rt[0] = 720; ct[0] = 720;
        vkn_draw_in fr = g_b1; fr.tex[1] = rt; uint64_t m = mark(); b1_draw(&fr, 3, NULL);
        const mk_ev *bar = barrier_of(m, R->image, 0);
        CHECK(bar && bar->l1 == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && bar->u[1] == (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT));
        vkn_pass_stats3 a, c; vkn_pass_stats3_get(&a);
        vkn_draw_in ci = g_in; ci.shader = H(33, 1); ci.tex[2] = ct; const vkn_img_bind ib1 = {1, H(306, 1)};
        m = mark(); CHECK_EQ(vkn_draw_dispatch(&ci, &ib1, 1, 1, 1, 1), 0);
        bar = barrier_of(m, R->image, 0);
        CHECK(bar && bar->l0 == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && bar->l1 == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && bar->u[1] == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        vkn_pass_stats3_get(&c); CHECK_EQ(c.tilebar_fix - a.tilebar_fix, 1);
        m = mark(); CHECK_EQ(vkn_draw_dispatch(&ci, &ib1, 1, 1, 1, 1), 0); CHECK(barrier_of(m, R->image, 0) == NULL);   // once
        fsubmit(0);
    }
    // ---- [B1 + B2] the UI's render-area hint (vkn_pass_area_hint / _get) and how it combines with the proof
    {
        b1_pol(1, 0, 1, 0);
        const int LD = VK_ATTACHMENT_LOAD_OP_LOAD, DC = VK_ATTACHMENT_LOAD_OP_DONT_CARE, CL = VK_ATTACHMENT_LOAD_OP_CLEAR;
        const VkRect2D H32 = {{0, 0}, {32, 32}}; const float k0[4] = {0, 0, 0, 0}; VkRect2D a; const mk_ev *b = NULL; uint64_t m;
        vkn_pass_stats3 a3, c3; vkn_pass_stats3_get(&a3);
        cb = fbegin(1);
        g_b1 = b1_base(); b1_verts(k_b1_full, 3);
        targets(g_a2, NULL, NULL); vkn_pass_end();
        CHECK_EQ(vkn_pass_area_get(&a), 0);
        CHECK_EQ(vkn_pass_area_hint(&H32), 0);
        m = mark(); CHECK_EQ(vkn_pass_ready(NULL), 0); b = nth(m, cb, MK_E_BEGIN_RP, 0);   // a UI draw (not through the chain): narrowed, LOAD
        CHECK(b && b->rect.extent.width == 32 && b->rect.extent.height == 32); CHECK_EQ(b1_load0(b), LD);
        CHECK_EQ(vkn_pass_area_get(&a), 1); CHECK(a.offset.x == 0 && a.extent.width == 32 && a.extent.height == 32);
        m = mark(); CHECK_EQ(vkn_pass_ready(NULL), 0); CHECK_EQ(cnt(m, cb, MK_E_BEGIN_RP), 0);   // the UI's next draw: the same pass
        m = mark(); vkn_pass_clear_color(g_a2, 0, k0, (const int32_t[4]){8, 8, 8, 8}); CHECK_EQ(cnt(m, cb, MK_E_END_RP), 0); CHECK_EQ(cnt(m, cb, MK_E_CLEAR_ATT), 1);
        m = mark(); vkn_pass_clear_color(g_a2, 0, k0, (const int32_t[4]){40, 0, 8, 8}); CHECK_EQ(cnt(m, cb, MK_E_END_RP), 1);   // outside: ends it, drops the hint
        { const mk_ev *ca = nth(m, cb, MK_E_CLEAR_ATT, 0); CHECK(ca && ca->cr[0].rect.offset.x == 40 && ca->cr[0].rect.extent.width == 8); }
        CHECK_EQ(vkn_pass_area_get(&a), 1); CHECK_EQ(a.extent.width, 64);   // (the region clear reopened the pass full)
        CHECK_EQ(vkn_pass_area_hint(&H32), 0);
        // an engine draw over everything begins the pass full (the hint speaks for the UI); one inside it narrowed: DONT_CARE when the
        // proof covers the whole hinted area, LOAD (counted "proof narrower than the hinted area") when it does not
        vkn_pass_end(); CHECK_EQ(b1_draw(&g_b1, 3, &b), VKN_PCV_FULL); CHECK(b && b->rect.extent.width == 64); CHECK_EQ(b1_load0(b), DC);
        g_b1.viewport[2] = 32; g_b1.scissor[2] = 32;
        vkn_pass_end(); CHECK_EQ(b1_draw(&g_b1, 3, &b), VKN_PCV_AREA); CHECK(b && b->rect.extent.width == 32); CHECK_EQ(b1_load0(b), DC);
        g_b1.viewport[2] = 16; g_b1.viewport[3] = 16; g_b1.scissor[2] = 16; g_b1.scissor[3] = 16;
        vkn_pass_end(); CHECK_EQ(b1_draw(&g_b1, 3, &b), VKN_PCV_HINT); CHECK(b && b->rect.extent.width == 32); CHECK_EQ(b1_load0(b), LD);
        vkn_draw_in o = b1_base();   // an engine draw outside the open narrowed pass: it ends, the next one is full
        m = mark(); CHECK_EQ(b1_draw(&o, 3, &b), VKN_PCV_FULL); CHECK_EQ(cnt(m, cb, MK_E_END_RP), 1); CHECK(b && b->rect.extent.width == 64);
        g_b1 = b1_base();
        // a remembered clear / a poison mode: full area
        vkn_pass_end(); vkn_pass_clear_color(g_a2, 0, k0, NULL);
        m = mark(); CHECK_EQ(vkn_pass_ready(NULL), 0); b = nth(m, cb, MK_E_BEGIN_RP, 0); CHECK(b && b->rect.extent.width == 64); CHECK_EQ(b1_load0(b), CL);
        b1_pol(3, 0, 1, 0); vkn_pass_end();
        m = mark(); CHECK_EQ(vkn_pass_ready(NULL), 0); b = nth(m, cb, MK_E_BEGIN_RP, 0); CHECK(b && b->rect.extent.width == 64);
        b1_pol(1, 0, 1, 0);
        // vkn_pass_targets of the same set ends a narrowed pass (the hint stays); another set drops the hint
        vkn_pass_end(); CHECK_EQ(vkn_pass_ready(NULL), 0); CHECK_EQ(vkn_pass_area_get(&a), 1); CHECK_EQ(a.extent.width, 32);
        m = mark(); targets(g_a2, NULL, NULL); CHECK_EQ(cnt(m, cb, MK_E_END_RP), 1); CHECK_EQ(vkn_pass_area_get(&a), 0);
        CHECK_EQ(vkn_pass_ready(NULL), 0); CHECK_EQ(vkn_pass_area_get(&a), 1); CHECK_EQ(a.extent.width, 32);
        targets(g_b2, NULL, NULL); targets(g_a2, NULL, NULL); CHECK_EQ(vkn_pass_ready(NULL), 0); CHECK_EQ(vkn_pass_area_get(&a), 1); CHECK_EQ(a.extent.width, 64);
        // NULL drops it; a recording begin drops it
        CHECK_EQ(vkn_pass_area_hint(&H32), 0); CHECK_EQ(vkn_pass_area_hint(NULL), 0); vkn_pass_end(); CHECK_EQ(vkn_pass_ready(NULL), 0); vkn_pass_area_get(&a); CHECK_EQ(a.extent.width, 64);
        CHECK_EQ(vkn_pass_area_hint(&H32), 0); vkn_pass_end(); fsubmit(0); cb = fbegin(1);
        CHECK_EQ(vkn_pass_ready(NULL), 0); vkn_pass_area_get(&a); CHECK_EQ(a.extent.width, 64);
        // a never-written target in a narrowed pass: the zero fill of all of it + LOAD (not the CLEAR to zeros of the area only)
        vkn_tex *N = mktex(730, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 730, 0); CHECK(N != NULL);
        if (N) {
            targets(N, NULL, NULL); CHECK_EQ(vkn_pass_area_hint(&H32), 0);
            m = mark(); CHECK_EQ(vkn_pass_ready(NULL), 0); b = nth(m, cb, MK_E_BEGIN_RP, 0);
            CHECK(b && b->rect.extent.width == 32); CHECK_EQ(b1_load0(b), LD); CHECK_EQ(clears_of(m, N->image), 1);
        }
        vkn_pass_stats3_get(&c3);
        CHECK(c3.hints - a3.hints >= 5); CHECK(c3.hint_passes - a3.hint_passes >= 6); CHECK_EQ(c3.hint_clear - a3.hint_clear, 1); CHECK_EQ(c3.hint_poison - a3.hint_poison, 1);
        CHECK(c3.hint_engine - a3.hint_engine >= 2); CHECK_EQ(c3.cover_why[VKN_PCV_HINT] - a3.cover_why[VKN_PCV_HINT], 1); CHECK_EQ(c3.area_restart_targets - a3.area_restart_targets, 1);
        vkn_pass_area_hint(NULL); vkn_pass_end(); fsubmit(0);
        // ---- store history with narrowed passes and a detached depth: never discarded; the same pattern attached: discarded
        b1_pol(1, 1, 1, 0);
        vkn_tex *SC = mktex(731, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 731, 0), *SD = mktex(732, EF_D32F, VKN_TC_DEPTH, 1, 64, 32, 1, 1, 732, 0);
        vkn_tex *SC2 = mktex(733, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 733, 0), *SD2 = mktex(734, EF_D32F, VKN_TC_DEPTH, 1, 64, 32, 1, 1, 734, 0);
        vkn_tex *SN = mktex(735, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 735, 0);
        CHECK(SC && SD && SC2 && SD2 && SN);
        if (SC && SD && SC2 && SD2 && SN) {
            int sd[7], sd2[7]; vkn_pass_stats3_get(&a3);
            for (int f = 0; f < 7; f++) {
                cb = fbegin(1);
                targets(SC, NULL, SD); vkn_pass_clear_depth(SD, 1, 1.0f, 0);
                m = mark(); CHECK_EQ(vkn_pass_ready(NULL), 0); sd[f] = b1_store(nth(m, cb, MK_E_BEGIN_RP, 0), 1);
                targets(SC, NULL, NULL); CHECK_EQ(vkn_pass_ready(NULL), 0);                       // detached: the same colour without the depth
                targets(SC2, NULL, SD2); vkn_pass_clear_depth(SD2, 1, 1.0f, 0);
                m = mark(); CHECK_EQ(vkn_pass_ready(NULL), 0); sd2[f] = b1_store(nth(m, cb, MK_E_BEGIN_RP, 0), 1);
                targets(g_b2, NULL, NULL); CHECK_EQ(vkn_pass_ready(NULL), 0);
                fsubmit(0);
            }
            vkn_pass_stats3_get(&c3);
            for (int f = 0; f < 7; f++) CHECK_EQ(sd[f], VK_ATTACHMENT_STORE_OP_STORE);
            for (int f = 0; f < 3; f++) CHECK_EQ(sd2[f], VK_ATTACHMENT_STORE_OP_STORE);
            for (int f = 3; f < 7; f++) CHECK_EQ(sd2[f], VK_ATTACHMENT_STORE_OP_DONT_CARE);
            CHECK_EQ(c3.store_keep_marked - a3.store_keep_marked, 1); CHECK(c3.store_keep - a3.store_keep >= 6);
            // SN in a narrowed pass every frame (never written first: the zero fill; then LOAD - a read): never discarded
            int sh[5];
            for (int f = 0; f < 5; f++) {
                cb = fbegin(1);
                targets(SN, NULL, NULL); CHECK_EQ(vkn_pass_area_hint(&H32), 0);
                m = mark(); CHECK_EQ(vkn_pass_ready(NULL), 0); const mk_ev *bb = nth(m, cb, MK_E_BEGIN_RP, 0);
                CHECK(bb && bb->rect.extent.width == 32); sh[f] = b1_store(bb, 0);
                vkn_pass_area_hint(NULL); vkn_pass_end(); fsubmit(0);
            }
            for (int f = 0; f < 5; f++) CHECK_EQ(sh[f], VK_ATTACHMENT_STORE_OP_STORE);
        }
        b1_pol(0, 0, 1, 0);
    }
    // ---- the family rule of vkn_policy.h (the shader library's xbg_gpu_family + xbg_effective_family)
    CHECK_EQ(vkn_policy_family_from(0x5143, "Adreno (TM) 750", NULL), VKN_FAM_ADRENO);
    CHECK_EQ(vkn_policy_family_from(0x5143, "Turnip Adreno (TM) 750", "mali"), VKN_FAM_MALI);
    CHECK_EQ(vkn_policy_family_from(0x5143, "x", "nobc,family-powervr"), VKN_FAM_POWERVR);
    CHECK_EQ(vkn_policy_family_from(0x5143, "x", "pvr"), VKN_FAM_POWERVR);
    CHECK_EQ(vkn_policy_family_from(0x1002, "AMD Xclipse 920", NULL), VKN_FAM_XCLIPSE);
    CHECK_EQ(vkn_policy_family_from(0x10de, "Mali-G57", "0"), VKN_FAM_MALI);
    CHECK_EQ(vkn_policy_family_from(0x10de, "something", "mali-ish"), VKN_FAM_OTHER);
    CHECK_EQ(vkn_policy_load_default(VKN_FAM_ADRENO), 0); CHECK_EQ(vkn_policy_load_default(VKN_FAM_MALI), 1); CHECK_EQ(vkn_policy_store_default(VKN_FAM_POWERVR), 1);
    CHECK_EQ(vkn_policy_fresh_default(VKN_FAM_ADRENO), 1); CHECK_EQ(vkn_policy_fresh_default(VKN_FAM_MALI), 2); CHECK_EQ(vkn_policy_tilebar_default(VKN_FAM_OTHER), 0);
    vkn_pass_policy_set(NULL); vkn_pass_policy_get(&pg, NULL); CHECK_EQ(pg.load, 0); CHECK_EQ(pg.store, 0);
    vkn_pass_cover_facts_fn(NULL);
    CHECK_EQ(mk_val_errors, v0); CHECK_EQ(mk_errors, e0);
}
static void test_b1_pass(void) {
    static const char *const env[][2] = {{"DM_VK_NATIVE_LOAD", NULL}, {NULL, NULL}};
    static a5_res R; a5_child(env, b1_child, &R);
}

// ================================================================================================================ [stage 5.2 audit] rtx_*
// The extensions of test_rt's scenario (the comment above rt_dyn_draws lists them). Objects are made by rtx_setup in the parent
// (before the forks); rtx_on sets a piece's switches (in the child, before the first recording begins), rtx_frame adds its frame part.
static vkn_tex *g_rx_ui_c, *g_rx_ui_d, *g_rx_atlas, *g_rx_sp, *g_rx_df;
static nr_mat_gate g_rx_gate; static VkShaderModule g_rx_cutm;
static uint32_t g_rx_o8, g_rx_o6, g_rx_o16, g_rx_o15;
static int32_t g_rx_ctex[VKN_TEX_SLOTS] = {346, -1, -1, 346, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}, g_rx_csmp[VKN_TEX_SLOTS] = {10, -1, -1, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static int32_t g_rx_stex[VKN_TEX_SLOTS], g_rx_ssmp[VKN_TEX_SLOTS], g_rx_ltex[VKN_TEX_SLOTS], g_rx_lsmp[VKN_TEX_SLOTS], g_rx_atex[VKN_TEX_SLOTS];
static void rtx_setup(void) {
    g_rx_ui_c = mktex(345, EF_RGBA8, VKN_TC_COLOR, 1, 64, 32, 1, 1, 345, 0); g_rx_ui_d = mktex(341, EF_D24S8, VKN_TC_DEPTH, 1, 64, 32, 1, 1, 341, 0);
    g_rx_atlas = mktex(342, 49 /* NVN DEPTH16 */, VKN_TC_DEPTH, 1, 512, 512, 1, 1, 342, 0);
    CHECK(g_rx_ui_c && g_rx_ui_d && g_rx_atlas);
    CHECK(mktex(346, EF_RGBA8, VKN_TC_SAMPLED, 1, 16, 16, 1, 1, 346, 0) != NULL);   // the casters' texture: never written (earlier tests destroy 305)
    CHECK(vkn_ds_put(H(76, 1), 1, 1, 1, 0, 0, 0, 0, 0) != 0);   // LESS with writes: the casters
    CHECK(mkshader(H(82, 1), "v300000000000001", "f300000000000001", NULL) != NULL);   // the casters' shader (test_a5_shc's H(80, 1))
    // MAT: fragment block slot 0 (bank 3) word 1 == 1.0f gates the kill (test_b5_mat's gate); variant B's cut-down vertex module
    memset(&g_rx_gate, 0, sizeof g_rx_gate);
    g_rx_gate.paths = 1; g_rx_gate.terms = 1; g_rx_gate.path[0].first = 0; g_rx_gate.path[0].count = 1; g_rx_gate.path[0].w_known = 1; g_rx_gate.path[0].w_bits = 0x3f800000u;
    g_rx_gate.term[0].bank_word = 3u << 16 | 1u; g_rx_gate.term[0].op = 180; g_rx_gate.term[0].cbits = 0x3f800000u; g_rx_gate.term[0].flags = 1 | 2;
    static const uint32_t spv[5] = {0x07230203u, 0x00010000u, 0, 1, 0};
    VkShaderModuleCreateInfo mi; memset(&mi, 0, sizeof mi); mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; mi.codeSize = sizeof spv; mi.pCode = spv;
    vkCreateShaderModule(vkn_device()->dev, &mi, NULL, &g_rx_cutm); CHECK(g_rx_cutm != VK_NULL_HANDLE);
    // LIGHTFUSE: test_draw's Sky / Light bundles (again: the first of a key is the one found), shaders of their own, Sp / Df
    const VkDescriptorType UB = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    xbg_vkn_stage *lv = mk_bundle_add("a400000000000001", 0); mk_bind(lv, 0, UB, XBG_SOURCE_SUPPORT, 0, 1392);
    xbg_vkn_stage *sk = mk_bundle_add("640a4c497bcc748b", 4);
    for (uint32_t k = 1; k <= 8; k++) mk_bind(sk, k, CIS, XBG_SOURCE_TEXTURE, k, 0);
    mk_bind(sk, 10, UB, XBG_SOURCE_UNIFORM, 10, 64); mk_bind(sk, 11, UB, XBG_SOURCE_UNIFORM, 11, 64);
    xbg_vkn_stage *li = mk_bundle_add("d236b8d5b7ffad3c", 4);
    mk_bind(li, 0, UB, XBG_SOURCE_SUPPORT, 0, 1392); mk_bind(li, 3, CIS, XBG_SOURCE_TEXTURE, 9, 0); mk_bind(li, 8, CIS, XBG_SOURCE_TEXTURE, 8, 0);
    mk_bind(li, 13, UB, XBG_SOURCE_UNIFORM, 14, 128); mk_bind(li, 16, UB, XBG_SOURCE_UNIFORM, 15, 256);
    CHECK(mkshader(H(73, 1), "a400000000000001", "640a4c497bcc748b", NULL) != NULL); CHECK(mkshader(H(74, 1), "a400000000000001", "d236b8d5b7ffad3c", NULL) != NULL);
    g_rx_sp = mktex(343, EF_RGBA16F, VKN_TC_COLOR, 1, 64, 32, 1, 1, 343, 0); g_rx_df = mktex(344, EF_RGBA16F, VKN_TC_COLOR, 1, 64, 32, 1, 1, 344, 0);
    CHECK(g_rx_sp && g_rx_df);
    uint8_t *ring = (uint8_t *)vkn_ring_cpu();
    CHECK_EQ(vkn_ring_alloc(64, &g_rx_o8), 0); CHECK_EQ(vkn_ring_alloc(64, &g_rx_o6), 0); CHECK_EQ(vkn_ring_alloc(128, &g_rx_o16), 0); CHECK_EQ(vkn_ring_alloc(256, &g_rx_o15), 0);
    memset(ring + g_rx_o8, 0x11, 64); memset(ring + g_rx_o6, 0x22, 64); memset(ring + g_rx_o16, 0x33, 128); memset(ring + g_rx_o15, 0x44, 256);
    for (int i = 0; i < VKN_TEX_SLOTS; i++) g_rx_stex[i] = g_rx_ssmp[i] = g_rx_ltex[i] = g_rx_lsmp[i] = g_rx_atex[i] = -1;
    for (int k = 1; k <= 8; k++) { g_rx_stex[k] = 300; g_rx_ssmp[k] = 10; }
    g_rx_stex[1] = 303; g_rx_ltex[8] = 343; g_rx_ltex[9] = 300; g_rx_lsmp[8] = g_rx_lsmp[9] = 10;
    g_rx_atex[0] = 342; g_rx_atex[3] = 303;
}
static void rtx_on(int ext) {
    if (ext & RTX_UI1) vkn_ui_pass_config(1, 0, 0, 0);
    if (ext & RTX_UI3) vkn_ui_pass_config(3, 0, 0, 0);
    if (ext & RTX_POL) { const vkn_pass_policy p = {1, 1, 2, 0, 1, 0, 8}; vkn_pass_policy_set(&p); }
    if (ext & RTX_MAT) { memset(&g_b5, 0, sizeof g_b5); g_b5.applied = 1; g_b5.gate = &g_rx_gate; g_b5.vsm = g_rx_cutm; g_b5.flags = NR_MAT_KILL; vkn_mat_test_setup(2, 0, b5_facts); }
    if (ext & RTX_SHC) {   // (once a process: only in a child)
        setenv("DM_VK_NATIVE_SHCACHE", "1", 1); setenv("DM_VK_NATIVE_SHFAR", "0", 1); setenv("DM_VK_NATIVE_SHCACHE_MIN", "4", 1); setenv("DM_VK_NATIVE_SHCACHE_VERIFY", "0", 1);
        unsetenv("DM_HACK_SHFAR"); unsetenv("DM_HACK_TIER");
        vkn_shc_install();
    }
    if (ext & RTX_LF) vkn_lf_test_setup(1, 1);
}
static void rtx_off(void) {
    vkn_ui_pass_config(0, 0, 0, 0); vkn_pass_policy_set(NULL); vkn_mat_test_setup(0, 0, NULL); vkn_lf_test_setup(0, 0);
}
static uint64_t rtx_metric(int ext) {   // how often the piece acted (the same in every mode: its decisions are the recording's)
    if (ext & (RTX_UI1 | RTX_UI3)) { vkn_ui_pass_stats u; vkn_ui_pass_stats_get(&u); return u.detached + u.reattach * 1000; }
    if (ext & RTX_POL) { vkn_pass_stats3 p; vkn_pass_stats3_get(&p); uint64_t n = p.att_store + p.store_dc * 1000; for (int i = 0; i < VKN_PDC_N; i++) n += p.att_nload[i] * (uint64_t)(7 + i); return n; }
    if (ext & RTX_MAT) { vkn_mat_stats m; vkn_mat_stats_get(&m); return m.no_fs + m.vs_swaps * 1000; }
    if (ext & RTX_SHC) { vkn_shc_stats c; vkn_shc_stats_get(&c); if (getenv("VKN_RTX_LOG")) { vkn_shc_census(); vkn_draw_stats d; vkn_draw_stats2 d2; vkn_draw_stats_get(&d); vkn_draw_stats2_get(&d2); printf("rtx draws %llu idx %llu drop shader %llu state %llu pipe %llu pass %llu prim %llu vb %llu ib %llu vp %llu sc_empty %llu\n", (unsigned long long)d.draws, (unsigned long long)d.indexed, (unsigned long long)d.dropped_shader, (unsigned long long)d.dropped_state, (unsigned long long)d.dropped_pipe, (unsigned long long)d.dropped_pass, (unsigned long long)d.dropped_prim, (unsigned long long)d2.dropped_vb, (unsigned long long)d2.dropped_ib, (unsigned long long)d2.dropped_viewport, (unsigned long long)d2.scissor_empty); } return c.skipped + c.copies * 1000; }
    if (ext & RTX_LF) { vkn_lf_stats l; vkn_lf_stats_get(&l); return l.fused + l.sky_skipped * 1000; }
    return 0;
}
static void rtx_frame(uint32_t f) {
    if (g_rtx & (RTX_UI1 | RTX_UI3)) {   // a GDraw epoch on its own target + DS: plain quads, a depth-tested one, a masked stencil clear, the fence
        vkn_rt_ui_targets(g_rx_ui_c, g_rx_ui_d); vkn_rt_ui_viewport(0, 0, 64, 32); vkn_rt_ui_scissor(0, 0, 64, 32);
        float verts[16] = {0, 0, 0, 0,  64, 0, 1, 0,  64, 32, 1, 1,  0, 32, 0, 1}, vsc[32], fsc[12];
        for (int i = 0; i < 32; i++) vsc[i] = (float)(i + f);
        for (int i = 0; i < 12; i++) fsc[i] = (float)i;
        vkn_ui_draw_in d; memset(&d, 0, sizeof d);
        d.vs = 0xB1; d.fs = 0xB2; d.vfmt = VKN_UI_VF_V2TC2; d.quads = 1; d.blend = 1; d.stencil_cmp = 0xff; d.tex[0] = g_t0;
        d.vsc = vsc; d.vsc_bytes = sizeof vsc; d.fsc = fsc; d.fsc_bytes = sizeof fsc; d.verts = verts; d.nverts = 4;
        vkn_rt_ui_draw(&d);
        if (f % 3 == 0) { d.ds = 2; vkn_rt_ui_draw(&d); d.ds = 0; }      // depth test: re-attach
        if (f % 5 == 1) vkn_rt_ui_clear_ds(0, 0x0f);                      // a masked stencil clear
        vkn_rt_ui_scissor(8, 4, 24, 12); vkn_rt_ui_draw(&d);
        vkn_rt_ui_pass_fence();                                           // RenderingEnd / custom-draw begin
    }
    if (g_rtx & RTX_MAT) {   // a depth-only pass: the gate word of fragment block 0 changes between draws (each value a fresh ring block)
        rt_targets(NULL, g_d3);
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t off = 0; CHECK_EQ(vkn_ring_alloc(32, &off), 0);
            float *fb = (float *)((uint8_t *)vkn_ring_cpu() + off); memset(fb, 0, 32); fb[1] = ((i + f) & 1) ? 1.0f : 0.25f;
            vkn_rt_ubo(1, 0, off, 32);
            vkn_draw_in in = g_in; in.tex[0] = NULL; in.smp[0] = NULL; in.tex[1] = rt_ftex; in.smp[1] = rt_fsmp; in.tex[2] = NULL; in.smp[2] = NULL;
            in.alpha_ref = -1.0f; in.ib = H(52, 1);
            vkn_rt_query_cond(0); vkn_rt_draw_indexed(&in, 3 * i, 3, 1);
        }
    }
    if (g_rtx & RTX_SHC) {   // the shadow atlas: 3 cascades of 12 static casters + 2 moving, then the safe point and a lighting draw
        rt_targets(NULL, g_rx_atlas); vkn_rt_pass_clear_depth(g_rx_atlas, 1, 1.0f, 0);
        vkn_rt_ubo(1, 0, g_offf0, 32);   // (the fragment block the scenario rewrites every frame: a fixed one for the casters; their texture 346 is never updated)
        const int32_t Q = 256, rc[3][2] = {{0, 0}, {Q, 0}, {0, Q}};
        for (int k = 0; k < 3; k++) {
            vkn_draw_in in = g_in; in.tex[0] = NULL; in.smp[0] = NULL; in.tex[1] = g_rx_ctex; in.smp[1] = g_rx_csmp; in.tex[2] = NULL; in.smp[2] = NULL;   // (slots of their own: static)
            in.viewport[0] = (float)rc[k][0]; in.viewport[1] = (float)rc[k][1]; in.viewport[2] = in.viewport[3] = (float)Q;
            in.scissor[0] = rc[k][0]; in.scissor[1] = rc[k][1]; in.scissor[2] = in.scissor[3] = Q; in.ds = H(76, 1); in.ib = H(53, 1); in.vb[0] = H(54, 1);   /* (buffers the scenario never updates: their versions stay) */
            in.shader = H(82, 1);
            for (int i = 0; i < 14; i++) {
                uint32_t off = 0; CHECK_EQ(vkn_ring_alloc(64, &off), 0);
                uint32_t *u = (uint32_t *)((uint8_t *)vkn_ring_cpu() + off); memset(u, 0, 64);
                u[0] = (uint32_t)k; u[1] = (uint32_t)i; u[2] = i >= 12 ? f + 1 : 0; u[3] = 0x53484331u;
                vkn_rt_ubo(0, 2, off, 64);
                in.vb_off[0] = 16u * (uint32_t)(k * 20 + i);
                vkn_rt_query_cond(0); vkn_rt_draw_indexed(&in, 3u * (uint32_t)(k * 20 + i), 3, 1);
            }
        }
        rt_targets(g_a2, g_d3);
        vkn_draw_in l = g_in; l.tex[0] = NULL; l.smp[0] = NULL; l.tex[1] = g_rx_atex; l.smp[1] = rt_fsmp; l.tex[2] = NULL; l.smp[2] = NULL; l.ib = H(52, 1);
        vkn_rt_query_cond(0); vkn_rt_draw_indexed(&l, 0, 3, 1);
    }
    if (g_rtx & RTX_LF) {   // DeferredSky into [Sp, Df], then ClusteredLightingWithSSAO into [Df]: learned, then fused
        vkn_tex *c2[VKN_PASS_COLORS] = {g_rx_sp, g_rx_df}; const uint32_t z[VKN_PASS_COLORS] = {0};
        vkn_rt_pass_targets(2, c2, z, z, NULL);
        static const float z4[4] = {0, 0, 0, 0};
        vkn_rt_pass_clear_color(g_rx_sp, 0, z4, NULL); vkn_rt_pass_clear_color(g_rx_df, 0, z4, NULL);
        vkn_rt_ubo(1, 10, g_rx_o8, 64); vkn_rt_ubo(1, 11, g_rx_o6, 64);
        vkn_draw_in sky = g_in;
        sky.shader = H(73, 1); sky.blend = 0; sky.ds = 0; sky.raster = 0; sky.vfmt = 0; sky.tex[0] = NULL; sky.smp[0] = NULL; sky.tex[1] = g_rx_stex; sky.smp[1] = g_rx_ssmp; sky.tex[2] = NULL; sky.smp[2] = NULL;
        vkn_rt_query_cond(0); vkn_rt_draw_arrays(&sky, 0, 3);
        rt_targets(g_rx_df, NULL);
        vkn_rt_ubo(1, 14, g_rx_o16, 128); vkn_rt_ubo(1, 15, g_rx_o15, 256);
        vkn_draw_in lt = sky; lt.shader = H(74, 1); lt.blend = H(41, 1); lt.tex[1] = g_rx_ltex; lt.smp[1] = g_rx_lsmp;
        vkn_rt_query_cond(0); vkn_rt_draw_arrays(&lt, 0, 3);
    }
}

// ================================================================================================================ [stage 5] technique A
// vkn_occ.c (DM_VK_NATIVE_COND 5 / 6, docs/dos2/nr/VK-OCCLUSION-ALT.md section 4). Pure parts: the box projection and the rect with its
// margin, the read-mask scan of hand-made SPIR-V (constant elements / a dynamic index / a whole-block load / no such binding), the dirt
// model over scripted sequences (unchanged, changed content, swapped order, a gone draw, a moving occluder sweeping cells, a draw
// without rect, a frame not hashed). Mock scenarios (forked children): a plain pre-pass draw + 5 objects (wall W, right occluder P,
// moving door M, hidden O1 behind W, hidden O2 behind P) with their gathers (identity camera), queries scripted by visibility,
// conditional draws in a G-buffer pass:
//   mode 5 with B = 2: at most 2 learning queries a frame, none once every object has a verdict; O1 / O2 skipped in the pre-pass and
//     in the G-buffer (4 + 3 draws instead of 6 + 5); the door moves over O2 and reveals it (frame 40): O2 drawn in that frame and
//     after; a camera change (frame 60): nothing carried, O1 learned again, skips back;
//   mode 6, true boxes: every pair queried, nothing skipped, A's would-skips agree with the queries, violations 0;
//   mode 6, a broken premise (the door's box stays where it was while its draw moves): the reveal of O2 is caught as a violation.
#include "../../../vkn/vkn_occ.h"
enum { OA_F = 90, OA_OBJ = 5 };
static int oa_facts(const vkn_shader *s) { (void)s; return 0; }
static const float k_oa_id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
typedef struct oa_out { int q, pre, gb; } oa_out;
// one frame: f = frame of the scenario, broken = the door's gather keeps its first box, cam = the camera's x scale
static void oa_frame(int f, int broken, float cam, oa_out *o) {
    const int door_moved = f >= 40;   // objects in draw order: W, P, M, O1, O2 (occluders first); x ranges in NDC, y [-0.2, 0.2]
    const float bx[OA_OBJ][2] = {{-1.0f, -0.4f}, {0.4f, 1.0f}, {door_moved && !broken ? 0.5f : -0.05f, door_moved && !broken ? 0.6f : 0.05f}, {-0.9f, -0.7f}, {0.7f, 0.9f}};
    const int vis[OA_OBJ] = {1, 1, 1, 0, door_moved ? 1 : 0};
    const uint64_t ef = (uint64_t)f + 100, B0 = 0x70000000ull, B1 = 0x70100000ull, base = (ef & 1) ? B1 : B0;
    vkn_occ_gather_bases(B0, B1);
    float vp[16]; memcpy(vp, k_oa_id, sizeof vp); vp[0] = cam;
    for (int i = 0; i < OA_OBJ; i++) { const float box[6] = {bx[i][0], -0.2f, 0.5f, bx[i][1], 0.2f, 0.6f}; CHECK_EQ(vkn_occ_gather(base + 16u * i, ef, 0x9000 + (uint64_t)i, box, vp), 0); }
    VkCommandBuffer cb = fbegin(1); targets(g_a2, NULL, g_d3); vkn_pass_clear_depth(g_d3, 3, 1.0f, 0);
    vkn_draw_in in = g_in; const uint64_t m = mark();
    mk_draw_samples = 1; in.vb_off[0] = 0; CHECK_EQ(vkn_draw_indexed(&in, 30, 3, 1), 0);   // a plain pre-pass draw (terrain): no pair, unchanged
    for (int i = 0; i < OA_OBJ; i++) {
        vkn_query_begin(base + 16u * i); mk_draw_samples = vis[i] ? 5 : 0;
        in.vb_off[0] = 16u * (uint32_t)(i + 1) + (i == 2 && door_moved ? 256u : 0u);   // the door's draw changes when it moves
        CHECK_EQ(vkn_draw_indexed(&in, 3u * (uint32_t)i, 3, 1), 0);
        vkn_query_end(base + 16u * i);
    }
    const int pre = cnt(m, cb, MK_E_DRAW_IDX);
    mk_draw_samples = 1; in.vb_off[0] = 16; targets(g_b2, NULL, g_d3);
    const uint64_t m2 = mark();
    for (int i = 0; i < OA_OBJ; i++) { vkn_query_cond(base + 16u * i); CHECK_EQ(vkn_draw_indexed(&in, 3u * (uint32_t)(10 + i), 3, 1), 0); }
    o->q = cnt(m, cb, MK_E_BEGIN_Q); o->pre = pre; o->gb = cnt(m2, cb, MK_E_DRAW_IDX);
    fsubmit(0);
}
static int g_oa_mode, g_oa_broken;
static void oa_child(a5_res *R) {
    vkn_occ_test_reset(); vkn_occ_test_fns(NULL, oa_facts);
    vkn_query_init(g_oa_mode); CHECK_EQ(vkn_query_mode(), g_oa_mode); CHECK_EQ(vkn_occ_mode(), g_oa_mode);
    static oa_out o[OA_F]; vkn_occ_stats s0, s1; vkn_occ_stats_get(&s0);
    for (int f = 0; f < OA_F; f++) oa_frame(f, g_oa_broken, f >= 60 ? 1.02f : 1.0f, &o[f]);
    vkn_occ_stats_get(&s1);
    printf("  occ-a mode %d%s (queries / pre-pass / G-buffer draws):", g_oa_mode, g_oa_broken ? " broken box" : "");
    for (int f = 0; f < OA_F; f++) if (f < 14 || (f >= 38 && f <= 46) || (f >= 58 && f <= 72) || f >= 86) printf(" %d:%d/%d/%d", f, o[f].q, o[f].pre, o[f].gb);
    printf("\n");
    vkn_occ_census();
    vkn_occ_census_v v; vkn_occ_objects(&v);
    if (g_oa_mode == VKN_COND_A) {
        CHECK_EQ(vkn_occ_budget(), 2);
        for (int f = 0; f < OA_F; f++) CHECK(o[f].q <= 2);                                                          // the budget
        for (int f = 30; f < 40; f++) { CHECK_EQ(o[f].q, 0); CHECK_EQ(o[f].pre, 4); CHECK_EQ(o[f].gb, 3); }       // O1, O2 skipped twice
        CHECK_EQ(o[40].pre, 5); CHECK_EQ(o[40].gb, 4);                                       // the door revealed O2: drawn at once
        for (int f = 41; f < 60; f++) { CHECK_EQ(o[f].pre, 5); CHECK_EQ(o[f].gb, 4); }      // O2 visible from then on, O1 still skipped
        for (int f = 50; f < 60; f++) CHECK_EQ(o[f].q, 0);                                  // ... and no more queries
        CHECK_EQ(o[60].pre, 6); CHECK_EQ(o[60].gb, 5);                                       // the camera changed: nothing carried
        for (int f = 80; f < OA_F; f++) { CHECK_EQ(o[f].pre, 5); CHECK_EQ(o[f].gb, 4); CHECK_EQ(o[f].q, 0); }   // O1 learned again
        CHECK(s1.skip_pre - s0.skip_pre > 40); CHECK_EQ(s1.violations, 0);
        CHECK(s1.whole[VKN_OCC_W_CAMERA] - s0.whole[VKN_OCC_W_CAMERA] >= 1);
        CHECK(v.hidden >= 1 && v.visible >= 3);
    } else {
        for (int f = 0; f < OA_F; f++) { CHECK_EQ(o[f].q, 5); CHECK_EQ(o[f].pre, 6); CHECK_EQ(o[f].gb, 5); }       // all queried, all drawn
        CHECK_EQ(s1.skip_pre - s0.skip_pre, 0); CHECK_EQ(s1.skip_cond - s0.skip_cond, 0);
        CHECK(s1.verify_agree - s0.verify_agree > 20);                                       // O1 / O2 judged, and right
        if (!g_oa_broken) CHECK_EQ(s1.violations - s0.violations, 0);
        else CHECK(s1.violations - s0.violations >= 1);                                      // the broken box is caught
    }
    R->model_draws = s1.violations - s0.violations;
    vkn_query_init(VKN_COND_OFF); mk_draw_samples = 1;
}
static void test_occ_a(void) {
    {   // ---- projection and rect
        const float box[6] = {-0.5f, -0.25f, 0.5f, 0.5f, 0.25f, 0.6f}; float n[4];
        CHECK_EQ(vkn_occ_project(box, k_oa_id, n), 0); CHECK_F(n[0], -0.5); CHECK_F(n[1], -0.25); CHECK_F(n[2], 0.5); CHECK_F(n[3], 0.25);
        float vp[16]; memcpy(vp, k_oa_id, sizeof vp); vp[15] = 0.0f;                         // w = 0 at every corner
        CHECK_EQ(vkn_occ_project(box, vp, n), -1);
        const float bad[6] = {1, 0, 0, 0, 1, 1}; CHECK_EQ(vkn_occ_project(bad, k_oa_id, n), -2);
        const float nd[4] = {-0.5f, -0.25f, 0.5f, 0.25f};
        const vkn_occ_rect r = vkn_occ_cells(nd, 1280, 720);                                 // + 10 % + 8 px: x [-0.6125, 0.6125], y [-0.3222, 0.3222]
        CHECK_EQ(r.kind, 1); CHECK_EQ(r.x0, 12); CHECK_EQ(r.x1, 51); CHECK_EQ(r.y0, 12); CHECK_EQ(r.y1, 23);
        const float off[4] = {1.5f, 0, 2.0f, 0.1f}; CHECK_EQ(vkn_occ_cells(off, 1280, 720).kind, 2);
        const float nan4[4] = {NAN, 0, 1, 1}; CHECK_EQ(vkn_occ_cells(nan4, 1280, 720).kind, 0);
    }
    {   // ---- read mask: %var (Binding 3) = Uniform { vec4 a[8] } (stride 16); access chains with constant elements 2 and 5
        enum { T_U = 1, C0 = 2, C2 = 3, C5 = 4, ARR = 5, ST = 6, PTR = 7, VAR = 8, PT = 9, R1 = 10, R2 = 11, DYN = 12, R3 = 13, BOUND = 14 };
        uint32_t w[80];
        const uint32_t head[] = {0x07230203u, 0x10000, 0, BOUND, 0,
            4u << 16 | 71, VAR, 33, 3,  4u << 16 | 71, ARR, 6, 16,
            4u << 16 | 43, T_U, C0, 0,  4u << 16 | 43, T_U, C2, 2,  4u << 16 | 43, T_U, C5, 5,
            3u << 16 | 30, ST, ARR,  4u << 16 | 32, PTR, 2, ST,  4u << 16 | 59, PTR, VAR, 2,
            6u << 16 | 65, PT, R1, VAR, C0, C2,  6u << 16 | 66, PT, R2, VAR, C0, C5};
        const size_t n = sizeof head / 4; memcpy(w, head, sizeof head);
        uint64_t bits[2] = {~0ull, ~0ull};
        CHECK_EQ(vkn_occ_spv_read_mask(w, n, 3, bits, 2), 1); CHECK_EQ(bits[0], (1ull << 2) | (1ull << 5)); CHECK_EQ(bits[1], 0);
        CHECK_EQ(vkn_occ_spv_read_mask(w, n, 4, bits, 2), -1);                              // no such binding
        const uint32_t dyn[] = {6u << 16 | 65, PT, R3, VAR, C0, DYN};
        memcpy(w + n, dyn, sizeof dyn); CHECK_EQ(vkn_occ_spv_read_mask(w, n + 6, 3, bits, 2), 0);   // a dynamic element: the whole block
        const uint32_t load[] = {4u << 16 | 61, ST, R3, VAR};
        memcpy(w + n, load, sizeof load); CHECK_EQ(vkn_occ_spv_read_mask(w, n + 4, 3, bits, 2), 0); // OpLoad of the whole block
        w[0] = 0; CHECK_EQ(vkn_occ_spv_read_mask(w, n, 3, bits, 2), -1);
    }
    {   // ---- the dirt model
        vkn_occ_test_reset();
        const vkn_occ_rect rA = {1, 0, 0, 9, 9}, rB = {1, 30, 0, 39, 9}, rO = {1, 50, 20, 55, 25}, unk = {0, 0, 0, 0, 0};
        vkn_occm_frame_begin(); vkn_occm_fin(11, 100, rA); vkn_occm_fin(12, 200, rB); vkn_occm_fin(13, 300, rO); vkn_occm_frame_end(1);   // 1: all new
        CHECK_EQ(vkn_occm_maxdirt(rA), 1); CHECK_EQ(vkn_occm_maxdirt(rO), 1);
        vkn_occm_frame_begin(); vkn_occm_fin(11, 100, rA); vkn_occm_fin(12, 200, rB); vkn_occm_fin(13, 300, rO); vkn_occm_frame_end(1);   // 2: same
        CHECK_EQ(vkn_occm_maxdirt(rA), 1); CHECK_EQ(vkn_occm_maxdirt(rB), 1); CHECK_EQ(vkn_occm_maxdirt(rO), 1);
        vkn_occm_frame_begin(); vkn_occm_fin(11, 100, rA); vkn_occm_fin(12, 201, rB); vkn_occm_fin(13, 300, rO); vkn_occm_frame_end(1);   // 3: B changed
        CHECK_EQ(vkn_occm_maxdirt(rB), 3); CHECK_EQ(vkn_occm_maxdirt(rA), 1); CHECK_EQ(vkn_occm_maxdirt(rO), 1);
        vkn_occm_frame_begin();                                                                // 4: A after B (swapped)
        vkn_occm_old e; int mv = 0;
        CHECK_EQ(vkn_occm_pre(12, &e, &mv), 1); CHECK_EQ(mv, 0); CHECK_EQ(e.content, 201);
        CHECK_EQ(vkn_occm_maxdirt(rA), 4);                                                     // A passed over: dirty at once
        vkn_occm_fin(12, 201, rB);
        CHECK_EQ(vkn_occm_pre(11, &e, &mv), 1); CHECK_EQ(mv, 1); vkn_occm_fin(11, 100, rA);
        vkn_occm_fin(13, 300, rO); vkn_occm_frame_end(1);
        CHECK_EQ(vkn_occm_maxdirt(rB), 3); CHECK_EQ(vkn_occm_maxdirt(rO), 1);                // B in order, unchanged: not dirtied again
        vkn_occm_frame_begin(); vkn_occm_fin(12, 201, rB); vkn_occm_fin(13, 300, rO); vkn_occm_frame_end(1);   // 5: A gone
        CHECK_EQ(vkn_occm_maxdirt(rA), 5); CHECK_EQ(vkn_occm_maxdirt(rO), 1);
        uint32_t first_o = 0;   // a moving occluder (4 cells a frame from x 30): O at x 50..55 dirty only once the rect reaches it
        for (int k = 0; k < 8; k++) {
            const vkn_occ_rect rm = {1, (uint8_t)(30 + 4 * k), 20, (uint8_t)(33 + 4 * k), 25};
            vkn_occm_frame_begin(); vkn_occm_fin(12, 201, rB); vkn_occm_fin(14, 400 + (uint64_t)k, rm); vkn_occm_fin(13, 300, rO); vkn_occm_frame_end(1);
            if (!first_o && vkn_occm_maxdirt(rO) > 1) first_o = vkn_occm_frame();
            if (rm.x1 < 50) CHECK_EQ(vkn_occm_maxdirt(rO), 1);
        }
        CHECK_EQ(first_o, 11);                                                                 // k = 5: x 50..53
        const uint32_t mf = vkn_occm_frame();
        vkn_occm_frame_begin(); vkn_occm_fin(12, 201, rB); vkn_occm_fin(13, 300, rO); vkn_occm_fin(15, 1, unk); vkn_occm_frame_end(1);   // a new draw, no rect
        CHECK_EQ(vkn_occm_maxdirt(rA), mf + 1);                                                // the whole screen
        vkn_occm_frame_begin(); vkn_occm_fin(12, 201, rB); vkn_occm_frame_end(0);             // not hashed: the next frame starts dirty
        vkn_occm_frame_begin(); CHECK_EQ(vkn_occm_maxdirt(rB), vkn_occm_frame()); vkn_occm_frame_end(1);
        vkn_occ_stats st; vkn_occ_stats_get(&st); CHECK(st.whole[VKN_OCC_W_UNHASHED] >= 1); CHECK(st.whole[VKN_OCC_W_UNKNOWN_RECT] >= 1);
        vkn_occ_test_reset();
    }
    // ---- mock scenarios (forked: the budget is read at install)
    if (!g_s1 || !g_a2 || !g_b2 || !g_d3) { CHECK(0); return; }
    static const char *const env5[][2] = {{"DM_VK_NATIVE_COND_B", "2"}, {"DM_GFX_SIM_GPU", NULL}, {NULL, NULL}};
    static a5_res R;
    g_oa_mode = VKN_COND_A; g_oa_broken = 0; a5_child(env5, oa_child, &R);
    g_oa_mode = VKN_COND_A_VERIFY; g_oa_broken = 0; a5_child(env5, oa_child, &R); CHECK_EQ(R.model_draws, 0);
    g_oa_mode = VKN_COND_A_VERIFY; g_oa_broken = 1; a5_child(env5, oa_child, &R); CHECK(R.model_draws >= 1);
}

// ================================================================================================================ [stage 5.3 V2] driver corrections
// vkn_drv.h (the old path's xbg_vendor_policy.h rules in the backend): the rules per family / driverID / switch (vkn_drv_decide over the
// vendor ids of ARM 0x13b5 / Imagination 0x1010 and the driverIDs of PanVK / RADV / the stock drivers; the SIM tokens), then each correction
// live on the mock with its configuration put in effect (vkn_drv_test_set): colour write mask 0 of a fragment stage without outputs (the
// fact from a stand-in of the shader library's), depth bias constant x2, binds forgotten after an in-pass vkCmdClearAttachments, the
// creation lock, PowerVR's static uniform buffers and the NR-08b read-only rule (layouts, dynamic offsets, set keys with static offsets,
// draws still exact), the SIM model's anisotropy; the Adreno answer (the mock's device) leaves pipelines as they were.
#include "../../../vkn/vkn_drv.h"
static const vkn_shader *g_v2_out0;   // the shader the stand-in calls "writes colour"
static int v2_facts(const vkn_shader *s) { return s == g_s1 ? 1 : s && s == g_v2_out0 ? 0 : -1; }
static const mk_ev *v2_last_pipe(uint64_t from, VkCommandBuffer cb) {
    const mk_ev *bp = NULL;
    for (int k = 0; nth(from, cb, MK_E_BIND_PIPE, k); k++) bp = nth(from, cb, MK_E_BIND_PIPE, k);
    return bp;
}
static void test_v2_drv(void) {
    static const char *const keys[] = {"DM_GFX_NOOUT_MASK", "DM_GFX_DEPTH_BIAS_X2", "DM_GFX_CLEAR_REBIND", "DM_GFX_PIPE_SERIAL", "DM_GFX_UBO_DYNAMIC", "DM_NR_UBODYN", "DM_GFX_SIM_GPU"};
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) unsetenv(keys[i]);
    // ---- the rules (vendor ids -> family as the shader library decides it, driverIDs, switches)
    const int fm = vkn_policy_family_from(0x13B5, "Mali-G57 MC2", NULL), fp = vkn_policy_family_from(0x1010, "PowerVR B-Series BXM-8-256", NULL);
    const int fa = vkn_policy_family_from(0x5143, "Adreno (TM) 750", NULL);
    CHECK_EQ(fm, VKN_FAM_MALI); CHECK_EQ(fp, VKN_FAM_POWERVR); CHECK_EQ(fa, VKN_FAM_ADRENO);
    CHECK_EQ(vkn_policy_family_from(0x5143, "Adreno (TM) 750", "mali,ubdyn8"), VKN_FAM_MALI);
    vkn_drv_cfg c;
    vkn_drv_decide(fa, VKN_DRVID_MESA_TURNIP, NULL, &c);   // the Mi 14: nothing
    CHECK(c.resolved && !c.noout && !c.bias_x2 && !c.rebind && !c.serial && c.ubo_dyn && !c.ubo_ro && c.ubodyn == 0 && !c.sim_noaniso && !c.sim_ubdyn8 && !c.from_env);
    vkn_drv_decide(fa, VKN_DRVID_QUALCOMM_PROPRIETARY, NULL, &c); CHECK(c.rebind && !c.noout && !c.bias_x2);   // stock Adreno: rebind only
    vkn_drv_decide(fm, 9 /* ARM proprietary */, NULL, &c); CHECK(c.noout && !c.bias_x2 && !c.rebind && !c.serial && c.ubo_dyn && !c.ubo_ro);
    vkn_drv_decide(fm, VKN_DRVID_MESA_PANVK, NULL, &c); CHECK(c.noout && c.bias_x2 && !c.rebind);
    vkn_drv_decide(VKN_FAM_XCLIPSE, VKN_DRVID_MESA_RADV, NULL, &c); CHECK(c.noout && c.bias_x2 && !c.serial);
    vkn_drv_decide(fp, VKN_DRVID_IMAGINATION_PROPRIETARY, NULL, &c);   // PowerVR: mask, rebind, serial, NR-08b read-only rule in force
    CHECK(c.noout && c.rebind && c.serial && c.ubo_dyn && c.ubo_ro && c.ubodyn == 1 && !c.bias_x2);
    vkn_drv_decide(fp, 0, NULL, &c); CHECK(!c.rebind && !c.bias_x2);   // driverID unknown: no driver correction
    setenv("DM_GFX_UBO_DYNAMIC", "0", 1); setenv("DM_NR_UBODYN", "0", 1);   // safe-mode step 1 on PowerVR: everything static
    vkn_drv_decide(fp, 7, NULL, &c); CHECK(!c.ubo_dyn && !c.ubo_ro && c.ubodyn == 0 && (c.from_env & (VKN_DRV_E_UBO | VKN_DRV_E_UBODYN)) == (VKN_DRV_E_UBO | VKN_DRV_E_UBODYN));
    setenv("DM_GFX_UBO_DYNAMIC", "2", 1); unsetenv("DM_NR_UBODYN");         // DOOM's spelling: the rule anywhere
    vkn_drv_decide(fa, 0, NULL, &c); CHECK(c.ubo_dyn && c.ubo_ro && c.ubodyn == 1);
    unsetenv("DM_GFX_UBO_DYNAMIC"); setenv("DM_NR_UBODYN", "3", 1);          // 3 does not exist: 0
    vkn_drv_decide(fp, 7, NULL, &c); CHECK(!c.ubo_ro && !c.ubo_dyn && c.ubodyn == 0);
    unsetenv("DM_NR_UBODYN");
    setenv("DM_GFX_NOOUT_MASK", "0", 1); setenv("DM_GFX_DEPTH_BIAS_X2", "1", 1); setenv("DM_GFX_CLEAR_REBIND", "0", 1); setenv("DM_GFX_PIPE_SERIAL", "1", 1);
    vkn_drv_decide(fp, 7, NULL, &c);
    CHECK(!c.noout && c.bias_x2 && !c.rebind && c.serial && (c.from_env & 15) == 15);
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) unsetenv(keys[i]);
    vkn_drv_decide(fp, 7, "powervr", &c); CHECK(c.sim_noaniso && !c.sim_ubdyn8);
    vkn_drv_decide(fm, 9, "mali,ubdyn8", &c); CHECK(!c.sim_noaniso && c.sim_ubdyn8);
    // ---- resolved from the device: the mock (Qualcomm vendor, Vulkan 1.1: driverID 0) = Adreno, nothing; DM_GFX_SIM_GPU=powervr = PowerVR
    vkn_drv_test_set(NULL);
    { const vkn_drv_cfg *r = vkn_drv(); CHECK(r->resolved && r->family == VKN_FAM_ADRENO && r->driver_id == 0 && !r->noout && !r->serial && r->ubo_dyn && !r->ubo_ro && !r->lock_lib); }
    setenv("DM_GFX_SIM_GPU", "powervr", 1); vkn_drv_test_set(NULL);
    { const vkn_drv_cfg *r = vkn_drv(); CHECK(r->family == VKN_FAM_POWERVR && r->noout && r->serial && r->ubo_ro && r->sim_noaniso && !r->rebind); }
    unsetenv("DM_GFX_SIM_GPU");
    // the creation lock: taken only with DM_GFX_PIPE_SERIAL (this module's own mutex without the shader library)
    vkn_drv_cfg off; vkn_drv_decide(fa, 0, NULL, &off);
    vkn_drv_test_set(&off); CHECK_EQ(vkn_drv_lock(), 0); vkn_drv_unlock(0);
    vkn_drv_cfg ser = off; ser.serial = 1; vkn_drv_test_set(&ser);
    { const int lk = vkn_drv_lock(); CHECK_EQ(lk, 1); vkn_drv_unlock(lk); vkn_drv_stats st; vkn_drv_stats_get(&st); CHECK_EQ(st.locked, 1); }
    // the colour-mask decision alone (no fragment stage / discard / no colour written / a piece's shader)
    vkn_drv_cfg mali; vkn_drv_decide(fm, 9, NULL, &mali); vkn_drv_test_set(&mali); vkn_drv_test_facts(v2_facts);
    {
        vkn_pipe_desc k; memset(&k, 0, sizeof k); vkn_pipe_src s; memset(&s, 0, sizeof s);
        s.vs = g_vs1_mod; s.fs = VK_NULL_HANDLE; k.write_mask[0] = 0xf;
        CHECK_EQ(vkn_drv_noout(&k, &s), 1);                                   // no fragment stage: masked
        s.fs = g_fs1_mod; k.fx = VKN_PIPE_FX_NO_FS; CHECK_EQ(vkn_drv_noout(&k, &s), 1); k.fx = 0;
        k.fx = VKN_PIPE_FX_DISCARD; CHECK_EQ(vkn_drv_noout(&k, &s), 0); k.fx = 0;   // nothing reaches the targets
        CHECK_EQ(vkn_drv_noout(&k, &s), 0);                                   // a piece's shader (src->shader NULL): unknown, kept
        s.shader = g_s1; CHECK_EQ(vkn_drv_noout(&k, &s), 1);
        k.write_mask[0] = 0; CHECK_EQ(vkn_drv_noout(&k, &s), 0);              // nothing written anyway
        vkn_drv_stats st; vkn_drv_stats_get(&st); CHECK_EQ(st.noout_nofs, 2); CHECK_EQ(st.noout_unknown, 1); CHECK_EQ(st.noout_masked, 1);
    }
    if (!g_s1 || !g_a2 || !g_d3) { CHECK(0); vkn_drv_test_facts(NULL); vkn_drv_test_set(NULL); return; }
    const uint32_t val0 = mk_val_errors, err0 = mk_errors;
    vkn_tex *A = mktex(760, EF_RGBA16F, VKN_TC_COLOR, 1, 64, 32, 1, 1, 760, 0), *D = mktex(761, EF_D32F, VKN_TC_DEPTH, 1, 64, 32, 1, 1, 761, 0);
    const vkn_shader *s31 = vkn_shader_get(H(31, 1)); g_v2_out0 = s31;
    CHECK(A && D && s31);
    if (!A || !D || !s31) { vkn_drv_test_facts(NULL); vkn_drv_test_set(NULL); return; }
    VkCommandBuffer cb = fbegin(1);
    vkn_draw_in in = g_in;
    // ---- DM_GFX_NOOUT_MASK on (Mali): new pipelines (a new pass class) of a shader without outputs get mask 0, the others keep theirs
    vkn_drv_test_set(&mali);
    targets(A, NULL, D);
    uint64_t m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
    mk_obj *po = pipe_of(v2_last_pipe(m, cb)); CHECK(po && po->gp.natt == 1 && po->gp.att[0].colorWriteMask == 0);
    in.shader = H(31, 1); m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
    po = pipe_of(v2_last_pipe(m, cb)); CHECK(po && po->gp.natt == 1 && po->gp.att[0].colorWriteMask == 0xf);
    { vkn_drv_stats st; vkn_drv_stats_get(&st); CHECK_EQ(st.noout_masked, 1); CHECK_EQ(st.noout_kept, 1); }
    // the Adreno answer: another pass class, the same no-output shader keeps the target's mask (the stage-5.2 pipeline)
    vkn_drv_test_set(&off);
    targets(A, NULL, NULL); in.shader = H(30, 1);
    m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
    po = pipe_of(v2_last_pipe(m, cb)); CHECK(po && po->gp.att[0].colorWriteMask == 0xf);
    { vkn_drv_stats st; vkn_drv_stats_get(&st); CHECK_EQ(st.noout_masked + st.noout_kept + st.noout_unknown + st.noout_nofs, 0); }
    // ---- DM_GFX_DEPTH_BIAS_X2: a raster state with polygon offset, built once with x1 and once (another pass class) with x2
    CHECK(vkn_raster_put(H(96, 1), 1, 0, 0, 0, 1) != 0); in.raster = H(96, 1);
    m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
    mk_obj *p1 = pipe_of(v2_last_pipe(m, cb));
    vkn_drv_cfg bias = off; bias.bias_x2 = 1; vkn_drv_test_set(&bias);
    targets(A, NULL, D); m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
    mk_obj *p2 = pipe_of(v2_last_pipe(m, cb));
    CHECK(p1 && p2 && p1 != p2);
    if (p1 && p2) {
        CHECK(p1->gp.rs.depthBiasEnable && p2->gp.rs.depthBiasEnable);
        CHECK(p1->gp.rs.depthBiasConstantFactor != 0.0f);
        CHECK_F(p2->gp.rs.depthBiasConstantFactor, 2.0 * p1->gp.rs.depthBiasConstantFactor);
        CHECK_F(p2->gp.rs.depthBiasSlopeFactor, p1->gp.rs.depthBiasSlopeFactor);   // only the constant factor
    }
    { vkn_drv_stats st; vkn_drv_stats_get(&st); CHECK_EQ(st.bias_x2, 1); }
    in.raster = g_in.raster;
    // ---- DM_GFX_CLEAR_REBIND: after an in-pass clear the next (identical) draw binds pipeline, set, streams, viewport, scissor again
    static const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    for (int on = 0; on <= 1; on++) {
        vkn_drv_cfg rb = off; rb.rebind = (uint8_t)on; vkn_drv_test_set(&rb);
        CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
        m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0); OPS(m, cb, MK_E_DRAW_IDX);   // nothing rebound
        m = mark(); vkn_pass_clear_color(A, 0, grey, NULL); CHECK_EQ(cnt(m, cb, MK_E_CLEAR_ATT), 1);
        m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
        if (on) { CHECK_EQ(cnt(m, cb, MK_E_BIND_PIPE), 1); CHECK_EQ(cnt(m, cb, MK_E_BIND_SETS), 1); CHECK(cnt(m, cb, MK_E_BIND_VB) >= 1); CHECK_EQ(cnt(m, cb, MK_E_BIND_IB), 1);
                  CHECK_EQ(cnt(m, cb, MK_E_VIEWPORT), 1); CHECK_EQ(cnt(m, cb, MK_E_SCISSOR), 1); CHECK_EQ(cnt(m, cb, MK_E_DRAW_IDX), 1); }
        else OPS(m, cb, MK_E_DRAW_IDX);
        vkn_drv_stats st; vkn_drv_stats_get(&st); CHECK_EQ(st.rebinds, on ? 1 : 0);
    }
    // ---- DM_GFX_PIPE_SERIAL: a build (new key: another pass class) is made under the lock
    vkn_drv_test_set(&ser); targets(A, NULL, g_d3);
    m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0); CHECK_EQ(cnt(m, cb, MK_E_BIND_PIPE), 1);
    { vkn_drv_stats st; vkn_drv_stats_get(&st); CHECK_EQ(st.locked, 1); }
    // ---- DM_GFX_UBO_DYNAMIC=0 (PowerVR at safe-mode step 1): every block static; the set key holds the offsets, the draws read the right block
    vkn_drv_cfg pv0; vkn_drv_decide(fp, 7, NULL, &pv0); pv0.ubo_dyn = 0; pv0.ubo_ro = 0; pv0.ubodyn = 0; pv0.serial = 0; pv0.noout = 0; pv0.rebind = 0;
    vkn_drv_test_set(&pv0);
    const vkn_shader *s87 = mkshader(H(87, 1), "v300000000000001", "f300000000000001", NULL);
    CHECK(s87 && s87->dyn_policy == 0 && s87->ndyn == 0);
    if (s87) for (uint32_t i = 0; i < s87->nbind; i++) CHECK(s87->bind[i].type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC && s87->bind[i].dyn == VKN_NO_DYN);
    { vkn_drv_stats st; vkn_drv_stats_get(&st); CHECK_EQ(st.ubo_static_shaders, 1); }
    const VkBuffer ring = vkn_ring_buffer(); const uint64_t rb = vkn_ring_base();
    uint32_t o2 = 0, o9 = 0, of0 = 0;   // this test's own blocks (other tests announce theirs)
    CHECK_EQ(vkn_ring_alloc(512, &o2), 0); CHECK_EQ(vkn_ring_alloc(128, &o9), 0); CHECK_EQ(vkn_ring_alloc(64, &of0), 0);
    vkn_draw_ubo(0, 2, o2, 512); vkn_draw_ubo(0, 9, o9, 128); vkn_draw_ubo(1, 0, of0, 64);
    in.shader = H(87, 1);
    m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
    const mk_ev *bs = nth(m, cb, MK_E_BIND_SETS, 0); CHECK(bs && bs->n == 0);
    const mk_dw *w6 = dw_of(bs, 6), *w12 = dw_of(bs, 12), *w10 = dw_of(bs, 10);
    CHECK(w6 && w6->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && w6->buf == ring && w6->off == rb + o2 && w6->range == 256);
    CHECK(w12 && w12->off == rb + o9 && w10 && w10->off == rb + of0);
    const uint64_t set_a = bs ? bs->a[1] : 0;
    uint32_t o2b = 0; CHECK_EQ(vkn_ring_alloc(512, &o2b), 0); vkn_draw_ubo(0, 2, o2b, 512);   // the engine moves slot 2: a new set with the new offset
    m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
    bs = nth(m, cb, MK_E_BIND_SETS, 0); CHECK(bs && bs->a[1] != set_a && bs->n == 0);
    w6 = dw_of(bs, 6); CHECK(w6 && w6->off == rb + o2b);
    vkn_draw_ubo(0, 2, o2, 512);
    m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);   // back: the first set again (its key: the same offsets)
    bs = nth(m, cb, MK_E_BIND_SETS, 0); CHECK(bs && bs->a[1] == set_a);
    // ---- the NR-08b read-only rule (PowerVR default): per-draw blocks dynamic only when scanned and read; support / c1 static
    const VkDescriptorType UB = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    xbg_vkn_stage *vs = mk_bundle_add("v3000000000000e1", 0);
    mk_bind(vs, 0, UB, XBG_SOURCE_SUPPORT, 0, 1392); mk_bind(vs, 1, UB, XBG_SOURCE_UNIFORM, 2, 256); mk_bind(vs, 2, UB, XBG_SOURCE_UNIFORM, 9, 0);
    mk_bind(vs, 3, UB, XBG_SOURCE_C1, 0, 32);
    vs->bindings[0].flags = 1u << 28; vs->bindings[1].flags = 1u << 28; vs->bindings[2].flags = (1u << 28) | (1u << 29); vs->bindings[3].flags = 1u << 28;   // b6 read, b12 NOT read
    vs->c1 = k_c1; vs->c1_bytes = 32; vs->input_count = 3; vs->inputs[0] = 0; vs->inputs[1] = 1; vs->inputs[2] = 4;
    vkn_drv_cfg pvr; vkn_drv_decide(fp, 7, NULL, &pvr); pvr.serial = 0; pvr.noout = 0; pvr.rebind = 0;
    vkn_drv_test_set(&pvr);
    const vkn_shader *s88 = mkshader(H(88, 1), "v3000000000000e1", "f300000000000001", NULL);   // the fragment block (b10) was never scanned
    CHECK(s88 && s88->dyn_policy == 2 && s88->ndyn == 1);
    if (s88) for (uint32_t i = 0; i < s88->nbind; i++) {
        const vkn_bind *b = &s88->bind[i];
        if (b->binding == 6) CHECK(b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC && b->dyn == 0);
        else CHECK(b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
    }
    { vkn_drv_stats st; vkn_drv_stats_get(&st); CHECK_EQ(st.ubo_ro_dynamic, 1); CHECK_EQ(st.ubo_ro_static, 2); }
    in.shader = H(88, 1);
    m = mark(); CHECK_EQ(vkn_draw_indexed(&in, 0, 3, 1), 0);
    bs = nth(m, cb, MK_E_BIND_SETS, 0); CHECK(bs && bs->n == 1 && bs->dyn[0] == o2);
    w6 = dw_of(bs, 6); w12 = dw_of(bs, 12); w10 = dw_of(bs, 10);
    CHECK(w6 && w6->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC && w6->off == rb);
    CHECK(w12 && w12->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && w12->off == rb + o9 && w10 && w10->off == rb + of0);
    vkn_draw_ubo(0, 2, g_off2, 512); vkn_draw_ubo(0, 9, g_off9, 128); vkn_draw_ubo(1, 0, g_offf0, 32);   // draw_setup's announcements again
    fsubmit(0);
    // ---- the SIM model's sampler anisotropy (xbg_sim_features NOANISO): anisotropy off, as the old path under DM_GFX_SIM_GPU=powervr
    vkn_drv_cfg na = off; na.sim_noaniso = 1; vkn_drv_test_set(&na);
    CHECK_EQ(vkn_sampler_create(H(64, 1), 40, 3, 0, 0, 0, 8, 0), 0);
    vkn_drv_test_set(&off);
    CHECK_EQ(vkn_sampler_create(H(65, 1), 41, 3, 0, 0, 0, 8, 0), 0);
    { mk_obj *o40 = mk_obj_of(VH(vkn_sampler_by_id(40))), *o41 = mk_obj_of(VH(vkn_sampler_by_id(41)));
      CHECK(o40 && !o40->sci.anisotropyEnable && o40->sci.maxAnisotropy == 1.0f); CHECK(o41 && o41->sci.anisotropyEnable && o41->sci.maxAnisotropy == 8.0f); }
    vkn_drv_census();
    CHECK_EQ(mk_val_errors, val0); CHECK_EQ(mk_errors, err0);
    vkn_drv_test_facts(NULL); vkn_drv_test_set(NULL);   // the next use resolves from the mock again (Adreno: nothing)
    CHECK(!vkn_drv()->noout && !vkn_drv()->serial);
}

// ================================================================================================================ [stage 5.3 V1] PRESKIN
// vkn_preskin.c with a stand-in library (vkn_preskin_test_setup: candidate flag, plan, slice / consumer words): the skinned draw of g_s1
// gets the consumer module and the record stream (stream 2: R32G32B32_UINT at location 0, stride 12, bound at the record's block);
// the same mesh drawn again in the frame (depth-only, then colour) reads the same record (one slice); new bones make a second record;
// the frame begins with the VERTEX_SHADER -> VERTEX_INPUT barrier and the prologue (a no-attachment pass: the slice pipeline with
// rasterizer discard / POINT_LIST over the draw's own stream 0 with its offset, its set: the bone block + the record as storage) is
// submitted before the frame; a new frame makes a new record; a 6-word record takes two groups; MAT's variant-B object keeps the
// consumer (composed); verify mode draws the engine's module and runs no slice; poison submits no prologue; off-frame recordings, a
// non-candidate, a refused plan and points are counted; the replay door; the draw-cost comparison of the effect output sees the override.
#include "../../../vkn/vkn_preskin.h"
static struct { int cand, plan_fail, nslice, ncons; uint32_t m, bone_binding; } g_v1;
static const uint32_t k_v1_spv[5] = {0x07230203u, 0x00010000u, 0, 1, 0};
static uint32_t *v1_words(size_t *n) { uint32_t *w = (uint32_t *)malloc(sizeof k_v1_spv); if (w) memcpy(w, k_v1_spv, sizeof k_v1_spv); *n = 5; return w; }
static int v1_cand(const void *s) { return s == (const void *)g_s1 ? g_v1.cand : 0; }
static int v1_plan(const void *s, uint64_t code, vkn_psk_plan *p) {
    (void)s; (void)code; memset(p, 0, sizeof *p);
    if (g_v1.plan_fail) { snprintf(p->why, sizeof p->why, "test refusal"); return 1; }
    p->ok = 1; p->m = g_v1.m; p->nvals = 1; p->val[0].hash = 0x1100u + g_v1.m; p->val[0].comps = g_v1.m; p->val[0].loc_mask = 1; p->val[0].ubo_mask = 1;
    uint32_t bb = 0; for (uint32_t i = 0; i < g_s1->nbind; i++) if (g_s1->bind[i].kind == VKN_BK_UNIFORM && g_s1->bind[i].stage == 0 && g_s1->bind[i].slot == 2) bb = g_s1->bind[i].binding;
    g_v1.bone_binding = bb;
    p->nubo = 1; p->ubo_binding[0] = bb; p->ubo_key[0] = 1u << 16 | 2;   // g_s1's vertex binding of engine uniform slot 2 (the bone block)
    p->slice_locs = 1; p->cons_locs = 0x12; p->orig_locs = 0x13; p->free_locs = 0xffedu; p->ssbo_binding = 60; p->bone_binding = bb;
    p->ops_orig = 100; p->ops_cons = 30; p->ops_slice = 60;
    return 0;
}
static int v1_slice(const vkn_psk_plan *p, uint32_t **out, size_t *n, const char **why) { (void)p; (void)why; g_v1.nslice++; *out = v1_words(n); return 0; }
static int v1_cons(const vkn_psk_plan *p, const uint32_t *pos, uint32_t **out, size_t *n, vkn_psk_layout *lay, const char **why) {
    (void)why; g_v1.ncons++; memset(lay, 0, sizeof *lay);
    uint32_t fr = p->free_locs, j = 0;   // xbg_spirv_preskin_layout: runs of consecutive record words (<= 4) at the lowest free locations
    while (j < p->m) {
        const uint32_t p0 = pos ? pos[j] : j; uint32_t w = 1;
        while (j + w < p->m && w < 4 && (pos ? pos[j + w] : j + w) == p0 + w) w++;
        const uint32_t l = (uint32_t)__builtin_ctz(fr); fr &= fr - 1;
        lay->loc[lay->groups] = l; lay->first[lay->groups] = p0; lay->width[lay->groups] = w; lay->groups++; j += w;
    }
    *out = v1_words(n); return 0;
}
static const vkn_preskin_test_lib k_v1_lib = {v1_cand, v1_plan, v1_slice, v1_cons};
static const VkVertexInputAttributeDescription *v1_attr(const mk_gp *p, uint32_t loc) {
    for (uint32_t i = 0; p && i < p->nattr; i++) if (p->attr[i].location == loc) return &p->attr[i];
    return NULL;
}
static uint32_t v1_stride(const mk_gp *p, uint32_t b) { for (uint32_t i = 0; p && i < p->nbind; i++) if (p->bind[i].binding == b) return p->bind[i].stride; return 0; }
static const mk_ev *v1_vb2(uint64_t from, VkCommandBuffer cb) {   // the last bind that covers binding 2 since `from`
    const mk_ev *r = NULL;
    for (uint64_t q = from; q < mk_seq; q++) { const mk_ev *e = mk_ev_at(q); if (e && e->cb == cb && e->op == MK_E_BIND_VB && e->u[0] <= 2 && e->u[0] + e->u[1] > 2) r = e; }
    return r;
}
static int v1_vtx_barrier(uint64_t from, VkCommandBuffer cb) {   // barriers VERTEX_SHADER -> VERTEX_INPUT in cb since `from`
    int n = 0;
    for (int k = 0; nth(from, cb, MK_E_BARRIER, k); k++) { const mk_ev *x = nth(from, cb, MK_E_BARRIER, k); if (x->u[0] == VK_PIPELINE_STAGE_VERTEX_SHADER_BIT && x->u[1] == VK_PIPELINE_STAGE_VERTEX_INPUT_BIT) n++; }
    return n;
}
static void test_v1_preskin(void) {
    if (!g_s1 || !g_d3 || !g_a2) { CHECK(0); return; }
    const int v0 = mk_val_errors, e0 = mk_errors;
    vkn_draw_in in = g_in; in.alpha_ref = -1.0f;
    memset(&g_v1, 0, sizeof g_v1); g_v1.cand = 1; g_v1.m = 3;
    vkn_preskin_test_setup(1, &k_v1_lib); CHECK_EQ(vkn_preskin_mode(), 1);
    vkn_preskin_stats s0, s1; vkn_preskin_stats_get(&s0);
    uint32_t bone2 = 0; CHECK_EQ(vkn_ring_alloc(512, &bone2), 0);
    // ---- frame 1
    uint64_t m = mark(); VkCommandBuffer cb = fbegin(1);
    CHECK_EQ(v1_vtx_barrier(m, cb), 1);
    vkn_draw_ubo(0, 2, g_off2, 512); vkn_draw_ubo(0, 9, g_off9, 128); vkn_draw_ubo(1, 0, g_offf0, 32);
    targets(NULL, NULL, g_d3);
    uint64_t m1 = mark(); vkn_draw_indexed(&in, 0, 3, 1);
    const mk_gp *p = b5_pipe(cb); const VkShaderModule cons = p ? p->module[0] : VK_NULL_HANDLE;
    CHECK(p && cons && cons != g_vs1_mod && p->nst == 2);
    {   const VkVertexInputAttributeDescription *a = v1_attr(p, 0), *a1 = v1_attr(p, 1), *a4 = v1_attr(p, 4);
        CHECK(a && a->binding == 2 && a->format == VK_FORMAT_R32G32B32_UINT && a->offset == 0); CHECK_EQ(v1_stride(p, 2), 12);
        CHECK(a1 && a1->binding == 0 && a1->offset == 12); CHECK(a4 && a4->binding == 1); }   // the engine's other attributes as they were
    const mk_ev *vb = v1_vb2(m1, cb); CHECK(vb != NULL);
    const uint64_t rbuf = vb ? vb->a[2 - vb->u[0]] : 0, roff = vb ? vb->off[2 - vb->u[0]] : 0;
    CHECK(rbuf && !(rbuf == VH(g_vb0.buf) && roff >= g_vb0.off && roff < g_vb0.off + 4096) && !(rbuf == VH(g_vb2.buf) && roff >= g_vb2.off && roff < g_vb2.off + 4096));   // (an arena block of its own)
    targets(g_a2, NULL, g_d3); vkn_draw_indexed(&in, 0, 3, 1);                     // colour pass, same mesh and bones: the same record
    p = b5_pipe(cb); CHECK(p && p->module[0] == cons);
    vkn_draw_ubo(0, 2, bone2, 512); m1 = mark(); vkn_draw_indexed(&in, 0, 3, 1);   // other bones: a second record
    vb = v1_vb2(m1, cb); CHECK(vb && (vb->a[2 - vb->u[0]] != rbuf || vb->off[2 - vb->u[0]] != roff));
    vkn_draw_ubo(0, 2, g_off2, 512);
    vkn_preskin_stats_get(&s1);
    CHECK_EQ(s1.recs[0] - s0.recs[0], 2); CHECK_EQ(s1.slices - s0.slices, 2); CHECK_EQ(s1.own[0] - s0.own[0], 1); CHECK_EQ(s1.served[0] - s0.served[0], 1);
    CHECK_EQ(s1.served[1] - s0.served[1], 2); CHECK_EQ(g_v1.ncons, 1); CHECK_EQ(g_v1.nslice, 1);
    CHECK_EQ(s1.rec_verts[0] - s0.rec_verts[0], 2 * 255);   // stream 0 from offset 16 of 4096 bytes, stride 16, a 12-byte attribute at 0
    {   // the effect output's comparison (draw cost) sees the override
        vkn_fx_out a, b; memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
        CHECK(vkn_fx_out_same(&a, &b)); a.vin.vs = cons; CHECK(!vkn_fx_out_same(&a, &b)); CHECK(!vkn_fx_out_same(&a, NULL));
        b = a; CHECK(vkn_fx_out_same(&a, &b)); b.vin.off = 4; CHECK(!vkn_fx_out_same(&a, &b));
    }
    m1 = mark(); fsubmit(0);
    {   // the prologue: its own command buffer, submitted before the frame's
        const mk_ev *sd = NULL, *fsub = nth(m1, cb, MK_E_SUBMIT, 0);
        for (uint64_t q = m; q < mk_seq; q++) { const mk_ev *e = mk_ev_at(q); if (e && e->op == MK_E_DRAW && e->cb != cb && !sd) sd = e; }
        CHECK(sd != NULL);
        if (sd) {
            CHECK_EQ(sd->u[0], 255); CHECK_EQ(sd->u[1], 1);
            const mk_ev *sub = nth(m1, sd->cb, MK_E_SUBMIT, 0); CHECK(sub && fsub && sub->seq < fsub->seq);
            CHECK_EQ(cnt(m, sd->cb, MK_E_DRAW), 2); CHECK_EQ(cnt(m, sd->cb, MK_E_BEGIN_RP), 1); CHECK_EQ(cnt(m, sd->cb, MK_E_END_RP), 1);
            const mk_obj *sp = pipe_of(nth(m, sd->cb, MK_E_BIND_PIPE, 0));
            CHECK(sp && sp->gp.nst == 1 && sp->gp.rs.rasterizerDiscardEnable && sp->gp.topo == VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
            CHECK(sp && v1_attr(&sp->gp, 0) && v1_attr(&sp->gp, 0)->format == VK_FORMAT_R32G32B32_SFLOAT && v1_attr(&sp->gp, 0)->binding == 0 && sp->gp.nattr == 1);
            const mk_ev *bv = nth(m, sd->cb, MK_E_BIND_VB, 0); CHECK(bv && bv->u[0] == 0 && bv->a[0] == VH(g_vb0.buf) && bv->off[0] == g_vb0.off + 16);
            const mk_ev *bs = nth(m, sd->cb, MK_E_BIND_SETS, 0);
            const mk_dw *wu = dw_of(bs, g_v1.bone_binding), *ws = dw_of(bs, 60);
            CHECK(wu && wu->buf == vkn_ring_buffer() && wu->off == vkn_ring_base() + g_off2 && wu->range == 256);   // the binding's 256 bytes
            CHECK(ws && ws->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && VH(ws->buf) == rbuf && ws->off == roff && ws->range == 255 * 12);
        }
    }
    vkn_preskin_stats_get(&s1); CHECK_EQ(s1.submits - s0.submits, 1);
    // ---- frame 2: the records were of frame 1
    vkn_preskin_stats_get(&s0);
    cb = fbegin(1); targets(NULL, NULL, g_d3); vkn_draw_indexed(&in, 0, 3, 1);
    p = b5_pipe(cb); CHECK(p && p->module[0] == cons);                                 // the same consumer (made once)
    vkn_preskin_stats_get(&s1); CHECK_EQ(s1.recs[0] - s0.recs[0], 1); CHECK_EQ(g_v1.ncons, 1);
    // a 6-word record: two groups (locations 0 and 2), stride 24
    vkn_preskin_test_setup(1, &k_v1_lib); g_v1.m = 6; vkn_draw_indexed(&in, 0, 3, 1);
    p = b5_pipe(cb);
    CHECK(p && v1_attr(p, 0) && v1_attr(p, 0)->format == VK_FORMAT_R32G32B32A32_UINT && v1_attr(p, 0)->binding == 2 && v1_attr(p, 0)->offset == 0);
    CHECK(p && v1_attr(p, 2) && v1_attr(p, 2)->format == VK_FORMAT_R32G32_UINT && v1_attr(p, 2)->offset == 16); CHECK_EQ(v1_stride(p, 2), 24);
    // MAT variant B in the same depth-only set: its cut-down object, the consumer as its vertex module (composed)
    vkn_preskin_stats_get(&s0);
    memset(&g_b5, 0, sizeof g_b5); g_b5.applied = 1; g_b5.vsm = g_fs1_mod;   // (any module of the mock: MAT's would-be cut-down vertex shader)
    vkn_mat_test_setup(2, 0, b5_facts);
    vkn_draw_indexed(&in, 0, 3, 1);
    p = b5_pipe(cb); CHECK(p && p->nst == 1 && p->module[0] != g_fs1_mod && p->module[0] != g_vs1_mod && v1_attr(p, 0) && v1_attr(p, 0)->binding == 2);
    vkn_mat_test_setup(0, 0, NULL);
    // refusals: a plan the split refuses, a shader that is no candidate (g_s1 with the flag off), points
    vkn_preskin_test_setup(1, &k_v1_lib); g_v1.m = 3; g_v1.plan_fail = 1; vkn_draw_indexed(&in, 0, 3, 1); vkn_draw_indexed(&in, 0, 3, 1);
    p = b5_pipe(cb); CHECK(p && p->module[0] == g_vs1_mod);
    g_v1.plan_fail = 0; vkn_preskin_test_setup(1, &k_v1_lib); g_v1.cand = 0; vkn_draw_indexed(&in, 0, 3, 1);
    p = b5_pipe(cb); CHECK(p && p->module[0] == g_vs1_mod);
    g_v1.cand = 1; vkn_preskin_test_setup(1, &k_v1_lib);
    { vkn_draw_in pt = in; pt.prim = 0; vkn_draw_arrays(&pt, 0, 3); }                 // points: refused
    {   // the replay door (vkn_shc.c replay)
        vkn_fx_ctx c; memset(&c, 0, sizeof c); c.shader = g_s1; c.depth = g_d3; c.vfmt = vkn_vfmt_id(g_in.vfmt); c.prim = 1; c.count = 3; c.instances = 1;
        vkn_fx_out o; memset(&o, 0, sizeof o);
        CHECK_EQ(vkn_preskin_apply(&in, &c, &o), 1); CHECK(o.vin.vs && o.vin.vs_id && o.vin.vfmt && o.vin.stream == 2 && o.vin.buf);
    }
    fsubmit(0);
    vkn_preskin_stats_get(&s1);
    CHECK_EQ(s1.why[VKN_PSK_W_PLAN] - s0.why[VKN_PSK_W_PLAN], 2); CHECK_EQ(s1.why[VKN_PSK_W_PRIM] - s0.why[VKN_PSK_W_PRIM], 1);
    CHECK_EQ(s1.composed - s0.composed, 1); CHECK_EQ(s1.plans_refused - s0.plans_refused, 1);
    // ---- off-frame recording: refused
    vkn_preskin_stats_get(&s0);
    cb = fbegin(0); targets(NULL, NULL, g_d3); vkn_draw_indexed(&in, 0, 3, 1); p = b5_pipe(cb); CHECK(p && p->module[0] == g_vs1_mod); fsubmit(1);
    vkn_preskin_stats_get(&s1); CHECK_EQ(s1.why[VKN_PSK_W_OFFFRAME] - s0.why[VKN_PSK_W_OFFFRAME], 1);
    // ---- verify: planned and counted, the engine's module drawn, no slice, no prologue
    vkn_preskin_test_setup(2, &k_v1_lib); CHECK_EQ(vkn_preskin_mode(), 2); vkn_preskin_stats_get(&s0);
    m = mark(); cb = fbegin(1); CHECK_EQ(v1_vtx_barrier(m, cb), 0);
    targets(NULL, NULL, g_d3); vkn_draw_indexed(&in, 0, 3, 1); vkn_draw_indexed(&in, 0, 3, 1);
    p = b5_pipe(cb); CHECK(p && p->module[0] == g_vs1_mod); fsubmit(0);
    vkn_preskin_stats_get(&s1);
    CHECK_EQ(s1.served[0] - s0.served[0], 2); CHECK_EQ(s1.recs[0] - s0.recs[0], 1); CHECK_EQ(s1.slices, s0.slices); CHECK_EQ(s1.submits, s0.submits);
    { int other = 0; for (uint64_t q = m; q < mk_seq; q++) { const mk_ev *e = mk_ev_at(q); if (e && e->op == MK_E_DRAW && e->cb != cb) other++; } CHECK_EQ(other, 0); }
    // ---- poison: the consumer drawn, the prologue never submitted
    vkn_preskin_test_setup(3, &k_v1_lib); vkn_preskin_stats_get(&s0);
    cb = fbegin(1); targets(NULL, NULL, g_d3); vkn_draw_indexed(&in, 0, 3, 1); p = b5_pipe(cb); CHECK(p && p->module[0] != g_vs1_mod); fsubmit(0);
    vkn_preskin_stats_get(&s1); CHECK_EQ(s1.slices - s0.slices, 1); CHECK_EQ(s1.submits, s0.submits);
    vkn_preskin_census();
    // ---- off: nothing of this piece in the frame
    vkn_preskin_test_setup(0, NULL); CHECK_EQ(vkn_preskin_mode(), 0);
    m = mark(); cb = fbegin(1); targets(g_a2, NULL, g_d3); vkn_draw_indexed(&in, 0, 3, 1); p = b5_pipe(cb); CHECK(p && p->module[0] == g_vs1_mod);
    CHECK_EQ(v1_vtx_barrier(m, cb), 0); fsubmit(0);
    CHECK_EQ(vkn_fx_drop("preskin"), 1); CHECK_EQ(vkn_fx_drop("preskin-late"), 1);
    targets(NULL, NULL, NULL); frames_n(4);
    CHECK_EQ(mk_val_errors, v0); CHECK_EQ(mk_errors, e0);
    CHECK(vkn_preskin_reason_name(VKN_PSK_W_LATE)[0] != '?');
}
