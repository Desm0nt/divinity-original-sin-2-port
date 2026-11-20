// [DOS2] NR-ZERO Z8 (DM_NR_Z8, docs/dos2/nr/NR-ZERO.md section 25): the pure part of "the thread that submits knows the draw".
// C++, no Vulkan / engine object: the engine side is xbg_nr_z8.inc, the host test tests/nr_z8_test.cpp.
// Needs dm/gfx/nvn/nvn_obj.h before it (the record opcodes and payloads, nvn_nb_draw.h, nvn_nb_state.h).
//
// Z5 (nr_z5p.h) PREDICTS the values of a draw record from the last record of the same site. Z8 makes the answer exact:
//   fold     the submitting thread walks the records of a submission in the order the translation thread will execute them (walk():
//            OP_JUMP / OP_CALL / OP_X_NB_STATE blocks with their per-epoch dictionary / OP_X_NATIVE_DRAW, as execute()) and applies
//            every record that writes the backend's State to its own copy (fold(): the statements of execute()'s cases, kept equal by
//            tests/source_structure_test.py; OP_X_NB_DRAW binds through nbd_apply and OP_X_NR_STATE through nr_state3.h's apply - the
//            translation thread's own functions). The copy is the State the translation thread will have at each record.
//   fix id   the part of that State a draw's resolution reads besides tables (targets, viewport, pools, programs, vertex format,
//            blend, depth-stencil, raster, storage bindings), in five groups; each group's content gets an id from a dictionary that
//            compares bytes (Dict), the five ids one more: equal fix ids = equal bytes. Ids are never handed out twice.
//   key      site words (nr_z5p.h) + fix id pick a slot; the slot's template names what else the resolution read - the texture /
//            sampler handles of the plan's rows, the vertex ranges of the pipeline's bindings, the uniform slots and sizes - WITH the
//            values they had (the translation thread publishes them from its own State). match() compares them with the copy: a
//            template is used only for a draw whose every compared word is equal.
// What is exact by this: the draw's plan, NR-08 entry and NR-09 entry are functions of the compared words and of the translation
// thread's tables, whose generations it checks when it executes the record. What stays "of the moment" (ring offsets, views, image
// states, the open pass) is resolved by the translation thread per draw as before and compared (nr_z5r.h Reader).
#ifndef DM_NR_Z8_H
#define DM_NR_Z8_H
#include <atomic>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include "nr_z5p.h"
#include "nr_state3.h"
#include "nr_desc.h"   // nr_dplan: the plan of a shader set (describe())

// The mutation switch of the host test (never defined in the engine build): NRZ8_MUTATE = k - 1 a REF block is executed without its
// epoch check, 2 OP_CALL is not followed, 3 the stencil fold ignores the face mask, 4 a vertex-stream record is folded without its
// first index, 5 the dictionary answers by four bits of the hash alone, 6 a group's id is not refreshed when the fold marked it, 7 match() does not
// compare the fix id, 8 match() does not compare the texture rows, 9 match() does not compare the vertex ranges, 10 match() does not
// compare the uniform sizes, 11 the table's reader does not look at the version after the copy, 12 OP_JUMP is not followed,
// 13 the depth range is folded into the wrong words, 14 match() does not compare the sampler of a row,
// 15 the reader of the marks does not compare the submission of an entry, 16 it does not compare the record's address, 17 it does not
// read the entry's token again after the words, 18 the writer of the marks publishes the token before the words,
// 19 Ranges::id_of does not compare the size of a range, 20 it does not compare the submission an id was given in; expect_diff does
// not compare 21 the pipeline, 22 the set / layout, 23 the dynamic offsets, 24 viewport / scissor, 25 the vertex streams, 26 the index
// buffer and the draw arguments,
// 27 a set with a support / c1 block bound as a dynamic uniform buffer cannot be a template (plan_form() as it was before NR-ZERO.md
// section 36: on a device with dynamic uniform buffers on, that is every set of the game - nothing was ever published), 28 match()
// gives the offset of such a block as 0, 29 expect_diff does not compare it, 30 the walk does not get its own thread while
// DM_NB_OWN is on (walker_thread() as it was: the game's default - the thread never started),
// [NR-ZERO.md section 39] 31 the walk looks at way 0 of a bucket only (the direct-mapped table of the phone series R8: "another site"),
// 32 prologue_ok() takes a prologue that changed State for a twin, 33 same_shaders() does not compare the fragment stage (a twin the
// prologue did not make this time), 34 match() refuses a draw whose vertex ranges differ (R8: "vertex ranges differ" - the answer does
// not read them). (31 and 34 cost answers, not exactness: they fail the census - the answer share of the world of the phone series;
// 7 now also covers the fix id the walk compares in a way, find() / probe().)
// (9 is the one that need NOT fail: the vertex ranges - like the uniform addresses - give values the translation thread resolves again and
// compares at the moment; without the compare here a draw is only written ahead in vain. The texture rows, the fix id and the uniform
// sizes pick the ANSWER, which is not resolved again: 7, 8, 10, 14 must fail.)
#ifndef NRZ8_MUTATE
#define NRZ8_MUTATE 0
#endif

