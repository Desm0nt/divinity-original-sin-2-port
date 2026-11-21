// [DOS2] VK-NATIVE stage 5.3 V1 PRESKIN (vkn_preskin.c, DM_VK_NATIVE_PRESKIN): C doors to NR-PRESKIN's SPIR-V split
// (dm/gfx/vulkan/engine/xbg_spirv_preskin.cpp: the plan = minimum cut of a skinning vertex shader, the SLICE = the cut values'
// computation storing raw words, the CONSUMER = the shader reading them as uint inputs; linked as it is from the engine archive, like
// vkn_mat_lib.cpp's trim). Pure functions of their arguments; the words are the library's final words (vkn_shlib_b5_words, retyped
// for the draw's vertex format). No game addresses.
#ifdef DM_GAME_DOS2
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <vector>
#include "vkn_preskin.h"
#include "../vulkan/engine/xbg_spirv_preskin.h"

namespace {
struct Priv { std::vector<uint32_t> words; XbgPreskinPlan p; };
uint32_t *dup(const std::vector<uint32_t> &v) {
    uint32_t *m = static_cast<uint32_t *>(malloc(v.size() * 4 + 4)); if (m) memcpy(m, v.data(), v.size() * 4);
    return m;
}
}  // namespace

extern "C" {
int vkn_psk_candidate(const uint32_t *w, size_t n) { return w && n && xbg_spirv_preskin_candidate(w, n) ? 1 : 0; }
// xbg_spirv_preskin_plan of the words with the module's uniform bindings (bind[i] -> key[i]); the plan's facts copied into *out, the
// words and the C++ plan kept in out->priv for the slice / consumer. 0 = planned, 1 = refused (out->why), -1 = bad arguments / memory.
int vkn_psk_plan_make(const uint32_t *w, size_t n, const uint32_t *bind, const uint32_t *key, uint32_t nbind, vkn_psk_plan *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!w || !n || nbind > 64 || (nbind && (!bind || !key))) { snprintf(out->why, sizeof out->why, "bad arguments"); return -1; }
    Priv *pr = new (std::nothrow) Priv; if (!pr) { snprintf(out->why, sizeof out->why, "out of memory"); return -1; }
    pr->words.assign(w, w + n);
    std::vector<XbgPreskinBind> b(nbind); for (uint32_t i = 0; i < nbind; i++) b[i] = {bind[i], key[i]};
    if (!xbg_spirv_preskin_plan(pr->words.data(), pr->words.size(), b.data(), b.size(), pr->p)) {
        snprintf(out->why, sizeof out->why, "%s", pr->p.why ? pr->p.why : "?"); delete pr; return 1;
    }
    const XbgPreskinPlan &p = pr->p;
    if (p.vals.size() > VKN_PSK_MAXV || p.slice_ubos.size() > VKN_PSK_UBOS) {
        snprintf(out->why, sizeof out->why, "%s", p.vals.size() > VKN_PSK_MAXV ? "too many values" : "the slice reads too many uniform blocks"); delete pr; return 1;
    }
    out->ok = 1; out->m = p.m; out->nvals = uint32_t(p.vals.size());
    for (uint32_t i = 0; i < out->nvals; i++) {
        out->val[i].hash = p.vals[i].hash; out->val[i].comps = p.vals[i].comps; out->val[i].first = p.vals[i].first;
        out->val[i].loc_mask = p.vals[i].loc_mask; out->val[i].ubo_mask = p.vals[i].ubo_mask;
    }
    out->nubo = uint32_t(p.slice_ubos.size());
    for (uint32_t i = 0; i < out->nubo; i++) { out->ubo_binding[i] = p.slice_ubos[i]; out->ubo_key[i] = p.ubo_keys[i]; }
    out->slice_locs = p.slice_locs; out->cons_locs = p.cons_locs; out->orig_locs = p.orig_locs; out->free_locs = p.free_locs;
    out->ssbo_binding = p.ssbo_binding; out->bone_binding = p.bone_binding; out->kdep = p.kdep; out->bone_loads = p.bone_loads;
    out->ops_orig = p.ops_orig; out->ops_cons = p.ops_cons; out->ops_slice = p.ops_slice;
    out->priv = pr;
    return 0;
}
void vkn_psk_plan_free(vkn_psk_plan *p) { if (p && p->priv) { delete static_cast<Priv *>(p->priv); p->priv = nullptr; } }
// The slice module's words (storage buffer at set 0 / ssbo_binding). 0 = *out (malloc'd), else *why.
int vkn_psk_slice(const vkn_psk_plan *p, uint32_t **out, size_t *n, const char **why) {
    *out = nullptr; *n = 0; if (why) *why = "";
    if (!p || !p->priv) { if (why) *why = "no plan"; return -1; }
    const Priv *pr = static_cast<const Priv *>(p->priv); std::vector<uint32_t> o; const char *w = "";
    if (!xbg_spirv_preskin_slice(pr->words.data(), pr->words.size(), pr->p, o, &w)) { if (why) *why = w; return 1; }
    if (!(*out = dup(o))) { if (why) *why = "out of memory"; return -1; }
    *n = o.size(); return 0;
}
// The consumer for record positions pos (plan.m entries; NULL = its own record) and its layout (xbg_spirv_preskin_layout).
int vkn_psk_consumer(const vkn_psk_plan *p, const uint32_t *pos, uint32_t **out, size_t *n, vkn_psk_layout *lay, const char **why) {
    *out = nullptr; *n = 0; if (why) *why = ""; if (lay) memset(lay, 0, sizeof *lay);
    if (!p || !p->priv) { if (why) *why = "no plan"; return -1; }
    const Priv *pr = static_cast<const Priv *>(p->priv); std::vector<uint32_t> o; XbgPreskinLayout l; const char *w = "";
    if (!xbg_spirv_preskin_consumer(pr->words.data(), pr->words.size(), pr->p, pos, o, &l, &w)) { if (why) *why = w; return 1; }
    if (lay) { lay->groups = l.groups; for (uint32_t g = 0; g < l.groups && g < 16; g++) { lay->loc[g] = l.loc[g]; lay->first[g] = l.first[g]; lay->width[g] = l.width[g]; } }
    if (!(*out = dup(o))) { if (why) *why = "out of memory"; return -1; }
    *n = o.size(); return 0;
}
}
#endif
