// [DOS2] VK-NATIVE state piece (vkn_state.h): state objects, vertex formats, write masks, primitives, render passes, framebuffers,
// pipeline cache. No game addresses, no NVN objects: the enum tables below are the engine's (read in main.raw, VK-NATIVE-MAP-1.md §3)
// expressed directly as Vulkan values.
#ifdef DM_GAME_DOS2
#include "vkn.h"
#include "vkn_state.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define VKN_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"

// ================================================================ state objects ================================================

// Engine blend factor 0..10 -> Vulkan (0x922530 via table 0x1f585fc = NVN 2,3,4,9,10,5,6,7,8,11 for 1..10; 0 and >10 -> NVN zero).
static VkBlendFactor blend_factor(uint8_t v) {
    static const VkBlendFactor t[11] = {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    return v <= 10 ? t[v] : VK_BLEND_FACTOR_ZERO;
}
// Engine compare function 0..7 -> Vulkan: NVN = n + 1 for 1..7, NEVER otherwise (0x922d10 / 0x921e40); VkCompareOp has the same order.
static VkCompareOp compare_op(uint8_t v) { return v <= 7 ? (VkCompareOp)v : VK_COMPARE_OP_NEVER; }
// Engine stencil op 1..7 -> Vulkan (table 0x1f58624 = NVN zero, replace, invert, incr wrap, decr wrap, incr, decr; else keep).
static VkStencilOp stencil_op(uint8_t v) {
    static const VkStencilOp t[8] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INVERT,
        VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP, VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP};
    return v <= 7 ? t[v] : VK_STENCIL_OP_KEEP;
}

enum { K_BLEND, K_DS, K_RASTER, K_VFMT, K_KINDS };
enum { VKN_STATE_HANDLES = 4096 };   // engine handle indices per kind (the engine's tables hold tens)
typedef struct { uint16_t id; } hrec;

typedef struct { VkPipelineColorBlendAttachmentState att; uint8_t args[7]; } blend_c;
typedef struct { VkPipelineDepthStencilStateCreateInfo info; uint8_t args[8]; } ds_c;
typedef struct { VkPipelineRasterizationStateCreateInfo info; uint8_t args[5]; } raster_c;
typedef struct { vkn_vfmt f; uint8_t *el; uint32_t count; } vfmt_c;

static pthread_mutex_t g_st_mu = PTHREAD_MUTEX_INITIALIZER;
static vkn_tab g_htab[K_KINDS]; static int g_htab_ok;
static blend_c g_blend[VKN_STATE_IDS]; static uint32_t g_nblend;
static ds_c g_ds[VKN_STATE_IDS]; static uint32_t g_nds;
static raster_c g_raster[VKN_STATE_IDS]; static uint32_t g_nraster;
static vfmt_c g_vfmt[VKN_VFMT_IDS]; static uint32_t g_nvfmt;
static uint64_t g_st_puts, g_st_kills, g_st_bad;
// [stage 5.1 G] vkn_state_handle_gen (vkn_state.h): + 1 under g_st_mu at every handle -> id write (hmap, hkill), read atomically
static uint32_t g_handle_gen;
uint32_t vkn_state_handle_gen(void) { return __atomic_load_n(&g_handle_gen, __ATOMIC_ACQUIRE); }
// Depth bias (0x93d9f0 sets these every frame; constants in this build).
static float g_bias_factor = -2.5f, g_bias_units = -5.0f, g_bias_clamp = 0.0f, g_bias_units_x = 1.0f;

static int htab_ready(void) {   // under g_st_mu
    if (g_htab_ok) return g_htab_ok > 0;
    g_htab_ok = 1;
    for (int k = 0; k < K_KINDS; k++)
        if (vkn_tab_init(&g_htab[k], VKN_STATE_HANDLES, sizeof(hrec))) { g_htab_ok = -1; vkn_logf(VKN_RED "state: handle tables: out of memory"); break; }
    return g_htab_ok > 0;
}
// Maps the engine handle to the content id (under g_st_mu). A live record under another generation is a replacement: just overwritten.
static void hmap(int kind, uint64_t h, uint16_t id) {
    if (!h || !id || !htab_ready()) return;
    hrec *r = (hrec *)vkn_tab_put(&g_htab[kind], h, NULL);
    if (!r) { g_st_bad++; static int noted; if (!noted++) vkn_logf(VKN_RED "state: engine handle index %u beyond %u: not mapped", vkn_h_idx(h), (unsigned)VKN_STATE_HANDLES); return; }
    r->id = id; g_st_puts++;
    __atomic_add_fetch(&g_handle_gen, 1, __ATOMIC_RELEASE);   // [stage 5.1 G] (a put also ends the older generation of this index)
}
static uint16_t hid(int kind, uint64_t h) {
    uint16_t id = 0;
    pthread_mutex_lock(&g_st_mu);
    if (g_htab_ok > 0) { const hrec *r = (const hrec *)vkn_tab_get(&g_htab[kind], h); if (r) id = r->id; }
    pthread_mutex_unlock(&g_st_mu);
    return id;
}
static int hkill(int kind, uint64_t h) {
    int was = 0;
    pthread_mutex_lock(&g_st_mu);
    if (g_htab_ok > 0) { hrec *r = (hrec *)vkn_tab_get(&g_htab[kind], h); if (r) r->id = 0; was = vkn_tab_kill(&g_htab[kind], h); if (was) g_st_kills++; }
    __atomic_add_fetch(&g_handle_gen, 1, __ATOMIC_RELEASE);   // [stage 5.1 G] every kill (blend / depth-stencil / raster / vertex format)
    pthread_mutex_unlock(&g_st_mu);
    return was;
}
static void full_note(const char *what, uint32_t cap) {
    static int noted[K_KINDS + 1]; const int i = what[0] == 'b' ? 0 : what[0] == 'd' ? 1 : what[0] == 'r' ? 2 : 3;
    g_st_bad++; if (!noted[i]++) vkn_logf(VKN_RED "state: more than %u distinct %s states: the rest unmapped (draws with them get no pipeline)", cap, what);
}

uint16_t vkn_blend_put(uint64_t h, uint8_t src_rgb, uint8_t dst_rgb, uint8_t src_a, uint8_t dst_a, uint8_t alpha_op, uint32_t flags) {
    // Only what changes the result is the content: without the separate bit, alpha arguments are ignored by the engine.
    const int sep = flags & 1;
    const uint8_t args[7] = {src_rgb, dst_rgb, sep ? src_a : 0, sep ? dst_a : 0, sep ? alpha_op : 0, (uint8_t)sep, 0};
    uint16_t id = 0;
    pthread_mutex_lock(&g_st_mu);
    for (uint32_t i = 0; i < g_nblend && !id; i++) if (!memcmp(g_blend[i].args, args, sizeof args)) id = (uint16_t)(i + 1);
    if (!id && g_nblend < VKN_STATE_IDS) {
        blend_c *c = &g_blend[g_nblend]; memcpy(c->args, args, sizeof args);
        VkPipelineColorBlendAttachmentState *a = &c->att;
        a->srcColorBlendFactor = blend_factor(src_rgb); a->dstColorBlendFactor = blend_factor(dst_rgb); a->colorBlendOp = VK_BLEND_OP_ADD;
        if (sep) {
            a->srcAlphaBlendFactor = blend_factor(src_a); a->dstAlphaBlendFactor = blend_factor(dst_a);
            a->alphaBlendOp = alpha_op >= 1 && alpha_op <= 4 ? (VkBlendOp)alpha_op : VK_BLEND_OP_ADD;   // NVN 2..5 = sub, rev sub, min, max
        } else { a->srcAlphaBlendFactor = a->srcColorBlendFactor; a->dstAlphaBlendFactor = a->dstColorBlendFactor; a->alphaBlendOp = VK_BLEND_OP_ADD; }
        // One / zero / add on both: the same as no blending (and valid on targets without blend support).
        const int off = a->srcColorBlendFactor == VK_BLEND_FACTOR_ONE && a->dstColorBlendFactor == VK_BLEND_FACTOR_ZERO && a->srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE &&
                        a->dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO && a->colorBlendOp == VK_BLEND_OP_ADD && a->alphaBlendOp == VK_BLEND_OP_ADD;
        a->blendEnable = off ? VK_FALSE : VK_TRUE;
        a->colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        __atomic_store_n(&g_nblend, g_nblend + 1, __ATOMIC_RELEASE); id = (uint16_t)g_nblend;
    } else if (!id) full_note("blend", VKN_STATE_IDS);
    hmap(K_BLEND, h, id);
    pthread_mutex_unlock(&g_st_mu);
    return id;
}