namespace nrz8 {

// ---- fold: what a record leaves in State -------------------------------------------------------------------------------------
// The dirty bits name the fix group (or table) the record wrote.
enum : unsigned { D_TARGET = 1u << 0, D_PROG = 1u << 1, D_BLEND = 1u << 2, D_DEPTH = 1u << 3, D_STORE = 1u << 4,   // groups 0..4
                  D_VB = 1u << 5, D_UB = 1u << 6, D_TEX = 1u << 7, D_OTHER = 1u << 8, D_BAD = 1u << 31 };
enum { GROUPS = 5 };
template <class T> static inline bool rd(T &o, const uint8_t *p, size_t n) { if (n < sizeof(T)) return false; memcpy(&o, p, sizeof o); return true; }
template <class T> static inline void rdv(T &o, const uint8_t *p, size_t n) { memset(&o, 0, sizeof o); memcpy(&o, p, n < sizeof(T) ? n : sizeof(T)); }

// One plain record of op (payload p, n bytes) into the State-shaped S. prog(program, stages): the program bind (the copy keeps the
// program word per stage; the translation thread its shaders); vattr(state, attributes, n): xbg_vertex_attributes_replace.
// 0 = the record writes no State; D_BAD = a record execute() would stop on.
// Every case is the statement of execute()'s case of the same op (xbg_game.cpp), `state` spelled st.
template <class S, class Prog, class Vattr> static inline unsigned fold(S &st, uint32_t op, const uint8_t *p, size_t n, Prog &&prog, Vattr &&vattr) {
    switch (op) {
    case OP_SET_RT: { p_set_rt a; if (!rd(a, p, n)) return D_BAD; st.rt = a; return D_TARGET; }
    case OP_VIEWPORTS: { p_viewports a; if (!rd(a, p, n) || !(a.first == 0 && a.n == 1)) return D_BAD;
        st.viewport.x = a.v[0][0]; st.viewport.y = a.v[0][1]; st.viewport.width = a.v[0][2]; st.viewport.height = a.v[0][3]; return D_TARGET; }
    case OP_SCISSOR: { p_scissor a; if (!rd(a, p, n) || !(a.w >= 0 && a.h >= 0)) return D_BAD; st.scissor = {{a.x, a.y}, {uint32_t(a.w), uint32_t(a.h)}}; return D_TARGET; }
    case OP_DEPTH_RANGE: { p_depth_range a; if (!rd(a, p, n)) return D_BAD;
        if (NRZ8_MUTATE == 13) { st.viewport.minDepth = a.f; st.viewport.maxDepth = a.n; return D_TARGET; }
        st.viewport.minDepth = a.n; st.viewport.maxDepth = a.f; return D_TARGET; }
    case OP_BIND_PROGRAM: { p_bind_program a; if (!rd(a, p, n)) return D_BAD; return prog(a.prog, a.stages) ? D_PROG : D_BAD; }
    case OP_BIND_STATE: { p_bind_state a; if (!rd(a, p, n)) return D_BAD;
        switch (a.kind) {
        case 0: { blend_st b; memcpy(&b, a.bytes, sizeof(b)); st.blend[b.target] = b; return D_BLEND; }
        case 1: memcpy(&st.color, a.bytes, sizeof(st.color)); return D_BLEND;
        case 2: memcpy(&st.mask, a.bytes, sizeof(st.mask)); return D_BLEND;
        case 3: memcpy(&st.ms, a.bytes, sizeof(st.ms)); return D_BLEND;
        case 4: memcpy(&st.poly, a.bytes, sizeof(st.poly)); return D_DEPTH;
        case 5: memcpy(&st.ds, a.bytes, sizeof(st.ds)); return D_DEPTH;
        default: return D_BAD; } }
    case OP_BIND_VATTR: { p_bind_vattr a; rdv(a, p, n);
        if (!(a.first == 0 && a.n >= 0 && a.n <= 16 && n >= offsetof(p_bind_vattr, st) + size_t(a.n) * 4)) return D_BAD;
        return vattr(st.attrib, reinterpret_cast<const uint32_t *>(a.st), a.n) == 0 ? D_PROG : D_BAD; }
    case OP_BIND_VSTREAM: { p_bind_vstream a; rdv(a, p, n);
        if (!(a.first >= 0 && a.n >= 0 && a.first + a.n <= 16 && n >= offsetof(p_bind_vstream, st) + size_t(a.n) * sizeof(vs_st))) return D_BAD;
        memcpy(st.stream + (NRZ8_MUTATE == 4 ? 0 : a.first), a.st, a.n * sizeof(vs_st)); return D_PROG; }
    case OP_BIND_VBUF: { p_bind_vbuf a; rdv(a, p, n);
        if (!(a.first >= 0 && a.n >= 0 && a.first + a.n <= 16 && n >= offsetof(p_bind_vbuf, r) + size_t(a.n) * sizeof(NVNbufferRange))) return D_BAD;
        memcpy(st.vb + a.first, a.r, a.n * sizeof(NVNbufferRange)); return D_VB; }
    case OP_BIND_UBO: case OP_BIND_SSBO: { p_bind_buf a; if (!rd(a, p, n) || !(a.stage >= 0 && a.stage < 6 && a.index >= 0 && a.index < 16)) return D_BAD;
        (op == OP_BIND_UBO ? st.ub : st.sb)[a.stage][a.index] = {a.addr, a.size}; return op == OP_BIND_UBO ? D_UB : D_STORE; }
    case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: { p_bind_handles a; rdv(a, p, n);
        if (!(a.stage >= 0 && a.stage < 6 && a.first >= 0 && a.n >= 0 && a.n <= 32 && a.first + a.n <= (op == OP_BIND_IMG ? 16 : 128) && n >= offsetof(p_bind_handles, h) + size_t(a.n) * 8)) return D_BAD;
        uint64_t *dst = op == OP_BIND_IMG ? st.img[a.stage] : op == OP_BIND_SAMP ? st.samp[a.stage] : st.tex[a.stage];
        memcpy(dst + a.first, a.h, a.n * 8); return op == OP_BIND_IMG ? D_STORE : D_TEX; }
    case OP_STENCIL: { p_stencil a; if (!rd(a, p, n) || !(a.face >= 0 && a.face <= 3 && a.which >= 0 && a.which <= 2)) return D_BAD;
        for (unsigned i = 0; i < 2; i++) if ((a.face & (1 << i)) || NRZ8_MUTATE == 3) { if (a.which == 0) st.stencil_compare[i] = a.value_mask; else if (a.which == 1) st.stencil_write[i] = a.mask; else st.stencil_ref[i] = a.ref; }
        return D_DEPTH; }
    case OP_POLY_OFFSET: { p_poly_offset a; if (!rd(a, p, n)) return D_BAD; st.bias[0] = a.factor; st.bias[1] = a.units; st.bias[2] = a.clamp; return D_DEPTH; }
    case OP_SET_TEX_POOL: { p_set_pool a; if (!rd(a, p, n) || !a.pool) return D_BAD; st.texture_pool = *a.pool; return D_TARGET; }
    case OP_SET_SAMP_POOL: { p_set_pool a; if (!rd(a, p, n) || !a.pool) return D_BAD; st.sampler_pool = *a.pool; return D_TARGET; }
    case OP_COPY_STRIDE: { p_copy_stride a; if (!rd(a, p, n) || a.stride < 0) return D_BAD; st.copy_stride = a.stride; return D_OTHER; }
    case OP_COPY_IMAGE_STRIDE: { p_copy_stride a; if (!rd(a, p, n) || a.stride < 0) return D_BAD; st.copy_image_stride = a.stride; return D_OTHER; }
    case OP_BLEND_COLOR: { p_blend_color a; if (!rd(a, p, n)) return D_BAD; memcpy(st.blend_color, a.rgba, 16); return D_BLEND; }
    case OP_X_PATCH_SIZE: { uint32_t size; if (!rd(size, p, n) || !size) return D_BAD; st.patch_size = size; return D_DEPTH; }
    case OP_X_DEPTH_BOUNDS: { xbg_depth_bounds a; if (!rd(a, p, n)) return D_BAD; st.bounds = a; return D_DEPTH; }
    case OP_X_PRIM_RESTART: { p_prim_restart a; if (!rd(a, p, n)) return D_BAD; st.restart = a; return D_DEPTH; }
    case OP_X_DEPTH_CLAMP: { p_flag a; if (!rd(a, p, n)) return D_BAD; st.depth_clamp = a.on != 0; return D_DEPTH; }
    case OP_X_RAST_DISCARD: { p_flag a; if (!rd(a, p, n)) return D_BAD; st.rast_discard = a.on != 0; return D_DEPTH; }
    case OP_X_ALPHA_REF: { p_alpha_ref a; if (!rd(a, p, n)) return D_BAD; st.alpha_ref = a.ref; return D_BLEND; }
    default: return 0;
    }
}
// The ops fold() knows (the walker's "a state record": it ends no run and is applied; anything else ends the run in hand).
static inline bool is_state_op(uint32_t op) {
    switch (op) {
    case OP_SET_RT: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM:
    case OP_BIND_VBUF: case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL: case OP_POLY_OFFSET:
    case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_COPY_STRIDE: case OP_COPY_IMAGE_STRIDE: case OP_BLEND_COLOR: case OP_X_PATCH_SIZE: case OP_X_DEPTH_BOUNDS:
    case OP_X_PRIM_RESTART: case OP_X_DEPTH_CLAMP: case OP_X_RAST_DISCARD: case OP_X_ALPHA_REF: return true;
    default: return false;
    }
}
// The dirty bits of an OP_X_NR_STATE record (nr_state3.h), from its header: the parts NR3_DEFINE_APPLY_RAW writes. A header bit this
// list does not know marks everything.
static inline unsigned dirty_of_nr3(const uint8_t *p, size_t n) {
    nr3_hdr h; if (n < sizeof h) return D_BAD; memcpy(&h, p, sizeof h);
    unsigned d = 0;
    if (h.nprog || (h.have & (NR3_H_VATTR | NR3_H_VSTREAM))) d |= D_PROG;
    if (h.have & (NR3_H_DS | NR3_H_SVMASK | NR3_H_SMASK | NR3_H_SREF | NR3_H_POLY)) d |= D_DEPTH;
    if (h.blend || (h.have & (NR3_H_COLOR | NR3_H_ALPHA))) d |= D_BLEND;
    if (h.vb) d |= D_VB;
    if (h.have & ~unsigned(NR3_H_ALL)) d |= D_PROG | D_DEPTH | D_BLEND | D_VB;
    return d;
}

// ---- the walk: the records of one command handle in execution order -------------------------------------------------------------
// H:  void state_op(uint32_t op, const uint8_t *p, size_t n)     a record fold() knows (also inside a state block / a called handle)
//     void nr_state(const uint8_t *p, size_t n)                  OP_X_NR_STATE
//     void nb_draw(uint8_t *rec, size_t n, bool top)             OP_X_NB_DRAW payload (writable: the marks); top: reached without OP_CALL
//     void other(uint32_t op)                                    any other record (it may write packets or state the fold does not hold)
// The dictionary of OP_X_NB_STATE's DEF blocks is the backend's (xbg_nb_state.inc nbstate::defs): a REF executes the content of the
// DEF of its (kind, id) in the same epoch, a REF without one is skipped.
struct Def { uint32_t epoch; const uint8_t *p; };
template <class H> struct Walker {
    Def *defs = nullptr; H *h = nullptr; uint64_t ref_miss = 0, records = 0;
    bool init() { defs = static_cast<Def *>(calloc(size_t(NBS_KINDS) * NBS_MAX_IDS, sizeof(Def))); return defs != nullptr; }
    void destroy() { free(defs); defs = nullptr; }
    // false: a record the walk cannot follow (the copy is no longer the State of the translation thread).
    bool walk(const uint8_t *pc, unsigned depth, uint64_t &budget, bool top) {
        if (depth >= 64 || !pc) return false;
        for (;;) {
            if (budget++ >= 10000000) return false;
            op_hdr hd; memcpy(&hd, pc, 8);
            if (hd.size < 8 || (hd.size & 7) || hd.size > (64u << 20)) return false;
            const uint8_t *p = pc + 8; const size_t n = hd.size - 8; ++records;
            switch (hd.op) {
            case OP_END: return true;
            case OP_BEGIN: break;
            case OP_JUMP: { uint64_t t; if (!rd(t, p, n)) return false; if (NRZ8_MUTATE == 12) break; pc = reinterpret_cast<const uint8_t *>(uintptr_t(t)); if (!pc) return false; continue; }
            case OP_CALL: { uint64_t t; if (!rd(t, p, n)) return false; h->other(hd.op);
                if (NRZ8_MUTATE != 2 && !walk(reinterpret_cast<const uint8_t *>(uintptr_t(t)), depth + 1, budget, false)) return false;
                h->other(hd.op); break; }
            case OP_X_NATIVE_DRAW: h->other(hd.op); if (!walk(p, depth + 1, budget, false)) return false; h->other(hd.op); break;
            case OP_X_NB_STATE: { nbs_hdr sh; nbs_view v[NBS_KINDS]; const int nb = nbs_parse(p, n, &sh, v); if (nb <= 0) return false;
                for (int i = 0; i < nb; i++) { const nbs_view &b = v[i]; const uint8_t *c = b.content;
                    Def &d = defs[size_t(b.b.kind) * NBS_MAX_IDS + b.b.id];
                    if (b.b.how == NBS_B_REF) { if ((d.epoch != sh.epoch && NRZ8_MUTATE != 1) || !d.p) { ++ref_miss; continue; } c = d.p; }
                    else if (b.b.how == NBS_B_DEF) d = Def{sh.epoch, c};
                    if (!walk(c, depth + 1, budget, false)) return false; }
                break; }
            case OP_X_NR_STATE: h->nr_state(p, n); break;
            case OP_X_NB_DRAW: h->nb_draw(const_cast<uint8_t *>(p), n, top); break;
            default: if (is_state_op(hd.op)) h->state_op(hd.op, p, n); else h->other(hd.op); break;
            }
            pc += hd.size;
        }
    }
};

// ---- Dict: content -> id, compared byte for byte ----------------------------------------------------------------------------
// Ids count up for the life of the dictionary and are never handed out twice: when the table or the arena is full it is emptied (the
// old ids are simply never answered again). One thread.
static inline uint64_t hash_bytes(const void *p, size_t n) {
    const uint8_t *b = static_cast<const uint8_t *>(p); uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t(n) * 0xff51afd7ed558ccdull);
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0xBF58476D1CE4E5B9ull; h ^= h >> 29; }
    if (n) { uint64_t w = 0; memcpy(&w, b, n); h = (h ^ w) * 0xBF58476D1CE4E5B9ull; h ^= h >> 29; }
    return h ^ h >> 32;
}
// [section 39, the walker's cost] The dictionaries' hash: four independent lanes of 8 bytes (hash_bytes is one chain of multiplies -
// 30 of them in a row for a group of 240 bytes). Equal ids still mean equal bytes: the dictionary compares the bytes.
static inline uint64_t hash_fast(const void *p, size_t n) {
    const uint8_t *b = static_cast<const uint8_t *>(p); const uint64_t M = 0xBF58476D1CE4E5B9ull;
    uint64_t h0 = 0x9E3779B97F4A7C15ull ^ (uint64_t(n) * 0xff51afd7ed558ccdull), h1 = 0xC2B2AE3D27D4EB4Full, h2 = 0x165667B19E3779F9ull, h3 = 0x27D4EB2F165667C5ull;
    for (; n >= 32; n -= 32, b += 32) { uint64_t w[4]; memcpy(w, b, 32); h0 = (h0 ^ w[0]) * M; h1 = (h1 ^ w[1]) * M; h2 = (h2 ^ w[2]) * M; h3 = (h3 ^ w[3]) * M;
        h0 ^= h0 >> 29; h1 ^= h1 >> 29; h2 ^= h2 >> 29; h3 ^= h3 >> 29; }
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h0 = (h0 ^ w) * M; h0 ^= h0 >> 29; }
    if (n) { uint64_t w = 0; memcpy(&w, b, n); h1 = (h1 ^ w) * M; h1 ^= h1 >> 29; }
    uint64_t h = (h0 ^ (h1 * 0x9E3779B97F4A7C15ull)) * M; h ^= (h2 ^ (h3 * 0xff51afd7ed558ccdull)) * M; h ^= h >> 31;
    return h ^ h >> 32;
}
struct Dict {
    struct E { uint64_t h; uint32_t off, size, id, used; };
    E *e = nullptr; uint8_t *arena = nullptr; uint32_t cap = 0, n = 0, acap = 0, aused = 0, next_id = 1; uint64_t clears = 0, finds = 0, news = 0;
    bool init(uint32_t entries, uint32_t bytes) {
        while (entries & (entries - 1)) entries &= entries - 1;
        if (!entries) return false;
        e = static_cast<E *>(calloc(entries, sizeof(E))); arena = static_cast<uint8_t *>(malloc(bytes));
        if (!e || !arena) { free(e); free(arena); e = nullptr; arena = nullptr; return false; }
        cap = entries; acap = bytes; return true;
    }
    void destroy() { free(e); free(arena); e = nullptr; arena = nullptr; cap = 0; }
    void clear() { memset(e, 0, size_t(cap) * sizeof(E)); n = 0; aused = 0; ++clears; }
    // 0 = the content is larger than the arena (never an id).
    uint32_t id_of(const void *p, uint32_t size) {
        if (size > acap) return 0;
        const uint64_t hh = NRZ8_MUTATE == 5 ? (hash_fast(p, size) & 15u) : hash_fast(p, size);
        for (int round = 0; round < 2; round++) {
            uint32_t i = uint32_t(hh) & (cap - 1);
            for (;; i = (i + 1) & (cap - 1)) {
                E &x = e[i];
                if (!x.used) break;
                if (x.h == hh && x.size == size && (NRZ8_MUTATE == 5 || !memcmp(arena + x.off, p, size))) { ++finds; return x.id; }
            }
            if (n * 2 < cap && aused + size <= acap) {
                E &x = e[i]; x.h = hh; x.off = aused; x.size = size; x.id = next_id++; x.used = 1; memcpy(arena + aused, p, size); aused += (size + 7u) & ~7u; ++n; ++news;
                if (!next_id) next_id = 1;   // (2^32 distinct contents: not in a session)
                return x.id;
            }
            clear();
        }
        return 0;
    }
};

