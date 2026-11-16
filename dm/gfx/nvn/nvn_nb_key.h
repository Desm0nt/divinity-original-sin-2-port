// [DOS2] NB-14: OP_X_NB_KEY, the key frame of the state at the start of an IR pass (docs/dos2/NATIVE_BACKEND_PLAN.md NB-14). Pure C (C and
// C++; no NVN calls, no Vulkan): included by nvn_obj.h after nvn_nb_state.h; used by the NVN tap (nvn_nb_key.inc, inside hle_nvn.c), by the
// backend (dm/gfx/vulkan/engine/xbg_nb_key.inc) and by the host test dm/gfx/vulkan/engine/tests/nb_key_test.c.
//
// What it is. Right after every OP_X_NB_PASS (DM_NB_RELOC 1 / 2) the tap writes one OP_X_NB_KEY: the backend state at that point of the
// stream, relative to the OP_X_NB_FRAME of the same IR part (the backend keeps a copy of its `state` there). base (state at FRAME) + the key
// = the whole state the pass starts from, whatever ran before it: a pass can then be executed out of order (NB-17a) or checked (=2).
//
// How the front end knows the state. At FRAME the mirror below is emptied and a walk position is set at the end of the FRAME record. At each
// PASS the tap walks the records written since (same command memory, OP_JUMP across segments, into OP_CALL handles, OP_X_NATIVE_DRAW,
// OP_X_NB_STATE blocks and OP_X_NB_DRAW binds) and files every state record: the backend executes exactly these records in this order,
// so the mirror is what the backend's state gained since FRAME. Once per pass, never per draw or per record written.
//
// Units (40). Record lists (0..9): the NB-13a kinds (blend, color, polygon, depth-stencil, vertex format, VB, program, viewport / scissor,
// keyed by nbs_kind), the render-target set (OP_SET_RT) and misc (texture / sampler pool, copy strides, patch size, depth bounds, primitive
// restart, depth clamp, rasterizer discard): the records in last-write order, one per key (replaying them gives the same fields as the whole
// sequence, NB-13a). Bind tables (10..39): texture / sampler / image handles, uniform / storage buffers per stage, the slots set since FRAME,
// written as canonical bind records (runs of <= 32 handles; one record per buffer slot). A unit with nothing set since FRAME is absent: its
// fields are base's.
// Encoding. nbk_hdr + nunit x nbk_unit {magic, unit, how, seq, bytes}: DEF = content (NVN records + OP_END, executed in place by the
// backend's execute(), like OP_X_NB_STATE); REF = the 8-byte address of an earlier DEF of the same unit and IR part (the last one emitted
// for that unit, when the content is the same): any key can be applied in any order (the address carries no order; the backend checks the
// DEF header it points to: magic, unit, seq). `lost`: units the front end could not follow (a list full, an OP_X_NB_STATE REF it could not
// resolve, a malformed state record): neither checked nor applied, the pass is not relocatable. NBK_F_LOST: the walk lost the stream (an
// OP_END of the recording before the pass, a bad record, the guard): the whole key is void.
#ifndef DM_NVN_NB_KEY_H
#define DM_NVN_NB_KEY_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { NBK_VER = 1, NBK_MAGIC = 0x314b424e /* "NBK1" */ };
enum { NBK_U_BLEND = NBS_K_BLEND, NBK_U_COLOR = NBS_K_COLOR, NBK_U_POLY = NBS_K_POLY, NBK_U_DS = NBS_K_DS, NBK_U_VFMT = NBS_K_VFMT,
       NBK_U_VB = NBS_K_VB, NBK_U_PROG = NBS_K_PROG, NBK_U_VPSC = NBS_K_VPSC, NBK_U_RT = 8, NBK_U_MISC = 9, NBK_LISTS = 10,
       NBK_U_TEX = 10, NBK_U_SAMP = 16, NBK_U_IMG = 22, NBK_U_UBO = 28, NBK_U_SSBO = 34, NBK_UNITS = 40 };
