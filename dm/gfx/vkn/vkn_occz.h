// [DOS2] VK-NATIVE stage 5, technique Z: "depth snapshot + engine boxes, decided on the engine thread" (vkn_occz.c, the pure model
// vkn_occz_model.c). Design and reasons: docs/dos2/nr/VK-OCCLUSION-CHEAP.md. DM_VK_NATIVE_COND 10 count / 11 verify / 12 on / 13 poison.
//
// Idea. The prize is the recording worker's time (13.3 of 13.4 ms a frame; oracle U16: 405 pre-pass + 410 G-buffer draws a frame hidden,
// ~4.15 us each). So a hidden object's draws must never reach the worker's draw path, and the worker must do no culling work: it only
// looks a verdict up (one hash probe per pair draw). Everything else runs off the worker:
//   GPU      after the depth pre-pass of a presented recording the worker records ONE compute dispatch (vkn_occz_reduce.comp): the max
//            depth of every 16 x 16 pixel cell of the pre-pass target into a host-visible buffer (a "snapshot", ~33 KB at 2160 x 972);
//   jobs     the engine's per-object gather hook (0xb0c570, job threads; dm/host/hle_dos2_vkn_occ.c) projects the object's world box
//            (obj+0x14c) with the stage camera: NDC rect + nearest depth z/w (vkn_occz_gather);
//   engine   the encoder (vkn_rt.c taps, engine thread) sees every pair, every pre-pass draw with its constants (ring bytes) and keeps a
//            content signature per object; at the frame's end (vkn_rt_rec_end) the model decides every pair of THIS frame with the
//            newest snapshot the GPU finished (frame s < F) and publishes a verdict table for the frame; the worker takes it at the
//            recording's begin (late = no skips that frame).
// Rule (exact; proof in the doc, section 3). Object O of frame F is skipped (its pre-pass draws and its conditional draws) when:
//   1. a snapshot s exists with the same camera (view-projection bytes), the same pre-pass target and size, s >= F - 7;
//   2. the frame globals did not change in (s, F]: draws on the pre-pass target outside pairs, its clears, pairs without a box, any
//      order-sensitive depth state (stencil test, a compare other than LESS / LEQUAL with writes) -> "scene";
//   3. O is clean (every draw of its pair on the pre-pass target, full viewport) and has a box rect in front of the camera;
//   4. every cell under O's rect (+ pad px) has snapshot depth < zmin(O) - eps, AND no HAZARD object reaches nearer than zmin(O) - eps
//      into that cell. A hazard is an object whose box or content signature changed in (s, F], or that was present at s and is gone
//      at F; its footprint is the union of its rects (and min of its nearest depths) over [s, F] (unknown rect = the whole screen).
//   Then every pixel of the cell got its depth at s from an object that is present at F unchanged, so the depth there at F is no
//   farther, and O (all of it inside its box: the engine's own culling premise) fails the depth test everywhere: its pre-pass draws
//   write nothing, its depth-tested conditional draws draw nothing. The worker still refuses a skip of a pre-pass draw whose stencil
//   would be written on a depth fail, and of a conditional draw that is not depth-tested LESS / LEQUAL / EQUAL (or not on that target).
// v2 (22.11, the doc section 10): a frame whose camera differs from the snapshot's is decided by REPROJECTION: the pair's card (its own
//   cells of rule 4 at its nearest depth, a quad in this frame's camera) goes into the snapshot's camera; the snapshot cells under it
//   must be nearer, and the region the new eye's rays through the card sweep in the snapshot (corners + their meeting points with the
//   region's nearest depth) must be one continuous surface ("sheet": min / max per cell, neighbours touching; snapshot = max + min) with
//   no hazard nearer. Signatures leave the per-view block's projection rows out (vkn_occz_cam_block) when the shader reads no view row.
//   Order-sensitive (rule 2) = a depth WRITE under a stencil test, or one that can raise the depth after another write; hazards use
//   their rect in the snapshot's frame only.
// Self-check (every mode): when the snapshot of frame f arrives, the decisions made for f are re-tested against f's own depth (in mode
// 12 that depth lacks the skipped objects: exactly the truth to check). A violation is printed red; in mode 12 the first one turns the
// skips off for the session (auto-fallback, no player flags).
// Budget (owner rule 21.11): the engine-side cost (taps + model + check) is measured every frame and compared with the engine thread's
// slack (the backpressure wait at the frame end, vkn_rt stats). Budget = min(DM_VK_NATIVE_OCCZ_BUDGET_US (2000), 1/2 of the slack EMA);
// the model stops deciding when a frame's budget is spent (the rest is drawn: still exact); when the taps alone stay over the budget for
// 32 frames, level 0 (taps off, nothing decided) with a 16-frame probe every 512 frames. Both numbers and the net gain estimate are in
// the census line ("occ-z").
// No game addresses (the host file has them).
#ifndef DM_VKN_OCCZ_H
#define DM_VKN_OCCZ_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_OZ_CELL = 16, VKN_OZ_GW = 256, VKN_OZ_GH = 128, VKN_OZ_MAXN = 4096, VKN_OZ_HIST = 8 };
enum { VKN_OZ_DRAW = 0, VKN_OZ_HIDDEN = 1, VKN_OZ_POISON = 2 };
// Why a pair was not decided hidden (one reason per pair, the first that failed). [v2] REPROJ: the camera moved and the swept region of
// the box could not be built (a corner or the new eye on the wrong side of the snapshot camera, the region leaves the snapshot's screen,
// the occluder depth did not settle); SHEET: a cell of the swept region is not a continuous surface (depth range / a jump to a neighbour).
enum { VKN_OZ_R_HIDDEN = 0, VKN_OZ_R_NOSNAP, VKN_OZ_R_CAMERA, VKN_OZ_R_TARGET, VKN_OZ_R_SCENE, VKN_OZ_R_OLD, VKN_OZ_R_NOOBJ, VKN_OZ_R_NORECT,
       VKN_OZ_R_UNCLEAN, VKN_OZ_R_OFFSCREEN, VKN_OZ_R_DEPTH, VKN_OZ_R_HAZARD, VKN_OZ_R_BUDGET, VKN_OZ_R_REPROJ, VKN_OZ_R_SHEET, VKN_OZ_R_N };

