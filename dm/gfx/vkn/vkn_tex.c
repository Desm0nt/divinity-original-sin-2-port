// [DOS2] VK-NATIVE stage 2: textures and samplers (vkn_tex.h). No game addresses, no engine record offsets, no NVN objects: the NVN
// format / target NUMBERS the engine used are plain enum values the host file passes; the Switch block-linear layout of packaged
// storage comes from the pure helpers nvn/nvn_tex.c + tex/nvn_deswizzle.c, CPU BC decoding from tex/nvn_bcdec.c, the ASTC blocks of
// the setup pack (stage 4, devices without BC) from vkn_astc.c, the GPU decode (-> ASTC 4x4) jobs (stage 4, after the pack) from vkn_bcgpu.c.
// Tables (one mutex g_mu): handle index -> linked texture (vkn_tab), cookie -> texture (chained hash), texture slot -> texture, engine
// texture-pool id -> (slot, view) (pages of 4096 u32 over the engine's 0x100000 ids), sampler handle -> (pool id, VkSampler) and
// sampler-pool id -> VkSampler (0x1000 ids).
// Lifetime: a texture has one reference for the engine (dropped by its destroy event) and one per queued upload (dropped once the
// upload is recorded); at zero it goes to vkn_retire (destroyed 3 frames later). An upload of a texture the engine already destroyed
// records nothing.
// Shared render-target storage (vkn_tex_create_shared): several engine textures may be ONE vkn_tex. The object's own handle / cookie /
// pool ids are one engine texture's; the others are name records (vkn_tex_name, t->names) whose handle, cookie and pool ids resolve to
// the same object, so every user (passes, draws, copies, read-back, UI, dump) sees one image with one layout and one pass state. The
// engine reference is dropped when the LAST name is destroyed: a name record leaves alone; the object's own name leaving hands its
// identity fields to the first name record (handover_locked). Images on shared storage are chained by identity (g_store).
#ifdef DM_GAME_DOS2
#include "vkn_tex.h"
#include "vkn_state.h"   // vkn_fb_forget_view: framebuffers of a dying texture's views
#include "vkn_astc.h"    // stage 4: the setup ASTC pack
#include "vkn_bcgpu.h"   // stage 4: GPU BC decode (-> ASTC 4x4) at upload
#include "vkn_fx.h"      // [stage 5 A1] vkn_sampler_cmp_by_id (the comparison twin of a sampler)
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../nvn/nvn_tex.h"
#include "../tex/nvn_deswizzle.h"
#include "../tex/nvn_bcdec.h"

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"

