// [DOS2] VK-NATIVE stage 5.2 B4: the depth pre-pass hybrid (vkn_prepass.h has the feed of the model, the interplay, the switches).
// Ported from xbg_game.cpp namespace prepass (config / retired / raster_hash / pre / read / Guard); the model xbg_prepass.h is used as
// it is. A draw on no depth target or on another depth than the open table's costs one pointer compare and two loads. No game addresses.
#ifdef DM_GAME_DOS2
#include "vkn_prepass.h"
#include "vkn_draw.h"
#include "vkn_buf.h"
#include "vkn_fx.h"
#include "vkn_frame.h"
#include "vkn_pass.h"
#include "vkn_query.h"
#include "vkn_shader.h"
#include "vkn_state.h"
#include "vkn_tex.h"
#include "../vulkan/engine/xbg_prepass.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
extern int vkn_shlib_on(void) __attribute__((weak));
extern int vkn_shlib_fs_facts(const void *shader) __attribute__((weak));
extern int vkn_shlib_b5_words(void *shader, uint64_t retype, uint32_t **words, size_t *nwords, uint8_t **raw, size_t *nraw) __attribute__((weak));

enum { IDS = 16384 };
enum { R_CFG, R_BAD, R_FALLBACK, R_TABLE, R_N };
enum { FS_KILL = 1 };   // vkn_shlib.h VKN_SHLIB_FS_KILL
static struct {
    int mode, inst, reg, hist, test_on; uint32_t admit; uint64_t log_every, logged;
    prepass_ctx *pp;
    vkn_prepass_stats s;
    uint32_t reds[R_N];
    uint16_t leq[256];                     // engine depth-stencil content id -> its LEQUAL + write twin (vkn_fx_ds)
    // [fixpp] one chain function only (the chain has 12 places and the merged tree uses 10 of them): the override of the draw in hand
    // is checked at the next call of this function against the chain's skip counter (vkn_fx_stats.skipped) taken when it was given
    int pend; uint64_t pend_key, pend_skipped;
    int repair;   // [eng3] DM_VK_NATIVE_PREPASS_REPAIR (default 1): skipped draws no G-buffer draw claimed are drawn again late
    int nofs_rule;   // [mgpu P4] DM_VK_NATIVE_PREPASS_NOFS
    vkn_prepass_test_fn test;
} g;
static const vkn_shader *g_fs_s[IDS]; static uint64_t g_fs_h[IDS]; static int8_t g_fs_c[IDS];

