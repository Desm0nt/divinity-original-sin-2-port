// [DOS2] NR-09U (DM_NR_UBO, docs/dos2/nr/NR-09U.md): the model of the engine's two uniform-block functions that every engine draw runs
// before its texture flush (draw functions 0x928e80 / 0x928fb0: `if ([r+0x17e0]) { 0x926f90(r); 0x925740(r); }`). Pure C (C and C++):
// the front end (dm/host/hle_dos2_nr_ubo.inc), the host test tests/nr_ubo_test.c and the Unicorn oracle tests/nr_ubo_oracle.py (which
// runs the game's own machine code of main.raw v786432 on the same states and compares every byte and every NVN call).
//
// What the machine code does (capstone of main.raw, offsets as the code uses them; r = renderer, dev = [r+0xb70], ctx = [dev+0x5248],
// cb = ctx + 0x38):
//   0x926f90 (global blocks)   r's own RW lock r+0xbe0 (owner thread r+0xbe4, -1 = none; the engine skips the lock when it owns it - the
//       model refuses then): READ lock (spin while >= 0x100000, then +1; a writer that came in: -1, again). n = bits 24..47 of [r+0xc00],
//       list = [r+0xbf8] (n pointers). For each block b of the list: if b.dirty (+0x34) or b.stamp (+0) != [dev+0x51f0]: REBUILD (below)
//       then BindUniformBuffer(cb, stage, slot +0x35, address +0x10, size +0x30) for the stages 0, 1 and 5 (no slot check). A block that
//       is not rebuilt is not bound. Unlock (-1).
//   0x925740 (shader blocks)   S = [r+0x17e0] (0: nothing). Three stages: arrays [S+0x20] / [S+0x28] / [S+0x30] of 0x40-byte blocks,
//       counts S+0x38 / +0x39 / +0x3a, NVN stages 0 / 1 / 5. Phase 1, stage 0 then 1 then 5, block 0 up: slot (+0x35) <= 7 and (dirty or
//       stamp != frame): REBUILD, then +0x2c = +0x28, +0x28 = 0. Phase 2, same order: a block with +0x18 != 0 and slot <= 7:
//       BindUniformBuffer(cb, stage, slot, +0x10, +0x30) - rebuilt or not (an unchanged block is bound again with its old range).
//   REBUILD(b): size = b+0x30 (u32). The device's ring lock dev+0x25aa8 as WRITER (spin while >= 0x100000 - `yield`, 400 spins, then
//       nn::os::YieldThread -, then +0x100000; another writer came in: -0x100000, again; then wait until the readers, bits 0..19, are 0 -
//       main+0x9156a0). h = ([dev+0x25a60] + 0xff) & ~0xff (32 bit); h + size > 0x137ffff -> h = 0; plain store head = h; old = atomic
//       fetch-add(head, size); unlock (-0x100000). b+0x10 = [dev+0x25aa0] + old (GPU), b+0x18 = [dev+0x25a98] + old (CPU); copy size bytes
//       from [b+0x20] (the block's shadow) to the CPU address (forward, 32-byte steps); b+0x34 = 0; stamp b+0 = [dev+0x51f0].
//
// The model does exactly that, in that order, with the same atomics on the same words (other threads that use the ring or r's lock - the
// game's own code - see the very protocol the engine's code speaks: native C atomics and the game's ldaxr / stlxr on one address are one
// lock). What it cannot represent it refuses BEFORE any store (the caller then runs the engine's two functions, untouched):
//   no device / no recording context / no ring base; r's lock owned by a thread (r+0xbe4 != -1: the engine's recursion path); more than
//   NRU_STAGE_MAX shader blocks a stage or NRU_GLOBAL_MAX global blocks; a null array / list / block pointer; a block of size over
//   NRU_SIZE_MAX or a null shadow with a size; a block, array, list or shadow that overlaps the ring, r's fields, the device's fields, the
//   shader record's header, or another block (identical global pointers are allowed - the second is clean by then, as in the engine).
//   The global list is read under r's read lock as the engine reads it: its checks run after the lock is taken, and a refusal there gives
//   the lock back the engine's way (+1 then -1: no other store, nothing for another thread to see but a reader that came and went).
// So execution never meets a case the model does not stand for: aliasing that the machine code would resolve by re-reading memory between
// its stores cannot occur, and the =2 simulation (copies of the blocks, no stores) predicts the very bytes.
//
// Calls: the model does not call NVN. It returns the BindUniformBuffer calls in the machine code's order with every argument as the engine
// would have passed it (read under the same lock, at the same point); the caller makes them through the body of the entry point. Every
// store the bodies do is to the command stream, none reads the blocks or the ring, so making them after the blocks' stores is the same.
#ifndef DM_NR_UBO_H
#define DM_NR_UBO_H
#include <stdint.h>
#include <string.h>