enum { NBK_H_DEF = 1, NBK_H_REF = 2 };
enum { NBK_F_LOST = 1 };
enum { NBK_LIST_ENTS = 64, NBK_LIST_BYTES = 2048, NBK_REC_MAX = 512, NBK_UNIT_MAX = 4096, NBK_PAYLOAD_MAX = 65536, NBK_DEPTH = 8,
       NBK_GUARD = 1 << 21 };
typedef struct { uint32_t seq; int16_t node; uint8_t ver, flags; uint16_t nunit, _pad; uint32_t walked; uint64_t lost; } nbk_hdr;
typedef struct { uint32_t magic; uint8_t unit, how; uint16_t _pad; uint32_t seq, bytes; } nbk_unit;   // REF: bytes = 8 (the DEF's address)
#ifdef __cplusplus
static_assert(sizeof(nbk_hdr) == 24 && sizeof(nbk_unit) == 16, "OP_X_NB_KEY layout");
#else
_Static_assert(sizeof(nbk_hdr) == 24 && sizeof(nbk_unit) == 16, "OP_X_NB_KEY layout");
#endif

// ---- which unit a record belongs to ----
// List units: kind / key (same key = same backend fields). -1 = not a list record.
static inline int nbk_list_of(uint32_t op, const uint8_t *pl, uint32_t n, uint32_t *key) {
    const int k = nbs_kind(op, pl, n, key);
    if (k >= 0) return k;
    switch (op) {
    case OP_SET_RT: *key = op << 16; return NBK_U_RT;
    case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_COPY_STRIDE: case OP_COPY_IMAGE_STRIDE: case OP_X_PATCH_SIZE: case OP_X_DEPTH_BOUNDS:
    case OP_X_PRIM_RESTART: case OP_X_DEPTH_CLAMP: case OP_X_RAST_DISCARD:
        *key = op << 16; return NBK_U_MISC;
    default: return -1;
    }
}
// Units a record of op may touch when it cannot be classified (state op with a short / unknown payload): they become lost.
static inline uint64_t nbk_op_units(uint32_t op) {
    const unsigned k = nbs_op_kinds(op);
    if (k) return k;
    switch (op) {
    case OP_SET_RT: return 1ull << NBK_U_RT;
    case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_COPY_STRIDE: case OP_COPY_IMAGE_STRIDE: case OP_X_PATCH_SIZE: case OP_X_DEPTH_BOUNDS:
    case OP_X_PRIM_RESTART: case OP_X_DEPTH_CLAMP: case OP_X_RAST_DISCARD: return 1ull << NBK_U_MISC;
    case OP_BIND_TEX: return 0x3full << NBK_U_TEX;
    case OP_BIND_SAMP: return 0x3full << NBK_U_SAMP;
    case OP_BIND_IMG: return 0x3full << NBK_U_IMG;
    case OP_BIND_UBO: return 0x3full << NBK_U_UBO;
    case OP_BIND_SSBO: return 0x3full << NBK_U_SSBO;
    default: return 0;
    }
}
// Bind records the backend accepts (xbg_game.cpp exec_bind_handles / exec_bind_buf limits). Returns the unit, -1 = rejected / not a bind.
static inline int nbk_bind_unit(uint32_t op, const uint8_t *pl, uint32_t n) {
    int32_t a[3];
    switch (op) {
    case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: {
        if (n < 16) return -1;
        memcpy(a, pl, 12);
        const int lim = op == OP_BIND_IMG ? 16 : 128;
        if (a[0] < 0 || a[0] >= 6 || a[1] < 0 || a[2] < 0 || a[2] > 32 || a[1] + a[2] > lim || n < 16u + 8u * (uint32_t)a[2]) return -1;
        return (op == OP_BIND_TEX ? NBK_U_TEX : op == OP_BIND_SAMP ? NBK_U_SAMP : NBK_U_IMG) + a[0];
    }
    case OP_BIND_UBO: case OP_BIND_SSBO:
        if (n < 24) return -1;
        memcpy(a, pl, 8);
        if (a[0] < 0 || a[0] >= 6 || a[1] < 0 || a[1] >= 16) return -1;
        return (op == OP_BIND_UBO ? NBK_U_UBO : NBK_U_SSBO) + a[0];
    default: return -1;
    }
}
static inline int nbk_unit_of(uint32_t op, const uint8_t *pl, uint32_t n) {
    uint32_t key;
    const int b = nbk_bind_unit(op, pl, n);
    return b >= 0 ? b : nbk_list_of(op, pl, n, &key);
}

