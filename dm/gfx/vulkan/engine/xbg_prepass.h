// [DOS2] DM_DOS2_PREPASS: the depth pre-pass ("FadePattern(depth)" in tools/dos2_gpu_breakdown.py, 526 draws / 2.8-3.9 ms GPU on the
// Adreno 750 in the 3D menu, docs/dos2/PREPASS.md). Pure C for C and C++ (no NVN / Vulkan types): used by xbg_game.cpp namespace prepass and
// by the host test dm/gfx/vulkan/engine/tests/prepass_test.c.
//
// Modes (DM_DOS2_PREPASS, read once):
//   0 = off (default): nothing changes, no line.
//   1 = hybrid skip: in a depth-only pass on the scene depth (no colour target, depth >= 640x360 and not square = not the shadow atlas) a
//       draw whose fragment stage is trivial (the opaque *_DEP_NX programs: an empty main, no kill / outputs / sample mask / storage
//       write), depth test + write on, stencil test off, alpha test off, no occlusion counting, indexed, is NOT drawn; its geometry key
//       (index buffer, range, base vertex, instances) is counted. In a later pass with colour targets on the same depth (the G-buffer),
//       up to that many draws with the same key and depth func EQUAL / LEQUAL get LEQUAL + depth write on (they now lay down the depth the
//       pre-pass would have). Every other draw keeps the game's state: alpha-tested / fading draws (their DEP programs kill) stay in the
//       pre-pass and their G-buffer draws keep EQUAL against it (cut-outs and the fade dither stay exact). Depth-only draws after the
//       first G-buffer draw on that depth in the frame are never skipped.
//   2 = count only: as 1 decides, nothing is skipped or changed (picture as 0) + the same NVN PREPASS line.
// The line (every DM_DOS2_PREPASS_LOG frames, default 300): pre-pass draws / skippable / kept by reason, their vertices; G-buffer draws on
// that depth by depth func, overrides, skipped keys never matched (draws only the pre-pass had: their depth is then missing).
#ifndef XBG_PREPASS_H
#define XBG_PREPASS_H
#include <stdint.h>
#include <string.h>

enum { PREPASS_OFF = 0, PREPASS_SKIP = 1, PREPASS_COUNT = 2, PREPASS_MODES = 3 };
enum { PREPASS_NVN_EQUAL = 3, PREPASS_NVN_LEQUAL = 4, PREPASS_NVN_ALWAYS_ALPHA = 8 };   // NVNdepthFunc EQUAL / LEQUAL; NVNalphaFunc ALWAYS
// why a pre-pass draw is kept (index into prepass_frame.keep)
enum { PREPASS_K_KILL = 0, PREPASS_K_FS = 1, PREPASS_K_STATE = 2, PREPASS_K_STENCIL = 3, PREPASS_K_ALPHA = 4, PREPASS_K_COUNTING = 5,
       PREPASS_K_NOTINDEXED = 6, PREPASS_K_LATE = 7, PREPASS_K_FULL = 8, PREPASS_K_N = 9 };
#define PREPASS_CAP 4096u   // geometry keys per frame (526 pre-pass draws in the 3D menu); a full table keeps the draw (never a false skip)

static inline int prepass_clamp_mode(long v) { return v < 0 || v >= PREPASS_MODES ? PREPASS_OFF : (int)v; }

// The pass is the scene depth pre-pass candidate: no colour target, a depth target of at least 640x360 that is not square.
static inline int prepass_is_depth_only(int colors, int has_depth, uint32_t w, uint32_t h) {
    return !colors && has_depth && w != h && (uint64_t)w * h >= 640u * 360u;
}
// Facts of one pre-pass draw -> -1 = skippable, else the PREPASS_K_* reason it is kept.
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
static inline uint64_t prepass_key(uint64_t ibuf, int32_t first, int32_t count, int32_t base_vertex, uint32_t instances, uint32_t base_instance) {
    uint64_t x = ibuf * 0x9E3779B97F4A7C15ull;
    x ^= ((uint64_t)(uint32_t)first << 32 | (uint32_t)count) * 0xC2B2AE3D27D4EB4Full; x ^= x >> 29;
    x ^= ((uint64_t)(uint32_t)base_vertex << 32 | instances) * 0x165667B19E3779F9ull; x ^= x >> 32;
    x ^= (uint64_t)base_instance * 0xD6E8FEB86659FD93ull; x ^= x >> 31;
    return x ? x : 1;   // 0 = empty slot
}

