// [DOS2] VK-ENGINE-SIDE stage 1 (docs/dos2/nr/VK-ENGINE-SIDE.md, sections 4 I1 and 11): decisions in the engine's own list receivers,
// before a draw exists. DM_VK_NATIVE=1 only; every hook here is installed only when one of the switches below asks for it.
// main = main.raw of update v786432, build 4979b200. Facts (capstone of main.raw, 22.11; scripts work/agent-scripts/eng_*.py of wt-vk):
//   ShadowStage D (vtable 0x299b1a8; the directional light's shadows): +0x20 = 0xb15ae0 BUILD(D, ctx), +0x30 = 0xb164d0 EXECUTE(D, ctx).
//     Build (the frame-building thread): for each cascade i < [D+0x342] (count): its camera [[D+0x80+8i]+0x98] is fitted (0xb17410,
//     0xb16cf0 = the extra cull planes at camera+0x400), its matrices recomputed (0xa4b400); then D gets the cascade's View rows
//     (D+0xe0+0x40i, 64 B, from camera+0xe0), its far plane (D+0xbc+4i = camera+0x9c), its frustum width (D+0xcc+4i = 2 * camera+0x388)
//     and ProjectionMatrix[i] (D+0x1e0+0x40i) = bias x camera VP (0x8e2970 with {0.25, -0.25, 0.25, 0.25}: clip -> the cascade's quarter
//     of the atlas in uv); then the sub-stage's prepare (vtable +0x20 = 0xb14f00) stores the cascade number in its cull stage
//     ([sub+0xa8]+0x191 = [sub+0xb8] + 2 [sub+0xb0]) and enqueues the cull job (0xb14e40 -> 0xb18280: the query copied to cull+0x1c0,
//     its camera at cull+0x1d0) and the draw-building job (sub vtable +0x68 = 0xb15090) on the job system.
//     Execute (the render thread = our encoder thread): ExecuteCommandList(D+0x18) then each sub-stage's +0x30 (its cascade's list).
//   The cull receiver 0xb17ed0(cull, obj, x2) (job threads): (flags [obj+0x1d8] & 0x80002) == 2 -> {obj, x2, 0} appended to the chunked
//     list at cull+0x380 (atomic index). Its only reader: the cull stage's gather (vtable 0x299b2a0 +0x20 = 0xb181e0: waits for the job,
//     0xa87980 into cull+0x400 / +0x408 / +0x410) and the cascade's draw build 0xb15090, which reads +0x408 / +0x410. The camera fit
//     reads no list (it runs before the cull). So an object kept out of a cascade's list costs no engine, recording or GPU work and
//     changes no matrix.
//   The resolve 0xac8d10 (ShadowRenderStage, D = [stage+0x128]) copies D's View rows (name id [0x2bbb7b0]), ProjectionMatrix
//     ([0x2bbb7d8]), ShadowFarPlanes (D+0xbc, [0x2bbc0f0]) and ShadowFrustumWidths (D+0xcc, [0x2bbc0f8]) into its command list when it is
//     BUILT (the CascadedPCF constants, NVN uniform 1 c4[23 + 4i + r]): the only reader of those fields of D.
//   The main-view receiver 0xb0c570(stage, obj, x2) (CullStage, job threads; hle_dos2_vkn_occ.c has its facts): the object's lists by the
//     bits of [obj+0x1d8] (doc section 2.2). Sphere obj+0x13c..0x148 (centre, radius), box obj+0x14c..0x160.
//
// Switches (read once at install):
//   DM_VK_ENG_CENSUS      0 off (default) / 1 the engine census / 2 + the merge census of the recording thread (dm/gfx/vkn/vkn_eng.h).
//                         Hooks the shadow receiver, the shadow build / execute and (when DM_VK_NATIVE_COND does not take it) the
//                         main-view receiver. Line "[vkn] eng census ..." every 10 s.
//   DM_VK_ENG_FARRATE     I1b, the far cascade every N-th frame (vkn_shc.h, which has the backend half): 0 off (default: picture
//                         changes), 1 on, 2 count (decided, nothing left out), 3 poison. _N = N (2..4, default 2); _MOVE = the largest
//                         movement of the far cascade's projection in texels between two builds that still allows leaving the next one
//                         out (default 8: a camera cut or a fast pan draws every frame).
//   DM_VK_ENG_SHMIN       I1v, small casters kept out of the middle and far cascades: 0 off (default: picture changes), 1 on, 2 count,
//                         3 poison (the threshold x 16: far shadows of medium objects vanish). _PX = the threshold: the caster's bounding
//                         sphere diameter in texels of that cascade (default 1.0); _FROM = the first cascade it applies to (default 1).
//   DM_VK_ENG_SHTILE      the cascade tile in texels for the size classes until the backend has seen the far tile (default 768).
//   DM_VK_ENG_SHSTATIC    I1a, the persistent static-caster layer (block "[eng I1a]" below; doc section 13): 0 off (default), 1 on, 2 verify,
//                         3 poison; _FROM / _STABLE / _MIN / _SHIFT / _TRUST / _AGE. Takes over the cascades' draw build (0xb15090) and
//                         the shadow cull's gather (0xa87980, caller 0xb18234); line "[vkn] eng shstatic cK ..." every 10 s.
// The texel size of a cascade: its camera's view-projection (camera+0x1e0, rows by the 0xa4c030 convention clip[c] = sum_r p[r] e[4r+c])
// is orthographic; the larger length of its x / y columns = clip units per world unit; x tile / 2 = texels per world unit.
//   DM_VK_ENG_OCC         I2 (VK-ENGINE-SIDE.md section 12; the backend half and the rule: dm/gfx/vkn/vkn_eocc.h): occlusion culling in
//                         the main-view receiver against an earlier frame's depth. 0 off (default), 1 on (a hidden object never reaches
//                         any list of the stage: no pre-pass, G-buffer or other draw is built for it), 2 count (decided, nothing hidden),
//                         3 poison (drawn, its G-buffer draws magenta). Only objects of the opaque list (bit 18) without bits of other
//                         lists: 2..8, 10 (other passes: decals, custom opaque, refraction), 9 / 11 (alpha, overlay: the character outline
//                         behind walls), 12..14 (special), 16 (parts), 20; bit 0 (the "second set" lists) allowed unless
//                         DM_VK_ENG_OCC_BIT0=0. Never the shadow receiver. Not with DM_VK_NATIVE_COND 5 / 6 / 10..13 (they own the hook).
//                         The hook calls the original for every other object and reports, after it, whether the object went into the
//                         opaque lists and its report pair of this frame (obj+0x268 when obj+0x270 = the frame: the condition key of its
//                         G-buffer draws, which the poison / count modes recognise on the recording thread).
// Static vs moving (census only): an object whose sphere and box words are unchanged for 30 engine frames (a table of 32768 objects
// by address, the first thread to see an object in a frame updates it).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "../gfx/vkn/vkn.h"
#include "../gfx/vkn/vkn_eng.h"
#include "../gfx/vkn/vkn_eocc.h"
#include "../gfx/vkn/vkn_rt.h"
#include "../gfx/vkn/vkn_shc.h"
#include "hle_dos2_vkn.h"
#include "hle_dos2_vkn_stack.h"

#define ENG_RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"
enum { O_SPHERE = 0x13c, O_RADIUS = 0x148, O_BOX = 0x14c, O_FLAGS = 0x1d8, O_PAIR = 0x268, O_PAIR_FRAME = 0x270, ST_CAMERA = 0x36b0, CULL_IDX = 0x191, CULL_CAM = 0x1d0, CAM_STALE = 0x90, CAM_VP = 0x1e0,
       D_SUB = 0x80, SUB_CULL = 0xa8, D_FARP = 0xbc, D_WIDTH = 0xcc, D_VIEW = 0xe0, D_PROJ = 0x1e0, D_COUNT = 0x342,
       R_DEV = 0xb70, DEV_FRAME = 0x51f0 };
enum { CAS = 4, SZ = 8, OT_N = 1 << 15, OT_PROBE = 16, OT_STATIC = 30, OT_STALE = 600 };
static const float k_sz_edge[SZ - 1] = {0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 64.0f};

