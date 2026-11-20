// [DOS2] VK-NATIVE (DM_VK_NATIVE, default 0): the engine's own render backend in Vulkan - no NVN, no HLE NVN, no nvn-translate on its
// path. Design: docs/dos2/nr/VK-NATIVE.md; the engine interface it stands behind: docs/dos2/nr/VK-NATIVE-MAP-1..3.md; state of the work:
// docs/dos2/nr/VK-NATIVE-STATUS.md.
//
// This header is the core every vkn module builds on (vkn_core.c): the mode, the Vulkan device of the front end, memory (arenas of
// persistently mapped host-visible buffers, device-local image memory), the frame counter with deferred destruction, the upload queue.
// Rules for every file of dm/gfx/vkn:
//   - no game addresses and no engine record offsets (dm/host/hle_dos2_vkn.c owns them and passes plain arguments);
//   - no NVN, no HLE NVN, no translator (xbg_game.cpp) state. The only doors to the rest of the port: dm_vk_handles / the queue lock
//     (android/vulkan_present.cpp), xbg_vkn_shader (shader modules of the packs), dm_present_async (the presenter);
//   - plain C, objects keyed by the ENGINE's handle (u64: low 32 bits index, bits 32..53 generation), tables indexed by the index;
//   - only the Vulkan entry points of dm/android/vk_tramp_names.h exist (122 names: no vkCmdSetStencilReference, no
//     vkResetDescriptorPool, no vkFreeDescriptorSets, no vkFreeCommandBuffers).
// Modes (vkn_mode): 0 off - nothing of this runs; 2 "alongside" - the old path draws, the backend only creates its resources from
// the same engine events (the whole-stage check of the resources); 1 native - the backend owns what it covers.
#ifndef DM_VKN_H
#define DM_VKN_H
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_OFF = 0, VKN_NATIVE = 1, VKN_ALONGSIDE = 2 };
int vkn_mode(void);                 // DM_VK_NATIVE as read at install (vkn_set_mode); 0 until then
void vkn_set_mode(int mode);        // dm/host/hle_dos2_vkn.c, install time only
void vkn_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   // one line, prefix "[vkn] "; red lines start with "【红】"
uint64_t vkn_now_ns(void);          // CLOCK_MONOTONIC

static inline uint32_t vkn_h_idx(uint64_t h) { return (uint32_t)h; }
static inline uint32_t vkn_h_gen(uint64_t h) { return (uint32_t)(h >> 32) & 0x3fffffu; }

// ---- the device (owned by the Android front end; valid once it exists, i.e. before the first guest instruction runs) ----
typedef struct vkn_dev {
    VkInstance instance; VkPhysicalDevice phys; VkDevice dev; VkQueue queue; uint32_t family;
    VkPhysicalDeviceMemoryProperties mem; VkPhysicalDeviceProperties props; VkPhysicalDeviceFeatures feat;
} vkn_dev;
const vkn_dev *vkn_device(void);    // NULL while the front end has no device (then nothing of the backend may run)
// The one VkQueue of the port: every vkQueueSubmit / vkQueueWaitIdle of the backend is bracketed by these.
void vkn_queue_lock(void);
void vkn_queue_unlock(void);
// Memory type for (requirement bits, wanted flags); prefer: extra flags tried first. -1 = none.
int vkn_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer);
// Device-local memory for an image (one allocation per image; images are few thousand and long-lived). 0 = ok.
int vkn_image_mem(VkImage img, VkDeviceMemory *out, uint64_t *bytes);

// ---- arenas: host-visible, persistently mapped VkBuffers (16 MB chunks, bigger for a bigger request) with every buffer usage the
// backend needs (vertex, index, uniform, storage, transfer source and destination). A block is (buffer, offset, size, CPU pointer).
// Thread safe. class: VKN_ARENA_STATIC (engine vertex / index buffers), VKN_ARENA_STAGING (upload sources), VKN_ARENA_RING. ----
enum { VKN_ARENA_STATIC, VKN_ARENA_STAGING, VKN_ARENA_RING, VKN_ARENAS };
typedef struct vkn_block { VkBuffer buf; uint64_t off, size; void *cpu; uint32_t chunk; uint8_t cls; } vkn_block;
int vkn_block_alloc(int cls, uint64_t size, uint32_t align, vkn_block *out);   // 0 = ok; align a power of two (0 = 16)
void vkn_block_free(const vkn_block *b);                                       // at once; use vkn_retire for anything a frame may read
void vkn_arena_stats(uint64_t used[VKN_ARENAS], uint64_t reserved[VKN_ARENAS]);

// ---- frames and deferred destruction. The frame number advances at the end of every presented frame (vkn_frame_end). A retired
// object is destroyed once 3 more frames ended (the engine's own rule is 2; one more for the command buffer ring). ----
uint64_t vkn_frame(void);
void vkn_frame_end(void);                               // runs the due retirements
void vkn_retire(void (*fn)(void *ctx), void *ctx);      // thread safe
void vkn_retire_block(const vkn_block *b);              // vkn_block_free after the delay
void vkn_retire_all(void);                              // shutdown / tests: everything now

// ---- the upload queue. Loader threads create objects at once and queue the data copies; the render thread records them at the
// start of the next frame's command buffer (vkn_upload_run). In mode 2 nobody records frames: vkn_upload_flush() records and submits
// what is queued on the calling thread and waits (called from the 10 s census of hle_dos2_vkn.c and when 64 MB are queued). ----
typedef void (*vkn_upload_fn)(VkCommandBuffer cb, void *ctx);   // records copies / layout changes; frees ctx' staging via vkn_retire_block
void vkn_upload_push(vkn_upload_fn fn, void *ctx, uint64_t bytes);
int vkn_upload_run(VkCommandBuffer cb);                 // records every queued upload into cb; returns how many
int vkn_upload_flush(void);                             // own command buffer + submit + wait; returns how many (0 = nothing queued)
uint64_t vkn_upload_pending_bytes(void);

// ---- one-shot command buffers (uploads in mode 2, read-backs): begin, record, vkn_once_end submits under the queue lock and waits ----
VkCommandBuffer vkn_once_begin(void);                   // VK_NULL_HANDLE = failed; serialised by an internal lock until vkn_once_end
int vkn_once_end(VkCommandBuffer cb);                   // 0 = ok

// ---- small generic pieces ----
// Open-addressing table "u32 engine handle index -> record": a module keeps `n` records of `rec` bytes and a handle per record.
typedef struct vkn_tab { void *rec; uint64_t *handle; uint32_t n, rec_bytes; uint64_t puts, kills, replaced, overflow; } vkn_tab;
int vkn_tab_init(vkn_tab *t, uint32_t n, uint32_t rec_bytes);                  // 0 = ok
void *vkn_tab_put(vkn_tab *t, uint64_t handle, int *was_alive);               // the zeroed-or-old record of the index; NULL = out of range
void *vkn_tab_get(const vkn_tab *t, uint64_t handle);                          // the record of this very handle, else NULL
void *vkn_tab_at(const vkn_tab *t, uint32_t idx);                              // by index, alive or not (NULL = out of range)
int vkn_tab_kill(vkn_tab *t, uint64_t handle);                                 // 1 = was alive under this handle (record left for the caller to clean BEFORE the call)
static inline uint64_t vkn_fnv(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p; uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

#ifdef __cplusplus
}
#endif
#endif
