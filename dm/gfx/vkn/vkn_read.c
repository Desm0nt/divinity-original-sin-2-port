// [DOS2] VK-NATIVE stage 4: texture read-back (vkn_read.h). No game addresses, no engine record offsets, no NVN objects (the NVN
// format NUMBER of a texture is a plain enum value; its bytes per texel come from the pure helper nvn/nvn_tex.c).
#ifdef DM_GAME_DOS2
#include "vkn_read.h"
#include "vkn_frame.h"
#include "vkn_pass.h"
#include <pthread.h>
#include <string.h>
#include "../nvn/nvn_tex.h"

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"

static vkn_read_stats g_st;
static uint32_t g_red;
static int red_ok(void) { return g_red++ < 16; }

// ---- the recording thread (vkn_frame.h: one thread records; its begin listener tells which) ----
static pthread_t g_rec_thread; static int g_rec_known;
static void on_begin(void) { g_rec_thread = pthread_self(); __atomic_store_n(&g_rec_known, 1, __ATOMIC_RELEASE); }
void vkn_read_init(void) { static int done; if (!done) { done = 1; vkn_rec_on_begin(on_begin); } }

// ---- formats: bytes per texel of the VkFormat the backend stores (the uncompressed colour formats vkn_tex.c's nvn_vk can give) ----
static uint32_t vk_bpt(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16: case VK_FORMAT_R4G4B4A4_UNORM_PACK16: return 2;
    case VK_FORMAT_R8G8B8_UNORM: case VK_FORMAT_R8G8B8_SNORM: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8_SINT: return 3;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return 4;
    case VK_FORMAT_R16G16B16_SFLOAT: case VK_FORMAT_R16G16B16_UNORM: case VK_FORMAT_R16G16B16_SNORM: case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16_SINT: return 6;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: return 8;
    case VK_FORMAT_R32G32B32_SFLOAT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: return 12;
    case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: return 16;
    default: return 0;   // depth / stencil, block-compressed, anything else: not read
    }
}
uint32_t vkn_read_texel_bytes(const vkn_tex *t) {
    if (!t) return 0;
    int bw = 0, bh = 0;
    const int b = nvn_fmt_bpb((int)t->nvn_fmt, &bw, &bh);
    return b > 0 && bw == 1 && bh == 1 ? (uint32_t)b : 0;
}
int vkn_read_supported(const vkn_tex *t, const char **why) {
    const char *w = NULL;
    const uint32_t nb = vkn_read_texel_bytes(t), vb = t ? vk_bpt(t->format) : 0;
    if (!t) w = "no texture";
    else if (t->dead) w = "destroyed";
    else if (t->decoded) w = "BC decoded on the CPU (the blocks are gone)";
    else if (t->aspect != VK_IMAGE_ASPECT_COLOR_BIT) w = "depth / stencil";
    else if (t->samples > 1) w = "multisampled";
    else if (!nb) w = "NVN format without 1x1 texels (compressed or unknown)";
    else if (!vb) w = "stored format not readable";
    else if (t->widened ? vb * 3 != nb * 4 : vb != nb) w = "stored texel size differs from the NVN one";
    if (why) *why = w;
    return w == NULL;
}

int vkn_read_now_ok(void) {
    if (vkn_cb() && vkn_rec_presented()) return 0;
    return !__atomic_load_n(&g_rec_known, __ATOMIC_ACQUIRE) || pthread_equal(g_rec_thread, pthread_self());
}

static int fail(uint64_t *ctr, int rc) { (*ctr)++; return rc; }

