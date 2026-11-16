// [DOS2] NB-13a host test: OP_X_NB_STATE shadow, dedup and unpack (dm/gfx/nvn/nvn_nb_state.h).
// - dictionary: same content -> same id, a change -> a new id, reset per epoch, full -> no id;
// - shadow: last-write order per key, unchanged kinds skipped, repeated content = REF, unknown marking (risk 3) forces a new emission
//   with only the entries set since;
// - round trip: random state call sequences (taken / plain calls, draws, epoch changes, a full dictionary) through the front end's rules
//   and a backend model that executes the blocks' records (DEF / REF / NOID), against the same calls applied one by one: the state at
//   every draw must be identical;
// - malformed records rejected (version, epoch, kinds, ids, content of another kind, missing OP_END, truncation).
// Build + run (from this directory):
//   clang -std=c11 -Wall -Wextra -DDM_GAME_DOS2 -D_CRT_SECURE_NO_WARNINGS -I../../../nvn/include nb_state_test.c -o nb_state_test && ./nb_state_test
#include "../../../nvn/nvn_obj.h"
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)
static uint64_t rnd_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 7; rnd_state ^= rnd_state << 17; return rnd_state; }

// ---- a record as hle_nvn.c rec() writes it ----
typedef struct { uint8_t b[NBS_REC_MAX]; uint32_t size; } recbuf;
static recbuf mk(uint32_t op, const void *pl, size_t n) {
    recbuf r; memset(&r, 0, sizeof r); r.size = (uint32_t)((8 + n + 7) & ~(size_t)7);
    const uint32_t h[2] = {op, r.size}; memcpy(r.b, h, 8); memcpy(r.b + 8, pl, n); return r;
}
static recbuf r_blend(int target, int src) { p_bind_state s; memset(&s, 0, sizeof s); blend_st b; memset(&b, 0, sizeof b); b.target = (uint64_t)target; b.src_rgb = (uint64_t)src; memcpy(s.bytes, &b, sizeof b); return mk(OP_BIND_STATE, &s, sizeof s); }
static recbuf r_state(int kind, uint32_t v) { p_bind_state s; memset(&s, 0, sizeof s); s.kind = kind; memcpy(s.bytes, &v, 4); return mk(OP_BIND_STATE, &s, sizeof s); }
static recbuf r_stencil(int face, int which, int v) { p_stencil s = {face, which == 0 ? v : 0, which == 1 ? v : 0, which == 2 ? v : 0, which}; return mk(OP_STENCIL, &s, sizeof s); }
static recbuf r_prog(uintptr_t p, int stages) { p_bind_program b; memset(&b, 0, sizeof b); b.prog = (const prog_t *)p; b.stages = stages; return mk(OP_BIND_PROGRAM, &b, sizeof b); }
static recbuf r_vbuf(int first, int n, uint64_t a) { p_bind_vbuf b; memset(&b, 0, sizeof b); b.first = first; b.n = n; for (int i = 0; i < n; i++) { b.r[i].address = a + (uint64_t)i * 0x100; b.r[i].size = 64; } return mk(OP_BIND_VBUF, &b, offsetof(p_bind_vbuf, r) + sizeof b.r[0] * (size_t)n); }
static recbuf r_vattr(int n, uint32_t seed) { p_bind_vattr b; memset(&b, 0, sizeof b); b.n = n; for (int i = 0; i < n; i++) b.st[i] = seed + (uint32_t)i; return mk(OP_BIND_VATTR, &b, offsetof(p_bind_vattr, st) + 4 * (size_t)n); }
static recbuf r_vstream(int n, uint32_t seed) { p_bind_vstream b; memset(&b, 0, sizeof b); b.n = n; for (int i = 0; i < n; i++) { b.st[i].stride = seed + (uint32_t)i; b.st[i].divisor = 0; } return mk(OP_BIND_VSTREAM, &b, offsetof(p_bind_vstream, st) + sizeof(vs_st) * (size_t)n); }
static recbuf r_vp(float x) { p_viewports v; memset(&v, 0, sizeof v); v.first = 0; v.n = 1; v.v[0][0] = x; v.v[0][2] = 100; v.v[0][3] = 50; return mk(OP_VIEWPORTS, &v, offsetof(p_viewports, v) + sizeof v.v[0]); }
static recbuf r_scissor(int x) { p_scissor s = {x, 0, 64, 32}; return mk(OP_SCISSOR, &s, sizeof s); }
static recbuf r_drange(float n) { p_depth_range d = {n, 1.f}; return mk(OP_DEPTH_RANGE, &d, sizeof d); }
static recbuf r_poff(float f) { p_poly_offset o = {f, 2.f, 0.f}; return mk(OP_POLY_OFFSET, &o, sizeof o); }
static recbuf r_bcolor(float r) { p_blend_color c = {{r, 0, 0, 1}}; return mk(OP_BLEND_COLOR, &c, sizeof c); }
static recbuf r_aref(float r) { p_alpha_ref a = {r, 0}; return mk(OP_X_ALPHA_REF, &a, sizeof a); }