#ifndef NRU_PTR
#define NRU_PTR(a) ((uint8_t *)(uintptr_t)(a))   // guest address -> host pointer (identity in the HLE; the test maps its arenas)
#endif
#ifndef NRU_YIELD_THREAD                           // nn::os::YieldThread (the engine's PLT 0x1bf45a0) after 400 spins
#include <sched.h>
#define NRU_YIELD_THREAD() sched_yield()
#endif
#ifndef NRU_SPIN_HINT
#if defined(__aarch64__)
#define NRU_SPIN_HINT() __asm__ __volatile__("yield" ::: "memory")
#elif defined(__x86_64__) || defined(__i386__)
#define NRU_SPIN_HINT() __builtin_ia32_pause()
#else
#define NRU_SPIN_HINT() ((void)0)
#endif
#endif

enum {
    NRU_R_DEV = 0xb70, NRU_R_GLOCK = 0xbe0, NRU_R_GOWNER = 0xbe4, NRU_R_GLIST = 0xbf8, NRU_R_GCOUNT = 0xc00, NRU_R_SHADER = 0x17e0,
    NRU_R_LO = 0xb70, NRU_R_HI = 0x17e8,                        // the r fields the two functions read
    NRU_D_FRAME = 0x51f0, NRU_D_CTX = 0x5248, NRU_D_HEAD = 0x25a60, NRU_D_CPU = 0x25a98, NRU_D_GPU = 0x25aa0, NRU_D_LOCK = 0x25aa8,
    NRU_D_HI = 0x25aac,
    NRU_S_ARR = 0x20, NRU_S_CNT = 0x38, NRU_S_HDR_LO = 0x20, NRU_S_HDR_HI = 0x3b,
    NRU_B_STAMP = 0, NRU_B_GPU = 0x10, NRU_B_CPU = 0x18, NRU_B_SRC = 0x20, NRU_B_W28 = 0x28, NRU_B_W2C = 0x2c, NRU_B_SIZE = 0x30,
    NRU_B_DIRTY = 0x34, NRU_B_SLOT = 0x35, NRU_BLOCK = 0x40,
    NRU_CB_OFF = 0x38,                                          // cb = ctx + 0x38
    NRU_RING_LAST = 0x137ffff, NRU_RING = 0x1380000,
    NRU_LOCK_W = 0x100000, NRU_LOCK_R = 0xfffff, NRU_SPINS = 400,
    NRU_STAGE_MAX = 16, NRU_GLOBAL_MAX = 16, NRU_SIZE_MAX = 0x10000,
    NRU_CALLS_MAX = 3 * NRU_GLOBAL_MAX + 3 * NRU_STAGE_MAX,     // 96
    NRU_VISIT_MAX = NRU_GLOBAL_MAX + 3 * NRU_STAGE_MAX,         // 64 distinct blocks
    NRU_SLOT_BIND = 0x2bbfa80,                                  // main: the engine's nvnCommandBufferBindUniformBuffer pointer
    NRU_FN_GLOBAL = 0x926f90, NRU_FN_SHADER = 0x925740,
};
static const uint8_t k_nru_stage[3] __attribute__((unused)) = {0, 1, 5};
// Code the model stands for (FNV-1a 64 of main[off + 16, end): a hook may patch the first 16 bytes). The two waits are the engine's
// spin helpers the model's spins reproduce (0x9156a0: readers drain; 0x918050: a writer leaves).
typedef struct { uint32_t off, end; uint64_t fnv; const char *name; } nru_code;
#define NRU_CODE_N 4
#define NRU_CODE_TABLE { {0x925740, 0x926f90, 0x30c2d1dfec6566e5ull, "shader uniform blocks 0x925740"}, \
                         {0x926f90, 0x927310, 0x87f581eed44e14b2ull, "global uniform blocks 0x926f90"}, \
                         {0x9156a0, 0x9156f0, 0x286189ea764fb851ull, "ring lock: wait for the readers 0x9156a0"}, \
                         {0x918050, 0x9180a0, 0x2cc5f6f9a69c3b89ull, "lock: wait for the writer 0x918050"} }
