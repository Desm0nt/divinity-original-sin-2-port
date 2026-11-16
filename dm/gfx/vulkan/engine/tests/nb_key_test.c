// [DOS2] NB-14 host test: OP_X_NB_KEY walk, mirror, pack and parse (dm/gfx/nvn/nvn_nb_key.h).
// - list: last-write order per key, replacement moves to the end, full -> refused;
// - round trip: random command streams written the way hle_nvn.c writes them (segments chained by OP_JUMP, OP_CALL handles, OP_X_NATIVE_DRAW
//   wrappers, OP_X_NB_STATE blocks from a real NB-13a shadow with DEF / REF / NOID and epochs, OP_X_NB_DRAW binds incl. check-only records,
//   plain state / bind / misc records, draws, other records), several IR parts (FRAME) and passes; the tap's steps at every PASS (walk, pack,
//   KEY right after PASS, commit, walk past the KEY). Then a backend model executes the whole stream as xbg_game.cpp does: base at FRAME, and
//   at every KEY: base + the key's units (executed record by record) must equal the live state on every field of a unit that is not lost;
// - REF: an unchanged unit is a REF to its last DEF (two passes in a row: every unit REF); the parser follows it in any order;
// - lost: recording end before the pass (void key), a full list (VB with 70 keys), an OP_X_NB_STATE REF whose DEF lies before the FRAME
//   without / with the dictionary fallback;
// - mutation: a mirror that forgets sampler binds is caught;
// - malformed keys rejected (magic, unit twice, lost + given, REF to no DEF, content of another unit, no OP_END, truncation, version).
// Build + run (from this directory):
//   clang -std=c11 -Wall -Wextra -DDM_GAME_DOS2 -D_CRT_SECURE_NO_WARNINGS -I../../../nvn/include nb_key_test.c -o nb_key_test && ./nb_key_test
#include "../../../nvn/nvn_obj.h"
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)
static uint64_t rnd_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 7; rnd_state ^= rnd_state << 17; return rnd_state; }
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }

// ---- command memory as hle_nvn.c lays it out: segments, a JUMP at the end of each (OP_RESERVE 24) ----
enum { SEGSZ = 4096, NSEG = 4096 };
static uint64_t g_mem[NSEG][SEGSZ / 8];
static int g_seg;
typedef struct { uint8_t *cur, *end; } wr;
static uint8_t *new_seg_n(int k) { if (g_seg + k > NSEG) { printf("out of test segments\n"); exit(2); } uint8_t *p = (uint8_t *)g_mem[g_seg]; g_seg += k; return p; }   // k blocks in a row
static uint8_t *new_seg(void) { return new_seg_n(1); }
static void w_init(wr *w) { w->cur = new_seg(); w->end = w->cur + SEGSZ; }
static uint8_t *w_alloc(wr *w, uint32_t size) {
    size = (size + 7) & ~7u;
    if (w->cur + size + 24 > w->end) {   // as cmd_alloc: the game adds a segment of at least size + OP_RESERVE
        const int k = (int)((size + 24 + SEGSZ - 1) / SEGSZ);
        uint8_t *s = new_seg_n(k); const uint32_t h[2] = {OP_JUMP, 16}; memcpy(w->cur, h, 8); const uint64_t t = (uint64_t)(uintptr_t)s; memcpy(w->cur + 8, &t, 8);
        w->cur = s; w->end = s + (size_t)k * SEGSZ;
    }
    uint8_t *p = w->cur; w->cur += size; memset(p, 0, size); const uint32_t h[2] = {0, size}; memcpy(p, h, 8); return p;
}
static uint8_t *w_rec(wr *w, uint32_t op, const void *pl, uint32_t n) {
    uint8_t *p = w_alloc(w, 8 + n); memcpy(p, &op, 4); if (n) memcpy(p + 8, pl, n); return p + 8;
}