uint16_t vkn_ds_put(uint64_t h, uint8_t depth_test, uint8_t depth_write, uint8_t depth_func, uint8_t stencil_enable,
                    uint8_t stencil_mask, uint8_t stencil_func, uint8_t depth_fail_op, uint8_t pass_op) {
    const uint8_t args[8] = {(uint8_t)(depth_test & 1), (uint8_t)(depth_write & 1), (uint8_t)compare_op(depth_func), (uint8_t)(stencil_enable & 1),
                             stencil_mask, (uint8_t)compare_op(stencil_func), (uint8_t)stencil_op(depth_fail_op), (uint8_t)stencil_op(pass_op)};
    uint16_t id = 0;
    pthread_mutex_lock(&g_st_mu);
    for (uint32_t i = 0; i < g_nds && !id; i++) if (!memcmp(g_ds[i].args, args, sizeof args)) id = (uint16_t)(i + 1);
    if (!id && g_nds < VKN_STATE_IDS) {
        ds_c *c = &g_ds[g_nds]; memcpy(c->args, args, sizeof args);
        VkPipelineDepthStencilStateCreateInfo *d = &c->info;
        d->sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        d->depthTestEnable = args[0]; d->depthWriteEnable = args[1]; d->depthCompareOp = (VkCompareOp)args[2];
        d->stencilTestEnable = args[3];
        VkStencilOpState s = {VK_STENCIL_OP_KEEP, (VkStencilOp)args[7], (VkStencilOp)args[6], (VkCompareOp)args[5], stencil_mask, stencil_mask, 0};
        d->front = s; d->back = s;   // NVN face 3 = front and back
        d->minDepthBounds = 0.0f; d->maxDepthBounds = 1.0f;
        __atomic_store_n(&g_nds, g_nds + 1, __ATOMIC_RELEASE); id = (uint16_t)g_nds;
    } else if (!id) full_note("depth-stencil", VKN_STATE_IDS);
    hmap(K_DS, h, id);
    pthread_mutex_unlock(&g_st_mu);
    return id;
}

static void raster_bias(VkPipelineRasterizationStateCreateInfo *r) {   // under g_st_mu
    r->depthBiasSlopeFactor = r->depthBiasEnable ? g_bias_factor : 0.0f;
    r->depthBiasConstantFactor = r->depthBiasEnable ? g_bias_units * g_bias_units_x : 0.0f;
    r->depthBiasClamp = r->depthBiasEnable ? g_bias_clamp : 0.0f;
    const vkn_dev *d = vkn_device();
    if (r->depthBiasClamp != 0.0f && d && !d->feat.depthBiasClamp) r->depthBiasClamp = 0.0f;
}
uint16_t vkn_raster_put(uint64_t h, uint8_t cull, uint8_t fill, uint8_t flag_a, uint8_t flag_b, uint32_t polygon_offset) {
    // flag A / B are part of the identity (the engine keeps them) but change nothing in Vulkan.
    const uint8_t args[5] = {(uint8_t)(cull == 1 ? 1 : cull == 2 ? 2 : 0), (uint8_t)(fill == 0 ? 0 : fill == 1 ? 1 : 2), (uint8_t)(flag_a & 1),
                             (uint8_t)(flag_b & 1), (uint8_t)(polygon_offset & 1)};
    uint16_t id = 0;
    pthread_mutex_lock(&g_st_mu);
    for (uint32_t i = 0; i < g_nraster && !id; i++) if (!memcmp(g_raster[i].args, args, sizeof args)) id = (uint16_t)(i + 1);
    if (!id && g_nraster < VKN_STATE_IDS) {
        raster_c *c = &g_raster[g_nraster]; memcpy(c->args, args, sizeof args);
        VkPipelineRasterizationStateCreateInfo *r = &c->info;
        r->sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        r->polygonMode = args[1] == 0 ? VK_POLYGON_MODE_FILL : args[1] == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_POINT;
        r->cullMode = args[0] == 1 ? VK_CULL_MODE_BACK_BIT : args[0] == 2 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
        r->frontFace = VK_FRONT_FACE_CLOCKWISE;   // nvnPolygonStateSetFrontFace(0) = CW; origin upper left in both APIs
        r->depthBiasEnable = args[4];
        r->lineWidth = 1.0f;
        raster_bias(r);
        __atomic_store_n(&g_nraster, g_nraster + 1, __ATOMIC_RELEASE); id = (uint16_t)g_nraster;
    } else if (!id) full_note("raster", VKN_STATE_IDS);
    hmap(K_RASTER, h, id);
    pthread_mutex_unlock(&g_st_mu);
    return id;
}
static int g_bias_pipes;   // pipelines built so far (a bias change after the first one is logged)
void vkn_state_set_depth_bias(float factor, float units, float clamp, float units_x) {
    pthread_mutex_lock(&g_st_mu);
    const int same = factor == g_bias_factor && units == g_bias_units && clamp == g_bias_clamp && units_x == g_bias_units_x;
    if (!same) {
        if (__atomic_load_n(&g_bias_pipes, __ATOMIC_RELAXED)) { static int noted; if (!noted++) vkn_logf("state: depth bias changed to %g / %g / %g x%g after pipelines were built: only new ones get it", factor, units, clamp, units_x); }
        g_bias_factor = factor; g_bias_units = units; g_bias_clamp = clamp; g_bias_units_x = units_x;
        for (uint32_t i = 0; i < g_nraster; i++) raster_bias(&g_raster[i].info);
    }
    pthread_mutex_unlock(&g_st_mu);
}

int vkn_blend_kill(uint64_t h) { return hkill(K_BLEND, h); }
int vkn_ds_kill(uint64_t h) { return hkill(K_DS, h); }
int vkn_raster_kill(uint64_t h) { return hkill(K_RASTER, h); }
int vkn_vfmt_kill(uint64_t h) { return hkill(K_VFMT, h); }
uint16_t vkn_blend_id(uint64_t h) { return hid(K_BLEND, h); }
uint16_t vkn_ds_id(uint64_t h) { return hid(K_DS, h); }
uint16_t vkn_raster_id(uint64_t h) { return hid(K_RASTER, h); }
uint16_t vkn_vfmt_id(uint64_t h) { return hid(K_VFMT, h); }
// Contents are written once before their id is published (under the mutex) and never change afterwards, except the raster bias
// floats (vkn_state_set_depth_bias; the pipeline build copies them under the mutex).
const VkPipelineColorBlendAttachmentState *vkn_blend_att(uint16_t id) { return id && id <= __atomic_load_n(&g_nblend, __ATOMIC_ACQUIRE) ? &g_blend[id - 1].att : NULL; }
const VkPipelineDepthStencilStateCreateInfo *vkn_ds_info(uint16_t id) { return id && id <= __atomic_load_n(&g_nds, __ATOMIC_ACQUIRE) ? &g_ds[id - 1].info : NULL; }
const VkPipelineRasterizationStateCreateInfo *vkn_raster_info(uint16_t id) { return id && id <= __atomic_load_n(&g_nraster, __ATOMIC_ACQUIRE) ? &g_raster[id - 1].info : NULL; }
const vkn_vfmt *vkn_vfmt_info(uint16_t id) { return id && id <= __atomic_load_n(&g_nvfmt, __ATOMIC_ACQUIRE) ? &g_vfmt[id - 1].f : NULL; }

// ================================================================ vertex formats ===============================================

// Component type 0..11 x count 1..4 -> Vulkan (0x9181d0: NVN R32F.. / R32UI.. / R32I.. / R16F.. / R16.. / R16UI.. / R16SN.. / R16I.. /
// R8.. / R8UI.. / R8SN.. / R8I..; sizes 4n, 4n, 4n, 2n x5, n x4).
static const VkFormat k_vtx[12][4] = {
    {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT},
    {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT},
    {VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT},
    {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT},
    {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM},
    {VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT},
    {VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM},
    {VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT},
    {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM},
    {VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT},
    {VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM},
    {VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT},
};
static const uint8_t k_vtx_size[12] = {4, 4, 4, 2, 2, 2, 2, 2, 1, 1, 1, 1};
static const uint8_t k_vtx_cls[12] = {VKN_CLS_FLOAT, VKN_CLS_UINT, VKN_CLS_SINT, VKN_CLS_FLOAT, VKN_CLS_FLOAT, VKN_CLS_UINT, VKN_CLS_FLOAT, VKN_CLS_SINT,
                                      VKN_CLS_FLOAT, VKN_CLS_UINT, VKN_CLS_FLOAT, VKN_CLS_SINT};
