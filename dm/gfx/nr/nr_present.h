// [DOS2] NR-11 DM_NR_PRESENT (docs/dos2/nr/NR-11.md): the game's last passes into the window texture render straight into the swapchain
// image, and the present blit goes away. Pure C (no Vulkan, no engine): the per-frame decisions the engine (xbg_nr_present.inc) and the
// presenter (android/vulkan_present.cpp) make, so tests/nr_present_test.c runs the same code on synthetic frames.
//
// The frame, as the phone's IR dumps show it (ra1 / rb1 / rc1, 41 frames, menu and ship deck): the game clears its window texture X once
// (a whole ClearTexture, kept pending by DM_GFX_CLEAR_FOLD / the NB plan), renders 3D and post into other targets, then sets X as the
// only render target, draws the combine (one full-screen draw that samples the fog target and the UI target), opens one more empty pass
// on X and presents X. X is read by nothing but the present. The window has two textures that alternate.
//
// Rules (every one counted; a frame that breaks one runs the old way):
//  * prediction: the texture this frame presents is the one presented two presents ago (p2; a one-texture window gives p2 == p1);
//  * start: at the SetRenderTargets that attaches the predicted X, only if X's first write this frame is that pending whole clear (so no
//    pass ever LOADs X's old content), the attachment is the whole one-level one-layer single-sample image in its own format, the last
//    present was not cropped and not flipped, conditional rendering does not run on the GPU, and the presenter hands out an acquired
//    swapchain image of X's format and size; the engine then records the rest of the frame into a second command buffer whose batch waits
//    for the acquire (only the tail waits, not the 3D);
//  * while active: every pass on X attaches the swapchain image instead; X's pending clear lands on the swapchain image; any other read
//    or write of X (sampled, copy source / destination, storage, partial clear) aborts: the swapchain image is copied into X and the frame
//    goes on with X and ends with the old blit into the already acquired image;
//  * present: X itself, not aborted, crop full, no flip -> direct (layout to PRESENT_SRC, the slot's last batch signals the present
//    semaphore, the presenter only queue-presents); anything else -> the blit into the acquired image (if the present is not X, X gets the
//    swapchain contents copied back first: mispredict).
//  * DM_NR_PRESENT=2: everything decided and counted, nothing executed (no acquire, no split, the old present); the rules' assumptions are
//    checked: a predicted X that is not presented, a read or write of X after the start, a start without the pending clear -> red lines.
#pragma once
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NRP_MODE_OFF = 0, NRP_MODE_ON = 1, NRP_MODE_CHECK = 2, NRP_MODE_POISON = 3 };
static inline int nrp_clamp_mode(long v) { return v >= 0 && v <= 3 ? (int)v : 0; }

// Why a frame does not start (the first reason that holds; one decision a frame).
enum {
    NRP_R_OK = 0,
    NRP_R_NO_PREDICTION,  // fewer than two presents seen yet
    NRP_R_NOT_CLEARED,    // X has no pending whole colour clear: its first pass would LOAD the old contents
    NRP_R_WRITTEN,        // X was already written in this frame
    NRP_R_VIEW,           // view of a part / another format, MSAA, mips or layers
    NRP_R_CROP,           // the last present cropped X (the presenter's buffers take the crop's size)
    NRP_R_FLIP,           // DM_GFX_TAA_FIX lower-left: the present shows a flipped copy
    NRP_R_COND,           // a query or a GPU conditional-rendering predicate (DM_GFX_COND_RENDER=1) open across the command-buffer split
    NRP_R_PRESENTER,      // the presenter said no (its reason below)
    NRP_R_N
};
static inline const char *nrp_reason_name(int r) {
    static const char *const n[NRP_R_N] = {"started", "no prediction", "no pending whole clear", "written earlier this frame", "partial view / MSAA",
                                           "cropped", "flipped", "query / GPU predicate open", "presenter"};
    return r >= 0 && r < NRP_R_N ? n[r] : "?";
}
// The presenter's reasons (dm_present_nrp_query / dm_present_nrp_take).
enum {
    NRP_P_OK = 0,
    NRP_P_OFF,           // the presenter has NR-11 off (DM_NR_PRESENT unset there) or no direct support
    NRP_P_NO_SWAPCHAIN,  // no swapchain / surface / window (background, start-up)
    NRP_P_USAGE,         // the swapchain images are not colour attachments (surface does not allow it)
    NRP_P_FORMAT,        // swapchain format != X's format
    NRP_P_EXTENT,        // swapchain extent != X's size (letterbox / scaling blit needed)
    NRP_P_SHARPEN,       // DM_GFX_SHARPEN: RCAS runs between X and the screen
    NRP_P_REBUILD,       // the swapchain is about to be recreated
    NRP_P_JOB_PENDING,   // the present thread still has a job (it may need the parked image)
    NRP_P_NO_IMAGE,      // no swapchain image acquired yet (display-bound: the present thread is still waiting for one)
    NRP_P_ERROR,         // acquire failed
    NRP_P_ROTATED,       // the swapchain is pre-rotated (preTransform != IDENTITY): the game's landscape passes cannot draw into it as they are
    NRP_P_N
};
static inline const char *nrp_presenter_name(int r) {
    static const char *const n[NRP_P_N] = {"ok", "off", "no swapchain", "usage", "format", "extent", "sharpen", "rebuild", "job pending", "no image",
                                           "acquire error", "pre-rotated"};
    return r >= 0 && r < NRP_P_N ? n[r] : "?";
}
// Why an active frame ends with the blit after all.
enum { NRP_F_NONE = 0, NRP_F_MISPREDICT, NRP_F_READ, NRP_F_WRITE, NRP_F_CROP, NRP_F_FLIP, NRP_F_N };
static inline const char *nrp_fallback_name(int f) {
    static const char *const n[NRP_F_N] = {"-", "another texture presented", "X read", "X written outside a pass", "cropped present", "flipped present"};
    return f >= 0 && f < NRP_F_N ? n[f] : "?";
}

