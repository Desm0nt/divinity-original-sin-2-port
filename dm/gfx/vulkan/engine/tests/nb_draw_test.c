// [DOS2] NB-09 host test: OP_X_NB_DRAW pack / unpack (dm/gfx/nvn/nvn_nb_draw.h). Round trip of every header field and bind kind, the
// maximum bind count, call order kept, limits (stage / slot per kind), malformed payloads rejected, the 8-byte tail of a record accepted,
// and the faithful unpack: the binds come back as exactly the bind records the NVN path would have written, in call order.
// NB-10: the check flag (NBD_F_CHECK) round trip; apply (nbd_view + nbd_apply) against the NB-09 unpack + the backend's per-bind record
// functions on a model of the backend state (tables, generation counters); nbd_compare / nbd_same_draw (DM_NB_OPS_CHECK); a rough
// micro benchmark of apply vs unpack per record (host CPU, -O2; printed, not checked).
// Build + run (from this directory):  clang -std=c11 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS nb_draw_test.c -o nb_draw_test && ./nb_draw_test
#include "../../../nvn/nvn_nb_draw.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int fails;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

static uint64_t rnd_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rnd_state ^= rnd_state << 13; rnd_state ^= rnd_state >> 7; rnd_state ^= rnd_state << 17; return rnd_state; }