// ---- the mirror: what the backend's state gained since FRAME ----
typedef struct { uint16_t n, used; struct { uint32_t key; uint16_t off, size; } e[NBK_LIST_ENTS]; uint8_t buf[NBK_LIST_BYTES]; } nbk_list;
typedef struct { uint64_t a, b; } nbk_range;   // = NVNbufferRange {address, size}
enum { NBK_S_WALKED, NBK_S_NESTED, NBK_S_BLOCKS, NBK_S_NBDRAW, NBK_S_UNRESOLVED, NBK_S_OVER, NBK_S_BAD, NBK_S_N };
typedef struct {
    nbk_list l[NBK_LISTS];
    uint64_t tex[6][128], samp[6][128], img[6][16];
    nbk_range ub[6][16], sb[6][16];
    uint64_t tex_set[6][2], samp_set[6][2];
    uint16_t img_set[6], ub_set[6], sb_set[6];
    uint64_t lost;                    // units that cannot be trusted until the next FRAME
    const uint8_t *last[NBK_UNITS];   // last DEF emitted per unit (its nbk_unit in the command memory), NULL = none this part
    uint64_t st[NBK_S_N];             // walk counters (cumulative; the tap takes deltas)
} nbk_mirror;
// OP_X_NB_STATE DEF blocks seen by the walk (kind, id) -> content, per epoch
typedef struct { uint32_t epoch; uint32_t bytes; const uint8_t *p; } nbk_def;
typedef struct { nbk_def d[NBS_KINDS][NBS_MAX_IDS]; } nbk_defs;
// Fallback for a REF whose DEF lies before the walk start: content of (kind, id) in epoch, or NULL (the tap asks the NB-13a dictionary).
typedef const uint8_t *(*nbk_resolve_fn)(unsigned kind, unsigned id, uint32_t epoch, uint32_t *bytes);

