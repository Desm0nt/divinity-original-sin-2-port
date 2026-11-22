// [DOS2] VK-ENGINE-SIDE stage I2 (docs/dos2/nr/VK-ENGINE-SIDE.md sections 4 I2 and 12): occlusion culling in the engine's main-view
// receiver, against the depth of an earlier frame - the way mobile engines do it. Switch DM_VK_ENG_OCC 0 off (default) / 1 on /
// 2 count ("would hide N", nothing changes) / 3 poison (the objects it would hide are drawn, their G-buffer draws magenta).
//
// Owner's bar (22.11): "a human eye cannot tell" - not pixel exact; no visible popping. So, unlike technique Z (vkn_occz.h: a proof
// that every occluder is bit-identical), no signatures and no proof of unchanged content: the object's world box (the engine's own
// culling bound) against a conservative max-depth grid of an earlier frame, reprojected when the camera moved, with guard bands.
//
// Threads and data:
//   worker   (recording thread) the effect function "eng-occ" (order 6) learns the scene depth target (the first depth-only, not
//            square, >= 640 x 360 set of a presented recording, as vkn_eng.h's pre-pass class) and its viewport; when the target set
//            leaves the depth-only pre-pass [v2: not after the G-buffer; rows: NDC y = +1 is row 0, the backend flips the viewport]
//            vkn_eocc_wk_targets records ONE compute dispatch: the max and min depth of each 16 x 16 cell into a host-visible slot
//            (dm/android/shaders/vkn_occz_reduce.comp, technique Z's), with two GPU timestamps around it. The slot is tagged with the
//            engine frame of its recording (vkn_eocc_enc_frame: a record at the frame's begin) and the recording's vkn_frame() (GPU
//            done test). In poison mode it also swaps the shader of the G-buffer draws of the frame's poisoned objects (by the draw's
//            report-pair condition key) for a twin: the engine's vertex module, a magenta fragment module.
//   jobs     (the engine's cull jobs, the host hook of the main-view receiver, dm/host/hle_dos2_vkn_eng.c) vkn_eocc_obj per object:
//            the first call of an engine frame prepares the frame (the newest finished snapshot, its camera, the camera move guard,
//            the hazard grid, the coarse level, the self-check of decisions whose own frame's depth arrived); every call then decides
//            "draw" / "hide" for one object. Lock-free: an object table by address (atomics), a frame state published once.
// Rule for an object O of engine frame E, snapshot s (engine frame E_s < E, camera VP_s, cells of max / min depth):
//   eligible  the host's flag filter (opaque list bit only, no transparent / parts / decal / other-pass / special bits) - the host
//             decides that, this file sees a yes / no;
//   history   O was seen in frame E - 1 too, it is not new, its box / flags did not change this frame, and it was not found visible in
//             the last HOLD frames (DM_VK_ENG_OCC_HOLD, default 4: an object that became visible stays drawn while the snapshots
//             that could show its occluder gone are still in flight);
//   camera    the eye moved < MOVE world units and the view turned < TURN degrees since VP_s (else: a cut - nothing hidden this frame);
//   test      the box projected with this frame's camera -> cells (+ PAD px) at its nearest depth zmin (the CARD). Same camera: every
//             cell's max depth is nearer than zmin by MARGIN (relative linear depth). Moved camera (technique Z v2's reprojection,
//             vkn_occz.h): the card's corners into VP_s, the snapshot cells under it nearer than its nearest depth there by MARGIN;
//             the region swept by the new eye's rays through the card (corners + their meeting points with the region's nearest
//             depth) on the snapshot's screen; every cell of it a continuous surface (SHEET) - so every ray of the new eye through
//             the box meets that surface first;
//   hazards   occluders that may be gone: objects drawn around E_s whose box or flags changed since E_s (a moving character, a wall
//             that started to fade), or that were not seen in E - 1 (destroyed, out of the view). Their box (the union of their boxes
//             while they moved) projected with VP_s marks the cells; a hazard nearer than O in the test cells -> drawn.
// Not exact by design: an occluder whose depth changed without a box / flag change (an animated door leaf inside a static box), or one
// the engine replaces between E_s and E, can reveal O 1-2 snapshot ages late; the self-check counts exactly those frames: when the
// depth of frame E arrives, each hidden O's card cells are tested against E's own depth (mode 1: without O; modes 2 / 3: with O drawn)
// - "violations" = O would have shown. A violating object is drawn for 60 frames.
// Budget: the summed time of the tests of one frame is measured (every call); past DM_VK_ENG_OCC_BUDGET_US (default 2000) the rest
// of the frame is drawn.
// No game addresses (the host file has them).
#ifndef DM_VKN_EOCC_H
#define DM_VKN_EOCC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// ---- the pure model (vkn_eocc_model.c; any thread, no state; host test vkn_eocc_test.c) ----
enum { VKN_EO_CELL = 16, VKN_EO_GW = 256, VKN_EO_GH = 128, VKN_EO_CQ = 4 };   // cells of 16 px; the coarse level: 4 x 4 cells
// Why an object was not hidden (one reason, the first that failed).
enum { VKN_EO_HIDDEN = 0, VKN_EO_R_NOTREADY, VKN_EO_R_NOSNAP, VKN_EO_R_OLD, VKN_EO_R_CUT, VKN_EO_R_NOREPROJ, VKN_EO_R_STAGE, VKN_EO_R_FLAGS,
       VKN_EO_R_NEW, VKN_EO_R_CHANGED, VKN_EO_R_HOLD, VKN_EO_R_NEAR, VKN_EO_R_OFFSCREEN, VKN_EO_R_EDGE, VKN_EO_R_DEPTH, VKN_EO_R_HAZARD,
       VKN_EO_R_SHEET, VKN_EO_R_REPROJ, VKN_EO_R_BUDGET, VKN_EO_R_TABLE, VKN_EO_R_N };