// ---- record builders (valid for the backend) ----
static uint32_t mk_bind_state(uint8_t *pl, int kind, uint64_t v) {
    p_bind_state s; memset(&s, 0, sizeof s); s.kind = kind;
    if (kind == 0) { blend_st b; memset(&b, 0, sizeof b); b.target = v & 7; b.src_rgb = (v >> 3) & 127; b.dst_a = (v >> 10) & 127; memcpy(s.bytes, &b, sizeof b); }
    else if (kind == 3) { for (int i = 0; i < 10; i++) s.bytes[i] = (uint8_t)(v >> i); }
    else { const uint32_t x = (uint32_t)v; memcpy(s.bytes, &x, 4); if (kind == 5) { const uint32_t y = (uint32_t)(v >> 32); memcpy(s.bytes + 4, &y, 4); } }
    memcpy(pl, &s, sizeof s); return sizeof s;
}
static const prog_t *fake_prog(unsigned i) { return (const prog_t *)(uintptr_t)(0x10000u + 0x100u * i); }
static const tex_t *fake_tex(unsigned i) { return i ? (const tex_t *)(uintptr_t)(0x200000u + 0x40u * i) : NULL; }
static dpool_t g_pools[4];
// One random state / bind / misc record into pl (payload), returns op (payload bytes in *n). vb_wide: VB keys from a wide set.
static uint32_t rnd_state_rec(uint8_t *pl, uint32_t *n, int vb_wide) {
    switch (rn(22)) {
    case 0: case 1: *n = mk_bind_state(pl, (int)rn(6), rnd()); return OP_BIND_STATE;
    case 2: { p_stencil s = {(int32_t)(1 + rn(3)), (int32_t)rn(256), (int32_t)rn(256), (int32_t)rn(256), (int32_t)rn(3)}; memcpy(pl, &s, sizeof s); *n = sizeof s; return OP_STENCIL; }
    case 3: { p_poly_offset o = {(float)rn(9), (float)rn(5), 0.f}; memcpy(pl, &o, sizeof o); *n = sizeof o; return OP_POLY_OFFSET; }
    case 4: { p_blend_color c = {{(float)rn(4), 0, 0, 1}}; memcpy(pl, &c, sizeof c); *n = sizeof c; return OP_BLEND_COLOR; }
    case 5: { p_alpha_ref a = {(float)rn(7) / 8.f, 0}; memcpy(pl, &a, sizeof a); *n = sizeof a; return OP_X_ALPHA_REF; }
    case 6: { p_bind_vattr b; memset(&b, 0, sizeof b); b.n = (int32_t)rn(17); for (int i = 0; i < b.n; i++) b.st[i] = (uint32_t)rnd(); *n = (uint32_t)(offsetof(p_bind_vattr, st) + 4 * (size_t)b.n); memcpy(pl, &b, *n); return OP_BIND_VATTR; }
    case 7: { p_bind_vstream b; memset(&b, 0, sizeof b); b.first = (int32_t)rn(4); b.n = (int32_t)(1 + rn(3)); for (int i = 0; i < b.n; i++) { b.st[i].stride = (uint32_t)rn(64); b.st[i].divisor = (uint32_t)rn(2); } *n = (uint32_t)(offsetof(p_bind_vstream, st) + sizeof(vs_st) * (size_t)b.n); memcpy(pl, &b, *n); return OP_BIND_VSTREAM; }
    case 8: { p_bind_vbuf b; memset(&b, 0, sizeof b); b.first = (int32_t)(vb_wide ? rn(16) : rn(3)); b.n = (int32_t)(1 + rn(vb_wide ? (unsigned)(16 - b.first) : 2u)); for (int i = 0; i < b.n; i++) { b.r[i].address = rnd() & ~0xffull; b.r[i].size = 64 + rn(64); } *n = (uint32_t)(offsetof(p_bind_vbuf, r) + sizeof b.r[0] * (size_t)b.n); memcpy(pl, &b, *n); return OP_BIND_VBUF; }
    case 9: { p_bind_program b; memset(&b, 0, sizeof b); b.prog = rn(8) ? fake_prog(rn(40)) : NULL; b.stages = (int32_t)(rn(2) ? 0x1f : 1u << rn(6)); memcpy(pl, &b, sizeof b); *n = sizeof b; return OP_BIND_PROGRAM; }
    case 10: { p_viewports v; memset(&v, 0, sizeof v); v.first = 0; v.n = 1; v.v[0][0] = (float)rn(8); v.v[0][2] = (float)(100 + rn(9)); v.v[0][3] = 50; *n = (uint32_t)(offsetof(p_viewports, v) + sizeof v.v[0]); memcpy(pl, &v, *n); return OP_VIEWPORTS; }
    case 11: { p_scissor s = {(int32_t)rn(9), 0, 64, (int32_t)rn(40)}; memcpy(pl, &s, sizeof s); *n = sizeof s; return OP_SCISSOR; }
    case 12: { p_depth_range d = {(float)rn(3) / 4.f, 1.f}; memcpy(pl, &d, sizeof d); *n = sizeof d; return OP_DEPTH_RANGE; }
    case 13: { p_set_rt r; memset(&r, 0, sizeof r); r.n = (int32_t)rn(5); for (int i = 0; i < r.n; i++) { r.color[i] = fake_tex(rn(6)); r.has_cview[i] = (uint8_t)rn(2); if (r.has_cview[i]) r.cview[i].base_level = (int32_t)rn(3); } r.depth = fake_tex(rn(4)); memcpy(pl, &r, sizeof r); *n = sizeof r; return OP_SET_RT; }
    case 14: { p_set_pool s = {&g_pools[rn(4)]}; memcpy(pl, &s, sizeof s); *n = sizeof s; return rn(2) ? OP_SET_TEX_POOL : OP_SET_SAMP_POOL; }
    case 15: { p_copy_stride s = {(int64_t)rn(4096)}; memcpy(pl, &s, sizeof s); *n = sizeof s; return rn(2) ? OP_COPY_STRIDE : OP_COPY_IMAGE_STRIDE; }
    case 16: { uint32_t ps = 1 + rn(4); memcpy(pl, &ps, 4); *n = 4; return OP_X_PATCH_SIZE; }
    case 17: { xbg_depth_bounds b = {(uint32_t)rn(2), 0.f, (float)rn(2)}; memcpy(pl, &b, sizeof b); *n = sizeof b; return OP_X_DEPTH_BOUNDS; }
    case 18: { p_prim_restart r = {(uint32_t)rn(2), rn(2) ? 0xffffu : 0xffffffffu}; memcpy(pl, &r, sizeof r); *n = sizeof r; return OP_X_PRIM_RESTART; }
    case 19: { p_flag f = {(int32_t)rn(2)}; memcpy(pl, &f, sizeof f); *n = sizeof f; return rn(2) ? OP_X_DEPTH_CLAMP : OP_X_RAST_DISCARD; }
    case 20: { p_bind_handles b; memset(&b, 0, sizeof b); const uint32_t op = rn(3) == 0 ? OP_BIND_IMG : rn(2) ? OP_BIND_TEX : OP_BIND_SAMP; const int lim = op == OP_BIND_IMG ? 16 : 128;
               b.stage = (int32_t)rn(6); b.n = (int32_t)(rn(8) ? 1 + rn(4) : 1 + rn(32)); if (b.n > lim) b.n = lim; b.first = (int32_t)rn((unsigned)(lim - b.n + 1)); if (rn(2)) b.first %= 20;
               for (int i = 0; i < b.n; i++) b.h[i] = rnd() & 0xfffffffull; *n = (uint32_t)(offsetof(p_bind_handles, h) + 8 * (size_t)b.n); memcpy(pl, &b, *n); return op; }
    default: { p_bind_buf b = {(int32_t)rn(6), (int32_t)rn(16), rnd() & ~0xffull, 256 + rn(256)}; memcpy(pl, &b, sizeof b); *n = sizeof b; return rn(3) ? OP_BIND_UBO : OP_BIND_SSBO; }
    }
}

