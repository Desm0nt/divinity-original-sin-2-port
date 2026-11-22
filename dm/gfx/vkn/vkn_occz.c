// [DOS2] VK-NATIVE stage 5 technique Z (contract, rule and threads: vkn_occz.h; design: docs/dos2/nr/VK-OCCLUSION-CHEAP.md).
// The glue around the pure model vkn_occz_model.c:
//   gather   (job threads)   the engine object's box projected -> a lock-free table by pair entry;
//   encoder  (engine thread) the vkn_rt.c taps: pairs, their draws' signatures (constants from the ring), the frame globals; at the
//            presented recording's end the model decides and the verdict table of the frame is published;
//   worker   (recording)     the effect function "occ-z" (order 45): a verdict lookup per pair draw / conditional draw (the only work
//            here), and the snapshot: one compute dispatch when the target set leaves the depth pre-pass (vkn_occz_wk_targets).
// No game addresses.
#ifdef DM_GAME_DOS2
#include "vkn_occz.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkn.h"
#include "vkn_buf.h"
#include "vkn_draw.h"
#include "vkn_frame.h"
#include "vkn_fx.h"
#include "vkn_pass.h"
#include "vkn_query.h"
#include "vkn_rt.h"
#include "vkn_shader.h"
#include "vkn_state.h"
#include "vkn_tex.h"
#include "../nr/nr_occ.h"

#define OZ_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
extern int vkn_shlib_on(void) __attribute__((weak));
extern int vkn_shlib_fs_facts(const void *shader) __attribute__((weak));
extern int vkn_shlib_read_mask(const void *shader, uint32_t binding, const uint64_t **bits, uint32_t *nwords) __attribute__((weak));
uint64_t vkn_frame_gpu_done(int *any);   // vkn_frame.c

static const uint32_t k_reduce_spv[] = {
#include "../../android/shaders/vkn_occz_reduce.comp.spv.inc"
};

enum { HALF = 2048, MAPN = 8192, SNAPS = 8, VT = 8, FR = 32, SAMPLE = 16 };
static inline uint64_t mix(uint64_t h, uint64_t v) { return nro_mix(h, v); }

// ================================================================ switches, state
static int g_mode;                        // 0 / 10 count / 11 verify / 12 on / 13 poison
int vkn_occz_enc_on;
static vkn_oz_cfg g_cfg;
static uint64_t g_budget_cap_ns = 2000000, g_draw_ns = 4150;   // DM_VK_NATIVE_OCCZ_BUDGET_US, DM_VK_NATIVE_OCCZ_DRAW_NS
static int g_level = 2, g_over, g_probe;  // 2 on, 0 off by the budget (probe frames left)
static int g_off_violation;               // mode 12: a self-check violation turned the skips off
int vkn_occz_mode(void) { return g_mode; }

// ================================================================ gather (any thread)
typedef struct gat {
    uint64_t eframe;                      // atomic: written last (release)
    uint64_t obj, box_h, cam;
    float ndc[4], zmin;
    uint32_t bad;                         // atomic: a second write in one frame (two stages): unusable
    uint8_t rect, has_vp;
    float vp[16];                         // [v2] the stage camera (the frame's camera for the reprojection)
} gat;
static gat g_g[2][HALF];
static uint64_t g_base[2];
static uint64_t g_gst[6];                 // calls, ok, behind, bad box, key unknown, conflicts
static uint64_t g_gather_ns;              // sampled
void vkn_occz_gather_bases(uint64_t b0, uint64_t b1) { __atomic_store_n(&g_base[0], b0, __ATOMIC_RELAXED); __atomic_store_n(&g_base[1], b1, __ATOMIC_RELAXED); }
static gat *gat_of(uint64_t key) {
    for (int h = 0; h < 2; h++) {
        const uint64_t b = __atomic_load_n(&g_base[h], __ATOMIC_RELAXED);
        if (b && key >= b && key < b + 16u * HALF && !((key - b) & 15)) return &g_g[h][(key - b) >> 4];
    }
    return NULL;
}
void vkn_occz_gather(uint64_t key, uint64_t eframe, uint64_t obj, const float box[6], const float vp[16]) {
    if (!g_mode) return;
    const uint64_t n = __atomic_add_fetch(&g_gst[0], 1, __ATOMIC_RELAXED);
    const uint64_t t0 = (n & (SAMPLE - 1)) == 0 ? vkn_now_ns() : 0;
    gat *g = gat_of(key);
    if (!g) { __atomic_add_fetch(&g_gst[4], 1, __ATOMIC_RELAXED); return; }
    if (__atomic_load_n(&g->eframe, __ATOMIC_ACQUIRE) == eframe) { __atomic_store_n(&g->bad, 1, __ATOMIC_RELEASE); __atomic_add_fetch(&g_gst[5], 1, __ATOMIC_RELAXED); return; }
    float ndc[4] = {0, 0, 0, 0}, z = 0.0f;
    const int r = vp ? vkn_oz_project(box, vp, ndc, &z) : -2;
    g->obj = obj; g->box_h = nro_hash_bytes(0x626f78ull, box, 24); g->cam = vp ? nro_hash_bytes(0x63616dull, vp, 64) | 1 : 0;
    memcpy(g->ndc, ndc, sizeof ndc); g->zmin = z; g->rect = r == 0;
    g->has_vp = vp != NULL; if (vp) memcpy(g->vp, vp, sizeof g->vp);
    __atomic_store_n(&g->bad, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g->eframe, eframe, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_gst[r == 0 ? 1 : r == -1 ? 2 : 3], 1, __ATOMIC_RELAXED);
    if (t0) __atomic_add_fetch(&g_gather_ns, SAMPLE * (vkn_now_ns() - t0), __ATOMIC_RELAXED);
}

// ================================================================ the encoder (engine thread)
typedef struct epair { uint8_t gath; } epair;
static vkn_oz_obj g_o[VKN_OZ_MAXN]; static epair g_ep[VKN_OZ_MAXN]; static uint32_t g_n;
static uint8_t g_ev[VKN_OZ_MAXN];         // the frame's verdicts (after the decision): later conditional draws of hidden pairs are counted
static uint64_t g_ecount_cond;            // conditional draws of pairs decided hidden (the engine counts them: the worker does no work for it)
static struct { uint64_t key; uint32_t gen, i; } g_map[MAPN]; static uint32_t g_gen = 1;
static int g_eopen = -1, g_epres, g_ephase, g_edecided;   // open pair; presented recording; 0 before the pre-pass, 1 in it, 2 after it; decided
static uint64_t g_ecam, g_eframe, g_F, g_glob, g_glob_nobj, g_cond_key; static int g_cond_on, g_emixed, g_eorder;
static vkn_tex *g_eT, *g_eT_prev; static uint32_t g_encol; static vkn_tex *g_edepth;
static struct { uint32_t off, size; uint8_t valid; } g_eu[2][VKN_UBO_SLOTS];
static uint64_t g_tap_ns, g_tap_calls, g_job_ns, g_check_ns;   // engine-side costs (taps sampled)
static uint64_t g_enc_frames, g_enc_late_cam, g_unver, g_ubo_bytes;
// [v2] the frame's camera (the stage camera of its gathered pairs), the per-view constant block, the order state of the pre-pass target
static float g_evp[16]; static int g_evp_ok;
static uint32_t g_camslot = 0xff; static int g_camview;
static int g_ezw;                         // a draw already wrote depth on the pre-pass target in this recording
static uint64_t g_edev;                   // depth events (clears / copies) on the target so far in this recording: part of every later signature
// [v2 diagnosis] why T10 / T11 saw "scene changed" in every pair and "order-sensitive" in every frame (the census line "occ-z why")
static struct {
    uint64_t frames, ord_frames, ord_pair_st, ord_pair_nm, ord_out_st, ord_out_nm, ord_unknown;
    uint64_t out_draws, out_p0, out_changed, out_cam_only, nobox, dev0, dev1, glob_changed;
    uint64_t cam_frames, cam_diff, cam_eye, cam_rot, cam_proj_only, cam_novp; double eye_sum, eye_max, rot_sum, rot_max;
    uint64_t camdep, camblk_ok, camblk_bad, camrows_cut, pv_same, pv_transposed, pv_other;
} g_dg;
static char g_dg_first[240];              // the first order-sensitive draw of the census period
enum { OUTN = 256 };
static uint64_t g_out_sig[OUTN], g_out_full[OUTN], g_out_prev[OUTN], g_out_prev_full[OUTN]; static uint32_t g_out_n, g_out_prev_n;
static uint64_t g_prev_glob; static float g_prev_vp[16]; static int g_prev_vp_ok;
// per-frame caches of handle lookups (the encoder's own; reset by generation)
typedef struct hc { uint64_t h; uint32_t gen; const void *p; uint16_t id; } hc;
static hc g_shc[256], g_dsc[64];
static uint32_t g_cgen = 1;
static struct { uint32_t off, gen; int ok; } g_cbk[16];   // the camera block's consistency per ring offset (this recording)

