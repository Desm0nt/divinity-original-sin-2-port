// [DOS2] VK-NATIVE shaders (vkn_shader.c): one native object per engine shader handle (CreateShader). Its stage modules come from OUR
// SPIR-V packs through a narrow door (the bundle with every load-time rewrite applied, xbg_vkn_stage of xbg_game.h): in mode 1 the backend's
// shader library (vkn_shlib.h, DM_VK_NATIVE_SHLIB, default on there), otherwise the translator's xbg_vkn_shader;
// this module adds the descriptor-set layout, the pipeline layout and a compact binding table for the draw path. No game addresses:
// dm/host/hle_dos2_vkn_shader.c reads the engine's records and passes plain pointers.
//
// Pack facts (bundles of work/shaders-vk/shaders.xgspack, 12329 of them, scanned 20.11): one descriptor set, Vulkan binding = original
// binding x 6 + bundle stage (bundle stages: 0 vertex, 4 fragment, 5 compute), descriptor count 1 everywhere. Sources DOS2 uses:
//   SUPPORT  every vertex / fragment / compute bundle, one each: the translator's 1392-byte block of the viewport (xbg_support_defaults:
//            1/w, 1/h at byte 144, w, h at 160, 73 x 1.0f from 192, the rest 0). The draw path binds vkn_shader_support(viewport w, h).
//   UNIFORM  engine constant blocks: slot = the block's binding index (block byte +0x35); slots 0..7 are the shader's own blocks
//            (record arrays per stage, FlushShaderConstants 0x925740), 8..13 the global blocks (FlushGlobalConstants 0x926f90).
//   C1       the shader's own constants (bundle bytes): this module copies them once into a static block (vkn_shader.c1[]).
//   TEXTURE  combined image sampler, slot = engine texture slot 0..25 of the stage, sampler from the slot (sampler_slot is none in
//            every bundle: the sampler of the texture handle the slot holds).
//   IMAGE    storage image, compute only (5 bundles), slot = image slot.
// Not used by any DOS2 bundle: STORAGE, DRIVER0 (the translator's storage-buffer address table), DRIVER2 (its texture / sampler handle
// table), the ETERNAL bindless kinds. They still get a table kind (VKN_BK_STORAGE / VKN_BK_DRIVER0 / VKN_BK_DRIVER2) and are counted.
#ifndef DM_VKN_SHADER_H
#define DM_VKN_SHADER_H
#include "vkn.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_STAGE_VS = 0, VKN_STAGE_FS = 1, VKN_STAGE_CS = 5 };   // the engine's stage numbers (NVN: BindUniformBuffer / BindTexture)
enum { VKN_SRC_VS, VKN_SRC_FS, VKN_SRC_CS, VKN_SRCS };          // index of src[] / module[] / key[]

// One stage of an engine shader as the engine built it (CreateStageProgram 0x919090 of the blob CreateShader got).
typedef struct vkn_stage_src {
    const void *control;        // the stage's control section, >= 0xe48 readable bytes during the call (copied); NULL = no stage
    const void *code;           // the code area (control +0x704 bytes long): hashed during the call; read again only for a missing stage's
                                // dump, while the creation is queued (a destroy of the handle cancels it first)
    const char *name;           // the engine file of the shader (LoadShaderFile path, ".bshd"), NULL = not known: for the red lines only
    uint32_t engine_ubo_mask;   // bit 31 set = read: then bit b (< 31) = the engine record has a constant block of this stage with binding index b
} vkn_stage_src;

// Binding kinds of the table (from the bundle's source).
enum { VKN_BK_UNIFORM = 1, VKN_BK_SUPPORT, VKN_BK_C1, VKN_BK_TEXTURE, VKN_BK_IMAGE, VKN_BK_STORAGE, VKN_BK_DRIVER0, VKN_BK_DRIVER2, VKN_BK_OTHER };
enum { VKN_NO_DYN = 0xff };
// One Vulkan binding of set 0, in binding order (the order of the dynamic offsets).
typedef struct vkn_bind {
    uint16_t binding;           // Vulkan binding number
    uint8_t kind;               // VKN_BK_*
    uint8_t stage;              // engine stage: 0 vertex, 1 fragment, 5 compute
    uint8_t slot;               // engine slot: uniform block binding index (byte +0x35) / texture slot / image slot / storage slot; 0 for SUPPORT / C1
    uint8_t count;              // descriptor count
    uint8_t dyn;                // index of its first element in the dynamic-offset list, VKN_NO_DYN = a static descriptor
    uint8_t bundle_stage;       // 0 vertex, 4 fragment, 5 compute
    VkDescriptorType type;      // as in the set layout (uniform buffers: UNIFORM_BUFFER_DYNAMIC unless the dynamic limit forbids)
    uint32_t bytes;             // uniform buffers: the bytes the SPIR-V may read (0 = up to 64 KiB); the range is min(block size, this)
    uint32_t flags;             // xbg_shader_binding flags (images: bit31 shape present, dim bits 8..10, array 12, MS 13, depth 14, scalar 16..17)
                                // [stage 5 A1] bit 30 = VKN_BF_CMP (vkn_fx.h): a texture binding the named Dref module samples with a
                                // depth comparison (vkn_shlib_named.h); the draw path binds the slot sampler's comparison twin
} vkn_bind;

