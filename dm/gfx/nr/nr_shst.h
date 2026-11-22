// [DOS2] VK-ENGINE-SIDE I1a (docs/dos2/nr/VK-ENGINE-SIDE.md section 13): the engine side's plan of the persistent static-caster
// shadow layer, one cascade at a time - pure C (no engine types, no game address, no Vulkan, no environment), used by
// dm/host/hle_dos2_vkn_eng.c (the engine's cascade draw build) and by the host test (tests/vkn_stage3_test.c block "eng I1a").
//
// The scheme. Per cascade the engine's draw build gets the cull list of its casters (objects: a key = the object's address, a hash of
// the words that place and select it = sphere, box, flags, the visual word of the list entry, its world box) and the cascade camera's
// words (NRST_CAMW: the view-projection, the view rows, the camera position, the far plane, one more word: everything the cascade's
// per-view constants are made of). A LAYER is the tile of the cascade with only its static members drawn into it (the backend copies the
// atlas tile into its layer image at the marker MID of a BUILD). While the layer is valid the members are NOT given to the draw build
// at all (no engine work, no draw, no recording) and the backend copies the layer into the cleared tile at the cascade's START; every
// other caster is drawn on top as the engine asks. Order does not matter: depth-only LESS / LEQUAL into a fixed-point atlas is
// min(clear, fragments) (docs/dos2/nr/NR-SHCACHE.md section 0), so min(layer, others) = the engine's tile when the members' fragments
// are what they would be now.
//   static      a caster is a candidate when its words did not change for `stable` frames AND the backend's verdict for it is OK: every
//               draw it made in this cascade (between its OBJ marker and the next marker) was an order-free depth-only draw into the
//               tile with an affine position program and no acting fragment stage, and two observations in two frames had the same
//               signature (mesh / index content versions, shader and states, its own position constants). Wind (no affine program or
//               own constants that change), skinning (bones: no affine program), dissolve (the fragment stage acts), CPU-written meshes
//               (content versions) and moved objects (words) are never in a layer.
//   exact       the layer is taken bit for bit only while the camera words are those of its build (REUSE, no shift); a camera whose
//               view-projection only translated by whole texels in x / y (the linear part, the z / w translation, the view's linear part
//               and z translation, far plane and the last word bit-equal; nr_shs.h nr_ss_shift within eps) takes the layer shifted by
//               (nx, ny) texels (the backend copies with that offset: the scroll rules of VK-SHADOWS-MOVING, x / y only) and the members
//               whose box touches the exposed strips are drawn (TOUCH); anything else (turned, z moved, not whole texels, a shift over
//               shift_max) builds again.
//   invalid     a member's words changed (moved, re-selected), a member missing from the list while its box at the camera of now
//               touches the tile (destroyed, hidden, culled), the backend said the layer is bad (NACK) or never said anything (NOACK),
//               enough new candidates since the build (REFRESH), age, another tile: the layer is dropped in THIS frame (the decision
//               is made before anything of the frame is drawn) and built again when the camera stands one frame and there are
//               >= min_static candidates.
//   ack         the backend publishes per cascade (epoch, good / bad) after the MID of a build; a valid layer is reused when its
//               epoch is acked good, or optimistically (trust) after 3 good acks in a row; otherwise the frame draws everything (HOLD).
// Groups of the cull list (the draw build emits them in this order, see nrst_plan_frame): STATIC (BUILD: drawn first, each with an OBJ
// marker, then MID), TOUCH (REUSE: strip touchers), OBS (casters whose verdict is not known yet: each with an OBJ marker, then NONE),
// DYN (the rest, no marker), LEFT (REUSE: not given to the draw build; VERIFY: drawn between V1 and V2).
#ifndef DM_NR_SHST_H
#define DM_NR_SHST_H
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "nr_shs.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { NRST_TAB = 8192, NRST_PROBE = 32, NRST_CAMW = 37, NRST_VP = 0, NRST_VIEW = 16, NRST_POS = 32, NRST_FAR = 35, NRST_AUX = 36, NRST_MARGIN = 2 };
enum { NRST_A_OFF, NRST_A_HOLD, NRST_A_BUILD, NRST_A_REUSE, NRST_A_VERIFY, NRST_A_N };            // the cascade's action this frame
enum { NRST_G_DYN, NRST_G_OBS, NRST_G_STATIC, NRST_G_TOUCH, NRST_G_LEFT, NRST_G_N };             // a caster's group
enum { NRST_V_UNKNOWN, NRST_V_ONCE, NRST_V_OK, NRST_V_NOT };                                     // the backend's verdict
// why a valid layer was dropped (counted once per drop)
enum { NRST_R_NONE, NRST_R_TURN, NRST_R_Z, NRST_R_FRAC, NRST_R_BIG, NRST_R_MOVED, NRST_R_GONE, NRST_R_REFRESH, NRST_R_AGE, NRST_R_NACK,
       NRST_R_NOACK, NRST_R_TILE, NRST_R_OFF, NRST_R_N };
