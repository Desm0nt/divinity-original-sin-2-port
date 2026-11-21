// [DOS2] VK-NATIVE stage 5.3 V1: PRESKIN on the backend (vkn_preskin.c; one vkn_fx function, order 150). The old path's NR-PRESKIN
// (dm/gfx/vulkan/engine/xbg_nr_preskin.inc, docs/dos2/nr/NR-PRESKIN.md) ported to mode 1: a skinned mesh is drawn several times a frame
// (shadow cascades, depth pre-pass, G-buffer, FX) and every draw re-ran the skinning part of its vertex shader; here the skinning runs
// ONCE per mesh instance and frame and the draws read its result.
// The pure parts are used as they are: xbg_spirv_preskin.cpp (plan = the minimum cut of the vertex shader between the bone loads and the
// rest; SLICE = the cut values' computation storing raw 32-bit words at gl_VertexIndex * m + c of a storage buffer; CONSUMER = the
// original reading them as uint vertex inputs, its skinning code dead and gone) through vkn_preskin_lib.cpp, the record table rule of
// dm/gfx/nr/nr_preskin.h (nrps_match / nrps_id_* / nrps_nverts), the library's final words (vkn_shlib_b5_words with the draw's retype
// code) and the load-time candidate test (vkn_shlib.cpp step sl_preskin: xbg_spirv_preskin_candidate of the final words, kept per
// vertex shader while this switch is on: no other shader is ever looked at).
//
// Exactness (NR-PRESKIN.md section 3): consumer(inputs, slice(inputs)) == original(inputs) bit for bit under SPIR-V semantics (same
// instructions in the same order on the same inputs; the record carries raw bits in R32*_UINT formats; NoContraction exactly on the
// slice's float ops that fed an Invariant output). The proof on the player's pack: tests/nr_preskin_test.cpp + tools/dos2_preskin_check.py
// (interpreter, every output of every vertex: 0 differences of 12 406 runs + 7 204 superset runs). Anything the split cannot prove is
// refused at plan time (counted by the plan's reason); anything the draw cannot keep exact is refused per draw (counted by reason).
//
// The old mechanism, ported: per mesh instance (bone block range + vertex streams + the shader's value set) the slice runs ONCE per
// submission as a vertex-only draw with rasterizer discard (POINT_LIST over the mesh's own vertex buffers and formats: the same fixed-
// function fetch) writing the record into an arena block; every draw of the frame whose values are in a record reads it as one more
// vertex stream with its consumer module. The old path recorded the slices into the submission's PROLOGUE command buffer (ahead of the
// slot's command buffer) - no pass break, no dispatch in the frame. Here the same: this module's own command buffer gets the slices while
// the frame records, and is submitted in the frame's close listener (vkn_frame.h) right before the frame's own vkQueueSubmit; the frame's
// command buffer starts (begin listener) with a memory barrier VERTEX_SHADER write -> VERTEX_INPUT attribute read, whose first scope holds
// everything earlier in submission order: the prologue. Added per frame: one submission, one barrier at the top, 0 pass breaks, 0
// dispatches. Inputs of a slice are host-written (engine vertex buffers in the static arena, the constant ring): visible at the submit.
//
// The draw (vkn_fx.h [stage 5.3 V1] vkn_fx_out.vin): the consumer module as the vertex stage of whatever object the chain ends with; the
// engine's vertex format plus the record's groups (R32..R32G32B32A32_UINT at 4 * first of the record, stride 4 * record words) as a
// vertex-format content id (vkn_vfmt_put(0, ...)) on a stream the engine format does not read (2, 1 or 0; none free = refused), bound at
// the record's block. A record is per frame (presented recordings only; off-frame recordings are refused): arena blocks retire with the
// frame (vkn_retire_block, 3 frames: every recording that read them has finished).
//
// Keys: "once per mesh and submission" as the old model keys it: a record = (bone block identity: the vertex-stage uniform slot's ring
// offset + size this frame; every Location the slice reads: format, offset, stride, the vertex buffer's engine handle + content version
// (vkn_buf_version) + arena block + draw offset; every uniform slot the slice reads: ring offset + size) x the plan's value set; another
// shader's record is reused when it holds every value this draw needs under the same leaves (nrps_match: hash + leaf identities).
//
// Interplay (the old rule, xbg_nr_preskin.inc apply_old: the consumer replaces the vertex module whatever else chose the pipeline):
//   - shadow cache (order 100): a skipped draw never reaches this function; its REPLAYS do not pass the chain: vkn_shc.c replay() may
//     call vkn_preskin_apply below exactly like vkn_mat_apply (not edited here: the lead's merge);
//   - MAT / XLQ (order 200): run after this function and decide as before (no_fs, their swapped object); the consumer stays the vertex
//     module (vkn_fx_out.vin wins over the object's own vertex module): exact (the consumer computes the same outputs; MAT's dropped
//     fragment stage / XLQ's fragment module stay). The old path also NR-32-trimmed the consumer to what the fragment shader reads; here
//     the consumer is drawn untrimmed (counted "composed with a MAT / XLQ object"): exact, the varying cut of those draws is lost.
//   - occlusion / prepass / lightfuse / post: not skinned draws, or they skip (then the record was made for nothing: counted).
//
// Switch DM_VK_NATIVE_PRESKIN (replaces DM_NR_PRESKIN, forced 0 in mode 1 by dm_dos2_vkn_pre; read once at the library's init: the load
// step depends on it):
//   0  off: no candidate test at load, the chain function is not registered;
//   1  on (as above);
//   2  verify = the old count mode: every skinned draw is planned, its record looked up / made as 1 would (slices built and checked,
//      consumers built), nothing is drawn differently, no slice runs; the census shows what 1 would do;
//   3  poison: as 1, but the prologue is never submitted (records hold stale words): skinned characters break on screen.
//   Default: tilers (Mali / PowerVR, also DM_GFX_SIM_GPU mali / pvr) 1, every other family 0 (xbg_nr_preskin_default: A750 spends
//   0.05..0.14 ms skinning a frame; G57 model -1..-2.5 ms). Sub-switches: DM_VK_NATIVE_PRESKIN_MAXM (16: record words a vertex at most;
//   3 = the depth passes' position only), _MAXV (65536: vertices a record may cover), _MB (8: record bytes a frame).
//   Needs vertexPipelineStoresAndAtomics (without it: 2).
// Census line "preskin:" every 10 s from the chain: skinned draws by pass class, served, refused by reason, records (meshes pre-skinned)
// per frame, record bytes, slices / prologue submits / pass breaks (0) per frame, plans / consumers / slice pipelines; red lines 【红】.
// No game addresses. Recording thread only, except the install and the library's load step.
#ifndef DM_VKN_PRESKIN_H
#define DM_VKN_PRESKIN_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// ---- the switch (one reader: the library's load step and the install use it) ----
static inline int vkn_preskin_default(int family) { return family == 2 || family == 3 ? 1 : 0; }   // VKN_FAM_MALI / VKN_FAM_POWERVR (vkn_policy.h)
int vkn_preskin_switch(int family);                // DM_VK_NATIVE_PRESKIN (0..3, else the family default), read once