static void pred(int kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void pred(int kind, const char *fmt, ...) {
    g.s.reds++;
    if (kind < 0 || kind >= R_N || g.reds[kind]++ >= 16) return;
    char b[900]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    vkn_logf(RED "prepass: %s", b);
}

// ---------------------------------------------------------------- the fragment class of a shader (once per shader id)
// xbg_game.cpp load_shader_key: empty_fs = one function whose body holds only OpLabel (248) / OpReturn (253) (OpFunction 54 /
// OpFunctionEnd 56 around it), on the final words; discards = OpKill / OpTerminateInvocation / OpDemoteToHelperInvocation.
typedef struct fs_job { const vkn_shader *s; int cls; } fs_job;
static void fs_lib(void *a) {
    fs_job *j = (fs_job *)a; uint32_t *w = NULL; size_t n = 0;
    j->cls = 3;
    if (vkn_shlib_b5_words(j->s->xs[VKN_SRC_FS], 0, &w, &n, NULL, NULL)) { free(w); return; }
    int empty = 1, fns = 0, in_fn = 0;
    for (size_t i = 5; i < n && empty;) {
        const uint32_t c = w[i] >> 16, op = w[i] & 0xffff;
        if (!c) break;
        if (op == 54) { ++fns; in_fn = 1; } else if (op == 56) in_fn = 0; else if (in_fn && op != 248 && op != 253) empty = 0;
        i += c;
    }
    free(w);
    if (empty && fns == 1) j->cls = 1;
}
static void *fs_run(void *a) { fs_lib(a); return NULL; }
static int fs_class(const vkn_shader *s) {
    if (!(s->stages & (1u << VKN_SRC_FS))) return 0;
    const uint32_t id = s->id;
    if (id < IDS && g_fs_s[id] == s && g_fs_h[id] == s->handle) return g_fs_c[id];
    int c = 3;
    if (g.test) { c = g.test(s); if (c < 0) { c = -1; g.s.facts_unknown++; } }
    else if (!(vkn_shlib_on && vkn_shlib_on() && vkn_shlib_fs_facts && s->xs[VKN_SRC_FS])) { c = -1; g.s.facts_unknown++; }
    else {
        const int f = vkn_shlib_fs_facts(s->xs[VKN_SRC_FS]);
        if (f < 0) { c = -1; g.s.facts_unknown++; }
        else if (f & FS_KILL) c = 2;
        else if (!f && vkn_shlib_b5_words) {   // the empty-main scan needs the words (the chain's rewrites: an 8 MB stack)
            fs_job j = {s, 3}; pthread_attr_t at; pthread_t th;
            if (!pthread_attr_init(&at)) {
                pthread_attr_setstacksize(&at, 8u << 20);
                if (!pthread_create(&th, &at, fs_run, &j)) pthread_join(th, NULL); else fs_lib(&j);
                pthread_attr_destroy(&at);
            }
            c = j.cls;
        }
    }
    if (id < IDS) { g_fs_s[id] = s; g_fs_h[id] = s->handle; g_fs_c[id] = (int8_t)c; }
    return c;
}

// ---------------------------------------------------------------- feed helpers
static uint64_t mix(uint64_t h, uint64_t w) { h = (h ^ w) * 0x100000001b3ull; return h ^ (h >> 29); }
static uint64_t raster_hash(const vkn_draw_in *in, uint16_t raster) {   // the old raster_hash: cull / fill / offset, viewport, scissor
    uint64_t h = mix(0x9E3779B97F4A7C15ull, raster);
    uint32_t v[4]; memcpy(v, in->viewport, sizeof v);
    h = mix(h, (uint64_t)v[0] << 32 | v[1]); h = mix(h, (uint64_t)v[2] << 32 | v[3]);
    h = mix(h, (uint64_t)(uint32_t)in->scissor[0] << 32 | (uint32_t)in->scissor[1]); h = mix(h, (uint64_t)(uint32_t)in->scissor[2] << 32 | (uint32_t)in->scissor[3]);
    return h;
}
static uint16_t leq_of(uint16_t base) {   // the same depth-stencil state with depth func LEQUAL (3) and write on
    uint16_t v = g.leq[base & 255];
    if (v) return v;
    uint8_t a[8];
    if (vkn_ds_args(base, a)) return 0;
    v = vkn_fx_ds(1, 1, 3, a[3], a[4], a[5], a[6], a[7]);
    if (!v) { g.s.ds_full++; pred(R_TABLE, "no depth-stencil id for the LEQUAL + write twin of state %u (table full): the G-buffer draw keeps its state (its key is lost)", base); return 0; }
    return g.leq[base & 255] = v;
}
static int pair_open(void) {   // a report pair is open now (COND != 0 only: with 0 the pairs decide nothing)
    if (!vkn_query_mode()) return 0;
    vkn_query_stats q; vkn_query_stats_get(&q);
    return q.begins > q.ends;
}
// [eng2] the instance of a draw: its per-object block = vertex binding 0 ("PGlobals" of every DOS2 material program: WorldMatrix at 0
// for static meshes, BoneMatrices from 0 and WorldMatrix at 6144 for skinned ones, DecalViewMatrix for decals). The GLSLC reflection of
// the game's 1860 DEP / DEF program pairs (Materials.pak + Engine.pak): 1800 have the same block, the other 60 differ only by trailing
// members of the DEF one (decal dimensions), never in the first 64 / 128 bytes - so the depth pre-pass draw and the G-buffer draw of
// one object hash the same bytes, two objects of one mesh at different places do not. Hashed: bytes [0, 64) (block < 128 bytes) or
// [0, 128) (>= 128), plus [6144, 6208) of a skinned block. 0 = no block of >= 64 bytes (instanced STI programs, screen quads): no
// identity - such a draw is never skipped (keep "noid") and never claims. Only a shader whose vertex stage declares that block counts:
// an announced block persists (vkn_draw_ubo), so a program without it would carry the previous draw's one.
static uint64_t inst_id(const vkn_shader *sh) {
    int has = 0;
    for (uint16_t i = 0; sh && i < sh->nbind && !has; i++) has = sh->bind[i].kind == VKN_BK_UNIFORM && sh->bind[i].stage == 0 && sh->bind[i].slot == 0;
    if (!has) return 0;
    uint32_t off = 0, n = 0;
    if (vkn_draw_ubo_get(0, 0, &off, &n) || n < 64) return 0;
    static const uint8_t *ring; if (!ring) ring = (const uint8_t *)vkn_ring_cpu();
    if (!ring) return 0;
    const uint8_t *p = ring + off; uint64_t h = 0x50474c4f42414c53ull, w;
    const uint32_t m = n >= 128 ? 128 : 64;
    for (uint32_t i = 0; i < m; i += 8) { memcpy(&w, p + i, 8); h = mix(h, w); }
    if (n >= 6208) for (uint32_t i = 6144; i < 6208; i += 8) { memcpy(&w, p + i, 8); h = mix(h, w); }
    return h ? h : 1;
}
// ---------------------------------------------------------------- [eng3] the skipped draws, kept for a late repair
// Phone G03 (Mali model, 140 s walk): one frame "unmatched 1, unsafe 1" - a skipped pre-pass draw whose G-buffer draw did not come in the
// G-buffer pass (it came later in another pass, under other per-object bytes, or not at all): its depth stayed missing for what came
// after. Every skipped draw is now kept here (its inputs, the constant blocks it saw, the slot ids, the buffer versions) for the table
// it belongs to. Two things use it:
//   twin by mesh  a G-buffer draw whose instance key has no pre-pass draw this frame but whose MESH key (index range) has a skipped draw
//                 nobody claimed is that draw's G-buffer draw with other per-object bytes (the block changed between the two passes):
//                 it claims for it (mode 1: the override), instead of drawing EQUAL against the missing depth;
//   repair        before the first draw after the G-buffer pass that could see the scene depth without them - a draw into it in another
//                 colour pass, or any draw that samples it - every skipped draw still unclaimed is drawn again into the scene depth (the
//                 same draw: the depth pass is min(depth, z) of LESS / LEQUAL fragments, its order does not matter), with the engine
//                 shader (the pieces' variants write the same depth). From then on the depth is the one the pre-pass would have left.
// A copy or a dispatch that reads the depth first is still seen only afterwards (counted bad, as before).
enum { REC_N = 512 };
typedef struct pp_ub { uint32_t off, size; uint8_t ok; } pp_ub;
typedef struct pp_rec {
    uint64_t key, mesh; vkn_draw_in in; uint32_t first, count, inst; uint8_t has_t[2], has_s[2], done;
    int32_t tex[2][VKN_TEX_SLOTS], smp[2][VKN_TEX_SLOTS]; pp_ub ub[2][VKN_UBO_SLOTS]; uint64_t vver[3], iver;
} pp_rec;
static pp_rec *g_rec; static uint32_t g_nrec; static uint64_t g_rec_frame, g_rec_depth; static vkn_tex *g_rec_D;
enum { MESH_N = 1024 };
static uint64_t g_mesh_k[MESH_N]; static uint32_t g_mesh_n[MESH_N];   // mesh key -> skipped records (a hint for the twin search)
static void rec_reset(uint64_t frame, uint64_t depth, vkn_tex *D) {
    if (g_rec_D) vkn_tex_unref(g_rec_D);
    g_rec_D = D && vkn_tex_ref(D) ? D : NULL;
    g_nrec = 0; g_rec_frame = frame; g_rec_depth = depth;
    memset(g_mesh_n, 0, sizeof g_mesh_n);
}
static uint32_t *mesh_slot(uint64_t mesh, int create) {
    uint32_t i = (uint32_t)(mesh * 0x9E3779B97F4A7C15ull >> 54) & (MESH_N - 1);
    for (int p = 0; p < 16; p++, i = (i + 1) & (MESH_N - 1)) {
        if (g_mesh_n[i] && g_mesh_k[i] == mesh) return &g_mesh_n[i];
        if (!g_mesh_n[i]) { if (!create) return NULL; g_mesh_k[i] = mesh; return &g_mesh_n[i]; }
    }
    return NULL;
}
static int rec_room(void) {
    if (!g_rec && !(g_rec = (pp_rec *)malloc(REC_N * sizeof *g_rec))) return 0;
    return g_nrec < REC_N;
}
static void rec_add(uint64_t key, uint64_t mesh, const vkn_draw_in *in, const vkn_fx_ctx *ctx) {
    pp_rec *r = &g_rec[g_nrec++];
    r->key = key; r->mesh = mesh; r->in = *in; r->first = ctx->first; r->count = ctx->count; r->inst = ctx->instances ? ctx->instances : 1; r->done = 0;
    for (int st = 0; st < 2; st++) {
        r->has_t[st] = in->tex[st] != NULL; r->has_s[st] = in->smp[st] != NULL;
        if (in->tex[st]) memcpy(r->tex[st], in->tex[st], sizeof r->tex[st]);
        if (in->smp[st]) memcpy(r->smp[st], in->smp[st], sizeof r->smp[st]);
        for (uint32_t sl = 0; sl < VKN_UBO_SLOTS; sl++) r->ub[st][sl].ok = !vkn_draw_ubo_get(st, sl, &r->ub[st][sl].off, &r->ub[st][sl].size);
    }
    for (int i = 0; i < 3; i++) r->vver[i] = in->vb[i] ? vkn_buf_version(VKN_BUF_VB, in->vb[i], NULL) : 0;
    r->iver = in->ib ? vkn_buf_version(VKN_BUF_IB, in->ib, NULL) : 0;
    uint32_t *m = mesh_slot(mesh, 1); if (m) (*m)++;
}
// Draws the records of the table still owed (skipped - claimed - repaired, per key) into the scene depth. ok: nothing read the depth first.
static void repair_all(vkn_fx_out *out, int ok) {
    prepass_frame *f = &g.pp->f;
    if (!g_nrec || !g_rec_D || f->depth != g_rec_depth) return;
    vkn_fx_out keep = *out;
    uint32_t n0 = 0, lay[VKN_PASS_COLORS], mip[VKN_PASS_COLORS]; vkn_tex *col[VKN_PASS_COLORS], *dep = NULL, *held[VKN_PASS_COLORS + 1];
    vkn_pass_cur_get(&n0, col, lay, mip, &dep);
    for (uint32_t i = 0; i <= VKN_PASS_COLORS; i++) { vkn_tex *t = i < VKN_PASS_COLORS ? col[i] : dep; held[i] = t && vkn_tex_ref(t) ? t : NULL; }
    pp_ub ub0[2][VKN_UBO_SLOTS];
    for (int st = 0; st < 2; st++) for (uint32_t sl = 0; sl < VKN_UBO_SLOTS; sl++) ub0[st][sl].ok = !vkn_draw_ubo_get(st, sl, &ub0[st][sl].off, &ub0[st][sl].size);
    int switched = 0;
    for (uint32_t i = 0; i < g_nrec; i++) {
        pp_rec *r = &g_rec[i];
        if (r->done) continue;
        r->done = 1;
        prepass_slot *sl = prepass_find(f, r->key, 0);
        if (!sl || sl->used + sl->repaired >= sl->skipped) continue;   // claimed (or repaired) already
        int stale = r->in.ib && vkn_buf_version(VKN_BUF_IB, r->in.ib, NULL) != r->iver;
        for (int k = 0; k < 3; k++) if (r->in.vb[k] && vkn_buf_version(VKN_BUF_VB, r->in.vb[k], NULL) != r->vver[k]) stale = 1;
        if (stale) { g.s.repair_stale++; pred(R_BAD, "repair: a skipped draw's vertex / index buffer was written since it was skipped: not drawn again (its depth stays missing this frame)"); continue; }
        if (!switched) { vkn_fx_bypass(1); vkn_pass_targets(0, NULL, NULL, NULL, g_rec_D); switched = 1; }
        vkn_draw_in in = r->in;
        in.tex[0] = r->has_t[0] ? r->tex[0] : NULL; in.tex[1] = r->has_t[1] ? r->tex[1] : NULL;
        in.smp[0] = r->has_s[0] ? r->smp[0] : NULL; in.smp[1] = r->has_s[1] ? r->smp[1] : NULL;
        for (int st = 0; st < 2; st++) for (uint32_t u = 0; u < VKN_UBO_SLOTS; u++) if (r->ub[st][u].ok) vkn_draw_ubo(st, u, r->ub[st][u].off, r->ub[st][u].size);
        if (vkn_draw_indexed(&in, r->first, r->count, r->inst)) { g.s.repair_drop++; pred(R_BAD, "repair: the draw path dropped a skipped draw drawn again (its depth stays missing this frame)"); continue; }
        prepass_note_repair(g.pp, r->key, 1, ok);
        g.s.repaired++;
    }
    if (switched) {
        for (int st = 0; st < 2; st++) for (uint32_t u = 0; u < VKN_UBO_SLOTS; u++) if (ub0[st][u].ok) vkn_draw_ubo(st, u, ub0[st][u].off, ub0[st][u].size);
        vkn_pass_targets(n0, col, lay, mip, dep);
        vkn_fx_bypass(0);
        g.s.repair_runs++;
    }
    for (uint32_t i = 0; i <= VKN_PASS_COLORS; i++) if (held[i]) vkn_tex_unref(held[i]);
    *out = keep;
    g_nrec = 0;   // (the table is repaired: a later claim of those keys only gives an override, harmless on the restored depth)
}
static int samples_tex(const vkn_draw_in *in, const vkn_shader *sh, const vkn_tex *D) {   // the draw's shader reads D through a slot
    if (!sh || !D || D->pool_id == ~0u) return 0;
    for (uint16_t i = 0; i < sh->nbind; i++) {
        const vkn_bind *b = &sh->bind[i];
        if (b->kind != VKN_BK_TEXTURE || b->stage > 1 || b->slot >= VKN_TEX_SLOTS) continue;
        const int32_t *ids = in->tex[b->stage]; if (ids && ids[b->slot] == (int32_t)D->pool_id) return 1;
    }
    return 0;
}
static int read_layout(VkImageLayout l) {
    return l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || l == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL || l == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
           l == VK_IMAGE_LAYOUT_GENERAL || l == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
}

// ---------------------------------------------------------------- the end of a frame with a pre-pass (old: retired)
static void line_of(const prepass_result *r) {
    vkn_logf("prepass: DM_VK_NATIVE_PREPASS=%d gate=%d frame=%llu pre_draws=%u cand=%u skip=%u pre_verts=%llu cand_verts=%llu skip_verts=%llu keep_verts=%llu keep kill=%u fs=%u state=%u "
             "stencil=%u alpha=%u counting=%u notindexed=%u late=%u full=%u first=%u mixed=%u new=%u admitted=%u occ_after_skip=%u | gbuffer draws=%u other_pass=%u dw=%u "
             "df never=%u less=%u equal=%u lequal=%u greater=%u notequal=%u gequal=%u always=%u override=%u refused func=%u blend=%u kill=%u raster=%u stencil=%u lost=%u "
             "vbdiff=%u hazards stencil=%u bounds=%u func=%u pass=%u blendw=%u read=%u | keys=%u ok=%u unsafe_keys=%u bad=%u unmatched=%u ambiguous=%u refused_keys=%u "
             "unsafe=%u | total skipped=%llu override=%llu bad_frames=%llu bad_keys=%llu fallbacks=%llu; backend: facts unknown %llu, pre-pass draws kept in an open "
             "report pair %llu, overrides lost after the chain %llu, ds table full %llu, red lines %llu; [mgpu] fragment stage dropped by an earlier piece %llu (rule %d); "
             "[I3] G-buffer stencil draws whose result does not depend on depth (no hazard) %llu; [eng2] instance keys: kept without an identity %u, "
             "same instance twice (exact, not bad) %u; [eng3] skipped draws drawn again late (no G-buffer draw claimed them) %u (total %llu in %llu "
             "runs, stale %llu, dropped %llu), G-buffer twins found by mesh (other per-object bytes) %llu, kept for lack of a record %llu",
             g.mode, g.pp->gate, (unsigned long long)r->frame, r->pre_draws, r->pre_cand, r->pre_skip, (unsigned long long)r->pre_verts, (unsigned long long)r->cand_verts,
             (unsigned long long)r->skip_verts, (unsigned long long)r->keep_verts, r->keep[PREPASS_K_KILL], r->keep[PREPASS_K_FS], r->keep[PREPASS_K_STATE],
             r->keep[PREPASS_K_STENCIL], r->keep[PREPASS_K_ALPHA], r->keep[PREPASS_K_COUNTING], r->keep[PREPASS_K_NOTINDEXED], r->keep[PREPASS_K_LATE],
             r->keep[PREPASS_K_FULL], r->keep[PREPASS_K_FIRST], r->keep[PREPASS_K_MIXED], r->keep[PREPASS_K_NEW], r->admitted, r->occ_after_skip,
             r->gb_draws, r->gb_other, r->gb_dw, r->gb_df[1], r->gb_df[2], r->gb_df[3], r->gb_df[4], r->gb_df[5], r->gb_df[6], r->gb_df[7], r->gb_df[8], r->gb_override,
             r->gb_refused[PREPASS_R_FUNC], r->gb_refused[PREPASS_R_BLEND], r->gb_refused[PREPASS_R_KILL], r->gb_refused[PREPASS_R_RASTER], r->gb_refused[PREPASS_R_STENCIL],
             r->gb_refused[PREPASS_R_LOST], r->vbdiff, r->hazard[PREPASS_H_STENCIL], r->hazard[PREPASS_H_BOUNDS], r->hazard[PREPASS_H_FUNC], r->hazard[PREPASS_H_PASS],
             r->hazard[PREPASS_H_BLENDW], r->hazard[PREPASS_H_READ], r->keys, r->ok_keys, r->unsafe_keys, r->bad_keys, r->unmatched, r->ambiguous, r->refused, r->unsafe,
             (unsigned long long)g.pp->total_skipped, (unsigned long long)g.pp->total_override, (unsigned long long)g.pp->total_bad_frames, (unsigned long long)g.pp->total_bad_keys,
             (unsigned long long)g.s.fallbacks, (unsigned long long)g.s.facts_unknown, (unsigned long long)g.s.counting_open, (unsigned long long)g.s.lost,
             (unsigned long long)g.s.ds_full, (unsigned long long)g.s.reds, (unsigned long long)g.s.nofs, g.nofs_rule, (unsigned long long)g.s.st_free, r->keep[PREPASS_K_NOID], r->amb_same, r->repaired, (unsigned long long)g.s.repaired,
             (unsigned long long)g.s.repair_runs, (unsigned long long)g.s.repair_stale, (unsigned long long)g.s.repair_drop, (unsigned long long)g.s.twin_mesh,
             (unsigned long long)g.s.rec_full);
}
static void retired(void) {
    const prepass_result *r = &g.pp->last; const uint32_t bad = g.pp->prev_frame_bad;
    g.s.tables = g.pp->tables; g.s.bad_frames = g.pp->total_bad_frames; g.s.bad_keys = g.pp->total_bad_keys;
    if (bad) pred(R_BAD, "frame %llu: %u bad keys (unmatched draws %u, ambiguous %u, refused %u, unsafe %u; hazards stencil %u bounds %u func %u other-pass %u blend+write %u "
                  "read %u) - mode %d %s", (unsigned long long)r->frame, bad, r->unmatched, r->ambiguous, r->refused, r->unsafe, r->hazard[PREPASS_H_STENCIL],
                  r->hazard[PREPASS_H_BOUNDS], r->hazard[PREPASS_H_FUNC], r->hazard[PREPASS_H_PASS], r->hazard[PREPASS_H_BLENDW], r->hazard[PREPASS_H_READ], g.mode,
                  g.mode == PREPASS_COUNT ? "would have drawn this frame wrong for those meshes" : "drew this frame wrong for those meshes (one frame; the history drops them)");
    if (g.mode == PREPASS_SKIP && prepass_fallback_note(g.pp, bad)) {
        g.mode = PREPASS_COUNT; g.s.fallbacks++;
        pred(R_FALLBACK, "fallback: more than %u frames with bad keys in %u frames: DM_VK_NATIVE_PREPASS 1 -> 2 (count only) for this session", PREPASS_FB_BAD, PREPASS_FB_WINDOW);
    }
    if (r->frame < g.logged + g.log_every) return;
    g.logged = r->frame;
    line_of(r);
}

// ---------------------------------------------------------------- the chain functions
static void pp_fx(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)user;
    if (g.pend) {   // [fixpp] a skip since the override: a later technique skipped that draw, or an earlier one a draw in between - lost
        vkn_fx_stats fs; vkn_fx_stats_get(&fs);   // (the second case is only a refusal too many: the key is not skipped next frame)
        if (fs.skipped != g.pend_skipped) { prepass_note_lost(g.pp, g.pend_key); g.s.lost++; }
        g.pend = 0;
    }
    vkn_tex *d = ctx->depth;
    // [eng3] the repair point: unclaimed skips of the open table, and this draw is past the G-buffer pass or reads the scene depth
    if (g_nrec && g.mode == PREPASS_SKIP && g.repair && ctx->frame == g_rec_frame && g.pp->f.depth == g_rec_depth) {
        prepass_frame *f = &g.pp->f;
        int colors0 = 0; uint64_t ph0 = 0x9E3779B97F4A7C15ull;
        for (uint32_t i = 0; i < VKN_PASS_COLORS; i++) { if (ctx->color[i]) colors0 = 1; ph0 = mix(ph0, (uint64_t)(uintptr_t)ctx->color[i]); }
        const int on_d = d && (uint64_t)(uintptr_t)d == g_rec_depth;
        const int other_pass = on_d && colors0 && f->gb_pass && (ph0 ? ph0 : 1) != f->gb_pass;
        const int reads = samples_tex(in, ctx->shader, g_rec_D);
        int ok = 1;
        if (!on_d && g_rec_D && f->pre_cand > f->claims && read_layout(g_rec_D->layout)) { prepass_note_read(g.pp, g_rec_depth); g.s.reads++; ok = 0; }   // read already (a copy / dispatch)
        if (other_pass || reads || !ok) repair_all(out, ok);
    }
    if (!d || g.mode <= 0) return;
    if (prepass_frame_begin(g.pp, ctx->frame)) retired();
    if (g.mode <= 0) return;
    const uint64_t serial = (uint64_t)(uintptr_t)d;
    int colors = 0;
    for (uint32_t i = 0; i < VKN_PASS_COLORS; i++) if (ctx->color[i]) { colors = 1; break; }
    if (!colors && !prepass_is_depth_only(0, 1, d->w, d->h)) return;
    if (colors && (!g.pp->f.depth || g.pp->f.depth != serial)) return;   // not the open scene pre-pass's depth
    if (g.pp->f.depth == serial && g.pp->f.pre_cand > g.pp->f.claims && read_layout(d->layout)) { prepass_note_read(g.pp, serial); g.s.reads++; }   // sampled / copied since
    const vkn_shader *s = ctx->shader;
    const uint16_t ds = out->ds ? out->ds : ctx->ds;
    uint8_t a[8]; memset(a, 0, sizeof a);
    if (ds) vkn_ds_args(ds, a);   // test, write, func (Vulkan op), stencil on, mask, func, fail, pass; id 0 = depth test off
    const int idx = ctx->indexed;
    // [eng2] the key names the instance (mesh range x per-object block, inst_id): two objects of one mesh no longer share a key, so a
    // G-buffer override can no longer go to the other one (phone D09: "ambiguous 2" in 4 frames of 40 s, each frame drawn wrong)
    const uint64_t iid = idx ? inst_id(ctx->shader) : 0;
    const uint64_t key = iid ? mix(prepass_key(in->ib, (int32_t)ctx->first, (int32_t)ctx->count, 0, ctx->instances, 0), iid) | 1 : 0;
    const uint64_t vb0 = in->vb[0] * 0x9E3779B97F4A7C15ull ^ in->vb_off[0];
    if (!colors) {
        g.s.pre_seen++;
        int fsc = s && !vkn_fx_shader_is(s) ? fs_class(s) : 3;
        if (fsc < 0) fsc = 3;
        // [mgpu P4] an earlier piece dropped the fragment stage (MAT, order 200: proven without effect - no kill, no depth / mask
        // output): the draw has no fragment stage, class 0, whatever the engine shader's words say (was: 338 of 673 kept as "fs",
        // U13r). DM_VK_NATIVE_PREPASS_NOFS=0: the old rule.
        if (out->no_fs && g.nofs_rule && !out->discard) { if (fsc != 0) g.s.nofs++; fsc = 0; }
        int counting = 0;
        if (pair_open()) { counting = 1; g.s.counting_open++; }
        prepass_pre_in pi; memset(&pi, 0, sizeof pi);
        pi.key = key; pi.vb0 = vb0; pi.verts = (uint64_t)ctx->count * (ctx->instances ? ctx->instances : 1);
        pi.why = prepass_classify(fsc, a[0], a[1], a[3], in->alpha_ref >= 0.0f ? 5 : PREPASS_NVN_ALWAYS_ALPHA, counting, idx, out->discard);
        if (pi.why < 0 && idx && !iid) { pi.why = PREPASS_K_NOID; g.s.noid++; }   // [eng2] no instance identity: kept
        if (pi.why < 0) pi.raster = raster_hash(in, ctx->raster);   // only a candidate's raster state is kept
        if (ctx->frame != g_rec_frame || serial != g_rec_depth) rec_reset(ctx->frame, serial, d);   // [eng3] a new table
        if (pi.why < 0 && g.mode == PREPASS_SKIP && g.repair && !rec_room()) { pi.why = PREPASS_K_FULL; g.s.rec_full++; }   // (no record: never skipped)
        if (prepass_note_pre(g.pp, serial, &pi) && (g.mode == PREPASS_SKIP || g.mode == PREPASS_POISON)) {
            out->skip = 1; g.s.skipped++;
            if (g.mode == PREPASS_SKIP && g.repair) rec_add(key, prepass_key(in->ib, (int32_t)ctx->first, (int32_t)ctx->count, 0, ctx->instances, 0), in, ctx);
        }
        return;
    }
    g.s.gb_seen++;
    prepass_gb_in gi; memset(&gi, 0, sizeof gi);
    gi.key = key; gi.vb0 = vb0;
    if (key && g.pp->f.pre_cand) gi.raster = raster_hash(in, ctx->raster);   // compared only for a candidate key
    uint64_t ph = 0x9E3779B97F4A7C15ull;
    for (uint32_t i = 0; i < VKN_PASS_COLORS; i++) ph = mix(ph, (uint64_t)(uintptr_t)ctx->color[i]);
    gi.pass = ph ? ph : 1;
    gi.dt = a[0]; gi.df = a[0] ? a[2] + 1 : 0; gi.dw = a[1]; gi.st = a[3]; gi.bounds = 0;   // NVN func = Vulkan op + 1 (1 NEVER .. 8 ALWAYS)
    // [VK-ENGINE-SIDE I3] the stencil hazard only when the stencil result depends on the depth test (xbg_prepass.h st_split): a[4] the
    // write (and compare) mask, a[6] / a[7] the depth-fail / depth-pass ops (the fail op of the stencil test itself is KEEP)
    gi.st_split = 1; gi.st_dep = a[3] && a[0] && a[4] && a[6] != a[7];
    if (a[3] && !gi.st_dep) g.s.st_free++;
    // [VK-ENGINE-SIDE I3] "blend" = a real blend, or a draw that may leave a channel of a target unwritten. The backend builds every
    // pipeline with the write mask = all channels of the target's format (vkn_write_mask_nvn; vkn_state.h: the engine's blend carries
    // no mask), so an R11G11B10F / RG16F / R8 target is written whole: it is NOT a partial mask (this check refused every G-buffer draw
    // of DOS2 - 3 of its 4 targets have fewer than 4 channels: phone logs "refused blend=328 ... keys=382 ok=0"). A format the table does
    // not know (mask 0) or a draw whose fragment stage an earlier piece dropped (it writes no colour) still counts as one.
    const VkPipelineColorBlendAttachmentState *ba = ctx->blend ? vkn_blend_att(ctx->blend) : NULL;
    if (out->no_fs) gi.blend = 1;
    for (uint32_t i = 0; i < VKN_PASS_COLORS && !gi.blend; i++) {
        const vkn_tex *c = ctx->color[i]; if (!c) continue;
        if ((ba && ba->blendEnable) || !vkn_write_mask_nvn(c->nvn_fmt)) gi.blend = 1;
    }
    // the engine shader's facts: a swap at this point is MAT's XLQ object, which keeps the engine's fragment module (vkn_mat.h)
    const int fsc = s && !vkn_fx_shader_is(s) ? fs_class(s) : -1;
    gi.kill = fsc < 0 || fsc == 2 || in->alpha_ref >= 0.0f;   // unknown facts: may kill
    // [eng3] twin by mesh: no pre-pass draw has this instance key, but a skipped draw of the same index range is unclaimed - this is its
    // G-buffer draw with other per-object bytes (the block changed between the two passes): it claims for that skipped draw
    if (key && g_nrec && serial == g_rec_depth && !prepass_find(&g.pp->f, key, 0)) {
        const uint64_t mesh = prepass_key(in->ib, (int32_t)ctx->first, (int32_t)ctx->count, 0, ctx->instances, 0);
        uint32_t *m = mesh_slot(mesh, 0);
        for (uint32_t i = 0; m && *m && i < g_nrec; i++) {
            const pp_rec *r = &g_rec[i]; if (r->mesh != mesh || r->done) continue;
            const prepass_slot *sl = prepass_find(&g.pp->f, r->key, 0);
            if (!sl || sl->gb_ok >= sl->cand) continue;   // (its claims are complete)
            gi.key = r->key; gi.raster = raster_hash(in, ctx->raster); g.s.twin_mesh++; (*m)--;
            break;
        }
    }
    if (!prepass_note_gbuffer(g.pp, serial, &gi) || g.mode != PREPASS_SKIP) return;
    const uint16_t v = leq_of(ds);
    if (!v) { prepass_note_lost(g.pp, gi.key); g.s.lost++; return; }
    out->ds = v; g.s.overrides++;
    { vkn_fx_stats fs; vkn_fx_stats_get(&fs); g.pend = 1; g.pend_key = gi.key; g.pend_skipped = fs.skipped; }
}

