// [DOS2] NR-06: buffers of the native renderer (DM_NR_BUF, default 0) - the decisions, host-testable. Pure C: no Vulkan, no game
// addresses, no locks; the engine side (dm/gfx/vulkan/engine/xbg_nr_buf.inc) owns the Vulkan objects and calls this from the
// translation thread only. Test: dm/gfx/vulkan/engine/tests/nr_buf_test.c. Design and numbers: docs/dos2/nr/NR-06.md.
//
// 1. The constant ring mirror (nr_ring). The engine has ONE append-only constant ring (dev = [r+0xb70]; head [dev+0x25a60], base
//    [dev+0x25aa0], 0x1380000 bytes; allocations start on 256-byte boundaries; a bound range is never rewritten until the head comes round
//    again, ~30 frames later in the campaign, ~10 in menus - NATIVE_BACKEND_PLAN.md NB-12). Without zero-copy the engine side keeps a
//    mirror of it (one VkBuffer, same offsets) and copies a bound range's 256-byte granules into it the first time an epoch binds them.
//    Per granule:
//      epoch[g]  snapshot epoch in which the granule was last made valid (copied, or compared equal); == current epoch: nothing to do
//      used[g]   newest slot serial whose draws may read the mirror granule (0 = never); a write is safe only once used + 2 <= serial
//                (that slot retired: the engine has two slots and retires one before it records into it again - recording_slot())
//      hold[g]   the granule goes to the old path while serial < hold (a CPU write in the stream, or the ring changed under a granule a
//                recorded / flying submission still reads: the mirror cannot hold two versions at one offset)
//    nr_ring_check classifies the granules of [addr, addr + n): all valid -> NRR_VALID; any held -> NRR_HOLD (old path); otherwise up to
//    NR_RING_RUNS runs to COPY (no reader in flight) or to COMPARE (a reader may be in flight: the bytes must be equal, else it is a race)
//    and NRR_WORK. The caller does the copies / compares and reports each run with nr_ring_done (valid now) or nr_ring_race (held).
// 2. The static buffer table (nr_stab). Engine vertex / index buffers by guest range [lo, hi), from the creation / destruction hooks
//    (dm/host/hle_dos2_nr.c). Kills, drops and the overlap rule take effect at once (the entry is marked dead, lookups skip it: an add
//    over a live range - memory reused - kills that range in the table right away); the adds themselves are queued and merged in by
//    nr_stab_apply (O(n + k log k); dead entries leave there), which the caller may defer and batch: until then a new buffer is simply
//    not found (old path). Lookups are a direct-mapped front + a binary search (nr_stab_find). Ranges never overlap; the result is that
//    of applying the events one by one.
#ifndef DM_NR_BUF_H
#define DM_NR_BUF_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

// ---- the constant ring mirror ----
enum { NR_RING_SIZE = 0x1380000, NR_RING_GSHIFT = 8, NR_RING_GRAN = 1 << NR_RING_GSHIFT, NR_RING_RUNS = 8 };
enum { NRR_VALID, NRR_WORK, NRR_NOT, NRR_SPAN, NRR_HOLD, NRR_RUNS_OVER, NRR_N };   // results of nr_ring_check
enum { NRR_COPY = 1, NRR_COMPARE = 2 };
const char *nr_ring_result_name(int r);
typedef struct nr_ring_run { uint32_t g0, g1; uint8_t kind; } nr_ring_run;   // granules [g0, g1)
typedef struct nr_ring_work { int n; nr_ring_run run[NR_RING_RUNS]; } nr_ring_work;
typedef struct nr_ring {
    uint64_t base, size; uint32_t ngran;
    uint32_t *epoch, *used, *hold;
    uint64_t holds_cpu, holds_race;   // granules held by nr_ring_cpu_write / nr_ring_race (counters)
} nr_ring;
int nr_ring_init(nr_ring *r, uint64_t base, uint64_t size);   // 0 = ok (arrays zeroed), -1 = no memory / bad size
void nr_ring_free(nr_ring *r);
static inline int nr_ring_has(const nr_ring *r, uint64_t addr) { return r->ngran && addr >= r->base && addr - r->base < r->size; }
static inline uint64_t nr_ring_goff(uint32_t g) { return (uint64_t)g << NR_RING_GSHIFT; }   // byte offset of granule g
// [addr, addr + n) against the granules. epoch: the engine's snapshot epoch, never 0 (changes at least at every slot submission and
// every guest submission); serial: the slot serial being recorded (constant within an epoch). Never writes the ring.
int nr_ring_check(const nr_ring *r, uint64_t addr, uint64_t n, uint32_t epoch, uint32_t serial, nr_ring_work *w);
void nr_ring_done(nr_ring *r, uint32_t g0, uint32_t g1, uint32_t epoch, uint32_t serial);   // the run is valid in this epoch, read by serial
void nr_ring_race(nr_ring *r, uint32_t g0, uint32_t g1, uint32_t serial);                   // COMPARE found other bytes: hold until the readers retired
// An in-stream CPU write (or GPU writeback) over [addr, addr + n): those granules are not valid any more; a granule a recorded or flying
// submission reads is held until those readers retired. Returns the granules held.
uint32_t nr_ring_cpu_write(nr_ring *r, uint64_t addr, uint64_t n, uint32_t serial);

