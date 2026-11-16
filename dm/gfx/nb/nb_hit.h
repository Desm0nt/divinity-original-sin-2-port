// [DOS2] NB-12: the descriptor memo HIT path of descriptors() (xbg_game.cpp), two cuts (docs/dos2/NATIVE_BACKEND_PLAN.md, "NB-12
// findings"). Pure C (C and C++): the decisions the backend makes (xbg_nb_hit.inc namespaces nbubo / hitfast) and the host test
// dm/gfx/vulkan/engine/tests/nb_hit_test.c.
//
// (1) DM_NB_UBO: uniform blocks from the engine's constant ring. The renderer's flush (main 0x926f90 global blocks, 0x925740 shader
//     blocks of stages 0 / 1 / 5, both inside the draw entry 0x928e80 / 0x928fb0 whose binds the front end packs into OP_X_NB_DRAW)
//     rebuilds a block only when it is dirty (+0x34) or stamped for an older frame (+0 != [dev+0x51f0]): it then takes a NEW range of
//     the device's constant ring ([dev+0x25a60] head, bump by size aligned to 256, wrap to 0 past 0x137ffff = 19.5 MB), stores GPU /
//     CPU pointers to +0x10 / +0x18, memcpys the shadow (+0x20) into it once, clears dirty and stamps the frame; only then
//     BindUniformBuffer(stage, slot +0x35, address +0x10, size +0x30). No other store into the range follows: a bound ring range
//     keeps its bytes until the ring head wraps around to it (~19.5 MB of later constants, tens of frames). So the backend may read
//     it in place (zero-copy) for as long as the frame is in flight, without the per-submission snapshot (map + copy + front cache).
//     A range is "from the ring" when an OP_X_NB_DRAW of this submission bound it (address, size >= the copy): nb_ring_note at the
//     record, nb_ring_find in uniform_snapshot. Direct-mapped, keyed by address, valid for the submission it was noted in (epoch =
//     snapshot_epoch): a stale slot never matches; a collision only forgets a ring range (the old path then snapshots it).
// (2) DM_GFX_HIT_FAST: images of a memo hit already checked in this submission. For every image of the entry the hit path runs
//     get_image(tex, upload) (with the read preflight: cpu_access over all pending writebacks + ~450 pending reports, a tex_t copy,
//     get_image_slow's lookup) and prepare_read (hooks, open-pass search, barrier decision). nb_hit_skip says when both are no-ops:
//     the same texture record / Image as when the full path last ran for it in this submission, the pending lists not modified
//     since (hit_pend_gen: every writer of pending_writebacks / pending_reports bumps it), the image front still names that Image,
//     the per-submission content check done (checked_submission) and the image has contents (uploaded / gpu_written), layout GENERAL,
//     no write after the last barrier the read would need, not an attachment of the open pass, no read hook armed. The only side
//     effect left is im.slot_use = slot_serial (get_image).
#ifndef DM_NB_HIT_H
#define DM_NB_HIT_H
#include <stdint.h>
#include <string.h>

// ---- (1) ring ranges ------------------------------------------------------------------------------------------------------
#define NB_RING_SLOTS 2048u
typedef struct { uint64_t addr, size, epoch, checked; } nb_ring_ent;   // checked: slot_serial of the last =2 race entry for it
typedef struct { nb_ring_ent e[NB_RING_SLOTS]; } nb_ring_tab;
static inline unsigned nb_ring_slot(uint64_t a) { uint64_t x = (a >> 4) * 0x9E3779B97F4A7C15ull; return (unsigned)(x >> 53) & (NB_RING_SLOTS - 1); }
static inline void nb_ring_note(nb_ring_tab *t, uint64_t addr, uint64_t size, uint64_t epoch) {
    if (!addr || !size) return;
    nb_ring_ent *e = &t->e[nb_ring_slot(addr)];
    if (e->addr == addr && e->epoch == epoch) { if (size > e->size) e->size = size; return; }
    e->addr = addr; e->size = size; e->epoch = epoch; e->checked = 0;
}
// the entry when [addr, addr + copy) was bound by a record of this epoch, else 0
static inline nb_ring_ent *nb_ring_find(nb_ring_tab *t, uint64_t addr, uint64_t copy, uint64_t epoch) {
    nb_ring_ent *e = &t->e[nb_ring_slot(addr)];
    return addr && e->addr == addr && e->epoch == epoch && copy <= e->size ? e : 0;
}

