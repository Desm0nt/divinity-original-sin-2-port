// [DOS2] NR-31: the portable pipeline warm-up seed (DM_NR_SEED; docs/dos2/nr/NR-31.md). Pure C: no Vulkan headers, no game addresses,
// no host types (Vulkan enum values are plain numbers here; the host test checks them against vulkan_core.h).
//
// The boot warm-up list (files/vk-pipeline-list.bin, xbg_game.cpp namespace warm) holds one record per graphics pipeline the device built:
// the shader pack keys of its stages + the exact create-info key (XbgPipelineKey words: layout definition, render-pass definition,
// pipeline create info, modules named by warm::fnv(key)). Those words are the DEVICE's: render-pass dependencies follow its tile policy,
// depth formats its D24 / D16 stand-ins, the layout its dynamic-uniform-buffer limit, vertex formats its fetch support, colour write masks
// its NOOUT policy, rasterizer / depth / blend fields its optional features, the vertex module tag its retype policy. A list pulled from
// one phone therefore does not match another phone byte for byte, and a draw takes a warm pipeline only on an exact match of those bytes.
//
// A seed is a list pulled from one device (the source) plus that device's policy (nsd_policy). nsd_expand() re-targets one record to
// this device: every device-dependent field is mapped from the source policy back to the game's value and forward to this device's
// value (the same derivations draw() / create_pass() / descriptors() make), everything else is copied, and every value is checked
// against the shapes draw() can produce and against this device's formats and limits. A record with any field the expander does not
// know, any mapping that is not invertible (an ambiguous stand-in), a layout the bundle tables under the source policy do not reproduce,
// or a format / limit this device lacks is REFUSED (never built). Safety argument: a draw takes a warm pipeline only when its own key
// equals the record's bytes (warm::take), so a wrong expansion is an unused build, never a different picture; the validation keeps
// every create info a valid one for this device.
//
// Content: shader pack keys (16 hex digits of xbg_shader_program_key = a 64-bit hash of the game's shader control + code; "#at" alpha
// variants name the NVN alpha func and reference) and fixed-function numbers (Vulkan enums, formats, strides, offsets, masks). No
// shader code, no SPIR-V, no game data: the seed cannot hold anything else (nsd_parse / nsd_shape refuse every other word).
#ifndef DM_NR_SEED_H
#define DM_NR_SEED_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NSD_STAGES = 5, NSD_MAX_LB = 1280, NSD_MAX_PUSH = 16, NSD_MAX_ATT = 9, NSD_MAX_COL = 8, NSD_MAX_DEP = 8, NSD_MAX_VB = 32,
       NSD_MAX_VA = 32, NSD_MAX_VP = 16, NSD_MAX_BLEND = 8, NSD_MAX_DYN = 64, NSD_KEY_MAX = 64, NSD_MAX_WORDS = 4096, NSD_MAX_BIND = 256,
       NSD_MAX_IN = 16 };
#define NSD_REC_MAGIC 0x57504c58u    /* "XLPW": a warm-up list record (warm::record) */
#define NSD_SEED_MAGIC 0x31535044u   /* "DPS1": the seed file */
#define NSD_SEED_VERSION 1u

// Refusal reasons (nsd_expand / nsd_check_record). Stable numbers: logs and tools/dos2_pipe_seed.py print them.
enum {
    NSD_OK = 0,
    NSD_E_PARSE,       // truncated / not a warm::build record
    NSD_E_BIG,         // over the expander's caps
    NSD_E_SHAPE,       // a field draw() never writes like this (unknown record)
    NSD_E_KEY,         // a stage key that is not a pack key / unknown variant tag
    NSD_E_ID,          // module id != warm::fnv(key)
    NSD_E_VARIANT,     // a source variant that cannot be re-targeted (#vi with conversion, #vi while the source retype is off)
    NSD_E_SHADER,      // a stage bundle is missing / unreadable here
    NSD_E_FACTS,       // shader facts the re-target needs are unavailable (input classes, fragment outputs)
    NSD_E_LAYOUT,      // the layout from the bundles under the source policy != the recorded one (model drift)
    NSD_E_DESC_LIMIT,  // the layout exceeds this device's descriptor limits
    NSD_E_DEPTH_AMBIG, // the source's depth stand-in maps two game formats to the recorded one
    NSD_E_FORMAT,      // an attachment format this device cannot render to (or blend)
    NSD_E_VERTEX,      // a vertex format this device cannot fetch, no fallback
    NSD_E_LIMIT,       // vertex / attachment / sample-count limits
    NSD_E_NOOUT,       // the source masked the colour writes (NOOUT): the game's masks are unknown
    NSD_E_DEPS,        // render-pass dependencies are not the source's create_pass form
    NSD_E_POLICY,      // this device runs a mode the expander does not model (NR-08 split, NR-08b read-only)
    NSD_E_VTX_AMBIG,   // a recorded vertex format the source may have widened from a format it cannot fetch
    NSD_E_SPLIT,       // [NR-31b] this device's NR-08 split gives the shader set a layout picked at run time (block change counts)
    NSD_E_COUNT
};
const char *nsd_reason_name(int r);