// why no layer was built in a frame without one (counted per frame)
enum { NRST_N_BUILT, NRST_N_FEW, NRST_N_MOVING, NRST_N_OFF, NRST_N_N };
static inline const char *nrst_r_name(int r) {
    static const char *const n[NRST_R_N] = {"none", "turned", "z moved", "not whole texels", "shift too big", "member moved", "member gone in the tile",
                                            "refresh", "age", "backend said bad", "no ack", "tile", "off"};
    return r >= 0 && r < NRST_R_N ? n[r] : "?";
}

typedef struct nrst_par {
    int mode;                 // 0 off, 1 on, 2 verify (VERIFY instead of REUSE: everything drawn), 3 poison (as 1; the backend poisons)
    uint32_t stable;          // frames a caster's words must stand to be a candidate (default 8)
    uint32_t min_static;      // candidates a layer needs (default 16)
    uint32_t shift_max;       // texels a layer may be shifted from its build in x or y (default 64)
    uint32_t refresh_min;     // new candidates that make a build again (at least; or a quarter of the members) (default 16)
    uint32_t age;             // frames a layer lives (default 1800)
    uint32_t reobs;           // frames after which a NOT caster is observed again (default 600)
    uint32_t noack;           // frames a build may wait for its ack before it is dropped (default 12)
    uint32_t trust;           // 1: reuse a layer before its ack after 3 good acks in a row of this cascade (default 0 since fix2: phone G06)
    double eps;               // px: how far a shift may be from whole texels (default 1 / 64)
} nrst_par;
static inline nrst_par nrst_par_default(void) {
    nrst_par p; memset(&p, 0, sizeof p);
    p.mode = 1; p.stable = 8; p.min_static = 16; p.shift_max = 64; p.refresh_min = 16; p.age = 1800; p.reobs = 600; p.noack = 12; p.trust = 0; p.eps = 1.0 / 64.0;
    return p;
}

typedef struct nrst_obj {
    uint64_t key;             // 0 = empty
    uint32_t h, since, seen;  // words hash; the frame it last changed; the frame it was last in the list
    uint32_t tag;             // the occupancy number of this slot (the backend's verdict is per (slot, tag))
    uint32_t mem, mseen;      // member of layer epoch `mem` (0 none); the frame it was counted as a present member
    uint32_t obs;             // the frame it was last observed (OBJ marker)
    float box[6];             // world box: min x y z, max x y z (as the engine stores it; sorted per axis when used)
} nrst_obj;
typedef struct nrst_in { uint64_t key; uint32_t h; float box[6]; } nrst_in;
typedef struct nrst_stats {
    uint64_t frames, act[NRST_A_N], drop[NRST_R_N], nob[NRST_N_N];
    uint64_t objs, left, touch, statics, obs, dyn, members, builds, full;
    uint64_t acks_good, acks_bad, optimistic, held, shifted, exact;
} nrst_stats;
typedef struct nrst_cas {
    nrst_obj tab[NRST_TAB];
    uint32_t fr, tag_next, epoch_next;
    // the layer
    int valid, conv; uint32_t epoch, built, nmem, good, acked; uint32_t cam[NRST_CAMW]; int32_t rect[4];
    // the previous frame's camera (a build waits for a camera that stood one frame)
    uint32_t prev[NRST_CAMW]; int have_prev;
    nrst_stats st;
} nrst_cas;
typedef struct nrst_plan {
    int act, drop, nob;       // the action; why a valid layer was dropped this frame (NRST_R_*); why none was built (NRST_N_*)
    uint32_t epoch;           // the layer's epoch (BUILD: the new one)
    int32_t nx, ny;           // REUSE / VERIFY: the layer's shift in texels (tile pixel p of now holds layer pixel p - n)
    int ne; nr_ss_rect e[2];  // the exposed strips (tile-local)
    uint32_t n[NRST_G_N];     // casters by group
    uint32_t members, cands;
} nrst_plan;