// The five groups of the fix part, serialized (whole fields, in a fixed order). sprog: the program word of each stage.
// buf: room for GROUP_MAX bytes. Returns the bytes written.
enum { GROUP_MAX = 2560 };
template <class S> static inline uint32_t group_bytes(int g, const S &st, const uint64_t *sprog, uint8_t *buf) {
    uint8_t *q = buf;
    auto put = [&q](const void *p, size_t n) { memcpy(q, p, n); q += n; };
    switch (g) {
    case 0: { put(&st.rt, sizeof st.rt); put(&st.viewport, sizeof st.viewport); put(&st.scissor, sizeof st.scissor);
        const uint64_t w[6] = {uint64_t(uintptr_t(st.texture_pool.pool)), uint64_t(st.texture_pool.off), uint64_t(uint32_t(st.texture_pool.n)),
                               uint64_t(uintptr_t(st.sampler_pool.pool)), uint64_t(st.sampler_pool.off), uint64_t(uint32_t(st.sampler_pool.n))};
        put(w, sizeof w); break; }
    case 1: put(sprog, 6 * sizeof(uint64_t)); put(st.attrib, sizeof st.attrib); put(st.stream, sizeof st.stream); break;
    case 2: put(st.blend, sizeof st.blend); put(&st.color, sizeof st.color); put(&st.mask, sizeof st.mask); put(&st.ms, sizeof st.ms);
        put(st.blend_color, sizeof st.blend_color); put(&st.alpha_ref, sizeof st.alpha_ref); break;
    case 3: put(&st.ds, sizeof st.ds); put(&st.poly, sizeof st.poly); put(st.bias, sizeof st.bias); put(st.stencil_compare, sizeof st.stencil_compare);
        put(st.stencil_write, sizeof st.stencil_write); put(st.stencil_ref, sizeof st.stencil_ref); put(&st.bounds, sizeof st.bounds); put(&st.restart, sizeof st.restart);
        put(&st.depth_clamp, sizeof st.depth_clamp); put(&st.rast_discard, sizeof st.rast_discard); put(&st.patch_size, sizeof st.patch_size); break;
    default: put(st.sb, sizeof st.sb); put(st.img, sizeof st.img); break;
    }
    return uint32_t(q - buf);
}
// The fix id of the copy: group ids refreshed for the groups the fold marked, then the id of the five.
struct Fix {
    Dict g[GROUPS], tuple; uint32_t gid[GROUPS] = {}, id = 0; unsigned dirty = (1u << GROUPS) - 1; bool ok = false; uint64_t refreshed = 0, asked = 0;
    bool init() { ok = true; for (auto &d : g) ok = d.init(4096, 1u << 20) && ok; ok = tuple.init(16384, 1u << 20) && ok; return ok; }
    void destroy() { for (auto &d : g) d.destroy(); tuple.destroy(); ok = false; }
    void mark(unsigned d) { dirty |= d & ((1u << GROUPS) - 1); }
    template <class S> uint32_t get(const S &st, const uint64_t *sprog) {
        ++asked;
        if (!dirty || NRZ8_MUTATE == 6) { if (id) return id; }
        uint8_t buf[GROUP_MAX]; bool moved = !id;
        for (unsigned m = dirty; m; m &= m - 1) { const int k = __builtin_ctz(m); const uint32_t n = group_bytes(k, st, sprog, buf); const uint32_t x = g[k].id_of(buf, n); ++refreshed;
            if (x != gid[k]) { gid[k] = x; moved = true; } }
        dirty = 0;
        if (moved) id = tuple.id_of(gid, sizeof gid);
        return id;
    }
    // (verify) the id from scratch: every group serialized again.
    template <class S> uint32_t fresh(const S &st, const uint64_t *sprog) {
        uint8_t buf[GROUP_MAX]; uint32_t x[GROUPS];
        for (int k = 0; k < GROUPS; k++) { const uint32_t n = group_bytes(k, st, sprog, buf); x[k] = g[k].id_of(buf, n); }
        return tuple.id_of(x, sizeof x);
    }
};

