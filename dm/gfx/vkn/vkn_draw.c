// [DOS2] VK-NATIVE stage 3, draws (vkn_draw.h): the engine's draw state (its renderer cache, read by dm/host/hle_dos2_vkn_draw.c
// and passed as vkn_draw_in) recorded as Vulkan commands into the open recording (vkn_frame.h vkn_cb). No game addresses.
// Per draw, in this order (vkn_pass.h: every sample call before vkn_pass_ready, because a sample may end the open pass):
//   1. shader object (vkn_shader_get) and its stages: a missing / unusable stage drops the draw (as the translator skips it);
//   2. texture bindings of the shader's table: engine texture-pool id of the stage's slot -> view (vkn_tex_view_by_id), sampler-pool
//      id -> sampler; vkn_pass_sample for each; an unknown id, an empty slot or a feedback attachment gets a 1x1 zero placeholder of
//      the binding's shape (created once, on a one-shot submission);
//   3. vkn_pass_ready: the pass of the current targets (compatibility id, write masks per colour slot);
//   4. pipeline key (vkn_state.h) of content ids + module variants of this module's numbering (vertex retype code / alpha reference:
//      small ids 1..255, 0 = base module); vkn_pipe_get only on a key change in this recording, modules fetched only on a cache miss;
//   5. descriptor set: uniform blocks from the slots vkn_draw_ubo announced (ring offsets, dynamic where the shader's table says so),
//      SUPPORT from vkn_shader_support(viewport size), C1 from the shader's static block; the set is looked up by its content key in a
//      cache of this module (pools never reset or freed singly: the whole generation is retired, vkn_draw_forget_all);
//   6. vertex buffers of the streams the vertex format reads (+ the null stream, binding 3, for shader inputs it lacks), index buffer,
//      viewport and scissor (clamped to the pass): each only when it differs from what this recording has bound.
// Primitives without a Vulkan topology (engine 2 triangle fan, 4 quads, 7 line loop; vkn_state.h vkn_prim_topology) are drawn as
// the primitive vkn_prim_rewrite names with indices generated here (arrays: from the vertex numbers; indexed: from the engine's
// indices read through the index buffer's CPU pointer) into a transient area of this module (see "index rewrite").
// Every drop has a counter (vkn_draw_stats / vkn_draw_stats2) and the first 16 of each reason print one red line with the values.
// Single-threaded: the recording thread (vkn_draw_forget_all from elsewhere is only a request).
#ifdef DM_GAME_DOS2
#include "vkn_draw.h"
#include "vkn_fx.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "vkn_buf.h"
#include "vkn_frame.h"
#include "vkn_pass.h"
#include "vkn_prof.h"   // [stage 5.1 G] the measurement probes (vkn_prof_mark: the "draw by part" line of DM_VK_NATIVE_PROF)
#include "vkn_shader.h"
#include "vkn_state.h"
#include "vkn_tex.h"
#include "../nr/nr_draw.h"   // [stage 5, draw cost] nr_fold_index / nr_fold_vertex / nr_vb_runs (the old path's folds, pure C)
#include "vkn_sort.h"         // [rt2 R7] DM_VK_NATIVE_SORT: the graphics commands below go through vkn_sort while it is on (weak: absent = off)
extern int vkn_sort_on __attribute__((weak)); extern int vkn_sort_cap __attribute__((weak));
uint64_t vkn_pass_begins(void) __attribute__((weak));
#define SORT (&vkn_sort_on && vkn_sort_on)

#define VKN_DRAW_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"   // the red-line mark as a literal of its own

enum { REDS = 16, MAX_ELEMS = 1024, MAX_DYN = 64, SET_BUDGET = 4096, SET_TAB = 8192, POOL_SETS = 256, MAX_POOLS = 64, ZERO_UBO = 65536,
       VARIANTS = 256 };
enum { R_SHADER, R_STAGE, R_STATE, R_PIPE, R_PASS, R_PRIM, R_VB, R_IB, R_BIND, R_VIEWPORT, R_VARIANT, R_SET, R_TEX, R_SMP, R_VIEW,
       R_PLACE, R_UBO, R_IMG, R_CB, R_RW, R_N };
static uint8_t g_red[R_N];
#define DRAW_RED(reason, ...) do { if (g_red[reason] < REDS) { g_red[reason]++; vkn_logf(VKN_DRAW_RED "draw: " __VA_ARGS__); } } while (0)

static vkn_draw_stats g_st;
static vkn_draw_stats2 g_st2;

// ---- [stage 5] draw cost (vkn_draw.h DM_VK_NATIVE_COST): the bits in effect, their counters, the other modules' generations ----
static uint32_t g_cost;
static vkn_draw_cost_stats g_cs;
uint32_t vkn_tex_bind_gen(void) __attribute__((weak));
uint32_t vkn_state_handle_gen(void) __attribute__((weak));
int vkn_shader_vindex(const vkn_shader *s) __attribute__((weak));
static uint32_t (*g_tex_gen)(void), (*g_state_gen)(void); static int (*g_vindex)(const void *shader);
static int vindex_weak(const void *s) { return vkn_shader_vindex((const vkn_shader *)s); }
static int g_gens_test;   // vkn_draw_cost_gens installed stand-ins: vkn_draw_cost_set keeps them
static int g_forget_req;  // vkn_draw_forget_all from any thread: executed at the next draw
static uint32_t g_ubo_gen;
uint32_t vkn_draw_ubo_gen(void) { return g_ubo_gen; }

// ---- uniform blocks announced by the constant flushes: (stage, slot) -> ring offset / size; persist like NVN's bindings ----
typedef struct ubo_slot { uint32_t off, size; uint8_t valid, has_h; uint64_t h, hframe; } ubo_slot;   // [rt P4] has_h / h / hframe
static ubo_slot g_ubo[VKN_STAGE_SLOTS][VKN_UBO_SLOTS];
void vkn_draw_ubo(int stage, uint32_t slot, uint32_t ring_off, uint32_t size) {
    if (stage < 0 || stage >= VKN_STAGE_SLOTS || slot >= VKN_UBO_SLOTS) {
        g_st2.ubo_bad++;
        DRAW_RED(R_UBO, "uniform block announced for stage %d slot %u (ring offset %#x, %u bytes): out of range, ignored", stage, slot, ring_off, size);
        return;
    }
    ubo_slot *u = &g_ubo[stage][slot];
    if (!u->valid || u->off != ring_off || u->size != size) g_ubo_gen++;   // [stage 5, draw cost] what the slot holds changed
    u->off = ring_off; u->size = size; u->valid = 1; u->has_h = 0;
    g_st2.ubo_announce++;
}
void vkn_draw_ubo_h(int stage, uint32_t slot, uint32_t ring_off, uint32_t size, uint64_t hash) {   // [rt P4]
    vkn_draw_ubo(stage, slot, ring_off, size);
    if (stage < 0 || stage >= VKN_STAGE_SLOTS || slot >= VKN_UBO_SLOTS) return;
    ubo_slot *u = &g_ubo[stage][slot]; u->has_h = 1; u->h = hash; u->hframe = vkn_frame();
}
int vkn_draw_ubo_hash(int stage, uint32_t slot, uint32_t ring_off, uint32_t size, uint64_t *h) {
    if (stage < 0 || stage >= VKN_STAGE_SLOTS || slot >= VKN_UBO_SLOTS) return -1;
    const ubo_slot *u = &g_ubo[stage][slot];
    if (!u->valid || !u->has_h || u->off != ring_off || u->size != size || u->hframe != vkn_frame()) return -1;
    *h = u->h; return 0;
}
int vkn_draw_ubo_get(int stage, uint32_t slot, uint32_t *ring_off, uint32_t *size) {   // [stage 5] vkn_fx.h: a piece's deferred replay
    if (stage < 0 || stage >= VKN_STAGE_SLOTS || slot >= VKN_UBO_SLOTS || !g_ubo[stage][slot].valid) return -1;
    if (ring_off) *ring_off = g_ubo[stage][slot].off;
    if (size) *size = g_ubo[stage][slot].size;
    return 0;
}
// ---- [stage 5] draw effects (vkn_fx.h). g_fx: the chain's output for the draw being prepared (NULL = none: the engine's own
// draw); g_call: what the entry point was asked to draw (the chain's context). The chain is weak: a build without vkn_fx.c
// (old host tests) draws everything as the engine asked. ----
int vkn_fx_active(void) __attribute__((weak));
void vkn_fx_run(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out) __attribute__((weak));
VkSampler vkn_sampler_cmp_by_id(uint32_t sampler_id) __attribute__((weak));
int vkn_fx_out_same(const vkn_fx_out *a, const vkn_fx_out *b) __attribute__((weak));   // [stage 5, draw cost]
extern int vkn_fx_time_next __attribute__((weak));   // [rt P0] (vkn_fx.c)
static const vkn_fx_out *g_fx; static vkn_fx_out g_fx_out;
static struct { uint8_t indexed; uint32_t first, count, instances; } g_call;
static uint64_t g_fx_drop;   // draws dropped for an fx binding without a source (red)
// [stage 5.1 G] > 0 while the chain runs (prepare's vkn_fx_run): a draw that enters now is a piece's own draw (vkn_fx_bypass, e.g. the
// shadow cache's replays at its safe point), NESTED inside the engine draw being prepared. Such a draw must leave that draw's state
// alone: it rewrites g_call (prepare keeps its own copy) and would enter the query module (Q_* below: it would count as a draw of an
// open bracket, begin that bracket's query in the piece's pass, and reset the outer draw's condition / predicate / count entry) - it
// is not an engine draw, the Switch's bracket never held it.
static int g_fx_nest;
// [stage 5.5] the recording thread (vkn_rt.c): the engine's indices of the indexed draw being executed from a record, copied when the
// engine's draw was encoded (the rewrite below reads them; the buffer's bytes may be newer by now). Only for that very draw (`in`), never
// for a piece's nested draw or a later replay of a copy.
const void *vkn_draw_rt_snap; const vkn_draw_in *vkn_draw_rt_in;
// [rt P2] the record's resolution (vkn_draw.h vkn_draw_pre): only for the draw vkn_draw_rt_in, never for a piece's nested draw
const vkn_draw_pre *vkn_draw_rt_pre; int vkn_draw_rt_res_verify;
static vkn_draw_res_stats g_rs;
static const vkn_draw_pre *pre_of(const vkn_draw_in *in) { return !g_fx_nest && vkn_draw_rt_pre && vkn_draw_rt_in == in ? vkn_draw_rt_pre : NULL; }
// [shcfix 22.11] (vkn_draw.h) a piece's replay of a kept draw: the blocks that draw was drawn with
const vkn_draw_bufs *vkn_draw_nest_bufs; const vkn_draw_in *vkn_draw_nest_in;
static const vkn_draw_bufs *nest_bufs_of(const vkn_draw_in *in) { return vkn_draw_nest_bufs && vkn_draw_nest_in == in ? vkn_draw_nest_bufs : NULL; }
void vkn_draw_res_stats_get(vkn_draw_res_stats *s) { if (s) *s = g_rs; }
// [rt P0] DM_VK_NATIVE_PROF=3 pipeline-change census; [rt P3] its counters and the check switch
static vkn_draw_prof3 g_p3; static vkn_pipe_desc g_ppd; static uint16_t g_ppd_vin; static int g_ppd_ok;
void vkn_draw_prof3_get(vkn_draw_prof3 *s) { if (s) *s = g_p3; }
static vkn_draw_cost3_stats g_c3; static int g_ccheck;
void vkn_draw_cost_check(int on) { g_ccheck = on ? 1 : 0; }
void vkn_draw_cost3_stats_get(vkn_draw_cost3_stats *s) { if (s) *s = g_c3; }

// ---- what this recording has bound (forgotten at every recording begin and by vkn_draw_raw_begin) ----
static struct {
    uint32_t serial; int valid;
    int gpipe_ok; vkn_pipe_key gkey; VkPipeline gpipe;
    VkPipeline cpipe;
    VkPipelineLayout glayout, clayout;
    VkDescriptorSet gset, cset; uint32_t gndyn, cndyn, gdyn[MAX_DYN], cdyn[MAX_DYN];
    VkBuffer vb[VKN_VFMT_NULL_BINDING + 1]; VkDeviceSize vb_off[VKN_VFMT_NULL_BINDING + 1];
    VkBuffer ib; VkDeviceSize ib_off; VkIndexType ib_type;
    int vp_ok, sc_ok; VkViewport vp; VkRect2D sc;
    // [stage 5, draw cost] what the stage-4 path would have bound (unfolded): only to count the binds it would have made
    VkBuffer ovb[VKN_VFMT_NULL_BINDING + 1]; VkDeviceSize ovb_off[VKN_VFMT_NULL_BINDING + 1];
    VkBuffer oib; VkDeviceSize oib_off; VkIndexType oib_type;
} g_b;
// [stage 5, draw cost] the fold of the draw being prepared: prepare() sets it, the draw command adds it (vertexOffset / firstVertex)
static uint32_t g_vfold;
// [stage 4: queries] conditional rendering / visibility queries (vkn_query.c; weak: a build without that file draws everything)
int vkn_query_draw_enter(void) __attribute__((weak));
void vkn_query_draw_pre(VkCommandBuffer cb) __attribute__((weak));
void vkn_query_draw_post(VkCommandBuffer cb) __attribute__((weak));
#define Q_ENTER() do { if (!g_fx_nest && vkn_query_draw_enter && vkn_query_draw_enter()) return 0; } while (0)   // 1 = skipped by its condition
#define Q_PRE(cb) do { if (!g_fx_nest && vkn_query_draw_pre) vkn_query_draw_pre(cb); } while (0)     // (a piece's nested draw: not the engine's)
#define Q_POST(cb) do { if (!g_fx_nest && vkn_query_draw_post) vkn_query_draw_post(cb); } while (0)
// [/stage 4: queries]
static void forget_bound(void) { if (SORT) vkn_sort_forget(); const uint32_t s = g_b.serial; memset(&g_b, 0, sizeof g_b); g_b.serial = s; g_ppd_ok = 0; }   // [rt2 R7]
void vkn_draw_raw_begin(void) { forget_bound(); }

// ---- static helpers of this module: zero uniform block, null stream, default samplers, placeholders ----
static vkn_block g_zero, g_null; static int g_zero_ok, g_null_ok;
static VkSampler g_smp_linear, g_smp_nearest; static int g_smp_tried;
static uint32_t ubo_align(void) { const vkn_dev *d = vkn_device(); uint32_t a = d ? (uint32_t)d->props.limits.minUniformBufferOffsetAlignment : 256; return a < 16 ? 16 : a; }
static const vkn_block *zero_block(void) {
    if (!g_zero_ok) {
        g_zero_ok = vkn_block_alloc(VKN_ARENA_STATIC, ZERO_UBO, ubo_align(), &g_zero) ? -1 : 1;
        if (g_zero_ok > 0) memset(g_zero.cpu, 0, ZERO_UBO); else DRAW_RED(R_UBO, "no arena block for the 64 KiB zero uniform block");
    }
    return g_zero_ok > 0 ? &g_zero : NULL;
}
static const vkn_block *null_stream(void) {   // binding 3: {0, 0, 0, 1.0f}, NVN's / GL's value of an attribute with no array
    if (!g_null_ok) {
        g_null_ok = vkn_block_alloc(VKN_ARENA_STATIC, 16, 16, &g_null) ? -1 : 1;
        if (g_null_ok > 0) { const float v[4] = {0.0f, 0.0f, 0.0f, 1.0f}; memcpy(g_null.cpu, v, 16); }
        else DRAW_RED(R_VB, "no arena block for the null vertex stream");
    }
    return g_null_ok > 0 ? &g_null : NULL;
}
static void samplers_make(void) {
    if (g_smp_tried) return;
    g_smp_tried = 1;
    const vkn_dev *d = vkn_device(); if (!d) { g_smp_tried = 0; return; }
    VkSamplerCreateInfo si; memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO; si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; si.maxLod = VK_LOD_CLAMP_NONE; si.maxAnisotropy = 1.0f;
    if (vkCreateSampler(d->dev, &si, NULL, &g_smp_linear) != VK_SUCCESS) g_smp_linear = VK_NULL_HANDLE;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;   // integer placeholders: no filtering
    if (vkCreateSampler(d->dev, &si, NULL, &g_smp_nearest) != VK_SUCCESS) g_smp_nearest = VK_NULL_HANDLE;
    if (!g_smp_linear || !g_smp_nearest) DRAW_RED(R_SMP, "default samplers: vkCreateSampler failed (linear %p, nearest %p)", (void *)g_smp_linear, (void *)g_smp_nearest);
}