// ---- backend model: the fields execute() sets (xbg_game.cpp), same semantics ----
typedef struct {
    uint64_t blend[8]; uint32_t color, mask, ms, poly, ds; int32_t sten[3][2]; float bias[3], bcol[4], aref;
    uint32_t nattr, attr[16]; vs_st stream[16]; NVNbufferRange vb[16]; uintptr_t shader[6];
    float vp[4], dmin, dmax; int32_t sc[4];
} model;
static void apply(model *m, const uint8_t *rec) {
    uint32_t op, size; memcpy(&op, rec, 4); memcpy(&size, rec + 4, 4); const uint8_t *p = rec + 8;
    switch (op) {
    case OP_BIND_STATE: { p_bind_state a; memcpy(&a, p, sizeof a);
        if (a.kind == 0) { blend_st b; memcpy(&b, a.bytes, sizeof b); memcpy(&m->blend[b.target], a.bytes, sizeof b); }
        else { uint32_t v; memcpy(&v, a.bytes, 4); if (a.kind == 1) m->color = v; else if (a.kind == 2) m->mask = v; else if (a.kind == 3) m->ms = v; else if (a.kind == 4) m->poly = v; else m->ds = v; }
        break; }
    case OP_STENCIL: { p_stencil a; memcpy(&a, p, sizeof a); for (int i = 0; i < 2; i++) if (a.face & (1 << i)) m->sten[a.which][i] = a.which == 0 ? a.value_mask : a.which == 1 ? a.mask : a.ref; break; }
    case OP_POLY_OFFSET: { p_poly_offset a; memcpy(&a, p, sizeof a); m->bias[0] = a.factor; m->bias[1] = a.units; m->bias[2] = a.clamp; break; }
    case OP_BLEND_COLOR: memcpy(m->bcol, p, 16); break;
    case OP_X_ALPHA_REF: memcpy(&m->aref, p, 4); break;
    case OP_BIND_VATTR: { p_bind_vattr a; memset(&a, 0, sizeof a); memcpy(&a, p, size - 8); m->nattr = (uint32_t)a.n; memset(m->attr, 0, sizeof m->attr); memcpy(m->attr, a.st, 4 * (size_t)a.n); break; }   // replace
    case OP_BIND_VSTREAM: { p_bind_vstream a; memset(&a, 0, sizeof a); memcpy(&a, p, size - 8); memcpy(m->stream + a.first, a.st, sizeof(vs_st) * (size_t)a.n); break; }
    case OP_BIND_VBUF: { p_bind_vbuf a; memset(&a, 0, sizeof a); memcpy(&a, p, size - 8); memcpy(m->vb + a.first, a.r, sizeof(NVNbufferRange) * (size_t)a.n); break; }
    case OP_BIND_PROGRAM: { p_bind_program a; memcpy(&a, p, sizeof a); for (int s = 0; s < 6; s++) if (a.stages & (1 << s)) m->shader[s] = a.prog ? (uintptr_t)a.prog ^ (uintptr_t)s : 0; break; }
    case OP_VIEWPORTS: { p_viewports a; memset(&a, 0, sizeof a); memcpy(&a, p, size - 8); memcpy(m->vp, a.v[0], 16); break; }
    case OP_SCISSOR: memcpy(m->sc, p, 16); break;
    case OP_DEPTH_RANGE: memcpy(&m->dmin, p, 4); memcpy(&m->dmax, p + 4, 4); break;
    default: break;
    }
}
static void apply_content(model *m, const uint8_t *c) {
    for (uint32_t off = 0;;) { uint32_t op, size; memcpy(&op, c + off, 4); memcpy(&size, c + off + 4, 4); if (!op) return; apply(m, c + off); off += size; }
}
// the backend's DEF table (xbg_nb_state.inc nbstate::defs): content is copied here because the test reuses its record buffer
typedef struct { uint32_t epoch, bytes; uint8_t c[NBS_SUB_BYTES]; } bdef;
static bdef g_defs[NBS_KINDS][NBS_MAX_IDS];
static int g_miss;
static void be_exec(model *m, const uint8_t *p, uint32_t n, uint64_t cnt[3]) {
    nbs_hdr h; nbs_view v[NBS_KINDS];
    const int nb = nbs_parse(p, n, &h, v);
    CHECK(nb > 0);
    for (int i = 0; i < nb; i++) {
        const nbs_view *b = &v[i]; const uint8_t *c = b->content;
        if (b->b.how == NBS_B_REF) { bdef *d = &g_defs[b->b.kind][b->b.id]; if (d->epoch != h.epoch) { g_miss++; continue; } c = d->c; }
        else if (b->b.how == NBS_B_DEF) { bdef *d = &g_defs[b->b.kind][b->b.id]; d->epoch = h.epoch; d->bytes = b->b.bytes; memcpy(d->c, c, b->b.bytes); }
        cnt[b->b.how]++;
        apply_content(m, c);
    }
}