// ---- the template of a slot: the values (as nr_z5p.h) + the words the resolution read, packed as 64-bit words ---------------------
// MAX_DYN: the dynamic offsets of a set - the game's uniform blocks (offsets into the constant ring) AND the support / c1 blocks the
// layout binds as dynamic buffers (their offset is the NR-08 entry's and does not move with the draw). 16 = the device limit the
// plans are held to on Adreno; a Fort Joy frame binds 7.1 a set (4.2 ring blocks + the fixed ones).
enum { MAX_DYN = 16, MAX_VB = NRZ5T_MAX_VB, MAX_ROWS = 12, HEAD_WORDS = 19, MAX_WORDS = HEAD_WORDS + MAX_DYN * 3 + MAX_VB * 5 + MAX_ROWS * 3 };
enum : uint64_t { DYN_FIXED = 1ull << 16 };   // word 1 of a dynamic offset: not a ring block - word 0 is the offset itself
// word 18 of a template: k | used << 32 | offsets << 48 (5 bits) | vertex bindings << 53 (4) | texture rows << 57 (4) | indexed << 61
static inline unsigned w18_noff(uint64_t w) { return unsigned(w >> 48 & 31); }
static inline unsigned w18_nvb(uint64_t w) { return unsigned(w >> 53 & 15); }
static inline unsigned w18_nrow(uint64_t w) { return unsigned(w >> 57 & 15); }
static inline bool w18_indexed(uint64_t w) { return (w >> 61 & 1) != 0; }
// What the translation thread describes of a draw it recorded itself.
struct Desc {
    nrz5_site site; uint32_t fixid = 0;
    const nrz5_vals *v = nullptr; uint32_t first_index = 0, k = 0;           // the values its tail bound; the folds
    unsigned noff = 0; struct Dyn { uint8_t st = 0, sl = 0, fixed = 0; uint64_t addr = 0; uint32_t rbytes = 0, copy = 0; } dyn[MAX_DYN];   // dynamic offset i: the uniform slot, the address bound, the row's bytes, the bytes served; fixed: a support / c1 block - the offset is v->off[i] as it stands
    unsigned nvb = 0; struct { uint8_t at; uint64_t addr, size; } vb[MAX_VB];                            // each used binding: the range bound
    unsigned nrow = 0; struct { uint8_t ns, slot, samp; uint64_t tex, smp; } row[MAX_ROWS];              // each texture row of the plan: handles bound
};
// 0 = the draw cannot be a template (an offset above its address).
static inline unsigned pack(const Desc &d, uint64_t *w) {
    const nrz5_vals &v = *d.v;
    if (d.noff > MAX_DYN || d.nvb > MAX_VB || d.nrow > MAX_ROWS || v.noff != d.noff) return 0;
    unsigned n = HEAD_WORDS;
    memcpy(w + 1, d.site.w, 6 * 8);
    w[7] = v.pipe; w[8] = v.layout; w[9] = v.set; w[10] = v.indexed ? v.ib : 0; w[11] = v.indexed ? v.ibo : 0;
    memcpy(w + 12, v.vp, NRZ5_VP_BYTES); memcpy(w + 15, v.sc, NRZ5_SC_BYTES);
    w[17] = uint64_t(v.indexed ? v.it : 0u) | uint64_t(v.indexed ? d.first_index : 0u) << 32;
    w[18] = uint64_t(d.k) | uint64_t(v.used) << 32 | uint64_t(d.noff) << 48 | uint64_t(d.nvb) << 53 | uint64_t(d.nrow) << 57 | uint64_t(v.indexed ? 1 : 0) << 61;
    for (unsigned i = 0; i < d.noff; i++) {
        if (d.dyn[i].fixed) { w[n++] = v.off[i]; w[n++] = DYN_FIXED; w[n++] = 0; continue; }
        if (!d.dyn[i].addr || d.dyn[i].addr < v.off[i]) return 0;
        w[n++] = d.dyn[i].addr - v.off[i]; w[n++] = uint64_t(d.dyn[i].st) | uint64_t(d.dyn[i].sl) << 8 | uint64_t(d.dyn[i].copy) << 32; w[n++] = d.dyn[i].rbytes; }
    for (unsigned j = 0; j < d.nvb; j++) { const unsigned at = d.vb[j].at & 15u; w[n++] = at; w[n++] = v.vb[at]; w[n++] = v.vbo[at]; w[n++] = d.vb[j].addr; w[n++] = d.vb[j].size; }
    for (unsigned r = 0; r < d.nrow; r++) { w[n++] = uint64_t(d.row[r].ns) | uint64_t(d.row[r].slot) << 8 | uint64_t(d.row[r].samp) << 16; w[n++] = d.row[r].tex; w[n++] = d.row[r].smp; }
    w[0] = uint64_t(n) | uint64_t(1) << 16 | uint64_t(d.fixid) << 32;
    return n;
}
static inline unsigned words_of(uint64_t w0) { return unsigned(w0 & 0xffffu); }
// The walker's outcome for a draw record. M_OK: the template's answer and the draw's values (packets are written ahead); M_NOVAL: the
// answer, not the values - a vertex range differs from the template's (NR-ZERO.md section 39: the answer - plan, NR-08 entry, NR-09
// entry - never reads a vertex range; the values do, and the translation thread resolves them again at the moment); the rest are
// refusals: another site / another fix id in the way, texture / sampler handles of a row, a uniform slot or size, a template that says
// "not describable" (form). M_EMPTY .. M_CALLED: outcomes before a template is asked (the census of the line "[nr] z8 census").
enum { M_OK = 0, M_SITE, M_FIX, M_TEX, M_NOVAL, M_UBO, M_FORM, M_N, M_EMPTY = M_N, M_NOKEY, M_CALLED, M_ALL };
static inline bool answered(int m) { return m == M_OK || m == M_NOVAL; }
// The submitting thread: is this template the one of the draw it is looking at, and then its values. A: the copy -
//   bool ub(unsigned st, unsigned sl, uint64_t *addr, uint64_t *size)   uint64_t tex(unsigned ns, unsigned slot)
//   bool vb(unsigned i, uint64_t *addr, uint64_t *size)                 uint64_t samp(unsigned ns, unsigned slot)
// off: room for MAX_DYN offsets (v->off points at it). M_OK: every compared word is equal and v holds the draw's values. M_NOVAL:
// every word that picks the answer is equal, a vertex range is not - v must not be written ahead (its vertex fields are the template's).
template <class A> static inline int match(const uint64_t *w, const nrz5_site *s, uint32_t fixid, int32_t count, int32_t instances, int32_t first, int32_t base_vertex,
                                           int32_t base_instance, A &&a, nrz5_vals *v, uint32_t *off) {
    const unsigned nw = words_of(w[0]);
    if (!(w[0] >> 16 & 1) || nw < HEAD_WORDS || nw > MAX_WORDS) return M_FORM;
    if (memcmp(w + 1, s->w, 6 * 8)) return M_SITE;
    if (NRZ8_MUTATE != 7 && uint32_t(w[0] >> 32) != fixid) return M_FIX;
    const unsigned noff = w18_noff(w[18]), nvb = w18_nvb(w[18]), nrow = w18_nrow(w[18]);
    if (noff > MAX_DYN || nvb > MAX_VB || nrow > MAX_ROWS || nw != HEAD_WORDS + noff * 3 + nvb * 5 + nrow * 3) return M_FORM;
    // (every field but the vertex arrays; of those only the used bindings are ever read - nrz5_emit / nrz5_after / pv_put / ranges_word)
    memset(v, 0, offsetof(nrz5_vals, vb)); memset(reinterpret_cast<uint8_t *>(v) + offsetof(nrz5_vals, indexed), 0, sizeof *v - offsetof(nrz5_vals, indexed));
    const uint64_t *q = w + HEAD_WORDS;
    v->pipe = w[7]; v->layout = w[8]; v->set = w[9]; v->noff = noff; v->off = off;
    for (unsigned i = 0; i < noff; i++, q += 3) {
        uint64_t addr = 0, size = 0;
        if (q[1] & DYN_FIXED) { off[i] = NRZ8_MUTATE == 28 ? 0u : uint32_t(q[0]); continue; }   // a support / c1 block: the entry's offset
        if (!a.ub(unsigned(q[1] & 255), unsigned(q[1] >> 8 & 255), &addr, &size) || !addr || !size) return M_UBO;
        const uint64_t lim = q[2] ? q[2] : 65536u, copy = size < lim ? size : lim;
        if (NRZ8_MUTATE != 10 && copy != (q[1] >> 32)) return M_UBO;
        if (addr < q[0] || addr - q[0] > 0xffffffffull) return M_UBO;
        off[i] = uint32_t(addr - q[0]);
    }
    v->used = uint16_t(w[18] >> 32); bool noval = false;
    for (unsigned j = 0; j < nvb; j++, q += 5) {
        const unsigned at = unsigned(q[0] & 15); uint64_t addr = 0, size = 0;
        if (!a.vb(at, &addr, &size)) return M_FORM;
        if (NRZ8_MUTATE != 9 && (addr != q[3] || size != q[4])) { if (NRZ8_MUTATE == 34) return M_UBO; noval = true; }   // (34: refused, as before section 39)
        v->vb[at] = q[1]; v->vbo[at] = q[2];
    }
    for (unsigned r = 0; r < nrow; r++, q += 3) {
        const unsigned ns = unsigned(q[0] & 255), slot = unsigned(q[0] >> 8 & 255), sm = unsigned(q[0] >> 16 & 255);
        if (NRZ8_MUTATE == 8) continue;
        if (a.tex(ns, slot) != q[1]) return M_TEX;
        if (NRZ8_MUTATE != 14 && (sm != 0xff ? a.samp(ns, sm) : 0) != q[2]) return M_TEX;
    }
    memcpy(v->vp, w + 12, NRZ5_VP_BYTES); memcpy(v->sc, w + 15, NRZ5_SC_BYTES);
    v->indexed = w18_indexed(w[18]); v->count = uint32_t(count);
    const uint32_t k = uint32_t(w[18]);
    if (v->indexed) { v->it = uint32_t(w[17]); v->ib = w[10]; v->ibo = w[11]; v->instances = uint32_t(instances); v->first = uint32_t(w[17] >> 32); v->vertex_offset = int32_t(uint32_t(base_vertex) + k); v->first_instance = uint32_t(base_instance); }
    else { v->instances = 1; v->first = uint32_t(first) + k; v->first_instance = 0; }
    return noval ? M_NOVAL : M_OK;
}

