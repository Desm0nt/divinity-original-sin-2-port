// [DOS2] VK-NATIVE stage 2 host test: the mock of everything dm/gfx/vkn/*.c reaches outside itself (included ONCE, by
// vkn_stage2_test.c). See that file's header for the build line.
//   - the Vulkan entry points the five vkn files call: fake non-null handles from one registry (kind, live, the create info copied),
//     counters of creates / destroys per kind, double / foreign destroys counted as mock errors; vkMapMemory hands out calloc'ed
//     memory of host-visible types; memory requirements echo the buffer size; format properties configurable (BC sampled or not,
//     3-component formats, D24 attachments, 3-component vertex formats); command buffers record barriers, buffer -> image copies
//     (with a pointer to the staged bytes) and image copies into logs the test reads; vkQueueSubmit checks the queue lock;
//   - the front end's doors: dm_vk_handles (a device can be switched off), dm_present_queue_lock / unlock (depth counted);
//   - the translator's doors (xbg_game.h / xbg_game_internal.h / xbg_vulkan.h): xbg_shader_program_key (the key is the first 16
//     characters of the control section; '!' = no key), xbg_vkn_shader (synthetic bundles registered by the test, an optional gate
//     that holds the worker inside the call), the alpha / retype variant getters (logged with their thread), xbg_support_defaults.
// Stage 3 additions (used by vkn_stage3_test.c; stage 2 does not turn them on):
//   - one ordered command log of every vkCmd* call, command buffer begin / end, submit and the presenter's doors (mk_ev: call + the key
//     arguments, the queue-lock depth at the call), with the command buffer of each entry;
//   - fences signal at the submit (default) or, with mk_fence_hold, stay pending until a wait (the GPU finishes, in queue order, when
//     somebody waits; mk_fence_stuck: a wait times out); a command buffer is "in flight" while its submission's fence is pending, and
//     destroying an object a pending submission uses, or resetting / re-beginning a pending buffer, is a mock error;
//   - descriptor pools / sets with their writes, query pools with timestamps, failure injection for begin / end / submit / set allocation;
//   - mk_validate_on: a validator replays each command buffer's log at its submit, in submission order (= GPU order): per-image layout
//     tracking (every image starts UNDEFINED), render pass begin / end pairing, commands legal inside / outside a pass, attachment and
//     descriptor layouts, feedback (an image sampled while it is an attachment of the pass), pipeline / pass compatibility, set /
//     pipeline layout match, dynamic offset count and range, bound vertex / index buffers, viewport / scissor inside the render area.
#ifndef VKN_MOCK_VK_H
#define VKN_MOCK_VK_H
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "../xbg_game.h"

// ---------------------------------------------------------------- registry
enum { MK_BUFFER, MK_MEMORY, MK_IMAGE, MK_VIEW, MK_SAMPLER, MK_DSL, MK_PL, MK_RP, MK_FB, MK_PIPE, MK_PCACHE, MK_POOL, MK_CB, MK_FENCE,
       MK_DPOOL, MK_DSET, MK_QPOOL, MK_SMOD, MK_KINDS };
static const char *const mk_kind_name[MK_KINDS] = {"buffer", "memory", "image", "view", "sampler", "set layout", "pipeline layout", "render pass",
                                                   "framebuffer", "pipeline", "pipeline cache", "command pool", "command buffer", "fence",
                                                   "descriptor pool", "descriptor set", "query pool", "shader module"};
#define MK_U __attribute__((unused))
typedef struct mk_dw {   // one written descriptor (stage 3)
    uint32_t binding, elem; VkDescriptorType type;
    VkImageView view; VkSampler smp; VkImageLayout layout;
    VkBuffer buf; VkDeviceSize off, range;
} mk_dw;
enum { MK_DW_MAX = 128 };
enum { MK_MAX_OBJ = 1 << 16 };
#define MK_BASE 0x7a0000000ull
#define MK_STEP 0x10ull

typedef struct mk_rp {   // a render pass create info, deep
    uint32_t natt; VkAttachmentDescription att[9];
    uint32_t ncolor; VkAttachmentReference color[8]; int has_depth; VkAttachmentReference depth;
    uint32_t ndep; VkSubpassDependency dep[4];
} mk_rp;
typedef struct mk_gp {   // a graphics pipeline create info, deep
    uint32_t nst; VkShaderStageFlagBits stage[2]; VkShaderModule module[2]; char entry[2][8];
    uint32_t nbind; VkVertexInputBindingDescription bind[8]; uint32_t nattr; VkVertexInputAttributeDescription attr[32];
    VkPrimitiveTopology topo; VkBool32 restart;
    uint32_t nvp, nsc;
    VkPipelineRasterizationStateCreateInfo rs; VkSampleCountFlagBits samples;
    int has_ds; VkPipelineDepthStencilStateCreateInfo ds;
    uint32_t natt; VkPipelineColorBlendAttachmentState att[8];
    uint32_t ndyn; VkDynamicState dyn[8];
    VkPipelineLayout layout; VkRenderPass pass; uint32_t subpass; VkPipelineCache cache;
} mk_gp;
typedef struct mk_obj {
    int kind, live;
    uint64_t size; uint32_t mem_type, usage;   // buffer: size, usage; memory: size, type
    void *cpu;                                 // memory: host backing (host-visible types); buffer: the bound memory's backing
    uint64_t bound;                            // buffer / image: memory handle bound
    int mapped;
    VkImageCreateInfo ici; VkImageViewCreateInfo vci; VkSamplerCreateInfo sci;
    uint32_t nb; VkDescriptorSetLayoutBinding b[64];              // set layout
    uint32_t nset; VkDescriptorSetLayout set0;                    // pipeline layout
    mk_rp rp;
    VkRenderPass fb_pass; uint32_t fb_n, fb_w, fb_h, fb_layers; VkImageView fb_view[9];
    int compute; mk_gp gp; VkShaderModule cs_module; VkPipelineLayout cs_layout;
    int cb_state; uint32_t pool_family;                           // command buffer: 0 initial, 1 recording, 2 executable
    int signaled;                                                 // fence
    // stage 3
    VkImageLayout gpu_layout;                                     // image: layout as the GPU executes the submissions (validator)
    uint64_t ev_first; int began;                                 // command buffer: log sequence number of its last begin
    uint64_t pend_seq;                                            // command buffer / fence: submission sequence while in flight (0 = not)
    uint64_t *uses; uint32_t nuses, capuses;                      // command buffer: objects its last submitted recording uses
    uint32_t max_sets, nsets;                                     // descriptor pool
    VkDescriptorSetLayout set_layout; uint64_t dpool; mk_dw *dw; uint32_t ndw;   // descriptor set
    uint64_t qv[64];                                              // query pool: values
} mk_obj;

static pthread_mutex_t mk_mu = PTHREAD_MUTEX_INITIALIZER;
static mk_obj *mk_obj_tab[MK_MAX_OBJ]; static uint32_t mk_nobj;
static uint64_t mk_created[MK_KINDS], mk_destroyed[MK_KINDS];
static int mk_errors;
static void mk_err(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); printf("MOCK ERROR: "); vprintf(fmt, ap); printf("\n"); va_end(ap);
    __atomic_add_fetch(&mk_errors, 1, __ATOMIC_RELAXED);
}
static uint64_t mk_new(int kind, mk_obj **out) {   // under mk_mu
    if (mk_nobj >= MK_MAX_OBJ) { mk_err("registry full"); *out = NULL; return 0; }
    mk_obj *o = (mk_obj *)calloc(1, sizeof *o);
    o->kind = kind; o->live = 1;
    mk_obj_tab[mk_nobj] = o; mk_created[kind]++;
    *out = o;
    return MK_BASE + MK_STEP * mk_nobj++;
}
static mk_obj *mk_find(uint64_t h, int kind) {   // under mk_mu; NULL for a handle that is not a live object of this kind
    if (h < MK_BASE || (h - MK_BASE) % MK_STEP) return NULL;
    const uint64_t i = (h - MK_BASE) / MK_STEP;
    if (i >= mk_nobj) return NULL;
    mk_obj *o = mk_obj_tab[i];
    return o->kind == kind && o->live ? o : NULL;
}
static mk_obj *mk_any(uint64_t h) {   // under mk_mu; dead ones too
    if (h < MK_BASE || (h - MK_BASE) % MK_STEP) return NULL;
    const uint64_t i = (h - MK_BASE) / MK_STEP;
    return i < mk_nobj ? mk_obj_tab[i] : NULL;
}
// ---------------------------------------------------------------- stage 3: the command log
enum { MK_E_BEGIN_CB = 1, MK_E_END_CB, MK_E_SUBMIT, MK_E_PRESENT, MK_E_PRESENT_WAIT, MK_E_BARRIER, MK_E_COPY_BUF_IMG, MK_E_COPY_IMG, MK_E_BLIT_IMG,
       MK_E_CLEAR_COLOR_IMG, MK_E_CLEAR_DS_IMG, MK_E_BEGIN_RP, MK_E_END_RP, MK_E_CLEAR_ATT, MK_E_BIND_PIPE, MK_E_BIND_SETS, MK_E_BIND_VB,
       MK_E_BIND_IB, MK_E_VIEWPORT, MK_E_SCISSOR, MK_E_DRAW, MK_E_DRAW_IDX, MK_E_DISPATCH, MK_E_RESET_QP, MK_E_TIMESTAMP, MK_E_WAIT, MK_E_N };
static const char *const mk_ev_name[MK_E_N] MK_U = {"?", "begin", "end", "submit", "present", "present-wait", "barrier", "copy-buf-img", "copy-img",
    "blit", "clear-color-img", "clear-ds-img", "begin-rp", "end-rp", "clear-att", "bind-pipe", "bind-sets", "bind-vb", "bind-ib", "viewport",
    "scissor", "draw", "draw-idx", "dispatch", "reset-qp", "timestamp", "fence-wait"};
typedef struct mk_ev {
    int op; VkCommandBuffer cb; uint64_t seq;
    uint64_t a[4];                 // handles (image / buffer / pipeline / set / pass / framebuffer / fence), by op
    uint32_t u[8];                 // small integers, by op
    int qlock;                     // queue-lock depth at the call
    VkImageLayout l0, l1;          // barrier old / new; copy / blit source / destination; clear-image layout
    VkImageSubresourceRange r;     // barrier / clear-image range
    VkRect2D rect; VkViewport vp;  // render area / scissor; viewport
    uint32_t n;                    // count of the arrays below in use
    VkClearValue cv[9];            // begin-rp clear values; clear-image value (cv[0])
    VkClearAttachment ca[4]; VkClearRect cr[2];
    uint32_t dyn[16];              // bind-sets dynamic offsets
    VkDeviceSize off[4];           // bind-vb offsets, bind-ib offset
    VkImageCopy ic[4]; VkImageBlit ib[4]; VkFilter filter;
} mk_ev;
enum { MK_EVS = 1 << 14 };
static mk_ev *mk_evs; static uint64_t mk_seq, mk_ev_base;   // entries [mk_ev_base, mk_seq) are in mk_evs[seq - mk_ev_base]
static int mk_validate_on, mk_val_errors, mk_fence_hold, mk_fence_stuck;
static uint64_t mk_sub_seq;                                    // submissions so far
static int mk_fail_begin, mk_fail_end, mk_fail_submit, mk_fail_dset, mk_fail_cpool;   // the next N calls fail
static uint64_t mk_ts_clock;
static mk_ev *mk_log(VkCommandBuffer cb, int op) {   // under mk_mu; never NULL
    static mk_ev spare;
    if (!mk_evs) mk_evs = (mk_ev *)calloc(MK_EVS, sizeof *mk_evs);
    if (mk_seq - mk_ev_base >= MK_EVS) { mk_err("command log full (%llu entries)", (unsigned long long)(mk_seq - mk_ev_base)); memset(&spare, 0, sizeof spare); return &spare; }
    mk_ev *e = &mk_evs[mk_seq - mk_ev_base]; memset(e, 0, sizeof *e);
    e->op = op; e->cb = cb; e->seq = mk_seq++; e->qlock = -1;
    return e;
}
static mk_ev *mk_ev_at(uint64_t seq) { return seq >= mk_ev_base && seq < mk_seq ? &mk_evs[seq - mk_ev_base] : NULL; }
static int mk_inflight_uses(uint64_t h);
static void mk_del(uint64_t h, int kind) {
    if (!h) return;   // destroying VK_NULL_HANDLE is legal
    pthread_mutex_lock(&mk_mu);
    mk_obj *o = mk_find(h, kind);
    if (o && kind != MK_MEMORY && kind != MK_CB && kind != MK_FENCE && kind != MK_POOL && mk_inflight_uses(h))
        mk_err("destroy of %s %#llx while a pending submission uses it", mk_kind_name[kind], (unsigned long long)h);
    if (!o) mk_err("destroy of %s %#llx that is not a live %s", mk_kind_name[kind], (unsigned long long)h, mk_kind_name[kind]);
    else {
        o->live = 0; mk_destroyed[kind]++; if (kind == MK_MEMORY) { free(o->cpu); o->cpu = NULL; }
        if (kind == MK_DPOOL)   // its sets die with it
            for (uint32_t i = 0; i < mk_nobj; i++) { mk_obj *s = mk_obj_tab[i]; if (s->kind == MK_DSET && s->live && s->dpool == h) { s->live = 0; mk_destroyed[MK_DSET]++; } }
    }
    pthread_mutex_unlock(&mk_mu);
}
// public helpers for the test
static mk_obj *mk_obj_of(uint64_t h) { pthread_mutex_lock(&mk_mu); mk_obj *o = mk_any(h); pthread_mutex_unlock(&mk_mu); return o; }
static MK_U int mk_is_live(uint64_t h, int kind) { pthread_mutex_lock(&mk_mu); const int r = mk_find(h, kind) != NULL; pthread_mutex_unlock(&mk_mu); return r; }
static uint64_t mk_live(int kind) { return mk_created[kind] - mk_destroyed[kind]; }
static MK_U uint64_t mk_last(int kind) {   // the newest object of a kind (0 = none)
    pthread_mutex_lock(&mk_mu); uint64_t h = 0;
    for (uint32_t i = mk_nobj; i-- > 0;) if (mk_obj_tab[i]->kind == kind) { h = MK_BASE + MK_STEP * i; break; }
    pthread_mutex_unlock(&mk_mu); return h;
}
// The live views of an image, in creation order (up to n).
static MK_U uint32_t mk_views_of(VkImage img, VkImageView *out, uint32_t n) {
    uint32_t k = 0; pthread_mutex_lock(&mk_mu);
    for (uint32_t i = 0; i < mk_nobj; i++) { mk_obj *o = mk_obj_tab[i]; if (o->kind == MK_VIEW && o->live && o->vci.image == img && k < n) out[k++] = (VkImageView)(uintptr_t)(MK_BASE + MK_STEP * i); }
    pthread_mutex_unlock(&mk_mu); return k;
}

