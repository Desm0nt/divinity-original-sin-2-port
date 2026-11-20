// [DOS2] NR-33B DM_NR_MAT: host test of dm/gfx/nr/nr_mat.c (docs/dos2/nr/NR-33B.md).
//   clang -std=c11 -Wall -Wextra nr_mat_test.c ../../../nr/nr_mat.c -o nr_mat_test.exe && nr_mat_test.exe
// CLI for tools/dos2_nr_mat_proof.py (the proof over every translated vertex shader of the player's pack):
//   nr_mat_test.exe scan  in.spv                -> "model M flags F in IN out OUT"
//   nr_mat_test.exe prune in.spv KEEP out.spv   -> "removed R pruned P words N" (KEEP hex), or "refused C"
//   nr_mat_test.exe empty out.spv               -> the empty fragment shader
#include "../../../nr/nr_mat.h"
#include "nr_mat_test_spv.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define N(a) (sizeof(a) / sizeof((a)[0]))

static uint32_t fb(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

typedef struct { uint32_t bw[4], val[4]; int n; } words;
static int rd(void *ctx, uint32_t bw, uint32_t *out) {
    const words *w = (const words *)ctx;
    for (int i = 0; i < w->n; i++) if (w->bw[i] == bw) { *out = w->val[i]; return 1; }
    return 0;
}

static size_t put(uint8_t *b, size_t o, uint32_t v) { memcpy(b + o, &v, 4); return o + 4; }

static int has_op(const uint32_t *w, size_t n, uint32_t opw) {
    for (size_t i = 5; i < n;) { if ((w[i] & 0xffff) == opw) return 1; i += w[i] >> 16 ? w[i] >> 16 : 1; }
    return 0;
}

static void unit(void) {
    nr_mat_facts f;
    // the empty fragment shader
    size_t ne; const uint32_t *e = nr_mat_empty_fs(&ne);
    CHECK(ne == 32 && e[0] == 0x07230203u && e[3] == 5);
    CHECK(nr_mat_scan(e, ne, &f) == 0 && f.model == 4 && f.flags == 0 && f.in_mask == 0 && f.out_mask == 0 && nr_mat_fs_pure(&f));
    // scan
    CHECK(nr_mat_scan(spv_vs3, N(spv_vs3), &f) == 0 && f.model == 0 && f.out_mask == 0xe && f.in_mask == 0x11 && !(f.flags & NR_MAT_COMPLEX_IO));
    CHECK(!nr_mat_fs_pure(&f));                                    // a vertex shader is not a fragment one
    CHECK(nr_mat_scan(spv_fsk, N(spv_fsk), &f) == 0 && f.model == 4 && (f.flags & NR_MAT_KILL) && f.in_mask == 0x4 && f.out_mask == 1 && !nr_mat_fs_pure(&f));
    CHECK(nr_mat_scan(spv_fsd, N(spv_fsd), &f) == 0 && (f.flags & NR_MAT_FRAGOUT) && !(f.flags & NR_MAT_KILL) && !nr_mat_fs_pure(&f));
    CHECK(nr_mat_scan(spv_fsw, N(spv_fsw), &f) == 0 && (f.flags & NR_MAT_SIDE) && !nr_mat_fs_pure(&f));
    CHECK(nr_mat_scan(spv_fsp, N(spv_fsp), &f) == 0 && f.flags == 0 && f.in_mask == 0xc && nr_mat_fs_pure(&f));
    uint32_t bad[8] = {0x07230203u, 0x10000, 0, 9, 0, 0, 0, 0};
    CHECK(nr_mat_scan(bad, 8, &f) == -1 && f.flags == NR_MAT_BAD);   // a zero-length instruction
    CHECK(nr_mat_scan(NULL, 0, &f) == -1);
    // prune
    uint32_t out[512]; size_t on = 0; uint32_t pm = 0;
    CHECK(N(spv_vs3) <= N(out));
    int r = nr_mat_prune(spv_vs3, N(spv_vs3), 0x4, out, &on, &pm);   // keep location 2 (uv) only
    CHECK(r == 2 && pm == 0xa && on == N(spv_vs3) - 6);
    CHECK(nr_mat_scan(out, on, &f) == 0 && f.out_mask == 0xe);       // still declared: the interface does not change
    {   // no store into locations 1 / 3 left, the one into 2 and gl_Position kept
        int st = 0; for (size_t i = 5; i < on; i += out[i] >> 16) if ((out[i] & 0xffff) == 62) st++;
        int st0 = 0; for (size_t i = 5; i < N(spv_vs3); i += spv_vs3[i] >> 16) if ((spv_vs3[i] & 0xffff) == 62) st0++;
        CHECK(st == st0 - 2);
    }
    r = nr_mat_prune(spv_vs3, N(spv_vs3), 0, out, &on, &pm);           // the empty FS: every Location output
    CHECK(r == 3 && pm == 0xe);
    CHECK(nr_mat_prune(spv_vs3, N(spv_vs3), 0xe, out, &on, &pm) == -4 && pm == 0 && on == 0);   // all read: nothing to do
    CHECK(nr_mat_prune(spv_vs3, N(spv_vs3), ~0u, out, &on, &pm) == -4);
    CHECK(nr_mat_prune(spv_vsrd, N(spv_vsrd), 0, out, &on, &pm) == -2);  // gl_Position = o1 + o2 reads the outputs back: refused
    CHECK(nr_mat_prune(bad, 8, 0, out, &on, &pm) == -1);
    CHECK(has_op(spv_vs3, N(spv_vs3), 62));
    // comparisons
    const float nan = nanf("");
    CHECK(nr_mat_cmp(180, fb(1.0f), fb(1.0f)) == 1 && nr_mat_cmp(180, fb(nan), fb(nan)) == 0 && nr_mat_cmp(181, fb(nan), fb(1.0f)) == 1);
    CHECK(nr_mat_cmp(182, fb(nan), fb(1.0f)) == 0 && nr_mat_cmp(183, fb(nan), fb(1.0f)) == 1 && nr_mat_cmp(182, fb(2.0f), fb(1.0f)) == 1);
    CHECK(nr_mat_cmp(184, fb(1.0f), fb(2.0f)) == 1 && nr_mat_cmp(184, fb(1.0f), fb(nan)) == 0 && nr_mat_cmp(185, fb(1.0f), fb(nan)) == 1);
    CHECK(nr_mat_cmp(186, fb(-0.0f), fb(0.0f)) == 0 && nr_mat_cmp(180, fb(-0.0f), fb(0.0f)) == 1 && nr_mat_cmp(190, fb(0.0f), fb(-0.0f)) == 1);
    CHECK(nr_mat_cmp(188, fb(INFINITY), fb(INFINITY)) == 1 && nr_mat_cmp(187, fb(nan), fb(0.0f)) == 1 && nr_mat_cmp(189, fb(3.0f), fb(2.0f)) == 0);
    CHECK(nr_mat_cmp(173, 0xffffffffu, 1) == 0 && nr_mat_cmp(172, 0xffffffffu, 1) == 1 && nr_mat_cmp(177, 0x80000000u, 0) == 1 && nr_mat_cmp(176, 0x80000000u, 0) == 0);
    CHECK(nr_mat_cmp(170, 5, 5) == 1 && nr_mat_cmp(171, 5, 5) == 0 && nr_mat_cmp(179, 7, 7) == 1 && nr_mat_cmp(175, 0, 0xffffffffu) == 1);
    CHECK(nr_mat_cmp(169, 0, 0) == -1 && nr_mat_cmp(192, 0, 0) == -1);
    // alpha test
    CHECK(nr_mat_alpha_pass(5, fb(1.0f), 0.5f) == 1 && nr_mat_alpha_pass(5, fb(0.5f), 0.5f) == 0 && nr_mat_alpha_pass(5, fb(nan), 0.5f) == 0);
    CHECK(nr_mat_alpha_pass(1, fb(1.0f), 0.5f) == 0 && nr_mat_alpha_pass(8, fb(nan), 0.5f) == 1 && nr_mat_alpha_pass(0, fb(1.0f), 0.0f) == 0);
    CHECK(nr_mat_alpha_pass(6, fb(nan), 0.5f) == 0 && nr_mat_alpha_pass(6, fb(1.0f), 0.5f) == 1 && nr_mat_alpha_pass(3, fb(0.5f), 0.5f) == 1);
    CHECK(nr_mat_alpha_pass(2, fb(0.25f), 0.5f) == 1 && nr_mat_alpha_pass(4, fb(0.5f), 0.5f) == 1 && nr_mat_alpha_pass(7, fb(0.25f), 0.5f) == 0);
    // gate records: the Base_PBR fade shape (path 0: 1.0 < U -> w 1.0; path 1: !(1.0 < U) && !(1.0 == U) -> w 1.0)
    uint8_t b[256]; size_t o = 0;
    const uint32_t bw = (3u << 16) | 0u;
    o = put(b, o, 0x314d4758u); o = put(b, o, 1); o = put(b, o, 0x89abcdefu); o = put(b, o, 0x01234567u); o = put(b, o, 2); o = put(b, o, 3);
    o = put(b, o, fb(1.0f)); o = put(b, o, 1); o = put(b, o, 0); o = put(b, o, 1);
    o = put(b, o, fb(1.0f)); o = put(b, o, 1); o = put(b, o, 1); o = put(b, o, 2);
    o = put(b, o, bw); o = put(b, o, 184); o = put(b, o, fb(1.0f)); o = put(b, o, 2);     // 1.0 < U true (constant on the left)
    o = put(b, o, bw); o = put(b, o, 184); o = put(b, o, fb(1.0f)); o = put(b, o, 0);     // 1.0 < U false
    o = put(b, o, bw); o = put(b, o, 180); o = put(b, o, fb(1.0f)); o = put(b, o, 0);     // 1.0 == U false
    nr_mat_gate g;
    CHECK(nr_mat_gate_parse(b, o, &g) == 0 && g.paths == 2 && g.terms == 3 && g.spirv_fnv == 0x0123456789abcdefull && g.path[1].first == 1);
    words u = {{bw}, {fb(0.0f)}, 1};
    CHECK(nr_mat_gate_eval(&g, rd, &u) == 1);                     // no fade: path 1
    u.val[0] = fb(2.0f); CHECK(nr_mat_gate_eval(&g, rd, &u) == 0);
    u.val[0] = fb(1.0f); CHECK(nr_mat_gate_eval(&g, rd, &u) == -1);   // the dissolve: keep the shader
    u.val[0] = fb(nan); CHECK(nr_mat_gate_eval(&g, rd, &u) == 1);     // NaN: both ordered comparisons false
    u.n = 0; CHECK(nr_mat_gate_eval(&g, rd, &u) == -1);               // unbound uniform: no path
    CHECK(nr_mat_gate_eval(&g, NULL, &u) == -1);
    uint8_t c[256];
    memcpy(c, b, o); c[0] ^= 1; CHECK(nr_mat_gate_parse(c, o, &g) != 0);
    memcpy(c, b, o); c[4] = 2; CHECK(nr_mat_gate_parse(c, o, &g) != 0);
    CHECK(nr_mat_gate_parse(b, o - 4, &g) != 0);
    memcpy(c, b, o); put(c, 24 + 32 + 16 + 4, 169); CHECK(nr_mat_gate_parse(c, o, &g) != 0);         // not a comparison
    memcpy(c, b, o); put(c, 24 + 16 + 12, 3); CHECK(nr_mat_gate_parse(c, o, &g) != 0);              // path past its terms
    memcpy(c, b, o); put(c, 24 + 32, (2u << 16)); CHECK(nr_mat_gate_parse(c, o, &g) != 0);          // bank 2: not a uniform slot
    memcpy(c, b, o); put(c, 16, 0); CHECK(nr_mat_gate_parse(c, 24 + 16 * 3, &g) != 0);              // no paths
    CHECK(nr_mat_fnv64("", 0) == 0xcbf29ce484222325ull && nr_mat_fnv64("a", 1) == 0xaf63dc4c8601ec8cull);
}

static uint32_t *load(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc((size_t)l + 4); *n = (size_t)l / 4;
    if (fread(w, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(w); return NULL; }
    fclose(f); return w;
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "scan")) {
        size_t n; uint32_t *w = load(argv[2], &n); if (!w) return 2;
        nr_mat_facts f; const int r = nr_mat_scan(w, n, &f);
        printf("model %u flags %u in %x out %x%s\n", f.model, f.flags, f.in_mask, f.out_mask, r ? " bad" : ""); return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "prune")) {
        size_t n, on = 0; uint32_t *w = load(argv[2], &n), pm = 0; if (!w) return 2;
        uint32_t *o = malloc(n * 4 + 4); const int r = nr_mat_prune(w, n, (uint32_t)strtoul(argv[3], NULL, 16), o, &on, &pm);
        if (r < 0) { printf("refused %d\n", r); return 0; }
        FILE *f = fopen(argv[4], "wb"); if (!f || fwrite(o, 4, on, f) != on) return 3; fclose(f);
        printf("removed %d pruned %x words %zu\n", r, pm, on); return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "empty")) {
        size_t n; const uint32_t *e = nr_mat_empty_fs(&n); FILE *f = fopen(argv[2], "wb"); if (!f) return 3;
        fwrite(e, 4, n, f); fclose(f); return 0;
    }
    unit();
    printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