static uint32_t mslot(uint64_t k) { return (uint32_t)(((k >> 4) * 0x9e3779b97f4a7c15ull) >> 51) & (MAPN - 1); }
static int map_get(uint64_t key) {
    for (uint32_t i = mslot(key), n = 0; n < MAPN; i = (i + 1) & (MAPN - 1), n++) { if (g_map[i].gen != g_gen) return -1; if (g_map[i].key == key) return (int)g_map[i].i; }
    return -1;
}
static void map_put(uint64_t key, uint32_t v) {
    for (uint32_t i = mslot(key), n = 0; n < MAPN; i = (i + 1) & (MAPN - 1), n++)
        if (g_map[i].gen != g_gen || g_map[i].key == key) { g_map[i].gen = g_gen; g_map[i].key = key; g_map[i].i = v; return; }
}
static const vkn_shader *sh_get(uint64_t h) {
    hc *c = &g_shc[(h * 0x9e3779b97f4a7c15ull) >> 56];
    if (c->gen == g_cgen && c->h == h) return (const vkn_shader *)c->p;
    c->gen = g_cgen; c->h = h; c->p = h ? vkn_shader_peek(h) : NULL;   // (never waits: the engine thread)
    return (const vkn_shader *)c->p;
}
static uint16_t ds_get(uint64_t h) {
    hc *c = &g_dsc[(h * 0x9e3779b97f4a7c15ull) >> 58];
    if (c->gen == g_cgen && c->h == h) return c->id;
    c->gen = g_cgen; c->h = h; c->id = h ? vkn_ds_id(h) : 0;
    return c->id;
}
// Depth-state class: 1 = test on, LESS / LEQUAL; 2 = writes depth; 8 = unknown; 0x10 = a stencil test other than ALWAYS; 0x20 = writes
// depth under a compare that can RAISE it (GREATER / NOTEQUAL / GEQUAL / ALWAYS); 0x40 = that compare is ALWAYS (content-free).
// (v1 flagged every stencil test and every non-LESS write, even without a depth write: such a draw cannot change the depth.)
static int ds_cls(uint16_t id) {
    if (!id) return 0;
    uint8_t a[8];
    if (vkn_ds_args(id, a)) return 8;
    int c = 0;
    if (a[0] && (a[2] == 1 || a[2] == 3)) c |= 1;
    if (a[0] && a[1]) c |= 2;
    if (a[3] && a[5] != 7) c |= 0x10;
    if (a[0] && a[1] && a[2] >= 4) c |= 0x20 | (a[2] == 7 ? 0x40 : 0);
    return c;
}
// [v2] 1 = this draw on the pre-pass target makes the target's depth depend on the draw ORDER (no longer the minimum over its draws): it
// writes depth under a stencil test (the stencil comes from earlier draws or frames), or raises the depth after another depth write (an
// ALWAYS write before any other depth write only replaces the cleared / copied content: order-free). Unknown state: order-sensitive.
static int order_of(int c) {
    if (c & 8) return 1;
    if (!(c & 2)) return 0;
    const int r = (c & 0x10) || ((c & 0x20) && (g_ezw || !(c & 0x40)));
    g_ezw = 1;
    return r;
}
static void ord_note(int in_pair, int c, const vkn_draw_in *in, uint16_t ds, uint32_t count) {
    g_eorder = 1;
    if (c & 8) g_dg.ord_unknown++;
    else if (c & 0x10) { if (in_pair) g_dg.ord_pair_st++; else g_dg.ord_out_st++; }
    else { if (in_pair) g_dg.ord_pair_nm++; else g_dg.ord_out_nm++; }
    if (g_dg_first[0]) return;
    uint8_t a[8] = {0, 0, 0, 0, 0, 0, 0, 0}; const int ok = ds ? vkn_ds_args(ds, a) == 0 : 0;
    snprintf(g_dg_first, sizeof g_dg_first, "%s draw, phase %d, depth-stencil %u%s (test %u write %u func %u stencil %u mask %u func %u ops %u/%u), shader %#llx, "
             "%u indices, colours %u, target %ux%u", in_pair ? "pair" : "outside-pair", g_ephase, ds, ok ? "" : " unknown", a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
             (unsigned long long)in->shader, count, g_encol, g_edepth ? g_edepth->w : 0, g_edepth ? g_edepth->h : 0);
}
static int fs_rel(const vkn_shader *s, const vkn_draw_in *in) {
    if (!(s->stages & (1u << VKN_SRC_FS))) return 0;
    if (in->alpha_ref >= 0.0f) return 1;
    const int f = vkn_shlib_on && vkn_shlib_on() && vkn_shlib_fs_facts ? vkn_shlib_fs_facts(s->xs[VKN_SRC_FS]) : -1;
    return f != 0;
}
// [v2] 1 = the per-view block at ring offset `off` is consistent: rows 8..11 (ViewProjection) = rows 0..3 (View) x rows 4..7 (Projection)
// in either order, to 1e-3 of the largest entry (cached per offset for this recording). Also compares it with the gathered camera.
static int cam_block_ok(uint32_t off, uint32_t n, const uint8_t *ring) {
    if (n < 13 * 16) return 0;
    const uint32_t ci = (off >> 4) & 15;
    if (g_cbk[ci].gen == g_cgen && g_cbk[ci].off == off) return g_cbk[ci].ok;
    float m[48]; memcpy(m, ring + off, sizeof m);
    const float *V = m, *P = m + 16, *VP = m + 32;
    float sc = 0.0f; for (int i = 0; i < 16; i++) { const float a = VP[i] < 0 ? -VP[i] : VP[i]; sc = a > sc ? a : sc; }
    int ok = 0;
    if (sc > 0.0f && sc == sc) for (int ord = 0; ord < 2 && !ok; ord++) {
        const float *A = ord ? P : V, *B = ord ? V : P; ok = 1;
        for (int r = 0; r < 4 && ok; r++) for (int c = 0; c < 4; c++) {
            const float v = A[4 * r] * B[c] + A[4 * r + 1] * B[4 + c] + A[4 * r + 2] * B[8 + c] + A[4 * r + 3] * B[12 + c], d = v - VP[4 * r + c];
            if (!((d < 0 ? -d : d) <= 1e-3f * sc)) { ok = 0; break; }
        }
    }
    if (ok) {   // (diagnostic: is the reprojection's camera - the gathered stage camera - the one the depth was drawn with?)
        int same = g_evp_ok, tr = g_evp_ok;
        for (int i = 0; i < 16 && (same || tr); i++) {
            const float a = g_evp[i], b = VP[i], t = VP[4 * (i & 3) + (i >> 2)];
            if (!((a - b < 0 ? b - a : a - b) <= 1e-4f * sc)) same = 0;
            if (!((a - t < 0 ? t - a : a - t) <= 1e-4f * sc)) tr = 0;
        }
        if (same) g_dg.pv_same++; else if (tr) g_dg.pv_transposed++; else if (g_evp_ok) g_dg.pv_other++;
    }
    g_cbk[ci].off = off; g_cbk[ci].gen = g_cgen; g_cbk[ci].ok = ok;
    if (ok) g_dg.camblk_ok++; else g_dg.camblk_bad++;
    return ok;
}
// One uniform block of a draw: *hf = its hash (every byte the shader reads), *hn = the camera-free one (the per-view block's projection
// rows left out when the shader reads no view row and the block is consistent; elsewhere *hn = *hf).
static void ubo_h2(int st, const vkn_bind *b, const vkn_shader *s, int src, uint32_t slot, uint64_t *hn, uint64_t *hf) {
    if (slot >= VKN_UBO_SLOTS || !g_eu[st][slot].valid) { *hn = *hf = mix(0x55420000ull + slot, (uint64_t)st); return; }
    const uint32_t off = g_eu[st][slot].off, size = g_eu[st][slot].size, lim = b->bytes ? b->bytes : 65536u, n = size < lim ? size : lim;
    const uint8_t *ring = (const uint8_t *)vkn_ring_cpu();
    if (!ring || (uint64_t)off + n > VKN_RING_BYTES) { g_unver++; *hn = *hf = mix(0x5ced0000ull, g_F); return; }
    const uint64_t *bits = NULL; uint32_t nw = 0;
    const int mk = vkn_shlib_on && vkn_shlib_on() && vkn_shlib_read_mask ? vkn_shlib_read_mask(s->xs[src], b->binding, &bits, &nw) : -1;
    uint32_t cut = 0;
    if (slot == g_camslot) {
        if (mk == 1 && bits && nw) {
            if (bits[0] & (g_camview ? 0ull : 0x100Full)) g_dg.camdep++;   // reads View / ViewPos: its shape may follow the view
            else if (cam_block_ok(off, n, ring)) cut = g_camview ? 0x1FFFu : 0x0FF0u;
        } else g_dg.camdep++;
    }
    uint64_t f = mix(0x55424f31ull, n), h = f;
    if (mk == 1 && bits) {
        for (uint32_t wi = 0; wi < nw; wi++) for (uint64_t m = bits[wi]; m; m &= m - 1) {
            const uint32_t e = wi * 64u + (uint32_t)__builtin_ctzll(m), at = e * 16u;
            if (at >= n) continue;
            const uint32_t len = n - at < 16 ? n - at : 16;
            const uint64_t rh = nro_hash_bytes(0x524f5700ull + e, ring + off + at, len); g_ubo_bytes += len;
            f = mix(f, rh);
            if (e < 32 && (cut >> e & 1)) g_dg.camrows_cut++; else h = mix(h, rh);
        }
    } else { f = h = nro_hash_bytes(f, ring + off, n); g_ubo_bytes += n; }
    *hn = h; *hf = f;
}
// Everything a draw's depth output depends on (the encoder's view; vkn_occ.c content_of is the worker's twin of A): *nc without the
// camera's projection rows (v2), *full with them.
static void draw_sig(const vkn_draw_in *in, int indexed, uint32_t first, uint32_t count, uint32_t inst, int *dscls, uint16_t *dsid, uint64_t *nc, uint64_t *full) {
    uint32_t ar; memcpy(&ar, &in->alpha_ref, 4);
    const uint16_t ds = ds_get(in->ds);
    *dscls = ds_cls(ds); *dsid = ds;
    uint64_t h = mix(0x4f5a5331ull, in->shader);
    h = mix(h, in->blend ^ in->raster << 1); h = mix(h, in->vfmt); h = mix(h, (uint64_t)ds);
    h = mix(h, (uint64_t)in->prim | (uint64_t)in->stencil_ref << 8 | (uint64_t)indexed << 16 | (uint64_t)ar << 32);
    h = mix(h, (uint64_t)first | (uint64_t)count << 32); h = mix(h, inst);
    h = nro_hash_bytes(h, in->viewport, sizeof in->viewport); h = nro_hash_bytes(h, in->scissor, sizeof in->scissor);
    for (int st = 0; st < 3; st++) {
        if (!in->vb[st]) continue;
        int none = 1; const uint64_t v = vkn_buf_version(VKN_BUF_VB, in->vb[st], &none);
        h = mix(mix(h, in->vb[st]), (uint64_t)in->vb_off[st] | (uint64_t)st << 32); h = mix(h, v);
        if (none) { g_unver++; h = mix(h, 0x5ced0001ull + g_F); }
    }
    if (indexed) { int none = 1; const uint64_t v = vkn_buf_version(VKN_BUF_IB, in->ib, &none); h = mix(mix(h, in->ib), v); if (none) { g_unver++; h = mix(h, 0x5ced0002ull + g_F); } }
    const vkn_shader *s = sh_get(in->shader);
    if (!s) { g_unver++; *nc = *full = mix(h, 0x5ced0003ull + g_F); return; }
    const int fsr = fs_rel(s, in);
    uint64_t hn = h, hf = h;
    for (uint32_t i = 0; i < s->nbind; i++) {
        const vkn_bind *b = &s->bind[i];
        const int st = b->stage == 0 ? 0 : b->stage == 1 ? 1 : -1;
        if (st < 0 || (st == 1 && !fsr)) continue;
        const int src = st == 0 ? VKN_SRC_VS : VKN_SRC_FS;
        for (uint32_t e = 0; e < b->count; e++) {
            const uint32_t slot = (uint32_t)b->slot + e;
            uint64_t v = 0;
            if (b->kind == VKN_BK_UNIFORM) { uint64_t un, uf; ubo_h2(st, b, s, src, slot, &un, &uf); hn = mix(hn, un); hf = mix(hf, uf); continue; }
            else if (b->kind == VKN_BK_TEXTURE) {
                const int32_t id = slot < VKN_TEX_SLOTS && in->tex[st] ? in->tex[st][slot] : -1, sid = slot < VKN_TEX_SLOTS && in->smp[st] ? in->smp[st][slot] : -1;
                v = mix(0x54455800ull, (uint64_t)(uint32_t)id | (uint64_t)(uint32_t)sid << 32);
                if (id >= 0) {
                    vkn_tex *tx = NULL; (void)vkn_tex_view_by_id((uint32_t)id, &tx);
                    if (!tx || tx->pass_ever) { g_unver++; v = mix(v, 0x5ced0004ull + g_F); }
                    else v = mix(mix(v, (uint64_t)(uintptr_t)tx), __atomic_load_n(&tx->shc_up, __ATOMIC_ACQUIRE));
                }
            } else if (b->kind != VKN_BK_SUPPORT && b->kind != VKN_BK_C1) { g_unver++; v = 0x5ced0005ull + g_F; }
            else continue;
            hn = mix(hn, v); hf = mix(hf, v);
        }
    }
    *nc = hn; *full = hf;
}
static void decide(void);
#define TAP_BEGIN const uint64_t tc_ = ++g_tap_calls; const uint64_t tt_ = (tc_ & (SAMPLE - 1)) == 0 ? vkn_now_ns() : 0
#define TAP_END do { if (tt_) g_tap_ns += SAMPLE * (vkn_now_ns() - tt_); } while (0)