// ---------------------------------------------------------------- configuration
static int mk_dev_on = 1;               // dm_vk_handles reports a device
static int mk_bc = 1;                   // BC formats sampled (optimal tiling)
static int mk_rgb3 = 0;                 // 3-component 8 / 16 / 32-bit formats sampled
static int mk_d24 = 1;                  // X8_D24 / D24S8 usable as depth attachments
static int mk_vtx3 = 1;                 // 3-component 8 / 16-bit vertex formats
static uint32_t mk_dyn_limit = 8;       // maxDescriptorSetUniformBuffersDynamic
static int mk_blit = 0;                 // stage 3: colour / depth formats report BLIT_SRC / BLIT_DST (optimal tiling)
static int mk_qlock_depth, mk_qlocks, mk_submits;
#define MK_BAD_MODULE ((VkShaderModule)(uintptr_t)0xbad0000ull)

static int mk_fmt_bc(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }
static int mk_fmt_rgb3(VkFormat f) {
    return (f >= VK_FORMAT_R8G8B8_UNORM && f <= VK_FORMAT_B8G8R8_SRGB) || (f >= VK_FORMAT_R16G16B16_UNORM && f <= VK_FORMAT_R16G16B16_SFLOAT) ||
           (f >= VK_FORMAT_R32G32B32_UINT && f <= VK_FORMAT_R32G32B32_SFLOAT);
}
static int mk_fmt_rgb3_small(VkFormat f) { return (f >= VK_FORMAT_R8G8B8_UNORM && f <= VK_FORMAT_B8G8R8_SRGB) || (f >= VK_FORMAT_R16G16B16_UNORM && f <= VK_FORMAT_R16G16B16_SFLOAT); }
static int mk_fmt_depth(VkFormat f) { return f >= VK_FORMAT_D16_UNORM && f <= VK_FORMAT_D32_SFLOAT_S8_UINT; }
static int mk_fmt_int(VkFormat f) {
    static const VkFormat k[] = {VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
        VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT,
        VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT,
        VK_FORMAT_A2B10G10R10_UINT_PACK32};
    for (unsigned i = 0; i < sizeof k / sizeof *k; i++) if (k[i] == f) return 1;
    return 0;
}

// ---------------------------------------------------------------- front end doors
int dm_vk_handles(VkInstance *i, VkPhysicalDevice *p, VkDevice *d, VkQueue *q, uint32_t *f) {
    if (!mk_dev_on) return 0;
    *i = (VkInstance)(uintptr_t)0x1110; *p = (VkPhysicalDevice)(uintptr_t)0x2220; *d = (VkDevice)(uintptr_t)0x3330; *q = (VkQueue)(uintptr_t)0x4440; *f = 2;
    return 1;
}
#define MK_DEV ((VkDevice)(uintptr_t)0x3330)
#define MK_PHYS ((VkPhysicalDevice)(uintptr_t)0x2220)
#define MK_QUEUE ((VkQueue)(uintptr_t)0x4440)
void dm_present_queue_lock(void) { pthread_mutex_lock(&mk_mu); mk_qlock_depth++; mk_qlocks++; pthread_mutex_unlock(&mk_mu); }
void dm_present_queue_unlock(void) {
    pthread_mutex_lock(&mk_mu); if (mk_qlock_depth <= 0) mk_err("queue unlock without lock"); else mk_qlock_depth--; pthread_mutex_unlock(&mk_mu);
}

// ---------------------------------------------------------------- physical device
static void mk_dev_check(VkDevice d, const char *fn) { if (d != MK_DEV) mk_err("%s on device %p", fn, (void *)d); }
void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice p, VkPhysicalDeviceMemoryProperties *m) {
    if (p != MK_PHYS) mk_err("memory properties of %p", (void *)p);
    memset(m, 0, sizeof *m);
    m->memoryTypeCount = 3;
    m->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    m->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    m->memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    m->memoryHeapCount = 1; m->memoryHeaps[0].size = 8ull << 30; m->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
}
void vkGetPhysicalDeviceProperties(VkPhysicalDevice p, VkPhysicalDeviceProperties *o) {
    (void)p; memset(o, 0, sizeof *o);
    o->apiVersion = VK_API_VERSION_1_1; o->vendorID = 0x5143; o->driverVersion = 1; strcpy(o->deviceName, "mock GPU");
    o->limits.maxSamplerAnisotropy = 16.0f; o->limits.maxDescriptorSetUniformBuffersDynamic = mk_dyn_limit; o->limits.minUniformBufferOffsetAlignment = 64;
}
void vkGetPhysicalDeviceFeatures(VkPhysicalDevice p, VkPhysicalDeviceFeatures *f) {
    (void)p; memset(f, 0, sizeof *f);
    f->samplerAnisotropy = VK_TRUE; f->depthBiasClamp = VK_TRUE; f->fillModeNonSolid = VK_TRUE;
}
void vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice p, VkFormat f, VkFormatProperties *o) {
    if (p != MK_PHYS) mk_err("format properties of %p", (void *)p);
    memset(o, 0, sizeof *o);
    const VkFormatFeatureFlags xfer = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    VkFormatFeatureFlags v;
    if (mk_fmt_depth(f)) {
        v = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | xfer;
        if (!mk_d24 && (f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D24_UNORM_S8_UINT)) v = 0;
    } else if (mk_fmt_bc(f)) v = mk_bc ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | xfer : 0;
    else if (mk_fmt_rgb3(f)) v = mk_rgb3 ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | xfer : 0;
    else {
        v = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | xfer;
        if (!mk_fmt_int(f)) v |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
    }
    if (mk_blit && v) v |= VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    o->optimalTilingFeatures = v;
    o->bufferFeatures = (mk_fmt_rgb3_small(f) && !mk_vtx3) ? 0 : VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
}

