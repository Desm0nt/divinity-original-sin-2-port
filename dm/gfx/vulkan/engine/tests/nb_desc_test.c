// [DOS2] NB-11 host test: the DM_NB_DESC reuse decision (dm/gfx/nb/nb_desc.h) against a model of the descriptor memo key of
// xbg_game.cpp descriptors() and of the backend tables / counters (exec_bind_buf / exec_bind_handles, nbdraw::apply, nbpost).
// Property: whenever nb_desc_pre, nb_desc_post and nb_desc_guard all say OK, the key built now equals the key of the previous
// descriptors() call (so the memo entry the reuse takes is the one the lookup would find). Plus unit cases per reason, a non-vacuous
// reuse rate on random streams, and a rough timing of key build + compare against the decision.
// Build: clang -std=c11 -O2 -Wall -Wextra nb_desc_test.c -o nb_desc_test
#include "../../../nb/nb_desc.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum { SRC_UNIFORM, SRC_DRIVER0, SRC_DRIVER2, SRC_TEXTURE, SRC_IMAGE, SRC_SUPPORT };
typedef struct { unsigned ns, src, slot, count, sampler_slot, bytes, dyn; } Bind;   // dyn = UBO_DYN(dmode, b) of the backend
typedef struct { Bind b[16]; unsigned n; uint64_t id; } Prog;
typedef struct {
    uint64_t tex[6][128], samp[6][128], img[6][16]; nbd_range ub[6][16];
    uint64_t tex_gen[6], buf_gen[6];
    const Prog *prog; uint64_t pool[5]; uint32_t vw, vh, alpha;
} St;
static St S;
static uint64_t memo_gen = 1; static unsigned slot_index = 0;

static unsigned rng_s = 12345;
static unsigned rnd(void) { rng_s = rng_s * 1103515245u + 12345u; return rng_s >> 8; }

// The key words of descriptors() for the model's sources (same order rules: shader identity, pools, per binding).
static size_t key_of(uint64_t *k) {
    size_t n = 0;
    k[n++] = S.prog ? S.prog->id : 0;
    for (int i = 0; i < 5; i++) k[n++] = S.pool[i];
    if (!S.prog) return n;
    for (unsigned i = 0; i < S.prog->n; i++) {
        const Bind *b = &S.prog->b[i];
        for (unsigned e = 0; e < b->count; e++) {
            const unsigned slot = b->slot + e;
            switch (b->src) {
            case SRC_UNIFORM: { const nbd_range r = S.ub[b->ns][slot];
                if (b->dyn && r.addr && r.size) k[n++] = r.size < b->bytes ? r.size : b->bytes;
                else { k[n++] = r.addr; k[n++] = r.size; } break; }
            case SRC_SUPPORT: k[n++] = ((uint64_t)S.vw << 32) | S.vh; k[n++] = S.alpha; break;
            case SRC_DRIVER0: case SRC_DRIVER2: k[n++] = b->dyn ? 0xD0D0 + b->src : (b->src == SRC_DRIVER0 ? S.tex_gen[b->ns] : S.buf_gen[b->ns]); break;
            case SRC_TEXTURE: k[n++] = S.tex[b->ns][slot]; if (b->sampler_slot != ~0u) k[n++] = S.samp[b->ns][b->sampler_slot]; break;
            case SRC_IMAGE: k[n++] = S.img[b->ns][slot]; break;
            }
        }
    }
    return n;
}
// nbdesc::capture of xbg_game.cpp on the model.
static void capture(nb_desc_snap *p, nb_desc_ident *id) {
    memset(p, 0, sizeof *p); memset(id, 0, sizeof *id);
    id->sh[0] = S.prog ? S.prog->id : 0; memcpy(id->pool, S.pool, sizeof S.pool); id->vp = ((uint64_t)S.vw << 32) | S.vh; id->alpha = S.alpha;
    if (S.prog) for (unsigned i = 0; i < S.prog->n; i++) { const Bind *b = &S.prog->b[i];
        for (unsigned e = 0; e < b->count; e++) { const unsigned slot = b->slot + e;
            if (b->src == SRC_UNIFORM) { const nbd_range r = S.ub[b->ns][slot];
                if (b->dyn && r.addr && r.size) p->udyn[b->ns] |= (uint16_t)(1u << slot); else p->ustat[b->ns] |= (uint16_t)(1u << slot); }
            else if ((b->src == SRC_DRIVER0 || b->src == SRC_DRIVER2) && !b->dyn) p->dep |= (uint8_t)(1u << b->ns); } }
    memcpy(p->tex_gen, S.tex_gen, sizeof S.tex_gen); memcpy(p->buf_gen, S.buf_gen, sizeof S.buf_gen);
}
static void ident_now(nb_desc_ident *id) { memset(id, 0, sizeof *id); id->sh[0] = S.prog ? S.prog->id : 0; memcpy(id->pool, S.pool, sizeof S.pool);
    id->vp = ((uint64_t)S.vw << 32) | S.vh; id->alpha = S.alpha; }

