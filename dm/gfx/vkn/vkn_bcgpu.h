// [DOS2] VK-NATIVE stage 4: BC textures on a GPU without BC, the GPU steps (vkn_bcgpu.c). vkn_tex decides the storage of a BC texture
// the device cannot sample (or under DM_GFX_SIM_GPU=mali / powervr) in this order:
//   1. ASTC blocks from the setup pack (vkn_astc.h: BC1..BC3 2D textures of packaged storage);
//   2. GPU ASTC 4x4: the exact GPU BC decode (xbg_bc_decode.comp, KB TEX-01 / TEX-02) followed by the GPU ASTC 4x4 encoder of
//      port-common bc-astc-gpu (xbg_bc_astc_encode.comp, KB TEX-18 = FN-60 + FN-61), at upload; BC1 / BC1A / BC2 / BC3 / BC7;
//   3. GPU RGBA8: the same decode, its RGBA8 output copied into the image; BC1 / BC1A / BC2 / BC3 / BC7;
//   4. CPU decode (nvn_bcdec.h) at creation: everything else (BC4 / BC5 / BC6H, 3D, multisampled, a format whose GPU decode failed its
//      self-test, DM_GFX_BC_DECODE=cpu, a build without the translator doors).
// (BC4 / BC5 / BC6H stay on the CPU: the decoder's output formats for them follow the translator's globals xbg_bc_8bit_channels /
// xbg_bc6_e5, the CPU decoder vkn_tex uses writes R8 / RG8 / RGBA16F - one texture must not get two formats by route.)
//
// The device decides once (vkn_bcgpu_route, at the first BC texture that needs it, on the creating thread):
//   - decoder: one compute pipeline per NVN format (specialization fmt, options 0); each new pipeline first decodes the 2048 known-answer
//     blocks of TEX-02 (xbg_bc_kat_blocks) through the real descriptor set and is kept only if every pixel equals the translator's CPU
//     decoder (xbg_decode_block). DM_GFX_BC_SELFTEST: 1 (default) failed format -> CPU; 2 log only, keep; 0 no test.
//   - encoder (route 2): DM_GFX_BC_ASTC auto (default: on, except on Qualcomm, where this path only runs simulated) / 1 (force) / 0
//     (off); ASTC 4x4 UNORM + sRGB with SAMPLED, FILTER_LINEAR, TRANSFER_SRC / DST; the pipeline (DM_GFX_BC_ASTC_CANDS 3 / 1) passes the
//     module's self-test once (285 blocks encoded on this GPU, decoded on the CPU by xbg_astc_ref_decode.h: no error / all-zero block and
//     every MSE <= host * 1.25 + 10; an all-zero read is re-read, then the test re-run once); a failed test turns route 2 off for the
//     process. Per family (a BC format and its sRGB twin) both decode pipelines must have passed theirs. Per texture the module's shape
//     rules (xbg_bc_astc_shape: not storage, single-sampled, 2D-like, no VT-cache shape / BC3 array unless DM_GFX_BC_ASTC_VT=1) and the
//     image format properties.
//   - DM_GFX_BC_DECODE=cpu turns routes 2 and 3 off (the translator's switch); DM_GFX_BC_BATCH_MB (32) sizes the scratch.
//
// Upload model (vkn.h): the creating thread (an engine loader thread) builds a job - the BC blocks of every (level, layer) as tight block
// rows, cut into batches that fit the scratch (a level bigger than a batch is cut into bands of block rows) - in one staging block
// (VKN_ARENA_STAGING: per batch the decoder's region table, 8 words a region, then the raw blocks: xbg_bc_gpu.h XbgBcGpuBatch). vkn_tex
// queues it with vkn_upload_push; whoever records the queued uploads (the start of the next recording in mode 1, vkn_upload_flush's own
// submission in mode 2) calls vkn_bcgpu_job_record: per batch a full barrier, copy staging -> the module's input buffer, decode dispatch
// (-> encode dispatch), copy of the RGBA8 / ASTC blocks into the image (TRANSFER_DST_OPTIMAL; vkn_tex does the layout changes around it).
// The scratch buffers (input, RGBA8 output, ASTC output; device local) and both descriptor sets are this module's, fixed after start-up
// (no descriptor update ever touches a set a pending command buffer uses); every batch begins with an ALL_COMMANDS barrier, whose first
// scope covers every earlier submission, so batches of different command buffers reuse the scratch in queue order. Never inside a render
// pass (dispatches and copies: the upload queue runs outside one).
// Thread safety: one mutex over the device state (taken only by vkn_bcgpu_route / reset; never while recording); recording reads state
// fixed before the job existed; counters atomic.
#ifndef DM_VKN_BCGPU_H
#define DM_VKN_BCGPU_H
#include "vkn.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_BCG_NONE = 0, VKN_BCG_ASTC = 1, VKN_BCG_RGBA8 = 2 };
// Why a BC texture got no GPU route (VKN_BCG_NONE -> CPU decode): _OFF DM_GFX_BC_DECODE=cpu, _DOOR no translator doors in this build,
// _FMT BC4 / BC5 / BC6H (not GPU-decoded by this backend), _SHAPE 3D / 1D / multisampled, _DEV scratch buffers / layouts / module failed,
// _PIPE the format's decode pipeline failed or failed its self-test
enum { VKN_BCGW_OFF, VKN_BCGW_DOOR, VKN_BCGW_FMT, VKN_BCGW_SHAPE, VKN_BCGW_DEV, VKN_BCGW_PIPE, VKN_BCGW_N };
// Why a texture of an encoded family stayed RGBA8 (route 3 instead of 2): _OFF DM_GFX_BC_ASTC=0, _QCOM auto on Qualcomm, _FEAT ASTC 4x4
// features, _ENC encoder pipeline / self-test failed, _FAMILY a decode pipeline of the family failed, _SHAPE the module's shape rules,
// _PROPS image format properties of the shape
enum { VKN_BCAK_OFF, VKN_BCAK_QCOM, VKN_BCAK_FEAT, VKN_BCAK_ENC, VKN_BCAK_FAMILY, VKN_BCAK_SHAPE, VKN_BCAK_PROPS, VKN_BCAK_N };