// ---- the decoded record (warm::build's reader, xbg_pipeline_key.h's writer; floats kept as bits) ----
typedef struct { uint32_t binding, type, count, stages; } nsd_lb;
typedef struct { uint32_t flags, format, samples, load, store, sload, sstore, init, fin; } nsd_att;
typedef struct { uint32_t att, layout; } nsd_ref;
typedef struct { uint32_t src, dst, src_stage, dst_stage, src_access, dst_access, flags; } nsd_dep;
typedef struct { uint32_t flags, stage; uint64_t id; } nsd_stage;
typedef struct { uint32_t enable, src_c, dst_c, op_c, src_a, dst_a, op_a, mask; } nsd_blend;
typedef struct { uint32_t fail, pass, dfail, cmp, cmask, wmask, ref; } nsd_stencil;
typedef struct nsd_pipe {
    uint32_t l_flags, nlb; nsd_lb lb[NSD_MAX_LB]; uint32_t pl_flags, sets, npush, push[NSD_MAX_PUSH][3];
    uint32_t r_flags, natt; nsd_att att[NSD_MAX_ATT]; uint32_t s_flags, s_bind, ncol; nsd_ref col[NSD_MAX_COL]; uint32_t has_depth; nsd_ref depth;
    uint32_t ndep; nsd_dep dep[NSD_MAX_DEP];
    uint32_t flags, subpass, nst; nsd_stage st[NSD_STAGES];
    uint32_t vi_flags, nvb, vb[NSD_MAX_VB][3], nva, va[NSD_MAX_VA][4];   // vb: binding, stride, rate; va: location, binding, format, offset
    uint32_t ia_flags, topology, restart;
    uint32_t has_tess, t_flags, patch;
    uint32_t vp_flags, nvp, nsc, has_vp, has_sc, vps[NSD_MAX_VP][6], scs[NSD_MAX_VP][4];
    uint32_t rs_flags, clamp, discard, poly, cull, front, bias_en, bias_const, bias_clamp, bias_slope, line_width;
    uint32_t ms_flags, samples, sshading, min_sshading, a2c, a2one, has_mask, mask[2];
    uint32_t ds_flags, dt, dw, dcmp, dbounds, st_en; nsd_stencil sf, sb; uint32_t db_min, db_max;
    uint32_t cb_flags, logic_en, logic_op, nblend; nsd_blend blend[NSD_MAX_BLEND]; uint32_t bconst[4];
    uint32_t dy_flags, ndyn, dyn[NSD_MAX_DYN];
} nsd_pipe;
int nsd_parse(const uint32_t *w, size_t n, nsd_pipe *p);            // NSD_OK / NSD_E_PARSE / NSD_E_BIG
size_t nsd_emit(const nsd_pipe *p, uint32_t *w, size_t cap);        // words written; 0 = does not fit
// The shapes draw() produces (strict): NSD_OK or NSD_E_SHAPE (*field: the first field that is not). nk = number of stage keys.
int nsd_shape(const nsd_pipe *p, uint32_t nk, const char **field);
// Name of the field word `at` of a record belongs to ("?" past the end / unparsable): the diff lines of DM_NR_SEED=2.
const char *nsd_field_at(const uint32_t *w, size_t n, size_t at);
uint64_t nsd_fnv(const char *s, size_t n);   // warm::fnv: FNV-1a 64 with the top bit set