static inline uint64_t nru_fnv(const uint8_t *p, size_t n) { uint64_t h = 0xcbf29ce484222325ull; for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull; return h; }

// Refusals (decided before any store but r's read lock given back).
enum { NRU_OK = 0, NRU_W_NODEV, NRU_W_NOCTX, NRU_W_NORING, NRU_W_OWNER, NRU_W_COUNT, NRU_W_NULL, NRU_W_SIZE, NRU_W_OVERLAP, NRU_WHY_N };
static const char *const k_nru_why[NRU_WHY_N] __attribute__((unused)) = {"ran", "no device", "no recording context", "no ring base", "r's lock owned by a thread",
                                                 "too many blocks", "null pointer", "block size", "overlap"};

typedef struct { uint64_t cb; uint32_t stage, slot; uint64_t addr; uint64_t size; } nru_call;   // BindUniformBuffer(cb, stage, slot, addr, size)
typedef struct { uint64_t dst, src; uint32_t n; uint32_t _pad; } nru_copy;                       // =2: what a rebuild copied
typedef struct {
    int why, ncall;
    nru_call call[NRU_CALLS_MAX];
    uint32_t reb_g, reb_s;            // blocks rebuilt (global / shader)
    uint32_t spin_w, spin_d, spin_r;  // contention: waits for a writer of the ring lock / its readers / a writer of r's lock (1 each)
    uint32_t yields;                  // nn::os::YieldThread calls (after 400 spins)
    // NR-UBD (DM_NR_UBD=2 with DM_NR_UBO=1, count only; same_on = 0 everywhere else): rebuilds of a block dirty in the same frame, and of
    // them the ones whose shadow equals the bytes of the block's previous ring range (only read, before the rebuild's own stores)
    int same_on; uint32_t reb_dirty, reb_same;
    // =2 (sim): copies of every block the run looks at (loads / stores go there), the head as it would end, what the rebuilds copy
    int sim, nvisit, ncopy;
    uint64_t vaddr[NRU_VISIT_MAX];
    uint8_t vblk[NRU_VISIT_MAX][NRU_BLOCK];
    uint32_t head, head_set;
    nru_copy copy[NRU_VISIT_MAX];
} nru_run;

static inline uint64_t nru_r64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint32_t nru_r32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void nru_w64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static inline void nru_w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
// [a, a + an) and [b, b + bn) share a byte (no wrap: the ranges are small and far below 2^64)
static inline int nru_ovl(uint64_t a, uint64_t an, uint64_t b, uint64_t bn) { return a < b + bn && b < a + an; }

