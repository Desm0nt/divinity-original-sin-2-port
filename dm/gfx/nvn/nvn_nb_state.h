// [DOS2] NB-13a: OP_X_NB_STATE, the state calls of the engine's state opcodes as content-deduplicated blocks (docs/dos2/NATIVE_BACKEND_PLAN.md
// §2, NB-13a / 13b / 14). Pure C (C, C++ and Objective-C; no NVN calls, no Vulkan): included by nvn_obj.h after the op enums and payloads
// (it classifies the NVN records OP_BIND_STATE / OP_BIND_PROGRAM / ...); used by the NVN tap (nvn_nb_state.inc, inside hle_nvn.c), by the
// backend (dm/gfx/vulkan/engine/xbg_nb_state.inc) and by the host test dm/gfx/vulkan/engine/tests/nb_state_test.c.
//
// Shadow. Inside the bracket of a taken engine state opcode (DM_NB_OPS bits 6 7 8 9 10 11 18 19 21 27 48 63) the state records the NVN
// entry points would write (bind_state / BindProgram / SetViewports / SetScissor / ...) go to a shadow instead of the command memory. The
// shadow holds, per sub-state ("kind"), the records in last-write order with one entry per key (same key = the same backend fields; a new
// record of a key replaces the old one and moves to the end). Replaying the entries of a kind in order leaves the backend's state exactly as
// the full call sequence did: every field ends at the value of its last write, and the last write of a field is the last record of its key.
// At the next record that is not captured (a draw, an OP_X_NB_DRAW, a clear, any other record) the dirty kinds are written as ONE
// OP_X_NB_STATE: a kind whose content equals what this epoch already emitted for it is skipped; otherwise its content gets a small id from a
// per-epoch dictionary (one per kind): a new content is a DEF block (id + the records + OP_END), a repeated one is a REF block (id only).
// Unknown (risk 3): a state record written outside a taken bracket clears its kind's shadow and forgets what was emitted for it, so the
// next taken call of that kind is emitted again with only the entries set since (nothing older can overwrite the raw call's fields).
// Epoch = one recording of one command buffer (the dictionary resets at a change of command buffer and at EndRecording): a REF always
// points to a DEF earlier in the same command handle, which the backend executes before it, in the same memory.
//
// Record payload (after op_hdr, 8-aligned):  nbs_hdr {ver, nblk, epoch} | nblk x (nbs_blk {kind, how, id, bytes} [+ bytes of content])
//   content (DEF / NOID) = NVN records (op_hdr + payload, as the entry points write them) + OP_END: the backend executes it in place.
#ifndef DM_NVN_NB_STATE_H
#define DM_NVN_NB_STATE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { NBS_VER = 1, NBS_KINDS = 8, NBS_MAX_IDS = 1024, NBS_HASH = 2048, NBS_ARENA = 32768, NBS_SUB_BYTES = 1024, NBS_SUB_ENTS = 32,
       NBS_REC_MAX = 512 };   // ids and dictionary bytes per kind and epoch; shadow bytes / entries per kind; largest captured record
enum { NBS_K_BLEND, NBS_K_COLOR, NBS_K_POLY, NBS_K_DS, NBS_K_VFMT, NBS_K_VB, NBS_K_PROG, NBS_K_VPSC };
// blend: BindBlendState (per target) + SetBlendColor; color: BindColorState / BindChannelMaskState / BindMultisampleState + SetAlphaRef;
// polygon: BindPolygonState + SetPolygonOffsetClamp; depth-stencil: BindDepthStencilState + SetStencil{ValueMask,Mask,Ref} (per face);
// vertex format: BindVertexAttribState / BindVertexStreamState; VB: BindVertexBuffer(s); program: BindProgram; viewport / scissor:
// SetViewport(s) / SetScissor(s) / SetDepthRange(s).
enum { NBS_B_REF = 0, NBS_B_DEF = 1, NBS_B_NOID = 2 };   // id only; id + content; content without an id (dictionary full)
typedef struct { uint8_t ver, nblk; uint16_t _pad; uint32_t epoch; } nbs_hdr;
typedef struct { uint8_t kind, how; uint16_t id; uint32_t bytes; } nbs_blk;   // bytes: content bytes after the block (0 for REF)
enum { NBS_PAYLOAD_MAX = (int)sizeof(nbs_hdr) + NBS_KINDS * ((int)sizeof(nbs_blk) + NBS_SUB_BYTES + 8) };

#ifdef __cplusplus
static_assert(sizeof(nbs_hdr) == 8 && sizeof(nbs_blk) == 8, "OP_X_NB_STATE layout");
#else
_Static_assert(sizeof(nbs_hdr) == 8 && sizeof(nbs_blk) == 8, "OP_X_NB_STATE layout");
#endif

