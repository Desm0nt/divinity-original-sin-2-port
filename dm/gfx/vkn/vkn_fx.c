// [DOS2] VK-NATIVE stage 5, the draw-effect chain (vkn_fx.h): registration, the run, shaders and depth-stencil states that no
// engine handle names, counters. The call point is vkn_draw.c prepare(). No game addresses.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vkn_fx.h"
#include "vkn_state.h"

#define RED "\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91"

typedef struct fx_fn { vkn_fx_fn fn; void *user; int order; char name[24]; uint32_t when; int (*still)(void); uint64_t ns, nt; } fx_fn;   // [rt P4] when / still; [rt P0] ns / nt
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
// The chain is replaced as a whole (copy, edit, publish the count last): the recording thread reads it without the lock. Entries
// are only ever inserted, and an insert shifts entries upwards starting from the end, so a reader that took the old count still
// sees valid functions (one draw may run a function twice or miss the new one while the insert is in flight: install time only).
static fx_fn g_fn[VKN_FX_CHAIN]; static uint32_t g_nfn;
static int g_bypass;                               // recording thread
static vkn_fx_stats g_st;
static uint64_t g_frame; static uint32_t g_draw_no;
static uint64_t g_vin_draws;                       // [stage 5.3 V1] draws the chain gave a vertex module of their own (vkn_fx_out.vin)
// ---- [rt P4] the gate (vkn_fx.h) ----
// Built-in entries. Each line: the name, the class bits it is gated for, `still`, and why that is exact by the function's code.
int vkn_mat_xlq(void) __attribute__((weak));
static int mat_still(void) { return !vkn_mat_xlq || !vkn_mat_xlq(); }
static const struct { const char *name; uint32_t when; int (*still)(void); } k_gates[] = {
    // vkn_mat.c mat_fx: "if (!ctx->draw_no) census_maybe();" (an edge draw), then "if (ctx->color[0] && !g.xlq) return;" - nothing else
    // before it. Colour target 0 bound and XLQ off (vkn_mat_xlq = g.xlq while the function is in the chain): a return without effect.
    {"mat", VKN_FX_G_C0, mat_still},
};
typedef struct { char name[24]; uint32_t when; int (*still)(void); } gate_e;
static gate_e g_gx[VKN_FX_CHAIN]; static uint32_t g_ngx;   // vkn_fx_gate's entries (under g_mu)
static int g_gate;                                          // DM_VK_NATIVE_FXGATE (install time)
static vkn_fx_gate_stats g_gs;
int vkn_fx_time_next;                                       // [rt P0]
static void gate_apply_locked(fx_fn *f) {   // the entry of f's name (vkn_fx_gate wins over the built-in one)
    f->when = 0; f->still = NULL;
    for (size_t i = 0; i < sizeof k_gates / sizeof k_gates[0]; i++) if (!strcmp(k_gates[i].name, f->name)) { f->when = k_gates[i].when; f->still = k_gates[i].still; }
    for (uint32_t i = 0; i < g_ngx; i++) if (!strcmp(g_gx[i].name, f->name)) { f->when = g_gx[i].when; f->still = g_gx[i].still; }
}