// Placeholder textures by (view type 0..6 = VkImageViewType, class 0 float / 1 sint / 2 uint / 3 depth): 1x1, every texel zero (the
// translator's null textures are zero too), SHADER_READ_ONLY_OPTIMAL, created on a one-shot submission the first time a shape is needed.
enum { PH_FLOAT, PH_SINT, PH_UINT, PH_DEPTH, PH_CLASSES };
typedef struct ph { VkImage img; VkDeviceMemory mem; VkImageView view; int8_t state; } ph;   // state 0 untried, 1 ok, -1 failed
static ph g_ph[7][PH_CLASSES];
static int ph_shape(uint32_t flags, VkImageViewType *vt, int *cls) {   // binding flags -> shape; 0 = ok
    *vt = VK_IMAGE_VIEW_TYPE_2D; *cls = PH_FLOAT;
    if (!(flags & (1u << 31))) return 0;   // no shape recorded: a plain 2D float texture
    const uint32_t dim = (flags >> 8) & 7, arr = (flags >> 12) & 1, ms = (flags >> 13) & 1, depth = (flags >> 14) & 1, scalar = (flags >> 16) & 3;
    if (ms) return -1;
    if (dim == 0) *vt = arr ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    else if (dim == 1) *vt = arr ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    else if (dim == 2) *vt = VK_IMAGE_VIEW_TYPE_3D;
    else if (dim == 3) *vt = arr ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    else return -1;   // buffer / rect / subpass: no image placeholder
    *cls = depth ? PH_DEPTH : scalar == 1 ? PH_SINT : scalar == 2 ? PH_UINT : PH_FLOAT;
    return 0;
}
static void ph_free(ph *p, const vkn_dev *d) {
    if (p->view) vkDestroyImageView(d->dev, p->view, NULL);
    if (p->img) vkDestroyImage(d->dev, p->img, NULL);
    if (p->mem) vkFreeMemory(d->dev, p->mem, NULL);
    p->view = VK_NULL_HANDLE; p->img = VK_NULL_HANDLE; p->mem = VK_NULL_HANDLE;
}
static VkImageView placeholder(uint32_t flags, int *cls_out) {
    VkImageViewType vt; int cls;
    if (ph_shape(flags, &vt, &cls)) return VK_NULL_HANDLE;
    *cls_out = cls;
    ph *p = &g_ph[vt][cls];
    if (p->state) return p->state > 0 ? p->view : VK_NULL_HANDLE;
    p->state = -1;
    const vkn_dev *d = vkn_device(); if (!d) { p->state = 0; return VK_NULL_HANDLE; }
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && !d->feat.imageCubeArray) { DRAW_RED(R_PLACE, "placeholder: cube array binding but the device lacks imageCubeArray"); return VK_NULL_HANDLE; }
    static const VkFormat fmt[PH_CLASSES] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_D16_UNORM};
    const int cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    const VkImageAspectFlags asp = cls == PH_DEPTH ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = (vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY) ? VK_IMAGE_TYPE_1D : vt == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = fmt[cls]; ci.extent.width = ci.extent.height = ci.extent.depth = 1; ci.mipLevels = 1; ci.arrayLayers = cube ? 6 : 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d->dev, &ci, NULL, &p->img) != VK_SUCCESS) { p->img = VK_NULL_HANDLE; goto fail; }
    if (vkn_image_mem(p->img, &p->mem, NULL)) { p->mem = VK_NULL_HANDLE; goto fail; }
    VkImageViewCreateInfo vi; memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = p->img; vi.viewType = vt; vi.format = fmt[cls];
    vi.subresourceRange.aspectMask = asp; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = ci.arrayLayers;
    if (vkCreateImageView(d->dev, &vi, NULL, &p->view) != VK_SUCCESS) { p->view = VK_NULL_HANDLE; goto fail; }
    VkCommandBuffer cb = vkn_once_begin(); if (!cb) goto fail;
    VkImageMemoryBarrier b; memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = p->img;
    b.subresourceRange = vi.subresourceRange; b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    if (cls == PH_DEPTH) { const VkClearDepthStencilValue z = {0.0f, 0}; vkCmdClearDepthStencilImage(cb, p->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &vi.subresourceRange); }
    else { VkClearColorValue z; memset(&z, 0, sizeof z); vkCmdClearColorImage(cb, p->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &vi.subresourceRange); }
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    if (vkn_once_end(cb)) goto fail;
    p->state = 1;
    vkn_logf("draw: placeholder texture view type %d class %d made (1x1, zero)", (int)vt, cls);
    return p->view;
fail:
    ph_free(p, d);
    DRAW_RED(R_PLACE, "placeholder texture view type %d class %d: creation failed", (int)vt, cls);
    return VK_NULL_HANDLE;
}

// ---- module variants (pipeline key bytes): this module's numbering, never reused ----
static uint64_t g_vs_code[VARIANTS]; static uint32_t g_nvs = 1;   // [0] = code 0 = the base module
static uint32_t g_fs_ref[VARIANTS]; static uint32_t g_nfs = 1;    // alpha test GREATER (NVN 5) with this reference (float bits); [0] = no test
static int variant_vs(uint64_t code) {
    if (!code) return 0;
    for (uint32_t i = 1; i < g_nvs; i++) if (g_vs_code[i] == code) return (int)i;
    if (g_nvs >= VARIANTS) return -1;
    g_vs_code[g_nvs] = code; return (int)g_nvs++;
}
static int variant_fs(float ref) {
    if (!(ref >= 0.0f)) return 0;   // < 0 (or NaN): no alpha test
    uint32_t bits; memcpy(&bits, &ref, 4);
    for (uint32_t i = 1; i < g_nfs; i++) if (g_fs_ref[i] == bits) return (int)i;
    if (g_nfs >= VARIANTS) return -1;
    g_fs_ref[g_nfs] = bits; return (int)g_nfs++;
}
// The translator's vertex retype policy (vtx_retype_mode, xbg_game.cpp): DM_GFX_VTX_RETYPE, default on everywhere but Adreno (vendor
// 0x5143), where the old path keeps the base module (Adreno passes the bits).
static int retype_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("DM_GFX_VTX_RETYPE"); const vkn_dev *d = vkn_device(); on = e && *e ? atoi(e) != 0 : d ? d->props.vendorID != 0x5143 : 0; }
    return on;
}
// The retype code as the translator's draw() builds it: for each shader input whose location the format feeds, the format's class
// (vkn_vfmt loc_class 1 float / 2 sint / 3 uint = the translator's want + 1) when it differs from the input's class (0 / 1 / 2).
static uint64_t retype_code(const vkn_shader *s, const vkn_vfmt *vf) {
    if (!vf || !retype_on()) return 0;
    uint32_t code = 0;
    for (uint32_t i = 0; i < s->ninputs && i < 16; i++) {
        const uint32_t l = s->inputs[i]; if (l >= VKN_VFMT_LOCS) continue;
        const uint32_t c = vf->loc_class[l];
        if (c && c - 1 != s->input_classes[i]) code |= c << (2 * l);
    }
    return code;
}