// The previous descriptors() call.
static struct { int valid; uint64_t key[256]; size_t nkey; nb_desc_snap s; nb_desc_ident id; uint64_t gen; unsigned slot; } P;
static void descriptors_commit(void) { P.nkey = key_of(P.key); capture(&P.s, &P.id); P.gen = memo_gen; P.slot = slot_index; P.valid = 1; }

// A record: binds into a shadow, then the apply of nbdraw (tables + one bump per touched stage). Returns the decision of pre + post
// and leaves etex / ebuf.
static uint64_t etex[6], ebuf[6];
static int record(const nbd_shadow *sh) {
    uint8_t buf[sizeof(nbd_hdr) + sizeof(nbd_bind) * NBD_MAX_BINDS]; const uint32_t n = nbd_pack(sh, buf, sizeof buf);
    nbd_hdr h; const uint8_t *b; if (nbd_view(buf, n, &h, &b)) { printf("FAIL view\n"); exit(1); }
    int texb = 0, w = P.valid ? nb_desc_pre(&P.s, S.tex_gen, S.buf_gen, &h, b, (const uint64_t(*)[128])S.tex, (const uint64_t(*)[128])S.samp,
                                            (const uint64_t(*)[16])S.img, (const nbd_range(*)[16])S.ub, &texb) : NBDS_PREV;
    unsigned ts, bs; if (nbd_apply(&h, b, S.tex, S.samp, S.img, S.ub, &ts, &bs)) { printf("FAIL apply\n"); exit(1); }
    for (unsigned m = ts; m; m &= m - 1) ++S.tex_gen[__builtin_ctz(m)];
    for (unsigned m = bs; m; m &= m - 1) ++S.buf_gen[__builtin_ctz(m)];
    if (w == NBDS_OK) w = nb_desc_post(&P.s, ts | bs);
    memcpy(etex, S.tex_gen, sizeof etex); memcpy(ebuf, S.buf_gen, sizeof ebuf);
    return w;
}
static int guard(void) { nb_desc_ident now; ident_now(&now);
    return P.valid ? nb_desc_guard(&P.id, &now, S.tex_gen, S.buf_gen, etex, ebuf, memo_gen, P.gen, slot_index, P.slot) : NBDS_PREV; }
static int same_key(void) { uint64_t k[256]; size_t n = key_of(k); return n == P.nkey && !memcmp(k, P.key, n * 8); }
// An old bind record (exec_bind_buf / exec_bind_handles): table + one bump.
static void old_bind_tex(unsigned st, unsigned sl, uint64_t hdl) { S.tex[st][sl] = hdl; ++S.tex_gen[st]; }
static void old_bind_ubo(unsigned st, unsigned sl, uint64_t a, uint64_t sz) { S.ub[st][sl].addr = a; S.ub[st][sl].size = sz; ++S.buf_gen[st]; }

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); ++fails; } } while (0)

// Program A: vertex UBO slot 0 dynamic (256 bytes), fragment UBO slot 1 dynamic, fragment UBO slot 2 static, 3 textures + samplers.
static Prog A = { { {0, SRC_UNIFORM, 0, 1, ~0u, 256, 1}, {1, SRC_UNIFORM, 1, 1, ~0u, 1024, 1}, {1, SRC_UNIFORM, 2, 1, ~0u, 64, 0},
                    {1, SRC_TEXTURE, 0, 3, 0, 0, 0}, {1, SRC_SUPPORT, 0, 1, ~0u, 0, 0}, {1, SRC_DRIVER0, 0, 1, ~0u, 0, 1} }, 6, 0xA };
