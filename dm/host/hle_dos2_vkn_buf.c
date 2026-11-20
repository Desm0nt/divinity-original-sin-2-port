// [DOS2] VK-NATIVE part "buffers" (dm_vkn_buf_register): the engine's vertex and index buffers on the native backend (dm/gfx/vkn/vkn_buf.c).
// main = main.raw of update v786432, build 4979b200; r = the renderer ([main+0x2bbdc68]); dev = [r+0xb70]. Facts below are from the
// decompilations of 0x917630 0x9179d0 0x917440 0x91db40 0x91d890 0x91ad20 0x9218c0 0x933890 0x93d9f0 0x93e5f0 0x940470 0x9404c0 and the
// disassembly of 0x91c170 / 0x91c480 (docs/dos2/nr/VK-NATIVE-MAP-1.md, -3.md).
//
// Engine records (tables r+0x180 VB / r+0x240 IB: pages +0, generations +8, records per page +0x10; live list count = bits 24..47 of
// table+0x80):
//   VB, 0x68: +0 NVNbuffer* (heap 0x30; "live" = non-zero: every binder 0x92c130 0x92c3c0 0x92f730 0x930910 0x941550 ... tests only
//             *rec != 0 and then binds +8 / +0x40, nothing else dereferences it), +8 GPU address, +0x10 CPU pointer (mapped for good),
//             +0x18 A (GpuMemAlloc record 0x20: +0 ptr, +8 offset in pool, +0x10 u32 size, +0x14 u16 align, +0x16 u8 pool kind, +0x18 pool),
//             +0x38 last-used frame, +0x40 size, +0x48 u8 usage, +0x60 handle (written by the slot allocator 0x917440, not by create).
//   IB, 0x58: +0, +8, +0x10 the same, +0x18 last-used frame, +0x20 u32 count, +0x24 u32 NVN index type (1 = u16, 2 = u32), +0x28 u32
//             index size, +0x2c u8 usage, +0x30 A, +0x50 handle.
// CreateVertexBuffer 0x917630(r, handle, size, u8 usage, init) and CreateIndexBuffer 0x9179d0(r, handle, count, indexSize, u8 usage, init)
// do NOT allocate the handle: the caller did (0x917440 / 0x9177d0, e.g. 0x92bf50), they look the record up by the handle, fill it,
// GpuMemAlloc 0x932900(&A, usage == 3 ? 5 : 4, size, 4), build an NVNbuffer over (A.pool + 0x50, A.offset, size), +8 = GetAddress,
// +0x10 = Map, memcpy init (if size && init). Return: void.
// Destroy 0x91db40 / 0x91d890(r, handle): if last-used + 2 < [dev+0x51f0]: nvnBufferFinalize(+0), 0x933890(&A), free(+0), reset of the
// record; else the whole record is copied into the pending table r+0xf40 / r+0x1000 (live list count r+0xfc0 / r+0x1080). Always
// then 0x91c170(r+0x180, &handle) / 0x91c480(r+0x240, &handle): generation + 1, record pointer out of the live list, index to the free
// list - the record itself is not touched. The command-list forms 0x940470 / 0x9404c0 tail-branch to 0x91db40 / 0x91d890 (entry).
// Drain 0x91ad20(r, force) (EndFrame): the VB / IB loops run only while the pending live counts r+0xfc0 / r+0x1080 are non-zero.
//
//   DM_VK_NATIVE=2  every hook calls the original, then only counts (no Vulkan memory: the old path owns the CPU pointer).
//   DM_VK_NATIVE=1  creates and destroys are REPLACED (no GpuMemAlloc, no NVN): the record gets a vkn block. The destroys always take
//                   the "immediate" branch for the engine record (reset + handle free) and hand the block to vkn_retire_block, which
//                   frees it 3 presented frames later (never earlier than the engine's last-use + 2 rule: the destroy follows the last
//                   use). So the pending tables r+0xf40 / r+0x1000 stay empty and 0x91ad20 stays the original: its VB / IB loops find
//                   nothing, its shader / r+0xe80 / third-kind loops are untouched. A record that is not ours (created before the
//                   hooks or by a failed install) goes to the original destroy.
//                   0x9218c0 (staged copy): vkn_buf_update at the same point of the frame (in place, or the buffer renamed when a
//                   recording may still read it - see the hook).
//   DM_NR hooks on the same four functions (hle_dos2_nr.c) are chained under these: in mode 1 they no longer see VB / IB events.
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "../gfx/vkn/vkn.h"
#include "../gfx/vkn/vkn_buf.h"
#include "hle_dos2_vkn.h"

