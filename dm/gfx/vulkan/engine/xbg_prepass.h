// [DOS2] DM_DOS2_PREPASS: the depth pre-pass ("FadePattern(depth)" in tools/dos2_gpu_breakdown.py, 526 draws / 2.8-3.9 ms GPU on the
// Adreno 750 in the 3D menu, docs/dos2/PREPASS.md, NR-30 docs/dos2/nr/NR-30.md). Pure C for C and C++ (no NVN / Vulkan types): used by
// xbg_game.cpp namespace prepass and by the host test dm/gfx/vulkan/engine/tests/prepass_test.c.
//
// Modes (DM_DOS2_PREPASS, read once; unset = the family default xbg_vendor_policy.h xbg_dos2_prepass_default: 1 on Mali / PowerVR, else 0):
//   0 = off: nothing changes, no line.
//   1 = hybrid skip: in a depth-only pass on the scene depth (no colour target, depth >= 640x360 and not square = not the shadow atlas) a
//       draw whose fragment stage is trivial (the opaque *_DEP_NX programs: an empty main, no kill / outputs / sample mask / storage
//       write), depth test + write on, stencil test off, alpha test off, no occlusion counting, indexed, is NOT drawn; its geometry key
//       (index buffer, range, base vertex, instances) is counted. In a later pass with colour targets on the same depth (the G-buffer),
//       up to that many draws with the same key, depth func EQUAL / LEQUAL, no real blending, no kill and the same raster state get
//       LEQUAL + depth write on (they now lay down the depth the pre-pass would have). Every other draw keeps the game's state: alpha-tested / fading draws (their DEP programs
//       kill) stay in the pre-pass and their G-buffer draws keep EQUAL against it (cut-outs and the fade dither stay exact).
//       NR-30 gates (each is a keep reason): the first pre-pass draw of the frame on that depth is always drawn (it begins the pass, so a
//       clear folded into its load op - DM_GFX_LAZY_CLEAR / CLEAR_FOLD / the NB-RP plan - still happens when every other draw is skipped);
//       a key already drawn in the pre-pass this frame is not skipped (MIXED: the claim order could not tell the two apart); and with the
//       history gate (DM_DOS2_PREPASS_HIST, default 1) a key is skipped only if in the previous frame it was a clean candidate: every
//       pre-pass draw of it skippable, none kept, and at least as many G-buffer draws claimed it, all with EQUAL / LEQUAL and no blending.
//       Geometry the G-buffer never draws (depth-only occluders = "unmatched"), keys whose G-buffer draw tests LESS / blends / kills /
//       rasterizes otherwise ("refused"), keys shared with a killing draw ("ambiguous") and keys still owed when a G-buffer draw that uses
//       stencil, depth bounds, a non-monotonic depth func, or a draw of another colour pass ran ("unsafe") are then never skipped. Claims
//       (and overrides) only happen in the first colour pass on that depth (the G-buffer), one per candidate. Only a frame in
//       which a key's role changes (a fade begins, an object leaves the G-buffer) can still be wrong, for that one frame - counted as bad,
//       see the fallback. Why the rest is exact: docs/dos2/nr/NR-30.md §2 (and the depth + colour model of prepass_test.c).
//       Depth-only draws after the first G-buffer draw on that depth in the frame are never skipped.
//   2 = compare: as 1 decides (history included), nothing is skipped or changed (picture as 0); at the end of each frame the decisions are
//       checked against the frame itself: a would-skip key that turned out unmatched / ambiguous / refused is bad (red line, first 16).
//   3 = poison: skip as 1, but no G-buffer override: every object mode 1 handles loses its depth and vanishes (EQUAL fails) - proves the
//       decisions reach the draws (SIM mali screenshots). Never a default.
// Fallback (mode 1): more than PREPASS_FB_BAD bad frames within PREPASS_FB_WINDOW frames -> mode 2 for the rest of the session (red line).
// The line (every DM_DOS2_PREPASS_LOG frames, default 300): pre-pass draws / skipped / kept by reason, their vertices; G-buffer draws on
// that depth by depth func, overrides, refusals, bad keys of the frame.
#ifndef XBG_PREPASS_H
#define XBG_PREPASS_H
#include <stdint.h>
#include <string.h>