// Why a uniform_snapshot of a ring range was not served in place ("[nb] ubo" line).
enum { NBU_DIRECT = 0, NBU_NOTRING, NBU_UI, NBU_NOZC, NBU_PENDING, NBU_CHUNK, NBU_ALIGN, NBU_STICKY, NBU_N };
// The in-place view decision for a ring range, given the zero-copy chunk that holds it (lo, n = 0 when none): offset or a reason.
static inline int nb_ubo_view(uint64_t addr, uint64_t bytes, uint64_t chunk_lo, uint64_t chunk_n, uint64_t align,
                              const uint8_t *sticky, uint64_t *off) {
    if (!chunk_n || addr < chunk_lo || addr - chunk_lo + bytes > chunk_n) return NBU_CHUNK;
    const uint64_t o = addr - chunk_lo;
    if (align > 1 && o % align) return NBU_ALIGN;
    for (uint64_t pg = o >> 12; pg <= (o + bytes - 1) >> 12; pg++) if (sticky && sticky[pg]) return NBU_STICKY;
    *off = o;
    return NBU_DIRECT;
}
static inline uint64_t nb_hash64(const void *p, size_t n) {   // FNV-1a over 8-byte words (equality only), as zcubo::hash
    const uint8_t *b = (const uint8_t *)p; uint64_t h = 1469598103934665603ull, w; size_t i = 0;
    for (; i + 8 <= n; i += 8) { memcpy(&w, b + i, 8); h = (h ^ w) * 1099511628211ull; }
    for (; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

// ---- (2) hit-path images --------------------------------------------------------------------------------------------------
#define NB_HIT_SLOTS 1024u
typedef struct { const void *tex, *im; uint64_t serial, mem, size, sub, pend; } nb_hit_ent;   // the last full get_image + prepare_read
typedef struct { nb_hit_ent e[NB_HIT_SLOTS]; } nb_hit_tab;
static inline unsigned nb_hit_slot(const void *tex) { uint64_t x = (uint64_t)(uintptr_t)tex * 0x9E3779B97F4A7C15ull; return (unsigned)(x >> 54) & (NB_HIT_SLOTS - 1); }
// What the backend reads for one image of the entry (filled by the caller; plain values, so the host test can model it).
typedef struct {
    const void *tex, *im;                       // MemoImage first / second
    uint64_t serial, mem, size;                 // tex_t serial, mem, L.size now
    uint64_t sub, pend;                         // stats.submissions, hit_pend_gen now
    int storage;                                // MemoImage storage (bound as a storage image: never skipped)
    int hooks;                                  // any read hook armed (pgraph, nbchk, pending clears, store history, draw_reads,
                                                // pass profile, provenance, capture)
    int front_ok;                               // img_front names this Image for the serial in the current images_epoch
    int checked, has_contents;                  // im.checked_submission == stats.submissions; im.uploaded || im.gpu_written
    int general;                                // im.layout == GENERAL
    uint64_t last_write, storage_write, seen;   // im fields; seen = read_frag_only ? frag_seq : barrier_seq
    int sampled_ever;                           // im.sampled_ever (prepare_read storage = -1: as_storage = !sampled_ever)
    int in_open;                                // an attachment of the open pass
} nb_hit_in;
enum { NBH_SKIP = 0, NBH_STORAGE, NBH_HOOK, NBH_NEW, NBH_SUB, NBH_PEND, NBH_FRONT, NBH_CHECK, NBH_LAYOUT, NBH_WRITTEN, NBH_OPEN, NBH_N };
static inline int nb_hit_skip(const nb_hit_tab *t, const nb_hit_in *x) {
    if (x->storage) return NBH_STORAGE;
    if (x->hooks) return NBH_HOOK;
    const nb_hit_ent *e = &t->e[nb_hit_slot(x->tex)];
    if (e->tex != x->tex || e->im != x->im || e->serial != x->serial || e->mem != x->mem || e->size != x->size) return NBH_NEW;
    if (e->sub != x->sub) return NBH_SUB;
    if (e->pend != x->pend) return NBH_PEND;
    if (!x->front_ok) return NBH_FRONT;
    if (!x->checked || !x->has_contents) return NBH_CHECK;
    if (!x->general) return NBH_LAYOUT;
    const int as_storage = !x->sampled_ever;
    if (x->last_write > x->seen || (!as_storage && x->storage_write > x->seen)) return NBH_WRITTEN;
    if (x->in_open) return NBH_OPEN;
    return NBH_SKIP;
}
// After the full path ran for a non-storage image (pend = hit_pend_gen after it: a resolve inside it is already behind).
static inline void nb_hit_note(nb_hit_tab *t, const nb_hit_in *x) {
    nb_hit_ent *e = &t->e[nb_hit_slot(x->tex)];
    e->tex = x->tex; e->im = x->im; e->serial = x->serial; e->mem = x->mem; e->size = x->size; e->sub = x->sub; e->pend = x->pend;
}
#endif