static int g_census, g_rate, g_period = 2, g_shmin, g_shmin_from = 1, g_occ, g_occ_bit0 = 1;
static float g_move = 8.0f, g_shmin_px = 1.0f, g_tile = 768.0f;
static vkn_fn g_o_recv, g_o_build, g_o_exec, g_o_main;

// ---- counters (atomics, relaxed: census only) ----
static struct {
    uint64_t builds, execs, recv, cast, other_stage, cas[CAS], st[CAS], mv[CAS], nw[CAS], sz[CAS][SZ], nocam[CAS];
    uint64_t far_left, far_would, small_left[CAS], small_would[CAS];
    uint64_t m_calls, m_hidden, m_special, m_opaque, m_opaque_st, m_opaque_mv, m_alpha, m_parts, m_other, m_batch, m_stages;
    uint64_t ot_full, fifo_over, fifo_under, dec_drawn, dec_left, dec_would, held_move, held_empty, held_first, held_bad;
} g_c;
#define ADD(f, n) __atomic_add_fetch(&g_c.f, (n), __ATOMIC_RELAXED)
static uint32_t g_scale_bits[CAS];   // the last world -> clip scale of each cascade (float bits)
static uint64_t g_census_frame0;     // engine frame at the last census line
static uint64_t g_main_stage[4];     // main-view stages seen in the window (distinct receivers' first argument)

static uint64_t eng_frame(void) {
    const uint64_t r = vkn_renderer(), dev = r ? vkn_rd64(r + R_DEV) : 0;
    return dev ? vkn_rd64(dev + DEV_FRAME) : 0;
}