enum { PREPASS_OFF = 0, PREPASS_SKIP = 1, PREPASS_COUNT = 2, PREPASS_POISON = 3, PREPASS_MODES = 4 };
enum { PREPASS_NVN_EQUAL = 3, PREPASS_NVN_LEQUAL = 4, PREPASS_NVN_ALWAYS_ALPHA = 8 };   // NVNdepthFunc EQUAL / LEQUAL; NVNalphaFunc ALWAYS
// why a pre-pass draw is kept (index into prepass_frame.keep). KILL .. FIRST are "nature" keeps (the draw is in the pre-pass on its own
// account: counted in the key's `kept`); MIXED / NEW are gate keeps of a candidate (counted in `held`, the key's verdict is not spoiled).
enum { PREPASS_K_KILL = 0, PREPASS_K_FS = 1, PREPASS_K_STATE = 2, PREPASS_K_STENCIL = 3, PREPASS_K_ALPHA = 4, PREPASS_K_COUNTING = 5,
       PREPASS_K_NOTINDEXED = 6, PREPASS_K_LATE = 7, PREPASS_K_FULL = 8, PREPASS_K_FIRST = 9, PREPASS_K_MIXED = 10, PREPASS_K_NEW = 11,
       PREPASS_K_NOID = 12, PREPASS_K_N = 13 };
// [eng2] NOID: the caller keys draws by instance (prepass_ctx.ident) and this draw has no instance identity (no per-object block): kept.
// why a G-buffer draw of a candidate key cannot carry the override: FUNC = no depth test, or neither EQUAL nor LEQUAL without write; BLEND = a colour target
// blends for real; KILL = the fragment stage may kill (discard, alpha test, alpha to coverage: its depth would not cover what the DEP draw
// wrote); RASTER = cull / front face / polygon mode / depth bias / clamp / viewport / scissor differ from the skipped pre-pass draw's
// STENCIL = it tests stencil (its stencil ops would follow LEQUAL instead of EQUAL) or depth bounds; LOST = it got the override but the
// host then did not record it (a later skip rule of draw(): missing bundle, bad viewport...) - prepass_note_lost
enum { PREPASS_R_FUNC = 0, PREPASS_R_BLEND = 1, PREPASS_R_KILL = 2, PREPASS_R_RASTER = 3, PREPASS_R_STENCIL = 4, PREPASS_R_LOST = 5, PREPASS_R_N = 6 };
// what made a G-buffer draw a hazard (below): stencil test, depth bounds, a depth func outside NEVER / LESS / EQUAL / LEQUAL, another
// colour pass, a blend (or a partial colour mask) together with a depth write; READ = the depth image was read (sampled, copied, storage)
enum { PREPASS_H_STENCIL = 0, PREPASS_H_BOUNDS = 1, PREPASS_H_FUNC = 2, PREPASS_H_PASS = 3, PREPASS_H_BLENDW = 4, PREPASS_H_READ = 5, PREPASS_H_N = 6 };
#define PREPASS_CAP 4096u          // geometry keys per frame (526 pre-pass draws in the 3D menu); a full table keeps the draw (never a false skip)
#define PREPASS_FB_WINDOW 600u     // fallback window (frames with a pre-pass)
#define PREPASS_FB_BAD 30u         // bad frames in a window that turn mode 1 into mode 2

static inline int prepass_clamp_mode(long v) { return v < 0 || v >= PREPASS_MODES ? PREPASS_OFF : (int)v; }

