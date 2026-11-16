// [DOS2] NB-18b-3 host test: the fused native bundle format XGF1 (dm/gfx/nb/nb_fused.h). A synthetic blob (2 extras, 1 requirement)
// round trip, validation failures on mutated copies, the host-head match against a synthetic translated bundle, the UBO requirement;
// then, for each file given on the command line (work/shaders-native-post/fused/*.xgs from tools/dos2_post_fuse.py, optionally with
// "--translated <dir>" = work/shaders-vk), parse + host head match against <dir>/<host key>.xgs.
// Build + run (from this directory):
//   clang -std=c11 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS nb_fused_test.c -o nb_fused_test && ./nb_fused_test [files...] [--translated dir]
#include "../../../nb/nb_fused.h"
#include <stdio.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

// translated bundle: 2 bindings, 1 input, 8 bytes c1, 8 bytes "SPIR-V"
static size_t make_translated(uint8_t *t) {
    memset(t, 0, 256);
    const uint32_t h[8] = {NB_FX_XGS_MAGIC, 1, 4, 2, 1, 8, 8, 0};
    for (int i = 0; i < 8; i++) put32(t + 4 * i, h[i]);
    const uint32_t b0[8] = {4, 6, 1, 1392, 1, 0, 0xffffffffu, 0}, b1[8] = {16, 1, 1, 0, 7, 0, 0xffffffffu, 0x80000100u};
    for (int i = 0; i < 8; i++) { put32(t + 32 + 4 * i, b0[i]); put32(t + 64 + 4 * i, b1[i]); }
    put32(t + 96, 0x11); memcpy(t + 100, "c1c1c1c1", 8); put32(t + 108, 0x07230203u); put32(t + 112, 0x10300);
    return 116;
}

// fused blob of that host: extras {58 UBO <- pass 1 support, 64 tex <- pass 1 texture slot 0}, requirement UBO_EQ (1,1,0,0)
enum { X_OFF = 112, R_OFF = X_OFF + 2 * 24, B_OFF = R_OFF + 32 };
static size_t make_fused(uint8_t *f, const uint8_t *t) {
    memset(f, 0, 1024);
    uint8_t *x = f + X_OFF, *r = f + R_OFF, *b = f + B_OFF;
    const uint32_t ex[2][3] = {{58, 6, 1392}, {64, 1, 0}}; const uint8_t ek[2] = {NB_FX_K_SUPPORT, NB_FX_K_TEX};
    for (int i = 0; i < 2; i++) {
        put32(x + 24 * i, ex[i][0]); put32(x + 24 * i + 4, ex[i][1]); put32(x + 24 * i + 8, ex[i][2]);
        x[24 * i + 12] = 1; x[24 * i + 13] = 1; x[24 * i + 14] = ek[i];
    }
    r[0] = 1; r[1] = 0; r[2] = NB_FX_R_UBO_EQ; const float v[4] = {1, 1, 0, 0}; memcpy(r + 16, v, 16);
    memcpy(b, t, 32); put32(b + 12, 4); put32(b + 24, 8);           // header: 2 + 2 bindings, 8 bytes of SPIR-V
    memcpy(b + 32, t + 32, 64);                                       // host bindings
    for (int i = 0; i < 2; i++) {                                     // extra entries
        uint8_t *e = b + 96 + 32 * i;
        put32(e, ex[i][0]); put32(e + 4, ex[i][1]); put32(e + 8, 1); put32(e + 12, ex[i][2]); put32(e + 16, NB_FX_SRC); put32(e + 20, (uint32_t)i);
        put32(e + 24, 0xffffffffu);
    }
    memcpy(b + 160, t + 96, 12); put32(b + 172, 0x07230203u); put32(b + 176, 0x10300);   // inputs + c1, SPIR-V
    const uint32_t bs = 180, h[8] = {NB_FX_MAGIC, 1, NB_FX_F_EXACT, 1, 2, 1, bs, 0};
    for (int i = 0; i < 8; i++) put32(f + 4 * i, h[i]);
    memcpy(f + 32, "91eb2d97a304709a", 16); memcpy(f + 48, "921728a5bbb1c71b", 16);
    return B_OFF + bs;
}

