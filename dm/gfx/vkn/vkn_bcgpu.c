// [DOS2] VK-NATIVE stage 4: GPU BC decode -> RGBA8 / -> ASTC 4x4 for vkn_tex on GPUs without BC (vkn_bcgpu.h). The shaders, the encoder
// tables and both self-test sets are the translator's (xbg_bc_decode.comp: KB TEX-01 / TEX-02; port-common bc-astc-gpu
// xbg_bc_astc_encode.comp: KB TEX-18), taken through the narrow data doors of vkn_shlib_bc.cpp (moved there from xbg_game.cpp's [DOS2] VK-NATIVE export block); the pipelines,
// descriptor sets, scratch buffers and submissions are this file's own (the translator's bcgpu / bcastc buffers belong to its recording,
// which still runs in mode 2). No game addresses, no NVN objects: NVN format / target NUMBERS only.
#ifdef DM_GAME_DOS2
#define VKN_BCG_DOOR __attribute__((weak))
#include "vkn_bcgpu.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
extern void pc_note(const char *id, int applied, const char *fmt, ...) __attribute__((weak));   // [pc] (android/vulkan_present.cpp)

enum { MAXR = 4096, FMTS = 16 };   // regions a batch (the decoder's binary search has no limit; 32 bytes of table each); NVN 0x41..0x50

// ---- device state (g_mu) ----
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_tried, g_ok, g_why_dev;                     // prepare: tried, usable, VKN_BCGW_* when not
static int g_selftest = 1, g_astc_mode = 1, g_cands = 3, g_vt;   // DM_GFX_BC_SELFTEST, DM_GFX_BC_ASTC (0 off, 1 auto, 2 force), _CANDS, _VT
static uint64_t g_out_bytes, g_in_bytes, g_astc_bytes;
static VkBuffer g_in, g_out, g_astcb, g_tab; static VkDeviceMemory g_in_mem, g_out_mem, g_astc_mem, g_tab_mem;
static VkDescriptorSetLayout g_dsl_dec, g_dsl_enc; static VkPipelineLayout g_pl_dec, g_pl_enc; static VkDescriptorPool g_pool;
static VkDescriptorSet g_set_dec, g_set_enc; static VkShaderModule g_mod_dec, g_mod_enc;
static VkPipeline g_pipe[FMTS]; static uint8_t g_pipe_st[FMTS];   // per NVN format: 0 untried, 1 usable, 2 failed (pipeline or self-test)
static VkPipeline g_enc; static int g_astc_st, g_astc_off;        // route 2: 0 undecided, 1 on, -1 off (VKN_BCAK_*)
static vkn_bcgpu_stats g_st;

static void cnt(uint64_t *c, uint64_t v) { __atomic_add_fetch(c, v, __ATOMIC_RELAXED); }

static int rgba8_fmt(uint32_t f) { return (f >= 0x41 && f <= 0x48) || f == 0x4d || f == 0x4e; }
// a BC format and its sRGB twin (xbg_bc_astc.h xbg_bc_astc_family): 0 bc1, 1 bc1a, 2 bc2, 3 bc3, 4 bc7, -1 others
static int family(uint32_t f) {
    switch (f) { case 0x41: case 0x45: return 0; case 0x42: case 0x46: return 1; case 0x43: case 0x47: return 2; case 0x44: case 0x48: return 3;
                 case 0x4d: case 0x4e: return 4; default: return -1; }
}
static int is_srgb(uint32_t f) { return (f >= 0x45 && f <= 0x48) || f == 0x4e; }
static uint32_t block_bytes(uint32_t f) { return f == 0x41 || f == 0x42 || f == 0x45 || f == 0x46 ? 8 : 16; }
VkFormat vkn_bcgpu_format(uint32_t nvn, int route) {
    if (!rgba8_fmt(nvn)) return VK_FORMAT_UNDEFINED;
    if (route == VKN_BCG_ASTC) return is_srgb(nvn) ? VK_FORMAT_ASTC_4x4_SRGB_BLOCK : VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    if (route == VKN_BCG_RGBA8) return is_srgb(nvn) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    return VK_FORMAT_UNDEFINED;
}