// ---------------------------------------------------------------- memory, buffers, images
VkResult vkCreateBuffer(VkDevice d, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out) {
    (void)a; mk_dev_check(d, "vkCreateBuffer");
    if (ci->sType != VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO || !ci->size) mk_err("vkCreateBuffer: bad create info");
    pthread_mutex_lock(&mk_mu); mk_obj *o; const uint64_t h = mk_new(MK_BUFFER, &o); o->size = ci->size; o->usage = ci->usage; pthread_mutex_unlock(&mk_mu);
    *out = (VkBuffer)(uintptr_t)h; return VK_SUCCESS;
}
void vkDestroyBuffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a) { (void)a; mk_dev_check(d, "vkDestroyBuffer"); mk_del((uint64_t)(uintptr_t)b, MK_BUFFER); }
void vkGetBufferMemoryRequirements(VkDevice d, VkBuffer b, VkMemoryRequirements *r) {
    (void)d; pthread_mutex_lock(&mk_mu); mk_obj *o = mk_find((uint64_t)(uintptr_t)b, MK_BUFFER);
    if (!o) mk_err("buffer requirements of a dead buffer");
    r->size = o ? (o->size + 255) & ~255ull : 256; r->alignment = 256; r->memoryTypeBits = 0x7;
    pthread_mutex_unlock(&mk_mu);
}
void vkGetImageMemoryRequirements(VkDevice d, VkImage img, VkMemoryRequirements *r) {
    (void)d; pthread_mutex_lock(&mk_mu); mk_obj *o = mk_find((uint64_t)(uintptr_t)img, MK_IMAGE);
    if (!o) mk_err("image requirements of a dead image");
    const uint64_t px = o ? (uint64_t)o->ici.extent.width * o->ici.extent.height * o->ici.extent.depth * o->ici.arrayLayers * o->ici.samples : 1;
    r->size = (px * 16 * 2 + 4095) & ~4095ull; r->alignment = 4096; r->memoryTypeBits = 0x7;
    pthread_mutex_unlock(&mk_mu);
}
VkResult vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    (void)a; mk_dev_check(d, "vkAllocateMemory");
    if (ai->memoryTypeIndex >= 3 || !ai->allocationSize) mk_err("vkAllocateMemory: type %u size %llu", ai->memoryTypeIndex, (unsigned long long)ai->allocationSize);
    pthread_mutex_lock(&mk_mu); mk_obj *o; const uint64_t h = mk_new(MK_MEMORY, &o); o->size = ai->allocationSize; o->mem_type = ai->memoryTypeIndex;
    if (ai->memoryTypeIndex == 1 || ai->memoryTypeIndex == 2) o->cpu = calloc(1, (size_t)ai->allocationSize);
    pthread_mutex_unlock(&mk_mu);
    *out = (VkDeviceMemory)(uintptr_t)h; return VK_SUCCESS;
}
void vkFreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) { (void)a; mk_dev_check(d, "vkFreeMemory"); mk_del((uint64_t)(uintptr_t)m, MK_MEMORY); }
VkResult vkBindBufferMemory(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize off) {
    (void)d; pthread_mutex_lock(&mk_mu);
    mk_obj *ob = mk_find((uint64_t)(uintptr_t)b, MK_BUFFER), *om = mk_find((uint64_t)(uintptr_t)m, MK_MEMORY);
    if (!ob || !om || off || ob->bound) mk_err("vkBindBufferMemory: bad arguments");
    else { ob->bound = (uint64_t)(uintptr_t)m; ob->cpu = om->cpu; if (om->size < ob->size) mk_err("memory smaller than the buffer"); }
    pthread_mutex_unlock(&mk_mu); return VK_SUCCESS;
}
VkResult vkBindImageMemory(VkDevice d, VkImage img, VkDeviceMemory m, VkDeviceSize off) {
    (void)d; pthread_mutex_lock(&mk_mu);
    mk_obj *oi = mk_find((uint64_t)(uintptr_t)img, MK_IMAGE), *om = mk_find((uint64_t)(uintptr_t)m, MK_MEMORY);
    if (!oi || !om || off || oi->bound) mk_err("vkBindImageMemory: bad arguments"); else oi->bound = (uint64_t)(uintptr_t)m;
    pthread_mutex_unlock(&mk_mu); return VK_SUCCESS;
}
VkResult vkMapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize off, VkDeviceSize size, VkMemoryMapFlags fl, void **pp) {
    (void)d; (void)fl; pthread_mutex_lock(&mk_mu); mk_obj *om = mk_find((uint64_t)(uintptr_t)m, MK_MEMORY); VkResult r = VK_SUCCESS;
    if (!om || !om->cpu || om->mapped || (size != VK_WHOLE_SIZE && off + size > om->size)) { mk_err("vkMapMemory: bad arguments"); r = VK_ERROR_MEMORY_MAP_FAILED; *pp = NULL; }
    else { om->mapped = 1; *pp = (uint8_t *)om->cpu + off; }
    pthread_mutex_unlock(&mk_mu); return r;
}
VkResult vkCreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out) {
    (void)a; mk_dev_check(d, "vkCreateImage");
    if (ci->sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO || !ci->extent.width || !ci->extent.height || !ci->extent.depth || !ci->mipLevels || !ci->arrayLayers || ci->format == VK_FORMAT_UNDEFINED)
        mk_err("vkCreateImage: bad create info");
    if ((ci->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && (ci->extent.width != ci->extent.height || ci->arrayLayers % 6)) mk_err("vkCreateImage: cube flag on a non-cube shape");
    pthread_mutex_lock(&mk_mu); mk_obj *o; const uint64_t h = mk_new(MK_IMAGE, &o); o->ici = *ci; o->ici.pNext = NULL; o->ici.pQueueFamilyIndices = NULL; pthread_mutex_unlock(&mk_mu);
    *out = (VkImage)(uintptr_t)h; return VK_SUCCESS;
}
void vkDestroyImage(VkDevice d, VkImage img, const VkAllocationCallbacks *a) { (void)a; mk_dev_check(d, "vkDestroyImage"); mk_del((uint64_t)(uintptr_t)img, MK_IMAGE); }
VkResult vkCreateImageView(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out) {
    (void)a; mk_dev_check(d, "vkCreateImageView");
    pthread_mutex_lock(&mk_mu);
    mk_obj *oi = mk_find((uint64_t)(uintptr_t)ci->image, MK_IMAGE);
    if (!oi) mk_err("vkCreateImageView of a dead image");
    else {
        const VkImageSubresourceRange *r = &ci->subresourceRange;
        if (!r->levelCount || !r->layerCount || r->baseMipLevel + r->levelCount > oi->ici.mipLevels || r->baseArrayLayer + r->layerCount > oi->ici.arrayLayers)
            mk_err("vkCreateImageView: range outside the image (levels %u+%u of %u, layers %u+%u of %u)", r->baseMipLevel, r->levelCount, oi->ici.mipLevels,
                   r->baseArrayLayer, r->layerCount, oi->ici.arrayLayers);
        if ((ci->viewType == VK_IMAGE_VIEW_TYPE_CUBE || ci->viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) && !(oi->ici.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)) mk_err("cube view of a non-cube image");
        if (!oi->bound) mk_err("view of an image without memory");
    }
    mk_obj *o; const uint64_t h = mk_new(MK_VIEW, &o); o->vci = *ci; o->vci.pNext = NULL;
    pthread_mutex_unlock(&mk_mu);
    *out = (VkImageView)(uintptr_t)h; return VK_SUCCESS;
}
void vkDestroyImageView(VkDevice d, VkImageView v, const VkAllocationCallbacks *a) { (void)a; mk_dev_check(d, "vkDestroyImageView"); mk_del((uint64_t)(uintptr_t)v, MK_VIEW); }
VkResult vkCreateSampler(VkDevice d, const VkSamplerCreateInfo *ci, const VkAllocationCallbacks *a, VkSampler *out) {
    (void)a; mk_dev_check(d, "vkCreateSampler");
    pthread_mutex_lock(&mk_mu); mk_obj *o; const uint64_t h = mk_new(MK_SAMPLER, &o); o->sci = *ci; o->sci.pNext = NULL; pthread_mutex_unlock(&mk_mu);
    *out = (VkSampler)(uintptr_t)h; return VK_SUCCESS;
}
void vkDestroySampler(VkDevice d, VkSampler s, const VkAllocationCallbacks *a) { (void)a; mk_dev_check(d, "vkDestroySampler"); mk_del((uint64_t)(uintptr_t)s, MK_SAMPLER); }

// ---------------------------------------------------------------- layouts, passes, framebuffers, pipelines
VkResult vkCreateDescriptorSetLayout(VkDevice d, const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
    (void)a; mk_dev_check(d, "vkCreateDescriptorSetLayout");
    pthread_mutex_lock(&mk_mu); mk_obj *o; const uint64_t h = mk_new(MK_DSL, &o);
    o->nb = ci->bindingCount > 64 ? 64 : ci->bindingCount; if (o->nb) memcpy(o->b, ci->pBindings, sizeof *o->b * o->nb);
    for (uint32_t i = 0; i < ci->bindingCount; i++) for (uint32_t j = 0; j < i; j++) if (ci->pBindings[i].binding == ci->pBindings[j].binding) mk_err("set layout: binding %u twice", ci->pBindings[i].binding);
    pthread_mutex_unlock(&mk_mu);
    *out = (VkDescriptorSetLayout)(uintptr_t)h; return VK_SUCCESS;
}
void vkDestroyDescriptorSetLayout(VkDevice d, VkDescriptorSetLayout l, const VkAllocationCallbacks *a) { (void)a; (void)d; mk_del((uint64_t)(uintptr_t)l, MK_DSL); }
VkResult vkCreatePipelineLayout(VkDevice d, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineLayout *out) {
    (void)a; mk_dev_check(d, "vkCreatePipelineLayout");
    pthread_mutex_lock(&mk_mu);
    if (ci->setLayoutCount && !mk_find((uint64_t)(uintptr_t)ci->pSetLayouts[0], MK_DSL)) mk_err("pipeline layout of a dead set layout");
    mk_obj *o; const uint64_t h = mk_new(MK_PL, &o); o->nset = ci->setLayoutCount; o->set0 = ci->setLayoutCount ? ci->pSetLayouts[0] : VK_NULL_HANDLE;
    pthread_mutex_unlock(&mk_mu);
    *out = (VkPipelineLayout)(uintptr_t)h; return VK_SUCCESS;
}
void vkDestroyPipelineLayout(VkDevice d, VkPipelineLayout l, const VkAllocationCallbacks *a) { (void)a; (void)d; mk_del((uint64_t)(uintptr_t)l, MK_PL); }
VkResult vkCreateRenderPass(VkDevice d, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *a, VkRenderPass *out) {
    (void)a; mk_dev_check(d, "vkCreateRenderPass");
    if (ci->subpassCount != 1 || ci->attachmentCount > 9) mk_err("render pass: %u subpasses, %u attachments", ci->subpassCount, ci->attachmentCount);
    pthread_mutex_lock(&mk_mu); mk_obj *o; const uint64_t h = mk_new(MK_RP, &o); mk_rp *r = &o->rp;
    r->natt = ci->attachmentCount; if (r->natt) memcpy(r->att, ci->pAttachments, sizeof *r->att * r->natt);
    const VkSubpassDescription *s = ci->pSubpasses;
    r->ncolor = s->colorAttachmentCount; if (r->ncolor) memcpy(r->color, s->pColorAttachments, sizeof *r->color * r->ncolor);
    r->has_depth = s->pDepthStencilAttachment != NULL; if (r->has_depth) r->depth = *s->pDepthStencilAttachment;
    r->ndep = ci->dependencyCount > 4 ? 4 : ci->dependencyCount; if (r->ndep) memcpy(r->dep, ci->pDependencies, sizeof *r->dep * r->ndep);
    for (uint32_t i = 0; i < r->ncolor; i++) if (r->color[i].attachment != VK_ATTACHMENT_UNUSED && r->color[i].attachment >= r->natt) mk_err("render pass: colour reference out of range");
    pthread_mutex_unlock(&mk_mu);
    *out = (VkRenderPass)(uintptr_t)h; return VK_SUCCESS;
}
VkResult vkCreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *a, VkFramebuffer *out) {
    (void)a; mk_dev_check(d, "vkCreateFramebuffer");
    pthread_mutex_lock(&mk_mu);
    mk_obj *rp = mk_find((uint64_t)(uintptr_t)ci->renderPass, MK_RP);
    if (!rp) mk_err("framebuffer of a dead render pass");
    else if (rp->rp.natt != ci->attachmentCount) mk_err("framebuffer: %u views for a pass of %u attachments", ci->attachmentCount, rp->rp.natt);
    for (uint32_t i = 0; i < ci->attachmentCount; i++) if (!mk_find((uint64_t)(uintptr_t)ci->pAttachments[i], MK_VIEW)) mk_err("framebuffer: view %u is not a live view", i);
    mk_obj *o; const uint64_t h = mk_new(MK_FB, &o);
    o->fb_pass = ci->renderPass; o->fb_n = ci->attachmentCount > 9 ? 9 : ci->attachmentCount; o->fb_w = ci->width; o->fb_h = ci->height; o->fb_layers = ci->layers;
    if (o->fb_n) memcpy(o->fb_view, ci->pAttachments, sizeof *o->fb_view * o->fb_n);
    pthread_mutex_unlock(&mk_mu);
    *out = (VkFramebuffer)(uintptr_t)h; return VK_SUCCESS;
}
void vkDestroyFramebuffer(VkDevice d, VkFramebuffer f, const VkAllocationCallbacks *a) { (void)a; (void)d; mk_del((uint64_t)(uintptr_t)f, MK_FB); }
VkResult vkCreatePipelineCache(VkDevice d, const VkPipelineCacheCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineCache *out) {
    (void)a; (void)ci; mk_dev_check(d, "vkCreatePipelineCache");
    pthread_mutex_lock(&mk_mu); mk_obj *o; const uint64_t h = mk_new(MK_PCACHE, &o); pthread_mutex_unlock(&mk_mu);
    *out = (VkPipelineCache)(uintptr_t)h; return VK_SUCCESS;
}
static int mk_pipe_fail_calls;
VkResult vkCreateGraphicsPipelines(VkDevice d, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    (void)a; mk_dev_check(d, "vkCreateGraphicsPipelines");
    if (n != 1) mk_err("vkCreateGraphicsPipelines: %u at once", n);
    for (uint32_t i = 0; i < ci->stageCount; i++) if (ci->pStages[i].module == MK_BAD_MODULE) { mk_pipe_fail_calls++; *out = VK_NULL_HANDLE; return VK_ERROR_INITIALIZATION_FAILED; }
    pthread_mutex_lock(&mk_mu);
    if (!mk_find((uint64_t)(uintptr_t)ci->layout, MK_PL)) mk_err("graphics pipeline with a dead layout");
    if (!mk_find((uint64_t)(uintptr_t)ci->renderPass, MK_RP)) mk_err("graphics pipeline with a dead render pass");
    mk_obj *o; const uint64_t h = mk_new(MK_PIPE, &o); mk_gp *g = &o->gp;
    g->nst = ci->stageCount > 2 ? 2 : ci->stageCount;
    for (uint32_t i = 0; i < g->nst; i++) { g->stage[i] = ci->pStages[i].stage; g->module[i] = ci->pStages[i].module; strncpy(g->entry[i], ci->pStages[i].pName, 7); }
    const VkPipelineVertexInputStateCreateInfo *vi = ci->pVertexInputState;
    g->nbind = vi->vertexBindingDescriptionCount; if (g->nbind > 8) { mk_err("too many vertex bindings"); g->nbind = 8; }
    g->nattr = vi->vertexAttributeDescriptionCount; if (g->nattr > 32) { mk_err("too many vertex attributes"); g->nattr = 32; }
    if (g->nbind) memcpy(g->bind, vi->pVertexBindingDescriptions, sizeof *g->bind * g->nbind);
    if (g->nattr) memcpy(g->attr, vi->pVertexAttributeDescriptions, sizeof *g->attr * g->nattr);
    g->topo = ci->pInputAssemblyState->topology; g->restart = ci->pInputAssemblyState->primitiveRestartEnable;
    g->nvp = ci->pViewportState->viewportCount; g->nsc = ci->pViewportState->scissorCount;
    g->rs = *ci->pRasterizationState; g->samples = ci->pMultisampleState->rasterizationSamples;
    g->has_ds = ci->pDepthStencilState != NULL; if (g->has_ds) g->ds = *ci->pDepthStencilState;
    g->natt = ci->pColorBlendState->attachmentCount; if (g->natt) memcpy(g->att, ci->pColorBlendState->pAttachments, sizeof *g->att * g->natt);
    g->ndyn = ci->pDynamicState ? ci->pDynamicState->dynamicStateCount : 0; if (g->ndyn) memcpy(g->dyn, ci->pDynamicState->pDynamicStates, sizeof *g->dyn * g->ndyn);
    g->layout = ci->layout; g->pass = ci->renderPass; g->subpass = ci->subpass; g->cache = cache;
    pthread_mutex_unlock(&mk_mu);
    *out = (VkPipeline)(uintptr_t)h; return VK_SUCCESS;
}
VkResult vkCreateComputePipelines(VkDevice d, VkPipelineCache cache, uint32_t n, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    (void)a; (void)cache; mk_dev_check(d, "vkCreateComputePipelines");
    if (n != 1 || ci->stage.stage != VK_SHADER_STAGE_COMPUTE_BIT) mk_err("vkCreateComputePipelines: bad arguments");
    if (ci->stage.module == MK_BAD_MODULE) { *out = VK_NULL_HANDLE; return VK_ERROR_INITIALIZATION_FAILED; }
    pthread_mutex_lock(&mk_mu); mk_obj *o; const uint64_t h = mk_new(MK_PIPE, &o); o->compute = 1; o->cs_module = ci->stage.module; o->cs_layout = ci->layout; pthread_mutex_unlock(&mk_mu);
    *out = (VkPipeline)(uintptr_t)h; return VK_SUCCESS;
}
void vkDestroyPipeline(VkDevice d, VkPipeline p, const VkAllocationCallbacks *a) { (void)a; (void)d; mk_del((uint64_t)(uintptr_t)p, MK_PIPE); }

