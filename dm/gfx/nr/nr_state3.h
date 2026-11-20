// [DOS2] NR-09S3 (DM_NR_STATE=4 / 5, default 0; docs/dos2/nr/NR-09S.md §10): the native state record of one opcode-21 packet.
// Pure C (C and C++, no NVN / Vulkan type, no engine call): used by the front end (dm/gfx/nvn/nvn_nr_state3.inc, inside hle_nvn.c), the
// backend (dm/gfx/vulkan/engine/xbg_nr_state3.inc) and the host test (dm/gfx/vulkan/engine/tests/nr_state3_test.c).
//
// S1 (DM_NR_STATE=1) made the packet's NVN calls (nrst_op of nr_state.h, proven against the machine code) through the HLE bodies: every
// bind wrote an NVN record, the NB-13a shadow captured it, deduplicated it by content hash and packed it as OP_X_NB_STATE, and the backend
// parsed that and ran each record through execute(). S3 makes ONE record of the whole packet instead: OP_X_NR_STATE = the bytes every
// bind would have copied (the same object bytes, in the same order of writes), applied by the backend straight into its State with the
// same field semantics as execute()'s cases. No content hash anywhere: nothing is deduplicated (the engine's own filter in the packet
// already drops what did not change), the record is keyed by nothing but the packet.
//
// Why no "handle -> blob id" dictionary (NR-09S §6 idea): the blend record's three objects are WRITTEN by the packet itself
// (SetBlendTarget / ColorStateSetBlendEnable follow the bound colour targets), so an engine handle does not name a content; DS / polygon /
// vertex-format blobs are 4-80 bytes, a lookup on both threads costs what copying them costs. A packet changes 2-3 things (the filter).
//
// Builder (front end): nr3_build walks the packet's calls in order. Object setters (BlendStateSetDefaults / SetBlendTarget,
// ColorStateSetDefaults / SetAlphaTest / SetBlendEnable) run on LOCAL copies of their objects (hooks NR3_BLEND_DEF ... = the HLE bodies on
// the copy), binds read the copy if the packet touched the object, the object's memory otherwise: the bytes are exactly the bytes the bind
// body would have copied at that point. nr3_build has no side effect at all (DM_NR_STATE=5 builds it next to the old calls); the caller
// writes the copies back (nr3_writeback: = what the setters would have left in the engine's objects), makes the taps' calls and writes the record.
// Not representable (0): a call the bodies would reject (vertex attrib / stream count, VB slot, face), or a write that "last write wins"
// per field would not reproduce (the same stencil part / stream set twice, > 3 programs, > 4 conditional-render calls): the caller
// makes the calls through the bodies (S1) - correct by construction.
//
// Record payload (8-aligned, after op_hdr):
//   nr3_hdr {ver, flags, nprog, nstream, have, blend (target mask), nvattr, vb (slot mask), the programs' NVN stage bits [3], seq (=5)}
//   then, only what is present, in this order:  prog[nprog] (u64 snapped prog_t *); ds (8); stencil value mask /
//   write mask / ref {i32 face, i32 value} (8 each); poly (4 + pad); blend[target] (8 each, target order); colour (4 + pad);
//   vattr {u32 x nvattr, padded to 8}; vstream (8 x nstream); vb[slot] {u64 address, u64 size}; alpha ref (f32 + pad).
//   flags: 0 = apply; NR3_F_CHECK = DM_NR_STATE=5: apply to a copy of the backend's state (the old records follow and write the real one);
//   NR3_F_CHECK_END = no content, compare the real state with the copy (seq pairs the two).
#ifndef DM_NR_STATE3_H
#define DM_NR_STATE3_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "nr_state.h"

