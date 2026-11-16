// [DOS2] NB-18b-2: the UI / post-tail audit (DM_NB_UIPASS=2 / DM_NB_POST=2; docs/dos2/NATIVE_BACKEND_PLAN.md "NB-18a findings" п. 4).
// Pure C, no backend types: the backend (dm/gfx/vulkan/engine/xbg_nb_ui.inc) fills one nb_ui_pass per run of draws into the same render
// target set (the guest's pass, not the host's Vulkan pass) and calls nb_ui_analyze at the frame's CombineUI* draw. Host test:
// dm/gfx/vulkan/engine/tests/nb_ui_test.cpp (synthetic P2 frames: campaign f=2509, menu f=835, AA off).
// Image ids = texture storage addresses (tex_t::mem), as the NB IR (nbchk::key) - a sampled view and a render-target view of the same
// memory are the same image.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Fragment shader classes by key prefix (work/shaders-vk/<key>.xgs; the keys of NB-18a's table)
enum { NB_UISH_OTHER = 0, NB_UISH_LUT, NB_UISH_SIGHT, NB_UISH_FXAA, NB_UISH_PPDEF, NB_UISH_VIG, NB_UISH_COMB_NOFETCH, NB_UISH_COMB,
       NB_UISH_COMB_BIC, NB_UISH_COMB_HDR, NB_UISH_IGGY, NB_UISH_IGGY_FILL, NB_UISH_IGGY_BLUR, NB_UISH_N };
static const char *const nb_ui_sh_name[NB_UISH_N] = {"other", "LUT", "Sight", "FXAA", "PPDefault", "Vignette", "CombineUI_NoFetch", "CombineUI",
                                                     "CombineUI_BicubicFilter", "CombineUI_HDR", "iggy", "iggy-fill", "iggy-blur"};
static inline int nb_ui_shader_class(const char *key) {
    static const struct { const char *k; int c; } t[] = {
        {"921728a5", NB_UISH_LUT}, {"91eb2d97", NB_UISH_SIGHT}, {"691768aa", NB_UISH_FXAA}, {"7b3210191abd", NB_UISH_PPDEF},
        {"43e8e8ed", NB_UISH_VIG}, {"98186c22", NB_UISH_COMB_NOFETCH}, {"2ec6c821", NB_UISH_COMB}, {"a0770357", NB_UISH_COMB_BIC},
        {"c0477a5e", NB_UISH_COMB_HDR}, {"d99f1fdb", NB_UISH_IGGY}, {"b5afd82f", NB_UISH_IGGY_FILL}, {"5e26ce73", NB_UISH_IGGY_BLUR}};
    if (!key) return NB_UISH_OTHER;
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) if (!strncmp(key, t[i].k, strlen(t[i].k))) return t[i].c;
    return NB_UISH_OTHER;
}
static inline int nb_ui_is_combine(int c) { return c >= NB_UISH_COMB_NOFETCH && c <= NB_UISH_COMB_HDR; }

// Blend of render target 0 (NVN codes: factor ZERO 1, ONE 2, SRC_ALPHA 5, ONE_MINUS_SRC_ALPHA 6, DST_ALPHA 7, 1-DST_ALPHA 8, DST_COLOR 9,
// 1-DST_COLOR 10; equation ADD 1; mask r1 g2 b4 a8). "Drawing the UI straight into the final pass equals CombineUI" (plan п. 3b) holds for
// OVER (rgb ONE|SRC_ALPHA / 1-SRC_ALPHA, alpha ONE / 1-SRC_ALPHA, all ADD) and NOCOLOR; ADD_KEEPA (additive rgb, alpha kept) differs where
// the UI alpha stays < 0.001 (CombineUI then shows the 3D only); every other class breaks it.
enum { NB_UIB_OVER = 0, NB_UIB_NOCOLOR, NB_UIB_ADD_KEEPA, NB_UIB_OFF, NB_UIB_ADD_A, NB_UIB_DST, NB_UIB_NOALPHA, NB_UIB_OTHER, NB_UIB_N };
static const char *const nb_ui_blend_name[NB_UIB_N] = {"over", "no-color", "add(a kept)", "opaque", "add(a written)", "dst-factor",
                                                       "over(a not written)", "other"};
