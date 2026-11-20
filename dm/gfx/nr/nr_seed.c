// [DOS2] NR-31: the portable pipeline warm-up seed (nr_seed.h). Pure C.
#include "nr_seed.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Vulkan numbers the expander uses (the host test checks them against vulkan_core.h).
enum {
    VKS_DT_UBO = 6, VKS_DT_SSBO = 7, VKS_DT_UBO_DYN = 8, VKS_DT_SSBO_DYN = 9,
    VKS_LAYOUT_GENERAL = 1, VKS_LOAD_LOAD = 0, VKS_STORE_STORE = 0,
    VKS_ST_VERTEX = 0x1, VKS_ST_TCS = 0x2, VKS_ST_TES = 0x4, VKS_ST_GEOM = 0x8, VKS_ST_FRAG = 0x10,
    VKS_F_D16 = 124, VKS_F_X8D24 = 125, VKS_F_D32 = 126, VKS_F_S8 = 127, VKS_F_D16S8 = 128, VKS_F_D24S8 = 129, VKS_F_D32S8 = 130,
    VKS_LINE_WIDTH_1 = 0x3f800000,
};
#define VKS_SUBPASS_EXTERNAL 0xffffffffu
#define VKS_PS_FRAG_SHADER 0x80u
#define VKS_PS_EARLY_FRAG 0x100u
#define VKS_PS_LATE_FRAG 0x200u
#define VKS_PS_COLOR_OUT 0x400u
#define VKS_PS_TRANSFER 0x1000u
#define VKS_PS_ALL_GRAPHICS 0x8000u
#define VKS_PS_ALL_COMMANDS 0x10000u
#define VKS_AC_CA_READ 0x80u
#define VKS_AC_CA_WRITE 0x100u
#define VKS_AC_DS_READ 0x200u
#define VKS_AC_DS_WRITE 0x400u
#define VKS_AC_MEM_READ 0x8000u
#define VKS_AC_MEM_WRITE 0x10000u
#define VKS_TILE_FRAG (VKS_PS_EARLY_FRAG | VKS_PS_LATE_FRAG | VKS_PS_FRAG_SHADER | VKS_PS_COLOR_OUT)

static const char *const reason_names[NSD_E_COUNT] = {
    "ok", "parse", "big", "shape", "key", "module-id", "variant", "shader", "facts", "layout", "descriptor-limit", "depth-ambiguous",
    "attachment-format", "vertex-format", "limit", "noout", "dependencies", "policy", "vertex-ambiguous", "split" };
const char *nsd_reason_name(int r) { return r >= 0 && r < NSD_E_COUNT ? reason_names[r] : "?"; }