void vkn_occz_enc_rec_begin(int presented) {
    if (!vkn_occz_enc_on) return;
    g_epres = presented; g_n = 0; g_eopen = -1; g_ephase = 0; g_edecided = 0; g_ecam = 0; g_emixed = 0; g_eorder = 0; g_eframe = 0;
    g_glob = 0x676c6f62ull; g_glob_nobj = 0; g_cond_on = 0; g_eT_prev = g_eT ? g_eT : g_eT_prev; g_eT = NULL;
    g_evp_ok = 0; g_ezw = 0; g_edev = 0; g_out_n = 0;
    if (++g_gen == 0) { memset(g_map, 0, sizeof g_map); g_gen = 1; }
    if (++g_cgen == 0) { memset(g_shc, 0, sizeof g_shc); memset(g_dsc, 0, sizeof g_dsc); memset(g_cbk, 0, sizeof g_cbk); g_cgen = 1; }
    g_F = vkn_rt_frame();
}
void vkn_occz_enc_targets(uint32_t n, vkn_tex *const *color, vkn_tex *depth) {
    if (!vkn_occz_enc_on || !g_epres) return;
    uint32_t nc = 0;
    for (uint32_t i = 0; color && i < n && i < VKN_PASS_COLORS; i++) nc += color[i] != NULL;
    g_encol = nc; g_edepth = depth;
    if (g_ephase == 1 && (nc || depth != g_eT)) { g_ephase = 2; decide(); }   // the snapshot point (vkn_occz_wk_targets: the same rule):
    // every pair's pre-pass draws are known now, so the frame is decided here - early enough for the worker in most frames
}
void vkn_occz_enc_ubo(int stage, uint32_t slot, uint32_t off, uint32_t size) {
    if (!vkn_occz_enc_on || stage < 0 || stage > 1 || slot >= VKN_UBO_SLOTS) return;
    g_eu[stage][slot].off = off; g_eu[stage][slot].size = size; g_eu[stage][slot].valid = 1;
}
void vkn_occz_enc_qbegin(uint64_t key) {
    if (!vkn_occz_enc_on || !g_epres || !key) return;
    TAP_BEGIN;
    if (g_eopen >= 0) g_o[g_eopen].clean = 0;   // nested: unusable
    g_eopen = -1;
    if (g_n >= VKN_OZ_MAXN || map_get(key) >= 0) { if (map_get(key) >= 0) g_o[map_get(key)].clean = 0; TAP_END; return; }
    const uint32_t i = g_n++;
    vkn_oz_obj *o = &g_o[i]; memset(o, 0, sizeof *o); g_ep[i].gath = 0;
    o->key = key; o->clean = 1; o->sig = 0x7369676eull; o->sig_full = o->sig;
    gat *g = gat_of(key);
    if (g) {
        const uint64_t e1 = __atomic_load_n(&g->eframe, __ATOMIC_ACQUIRE);
        if (!g_eframe && e1) g_eframe = e1;   // the engine frame of this recording's gather: the first pair names it
        if (e1 && e1 == g_eframe && !__atomic_load_n(&g->bad, __ATOMIC_ACQUIRE)) {
            vkn_oz_obj t; t.obj = g->obj; t.box_h = g->box_h; memcpy(t.ndc, g->ndc, sizeof t.ndc); t.zmin = g->zmin; t.rect = g->rect;
            float vp[16]; const int hv = g->has_vp; if (hv) memcpy(vp, g->vp, sizeof vp);
            const uint64_t cam = g->cam;
            if (__atomic_load_n(&g->eframe, __ATOMIC_ACQUIRE) == e1 && !__atomic_load_n(&g->bad, __ATOMIC_ACQUIRE)) {
                o->obj = t.obj; o->box_h = t.box_h; memcpy(o->ndc, t.ndc, sizeof o->ndc); o->zmin = t.zmin; o->rect = t.rect; g_ep[i].gath = 1;
                if (cam) {
                    if (!g_ecam) { g_ecam = cam; if (hv) { memcpy(g_evp, vp, sizeof g_evp); g_evp_ok = 1; } }
                    else if (g_ecam != cam) g_emixed = 1;
                }
            }
        }
    }
    map_put(key, i);
    g_eopen = (int)i;
    TAP_END;
}
void vkn_occz_enc_qend(uint64_t key) {
    if (!vkn_occz_enc_on || !g_epres) return;
    if (g_eopen >= 0 && g_o[g_eopen].key == key) g_eopen = -1;
}
void vkn_occz_enc_cond(uint64_t key) { if (vkn_occz_enc_on && g_epres) { g_cond_key = key; g_cond_on = key != 0; } }
void vkn_occz_enc_depth_event(vkn_tex *t) {
    if (!vkn_occz_enc_on || !g_epres) return;
    if (!t || t == g_eT || t == g_eT_prev) {
        g_glob = mix(g_glob, 0x64657674ull + (uint64_t)(uintptr_t)t + ((uint64_t)g_ephase << 60));
        if (g_ephase < 2) { g_edev++; if (g_ephase) g_dg.dev1++; else g_dg.dev0++; }
    }
}
void vkn_occz_enc_draw(const vkn_draw_in *in, int indexed, uint32_t first, uint32_t count, uint32_t inst) {
    if (!vkn_occz_enc_on || !g_epres || !in) return;
    TAP_BEGIN;
    if (g_cond_on) { g_cond_on = 0; const int p = map_get(g_cond_key); if (p >= 0) { g_o[p].ncond++; if (g_edecided && g_ev[p] == VKN_OZ_HIDDEN) g_ecount_cond++; } }
    if (g_eopen >= 0) {
        vkn_oz_obj *o = &g_o[g_eopen];
        int on = 0;
        if (!g_encol && g_edepth && g_ephase < 2 && (!g_eT || g_eT == g_edepth)) { g_eT = g_edepth; g_ephase = 1; on = 1; } else o->clean = 0;
        int c = 0; uint16_t ds = 0; uint64_t nc = 0, fu = 0;
        draw_sig(in, indexed, first, count, inst, &c, &ds, &nc, &fu);
        o->sig = mix(mix(o->sig, nc), g_edev); o->sig_full = mix(mix(o->sig_full, fu), g_edev);   // (its place among the target's clears / copies too)
        o->ndraw++;
        if (on && order_of(c)) ord_note(1, c, in, ds, count);
        if ((c & 3) != 3 || (c & 8)) o->clean = 0;   // a candidate's pre-pass draws: test LESS / LEQUAL with writes
        if (g_eT && (in->viewport[0] != 0.0f || in->viewport[1] != 0.0f || in->viewport[2] != (float)g_eT->w || in->viewport[3] != (float)g_eT->h)) o->clean = 0;
    } else if (g_ephase < 2 && g_edepth && (g_edepth == g_eT || (!g_eT && g_edepth == g_eT_prev))) {   // a draw outside pairs on the pre-pass depth
        if (ds_cls(ds_get(in->ds)) & (2 | 8)) {   // (only a draw that can write depth can change the snapshot)
            int c = 0; uint16_t ds = 0; uint64_t nc = 0, fu = 0;
            draw_sig(in, indexed, first, count, inst, &c, &ds, &nc, &fu);
            g_glob = mix(mix(g_glob, nc), g_edev);
            if (order_of(c)) ord_note(0, c, in, ds, count);
            g_dg.out_draws++; g_dg.out_p0 += (uint64_t)(g_eT == NULL);
            if (g_out_n < OUTN) { g_out_sig[g_out_n] = nc; g_out_full[g_out_n] = fu; }
            g_out_n++;
        }
    }
    TAP_END;
}

