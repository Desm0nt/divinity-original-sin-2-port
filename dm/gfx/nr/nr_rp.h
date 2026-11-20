// [DOS2] NR-10 DM_NR_RP (docs/dos2/nr/NR-10.md): render-pass visibility - barriers by what a render pass already guarantees - and the
// "storage clobbered" census of attachments. Pure C (C and C++): no Vulkan, no game addresses. The backend (xbg_game.cpp namespace nrrp,
// prepare_read, end_open_pass, xbg_nb.inc nbrp::prebar) uses it; dm/gfx/vulkan/engine/tests/nr_rp_test.c tests it on the PC.
//
// What the host did (NB-06 / NB-08 included): every image hazard is ordered by sequence numbers. A write takes im.last_write =
// ++write_seq, a full barrier (ALL_COMMANDS -> ALL_COMMANDS, global memory) sets barrier_seq = write_seq, a fragment barrier
// (DM_GFX_TILE_BARRIER) frag_seq. A read of an image written after the point it needs gets a barrier first (prepare_read) and, with
// DM_NB_RP, every IR pass that samples something written in the submission gets a FULL barrier before it begins (NB_NP_PREBAR) -
// decided by "anything written since the last barrier", which the pass's own attachments always are. Phone, ship deck, 17.11 (rd2):
// 49 host passes a frame, 26 read barriers + 38 pre-pass barriers = 65 full barriers a frame; under SIM mali (ab7rp3) 31 fragment
// barriers + 43 FULL pre-pass barriers: the pre-pass barrier undid DM_GFX_TILE_BARRIER for most passes.
//
// What a render pass already guarantees. Every host pass is created with the dependency (create_pass, dep[1])
//     subpass 0 -> EXTERNAL: ALL_GRAPHICS -> ALL_COMMANDS (tile: fragment stages | TRANSFER), attachment writes -> MEMORY_READ | WRITE
// which Vulkan defines as a pipeline barrier of those masks right after vkCmdEndRenderPass. So once a pass has ENDED, what it wrote
// into its attachments is visible to every later stage (tile policy: to the fragment stages and transfers) with no barrier of ours;
// all images stay in GENERAL, so no layout change is owed either. The pass's own load is ordered after every earlier write by dep[0]
// (ALL_COMMANDS / MEMORY_WRITE -> attachment access), so an attachment's content after the pass is the pass's store.
// One hole: the dependency speaks about the attached subresource only. An image whose OTHER mip / layer was written before the pass by
// something that is not yet behind a barrier (a copy, a storage write) would be declared visible as a whole. So a pass stamps its
// images only when every one of them was clean when the pass began (its previous write already visible): nr_rp_clean.
//
// DM_NR_RP=1: at the end of such a pass each attachment gets its own visibility point (nrvis_frag always, nrvis_full when the
// dependency waits in ALL_COMMANDS = no tile policy): "writes up to last_write are visible". prepare_read needs a barrier only if
// last_write is beyond max(global point, the image's op point (NB-08), the image's render-pass point); a read of the still open
// previous pass's attachment ends that pass and then needs nothing. The pre-pass barrier is decided by the header's read images one by
// one (nr_rp_prebar) and is a fragment barrier on a tile GPU when the pass's first draw reads in the fragment stage only.
// Storage writes (im.storage_write) are never covered by a render-pass point.
// DM_NR_RP=2: the old barriers are executed; the render-pass points are kept and a shadow pair (sh_b, sh_f) follows what barrier_seq /
// frag_seq would be at 1, so every read and every pre-pass barrier is classified (kept / dropped / full -> fragment).
// DM_NR_RP=3: 1 + poison of what DM_NR_RP_FRESH begins with load DONT_CARE.
//
// "Fresh" attachments (census at 1 / 2 / 3, applied only with DM_NR_RP_FRESH=1). The engine hands its render targets out of a shared
// storage pool (CreateTexture flag 0x2000, NR-01): different textures (other size / format) live over the same guest memory, each a
// separate VkImage here. When another texture over the same storage has been rendered since this one's last write, the Switch itself
// holds that other texture's bytes there: this one's old content is gone, so its load can be DONT_CARE without a coverage proof
// (nr_rp_fresh). Strictly that is proven only for the bytes the other texture covers (a smaller one clobbers a corner), hence the
// separate switch and the poison; see NR-10.md for the argument and the risk.
#ifndef DM_NR_RP_H
#define DM_NR_RP_H
#include <stdint.h>