// The pass is the scene depth pre-pass candidate: no colour target, a depth target of at least 640x360 that is not square.
static inline int prepass_is_depth_only(int colors, int has_depth, uint32_t w, uint32_t h) {
    return !colors && has_depth && w != h && (uint64_t)w * h >= 640u * 360u;
}
// Facts of one pre-pass draw -> -1 = candidate, else the PREPASS_K_* reason it is kept.
// fs: 0 = no fragment stage, 1 = trivial (no kill / outputs / sample mask / storage writes), 2 = kills, 3 = other non-trivial.
static inline int prepass_classify(int fs, int dt, int dw, int st, int alpha_func, int counting, int indexed, int rast_discard) {
    if (fs == 2) return PREPASS_K_KILL;
    if (fs == 3) return PREPASS_K_FS;
    if (!dt || !dw || rast_discard) return PREPASS_K_STATE;
    if (st) return PREPASS_K_STENCIL;
    if (alpha_func != PREPASS_NVN_ALWAYS_ALPHA) return PREPASS_K_ALPHA;
    if (counting) return PREPASS_K_COUNTING;
    if (!indexed) return PREPASS_K_NOTINDEXED;
    return -1;
}
// NVN blend of one colour target that changes nothing against replacing: (ONE, ZERO, ADD) for colour and alpha (NVN ONE 2, ZERO 1, ADD 1).
static inline int prepass_blend_is_replace(unsigned src_rgb, unsigned dst_rgb, unsigned src_a, unsigned dst_a, unsigned eq_rgb, unsigned eq_a) {
    return src_rgb == 2u && dst_rgb == 1u && src_a == 2u && dst_a == 1u && eq_rgb == 1u && eq_a == 1u;
}
static inline uint64_t prepass_key(uint64_t ibuf, int32_t first, int32_t count, int32_t base_vertex, uint32_t instances, uint32_t base_instance) {
    uint64_t x = ibuf * 0x9E3779B97F4A7C15ull;
    x ^= ((uint64_t)(uint32_t)first << 32 | (uint32_t)count) * 0xC2B2AE3D27D4EB4Full; x ^= x >> 29;
    x ^= ((uint64_t)(uint32_t)base_vertex << 32 | instances) * 0x165667B19E3779F9ull; x ^= x >> 32;
    x ^= (uint64_t)base_instance * 0xD6E8FEB86659FD93ull; x ^= x >> 31;
    return x ? x : 1;   // 0 = empty slot
}

