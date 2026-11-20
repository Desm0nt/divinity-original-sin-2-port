// [DOS2] N6 host test (docs/dos2/nr/NR-HACKS.md): dm/gfx/nr/nr_hacks.h - parsing of every DM_HACK_*, the tiers, explicit values over the
// tier (also an explicit 0), clamping, and the far-cascade ordinal rule of DM_HACK_SHFAR on the atlas layouts of ShadowQuality 0 / 1 / 2.
// Build: clang -std=c11 -O2 -Wall -Wextra nr_hacks_test.c   (no other file)
#include "../../../nr/nr_hacks.h"
#include <stdio.h>

static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; if (fails < 30) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

// a fake environment
static const char *env_k[16], *env_v[16]; static int env_n;
static void env_clear(void) { env_n = 0; }
static void env_set(const char *k, const char *v) { env_k[env_n] = k; env_v[env_n++] = v; }
static const char *env_get(const char *k) { for (int i = 0; i < env_n; i++) if (!strcmp(env_k[i], k)) return env_v[i]; return NULL; }

static void test_parse(void) {
    int v = -1;
    CHECK(!nrh_parse(NRH_SCALE, NULL, &v) && !nrh_parse(NRH_SCALE, "", &v) && !nrh_parse(NRH_SCALE, "abc", &v) && !nrh_parse(NRH_SCALE, "75x", &v), "garbage refused");
    CHECK(nrh_parse(NRH_SCALE, "75", &v) && v == 75, "75 -> %d", v);
    CHECK(nrh_parse(NRH_SCALE, "75%", &v) && v == 75, "75%% -> %d", v);
    CHECK(nrh_parse(NRH_SCALE, "100", &v) && v == 0, "100 = off -> %d", v);
    CHECK(nrh_parse(NRH_SCALE, "120", &v) && v == 0, "120 = off -> %d", v);
    CHECK(nrh_parse(NRH_SCALE, "0", &v) && v == 0, "0 = off");
    CHECK(nrh_parse(NRH_SCALE, "30", &v) && v == 50, "30 -> 50 (floor) %d", v);
    CHECK(nrh_parse(NRH_SCALE, "-5", &v) && v == 0, "negative = off %d", v);
    CHECK(nrh_parse(NRH_TEXBIAS, "0.5", &v) && v == 50, "0.5 -> 50 (%d)", v);
    CHECK(nrh_parse(NRH_TEXBIAS, "1", &v) && v == 100, "1 -> 100");
    CHECK(nrh_parse(NRH_TEXBIAS, "0.25", &v) && v == 25, "0.25");
    CHECK(nrh_parse(NRH_TEXBIAS, "9", &v) && v == 200, "9 -> 200 (clamp) %d", v);
    CHECK(nrh_parse(NRH_TEXBIAS, "-1", &v) && v == 0, "negative bias -> 0 (%d)", v);
    CHECK(nrh_parse(NRH_FSR, "1", &v) && v == 1 && nrh_parse(NRH_FSR, "2", &v) && v == 2 && nrh_parse(NRH_FSR, "7", &v) && v == 3 && nrh_parse(NRH_FSR, "3", &v) && v == 3, "FSR 0..3");
    CHECK(nrh_parse(NRH_LOD, "80", &v) && v == 80 && nrh_parse(NRH_LOD, "100", &v) && v == 0 && nrh_parse(NRH_LOD, "20", &v) && v == 50, "LOD %% (100 = off, floor 50)");
    CHECK(nrh_parse(NRH_LIGHTS, "1", &v) && v == 2 && nrh_parse(NRH_LIGHTS, "3", &v) && v == 4 && nrh_parse(NRH_LIGHTS, "9", &v) && v == 4 &&
          nrh_parse(NRH_LIGHTS, "0", &v) && v == 0, "LIGHTS -> 0 / 2 / 4");
    CHECK(nrh_parse(NRH_SHPCF, "1", &v) && v == 0, "SHPCF 1 (exact) is no hack");
    CHECK(nrh_parse(NRH_SHPCF, "3", &v) && v == 3 && nrh_parse(NRH_SHPCF, "2", &v) && v == 2, "SHPCF 2 / 3");
    CHECK(nrh_parse(NRH_ANISO, "4", &v) && v == 4 && nrh_parse(NRH_ANISO, "64", &v) && v == 16, "ANISO clamp");
    CHECK(nrh_parse(NRH_SHFAR, "1\n", &v) && v == 1, "trailing newline (an env file) accepted");
    CHECK(!nrh_parse(-1, "1", &v) && !nrh_parse(NRH_N, "1", &v), "bad index");
}

