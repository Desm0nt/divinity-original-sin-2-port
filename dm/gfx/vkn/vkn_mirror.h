// [DOS2] VK-NATIVE stage 4 (vkn_mirror.c): which rows of a texture the engine writes through its CPU storage changed since the
// last time they were mirrored into the native image. No game addresses, no engine offsets, no Vulkan: the caller (dm/host/
// hle_dos2_vkn_glue.c) owns the storage pointers and does the uploads.
//   The rows of a region are cut into bands of VKN_MIRROR_BAND rows; each band keeps a 64-bit hash of the bytes it had at its last
//   mirror. vkn_mirror_diff hashes the region now, compares band by band, stores the new hashes and returns the changed bands as runs
//   (adjacent changed bands merged into one run). A region seen for the first time (or after vkn_mirror_forget, or with another
//   generation word / geometry) is one run over all its rows ("full").
//   Exact up to a 64-bit hash collision of a changed band (probability 2^-64 per changed band).
//   The caller must forget a key whenever the native image changed by any other way (another writer's upload, a GPU write, an
//   upload that failed): the hashes describe what the IMAGE holds, not what the storage held.
// Thread safe (one mutex); a bounded table of VKN_MIRROR_KEYS regions, least recently used one evicted.
#ifndef DM_VKN_MIRROR_H
#define DM_VKN_MIRROR_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_MIRROR_BAND = 16, VKN_MIRROR_KEYS = 64, VKN_MIRROR_RUNS = 32 };
typedef struct vkn_mirror_run { uint32_t y, rows; } vkn_mirror_run;

// 64-bit hash of n bytes (4 lanes of the XXH64 round; deterministic, endian of the host).
uint64_t vkn_mirror_hash(const void *p, size_t n, uint64_t seed);

// The changed runs of the region `rows` rows x `rowbytes` bytes at src, rows `stride` bytes apart (stride >= rowbytes), of key
// (the caller's texture identity) with generation word gen (anything that changes when the image is replaced). At most max_runs
// runs (1..VKN_MIRROR_RUNS; more changed runs are merged into the last one, so a run may cover unchanged bands). *full = 1 when
// the whole region is returned because nothing was known. Returns the number of runs (0 = nothing changed), -1 = bad arguments.
// The stored hashes are those of the region now: the caller uploads the runs (or calls vkn_mirror_forget when it could not).
int vkn_mirror_diff(uint64_t key, uint64_t gen, const uint8_t *src, uint32_t stride, uint32_t rowbytes, uint32_t rows, vkn_mirror_run *runs, int max_runs, int *full);
void vkn_mirror_forget(uint64_t key);    // the next diff of the key is full
void vkn_mirror_forget_all(void);

typedef struct vkn_mirror_stats {
    uint64_t diffs, full, unchanged;      // calls; of them full; of them with nothing changed
    uint64_t bands, dirty_bands, merged;  // bands compared (not full), changed among them, runs merged over max_runs
    uint64_t hashed_bytes, evicted, forgot, no_mem;
} vkn_mirror_stats;
void vkn_mirror_stats_get(vkn_mirror_stats *s);

#ifdef __cplusplus
}
#endif
#endif