// [NR-ZERO Z8 values, NR-ZERO.md section 31] The values the walk wrote a draw's packets from are match()'s: the words of the
// template, the uniform addresses of the copy, the arguments of the record. The translation thread holds the same template (its
// own slot at the version the record names) and the same words of State, so it can compare the values it resolved at the moment
// with them WITHOUT reading what the walker wrote. Wd(j): word j of the slot's template; a.ub: State's uniform ranges.
// 0: the same; else the code nr_z5r.h pv_diff gives the first field that differs (1 pipeline, 2 set / layout, 3 dynamic offsets,
// 4 viewport, 5 scissor, 6 vertex streams, 7 index buffer, 8 draw arguments; 9: the words are not a template).
template <class Wd, class A> static inline int expect_diff(Wd &&W, const nrz5_vals *r, int32_t count, int32_t instances, int32_t first, int32_t base_vertex, int32_t base_instance, A &&a) {
    const uint64_t w0 = W(0), w18 = W(18); const unsigned nw = words_of(w0);
    const unsigned noff = w18_noff(w18), nvb = w18_nvb(w18), nrow = w18_nrow(w18);
    if (!(w0 >> 16 & 1) || noff > MAX_DYN || nvb > MAX_VB || nrow > MAX_ROWS || nw != HEAD_WORDS + noff * 3 + nvb * 5 + nrow * 3) return 9;
    if (NRZ8_MUTATE != 21 && r->pipe != W(7)) return 1;
    if (NRZ8_MUTATE != 22 && (r->layout != W(8) || r->set != W(9))) return 2;
    const bool indexed = w18_indexed(w18);
    if (r->noff != noff) return 3;
    if (r->used != uint16_t(w18 >> 32)) return 6;
    if (r->indexed != indexed) return 8;
    unsigned q = HEAD_WORDS;
    for (unsigned i = 0; i < noff; i++, q += 3) {
        uint64_t addr = 0, size = 0; const uint64_t q0 = W(q), q1 = W(q + 1);
        if (q1 & DYN_FIXED) { if (NRZ8_MUTATE != 29 && r->off[i] != uint32_t(q0)) return 3; continue; }
        if (!a.ub(unsigned(q1 & 255), unsigned(q1 >> 8 & 255), &addr, &size) || addr < q0 || addr - q0 > 0xffffffffull) return 3;
        if (NRZ8_MUTATE != 23 && r->off[i] != uint32_t(addr - q0)) return 3;
    }
    if (NRZ8_MUTATE != 24) { const uint64_t vp[3] = {W(12), W(13), W(14)}, sc[2] = {W(15), W(16)}; if (memcmp(r->vp, vp, NRZ5_VP_BYTES)) return 4; if (memcmp(r->sc, sc, NRZ5_SC_BYTES)) return 5; }
    for (unsigned j = 0; j < nvb; j++, q += 5) { const unsigned at = unsigned(W(q) & 15); if (NRZ8_MUTATE != 25 && (r->vb[at] != W(q + 1) || r->vbo[at] != W(q + 2))) return 6; }
    const uint32_t k = uint32_t(w18);
    if (NRZ8_MUTATE == 26) return 0;
    if (indexed) { const uint64_t w17 = W(17);
        if (r->ib != W(10) || r->ibo != W(11) || r->it != uint32_t(w17)) return 7;
        if (r->count != uint32_t(count) || r->instances != uint32_t(instances) || r->first != uint32_t(w17 >> 32) || r->first_instance != uint32_t(base_instance) || r->vertex_offset != int32_t(uint32_t(base_vertex) + k)) return 8; }
    else if (r->count != uint32_t(count) || r->instances != 1 || r->first != uint32_t(first) + k || r->first_instance != 0) return 8;
    return 0;
}