// ---- snapshots (written by the worker, read by the engine) ----
typedef struct snap {
    uint32_t state;                       // atomic: 0 free, 1 recorded (meta valid), 2 pinned by the engine
    uint64_t frame, tex; uint32_t w, h, gw, gh;
    uint8_t checked;                      // the engine's: the self-check ran
    VkDescriptorSet set;
} snap;
static snap g_sn[SNAPS];
// SNAPS slots of 2 planes of VKN_OZ_GW x VKN_OZ_GH floats: the cells' MAX depth, then [v2] their MIN depth
enum { PLANE = VKN_OZ_GW * VKN_OZ_GH };
static vkn_block g_sbuf; static float *g_scpu;
static inline float *snap_max(int i) { return g_scpu + (size_t)i * 2u * PLANE; }
static inline float *snap_min(int i) { return g_scpu + (size_t)i * 2u * PLANE + PLANE; }
static struct { uint64_t frame, cam, tex; uint32_t w, h; float vp[16]; uint8_t vp_ok; } g_fr[FR];   // the engine's camera / target per decided frame
// ---- verdict tables (engine -> worker) ----
typedef struct vtab { uint64_t frame; uint32_t gen; uint64_t key[MAPN]; uint8_t v[MAPN]; uint32_t g[MAPN]; } vtab;
static vtab g_vt[VT];
static uint64_t g_slack_last, g_bp_last; static double g_slack_ema, g_own_ema, g_tap_ema; static uint64_t g_tap_last, g_job_last;
static uint64_t g_red_n;
static void red_fn(uint64_t key, float zmin, float cell, uint32_t cx, uint32_t cy, uint64_t frame) {
    if (++g_red_n <= 16) vkn_logf(OZ_RED "occ-z: VIOLATION frame %llu pair %#llx: box nearest %.7f, cell (%u, %u) of the frame's own pre-pass depth %.7f",
                                  (unsigned long long)frame, (unsigned long long)key, (double)zmin, cx, cy, (double)cell);
}
static void vt_put(vtab *t, uint64_t key, uint8_t v) {
    for (uint32_t i = mslot(key), n = 0; n < MAPN; i = (i + 1) & (MAPN - 1), n++)
        if (t->g[i] != t->gen || t->key[i] == key) { t->g[i] = t->gen; t->key[i] = key; t->v[i] = v; return; }
}
static uint8_t vt_get(const vtab *t, uint64_t key) {
    for (uint32_t i = mslot(key), n = 0; n < MAPN; i = (i + 1) & (MAPN - 1), n++) { if (t->g[i] != t->gen) return 0; if (t->key[i] == key) return t->v[i]; }
    return 0;
}
static void budget_tick(uint64_t own_ns) {
    vkn_rt_stats rs; vkn_rt_stats_get(&rs);
    const uint64_t bp = rs.bp_wait_ns, fr = rs.frames;
    const uint64_t df = fr > g_slack_last ? fr - g_slack_last : 1;
    const double slack = (double)(bp - g_bp_last) / (double)df;
    g_bp_last = bp; g_slack_last = fr;
    g_slack_ema += (slack - g_slack_ema) / 16.0;
    g_own_ema += ((double)own_ns - g_own_ema) / 16.0;
    const double avail = g_slack_ema + g_own_ema, b = 0.6 * avail;
    g_cfg.budget_ns = (uint64_t)(b < (double)g_budget_cap_ns ? (b > 0 ? b : 0) : (double)g_budget_cap_ns);
    if (!vkn_rt_threaded()) g_cfg.budget_ns = g_budget_cap_ns;   // (no worker: no slack to measure; its verdicts come late anyway)
}
void vkn_occz_enc_rec_end(int present) {
    if (!g_mode) return;
    if (!vkn_occz_enc_on) {   // level 0: probe again now and then
        if (g_level == 0 && ++g_probe >= 512) { g_probe = 0; g_level = 1; vkn_occz_enc_on = 1; g_over = 0; }
        return;
    }
    if (g_epres && present && !g_edecided) decide();   // (a frame without a pre-pass end seen)
}
// [v2 diagnosis] per decided frame: what changed against the previous decided frame (globals, the draws outside pairs one by one, the
// camera: the eye's move, the view's turn, or the projection alone), and the pairs without a box
static void diag_frame(const vkn_oz_frame *fr) {
    g_dg.frames++; g_dg.ord_frames += (uint64_t)(fr->order_sensitive != 0);
    for (uint32_t i = 0; i < g_n; i++) g_dg.nobox += (uint64_t)!g_ep[i].gath;
    if (g_dg.frames > 1 && fr->global != g_prev_glob) g_dg.glob_changed++;
    g_prev_glob = fr->global;
    const uint32_t n = g_out_n < OUTN ? g_out_n : OUTN, pn = g_out_prev_n < OUTN ? g_out_prev_n : OUTN;
    for (uint32_t k = 0; k < n && k < pn; k++) {
        if (g_out_sig[k] != g_out_prev[k]) g_dg.out_changed++;
        else if (g_out_full[k] != g_out_prev_full[k]) g_dg.out_cam_only++;
    }
    g_dg.out_changed += (uint64_t)(n > pn ? n - pn : pn - n);
    memcpy(g_out_prev, g_out_sig, n * sizeof g_out_prev[0]); memcpy(g_out_prev_full, g_out_full, n * sizeof g_out_prev_full[0]); g_out_prev_n = g_out_n;
    if (!fr->vp) { g_dg.cam_novp++; g_prev_vp_ok = 0; return; }
    if (g_prev_vp_ok) {
        g_dg.cam_frames++;
        if (memcmp(g_prev_vp, fr->vp, sizeof g_prev_vp)) {
            g_dg.cam_diff++;
            double e0[3], e1[3]; const int ok = !vkn_oz_eye(g_prev_vp, e0) && !vkn_oz_eye(fr->vp, e1);
            const double d = ok ? sqrt((e1[0] - e0[0]) * (e1[0] - e0[0]) + (e1[1] - e0[1]) * (e1[1] - e0[1]) + (e1[2] - e0[2]) * (e1[2] - e0[2])) : 0.0;
            double ang = 0.0;   // the turn of the view axis (clip w's direction) and of the screen x axis
            for (int j = 0; j < 4; j += 3) {
                const double a[3] = {g_prev_vp[j], g_prev_vp[4 + j], g_prev_vp[8 + j]}, b[3] = {fr->vp[j], fr->vp[4 + j], fr->vp[8 + j]};
                const double na = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), nb = sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
                if (na > 0 && nb > 0) { double c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (na * nb); c = c > 1 ? 1 : c < -1 ? -1 : c; const double x = acos(c) * 57.29577951308232; ang = x > ang ? x : ang; }
            }
            if (d > 1e-4) g_dg.cam_eye++; else if (ang > 1e-3) g_dg.cam_rot++; else g_dg.cam_proj_only++;
            g_dg.eye_sum += d; g_dg.eye_max = d > g_dg.eye_max ? d : g_dg.eye_max; g_dg.rot_sum += ang; g_dg.rot_max = ang > g_dg.rot_max ? ang : g_dg.rot_max;
        }
    }
    memcpy(g_prev_vp, fr->vp, sizeof g_prev_vp); g_prev_vp_ok = 1;
}
static void decide(void) {
    g_edecided = 1;
    const uint64_t t0 = vkn_now_ns(), F = g_F;
    g_enc_frames++;
    // the frame's globals and identity
    vkn_oz_frame fr; memset(&fr, 0, sizeof fr);
    fr.frame = F; fr.cam = g_emixed ? 0 : g_ecam; fr.tex = (uint64_t)(uintptr_t)g_eT; fr.w = g_eT ? g_eT->w : 0; fr.h = g_eT ? g_eT->h : 0;
    for (uint32_t i = 0; i < g_n; i++) {
        if (!g_o[i].ndraw) g_o[i].clean = 0;   // (an empty pair: exact skips of its conditional draws are the worker's, as COND=1)
        if (!g_ep[i].gath) g_glob_nobj += mix(g_o[i].sig, 0x6e6f6f62ull);   // a pair without a box: in the globals, order-free
    }
    fr.global = mix(g_glob, g_glob_nobj); fr.order_sensitive = g_eorder; fr.o = g_o; fr.n = g_n;
    fr.vp = g_evp_ok && !g_emixed ? g_evp : NULL;
    if (g_emixed) g_enc_late_cam++;
    { const uint32_t i = (uint32_t)(F % FR); g_fr[i].frame = F; g_fr[i].cam = fr.cam; g_fr[i].tex = fr.tex; g_fr[i].w = fr.w; g_fr[i].h = fr.h;
      g_fr[i].vp_ok = fr.vp != NULL; if (fr.vp) memcpy(g_fr[i].vp, fr.vp, sizeof g_fr[i].vp); }
    diag_frame(&fr);
    // newly finished snapshots: self-check, then the newest one is used
    int any = 0; const uint64_t done = vkn_frame_gpu_done(&any);
    int best = -1; uint64_t tc = vkn_now_ns();
    for (int i = 0; i < SNAPS; i++) {
        snap *s = &g_sn[i];
        uint32_t one = 1;
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != 1 || !any || s->frame > done) continue;
        if (!__atomic_compare_exchange_n(&s->state, &one, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;   // pinned: the worker cannot take it now
        const uint32_t fi = (uint32_t)(s->frame % FR);
        if (!s->checked) {
            s->checked = 1;
            if (g_fr[fi].frame == s->frame) {
                const vkn_oz_snap v = {s->frame, g_fr[fi].cam, s->tex, s->w, s->h, s->gw, s->gh, snap_max(i), NULL, NULL};
                if (vkn_ozm_check(&v, red_fn) && g_mode == 12 && !g_off_violation) {
                    g_off_violation = 1; vkn_logf(OZ_RED "occ-z: a skip was wrong (self-check): DM_VK_NATIVE_COND=12 skips nothing more this session");
                }
            }
        }
        if (g_fr[fi].frame == s->frame && (best < 0 || s->frame > g_sn[best].frame)) { if (best >= 0) __atomic_store_n(&g_sn[best].state, 1, __ATOMIC_RELEASE); best = i; }
        else __atomic_store_n(&s->state, 1, __ATOMIC_RELEASE);
    }
    g_check_ns += vkn_now_ns() - tc;
    vkn_oz_snap sv; const vkn_oz_snap *sp = NULL;
    if (best >= 0) {   // (pinned until the model has read it)
        snap *s = &g_sn[best]; const uint32_t fi = (uint32_t)(s->frame % FR);
        sv.frame = s->frame; sv.cam = g_fr[fi].cam; sv.tex = s->tex; sv.w = s->w; sv.h = s->h; sv.gw = s->gw; sv.gh = s->gh;
        sv.cell = snap_max(best); sv.cmin = snap_min(best); sv.vp = g_fr[fi].vp_ok ? g_fr[fi].vp : NULL; sp = &sv;
    }
    // decide
    vtab *t = &g_vt[F % VT];
    __atomic_store_n(&t->frame, 0, __ATOMIC_RELEASE);
    if (++t->gen == 0) { memset(t->g, 0, sizeof t->g); t->gen = 1; }
    static uint8_t verdict[VKN_OZ_MAXN];
    const uint64_t tap_now = g_tap_ns - g_tap_last; g_tap_last = g_tap_ns;
    budget_tick(tap_now + (g_job_ns - g_job_last)); g_job_last = g_job_ns;
    g_tap_ema += ((double)tap_now - g_tap_ema) / 16.0;
    if (g_level >= 1 && g_enc_frames > 64 && g_tap_ema > (double)g_cfg.budget_ns) { if (++g_over >= 32) { g_level = 0; vkn_occz_enc_on = 0; g_probe = 0; vkn_logf("occ-z: the encoder taps cost %.0f us a frame, over the budget %.0f us (engine slack %.0f us): off, probe in 512 frames", g_tap_ema / 1e3, (double)g_cfg.budget_ns / 1e3, g_slack_ema / 1e3); } }
    else g_over = 0;
    if (g_level == 1 && g_over == 0) g_level = 2;
    vkn_ozm_cfg(&g_cfg);
    (void)vkn_ozm_frame(&fr, sp, verdict, vkn_now_ns);
    memcpy(g_ev, verdict, g_n);
    if (sp) __atomic_store_n(&g_sn[best].state, 1, __ATOMIC_RELEASE);
    const uint8_t want = g_mode == 13 ? VKN_OZ_POISON : VKN_OZ_HIDDEN;
    for (uint32_t i = 0; i < g_n; i++) if (verdict[i] == want) vt_put(t, g_o[i].key, 1);
    __atomic_store_n(&t->frame, F, __ATOMIC_RELEASE);
    g_job_ns += vkn_now_ns() - t0;
}

// ================================================================ the worker (recording thread)
static vtab *g_wt;                        // this recording's verdicts (NULL = none / late)
static vkn_tex *g_wT; static int g_wphase, g_wsnap; static uint64_t g_wframe;
static struct { uint16_t id; uint8_t c, set; } g_wds[256];
static uint8_t g_wv[VKN_Q_RECS]; static uint32_t g_wvg[VKN_Q_RECS], g_wgen = 1;
static struct { uint64_t recs, late, skip_pre, skip_cond, skip_empty, refuse_pre, refuse_cond, fx_ns, fx_calls, snaps, snap_fail, snap_ns, qdis, qchk; } g_w;
static uint8_t g_qz[VKN_Q_SLOTS][VKN_Q_CAP];   // mode 11: Z called query q of slot s hidden
// GPU objects
static int g_gpu_st;                      // 0 untried, 1 ok, -1 failed
static VkSampler g_smp; static VkDescriptorSetLayout g_dsl; static VkPipelineLayout g_pl; static VkPipeline g_pipe; static VkDescriptorPool g_pool; static VkShaderModule g_mod;

static int gpu_setup(void) {
    if (g_gpu_st) return g_gpu_st;
    g_gpu_st = -1;
    const vkn_dev *d = vkn_device();
    if (!d) { g_gpu_st = 0; return -1; }
    const VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.0f};
    const VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                               {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    const VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
    const VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    const VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof k_reduce_spv, .pCode = k_reduce_spv};
    if (vkCreateSampler(d->dev, &si, NULL, &g_smp) != VK_SUCCESS || vkCreateDescriptorSetLayout(d->dev, &li, NULL, &g_dsl) != VK_SUCCESS) goto fail;
    { const VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
      if (vkCreatePipelineLayout(d->dev, &pi, NULL, &g_pl) != VK_SUCCESS) goto fail; }
    if (vkCreateShaderModule(d->dev, &mi, NULL, &g_mod) != VK_SUCCESS) goto fail;
    { const VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = g_mod, .pName = "main"}, .layout = g_pl};
      if (vkCreateComputePipelines(d->dev, VK_NULL_HANDLE, 1, &ci, NULL, &g_pipe) != VK_SUCCESS) goto fail; }
    { const VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SNAPS}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SNAPS}};
      const VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = SNAPS, .poolSizeCount = 2, .pPoolSizes = ps};
      if (vkCreateDescriptorPool(d->dev, &pi, NULL, &g_pool) != VK_SUCCESS) goto fail; }
    for (int i = 0; i < SNAPS; i++) {
        const VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_pool, .descriptorSetCount = 1, .pSetLayouts = &g_dsl};
        if (vkAllocateDescriptorSets(d->dev, &ai, &g_sn[i].set) != VK_SUCCESS) goto fail;
    }
    if (vkn_block_alloc(VKN_ARENA_STATIC, (uint64_t)SNAPS * 2u * PLANE * 4u, 256, &g_sbuf) || !g_sbuf.cpu) goto fail;
    g_scpu = (float *)g_sbuf.cpu;
    g_gpu_st = 1;
    vkn_logf("occ-z: snapshot pipeline ready (%d slots of %dx%d cells of %d px, max + min depth)", SNAPS, VKN_OZ_GW, VKN_OZ_GH, VKN_OZ_CELL);
    return 1;
