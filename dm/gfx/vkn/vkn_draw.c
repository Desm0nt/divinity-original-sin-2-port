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
#include <stdlib.h>
#include <string.h>
#include "vkn_buf.h"
#include "vkn_frame.h"
#include "vkn_pass.h"
#include "vkn_shader.h"
#include "vkn_state.h"
#include "vkn_tex.h"

#define VKN_DRAW_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"   // the red-line mark as a literal of its own

enum { REDS = 16, MAX_ELEMS = 1024, MAX_DYN = 64, SET_BUDGET = 4096, SET_TAB = 8192, POOL_SETS = 256, MAX_POOLS = 64, ZERO_UBO = 65536,
       VARIANTS = 256 };
enum { R_SHADER, R_STAGE, R_STATE, R_PIPE, R_PASS, R_PRIM, R_VB, R_IB, R_BIND, R_VIEWPORT, R_VARIANT, R_SET, R_TEX, R_SMP, R_VIEW,
       R_PLACE, R_UBO, R_IMG, R_CB, R_RW, R_N };
static uint8_t g_red[R_N];
#define DRAW_RED(reason, ...) do { if (g_red[reason] < REDS) { g_red[reason]++; vkn_logf(VKN_DRAW_RED "draw: " __VA_ARGS__); } } while (0)

static vkn_draw_stats g_st;
static vkn_draw_stats2 g_st2;

// ---- uniform blocks announced by the constant flushes: (stage, slot) -> ring offset / size; persist like NVN's bindings ----
typedef struct ubo_slot { uint32_t off, size; uint8_t valid; } ubo_slot;
static ubo_slot g_ubo[VKN_STAGE_SLOTS][VKN_UBO_SLOTS];
void vkn_draw_ubo(int stage, uint32_t slot, uint32_t ring_off, uint32_t size) {
    if (stage < 0 || stage >= VKN_STAGE_SLOTS || slot >= VKN_UBO_SLOTS) {
        g_st2.ubo_bad++;
        DRAW_RED(R_UBO, "uniform block announced for stage %d slot %u (ring offset %#x, %u bytes): out of range, ignored", stage, slot, ring_off, size);
        return;
    }
    g_ubo[stage][slot].off = ring_off; g_ubo[stage][slot].size = size; g_ubo[stage][slot].valid = 1;
    g_st2.ubo_announce++;
}

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
} g_b;
static void forget_bound(void) { const uint32_t s = g_b.serial; memset(&g_b, 0, sizeof g_b); g_b.serial = s; }
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
static int g_forget_req;   // vkn_draw_forget_all from any thread: executed at the next draw
static uint64_t g_death; static int g_death_ok;
static void pool_destroy(void *ctx) { const vkn_dev *d = vkn_device(); if (d) vkDestroyDescriptorPool(d->dev, (VkDescriptorPool)(uintptr_t)ctx, NULL); }
static void forget_now(void) {
    for (uint32_t i = 0; i < g_npool; i++) vkn_retire(pool_destroy, (void *)(uintptr_t)g_pool[i]);   // sets bound in flight live 3 more frames
    g_npool = 0;
    if (g_sets) for (uint32_t i = 0; i < SET_TAB; i++) { free(g_sets[i].w); g_sets[i].w = NULL; g_sets[i].nw = 0; g_sets[i].set = VK_NULL_HANDLE; }
    g_nsets = 0; g_st2.set_bytes_words = 0;
    g_b.gset = g_b.cset = VK_NULL_HANDLE; g_b.gndyn = g_b.cndyn = 0;
    g_st2.forget_all++;
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
    g_death = n; g_death_ok = 1;
}
static VkDescriptorPool pool_new(void) {
    const vkn_dev *d = vkn_device(); if (!d || g_npool >= MAX_POOLS) return VK_NULL_HANDLE;
    const VkDescriptorPoolSize sz[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, POOL_SETS * 12}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, POOL_SETS * 12},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, POOL_SETS * 16}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, POOL_SETS * 4},
        {VK_DESCRIPTOR_TYPE_SAMPLER, POOL_SETS * 4}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64}};
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