static int vtx_format_ok(VkFormat f) {   // vertex-buffer support (3-component 8 / 16-bit formats are optional in Vulkan)
    const vkn_dev *d = vkn_device();
    if (!d) return 1;
    VkFormatProperties p; memset(&p, 0, sizeof p);
    vkGetPhysicalDeviceFormatProperties(d->phys, f, &p);
    return (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
}
static int usage_location(uint8_t usage, uint8_t index) {   // 0x9181d0
    switch (usage) {
    case 2: return 4 + index;
    case 3: return 1;
    case 4: return 3;
    case 5: return 2;
    case 6: return 14;
    case 7: return 15;
    case 8: return 9 + index;
    default: return 0;
    }
}
static void vfmt_build(vkn_vfmt *f, const uint8_t *el, uint32_t count) {
    memset(f, 0, sizeof *f);
    int inst[VKN_VFMT_STREAMS] = {0, 0, 0}, used[VKN_VFMT_STREAMS] = {0, 0, 0};
    VkVertexInputAttributeDescription at[VKN_VFMT_LOCS]; memset(at, 0, sizeof at);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = el + 6 * i;
        const uint8_t stream = e[0], type = e[4], n = e[5];
        if (stream >= VKN_VFMT_STREAMS) { static int noted; if (!noted++) vkn_logf(VKN_RED "vertex format: stream %u of an element: element ignored", stream); continue; }
        if (e[3] == 1) inst[stream] = 1;
        if (type >= 12) continue;                 // the engine sets no format and adds no stride
        const uint32_t off = f->stride[stream];
        f->stride[stream] += (uint32_t)k_vtx_size[type] * n;   // the engine adds size x count even for a count it has no format for
        if (n < 1 || n > 4) continue;
        const int loc = usage_location(e[1], e[2]);
        if (loc >= VKN_VFMT_LOCS) { static int noted; if (!noted++) vkn_logf(VKN_RED "vertex format: usage %u index %u -> location %d beyond 15: ignored", e[1], e[2], loc); continue; }
        VkFormat vf = k_vtx[type][n - 1];
        f->widened &= (uint16_t)~(1u << loc);
        if (n == 3 && k_vtx_size[type] < 4 && !vtx_format_ok(vf)) { vf = k_vtx[type][3]; f->widened |= (uint16_t)(1u << loc); }
        at[loc].location = (uint32_t)loc; at[loc].binding = stream; at[loc].format = vf; at[loc].offset = off;
        f->loc_mask |= (uint16_t)(1u << loc); f->loc_class[loc] = k_vtx_cls[type]; f->loc_comps[loc] = n;
    }
    for (int loc = 0; loc < VKN_VFMT_LOCS; loc++) if (f->loc_mask & (1u << loc)) { f->attr[f->nattr++] = at[loc]; used[at[loc].binding] = 1; }
    for (uint32_t s = 0; s < VKN_VFMT_STREAMS; s++)
        if (used[s]) {
            VkVertexInputBindingDescription *b = &f->bind[f->nbind++];
            b->binding = s; b->stride = f->stride[s]; b->inputRate = inst[s] ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
        }
}
uint16_t vkn_vfmt_put(uint64_t h, const uint8_t *elems, uint32_t count) {
    if (count > 255 || (count && !elems)) { pthread_mutex_lock(&g_st_mu); g_st_bad++; pthread_mutex_unlock(&g_st_mu); return 0; }
    uint16_t id = 0;
    pthread_mutex_lock(&g_st_mu);
    for (uint32_t i = 0; i < g_nvfmt && !id; i++)
        if (g_vfmt[i].count == count && (!count || !memcmp(g_vfmt[i].el, elems, 6u * count))) id = (uint16_t)(i + 1);
    if (!id && g_nvfmt < VKN_VFMT_IDS) {
        vfmt_c *c = &g_vfmt[g_nvfmt];
        c->el = count ? (uint8_t *)malloc(6u * count) : NULL;
        if (count && !c->el) { g_st_bad++; pthread_mutex_unlock(&g_st_mu); return 0; }
        if (count) memcpy(c->el, elems, 6u * count);
        c->count = count;
        vfmt_build(&c->f, elems, count);
        __atomic_store_n(&g_nvfmt, g_nvfmt + 1, __ATOMIC_RELEASE); id = (uint16_t)g_nvfmt;
    } else if (!id) full_note("vertex format", VKN_VFMT_IDS);
    hmap(K_VFMT, h, id);
    pthread_mutex_unlock(&g_st_mu);
    return id;
}

// ================================================================ write masks ==================================================

// 0x9282c0: nvnCommandBufferSetChannelMask(target, R, G, B, A) by nvnTextureGetFormat; formats outside the lists keep mask 0.
VkColorComponentFlags vkn_write_mask_nvn(uint32_t f) {
    const VkColorComponentFlags R = VK_COLOR_COMPONENT_R_BIT, RG = R | VK_COLOR_COMPONENT_G_BIT, RGB = RG | VK_COLOR_COMPONENT_B_BIT,
                                RGBA = RGB | VK_COLOR_COMPONENT_A_BIT;
    switch (f) {
    case 0x01: case 0x03: case 0x04: case 0x05: case 0x06: case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x32: case 0x33: case 0x34:
        return R;
    case 0x0d: case 0x0f: case 0x10: case 0x11: case 0x12: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x35:
        return RG;
    case 0x19: case 0x1d: case 0x1e: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x3f: case 0x95: case 0x99:
        return RGB;
    case 0x25: case 0x26: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: case 0x2e: case 0x2f: case 0x30: case 0x38: case 0x39:
    case 0x3b: case 0x3d: case 0x98: case 0x9a: case 0x9c:
        return RGBA;
    default:
        return 0;
    }
}
// Engine format 0..62 -> its NVN format (0x919c20 without sRGB; the sRGB partners 0x38 / 0x9c have the same mask). 0 = none / trap.
static const uint8_t k_engine_nvn[63] = {
    0x00, 0x01, 0x0d, 0x19, 0x25, 0x06, 0x12, 0x1e, 0x2a, 0x02, 0x0e, 0x1a, 0x26, 0x07, 0x13, 0x1f,   //  0..15
    0x2b, 0x05, 0x11, 0x1d, 0x29, 0x0a, 0x16, 0x22, 0x2e, 0x09, 0x15, 0x21, 0x2d, 0x0c, 0x18, 0x24,   // 16..31
    0x30, 0x08, 0x14, 0x20, 0x2c, 0x0b, 0x17, 0x23, 0x2f, 0x00, 0x40, 0x3f, 0x95, 0x39, 0x3d, 0x00,   // 32..47 (41 RGBE8 none, 47 trap)
    0x98, 0x32, 0x33, 0x35, 0x34, 0x41, 0x43, 0x44, 0x49, 0x4b, 0x50, 0x4d, 0x99, 0x9a, 0x00,         // 48..62 (62 trap)
};
VkColorComponentFlags vkn_write_mask_engine(uint32_t engine_format) { return engine_format < 63 ? vkn_write_mask_nvn(k_engine_nvn[engine_format]) : 0; }

// ================================================================ primitives ===================================================

int vkn_prim_topology(uint8_t prim, VkPrimitiveTopology *out) {
    switch (prim) {
    case 1: *out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return 0;
    case 3: *out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return 0;
    case 5: *out = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return 0;
    case 6: *out = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; return 0;
    case 2: case 4: case 7: return 1;                                  // fan, quads, line loop
    default: *out = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; return 0;       // 0x92c3c0: out of range -> NVN 0 = points
    }
}
uint8_t vkn_prim_rewrite(uint8_t prim) { return prim == 2 || prim == 4 ? 1 : prim == 7 ? 6 : prim; }

// ================================================================ open-addressing table ========================================