static inline void nrst_init(nrst_cas *c) { memset(c, 0, sizeof *c); }
// [fix2 22.11] The backend's ack (vkn_shc.h: 1 confirmed, 2 bad, 3 built - camera not yet confirmed) as nrst_plan_frame takes it for the
// cascade's current layer `cur`: bad only for that epoch; safe (DM_VK_ENG_SHSTATIC_SAFE: the members are still given and the backend
// checks every use) = acked at once; otherwise acked only when confirmed (a frame after the build had the layer's camera).
static inline void nrst_ack_view(int safe, uint32_t cur, uint32_t ae, int as, uint32_t *ack_epoch, int *ack_bad) {
    *ack_bad = 0; *ack_epoch = 0;
    if (as == 2 && ae == cur) { *ack_bad = 1; *ack_epoch = ae; }
    else if (safe) *ack_epoch = cur;
    else if (as == 1) *ack_epoch = ae;
}

// The view-projection's layout: 0 = row vectors (clip[c] = sum_r p[r] m[4r + c]; the translation m[12..14]), 1 = column vectors (the
// translation m[3], m[7], m[11]), -1 = neither (not orthographic with w = 1: no shift, exact reuse only).
static inline int nrst_conv(const float *m) {
    if (m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f && m[15] == 1.0f) return 0;
    if (m[12] == 0.0f && m[13] == 0.0f && m[14] == 0.0f && m[15] == 1.0f) return 1;
    return -1;
}
static inline float nrst_m(const float *m, int conv, int r, int c) { return conv ? m[4 * c + r] : m[4 * r + c]; }   // coefficient of input r in output c
static inline int nrst_idx(int conv, int r, int c) { return conv ? 4 * c + r : 4 * r + c; }
// The tile-local pixel rectangle a world box can touch under the view-projection m (the draw path's viewport transform: pixel x =
// (clip x + 1) W / 2, pixel y = (1 - clip y) H / 2), NRST_MARGIN px outwards. 0 = not finite.
static inline int nrst_rect(const float *m, int conv, const float b[6], int32_t W, int32_t H, nr_ss_rect *r) {
    double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
    for (int q = 0; q < 8; q++) {
        const float p[3] = {(q & 1) ? b[3] : b[0], (q & 2) ? b[4] : b[1], (q & 4) ? b[5] : b[2]};
        double cx = nrst_m(m, conv, 3, 0), cy = nrst_m(m, conv, 3, 1);
        for (int i = 0; i < 3; i++) { cx += (double)p[i] * nrst_m(m, conv, i, 0); cy += (double)p[i] * nrst_m(m, conv, i, 1); }
        const double px = (cx + 1.0) * 0.5 * W, py = (1.0 - cy) * 0.5 * H;
        if (!(px == px && py == py)) return 0;
        if (px < x0) x0 = px; if (px > x1) x1 = px; if (py < y0) y0 = py; if (py > y1) y1 = py;
    }
    if (fabs(x0) > 1e8 || fabs(x1) > 1e8 || fabs(y0) > 1e8 || fabs(y1) > 1e8) return 0;
    r->x0 = (int32_t)floor(x0) - NRST_MARGIN; r->x1 = (int32_t)ceil(x1) + NRST_MARGIN; r->y0 = (int32_t)floor(y0) - NRST_MARGIN; r->y1 = (int32_t)ceil(y1) + NRST_MARGIN;
    return 1;
}
// The camera of now against the layer's: 0 = bit-equal; 1 = a whole-texel x / y shift (nx, ny) within shift_max; else -(NRST_R_* why).
// `still` mode (a = the frame before): 1 = everything but the x / y translation stood (a build may come).
static inline int nrst_cam_rel(const uint32_t a[NRST_CAMW], const uint32_t b[NRST_CAMW], int conv, int32_t W, int32_t H, double eps, uint32_t shift_max, int32_t *nx, int32_t *ny) {
    *nx = *ny = 0;
    if (!memcmp(a, b, NRST_CAMW * 4)) return 0;
    if (conv < 0) return -NRST_R_TURN;
    float va[16], vb[16]; memcpy(va, a + NRST_VP, 64); memcpy(vb, b + NRST_VP, 64);
    if (nrst_conv(vb) != conv) return -NRST_R_TURN;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) { const int i = nrst_idx(conv, r, c); if (a[NRST_VP + i] != b[NRST_VP + i] || a[NRST_VIEW + i] != b[NRST_VIEW + i]) return -NRST_R_TURN; }
    if (a[NRST_FAR] != b[NRST_FAR] || a[NRST_AUX] != b[NRST_AUX] || a[NRST_VP + nrst_idx(conv, 3, 3)] != b[NRST_VP + nrst_idx(conv, 3, 3)]) return -NRST_R_TURN;
    if (a[NRST_VP + nrst_idx(conv, 3, 2)] != b[NRST_VP + nrst_idx(conv, 3, 2)] || a[NRST_VIEW + nrst_idx(conv, 3, 2)] != b[NRST_VIEW + nrst_idx(conv, 3, 2)]) return -NRST_R_Z;
    double err = 0;
    const double dtx = (double)nrst_m(vb, conv, 3, 0) - (double)nrst_m(va, conv, 3, 0), dty = (double)nrst_m(vb, conv, 3, 1) - (double)nrst_m(va, conv, 3, 1);
    if (!nr_ss_shift(dtx, dty, W, H, eps, nx, ny, &err)) return -NRST_R_FRAC;
    if ((uint32_t)abs(*nx) > shift_max || (uint32_t)abs(*ny) > shift_max || abs(*nx) >= W / 2 || abs(*ny) >= H / 2) return -NRST_R_BIG;
    return 1;
}

