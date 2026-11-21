// [DOS2] VK-NATIVE state piece (vkn_state.c): the engine's state objects (blend, depth-stencil, raster), vertex formats, render
// passes, framebuffers and the pipeline cache. Design: docs/dos2/nr/VK-NATIVE.md §2.2 / §3.5; the engine's arguments and enum tables:
// VK-NATIVE-MAP-1.md §2 / §3, how they are applied at draw time: VK-NATIVE-MAP-2.md §0 (6, 7), §4.1.
// No game addresses, no NVN objects: dm/host/hle_dos2_vkn_state.c passes the engine's plain arguments.
//
// Ids. Every state object and vertex format gets a small CONTENT id (1-based, 0 = none / unknown): two engine handles with the same
// arguments share one id, and an id never changes its meaning (contents are never freed - the engine has 10 blend, 18 depth-stencil,
// 10 raster states and a few dozen vertex formats). So a pipeline key made of ids never goes stale when the engine destroys a handle
// and reuses its slot; a destroy only drops the handle -> id mapping. Render passes get a pass id and a COMPATIBILITY id (formats +
// samples only, Vulkan render pass compatibility): pipelines are keyed by the compatibility id.
// Threads: the create / destroy entry points run on the engine's loader threads, the lookups and builds on the render thread; every
// table has its own mutex. The content structs returned by pointer are immutable once created (static storage).
#ifndef DM_VKN_STATE_H
#define DM_VKN_STATE_H
#include <stdint.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_STATE_IDS = 255, VKN_VFMT_IDS = 1023, VKN_VFMT_LOCS = 16, VKN_VFMT_STREAMS = 3 };
// Binding of the "null stream": a vertex shader input the vertex format does not feed reads this binding (stride 0, format
// R32G32B32A32_SFLOAT, offset 0). The draw path binds a 16-byte buffer {0, 0, 0, 1.0f} there (NVN's / GL's value of a disabled
// attribute) for every pipeline built with vkn_pipe_src.vs_inputs set.
enum { VKN_VFMT_NULL_BINDING = 3 };

// ---- state objects (engine arguments exactly as CreateBlendState / CreateDepthStencilState / CreateRasterState take them) ----
// Blend 0x922530(r, srcRGB, dstRGB, srcA, dstA, alphaOp, flags): factors 0..10 (0 zero, 1 one, 2 src colour, 3 1-src colour,
// 4 dst colour, 5 1-dst colour, 6 src alpha, 7 1-src alpha, 8 dst alpha, 9 1-dst alpha, 10 alpha saturate; others zero);
// flags bit 0: separate alpha (factors srcA / dstA and alphaOp 1..4 = sub, reverse sub, min, max; else add); without it the alpha
// factors are the colour factors and both equations add. Colour always adds. Blending is on for every bound colour target (the
// engine expresses "no blending" by factors: one / zero / add becomes blendEnable = VK_FALSE here, same result).
uint16_t vkn_blend_put(uint64_t h, uint8_t src_rgb, uint8_t dst_rgb, uint8_t src_a, uint8_t dst_a, uint8_t alpha_op, uint32_t flags);
// Depth-stencil 0x922d10(r, depthTest, depthWrite, depthFunc, stencilEnable, stencilMask, stencilFunc, depthFailOp, passOp):
// functions 0..7 = never, less, equal, lequal, greater, notequal, gequal, always (others never); ops 0..7 = keep, zero, replace,
// invert, incr wrap, decr wrap, incr, decr (others keep); stencil fail op is always keep; both faces alike. The byte is BOTH the
// write and the compare mask (0x92c3c0 binds it to SetStencilMask and SetStencilValueMask); the reference is in the pipeline key.
uint16_t vkn_ds_put(uint64_t h, uint8_t depth_test, uint8_t depth_write, uint8_t depth_func, uint8_t stencil_enable,
                    uint8_t stencil_mask, uint8_t stencil_func, uint8_t depth_fail_op, uint8_t pass_op);