#define VB_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"   // the red-line mark, a literal of its own (the next letter may be a hex digit)

// ---- engine offsets ----
enum {
    R_DEV = 0xb70, DEV_FRAME = 0x51f0,
    R_VB = 0x180, VB_REC = 0x68, R_IB = 0x240, IB_REC = 0x58,
    TAB_LIVE = 0x80,                                   // table + 0x80: u64, live count in bits 24..47 (0x917440 / 0x91c170)
    R_VB_PEND = 0xf40, R_IB_PEND = 0x1000,
    VB_VA = 0x08, VB_CPU = 0x10, VB_A = 0x18, VB_LAST = 0x38, VB_SIZE = 0x40, VB_USAGE = 0x48,
    IB_VA = 0x08, IB_CPU = 0x10, IB_LAST = 0x18, IB_COUNT = 0x20, IB_TYPE = 0x24, IB_ISIZE = 0x28, IB_USAGE = 0x2c, IB_A = 0x30,
    A_BYTES = 0x20,
    // 0x9218c0 descriptor (a 0x40 node of the table r+0x18a0 from +0x10, filled by packet 0x2e in 0x93e5f0): +0 handle, +8 u8 kind
    // (0 VB / 1 IB / 2 the third kind r+0x300), +0xc u32 offset in the target, +0x10 CPU pointer of the source (inside the engine's
    // staged ring [r+0x1870], 0xf00000 bytes of pool memory the CPU filled when the packet ran), +0x18 source offset in its pool,
    // +0x20 source pool, +0x28 s32 size.
    SC_HANDLE = 0x00, SC_KIND = 0x08, SC_DST = 0x0c, SC_SRC = 0x10, SC_SIZE = 0x28,
};
static inline void wr64(uint64_t p, uint64_t v) { memcpy((void *)(uintptr_t)p, &v, 8); }
static inline void wr32(uint64_t p, uint32_t v) { memcpy((void *)(uintptr_t)p, &v, 4); }
static inline void wr16(uint64_t p, uint16_t v) { memcpy((void *)(uintptr_t)p, &v, 2); }
static inline void wr8(uint64_t p, uint8_t v) { *(uint8_t *)(uintptr_t)p = v; }

// Record +0 of a native buffer: non-zero is all the engine asks of it ("live"); nothing of the engine dereferences it once the
// destroys are ours. A zeroed 0x30-byte object (the size of an NVNbuffer), so even a stray read finds zeros, never freed.
static const uint8_t g_rec0[0x30] __attribute__((aligned(16)));
// The engine's handle-free helpers (pure handle-table operations, see above): (table, &handle).
typedef uint64_t (*vb_hfree_fn)(uint64_t table, uint64_t *handle);
static vb_hfree_fn hfree_vb(void) { return (vb_hfree_fn)(void *)(dm_vkn_base() + 0x91c170); }
static vb_hfree_fn hfree_ib(void) { return (vb_hfree_fn)(void *)(dm_vkn_base() + 0x91c480); }

static vkn_fn g_o_cvb, g_o_cib, g_o_dvb, g_o_dib, g_o_copy;
static uint64_t g_bad_handle, g_not_ours;   // creates / destroys with a stale handle (the original would crash), destroys passed on

