// [DOS2] NR-ZERO Z3 "cheap key" (DM_NR_Z3K, default 0; docs/dos2/nr/NR-ZERO.md section 17): the front table of nr_z3.h's table. Pure C for
// C and C++ (no Vulkan / engine / host type, no environment). The engine side is in dm/gfx/vulkan/engine/xbg_nr_z3.inc (namespace nrz3,
// "front"); the host test is dm/gfx/vulkan/engine/tests/nr_z3k_test.c.
//
// Why. The phone run of Z3 (X_p1, Mi 14 Ultra): "gates, key and lookup" cost 734 ns a draw - more than the three lookups it replaces
// (385 + 158). The time is not arithmetic: to ask nr_z3.h's table the translator first finds the shader set's plan (a slot + the plan),
// copies up to 39 words of state into a buffer through the plan's rows, hashes them, and compares them with a 448-byte entry - six to
// eight cache lines that are cold again every frame, per draw.
//
// What. The record of an engine draw already carries the game thread's identity of the draw (nrd_tail.key: the NR-02 key id the game
// thread computed when it wrote the record - shader, blend, depth-stencil, raster, vertex format handles, RT format set, primitive). The
// front table is indexed by THAT word and by a few words the translator has in hand without a walk; nothing is gathered and nothing is
// hashed word by word:
//   index     key id (from the record) + the variant words (var4, var5), the render pass definition, the fixed-words id, vertex / fragment
//             shader object + the texture handles of up to three rows the key id's shader set reads (nrzk_tab.kd: learned per key id
//             and fragment shader).
//             The index is only a place to look: a wrong or stale hint costs a miss, never an answer.
//   compare   the entry holds the FULL key of nr_z3.h (nrz_key's words) and the rows (stage, slot, sampler slot) its texture words were
//             read from; a hit compares every one of them with the live state IN PLACE: the two shader objects (the other three stages
//             are empty at every asking draw: a gate of nrz3::draw), the six fixed words (as one id: nrzk_fixed, exact interning by
//             comparison), every (texture handle, sampler handle) of the entry's rows against tex[stage][slot] / samp[stage][slot], the
//             key id, var4, var5, the pass definition. So a front hit is "the full key of this draw equals the full key the entry was
//             stored with" - the same statement nrz_find makes with memcmp, without the buffer.
//   rows      the rows are the plan's, a function of the shader set under NR-08's generation (plans are made once per shader set and
//             go only at NR-08's release, which moves NRZ_ST_DESC): while the stamps stand, the entry's rows are the rows nrdesc::gather
//             would read for the same two shader objects.
//   stamps    the three stamps of nr_z3.h. The front table is CLEARED when any of them is not the one it was filled under (nrzk_sync, at
//             every lookup and before every store): one comparison of three words a draw, no per-entry stamp.
//   answer    the nrz_val nrz_put stored for that key (the engine stores into the front table at the same two places it learns /
//             finds the full table's entry). An entry that the full table has since replaced by another key (its set of two ways was
//             full) still answers: its key and stamps are the ones it was stored with - eviction is not an invalidation.
// The typical entry (<= 2 texture rows: 2 lines; <= 10: 3 lines of 64 bytes) is read front to back.
#ifndef DM_NR_Z3K_H
#define DM_NR_Z3K_H
#include "nr_z3.h"
#ifdef __cplusplus
extern "C" {
#endif

// The mutation switch of the host test (nr_z3k_test.c builds itself once per value and must FAIL for each). NRZK_MUTATE = k: the
// in-place comparison leaves out 1 the key id, 2 the vertex shader, 3 the fragment shader, 4 var4, 5 var5, 6 the pass definition, 7 the
// fixed-words id, 8 the texture handles, 9 the sampler handles, 10 the last texture row; 11..13 nrzk_sync does not compare stamp 0..2;
// 14 the fixed-words memo does not compare word 0 (the viewport size), 15 the fixed-words table does not compare word 5; 16 a store
// keeps the other three shader stages out of sight (a key with a geometry stage is stored as if it had none). Never defined in the
// engine build (source_structure_test.py checks). (nrzk_put's own nrzk_sync is not a mutation: without it the entry would only be lost
// at the next lookup's nrzk_sync - stamps never come back.)
#ifndef NRZK_MUTATE
#define NRZK_MUTATE 0
#endif

enum { NRZK_WAYS = 2, NRZK_FIXED_MAX = 64, NRZK_HINT_ROWS = 3, NRZK_STAGES = 6, NRZK_SLOTS = 128, NRZK_NO_SAMP = 0xff };

typedef struct nrzk_row { uint8_t ns, slot, samp; } nrzk_row;   // a texture row of the plan: NVN stage, texture slot, sampler slot (NRZK_NO_SAMP: none)
typedef struct nrzk_ent {
    uint32_t key_id;                                    // 0 = empty (a record without a key id never asks)
    int32_t did;                                        // nrz_val.did
    uint64_t vs, fs, var4, var5, pass_def, plan, pent;  // line 0: the key's shader objects, variant words, pass definition; nrz_val.plan / .pent
    uint32_t fixed_id, tick;                            // line 1: nrzk_fixed's id of the six fixed words; last use
    uint8_t nimg, frag, _pad[2];
    nrzk_row row[NRZ_IMG_MAX];
    uint64_t th[2 * NRZ_IMG_MAX];                       // (texture handle, sampler handle) of each row
    uint64_t _align[2];
} nrzk_ent;                                             // 320 bytes = 5 lines
typedef struct nrzk_tab {
    nrzk_ent *e; void *raw; uint32_t sets, tick;        // sets * NRZK_WAYS entries, 64-byte aligned; sets is a power of two
    uint32_t *kd; uint32_t nkd;                         // per (key id, fragment shader), direct mapped: the rows whose texture handles go into the index (count | 3 x (stage * 128 + slot))
    uint64_t stamp[NRZ_STAMPS]; int stamped;
    uint64_t fixed[NRZK_FIXED_MAX][NRZ_KEY_FIXED]; uint32_t nfixed, last_fixed;   // the fixed-words sets seen since the last clear; id = index + 1
    uint64_t hits, misses, puts, replaced, clears, fixed_new, fixed_full;         // counters (the engine's 10 s window resets them)
} nrzk_tab;

// 0 ok, -1 no memory / bad size (entries: a power of two >= NRZK_WAYS; hints: slots of the hint table, a power of two).
static inline int nrzk_init(nrzk_tab *t, uint32_t entries, uint32_t hints) {
    memset(t, 0, sizeof *t);
    if (entries < NRZK_WAYS || (entries & (entries - 1)) || !hints || (hints & (hints - 1))) return -1;
    t->raw = calloc((size_t)entries * sizeof(nrzk_ent) + 64, 1);
    t->kd = (uint32_t *)calloc(hints, sizeof(uint32_t));
    if (!t->raw || !t->kd) { free(t->raw); free(t->kd); memset(t, 0, sizeof *t); return -1; }
    t->e = (nrzk_ent *)(((uintptr_t)t->raw + 63) & ~(uintptr_t)63);
    t->sets = entries / NRZK_WAYS; t->nkd = hints;
    return 0;
}
static inline void nrzk_free(nrzk_tab *t) { free(t->raw); free(t->kd); memset(t, 0, sizeof *t); }
// Every entry and every fixed-words id is gone (ids are handed out again: an entry never outlives the id table it was stored under).
static inline void nrzk_clear(nrzk_tab *t) {
    if (!t->e) return;
    memset(t->e, 0, (size_t)t->sets * NRZK_WAYS * sizeof(nrzk_ent));
    t->nfixed = 0; t->last_fixed = 0; t->clears++;
}
// The stamps of now against the ones the table was filled under: any other -> the table is cleared and filled under the new ones.
static inline void nrzk_sync(nrzk_tab *t, const uint64_t stamp[NRZ_STAMPS]) {
    int same = t->stamped;
    for (unsigned i = 0; i < NRZ_STAMPS && same; i++) if (NRZK_MUTATE != 11 + (int)i && t->stamp[i] != stamp[i]) same = 0;
    if (same) return;
    if (t->stamped) nrzk_clear(t);
    memcpy(t->stamp, stamp, sizeof t->stamp); t->stamped = 1;
}
// The id (1..) of the six fixed words: the last one asked is compared first (the words change at a pass change, not per draw); else the
// table of the sets seen since the last clear; a new set gets the next id. Exact: an id names one set of six words until the table is
// cleared. make = 0: 0 when the set is not known (a lookup never adds). A full table clears the front table (ids restart).
static inline uint32_t nrzk_fixed(nrzk_tab *t, const uint64_t f[NRZ_KEY_FIXED], int make) {
    if (t->last_fixed) { const uint64_t *l = t->fixed[t->last_fixed - 1]; int same = 1;
        for (unsigned i = 0; i < NRZ_KEY_FIXED && same; i++) if (!(NRZK_MUTATE == 14 && i == 0) && l[i] != f[i]) same = 0;
        if (same) return t->last_fixed; }
    for (uint32_t k = 0; k < t->nfixed; k++) { int same = 1;
        for (unsigned i = 0; i < NRZ_KEY_FIXED && same; i++) if (!(NRZK_MUTATE == 15 && i == 5) && t->fixed[k][i] != f[i]) same = 0;
        if (same) return t->last_fixed = k + 1; }
    if (!make) return 0;
    if (t->nfixed == NRZK_FIXED_MAX) { nrzk_clear(t); t->fixed_full++; }
    memcpy(t->fixed[t->nfixed], f, sizeof t->fixed[0]); t->fixed_new++;
    return t->last_fixed = ++t->nfixed;
}
static inline uint64_t nrzk_rotl(uint64_t x, unsigned r) { return x << r | x >> (64 - r); }
// The place to look. h3: the texture handles of the key id's hint rows (0 where the hint has fewer).
static inline uint32_t nrzk_index(const nrzk_tab *t, uint32_t key_id, uint64_t vs, uint64_t fs, uint64_t var4, uint64_t var5, uint64_t pass_def, uint32_t fixed_id, const uint64_t h3[NRZK_HINT_ROWS]) {
    uint64_t h = ((uint64_t)key_id << 32 | fixed_id) * 0x9E3779B97F4A7C15ull;
    h ^= (var4 ^ nrzk_rotl(var5, 23)) * 0xC2B2AE3D27D4EB4Full;
    h ^= (vs ^ nrzk_rotl(fs, 17) ^ nrzk_rotl(pass_def, 41)) * 0xFF51AFD7ED558CCDull;
    h ^= (h3[0] ^ nrzk_rotl(h3[1], 21) ^ nrzk_rotl(h3[2], 42)) * 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull;
    return (uint32_t)(h >> 32) & (t->sets - 1);
}
// The hint's slot: the key id names the engine's shader, but a twin of the fragment shader (NR-33B / NR-33C) may stand in for some of
// its draws - another plan, other rows - so the fragment shader object is part of the slot. (A slot two pairs share only costs misses.)
static inline uint32_t nrzk_kd_slot(const nrzk_tab *t, uint32_t key_id, uint64_t fs) { return (key_id * 0x9E3779B1u ^ (uint32_t)(fs >> 4) * 0x85EBCA6Bu) >> 7 & (t->nkd - 1); }
static inline void nrzk_hint(const nrzk_tab *t, uint32_t key_id, uint64_t fs, const uint64_t (*tex)[NRZK_SLOTS], uint64_t h3[NRZK_HINT_ROWS]) {
    const uint32_t kd = t->kd[nrzk_kd_slot(t, key_id, fs)], n = kd & 3;
    for (unsigned i = 0; i < NRZK_HINT_ROWS; i++) { const uint32_t r = kd >> (2 + 10 * i) & 1023; h3[i] = i < n ? tex[r >> 7][r & 127] : 0; }
}
// The entry whose full key is the live state's, or NULL. tex / samp: the backend's tables [stage][slot] (the ones nrdesc::gather reads).
// The caller has checked: key_id != 0, the stages between the vertex and the fragment shader are empty,
// nrzk_sync with the stamps of now, fixed_id = nrzk_fixed(.., 0) != 0.
static inline const nrzk_ent *nrzk_find(nrzk_tab *t, uint32_t key_id, uint64_t vs, uint64_t fs, uint64_t var4, uint64_t var5, uint64_t pass_def, uint32_t fixed_id,
                                        const uint64_t (*tex)[NRZK_SLOTS], const uint64_t (*samp)[NRZK_SLOTS]) {
    uint64_t h3[NRZK_HINT_ROWS];
    nrzk_hint(t, key_id, fs, tex, h3);
    nrzk_ent *s = &t->e[(size_t)nrzk_index(t, key_id, vs, fs, var4, var5, pass_def, fixed_id, h3) * NRZK_WAYS];
    for (unsigned w = 0; w < NRZK_WAYS; w++) {
        nrzk_ent *e = &s[w];
        if (!e->key_id) continue;
        if (NRZK_MUTATE != 1 && e->key_id != key_id) continue;
        if (NRZK_MUTATE != 2 && e->vs != vs) continue;
        if (NRZK_MUTATE != 3 && e->fs != fs) continue;
        if (NRZK_MUTATE != 4 && e->var4 != var4) continue;
        if (NRZK_MUTATE != 5 && e->var5 != var5) continue;
        if (NRZK_MUTATE != 6 && e->pass_def != pass_def) continue;
        if (NRZK_MUTATE != 7 && e->fixed_id != fixed_id) continue;
        const unsigned n = NRZK_MUTATE == 10 && e->nimg ? e->nimg - 1u : e->nimg;
        unsigned i = 0;
        for (; i < n; i++) { const nrzk_row r = e->row[i];
            if (NRZK_MUTATE != 8 && tex[r.ns][r.slot] != e->th[2 * i]) break;
            if (NRZK_MUTATE != 9 && (r.samp != NRZK_NO_SAMP ? samp[r.ns][r.samp] : 0) != e->th[2 * i + 1]) break; }
        if (i < n) continue;
        e->tick = ++t->tick; t->hits++;
        return e;
    }
    t->misses++;
    return NULL;
}
static inline void nrzk_val_of(const nrzk_ent *e, nrz_val *v) { memset(v, 0, sizeof *v); v->plan = e->plan; v->pent = e->pent; v->did = e->did; v->frag = e->frag; }
// Store (or replace) the resolution of a full key - key[n]: the words nrz_key wrote for a shader set of nimg texture rows; row[nimg]:
// the rows its texture words were read from (the plan's) - under the stamps of now. 1 = stored; 0 = not a key the front table holds
// (a stage between the vertex and the fragment shader, no key id, a row outside the tables).
static inline int nrzk_put(nrzk_tab *t, const uint64_t stamp[NRZ_STAMPS], const uint64_t *key, unsigned n, unsigned nimg, const nrzk_row *row, const nrz_val *v) {
    if (!t->e || nimg > NRZ_IMG_MAX || n != NRZ_KEY_SH + NRZ_KEY_FIXED + 2 * nimg + NRZ_KEY_TAIL) return 0;
    const uint64_t *th = key + NRZ_KEY_SH + NRZ_KEY_FIXED, *tail = th + 2 * nimg;
    const uint32_t key_id = (uint32_t)tail[0];
    if (!key_id || tail[0] >> 32) return 0;
    if (NRZK_MUTATE != 16 && (key[1] || key[2] || key[3])) return 0;
    for (unsigned i = 0; i < nimg; i++) if (row[i].ns >= NRZK_STAGES || row[i].slot >= NRZK_SLOTS || (row[i].samp != NRZK_NO_SAMP && row[i].samp >= NRZK_SLOTS)) return 0;
    nrzk_sync(t, stamp);
    const uint32_t fixed_id = nrzk_fixed(t, key + NRZ_KEY_SH, 1);
    uint32_t kd = nimg < NRZK_HINT_ROWS ? nimg : (uint32_t)NRZK_HINT_ROWS; uint64_t h3[NRZK_HINT_ROWS] = {0, 0, 0};
    for (unsigned i = 0; i < (kd & 3); i++) { kd |= ((uint32_t)row[i].ns << 7 | row[i].slot) << (2 + 10 * i); h3[i] = th[2 * i]; }
    t->kd[nrzk_kd_slot(t, key_id, key[4])] = kd;   // (another shader set in the same slot moves the hint: the entries stored under the old one are not found again, and replaced)
    nrzk_ent *s = &t->e[(size_t)nrzk_index(t, key_id, key[0], key[4], tail[1], tail[2], tail[3], fixed_id, h3) * NRZK_WAYS], *pick = NULL;
    for (unsigned w = 0; w < NRZK_WAYS && !pick; w++) { nrzk_ent *e = &s[w];
        if (e->key_id == key_id && e->vs == key[0] && e->fs == key[4] && e->var4 == tail[1] && e->var5 == tail[2] && e->pass_def == tail[3] && e->fixed_id == fixed_id && e->nimg == nimg &&
            !memcmp(e->row, row, nimg * sizeof(nrzk_row)) && !memcmp(e->th, th, (size_t)nimg * 16)) pick = e; }
    for (unsigned w = 0; w < NRZK_WAYS && !pick; w++) if (!s[w].key_id) pick = &s[w];
    if (!pick) { pick = &s[0]; for (unsigned w = 1; w < NRZK_WAYS; w++) if ((uint32_t)(t->tick - s[w].tick) > (uint32_t)(t->tick - pick->tick)) pick = &s[w]; t->replaced++; }
    memset(pick, 0, sizeof *pick);
    pick->key_id = key_id; pick->did = v->did; pick->vs = key[0]; pick->fs = key[4]; pick->var4 = tail[1]; pick->var5 = tail[2]; pick->pass_def = tail[3];
    pick->plan = v->plan; pick->pent = v->pent; pick->fixed_id = fixed_id; pick->tick = ++t->tick; pick->nimg = (uint8_t)nimg; pick->frag = v->frag;
    memcpy(pick->row, row, nimg * sizeof(nrzk_row)); memcpy(pick->th, th, (size_t)nimg * 16);
    t->puts++;
    return 1;
}

#ifdef __cplusplus
}
#endif
#endif
