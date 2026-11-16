// [DOS2] NB-13b: the pipeline memo by state-group id (docs/dos2/NATIVE_BACKEND_PLAN.md, "NB-13b findings"). Pure C (C and C++, no
// Vulkan): used by the backend (dm/gfx/vulkan/engine/xbg_nb_pipe.inc, namespace nbpipe) and the host test
// dm/gfx/vulkan/engine/tests/nb_pipe_test.c.
//
// The pipeline memo of draw() (xbg_game.cpp PipeMemoKey, DM_GFX_DRAW_FAST / DM_GFX_DYN_STATE 0x2) keys a pipeline on ~700 raw bytes:
// every draw memsets + copies them (pipe_memo_fill), compares them with the previous draw's key and, when they differ, hashes and
// compares them again in the POD map. Those bytes are the five state groups the engine's state opcodes set (the NB-13a kinds:
// blend, colour, polygon, depth-stencil, vertex format; + a "misc" group of the DOOM-recorder states patch size / depth bounds /
// primitive restart / depth clamp / rasterizer discard) plus a dozen per-draw words (shaders, layout, pass, primitive, samples, ...).
// NB-13b gives each group a small id in the backend: the group's bytes, interned exactly (memcmp) in a per-group dictionary. A group
// is re-checked only after a record that writes it executed (OP_X_NB_STATE blocks run their records through execute(), so with the
// NB-13a shadow only the groups whose content changed are touched; plain records work the same). The memo key = 6 group ids + the
// per-draw words = 128 bytes; equal keys mean equal PipeMemoKey bytes (ids are exact), so a hit returns the pipeline the old memo
// returns for that state. A hit also returns the vertex binding list of that draw (a function of the vertex-format group and the
// vertex shader, both in the key), so the CreateInfo build, pipe_memo_fill, the POD lookup and the binding loop are all skipped.
// Why not the front end's block ids: they live one epoch (~ one command buffer) and, after a plain record of the kind, name only the
// shadow's part of the kind (NB-13a findings); the backend's content id is exact across epochs and across plain records.
#ifndef DM_NB_PIPE_H
#define DM_NB_PIPE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { NBP_G_BLEND, NBP_G_COLOR, NBP_G_POLY, NBP_G_DS, NBP_G_VFMT, NBP_G_MISC, NBP_GROUPS };   // 0..4 = NB-13a's NBS_K_*
enum { NBP_SPANS = 2, NBP_GROUP_MAX = 256,        // byte ranges of the state per group; largest group
       NBP_IDS = 1024, NBP_DHASH = 2048,          // ids per group dictionary (full -> it restarts, the memo is cleared)
       NBP_ENTRIES = 4096, NBP_SLOTS = 8192,      // memo entries (full -> cleared) and its open-addressing table
       NBP_KEY_WORDS = 16 };
typedef struct { uint16_t off[NBP_SPANS], len[NBP_SPANS]; } nbp_span;   // where a group's bytes lie in the state struct (len 0 = unused)

static inline uint64_t nbp_hash(const uint8_t *p, uint32_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h = (h ^ w) * 0xff51afd7ed558ccdull; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull; h ^= h >> 33;
    return h;
}

// ---- per-group dictionary: bytes -> id (exact) ----
typedef struct { uint64_t h; } nbp_did;
typedef struct { uint32_t size, nid; uint16_t tab[NBP_DHASH]; nbp_did id[NBP_IDS]; uint8_t *arena; } nbp_dict;   // arena: NBP_IDS x size
static inline void nbp_dict_reset(nbp_dict *d) { d->nid = 0; memset(d->tab, 0, sizeof d->tab); }
// The id of p[0..size): existing or new. -1 = full (the caller restarts the dictionary and clears the memo).
static inline int nbp_dict_get(nbp_dict *d, const uint8_t *p, int *fresh) {
    const uint64_t h = nbp_hash(p, d->size);
    uint32_t k = (uint32_t)(h >> 40) & (NBP_DHASH - 1);
    for (;; k = (k + 1) & (NBP_DHASH - 1)) {   // at most NBP_IDS of NBP_DHASH slots used: an empty slot is always found
        const uint16_t t = d->tab[k];
        if (!t) break;
        if (d->id[t - 1].h == h && !memcmp(d->arena + (size_t)(t - 1) * d->size, p, d->size)) { *fresh = 0; return t - 1; }
    }
    if (d->nid >= NBP_IDS) return -1;
    d->id[d->nid].h = h; memcpy(d->arena + (size_t)d->nid * d->size, p, d->size);
    d->tab[k] = (uint16_t)(++d->nid);
    *fresh = 1; return (int)d->nid - 1;
}