// ---- create ----
VKN_HOOK vb_h_cvb(VKN_A) {   // 0x917630 CreateVertexBuffer(r, handle, size, u8 usage, init)
    if (vkn_mode() != VKN_NATIVE) {
        const uint64_t rc = g_o_cvb(VKN_F);
        vkn_buf_note(VKN_BUF_VB, a1, a2, 0, (uint8_t)a3, a2 && a4);
        return rc;
    }
    const uint64_t rec = vkn_rec(a0, R_VB, VB_REC, a1);
    if (!rec) { __atomic_add_fetch(&g_bad_handle, 1, __ATOMIC_RELAXED); vkn_logf(VB_RED "CreateVertexBuffer: stale handle %#llx (the original would crash): nothing created", (unsigned long long)a1); return 0; }
    // The original's stores, in its order: +0x48 usage byte, +0x40 size (all 64 bits), A = the new GpuMemAlloc record. Ours: A all zero
    // (pool kind byte +0x16 = 0, size 0, ptr 0): 0x933890 returns at once for such a record ("size == 0 && ptr == 0"), and nothing
    // of ours calls it anyway.
    wr8(rec + VB_USAGE, (uint8_t)a3);
    wr64(rec + VB_SIZE, a2);
    memset((void *)(uintptr_t)(rec + VB_A), 0, A_BYTES);
    vkn_block b;
    if (vkn_buf_create(VKN_BUF_VB, a1, a2, 0, (uint8_t)a3, a2 ? (const void *)(uintptr_t)a4 : NULL, &b)) {
        // No memory: the record stays "not live" (+0 = 0), so binders skip it; an engine write through +0x10 would fault.
        wr64(rec, 0); wr64(rec + VB_CPU, 0);
        return 0;
    }
    wr64(rec, (uint64_t)(uintptr_t)g_rec0);
    wr64(rec + VB_VA, vkn_buf_va(&b));            // pseudo GPU address (vkn_buf.h): binders add offsets to it, vkn_buf_resolve undoes it
    wr64(rec + VB_CPU, (uint64_t)(uintptr_t)b.cpu);
    return 0;
}
VKN_HOOK vb_h_cib(VKN_A) {   // 0x9179d0 CreateIndexBuffer(r, handle, s32 count, s32 indexSize, u8 usage, init)
    const uint32_t count = (uint32_t)a2, isz = (uint32_t)a3, size = count * isz;   // the original: int multiply, used as u32
    if (vkn_mode() != VKN_NATIVE) {
        const uint64_t rc = g_o_cib(VKN_F);
        vkn_buf_note(VKN_BUF_IB, a1, size, isz, (uint8_t)a4, size && a5);
        return rc;
    }
    const uint64_t rec = vkn_rec(a0, R_IB, IB_REC, a1);
    if (!rec) { __atomic_add_fetch(&g_bad_handle, 1, __ATOMIC_RELAXED); vkn_logf(VB_RED "CreateIndexBuffer: stale handle %#llx (the original would crash): nothing created", (unsigned long long)a1); return 0; }
    // The original: +0x20 count, +0x28 index size, +0x24 = 1 for 2 bytes / 2 for 4 bytes (left as it was for any other size),
    // +0x2c usage byte, A at +0x30.
    wr32(rec + IB_COUNT, count);
    wr32(rec + IB_ISIZE, isz);
    if (isz == 2) wr32(rec + IB_TYPE, 1); else if (isz == 4) wr32(rec + IB_TYPE, 2);
    wr8(rec + IB_USAGE, (uint8_t)a4);
    memset((void *)(uintptr_t)(rec + IB_A), 0, A_BYTES);
    vkn_block b;
    if (vkn_buf_create(VKN_BUF_IB, a1, size, isz, (uint8_t)a4, size ? (const void *)(uintptr_t)a5 : NULL, &b)) {
        wr64(rec, 0); wr64(rec + IB_CPU, 0);
        return 0;
    }
    wr64(rec, (uint64_t)(uintptr_t)g_rec0);
    wr64(rec + IB_VA, vkn_buf_va(&b));
    wr64(rec + IB_CPU, (uint64_t)(uintptr_t)b.cpu);
    return 0;
}