static inline uint32_t nrst_hash_key(uint64_t k) { return (uint32_t)((k >> 3) * 0x9E3779B97F4A7C15ull >> 40); }
// The object's slot (inserted when new: *fresh = 1); -1 = no room (counted, the caller draws it as a dynamic caster).
static inline int nrst_find(nrst_cas *c, uint64_t key, int *fresh) {
    *fresh = 0;
    uint32_t i = nrst_hash_key(key) & (NRST_TAB - 1); int free_i = -1;
    for (int p = 0; p < NRST_PROBE; p++, i = (i + 1) & (NRST_TAB - 1)) {
        nrst_obj *e = &c->tab[i];
        if (e->key == key) return (int)i;
        // empty, or stale: not seen for reobs-like 600 frames and no member of the valid layer (its slot gets a new tag)
        if (free_i < 0 && (!e->key || (c->fr - e->seen > 600u && !(c->valid && e->mem == c->epoch)))) free_i = (int)i;
        if (!e->key) break;
    }
    if (free_i < 0) return -1;
    nrst_obj *e = &c->tab[free_i];
    memset(e, 0, sizeof *e); e->key = key; e->tag = ++c->tag_next; if (!e->tag) e->tag = ++c->tag_next;
    *fresh = 1;
    return free_i;
}