enum { NRP_S_IDLE = 0, NRP_S_ACTIVE, NRP_S_ABORTED, NRP_S_WOULD };
// What happens to X (nrp_touch kinds) and what the engine does about it.
enum { NRP_T_PASS = 0, NRP_T_READ, NRP_T_WRITE, NRP_T_CLEAR };
enum { NRP_A_NONE = 0, NRP_A_SUBST, NRP_A_ABORT };
enum { NRP_V_OLD = 0, NRP_V_DIRECT, NRP_V_FALLBACK };

typedef struct nrp_facts {
    int pending_clear;     // X has a pending whole colour clear
    int written;           // X written since the last present
    int whole_view;        // whole image, own format, 1 level, 1 layer, 1 sample
    int crop_full;         // the last present was not cropped
    int flip;              // the present flips (DM_GFX_TAA_FIX lower-left)
    int cond_gpu;          // conditional rendering on the GPU is in use
    int presenter;         // NRP_P_*
} nrp_facts;

typedef struct nrp_frame {
    uint64_t p1, p2;                      // the last two presented serials (p1 newest)
    uint64_t target;                      // X of this frame (0 = none)
    int phase;                            // NRP_S_* (not "state": the engine has a macro of that name)
    int tried;                            // the frame's start decision was made
    int fail;                             // NRP_F_* that aborted it
    uint32_t passes, clears;              // passes attached to the swapchain image, pending clears landed on it
} nrp_frame;

typedef struct nrp_pres_stats {
    uint64_t frames, started, would, direct, would_direct, fallback, would_fallback, passes, clears;
    uint64_t why[NRP_R_N], presenter[NRP_P_N], fail[NRP_F_N], would_fail[NRP_F_N];
    uint64_t red;                         // mode 2: assumption broken (mispredict, read / write after the start)
} nrp_pres_stats;

static inline uint64_t nrp_predict(const nrp_frame *f) { return f->p2; }

static inline int nrp_start_reason(const nrp_facts *x) {
    if (!x->pending_clear) return x->written ? NRP_R_WRITTEN : NRP_R_NOT_CLEARED;
    if (x->written) return NRP_R_WRITTEN;
    if (!x->whole_view) return NRP_R_VIEW;
    if (!x->crop_full) return NRP_R_CROP;
    if (x->flip) return NRP_R_FLIP;
    if (x->cond_gpu) return NRP_R_COND;
    if (x->presenter != NRP_P_OK) return NRP_R_PRESENTER;
    return NRP_R_OK;
}