static inline void nbk_reset(nbk_mirror *m) {   // FRAME: nothing set since (the tables' values are don't-care while their bit is 0)
    for (int i = 0; i < NBK_LISTS; i++) { m->l[i].n = 0; m->l[i].used = 0; }
    memset(m->tex_set, 0, sizeof m->tex_set); memset(m->samp_set, 0, sizeof m->samp_set);
    memset(m->img_set, 0, sizeof m->img_set); memset(m->ub_set, 0, sizeof m->ub_set); memset(m->sb_set, 0, sizeof m->sb_set);
    m->lost = 0; memset(m->last, 0, sizeof m->last);
}
// A whole record (header + payload, size bytes, 8-aligned), one per key, last-write order. -1 = it does not fit (list left as it was).
static inline int nbk_list_put(nbk_list *s, uint32_t key, const uint8_t *rec, uint32_t size) {
    int at = -1;
    for (int i = 0; i < s->n; i++) if (s->e[i].key == key) { at = i; break; }
    const uint32_t used = s->used - (at >= 0 ? s->e[at].size : 0u) + size, n = s->n - (at >= 0) + 1u;
    if (size < 8 || (size & 7) || n > NBK_LIST_ENTS || used + 8 > NBK_LIST_BYTES) return -1;   // + 8: room for OP_END
    if (at >= 0) {
        const uint16_t off = s->e[at].off, sz = s->e[at].size;
        memmove(s->buf + off, s->buf + off + sz, (size_t)(s->used - off - sz));
        for (int i = at; i + 1 < s->n; i++) { s->e[i] = s->e[i + 1]; s->e[i].off = (uint16_t)(s->e[i].off - sz); }
        s->n--; s->used = (uint16_t)(s->used - sz);
    }
    s->e[s->n].key = key; s->e[s->n].off = s->used; s->e[s->n].size = (uint16_t)size; s->n++;
    memcpy(s->buf + s->used, rec, size); s->used = (uint16_t)(s->used + size);
    return 0;
}
static inline void nbk_set_handle(nbk_mirror *m, uint32_t op, unsigned st, unsigned sl, uint64_t v) {
    if (op == OP_BIND_TEX) { m->tex[st][sl] = v; m->tex_set[st][sl >> 6] |= 1ull << (sl & 63); }
    else if (op == OP_BIND_SAMP) { m->samp[st][sl] = v; m->samp_set[st][sl >> 6] |= 1ull << (sl & 63); }
    else { m->img[st][sl] = v; m->img_set[st] = (uint16_t)(m->img_set[st] | 1u << sl); }
}
static inline void nbk_set_buf(nbk_mirror *m, uint32_t op, unsigned st, unsigned sl, uint64_t a, uint64_t b) {
    nbk_range *r = op == OP_BIND_UBO ? &m->ub[st][sl] : &m->sb[st][sl]; r->a = a; r->b = b;
    if (op == OP_BIND_UBO) m->ub_set[st] = (uint16_t)(m->ub_set[st] | 1u << sl); else m->sb_set[st] = (uint16_t)(m->sb_set[st] | 1u << sl);
}
// One record (rec = its header, size bytes) the backend executes, filed into the mirror. Nested records are the walk's business.
static inline void nbk_file(nbk_mirror *m, const uint8_t *rec, uint32_t size) {
    uint32_t op; memcpy(&op, rec, 4);
    const uint8_t *pl = rec + 8; const uint32_t n = size - 8;
    if (op == OP_BIND_TEX || op == OP_BIND_SAMP || op == OP_BIND_IMG) {
        if (nbk_bind_unit(op, pl, n) < 0) { m->st[NBK_S_BAD]++; return; }   // the backend stops on it (need)
        int32_t a[3]; memcpy(a, pl, 12);
        for (int i = 0; i < a[2]; i++) { uint64_t v; memcpy(&v, pl + 16 + 8 * i, 8); nbk_set_handle(m, op, (unsigned)a[0], (unsigned)(a[1] + i), v); }
        return;
    }
    if (op == OP_BIND_UBO || op == OP_BIND_SSBO) {
        if (nbk_bind_unit(op, pl, n) < 0) { m->st[NBK_S_BAD]++; return; }
        int32_t a[2]; uint64_t r[2]; memcpy(a, pl, 8); memcpy(r, pl + 8, 16);
        nbk_set_buf(m, op, (unsigned)a[0], (unsigned)a[1], r[0], r[1]);
        return;
    }
    uint32_t key; const int u = nbk_list_of(op, pl, n, &key);
    if (u < 0) { const uint64_t x = nbk_op_units(op); if (x) { m->lost |= x; m->st[NBK_S_BAD]++; } return; }   // a state op we cannot key
    if (size > NBK_REC_MAX || nbk_list_put(&m->l[u], key, rec, size)) { m->lost |= 1ull << u; m->st[NBK_S_OVER]++; }
}
// The binds of an OP_X_NB_DRAW payload (a check-only record, NBD_F_CHECK, applies nothing: its old records are in the stream too).
static inline void nbk_nbdraw(nbk_mirror *m, const uint8_t *pl, uint32_t n) {
    nbd_hdr h; const uint8_t *b;
    if (nbd_view(pl, n, &h, &b)) { m->st[NBK_S_BAD]++; return; }   // the backend stops on it
    m->st[NBK_S_NBDRAW]++;
    if (h.flags & NBD_F_CHECK) return;
    for (unsigned i = 0; i < h.nbind; i++) {
        nbd_bind e; memcpy(&e, b + sizeof e * i, sizeof e);
        if (!nbd_bind_ok(e.kind, e.stage, e.slot)) continue;   // nbd_apply skips it too
        if (e.kind == NBD_B_UBO) nbk_set_buf(m, OP_BIND_UBO, e.stage, e.slot, e.a, e.b);
        else nbk_set_handle(m, e.kind == NBD_B_TEX ? OP_BIND_TEX : e.kind == NBD_B_SAMP ? OP_BIND_SAMP : OP_BIND_IMG, e.stage, e.slot, e.a);
    }
}

