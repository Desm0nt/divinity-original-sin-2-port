// [DOS2] NR-ZERO Z3b / Z3c / Z3d (docs/dos2/nr/NR-ZERO.md §13): the epochs that let a "live" step of an engine draw be checked by one
// word instead of being walked. No Vulkan / engine / host type, no environment. The engine side is in xbg_game.cpp (namespace nrzx),
// xbg_nr_buf.inc (the ring word), xbg_nr_desc.inc / xbg_nr_z3.inc (the image word); the host test is tests/nr_z3e_test.cpp.
//
//   Z3b  DM_NR_Z3B  the image word. NR-08 walks the images of an entry at every draw (nr_dimg_skip of nr_desc.h) and almost always finds
//        every one of them "quiet": never written by the GPU, uploaded, checked in this submission, in the layout sets read. An entry
//        remembers (nrz_quiet) that its walk found all quiet, under three words: the image epoch, the submission number and the slot
//        serial. The image epoch moves at every write of an Image field that can end "quiet" - enforced by the TYPE of those fields
//        (nrz_rise / nrz_fall / nrz_leave / nrz_checked below: an assignment that leaves the quiet value bumps the epoch; a use the
//        wrapper does not know - a reference, a pointer, ++ - does not compile) and by ~Image.
//   Z3c  DM_NR_Z3C  the mesh views. The vertex / index range of a draw (address, bytes) -> (buffer, offset) is remembered (nrz_vtab) under
//        the words its answer depends on: an in-place zero-copy view - the chunk liveness epoch, the write version of its chunk (every
//        replay CPU write onto the chunk), the slot serial (the serve is noted once a submission), the pending-writeback generation; a
//        static copy of NR-06 - the snapshot epoch, NR-06's static epoch (demotes, freed copies, trims, table changes), the
//        pending-writeback generation.
//   Z3d  DM_NR_Z3D  the ring word. A uniform block inside the constant ring served in place is (address - chunk base) when the ring lies in
//        ONE live zero-copy chunk, no other live chunk meets it and none of its pages is sticky (nrz_ring): valid while the chunk
//        liveness epoch and the sticky version of that chunk stand. (The mirror of devices without zero-copy copies granules at their
//        first bind: that is the data path, not a lookup - nothing to remember.)
#ifndef DM_NR_Z3E_H
#define DM_NR_Z3E_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The mutation switch of the host test (never defined in the engine build; source_structure_test.py checks): NRZE_MUTATE = k drops
// 1 the bump of nrz_rise, 2 of nrz_fall, 3 of nrz_leave, 4 of nrz_checked, 5 the epoch word of nrz_quiet_ok, 6 its submission word,
// 7 its serial word, 8..11 stamp 0..3 of nrz_vfresh, 12 the address compare of nrz_vmatch, 13 its size compare, 14 the sticky scan of
// nrz_ring_clean, 15 the alignment test of nrz_ring_off, 16 its upper bound; [NR-ZERO Z8 images] 17 the epoch word of nrz_rtq_ok, 18 its
// submission word, 19 its serial word, 20 its fragment-only flag, 21 its count of other write marks, 22 its pass words; of nrz_rt_img_ok
// 23 "in the layout sets read", 24 "not an attachment of the open pass", 25 "not written since the barrier the read sees", 26 "checked
// in this submission", 27 "no pending clear".
#ifndef NRZE_MUTATE
#define NRZE_MUTATE 0
#endif

