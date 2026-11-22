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
// them; else (stage 4) a BC1..BC3 2D texture from packaged storage is looked up in the setup ASTC pack (vkn_astc.h: key = CRC-32 +
// Adler-32 of its Switch storage) and a hit becomes an image of the entry's ASTC format with the pack's blocks; else (stage 4, vkn_bcgpu.h)
// BC1 / BC2 / BC3 / BC7 take the GPU at upload: exact GPU decode + GPU ASTC 4x4 encode (bc-astc-gpu, self-tested per device), else GPU
// decode to RGBA8; anything else is decoded on the CPU at creation (nvn_bcdec.h: BC1..BC7 incl. BC6H -> RGBA16F); 3-component formats the device cannot
// sample are widened to 4 components; D24 formats without depth-attachment support become D32F (+S8).
// [VK-DRV] D24 stand-ins (docs/dos2/nr/VK-DRIVERS.md; KB XCL-02): an X8_D24 / D24S8 texture of ANY class whose device lacks the features
// its class needs (stock Xclipse: no D24 at all; DM_GFX_SIM_GPU=xclipse / nod24 model it) is stored as D32_SFLOAT / D32_SFLOAT_S8_UINT
// (t->d24f = 1), every texture of that format alike (copies stay format-equal). Depth data uploaded into a depth texture goes as the depth
// plane: NVN Z24S8 / X8Z24 keep depth in bits 8..31 (xbg_depth.h), Vulkan's D24 plane wants bits 0..23 and the D32F plane a float
// (vkn_tex_depth_plane; the stencil byte is not uploaded, as before). The backend reads no depth back to the CPU (vkn_read.c refuses depth
// / stencil), so nothing converts back. Switches: DM_VK_NATIVE_D24 = 1 (default) stand-in only where the device lacks D24, 0 never (the
// texture then fails as an unsupported format), 2 always (test of the stand-in on a device with D24); DM_VK_NATIVE_DEPTH_UP = 1
// (default) the depth plane converted at upload, 0 the raw bytes (the backend before VK-DRV).
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
// why an alias of shared render-target storage got an image of its own (vkn_tex_create_shared)
enum { VKN_AL_SIZE, VKN_AL_FORMAT, VKN_AL_SHAPE, VKN_AL_USAGE, VKN_AL_DATA, VKN_AL_VIEWS, VKN_AL_N };

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
    uint8_t d24f;                             // [VK-DRV] an NVN D24 format stored as D32_SFLOAT(_S8_UINT) (the device lacks D24)
    uint8_t astc;                             // stage 4: ASTC block size (4 / 5 / 6) of a BC texture stored as ASTC (pack or GPU encode), 0 = not
    uint8_t bcgpu;                            // stage 4: VKN_BCG_ASTC / VKN_BCG_RGBA8 = written by the GPU BC route (vkn_bcgpu.h), 0 = not
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
    uint64_t pass_clr_ns;                      // vkn_now_ns() of the newest remembered clear (vkn_tex_update orders queued data after it)
    // shared render-target storage (vkn_tex_create_shared; private, vkn_tex.c): the other engine textures attached to this image (their
    // handle / cookie / pool ids resolve to this object), the storage identity it sits on, what aliases are compared against
    struct vkn_tex_name *names; uint32_t nnames;
    uint8_t shared; struct vkn_tex *snext; uint64_t st_block, st_off;
    VkImageUsageFlags usage; VkImageCreateFlags cflags; VkComponentMapping swz;
    char sname[32];                            // the engine's target name (logs)
    uint64_t shc_up;                           // [stage 5 A3] upload number (end of this header)
    uint8_t ui_tgt;                            // [mgpu P7] a target of the UI (vkn_ui.c: GDraw's set, the filter cache atlas): the journal's UI share
    // [P10] the merged tile pass (vkn_pass.c "P10"; recording thread): tl_ep == the chain's / the observer's epoch: attachment tl_i of it;
    // tl_rd == the chain's epoch: a draw of the chain sampled it (a foreign write of it must come after the chain)
    uint32_t tl_ep, tl_rd; uint8_t tl_i;
    // [frame F1] DM_VK_NATIVE_FRAMEAREA (vkn_pass.c "frame area"; the recording thread): while fa_ok and fa_up == shc_up, every texel of
    // level 0 outside the box fa_r (x0, y0, x1, y1; x1 <= x0 = no texel inside) holds the clear value fa_v (colour: the engine's four
    // floats; depth: fa_v[0], stencil fa_s). Written by vkn_pass.c only; zero (unknown) at creation.
    uint8_t fa_ok; uint32_t fa_s; int32_t fa_r[4]; float fa_v[4]; uint64_t fa_up;
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
// Stage 4: an old texture whose storage differs from the new one's (ASTC pack 4x4 / 5x5 / 6x6, GPU ASTC 4x4, decoded RGBA8: the new one
// has no data, so its own route may have chosen otherwise) gets the new one re-made in the old one's storage first (vkCmdCopyImage needs
// compatible formats; UNORM / sRGB of one block size are).
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
// [P10] 1 = the id is known, its sampler filters NEAREST (min and mag) and no DM_VK_NATIVE_TEXHACK twin can replace it; else 0
int vkn_sampler_nearest_by_id(uint32_t sampler_id);