// ---- the walk ----
enum { NBK_W_OK = 0, NBK_W_END = -1, NBK_W_BAD = -2, NBK_W_GUARD = -3 };
static inline int nbk_walk_seq(nbk_mirror *m, nbk_defs *defs, nbk_resolve_fn res, const uint8_t **pos, const uint8_t *to, int depth, uint32_t *guard);
static inline void nbk_state_rec(nbk_mirror *m, nbk_defs *defs, nbk_resolve_fn res, const uint8_t *pl, uint32_t n, int depth, uint32_t *guard) {
    nbs_hdr h; nbs_view v[NBS_KINDS];
    const int nb = nbs_parse(pl, n, &h, v);
    if (nb <= 0) { m->lost |= 0xffull; m->st[NBK_S_BAD]++; return; }   // the backend stops on it
    for (int i = 0; i < nb; i++) {
        const unsigned k = v[i].b.kind, id = v[i].b.id; const uint8_t *c = v[i].content; uint32_t bytes = v[i].b.bytes;
        m->st[NBK_S_BLOCKS]++;
        if (v[i].b.how == NBS_B_REF) {
            const nbk_def *d = defs ? &defs->d[k][id] : NULL;
            if (d && d->p && d->epoch == h.epoch) { c = d->p; bytes = d->bytes; }
            else if (!res || !(c = res(k, id, h.epoch, &bytes)) || !nbs_content_ok(k, c, bytes)) { m->lost |= 1ull << k; m->st[NBK_S_UNRESOLVED]++; continue; }
        } else if (v[i].b.how == NBS_B_DEF && defs) { defs->d[k][id].epoch = h.epoch; defs->d[k][id].bytes = bytes; defs->d[k][id].p = c; }
        const uint8_t *q = c;
        if (nbk_walk_seq(m, defs, res, &q, NULL, depth + 1, guard) != NBK_W_OK) m->lost |= 1ull << k;
    }
}
// Records from *pos: at depth 0 up to `to` (exclusive; *pos is left there), nested (to NULL) up to their OP_END.
static inline int nbk_walk_seq(nbk_mirror *m, nbk_defs *defs, nbk_resolve_fn res, const uint8_t **pos, const uint8_t *to, int depth, uint32_t *guard) {
    const uint8_t *pc = *pos;
    if (!pc || depth > NBK_DEPTH) return NBK_W_BAD;
    for (;;) {
        if (!depth && pc == to) { *pos = pc; return NBK_W_OK; }
        if (++*guard > NBK_GUARD) { *pos = pc; return NBK_W_GUARD; }
        uint32_t hd[2]; memcpy(hd, pc, 8);
        const uint32_t op = hd[0], size = hd[1];
        if (op == OP_END) { *pos = pc; return depth ? NBK_W_OK : NBK_W_END; }
        if (size < 8 || (size & 7) || size > (64u << 20)) { *pos = pc; return NBK_W_BAD; }
        const uint8_t *pl = pc + 8; const uint32_t n = size - 8;
        if (op == OP_JUMP) {
            uint64_t t; if (n < 8) { *pos = pc; return NBK_W_BAD; }
            memcpy(&t, pl, 8); pc = (const uint8_t *)(uintptr_t)t;
            if (!pc) { *pos = pc; return NBK_W_BAD; }
            continue;
        }
        m->st[NBK_S_WALKED]++;
        if (op == OP_CALL || op == OP_X_NATIVE_DRAW) {
            const uint8_t *q = pl;
            if (op == OP_CALL) { uint64_t t; if (n < 8) { *pos = pc; return NBK_W_BAD; } memcpy(&t, pl, 8); q = (const uint8_t *)(uintptr_t)t; }
            m->st[NBK_S_NESTED]++;
            const int r = nbk_walk_seq(m, defs, res, &q, NULL, depth + 1, guard);
            if (r != NBK_W_OK) { *pos = pc; return r == NBK_W_END ? NBK_W_BAD : r; }
        }
        else if (op == OP_X_NB_STATE) nbk_state_rec(m, defs, res, pl, n, depth, guard);
        else if (op == OP_X_NB_DRAW) nbk_nbdraw(m, pl, n);
        else nbk_file(m, pc, size);
        pc += size;
    }
}
static inline int nbk_walk(nbk_mirror *m, nbk_defs *defs, nbk_resolve_fn res, const uint8_t **pos, const uint8_t *to) {
    uint32_t guard = 0;
    return nbk_walk_seq(m, defs, res, pos, to, 0, &guard);
}

