// [DOS2] NB-09: OP_X_NB_DRAW, one engine draw as one record (docs/dos2/NATIVE_BACKEND_PLAN.md §2, NB-09 / NB-10). Pure C (C and C++,
// no NVN / Vulkan types): included by nvn_obj.h, packed by the NVN tap (nvn_nb_tap.inc, inside hle_nvn.c), unpacked by the backend
// (xbg_nb.inc nbdraw) and by the host test dm/gfx/vulkan/engine/tests/nb_draw_test.c.
//
// Inside the engine's draw bracket (0x928e80 DrawElements = opcode 36, 0x928fb0 DrawElementsInstanced = 37; hle_dos2_native.c) with the
// opcode's bit set in DM_NB_OPS, the binds the renderer's flush makes (BindTexture / BindSeparateSampler / BindImage with one handle,
// BindUniformBuffer(s)), SetRenderEnable(Conditional) and the draw itself (DrawElements / DrawElementsBaseVertex / DrawElementsInstanced /
// DrawArrays) do not write their NVN records: they go to a shadow (nbd_shadow) that becomes one OP_X_NB_DRAW at the draw. Any other
// record inside the bracket first flushes the shadow as the ordinary NVN records ("foreign").
//
// Layout (payload after op_hdr, little-endian, 8-aligned):
//   nbd_hdr (64 bytes)            ver, engine opcode, draw kind, flags, nbind, render-enable state, texture-set generation, draw arguments
//   nbd_bind bind[nbind] (24 each) in call order: kind (texture / sampler / image / UBO), stage, slot, handle or {address, size}
// The binds keep the call order and are not merged: the faithful unpack (NB-09 backend) replays them as the same NVN records one by one, so
// the backend's tables and generation counters (tex_gen / buf_gen) end exactly as the old stream left them.
#ifndef DM_NVN_NB_DRAW_H
#define DM_NVN_NB_DRAW_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { NBD_VER = 1, NBD_MAX_BINDS = 64 };
enum { NBD_DRAW_NONE = 0, NBD_DRAW_IDX = 1, NBD_DRAW_ARRAYS = 2 };   // IDX = OP_DRAW_IDX (p_draw_idx), ARRAYS = OP_DRAW (p_draw)
enum { NBD_B_TEX = 0, NBD_B_SAMP = 1, NBD_B_IMG = 2, NBD_B_UBO = 3, NBD_B_KINDS };
// flags: SetRenderEnable seen in the bracket (+ its value), SetRenderEnableConditional seen (mode / address below). The host executes
// neither (hle_nvn.c logs them once and draws anyway), so the unpack ignores them; NB-10+ may use them.
// NBD_F_CHECK (NB-10, DM_NB_OPS_CHECK): the old NVN records of this draw were written too (binds before this record, the draw record right
// after it); the backend executes those and only compares (xbg_nb.inc nbdraw::check), it does not draw this record.
enum { NBD_F_REN = 1, NBD_F_REN_ON = 2, NBD_F_COND = 4, NBD_F_CHECK = 8 };
typedef struct {
    uint8_t ver, engine_op, draw, flags;      // NBD_VER; 36 / 37; NBD_DRAW_*; NBD_F_*
    uint16_t nbind; uint8_t cond_mode, _pad0;
    uint32_t texgen, _pad1;                    // texture-set generation (see nvn_nb_tap.inc: bumped by every texture / sampler / image bind and pool change)
    int32_t prim, itype, count, base_vertex, base_instance, instances, first, _pad2;   // draw arguments (first: DrawArrays only)
    uint64_t ibuf, cond_addr;                  // index buffer (IDX); SetRenderEnableConditional address
} nbd_hdr;
typedef struct { uint8_t kind, stage, slot, _pad[5]; uint64_t a, b; } nbd_bind;   // TEX/SAMP/IMG: a = handle; UBO: a = address, b = size
typedef struct { nbd_hdr h; nbd_bind bind[NBD_MAX_BINDS]; } nbd_shadow;

#ifdef __cplusplus
static_assert(sizeof(nbd_hdr) == 64 && sizeof(nbd_bind) == 24, "OP_X_NB_DRAW layout");
#else
_Static_assert(sizeof(nbd_hdr) == 64 && sizeof(nbd_bind) == 24, "OP_X_NB_DRAW layout");
#endif