// The GPU route of a BC texture (NVN format, NVN target, size, levels, array layers (cube: x 6), samples, storage use) that the setup
// pack did not take. May run the self-tests (first call per device / format: a one-shot submission and a wait on this thread). *why: a
// VKN_BCGW_* when VKN_BCG_NONE is returned.
int vkn_bcgpu_route(uint32_t nvn, uint32_t target, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers, uint32_t samples, int storage, int *why);
// The VkFormat route 2 stores (ASTC 4x4 UNORM / sRGB by the NVN format) and route 3 stores (RGBA8 UNORM / sRGB).
VkFormat vkn_bcgpu_format(uint32_t nvn, int route);

// ---- jobs ----
typedef struct vkn_bcg_job vkn_bcg_job;
vkn_bcg_job *vkn_bcgpu_job_new(uint32_t nvn, int route);   // NULL = no memory / not a GPU route / the format has no pipeline
// One rectangle of one (level, layer): texels x, y (multiples of 4), w, h (clipped by the caller to the level), its BC blocks in rows of
// ceil(w / 4) blocks, `pitch` bytes apart (ceil(h / 4) rows). Copied during the call. 0 = ok, -1 = no memory / bad rectangle.
int vkn_bcgpu_job_add(vkn_bcg_job *j, uint32_t level, uint32_t layer, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *blocks, uint64_t pitch);
// Writes every batch into one staging block; *bytes = its size (for vkn_upload_push). 0 = ok (-1: no staging memory, nothing added).
int vkn_bcgpu_job_stage(vkn_bcg_job *j, uint64_t *bytes);
// Records the job into cb (outside a render pass; img in TRANSFER_DST_OPTIMAL, colour aspect). Leaves the copies' writes to the caller's
// following barrier.
void vkn_bcgpu_job_record(VkCommandBuffer cb, vkn_bcg_job *j, VkImage img);
void vkn_bcgpu_job_free(vkn_bcg_job *j);   // the staging block retired (vkn_retire_block), the job freed; NULL ok
uint32_t vkn_bcgpu_job_batches(const vkn_bcg_job *j);