static int same_shadow(const nbd_shadow *a, const nbd_shadow *b) {
    nbd_hdr x = a->h, y = b->h; x.ver = y.ver = 0;
    if (memcmp(&x, &y, sizeof x)) return 0;
    return !memcmp(a->bind, b->bind, sizeof(nbd_bind) * a->h.nbind);
}
static void fill_hdr(nbd_shadow *s, int draw) {
    memset(s, 0, sizeof *s);
    s->h.engine_op = 37; s->h.draw = (uint8_t)draw; s->h.flags = NBD_F_REN | NBD_F_REN_ON | NBD_F_COND; s->h.cond_mode = 2;
    s->h.texgen = 0xdeadbeefu; s->h.prim = 4; s->h.itype = 2; s->h.count = 123456; s->h.base_vertex = -7; s->h.base_instance = 3;
    s->h.instances = 99; s->h.first = 11; s->h.ibuf = 0x123456789abcdef0ull; s->h.cond_addr = 0xfedcba9876543210ull;
}
static void test_roundtrip_fields(void) {
    for (int d = NBD_DRAW_NONE; d <= NBD_DRAW_ARRAYS; d++) {
        nbd_shadow s, o; fill_hdr(&s, d);
        CHECK(nbd_add(&s, NBD_B_TEX, 1, 127, 0x00100003ull | (5ull << 20), 0));
        CHECK(nbd_add(&s, NBD_B_SAMP, 0, 5, 0x42, 0));
        CHECK(nbd_add(&s, NBD_B_IMG, 5, 15, 0x77, 0));
        CHECK(nbd_add(&s, NBD_B_UBO, 4, 15, 0x7f0000001000ull, 0x10000));
        uint8_t buf[sizeof(nbd_hdr) + NBD_MAX_BINDS * sizeof(nbd_bind) + 8];
        const uint32_t n = nbd_pack(&s, buf, sizeof buf);
        CHECK(n == sizeof(nbd_hdr) + 4 * sizeof(nbd_bind) && n == nbd_size(&s) && n % 8 == 0);
        CHECK(nbd_unpack(buf, n, &o) == 0);
        CHECK(o.h.ver == NBD_VER);
        CHECK(same_shadow(&s, &o));
        CHECK(o.h.draw == d && o.h.engine_op == 37 && o.h.texgen == 0xdeadbeefu && o.h.base_vertex == -7 && o.h.ibuf == 0x123456789abcdef0ull);
        CHECK(o.bind[0].kind == NBD_B_TEX && o.bind[0].stage == 1 && o.bind[0].slot == 127 && o.bind[0].a == (0x00100003ull | (5ull << 20)));
        CHECK(o.bind[3].kind == NBD_B_UBO && o.bind[3].a == 0x7f0000001000ull && o.bind[3].b == 0x10000);
        CHECK(nbd_unpack(buf, n + 8, &o) == 0);   // the record's 8-aligned tail
    }
}
static void test_max_binds_random(void) {
    for (int round = 0; round < 2000; round++) {
        nbd_shadow s, o; fill_hdr(&s, (int)(rnd() % 3));
        const int want = round == 0 ? NBD_MAX_BINDS : (int)(rnd() % (NBD_MAX_BINDS + 1));
        for (int i = 0; i < want; i++) {
            const unsigned k = (unsigned)(rnd() % NBD_B_KINDS); const int lim = (k == NBD_B_UBO || k == NBD_B_IMG) ? 16 : 128;
            CHECK(nbd_add(&s, k, (int)(rnd() % 6), (int)(rnd() % (uint64_t)lim), rnd(), k == NBD_B_UBO ? rnd() : 0));
        }
        CHECK(s.h.nbind == want);
        if (want == NBD_MAX_BINDS) CHECK(!nbd_add(&s, NBD_B_TEX, 0, 0, 1, 0));   // full: the caller flushes
        uint8_t buf[sizeof(nbd_hdr) + NBD_MAX_BINDS * sizeof(nbd_bind)];
        const uint32_t n = nbd_pack(&s, buf, sizeof buf);
        CHECK(n == nbd_size(&s));
        CHECK(nbd_unpack(buf, n, &o) == 0 && same_shadow(&s, &o));
        if (n > sizeof(nbd_hdr)) CHECK(nbd_unpack(buf, n - 8, &o) != 0);   // truncated
        CHECK(nbd_pack(&s, buf, n - 1) == 0);                             // does not fit
    }
    CHECK(sizeof(nbd_hdr) + NBD_MAX_BINDS * sizeof(nbd_bind) == 1600);    // largest record payload (fits the DM_NATIVE_DRAW 4 KB buffer)
}
static void test_limits(void) {
    nbd_shadow s; memset(&s, 0, sizeof s);
    CHECK(!nbd_add(&s, NBD_B_TEX, 6, 0, 1, 0));    // stage
    CHECK(!nbd_add(&s, NBD_B_TEX, -1, 0, 1, 0));
    CHECK(!nbd_add(&s, NBD_B_TEX, 0, 128, 1, 0));  // texture slot
    CHECK(nbd_add(&s, NBD_B_SAMP, 0, 127, 1, 0));
    CHECK(!nbd_add(&s, NBD_B_IMG, 0, 16, 1, 0));   // image slot
    CHECK(!nbd_add(&s, NBD_B_UBO, 0, 16, 1, 0));   // UBO slot
    CHECK(!nbd_add(&s, NBD_B_UBO, 0, -1, 1, 0));
    CHECK(!nbd_add(&s, NBD_B_KINDS, 0, 0, 1, 0));  // kind
    CHECK(s.h.nbind == 1);
}
static void test_malformed(void) {
    nbd_shadow s, o; fill_hdr(&s, NBD_DRAW_IDX);
    nbd_add(&s, NBD_B_TEX, 0, 0, 1, 0);
    uint8_t buf[256]; const uint32_t n = nbd_pack(&s, buf, sizeof buf);
    CHECK(nbd_unpack(buf, sizeof(nbd_hdr) - 8, &o) != 0);                       // shorter than the header
    uint8_t b2[256]; memcpy(b2, buf, n); b2[0] = 2; CHECK(nbd_unpack(b2, n, &o) != 0);   // version
    memcpy(b2, buf, n); b2[2] = 3; CHECK(nbd_unpack(b2, n, &o) != 0);            // draw kind
    memcpy(b2, buf, n); { uint16_t c = NBD_MAX_BINDS + 1; memcpy(b2 + 4, &c, 2); } CHECK(nbd_unpack(b2, sizeof b2, &o) != 0);   // count
    memcpy(b2, buf, n); b2[sizeof(nbd_hdr) + 1] = 6; CHECK(nbd_unpack(b2, n, &o) != 0);   // bind stage
    memcpy(b2, buf, n); b2[sizeof(nbd_hdr)] = 9; CHECK(nbd_unpack(b2, n, &o) != 0);       // bind kind
    CHECK(nbd_unpack(NULL, 64, &o) != 0);
}
// The faithful unpack: what the old path wrote (one record per bind call, in call order; a BindUniformBuffers(n) = n UBO records) and
// what the backend replays from the record must be the same sequence. Modelled as (kind, stage, slot, a, b) tuples.
typedef struct { int kind, stage, slot; uint64_t a, b; } call;
static void test_faithful_order(void) {
    call calls[NBD_MAX_BINDS]; int nc = 0;
    nbd_shadow s; fill_hdr(&s, NBD_DRAW_IDX);
    // the renderer's flush: uniform blocks (0x925740) then texture slots (0x924730), rebinding a slot twice (not merged)
    for (int i = 0; i < 6; i++) calls[nc++] = (call){NBD_B_UBO, i & 1, 2 + i, 0x7f00000000ull + 0x100u * (unsigned)i, 0x100, };
    for (int i = 0; i < 9; i++) calls[nc++] = (call){i % 3 == 2 ? NBD_B_SAMP : NBD_B_TEX, 1, i % 4, 0x1000u + (unsigned)i, 0};
    for (int i = 0; i < nc; i++) CHECK(nbd_add(&s, (unsigned)calls[i].kind, calls[i].stage, calls[i].slot, calls[i].a, calls[i].b));
    uint8_t buf[2048]; const uint32_t n = nbd_pack(&s, buf, sizeof buf);
    nbd_shadow o; CHECK(nbd_unpack(buf, n, &o) == 0);
    CHECK(o.h.nbind == nc);
    for (int i = 0; i < nc && i < o.h.nbind; i++)
        CHECK(o.bind[i].kind == calls[i].kind && o.bind[i].stage == calls[i].stage && o.bind[i].slot == calls[i].slot && o.bind[i].a == calls[i].a
              && o.bind[i].b == calls[i].b);
    // the backend tables after the old records == after the unpack (last write per slot wins; one generation bump per record)
    uint64_t t_old[6][128] = {{0}}, t_new[6][128] = {{0}}; unsigned g_old = 0, g_new = 0;
    for (int i = 0; i < nc; i++) { t_old[calls[i].stage][calls[i].slot + (calls[i].kind == NBD_B_UBO ? 0 : 16)] = calls[i].a; g_old++; }
    for (int i = 0; i < o.h.nbind; i++) { t_new[o.bind[i].stage][o.bind[i].slot + (o.bind[i].kind == NBD_B_UBO ? 0 : 16)] = o.bind[i].a; g_new++; }
    CHECK(!memcmp(t_old, t_new, sizeof t_old) && g_old == g_new);
}