// ---- the locks (exactly the machine code's protocol; sim: nothing) ----
static inline uint32_t nru_ldar(uint8_t *p) { return __atomic_load_n((uint32_t *)(void *)p, __ATOMIC_ACQUIRE); }
static inline void nru_spin(nru_run *s, uint32_t *n) { NRU_SPIN_HINT(); if (++*n >= NRU_SPINS) { NRU_YIELD_THREAD(); s->yields++; *n = 0; } }
static inline void nru_wlock(nru_run *s, uint8_t *lk) {          // the ring lock as writer (0x9258fc...)
    for (;;) {
        if (nru_ldar(lk) >= NRU_LOCK_W) { uint32_t n = 0; s->spin_w++; while (nru_ldar(lk) >= NRU_LOCK_W) nru_spin(s, &n); }
        const uint32_t old = __atomic_fetch_add((uint32_t *)(void *)lk, NRU_LOCK_W, __ATOMIC_ACQ_REL);
        if (old < NRU_LOCK_W) break;
        __atomic_fetch_sub((uint32_t *)(void *)lk, NRU_LOCK_W, __ATOMIC_ACQ_REL);
    }
    if (nru_ldar(lk) & NRU_LOCK_R) { uint32_t n = 0; s->spin_d++; while (nru_ldar(lk) & NRU_LOCK_R) nru_spin(s, &n); }   // 0x9156a0
}
static inline void nru_wunlock(uint8_t *lk) { __atomic_fetch_sub((uint32_t *)(void *)lk, NRU_LOCK_W, __ATOMIC_ACQ_REL); }
static inline void nru_rlock(nru_run *s, uint8_t *lk) {          // r's lock as reader (0x926fcc...)
    for (;;) {
        if (nru_ldar(lk) >= NRU_LOCK_W) { uint32_t n = 0; s->spin_r++; while (nru_ldar(lk) >= NRU_LOCK_W) nru_spin(s, &n); }   // 0x918050
        const uint32_t now = __atomic_add_fetch((uint32_t *)(void *)lk, 1, __ATOMIC_ACQ_REL);
        if (now < NRU_LOCK_W) break;
        __atomic_fetch_sub((uint32_t *)(void *)lk, 1, __ATOMIC_ACQ_REL);
    }
}
static inline void nru_runlock(uint8_t *lk) { __atomic_fetch_sub((uint32_t *)(void *)lk, 1, __ATOMIC_ACQ_REL); }

// ---- blocks: the real memory, or (sim) a copy made at the first look ----
static inline uint8_t *nru_blk(nru_run *s, uint64_t b) {
    if (!s->sim) return NRU_PTR(b);
    for (int i = 0; i < s->nvisit; i++) if (s->vaddr[i] == b) return s->vblk[i];
    const int i = s->nvisit < NRU_VISIT_MAX ? s->nvisit++ : NRU_VISIT_MAX - 1;   // the validation bounds the distinct blocks
    s->vaddr[i] = b; memcpy(s->vblk[i], NRU_PTR(b), NRU_BLOCK);
    return s->vblk[i];
}

// The machine code's copy (0x925c80...): 32-byte steps of two 16-byte load / store pairs, then 16, 8, 4, 2, 1 bytes, forward, each load
// before its store - so even a shadow that overlaps its destination copies as the engine copies it.
static inline void nru_copy_fwd(uint8_t *d, const uint8_t *s, uint32_t n) {
    uint64_t a, b;
    for (uint32_t k = n >> 5; k; k--) {
        memcpy(&a, s, 8); memcpy(&b, s + 8, 8); memcpy(d, &a, 8); memcpy(d + 8, &b, 8);
        memcpy(&a, s + 16, 8); memcpy(&b, s + 24, 8); memcpy(d + 16, &a, 8); memcpy(d + 24, &b, 8); s += 32; d += 32;
    }
    if (n & 16) { memcpy(&a, s, 8); memcpy(&b, s + 8, 8); memcpy(d, &a, 8); memcpy(d + 8, &b, 8); s += 16; d += 16; }
    if (n & 8) { memcpy(&a, s, 8); memcpy(d, &a, 8); s += 8; d += 8; }
    if (n & 4) { uint32_t w; memcpy(&w, s, 4); memcpy(d, &w, 4); s += 4; d += 4; }
    if (n & 2) { uint16_t h; memcpy(&h, s, 2); memcpy(d, &h, 2); s += 2; d += 2; }
    if (n & 1) *d = *s;
}