static nbs_state S;
static uint8_t out[NBS_PAYLOAD_MAX];

static void test_dict(void) {
    static nbs_dict d; nbs_dict_reset(&d);
    const uint8_t a[16] = {1, 2, 3}, b[16] = {1, 2, 4};
    int f1, f2, f3, f4;
    const int ia = nbs_dict_get(&d, a, 16, &f1), ia2 = nbs_dict_get(&d, a, 16, &f2), ib = nbs_dict_get(&d, b, 16, &f3), ia3 = nbs_dict_get(&d, a, 8, &f4);
    CHECK(ia == 0 && f1 == 1); CHECK(ia2 == ia && f2 == 0); CHECK(ib == 1 && f3 == 1); CHECK(ia3 == 2 && f4 == 1);   // a prefix is another content
    nbs_dict_reset(&d); CHECK(nbs_dict_get(&d, b, 16, &f1) == 0 && f1 == 1);
    nbs_dict_reset(&d);
    int full = 0;
    for (uint32_t i = 0; i < NBS_MAX_IDS + 10; i++) { uint8_t c[24]; memset(c, 0, sizeof c); memcpy(c, &i, 4); int f; const int id = nbs_dict_get(&d, c, 24, &f);
        if (i < NBS_MAX_IDS) CHECK(id == (int)i && f == 1); else full += id < 0; }
    CHECK(full == 10);
    { int f; uint32_t i = 5; uint8_t c[24]; memset(c, 0, sizeof c); memcpy(c, &i, 4); CHECK(nbs_dict_get(&d, c, 24, &f) == 5 && f == 0); }   // still found when full
}

