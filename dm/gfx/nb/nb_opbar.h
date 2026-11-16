// [DOS2] NB-08 DM_NB_OPBARRIER: copies and clears outside a render pass with barriers on their own images instead of two full
// barriers (docs/dos2/NATIVE_BACKEND_PLAN.md, "NB-08 findings"). Pure C (C and C++): the read-barrier bookkeeping the backend
// (xbg_game.cpp namespace opbar, OpCommands, prepare_read) uses and the host test dm/gfx/vulkan/engine/tests/nb_opbar_test.c.
//
// The host orders image hazards with sequence numbers: every write of an image takes im.last_write = ++write_seq; a full barrier
// sets barrier_seq = write_seq (everything written so far is visible to every stage), a fragment barrier (DM_GFX_TILE_BARRIER) sets
// frag_seq (visible to the fragment stages). A read whose image was written after the point it needs (last_write > seen) gets a
// barrier first (prepare_read). Until NB-08 every copy / blit / resolve / ClearTexture (Commands, non-draw) recorded a full barrier
// before and after itself, ALL_COMMANDS -> ALL_COMMANDS: all earlier work drains before it and all later work waits for it (on a
// tiler: the next pass's vertex / binning work cannot overlap the earlier fragment work, FN-32).
//
// With DM_NB_OPBARRIER=1 the operation keeps only the layout transitions of its own images, narrowed:
//   before: ALL_COMMANDS -> TRANSFER (the op waits for every earlier access of its images; nothing later waits on this barrier);
//   after:  TRANSFER -> frag stages | TRANSFER with DM_GFX_TILE_BARRIER (what the host's render-pass dependencies already use),
//           TRANSFER -> ALL_COMMANDS without it.
// No global point moves, so the bookkeeping gets a per-image one: the op stamps each of its images (source too: its layout
// transition is a write) with a new write sequence s and records that the image's writes up to s are visible to the fragment
// stages (vis_frag), and to every stage when the after-barrier went to ALL_COMMANDS (vis_full). A later read then needs a barrier
// only if last_write > max(global point, the image's own point): a fragment read of an op image needs none, a vertex / compute
// read of one gets the full barrier it needs, and an image written BEFORE the op that the op does not touch is no longer covered
// by the op's barrier: its first reader records the barrier itself (the "lazy" barrier the host already has for render targets).
// Those are the cost of NB-08 and are counted: exactly by NB_OB_EXTRA / NB_OB_SAVED in mode 2 (narrow read barriers = old + EXTRA -
// SAVED, host test), as a lower bound by nb_ob_caused in mode 1 (only writes the LAST narrowed op would have covered).
//
// Mode 2 (check): the old full barriers are executed; a shadow pair (sh_b, sh_f) follows what barrier_seq / frag_seq would be in
// mode 1 (it moves on every full / fragment barrier except those the ops record, and at a read the narrow path would barrier),
// and op stamps go to im.opw instead of last_write. nb_ob_shadow_read classifies each read: the same decision, a barrier only the
// narrow path needs (EXTRA), or one only the old path needs (SAVED: the shadow does not move on it).
#ifndef DM_NB_OPBAR_H
#define DM_NB_OPBAR_H
#include <stdint.h>

// The point a read has seen: the global barrier point (barrier_seq, or frag_seq for a fragment-only read) or the image's own.
static inline uint64_t nb_ob_seen(uint64_t global_seq, uint64_t image_vis) { return global_seq > image_vis ? global_seq : image_vis; }

// prepare_read's test: a write (attachment / copy / clear: last_write; storage image: storage_write, which a storage read orders
// itself) after the point the read has seen.
static inline int nb_ob_written(uint64_t last_write, uint64_t storage_write, int as_storage, uint64_t seen) {
    return last_write > seen || (!as_storage && storage_write > seen);
}

// After a narrowed op on an image stamped with write sequence seq: its writes up to seq are visible to the fragment stages, and
// to every stage when the after-barrier waited in ALL_COMMANDS (tile == 0). Points only move forward.
static inline void nb_ob_stamp(uint64_t *vis_full, uint64_t *vis_frag, uint64_t seq, int tile) {
    if (seq > *vis_frag) *vis_frag = seq;
    if (!tile && seq > *vis_full) *vis_full = seq;
}

// Mode 1: a read barrier the old op barriers would have spared: the read needs a barrier (written) and every write it waits for
// is not newer than the last narrowed op (skip_seq = write_seq after its stamps): the old path recorded a full barrier there.
static inline int nb_ob_caused(int written, uint64_t last_write, uint64_t storage_write, int as_storage, uint64_t skip_seq) {
    return written && last_write <= skip_seq && (as_storage || storage_write <= skip_seq);
}

// Mode 2: one read compared. narrow = the shadow says "written", old = the executed path says so; other = the read breaks / barriers
// for a reason both paths share (layout not GENERAL, attachment of the open pass): then both record the same barrier.
enum { NB_OB_SAME = 0, NB_OB_EXTRA = 1, NB_OB_SAVED = 2 };
static inline int nb_ob_shadow_read(int narrow, int old, int other) {
    if (other || narrow == old) return NB_OB_SAME;
    return narrow ? NB_OB_EXTRA : NB_OB_SAVED;
}
#endif