// REBUILD of block b (shader = 1: the 0x925740 tail +0x28 -> +0x2c). dev = the device; D its host pointer.
static inline void nru_same_count(nru_run *s, const uint8_t *B, const uint8_t *D);
static inline void nru_rebuild(nru_run *s, uint64_t b, const uint8_t *D0, int shader) {
    uint8_t *B = nru_blk(s, b), *D = (uint8_t *)(uintptr_t)D0;
    if (__builtin_expect(s->same_on, 0) && !s->sim) nru_same_count(s, B, D);   // NR-UBD=2: reads only
    const uint32_t size = nru_r32(B + NRU_B_SIZE);
    uint32_t old;
    if (!s->sim) {
        nru_wlock(s, D + NRU_D_LOCK);
        uint32_t h = (nru_r32(D + NRU_D_HEAD) + 0xffu) & 0xffffff00u;
        if ((uint64_t)h + size > NRU_RING_LAST) h = 0;
        __atomic_store_n((uint32_t *)(void *)(D + NRU_D_HEAD), h, __ATOMIC_RELAXED);
        old = __atomic_fetch_add((uint32_t *)(void *)(D + NRU_D_HEAD), size, __ATOMIC_ACQ_REL);
        nru_wunlock(D + NRU_D_LOCK);
    } else {
        if (!s->head_set) { s->head = nru_r32(D + NRU_D_HEAD); s->head_set = 1; }
        uint32_t h = (s->head + 0xffu) & 0xffffff00u;
        if ((uint64_t)h + size > NRU_RING_LAST) h = 0;
        old = h; s->head = h + size;
    }
    const uint64_t cpu = nru_r64(D + NRU_D_CPU) + old, gpu = nru_r64(D + NRU_D_GPU) + old;
    nru_w64(B + NRU_B_GPU, gpu); nru_w64(B + NRU_B_CPU, cpu);
    const uint32_t n = nru_r32(B + NRU_B_SIZE);
    const uint64_t src = nru_r64(B + NRU_B_SRC);
    if (!s->sim) nru_copy_fwd(NRU_PTR(cpu), NRU_PTR(src), n);
    else if (s->ncopy < NRU_VISIT_MAX) { nru_copy *c = &s->copy[s->ncopy++]; c->dst = cpu; c->src = src; c->n = n; c->_pad = 0; }
    if (shader) { const uint32_t w = nru_r32(B + NRU_B_W28); B[NRU_B_DIRTY] = 0; nru_w32(B + NRU_B_W28, 0); nru_w32(B + NRU_B_W2C, w); }
    else B[NRU_B_DIRTY] = 0;
    nru_w64(B + NRU_B_STAMP, nru_r64(D + NRU_D_FRAME));
    if (shader) s->reb_s++; else s->reb_g++;
}
// NR-UBD=2: a rebuild forced by the dirty byte inside the frame the block was last rebuilt in (stamp == frame), whose shadow equals the
// bytes at its previous CPU range (inside the ring) - the case where binding the previous range would give the GPU the same bytes. Count
// only: the model still copies (NR-UBD.md: why the copy is not left out).
static inline void nru_same_count(nru_run *s, const uint8_t *B, const uint8_t *D) {
    if (!B[NRU_B_DIRTY] || nru_r64(B + NRU_B_STAMP) != nru_r64(D + NRU_D_FRAME)) return;
    s->reb_dirty++;
    const uint32_t n = nru_r32(B + NRU_B_SIZE);
    const uint64_t prev = nru_r64(B + NRU_B_CPU), src = nru_r64(B + NRU_B_SRC), ring = nru_r64(D + NRU_D_CPU);
    if (!n || !src || prev < ring || prev - ring > (uint64_t)NRU_RING - n) return;
    if (!memcmp(NRU_PTR(prev), NRU_PTR(src), n)) s->reb_same++;
}
static inline void nru_put(nru_run *s, uint64_t cb, uint32_t stage, const uint8_t *B) {
    nru_call *c = &s->call[s->ncall++];   // bounded by the validation (NRU_CALLS_MAX)
    c->cb = cb; c->stage = stage; c->slot = B[NRU_B_SLOT]; c->addr = nru_r64(B + NRU_B_GPU); c->size = nru_r32(B + NRU_B_SIZE);
}