static Prog B = { { {0, SRC_UNIFORM, 0, 1, ~0u, 256, 1}, {1, SRC_TEXTURE, 0, 1, 0, 0, 0} }, 2, 0xB };
// Program D: a non-dynamic driver block in the fragment stage (the key holds its counter; DM_GFX_UBO_DYNAMIC=0 / PowerVR, or more UBOs than the limit).
static Prog D = { { {0, SRC_UNIFORM, 0, 1, ~0u, 256, 1}, {1, SRC_DRIVER2, 0, 1, ~0u, 0, 0}, {1, SRC_TEXTURE, 0, 1, 0, 0, 0} }, 3, 0xD };

static void reset(const Prog *p) {
    memset(&S, 0, sizeof S); S.prog = p; S.pool[0] = 0x1000; S.pool[1] = 0; S.pool[2] = 4096; S.pool[3] = 0x2000; S.vw = 1280; S.vh = 720; S.alpha = 8;
    for (int st = 0; st < 6; st++) for (int i = 0; i < 128; i++) { S.tex[st][i] = 0x100 + i; S.samp[st][i] = 0x200 + i; }
    for (int st = 0; st < 6; st++) for (int i = 0; i < 16; i++) { S.ub[st][i].addr = 0x10000 + 0x1000 * i; S.ub[st][i].size = 256; }
    S.ub[1][1].size = 1024; S.ub[1][2].size = 64;
    memset(&P, 0, sizeof P); memo_gen = 1; slot_index = 0;
}
static nbd_shadow typical(uint64_t ring) {   // the flush of 0x925740: two ring UBOs per draw, same sizes
    nbd_shadow sh; memset(&sh, 0, sizeof sh); sh.h.draw = NBD_DRAW_IDX; sh.h.engine_op = 36; sh.h.count = 6;
    nbd_add(&sh, NBD_B_UBO, 0, 0, ring, 256); nbd_add(&sh, NBD_B_UBO, 1, 1, ring + 0x100, 1024); return sh;
}

