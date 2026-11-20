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
enum { MK_BUFFER, MK_MEMORY, MK_IMAGE, MK_VIEW, MK_SAMPLER, MK_DSL, MK_PL, MK_RP, MK_FB, MK_PIPE, MK_PCACHE, MK_POOL, MK_CB, MK_FENCE, MK_KINDS };
static const char *const mk_kind_name[MK_KINDS] = {"buffer", "memory", "image", "view", "sampler", "set layout", "pipeline layout", "render pass",
                                                   "framebuffer", "pipeline", "pipeline cache", "command pool", "command buffer", "fence"};
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
static void mk_del(uint64_t h, int kind) {
    if (!h) return;   // destroying VK_NULL_HANDLE is legal
    pthread_mutex_lock(&mk_mu);
    mk_obj *o = mk_find(h, kind);
    if (!o) mk_err("destroy of %s %#llx that is not a live %s", mk_kind_name[kind], (unsigned long long)h, mk_kind_name[kind]);
    else { o->live = 0; mk_destroyed[kind]++; if (kind == MK_MEMORY) { free(o->cpu); o->cpu = NULL; } }
    pthread_mutex_unlock(&mk_mu);
}
// public helpers for the test
static mk_obj *mk_obj_of(uint64_t h) { pthread_mutex_lock(&mk_mu); mk_obj *o = mk_any(h); pthread_mutex_unlock(&mk_mu); return o; }
static int mk_is_live(uint64_t h, int kind) { pthread_mutex_lock(&mk_mu); const int r = mk_find(h, kind) != NULL; pthread_mutex_unlock(&mk_mu); return r; }
static uint64_t mk_live(int kind) { return mk_created[kind] - mk_destroyed[kind]; }
static uint64_t mk_last(int kind) {   // the newest object of a kind (0 = none)
    pthread_mutex_lock(&mk_mu); uint64_t h = 0;
    for (uint32_t i = mk_nobj; i-- > 0;) if (mk_obj_tab[i]->kind == kind) { h = MK_BASE + MK_STEP * i; break; }
    pthread_mutex_unlock(&mk_mu); return h;
}
// The live views of an image, in creation order (up to n).
static uint32_t mk_views_of(VkImage img, VkImageView *out, uint32_t n) {
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
VkResult vkResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags f) { (void)f; pthread_mutex_lock(&mk_mu); mk_obj *o = mk_cb(cb, "vkResetCommandBuffer", -1); if (o) o->cb_state = 0; pthread_mutex_unlock(&mk_mu); return VK_SUCCESS; }
VkResult vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) { (void)bi; pthread_mutex_lock(&mk_mu); mk_obj *o = mk_cb(cb, "vkBeginCommandBuffer", 0); if (o) o->cb_state = 1; pthread_mutex_unlock(&mk_mu); return VK_SUCCESS; }
VkResult vkEndCommandBuffer(VkCommandBuffer cb) { pthread_mutex_lock(&mk_mu); mk_obj *o = mk_cb(cb, "vkEndCommandBuffer", 1); if (o) o->cb_state = 2; pthread_mutex_unlock(&mk_mu); return VK_SUCCESS; }
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
VkResult vkWaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout) {
    (void)d; (void)all; (void)timeout; VkResult r = VK_SUCCESS; pthread_mutex_lock(&mk_mu);
    for (uint32_t i = 0; i < n; i++) { mk_obj *o = mk_find((uint64_t)(uintptr_t)f[i], MK_FENCE); if (!o || !o->signaled) { mk_err("wait on a fence that is never signalled"); r = VK_TIMEOUT; } }
    pthread_mutex_unlock(&mk_mu); return r;
}
static VkCommandBuffer mk_last_submitted;
VkResult vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence fence) {
    pthread_mutex_lock(&mk_mu);
    if (q != MK_QUEUE) mk_err("submit to queue %p", (void *)q);
    if (mk_qlock_depth != 1) mk_err("vkQueueSubmit outside the queue lock (depth %d)", mk_qlock_depth);
    for (uint32_t i = 0; i < n; i++) for (uint32_t j = 0; j < si[i].commandBufferCount; j++) {
        mk_obj *o = mk_cb(si[i].pCommandBuffers[j], "vkQueueSubmit", 2); mk_last_submitted = si[i].pCommandBuffers[j]; (void)o;
    }
    if (fence) { mk_obj *o = mk_find((uint64_t)(uintptr_t)fence, MK_FENCE); if (!o || o->signaled) mk_err("submit with a dead or signalled fence"); else o->signaled = 1; }
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
static void mk_log_reset(void) { pthread_mutex_lock(&mk_mu); mk_nbar = mk_ncopy = mk_nicopy = 0; pthread_mutex_unlock(&mk_mu); }
void vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags dst, VkDependencyFlags df, uint32_t nm, const VkMemoryBarrier *m,
                          uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im) {
    (void)s; (void)dst; (void)df; (void)nm; (void)m; (void)nb; (void)b;
    pthread_mutex_lock(&mk_mu); mk_cb(cb, "vkCmdPipelineBarrier", 1);
    for (uint32_t i = 0; i < ni; i++) {
        mk_obj *o = mk_find((uint64_t)(uintptr_t)im[i].image, MK_IMAGE);
        if (!o) mk_err("barrier on a dead image");
        else if (im[i].subresourceRange.levelCount > o->ici.mipLevels || im[i].subresourceRange.layerCount > o->ici.arrayLayers) mk_err("barrier range outside the image");
        if (mk_nbar < MK_LOG) mk_bar[mk_nbar++] = (mk_bar_t){im[i].image, im[i].oldLayout, im[i].newLayout, im[i].subresourceRange};
    }
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
    pthread_mutex_unlock(&mk_mu);
}
void vkCmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy *c) {
    pthread_mutex_lock(&mk_mu); mk_cb(cb, "vkCmdCopyImage", 1);
    if (!mk_find((uint64_t)(uintptr_t)src, MK_IMAGE) || !mk_find((uint64_t)(uintptr_t)dst, MK_IMAGE)) mk_err("vkCmdCopyImage: dead image");
    if (mk_nicopy < 64) { mk_icopy_t *e = &mk_icopy[mk_nicopy++]; e->src = src; e->dst = dst; e->sl = sl; e->dl = dl; e->n = n > 16 ? 16 : n; memcpy(e->c, c, sizeof *c * e->n); }
    pthread_mutex_unlock(&mk_mu);
}
// A command buffer of the test's own, recording (for vkn_upload_run).
static VkCommandBuffer mk_test_cb(void) {
    pthread_mutex_lock(&mk_mu); mk_obj *o; const uint64_t h = mk_new(MK_CB, &o); o->cb_state = 1; pthread_mutex_unlock(&mk_mu);
    return (VkCommandBuffer)(uintptr_t)h;
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

static xbg_vkn_stage *mk_bundle_add(const char *key, uint32_t stage) {
    mk_bundle *b = &mk_bundles[mk_nbundles++]; memset(b, 0, sizeof *b);
    memcpy(b->key, key, 16); b->key[16] = 0;
    b->st.stage = stage; b->st.shader = (void *)(uintptr_t)(0x5a000 + 0x100 * mk_nbundles);
    b->st.module = (VkShaderModule)(uintptr_t)(0x6a0000ull + 0x100 * mk_nbundles);
    return &b->st;
}
static void mk_bind(xbg_vkn_stage *s, uint32_t orig, uint32_t type, uint32_t source, uint32_t slot, uint32_t bytes) {
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