enum { NR3_VER = 1, NR3_PROGS = 3, NR3_OBJS = 8, NR3_RENS = 4 };
enum { NR3_SZ_BLEND = 8, NR3_SZ_COLOR = 4, NR3_SZ_DS = 8, NR3_SZ_POLY = 4, NR3_SZ_VATTR = 4, NR3_SZ_VSTREAM = 8 };   // blend_st color_st ds_st poly_st va_st vs_st
enum { NR3_H_DS = 1u << 0, NR3_H_SVMASK = 1u << 1, NR3_H_SMASK = 1u << 2, NR3_H_SREF = 1u << 3, NR3_H_POLY = 1u << 4, NR3_H_COLOR = 1u << 5,
       NR3_H_VATTR = 1u << 6, NR3_H_VSTREAM = 1u << 7, NR3_H_ALPHA = 1u << 8, NR3_H_ALL = (1u << 9) - 1u };
enum { NR3_F_CHECK = 1, NR3_F_CHECK_END = 2 };
// NB-13a kinds (nvn_nb_state.h NBS_K_*; = NB-13b groups 0..4): asserted equal where both are visible
enum { NR3_K_BLEND, NR3_K_COLOR, NR3_K_POLY, NR3_K_DS, NR3_K_VFMT, NR3_K_VB, NR3_K_PROG };
typedef struct { uint8_t ver, flags, nprog, nstream; uint16_t have; uint16_t vb; uint8_t blend, nvattr, pst[NR3_PROGS], _r; uint16_t seq; } nr3_hdr;
#ifdef __cplusplus
static_assert(sizeof(nr3_hdr) == 16, "OP_X_NR_STATE header");
#else
_Static_assert(sizeof(nr3_hdr) == 16, "OP_X_NR_STATE header");
#endif

// The content of one record (the builder's output = the parser's output).
typedef struct {
    uint8_t flags, nprog, nstream, nvattr; uint16_t have, blend, vb; uint32_t seq;
    uint64_t prog[NR3_PROGS]; int32_t pstage[NR3_PROGS];
    uint64_t ds; int32_t sten[3][2];            // [0] value mask, [1] write mask, [2] ref: {face, value} (p_stencil which 0 / 1 / 2)
    uint32_t poly, color; float alpha;
    uint64_t blendb[8];                          // by the target in the bytes (blend_st.target = the low 3 bits of byte 0)
    uint32_t vattr[16]; uint64_t vstream[16];
    uint64_t vba[16], vbs[16];
} nr3_rec;
// The builder: the record + what the caller does besides writing it.
typedef struct { uint64_t p; uint32_t size, local, touched; uint64_t v; } nr3_obj;   // v: the copy (size bytes used)
typedef struct {
    nr3_rec r;
    uint64_t praw[NR3_PROGS];                   // the NVNprogram pointers (the IR tap / NR-02 tap want them, the record wants the snapshot)
    uint32_t notes_state, notes_vbuf;           // the NB tap's nbt_note counts the bind bodies would have made (NBN_STATE / NBN_VBUF)
    int nobj, nren; nr3_obj obj[NR3_OBJS]; int ren[NR3_RENS];   // ren: indices of SetRenderEnable(Conditional) calls (made through the bodies)
} nr3_b;

static inline uint64_t nr3_rd(uint64_t p, uint32_t n) { uint64_t v = 0; memcpy(&v, NRST_PTR(p), n); return v; }
// The object the packet's setters touch: a local copy (created on first touch: the object's bytes, or zero for the default path's stack
// objects NRST_LOCAL_*). NULL = more objects than NR3_OBJS (not representable).
static inline nr3_obj *nr3_obj_get(nr3_b *b, uint64_t p, uint32_t size) {
    for (int i = 0; i < b->nobj; i++) if (b->obj[i].p == p) return &b->obj[i];
    if (b->nobj >= NR3_OBJS) return NULL;
    nr3_obj *o = &b->obj[b->nobj++];
    o->p = p; o->size = size; o->local = p == NRST_LOCAL_BLEND || p == NRST_LOCAL_COLOR; o->touched = 1;
    o->v = o->local ? 0 : nr3_rd(p, size);
    return o;
}
// The bytes a bind body copies from object p now (the copy if a setter of this packet touched it).
static inline uint64_t nr3_bytes(const nr3_b *b, uint64_t p, uint32_t size) {
    for (int i = 0; i < b->nobj; i++) if (b->obj[i].p == p) return b->obj[i].v;
    if (p == NRST_LOCAL_BLEND || p == NRST_LOCAL_COLOR) return 0;
    return nr3_rd(p, size);
}