// Raster 0x923360(r, cull, fill, flagA, flagB, polygonOffset): cull 1 back, 2 front, else none; fill 0 solid, 1 lines, else points;
// front face clockwise (NVN window origin upper left, as Vulkan: no flip); polygonOffset bit 0 = depth bias on (values: see
// vkn_state_set_depth_bias). flagA / flagB are only stored by the engine, never applied (every reader of the record binds +8 only).
uint16_t vkn_raster_put(uint64_t h, uint8_t cull, uint8_t fill, uint8_t flag_a, uint8_t flag_b, uint32_t polygon_offset);
// Depth bias of every raster state with polygonOffset set. The engine sets it once per frame in 0x93d9f0:
// nvnCommandBufferSetPolygonOffsetClamp(factor -2.5, units -5.0, clamp 0) (GL glPolygonOffsetClamp: offset = factor * dz/dxy +
// units * r, clamp 0 = none), i.e. depthBiasSlopeFactor = factor, depthBiasConstantFactor = units * units_x, depthBiasClamp = clamp
// (0 when the device lacks depthBiasClamp). units_x: the caller's scale for drivers that halve the constant (PanVK, RADV: 2) or a
// D16 stand-in image of more bits; 1 normally. Those are the defaults until somebody calls this. Pipelines are built with the values
// current at build time (bias is not dynamic state); a later change only reaches new pipelines (logged once).
void vkn_state_set_depth_bias(float factor, float units, float clamp, float units_x);
// Destroys 0x923c20 / 0x923e60 / 0x924080: drop the handle -> id mapping. 1 = the handle was known.
int vkn_blend_kill(uint64_t h);
int vkn_ds_kill(uint64_t h);
int vkn_raster_kill(uint64_t h);
// Handle -> content id (0 = unknown handle).
uint16_t vkn_blend_id(uint64_t h);
uint16_t vkn_ds_id(uint64_t h);
uint16_t vkn_raster_id(uint64_t h);
// Content by id (NULL = no such id). colorWriteMask of the blend is VK_COLOR_COMPONENT_RGBA (the real mask comes from the target
// format, vkn_write_mask_*, and is applied per attachment at pipeline build); stencil reference 0 (key); raster bias = current values.
const VkPipelineColorBlendAttachmentState *vkn_blend_att(uint16_t id);
const VkPipelineDepthStencilStateCreateInfo *vkn_ds_info(uint16_t id);
const VkPipelineRasterizationStateCreateInfo *vkn_raster_info(uint16_t id);

// ---- vertex formats: CreateVertexFormat 0x917100(r, elems, count, ?, u64 *out) with 6-byte elements {stream 0..2, usage, usage
// index, per-instance, component type 0..11, component count 1..4} (BuildVertexFormat 0x9181d0). Location from usage (2 -> 4+index,
// 3 -> 1, 4 -> 3, 5 -> 2, 6 -> 14, 7 -> 15, 8 -> 9+index, else 0); offset = running stride of the element's stream (stride grows by
// the element size; an element of an unknown type adds nothing); a stream is per-instance when any of its elements is; a location
// set twice keeps the last element (as NVN's attribute state). Binding number = stream number. ----
enum { VKN_CLS_NONE = 0, VKN_CLS_FLOAT = 1, VKN_CLS_SINT = 2, VKN_CLS_UINT = 3 };
typedef struct vkn_vfmt {
    VkVertexInputAttributeDescription attr[VKN_VFMT_LOCS]; uint32_t nattr;   // sorted by location
    VkVertexInputBindingDescription bind[VKN_VFMT_STREAMS]; uint32_t nbind;   // only the streams some attribute reads
    uint32_t stride[VKN_VFMT_STREAMS];                                        // the engine's running strides (all three streams)
    uint8_t loc_class[VKN_VFMT_LOCS];   // per location: VKN_CLS_* of the data as the shader sees it (unorm / snorm / half = float)
    uint8_t loc_comps[VKN_VFMT_LOCS];   // per location: component count of the engine element (1..4; the format may be wider, below)
    uint16_t loc_mask;                  // locations present
    uint16_t widened;                   // locations whose 3-component 8 / 16-bit format the device lacks as vertex format: 4 components read
} vkn_vfmt;
// Idempotent: the engine returns the existing handle for equal elements, and equal elements get the same id. Returns the id (0 = bad).
uint16_t vkn_vfmt_put(uint64_t h, const uint8_t *elems, uint32_t count);
uint16_t vkn_vfmt_id(uint64_t h);
int vkn_vfmt_kill(uint64_t h);                       // no engine destroy is known; for completeness
const vkn_vfmt *vkn_vfmt_info(uint16_t id);          // NULL = no such id