// Ranges no store of the run may touch (lo / n pairs).
typedef struct { uint64_t lo[12], n[12]; int k; } nru_ranges;
static inline int nru_hit(const nru_ranges *z, uint64_t a, uint64_t n) { for (int i = 0; i < z->k; i++) if (nru_ovl(a, n, z->lo[i], z->n[i])) return 1; return 0; }
// One engine draw's 0x926f90(r) + 0x925740(r). sim = 0: executes (stores, atomics, copies) and leaves the calls in s->call; sim = 1
// (DM_NR_UBO=2): stores nothing, takes no lock, predicts everything into s (blocks as they will be, the head, the copies, the calls).
// Returns 1 = ran (the caller makes s->call[0..ncall) through the body; the engine's two functions must NOT run), 0 = refused (s->why;
// nothing was stored - the caller runs the engine's functions).
// The checks. Execution re-reads every block field, list entry and device frame exactly where the machine code does (same order of loads
// and stores, the same copy), so it equals the engine for any contents of them. What it reads ONCE where the engine reads again - dev
// r+0xb70, S r+0x17e0, the owner r+0xbe4, ctx dev+0x5248, the shader record's counts / array pointers - and the lock / head words must
// not be hit by a store: the blocks that may be stored (shader blocks with slot <= 7, every global block) and the ring (the copies'
// destination) are kept off them, the arrays and global blocks off the ring (a copy over a block's size would change it) and off each
// other unless identical (one block's store over another's size / shadow). A size over NRU_SIZE_MAX or a null shadow with a size (the
// engine would read past its memory / fault) is refused. The simulation (=2) predicts from copies, so it also needs every shadow off the
// ring and the blocks, and the list off the blocks (it refuses more; a draw it refuses is not compared).
static inline int nru_draw(uint64_t r, nru_run *s, int sim) {
    s->why = NRU_OK; s->ncall = 0; s->reb_g = s->reb_s = 0; s->spin_w = s->spin_d = s->spin_r = s->yields = 0;
    s->reb_dirty = s->reb_same = 0;
    s->sim = sim; s->nvisit = 0; s->ncopy = 0; s->head = 0; s->head_set = 0;
    uint8_t *R = NRU_PTR(r);
    const uint64_t dev = nru_r64(R + NRU_R_DEV);
    if (!dev) { s->why = NRU_W_NODEV; return 0; }
    const uint8_t *D = NRU_PTR(dev);
    const uint64_t ctx = nru_r64(D + NRU_D_CTX), ring = nru_r64(D + NRU_D_CPU);
    if (!ctx) { s->why = NRU_W_NOCTX; return 0; }
    if (!ring) { s->why = NRU_W_NORING; return 0; }
    if (nru_r32(R + NRU_R_GOWNER) != 0xffffffffu) { s->why = NRU_W_OWNER; return 0; }
    const uint64_t cb = ctx + NRU_CB_OFF;
    nru_ranges z; z.k = 0;
#define NRU_Z(a_, n_) do { z.lo[z.k] = (a_); z.n[z.k] = (n_); z.k++; } while (0)
    NRU_Z(ring, NRU_RING); NRU_Z(r + NRU_R_DEV, 8); NRU_Z(r + NRU_R_GLOCK, 8); NRU_Z(r + NRU_R_SHADER, 8); NRU_Z(dev + NRU_D_CTX, 8);
    NRU_Z(dev + NRU_D_HEAD, NRU_D_HI - NRU_D_HEAD);
    if (sim) { NRU_Z(r + NRU_R_LO, NRU_R_HI - NRU_R_LO); NRU_Z(dev + NRU_D_FRAME, 8); }
    for (int i = 1; i < z.k; i++) if (nru_ovl(ring, NRU_RING, z.lo[i], z.n[i])) { s->why = NRU_W_OVERLAP; return 0; }
    const uint64_t S = nru_r64(R + NRU_R_SHADER);
    const uint8_t *SP = S ? NRU_PTR(S) : NULL;
    uint64_t arr[3] = {0, 0, 0}; unsigned cnt[3] = {0, 0, 0};
    if (S) {
        if (nru_hit(&z, S + NRU_S_HDR_LO, NRU_S_HDR_HI - NRU_S_HDR_LO)) { s->why = NRU_W_OVERLAP; return 0; }
        NRU_Z(S + NRU_S_HDR_LO, NRU_S_HDR_HI - NRU_S_HDR_LO);
        for (int st = 0; st < 3; st++) {
            cnt[st] = SP[NRU_S_CNT + st];
            if (!cnt[st]) continue;
            if (cnt[st] > NRU_STAGE_MAX) { s->why = NRU_W_COUNT; return 0; }
            arr[st] = nru_r64(SP + NRU_S_ARR + 8 * st);
            if (!arr[st]) { s->why = NRU_W_NULL; return 0; }
            if (nru_hit(&z, arr[st], (uint64_t)NRU_BLOCK * cnt[st])) { s->why = NRU_W_OVERLAP; return 0; }   // incl. the earlier arrays
            NRU_Z(arr[st], (uint64_t)NRU_BLOCK * cnt[st]);
        }
        for (int st = 0; st < 3; st++) for (unsigned i = 0; i < cnt[st]; i++) {   // shadows of every block that may rebuild
            const uint8_t *B = NRU_PTR(arr[st] + (uint64_t)NRU_BLOCK * i);
            if (B[NRU_B_SLOT] > 7) continue;
            const uint32_t n = nru_r32(B + NRU_B_SIZE);
            if (n > NRU_SIZE_MAX) { s->why = NRU_W_SIZE; return 0; }
            const uint64_t src = nru_r64(B + NRU_B_SRC);
            if (n && !src) { s->why = NRU_W_NULL; return 0; }
            if (sim && n && nru_hit(&z, src, n)) { s->why = NRU_W_OVERLAP; return 0; }
        }
    }
    // ---- 0x926f90: under r's read lock ----
    uint8_t *GL = R + NRU_R_GLOCK;
    if (!sim) nru_rlock(s, GL);
    const uint64_t ng = (nru_r64(R + NRU_R_GCOUNT) >> 24) & 0xffffff;
    uint64_t list = 0;
    if (ng) {
        int why = NRU_OK;
        list = nru_r64(R + NRU_R_GLIST);
        if (ng > NRU_GLOBAL_MAX) why = NRU_W_COUNT;
        else if (!list) why = NRU_W_NULL;
        else if (sim && nru_hit(&z, list, 8 * ng)) why = NRU_W_OVERLAP;
        for (uint64_t i = 0; i < ng && !why; i++) {
            const uint64_t b = nru_r64(NRU_PTR(list) + 8 * i);
            if (!b) { why = NRU_W_NULL; break; }
            const uint8_t *B = NRU_PTR(b);
            if (nru_hit(&z, b, NRU_BLOCK)) { why = NRU_W_OVERLAP; break; }
            for (uint64_t j = 0; j < i; j++) { const uint64_t c = nru_r64(NRU_PTR(list) + 8 * j); if (c != b && nru_ovl(b, NRU_BLOCK, c, NRU_BLOCK)) { why = NRU_W_OVERLAP; break; } }
            if (why) break;
            const uint32_t n = nru_r32(B + NRU_B_SIZE);
            const uint64_t src = nru_r64(B + NRU_B_SRC);
            if (n > NRU_SIZE_MAX) { why = NRU_W_SIZE; break; }
            if (n && !src) { why = NRU_W_NULL; break; }
            if (nru_ovl(b, NRU_BLOCK, list, 8 * ng)) { why = NRU_W_OVERLAP; break; }   // a store over the list
            if (!sim) continue;
            if (n && nru_hit(&z, src, n)) { why = NRU_W_OVERLAP; break; }
            for (uint64_t j = 0; j < ng; j++) if (n && nru_ovl(src, n, nru_r64(NRU_PTR(list) + 8 * j), NRU_BLOCK)) { why = NRU_W_OVERLAP; break; }   // a shadow inside a global block
        }
        if (sim && !why && S) for (int st = 0; st < 3; st++) for (unsigned i = 0; i < cnt[st] && !why; i++) {   // a shader shadow inside a global block
            const uint8_t *B = NRU_PTR(arr[st] + (uint64_t)NRU_BLOCK * i);
            const uint32_t n = nru_r32(B + NRU_B_SIZE);
            if (B[NRU_B_SLOT] > 7 || !n) continue;
            for (uint64_t j = 0; j < ng; j++) if (nru_ovl(nru_r64(B + NRU_B_SRC), n, nru_r64(NRU_PTR(list) + 8 * j), NRU_BLOCK)) { why = NRU_W_OVERLAP; break; }
        }
        if (why) { if (!sim) nru_runlock(GL); s->why = why; return 0; }
        for (uint64_t i = 0; i < ng; i++) {
            const uint64_t b = nru_r64(NRU_PTR(list) + 8 * i);
            const uint8_t *B = nru_blk(s, b);
            if (B[NRU_B_DIRTY] || nru_r64(B + NRU_B_STAMP) != nru_r64(D + NRU_D_FRAME)) {
                nru_rebuild(s, b, D, 0);
                for (int k = 0; k < 3; k++) nru_put(s, cb, k_nru_stage[k], B);
            }
        }
    }
    if (!sim) nru_runlock(GL);
    // ---- 0x925740 ----
    if (!S) return 1;
    for (int st = 0; st < 3; st++) for (unsigned i = 0; i < cnt[st]; i++) {   // phase 1
        const uint64_t b = arr[st] + (uint64_t)NRU_BLOCK * i;
        const uint8_t *B = nru_blk(s, b);
        if (B[NRU_B_SLOT] <= 7 && (B[NRU_B_DIRTY] || nru_r64(B + NRU_B_STAMP) != nru_r64(D + NRU_D_FRAME))) nru_rebuild(s, b, D, 1);
    }
    for (int st = 0; st < 3; st++) for (unsigned i = 0; i < cnt[st]; i++) {   // phase 2
        const uint8_t *B = nru_blk(s, arr[st] + (uint64_t)NRU_BLOCK * i);
        if (nru_r64(B + NRU_B_CPU) && B[NRU_B_SLOT] <= 7) nru_put(s, cb, k_nru_stage[st], B);
    }
#undef NRU_Z
    return 1;
}