static inline int nb_ui_blend_class(int en, unsigned sr, unsigned dr, unsigned sa, unsigned da, unsigned er, unsigned ea, unsigned mask) {
    mask &= 15;
    if (!mask) return NB_UIB_NOCOLOR;
    if (!en) return mask == 15 ? NB_UIB_OFF : NB_UIB_OTHER;
#define NB_UI_DSTF(f) ((f) >= 7 && (f) <= 10)
    if (NB_UI_DSTF(sr) || NB_UI_DSTF(dr) || ((mask & 8) && (NB_UI_DSTF(sa) || NB_UI_DSTF(da)))) return NB_UIB_DST;
#undef NB_UI_DSTF
    const int over_rgb = er == 1 && dr == 6 && (sr == 2 || sr == 5), add_rgb = er == 1 && dr == 2;
    if (!(mask & 8)) return over_rgb ? NB_UIB_NOALPHA : add_rgb ? NB_UIB_ADD_KEEPA : NB_UIB_OTHER;
    if (over_rgb && ea == 1 && sa == 2 && da == 6) return NB_UIB_OVER;
    if (add_rgb) return ea == 1 && sa == 1 && da == 2 ? NB_UIB_ADD_KEEPA : NB_UIB_ADD_A;
    return NB_UIB_OTHER;
}
static inline int nb_ui_blend_exact(int c) { return c == NB_UIB_OVER || c == NB_UIB_NOCOLOR; }

#define NB_UI_MAXP 256
#define NB_UI_RD 12
typedef struct {
    uint64_t color[8], depth; uint32_t ncolor, w, h;   // the render target set (w x h of colour 0, else depth)
    uint32_t draws; int sh;                             // sh: class of the first draw's fragment shader
    uint32_t sh_mask;                                   // classes of all its draws (1 << class)
    uint32_t blend[NB_UIB_N];                           // draws per blend class (target 0)
    uint8_t dt, dw, st, sw;                             // any draw: depth test / depth write / stencil test / stencil write
    int32_t x0, y0, x1, y1;                             // union of scissor ∩ viewport ∩ target over its draws (x1 <= x0: none)
    uint64_t rd[NB_UI_RD]; uint32_t nrd; uint8_t rd_over;   // images its fragment shaders sampled (rd_over: more than NB_UI_RD)
} nb_ui_pass;
static inline void nb_ui_pass_init(nb_ui_pass *p) { memset(p, 0, sizeof *p); p->sh = -1; p->x0 = p->y0 = INT32_MAX; p->x1 = p->y1 = INT32_MIN; }
static inline void nb_ui_add_read(nb_ui_pass *p, uint64_t im) {
    if (!im) return;
    for (uint32_t i = 0; i < p->nrd; i++) if (p->rd[i] == im) return;
    if (p->nrd < NB_UI_RD) p->rd[p->nrd++] = im; else p->rd_over = 1;
}
static inline void nb_ui_add_rect(nb_ui_pass *p, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (x1 <= x0 || y1 <= y0) return;
    if (x0 < p->x0) p->x0 = x0; if (y0 < p->y0) p->y0 = y0; if (x1 > p->x1) p->x1 = x1; if (y1 > p->y1) p->y1 = y1;
}
static inline int nb_ui_writes(const nb_ui_pass *p, uint64_t im) {
    for (uint32_t k = 0; k < p->ncolor && k < 8; k++) if (p->color[k] == im) return 1;
    return 0;
}
static inline int nb_ui_reads(const nb_ui_pass *p, uint64_t im) {
    for (uint32_t i = 0; i < p->nrd; i++) if (p->rd[i] == im) return 1;
    return 0;
}