int vkn_fx_register(const char *name, vkn_fx_fn fn, void *user, int order) {
    if (!fn) return -1;
    pthread_mutex_lock(&g_mu);
    uint32_t n = g_nfn; int rc = -1;
    for (uint32_t i = 0; i < n; i++) if (name && !strcmp(g_fn[i].name, name)) { n = VKN_FX_CHAIN; break; }
    if (n < VKN_FX_CHAIN) {
        uint32_t at = n;
        while (at > 0 && g_fn[at - 1].order > order) { g_fn[at] = g_fn[at - 1]; at--; }
        g_fn[at].fn = fn; g_fn[at].user = user; g_fn[at].order = order;
        snprintf(g_fn[at].name, sizeof g_fn[at].name, "%s", name ? name : "?");
        g_fn[at].ns = g_fn[at].nt = 0; gate_apply_locked(&g_fn[at]);   // [rt P4]
        __atomic_store_n(&g_nfn, n + 1, __ATOMIC_RELEASE); rc = 0;
    }
    pthread_mutex_unlock(&g_mu);
    if (rc) vkn_logf(RED "fx: \"%s\" not registered (chain of %d full, or the name is there already)", name ? name : "?", VKN_FX_CHAIN);
    else vkn_logf("fx: \"%s\" registered, order %d", name ? name : "?", order);
    return rc;
}
int vkn_fx_drop(const char *name) {
    int rc = 0;
    pthread_mutex_lock(&g_mu);
    const uint32_t n = g_nfn;
    for (uint32_t i = 0; name && i < n; i++) if (!strcmp(g_fn[i].name, name)) {
        for (uint32_t k = i; k + 1 < n; k++) g_fn[k] = g_fn[k + 1];
        __atomic_store_n(&g_nfn, n - 1, __ATOMIC_RELEASE); rc = 1; break;
    }
    pthread_mutex_unlock(&g_mu);
    if (rc) vkn_logf("fx: \"%s\" dropped", name);
    return rc;
}
static const vkn_fx_out *g_force;                 // [stage 5.1 A4] vkn_fx_force: recording thread
void vkn_fx_force(const vkn_fx_out *o) { g_force = o; }
int vkn_fx_active(void) {
    if (g_force) return 1;                         // [stage 5.1 A4] a piece's own draw with its output (vkn_fx_run copies it)
    if (g_bypass) { g_st.bypassed++; return 0; }
    return __atomic_load_n(&g_nfn, __ATOMIC_ACQUIRE) != 0;
}
void vkn_fx_bypass(int on) { if (on) g_bypass++; else if (g_bypass > 0) g_bypass--; }
// [rt P4] the gate's per-set state (recording thread): the target set of the last call, the gated chain positions for it
static struct { vkn_tex *c[VKN_PASS_COLORS], *d; uint64_t frame; uint32_t mask, nfn; int ok; } g_gset;
static uint32_t gate_set(const vkn_fx_ctx *c, uint32_t n) {   // the gated positions for the draw (edge draws: none, the set's mask computed)
    int edge = !g_gset.ok || c->draw_no == 0 || c->frame != g_gset.frame || c->depth != g_gset.d || n != g_gset.nfn;
    for (uint32_t i = 0; !edge && i < VKN_PASS_COLORS; i++) edge = c->color[i] != g_gset.c[i];
    if (!edge) return g_gset.mask;
    g_gs.edges++;
    memcpy(g_gset.c, c->color, sizeof g_gset.c); g_gset.d = c->depth; g_gset.frame = c->frame; g_gset.nfn = n; g_gset.ok = 1;
    int cx = 0; for (uint32_t i = 0; i < VKN_PASS_COLORS; i++) cx |= c->color[i] != NULL;
    const uint32_t cls = (c->color[0] ? VKN_FX_G_C0 : 0) | (cx ? VKN_FX_G_CX : VKN_FX_G_NOC) | (c->depth ? VKN_FX_G_D : VKN_FX_G_ND);
    uint32_t m = 0;
    for (uint32_t i = 0; i < n && i < 32; i++) if (g_fn[i].when && (cls & g_fn[i].when) == g_fn[i].when && (!g_fn[i].still || g_fn[i].still())) m |= 1u << i;
    g_gset.mask = m;
    return 0;   // (the edge draw itself: every function)
}
void vkn_fx_run(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_fx_out *out) {
    if (g_force) { if (out != g_force) *out = *g_force; out->skip = 0; g_st.bypassed++; vkn_fx_time_next = 0; return; }   // [stage 5.1 A4] (counted as a piece's own draw)
    memset(out, 0, sizeof *out);
    const uint32_t n = __atomic_load_n(&g_nfn, __ATOMIC_ACQUIRE);
    vkn_fx_ctx c = *ctx;
    if (g_frame != c.frame) { g_frame = c.frame; g_draw_no = 0; }
    c.draw_no = g_draw_no++;
    g_st.calls++;
    const uint32_t gm = g_gate ? gate_set(&c, n) : 0;   // [rt P4]
    const int tm = vkn_fx_time_next; vkn_fx_time_next = 0;   // [rt P0]
    for (uint32_t i = 0; i < n; i++) {
        if (__builtin_expect(gm >> i & 1, 0)) {   // [rt P4] gated for this set
            if (g_gate == 1) { g_gs.gated++; continue; }
            vkn_fx_out before = *out;   // mode 2: called; the gate would have been exact for this call if out stays as it was
            g_fn[i].fn(in, &c, out, g_fn[i].user);
            g_gs.verified++;
            if (!vkn_fx_out_same(&before, out) || out->skip != before.skip) {
                if (g_gs.differ++ < 16) vkn_logf(RED "fx gate: \"%s\" changed the output of a draw its gate would have left out (DM_VK_NATIVE_FXGATE=2): the gate entry is wrong", g_fn[i].name);
            }
            if (out->skip) { g_st.skipped++; g_st.by_fn_skip[i]++; return; }
            continue;
        }
        if (__builtin_expect(tm, 0)) { const uint64_t t0 = vkn_now_ns(); g_fn[i].fn(in, &c, out, g_fn[i].user); g_fn[i].ns += vkn_now_ns() - t0; g_fn[i].nt++; }
        else g_fn[i].fn(in, &c, out, g_fn[i].user);
        if (out->skip) { g_st.skipped++; g_st.by_fn_skip[i]++; return; }
    }
    if (out->shader) g_st.shader_swaps++;
    if (out->ds) g_st.ds_swaps++;
    if (out->discard) g_st.discards++;
    if (out->no_fs) g_st.no_fs++;
    if (out->cmp_mask[0] | out->cmp_mask[1]) g_st.cmp_slots++;
    if (out->vin.vs) g_vin_draws++;   // [stage 5.3 V1]
}
int vkn_fx_out_same(const vkn_fx_out *a, const vkn_fx_out *b) {
    static const vkn_fx_out zero;
    if (!a) a = &zero;
    if (!b) b = &zero;
    if (a->skip != b->skip || a->discard != b->discard || a->no_fs != b->no_fs || a->ds != b->ds || a->shader != b->shader) return 0;
    if (memcmp(a->cmp_mask, b->cmp_mask, sizeof a->cmp_mask)) return 0;
    for (int i = 0; i < VKN_FX_TEX; i++) if (a->tex[i].view != b->tex[i].view || a->tex[i].sampler != b->tex[i].sampler || a->tex[i].tex != b->tex[i].tex) return 0;
    for (int i = 0; i < VKN_FX_UBO; i++) if (a->ubo[i].buf != b->ubo[i].buf || a->ubo[i].off != b->ubo[i].off || a->ubo[i].size != b->ubo[i].size) return 0;
    // [stage 5.3 V1] the vertex module / input override (vkn_fx_out.vin): a different module, format, stream or record buffer binds otherwise
    if (a->vin.vs != b->vin.vs || a->vin.vs_id != b->vin.vs_id || a->vin.vfmt != b->vin.vfmt || a->vin.stream != b->vin.stream || a->vin.buf != b->vin.buf ||
        a->vin.off != b->vin.off) return 0;
    return 1;
}
// [stage 5.3 V1] ids of per-draw vertex modules (vkn_fx_out.vin.vs_id): one per module, never reused
static uint32_t g_vs_ids;
uint16_t vkn_fx_vs_id(void) {
    const uint32_t n = __atomic_add_fetch(&g_vs_ids, 1, __ATOMIC_RELAXED);
    if (n > 0xffffu) { static int noted; if (!__atomic_fetch_add(&noted, 1, __ATOMIC_RELAXED)) vkn_logf(RED "fx: more than 65535 per-draw vertex modules: refused"); return 0; }
    return (uint16_t)n;
}