// VkFormat of an engine format (0..62) / of an NVN format number, before device fallbacks (UNDEFINED = none).
VkFormat vkn_tex_engine_format(uint32_t efmt, int srgb, uint32_t *nvn_out);
VkFormat vkn_tex_nvn_format(uint32_t nvn);

// stage 4: how a BC texture is stored: native BC / ASTC from the setup pack / decoded on the CPU / GPU decode + ASTC 4x4 encode / GPU decode to RGBA8
enum { VKN_BC_NATIVE, VKN_BC_PACK, VKN_BC_DECODED, VKN_BC_GPU_ASTC, VKN_BC_GPU_RGBA8, VKN_BC_N };
// VKN_DW_OFF the pack is not usable (missing, DM_VK_NATIVE_ASTC=0, no ASTC on the device), _FMT BC4..BC7 (the pack holds BC1..BC3),
// _SRC not packaged storage (linear data, CreateTexture data, no data), _SHAPE not 2D / one layer / one sample, _MISS_SHAPE no entry
// of its size / format / levels, _MISS_KEY no entry with its bytes, _BLOCK the entry's block size is not usable on the device
enum { VKN_DW_OFF, VKN_DW_FMT, VKN_DW_SRC, VKN_DW_SHAPE, VKN_DW_MISS_SHAPE, VKN_DW_MISS_KEY, VKN_DW_BLOCK, VKN_DW_N };
typedef struct vkn_tex_stats {
    uint32_t alive[VKN_TC_N], pending;        // linked or waiting textures by class; pending = known by cookie only
    uint64_t bytes[VKN_TC_N];                 // image memory by class
    uint64_t created, destroyed, failed, uploads, upload_bytes, upload_ns, decoded, widened, deswizzled, swaps;
    uint32_t samplers; uint64_t samplers_made, samplers_failed;
    // ---- stage 4 (ASTC pack): BC textures by how they are stored, VKN_BC_*: alive, image bytes alive, created so far ----
    uint32_t bc_alive[VKN_BC_N]; uint64_t bc_bytes[VKN_BC_N], bc_made[VKN_BC_N];
    uint64_t dec_why[VKN_DW_N];               // why a BC texture on a device without BC was decoded, VKN_DW_*
    uint64_t pack_ns;                         // creation time in the pack path (shape filter, key hash, read of the blocks)
    uint64_t pack_tail;                       // mip reductions re-made in the old texture's storage (vkn_tex_swap_reduced)
    uint64_t bc_ns[VKN_BC_N];                 // creation time of BC textures by storage (the creating thread: pack read, GPU job build, CPU decode)
    uint64_t bc_upload[VKN_BC_N];             // bytes staged for them (BC blocks for the GPU routes, decoded texels for the CPU one)
    // ---- shared render-target storage (vkn_tex_create_shared): alive storage identities with an image on them, images on them,
    // engine textures attached to another one's image; creations so far: new image, attached, kept separate by reason (VKN_AL_*),
    // names detached (destroyed while their image lives on), owner name handed over to an attached one
    uint32_t sh_storages, sh_images, sh_aliases;
    uint64_t sh_made, sh_attached, sh_kept[VKN_AL_N], sh_detached, sh_handover;
} vkn_tex_stats;
void vkn_tex_get_stats(vkn_tex_stats *s);
// [VK-DRV] D24 stand-ins: textures made as D32F (+S8) for an NVN D24 format, depth uploads converted; *on = a stand-in was ever made
void vkn_tex_depth_stats(uint64_t *standins, uint64_t *converted, int *on);
// [VK-DRV] the depth plane of n texels of NVN depth format nvn (0x32 D16, 0x33 X8Z24, 0x34 D32F, 0x35 Z24S8, 0x36 Z32F_X24S8) in the
// layout of the Vulkan image's depth aspect: D16 2 bytes, D24 4 bytes depth in bits 0..23, D32F 4 bytes float; d24f = 1: an NVN D24
// format into a D32F stand-in (depth / 16777215). Returns the destination bytes per texel, 0 = not a depth format with a depth plane.
uint32_t vkn_tex_depth_plane(uint32_t nvn, int d24f, const uint8_t *src, uint64_t n, uint8_t *dst);
// [VK-SEED] the VkFormat this device makes a render target of recorded VkFormat f as (depth = 1: a depth / stencil attachment, else a
// colour one): the D24 stand-in rule of the texture path above (DM_VK_NATIVE_D24, DM_GFX_SIM_GPU=xclipse / nod24), 0 = this device
// cannot render to it. Any thread (format properties of the device; vkn_seed.c translates the portable seed's passes with it).
uint32_t vkn_tex_rt_format(uint32_t f, int depth);