typedef struct { uint64_t ui, base; int have_c5; float vp_off[2], params[4]; } nb_ui_comb;   // the CombineUI draw: Tex (vk 10), Base (vk 16), c5

typedef struct {
    // UI segment: from the first pass drawing into the combine's Tex to the last one before the combine
    const char *ui_why;   // NULL = segment found
    int first, last, comb;
    uint32_t ui_passes, ui_draws, flt_passes, flt_draws, chains, flt_rts, rt_reused, between;   // between: passes after the segment, before the combine
    uint32_t blend_ui[NB_UIB_N], blend_flt[NB_UIB_N];
    uint32_t ds_att, ds_used, dt, dw, st, sw;   // segment passes with a depth/stencil attachment; using it at all; per kind
    uint32_t flt_ds_unused;                     // filter passes with a depth/stencil attachment no draw tests or writes
    uint64_t flt_area, flt_rt_area; uint32_t flt_small, flt_noin;   // filter passes: union area / target area, < 25 % of it, no sampled input
    uint32_t ui_readers;                        // passes other than the combine reading Tex
    int params_one;                             // c5 Params.y == 1 (-1: no c5)
    // post tail (walking back from the combine's Base): [LUT] Sight [FXAA|PPDefault] [Vignette] Combine
    const char *post_why; int post_bad_sh, post_bad_pass; char chain[96]; uint32_t tail_n; int tail[6];
} nb_ui_res;

static inline int nb_ui_last_writer(const nb_ui_pass *p, int before, uint64_t im) {
    for (int j = before - 1; j >= 0; j--) if (nb_ui_writes(&p[j], im)) return j;
    return -1;
}
// readers of the image written by pass w in (w, next write), other than pass r
static inline int nb_ui_other_reader(const nb_ui_pass *p, int n, int w, int r, uint64_t im) {
    for (int q = w + 1; q < n; q++) {
        if (q != r && nb_ui_reads(&p[q], im)) return q;
        if (q != w && nb_ui_writes(&p[q], im)) break;
    }
    return -1;
}

