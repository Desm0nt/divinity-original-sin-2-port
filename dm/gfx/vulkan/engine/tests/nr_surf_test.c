// [DOS2] NR-33C: dm/gfx/nr/nr_surf.{h,c} - record parse, the alpha gate, the variant choice. Build (Windows clang):
//   clang -std=c11 -D_CRT_SECURE_NO_WARNINGS -I../../../nr nr_surf_test.c ../../../nr/nr_surf.c -o nr_surf_test.exe
#include "nr_surf.h"
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void rec(uint8_t *b, uint64_t fnv, uint32_t v, uint32_t z) { memcpy(b, "XSR1", 4); memcpy(b + 4, &fnv, 8); memcpy(b + 12, &v, 4); memcpy(b + 16, &z, 4); }

int main(void) {
    // ---- FNV-1a 64 (the same as tools/dos2_nr_surf.py fnv64)
    CHECK(nr_surf_fnv64((const uint8_t *)"", 0) == 0xcbf29ce484222325ull);
    CHECK(nr_surf_fnv64((const uint8_t *)"a", 1) == 0xaf63dc4c8601ec8cull);
    // ---- records
    uint8_t b[24]; nr_surf_rec r;
    rec(b, 0x1122334455667788ull, NR_SURF_V_K | NR_SURF_V_A | NR_SURF_V_M | NR_SURF_V_P, 0);
    CHECK(!nr_surf_rec_parse(b, 20, &r) && r.spirv_fnv == 0x1122334455667788ull && r.variants == 15);
    CHECK(nr_surf_rec_parse(b, 19, &r) != 0); CHECK(nr_surf_rec_parse(b, 24, &r) != 0); CHECK(nr_surf_rec_parse(0, 20, &r) != 0);
    rec(b, 1, 0, 0); CHECK(nr_surf_rec_parse(b, 20, &r) != 0);              // no variant
    rec(b, 1, 0x10, 0); CHECK(nr_surf_rec_parse(b, 20, &r) != 0);           // unknown bit
    rec(b, 1, 1, 7); CHECK(nr_surf_rec_parse(b, 20, &r) != 0);              // reserved word
    rec(b, 1, 1, 0); b[0] = 'Y'; CHECK(nr_surf_rec_parse(b, 20, &r) != 0);  // magic
    rec(b, 1, NR_SURF_V_A, 0); CHECK(!nr_surf_rec_parse(b, 20, &r) && r.variants == NR_SURF_V_A);   // alpha only (no kill in the shader)
    // ---- the alpha gate: colour 0 SRC_ALPHA / ONE_MINUS_SRC_ALPHA ADD, alpha ONE / ONE_MINUS_SRC_ALPHA ADD (the surface pass)
    nr_surf_att at[3] = {{1, 6, 7, 0, 1, 7, 0, 0xf}, {0, 1, 0, 0, 1, 0, 0, 0xf}, {0, 1, 0, 0, 1, 0, 0, 0}};
    CHECK(nr_surf_alpha_gate(at, 2, 0x1, 0, 0, 0, 0, 0) == 0);                  // attachment 1 enabled but not written by the shader
    CHECK(nr_surf_alpha_gate(at, 2, 0x3, 0, 0, 0, 0, 0) == NR_SURF_G_OTHER);    // ... written: its own alpha decides there
    CHECK(nr_surf_alpha_gate(at, 1, 0x1, 1, 0, 0, 0, 0) == NR_SURF_G_DEPTH);
    CHECK(nr_surf_alpha_gate(at, 1, 0x1, 0, 1, 0, 0, 0) == NR_SURF_G_STENCIL);
    CHECK(nr_surf_alpha_gate(at, 1, 0x1, 0, 0, 1, 0, 0) == NR_SURF_G_A2C);
    CHECK(nr_surf_alpha_gate(at, 1, 0x1, 0, 0, 0, 1, 0) == NR_SURF_G_LOGIC);
    CHECK(nr_surf_alpha_gate(at, 1, 0x1, 0, 0, 0, 0, 1) == NR_SURF_G_QUERY);
    CHECK(nr_surf_alpha_gate(0, 0, 0x1, 0, 0, 0, 0, 0) == NR_SURF_G_BLEND);
    nr_surf_att x = at[0];
    x.dst_c = 1; CHECK(nr_surf_alpha_gate(&x, 1, 1, 0, 0, 0, 0, 0) == 0);           // additive with source alpha: dst + src * 0
    x = at[0]; x.src_c = 1; CHECK(nr_surf_alpha_gate(&x, 1, 1, 0, 0, 0, 0, 0) == NR_SURF_G_BLEND);   // premultiplied: src * 1 is seen
    x = at[0]; x.blend = 0; CHECK(nr_surf_alpha_gate(&x, 1, 1, 0, 0, 0, 0, 0) == NR_SURF_G_BLEND);   // no blending: the colour lands
    x = at[0]; x.op_c = 2; CHECK(nr_surf_alpha_gate(&x, 1, 1, 0, 0, 0, 0, 0) == NR_SURF_G_BLEND);
    x = at[0]; x.dst_a = 0; CHECK(nr_surf_alpha_gate(&x, 1, 1, 0, 0, 0, 0, 0) == NR_SURF_G_BLEND);  // dst alpha * 0: alpha cleared
    x = at[0]; x.src_a = 0; CHECK(nr_surf_alpha_gate(&x, 1, 1, 0, 0, 0, 0, 0) == 0);                // any source alpha factor: As * G = 0
    x = at[0]; x.blend = 0; x.mask = 0; CHECK(nr_surf_alpha_gate(&x, 1, 1, 0, 0, 0, 0, 0) == 0);   // nothing written at all
    CHECK(nr_surf_alpha_gate(at, 1, 0x1, 1, 1, 1, 1, 1) == (NR_SURF_G_DEPTH | NR_SURF_G_STENCIL | NR_SURF_G_A2C | NR_SURF_G_LOGIC | NR_SURF_G_QUERY));
    // ---- choice
    CHECK(nr_surf_choose(NR_SURF_OFF, 15, 0) == 0);
    CHECK(nr_surf_choose(NR_SURF_EXACT, 15, 0) == 'a'); CHECK(nr_surf_choose(NR_SURF_EXACT, 15, NR_SURF_G_DEPTH) == 'k');
    CHECK(nr_surf_choose(NR_SURF_MOBILE, 15, 0) == 'm'); CHECK(nr_surf_choose(NR_SURF_MOBILE, 15, NR_SURF_G_BLEND) == 'k');
    CHECK(nr_surf_choose(NR_SURF_POISON, 15, 0) == 'p'); CHECK(nr_surf_choose(NR_SURF_CHECK, 15, 0) == 'a');
    CHECK(nr_surf_choose(NR_SURF_EXACT, NR_SURF_V_K, 0) == 'k');                 // a G-buffer program: kills only
    CHECK(nr_surf_choose(NR_SURF_EXACT, NR_SURF_V_A, NR_SURF_G_QUERY) == 0);     // no kill point and the gate fails: the game's shader
    CHECK(nr_surf_choose(NR_SURF_MOBILE, NR_SURF_V_K | NR_SURF_V_A, 0) == 'k'); // no m variant made
    CHECK(nr_surf_choose(9, 15, 0) == 0);
    printf("%s nr_surf_test: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