// ---- colour write mask by target format (0x9282c0 ChannelMaskForFormat): the engine picks the mask from the bound texture's NVN
// format alone; the mask argument of ApplyBlend 0x9284e0 is never applied. Formats outside its four lists get mask 0 (nothing is
// written: e.g. R8SN, RG16SN, RGBA8UI, RGB10A2UI). Chosen interface: by ENGINE format (0..62, the texture record's +0x10, what the
// texture piece keeps per texture) - exact, independent of the VkFormat the texture piece substitutes (an RGB8 target stored as
// RGBA8 still gets RGB). The NVN-format variant is for textures made with an explicit NVN format (CreateTexture's stack argument;
// the texture piece knows it). sRGB does not change the mask (RGBA8 / RGBA8_SRGB / BGRA8 / BGRA8_SRGB are all RGBA). ----
VkColorComponentFlags vkn_write_mask_engine(uint32_t engine_format);
VkColorComponentFlags vkn_write_mask_nvn(uint32_t nvn_format);

// ---- primitives: engine value 1..7 (0x92c3c0 via table 0x1f58640 = NVN triangles, triangle fan, triangle strip, quads, lines,
// line strip, line loop; out of range -> NVN 0 = points). Vulkan has no quads / line loops and the port does not use fans: those
// three need an index rewrite by the draw path, which then asks for the primitive vkn_prim_rewrite gives. ----
int vkn_prim_topology(uint8_t prim, VkPrimitiveTopology *out);   // 0 = direct; 1 = needs a rewrite (*out untouched)
uint8_t vkn_prim_rewrite(uint8_t prim);   // quads -> 1 (two triangles a quad), fan -> 1, line loop -> 6 (strip + the first index again); else prim

// ---- render passes. One subpass; attachments in order: the bound colour slots (VK_FORMAT_UNDEFINED = slot not bound: attachment
// reference VK_ATTACHMENT_UNUSED), then depth. Layouts: colour COLOR_ATTACHMENT_OPTIMAL -> COLOR_ATTACHMENT_OPTIMAL, depth
// DEPTH_STENCIL_ATTACHMENT_OPTIMAL both ways; initial UNDEFINED when nothing of the attachment is loaded (load op not LOAD). Two
// fixed external dependencies (attachment writes of the pass before -> this pass's attachment access; this pass's writes -> later
// fragment / compute shader reads, attachments, transfers), the same in every pass, so passes of equal formats and samples stay
// compatible. Layout changes for sampling are the draw path's barriers (§4.3). ----
typedef struct vkn_rp_key {
    uint32_t color[8];                // VkFormat per slot; VK_FORMAT_UNDEFINED = not bound
    uint32_t depth;                   // VkFormat; VK_FORMAT_UNDEFINED = no depth-stencil
    uint8_t ncolor;                   // colour slots 0..8 (highest bound slot + 1)
    uint8_t samples;                  // VkSampleCountFlagBits (1, 2, 4, 8)
    uint8_t load[8], store[8];        // VkAttachmentLoadOp (LOAD / CLEAR / DONT_CARE), VkAttachmentStoreOp (STORE / DONT_CARE)
    uint8_t depth_load, depth_store, stencil_load, stencil_store;
    uint8_t pad[2];
} vkn_rp_key;
void vkn_rp_key_init(vkn_rp_key *k);   // zero (the key is hashed as bytes), samples 1
// The pass of a key (created on a miss). Returns the pass id (1-based, 0 = failure); *pass / *compat filled when not NULL.
uint16_t vkn_rp_get(const vkn_rp_key *k, VkRenderPass *pass, uint16_t *compat);
VkRenderPass vkn_rp_compat_pass(uint16_t compat);   // the first pass of that compatibility class (what pipelines are built against)
int vkn_rp_mode_get(int *dep, int *te);
// [mgpu P6.2 / P6.3] the pure parts (host test): the destination stages of the dependency after a pass (narrow: no vertex / compute), the
// initial layout of an attachment (loaded, or te: the attachment layout vkn_pass always moves it into before the begin; else UNDEFINED)
static inline VkPipelineStageFlags vkn_rp_dep_after(int narrow) {
    const VkPipelineStageFlags att = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    return att | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | (narrow ? 0u : (VkPipelineStageFlags)(VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT));
}
static inline VkImageLayout vkn_rp_initial(int loaded, int te, int depth) {
    return loaded || te ? (depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) : VK_IMAGE_LAYOUT_UNDEFINED;
}   // [mgpu P6.2 / P6.3] how passes are built (vkn_state.c "mgpu P6"): narrow external dependency, initial layout
// Framebuffer of (compatibility class, views in attachment order: bound colour slots then depth, extent, layers); created on a miss.
VkFramebuffer vkn_fb_get(uint16_t compat, const VkImageView *views, uint32_t nviews, uint32_t width, uint32_t height, uint32_t layers);
// [VK-DRV] the same, made with render pass rp itself (DM_VK_NATIVE_FB_EXACT, vkn_drv.h: PowerVR stock); rp NULL = vkn_fb_get.
VkFramebuffer vkn_fb_get_rp(uint16_t compat, VkRenderPass rp, const VkImageView *views, uint32_t nviews, uint32_t width, uint32_t height, uint32_t layers);
// The texture piece calls this BEFORE it destroys an image view: every framebuffer using the view is retired (destroyed after the
// frame delay of vkn_retire) and leaves the cache. Thread safe.
void vkn_fb_forget_view(VkImageView v);

