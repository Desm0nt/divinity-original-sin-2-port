// [DOS2] VK-NATIVE buffers (vkn_buf.c): the engine's vertex and index buffers and the constant ring, owned by the native backend.
// No game addresses, no engine record offsets (dm/host/hle_dos2_vkn_buf.c owns them and passes plain arguments).
//   - Buffers are keyed by the ENGINE's handle (u64: low 32 bits index, bits 32..53 generation), one table per kind. Each live buffer
//     is one block of VKN_ARENA_STATIC (host-visible, coherent, persistently mapped): the engine writes vertices / indices straight
//     through the block's CPU pointer at any time, the GPU reads (block.buf, block.off + offset). No copies on any GPU.
//   - A destroyed buffer's block goes through vkn_retire_block (3 presented frames after the destroy, which is never earlier than the
//     engine's own rule "last use + 2 < frame": the destroy comes after the last use).
//   - Mode 2 (alongside) only notes buffers (vkn_buf_note: size / usage / init, no memory: the old path owns the one CPU pointer).
//   - The constant ring: one block of VKN_RING_BYTES from VKN_ARENA_RING, created on first use, handed out by the engine's own rule.
// Thread safe (one mutex for the tables and the counters, one for the ring): the engine creates buffers on loader threads.
#ifndef DM_VKN_BUF_H
#define DM_VKN_BUF_H
#include "vkn.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_BUF_VB = 0, VKN_BUF_IB = 1, VKN_BUF_KINDS = 2 };
enum { VKN_BUF_USAGE_DYNAMIC = 3 };        // the engine's usage byte: 3 = written every frame (its pool 5), anything else static (pool 4)
enum { VKN_BUF_TAB = 1u << 20 };           // handle indices per kind (the engine aborts on indices above 0xfffff in its tables)
#define VKN_RING_BYTES 0x1380000u          // the engine's constant ring (19.5 MB)

// The pseudo GPU address of a block (what the engine keeps as "GPU address" of a native buffer and does arithmetic on: + offset).
// TAG in bits 48..55, the arena chunk in bits 36..47, the offset inside the chunk's VkBuffer in bits 0..35. Never a real address
// (NVN / translator GPU addresses are 40-bit), so a stray use on the old path cannot alias real memory.
#define VKN_BUF_VA_TAG 0x00b0000000000000ull
static inline uint64_t vkn_buf_va(const vkn_block *b) { return VKN_BUF_VA_TAG | (uint64_t)(b->chunk & 0xfffu) << 36 | (b->off & 0xfffffffffull); }
// va (+ any offset the engine added) -> (VkBuffer, offset in it). 0 = ok; -1 = not a pseudo address of a live chunk.
int vkn_buf_resolve(uint64_t va, VkBuffer *buf, uint64_t *off);

typedef struct vkn_buf_info {
    vkn_block blk;            // blk.size == 0: noted only (mode 2), no memory
    uint64_t size;            // engine size in bytes (the block may be bigger: rounded to 16, at least 16)
    uint32_t index_size;      // IB: 2 or 4 (anything else: the engine leaves its index type unset); VB: 0
    uint8_t usage, kind, has_init, pad;
} vkn_buf_info;

// Native create (mode 1): a block of max(size, 16) bytes, init copied when given (size bytes), else zeroed. Replaces a live record of the same
// index (its block retired). 0 = ok, *out = the block; -1 = no device / no memory / index out of range.
int vkn_buf_create(int kind, uint64_t handle, uint64_t size, unsigned index_size, unsigned usage, const void *init, vkn_block *out);
// Alongside (mode 2): the same bookkeeping and counters without memory.
int vkn_buf_note(int kind, uint64_t handle, uint64_t size, unsigned index_size, unsigned usage, int has_init);
// Forget the handle; its block (if any) goes through vkn_retire_block. A handle that is not live here is counted and ignored.
void vkn_buf_destroy(int kind, uint64_t handle);
// The block of a live handle (NULL = not ours / not live / noted only). The pointer is into the table: stable storage, but a
// concurrent destroy clears it; render-thread users read it in the frame that uses the handle (the engine never destroys a buffer
// it still draws from in the same frame).
const vkn_block *vkn_buf_get(int kind, uint64_t handle);
int vkn_buf_lookup(int kind, uint64_t handle, vkn_buf_info *out);   // copy under the lock; 0 = live
VkIndexType vkn_buf_index_type(unsigned index_size);                // 2 -> UINT16, 4 -> UINT32, else UINT16
// memcpy into a live native buffer at dst_off (the staged copy of the engine, CPU side). 0 = ok; -1 = not ours; -2 = out of range
// (nothing copied).
int vkn_buf_copy_in(int kind, uint64_t handle, uint64_t dst_off, const void *src, uint64_t size);