// One frame of the cascade. in[n]: the cull list; cam: the camera words of now; rect: the tile (atlas pixels); verdict(u, slot, tag):
// the backend's NRST_V_*; ack_epoch / ack_bad: the backend's last ack of this cascade. Out: grp[i] (NRST_G_*) and slot[i] / tag[i]
// (the OBJ markers' payload; slot -1 = none) for every caster, and the plan. Single-threaded per cascade.
typedef int (*nrst_verdict_fn)(void *u, uint32_t slot, uint32_t tag);
static inline void nrst_plan_frame(nrst_cas *c, const nrst_par *p, const nrst_in *in, uint32_t n, const uint32_t cam[NRST_CAMW], const int32_t rect[4],
                                   nrst_verdict_fn verdict, void *vu, uint32_t ack_epoch, int ack_bad, uint8_t *grp, int32_t *slot, uint32_t *tag, nrst_plan *out) {
    memset(out, 0, sizeof *out);
    const uint32_t fr = ++c->fr; c->st.frames++; c->st.objs += n;
    const int32_t W = rect[2], H = rect[3];
    int drop = NRST_R_NONE;
    if (!p->mode && c->valid) drop = NRST_R_OFF;
    if (c->valid && memcmp(rect, c->rect, sizeof c->rect)) drop = NRST_R_TILE;
    if (c->valid && ack_epoch == c->epoch && c->acked != c->epoch) {   // the backend answered for this layer
        c->acked = c->epoch;
        if (ack_bad) { c->good = 0; c->st.acks_bad++; if (!drop) drop = NRST_R_NACK; }
        else { c->good++; c->st.acks_good++; }
    }
    if (c->valid && !drop && c->acked != c->epoch && fr - c->built > p->noack) drop = NRST_R_NOACK;
    // the casters: their slots, words, verdicts
    uint32_t present = 0, cands = 0, newc = 0;
    int moved = 0;
    for (uint32_t i = 0; i < n; i++) {
        int fresh = 0; const int s = nrst_find(c, in[i].key, &fresh);
        slot[i] = s; tag[i] = 0; grp[i] = NRST_G_DYN;
        if (s < 0) { c->st.full++; continue; }
        nrst_obj *e = &c->tab[s];
        tag[i] = e->tag;
        if (fresh) { e->h = in[i].h; e->since = fr; memcpy(e->box, in[i].box, sizeof e->box); }
        else if (e->h != in[i].h) { e->h = in[i].h; e->since = fr; memcpy(e->box, in[i].box, sizeof e->box); if (c->valid && e->mem == c->epoch) moved = 1; }
        e->seen = fr;
        const int member = c->valid && e->mem == c->epoch;
        if (member && e->mseen != fr) { e->mseen = fr; present++; }
        const int v = verdict ? verdict(vu, (uint32_t)s, e->tag) : NRST_V_UNKNOWN;
        const int cand = fr - e->since >= p->stable && v == NRST_V_OK;
        // (scratch: bit 0 candidate, bit 1 to observe, bit 2 member)
        const int need = v == NRST_V_UNKNOWN || v == NRST_V_ONCE || (v == NRST_V_NOT && fr - e->obs >= p->reobs);
        grp[i] = (uint8_t)((cand ? 1 : 0) | (need ? 2 : 0) | (member ? 4 : 0));
        if (cand) { cands++; if (!member) newc++; }
    }
    // the camera
    int32_t nx = 0, ny = 0; int rel = 0;
    if (c->valid && !drop) {
        if (moved) drop = NRST_R_MOVED;
        else if ((rel = nrst_cam_rel(c->cam, cam, c->conv, W, H, p->eps, p->shift_max, &nx, &ny)) < 0) drop = -rel;
    }
    float vp[16]; memcpy(vp, cam + NRST_VP, 64);
    const int conv_now = nrst_conv(vp);
    if (c->valid && !drop && present < c->nmem) {   // members missing from the list: fine only when their box left the tile
        const nr_ss_rect tile = {0, 0, W, H};
        for (uint32_t i = 0; i < NRST_TAB && !drop; i++) {
            const nrst_obj *e = &c->tab[i];
            if (!e->key || e->mem != c->epoch || e->mseen == fr) continue;
            nr_ss_rect r;
            if (conv_now < 0 || !nrst_rect(vp, conv_now, e->box, W, H, &r) || nr_ss_isect(&r, &tile)) drop = NRST_R_GONE;
        }
    }
    if (c->valid && !drop && newc >= (p->refresh_min > c->nmem / 4 ? p->refresh_min : c->nmem / 4)) drop = NRST_R_REFRESH;
    if (c->valid && !drop && fr - c->built >= p->age) drop = NRST_R_AGE;
    if (drop && c->valid) { c->valid = 0; c->st.drop[drop]++; }
    out->drop = drop;
    int act = NRST_A_OFF;
    if (c->valid) {
        const int ok = c->acked == c->epoch || (p->trust && c->good >= 3);
        if (ok) { act = p->mode == 2 ? NRST_A_VERIFY : NRST_A_REUSE; if (c->acked != c->epoch) c->st.optimistic++; if (rel == 1) c->st.shifted++; else c->st.exact++; }
        else { act = NRST_A_HOLD; c->st.held++; }
    } else if (p->mode) {
        // a build: enough candidates and a camera that stood one frame (but for its x / y translation: a camera moving in z or turning
        // would drop the layer at once)
        int32_t sx, sy;
        const int still = c->have_prev && nrst_cam_rel(c->prev, cam, conv_now, W, H, p->eps, 1u << 30, &sx, &sy) >= 0;
        if (cands < p->min_static) out->nob = NRST_N_FEW;
        else if (!still) out->nob = NRST_N_MOVING;
        else {
            act = NRST_A_BUILD; out->nob = NRST_N_BUILT;
            c->valid = 1; c->epoch = ++c->epoch_next; if (!c->epoch) c->epoch = ++c->epoch_next;
            c->built = fr; c->nmem = 0; c->conv = conv_now; memcpy(c->cam, cam, sizeof c->cam); memcpy(c->rect, rect, sizeof c->rect);
            c->st.builds++;
        }
        c->st.nob[out->nob]++;
    } else { out->nob = NRST_N_OFF; c->st.nob[NRST_N_OFF]++; }
    c->st.act[act]++;
    out->act = act; out->epoch = c->valid ? c->epoch : 0; out->nx = nx; out->ny = ny;
    if (act == NRST_A_REUSE || act == NRST_A_VERIFY) out->ne = nr_ss_exposed(W, H, nx, ny, out->e);
    // the groups
    for (uint32_t i = 0; i < n; i++) {
        const int s = slot[i]; if (s < 0) { grp[i] = NRST_G_DYN; out->n[NRST_G_DYN]++; continue; }
        nrst_obj *e = &c->tab[s];
        const int cand = grp[i] & 1, need = (grp[i] & 2) != 0;
        int g = NRST_G_DYN;
        if (act == NRST_A_BUILD && cand) {
            g = NRST_G_STATIC; e->obs = fr;
            if (e->mem != c->epoch) { e->mem = c->epoch; e->mseen = fr; c->nmem++; }
        } else if ((act == NRST_A_REUSE || act == NRST_A_VERIFY) && e->mem == c->epoch) {
            g = NRST_G_LEFT;
            if (out->ne) {
                nr_ss_rect r;
                if (conv_now < 0 || !nrst_rect(vp, conv_now, e->box, W, H, &r) || nr_ss_touch(&r, out->e, out->ne)) g = NRST_G_TOUCH;
            }
        } else if (need) { g = NRST_G_OBS; e->obs = fr; }
        grp[i] = (uint8_t)g; out->n[g]++;
    }
    out->members = c->nmem; out->cands = cands;
    c->st.left += out->n[NRST_G_LEFT]; c->st.touch += out->n[NRST_G_TOUCH]; c->st.statics += out->n[NRST_G_STATIC]; c->st.obs += out->n[NRST_G_OBS];
    c->st.dyn += out->n[NRST_G_DYN]; c->st.members += c->valid ? c->nmem : 0;
    memcpy(c->prev, cam, sizeof c->prev); c->have_prev = 1;
}

#ifdef __cplusplus
}
#endif
#endif