// The SetRenderTargets that attaches `serial`. Returns 1 when the caller must decide a start now (serial is the prediction and the frame
// has not decided yet); the caller then fills nrp_facts (asking the presenter last: take only when every other rule holds) and calls
// nrp_decide.
static inline int nrp_wants(const nrp_frame *f, uint64_t serial) { return !f->tried && serial && f->p2 && serial == f->p2; }
// Records the decision. would = mode 2 (counted, not executed). Returns the reason (NRP_R_OK = started / would start).
static inline int nrp_decide(nrp_frame *f, nrp_pres_stats *s, uint64_t serial, const nrp_facts *x, int would) {
    f->tried = 1;
    const int r = nrp_start_reason(x);
    s->why[r]++;
    if (r == NRP_R_PRESENTER && x->presenter > 0 && x->presenter < NRP_P_N) s->presenter[x->presenter]++;
    if (r != NRP_R_OK) return r;
    f->target = serial; f->phase = would ? NRP_S_WOULD : NRP_S_ACTIVE; f->fail = NRP_F_NONE; f->passes = f->clears = 0;
    if (would) s->would++; else s->started++;
    return r;
}
// Something happens to an image while a frame is active. Returns NRP_A_SUBST (attach / clear the swapchain image), NRP_A_ABORT (copy the
// swapchain image back into X now, then go on with X) or NRP_A_NONE. A WOULD frame only records the failure (mode 2 executes the old way).
static inline int nrp_touch(nrp_frame *f, uint64_t serial, int kind) {
    if (!serial || serial != f->target) return NRP_A_NONE;
    if (f->phase == NRP_S_ACTIVE || f->phase == NRP_S_WOULD) {
        if (kind == NRP_T_PASS) { f->passes++; return f->phase == NRP_S_ACTIVE ? NRP_A_SUBST : NRP_A_NONE; }
        if (kind == NRP_T_CLEAR) { f->clears++; return f->phase == NRP_S_ACTIVE ? NRP_A_SUBST : NRP_A_NONE; }
        if (!f->fail) f->fail = kind == NRP_T_READ ? NRP_F_READ : NRP_F_WRITE;
        if (f->phase == NRP_S_ACTIVE) { f->phase = NRP_S_ABORTED; return NRP_A_ABORT; }
    }
    return NRP_A_NONE;
}
// The present of `serial`. *copy_back = 1 when X (the redirected texture, not the presented one) must get the swapchain contents first.
// Returns NRP_V_OLD (no frame active, or mode 2), NRP_V_DIRECT or NRP_V_FALLBACK (blit into the image the frame acquired).
// Moves the prediction on and resets the frame.
static inline int nrp_present(nrp_frame *f, nrp_pres_stats *s, uint64_t serial, int crop_full, int flip, int *copy_back) {
    int v = NRP_V_OLD;
    *copy_back = 0;
    s->frames++;
    if (f->phase != NRP_S_IDLE) {
        int fail = f->fail;
        if (!fail && serial != f->target) fail = NRP_F_MISPREDICT;
        if (!fail && !crop_full) fail = NRP_F_CROP;
        if (!fail && flip) fail = NRP_F_FLIP;
        s->passes += f->passes; s->clears += f->clears;
        if (f->phase == NRP_S_WOULD) {
            if (fail) { s->would_fallback++; s->would_fail[fail]++; if (fail == NRP_F_MISPREDICT || fail == NRP_F_READ || fail == NRP_F_WRITE) s->red++; }
            else s->would_direct++;
        } else {
            if (fail) { s->fallback++; s->fail[fail]++; v = NRP_V_FALLBACK; *copy_back = serial != f->target && f->phase == NRP_S_ACTIVE; }
            else { s->direct++; v = NRP_V_DIRECT; }
        }
    }
    if (serial != f->p1) { f->p2 = f->p1; f->p1 = serial; } else f->p2 = serial;   // two textures alternate; one texture repeats
    f->target = 0; f->phase = NRP_S_IDLE; f->tried = 0; f->fail = NRP_F_NONE; f->passes = f->clears = 0;
    return v;
}

#ifdef VK_VERSION_1_0
// What the presenter hands the engine (dm_present_nrp_take) and gets back with the frame's present (dm_present_async_nrp /
// dm_present_nrp_inline): an acquired swapchain image, its colour-attachment view, the semaphore its acquire signals (the engine's tail
// batch waits on it) and the one the present waits on (the engine signals it for a direct present; the blit signals it otherwise).
typedef struct dm_nrp_img {
    uint32_t index; VkImage image; VkImageView view; VkSemaphore acquired, finished; uint64_t gen; VkFormat format; uint32_t width, height;
} dm_nrp_img;
#endif

#ifdef __cplusplus
}
#endif