// ---- the key ----
static inline uint32_t nbk_put(uint8_t *dst, uint32_t cap, uint32_t off, uint32_t op, const void *pl, uint32_t n) {   // 0 = no room
    const uint32_t size = (8 + n + 7) & ~7u;
    if (off + size + 8 > cap) return 0;
    const uint32_t h[2] = {op, size}; memcpy(dst + off, h, 8); memcpy(dst + off + 8, pl, n); memset(dst + off + 8 + n, 0, size - 8 - n);
    return off + size;
}
// Content of unit u (records + OP_END) into dst (cap bytes). 0 = nothing set for the unit; *full = it did not fit.
static inline uint32_t nbk_content(const nbk_mirror *m, unsigned u, uint8_t *dst, uint32_t cap, int *full) {
    uint32_t off = 0; *full = 0;
    if (u < NBK_LISTS) {
        const nbk_list *s = &m->l[u];
        if (!s->n) return 0;
        if ((uint32_t)s->used + 8 > cap) { *full = 1; return 0; }
        memcpy(dst, s->buf, s->used); off = s->used;
    } else if (u < NBK_U_UBO) {
        const unsigned g = (u - NBK_U_TEX) / 6, st = (u - NBK_U_TEX) % 6, lim = g == 2 ? 16 : 128;
        const uint32_t op = g == 0 ? OP_BIND_TEX : g == 1 ? OP_BIND_SAMP : OP_BIND_IMG;
        const uint64_t *val = g == 0 ? m->tex[st] : g == 1 ? m->samp[st] : m->img[st];
        for (unsigned s = 0; s < lim;) {
            const int on = g == 2 ? (m->img_set[st] >> s & 1) : (int)((g ? m->samp_set : m->tex_set)[st][s >> 6] >> (s & 63) & 1);
            if (!on) { s++; continue; }
            unsigned e = s;
            while (e < lim && e - s < 32 && (g == 2 ? (m->img_set[st] >> e & 1) : (int)((g ? m->samp_set : m->tex_set)[st][e >> 6] >> (e & 63) & 1))) e++;
            uint8_t pl[16 + 8 * 32]; const int32_t a[4] = {(int32_t)st, (int32_t)s, (int32_t)(e - s), 0};
            memcpy(pl, a, 16); memcpy(pl + 16, val + s, 8 * (size_t)(e - s));
            if (!(off = nbk_put(dst, cap, off, op, pl, 16 + 8 * (e - s)))) { *full = 1; return 0; }
            s = e;
        }
        if (!off) return 0;
    } else {
        const int ubo = u < NBK_U_SSBO; const unsigned st = (u - (ubo ? NBK_U_UBO : NBK_U_SSBO));
        const uint16_t set = ubo ? m->ub_set[st] : m->sb_set[st];
        if (!set) return 0;
        for (unsigned s = 0; s < 16; s++) if (set >> s & 1) {
            const nbk_range *r = ubo ? &m->ub[st][s] : &m->sb[st][s];
            uint8_t pl[24]; const int32_t a[2] = {(int32_t)st, (int32_t)s}; memcpy(pl, a, 8); memcpy(pl + 8, &r->a, 8); memcpy(pl + 16, &r->b, 8);
            if (!(off = nbk_put(dst, cap, off, ubo ? OP_BIND_UBO : OP_BIND_SSBO, pl, 24))) { *full = 1; return 0; }
        }
    }
    if (off + 8 > cap) { *full = 1; return 0; }
    const uint32_t end[2] = {0, 8}; memcpy(dst + off, end, 8);
    return off + 8;
}
// The key payload into dst (cap bytes). def_off[u] = offset of the unit's nbk_unit when it is a DEF (0 otherwise), for nbk_commit.
// lost_all: the walk lost the stream (NBK_F_LOST, no units). Units that do not fit become lost. Returns the payload bytes.
static inline uint32_t nbk_pack(nbk_mirror *m, uint32_t seq, int node, uint32_t walked, int lost_all, uint8_t *dst, uint32_t cap,
                                uint32_t def_off[NBK_UNITS], unsigned *ndef, unsigned *nref) {
    nbk_hdr h; memset(&h, 0, sizeof h);
    h.seq = seq; h.node = (int16_t)node; h.ver = NBK_VER; h.flags = lost_all ? NBK_F_LOST : 0; h.walked = walked;
    *ndef = *nref = 0;
    for (int u = 0; u < NBK_UNITS; u++) def_off[u] = 0;
    if (cap < sizeof h) return 0;
    uint32_t off = sizeof h;
    for (unsigned u = 0; u < NBK_UNITS && !lost_all; u++) {
        if (m->lost >> u & 1) continue;
        int full = 0; uint32_t bytes = 0;
        if (off + sizeof(nbk_unit) + 8 <= cap) {
            const uint32_t room = cap - off - (uint32_t)sizeof(nbk_unit);
            bytes = nbk_content(m, u, dst + off + sizeof(nbk_unit), room < NBK_UNIT_MAX ? room : NBK_UNIT_MAX, &full);
        } else full = 1;
        if (full) { m->lost |= 1ull << u; continue; }   // stays lost until the next FRAME (a later key cannot be complete either)
        if (!bytes) continue;
        nbk_unit un; memset(&un, 0, sizeof un); un.magic = NBK_MAGIC; un.unit = (uint8_t)u; un.seq = seq;
        const uint8_t *last = m->last[u]; nbk_unit lu;
        if (last) memcpy(&lu, last, sizeof lu);
        if (last && lu.bytes == bytes && !memcmp(last + sizeof lu, dst + off + sizeof un, bytes)) {
            un.how = NBK_H_REF; un.bytes = 8; memcpy(dst + off, &un, sizeof un);
            const uint64_t a = (uint64_t)(uintptr_t)last; memcpy(dst + off + sizeof un, &a, 8);
            off += (uint32_t)sizeof un + 8; ++*nref;
        } else {
            un.how = NBK_H_DEF; un.bytes = bytes; memcpy(dst + off, &un, sizeof un);
            def_off[u] = off; off += (uint32_t)sizeof un + bytes; ++*ndef;
        }
        h.nunit++;
    }
    h.lost = lost_all ? ~0ull >> (64 - NBK_UNITS) : m->lost;
    memcpy(dst, &h, sizeof h);
    return off;
}
// After the payload was copied to its record (payload = its address in the command memory): the DEFs become the units' last emission.
static inline void nbk_commit(nbk_mirror *m, const uint8_t *payload, const uint32_t def_off[NBK_UNITS]) {
    for (int u = 0; u < NBK_UNITS; u++) if (def_off[u]) m->last[u] = payload + def_off[u];
}