// ---- descriptor sets: content-keyed cache over pools of this generation ----
typedef struct set_e { uint64_t hash; uint64_t *w; uint32_t nw; VkDescriptorSet set; } set_e;
static set_e *g_sets; static uint32_t g_nsets;
static VkDescriptorPool g_pool[MAX_POOLS]; static uint32_t g_npool;
static uint64_t g_death; static int g_death_ok;
static void cost_forget(void);   // [stage 5, draw cost] the caches that name sets / pipelines / shaders go with them
static void pool_destroy(void *ctx) { const vkn_dev *d = vkn_device(); if (d) vkDestroyDescriptorPool(d->dev, (VkDescriptorPool)(uintptr_t)ctx, NULL); }
static void forget_now(void) {
    for (uint32_t i = 0; i < g_npool; i++) vkn_retire(pool_destroy, (void *)(uintptr_t)g_pool[i]);   // sets bound in flight live 3 more frames
    g_npool = 0;
    if (g_sets) for (uint32_t i = 0; i < SET_TAB; i++) { free(g_sets[i].w); g_sets[i].w = NULL; g_sets[i].nw = 0; g_sets[i].set = VK_NULL_HANDLE; }
    g_nsets = 0; g_st2.set_bytes_words = 0;
    g_b.gset = g_b.cset = VK_NULL_HANDLE; g_b.gndyn = g_b.cndyn = 0;
    g_st2.forget_all++;
    cost_forget();
}
void vkn_draw_forget_all(void) { __atomic_store_n(&g_forget_req, 1, __ATOMIC_RELEASE); }
static uint64_t death_count(void) {   // textures destroyed / swapped, samplers destroyed, shaders destroyed / replaced
    vkn_tex_stats t; vkn_shader_stats s; memset(&t, 0, sizeof t); memset(&s, 0, sizeof s);
    vkn_tex_get_stats(&t); vkn_shader_stats_get(&s);
    return t.destroyed + t.swaps + (t.samplers_made - t.samplers) + s.destroyed + s.replaced;
}
static void death_check(void) {
    const uint64_t n = death_count();
    if (g_death_ok && n != g_death && g_nsets) forget_now();
    else if (g_death_ok && n != g_death) cost_forget();   // [stage 5, draw cost] (forget_now does it too)
    g_death = n; g_death_ok = 1;
}
static VkDescriptorPool pool_new(void) {
    const vkn_dev *d = vkn_device(); if (!d || g_npool >= MAX_POOLS) return VK_NULL_HANDLE;
    const VkDescriptorPoolSize sz[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, POOL_SETS * 12}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, POOL_SETS * 12},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, POOL_SETS * 16}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, POOL_SETS * 4},
        {VK_DESCRIPTOR_TYPE_SAMPLER, POOL_SETS * 4}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64}, {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 64}};   // [P10]
    VkDescriptorPoolCreateInfo pi; memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; pi.maxSets = POOL_SETS; pi.poolSizeCount = sizeof sz / sizeof sz[0]; pi.pPoolSizes = sz;
    VkDescriptorPool p = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(d->dev, &pi, NULL, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    g_pool[g_npool++] = p; g_st2.pools++;
    return p;
}
static VkDescriptorSet set_alloc(VkDescriptorSetLayout layout) {
    const vkn_dev *d = vkn_device(); if (!d) return VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
    VkDescriptorSet s = VK_NULL_HANDLE;
    if (g_npool) { ai.descriptorPool = g_pool[g_npool - 1]; if (vkAllocateDescriptorSets(d->dev, &ai, &s) == VK_SUCCESS) return s; }
    if (g_npool >= MAX_POOLS) forget_now();   // every pool full: a new generation
    ai.descriptorPool = pool_new();
    if (!ai.descriptorPool || vkAllocateDescriptorSets(d->dev, &ai, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
    return s;
}

// One element of the shader's binding table (binding order, then array element), filled in two phases (textures before the pass
// is ready, uniform blocks after).
typedef struct elem { VkDescriptorImageInfo img; VkDescriptorBufferInfo buf; uint32_t dyn_off; } elem;
static elem g_el[MAX_ELEMS];
static uint64_t g_kw[2 + 2 * MAX_ELEMS];
static int src_of_stage(uint8_t engine_stage) { return engine_stage == VKN_STAGE_VS ? 0 : engine_stage == VKN_STAGE_FS ? 1 : engine_stage == VKN_STAGE_CS ? 2 : -1; }

// The VkImageViewType the texture piece gave tex->view (vkn_tex.c vkn_tex_create, mirrored), or a layer view (2D / 1D).
static VkImageViewType tex_view_type(const vkn_tex *t, VkImageView v) {
    for (int i = 0; i < VKN_TEX_LAYER_IDS; i++) if (t->layer_view[i] == v) return t->nvn_target == 0 || t->nvn_target == 3 ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D;
    const uint32_t tg = t->nvn_target;
    if (tg == 2) return VK_IMAGE_VIEW_TYPE_3D;
    if (tg == 0) return VK_IMAGE_VIEW_TYPE_1D;
    if (tg == 3) return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    if (tg == 4 || tg == 6) return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (tg == 8 && t->cube) return VK_IMAGE_VIEW_TYPE_CUBE;
    if (tg == 9 && t->cube) return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    return t->layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
}

// ---- [stage 5, draw cost] the texture bindings of the last graphics bind_textures (bit SET with vkn_tex_bind_gen): per element the
// slot values it read, the texture it handed to vkn_pass_sample (NULL: none) and that call's result; g_el still holds what it built
// while ok (every bind_textures clears ok first: a dispatch or another shader overwrites g_el). The counters it moved are replayed. ----
typedef struct te_rec { int32_t tid, sid; vkn_tex *t; uint8_t fb, tex; } te_rec;
static struct { int ok; const vkn_shader *s; uint32_t gen, sgen, ne; uint64_t d_unknown_tex, d_empty, d_feedback, d_mismatch, d_unknown_smp; te_rec e[MAX_ELEMS]; } g_te;

// ---- [rt P3] DM_VK_NATIVE_COST bit MIRROR (vkn_draw.h): pool id -> answer, valid while vkn_tex_bind_gen() is the generation the entry
// was filled under (read before that fill's locked lookup). Recording thread only (this module's tables). ----
int vkn_sampler_by_id_pure(void) __attribute__((weak));
enum { MT = 4096 };
static struct { uint32_t id, gen; uint8_t ok, pad[3]; VkImageView v; vkn_tex *t; } g_mt[MT];
static struct { uint32_t id, gen; uint8_t ok, pad[3]; VkSampler s; } g_ms[MT], g_mc[MT];
static int g_mir; static uint32_t g_mirgen;   // this bind_textures call: mirror on, the generation loaded at its start
static void mirror_begin(void) {
    g_mir = (g_cost & VKN_COST_MIRROR) && vkn_tex_bind_gen && vkn_sampler_by_id_pure && vkn_sampler_by_id_pure();
    if (g_mir) g_mirgen = vkn_tex_bind_gen();
}
static void check_differ(const char *what, uint32_t id) {
    g_c3.check_differ++;
    DRAW_RED(R_TEX, "DM_VK_NATIVE_COST_CHECK: %s of id %u differs from the locked lookup (the lookup used)", what, id);
}
static VkImageView tex_lookup(uint32_t id, vkn_tex **t) {
    if (!g_mir) return vkn_tex_view_by_id(id, t);
    __typeof__(g_mt[0]) *e = &g_mt[id & (MT - 1)];
    if (e->ok && e->id == id && e->gen == g_mirgen) {
        g_c3.mirror_hits++;
        if (g_ccheck) { g_c3.check_runs++; vkn_tex *x = NULL; const VkImageView v = vkn_tex_view_by_id(id, &x); if (v != e->v || x != e->t) check_differ("texture view", id); *t = x; return v; }
        *t = e->t; return e->v;
    }
    g_c3.mirror_miss++;
    const VkImageView v = vkn_tex_view_by_id(id, t);
    e->id = id; e->gen = g_mirgen; e->v = v; e->t = *t; e->ok = 1;
    return v;
}
static VkSampler smp_lookup(uint32_t id, int cmp) {
    VkSampler (*const f)(uint32_t) = cmp ? vkn_sampler_cmp_by_id : vkn_sampler_by_id;
    if (!g_mir) return f(id);
    __typeof__(g_ms[0]) *e = cmp ? &g_mc[id & (MT - 1)] : &g_ms[id & (MT - 1)];
    if (e->ok && e->id == id && e->gen == g_mirgen) {
        g_c3.mirror_hits++;
        if (g_ccheck) { g_c3.check_runs++; const VkSampler x = f(id); if (x != e->s) check_differ(cmp ? "comparison sampler" : "sampler", id); return x; }
        return e->s;
    }
    g_c3.mirror_miss++;
    const VkSampler x = f(id);
    // (a comparison twin made now bumps the generation: this entry never answers - the next call fills it again under the new one)
    e->id = id; e->gen = g_mirgen; e->s = x; e->ok = 1;
    return x;
}
// Phase 1: textures and storage images of the table. compute: the dispatch path (the pass is already ended; an attachment of the
// current target set is transitioned for reading instead of being replaced). 0 = ok, -1 = drop (counted).
static int bind_textures(const vkn_shader *s, const vkn_draw_in *in, const vkn_img_bind *img, uint32_t nimg, int compute) {
    uint32_t e = 0;
    g_te.ok = 0;   // [stage 5, draw cost] g_el is rewritten now
    mirror_begin();   // [rt P3]
    for (uint32_t i = 0; i < s->nbind; i++) {
        const vkn_bind *b = &s->bind[i];
        for (uint32_t k = 0; k < b->count; k++, e++) {
            if (e >= MAX_ELEMS) { g_st2.dropped_set++; DRAW_RED(R_SET, "shader %#llx: more than %d descriptors", (unsigned long long)s->handle, MAX_ELEMS); return -1; }
            elem *el = &g_el[e]; memset(el, 0, sizeof *el);
            te_rec *te = &g_te.e[e]; te->tex = 0; te->t = NULL; te->fb = 0;
            if (b->kind == VKN_BK_TEXTURE) {
                const int st = src_of_stage(b->stage); const uint32_t slot = b->slot + k;
                if (st < 0 || (compute ? st != 2 : st == 2)) { g_st2.dropped_bind++; DRAW_RED(R_BIND, "shader %#llx binding %u: texture of engine stage %u in a %s", (unsigned long long)s->handle, b->binding, b->stage, compute ? "dispatch" : "draw"); return -1; }
                const int32_t id = in->tex[st] && slot < VKN_TEX_SLOTS ? in->tex[st][slot] : -1;
                const int32_t sid = in->smp[st] && slot < VKN_TEX_SLOTS ? in->smp[st][slot] : -1;
                vkn_tex *t = NULL; VkImageView v = id >= 0 ? tex_lookup((uint32_t)id, &t) : VK_NULL_HANDLE;   // [rt P3] (the mirror when on)
                VkSampler sm = sid >= 0 ? smp_lookup((uint32_t)sid, 0) : VK_NULL_HANDLE;
                vkn_prof_mark_f(VKN_PROF_F_TEX_LOOK);   // [rt P0]
                te->tex = 1; te->tid = id; te->sid = sid;
                int use_ph = 0;
                if (!v || !t) {
                    use_ph = 1;
                    if (id >= 0) { g_st.unknown_tex++; DRAW_RED(R_TEX, "shader %#llx stage %d slot %u: texture-pool id %d is not known (placeholder bound)", (unsigned long long)s->handle, st, slot, id); }
                    else g_st2.empty_tex++;
                } else {
                    if (b->flags & (1u << 31)) {
                        VkImageViewType want; int cls;
                        if (!ph_shape(b->flags, &want, &cls) && want != tex_view_type(t, v)) {
                            g_st2.view_mismatch++;
                            DRAW_RED(R_VIEW, "shader %#llx stage %d slot %u: binding of view type %d, texture id %d is of view type %d (NVN target %u, %u layers): bound anyway",
                                     (unsigned long long)s->handle, st, slot, (int)want, id, (int)tex_view_type(t, v), t->nvn_target, t->layers);
                        }
                    }
                    te->t = t;
                    vkn_prof_mark_f(VKN_PROF_F_TEX_REST);   // [rt P0]
                    if (b->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {   // [P10] an attachment of the merged tile pass, read in place
                        if (compute || vkn_pass_input(t, &el->img.imageView, &el->img.imageLayout)) { g_st2.dropped_bind++; DRAW_RED(R_BIND, "shader %#llx binding %u: input attachment without its tile pass (draw dropped)", (unsigned long long)s->handle, b->binding); return -1; }
                        continue;
                    }
                    if (vkn_pass_sample(t) == 1) {
                        te->fb = 1;
                        if (compute) vkn_pass_layout(t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        else { use_ph = 1; g_st.feedback_tex++; }
                    }
                    vkn_prof_mark_f(VKN_PROF_F_TEX_SAMPLE);   // [rt P0]
                }
                if (use_ph && b->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) { g_st2.dropped_bind++; return -1; }   // [P10] (no placeholder for an input)
                if (use_ph) {
                    int cls = PH_FLOAT; v = placeholder(b->flags, &cls);
                    if (!v) { g_st2.placeholder_fail++; DRAW_RED(R_PLACE, "shader %#llx stage %d slot %u: no placeholder for binding flags %#x (draw dropped)", (unsigned long long)s->handle, st, slot, b->flags); return -1; }
                    if (cls == PH_SINT || cls == PH_UINT) { samplers_make(); sm = g_smp_nearest; }
                }
                if (!sm) {
                    if (sid >= 0 || !use_ph) { g_st.unknown_smp++; if (sid >= 0) DRAW_RED(R_SMP, "shader %#llx stage %d slot %u: sampler-pool id %d is not known (default sampler)", (unsigned long long)s->handle, st, slot, sid); }
                    samplers_make(); sm = g_smp_linear;
                }
                // [stage 5] depth comparison (vkn_fx.h): the binding's flag (a named Dref bundle) or the chain's mask picks the slot
                // sampler's comparison twin; never for a placeholder (no depth image behind it).
                if (sid >= 0 && !use_ph && vkn_sampler_cmp_by_id && ((b->flags & VKN_BF_CMP) || (g_fx && slot < 32 && (g_fx->cmp_mask[st] >> slot & 1)))) {
                    const VkSampler c = smp_lookup((uint32_t)sid, 1); if (c) sm = c;   // [rt P3]
                }
                if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) { if (!sm) { g_st2.dropped_bind++; return -1; } el->img.sampler = sm; }
                else if (b->type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) { g_st2.dropped_bind++; DRAW_RED(R_BIND, "shader %#llx binding %u: texture with descriptor type %d", (unsigned long long)s->handle, b->binding, (int)b->type); return -1; }
                el->img.imageView = v; el->img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            } else if (b->kind == VKN_BK_IMAGE) {
                const uint32_t slot = b->slot + k; vkn_tex *t = NULL;
                if (compute) for (uint32_t j = 0; j < nimg; j++) if (img[j].slot == slot) { t = vkn_tex_get(img[j].tex); break; }
                if (!t || !t->view || b->type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                    g_st2.dropped_bind++;
                    DRAW_RED(R_IMG, "shader %#llx binding %u: storage image slot %u of stage %u has no live texture (%s; %u images given)", (unsigned long long)s->handle, b->binding, slot, b->stage,
                             compute ? "dispatch" : "draw: images only in dispatches", nimg);
                    return -1;
                }
                vkn_pass_storage(t);
                el->img.imageView = t->view; el->img.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            } else if (b->kind == VKN_BK_FX_TEX) {   // [stage 5] a texture the chain gave (vkn_fx_out.tex[slot])
                const vkn_fx_tex *ft = g_fx && (uint32_t)b->slot + k < VKN_FX_TEX ? &g_fx->tex[b->slot + k] : NULL;
                if (ft && ft->tex && b->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {   // [P10]
                    if (vkn_pass_input(ft->tex, &el->img.imageView, &el->img.imageLayout)) { g_st2.dropped_bind++; g_fx_drop++; return -1; }
                    continue;
                }
                if (!ft || !ft->view || b->type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || (ft->tex && vkn_pass_sample(ft->tex) == 1)) {
                    g_st2.dropped_bind++; g_fx_drop++;
                    DRAW_RED(R_BIND, "shader %#llx (%s) binding %u: effect texture %u has no view, or is a target of this pass (draw dropped)", (unsigned long long)s->handle, s->key[0], b->binding, b->slot + k);
                    return -1;
                }
                VkSampler sm = ft->sampler; if (!sm) { samplers_make(); sm = g_smp_linear; }
                if (!sm) { g_st2.dropped_bind++; return -1; }
                el->img.sampler = sm; el->img.imageView = ft->view; el->img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            } else if (b->kind != VKN_BK_UNIFORM && b->kind != VKN_BK_SUPPORT && b->kind != VKN_BK_C1 && b->kind != VKN_BK_FX_UBO) {
                g_st2.dropped_bind++;
                DRAW_RED(R_BIND, "shader %#llx binding %u: kind %u (storage / driver table / other) has no source in the draw path", (unsigned long long)s->handle, b->binding, b->kind);
                return -1;
            }
        }
    }
    g_te.ne = e;
    return 0;
}

// [stage 5, draw cost] Phase 1 of a graphics draw. Bit SET with the texture generation: when the previous graphics bind_textures
// was for this shader object, with an all-zero effect output both times, under the same generation (no texture-pool / sampler-pool
// id has changed what it names since) and every texture binding reads the same (texture id, sampler id) from the slots, g_el holds
// exactly what bind_textures would build now - except what depends on the pass, which is asked again: vkn_pass_sample for the same
// textures in the same order (the same calls bind_textures makes; a call it makes twice is a no-op: the layout is then right); a
// different answer (a texture became / stopped being an attachment of the target set) takes the full path. Lookups have no effect,
// so leaving them out changes nothing but the counters, which are replayed.
// ---- [rt P3] DM_VK_NATIVE_COST bit SHMEM (vkn_draw.h): what g_te keeps for the last draw, kept for the last TM_WAYS draws of each of
// TM_SETS shader objects (direct-mapped by shader id), with the image half of the elements and the set key / set of that draw. ----
enum { TM_SETS = 64, TM_WAYS = 2, TM_E = 48, TM_KW = 2 + 2 * TM_E };
typedef struct te_mem {
    const vkn_shader *s; uint32_t gen, sgen, ne, ok, nw, age;
    uint64_t d_unknown_tex, d_empty, d_feedback, d_mismatch, d_unknown_smp;
    VkDescriptorSet set; uint64_t kw[TM_KW];
    te_rec e[TM_E]; VkDescriptorImageInfo img[TM_E];
} te_mem;
static te_mem g_tm[TM_SETS][TM_WAYS]; static uint32_t g_tm_age;
static te_mem *g_tm_cur;   // the entry of the draw being prepared (its set key / set go there), NULL = none
static void tm_forget(void) { for (int i = 0; i < TM_SETS; i++) for (int w = 0; w < TM_WAYS; w++) { g_tm[i][w].ok = 0; g_tm[i][w].s = NULL; g_tm[i][w].set = VK_NULL_HANDLE; g_tm[i][w].nw = 0; } g_tm_cur = NULL; }
static te_mem *tm_find(const vkn_shader *s) {
    te_mem *row = g_tm[(s->id ^ s->id >> 6) & (TM_SETS - 1)];
    for (int w = 0; w < TM_WAYS; w++) if (row[w].ok && row[w].s == s) return &row[w];
    return NULL;
}
static te_mem *tm_slot(const vkn_shader *s) {   // the way to (re)fill for s: its own, else the older one
    te_mem *row = g_tm[(s->id ^ s->id >> 6) & (TM_SETS - 1)];
    for (int w = 0; w < TM_WAYS; w++) if (row[w].ok && row[w].s == s) return &row[w];
    te_mem *o = &row[0];
    for (int w = 1; w < TM_WAYS; w++) if (!row[w].ok || (o->ok && row[w].age < o->age)) o = &row[w];
    return o;
}
static int tex_bind_mem(const vkn_shader *s, const vkn_draw_in *in, int fx_zero, uint32_t gen) {
    g_tm_cur = NULL;
    const uint32_t sg = vkn_pipe_forget_gen();
    te_mem *m = fx_zero ? tm_find(s) : NULL;
    if (m && m->gen == gen && m->sgen == sg) {
        int same = 1; uint32_t e = 0;
        for (uint32_t i = 0; same && i < s->nbind; i++) {
            const vkn_bind *b = &s->bind[i];
            for (uint32_t k = 0; k < b->count; k++, e++) {
                if (b->kind != VKN_BK_TEXTURE) continue;
                const int st = src_of_stage(b->stage); const uint32_t slot = b->slot + k;
                const int32_t id = in->tex[st] && slot < VKN_TEX_SLOTS ? in->tex[st][slot] : -1;
                const int32_t sid = in->smp[st] && slot < VKN_TEX_SLOTS ? in->smp[st][slot] : -1;
                if (id != m->e[e].tid || sid != m->e[e].sid) { same = 0; break; }
            }
        }
        for (uint32_t j = 0; same && j < m->ne; j++) if (m->e[j].t && vkn_pass_sample(m->e[j].t) != m->e[j].fb) same = 0;
        vkn_prof_mark_f(VKN_PROF_F_TEX_SAMPLE);   // [rt P0]
        if (same) {
            for (uint32_t j = 0; j < m->ne; j++) { memset(&g_el[j], 0, sizeof g_el[j]); g_el[j].img = m->img[j]; }
            g_te.ok = 0;   // (g_el is the memory's now)
            if (g_ccheck) {   // the full path once more (the same lookups, the same vkn_pass_sample answers): compare, use it
                const vkn_draw_stats a = g_st; const vkn_draw_stats2 a2 = g_st2;
                g_c3.check_runs++;
                if (bind_textures(s, in, NULL, 0, 0)) { g_st = a; g_st2 = a2; check_differ("shader binding memory (full path dropped)", s->id); return -1; }
                int d = g_te.ne != m->ne;
                for (uint32_t j = 0; !d && j < m->ne; j++) d = memcmp(&g_el[j].img, &m->img[j], sizeof m->img[j]) != 0;
                if (d) check_differ("shader binding memory", s->id);
                g_st = a; g_st2 = a2;
            }
            g_st.unknown_tex += m->d_unknown_tex; g_st2.empty_tex += m->d_empty; g_st.feedback_tex += m->d_feedback;
            g_st2.view_mismatch += m->d_mismatch; g_st.unknown_smp += m->d_unknown_smp;
            g_c3.shmem_hits++; g_cs.set_tex_reuse++; m->age = ++g_tm_age; g_tm_cur = m;
            return 0;
        }
    }
    g_c3.shmem_miss++; g_cs.set_tex_lookups++;
    const uint64_t u0 = g_st.unknown_tex, e0 = g_st2.empty_tex, f0 = g_st.feedback_tex, m0 = g_st2.view_mismatch, s0 = g_st.unknown_smp;
    if (bind_textures(s, in, NULL, 0, 0)) return -1;
    if (fx_zero && g_te.ne <= TM_E) {
        m = tm_slot(s);
        if (m->s != s || m->gen != gen || m->sgen != sg) { m->set = VK_NULL_HANDLE; m->nw = 0; }   // (another draw's set key: not this one's)
        m->ok = 1; m->s = s; m->gen = gen; m->sgen = sg; m->ne = g_te.ne; m->age = ++g_tm_age;   // gen / sg read before the lookups
        m->d_unknown_tex = g_st.unknown_tex - u0; m->d_empty = g_st2.empty_tex - e0; m->d_feedback = g_st.feedback_tex - f0;
        m->d_mismatch = g_st2.view_mismatch - m0; m->d_unknown_smp = g_st.unknown_smp - s0;
        for (uint32_t j = 0; j < g_te.ne; j++) { m->e[j] = g_te.e[j]; m->img[j] = g_el[j].img; }
        g_tm_cur = m;
    }
    return 0;
}
static int tex_bind_draw(const vkn_shader *s, const vkn_draw_in *in, int fx_zero) {
    if (s->has_inputs) return bind_textures(s, in, NULL, 0, 0);   // [P10] (a twin's draw: once a frame, never from memory)
    const int on = (g_cost & VKN_COST_SET) && g_tex_gen;
    if (on && (g_cost & VKN_COST_SHMEM)) return tex_bind_mem(s, in, fx_zero, g_tex_gen());   // [rt P3]
    uint32_t gen = 0;
    if (on) {
        gen = g_tex_gen();
        // (sgen: vkn_pipe_forget_gen - a dead shader's record may be freed later and its address reused by another one)
        if (fx_zero && g_te.ok && g_te.s == s && g_te.gen == gen && g_te.sgen == vkn_pipe_forget_gen()) {
            int same = 1; uint32_t e = 0;
            for (uint32_t i = 0; same && i < s->nbind; i++) {
                const vkn_bind *b = &s->bind[i];
                for (uint32_t k = 0; k < b->count; k++, e++) {
                    if (b->kind != VKN_BK_TEXTURE) continue;
                    const int st = src_of_stage(b->stage); const uint32_t slot = b->slot + k;   // st 0 / 1: a draw's table passed bind_textures
                    const int32_t id = in->tex[st] && slot < VKN_TEX_SLOTS ? in->tex[st][slot] : -1;
                    const int32_t sid = in->smp[st] && slot < VKN_TEX_SLOTS ? in->smp[st][slot] : -1;
                    if (id != g_te.e[e].tid || sid != g_te.e[e].sid) { same = 0; break; }
                }
            }
            for (uint32_t j = 0; same && j < g_te.ne; j++)
                if (g_te.e[j].t && vkn_pass_sample(g_te.e[j].t) != g_te.e[j].fb) same = 0;
            if (same) {
                g_st.unknown_tex += g_te.d_unknown_tex; g_st2.empty_tex += g_te.d_empty; g_st.feedback_tex += g_te.d_feedback;
                g_st2.view_mismatch += g_te.d_mismatch; g_st.unknown_smp += g_te.d_unknown_smp;
                g_cs.set_tex_reuse++;
                return 0;
            }
        }
        g_cs.set_tex_lookups++;
    }
    const uint64_t u0 = g_st.unknown_tex, e0 = g_st2.empty_tex, f0 = g_st.feedback_tex, m0 = g_st2.view_mismatch, s0 = g_st.unknown_smp;
    const uint32_t sgen = vkn_pipe_forget_gen();
    if (bind_textures(s, in, NULL, 0, 0)) return -1;
    if (on && fx_zero) {
        g_te.ok = 1; g_te.s = s; g_te.gen = gen; g_te.sgen = sgen;   // both read before the lookups: a change during them makes the record stale
        g_te.d_unknown_tex = g_st.unknown_tex - u0; g_te.d_empty = g_st2.empty_tex - e0; g_te.d_feedback = g_st.feedback_tex - f0;
        g_te.d_mismatch = g_st2.view_mismatch - m0; g_te.d_unknown_smp = g_st.unknown_smp - s0;
    }
    return 0;
}

// ================= [stage 5] draw cost (vkn_draw.h DM_VK_NATIVE_COST): the small exact caches of the per-draw path =================
// Each one answers a question whose answer cannot have changed since it was asked; the reason is written at each. Everything a
// cache holds that names a set, a pipeline or a shader object goes in cost_forget (with the set cache, vkn_draw_forget_all, a death).
// The ring: vkn_buf.c creates its one block once (g_ring_state 1 stays: no destroy), so its VkBuffer and base never change after.
static VkBuffer g_ringb; static uint64_t g_ringo; static int g_ring_ok;
static void ring_of(VkBuffer *b, uint64_t *base) {
    if (g_cost & VKN_COST_LOOKUP) {
        if (g_ring_ok) { *b = g_ringb; *base = g_ringo; g_cs.ring_hits++; return; }
        const VkBuffer rb = vkn_ring_buffer();
        if (rb) { g_ringb = rb; g_ringo = vkn_ring_base(); g_ring_ok = 1; *b = rb; *base = g_ringo; return; }
    }
    *b = vkn_ring_buffer(); *base = vkn_ring_base();
}
// SUPPORT blocks: vkn_shader_support keeps one block per (w, h); [DRS 21.11] once its 256 sizes are taken it rewrites the least
// recently used idle one, so an entry here is good for the frame it was looked up in: a hit of another frame asks again (that also
// marks the size used there - a size drawn every frame is never rewritten under the GPU).
enum { SUP_C = 4 };
static struct { float w, h; vkn_block b; uint64_t frame; } g_supc[SUP_C]; static uint32_t g_nsupc, g_supc_next;
static int support_of(float w, float h, vkn_block *out) {
    const uint64_t now = vkn_frame();
    if (g_cost & VKN_COST_LOOKUP)
        for (uint32_t i = 0; i < g_nsupc; i++)
            if (g_supc[i].w == w && g_supc[i].h == h) {
                if (g_supc[i].frame == now) { *out = g_supc[i].b; g_cs.support_hits++; return 0; }
                if (vkn_shader_support(w, h, out)) return -1;
                g_supc[i].b = *out; g_supc[i].frame = now; return 0;
            }
    const int r = vkn_shader_support(w, h, out);
    if (!r && (g_cost & VKN_COST_LOOKUP)) {
        const uint32_t i = g_nsupc < SUP_C ? g_nsupc++ : g_supc_next++ % SUP_C;
        g_supc[i].w = w; g_supc[i].h = h; g_supc[i].b = *out; g_supc[i].frame = now;
    }
    return r;
}
// [DRS 21.11] safety net: no SUPPORT block for this size -> a frame-local copy in the recording ring (vkn_ring2: copies made where the
// recording is, as vkn_ui.c's) - never the zero block (1/w, 1/h = 0 would make every full-screen pass sample one texel).
// 0 = ok: *buf / *off = the descriptor's buffer and the offset inside it.
static int support_ring(float w, float h, VkBuffer *buf, uint64_t *off) {
    uint8_t tmp[1392]; uint32_t ro = 0;
    if (vkn_shader_support_fill(w, h, tmp, sizeof tmp)) return -1;
    uint8_t *cpu = (uint8_t *)vkn_ring2_cpu();
    if (!cpu || vkn_ring2_alloc(sizeof tmp, &ro)) return -1;
    memcpy(cpu + ro, tmp, sizeof tmp);
    *buf = vkn_ring2_buffer(); *off = vkn_ring2_base() + ro;
    return *buf ? 0 : -1;
}
// Shader objects: vkn_shader_get(h) changes its answer only when the record of h leaves the table (destroy / replace), and every such
// record goes through vkn_shader_set_dead_fn -> vkn_pipe_forget_shader (hle_dos2_vkn_shader.c, both modes: the pipeline cache needs
// it as well), which bumps vkn_pipe_forget_gen(). The generation is read BEFORE the lookup, so a death between the two leaves a stale
// entry that never hits. (A record is valid memory 3 frames after it left the table: vkn_retire.)
enum { SHC = 64 };
static struct { uint64_t h; const vkn_shader *s; uint32_t gen; } g_shc[SHC];
static const vkn_shader *shader_lookup(uint64_t h) {
    if (!(g_cost & VKN_COST_LOOKUP)) return vkn_shader_get(h);
    const uint32_t i = (uint32_t)((h ^ h >> 29) * 0x9e3779b97f4a7c15ull >> 58) & (SHC - 1), gen = vkn_pipe_forget_gen();
    if (g_shc[i].s && g_shc[i].h == h && g_shc[i].gen == gen) { g_cs.shader_hits++; return g_shc[i].s; }
    const vkn_shader *s = vkn_shader_get(h);
    if (s) { g_shc[i].h = h; g_shc[i].s = s; g_shc[i].gen = gen; }
    return s;
}
// State ids. Vertex formats: vkn_vfmt_id(h) changes only by vkn_vfmt_put of the same handle value (an engine handle names one content:
// its generation bits change on reuse) or vkn_vfmt_kill (no caller: the engine has no destroy of them) - so the first id is the id.
// Blend / depth-stencil / raster have destroys (vkn_*_kill): only with vkn_state_handle_gen (then every kind is checked against it).
enum { ST_BLEND, ST_DS, ST_RASTER, ST_VFMT, ST_KINDS };
static struct { uint64_t h; uint16_t id; uint32_t gen; } g_stc[ST_KINDS][2];
static uint16_t state_id(int kind, uint64_t h) {
    uint16_t (*const f[ST_KINDS])(uint64_t) = {vkn_blend_id, vkn_ds_id, vkn_raster_id, vkn_vfmt_id};
    if (!h) return 0;
    const int on = (g_cost & VKN_COST_LOOKUP) && (g_state_gen || kind == ST_VFMT);
    if (!on) return f[kind](h);
    const uint32_t gen = g_state_gen ? g_state_gen() : 0;
    for (int j = 0; j < 2; j++)
        if (g_stc[kind][j].id && g_stc[kind][j].h == h && g_stc[kind][j].gen == gen) { if (kind == ST_VFMT) g_cs.vfmt_hits++; else g_cs.state_hits++; return g_stc[kind][j].id; }
    const uint16_t id = f[kind](h);
    if (id) { g_stc[kind][1] = g_stc[kind][0]; g_stc[kind][0].h = h; g_stc[kind][0].id = id; g_stc[kind][0].gen = gen; }
    return id;
}
// Pipelines: vkn_pipe_get(k) answers a key with what its table holds; an entry of the table changes only when vkn_pipe_forget_shader
// removes it (vkn_pipe_forget_gen() + 1) - inserts are of new keys (a failure is cached too, and never kept here). A build the table
// did not keep (a death during the build: "stale") returns a pipeline that is retired: the generation read before the lookup differs
// from the current one afterwards, so that entry never hits.
enum { PFC = 256 };
static struct { vkn_pipe_key k; VkPipeline p; uint32_t gen; } g_pfc[PFC];
static uint32_t pfc_slot(const vkn_pipe_key *k) { uint64_t h = k->w[0] * 0x9e3779b97f4a7c15ull ^ k->w[1] * 0xc2b2ae3d27d4eb4full ^ k->w[2] * 0x165667b19e3779f9ull; return (uint32_t)(h >> 56) & (PFC - 1); }
// Descriptor-set keys: hashed by words (the content is compared in full on a hit, so only the probe start differs from vkn_fnv); the
// previous lookup's key and set answer an equal key without the probe (the set is in the cache: only inserted / found sets are kept).
static uint64_t key_hash(const uint64_t *w, uint32_t n) {
    uint64_t h = 0x9e3779b97f4a7c15ull ^ n;
    for (uint32_t i = 0; i < n; i++) { h ^= w[i]; h *= 0xff51afd7ed558ccdull; h ^= h >> 32; }
    return h;
}
static uint64_t g_lk[2 + 2 * MAX_ELEMS]; static uint32_t g_lk_n; static VkDescriptorSet g_lk_set; static int g_lk_ok;
static void lk_keep(uint32_t nw, VkDescriptorSet set) { if (g_cost & VKN_COST_SET) { memcpy(g_lk, g_kw, nw * sizeof g_kw[0]); g_lk_n = nw; g_lk_set = set; g_lk_ok = 1; } }
static void cost_forget(void) {
    g_lk_ok = 0; g_te.ok = 0; tm_forget();   // [rt P3] (the shader memory names sets / shader objects too)
    memset(g_shc, 0, sizeof g_shc); memset(g_pfc, 0, sizeof g_pfc);
}
void vkn_draw_cost_set(uint32_t mask) {
    mask &= VKN_COST_ALL2;   // [rt P3] bits 0x100 / 0x200
    if ((mask ^ g_cost) & VKN_COST_SET) __atomic_store_n(&g_forget_req, 1, __ATOMIC_RELEASE);   // set keys hashed the other way: a new generation
    g_cost = mask;
    if (!g_gens_test) { g_tex_gen = vkn_tex_bind_gen; g_state_gen = vkn_state_handle_gen; g_vindex = vkn_shader_vindex ? vindex_weak : NULL; }
    cost_forget(); memset(g_stc, 0, sizeof g_stc); g_nsupc = 0; g_ring_ok = 0;
    memset(g_mt, 0, sizeof g_mt); memset(g_ms, 0, sizeof g_ms); memset(g_mc, 0, sizeof g_mc);   // [rt P3]
}
uint32_t vkn_draw_cost(void) { return g_cost; }
void vkn_draw_cost_gens(uint32_t (*tex_gen)(void), uint32_t (*state_gen)(void), int (*vindex)(const void *shader)) {
    g_gens_test = 1;
    g_tex_gen = tex_gen ? tex_gen : vkn_tex_bind_gen; g_state_gen = state_gen ? state_gen : vkn_state_handle_gen;
    g_vindex = vindex ? vindex : vkn_shader_vindex ? vindex_weak : NULL;
    cost_forget(); memset(g_stc, 0, sizeof g_stc);
}
void vkn_draw_cost_stats_get(vkn_draw_cost_stats *s) { if (s) *s = g_cs; }

// Phase 2: uniform blocks, then the key, the cache, the writes. *set / dyn[] / *ndyn out. 0 = ok (set may be NULL: no bindings).
static int build_set(const vkn_shader *s, const vkn_draw_in *in, VkDescriptorSet *out, uint32_t *dyn, uint32_t *ndyn) {
    *out = VK_NULL_HANDLE; *ndyn = 0;
    if (!s->nbind) return 0;
    if (s->ndyn > MAX_DYN) { g_st2.dropped_set++; DRAW_RED(R_SET, "shader %#llx: %u dynamic offsets (max %d)", (unsigned long long)s->handle, s->ndyn, MAX_DYN); return -1; }
    const vkn_dev *d = vkn_device(); if (!d) return -1;
    const uint64_t maxr = d->props.limits.maxUniformBufferRange ? d->props.limits.maxUniformBufferRange : 16384;
    VkBuffer ring; uint64_t ring_base; ring_of(&ring, &ring_base);
    uint32_t e = 0, nw = 0;
    g_kw[nw++] = (uint64_t)(uintptr_t)s->set_layout; g_kw[nw++] = s->nbind;
    for (uint32_t i = 0; i < s->nbind; i++) {
        const vkn_bind *b = &s->bind[i];
        const int dynamic = b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        for (uint32_t k = 0; k < b->count; k++, e++) {
            elem *el = &g_el[e];
            if (b->kind == VKN_BK_UNIFORM || b->kind == VKN_BK_SUPPORT || b->kind == VKN_BK_C1 || b->kind == VKN_BK_FX_UBO) {
                if (b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && !dynamic) { g_st2.dropped_bind++; DRAW_RED(R_BIND, "shader %#llx binding %u: uniform source with descriptor type %d", (unsigned long long)s->handle, b->binding, (int)b->type); return -1; }
                VkBuffer buf = VK_NULL_HANDLE; uint64_t off = 0, size = 0, doff = 0;   // doff: the dynamic offset (dynamic descriptors only)
                const int st = src_of_stage(b->stage);
                if (b->kind == VKN_BK_UNIFORM) {
                    const uint32_t slot = b->slot + k;
                    if (st >= 0 && slot < VKN_UBO_SLOTS && g_ubo[st][slot].valid && ring) {
                        buf = ring; size = g_ubo[st][slot].size;
                        if (dynamic) { off = ring_base; doff = g_ubo[st][slot].off; } else off = ring_base + g_ubo[st][slot].off;
                    }
                } else if (b->kind == VKN_BK_FX_UBO) {   // [stage 5] a block the chain gave: the ring (buf NULL) or its own buffer
                    const vkn_fx_ubo *fu = g_fx && (uint32_t)b->slot + k < VKN_FX_UBO ? &g_fx->ubo[b->slot + k] : NULL;
                    if (fu && fu->size && (fu->buf || ring)) {
                        size = fu->size;
                        if (fu->buf) { buf = fu->buf; if (dynamic) doff = fu->off; else off = fu->off; }
                        else { buf = ring; if (dynamic) { off = ring_base; doff = fu->off; } else off = ring_base + fu->off; }
                    }
                } else if (b->kind == VKN_BK_SUPPORT) {
                    vkn_block sb;
                    if (!support_of(in->viewport[2], in->viewport[3], &sb)) { buf = sb.buf; off = sb.off; size = sb.size; }
                    else if (!support_ring(in->viewport[2], in->viewport[3], &buf, &off)) size = 1392;   // [DRS 21.11] (counted by vkn_shader)
                    else buf = VK_NULL_HANDLE;
                } else if (st >= 0 && s->c1[st].size) { buf = s->c1[st].buf; off = s->c1[st].off; size = s->c1[st].size; }
                if (!buf || !size) {
                    const vkn_block *z = zero_block();
                    if (!z) { g_st2.dropped_set++; return -1; }
                    buf = z->buf; off = z->off; size = ZERO_UBO; doff = 0; g_st2.zero_ubo++;
                }
                uint64_t range = size;
                if (b->bytes && b->bytes < range) range = b->bytes;
                if (range > 65536) range = 65536;
                if (range > maxr) range = maxr;
                el->buf.buffer = buf; el->buf.offset = off; el->buf.range = range;
                if (dynamic) { if (b->dyn + k < MAX_DYN) dyn[b->dyn + k] = (uint32_t)doff; }
                g_kw[nw++] = (uint64_t)(uintptr_t)buf; g_kw[nw++] = off << 20 ^ range;
            } else {   // (view, sampler); bit 63 = storage image (GENERAL). Non-dispatchable handles may be small integers: no mixing.
                g_kw[nw++] = (uint64_t)(uintptr_t)el->img.imageView;
                g_kw[nw++] = (uint64_t)(uintptr_t)el->img.sampler | (uint64_t)(el->img.imageLayout == VK_IMAGE_LAYOUT_GENERAL) << 63;
            }
        }
    }
    *ndyn = s->ndyn;
    vkn_prof_mark_f(VKN_PROF_F_SET_KEY);   // [rt P0]
    // [rt P3] SHMEM: the key of this shader's draw the binding memory answered for (its set is in the cache: cost_forget clears both)
    te_mem *const tm = g_tm_cur; g_tm_cur = NULL;
    if (tm && tm->set && tm->nw == nw && !memcmp(tm->kw, g_kw, nw * sizeof g_kw[0])) {
        g_c3.shmem_set_hits++;
        if (!g_ccheck) { g_st.set_cache++; *out = tm->set; return 0; }
        g_c3.check_runs++;   // (checked below: the probe must find the same set)
    }
    // [stage 5, draw cost] the previous lookup's key: the same set (still in the cache: cost_forget clears this with it)
    if ((g_cost & VKN_COST_SET) && g_lk_ok && g_lk_n == nw && !memcmp(g_lk, g_kw, nw * sizeof g_kw[0])) {
        g_st.set_cache++; g_cs.set_key_same++; *out = g_lk_set;
        if (tm && g_ccheck && tm->set && tm->nw == nw && !memcmp(tm->kw, g_kw, nw * sizeof g_kw[0]) && tm->set != g_lk_set) check_differ("shader set memory", s->id);
        if (tm && nw <= TM_KW) { memcpy(tm->kw, g_kw, nw * sizeof g_kw[0]); tm->nw = nw; tm->set = g_lk_set; }
        return 0;
    }
    // lookup
    const uint64_t h = (g_cost & VKN_COST_SET) ? key_hash(g_kw, nw) : vkn_fnv(g_kw, nw * sizeof g_kw[0]);
    if (!g_sets) { g_sets = (set_e *)calloc(SET_TAB, sizeof *g_sets); if (!g_sets) { g_st2.dropped_set++; DRAW_RED(R_SET, "set cache: out of memory"); return -1; } }
    uint32_t pos = (uint32_t)h & (SET_TAB - 1);
    for (uint32_t n = 0; n < SET_TAB; n++, pos = (pos + 1) & (SET_TAB - 1)) {
        set_e *x = &g_sets[pos];
        if (!x->w) break;
        if (x->hash == h && x->nw == nw && !memcmp(x->w, g_kw, nw * sizeof g_kw[0])) {
            g_st.set_cache++; *out = x->set; lk_keep(nw, x->set);
            if (tm && g_ccheck && tm->set && tm->nw == nw && !memcmp(tm->kw, g_kw, nw * sizeof g_kw[0]) && tm->set != x->set) check_differ("shader set memory", s->id);
            if (tm && nw <= TM_KW) { memcpy(tm->kw, g_kw, nw * sizeof g_kw[0]); tm->nw = nw; tm->set = x->set; }   // [rt P3]
            vkn_prof_mark_f(VKN_PROF_F_SET_FIND);
            return 0;
        }
    }
    if (tm && g_ccheck && tm->set && tm->nw == nw && !memcmp(tm->kw, g_kw, nw * sizeof g_kw[0])) check_differ("shader set memory (not in the cache)", s->id);
    // miss: a new set (a full cache starts a new generation first)
    if (g_nsets >= SET_BUDGET) { forget_now(); pos = (uint32_t)h & (SET_TAB - 1); }
    VkDescriptorSet set = set_alloc(s->set_layout);
    if (!set) { g_st2.dropped_set++; DRAW_RED(R_SET, "shader %#llx: vkAllocateDescriptorSets failed (%u pools, %u sets cached)", (unsigned long long)s->handle, g_npool, g_nsets); return -1; }
    static VkWriteDescriptorSet wr[MAX_ELEMS];
    uint32_t nwr = 0; e = 0;
    for (uint32_t i = 0; i < s->nbind; i++) {
        const vkn_bind *b = &s->bind[i];
        VkWriteDescriptorSet *w = &wr[nwr++]; memset(w, 0, sizeof *w);
        w->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w->dstSet = set; w->dstBinding = b->binding; w->descriptorCount = b->count; w->descriptorType = b->type;
        if (b->kind == VKN_BK_UNIFORM || b->kind == VKN_BK_SUPPORT || b->kind == VKN_BK_C1 || b->kind == VKN_BK_FX_UBO) w->pBufferInfo = &g_el[e].buf;
        else w->pImageInfo = &g_el[e].img;
        e += b->count;
    }
    // elements of one binding are consecutive in g_el, but pBufferInfo / pImageInfo need arrays of their own type: gather per binding
    static VkDescriptorBufferInfo bi[MAX_ELEMS]; static VkDescriptorImageInfo ii[MAX_ELEMS];
    e = 0;
    for (uint32_t i = 0; i < nwr; i++) {
        const vkn_bind *b = &s->bind[i];
        if (wr[i].pBufferInfo) { for (uint32_t k = 0; k < b->count; k++) bi[e + k] = g_el[e + k].buf; wr[i].pBufferInfo = &bi[e]; }
        else { for (uint32_t k = 0; k < b->count; k++) ii[e + k] = g_el[e + k].img; wr[i].pImageInfo = &ii[e]; }
        e += b->count;
    }
    vkUpdateDescriptorSets(d->dev, nwr, wr, 0, NULL);
    g_st.set_builds++;
    // insert
    uint64_t *kw = (uint64_t *)malloc(nw * sizeof *kw);
    if (kw) {
        memcpy(kw, g_kw, nw * sizeof *kw);
        for (uint32_t n = 0; n < SET_TAB; n++, pos = (pos + 1) & (SET_TAB - 1))
            if (!g_sets[pos].w) {
                g_sets[pos].hash = h; g_sets[pos].w = kw; g_sets[pos].nw = nw; g_sets[pos].set = set; g_nsets++; g_st2.set_bytes_words += nw; kw = NULL; lk_keep(nw, set);
                if (tm && nw <= TM_KW) { memcpy(tm->kw, g_kw, nw * sizeof g_kw[0]); tm->nw = nw; tm->set = set; }   // [rt P3] (only a set the cache keeps)
                break;
            }
        free(kw);   // table full (cannot happen below the budget): the set is used once, not cached
    }
    *out = set;
    vkn_prof_mark_f(VKN_PROF_F_SET_FIND);   // [rt P0]
    return 0;
}

// ---- recording state ----
static void on_begin(void) { forget_bound(); g_b.serial = vkn_cb_serial(); g_b.valid = 1; death_check(); }
void vkn_draw_install(void) { vkn_rec_on_begin(on_begin); }
static VkCommandBuffer cb_ready(int graphics) {
    VkCommandBuffer cb = graphics && SORT ? vkn_cb_peek() : vkn_cb();   // [rt2 R7] a graphics draw may join the sort window: no flush here
    if (!cb) return VK_NULL_HANDLE;
    if (!g_b.valid || g_b.serial != vkn_cb_serial()) on_begin();   // a recording this module was not told about (no listener)
    if (__atomic_exchange_n(&g_forget_req, 0, __ATOMIC_ACQ_REL)) forget_now();
    return cb;
}

// Shader of a draw: both graphics stages present and usable (a stage the engine shader does not have is fine only for fragment).
static const vkn_shader *shader_for(const vkn_draw_in *in, int compute, const vkn_draw_pre *pre) {
    const vkn_shader *s;
    if (pre && !vkn_draw_rt_res_verify) { s = pre->shader; g_rs.used++; }   // [rt P2] resolved at encode
    else {
        s = in->shader ? shader_lookup(in->shader) : NULL;
        if (pre) { g_rs.verified++; if (s != pre->shader) { g_rs.differ++; g_rs.differ_shader++; } }
    }
    if (!s) { g_st.dropped_shader++; DRAW_RED(R_SHADER, "engine shader %#llx: no native object (%s dropped)", (unsigned long long)in->shader, compute ? "dispatch" : "draw"); return NULL; }
    const uint8_t bad = s->missing | s->unusable;
    const int ok = compute ? (s->stages & (1u << VKN_SRC_CS)) && !(bad & (1u << VKN_SRC_CS))
                           : (s->stages & (1u << VKN_SRC_VS)) && !(bad & ((1u << VKN_SRC_VS) | (1u << VKN_SRC_FS)));
    if (!ok || !s->layout) {
        g_st.dropped_shader++;
        DRAW_RED(R_STAGE, "engine shader %#llx (%s / %s / %s): stages %#x missing %#x unusable %#x layout %p (%s dropped)", (unsigned long long)in->shader, s->key[0], s->key[1], s->key[2],
                 s->stages, s->missing, s->unusable, (void *)s->layout, compute ? "dispatch" : "draw");
        return NULL;
    }
    return s;
}

static void bind_set(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, VkDescriptorSet set, const uint32_t *dyn, uint32_t ndyn) {
    const int g = bp == VK_PIPELINE_BIND_POINT_GRAPHICS;
    VkDescriptorSet *bs = g ? &g_b.gset : &g_b.cset; VkPipelineLayout *bl = g ? &g_b.glayout : &g_b.clayout;
    uint32_t *bn = g ? &g_b.gndyn : &g_b.cndyn, *bd = g ? g_b.gdyn : g_b.cdyn;
    if (!set) return;
    if (*bs == set && *bl == layout && *bn == ndyn && !memcmp(bd, dyn, ndyn * sizeof *dyn)) return;
    vkn_prof_mark_f(VKN_PROF_F_SET_FIND);   // [rt P0]
    if (g && SORT) vkn_sort_sets(cb, layout, set, ndyn, dyn); else vkCmdBindDescriptorSets(cb, bp, layout, 0, 1, &set, ndyn, ndyn ? dyn : NULL);   // [rt2 R7]
    vkn_prof_mark_f(VKN_PROF_F_SET_CMD); vkn_prof_driver_calls(1);
    *bs = set; *bl = layout; *bn = ndyn; memcpy(bd, dyn, ndyn * sizeof *dyn);
    g_st.set_binds++;
}

// Everything of a graphics draw up to the draw command. 0 = ready (*vf / pass info filled), -1 = dropped (counted).
// prim: the primitive the pipeline draws (in->prim, or vkn_prim_rewrite(in->prim) for a rewritten draw).
static int prepare(const vkn_draw_in *in, uint8_t prim, VkCommandBuffer *cb_out, vkn_pass_info *pi, const vkn_vfmt **vf_out) {
    const uint8_t call_indexed = g_call.indexed; const uint32_t call_first = g_call.first;   // [stage 5.1 G] (a nested draw rewrites g_call)
    VkCommandBuffer cb = cb_ready(1);
    if (!cb) { g_st2.dropped_cb++; DRAW_RED(R_CB, "draw without an open recording (dropped)"); return -1; }
    if (SORT) vkn_sort_cap = 0;   // [rt2 R7] (decided after vkn_pass_ready below)
    g_tm_cur = NULL;   // [rt P3]
    const vkn_draw_pre *const pre = pre_of(in); const int puse = pre && !vkn_draw_rt_res_verify;   // [rt P2]
    const vkn_shader *s = shader_for(in, 0, pre); if (!s) return -1;
    // state ids (handle 0 = the default: blend off / depth-stencil off / default raster / no vertex input)
    uint16_t bid, rid, did, vid;
    if (puse) { bid = in->blend ? pre->blend : 0; rid = in->raster ? pre->raster : 0; did = in->ds ? pre->ds : 0; vid = in->vfmt ? pre->vfmt : 0; }   // [rt P2]
    else {
        bid = in->blend ? state_id(ST_BLEND, in->blend) : 0; rid = in->raster ? state_id(ST_RASTER, in->raster) : 0;
        did = in->ds ? state_id(ST_DS, in->ds) : 0; vid = in->vfmt ? state_id(ST_VFMT, in->vfmt) : 0;
        if (pre && ((in->blend ? pre->blend : 0) != bid || (in->raster ? pre->raster : 0) != rid || (in->ds ? pre->ds : 0) != did || (in->vfmt ? pre->vfmt : 0) != vid)) {
            g_rs.differ++; g_rs.differ_state++;
        }
    }
    if ((in->blend && !bid) || (in->ds && !did) || (in->raster && !rid) || (in->vfmt && !vid)) {
        g_st.dropped_state++;
        DRAW_RED(R_STATE, "unknown state handle: blend %#llx -> %u, depth-stencil %#llx -> %u, raster %#llx -> %u, vertex format %#llx -> %u (shader %#llx, draw dropped)",
                 (unsigned long long)in->blend, bid, (unsigned long long)in->ds, did, (unsigned long long)in->raster, rid, (unsigned long long)in->vfmt, vid, (unsigned long long)in->shader);
        return -1;
    }
    VkPrimitiveTopology topo;
    if (vkn_prim_topology(prim, &topo)) { g_st.dropped_prim++; DRAW_RED(R_PRIM, "engine primitive %u (drawn as %u) has no Vulkan topology: draw dropped", in->prim, prim); return -1; }
    if (!(in->viewport[2] > 0.0f) || in->viewport[3] == 0.0f) {
        g_st2.dropped_viewport++;
        DRAW_RED(R_VIEWPORT, "viewport %g %g %g %g (x y w h): no area, draw dropped", in->viewport[0], in->viewport[1], in->viewport[2], in->viewport[3]);
        return -1;
    }
    vkn_prof_mark(VKN_PROF_STATE);   // [stage 5 A0 probes]
    // [stage 5] THE call point of the draw effects (vkn_fx.h): after the engine's shader and state ids are known, before anything
    // of the draw is touched (buffers, textures, pass, pipeline). A skipped draw leaves no trace in the recording.
    g_fx = NULL; uint8_t fxbits = 0;
    if (vkn_fx_active && vkn_fx_run && vkn_fx_active()) {
        vkn_fx_ctx fc; memset(&fc, 0, sizeof fc);
        fc.shader = s; for (uint32_t i = 0; i < VKN_PASS_COLORS; i++) fc.color[i] = vkn_pass_color(i);
        fc.depth = vkn_pass_depth(); fc.blend = bid; fc.ds = did; fc.raster = rid; fc.vfmt = vid; fc.prim = prim;
        fc.indexed = g_call.indexed; fc.first = g_call.first; fc.count = g_call.count; fc.instances = g_call.instances; fc.frame = vkn_frame();
        if (&vkn_fx_time_next) vkn_fx_time_next = vkn_prof_sampled3();   // [rt P0] (the chain times its functions on this sampled draw)
        g_fx_nest++; vkn_fx_run(in, &fc, &g_fx_out); g_fx_nest--;   // [stage 5.1 G] (a piece's draws inside: nested)
        if (g_fx_out.skip) { vkn_prof_mark(VKN_PROF_FX); return 1; }
        g_fx = &g_fx_out;
        if (g_fx_out.shader) s = g_fx_out.shader;
        if (g_fx_out.ds) did = g_fx_out.ds;
        if (g_fx_out.discard) fxbits |= VKN_PIPE_FX_DISCARD;
        if (g_fx_out.no_fs && (s->stages & (1u << VKN_SRC_FS))) fxbits |= VKN_PIPE_FX_NO_FS;
    }
    vkn_prof_mark(VKN_PROF_FX);   // [stage 5 A0 probes]
    int fxs = vkn_fx_shader_is(s);   // a piece's shader: its own modules, no variants
    const vkn_vfmt *vf = vid ? vkn_vfmt_info(vid) : NULL;
    uint16_t vs_inputs = 0;
    for (uint32_t i = 0; i < s->ninputs && i < 16; i++) if (s->inputs[i] < VKN_VFMT_LOCS) vs_inputs |= (uint16_t)(1u << s->inputs[i]);
    uint64_t code = fxs ? 0 : retype_code(s, vf);
    // [P10] the merged tile pass (vkn_pass.h): a full-screen draw of its chain is drawn with the input-attachment twin of its shader
    if (vkn_tile_hook) { const vkn_shader *tw = vkn_tile_twin(s, in, g_fx, code); if (tw) { s = tw; fxs = 1; code = 0; } }
    // [stage 5.3 V1] a per-draw vertex module (vkn_fx.h vkn_fx_out.vin): the code above stays the engine format's (the module is made for
    // it); its own vertex format (content id) feeds the pipeline and the stream binds, its stream `vin->stream` is bound at its buffer
    const vkn_fx_vin *vin = g_fx && g_fx_out.vin.vs ? &g_fx_out.vin : NULL;
    uint16_t vid_p = vid; uint32_t vin_st = 0xff;
    if (vin && vin->vfmt) {
        const vkn_vfmt *ov = vkn_vfmt_info(vin->vfmt);
        if (!ov || vin->stream >= VKN_VFMT_STREAMS || !vin->buf) {
            g_st.dropped_state++; g_fx_drop++;
            DRAW_RED(R_STATE, "effect vertex input: format id %u, stream %u, buffer %p is not usable (shader %#llx, draw dropped)", vin->vfmt, vin->stream, (void *)vin->buf, (unsigned long long)in->shader);
            return -1;
        }
        vf = ov; vid_p = vin->vfmt; vin_st = vin->stream;
    }
    const int has_fs = (s->stages & (1u << VKN_SRC_FS)) && !(fxbits & VKN_PIPE_FX_NO_FS);
    const int vsv = variant_vs(code), fsv = has_fs && !fxs ? variant_fs(in->alpha_ref) : 0;
    if (vsv < 0 || fsv < 0) { g_st2.dropped_variant++; DRAW_RED(R_VARIANT, "more than %d module variants (retype %#llx, alpha reference %g): draw dropped", VARIANTS - 1, (unsigned long long)code, in->alpha_ref); return -1; }
    // vertex / index buffers (resolved before the pass: a drop must not open one)
    VkBuffer vbuf[VKN_VFMT_NULL_BINDING + 1]; VkDeviceSize voff[VKN_VFMT_NULL_BINDING + 1], vraw[VKN_VFMT_NULL_BINDING + 1]; uint8_t vuse = 0, vinst = 0;
    uint32_t vstride[VKN_VFMT_NULL_BINDING + 1] = {0, 0, 0, 0};
    const int cvb = (g_cost & VKN_COST_VB) != 0;
    const vkn_block *vblk[VKN_VFMT_STREAMS]; uint32_t nvfound = 0;
    const vkn_draw_bufs *const nbk = nest_bufs_of(in);   // [shcfix 22.11]
    if (vf && cvb && !puse && !(nbk && nbk->vset)) {   // [stage 5, draw cost] every stream the format reads under one lock (stops where the loop below drops)
        uint64_t vh[VKN_VFMT_STREAMS]; uint32_t nh = 0;
        for (uint32_t i = 0; i < vf->nbind; i++) if (vf->bind[i].binding < VKN_VFMT_STREAMS && vf->bind[i].binding != vin_st && nh < VKN_VFMT_STREAMS) vh[nh++] = in->vb[vf->bind[i].binding];
        nvfound = nh ? vkn_buf_get_n(VKN_BUF_VB, vh, nh, vblk) : 0; g_cs.vb_batched++;
    }
    for (uint32_t i = 0, j = 0; vf && i < vf->nbind; i++) {
        const uint32_t st = vf->bind[i].binding; if (st >= VKN_VFMT_STREAMS) continue;
        if (st == vin_st) {   // [stage 5.3 V1] the effect's stream (its own buffer, no engine handle)
            vbuf[st] = vin->buf; voff[st] = vin->off; vuse |= (uint8_t)(1u << st);
            vstride[st] = vf->bind[i].stride; if (vf->bind[i].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE) vinst |= (uint8_t)(1u << st);
            continue;
        }
        VkBuffer bb = VK_NULL_HANDLE; uint64_t bo = 0;
        if (puse) { bb = pre->vb[st]; bo = pre->vb_off[st]; }   // [rt P2] resolved at encode (buf NULL: what a failed lookup drops)
        else if (nbk && nbk->vset) {   // [shcfix 22.11] the blocks the kept draw was drawn with
            if (st < 3 && (nbk->vset >> st & 1)) { bb = nbk->vb[st]; bo = nbk->vb_off[st]; }
            else { const vkn_block *blk = in->vb[st] ? vkn_buf_get(VKN_BUF_VB, in->vb[st]) : NULL; if (blk) { bb = blk->buf; bo = blk->off; } }
        }
        else {
            const vkn_block *blk = cvb ? (j < nvfound ? vblk[j] : NULL) : in->vb[st] ? vkn_buf_get(VKN_BUF_VB, in->vb[st]) : NULL;
            if (blk) { bb = blk->buf; bo = blk->off; }
            if (pre && (pre->vb[st] != bb || (bb && pre->vb_off[st] != bo))) { g_rs.differ++; g_rs.differ_vb++; }
        }
        j++;
        if (!bb) {
            g_st2.dropped_vb++;
            DRAW_RED(R_VB, "vertex format %#llx reads stream %u, vertex buffer %#llx is not a live native buffer (draw dropped)", (unsigned long long)in->vfmt, st, (unsigned long long)in->vb[st]);
            return -1;
        }
        vbuf[st] = bb; voff[st] = bo + in->vb_off[st]; vuse |= (uint8_t)(1u << st);
        vstride[st] = vf->bind[i].stride; if (vf->bind[i].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE) vinst |= (uint8_t)(1u << st);
    }
    if (vs_inputs & ~(vf ? vf->loc_mask : 0)) {
        const vkn_block *nb = null_stream(); if (!nb) { g_st2.dropped_vb++; return -1; }
        vbuf[VKN_VFMT_NULL_BINDING] = nb->buf; voff[VKN_VFMT_NULL_BINDING] = nb->off; vuse |= 1u << VKN_VFMT_NULL_BINDING;   // stride 0: never folded
    }
    // [stage 5, draw cost] vertex fold (nr_fold_vertex): stream b fetches vertex v at voff[b] - k * stride[b] + (v + k) * stride[b] =
    // voff[b] + v * stride[b], the bytes of the unfolded bind, for v = index (+ 0: the engine's draws have no base vertex) or
    // firstVertex + i; per-instance streams (instanceIndex = firstInstance + i: untouched) and stride 0 (the null stream) keep their
    // offset. k is added to vertexOffset / firstVertex, which changes gl_VertexIndex / gl_BaseVertex: only for a vertex module known not
    // to read them (or gl_VertexID): vkn_shader_vindex exactly 0; a piece's shader never. Bit VFOLD ([stage 5.1 G], with VB) gates it.
    // The int32 check of nr_fold_vertex takes the draw's own first vertex (arrays: firstVertex; rewritten arrays: their vertexOffset).
    memcpy(vraw, voff, sizeof voff);
    g_vfold = 0;
    if (cvb && vuse && (g_cost & VKN_COST_VFOLD)) {
        const int vi = fxs || !g_vindex ? -1 : g_vindex(s);
        if (!vi) {
            uint32_t st16[16]; uint64_t of16[16], out16[16]; memset(st16, 0, sizeof st16); memset(of16, 0, sizeof of16);
            for (uint32_t b = 0; b <= VKN_VFMT_NULL_BINDING; b++) if (vuse >> b & 1) { st16[b] = vstride[b]; of16[b] = voff[b]; }
            g_vfold = nr_fold_vertex(vuse, vinst, st16, of16, call_indexed ? 0 : (int64_t)call_first, 1, out16);
            if (g_vfold) { for (uint32_t b = 0; b <= VKN_VFMT_NULL_BINDING; b++) if (vuse >> b & 1) voff[b] = out16[b]; g_cs.vb_folded++; }
        } else g_cs.vb_vindex++;
    }
    vkn_prof_mark_f(VKN_PROF_F_BUF_LOOK);   // [rt P0]
    vkn_prof_mark(VKN_PROF_BUF);   // [stage 5 A0 probes]
    // textures (vkn_pass_sample may end the open pass), then the pass
    if (g_cost & VKN_COST_SET) {   // [stage 5, draw cost] the previous draw's texture bindings when nothing they depend on changed
        const int fx_zero = g_tex_gen && (!g_fx || (vkn_fx_out_same && vkn_fx_out_same(g_fx, NULL)));   // (only asked when it can matter)
        if (tex_bind_draw(s, in, fx_zero)) return -1;
    } else if (bind_textures(s, in, NULL, 0, 0)) return -1;
    vkn_prof_mark_f(VKN_PROF_F_TEX_REST);   // [rt P0]
    vkn_prof_mark(VKN_PROF_TEX);   // [stage 5 A0 probes]
    if (vkn_pass_ready(pi)) { g_st.dropped_pass++; DRAW_RED(R_PASS, "no render pass for the current targets (shader %#llx, draw dropped)", (unsigned long long)in->shader); return -1; }
    cb = vkn_cb_peek();   // [P10] the pass may have moved the recording into / out of a subpass buffer (vkn_cb_route; the router forgot what was bound)
    if (SORT) (void)vkn_sort_eligible(did, pi->ncolor, pi->write_mask, pi->has_depth, s, g_fx_nest > 0, prim != in->prim, vkn_pass_begins ? vkn_pass_begins() : 0);   // [rt2 R7]
    vkn_prof_mark(VKN_PROF_PASS);   // [stage 5 A0 probes]
    // pipeline
    vkn_pipe_desc pd; memset(&pd, 0, sizeof pd);
    pd.shader = s->id; pd.vs_variant = (uint8_t)vsv; pd.fs_variant = (uint8_t)fsv; pd.blend = bid; pd.ds = did; pd.raster = rid; pd.vfmt = vid_p;
    pd.prim = prim; pd.stencil_ref = in->stencil_ref; pd.pass = pi->compat; pd.fx = fxbits;
    for (int i = 0; i < 8; i++) pd.write_mask[i] = (uint8_t)(pi->write_mask[i] & 0xf);
    vkn_pipe_key key = vkn_pipe_key_pack(&pd);
    if (vin) key.w[2] |= (uint64_t)vin->vs_id << 48;   // [stage 5.3 V1] the per-draw vertex module (bits 48..63 are 0 in every packed key)
    vkn_prof_mark_f(VKN_PROF_F_PIPE_KEY);   // [rt P0]
    if (!g_b.gpipe_ok || memcmp(&key, &g_b.gkey, sizeof key)) {
        if (vkn_prof_lvl3) {   // [rt P0] what differs from the previous key of this recording
            const uint16_t vsid = vin ? vin->vs_id : 0;
            if (g_ppd_ok) {
                const vkn_pipe_desc *o = &g_ppd; uint8_t f[12];
                f[0] = o->shader != pd.shader; f[1] = o->vs_variant != pd.vs_variant || o->fs_variant != pd.fs_variant; f[2] = o->vfmt != pd.vfmt; f[3] = o->ds != pd.ds;
                f[4] = o->raster != pd.raster; f[5] = o->blend != pd.blend; f[6] = o->stencil_ref != pd.stencil_ref; f[7] = memcmp(o->write_mask, pd.write_mask, sizeof pd.write_mask) != 0;
                f[8] = o->prim != pd.prim; f[9] = o->pass != pd.pass; f[10] = o->fx != pd.fx; f[11] = g_ppd_vin != vsid;
                g_p3.changes++; int other = 0, nb = 0;
                for (int i = 0; i < 12; i++) if (f[i]) { g_p3.f[i]++; if (i != 3 && i != 4 && i != 5 && i != 6) other = 1; if (i == 5) nb = 1; }
                if (!other) { g_p3.dyn_only++; if (!nb) g_p3.dyn_only_nb++; }
            }
            g_ppd = pd; g_ppd_vin = vsid; g_ppd_ok = 1;
        }
        VkPipeline p = VK_NULL_HANDLE; uint32_t fslot = 0, fgen = 0; int fhit = 0;
        if (g_cost & VKN_COST_PIPE) {   // [stage 5, draw cost] the front cache (generation read before the lookup / build below)
            fslot = pfc_slot(&key); fgen = vkn_pipe_forget_gen();
            if (g_pfc[fslot].p && g_pfc[fslot].gen == fgen && !memcmp(&g_pfc[fslot].k, &key, sizeof key)) { p = g_pfc[fslot].p; fhit = 1; g_cs.pipe_front_hits++; }
            else g_cs.pipe_front_miss++;
        }
        if (!p) p = vkn_pipe_get(&key, NULL);
        if (vkn_tile_prime_compat) {   // [P10] the same draw's pipeline for its subpass of the learned merged pass, built ahead (a few ms a frame)
            vkn_pipe_desc p2 = pd; p2.pass = vkn_tile_prime_compat; vkn_pipe_key k2 = vkn_pipe_key_pack(&p2);
            if (vin) k2.w[2] |= (uint64_t)vin->vs_id << 48;
            if (!vkn_pipe_get(&k2, NULL)) {
                int built = 0; uint64_t t0 = 0;
                if (vkn_tile_prime_budget()) {
                    vkn_pipe_src s2; memset(&s2, 0, sizeof s2); t0 = vkn_now_ns();
                    s2.vs = vin ? vin->vs : fxs ? s->module[VKN_SRC_VS] : vkn_shader_module(s, VKN_STAGE_VS, 0, 0.0f, code);
                    s2.fs = !has_fs ? VK_NULL_HANDLE : fxs ? s->module[VKN_SRC_FS] : vkn_shader_module(s, VKN_STAGE_FS, fsv ? 5u : 0u, fsv ? in->alpha_ref : 0.0f, 0);
                    s2.layout = s->layout; s2.pass = vkn_tile_prime_rp; s2.vs_inputs = vs_inputs;
                    built = s2.vs && (s2.fs || !has_fs) && vkn_pipe_get(&k2, &s2) != VK_NULL_HANDLE;
                }
                vkn_tile_prime_note(built, built ? vkn_now_ns() - t0 : 0);
            }
        }
        if (!p) {
            vkn_pipe_src src; memset(&src, 0, sizeof src);
            src.vs = vin ? vin->vs : fxs ? s->module[VKN_SRC_VS] : vkn_shader_module(s, VKN_STAGE_VS, 0, 0.0f, code);   // [stage 5.3 V1] vin
            src.fs = !has_fs ? VK_NULL_HANDLE : fxs ? s->module[VKN_SRC_FS] : vkn_shader_module(s, VKN_STAGE_FS, fsv ? 5u : 0u, fsv ? in->alpha_ref : 0.0f, 0);
            src.layout = s->layout; src.pass = pi->pass; src.vs_inputs = vs_inputs;
            src.shader = s; src.retype = code; src.alpha_func = fsv ? 5u : 0u; src.alpha_ref = fsv ? in->alpha_ref : 0.0f;   // [DOS2] VK-NATIVE stage 4 warm-up (vkn_warm.c): what makes this build replayable
            src.vs_id = vin ? vin->vs_id : 0;   // [stage 5 rest] effect variants too: fx bits, a piece's shader / vertex module by its recipe (vkn_warm.h)
            if (src.vs && (src.fs || !has_fs)) p = vkn_pipe_get(&key, &src);
        }
        if (!p) {
            g_st.dropped_pipe++;
            DRAW_RED(R_PIPE, "no pipeline: shader %#llx (%s / %s) blend %u ds %u raster %u vfmt %u prim %u pass %u, variants vs %d fs %d (draw dropped)",
                     (unsigned long long)in->shader, s->key[0], s->key[1], bid, did, rid, vid, prim, pi->compat, vsv, fsv);
            return -1;
        }
        if ((g_cost & VKN_COST_PIPE) && !fhit) { g_pfc[fslot].k = key; g_pfc[fslot].p = p; g_pfc[fslot].gen = fgen; }
        vkn_prof_mark_f(VKN_PROF_F_PIPE_FIND);   // [rt P0]
        if (p != g_b.gpipe) { if (SORT) vkn_sort_pipe(cb, p); else vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); g_st.pipe_binds++; vkn_prof_driver_calls(1); }   // [rt2 R7]
        vkn_prof_mark_f(VKN_PROF_F_PIPE_CMD);
        g_b.gpipe = p; g_b.gkey = key; g_b.gpipe_ok = 1;
    }
    vkn_prof_mark(VKN_PROF_PIPE);   // [stage 5 A0 probes]
    // descriptor set
    VkDescriptorSet set; uint32_t dyn[MAX_DYN], ndyn = 0;
    if ((g_cost & VKN_COST_SET) && s->ndyn <= MAX_DYN) memset(dyn, 0, s->ndyn * sizeof dyn[0]);   // [stage 5, draw cost] (only the first ndyn are read)
    else memset(dyn, 0, sizeof dyn);
    if (build_set(s, in, &set, dyn, &ndyn)) return -1;
    bind_set(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->layout, set, dyn, ndyn);
    vkn_prof_mark(VKN_PROF_TEX);   // [stage 5 A0 probes] (textures and the set are one part)
    // vertex buffers
    if (cvb) {   // [stage 5, draw cost] one vkCmdBindVertexBuffers per run of used bindings holding a change (nr_vb_runs: a run may
                 // rebind an unchanged used binding between two changed ones with the same values - no state change)
        unsigned changed = 0;
        for (uint32_t b = 0; b <= VKN_VFMT_NULL_BINDING; b++) {
            if (!(vuse >> b & 1)) continue;
            if (g_b.vb[b] != vbuf[b] || g_b.vb_off[b] != voff[b]) changed |= 1u << b;
            if (g_b.ovb[b] != vbuf[b] || g_b.ovb_off[b] != vraw[b]) { g_b.ovb[b] = vbuf[b]; g_b.ovb_off[b] = vraw[b]; g_cs.vb_calls_old++; }   // the stage-4 path's bind
        }
        uint8_t rf[8], rc[8]; const unsigned nrun = nr_vb_runs(changed, vuse, rf, rc);
        vkn_prof_mark_f(VKN_PROF_F_BUF_LOOK); vkn_prof_driver_calls(nrun);   // [rt P0]
        for (unsigned r = 0; r < nrun; r++) {
            if (SORT) vkn_sort_vb(cb, rf[r], rc[r], &vbuf[rf[r]], &voff[rf[r]]); else vkCmdBindVertexBuffers(cb, rf[r], rc[r], &vbuf[rf[r]], &voff[rf[r]]);   // [rt2 R7]
            for (uint32_t b = rf[r]; b < (uint32_t)rf[r] + rc[r]; b++) { g_b.vb[b] = vbuf[b]; g_b.vb_off[b] = voff[b]; }
            g_st.vb_binds++; g_cs.vb_calls++;
        }
    } else
    for (uint32_t b = 0; b <= VKN_VFMT_NULL_BINDING; b++) {
        if (!(vuse >> b & 1) || (g_b.vb[b] == vbuf[b] && g_b.vb_off[b] == voff[b])) continue;
        if (SORT) vkn_sort_vb(cb, b, 1, &vbuf[b], &voff[b]); else vkCmdBindVertexBuffers(cb, b, 1, &vbuf[b], &voff[b]);   // [rt2 R7]
        g_b.vb[b] = vbuf[b]; g_b.vb_off[b] = voff[b]; g_st.vb_binds++; vkn_prof_driver_calls(1);
    }
    vkn_prof_mark_f(VKN_PROF_F_BUF_CMD);   // [rt P0]
    vkn_prof_mark(VKN_PROF_BUF);   // [stage 5 A0 probes]
    // Viewport. NVN with the UPPER_LEFT window origin (the engine sets it) maps NDC y = +1 to row 0; Vulkan maps NDC y = -1 to row 0.
    // The packs' SPIR-V is the game's own clip space (no flip inside), so the viewport is flipped (negative height, Vulkan 1.1), exactly
    // as the translator's xbg_nvn_viewport does: rows and the winding seen in framebuffer space (front face CW) then equal NVN's.
    // (First phone run V3_a: without the flip every back-face-culled draw vanished - a black frame with 0 dropped draws.) Depth 0..1.
    // Scissor is in framebuffer rows already (upper-left origin in both APIs), clamped to the pass.
    const VkViewport vp = {in->viewport[0], in->viewport[1] + in->viewport[3], in->viewport[2], -in->viewport[3], 0.0f, 1.0f};
    if (!g_b.vp_ok || memcmp(&vp, &g_b.vp, sizeof vp)) { if (SORT) vkn_sort_vp(cb, &vp); else vkCmdSetViewport(cb, 0, 1, &vp); vkn_prof_mark_f(VKN_PROF_F_VIEW_CMD); vkn_prof_driver_calls(1); g_b.vp = vp; g_b.vp_ok = 1; g_st.viewport_sets++; }
    int64_t x0 = in->scissor[0], y0 = in->scissor[1], x1 = x0 + in->scissor[2], y1 = y0 + in->scissor[3];
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int64_t)pi->width) x1 = pi->width;
    if (y1 > (int64_t)pi->height) y1 = pi->height;
    if (x1 <= x0 || y1 <= y0) { g_st2.scissor_empty++; return 1; }
    const VkRect2D sc = {{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    if (!g_b.sc_ok || memcmp(&sc, &g_b.sc, sizeof sc)) { if (SORT) vkn_sort_sc(cb, &sc); else vkCmdSetScissor(cb, 0, 1, &sc); vkn_prof_mark_f(VKN_PROF_F_VIEW_CMD); vkn_prof_driver_calls(1); g_b.sc = sc; g_b.sc_ok = 1; }
    // Debug (DM_VK_NATIVE_DUMP): in the dumped frame, the whole state of every draw into a big RGBA8 target (the post chain, the
    // window): what the pipeline was built from, where it draws, what it samples. At most 160 lines a dump.
    if (vkn_pass_dump_active()) {
        static uint32_t lines; static uint64_t lines_frame;
        if (lines_frame != vkn_frame()) { lines_frame = vkn_frame(); lines = 0; }
        vkn_tex *ct = vkn_pass_color(0);
        if (ct && ct->format == VK_FORMAT_R8G8B8A8_UNORM && ct->w >= 1000 && lines++ < 160) {
            const VkPipelineDepthStencilStateCreateInfo *di = did ? vkn_ds_info(did) : NULL;
            const VkPipelineRasterizationStateCreateInfo *ri = rid ? vkn_raster_info(rid) : NULL;
            const VkPipelineColorBlendAttachmentState *bi = bid ? vkn_blend_att(bid) : NULL;
            char tx[384]; int n = 0; tx[0] = 0;
            for (uint32_t i = 0; i < s->nbind && n < (int)sizeof tx - 48; i++) {
                const vkn_bind *b = &s->bind[i]; if (b->kind != VKN_BK_TEXTURE) continue;
                const int st = src_of_stage(b->stage);
                const int32_t id = st >= 0 && in->tex[st] && b->slot < VKN_TEX_SLOTS ? in->tex[st][b->slot] : -1;
                vkn_tex *t = NULL; if (id >= 0) vkn_tex_view_by_id((uint32_t)id, &t);
                n += snprintf(tx + n, sizeof tx - (size_t)n, " s%d.%u=%llx(%ux%u f%d L%d)", st, b->slot, t ? (unsigned long long)(t->handle & 0xffff) : 0xffffull, t ? t->w : 0, t ? t->h : 0, t ? (int)t->format : 0, t ? (int)t->layout : -1);
            }
            {   // the uniform blocks the shader reads: stage.slot = ring offset / size, the first 4 floats and floats 52..59 (vec4 13, 14)
                char ub[640]; int m = 0; ub[0] = 0; const float *ring = (const float *)vkn_ring_cpu();
                for (uint32_t i = 0; i < s->nbind && m < (int)sizeof ub - 160; i++) {
                    const vkn_bind *b = &s->bind[i]; if (b->kind != VKN_BK_UNIFORM) continue;
                    const int st = src_of_stage(b->stage);
                    if (st < 0 || b->slot >= VKN_UBO_SLOTS || !g_ubo[st][b->slot].valid || !ring) { m += snprintf(ub + m, sizeof ub - (size_t)m, " u%d.%u=none", st, b->slot); continue; }
                    const uint32_t off = g_ubo[st][b->slot].off, sz = g_ubo[st][b->slot].size; const float *f = (const float *)((const uint8_t *)ring + off);
                    m += snprintf(ub + m, sizeof ub - (size_t)m, " u%d.%u=@%x+%u[%g %g %g %g", st, b->slot, off, sz, sz >= 16 ? f[0] : 0, sz >= 16 ? f[1] : 0, sz >= 16 ? f[2] : 0, sz >= 16 ? f[3] : 0);
                    if (sz >= 240) m += snprintf(ub + m, sizeof ub - (size_t)m, " | %g %g %g %g %g %g %g %g", f[52], f[53], f[54], f[55], f[56], f[57], f[58], f[59]);
                    m += snprintf(ub + m, sizeof ub - (size_t)m, "]");
                }
                vkn_logf("trace: uniforms of the next line:%s", ub);
            }
            vkn_logf("trace: target %llx %ux%u (pass %ux%u, %u colours, depth %d) shader %llx vs %s fs %s | blend h %llx id %u en %d mask in pipe %x | ds id %u test %d write %d op %d stencil %d ref %u | cull %d front %d | prim %u vfmt %u vb0 %llx+%u | vp %g %g %g %g sc %d %d %u %u | alpha %g fsv %d |%s",
                     (unsigned long long)(ct->handle & 0xffff), ct->w, ct->h, pi->width, pi->height, pi->ncolor, pi->has_depth, (unsigned long long)(in->shader & 0xffff), s->key[0], s->key[1],
                     (unsigned long long)in->blend, bid, bi ? (int)bi->blendEnable : -1, (unsigned)pd.write_mask[0],
                     did, di ? (int)di->depthTestEnable : -1, di ? (int)di->depthWriteEnable : -1, di ? (int)di->depthCompareOp : -1, di ? (int)di->stencilTestEnable : -1, in->stencil_ref,
                     ri ? (int)ri->cullMode : -1, ri ? (int)ri->frontFace : -1, prim, vid, (unsigned long long)(in->vb[0] & 0xffff), in->vb_off[0],
                     vp.x, vp.y, vp.width, vp.height, sc.offset.x, sc.offset.y, sc.extent.width, sc.extent.height, in->alpha_ref, fsv, tx);
        }
    }
    vkn_prof_mark(VKN_PROF_VIEW);   // [stage 5 A0 probes]
    *cb_out = cb; *vf_out = vf;
    return 0;
}

// ---- index rewrite: engine 2 (triangle fan) -> triangle list, 4 (quads) -> two triangles a quad, 7 (line loop) -> line strip +
// the first index again (vkn_prim_rewrite). The generated indices go into a transient area of this module: one block of
// VKN_ARENA_STAGING (host-visible, coherent, index usage) handed out linearly and never written twice; a full block goes through
// vkn_retire_block (freed 3 presented frames later, after every recording that read it) and the next draw takes a new one. ----
enum { RW_CHUNK = 256 * 1024 };
static vkn_block g_rw; static uint64_t g_rw_used; static int g_rw_ok;
static vkn_draw_rw_stats g_rw_st;
static int rw_kind(uint8_t p) { return p == 2 ? VKN_RW_FAN : p == 4 ? VKN_RW_QUADS : p == 7 ? VKN_RW_LOOP : -1; }
// Generated indices for n input vertices / indices (0 = nothing to draw: GL / NVN draw nothing for a fan under 3 or a loop under 2;
// a quad list ignores an incomplete tail).
static uint64_t rw_count(int k, uint32_t n) {
    return k == VKN_RW_FAN ? (n >= 3 ? 3ull * (n - 2) : 0) : k == VKN_RW_QUADS ? 6ull * (n / 4) : k == VKN_RW_LOOP ? (n >= 2 ? (uint64_t)n + 1 : 0) : 0;
}
static void *rw_alloc(uint64_t bytes, VkBuffer *buf, VkDeviceSize *off) {
    bytes = (bytes + 3) & ~3ull;   // u32 index alignment for the next draw
    if (g_rw_ok && g_rw_used + bytes > g_rw.size) { vkn_retire_block(&g_rw); g_rw_ok = 0; g_rw_st.retired++; }
    if (!g_rw_ok) {
        if (vkn_block_alloc(VKN_ARENA_STAGING, bytes > RW_CHUNK ? bytes : RW_CHUNK, 256, &g_rw)) return NULL;
        g_rw_ok = 1; g_rw_used = 0; g_rw_st.blocks++;
    }
    void *p = (uint8_t *)g_rw.cpu + g_rw_used;
    *buf = g_rw.buf; *off = g_rw.off + g_rw_used;
    g_rw_used += bytes; g_rw_st.bytes += bytes;
    return p;
}
// src NULL: arrays (index i = vertex i counted from the first vertex); else the engine's indices of isz (2 / 4) bytes.
static inline uint32_t rw_src(const void *src, uint32_t isz, uint32_t i) {
    return !src ? i : isz == 4 ? ((const uint32_t *)src)[i] : ((const uint16_t *)src)[i];
}
static void rw_fill(int k, const void *src, uint32_t isz, uint32_t n, void *dst, int out32) {
    uint32_t *d32 = (uint32_t *)dst; uint16_t *d16 = (uint16_t *)dst; uint64_t o = 0;
#define RW_PUT(v) do { const uint32_t v_ = (v); if (out32) d32[o++] = v_; else d16[o++] = (uint16_t)v_; } while (0)
    if (k == VKN_RW_FAN) {   // triangle i = (v0, v[i+1], v[i+2]): GL's / NVN's fan, same winding
        const uint32_t v0 = rw_src(src, isz, 0);
        for (uint32_t i = 0; i + 2 < n; i++) { RW_PUT(v0); RW_PUT(rw_src(src, isz, i + 1)); RW_PUT(rw_src(src, isz, i + 2)); }
    } else if (k == VKN_RW_QUADS) {   // quad (a, b, c, d) = (a, b, c) + (a, c, d), as the translator's {0, 1, 2, 0, 2, 3}
        for (uint32_t q = 0; q + 4 <= n; q += 4) {
            const uint32_t a = rw_src(src, isz, q), b = rw_src(src, isz, q + 1), c = rw_src(src, isz, q + 2), e = rw_src(src, isz, q + 3);
            RW_PUT(a); RW_PUT(b); RW_PUT(c); RW_PUT(a); RW_PUT(c); RW_PUT(e);
        }
    } else {   // line loop: the strip of every index, then the first one again
        for (uint32_t i = 0; i < n; i++) RW_PUT(rw_src(src, isz, i));
        RW_PUT(rw_src(src, isz, 0));
    }
#undef RW_PUT
}
// The rewritten draw. src NULL: arrays draw of n vertices from first_vertex; else n engine indices at src (isz bytes each).
// 0 = recorded / nothing to draw, 1 = dropped (counted).
static int draw_rewritten(const vkn_draw_in *in, const void *src, uint32_t isz, uint32_t first_vertex, uint32_t n, uint32_t instances) {
    const int k = rw_kind(in->prim), ix = src != NULL;
    const uint8_t q = vkn_prim_rewrite(in->prim);
    const uint64_t m = rw_count(k, n);
    if (k < 0 || !m) { g_rw_st.degenerate++; return 0; }
    if (m > 0x7fffffffull) {
        g_rw_st.dropped++;
        DRAW_RED(R_RW, "rewrite of engine primitive %u: %u %s give %llu indices (draw dropped)", in->prim, n, ix ? "indices" : "vertices", (unsigned long long)m);
        return 1;
    }
    const int out32 = ix ? isz == 4 : n > 0xffffu;   // arrays: relative indices 0..n-1, the first vertex goes to vertexOffset
    VkCommandBuffer cb; vkn_pass_info pi; const vkn_vfmt *vf;
    g_call.indexed = (uint8_t)ix; g_call.first = first_vertex; g_call.count = n; g_call.instances = instances ? instances : 1;
    const int r = prepare(in, q, &cb, &pi, &vf);
    if (r) { if (SORT && vkn_sort_cap) vkn_sort_nodraw(); return r < 0 ? 1 : 0; }   // [rt2 R7] (binds of a captured draw that ended)
    VkBuffer buf; VkDeviceSize off;
    void *dst = rw_alloc(m * (out32 ? 4u : 2u), &buf, &off);
    if (!dst) {
        g_rw_st.dropped++;
        DRAW_RED(R_RW, "rewrite of engine primitive %u: no transient index block for %llu indices (draw dropped)", in->prim, (unsigned long long)m);
        return 1;
    }
    rw_fill(k, src, isz, n, dst, out32);
    vkn_prof_mark(VKN_PROF_BUF);   // [stage 5 A0 probes] (the generated indices)
    const VkIndexType it = out32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    uint32_t fi = 0; const VkDeviceSize roff = off;
    if (g_cost & VKN_COST_IB) {   // [stage 5, draw cost] the index fold (rw_alloc hands out 4-aligned offsets: off mod size is 0)
        uint64_t bo; uint32_t q; nr_fold_index(off, out32 ? 2 : 1, 1, &bo, &q);
        if ((uint64_t)q + m <= 0xffffffffull) { off = bo; fi = q; if (q) g_cs.ib_folded++; }
        if (g_b.oib != buf || g_b.oib_off != roff || g_b.oib_type != it) { g_b.oib = buf; g_b.oib_off = roff; g_b.oib_type = it; g_cs.ib_calls_old++; }
    }
    if (g_b.ib != buf || g_b.ib_off != off || g_b.ib_type != it) {   // the engine's next indexed draw sees the difference and rebinds
        if (SORT) vkn_sort_ib(cb, buf, off, it); else vkCmdBindIndexBuffer(cb, buf, off, it);   // [rt2 R7]
        g_b.ib = buf; g_b.ib_off = off; g_b.ib_type = it; g_st.ib_binds++;
        if (g_cost & VKN_COST_IB) g_cs.ib_calls++;
    }
    vkn_prof_mark(VKN_PROF_BUF);   // [stage 5 A0 probes]
    Q_PRE(cb);   // [stage 4: queries]
    // vertexOffset: the arrays' first vertex (+ the vertex fold of prepare); indexed: 0 (+ the fold)
    if (SORT) vkn_sort_draw_idx(cb, (uint32_t)m, instances ? instances : 1, fi, (ix ? 0 : (int32_t)first_vertex) + (int32_t)g_vfold, 0);   // [rt2 R7]
    else vkCmdDrawIndexed(cb, (uint32_t)m, instances ? instances : 1, fi, (ix ? 0 : (int32_t)first_vertex) + (int32_t)g_vfold, 0);
    Q_POST(cb);  // [stage 4: queries]
    vkn_prof_mark(VKN_PROF_CMD);   // [stage 5 A0 probes]
    g_st.draws++;
    if (ix) g_st.indexed++; else g_st.arrays++;
    g_rw_st.draws[ix][k]++; g_rw_st.indices += m;
    return 0;
}
void vkn_draw_rw_stats_get(vkn_draw_rw_stats *s) { if (s) *s = g_rw_st; }

// [P10] mode 2 (vkn_pass.h): the draw just recorded with an input-attachment twin, once more the classic way (same effect output)
void vkn_fx_force(const vkn_fx_out *o) __attribute__((weak));
static void tile_redraw(const vkn_draw_in *in, int indexed, uint32_t first, uint32_t count, uint32_t instances) {
    static vkn_fx_out fo;
    if (!vkn_fx_force) return;
    if (g_fx) fo = *g_fx; else memset(&fo, 0, sizeof fo);
    if (!vkn_tile_redraw_begin()) return;
    vkn_fx_force(&fo);
    if (indexed) (void)vkn_draw_indexed(in, first, count, instances); else (void)vkn_draw_arrays(in, first, count);
    vkn_fx_force(NULL);
    vkn_tile_redraw_end();
}
int vkn_draw_arrays(const vkn_draw_in *in, uint32_t first, uint32_t count);
int vkn_draw_indexed(const vkn_draw_in *in, uint32_t first_index, uint32_t count, uint32_t instances) {
    if (!in) return 1;
    if (!count) return 0;
    Q_ENTER();   // [stage 4: queries]
    // the index buffer first: no pass or pipeline work for a draw that cannot be made
    vkn_buf_info ib; memset(&ib, 0, sizeof ib);
    int ib_rc;
    const vkn_draw_pre *const pre = pre_of(in);   // [rt P2] the IB resolved at encode
    const vkn_draw_bufs *const nbk = nest_bufs_of(in);   // [shcfix 22.11] a replay of a kept draw: its index block
    if (pre && !vkn_draw_rt_res_verify) { ib = pre->ib; ib_rc = in->ib ? pre->ib_rc : -1; }
    else if (nbk && nbk->iset) { ib = nbk->ib; ib_rc = in->ib ? nbk->ib_rc : -1; }
    else {
        ib_rc = in->ib ? vkn_buf_lookup(VKN_BUF_IB, in->ib, &ib) : -1;
        if (pre) {
            const int prc = in->ib ? pre->ib_rc : -1;
            if (prc != ib_rc || (!ib_rc && (ib.blk.buf != pre->ib.blk.buf || ib.blk.off != pre->ib.blk.off || ib.blk.size != pre->ib.blk.size || ib.blk.cpu != pre->ib.blk.cpu ||
                                            ib.size != pre->ib.size || ib.index_size != pre->ib.index_size))) { g_rs.differ++; g_rs.differ_ib++; }
        }
    }
    if (!in->ib || ib_rc || !ib.blk.size || !ib.blk.buf) {
        g_st2.dropped_ib++; DRAW_RED(R_IB, "index buffer %#llx is not a live native buffer (draw of %u indices dropped)", (unsigned long long)in->ib, count); return 1;
    }
    const uint32_t isz = ib.index_size == 4 ? 4 : 2;
    if (((uint64_t)first_index + count) * isz > ib.size) {
        g_st2.dropped_ib++;
        DRAW_RED(R_IB, "index buffer %#llx: indices %u + %u of %u bytes each exceed its %llu bytes (draw dropped)", (unsigned long long)in->ib, first_index, count, isz, (unsigned long long)ib.size);
        return 1;
    }
    vkn_prof_mark(VKN_PROF_BUF);   // [stage 5 A0 probes] (the index buffer lookup)
    if (rw_kind(in->prim) >= 0) {   // the engine's indices as it wrote them through the buffer's CPU pointer (mapped, coherent)
        if (!ib.blk.cpu) {
            g_rw_st.dropped++;
            DRAW_RED(R_RW, "index buffer %#llx has no CPU pointer: no rewrite of engine primitive %u (draw dropped)", (unsigned long long)in->ib, in->prim);
            return 1;
        }
        const void *src = !g_fx_nest && vkn_draw_rt_snap && vkn_draw_rt_in == in ? vkn_draw_rt_snap : (const uint8_t *)ib.blk.cpu + (uint64_t)first_index * isz;   // [stage 5.5]
        return draw_rewritten(in, src, isz, 0, count, instances);
    }
    VkCommandBuffer cb; vkn_pass_info pi; const vkn_vfmt *vf;
    g_call.indexed = 1; g_call.first = first_index; g_call.count = count; g_call.instances = instances ? instances : 1;
    const int r = prepare(in, in->prim, &cb, &pi, &vf);
    if (r) { if (SORT && vkn_sort_cap) vkn_sort_nodraw(); return r < 0 ? 1 : 0; }   // [rt2 R7] (binds of a captured draw that ended)
    const VkIndexType it = vkn_buf_index_type(ib.index_size);
    VkDeviceSize ioff = ib.blk.off; uint32_t fi = first_index;
    if (g_cost & VKN_COST_IB) {
        // [stage 5, draw cost] index fold (nr_fold_index): bound at off mod isz, index j read at that + (first + off / isz + j) * isz =
        // off + (first + j) * isz, the unfolded address. Engine index buffers are 256-aligned blocks of one arena VkBuffer: (buffer, 0)
        // for all of them. isz = 4 for UINT32, 2 otherwise (the index type vkn_buf_index_type binds).
        uint64_t bo; uint32_t q; nr_fold_index(ioff, isz == 4 ? 2 : 1, 1, &bo, &q);
        if ((uint64_t)first_index + q + count <= 0xffffffffull) { ioff = bo; fi = first_index + q; if (q) g_cs.ib_folded++; }
        if (g_b.oib != ib.blk.buf || g_b.oib_off != ib.blk.off || g_b.oib_type != it) { g_b.oib = ib.blk.buf; g_b.oib_off = ib.blk.off; g_b.oib_type = it; g_cs.ib_calls_old++; }
    }
    if (g_b.ib != ib.blk.buf || g_b.ib_off != ioff || g_b.ib_type != it) {
        vkn_prof_mark_f(VKN_PROF_F_BUF_LOOK); if (SORT) vkn_sort_ib(cb, ib.blk.buf, ioff, it); else vkCmdBindIndexBuffer(cb, ib.blk.buf, ioff, it); vkn_prof_driver_calls(1);   // [rt P0] [rt2 R7]
        g_b.ib = ib.blk.buf; g_b.ib_off = ioff; g_b.ib_type = it; g_st.ib_binds++;
        if (g_cost & VKN_COST_IB) g_cs.ib_calls++;
    }
    vkn_prof_mark(VKN_PROF_BUF);   // [stage 5 A0 probes]
    vkn_prof_mark_f(VKN_PROF_F_BUF_CMD);   // [rt P0]
    Q_PRE(cb);   // [stage 4: queries]
    vkn_prof_mark_f(VKN_PROF_F_Q_PRE);
    if (SORT) vkn_sort_draw_idx(cb, count, instances ? instances : 1, fi, (int32_t)g_vfold, 0);   // [rt2 R7]
    else vkCmdDrawIndexed(cb, count, instances ? instances : 1, fi, (int32_t)g_vfold, 0);   // vertexOffset: the vertex fold of prepare (0 without)
    vkn_prof_mark_f(VKN_PROF_F_DRAW_CMD); vkn_prof_driver_calls(1);
    Q_POST(cb);  // [stage 4: queries]
    vkn_prof_mark(VKN_PROF_CMD);   // [stage 5 A0 probes]
    g_st.draws++; g_st.indexed++;
    if (vkn_tile_redraw_on) tile_redraw(in, 1, first_index, count, instances);   // [P10] mode 2
    return 0;
}

int vkn_draw_arrays(const vkn_draw_in *in, uint32_t first, uint32_t count) {
    if (!in) return 1;
    if (!count) return 0;
    Q_ENTER();   // [stage 4: queries]
    if (rw_kind(in->prim) >= 0) return draw_rewritten(in, NULL, 0, first, count, 1);
    VkCommandBuffer cb; vkn_pass_info pi; const vkn_vfmt *vf;
    g_call.indexed = 0; g_call.first = first; g_call.count = count; g_call.instances = 1;
    const int r = prepare(in, in->prim, &cb, &pi, &vf);
    if (r) { if (SORT && vkn_sort_cap) vkn_sort_nodraw(); return r < 0 ? 1 : 0; }   // [rt2 R7] (binds of a captured draw that ended)
    Q_PRE(cb);   // [stage 4: queries]
    vkn_prof_mark_f(VKN_PROF_F_Q_PRE);   // [rt P0]
    if (SORT) vkn_sort_draw(cb, count, 1, first + g_vfold, 0);   // [rt2 R7]
    else vkCmdDraw(cb, count, 1, first + g_vfold, 0);   // firstVertex + the vertex fold of prepare (0 without; nr_fold_vertex keeps it in int32)
    vkn_prof_mark_f(VKN_PROF_F_DRAW_CMD); vkn_prof_driver_calls(1);
    Q_POST(cb);  // [stage 4: queries]
    vkn_prof_mark(VKN_PROF_CMD);   // [stage 5 A0 probes]
    g_st.draws++; g_st.arrays++;
    if (vkn_tile_redraw_on) tile_redraw(in, 0, first, count, 1);   // [P10] mode 2
    return 0;
}

int vkn_draw_dispatch(const vkn_draw_in *in, const vkn_img_bind *img, uint32_t nimg, uint32_t x, uint32_t y, uint32_t z) {
    g_fx = NULL;   // [stage 5] dispatches do not pass the effect chain
    if (!in) return 1;
    VkCommandBuffer cb = cb_ready(0);
    if (!cb) { g_st2.dropped_cb++; DRAW_RED(R_CB, "dispatch without an open recording (dropped)"); return 1; }
    g_tm_cur = NULL;   // [rt P3]
    const vkn_shader *s = shader_for(in, 1, NULL); if (!s) return 1;
    if (!x || !y || !z) return 0;
    vkn_pass_end();   // a dispatch is recorded outside every render pass
    if (bind_textures(s, in, img, nimg, 1)) return 1;
    const VkShaderModule m = vkn_shader_module(s, VKN_STAGE_CS, 0, 0.0f, 0);
    const VkPipeline p = m ? vkn_pipe_compute(s->id, m, s->layout) : VK_NULL_HANDLE;
    if (!p) { g_st.dropped_pipe++; DRAW_RED(R_PIPE, "no compute pipeline: shader %#llx (%s), module %p (dispatch dropped)", (unsigned long long)in->shader, s->key[2], (void *)m); return 1; }
    if (p != g_b.cpipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p); g_b.cpipe = p; g_st.pipe_binds++; }
    VkDescriptorSet set; uint32_t dyn[MAX_DYN], ndyn = 0;
    memset(dyn, 0, sizeof dyn);
    if (build_set(s, in, &set, dyn, &ndyn)) return 1;
    bind_set(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s->layout, set, dyn, ndyn);
    vkCmdDispatch(cb, x, y, z);
    g_st.dispatches++;
    return 0;
}

// ==== [rt P2] the encoder's resolution (vkn_draw.h vkn_draw_pre): the engine thread, own caches (the recording thread's g_shc / g_stc
// stay its own: no cache line shared). Each cache entry is valid under the generation read BEFORE its lookup, as on the recording side. ====
static struct { uint64_t h; const vkn_shader *s; uint32_t gen; } g_eshc[SHC];
static struct { uint64_t h; uint16_t id; uint32_t gen; } g_estc[ST_KINDS][2];
static uint64_t g_evf_h; static const vkn_vfmt *g_evf; static uint16_t g_evf_id;
static uint16_t enc_state_id(int kind, uint64_t h) {
    uint16_t (*const f[ST_KINDS])(uint64_t) = {vkn_blend_id, vkn_ds_id, vkn_raster_id, vkn_vfmt_id};
    if (!h) return 0;
    const int on = (g_cost & VKN_COST_LOOKUP) && (vkn_state_handle_gen || kind == ST_VFMT);
    if (!on) return f[kind](h);
    const uint32_t gen = vkn_state_handle_gen ? vkn_state_handle_gen() : 0;
    for (int j = 0; j < 2; j++) if (g_estc[kind][j].id && g_estc[kind][j].h == h && g_estc[kind][j].gen == gen) return g_estc[kind][j].id;
    const uint16_t id = f[kind](h);
    if (id) { g_estc[kind][1] = g_estc[kind][0]; g_estc[kind][0].h = h; g_estc[kind][0].id = id; g_estc[kind][0].gen = gen; }
    return id;
}
void vkn_draw_resolve(const vkn_draw_in *in, int indexed, vkn_draw_pre *p) {
    memset(p, 0, sizeof *p); p->ib_rc = -1;
    if (in->shader) {
        if (g_cost & VKN_COST_LOOKUP) {
            const uint64_t h = in->shader;
            const uint32_t i = (uint32_t)((h ^ h >> 29) * 0x9e3779b97f4a7c15ull >> 58) & (SHC - 1), gen = vkn_pipe_forget_gen();
            if (g_eshc[i].s && g_eshc[i].h == h && g_eshc[i].gen == gen) p->shader = g_eshc[i].s;
            else { p->shader = vkn_shader_get(h); if (p->shader) { g_eshc[i].h = h; g_eshc[i].s = p->shader; g_eshc[i].gen = gen; } }
        } else p->shader = vkn_shader_get(in->shader);
    }
    p->blend = enc_state_id(ST_BLEND, in->blend); p->ds = enc_state_id(ST_DS, in->ds); p->raster = enc_state_id(ST_RASTER, in->raster); p->vfmt = enc_state_id(ST_VFMT, in->vfmt);
    // the vertex streams the format reads (marked as vkn_buf_mark does), the index buffer (marked by the lookup)
    const vkn_vfmt *vf = NULL;
    if (p->vfmt) { if (g_evf && g_evf_id == p->vfmt && g_evf_h == in->vfmt) vf = g_evf; else { vf = vkn_vfmt_info(p->vfmt); if (vf) { g_evf = vf; g_evf_id = p->vfmt; g_evf_h = in->vfmt; } } }   // (contents never change)
    uint64_t h[VKN_VFMT_STREAMS]; uint8_t st_of[VKN_VFMT_STREAMS]; uint32_t n = 0;
    for (uint32_t i = 0; vf && i < vf->nbind; i++) {
        const uint32_t st = vf->bind[i].binding;
        if (st < VKN_VFMT_STREAMS && st < 3 && in->vb[st] && n < VKN_VFMT_STREAMS) { int dup = 0; for (uint32_t k = 0; k < n; k++) dup |= st_of[k] == st; if (!dup) { h[n] = in->vb[st]; st_of[n++] = (uint8_t)st; } }
    }
    if (n) {
        vkn_block b[VKN_VFMT_STREAMS]; vkn_buf_mark_get(VKN_BUF_VB, h, n, b);
        for (uint32_t k = 0; k < n; k++) { p->vb[st_of[k]] = b[k].size ? b[k].buf : VK_NULL_HANDLE; p->vb_off[st_of[k]] = b[k].off; }
    }
    if (indexed) p->ib_rc = in->ib ? vkn_buf_lookup(VKN_BUF_IB, in->ib, &p->ib) : -1;
}

void vkn_draw_stats_get(vkn_draw_stats *s) { if (s) *s = g_st; }
void vkn_draw_stats2_get(vkn_draw_stats2 *s) { if (s) *s = g_st2; }
#endif