// Hooks the includer defines (front end: the HLE bodies; test: copies of them). o = pointer to the local copy. Without them (the
// backend) there is no builder.
#ifdef NR3_BLEND_DEF
#ifndef NR3_SNAP_PROG
#define NR3_SNAP_PROG(p) (p)
#endif
// 1 = the record (b->r) and the side lists are complete; 0 = not representable (make the calls through the bodies).
static inline int nr3_build(nr3_b *b, const nrst_op *op, int n) {
    memset(&b->r, 0, offsetof(nr3_rec, prog));
    b->notes_state = b->notes_vbuf = 0; b->nobj = b->nren = 0;
    nr3_rec *r = &b->r;
    for (int k = 0; k < n; k++) {
        const nrst_op *o = &op[k];
        switch (o->fn) {
        case NRST_F_PROGRAM:
            if (r->nprog >= NR3_PROGS || o->i < 0 || o->i > 0xff) return 0;   // (NVN stage bits: 6 stages)
            b->praw[r->nprog] = o->p; r->prog[r->nprog] = NR3_SNAP_PROG(o->p); r->pstage[r->nprog] = o->i; r->nprog++;
            break;
        case NRST_F_DS: r->ds = nr3_bytes(b, o->p, NR3_SZ_DS); r->have |= NR3_H_DS; b->notes_state++; break;
        case NRST_F_STENCIL_VMASK: case NRST_F_STENCIL_MASK: case NRST_F_STENCIL_REF: {
            const int w = o->fn == NRST_F_STENCIL_VMASK ? 0 : o->fn == NRST_F_STENCIL_MASK ? 1 : 2;
            const uint16_t h = (uint16_t)(NR3_H_SVMASK << w);
            if ((r->have & h) || o->i < 0 || o->i > 3) return 0;   // a second write of the part (another face would be lost) / execute()'s need
            r->sten[w][0] = o->i; r->sten[w][1] = o->j; r->have |= h;
            break; }
        case NRST_F_POLY: r->poly = (uint32_t)nr3_bytes(b, o->p, NR3_SZ_POLY); r->have |= NR3_H_POLY; b->notes_state++; break;
        case NRST_F_BLEND: { const uint64_t v = nr3_bytes(b, o->p, NR3_SZ_BLEND); const unsigned t = (unsigned)(v & 7u);
            r->blendb[t] = v; r->blend = (uint16_t)(r->blend | 1u << t); b->notes_state++; break; }
        case NRST_F_COLOR: r->color = (uint32_t)nr3_bytes(b, o->p, NR3_SZ_COLOR); r->have |= NR3_H_COLOR; b->notes_state++; break;
        case NRST_F_BLEND_DEF: { nr3_obj *x = nr3_obj_get(b, o->p, NR3_SZ_BLEND); if (!x) return 0; NR3_BLEND_DEF(&x->v); break; }
        case NRST_F_BLEND_TARGET: { nr3_obj *x = nr3_obj_get(b, o->p, NR3_SZ_BLEND); if (!x) return 0; NR3_BLEND_TARGET(&x->v, o->i); break; }
        case NRST_F_COLOR_DEF: { nr3_obj *x = nr3_obj_get(b, o->p, NR3_SZ_COLOR); if (!x) return 0; NR3_COLOR_DEF(&x->v); break; }
        case NRST_F_COLOR_ATEST: { nr3_obj *x = nr3_obj_get(b, o->p, NR3_SZ_COLOR); if (!x) return 0; NR3_COLOR_ATEST(&x->v, o->i); break; }
        case NRST_F_COLOR_BLEND: { nr3_obj *x = nr3_obj_get(b, o->p, NR3_SZ_COLOR); if (!x) return 0; NR3_COLOR_BLEND(&x->v, o->i, (uint32_t)o->j); break; }
        case NRST_F_VATTR:
            if (o->i < 0 || o->i > 16) return 0;
            r->nvattr = (uint8_t)o->i; if (o->i) memcpy(r->vattr, NRST_PTR(o->p), NR3_SZ_VATTR * (size_t)o->i);   // (VATTR replaces the set: last wins)
            r->have |= NR3_H_VATTR; b->notes_state++;
            break;
        case NRST_F_VSTREAM:
            if ((r->have & NR3_H_VSTREAM) || o->i < 0 || o->i > 16) return 0;   // a second, shorter set would not clear the tail
            r->nstream = (uint8_t)o->i; if (o->i) memcpy(r->vstream, NRST_PTR(o->p), NR3_SZ_VSTREAM * (size_t)o->i);
            r->have |= NR3_H_VSTREAM; b->notes_state++;
            break;
        case NRST_F_VB:
            if (o->i < 0 || o->i >= 16) return 0;
            r->vba[o->i] = o->p; r->vbs[o->i] = o->q; r->vb = (uint16_t)(r->vb | 1u << o->i); b->notes_vbuf++;
            break;
        case NRST_F_REN: case NRST_F_REN_COND:
            if (b->nren >= NR3_RENS) return 0;
            b->ren[b->nren++] = k;
            break;
        case NRST_F_ALPHA_REF: r->alpha = o->f; r->have |= NR3_H_ALPHA; break;
        default: return 0;
        }
    }
    return 1;
}
#endif
// The NB-13a kinds the record writes (front end: they become "unknown" in the shadow; backend: the NB-13b groups 0..4 to re-read).
static inline unsigned nr3_kinds(const nr3_rec *r) {
    unsigned k = 0;
    if (r->blend) k |= 1u << NR3_K_BLEND;
    if (r->have & (NR3_H_COLOR | NR3_H_ALPHA)) k |= 1u << NR3_K_COLOR;
    if (r->have & NR3_H_POLY) k |= 1u << NR3_K_POLY;
    if (r->have & (NR3_H_DS | NR3_H_SVMASK | NR3_H_SMASK | NR3_H_SREF)) k |= 1u << NR3_K_DS;
    if (r->have & (NR3_H_VATTR | NR3_H_VSTREAM)) k |= 1u << NR3_K_VFMT;
    if (r->vb) k |= 1u << NR3_K_VB;
    if (r->nprog) k |= 1u << NR3_K_PROG;
    return k;
}
// The setters' result into the engine's objects (not the default path's stack objects): what the bodies would have left there.
static inline void nr3_writeback(const nr3_b *b) {
    for (int i = 0; i < b->nobj; i++) if (!b->obj[i].local) memcpy(NRST_PTR(b->obj[i].p), &b->obj[i].v, b->obj[i].size);
}
static inline uint32_t nr3_pop(uint32_t v) { return (uint32_t)__builtin_popcount(v); }
// Payload bytes of r (a multiple of 8).
static inline uint32_t nr3_size(const nr3_rec *r) {
    if (r->flags & NR3_F_CHECK_END) return (uint32_t)sizeof(nr3_hdr);
    uint32_t n = (uint32_t)sizeof(nr3_hdr) + 8u * r->nprog;
    n += 8u * nr3_pop(r->have & (NR3_H_DS | NR3_H_SVMASK | NR3_H_SMASK | NR3_H_SREF | NR3_H_POLY | NR3_H_COLOR | NR3_H_ALPHA));
    n += 8u * nr3_pop(r->blend) + 16u * nr3_pop(r->vb);
    if (r->have & NR3_H_VATTR) n += (4u * r->nvattr + 7u) & ~7u;
    if (r->have & NR3_H_VSTREAM) n += 8u * r->nstream;
    return n;
}
static inline uint8_t *nr3_w8(uint8_t *d, const void *s, uint32_t n) { memcpy(d, s, n); if (n & 7) memset(d + n, 0, 8 - (n & 7)); return d + ((n + 7) & ~7u); }
// Writes r (flags / seq as in r) into dst (nr3_size(r) bytes).
static inline void nr3_put(const nr3_rec *r, uint8_t *dst) {
    nr3_hdr h; memset(&h, 0, sizeof h);
    h.ver = NR3_VER; h.flags = r->flags; h.seq = (uint16_t)r->seq;
    if (!(r->flags & NR3_F_CHECK_END)) {
        h.nprog = r->nprog; h.nstream = r->nstream; h.have = r->have; h.blend = (uint8_t)r->blend; h.vb = r->vb; h.nvattr = r->nvattr;
        for (int i = 0; i < r->nprog; i++) h.pst[i] = (uint8_t)r->pstage[i];
    }
    memcpy(dst, &h, sizeof h); uint8_t *d = dst + sizeof h;
    if (r->flags & NR3_F_CHECK_END) return;
    for (int i = 0; i < r->nprog; i++) d = nr3_w8(d, &r->prog[i], 8);
    if (r->have & NR3_H_DS) d = nr3_w8(d, &r->ds, 8);
    for (int w = 0; w < 3; w++) if (r->have & (NR3_H_SVMASK << w)) d = nr3_w8(d, r->sten[w], 8);
    if (r->have & NR3_H_POLY) d = nr3_w8(d, &r->poly, 4);
    for (unsigned m = r->blend; m; m &= m - 1) d = nr3_w8(d, &r->blendb[__builtin_ctz(m)], 8);
    if (r->have & NR3_H_COLOR) d = nr3_w8(d, &r->color, 4);
    if ((r->have & NR3_H_VATTR) && r->nvattr) d = nr3_w8(d, r->vattr, 4u * r->nvattr);
    if ((r->have & NR3_H_VSTREAM) && r->nstream) d = nr3_w8(d, r->vstream, 8u * r->nstream);
    for (unsigned m = r->vb; m; m &= m - 1) { const int k = __builtin_ctz(m); uint64_t q[2] = {r->vba[k], r->vbs[k]}; d = nr3_w8(d, q, 16); }
    if (r->have & NR3_H_ALPHA) d = nr3_w8(d, &r->alpha, 4);
}
// Reads a payload of n bytes (may include the record's alignment tail) into r. 0 = malformed.
static inline int nr3_parse(const uint8_t *p, size_t n, nr3_rec *r) {
    nr3_hdr h;
    if (!p || n < sizeof h) return 0;
    memcpy(&h, p, sizeof h);
    if (h.ver != NR3_VER || h.flags > NR3_F_CHECK_END || h.nprog > NR3_PROGS || h.nstream > 16 || h.nvattr > 16 || (h.have & ~NR3_H_ALL)) return 0;
    memset(r, 0, offsetof(nr3_rec, prog));
    r->flags = h.flags; r->seq = h.seq;
    if (h.flags & NR3_F_CHECK_END) return !(h.nprog | h.nstream | h.have | h.blend | h.vb | h.nvattr | h.pst[0] | h.pst[1] | h.pst[2]);
    r->nprog = h.nprog; r->nstream = h.nstream; r->have = h.have; r->blend = h.blend; r->vb = h.vb; r->nvattr = h.nvattr;
    if (!(h.have & NR3_H_VATTR) && h.nvattr) return 0;
    if (!(h.have & NR3_H_VSTREAM) && h.nstream) return 0;
    if (nr3_size(r) > n) return 0;
    const uint8_t *s = p + sizeof h;
    for (int i = 0; i < r->nprog; i++) { memcpy(&r->prog[i], s, 8); s += 8; r->pstage[i] = h.pst[i]; }
    if (r->have & NR3_H_DS) { memcpy(&r->ds, s, 8); s += 8; }
    for (int w = 0; w < 3; w++) if (r->have & (NR3_H_SVMASK << w)) { memcpy(r->sten[w], s, 8); s += 8; if (r->sten[w][0] < 0 || r->sten[w][0] > 3) return 0; }
    if (r->have & NR3_H_POLY) { memcpy(&r->poly, s, 4); s += 8; }
    for (int t = 0; t < 8; t++) if (r->blend >> t & 1) { memcpy(&r->blendb[t], s, 8); s += 8; if ((r->blendb[t] & 7u) != (uint64_t)t) return 0; }
    if (r->have & NR3_H_COLOR) { memcpy(&r->color, s, 4); s += 8; }
    if ((r->have & NR3_H_VATTR) && r->nvattr) { memcpy(r->vattr, s, 4u * r->nvattr); s += (4u * r->nvattr + 7u) & ~7u; }
    if ((r->have & NR3_H_VSTREAM) && r->nstream) { memcpy(r->vstream, s, 8u * r->nstream); s += 8u * r->nstream; }
    for (int k = 0; k < 16; k++) if (r->vb >> k & 1) { uint64_t q[2]; memcpy(q, s, 16); s += 16; r->vba[k] = q[0]; r->vbs[k] = q[1]; }
    if (r->have & NR3_H_ALPHA) { memcpy(&r->alpha, s, 4); s += 8; }
    return 1;
}