static void units(void) {
    reset(&A); descriptors_commit();
    nbd_shadow sh = typical(0x500000);
    CHECK(record(&sh) == NBDS_OK && guard() == NBDS_OK && same_key(), "ring UBOs, same sizes, no texture change -> reuse");
    descriptors_commit();
    sh = typical(0x600000); nbd_add(&sh, NBD_B_TEX, 1, 1, S.tex[1][1], 0);
    CHECK(record(&sh) == NBDS_OK && guard() == NBDS_OK && same_key(), "a texture rebind to the same handle -> reuse");
    descriptors_commit();
    sh = typical(0x700000); nbd_add(&sh, NBD_B_TEX, 1, 1, 0x9999, 0);
    CHECK(record(&sh) == NBDS_TEX && !same_key(), "a changed texture -> NBDS_TEX");
    descriptors_commit();
    sh = typical(0x700000); nbd_add(&sh, NBD_B_TEX, 1, 100, 0x9999, 0);
    CHECK(record(&sh) == NBDS_TEX && same_key(), "a changed texture in a slot the shaders do not read -> NBDS_TEX (conservative)");
    descriptors_commit();
    sh = typical(0x800000); sh.bind[1].b = 512;
    CHECK(record(&sh) == NBDS_USIZE && !same_key(), "a dynamic UBO of another size -> NBDS_USIZE");
    descriptors_commit();
    sh = typical(0x800000); sh.bind[1].a = 0;
    CHECK(record(&sh) == NBDS_USIZE && !same_key(), "a dynamic UBO unbound (address 0) -> NBDS_USIZE");
    descriptors_commit(); S.ub[1][1].addr = 0x8100; S.ub[1][1].size = 1024; ++S.buf_gen[1]; descriptors_commit();   // dyn form again
    sh = typical(0x900000); nbd_add(&sh, NBD_B_UBO, 1, 2, 0x123400, 64);
    CHECK(record(&sh) == NBDS_USTAT && !same_key(), "a static UBO at another address -> NBDS_USTAT");
    descriptors_commit();
    sh = typical(0x900000); nbd_add(&sh, NBD_B_UBO, 1, 2, S.ub[1][2].addr, 64);
    CHECK(record(&sh) == NBDS_OK && guard() == NBDS_OK && same_key(), "a static UBO rebound to the same range -> reuse");
    descriptors_commit();
    sh = typical(0xA00000); nbd_add(&sh, NBD_B_UBO, 3, 7, 0x55, 4);
    CHECK(record(&sh) == NBDS_OK && guard() == NBDS_OK && same_key(), "a UBO slot the shaders do not read -> reuse");
    descriptors_commit();
    old_bind_tex(1, 0, 0x4242); sh = typical(0xB00000);
    CHECK(record(&sh) == NBDS_GEN && !same_key(), "an old bind record between the draws -> NBDS_GEN");
    descriptors_commit();
    old_bind_ubo(1, 2, S.ub[1][2].addr, 64); sh = typical(0xB00000);
    CHECK(record(&sh) == NBDS_GEN, "even a same-value old bind record -> NBDS_GEN (conservative)");
    descriptors_commit();
    sh = typical(0xC00000); S.prog = &B;
    CHECK(record(&sh) == NBDS_OK && guard() == NBDS_SHADER && !same_key(), "another program (BindProgram record) -> NBDS_SHADER");
    S.prog = &A; descriptors_commit();
    sh = typical(0xC00000); S.pool[0] = 0x3000;
    CHECK(record(&sh) == NBDS_OK && guard() == NBDS_POOL && !same_key(), "another texture pool -> NBDS_POOL");
    descriptors_commit();
    sh = typical(0xC00000); S.vw = 640;
    CHECK(record(&sh) == NBDS_OK && guard() == NBDS_SUPPORT && !same_key(), "another viewport (support block) -> NBDS_SUPPORT");
    descriptors_commit();
    sh = typical(0xD00000);   // a render pass change between the draws: no table / counter / key word changes
    CHECK(record(&sh) == NBDS_OK && guard() == NBDS_OK && same_key(), "a render pass change -> still reuse (the key has no pass)");
    descriptors_commit();
    sh = typical(0xD00000); CHECK(record(&sh) == NBDS_OK, "record"); ++memo_gen;
    CHECK(guard() == NBDS_MEMO, "a memo insert / erase / reset (memo_mod_gen) -> NBDS_MEMO");
    descriptors_commit();
    sh = typical(0xD00000); CHECK(record(&sh) == NBDS_OK, "record"); slot_index ^= 1;
    CHECK(guard() == NBDS_MEMO, "another slot -> NBDS_MEMO");
    descriptors_commit();
    sh = typical(0xE00000); CHECK(record(&sh) == NBDS_OK, "record"); ++S.tex_gen[1]; ++S.buf_gen[1];   // nbpost::pre of a fused draw
    CHECK(guard() == NBDS_LATE, "counters moved between the record and descriptors() (fused post draw) -> NBDS_LATE");
    P.valid = 0; sh = typical(0xE00000);
    CHECK(record(&sh) == NBDS_PREV, "no previous entry -> NBDS_PREV");
    reset(&D); descriptors_commit();
    sh = typical(0xF00000); sh.h.nbind = 1;   // vertex UBO only: the fragment counter (keyed by the driver block) does not move
    CHECK(record(&sh) == NBDS_OK && guard() == NBDS_OK && same_key(), "non-dynamic driver block, record touches another stage -> reuse");
    descriptors_commit();
    nbd_add(&sh, NBD_B_TEX, 1, 0, S.tex[1][0], 0);   // a same-handle rebind in the fragment stage bumps tex_gen[1]
    CHECK(record(&sh) == NBDS_DEP, "non-dynamic driver block of a stage the record bumped -> NBDS_DEP");
}