// ---- shaders of the pieces ----
static vkn_shader *g_sh[VKN_FX_SHADERS]; static uint32_t g_nsh;
const vkn_shader *vkn_fx_shader_create(const vkn_fx_shader_src *src) {
    const vkn_dev *d = vkn_device();
    const char *nm = src && src->name ? src->name : "?";
    if (!d || !src || !src->vs || src->nbind > 64 || (src->nbind && !src->bind)) { vkn_logf(RED "fx: shader \"%s\": no device / vertex module / bad table", nm); return NULL; }
    vkn_shader *s = (vkn_shader *)calloc(1, sizeof *s);
    vkn_bind *tab = src->nbind ? (vkn_bind *)malloc(src->nbind * sizeof *tab) : NULL;
    if (!s || (src->nbind && !tab)) { free(s); free(tab); vkn_logf(RED "fx: shader \"%s\": out of memory", nm); return NULL; }
    VkDescriptorSetLayoutBinding lb[64]; memset(lb, 0, sizeof lb);
    uint32_t dyn = 0; int bad = 0;
    for (uint32_t i = 0; i < src->nbind; i++) {
        vkn_bind *t = &tab[i]; *t = src->bind[i];
        const int ubo = t->kind == VKN_BK_UNIFORM || t->kind == VKN_BK_SUPPORT || t->kind == VKN_BK_FX_UBO;
        const int tex = t->kind == VKN_BK_TEXTURE || t->kind == VKN_BK_FX_TEX;
        if ((!ubo && !tex) || t->count != 1 || (i && t->binding <= tab[i - 1].binding) ||
            (ubo && t->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && t->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) ||
            (tex && t->type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) ||
            (t->kind == VKN_BK_FX_TEX && t->slot >= VKN_FX_TEX) || (t->kind == VKN_BK_FX_UBO && t->slot >= VKN_FX_UBO)) { bad = 1; break; }
        t->dyn = t->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ? (uint8_t)dyn++ : (uint8_t)VKN_NO_DYN;
        lb[i].binding = t->binding; lb[i].descriptorType = t->type; lb[i].descriptorCount = 1;
        lb[i].stageFlags = t->bundle_stage == 4 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    }
    if (bad || dyn > d->props.limits.maxDescriptorSetUniformBuffersDynamic) {
        vkn_logf(RED "fx: shader \"%s\": binding table refused (%u bindings, %u dynamic of %u allowed; kinds UNIFORM / SUPPORT / TEXTURE / FX_TEX / FX_UBO, count 1, ascending)",
                 nm, src->nbind, dyn, d->props.limits.maxDescriptorSetUniformBuffersDynamic);
        free(s); free(tab); return NULL;
    }
    VkDescriptorSetLayoutCreateInfo li; memset(&li, 0, sizeof li);
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; li.bindingCount = src->nbind; li.pBindings = src->nbind ? lb : NULL;
    VkDescriptorSetLayout sl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipelineLayoutCreateInfo pi; memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pi.setLayoutCount = 1; pi.pSetLayouts = &sl;
    if (vkCreateDescriptorSetLayout(d->dev, &li, NULL, &sl) != VK_SUCCESS || vkCreatePipelineLayout(d->dev, &pi, NULL, &pl) != VK_SUCCESS) {
        if (sl) vkDestroyDescriptorSetLayout(d->dev, sl, NULL);
        vkn_logf(RED "fx: shader \"%s\": vkCreateDescriptorSetLayout / vkCreatePipelineLayout failed", nm);
        free(s); free(tab); return NULL;
    }
    pthread_mutex_lock(&g_mu);
    const uint32_t n = g_nsh;
    if (n < VKN_FX_SHADERS) { g_sh[n] = s; g_nsh = n + 1; }
    pthread_mutex_unlock(&g_mu);
    if (n >= VKN_FX_SHADERS) {
        vkDestroyPipelineLayout(d->dev, pl, NULL); vkDestroyDescriptorSetLayout(d->dev, sl, NULL);
        vkn_logf(RED "fx: shader \"%s\": more than %d shaders of the pieces", nm, VKN_FX_SHADERS);
        free(s); free(tab); return NULL;
    }
    s->id = (uint32_t)VKN_FX_SHADER_ID0 + n;
    s->stages = (uint8_t)(1u << VKN_SRC_VS | (src->fs ? 1u << VKN_SRC_FS : 0));
    s->set_layout = sl; s->layout = pl;
    s->module[VKN_SRC_VS] = src->vs; s->module[VKN_SRC_FS] = src->fs;
    snprintf(s->key[VKN_SRC_VS], sizeof s->key[0], "%.16s", nm);
    if (src->fs) snprintf(s->key[VKN_SRC_FS], sizeof s->key[0], "%.16s", nm);
    s->bind = tab; s->nbind = (uint16_t)src->nbind; s->ndyn = (uint16_t)dyn;
    s->ninputs = src->ninputs > 16 ? 16 : src->ninputs;
    memcpy(s->inputs, src->inputs, sizeof s->inputs); memcpy(s->input_classes, src->input_classes, sizeof s->input_classes);
    vkn_logf("fx: shader \"%s\" id %#x: %u bindings (%u dynamic), fragment stage %d", nm, s->id, src->nbind, dyn, src->fs ? 1 : 0);
    return s;
}