// =2, after the engine's two functions ran on the state s predicted (nru_draw(..., 1) just before): 0 = the same; else a bit mask
// (1 blocks, 2 head, 4 ring bytes, 8 calls) and *at = the first differing block / copy / call index. raced = the head moved by more than
// the prediction (another thread allocated in between): the offsets cannot be compared, only what does not depend on them.
enum { NRU_BAD_BLOCK = 1, NRU_BAD_HEAD = 2, NRU_BAD_RING = 4, NRU_BAD_CALLS = 8 };
static inline int nru_check(const nru_run *s, uint64_t r, const nru_call *got, int ngot, int *at, int *raced) {
    const uint8_t *D = NRU_PTR(nru_r64(NRU_PTR(r) + NRU_R_DEV));
    int bad = 0; *at = -1;
    *raced = s->head_set && nru_r32(D + NRU_D_HEAD) != s->head;
    for (int i = 0; i < s->nvisit; i++) {
        const uint8_t *m = NRU_PTR(s->vaddr[i]), *e = s->vblk[i];
        int same = *raced ? !memcmp(m, e, NRU_B_GPU) && !memcmp(m + NRU_B_SRC, e + NRU_B_SRC, NRU_BLOCK - NRU_B_SRC) : !memcmp(m, e, NRU_BLOCK);
        if (!same) { if (!bad) *at = i; bad |= NRU_BAD_BLOCK; }
    }
    if (*raced) return bad;   // the head and the ring offsets belong to the other thread's allocation too
    for (int i = 0; i < s->ncopy; i++) if (s->copy[i].n && memcmp(NRU_PTR(s->copy[i].dst), NRU_PTR(s->copy[i].src), s->copy[i].n)) { if (!(bad & ~NRU_BAD_BLOCK)) *at = i; bad |= NRU_BAD_RING; }
    if (ngot != s->ncall) { bad |= NRU_BAD_CALLS; if (*at < 0) *at = ngot < s->ncall ? ngot : s->ncall; }
    else for (int i = 0; i < ngot; i++) {
        const nru_call *a = &got[i], *b = &s->call[i];
        if (a->cb != b->cb || a->stage != b->stage || a->slot != b->slot || a->addr != b->addr || a->size != b->size) { bad |= NRU_BAD_CALLS; if (*at < 0) *at = i; break; }
    }
    return bad;
}
#endif