// cand: pre-pass draws of the key that were candidates (classify -1, not late / first); kept: its nature keeps; held: candidates the gates
// kept; skipped: candidates skipped (mode 2: would be); used: G-buffer overrides given (mode 2 / 3: would be); gb_ok / gb_bad: its G-buffer
// draws that can / cannot carry the override; vb0: vertex stream 0 of its first candidate (diagnostic: the G-buffer draw binds the same?)
// raster: the hash of the first candidate's raster state (RASTER above)
// claim_seq: the G-buffer draw number at which its last candidate was claimed (0 = not all claimed)
// adm: 1 = the key was admitted this frame (a first skip after a clean frame; PREPASS_K_NEW keeps the rest beyond the admission limit)
// [eng3] repaired: skipped draws the caller drew again itself (late, into the scene depth: prepass_note_repair) because no G-buffer draw
// claimed them; repair_ok: every repair came before anything on that depth could see the depth without them (no hazard, no read before)
// [eng4] rep_owed: the skips the repairs covered (skipped - used when repaired): a claim that comes after the repair (the G-buffer resumed after
// another pass on the depth) finds the depth whole - the override on it is harmless - so the key stays fixed
typedef struct { uint64_t key, vb0, raster; uint32_t cand, kept, held, skipped, used, gb_ok, gb_bad, claim_seq, adm, repaired, repair_ok, rep_owed; } prepass_slot;
// one pre-pass draw: key (0 = not indexed), vertex stream 0 address, raster hash, vertices, why = prepass_classify()
typedef struct { uint64_t key, vb0, raster, verts; int why; } prepass_pre_in;
// one draw with colour targets: key, vb0, raster as above; pass = the identity of its colour target set (a hash of the targets' serials:
// the G-buffer = the first set on this depth after the pre-pass); its depth test / func / write, stencil test, depth bounds test, real
// blending or a partial colour mask, may kill (prepass_note_gbuffer)
// [VK-ENGINE-SIDE I3] st_split = 1: st_dep says whether the draw's stencil RESULT depends on its depth test (depth test on, a stencil
// write mask, depth-fail op != depth-pass op). A stencil test whose result does not (the same op either way, or no write) writes the same
// stencil whatever depth the skipped draws left, and its colour / depth writes are repaired by the override like any EQUAL draw's: it is
// no hazard (it is still refused as a claimer: its depth write would follow its stencil test). st_split = 0: every stencil test is a
// hazard (the old rule; xbg_game.cpp leaves it 0).
typedef struct { uint64_t key, vb0, raster, pass; int dt, df, dw, st, bounds, blend, kill; int st_split, st_dep; } prepass_gb_in;
typedef struct {
    uint64_t frame, depth;   // depth: the scene depth target's serial (0 = none seen this frame)
    uint32_t n;              // keys in use
    // counters of this table
    uint32_t pre_draws, pre_skip, pre_cand, keep[PREPASS_K_N], occ_after_skip, admitted;
    uint32_t gb_draws, gb_df[16], gb_dw, gb_override, gb_refused[PREPASS_R_N], gb_vbdiff;
    // Hazards: a draw with colour targets on this depth that runs while candidates are still unclaimed (pre_cand > claims) and whose
    // result depends on the depth at that moment beyond what the claim order repairs: a stencil test (its ops follow the depth test), a
    // depth bounds test, a depth func outside NEVER / LESS / EQUAL / LEQUAL (writes that are not a min), a blend with a depth write (it
    // blends onto a colour a later override would have replaced, and its depth then stops that override), or any draw of another colour
    // pass (it runs between the claims). Everything else that runs early is repaired by the override at its place in the order. Only the first one matters: a key whose last claim comes after it (or never) is unsafe - not a
    // candidate for the next frame, and bad if skipped now. A key claimed before it saw the exact depth. claims: G-buffer draws that
    // claimed a candidate (at most cand per key) - the same count in every mode. gb_pass / gb_seq: the claiming pass and its draw counter.
    uint64_t gb_pass;
    uint32_t gb_seq, hazard_seq, hazard[PREPASS_H_N], claims, gb_other;
    uint64_t pre_verts, skip_verts, keep_verts, cand_verts;
    prepass_slot slot[PREPASS_CAP];
} prepass_frame;
// verdicts of the previous frame's candidate keys: bit 0 ok = may be skipped this frame; bit 1 est = it was skipped (or would have been,
// mode 2) in that frame too - an established key, not limited by the admission limit
enum { PREPASS_HIST_OK = 1, PREPASS_HIST_EST = 2 };
typedef struct { uint64_t key[PREPASS_CAP]; uint8_t ok[PREPASS_CAP]; uint32_t n; } prepass_hist;
// what a retired table showed (the line and the compare use it)
typedef struct {
    uint64_t frame; uint32_t keys, ok_keys, bad_keys, unmatched, ambiguous, refused, unsafe, unsafe_keys, vbdiff, hazard[PREPASS_H_N], gb_other;
    uint32_t repaired;               // [eng3] skipped draws drawn again late (no G-buffer draw claimed them; prepass_note_repair)
    uint32_t amb_same;               // [eng2] keys skipped with a second pre-pass draw or more claimers, under instance keys: the same instance (not bad)
    uint32_t pre_draws, pre_skip, pre_cand, keep[PREPASS_K_N], occ_after_skip, admitted, gb_draws, gb_df[16], gb_dw, gb_override, gb_refused[PREPASS_R_N];
    uint64_t pre_verts, skip_verts, keep_verts, cand_verts;
} prepass_result;
typedef struct {
    prepass_frame f;
    prepass_hist hist[2]; int cur;   // hist[cur]: verdicts of the previous frame; hist[cur ^ 1]: being built from this frame's tables
    int gate;                        // 1 = history gate on (DM_DOS2_PREPASS_HIST)
    // [eng2] 1 = the caller's keys name one INSTANCE (mesh range + the bytes of its per-object block: World / bones), not one mesh: two
    // draws with one key are the same geometry at the same place, so their depth is the same - whichever G-buffer draw of the key takes
    // the override lays down exactly the depth the skipped pre-pass draw would have (EQUAL of the others then passes where it did), and a
    // second pre-pass draw of the key (kept or skipped) or a second claimer does not make the frame wrong. 0 = keys name a mesh (the old
    // path): two instances of one mesh may share a key, the override may go to the wrong one: such a key is "ambiguous" = bad.
    int ident;
    uint32_t admit_limit;            // newly clean keys allowed to start skipping per frame (DM_DOS2_PREPASS_ADMIT; 0 = no limit). Bounds
                                     // the burst when an area appears: new LEQUAL + write pipeline variants to build, extra G-buffer overdraw
    prepass_result last;             // the last retired table with a pre-pass
    int last_valid;
    uint32_t fb_frames, fb_bad;      // fallback window
    uint32_t frame_bad, prev_frame_bad;   // bad keys of the tables of this / the previous frame
    uint64_t total_skipped, total_override, total_bad_frames, total_bad_keys, tables;
} prepass_ctx;

