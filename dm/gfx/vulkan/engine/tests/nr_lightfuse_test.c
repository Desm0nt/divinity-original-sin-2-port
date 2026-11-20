// [DOS2] NR-LIGHTFUSE host test (dm/gfx/nr/nr_lightfuse.h): the extras check of the fused bundle, the blend rule, the Sky / Light decisions.
//   nr_lightfuse_test [native-nr.xgspack]   with a pack (tools/dos2_nr_sh_pack.py output): its f.ClusteredLightingWithSSAO passes, its
//                                           x.ClusteredLightingWithSSAO (no extras) and a mutated f bundle fail.
#include "../../../nr/nr_lightfuse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
// an XGS1 bundle: table bindings tb[0..nt), SPIR-V declaring sb[0..ns) in descriptor set `set`
static size_t bundle(uint8_t *out, const uint32_t *tb, int nt, const uint32_t *sb, int ns, uint32_t set) {
    uint32_t w[512]; int n = 0;
    w[n++] = 0x07230203u; w[n++] = 0x10300; w[n++] = 0; w[n++] = 1000; w[n++] = 0;
    for (int i = 0; i < ns; i++) {
        w[n++] = 4u << 16 | 71; w[n++] = (uint32_t)(10 + i); w[n++] = 33; w[n++] = sb[i];
        w[n++] = 4u << 16 | 71; w[n++] = (uint32_t)(10 + i); w[n++] = 34; w[n++] = set;
    }
    const size_t head = 32 + 32 * (size_t)nt;
    memset(out, 0, head);
    wr32(out, 0x31534758u); wr32(out + 4, 1); wr32(out + 8, 4); wr32(out + 12, (uint32_t)nt); wr32(out + 16, 0); wr32(out + 20, 0); wr32(out + 24, (uint32_t)n * 4);
    for (int i = 0; i < nt; i++) wr32(out + 32 + 32 * i, tb[i]);
    for (int i = 0; i < n; i++) wr32(out + head + 4 * (size_t)i, w[i]);
    return head + 4 * (size_t)n;
}