static int nblocks(const uint8_t *p, uint32_t n, int how, int kind) {
    nbs_hdr h; nbs_view v[NBS_KINDS]; const int nb = nbs_parse(p, n, &h, v); int k = 0;
    for (int i = 0; i < nb; i++) if ((how < 0 || v[i].b.how == how) && (kind < 0 || v[i].b.kind == kind)) k++;
    return nb < 0 ? -1 : k;
}
static void cap(recbuf r) { CHECK(nbs_capture(&S, r.b, r.size) >= 0); }
static void test_shadow(void) {
    nbs_init(&S, 1);
    cap(r_blend(0, 5)); cap(r_blend(1, 6)); cap(r_prog(0x1000, 0x1f));
    uint32_t n = nbs_pack(&S, out, sizeof out);
    CHECK(n > 0 && nblocks(out, n, NBS_B_DEF, NBS_K_BLEND) == 1 && nblocks(out, n, NBS_B_DEF, NBS_K_PROG) == 1 && nblocks(out, n, -1, -1) == 2);
    CHECK(S.s[NBS_K_BLEND].n == 2);
    // the same values again: nothing to emit (the backend has them)
    cap(r_blend(1, 6)); cap(r_prog(0x1000, 0x1f));
    CHECK(S.s[NBS_K_BLEND].n == 2 && S.s[NBS_K_BLEND].e[1].key == ((uint32_t)OP_BIND_STATE << 16 | 1));
    n = nbs_pack(&S, out, sizeof out); CHECK(n == 0); CHECK(S.st[NBS_S_SAME] == 2);
    // a change -> a new id; back -> REF of the first id
    const int32_t p0 = S.emitted[NBS_K_PROG];
    cap(r_prog(0x2000, 0x1f)); n = nbs_pack(&S, out, sizeof out); CHECK(nblocks(out, n, NBS_B_DEF, NBS_K_PROG) == 1); CHECK(S.emitted[NBS_K_PROG] == p0 + 1);
    cap(r_prog(0x1000, 0x1f)); n = nbs_pack(&S, out, sizeof out); CHECK(nblocks(out, n, NBS_B_REF, NBS_K_PROG) == 1); CHECK(S.emitted[NBS_K_PROG] == p0);
    // blend target 0 rewritten: moves to the end (last-write order) -> a different content than [t0, t1]
    cap(r_blend(0, 5)); CHECK(S.s[NBS_K_BLEND].e[1].key == ((uint32_t)OP_BIND_STATE << 16 | 0));
    n = nbs_pack(&S, out, sizeof out); CHECK(nblocks(out, n, NBS_B_DEF, NBS_K_BLEND) == 1);
    // unknown (a plain record of the kind): entries forgotten, the next equal content is emitted again (REF: same epoch dictionary)
    nbs_unknown(&S, 1u << NBS_K_BLEND); CHECK(S.s[NBS_K_BLEND].n == 0 && S.emitted[NBS_K_BLEND] == -1 && S.st[NBS_S_UNKNOWN] == 1);
    cap(r_blend(1, 6));
    n = nbs_pack(&S, out, sizeof out); CHECK(nblocks(out, n, -1, NBS_K_BLEND) == 1 && S.s[NBS_K_BLEND].n == 1);   // only target 1: target 0 is the plain call's
    nbs_unknown(&S, 1u << NBS_K_BLEND); cap(r_blend(1, 6)); n = nbs_pack(&S, out, sizeof out); CHECK(nblocks(out, n, NBS_B_REF, NBS_K_BLEND) == 1);
    // a new epoch: dictionaries empty, a content is a DEF again, nothing emitted without a change
    nbs_new_epoch(&S, 2); CHECK(nbs_pack(&S, out, sizeof out) == 0);
    cap(r_blend(1, 6)); n = nbs_pack(&S, out, sizeof out); CHECK(nblocks(out, n, NBS_B_DEF, NBS_K_BLEND) == 1);
    { nbs_hdr h; memcpy(&h, out, sizeof h); CHECK(h.epoch == 2 && h.ver == NBS_VER); }
    // keys: stencil per face / which, vbuf per first / n, vattr one key
    nbs_init(&S, 3);
    cap(r_stencil(1, 2, 7)); cap(r_stencil(3, 2, 8)); cap(r_stencil(1, 2, 9)); CHECK(S.s[NBS_K_DS].n == 2);
    cap(r_vattr(3, 1)); cap(r_vattr(5, 1)); CHECK(S.s[NBS_K_VFMT].n == 1);
    cap(r_vbuf(0, 2, 0x10000)); cap(r_vbuf(1, 1, 0x20000)); cap(r_vbuf(0, 2, 0x30000)); CHECK(S.s[NBS_K_VB].n == 2);
    // not modelled: a bind-state kind the backend does not know, a non-state op
    { p_bind_state s; memset(&s, 0, sizeof s); s.kind = 9; recbuf r = mk(OP_BIND_STATE, &s, sizeof s); CHECK(nbs_capture(&S, r.b, r.size) < 0); }
    { p_draw d = {0, 0, 3}; recbuf r = mk(OP_DRAW, &d, sizeof d); CHECK(nbs_capture(&S, r.b, r.size) < 0); }
    CHECK(nbs_op_kinds(OP_DRAW) == 0 && nbs_op_kinds(OP_BIND_STATE) == 0x0f && nbs_op_kinds(OP_BIND_PROGRAM) == 1u << NBS_K_PROG);
    // the shadow of a kind is full: refused, the shadow stays as it was
    nbs_init(&S, 4);
    int refused = 0; for (int i = 0; i < 16; i++) for (int k = 1; k <= 16 - i; k++) { recbuf r = r_vbuf(i, k, 0x1000); if (nbs_capture(&S, r.b, r.size) < 0) refused++; }
    CHECK(refused > 0 && S.s[NBS_K_VB].used + 8 <= NBS_SUB_BYTES && S.s[NBS_K_VB].n <= NBS_SUB_ENTS);
}

