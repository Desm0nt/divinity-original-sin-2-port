// [DOS2] VK-NATIVE stage 2, textures and samplers (vkn_tex.c): every engine texture / render target / window texture as a VkImage
// with its views, every engine sampler as a VkSampler, created from the ENGINE's create calls (dm/host/hle_dos2_vkn_tex.c decodes them
// and passes plain arguments; nothing here knows a game address or a record offset).
//
// Keys: a texture lives under the engine handle (u64) the create call filled, and under a "cookie" (the engine's own texture object,
// an opaque u64 here): the engine destroys a texture by its record, which may be a pending copy whose handle is already reused, so
// the destroy event names the cookie. Draw-time lookups go by the engine's pool ids (the slot arrays of the engine's binding cache hold
// texture-pool ids and sampler-pool ids): vkn_tex_view_by_id / vkn_sampler_by_id.
//
// Formats: the descriptor carries the NVN format NUMBER the engine gave its texture builder (an enum value, no NVN object; 0 = derive
// it from the engine format + sRGB through the engine-format table of vkn_tex.c). BC formats are used natively when the device samples
// them, else decoded on the CPU at creation (nvn_bcdec.h: BC1..BC7 incl. BC6H -> RGBA16F); 3-component formats the device cannot
// sample are widened to 4 components; D24 formats without depth-attachment support become D32F (+S8).
// Data: staging block (VKN_ARENA_STAGING) -> vkn_upload_push (copies + UNDEFINED -> TRANSFER_DST -> SHADER_READ_ONLY) -> staging retired.
// All data is copied (and deswizzled / decoded) inside vkn_tex_create: the caller's pointers need to live only for the call.
// Thread safety: one mutex over the tables; creation may run on any thread.
#ifndef DM_VKN_TEX_H
#define DM_VKN_TEX_H
#include "vkn.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_TEX_LAYER_IDS = 7 };   // per-layer / per-face pool ids the engine keeps (texture object +0xc4..+0xdc)
enum { VKN_TC_SAMPLED, VKN_TC_COLOR, VKN_TC_DEPTH, VKN_TC_WINDOW, VKN_TC_N };   // texture classes (census, usage)
enum { VKN_SRC_NONE, VKN_SRC_REGIONS, VKN_SRC_PACKAGED };

// One (level, layer) of linear data: block rows packed tight (row = ceil(w / block width) blocks), `d` slices for a 3D level from z.
typedef struct vkn_tex_region { uint32_t level, layer, z, w, h, d; const void *src; } vkn_tex_region;

typedef struct vkn_tex_desc {
    uint32_t efmt;          // engine format 0..62 (census / logs; the format itself when nvn_fmt is 0)
    uint32_t nvn_fmt;       // NVN format number the engine used (0 = from efmt + srgb)
    uint32_t nvn_target;    // NVN target the engine used: 0 1D, 1 2D, 2 3D, 3 1D array, 4 2D array, 5 MS, 6 MS array, 8 cube, 9 cube array
    uint32_t w, h, depth;   // depth: 3D only (else 1)
    uint32_t mips, layers;  // layers: array layers (cube: cubes; the image gets 6 x layers)
    uint32_t samples;       // 1 / 2 / 4 / 8
    uint32_t cls;           // VKN_TC_*
    uint8_t srgb, storage;  // storage: the engine asked for an image (compute) usage
    uint32_t pool_id;                         // engine texture-pool id of the whole texture (~0u = none)
    uint32_t layer_ids[VKN_TEX_LAYER_IDS];    // per-layer (cube: per-face) 2D views (~0u = none)
    uint32_t sampler_id;                      // engine sampler-pool id of the texture's default sampler (~0u = none)
    uint64_t cookie;                          // the engine's texture object (identifies the texture in destroy events)
} vkn_tex_desc;

typedef struct vkn_tex_src {
    int kind;                                 // VKN_SRC_*
    const vkn_tex_region *reg; uint32_t nreg; // VKN_SRC_REGIONS
    const void *storage; uint64_t storage_bytes; int pkg_block_h; uint32_t nvn_depth;   // VKN_SRC_PACKAGED: whole Switch block-linear
                                              // storage; pkg_block_h = packaged layout byte 0 + 1; nvn_depth = the builder's size z
} vkn_tex_src;

