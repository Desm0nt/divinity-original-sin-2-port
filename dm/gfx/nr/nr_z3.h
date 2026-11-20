// [DOS2] NR-ZERO Z3 (DM_NR_Z3, default 0; docs/dos2/nr/NR-ZERO.md §9): "the same draw that is already resolved". Pure C for C and C++
// (no Vulkan / engine / host type, no environment): the key of a resolved engine draw, the table key -> resolution, the stamps that
// make an entry stale, and the classes of the DM_NR_Z3=2 comparison. The engine side is dm/gfx/vulkan/engine/xbg_nr_z3.inc (namespace
// nrz3); the host test is dm/gfx/vulkan/engine/tests/nr_z3_test.c.
//
// What is resolved. An engine draw (OP_X_NR_DRAW) that the short path serves goes through three lookups whose answers do not depend on
// the draw's data, only on the identity of what is bound:
//   the shader set's plan and its NR-08 entry   <- the five shader objects, NR-08's fixed words (viewport size, texture / sampler pool),
//                                                  the texture and sampler handles of the slots the shader set reads
//   the pipeline of NR-09's direct table        <- the record's key id, vertex / fragment shader, the descriptor layout's definition (a
//                                                  function of the plan), the render pass's definition, the variant bits (primitive,
//                                                  samples, targets, restart, depth format, the pre-pass override) and the six state ids
// The table of this file maps exactly those words to (plan, NR-08 entry id, NR-09 table entry). Everything that depends on the
// draw itself or on what only the translation thread knows at the moment of the draw - the uniform blocks' ring offsets and the
// validity of their in-place views, the read hazards of the entry's images, the entry's set and its recycler node, the open pass, the
// vertex / index views of the bound ranges, viewport / scissor, the bind cache - is NOT cached: the engine side computes it with the
// same code as the path without the table.
//
// Stale entries. Three counters of the engine make an answer stale without any of the key words changing (nrz_stamp):
//   NRZ_ST_DESC   NR-08's table generation: every flush of its entries (ids are handed out again), and its release
//   NRZ_ST_PIPE   NR-09's table generation: every clear (its entries are handed out again; an entry taught again or zeroed in place needs
//                 no stamp: the engine reads the entry itself)
//   NRZ_ST_OBJ    the backend's object epoch (nbpipe::epoch): state-group ids restarted; pipelines, layouts or render passes destroyed
// An entry answers only while all three are the ones it was stored with.
#ifndef DM_NR_Z3_H
#define DM_NR_Z3_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NRZ_IMG_MAX = 12,                                 // texture rows of a shader set the key holds (more: the draw is not cached)
       NRZ_KEY_SH = 5, NRZ_KEY_FIXED = 6, NRZ_KEY_TAIL = 4,
       NRZ_KEY_MAX = NRZ_KEY_SH + NRZ_KEY_FIXED + 2 * NRZ_IMG_MAX + NRZ_KEY_TAIL,   // 39 words
       NRZ_ST_DESC = 0, NRZ_ST_PIPE, NRZ_ST_OBJ, NRZ_STAMPS,
       NRZ_WAYS = 2 };

// The mutation switch of the host test (nr_z3_test.c builds itself once per value and must FAIL for each): NRZ_MUTATE = k leaves key
// part k out of the key (1 shader objects, 2 fixed words, 3 texture handles, 4 sampler handles, 5 key id, 6 variant bits, 7 state ids,
// 8 render pass definition), 9..11 = stamp 0..2 is not compared. Never defined in the engine build (source_structure_test.py checks).
#ifndef NRZ_MUTATE
#define NRZ_MUTATE 0
#endif

// ---- the key ----
// sh[5]: the backend's shader objects of the five stages (0 = none). fixed[6]: NR-08's fixed words. th[2 * nimg]: (texture handle,
// sampler handle or 0) of each texture row of the plan, in the plan's order. key_id: the record's NR-02 key id. var4 / var5: words 4 and
// 5 of NR-09's variant (nr_draw_var: primitive, samples, target count, colour mask, depth, restart, depth format, NR_DRAW_VAR_PP; the
// six state-group ids). pass_def: the render pass's pipeline definition. Returns the number of words (<= NRZ_KEY_MAX), 0 = too many
// texture rows.
static inline unsigned nrz_key(uint64_t *key, const uint64_t sh[NRZ_KEY_SH], const uint64_t fixed[NRZ_KEY_FIXED], const uint64_t *th, unsigned nimg,
                               uint32_t key_id, uint64_t var4, uint64_t var5, uint64_t pass_def) {
    unsigned n = 0;
    if (nimg > NRZ_IMG_MAX) return 0;
    for (unsigned i = 0; i < NRZ_KEY_SH; i++) key[n++] = NRZ_MUTATE == 1 ? 0 : sh[i];
    for (unsigned i = 0; i < NRZ_KEY_FIXED; i++) key[n++] = NRZ_MUTATE == 2 ? 0 : fixed[i];
    for (unsigned i = 0; i < nimg; i++) { key[n++] = NRZ_MUTATE == 3 ? 0 : th[2 * i]; key[n++] = NRZ_MUTATE == 4 ? 0 : th[2 * i + 1]; }
    key[n++] = NRZ_MUTATE == 5 ? 0 : (uint64_t)key_id;
    key[n++] = NRZ_MUTATE == 6 ? 0 : var4;
    key[n++] = NRZ_MUTATE == 7 ? 0 : var5;
    key[n++] = NRZ_MUTATE == 8 ? 0 : pass_def;
    return n;
}
static inline uint64_t nrz_hash(const uint64_t *key, unsigned n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ ((uint64_t)n << 56);
    for (unsigned i = 0; i < n; i++) { h = (h ^ key[i]) * 0xFF51AFD7ED558CCDull; h ^= h >> 29; }
    h *= 0xC4CEB9FE1A85EC53ull;
    return h ^ (h >> 32);
}