uint16_t vkn_fx_ds(uint8_t depth_test, uint8_t depth_write, uint8_t depth_func, uint8_t stencil_enable, uint8_t stencil_mask,
                   uint8_t stencil_func, uint8_t depth_fail_op, uint8_t pass_op) {
    return vkn_ds_put(0, depth_test, depth_write, depth_func, stencil_enable, stencil_mask, stencil_func, depth_fail_op, pass_op);   // handle 0: content only
}

int vkn_fx_gate(const char *name, uint32_t when, int (*still)(void)) {   // [rt P4]
    if (!name) return -1;
    pthread_mutex_lock(&g_mu);
    uint32_t i = 0; for (; i < g_ngx; i++) if (!strcmp(g_gx[i].name, name)) break;
    int rc = 0;
    if (i == g_ngx) { if (g_ngx < VKN_FX_CHAIN) g_ngx++; else rc = -1; }
    if (!rc) {
        snprintf(g_gx[i].name, sizeof g_gx[i].name, "%s", name); g_gx[i].when = when; g_gx[i].still = still;
        for (uint32_t k = 0; k < g_nfn; k++) gate_apply_locked(&g_fn[k]);
        g_gset.ok = 0;
    }
    pthread_mutex_unlock(&g_mu);
    return rc;
}
void vkn_fx_gate_mode(int mode) {
    g_gate = mode == 1 || mode == 2 ? mode : 0; g_gset.ok = 0;
    char gl[256]; int k = 0; gl[0] = 0;
    pthread_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < g_nfn; i++) gate_apply_locked(&g_fn[i]);
    for (size_t i = 0; i < sizeof k_gates / sizeof k_gates[0] && k < (int)sizeof gl - 40; i++) k += snprintf(gl + k, sizeof gl - (size_t)k, "%s%s %#x", i ? ", " : "", k_gates[i].name, k_gates[i].when);
    pthread_mutex_unlock(&g_mu);
    vkn_logf("fx: DM_VK_NATIVE_FXGATE=%d (%s); built-in gates: %s", g_gate, g_gate == 1 ? "gated functions left out for their target-set classes" : g_gate == 2 ? "verify: every function called, gated calls checked" : "off", gl);
}
void vkn_fx_gate_stats_get(vkn_fx_gate_stats *s) { if (s) *s = g_gs; }
void vkn_fx_stats_get(vkn_fx_stats *s) { if (!s) return; *s = g_st; s->nfn = __atomic_load_n(&g_nfn, __ATOMIC_ACQUIRE); s->nshaders = g_nsh; }
void vkn_fx_census(void) {
    static vkn_fx_stats last;
    vkn_fx_stats s; vkn_fx_stats_get(&s);
    if (!s.nfn && !s.calls) return;
    char by[384]; int k = 0; by[0] = 0;
    for (uint32_t i = 0; i < s.nfn && k < (int)sizeof by - 48; i++)
        k += snprintf(by + k, sizeof by - (size_t)k, "%s%s skipped %llu", i ? ", " : "", g_fn[i].name, (unsigned long long)(s.by_fn_skip[i] - last.by_fn_skip[i]));
    static uint64_t last_vin;   // [stage 5.3 V1]
    vkn_logf("fx: %u functions (%s); since the last line: draws seen %llu, skipped %llu, shader swaps %llu, depth-state swaps %llu, discard %llu, no fragment stage %llu, "
             "comparison slots asked %llu, own vertex modules %llu, own draws of the pieces %llu; %u shaders of the pieces, %u per-draw vertex modules",
             s.nfn, by, (unsigned long long)(s.calls - last.calls), (unsigned long long)(s.skipped - last.skipped), (unsigned long long)(s.shader_swaps - last.shader_swaps),
             (unsigned long long)(s.ds_swaps - last.ds_swaps), (unsigned long long)(s.discards - last.discards), (unsigned long long)(s.no_fs - last.no_fs),
             (unsigned long long)(s.cmp_slots - last.cmp_slots), (unsigned long long)(g_vin_draws - last_vin), (unsigned long long)(s.bypassed - last.bypassed), s.nshaders,
             __atomic_load_n(&g_vs_ids, __ATOMIC_RELAXED));
    last = s; last_vin = g_vin_draws;
    {   // [rt P4] the gate; [rt P0] the chain by function (DM_VK_NATIVE_PROF=3 samples)
        static vkn_fx_gate_stats go;
        if (g_gate) vkn_logf("fx gate: DM_VK_NATIVE_FXGATE=%d: calls left out %llu, verified %llu, differ %llu, target-set edges %llu (since the last line)", g_gate,
                             (unsigned long long)(g_gs.gated - go.gated), (unsigned long long)(g_gs.verified - go.verified), (unsigned long long)(g_gs.differ - go.differ), (unsigned long long)(g_gs.edges - go.edges));
        go = g_gs;
        static uint64_t ons[VKN_FX_CHAIN], ont[VKN_FX_CHAIN];
        char fl[640]; int k = 0; fl[0] = 0; uint64_t any = 0;
        for (uint32_t i = 0; i < s.nfn && k < (int)sizeof fl - 48; i++) {
            const uint64_t nt = g_fn[i].nt - ont[i], ns = g_fn[i].ns - ons[i]; any |= nt;
            k += snprintf(fl + k, sizeof fl - (size_t)k, "%s%s %.3f", i ? ", " : "", g_fn[i].name, nt ? (double)ns / (double)nt / 1000.0 : 0.0);
            ont[i] = g_fn[i].nt; ons[i] = g_fn[i].ns;
        }
        if (any) vkn_logf("fx by function (us a sampled chain call, DM_VK_NATIVE_PROF=3; x chain calls a frame = ms): %s", fl);
    }
}