// ==== added by the UI piece (stage 3, vkn_tex.c): texture updates after creation, view swizzles ====
// Writes a rectangle of one (level, layer) of t: rows of block rows `pitch` bytes apart (block formats: x, y, w, h in texels, the rows
// are block rows), copied into a staging block during the call. On the recording thread with a recording open (vkn_frame.h) the copy
// is recorded at once into vkn_cb() in order with the draws around it (vkn_pass_layout ends a pass that is open and runs remembered
// clears first; the texture ends in SHADER_READ_ONLY_OPTIMAL); on any other thread, or with no recording open, it is queued like a
// creation upload (vkn_upload_push: the start of the next recording). The rectangle is clipped to the level. CPU-decoded BC and
// widened textures take the same conversion as at creation (BC: block-aligned rectangles only; a texture stored from the ASTC pack
// cannot take BC data: dropped as dropped_fmt; one stored as GPU ASTC 4x4 takes them through the GPU decode + encode). 0 = recorded / queued; -1 = dropped
// (dead or unknown texture, empty rectangle, out of range, no memory: counted in vkn_tex_update_stats).
int vkn_tex_update(vkn_tex *t, uint32_t level, uint32_t layer, int32_t x, int32_t y, uint32_t w, uint32_t h, const void *rows, uint32_t pitch);
// Install time (mode 1): registers the begin listener that tells vkn_tex_update which thread records. Without it every update is queued.
void vkn_tex_update_init(void);
typedef struct vkn_tex_update_stats { uint64_t recorded, queued, bytes, dropped_dead, dropped_rect, dropped_fmt, dropped_mem; } vkn_tex_update_stats;
void vkn_tex_update_stats_get(vkn_tex_update_stats *s);
// vkn_tex_create with an explicit view swizzle (NVN TextureSwizzle numbers per output component r, g, b, a: 0 zero, 1 one, 2 R, 3 G,
// 4 B, 5 A; what nvnTextureBuilderSetSwizzle got) instead of the format's own. Applies to the sampled views (whole and per layer).
vkn_tex *vkn_tex_create_swz(uint64_t handle, const vkn_tex_desc *d, const vkn_tex_src *src, const uint8_t swz[4], const char **why);