// ---- destroy ----
VKN_HOOK vb_h_dvb(VKN_A) {   // 0x91db40 DestroyVertexBuffer(r, handle)
    if (vkn_mode() != VKN_NATIVE) { vkn_buf_destroy(VKN_BUF_VB, a1); return g_o_dvb(VKN_F); }
    const uint64_t rec = vkn_rec(a0, R_VB, VB_REC, a1);
    if (!rec) { __atomic_add_fetch(&g_bad_handle, 1, __ATOMIC_RELAXED); vkn_logf(VB_RED "DestroyVertexBuffer: stale handle %#llx (the original would crash): ignored", (unsigned long long)a1); return 0; }
    if (!vkn_buf_get(VKN_BUF_VB, a1) || vkn_rd64(rec) != (uint64_t)(uintptr_t)g_rec0) { __atomic_add_fetch(&g_not_ours, 1, __ATOMIC_RELAXED); vkn_buf_destroy(VKN_BUF_VB, a1); return g_o_dvb(VKN_F); }
    // The original's immediate branch, minus Finalize / GpuMemFree / free: +0 = 0, +0x10 = 0, A (+0x18) cleared, u16 +0x48 = 2,
    // +0x38 = +0x40 = +0x50 = +0x58 = 0. +8 (GPU address) is left as the original leaves it.
    wr64(rec, 0); wr64(rec + VB_CPU, 0);
    memset((void *)(uintptr_t)(rec + VB_A), 0, A_BYTES);
    wr16(rec + VB_USAGE, 2);
    wr64(rec + VB_LAST, 0); wr64(rec + VB_SIZE, 0); wr64(rec + 0x50, 0); wr64(rec + 0x58, 0);
    vkn_buf_destroy(VKN_BUF_VB, a1);   // the block: vkn_retire_block (3 presented frames)
    uint64_t h = a1;
    hfree_vb()(a0 + R_VB, &h);
    return 0;
}
VKN_HOOK vb_h_dib(VKN_A) {   // 0x91d890 DestroyIndexBuffer(r, handle)
    if (vkn_mode() != VKN_NATIVE) { vkn_buf_destroy(VKN_BUF_IB, a1); return g_o_dib(VKN_F); }
    const uint64_t rec = vkn_rec(a0, R_IB, IB_REC, a1);
    if (!rec) { __atomic_add_fetch(&g_bad_handle, 1, __ATOMIC_RELAXED); vkn_logf(VB_RED "DestroyIndexBuffer: stale handle %#llx (the original would crash): ignored", (unsigned long long)a1); return 0; }
    if (!vkn_buf_get(VKN_BUF_IB, a1) || vkn_rd64(rec) != (uint64_t)(uintptr_t)g_rec0) { __atomic_add_fetch(&g_not_ours, 1, __ATOMIC_RELAXED); vkn_buf_destroy(VKN_BUF_IB, a1); return g_o_dib(VKN_F); }
    // The original's immediate branch: +0 = 0, u32 +0x20 = 0, +0x10 = 0, +0x18 = 0, u64 at +0x24 = 1 (type 1, index size 0),
    // u16 +0x2c = 1, A (+0x30) cleared.
    wr64(rec, 0); wr32(rec + IB_COUNT, 0); wr64(rec + IB_CPU, 0); wr64(rec + IB_LAST, 0);
    wr64(rec + IB_TYPE, 1); wr16(rec + IB_USAGE, 1);
    memset((void *)(uintptr_t)(rec + IB_A), 0, A_BYTES);
    vkn_buf_destroy(VKN_BUF_IB, a1);
    uint64_t h = a1;
    hfree_ib()(a0 + R_IB, &h);
    return 0;
}