typedef struct vkn_eo_cfg {
    uint32_t pad_px;       // the card's pad in pixels (default 16: one more cell at each edge)
    float margin;          // the occluder must be nearer by this share of the object's linear depth (default 0.02)
    float sheet;           // technique Z's sheet tolerance (relative linear depth range of a cell and a neighbour step; default 0.05)
    float max_move;        // eye move (world units) between the snapshot's camera and this frame's beyond which nothing is hidden
    float max_turn;        // view turn (degrees), same
    int box_reproj;        // [v2] moved camera: reproject the box's 8 corners (1) or technique Z's card of this frame's cells (0)
} vkn_eo_cfg;
typedef struct vkn_eo_snap {
    uint32_t w, h, gw, gh;            // the depth area in pixels (the viewport from 0, 0) and cells
    const float *mx, *mn;             // gw x gh max / min depth, row-major
    const float *cx;                  // coarse max (cgw x cgh, VKN_EO_CQ x VKN_EO_CQ cells each), NULL = none
    uint32_t cgw, cgh;
    float vp[16];                     // the snapshot frame's view-projection (row vectors: clip = (x, y, z, 1) * vp)
    int persp; double la, lb;         // vkn_eo_snap_prep: perspective with linear depth lb / (z - la)
    int yup;                          // [v2] 1 = NDC y = +1 is pixel row 0 (the DOS2 backend: flipped Vulkan viewport, vkn_draw.c); 0 = y = -1 is
} vkn_eo_snap;
// The frame's view of a snapshot (vkn_eo_view_prep, once a frame): this frame's camera, same / moved, the new eye in the snapshot's clip
// space, the hazard grid (gw x gh nearest hazard depth in the snapshot's camera, FLT_MAX = none; NULL = no hazards).
typedef struct vkn_eo_view {
    const vkn_eo_snap *s; const float *hz;
    float vp[16]; double inv[16];
    int same, moved;                  // same camera bytes / a moved camera the reprojection can handle
    double ce[4];
    float move, turn;                 // the camera change (world units, degrees)
} vkn_eo_view;
typedef struct vkn_eo_res { float zmin; uint16_t c[4]; uint32_t cells; } vkn_eo_res;   // the card (this frame's cells) and the work done
int vkn_eo_snap_prep(vkn_eo_snap *s);                                         // derived fields; 0 = ok
// 0 = ok (v->same or v->moved), else VKN_EO_R_CUT (moved / turned too far) / VKN_EO_R_NOREPROJ (no inverse, no eye, not perspective)
int vkn_eo_view_prep(vkn_eo_view *v, const vkn_eo_snap *s, const float vp_now[16], const float *hz, const vkn_eo_cfg *cfg);
int vkn_eo_test(const vkn_eo_view *v, const vkn_eo_cfg *cfg, const float box[6], vkn_eo_res *r);   // VKN_EO_HIDDEN or the reason
// A hazard's footprint: box projected with the snapshot's camera into hz (min depth per cell, + pad px). 1 = it reaches behind the
// snapshot's camera plane: the whole grid (-FLT_MAX); 0 = done / off screen.
int vkn_eo_hz_add(float *hz, const vkn_eo_snap *s, const float box[6], uint32_t pad);
void vkn_eo_coarse(const float *mx, uint32_t gw, uint32_t gh, float *cx, uint32_t *cgw, uint32_t *cgh);   // max over 4 x 4 cells
// The self-check of one decision against its own frame's depth (s = the snapshot of that frame): 1 = a cell of the card is not nearer
// than zmin (the object could have shown), 0 = it stayed covered.
int vkn_eo_check(const vkn_eo_snap *s, float zmin, const uint16_t c[4]);
const char *vkn_eo_reason(int r);