// Entries of `esz` bytes: {u64 value, u8 state, pad, key of ksz bytes}. State 0 empty, 1 full, 2 tombstone. The caller locks.
typedef struct { uint64_t val; uint8_t st, pad[7]; } ot_hdr;
typedef struct { uint8_t *e; uint32_t cap, live, tomb, ksz, esz; } otab;
static inline ot_hdr *ot_at(const otab *t, uint32_t i) { return (ot_hdr *)(t->e + (size_t)i * t->esz); }
static inline void *ot_key(ot_hdr *h) { return (uint8_t *)h + sizeof(ot_hdr); }
static int ot_init(otab *t, uint32_t cap, uint32_t ksz) {   // cap a power of two
    memset(t, 0, sizeof *t);
    t->ksz = ksz; t->esz = (uint32_t)((sizeof(ot_hdr) + ksz + 7) & ~7u); t->cap = cap;
    t->e = (uint8_t *)calloc(cap, t->esz);
    return t->e ? 0 : -1;
}
static ot_hdr *ot_find(const otab *t, const void *key) {
    if (!t->e) return NULL;
    for (uint32_t i = (uint32_t)vkn_fnv(key, t->ksz) & (t->cap - 1), n = 0; n < t->cap; i = (i + 1) & (t->cap - 1), n++) {
        ot_hdr *h = ot_at(t, i);
        if (h->st == 0) return NULL;
        if (h->st == 1 && !memcmp(ot_key(h), key, t->ksz)) return h;
    }
    return NULL;
}
static ot_hdr *ot_slot(otab *t, const void *key) {   // a free slot for a key known to be absent
    for (uint32_t i = (uint32_t)vkn_fnv(key, t->ksz) & (t->cap - 1);; i = (i + 1) & (t->cap - 1)) { ot_hdr *h = ot_at(t, i); if (h->st != 1) return h; }
}
static ot_hdr *ot_insert(otab *t, const void *key, uint64_t val) {
    if (!t->e) return NULL;
    if ((uint64_t)(t->live + t->tomb + 1) * 4 > (uint64_t)t->cap * 3) {   // grow (or just drop the tombstones)
        otab n; if (ot_init(&n, (uint64_t)(t->live + 1) * 2 > t->cap ? t->cap * 2 : t->cap, t->ksz)) return NULL;
        for (uint32_t i = 0; i < t->cap; i++) { ot_hdr *h = ot_at(t, i); if (h->st == 1) { ot_hdr *d = ot_slot(&n, ot_key(h)); memcpy(d, h, t->esz); n.live++; } }
        free(t->e); *t = n;
    }
    ot_hdr *h = ot_slot(t, key);
    if (h->st == 2) t->tomb--;
    h->st = 1; h->val = val; memcpy(ot_key(h), key, t->ksz); t->live++;
    return h;
}
static void ot_remove(otab *t, ot_hdr *h) { h->st = 2; t->live--; t->tomb++; }

// ================================================================ render passes ================================================

enum { VKN_COMPAT_MAX = 255, VKN_RP_MAX = 4095 };
typedef struct { uint32_t color[8]; uint32_t depth; uint8_t ncolor, samples, pad[2]; } compat_k;
typedef struct { compat_k k; VkRenderPass pass; } compat_e;
static pthread_mutex_t g_rp_mu = PTHREAD_MUTEX_INITIALIZER;
static otab g_rp_tab;            // canonical vkn_rp_key -> pass id
static VkRenderPass g_rp[VKN_RP_MAX]; static uint16_t g_rp_compat[VKN_RP_MAX]; static uint32_t g_nrp;
static compat_e g_compat[VKN_COMPAT_MAX]; static uint32_t g_ncompat;
static int g_rp_ok;
// Framebuffer key: views in attachment order (VkImageView as u64), extent, layers, compatibility class.
typedef struct { uint64_t view[9]; uint32_t w, h, layers; uint16_t compat; uint8_t nviews, pad; } fb_k;
static otab g_fb_tab;            // fb_k -> VkFramebuffer
static uint64_t g_fb_retired;

static int fmt_has_stencil(uint32_t f) {
    return f == VK_FORMAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}
static int fmt_has_depth(uint32_t f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D16_UNORM_S8_UINT ||
           f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}
