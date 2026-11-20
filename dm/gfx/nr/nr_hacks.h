// [DOS2] N6 "Хаки" (docs/dos2/nr/NR-HACKS.md): lossy speed settings, every one 0 = today's picture. Pure C, header only: read by the
// HLE side (dm/host/hle_dos2_quality.c: render scale, bloom, FXAA, shadow maps) and by the Vulkan backend (xbg_nr_hacks.inc: FSR 1 EASU,
// far shadow cascade, texture LOD bias / anisotropy cap, shadow resolve variant). Host test: dm/gfx/vulkan/engine/tests/nr_hacks_test.c.
//
//   DM_HACK_TIER     0 none (default) / 1 light / 2 medium / 3 max: defaults for every DM_HACK_* below that is not set itself
//   DM_HACK_SCALE    0 = 100 % (default); 50..99 = the 3D at that % of the window per side (the game's DRS object: min / ceiling)
//   DM_HACK_FSR      0 = the console's stretch (FXAA / bilinear); 1 = FSR 1 EASU in the stretch pass when the 3D is below 100 %, half
//                    precision (FsrEasuH form, mediump); 3 = the same in fp32 (FsrEasuF); 2 = check: the module is built and checked, not
//                    used. RCAS = the gear's sharpening (DM_GFX_SHARPEN) at present
//   DM_HACK_NOBLOOM  1 = bloom off (GraphicSettings +0x68 = 0)
//   DM_HACK_NOAA     1 = no FXAA at 100 % (as DM_DOS2_AA=0; ignored while FSR stretches a lower scale - the step is the EASU pass)
//   DM_HACK_SHADOWS  1 = shadow maps 512 x 3 (ShadowQuality 2, as DM_DOS2_SHADOWS=2)
//   DM_HACK_SHPCF    0 = the shadow resolve the backend picks; 2 = mobile (textureGather), 3 = dref (hardware compare): as DM_NR_SH_PCF
//   DM_HACK_SHFAR    1 = the far sun-shadow cascade is not drawn (its tile stays cleared: no shadows beyond the middle cascade);
//                    2 = count only (the draws it would skip, drawn)
//   DM_HACK_TEXBIAS  0 = none; <x> = mip LOD bias added to the samplers of uploaded mipmapped textures (0.25 .. 2: blurrier, less bandwidth)
//   DM_HACK_ANISO    0 = the game's; N = anisotropy capped at N (1 = off, 2, 4)
//   DM_HACK_LOD      0 = the game's; 50..99 = every model LOD distance and the cull distance at that % (12 LOD visitors, code caves)
//   DM_HACK_LIGHTS   0 = all; 2 / 4 = the first that many clustered lights that light a pixel (lighting variant; 1 -> 2, 3+ -> 4)
// An explicit DM_HACK_<name> (also "0") wins over the tier. dm/env and the gear's "Хаки" page set these.
#ifndef NR_HACKS_H
#define NR_HACKS_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { NRH_SCALE, NRH_FSR, NRH_NOBLOOM, NRH_NOAA, NRH_SHADOWS, NRH_SHPCF, NRH_SHFAR, NRH_TEXBIAS, NRH_ANISO, NRH_LOD, NRH_LIGHTS, NRH_N };
// Values in hundredths (TEXBIAS 0.5 = 50); others plain integers.
static const char *const nrh_names[NRH_N] = {"SCALE", "FSR", "NOBLOOM", "NOAA", "SHADOWS", "SHPCF", "SHFAR", "TEXBIAS", "ANISO", "LOD", "LIGHTS"};
// Tier table (rows: tier 0..3; round 2, 19.11: less resolution, more per-feature hacks). light = barely visible: 85 % + EASU, mobile
// shadow resolve, LOD distances 90 %; medium: 85 % + EASU, 512 x 3 shadow maps, mobile resolve, mip bias 0.5, anisotropy 4, LOD 80 %,
// at most 4 lights a pixel; max: 75 % + EASU, bloom off, far cascade off, mip bias 1, anisotropy 2, LOD 70 %, at most 2 lights.
static const int nrh_tier_table[4][NRH_N] = {
    /* none   */ {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    /* light  */ {85, 1, 0, 0, 0, 2, 0, 0, 0, 90, 0},
    /* medium */ {85, 1, 0, 0, 1, 2, 0, 50, 4, 80, 4},
    /* max    */ {75, 1, 1, 0, 1, 2, 1, 100, 2, 70, 2},
};
static const int nrh_min[NRH_N] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const int nrh_max[NRH_N] = {99, 3, 1, 1, 1, 3, 2, 200, 16, 99, 4};

// Parse one value: SCALE >= 100 -> 0 (off), < 50 -> 50; TEXBIAS as a float in mip levels -> hundredths; out of range -> clamped.
// Returns 1 when understood.
static inline int nrh_parse(int k, const char *s, int *out) {
    if (!s || !*s || k < 0 || k >= NRH_N) return 0;
    char *end = NULL; int v;
    if (k == NRH_TEXBIAS) { double d = strtod(s, &end); if (end == s) return 0; v = (int)(d * 100.0 + (d >= 0 ? 0.5 : -0.5)); }
    else { long l = strtol(s, &end, 10); if (end == s) return 0; v = (int)l; }
    while (end && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '%')) end++;
    if (end && *end) return 0;
    if (k == NRH_SCALE || k == NRH_LOD) { if (v >= 100 || v <= 0) v = 0; else if (v < 50) v = 50; }
    if (k == NRH_LIGHTS && v > 0) v = v <= 2 ? 2 : 4;
    if (k == NRH_SHPCF && v == 1) v = 0;   // 1 (exact) is no hack
    if (v < nrh_min[k]) v = nrh_min[k];
    if (v > nrh_max[k]) v = nrh_max[k];
    *out = v; return 1;
}
static inline int nrh_tier_of(const char *s) {
    if (!s || !*s) return 0;
    if (!strcmp(s, "light")) return 1;
    if (!strcmp(s, "medium")) return 2;
    if (!strcmp(s, "max")) return 3;
    int t = atoi(s); return t < 0 ? 0 : t > 3 ? 3 : t;
}
// The value of hack k from an environment lookup (getenv, or a test's table): explicit first, then the tier. from (optional): 'e' explicit,
// 't' tier, 0 none.
typedef const char *(*nrh_getenv_fn)(const char *);
static inline int nrh_value_from(int k, nrh_getenv_fn ge, char *from) {
    char name[24] = "DM_HACK_"; strncat(name, nrh_names[k], sizeof name - 9);
    int v = 0;
    if (nrh_parse(k, ge(name), &v)) { if (from) *from = 'e'; return v; }
    const int t = nrh_tier_of(ge("DM_HACK_TIER"));
    v = nrh_tier_table[t][k];
    if (from) *from = v ? 't' : 0;
    return v;
}
// DM_HACK_SHFAR: the cascades of the sun-shadow atlas are its tiles (viewport rects); the frame's first draw into each gives its ordinal (the
// engine draws near to far). The far cascade = the last: ordinal 1 of 2 (a 2048 atlas: 1024 x 2) or 2 of 3 (1536 / 1024: 768 / 512 x 3).
// t: reset (n = 0) at each new frame. Returns 1 = a draw of the far cascade, 0 = another cascade, -1 = a fifth rect (not tracked).
typedef struct { int32_t r[4][4]; int n; } nrh_tiles;
static inline int nrh_far(nrh_tiles *t, const int32_t rect[4], uint32_t atlas_w) {
    int k = 0;
    while (k < t->n && memcmp(t->r[k], rect, sizeof t->r[k])) k++;
    if (k == t->n) { if (t->n >= 4) return -1; memcpy(t->r[t->n++], rect, sizeof t->r[0]); }
    return k == (atlas_w >= 2048 ? 1 : 2);
}
static inline const char *nrh_getenv(const char *n) { return getenv(n); }
static inline int nrh_value(int k, char *from) { return nrh_value_from(k, nrh_getenv, from); }
#endif