// ---- the static buffer table ----
enum { NRS_KIND_VB, NRS_KIND_IB, NRS_KINDS, NRS_HANDLES = 65536 };
enum { NRS_NEW, NRS_READY, NRS_DEMOTED, NRS_NOBUDGET, NRS_PHASES };   // engine phase of an entry's copy
enum { NRS_ELIGIBLE, NRS_DYNAMIC, NRS_NOINIT, NRS_CLASSES };          // usage 3 = dynamic; static without initial data = no-init
const char *nr_sbuf_class_name(int c);
typedef struct nr_sbuf {
    uint64_t lo, hi, handle, hash;   // guest range [lo, hi); engine handle; FNV of the bytes at creation (DM_NR_BUF=2, 0 = none)
    uint64_t seq;                    // event order (internal)
    int32_t slot;                    // engine copy slot, -1 = none (owned by the engine side)
    uint32_t ready;                  // snapshot epoch of the last first-use check (engine side)
    uint64_t stamp;                  // engine side (dirty-tracker stamp of the copy + 1, 0 = untracked; NOBUDGET: serial of the next try)
    uint8_t kind, cls, phase, dead;   // phase: NRS_NEW...NRS_NOBUDGET (engine side; not "state": a context macro of xbg_game.cpp)
} nr_sbuf;
typedef struct nr_stab {
    nr_sbuf *e; uint32_t n, cap, ndead; // entries sorted by lo, never overlapping (dead ones until the next apply)
    nr_sbuf *b; uint32_t nb, capb;      // queued adds, in event order
    uint32_t *ix; uint32_t capix;       // apply's scratch
    uint64_t *lo_of[NRS_KINDS];         // handle index -> lo of its entry (0 = none), for kills
    uint64_t seq; uint32_t ver;         // ver: bumped when entries move (apply); the lookup front is valid for one ver
    struct { uint64_t addr; uint32_t idx, ver; } front[1024];
    struct { uint64_t adds, kills, kill_miss, drops, overlapped, applies, no_index; } st;
} nr_stab;
int nr_stab_init(nr_stab *t);           // 0 = ok
void nr_stab_free(nr_stab *t);
int nr_stab_add(nr_stab *t, int kind, uint64_t handle, uint64_t lo, uint64_t size, int cls, uint64_t hash);   // 0 ok, -1 no memory / empty
int nr_stab_kill(nr_stab *t, int kind, uint64_t handle);       // 1 = found (in the table or the queue)
uint32_t nr_stab_drop(nr_stab *t, uint64_t lo, uint64_t n);    // a pool went: every range overlapping it dies now; returns how many
// Merges the queued adds. gone(ctx, e) is called once for every entry that leaves the table (killed, dropped, overlapped - queued ones
// too, with slot -1) while e still holds its slot. Returns the live entries. Entry pointers stay valid until the next add / apply.
uint32_t nr_stab_apply(nr_stab *t, void (*gone)(void *ctx, const nr_sbuf *e), void *ctx);
nr_sbuf *nr_stab_find(nr_stab *t, uint64_t addr, uint64_t n);   // the live entry holding [addr, addr + n), else NULL
uint32_t nr_stab_each(nr_stab *t, uint64_t lo, uint64_t n, void (*fn)(void *ctx, nr_sbuf *e), void *ctx);   // every live table entry overlapping [lo, lo + n)
static inline uint64_t nr_fnv64(const void *p, size_t n) {   // FNV-1a over 8-byte words (equality only), as nb_hash64
    const uint8_t *b = (const uint8_t *)p; uint64_t h = 1469598103934665603ull, w; size_t i = 0;
    for (; i + 8 <= n; i += 8) { memcpy(&w, b + i, 8); h = (h ^ w) * 1099511628211ull; }
    for (; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

#ifdef __cplusplus
}
#endif
#endif