// ---- the backend side, generic over the state struct (the backend's State, its check copy, the test's model) ----
// T has the fields of xbg_game.cpp State with the same names and types: shader[6], ds, stencil_compare / stencil_write / stencil_ref[2],
// poly, blend[8], color, attrib[16], stream[16], vb[16] (NVNbufferRange-like {address, size}), alpha_ref.
// BIND(S, prog, stages): the OP_BIND_PROGRAM case of execute() on S. VATTR(attrib, st, n): xbg_vertex_attributes_replace (0 = ok).
// Every field below is set as execute()'s case for the record the bind body would have written sets it.
#define NR3_DEFINE_APPLY(NAME, T, BIND, VATTR)                                                                                          \
    static inline int NAME(T *S, const nr3_rec *v) {                                                                                     \
        for (int i = 0; i < v->nprog; i++) BIND(S, v->prog[i], v->pstage[i]);                                                            \
        if (v->have & NR3_H_DS) memcpy(&S->ds, &v->ds, sizeof S->ds);                                                                    \
        for (int w = 0; w < 3; w++) if (v->have & (NR3_H_SVMASK << w)) {                                                                 \
            const int f = v->sten[w][0]; const int32_t x = v->sten[w][1];                                                                \
            for (unsigned i = 0; i < 2; i++) if (f & (1 << i)) { if (w == 0) S->stencil_compare[i] = x; else if (w == 1) S->stencil_write[i] = x; else S->stencil_ref[i] = x; } \
        }                                                                                                                                \
        if (v->have & NR3_H_POLY) memcpy(&S->poly, &v->poly, sizeof S->poly);                                                            \
        for (int t = 0; t < 8; t++) if (v->blend >> t & 1) memcpy(&S->blend[t], &v->blendb[t], sizeof S->blend[t]);                    \
        if (v->have & NR3_H_COLOR) memcpy(&S->color, &v->color, sizeof S->color);                                                        \
        if ((v->have & NR3_H_VATTR) && VATTR(S->attrib, v->vattr, v->nvattr)) return -1;                                                  \
        if (v->have & NR3_H_VSTREAM) memcpy(S->stream, v->vstream, 8u * v->nstream);                                                     \
        for (int k = 0; k < 16; k++) if (v->vb >> k & 1) { S->vb[k].address = v->vba[k]; S->vb[k].size = v->vbs[k]; }                   \
        if (v->have & NR3_H_ALPHA) S->alpha_ref = v->alpha;                                                                              \
        return 0;                                                                                                                        \
    }