static void full_barrier(VkCommandBuffer cb) {
    const VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL, 0, NULL);
}
static void host_barrier(VkCommandBuffer cb) {
    const VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, NULL, 0, NULL);
}
static int make_buffer(const vkn_dev *d, uint64_t n, VkMemoryPropertyFlags want, VkBuffer *b, VkDeviceMemory *m) {
    const VkBufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n,
                                   .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(d->dev, &ci, NULL, b) != VK_SUCCESS) { *b = VK_NULL_HANDLE; return -1; }
    VkMemoryRequirements r; vkGetBufferMemoryRequirements(d->dev, *b, &r);
    const int t = vkn_mem_type(r.memoryTypeBits, want, 0); if (t < 0) return -1;
    const VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size, .memoryTypeIndex = (uint32_t)t};
    if (vkAllocateMemory(d->dev, &ai, NULL, m) != VK_SUCCESS) { *m = VK_NULL_HANDLE; return -1; }
    if (vkBindBufferMemory(d->dev, *b, *m, 0) != VK_SUCCESS) return -1;
    if (want & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) cnt(&g_st.scratch_bytes, r.size);
    return 0;
}
static VkDescriptorSetLayout make_dsl(const vkn_dev *d, uint32_t n) {
    VkDescriptorSetLayoutBinding b[4]; memset(b, 0, sizeof b);
    for (uint32_t k = 0; k < n; k++) { b[k].binding = k; b[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b[k].descriptorCount = 1; b[k].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    const VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n, .pBindings = b};
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    return vkCreateDescriptorSetLayout(d->dev, &ci, NULL, &l) == VK_SUCCESS ? l : VK_NULL_HANDLE;
}
static VkPipelineLayout make_pl(const vkn_dev *d, VkDescriptorSetLayout dsl) {
    const VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    const VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl,
                                           .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkPipelineLayout l = VK_NULL_HANDLE;
    return vkCreatePipelineLayout(d->dev, &ci, NULL, &l) == VK_SUCCESS ? l : VK_NULL_HANDLE;
}
static VkDescriptorSet make_set(const vkn_dev *d, VkDescriptorSetLayout dsl, const VkBuffer *bufs, uint32_t n) {
    const VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_pool, .descriptorSetCount = 1, .pSetLayouts = &dsl};
    VkDescriptorSet s = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(d->dev, &ai, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorBufferInfo bi[4]; VkWriteDescriptorSet w[4]; memset(w, 0, sizeof w);
    for (uint32_t k = 0; k < n; k++) {
        bi[k] = (VkDescriptorBufferInfo){bufs[k], 0, VK_WHOLE_SIZE};
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[k].dstSet = s; w[k].dstBinding = k; w[k].descriptorCount = 1;
        w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[k].pBufferInfo = &bi[k];
    }
    vkUpdateDescriptorSets(d->dev, n, w, 0, NULL);
    return s;
}
static VkShaderModule make_module(const vkn_dev *d, int which) {
    size_t bytes = 0; const uint32_t *code = vkn_shlib_bc_blob(which, &bytes);
    if (!code || !bytes) return VK_NULL_HANDLE;
    const VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(d->dev, &ci, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}
static VkPipeline make_pipe(const vkn_dev *d, VkShaderModule m, VkPipelineLayout pl, const uint32_t *spec, uint32_t nspec) {
    const VkSpecializationMapEntry map[2] = {{0, 0, 4}, {1, 4, 4}};
    const VkSpecializationInfo si = {nspec, map, 4 * nspec, spec};
    const VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pl,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main", .pSpecializationInfo = &si}};
    VkPipeline p = VK_NULL_HANDLE;
    return vkCreateComputePipelines(d->dev, VK_NULL_HANDLE, 1, &ci, NULL, &p) == VK_SUCCESS ? p : VK_NULL_HANDLE;
}

static void destroy_locked(void) {
    const vkn_dev *d = vkn_device(); if (!d) return;
    for (int i = 0; i < FMTS; i++) if (g_pipe[i]) vkDestroyPipeline(d->dev, g_pipe[i], NULL);
    if (g_enc) vkDestroyPipeline(d->dev, g_enc, NULL);
    if (g_mod_dec) vkDestroyShaderModule(d->dev, g_mod_dec, NULL);
    if (g_mod_enc) vkDestroyShaderModule(d->dev, g_mod_enc, NULL);
    if (g_pool) vkDestroyDescriptorPool(d->dev, g_pool, NULL);
    if (g_pl_dec) vkDestroyPipelineLayout(d->dev, g_pl_dec, NULL);
    if (g_pl_enc) vkDestroyPipelineLayout(d->dev, g_pl_enc, NULL);
    if (g_dsl_dec) vkDestroyDescriptorSetLayout(d->dev, g_dsl_dec, NULL);
    if (g_dsl_enc) vkDestroyDescriptorSetLayout(d->dev, g_dsl_enc, NULL);
    const VkBuffer b[4] = {g_in, g_out, g_astcb, g_tab}; const VkDeviceMemory m[4] = {g_in_mem, g_out_mem, g_astc_mem, g_tab_mem};
    for (int i = 0; i < 4; i++) { if (b[i]) vkDestroyBuffer(d->dev, b[i], NULL); if (m[i]) vkFreeMemory(d->dev, m[i], NULL); }
    memset(g_pipe, 0, sizeof g_pipe); memset(g_pipe_st, 0, sizeof g_pipe_st);
    g_enc = VK_NULL_HANDLE; g_mod_dec = g_mod_enc = VK_NULL_HANDLE; g_pool = VK_NULL_HANDLE; g_pl_dec = g_pl_enc = VK_NULL_HANDLE;
    g_dsl_dec = g_dsl_enc = VK_NULL_HANDLE; g_set_dec = g_set_enc = VK_NULL_HANDLE;
    g_in = g_out = g_astcb = g_tab = VK_NULL_HANDLE; g_in_mem = g_out_mem = g_astc_mem = g_tab_mem = VK_NULL_HANDLE;
}

// Once: switches, doors, scratch buffers, the decoder's layouts / set / module (pipelines per format: decode_pipe_locked).
static int prepare_locked(void) {
    if (g_tried) return g_ok;
    g_tried = 1;
    const uint64_t t0 = vkn_now_ns();
    const char *e = getenv("DM_GFX_BC_DECODE");
    const int cpu = e && !strcmp(e, "cpu");
    e = getenv("DM_GFX_BC_SELFTEST"); g_selftest = e ? atoi(e) : 1; if (g_selftest < 0 || g_selftest > 2) g_selftest = 1;
    e = getenv("DM_GFX_BC_ASTC"); g_astc_mode = !e || !*e || !strcmp(e, "auto") ? 1 : !strcmp(e, "0") ? 0 : !strcmp(e, "1") ? 2 : 1;
    e = getenv("DM_GFX_BC_ASTC_CANDS"); g_cands = e && atoi(e) == 1 ? 1 : 3;
    e = getenv("DM_GFX_BC_ASTC_VT"); g_vt = e && atoi(e) != 0;
    e = getenv("DM_GFX_BC_BATCH_MB"); long mb = e ? atol(e) : 32; if (mb < 4) mb = 4; if (mb > 128) mb = 128;
    g_out_bytes = (uint64_t)mb << 20; g_in_bytes = g_out_bytes / 2 + (uint64_t)MAXR * 32; g_astc_bytes = g_out_bytes / 4;
    const vkn_dev *d = vkn_device();
    const char *why = NULL;
    if (cpu) { g_why_dev = VKN_BCGW_OFF; why = "DM_GFX_BC_DECODE=cpu"; }
    else if (!vkn_shlib_bc_blob || !vkn_shlib_bc_kat_input || !vkn_shlib_bc_kat_check || !vkn_shlib_bc_blob(0, NULL)) { g_why_dev = VKN_BCGW_DOOR; why = "no data doors (vkn_shlib_bc.cpp) in this build"; }
    else if (!d) { g_why_dev = VKN_BCGW_DEV; why = "no device"; }
    else {
        const uint32_t range = d->props.limits.maxStorageBufferRange;
        g_why_dev = VKN_BCGW_DEV;
        if (range && (range < g_in_bytes || range < g_out_bytes)) why = "maxStorageBufferRange below the scratch size (DM_GFX_BC_BATCH_MB)";
        else if (make_buffer(d, g_in_bytes, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g_in, &g_in_mem) ||
                 make_buffer(d, g_out_bytes, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g_out, &g_out_mem)) why = "scratch buffers failed";
        else if (!(g_dsl_dec = make_dsl(d, 2)) || !(g_pl_dec = make_pl(d, g_dsl_dec))) why = "decoder layouts failed";
        else {
            const VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6};
            const VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &ps};
            const VkBuffer bufs[2] = {g_in, g_out};
            if (vkCreateDescriptorPool(d->dev, &pi, NULL, &g_pool) != VK_SUCCESS) { g_pool = VK_NULL_HANDLE; why = "descriptor pool failed"; }
            else if (!(g_set_dec = make_set(d, g_dsl_dec, bufs, 2))) why = "decoder descriptor set failed";
            else if (!(g_mod_dec = make_module(d, 0))) why = "decoder shader module failed";
            else g_ok = 1;
        }
    }
    if (!g_ok) { destroy_locked(); g_st.ready = -1; vkn_logf("bc-gpu: GPU BC decode off (%s): BC textures without the pack are decoded on the CPU", why ? why : "?"); }
    else {
        g_st.ready = 1;
        vkn_logf("bc-gpu: GPU BC decode ready (scratch in %.1f MB, out %.1f MB, self-test %d (DM_GFX_BC_SELFTEST), DM_GFX_BC_ASTC=%s)", g_in_bytes / 1048576.0,
                 g_out_bytes / 1048576.0, g_selftest, g_astc_mode == 0 ? "0" : g_astc_mode == 2 ? "1" : "auto");
    }
    cnt(&g_st.prep_ns, vkn_now_ns() - t0);
    return g_ok;
}