// ---- static vs moving: objects by address ----
typedef struct ot_e { uint64_t key; uint32_t h, since, seen, pad; } ot_e;
static ot_e *g_ot;
static uint32_t words_hash(uint64_t p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i += 4) { h ^= vkn_rd32(p + i); h *= 16777619u; }
    return h;
}
static int ot_state(uint64_t obj, uint32_t fr) {   // 0 new / 1 moving / 2 static (unchanged >= OT_STATIC frames); -1 table full
    if (!g_ot) return -1;
    const uint32_t h = words_hash(obj + O_SPHERE, 0x160 + 4 - O_SPHERE);   // sphere + radius + box
    uint32_t i = (uint32_t)((obj >> 4) * 0x9E3779B97F4A7C15ull >> 49) & (OT_N - 1);
    for (int p = 0; p < OT_PROBE; p++, i = (i + 1) & (OT_N - 1)) {
        ot_e *e = &g_ot[i];
        uint64_t k = __atomic_load_n(&e->key, __ATOMIC_ACQUIRE);
        if (k != obj) {
            const uint32_t seen = __atomic_load_n(&e->seen, __ATOMIC_RELAXED);
            if (k && fr - seen < OT_STALE) continue;
            if (!__atomic_compare_exchange_n(&e->key, &k, obj, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { if (k != obj) continue; }
            else { __atomic_store_n(&e->h, h, __ATOMIC_RELAXED); __atomic_store_n(&e->since, fr, __ATOMIC_RELAXED); __atomic_store_n(&e->seen, fr, __ATOMIC_RELAXED); return 0; }
        }
        if (__atomic_exchange_n(&e->seen, fr, __ATOMIC_RELAXED) != fr && __atomic_load_n(&e->h, __ATOMIC_RELAXED) != h) {
            __atomic_store_n(&e->h, h, __ATOMIC_RELAXED); __atomic_store_n(&e->since, fr, __ATOMIC_RELAXED);
        }
        return fr - __atomic_load_n(&e->since, __ATOMIC_RELAXED) >= OT_STATIC ? 2 : 1;
    }
    ADD(ot_full, 1);
    return -1;
}

// ---- the far-cascade rate: the build's decision (I1b) ----
static uint64_t g_cull[CAS];               // the cull stages of D's cascades (this build)
static int g_far_idx = -1;                 // the far cascade's number
static int g_skip, g_would;                // this build: leave the far cascade out / would (receivers read them)
static int g_have, g_run, g_moved, g_last_dec, g_bad_local, g_off_said, g_have_prev;
static uint64_t g_far_objs, g_last_drawn_objs;
typedef struct far_fields { uint8_t view[64], proj[64], farp[4], width[4]; } far_fields;
static far_fields g_saved, g_prev;
static int g_fifo[8]; static uint32_t g_fh, g_ft;
static void far_read(uint64_t D, int k, far_fields *f) {
    memcpy(f->view, (const void *)(uintptr_t)(D + D_VIEW + 0x40u * (uint32_t)k), 64); memcpy(f->proj, (const void *)(uintptr_t)(D + D_PROJ + 0x40u * (uint32_t)k), 64);
    memcpy(f->farp, (const void *)(uintptr_t)(D + D_FARP + 4u * (uint32_t)k), 4); memcpy(f->width, (const void *)(uintptr_t)(D + D_WIDTH + 4u * (uint32_t)k), 4);
}
static void far_write(uint64_t D, int k, const far_fields *f) {
    memcpy((void *)(uintptr_t)(D + D_VIEW + 0x40u * (uint32_t)k), f->view, 64); memcpy((void *)(uintptr_t)(D + D_PROJ + 0x40u * (uint32_t)k), f->proj, 64);
    memcpy((void *)(uintptr_t)(D + D_FARP + 4u * (uint32_t)k), f->farp, 4); memcpy((void *)(uintptr_t)(D + D_WIDTH + 4u * (uint32_t)k), f->width, 4);
}
static float tile_px(void) { const int t = vkn_shc_eng_far_tile(); return t > 0 ? (float)t : g_tile; }
static float proj_motion(const far_fields *a, const far_fields *b) {   // the largest change of a projection word, in atlas texels
    float m = 0.0f;
    for (int i = 0; i < 16; i++) { float x, y; memcpy(&x, a->proj + 4 * i, 4); memcpy(&y, b->proj + 4 * i, 4); const float d = fabsf(x - y); if (!(d <= m)) m = d; }
    return m * 2.0f * tile_px();   // uv of the whole atlas (2 x 2 tiles)
}

static void fifo_push(int v) {   // build thread -> encoder thread: one entry per build (-1: no decision), popped by its execute
    const uint32_t h = __atomic_load_n(&g_fh, __ATOMIC_RELAXED), t = __atomic_load_n(&g_ft, __ATOMIC_ACQUIRE);
    if (h - t >= 4) { ADD(fifo_over, 1); g_bad_local++; return; }   // builds without executes: the pairing is lost
    g_fifo[h & 7] = v; __atomic_store_n(&g_fh, h + 1, __ATOMIC_RELEASE);
}
static uint64_t g_D, g_build_frame, g_build_seq, g_res_seq;
static uint64_t g_res_after, g_res_other;   // resolve builds that came after the shadow build of their frame / otherwise (census + guard)
VKN_HOOK eh_build(VKN_A) {   // 0xb15ae0 ShadowStage build (D, ctx)
    const uint64_t D = a0;
    if (!D) return g_o_build(VKN_F);
    ADD(builds, 1);
    const int cnt = vkn_rd8(D + D_COUNT), far = cnt >= 2 && cnt <= CAS ? cnt - 1 : -1;
    for (int i = 0; i < CAS; i++) {
        const uint64_t sub = i < cnt ? vkn_rd64(D + D_SUB + 8u * (uint32_t)i) : 0;
        __atomic_store_n(&g_cull[i], sub ? vkn_rd64(sub + SUB_CULL) : 0, __ATOMIC_RELAXED);
    }
    if (D != g_D || far != __atomic_load_n(&g_far_idx, __ATOMIC_RELAXED)) { g_D = D; g_have = g_have_prev = 0; g_run = 0; g_last_dec = 0; }   // another level / cascade count
    const uint64_t objs = __atomic_exchange_n(&g_far_objs, 0, __ATOMIC_RELAXED);   // (the far objects the last build's cull saw)
    if (g_last_dec == 0) g_last_drawn_objs = objs;
    int dec = 0;
    if (g_rate && far >= 0 && g_res_other >= 30 && g_res_other > g_res_after) {   // the resolve is not built after the shadow build: the
        if (!g_bad_local) vkn_logf(ENG_RED "eng: DM_VK_ENG_FARRATE: the resolve stage was built %llu times without the shadow build of its frame before it (%llu with): "
                                   "the far matrices cannot be put back for it - off", (unsigned long long)g_res_other, (unsigned long long)g_res_after);   // far matrices would miss it
        g_bad_local = 3;
    }
    if (g_rate && far >= 0) {
        const int bad = vkn_shc_eng_far_bad() + g_bad_local;
        if (bad >= 3) {
            ADD(held_bad, 1);
            if (!g_off_said) { g_off_said = 1; vkn_logf(ENG_RED "eng: DM_VK_ENG_FARRATE: %d decisions disagreed with the recording (vkn_shc.h): the far cascade is drawn every frame for this session", bad); }
        } else if (!g_have) ADD(held_first, 1);
        else if (g_moved) ADD(held_move, 1);
        else if (!g_last_drawn_objs) ADD(held_empty, 1);
        else if (g_run < g_period - 1) dec = 1;
    }
    __atomic_store_n(&g_far_idx, far, __ATOMIC_RELAXED);
    __atomic_store_n(&g_skip, dec && (g_rate == 1 || g_rate == 3), __ATOMIC_RELEASE);
    __atomic_store_n(&g_would, dec && g_rate == 2, __ATOMIC_RELEASE);
    const uint64_t rc = g_o_build(VKN_F);
    g_build_frame = eng_frame(); g_build_seq++;
    if (g_rate && far >= 0) {
        far_fields fresh; far_read(D, far, &fresh);
        g_moved = g_have_prev && proj_motion(&fresh, &g_prev) > g_move;
        g_prev = fresh; g_have_prev = 1;
        if (dec && g_rate != 2) far_write(D, far, &g_saved);   // the resolve of this build reads the far tile with the matrices it was drawn with
        else if (!dec) { g_saved = fresh; g_have = 1; }
        g_run = dec ? g_run + 1 : 0;
        if (dec) ADD(dec_left, g_rate == 2 ? 0 : 1); else ADD(dec_drawn, 1);
        if (dec && g_rate == 2) ADD(dec_would, 1);
        g_last_dec = dec && g_rate != 2;
        fifo_push(dec ? (g_rate == 2 ? 2 : 1) : 0);
    } else if (g_rate) fifo_push(-1);
    return rc;
}
// 0xac8d10 the resolve stage's build (ShadowRenderStage, x0): the guard of the matrices' order (FARRATE) - it must come after the
// shadow build of its own frame (then it reads D as that build left it). Counted; mostly "before" turns FARRATE off (eh_build).
static vkn_fn g_o_res;
VKN_HOOK eh_res(VKN_A) {
    if (g_build_seq != g_res_seq && g_build_frame == eng_frame()) g_res_after++; else g_res_other++;
    g_res_seq = g_build_seq;
    return g_o_res(VKN_F);
}
static void eh_dec_rec(void *p) { int v; memcpy(&v, p, sizeof v); vkn_shc_eng_far(v); }   // recording thread, in the frame's order
VKN_HOOK eh_exec(VKN_A) {   // 0xb164d0 ShadowStage execute (D, ctx): the encoder thread
    if (g_rate) {
        ADD(execs, 1);
        const uint32_t t = __atomic_load_n(&g_ft, __ATOMIC_RELAXED), h = __atomic_load_n(&g_fh, __ATOMIC_ACQUIRE);
        if (t == h) ADD(fifo_under, 1);
        else { const int v = g_fifo[t & 7]; __atomic_store_n(&g_ft, t + 1, __ATOMIC_RELEASE); vkn_rt_call(eh_dec_rec, &v, sizeof v); }
    }
    return g_o_exec(VKN_F);
}

// ---- the shadow receiver: census, I1b, I1v ----
VKN_HOOK eh_recv(VKN_A) {   // 0xb17ed0(cull, obj, x2): void
    const uint64_t cull = a0, obj = a1;
    if (!cull || !obj) return g_o_recv(VKN_F);
    const uint32_t fl = vkn_rd32(obj + O_FLAGS);
    if ((fl & 0x80002u) != 2u) return g_o_recv(VKN_F);   // not a caster (the original appends nothing)
    int k = -1;
    for (int i = 0; i < CAS; i++) if (__atomic_load_n(&g_cull[i], __ATOMIC_RELAXED) == cull) { k = i; break; }
    if (k < 0) { if (g_census) ADD(other_stage, 1); return g_o_recv(VKN_F); }   // another shadow stage (not the sun's cascades)
    const int far = __atomic_load_n(&g_far_idx, __ATOMIC_RELAXED) == k;
    if (far) __atomic_add_fetch(&g_far_objs, 1, __ATOMIC_RELAXED);
    float px = -1.0f;
    if (g_census || g_shmin) {
        const uint64_t cam = vkn_rd64(cull + CULL_CAM);
        if (cam && !vkn_rd8(cam + CAM_STALE)) {
            float e[12]; memcpy(e, (const void *)(uintptr_t)(cam + CAM_VP), sizeof e);
            const float sx = sqrtf(e[0] * e[0] + e[4] * e[4] + e[8] * e[8]), sy = sqrtf(e[1] * e[1] + e[5] * e[5] + e[9] * e[9]), s = sx > sy ? sx : sy;
            float r; memcpy(&r, (const void *)(uintptr_t)(obj + O_RADIUS), 4);
            if (s > 0.0f && r >= 0.0f) px = r * s * tile_px();   // diameter 2r x s clip units x tile / 2 texels each
            if (g_census) { uint32_t b; memcpy(&b, &s, 4); __atomic_store_n(&g_scale_bits[k], b, __ATOMIC_RELAXED); }
        } else if (g_census) ADD(nocam[k], 1);
    }
    if (g_census) {
        ADD(recv, 1); ADD(cas[k], 1);
        const int st = ot_state(obj, (uint32_t)eng_frame());
        if (st == 2) ADD(st[k], 1); else if (st == 1) ADD(mv[k], 1); else if (st == 0) ADD(nw[k], 1);
        if (px >= 0.0f) { int b = 0; while (b < SZ - 1 && px >= k_sz_edge[b]) b++; ADD(sz[k][b], 1); }
    }
    if (far && __atomic_load_n(&g_skip, __ATOMIC_ACQUIRE)) { ADD(far_left, 1); return 0; }   // I1b: not in this build's far list
    if (far && __atomic_load_n(&g_would, __ATOMIC_ACQUIRE)) ADD(far_would, 1);
    if (g_shmin && k >= g_shmin_from && px >= 0.0f && px < (g_shmin == 3 ? g_shmin_px * 16.0f : g_shmin_px)) {   // I1v
        if (g_shmin == 2) ADD(small_would[k], 1);
        else { ADD(small_left[k], 1); return 0; }
    }
    return g_o_recv(VKN_F);
}

// ================================================================================================ [eng I1a] DM_VK_ENG_SHSTATIC
#include "../gfx/nr/nr_shst.h"
static int env_int(const char *n, int def, int lo, int hi);
// The persistent static-caster layer (docs/dos2/nr/VK-ENGINE-SIDE.md section 13; the rules: dm/gfx/nr/nr_shst.h; the backend half:
// dm/gfx/vkn/vkn_shc.h block "eng I1a"). Facts (capstone of main.raw, 22.11):
//   The cascade's draw build 0xb15090(sub) (sub-stage vtable 0x299b0a0 +0x68, a job thread): set-up commands into the cascade's command
//     list sub+0x18 (cascade 0: the atlas clear, type 5; then render target 0x16, viewport 0x12 {T, T, [sub+0xb0] T, [sub+0xb8] T},
//     T = [[sub+0x90]+0x340], per-view constants 0x17 from [sub+0xc0] + 64 i, [[sub+0xd0] + 4 i], camera [sub+0x98] +0x74 (12 B), +0x98 and
//     the view-projection 0xa4be10(camera) (= camera+0x160 when clean), i = [sub+0xb8] + 2 [sub+0xb0]), then the cull stage's gather
//     ([[sub+0xa8]] +0x20 = 0xb181e0: waits for the cull job, 0xa87980(cull+0x380, cull+0x400) makes the array cull+0x408 of {object,
//     word, 0} entries (0x18 B), count = bits 24..47 of [cull+0x410]), the sort 0xa88090(0, n-1, cull+0x380, camera) by the objects'
//     vtable +0x80 key, and for i = n-1 .. 0: object->vtable[+0xb8](object, entry, ctx) appends the caster's commands. ctx = 0x70 B on
//     its stack: {list, (u32) 2, (u16) 0x10, [sub+0x10], sub, 0...}. [sub+0x48] = 0: immediate mode (no list; not taken over).
//   The command list allocator 0x941720(list, size, align) returns the command's memory; the executor (0xa49c30 -> 0x93e5f0) runs
//     command type 1 as a callback: [cmd+8](cmd+0x18, 1, 0) returns the payload's size (the command is 0x18 + that).
// What the hooks do: the gather (caller 0xb18234 only, while this cascade's draw build is in a hook) hands the entry count over and
// leaves 0 for the original (its sort and loop do nothing); after the original's set-up the hook plans (nr_shst.h), puts the entries
// back in groups and gives them to the casters' own +0xb8 in the original's way, with callback commands (markers: vkn_shc.h) between
// the groups and before each observed caster. The members of a valid layer are not given to the draw build at all.
enum { SUB_CTX = 0x10, SUB_LIST = 0x18, SUB_DEFER = 0x48, SUB_D = 0x90, SUB_CAM = 0x98, SUB_TX = 0xb0, SUB_TY = 0xb8, SUB_VIEWS = 0xc0, SUB_FARS = 0xd0,
       D_TILE = 0x340, CULL_LIST = 0x380, CULL_OUT = 0x400, CULL_ARR = 0x408, CULL_CNT = 0x410, CAM_POS = 0x74, CAM_AUX = 0x98, O_VT_SHADOW = 0xb8,   /* O_BOX = 0x14c: the census enum above */
       ST_MAXN = 6144, ENT = 0x18 };
typedef struct st_k {
    int busy, have; uint64_t cull, cnt;      // the gather's hand-over (a cascade's draw build runs on one job at a time)
    nrst_cas c; nrst_stats prev;
    nrst_in in[ST_MAXN]; uint8_t grp[ST_MAXN]; int32_t slot[ST_MAXN]; uint32_t tag[ST_MAXN]; uint32_t perm[ST_MAXN];
    uint8_t ent[ST_MAXN * ENT];
    uint64_t busy_n, imm, many, farskip, calls, marks, given, left;
} st_k;
static int g_st, g_st_from = 1, g_st_off_said; static nrst_par g_stp;
static st_k *g_stk[CAS];
static vkn_fn g_o_dbuild, g_o_gath;
static void *g_mark_stub; static uint64_t g_gath_ret;
typedef uint64_t (*st_alloc_fn)(uint64_t list, uint64_t size, uint64_t align);
typedef void (*st_sort_fn)(int lo, int hi, uint64_t list, uint64_t cam);
typedef uint64_t (*st_vp_fn)(uint64_t cam);
typedef void (*st_cast_fn)(uint64_t obj, uint64_t entry, uint64_t ctx);
static void st_mark_rec(void *p) { vkn_shc_eng_st((const vkn_shst_mark *)p); }
VKN_HOOK eh_mark(VKN_A) {   // command type 1: fn(payload, 1, 0) on the render thread, at its place among the cascade's draws
    VKN_UNUSED;
    if (a1 == 1 && a2 == 0 && a0) vkn_rt_call(st_mark_rec, (const void *)(uintptr_t)a0, sizeof(vkn_shst_mark));
    return sizeof(vkn_shst_mark);
}
static void st_mark(st_k *s, uint64_t list, const vkn_shst_mark *m) {
    uint8_t *p = (uint8_t *)(uintptr_t)((st_alloc_fn)(uintptr_t)(dm_vkn_base() + 0x941720))(list, 0x18 + sizeof *m, 8);
    if (!p) return;
    memset(p, 0, 0x18); p[0] = 1; memcpy(p + 8, &g_mark_stub, 8); memcpy(p + 0x18, m, sizeof *m);
    s->marks++;
}
static void st_mark_k(st_k *s, uint64_t list, uint32_t kind, int k, uint32_t slot, uint32_t tag) {
    vkn_shst_mark m; memset(&m, 0, sizeof m); m.magic = VKN_SHST_MAGIC; m.kind = kind; m.k = (uint32_t)k; m.slot = slot; m.tag = tag;
    st_mark(s, list, &m);
}
VKN_HOOK eh_gath(VKN_A) {   // 0xa87980(list, out): the chunked list into the array (many callers; only the shadow cull's gather is ours)
    const uint64_t rc = g_o_gath(VKN_F);
    if (dm_vkn_caller() != g_gath_ret) return rc;
    const uint64_t cull = a0 - CULL_LIST;
    for (int k = g_st_from; k < CAS; k++) {
        st_k *s = g_stk[k];
        if (!s || !__atomic_load_n(&s->busy, __ATOMIC_ACQUIRE) || s->cull != cull || a1 != cull + CULL_OUT) continue;
        const uint64_t v = vkn_rd64(cull + CULL_CNT), z = v & ~(0xffffffull << 24);
        s->cnt = v; s->have = 1;
        memcpy((void *)(uintptr_t)(cull + CULL_CNT), &z, 8);   // the original's sort and loop see no entry
        break;
    }
    return rc;
}
static int st_verdict(void *u, uint32_t slot, uint32_t tag) { return vkn_shc_eng_st_verdict((int)(intptr_t)u, slot, tag); }
static uint32_t st_words(uint64_t obj, uint64_t word) {
    uint32_t h = words_hash(obj + O_SPHERE, 0x164 - O_SPHERE);   // sphere, radius, box
    h = (h ^ vkn_rd32(obj + O_FLAGS)) * 16777619u; h = (h ^ (uint32_t)word) * 16777619u; h = (h ^ (uint32_t)(word >> 32)) * 16777619u;
    return h;
}
// Entries [lo, hi] of the array in the original's way: sorted (unless observed: their markers follow the order of the plan), the last
// first; observed casters get an OBJ marker before their commands.
static void st_give(st_k *s, int k, uint64_t arr, uint64_t list, uint64_t cull, uint64_t cam, uint64_t ctx, uint32_t lo, uint32_t hi, int mark) {
    if (hi <= lo) return;
    if (!mark && hi - lo > 1) ((st_sort_fn)(uintptr_t)(dm_vkn_base() + 0xa88090))((int)lo, (int)(hi - 1), cull + CULL_LIST, cam);
    for (uint32_t i = hi; i-- > lo;) {
        const uint64_t e = arr + (uint64_t)i * ENT, obj = vkn_rd64(e);
        if (!obj) continue;
        if (mark) { const uint32_t o = s->perm[i]; st_mark_k(s, list, VKN_SHST_OBJ, k, s->slot[o] < 0 ? 0xffffffffu : (uint32_t)s->slot[o], s->tag[o]); }
        ((st_cast_fn)(uintptr_t)vkn_rd64(vkn_rd64(obj) + O_VT_SHADOW))(obj, e, ctx);
        s->given++;
    }
}
static void st_frame(int k, st_k *s, uint64_t sub, uint64_t cull) {
    const uint64_t mask = 0xffffffull << 24, now = vkn_rd64(cull + CULL_CNT), back = (now & ~mask) | (s->cnt & mask);
    memcpy((void *)(uintptr_t)(cull + CULL_CNT), &back, 8);   // the count back (as the original leaves it)
    const uint32_t n = (uint32_t)(s->cnt >> 24 & 0xffffff);
    const uint64_t arr = vkn_rd64(cull + CULL_ARR), list = sub + SUB_LIST, cam = vkn_rd64(sub + SUB_CAM);
    uint64_t ctx[32]; memset(ctx, 0, sizeof ctx);   // (the original's 0x70 bytes on its stack)
    ctx[0] = list; { const uint32_t two = 2; const uint16_t f = 0x10; memcpy((uint8_t *)ctx + 8, &two, 4); memcpy((uint8_t *)ctx + 12, &f, 2); }
    ctx[2] = vkn_rd64(sub + SUB_CTX); ctx[3] = sub;
    const uint64_t cx = (uint64_t)(uintptr_t)ctx;
    s->calls++;
    if ((!arr && n) || !cam || n > ST_MAXN) {   // not planned: the original's own sort and loop
        if (n > ST_MAXN) s->many++;
        for (uint32_t i = 0; i < n; i++) s->perm[i] = i;
        st_give(s, k, arr, list, cull, cam, cx, 0, arr ? n : 0, 0);
        return;
    }
    // the camera words (what the cascade's per-view constants are made of) and the tile
    uint32_t cw[NRST_CAMW];
    const uint64_t vpp = ((st_vp_fn)(uintptr_t)(dm_vkn_base() + 0xa4be10))(cam);
    const uint32_t ci = vkn_rd32(sub + SUB_TY) + 2u * vkn_rd32(sub + SUB_TX);
    memcpy(cw + NRST_VP, (const void *)(uintptr_t)vpp, 64);
    memcpy(cw + NRST_VIEW, (const void *)(uintptr_t)(vkn_rd64(sub + SUB_VIEWS) + 64ull * ci), 64);
    memcpy(cw + NRST_POS, (const void *)(uintptr_t)(cam + CAM_POS), 12);
    cw[NRST_FAR] = vkn_rd32(vkn_rd64(sub + SUB_FARS) + 4ull * ci); cw[NRST_AUX] = vkn_rd32(cam + CAM_AUX);
    const int32_t T = (int32_t)vkn_rd16(vkn_rd64(sub + SUB_D) + D_TILE);
    const int32_t rect[4] = {(int32_t)vkn_rd32(sub + SUB_TX) * T, (int32_t)vkn_rd32(sub + SUB_TY) * T, T, T};
    // the casters
    if (n) memcpy(s->ent, (const void *)(uintptr_t)arr, (size_t)n * ENT);
    for (uint32_t i = 0; i < n; i++) {
        const uint64_t obj = vkn_rd64(arr + (uint64_t)i * ENT), word = vkn_rd64(arr + (uint64_t)i * ENT + 8);
        s->in[i].key = obj ? obj : 1; s->in[i].h = obj ? st_words(obj, word) : 0;
        if (obj) memcpy(s->in[i].box, (const void *)(uintptr_t)(obj + O_BOX), 24); else memset(s->in[i].box, 0, 24);
    }
    nrst_par p = g_stp;
    if (vkn_shc_eng_st_bad() >= 3) {
        p.mode = 0;
        if (!g_st_off_said) { g_st_off_said = 1; vkn_logf(ENG_RED "eng: DM_VK_ENG_SHSTATIC: %d reuses could not be honoured by the backend: static casters are drawn every frame for this session", vkn_shc_eng_st_bad()); }
    }
    uint32_t ae = 0; const int as = vkn_shc_eng_st_ack(k, &ae), safe = vkn_shc_eng_st_safe();
    uint32_t ack_e = 0; int ack_bad = 0; nrst_ack_view(safe, s->c.epoch, ae, as, &ack_e, &ack_bad);   // [fix2]
    nrst_plan pl; nrst_plan_frame(&s->c, &p, s->in, n, cw, rect, st_verdict, (void *)(intptr_t)k, ack_e, ack_bad, s->grp, s->slot, s->tag, &pl);
    // the order of the groups: BUILD STATIC | MID | OBS | NONE | DYN; REUSE TOUCH | OBS | NONE | DYN (LEFT not given); VERIFY OBS | NONE |
    // DYN | V1 | LEFT | V2 | TOUCH; OFF / HOLD OBS | NONE | DYN
    // [fix2] DM_VK_ENG_SHSTATIC_SAFE: REUSE SKIP0 | LEFT | SKIP1 | TOUCH | OBS | NONE | DYN (the members first: skipped by the backend when the
    // layer's camera matched at their first draw, drawn otherwise)
    static const uint8_t k_ord_all[2][NRST_A_N][5] = {
        {{NRST_G_OBS, NRST_G_DYN, 255, 255, 255}, {NRST_G_OBS, NRST_G_DYN, 255, 255, 255}, {NRST_G_STATIC, NRST_G_OBS, NRST_G_DYN, 255, 255},
         {NRST_G_TOUCH, NRST_G_OBS, NRST_G_DYN, 255, 255}, {NRST_G_OBS, NRST_G_DYN, NRST_G_LEFT, NRST_G_TOUCH, 255}},
        {{NRST_G_OBS, NRST_G_DYN, 255, 255, 255}, {NRST_G_OBS, NRST_G_DYN, 255, 255, 255}, {NRST_G_STATIC, NRST_G_OBS, NRST_G_DYN, 255, 255},
         {NRST_G_LEFT, NRST_G_TOUCH, NRST_G_OBS, NRST_G_DYN, 255}, {NRST_G_OBS, NRST_G_DYN, NRST_G_LEFT, NRST_G_TOUCH, 255}}};
    const uint8_t (*k_ord)[5] = k_ord_all[safe ? 1 : 0];
    uint32_t lo[NRST_G_N], cnt[NRST_G_N] = {0}, at = 0;
    for (uint32_t i = 0; i < n; i++) cnt[s->grp[i]]++;
    for (int j = 0; j < 5 && k_ord[pl.act][j] != 255; j++) { lo[k_ord[pl.act][j]] = at; at += cnt[k_ord[pl.act][j]]; }
    const uint32_t given_n = at;
    for (int g = 0; g < NRST_G_N; g++) { int in = 0; for (int j = 0; j < 5; j++) in |= k_ord[pl.act][j] == g; if (!in) { lo[g] = at; at += cnt[g]; } }
    uint32_t fill[NRST_G_N]; memcpy(fill, lo, sizeof fill);
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t d = fill[s->grp[i]]++;
        s->perm[d] = i;
        memcpy((void *)(uintptr_t)(arr + (uint64_t)d * ENT), s->ent + (size_t)i * ENT, ENT);
    }
    vkn_shst_mark m; memset(&m, 0, sizeof m);
    m.magic = VKN_SHST_MAGIC; m.kind = VKN_SHST_START; m.k = (uint32_t)k; m.act = (uint32_t)pl.act; m.epoch = pl.epoch; m.nx = pl.nx; m.ny = pl.ny;
    memcpy(m.rect, rect, sizeof m.rect); for (int g = 0; g < 5; g++) m.n[g] = pl.n[g]; m.n[5] = pl.members;
    st_mark(s, list, &m);
#define GIVE(g, mk) st_give(s, k, arr, list, cull, cam, cx, lo[g], lo[g] + cnt[g], mk)
    switch (pl.act) {
    case NRST_A_BUILD: GIVE(NRST_G_STATIC, 1); st_mark_k(s, list, VKN_SHST_MID, k, 0, 0); GIVE(NRST_G_OBS, 1); st_mark_k(s, list, VKN_SHST_NONE, k, 0, 0); GIVE(NRST_G_DYN, 0); break;
    case NRST_A_REUSE:
        if (safe) { st_mark_k(s, list, VKN_SHST_SKIP0, k, 0, 0); GIVE(NRST_G_LEFT, 0); st_mark_k(s, list, VKN_SHST_SKIP1, k, 0, 0); }
        GIVE(NRST_G_TOUCH, 0); GIVE(NRST_G_OBS, 1); st_mark_k(s, list, VKN_SHST_NONE, k, 0, 0); GIVE(NRST_G_DYN, 0); break;
    case NRST_A_VERIFY:
        GIVE(NRST_G_OBS, 1); st_mark_k(s, list, VKN_SHST_NONE, k, 0, 0); GIVE(NRST_G_DYN, 0);
        st_mark_k(s, list, VKN_SHST_V1, k, 0, 0); GIVE(NRST_G_LEFT, 0); st_mark_k(s, list, VKN_SHST_V2, k, 0, 0); GIVE(NRST_G_TOUCH, 0); break;
    default: GIVE(NRST_G_OBS, 1); st_mark_k(s, list, VKN_SHST_NONE, k, 0, 0); GIVE(NRST_G_DYN, 0); break;
    }
#undef GIVE
    st_mark_k(s, list, VKN_SHST_END, k, 0, 0);
    s->left += n - given_n;
}
VKN_HOOK eh_dbuild(VKN_A) {   // 0xb15090(sub): the cascade's draw build (a job thread)
    const uint64_t sub = a0, cull = sub ? vkn_rd64(sub + SUB_CULL) : 0;
    int k = -1;
    for (int i = 0; i < CAS && cull; i++) if (__atomic_load_n(&g_cull[i], __ATOMIC_RELAXED) == cull) { k = i; break; }
    st_k *s = k >= g_st_from ? g_stk[k] : NULL;
    if (!s) return g_o_dbuild(VKN_F);
    if (!vkn_rd8(sub + SUB_DEFER)) { s->imm++; return g_o_dbuild(VKN_F); }   // immediate mode: no list to put markers in
    if (k == __atomic_load_n(&g_far_idx, __ATOMIC_RELAXED) && __atomic_load_n(&g_skip, __ATOMIC_ACQUIRE)) { s->farskip++; return g_o_dbuild(VKN_F); }   // I1b left it out
    if (__atomic_exchange_n(&s->busy, 1, __ATOMIC_ACQ_REL)) { s->busy_n++; return g_o_dbuild(VKN_F); }
    s->cull = cull; s->have = 0;
    const uint64_t rc = g_o_dbuild(VKN_F);
    if (s->have) st_frame(k, s, sub, cull);   // (no gather: the original returned before it - shadows off in the settings)
    __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE);
    return rc;
}
static void st_census(void) {
    typedef unsigned long long ull;
    for (int k = g_st_from; k < CAS; k++) {
        st_k *s = g_stk[k]; if (!s) continue;
        nrst_stats c = s->c.st, d; const uint64_t *x = (const uint64_t *)&c, *y = (const uint64_t *)&s->prev; uint64_t *z = (uint64_t *)&d;
        for (size_t i = 0; i < sizeof d / 8; i++) z[i] = x[i] - y[i];
        s->prev = c;
        if (!d.frames) continue;
        const double f = (double)d.frames;
        char b[1000]; size_t o = 0;
        o += (size_t)snprintf(b + o, sizeof b - o, "eng shstatic c%d (DM_VK_ENG_SHSTATIC=%d, last 10 s, %llu builds of the cascade): off %llu, hold %llu, build %llu, reuse %llu (exact %llu, shifted %llu, before the ack %llu), verify %llu; "
                              "casters a frame %.0f: left out %.0f, strip touchers %.0f, members drawn at a build %.0f, observed %.0f, other %.0f; members %.0f; dropped [", k, g_st, (ull)d.frames,
                              (ull)d.act[NRST_A_OFF], (ull)d.act[NRST_A_HOLD], (ull)d.act[NRST_A_BUILD], (ull)d.act[NRST_A_REUSE], (ull)d.exact, (ull)d.shifted, (ull)d.optimistic, (ull)d.act[NRST_A_VERIFY],
                              d.objs / f, d.left / f, d.touch / f, d.statics / f, d.obs / f, d.dyn / f, d.members / f);
        for (int r = 1, m = 0; r < NRST_R_N && o < sizeof b - 64; r++) if (d.drop[r]) o += (size_t)snprintf(b + o, sizeof b - o, "%s%s %llu", m++ ? ", " : "", nrst_r_name(r), (ull)d.drop[r]);
        o += (size_t)snprintf(b + o, sizeof b - o, "]; no build: few candidates %llu, camera moving %llu; acks good %llu bad %llu; table full %llu; immediate %llu, busy %llu, too many %llu, far left out %llu; markers %llu",
                              (ull)d.nob[NRST_N_FEW], (ull)d.nob[NRST_N_MOVING], (ull)d.acks_good, (ull)d.acks_bad, (ull)d.full, (ull)s->imm, (ull)s->busy_n, (ull)s->many, (ull)s->farskip, (ull)s->marks);
        s->imm = s->busy_n = s->many = s->farskip = s->marks = 0;
        vkn_logf("%s", b);
    }
}
// Registration (from dm_vkn_eng_register): the switch, the tables, the marker stub; 1 = the hooks are wanted.
static int st_register(void) {
    g_st = env_int("DM_VK_ENG_SHSTATIC", 0, 0, 3);
    if (!g_st) return 0;
    if (vkn_shc_eng_st_mode() != g_st) { vkn_logf("eng: DM_VK_ENG_SHSTATIC=%d but the shadow cache's half is %d (vkn_shc.h): 0", g_st, vkn_shc_eng_st_mode()); g_st = 0; return 0; }
    g_st_from = vkn_shc_eng_st_from();
    g_stp = nrst_par_default(); g_stp.mode = g_st;
    g_stp.stable = (uint32_t)env_int("DM_VK_ENG_SHSTATIC_STABLE", 8, 1, 10000); g_stp.min_static = (uint32_t)env_int("DM_VK_ENG_SHSTATIC_MIN", 16, 1, 100000);
    g_stp.shift_max = (uint32_t)env_int("DM_VK_ENG_SHSTATIC_SHIFT", 64, 0, 1024); g_stp.trust = (uint32_t)env_int("DM_VK_ENG_SHSTATIC_TRUST", 0, 0, 1);   // ([fix2] 0)
    g_stp.age = (uint32_t)env_int("DM_VK_ENG_SHSTATIC_AGE", 1800, 2, 1000000);
    g_mark_stub = dm_vkn_stack_stub_ex((void *)eh_mark, "ShadowStaticMarker", DM_VKN_SF_ENC);
    for (int k = g_st_from; k < CAS && g_mark_stub; k++) { if (!(g_stk[k] = (st_k *)calloc(1, sizeof(st_k)))) break; nrst_init(&g_stk[k]->c); }
    if (!g_mark_stub || !g_stk[CAS - 1]) {
        vkn_logf(ENG_RED "eng: DM_VK_ENG_SHSTATIC=%d: no %s: 0", g_st, g_mark_stub ? "memory for the cascade tables" : "host-stack stub for the marker command");
        for (int k = 0; k < CAS; k++) { free(g_stk[k]); g_stk[k] = NULL; }
        g_st = 0; return 0;
    }
    g_gath_ret = (uint64_t)(uintptr_t)(dm_vkn_base() + 0xb18234);
    vkn_logf("eng (VK-ENGINE-SIDE I1a): DM_VK_ENG_SHSTATIC=%d (%s) from cascade %d: stable %u frames, min %u, shift %u texels, trust %u, age %u; tables %.1f MB",
             g_st, g_st == 1 ? "static casters left out while their cascade's layer holds" : g_st == 2 ? "verify" : "POISON", g_st_from, g_stp.stable, g_stp.min_static, g_stp.shift_max,
             g_stp.trust, g_stp.age, (CAS - g_st_from) * sizeof(st_k) / 1048576.0);
    return 1;
}
static void st_install(void) {
    // First 4 instructions (capstone; none PC-relative).
    static const uint32_t k_dbuild[4] = {0xd10483ff /* sub sp,sp,#0x120 */, 0xa90c6ffc, 0xa90d67fa, 0xa90e5ff8};
    static const uint32_t k_gath[4] = {0xf81c0ff7 /* str x23,[sp,#-0x40]! */, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};
    dm_vkn_stack_flag((void *)eh_dbuild, DM_VKN_SF_ENC); dm_vkn_stack_flag((void *)eh_gath, DM_VKN_SF_ENC);
    dm_vkn_site(0xb15090, k_dbuild, "ShadowCascade::BuildDraws", (void *)eh_dbuild, &g_o_dbuild, VKN_M_NATIVE);
    dm_vkn_site(0xa87980, k_gath, "CullList::Gather", (void *)eh_gath, &g_o_gath, VKN_M_NATIVE);
}