// Dynamic-offset policy of a shader (the translator's dyn_mode_for, by maxDescriptorSetUniformBuffersDynamic):
// 1 every uniform buffer dynamic; 2 only UNIFORM (engine blocks) dynamic, SUPPORT / C1 static; 0 none dynamic.
typedef struct vkn_shader {
    uint64_t handle;            // engine handle
    uint32_t id;                // small dense id >= 1 for pipeline keys (reused only after the record is retired; see vkn_shader_set_dead_fn)
    uint8_t stages;             // bit VKN_SRC_*: the engine shader has this stage
    uint8_t missing;            // bit VKN_SRC_*: no bundle for the stage's key (its draws must be skipped, as the translator does)
    uint8_t unusable;           // bit VKN_SRC_*: the bundle needs a capability the device lacks (no module; skip)
    uint8_t dyn_policy;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkShaderModule module[VKN_SRCS];   // base modules (owned by the loader, live until the process ends)
    void *xs[VKN_SRCS];         // the loader's shader objects (variant getters)
    char key[VKN_SRCS][17];     // program keys ("" = no stage / no key)
    const vkn_bind *bind; uint16_t nbind, ndyn;
    uint8_t ninputs; uint8_t inputs[16], input_classes[16];   // vertex: input locations, scalar class (0 float, 1 sint, 2 uint)
    vkn_block c1[VKN_SRCS];     // XBG_SOURCE_C1 data per stage in the static arena (size 0 = none); bind (buf, off, size)
} vkn_shader;

// Creates (or replaces: an older record under the same index is retired) the object of an engine shader. src[VKN_SRC_*]; a stage
// without control is absent. Thread safe (engine loader threads). The stage keys are computed here; the rest (bundle loads through the
// translator, layouts) is queued to a private worker thread with a big stack and returns at once. 0 = queued, -1 = no device /
// translator / memory.
int vkn_shader_create(uint64_t handle, const vkn_stage_src src[VKN_SRCS]);
// The object of this very handle (generation included), NULL = none; waits for its creation when that is still queued. Valid at
// least 3 frames after its destroy (vkn_retire).
const vkn_shader *vkn_shader_get(uint64_t handle);
void vkn_shader_sync(void);   // waits until every queued creation is done
// The module for a pipeline: stage VKN_STAGE_VS / FS / CS. alpha_func: NVN compare func 1 NEVER .. 8 ALWAYS (0 or 8 = no test) with
// alpha_ref: the fragment variant with the alpha test ("#at"); retype: the vertex variant for this retype code ("#vi": low 32 bits
// 2 bits per input location, 0 keep / 1 float / 2 sint / 3 uint; high 32 bits 1 = SSCALED fed as SINT, 2 = USCALED fed as UINT; 0 = none).
// Variants are built once by the loader and cached there. VK_NULL_HANDLE = no such stage / unusable / missing.
VkShaderModule vkn_shader_module(const vkn_shader *s, int stage, uint32_t alpha_func, float alpha_ref, uint64_t retype);
// The engine destroyed the shader (DestroyShaderIfOld returned true): the record leaves the table, its objects go through vkn_retire.
void vkn_shader_destroy(uint64_t handle);
// Called (any thread, outside every lock of this module) for each destroyed or replaced record, before it is retired: the pipeline cache
// drops what it built with this id. One listener.
void vkn_shader_set_dead_fn(void (*fn)(uint32_t id, uint64_t handle));
// The SUPPORT block for a viewport size (1392 bytes, uniform buffer, static, cached per size). 0 = ok.
int vkn_shader_support(float width, float height, vkn_block *out);

typedef struct vkn_shader_stats {
    uint64_t alive, created, replaced, destroyed, failed, cancelled, queued;
    uint64_t stages, stages_bundle, stages_missing, stages_unusable, stage_mismatch;
    uint64_t layouts, layouts_own, table_bytes, c1_bytes;
    uint64_t dyn_policy[3];                   // shaders per policy 0 / 1 / 2
    uint64_t bind_kind[VKN_BK_OTHER + 1];     // bindings per kind, of every created record
    uint64_t ubo_no_block;                    // UNIFORM bindings (slot < 8) the engine record has no block for
    uint64_t ns;                              // time spent: keys on the engine's threads + creation on the worker
} vkn_shader_stats;
void vkn_shader_stats_get(vkn_shader_stats *out);
void vkn_shader_census(void);   // one line (and red lines for what is wrong), through vkn_logf

// ==== [DOS2] VK-NATIVE stage 4 (pipeline warm-up, vkn_warm.c): lookups the replay of the pipeline list needs ====
// fn(record, ctx) for every live record, under this module's mutex (fn must be quick and must not call into this module); a non-zero
// return stops the walk. Returns the records visited. A record pointer stays valid at least 3 frames after its destroy (vkn_retire).
uint32_t vkn_shader_each(int (*fn)(const vkn_shader *s, void *ctx), void *ctx);
// The live record whose vertex / fragment program keys are these ("" = no such stage), NULL = none (a walk of the table).
const vkn_shader *vkn_shader_find_keys(const char *vs_key, const char *fs_key);
uint64_t vkn_shader_epoch(void);   // + 1 for every record that entered the table (a new shader may be there)

// ==== [DOS2] VK-NATIVE stage 5.1 G (vkn_draw.h "stage 5 draw cost": the vertex fold, DM_VK_NATIVE_COST bit VFOLD) ====
// 1 = the vertex stage of s (every module the draw path can use for it: the base module and its "#vi" retype variants) reads BuiltIn
// VertexIndex / VertexId / BaseVertex, which include vertexOffset / firstVertex (changed by the fold), or that is not known: a piece's
// shader (vkn_fx_shader_is), the translator's loader (DM_VK_NATIVE_SHLIB=0 / mode 2), no vertex object, NULL. 0 = known not to read them
// (the shader library's scan of the final words, vkn_shlib_vs_vindex). Lock-free; any thread.
int vkn_shader_vindex(const vkn_shader *s);

#ifdef __cplusplus
}
#endif
#endif