void vkn_rp_key_init(vkn_rp_key *k) { memset(k, 0, sizeof *k); k->samples = VK_SAMPLE_COUNT_1_BIT; }
// The canonical key: nothing that cannot matter differs (unbound slots, ops of an absent aspect), so equal passes share one entry.
static void rp_canon(vkn_rp_key *c, const vkn_rp_key *k) {
    *c = *k; memset(c->pad, 0, sizeof c->pad);
    if (c->ncolor > 8) c->ncolor = 8;
    if (!c->samples) c->samples = VK_SAMPLE_COUNT_1_BIT;
    for (int i = 0; i < 8; i++)
        if (i >= c->ncolor || !c->color[i]) { c->color[i] = 0; c->load[i] = c->store[i] = 0; }
        else { if (c->load[i] > VK_ATTACHMENT_LOAD_OP_DONT_CARE) c->load[i] = VK_ATTACHMENT_LOAD_OP_DONT_CARE; if (c->store[i] > VK_ATTACHMENT_STORE_OP_DONT_CARE) c->store[i] = VK_ATTACHMENT_STORE_OP_DONT_CARE; }
    while (c->ncolor && !c->color[c->ncolor - 1]) c->ncolor--;
    if (!c->depth) c->depth_load = c->depth_store = c->stencil_load = c->stencil_store = 0;
    else {
        if (!fmt_has_depth(c->depth)) { c->depth_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE; c->depth_store = VK_ATTACHMENT_STORE_OP_DONT_CARE; }
        if (!fmt_has_stencil(c->depth)) { c->stencil_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE; c->stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE; }
        if (c->depth_load > 2) c->depth_load = 2; if (c->stencil_load > 2) c->stencil_load = 2;
        if (c->depth_store > 1) c->depth_store = 1; if (c->stencil_store > 1) c->stencil_store = 1;
    }
}
static VkRenderPass rp_create(const vkn_rp_key *k) {
    const vkn_dev *d = vkn_device();
    if (!d) return VK_NULL_HANDLE;
    VkAttachmentDescription at[9]; VkAttachmentReference cref[8], dref; uint32_t na = 0;
    memset(at, 0, sizeof at);
    for (uint32_t i = 0; i < k->ncolor; i++) {
        if (!k->color[i]) { cref[i].attachment = VK_ATTACHMENT_UNUSED; cref[i].layout = VK_IMAGE_LAYOUT_UNDEFINED; continue; }
        VkAttachmentDescription *a = &at[na];
        a->format = (VkFormat)k->color[i]; a->samples = (VkSampleCountFlagBits)k->samples;
        a->loadOp = (VkAttachmentLoadOp)k->load[i]; a->storeOp = (VkAttachmentStoreOp)k->store[i];
        a->stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a->stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a->initialLayout = k->load[i] == VK_ATTACHMENT_LOAD_OP_LOAD ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        a->finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        cref[i].attachment = na++; cref[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (k->depth) {
        VkAttachmentDescription *a = &at[na];
        a->format = (VkFormat)k->depth; a->samples = (VkSampleCountFlagBits)k->samples;
        a->loadOp = (VkAttachmentLoadOp)k->depth_load; a->storeOp = (VkAttachmentStoreOp)k->depth_store;
        a->stencilLoadOp = (VkAttachmentLoadOp)k->stencil_load; a->stencilStoreOp = (VkAttachmentStoreOp)k->stencil_store;
        const int loaded = k->depth_load == VK_ATTACHMENT_LOAD_OP_LOAD || k->stencil_load == VK_ATTACHMENT_LOAD_OP_LOAD;
        a->initialLayout = loaded ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        a->finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        dref.attachment = na++; dref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    VkSubpassDescription sp; memset(&sp, 0, sizeof sp);
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = k->ncolor; sp.pColorAttachments = k->ncolor ? cref : NULL;
    sp.pDepthStencilAttachment = k->depth ? &dref : NULL;
    // The same two dependencies in every pass (render pass compatibility needs them identical).
    const VkPipelineStageFlags att_st = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags att_w = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    const VkAccessFlags att_rw = att_w | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    VkSubpassDependency dep[2]; memset(dep, 0, sizeof dep);
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[0].srcStageMask = att_st | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    dep[0].srcAccessMask = att_w | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    dep[0].dstStageMask = att_st; dep[0].dstAccessMask = att_rw;
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = att_st; dep[1].srcAccessMask = att_w;
    dep[1].dstStageMask = att_st | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    dep[1].dstAccessMask = att_rw | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = na; ci.pAttachments = na ? at : NULL; ci.subpassCount = 1; ci.pSubpasses = &sp; ci.dependencyCount = 2; ci.pDependencies = dep;
    VkRenderPass p = VK_NULL_HANDLE;
    const VkResult r = vkCreateRenderPass(d->dev, &ci, NULL, &p);
    if (r != VK_SUCCESS) {
        static int noted; if (noted++ < 8) vkn_logf(VKN_RED "render pass: vkCreateRenderPass %d (colour %u, depth format %u, samples %u)", (int)r, k->ncolor, k->depth, k->samples);
        return VK_NULL_HANDLE;
    }
    return p;
}
static int rp_ready(void) {   // under g_rp_mu
    if (!g_rp_ok) g_rp_ok = ot_init(&g_rp_tab, 256, sizeof(vkn_rp_key)) || ot_init(&g_fb_tab, 1024, sizeof(fb_k)) ? -1 : 1;
    return g_rp_ok > 0;
}
static const compat_e *compat_get(uint16_t id) {   // contents are written before the count is published and never change
    return id && id <= __atomic_load_n(&g_ncompat, __ATOMIC_ACQUIRE) ? &g_compat[id - 1] : NULL;
}
uint16_t vkn_rp_get(const vkn_rp_key *k, VkRenderPass *pass, uint16_t *compat) {
    vkn_rp_key c; rp_canon(&c, k);
    uint16_t id = 0, cid = 0; VkRenderPass p = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_rp_mu);
    if (rp_ready()) {
        const ot_hdr *h = ot_find(&g_rp_tab, &c);
        if (h) id = (uint16_t)h->val;
        else if (g_nrp >= VKN_RP_MAX) { static int noted; if (!noted++) vkn_logf(VKN_RED "render pass: more than %u passes: no more", (unsigned)VKN_RP_MAX); }
        else {
            compat_k ck; memset(&ck, 0, sizeof ck);
            memcpy(ck.color, c.color, sizeof ck.color); ck.depth = c.depth; ck.ncolor = c.ncolor; ck.samples = c.samples;
            for (uint32_t i = 0; i < g_ncompat && !cid; i++) if (!memcmp(&g_compat[i].k, &ck, sizeof ck)) cid = (uint16_t)(i + 1);
            if (!cid && g_ncompat >= VKN_COMPAT_MAX) { static int noted; if (!noted++) vkn_logf(VKN_RED "render pass: more than %u compatibility classes: no more", (unsigned)VKN_COMPAT_MAX); }
            else if ((p = rp_create(&c)) != VK_NULL_HANDLE) {
                if (!cid) { g_compat[g_ncompat].k = ck; g_compat[g_ncompat].pass = p; __atomic_store_n(&g_ncompat, g_ncompat + 1, __ATOMIC_RELEASE); cid = (uint16_t)g_ncompat; }
                g_rp[g_nrp] = p; g_rp_compat[g_nrp] = cid; id = (uint16_t)++g_nrp;
                if (!ot_insert(&g_rp_tab, &c, id)) vkn_logf(VKN_RED "render pass: cache table: out of memory");
            }
        }
        if (id) { p = g_rp[id - 1]; cid = g_rp_compat[id - 1]; }
    }
    pthread_mutex_unlock(&g_rp_mu);
    if (pass) *pass = p;
    if (compat) *compat = cid;
    return id;
}
VkRenderPass vkn_rp_compat_pass(uint16_t compat) { const compat_e *c = compat_get(compat); return c ? c->pass : VK_NULL_HANDLE; }

// ================================================================ framebuffers =================================================

static void fb_destroy(void *ctx) { const vkn_dev *d = vkn_device(); if (d) vkDestroyFramebuffer(d->dev, (VkFramebuffer)ctx, NULL); }
VkFramebuffer vkn_fb_get(uint16_t compat, const VkImageView *views, uint32_t nviews, uint32_t width, uint32_t height, uint32_t layers) {
    const compat_e *c = compat_get(compat); const vkn_dev *d = vkn_device();
    if (!c || !d || nviews > 9 || (nviews && !views) || !width || !height) return VK_NULL_HANDLE;
    fb_k k; memset(&k, 0, sizeof k);
    for (uint32_t i = 0; i < nviews; i++) k.view[i] = (uint64_t)(uintptr_t)views[i];
    k.w = width; k.h = height; k.layers = layers ? layers : 1; k.compat = compat; k.nviews = (uint8_t)nviews;
    VkFramebuffer fb = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_rp_mu);
    if (rp_ready()) {
        const ot_hdr *h = ot_find(&g_fb_tab, &k);
        if (h) fb = (VkFramebuffer)(uintptr_t)h->val;
        else {
            VkFramebufferCreateInfo ci; memset(&ci, 0, sizeof ci);
            ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; ci.renderPass = c->pass;
            ci.attachmentCount = nviews; ci.pAttachments = views; ci.width = width; ci.height = height; ci.layers = k.layers;
            const VkResult r = vkCreateFramebuffer(d->dev, &ci, NULL, &fb);
            if (r != VK_SUCCESS) { fb = VK_NULL_HANDLE; static int noted; if (noted++ < 8) vkn_logf(VKN_RED "framebuffer: vkCreateFramebuffer %d (%u views, %ux%u x%u)", (int)r, nviews, width, height, k.layers); }
            else if (!ot_insert(&g_fb_tab, &k, (uint64_t)(uintptr_t)fb)) { vkn_retire(fb_destroy, (void *)fb); g_fb_retired++; fb = VK_NULL_HANDLE; }
        }
    }
    pthread_mutex_unlock(&g_rp_mu);
    return fb;
}
void vkn_fb_forget_view(VkImageView v) {
    if (v == VK_NULL_HANDLE) return;
    const uint64_t vv = (uint64_t)(uintptr_t)v;
    pthread_mutex_lock(&g_rp_mu);
    if (g_rp_ok > 0)
        for (uint32_t i = 0; i < g_fb_tab.cap; i++) {
            ot_hdr *h = ot_at(&g_fb_tab, i);
            if (h->st != 1) continue;
            const fb_k *k = (const fb_k *)ot_key(h); int hit = 0;
            for (uint32_t j = 0; j < k->nviews && !hit; j++) hit = k->view[j] == vv;
            if (!hit) continue;
            vkn_retire(fb_destroy, (void *)(uintptr_t)h->val); g_fb_retired++;
            ot_remove(&g_fb_tab, h);
        }
    pthread_mutex_unlock(&g_rp_mu);
}

// ================================================================ pipelines ====================================================

// The warm-up module (vkn_warm.c; weak: absent in host tests that do not link it). take: a pipeline the worker built for this
// key's content (waits while the worker builds it), 1 = *out filled, ownership passes to this table. note: the render thread built
// one (listed for the next run). forget: a shader id died. owns_cache: the worker creates the VkPipelineCache (from its file).
extern int vkn_warm_take(const vkn_pipe_key *k, const vkn_pipe_src *src, VkPipeline *out) __attribute__((weak));
extern void vkn_warm_note(const vkn_pipe_desc *d, const vkn_pipe_src *src) __attribute__((weak));
extern void vkn_warm_forget(uint32_t shader) __attribute__((weak));
extern int vkn_warm_owns_cache(void) __attribute__((weak));

static pthread_mutex_t g_pipe_mu = PTHREAD_MUTEX_INITIALIZER;
static otab g_pipe_tab; static int g_pipe_ok; static VkPipelineCache g_pcache;   // g_pcache: atomic (set once by vkn_pipe_cache_init)
static uint64_t g_hits, g_misses, g_fails, g_rewrites, g_build_ns; static uint32_t g_ncompute;
static uint64_t g_warm_hits, g_stale;                                            // under g_pipe_mu
static uint64_t g_wbuilds, g_wbuild_ns, g_compiles; static uint32_t g_forget_gen;  // atomics
static const uint64_t k_compute_w2 = ~0ull;   // w[2] of a compute key (a graphics key has its upper 24 bits zero)
enum { COMPILE_NS = 2000000 };                // a build under 2 ms came from the driver's cache (the translator's rule, xbg_game.cpp)

// w0: shader (24) | vs variant (8) << 24 | fs variant (8) << 32 | vertex format (16) << 40 | primitive (8) << 56
// w1: blend (16) | depth-stencil (16) << 16 | raster (16) << 32 | pass compatibility (16) << 48
// w2: write masks (4 bits a slot, slot 0 lowest) | stencil reference (8) << 32 | fx bits (2) << 40 (stage 5, vkn_fx.h)
vkn_pipe_key vkn_pipe_key_pack(const vkn_pipe_desc *d) {
    vkn_pipe_key k;
    const compat_e *c = compat_get(d->pass);
    const VkPipelineDepthStencilStateCreateInfo *ds = vkn_ds_info(d->ds);
    // The reference only matters with a stencil test on a pass that has a stencil aspect.
    const uint8_t ref = ds && ds->stencilTestEnable && c && fmt_has_stencil(c->k.depth) ? d->stencil_ref : 0;
    uint64_t masks = 0;
    for (uint32_t i = 0; i < 8; i++) if (!c || (i < c->k.ncolor && c->k.color[i])) masks |= (uint64_t)(d->write_mask[i] & 0xf) << (4 * i);
    k.w[0] = (uint64_t)(d->shader & 0xffffff) | (uint64_t)d->vs_variant << 24 | (uint64_t)d->fs_variant << 32 | (uint64_t)d->vfmt << 40 | (uint64_t)d->prim << 56;
    k.w[1] = (uint64_t)d->blend | (uint64_t)d->ds << 16 | (uint64_t)d->raster << 32 | (uint64_t)d->pass << 48;
    k.w[2] = masks | (uint64_t)ref << 32 | (uint64_t)(d->fx & VKN_PIPE_FX_MASK) << 40;
    return k;
}
void vkn_pipe_key_unpack(const vkn_pipe_key *k, vkn_pipe_desc *d) {
    memset(d, 0, sizeof *d);
    d->shader = (uint32_t)(k->w[0] & 0xffffff); d->vs_variant = (uint8_t)(k->w[0] >> 24); d->fs_variant = (uint8_t)(k->w[0] >> 32);
    d->vfmt = (uint16_t)(k->w[0] >> 40); d->prim = (uint8_t)(k->w[0] >> 56);
    d->blend = (uint16_t)k->w[1]; d->ds = (uint16_t)(k->w[1] >> 16); d->raster = (uint16_t)(k->w[1] >> 32); d->pass = (uint16_t)(k->w[1] >> 48);
    for (uint32_t i = 0; i < 8; i++) d->write_mask[i] = (uint8_t)((k->w[2] >> (4 * i)) & 0xf);
    d->stencil_ref = (uint8_t)(k->w[2] >> 32); d->fx = (uint8_t)(k->w[2] >> 40) & VKN_PIPE_FX_MASK;
}
VkPipelineCache vkn_pipe_cache_get(void) { return __atomic_load_n(&g_pcache, __ATOMIC_ACQUIRE); }
static pthread_mutex_t g_pcache_mu = PTHREAD_MUTEX_INITIALIZER;   // serialises the one creation of the cache
int vkn_pipe_cache_init(const void *data, size_t bytes) {
    if (vkn_pipe_cache_get()) return 1;
    const vkn_dev *d = vkn_device();
    if (!d) return -1;
    // Under its own mutex, not the table's: a seeded cache of ~20 MB takes a while and lookups must not wait for it. Only the
    // warm-up worker creates it while the warm-up is on (pipe_ready then leaves it), so nobody waits here in practice.
    int rc = 1;
    pthread_mutex_lock(&g_pcache_mu);
    if (!vkn_pipe_cache_get()) {
        VkPipelineCacheCreateInfo ci; memset(&ci, 0, sizeof ci); ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        ci.initialDataSize = data ? bytes : 0; ci.pInitialData = data && bytes ? data : NULL;
        VkPipelineCache c = VK_NULL_HANDLE;
        VkResult r = vkCreatePipelineCache(d->dev, &ci, NULL, &c);
        if (r != VK_SUCCESS && ci.initialDataSize) {
            vkn_logf(VKN_RED "pipelines: vkCreatePipelineCache %d with %zu bytes of saved data: starting empty", (int)r, bytes);
            ci.initialDataSize = 0; ci.pInitialData = NULL;
            r = vkCreatePipelineCache(d->dev, &ci, NULL, &c);
        }
        if (r == VK_SUCCESS) { __atomic_store_n(&g_pcache, c, __ATOMIC_RELEASE); rc = 0; } else rc = -1;
    }
    pthread_mutex_unlock(&g_pcache_mu);
    return rc;
}
static int pipe_ready(void) {   // under g_pipe_mu
    if (g_pipe_ok) return g_pipe_ok > 0;
    const vkn_dev *d = vkn_device();
    if (!d) return 0;   // try again later
    g_pipe_ok = ot_init(&g_pipe_tab, 1024, sizeof(vkn_pipe_key)) ? -1 : 1;
    if (g_pipe_ok > 0) {
        // With the warm-up the worker creates the cache from its file; builds before that go without one (they still work).
        if (!(vkn_warm_owns_cache && vkn_warm_owns_cache())) vkn_pipe_cache_init(NULL, 0);
    } else vkn_logf(VKN_RED "pipelines: cache table: out of memory");
    return g_pipe_ok > 0;
}
// Colour formats the device cannot blend (integer formats; some float ones on some GPUs): blending off there, as NVN ignores it.
static pthread_mutex_t g_blendable_mu = PTHREAD_MUTEX_INITIALIZER;   // builds run on the render thread and the warm-up worker
static int fmt_blendable(VkFormat f) {
    static struct { VkFormat f; uint8_t ok; } cache[64]; static uint32_t n;
    pthread_mutex_lock(&g_blendable_mu);
    for (uint32_t i = 0; i < n; i++) if (cache[i].f == f) { const int ok = cache[i].ok; pthread_mutex_unlock(&g_blendable_mu); return ok; }
    const vkn_dev *d = vkn_device(); VkFormatProperties p; memset(&p, 0, sizeof p);
    if (d) vkGetPhysicalDeviceFormatProperties(d->phys, f, &p);
    const uint8_t ok = (p.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;
    if (n < 64) { cache[n].f = f; cache[n].ok = ok; n++; }
    pthread_mutex_unlock(&g_blendable_mu);
    return ok;
}
// Thread safe (no table access): every input is immutable content (state ids, compatibility classes) or the caller's.
static VkPipeline pipe_build(const vkn_pipe_desc *k, VkPrimitiveTopology topo, const vkn_pipe_src *src) {
    const vkn_dev *dv = vkn_device(); const compat_e *c = compat_get(k->pass);
    if (!dv || !c || !src->vs || !src->layout) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2]; memset(st, 0, sizeof st); uint32_t nst = 0;
    st[nst].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[nst].stage = VK_SHADER_STAGE_VERTEX_BIT; st[nst].module = src->vs; st[nst].pName = "main"; nst++;
    if (src->fs && !(k->fx & VKN_PIPE_FX_NO_FS)) { st[nst].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st[nst].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[nst].module = src->fs; st[nst].pName = "main"; nst++; }
    // Vertex input: the format's attributes, plus the null stream for every location the vertex module reads and the format lacks.
    VkVertexInputAttributeDescription at[VKN_VFMT_LOCS]; VkVertexInputBindingDescription bd[VKN_VFMT_STREAMS + 1]; uint32_t nat = 0, nbd = 0;
    const vkn_vfmt *vf = vkn_vfmt_info(k->vfmt);
    uint16_t have = 0;
    if (vf) { memcpy(at, vf->attr, vf->nattr * sizeof at[0]); nat = vf->nattr; memcpy(bd, vf->bind, vf->nbind * sizeof bd[0]); nbd = vf->nbind; have = vf->loc_mask; }
    const uint16_t missing = (uint16_t)(src->vs_inputs & ~have);
    if (missing) {
        for (uint32_t l = 0; l < VKN_VFMT_LOCS; l++)
            if (missing & (1u << l)) { at[nat].location = l; at[nat].binding = VKN_VFMT_NULL_BINDING; at[nat].format = VK_FORMAT_R32G32B32A32_SFLOAT; at[nat].offset = 0; nat++; }
        bd[nbd].binding = VKN_VFMT_NULL_BINDING; bd[nbd].stride = 0; bd[nbd].inputRate = VK_VERTEX_INPUT_RATE_VERTEX; nbd++;
    }
    VkPipelineVertexInputStateCreateInfo vi; memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = nbd; vi.pVertexBindingDescriptions = nbd ? bd : NULL;
    vi.vertexAttributeDescriptionCount = nat; vi.pVertexAttributeDescriptions = nat ? at : NULL;
    VkPipelineInputAssemblyStateCreateInfo ia; memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO; ia.topology = topo;
    VkPipelineViewportStateCreateInfo vp; memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO; vp.viewportCount = 1; vp.scissorCount = 1;
    // Raster: the state's struct with the bias current now (copied under the state mutex); no state = no cull, fill, CW.
    VkPipelineRasterizationStateCreateInfo rs; memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE; rs.lineWidth = 1.0f;
    pthread_mutex_lock(&g_st_mu);
    const VkPipelineRasterizationStateCreateInfo *r0 = vkn_raster_info(k->raster);
    if (r0) rs = *r0;
    pthread_mutex_unlock(&g_st_mu);
    if (k->fx & VKN_PIPE_FX_DISCARD) rs.rasterizerDiscardEnable = VK_TRUE;   // [stage 5] nothing reaches the targets (vkn_fx.h)
    if (rs.polygonMode != VK_POLYGON_MODE_FILL && !dv->feat.fillModeNonSolid) {
        static int noted; if (!__atomic_fetch_add(&noted, 1, __ATOMIC_RELAXED)) vkn_logf("pipelines: polygon mode %d needs fillModeNonSolid, which the device lacks: drawn filled", (int)rs.polygonMode);
        rs.polygonMode = VK_POLYGON_MODE_FILL;
    }
    VkPipelineMultisampleStateCreateInfo ms; memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO; ms.rasterizationSamples = (VkSampleCountFlagBits)c->k.samples; ms.minSampleShading = 1.0f;
    VkPipelineDepthStencilStateCreateInfo ds; memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO; ds.depthCompareOp = VK_COMPARE_OP_ALWAYS; ds.maxDepthBounds = 1.0f;
    const VkPipelineDepthStencilStateCreateInfo *d0 = vkn_ds_info(k->ds);
    if (d0) ds = *d0;
    ds.front.reference = ds.back.reference = k->stencil_ref;
    if (!fmt_has_stencil(c->k.depth)) ds.stencilTestEnable = VK_FALSE;
    if (!fmt_has_depth(c->k.depth)) ds.depthTestEnable = ds.depthWriteEnable = VK_FALSE;
    // Colour: one blend description on every bound slot (ApplyBlend 0x9284e0), the slot's mask from its format (0x9282c0).
    VkPipelineColorBlendAttachmentState cb[8]; memset(cb, 0, sizeof cb);
    const VkPipelineColorBlendAttachmentState *b0 = vkn_blend_att(k->blend);
    for (uint32_t i = 0; i < c->k.ncolor; i++) {
        if (!c->k.color[i]) continue;   // VK_ATTACHMENT_UNUSED: blend off, mask 0
        if (b0) cb[i] = *b0;
        if (cb[i].blendEnable && !fmt_blendable((VkFormat)c->k.color[i])) cb[i].blendEnable = VK_FALSE;
        cb[i].colorWriteMask = k->write_mask[i] & 0xf;
    }
    VkPipelineColorBlendStateCreateInfo cbs; memset(&cbs, 0, sizeof cbs);
    cbs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO; cbs.attachmentCount = c->k.ncolor; cbs.pAttachments = c->k.ncolor ? cb : NULL;
    static const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy; memset(&dy, 0, sizeof dy);
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO; dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = nst; ci.pStages = st; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp;
    ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pDepthStencilState = c->k.depth ? &ds : NULL; ci.pColorBlendState = &cbs;
    ci.pDynamicState = &dy; ci.layout = src->layout; ci.renderPass = src->pass ? src->pass : c->pass; ci.subpass = 0; ci.basePipelineIndex = -1;
    VkPipeline p = VK_NULL_HANDLE;
    // The default VkPipelineCache is internally synchronised (created without EXTERNALLY_SYNCHRONIZED): both builders share it.
    const VkResult r = vkCreateGraphicsPipelines(dv->dev, vkn_pipe_cache_get(), 1, &ci, NULL, &p);
    if (r != VK_SUCCESS) {
        static int noted;
        if (__atomic_fetch_add(&noted, 1, __ATOMIC_RELAXED) < 16)
            vkn_logf(VKN_RED "pipelines: vkCreateGraphicsPipelines %d (shader %u vs %u fs %u, blend %u ds %u raster %u vfmt %u, prim %u, pass %u)", (int)r,
                     k->shader, k->vs_variant, k->fs_variant, k->blend, k->ds, k->raster, k->vfmt, k->prim, k->pass);
        return VK_NULL_HANDLE;
    }
    __atomic_add_fetch(&g_bias_pipes, 1, __ATOMIC_RELAXED);
    return p;
}
static void pipe_destroy(void *ctx) { const vkn_dev *d = vkn_device(); if (d) vkDestroyPipeline(d->dev, (VkPipeline)ctx, NULL); }
// Render thread. The table mutex is held only for the find and the insert: a build (or the wait for the warm-up worker's build of
// the same content) runs outside it, so vkn_pipe_forget_shader (shader threads) never waits for a build and there is no lock order
// between this mutex and the worker's.
VkPipeline vkn_pipe_get(const vkn_pipe_key *k, const vkn_pipe_src *src) {
    VkPipeline p = VK_NULL_HANDLE; int found = 0;
    pthread_mutex_lock(&g_pipe_mu);
    const int ok = pipe_ready();
    if (ok) { const ot_hdr *h = ot_find(&g_pipe_tab, k); if (h) { g_hits++; p = (VkPipeline)(uintptr_t)h->val; found = 1; } }
    const uint32_t gen = __atomic_load_n(&g_forget_gen, __ATOMIC_ACQUIRE);
    pthread_mutex_unlock(&g_pipe_mu);
    if (found || !ok || !src) return p;
    vkn_pipe_desc d; vkn_pipe_key_unpack(k, &d);
    VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    if (vkn_prim_topology(d.prim, &topo)) { pthread_mutex_lock(&g_pipe_mu); g_rewrites++; pthread_mutex_unlock(&g_pipe_mu); return VK_NULL_HANDLE; }   // the draw path rewrites the indices and asks again
    int warm = 0; uint64_t ns = 0;
    if (vkn_warm_take && vkn_warm_take(k, src, &p) && p) warm = 1;   // built by the warm-up worker (this content, these modules)
    else {
        p = VK_NULL_HANDLE;
        const uint64_t t0 = vkn_now_ns();
        p = pipe_build(&d, topo, src);
        ns = vkn_now_ns() - t0;
        if (p && ns >= COMPILE_NS) __atomic_add_fetch(&g_compiles, 1, __ATOMIC_RELAXED);
    }
    int keep = 1;
    pthread_mutex_lock(&g_pipe_mu);
    if (warm) g_warm_hits++;
    else { g_misses++; g_build_ns += ns; if (!p) g_fails++; }
    const ot_hdr *h = ot_find(&g_pipe_tab, k);   // only this thread inserts graphics keys; checked anyway
    if (h) { if (p) vkn_retire(pipe_destroy, (void *)(uintptr_t)p); p = (VkPipeline)(uintptr_t)h->val; keep = 0; }
    else if (__atomic_load_n(&g_forget_gen, __ATOMIC_ACQUIRE) != gen) {
        // A shader died while this was built: if it was this one, its id may be reused - never cache that. The pipeline still
        // serves this draw (the modules live 3 more frames, so does a retired pipeline).
        g_stale++; keep = 0;
        if (p) vkn_retire(pipe_destroy, (void *)(uintptr_t)p);
    } else if (!ot_insert(&g_pipe_tab, k, (uint64_t)(uintptr_t)p)) vkn_logf(VKN_RED "pipelines: cache table: out of memory");   // a failure is cached too
    pthread_mutex_unlock(&g_pipe_mu);
    if (keep && !warm && p && vkn_warm_note) vkn_warm_note(&d, src);   // listed for the next run (only queued on this thread)
    return p;
}
VkPipeline vkn_pipe_build(const vkn_pipe_desc *d, const vkn_pipe_src *src, uint64_t *ns) {
    if (ns) *ns = 0;
    VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    if (!d || !src || vkn_prim_topology(d->prim, &topo) || !vkn_device()) return VK_NULL_HANDLE;
    const uint64_t t0 = vkn_now_ns();
    const VkPipeline p = pipe_build(d, topo, src);
    const uint64_t dt = vkn_now_ns() - t0;
    __atomic_add_fetch(&g_wbuilds, 1, __ATOMIC_RELAXED); __atomic_add_fetch(&g_wbuild_ns, dt, __ATOMIC_RELAXED);
    if (p && dt >= COMPILE_NS) __atomic_add_fetch(&g_compiles, 1, __ATOMIC_RELAXED);
    if (ns) *ns = dt;
    return p;
}
uint64_t vkn_pipe_compiles(void) { return __atomic_load_n(&g_compiles, __ATOMIC_RELAXED); }
uint32_t vkn_pipe_forget_gen(void) { return __atomic_load_n(&g_forget_gen, __ATOMIC_ACQUIRE); }
VkPipeline vkn_pipe_compute(uint32_t shader, VkShaderModule module, VkPipelineLayout layout) {
    const vkn_pipe_key k = {{shader, 0, k_compute_w2}};
    VkPipeline p = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_pipe_mu);
    if (pipe_ready()) {
        const ot_hdr *h = ot_find(&g_pipe_tab, &k);
        if (h) { g_hits++; p = (VkPipeline)(uintptr_t)h->val; }
        else if (module && layout) {   // compute pipelines are few (5 bundles): built under the mutex, never listed
            g_misses++;
            const uint64_t t0 = vkn_now_ns();
            VkComputePipelineCreateInfo ci; memset(&ci, 0, sizeof ci);
            ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO; ci.layout = layout; ci.basePipelineIndex = -1;
            ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = module; ci.stage.pName = "main";
            const VkResult r = vkCreateComputePipelines(vkn_device()->dev, vkn_pipe_cache_get(), 1, &ci, NULL, &p);
            g_build_ns += vkn_now_ns() - t0;
            if (r != VK_SUCCESS) { p = VK_NULL_HANDLE; g_fails++; static int noted; if (noted++ < 8) vkn_logf(VKN_RED "pipelines: vkCreateComputePipelines %d (shader %u)", (int)r, shader); }
            if (ot_insert(&g_pipe_tab, &k, (uint64_t)(uintptr_t)p)) g_ncompute++;
        }
    }
    pthread_mutex_unlock(&g_pipe_mu);
    return p;
}
void vkn_pipe_forget_shader(uint32_t shader) {
    pthread_mutex_lock(&g_pipe_mu);
    __atomic_add_fetch(&g_forget_gen, 1, __ATOMIC_ACQ_REL);
    if (g_pipe_ok > 0)
        for (uint32_t i = 0; i < g_pipe_tab.cap; i++) {
            ot_hdr *h = ot_at(&g_pipe_tab, i);
            if (h->st != 1) continue;
            const vkn_pipe_key *k = (const vkn_pipe_key *)ot_key(h);
            const int compute = k->w[2] == k_compute_w2;
            if (compute ? k->w[0] != shader : (k->w[0] & 0xffffff) != (shader & 0xffffff)) continue;
            if (h->val) vkn_retire(pipe_destroy, (void *)(uintptr_t)h->val);
            if (compute) g_ncompute--;
            ot_remove(&g_pipe_tab, h);
        }
    pthread_mutex_unlock(&g_pipe_mu);
    if (vkn_warm_forget) vkn_warm_forget(shader);   // the worker's pipelines of this id (outside this mutex: no lock order)
}