static void test_tiers(void) {
    char f;
    CHECK(nrh_tier_of(NULL) == 0 && nrh_tier_of("") == 0 && nrh_tier_of("light") == 1 && nrh_tier_of("medium") == 2 && nrh_tier_of("max") == 3, "tier names");
    CHECK(nrh_tier_of("2") == 2 && nrh_tier_of("9") == 3 && nrh_tier_of("-1") == 0 && nrh_tier_of("custom") == 0, "tier numbers");
    // nothing set: every hack 0, no source
    env_clear();
    for (int k = 0; k < NRH_N; k++) { f = 'x'; CHECK(nrh_value_from(k, env_get, &f) == 0 && f == 0, "unset %s", nrh_names[k]); }
    // each tier = its table row, source 't' where non-zero
    static const char *tn[4] = {"0", "1", "2", "3"};
    for (int t = 0; t < 4; t++) {
        env_clear(); env_set("DM_HACK_TIER", tn[t]);
        for (int k = 0; k < NRH_N; k++) { f = 'x'; int v = nrh_value_from(k, env_get, &f);
            CHECK(v == nrh_tier_table[t][k] && f == (v ? 't' : 0), "tier %d %s = %d (%c)", t, nrh_names[k], v, f ? f : '0'); }
    }
    // explicit beats the tier, also an explicit 0
    env_clear(); env_set("DM_HACK_TIER", "max"); env_set("DM_HACK_SCALE", "90"); env_set("DM_HACK_SHFAR", "0"); env_set("DM_HACK_TEXBIAS", "garbage");
    CHECK(nrh_value_from(NRH_SCALE, env_get, &f) == 90 && f == 'e', "explicit scale");
    CHECK(nrh_value_from(NRH_SHFAR, env_get, &f) == 0 && f == 'e', "explicit 0 wins over the tier");
    CHECK(nrh_value_from(NRH_TEXBIAS, env_get, &f) == 100 && f == 't', "garbage explicit -> the tier");
    CHECK(nrh_value_from(NRH_NOBLOOM, env_get, &f) == 1 && f == 't', "the rest from the tier");
    // tier sanity: light never draws less of the scene (no far cascade, bloom stays), max has the lowest scale; tier 0 is all zero
    for (int k = 0; k < NRH_N; k++) CHECK(nrh_tier_table[0][k] == 0, "tier 0 row is today");
    CHECK(nrh_tier_table[1][NRH_SHFAR] == 0 && nrh_tier_table[1][NRH_NOBLOOM] == 0 && nrh_tier_table[2][NRH_SHFAR] == 0, "light / medium keep the scene");
    CHECK(nrh_tier_table[1][NRH_SCALE] >= nrh_tier_table[2][NRH_SCALE] && nrh_tier_table[2][NRH_SCALE] > nrh_tier_table[3][NRH_SCALE], "scales fall with the tier");
    CHECK(nrh_tier_table[2][NRH_SCALE] >= 85, "round 2: medium keeps 85 %% or more (the owner: not resolution first)");
    CHECK(nrh_tier_table[1][NRH_LOD] > nrh_tier_table[2][NRH_LOD] && nrh_tier_table[2][NRH_LOD] > nrh_tier_table[3][NRH_LOD], "LOD falls with the tier");
    for (int t = 1; t < 4; t++) for (int k = 0; k < NRH_N; k++) {
        int v; char b[16]; snprintf(b, sizeof b, "%d", nrh_tier_table[t][k]);
        if (k == NRH_TEXBIAS) snprintf(b, sizeof b, "%.2f", nrh_tier_table[t][k] / 100.0);
        CHECK(nrh_parse(k, b, &v) && v == nrh_tier_table[t][k], "tier %d %s round-trips through the env form (%s -> %d)", t, nrh_names[k], b, v);
    }
}

static void test_far(void) {
    // ShadowQuality 1: atlas 1536, tiles 768 at (0,0) (768,0) (0,768); draws come cascade by cascade, a cascade may be re-entered
    nrh_tiles t; memset(&t, 0, sizeof t);
    const int32_t c0[4] = {0, 0, 768, 768}, c1[4] = {768, 0, 768, 768}, c2[4] = {0, 768, 768, 768}, c3[4] = {768, 768, 768, 768}, odd[4] = {1, 1, 5, 5};
    CHECK(nrh_far(&t, c0, 1536) == 0 && nrh_far(&t, c0, 1536) == 0, "near");
    CHECK(nrh_far(&t, c1, 1536) == 0, "middle");
    CHECK(nrh_far(&t, c2, 1536) == 1 && nrh_far(&t, c2, 1536) == 1, "far = third");
    CHECK(nrh_far(&t, c0, 1536) == 0, "near again keeps its ordinal");
    CHECK(nrh_far(&t, c3, 1536) == 0, "a fourth rect is not the far one");
    CHECK(nrh_far(&t, odd, 1536) == -1, "a fifth rect is not tracked");
    CHECK(nrh_far(&t, c2, 1536) == 1, "still far");
    // next frame: reset
    t.n = 0;
    CHECK(nrh_far(&t, c1, 1536) == 0 && nrh_far(&t, c0, 1536) == 0 && nrh_far(&t, c2, 1536) == 1, "order by first draw, not by position");
    // ShadowQuality 0: atlas 2048, 2 cascades of 1024: the second is the far one
    t.n = 0; const int32_t d0[4] = {0, 0, 1024, 1024}, d1[4] = {1024, 0, 1024, 1024};
    CHECK(nrh_far(&t, d0, 2048) == 0 && nrh_far(&t, d1, 2048) == 1, "2 cascades: the second is far");
    // ShadowQuality 2: atlas 1024, 3 x 512
    t.n = 0; const int32_t e0[4] = {0, 0, 512, 512}, e1[4] = {512, 0, 512, 512}, e2[4] = {0, 512, 512, 512};
    CHECK(nrh_far(&t, e0, 1024) == 0 && nrh_far(&t, e1, 1024) == 0 && nrh_far(&t, e2, 1024) == 1, "512 x 3: the third");
}

int main(void) {
    test_parse(); test_tiers(); test_far();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