// ==== shared render-target storage (aliasing; the cause of black save-game thumbnails in mode 1) ====
// The engine creates some named targets on one reserved piece of memory per "storage key" (several names map to one key): on the
// Switch those textures ARE the same memory, and the engine relies on it (the screenshot target is never drawn into: the scene's last
// pass writes the scaled back buffer, which is the same storage, and the copy out of the screenshot target reads the scene).
// vkn_tex_create_shared creates a texture that sits on storage `sh` (identity: the engine's pool block + offset of the reservation).
// When a live image on the same identity has exactly the same shape (VkFormat and NVN format, size, depth, levels, layers, samples,
// target, usage, create flags, view swizzle, the per-layer views this one needs) and the creation has no initial data, NO new image is
// made: the new engine texture is attached to that one - its handle, cookie and pool ids resolve to the SAME vkn_tex (one image, one
// layout, one set of remembered clears / "ever written" / views / attachment views), and the returned pointer is that object (its
// `handle` field stays the first live name's). The object lives until its last name is destroyed (destroy by cookie or handle drops
// one name; the owner's own name leaving hands the object's identity fields to an attached name). Anything that differs (the same
// memory reinterpreted: garbage on the Switch too) gets an image of its own on the same identity; VKN_AL_* counts why.
// The other create functions never attach (identity unknown): only creations the engine made with its "shared storage" flag call this.
typedef struct vkn_tex_share {
    uint64_t block, offset;                   // storage identity (the engine's pool block and offset in it)
    uint64_t bytes;                           // reservation size (logs)
    const char *name, *key;                   // the engine's target name and storage-key name (logs; NULL = unknown)
} vkn_tex_share;
vkn_tex *vkn_tex_create_shared(uint64_t handle, const vkn_tex_desc *d, const vkn_tex_src *src, const vkn_tex_share *sh, const char **why);