// ---- backend model: the fields of xbg_game.cpp State a record sets, same semantics as execute() ----
typedef struct {
    p_set_rt rt; uintptr_t shader[6]; int missing;
    NVNbufferRange ub[6][16], sb[6][16], vb[16];
    uint64_t tex[6][128], samp[6][128], img[6][16];
    dpool_t tp, sp;
    uint32_t attrib[16]; vs_st stream[16];
    blend_st blend[8]; float bcol[4];
    color_st color; chmask_st mask; ms_st ms; float aref;
    poly_st poly; float bias[3];
    ds_st ds; uint32_t scmp[2], swr[2], sref[2];
    float vp[6]; int32_t sc[4];
    int64_t cs, cis; uint32_t patch; xbg_depth_bounds bounds; p_prim_restart restart; uint32_t clamp, discard;
} model;
static void m_exec_rec(model *m, uint32_t op, const uint8_t *p, uint32_t n) {
    switch (op) {
    case OP_SET_RT: memcpy(&m->rt, p, sizeof m->rt); break;
    case OP_VIEWPORTS: { p_viewports a; memset(&a, 0, sizeof a); memcpy(&a, p, n < sizeof a ? n : sizeof a); for (int i = 0; i < 4; i++) m->vp[i] = a.v[0][i]; break; }
    case OP_SCISSOR: memcpy(m->sc, p, 16); break;
    case OP_DEPTH_RANGE: memcpy(&m->vp[4], p, 8); break;
    case OP_BIND_PROGRAM: { p_bind_program a; memcpy(&a, p, sizeof a); for (int s = 0; s < 6; s++) if (a.stages >> s & 1) m->shader[s] = 0;
        if (!a.prog) break; m->missing = (int)(((uintptr_t)a.prog >> 8) & 1); for (int s = 0; s < 6; s++) if (a.stages >> s & 1) m->shader[s] = (uintptr_t)a.prog ^ (uintptr_t)s; break; }
    case OP_BIND_STATE: { p_bind_state a; memcpy(&a, p, sizeof a);
        switch (a.kind) { case 0: { blend_st b; memcpy(&b, a.bytes, sizeof b); m->blend[b.target] = b; break; } case 1: memcpy(&m->color, a.bytes, sizeof m->color); break;
                          case 2: memcpy(&m->mask, a.bytes, sizeof m->mask); break; case 3: memcpy(&m->ms, a.bytes, sizeof m->ms); break;
                          case 4: memcpy(&m->poly, a.bytes, sizeof m->poly); break; default: memcpy(&m->ds, a.bytes, sizeof m->ds); }
        break; }
    case OP_BIND_VATTR: { p_bind_vattr a; memset(&a, 0, sizeof a); memcpy(&a, p, n < sizeof a ? n : sizeof a); memset(m->attrib, 0, sizeof m->attrib); memcpy(m->attrib, a.st, 4 * (size_t)a.n); break; }
    case OP_BIND_VSTREAM: { p_bind_vstream a; memset(&a, 0, sizeof a); memcpy(&a, p, n < sizeof a ? n : sizeof a); memcpy(m->stream + a.first, a.st, sizeof(vs_st) * (size_t)a.n); break; }
    case OP_BIND_VBUF: { p_bind_vbuf a; memset(&a, 0, sizeof a); memcpy(&a, p, n < sizeof a ? n : sizeof a); memcpy(m->vb + a.first, a.r, sizeof(NVNbufferRange) * (size_t)a.n); break; }
    case OP_BIND_UBO: case OP_BIND_SSBO: { p_bind_buf a; memcpy(&a, p, sizeof a); NVNbufferRange *r = op == OP_BIND_UBO ? &m->ub[a.stage][a.index] : &m->sb[a.stage][a.index]; r->address = a.addr; r->size = a.size; break; }
    case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: { p_bind_handles a; memset(&a, 0, sizeof a); memcpy(&a, p, n < sizeof a ? n : sizeof a);
        uint64_t *d = op == OP_BIND_IMG ? m->img[a.stage] : op == OP_BIND_SAMP ? m->samp[a.stage] : m->tex[a.stage]; memcpy(d + a.first, a.h, 8 * (size_t)a.n); break; }
    case OP_STENCIL: { p_stencil a; memcpy(&a, p, sizeof a); for (int i = 0; i < 2; i++) if (a.face & (1 << i)) { if (a.which == 0) m->scmp[i] = (uint32_t)a.value_mask; else if (a.which == 1) m->swr[i] = (uint32_t)a.mask; else m->sref[i] = (uint32_t)a.ref; } break; }
    case OP_POLY_OFFSET: { p_poly_offset a; memcpy(&a, p, sizeof a); m->bias[0] = a.factor; m->bias[1] = a.units; m->bias[2] = a.clamp; break; }
    case OP_BLEND_COLOR: memcpy(m->bcol, p, 16); break;
    case OP_X_ALPHA_REF: memcpy(&m->aref, p, 4); break;
    case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: { p_set_pool a; memcpy(&a, p, sizeof a); if (op == OP_SET_TEX_POOL) m->tp = *a.pool; else m->sp = *a.pool; break; }
    case OP_COPY_STRIDE: memcpy(&m->cs, p, 8); break;
    case OP_COPY_IMAGE_STRIDE: memcpy(&m->cis, p, 8); break;
    case OP_X_PATCH_SIZE: memcpy(&m->patch, p, 4); break;
    case OP_X_DEPTH_BOUNDS: memcpy(&m->bounds, p, sizeof m->bounds); break;
    case OP_X_PRIM_RESTART: memcpy(&m->restart, p, sizeof m->restart); break;
    case OP_X_DEPTH_CLAMP: { p_flag f; memcpy(&f, p, sizeof f); m->clamp = f.on != 0; break; }
    case OP_X_RAST_DISCARD: { p_flag f; memcpy(&f, p, sizeof f); m->discard = f.on != 0; break; }
    default: break;
    }
}
// Fields of unit u equal? (memcmp over model fields: every one is written by memcpy-like copies, padding stays as zeroed)
static int m_unit_same(const model *a, const model *b, unsigned u) {
#define EQ(f) (!memcmp(&a->f, &b->f, sizeof a->f))
    if (u >= NBK_U_TEX && u < NBK_U_SAMP) return EQ(tex[u - NBK_U_TEX]);
    if (u >= NBK_U_SAMP && u < NBK_U_IMG) return EQ(samp[u - NBK_U_SAMP]);
    if (u >= NBK_U_IMG && u < NBK_U_UBO) return EQ(img[u - NBK_U_IMG]);
    if (u >= NBK_U_UBO && u < NBK_U_SSBO) return EQ(ub[u - NBK_U_UBO]);
    if (u >= NBK_U_SSBO) return EQ(sb[u - NBK_U_SSBO]);
    switch (u) {
    case NBK_U_BLEND: return EQ(blend) && EQ(bcol);
    case NBK_U_COLOR: return EQ(color) && EQ(mask) && EQ(ms) && EQ(aref);
    case NBK_U_POLY: return EQ(poly) && EQ(bias);
    case NBK_U_DS: return EQ(ds) && EQ(scmp) && EQ(swr) && EQ(sref);
    case NBK_U_VFMT: return EQ(attrib) && EQ(stream);
    case NBK_U_VB: return EQ(vb);
    case NBK_U_PROG: return EQ(shader) && EQ(missing);
    case NBK_U_VPSC: return EQ(vp) && EQ(sc);
    case NBK_U_RT: return EQ(rt);
    default: return EQ(tp) && EQ(sp) && EQ(cs) && EQ(cis) && EQ(patch) && EQ(bounds) && EQ(restart) && EQ(clamp) && EQ(discard);
    }
#undef EQ
}