static inline uint64_t nr_rp_max(uint64_t a, uint64_t b) { return a > b ? a : b; }

// An attachment at the begin of its pass: clean = the write before this pass (prior_write) is behind the point `seen`
// (max of the global barrier point and the image's op point) or its render-pass point, and no storage write is pending (a
// render-pass point never covers one). The pass stamps only if all its images are clean.
static inline int nr_rp_clean(uint64_t prior_write, uint64_t storage_write, uint64_t seen, uint64_t rpvis) {
    return prior_write <= nr_rp_max(seen, rpvis) && storage_write <= seen;
}

// vkCmdEndRenderPass of a pass that began clean (clean_full: against the all-stage points, clean_frag: against the fragment points):
// the attachment's writes up to last_write are visible to the fragment stages, and to every stage when the pass dependency waits
// in ALL_COMMANDS (tile == 0). Points only move forward.
static inline void nr_rp_pass_end(uint64_t *vis_full, uint64_t *vis_frag, uint64_t last_write, int clean_full, int clean_frag, int tile) {
    if (clean_frag && last_write > *vis_frag) *vis_frag = last_write;
    if (!tile && clean_full && last_write > *vis_full) *vis_full = last_write;
}

// prepare_read's test with the render-pass point. seen: max(global point, op point) as before NR-10 (it also covers storage writes:
// an op barrier and a global barrier are memory barriers of everything); rpvis: the image's render-pass point for this kind of read
// (nrvis_frag for a fragment-only read under the tile policy, nrvis_full otherwise) - attachment / copy / clear writes only.
static inline int nr_rp_written(uint64_t last_write, uint64_t storage_write, int as_storage, uint64_t seen, uint64_t rpvis) {
    return last_write > nr_rp_max(seen, rpvis) || (!as_storage && storage_write > seen);
}

// The barrier before an IR pass (NB_NP_PREBAR). any_full / any_frag: a read image of the header is written beyond the all-stage /
// fragment point (nr_rp_written per image); unknown: the header cannot name every read (read_over) and something was written since
// the last full barrier, or a storage buffer write is pending (storage_unbarriered) - then the old rule stands. tile: DM_GFX_TILE_BARRIER;
// frag_first: the first draw of the pass binds images in the fragment stage only (a later pre-fragment read of a written image then
// gets its own full barrier in prepare_read, as before DM_NB_RP).
enum { NR_PB_NONE = 0, NR_PB_FRAG = 1, NR_PB_FULL = 2 };
static inline int nr_rp_prebar(int any_full, int any_frag, int unknown, int tile, int frag_first) {
    if (unknown) return NR_PB_FULL;
    if (tile && frag_first) return any_frag ? NR_PB_FRAG : NR_PB_NONE;
    return any_full ? NR_PB_FULL : NR_PB_NONE;
}

// Mode 2: one executed barrier against the narrow decision.
enum { NR_CMP_SAME = 0, NR_CMP_DROP = 1, NR_CMP_FRAG = 2, NR_CMP_MORE = 3 };   // MORE: the narrow path wants more than was executed (never expected)
static inline int nr_rp_compare(int executed, int narrow) {   // both NR_PB_*
    if (executed == narrow) return NR_CMP_SAME;
    if (narrow > executed) return NR_CMP_MORE;
    return narrow == NR_PB_NONE ? NR_CMP_DROP : NR_CMP_FRAG;
}

// "Fresh": an attachment whose guest storage another image was rendered into (or cleared / copied into) since its own last GPU
// write. plan_clear: the pass begins it with CLEAR anyway; sampled_by_first: the pass's first draw samples it (feedback).
static inline int nr_rp_fresh(int clobbered, int plan_clear, int sampled_by_first) { return clobbered && !plan_clear && !sampled_by_first; }

// Bytes of one load or store of an attachment (census): 4 B a pixel unless the caller knows better.
static inline uint64_t nr_rp_bytes(uint32_t w, uint32_t h, uint32_t bytes_per_pixel) { return (uint64_t)w * h * (bytes_per_pixel ? bytes_per_pixel : 4u); }

#endif