// ---- pipelines. The key is a few small integers packed in three words (vkn_pipe_key_pack). On a miss the caller's sources build
// it; failures are cached (no rebuild storm) and counted. Dynamic state: viewport and scissor only (one each). Single VkPipelineCache
// of this piece; with the warm-up on (vkn_warm.h) it is created from vkn-pipeline-cache.bin by the warm-up worker and saved by it.
// Threads (stage 4): the render thread looks up and builds; the warm-up worker builds into its own table (vkn_warm.c), which a miss
// asks before building (vkn_warm_take: a pipeline the worker is building right now is waited for, never built twice). No build runs
// under this piece's table mutex, so a hit never waits for a build. ----
typedef struct vkn_pipe_key { uint64_t w[3]; } vkn_pipe_key;
typedef struct vkn_pipe_desc {
    uint32_t shader;                   // the shader piece's small id (1..2^24-1)
    uint8_t vs_variant, fs_variant;    // module variants (vertex input retype, alpha test, ...): the shader piece's numbering
    uint16_t blend, ds, raster, vfmt;  // content ids (0 = none: blend off / depth-stencil off / default raster / no vertex input)
    uint8_t prim;                      // engine primitive 1..7 (after vkn_prim_rewrite)
    uint8_t stencil_ref;               // block +0x10 of 0x92c3c0 (forced 0 when the depth-stencil state has no stencil test)
    uint16_t pass;                     // render pass COMPATIBILITY id
    uint8_t write_mask[8];             // VkColorComponentFlags per colour slot (vkn_write_mask_*; 0 for a slot not bound)
    uint8_t fx;                        // [stage 5, vkn_fx.h] VKN_PIPE_FX_*: variants a draw effect asked for (0 = the engine's own draw)
} vkn_pipe_desc;
// fx bits. DISCARD: rasterizerDiscardEnable. NO_FS: built without the fragment stage (the caller passes vkn_pipe_src.fs = NULL).
// A swapped depth-stencil state is simply another `ds` id; a swapped shader another `shader` id (vkn_fx.h VKN_FX_SHADER_ID0).
enum { VKN_PIPE_FX_DISCARD = 1, VKN_PIPE_FX_NO_FS = 2, VKN_PIPE_FX_MASK = 3 };
vkn_pipe_key vkn_pipe_key_pack(const vkn_pipe_desc *d);
void vkn_pipe_key_unpack(const vkn_pipe_key *k, vkn_pipe_desc *d);
// What a miss needs from the caller (the shader piece's objects; this piece does not include its header).
typedef struct vkn_pipe_src {
    VkShaderModule vs, fs;             // fs may be VK_NULL_HANDLE (depth-only)
    VkPipelineLayout layout;
    VkRenderPass pass;                 // VK_NULL_HANDLE = vkn_rp_compat_pass(key's pass)
    uint16_t vs_inputs;                // locations the vertex module reads (0 = unknown: no null-stream fill)
    // [DOS2] VK-NATIVE stage 4 (pipeline warm-up, vkn_warm.h): what makes a build replayable in a later run without a draw. Zero
    // (memset) = not given: the build is not listed. shader: the vkn_shader the modules belong to (vkn_shader.h; its stage keys);
    // retype: the vertex variant's code; alpha_func / alpha_ref: the fragment variant (0 = base module).
    const void *shader;
    uint64_t retype;
    uint32_t alpha_func;
    float alpha_ref;
    // [DOS2] VK-NATIVE stage 5 rest (vkn_warm.h "technique pipelines"): the per-draw vertex module's id (vkn_fx_out.vin.vs_id, key w[2]
    // bits 48..63; 0 = none). With `shader` set for an effect's build too, the warm-up lists it by the piece's recipe.
    uint16_t vs_id;
} vkn_pipe_src;
// VK_NULL_HANDLE: the primitive needs a rewrite (counted), or the build failed (counted, cached), or no device. src may be NULL on
// a lookup that must not build (returns VK_NULL_HANDLE on a miss).
VkPipeline vkn_pipe_get(const vkn_pipe_key *k, const vkn_pipe_src *src);
VkPipeline vkn_pipe_compute(uint32_t shader, VkShaderModule module, VkPipelineLayout layout);
// The shader piece calls this when a shader id dies: its graphics and compute pipelines are retired and leave the cache.
void vkn_pipe_forget_shader(uint32_t shader);