static inline void nb_ui_analyze(const nb_ui_pass *p, int n, const nb_ui_comb *c, nb_ui_res *r) {
    memset(r, 0, sizeof *r); r->first = r->last = r->post_bad_pass = -1; r->comb = n - 1; r->params_one = -1;
    if (c->have_c5) r->params_one = c->params[1] == 1.0f;
    const int cb = n - 1;
    // ---- UI segment
    if (!c->ui) r->ui_why = "no Tex bound";
    else {
        for (int j = 0; j < cb; j++) if (nb_ui_writes(&p[j], c->ui)) { if (r->first < 0) r->first = j; r->last = j; }
        if (r->first < 0) r->ui_why = "Tex not drawn this frame";
    }
    for (int j = 0; j < cb; j++) if (c->ui && nb_ui_reads(&p[j], c->ui)) r->ui_readers++;
    if (!r->ui_why) {
        int in_chain = 0, chain_of[NB_UI_MAXP]; uint64_t rts[64]; int rt_chain[64]; uint32_t nrt = 0;
        for (int j = r->first; j <= r->last; j++) {
            const nb_ui_pass *q = &p[j]; const int ui = nb_ui_writes(q, c->ui);
            chain_of[j] = 0;
            if (ui) { r->ui_passes++; r->ui_draws += q->draws; in_chain = 0; for (int b = 0; b < NB_UIB_N; b++) r->blend_ui[b] += q->blend[b]; }
            else {
                if (!in_chain) { in_chain = 1; r->chains++; }
                chain_of[j] = (int)r->chains;
                r->flt_passes++; r->flt_draws += q->draws; for (int b = 0; b < NB_UIB_N; b++) r->blend_flt[b] += q->blend[b];
                const uint64_t ta = (uint64_t)q->w * q->h;
                uint64_t ua = q->x1 > q->x0 && q->y1 > q->y0 ? (uint64_t)(q->x1 - q->x0) * (uint64_t)(q->y1 - q->y0) : 0;
                if (ua > ta) ua = ta;
                r->flt_area += ua; r->flt_rt_area += ta; if (ua * 4 < ta) r->flt_small++;
                if (!q->nrd) r->flt_noin++;
                for (uint32_t k = 0; k < q->ncolor && k < 8; k++) {
                    uint32_t i = 0; while (i < nrt && rts[i] != q->color[k]) i++;
                    if (i == nrt) { if (nrt < 64) { rts[nrt] = q->color[k]; rt_chain[nrt] = chain_of[j]; nrt++; } }
                    else if (rt_chain[i] != chain_of[j] && rt_chain[i] > 0) { r->rt_reused++; rt_chain[i] = -1; }   // counted once per image
                }
            }
            if (q->depth) {
                const int used = q->dt || q->dw || q->st || q->sw;
                r->ds_att++; r->ds_used += (uint32_t)used; r->dt += q->dt; r->dw += q->dw; r->st += q->st; r->sw += q->sw;
                if (!ui && !used) r->flt_ds_unused++;
            }
        }
        r->flt_rts = nrt; r->between = (uint32_t)(cb - 1 - r->last);
    }
    // ---- post tail
    static const int order[5] = {NB_UISH_VIG, NB_UISH_FXAA, NB_UISH_SIGHT, NB_UISH_LUT, -1};   // backwards; FXAA stands for FXAA | PPDefault
    int cur = cb; uint64_t in = c->base; int step = 0, k = 0, tail[6]; tail[k++] = cb;
    if (!c->base) r->post_why = "no Base bound";
    while (!r->post_why) {
        const int w = nb_ui_last_writer(p, cur, in);
        if (w < 0) { r->post_why = step == 0 ? "Base not drawn this frame" : "input of a tail pass not drawn this frame"; break; }
        const int s = p[w].sh, cls = s == NB_UISH_PPDEF ? NB_UISH_FXAA : s;
        while (order[step] >= 0 && order[step] != cls && order[step] != NB_UISH_SIGHT && order[step] != NB_UISH_LUT) step++;   // optional Vignette / FXAA
        if (order[step] != cls) { r->post_why = "unknown shader in the tail"; r->post_bad_sh = s; r->post_bad_pass = w; break; }
        if (p[w].ncolor != 1) { r->post_why = "tail pass with several targets"; break; }
        if (p[w].w != p[cur].w || p[w].h != p[cur].h) {
            if (cur != cb) { r->post_why = "tail sizes differ"; break; }   // the combine upscales 3D to the window: only it may differ
        }
        if (nb_ui_other_reader(p, cb, w, cur, in) >= 0) { r->post_why = "intermediate read by another pass"; break; }
        if (k < 6) tail[k++] = w;
        if (cls == NB_UISH_LUT) break;
        // its main input: the read written by the nearest earlier pass
        int best = -1; uint64_t bi = 0;
        for (uint32_t i = 0; i < p[w].nrd; i++) { const int ww = nb_ui_last_writer(p, w, p[w].rd[i]); if (ww > best) { best = ww; bi = p[w].rd[i]; } }
        if (best < 0) { r->post_why = "tail pass without a drawn input"; break; }
        cur = w; in = bi; step++;
    }
    r->tail_n = (uint32_t)k;
    for (int i = 0; i < k; i++) r->tail[i] = tail[i];
    int o = 0; r->chain[0] = 0;
    for (int i = k - 1; i >= 0 && o < (int)sizeof r->chain - 24; i--) {
        const int s = p[tail[i]].sh;
        o += snprintf(r->chain + o, sizeof r->chain - (size_t)o, "%s%s", o ? ">" : "", s >= 0 && s < NB_UISH_N ? nb_ui_sh_name[s] : "?");
    }
    if (!r->post_why && p[tail[k - 1]].sh != NB_UISH_LUT) r->post_why = "no LUT at the head";
}