// ---- the resolution ----
typedef struct nrz_val {
    uint64_t plan;            // NR-08's plan of the shader set (an address; its layout names the pipeline layout)
    uint64_t pent;            // NR-09's table entry of (key id, variant) (an address: entries do not move until the table is cleared; its
                              // pipeline and vertex binding list are read from it at every draw - the old tail may teach them again, a
                              // destroyed pipeline is zeroed there)
    int32_t did;              // NR-08's entry id
    uint8_t frag;             // no stage before the fragment shader reads an image (descriptors(): the read barriers may be fragment-only)
    uint8_t _pad[3];
} nrz_val;
typedef struct nrz_ent {
    uint64_t h;               // 0 = empty (a stored hash is never 0)
    uint64_t stamp[NRZ_STAMPS];
    nrz_val v;
    uint32_t tick;            // last use (which of a set's two entries is replaced)
    uint8_t nkey, _pad[3];
    uint64_t key[NRZ_KEY_MAX];
} nrz_ent;
typedef struct nrz_tab {
    nrz_ent *e; uint32_t sets, tick;                      // sets * NRZ_WAYS entries; sets is a power of two
    uint64_t hits, misses, stale, puts, replaced;         // counters (the engine's 10 s window resets them)
} nrz_tab;
static inline int nrz_init(nrz_tab *t, uint32_t entries) {   // 0 ok, -1 no memory / bad size (entries: a power of two >= 2 * NRZ_WAYS)
    memset(t, 0, sizeof *t);
    if (entries < 2 * NRZ_WAYS || (entries & (entries - 1))) return -1;
    t->e = (nrz_ent *)calloc(entries, sizeof(nrz_ent));
    if (!t->e) return -1;
    t->sets = entries / NRZ_WAYS;
    return 0;
}
static inline void nrz_free(nrz_tab *t) { free(t->e); memset(t, 0, sizeof *t); }
static inline void nrz_clear(nrz_tab *t) { if (t->e) memset(t->e, 0, (size_t)t->sets * NRZ_WAYS * sizeof(nrz_ent)); }
static inline int nrz_stamp_same(const uint64_t a[NRZ_STAMPS], const uint64_t b[NRZ_STAMPS]) {
    for (unsigned i = 0; i < NRZ_STAMPS; i++) if (NRZ_MUTATE != 9 + (int)i && a[i] != b[i]) return 0;
    return 1;
}
// The resolution stored for the key, if every stamp is still the current one. NULL: no entry (*stale = 0) or a stale one (*stale = 1;
// it stays until nrz_put replaces it).
static inline const nrz_val *nrz_find(nrz_tab *t, uint64_t h, const uint64_t *key, unsigned n, const uint64_t stamp[NRZ_STAMPS], int *stale) {
    *stale = 0;
    if (!t->e || !n || n > NRZ_KEY_MAX) return NULL;
    if (!h) h = 1;
    nrz_ent *s = &t->e[(size_t)(h & (t->sets - 1)) * NRZ_WAYS];
    for (unsigned w = 0; w < NRZ_WAYS; w++) {
        nrz_ent *e = &s[w];
        if (e->h != h || e->nkey != n || memcmp(e->key, key, (size_t)n * 8)) continue;
        if (!nrz_stamp_same(e->stamp, stamp)) { *stale = 1; t->stale++; return NULL; }
        e->tick = ++t->tick; t->hits++;
        return &e->v;
    }
    t->misses++;
    return NULL;
}
// Store (or replace) the resolution of the key under the current stamps. The set's entry of the same key is replaced; else an empty
// one is taken; else the one used longest ago.
static inline void nrz_put(nrz_tab *t, uint64_t h, const uint64_t *key, unsigned n, const uint64_t stamp[NRZ_STAMPS], const nrz_val *v) {
    if (!t->e || !n || n > NRZ_KEY_MAX) return;
    if (!h) h = 1;
    nrz_ent *s = &t->e[(size_t)(h & (t->sets - 1)) * NRZ_WAYS], *pick = NULL;
    for (unsigned w = 0; w < NRZ_WAYS && !pick; w++) if (s[w].h == h && s[w].nkey == n && !memcmp(s[w].key, key, (size_t)n * 8)) pick = &s[w];
    for (unsigned w = 0; w < NRZ_WAYS && !pick; w++) if (!s[w].h) pick = &s[w];
    if (!pick) { pick = &s[0]; for (unsigned w = 1; w < NRZ_WAYS; w++) if ((uint32_t)(t->tick - s[w].tick) > (uint32_t)(t->tick - pick->tick)) pick = &s[w]; t->replaced++; }
    pick->h = h; pick->nkey = (uint8_t)n; memcpy(pick->key, key, (size_t)n * 8); memcpy(pick->stamp, stamp, sizeof pick->stamp);
    pick->v = *v; pick->tick = ++t->tick; t->puts++;
}
static inline int nrz_val_same(const nrz_val *a, const nrz_val *b) {
    return a->plan == b->plan && a->pent == b->pent && a->did == b->did && a->frag == b->frag;
}