// ---- the constant ring ----
// The engine's rule: head aligned up to 256; if head + size >= VKN_RING_BYTES then head = 0 (wrap); offset = head; head += size.
// No check that the GPU is done with what gets overwritten (the engine has none either: the ring is big enough for its frames).
// 0 = ok (*off = offset inside the ring); -1 = no ring (no device / no memory) or size >= VKN_RING_BYTES.
int vkn_ring_alloc(uint32_t size, uint32_t *off);
VkBuffer vkn_ring_buffer(void);           // VK_NULL_HANDLE until the ring exists (vkn_ring_alloc / vkn_ring_cpu create it)
void *vkn_ring_cpu(void);                 // CPU pointer of ring offset 0 (creates the ring); NULL = none
uint64_t vkn_ring_base(void);             // offset of ring offset 0 inside vkn_ring_buffer() (descriptor base / dynamic offsets add to it)

// ---- census ----
typedef struct vkn_buf_stats {
    uint64_t created[VKN_BUF_KINDS], destroyed[VKN_BUF_KINDS], replaced[VKN_BUF_KINDS], unknown_destroy[VKN_BUF_KINDS], fail[VKN_BUF_KINDS];
    uint64_t live[VKN_BUF_KINDS], live_bytes[VKN_BUF_KINDS];
    uint64_t bytes_static[VKN_BUF_KINDS], bytes_dynamic[VKN_BUF_KINDS];           // created so far (engine sizes)
    uint64_t with_init[VKN_BUF_KINDS], without_init[VKN_BUF_KINDS];
    uint64_t copies[3], copy_bytes[3], copy_fail;                                 // staged copies by the engine's kind 0 VB / 1 IB / 2 other
    uint64_t ring_allocs, ring_wraps, ring_bytes, ring_fail;
} vkn_buf_stats;
void vkn_buf_stats_get(vkn_buf_stats *s);
void vkn_buf_count_copy(unsigned kind, uint64_t bytes, int ok);   // the hooks count every staged copy here (kind > 2 counted as 2)

// ==== [DOS2] VK-NATIVE stage 4: the engine's staged updates (0x9218c0) without GPU hazards ====
// vkn_buf_get / vkn_buf_lookup mark the buffer "used in frame vkn_frame()" (the draw paths bind what they return). An update of `size`
// bytes at dst_off from src: when no recording of the last 3 frames can read the block (never handed out, or handed out >= 3 presented
// frames ago) the bytes are copied in place; otherwise the buffer is RENAMED - a new block (every old byte outside the update copied,
// then the update), the old block retired (vkn_retire_block): draws recorded before keep the old block, later ones get the new one,
// the order NVN's copy in the command buffer gives. *cpu / *va (non-NULL) receive the new block's CPU pointer / pseudo address on a
// rename (the caller writes them into the engine record, +0x10 / +8). 0 = in place, 1 = renamed, -1 = not ours / no block, -2 = out
// of range (nothing copied), -3 = no memory for a new block (copied in place: the old hazard, counted).
int vkn_buf_update(int kind, uint64_t handle, uint64_t dst_off, const void *src, uint64_t size, void **cpu, uint64_t *va);
// Stage 4 (copies) counters: rest_bytes = carried over by renames; whole = updates that cover the buffer's engine size (a rename
// then carries nothing), partial = the others; by the engine's usage byte (VKN_BUF_USAGE_DYNAMIC or not): renames and carried bytes.
typedef struct vkn_buf_ustats {
    uint64_t inplace, renamed, rename_fail, bytes, rest_bytes;
    uint64_t whole, partial, renamed_dyn, rest_dyn, renamed_static, rest_static;
} vkn_buf_ustats;
void vkn_buf_ustats_get(vkn_buf_ustats *s);
// ==== [DOS2] VK-NATIVE stage 4 (copies): vkn_buf_update with what it did ====
// The same as vkn_buf_update; *out (non-NULL) gets the details of this call. A rename carries over only bytes inside the buffer's
// engine size (bytes past it, the block's rounding, are never part of the engine's buffer).
typedef struct vkn_buf_uinfo { uint64_t carried; uint8_t usage, whole, renamed, pad; } vkn_buf_uinfo;
// Whether the handle is live here with a block, WITHOUT marking it used (vkn_buf_get / vkn_buf_lookup mark it: a draw binds it).
int vkn_buf_has_block(int kind, uint64_t handle);
int vkn_buf_update_ex(int kind, uint64_t handle, uint64_t dst_off, const void *src, uint64_t size, void **cpu, uint64_t *va, vkn_buf_uinfo *out);