// ---- the pure model (vkn_occz_model.c; the engine thread, or a host test) ----
typedef struct vkn_oz_obj {
    uint64_t key;          // the pair (its entry address: what the worker looks up)
    uint64_t obj;          // the engine object, 0 = no gather data (identity unknown: such pairs go into the frame's global hash)
    float ndc[4];          // x0, y0, x1, y1 of the box's projection
    float zmin;            // nearest z / w of the box corners
    uint8_t rect;          // 1 = ndc / zmin valid (every corner in front of the camera)
    uint8_t clean;         // 1 = a candidate: every draw of the pair on the pre-pass target, full viewport, safe depth state
    uint64_t box_h, sig;   // hash of the raw world box; content signature of the pair's draws WITHOUT the camera rows (v2: the camera is
                           // the reprojection's business; a draw whose shader reads the view itself keeps its camera bytes in here)
    uint32_t ndraw, ncond; // its pre-pass draws / conditional draws this frame (the savings estimate)
    uint64_t sig_full;     // [v2] the signature WITH the camera bytes (diagnostic: "changed by the camera only")
} vkn_oz_obj;
typedef struct vkn_oz_frame {
    uint64_t frame, cam, tex, global;   // frame number (the recording's vkn_frame()); camera key (0 = unknown / mixed); target identity; global hash
    uint32_t w, h;                      // pre-pass target size (0 = none seen)
    int order_sensitive;
    const vkn_oz_obj *o; uint32_t n;
    const float *vp;                    // [v2] the frame's view-projection (16 floats, row vectors: clip = (x, y, z, 1) * vp); NULL = unknown
} vkn_oz_frame;
typedef struct vkn_oz_snap {
    uint64_t frame, cam, tex; uint32_t w, h, gw, gh;
    const float *cell;                  // gw x gh max depths, row-major
    const float *vp;                    // [v2] the snapshot frame's view-projection; NULL = unknown (no reprojection)
    const float *cmin;                  // [v2] gw x gh MIN depths; NULL = none (no reprojection)
} vkn_oz_snap;
// sheet: [v2] the relative range of linear depth a cell (and a pair of neighbour cells) may span to count as one continuous surface under a
// moved camera; 0 = no reprojection (v1: the snapshot's own camera only).
typedef struct vkn_oz_cfg { float eps; uint32_t pad_px; uint64_t budget_ns; float sheet; } vkn_oz_cfg;
typedef struct vkn_oz_stats {
    uint64_t frames, pairs, cands, hidden, poison, why[VKN_OZ_R_N], hazards, hazard_whole, would_pre, would_cond, model_ns;
    uint64_t checked, violations, check_frames, check_lost, snaps_used, snap_age_sum;
    uint64_t changed, order_frames;   // objects whose box / signature changed this frame (churn); order-sensitive frames
    // [v2 diagnosis] the churn split: by the box, by the camera-free signature, by the camera bytes only (would be churn without the split)
    uint64_t changed_box, changed_sig, changed_cam_only;
    // [v2] frames decided under a moved camera (snapshot camera != frame camera) and their candidates / hidden pairs
    uint64_t moved_frames, moved_cands, moved_hidden, moved_region_cells;
} vkn_oz_stats;
void vkn_ozm_reset(void);
void vkn_ozm_cfg(const vkn_oz_cfg *c);
// Decides frame `f` (verdict[i] for f->o[i]; NULL snapshot = none). `now` = a clock for the budget (NULL = no budget). Returns hidden.
uint32_t vkn_ozm_frame(const vkn_oz_frame *f, const vkn_oz_snap *s, uint8_t *verdict, uint64_t (*now)(void));
// The self-check: snapshot s of frame s->frame against the decisions stored for that frame. Returns violations found (the first ones are
// handed to `red` with the pair key, its zmin, the cell value and the cell).
typedef void (*vkn_oz_red_fn)(uint64_t key, float zmin, float cell, uint32_t cx, uint32_t cy, uint64_t frame);
uint32_t vkn_ozm_check(const vkn_oz_snap *s, vkn_oz_red_fn red);
void vkn_ozm_stats_get(vkn_oz_stats *s);
// Pure helpers (tests): the box projection (0 ok, -1 behind the camera, -2 bad box) and the cell rect of an NDC rect.
int vkn_oz_project(const float box[6], const float vp[16], float ndc[4], float *zmin);
int vkn_oz_cells(const float ndc[4], uint32_t w, uint32_t h, uint32_t gw, uint32_t gh, uint32_t pad, uint32_t c[4]);   // 0 = on screen
// [v2] the eye of a perspective view-projection (the point with clip x = y = w = 0): 0 ok, -1 none (orthographic / singular);
// its linear depth: w = b / (z_ndc - a) for a standard projection (clip z = a w + b): 0 ok, -1 not a standard perspective.
int vkn_oz_eye(const float vp[16], double eye[3]);
int vkn_oz_lin(const float vp[16], double *a, double *b);