static uint8_t *pack_get(const uint8_t *d, size_t n, const char *key, size_t *sz) {
    if (n < 16 || d[0] != 'X' || d[1] != 'G' || d[2] != 'S' || d[3] != 'P') return NULL;
    const uint32_t cnt = nr_lf_rd32(d + 8);
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *e = d + 16 + 56 * (size_t)i;
        if (!strncmp((const char *)e, key, 32)) {
            uint64_t off = nr_lf_rd32(e + 32) | (uint64_t)nr_lf_rd32(e + 36) << 32; *sz = nr_lf_rd32(e + 40);
            if (off + *sz > n) return NULL;
            uint8_t *b = malloc(*sz); memcpy(b, d + off, *sz); return b;
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    static uint8_t b[64 * 1024];
    const uint32_t light[] = {4, 10, 16, 22, 28, 34, 40, 46, 52, 58, 64, 70, 76, 82, 88, 94, 100};
    uint32_t all[32]; int na = 0;
    for (int i = 0; i < 17; i++) all[na++] = light[i];
    for (int k = 0; k < NR_LF_NX; k++) all[na++] = nr_lf_x[k].fused;
    size_t n = bundle(b, light, 17, all, na, 0);
    CHECK(nr_lf_bindings_ok(b, n) == 1, "the Light table + every extra");
    n = bundle(b, light, 17, all, na - 1, 0);
    CHECK(nr_lf_bindings_ok(b, n) == 0, "an extra missing");
    { uint32_t x[32]; memcpy(x, all, sizeof x); x[3] = 199; n = bundle(b, light, 17, x, na, 0); CHECK(nr_lf_bindings_ok(b, n) == 0, "a binding in neither"); }
    n = bundle(b, light, 17, all, na, 1);
    CHECK(nr_lf_bindings_ok(b, n) == 0, "descriptor set 1");
    n = bundle(b, light, 17, light, 17, 0);
    CHECK(nr_lf_bindings_ok(b, n) == 0, "no extras (the NR-04 bundle)");
    n = bundle(b, light, 17, all, na, 0); b[0] ^= 1;
    CHECK(nr_lf_bindings_ok(b, n) == 0 && nr_lf_bindings_ok(NULL, 0) == 0, "bad bundle");
    // blend: ONE (1), ONE (1), ADD (0), RGB written
    CHECK(nr_lf_blend_ok(1, 1, 1, 0, 0xf) && nr_lf_blend_ok(1, 1, 1, 0, 0x7), "ONE ONE ADD");
    CHECK(!nr_lf_blend_ok(0, 1, 1, 0, 0xf) && !nr_lf_blend_ok(1, 6, 7, 0, 0xf) && !nr_lf_blend_ok(1, 1, 0, 0, 0xf) && !nr_lf_blend_ok(1, 1, 1, 1, 0xf) && !nr_lf_blend_ok(1, 1, 1, 0, 0x8),
          "other blends / masks refused");
    // fog: the game's DeferredFog writes R G B (mask 7) into B10G11R11_UFLOAT (122): accepted; R G B A anywhere; A missing on a format with alpha refused
    CHECK(nr_lf_fog_mask_ok(0x7, 122) && nr_lf_fog_mask_ok(0x7, 123) && nr_lf_fog_mask_ok(0xf, 122) && nr_lf_fog_mask_ok(0xf, 97) && nr_lf_fog_mask_ok(0xf, 0),
          "fog mask: RGB on an alpha-less target, RGBA anywhere");
    CHECK(!nr_lf_fog_mask_ok(0x7, 97) && !nr_lf_fog_mask_ok(0x7, 37) && !nr_lf_fog_mask_ok(0x7, 64) && !nr_lf_fog_mask_ok(0x7, 0) && !nr_lf_fog_mask_ok(0x3, 122) &&
          !nr_lf_fog_mask_ok(0xb, 122) && !nr_lf_fog_mask_ok(0x8, 97), "fog mask: RGB only on RGBA16F / RGBA8 / A2B10G10R10 / unknown, or R G B not all written: refused");
    CHECK(nr_lf_vk_no_alpha(122) && nr_lf_vk_no_alpha(23) && nr_lf_vk_no_alpha(90) && !nr_lf_vk_no_alpha(37) && !nr_lf_vk_no_alpha(44) && !nr_lf_vk_no_alpha(97) &&
          !nr_lf_vk_no_alpha(109) && !nr_lf_vk_no_alpha(64) && !nr_lf_vk_no_alpha(2), "alpha-less formats");
    // decisions
    { nr_lf_state s = {0};
      CHECK(nr_lf_sky(&s, 1, 1) == NR_LF_SKY_DRAW, "mode 1: Sky drawn until a chain is learned");
      CHECK(nr_lf_light(&s, 1, 0, 1) == NR_LF_LIGHT_RECORDED && s.learned == 1, "learned at a checked Light");
      CHECK(nr_lf_sky(&s, 1, 0) == NR_LF_SKY_DRAW, "no twins: drawn");
      CHECK(nr_lf_sky(&s, 1, 1) == NR_LF_SKY_SKIP, "learned: skipped");
      CHECK(nr_lf_light(&s, 1, 1, 1) == NR_LF_LIGHT_FUSED && s.fused == 1, "fused");
      CHECK(nr_lf_sky(&s, 3, 1) == NR_LF_SKY_SKIP && nr_lf_light(&s, 3, 1, 0) == NR_LF_LIGHT_LOST && s.learned == 0 && s.lost == 1, "a failed check after the skip: lost, unlearned");
      CHECK(nr_lf_sky(&s, 1, 1) == NR_LF_SKY_DRAW, "after the loss Sky is drawn again");
      nr_lf_light(&s, 1, 0, 1); nr_lf_sky(&s, 1, 1); nr_lf_orphan(&s);
      CHECK(s.learned == 0 && s.lost == 2, "orphaned skip");
      nr_lf_state v = {0};
      CHECK(nr_lf_light(&v, 2, 0, 1) == NR_LF_LIGHT_RECORDED && !v.learned && nr_lf_sky(&v, 2, 1) == NR_LF_SKY_DRAW, "mode 2 never learns / skips");
      CHECK(nr_lf_sky(&v, 0, 1) == NR_LF_SKY_DRAW, "mode 0"); }
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb"); CHECK(f != NULL, "open %s", argv[1]);
        if (f) {
            fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f); uint8_t *d = malloc((size_t)sz);
            if (fread(d, 1, (size_t)sz, f) != (size_t)sz) sz = 0;
            fclose(f);
            size_t fb = 0, xb = 0; uint8_t *fu = pack_get(d, (size_t)sz, "f.ClusteredLightingWithSSAO", &fb), *xu = pack_get(d, (size_t)sz, "x.ClusteredLightingWithSSAO", &xb);
            CHECK(fu && nr_lf_bindings_ok(fu, fb) == 1, "the pack's f bundle passes");
            CHECK(xu && nr_lf_bindings_ok(xu, xb) == 0, "the pack's x bundle has no extras");
            CHECK(fu && xu && nr_lf_head(fu, fb) == nr_lf_head(xu, xb) && !memcmp(fu, xu, 24) && !memcmp(fu + 28, xu + 28, nr_lf_head(xu, xb) - 28), "f and x carry the same translated head");
            if (fu) { const size_t h = nr_lf_head(fu, fb); int mut = 0;   // the first extra's Binding decoration -> 199
                for (size_t i = h + 20; i + 16 <= fb; i += 4) if (nr_lf_rd32(fu + i) == (4u << 16 | 71) && nr_lf_rd32(fu + i + 8) == 33 && nr_lf_rd32(fu + i + 12) == 200) { wr32(fu + i + 12, 199); mut = 1; break; }
                CHECK(mut && nr_lf_bindings_ok(fu, fb) == 0, "a mutated f bundle fails"); }
            free(fu); free(xu); free(d);
        }
    }
    printf("nr_lightfuse_test: %d checks, %d failed%s\n", checks, fails, argc > 1 ? " (with the pack)" : "");
    return fails ? 1 : 0;
}