// ---- what the translation thread can describe of a draw it recorded itself (nrz8t::publish; NR-ZERO.md section 36) ---------------
// The reasons a recorded draw is not a template (the numbers of the line "[nr] z8, translation thread: ... not describable").
enum { P_OK = 0, P_NOT_SERVED = 1, P_STATIC = 2, P_MANY = 3, P_ENTRY = 4, P_GUARD = 5, P_STAGES = 6, P_OFFSET = 7, P_N = 8 };
// The plan of the draw's shader set (nr_desc.h) against the values its tail bound. A template can carry: every ring block as a
// dynamic offset (RING_DYN) and every support / c1 block either as a dynamic buffer whose offset is the entry's (FIXED_DYN - what a
// device with dynamic uniform buffers on gives EVERY set of the game) or baked into the set (FIXED_STAT). It cannot carry a ring
// block baked into the set (RING_STAT, DM_NR_DESC_SPLIT: the set itself depends on the block's offset).
static inline int plan_form(const nr_dplan &p, const nrz5_vals &v) {
    if (p.why) return P_NOT_SERVED;
    if (NRZ8_MUTATE == 27 && p.nubo != p.ndyn) return P_STATIC;
    if (p.nstat || p.ndyn != v.noff) return P_STATIC;
    if (p.nimg > MAX_ROWS || v.noff > MAX_DYN) return P_MANY;
    return P_OK;
}
// Fills d (dynamic offsets, vertex ranges, texture rows; the caller sets site, fixid, first_index, k) from the plan, the values and
// the translation thread's State. copy[u]: the bytes NR-08 serves of ring block u (the plan's). A: as match()'s. P_OK or the reason.
template <class A> static inline int describe(const nr_dplan &p, const uint32_t *copy, const nrz5_vals &v, A &&a, Desc &d) {
    if (const int w = plan_form(p, v)) return w;
    d.v = &v; d.noff = v.noff; uint32_t seen = 0;
    for (unsigned i = 0; i < d.noff; i++) d.dyn[i] = Desc::Dyn{};
    for (unsigned u = 0; u < p.nubo; u++) {
        const nr_drow &r = p.row[p.ubo_row[u]]; uint64_t addr = 0, size = 0;
        if (r.kind != NRD_K_RING_DYN || r.dyn_at >= v.noff || (seen >> r.dyn_at & 1) || !a.ub(unsigned(r.ns), unsigned(r.slot), &addr, &size)) return P_STATIC;
        seen |= 1u << r.dyn_at; Desc::Dyn &x = d.dyn[r.dyn_at]; x.st = r.ns; x.sl = r.slot; x.addr = addr; x.rbytes = r.bytes; x.copy = copy[u];
    }
    for (unsigned i = 0; i < p.nrow; i++) {
        const nr_drow &r = p.row[i]; if (r.kind != NRD_K_FIXED_DYN) continue;
        if (r.dyn_at >= v.noff || (seen >> r.dyn_at & 1)) return P_STATIC;
        seen |= 1u << r.dyn_at; d.dyn[r.dyn_at].fixed = 1;
    }
    if (seen != (1u << v.noff) - 1u) return P_STATIC;   // (every offset of the set is one of the two kinds; v.noff <= MAX_DYN)
    unsigned nvb = 0;
    for (uint32_t m = v.used; m; m &= m - 1) {
        const uint32_t i = uint32_t(__builtin_ctz(m)); uint64_t addr = 0, size = 0;
        if (nvb >= MAX_VB || !a.vb(i, &addr, &size)) return P_MANY;
        d.vb[nvb].at = uint8_t(i); d.vb[nvb].addr = addr; d.vb[nvb].size = size; ++nvb;
    }
    d.nvb = nvb; d.nrow = p.nimg;
    for (unsigned i = 0; i < p.nimg; i++) {
        const nr_drow &r = p.row[p.img_row[i]];
        d.row[i].ns = r.ns; d.row[i].slot = r.slot; d.row[i].samp = r.samp; d.row[i].tex = a.tex(r.ns, r.slot); d.row[i].smp = r.samp != 0xff ? a.samp(r.ns, r.samp) : 0;
    }
    return P_OK;
}

// ---- what the prologue of the translation thread's draw() did (NR-ZERO.md section 39) ----------------------------------------------
// PRO_NONE: nothing - State at the hook / the lane's tail is the walk's copy. PRO_TWIN: only the fragment shader is a twin (NR-MAT's
// empty fragment shader in a pass without colour, NR-SH-SURF's variant of a surface shader; their Guard puts the game's shader back
// after the draw): every other word of State is the copy's. PRO_STATE: anything else (the pre-pass override, LIGHTFUSE, ONCHIP's
// twin that steps a subpass, a post fusion) - State is not the copy.
// A twin draw is published and answered: its answer (the plan and NR-08 entry of the twin's shader set, the NR-09 entry of its variant)
// was resolved under the shaders the prologue left, and it is taken only when the shaders at the hook are those (same_shaders) - a
// prologue that decides otherwise this time (NR-MAT's gate reads a uniform block; another NR-SH-SURF variant) is refused there.
enum { PRO_NONE = 0, PRO_TWIN = 1, PRO_STATE = 2 };
static inline bool prologue_ok(int pro, int twin) { return pro == PRO_NONE || (pro == PRO_TWIN && twin) || (NRZ8_MUTATE == 32 && pro == PRO_STATE); }
// a[i] / b[i]: the shader of stage i the answer was resolved for / that State holds now; the fragment stage is the last of the n.
template <class P> static inline bool same_shaders(const P *const *a, const P *const *b, unsigned n) {
    for (unsigned i = 0; i < n; i++) if (a[i] != b[i] && !(NRZ8_MUTATE == 33 && i + 1 == n)) return false;
    return true;
}

// [DM_NR_Z8_THREAD, NR-ZERO.md sections 28 and 36] Does the walk get its own thread? want: the switch (0 / 1). nb_own: DM_NB_OWN
// (the game's default: a frame's last submission is queued as the frame's open job). It does not matter: an open job is numbered
// by the queue exactly as any submission (hle_nvn.c x_push: the next number, under the lock the post is made under - the front
// compares the number it announced with the number the job got at every submission, xbg_nr_z8_sub_end).
static inline int walker_thread(int want, int nb_own) { if (NRZ8_MUTATE == 30 && nb_own) return 0; return want == 1 ? 1 : 0; }

// One writer (the translation thread), readers validate with the slot's version (as nr_z5p.h: 0 = never written, odd = being
// written); every word an atomic. A reader copies only the words the template has.
// [NR-ZERO.md section 39] A bucket of WAYS slots (slot_of gives its first): a draw's template lives in any of its ways; the walker asks
// the ways for its site words and fix id (find), the translation thread picks the way it writes (pick: the way of the same draw, an
// empty way, else the way used longest ago). The phone series R8 had one slot a site: 3.8 % of the draw records met another site's
// template there (the census world: 2 ways 96.4 % answered, 4 ways 99.0 %). Words are loaded / stored relaxed between the version's acquire / release and a fence (a seqlock as Boehm's
// "Can seqlocks get along with programming language memory models?"): one barrier a template instead of one a word.
// "none" templates (not describable) carry the site and the fix id too: the walker tells "this draw is not describable" (M_FORM)
// from "another draw is in the way" (M_SITE).
enum { WAYS = 4 };   // (a power of two)
struct alignas(64) Slot { std::atomic<uint32_t> ver; std::atomic<uint64_t> w[MAX_WORDS]; };   // (the version and the head - site words, fix id - in the first cache line)
static_assert(sizeof(Slot) % 64 == 0 && offsetof(Slot, w) + 7 * 8 <= 64, "nr_z8.h: a slot's head is one cache line");
static inline unsigned none_of(const nrz5_site &site, uint32_t fixid, uint64_t *w) {
    memset(w, 0, HEAD_WORDS * 8); w[0] = uint64_t(HEAD_WORDS) | uint64_t(fixid) << 32; memcpy(w + 1, site.w, 6 * 8); return HEAD_WORDS; }