// ---------------------------------------------------------------- command buffers, the queue
VkResult vkCreateCommandPool(VkDevice d, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkCommandPool *out) {
    (void)a; mk_dev_check(d, "vkCreateCommandPool");
    if (mk_fail_cpool > 0) { mk_fail_cpool--; *out = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    if (ci->queueFamilyIndex != 2) mk_err("command pool of family %u", ci->queueFamilyIndex);
    pthread_mutex_lock(&mk_mu); mk_obj *o; const uint64_t h = mk_new(MK_POOL, &o); o->pool_family = ci->queueFamilyIndex; pthread_mutex_unlock(&mk_mu);
    *out = (VkCommandPool)(uintptr_t)h; return VK_SUCCESS;
}
void vkDestroyCommandPool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a) { (void)a; (void)d; mk_del((uint64_t)(uintptr_t)p, MK_POOL); }
VkResult vkAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *out) {
    mk_dev_check(d, "vkAllocateCommandBuffers");
    pthread_mutex_lock(&mk_mu);
    if (!mk_find((uint64_t)(uintptr_t)ai->commandPool, MK_POOL)) mk_err("command buffer of a dead pool");
    for (uint32_t i = 0; i < ai->commandBufferCount; i++) { mk_obj *o; out[i] = (VkCommandBuffer)(uintptr_t)mk_new(MK_CB, &o); }
    pthread_mutex_unlock(&mk_mu); return VK_SUCCESS;
}
static mk_obj *mk_cb(VkCommandBuffer cb, const char *fn, int want) {   // under mk_mu
    mk_obj *o = mk_find((uint64_t)(uintptr_t)cb, MK_CB);
    if (!o) mk_err("%s on %p: not a live command buffer", fn, (void *)cb);
    else if (want >= 0 && o->cb_state != want) mk_err("%s on a command buffer in state %d (want %d)", fn, o->cb_state, want);
    return o;
}
VkResult vkResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags f) {
    (void)f; pthread_mutex_lock(&mk_mu); mk_obj *o = mk_cb(cb, "vkResetCommandBuffer", -1);
    if (o && o->pend_seq) mk_err("vkResetCommandBuffer of a command buffer still in flight");
    if (o) o->cb_state = 0;
    pthread_mutex_unlock(&mk_mu); return VK_SUCCESS;
}
VkResult vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) {
    (void)bi; pthread_mutex_lock(&mk_mu);
    if (mk_fail_begin > 0) { mk_fail_begin--; pthread_mutex_unlock(&mk_mu); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    mk_obj *o = mk_cb(cb, "vkBeginCommandBuffer", 0);
    if (o && o->pend_seq) mk_err("vkBeginCommandBuffer of a command buffer still in flight");
    if (o) { o->cb_state = 1; o->ev_first = mk_log(cb, MK_E_BEGIN_CB)->seq; o->began = 1; }
    pthread_mutex_unlock(&mk_mu); return VK_SUCCESS;
}
VkResult vkEndCommandBuffer(VkCommandBuffer cb) {
    pthread_mutex_lock(&mk_mu);
    if (mk_fail_end > 0) { mk_fail_end--; pthread_mutex_unlock(&mk_mu); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    mk_obj *o = mk_cb(cb, "vkEndCommandBuffer", 1); if (o) o->cb_state = 2;
    mk_log(cb, MK_E_END_CB);
    pthread_mutex_unlock(&mk_mu); return VK_SUCCESS;
}
VkResult vkCreateFence(VkDevice d, const VkFenceCreateInfo *ci, const VkAllocationCallbacks *a, VkFence *out) {
    (void)a; mk_dev_check(d, "vkCreateFence");
    pthread_mutex_lock(&mk_mu); mk_obj *o; const uint64_t h = mk_new(MK_FENCE, &o); o->signaled = (ci->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0; pthread_mutex_unlock(&mk_mu);
    *out = (VkFence)(uintptr_t)h; return VK_SUCCESS;
}
VkResult vkResetFences(VkDevice d, uint32_t n, const VkFence *f) {
    (void)d; pthread_mutex_lock(&mk_mu);
    for (uint32_t i = 0; i < n; i++) { mk_obj *o = mk_find((uint64_t)(uintptr_t)f[i], MK_FENCE); if (!o) mk_err("reset of a dead fence"); else o->signaled = 0; }
    pthread_mutex_unlock(&mk_mu); return VK_SUCCESS;
}
// The GPU finishes every submission up to sequence s (queue order): their fences signal, their command buffers leave flight. Under mk_mu.
static void mk_gpu_done_upto(uint64_t s) {
    for (uint32_t i = 0; i < mk_nobj; i++) {
        mk_obj *o = mk_obj_tab[i];
        if ((o->kind == MK_FENCE || o->kind == MK_CB) && o->pend_seq && o->pend_seq <= s) { o->pend_seq = 0; if (o->kind == MK_FENCE) o->signaled = 1; }
    }
}
static uint64_t mk_fence_waits;   // waits that found the fence pending (mk_fence_hold)
VkResult vkWaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout) {
    (void)d; (void)all; (void)timeout; VkResult r = VK_SUCCESS; pthread_mutex_lock(&mk_mu);
    for (uint32_t i = 0; i < n; i++) {
        mk_obj *o = mk_find((uint64_t)(uintptr_t)f[i], MK_FENCE);
        if (o && !o->signaled && o->pend_seq) {   // in flight (mk_fence_hold): the GPU finishes it now, unless it is stuck
            mk_ev *e = mk_log(VK_NULL_HANDLE, MK_E_WAIT); e->a[0] = (uint64_t)(uintptr_t)f[i];
            if (mk_fence_stuck) { r = VK_TIMEOUT; continue; }
            mk_fence_waits++; mk_gpu_done_upto(o->pend_seq);
        }
        if (!o || !o->signaled) { mk_err("wait on a fence that is never signalled"); r = VK_TIMEOUT; }
    }
    pthread_mutex_unlock(&mk_mu); return r;
}
VkResult vkGetFenceStatus(VkDevice d, VkFence f) {
    (void)d; pthread_mutex_lock(&mk_mu); mk_obj *o = mk_find((uint64_t)(uintptr_t)f, MK_FENCE); VkResult r = VK_ERROR_DEVICE_LOST;
    if (!o) mk_err("vkGetFenceStatus of a dead fence"); else r = o->signaled ? VK_SUCCESS : VK_NOT_READY;
    pthread_mutex_unlock(&mk_mu); return r;
}
void vkDestroyFence(VkDevice d, VkFence f, const VkAllocationCallbacks *a) {
    (void)a; (void)d; pthread_mutex_lock(&mk_mu); mk_obj *o = mk_find((uint64_t)(uintptr_t)f, MK_FENCE);
    if (o && o->pend_seq) mk_err("vkDestroyFence of a fence still in flight");
    pthread_mutex_unlock(&mk_mu);
    mk_del((uint64_t)(uintptr_t)f, MK_FENCE);
}
static void mk_replay(mk_obj *cbo, VkCommandBuffer cb);
static VkCommandBuffer mk_last_submitted;
VkResult vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence fence) {
    pthread_mutex_lock(&mk_mu);
    if (mk_fail_submit > 0) { mk_fail_submit--; pthread_mutex_unlock(&mk_mu); return VK_ERROR_DEVICE_LOST; }
    if (q != MK_QUEUE) mk_err("submit to queue %p", (void *)q);
    if (mk_qlock_depth != 1) mk_err("vkQueueSubmit outside the queue lock (depth %d)", mk_qlock_depth);
    const uint64_t s = ++mk_sub_seq;
    for (uint32_t i = 0; i < n; i++) for (uint32_t j = 0; j < si[i].commandBufferCount; j++) {
        const VkCommandBuffer cb = si[i].pCommandBuffers[j];
        mk_obj *o = mk_cb(cb, "vkQueueSubmit", 2); mk_last_submitted = cb;
        mk_ev *e = mk_log(cb, MK_E_SUBMIT); e->a[0] = (uint64_t)(uintptr_t)fence; e->qlock = mk_qlock_depth; e->u[0] = (uint32_t)s;
        if (o) { mk_replay(o, cb); if (mk_fence_hold) o->pend_seq = s; }
    }
    if (fence) {
        mk_obj *o = mk_find((uint64_t)(uintptr_t)fence, MK_FENCE);
        if (!o || o->signaled || o->pend_seq) mk_err("submit with a dead, signalled or pending fence");
        else if (mk_fence_hold) o->pend_seq = s; else o->signaled = 1;
    }
    mk_submits++;
    pthread_mutex_unlock(&mk_mu); return VK_SUCCESS;
}

// recorded commands
typedef struct { VkImage img; VkImageLayout from, to; VkImageSubresourceRange r; } mk_bar_t;
typedef struct { VkCommandBuffer cb; VkBuffer buf; VkImage img; VkImageLayout layout; VkBufferImageCopy c; const uint8_t *src; } mk_copy_t;
typedef struct { VkImage src, dst; VkImageLayout sl, dl; uint32_t n; VkImageCopy c[16]; } mk_icopy_t;
enum { MK_LOG = 512 };
static mk_bar_t mk_bar[MK_LOG]; static uint32_t mk_nbar;
static mk_copy_t mk_copy[MK_LOG]; static uint32_t mk_ncopy;
static mk_icopy_t mk_icopy[64]; static uint32_t mk_nicopy;
static MK_U void mk_log_reset(void) { pthread_mutex_lock(&mk_mu); mk_nbar = mk_ncopy = mk_nicopy = 0; pthread_mutex_unlock(&mk_mu); }
void vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags dst, VkDependencyFlags df, uint32_t nm, const VkMemoryBarrier *m,
                          uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im) {
    (void)s; (void)dst; (void)df; (void)nm; (void)m; (void)nb; (void)b;
    pthread_mutex_lock(&mk_mu); mk_cb(cb, "vkCmdPipelineBarrier", 1);
    for (uint32_t i = 0; i < ni; i++) {
        mk_obj *o = mk_find((uint64_t)(uintptr_t)im[i].image, MK_IMAGE);
        if (!o) mk_err("barrier on a dead image");
        else if (im[i].subresourceRange.levelCount > o->ici.mipLevels || im[i].subresourceRange.layerCount > o->ici.arrayLayers) mk_err("barrier range outside the image");
        if (mk_nbar < MK_LOG) mk_bar[mk_nbar++] = (mk_bar_t){im[i].image, im[i].oldLayout, im[i].newLayout, im[i].subresourceRange};
        mk_ev *e = mk_log(cb, MK_E_BARRIER); e->a[0] = (uint64_t)(uintptr_t)im[i].image; e->l0 = im[i].oldLayout; e->l1 = im[i].newLayout; e->r = im[i].subresourceRange;
        e->u[0] = s; e->u[1] = dst; e->u[2] = im[i].srcAccessMask; e->u[3] = im[i].dstAccessMask;
    }
    if (!ni) mk_log(cb, MK_E_BARRIER)->u[0] = s;   // a memory / buffer barrier only (a[0] = 0)
    pthread_mutex_unlock(&mk_mu);
}
void vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer buf, VkImage img, VkImageLayout l, uint32_t n, const VkBufferImageCopy *c) {
    pthread_mutex_lock(&mk_mu); mk_cb(cb, "vkCmdCopyBufferToImage", 1);
    mk_obj *ob = mk_find((uint64_t)(uintptr_t)buf, MK_BUFFER), *oi = mk_find((uint64_t)(uintptr_t)img, MK_IMAGE);
    if (!ob || !oi) mk_err("vkCmdCopyBufferToImage: dead buffer or image");
    if (l != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) mk_err("vkCmdCopyBufferToImage: layout %d", (int)l);
    for (uint32_t i = 0; i < n; i++) {
        if (c[i].bufferOffset % 4) mk_err("copy: bufferOffset %llu not a multiple of 4", (unsigned long long)c[i].bufferOffset);
        if (oi && (c[i].imageSubresource.mipLevel >= oi->ici.mipLevels || c[i].imageSubresource.baseArrayLayer + c[i].imageSubresource.layerCount > oi->ici.arrayLayers)) mk_err("copy outside the image");
        if (mk_ncopy < MK_LOG) mk_copy[mk_ncopy++] = (mk_copy_t){cb, buf, img, l, c[i], ob && ob->cpu ? (const uint8_t *)ob->cpu + c[i].bufferOffset : NULL};
    }
    mk_ev *e = mk_log(cb, MK_E_COPY_BUF_IMG); e->a[0] = (uint64_t)(uintptr_t)img; e->a[1] = (uint64_t)(uintptr_t)buf; e->l1 = l; e->n = n;
    pthread_mutex_unlock(&mk_mu);
}
void vkCmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy *c) {
    pthread_mutex_lock(&mk_mu); mk_cb(cb, "vkCmdCopyImage", 1);
    if (!mk_find((uint64_t)(uintptr_t)src, MK_IMAGE) || !mk_find((uint64_t)(uintptr_t)dst, MK_IMAGE)) mk_err("vkCmdCopyImage: dead image");
    if (mk_nicopy < 64) { mk_icopy_t *e = &mk_icopy[mk_nicopy++]; e->src = src; e->dst = dst; e->sl = sl; e->dl = dl; e->n = n > 16 ? 16 : n; memcpy(e->c, c, sizeof *c * e->n); }
    mk_ev *e = mk_log(cb, MK_E_COPY_IMG); e->a[0] = (uint64_t)(uintptr_t)src; e->a[1] = (uint64_t)(uintptr_t)dst; e->l0 = sl; e->l1 = dl;
    e->n = n; memcpy(e->ic, c, sizeof *c * (n > 4 ? 4 : n));
    pthread_mutex_unlock(&mk_mu);
}
// A command buffer of the test's own, recording (for vkn_upload_run).
static MK_U VkCommandBuffer mk_test_cb(void) {
    pthread_mutex_lock(&mk_mu); mk_obj *o; const uint64_t h = mk_new(MK_CB, &o); o->cb_state = 1; pthread_mutex_unlock(&mk_mu);
    return (VkCommandBuffer)(uintptr_t)h;
}