fail:
    vkn_logf(OZ_RED "occ-z: the snapshot pipeline could not be created: technique Z decides nothing (everything drawn)");
    return -1;
}
// The worker side of vkn_rt_pass_targets (vkn_rt.c: before vkn_pass_targets, in every DM_VK_NATIVE_RT mode).
void vkn_eocc_wk_targets(uint32_t n, vkn_tex *const *color, vkn_tex *depth) __attribute__((weak));   // [VK-ENGINE-SIDE I2] vkn_eocc.h
void vkn_occz_wk_targets(uint32_t n, vkn_tex *const *color, vkn_tex *depth) {
    if (vkn_eocc_wk_targets) vkn_eocc_wk_targets(n, color, depth);   // [VK-ENGINE-SIDE I2] its snapshot point (off: returns at once)
    if (!g_mode || g_wphase != 1 || g_wsnap || !vkn_rec_presented()) return;
    uint32_t nc = 0;
    for (uint32_t i = 0; color && i < n && i < VKN_PASS_COLORS; i++) nc += color[i] != NULL;
    if (!nc && depth == g_wT) return;
    g_wphase = 2; g_wsnap = 1;
    const uint64_t t0 = vkn_now_ns();
    vkn_tex *T = g_wT;
    if (gpu_setup() != 1 || !T || !T->view || !T->w || !T->h) { g_w.snap_fail++; return; }
    const uint32_t gw = (T->w + VKN_OZ_CELL - 1) / VKN_OZ_CELL, gh = (T->h + VKN_OZ_CELL - 1) / VKN_OZ_CELL;
    if (gw > VKN_OZ_GW || gh > VKN_OZ_GH) { g_w.snap_fail++; return; }
    // a slot: free, or recorded and retired (3 frames: its GPU write is long done) and not pinned by the engine; the oldest first
    int si = -1; const uint64_t f = vkn_frame();
    for (int i = 0; i < SNAPS; i++) {
        const uint32_t st = __atomic_load_n(&g_sn[i].state, __ATOMIC_ACQUIRE);
        if (st == 2) continue;
        if (st == 1 && g_sn[i].frame + 3 > f) continue;
        if (si < 0 || st == 0 || (__atomic_load_n(&g_sn[si].state, __ATOMIC_ACQUIRE) == 1 && g_sn[i].frame < g_sn[si].frame)) si = i;
        if (st == 0) break;
    }
    if (si < 0) { g_w.snap_fail++; return; }
    snap *s = &g_sn[si];
    uint32_t exp = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
    if (exp == 2 || !__atomic_compare_exchange_n(&s->state, &exp, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { g_w.snap_fail++; return; }
    VkCommandBuffer cb = vkn_cb();
    if (!cb) { g_w.snap_fail++; return; }
    vkn_pass_layout(T, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);   // (ends the pre-pass; the next pass brings it back as an attachment)
    const VkDescriptorImageInfo ii = {g_smp, T->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDeviceSize sb = (VkDeviceSize)2u * PLANE * 4u;
    const VkDescriptorBufferInfo bi = {g_sbuf.buf, g_sbuf.off + (VkDeviceSize)si * sb, sb};
    const VkWriteDescriptorSet w[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
                                       {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s->set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi}};
    vkUpdateDescriptorSets(vkn_device()->dev, 2, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_pl, 0, 1, &s->set, 0, NULL);
    const int32_t pc[4] = {(int32_t)T->w, (int32_t)T->h, (int32_t)gw, (int32_t)PLANE};   // (the min plane follows the max one)
    vkCmdPushConstants(cb, g_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, gw, gh, 1);
    const VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    // (compute bindings are their own bind point: the draw path's graphics bindings stay as they were)
    s->frame = f; s->tex = (uint64_t)(uintptr_t)T; s->w = T->w; s->h = T->h; s->gw = gw; s->gh = gh; s->checked = 0;
    __atomic_store_n(&s->state, 1, __ATOMIC_RELEASE);
    g_w.snaps++; g_w.snap_ns += vkn_now_ns() - t0;
}
static int g_wpres;
void vkn_occz_q_rec_begin(void) {   // (from vkn_query.c's recording-begin listener: the frame module's 8 listener slots are taken)
    g_wt = NULL; g_wT = NULL; g_wphase = 0; g_wsnap = 0; g_wpres = 0;
    if (++g_wgen == 0) { memset(g_wvg, 0, sizeof g_wvg); g_wgen = 1; }
    if (!g_mode || !vkn_rec_presented()) return;
    g_wframe = vkn_frame(); g_wpres = 1; g_w.recs++;
}
// The frame's table, looked up lazily (the engine publishes it when its encoder passed the pre-pass: often while the worker is in
// this very recording; every decision is exact on its own, so a table that comes in the middle is used from then on).
static inline int wtab(void) {
    if (g_wt) return 1;
    if (!g_wpres) return 0;
    vtab *t = &g_vt[g_wframe % VT];
    if (__atomic_load_n(&t->frame, __ATOMIC_ACQUIRE) != g_wframe) return 0;
    g_wt = t; return 1;
}
static uint8_t wds(uint16_t id) {   // 1 = depth-tested LESS / LEQUAL / EQUAL; 2 = a depth fail writes stencil
    if (!id) return 0;
    uint8_t *e = &g_wds[id & 255].c;
    if (g_wds[id & 255].set && g_wds[id & 255].id == id) return *e;
    uint8_t a[8], c = 0;
    if (!vkn_ds_args(id, a)) { if (a[0] && (a[2] == 1 || a[2] == 2 || a[2] == 3)) c |= 1; if (a[3] && a[6] != 0) c |= 2; } else c = 2;
    g_wds[id & 255].id = id; g_wds[id & 255].set = 1; *e = c;
    return c;
}
static uint8_t wverdict(int rec) {
    if (rec < 0 || rec >= VKN_Q_RECS || !wtab()) { g_w.late++; return 0; }
    if (g_wvg[rec] == g_wgen) return g_wv[rec];
    g_wvg[rec] = g_wgen;
    return g_wv[rec] = vt_get(g_wt, vkn_query_occ_key((uint32_t)rec));
}
static void occz_fx(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out, void *user) {
    (void)in; (void)user;
    const int open = vkn_query_occ_open(), cond = vkn_query_occ_cond();
    if (open < 0 && cond < 0) return;
    const uint64_t c = ++g_w.fx_calls, t0 = (c & (SAMPLE - 1)) == 0 ? vkn_now_ns() : 0;
    if (open >= 0 && g_wphase < 2 && ctx->depth) {   // the pre-pass target (the snapshot's)
        int nc = 0; for (int i = 0; i < VKN_PASS_COLORS; i++) nc += ctx->color[i] != NULL;
        if (!nc && (!g_wT || g_wT == ctx->depth)) { g_wT = ctx->depth; g_wphase = 1; }
    }
    const int real = (g_mode == 12 && !g_off_violation) || g_mode == 13;
    if (real) {
        const uint16_t ds = out->ds ? out->ds : ctx->ds;
        if (open >= 0 && wverdict(open)) {
            if (!(wds(ds) & 2) && ctx->depth == g_wT && !out->shader) { out->skip = 1; vkn_query_occ_skipped(); g_w.skip_pre++; } else g_w.refuse_pre++;
        } else if (cond >= 0) {
            uint32_t seen = 0, done = 0; int closed = 0, empty = 0;
            if (g_mode == 12 && !vkn_query_occ_rec((uint32_t)cond, &seen, &done, &closed, &empty) && empty && closed) { out->skip = 1; g_w.skip_empty++; }
            else if (wverdict(cond)) {
                if ((wds(ds) & 3) == 1 && ctx->depth == g_wT && g_wT) { out->skip = 1; g_w.skip_cond++; } else g_w.refuse_cond++;
            }
        }
    }
    if (t0) g_w.fx_ns += SAMPLE * (vkn_now_ns() - t0);
}
// mode 11: the GPU's own verdict of the pairs Z called hidden (not a violation by itself: an object drawn before its occluders has
// samples and is still hidden in the final depth; the self-check is the exact test)
void vkn_occz_q_copied(int slot, uint32_t q, uint64_t key, int whole) {
    if (g_mode != 11 || slot < 0 || slot >= VKN_Q_SLOTS || q >= VKN_Q_CAP) return;
    g_qz[slot][q] = (uint8_t)(whole && wtab() && vt_get(g_wt, key));
}
void vkn_occz_q_result(int slot, uint32_t q, uint32_t samples) {
    if (g_mode != 11 || slot < 0 || slot >= VKN_Q_SLOTS || q >= VKN_Q_CAP || !g_qz[slot][q]) return;
    g_qz[slot][q] = 0; g_w.qchk++;
    if (samples) g_w.qdis++;
}

// ---- host tests ----
void vkn_occz_test_fill(float v) { if (g_scpu) for (size_t i = 0; i < (size_t)SNAPS * 2u * PLANE; i++) g_scpu[i] = v; }
int vkn_occz_test_off(void) { return g_off_violation; }
void vkn_occz_test_reset(void) {
    g_off_violation = 0; g_level = 2; g_over = 0; g_probe = 0; g_red_n = 0;
    for (int i = 0; i < SNAPS; i++) { g_sn[i].state = 0; g_sn[i].frame = 0; g_sn[i].checked = 0; }
    for (int i = 0; i < VT; i++) g_vt[i].frame = 0;
    memset(g_fr, 0, sizeof g_fr); memset(&g_w, 0, sizeof g_w); vkn_ozm_reset();
}

// ================================================================ install, census
void vkn_occz_cam_block(uint32_t slot, int view_too) {
    g_camslot = slot < VKN_UBO_SLOTS ? slot : 0xff;
    const char *e = getenv("DM_VK_NATIVE_OCCZ_CAMVIEW");
    g_camview = e && *e ? atoi(e) != 0 : view_too != 0;
}
void vkn_occz_init(int mode) {
    g_mode = mode >= 10 && mode <= 13 ? mode : 0;
    vkn_occz_enc_on = g_mode != 0;
    const char *e;
    g_cfg.eps = (e = getenv("DM_VK_NATIVE_OCCZ_EPS")) && *e ? (float)atof(e) : 1e-5f;
    g_cfg.pad_px = (e = getenv("DM_VK_NATIVE_OCCZ_PAD")) && *e ? (uint32_t)atoi(e) : 2;
    g_budget_cap_ns = ((e = getenv("DM_VK_NATIVE_OCCZ_BUDGET_US")) && *e ? (uint64_t)atoll(e) : 2000) * 1000;
    g_draw_ns = (e = getenv("DM_VK_NATIVE_OCCZ_DRAW_NS")) && *e ? (uint64_t)atoll(e) : 4150;
    g_cfg.sheet = (e = getenv("DM_VK_NATIVE_OCCZ_SHEET")) && *e ? (float)atof(e) : 0.05f;   // [v2] 0 = the snapshot's own camera only (v1)
    if ((e = getenv("DM_VK_NATIVE_OCCZ_CAMVIEW")) && *e) g_camview = atoi(e) != 0;
    vkn_ozm_cfg(&g_cfg);
    if (!g_mode) return;
    static int reg;
    if (!reg) { reg = 1; (void)vkn_fx_register("occ-z", occz_fx, NULL, 45); }
    vkn_logf("occ-z: DM_VK_NATIVE_COND=%d (%s): depth snapshot of the pre-pass (cells of %d px) + engine boxes, decided on the engine thread; eps %g, pad %u px, "
             "budget cap %llu us (0.6 x the engine slack), saved recording cost %llu ns a skipped draw; v2 reprojection %s (sheet %g), camera block %s%s", g_mode,
             g_mode == 10 ? "count only" : g_mode == 11 ? "verify: queries on every pair + self-check, nothing skipped" : g_mode == 12 ? "on" : "POISON: visible trusted objects skipped",
             VKN_OZ_CELL, (double)g_cfg.eps, g_cfg.pad_px, (unsigned long long)(g_budget_cap_ns / 1000), (unsigned long long)g_draw_ns,
             g_cfg.sheet > 0.0f ? "on" : "OFF", (double)g_cfg.sheet, g_camslot < VKN_UBO_SLOTS ? "set" : "none", g_camview ? " (view rows pure too)" : "");
}
// The census appends sections to one line and starts a continuation line before vkn_logf's 1024-byte limit would cut it.
static void cz_put(char *o, int *n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void cz_put(char *o, int *n, const char *fmt, ...) {
    char t[1024]; va_list ap; va_start(ap, fmt); int k = vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    if (k < 0) return;
    if (k >= (int)sizeof t) k = (int)sizeof t - 1;
    if (*n + k > 960) { vkn_logf("%s", o); *n = snprintf(o, 64, "occ-z (cont.):"); }
    memcpy(o + *n, t, (size_t)k + 1); *n += k;
}
void vkn_occz_census(void) {
    if (!g_mode) return;
    static vkn_oz_stats p; static uint64_t pf, ptap, pjob, pchk, pg, pgn, pw[16];
    vkn_oz_stats s; vkn_ozm_stats_get(&s);
    const uint64_t fr = s.frames - p.frames; const double df = fr ? (double)fr : 1.0;
    static const char *const wn[VKN_OZ_R_N] = {"hidden", "no snapshot", "camera moved (no reprojection)", "other target", "scene changed", "snapshot too old", "no object",
        "no box", "unclean", "off screen", "depth (visible)", "hazard", "budget", "reprojection", "not a sheet"};
    char o[1100]; int n = 0;
#define D(x) ((double)(s.x - p.x) / df)
    cz_put(o, &n, "occ-z: DM_VK_NATIVE_COND=%d level %d%s; a frame (%llu decided): pairs %.1f, candidates %.1f, HIDDEN %.1f, poison-eligible %.1f; why:",
                  g_mode, g_level, g_off_violation ? " (skips off: violation)" : "", (unsigned long long)fr, D(pairs), D(cands), D(hidden), D(poison));
    for (int i = 1; i < VKN_OZ_R_N; i++) if (s.why[i] - p.why[i]) cz_put(o, &n, " %s %.1f,", wn[i], D(why[i]));
    static uint64_t pcc; const double wcond_e = (double)(g_ecount_cond - pcc) / df; pcc = g_ecount_cond;
    const double wpre = D(would_pre), wcond = D(would_cond) + wcond_e, age = s.snaps_used > p.snaps_used ? (double)(s.snap_age_sum - p.snap_age_sum) / (double)(s.snaps_used - p.snaps_used) : 0.0;
    cz_put(o, &n, " | would skip: pre-pass draws %.1f, conditional draws %.1f; hazards %.1f (whole screen %.1f), objects changed %.1f (box %.1f, signature %.1f; camera bytes only, not counted %.1f), order-sensitive frames %.0f %%; snapshot age %.1f frames (used in %.0f %% of frames)",
                  wpre, wcond, D(hazards), D(hazard_whole), D(changed), D(changed_box), D(changed_sig), D(changed_cam_only), 100.0 * D(order_frames), age, 100.0 * (double)(s.snaps_used - p.snaps_used) / df);
    cz_put(o, &n, " | v2 moved camera: frames %.0f %%, candidates %.1f, HIDDEN %.1f a frame, swept region %.1f cells a candidate",
                  100.0 * D(moved_frames), D(moved_cands), D(moved_hidden), s.moved_cands > p.moved_cands ? (double)(s.moved_region_cells - p.moved_region_cells) / (double)(s.moved_cands - p.moved_cands) : 0.0);
    const double dw = g_w.recs > pw[0] ? (double)(g_w.recs - pw[0]) : 1.0;
    cz_put(o, &n, " | worker: skipped pre %.1f cond %.1f empty %.1f, refused pre %.1f cond %.1f, lookups without the table yet %.1f a frame; snapshots %llu (failed %llu)",
                  (double)(g_w.skip_pre - pw[1]) / dw, (double)(g_w.skip_cond - pw[2]) / dw, (double)(g_w.skip_empty - pw[3]) / dw, (double)(g_w.refuse_pre - pw[4]) / dw,
                  (double)(g_w.refuse_cond - pw[5]) / dw, (double)(g_w.late - pw[6]) / dw, (unsigned long long)(g_w.snaps - pw[7]), (unsigned long long)(g_w.snap_fail - pw[8]));
    cz_put(o, &n, " | self-check: decisions %.1f a checked frame, VIOLATIONS %llu (total %llu), lost %llu",
                  s.check_frames > p.check_frames ? (double)(s.checked - p.checked) / (double)(s.check_frames - p.check_frames) : 0.0,
                  (unsigned long long)(s.violations - p.violations), (unsigned long long)s.violations, (unsigned long long)(s.check_lost - p.check_lost));
    if (g_mode == 11) cz_put(o, &n, "; queries of hidden pairs %llu, with samples %llu (drawn before their occluders or a box too small)",
                                    (unsigned long long)(g_w.qchk - pw[9]), (unsigned long long)(g_w.qdis - pw[10]));
    // own cost and the gain estimate (us a frame)
    const double tap = (double)(g_tap_ns - ptap) / df / 1e3, job = (double)(g_job_ns - pjob) / df / 1e3, chk = (double)(g_check_ns - pchk) / df / 1e3;
    const double gat = (double)(g_gather_ns - pg) / df / 1e3, wfx = (double)(g_w.fx_ns - pw[11]) / dw / 1e3, wsn = (double)(g_w.snap_ns - pw[12]) / dw / 1e3;
    const double eng = tap + job, wown = wfx + wsn, saved = (wpre + wcond) * (double)g_draw_ns / 1e3, slack = g_slack_ema / 1e3;
    cz_put(o, &n, " | OWN COST us a frame: engine %.0f (taps %.0f, decide %.0f incl. self-check %.0f), job threads gather %.0f, worker %.0f (verdict lookups %.0f, snapshot record %.0f); "
                  "engine slack %.0f us, budget %.0f us; ubo bytes hashed %.1f KB, inputs without a version %.1f | NET (worker): saved %.0f - own %.0f = %+.0f us a frame%s; at 2.5x slower CPU: %+.0f us, engine %.0f us %s the slack",
                  eng, tap, job, chk, gat, wown, wfx, wsn, slack, (double)g_cfg.budget_ns / 1e3, (double)g_ubo_bytes / df / 1024.0, (double)g_unver / df,
                  saved, wown, saved - wown, g_mode == 12 ? "" : " (would)", 2.5 * (saved - wown), 2.5 * eng, 2.5 * eng < slack ? "fits" : "does NOT fit");
    (void)pf; (void)pgn;
#undef D
    vkn_logf("%s", o);
    {   // [v2 diagnosis] the sources of "order-sensitive" and "scene changed", the camera's change, the camera-free signature
        static uint64_t q[40]; static double qd[4];
        const double f2 = g_dg.frames > q[0] ? (double)(g_dg.frames - q[0]) : 1.0;
#define G(i, x) ((double)(g_dg.x - q[i]) / f2)
        vkn_logf("occ-z why (order, globals): order-sensitive frames %.0f %% - depth writes a frame: under a stencil test in pairs %.1f / outside %.1f, raising the depth after another "
                 "write in pairs %.1f / outside %.1f, unknown state %.1f; first: %s | globals changed in %.0f %% of frames: depth-writing draws outside pairs %.1f a frame "
                 "(%.1f before the first pair), changed %.1f (camera bytes only %.1f); pairs without a box %.1f; depth events before the pre-pass %.1f, inside %.1f",
                 100.0 * G(1, ord_frames), G(2, ord_pair_st), G(3, ord_out_st), G(4, ord_pair_nm), G(5, ord_out_nm), G(6, ord_unknown), g_dg_first[0] ? g_dg_first : "none",
                 100.0 * G(7, glob_changed), G(8, out_draws), G(9, out_p0), G(10, out_changed), G(11, out_cam_only), G(12, nobox), G(13, dev0), G(14, dev1));
        vkn_logf("occ-z why (camera, signature): frames with a camera %.0f %%, changed %.0f %% of them (eye moved %.0f, turned only %.0f, projection only %.0f); eye move %.4f avg / %.4f max, turn %.3f / %.3f deg | "
                 "camera block reads with a view row %.1f a frame, blocks consistent %.1f / not %.1f, camera rows cut %.1f; its ViewProjection = the gathered camera %.1f, transposed %.1f, other %.1f",
                 100.0 * (1.0 - G(15, cam_novp)), g_dg.cam_frames > q[16] ? 100.0 * (double)(g_dg.cam_diff - q[17]) / (double)(g_dg.cam_frames - q[16]) : 0.0,
                 (double)(g_dg.cam_eye - q[18]), (double)(g_dg.cam_rot - q[19]), (double)(g_dg.cam_proj_only - q[20]),
                 g_dg.cam_diff > q[17] ? (g_dg.eye_sum - qd[0]) / (double)(g_dg.cam_diff - q[17]) : 0.0, g_dg.eye_max,
                 g_dg.cam_diff > q[17] ? (g_dg.rot_sum - qd[1]) / (double)(g_dg.cam_diff - q[17]) : 0.0, g_dg.rot_max,
                 G(21, camdep), G(22, camblk_ok), G(23, camblk_bad), G(24, camrows_cut), G(25, pv_same), G(26, pv_transposed), G(27, pv_other));
#undef G
        q[0] = g_dg.frames; q[1] = g_dg.ord_frames; q[2] = g_dg.ord_pair_st; q[3] = g_dg.ord_out_st; q[4] = g_dg.ord_pair_nm; q[5] = g_dg.ord_out_nm; q[6] = g_dg.ord_unknown;
        q[7] = g_dg.glob_changed; q[8] = g_dg.out_draws; q[9] = g_dg.out_p0; q[10] = g_dg.out_changed; q[11] = g_dg.out_cam_only; q[12] = g_dg.nobox; q[13] = g_dg.dev0; q[14] = g_dg.dev1;
        q[15] = g_dg.cam_novp; q[16] = g_dg.cam_frames; q[17] = g_dg.cam_diff; q[18] = g_dg.cam_eye; q[19] = g_dg.cam_rot; q[20] = g_dg.cam_proj_only;
        q[21] = g_dg.camdep; q[22] = g_dg.camblk_ok; q[23] = g_dg.camblk_bad; q[24] = g_dg.camrows_cut; q[25] = g_dg.pv_same; q[26] = g_dg.pv_transposed; q[27] = g_dg.pv_other;
        qd[0] = g_dg.eye_sum; qd[1] = g_dg.rot_sum; g_dg.eye_max = 0; g_dg.rot_max = 0; g_dg_first[0] = 0;
    }
    vkn_logf("occ-z gather: calls %llu, projected %llu, behind the camera %llu, bad box %llu, unknown pair %llu, two stages %llu",
             (unsigned long long)g_gst[0], (unsigned long long)g_gst[1], (unsigned long long)g_gst[2], (unsigned long long)g_gst[3], (unsigned long long)g_gst[4], (unsigned long long)g_gst[5]);
    p = s; ptap = g_tap_ns; pjob = g_job_ns; pchk = g_check_ns; pg = g_gather_ns; g_ubo_bytes = 0; g_unver = 0;
    pw[0] = g_w.recs; pw[1] = g_w.skip_pre; pw[2] = g_w.skip_cond; pw[3] = g_w.skip_empty; pw[4] = g_w.refuse_pre; pw[5] = g_w.refuse_cond; pw[6] = g_w.late;
    pw[7] = g_w.snaps; pw[8] = g_w.snap_fail; pw[9] = g_w.qchk; pw[10] = g_w.qdis; pw[11] = g_w.fx_ns; pw[12] = g_w.snap_ns;
}
#endif
