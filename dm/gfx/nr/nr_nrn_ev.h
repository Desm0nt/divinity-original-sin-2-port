// [DOS2] NR-NATIVE step E (docs/dos2/nr/NR-NATIVE-E.md): resources by ENGINE EVENTS instead of page tracking. Pure C (C and C++, no NVN /
// Vulkan / engine type, no environment): the host half dm/host/hle_dos2_nrn_ev.inc (the hooks: the writers on the engine's threads), the
// backend half dm/gfx/vulkan/engine/xbg_nr_nrn_ev.inc (the consumers on the translation thread) and the host tests
// dm/gfx/vulkan/engine/tests/nr_nrn_ev_test.c / nr_nrn_ev_tsan_test.cpp include it. Switch DM_NRN_EV (0 default, 2 verify, 1 apply).
//
// What the engine does to the bytes of a vertex / index buffer after it created it (main.raw of update v786432, read in the binary):
//   creation        0x917630 / 0x9179d0: nvnBufferInitialize over pool memory, then one memcpy of the initial data into the mapped pointer
//   GPU copy        0x9218c0 (r, update): nvnCommandBufferCopyBufferToBuffer from the staging ring into the buffer (every dynamic update:
//                   opcodes 0x2e / 0x31 fill the staging, 0x30 / 0x32 and nine direct callers flush) - the BACKEND writes the bytes when
//                   it executes the record
//   CPU copy        opcode 0x2f (0x9412f0) vertex buffer -> vertex buffer, opcode 0x33 (0x9414f0 / 0x921b10) index -> index: memcpy
//                   between the mapped pointers
//   scratch         the immediate-mode draws (opcodes 0x26..0x2a: 0x930910 / 0x930e90 / 0x92f730 / 0x93ff20 / 0x930210) write their
//                   vertices into the renderer's own buffer [r+0x8d0] (+ [r+0x8e0]) at a moving cursor: never covered here
//   destroy         0x91db40 / 0x91d890
// Nothing else in main indexes the two buffer tables (r+0x180 / r+0x240) and touches the mapped pointer. So a buffer's bytes change only
// at an EVENT one of those places can announce - and the page tracker (mprotect + write faults) is not needed to know it.
//
//   the journal (nrne_j)     one serial for every event; a page slot and a block slot per address hash hold the serial of the newest event
//                            that touched a page / block mapping to them (as nr_shc.h nrshc_wj, but any thread may write: the serial is a
//                            fetch-add, a slot only grows). The version of a range = the max over the slots it covers.
//   the coverage (nrne_cov)  which guest ranges ARE engine buffers, with their class: a sorted array behind a sequence word; one writer
//                            at a time (the host's mutex), readers never wait - a reader that meets a writer gets "not covered", which
//                            is always a safe answer (the caller keeps the page tracker for that ask).
//   the handles (nrne_h)     engine handle (generation bits included) -> the serial of its newest event; for the game thread, which
//                            holds handles and not addresses.
//   the ring (nrne_ring)     the constant ring's wrap count, from its head word.
// The small API for the other pieces is at the end: nrn_ev_range_epoch / nrn_ev_buf_epoch / nrn_ev_tex_epoch / nrn_ev_ring_epoch.
#ifndef DM_NR_NRN_EV_H
#define DM_NR_NRN_EV_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// The mutation switch of the host test (never defined in the engine build): NRNE_MUTATE = k - 1 an update event does not reach the
// journal, 2 a destroy leaves the range covered, 3 the range version skips the last page of the range, 4 a slot may shrink (a late
// writer overwrites a newer serial), 5 a reader that met a writer is answered from the half-moved array, 6 a range that only partly
// lies in a buffer counts as covered, 7 a re-created handle keeps the epoch of the dead one, 8 a scratch buffer counts as covered,
// 9 the ring's wrap is not counted, 10 a new buffer does not remove the dead entries it overlaps.
#ifndef NRNE_MUTATE
#define NRNE_MUTATE 0
#endif

#define NRNE_LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define NRNE_STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