// ---- backend model run: the whole stream as xbg_game.cpp executes it ----
static struct { uint32_t epoch; const uint8_t *p; uint32_t bytes; } g_bedef[NBS_KINDS][NBS_MAX_IDS];
typedef struct { uint64_t keys, void_keys, checked, units, bad_units, lost_units, def, ref, parse_bad, nobase; } be_stats;
static be_stats B;
static model g_live, g_base; static int g_have_base; static uint32_t g_base_seq;
static void be_exec(model *m, const uint8_t *pc, int depth, int top);
static void be_key(const uint8_t *pl, uint32_t n) {
    nbk_hdr h; nbk_view v[NBK_UNITS];
    const int nu = nbk_parse(pl, n, &h, v);
    B.keys++;
    if (nu < 0) { B.parse_bad++; return; }
    if (!g_have_base || h.seq != g_base_seq) { B.nobase++; return; }
    if (h.flags & NBK_F_LOST) { B.void_keys++; return; }
    model c; memcpy(&c, &g_base, sizeof c);
    for (int i = 0; i < nu; i++) { be_exec(&c, v[i].content, 1, 0); if (v[i].u.how == NBK_H_DEF) B.def++; else B.ref++; }
    B.checked++;
    for (unsigned u = 0; u < NBK_UNITS; u++) {
        if (h.lost >> u & 1) { B.lost_units++; continue; }
        B.units++;
        if (!m_unit_same(&c, &g_live, u)) { B.bad_units++; if (B.bad_units <= 8) printf("  key seq %u node %d: unit %u differs\n", h.seq, h.node, u); }
    }
}
static void be_exec(model *m, const uint8_t *pc, int depth, int top) {
    for (;;) {
        uint32_t hd[2]; memcpy(hd, pc, 8); const uint8_t *p = pc + 8; const uint32_t n = hd[1] - 8;
        switch (hd[0]) {
        case OP_END: {   // top level: a recording that ends and another one recorded right after it (no submit between) runs next
            if (!top) return;
            const uint8_t *q = pc + 8; uint32_t nh[2]; memcpy(nh, q, 8);
            if (nh[0] == OP_JUMP) { uint64_t t; memcpy(&t, q + 8, 8); q = (const uint8_t *)(uintptr_t)t; memcpy(nh, q, 8); }
            if (nh[0] != OP_BEGIN) return;
            pc = q; continue; }
        case OP_JUMP: { uint64_t t; memcpy(&t, p, 8); pc = (const uint8_t *)(uintptr_t)t; continue; }
        case OP_CALL: { uint64_t t; memcpy(&t, p, 8); be_exec(m, (const uint8_t *)(uintptr_t)t, depth + 1, 0); break; }
        case OP_X_NATIVE_DRAW: be_exec(m, p, depth + 1, 0); break;
        case OP_X_NB_STATE: { nbs_hdr h; nbs_view v[NBS_KINDS]; const int nb = nbs_parse(p, n, &h, v); CHECK(nb > 0);
            for (int i = 0; i < nb; i++) { const uint8_t *c = v[i].content;
                if (v[i].b.how == NBS_B_REF) { CHECK(g_bedef[v[i].b.kind][v[i].b.id].epoch == h.epoch); c = g_bedef[v[i].b.kind][v[i].b.id].p; }
                else if (v[i].b.how == NBS_B_DEF) { g_bedef[v[i].b.kind][v[i].b.id].epoch = h.epoch; g_bedef[v[i].b.kind][v[i].b.id].p = c; }
                if (c) be_exec(m, c, depth + 1, 0); }
            break; }
        case OP_X_NB_DRAW: { nbd_hdr h; const uint8_t *b; CHECK(nbd_view(p, n, &h, &b) == 0);
            if (!(h.flags & NBD_F_CHECK)) { unsigned ts, bs; CHECK(nbd_apply(&h, b, m->tex, m->samp, m->img, (nbd_range (*)[16])m->ub, &ts, &bs) == 0); }
            break; }
        case OP_X_NB_FRAME: if (top) { p_nb_frame f; memcpy(&f, p, sizeof f); memcpy(&g_base, m, sizeof g_base); g_have_base = 1; g_base_seq = f.seq; } break;
        case OP_X_NB_KEY: if (top) be_key(p, n); break;
        default: m_exec_rec(m, hd[0], p, n);
        }
        pc += hd[1];
    }
}