typedef struct { uint64_t key; uint32_t skipped, kept, used; } prepass_slot;
typedef struct {
    uint64_t frame, depth;   // depth: the scene depth target's serial (0 = none seen this frame)
    uint32_t n;              // keys in use
    // counters of this frame
    uint32_t pre_draws, pre_skip, keep[PREPASS_K_N], gb_draws, gb_df[16], gb_dw, gb_override, gb_refused;
    uint64_t pre_verts, skip_verts, keep_verts;
    prepass_slot slot[PREPASS_CAP];
} prepass_frame;

static inline void prepass_reset(prepass_frame *f, uint64_t frame) { memset(f, 0, sizeof *f); f->frame = frame; }
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
// A pre-pass draw (depth-only pass on depth serial `depth`): returns 1 = skip it. why = prepass_classify(); a new depth serial
// within the frame restarts the table (only one scene pre-pass per frame is tracked).
static inline int prepass_note_pre(prepass_frame *f, uint64_t depth, uint64_t key, int why, uint64_t verts) {
    if (f->depth != depth) { uint64_t fr = f->frame; prepass_reset(f, fr); f->depth = depth; }
    f->pre_draws++; f->pre_verts += verts;
    if (why < 0 && f->gb_draws) why = PREPASS_K_LATE;   // the G-buffer on this depth has begun: a later depth-only draw is not the pre-pass
    prepass_slot *s = key ? prepass_find(f, key, 1) : 0;   // key 0: not indexed (never skipped)
    if (why < 0 && !s) why = PREPASS_K_FULL;
    if (why >= 0) { f->keep[why]++; f->keep_verts += verts; if (s) s->kept++; return 0; }
    s->skipped++; f->pre_skip++; f->skip_verts += verts; return 1;
}
// A draw with colour targets on depth serial `depth`: returns 1 = give it depth func LEQUAL + depth write (a skipped pre-pass draw of
// the same geometry is still owed its depth). dt / df: the draw's own depth test / func.
static inline int prepass_note_gbuffer(prepass_frame *f, uint64_t depth, uint64_t key, int dt, int df, int dw) {
    if (!f->depth || f->depth != depth) return 0;
    f->gb_draws++; f->gb_df[df & 15]++; if (dw) f->gb_dw++;
    if (!f->pre_skip || !key) return 0;
    prepass_slot *s = prepass_find(f, key, 0);
    if (!s || s->used >= s->skipped) return 0;
    if (!dt || (df != PREPASS_NVN_EQUAL && df != PREPASS_NVN_LEQUAL)) { f->gb_refused++; return 0; }
    s->used++; f->gb_override++; return 1;
}
// keys skipped more often than a G-buffer draw claimed them (pre-pass-only geometry: its depth is missing in mode 1); ambiguous: keys
// that were skipped and kept in the same frame (the claim order decides which draw gets LEQUAL)
static inline void prepass_tail(const prepass_frame *f, uint32_t *unmatched, uint32_t *ambiguous) {
    uint32_t u = 0, a = 0;
    for (uint32_t i = 0; i < PREPASS_CAP; i++) {
        const prepass_slot *s = &f->slot[i];
        if (!s->key) continue;
        if (s->skipped > s->used) u += s->skipped - s->used;
        if (s->skipped && s->kept) a++;
    }
    *unmatched = u; *ambiguous = a;
}
#endif