// ---- the library side (vkn_preskin_lib.cpp): xbg_spirv_preskin.cpp for C ----
enum { VKN_PSK_MAXV = 16, VKN_PSK_UBOS = 8 };
typedef struct vkn_psk_plan {
    int ok; char why[96];
    uint32_t m, nvals;                             // record words a vertex, cut values (record order = sorted by hash)
    struct { uint64_t hash; uint32_t comps, first, loc_mask, ubo_mask; } val[VKN_PSK_MAXV];   // ubo_mask: bit i = ubo_binding[i]
    uint32_t nubo, ubo_binding[VKN_PSK_UBOS], ubo_key[VKN_PSK_UBOS];   // uniform blocks the slice reads (set 0 bindings, their keys)
    uint32_t slice_locs, cons_locs, orig_locs, free_locs;              // Location masks
    uint32_t ssbo_binding, bone_binding, kdep, bone_loads, ops_orig, ops_cons, ops_slice;
    void *priv;                                    // the words + the C++ plan (vkn_psk_plan_free)
} vkn_psk_plan;
typedef struct vkn_psk_layout { uint32_t groups, loc[16], first[16], width[16]; } vkn_psk_layout;
int vkn_psk_candidate(const uint32_t *w, size_t n);                    // 1 = reads a uniform block with a vertex-input index
int vkn_psk_plan_make(const uint32_t *w, size_t n, const uint32_t *bind, const uint32_t *key, uint32_t nbind, vkn_psk_plan *out);
void vkn_psk_plan_free(vkn_psk_plan *p);
int vkn_psk_slice(const vkn_psk_plan *p, uint32_t **out, size_t *n, const char **why);
int vkn_psk_consumer(const vkn_psk_plan *p, const uint32_t *pos, uint32_t **out, size_t *n, vkn_psk_layout *lay, const char **why);