static inline uint32_t nbd_size(const nbd_shadow *s) { return (uint32_t)(sizeof(nbd_hdr) + sizeof(nbd_bind) * (size_t)s->h.nbind); }
// Bind slot limits the backend accepts (xbg_game.cpp execute: UBO / image 16 slots, texture / sampler 128, stages 0..5).
static inline int nbd_bind_ok(unsigned kind, int stage, int slot) {
    return kind < NBD_B_KINDS && stage >= 0 && stage < 6 && slot >= 0 && slot < ((kind == NBD_B_UBO || kind == NBD_B_IMG) ? 16 : 128);
}
// Appends a bind (call order kept). 0 = the shadow is full or the bind is outside the limits (the caller writes it as a normal record).
static inline int nbd_add(nbd_shadow *s, unsigned kind, int stage, int slot, uint64_t a, uint64_t b) {
    if (s->h.nbind >= NBD_MAX_BINDS || !nbd_bind_ok(kind, stage, slot)) return 0;
    nbd_bind *e = &s->bind[s->h.nbind++];
    memset(e, 0, sizeof *e); e->kind = (uint8_t)kind; e->stage = (uint8_t)stage; e->slot = (uint8_t)slot; e->a = a; e->b = b;
    return 1;
}
// Writes the record payload (cap bytes at dst). Returns the bytes written, 0 if it does not fit.
static inline uint32_t nbd_pack(const nbd_shadow *s, uint8_t *dst, size_t cap) {
    const uint32_t n = nbd_size(s);
    if (s->h.nbind > NBD_MAX_BINDS || cap < n) return 0;
    nbd_hdr h = s->h; h.ver = NBD_VER; h._pad0 = 0; h._pad1 = 0; h._pad2 = 0;
    memcpy(dst, &h, sizeof h);
    memcpy(dst + sizeof h, s->bind, sizeof(nbd_bind) * (size_t)s->h.nbind);
    return n;
}
// Reads a record payload of n bytes (n may include the 8-byte alignment tail). 0 = ok, -1 = malformed (version, count, sizes, a bind or
// draw kind outside the limits).
static inline int nbd_unpack(const uint8_t *p, size_t n, nbd_shadow *out) {
    if (!p || n < sizeof(nbd_hdr)) return -1;
    memcpy(&out->h, p, sizeof out->h);
    if (out->h.ver != NBD_VER || out->h.nbind > NBD_MAX_BINDS || out->h.draw > NBD_DRAW_ARRAYS) return -1;
    if (n < nbd_size(out)) return -1;
    memcpy(out->bind, p + sizeof(nbd_hdr), sizeof(nbd_bind) * (size_t)out->h.nbind);
    for (unsigned i = 0; i < out->h.nbind; i++) if (!nbd_bind_ok(out->bind[i].kind, out->bind[i].stage, out->bind[i].slot)) return -1;
    return 0;
}