static int parse_bad(const uint8_t *f, size_t n, size_t off, uint8_t val) {
    uint8_t c[1024]; memcpy(c, f, n); c[off] = val; nb_fx fx; const char *why = 0;
    return nb_fx_parse(c, n, &fx, &why) != 0 && why;
}

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long s = ftell(f); rewind(f);
    uint8_t *b = (uint8_t *)malloc((size_t)s); if (b && fread(b, 1, (size_t)s, f) != (size_t)s) { free(b); b = 0; }
    fclose(f); *n = (size_t)s; return b;
}

int main(int argc, char **argv) {
    uint8_t t[256], f[1024]; const size_t tn = make_translated(t), fn = make_fused(f, t);
    nb_fx fx; const char *why = "?";
    CHECK(nb_fx_parse(f, fn, &fx, &why) == 0 && !why);
    CHECK(fx.n_elided == 1 && fx.n_extra == 2 && fx.n_req == 1 && fx.host_bindings == 2 && fx.flags == NB_FX_F_EXACT);
    CHECK(!strcmp(fx.host_key, "91eb2d97a304709a") && !strcmp(fx.elided_key[0], "921728a5bbb1c71b") && !fx.elided_key[1][0]);
    CHECK(fx.spirv_size == 8 && nb_fx_u32(fx.bundle + fx.spirv_off) == 0x07230203u);
    CHECK(nb_fx_find(&fx, 64) && nb_fx_find(&fx, 64)->src_kind == NB_FX_K_TEX && !nb_fx_find(&fx, 16));
    CHECK(nb_fx_host_head_matches(&fx, t, tn));
    { uint8_t t2[256]; memcpy(t2, t, tn); t2[100] ^= 1; CHECK(!nb_fx_host_head_matches(&fx, t2, tn)); }   // c1 differs
    { uint8_t t2[256]; memcpy(t2, t, tn); t2[64] = 22; CHECK(!nb_fx_host_head_matches(&fx, t2, tn)); }   // a host binding differs
    { const float ok[8] = {1, 1, 0, 0, 5, 5, 5, 5}, bad[4] = {0.9f, 0.9f, 0, 0};
      CHECK(nb_fx_req_ubo_ok(&fx.req[0], ok, sizeof ok) && !nb_fx_req_ubo_ok(&fx.req[0], bad, sizeof bad) &&
            !nb_fx_req_ubo_ok(&fx.req[0], ok, 8)); }
    {   // NB_FX_R_VP_MAP (phone fix 16.11): s.xy * size(O) == viewport size, s.zw * size(O) == its origin
        uint8_t c[1024]; memcpy(c, f, fn); c[R_OFF + 2] = NB_FX_R_VP_MAP; nb_fx v; const char *w2 = "?";
        CHECK(nb_fx_parse(c, fn, &v, &w2) == 0 && v.req[0].kind == NB_FX_R_VP_MAP);
        const float full[4] = {0, 0, 2160, 972}, sub[4] = {108, 0, 1944, 972}, one[4] = {1, 1, 0, 0};
        const float part[4] = {1944.0f / 2160, 1, 108.0f / 2160, 0}, zoom[4] = {0.9f, 0.9f, 0, 0}, wide[4] = {2160.0f / 2400, 972.0f / 1080, 0, 0};
        float s[4];
        CHECK(nb_fx_req_vp_ok(&v.req[0], one, 16, full, 2160, 972, s) && s[0] == 1 && s[3] == 0);   // the old UBO_EQ case
        CHECK(nb_fx_req_vp_ok(&v.req[0], part, 16, sub, 2160, 972, 0));                            // a sub-viewport, its own descriptor
        CHECK(!nb_fx_req_vp_ok(&v.req[0], one, 16, sub, 2160, 972, 0));                            // full uv over a sub-viewport: stretched
        CHECK(!nb_fx_req_vp_ok(&v.req[0], zoom, 16, full, 2160, 972, 0));                          // reads 90 % of O: not 1:1
        CHECK(nb_fx_req_vp_ok(&v.req[0], wide, 16, full, 2400, 1080, 0));                          // 2160x972 corner of a 2400x1080 O: 1:1
        CHECK(!nb_fx_req_vp_ok(&v.req[0], wide, 16, full, 2160, 972, 0));                          // descriptor 2400x1080 but O 2160x972
        CHECK(!nb_fx_req_vp_ok(&v.req[0], one, 8, full, 2160, 972, 0));                            // short c-buffer
        CHECK(!nb_fx_req_vp_ok(&v.req[0], one, 16, full, 0, 972, 0));
        CHECK(nb_fx_req_ubo_ok(&v.req[0], zoom, 16));                                              // other kinds: no UBO_EQ verdict
        CHECK(parse_bad(f, fn, R_OFF + 2, 4));                                                     // kind 4 unknown
    }
    {   // NB-18b-8 (F1A): NB_FX_E_LINEAR on a texture extra is carried through
        uint8_t c[1024]; memcpy(c, f, fn); c[X_OFF + 24 + 15] = NB_FX_E_LINEAR; nb_fx v; const char *w2 = "?";
        CHECK(nb_fx_parse(c, fn, &v, &w2) == 0 && (v.extra[1].flags & NB_FX_E_LINEAR) && !(v.extra[0].flags & NB_FX_E_LINEAR) && v.extra[1].src_kind == NB_FX_K_TEX);
    }
    CHECK(parse_bad(f, fn, 0, 0));                             // magic
    CHECK(parse_bad(f, fn, 12, 5));                            // n_elided > 4
    CHECK(parse_bad(f, fn, 32, 'x'));                          // host key not hex
    CHECK(parse_bad(f, fn, 64, 'a'));                          // second elided key while n_elided = 1
    CHECK(parse_bad(f, fn, X_OFF + 12, 2));                    // extra from pass 2 of 1
    CHECK(parse_bad(f, fn, X_OFF + 14, 3));                    // unknown source kind
    CHECK(parse_bad(f, fn, X_OFF + 24 + 4, 6));                // texture extra declared as a UBO
    CHECK(parse_bad(f, fn, R_OFF + 2, 9));                     // unknown requirement
    CHECK(parse_bad(f, fn, B_OFF + 96 + 20, 1));               // bundle extra entry slot != table index
    CHECK(parse_bad(f, fn, B_OFF + 96, 59));                   // bundle extra binding != table
    CHECK(parse_bad(f, fn, B_OFF + 64 + 16, NB_FX_SRC));       // host entry marked fused
    CHECK(parse_bad(f, fn, B_OFF + 172, 0));                   // no SPIR-V magic
    CHECK(nb_fx_parse(f, fn - 4, &fx, &why) != 0);            // truncated
    CHECK(nb_fx_parse(f, 50, &fx, &why) != 0);
    const char *tdir = 0; int nfiles = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--translated") && i + 1 < argc) tdir = argv[++i];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--translated")) { i++; continue; }
        size_t n = 0; uint8_t *b = slurp(argv[i], &n); CHECK(b != 0); if (!b) continue;
        nb_fx r; const char *w = 0; const int rc = nb_fx_parse(b, n, &r, &w); CHECK(rc == 0);
        if (rc) { printf("%s: %s\n", argv[i], w); free(b); continue; }
        nfiles++;
        unsigned lin = 0; for (uint32_t e = 0; e < r.n_extra; e++) { if (r.extra[e].flags & NB_FX_E_LINEAR) { lin++; CHECK(r.extra[e].src_kind == NB_FX_K_TEX); } }
        printf("%s: host %s, elided %u (%s), extras %u (linear %u), requirements %u, flags %u, SPIR-V %u bytes", argv[i], r.host_key, r.n_elided,
               r.n_elided ? r.elided_key[0] : "-", r.n_extra, lin, r.n_req, r.flags, r.spirv_size);
        if (tdir) {
            char path[1024]; snprintf(path, sizeof path, "%s/%s.xgs", tdir, r.host_key);
            size_t tn2 = 0; uint8_t *tb = slurp(path, &tn2); CHECK(tb != 0);
            if (tb) { const int m = nb_fx_host_head_matches(&r, tb, tn2); CHECK(m); printf(", host head %s", m ? "matches" : "DIFFERS"); free(tb); }
        }
        printf("\n"); free(b);
    }
    if (fails) printf("%d FAILED\n", fails); else printf("all passed (%d real file(s))\n", nfiles);
    return fails != 0;
}