// ---- stage 4: what the warm-up lists (engine argument tuples behind the content ids, compatibility classes) ----
// Inverse of stencil_op (VkStencilOp -> the engine's 0..7).
static uint8_t stencil_op_engine(uint8_t vk) { static const uint8_t t[8] = {0, 1, 2, 6, 7, 3, 4, 5}; return vk < 8 ? t[vk] : 0; }
int vkn_blend_args(uint16_t id, uint8_t out[6]) {
    if (!id || id > __atomic_load_n(&g_nblend, __ATOMIC_ACQUIRE)) return -1;
    memcpy(out, g_blend[id - 1].args, 6);   // src_rgb, dst_rgb, src_a, dst_a, alpha_op, separate bit (= the flags argument)
    return 0;
}
int vkn_ds_args(uint16_t id, uint8_t out[8]) {
    if (!id || id > __atomic_load_n(&g_nds, __ATOMIC_ACQUIRE)) return -1;
    const uint8_t *a = g_ds[id - 1].args;   // compare ops are stored as the engine's values (0..7 identity), stencil ops as VkStencilOp
    out[0] = a[0]; out[1] = a[1]; out[2] = a[2]; out[3] = a[3]; out[4] = a[4]; out[5] = a[5]; out[6] = stencil_op_engine(a[6]); out[7] = stencil_op_engine(a[7]);
    return 0;
}
int vkn_raster_args(uint16_t id, uint8_t out[5]) {
    if (!id || id > __atomic_load_n(&g_nraster, __ATOMIC_ACQUIRE)) return -1;
    memcpy(out, g_raster[id - 1].args, 5);   // normalised cull / fill / flags / polygon offset: vkn_raster_put maps them to themselves
    return 0;
}
const uint8_t *vkn_vfmt_elems(uint16_t id, uint32_t *count) {
    if (!id || id > __atomic_load_n(&g_nvfmt, __ATOMIC_ACQUIRE)) return NULL;
    if (count) *count = g_vfmt[id - 1].count;
    static const uint8_t none[6];
    return g_vfmt[id - 1].el ? g_vfmt[id - 1].el : none;
}
int vkn_rp_compat_info(uint16_t compat, uint32_t color[8], uint8_t *ncolor, uint32_t *depth, uint8_t *samples) {
    const compat_e *c = compat_get(compat);
    if (!c) return -1;
    if (color) memcpy(color, c->k.color, sizeof c->k.color);
    if (ncolor) *ncolor = c->k.ncolor;
    if (depth) *depth = c->k.depth;
    if (samples) *samples = c->k.samples;
    return 0;
}
uint16_t vkn_rp_compat_make(const uint32_t color[8], uint8_t ncolor, uint32_t depth, uint8_t samples) {
    compat_k ck; memset(&ck, 0, sizeof ck);
    if (ncolor > 8) return 0;
    for (uint32_t i = 0; i < ncolor; i++) ck.color[i] = color[i];
    ck.depth = depth; ck.ncolor = ncolor; ck.samples = samples ? samples : VK_SAMPLE_COUNT_1_BIT;
    const uint32_t n = __atomic_load_n(&g_ncompat, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n; i++) if (!memcmp(&g_compat[i].k, &ck, sizeof ck)) return (uint16_t)(i + 1);
    vkn_rp_key k; vkn_rp_key_init(&k);
    for (uint32_t i = 0; i < ncolor; i++) { k.color[i] = color[i]; k.load[i] = VK_ATTACHMENT_LOAD_OP_LOAD; k.store[i] = VK_ATTACHMENT_STORE_OP_STORE; }
    k.ncolor = ncolor; k.depth = depth; k.samples = ck.samples;
    k.depth_load = k.stencil_load = VK_ATTACHMENT_LOAD_OP_LOAD; k.depth_store = k.stencil_store = VK_ATTACHMENT_STORE_OP_STORE;
    uint16_t cid = 0;
    return vkn_rp_get(&k, NULL, &cid) ? cid : 0;
}
void vkn_state_wstats_get(vkn_state_wstats *s) {
    memset(s, 0, sizeof *s);
    pthread_mutex_lock(&g_pipe_mu); s->warm_hits = g_warm_hits; s->stale_builds = g_stale; pthread_mutex_unlock(&g_pipe_mu);
    s->warm_builds = __atomic_load_n(&g_wbuilds, __ATOMIC_RELAXED); s->warm_build_ns = __atomic_load_n(&g_wbuild_ns, __ATOMIC_RELAXED);
    s->compiles = __atomic_load_n(&g_compiles, __ATOMIC_RELAXED);
}