static void test_malformed(void) {
    nbs_init(&S, 7); cap(r_blend(0, 1)); cap(r_scissor(3));
    uint32_t n = nbs_pack(&S, out, sizeof out); CHECK(n > 0);
    nbs_hdr h; nbs_view v[NBS_KINDS];
    CHECK(nbs_parse(out, n, &h, v) == 2); CHECK(nbs_parse(out, n + 8, &h, v) == 2);   // the record's alignment tail
    static uint8_t t[NBS_PAYLOAD_MAX];
    memcpy(t, out, n); t[0] = 9; CHECK(nbs_parse(t, n, &h, v) < 0);                       // version
    memcpy(t, out, n); memset(t + 4, 0, 4); CHECK(nbs_parse(t, n, &h, v) < 0);            // epoch 0
    memcpy(t, out, n); t[1] = 0; CHECK(nbs_parse(t, n, &h, v) < 0);                       // no blocks
    memcpy(t, out, n); t[1] = 3; CHECK(nbs_parse(t, n, &h, v) < 0);                       // more blocks than bytes
    for (uint32_t cut = 0; cut < n; cut += 4) CHECK(nbs_parse(out, cut, &h, v) < 0);     // truncation
    memcpy(t, out, n); t[8] = NBS_KINDS; CHECK(nbs_parse(t, n, &h, v) < 0);               // kind
    memcpy(t, out, n); t[9] = 3; CHECK(nbs_parse(t, n, &h, v) < 0);                       // how
    memcpy(t, out, n); t[8] = NBS_K_VPSC; CHECK(nbs_parse(t, n, &h, v) < 0);              // blend records in a viewport block
    { memcpy(t, out, n); nbs_blk b; memcpy(&b, t + 8, 8); memset(t + 16 + b.bytes - 8, 0xee, 4); CHECK(nbs_parse(t, n, &h, v) < 0); }   // no OP_END
    { memcpy(t, out, n); nbs_blk b; memcpy(&b, t + 8, 8); b.how = NBS_B_REF; memcpy(t + 8, &b, 8); CHECK(nbs_parse(t, n, &h, v) < 0); }   // REF with content bytes
    { memcpy(t, out, n); nbs_blk b; memcpy(&b, t + 8, 8); b.id = NBS_MAX_IDS; memcpy(t + 8, &b, 8); CHECK(nbs_parse(t, n, &h, v) < 0); }   // id
}

