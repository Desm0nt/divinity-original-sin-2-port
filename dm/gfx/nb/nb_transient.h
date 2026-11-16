// [dos2-nb] NB-19: transient attachments (docs/dos2/NATIVE_BACKEND_PLAN.md "NB-19 findings"). Pure C, inline, no Vulkan: the backend
// (xbg_nb_transient.inc, DM_NB_TRANSIENT) and the host test tests/nb_transient_test.c call the same code.
//
// An attachment image may live in tile memory only (VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT + VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT:
// no DRAM backing on Mali / PowerVR / Xclipse) when its contents never leave a render pass:
//   - every host pass that uses it was begun with the planner's decision (DM_NB_RP 1 / 3: an OP_X_NB_PASS header mapped onto the host
//     pass), and that decision begins it without its old texels (load CLEAR - a clear folded into the load op - or DONT_CARE) and ends
//     it with store DONT_CARE: written and consumed inside one IR pass span;
//   - it is never sampled, bound as a storage image, copied (source or destination), blitted, resolved, read back, presented or cleared
//     by a transfer (vkCmdClear*Image): a transient image has attachment usage only, so any of these needs the image re-created;
//   - its shape allows it: 2D, one level, one layer, the driver accepts the usage, no other image over the same memory;
//   - all of the above held for min_frames consecutive frames in which it was an attachment (default 8, like STORE_HIST).
// The backend gathers per-frame flags (nb_tr_pass at each host pass begin, nb_tr_touch at each other use) and judges each image at the
// end of the frame (nb_tr_judge): one reason per image and frame (the first that applies, in enum order) and an action.
// A use that needs other usage while the image is transient (nb_tr_touch returns 1) re-creates it as before at once (its texels are
// gone: one frame of garbage where a reader appeared after >= min_frames frames without one) and bans it; a pass that loads or stores
// a transient image is valid Vulkan (lazy memory gets committed), so that pattern break is judged at the frame end: revert + ban.
#ifndef DM_GFX_NB_TRANSIENT_H
#define DM_GFX_NB_TRANSIENT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NB_TR_OFF = 0, NB_TR_ON = 1, NB_TR_COUNT = 2 };              // DM_NB_TRANSIENT: 1 re-create candidates, 2 count only
// Frame flags (nb_tr_img.frame_flags).
enum { NB_TRF_ATT = 1,            // an attachment of a host pass this frame
       NB_TRF_UNPLANNED = 2,      // a host pass with it was begun by the host heuristics (no applied NB header: not proven)
       NB_TRF_LOAD = 4,           // a planned pass loads its old texels (content crosses passes)
       NB_TRF_STORE = 8,          // a planned pass stores it
       NB_TRF_READ = 16,          // sampled / storage view / prepare_read
       NB_TRF_XFER = 32 };        // transfer use: copy / blit / resolve / readback / present source / vkCmdClear*Image / upload
// Shape refusals (nb_tr_shape).
enum { NB_TRS_OK = 0, NB_TRS_TYPE, NB_TRS_LEVELS, NB_TRS_LAYERS, NB_TRS_ALIAS, NB_TRS_FORMAT, NB_TRS_KINDS };
// Reasons (one per judged image and frame, the first that applies).
enum { NB_TRR_CAND = 0,           // candidate (or already transient and clean)
       NB_TRR_BANNED,             // was transient and reverted, or broke the pattern once: never again
       NB_TRR_SHAPE,              // nb_tr_shape refused it
       NB_TRR_READ,               // sampled / storage-bound at some point in its life (sticky)
       NB_TRR_XFER,               // transfer use at some point in its life (sticky)
       NB_TRR_UNPLANNED,          // a pass with it this frame was not begun from the plan
       NB_TRR_LOAD,               // a planned pass loads it
       NB_TRR_STORE,              // a planned pass stores it
       NB_TRR_STREAK,             // conditions met, fewer than min_frames frames in a row yet
       NB_TRR_KINDS };
enum { NB_TRA_NONE = 0, NB_TRA_CREATE, NB_TRA_KEEP, NB_TRA_REVERT };   // nb_tr_judge actions

typedef struct nb_tr_img {
    uint32_t frame;               // frame number frame_flags belong to
    uint16_t streak;              // consecutive judged frames that met the conditions
    uint8_t frame_flags;          // NB_TRF_* gathered this frame
    uint8_t ever;                 // NB_TRF_READ / NB_TRF_XFER seen at any time (sticky)
    uint8_t banned, transient;    // transient: the image is created transient now
} nb_tr_img;