// Random streams: the property (all OK -> same key) on every draw, and the reuse rate.
static void randomized(void) {
    static const Prog *progs[] = { &A, &B, &D };
    unsigned long draws = 0, reused = 0, why[NBDS_N] = {0};
    for (int round = 0; round < 2000; round++) {
        reset(progs[rnd() % 3]); descriptors_commit();
        for (int d = 0; d < 200; d++) {
            const unsigned ev = rnd() % 100;
            if (ev < 4) old_bind_tex(rnd() % 2, rnd() % 4, 0x100 + rnd() % 3);
            else if (ev < 7) old_bind_ubo(rnd() % 2, rnd() % 3, 0x10000 + (rnd() % 4) * 0x1000, (rnd() % 2) ? 256 : 64);
            else if (ev < 9) S.prog = progs[rnd() % 3];
            else if (ev < 10) S.pool[rnd() % 5] ^= 0x10;
            else if (ev < 11) S.vw = (rnd() % 2) ? 1280 : 640;
            else if (ev < 12) ++memo_gen;
            else if (ev < 13) slot_index ^= 1;
            nbd_shadow sh = typical(0x100000 + 0x1000 * (uint64_t)d);
            if (rnd() % 4 == 0) sh.bind[1].b = (rnd() % 2) ? 1024 : 512;
            if (rnd() % 4 == 0) nbd_add(&sh, NBD_B_TEX, 1, rnd() % 4, 0x100 + rnd() % 4, 0);
            if (rnd() % 6 == 0) nbd_add(&sh, NBD_B_SAMP, 1, rnd() % 4, 0x200 + rnd() % 4, 0);
            if (rnd() % 6 == 0) nbd_add(&sh, NBD_B_UBO, 1, 2, (rnd() % 2) ? S.ub[1][2].addr : 0x77700, 64);
            if (rnd() % 8 == 0) nbd_add(&sh, NBD_B_UBO, 0, 5 + rnd() % 3, rnd() % 2 ? 0 : 0x4000, rnd() % 512);
            int w = record(&sh);
            if (rnd() % 30 == 0) { ++S.tex_gen[1]; ++S.buf_gen[1]; }   // a fused draw between
            if (w == NBDS_OK) w = guard();
            ++draws; ++why[w];
            if (w == NBDS_OK) { ++reused; if (!same_key()) { printf("FAIL round %d draw %d: reuse decided but the key differs\n", round, d); ++fails; return; } }
            if (rnd() % 20 == 0) P.valid = 0; else descriptors_commit();   // a miss without a memo entry / a compute dispatch
        }
    }
    printf("random: %lu draws, reused %lu (%.1f%%); not: prev %lu gen %lu tex %lu ustat %lu usize %lu dep %lu shader %lu pool %lu support %lu late %lu memo %lu\n",
           draws, reused, 100.0 * reused / draws, why[NBDS_PREV], why[NBDS_GEN], why[NBDS_TEX], why[NBDS_USTAT], why[NBDS_USIZE], why[NBDS_DEP],
           why[NBDS_SHADER], why[NBDS_POOL], why[NBDS_SUPPORT], why[NBDS_LATE], why[NBDS_MEMO]);
    CHECK(reused > draws / 10, "reuse rate too low (%lu of %lu): the test would be vacuous", reused, draws);
    for (int k = NBDS_PREV; k <= NBDS_MEMO; k++) if (k != NBDS_LIMIT) CHECK(why[k] > 0, "reason %d never hit", k);
}

static double now_ns(void) { struct timespec t; timespec_get(&t, TIME_UTC); return t.tv_sec * 1e9 + t.tv_nsec; }
static void timing(void) {   // rough, x86 host: key build + compare (the memo front path) against pre + post + guard
    reset(&A); descriptors_commit(); nbd_shadow sh = typical(0x500000);
    uint8_t buf[sizeof(nbd_hdr) + sizeof(nbd_bind) * NBD_MAX_BINDS]; const uint32_t n = nbd_pack(&sh, buf, sizeof buf);
    nbd_hdr h; const uint8_t *b; nbd_view(buf, n, &h, &b);
    enum { R = 2000000 }; volatile unsigned sink = 0; uint64_t k[256];
    double t0 = now_ns();
    for (int i = 0; i < R; i++) { S.ub[0][0].addr += 0x100; size_t nk = key_of(k); sink += nk == P.nkey && !memcmp(k, P.key, nk * 8); }
    double t1 = now_ns();
    for (int i = 0; i < R; i++) { int texb; nb_desc_ident now; ident_now(&now);
        sink += nb_desc_pre(&P.s, S.tex_gen, S.buf_gen, &h, b, (const uint64_t(*)[128])S.tex, (const uint64_t(*)[128])S.samp, (const uint64_t(*)[16])S.img,
                            (const nbd_range(*)[16])S.ub, &texb) == NBDS_OK && nb_desc_post(&P.s, 3) == NBDS_OK &&
                nb_desc_guard(&P.id, &now, S.tex_gen, S.buf_gen, S.tex_gen, S.buf_gen, memo_gen, P.gen, slot_index, P.slot) == NBDS_OK; }
    double t2 = now_ns();
    printf("timing (model, %u key words): key build + compare %.1f ns, decision %.1f ns (sink %u)\n", (unsigned)P.nkey, (t1 - t0) / R, (t2 - t1) / R, sink);
}

int main(void) {
    units(); randomized(); timing();
    if (fails) { printf("FAIL: %d\n", fails); return 1; }
    printf("PASS: NB-11 reuse decision (units, random property: decided reuse -> same key)\n");
    return 0;
}