// ---- staged copy ----
// 0x9218c0 CopyStagedBufferData(r, desc): callers 0x93d9f0 (start of the window's frame: every queued update), packet 0x30 in 0x93e5f0,
// 0x921dc0 (packet 0x32 / 0x9414e0), 0x941490, and nine engine functions that lock / fill / unlock a dynamic buffer inline (0xaab5e0
// 0xc69520 0xc7d430 0xc9d5c0 0xd55120 0x10e2900 0x12b03b0 0x12dbd50 0x1406ed0: list recording -> packet 0x2e with the data in the
// packet, then packet 0x30; immediate -> the data straight into the staged ring r+0x1870, then 0x9218c0 at once when r+0x18c0 "frame
// recording" is set, else left for BeginRender). Original: two temporary NVNbuffers over (target A.pool, A.offset + desc+0xc) and
// (desc+0x20 pool, desc+0x18), CopyBufferToBuffer of desc+0x28 bytes in the current command buffer - ordered with the draws around it.
// So a lock / unlock pair between draws of the same frame (packet 0x2e then 0x30, inside one executed list) is the normal case, and
// draws recorded before it must keep the old bytes. Mode 1: vkn_buf_update - in place when no recording of the last 3 frames can read
// the block, else the buffer is renamed (new block, old one retired; the record's +0x10 CPU pointer and +8 pseudo address follow).
// Kind 2 (the third buffer table r+0x300) is dead in DOS2: no engine function creates such a record (the only NVN buffer initialisers
// are 0x917630 / 0x9179d0 / 0x9218c0's temporaries / device and context setup 0x938330 / 0x939850 / Iggy 0x1bf1xxx; none fills the
// r+0x300 table), and every writer of a pending node stores kind 0 (packet 0x2e and the nine inline lockers) or 1 (0x921c60). In mode 1
// a kind-2 (or unknown) descriptor is dropped and counted, never handed to NVN.
static uint64_t g_kind2, g_kind_bad;
VKN_HOOK vb_h_copy(VKN_A) {
    const uint64_t d = a1;
    const unsigned kind = d ? vkn_rd8(d + SC_KIND) : 3;
    const uint64_t size = d ? (uint64_t)(int64_t)(int32_t)vkn_rd32(d + SC_SIZE) : 0;
    if (vkn_mode() == VKN_NATIVE && kind <= 1) {
        const uint64_t h = vkn_rd64(d + SC_HANDLE);
        const int k = kind == 0 ? VKN_BUF_VB : VKN_BUF_IB;
        const uint64_t rec = (h & 0x3fffff00000000ull) ? vkn_rec(a0, kind == 0 ? R_VB : R_IB, kind == 0 ? VB_REC : IB_REC, h) : 0;
        if (!rec) { vkn_buf_count_copy(kind, 0, 1); return 0; }   // the original returns
        void *cpu = NULL; uint64_t va = 0;
        const int rc = vkn_buf_update(k, h, vkn_rd32(d + SC_DST), (const void *)(uintptr_t)vkn_rd64(d + SC_SRC), size, &cpu, &va);
        if (rc != -1) {
            if (rc == 1) { wr64(rec + (kind == 0 ? VB_CPU : IB_CPU), (uint64_t)(uintptr_t)cpu); wr64(rec + (kind == 0 ? VB_VA : IB_VA), va); }
            vkn_buf_count_copy(kind, size, rc >= 0);
            if (rc == -2) vkn_logf(VB_RED "staged copy into %s %#llx: offset %u + %llu bytes outside the buffer or no source: skipped", kind ? "IB" : "VB", (unsigned long long)h, vkn_rd32(d + SC_DST), (unsigned long long)size);
            if (rc == -3) { static int n; if (n++ < 16) vkn_logf(VB_RED "staged copy into %s %#llx: no memory for a new version, copied in place (draws of this frame may see it)", kind ? "IB" : "VB", (unsigned long long)h); }
            return 0;
        }
    } else if (vkn_mode() == VKN_NATIVE) {
        const uint64_t n = kind == 2 ? __atomic_add_fetch(&g_kind2, 1, __ATOMIC_RELAXED) : __atomic_add_fetch(&g_kind_bad, 1, __ATOMIC_RELAXED);
        if (n <= 16) vkn_logf(VB_RED "staged copy of buffer kind %u (handle %#llx, %llu bytes): %s - dropped", kind, d ? (unsigned long long)vkn_rd64(d + SC_HANDLE) : 0ull,
                              (unsigned long long)size, kind == 2 ? "the third buffer kind was thought dead in DOS2" : "not a kind the engine has");
        vkn_buf_count_copy(kind, size, 0);
        return 0;
    }
    vkn_buf_count_copy(kind, size, 1);
    return g_o_copy(VKN_F);   // mode 2, or a buffer that is not ours
}