// ---------------------------------------------------------------- install, census, tests
static int family_tile(uint32_t v, const char *n) {   // xbg_tile_family(xbg_effective_family(...)): Mali / PowerVR, the SIM models included
    const char *sim = getenv("DM_GFX_SIM_GPU");
    if (sim && (strstr(sim, "mali") || strstr(sim, "pvr") || strstr(sim, "powervr"))) return 1;
    if (v == 0x13B5 || v == 0x1010) return 1;
    if (v == 0x5143) return 1;   // [23.11] on again: H02 (Turnip, 140 s walk) and H01 (Mali model) 0 bad keys with the eng3 repair;   // [22.11] back to 0: G03 (Mali model, walking 140 s) had 1 bad key "unmatched 1, unsafe 1"; was Adreno too after the phone (F09 Turnip walking: 0 bad keys, skip 223 of 625, not slower; D09 had 4 before the eng2 key fix)
    return n && (strstr(n, "Mali") || strstr(n, "PowerVR") || strstr(n, "IMG ")) ? 1 : 0;
}
static int reg_fn(void) {
    if (!g.reg) g.reg = !vkn_fx_register("prepass", pp_fx, NULL, 220);   // [fixpp] one function (was + "prepass-late" at 1100)
    return g.reg;
}
static int alloc_ctx(void) {
    if (!g.pp && !(g.pp = (prepass_ctx *)malloc(sizeof *g.pp))) { pred(R_CFG, "no memory for the tables (%zu KB): off", sizeof *g.pp / 1024); return 0; }
    prepass_ctx_init(g.pp, g.hist); g.pp->admit_limit = g.admit; g.pp->ident = 1;   // [eng2] instance keys (inst_id)
    return 1;
}
void vkn_prepass_install(uint32_t vendor, const char *device_name) {
    static int done; if (done) return; done = 1;
    const int def = family_tile(vendor, device_name);
    const char *e = getenv("DM_VK_NATIVE_PREPASS");
    g.mode = e && *e ? prepass_clamp_mode(atol(e)) : def;
    {   // [23.11] I2 (DM_VK_ENG_OCC) takes its depth snapshot with a compute pass at the pre-pass end - a read the repair of skipped
        // draws cannot precede (phone I03 / H10 / H12: "unsafe 32 ... read 2" every other frame). Until I2 orders its snapshot after the
        // repair, the skip is off while I2 is on (I2 is off by default and parked).
        const char *oc = getenv("DM_VK_ENG_OCC");
        if (g.mode == PREPASS_SKIP && oc && *oc && atol(oc) > 0) { vkn_logf("DM_VK_NATIVE_PREPASS=1 with DM_VK_ENG_OCC=%s: the skip is off (I2's mid-frame depth snapshot cannot see repaired draws): 2 (count only)", oc); g.mode = PREPASS_COUNT; }
    }
    const char *h = getenv("DM_VK_NATIVE_PREPASS_HIST"); g.hist = h && *h ? atol(h) != 0 : 1;
    const char *ad = getenv("DM_VK_NATIVE_PREPASS_ADMIT"); g.admit = ad && *ad && atol(ad) >= 0 ? (uint32_t)atol(ad) : 32u;
    const char *l = getenv("DM_VK_NATIVE_PREPASS_LOG"); g.log_every = l && *l && atol(l) > 0 ? (uint64_t)atol(l) : 300u;
    { const char *nf = getenv("DM_VK_NATIVE_PREPASS_NOFS"); g.nofs_rule = nf && *nf ? atol(nf) != 0 : 1; }   // [mgpu P4]
    { const char *rp = getenv("DM_VK_NATIVE_PREPASS_REPAIR"); g.repair = rp && *rp ? atol(rp) != 0 : 1; }   // [eng3]
    vkn_logf("DM_VK_NATIVE_PREPASS=%d (stage 5.2 B4, replaces DM_DOS2_PREPASS; %s; family default %d): %s; history gate %d, new meshes admitted a frame %u (0 = all); "
             "line every %llu frames", g.mode, e && *e ? "env" : "family default", def,
             g.mode == PREPASS_SKIP ? "opaque pre-pass draws skipped, their G-buffer draws LEQUAL + depth write" : g.mode == PREPASS_COUNT ? "compare - decisions and their check, nothing changes" :
             g.mode == PREPASS_POISON ? "POISON - opaque pre-pass draws skipped WITHOUT the G-buffer override (they vanish)" : "off",
             g.hist, g.admit, (unsigned long long)g.log_every);
    if (!g.mode) return;
    if (!alloc_ctx() || !reg_fn()) { pred(R_CFG, "not installed (memory / the draw-effect chain refused the functions): off"); g.mode = 0; return; }
    g.inst = 1;
}
int vkn_prepass_mode(void) { return g.inst ? g.mode : 0; }
void vkn_prepass_stats_get(vkn_prepass_stats *s) { if (s) *s = g.s; }
void vkn_prepass_census(void) { if (g.inst && g.pp && g.pp->last_valid) line_of(&g.pp->last); }
void vkn_prepass_test_setup(int mode, vkn_prepass_test_fn fn) {
    memset(g_fs_s, 0, sizeof g_fs_s);
    g.test = fn; g.mode = prepass_clamp_mode(mode); g.hist = 1; g.admit = 32; g.log_every = 1; g.logged = 0; g.pend = 0; g.nofs_rule = 1; g.repair = 1; rec_reset(0, 0, NULL);
    memset(g.leq, 0, sizeof g.leq);
    g.inst = g.mode && alloc_ctx() && reg_fn();
    if (!g.inst) g.mode = 0;
}
const void *vkn_prepass_test_last(void) { return g.pp && g.pp->last_valid ? &g.pp->last : NULL; }
#endif