// ---- the tap's side (nvn_nb_key.inc nbk_frame / nbk_pass) ----
static nbk_mirror g_m; static nbk_defs g_defs; static const uint8_t *g_pos; static int g_lost; static uint32_t g_seq;
static uint8_t g_out[NBK_PAYLOAD_MAX];
static nbs_state g_sh;   // NB-13a shadow of the stream (its dictionary is the tap's fallback)
static int g_use_res = 1, g_mut;
static uint64_t g_keys_written, g_units_written, g_def_w, g_ref_w, g_bytes_w, g_lost_end, g_partial;
static const uint8_t *test_res(unsigned kind, unsigned id, uint32_t epoch, uint32_t *bytes) {
    if (!g_use_res || g_sh.epoch != epoch || id >= g_sh.d[kind].nid) return NULL;
    *bytes = g_sh.d[kind].id[id].size; return g_sh.d[kind].arena + g_sh.d[kind].id[id].off;
}
static void fe_frame(wr *w) {
    p_nb_frame f; memset(&f, 0, sizeof f); f.seq = ++g_seq; w_rec(w, OP_X_NB_FRAME, &f, sizeof f);
    nbk_reset(&g_m); g_pos = w->cur; g_lost = 0;
}
static uint8_t *fe_pass(wr *w, int node, unsigned *nd_out, unsigned *nr_out) {
    p_nb_pass r; memset(&r, 0, sizeof r); r.seq = g_seq; r.node = (int16_t)node; w_rec(w, OP_X_NB_PASS, &r, sizeof r);
    const uint64_t w0 = g_m.st[NBK_S_WALKED];
    if (!g_lost) { const int rc = nbk_walk(&g_m, &g_defs, test_res, &g_pos, w->cur); if (rc != NBK_W_OK) { g_lost = 1; if (rc == NBK_W_END) g_lost_end++; } }
    if (g_mut) memset(g_m.samp_set, 0, sizeof g_m.samp_set);   // mutation: forget sampler binds
    uint32_t def_off[NBK_UNITS]; unsigned nd, nr;
    const uint32_t n = nbk_pack(&g_m, g_seq, node, (uint32_t)(g_m.st[NBK_S_WALKED] - w0), g_lost, g_out, sizeof g_out, def_off, &nd, &nr);
    CHECK(n >= sizeof(nbk_hdr));
    uint8_t *p = w_rec(w, OP_X_NB_KEY, g_out, n);
    nbk_commit(&g_m, p, def_off);
    if (g_m.lost && !g_lost) g_partial++;
    g_keys_written++; g_units_written += nd + nr; g_def_w += nd; g_ref_w += nr; g_bytes_w += n;
    if (!g_lost) { const uint64_t w1 = g_m.st[NBK_S_WALKED]; CHECK(nbk_walk(&g_m, &g_defs, test_res, &g_pos, w->cur) == NBK_W_OK); CHECK(g_m.st[NBK_S_WALKED] - w1 == 1); }
    if (nd_out) *nd_out = nd;
    if (nr_out) *nr_out = nr;
    return p;
}
// NB-13a: emit the shadow's dirty kinds as OP_X_NB_STATE (as nbs_write)
static uint8_t g_shout[NBS_PAYLOAD_MAX];
static void sh_flush(wr *w) { const uint32_t n = nbs_pack(&g_sh, g_shout, sizeof g_shout); if (n) w_rec(w, OP_X_NB_STATE, g_shout, n); }
static void plain_state(wr *w, uint32_t op, const uint8_t *pl, uint32_t n) {   // a plain record: the shadow goes out first, its kind becomes unknown
    sh_flush(w); w_rec(w, op, pl, n);
    uint32_t key; const int k = nbs_kind(op, pl, n, &key); if (k >= 0) nbs_unknown(&g_sh, 1u << k); else if (nbs_op_kinds(op)) nbs_unknown(&g_sh, nbs_op_kinds(op));
}