// ---- the runtime (vkn_eocc.c) ----
// [v2] the host's flag filter as a group (vkn_eocc_obj's `eligible`): 1 = may be hidden; else why not (counted in the census line)
enum { VKN_EO_FL_NOTOPAQUE = 0, VKN_EO_FL_OK = 1, VKN_EO_FL_ALPHA, VKN_EO_FL_PARTS, VKN_EO_FL_OTHER, VKN_EO_FL_SPECIAL, VKN_EO_FL_BIT0,
       VKN_EO_FL_CHARACTER, VKN_EO_FL_N };
void vkn_eocc_init(int mode);              // DM_VK_ENG_OCC (and its _* switches, read here); 0 = off: nothing registered
int vkn_eocc_mode(void);
// The host hook, per object of the main-view receiver (job threads). stage: the receiver's first argument (the stage object);
// eframe: the engine's frame counter; vp: the stage camera's view-projection (NULL = unknown / stale); box: the world box;
// eligible: VKN_EO_FL_OK = the host's flag filter passed, else its group (VKN_EO_FL_*); fl_hash: the object's flags (any change of them counts as a change of the object).
// Returns 1 = hide (do not call the original), 0 = call it. *poison = 1: call the original, then vkn_eocc_poison_key with its pair.
int vkn_eocc_obj(uint64_t stage, uint64_t obj, uint64_t eframe, const float *vp, const float box[6], int eligible, uint32_t fl_hash, int *poison);
// After the original ran (call it always when vkn_eocc_obj returned 0): took_pair = the object holds a report pair of this frame
// (key = its entry: the condition key of its G-buffer draws), drew = it went into the opaque (pre-pass + G-buffer) lists.
void vkn_eocc_after(uint64_t stage, uint64_t obj, uint64_t eframe, int drew, uint64_t pair_key, int poison);
void vkn_eocc_enc_frame(uint64_t eframe);  // encoder thread, a presented frame's begin (after vkn_rt_rec_begin): tags the recording
// [v2] encoder thread, a report pair begins (packet 0x3d): `key` is the engine's pair entry; entry + 8 = the cull frame that allocated it.
// The first of a presented recording LABELS the recording's snapshot with that cull frame (the engine may build the next frame's lists
// while this one is drawn: the frame counter at the draw's begin is not always the cull frame of the lists it draws).
void vkn_eocc_enc_pair(uint64_t key);
struct vkn_tex;
void vkn_eocc_wk_targets(uint32_t n, struct vkn_tex *const *color, struct vkn_tex *depth);   // worker, before vkn_pass_targets
void vkn_eocc_census(void);                // the "[vkn] eng occ" line(s), every 10 s
// ---- host tests ----
typedef struct vkn_eocc_stats {
    uint64_t frames, calls, tested, hidden, would, poison, why[VKN_EO_R_N], snaps, snap_fail, checked, violations, gb_draws, gb_would, pre_draws, twins;
} vkn_eocc_stats;
void vkn_eocc_stats_get(vkn_eocc_stats *s);
void vkn_eocc_test_reset(int mode);        // forget the session state (table, frames, snapshots); mode as if read
float *vkn_eocc_test_slot(int i, uint64_t *eframe, uint64_t *rframe);   // the CPU planes of snapshot slot i (max, then min)

#ifdef __cplusplus
}
#endif
#endif