static inline uint32_t nbs_rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
// The kinds a record of op can touch (OP_BIND_STATE: by its kind field, unknown before the payload is written = all four). 0 = not state.
static inline unsigned nbs_op_kinds(uint32_t op) {
    switch (op) {
    case OP_BIND_STATE: return 1u << NBS_K_BLEND | 1u << NBS_K_COLOR | 1u << NBS_K_POLY | 1u << NBS_K_DS;
    case OP_BLEND_COLOR: return 1u << NBS_K_BLEND;
    case OP_X_ALPHA_REF: return 1u << NBS_K_COLOR;
    case OP_POLY_OFFSET: return 1u << NBS_K_POLY;
    case OP_STENCIL: return 1u << NBS_K_DS;
    case OP_BIND_VATTR: case OP_BIND_VSTREAM: return 1u << NBS_K_VFMT;
    case OP_BIND_VBUF: return 1u << NBS_K_VB;
    case OP_BIND_PROGRAM: return 1u << NBS_K_PROG;
    case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: return 1u << NBS_K_VPSC;
    default: return 0;
    }
}
// Kind and key of a written record (pl = payload, n = payload bytes). -1 = not a state record we model (it stays a plain record).
// Same key = the record sets the same backend fields (xbg_game.cpp execute): BIND_STATE kind (+ blend target), STENCIL face / which,
// VIEWPORTS / VSTREAM / VBUF first / n, BIND_PROGRAM stages; VATTR replaces the whole attribute set; the rest are one value each.
static inline int nbs_kind(uint32_t op, const uint8_t *pl, uint32_t n, uint32_t *key) {
    const uint32_t k0 = op << 16;
    switch (op) {
    case OP_BIND_STATE: {
        if (n < 5) return -1;
        const uint32_t kind = nbs_rd32(pl);
        if (kind == 0) { *key = k0 | (pl[4] & 7u); return NBS_K_BLEND; }   // blend_st.target = the low 3 bits
        *key = k0 | kind << 8;
        return kind == 4 ? NBS_K_POLY : kind == 5 ? NBS_K_DS : kind <= 3 ? NBS_K_COLOR : -1;
    }
    case OP_STENCIL: if (n < 20) return -1; *key = k0 | (nbs_rd32(pl) & 3u) << 2 | (nbs_rd32(pl + 16) & 3u); return NBS_K_DS;
    case OP_VIEWPORTS: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
        if (n < 8) return -1;
        *key = k0 | (nbs_rd32(pl) & 31u) << 5 | (nbs_rd32(pl + 4) & 31u);
        return op == OP_VIEWPORTS ? NBS_K_VPSC : op == OP_BIND_VBUF ? NBS_K_VB : NBS_K_VFMT;
    case OP_BIND_PROGRAM: {   // [NB-14] a NULL program has its own key: every non-NULL BindProgram also sets missing_program (all stages), so
        // a NULL one of the same stages must not replace it (the last non-NULL record must stay in the replay)
        if (n < 12) return -1;
        uint64_t prog; memcpy(&prog, pl, 8);
        *key = k0 | (nbs_rd32(pl + 8) & 0x7fffu) | (prog ? 0u : 0x8000u); return NBS_K_PROG;
    }
    case OP_BIND_VATTR: *key = k0; return NBS_K_VFMT;
    case OP_BLEND_COLOR: *key = k0; return NBS_K_BLEND;
    case OP_X_ALPHA_REF: *key = k0; return NBS_K_COLOR;
    case OP_POLY_OFFSET: *key = k0; return NBS_K_POLY;
    case OP_SCISSOR: case OP_DEPTH_RANGE: *key = k0; return NBS_K_VPSC;
    default: return -1;
    }
}

// ---- shadow of one kind: records in last-write order, one per key ----
typedef struct { uint32_t key; uint16_t off, size; } nbs_ent;
typedef struct { uint16_t n, used; nbs_ent e[NBS_SUB_ENTS]; uint8_t buf[NBS_SUB_BYTES]; } nbs_sub;
// Put a whole record (header + payload, size bytes, 8-aligned). -1 = it does not fit (the shadow is left as it was).
static inline int nbs_sub_put(nbs_sub *s, uint32_t key, const uint8_t *rec, uint32_t size) {
    int at = -1;
    for (int i = 0; i < s->n; i++) if (s->e[i].key == key) { at = i; break; }
    const uint32_t used = s->used - (at >= 0 ? s->e[at].size : 0u) + size, n = s->n - (at >= 0) + 1u;
    if (size < 8 || (size & 7) || n > NBS_SUB_ENTS || used + 8 > NBS_SUB_BYTES) return -1;   // + 8: room for OP_END
    if (at >= 0) {   // remove it (later records move down)
        const uint16_t off = s->e[at].off, sz = s->e[at].size;
        memmove(s->buf + off, s->buf + off + sz, s->used - off - sz);
        for (int i = at; i + 1 < s->n; i++) { s->e[i] = s->e[i + 1]; s->e[i].off = (uint16_t)(s->e[i].off - sz); }
        s->n--; s->used = (uint16_t)(s->used - sz);
    }
    s->e[s->n].key = key; s->e[s->n].off = s->used; s->e[s->n].size = (uint16_t)size; s->n++;
    memcpy(s->buf + s->used, rec, size); s->used = (uint16_t)(s->used + size);
    return 0;
}