struct Table {
    Slot *s = nullptr; uint32_t n = 0; uint64_t puts = 0, same = 0;
    bool init(uint32_t slots) {
        while (slots & (slots - 1)) slots &= slots - 1;
        if (!slots) return false;
        void *m = nullptr; if (posix_memalign(&m, 64, sizeof(Slot) * size_t(slots)) || !m) return false;
        s = static_cast<Slot *>(m); n = slots;
        for (uint32_t i = 0; i < n; i++) { new (&s[i]) Slot; s[i].ver.store(0, std::memory_order_relaxed); for (int j = 0; j < MAX_WORDS; j++) s[i].w[j].store(0, std::memory_order_relaxed); }
        return true;
    }
    void destroy() { free(static_cast<void *>(s)); s = nullptr; n = 0; }
    // the bucket's first way (by the site words and the fix id: one site under several fix ids - the cascades of a shadow, say - is
    // spread over buckets; by the site alone the host test's world lost a fifth of its answers to full buckets)
    uint32_t slot_of(const nrz5_site *site, uint32_t fixid) const { return uint32_t(nrz5_site_hash(site, fixid) >> 20) & (n - 1) & ~uint32_t(WAYS - 1); }
    // writer: the words of a template (nw of them, pack()'s) into slot i unless it holds them. Returns the slot's version (even).
    uint32_t put(uint32_t i, const uint64_t *w, unsigned nw) {
        Slot &x = s[i & (n - 1)];
        const uint32_t v = x.ver.load(std::memory_order_relaxed);
        if (v) { bool eq = true; for (unsigned j = 0; j < nw && eq; j++) eq = x.w[j].load(std::memory_order_relaxed) == w[j]; if (eq) { ++same; return v; } }
        x.ver.store(v + 1, std::memory_order_relaxed); std::atomic_thread_fence(std::memory_order_release);   // (odd before any word)
        for (unsigned j = 0; j < nw; j++) x.w[j].store(w[j], std::memory_order_relaxed);
        uint32_t nv = v + 2; if (!nv) nv = 2;
        x.ver.store(nv, std::memory_order_release); ++puts;
        return nv;
    }
    // reader: the template of slot i as one consistent block (w: MAX_WORDS of room) and its version. false = empty, being written, or
    // written meanwhile.
    bool get(uint32_t i, uint64_t *w, uint32_t *ver) const {
        const Slot &x = s[i & (n - 1)];
        const uint32_t v0 = x.ver.load(std::memory_order_acquire);
        if (!v0 || (v0 & 1)) return false;
        w[0] = x.w[0].load(std::memory_order_relaxed);
        unsigned nw = words_of(w[0]); if (nw > MAX_WORDS) nw = MAX_WORDS;
        for (unsigned j = 1; j < nw; j++) w[j] = x.w[j].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (NRZ8_MUTATE != 11 && x.ver.load(std::memory_order_relaxed) != v0) return false;
        *ver = v0; return true;
    }
    // reader, one way: M_OK - slot i holds a template of (site, fixid): its words in w (as get()) and its version; M_SITE / M_FIX - a
    // template of another draw; M_EMPTY - never written, being written or written meanwhile. The head (the site words, the fix id) is
    // compared before the rest is read: a way of another draw costs its first cache line.
    int probe(uint32_t i, const nrz5_site *site, uint32_t fixid, uint64_t *w, uint32_t *ver) const {
        const Slot &x = s[i & (n - 1)];
        const uint32_t v0 = x.ver.load(std::memory_order_acquire);
        if (!v0 || (v0 & 1)) return M_EMPTY;
        for (unsigned j = 0; j < 7; j++) w[j] = x.w[j].load(std::memory_order_relaxed);
        int r = memcmp(w + 1, site->w, 6 * 8) ? M_SITE : (NRZ8_MUTATE != 7 && uint32_t(w[0] >> 32) != fixid) ? M_FIX : M_OK;
        if (r == M_OK) { unsigned nw = words_of(w[0]); if (nw > MAX_WORDS) nw = MAX_WORDS; for (unsigned j = 7; j < nw; j++) w[j] = x.w[j].load(std::memory_order_relaxed); }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (NRZ8_MUTATE != 11 && x.ver.load(std::memory_order_relaxed) != v0) return M_EMPTY;
        if (r == M_OK) *ver = v0;
        return r;
    }
    // the walker: the way of bucket b (slot_of) that holds the template of (site, fixid) - M_OK, *slot that way; else the outcome
    // (M_FIX: a way holds this site under another fix id; M_EMPTY: a way is empty or being written; M_SITE: every way holds another
    // draw) and *slot the way a template of this draw would take (the translation thread picks again when it publishes).
    int find(uint32_t b, const nrz5_site *site, uint32_t fixid, uint64_t *w, uint32_t *ver, uint32_t *slot) const {
        int out = M_SITE; *slot = b;
        for (uint32_t k = 0; k < (NRZ8_MUTATE == 31 ? 1u : uint32_t(WAYS)); k++) {
            const int r = probe(b + k, site, fixid, w, ver);
            if (r == M_OK) { *slot = b + k; return M_OK; }
            if (r == M_FIX) { out = M_FIX; *slot = b + k; } else if (r == M_EMPTY && out != M_FIX) { if (out != M_EMPTY) *slot = b + k; out = M_EMPTY; }
        }
        return out;
    }
    // the translation thread (the only writer: its own words are read relaxed): the way of hint's bucket the template w (pack()'s or
    // none_of()'s) goes to - the way that holds the same draw (site words and fix id), else an empty way, else the way whose use stamp
    // is the oldest (used[slot]: the translation thread's count when it last put or used the slot's template).
    uint32_t pick(uint32_t hint, const uint64_t *w, const uint32_t *used) const {
        const uint32_t b = hint & (n - 1) & ~uint32_t(WAYS - 1); int empty = -1;
        for (uint32_t k = 0; k < uint32_t(WAYS); k++) {
            const Slot &x = s[b + k];
            if (!x.ver.load(std::memory_order_relaxed)) { if (empty < 0) empty = int(k); continue; }
            bool eq = uint32_t(x.w[0].load(std::memory_order_relaxed) >> 32) == uint32_t(w[0] >> 32);
            for (unsigned j = 1; j < 7 && eq; j++) eq = x.w[j].load(std::memory_order_relaxed) == w[j];
            if (eq) return b + k;
        }
        if (empty >= 0) return b + uint32_t(empty);
        uint32_t old = b; for (uint32_t k = 1; k < uint32_t(WAYS); k++) if (int32_t(used[b + k] - used[old]) < 0) old = b + k;
        return old;
    }
};

// ---- the walker's look at one draw record (xbg_nr_z8.inc Hnd::nb_draw and the host test's walk call this) ---------------------
// site / fixid: the record's site words and the copy's fix id. A: the copy (as match()'s). w / v / off: room for the template, the
// values and the offsets. m: M_OK / M_NOVAL (answered: ver is the template's version, slot its way) or the refusal; slot is always the
// way a template of this draw is published to (the translation thread picks again).
// b: the bucket (Table::slot_of).
struct Look { int m = M_EMPTY; uint32_t slot = 0, ver = 0; };
template <class A> static inline Look look(const Table &t, uint32_t b, const nrz5_site &site, uint32_t fixid, const nbd_hdr &h, A &&a, uint64_t *w, nrz5_vals *v, uint32_t *off) {
    Look o; o.slot = b;
    if (!fixid) return o;
    const int f = t.find(o.slot, &site, fixid, w, &o.ver, &o.slot);
    o.m = f ? f : match(w, &site, fixid, h.count, h.instances, h.first, h.base_vertex, h.base_instance, a, v, off);
    return o;
}