// The same straight from the payload (the backend's path: no copy into an nr3_rec). -1 = malformed (or VATTR refused). Equal to
// NR3_DEFINE_APPLY on nr3_parse's result for every record nr3_parse accepts (tests/nr_state3_test.c); a blend part goes to the target its
// bytes name, as execute()'s OP_BIND_STATE does (state.blend[b.target]).
#define NR3_DEFINE_APPLY_RAW(NAME, T, BIND, VATTR)                                                                                      \
    static inline int NAME(T *S, const uint8_t *p, size_t n) {                                                                           \
        nr3_hdr h; if (n < sizeof h) return -1; memcpy(&h, p, sizeof h);                                                                  \
        if (h.ver != NR3_VER || (h.flags & NR3_F_CHECK_END) || h.nprog > NR3_PROGS || h.nstream > 16 || h.nvattr > 16 || (h.have & ~NR3_H_ALL) \
            || (h.nvattr && !(h.have & NR3_H_VATTR)) || (h.nstream && !(h.have & NR3_H_VSTREAM))) return -1;                                \
        { nr3_rec z; z.flags = 0; z.nprog = h.nprog; z.nstream = h.nstream; z.nvattr = h.nvattr; z.have = h.have; z.blend = h.blend; z.vb = h.vb; \
          if (nr3_size(&z) > n) return -1; }                                                                                              \
        const uint8_t *s = p + sizeof h;                                                                                                 \
        for (int i = 0; i < h.nprog; i++, s += 8) { uint64_t pr; memcpy(&pr, s, 8); BIND(S, pr, (int32_t)h.pst[i]); }                    \
        if (h.have & NR3_H_DS) { memcpy(&S->ds, s, sizeof S->ds); s += 8; }                                                               \
        for (int w = 0; w < 3; w++) if (h.have & (NR3_H_SVMASK << w)) {                                                                   \
            int32_t fx[2]; memcpy(fx, s, 8); s += 8; if (fx[0] < 0 || fx[0] > 3) return -1;                                               \
            for (unsigned i = 0; i < 2; i++) if (fx[0] & (1 << i)) { if (w == 0) S->stencil_compare[i] = fx[1]; else if (w == 1) S->stencil_write[i] = fx[1]; else S->stencil_ref[i] = fx[1]; } \
        }                                                                                                                                \
        if (h.have & NR3_H_POLY) { memcpy(&S->poly, s, sizeof S->poly); s += 8; }                                                         \
        for (unsigned m = h.blend; m; m &= m - 1, s += 8) memcpy(&S->blend[s[0] & 7u], s, sizeof S->blend[0]);                            \
        if (h.have & NR3_H_COLOR) { memcpy(&S->color, s, sizeof S->color); s += 8; }                                                      \
        if (h.have & NR3_H_VATTR) { if (VATTR(S->attrib, (const uint32_t *)(const void *)s, h.nvattr)) return -1; s += (4u * h.nvattr + 7u) & ~7u; } \
        if (h.have & NR3_H_VSTREAM) { memcpy(S->stream, s, 8u * h.nstream); s += 8u * h.nstream; }                                       \
        for (unsigned m = h.vb; m; m &= m - 1, s += 16) memcpy(&S->vb[__builtin_ctz(m)], s, 16);                                         \
        if (h.have & NR3_H_ALPHA) memcpy(&S->alpha_ref, s, 4);                                                                            \
        return 0;                                                                                                                        \
    }