#ifdef __cplusplus
// ---- Z3b: the image epoch and the field types that move it ----
inline uint64_t nrz_img_ep = 1;      // +1 whenever an image may have stopped being quiet (never 0)
uint64_t nrz_sub_now();              // the includer defines it: the submission number "checked in this submission" is compared with
// (Each type has a user-provided destructor on purpose: a field passed as it is through a variadic call - a log line - then does not
// compile instead of being passed as a struct; such a call names the value: (int)im.uploaded.)
// 0 / false -> anything else (gpu_written, last_write, storage_write, opw)
template <class T> struct nrz_rise {
    T v;
    ~nrz_rise() {}
    operator T() const { return v; }
    nrz_rise &operator=(T x) { if (NRZE_MUTATE != 1 && !v && x) ++nrz_img_ep; v = x; return *this; }
    nrz_rise &operator=(const nrz_rise &o) { return *this = o.v; }
};
// true -> false (uploaded)
struct nrz_fall {
    bool v;
    ~nrz_fall() {}
    operator bool() const { return v; }
    nrz_fall &operator=(bool x) { if (NRZE_MUTATE != 2 && v && !x) ++nrz_img_ep; v = x; return *this; }
    nrz_fall &operator=(const nrz_fall &o) { return *this = o.v; }
};
// Q -> anything else (layout: Q = the layout sets read)
template <class T, T Q> struct nrz_leave {
    T v;
    ~nrz_leave() {}
    operator T() const { return v; }
    nrz_leave &operator=(T x) { if (NRZE_MUTATE != 3 && v == Q && x != Q) ++nrz_img_ep; v = x; return *this; }
    nrz_leave &operator=(const nrz_leave &o) { return *this = o.v; }
};
// the current submission -> anything else (checked_submission)
struct nrz_checked {
    uint64_t v;
    ~nrz_checked() {}
    operator uint64_t() const { return v; }
    nrz_checked &operator=(uint64_t x) { if (NRZE_MUTATE != 4 && x != v && v == nrz_sub_now()) ++nrz_img_ep; v = x; return *this; }
    nrz_checked &operator=(const nrz_checked &o) { return *this = o.v; }
};
extern "C" {
#endif

// ---- Z3b: "every image of the entry was quiet" ----
typedef struct nrz_quiet { uint64_t ep, sub, serial; } nrz_quiet;   // ep 0 = never
static inline int nrz_quiet_ok(const nrz_quiet *q, uint64_t ep, uint64_t sub, uint64_t serial) {
    return (NRZE_MUTATE == 5 || q->ep == ep) && q->ep != 0 && (NRZE_MUTATE == 6 || q->sub == sub) && (NRZE_MUTATE == 7 || q->serial == serial);
}
static inline void nrz_quiet_set(nrz_quiet *q, uint64_t ep, uint64_t sub, uint64_t serial) { q->ep = ep; q->sub = sub; q->serial = serial; }

// ---- [NR-ZERO Z8 images, NR-ZERO.md section 29] "a walk of the entry would do nothing", also for images the GPU wrote ----
// nrz_quiet stands only for entries whose images were never written by the GPU: an image that was (a render target that is sampled)
// is sent to the read path at every draw - get_image and prepare_read - and for nearly every draw they do nothing. They do nothing
// when the image is checked in this submission and has contents, is in the layout sets read, is no attachment of the open pass, has
// no pending clear and was not written since the barrier the read sees. After a walk that left every image of the entry either
// quiet or so, the word is set under: the image epoch (layout / uploaded / checked / an image destroyed: the fields of nrz_quiet's
// types), the submission, the slot serial, the open pass (pass + framebuffer: the attachments), whether the read is fragment-only,
// and the count of write marks OTHER than the marks a draw gives the attachments of the pass it continues (`other` = the engine's
// write_seq - its count of those marks). Every write mark of an image takes a new write_seq, so while `other` stands no image
// that is not an attachment of the open pass was written, and the barrier points only move forward: the walk would do nothing.
typedef struct nrz_rtq { uint64_t ep, sub, serial, other, pass, fb; uint8_t frag; } nrz_rtq;   // ep 0 = never
static inline int nrz_rtq_ok(const nrz_rtq *q, uint64_t ep, uint64_t sub, uint64_t serial, uint64_t other, uint64_t pass, uint64_t fb, int frag) {
    return q->ep != 0 && (NRZE_MUTATE == 17 || q->ep == ep) && (NRZE_MUTATE == 18 || q->sub == sub) && (NRZE_MUTATE == 19 || q->serial == serial) &&
           (NRZE_MUTATE == 20 || q->frag == (uint8_t)(frag != 0)) && (NRZE_MUTATE == 21 || q->other == other) && (NRZE_MUTATE == 22 || (q->pass == pass && q->fb == fb));
}
static inline void nrz_rtq_set(nrz_rtq *q, uint64_t ep, uint64_t sub, uint64_t serial, uint64_t other, uint64_t pass, uint64_t fb, int frag) {
    q->ep = ep; q->sub = sub; q->serial = serial; q->other = other; q->pass = pass; q->fb = fb; q->frag = (uint8_t)(frag != 0);
}
// One image of the entry after the walk. skip: nr_dimg_skip answers SKIP; rt: it answers "written by the GPU at some time".
typedef struct nrz_rt_in { uint8_t skip, rt, checked, content, general, in_open, written, pending; } nrz_rt_in;
static inline int nrz_rt_img_ok(const nrz_rt_in *x) {
    if (x->skip) return 1;
    return x->rt && (NRZE_MUTATE == 26 || x->checked) && x->content && (NRZE_MUTATE == 23 || x->general) && (NRZE_MUTATE == 24 || !x->in_open) &&
           (NRZE_MUTATE == 25 || !x->written) && (NRZE_MUTATE == 27 || !x->pending);
}

// ---- Z3c: (address, bytes) -> view ----
enum { NRZV_NONE = 0, NRZV_STATIC = 1, NRZV_INPLACE = 2, NRZV_STAMPS = 4 };
typedef struct nrz_view {
    uint64_t addr, bytes;
    uint64_t st[NRZV_STAMPS];
    uint64_t buf, mem, off, mapped;      // the answer (handles as words)
    uint32_t kind, tag;                  // NRZV_*; tag: the call site and alignment the answer was made for
    int32_t chunk; uint32_t coherent;    // INPLACE: the chunk's table slot (its write version is stamp 1)
} nrz_view;
typedef struct nrz_vtab { nrz_view *e; uint32_t mask; } nrz_vtab;
static inline int nrz_vinit(nrz_vtab *t, uint32_t entries) {   // a power of two
    t->e = NULL; t->mask = 0;
    if (!entries || (entries & (entries - 1))) return -1;
    t->e = (nrz_view *)calloc(entries, sizeof(nrz_view));
    if (!t->e) return -1;
    t->mask = entries - 1;
    return 0;
}
static inline void nrz_vfree(nrz_vtab *t) { free(t->e); t->e = NULL; t->mask = 0; }
static inline nrz_view *nrz_vslot(nrz_vtab *t, uint64_t addr, uint64_t bytes) {
    uint64_t x = addr * 0x9E3779B97F4A7C15ull ^ bytes * 0xC2B2AE3D27D4EB4Full;
    return &t->e[(x ^ (x >> 29)) & t->mask];
}
static inline int nrz_vmatch(const nrz_view *e, uint64_t addr, uint64_t bytes, uint32_t tag) {
    return e->kind != NRZV_NONE && (NRZE_MUTATE == 12 || e->addr == addr) && (NRZE_MUTATE == 13 || e->bytes == bytes) && e->tag == tag;
}
static inline int nrz_vfresh(const nrz_view *e, const uint64_t st[NRZV_STAMPS]) {
    for (unsigned i = 0; i < NRZV_STAMPS; i++) if (NRZE_MUTATE != 8 + (int)i && e->st[i] != st[i]) return 0;
    return 1;
}

// ---- Z3d: the constant ring inside one zero-copy chunk ----
typedef struct nrz_ring {
    uint64_t lo, hi;          // the ring [lo, hi)
    uint64_t chunk_lo;        // base of the chunk that holds it
    uint64_t align;           // minUniformBufferOffsetAlignment
    int ok;                   // the whole ring is served in place (set by the engine from nrz_ring_clean + its chunk checks)
} nrz_ring;
// No page of [lo, hi) of the chunk [chunk_lo, chunk_lo + chunk_n) is sticky (sticky: one byte a 4 KB page of the chunk).
static inline int nrz_ring_clean(const uint8_t *sticky, uint64_t chunk_lo, uint64_t chunk_n, uint64_t lo, uint64_t hi) {
    if (!sticky || hi <= lo || lo < chunk_lo || hi - chunk_lo > chunk_n) return 0;
    if (NRZE_MUTATE == 14) return 1;
    for (uint64_t pg = (lo - chunk_lo) >> 12; pg <= (hi - 1 - chunk_lo) >> 12; pg++) if (sticky[pg]) return 0;
    return 1;
}
// The in-place offset of the block [addr, addr + n): 1 = *off is what the page walk (nb_ubo_view of nb_hit.h) answers; 0 = ask the walk
// (not wholly inside the ring, not aligned, the ring word not valid).
static inline int nrz_ring_off(const nrz_ring *r, uint64_t addr, uint64_t n, uint64_t *off) {
    if (!r->ok || !n || addr < r->lo || addr >= r->hi) return 0;
    if (NRZE_MUTATE != 16 && n > r->hi - addr) return 0;
    const uint64_t o = addr - r->chunk_lo;
    if (NRZE_MUTATE != 15 && r->align > 1 && o % r->align) return 0;
    *off = o;
    return 1;
}

#ifdef __cplusplus
}
#endif
#endif