// ---- the marks of a walk that runs on its own thread (DM_NR_Z8_THREAD=1, NR-ZERO.md section 28) ----------------------------------
// The walk on the submitting thread writes its words into the draw record before the job is queued. A walk on a thread of its own
// runs WHILE the translation thread may execute the same records, so it writes nothing into the command memory: the words of the k-th
// draw record of submission `sub` (k counts the OP_X_NB_DRAW records without NBD_F_CHECK in the order of the walk = the order of
// execute()) go into entry (base(sub) + k) of a ring, under a token sub << 24 | k that is never used twice.
//   writer   (the walker thread, one) begin(sub): base(sub) = its count of entries so far. put(): token 0, the words, the token.
//            end(): `done` = sub - the translation thread waits for it at the END of the submission before it reports the submission
//            executed (the game may reuse the command memory after that; the walk must have left it).
//   reader   (the translation thread) counts the same records itself. Entry base + k is the record's iff its token is (sub, k), its
//            address word is the record's address, and the token is the same after the words were read (a walk that laps the ring
//            starts an entry by clearing its token). Not there yet (the translation thread is ahead of the walk), lapped, or the
//            counts differ: the record has no words - it is resolved and recorded as today. Nothing waits for the walk here.
// So a record's words are taken only when the walk of THIS submission wrote them for THIS record at THIS place of the order.
struct MarkVal { uint64_t ca = 0; uint32_t mark = 0; int32_t slot1 = 0; uint64_t aux = 0, vw = 0, pk = 0; uint8_t why = 0; };   // why: the walker's outcome (M_*; NR-ZERO.md section 39, the census)   // pk: a tail draw's packets in its run's chunk, bytes | count << 32   // cond_addr word (fix id | version << 32), the run's mark, slot + 1, the caller's word (verify: the digest's place + 1)
enum { MARK_ORD_BITS = 24, MARK_SUBS = 64, MARK_SCAN = 2048 };
struct Marks {
    struct alignas(64) E { std::atomic<uint64_t> tok, rec, ca, w, aux, vw, pk; };   // (one cache line an entry; w: mark | slot + 1 << 32 (24 bits) | the outcome << 56)   // vw: the places of the draw's vertex / index ranges among the ranges of the submission (Ranges; 0: none)
    struct B { std::atomic<uint64_t> sub, base; };
    E *e = nullptr; uint32_t n = 0; B b[MARK_SUBS];
    std::atomic<uint64_t> done{0};               // the last submission the walker left (walked to its end or passed over)
    uint64_t seq = 0, wsub = 0; uint32_t word = 0;   // writer: entries so far; the submission in hand and its next ordinal
    bool init(uint32_t entries) {
        while (entries & (entries - 1)) entries &= entries - 1;
        if (!entries) return false;
        void *m = malloc(sizeof(E) * size_t(entries)); if (!m) return false;
        e = static_cast<E *>(m); n = entries;
        for (uint32_t i = 0; i < n; i++) { new (&e[i]) E; e[i].tok.store(0, std::memory_order_relaxed); e[i].rec.store(0, std::memory_order_relaxed); e[i].ca.store(0, std::memory_order_relaxed); e[i].w.store(0, std::memory_order_relaxed); e[i].aux.store(0, std::memory_order_relaxed); e[i].vw.store(0, std::memory_order_relaxed); e[i].pk.store(0, std::memory_order_relaxed); }
        for (auto &x : b) { x.sub.store(0, std::memory_order_relaxed); x.base.store(0, std::memory_order_relaxed); }
        return true;
    }
    void destroy() { free(static_cast<void *>(e)); e = nullptr; n = 0; }
    static uint64_t token(uint64_t sub, uint32_t k) { return sub << MARK_ORD_BITS | k; }
    // writer
    void begin(uint64_t sub) { B &x = b[sub % MARK_SUBS]; x.sub.store(0, std::memory_order_release); x.base.store(seq, std::memory_order_release); x.sub.store(sub, std::memory_order_release); wsub = sub; word = 0; }
    bool room() const { return word < (1u << MARK_ORD_BITS) - 1; }
    void put(const uint8_t *rec, const MarkVal &v) {
        if (!room()) return;   // (16 million draw records in one submission: the rest has no words)
        E &x = e[seq & (n - 1)]; const uint64_t t = token(wsub, word);
        x.tok.store(NRZ8_MUTATE == 18 ? t : 0, std::memory_order_relaxed); std::atomic_thread_fence(std::memory_order_release);   // (0 before any word)
#ifdef NRZ8_TEST_MID_PUT
        NRZ8_TEST_MID_PUT();   // (the host test only: what a reader on another thread sees between the first store and the words)
#endif
        x.rec.store(uint64_t(uintptr_t(rec)), std::memory_order_relaxed); x.ca.store(v.ca, std::memory_order_relaxed);
        x.w.store(uint64_t(v.mark) | uint64_t(uint32_t(v.slot1) & 0xffffffu) << 32 | uint64_t(v.why) << 56, std::memory_order_relaxed); x.aux.store(v.aux, std::memory_order_relaxed); x.vw.store(v.vw, std::memory_order_relaxed); x.pk.store(v.pk, std::memory_order_relaxed);
        x.tok.store(t, std::memory_order_release); ++seq; ++word;
    }
    void end() { done.store(wsub, std::memory_order_release); }
    void pass(uint64_t sub) { done.store(sub, std::memory_order_release); }   // a submission the walker does not walk
};
struct MarkReader {
    uint64_t sub = 0, base = 0; bool have = false; uint32_t ord = 0;
    uint64_t got = 0, not_there = 0, no_base = 0, resync = 0, desync = 0;
    void begin(uint64_t s) { sub = s; ord = 0; have = false; }
    // 0: entry k of this submission is the record's (o holds its words); 1: not written (yet), lapped or being written; 2: the entry
    // is of another record (the counts differ).
    int probe(const Marks &m, uint32_t k, const uint8_t *rec, MarkVal *o) const {
        const Marks::E &x = m.e[(base + k) & (m.n - 1)];
        const uint64_t t = x.tok.load(std::memory_order_acquire);
        if (NRZ8_MUTATE == 15 ? (t & ((1u << MARK_ORD_BITS) - 1)) != k || !t : t != Marks::token(sub, k)) return 1;
        const uint64_t r = x.rec.load(std::memory_order_relaxed);
        o->ca = x.ca.load(std::memory_order_relaxed); const uint64_t w = x.w.load(std::memory_order_relaxed); o->aux = x.aux.load(std::memory_order_relaxed); o->vw = x.vw.load(std::memory_order_relaxed); o->pk = x.pk.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (NRZ8_MUTATE != 17 && x.tok.load(std::memory_order_relaxed) != t) return 1;
        if (NRZ8_MUTATE != 16 && r != uint64_t(uintptr_t(rec))) return 2;
        o->mark = uint32_t(w); o->slot1 = int32_t(uint32_t(w >> 32) & 0xffffffu); o->why = uint8_t(w >> 56);
        return 0;
    }
    // The next draw record the translation thread executes (every OP_X_NB_DRAW without NBD_F_CHECK, in execute()'s order, once).
    bool next(const Marks &m, const uint8_t *rec, MarkVal *o) {
        const uint32_t k = ord++;
        if (!sub || k >= (1u << MARK_ORD_BITS) - 1) return false;
        if (!have) {
            const Marks::B &x = m.b[sub % MARK_SUBS];
            if (x.sub.load(std::memory_order_acquire) != sub) { ++no_base; return false; }   // the walk of this submission has not begun
            base = x.base.load(std::memory_order_acquire);
            if (x.sub.load(std::memory_order_acquire) != sub) { ++no_base; return false; }
            have = true;
        }
        int r = probe(m, k, rec, o);
        if (r == 0) { ++got; return true; }
        if (r == 1) { ++not_there; return false; }
        for (uint32_t j = k + 1; j < k + 1 + MARK_SCAN && j < (1u << MARK_ORD_BITS) - 1; j++) {   // the walk counted records this thread did not execute: the record's entry is further on
            r = probe(m, j, rec, o);
            if (r == 0) { ord = j + 1; ++resync; return true; }
            if (r == 1) break;
        }
        ++desync; return false;
    }
};

// ---- the ranges of a submission (DM_NR_Z8_VIEW, NR-ZERO.md section 30) ----------------------------------------------------------
// The walker gives every distinct (address, bytes, kind) range the answered draws of ONE submission bind - vertex streams, index
// ranges - a number 1, 2, 3, ... in the order of first use. The translation thread keeps the view it resolved for a range at the
// entry of that number, so the next draw of the submission that binds the same range finds it without a lookup. The number is only
// a place: the entry holds the range and the words under which its view stands, and the translation thread compares them.
struct Ranges {
    enum { N = 4096, MAX_ID = 3000 };
    struct E { uint64_t addr, bytes; uint32_t tag, gen, id; };
    E e[N]; uint32_t gen = 0, n = 0;
    E last[4] = {};   // [section 39, the walker's cost] the last range met at each place of a draw (3 vertex bindings, the indices): a repeat skips the table
    Ranges() { memset(e, 0, sizeof e); }
    void begin() { if (!++gen) { memset(e, 0, sizeof e); memset(last, 0, sizeof last); gen = 1; } n = 0; }
    // 0: no number (the table of this submission is full). k: the place of the range in its draw (0..3; 4: none).
    uint32_t id_of(uint64_t addr, uint64_t bytes, uint32_t tag, unsigned k = 4) {
        if (k < 4) { E &l = last[k]; if ((l.gen == gen || NRZ8_MUTATE == 20) && l.id && l.addr == addr && (l.bytes == bytes || NRZ8_MUTATE == 19) && l.tag == tag) return l.id;
            const uint32_t id = find(addr, bytes, tag); if (id) l = E{addr, bytes, tag, gen, id}; return id; }
        return find(addr, bytes, tag);
    }
    uint32_t find(uint64_t addr, uint64_t bytes, uint32_t tag) {
        uint32_t i = uint32_t((addr ^ bytes * 0x9E3779B97F4A7C15ull ^ tag) * 0xBF58476D1CE4E5B9ull >> 40) & (N - 1);
        for (unsigned p = 0; p < 32; p++, i = (i + 1) & (N - 1)) {
            E &x = e[i];
            if (x.gen != gen && NRZ8_MUTATE != 20) { if (n >= MAX_ID) return 0; x.addr = addr; x.bytes = bytes; x.tag = tag; x.gen = gen; x.id = ++n; return x.id; }
            if (x.addr == addr && (x.bytes == bytes || NRZ8_MUTATE == 19) && x.tag == tag) return x.id;
            if (NRZ8_MUTATE == 20 && !x.id) { if (n >= MAX_ID) return 0; x.addr = addr; x.bytes = bytes; x.tag = tag; x.gen = gen; x.id = ++n; return x.id; }
        }
        return 0;
    }
};
enum { VW_MAX_VB = 3 };   // the word of a draw: 16 bits a vertex binding (ascending, at most 3), bits 48..63 the index range
template <class VB> static inline uint64_t ranges_word(Ranges &r, uint32_t used, VB &&vb, bool indexed, uint64_t ibuf, uint64_t ibytes) {
    uint64_t w = 0; unsigned k = 0;
    for (uint32_t m = used; m; m &= m - 1) { const uint32_t i = uint32_t(__builtin_ctz(m)); if (k >= VW_MAX_VB) return 0;
        uint64_t a = 0, z = 0; if (!vb(i, &a, &z)) return 0; const uint32_t id = r.id_of(a, z, 1, k); if (!id) return 0; w |= uint64_t(id) << (16 * k++); }
    if (indexed) { const uint32_t id = r.id_of(ibuf, ibytes, 2, 3); if (!id) return 0; w |= uint64_t(id) << 48; }
    return w;
}

// The words a draw record carries besides its mark (nbd_hdr): _pad0 = the stamp of the submission whose walk wrote them (1..255; a
// record of a handle submitted again carries the later walk's stamp and is not taken), _pad2 = slot + 1, cond_addr (unused by a draw
// without a render condition) = the fix id | the template's version << 32 (0: the walk had no template for the record).
static inline uint8_t stamp_of(uint64_t sub) { return uint8_t(sub % 255u + 1u); }

} // namespace nrz8
#endif