// The NB-13a kinds of a payload (backend: NB-13b's groups), from its header only.
static inline unsigned nr3_kinds_raw(const uint8_t *p) {
    nr3_hdr h; memcpy(&h, p, sizeof h);
    nr3_rec z; z.nprog = h.nprog; z.have = h.have; z.blend = h.blend; z.vb = h.vb;
    return nr3_kinds(&z);
}
// The compared fields of the =5 check, in this order (nr3_field_name).
enum { NR3_C_SHADER, NR3_C_DS, NR3_C_SCMP, NR3_C_SWRITE, NR3_C_SREF, NR3_C_POLY, NR3_C_BLEND, NR3_C_COLOR, NR3_C_ATTRIB, NR3_C_STREAM, NR3_C_VB,
       NR3_C_ALPHA, NR3_C_MISSING, NR3_C_N };
static inline const char *nr3_field_name(int f) {
    static const char *const n[NR3_C_N] = {"shader", "ds", "stencil_compare", "stencil_write", "stencil_ref", "poly", "blend", "color", "attrib",
                                           "stream", "vb", "alpha_ref", "missing_program"};
    return f >= 0 && f < NR3_C_N ? n[f] : "-";
}
// Copy of the compared fields of S (D: any struct with them + `missing`); MISSING = the flag of S.
#define NR3_DEFINE_SNAP(NAME, D, T)                                                                                                     \
    static inline void NAME(D *d, const T *S, int missing) {                                                                             \
        memcpy(d->shader, S->shader, sizeof d->shader); memcpy(&d->ds, &S->ds, sizeof d->ds);                                           \
        memcpy(d->stencil_compare, S->stencil_compare, sizeof d->stencil_compare); memcpy(d->stencil_write, S->stencil_write, sizeof d->stencil_write); \
        memcpy(d->stencil_ref, S->stencil_ref, sizeof d->stencil_ref); memcpy(&d->poly, &S->poly, sizeof d->poly);                      \
        memcpy(d->blend, S->blend, sizeof d->blend); memcpy(&d->color, &S->color, sizeof d->color); memcpy(d->attrib, S->attrib, sizeof d->attrib); \
        memcpy(d->stream, S->stream, sizeof d->stream); memcpy(d->vb, S->vb, sizeof d->vb); d->alpha_ref = S->alpha_ref; d->missing = missing != 0; \
    }