uint64_t nsd_fnv(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ull; }
    return h | (1ull << 63);
}
uint64_t nsd_fnv64(const uint8_t *b, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

// ---- reader / writer ----
typedef struct { const uint32_t *w; size_t n, i; int bad; size_t watch; const char *hit; } rd;
static uint32_t U(rd *r, const char *name) {
    if (r->i >= r->n) { r->bad = 1; return 0; }
    if (r->i == r->watch && !r->hit) r->hit = name;
    return r->w[r->i++];
}
static uint64_t Q(rd *r, const char *name) { uint64_t lo = U(r, name); return lo | ((uint64_t)U(r, name) << 32); }

static int parse_layout(rd *r, size_t end, nsd_pipe *p) {
    p->l_flags = U(r, "layout.set.flags"); p->nlb = U(r, "layout.set.bindings");
    if (r->bad) return NSD_E_PARSE;
    if (p->nlb > 4096) return NSD_E_PARSE;
    if (p->nlb > NSD_MAX_LB) return NSD_E_BIG;
    for (uint32_t i = 0; i < p->nlb; i++) {
        nsd_lb *b = &p->lb[i];
        b->binding = U(r, "layout.binding"); b->type = U(r, "layout.binding.type"); b->count = U(r, "layout.binding.count");
        b->stages = U(r, "layout.binding.stages");
    }
    p->pl_flags = U(r, "layout.flags"); p->sets = U(r, "layout.sets"); p->npush = U(r, "layout.push");
    if (r->bad) return NSD_E_PARSE;
    if (p->npush > NSD_MAX_PUSH) return NSD_E_PARSE;
    for (uint32_t i = 0; i < p->npush; i++) { p->push[i][0] = U(r, "layout.push.stages"); p->push[i][1] = U(r, "layout.push.offset"); p->push[i][2] = U(r, "layout.push.size"); }
    if (r->bad || r->i != end) return NSD_E_PARSE;
    return NSD_OK;
}
static int parse_pass(rd *r, size_t end, nsd_pipe *p) {
    p->r_flags = U(r, "pass.flags"); p->natt = U(r, "pass.attachments");
    if (r->bad || p->natt > NSD_MAX_ATT) return NSD_E_PARSE;
    for (uint32_t i = 0; i < p->natt; i++) {
        nsd_att *a = &p->att[i];
        a->flags = U(r, "pass.att.flags"); a->format = U(r, "pass.att.format"); a->samples = U(r, "pass.att.samples"); a->load = U(r, "pass.att.load");
        a->store = U(r, "pass.att.store"); a->sload = U(r, "pass.att.stencil_load"); a->sstore = U(r, "pass.att.stencil_store");
        a->init = U(r, "pass.att.initial_layout"); a->fin = U(r, "pass.att.final_layout");
    }
    p->s_flags = U(r, "pass.subpass.flags"); p->s_bind = U(r, "pass.subpass.bind_point"); p->ncol = U(r, "pass.colors");
    if (r->bad || p->ncol > NSD_MAX_COL) return NSD_E_PARSE;
    for (uint32_t i = 0; i < p->ncol; i++) { p->col[i].att = U(r, "pass.color.attachment"); p->col[i].layout = U(r, "pass.color.layout"); }
    p->has_depth = U(r, "pass.has_depth") != 0;
    if (p->has_depth) { p->depth.att = U(r, "pass.depth.attachment"); p->depth.layout = U(r, "pass.depth.layout"); } else p->depth.att = p->depth.layout = 0;
    p->ndep = U(r, "pass.dependencies");
    if (r->bad || p->ndep > NSD_MAX_DEP) return NSD_E_PARSE;
    for (uint32_t i = 0; i < p->ndep; i++) {
        nsd_dep *d = &p->dep[i];
        d->src = U(r, "pass.dep.src"); d->dst = U(r, "pass.dep.dst"); d->src_stage = U(r, "pass.dep.src_stage"); d->dst_stage = U(r, "pass.dep.dst_stage");
        d->src_access = U(r, "pass.dep.src_access"); d->dst_access = U(r, "pass.dep.dst_access"); d->flags = U(r, "pass.dep.flags");
    }
    if (r->bad || r->i != end) return NSD_E_PARSE;
    return NSD_OK;
}
static void stencil_rd(rd *r, nsd_stencil *s) {
    s->fail = U(r, "ds.stencil.fail"); s->pass = U(r, "ds.stencil.pass"); s->dfail = U(r, "ds.stencil.depth_fail"); s->cmp = U(r, "ds.stencil.compare");
    s->cmask = U(r, "ds.stencil.compare_mask"); s->wmask = U(r, "ds.stencil.write_mask"); s->ref = U(r, "ds.stencil.reference");
}
static int parse_r(rd *r, nsd_pipe *p) {
    memset(p, 0, offsetof(nsd_pipe, lb));
    if (U(r, "version") != 1 || r->bad) return NSD_E_PARSE;
    uint32_t ln = U(r, "layout.words");
    if (r->bad || r->i + ln > r->n) return NSD_E_PARSE;
    int e = parse_layout(r, r->i + ln, p); if (e) return e;
    uint32_t pn = U(r, "pass.words");
    if (r->bad || r->i + pn > r->n) return NSD_E_PARSE;
    e = parse_pass(r, r->i + pn, p); if (e) return e;
    p->flags = U(r, "ci.flags"); p->subpass = U(r, "ci.subpass"); p->nst = U(r, "ci.stages");
    if (r->bad || p->nst > NSD_STAGES) return NSD_E_PARSE;
    for (uint32_t i = 0; i < p->nst; i++) { p->st[i].flags = U(r, "stage.flags"); p->st[i].stage = U(r, "stage.stage"); p->st[i].id = Q(r, "stage.module"); }
    p->vi_flags = U(r, "vi.flags"); p->nvb = U(r, "vi.bindings");
    if (r->bad || p->nvb > NSD_MAX_VB) return NSD_E_PARSE;
    for (uint32_t i = 0; i < p->nvb; i++) { p->vb[i][0] = U(r, "vi.binding"); p->vb[i][1] = U(r, "vi.binding.stride"); p->vb[i][2] = U(r, "vi.binding.rate"); }
    p->nva = U(r, "vi.attributes");
    if (r->bad || p->nva > NSD_MAX_VA) return NSD_E_PARSE;
    for (uint32_t i = 0; i < p->nva; i++) { p->va[i][0] = U(r, "vi.attr.location"); p->va[i][1] = U(r, "vi.attr.binding"); p->va[i][2] = U(r, "vi.attr.format"); p->va[i][3] = U(r, "vi.attr.offset"); }
    p->ia_flags = U(r, "ia.flags"); p->topology = U(r, "ia.topology"); p->restart = U(r, "ia.restart");
    p->has_tess = U(r, "tess.present") != 0;
    if (p->has_tess) { p->t_flags = U(r, "tess.flags"); p->patch = U(r, "tess.patch"); }
    p->vp_flags = U(r, "vp.flags"); p->nvp = U(r, "vp.viewports"); p->nsc = U(r, "vp.scissors"); p->has_vp = U(r, "vp.static_viewports") != 0; p->has_sc = U(r, "vp.static_scissors") != 0;
    if (r->bad || p->nvp > NSD_MAX_VP || p->nsc > NSD_MAX_VP) return NSD_E_PARSE;
    if (p->has_vp) for (uint32_t i = 0; i < p->nvp; i++) for (int k = 0; k < 6; k++) p->vps[i][k] = U(r, "vp.viewport");
    if (p->has_sc) for (uint32_t i = 0; i < p->nsc; i++) for (int k = 0; k < 4; k++) p->scs[i][k] = U(r, "vp.scissor");
    p->rs_flags = U(r, "rs.flags"); p->clamp = U(r, "rs.depth_clamp"); p->discard = U(r, "rs.discard"); p->poly = U(r, "rs.polygon_mode"); p->cull = U(r, "rs.cull");
    p->front = U(r, "rs.front_face"); p->bias_en = U(r, "rs.depth_bias"); p->bias_const = U(r, "rs.depth_bias_constant"); p->bias_clamp = U(r, "rs.depth_bias_clamp");
    p->bias_slope = U(r, "rs.depth_bias_slope"); p->line_width = U(r, "rs.line_width");
    p->ms_flags = U(r, "ms.flags"); p->samples = U(r, "ms.samples"); p->sshading = U(r, "ms.sample_shading"); p->min_sshading = U(r, "ms.min_sample_shading");
    p->a2c = U(r, "ms.alpha_to_coverage"); p->a2one = U(r, "ms.alpha_to_one"); p->has_mask = U(r, "ms.sample_mask") != 0;
    if (r->bad) return NSD_E_PARSE;
    if (p->has_mask) { if (p->samples > 64) return NSD_E_PARSE; for (uint32_t i = 0; i < (p->samples + 31) / 32; i++) p->mask[i] = U(r, "ms.sample_mask.word"); }
    p->ds_flags = U(r, "ds.flags"); p->dt = U(r, "ds.depth_test"); p->dw = U(r, "ds.depth_write"); p->dcmp = U(r, "ds.depth_compare"); p->dbounds = U(r, "ds.depth_bounds");
    p->st_en = U(r, "ds.stencil_test"); stencil_rd(r, &p->sf); stencil_rd(r, &p->sb); p->db_min = U(r, "ds.min_depth_bounds"); p->db_max = U(r, "ds.max_depth_bounds");
    p->cb_flags = U(r, "cb.flags"); p->logic_en = U(r, "cb.logic_op_enable"); p->logic_op = U(r, "cb.logic_op"); p->nblend = U(r, "cb.attachments");
    if (r->bad || p->nblend > NSD_MAX_BLEND) return NSD_E_PARSE;
    for (uint32_t i = 0; i < p->nblend; i++) {
        nsd_blend *b = &p->blend[i];
        b->enable = U(r, "cb.blend_enable"); b->src_c = U(r, "cb.src_color"); b->dst_c = U(r, "cb.dst_color"); b->op_c = U(r, "cb.color_op");
        b->src_a = U(r, "cb.src_alpha"); b->dst_a = U(r, "cb.dst_alpha"); b->op_a = U(r, "cb.alpha_op"); b->mask = U(r, "cb.write_mask");
    }
    for (int i = 0; i < 4; i++) p->bconst[i] = U(r, "cb.blend_constants");
    p->dy_flags = U(r, "dy.flags"); p->ndyn = U(r, "dy.states");
    if (r->bad || p->ndyn > NSD_MAX_DYN) return NSD_E_PARSE;
    for (uint32_t i = 0; i < p->ndyn; i++) p->dyn[i] = U(r, "dy.state");
    if (r->bad || r->i != r->n) return NSD_E_PARSE;
    return NSD_OK;
}
int nsd_parse(const uint32_t *w, size_t n, nsd_pipe *p) {
    rd r = {w, n, 0, 0, (size_t)-1, NULL};
    return parse_r(&r, p);
}
const char *nsd_field_at(const uint32_t *w, size_t n, size_t at) {
    nsd_pipe *scratch = (nsd_pipe *)malloc(sizeof *scratch);   // diagnostics only
    if (!scratch) return "?";
    rd r = {w, n, 0, 0, at, NULL};
    parse_r(&r, scratch);
    free(scratch);
    return r.hit ? r.hit : "?";
}

typedef struct { uint32_t *w; size_t cap, n; int full; } wr;
static void P(wr *o, uint32_t v) { if (o->n < o->cap) o->w[o->n++] = v; else o->full = 1; }
size_t nsd_emit(const nsd_pipe *p, uint32_t *w, size_t cap) {
    wr o = {w, cap, 0, 0};
    P(&o, 1);
    size_t at = o.n; P(&o, 0);
    P(&o, p->l_flags); P(&o, p->nlb);
    for (uint32_t i = 0; i < p->nlb; i++) { P(&o, p->lb[i].binding); P(&o, p->lb[i].type); P(&o, p->lb[i].count); P(&o, p->lb[i].stages); }
    P(&o, p->pl_flags); P(&o, p->sets); P(&o, p->npush);
    for (uint32_t i = 0; i < p->npush; i++) for (int k = 0; k < 3; k++) P(&o, p->push[i][k]);
    if (!o.full) w[at] = (uint32_t)(o.n - at - 1);
    at = o.n; P(&o, 0);
    P(&o, p->r_flags); P(&o, p->natt);
    for (uint32_t i = 0; i < p->natt; i++) {
        const nsd_att *a = &p->att[i];
        P(&o, a->flags); P(&o, a->format); P(&o, a->samples); P(&o, a->load); P(&o, a->store); P(&o, a->sload); P(&o, a->sstore); P(&o, a->init); P(&o, a->fin);
    }
    P(&o, p->s_flags); P(&o, p->s_bind); P(&o, p->ncol);
    for (uint32_t i = 0; i < p->ncol; i++) { P(&o, p->col[i].att); P(&o, p->col[i].layout); }
    P(&o, p->has_depth); if (p->has_depth) { P(&o, p->depth.att); P(&o, p->depth.layout); }
    P(&o, p->ndep);
    for (uint32_t i = 0; i < p->ndep; i++) {
        const nsd_dep *d = &p->dep[i];
        P(&o, d->src); P(&o, d->dst); P(&o, d->src_stage); P(&o, d->dst_stage); P(&o, d->src_access); P(&o, d->dst_access); P(&o, d->flags);
    }
    if (!o.full) w[at] = (uint32_t)(o.n - at - 1);
    P(&o, p->flags); P(&o, p->subpass); P(&o, p->nst);
    for (uint32_t i = 0; i < p->nst; i++) { P(&o, p->st[i].flags); P(&o, p->st[i].stage); P(&o, (uint32_t)p->st[i].id); P(&o, (uint32_t)(p->st[i].id >> 32)); }
    P(&o, p->vi_flags); P(&o, p->nvb);
    for (uint32_t i = 0; i < p->nvb; i++) for (int k = 0; k < 3; k++) P(&o, p->vb[i][k]);
    P(&o, p->nva);
    for (uint32_t i = 0; i < p->nva; i++) for (int k = 0; k < 4; k++) P(&o, p->va[i][k]);
    P(&o, p->ia_flags); P(&o, p->topology); P(&o, p->restart);
    P(&o, p->has_tess); if (p->has_tess) { P(&o, p->t_flags); P(&o, p->patch); }
    P(&o, p->vp_flags); P(&o, p->nvp); P(&o, p->nsc); P(&o, p->has_vp); P(&o, p->has_sc);
    if (p->has_vp) for (uint32_t i = 0; i < p->nvp; i++) for (int k = 0; k < 6; k++) P(&o, p->vps[i][k]);
    if (p->has_sc) for (uint32_t i = 0; i < p->nsc; i++) for (int k = 0; k < 4; k++) P(&o, p->scs[i][k]);
    P(&o, p->rs_flags); P(&o, p->clamp); P(&o, p->discard); P(&o, p->poly); P(&o, p->cull); P(&o, p->front); P(&o, p->bias_en); P(&o, p->bias_const);
    P(&o, p->bias_clamp); P(&o, p->bias_slope); P(&o, p->line_width);
    P(&o, p->ms_flags); P(&o, p->samples); P(&o, p->sshading); P(&o, p->min_sshading); P(&o, p->a2c); P(&o, p->a2one); P(&o, p->has_mask);
    if (p->has_mask) for (uint32_t i = 0; i < (p->samples + 31) / 32 && i < 2; i++) P(&o, p->mask[i]);
    P(&o, p->ds_flags); P(&o, p->dt); P(&o, p->dw); P(&o, p->dcmp); P(&o, p->dbounds); P(&o, p->st_en);
    const nsd_stencil *ss[2] = {&p->sf, &p->sb};
    for (int k = 0; k < 2; k++) { P(&o, ss[k]->fail); P(&o, ss[k]->pass); P(&o, ss[k]->dfail); P(&o, ss[k]->cmp); P(&o, ss[k]->cmask); P(&o, ss[k]->wmask); P(&o, ss[k]->ref); }
    P(&o, p->db_min); P(&o, p->db_max);
    P(&o, p->cb_flags); P(&o, p->logic_en); P(&o, p->logic_op); P(&o, p->nblend);
    for (uint32_t i = 0; i < p->nblend; i++) {
        const nsd_blend *b = &p->blend[i];
        P(&o, b->enable); P(&o, b->src_c); P(&o, b->dst_c); P(&o, b->op_c); P(&o, b->src_a); P(&o, b->dst_a); P(&o, b->op_a); P(&o, b->mask);
    }
    for (int i = 0; i < 4; i++) P(&o, p->bconst[i]);
    P(&o, p->dy_flags); P(&o, p->ndyn);
    for (uint32_t i = 0; i < p->ndyn; i++) P(&o, p->dyn[i]);
    return o.full ? 0 : o.n;
}

// ---- the shapes draw() produces ----
static int is_samples(uint32_t s) { return s == 1 || s == 2 || s == 4 || s == 8 || s == 16 || s == 32 || s == 64; }
static int stage_index(uint32_t bit) {
    switch (bit) { case VKS_ST_VERTEX: return 0; case VKS_ST_TCS: return 1; case VKS_ST_TES: return 2; case VKS_ST_GEOM: return 3; case VKS_ST_FRAG: return 4; default: return -1; }
}
#define SHAPE(cond, name) do { if (!(cond)) { if (field) *field = name; return NSD_E_SHAPE; } } while (0)
static int stencil_ok(const nsd_stencil *s) { return s->fail <= 7 && s->pass <= 7 && s->dfail <= 7 && s->cmp <= 7; }
int nsd_shape(const nsd_pipe *p, uint32_t nk, const char **field) {
    // layout: one set, no push constants (descriptors() never makes them), draw()'s descriptor types
    SHAPE(p->l_flags == 0 && p->pl_flags == 0 && p->sets == 1 && p->npush == 0, "layout");
    for (uint32_t i = 0; i < p->nlb; i++) {
        const nsd_lb *b = &p->lb[i];
        SHAPE(b->type <= VKS_DT_UBO_DYN && b->count >= 1 && b->count <= 128 && stage_index(b->stages) >= 0, "layout.binding");
    }
    // pass: create_pass(): LOAD / STORE, GENERAL everywhere, one graphics subpass, two dependencies (checked against the policy later)
    SHAPE(p->r_flags == 0 && p->natt >= 1 && p->s_flags == 0 && p->s_bind == 0 && p->ndep == 2, "pass");
    uint32_t used = 0;
    for (uint32_t i = 0; i < p->natt; i++) {
        const nsd_att *a = &p->att[i];
        SHAPE(a->flags == 0 && is_samples(a->samples), "pass.att");
        SHAPE(a->load == VKS_LOAD_LOAD && a->store == VKS_STORE_STORE && a->sload == VKS_LOAD_LOAD && a->sstore == VKS_STORE_STORE, "pass.att.ops");
        SHAPE(a->init == VKS_LAYOUT_GENERAL && a->fin == VKS_LAYOUT_GENERAL, "pass.att.layout");
    }
    for (uint32_t i = 0; i < p->ncol; i++) {
        SHAPE(p->col[i].layout == VKS_LAYOUT_GENERAL, "pass.color.layout");
        if (p->col[i].att == VKS_SUBPASS_EXTERNAL) continue;   // VK_ATTACHMENT_UNUSED (same value): an unbound colour slot
        SHAPE(p->col[i].att < p->natt && !(used >> p->col[i].att & 1), "pass.color.attachment"); used |= 1u << p->col[i].att;
    }
    if (p->has_depth) {
        SHAPE(p->depth.layout == VKS_LAYOUT_GENERAL && p->depth.att < p->natt && !(used >> p->depth.att & 1), "pass.depth"); used |= 1u << p->depth.att;
    }
    SHAPE(used == (p->natt >= 32 ? 0xffffffffu : (1u << p->natt) - 1), "pass.attachments");   // every attachment is referenced once
    // pipeline
    SHAPE(p->flags == 0 && p->subpass == 0 && p->nst >= 1 && p->nst == nk, "ci");
    int last = -1, tcs = 0, tes = 0;
    for (uint32_t i = 0; i < p->nst; i++) {
        const int s = stage_index(p->st[i].stage);
        SHAPE(p->st[i].flags == 0 && s > last, "stage"); last = s;
        if (s == 1) tcs = 1;
        if (s == 2) tes = 1;
    }
    SHAPE(p->st[0].stage == VKS_ST_VERTEX, "stage.vertex");
    SHAPE(tcs == tes && (uint32_t)tcs == p->has_tess && (!p->has_tess || p->t_flags == 0), "tess");
    SHAPE(p->vi_flags == 0 && p->nvb <= 16 && p->nva <= 16, "vi");
    for (uint32_t i = 0; i < p->nvb; i++) SHAPE(p->vb[i][0] < 16 && (i == 0 || p->vb[i][0] > p->vb[i - 1][0]) && p->vb[i][2] <= 1, "vi.binding");
    for (uint32_t i = 0; i < p->nva; i++) {
        int found = 0;
        for (uint32_t k = 0; k < p->nvb; k++) if (p->vb[k][0] == p->va[i][1]) found = 1;
        SHAPE(p->va[i][0] < 16 && found && p->va[i][2] != 0, "vi.attr");
        for (uint32_t k = 0; k < i; k++) SHAPE(p->va[k][0] != p->va[i][0], "vi.attr.location");
    }
    SHAPE(p->ia_flags == 0 && p->topology <= 10 && p->restart <= 1, "ia");
    SHAPE(p->vp_flags == 0 && p->nvp == 1 && p->nsc == 1 && !p->has_vp && !p->has_sc, "vp");
    SHAPE(p->rs_flags == 0 && p->clamp <= 1 && p->discard <= 1 && p->poly <= 2 && p->cull <= 3 && p->front <= 1 && p->bias_en <= 1 && p->line_width == VKS_LINE_WIDTH_1, "rs");
    SHAPE(p->ms_flags == 0 && is_samples(p->samples) && p->sshading == 0 && p->min_sshading == 0 && p->a2c <= 1 && p->a2one == 0 && !p->has_mask, "ms");
    for (uint32_t i = 0; i < p->natt; i++) SHAPE(p->att[i].samples == p->samples, "ms.samples");
    SHAPE(p->ds_flags == 0 && p->dt <= 1 && p->dw <= 1 && p->dcmp <= 7 && p->dbounds <= 1 && p->st_en <= 1 && stencil_ok(&p->sf) && stencil_ok(&p->sb), "ds");
    SHAPE(p->cb_flags == 0 && p->logic_en <= 1 && p->logic_op <= 15 && p->nblend == p->ncol, "cb");
    for (uint32_t i = 0; i < p->nblend; i++) {
        const nsd_blend *b = &p->blend[i];
        SHAPE(b->enable <= 1 && b->src_c <= 18 && b->dst_c <= 18 && b->src_a <= 18 && b->dst_a <= 18 && b->op_c <= 4 && b->op_a <= 4 && b->mask <= 15, "cb.attachment");
    }
    SHAPE(p->dy_flags == 0 && p->ndyn == 2 && p->dyn[0] == 0 && p->dyn[1] == 1, "dy");
    return NSD_OK;
}

// ---- keys ----
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int hex8(const char *s, size_t n, uint32_t *v) {
    if (n != 8) return -1;
    uint32_t x = 0;
    for (size_t i = 0; i < 8; i++) { int h = hexv(s[i]); if (h < 0) return -1; x = x << 4 | (uint32_t)h; }
    *v = x; return 0;
}
int nsd_key_parse(const char *s, size_t n, nsd_key *k) {
    memset(k, 0, sizeof *k);
    if (!s || n < 16 || n > NSD_KEY_MAX) return -1;
    if (s[0] == 'F' && (s[1] == '1' || s[1] == '2')) {   // [NR-31b] a fused post stage: F1 / F2, up to two upper-case letters, '.', 16 hex
        size_t i = 2;
        while (i < 4 && i < n && s[i] >= 'A' && s[i] <= 'Z') i++;
        if (i >= n || s[i] != '.' || n != i + 17) return -1;
        for (size_t j = i + 1; j < n; j++) if (!((s[j] >= '0' && s[j] <= '9') || (s[j] >= 'a' && s[j] <= 'f'))) return -1;
        memcpy(k->base, s, n); k->base[n] = 0; k->tag = NSD_TAG_FUSED;
        return 0;
    }
    for (int i = 0; i < 16; i++) { if (hexv(s[i]) < 0) return -1; k->base[i] = s[i]; }
    k->base[16] = 0;
    if (n == 16) return 0;
    const char *t = s + 16; size_t m = n - 16;
    if (m > 3 && !memcmp(t, "#at", 3)) {   // #at<func>_<8 hex>
        size_t i = 3; uint32_t f = 0;
        if (i >= m || t[i] < '0' || t[i] > '9') return -1;
        while (i < m && t[i] >= '0' && t[i] <= '9' && f < 1000) f = f * 10 + (uint32_t)(t[i++] - '0');
        if (i >= m || t[i] != '_' || hex8(t + i + 1, m - i - 1, &k->at_bits)) return -1;
        k->tag = NSD_TAG_AT; k->at_func = f;
        char back[NSD_KEY_MAX + 1]; snprintf(back, sizeof back, "#at%u_%08x", f, k->at_bits);   // canonical spelling only (alpha_module's)
        return strlen(back) == m && !memcmp(back, t, m) ? 0 : -1;
    }
    if (m > 3 && !memcmp(t, "#vi", 3)) {   // #vi<8 hex>[c<8 hex>]
        if (m == 11) { if (hex8(t + 3, 8, &k->vi_code)) return -1; }
        else if (m == 20 && t[11] == 'c') { if (hex8(t + 3, 8, &k->vi_code) || hex8(t + 12, 8, &k->vi_conv)) return -1; }
        else return -1;
        k->tag = NSD_TAG_VI;
        char back[NSD_KEY_MAX + 1];
        if (k->vi_conv || m == 20) snprintf(back, sizeof back, "#vi%08xc%08x", k->vi_code, k->vi_conv); else snprintf(back, sizeof back, "#vi%08x", k->vi_code);
        return strlen(back) == m && !memcmp(back, t, m) ? 0 : -1;
    }
    return -1;
}

// ---- policies ----
void nsd_policy_words(const nsd_policy *p, uint32_t o[NSD_POLICY_WORDS]) {
    o[0] = p->tile_barrier; o[1] = p->d24s8_as_d32s8; o[2] = p->x8d24_as_d32; o[3] = p->depth16_as_d24; o[4] = p->bias_x2; o[5] = p->ubo_dynamic;
    o[6] = p->ubo_dyn_limit; o[7] = p->dyn_partial; o[8] = p->noout; o[9] = p->retype; o[10] = p->alpha_test; o[11] = p->features; o[12] = p->unmodelled;
    for (int i = 0; i < 8; i++) o[13 + i] = p->vtx_unsup[i];
}
void nsd_policy_from(const uint32_t i[NSD_POLICY_WORDS], nsd_policy *p) {
    p->tile_barrier = i[0]; p->d24s8_as_d32s8 = i[1]; p->x8d24_as_d32 = i[2]; p->depth16_as_d24 = i[3]; p->bias_x2 = i[4]; p->ubo_dynamic = i[5];
    p->ubo_dyn_limit = i[6]; p->dyn_partial = i[7]; p->noout = i[8]; p->retype = i[9]; p->alpha_test = i[10]; p->features = i[11]; p->unmodelled = i[12];
    for (int k = 0; k < 8; k++) p->vtx_unsup[k] = i[13 + k];
    p->split = 0; p->ubo_ro = 0;   // destination rules only: a seed's layouts are the plain model's
}
uint32_t nsd_vertex_widen(uint32_t f) {   // a copy of xbg_vertex_widen (source_structure_test.py checks the two lists agree)
    switch (f) {
    case 23: return 37; case 24: return 38; case 27: return 41; case 28: return 42;            // R8G8B8 UNORM / SNORM / UINT / SINT
    case 84: return 91; case 85: return 92; case 88: return 95; case 89: return 96; case 90: return 97;   // R16G16B16 UNORM / SNORM / UINT / SINT / SFLOAT
    case 104: return 107; case 105: return 108; case 106: return 109;                          // R32G32B32 UINT / SINT / SFLOAT
    default: return 0;
    }
}
// Could the source have fetched a game format it lacks as recorded format f? (its plain widenings; conversions carry a #vi tag)
static int vtx_ambiguous(const nsd_policy *s, uint32_t f) {
    for (uint32_t g = 1; g < 256; g++) if ((s->vtx_unsup[g >> 5] >> (g & 31) & 1) && nsd_vertex_widen(g) == f) return 1;
    return 0;
}

void nsd_pass_deps(uint32_t tile, nsd_dep dep[2]) {
    // create_pass(): dep[0] external -> 0, dep[1] 0 -> external; DM_GFX_TILE_BARRIER narrows both destination stage masks
    dep[0].src = VKS_SUBPASS_EXTERNAL; dep[0].dst = 0; dep[0].src_stage = VKS_PS_ALL_COMMANDS; dep[0].dst_stage = VKS_PS_ALL_GRAPHICS;
    dep[0].src_access = VKS_AC_MEM_WRITE; dep[0].dst_access = VKS_AC_CA_READ | VKS_AC_CA_WRITE | VKS_AC_DS_READ | VKS_AC_DS_WRITE; dep[0].flags = 0;
    dep[1].src = 0; dep[1].dst = VKS_SUBPASS_EXTERNAL; dep[1].src_stage = VKS_PS_ALL_GRAPHICS; dep[1].dst_stage = VKS_PS_ALL_COMMANDS;
    dep[1].src_access = VKS_AC_CA_WRITE | VKS_AC_DS_WRITE; dep[1].dst_access = VKS_AC_MEM_READ | VKS_AC_MEM_WRITE; dep[1].flags = 0;
    if (tile) { dep[0].dst_stage = VKS_TILE_FRAG; dep[1].dst_stage = VKS_TILE_FRAG | VKS_PS_TRANSFER; }
}

// [NR-31b] xbg_nr_desc.inc split_pick: the shader set takes dyn_mode_for's mode 3 (its dynamic blocks are the most-changing ones so far:
// picked at run time, not from the bundles) - no driver blocks, every game block one element on a slot below 16, at most 16, more than lim
static int split_eligible(const nsd_shader *const *sh, uint32_t nsh, uint32_t lim) {
    uint32_t n = 0;
    for (uint32_t s = 0; s < nsh; s++) for (uint32_t i = 0; i < sh[s]->nb; i++) {
        const nsd_bind *b = &sh[s]->b[i];
        if (b->type != VKS_DT_UBO) continue;
        if (b->source == 5 || b->source == 6) return 0;   // XBG_SOURCE_DRIVER0 / DRIVER2
        if (b->source != 2) continue;                     // XBG_SOURCE_UNIFORM
        if (b->count != 1 || b->slot >= 16 || n >= 16) return 0;   // NRD_MAX_UBO
        n++;
    }
    return lim > 0 && n > lim;
}
// descriptors(): dyn_mode_for + UBO_DYN + the layout key loop (binding, type, count, 1 << stage) over the bound stages in stage order
int nsd_layout(const nsd_policy *pol, const nsd_shader *const *sh, uint32_t nsh, nsd_pipe *p) {
    uint32_t all = 0, vary = 0, n = 0;
    for (uint32_t s = 0; s < nsh; s++) for (uint32_t i = 0; i < sh[s]->nb; i++) {
        const nsd_bind *b = &sh[s]->b[i];
        if (b->type == VKS_DT_UBO) { all += b->count; if (b->source == 2 || b->source == 5 || b->source == 6) vary += b->count; }   // XBG_SOURCE_UNIFORM / DRIVER0 / DRIVER2
    }
    unsigned mode = 0;
    if (pol->ubo_dynamic) {
        const uint32_t lim = pol->ubo_dyn_limit;
        if (pol->unmodelled) return NSD_E_POLICY;
        if (pol->split && vary > lim && !(all && all <= lim) && split_eligible(sh, nsh, lim)) return NSD_E_SPLIT;   // NR-08 DM_NR_DESC_SPLIT
        if (pol->ubo_ro) mode = vary && vary <= lim && pol->dyn_partial ? 2 : 0;                                  // NR-08b: per-draw blocks only
        else mode = all && all <= lim ? 1 : (vary && vary <= lim && pol->dyn_partial ? 2 : 0);
    }
    for (uint32_t s = 0; s < nsh; s++) {
        if (sh[s]->stage > 4) return NSD_E_SHAPE;
        for (uint32_t i = 0; i < sh[s]->nb; i++) {
            const nsd_bind *b = &sh[s]->b[i];
            if (!b->count || b->count > 128 || b->type > VKS_DT_SSBO) return NSD_E_SHAPE;   // draw() stops on these ("Unsupported descriptor array/type")
            if (n >= NSD_MAX_LB) return NSD_E_BIG;
            if (pol->ubo_ro && b->type == VKS_DT_UBO && mode && !sh[s]->reads) return NSD_E_FACTS;
            const int dyn = b->type == VKS_DT_UBO && (mode == 1 || (mode == 2 && (b->source == 2 || b->source == 5 || b->source == 6)))
                         && !(pol->ubo_ro && !sh[s]->read[i]);   // UBO_DYN: under the read-only rule only a block its stage reads
            p->lb[n].binding = b->binding; p->lb[n].type = dyn ? VKS_DT_UBO_DYN : b->type; p->lb[n].count = b->count; p->lb[n].stages = 1u << sh[s]->stage; n++;
        }
    }
    p->l_flags = 0; p->nlb = n; p->pl_flags = 0; p->sets = 1; p->npush = 0;
    return NSD_OK;
}

// depth stand-ins: the game's format of a recorded depth attachment under a policy (0 = ambiguous), and the device's of a game format
static uint32_t depth_back(const nsd_policy *s, uint32_t f) {
    switch (f) {
    case VKS_F_X8D24: return s->depth16_as_d24 ? 0 : f;   // D16 widened, or a real X8_D24
    case VKS_F_D32: return s->x8d24_as_d32 ? 0 : f;
    case VKS_F_D32S8: return s->d24s8_as_d32s8 ? 0 : f;
    default: return f;
    }
}
static uint32_t depth_fwd(const nsd_policy *d, uint32_t f) {
    switch (f) {
    case VKS_F_D16: return d->depth16_as_d24 ? VKS_F_X8D24 : f;
    case VKS_F_X8D24: return d->x8d24_as_d32 ? VKS_F_D32 : f;
    case VKS_F_D24S8: return d->d24s8_as_d32s8 ? VKS_F_D32S8 : f;
    default: return f;
    }
}
static float f_of(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static uint32_t b_of(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

static int deps_equal(const nsd_dep *a, const nsd_dep *b) { return !memcmp(a, b, 2 * sizeof(nsd_dep)); }

#define REFUSE(code, name) do { out->field = name; return code; } while (0)
int nsd_expand(const nsd_policy *src, const nsd_policy *dst, const nsd_dev *dev, const char *const *keys, const uint32_t *klen, uint32_t nk,
               const uint32_t *w, size_t n, nsd_work *wk, nsd_out *out) {
    nsd_pipe *p = &wk->p;
    out->nk = 0; out->nw = 0; out->field = "";
    if (dst->unmodelled && dst->ubo_dynamic) REFUSE(NSD_E_POLICY, "destination policy");
    int e = nsd_parse(w, n, p);
    if (e) REFUSE(e, "record");
    const char *f = NULL;
    if (nsd_shape(p, nk, &f)) REFUSE(NSD_E_SHAPE, f);
    // stage keys: pack keys, the recorded module ids, the source variants re-targeted
    nsd_key k[NSD_STAGES]; int si[NSD_STAGES];
    for (uint32_t i = 0; i < nk; i++) {
        if (nsd_key_parse(keys[i], klen[i], &k[i])) REFUSE(NSD_E_KEY, "stage key");
        if (nsd_fnv(keys[i], klen[i]) != p->st[i].id) REFUSE(NSD_E_ID, "stage.module");
        si[i] = stage_index(p->st[i].stage);
        if (k[i].tag == NSD_TAG_VI && (si[i] != 0 || !src->retype || k[i].vi_conv)) REFUSE(NSD_E_VARIANT, "vertex variant");   // conv: the formats are integer stand-ins
        if ((k[i].tag == NSD_TAG_AT || k[i].tag == NSD_TAG_FUSED) && si[i] != 4) REFUSE(NSD_E_VARIANT, k[i].tag == NSD_TAG_AT ? "alpha variant" : "fused variant");
    }
    // bundle facts
    static const unsigned no_want = 0;
    nsd_shader *sh[NSD_STAGES], *shs = wk->sh;
    int vs = -1, fs = -1;
    for (uint32_t i = 0; i < nk; i++) { if (si[i] == 0) vs = (int)i; if (si[i] == 4) fs = (int)i; }
    for (uint32_t i = 0; i < nk; i++) {
        unsigned want = no_want;
        if ((int)i == vs && dst->retype) want |= NSD_W_CLASSES;
        if ((int)i == fs && (dst->noout || src->noout) && p->nblend) want |= NSD_W_FACTS;
        if (dst->ubo_ro) want |= NSD_W_READS;
        sh[i] = &shs[i]; memset(sh[i], 0, sizeof *sh[i]);
        if (!dev->shader || dev->shader(dev->u, k[i].base, sh[i], want)) REFUSE(NSD_E_SHADER, "bundle");
        if (sh[i]->stage != (uint32_t)si[i] || sh[i]->nb > NSD_MAX_BIND || sh[i]->nin > NSD_MAX_IN) REFUSE(NSD_E_SHADER, "bundle stage");
        if ((want & NSD_W_CLASSES) && !sh[i]->classes) REFUSE(NSD_E_FACTS, "vertex input classes");
        if ((want & NSD_W_FACTS) && !sh[i]->facts) REFUSE(NSD_E_FACTS, "fragment outputs");
        if ((want & NSD_W_READS) && !sh[i]->reads) REFUSE(NSD_E_FACTS, "uniform reads");
    }
    // layout: the bundles under the source policy must give the recorded layout; then this device's
    {
        nsd_pipe *lay = &wk->lay;
        const nsd_shader *const *cs = (const nsd_shader *const *)sh;
        int le = nsd_layout(src, cs, nk, lay);
        if (le == NSD_E_POLICY) REFUSE(NSD_E_LAYOUT, "source policy");
        if (le) REFUSE(le, "layout binding");
        if (lay->nlb != p->nlb || memcmp(lay->lb, p->lb, sizeof(nsd_lb) * p->nlb)) REFUSE(NSD_E_LAYOUT, "layout");
        le = nsd_layout(dst, cs, nk, lay);
        if (le) REFUSE(le, le == NSD_E_SPLIT ? "NR-08 split layout" : le == NSD_E_FACTS ? "uniform reads" : "layout");
        memcpy(p->lb, lay->lb, sizeof(nsd_lb) * lay->nlb); p->nlb = lay->nlb;
        // descriptor limits (0 = not checked)
        uint32_t stl[NSD_STAGES][5] = {{0}}, setl[5] = {0}, udyn = 0, sdyn = 0;
        for (uint32_t i = 0; i < p->nlb; i++) {
            const nsd_lb *b = &p->lb[i]; const int s = stage_index(b->stages); int c = -1, c2 = -1;
            switch (b->type) { case 0: c = 0; break; case 1: c = 0; c2 = 3; break; case 2: case 4: c = 3; break; case 3: case 5: c = 4; break;
                               case VKS_DT_UBO: c = 1; break; case VKS_DT_UBO_DYN: c = 1; udyn += b->count; break; case VKS_DT_SSBO: c = 2; break; default: c = 2; sdyn += b->count; }
            if (s < 0) REFUSE(NSD_E_SHAPE, "layout.binding.stages");
            stl[s][c] += b->count; setl[c] += b->count;
            if (c2 >= 0) { stl[s][c2] += b->count; setl[c2] += b->count; }
        }
        for (int c = 0; c < 5; c++) {
            if (dev->set_lim[c] && setl[c] > dev->set_lim[c]) REFUSE(NSD_E_DESC_LIMIT, "descriptors per set");
            for (int s = 0; s < NSD_STAGES; s++) if (dev->stage_lim[c] && stl[s][c] > dev->stage_lim[c]) REFUSE(NSD_E_DESC_LIMIT, "descriptors per stage");
        }
        if ((dev->set_ubo_dyn && udyn > dev->set_ubo_dyn) || (dev->set_ssbo_dyn && sdyn > dev->set_ssbo_dyn)) REFUSE(NSD_E_DESC_LIMIT, "dynamic buffers");
    }
    // render pass: the source's create_pass dependencies, depth stand-ins back and forward, formats this device renders to
    {
        nsd_dep d[2];
        nsd_pass_deps(src->tile_barrier, d);
        if (!deps_equal(d, p->dep)) REFUSE(NSD_E_DEPS, "pass.dependencies");
        nsd_pass_deps(dst->tile_barrier, p->dep);
    }
    uint32_t d16 = 0;
    if (p->has_depth) {
        nsd_att *a = &p->att[p->depth.att];
        const uint32_t game = depth_back(src, a->format);
        if (!game) REFUSE(NSD_E_DEPTH_AMBIG, "pass.depth.format");
        d16 = game == VKS_F_D16;
        a->format = depth_fwd(dst, game);
        if (dev->att_ok && !dev->att_ok(dev->u, a->format, 1, 0)) REFUSE(NSD_E_FORMAT, "pass.depth.format");
        if (dev->depth_samples && !(dev->depth_samples & a->samples)) REFUSE(NSD_E_LIMIT, "depth samples");
    }
    for (uint32_t i = 0; i < p->ncol; i++) {
        if (p->col[i].att == VKS_SUBPASS_EXTERNAL) continue;
        const nsd_att *a = &p->att[p->col[i].att];
        if (dev->att_ok && !dev->att_ok(dev->u, a->format, 0, p->blend[i].enable != 0)) REFUSE(NSD_E_FORMAT, "pass.color.format");
        if (dev->color_samples && !(dev->color_samples & a->samples)) REFUSE(NSD_E_LIMIT, "color samples");
    }
    if (dev->max_color && p->ncol > dev->max_color) REFUSE(NSD_E_LIMIT, "color attachments");
    // vertex input: fetch formats (fallbacks) and the vertex module variant of this device
    uint32_t conv = 0, code = 0;
    for (uint32_t i = 0; i < p->nva; i++) {
        if (vtx_ambiguous(src, p->va[i][2])) REFUSE(NSD_E_VTX_AMBIG, "vi.attr.format");
        uint32_t kind = 0, fmt = dev->vtx_format ? dev->vtx_format(dev->u, p->va[i][2], &kind) : p->va[i][2];
        if (!fmt) REFUSE(NSD_E_VERTEX, "vi.attr.format");
        p->va[i][2] = fmt; conv |= (kind & 3) << (2 * p->va[i][0]);
        if (dev->max_vi_offset && p->va[i][3] > dev->max_vi_offset) REFUSE(NSD_E_LIMIT, "vi.attr.offset");
        if (dev->max_vi_attr && p->va[i][0] >= dev->max_vi_attr) REFUSE(NSD_E_LIMIT, "vi.attr.location");
    }
    for (uint32_t i = 0; i < p->nvb; i++) {
        if (dev->max_vi_bind && p->vb[i][0] >= dev->max_vi_bind) REFUSE(NSD_E_LIMIT, "vi.binding");
        if (dev->max_vi_stride && p->vb[i][1] > dev->max_vi_stride) REFUSE(NSD_E_LIMIT, "vi.binding.stride");
    }
    if (dst->retype && vs >= 0) {   // draw(): an input whose declared class differs from its attribute format's gets the retyped module
        const nsd_shader *v = sh[vs];
        for (uint32_t i = 0; i < p->nva; i++) for (uint32_t j = 0; j < v->nin; j++) if (v->in_loc[j] == p->va[i][0]) {
            const uint32_t loc = p->va[i][0];
            if ((conv >> (2 * loc)) & 3) break;   // converted, not bitcast
            const uint32_t want = dev->vtx_class ? dev->vtx_class(p->va[i][2]) : 0;
            if (want != v->in_class[j]) code |= (want + 1) << (2 * loc);
            break;
        }
    }
    // rasterizer / multisample / depth: optional features and the depth-bias scale of this device
    if (p->poly != 0 && !(dst->features & NSD_F_NONSOLID)) p->poly = 0;
    if (p->clamp && !(dst->features & NSD_F_DCLAMP)) p->clamp = 0;
    {
        const float s = (src->depth16_as_d24 && d16 ? 256.0f : 1.0f) * (src->bias_x2 ? 2.0f : 1.0f);
        const float game = f_of(p->bias_const) / s;
        p->bias_const = b_of(game * (dst->depth16_as_d24 && d16 ? 256.0f : 1.0f) * (dst->bias_x2 ? 2.0f : 1.0f));
    }
    if (f_of(p->bias_clamp) != 0.0f && !(dst->features & NSD_F_BCLAMP)) p->bias_clamp = 0;
    if (p->dbounds && !(dst->features & NSD_F_DBOUNDS)) p->dbounds = 0;
    // colour blend: logic op feature, NOOUT masks
    if (p->logic_en && !(dst->features & NSD_F_LOGICOP)) { p->logic_en = 0; p->logic_op = 0; }
    if (p->nblend) {
        const int no_out = fs < 0 || (!sh[fs]->outputs && !sh[fs]->sample_mask);
        if (src->noout && no_out && !dst->noout) REFUSE(NSD_E_NOOUT, "cb.write_mask");
        if (dst->noout && no_out) for (uint32_t i = 0; i < p->nblend; i++) p->blend[i].mask = 0;
    }
    // stage keys of this device: the alpha variant if this device runs the alpha test, the vertex variant it computes
    for (uint32_t i = 0; i < nk; i++) {
        char *o = out->key[i]; int len;
        if (si[i] == 0 && (code || conv)) {
            if (conv) len = snprintf(o, NSD_KEY_MAX + 1, "%s#vi%08xc%08x", k[i].base, code, conv);
            else len = snprintf(o, NSD_KEY_MAX + 1, "%s#vi%08x", k[i].base, code);
        } else if (k[i].tag == NSD_TAG_AT && dst->alpha_test) len = snprintf(o, NSD_KEY_MAX + 1, "%.*s", (int)klen[i], keys[i]);
        else len = snprintf(o, NSD_KEY_MAX + 1, "%s", k[i].base);
        if (len <= 0 || len > NSD_KEY_MAX) REFUSE(NSD_E_KEY, "stage key");
        out->klen[i] = (uint32_t)len;
        p->st[i].id = nsd_fnv(o, (size_t)len);
    }
    out->nk = nk;
    out->nw = nsd_emit(p, out->words, NSD_MAX_WORDS);
    if (!out->nw) REFUSE(NSD_E_BIG, "words");
    return NSD_OK;
}

// ---- seed file ----
static uint32_t rd32(const uint8_t *b) { uint32_t v; memcpy(&v, b, 4); return v; }
int nsd_seed_open(const uint8_t *b, size_t n, nsd_seed_hdr *h) {
    memset(h, 0, sizeof *h);
    if (!b || n < 32 || rd32(b) != NSD_SEED_MAGIC) return -1;
    h->version = rd32(b + 4);
    const uint32_t hw = rd32(b + 8), pw = rd32(b + 28);
    if (h->version != NSD_SEED_VERSION || pw != NSD_POLICY_WORDS || hw != 8 + NSD_POLICY_WORDS || (size_t)hw * 4 > n) return -1;
    h->nrec = rd32(b + 12); h->flags = rd32(b + 16); h->id = rd32(b + 20) | (uint64_t)rd32(b + 24) << 32;
    uint32_t pol[NSD_POLICY_WORDS];
    for (int i = 0; i < NSD_POLICY_WORDS; i++) pol[i] = rd32(b + 32 + 4 * i);
    nsd_policy_from(pol, &h->src);
    h->body = (size_t)hw * 4;
    if (nsd_fnv64(b + h->body, n - h->body) != h->id) return -1;
    return 0;
}
int nsd_rec_next(const uint8_t *b, size_t n, size_t *off, nsd_rec *r) {
    size_t i = *off;
    if (i >= n) return 0;
    if (i + 8 > n || rd32(b + i) != NSD_REC_MAGIC) return -1;
    const uint32_t ns = rd32(b + i + 4); i += 8;
    if (ns > NSD_STAGES) return -1;
    r->nk = ns;
    for (uint32_t s = 0; s < ns; s++) {
        if (i + 4 > n) return -1;
        const uint32_t len = rd32(b + i); i += 4;
        if (len > 256 || i + len > n) return -1;
        r->key[s] = (const char *)(b + i); r->klen[s] = len; i += len;
    }
    if (i + 4 > n) return -1;
    const uint32_t count = rd32(b + i); i += 4;
    if (count > NSD_MAX_WORDS || i + (size_t)count * 4 > n) return -1;
    memcpy(r->words, b + i, (size_t)count * 4); r->nw = count; i += (size_t)count * 4;
    *off = i;
    return 1;
}