// ---- the backend side (vkn_preskin.c) ----
struct vkn_draw_in; struct vkn_fx_ctx; struct vkn_fx_out;
// Reads the switch (family of the library's device), creates the objects, registers "preskin" (order 150) and the frame listeners.
// Idempotent. The library's init calls it (vkn_shlib.cpp, after vkn_b5_install).
void vkn_preskin_install(uint32_t vendor, const char *device_name);
int vkn_preskin_mode(void);                        // in effect (0 = off / not installed)
// The chain's decision for a draw that does not pass the chain (vkn_shc.c replay(), like vkn_mat_apply): fills out->vin as the chain
// function would; 1 = set. Recording thread; the draw's constant blocks must be announced (vkn_draw_ubo) before.
int vkn_preskin_apply(const struct vkn_draw_in *in, const struct vkn_fx_ctx *ctx, struct vkn_fx_out *out);

enum { VKN_PSK_W_NOTCAND, VKN_PSK_W_NOLIB, VKN_PSK_W_PLAN, VKN_PSK_W_WIDE, VKN_PSK_W_CODES, VKN_PSK_W_OFFFRAME, VKN_PSK_W_ATTR, VKN_PSK_W_INST,
       VKN_PSK_W_UNBOUND, VKN_PSK_W_STRIDE, VKN_PSK_W_BIG, VKN_PSK_W_UBO, VKN_PSK_W_ARENA, VKN_PSK_W_POOL, VKN_PSK_W_PIPE, VKN_PSK_W_CONS,
       VKN_PSK_W_STREAM, VKN_PSK_W_VFMT, VKN_PSK_W_IDS, VKN_PSK_W_PRIM, VKN_PSK_W_LATE, VKN_PSK_W_N };
typedef struct vkn_preskin_stats {
    uint64_t draws, cand[2], cand_idx[2];          // draws seen; skinning-candidate draws [0 depth-only / 1 colour] and their indices
    uint64_t why[VKN_PSK_W_N], why_idx[VKN_PSK_W_N];
    uint64_t served[2], served_idx[2], own[2], sup[2], composed;   // drawn from a record (verify: would be); own record / another shader's; + MAT / XLQ object
    uint64_t recs[2], rec_verts[2], rec_bytes, slices, submits, frames, ops_saved, ops_slice, reds;
    uint64_t plans, plans_refused, consumers, slice_pipes, vfmts;   // totals
} vkn_preskin_stats;
void vkn_preskin_stats_get(vkn_preskin_stats *s);
void vkn_preskin_census(void);
const char *vkn_preskin_reason_name(int w);

// ---- host tests ----
// Stand-ins for the library (the C host test links no C++): the candidate flag of a vertex shader object, its plan for a retype code
// (out->priv is the stand-in's), the slice / consumer words of a plan (malloc'd). NULL lib = the library (vkn_shlib_* / vkn_psk_*).
// mode: the switch (registers the functions once; objects made on the first mode 1 / 3); every verdict and the frame state forgotten.
typedef struct vkn_preskin_test_lib {
    int (*candidate)(const void *shader);
    int (*plan)(const void *shader, uint64_t code, vkn_psk_plan *out);
    int (*slice)(const vkn_psk_plan *p, uint32_t **out, size_t *n, const char **why);
    int (*consumer)(const vkn_psk_plan *p, const uint32_t *pos, uint32_t **out, size_t *n, vkn_psk_layout *lay, const char **why);
} vkn_preskin_test_lib;
void vkn_preskin_test_setup(int mode, const vkn_preskin_test_lib *lib);

#ifdef __cplusplus
}
#endif
#endif