// ---------------------------------------------------------------- stage 3: frame commands, descriptor sets, query pools
static mk_ev *mk_cmd(VkCommandBuffer cb, const char *fn, int op) {   // under mk_mu: a recording command buffer's new log entry
    mk_cb(cb, fn, 1); return mk_log(cb, op);
}
#define MK_H(x) ((uint64_t)(uintptr_t)(x))
void vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents c) {
    pthread_mutex_lock(&mk_mu); mk_ev *e = mk_cmd(cb, "vkCmdBeginRenderPass", MK_E_BEGIN_RP);
    e->a[0] = MK_H(bi->renderPass); e->a[1] = MK_H(bi->framebuffer); e->rect = bi->renderArea; e->u[0] = (uint32_t)c; e->u[1] = bi->clearValueCount;
    e->n = bi->clearValueCount > 9 ? 9 : bi->clearValueCount; if (e->n) memcpy(e->cv, bi->pClearValues, sizeof *e->cv * e->n);
    pthread_mutex_unlock(&mk_mu);
}
void vkCmdEndRenderPass(VkCommandBuffer cb) { pthread_mutex_lock(&mk_mu); mk_cmd(cb, "vkCmdEndRenderPass", MK_E_END_RP); pthread_mutex_unlock(&mk_mu); }
void vkCmdClearAttachments(VkCommandBuffer cb, uint32_t n, const VkClearAttachment *ca, uint32_t nr, const VkClearRect *cr) {
    pthread_mutex_lock(&mk_mu); mk_ev *e = mk_cmd(cb, "vkCmdClearAttachments", MK_E_CLEAR_ATT);
    e->n = n > 4 ? 4 : n; memcpy(e->ca, ca, sizeof *ca * e->n); e->u[0] = nr; memcpy(e->cr, cr, sizeof *cr * (nr > 2 ? 2 : nr));
    if (!n || !nr) mk_err("vkCmdClearAttachments with %u attachments / %u rects", n, nr);
    pthread_mutex_unlock(&mk_mu);
}
void vkCmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearColorValue *v, uint32_t nr, const VkImageSubresourceRange *r) {
    pthread_mutex_lock(&mk_mu); mk_ev *e = mk_cmd(cb, "vkCmdClearColorImage", MK_E_CLEAR_COLOR_IMG);
    e->a[0] = MK_H(img); e->l1 = l; e->cv[0].color = *v; e->r = r[0]; e->u[0] = nr; e->n = 1;
    pthread_mutex_unlock(&mk_mu);
}
void vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout l, const VkClearDepthStencilValue *v, uint32_t nr, const VkImageSubresourceRange *r) {
    pthread_mutex_lock(&mk_mu); mk_ev *e = mk_cmd(cb, "vkCmdClearDepthStencilImage", MK_E_CLEAR_DS_IMG);
    e->a[0] = MK_H(img); e->l1 = l; e->cv[0].depthStencil = *v; e->r = r[0]; e->u[0] = nr; e->n = 1;
    pthread_mutex_unlock(&mk_mu);
}
// (the warm-up's cache file, vkn_warm.c: no data in the mock)
VkResult vkGetPipelineCacheData(VkDevice d, VkPipelineCache c, size_t *n, void *data) { (void)d; (void)c; (void)data; if (n) *n = 0; return VK_SUCCESS; }
// (only the DM_VK_NATIVE_DUMP debug aid of vkn_pass.c calls it; the tests never set that switch)
void vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkBuffer dst, uint32_t n, const VkBufferImageCopy *r) { (void)cb; (void)src; (void)sl; (void)dst; (void)n; (void)r; }
void vkCmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageBlit *b, VkFilter f) {
    pthread_mutex_lock(&mk_mu); mk_ev *e = mk_cmd(cb, "vkCmdBlitImage", MK_E_BLIT_IMG);
    e->a[0] = MK_H(src); e->a[1] = MK_H(dst); e->l0 = sl; e->l1 = dl; e->n = n; memcpy(e->ib, b, sizeof *b * (n > 4 ? 4 : n)); e->filter = f;
    pthread_mutex_unlock(&mk_mu);
}
void vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    pthread_mutex_lock(&mk_mu); mk_ev *e = mk_cmd(cb, "vkCmdBindPipeline", MK_E_BIND_PIPE); e->u[0] = (uint32_t)bp; e->a[0] = MK_H(p); pthread_mutex_unlock(&mk_mu);
}
void vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *sets, uint32_t ndyn, const uint32_t *dyn) {
    pthread_mutex_lock(&mk_mu); mk_ev *e = mk_cmd(cb, "vkCmdBindDescriptorSets", MK_E_BIND_SETS);
    e->u[0] = (uint32_t)bp; e->a[0] = MK_H(l); e->u[1] = first; e->u[2] = n; e->a[1] = n ? MK_H(sets[0]) : 0;
    if (n != 1 || first) mk_err("vkCmdBindDescriptorSets: sets %u..%u (the backend binds set 0 only)", first, first + n);
    if (ndyn > 16) mk_err("vkCmdBindDescriptorSets: %u dynamic offsets", ndyn);
    e->n = ndyn > 16 ? 16 : ndyn; if (e->n) memcpy(e->dyn, dyn, sizeof *dyn * e->n);
    pthread_mutex_unlock(&mk_mu);
}
void vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *off) {
    pthread_mutex_lock(&mk_mu); mk_ev *e = mk_cmd(cb, "vkCmdBindVertexBuffers", MK_E_BIND_VB);
    e->u[0] = first; e->u[1] = n; if (n > 4) mk_err("vkCmdBindVertexBuffers: %u at once", n);
    for (uint32_t i = 0; i < n && i < 4; i++) { e->a[i] = MK_H(b[i]); e->off[i] = off[i]; }
    pthread_mutex_unlock(&mk_mu);
}
void vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkIndexType t) {
    pthread_mutex_lock(&mk_mu); mk_ev *e = mk_cmd(cb, "vkCmdBindIndexBuffer", MK_E_BIND_IB); e->a[0] = MK_H(b); e->off[0] = off; e->u[0] = (uint32_t)t; pthread_mutex_unlock(&mk_mu);
}
void vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v) {
    pthread_mutex_lock(&mk_mu); mk_ev *e = mk_cmd(cb, "vkCmdSetViewport", MK_E_VIEWPORT); e->u[0] = first; e->u[1] = n; e->vp = v[0]; pthread_mutex_unlock(&mk_mu);
}
void vkCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *r) {
    pthread_mutex_lock(&mk_mu); mk_ev *e = mk_cmd(cb, "vkCmdSetScissor", MK_E_SCISSOR); e->u[0] = first; e->u[1] = n; e->rect = r[0]; pthread_mutex_unlock(&mk_mu);
}
void vkCmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    pthread_mutex_lock(&mk_mu); mk_ev *e = mk_cmd(cb, "vkCmdDraw", MK_E_DRAW); e->u[0] = vc; e->u[1] = ic; e->u[2] = fv; e->u[3] = fi; pthread_mutex_unlock(&mk_mu);
}
void vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t ic, uint32_t inst, uint32_t fi, int32_t vo, uint32_t finst) {
    pthread_mutex_lock(&mk_mu); mk_ev *e = mk_cmd(cb, "vkCmdDrawIndexed", MK_E_DRAW_IDX);
    e->u[0] = ic; e->u[1] = inst; e->u[2] = fi; e->u[3] = (uint32_t)vo; e->u[4] = finst; pthread_mutex_unlock(&mk_mu);
}
void vkCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    pthread_mutex_lock(&mk_mu); mk_ev *e = mk_cmd(cb, "vkCmdDispatch", MK_E_DISPATCH); e->u[0] = x; e->u[1] = y; e->u[2] = z; pthread_mutex_unlock(&mk_mu);
}
VkResult vkCreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out) {
    (void)a; mk_dev_check(d, "vkCreateShaderModule");
    if (!ci->codeSize || ci->codeSize % 4 || !ci->pCode || ci->pCode[0] != 0x07230203u) mk_err("vkCreateShaderModule: not SPIR-V (%zu bytes)", ci->codeSize);
    pthread_mutex_lock(&mk_mu); mk_obj *o; const uint64_t h = mk_new(MK_SMOD, &o); o->size = ci->codeSize; pthread_mutex_unlock(&mk_mu);
    *out = (VkShaderModule)(uintptr_t)h; return VK_SUCCESS;
}
void vkDestroyShaderModule(VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a) { (void)a; (void)d; mk_del(MK_H(m), MK_SMOD); }
VkResult vkCreateQueryPool(VkDevice d, const VkQueryPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkQueryPool *out) {
    (void)a; mk_dev_check(d, "vkCreateQueryPool");
    if (ci->queryType != VK_QUERY_TYPE_TIMESTAMP || ci->queryCount > 64) mk_err("vkCreateQueryPool: type %d count %u", (int)ci->queryType, ci->queryCount);
    pthread_mutex_lock(&mk_mu); mk_obj *o; const uint64_t h = mk_new(MK_QPOOL, &o); o->size = ci->queryCount; pthread_mutex_unlock(&mk_mu);
    *out = (VkQueryPool)(uintptr_t)h; return VK_SUCCESS;
}
void vkDestroyQueryPool(VkDevice d, VkQueryPool p, const VkAllocationCallbacks *a) { (void)a; (void)d; mk_del(MK_H(p), MK_QPOOL); }
void vkCmdResetQueryPool(VkCommandBuffer cb, VkQueryPool p, uint32_t first, uint32_t n) {
    pthread_mutex_lock(&mk_mu); mk_ev *e = mk_cmd(cb, "vkCmdResetQueryPool", MK_E_RESET_QP); e->a[0] = MK_H(p); e->u[0] = first; e->u[1] = n;
    mk_obj *o = mk_find(MK_H(p), MK_QPOOL);
    if (!o || first + n > o->size) mk_err("vkCmdResetQueryPool: bad pool / range"); else for (uint32_t i = 0; i < n; i++) o->qv[first + i] = 0;
    pthread_mutex_unlock(&mk_mu);
}
void vkCmdWriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits st, VkQueryPool p, uint32_t q) {
    pthread_mutex_lock(&mk_mu); mk_ev *e = mk_cmd(cb, "vkCmdWriteTimestamp", MK_E_TIMESTAMP); e->a[0] = MK_H(p); e->u[0] = q; e->u[1] = (uint32_t)st;
    mk_obj *o = mk_find(MK_H(p), MK_QPOOL);
    if (!o || q >= o->size) mk_err("vkCmdWriteTimestamp: bad pool / query"); else { o->qv[q] = (mk_ts_clock += 1000); e->a[1] = o->qv[q]; }   // (written at record time: the mock GPU runs as recorded)
    pthread_mutex_unlock(&mk_mu);
}
VkResult vkGetQueryPoolResults(VkDevice d, VkQueryPool p, uint32_t first, uint32_t n, size_t sz, void *data, VkDeviceSize stride, VkQueryResultFlags fl) {
    (void)d; pthread_mutex_lock(&mk_mu); mk_obj *o = mk_find(MK_H(p), MK_QPOOL); VkResult r = VK_SUCCESS;
    if (!o || first + n > o->size || !(fl & VK_QUERY_RESULT_64_BIT) || stride < 8 || sz < stride * (n - 1) + 8) { mk_err("vkGetQueryPoolResults: bad arguments"); r = VK_ERROR_DEVICE_LOST; }
    else for (uint32_t i = 0; i < n; i++) { const uint64_t v = o->qv[first + i]; memcpy((uint8_t *)data + stride * i, &v, 8); if (!v) r = VK_NOT_READY; }
    pthread_mutex_unlock(&mk_mu); return r;
}
VkResult vkCreateDescriptorPool(VkDevice d, const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorPool *out) {
    (void)a; mk_dev_check(d, "vkCreateDescriptorPool");
    if (!ci->maxSets || !ci->poolSizeCount) mk_err("vkCreateDescriptorPool: %u sets, %u sizes", ci->maxSets, ci->poolSizeCount);
    pthread_mutex_lock(&mk_mu); mk_obj *o; const uint64_t h = mk_new(MK_DPOOL, &o); o->max_sets = ci->maxSets; pthread_mutex_unlock(&mk_mu);
    *out = (VkDescriptorPool)(uintptr_t)h; return VK_SUCCESS;
}
void vkDestroyDescriptorPool(VkDevice d, VkDescriptorPool p, const VkAllocationCallbacks *a) { (void)a; mk_dev_check(d, "vkDestroyDescriptorPool"); mk_del(MK_H(p), MK_DPOOL); }
VkResult vkAllocateDescriptorSets(VkDevice d, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out) {
    mk_dev_check(d, "vkAllocateDescriptorSets"); pthread_mutex_lock(&mk_mu); VkResult r = VK_SUCCESS;
    mk_obj *p = mk_find(MK_H(ai->descriptorPool), MK_DPOOL);
    if (!p) { mk_err("vkAllocateDescriptorSets from a dead pool"); r = VK_ERROR_OUT_OF_POOL_MEMORY; }
    else if (mk_fail_dset > 0) { mk_fail_dset--; r = VK_ERROR_OUT_OF_POOL_MEMORY; }
    else if (p->nsets + ai->descriptorSetCount > p->max_sets) r = VK_ERROR_OUT_OF_POOL_MEMORY;
    else for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
        if (!mk_find(MK_H(ai->pSetLayouts[i]), MK_DSL)) mk_err("vkAllocateDescriptorSets with a dead set layout");
        mk_obj *o; out[i] = (VkDescriptorSet)(uintptr_t)mk_new(MK_DSET, &o); o->set_layout = ai->pSetLayouts[i]; o->dpool = MK_H(ai->descriptorPool);
        o->dw = (mk_dw *)calloc(MK_DW_MAX, sizeof *o->dw); p->nsets++;
    }
    pthread_mutex_unlock(&mk_mu); return r;
}
static int mk_is_img_desc(VkDescriptorType t) { return t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; }
static int mk_is_buf_desc(VkDescriptorType t) { return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; }
static int mk_is_dyn_desc(VkDescriptorType t) { return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; }
static uint64_t mk_desc_writes;
void vkUpdateDescriptorSets(VkDevice d, uint32_t n, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c) {
    (void)c; mk_dev_check(d, "vkUpdateDescriptorSets"); pthread_mutex_lock(&mk_mu);
    if (nc) mk_err("vkUpdateDescriptorSets: copies are not modelled");
    for (uint32_t i = 0; i < n; i++) {
        mk_obj *s = mk_find(MK_H(w[i].dstSet), MK_DSET);
        if (!s) { mk_err("descriptor write to a dead set"); continue; }
        mk_obj *l = mk_any(MK_H(s->set_layout)); const VkDescriptorSetLayoutBinding *lb = NULL;
        for (uint32_t k = 0; l && k < l->nb; k++) if (l->b[k].binding == w[i].dstBinding) lb = &l->b[k];
        if (!lb) { mk_err("descriptor write to binding %u the set layout lacks", w[i].dstBinding); continue; }
        if (lb->descriptorType != w[i].descriptorType) mk_err("descriptor write of type %d to binding %u of type %d", (int)w[i].descriptorType, w[i].dstBinding, (int)lb->descriptorType);
        if (w[i].dstArrayElement + w[i].descriptorCount > lb->descriptorCount) mk_err("descriptor write beyond binding %u's count", w[i].dstBinding);
        for (uint32_t k = 0; k < w[i].descriptorCount; k++) {
            mk_dw x; memset(&x, 0, sizeof x); x.binding = w[i].dstBinding; x.elem = w[i].dstArrayElement + k; x.type = w[i].descriptorType;
            if (mk_is_img_desc(x.type)) {
                const VkDescriptorImageInfo *ii = &w[i].pImageInfo[k]; x.view = ii->imageView; x.smp = ii->sampler; x.layout = ii->imageLayout;
                if (!mk_find(MK_H(x.view), MK_VIEW)) mk_err("descriptor write (binding %u): view %#llx is not live", x.binding, (unsigned long long)MK_H(x.view));
                if (x.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && !mk_find(MK_H(x.smp), MK_SAMPLER)) mk_err("descriptor write (binding %u): sampler is not live", x.binding);
            } else if (mk_is_buf_desc(x.type)) {
                const VkDescriptorBufferInfo *bi = &w[i].pBufferInfo[k]; x.buf = bi->buffer; x.off = bi->offset; x.range = bi->range;
                mk_obj *b = mk_find(MK_H(x.buf), MK_BUFFER);
                if (!b) mk_err("descriptor write (binding %u): buffer is not live", x.binding);
                else if (!x.range || (x.range != VK_WHOLE_SIZE && x.off + x.range > b->size)) mk_err("descriptor write (binding %u): range %llu at %llu of a %llu-byte buffer", x.binding, (unsigned long long)x.range, (unsigned long long)x.off, (unsigned long long)b->size);
                if (x.off % 64) mk_err("descriptor write (binding %u): offset %llu not aligned to 64", x.binding, (unsigned long long)x.off);
                if ((x.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || x.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) && x.range > 65536) mk_err("uniform range %llu > 64 KiB", (unsigned long long)x.range);
            }
            uint32_t j = 0; while (j < s->ndw && (s->dw[j].binding != x.binding || s->dw[j].elem != x.elem)) j++;
            if (j >= MK_DW_MAX) { mk_err("too many descriptors in one set"); break; }
            s->dw[j] = x; if (j == s->ndw) s->ndw++;
            mk_desc_writes++;
        }
    }
    pthread_mutex_unlock(&mk_mu);
}
static MK_U const mk_dw *mk_set_dw(VkDescriptorSet set, uint32_t binding, uint32_t elem) {   // the test reads a set's descriptor
    mk_obj *s = mk_obj_of(MK_H(set)); if (!s || s->kind != MK_DSET) return NULL;
    for (uint32_t j = 0; j < s->ndw; j++) if (s->dw[j].binding == binding && s->dw[j].elem == elem) return &s->dw[j];
    return NULL;
}