// ---- why a draw with a record identity was not answered by the table (counted per reason) ----
enum { NRZW_SERVED = 0,
       NRZW_GATE,        // a diagnostic or a switch the short path does not run under (capture, pass graph, sort, multi-draw count, ...)
       NRZW_SPECIAL,     // GPU-indirect, quads, the UI offset, a UI draw, extra stages, no key id
       NRZW_NO_PLAN,     // the shader set has no NR-08 plan yet / is not eligible / backs off
       NRZW_LONG,        // more texture rows than the key holds
       NRZW_MISS,        // no entry for the key
       NRZW_STALE,       // an entry of another table generation
       NRZW_PIPE,        // NR-09's entry holds no pipeline now (destroyed: the old tail teaches it again)
       NRZW_DESC,        // NR-08's entry cannot answer now (remade, another ring buffer, an image replaced)
       NRZW_IMAGE,       // the read path of an image ended the submission (the draw is built again in the new one)
       NRZW_PASS,        // the draw does not continue the open pass with the same targets
       NRZW_BLOCK,       // a uniform block is not served by the ring now
       NRZW_SET,         // the entry's set was recycled (it is rewritten by the path without the table)
       NRZW_LATE,        // answered, but the vertex / index views, the viewport or the pass check sent the draw to the old tail
       NRZW_PASS_LATE,   // the descriptors answered, the read path of an image ended the open pass: create_pass and the lane ran
       NRZW_N };
static inline const char *nrz_why(int w) {
    static const char *const n[NRZW_N] = {"served", "diagnostics / switches", "indirect / quads / UI / stages", "no plan", "key too long", "no entry", "stale entry", "pipeline gone",
                                          "NR-08 entry not ready", "submission ended in an image read", "not the open pass", "uniform block", "set recycled", "old tail after the answer", "pass ended in an image read"};
    return w >= 0 && w < NRZW_N ? n[w] : "?";
}

// ---- DM_NR_Z3=2: the table's answer against what the path without the table did for the same draw ----
// predicted: the table answered and every check made before the first side effect passed. desc: how NR-08 answered (NRZD_*);
// images_read: images of the entry that took the read path (it may end the open pass); pass_fast: create_pass continued the open pass; lane: 0 the short path recorded the draw, 1 it failed before its pipeline was known
// (a gate, no state ids, a table miss), 2 it failed after (views, viewport, pass check: the same code runs after the table's answer).
enum { NRZD_NONE = 0, NRZD_HIT, NRZD_BUILT, NRZD_FELL_LIVE, NRZD_FELL_GATE };   // NR-08: not asked / an entry hit / made or remade / block or set / gate, not eligible
enum { NRZC_NOT_PREDICTED = 0,   // nothing to compare
       NRZC_SAME,                // the path without the table recorded the draw with the table's answer
       NRZC_LIVE,                // it left the short path at a step the table does not cache (block, set, views, viewport, pass check)
       NRZC_BAD };               // it resolved something else, or left at a step the table's checks cover: a missing key word / stamp / check
static inline int nrz_classify(int predicted, int desc, int images_read, int pass_fast, int lane, int same_values) {
    if (!predicted) return NRZC_NOT_PREDICTED;
    if (desc == NRZD_FELL_LIVE) return NRZC_LIVE;
    if (desc != NRZD_HIT) return NRZC_BAD;
    if (!pass_fast) return images_read ? NRZC_LIVE : NRZC_BAD;   // only the read path of an image may end the pass between the answer and create_pass
    if (lane == 1) return NRZC_BAD;
    if (lane == 2) return NRZC_LIVE;
    return same_values ? NRZC_SAME : NRZC_BAD;
}

#ifdef __cplusplus
}
#endif
#endif
