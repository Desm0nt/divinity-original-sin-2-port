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
//       dm/gfx/vkn/vkn_post.c dm/gfx/nb/nb_plan.c
//       dm/gfx/nvn/nvn_tex.c
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
static uint8_t g_ctl[32][CTL], g_code[32][64]; static int g_nctl;
static vkn_stage_src stage_of(const char *key) {   // a stage whose control section carries its program key (the mock's rule)
    const int i = g_nctl++; memset(g_ctl[i], 0, CTL); memcpy(g_ctl[i], key, 16); memset(g_code[i], i, 64);
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

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    mk_main_thread = pthread_self();
    mk_validate_on = 1; mk_blit = 1;
    vkn_set_mode(VKN_NATIVE);
    // install order of the host: the modules' listeners first, then the test's own
    vkn_pass_init(); vkn_draw_install(); vkn_tex_update_init();
    vkn_draw_cost_set(VKN_COST_ALL);   // [stage 5] the draw-cost shortcuts as the host sets them in mode 1 (DM_VK_NATIVE_COST default)
    vkn_rec_on_begin(lis_begin); vkn_rec_on_close(lis_close);
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
    test_a5_shc();      // [stage 5.1 A5] (forked children: the cache's switches are read once per process)
    test_a5_fx();
    test_a5_oracle();
    test_post();   // [stage 5.2 B3] last: it installs the post effect for the rest of the process
    CHECK_EQ(mk_val_errors, 0);
    CHECK_EQ(mk_errors, 0);
    CHECK_EQ(mk_qlock_depth, 0);
    if (g_fails) { printf("vkn_stage3_test: %d of %d checks FAILED\n", g_fails, g_checks); return 1; }
    printf("vkn_stage3_test: %d checks passed\n", g_checks);
    return 0;
}