// ---- one random stream ----
typedef struct { int vb_wide, end_inside; } opts;
static const uint8_t *g_handles[8]; static int g_nhandles;
static void make_handles(void) {
    g_nhandles = 0;
    for (int h = 0; h < 4; h++) {
        wr w; w_init(&w); uint8_t *b = w_alloc(&w, 8); memcpy(b, &(uint32_t){OP_BEGIN}, 4);
        for (int i = 0; i < 6; i++) { uint8_t pl[512]; uint32_t n; const uint32_t op = rnd_state_rec(pl, &n, 0); w_rec(&w, op, pl, n); }
        w_alloc(&w, 8);   // OP_END (op 0)
        g_handles[g_nhandles++] = b;
    }
}
static void one_stream(int steps, opts o) {
    wr w; w_init(&w);
    const uint8_t *start = w.cur; uint8_t *b = w_alloc(&w, 8); memcpy(b, &(uint32_t){OP_BEGIN}, 4);
    memset(&g_live, 0, sizeof g_live); g_have_base = 0;
    nbs_init(&g_sh, 0); { static uint32_t ep = 1; nbs_new_epoch(&g_sh, ep++); }
    int node = 0, in_part = 0;
    for (int s = 0; s < steps; s++) {
        const unsigned r = rn(100);
        uint8_t pl[1024]; uint32_t n;
        if (r < 3 || (!in_part && r < 10)) { sh_flush(&w); fe_frame(&w); in_part = 1; node = 0; continue; }
        if (r < 12 && in_part) {
            sh_flush(&w);   // the PASS record's cmd_alloc writes the pending shadow first (NBS_BEFORE)
            fe_pass(&w, node++, NULL, NULL);
            if (rn(6) == 0) { unsigned nd, nr; fe_pass(&w, node++, &nd, &nr); CHECK(nd == 0 || g_lost || g_mut); }   // nothing between: every unit REF
            continue;
        }
        if (r < 40) { const uint32_t op = rnd_state_rec(pl, &n, o.vb_wide); plain_state(&w, op, pl, n); continue; }
        if (r < 58) {   // a taken state call: into the shadow (bind / misc records are never captured: plain)
            const uint32_t op = rnd_state_rec(pl, &n, o.vb_wide);
            uint8_t rec[NBS_REC_MAX]; const uint32_t size = (8 + n + 7) & ~7u;
            if (size <= NBS_REC_MAX) { memset(rec, 0, size); memcpy(rec, &op, 4); memcpy(rec + 4, &size, 4); memcpy(rec + 8, pl, n);
                if (nbs_capture(&g_sh, rec, size) >= 0) continue; }
            plain_state(&w, op, pl, n); continue;
        }
        if (r < 64) { sh_flush(&w); static uint32_t ep = 100000; if (rn(3) == 0) nbs_new_epoch(&g_sh, ep++); continue; }   // a draw flushes; sometimes a new epoch
        if (r < 72) {   // OP_X_NB_DRAW (sometimes check-only: its old bind records first)
            sh_flush(&w);
            nbd_shadow d; memset(&d, 0, sizeof d); d.h.ver = NBD_VER; d.h.draw = NBD_DRAW_IDX; const int chk = rn(4) == 0; if (chk) d.h.flags = NBD_F_CHECK;
            const int nb = (int)rn(12);
            for (int i = 0; i < nb; i++) { const unsigned k = rn(4); const int st = (int)rn(6), sl = (int)rn(k == NBD_B_UBO || k == NBD_B_IMG ? 16 : 40);
                nbd_add(&d, k, st, sl, rnd() & 0xffffffull, 256);
                if (chk) { if (k == NBD_B_UBO) { p_bind_buf bb = {st, sl, d.bind[d.h.nbind - 1].a, 256}; w_rec(&w, OP_BIND_UBO, &bb, sizeof bb); }
                           else { p_bind_handles bh; memset(&bh, 0, sizeof bh); bh.stage = st; bh.first = sl; bh.n = 1; bh.h[0] = d.bind[d.h.nbind - 1].a;
                                  w_rec(&w, k == NBD_B_TEX ? OP_BIND_TEX : k == NBD_B_SAMP ? OP_BIND_SAMP : OP_BIND_IMG, &bh, (uint32_t)(offsetof(p_bind_handles, h) + 8)); } } }
            uint8_t dp[sizeof(nbd_shadow)]; const uint32_t dn = nbd_pack(&d, dp, sizeof dp); w_rec(&w, OP_X_NB_DRAW, dp, dn);
            continue;
        }
        if (r < 78) {   // DM_NATIVE_DRAW wrapper: records + OP_END inside one record
            sh_flush(&w);
            uint8_t buf[2048]; uint32_t used = 0; const int k = 1 + (int)rn(5);
            for (int i = 0; i < k; i++) { const uint32_t op = rnd_state_rec(pl, &n, 0); const uint32_t size = (8 + n + 7) & ~7u; if (used + size + 8 > sizeof buf) break;
                memset(buf + used, 0, size); memcpy(buf + used, &op, 4); memcpy(buf + used + 4, &size, 4); memcpy(buf + used + 8, pl, n); used += size; }
            const uint32_t e[2] = {0, 8}; memcpy(buf + used, e, 8); used += 8;
            w_rec(&w, OP_X_NATIVE_DRAW, buf, used);
            continue;
        }
        if (r < 82) { sh_flush(&w); const uint64_t t = (uint64_t)(uintptr_t)g_handles[rn((unsigned)g_nhandles)]; w_rec(&w, OP_CALL, &t, 8); continue; }   // CallCommands
        if (r < 84 && o.end_inside && in_part) { sh_flush(&w); w_alloc(&w, 8); uint8_t *bb = w_alloc(&w, 8); memcpy(bb, &(uint32_t){OP_BEGIN}, 4); continue; }   // recording end + a new one, no submit
        { sh_flush(&w); p_draw d = {4, 0, 3}; w_rec(&w, OP_DRAW, &d, sizeof d); uint64_t x[3] = {1, 2, 3}; w_rec(&w, OP_COPY_B2B, x, 24); }   // a draw + another record
    }
    sh_flush(&w);
    w_alloc(&w, 8);   // OP_END
    w_alloc(&w, 8);   // a zero record after it: the model's "next recording?" peek never reads stale bytes of a reused segment
    // the backend executes the submitted stream
    memset(&g_live, 0, sizeof g_live); g_have_base = 0;
    be_exec(&g_live, start, 0, 1);
}