// One self-test submission on the one-shot command buffer: staging (input + readback) of `bytes`, rec records the work. 0 = ran.
typedef void (*kat_rec)(VkCommandBuffer cb, const vkn_block *b, void *ctx);
static int kat_run(const vkn_block *b, kat_rec rec, void *ctx) {
    VkCommandBuffer cb = vkn_once_begin(); if (!cb) return -1;
    rec(cb, b, ctx);
    return vkn_once_end(cb);
}
typedef struct dec_kat { VkPipeline p; uint64_t in_b, out_b, rb_at; uint32_t push[4]; } dec_kat;
static void dec_kat_rec(VkCommandBuffer cb, const vkn_block *b, void *ctx) {
    const dec_kat *k = (const dec_kat *)ctx;
    full_barrier(cb);
    const VkBufferCopy c = {b->off, 0, k->in_b}; vkCmdCopyBuffer(cb, b->buf, g_in, 1, &c);
    full_barrier(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, k->p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl_dec, 0, 1, &g_set_dec, 0, NULL);
    vkCmdPushConstants(cb, g_pl_dec, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, k->push);
    vkCmdDispatch(cb, (k->push[1] + 63) / 64, 1, 1);
    full_barrier(cb);
    const VkBufferCopy o = {0, b->off + k->rb_at, k->out_b}; vkCmdCopyBuffer(cb, g_out, b->buf, 1, &o);
    host_barrier(cb);
}
// TEX-02: the format's 2048 known-answer blocks through the new pipeline; 1 = every pixel equals the CPU decoder (or no test).
static int dec_selftest_locked(VkPipeline p, uint32_t fmt) {
    if (!g_selftest) return 1;
    dec_kat k = {.p = p}; vkn_block b; char line[200] = "";
    const uint64_t cap = 2048ull * 16 + 64;   // input of 2048 blocks of at most 16 bytes + one region
    if (vkn_block_alloc(VKN_ARENA_STAGING, cap + 2048ull * 64, 16, &b)) { vkn_logf(RED "bc-gpu: self-test fmt=%#x: no staging memory, format left on the CPU", fmt); return 0; }
    size_t in_b = 0, out_b = 0; int bad = -1, ran = 0;
    if (!vkn_shlib_bc_kat_input(fmt, b.cpu, (size_t)cap, &in_b, &out_b, k.push) && out_b <= 2048ull * 64 && out_b <= g_out_bytes && in_b <= g_in_bytes) {
        k.in_b = in_b; k.out_b = out_b; k.rb_at = (in_b + 15) & ~15ull;
        if (k.rb_at + out_b <= b.size && !kat_run(&b, dec_kat_rec, &k)) { ran = 1; bad = vkn_shlib_bc_kat_check(fmt, (const uint8_t *)b.cpu + k.rb_at, out_b, line, sizeof line); }
    }
    vkn_block_free(&b);
    const int good = ran && bad == 0;
    vkn_logf("%sbc-gpu: decoder self-test fmt=%#x blocks=2048 ran=%d bad=%d%s%s -> %s", good ? "" : RED, fmt, ran, bad, line[0] ? " " : "", line,
             good ? "gpu" : g_selftest == 2 ? "gpu (DM_GFX_BC_SELFTEST=2 keeps it)" : "cpu (this format is decoded on the CPU)");
    return good || g_selftest == 2;
}
static int decode_pipe_locked(uint32_t fmt) {
    if (fmt < 0x41 || fmt > 0x50) return 0;
    const int i = (int)(fmt - 0x41);
    if (g_pipe_st[i]) return g_pipe_st[i] == 1;
    const vkn_dev *d = vkn_device(); if (!d) return 0;
    const uint64_t t0 = vkn_now_ns();
    const uint32_t spec[2] = {fmt, 0};   // options 0: BC1..BC3 / BC7 decode to RGBA8 whatever the options say
    VkPipeline p = make_pipe(d, g_mod_dec, g_pl_dec, spec, 2);
    int ok = p != VK_NULL_HANDLE;
    if (!ok) vkn_logf(RED "bc-gpu: decoder pipeline fmt=%#x failed: decoded on the CPU", fmt);
    else if (!dec_selftest_locked(p, fmt)) { vkDestroyPipeline(d->dev, p, NULL); p = VK_NULL_HANDLE; ok = 0; }
    g_pipe[i] = p; g_pipe_st[i] = ok ? 1 : 2;
    if (ok) g_st.kat_ok |= 1u << i; else g_st.kat_bad |= 1u << i;
    cnt(&g_st.prep_ns, vkn_now_ns() - t0);
    return ok;
}