// ---- census ----
static void vb_census(void) {
    vkn_buf_stats s; vkn_buf_stats_get(&s);
    const double M = 1048576.0;
    for (int k = 0; k < VKN_BUF_KINDS; k++)
        vkn_logf("buffers %s: created %llu (static %.1f MB, dynamic %.1f MB; init %llu, none %llu), destroyed %llu (unknown %llu), replaced %llu, failed %llu, live %llu = %.1f MB",
                 k ? "IB" : "VB", (unsigned long long)s.created[k], s.bytes_static[k] / M, s.bytes_dynamic[k] / M, (unsigned long long)s.with_init[k], (unsigned long long)s.without_init[k],
                 (unsigned long long)s.destroyed[k], (unsigned long long)s.unknown_destroy[k], (unsigned long long)s.replaced[k], (unsigned long long)s.fail[k],
                 (unsigned long long)s.live[k], s.live_bytes[k] / M);
    const uint64_t r = dm_vkn_base() ? vkn_renderer() : 0;
    if (r) {   // the engine's own counts: live records of the two tables and of the two pending tables (mode 1: pending stays 0)
        const unsigned lvb = (unsigned)(vkn_rd64(r + R_VB + TAB_LIVE) >> 24 & 0xffffff), lib = (unsigned)(vkn_rd64(r + R_IB + TAB_LIVE) >> 24 & 0xffffff);
        const unsigned pvb = (unsigned)(vkn_rd64(r + R_VB_PEND + TAB_LIVE) >> 24 & 0xffffff), pib = (unsigned)(vkn_rd64(r + R_IB_PEND + TAB_LIVE) >> 24 & 0xffffff);
        vkn_logf("buffers engine: live VB %u IB %u, pending destroy VB %u IB %u; frame %llu", lvb, lib, pvb, pib, (unsigned long long)vkn_rd64(vkn_rd64(r + R_DEV) + DEV_FRAME));
    }
    vkn_logf("buffers staged copies: VB %llu (%.2f MB), IB %llu (%.2f MB), other %llu (%.2f MB), failed %llu; ring allocs %llu (%.1f MB, wraps %llu, failed %llu); stale handles %llu, destroys passed on %llu",
             (unsigned long long)s.copies[0], s.copy_bytes[0] / M, (unsigned long long)s.copies[1], s.copy_bytes[1] / M, (unsigned long long)s.copies[2], s.copy_bytes[2] / M,
             (unsigned long long)s.copy_fail, (unsigned long long)s.ring_allocs, s.ring_bytes / M, (unsigned long long)s.ring_wraps, (unsigned long long)s.ring_fail,
             (unsigned long long)__atomic_load_n(&g_bad_handle, __ATOMIC_RELAXED), (unsigned long long)__atomic_load_n(&g_not_ours, __ATOMIC_RELAXED));
    if (vkn_mode() == VKN_NATIVE) {
        vkn_buf_ustats u; vkn_buf_ustats_get(&u);
        vkn_logf("buffers staged updates: %llu in place, %llu renamed (old block busy; %.2f MB carried over), %llu without memory (in place), %.2f MB updated; kind 2 dropped %llu, bad kind %llu",
                 (unsigned long long)u.inplace, (unsigned long long)u.renamed, u.rest_bytes / M, (unsigned long long)u.rename_fail, u.bytes / M,
                 (unsigned long long)__atomic_load_n(&g_kind2, __ATOMIC_RELAXED), (unsigned long long)__atomic_load_n(&g_kind_bad, __ATOMIC_RELAXED));
    }
}

// [DOS2] VK-NATIVE stage 4: the pipeline warm-up part (hle_dos2_vkn_warm.c) has no slot of its own in hle_dos2_vkn.c; registered here.
void dm_vkn_warm_register(void) __attribute__((weak));
void dm_vkn_buf_register(void);
void dm_vkn_buf_register(void) {
    if (dm_vkn_warm_register) dm_vkn_warm_register();
    // First four words of each function (none PC-relative).
    static const uint32_t k_cvb[4] = {0xd10203ff /* sub sp,sp,#0x80 */, 0xf90023f7, 0xa90557f6, 0xa9064ff4};
    static const uint32_t k_cib[4] = {0xd10203ff /* sub sp,sp,#0x80 */, 0xf90023f7, 0xa90557f6, 0xa9064ff4};
    static const uint32_t k_dvb[4] = {0xf81b0ff9 /* str x25,[sp,#-0x50]! */, 0xa9015ff8, 0xa90257f6, 0xa9034ff4};
    static const uint32_t k_dib[4] = {0xf81b0ff9 /* str x25,[sp,#-0x50]! */, 0xa9015ff8, 0xa90257f6, 0xa9034ff4};
    static const uint32_t k_copy[4] = {0xd10383ff /* sub sp,sp,#0xe0 */, 0xf90053f7, 0xa90b57f6, 0xa90c4ff4};
    dm_vkn_site(0x917630, k_cvb, "CreateVertexBuffer", (void *)vb_h_cvb, &g_o_cvb, VKN_M_BOTH);
    dm_vkn_site(0x9179d0, k_cib, "CreateIndexBuffer", (void *)vb_h_cib, &g_o_cib, VKN_M_BOTH);
    dm_vkn_site(0x91db40, k_dvb, "DestroyVertexBuffer", (void *)vb_h_dvb, &g_o_dvb, VKN_M_BOTH);
    dm_vkn_site(0x91d890, k_dib, "DestroyIndexBuffer", (void *)vb_h_dib, &g_o_dib, VKN_M_BOTH);
    dm_vkn_site(0x9218c0, k_copy, "CopyStagedBufferData", (void *)vb_h_copy, &g_o_copy, VKN_M_BOTH);
    dm_vkn_census(vb_census);
}
#endif