// ---- census ----
typedef struct vkn_state_stats {
    uint32_t blend_ids, ds_ids, raster_ids, vfmt_ids;          // contents
    uint32_t blend_live, ds_live, raster_live, vfmt_live;      // engine handles alive
    uint64_t puts, kills, bad;                                 // create events, destroy events, rejected arguments / full tables
    uint32_t passes, compat, framebuffers;
    uint64_t fb_retired;
    uint32_t pipelines, compute;
    uint64_t hits, misses, failures, rewrites, build_ns;
} vkn_state_stats;
void vkn_state_stats_get(vkn_state_stats *s);

// ==== [DOS2] VK-NATIVE stage 4: pipeline warm-up support (callers: vkn_warm.c) ====
// The VkPipelineCache: created once, from `data` (a vkGetPipelineCacheData blob the caller validated; NULL / 0 = empty). 0 = created
// by this call, 1 = it existed already (data ignored), -1 = no device / failed. Without the warm-up the first lookup creates it empty.
int vkn_pipe_cache_init(const void *data, size_t bytes);
VkPipelineCache vkn_pipe_cache_get(void);          // VK_NULL_HANDLE until created
uint64_t vkn_pipe_compiles(void);                  // builds of >= 2 ms so far (render thread + worker): real compiles, the cache grew
// A build outside the table (the warm-up worker; any thread). VK_NULL_HANDLE: the primitive needs a rewrite / no device / failed.
// *ns (non-NULL) gets the build time. Counted as a worker build, not as a miss.
VkPipeline vkn_pipe_build(const vkn_pipe_desc *d, const vkn_pipe_src *src, uint64_t *ns);
uint32_t vkn_pipe_forget_gen(void);                // + 1 at every vkn_pipe_forget_shader (a build that saw it change may be stale)
// Engine argument tuples behind a content id (what the state was created with, normalised: re-putting them gives the same id).
// blend: src_rgb, dst_rgb, src_a, dst_a, alpha_op, flags; ds: the 8 arguments of vkn_ds_put; raster: the 5 of vkn_raster_put. 0 = ok.
int vkn_blend_args(uint16_t id, uint8_t out[6]);
int vkn_ds_args(uint16_t id, uint8_t out[8]);
int vkn_raster_args(uint16_t id, uint8_t out[5]);
const uint8_t *vkn_vfmt_elems(uint16_t id, uint32_t *count);   // the 6-byte engine elements (immutable), NULL = no such id
// Render pass compatibility class: its content (VkFormats per colour slot, colour slot count, depth VkFormat, samples), and the class
// of a content (an existing one, else made through a pass with LOAD / STORE ops). 0 = ok / 0 = failure.
int vkn_rp_compat_info(uint16_t compat, uint32_t color[8], uint8_t *ncolor, uint32_t *depth, uint8_t *samples);
uint16_t vkn_rp_compat_make(const uint32_t color[8], uint8_t ncolor, uint32_t depth, uint8_t samples);
typedef struct vkn_state_wstats { uint64_t warm_hits, warm_builds, warm_build_ns, stale_builds, compiles; } vkn_state_wstats;
void vkn_state_wstats_get(vkn_state_wstats *s);
// ==== [stage 5.1 G] the handle generation (vkn_draw.h "stage 5 draw cost": the state-id cache of DM_VK_NATIVE_COST bit LOOKUP) ====
// + 1 (atomic, under the state mutex) at every handle -> id write: every put of a blend / depth-stencil / raster state or a vertex
// format (a put of a handle also ends the older generation of its index) and every kill. While it is unchanged, vkn_blend_id /
// vkn_ds_id / vkn_raster_id / vkn_vfmt_id give every handle the same answer. A plain atomic load; read it BEFORE the lookups.
uint32_t vkn_state_handle_gen(void);

#ifdef __cplusplus
}
#endif
#endif