// ---- route 2: the encoder, decided once ----
typedef struct enc_kat { uint64_t in_b, rgba_at, rgba_b, rb_at; uint32_t nd, blocks; uint32_t push[8][4]; } enc_kat;
static void enc_kat_rec(VkCommandBuffer cb, const vkn_block *b, void *ctx) {
    const enc_kat *k = (const enc_kat *)ctx;
    full_barrier(cb);
    VkBufferCopy c = {b->off, 0, k->in_b}; vkCmdCopyBuffer(cb, b->buf, g_in, 1, &c);
    c = (VkBufferCopy){b->off + k->rgba_at, 0, k->rgba_b}; vkCmdCopyBuffer(cb, b->buf, g_out, 1, &c);
    full_barrier(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_enc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl_enc, 0, 1, &g_set_enc, 0, NULL);
    for (uint32_t i = 0; i < k->nd; i++) {
        vkCmdPushConstants(cb, g_pl_enc, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, k->push[i]);
        vkCmdDispatch(cb, (k->push[i][2] - k->push[i][1] + 63) / 64, 1, 1);
    }
    full_barrier(cb);
    c = (VkBufferCopy){0, b->off + k->rb_at, (VkDeviceSize)k->blocks * 16}; vkCmdCopyBuffer(cb, g_astcb, b->buf, 1, &c);
    host_barrier(cb);
}
static int astc_feat_ok(const vkn_dev *d) {
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    const VkFormat f[2] = {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_4x4_SRGB_BLOCK};
    for (int i = 0; i < 2; i++) {
        VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(d->phys, f[i], &p);
        VkFormatFeatureFlags v = p.optimalTilingFeatures;
        if (d->props.apiVersion < VK_API_VERSION_1_1 && (v & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) v |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        if ((v & need) != need) return 0;
    }
    return 1;
}
// FN-61: the module's 285 blocks through the real pipeline and set; 1 = pass.
static int enc_selftest_locked(char *line, size_t cap) {
    enc_kat k; memset(&k, 0, sizeof k);
    const uint64_t in_cap = 64 * 1024, rgba_cap = 512 * 1024, rb_cap = 64 * 1024;
    vkn_block b;
    if (vkn_block_alloc(VKN_ARENA_STAGING, in_cap + rgba_cap + rb_cap, 16, &b)) { snprintf(line, cap, "no staging memory"); return 0; }
    size_t in_b = 0, rgba_b = 0;
    int good = 0, ran = 0, zero = 0, retries = 0;
    k.rgba_at = in_cap; k.rb_at = in_cap + rgba_cap;
    if (vkn_shlib_bca_kat_input(b.cpu, (size_t)in_cap, &in_b, (uint8_t *)b.cpu + k.rgba_at, (size_t)rgba_cap, &rgba_b, k.push, 8, &k.nd, &k.blocks) ||
        in_b > g_in_bytes || rgba_b > g_out_bytes || (uint64_t)k.blocks * 16 > rb_cap || (uint64_t)k.blocks * 16 > g_astc_bytes) snprintf(line, cap, "self-test does not fit");
    else {
        k.in_b = in_b; k.rgba_b = rgba_b;
        uint8_t *rb = (uint8_t *)b.cpu + k.rb_at;
        memset(rb, 0, (size_t)k.blocks * 16);
        if (!kat_run(&b, enc_kat_rec, &k)) {
            ran = 1; good = vkn_shlib_bca_kat_check(rb, (uint32_t)g_cands, &zero, line, cap);
            // FN-61: a result read as zeros may be a stale read: look again, then run the test once more
            if (zero) { retries++; good = vkn_shlib_bca_kat_check(rb, (uint32_t)g_cands, &zero, line, cap); }
            if (zero) { retries++; memset(rb, 0, (size_t)k.blocks * 16); ran = !kat_run(&b, enc_kat_rec, &k); if (ran) good = vkn_shlib_bca_kat_check(rb, (uint32_t)g_cands, &zero, line, cap); }
        } else snprintf(line, cap, "submission failed");
    }
    vkn_block_free(&b);
    vkn_logf("%sbc-gpu: encoder self-test ran=%d zero-retries=%d %s -> %s", good ? "" : RED, ran, retries, line, good ? "gpu" : "off (every BC texture stays RGBA8)");
    return ran && good;
}
static void astc_decide_locked(void) {
    if (g_astc_st) return;
    const uint64_t t0 = vkn_now_ns();
    const vkn_dev *d = vkn_device();
    char line[200] = "";
    int off = -1;
    if (!g_astc_mode) off = VKN_BCAK_OFF;
    else if (g_astc_mode == 1 && d && d->props.vendorID == 0x5143) off = VKN_BCAK_QCOM;
    else if (!d || !astc_feat_ok(d)) off = VKN_BCAK_FEAT;
    else if (!vkn_shlib_bca_kat_input || !vkn_shlib_bca_kat_check || !vkn_shlib_bca_flags || !vkn_shlib_bca_shape || !vkn_shlib_bc_blob(1, NULL) || !vkn_shlib_bc_blob(2, NULL)) {
        off = VKN_BCAK_ENC; snprintf(line, sizeof line, "no encoder doors in this build");
    } else {
        size_t tb = 0; const uint32_t *tab = vkn_shlib_bc_blob(2, &tb); void *map = NULL;
        VkBuffer eb[4] = {g_in, g_out, VK_NULL_HANDLE, VK_NULL_HANDLE};
        off = VKN_BCAK_ENC;
        if (make_buffer(d, g_astc_bytes, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g_astcb, &g_astc_mem)) snprintf(line, sizeof line, "ASTC scratch failed");
        else if (make_buffer(d, tb, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &g_tab, &g_tab_mem) ||
                 vkMapMemory(d->dev, g_tab_mem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) snprintf(line, sizeof line, "table buffer failed");
        else {
            memcpy(map, tab, tb); vkUnmapMemory(d->dev, g_tab_mem);
            eb[2] = g_astcb; eb[3] = g_tab;
            const uint32_t spec = (uint32_t)g_cands;
            if (!(g_dsl_enc = make_dsl(d, 4)) || !(g_pl_enc = make_pl(d, g_dsl_enc)) || !(g_set_enc = make_set(d, g_dsl_enc, eb, 4))) snprintf(line, sizeof line, "encoder layouts / set failed");
            else if (!(g_mod_enc = make_module(d, 1))) snprintf(line, sizeof line, "encoder shader module failed");
            else {
                const uint64_t p0 = vkn_now_ns();
                g_enc = make_pipe(d, g_mod_enc, g_pl_enc, &spec, 1);
                vkn_logf("bc-gpu: encoder pipeline %s ms=%.1f (candidates %d, scratch %.1f MB)", g_enc ? "ok" : "failed", (vkn_now_ns() - p0) / 1e6, g_cands, g_astc_bytes / 1048576.0);
                if (!g_enc) snprintf(line, sizeof line, "encoder pipeline failed");
                else if (enc_selftest_locked(line, sizeof line)) off = -1;
            }
        }
        snprintf(g_st.line, sizeof g_st.line, "%s", line);
        if (off >= 0) {   // route 2 stays off: the encoder's objects go (the ASTC scratch is 1/4 of a batch)
            if (g_enc) vkDestroyPipeline(d->dev, g_enc, NULL);
            if (g_mod_enc) vkDestroyShaderModule(d->dev, g_mod_enc, NULL);
            if (g_pl_enc) vkDestroyPipelineLayout(d->dev, g_pl_enc, NULL);
            if (g_dsl_enc) vkDestroyDescriptorSetLayout(d->dev, g_dsl_enc, NULL);
            if (g_astcb) vkDestroyBuffer(d->dev, g_astcb, NULL);
            if (g_astc_mem) vkFreeMemory(d->dev, g_astc_mem, NULL);
            if (g_tab) vkDestroyBuffer(d->dev, g_tab, NULL);
            if (g_tab_mem) vkFreeMemory(d->dev, g_tab_mem, NULL);
            g_enc = VK_NULL_HANDLE; g_mod_enc = VK_NULL_HANDLE; g_pl_enc = VK_NULL_HANDLE; g_dsl_enc = VK_NULL_HANDLE;
            g_astcb = g_tab = VK_NULL_HANDLE; g_astc_mem = g_tab_mem = VK_NULL_HANDLE; g_set_enc = VK_NULL_HANDLE;   // (the set stays in the pool)
        }
    }
    g_astc_st = off < 0 ? 1 : -1; g_astc_off = off < 0 ? 0 : off;
    g_st.astc = g_astc_st; g_st.astc_off = g_astc_off;
    static const char *const k_off[VKN_BCAK_N] = {"DM_GFX_BC_ASTC=0", "DM_GFX_BC_ASTC=auto on Qualcomm (this path only runs simulated here; =1 forces)",
        "VK_FORMAT_ASTC_4x4 lacks sampled / linear filter / transfer features", "encoder", "family", "shape", "image format properties"};
    if (off < 0) vkn_logf("bc-gpu: route 2 on: BC1 / BC2 / BC3 / BC7 without the pack stored as ASTC 4x4 (GPU decode + GPU encode, DM_GFX_BC_ASTC=%s)", g_astc_mode == 2 ? "1" : "auto");
    else vkn_logf("bc-gpu: route 2 off (%s%s%s): GPU-decoded BC stays RGBA8", k_off[off], line[0] ? ": " : "", line);
    if (pc_note) {
        if (off < 0) pc_note("bc-astc-gpu", 1, "vkn: GPU-decoded BC1 / BC2 / BC3 / BC7 stored as ASTC 4x4 (DM_GFX_BC_ASTC=%s, self-test ok, %d candidates; FN-60 / FN-61)", g_astc_mode == 2 ? "1" : "auto", g_cands);
        else pc_note("bc-astc-gpu", 0, "vkn: %s%s%s", k_off[off], line[0] ? ": " : "", line);
    }
    cnt(&g_st.prep_ns, vkn_now_ns() - t0);
}
static int family_ok_locked(int f) {
    static const uint32_t lin[5] = {0x41, 0x42, 0x43, 0x44, 0x4d}, srgb[5] = {0x45, 0x46, 0x47, 0x48, 0x4e};
    return f >= 0 && f < 5 && decode_pipe_locked(lin[f]) && decode_pipe_locked(srgb[f]);
}

int vkn_bcgpu_route(uint32_t nvn, uint32_t target, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers, uint32_t samples, int storage, int *why) {
    int dummy; if (!why) why = &dummy;
    int r = VKN_BCG_NONE, kept = -1;
    pthread_mutex_lock(&g_mu);
    if (!prepare_locked()) *why = g_why_dev;
    else if (!rgba8_fmt(nvn)) *why = VKN_BCGW_FMT;
    else if (samples > 1 || !(target == 1 || target == 4 || target == 8 || target == 9)) *why = VKN_BCGW_SHAPE;   // 2D, 2D array, cube, cube array
    else if (!decode_pipe_locked(nvn)) *why = VKN_BCGW_PIPE;
    else {
        r = VKN_BCG_RGBA8;
        astc_decide_locked();
        if (g_astc_st < 0) kept = g_astc_off;
        else if (!family_ok_locked(family(nvn))) kept = VKN_BCAK_FAMILY;
        else if (vkn_shlib_bca_shape(nvn, (int)target, w, h, levels, layers, (int)samples, storage, g_vt)) kept = VKN_BCAK_SHAPE;
        else {
            const vkn_dev *d = vkn_device(); VkImageFormatProperties q; memset(&q, 0, sizeof q);
            const VkImageCreateFlags fl = (target == 8 || target == 9) && w == h ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
            const VkResult qr = d ? vkGetPhysicalDeviceImageFormatProperties(d->phys, vkn_bcgpu_format(nvn, VKN_BCG_ASTC), VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, fl, &q) : VK_ERROR_INITIALIZATION_FAILED;
            if (qr != VK_SUCCESS || q.maxExtent.width < w || q.maxExtent.height < h || q.maxMipLevels < levels || q.maxArrayLayers < layers) kept = VKN_BCAK_PROPS;
            else r = VKN_BCG_ASTC;
        }
    }
    g_st.routed[r]++;
    if (r == VKN_BCG_NONE) g_st.why[*why < VKN_BCGW_N ? *why : 0]++;
    if (kept >= 0) g_st.kept[kept]++;
    pthread_mutex_unlock(&g_mu);
    if (kept == VKN_BCAK_SHAPE || kept == VKN_BCAK_PROPS) {
        static uint32_t told;
        if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 16)
            vkn_logf("bc-gpu: kept RGBA8 fmt=%#x target=%u %ux%u levels=%u layers=%u%s (%s)", nvn, target, w, h, levels, layers, storage ? " storage" : "",
                     kept == VKN_BCAK_SHAPE ? vkn_shlib_bca_shape(nvn, (int)target, w, h, levels, layers, (int)samples, storage, g_vt) : "image format properties");
    }
    return r;
}

// ---- jobs ----
typedef struct bcg_batch {
    uint32_t *tab; VkBufferImageCopy *c; uint32_t nr, cap_r;   // region table (8 words a region, input offsets from the raw start), copies
    uint8_t *raw; uint64_t raw_bytes, raw_cap, out_bytes; uint32_t blocks;
    uint64_t in_off, in_bytes;                                 // in the staging block (vkn_bcgpu_job_stage)
} bcg_batch;
struct vkn_bcg_job {
    uint32_t nvn, route, bb, flags; VkPipeline dec;
    bcg_batch *b; uint32_t nb, cap_b;
    vkn_block blk; int staged; uint64_t ns;
};
vkn_bcg_job *vkn_bcgpu_job_new(uint32_t nvn, int route) {
    if ((route != VKN_BCG_ASTC && route != VKN_BCG_RGBA8) || !rgba8_fmt(nvn)) return NULL;
    pthread_mutex_lock(&g_mu);
    const int i = (int)(nvn - 0x41);
    VkPipeline dec = g_ok && g_pipe_st[i] == 1 ? g_pipe[i] : VK_NULL_HANDLE;
    const int enc_ok = route != VKN_BCG_ASTC || (g_astc_st > 0 && g_enc);
    pthread_mutex_unlock(&g_mu);
    if (!dec || !enc_ok) return NULL;
    vkn_bcg_job *j = (vkn_bcg_job *)calloc(1, sizeof *j); if (!j) return NULL;
    j->nvn = nvn; j->route = (uint32_t)route; j->bb = block_bytes(nvn); j->dec = dec;
    j->flags = route == VKN_BCG_ASTC && vkn_shlib_bca_flags ? vkn_shlib_bca_flags(nvn) : 0;
    return j;
}
static uint64_t al16(uint64_t n) { return (n + 15) & ~15ull; }
static bcg_batch *batch_for(vkn_bcg_job *j, uint64_t raw_n, uint64_t out_n, uint32_t nblocks) {
    bcg_batch *b = j->nb ? &j->b[j->nb - 1] : NULL;
    if (b && b->nr < MAXR && (uint64_t)(b->nr + 1) * 32 + al16(b->raw_bytes) + raw_n <= g_in_bytes && al16(b->out_bytes) + out_n <= g_out_bytes &&
        (uint64_t)(b->blocks + nblocks) * 16 <= g_astc_bytes) return b;
    if (j->nb == j->cap_b) {
        const uint32_t nc = j->cap_b ? 2 * j->cap_b : 4;
        bcg_batch *nb = (bcg_batch *)realloc(j->b, sizeof *nb * nc); if (!nb) return NULL;
        j->b = nb; j->cap_b = nc;
    }
    b = &j->b[j->nb++]; memset(b, 0, sizeof *b);
    return b;
}
static int add_region(vkn_bcg_job *j, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *src, uint64_t pitch,
                      uint32_t bx, uint32_t by) {
    const uint64_t row = (uint64_t)bx * j->bb, raw_n = row * by, out_n = (uint64_t)bx * by * 64;
    bcg_batch *b = batch_for(j, raw_n, out_n, bx * by); if (!b) return -1;
    if (b->nr == b->cap_r) {
        const uint32_t nc = b->cap_r ? 2 * b->cap_r : 8;
        uint32_t *t = (uint32_t *)realloc(b->tab, sizeof(uint32_t) * 8 * nc); if (!t) return -1;
        b->tab = t;
        VkBufferImageCopy *c = (VkBufferImageCopy *)realloc(b->c, sizeof *c * nc); if (!c) return -1;
        b->c = c; b->cap_r = nc;
    }
    const uint64_t raw_at = al16(b->raw_bytes), out_at = al16(b->out_bytes);
    if (raw_at + raw_n > b->raw_cap) {
        uint64_t nc = b->raw_cap ? b->raw_cap : 65536; while (nc < raw_at + raw_n) nc *= 2;
        uint8_t *r = (uint8_t *)realloc(b->raw, (size_t)nc); if (!r) return -1;
        b->raw = r; b->raw_cap = nc;
    }
    memset(b->raw + b->raw_bytes, 0, (size_t)(raw_at - b->raw_bytes));
    for (uint32_t r = 0; r < by; r++) memcpy(b->raw + raw_at + r * row, src + r * pitch, (size_t)row);
    // xbg_bc_gpu.h XbgBcGpuBatch region words: first block, blocks x, blocks a slice, blocks y, input word of its first block (moved past the
    // table at staging), output word of its first pixel, output words per pixel row (RGBA8: blocks x * 4), block count
    uint32_t *t = b->tab + 8 * b->nr;
    t[0] = b->blocks; t[1] = bx; t[2] = bx * by; t[3] = by; t[4] = (uint32_t)(raw_at / 4); t[5] = (uint32_t)(out_at / 4); t[6] = bx * 4; t[7] = bx * by;
    VkBufferImageCopy *c = &b->c[b->nr];
    memset(c, 0, sizeof *c);
    // RGBA8: the decoder's output (rows padded to whole blocks); ASTC: the encoder's blocks, block g of the batch at 16 * g (tight rows)
    c->bufferOffset = j->route == VKN_BCG_ASTC ? (VkDeviceSize)b->blocks * 16 : out_at;
    c->bufferRowLength = bx * 4; c->bufferImageHeight = by * 4;
    c->imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1};
    c->imageOffset = (VkOffset3D){(int32_t)x, (int32_t)y, 0}; c->imageExtent = (VkExtent3D){w, h, 1};
    b->nr++; b->raw_bytes = raw_at + raw_n; b->out_bytes = out_at + out_n; b->blocks += bx * by;
    return 0;
}
int vkn_bcgpu_job_add(vkn_bcg_job *j, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *blocks, uint64_t pitch) {
    if (!j || !blocks || !w || !h || (x & 3) || (y & 3)) return -1;
    const uint64_t t0 = vkn_now_ns();
    const uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    if (pitch < (uint64_t)bx * j->bb || (uint64_t)bx * 64 > g_out_bytes) return -1;
    // a rectangle bigger than a batch: bands of block rows (each band a region of its own; the last one may be short)
    uint32_t band = (uint32_t)(g_out_bytes / ((uint64_t)bx * 64)); if (band > by) band = by;
    const uint32_t astc_rows = (uint32_t)(g_astc_bytes / ((uint64_t)bx * 16)); if (band > astc_rows) band = astc_rows;
    if (!band) return -1;
    for (uint32_t r = 0; r < by; r += band) {
        const uint32_t n = by - r < band ? by - r : band, hh = h - r * 4 < n * 4 ? h - r * 4 : n * 4;
        if (add_region(j, level, layer, x, y + r * 4, w, hh, (const uint8_t *)blocks + r * pitch, pitch, bx, n)) return -1;
    }
    j->ns += vkn_now_ns() - t0;
    return 0;
}
int vkn_bcgpu_job_stage(vkn_bcg_job *j, uint64_t *bytes) {
    if (!j || !j->nb || j->staged) return -1;
    const uint64_t t0 = vkn_now_ns();
    uint64_t total = 0;
    for (uint32_t i = 0; i < j->nb; i++) { bcg_batch *b = &j->b[i]; b->in_off = total; b->in_bytes = (uint64_t)b->nr * 32 + b->raw_bytes; total = al16(total + b->in_bytes); }
    if (vkn_block_alloc(VKN_ARENA_STAGING, total, 16, &j->blk)) return -1;
    for (uint32_t i = 0; i < j->nb; i++) {
        const bcg_batch *b = &j->b[i]; uint8_t *o = (uint8_t *)j->blk.cpu + b->in_off;
        uint32_t *w = (uint32_t *)o;
        memcpy(w, b->tab, (size_t)b->nr * 32);
        for (uint32_t k = 0; k < b->nr; k++) w[k * 8 + 4] += b->nr * 8;   // input offsets past the table (XbgBcGpuBatch::write_input)
        memcpy(o + (size_t)b->nr * 32, b->raw, (size_t)b->raw_bytes);
    }
    j->staged = 1;
    if (bytes) *bytes = total;
    j->ns += vkn_now_ns() - t0;
    cnt(&g_st.jobs, 1); cnt(&g_st.batches, j->nb); cnt(&g_st.in_bytes, total); cnt(&g_st.build_ns, j->ns);
    for (uint32_t i = 0; i < j->nb; i++) cnt(&g_st.blocks, j->b[i].blocks);
    return 0;
}
void vkn_bcgpu_job_record(VkCommandBuffer cb, vkn_bcg_job *j, VkImage img) {
    if (!j || !j->staged || !cb || !img) return;
    for (uint32_t i = 0; i < j->nb; i++) {
        const bcg_batch *b = &j->b[i];
        const uint32_t push_d[4] = {b->nr, b->blocks, 4, j->bb / 4};
        full_barrier(cb);   // the scratch: every earlier batch (any command buffer, earlier in queue order) is done with it
        const VkBufferCopy c = {j->blk.off + b->in_off, 0, b->in_bytes}; vkCmdCopyBuffer(cb, j->blk.buf, g_in, 1, &c);
        full_barrier(cb);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, j->dec);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl_dec, 0, 1, &g_set_dec, 0, NULL);
        vkCmdPushConstants(cb, g_pl_dec, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, push_d);
        vkCmdDispatch(cb, (b->blocks + 63) / 64, 1, 1);
        full_barrier(cb);
        if (j->route == VKN_BCG_ASTC) {
            const uint32_t push_e[4] = {b->nr, 0, b->blocks, j->flags};
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_enc);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl_enc, 0, 1, &g_set_enc, 0, NULL);
            vkCmdPushConstants(cb, g_pl_enc, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, push_e);
            vkCmdDispatch(cb, (b->blocks + 63) / 64, 1, 1);
            full_barrier(cb);
            vkCmdCopyBufferToImage(cb, g_astcb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b->nr, b->c);
            cnt(&g_st.enc_dispatch, 1);
        } else vkCmdCopyBufferToImage(cb, g_out, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b->nr, b->c);
        cnt(&g_st.dec_dispatch, 1); cnt(&g_st.copies, b->nr);
    }
    cnt(&g_st.recorded, 1);
}
void vkn_bcgpu_job_free(vkn_bcg_job *j) {
    if (!j) return;
    if (j->staged) vkn_retire_block(&j->blk);
    for (uint32_t i = 0; i < j->nb; i++) { free(j->b[i].tab); free(j->b[i].c); free(j->b[i].raw); }
    free(j->b); free(j);
}
uint32_t vkn_bcgpu_job_batches(const vkn_bcg_job *j) { return j ? j->nb : 0; }

void vkn_bcgpu_get_stats(vkn_bcgpu_stats *s) {
    pthread_mutex_lock(&g_mu);
    *s = g_st;
    pthread_mutex_unlock(&g_mu);
}
void vkn_bcgpu_reset(void) {
    pthread_mutex_lock(&g_mu);
    destroy_locked();
    g_tried = g_ok = g_why_dev = 0; g_astc_st = g_astc_off = 0;
    memset(&g_st, 0, sizeof g_st);
    pthread_mutex_unlock(&g_mu);
}
#endif