static void test_list(void) {
    nbk_list l; memset(&l, 0, sizeof l);
    uint8_t r[24]; memset(r, 0, sizeof r);
    for (uint32_t k = 0; k < 3; k++) { r[8] = (uint8_t)k; const uint32_t h[2] = {OP_SCISSOR, 24}; memcpy(r, h, 8); CHECK(nbk_list_put(&l, k, r, 24) == 0); }
    r[8] = 9; CHECK(nbk_list_put(&l, 0, r, 24) == 0);   // key 0 again: replaced and moved to the end
    CHECK(l.n == 3 && l.e[0].key == 1 && l.e[1].key == 2 && l.e[2].key == 0 && l.buf[l.e[2].off + 8] == 9 && l.used == 72);
    for (uint32_t k = 3; k < NBK_LIST_ENTS; k++) CHECK(nbk_list_put(&l, k, r, 24) == 0);
    CHECK(nbk_list_put(&l, 999, r, 24) == -1 && l.n == NBK_LIST_ENTS);
}
static void test_parse(void) {
    // a key with one DEF (scissor) and a REF to it in a second key
    static uint64_t mem[1024]; uint8_t *base = (uint8_t *)mem;
    nbk_mirror *m = (nbk_mirror *)calloc(1, sizeof *m); nbk_reset(m);
    uint8_t rec[24]; memset(rec, 0, sizeof rec); const uint32_t h[2] = {OP_SCISSOR, 24}; memcpy(rec, h, 8); nbk_file(m, rec, 24);
    uint32_t off[NBK_UNITS]; unsigned nd, nr;
    const uint32_t n1 = nbk_pack(m, 7, 1, 1, 0, base, 2048, off, &nd, &nr); CHECK(nd == 1 && nr == 0); nbk_commit(m, base, off);
    uint8_t *k2 = base + 2048;
    const uint32_t n2 = nbk_pack(m, 7, 2, 0, 0, k2, 2048, off, &nd, &nr); CHECK(nd == 0 && nr == 1);
    nbk_hdr hh; nbk_view v[NBK_UNITS];
    CHECK(nbk_parse(base, n1, &hh, v) == 1 && v[0].u.how == NBK_H_DEF && v[0].u.unit == NBK_U_VPSC);
    CHECK(nbk_parse(k2, n2, &hh, v) == 1 && v[0].u.how == NBK_H_REF && v[0].content == base + sizeof(nbk_hdr) + sizeof(nbk_unit));
    uint8_t bad[2048];
    memcpy(bad, base, n1); bad[sizeof(nbk_hdr)] ^= 1; CHECK(nbk_parse(bad, n1, &hh, v) == NBK_P_BAD);                      // magic
    memcpy(bad, base, n1); bad[6] = 9; CHECK(nbk_parse(bad, n1, &hh, v) == NBK_P_BAD);                                    // version
    memcpy(bad, base, n1); { uint64_t lost = 1ull << NBK_U_VPSC; memcpy(bad + 16, &lost, 8); } CHECK(nbk_parse(bad, n1, &hh, v) == NBK_P_BAD);   // lost + given
    memcpy(bad, base, n1); CHECK(nbk_parse(bad, n1 - 8, &hh, v) == NBK_P_BAD);                                           // truncated (no OP_END)
    memcpy(bad, base, n1); { const uint32_t op = OP_BIND_UBO; memcpy(bad + sizeof(nbk_hdr) + sizeof(nbk_unit), &op, 4); } CHECK(nbk_parse(bad, n1, &hh, v) == NBK_P_BAD);   // another unit's record
    memcpy(bad, base, n1); bad[sizeof(nbk_hdr) + sizeof(nbk_unit) + 24] = 1; CHECK(nbk_parse(bad, n1, &hh, v) == NBK_P_BAD);   // OP_END replaced
    memcpy(bad, k2, n2); { const uint64_t a = (uint64_t)(uintptr_t)(base + 8); memcpy(bad + sizeof(nbk_hdr) + sizeof(nbk_unit), &a, 8); } CHECK(nbk_parse(bad, n2, &hh, v) == NBK_P_REF);   // REF to no DEF
    memcpy(bad, k2, n2); { const uint32_t seq = 8; memcpy(bad, &seq, 4); memcpy(bad + sizeof(nbk_hdr) + 8, &seq, 4); } CHECK(nbk_parse(bad, n2, &hh, v) == NBK_P_REF);   // REF to another part's DEF
    {   // the same unit twice
        uint8_t two[4096]; memcpy(two, base, n1); memcpy(two + n1, base + sizeof(nbk_hdr), n1 - sizeof(nbk_hdr)); nbk_hdr t; memcpy(&t, two, sizeof t); t.nunit = 2; memcpy(two, &t, sizeof t);
        CHECK(nbk_parse(two, 2 * n1 - sizeof(nbk_hdr), &hh, v) == NBK_P_BAD); }
    { nbk_hdr t; memset(&t, 0, sizeof t); t.ver = NBK_VER; t.flags = NBK_F_LOST; t.nunit = 1; CHECK(nbk_parse((const uint8_t *)&t, sizeof t, &hh, v) == NBK_P_BAD); }
    free(m);
}
// A NULL BindProgram must not replace a non-NULL one of the same stages (missing_program comes from the last non-NULL one): P1 (stage 2),
// P0 (all stages), NULL (all stages) -> the key keeps P0 after P1, so the replay ends with P0's missing flag (nbs_kind NULL key, NB-14).
static void test_prog(void) {
    memset(&B, 0, sizeof B);
    wr w; w_init(&w); const uint8_t *start = w.cur; uint8_t *b = w_alloc(&w, 8); memcpy(b, &(uint32_t){OP_BEGIN}, 4);
    fe_frame(&w);
    const p_bind_program r[3] = {{fake_prog(1), 0x2}, {fake_prog(2), 0x1f}, {NULL, 0x1f}};   // fake_prog(1): missing 1, fake_prog(2): missing 0
    for (int i = 0; i < 3; i++) w_rec(&w, OP_BIND_PROGRAM, &r[i], sizeof r[i]);
    fe_pass(&w, 0, NULL, NULL); w_alloc(&w, 8); w_alloc(&w, 8);
    memset(&g_live, 0, sizeof g_live); be_exec(&g_live, start, 0, 1);
    CHECK(B.checked == 1 && B.bad_units == 0 && g_live.missing == 0);
}
static void test_lost(void) {
    // a full VB list: 70 distinct (first, n) keys -> VB lost, the other units still exact
    memset(&B, 0, sizeof B); g_partial = 0;
    { wr w; w_init(&w); const uint8_t *start = w.cur; uint8_t *b = w_alloc(&w, 8); memcpy(b, &(uint32_t){OP_BEGIN}, 4); nbs_init(&g_sh, 0); nbs_new_epoch(&g_sh, 900001);
      fe_frame(&w);
      for (int f = 0; f < 16; f++) for (int k = 1; f + k <= 16 && k <= 5; k++) { p_bind_vbuf v; memset(&v, 0, sizeof v); v.first = f; v.n = k; v.r[0].address = 0x1000u * (unsigned)f;
          w_rec(&w, OP_BIND_VBUF, &v, (uint32_t)(offsetof(p_bind_vbuf, r) + sizeof v.r[0] * (size_t)k)); }
      uint8_t pl[512]; uint32_t n; const uint32_t op = (uint32_t)mk_bind_state(pl, 1, 77); (void)op; n = sizeof(p_bind_state); w_rec(&w, OP_BIND_STATE, pl, n);
      fe_pass(&w, 0, NULL, NULL); w_alloc(&w, 8); w_alloc(&w, 8);
      memset(&g_live, 0, sizeof g_live); be_exec(&g_live, start, 0, 1); }
    CHECK(B.checked == 1 && B.bad_units == 0 && B.lost_units == 1 && g_partial == 1);
    // the recording ends between FRAME and the pass: the key is void
    memset(&B, 0, sizeof B); g_lost_end = 0;
    { wr w; w_init(&w); const uint8_t *start = w.cur; uint8_t *b = w_alloc(&w, 8); memcpy(b, &(uint32_t){OP_BEGIN}, 4);
      fe_frame(&w); p_scissor s = {1, 2, 3, 4}; w_rec(&w, OP_SCISSOR, &s, sizeof s); w_alloc(&w, 8); b = w_alloc(&w, 8); memcpy(b, &(uint32_t){OP_BEGIN}, 4);
      fe_pass(&w, 0, NULL, NULL); w_alloc(&w, 8); w_alloc(&w, 8);
      memset(&g_live, 0, sizeof g_live); be_exec(&g_live, start, 0, 1); }   // the model stops at the first OP_END: the key is not reached
    CHECK(g_lost_end == 1);
    // an OP_X_NB_STATE REF whose DEF is before the FRAME: lost without the dictionary, exact with it
    for (int use = 0; use < 2; use++) {
        memset(&B, 0, sizeof B); g_partial = 0; g_use_res = use;
        wr w; w_init(&w); const uint8_t *start = w.cur; uint8_t *b = w_alloc(&w, 8); memcpy(b, &(uint32_t){OP_BEGIN}, 4);
        nbs_init(&g_sh, 0); nbs_new_epoch(&g_sh, 910000u + (uint32_t)use);
        uint8_t rec[64]; memset(rec, 0, sizeof rec); const uint32_t hh[2] = {OP_SCISSOR, 24}; memcpy(rec, hh, 8); rec[8] = 5;
        nbs_capture(&g_sh, rec, 24); sh_flush(&w);              // DEF id 0 (scissor 5)
        rec[8] = 6; nbs_capture(&g_sh, rec, 24); sh_flush(&w);  // DEF id 1 (scissor 6)
        fe_frame(&w);
        rec[8] = 5; nbs_capture(&g_sh, rec, 24); sh_flush(&w);  // REF id 0: its DEF is before the walk start
        fe_pass(&w, 0, NULL, NULL); w_alloc(&w, 8); w_alloc(&w, 8);
        memset(&g_live, 0, sizeof g_live); be_exec(&g_live, start, 0, 1);
        CHECK(B.checked == 1 && B.bad_units == 0);
        CHECK(use ? (g_partial == 0 && B.lost_units == 0) : (g_partial == 1 && B.lost_units == 1));
    }
    g_use_res = 1;
}