// ==== [DOS2] VK-NATIVE stage 5 (draw cost, vkn_draw.c DM_VK_NATIVE_COST bit VB): several lookups under one lock ====
// vkn_buf_get for h[0], h[1], ... in this order under ONE lock: out[i] as vkn_buf_get(kind, h[i]) would return it (marked used the
// same way). Stops at the first handle that is 0 / not live / has no block (that out[i] = NULL, the later ones untouched and not
// marked), as the sequence of single calls the draw path made stops at its first drop (a block without a VkBuffer ends it too, after
// being returned). Returns the number of entries of out[] written with a block.
uint32_t vkn_buf_get_n(int kind, const uint64_t *h, uint32_t n, const vkn_block **out);
// ==== [stage 5 A3] content versions (the shadow cache, vkn_shc.c): what the cache keys a vertex / index buffer's bytes by ====
// A number that changes with every write of the buffer this module sees - the create (its init, or the zeros) and vkn_buf_update /
// _ex (in place or renamed), vkn_buf_copy_in - and is unique over every buffer (one serial); 0 = not live / noted only. *none (non-NULL)
// = 1 when the number does not stand for the bytes (the cache never keeps a draw that reads such a buffer): not live, or the engine's
// usage byte VKN_BUF_USAGE_DYNAMIC (written every frame through the mapped CPU pointer, which no call here sees). The engine's other CPU
// writes of buffers (main.raw, 21.11): 0xa487d0 creates a vertex buffer and memcpys another one into it inside the same call, before
// any draw can name the new handle (the create's number covers it); packets 0x2f / 0x33 (0x9412f0 / 0x9414f0, only caller 0x1406ed0)
// copy into a buffer made right before (vertex: 0x940280(..., usage 3, 0) = dynamic; index: 0x9403a0) and destroyed after use, so every
// such copy lands in a buffer with a new number. Lock-free (the recording thread reads it per shadow draw): the engine does not create a
// handle while it draws from the old one of the same index.
uint64_t vkn_buf_version(int kind, uint64_t handle, int *none);
// ==== [stage 5.5] the recording thread (vkn_rt.h; the head of the [stage 5.5] block in vkn_buf.c) ====
// The encoder's marks: vkn_buf_get's "used in frame vkn_rt_frame()" for each live handle (one lock). The rename rule of vkn_buf_update
// runs on the encoder; while a record executes (vkn_rt_decoding) the lookups above return the recording's view and mark nothing.
void vkn_buf_mark(int kind, const uint64_t *h, uint32_t n);
uint64_t vkn_ring_cum(void);              // ring bytes handed out so far (a wrap's skipped tail included): the guard's measure
// The recording ring: the engine's rule over a block of its own, for copies the recording side makes itself (vkn_ui.c), so that the
// engine ring's offsets depend only on the encoder's order and this one's only on the recording's.
#define VKN_RING2_BYTES 0x400000u
int vkn_ring2_alloc(uint32_t size, uint32_t *off);
VkBuffer vkn_ring2_buffer(void);
void *vkn_ring2_cpu(void);
uint64_t vkn_ring2_base(void);

// ==== [rt P0] DM_VK_NATIVE_PROF=3: the lookups' lock (vkn_buf_get / _lookup / _get_n / _mark / _mark_get) taken / found taken ====
extern int vkn_buf_lockprof;              // 1 = count (vkn_prof_init at level 3)
void vkn_buf_lock_stats(uint64_t *locks, uint64_t *contended);
// ==== [rt P2] DM_VK_NATIVE_RT_RES (vkn_rt.h): the encoder resolves a draw's vertex buffers at encode ====
// vkn_buf_mark's marks (the encoder view, frame vkn_rt_frame()) and, under the same lock, out[i] = the block of h[i] in the ENCODER view
// (i.blk: what a record executed later finds in the recording view, vkn_buf.h "[stage 5.5]": the commit record of every rename before
// this point executes before the draw's record, every later one after it); a handle that is 0 / not live / without memory: out[i] all zero.
void vkn_buf_mark_get(int kind, const uint64_t *h, uint32_t n, vkn_block *out);
#ifdef __cplusplus
}
#endif
#endif