// ---- the main-view receiver: census; I2 ----
static void main_census(uint64_t stage, uint64_t obj, uint32_t fl) {
    ADD(m_calls, 1);
    for (int i = 0; i < 4; i++) {
        uint64_t s = __atomic_load_n(&g_main_stage[i], __ATOMIC_RELAXED);
        if (s == stage) break;
        if (!s && __atomic_compare_exchange_n(&g_main_stage[i], &s, stage, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { ADD(m_stages, 1); break; }
        if (s == stage) break;
    }
    if (fl & 1u << 19) { ADD(m_hidden, 1); return; }
    if (fl & (1u << 12 | 1u << 13 | 1u << 14)) { ADD(m_special, 1); return; }
    if (fl & 1u << 18) {
        ADD(m_opaque, 1);
        const int st = ot_state(obj, (uint32_t)eng_frame());
        if (st == 2) ADD(m_opaque_st, 1); else if (st == 1) ADD(m_opaque_mv, 1);
    }
    if (fl & (1u << 9 | 1u << 11)) ADD(m_alpha, 1);
    if (fl & 1u << 16) ADD(m_parts, 1);
    if (fl & (0x1fcu | 1u << 10 | 1u << 20)) ADD(m_other, 1);   // bits 2..8, 10, 20: the other passes (decals, custom opaque, refraction)
    if (fl & 1u) ADD(m_batch, 1);
}
// I2: the lists a hidden object may be kept out of - the opaque list only (doc section 2.2): bits 2..14, 16, 19, 20 forbid.
#define OCC_FORBID (0x7ffcu | 1u << 16 | 1u << 19 | 1u << 20)
VKN_HOOK eh_main(VKN_A) {   // 0xb0c570(stage, obj, x2): void
    const uint64_t stage = a0, obj = a1;
    if (!g_occ || !stage || !obj) {
        const uint64_t rc = g_o_main(VKN_F);
        if (g_census && obj) main_census(stage, obj, vkn_rd32(obj + O_FLAGS));
        return rc;
    }
    const uint32_t fl = vkn_rd32(obj + O_FLAGS);
    const uint64_t E = eng_frame();
    float box[6], vp[16]; memcpy(box, (const void *)(uintptr_t)(obj + O_BOX), sizeof box);
    const uint64_t cam = vkn_rd64(stage + ST_CAMERA);
    const float *v = NULL;
    if (cam && !vkn_rd8(cam + CAM_STALE)) { memcpy(vp, (const void *)(uintptr_t)(cam + CAM_VP), sizeof vp); v = vp; }
    // [v2] the filter as a group (counted): characters (the receiver's own test 0xb0c9c4: vtbl[0](obj) has all bits of 0x2003) are never
    // hidden - they move, animate and own the outline / offscreen paths
    int elig = !(fl & 1u << 18) ? VKN_EO_FL_NOTOPAQUE : fl & (1u << 9 | 1u << 11) ? VKN_EO_FL_ALPHA : fl & 1u << 16 ? VKN_EO_FL_PARTS
             : fl & (1u << 12 | 1u << 13 | 1u << 14 | 1u << 19) ? VKN_EO_FL_SPECIAL : fl & OCC_FORBID ? VKN_EO_FL_OTHER : !g_occ_bit0 && (fl & 1u) ? VKN_EO_FL_BIT0 : VKN_EO_FL_OK;
    if (elig == VKN_EO_FL_OK) {
        const uint64_t vt = vkn_rd64(obj), f0 = vt ? vkn_rd64(vt) : 0;
        if (!f0 || (((uint32_t (*)(uint64_t))(uintptr_t)f0)(obj) & 0x2003u) == 0x2003u) elig = VKN_EO_FL_CHARACTER;
    }
    int poison = 0;
    if (E && vkn_eocc_obj(stage, obj, E, v, box, elig, fl, &poison)) {   // hidden: the object is in no list of the stage this frame
        if (g_census) main_census(stage, obj, fl);
        return 0;
    }
    const uint64_t rc = g_o_main(VKN_F);
    const int drew = (fl & 1u << 18) && !(fl & (1u << 19 | 1u << 12 | 1u << 13 | 1u << 14));
    const uint64_t pair = vkn_rd64(obj + O_PAIR_FRAME) == E ? vkn_rd64(obj + O_PAIR) : 0;
    if (E) vkn_eocc_after(stage, obj, E, drew, pair, poison);
    if (g_census) main_census(stage, obj, fl);
    return rc;
}

// ---- census line ----
static void eng_census(void) {
    typedef unsigned long long ull;
    __typeof__(g_c) c; memcpy(&c, &g_c, sizeof c); memset(&g_c, 0, sizeof g_c);   // (racy by a few counts: census only)
    const uint64_t fr_now = eng_frame(), frames = !g_census_frame0 ? c.builds : fr_now > g_census_frame0 ? fr_now - g_census_frame0 : 0;   // (first window: the builds)
    g_census_frame0 = fr_now;
    for (int i = 0; i < 4; i++) __atomic_store_n(&g_main_stage[i], 0, __ATOMIC_RELAXED);
    const double fm = frames ? (double)frames : 1.0, fb = c.builds ? (double)c.builds : 1.0;
    if (g_census) {
        vkn_logf("eng census (last 10 s, %llu engine frames, %llu shadow builds): main view objects a frame %.0f (opaque %.0f [static %.0f, moving %.0f], alpha %.0f, parts %.0f, "
                 "other passes %.0f, special 12-14 %.0f, hidden %.0f, bit 0 %.0f) from %llu stages; shadow casters a frame by cascade %.0f / %.0f / %.0f / %.0f (other shadow stages %.0f), "
                 "static %.0f / %.0f / %.0f / %.0f, moving %.0f / %.0f / %.0f / %.0f, new %.0f / %.0f / %.0f / %.0f, no camera %llu; object table full %llu",
                 (ull)frames, (ull)c.builds, c.m_calls / fm, c.m_opaque / fm, c.m_opaque_st / fm, c.m_opaque_mv / fm, c.m_alpha / fm, c.m_parts / fm, c.m_other / fm, c.m_special / fm,
                 c.m_hidden / fm, c.m_batch / fm, (ull)c.m_stages, c.cas[0] / fb, c.cas[1] / fb, c.cas[2] / fb, c.cas[3] / fb, c.other_stage / fb, c.st[0] / fb, c.st[1] / fb,
                 c.st[2] / fb, c.st[3] / fb, c.mv[0] / fb, c.mv[1] / fb, c.mv[2] / fb, c.mv[3] / fb, c.nw[0] / fb, c.nw[1] / fb, c.nw[2] / fb, c.nw[3] / fb,
                 (ull)(c.nocam[0] + c.nocam[1] + c.nocam[2] + c.nocam[3]), (ull)c.ot_full);
        char b[1000]; size_t n = 0;
        n += (size_t)snprintf(b + n, sizeof b - n, "eng census sizes (caster sphere diameter in texels of its cascade, tile %.0f; classes <0.5 <1 <2 <4 <8 <16 <64 >=64, a frame):", tile_px());
        for (int k = 0; k < CAS && n < sizeof b - 160; k++) {
            if (!c.cas[k]) continue;
            float s; const uint32_t sb = __atomic_load_n(&g_scale_bits[k], __ATOMIC_RELAXED); memcpy(&s, &sb, 4);
            n += (size_t)snprintf(b + n, sizeof b - n, " c%d [texel %.3f world units]", k, s > 0.0f ? 2.0 / ((double)s * tile_px()) : 0.0);
            for (int j = 0; j < SZ; j++) n += (size_t)snprintf(b + n, sizeof b - n, " %.0f", c.sz[k][j] / fb);
            n += (size_t)snprintf(b + n, sizeof b - n, ";");
        }
        vkn_logf("%s", b);
    }
    if (g_rate)
        vkn_logf("eng farrate (DM_VK_ENG_FARRATE=%d, N %d, move %.1f texels): builds %llu, executes %llu; drawn %llu, left out %llu, would %llu; held: first %llu, moved %llu, "
                 "no far caster %llu, fallback %llu; far casters kept out %.0f a build (would %.0f); pairing over %llu under %llu; resolve built after the shadow build of its frame %llu, "
                 "otherwise %llu (session)",
                 g_rate, g_period, g_move, (ull)c.builds, (ull)c.execs, (ull)c.dec_drawn, (ull)c.dec_left, (ull)c.dec_would, (ull)c.held_first, (ull)c.held_move, (ull)c.held_empty,
                 (ull)c.held_bad, c.far_left / fb, c.far_would / fb, (ull)c.fifo_over, (ull)c.fifo_under, (ull)g_res_after, (ull)g_res_other);
    if (g_shmin)
        vkn_logf("eng shmin (DM_VK_ENG_SHMIN=%d, below %.2f texels from cascade %d): casters kept out a build %.0f / %.0f / %.0f / %.0f (would %.0f / %.0f / %.0f / %.0f)",
                 g_shmin, g_shmin == 3 ? g_shmin_px * 16.0f : g_shmin_px, g_shmin_from, c.small_left[0] / fb, c.small_left[1] / fb, c.small_left[2] / fb, c.small_left[3] / fb,
                 c.small_would[0] / fb, c.small_would[1] / fb, c.small_would[2] / fb, c.small_would[3] / fb);
    if (g_census >= 2) vkn_eng_census();
    if (g_st) st_census();   // [eng I1a]
}

static int env_int(const char *n, int def, int lo, int hi) { const char *v = getenv(n); if (!v || !*v) return def; const int x = atoi(v); return x < lo || x > hi ? def : x; }
static float env_f(const char *n, float def) { const char *v = getenv(n); if (!v || !*v) return def; const float x = (float)atof(v); return x >= 0.0f ? x : def; }
void dm_vkn_eng_register(void);
void dm_vkn_eng_register(void) {
    if (vkn_mode() != VKN_NATIVE) return;
    g_census = env_int("DM_VK_ENG_CENSUS", 0, 0, 2);
    g_rate = env_int("DM_VK_ENG_FARRATE", 1, 0, 3);   // [23.11] default 1 by the owner (D07 +11 % walking; clip F07/F08 accepted)
    g_period = env_int("DM_VK_ENG_FARRATE_N", 2, 2, 4);
    g_move = env_f("DM_VK_ENG_FARRATE_MOVE", 8.0f);
    g_shmin = env_int("DM_VK_ENG_SHMIN", 0, 0, 3);
    g_shmin_px = env_f("DM_VK_ENG_SHMIN_PX", 1.0f);
    g_shmin_from = env_int("DM_VK_ENG_SHMIN_FROM", 1, 0, 3);
    g_tile = env_f("DM_VK_ENG_SHTILE", 768.0f); if (g_tile < 64.0f) g_tile = 768.0f;
    g_occ = env_int("DM_VK_ENG_OCC", 0, 0, 3);
    g_occ_bit0 = env_int("DM_VK_ENG_OCC_BIT0", 1, 0, 1);
    const int cond = env_int("DM_VK_NATIVE_COND", 0, -1000, 1000), cond_main = cond == 5 || cond == 6 || (cond >= 10 && cond <= 13);
    if (g_occ && cond_main) { vkn_logf(ENG_RED "eng occ: DM_VK_NATIVE_COND=%d hooks the main-view receiver: DM_VK_ENG_OCC=%d -> 0", cond, g_occ); g_occ = 0; }
    if (g_occ) { vkn_eocc_init(g_occ); g_occ = vkn_eocc_mode(); }
    if (g_rate && vkn_shc_eng_far_mode() != g_rate) {   // (the backend half: SHFAR=1, a chain refusal, a bad value)
        vkn_logf("eng: DM_VK_ENG_FARRATE=%d but the shadow cache's half is %d (vkn_shc.h; DM_VK_NATIVE_SHFAR=1?): 0", g_rate, vkn_shc_eng_far_mode()); g_rate = 0;
    }
    vkn_logf("eng (VK-ENGINE-SIDE stage 1): DM_VK_ENG_CENSUS=%d, DM_VK_ENG_FARRATE=%d (N %d, move %.1f texels: %s), DM_VK_ENG_SHMIN=%d (below %.2f texels from cascade %d: %s)",
             g_census, g_rate, g_period, g_move, g_rate == 1 ? "the far cascade left out of every N-th build but one" : g_rate == 2 ? "count" : g_rate == 3 ? "POISON" : "off",
             g_shmin, g_shmin_px, g_shmin_from, g_shmin == 1 ? "small casters kept out" : g_shmin == 2 ? "count" : g_shmin == 3 ? "POISON x16" : "off");
    if (g_occ) vkn_logf("eng occ: DM_VK_ENG_OCC=%d (I2, VK-ENGINE-SIDE.md section 12), bit-0 objects %s", g_occ, g_occ_bit0 ? "included" : "left out (DM_VK_ENG_OCC_BIT0=0)");
    const int st = st_register();   // [eng I1a]
    if (!g_census && !g_rate && !g_shmin && !g_occ && !st) return;
    if (g_census && !(g_ot = (ot_e *)calloc(OT_N, sizeof *g_ot))) vkn_logf(ENG_RED "eng census: no memory for the object table (static vs moving not counted)");
    if (g_census >= 2) vkn_eng_install();
    // First 4 instructions of each site (capstone; none PC-relative).
    static const uint32_t k_recv[4] = {0xd10103ff /* sub sp,sp,#0x40 */, 0xa9024ff4, 0xa9037bfd, 0x9100c3fd};
    static const uint32_t k_build[4] = {0xd10583ff /* sub sp,sp,#0x160 */, 0x6d0c3bef, 0x6d0d33ed, 0x6d0e2beb};
    static const uint32_t k_exec[4] = {0xf81d0ff5 /* str x21,[sp,#-0x30]! */, 0xa9014ff4, 0xa9027bfd, 0x910083fd};
    static const uint32_t k_main[4] = {0xd10203ff /* sub sp,sp,#0x80 */, 0xa9026ffc, 0xa90367fa, 0xa9045ff8};
    dm_vkn_stack_flag((void *)eh_recv, DM_VKN_SF_ENC); dm_vkn_stack_flag((void *)eh_build, DM_VKN_SF_ENC);
    dm_vkn_stack_flag((void *)eh_exec, DM_VKN_SF_ENC); dm_vkn_stack_flag((void *)eh_main, DM_VKN_SF_ENC);
    if (g_census || g_rate || g_shmin)   // ([eng I1a] alone needs only the build: it names the cascades' cull stages)
        dm_vkn_site(0xb17ed0, k_recv, "ShadowCullReceiver", (void *)eh_recv, &g_o_recv, VKN_M_NATIVE);
    if (g_census || g_rate || g_shmin || st) {
        dm_vkn_site(0xb15ae0, k_build, "ShadowStage::Build", (void *)eh_build, &g_o_build, VKN_M_NATIVE);
        dm_vkn_site(0xb164d0, k_exec, "ShadowStage::Execute", (void *)eh_exec, &g_o_exec, VKN_M_NATIVE);
        static const uint32_t k_res[4] = {0xa9ba6ffc /* stp x28,x27,[sp,#-0x60]! */, 0xa90167fa, 0xa9025ff8, 0xa90357f6};
        dm_vkn_stack_flag((void *)eh_res, DM_VKN_SF_ENC);
        dm_vkn_site(0xac8d10, k_res, "ShadowRenderStage::Build", (void *)eh_res, &g_o_res, VKN_M_NATIVE);
    }
    if (g_census || g_occ) {   // the main-view receiver, unless technique A / Z (hle_dos2_vkn_occ.c) hooks it
        if (cond_main) vkn_logf("eng census: DM_VK_NATIVE_COND=%d hooks the main-view receiver: its census is not counted", cond);
        else dm_vkn_site(0xb0c570, k_main, g_occ ? "ObjectGather(occ)" : "ObjectGather(census)", (void *)eh_main, &g_o_main, VKN_M_NATIVE);
    }
    if (g_occ) dm_vkn_census(vkn_eocc_census);
    if (st) st_install();   // [eng I1a]
    g_census_frame0 = 0;
    dm_vkn_census(eng_census);
}
#endif