// ---- formats ----
// Engine format 0..62 -> NVN format number {linear, sRGB} (the engine's 0x919c20 rule; 0xffff = the engine traps on it, 0 = none).
static const uint16_t k_efmt_nvn[63][2] = {
    {0, 0}, {0x01, 0x01}, {0x0d, 0x0d}, {0x19, 0x19}, {0x25, 0x38},                 //  0 None, R8, RG8, RGB8, RGBA8
    {0x06, 0x06}, {0x12, 0x12}, {0x1e, 0x1e}, {0x2a, 0x2a},                         //  5 R16, RG16, RGB16, RGBA16
    {0x02, 0x02}, {0x0e, 0x0e}, {0x1a, 0x1a}, {0x26, 0x26},                         //  9 R8S, RG8S, RGB8S, RGBA8S
    {0x07, 0x07}, {0x13, 0x13}, {0x1f, 0x1f}, {0x2b, 0x2b},                         // 13 R16S .. RGBA16S
    {0x05, 0x05}, {0x11, 0x11}, {0x1d, 0x1d}, {0x29, 0x29},                         // 17 R16F .. RGBA16F
    {0x0a, 0x0a}, {0x16, 0x16}, {0x22, 0x22}, {0x2e, 0x2e},                         // 21 R32F .. RGBA32F
    {0x09, 0x09}, {0x15, 0x15}, {0x21, 0x21}, {0x2d, 0x2d},                         // 25 R16I .. RGBA16I
    {0x0c, 0x0c}, {0x18, 0x18}, {0x24, 0x24}, {0x30, 0x30},                         // 29 R32I .. RGBA32I
    {0x08, 0x08}, {0x14, 0x14}, {0x20, 0x20}, {0x2c, 0x2c},                         // 33 R16UI .. RGBA16UI
    {0x0b, 0x0b}, {0x17, 0x17}, {0x23, 0x23}, {0x2f, 0x2f},                         // 37 R32UI .. RGBA32UI
    {0, 0}, {0x40, 0x40}, {0x3f, 0x3f}, {0x95, 0x95}, {0x39, 0x39}, {0x3d, 0x3d},   // 41 RGBE8, RGB9E5, RG11B10F, RGB565, RGBA4, RGB10A2
    {0xffff, 0xffff}, {0x98, 0x98},                                                 // 47 BGR10A2 (trap), A1RGB5
    {0x32, 0x32}, {0x33, 0x33}, {0x35, 0x35}, {0x34, 0x34},                         // 49 D16, D24, D24S8, D32F
    {0x41, 0x45}, {0x43, 0x47}, {0x44, 0x48}, {0x49, 0x49}, {0x4b, 0x4b}, {0x50, 0x50}, {0x4d, 0x4d},   // 53 DXT1, DXT3, DXT5, BC4U, BC5U, BC6H, BC7
    {0x99, 0x99}, {0x9a, 0x9c}, {0xffff, 0xffff},                                   // 60 BGR8, BGRA8, D32FS8 (trap)
};
// NVN format number -> VkFormat (the translator's order, xbg_game.cpp format()). x: alpha reads as one (RGBX formats).
static VkFormat nvn_vk(uint32_t f, int *x) {
    static const VkFormat t[0x37] = {VK_FORMAT_UNDEFINED,
        VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT,
        VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT,
        VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT,
        VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT,
        VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT,
        VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT,
        VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8_SINT,
        VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_SINT,
        VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT,
        VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT,
        VK_FORMAT_S8_UINT, VK_FORMAT_D16_UNORM, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
    *x = 0;
    if (f < 0x37) return t[f];
    if (f >= 0x41 && f <= 0x50) {   // DXT1 RGB / RGBA, DXT3, DXT5, the same four sRGB, RGTC1 U / S, RGTC2 U / S, BPTC unorm / sRGB, BPTC sfloat / ufloat
        static const VkFormat bc[16] = {VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK,
            VK_FORMAT_BC1_RGB_SRGB_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK,
            VK_FORMAT_BC4_SNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_SNORM_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK,
            VK_FORMAT_BC6H_SFLOAT_BLOCK, VK_FORMAT_BC6H_UFLOAT_BLOCK};
        return bc[f - 0x41];
    }
    if (f >= 0x79 && f <= 0x94) { const int k = (int)(f - 0x79) % 14; return (VkFormat)((int)VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * k + (f >= 0x87)); }
    switch (f) {
    case 0x37: *x = 1; return VK_FORMAT_R8G8B8A8_SRGB;            // RGBX8 sRGB
    case 0x38: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x3d: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x3e: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 0x3f: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x6d: *x = 1; return VK_FORMAT_R8G8B8A8_UNORM;           // RGBX8
    case 0x9a: return VK_FORMAT_B8G8R8A8_UNORM;
    case 0x99: *x = 1; return VK_FORMAT_B8G8R8A8_UNORM;           // BGRX8 (engine "BGR8", 4 bytes a pixel)
    case 0x9b: *x = 1; return VK_FORMAT_B8G8R8A8_SRGB;
    case 0x9c: return VK_FORMAT_B8G8R8A8_SRGB;
    // 16-bit packed formats: never reached the translator (it has no mapping for them); bit orders are guesses (LSB-first names).
    case 0x95: return VK_FORMAT_R5G6B5_UNORM_PACK16;              // BGR565 (engine RGB565)
    case 0x3c: return VK_FORMAT_B5G6R5_UNORM_PACK16;              // RGB565
    case 0x98: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;            // A1BGR5 (engine A1RGB5)
    case 0x39: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;            // RGBA4 (swizzled in the view, see fmt_swizzle)
    default: return VK_FORMAT_UNDEFINED;
    }
}
VkFormat vkn_tex_nvn_format(uint32_t nvn) { int x; return nvn_vk(nvn, &x); }
VkFormat vkn_tex_engine_format(uint32_t efmt, int srgb, uint32_t *nvn_out) {
    const uint32_t n = efmt < 63 ? k_efmt_nvn[efmt][srgb ? 1 : 0] : 0;
    if (nvn_out) *nvn_out = n == 0xffff ? 0 : n;
    return n && n != 0xffff ? vkn_tex_nvn_format(n) : VK_FORMAT_UNDEFINED;
}
static int fmt_is_bc(uint32_t f) { return f >= 0x41 && f <= 0x50; }
static int fmt_is_depth(VkFormat f) {
    return f == VK_FORMAT_S8_UINT || f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D24_UNORM_S8_UINT ||
           f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}
static VkImageAspectFlags fmt_aspect(VkFormat f) {
    if (f == VK_FORMAT_S8_UINT) return VK_IMAGE_ASPECT_STENCIL_BIT;
    if (f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (fmt_is_depth(f)) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}
// 3-component formats the device may not sample: the 4-component format, the component size and the bit pattern of alpha = 1.
static int widen_of(uint32_t f, VkFormat *to, uint32_t *cs, uint32_t *one) {
    static const struct { uint8_t f, cs; uint32_t one; VkFormat to; } w[] = {
        {0x19, 1, 0xff, VK_FORMAT_R8G8B8A8_UNORM}, {0x1a, 1, 0x7f, VK_FORMAT_R8G8B8A8_SNORM}, {0x1b, 1, 1, VK_FORMAT_R8G8B8A8_UINT}, {0x1c, 1, 1, VK_FORMAT_R8G8B8A8_SINT},
        {0x1d, 2, 0x3c00, VK_FORMAT_R16G16B16A16_SFLOAT}, {0x1e, 2, 0xffff, VK_FORMAT_R16G16B16A16_UNORM}, {0x1f, 2, 0x7fff, VK_FORMAT_R16G16B16A16_SNORM},
        {0x20, 2, 1, VK_FORMAT_R16G16B16A16_UINT}, {0x21, 2, 1, VK_FORMAT_R16G16B16A16_SINT},
        {0x22, 4, 0x3f800000u, VK_FORMAT_R32G32B32A32_SFLOAT}, {0x23, 4, 1, VK_FORMAT_R32G32B32A32_UINT}, {0x24, 4, 1, VK_FORMAT_R32G32B32A32_SINT}};
    for (unsigned i = 0; i < sizeof w / sizeof *w; i++) if (w[i].f == f) { *to = w[i].to; *cs = w[i].cs; *one = w[i].one; return 1; }
    return 0;
}
// BC decoded on the CPU: the format nvn_bc_decode writes (its bytes per pixel come from nvn_bc_unpacked).
static VkFormat bc_decoded(uint32_t f) {
    switch (f) {
    case 0x41: case 0x42: case 0x43: case 0x44: case 0x4d: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x45: case 0x46: case 0x47: case 0x48: case 0x4e: return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x49: return VK_FORMAT_R8_UNORM;
    case 0x4a: return VK_FORMAT_R8_SNORM;
    case 0x4b: return VK_FORMAT_R8G8_UNORM;
    case 0x4c: return VK_FORMAT_R8G8_SNORM;
    case 0x4f: case 0x50: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}
static VkFormatFeatureFlags fmt_feat(VkFormat f) {
    const vkn_dev *d = vkn_device(); if (!d || f == VK_FORMAT_UNDEFINED) return 0;
    VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(d->phys, f, &p);
    VkFormatFeatureFlags v = p.optimalTilingFeatures;
    // TRANSFER_DST is a 1.1 bit (KHR_maintenance1): a 1.0 driver does not report it, copies into a sampled format work there anyway.
    if (d->props.apiVersion < VK_API_VERSION_1_1 && (v & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) v |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    return v;
}
// DM_GFX_SIM_GPU=mali / powervr (the port's capability model of a GPU without BC, xbg_vendor_policy.h): the real device here samples
// BC, so the model must force the decision a real Mali gets from its format properties.
static int sim_no_bc(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_GFX_SIM_GPU"); v = e && (!strcmp(e, "mali") || !strcmp(e, "powervr")); }
    return v;
}
static VkComponentMapping fmt_swizzle(uint32_t nvn, int x) {
    VkComponentMapping m = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    if (x) m.a = VK_COMPONENT_SWIZZLE_ONE;
    if (nvn == 0x39) { m.r = VK_COMPONENT_SWIZZLE_A; m.g = VK_COMPONENT_SWIZZLE_B; m.b = VK_COMPONENT_SWIZZLE_G; m.a = VK_COMPONENT_SWIZZLE_R; }   // R in the low bits (guess)
    return m;
}

// ---- tables ----
enum { TEX_HANDLES = 1u << 17, TEX_SLOTS = 1u << 17, COOKIE_BUCKETS = 1u << 16, ID_PAGE = 4096, ID_PAGES = 0x100000 / ID_PAGE, SAMP_IDS = 0x1000, SAMP_HANDLES = 1u << 14 };
typedef struct samp_rec { VkSampler s; uint32_t id; } samp_rec;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_init;                                // 1 ok, -1 failed
static vkn_tab g_handles;                         // vkn_tex * by engine handle
static vkn_tex **g_slot; static uint32_t *g_free_slot, g_nfree, g_slot_hi;
static vkn_tex *g_cookie[COOKIE_BUCKETS];
static uint32_t *g_id_page[ID_PAGES];             // (slot + 1) << 3 | view (0 whole, 1..7 layer i - 1)
static vkn_tab g_samp; static VkSampler g_samp_by_id[SAMP_IDS];
static vkn_tex_stats g_st;
static uint64_t g_up_serial;                      // [stage 5 A3] the upload numbers (vkn_tex.shc_up; atomic)
// Shared render-target storage (vkn_tex_create_shared): an engine texture attached to another one's image is a name record - its
// handle, cookie and pool ids resolve to that vkn_tex (cookie hash of names g_ncookie; the handle table and the pool-id cells hold the
// vkn_tex itself). Images that sit on shared storage are chained by storage identity in g_store. g_share_mu serialises shared
// creations (taken before g_mu), so two creations on one storage always see each other.
typedef struct vkn_tex_name {
    uint64_t handle, cookie; uint32_t pool_id, layer_ids[VKN_TEX_LAYER_IDS], sampler_id; uint8_t linked;
    vkn_tex *t; struct vkn_tex_name *cnext, *tnext;   // cookie bucket chain; the texture's list
    char sname[32];
} vkn_tex_name;
enum { STORE_BUCKETS = 256 };
static vkn_tex_name *g_ncookie[COOKIE_BUCKETS];
static vkn_tex *g_store[STORE_BUCKETS];
static pthread_mutex_t g_share_mu = PTHREAD_MUTEX_INITIALIZER;
static __thread const vkn_tex_share *t_share;   // vkn_tex_create_shared: the storage of this creation, NULL = not shared
static uint32_t g_al_logged, g_kept_logged;

static int tables_init_locked(void) {
    if (g_init) return g_init > 0 ? 0 : -1;
    g_init = -1;
    if (vkn_tab_init(&g_handles, TEX_HANDLES, sizeof(vkn_tex *))) return -1;
    if (vkn_tab_init(&g_samp, SAMP_HANDLES, sizeof(samp_rec))) return -1;
    g_slot = (vkn_tex **)calloc(TEX_SLOTS, sizeof *g_slot); g_free_slot = (uint32_t *)malloc(sizeof(uint32_t) * TEX_SLOTS);
    if (!g_slot || !g_free_slot) return -1;
    g_init = 1;
    return 0;
}
static uint32_t cookie_bucket(uint64_t c) { return (uint32_t)((c >> 4) ^ (c >> 20)) & (COOKIE_BUCKETS - 1); }
static void cookie_unlink_locked(vkn_tex *t) {
    vkn_tex **p = &g_cookie[cookie_bucket(t->cookie)];
    while (*p && *p != t) p = &(*p)->hnext;
    if (*p) *p = t->hnext;
    t->hnext = NULL;
}
static vkn_tex *cookie_find_locked(uint64_t c) {
    for (vkn_tex *t = g_cookie[cookie_bucket(c)]; t; t = t->hnext) if (t->cookie == c && !t->dead) return t;
    return NULL;
}
static uint32_t *id_cell_locked(uint32_t id, int make) {
    if (id >= 0x100000) return NULL;
    uint32_t **pg = &g_id_page[id / ID_PAGE];
    if (!*pg) { if (!make) return NULL; *pg = (uint32_t *)calloc(ID_PAGE, sizeof(uint32_t)); if (!*pg) return NULL; }
    return &(*pg)[id % ID_PAGE];
}
// The pool ids of one engine texture (t's own or an attached name's) -> t's slot and views.
static void ids_set_of_locked(vkn_tex *t, uint32_t pool_id, const uint32_t *lids) {
    uint32_t *c = pool_id != ~0u ? id_cell_locked(pool_id, 1) : NULL;
    if (c) *c = (t->slot + 1) << 3;
    for (int i = 0; i < VKN_TEX_LAYER_IDS; i++)
        if (lids[i] != ~0u && t->layer_view[i] && (c = id_cell_locked(lids[i], 1))) *c = (t->slot + 1) << 3 | (uint32_t)(i + 1);
}
// Only the cells that still name t (an id may have gone to a newer texture).
static void ids_clear_of_locked(const vkn_tex *t, uint32_t pool_id, const uint32_t *lids) {
    uint32_t *c = pool_id != ~0u ? id_cell_locked(pool_id, 0) : NULL;
    if (c && *c >> 3 == t->slot + 1) *c = 0;
    for (int i = 0; i < VKN_TEX_LAYER_IDS; i++)
        if (lids[i] != ~0u && (c = id_cell_locked(lids[i], 0)) && *c >> 3 == t->slot + 1) *c = 0;
}
static void ids_set_locked(vkn_tex *t) { ids_set_of_locked(t, t->pool_id, t->layer_ids); }
static void ids_clear_locked(vkn_tex *t) { ids_clear_of_locked(t, t->pool_id, t->layer_ids); }
// Unlinks t from its handle (if it is the one linked there).
static void handle_unlink_locked(vkn_tex *t) {
    if (!t->linked) return;
    vkn_tex **r = (vkn_tex **)vkn_tab_get(&g_handles, t->handle);
    if (r && *r == t) { *r = NULL; vkn_tab_kill(&g_handles, t->handle); }
    t->linked = 0;
}
// A newer link takes the handle index of `handle`: the name of x (its own or an attached one) that held it loses its link; x stays
// known by its cookie.
static void index_lost_locked(vkn_tex *x, uint64_t handle) {
    if (x->linked && vkn_h_idx(x->handle) == vkn_h_idx(handle)) x->linked = 0;
    for (vkn_tex_name *n = x->names; n; n = n->tnext) if (n->linked && vkn_h_idx(n->handle) == vkn_h_idx(handle)) n->linked = 0;
}
static void handle_link_locked(vkn_tex *t, uint64_t handle) {
    int alive = 0;
    vkn_tex **r = (vkn_tex **)vkn_tab_put(&g_handles, handle, &alive);
    if (!r) return;
    if (alive && *r) index_lost_locked(*r, handle);   // the previous texture of this index stays known by its cookie
    *r = t; t->handle = handle; t->linked = 1;
}

// ---- shared render-target storage: names attached to another texture's image ----
static uint32_t store_bucket(uint64_t blk, uint64_t off) {
    const uint64_t k = blk ^ (off * 0x9e3779b97f4a7c15ull);
    return (uint32_t)(k >> 7 ^ k >> 29) & (STORE_BUCKETS - 1);
}
static void store_unlink_locked(vkn_tex *t) {
    vkn_tex **p = &g_store[store_bucket(t->st_block, t->st_off)];
    while (*p && *p != t) p = &(*p)->snext;
    if (*p) *p = t->snext;
    t->snext = NULL; t->shared = 0;
}
static vkn_tex_name *name_find_locked(uint64_t c) {
    for (vkn_tex_name *n = g_ncookie[cookie_bucket(c)]; n; n = n->cnext) if (n->cookie == c) return n;
    return NULL;
}
static void name_cookie_unlink_locked(vkn_tex_name *n) {
    vkn_tex_name **p = &g_ncookie[cookie_bucket(n->cookie)];
    while (*p && *p != n) p = &(*p)->cnext;
    if (*p) *p = n->cnext;
    n->cnext = NULL;
}
// n leaves every table and its texture's list (the image stays: it is the texture's).
static void name_drop_locked(vkn_tex_name *n) {
    vkn_tex *t = n->t;
    if (n->linked) {
        vkn_tex **r = (vkn_tex **)vkn_tab_get(&g_handles, n->handle);
        if (r && *r == t) { *r = NULL; vkn_tab_kill(&g_handles, n->handle); }
        n->linked = 0;
    }
    name_cookie_unlink_locked(n);
    if (t->slot < TEX_SLOTS) ids_clear_of_locked(t, n->pool_id, n->layer_ids);
    for (vkn_tex_name **p = &t->names; *p; p = &(*p)->tnext) if (*p == n) { *p = n->tnext; break; }
    t->nnames--; g_st.sh_aliases--; g_st.sh_detached++;
    free(n);
}
// t's own engine texture is gone while names stay attached: the first attached name becomes the object's own (handle, cookie, pool
// ids, name); the image, its layout and its pass state stay as they are.
static void handover_locked(vkn_tex *t) {
    vkn_tex_name *n = t->names;
    handle_unlink_locked(t); cookie_unlink_locked(t);
    if (t->slot < TEX_SLOTS) ids_clear_locked(t);
    t->names = n->tnext; t->nnames--; g_st.sh_aliases--;
    name_cookie_unlink_locked(n);
    t->handle = n->handle; t->linked = n->linked; t->cookie = n->cookie; t->pool_id = n->pool_id; t->sampler_id = n->sampler_id;
    memcpy(t->layer_ids, n->layer_ids, sizeof t->layer_ids); memcpy(t->sname, n->sname, sizeof t->sname);
    const uint32_t b = cookie_bucket(t->cookie); t->hnext = g_cookie[b]; g_cookie[b] = t;
    free(n);
    g_st.sh_handover++;
}

static void tex_free(void *ctx) {
    vkn_tex *t = (vkn_tex *)ctx; const vkn_dev *d = vkn_device();
    if (d) {
        if (t->view) vkDestroyImageView(d->dev, t->view, NULL);
        if (t->att) vkDestroyImageView(d->dev, t->att, NULL);
        for (int i = 0; i < VKN_TEX_LAYER_IDS; i++) if (t->layer_view[i]) vkDestroyImageView(d->dev, t->layer_view[i], NULL);
        if (t->att_views) for (uint32_t i = 0, n = t->mips * t->layers; i < n; i++) if (t->att_views[i]) vkDestroyImageView(d->dev, t->att_views[i], NULL);
        if (t->image) vkDestroyImage(d->dev, t->image, NULL);
        if (t->mem) vkFreeMemory(d->dev, t->mem, NULL);
    }
    free(t->att_views); free(t);
}
// How a BC texture is stored (VKN_BC_*), -1 = not a BC format.
static int bc_kind(const vkn_tex *t) {
    if (!fmt_is_bc(t->nvn_fmt)) return -1;
    if (t->bcgpu == VKN_BCG_ASTC) return VKN_BC_GPU_ASTC;
    if (t->bcgpu == VKN_BCG_RGBA8) return VKN_BC_GPU_RGBA8;
    return t->astc ? VKN_BC_PACK : t->decoded ? VKN_BC_DECODED : VKN_BC_NATIVE;
}
static void unref_locked(vkn_tex *t) { if (t->refs && !--t->refs) vkn_retire(tex_free, t); }
static void unref(vkn_tex *t) { pthread_mutex_lock(&g_mu); unref_locked(t); pthread_mutex_unlock(&g_mu); }
// The engine is done with t: out of every table, its engine reference dropped.
static void kill_locked(vkn_tex *t) {
    if (t->dead) return;
    while (t->names) name_drop_locked(t->names);   // (callers that mean one name only use drop_cookie_locked / vkn_tex_destroy_handle)
    if (t->shared) store_unlink_locked(t);
    t->dead = 1;
    handle_unlink_locked(t); cookie_unlink_locked(t); ids_clear_locked(t);
    // vkn_state.h: framebuffers keyed by a view must leave the cache before the view dies (a later view may get the same handle value);
    // retired now, they are destroyed before (or with) this texture, whose retirement is queued after them. (g_rp_mu after g_mu: the
    // state piece never calls into this one.)
    vkn_fb_forget_view(t->view); vkn_fb_forget_view(t->att);
    for (int i = 0; i < VKN_TEX_LAYER_IDS; i++) vkn_fb_forget_view(t->layer_view[i]);
    if (t->att_views) for (uint32_t i = 0, n = t->mips * t->layers; i < n; i++) vkn_fb_forget_view(t->att_views[i]);
    if (t->slot < TEX_SLOTS && g_slot[t->slot] == t) { g_slot[t->slot] = NULL; g_free_slot[g_nfree++] = t->slot; }
    if (t->cls < VKN_TC_N) { g_st.alive[t->cls]--; g_st.bytes[t->cls] -= t->bytes; }
    const int k = bc_kind(t); if (k >= 0) { g_st.bc_alive[k]--; g_st.bc_bytes[k] -= t->bytes; }
    g_st.destroyed++;
    unref_locked(t);
}
// The engine is done with its texture object `c`: that one name leaves; the image dies with its last name. 1 = known.
static int drop_cookie_locked(uint64_t c) {
    vkn_tex *t = cookie_find_locked(c);
    if (t) { if (t->names) handover_locked(t); else kill_locked(t); return 1; }
    vkn_tex_name *n = name_find_locked(c);
    if (n) { name_drop_locked(n); return 1; }
    return 0;
}
// vkn_tex_create_shared: why the live image c on the same storage cannot be this creation's image (VKN_AL_*), -1 = it can. conv:
// decoded | widened << 1 | ASTC << 2 | GPU route << 3 of this creation.
static int alias_mismatch(const vkn_tex *c, const VkImageCreateInfo *ci, uint32_t nvn, uint32_t tg, VkComponentMapping sw, uint32_t conv, int has_data,
                          const vkn_tex_desc *d) {
    if (c->w != ci->extent.width || c->h != ci->extent.height || c->depth != ci->extent.depth) return VKN_AL_SIZE;
    if (c->format != ci->format || c->nvn_fmt != nvn || (uint32_t)(c->decoded | c->widened << 1 | (c->astc != 0) << 2 | (c->bcgpu != 0) << 3) != conv) return VKN_AL_FORMAT;
    if (c->mips != ci->mipLevels || c->layers != ci->arrayLayers || c->samples != (uint32_t)ci->samples || c->nvn_target != tg || c->cflags != ci->flags) return VKN_AL_SHAPE;
    if (c->usage != ci->usage || memcmp(&c->swz, &sw, sizeof sw)) return VKN_AL_USAGE;
    if (has_data) return VKN_AL_DATA;   // (initial data would have to land in the other texture's image: never expected for targets)
    if ((c->usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) && tg != 2)
        for (uint32_t i = 0; i < VKN_TEX_LAYER_IDS && i < ci->arrayLayers; i++) if (d->layer_ids[i] != ~0u && !c->layer_view[i]) return VKN_AL_VIEWS;
    return -1;
}
static void sname_set(char out[32], const char *s) { snprintf(out, 32, "%s", s && *s ? s : "?"); }
// Attaches the engine texture of this creation (handle, d's cookie and pool ids) to o's image. 0 = attached; -1 = no memory.
static int attach_locked(vkn_tex *o, uint64_t handle, const vkn_tex_desc *d, const vkn_tex_share *sh) {
    vkn_tex_name *n = (vkn_tex_name *)calloc(1, sizeof *n);
    if (!n) return -1;
    n->t = o; n->cookie = d->cookie; n->pool_id = d->pool_id; n->sampler_id = d->sampler_id; n->handle = handle;
    memcpy(n->layer_ids, d->layer_ids, sizeof n->layer_ids); sname_set(n->sname, sh->name);
    int alive = 0;
    vkn_tex **r = (vkn_tex **)vkn_tab_put(&g_handles, handle, &alive);
    if (r) { if (alive && *r) index_lost_locked(*r, handle); *r = o; n->linked = 1; }
    const uint32_t b = cookie_bucket(n->cookie); n->cnext = g_ncookie[b]; g_ncookie[b] = n;
    n->tnext = o->names; o->names = n; o->nnames++;
    if (o->slot < TEX_SLOTS) ids_set_of_locked(o, n->pool_id, n->layer_ids);
    g_st.sh_aliases++; g_st.sh_attached++;
    return 0;
}

// ---- uploads ----
// ns: queued vkn_tex_update (0 = other); job: stage 4 GPU BC route (vkn_bcgpu.h: its own staging; then n = 0, no blk)
typedef struct up_ctx { vkn_tex *t; vkn_block blk; int have_blk; uint32_t n; uint64_t ns; vkn_bcg_job *job; VkBufferImageCopy c[]; } up_ctx;
// (vkn_frame.c / vkn_pass.c; weak: this file also links without them, as in the stage-2 host test)
extern VkCommandBuffer vkn_cb(void) __attribute__((weak));
extern void vkn_pass_layout(vkn_tex *t, VkImageLayout to) __attribute__((weak));
static void barrier(VkCommandBuffer cb, vkn_tex *t, VkImageLayout from, VkImageLayout to) {
    const VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = t->image, .subresourceRange = {t->aspect, 0, t->mips, 0, t->layers}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void up_run(VkCommandBuffer cb, void *ctx) {
    up_ctx *u = (up_ctx *)ctx; vkn_tex *t = u->t;
    if (!t->dead) {   // (read without the lock: dead only ever goes 0 -> 1, and a dead texture's image lives until its last reference)
        // a queued update whose texture has clears vkn_pass.c holds back that the engine issued BEFORE this update: they run first, else
        // they would run later (next pass's CLEAR, or before a sample) and wipe the data (only in the recording vkn_pass records into)
        if (u->ns && (t->pass_nclr || t->pass_dclr) && t->pass_clr_ns <= u->ns && vkn_cb && vkn_pass_layout && vkn_cb() == cb)
            vkn_pass_layout(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        barrier(cb, t, t->layout, u->n || u->job ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (u->job) {   // stage 4: per batch staging -> scratch, GPU decode (-> ASTC encode), scratch -> image (vkn_bcgpu.c)
            vkn_bcgpu_job_record(cb, u->job, t->image);
            barrier(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        } else if (u->n) {
            vkCmdCopyBufferToImage(cb, u->blk.buf, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, u->n, u->c);
            barrier(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        __atomic_store_n(&t->shc_up, __atomic_add_fetch(&g_up_serial, 1, __ATOMIC_RELAXED), __ATOMIC_RELEASE);   // [stage 5 A3] new texels
    }
    if (u->job) vkn_bcgpu_job_free(u->job);   // (its staging retired: the GPU may still read it)
    if (u->have_blk) vkn_retire_block(&u->blk);
    unref(t); free(u);
}

// ---- data preparation: every region into one staging block ----
static uint64_t lcm16(uint32_t b) { uint64_t a = 16; while (a % (b ? b : 1)) a += 16; return a; }
static uint32_t mipdim(uint32_t s, uint32_t l) { s >>= l; return s ? s : 1; }
typedef struct prep { const vkn_tex_desc *d; vkn_tex *t; uint32_t nvn; int bw, bh, bpb; int mode; uint32_t cs, one, dbpb; uint64_t align; } prep;   // mode 0 copy, 1 BC decode, 2 widen
// Destination bytes of a (w, h, d) region.
static uint64_t dst_bytes(const prep *p, uint32_t w, uint32_t h, uint32_t d) {
    if (p->mode == 0) return (uint64_t)((w + p->bw - 1) / p->bw) * ((h + p->bh - 1) / p->bh) * p->bpb * d;
    return (uint64_t)w * h * d * p->dbpb;
}
// Converts one region of tight block rows (src) into dst (tight in the destination format).
static int convert(const prep *p, const uint8_t *src, uint32_t w, uint32_t h, uint32_t d, uint8_t *dst) {
    const uint64_t srow = (uint64_t)((w + p->bw - 1) / p->bw) * p->bpb, srows = (h + p->bh - 1) / p->bh;
    if (p->mode == 0) { memcpy(dst, src, (size_t)(srow * srows * d)); return 0; }
    for (uint32_t z = 0; z < d; z++) {
        const uint8_t *s = src + z * srow * srows; uint8_t *o = dst + (uint64_t)z * w * h * p->dbpb;
        if (p->mode == 1) { if (nvn_bc_decode(p->nvn, s, (size_t)srow, w, h, o, (size_t)w * p->dbpb)) return -1; continue; }
        for (uint64_t i = 0, n = (uint64_t)w * h; i < n; i++) {   // widen RGB -> RGBA with alpha = one
            memcpy(o + i * 4 * p->cs, s + i * 3 * p->cs, 3 * p->cs);
            if (p->cs == 1) o[i * 4 + 3] = (uint8_t)p->one; else if (p->cs == 2) { const uint16_t v = (uint16_t)p->one; memcpy(o + i * 8 + 6, &v, 2); } else memcpy(o + i * 16 + 12, &p->one, 4);
        }
    }
    return 0;
}

// A region inside the image (others are skipped, in both passes alike).
static int reg_ok(const vkn_tex_region *r, uint32_t w, uint32_t h, uint32_t dep, uint32_t mips, uint32_t layers) {
    if (!r->src || r->level >= mips || r->layer >= layers || !r->w || !r->h) return 0;
    const uint32_t d = r->d ? r->d : 1;
    return r->w <= mipdim(w, r->level) && r->h <= mipdim(h, r->level) && r->z + d <= mipdim(dep, r->level);
}

// ---- creation ----
static VkImageView make_view(VkDevice dev, vkn_tex *t, VkImageViewType type, VkImageAspectFlags asp, uint32_t lv0, uint32_t nlv, uint32_t l0, uint32_t nl, VkComponentMapping m) {
    const VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->image, .viewType = type, .format = t->format, .components = m,
                                      .subresourceRange = {asp, lv0, nlv, l0, nl}};
    VkImageView v = VK_NULL_HANDLE;
    return vkCreateImageView(dev, &vi, NULL, &v) == VK_SUCCESS ? v : VK_NULL_HANDLE;
}

// ---- stage 4: the setup ASTC pack (vkn_astc.h) on a device without BC ----
// The pack's entry for this creation: 1 = hit (*pe), else 0 and *dw = why the texture is decoded (VKN_DW_*). The key is the one of the
// translator (xbg_game.cpp astccache::lookup): the storage bytes of the packaged layout (nvn_tex_layout of the builder's description,
// the bytes the engine's image holds at its storage pointer), the NVN format the engine gave (sRGB twin included), w, h, levels as the
// engine gave them (before the level clamp of vkn_tex_create, like the builder's). Linear data is never keyed: the tool keys the XTX
// storage, which the engine hands over only as packaged storage.
static int pack_try(const vkn_tex_desc *d, const vkn_tex_src *src, uint32_t nvn, vkn_astc_entry *pe, int *dw) {
    if (nvn < 0x41 || nvn > 0x48) { *dw = VKN_DW_FMT; return 0; }
    if (!src || src->kind != VKN_SRC_PACKAGED || !src->storage) { *dw = VKN_DW_SRC; return 0; }
    if (vkn_astc_open() != VKN_ASTC_READY) { *dw = VKN_DW_OFF; return 0; }
    const uint32_t levels = d->mips ? d->mips : 1;
    nvn_texdesc nd; nvn_texlayout L; memset(&nd, 0, sizeof nd); memset(&L, 0, sizeof L);
    nd.target = (int)d->nvn_target; nd.format = (int)nvn; nd.width = d->w; nd.height = d->h; nd.depth = src->nvn_depth ? src->nvn_depth : 1; nd.levels = levels;
    nd.pkg_block_h = src->pkg_block_h;
    nvn_tex_layout(&nd, &L);
    if (!vkn_astc_cacheable(nvn, d->nvn_target, L.layers, d->samples, L.size, d->w, d->h) || L.size > src->storage_bytes) { *dw = VKN_DW_SHAPE; return 0; }
    const char *why = NULL;
    if (vkn_astc_lookup(src->storage, L.size, nvn, levels, d->w, d->h, pe, &why)) return 1;
    *dw = !why ? VKN_DW_OFF : !strcmp(why, "shape") ? VKN_DW_MISS_SHAPE : !strcmp(why, "key") ? VKN_DW_MISS_KEY : !strcmp(why, "block") ? VKN_DW_BLOCK : VKN_DW_OFF;
    return 0;
}
// vkn_tex_swap_reduced: make this BC texture (no data) an ASTC image of this block size / a decoded one, written by this GPU route (bcgpu)
static __thread uint8_t t_force_astc, t_force_dec, t_force_bcg;

static __thread const uint8_t *t_swz;   // vkn_tex_create_swz (UI piece): the view swizzle of this creation, NULL = the format's own
vkn_tex *vkn_tex_create(uint64_t handle, const vkn_tex_desc *d, const vkn_tex_src *src, const char **why) {
    const char *dummy; if (!why) why = &dummy;
    *why = "no device";
    const vkn_dev *dv = vkn_device(); if (!dv) goto fail0;
    pthread_mutex_lock(&g_mu); const int ti = tables_init_locked(); pthread_mutex_unlock(&g_mu);
    if (ti) { *why = "no memory for the tables"; goto fail0; }
    const uint64_t t0 = vkn_now_ns();

    // format: the engine's NVN number, device fallbacks
    uint32_t nvn = d->nvn_fmt; VkFormat fmt;
    if (nvn) fmt = vkn_tex_nvn_format(nvn); else fmt = vkn_tex_engine_format(d->efmt, d->srgb, &nvn);
    int x = 0; nvn_vk(nvn, &x);
    if (fmt == VK_FORMAT_UNDEFINED || !nvn) { *why = "format without a Vulkan equivalent"; goto fail0; }
    prep p = {.d = d, .nvn = nvn};
    p.bpb = nvn_fmt_bpb((int)nvn, &p.bw, &p.bh);
    if (!p.bpb) { *why = "format without a block size"; goto fail0; }
    const int rt = d->cls == VKN_TC_COLOR || d->cls == VKN_TC_DEPTH || d->cls == VKN_TC_WINDOW;
    VkFormatFeatureFlags ff = fmt_feat(fmt);
    const VkFormatFeatureFlags want_s = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    uint8_t decoded = 0, widened = 0, astc = 0, bcg = 0, frompack = 0, nopack = 0; int dw = VKN_DW_OFF; uint64_t pack_ns = 0;
    vkn_astc_entry pe; memset(&pe, 0, sizeof pe);
    vkn_bcg_job *job = NULL;
    if (fmt_is_bc(nvn) && ((ff & want_s) != want_s || sim_no_bc())) {   // no native BC sampling (or the Mali / PowerVR model)
        // stage 4, in this order: (1) the ASTC pack's blocks when it has this texture (read below, after the image); (2) / (3) the GPU
        // route (vkn_bcgpu.h: decode + ASTC 4x4 encode / decode to RGBA8, recorded with the upload); (4) decoded once here on the CPU
        const uint64_t a0 = vkn_now_ns();
        if (t_force_astc) { astc = t_force_astc; bcg = t_force_bcg; }
        else if (!t_force_dec && pack_try(d, src, nvn, &pe, &dw)) { astc = pe.bw; frompack = 1; }
        pack_ns = vkn_now_ns() - a0;
        if (t_force_dec) bcg = t_force_bcg;
        else if (!astc) {
            nopack = 1;
            const uint32_t tg0 = d->nvn_target, l0 = d->layers ? d->layers : 1;
            const uint32_t lay = tg0 == 8 ? 6 : tg0 == 9 ? 6 * l0 : (tg0 == 3 || tg0 == 4 || tg0 == 6) ? l0 : 1;
            int gw = 0;
            bcg = (uint8_t)vkn_bcgpu_route(nvn, tg0, d->w, d->h, d->mips ? d->mips : 1, lay, d->samples ? d->samples : 1, d->storage, &gw);
            if (bcg == VKN_BCG_ASTC) astc = 4;
        }
        if (astc) { fmt = vkn_astc_vkformat(astc, astc, vkn_astc_srgb(nvn)); ff = fmt_feat(fmt); }
        else {
            int bpp = 0; uint32_t pf = 0;
            if (!nvn_bc_unpacked(nvn, &pf, &bpp) || (fmt = bc_decoded(nvn)) == VK_FORMAT_UNDEFINED) { *why = "BC format the CPU decoder does not know"; goto fail0; }
            p.mode = 1; p.dbpb = (uint32_t)bpp; decoded = 1; ff = fmt_feat(fmt);
        }
    } else if ((ff & want_s) != want_s && !rt) {
        VkFormat to;
        if (widen_of(nvn, &to, &p.cs, &p.one)) { p.mode = 2; p.dbpb = 4 * p.cs; fmt = to; widened = 1; ff = fmt_feat(fmt); }
    }
    if (d->cls == VKN_TC_DEPTH && !(ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {   // D24 stand-ins (Mali / PowerVR class drivers)
        if (fmt == VK_FORMAT_X8_D24_UNORM_PACK32) fmt = VK_FORMAT_D32_SFLOAT; else if (fmt == VK_FORMAT_D24_UNORM_S8_UINT) fmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
        ff = fmt_feat(fmt);
        if (!(ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) { *why = "depth format not renderable"; goto fail0; }
    }
    if ((d->cls == VKN_TC_COLOR || d->cls == VKN_TC_WINDOW) && !(ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) { *why = "colour format not renderable"; goto fail0; }
    if (!rt && (ff & want_s) != want_s) { *why = "format not sampled by the device"; goto fail0; }
    p.align = lcm16(p.mode ? p.dbpb : (uint32_t)p.bpb);   // bufferOffset: a multiple of 4 and of the texel block (12-byte texels -> 48)

    // shape
    const uint32_t tg = d->nvn_target;
    const int cube = tg == 8 || tg == 9, d3 = tg == 2, d1 = tg == 0 || tg == 3;
    uint32_t layers = tg == 8 ? 6 : tg == 9 ? 6 * (d->layers ? d->layers : 1) : (tg == 3 || tg == 4 || tg == 6) ? (d->layers ? d->layers : 1) : 1;
    uint32_t w = d->w ? d->w : 1, h = d1 ? 1 : (d->h ? d->h : 1), dep = d3 ? (d->depth ? d->depth : 1) : 1, mips = d->mips ? d->mips : 1, maxm = 1;
    for (uint32_t m = w > h ? w : h, mm = m > dep ? m : dep; mm > 1; mm >>= 1) maxm++;
    if (mips > maxm) mips = maxm;
    VkSampleCountFlagBits samples = d->samples == 2 ? VK_SAMPLE_COUNT_2_BIT : d->samples == 4 ? VK_SAMPLE_COUNT_4_BIT : d->samples == 8 ? VK_SAMPLE_COUNT_8_BIT : VK_SAMPLE_COUNT_1_BIT;
    if (samples != VK_SAMPLE_COUNT_1_BIT) mips = 1;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (d->cls == VKN_TC_COLOR || d->cls == VKN_TC_WINDOW) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (d->cls == VKN_TC_DEPTH) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (d->storage && (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && samples == VK_SAMPLE_COUNT_1_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    const VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube && w == h ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
        .imageType = d3 ? VK_IMAGE_TYPE_3D : d1 ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D, .format = fmt, .extent = {w, h, dep}, .mipLevels = mips, .arrayLayers = layers,
        .samples = samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    // view swizzle (sampled views)
    VkComponentMapping sw = fmt_swizzle(nvn, x && !decoded);
    if (t_swz) {   // vkn_tex_create_swz: the caller's NVN swizzle (0 zero, 1 one, 2 R, 3 G, 4 B, 5 A)
        static const VkComponentSwizzle k[6] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                                                VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
        VkComponentSwizzle *c[4] = {&sw.r, &sw.g, &sw.b, &sw.a};
        for (int i = 0; i < 4; i++) if (t_swz[i] < 6) *c[i] = k[t_swz[i]];
    }
    const int has_data = src && ((src->kind == VKN_SRC_REGIONS && src->nreg) || (src->kind == VKN_SRC_PACKAGED && src->storage));

    // shared render-target storage (vkn_tex_create_shared): a live image of exactly this shape on the same storage IS this texture on
    // the Switch (one piece of memory): the engine texture is attached to it, no image is made
    int kept = -1; char kept_line[320]; kept_line[0] = 0;
    if (t_share) {
        char line[320]; line[0] = 0;
        pthread_mutex_lock(&g_mu);
        drop_cookie_locked(d->cookie);   // (the engine object reused without a destroy event we saw: that texture is gone)
        vkn_tex *o = NULL;
        const uint32_t conv = (uint32_t)(decoded | widened << 1 | (astc != 0) << 2 | (bcg != 0) << 3);
        for (vkn_tex *c = g_store[store_bucket(t_share->block, t_share->offset)]; c; c = c->snext) {
            if (c->st_block != t_share->block || c->st_off != t_share->offset) continue;
            const int m = alias_mismatch(c, &ci, nvn, tg, sw, conv, has_data, d);
            if (m < 0) { o = c; break; }
            if (m > kept) {   // the reason against the most similar image there (the checks run size, format, shape, usage, data, views)
                kept = m;
                snprintf(kept_line, sizeof kept_line, "kept separate from %#llx (%s, %ux%u format %d NVN %#x, %u levels, %u layers, %u samples, usage %#x)",
                         (unsigned long long)c->handle, c->sname, c->w, c->h, (int)c->format, c->nvn_fmt, c->mips, c->layers, c->samples, (unsigned)c->usage);
            }
        }
        if (o && !attach_locked(o, handle, d, t_share)) {
            if (g_al_logged < 16) {
                g_al_logged++;
                snprintf(line, sizeof line, "tex shared: %#llx (%s) = %#llx (%s): %ux%u, format %d (NVN %#x), %u levels, %u layers, %u samples; storage %#llx + %#llx (%llu bytes, key %s): one image, %u names",
                         (unsigned long long)handle, t_share->name ? t_share->name : "?", (unsigned long long)o->handle, o->sname, o->w, o->h, (int)o->format,
                         o->nvn_fmt, o->mips, o->layers, o->samples, (unsigned long long)t_share->block, (unsigned long long)t_share->offset,
                         (unsigned long long)t_share->bytes, t_share->key ? t_share->key : "?", o->nnames + 1);
            }
            pthread_mutex_unlock(&g_mu);
            if (line[0]) vkn_logf("%s", line);
            *why = NULL;
            return o;
        }
        pthread_mutex_unlock(&g_mu);   // (no image to attach to, or no memory for the name record: an image of its own)
    }

    vkn_tex *t = (vkn_tex *)calloc(1, sizeof *t);
    if (!t) { *why = "no memory"; goto fail0; }
    t->usage = usage; t->cflags = ci.flags; t->swz = sw;
    t->cookie = d->cookie; t->format = fmt; t->aspect = fmt_aspect(fmt); t->w = w; t->h = h; t->depth = dep; t->mips = mips; t->layers = layers;
    t->samples = (uint32_t)samples; t->efmt = d->efmt; t->nvn_fmt = nvn; t->nvn_target = tg; t->cls = d->cls < VKN_TC_N ? d->cls : VKN_TC_SAMPLED; t->cube = (uint8_t)cube;
    t->decoded = decoded; t->widened = widened; t->astc = astc; t->bcgpu = bcg; t->pool_id = d->pool_id; t->sampler_id = d->sampler_id; t->layout = VK_IMAGE_LAYOUT_UNDEFINED; t->refs = 1;
    memcpy(t->layer_ids, d->layer_ids, sizeof t->layer_ids);
    p.t = t;
    if (vkCreateImage(dv->dev, &ci, NULL, &t->image) != VK_SUCCESS) { t->image = VK_NULL_HANDLE; *why = "vkCreateImage failed"; goto fail1; }
    if (vkn_image_mem(t->image, &t->mem, &t->bytes)) { *why = "no device memory"; goto fail1; }

    // views
    const VkImageAspectFlags sasp = t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : t->aspect;   // a depth-stencil texture samples depth
    const VkImageViewType vt = d3 ? VK_IMAGE_VIEW_TYPE_3D : tg == 0 ? VK_IMAGE_VIEW_TYPE_1D : tg == 3 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : (tg == 4 || tg == 6) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                             : tg == 8 && (ci.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ? VK_IMAGE_VIEW_TYPE_CUBE
                             : tg == 9 && (ci.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    if (usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) {
        if (!(t->view = make_view(dv->dev, t, vt, sasp, 0, mips, 0, layers, sw))) { *why = "vkCreateImageView failed"; goto fail1; }
        if (!d3)
            for (uint32_t i = 0; i < VKN_TEX_LAYER_IDS && i < layers; i++)
                if (d->layer_ids[i] != ~0u && !(t->layer_view[i] = make_view(dv->dev, t, d1 ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D, sasp, 0, mips, i, 1, sw))) { *why = "layer view failed"; goto fail1; }
    }
    if (rt && !d3 && !(t->att = make_view(dv->dev, t, VK_IMAGE_VIEW_TYPE_2D, t->aspect, 0, 1, 0, 1, (VkComponentMapping){0}))) { *why = "attachment view failed"; goto fail1; }

    // data -> one staging block
    up_ctx *u = NULL; uint64_t total = 0; uint32_t nc = 0;
    if (frompack) {
        // stage 4: the entry's blocks (every level the pack holds; level 0 first, tight 16-byte blocks: every level offset is a multiple
        // of 16, the texel block size) read into one staging block, one copy per level. The key includes the level count, so the entry
        // has exactly the engine's levels; only the level clamp above could leave fewer in the image (then only those are copied).
        const uint32_t nl = mips < pe.levels ? mips : pe.levels;
        u = (up_ctx *)calloc(1, sizeof *u + sizeof(VkBufferImageCopy) * nl);
        if (!u) { *why = "no memory"; goto fail1; }
        if (vkn_block_alloc(VKN_ARENA_STAGING, pe.data_bytes, 16, &u->blk)) { free(u); u = NULL; *why = "no staging memory"; goto fail1; }
        u->have_blk = 1; total = pe.data_bytes;
        const uint64_t r0 = vkn_now_ns();
        vkn_astc_read(&pe, (uint8_t *)u->blk.cpu);   // (a short read leaves transparent black: counted and logged there)
        pack_ns += vkn_now_ns() - r0;
        uint64_t off = 0;
        for (uint32_t l = 0; l < nl; l++) {
            VkBufferImageCopy *c = &u->c[nc++];
            c->bufferOffset = u->blk.off + off; c->bufferRowLength = 0; c->bufferImageHeight = 0;
            c->imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
            c->imageOffset = (VkOffset3D){0, 0, 0}; c->imageExtent = (VkExtent3D){mipdim(w, l), mipdim(h, l), 1};
            off += vkn_astc_level_bytes(&pe, l);
        }
    } else if (bcg && has_data && samples == VK_SAMPLE_COUNT_1_BIT) {
        // stage 4 GPU route: the BC blocks of every (level, layer) as tight block rows into a job (vkn_bcgpu.c cuts it into batches and
        // stages it); recorded with this texture's upload. 2D-like targets only (the route refuses 3D / 1D).
        job = vkn_bcgpu_job_new(nvn, bcg);
        if (!job) { *why = "GPU BC job failed"; goto fail1; }
        uint8_t *tmp = NULL; uint64_t tmp_bytes = 0;
        nvn_texdesc nd = {0}; nvn_texlayout L = {0};
        if (src->kind == VKN_SRC_PACKAGED) {
            nd.target = (int)tg; nd.format = (int)nvn; nd.width = d->w; nd.height = d->h; nd.depth = src->nvn_depth ? src->nvn_depth : 1; nd.levels = mips;
            nd.pkg_block_h = src->pkg_block_h;
            nvn_tex_layout(&nd, &L);
            if (!L.size || L.size > src->storage_bytes) { *why = "packaged storage smaller than its layout"; goto fail3; }
        }
        const uint32_t maxc = src->kind == VKN_SRC_REGIONS ? src->nreg : mips * layers;
        for (uint32_t i = 0; i < maxc; i++) {
            uint32_t lw, lh, lv, ly; const uint8_t *s;
            if (src->kind == VKN_SRC_REGIONS) {
                const vkn_tex_region *r = &src->reg[i]; if (!reg_ok(r, w, h, dep, mips, layers) || (r->d ? r->d : 1) != 1) continue;
                lw = r->w; lh = r->h; lv = r->level; ly = r->layer; s = (const uint8_t *)r->src;
            } else {
                lv = i % mips; ly = i / mips; lw = mipdim(w, lv); lh = mipdim(h, lv);
                const uint64_t lin = (uint64_t)((lw + p.bw - 1) / p.bw) * p.bpb * ((lh + p.bh - 1) / p.bh);
                if (lin > tmp_bytes) { free(tmp); tmp = (uint8_t *)malloc((size_t)lin); tmp_bytes = tmp ? lin : 0; }
                if (!tmp) { *why = "no memory"; goto fail3; }
                if (nvn_deswizzle_gob(&nd, lv, ly, src->storage, tmp, (size_t)((lw + p.bw - 1) / p.bw) * p.bpb)) { *why = "deswizzle refused the layout"; goto fail3; }
                s = tmp;
            }
            if (vkn_bcgpu_job_add(job, lv, ly, 0, 0, lw, lh, s, (uint64_t)((lw + p.bw - 1) / p.bw) * p.bpb)) { *why = "GPU BC job: no memory"; goto fail3; }
        }
        free(tmp); tmp = NULL;
        if (vkn_bcgpu_job_batches(job) && vkn_bcgpu_job_stage(job, &total)) { *why = "no staging memory"; goto fail3; }
        if (src->kind == VKN_SRC_PACKAGED) __atomic_add_fetch(&g_st.deswizzled, 1, __ATOMIC_RELAXED);
        u = (up_ctx *)calloc(1, sizeof *u);
        if (!u) { *why = "no memory"; goto fail3; }
        if (!vkn_bcgpu_job_batches(job)) { vkn_bcgpu_job_free(job); job = NULL; }   // (no region inside the image: only the layout change)
        u->job = job; job = NULL; nc = 0;
        if (0) { fail3: free(tmp); vkn_bcgpu_job_free(job); goto fail1; }
    } else if (has_data && samples == VK_SAMPLE_COUNT_1_BIT) {
        nvn_texdesc nd = {0}; nvn_texlayout L = {0};
        uint32_t maxc = src->kind == VKN_SRC_REGIONS ? src->nreg : mips * (d3 ? 1 : layers);
        if (src->kind == VKN_SRC_PACKAGED) {
            nd.target = (int)tg; nd.format = (int)nvn; nd.width = d->w; nd.height = d->h; nd.depth = src->nvn_depth ? src->nvn_depth : 1; nd.levels = mips;
            nd.pkg_block_h = src->pkg_block_h;
            nvn_tex_layout(&nd, &L);
            if (!L.size || L.size > src->storage_bytes) { *why = "packaged storage smaller than its layout"; goto fail1; }
        }
        // sizes first (one block)
        for (uint32_t i = 0; i < maxc; i++) {
            uint32_t lw, lh, ld;
            if (src->kind == VKN_SRC_REGIONS) { const vkn_tex_region *r = &src->reg[i]; if (!reg_ok(r, w, h, dep, mips, layers)) continue; lw = r->w; lh = r->h; ld = r->d ? r->d : 1; }
            else { const uint32_t m = i % mips; lw = mipdim(w, m); lh = mipdim(h, m); ld = d3 ? mipdim(dep, m) : 1; }
            total = (total + p.align - 1) / p.align * p.align + dst_bytes(&p, lw, lh, ld);
        }
        u = (up_ctx *)calloc(1, sizeof *u + sizeof(VkBufferImageCopy) * maxc);
        if (!u) { *why = "no memory"; goto fail1; }
        // p.align is 48 for 3-, 6- and 12-byte texels: no power of two, which vkn_block_alloc refuses. Such a block is taken 16-aligned
        // with p.align bytes more, and its start moved up to the next multiple of p.align (skew): every bufferOffset stays a multiple.
        const int al_pow2 = !(p.align & (p.align - 1));
        if (total && vkn_block_alloc(VKN_ARENA_STAGING, total + (al_pow2 ? 0 : p.align), al_pow2 ? (uint32_t)p.align : 16, &u->blk)) { free(u); *why = "no staging memory"; goto fail1; }
        u->have_blk = total != 0;
        const uint64_t skew = u->have_blk ? (p.align - u->blk.off % p.align) % p.align : 0;
        uint8_t *const stage = u->have_blk ? (uint8_t *)u->blk.cpu + skew : NULL;
        uint8_t *tmp = NULL; uint64_t tmp_bytes = 0, off = 0;
        for (uint32_t i = 0; i < maxc && total; i++) {
            uint32_t lw, lh, ld, lv, ly, z = 0; const uint8_t *s;
            if (src->kind == VKN_SRC_REGIONS) {
                const vkn_tex_region *r = &src->reg[i]; if (!reg_ok(r, w, h, dep, mips, layers)) continue;
                lw = r->w; lh = r->h; ld = r->d ? r->d : 1; lv = r->level; ly = r->layer; z = r->z; s = (const uint8_t *)r->src;
            } else {
                lv = i % mips; ly = i / mips; lw = mipdim(w, lv); lh = mipdim(h, lv); ld = d3 ? mipdim(dep, lv) : 1;
                const uint64_t lin = (uint64_t)((lw + p.bw - 1) / p.bw) * p.bpb * ((lh + p.bh - 1) / p.bh) * ld;
                if (p.mode) { if (lin > tmp_bytes) { free(tmp); tmp = (uint8_t *)malloc((size_t)lin); tmp_bytes = tmp ? lin : 0; } if (!tmp) { *why = "no memory"; goto fail2; } }
                off = (off + p.align - 1) / p.align * p.align;
                uint8_t *to = p.mode ? tmp : stage + off;
                if (nvn_deswizzle_gob(&nd, lv, ly, src->storage, to, (size_t)((lw + p.bw - 1) / p.bw) * p.bpb)) { *why = "deswizzle refused the layout"; goto fail2; }
                s = to;
            }
            off = (off + p.align - 1) / p.align * p.align;
            if (src->kind == VKN_SRC_REGIONS || p.mode) {
                if (off + dst_bytes(&p, lw, lh, ld) > total) { *why = "staging overflow"; goto fail2; }
                if (convert(&p, s, lw, lh, ld, stage + off)) { *why = "BC decode failed"; goto fail2; }
            }
            VkBufferImageCopy *c = &u->c[nc++];
            c->bufferOffset = u->blk.off + skew + off; c->bufferRowLength = 0; c->bufferImageHeight = 0;
            c->imageSubresource = (VkImageSubresourceLayers){t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : t->aspect, lv, d3 ? 0 : ly, 1};
            c->imageOffset = (VkOffset3D){0, 0, d3 ? (int32_t)z : 0};
            c->imageExtent = (VkExtent3D){lw, d1 ? 1 : lh, ld};
            off += dst_bytes(&p, lw, lh, ld);
        }
        free(tmp);
        if (src->kind == VKN_SRC_PACKAGED) __atomic_add_fetch(&g_st.deswizzled, 1, __ATOMIC_RELAXED);
        if (0) { fail2: free(tmp); if (u->have_blk) vkn_block_free(&u->blk); free(u); goto fail1; }
    } else if (!rt) u = (up_ctx *)calloc(1, sizeof *u);   // a sampled texture without data: only the layout change (content undefined, as on the engine)
    if (u) { u->t = t; u->n = nc; }

    // tables
    pthread_mutex_lock(&g_mu);
    drop_cookie_locked(t->cookie);   // the engine object was reused without a destroy event we saw: the old texture is gone
    if (g_nfree) t->slot = g_free_slot[--g_nfree]; else if (g_slot_hi < TEX_SLOTS) t->slot = g_slot_hi++; else t->slot = ~0u;
    if (t->slot != ~0u) g_slot[t->slot] = t;
    handle_link_locked(t, handle);
    t->shc_up = __atomic_add_fetch(&g_up_serial, 1, __ATOMIC_RELAXED);   // [stage 5 A3]
    const uint32_t b = cookie_bucket(t->cookie); t->hnext = g_cookie[b]; g_cookie[b] = t;
    if (t->slot != ~0u) ids_set_locked(t);
    g_st.alive[t->cls]++; g_st.bytes[t->cls] += t->bytes; g_st.created++;
    { const int k = bc_kind(t); if (k >= 0) { g_st.bc_alive[k]++; g_st.bc_bytes[k] += t->bytes; g_st.bc_made[k]++; } }
    if (nopack) g_st.dec_why[dw]++;   // (why the pack did not take it: the GPU routes and the CPU decode alike)
    g_st.pack_ns += pack_ns;
    { const int k = bc_kind(t); if (k >= 0) { g_st.bc_ns[k] += vkn_now_ns() - t0; g_st.bc_upload[k] += total; } }
    if (u) { t->refs++; g_st.uploads += nc || u->job ? 1 : 0; g_st.upload_bytes += total; g_st.decoded += decoded && !bcg; g_st.widened += widened; g_st.upload_ns += vkn_now_ns() - t0; }
    char kline[480]; kline[0] = 0;
    if (t_share) {   // the first image on this storage, or one that differs from those there (kept: VKN_AL_*)
        t->shared = 1; t->st_block = t_share->block; t->st_off = t_share->offset; sname_set(t->sname, t_share->name);
        const uint32_t sb = store_bucket(t->st_block, t->st_off); t->snext = g_store[sb]; g_store[sb] = t;
        g_st.sh_made++;
        if (kept >= 0) {
            g_st.sh_kept[kept]++;
            static const char *const k_al[VKN_AL_N] = {"size", "format", "shape", "usage", "initial data", "layer views"};
            if (g_kept_logged < 16) {
                g_kept_logged++;
                snprintf(kline, sizeof kline, "tex shared: %#llx (%s, %ux%u format %d NVN %#x) on storage %#llx + %#llx (key %s) %s: differs in %s, own image",
                         (unsigned long long)handle, t->sname, t->w, t->h, (int)t->format, t->nvn_fmt, (unsigned long long)t->st_block, (unsigned long long)t->st_off,
                         t_share->key ? t_share->key : "?", kept_line, k_al[kept]);
            }
        }
    }
    pthread_mutex_unlock(&g_mu);
    if (kline[0]) vkn_logf("%s", kline);
    if (u) vkn_upload_push(up_run, u, total);
    *why = NULL;
    return t;
fail1:
    t->refs = 0; tex_free(t);
fail0:
    __atomic_add_fetch(&g_st.failed, 1, __ATOMIC_RELAXED);
    return NULL;
}

vkn_tex *vkn_tex_get(uint64_t handle) {
    pthread_mutex_lock(&g_mu);
    vkn_tex **r = g_init > 0 ? (vkn_tex **)vkn_tab_get(&g_handles, handle) : NULL; vkn_tex *t = r ? *r : NULL;
    pthread_mutex_unlock(&g_mu);
    return t;
}
vkn_tex *vkn_tex_by_cookie(uint64_t cookie) {
    pthread_mutex_lock(&g_mu);
    vkn_tex *t = g_init > 0 ? cookie_find_locked(cookie) : NULL;
    if (!t && g_init > 0) { const vkn_tex_name *n = name_find_locked(cookie); if (n) t = n->t; }   // attached to another texture's image
    pthread_mutex_unlock(&g_mu);
    return t;
}
VkImageView vkn_tex_view_by_id(uint32_t pool_id, vkn_tex **tex) {
    VkImageView v = VK_NULL_HANDLE; vkn_tex *t = NULL;
    pthread_mutex_lock(&g_mu);
    const uint32_t *c = g_init > 0 ? id_cell_locked(pool_id, 0) : NULL;
    if (c && *c) {
        const uint32_t s = (*c >> 3) - 1, vi = *c & 7;
        t = s < TEX_SLOTS ? g_slot[s] : NULL;
        if (t) v = vi ? t->layer_view[vi - 1] : t->view;
    }
    pthread_mutex_unlock(&g_mu);
    if (tex) *tex = t;
    return v;
}
int vkn_tex_destroy_cookie(uint64_t cookie) {
    pthread_mutex_lock(&g_mu);
    const int known = g_init > 0 ? drop_cookie_locked(cookie) : 0;   // (one name; the image dies with its last one)
    pthread_mutex_unlock(&g_mu);
    return known;
}
int vkn_tex_destroy_handle(uint64_t handle) {
    pthread_mutex_lock(&g_mu);
    vkn_tex **r = g_init > 0 ? (vkn_tex **)vkn_tab_get(&g_handles, handle) : NULL; vkn_tex *t = r ? *r : NULL;
    if (t) {   // the name linked under this handle: the texture's own, or an attached one
        vkn_tex_name *n = NULL;
        if (!(t->linked && t->handle == handle)) for (n = t->names; n && !(n->linked && n->handle == handle); n = n->tnext) {}
        if (n) name_drop_locked(n);
        else if (t->names && t->linked && t->handle == handle) handover_locked(t);
        else kill_locked(t);
    }
    pthread_mutex_unlock(&g_mu);
    return t != NULL;
}

// ---- mip reduction: re-link and copy the level tail ----
typedef struct tail_ctx { vkn_tex *src, *dst; uint32_t n; VkImageCopy c[16]; } tail_ctx;
static void tail_run(VkCommandBuffer cb, void *ctx) {
    tail_ctx *u = (tail_ctx *)ctx;
    if (!u->dst->dead) {
        barrier(cb, u->src, u->src->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier(cb, u->dst, u->dst->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyImage(cb, u->src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, u->dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, u->n, u->c);
        barrier(cb, u->src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barrier(cb, u->dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        u->src->layout = u->dst->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        __atomic_store_n(&u->dst->shc_up, __atomic_add_fetch(&g_up_serial, 1, __ATOMIC_RELAXED), __ATOMIC_RELEASE);   // [stage 5 A3]
    }
    pthread_mutex_lock(&g_mu); unref_locked(u->src); unref_locked(u->dst); pthread_mutex_unlock(&g_mu);
    free(u);
}
// Stage 4: the engine's smaller texture (0x920460 makes it by 0x91e2a0 with the old texture's NVN format and flags, no data) was made
// on a device without BC by its own route (no data: never from the pack; the GPU route by format and shape, else CPU-decoded), while
// the old one may be stored otherwise (ASTC pack 4x4 / 5x5 / 6x6, GPU ASTC 4x4, decoded RGBA8): vkCmdCopyImage cannot copy ASTC blocks
// into RGBA8 or between block sizes. The new one is made again as an empty image in the old one's storage (ASTC block size or decoded;
// the sRGB-ness of its own format; UNORM / sRGB of one block size are copy-compatible) and the old one's GPU route mark (later updates
// take the same path), under the same handle and cookie (vkn_tex_create replaces the first one: same pool ids).
static int tex_copy_compatible(const vkn_tex *o, const vkn_tex *n) { return o->format == n->format || (o->astc && o->astc == n->astc); }
static void remake_astc(uint64_t old_cookie, uint64_t new_cookie) {
    pthread_mutex_lock(&g_mu);
    vkn_tex *o = g_init > 0 ? cookie_find_locked(old_cookie) : NULL, *n = g_init > 0 ? cookie_find_locked(new_cookie) : NULL;
    // (n linked under its temporary handle: the engine made it inside this very call; an unlinked one would re-link a handle index
    // that may belong to another texture by now)
    if (!o || !n || tex_copy_compatible(o, n) || !(o->astc || o->decoded) || !(n->astc || n->decoded) || !n->linked || !fmt_is_bc(n->nvn_fmt) ||
        n->nvn_fmt != o->nvn_fmt || n->samples != 1) { pthread_mutex_unlock(&g_mu); return; }
    vkn_tex_desc d; memset(&d, 0, sizeof d);
    const uint32_t tg = n->nvn_target;
    d.efmt = n->efmt; d.nvn_fmt = n->nvn_fmt; d.nvn_target = tg; d.w = n->w; d.h = n->h; d.depth = n->depth; d.mips = n->mips; d.samples = 1; d.cls = n->cls;
    d.layers = tg == 9 ? n->layers / 6 : (tg == 3 || tg == 4 || tg == 6) ? n->layers : 1;
    d.pool_id = n->pool_id; memcpy(d.layer_ids, n->layer_ids, sizeof d.layer_ids); d.sampler_id = n->sampler_id; d.cookie = n->cookie;
    const uint64_t h = n->handle; const uint8_t b = o->astc, bg = o->bcgpu;
    pthread_mutex_unlock(&g_mu);
    const char *why = NULL;
    t_force_astc = b; t_force_dec = !b; t_force_bcg = bg;
    vkn_tex *r = vkn_tex_create(h, &d, NULL, &why);
    t_force_astc = t_force_dec = t_force_bcg = 0;
    pthread_mutex_lock(&g_mu);
    if (r) g_st.pack_tail++;
    pthread_mutex_unlock(&g_mu);
    if (!r) {
        static uint32_t told;
        if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 16)
            vkn_logf(RED "tex: mip reduction %ux%u x %u levels (NVN %#x): %s image %ux%u failed (%s): the level tail stays undefined", d.w, d.h, d.mips, d.nvn_fmt,
                     b ? "ASTC" : "decoded", b, b, why ? why : "?");
    }
}
int vkn_tex_swap_reduced(uint64_t handle, uint64_t old_cookie, uint64_t new_cookie) {
    remake_astc(old_cookie, new_cookie);
    tail_ctx *u = (tail_ctx *)calloc(1, sizeof *u); if (!u) return -1;
    pthread_mutex_lock(&g_mu);
    vkn_tex *o = g_init > 0 ? cookie_find_locked(old_cookie) : NULL, *n = g_init > 0 ? cookie_find_locked(new_cookie) : NULL;
    if (!n) { pthread_mutex_unlock(&g_mu); free(u); return -1; }
    if (!o) {   // the old texture is unknown (its creation failed): the handle still owns the new one, without its level tail
        handle_unlink_locked(n); handle_link_locked(n, handle); g_st.swaps++;
        pthread_mutex_unlock(&g_mu); free(u); return -1;
    }
    handle_unlink_locked(n);          // the temporary handle of the engine call
    handle_unlink_locked(o);
    handle_link_locked(n, handle);
    g_st.swaps++;
    // engine 0x920b00 rule: same format only; one level each -> one copy (smaller depth); else new level i <- old level (n_old - n_new + i)
    int ok = tex_copy_compatible(o, n) && n->mips <= o->mips;   // (ASTC: UNORM / sRGB twins copy as they are)
    if (ok)
        for (uint32_t i = 0; i < n->mips && u->n < 16; i++) {
            const uint32_t sl = o->mips - n->mips + i, nl = o->layers < n->layers ? o->layers : n->layers;
            VkImageCopy *c = &u->c[u->n++];
            c->srcSubresource = (VkImageSubresourceLayers){o->aspect, sl, 0, nl}; c->dstSubresource = (VkImageSubresourceLayers){n->aspect, i, 0, nl};
            const uint32_t dd = mipdim(n->depth, i), sd = mipdim(o->depth, sl);
            c->extent = (VkExtent3D){mipdim(n->w, i), mipdim(n->h, i), dd < sd ? dd : sd};
            if (mipdim(o->w, sl) != c->extent.width || mipdim(o->h, sl) != c->extent.height) { ok = 0; break; }
        }
    if (ok && u->n) { u->src = o; u->dst = n; o->refs++; n->refs++; }
    pthread_mutex_unlock(&g_mu);
    if (!ok || !u->n) { free(u); return ok ? 0 : -2; }
    vkn_upload_push(tail_run, u, 0);
    return 0;
}

// ---- stage 3: attachment views of one (level, layer), references ----
VkImageView vkn_tex_att_view(vkn_tex *t, uint32_t mip, uint32_t layer) {
    if (!t || !t->att || mip >= t->mips || layer >= t->layers) return VK_NULL_HANDLE;   // att: made for 2D-like targets only (not 3D)
    if (!mip && !layer) return t->dead ? VK_NULL_HANDLE : t->att;
    const vkn_dev *dv = vkn_device(); if (!dv) return VK_NULL_HANDLE;
    const uint32_t i = mip * t->layers + layer;
    pthread_mutex_lock(&g_mu);
    if (!t->dead && !t->att_views) t->att_views = (VkImageView *)calloc((size_t)t->mips * t->layers, sizeof(VkImageView));
    VkImageView v = !t->dead && t->att_views ? t->att_views[i] : VK_NULL_HANDLE; const int dead = t->dead || !t->att_views;
    pthread_mutex_unlock(&g_mu);
    if (v || dead) return v;
    // (made outside the lock; a texture that died meanwhile gets no view: kill_locked forgot the framebuffers of the views it saw)
    const int d1 = t->nvn_target == 0 || t->nvn_target == 3;
    VkImageView nv = make_view(dv->dev, t, d1 ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D, t->aspect, mip, 1, layer, 1, (VkComponentMapping){0});
    if (!nv) return VK_NULL_HANDLE;
    pthread_mutex_lock(&g_mu);
    VkImageView drop = VK_NULL_HANDLE;
    if (t->dead) { drop = nv; nv = VK_NULL_HANDLE; }
    else if (t->att_views[i]) { drop = nv; nv = t->att_views[i]; }   // (one recording thread: not expected)
    else t->att_views[i] = nv;
    pthread_mutex_unlock(&g_mu);
    if (drop) vkDestroyImageView(dv->dev, drop, NULL);
    return nv;
}
int vkn_tex_ref(vkn_tex *t) { if (!t) return 0; pthread_mutex_lock(&g_mu); const int ok = t->refs != 0; if (ok) t->refs++; pthread_mutex_unlock(&g_mu); return ok; }
void vkn_tex_unref(vkn_tex *t) { if (t) unref(t); }

// ---- samplers ----
static void sampler_free(void *ctx) { const vkn_dev *d = vkn_device(); if (d) vkDestroySampler(d->dev, (VkSampler)(uintptr_t)ctx, NULL); }
// [stage 5 A1] the creation arguments of every sampler-pool id (the comparison twin is made from them) and the twins themselves
// (vkn_sampler_cmp_by_id). Under g_mu. g_samp_gen[id] + 1 at every create / destroy of the id: a twin made from older arguments is
// not installed.
typedef struct samp_args { uint32_t filter, wrap_u, wrap_v, wrap_w, max_aniso, compare; uint8_t known; } samp_args;
static samp_args g_samp_args[SAMP_IDS]; static uint32_t g_samp_gen[SAMP_IDS];
static VkSampler g_samp_cmp[SAMP_IDS]; static uint8_t g_samp_cmp_own[SAMP_IDS];   // own 0: the engine sampler itself (it compares already)
static uint64_t g_cmp_made, g_cmp_game, g_cmp_failed; static uint32_t g_cmp_alive, g_cmp_reds;
static void cmp_drop_locked(uint32_t id) {   // the twin of id leaves with its engine sampler (under g_mu)
    if (id >= SAMP_IDS) return;
    g_samp_gen[id]++;
    if (g_samp_cmp[id] && g_samp_cmp_own[id]) { vkn_retire(sampler_free, (void *)(uintptr_t)g_samp_cmp[id]); g_cmp_alive--; }
    g_samp_cmp[id] = VK_NULL_HANDLE; g_samp_cmp_own[id] = 0;
}
// The engine's sampler arguments as a VkSamplerCreateInfo. twin 1: the comparison twin of the old path's Dref host (xbg_game.cpp
// dref_sampler, DM_NR_SH_PCF=3): compare LESS (NVN compare func 2 - 1; ref < texel = lit, the Switch shader's manual test), linear
// min / mag (hardware 2x2 PCF per tap), the mip mode, wraps, anisotropy and LOD range of the game's sampler kept.
static VkSamplerCreateInfo samp_info(const vkn_dev *d, uint32_t filter, uint32_t wrap_u, uint32_t wrap_v, uint32_t wrap_w, uint32_t max_aniso, uint32_t compare, int twin) {
    // engine 0x921e40: filter 1..5 -> min linear (mip nearest for 1, 2, 4; mip linear for 3, 5), mag linear; 0 / other -> nearest, mip nearest
    const int lin = twin || (filter >= 1 && filter <= 5), mipl = filter == 3 || filter == 5;
    // wrap: 0 repeat, 1 clamp to edge, 2 mirrored repeat, 3 clamp to border (border colour 0); anything else repeat
    static const VkSamplerAddressMode k_wrap[4] = {VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
                                                   VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
    const float lim = d->props.limits.maxSamplerAnisotropy;
    float an = (float)max_aniso; if (an > lim) an = lim;
    const VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, .minFilter = lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = mipl ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = k_wrap[wrap_u < 4 ? wrap_u : 0],
        .addressModeV = k_wrap[wrap_v < 4 ? wrap_v : 0], .addressModeW = k_wrap[wrap_w < 4 ? wrap_w : 0], .mipLodBias = 0.0f,
        .anisotropyEnable = an > 1.0f && d->feat.samplerAnisotropy ? VK_TRUE : VK_FALSE, .maxAnisotropy = an > 1.0f ? an : 1.0f,
        // compare: engine n (1..7) = VkCompareOp n (less, equal, less-or-equal, greater, not-equal, greater-or-equal, always); 0 = off;
        // above 7 the engine enables the compare with "never"
        .compareEnable = twin || compare ? VK_TRUE : VK_FALSE, .compareOp = twin ? VK_COMPARE_OP_LESS : compare <= 7 ? (VkCompareOp)compare : VK_COMPARE_OP_NEVER,
        .minLod = 0.0f, .maxLod = VK_LOD_CLAMP_NONE, .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, .unnormalizedCoordinates = VK_FALSE};
    return si;
}
int vkn_sampler_create(uint64_t handle, uint32_t pool_id, uint32_t filter, uint32_t wrap_u, uint32_t wrap_v, uint32_t wrap_w, uint32_t max_aniso, uint32_t compare) {
    const vkn_dev *d = vkn_device(); if (!d) { __atomic_add_fetch(&g_st.samplers_failed, 1, __ATOMIC_RELAXED); return -1; }
    pthread_mutex_lock(&g_mu); const int ti = tables_init_locked(); pthread_mutex_unlock(&g_mu);
    if (ti) return -1;
    const VkSamplerCreateInfo si = samp_info(d, filter, wrap_u, wrap_v, wrap_w, max_aniso, compare, 0);
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(d->dev, &si, NULL, &s) != VK_SUCCESS) { __atomic_add_fetch(&g_st.samplers_failed, 1, __ATOMIC_RELAXED); return -1; }
    pthread_mutex_lock(&g_mu);
    int alive = 0; samp_rec *r = (samp_rec *)vkn_tab_put(&g_samp, handle, &alive);
    if (r && alive && r->s) {   // a record of this index we never saw destroyed
        if (r->id < SAMP_IDS && g_samp_by_id[r->id] == r->s) { g_samp_by_id[r->id] = VK_NULL_HANDLE; cmp_drop_locked(r->id); }
        vkn_retire(sampler_free, (void *)(uintptr_t)r->s); g_st.samplers--;
    }
    if (r) { r->s = s; r->id = pool_id; g_st.samplers++; g_st.samplers_made++; }
    if (r && pool_id < SAMP_IDS) {
        g_samp_by_id[pool_id] = s; cmp_drop_locked(pool_id);
        g_samp_args[pool_id] = (samp_args){filter, wrap_u, wrap_v, wrap_w, max_aniso, compare, 1};
    }
    pthread_mutex_unlock(&g_mu);
    if (!r) { vkDestroySampler(d->dev, s, NULL); __atomic_add_fetch(&g_st.samplers_failed, 1, __ATOMIC_RELAXED); return -1; }
    return 0;
}
int vkn_sampler_destroy(uint64_t handle) {
    pthread_mutex_lock(&g_mu);
    samp_rec *r = g_init > 0 ? (samp_rec *)vkn_tab_get(&g_samp, handle) : NULL; int known = 0;
    if (r && r->s) {
        if (r->id < SAMP_IDS && g_samp_by_id[r->id] == r->s) { g_samp_by_id[r->id] = VK_NULL_HANDLE; cmp_drop_locked(r->id); g_samp_args[r->id].known = 0; }
        vkn_retire(sampler_free, (void *)(uintptr_t)r->s); r->s = VK_NULL_HANDLE; g_st.samplers--; known = 1;
    }
    if (r) vkn_tab_kill(&g_samp, handle);
    pthread_mutex_unlock(&g_mu);
    return known;
}
VkSampler vkn_sampler_by_id(uint32_t id) {
    if (id >= SAMP_IDS) return VK_NULL_HANDLE;
    pthread_mutex_lock(&g_mu); const VkSampler s = g_samp_by_id[id]; pthread_mutex_unlock(&g_mu);
    return s;
}
// [stage 5 A1] vkn_fx.h: the comparison twin of the engine sampler of this pool id, made on first use (on the recording thread: one
// vkCreateSampler per sampler id that a Dref binding ever reads, a handful), cached per id, retired with the engine sampler (destroy or
// replacement of the id). A game sampler that compares already is its own twin (the old path left it as it was).
VkSampler vkn_sampler_cmp_by_id(uint32_t id) {
    const vkn_dev *d = vkn_device();
    if (id >= SAMP_IDS || !d) return VK_NULL_HANDLE;
    pthread_mutex_lock(&g_mu);
    VkSampler c = g_samp_cmp[id]; const VkSampler base = g_samp_by_id[id]; const samp_args a = g_samp_args[id]; const uint32_t gen = g_samp_gen[id];
    if (!c && base && a.known && a.compare) { c = g_samp_cmp[id] = base; g_samp_cmp_own[id] = 0; g_cmp_game++; }
    const int none = !c && (!base || !a.known);
    const int red = none && g_cmp_reds < 16 ? (int)++g_cmp_reds : 0;
    if (none) g_cmp_failed++;
    pthread_mutex_unlock(&g_mu);
    if (c || none) {
        if (red) vkn_logf(RED "tex: comparison twin of sampler-pool id %u: no engine sampler there (sampled without the comparison)", id);
        return c;
    }
    const VkSamplerCreateInfo si = samp_info(d, a.filter, a.wrap_u, a.wrap_v, a.wrap_w, a.max_aniso, 0, 1);
    VkSampler t = VK_NULL_HANDLE;
    const VkResult vr = vkCreateSampler(d->dev, &si, NULL, &t);
    pthread_mutex_lock(&g_mu);
    int installed = 0, n = 0;
    if (vr == VK_SUCCESS && g_samp_gen[id] == gen && !g_samp_cmp[id]) { g_samp_cmp[id] = t; g_samp_cmp_own[id] = 1; g_cmp_alive++; n = (int)++g_cmp_made; installed = 1; }
    else if (vr != VK_SUCCESS) g_cmp_failed++;
    c = g_samp_cmp[id];
    const int red2 = vr != VK_SUCCESS && g_cmp_reds < 16 ? (int)++g_cmp_reds : 0;
    pthread_mutex_unlock(&g_mu);
    if (!installed && vr == VK_SUCCESS) vkDestroySampler(d->dev, t, NULL);   // the id changed meanwhile (or another caller won): never used
    if (red2) vkn_logf(RED "tex: comparison twin of sampler-pool id %u: vkCreateSampler %d (sampled without the comparison)", id, (int)vr);
    if (n && n <= 8)
        vkn_logf("tex: comparison twin of sampler-pool id %u: game filter %u wrap %u/%u/%u anisotropy %u compare %u -> compare LESS, linear min / mag, mip %s (%d made)",
                 id, a.filter, a.wrap_u, a.wrap_v, a.wrap_w, a.max_aniso, a.compare, si.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? "linear" : "nearest", n);
    return c;
}
void vkn_sampler_cmp_stats(uint64_t *made, uint64_t *game_cmp, uint64_t *failed, uint32_t *alive) {
    pthread_mutex_lock(&g_mu);
    if (made) *made = g_cmp_made;
    if (game_cmp) *game_cmp = g_cmp_game;
    if (failed) *failed = g_cmp_failed;
    if (alive) *alive = g_cmp_alive;
    pthread_mutex_unlock(&g_mu);
}

void vkn_tex_get_stats(vkn_tex_stats *s) {
    pthread_mutex_lock(&g_mu);
    *s = g_st;
    s->pending = 0; s->sh_storages = s->sh_images = 0;
    if (g_init > 0)
        for (uint32_t i = 0; i < g_slot_hi; i++) {
            const vkn_tex *t = g_slot[i]; if (!t) continue;
            int linked = t->linked;
            for (const vkn_tex_name *n = t->names; n && !linked; n = n->tnext) linked = n->linked;
            if (!linked) s->pending++;
        }
    for (uint32_t b = 0; b < STORE_BUCKETS; b++)   // images on shared storage, and the distinct identities they sit on
        for (const vkn_tex *t = g_store[b]; t; t = t->snext) {
            s->sh_images++;
            const vkn_tex *e = g_store[b]; while (e != t && (e->st_block != t->st_block || e->st_off != t->st_off)) e = e->snext;
            if (e == t) s->sh_storages++;
        }
    pthread_mutex_unlock(&g_mu);
}

// ==== UI piece (stage 3): updates after creation (vkn_tex.h), the swizzle variant of creation ====
// The recording thread: set by a begin listener of vkn_frame (weak: this file also links without the frame and pass modules, as in
// the stage-2 host test; then every update is queued).
extern void vkn_rec_on_begin(void (*fn)(void)) __attribute__((weak));
static pthread_t g_rec_thread; static int g_rec_known;
static vkn_tex_update_stats g_ust;
static void rec_begin_seen(void) { g_rec_thread = pthread_self(); __atomic_store_n(&g_rec_known, 1, __ATOMIC_RELEASE); }
void vkn_tex_update_init(void) {
    static int done;
    if (done || !vkn_rec_on_begin) return;
    done = 1;
    vkn_rec_on_begin(rec_begin_seen);
}
int vkn_tex_on_rec_thread(void) {   // [stage 5 A1] vkn_tex.h
    return __atomic_load_n(&g_rec_known, __ATOMIC_ACQUIRE) && pthread_equal(pthread_self(), g_rec_thread);
}
void vkn_tex_update_stats_get(vkn_tex_update_stats *s) {
    if (!s) return;
    pthread_mutex_lock(&g_mu); *s = g_ust; pthread_mutex_unlock(&g_mu);
}
static void ust_count(uint64_t *c) { pthread_mutex_lock(&g_mu); (*c)++; pthread_mutex_unlock(&g_mu); }

int vkn_tex_update(vkn_tex *t, uint32_t level, uint32_t layer, int32_t x, int32_t y, uint32_t w, uint32_t h, const void *rows, uint32_t pitch) {
    if (!t || !rows || t->dead || !t->image || t->samples != 1) { ust_count(&g_ust.dropped_dead); return -1; }
    if (level >= t->mips || layer >= (t->nvn_target == 2 ? mipdim(t->depth, level) : t->layers)) { ust_count(&g_ust.dropped_rect); return -1; }
    // stage 4: stored from the ASTC pack: BC rows cannot go into it (no CPU ASTC encoder here); stored by the GPU route as ASTC 4x4: they
    // take the same GPU decode + encode (a job of one rectangle, below)
    if (t->astc && t->bcgpu != VKN_BCG_ASTC) { ust_count(&g_ust.dropped_fmt); return -1; }
    // format facts as at creation (the texture's NVN number; decoded / widened textures convert the same way)
    prep p; memset(&p, 0, sizeof p);
    p.nvn = t->nvn_fmt;
    p.bpb = nvn_fmt_bpb((int)p.nvn, &p.bw, &p.bh);
    if (!p.bpb || p.bw <= 0 || p.bh <= 0) { ust_count(&g_ust.dropped_fmt); return -1; }
    if (t->decoded) {
        int bpp = 0; uint32_t pf = 0;
        if (!nvn_bc_unpacked(p.nvn, &pf, &bpp)) { ust_count(&g_ust.dropped_fmt); return -1; }
        p.mode = 1; p.dbpb = (uint32_t)bpp;
    } else if (t->widened) {
        VkFormat to;
        if (!widen_of(p.nvn, &to, &p.cs, &p.one)) { ust_count(&g_ust.dropped_fmt); return -1; }
        p.mode = 2; p.dbpb = 4 * p.cs;
    }
    p.align = lcm16(p.mode ? p.dbpb : (uint32_t)p.bpb);
    // clip to the level (block formats: whole blocks; a clipped origin moves the source pointer)
    const int64_t lw = mipdim(t->w, level), lh = mipdim(t->h, level);
    const uint8_t *src = (const uint8_t *)rows;
    if (x < 0) {
        if ((int64_t)w <= -(int64_t)x || (-x) % p.bw) { ust_count(&g_ust.dropped_rect); return -1; }
        src += (uint64_t)((-x) / p.bw) * (uint64_t)p.bpb; w -= (uint32_t)-x; x = 0;
    }
    if (y < 0) {
        if ((int64_t)h <= -(int64_t)y || (-y) % p.bh) { ust_count(&g_ust.dropped_rect); return -1; }
        src += (uint64_t)((-y) / p.bh) * pitch; h -= (uint32_t)-y; y = 0;
    }
    if (x >= lw || y >= lh || !w || !h) { ust_count(&g_ust.dropped_rect); return -1; }
    if (x + (int64_t)w > lw) w = (uint32_t)(lw - x);
    if (y + (int64_t)h > lh) h = (uint32_t)(lh - y);
    if ((p.bw > 1 || p.bh > 1) && ((x % p.bw) || (y % p.bh) || ((w % (uint32_t)p.bw) && x + (int64_t)w != lw) || ((h % (uint32_t)p.bh) && y + (int64_t)h != lh))) {
        ust_count(&g_ust.dropped_rect); return -1;
    }
    const uint64_t srow = (uint64_t)((w + p.bw - 1) / p.bw) * (uint64_t)p.bpb, nrows = (h + p.bh - 1) / p.bh;
    if (pitch < srow) { ust_count(&g_ust.dropped_rect); return -1; }
    uint64_t bytes = dst_bytes(&p, w, h, 1);
    // staging: the rows tight (converted when the texture is decoded / widened)
    up_ctx *u = (up_ctx *)calloc(1, sizeof *u + sizeof(VkBufferImageCopy));
    if (!u) { ust_count(&g_ust.dropped_mem); return -1; }
    if (t->bcgpu == VKN_BCG_ASTC) {   // stage 4: GPU decode + ASTC 4x4 encode of the rectangle's blocks (vkn_bcgpu.h)
        vkn_bcg_job *j = vkn_bcgpu_job_new(t->nvn_fmt, VKN_BCG_ASTC);
        if (!j || vkn_bcgpu_job_add(j, level, layer, (uint32_t)x, (uint32_t)y, w, h, src, pitch) || vkn_bcgpu_job_stage(j, &bytes)) {
            vkn_bcgpu_job_free(j); free(u); ust_count(&g_ust.dropped_mem); return -1;
        }
        u->job = j; u->t = t;
        goto submit;
    }
    const int al_pow2 = !(p.align & (p.align - 1));
    if (vkn_block_alloc(VKN_ARENA_STAGING, bytes + (al_pow2 ? 0 : p.align), al_pow2 ? (uint32_t)p.align : 16, &u->blk)) { free(u); ust_count(&g_ust.dropped_mem); return -1; }
    u->have_blk = 1;
    const uint64_t skew = (p.align - u->blk.off % p.align) % p.align;
    uint8_t *stage = (uint8_t *)u->blk.cpu + skew;
    if (p.mode == 0) {
        for (uint64_t r = 0; r < nrows; r++) memcpy(stage + r * srow, src + r * pitch, (size_t)srow);
    } else {
        uint8_t *tight = (uint8_t *)malloc((size_t)(srow * nrows));
        if (!tight) { vkn_block_free(&u->blk); free(u); ust_count(&g_ust.dropped_mem); return -1; }
        for (uint64_t r = 0; r < nrows; r++) memcpy(tight + r * srow, src + r * pitch, (size_t)srow);
        const int cr = convert(&p, tight, w, h, 1, stage);
        free(tight);
        if (cr) { vkn_block_free(&u->blk); free(u); ust_count(&g_ust.dropped_fmt); return -1; }
    }
    VkBufferImageCopy *c = &u->c[0];
    c->bufferOffset = u->blk.off + skew; c->bufferRowLength = 0; c->bufferImageHeight = 0;
    const int d3 = t->nvn_target == 2;
    c->imageSubresource = (VkImageSubresourceLayers){t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : t->aspect, level, d3 ? 0 : layer, 1};
    c->imageOffset = (VkOffset3D){x, y, d3 ? (int32_t)layer : 0};
    c->imageExtent = (VkExtent3D){w, h, 1};
    u->n = 1; u->t = t;
submit:;
    // a reference for the copy (up_run drops it)
    pthread_mutex_lock(&g_mu);
    const int alive = !t->dead && t->refs != 0;
    if (alive) t->refs++;
    pthread_mutex_unlock(&g_mu);
    if (!alive) { if (u->have_blk) vkn_block_free(&u->blk); vkn_bcgpu_job_free(u->job); free(u); ust_count(&g_ust.dropped_dead); return -1; }
    const int here = vkn_cb && vkn_pass_layout && __atomic_load_n(&g_rec_known, __ATOMIC_ACQUIRE) && pthread_equal(pthread_self(), g_rec_thread) &&
                     vkn_cb() != VK_NULL_HANDLE;
    if (here) {
        // in order with the draws of this recording: an open pass ends, remembered clears of t run, then the copy
        vkn_pass_layout(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        up_run(vkn_cb(), u);   // -> copy -> SHADER_READ_ONLY_OPTIMAL; staging retired, reference dropped
    } else { u->ns = vkn_now_ns(); vkn_upload_push(up_run, u, bytes); }
    pthread_mutex_lock(&g_mu);
    if (here) g_ust.recorded++; else g_ust.queued++;
    g_ust.bytes += bytes;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

vkn_tex *vkn_tex_create_swz(uint64_t handle, const vkn_tex_desc *d, const vkn_tex_src *src, const uint8_t swz[4], const char **why) {
    t_swz = swz;
    vkn_tex *t = vkn_tex_create(handle, d, src, why);
    t_swz = NULL;
    return t;
}

// ==== shared render-target storage (vkn_tex.h): one creation at a time (g_share_mu, before g_mu), so two textures of one storage
// created on two threads still find each other ====
vkn_tex *vkn_tex_create_shared(uint64_t handle, const vkn_tex_desc *d, const vkn_tex_src *src, const vkn_tex_share *sh, const char **why) {
    if (!sh) return vkn_tex_create(handle, d, src, why);
    pthread_mutex_lock(&g_share_mu);
    t_share = sh;
    vkn_tex *t = vkn_tex_create(handle, d, src, why);
    t_share = NULL;
    pthread_mutex_unlock(&g_share_mu);
    return t;
}
// ==== [stage 5 A3] the shadow cache's own depth images (vkn_tex.h) ====
vkn_tex *vkn_tex_private_depth(VkFormat fmt, uint32_t w, uint32_t h, const char *name) {
    const vkn_dev *dv = vkn_device();
    if (!dv || !w || !h || !fmt_is_depth(fmt)) return NULL;
    vkn_tex *t = (vkn_tex *)calloc(1, sizeof *t);
    if (!t) return NULL;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = {w, h, 1}, .mipLevels = 1,
        .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    t->usage = usage; t->format = fmt; t->aspect = fmt_aspect(fmt); t->w = w; t->h = h; t->depth = 1; t->mips = 1; t->layers = 1; t->samples = 1;
    t->nvn_target = 1; t->cls = VKN_TC_DEPTH; t->layout = VK_IMAGE_LAYOUT_UNDEFINED; t->slot = ~0u; t->pool_id = ~0u; t->sampler_id = ~0u;
    memset(t->layer_ids, 0xff, sizeof t->layer_ids);
    sname_set(t->sname, name);
    if (vkCreateImage(dv->dev, &ci, NULL, &t->image) != VK_SUCCESS) { t->image = VK_NULL_HANDLE; tex_free(t); return NULL; }
    if (vkn_image_mem(t->image, &t->mem, &t->bytes)) { tex_free(t); return NULL; }
    if (!(t->att = make_view(dv->dev, t, VK_IMAGE_VIEW_TYPE_2D, t->aspect, 0, 1, 0, 1, (VkComponentMapping){0}))) { tex_free(t); return NULL; }
    t->shc_up = __atomic_add_fetch(&g_up_serial, 1, __ATOMIC_RELAXED);
    t->refs = 1;
    return t;
}
void vkn_tex_private_free(vkn_tex *t) {
    if (!t) return;
    pthread_mutex_lock(&g_mu);
    if (!t->dead) { t->dead = 1; vkn_fb_forget_view(t->att); unref_locked(t); }
    pthread_mutex_unlock(&g_mu);
}
#endif