// ==== [stage 5 A1] named shaders + Dref (vkn_shlib_named.h); vkn_sampler_cmp_by_id itself is declared in vkn_fx.h ====
// 1 = the calling thread is the recording thread (the begin listener of vkn_tex_update_init saw it begin a recording): its current
// target set (vkn_pass.h) is the pass a pipeline is being built for. 0 = another thread or not known yet.
int vkn_tex_on_rec_thread(void);
// The comparison twins (vkn_sampler_cmp_by_id): made, ids whose game sampler compares already (its own twin), refused (no engine
// sampler under the id / vkCreateSampler failed), alive now.
void vkn_sampler_cmp_stats(uint64_t *made, uint64_t *game_cmp, uint64_t *failed, uint32_t *alive);
// ==== [stage 5 A3] the shadow cache (vkn_shc.c): upload numbers, own depth images ====
// vkn_tex.shc_up: a number unique over every texture of the process, taken at creation and again whenever this module records data
// into the image (a creation upload, vkn_tex_update, the level-tail copy of vkn_tex_swap_reduced) - in command order, so a draw recorded
// after the new number reads the new texels. Writes by the GPU through vkn_pass (attachment, clear, copy destination, storage) do not
// take a number: they set pass_ever, and the cache treats such a texture as having no version (never kept). Read on the recording thread.
vkn_tex *vkn_tex_private_depth(VkFormat fmt, uint32_t w, uint32_t h, const char *name);
// The backend's own depth target: one VkImage (w x h, 1 level / layer / sample; usage depth attachment, transfer source and
// destination) with its attachment view, class VKN_TC_DEPTH, no engine handle / cookie / pool id (no lookup finds it); vkn_pass takes it
// as any target. NULL = failure. vkn_tex_private_free: its owner lets go (dead, the framebuffers of its view forgotten; the object is
// retired when the last reference - vkn_pass's current set - is dropped).
void vkn_tex_private_free(vkn_tex *t);
// [DOS2] VK-DRS-FSR (vkn_fsr.c): the backend's own COLOUR target: as vkn_tex_private_depth (no engine handle / id, vkn_pass takes it as any
// target, vkn_tex_private_free lets it go), usage colour attachment + sampled + transfer, class VKN_TC_COLOR, `view` (sampled) and `att`.
vkn_tex *vkn_tex_private_color(VkFormat fmt, uint32_t w, uint32_t h, const char *name);
// ==== [stage 5.1 G] the binding generation (vkn_draw.h "stage 5 draw cost": the texture-binding reuse of DM_VK_NATIVE_COST bit SET) ====
// + 1 (atomic, under this module's mutex) at every change that can make vkn_tex_view_by_id (the view or the vkn_tex it returns),
// vkn_sampler_by_id or vkn_sampler_cmp_by_id answer differently for some id: a pool-id cell set or cleared (creation, attach to shared
// storage, a name dropped / handed over, destroy, replacement), a texture slot taken or freed, a mip-reduction swap, a private texture
// freed, a sampler created / replaced / destroyed, a comparison twin installed or dropped. Texel writes (uploads, updates, GPU writes)
// and layout changes do not count: they change no answer. A plain atomic load; read it BEFORE the lookups it is to guard.
uint32_t vkn_tex_bind_gen(void);
// ==== [stage 5.4 G2] DM_HACK_TEXBIAS / DM_HACK_ANISO (dm/gfx/nr/nr_hacks.h; the old path: xbg_nr_hacks.inc par) ====
// The player's values (explicit DM_HACK_<name> over DM_HACK_TIER), read once at the first sampler creation: a mip LOD bias added and an
// anisotropy cap N (min(game's, N), 1 = off). Applied through a HACK TWIN per sampler-pool id: the game sampler's VkSamplerCreateInfo
// with mipLodBias + bias and maxAnisotropy min(game, N), made at vkn_sampler_create (loader thread: never on the draw path), retired
// with its game sampler. Which binding gets the twin, DM_VK_NATIVE_TEXHACK (new; no old name - the old path applied the values in the
// translator's sampler lookup):
//   0  off: the values are not applied (the hacks line of vkn_hacks.c prints it red);
//   1  per texture (default; the old path's rule): vkn_sampler_by_id returns the twin when this thread's previous vkn_tex_view_by_id
//      gave a texture the CPU uploaded and the GPU never wrote (class SAMPLED, no vkn_pass write), with >= 2 levels: the draw path asks
//      view then sampler per texture binding (vkn_draw.c bind_textures, the effect pieces the same way); a sampler lookup without a view
//      before it gets the game sampler. Targets, the post chain and the UI atlas keep the game's values (a sampler bias also moves
//      explicit textureLod reads: Vulkan's lambda = lod + mipLodBias). Comparison twins (Dref: the shadow atlas, a target): unchanged.
//   2  per sampler: every lookup of a sampler that repeats or mirrors in U or V gets the twin (materials tile; targets / post / UI
//      clamp), whatever the texture; its comparison twin carries the values too. No per-texture decision.
// The gear applies DM_HACK_* at the next launch (Tl2Gear.java "next launch"): nothing changes them in a running game. vkn_tex_hack_set
// is the live path anyway (host test; a future live row): any thread; every twin (and every comparison twin of our own) is retired
// (vkn_retire: 3 frames - a draw recorded before still has its sampler), the binding generation bumped (the draw-cost reuse rebuilds),
// twins made again from the creation arguments; returns how many. Twins count as samplers in vkn_tex_stats (samplers, samplers_made):
// the draw path's death check (samplers_made - samplers) sees a retired twin and forgets its descriptor sets.
typedef struct vkn_tex_hack_stats {
    int mode, on, aniso; float bias;              // DM_VK_NATIVE_TEXHACK, any value in effect, the cap, the bias (mip levels)
    uint32_t twins;                               // alive
    uint64_t made, failed, retired, picked, kept, cmp, sets;   // twins made / failed / retired; lookups that took the twin / kept the
                                                  // game sampler (mode 1: not a material); comparison twins with the values (mode 2); live sets
} vkn_tex_hack_stats;
void vkn_tex_hack_get(vkn_tex_hack_stats *s);
int vkn_tex_hack_set(int mode, float bias, int aniso);
VkSampler vkn_sampler_hack_by_id(uint32_t sampler_id);   // the twin of an id (VK_NULL_HANDLE = none); tests and logs

// ==== [rt P0] DM_VK_NATIVE_PROF=3: the draw-time lookups' lock (vkn_tex_view_by_id / vkn_sampler_by_id / _cmp_by_id) taken / found taken ====
extern int vkn_tex_lockprof;
void vkn_tex_lock_stats(uint64_t *locks, uint64_t *contended);
// ==== [rt P3] DM_VK_NATIVE_COST bit 0x100 (vkn_draw.c, the lock-free mirror of the pool ids) ====
// 1 = vkn_sampler_by_id(id) answers from the id alone (no DM_VK_NATIVE_TEXHACK=1 rule that looks at the texture of this thread's last
// vkn_tex_view_by_id). 0 = the mirror stays off (it would skip the view lookup that rule reads). Lock-free; any thread.
int vkn_sampler_by_id_pure(void);
#ifdef __cplusplus
}
#endif
#endif