// ---- the journal ----
enum { NRNE_PG = 1 << 16, NRNE_BK = 1 << 12, NRNE_BSH = 4 };   // page slots; block slots; pages a block = 1 << BSH (64 KB)
typedef struct nrne_j { uint64_t serial; uint64_t pg[NRNE_PG], bk[NRNE_BK]; } nrne_j;   // every word an atomic
static inline uint32_t nrne_ps(uint64_t page) { return (uint32_t)((page * 0x9E3779B97F4A7C15ull) >> 40) & (NRNE_PG - 1); }
static inline uint32_t nrne_bs(uint64_t blk) { return (uint32_t)((blk * 0xC2B2AE3D27D4EB4Full) >> 40) & (NRNE_BK - 1); }
static inline void nrne_max(uint64_t *slot, uint64_t s) {   // the slot only grows, whoever comes last
    uint64_t c = __atomic_load_n(slot, __ATOMIC_RELAXED);
    if (NRNE_MUTATE == 4) { __atomic_store_n(slot, s, __ATOMIC_RELEASE); return; }
    while (c < s && !__atomic_compare_exchange_n(slot, &c, s, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {}
}
// An event on the bytes [a, a + n). Returns its serial (> every serial before it). n == 0: a serial, no slot.
static inline uint64_t nrne_j_write(nrne_j *j, uint64_t a, uint64_t n) {
    const uint64_t s = __atomic_add_fetch(&j->serial, 1, __ATOMIC_ACQ_REL);
    if (!n) return s;
    const uint64_t p0 = a >> 12, p1 = (a + n - 1) >> 12, b0 = p0 >> NRNE_BSH, b1 = p1 >> NRNE_BSH;
    if (p1 - p0 >= NRNE_PG) { for (uint32_t i = 0; i < NRNE_PG; i++) nrne_max(&j->pg[i], s); }
    else for (uint64_t p = p0; p <= p1; p++) nrne_max(&j->pg[nrne_ps(p)], s);
    if (b1 - b0 >= NRNE_BK) { for (uint32_t i = 0; i < NRNE_BK; i++) nrne_max(&j->bk[i], s); }
    else for (uint64_t b = b0; b <= b1; b++) nrne_max(&j->bk[nrne_bs(b)], s);
    return s;
}
// The version of [a, a + n): the serial of the newest event that touched one of its pages (or one sharing a slot: a false change,
// never a missed one). An event W with serial s on the range raises the slot of every page and block it touches to >= s before it
// returns; a later ask reads at least one of them, so its answer is >= s > every answer given before W began.
static inline uint64_t nrne_j_ver(const nrne_j *j, uint64_t a, uint64_t n) {
    if (!n) return 0;
    const uint64_t p0 = a >> 12, p1 = ((a + n - 1) >> 12) - (NRNE_MUTATE == 3 && ((a + n - 1) >> 12) > (a >> 12) ? 1u : 0u), b0 = p0 >> NRNE_BSH, b1 = p1 >> NRNE_BSH;
    uint64_t v = 0;
    for (uint64_t b = b0; b <= b1; b++) {
        const uint64_t lo = b << NRNE_BSH, hi = lo + (1u << NRNE_BSH) - 1;
        if (p0 <= lo && hi <= p1) { const uint64_t x = NRNE_LOAD(((nrne_j *)j)->bk[nrne_bs(b)]); if (x > v) v = x; continue; }
        for (uint64_t p = p0 > lo ? p0 : lo, e = p1 < hi ? p1 : hi; p <= e; p++) { const uint64_t x = NRNE_LOAD(((nrne_j *)j)->pg[nrne_ps(p)]); if (x > v) v = x; }
    }
    return v;
}

// ---- the coverage ----
// kind: what the range is; cls: how the engine made it (nr_buf.h NRS_*: the caller maps); the flag SCRATCH: the engine writes it
// outside every event (the immediate-mode vertex buffer) - found, and never covered.
enum { NRNE_K_VB, NRNE_K_IB, NRNE_K_TEX, NRNE_K_N };
enum { NRNE_C_STATIC, NRNE_C_DYNAMIC, NRNE_C_NOINIT, NRNE_C_SCRATCH, NRNE_C_N };
static inline const char *nrne_cls_name(int kind, int cls) {
    static const char *const k[NRNE_C_N] = {"static", "dynamic", "no-init", "scratch"};
    (void)kind;
    return cls >= 0 && cls < NRNE_C_N ? k[cls] : "?";
}
enum { NRNE_CAP = 16384 };
typedef struct nrne_ent { uint64_t lo, hi, meta; } nrne_ent;   // [lo, hi); meta: handle (54 bits) | kind << 56 | cls << 58
typedef struct nrne_cov { uint64_t seq; uint64_t n; nrne_ent e[NRNE_CAP]; uint64_t full, adds, dels, evicted; } nrne_cov;   // seq odd: a writer is inside
static inline uint64_t nrne_meta(int kind, int cls, uint64_t handle) { return (handle & 0x003fffffffffffffull) | (uint64_t)kind << 56 | (uint64_t)cls << 58; }
static inline int nrne_meta_kind(uint64_t m) { return (int)(m >> 56 & 3); }
static inline int nrne_meta_cls(uint64_t m) { return (int)(m >> 58 & 3); }
static inline uint64_t nrne_meta_handle(uint64_t m) { return m & 0x003fffffffffffffull; }
// writer side (one writer at a time): index of the first entry with lo >= a
static inline uint32_t nrne_cov_lb(const nrne_cov *c, uint64_t a) {
    uint32_t l = 0, h = (uint32_t)c->n;
    while (l < h) { const uint32_t m = (l + h) / 2; if (c->e[m].lo < a) l = m + 1; else h = m; }
    return l;
}
static inline void nrne_cov_put(nrne_ent *d, const nrne_ent *s) { NRNE_STORE(d->lo, s->lo); NRNE_STORE(d->hi, s->hi); NRNE_STORE(d->meta, s->meta); }
// writer: remove the entries [i, k)
static inline void nrne_cov_cut(nrne_cov *c, uint32_t i, uint32_t k) {
    const uint32_t n = (uint32_t)c->n;
    for (uint32_t s = k, d = i; s < n; s++, d++) nrne_cov_put(&c->e[d], &c->e[s]);
    NRNE_STORE(c->n, (uint64_t)(n - (k - i)));
}
// writer: the range [lo, hi) is now this engine resource. Entries it overlaps are dead (their destroy was not seen, or the pool
// memory was handed out again): removed. 0 = the table is full - the range is simply not covered.
static inline int nrne_cov_add(nrne_cov *c, uint64_t lo, uint64_t hi, uint64_t meta) {
    if (hi <= lo) return 0;
    NRNE_STORE(c->seq, c->seq + 1);
    uint32_t i = nrne_cov_lb(c, lo);
    if (i > 0 && c->e[i - 1].hi > lo) i--;                      // the entry before overlaps
    uint32_t k = i;
    while (k < c->n && c->e[k].lo < hi && c->e[k].hi > lo) k++;
    if (NRNE_MUTATE == 10) k = i;
    if (k > i) { c->evicted += k - i; nrne_cov_cut(c, i, k); }
    int ok = 0;
    if (c->n < NRNE_CAP) {
        i = nrne_cov_lb(c, lo);
        for (uint32_t s = (uint32_t)c->n; s > i; s--) nrne_cov_put(&c->e[s], &c->e[s - 1]);
        const nrne_ent e = {lo, hi, meta};
        nrne_cov_put(&c->e[i], &e);
        NRNE_STORE(c->n, c->n + 1); c->adds++; ok = 1;
    } else c->full++;
    NRNE_STORE(c->seq, c->seq + 1);
    return ok;
}
// writer: the resource that began at lo is gone (only when it is still the one named by meta's handle and kind).
static inline int nrne_cov_del(nrne_cov *c, uint64_t lo, uint64_t meta) {
    if (NRNE_MUTATE == 2) return 0;
    const uint32_t i = nrne_cov_lb(c, lo);
    if (i >= c->n || c->e[i].lo != lo || ((c->e[i].meta ^ meta) & 0x03ffffffffffffffull)) return 0;
    NRNE_STORE(c->seq, c->seq + 1);
    nrne_cov_cut(c, i, i + 1); c->dels++;
    NRNE_STORE(c->seq, c->seq + 1);
    return 1;
}
// writer: the entry of `handle` wherever it begins (a linear walk: only when the handle table no longer knows the handle's range).
static inline int nrne_cov_del_handle(nrne_cov *c, uint64_t meta) {
    if (NRNE_MUTATE == 2) return 0;
    for (uint32_t i = 0; i < c->n; i++) if (!((c->e[i].meta ^ meta) & 0x03ffffffffffffffull)) {
        NRNE_STORE(c->seq, c->seq + 1);
        nrne_cov_cut(c, i, i + 1); c->dels++;
        NRNE_STORE(c->seq, c->seq + 1);
        return 1;
    }
    return 0;
}
// writer side: the range of the buffer `meta` names (handle and kind), by a walk.
static inline int nrne_cov_range_of(const nrne_cov *c, uint64_t meta, uint64_t *lo, uint64_t *size) {
    for (uint32_t i = 0; i < c->n; i++) if (!((c->e[i].meta ^ meta) & 0x03ffffffffffffffull)) { *lo = c->e[i].lo; *size = c->e[i].hi - c->e[i].lo; return 1; }
    return 0;
}
// writer: the class of the entry that begins at lo (a buffer found to be the immediate-mode scratch).
static inline int nrne_cov_class(nrne_cov *c, uint64_t lo, int cls) {
    const uint32_t i = nrne_cov_lb(c, lo);
    if (i >= c->n || c->e[i].lo != lo) return 0;
    const uint64_t m = c->e[i].meta;
    if (nrne_meta_cls(m) == cls) return 1;
    NRNE_STORE(c->seq, c->seq + 1);
    NRNE_STORE(c->e[i].meta, (m & ~(3ull << 58)) | (uint64_t)cls << 58);
    NRNE_STORE(c->seq, c->seq + 1);
    return 1;
}
// reader (any thread, never waits): 1 = [a, a + n) lies wholly inside one engine resource, *out = its entry. 0 = it does not, or a
// writer was inside (the caller treats both as "not covered": it keeps what it did without the events).
static inline int nrne_cov_find(const nrne_cov *cc, uint64_t a, uint64_t n, nrne_ent *out) {
    nrne_cov *c = (nrne_cov *)cc;
    const uint64_t s1 = NRNE_LOAD(c->seq);
    if ((s1 & 1) && NRNE_MUTATE != 5) return 0;
    uint64_t l = 0, h = NRNE_LOAD(c->n);
    if (h > NRNE_CAP) return 0;
    while (l < h) { const uint64_t m = (l + h) / 2; if (NRNE_LOAD(c->e[m].lo) <= a) l = m + 1; else h = m; }
    if (!l) return 0;
    nrne_ent e; e.lo = NRNE_LOAD(c->e[l - 1].lo); e.hi = NRNE_LOAD(c->e[l - 1].hi); e.meta = NRNE_LOAD(c->e[l - 1].meta);
    if (NRNE_LOAD(c->seq) != s1 && NRNE_MUTATE != 5) return 0;
    if (!n || a < e.lo || a >= e.hi) return 0;
    if (a + n > e.hi && NRNE_MUTATE != 6) return 0;
    *out = e;
    return 1;
}

// ---- the handles ----
// One slot a (kind, index); a slot holds the newest handle that mapped to it. epoch: the serial of the handle's newest event
// (creation, an update recorded for it, a CPU copy into it, the scratch mark); 0 = unknown (never seen, pushed out, destroyed).
enum { NRNE_H = 16384 };
typedef struct nrne_hent { uint64_t handle, epoch, lo, size; } nrne_hent;   // atomics; lo / size: its guest range (0: none known)
typedef struct nrne_h { nrne_hent e[NRNE_K_N][NRNE_H]; } nrne_h;
static inline nrne_hent *nrne_h_slot(const nrne_h *t, int kind, uint64_t handle) { return (nrne_hent *)&t->e[kind][(uint32_t)handle & (NRNE_H - 1)]; }
static inline int nrne_h_none(uint64_t handle) { return !(handle & 0x003fffff00000000ull); }   // generation 0: the engine's "no object"
// The engine made `handle` (over [lo, lo + size), 0 / 0 when unknown): the slot is its, whatever it held.
static inline void nrne_h_new(nrne_h *t, int kind, uint64_t handle, uint64_t lo, uint64_t size, uint64_t serial) {
    if (nrne_h_none(handle)) return;
    nrne_hent *e = nrne_h_slot(t, kind, handle);
    const int same = NRNE_LOAD(e->handle) == (handle & 0x003fffffffffffffull);
    NRNE_STORE(e->handle, 0);                                    // a reader between the stores sees "unknown"
    if (!(NRNE_MUTATE == 7 && !same)) NRNE_STORE(e->epoch, serial);
    else if (!NRNE_LOAD(e->epoch)) NRNE_STORE(e->epoch, serial);
    NRNE_STORE(e->lo, lo); NRNE_STORE(e->size, size);
    NRNE_STORE(e->handle, handle & 0x003fffffffffffffull);
}
// An event on a known handle. 1 = it is known (*lo / *size: its range).
static inline int nrne_h_event(nrne_h *t, int kind, uint64_t handle, uint64_t serial, uint64_t *lo, uint64_t *size) {
    nrne_hent *e = nrne_h_slot(t, kind, handle);
    if (nrne_h_none(handle) || NRNE_LOAD(e->handle) != (handle & 0x003fffffffffffffull)) return 0;
    nrne_max(&e->epoch, serial);
    if (lo) *lo = NRNE_LOAD(e->lo);
    if (size) *size = NRNE_LOAD(e->size);
    return 1;
}
static inline int nrne_h_kill(nrne_h *t, int kind, uint64_t handle, uint64_t *lo, uint64_t *size) {
    nrne_hent *e = nrne_h_slot(t, kind, handle);
    if (nrne_h_none(handle) || NRNE_LOAD(e->handle) != (handle & 0x003fffffffffffffull)) return 0;
    if (lo) *lo = NRNE_LOAD(e->lo);
    if (size) *size = NRNE_LOAD(e->size);
    NRNE_STORE(e->handle, 0);
    return 1;
}
static inline uint64_t nrne_h_epoch(const nrne_h *t, int kind, uint64_t handle) {
    const nrne_hent *e = nrne_h_slot(t, kind, handle);
    if (nrne_h_none(handle) || NRNE_LOAD(((nrne_hent *)e)->handle) != (handle & 0x003fffffffffffffull)) return 0;
    const uint64_t v = NRNE_LOAD(((nrne_hent *)e)->epoch);
    return NRNE_LOAD(((nrne_hent *)e)->handle) == (handle & 0x003fffffffffffffull) ? v : 0;   // the slot changed hands under the read
}

// ---- the constant ring ----
// The ring is only appended to (head = [dev+0x25a60]; an allocation that does not fit starts again at 0: nr_ubo.h). A head lower than
// the last one seen = the ring wrapped: every range bound before may be overwritten from now on.
typedef struct nrne_ring { uint64_t last, wraps; } nrne_ring;   // atomics
static inline void nrne_ring_note(nrne_ring *r, uint32_t head) {
    const uint64_t l = __atomic_exchange_n(&r->last, (uint64_t)head, __ATOMIC_ACQ_REL);
    if ((uint64_t)head < l && NRNE_MUTATE != 9) __atomic_add_fetch(&r->wraps, 1, __ATOMIC_ACQ_REL);
}

// ---- everything together (one object, made by the host when DM_NRN_EV is 1 or 2) ----
typedef struct nrn_ev { nrne_j j; nrne_cov cov; nrne_h h; nrne_ring ring; } nrn_ev;

// The writers' side (the host's hooks; `locked` = the caller holds the one mutex of the coverage).
// A buffer / texture the engine created over [lo, lo + size) (size 0: range unknown - the handle only).
static inline uint64_t nrn_ev_created(nrn_ev *v, int kind, int cls, uint64_t handle, uint64_t lo, uint64_t size) {
    const uint64_t s = nrne_j_write(&v->j, lo, size);
    if (size && kind != NRNE_K_TEX) nrne_cov_add(&v->cov, lo, lo + size, nrne_meta(kind, cls, handle));
    nrne_h_new(&v->h, kind, handle, lo, size, s);
    return s;
}
static inline uint64_t nrn_ev_destroyed(nrn_ev *v, int kind, uint64_t handle) {
    uint64_t lo = 0, size = 0;
    if (!nrne_h_kill(&v->h, kind, handle, &lo, &size)) {   // another handle took its slot: the coverage must not keep the range
        if (kind != NRNE_K_TEX && !nrne_h_none(handle)) nrne_cov_del_handle(&v->cov, nrne_meta(kind, 0, handle));
        return nrne_j_write(&v->j, 0, 0);
    }
    if (size && kind != NRNE_K_TEX) nrne_cov_del(&v->cov, lo, nrne_meta(kind, 0, handle));
    return nrne_j_write(&v->j, lo, size);
}
// The bytes of `handle` change now (a CPU copy into it) or will be changed by the record being written (a GPU copy): [off, off + n)
// of its range, n == 0: all of it. journal: 0 for an update the backend makes later (it journals the write itself when it executes
// the record - nrn_ev_wrote); the handle's epoch moves either way (the game thread reads handles at record time).
static inline uint64_t nrn_ev_updated(nrn_ev *v, int kind, uint64_t handle, uint64_t off, uint64_t n, int journal) {
    uint64_t lo = 0, size = 0;
    if (NRNE_MUTATE == 1) return 0;
    if (!nrne_h_event(&v->h, kind, handle, 0, &lo, &size)) return 0;   // (serial 0 moves nothing: the lookup)
    if (!n || off > size || n > size - off) { off = 0; n = size; }
    const uint64_t s = nrne_j_write(&v->j, journal ? lo + off : 0, journal ? n : 0);
    nrne_h_event(&v->h, kind, handle, s, NULL, NULL);
    return s;
}
// A CPU copy into a buffer whose handle the handle table no longer knows (another handle of its slot came later; nrn_ev_updated
// returned 0): the journal of its range must still move - the range comes from the coverage. The caller holds the coverage's mutex.
// 0 = no such buffer is covered (nothing can be stale).
static inline uint64_t nrn_ev_updated_slow(nrn_ev *v, int kind, uint64_t handle) {
    uint64_t lo = 0, size = 0;
    if (NRNE_MUTATE == 1 || nrne_h_none(handle) || !nrne_cov_range_of(&v->cov, nrne_meta(kind, 0, handle), &lo, &size)) return 0;
    return nrne_j_write(&v->j, lo, size);
}
// `handle` is written outside every event from now on (the immediate-mode scratch buffer): never covered. The caller holds the
// coverage's mutex.
static inline void nrn_ev_scratch(nrn_ev *v, int kind, uint64_t handle) {
    uint64_t lo = 0, size = 0;
    const uint64_t s = nrne_j_write(&v->j, 0, 0);
    if (nrne_h_none(handle)) return;
    if ((!nrne_h_event(&v->h, kind, handle, s, &lo, &size) || !size) && !nrne_cov_range_of(&v->cov, nrne_meta(kind, 0, handle), &lo, &size)) return;
    nrne_cov_class(&v->cov, lo, NRNE_C_SCRATCH);
    nrne_j_write(&v->j, lo, size);
}
// The backend wrote [a, a + n) of guest memory itself (a replayed copy, a GPU writeback), or a pool came / went there.
static inline uint64_t nrn_ev_wrote(nrn_ev *v, uint64_t a, uint64_t n) { return nrne_j_write(&v->j, a, n ? n : 1); }

// ---- the API of the other pieces (any thread, no lock, never waits) ----
// The content epoch of the guest bytes [a, a + n): != 0 only when the range lies wholly inside one engine buffer whose every write is
// an event (not the scratch buffer). Two asks with the same answer saw the same bytes (as far as the events of the header go); 0 =
// unknown / not covered - ask the page tracker. *cls / *kind (may be NULL): what the buffer is.
static inline uint64_t nrn_ev_range_epoch(const nrn_ev *v, uint64_t a, uint64_t n, int *kind, int *cls) {
    nrne_ent e;
    if (!v || !nrne_cov_find(&v->cov, a, n, &e)) return 0;
    if (kind) *kind = nrne_meta_kind(e.meta);
    if (cls) *cls = nrne_meta_cls(e.meta);
    if (nrne_meta_cls(e.meta) == NRNE_C_SCRATCH && NRNE_MUTATE != 8) return 0;
    const uint64_t s = nrne_j_ver(&v->j, a, n);
    nrne_ent e2;
    if (!nrne_cov_find(&v->cov, a, n, &e2) || e2.lo != e.lo || e2.meta != e.meta) return 0;   // it changed hands under the ask
    // the entry's identity is part of the word: another buffer at the same address is another epoch even at the same serial
    uint64_t w = (s ^ e.lo * 0x9E3779B97F4A7C15ull ^ e.meta * 0xC2B2AE3D27D4EB4Full);
    w ^= w >> 31;
    return w ? w : 1;
}
// The epoch of an engine vertex (NRNE_K_VB) / index (NRNE_K_IB) buffer handle as the GAME THREAD sees it when it records a draw: it
// moves when the handle is created, when an update of it is recorded (the draw recorded after it reads the new bytes) and at a CPU copy
// into it. 0 = unknown (not seen, pushed out of its slot, destroyed, generation 0).
static inline uint64_t nrn_ev_buf_epoch(const nrn_ev *v, int kind, uint64_t handle) { return v && kind >= 0 && kind < NRNE_K_TEX ? nrne_h_epoch(&v->h, kind, handle) : 0; }
// The epoch of an engine texture handle: creation and every opcode-0x2c write into it. 0 = unknown. (What the GPU renders into a
// target is not an event: a target's content is not described by this word.)
static inline uint64_t nrn_ev_tex_epoch(const nrn_ev *v, uint64_t handle) { return v ? nrne_h_epoch(&v->h, NRNE_K_TEX, handle) : 0; }
// How many times the constant ring has wrapped (a range bound under another count may hold other bytes now).
static inline uint64_t nrn_ev_ring_epoch(const nrn_ev *v) { return v ? NRNE_LOAD(((nrn_ev *)v)->ring.wraps) : 0; }

// ---- the verify of DM_NRN_EV=2 (the backend's; pure, so the host test runs it against a model of the page tracker) ----
// One ask about a covered range: the tracker's answer (have_tr: every page read-only now; g / inc: its generation / pool number), the
// events' epoch, a hash of the bytes (0 = not hashed). Against the previous ask of the same range:
//   SAME      neither moved
//   BOTH      both moved
//   EVENT     only the events moved (an update the tracker did not see as a CPU write - a GPU copy the backend made, a neighbour's
//             event on a shared page): counted
//   NEIGHBOUR only the tracker moved and the bytes are the same (a write to something else on a shared page)
//   UNHASHED  only the tracker moved and the bytes were not hashed (budget): not decided
//   BAD       only the tracker moved and the bytes differ: a write no event announced - the class is not coverable
//   FIRST     nothing to compare with (first ask, or the tracker had no version at one of the two)
enum { NRNE_V_FIRST, NRNE_V_SAME, NRNE_V_BOTH, NRNE_V_EVENT, NRNE_V_NEIGHBOUR, NRNE_V_UNHASHED, NRNE_V_BAD, NRNE_V_N };
static inline const char *nrne_v_name(int v) {
    static const char *const k[NRNE_V_N] = {"first", "same", "both moved", "event only", "tracker only, bytes the same", "tracker only, not hashed", "BAD"};
    return v >= 0 && v < NRNE_V_N ? k[v] : "?";
}
typedef struct nrne_vmemo { uint64_t a, n, ev, g, inc, hash; uint8_t have_tr, used, pad[6]; } nrne_vmemo;
static inline int nrne_verify(nrne_vmemo *m, uint64_t a, uint64_t n, uint64_t ev, int have_tr, uint64_t g, uint64_t inc, uint64_t hash) {
    int r = NRNE_V_FIRST;
    if (m->used && m->a == a && m->n == n && m->have_tr && have_tr) {
        const int tr = m->g != g || m->inc != inc, e = m->ev != ev;
        if (!tr && !e) r = NRNE_V_SAME;
        else if (tr && e) r = NRNE_V_BOTH;
        else if (e) r = NRNE_V_EVENT;
        else if (!hash || !m->hash) r = NRNE_V_UNHASHED;
        else r = hash == m->hash ? NRNE_V_NEIGHBOUR : NRNE_V_BAD;
    }
    m->a = a; m->n = n; m->ev = ev; m->used = 1;
    if (have_tr) { m->g = g; m->inc = inc; m->have_tr = 1; if (hash) m->hash = hash; else if (r != NRNE_V_SAME && r != NRNE_V_FIRST) m->hash = 0; }
    else m->have_tr = 0;
    return r;
}
static inline uint64_t nrne_hash(const void *p, size_t n) {   // of a range's bytes (verify only); never 0
    const uint8_t *b = (const uint8_t *)p; uint64_t h[4] = {0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
    size_t i = 0;
    for (; i + 32 <= n; i += 32) { uint64_t w[4]; memcpy(w, b + i, 32); for (int k = 0; k < 4; k++) { h[k] = (h[k] ^ w[k]) * 0x9FB21C651E98DF25ull; h[k] ^= h[k] >> 29; } }
    uint64_t t[4] = {0, 0, 0, 0}; if (i < n) memcpy(t, b + i, n - i);
    uint64_t x = (uint64_t)n;
    for (int k = 0; k < 4; k++) { x = (x ^ h[k] ^ t[k]) * 0xBF58476D1CE4E5B9ull; x ^= x >> 31; }
    return x ? x : 1;
}
#endif
