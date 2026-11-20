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
    VkImageView *att_views;                    // vkn_tex_att_view cache: [mip * layers + layer] (NULL until asked; (0, 0) is `att`)
    // render-pass bookkeeping (stage 3, vkn_pass.c only; the recording thread): see vkn_pass.h
    uint64_t pass_written;                     // vkn_frame() + 1 of the last write as an attachment / clear / copy (0 = never)
    uint8_t pass_ever;                         // written by the GPU at least once (attachment, clear, copy destination)
    uint8_t pass_nclr;                         // remembered colour clears (level 0) in pass_clr
    uint8_t pass_dclr;                         // remembered depth-stencil clear: bit 0 depth, bit 1 stencil
    uint32_t pass_stencil; float pass_depth;
    struct { uint32_t layer; float rgba[4]; } pass_clr[4];
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
// ---- stage 3 (render passes) ----
// Attachment view of one (level, layer) of a colour / depth / window target, made on first ask and cached on the texture (destroyed
// with it; its framebuffers leave the cache through vkn_fb_forget_view when the engine destroys the texture). VK_NULL_HANDLE: not a
// target (no attachment usage), 3D, out of range, the engine destroyed it already, or vkCreateImageView failed.
VkImageView vkn_tex_att_view(vkn_tex *t, uint32_t mip, uint32_t layer);
// One more / one less reference: a holder of a vkn_tex pointer beyond the engine's destroy event (the current render targets of
// vkn_pass) keeps the object (not the engine's view of it: t->dead still turns 1) alive until it lets go. vkn_tex_ref: 1 = taken,
// 0 = the object is already on its way out (no reference left: do not keep the pointer).
int vkn_tex_ref(vkn_tex *t);
void vkn_tex_unref(vkn_tex *t);

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


// ==== added by the UI piece (stage 3, vkn_tex.c): texture updates after creation, view swizzles ====
// Writes a rectangle of one (level, layer) of t: rows of block rows `pitch` bytes apart (block formats: x, y, w, h in texels, the rows
// are block rows), copied into a staging block during the call. On the recording thread with a recording open (vkn_frame.h) the copy
// is recorded at once into vkn_cb() in order with the draws around it (vkn_pass_layout ends a pass that is open and runs remembered
// clears first; the texture ends in SHADER_READ_ONLY_OPTIMAL); on any other thread, or with no recording open, it is queued like a
// creation upload (vkn_upload_push: the start of the next recording). The rectangle is clipped to the level. CPU-decoded BC and
// widened textures take the same conversion as at creation (BC: block-aligned rectangles only). 0 = recorded / queued; -1 = dropped
// (dead or unknown texture, empty rectangle, out of range, no memory: counted in vkn_tex_update_stats).
int vkn_tex_update(vkn_tex *t, uint32_t level, uint32_t layer, int32_t x, int32_t y, uint32_t w, uint32_t h, const void *rows, uint32_t pitch);
// Install time (mode 1): registers the begin listener that tells vkn_tex_update which thread records. Without it every update is queued.
void vkn_tex_update_init(void);
typedef struct vkn_tex_update_stats { uint64_t recorded, queued, bytes, dropped_dead, dropped_rect, dropped_fmt, dropped_mem; } vkn_tex_update_stats;
void vkn_tex_update_stats_get(vkn_tex_update_stats *s);
// vkn_tex_create with an explicit view swizzle (NVN TextureSwizzle numbers per output component r, g, b, a: 0 zero, 1 one, 2 R, 3 G,
// 4 B, 5 A; what nvnTextureBuilderSetSwizzle got) instead of the format's own. Applies to the sampled views (whole and per layer).
vkn_tex *vkn_tex_create_swz(uint64_t handle, const vkn_tex_desc *d, const vkn_tex_src *src, const uint8_t swz[4], const char **why);

#ifdef __cplusplus
}
#endif
#endif
