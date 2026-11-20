// [DOS2] NR-NATIVE shadows, DM_NRN_SHC (docs/dos2/nr/NR-NATIVE.md section 11): the TAPE of a cascade draw's structure hash. Pure C (C and
// C++): the backend dm/gfx/vulkan/engine/xbg_nr_shc.inc (hash_ser, generated from hash() by tools/dos2_shc_ser.py) and the host test
// dm/gfx/vulkan/engine/tests/nr_nrn_shc_test.c include it. Needs nr_shc.h (nrshc_mix, nrshc_bytes) included first.
//
// hash() of NR-SHCACHE folds about sixty words and small structures of the decoded State into one chain,
//     h = 0x53484331;  h = nrshc_mix(h, word)  |  h = nrshc_bytes(h, bytes, n)  ...
// and nearly all of them are the same for one engine draw from frame to frame. The tape is that chain not yet folded: an item per
// call (a word, or a byte string with its length), in call order. nrnt_end folds it exactly as the calls would have - the same value -
// unless the tape equals, byte for byte, the tape kept under the same key (the draw's NR-NATIVE card): then the kept hash is the
// answer and nothing is multiplied. Equal tapes are equal inputs in the same order, so the answer is exact by construction; the key
// only says where to look. A tape longer than the scratch is folded in parts and never kept.
#ifndef DM_NR_NRN_SHC_H
#define DM_NR_NRN_SHC_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The mutation switch of the host test (never defined in the engine build): NRNT_MUTATE = k - 1 a kept tape answers when only its
// length is equal, 2 unused (the key is not needed for exactness: an equal tape is an equal input), 3 the replay skips the byte strings, 4 a tape folded
// in parts is kept, 5 the replay starts from 0 instead of the seed, 6 a word item is folded as a byte string.
#ifndef NRNT_MUTATE
#define NRNT_MUTATE 0
#endif

enum { NRNT_SCRATCH = 8192, NRNT_ENT = 2048, NRNT_CAP = 2024, NRNT_BIG = 4096, NRNT_SEED = 0x53484331 };
enum { NRNT_C_EQUAL, NRNT_C_KEPT, NRNT_C_LONG, NRNT_C_NOCARD, NRNT_C_N };
typedef struct nrnt_ent { uint64_t key, hs; uint32_t n, pad; uint8_t b[NRNT_CAP]; } nrnt_ent;
typedef struct nrnt { uint8_t b[NRNT_SCRATCH]; uint32_t n; uint64_t h; int part; uint32_t turn; nrnt_ent *tab; uint64_t c[NRNT_C_N]; } nrnt;

static inline int nrnt_init(nrnt *t) { memset(t, 0, sizeof *t); t->tab = (nrnt_ent *)calloc(NRNT_ENT, sizeof *t->tab); return t->tab ? 0 : -1; }
static inline void nrnt_free(nrnt *t) { free(t->tab); t->tab = NULL; }
// Folds the items on the scratch into the running hash, as the calls of hash() would have.
static inline void nrnt_replay(nrnt *t) {
    const uint8_t *p = t->b, *e = t->b + t->n; uint64_t h = t->h;
    while (p < e) {
        if (*p == 0 && NRNT_MUTATE != 6) { uint64_t v; memcpy(&v, p + 1, 8); h = nrshc_mix(h, v); p += 9; }
        else if (*p == 0) { h = nrshc_bytes(h, p + 1, 8); p += 9; }
        else { uint16_t n; memcpy(&n, p + 1, 2); if (NRNT_MUTATE != 3) h = nrshc_bytes(h, p + 3, n); p += 3 + n; }
    }
    t->h = h; t->n = 0;
}
static inline void nrnt_begin(nrnt *t) { t->n = 0; t->h = NRNT_MUTATE == 5 ? 0 : (uint64_t)NRNT_SEED; t->part = 0; }
static inline void nrnt_word(nrnt *t, uint64_t v) {
    if (t->n + 9 > NRNT_SCRATCH) { nrnt_replay(t); t->part = 1; }
    t->b[t->n] = 0; memcpy(t->b + t->n + 1, &v, 8); t->n += 9;
}
static inline void nrnt_bytes(nrnt *t, const void *p, size_t n) {
    if (n > NRNT_BIG) { nrnt_replay(t); t->part = 1; t->h = nrshc_bytes(t->h, p, n); return; }
    if (t->n + 3 + n > NRNT_SCRATCH) { nrnt_replay(t); t->part = 1; }
    t->b[t->n] = 1; const uint16_t l = (uint16_t)n; memcpy(t->b + t->n + 1, &l, 2); memcpy(t->b + t->n + 3, p, n); t->n += 3 + (uint32_t)n;
}
// The structure hash of the tape. card: the key (0 = the draw has none: a key of the tape's first words).
static inline uint64_t nrnt_end(nrnt *t, uint64_t card) {
    if ((t->part && NRNT_MUTATE != 4) || t->n > NRNT_CAP || !t->tab) { nrnt_replay(t); t->c[NRNT_C_LONG]++; return t->h; }
    uint64_t key = card;
    if (!key) { t->c[NRNT_C_NOCARD]++; key = nrshc_bytes(0x4b, t->b, t->n < 45 ? t->n : 45) | 1; }
    // two ways: the slot of the key and its neighbour (a direct-mapped table lost a fifth of the answers to cards sharing a slot)
    const uint32_t s = (uint32_t)(key ^ key >> 32) & (NRNT_ENT - 1), n = t->n;
    nrnt_ent *e = &t->tab[s], *o = &t->tab[s ^ 1];
    if (e->key != key && o->key == key) { nrnt_ent *x = e; e = o; o = x; }
    if (e->key == key && e->n == n && (NRNT_MUTATE == 1 || !memcmp(e->b, t->b, n))) { t->c[NRNT_C_EQUAL]++; return e->hs; }
    if (e->key != key) { if (!o->key) e = o; else if (e->key && ((t->turn++) & 1)) e = o; }   // a new card: a free way, else the ways in turn
    nrnt_replay(t);
    e->key = key; e->n = n; memcpy(e->b, t->b, n); e->hs = t->h; t->c[NRNT_C_KEPT]++;
    return t->h;
}
#endif