int vkn_read_texels(vkn_tex *t, uint32_t level, uint32_t layer, int32_t x, int32_t y, uint32_t w, uint32_t h, void *dst, uint64_t pitch, uint64_t *wait_ns) {
    if (wait_ns) *wait_ns = 0;
    if (!t || t->dead) return fail(&g_st.e_tex, VKN_READ_E_TEX);
    const char *why = NULL;
    if (!vkn_read_supported(t, &why)) {
        if (red_ok()) vkn_logf(RED "read: texture %#llx (NVN format %#x, VkFormat %d, %ux%u): %s", (unsigned long long)t->handle, t->nvn_fmt, (int)t->format, t->w, t->h, why);
        return fail(&g_st.e_fmt, VKN_READ_E_FMT);
    }
    const int d3 = t->nvn_target == 2;
    const uint32_t lw = t->w >> level ? t->w >> level : 1, lh = t->h >> level ? t->h >> level : 1, ld = d3 ? (t->depth >> level ? t->depth >> level : 1) : t->layers;
    if (!dst || !w || !h || level >= t->mips || layer >= ld || x < 0 || y < 0 || (uint64_t)(uint32_t)x + w > lw || (uint64_t)(uint32_t)y + h > lh) {
        if (red_ok()) vkn_logf(RED "read: texture %#llx (%ux%u, %u levels, %u layers / slices): rectangle %d,%d %ux%u of level %u layer %u out of range", (unsigned long long)t->handle,
                               t->w, t->h, t->mips, ld, x, y, w, h, level, layer);
        return fail(&g_st.e_rect, VKN_READ_E_RECT);
    }
    if (vkn_cb() && vkn_rec_presented()) return fail(&g_st.e_busy, VKN_READ_E_BUSY);
    if (__atomic_load_n(&g_rec_known, __ATOMIC_ACQUIRE) && !pthread_equal(g_rec_thread, pthread_self())) return fail(&g_st.e_thread, VKN_READ_E_THREAD);
    const uint32_t nb = vkn_read_texel_bytes(t), vb = vk_bpt(t->format);
    const uint64_t row_out = pitch ? pitch : (uint64_t)w * nb;
    if (row_out < (uint64_t)w * nb) return fail(&g_st.e_rect, VKN_READ_E_RECT);

    // The recording: the engine's open off-frame one, else one of our own.
    int own = 0;
    if (!vkn_cb()) {
        if (vkn_rec_begin(0) || !vkn_cb()) return fail(&g_st.e_cb, VKN_READ_E_CB);
        own = 1;
    }
    VkCommandBuffer cb = vkn_cb();
    // Never written (no GPU write, no upload, no remembered clear): zeros. Uploads queued after the open recording began run first.
    if (t->layout == VK_IMAGE_LAYOUT_UNDEFINED && !t->pass_nclr && vkn_upload_pending_bytes()) { vkn_pass_end(); (void)vkn_upload_run(cb); }
    if (t->layout == VK_IMAGE_LAYOUT_UNDEFINED && !t->pass_nclr) {
        if (own) (void)vkn_rec_submit(1);   // (empty: closes our recording)
        for (uint32_t r = 0; r < h; r++) memset((uint8_t *)dst + r * row_out, 0, (size_t)w * nb);
        g_st.undefined_zero++; g_st.reads++; g_st.bytes += (uint64_t)w * h * nb;
        return VKN_READ_OK;
    }
    // Staging: bufferOffset a multiple of 4 and of the texel size (vkCmdCopyImageToBuffer).
    uint32_t a = 4; while (a % vb) a += 4;
    const uint64_t bytes = (uint64_t)w * h * vb;
    vkn_block b;
    if (vkn_block_alloc(VKN_ARENA_STAGING, bytes + a, 16, &b)) {
        if (own) (void)vkn_rec_submit(1);
        if (red_ok()) vkn_logf(RED "read: no staging memory for %llu bytes (texture %#llx)", (unsigned long long)bytes, (unsigned long long)t->handle);
        return fail(&g_st.e_mem, VKN_READ_E_MEM);
    }
    const uint64_t off = (b.off + a - 1) / a * a;
    vkn_pass_layout(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);   // ends a pass, runs remembered clears, transitions
    if (t->layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        vkn_block_free(&b);
        if (own) (void)vkn_rec_submit(1);
        if (red_ok()) vkn_logf(RED "read: texture %#llx could not be made a copy source (layout %d)", (unsigned long long)t->handle, (int)t->layout);
        return fail(&g_st.e_cb, VKN_READ_E_CB);
    }
    const VkBufferImageCopy rg = {.bufferOffset = off, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, d3 ? 0 : layer, 1}, .imageOffset = {x, y, d3 ? (int32_t)layer : 0}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.buf, 1, &rg);
    // The copy's writes made visible to the host reads after the fence (a fence alone does not).
    const VkBufferMemoryBarrier hb = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = b.buf, .offset = off, .size = bytes};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &hb, 0, NULL);
    if (own) g_st.own_rec++; else g_st.engine_rec++;
    const uint64_t t0 = vkn_now_ns();
    const int rc = vkn_rec_submit(1);
    const uint64_t dt = vkn_now_ns() - t0;
    if (wait_ns) *wait_ns = dt;
    g_st.wait_ns += dt;
    if (rc) {
        vkn_retire_block(&b);   // (the GPU may still own it)
        if (red_ok()) vkn_logf(RED "read: texture %#llx: submit / wait of the read-back failed: no data", (unsigned long long)t->handle);
        return fail(&g_st.e_submit, VKN_READ_E_SUBMIT);
    }
    // Out of the staging block (host-coherent arena memory): rows tight in the buffer (w * vb), `row_out` apart in dst.
    const uint8_t *src = (const uint8_t *)b.cpu + (off - b.off);
    for (uint32_t r = 0; r < h; r++) {
        const uint8_t *s = src + (uint64_t)r * w * vb; uint8_t *o = (uint8_t *)dst + r * row_out;
        if (!t->widened) { memcpy(o, s, (size_t)w * nb); continue; }
        const uint32_t cs = vb / 4;   // widened RGB -> RGBA: the 4th component (alpha = one) dropped
        for (uint32_t i = 0; i < w; i++) memcpy(o + (size_t)i * 3 * cs, s + (size_t)i * 4 * cs, 3 * cs);
    }
    vkn_block_free(&b);
    g_st.reads++; g_st.bytes += (uint64_t)w * h * nb;
    return VKN_READ_OK;
}

void vkn_read_stats_get(vkn_read_stats *s) { if (s) *s = g_st; }
#endif
