// [dos2-nb] NB-07: "program -> texture / image slots its shaders sample, per NVN stage" (docs/dos2/NATIVE_BACKEND_PLAN.md NB-07).
// Pure C, inline, C and C++ (GCC / clang __atomic builtins on plain words): the backend publishes (xbg_nb.inc nbsm, from Shader::bindings
// when program_shaders resolves a program), the NVN tap reads on the game's render thread (nvn_nb_tap.inc: a draw records only the bound
// slots the bound program samples), and the host test tests/nb_slotmask_test.cpp drives both from several threads.
//
// Table: fixed size, open addressing (linear probe, at most NB_SM_PROBE slots), insert-only. Key = prog_t::serial (nvnProgramSetShaders
// gives every new shader set a new serial, never reused; 0 = no key). A key is claimed by CAS 0 -> key; the masks are guarded by a
// per-entry seqlock (odd = being written; a writer takes it by CAS even -> odd, so two writers of one key serialize; 0 = never written).
// A reader that sees no key, a never-written entry or a torn read (retries exhausted) gets "unknown": the caller stays conservative
// (every bound slot is a read). Readers never block and never write.
// Masks are by NVN stage (0 VS, 1 FS, 2 GS, 3 TCS, 4 TES, 5 CS = the stage index of BindTexture / BindImage), bit s = slot s (< 64).
// A stage the program has no shader for has mask 0 (the backend binds nothing there); NB_SM_ALL in a stage = "every slot" (the shader
// reads textures not by slot: UBO-addressed handles / pools), the same as unknown for that stage.
#ifndef DM_GFX_NB_SLOTMASK_H
#define DM_GFX_NB_SLOTMASK_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NB_SM_STAGES = 6, NB_SM_CAP = 8192, NB_SM_PROBE = 64, NB_SM_RETRY = 32 };
#define NB_SM_ALL (~(uint64_t)0)
typedef struct nb_sm_masks { uint64_t tex[NB_SM_STAGES], img[NB_SM_STAGES]; } nb_sm_masks;
typedef struct nb_sm_entry { uint64_t key; uint32_t seq, pad; uint64_t w[2 * NB_SM_STAGES]; } nb_sm_entry;   // w = tex[6], img[6]
typedef struct nb_sm_table {
    nb_sm_entry e[NB_SM_CAP];
    uint32_t n;        // keys inserted
    uint32_t full;     // publishes dropped: no free entry within NB_SM_PROBE (the program stays unknown = conservative)
    uint32_t writes;   // publishes done (a republish of a key counts again)
} nb_sm_table;

static inline uint32_t nb_sm_home(uint64_t key) { return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 40) & (NB_SM_CAP - 1); }

// Insert or overwrite key's masks. Any thread. 1 = published, 0 = key 0 or no room (counted in full).
static inline int nb_sm_publish(nb_sm_table *t, uint64_t key, const nb_sm_masks *m) {
    if (!key) return 0;
    nb_sm_entry *e = 0;
    for (uint32_t i = 0, h = nb_sm_home(key); i < NB_SM_PROBE; i++) {
        nb_sm_entry *c = &t->e[(h + i) & (NB_SM_CAP - 1)];
        uint64_t k = __atomic_load_n(&c->key, __ATOMIC_ACQUIRE);
        if (!k) {
            uint64_t z = 0;
            if (__atomic_compare_exchange_n(&c->key, &z, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { __atomic_fetch_add(&t->n, 1, __ATOMIC_RELAXED); e = c; break; }
            k = z;   // another writer claimed it first
        }
        if (k == key) { e = c; break; }
    }
    if (!e) { __atomic_fetch_add(&t->full, 1, __ATOMIC_RELAXED); return 0; }
    uint32_t s = __atomic_load_n(&e->seq, __ATOMIC_RELAXED);
    for (;;) {   // take the seqlock: even -> odd
        if (s & 1) { s = __atomic_load_n(&e->seq, __ATOMIC_RELAXED); continue; }
        if (__atomic_compare_exchange_n(&e->seq, &s, s + 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);   // the odd seq is visible before any word changes
    for (int k = 0; k < NB_SM_STAGES; k++) {
        __atomic_store_n(&e->w[k], m->tex[k], __ATOMIC_RELAXED);
        __atomic_store_n(&e->w[NB_SM_STAGES + k], m->img[k], __ATOMIC_RELAXED);
    }
    __atomic_store_n(&e->seq, s + 2, __ATOMIC_RELEASE);
    __atomic_fetch_add(&t->writes, 1, __ATOMIC_RELAXED);
    return 1;
}

// key's masks. Any thread, lock-free (bounded retries). 1 = found and consistent, 0 = unknown (out untouched).
static inline int nb_sm_lookup(const nb_sm_table *t, uint64_t key, nb_sm_masks *out) {
    if (!t || !key) return 0;
    for (uint32_t i = 0, h = nb_sm_home(key); i < NB_SM_PROBE; i++) {
        const nb_sm_entry *c = &t->e[(h + i) & (NB_SM_CAP - 1)];
        const uint64_t k = __atomic_load_n(&c->key, __ATOMIC_ACQUIRE);
        if (!k) return 0;   // end of the probe chain (insert-only: no tombstones)
        if (k != key) continue;
        for (int r = 0; r < NB_SM_RETRY; r++) {
            const uint32_t s1 = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
            if (!s1) return 0;        // claimed, masks not written yet
            if (s1 & 1) continue;     // a writer is inside
            uint64_t w[2 * NB_SM_STAGES];
            for (int j = 0; j < 2 * NB_SM_STAGES; j++) w[j] = __atomic_load_n(&c->w[j], __ATOMIC_RELAXED);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&c->seq, __ATOMIC_RELAXED) != s1) continue;
            for (int j = 0; j < NB_SM_STAGES; j++) { out->tex[j] = w[j]; out->img[j] = w[NB_SM_STAGES + j]; }
            return 1;
        }
        return 0;
    }
    return 0;
}

// The slots of one stage table to record as reads now: (all ? every slot : the rebound ones) & mask. Slots outside the mask stay pending
// in *dirty, so a later draw of this pass with a program that does sample them still records them without a rebind.
static inline uint64_t nb_sm_take(uint64_t *dirty, int all, uint64_t mask) {
    const uint64_t m = all ? NB_SM_ALL : *dirty;
    *dirty = m & ~mask;
    return m & mask;
}

#ifdef __cplusplus
}
#endif
#endif