// ---- NB-10: the record applied straight to the backend's tables (xbg_nb.inc nbdraw::apply; host test nb_draw_test.c) ----
// nbd_view: the header of a record payload of n bytes (copied to *h) and a pointer to its binds in place, no copy of the binds. 0 = ok,
// -1 = malformed (as nbd_unpack, without the per-bind limits: nbd_apply checks those while it writes).
static inline int nbd_view(const uint8_t *p, size_t n, nbd_hdr *h, const uint8_t **binds) {
    if (!p || n < sizeof(nbd_hdr)) return -1;
    memcpy(h, p, sizeof *h);
    if (h->ver != NBD_VER || h->nbind > NBD_MAX_BINDS || h->draw > NBD_DRAW_ARRAYS || n < sizeof(nbd_hdr) + sizeof(nbd_bind) * (size_t)h->nbind) return -1;
    *binds = p + sizeof(nbd_hdr);
    return 0;
}
typedef struct { uint64_t addr, size; } nbd_range;   // = NVNbufferRange {address, size} (the backend's state.ub)
// The binds of one record, in call order, into the tables: the last write to a slot wins, exactly as the old records one by one did. No
// record is built per bind. *tex_stages / *buf_stages get bit st for each stage whose texture set (texture / sampler / image) or uniform
// buffers were written: the backend bumps tex_gen[st] / buf_gen[st] once per such stage per record, where the old records bumped once per
// bind (every reader of those counters - driver_block, the uniform-eval key, the descriptor memo key - only asks "changed since", and no
// reader runs between the binds of one record). 0 = ok, -1 = a bind outside the limits (skipped, nothing written for it).
static inline int nbd_apply(const nbd_hdr *h, const uint8_t *binds, uint64_t (*tex)[128], uint64_t (*samp)[128], uint64_t (*img)[16],
                            nbd_range (*ub)[16], unsigned *tex_stages, unsigned *buf_stages) {
    unsigned ts = 0, bs = 0; int bad = 0;
    for (unsigned i = 0; i < h->nbind; i++) {
        nbd_bind b; memcpy(&b, binds + sizeof b * i, sizeof b);
        const unsigned st = b.stage, sl = b.slot;
        if (st >= 6) { bad = 1; continue; }
        switch (b.kind) {
        case NBD_B_TEX:  if (sl < 128) { tex[st][sl] = b.a; ts |= 1u << st; } else bad = 1; break;
        case NBD_B_SAMP: if (sl < 128) { samp[st][sl] = b.a; ts |= 1u << st; } else bad = 1; break;
        case NBD_B_IMG:  if (sl < 16) { img[st][sl] = b.a; ts |= 1u << st; } else bad = 1; break;
        case NBD_B_UBO:  if (sl < 16) { ub[st][sl].addr = b.a; ub[st][sl].size = b.b; bs |= 1u << st; } else bad = 1; break;
        default: bad = 1;
        }
    }
    *tex_stages = ts; *buf_stages = bs;
    return bad ? -1 : 0;
}
// DM_NB_OPS_CHECK: the tables after the old records against what the record would leave in them. Only the last write to each slot of the
// record is compared (an earlier write to the same slot is overwritten in both paths). Returns a mask of 1 << NBD_B_* kinds that differ
// (bit NBD_B_KINDS: a bind outside the limits); the first difference goes to *first (if not NULL).
typedef struct { unsigned kind, stage, slot; uint64_t want_a, want_b, got_a, got_b; } nbd_diff;
static inline unsigned nbd_compare(const nbd_hdr *h, const uint8_t *binds, const uint64_t (*tex)[128], const uint64_t (*samp)[128],
                                   const uint64_t (*img)[16], const nbd_range (*ub)[16], nbd_diff *first) {
    unsigned bad = 0;
    for (unsigned i = 0; i < h->nbind; i++) {
        nbd_bind b; memcpy(&b, binds + sizeof b * i, sizeof b);
        if (!nbd_bind_ok(b.kind, b.stage, b.slot)) { bad |= 1u << NBD_B_KINDS; continue; }
        int later = 0;
        for (unsigned j = i + 1; j < h->nbind && !later; j++) {
            nbd_bind c; memcpy(&c, binds + sizeof c * j, sizeof c);
            later = c.kind == b.kind && c.stage == b.stage && c.slot == b.slot;
        }
        if (later) continue;
        uint64_t ga, gb = 0;
        switch (b.kind) {
        case NBD_B_TEX: ga = tex[b.stage][b.slot]; break;
        case NBD_B_SAMP: ga = samp[b.stage][b.slot]; break;
        case NBD_B_IMG: ga = img[b.stage][b.slot]; break;
        default: ga = ub[b.stage][b.slot].addr; gb = ub[b.stage][b.slot].size; break;
        }
        const uint64_t wb = b.kind == NBD_B_UBO ? b.b : 0;
        if (ga != b.a || gb != wb) {
            if (!bad && first) { first->kind = b.kind; first->stage = b.stage; first->slot = b.slot; first->want_a = b.a; first->want_b = wb; first->got_a = ga; first->got_b = gb; }
            bad |= 1u << b.kind;
        }
    }
    return bad;
}
// DM_NB_OPS_CHECK: the old draw record against the record's draw arguments. kind = NBD_DRAW_IDX (d = OP_DRAW_IDX payload fields) or
// NBD_DRAW_ARRAYS (prim, first, count). 1 = the same draw.
static inline int nbd_same_draw(const nbd_hdr *h, int kind, int prim, int itype, int count, int bv, int bi, int inst, uint64_t ib, int first) {
    if (h->draw != kind || h->prim != prim || h->count != count) return 0;
    if (kind == NBD_DRAW_ARRAYS) return h->first == first;
    return h->itype == itype && h->base_vertex == bv && h->base_instance == bi && h->instances == inst && h->ibuf == ib;
}
#endif