int main(void) {
    test_list();
    test_parse();
    make_handles();
    for (int i = 0; i < 4; i++) { g_pools[i].pool = (const pool_t *)(uintptr_t)(0x9000u + 0x10u * (unsigned)i); g_pools[i].off = i * 64; g_pools[i].n = 1024 + i; }
    memset(&B, 0, sizeof B);
    opts o = {0, 0};
    for (int run = 0; run < 40; run++) { o.vb_wide = run % 5 == 4; one_stream(1500, o); if (g_seg > NSEG - 600) { g_seg = 0; make_handles(); } }
    printf("round trip: %llu keys written (%llu bytes, %.1f units a key: %llu DEF %llu REF), %llu with lost units; backend: %llu checked, %llu void, "
           "%llu units compared, %llu differ, %llu lost units skipped, parse errors %llu, no base %llu\n",
           (unsigned long long)g_keys_written, (unsigned long long)g_bytes_w, g_keys_written ? (double)g_units_written / (double)g_keys_written : 0.,
           (unsigned long long)g_def_w, (unsigned long long)g_ref_w, (unsigned long long)g_partial, (unsigned long long)B.checked, (unsigned long long)B.void_keys,
           (unsigned long long)B.units, (unsigned long long)B.bad_units, (unsigned long long)B.lost_units, (unsigned long long)B.parse_bad, (unsigned long long)B.nobase);
    CHECK(B.checked > 1000 && B.bad_units == 0 && B.parse_bad == 0 && B.void_keys == 0 && g_ref_w > 0 && g_def_w > 0);
    // recordings ending inside parts (no submit between): the keys after it are void, the others exact
    memset(&B, 0, sizeof B); g_seg = 0; make_handles();
    o.end_inside = 1; for (int run = 0; run < 6; run++) one_stream(1500, o); o.end_inside = 0;
    printf("recording ends inside parts: backend reached %llu keys (%llu checked, %llu void), %llu units differ\n", (unsigned long long)B.keys,
           (unsigned long long)B.checked, (unsigned long long)B.void_keys, (unsigned long long)B.bad_units);
    CHECK(B.bad_units == 0 && B.parse_bad == 0 && B.void_keys > 0 && B.checked > 100);
    // mutation: a mirror that forgets sampler binds must be caught
    memset(&B, 0, sizeof B); g_seg = 0; make_handles(); g_mut = 1;
    for (int run = 0; run < 4; run++) one_stream(1500, o);
    g_mut = 0;
    printf("mutation (sampler binds forgotten): %llu units differ (must be > 0)\n", (unsigned long long)B.bad_units);
    CHECK(B.bad_units > 0);
    g_seg = 0; make_handles();
    test_prog();
    test_lost();
    if (fails) { printf("FAIL: %d checks\n", fails); return 1; }
    printf("PASS: NB-14 key frame (list, parse / malformed, round trip, REF, NULL program key, lost cases, mutation)\n");
    return 0;
}