// ---- the gather hand-off (any thread: the host hook of 0xb0c570) ----
void vkn_occz_gather_bases(uint64_t base0, uint64_t base1);
void vkn_occz_gather(uint64_t key, uint64_t eframe, uint64_t obj, const float box[6], const float vp[16]);
// [v2] the engine's per-view constant block (the host file knows the game: DOS2 PerView, uniform slot 12, row-major: rows 0..3 View,
// 4..7 Projection, 8..11 ViewProjection, 12 ViewPos). Its rows 4..11 are left out of the camera-free signature of a draw whose shader
// reads no other camera row (View / ViewPos: a view-dependent shape, e.g. a billboard), and only while the block is consistent
// (rows 8..11 = rows 0..3 x rows 4..7, checked per block): the reprojection accounts for the projection. view_too = rows 0..3 and 12
// count as pure too (DM_VK_NATIVE_OCCZ_CAMVIEW=1). slot >= 16 = none (every camera byte stays in the signature).
void vkn_occz_cam_block(uint32_t slot, int view_too);

// ---- install (vkn_query_init for modes 10..13) ----
void vkn_occz_init(int mode);              // 0 = off
int vkn_occz_mode(void);
extern int vkn_occz_enc_on;                // the encoder taps run (mode != 0 and level > 0)

// ---- the encoder taps (vkn_rt.c, engine thread; weak there) ----
struct vkn_tex; struct vkn_draw_in;
void vkn_occz_enc_rec_begin(int presented);
void vkn_occz_enc_rec_end(int present);
void vkn_occz_enc_targets(uint32_t n, struct vkn_tex *const *color, struct vkn_tex *depth);
void vkn_occz_enc_ubo(int stage, uint32_t slot, uint32_t off, uint32_t size);
void vkn_occz_enc_qbegin(uint64_t key);
void vkn_occz_enc_qend(uint64_t key);
void vkn_occz_enc_cond(uint64_t key);
void vkn_occz_enc_draw(const struct vkn_draw_in *in, int indexed, uint32_t first, uint32_t count, uint32_t instances);
void vkn_occz_enc_depth_event(struct vkn_tex *t);   // a clear / copy that may change a depth target (NULL = the bound one)

// ---- the worker (recording thread) ----
void vkn_occz_wk_targets(uint32_t n, struct vkn_tex *const *color, struct vkn_tex *depth);   // vkn_rt.c: before vkn_pass_targets (every RT mode)
// ---- the query module's side (vkn_query.c, recording thread) ----
void vkn_occz_q_rec_begin(void);                                         // a recording begins (its occ_begin listener)
void vkn_occz_q_copied(int slot, uint32_t q, uint64_t key, int whole);   // mode 11: query q of the slot holds pair `key`
void vkn_occz_q_result(int slot, uint32_t q, uint32_t samples);
void vkn_occz_census(void);                                             // the "[vkn] occ-z" line(s)
// ---- host tests: every snapshot slot's cells set to v (the mock runs no compute shader); forget the session state; the fallback flag ----
void vkn_occz_test_fill(float v);
void vkn_occz_test_reset(void);
int vkn_occz_test_off(void);

#ifdef __cplusplus
}
#endif
#endif