// ---- objects in flight
static int mk_inflight_uses(uint64_t h) {   // under mk_mu
    for (uint32_t i = 0; i < mk_nobj; i++) {
        mk_obj *o = mk_obj_tab[i];
        if (o->kind == MK_CB && o->pend_seq) for (uint32_t k = 0; k < o->nuses; k++) if (o->uses[k] == h) return 1;
    }
    return 0;
}
static void mk_use(mk_obj *cbo, uint64_t h) {
    if (!h) return;
    for (uint32_t k = 0; k < cbo->nuses; k++) if (cbo->uses[k] == h) return;
    if (cbo->nuses == cbo->capuses) { cbo->capuses = cbo->capuses ? 2 * cbo->capuses : 64; cbo->uses = (uint64_t *)realloc(cbo->uses, sizeof *cbo->uses * cbo->capuses); }
    cbo->uses[cbo->nuses++] = h;
}
static MK_U void mk_gpu_finish_all(void) { pthread_mutex_lock(&mk_mu); mk_gpu_done_upto(~0ull); pthread_mutex_unlock(&mk_mu); }

// ---- the validator: replays one command buffer's recording at its submit (GPU order), under mk_mu
static VkImageAspectFlags mk_fmt_aspects(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
static int mk_rp_compat(const mk_rp *a, const mk_rp *b) {
    if (a->natt != b->natt || a->ncolor != b->ncolor || a->has_depth != b->has_depth) return 0;
    for (uint32_t i = 0; i < a->natt; i++) if (a->att[i].format != b->att[i].format || a->att[i].samples != b->att[i].samples) return 0;
    for (uint32_t i = 0; i < a->ncolor; i++) if (a->color[i].attachment != b->color[i].attachment) return 0;
    return !a->has_depth || a->depth.attachment == b->depth.attachment;
}
static uint32_t mk_mipdim(uint32_t s, uint32_t l) { s >>= l; return s ? s : 1; }
static uint64_t mk_view_image(uint64_t v) { mk_obj *o = mk_any(v); return o && o->kind == MK_VIEW ? MK_H(o->vci.image) : 0; }

typedef struct mk_vstate {
    int in; mk_obj *rp, *fb; VkRect2D area; uint64_t att_img[9];
    uint64_t gpipe, cpipe, gset, cset, glayout, clayout; uint32_t gndyn, cndyn, gdyn[16], cdyn[16];
    uint64_t vb[8]; VkDeviceSize vboff[8]; uint64_t ib; VkDeviceSize iboff; uint32_t itype;
    int vp, sc; VkRect2D scr; VkViewport vpv;
} mk_vstate;
static VkCommandBuffer mk_v_cb; static const mk_ev *mk_v_ev;
#define MK_V(...) do { if (mk_validate_on) { mk_val_errors++; printf("VALIDATION (cb %#llx, #%llu %s): ", (unsigned long long)MK_H(mk_v_cb), (unsigned long long)mk_v_ev->seq, mk_ev_name[mk_v_ev->op]); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static mk_obj *mk_v_img(mk_obj *cbo, uint64_t img) {
    mk_obj *o = mk_find(img, MK_IMAGE);
    if (!o) MK_V("image %#llx is not live", (unsigned long long)img); else mk_use(cbo, img);
    return o;
}
static void mk_v_whole(const mk_obj *im, const VkImageSubresourceRange *r) {
    const uint32_t lc = r->levelCount == VK_REMAINING_MIP_LEVELS ? im->ici.mipLevels - r->baseMipLevel : r->levelCount;
    const uint32_t ac = r->layerCount == VK_REMAINING_ARRAY_LAYERS ? im->ici.arrayLayers - r->baseArrayLayer : r->layerCount;
    if (r->baseMipLevel || r->baseArrayLayer || lc != im->ici.mipLevels || ac != im->ici.arrayLayers) MK_V("barrier of part of an image (levels %u+%u of %u, layers %u+%u of %u): layouts are tracked per image", r->baseMipLevel, lc, im->ici.mipLevels, r->baseArrayLayer, ac, im->ici.arrayLayers);
    if (r->aspectMask != mk_fmt_aspects(im->ici.format)) MK_V("barrier aspect %#x, the format %d has %#x", r->aspectMask, (int)im->ici.format, mk_fmt_aspects(im->ici.format));
}
// A bound set at a draw / dispatch: written fully, image layouts as the descriptors say, no attachment of the pass sampled, dynamic ranges.
static void mk_v_set(mk_obj *cbo, const mk_vstate *st, uint64_t set, uint64_t pl_draw, uint64_t pl_bound, const uint32_t *dyn, uint32_t ndyn) {
    mk_obj *pl = mk_any(pl_draw); mk_obj *sl = pl && pl->nset ? mk_any(MK_H(pl->set0)) : NULL;
    if (!sl || !sl->nb) return;   // nothing statically used
    if (!set) { MK_V("no descriptor set bound for a pipeline layout with %u bindings", sl->nb); return; }
    mk_obj *pb = mk_any(pl_bound);
    if (!pb || pb->set0 != pl->set0) MK_V("set bound with pipeline layout %#llx, the pipeline's layout %#llx has another set layout", (unsigned long long)pl_bound, (unsigned long long)pl_draw);
    mk_obj *s = mk_find(set, MK_DSET);
    if (!s) { MK_V("descriptor set %#llx is not live (pool destroyed?)", (unsigned long long)set); return; }
    mk_use(cbo, set); mk_use(cbo, s->dpool);
    if (MK_H(s->set_layout) != MK_H(pl->set0)) MK_V("set of layout %#llx used with a pipeline of set layout %#llx", (unsigned long long)MK_H(s->set_layout), (unsigned long long)MK_H(pl->set0));
    // bindings in binding order (dynamic offsets follow it)
    uint32_t order[64], n = sl->nb < 64 ? sl->nb : 64, kd = 0;
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    for (uint32_t i = 1; i < n; i++) for (uint32_t j = i; j > 0 && sl->b[order[j]].binding < sl->b[order[j - 1]].binding; j--) { const uint32_t t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    for (uint32_t i = 0; i < n; i++) {
        const VkDescriptorSetLayoutBinding *lb = &sl->b[order[i]];
        for (uint32_t k = 0; k < lb->descriptorCount; k++) {
            const mk_dw *w = NULL;
            for (uint32_t j = 0; j < s->ndw; j++) if (s->dw[j].binding == lb->binding && s->dw[j].elem == k) w = &s->dw[j];
            if (!w) { MK_V("binding %u element %u of the set was never written", lb->binding, k); if (mk_is_dyn_desc(lb->descriptorType)) kd++; continue; }
            if (mk_is_img_desc(w->type)) {
                if (!mk_find(MK_H(w->view), MK_VIEW)) { MK_V("binding %u: view is dead", lb->binding); continue; }
                mk_use(cbo, MK_H(w->view)); mk_use(cbo, MK_H(w->smp));
                const uint64_t img = mk_view_image(MK_H(w->view)); mk_obj *im = mk_v_img(cbo, img); if (!im) continue;
                const VkImageLayout want = w->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                if (w->layout != want && !(w->layout == VK_IMAGE_LAYOUT_GENERAL)) MK_V("binding %u: descriptor layout %d", lb->binding, (int)w->layout);
                if (im->gpu_layout != w->layout) MK_V("binding %u: image %#llx is in layout %d, the descriptor says %d", lb->binding, (unsigned long long)img, (int)im->gpu_layout, (int)w->layout);
                if (st->in) for (int a = 0; a < 9; a++) if (st->att_img[a] == img) MK_V("binding %u: image %#llx is sampled while it is an attachment of the render pass (feedback loop)", lb->binding, (unsigned long long)img);
            } else if (mk_is_buf_desc(w->type)) {
                mk_obj *b = mk_find(MK_H(w->buf), MK_BUFFER); if (!b) { MK_V("binding %u: buffer is dead", lb->binding); continue; }
                mk_use(cbo, MK_H(w->buf));
                uint64_t d = 0;
                if (mk_is_dyn_desc(w->type)) {
                    if (kd < ndyn) d = dyn[kd];
                    kd++;
                    if (d % 64) MK_V("binding %u: dynamic offset %llu not aligned", lb->binding, (unsigned long long)d);
                }
                if (w->off + d + w->range > b->size) MK_V("binding %u: offset %llu + dynamic %llu + range %llu beyond the %llu-byte buffer", lb->binding, (unsigned long long)w->off, (unsigned long long)d, (unsigned long long)w->range, (unsigned long long)b->size);
            }
        }
    }
    if (kd != ndyn) MK_V("%u dynamic offsets bound, the set layout has %u dynamic descriptors", ndyn, kd);
}

static void mk_replay(mk_obj *cbo, VkCommandBuffer cb) {
    cbo->nuses = 0; mk_v_cb = cb;
    static const mk_ev nil = {0}; mk_v_ev = &nil;
    if (!cbo->began || cbo->ev_first < mk_ev_base) { MK_V("the log of this recording was reset before its submit"); return; }
    mk_vstate st; memset(&st, 0, sizeof st);
    for (uint64_t seq = cbo->ev_first + 1; seq < mk_seq; seq++) {
        const mk_ev *e = mk_ev_at(seq);
        if (e->cb != cb) continue;
        mk_v_ev = e;
        if (e->op == MK_E_END_CB) { if (st.in) MK_V("command buffer ended inside a render pass"); break; }
        if (e->op == MK_E_SUBMIT || e->op == MK_E_BEGIN_CB) { MK_V("recording without its end"); break; }
        const int outside = e->op == MK_E_BARRIER || e->op == MK_E_COPY_BUF_IMG || e->op == MK_E_COPY_IMG || e->op == MK_E_BLIT_IMG || e->op == MK_E_CLEAR_COLOR_IMG ||
                            e->op == MK_E_CLEAR_DS_IMG || e->op == MK_E_DISPATCH || e->op == MK_E_RESET_QP || e->op == MK_E_BEGIN_RP;
        if (outside && st.in) MK_V("not allowed inside a render pass");
        const int inside = e->op == MK_E_END_RP || e->op == MK_E_CLEAR_ATT || e->op == MK_E_DRAW || e->op == MK_E_DRAW_IDX;
        if (inside && !st.in) MK_V("allowed only inside a render pass");
        switch (e->op) {
        case MK_E_BARRIER: {
            if (!e->a[0]) break;
            mk_obj *im = mk_v_img(cbo, e->a[0]); if (!im) break;
            mk_v_whole(im, &e->r);
            if (e->l0 != VK_IMAGE_LAYOUT_UNDEFINED && e->l0 != im->gpu_layout) MK_V("old layout %d, the image %#llx is in %d", (int)e->l0, (unsigned long long)e->a[0], (int)im->gpu_layout);
            if (e->l1 == VK_IMAGE_LAYOUT_UNDEFINED || e->l1 == VK_IMAGE_LAYOUT_PREINITIALIZED) MK_V("new layout %d", (int)e->l1);
            im->gpu_layout = e->l1;
            break;
        }
        case MK_E_COPY_BUF_IMG: {
            mk_obj *im = mk_v_img(cbo, e->a[0]); mk_use(cbo, e->a[1]);
            if (im && (im->gpu_layout != e->l1 || (e->l1 != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && e->l1 != VK_IMAGE_LAYOUT_GENERAL))) MK_V("destination layout %d, the image is in %d", (int)e->l1, (int)im->gpu_layout);
            break;
        }
        case MK_E_COPY_IMG: case MK_E_BLIT_IMG: {
            mk_obj *s = mk_v_img(cbo, e->a[0]), *d = mk_v_img(cbo, e->a[1]); if (!s || !d) break;
            if (s->gpu_layout != e->l0 || (e->l0 != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && e->l0 != VK_IMAGE_LAYOUT_GENERAL)) MK_V("source layout %d, the image is in %d", (int)e->l0, (int)s->gpu_layout);
            if (d->gpu_layout != e->l1 || (e->l1 != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && e->l1 != VK_IMAGE_LAYOUT_GENERAL)) MK_V("destination layout %d, the image is in %d", (int)e->l1, (int)d->gpu_layout);
            if (s == d) MK_V("copy of an image onto itself");
            for (uint32_t i = 0; i < e->n && i < 4; i++) {
                const VkImageSubresourceLayers *ss = e->op == MK_E_COPY_IMG ? &e->ic[i].srcSubresource : &e->ib[i].srcSubresource, *ds = e->op == MK_E_COPY_IMG ? &e->ic[i].dstSubresource : &e->ib[i].dstSubresource;
                if (ss->mipLevel >= s->ici.mipLevels || ss->baseArrayLayer + ss->layerCount > s->ici.arrayLayers || ds->mipLevel >= d->ici.mipLevels || ds->baseArrayLayer + ds->layerCount > d->ici.arrayLayers)
                    MK_V("region %u outside an image (src level %u layers %u+%u, dst level %u layers %u+%u)", i, ss->mipLevel, ss->baseArrayLayer, ss->layerCount, ds->mipLevel, ds->baseArrayLayer, ds->layerCount);
                if (ss->aspectMask != mk_fmt_aspects(s->ici.format) || ds->aspectMask != mk_fmt_aspects(d->ici.format)) MK_V("region %u aspects %#x / %#x", i, ss->aspectMask, ds->aspectMask);
                if (e->op == MK_E_COPY_IMG) {
                    const VkImageCopy *c = &e->ic[i];
                    if (c->srcOffset.x + c->extent.width > mk_mipdim(s->ici.extent.width, ss->mipLevel) || c->srcOffset.y + c->extent.height > mk_mipdim(s->ici.extent.height, ss->mipLevel) ||
                        c->dstOffset.x + c->extent.width > mk_mipdim(d->ici.extent.width, ds->mipLevel) || c->dstOffset.y + c->extent.height > mk_mipdim(d->ici.extent.height, ds->mipLevel) ||
                        c->srcOffset.z + c->extent.depth > mk_mipdim(s->ici.extent.depth, ss->mipLevel) || c->dstOffset.z + c->extent.depth > mk_mipdim(d->ici.extent.depth, ds->mipLevel))
                        MK_V("copy region %u (%ux%ux%u) outside a level", i, c->extent.width, c->extent.height, c->extent.depth);
                    if (ss->layerCount != ds->layerCount) MK_V("copy region %u: %u source layers, %u destination layers", i, ss->layerCount, ds->layerCount);
                } else {
                    const VkImageBlit *b = &e->ib[i];
                    if ((uint32_t)b->srcOffsets[1].x > mk_mipdim(s->ici.extent.width, ss->mipLevel) || (uint32_t)b->srcOffsets[1].y > mk_mipdim(s->ici.extent.height, ss->mipLevel) ||
                        (uint32_t)b->dstOffsets[1].x > mk_mipdim(d->ici.extent.width, ds->mipLevel) || (uint32_t)b->dstOffsets[1].y > mk_mipdim(d->ici.extent.height, ds->mipLevel))
                        MK_V("blit region %u outside a level", i);
                    if (mk_fmt_aspects(s->ici.format) != VK_IMAGE_ASPECT_COLOR_BIT && e->filter != VK_FILTER_NEAREST) MK_V("depth blit with a linear filter");
                }
            }
            break;
        }
        case MK_E_CLEAR_COLOR_IMG: case MK_E_CLEAR_DS_IMG: {
            mk_obj *im = mk_v_img(cbo, e->a[0]); if (!im) break;
            if (im->gpu_layout != e->l1 || (e->l1 != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && e->l1 != VK_IMAGE_LAYOUT_GENERAL)) MK_V("layout %d, the image is in %d", (int)e->l1, (int)im->gpu_layout);
            const VkImageAspectFlags fa = mk_fmt_aspects(im->ici.format);
            if (e->op == MK_E_CLEAR_COLOR_IMG ? fa != VK_IMAGE_ASPECT_COLOR_BIT || e->r.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT : (fa & VK_IMAGE_ASPECT_COLOR_BIT) || !e->r.aspectMask || (e->r.aspectMask & ~fa))
                MK_V("aspect %#x of a format with %#x", e->r.aspectMask, fa);
            if (e->r.baseMipLevel + e->r.levelCount > im->ici.mipLevels || e->r.baseArrayLayer + e->r.layerCount > im->ici.arrayLayers) MK_V("range outside the image");
            break;
        }
        case MK_E_BEGIN_RP: {
            mk_obj *rp = mk_find(e->a[0], MK_RP), *fb = mk_find(e->a[1], MK_FB);
            if (!rp || !fb) { MK_V("dead render pass / framebuffer"); break; }
            mk_use(cbo, e->a[0]); mk_use(cbo, e->a[1]);
            mk_obj *fbrp = mk_any(MK_H(fb->fb_pass));
            if (!fbrp || !mk_rp_compat(&fbrp->rp, &rp->rp)) MK_V("framebuffer of an incompatible render pass");
            if (fb->fb_n != rp->rp.natt) MK_V("framebuffer with %u views, pass with %u attachments", fb->fb_n, rp->rp.natt);
            if (e->rect.offset.x < 0 || e->rect.offset.y < 0 || e->rect.offset.x + e->rect.extent.width > fb->fb_w || e->rect.offset.y + e->rect.extent.height > fb->fb_h || !e->rect.extent.width || !e->rect.extent.height)
                MK_V("render area %d,%d %ux%u outside the %ux%u framebuffer", e->rect.offset.x, e->rect.offset.y, e->rect.extent.width, e->rect.extent.height, fb->fb_w, fb->fb_h);
            st.in = 1; st.rp = rp; st.fb = fb; st.area = e->rect; memset(st.att_img, 0, sizeof st.att_img);
            for (uint32_t i = 0; i < rp->rp.natt && i < fb->fb_n; i++) {
                const VkAttachmentDescription *ad = &rp->rp.att[i];
                mk_obj *v = mk_find(MK_H(fb->fb_view[i]), MK_VIEW); if (!v) { MK_V("attachment %u: dead view", i); continue; }
                mk_use(cbo, MK_H(fb->fb_view[i]));
                mk_obj *im = mk_v_img(cbo, MK_H(v->vci.image)); if (!im) continue;
                st.att_img[i] = MK_H(v->vci.image);
                if (v->vci.format != ad->format) MK_V("attachment %u: view format %d, pass format %d", i, (int)v->vci.format, (int)ad->format);
                if (v->vci.subresourceRange.levelCount != 1 || v->vci.subresourceRange.layerCount != 1) MK_V("attachment %u: view of %u levels / %u layers", i, v->vci.subresourceRange.levelCount, v->vci.subresourceRange.layerCount);
                const uint32_t lv = v->vci.subresourceRange.baseMipLevel;
                if (mk_mipdim(im->ici.extent.width, lv) < fb->fb_w || mk_mipdim(im->ici.extent.height, lv) < fb->fb_h) MK_V("attachment %u: level %u smaller than the framebuffer", i, lv);
                if (ad->initialLayout != VK_IMAGE_LAYOUT_UNDEFINED && ad->initialLayout != im->gpu_layout) MK_V("attachment %u: initial layout %d, the image %#llx is in %d", i, (int)ad->initialLayout, (unsigned long long)MK_H(v->vci.image), (int)im->gpu_layout);
                if ((ad->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR || ad->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) && e->u[1] <= i) MK_V("attachment %u is cleared, only %u clear values", i, e->u[1]);
                VkImageLayout in_layout = ad->finalLayout;
                for (uint32_t c = 0; c < rp->rp.ncolor; c++) if (rp->rp.color[c].attachment == i) in_layout = rp->rp.color[c].layout;
                if (rp->rp.has_depth && rp->rp.depth.attachment == i) in_layout = rp->rp.depth.layout;
                im->gpu_layout = in_layout;
            }
            break;
        }
        case MK_E_END_RP:
            if (st.rp && st.fb) for (uint32_t i = 0; i < st.rp->rp.natt && i < 9; i++) { mk_obj *im = st.att_img[i] ? mk_find(st.att_img[i], MK_IMAGE) : NULL; if (im) im->gpu_layout = st.rp->rp.att[i].finalLayout; }
            st.in = 0; st.rp = st.fb = NULL; memset(st.att_img, 0, sizeof st.att_img);
            break;
        case MK_E_CLEAR_ATT:
            if (!st.rp) break;
            for (uint32_t i = 0; i < e->n; i++) {
                const VkClearAttachment *ca = &e->ca[i];
                if (ca->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
                    if (ca->aspectMask != VK_IMAGE_ASPECT_COLOR_BIT || ca->colorAttachment >= st.rp->rp.ncolor || st.rp->rp.color[ca->colorAttachment].attachment == VK_ATTACHMENT_UNUSED) MK_V("colour attachment %u is not in the subpass", ca->colorAttachment);
                } else if (!st.rp->rp.has_depth || (ca->aspectMask & ~mk_fmt_aspects(st.rp->rp.att[st.rp->rp.depth.attachment].format))) MK_V("depth / stencil clear (aspect %#x) without such an attachment", ca->aspectMask);
            }
            for (uint32_t i = 0; i < e->u[0] && i < 2; i++) {
                const VkClearRect *r = &e->cr[i];
                if (r->rect.offset.x < st.area.offset.x || r->rect.offset.y < st.area.offset.y || r->rect.offset.x + r->rect.extent.width > st.area.offset.x + st.area.extent.width ||
                    r->rect.offset.y + r->rect.extent.height > st.area.offset.y + st.area.extent.height || !r->rect.extent.width || !r->rect.extent.height) MK_V("clear rect outside the render area");
                if (r->baseArrayLayer != 0 || r->layerCount != 1) MK_V("clear rect layers %u+%u of a 1-layer framebuffer", r->baseArrayLayer, r->layerCount);
            }
            break;
        case MK_E_BIND_PIPE: {
            mk_obj *p = mk_find(e->a[0], MK_PIPE); if (!p) { MK_V("dead pipeline"); break; }
            mk_use(cbo, e->a[0]);
            if ((e->u[0] == VK_PIPELINE_BIND_POINT_COMPUTE) != (p->compute != 0)) MK_V("pipeline bound at the wrong bind point");
            if (e->u[0] == VK_PIPELINE_BIND_POINT_COMPUTE) st.cpipe = e->a[0]; else st.gpipe = e->a[0];
            break;
        }
        case MK_E_BIND_SETS: {
            mk_obj *s = mk_find(e->a[1], MK_DSET), *pl = mk_find(e->a[0], MK_PL);
            if (!s || !pl) { MK_V("dead set / pipeline layout"); break; }
            if (MK_H(pl->set0) != MK_H(s->set_layout)) MK_V("set of layout %#llx bound with a pipeline layout of set layout %#llx", (unsigned long long)MK_H(s->set_layout), (unsigned long long)MK_H(pl->set0));
            if (e->u[0] == VK_PIPELINE_BIND_POINT_COMPUTE) { st.cset = e->a[1]; st.clayout = e->a[0]; st.cndyn = e->n; memcpy(st.cdyn, e->dyn, sizeof st.cdyn); }
            else { st.gset = e->a[1]; st.glayout = e->a[0]; st.gndyn = e->n; memcpy(st.gdyn, e->dyn, sizeof st.gdyn); }
            break;
        }
        case MK_E_BIND_VB:
            for (uint32_t i = 0; i < e->u[1] && i < 4; i++) if (e->u[0] + i < 8) { st.vb[e->u[0] + i] = e->a[i]; st.vboff[e->u[0] + i] = e->off[i]; mk_use(cbo, e->a[i]); if (!mk_find(e->a[i], MK_BUFFER)) MK_V("dead vertex buffer"); }
            break;
        case MK_E_BIND_IB: st.ib = e->a[0]; st.iboff = e->off[0]; st.itype = e->u[0]; mk_use(cbo, e->a[0]); if (!mk_find(e->a[0], MK_BUFFER)) MK_V("dead index buffer"); break;
        case MK_E_VIEWPORT: if (e->u[0] || e->u[1] != 1 || !(e->vp.width > 0) || e->vp.height == 0) MK_V("viewport %g x %g", e->vp.width, e->vp.height); st.vp = 1; st.vpv = e->vp; break;
        case MK_E_SCISSOR: if (e->u[0] || e->u[1] != 1 || e->rect.offset.x < 0 || e->rect.offset.y < 0) MK_V("scissor %d,%d", e->rect.offset.x, e->rect.offset.y); st.sc = 1; st.scr = e->rect; break;
        case MK_E_DRAW: case MK_E_DRAW_IDX: {
            if (!st.in || !st.rp) break;
            mk_obj *p = st.gpipe ? mk_find(st.gpipe, MK_PIPE) : NULL;
            if (!p || p->compute) { MK_V("draw without a graphics pipeline"); break; }
            mk_obj *prp = mk_any(MK_H(p->gp.pass));
            if (!prp || !mk_rp_compat(&prp->rp, &st.rp->rp)) MK_V("pipeline built for a render pass incompatible with the open one");
            if (p->gp.subpass) MK_V("pipeline of subpass %u", p->gp.subpass);
            int dvp = 0, dsc = 0; for (uint32_t i = 0; i < p->gp.ndyn; i++) { dvp |= p->gp.dyn[i] == VK_DYNAMIC_STATE_VIEWPORT; dsc |= p->gp.dyn[i] == VK_DYNAMIC_STATE_SCISSOR; }
            if (dvp && !st.vp) MK_V("draw without a viewport");
            if (dsc && !st.sc) MK_V("draw without a scissor");
            if (st.sc && (st.scr.offset.x < st.area.offset.x || st.scr.offset.y < st.area.offset.y || st.scr.offset.x + st.scr.extent.width > st.area.offset.x + st.area.extent.width ||
                          st.scr.offset.y + st.scr.extent.height > st.area.offset.y + st.area.extent.height)) MK_V("scissor %d,%d %ux%u outside the render area %ux%u", st.scr.offset.x, st.scr.offset.y, st.scr.extent.width, st.scr.extent.height, st.area.extent.width, st.area.extent.height);
            for (uint32_t i = 0; i < p->gp.nbind; i++) { const uint32_t b = p->gp.bind[i].binding; if (b >= 8 || !st.vb[b]) MK_V("vertex binding %u not bound", b); }
            if (e->op == MK_E_DRAW_IDX) {
                mk_obj *ib = st.ib ? mk_find(st.ib, MK_BUFFER) : NULL;
                if (!ib) MK_V("indexed draw without an index buffer");
                else { const uint64_t isz = st.itype == VK_INDEX_TYPE_UINT32 ? 4 : 2; if (st.iboff + ((uint64_t)e->u[2] + e->u[0]) * isz > ib->size) MK_V("indices beyond the index buffer"); }
            }
            mk_v_set(cbo, &st, st.gset, MK_H(p->gp.layout), st.glayout, st.gdyn, st.gndyn);
            break;
        }
        case MK_E_DISPATCH: {
            mk_obj *p = st.cpipe ? mk_find(st.cpipe, MK_PIPE) : NULL;
            if (!p || !p->compute) { MK_V("dispatch without a compute pipeline"); break; }
            mk_v_set(cbo, &st, st.cset, MK_H(p->cs_layout), st.clayout, st.cdyn, st.cndyn);
            break;
        }
        default: break;
        }
    }
}

// ---------------------------------------------------------------- the translator's doors
typedef struct mk_bundle { char key[17]; xbg_vkn_stage st; } mk_bundle;
static mk_bundle mk_bundles[32]; static int mk_nbundles;
static int mk_xbg_down;                         // xbg_vkn_shader returns -1
static pthread_t mk_main_thread;
static int mk_xbg_calls, mk_xbg_off_main, mk_key_calls, mk_alpha_calls, mk_retype_calls, mk_variant_off_main;
static uint32_t mk_alpha_func; static float mk_alpha_ref; static uint64_t mk_retype_code; static void *mk_variant_xs;
// gate: while mk_gate_on, xbg_vkn_shader for the key mk_gate_key blocks (the worker stays inside the call)
static pthread_mutex_t mk_gate_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t mk_gate_cv = PTHREAD_COND_INITIALIZER;
static int mk_gate_on, mk_gate_inside; static char mk_gate_key[17];

static MK_U xbg_vkn_stage *mk_bundle_add(const char *key, uint32_t stage) {
    mk_bundle *b = &mk_bundles[mk_nbundles++]; memset(b, 0, sizeof *b);
    memcpy(b->key, key, 16); b->key[16] = 0;
    b->st.stage = stage; b->st.shader = (void *)(uintptr_t)(0x5a000 + 0x100 * mk_nbundles);
    b->st.module = (VkShaderModule)(uintptr_t)(0x6a0000ull + 0x100 * mk_nbundles);
    return &b->st;
}
static MK_U void mk_bind(xbg_vkn_stage *s, uint32_t orig, uint32_t type, uint32_t source, uint32_t slot, uint32_t bytes) {
    xbg_shader_binding *b = &s->bindings[s->binding_count++]; memset(b, 0, sizeof *b);
    b->binding = orig * 6 + s->stage; b->type = type; b->count = 1; b->bytes = bytes; b->source = source; b->slot = slot; b->sampler_slot = 0xffffffffu;
}
int xbg_shader_program_key(const void *control, const void *code, char out_hex17[17]) {
    (void)code; __atomic_add_fetch(&mk_key_calls, 1, __ATOMIC_RELAXED);
    const char *c = (const char *)control;
    if (c[0] == '!') return -1;
    memcpy(out_hex17, c, 16); out_hex17[16] = 0;
    return 0;
}
int xbg_vkn_shader(const char *key, const void *control, const void *code, xbg_vkn_stage *out) {
    (void)control; (void)code;
    __atomic_add_fetch(&mk_xbg_calls, 1, __ATOMIC_RELAXED);
    if (pthread_equal(pthread_self(), mk_main_thread)) __atomic_add_fetch(&mk_xbg_off_main, 0, __ATOMIC_RELAXED); else __atomic_add_fetch(&mk_xbg_off_main, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&mk_gate_mu);
    if (mk_gate_on && !strcmp(key, mk_gate_key)) { mk_gate_inside = 1; pthread_cond_broadcast(&mk_gate_cv); while (mk_gate_on) pthread_cond_wait(&mk_gate_cv, &mk_gate_mu); mk_gate_inside = 0; }
    pthread_mutex_unlock(&mk_gate_mu);
    if (mk_xbg_down) return -1;
    for (int i = 0; i < mk_nbundles; i++) if (!strcmp(mk_bundles[i].key, key)) { *out = mk_bundles[i].st; return 0; }
    return 1;
}
VkShaderModule xbg_vkn_alpha_module(void *shader, uint32_t func, float ref) {
    mk_alpha_calls++; mk_alpha_func = func; mk_alpha_ref = ref; mk_variant_xs = shader;
    if (!pthread_equal(pthread_self(), mk_main_thread)) mk_variant_off_main++;
    return (VkShaderModule)(uintptr_t)(0x7a00000ull + func);
}
VkShaderModule xbg_vkn_retype_module(void *shader, uint64_t code) {
    mk_retype_calls++; mk_retype_code = code; mk_variant_xs = shader;
    if (!pthread_equal(pthread_self(), mk_main_thread)) mk_variant_off_main++;
    return (VkShaderModule)(uintptr_t)(0x7b00000ull + (code & 0xffff));
}
void xbg_support_defaults(void *bytes1392, float width, float height) {   // the layout of vkn_shader.h's comment (xbg_vulkan.cpp)
    float *f = (float *)bytes1392; memset(bytes1392, 0, 1392);
    f[144 / 4] = 1.0f / width; f[148 / 4] = 1.0f / height; f[160 / 4] = width; f[164 / 4] = height;
    for (int i = 0; i < 73; i++) f[192 / 4 + i] = 1.0f;
}
#endif