// ---- the groups of one state struct ----
enum { NBP_S_REFRESH, NBP_S_SAME, NBP_S_NEWID, NBP_S_DICT_RESET, NBP_S_N };
typedef struct {
    nbp_span sp[NBP_GROUPS];
    uint32_t dirty;                          // groups a record may have changed since their id was taken
    uint32_t valid;                          // groups that have an id (cur / gid meaningful)
    uint32_t gid[NBP_GROUPS];                // id of cur[g]
    uint8_t cur[NBP_GROUPS][NBP_GROUP_MAX];  // the bytes the id names (spans concatenated)
    nbp_dict d[NBP_GROUPS];
    uint64_t st[NBP_S_N];
} nbp_groups;
static inline uint32_t nbp_gsize(const nbp_span *s) { uint32_t n = 0; for (int i = 0; i < NBP_SPANS; i++) n += s->len[i]; return n; }
// 0 = ok, -1 = out of memory / a group larger than NBP_GROUP_MAX
static inline int nbp_groups_init(nbp_groups *g, const nbp_span sp[NBP_GROUPS]) {
    memset(g, 0, sizeof *g);
    for (int k = 0; k < NBP_GROUPS; k++) {
        g->sp[k] = sp[k]; g->d[k].size = nbp_gsize(&sp[k]);
        if (!g->d[k].size || g->d[k].size > NBP_GROUP_MAX) return -1;
        g->d[k].arena = (uint8_t *)malloc((size_t)NBP_IDS * g->d[k].size);
        if (!g->d[k].arena) return -1;
    }
    g->dirty = (1u << NBP_GROUPS) - 1;
    return 0;
}
static inline void nbp_groups_free(nbp_groups *g) { for (int k = 0; k < NBP_GROUPS; k++) { free(g->d[k].arena); g->d[k].arena = NULL; } }
static inline int nbp_group_same(const nbp_groups *g, int k, const uint8_t *base) {
    const nbp_span *s = &g->sp[k]; uint32_t at = 0;
    for (int i = 0; i < NBP_SPANS; i++) { if (memcmp(g->cur[k] + at, base + s->off[i], s->len[i])) return 0; at += s->len[i]; }
    return 1;
}
// Re-check the dirty groups against the state at base: a group whose bytes changed gets the id of its new bytes. Returns 1 when a
// dictionary restarted (ids of that group were reused: the caller clears the memo), else 0.
static inline int nbp_refresh(nbp_groups *g, const uint8_t *base) {
    int restarted = 0;
    for (uint32_t m = g->dirty; m; m &= m - 1) {
        const int k = __builtin_ctz(m);
        g->st[NBP_S_REFRESH]++;
        if ((g->valid >> k & 1) && nbp_group_same(g, k, base)) { g->st[NBP_S_SAME]++; continue; }
        const nbp_span *s = &g->sp[k]; uint32_t at = 0;
        for (int i = 0; i < NBP_SPANS; i++) { memcpy(g->cur[k] + at, base + s->off[i], s->len[i]); at += s->len[i]; }
        int fresh = 0, id = nbp_dict_get(&g->d[k], g->cur[k], &fresh);
        if (id < 0) { nbp_dict_reset(&g->d[k]); g->st[NBP_S_DICT_RESET]++; restarted = 1; id = nbp_dict_get(&g->d[k], g->cur[k], &fresh); }
        if (fresh) g->st[NBP_S_NEWID]++;
        g->gid[k] = (uint32_t)id; g->valid |= 1u << k;
    }
    g->dirty = 0;
    return restarted;
}
// The groups whose cur bytes do not equal the state (check mode: a writer that did not mark its group). Bit mask.
static inline uint32_t nbp_stale(const nbp_groups *g, const uint8_t *base) {
    uint32_t bad = 0;
    for (int k = 0; k < NBP_GROUPS; k++) if ((g->valid >> k & 1) && !(g->dirty >> k & 1) && !nbp_group_same(g, k, base)) bad |= 1u << k;
    return bad;
}