// ---- per-kind dictionary of one epoch: content -> id ----
typedef struct { uint64_t h; uint32_t off, size; } nbs_did;
typedef struct { uint32_t nid, used; uint16_t tab[NBS_HASH]; nbs_did id[NBS_MAX_IDS]; uint8_t arena[NBS_ARENA]; } nbs_dict;
static inline uint64_t nbs_hash(const uint8_t *p, uint32_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    for (uint32_t i = 0; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h = (h ^ w) * 0xff51afd7ed558ccdull; h ^= h >> 29; }
    for (uint32_t i = n & ~7u; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull; h ^= h >> 33;
    return h;
}
static inline void nbs_dict_reset(nbs_dict *d) { d->nid = 0; d->used = 0; memset(d->tab, 0, sizeof d->tab); }
// The id of content p[0..n): the existing one (*fresh = 0) or a new one (*fresh = 1). -1 = the dictionary or its arena is full.
static inline int nbs_dict_get(nbs_dict *d, const uint8_t *p, uint32_t n, int *fresh) {
    const uint64_t h = nbs_hash(p, n);
    uint32_t k = (uint32_t)(h >> 40) & (NBS_HASH - 1);
    for (;; k = (k + 1) & (NBS_HASH - 1)) {   // at most NBS_MAX_IDS of NBS_HASH slots are used: an empty slot is always found
        const uint16_t t = d->tab[k];
        if (!t) break;
        const nbs_did *e = &d->id[t - 1];
        if (e->h == h && e->size == n && !memcmp(d->arena + e->off, p, n)) { *fresh = 0; return t - 1; }
    }
    if (d->nid >= NBS_MAX_IDS || d->used + n > NBS_ARENA) return -1;
    nbs_did *e = &d->id[d->nid]; e->h = h; e->off = d->used; e->size = n;
    memcpy(d->arena + d->used, p, n); d->used += n;
    d->tab[k] = (uint16_t)(++d->nid);
    *fresh = 1; return (int)d->nid - 1;
}

// ---- the whole shadow ----
enum { NBS_S_DEF, NBS_S_REF, NBS_S_NOID, NBS_S_SAME, NBS_S_UNKNOWN, NBS_S_N };   // pack / unknown counters
typedef struct {
    uint32_t epoch; uint8_t dirty;                // epoch of the dictionaries (0 = none yet); kinds changed since the last pack
    int32_t emitted[NBS_KINDS];                   // id this epoch last emitted per kind, -1 = nothing / unknown / without an id
    nbs_sub s[NBS_KINDS];
    nbs_dict d[NBS_KINDS];
    uint64_t st[NBS_S_N];
} nbs_state;
static inline void nbs_new_epoch(nbs_state *s, uint32_t epoch) {
    s->epoch = epoch;
    for (int k = 0; k < NBS_KINDS; k++) { nbs_dict_reset(&s->d[k]); s->emitted[k] = -1; }
}
static inline void nbs_init(nbs_state *s, uint32_t epoch) { memset(s, 0, sizeof *s); nbs_new_epoch(s, epoch); }
// A captured record (header + payload, size bytes). -1 = not modelled or it does not fit: the caller writes it as a plain record (after a
// pack) and marks its kind unknown.
static inline int nbs_capture(nbs_state *s, const uint8_t *rec, uint32_t size) {
    if (size < 8 || size > NBS_REC_MAX) return -1;
    uint32_t key; const int k = nbs_kind(nbs_rd32(rec), rec + 8, size - 8, &key);
    if (k < 0 || nbs_sub_put(&s->s[k], key, rec, size)) return -1;
    s->dirty |= (uint8_t)(1u << k);
    return k;
}
// Kinds in mask were changed behind the shadow (a plain record): forget their entries and what was emitted for them.
static inline void nbs_unknown(nbs_state *s, unsigned mask) {
    for (int k = 0; k < NBS_KINDS; k++) if (mask >> k & 1) {
        if (s->s[k].n || s->emitted[k] >= 0) s->st[NBS_S_UNKNOWN]++;
        s->s[k].n = 0; s->s[k].used = 0; s->emitted[k] = -1; s->dirty &= (uint8_t)~(1u << k);
    }
}
// The OP_X_NB_STATE payload for the dirty kinds into dst (cap >= NBS_PAYLOAD_MAX). Returns its bytes, 0 = nothing to emit. Clears dirty.
static inline uint32_t nbs_pack(nbs_state *s, uint8_t *dst, size_t cap) {
    if (!s->dirty || cap < (size_t)NBS_PAYLOAD_MAX) { s->dirty = 0; return 0; }
    uint32_t off = sizeof(nbs_hdr); unsigned nblk = 0;
    for (int k = 0; k < NBS_KINDS; k++) {
        if (!(s->dirty >> k & 1)) continue;
        nbs_sub *u = &s->s[k];
        if (!u->n) continue;
        uint8_t *c = dst + off + sizeof(nbs_blk);
        memcpy(c, u->buf, u->used); memset(c + u->used, 0, 8); { const uint32_t endsz = 8; memcpy(c + u->used + 4, &endsz, 4); }   // + OP_END (op 0, size 8)
        const uint32_t bytes = u->used + 8u;
        int fresh = 0; const int id = nbs_dict_get(&s->d[k], c, bytes, &fresh);
        if (id >= 0 && id == s->emitted[k]) { s->st[NBS_S_SAME]++; continue; }   // the backend has this content of the kind already
        nbs_blk b = {(uint8_t)k, (uint8_t)(id < 0 ? NBS_B_NOID : fresh ? NBS_B_DEF : NBS_B_REF), (uint16_t)(id < 0 ? 0 : id), 0};
        if (b.how != NBS_B_REF) b.bytes = bytes;
        memcpy(dst + off, &b, sizeof b); off += (uint32_t)sizeof b + b.bytes; nblk++;
        s->emitted[k] = id;   // -1 for NOID: the next change of the kind is emitted again
        s->st[b.how == NBS_B_DEF ? NBS_S_DEF : b.how == NBS_B_REF ? NBS_S_REF : NBS_S_NOID]++;
    }
    s->dirty = 0;
    if (!nblk) return 0;
    const nbs_hdr h = {NBS_VER, (uint8_t)nblk, 0, s->epoch};
    memcpy(dst, &h, sizeof h);
    return off;
}

// ---- reading a record (backend, test) ----
// Content check: 8-aligned records of the block's kind only, ended by exactly one OP_END at its last 8 bytes.
static inline int nbs_content_ok(unsigned kind, const uint8_t *c, uint32_t bytes) {
    if (bytes < 16 || (bytes & 7) || bytes > NBS_SUB_BYTES) return 0;
    uint32_t off = 0;
    while (off < bytes) {
        const uint32_t op = nbs_rd32(c + off), size = nbs_rd32(c + off + 4);
        if (size < 8 || (size & 7) || size > bytes - off) return 0;
        if (op == 0) return size == 8 && off + 8 == bytes && off > 0;   // OP_END
        uint32_t key;
        if (size > NBS_REC_MAX || nbs_kind(op, c + off + 8, size - 8, &key) != (int)kind) return 0;
        off += size;
    }
    return 0;
}
typedef struct { nbs_blk b; const uint8_t *content; } nbs_view;
// Walks a payload of n bytes (may include the record's 8-byte alignment tail). Returns the number of blocks, -1 = malformed.
static inline int nbs_parse(const uint8_t *p, size_t n, nbs_hdr *h, nbs_view out[NBS_KINDS]) {
    if (!p || n < sizeof(nbs_hdr)) return -1;
    memcpy(h, p, sizeof *h);
    if (h->ver != NBS_VER || !h->nblk || h->nblk > NBS_KINDS || !h->epoch) return -1;
    size_t off = sizeof *h;
    for (unsigned i = 0; i < h->nblk; i++) {
        if (n - off < sizeof(nbs_blk)) return -1;
        nbs_view *v = &out[i]; memcpy(&v->b, p + off, sizeof v->b); off += sizeof v->b;
        if (v->b.kind >= NBS_KINDS || v->b.how > NBS_B_NOID || v->b.id >= NBS_MAX_IDS) return -1;
        if (v->b.how == NBS_B_REF) { if (v->b.bytes) return -1; v->content = NULL; continue; }
        if (v->b.how == NBS_B_NOID && v->b.id) return -1;
        if (n - off < v->b.bytes || !nbs_content_ok(v->b.kind, p + off, v->b.bytes)) return -1;
        v->content = p + off; off += v->b.bytes;
    }
    return (int)h->nblk;
}
#endif