// Phase 1: textures and storage images of the table. compute: the dispatch path (the pass is already ended; an attachment of the
// current target set is transitioned for reading instead of being replaced). 0 = ok, -1 = drop (counted).
static int bind_textures(const vkn_shader *s, const vkn_draw_in *in, const vkn_img_bind *img, uint32_t nimg, int compute) {
    uint32_t e = 0;
    for (uint32_t i = 0; i < s->nbind; i++) {
        const vkn_bind *b = &s->bind[i];
        for (uint32_t k = 0; k < b->count; k++, e++) {
            if (e >= MAX_ELEMS) { g_st2.dropped_set++; DRAW_RED(R_SET, "shader %#llx: more than %d descriptors", (unsigned long long)s->handle, MAX_ELEMS); return -1; }
            elem *el = &g_el[e]; memset(el, 0, sizeof *el);
            if (b->kind == VKN_BK_TEXTURE) {
                const int st = src_of_stage(b->stage); const uint32_t slot = b->slot + k;
                if (st < 0 || (compute ? st != 2 : st == 2)) { g_st2.dropped_bind++; DRAW_RED(R_BIND, "shader %#llx binding %u: texture of engine stage %u in a %s", (unsigned long long)s->handle, b->binding, b->stage, compute ? "dispatch" : "draw"); return -1; }
                const int32_t id = in->tex[st] && slot < VKN_TEX_SLOTS ? in->tex[st][slot] : -1;
                const int32_t sid = in->smp[st] && slot < VKN_TEX_SLOTS ? in->smp[st][slot] : -1;
                vkn_tex *t = NULL; VkImageView v = id >= 0 ? vkn_tex_view_by_id((uint32_t)id, &t) : VK_NULL_HANDLE;
                VkSampler sm = sid >= 0 ? vkn_sampler_by_id((uint32_t)sid) : VK_NULL_HANDLE;
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
                    if (vkn_pass_sample(t) == 1) {
                        if (compute) vkn_pass_layout(t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        else { use_ph = 1; g_st.feedback_tex++; }
                    }
                }
                if (use_ph) {
                    int cls = PH_FLOAT; v = placeholder(b->flags, &cls);
                    if (!v) { g_st2.placeholder_fail++; DRAW_RED(R_PLACE, "shader %#llx stage %d slot %u: no placeholder for binding flags %#x (draw dropped)", (unsigned long long)s->handle, st, slot, b->flags); return -1; }
                    if (cls == PH_SINT || cls == PH_UINT) { samplers_make(); sm = g_smp_nearest; }
                }
                if (!sm) {
                    if (sid >= 0 || !use_ph) { g_st.unknown_smp++; if (sid >= 0) DRAW_RED(R_SMP, "shader %#llx stage %d slot %u: sampler-pool id %d is not known (default sampler)", (unsigned long long)s->handle, st, slot, sid); }
                    samplers_make(); sm = g_smp_linear;
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
            } else if (b->kind != VKN_BK_UNIFORM && b->kind != VKN_BK_SUPPORT && b->kind != VKN_BK_C1) {
                g_st2.dropped_bind++;
                DRAW_RED(R_BIND, "shader %#llx binding %u: kind %u (storage / driver table / other) has no source in the draw path", (unsigned long long)s->handle, b->binding, b->kind);
                return -1;
            }
        }
    }
    return 0;
}

// Phase 2: uniform blocks, then the key, the cache, the writes. *set / dyn[] / *ndyn out. 0 = ok (set may be NULL: no bindings).
static int build_set(const vkn_shader *s, const vkn_draw_in *in, VkDescriptorSet *out, uint32_t *dyn, uint32_t *ndyn) {
    *out = VK_NULL_HANDLE; *ndyn = 0;
    if (!s->nbind) return 0;
    if (s->ndyn > MAX_DYN) { g_st2.dropped_set++; DRAW_RED(R_SET, "shader %#llx: %u dynamic offsets (max %d)", (unsigned long long)s->handle, s->ndyn, MAX_DYN); return -1; }
    const vkn_dev *d = vkn_device(); if (!d) return -1;
    const uint64_t maxr = d->props.limits.maxUniformBufferRange ? d->props.limits.maxUniformBufferRange : 16384;
    const VkBuffer ring = vkn_ring_buffer(); const uint64_t ring_base = vkn_ring_base();
    uint32_t e = 0, nw = 0;
    g_kw[nw++] = (uint64_t)(uintptr_t)s->set_layout; g_kw[nw++] = s->nbind;
    for (uint32_t i = 0; i < s->nbind; i++) {
        const vkn_bind *b = &s->bind[i];
        const int dynamic = b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        for (uint32_t k = 0; k < b->count; k++, e++) {
            elem *el = &g_el[e];
            if (b->kind == VKN_BK_UNIFORM || b->kind == VKN_BK_SUPPORT || b->kind == VKN_BK_C1) {
                if (b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && !dynamic) { g_st2.dropped_bind++; DRAW_RED(R_BIND, "shader %#llx binding %u: uniform source with descriptor type %d", (unsigned long long)s->handle, b->binding, (int)b->type); return -1; }
                VkBuffer buf = VK_NULL_HANDLE; uint64_t off = 0, size = 0, doff = 0;   // doff: the dynamic offset (dynamic descriptors only)
                const int st = src_of_stage(b->stage);
                if (b->kind == VKN_BK_UNIFORM) {
                    const uint32_t slot = b->slot + k;
                    if (st >= 0 && slot < VKN_UBO_SLOTS && g_ubo[st][slot].valid && ring) {
                        buf = ring; size = g_ubo[st][slot].size;
                        if (dynamic) { off = ring_base; doff = g_ubo[st][slot].off; } else off = ring_base + g_ubo[st][slot].off;
                    }
                } else if (b->kind == VKN_BK_SUPPORT) {
                    vkn_block sb;
                    if (!vkn_shader_support(in->viewport[2], in->viewport[3], &sb)) { buf = sb.buf; off = sb.off; size = sb.size; }
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
    // lookup
    const uint64_t h = vkn_fnv(g_kw, nw * sizeof g_kw[0]);
    if (!g_sets) { g_sets = (set_e *)calloc(SET_TAB, sizeof *g_sets); if (!g_sets) { g_st2.dropped_set++; DRAW_RED(R_SET, "set cache: out of memory"); return -1; } }
    uint32_t pos = (uint32_t)h & (SET_TAB - 1);
    for (uint32_t n = 0; n < SET_TAB; n++, pos = (pos + 1) & (SET_TAB - 1)) {
        set_e *x = &g_sets[pos];
        if (!x->w) break;
        if (x->hash == h && x->nw == nw && !memcmp(x->w, g_kw, nw * sizeof g_kw[0])) { g_st.set_cache++; *out = x->set; return 0; }
    }
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
        if (b->kind == VKN_BK_UNIFORM || b->kind == VKN_BK_SUPPORT || b->kind == VKN_BK_C1) w->pBufferInfo = &g_el[e].buf;
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
            if (!g_sets[pos].w) { g_sets[pos].hash = h; g_sets[pos].w = kw; g_sets[pos].nw = nw; g_sets[pos].set = set; g_nsets++; g_st2.set_bytes_words += nw; kw = NULL; break; }
        free(kw);   // table full (cannot happen below the budget): the set is used once, not cached
    }
    *out = set;
    return 0;
}

// ---- recording state ----
static void on_begin(void) { forget_bound(); g_b.serial = vkn_cb_serial(); g_b.valid = 1; death_check(); }
void vkn_draw_install(void) { vkn_rec_on_begin(on_begin); }
static VkCommandBuffer cb_ready(void) {
    VkCommandBuffer cb = vkn_cb();
    if (!cb) return VK_NULL_HANDLE;
    if (!g_b.valid || g_b.serial != vkn_cb_serial()) on_begin();   // a recording this module was not told about (no listener)
    if (__atomic_exchange_n(&g_forget_req, 0, __ATOMIC_ACQ_REL)) forget_now();
    return cb;
}

// Shader of a draw: both graphics stages present and usable (a stage the engine shader does not have is fine only for fragment).
static const vkn_shader *shader_for(const vkn_draw_in *in, int compute) {
    const vkn_shader *s = in->shader ? vkn_shader_get(in->shader) : NULL;
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
    vkCmdBindDescriptorSets(cb, bp, layout, 0, 1, &set, ndyn, ndyn ? dyn : NULL);
    *bs = set; *bl = layout; *bn = ndyn; memcpy(bd, dyn, ndyn * sizeof *dyn);
    g_st.set_binds++;
}

// Everything of a graphics draw up to the draw command. 0 = ready (*vf / pass info filled), -1 = dropped (counted).
// prim: the primitive the pipeline draws (in->prim, or vkn_prim_rewrite(in->prim) for a rewritten draw).
static int prepare(const vkn_draw_in *in, uint8_t prim, VkCommandBuffer *cb_out, vkn_pass_info *pi, const vkn_vfmt **vf_out) {
    VkCommandBuffer cb = cb_ready();
    if (!cb) { g_st2.dropped_cb++; DRAW_RED(R_CB, "draw without an open recording (dropped)"); return -1; }
    const vkn_shader *s = shader_for(in, 0); if (!s) return -1;
    // state ids (handle 0 = the default: blend off / depth-stencil off / default raster / no vertex input)
    const uint16_t bid = in->blend ? vkn_blend_id(in->blend) : 0, did = in->ds ? vkn_ds_id(in->ds) : 0, rid = in->raster ? vkn_raster_id(in->raster) : 0;
    const uint16_t vid = in->vfmt ? vkn_vfmt_id(in->vfmt) : 0;
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
    const vkn_vfmt *vf = vid ? vkn_vfmt_info(vid) : NULL;
    uint16_t vs_inputs = 0;
    for (uint32_t i = 0; i < s->ninputs && i < 16; i++) if (s->inputs[i] < VKN_VFMT_LOCS) vs_inputs |= (uint16_t)(1u << s->inputs[i]);
    const uint64_t code = retype_code(s, vf);
    const int vsv = variant_vs(code), fsv = (s->stages & (1u << VKN_SRC_FS)) ? variant_fs(in->alpha_ref) : 0;
    if (vsv < 0 || fsv < 0) { g_st2.dropped_variant++; DRAW_RED(R_VARIANT, "more than %d module variants (retype %#llx, alpha reference %g): draw dropped", VARIANTS - 1, (unsigned long long)code, in->alpha_ref); return -1; }
    // vertex / index buffers (resolved before the pass: a drop must not open one)
    VkBuffer vbuf[VKN_VFMT_NULL_BINDING + 1]; VkDeviceSize voff[VKN_VFMT_NULL_BINDING + 1]; uint8_t vuse = 0;
    for (uint32_t i = 0; vf && i < vf->nbind; i++) {
        const uint32_t st = vf->bind[i].binding; if (st >= VKN_VFMT_STREAMS) continue;
        const vkn_block *blk = in->vb[st] ? vkn_buf_get(VKN_BUF_VB, in->vb[st]) : NULL;
        if (!blk || !blk->buf) {
            g_st2.dropped_vb++;
            DRAW_RED(R_VB, "vertex format %#llx reads stream %u, vertex buffer %#llx is not a live native buffer (draw dropped)", (unsigned long long)in->vfmt, st, (unsigned long long)in->vb[st]);
            return -1;
        }
        vbuf[st] = blk->buf; voff[st] = blk->off + in->vb_off[st]; vuse |= (uint8_t)(1u << st);
    }
    if (vs_inputs & ~(vf ? vf->loc_mask : 0)) {
        const vkn_block *nb = null_stream(); if (!nb) { g_st2.dropped_vb++; return -1; }
        vbuf[VKN_VFMT_NULL_BINDING] = nb->buf; voff[VKN_VFMT_NULL_BINDING] = nb->off; vuse |= 1u << VKN_VFMT_NULL_BINDING;
    }
    // textures (vkn_pass_sample may end the open pass), then the pass
    if (bind_textures(s, in, NULL, 0, 0)) return -1;
    if (vkn_pass_ready(pi)) { g_st.dropped_pass++; DRAW_RED(R_PASS, "no render pass for the current targets (shader %#llx, draw dropped)", (unsigned long long)in->shader); return -1; }
    // pipeline
    vkn_pipe_desc pd; memset(&pd, 0, sizeof pd);
    pd.shader = s->id; pd.vs_variant = (uint8_t)vsv; pd.fs_variant = (uint8_t)fsv; pd.blend = bid; pd.ds = did; pd.raster = rid; pd.vfmt = vid;
    pd.prim = prim; pd.stencil_ref = in->stencil_ref; pd.pass = pi->compat;
    for (int i = 0; i < 8; i++) pd.write_mask[i] = (uint8_t)(pi->write_mask[i] & 0xf);
    const vkn_pipe_key key = vkn_pipe_key_pack(&pd);
    if (!g_b.gpipe_ok || memcmp(&key, &g_b.gkey, sizeof key)) {
        VkPipeline p = vkn_pipe_get(&key, NULL);
        if (!p) {
            vkn_pipe_src src; memset(&src, 0, sizeof src);
            src.vs = vkn_shader_module(s, VKN_STAGE_VS, 0, 0.0f, code);
            src.fs = (s->stages & (1u << VKN_SRC_FS)) ? vkn_shader_module(s, VKN_STAGE_FS, fsv ? 5u : 0u, fsv ? in->alpha_ref : 0.0f, 0) : VK_NULL_HANDLE;
            src.layout = s->layout; src.pass = pi->pass; src.vs_inputs = vs_inputs;
            if (src.vs && (src.fs || !(s->stages & (1u << VKN_SRC_FS)))) p = vkn_pipe_get(&key, &src);
        }
        if (!p) {
            g_st.dropped_pipe++;
            DRAW_RED(R_PIPE, "no pipeline: shader %#llx (%s / %s) blend %u ds %u raster %u vfmt %u prim %u pass %u, variants vs %d fs %d (draw dropped)",
                     (unsigned long long)in->shader, s->key[0], s->key[1], bid, did, rid, vid, prim, pi->compat, vsv, fsv);
            return -1;
        }
        if (p != g_b.gpipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); g_st.pipe_binds++; }
        g_b.gpipe = p; g_b.gkey = key; g_b.gpipe_ok = 1;
    }
    // descriptor set
    VkDescriptorSet set; uint32_t dyn[MAX_DYN], ndyn = 0;
    memset(dyn, 0, sizeof dyn);
    if (build_set(s, in, &set, dyn, &ndyn)) return -1;
    bind_set(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->layout, set, dyn, ndyn);
    // vertex buffers
    for (uint32_t b = 0; b <= VKN_VFMT_NULL_BINDING; b++) {
        if (!(vuse >> b & 1) || (g_b.vb[b] == vbuf[b] && g_b.vb_off[b] == voff[b])) continue;
        vkCmdBindVertexBuffers(cb, b, 1, &vbuf[b], &voff[b]);
        g_b.vb[b] = vbuf[b]; g_b.vb_off[b] = voff[b]; g_st.vb_binds++;
    }
    // viewport (no flip: NVN's origin is upper left and its depth 0..1, as Vulkan's) and scissor clamped to the pass
    const VkViewport vp = {in->viewport[0], in->viewport[1], in->viewport[2], in->viewport[3], 0.0f, 1.0f};
    if (!g_b.vp_ok || memcmp(&vp, &g_b.vp, sizeof vp)) { vkCmdSetViewport(cb, 0, 1, &vp); g_b.vp = vp; g_b.vp_ok = 1; g_st.viewport_sets++; }
    int64_t x0 = in->scissor[0], y0 = in->scissor[1], x1 = x0 + in->scissor[2], y1 = y0 + in->scissor[3];
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int64_t)pi->width) x1 = pi->width;
    if (y1 > (int64_t)pi->height) y1 = pi->height;
    if (x1 <= x0 || y1 <= y0) { g_st2.scissor_empty++; return 1; }
    const VkRect2D sc = {{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    if (!g_b.sc_ok || memcmp(&sc, &g_b.sc, sizeof sc)) { vkCmdSetScissor(cb, 0, 1, &sc); g_b.sc = sc; g_b.sc_ok = 1; }
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
    const int r = prepare(in, q, &cb, &pi, &vf);
    if (r) return r < 0 ? 1 : 0;
    VkBuffer buf; VkDeviceSize off;
    void *dst = rw_alloc(m * (out32 ? 4u : 2u), &buf, &off);
    if (!dst) {
        g_rw_st.dropped++;
        DRAW_RED(R_RW, "rewrite of engine primitive %u: no transient index block for %llu indices (draw dropped)", in->prim, (unsigned long long)m);
        return 1;
    }
    rw_fill(k, src, isz, n, dst, out32);
    const VkIndexType it = out32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    if (g_b.ib != buf || g_b.ib_off != off || g_b.ib_type != it) {   // the engine's next indexed draw sees the difference and rebinds
        vkCmdBindIndexBuffer(cb, buf, off, it);
        g_b.ib = buf; g_b.ib_off = off; g_b.ib_type = it; g_st.ib_binds++;
    }
    vkCmdDrawIndexed(cb, (uint32_t)m, instances ? instances : 1, 0, ix ? 0 : (int32_t)first_vertex, 0);
    g_st.draws++;
    if (ix) g_st.indexed++; else g_st.arrays++;
    g_rw_st.draws[ix][k]++; g_rw_st.indices += m;
    return 0;
}
void vkn_draw_rw_stats_get(vkn_draw_rw_stats *s) { if (s) *s = g_rw_st; }

int vkn_draw_indexed(const vkn_draw_in *in, uint32_t first_index, uint32_t count, uint32_t instances) {
    if (!in) return 1;
    if (!count) return 0;
    // the index buffer first: no pass or pipeline work for a draw that cannot be made
    vkn_buf_info ib; memset(&ib, 0, sizeof ib);
    if (!in->ib || vkn_buf_lookup(VKN_BUF_IB, in->ib, &ib) || !ib.blk.size || !ib.blk.buf) {
        g_st2.dropped_ib++; DRAW_RED(R_IB, "index buffer %#llx is not a live native buffer (draw of %u indices dropped)", (unsigned long long)in->ib, count); return 1;
    }
    const uint32_t isz = ib.index_size == 4 ? 4 : 2;
    if (((uint64_t)first_index + count) * isz > ib.size) {
        g_st2.dropped_ib++;
        DRAW_RED(R_IB, "index buffer %#llx: indices %u + %u of %u bytes each exceed its %llu bytes (draw dropped)", (unsigned long long)in->ib, first_index, count, isz, (unsigned long long)ib.size);
        return 1;
    }
    if (rw_kind(in->prim) >= 0) {   // the engine's indices as it wrote them through the buffer's CPU pointer (mapped, coherent)
        if (!ib.blk.cpu) {
            g_rw_st.dropped++;
            DRAW_RED(R_RW, "index buffer %#llx has no CPU pointer: no rewrite of engine primitive %u (draw dropped)", (unsigned long long)in->ib, in->prim);
            return 1;
        }
        return draw_rewritten(in, (const uint8_t *)ib.blk.cpu + (uint64_t)first_index * isz, isz, 0, count, instances);
    }
    VkCommandBuffer cb; vkn_pass_info pi; const vkn_vfmt *vf;
    const int r = prepare(in, in->prim, &cb, &pi, &vf);
    if (r) return r < 0 ? 1 : 0;
    const VkIndexType it = vkn_buf_index_type(ib.index_size);
    if (g_b.ib != ib.blk.buf || g_b.ib_off != ib.blk.off || g_b.ib_type != it) {
        vkCmdBindIndexBuffer(cb, ib.blk.buf, ib.blk.off, it);
        g_b.ib = ib.blk.buf; g_b.ib_off = ib.blk.off; g_b.ib_type = it; g_st.ib_binds++;
    }
    vkCmdDrawIndexed(cb, count, instances ? instances : 1, first_index, 0, 0);
    g_st.draws++; g_st.indexed++;
    return 0;
}

int vkn_draw_arrays(const vkn_draw_in *in, uint32_t first, uint32_t count) {
    if (!in) return 1;
    if (!count) return 0;
    if (rw_kind(in->prim) >= 0) return draw_rewritten(in, NULL, 0, first, count, 1);
    VkCommandBuffer cb; vkn_pass_info pi; const vkn_vfmt *vf;
    const int r = prepare(in, in->prim, &cb, &pi, &vf);
    if (r) return r < 0 ? 1 : 0;
    vkCmdDraw(cb, count, 1, first, 0);
    g_st.draws++; g_st.arrays++;
    return 0;
}

int vkn_draw_dispatch(const vkn_draw_in *in, const vkn_img_bind *img, uint32_t nimg, uint32_t x, uint32_t y, uint32_t z) {
    if (!in) return 1;
    VkCommandBuffer cb = cb_ready();
    if (!cb) { g_st2.dropped_cb++; DRAW_RED(R_CB, "dispatch without an open recording (dropped)"); return 1; }
    const vkn_shader *s = shader_for(in, 1); if (!s) return 1;
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

void vkn_draw_stats_get(vkn_draw_stats *s) { if (s) *s = g_st; }
void vkn_draw_stats2_get(vkn_draw_stats2 *s) { if (s) *s = g_st2; }
#endif