// ---- stage keys: "<16 hex>" + "" | "#at<func>_<8 hex>" | "#vi<8 hex>" | "#vi<8 hex>c<8 hex>" ----
// [NR-31b] or a fused post stage of the backend (DM_NB_POST, xbg_nb_post.inc fused_name): "F1" / "F2" + up to two upper-case letters + "."
// + the host's 16 hex digits ("F1.", "F1A.", "F1AS.", "F1AR.", "F2."): a fragment stage whose bundle is its own (the post pack), named by the
// whole key (base = the whole key).
enum { NSD_TAG_NONE, NSD_TAG_AT, NSD_TAG_VI, NSD_TAG_FUSED };
typedef struct { char base[24]; int tag; uint32_t at_func, at_bits, vi_code, vi_conv; } nsd_key;
int nsd_key_parse(const char *s, size_t n, nsd_key *k);   // 0 ok, -1 not a key

// ---- policies: what the source device did / what this device does ----
enum { NSD_F_NONSOLID = 1, NSD_F_DCLAMP = 2, NSD_F_BCLAMP = 4, NSD_F_DBOUNDS = 8, NSD_F_LOGICOP = 16, NSD_F_ALL = 31 };
typedef struct nsd_policy {
    uint32_t tile_barrier;                                    // DM_GFX_TILE_BARRIER: create_pass narrows its two dependencies
    uint32_t d24s8_as_d32s8, x8d24_as_d32, depth16_as_d24;    // format(): depth stand-ins; get_image: NVN D16 as X8_D24
    uint32_t bias_x2;                                         // DM_GFX_DEPTH_BIAS_X2
    uint32_t ubo_dynamic, ubo_dyn_limit, dyn_partial;         // dyn_mode_for: DM_GFX_UBO_DYNAMIC, maxDescriptorSetUniformBuffersDynamic, partial mode
    uint32_t noout;                                           // DM_GFX_NOOUT_MASK
    uint32_t retype;                                          // DM_GFX_VTX_RETYPE
    uint32_t alpha_test;                                      // DM_GFX_ALPHA_TEST
    uint32_t features;                                        // NSD_F_*: enabled optional features
    uint32_t unmodelled;                                      // != 0: a mode the expander does not model (refuse all; a seed's header has 0)
    uint32_t vtx_unsup[8];                                    // bit f: core VkFormat f < 256 without VERTEX_BUFFER (source: its widenings are not invertible)
    // [NR-31b] layout rules of a destination (dyn_mode_for / UBO_DYN); not in the seed's header (its layouts are the plain model's: 0 there)
    uint32_t split;                                           // NR-08 DM_NR_DESC_SPLIT: a shader set with more per-draw blocks than the limit -> NSD_E_SPLIT
    uint32_t ubo_ro;                                          // NR-08b DM_NR_UBODYN read-only rule: mode <= 2, a dynamic block must be read by its stage
} nsd_policy;
enum { NSD_POLICY_WORDS = 21 };
uint32_t nsd_vertex_widen(uint32_t f);   // xbg_vertex_widen (xbg_vendor_policy.h): the 4-component twin of a 3-component format, or 0
void nsd_policy_words(const nsd_policy *p, uint32_t out[NSD_POLICY_WORDS]);
void nsd_policy_from(const uint32_t in[NSD_POLICY_WORDS], nsd_policy *p);