typedef struct vkn_tex {
    uint64_t handle, cookie;
    VkImage image; VkDeviceMemory mem; uint64_t bytes;
    VkFormat format; VkImageAspectFlags aspect;
    uint32_t w, h, depth, mips, layers, samples, efmt, nvn_fmt, nvn_target, cls;
    uint8_t cube, decoded, widened;           // decoded: BC decoded on the CPU; widened: RGB stored as RGBA
    VkImageView view;                          // the whole texture (type of the engine's target; depth formats: depth aspect)
    VkImageView layer_view[VKN_TEX_LAYER_IDS]; // 2D views of one layer / face, all levels (engine per-layer ids)
    VkImageView att;                           // colour / depth / window targets: level 0, layer 0, every aspect (render passes)
    uint32_t pool_id, layer_ids[VKN_TEX_LAYER_IDS], sampler_id;
    VkImageLayout layout;                      // current layout (written by whoever records: uploads now, render passes in stage 3)
    // private (vkn_tex.c)
    uint32_t slot, refs; uint8_t linked, dead; struct vkn_tex *hnext;
} vkn_tex;

// Creates the texture (VkImage, memory, views) and queues its data. Replaces a texture still linked under the same handle index
// (that one stays known by its cookie until its destroy event). NULL = failed (*why: a short reason, static).
vkn_tex *vkn_tex_create(uint64_t handle, const vkn_tex_desc *d, const vkn_tex_src *src, const char **why);
vkn_tex *vkn_tex_get(uint64_t handle);                          // the texture linked under this very handle, else NULL
VkImageView vkn_tex_view_by_id(uint32_t pool_id, vkn_tex **tex); // engine texture-pool id -> view (whole or one layer); NULL if unknown
int vkn_tex_destroy_cookie(uint64_t cookie);                    // the engine finalized this texture: 1 = known (retired after uploads)
int vkn_tex_destroy_handle(uint64_t handle);                    // same by handle (window textures replaced by the engine)
vkn_tex *vkn_tex_by_cookie(uint64_t cookie);
// Mip reduction (engine 0x920460 semantics): the engine made a smaller texture under a temporary handle (cookie new_cookie), copied
// the level tail of the old one into it and swapped the records, so `handle` now owns the new texture. Re-links the handle, unlinks
// the temporary one, queues the GPU copy of the level tail (old levels (n_old - n_new + i) -> new level i). 0 = ok.
int vkn_tex_swap_reduced(uint64_t handle, uint64_t old_cookie, uint64_t new_cookie);

// Samplers: engine arguments of CreateSampler (filter 0..5, wrap U / V / W 0..3, max anisotropy, compare 0..7).
int vkn_sampler_create(uint64_t handle, uint32_t pool_id, uint32_t filter, uint32_t wrap_u, uint32_t wrap_v, uint32_t wrap_w, uint32_t max_aniso,
                       uint32_t compare);   // 0 = ok
int vkn_sampler_destroy(uint64_t handle);   // 1 = was known (VkSampler retired)
VkSampler vkn_sampler_by_id(uint32_t sampler_id);   // engine sampler-pool id -> VkSampler (VK_NULL_HANDLE = unknown)

// VkFormat of an engine format (0..62) / of an NVN format number, before device fallbacks (UNDEFINED = none).
VkFormat vkn_tex_engine_format(uint32_t efmt, int srgb, uint32_t *nvn_out);
VkFormat vkn_tex_nvn_format(uint32_t nvn);

typedef struct vkn_tex_stats {
    uint32_t alive[VKN_TC_N], pending;        // linked or waiting textures by class; pending = known by cookie only
    uint64_t bytes[VKN_TC_N];                 // image memory by class
    uint64_t created, destroyed, failed, uploads, upload_bytes, upload_ns, decoded, widened, deswizzled, swaps;
    uint32_t samplers; uint64_t samplers_made, samplers_failed;
} vkn_tex_stats;
void vkn_tex_get_stats(vkn_tex_stats *s);

#ifdef __cplusplus
}
#endif
#endif