// (a frame that touched nothing - menus, loading - costs no memset of the 229 KB table / 37 KB history)
static inline void prepass_frame_reset(prepass_frame *f, uint64_t frame) {
    if (!f->n && !f->depth && !f->pre_draws && !f->gb_draws) { f->frame = frame; return; }
    memset(f, 0, sizeof *f); f->frame = frame;
}
static inline void prepass_hist_clear(prepass_hist *h) { if (h->n) memset(h, 0, sizeof *h); }
static inline void prepass_ctx_init(prepass_ctx *c, int gate) { memset(c, 0, sizeof *c); c->gate = gate ? 1 : 0; }

// slot of key; create: insert when absent (NULL when the table is full)
static inline prepass_slot *prepass_find(prepass_frame *f, uint64_t key, int create) {
    uint32_t i = (uint32_t)(key ^ key >> 32) & (PREPASS_CAP - 1u);
    for (uint32_t probe = 0; probe < PREPASS_CAP; probe++, i = (i + 1u) & (PREPASS_CAP - 1u)) {
        prepass_slot *s = &f->slot[i];
        if (s->key == key) return s;
        if (!s->key) {
            if (!create || f->n >= PREPASS_CAP * 3u / 4u) return 0;
            s->key = key; f->n++; return s;
        }
    }
    return 0;
}
// history: PREPASS_HIST_* bits of the key's verdict from last frame (0 = not a clean candidate)
static inline int prepass_hist_ok(const prepass_hist *h, uint64_t key) {
    uint32_t i = (uint32_t)(key ^ key >> 32) & (PREPASS_CAP - 1u);
    for (uint32_t probe = 0; probe < PREPASS_CAP; probe++, i = (i + 1u) & (PREPASS_CAP - 1u)) {
        if (h->key[i] == key) return h->ok[i];
        if (!h->key[i]) return 0;
    }
    return 0;
}
// merge a verdict (several tables in one frame: a bit stays only if every table sets it); a full history drops the verdict (= not ok)
static inline void prepass_hist_put(prepass_hist *h, uint64_t key, int bits) {
    uint32_t i = (uint32_t)(key ^ key >> 32) & (PREPASS_CAP - 1u);
    for (uint32_t probe = 0; probe < PREPASS_CAP; probe++, i = (i + 1u) & (PREPASS_CAP - 1u)) {
        if (h->key[i] == key) { h->ok[i] = (uint8_t)(h->ok[i] & bits); return; }
        if (!h->key[i]) {
            if (h->n >= PREPASS_CAP * 3u / 4u) return;
            h->key[i] = key; h->ok[i] = (uint8_t)(bits & 3); h->n++; return;
        }
    }
}