// ---- reading a key (backend, test) ----
// Content of unit u: 8-aligned records of that unit only (binds within the backend's limits), ended by one OP_END at its last 8 bytes.
static inline int nbk_content_ok(unsigned u, const uint8_t *c, uint32_t bytes) {
    if (u >= NBK_UNITS || bytes < 16 || (bytes & 7) || bytes > NBK_UNIT_MAX) return 0;
    uint32_t off = 0;
    while (off < bytes) {
        uint32_t hd[2]; memcpy(hd, c + off, 8);
        if (hd[1] < 8 || (hd[1] & 7) || hd[1] > bytes - off) return 0;
        if (hd[0] == 0) return hd[1] == 8 && off + 8 == bytes && off > 0;
        if (nbk_unit_of(hd[0], c + off + 8, hd[1] - 8) != (int)u) return 0;
        off += hd[1];
    }
    return 0;
}
typedef struct { nbk_unit u; const uint8_t *content; uint32_t bytes; } nbk_view;   // u.how DEF or REF; content = the DEF's records
enum { NBK_P_BAD = -1, NBK_P_REF = -2 };
// Walks a payload of n bytes (may include the 8-byte alignment tail). Returns the number of units, NBK_P_BAD = malformed, NBK_P_REF = a
// REF that does not point to a DEF of its unit and IR part.
static inline int nbk_parse(const uint8_t *p, size_t n, nbk_hdr *h, nbk_view out[NBK_UNITS]) {
    if (!p || n < sizeof *h) return NBK_P_BAD;
    memcpy(h, p, sizeof *h);
    if (h->ver != NBK_VER || h->nunit > NBK_UNITS || ((h->flags & NBK_F_LOST) && h->nunit)) return NBK_P_BAD;
    size_t off = sizeof *h; uint64_t seen = 0;
    for (unsigned i = 0; i < h->nunit; i++) {
        nbk_unit u;
        if (n - off < sizeof u) return NBK_P_BAD;
        memcpy(&u, p + off, sizeof u); off += sizeof u;
        if (u.magic != NBK_MAGIC || u.unit >= NBK_UNITS || (seen >> u.unit & 1) || (h->lost >> u.unit & 1) || u.seq != h->seq) return NBK_P_BAD;
        seen |= 1ull << u.unit;
        nbk_view *v = &out[i]; v->u = u;
        if (u.how == NBK_H_DEF) {
            if (n - off < u.bytes || !nbk_content_ok(u.unit, p + off, u.bytes)) return NBK_P_BAD;
            v->content = p + off; v->bytes = u.bytes; off += u.bytes;
        } else if (u.how == NBK_H_REF) {
            uint64_t a; nbk_unit d;
            if (u.bytes != 8 || n - off < 8) return NBK_P_BAD;
            memcpy(&a, p + off, 8); off += 8;
            const uint8_t *q = (const uint8_t *)(uintptr_t)a;
            if (!q || (a & 7)) return NBK_P_REF;
            memcpy(&d, q, sizeof d);
            if (d.magic != NBK_MAGIC || d.how != NBK_H_DEF || d.unit != u.unit || d.seq != h->seq || !nbk_content_ok(u.unit, q + sizeof d, d.bytes)) return NBK_P_REF;
            v->content = q + sizeof d; v->bytes = d.bytes;
        } else return NBK_P_BAD;
    }
    return (int)h->nunit;
}
#endif
