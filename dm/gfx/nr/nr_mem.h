// [DOS2] NR-29 DM_NR_MEM (docs/dos2/nr/NR-29.md): one RAM-based budget for the native renderer's caches, and the order in which a
// memory trim frees them. Pure C, no Vulkan, no host: the front end is dm/host/hle_dos2_nr_mem.c, the engine side
// dm/gfx/vulkan/engine/xbg_nr_mem.inc; host test dm/gfx/vulkan/engine/tests/nr_mem_test.c.
//
// The caches (each had its own default, together up to ~600-700 MB on a 6-8 GB phone with NR-05 / 06 / 22 on, beside a 3285 MB guest heap):
//   NRM_TEXBLOB    NR-05 heap blobs of material textures (DM_NR_TEX_PREP_MB, default RAM / 64, 32..128 MB)
//   NRM_TEXSTREAM  NR-22 prepared VkImages + staging not taken yet (DM_NR_TEXSTREAM_MB, default 2 x the NR-05 default)
//   NRM_BUFSTATIC  NR-06 immutable copies of static vertex / index buffers (DM_NR_BUF_MB, default 192)
//   NRM_RING       NR-06 constant ring mirror (ring size + 64 KB = 19.5 MB on DOS2): fixed, never capped, only reserved in the split
//   NRM_ASTCQ      the lazy ASTC encoder's queue of storage copies (DM_TEX_ASTC_QUEUE_MB, default 96)
//   NRM_PIPE       the warm-up's rewritten SPIR-V kept for load_shader (gfx-shader-reuse, no cap before: 6-13 MB seen)
// An explicit DM_* MB variable of a cache always wins over the plan (the plan only replaces defaults, and only lowers them).
#ifndef DM_NR_MEM_H
#define DM_NR_MEM_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NRM_TEXBLOB, NRM_TEXSTREAM, NRM_BUFSTATIC, NRM_RING, NRM_ASTCQ, NRM_PIPE, NRM_N };
const char *nrm_name(int which);
// RAM classes by MemTotal (what the kernel reports: a "6 GB" phone shows ~5.3-5.7 GiB, an "8 GB" one ~7.2-7.6, a "12 GB" ~11).
enum { NRM_T_4GB, NRM_T_6GB, NRM_T_8GB, NRM_T_BIG, NRM_TIERS };
const char *nrm_tier_name(int tier);
int nrm_tier(uint64_t ram_bytes);             // <4.75 GiB 4 GB class, <6.75 GiB 6 GB, <10 GiB 8 GB, else 12+ (no cap)
uint64_t nrm_tier_budget(int tier);           // bytes for all caps together; 0 = no cap (12+ GB: every cache keeps its old default)
#define NRM_RING_RESERVE (20ull << 20)        // the NR-06 ring mirror (19.5 MB + 64 KB slack), reserved when NR-06 is on

typedef struct nrm_plan {
    uint64_t ram, budget;      // budget 0 = no cap
    int tier;
    unsigned on;               // 1 << NRM_* of the caches the switches turn on (the split goes to these only)
    uint64_t cap[NRM_N];       // 0 = no cap (the cache keeps its default); NRM_RING is never capped
    uint64_t want;             // sum of the shares before the floors (diagnostic)
    int floored;               // the budget was below the floors' sum: the floors were kept (over budget)
} nrm_plan;
// The split: budget minus the ring reserve (when NRM_RING is on), shared by weight among the caches that are on (TEXBLOB 2, TEXSTREAM 4,
// BUFSTATIC 4, ASTCQ 2, PIPE 1), each share raised to its floor (16 / 32 / 32 / 16 / 4 MB) and rounded down to 1 MB.
void nrm_plan_make(nrm_plan *p, uint64_t ram_bytes, unsigned on);
// What a cache uses: min(its default, its cap); a default of UINT64_MAX ("no cap before") gets the cap; no cap -> the default.
uint64_t nrm_apply(const nrm_plan *p, int which, uint64_t dflt);

// ---- trims ----
// Trim tiers, cheapest to rebuild first (each includes the ones before):
//   NRM_TRIM_1  speculative work nothing used yet: NR-05 blobs / NR-22 prepared images not taken + free NR-22 staging chunks, the ASTC
//               encoder's waiting queue (re-offered at a later upload or run). The old path redoes any of it on demand.
//   NRM_TRIM_2  + the warm-up's rewritten SPIR-V (load_shader rewrites again: 1-9 ms per big shader) and the NR-06 static copies (copied
//               again at their next bind, on the translation thread: only the meshes still drawn come back)
//   NRM_TRIM_3  + warm pipelines built and not taken yet (a draw that needs one compiles it, the VkPipelineCache makes that a cache hit)
// The ring mirror is never trimmed (every frame rebuilds it at once: nothing would be saved).
enum { NRM_TRIM_NONE, NRM_TRIM_1, NRM_TRIM_2, NRM_TRIM_3, NRM_TRIM_N };
// ComponentCallbacks2.onTrimMemory level -> tier: 5 RUNNING_MODERATE 1; 10 RUNNING_LOW 2; 15 RUNNING_CRITICAL 3; 20 UI_HIDDEN 2;
// 40 BACKGROUND, 60 MODERATE, 80 COMPLETE 3; other values below 5 none, unknown values >= 5 by the next lower known level.
int nrm_trim_tier(int level);
// The native poll (Android 14+ stops delivering the RUNNING_* levels to apps): MemAvailable at or below max(8 % of RAM, min(2 x the
// ActivityManager threshold, 12 %)) -> 1; at or below max(4 %, min(1.25 x threshold, 6 %)) -> 2; never 3 (the game is in front: no
// pipeline hitches). threshold 0 = unknown (the RAM fractions alone). avail 0 = unknown -> none.
int nrm_pressure(uint64_t avail, uint64_t ram, uint64_t threshold);
// One trim of a tier at most every gap ns (a higher tier passes at once, 1 -> 2 -> 3); 1 = go.
typedef struct nrm_gate { uint64_t last[NRM_TRIM_N]; } nrm_gate;
int nrm_gate_pass(nrm_gate *g, int tier, uint64_t now, uint64_t gap);

// ---- usage / freed slots shared by the front end and the engine (bytes; NRM_U_PIPES is a count) ----
//   TEXBLOB   NR-05 table: heap blobs + NR-22 staging of objects not taken (front end)   TS_IMG   NR-22 image memory of objects not taken
//   TS_FREE   NR-22 free staging chunks kept for reuse                                    BUFSTATIC NR-06 static copy chunks
//   RING      NR-06 ring mirror                                                           ASTCQ    storage copies waiting for the encoder
//   SPIRV     warm-up SPIR-V kept for load_shader                                         PIPES    warm pipelines built, not taken
enum { NRM_U_TEXBLOB, NRM_U_TS_IMG, NRM_U_TS_FREE, NRM_U_BUFSTATIC, NRM_U_RING, NRM_U_ASTCQ, NRM_U_SPIRV, NRM_U_PIPES, NRM_U_N };
uint64_t nrm_usage_bytes(const uint64_t u[NRM_U_N]);   // the byte slots summed (PIPES left out)

// "<key>:   <n> kB" of /proc/meminfo text -> bytes; 0 = not found
uint64_t nrm_meminfo(const char *text, const char *key);

#ifdef __cplusplus
}
#endif
#endif