// ---- the memo: key -> pipeline + vertex bindings ----
typedef struct { uint64_t w[NBP_KEY_WORDS]; } nbp_key;   // 128 bytes, every byte set by the caller
typedef struct {
    nbp_key k;
    uint64_t pipe;                   // VkPipeline; 0 = dead (its pipeline was destroyed): a lookup misses, a store revives the entry
    uint32_t stride[16];             // vertex binding i: stride (binding_used bit i), input rate instance when inst bit i
    uint16_t used, inst;
    int8_t zero;                     // the zero (constant input) binding, -1 = none
    uint8_t _pad[3];
} nbp_ent;
enum { NBP_M_CLEARS, NBP_M_FULL, NBP_M_KILLED, NBP_M_N };
typedef struct { uint32_t n; uint16_t slot[NBP_SLOTS]; nbp_ent *e; uint64_t st[NBP_M_N]; } nbp_memo;
static inline uint64_t nbp_key_hash(const nbp_key *k) {
    uint64_t h = 0x84222325cbf29ce4ull;
    for (int i = 0; i < NBP_KEY_WORDS; i++) { h = (h ^ k->w[i]) * 0x100000001b3ull; h ^= h >> 31; }
    h ^= h >> 29; h *= 0xbf58476d1ce4e5b9ull; h ^= h >> 32;
    return h;
}
static inline int nbp_memo_init(nbp_memo *m) {
    memset(m, 0, sizeof *m);
    m->e = (nbp_ent *)calloc(NBP_ENTRIES, sizeof(nbp_ent));
    return m->e ? 0 : -1;
}
static inline void nbp_memo_free(nbp_memo *m) { free(m->e); m->e = NULL; }
static inline void nbp_memo_clear(nbp_memo *m) { if (m->n) { m->n = 0; memset(m->slot, 0, sizeof m->slot); } m->st[NBP_M_CLEARS]++; }
// The entry with this key (live or dead), NULL = none.
static inline nbp_ent *nbp_find(nbp_memo *m, const nbp_key *k, uint64_t h) {
    for (uint32_t s = (uint32_t)h & (NBP_SLOTS - 1);; s = (s + 1) & (NBP_SLOTS - 1)) {   // at most NBP_ENTRIES of NBP_SLOTS used
        const uint16_t t = m->slot[s];
        if (!t) return NULL;
        nbp_ent *e = &m->e[t - 1];
        if (!memcmp(&e->k, k, sizeof *k)) return e;
    }
}
// The entry for this key: the existing one (*fresh = 0, live or dead) or a new one (*fresh = 1, pipe 0). Full -> the memo is cleared
// first (*cleared = 1).
static inline nbp_ent *nbp_put(nbp_memo *m, const nbp_key *k, uint64_t h, int *cleared, int *fresh) {
    *cleared = 0; *fresh = 0;
    nbp_ent *e = nbp_find(m, k, h);
    if (e) return e;
    *fresh = 1;
    if (m->n >= NBP_ENTRIES) { nbp_memo_clear(m); m->st[NBP_M_FULL]++; *cleared = 1; }
    uint32_t s = (uint32_t)h & (NBP_SLOTS - 1);
    while (m->slot[s]) s = (s + 1) & (NBP_SLOTS - 1);
    e = &m->e[m->n]; memset(e, 0, sizeof *e); e->k = *k; e->zero = -1;
    m->slot[s] = (uint16_t)(++m->n);
    return e;
}
// A pipeline was destroyed (eviction): its entries go dead.
static inline void nbp_kill(nbp_memo *m, uint64_t pipe) {
    if (!pipe) return;
    for (uint32_t i = 0; i < m->n; i++) if (m->e[i].pipe == pipe) { m->e[i].pipe = 0; m->st[NBP_M_KILLED]++; }
}
#endif