typedef struct vkn_bcgpu_stats {
    int ready;                                   // 0 not tried, 1 GPU decode available, -1 not available (why[]: the reason)
    int astc;                                    // route 2: 0 not decided, 1 on, -1 off (astc_off: VKN_BCAK_* of the decision)
    int astc_off;
    char line[200];                              // the encoder self-test verdict line (empty until it ran)
    uint32_t kat_ok, kat_bad;                    // decoder self-test: NVN formats (bit fmt - 0x41) passed / failed
    uint64_t routed[3], why[VKN_BCGW_N], kept[VKN_BCAK_N];   // route decisions by VKN_BCG_*; reasons
    uint64_t jobs, batches, dec_dispatch, enc_dispatch, blocks, in_bytes, copies, recorded;   // built / recorded GPU work
    uint64_t build_ns, prep_ns;                  // job building (copying the blocks into staging) / device preparation incl. self-tests
    uint64_t scratch_bytes;                      // device-local scratch (input + RGBA8 + ASTC)
} vkn_bcgpu_stats;
void vkn_bcgpu_get_stats(vkn_bcgpu_stats *s);
// Tests only: destroys the device objects (no job may be pending) and forgets every decision and switch (read again at the next route).
void vkn_bcgpu_reset(void);

// ---- the data doors (vkn_shlib_bc.cpp: the shader library piece moved them out of xbg_game.cpp's [DOS2] VK-NATIVE export block; pure
// functions of their arguments). vkn_bcgpu.c declares them weak (VKN_BCG_DOOR): a build without them has no GPU route. ----
#ifndef VKN_BCG_DOOR
#define VKN_BCG_DOOR
#endif
// SPIR-V / table words: which 0 = the GPU BC decoder, 1 = the ASTC 4x4 encoder, 2 = the encoder's tables (storage buffer, binding 3).
const uint32_t *vkn_shlib_bc_blob(int which, size_t *bytes) VKN_BCG_DOOR;
// Decoder self-test of fmt (BC1..BC3 / BC7 only): the batch input (2048 known-answer blocks, one region of 64 x 32 blocks) into in;
// *in_bytes, *out_bytes (the RGBA8 output it writes), push = its push constants. 0 = ok.
int vkn_shlib_bc_kat_input(uint32_t fmt, void *in, size_t in_cap, size_t *in_bytes, size_t *out_bytes, uint32_t push[4]) VKN_BCG_DOOR;
// Its verdict: bad blocks against the CPU decoder (0 = pass, -1 = not checkable); line: the first bad block.
int vkn_shlib_bc_kat_check(uint32_t fmt, const void *out, size_t out_bytes, char *line, size_t cap) VKN_BCG_DOOR;
// Encoder self-test: batch input into in, the decoder-layout RGBA8 texels into rgba, one dispatch per category (push[i] = regions,
// first, end, flags). 0 = ok.
int vkn_shlib_bca_kat_input(void *in, size_t in_cap, size_t *in_bytes, void *rgba, size_t rgba_cap, size_t *rgba_bytes, uint32_t push[][4],
                          uint32_t max_disp, uint32_t *ndisp, uint32_t *blocks) VKN_BCG_DOOR;
// Its verdict over blocks x 16 bytes: 1 = pass; *zero = some block read all zero; line: the log text.
int vkn_shlib_bca_kat_check(const void *enc, uint32_t cands, int *zero, char *line, size_t cap) VKN_BCG_DOOR;
uint32_t vkn_shlib_bca_flags(uint32_t nvn) VKN_BCG_DOOR;   // the encoder's push flags of an NVN format, 0 = not an encoded family
const char *vkn_shlib_bca_shape(uint32_t nvn, int target, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers, int samples, int storage,
                              int vt) VKN_BCG_DOOR;   // why such a texture stays RGBA8, NULL = may be ASTC

#ifdef __cplusplus
}
#endif
#endif