// ---- NB-10 ----
static void test_check_flag(void) {
    nbd_shadow s, o; fill_hdr(&s, NBD_DRAW_IDX); s.h.flags = NBD_F_REN | NBD_F_CHECK;
    nbd_add(&s, NBD_B_UBO, 1, 3, 0x1000, 0x40);
    uint8_t buf[256]; const uint32_t n = nbd_pack(&s, buf, sizeof buf);
    CHECK(nbd_unpack(buf, n, &o) == 0 && (o.h.flags & NBD_F_CHECK) && same_shadow(&s, &o));
    CHECK(buf[offsetof(nbd_hdr, flags)] & NBD_F_CHECK);   // the byte the backend tests before choosing check / apply
    nbd_hdr h; const uint8_t *b = NULL;
    CHECK(nbd_view(buf, n, &h, &b) == 0 && (h.flags & NBD_F_CHECK) && b == buf + sizeof(nbd_hdr) && h.nbind == 1);
    s.h.flags = NBD_F_REN; nbd_pack(&s, buf, sizeof buf);
    CHECK(!(buf[offsetof(nbd_hdr, flags)] & NBD_F_CHECK));
    // nbd_view rejects what nbd_unpack rejects at the header level
    CHECK(nbd_view(buf, sizeof(nbd_hdr) - 8, &h, &b) != 0);
    CHECK(nbd_view(buf, n - 8, &h, &b) != 0);                        // truncated binds
    uint8_t b2[256]; memcpy(b2, buf, n); b2[0] = 2; CHECK(nbd_view(b2, n, &h, &b) != 0);
    memcpy(b2, buf, n); b2[2] = 3; CHECK(nbd_view(b2, n, &h, &b) != 0);
    CHECK(nbd_view(NULL, 64, &h, &b) != 0);
}
// A model of the backend state: State.tex / samp / img / ub + tex_gen / buf_gen (xbg_game.cpp), and the two record functions of the old
// path (exec_bind_buf / exec_bind_handles, same checks) that the NB-09 unpack calls one bind at a time. The payload structs are copies of
// nvn_obj.h p_bind_buf / p_bind_handles.
typedef struct { int32_t stage, index; uint64_t addr, size; } m_bind_buf;
typedef struct { int32_t stage, first, n; uint64_t h[32]; } m_bind_handles;
typedef struct { uint64_t tex[6][128], samp[6][128], img[6][16]; nbd_range ub[6][16]; uint64_t tex_gen[6], buf_gen[6]; int bad; } model;
static void m_bind_buf_exec(model *m, const m_bind_buf *a) {
    if (!(a->stage >= 0 && a->stage < 6 && a->index >= 0 && a->index < 16)) { m->bad = 1; return; }
    m->ub[a->stage][a->index].addr = a->addr; m->ub[a->stage][a->index].size = a->size; ++m->buf_gen[a->stage];
}
static void m_bind_handles_exec(model *m, unsigned kind, const m_bind_handles *a, size_t n) {
    if (!(a->stage >= 0 && a->stage < 6 && a->first >= 0 && a->n >= 0 && a->n <= 32 && a->first + a->n <= (kind == NBD_B_IMG ? 16 : 128)
          && n >= offsetof(m_bind_handles, h) + (size_t)a->n * 8)) { m->bad = 1; return; }
    uint64_t *dst = kind == NBD_B_IMG ? m->img[a->stage] : kind == NBD_B_SAMP ? m->samp[a->stage] : m->tex[a->stage];
    memcpy(dst + a->first, a->h, (size_t)a->n * 8); ++m->tex_gen[a->stage];
}
static void m_unpack(model *m, const uint8_t *p, size_t n) {   // xbg_nb.inc nbdraw::unpack (NB-09), binds part
    nbd_shadow sh;
    if (nbd_unpack(p, n, &sh)) { m->bad = 1; return; }
    for (unsigned i = 0; i < sh.h.nbind; i++) {
        const nbd_bind *e = &sh.bind[i];
        if (e->kind == NBD_B_UBO) { m_bind_buf a = {e->stage, e->slot, e->a, e->b}; m_bind_buf_exec(m, &a); }
        else { m_bind_handles a; memset(&a, 0, sizeof a); a.stage = e->stage; a.first = e->slot; a.n = 1; a.h[0] = e->a;
               m_bind_handles_exec(m, e->kind, &a, offsetof(m_bind_handles, h) + 8); }
    }
}
static void m_apply(model *m, const uint8_t *p, size_t n) {    // xbg_nb.inc nbdraw::apply (NB-10), binds part
    nbd_hdr h; const uint8_t *b; unsigned ts, bs;
    if (nbd_view(p, n, &h, &b) || nbd_apply(&h, b, m->tex, m->samp, m->img, m->ub, &ts, &bs)) { m->bad = 1; return; }
    for (unsigned st = 0; st < 6; st++) { if (ts >> st & 1) ++m->tex_gen[st]; if (bs >> st & 1) ++m->buf_gen[st]; }
}
static void model_random(model *m) {
    memset(m, 0, sizeof *m);
    for (int st = 0; st < 6; st++) {
        for (int i = 0; i < 128; i++) { m->tex[st][i] = rnd(); m->samp[st][i] = rnd(); }
        for (int i = 0; i < 16; i++) { m->img[st][i] = rnd(); m->ub[st][i].addr = rnd(); m->ub[st][i].size = rnd(); }
        m->tex_gen[st] = rnd() >> 8; m->buf_gen[st] = rnd() >> 8;
    }
}
// A random record like the renderer's flush: few stages, small slot numbers (so slots repeat), sometimes the same slot rebound.
static uint32_t random_record(uint8_t *buf, size_t cap, nbd_shadow *s) {
    fill_hdr(s, 1 + (int)(rnd() % 2)); s->h.flags = 0;
    const int nb = (int)(rnd() % (NBD_MAX_BINDS + 1));
    for (int i = 0; i < nb; i++) {
        const unsigned k = (unsigned)(rnd() % NBD_B_KINDS);
        CHECK(nbd_add(s, k, (int)(rnd() % 6), (int)(rnd() % 6), rnd(), k == NBD_B_UBO ? rnd() % 0x10000 : 0));
    }
    return nbd_pack(s, buf, cap);
}
static void test_apply_vs_unpack(void) {
    static model a, u, t;
    uint8_t buf[sizeof(nbd_hdr) + NBD_MAX_BINDS * sizeof(nbd_bind)];
    for (int round = 0; round < 3000; round++) {
        model_random(&u); a = u; t = u;
        // several records in a row (a frame's draws), gen counters carried over
        const int nrec = 1 + (int)(rnd() % 4);
        for (int r = 0; r < nrec; r++) {
            nbd_shadow s; const uint32_t n = random_record(buf, sizeof buf, &s);
            const model before = u;
            m_unpack(&u, buf, n); m_apply(&a, buf, n);
            CHECK(!u.bad && !a.bad);
            CHECK(!memcmp(u.tex, a.tex, sizeof u.tex) && !memcmp(u.samp, a.samp, sizeof u.samp) && !memcmp(u.img, a.img, sizeof u.img)
                  && !memcmp(u.ub, a.ub, sizeof u.ub));
            // generations: per stage "changed since" is the same (old: +1 per bind, new: +1 per record) and the new one never goes back
            for (int st = 0; st < 6; st++) {
                CHECK((u.tex_gen[st] != before.tex_gen[st]) == (a.tex_gen[st] != t.tex_gen[st]));
                CHECK((u.buf_gen[st] != before.buf_gen[st]) == (a.buf_gen[st] != t.buf_gen[st]));
                CHECK(a.tex_gen[st] - t.tex_gen[st] <= 1 && a.buf_gen[st] - t.buf_gen[st] <= 1);
            }
            t = a;
            // DM_NB_OPS_CHECK: the tables after the old records match the record (nothing differs) ...
            nbd_hdr h; const uint8_t *b; nbd_diff d;
            CHECK(nbd_view(buf, n, &h, &b) == 0);
            CHECK(nbd_compare(&h, b, (const uint64_t (*)[128])u.tex, (const uint64_t (*)[128])u.samp, (const uint64_t (*)[16])u.img,
                              (const nbd_range (*)[16])u.ub, &d) == 0);
            // ... and a changed last-write slot is found, with its kind / stage / slot
            if (h.nbind) {
                const unsigned i = (unsigned)(rnd() % h.nbind); nbd_bind e; memcpy(&e, b + sizeof e * i, sizeof e);
                unsigned last = i;   // the last write to e's slot
                for (unsigned j = i + 1; j < h.nbind; j++) { nbd_bind c; memcpy(&c, b + sizeof c * j, sizeof c); if (c.kind == e.kind && c.stage == e.stage && c.slot == e.slot) last = j; }
                model x = u;
                if (e.kind == NBD_B_TEX) x.tex[e.stage][e.slot] ^= 1; else if (e.kind == NBD_B_SAMP) x.samp[e.stage][e.slot] ^= 1;
                else if (e.kind == NBD_B_IMG) x.img[e.stage][e.slot] ^= 1; else x.ub[e.stage][e.slot].size ^= 1;
                memset(&d, 0, sizeof d);
                const unsigned bad = nbd_compare(&h, b, (const uint64_t (*)[128])x.tex, (const uint64_t (*)[128])x.samp, (const uint64_t (*)[16])x.img,
                                                 (const nbd_range (*)[16])x.ub, &d);
                CHECK(bad == 1u << e.kind && d.kind == e.kind && d.stage == e.stage && d.slot == e.slot);
                (void)last;
            }
        }
    }
    // a bind outside the limits (a hand-made payload; nbd_add would refuse it): nbd_apply skips it and fails, nbd_compare flags it
    nbd_shadow s; fill_hdr(&s, NBD_DRAW_IDX); s.h.flags = 0;
    nbd_add(&s, NBD_B_TEX, 0, 1, 0x55, 0); nbd_add(&s, NBD_B_TEX, 0, 2, 0x66, 0);
    const uint32_t n = nbd_pack(&s, buf, sizeof buf);
    buf[sizeof(nbd_hdr) + sizeof(nbd_bind) + 2] = 200;   // second bind: slot 200
    model_random(&a); const uint64_t keep = a.tex[0][200 & 127];
    nbd_hdr h; const uint8_t *b; unsigned ts, bs;
    CHECK(nbd_view(buf, n, &h, &b) == 0);
    CHECK(nbd_apply(&h, b, a.tex, a.samp, a.img, a.ub, &ts, &bs) == -1 && a.tex[0][1] == 0x55 && a.tex[0][200 & 127] == keep);
    CHECK(nbd_compare(&h, b, (const uint64_t (*)[128])a.tex, (const uint64_t (*)[128])a.samp, (const uint64_t (*)[16])a.img,
                      (const nbd_range (*)[16])a.ub, NULL) & 1u << NBD_B_KINDS);
    buf[sizeof(nbd_hdr) + sizeof(nbd_bind) + 2] = 2; buf[sizeof(nbd_hdr) + sizeof(nbd_bind) + 1] = 6;   // stage 6
    CHECK(nbd_apply(&h, b, a.tex, a.samp, a.img, a.ub, &ts, &bs) == -1);
    buf[sizeof(nbd_hdr) + sizeof(nbd_bind) + 1] = 0; buf[sizeof(nbd_hdr) + sizeof(nbd_bind)] = 7;        // kind 7
    CHECK(nbd_apply(&h, b, a.tex, a.samp, a.img, a.ub, &ts, &bs) == -1);
}
static void test_same_draw(void) {
    nbd_shadow s; fill_hdr(&s, NBD_DRAW_IDX);
    const nbd_hdr *h = &s.h;
    CHECK(nbd_same_draw(h, NBD_DRAW_IDX, 4, 2, 123456, -7, 3, 99, 0x123456789abcdef0ull, 0));
    CHECK(nbd_same_draw(h, NBD_DRAW_IDX, 4, 2, 123456, -7, 3, 99, 0x123456789abcdef0ull, 55));   // first: arrays only
    CHECK(!nbd_same_draw(h, NBD_DRAW_IDX, 4, 2, 123456, -7, 3, 98, 0x123456789abcdef0ull, 0));
    CHECK(!nbd_same_draw(h, NBD_DRAW_IDX, 4, 2, 123456, -7, 3, 99, 0x123456789abcdef1ull, 0));
    CHECK(!nbd_same_draw(h, NBD_DRAW_IDX, 5, 2, 123456, -7, 3, 99, 0x123456789abcdef0ull, 0));
    CHECK(!nbd_same_draw(h, NBD_DRAW_ARRAYS, 4, 2, 123456, -7, 3, 99, 0x123456789abcdef0ull, 0));
    fill_hdr(&s, NBD_DRAW_ARRAYS);
    CHECK(nbd_same_draw(h, NBD_DRAW_ARRAYS, 4, 0, 123456, 0, 0, 0, 0, 11));
    CHECK(!nbd_same_draw(h, NBD_DRAW_ARRAYS, 4, 0, 123456, 0, 0, 0, 0, 12));
}
static double now_ns(void) { struct timespec t; timespec_get(&t, TIME_UTC); return t.tv_sec * 1e9 + t.tv_nsec; }
static void bench(void) {
    static model m; model_random(&m);
    enum { NREC = 256 };
    static uint8_t recs[NREC][sizeof(nbd_hdr) + NBD_MAX_BINDS * sizeof(nbd_bind)]; static uint32_t len[NREC];
    const int sizes[] = {4, 6, 16};
    for (int z = 0; z < 3; z++) {
        for (int r = 0; r < NREC; r++) {   // the flush's shape: UBOs of stages 0 / 4, then texture + sampler pairs of stage 4
            nbd_shadow s; fill_hdr(&s, NBD_DRAW_IDX); s.h.flags = 0;
            for (int i = 0; i < sizes[z]; i++) {
                if (i < 2) nbd_add(&s, NBD_B_UBO, i ? 4 : 0, 2 + i, rnd(), 256);
                else nbd_add(&s, (i & 1) ? NBD_B_SAMP : NBD_B_TEX, 4, (i - 2) / 2, rnd(), 0);
            }
            len[r] = nbd_pack(&s, recs[r], sizeof recs[r]);
        }
        const int iters = 4000;
        double best_u = 1e30, best_a = 1e30;
        for (int rep = 0; rep < 5; rep++) {
            double t0 = now_ns();
            for (int it = 0; it < iters; it++) for (int r = 0; r < NREC; r++) m_unpack(&m, recs[r], len[r]);
            double t1 = now_ns();
            for (int it = 0; it < iters; it++) for (int r = 0; r < NREC; r++) m_apply(&m, recs[r], len[r]);
            double t2 = now_ns();
            if (t1 - t0 < best_u) best_u = t1 - t0;
            if (t2 - t1 < best_a) best_a = t2 - t1;
        }
        const double k = 1.0 / ((double)iters * NREC);
        printf("bench (host, binds part only, draw() excluded): %2d binds a record: unpack %.1f ns, apply %.1f ns a record (x%.1f)%s\n", sizes[z],
               best_u * k, best_a * k, best_a > 0 ? best_u / best_a : 0., m.bad ? " BAD" : "");
    }
}
int main(void) {
    test_roundtrip_fields(); test_max_binds_random(); test_limits(); test_malformed(); test_faithful_order();
    test_check_flag(); test_apply_vs_unpack(); test_same_draw();
    if (fails) { printf("nb_draw_test: %d FAILED\n", fails); return 1; }
    printf("nb_draw_test: PASS (round trip of all fields and kinds, %d binds max, limits, malformed, faithful order; NB-10: check flag, "
           "apply == unpack on the state model (3000 rounds), compare / same draw)\n", NBD_MAX_BINDS);
    bench();
    return 0;
}