// The table ends (a new frame, or another scene depth within the frame): verdicts into the history being built, the check of this
// table's decisions. Returns the number of bad keys (skipped keys the table contradicts). Idempotent.
static inline uint32_t prepass_retire(prepass_ctx *c) {
    prepass_frame *f = &c->f;
    if (!f->depth || !f->pre_draws) return 0;
    prepass_result *r = &c->last; memset(r, 0, sizeof *r);
    r->frame = f->frame; r->pre_draws = f->pre_draws; r->pre_skip = f->pre_skip; r->pre_cand = f->pre_cand;
    memcpy(r->keep, f->keep, sizeof r->keep); r->occ_after_skip = f->occ_after_skip; r->admitted = f->admitted; r->gb_draws = f->gb_draws;
    memcpy(r->gb_df, f->gb_df, sizeof r->gb_df); r->gb_dw = f->gb_dw; r->gb_override = f->gb_override;
    memcpy(r->gb_refused, f->gb_refused, sizeof r->gb_refused); r->vbdiff = f->gb_vbdiff; memcpy(r->hazard, f->hazard, sizeof r->hazard);
    r->gb_other = f->gb_other;
    r->pre_verts = f->pre_verts; r->skip_verts = f->skip_verts; r->keep_verts = f->keep_verts; r->cand_verts = f->cand_verts;
    prepass_hist *next = &c->hist[c->cur ^ 1];
    for (uint32_t i = 0; i < PREPASS_CAP; i++) {
        const prepass_slot *s = &f->slot[i];
        if (!s->key) continue;
        r->keys++;
        // [eng3] the skips no claimer took, every one drawn again by the caller before a hazard / read: the depth is whole again
        const uint32_t owed = s->skipped > s->used ? s->skipped - s->used : 0;
        // [eng4] (phone H10 / H12: the G-buffer interrupted by ~32 draws of another colour pass on the depth, then resumed: the repair ran at
        // the first of them and covered every skip; the claims came after it, owed became 0 and the key was called unsafe - 32 bad keys a
        // frame although the frame was right) fixed = every skip unclaimed at the repair was drawn again before any hazard / read
        const int fixed = s->repaired && s->repair_ok && s->repaired >= s->rep_owed;
        const int hazard = !fixed && f->hazard_seq && (!s->claim_seq || s->claim_seq >= f->hazard_seq);   // owed when the first hazard ran (a hazard draw never claims)
        if (s->cand) {
            r->unsafe_keys += (uint32_t)hazard;
            const int ok = !hazard && !s->kept && !s->gb_bad && s->gb_ok == s->cand;   // one claimer per candidate (more: which owes what is unknown)
            r->ok_keys += (uint32_t)ok;
            prepass_hist_put(next, s->key, ok ? PREPASS_HIST_OK | (s->skipped ? PREPASS_HIST_EST : 0) : 0);
        }
        if (s->skipped) {
            const uint32_t un = owed > s->repaired ? owed - s->repaired : 0;
            r->repaired += fixed ? s->rep_owed : 0;
            const int extra = s->gb_ok > s->cand;   // more claimers than candidates: the override may have gone to the wrong draw
            const int amb = s->kept || extra;   // [eng2] under instance keys: the same instance twice (exact)
            r->unmatched += un; r->ambiguous += amb && !c->ident ? 1u : 0u; r->amb_same += amb && c->ident ? 1u : 0u;
            r->refused += s->gb_bad ? 1u : 0u; r->unsafe += hazard ? 1u : 0u;
            if (un || (amb && !c->ident) || s->gb_bad || hazard) r->bad_keys++;
        }
    }
    c->last_valid = 1; c->tables++;
    c->total_bad_keys += r->bad_keys; c->frame_bad += r->bad_keys;
    f->depth = 0; f->pre_draws = 0;   // retired: a second call (frame_begin after an explicit one) does nothing; later notes on that depth are ignored
    return r->bad_keys;
}
// Called before every draw with the frame number: a new frame retires the table and makes this frame's verdicts the history.
// Returns 1 when the ended frame had a pre-pass (c->last describes its last table, c->prev_frame_bad its bad keys).
static inline int prepass_frame_begin(prepass_ctx *c, uint64_t frame) {
    if (c->f.frame == frame) return 0;
    prepass_retire(c);
    const int had = c->tables != 0 && c->last_valid && c->last.frame == c->f.frame;
    c->prev_frame_bad = c->frame_bad; c->frame_bad = 0;
    c->cur ^= 1; prepass_hist_clear(&c->hist[c->cur ^ 1]);
    prepass_frame_reset(&c->f, frame);
    return had;
}
// A pre-pass draw (depth-only pass on depth serial `depth`): returns 1 = skip it (mode 1 / 3 skip, mode 2 counts it as would-skip).
// why = prepass_classify(); a new depth serial within the frame retires the table (only one scene pre-pass per table).
static inline int prepass_note_pre(prepass_ctx *c, uint64_t depth, const prepass_pre_in *in) {
    prepass_frame *f = &c->f;
    const uint64_t key = in->key, verts = in->verts; int why = in->why;
    if (f->depth != depth) { if (f->depth) prepass_retire(c); uint64_t fr = f->frame; prepass_frame_reset(f, fr); f->depth = depth; }
    const int first = f->pre_draws == 0;
    f->pre_draws++; f->pre_verts += verts;
    if (why < 0 && f->gb_draws) why = PREPASS_K_LATE;   // the G-buffer on this depth has begun: a later depth-only draw is not the pre-pass
    if (why < 0 && first) why = PREPASS_K_FIRST;        // it begins the pass (and its folded clear)
    prepass_slot *s = key ? prepass_find(f, key, 1) : 0;   // key 0: not indexed (never skipped)
    if (why < 0 && !s) why = PREPASS_K_FULL;
    if (why >= 0) {
        if (why == PREPASS_K_COUNTING && f->pre_skip) f->occ_after_skip++;   // NR-03: its query sees the depth without the skipped draws
        f->keep[why]++; f->keep_verts += verts; if (s) s->kept++; return 0;
    }
    f->pre_cand++; f->cand_verts += verts;
    if (!s->cand) { s->vb0 = in->vb0; s->raster = in->raster; }
    s->cand++;
    int gate = -1;
    if (s->kept) gate = PREPASS_K_MIXED;
    else if (c->gate) {
        const int h = prepass_hist_ok(&c->hist[c->cur], key);
        if (!(h & PREPASS_HIST_OK)) gate = PREPASS_K_NEW;
        else if (!(h & PREPASS_HIST_EST) && !s->adm) {   // clean last frame, not skipped then: a new admission
            if (c->admit_limit && f->admitted >= c->admit_limit) gate = PREPASS_K_NEW;
            else { s->adm = 1; f->admitted++; }
        }
    }
    if (gate >= 0) { f->keep[gate]++; f->keep_verts += verts; s->held++; return 0; }
    s->skipped++; f->pre_skip++; f->skip_verts += verts; c->total_skipped++; return 1;
}
// A draw with colour targets on depth serial `depth`: returns 1 = it is owed a skipped draw's depth (mode 1: give it depth func LEQUAL +
// depth write). The first claimers of a key get the override: a second G-buffer draw of the same geometry (a layer) then runs after it.
static inline int prepass_note_gbuffer(prepass_ctx *c, uint64_t depth, const prepass_gb_in *in) {
    prepass_frame *f = &c->f;
    if (!f->depth || f->depth != depth) return 0;
    f->gb_draws++; f->gb_df[in->df & 15]++; if (in->dw) f->gb_dw++;
    const uint64_t pass = in->pass ? in->pass : 1;
    if (!f->gb_pass) f->gb_pass = pass;
    const uint32_t seq = ++f->gb_seq;
    const int claim_pass = pass == f->gb_pass;
    if (!claim_pass) f->gb_other++;
    if (f->pre_cand > f->claims) {   // candidates still unclaimed: this draw sees the depth the skip changes
        int h = -1;
        if (!claim_pass) h = PREPASS_H_PASS;
        else if (in->st && (!in->st_split || in->st_dep)) h = PREPASS_H_STENCIL;
        else if (in->bounds) h = PREPASS_H_BOUNDS;
        else if (in->dt && !(in->df == 1 || in->df == 2 || in->df == PREPASS_NVN_EQUAL || in->df == PREPASS_NVN_LEQUAL)) h = PREPASS_H_FUNC;
        else if (in->dt && in->dw && in->blend) h = PREPASS_H_BLENDW;
        if (h >= 0) { f->hazard[h]++; if (!f->hazard_seq) f->hazard_seq = seq; }
    }
    if (!claim_pass || !in->key || !f->pre_cand) return 0;
    prepass_slot *s = prepass_find(f, in->key, 0);
    if (!s || !s->cand) return 0;
    int bad = -1;
    // EQUAL, or LEQUAL without a depth write: the forms a G-buffer draw of a pre-passed object takes (LEQUAL + write is how an object
    // that has no pre-pass draw shows itself - not ours to claim)
    if (!in->dt || !(in->df == PREPASS_NVN_EQUAL || (in->df == PREPASS_NVN_LEQUAL && !in->dw))) bad = PREPASS_R_FUNC;
    else if (in->blend) bad = PREPASS_R_BLEND;
    else if (in->kill) bad = PREPASS_R_KILL;
    else if (in->st || in->bounds) bad = PREPASS_R_STENCIL;
    else if (in->raster != s->raster) bad = PREPASS_R_RASTER;
    if (bad >= 0) { s->gb_bad++; f->gb_refused[bad]++; return 0; }
    s->gb_ok++; if (s->gb_ok <= s->cand) { f->claims++; if (s->gb_ok == s->cand) s->claim_seq = seq; }
    if (in->vb0 != s->vb0) f->gb_vbdiff++;
    if (s->used >= s->skipped) return 0;
    s->used++; f->gb_override++; c->total_override++; return 1;
}
// The scene depth (image serial) is read - sampled, copied, a storage view - outside the draws above: while candidates are unclaimed it
// sees the depth the skip changes (a hazard from the next G-buffer draw on; a read during the pre-pass makes every key unsafe).
static inline void prepass_note_read(prepass_ctx *c, uint64_t serial) {
    prepass_frame *f = &c->f;
    if (!f->depth || f->depth != serial || f->pre_cand <= f->claims) return;
    f->hazard[PREPASS_H_READ]++; if (!f->hazard_seq) f->hazard_seq = f->gb_seq + 1;
}
// [eng3] n skipped draws of key were drawn again by the caller (into the scene depth, the same draws); ok = before any hazard or read of
// that depth (else they still count as unmatched / unsafe). The key does not become clean (gb_ok < cand): the history keeps it out.
static inline void prepass_note_repair(prepass_ctx *c, uint64_t key, uint32_t n, int ok) {
    prepass_frame *f = &c->f;
    if (!f->depth) return;
    prepass_slot *s = key ? prepass_find(f, key, 0) : 0;
    if (!s) return;
    const uint32_t owed = s->skipped > s->used + s->repaired ? s->skipped - s->used - s->repaired : 0;   // [eng4] (before this repair)
    if (!s->repaired) s->rep_owed = 0;
    s->rep_owed += owed < n ? owed : n;
    s->repaired += n; s->repair_ok = (s->repaired == n ? 1u : s->repair_ok) & (ok ? 1u : 0u);
}
// A draw that was given the override (mode 1) was not recorded after all: its depth is missing this frame; the key is refused.
static inline void prepass_note_lost(prepass_ctx *c, uint64_t key) {
    prepass_frame *f = &c->f;
    if (!f->depth) return;   // (the table was retired: its frame is already judged)
    prepass_slot *s = key ? prepass_find(f, key, 0) : 0;
    f->gb_refused[PREPASS_R_LOST]++;
    if (s) s->gb_bad++;
}
// Mode 1 fallback, once per retired table with a pre-pass: 1 = too many bad frames in the window (turn mode 1 into mode 2).
static inline int prepass_fallback_note(prepass_ctx *c, uint32_t bad_keys) {
    c->fb_frames++; if (bad_keys) { c->fb_bad++; c->total_bad_frames++; }
    if (c->fb_bad > PREPASS_FB_BAD) return 1;
    if (c->fb_frames >= PREPASS_FB_WINDOW) { c->fb_frames = 0; c->fb_bad = 0; }
    return 0;
}
#endif