// ================================================================ census =======================================================

void vkn_state_stats_get(vkn_state_stats *s) {
    memset(s, 0, sizeof *s);
    pthread_mutex_lock(&g_st_mu);
    s->blend_ids = g_nblend; s->ds_ids = g_nds; s->raster_ids = g_nraster; s->vfmt_ids = g_nvfmt;
    if (g_htab_ok > 0) {
        uint32_t live[K_KINDS] = {0, 0, 0, 0};
        for (int k = 0; k < K_KINDS; k++) for (uint32_t i = 0; i < g_htab[k].n; i++) live[k] += g_htab[k].handle[i] != 0;
        s->blend_live = live[K_BLEND]; s->ds_live = live[K_DS]; s->raster_live = live[K_RASTER]; s->vfmt_live = live[K_VFMT];
    }
    s->puts = g_st_puts; s->kills = g_st_kills; s->bad = g_st_bad;
    pthread_mutex_unlock(&g_st_mu);
    pthread_mutex_lock(&g_rp_mu);
    s->passes = g_nrp; s->compat = g_ncompat; s->framebuffers = g_rp_ok > 0 ? g_fb_tab.live : 0; s->fb_retired = g_fb_retired;
    pthread_mutex_unlock(&g_rp_mu);
    pthread_mutex_lock(&g_pipe_mu);
    s->compute = g_ncompute; s->pipelines = (g_pipe_ok > 0 ? g_pipe_tab.live : 0) - g_ncompute;
    s->hits = g_hits; s->misses = g_misses; s->failures = g_fails; s->rewrites = g_rewrites; s->build_ns = g_build_ns;
    pthread_mutex_unlock(&g_pipe_mu);
}
#endif