static inline const char *nb_tr_reason_name(int r) {
    static const char *const n[NB_TRR_KINDS] = { "candidate", "banned", "shape", "read", "transfer", "unplanned", "load", "store", "streak" };
    return r >= 0 && r < NB_TRR_KINDS ? n[r] : "?";
}
static inline const char *nb_tr_shape_name(int s) {
    static const char *const n[NB_TRS_KINDS] = { "ok", "type", "levels", "layers", "alias", "format" };
    return s >= 0 && s < NB_TRS_KINDS ? n[s] : "?";
}
// type2d: a 2D image (not 1D / 3D / cube); aliased: another image object over the same memory; usage_ok: the driver accepts the
// image with attachment usage + TRANSIENT (vkGetPhysicalDeviceImageFormatProperties; pass 1 when not asked yet).
static inline int nb_tr_shape(int type2d, uint32_t levels, uint32_t layers, int aliased, int usage_ok) {
    if (!type2d) return NB_TRS_TYPE;
    if (levels != 1) return NB_TRS_LEVELS;
    if (layers != 1) return NB_TRS_LAYERS;
    if (aliased) return NB_TRS_ALIAS;
    if (!usage_ok) return NB_TRS_FORMAT;
    return NB_TRS_OK;
}
static inline int nb_tr_begin_frame(nb_tr_img *s, uint32_t frame) {   // 1 = first flag of this frame (the caller lists the image)
    if (s->frame == frame && s->frame_flags) return 0;
    s->frame = frame; s->frame_flags = 0;
    return 1;
}
// A host pass begun with the image as an attachment. planned: begun from the applied NB header; load_ok: begun without its old texels
// (load op CLEAR on every aspect, or DONT_CARE); store_dc: store op DONT_CARE. Returns nb_tr_begin_frame's value.
static inline int nb_tr_pass(nb_tr_img *s, uint32_t frame, int planned, int load_ok, int store_dc) {
    const int first = nb_tr_begin_frame(s, frame);
    s->frame_flags |= NB_TRF_ATT;
    if (!planned) s->frame_flags |= NB_TRF_UNPLANNED;
    else { if (!load_ok) s->frame_flags |= NB_TRF_LOAD; if (!store_dc) s->frame_flags |= NB_TRF_STORE; }
    return first;
}
// Any use outside an attachment (kind NB_TRF_READ / NB_TRF_XFER). Returns 1 when the image is transient now: the caller re-creates it
// with its original usage before the use (contents lost) - it is banned from then on.
static inline int nb_tr_touch(nb_tr_img *s, uint32_t frame, unsigned kind) {
    nb_tr_begin_frame(s, frame);
    s->frame_flags |= (uint8_t)kind; s->ever |= (uint8_t)kind;
    if (!s->transient) return 0;
    s->transient = 0; s->banned = 1; s->streak = 0;
    return 1;
}
// End of the frame for an image that has frame flags of this frame (or is transient). shape: nb_tr_shape (NB_TRS_OK = fits);
// apply: DM_NB_TRANSIENT=1 (create candidates); min_frames <= 0 = 8. *action: NB_TRA_*; the caller performs CREATE / REVERT and
// nb_tr_judge already set transient / banned accordingly (CREATE: transient = 1; REVERT: transient = 0, banned = 1).
static inline int nb_tr_judge(nb_tr_img *s, int shape, int min_frames, int apply, int *action) {
    const unsigned f = s->frame_flags;
    int why = NB_TRR_CAND;
    *action = NB_TRA_NONE;
    if (min_frames <= 0) min_frames = 8;
    s->ever |= (uint8_t)(f & (NB_TRF_READ | NB_TRF_XFER));
    if (s->banned) why = NB_TRR_BANNED;
    else if (shape != NB_TRS_OK) why = NB_TRR_SHAPE;
    else if (s->ever & NB_TRF_READ) why = NB_TRR_READ;
    else if (s->ever & NB_TRF_XFER) why = NB_TRR_XFER;
    else if (!(f & NB_TRF_ATT)) return NB_TRR_CAND;   // transient, not used this frame: nothing to judge, it stays
    else if (f & NB_TRF_UNPLANNED) why = NB_TRR_UNPLANNED;
    else if (f & NB_TRF_LOAD) why = NB_TRR_LOAD;
    else if (f & NB_TRF_STORE) why = NB_TRR_STORE;
    if (why != NB_TRR_CAND) {
        s->streak = 0;
        if (s->transient) { s->transient = 0; s->banned = 1; *action = NB_TRA_REVERT; }
        return why;
    }
    if (s->streak < 0xffff) s->streak++;
    if (s->transient) { *action = NB_TRA_KEEP; return NB_TRR_CAND; }
    if (s->streak < (unsigned)min_frames) return NB_TRR_STREAK;
    if (apply) { s->transient = 1; *action = NB_TRA_CREATE; }
    return NB_TRR_CAND;
}

#ifdef __cplusplus
}
#endif
#endif