// ---- round trip ----
static recbuf random_call(int *kind_out) {
    const int k = (int)(rnd() % 13); const int v = (int)(rnd() % 3);   // few values: repeats and REF happen
    *kind_out = k;
    switch (k) {
    case 0: return r_blend((int)(rnd() % 3), v);
    case 1: return r_state(1 + (int)(rnd() % 3), (uint32_t)v);
    case 2: return r_state(4, (uint32_t)v);
    case 3: return r_state(5, (uint32_t)v);
    case 4: return r_stencil(1 + (int)(rnd() % 3), (int)(rnd() % 3), v);
    case 5: return rnd() & 1 ? r_vattr(1 + (int)(rnd() % 4), (uint32_t)v) : r_vstream(1 + (int)(rnd() % 4), (uint32_t)v);
    case 6: return r_vbuf((int)(rnd() % 3), 1 + (int)(rnd() % 3), 0x10000u * (uint64_t)v);
    case 7: return r_prog(0x1000u * (uintptr_t)(1 + v), rnd() & 1 ? 0x1f : 0x11);
    case 8: return r_vp((float)v);
    case 9: return r_scissor(v);
    case 10: return r_drange((float)v);
    case 11: return rnd() & 1 ? r_poff((float)v) : r_bcolor((float)v);
    default: return r_aref((float)v);
    }
}
static void test_roundtrip(void) {
    memset(g_defs, 0, sizeof g_defs); g_miss = 0;
    uint64_t cnt[3] = {0, 0, 0}, draws = 0, bad = 0, plain = 0, bytes_old = 0, bytes_new = 0, fallback = 0;
    for (int run = 0; run < 40; run++) {
        model ref, be; memset(&ref, 0, sizeof ref); memset(&be, 0, sizeof be);
        uint32_t epoch = 100 + (uint32_t)run * 1000; nbs_init(&S, epoch);
        const int plain_pct = run % 4 == 0 ? 0 : run % 4 == 1 ? 5 : run % 4 == 2 ? 30 : 60;
        for (int step = 0; step < 4000; step++) {
            const uint64_t r = rnd() % 100;
            if (r < 12) {   // a draw (any other record): the state goes out first, then the backend state must equal the reference
                const uint32_t n = nbs_pack(&S, out, sizeof out);
                if (n) { be_exec(&be, out, n, cnt); bytes_new += (n + 8 + 7) & ~7u; }
                draws++; if (memcmp(&ref, &be, sizeof ref)) bad++;
            } else if (r < 13) {   // EndRecording / another command buffer: a new epoch
                const uint32_t n = nbs_pack(&S, out, sizeof out); if (n) be_exec(&be, out, n, cnt);
                nbs_new_epoch(&S, ++epoch);
            } else {
                int kind; recbuf c = random_call(&kind);
                apply(&ref, c.b);
                if ((int)(rnd() % 100) < plain_pct) {   // a plain record: what is pending first, then the record itself, then its kind unknown
                    const uint32_t n = nbs_pack(&S, out, sizeof out); if (n) be_exec(&be, out, n, cnt);
                    apply(&be, c.b); plain++;
                    uint32_t key; const int k = nbs_kind(*(const uint32_t *)c.b, c.b + 8, c.size - 8, &key);
                    nbs_unknown(&S, 1u << k);
                } else {
                    bytes_old += c.size;
                    if (nbs_capture(&S, c.b, c.size) < 0) {   // does not fit: the same fallback as nvn_nb_state.inc
                        const uint32_t n = nbs_pack(&S, out, sizeof out); if (n) be_exec(&be, out, n, cnt);
                        apply(&be, c.b); fallback++;
                        uint32_t key; nbs_unknown(&S, 1u << nbs_kind(*(const uint32_t *)c.b, c.b + 8, c.size - 8, &key));
                    }
                }
            }
        }
    }
    printf("round trip: %llu draws, %llu differ; blocks DEF %llu REF %llu NOID %llu; plain %llu, fallback %llu, REF misses %d; "
           "captured %llu bytes as NVN records -> %llu bytes OP_X_NB_STATE\n", (unsigned long long)draws, (unsigned long long)bad,
           (unsigned long long)cnt[1], (unsigned long long)cnt[0], (unsigned long long)cnt[2], (unsigned long long)plain,
           (unsigned long long)fallback, g_miss, (unsigned long long)bytes_old, (unsigned long long)bytes_new);
    CHECK(bad == 0); CHECK(g_miss == 0); CHECK(cnt[0] > 0 && cnt[1] > 0);
}
// a full dictionary: distinct vertex buffers every draw -> NOID blocks, still exact
static void test_noid(void) {
    memset(g_defs, 0, sizeof g_defs);
    model ref, be; memset(&ref, 0, sizeof ref); memset(&be, 0, sizeof be);
    nbs_init(&S, 9); uint64_t cnt[3] = {0, 0, 0}; int bad = 0;
    for (uint32_t i = 0; i < NBS_MAX_IDS + 300; i++) {
        recbuf c = r_vbuf(0, 1, 0x100000ull + i * 0x40ull); apply(&ref, c.b); cap(c);
        const uint32_t n = nbs_pack(&S, out, sizeof out); CHECK(n > 0); be_exec(&be, out, n, cnt);
        if (memcmp(&ref, &be, sizeof ref)) bad++;
    }
    CHECK(bad == 0); CHECK(cnt[1] + cnt[2] == NBS_MAX_IDS + 300 && cnt[2] >= 300 && cnt[0] == 0);   // ids or arena run out first (40-byte contents: the arena)
    printf("full dictionary: %llu DEF, %llu NOID\n", (unsigned long long)cnt[1], (unsigned long long)cnt[2]);
    // the first content again: still in the dictionary -> REF
    recbuf c = r_vbuf(0, 1, 0x100000ull); apply(&ref, c.b); cap(c);
    const uint32_t n = nbs_pack(&S, out, sizeof out); be_exec(&be, out, n, cnt); CHECK(cnt[0] == 1 && !memcmp(&ref, &be, sizeof ref));
}

int main(void) {
    test_dict();
    test_shadow();
    test_malformed();
    test_roundtrip();
    test_noid();
    if (fails) { printf("nb_state_test: %d FAILED\n", fails); return 1; }
    printf("nb_state_test: PASS\n");
    return 0;
}