// ---- the device: bundle facts, formats, limits (callbacks: the host's own functions) ----
typedef struct { uint32_t binding, type, count, source, slot; } nsd_bind;   // xbg_shader_binding fields the layout uses (slot: NR-08 split)
typedef struct nsd_shader {
    uint32_t stage;                                    // XGS stage (0 vertex .. 4 fragment)
    uint32_t nb; nsd_bind b[NSD_MAX_BIND];             // the bundle's binding table, in its order
    uint32_t nin, in_loc[NSD_MAX_IN];                  // vertex: input locations (bundle)
    int classes; uint32_t in_class[NSD_MAX_IN];        // vertex: scalar class per input from the final words (1 = filled)
    int facts; uint32_t outputs, sample_mask;          // fragment: declared outputs / SampleMask from the final words (1 = filled)
    int reads; uint8_t read[NSD_MAX_BIND];             // [NR-31b] per binding: the final words read it (nr_spv_binding_reads; uniform buffers; 1 = filled)
} nsd_shader;
enum { NSD_W_CLASSES = 1, NSD_W_FACTS = 2, NSD_W_READS = 4 };
typedef struct nsd_dev {
    void *u;
    // The bundle of a base key (16 hex digits); want = NSD_W_* facts needed. 0 = filled.
    int (*shader)(void *u, const char *base, nsd_shader *out, unsigned want);
    // The format this device fetches a vertex attribute of format f with (draw(): f when it has VERTEX_BUFFER, else
    // vertex_format_fallback); *conv = 0 / 1 SSCALED->SINT / 2 USCALED->UINT. 0 = no fetchable format.
    uint32_t (*vtx_format)(void *u, uint32_t f, uint32_t *conv);
    uint32_t (*vtx_class)(uint32_t f);                 // xbg_vertex_format_class
    // An attachment format this device can render to: depth = depth / stencil attachment, blend = colour with blending enabled.
    int (*att_ok)(void *u, uint32_t f, int depth, int blend);
    uint32_t max_color, max_vi_bind, max_vi_attr, max_vi_offset, max_vi_stride;
    uint32_t color_samples, depth_samples;             // VkSampleCountFlags
    uint32_t stage_lim[5], set_lim[5];                 // per stage / per set: samplers, uniform buffers, storage buffers, sampled images, storage images
    uint32_t set_ubo_dyn, set_ssbo_dyn;
} nsd_dev;

// ---- re-targeting ----
typedef struct nsd_out {
    uint32_t nk; char key[NSD_STAGES][NSD_KEY_MAX + 1]; uint32_t klen[NSD_STAGES];
    uint32_t words[NSD_MAX_WORDS]; size_t nw;
    const char *field;   // refusal detail (shape field / what differs)
} nsd_out;
// Scratch of one expanding thread (large: allocate once per thread).
typedef struct nsd_work { nsd_pipe p, lay; nsd_shader sh[NSD_STAGES]; } nsd_work;
// One record (keys + words as in the list) from the source policy to the destination one.
// NSD_OK = out holds this device's record; otherwise the refusal reason (out->field: detail).
int nsd_expand(const nsd_policy *src, const nsd_policy *dst, const nsd_dev *dev, const char *const *keys, const uint32_t *klen, uint32_t nk,
               const uint32_t *w, size_t n, nsd_work *wk, nsd_out *out);
// The two dependencies create_pass() writes under a policy (the host checks its own against this at start-up).
void nsd_pass_deps(uint32_t tile_barrier, nsd_dep dep[2]);
// The layout descriptors() derives for these stage shaders (st[i]: XGS stage index per shader, ascending) under a policy:
// NSD_OK / NSD_E_SHAPE (a binding draw() refuses) / NSD_E_BIG / NSD_E_POLICY / NSD_E_SPLIT (pol->split: a layout picked at run time) /
// NSD_E_FACTS (pol->ubo_ro and a stage without its read flags).
int nsd_layout(const nsd_policy *pol, const nsd_shader *const *sh, uint32_t nsh, nsd_pipe *p);

// ---- the seed file: header + list records ----
// u32 magic "DPS1", version, header words H, record count, flags, id lo, id hi, policy word count P, P policy words; records from
// byte 4*H to the end (warm::record's format). id = nsd_fnv64 of the record bytes.
typedef struct nsd_seed_hdr { uint32_t version, nrec, flags; nsd_policy src; uint64_t id; size_t body; } nsd_seed_hdr;
uint64_t nsd_fnv64(const uint8_t *b, size_t n);
int nsd_seed_open(const uint8_t *b, size_t n, nsd_seed_hdr *h);   // 0 ok, -1 not a seed / damaged / other version
typedef struct nsd_rec { uint32_t nk; const char *key[NSD_STAGES]; uint32_t klen[NSD_STAGES]; uint32_t words[NSD_MAX_WORDS]; size_t nw; } nsd_rec;
int nsd_rec_next(const uint8_t *b, size_t n, size_t *off, nsd_rec *r);   // 1 = a record, 0 = end, -1 = damaged

#ifdef __cplusplus
}
#endif
#endif