// -1 = the same; else the first differing field (NR3_C_*).
#define NR3_DEFINE_CMP(NAME, D)                                                                                                         \
    static inline int NAME(const D *a, const D *b) {                                                                                     \
        if (memcmp(a->shader, b->shader, sizeof a->shader)) return NR3_C_SHADER;                                                         \
        if (memcmp(&a->ds, &b->ds, sizeof a->ds)) return NR3_C_DS;                                                                       \
        if (memcmp(a->stencil_compare, b->stencil_compare, sizeof a->stencil_compare)) return NR3_C_SCMP;                                \
        if (memcmp(a->stencil_write, b->stencil_write, sizeof a->stencil_write)) return NR3_C_SWRITE;                                    \
        if (memcmp(a->stencil_ref, b->stencil_ref, sizeof a->stencil_ref)) return NR3_C_SREF;                                            \
        if (memcmp(&a->poly, &b->poly, sizeof a->poly)) return NR3_C_POLY;                                                               \
        if (memcmp(a->blend, b->blend, sizeof a->blend)) return NR3_C_BLEND;                                                             \
        if (memcmp(&a->color, &b->color, sizeof a->color)) return NR3_C_COLOR;                                                           \
        if (memcmp(a->attrib, b->attrib, sizeof a->attrib)) return NR3_C_ATTRIB;                                                         \
        if (memcmp(a->stream, b->stream, sizeof a->stream)) return NR3_C_STREAM;                                                         \
        if (memcmp(a->vb, b->vb, sizeof a->vb)) return NR3_C_VB;                                                                         \
        if (memcmp(&a->alpha_ref, &b->alpha_ref, sizeof a->alpha_ref)) return NR3_C_ALPHA;                                               \
        if (a->missing != b->missing) return NR3_C_MISSING;                                                                              \
        return -1;                                                                                                                       \
    }
#endif
