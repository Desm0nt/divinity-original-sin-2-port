// [DOS2] VK-NATIVE stage 5 rest host test: the warm-up list with technique pipelines and the portable seed (vkn_warm.c, vkn_seed.c,
// dm/gfx/nr/nr_seed.c) against the mock of vkn_mock_vk.h. Every "run" is a forked child (the worker thread and the list are per
// process, as on the phone across starts); a run's exit code is its failure count.
//   run 1  render-thread builds through vkn_pipe_get: a plain one, its no-fragment-stage variant (fx bit), the same with a piece's
//          vertex module (vin id + recipe), a piece's shader object (recipe), a vin module WITHOUT a recipe (not listable): the list
//          file holds 4 version-2 records, the reasons are counted;
//   run 2  the next start: the worker makes every listed one again (the pieces' makers asked for the module / object) and a draw's
//          vkn_warm_take of the same key + modules gets each - the technique pipelines are built before their first draw;
//   run 3  a version-1 list (stage 4) of the plain record is read, widened, served, and the file rewritten as version 2;
//   run 4  DM_VK_NATIVE_SEED=2 on a device without D24 depth attachments and without 3-component vertex formats: an old-path seed
//          (NR-31 records made here from a pipeline the backend built on a "source" device, RGB8 vertex attribute, D24S8 pass) is
//          translated when its shader exists - D24S8 -> D32S8, the widened attribute matched, the state ids of this run - and the
//          render thread's build of that pipeline is "the seed had it" byte for byte; another raster state = "same shaders differs";
//          a record of a shader the run never makes waits; a fused post stage is refused;
//   run 5  DM_VK_NATIVE_SEED=1, same device and seed: the record enters the list (not the file) and the worker builds it; the draw's
//          take gets it.
// Build + run (WSL gcc, <vk> as in vkn_stage2_test.c):
//   wsl gcc -std=c11 -D_GNU_SOURCE -DDM_GAME_DOOM=1 -DDM_GAME_D3=1 -DDM_GAME_TL2=1 -DDM_GAME_DOS2=1 -O1 -g -Wall -Wextra -Wno-unused-parameter -I<vk> -o /tmp/vkn_warm_test
//       dm/gfx/vulkan/engine/tests/vkn_warm_test.c dm/gfx/vkn/vkn_core.c dm/gfx/vkn/vkn_buf.c dm/gfx/vkn/vkn_tex.c dm/gfx/vkn/vkn_shader.c
//       dm/gfx/vkn/vkn_state.c dm/gfx/vkn/vkn_mirror.c dm/gfx/vkn/vkn_astc.c dm/gfx/vkn/vkn_bcgpu.c dm/gfx/vkn/vkn_drv.c dm/gfx/vkn/vkn_warm.c
//       dm/gfx/vkn/vkn_seed.c dm/gfx/nr/nr_seed.c dm/gfx/nvn/nvn_tex.c dm/gfx/tex/nvn_deswizzle.c dm/gfx/tex/nvn_bcdec.c dm/gfx/nr/nr_reg.c
//       -lpthread -lm && wsl /tmp/vkn_warm_test
// Ends with "vkn_warm_test: N checks passed" (exit 0) or the failures and exit 1.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "vkn_mock_vk.h"
#include "../../../vkn/vkn.h"
#include "../../../vkn/vkn_shader.h"
#include "../../../vkn/vkn_state.h"
#include "../../../vkn/vkn_warm.h"
#include "../../../vkn/vkn_seed.h"
#include "../../../vkn/vkn_fx.h"
#include "../../../nr/nr_seed.h"

static int g_checks, g_fails;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { const long long a_ = (long long)(a), b_ = (long long)(b); g_checks++; \
    if (a_ != b_) { g_fails++; printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, a_, b_); } } while (0)
#define H(idx, gen) ((uint64_t)(gen) << 32 | (uint32_t)(idx))

// ---- the fixture: the same states, shader and piece objects in every run (content ids come out the same) ----
enum { CTL = 0xe48 };
static uint8_t g_ctl[4][CTL], g_code[4][64];
static vkn_stage_src stage(int i, const char *key) { memset(g_ctl[i], 0, CTL); memcpy(g_ctl[i], key, 16); memset(g_code[i], i, 64); return (vkn_stage_src){g_ctl[i], g_code[i], NULL, 0}; }
static const vkn_shader *S;            // the engine shader (keys a1.. / b1..)
static vkn_shader g_fx;                // a piece's shader object (fake: id in the pieces' range, the engine shader's layout)
static const VkShaderModule M_VIN = (VkShaderModule)(uintptr_t)0x7c00010ull, M_FXV = (VkShaderModule)(uintptr_t)0x7c00020ull, M_FXF = (VkShaderModule)(uintptr_t)0x7c00030ull;
static uint16_t g_blend, g_blend2, g_ds, g_raster, g_raster2, g_vfmt;
static int g_maker_calls;
static int tst_make(const struct vkn_shader *eng, const void *arg, uint32_t n, const struct vkn_shader **fx, VkShaderModule *vs, uint16_t *vs_id) {
    g_maker_calls++;
    if (eng != S || n != 2) return -1;
    const uint8_t *a = (const uint8_t *)arg;
    if (a[0] == 'v' && a[1] == 1) { *vs = M_VIN; *vs_id = 77; return 0; }
    if (a[0] == 'o' && a[1] == 2) { *fx = &g_fx; return 0; }
    return -1;
}
static void fixture(void) {
    mk_main_thread = pthread_self();
    vkn_set_mode(VKN_NATIVE);
    xbg_vkn_stage *vs = mk_bundle_add("a000000000000001", 0);
    mk_bind(vs, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, XBG_SOURCE_UNIFORM, 1, 64);
    vs->input_count = 2; vs->inputs[0] = 0; vs->inputs[1] = 4;   // position, the RGB8 colour (usage 2 -> location 4)
    xbg_vkn_stage *fs = mk_bundle_add("b000000000000001", 4);
    mk_bind(fs, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, XBG_SOURCE_TEXTURE, 0, 0);
    vkn_stage_src src[VKN_SRCS]; memset(src, 0, sizeof src);
    src[VKN_SRC_VS] = stage(0, "a000000000000001"); src[VKN_SRC_FS] = stage(1, "b000000000000001");
    CHECK_EQ(vkn_shader_create(H(10, 1), src), 0);
    S = vkn_shader_get(H(10, 1)); CHECK(S != NULL && S->layout);
    g_blend = vkn_blend_put(H(1, 1), 1, 0, 0, 0, 0, 0);    // one / zero (off)
    g_blend2 = vkn_blend_put(H(2, 1), 6, 7, 0, 0, 0, 0);   // alpha blending
    g_ds = vkn_ds_put(H(3, 1), 1, 1, 1, 0, 0, 0, 0, 0);    // LESS + write
    g_raster = vkn_raster_put(H(4, 1), 1, 0, 0, 0, 0);     // cull back
    g_raster2 = vkn_raster_put(H(5, 1), 2, 0, 0, 0, 0);    // cull front
    static const uint8_t el[12] = {0, 0, 0, 0, 0, 3, /* stream 0, usage 0, position, float x3 */ 0, 2, 0, 0, 8, 3 /* usage 2 index 0 (location 4), R8 unorm x3 */};
    g_vfmt = vkn_vfmt_put(H(6, 1), el, 2);
    CHECK(g_blend && g_blend2 && g_ds && g_raster && g_raster2 && g_vfmt);
    memset(&g_fx, 0, sizeof g_fx);
    g_fx.id = (uint32_t)VKN_FX_SHADER_ID0 + 5; g_fx.stages = 1u << VKN_SRC_VS | 1u << VKN_SRC_FS; g_fx.layout = S->layout;
    g_fx.module[VKN_SRC_VS] = M_FXV; g_fx.module[VKN_SRC_FS] = M_FXF; snprintf(g_fx.key[0], 17, "test-fx"); snprintf(g_fx.key[1], 17, "test-fx");
    g_fx.ninputs = S->ninputs; memcpy(g_fx.inputs, S->inputs, sizeof g_fx.inputs);
    CHECK_EQ(vkn_warm_maker("tst", tst_make), 0);
}
static vkn_pipe_desc desc(uint32_t depth_fmt, uint16_t raster) {
    vkn_pipe_desc d; memset(&d, 0, sizeof d);
    const uint32_t col[8] = {VK_FORMAT_R8G8B8A8_UNORM, 0, 0, 0, 0, 0, 0, 0};
    d.shader = S->id; d.blend = g_blend; d.ds = g_ds; d.raster = raster; d.vfmt = g_vfmt; d.prim = 1; d.write_mask[0] = 0xf;
    d.pass = vkn_rp_compat_make(col, 1, depth_fmt, 1);
    return d;
}
static vkn_pipe_src src_of(const vkn_pipe_desc *d) {
    vkn_pipe_src s; memset(&s, 0, sizeof s);
    s.vs = vkn_shader_module(S, VKN_STAGE_VS, 0, 0.0f, 0); s.fs = (d->fx & VKN_PIPE_FX_NO_FS) ? VK_NULL_HANDLE : vkn_shader_module(S, VKN_STAGE_FS, 0, 0.0f, 0);
    s.layout = S->layout; s.shader = S;
    for (uint32_t k = 0; k < S->ninputs; k++) s.vs_inputs |= (uint16_t)(1u << S->inputs[k]);
    return s;
}
// the five render-thread builds of run 1 (k 0 plain, 1 no fragment stage, 2 vin with a recipe, 3 a piece's object, 4 vin without one)
static void build_case(int k, vkn_pipe_key *key, vkn_pipe_src *src) {
    vkn_pipe_desc d = desc(VK_FORMAT_D24_UNORM_S8_UINT, g_raster);
    if (k == 1) d.fx = VKN_PIPE_FX_NO_FS;
    if (k == 3) d.shader = g_fx.id;
    *src = src_of(&d);
    if (k == 2 || k == 4) { src->vs = M_VIN; src->vs_id = (uint16_t)(k == 2 ? 77 : 78); }
    if (k == 3) { src->vs = M_FXV; src->fs = M_FXF; src->shader = &g_fx; }
    *key = vkn_pipe_key_pack(&d);
    key->w[2] |= (uint64_t)src->vs_id << 48;
}
static void wait_for(int (*cond)(void), int ms) { for (int i = 0; i < ms / 10 && !cond(); i++) usleep(10000); }
static int c_ready(void) { vkn_warm_stats w; vkn_warm_stats_get(&w); return w.cache_state != 0; }
static int c_noted(void) { vkn_warm_stats w; vkn_warm_stats_get(&w); return w.appended + w.not_listable >= 5; }
static int c_built4(void) { vkn_warm_stats w; vkn_warm_stats_get(&w); return w.built >= 4; }
static int c_built1(void) { vkn_warm_stats w; vkn_warm_stats_get(&w); return w.built >= 1; }
static char g_dir[128];
static void start(const char *seed_mode) {
    setenv("DM_FILES_DIR", g_dir, 1); setenv("DM_VK_NATIVE_SEED", seed_mode, 1);
    CHECK_EQ(vkn_warm_start(), 1);
    wait_for(c_ready, 3000); CHECK(c_ready());
}
static size_t file_size(const char *name) { char p[256]; snprintf(p, sizeof p, "%s/%s", g_dir, name); struct stat st; return stat(p, &st) ? 0 : (size_t)st.st_size; }

static void run1(void) {
    fixture();
    vkn_warm_recipe_vs(77, "tst", S, "v\x01", 2);
    vkn_warm_recipe_fx(&g_fx, "tst", S, "o\x02", 2);
    start("0");
    for (int k = 0; k < 5; k++) { vkn_pipe_key key; vkn_pipe_src src; build_case(k, &key, &src); CHECK(vkn_pipe_get(&key, &src) != VK_NULL_HANDLE); }
    wait_for(c_noted, 3000);
    usleep(300000);   // (the worker appends at its next tick)
    vkn_warm_stats w; vkn_warm_stats_get(&w);
    CHECK_EQ(w.appended, 4); CHECK_EQ(w.not_listable, 1); CHECK_EQ(w.nl_vin, 1); CHECK_EQ(w.tech_listed, 3);
    CHECK_EQ(file_size("vkn-pipeline-list.bin"), 48 + 4 * sizeof(vkn_warm_rec));
    vkn_warm_census();
}
static void run2(void) {
    fixture();
    start("0");
    wait_for(c_built4, 3000);
    vkn_warm_stats w; vkn_warm_stats_get(&w);
    CHECK_EQ(w.loaded, 4); CHECK_EQ(w.built, 4); CHECK_EQ(w.tech_replayed, 3); CHECK_EQ(g_maker_calls, 2);
    for (int k = 0; k < 4; k++) { vkn_pipe_key key; vkn_pipe_src src; build_case(k, &key, &src); VkPipeline p = VK_NULL_HANDLE; CHECK_EQ(vkn_warm_take(&key, &src, &p), 1); CHECK(p != VK_NULL_HANDLE); }
    // the no-fragment variant was built without a fragment stage, the vin one with the piece's module
    { vkn_pipe_key key; vkn_pipe_src src; build_case(1, &key, &src); src.fs = VK_NULL_HANDLE; VkPipeline p; CHECK_EQ(vkn_warm_take(&key, &src, &p), 0); }   // (taken already)
    vkn_warm_census();
}
static void run3(void) {   // a stage-4 (version 1) list holding the plain record
    fixture();
    char p[256]; snprintf(p, sizeof p, "%s/vkn-pipeline-list.bin", g_dir);
    FILE *f = fopen(p, "rb"); uint8_t hdr[48]; vkn_warm_rec r; CHECK(f != NULL); if (!f) return;
    CHECK_EQ(fread(hdr, 1, 48, f), 48); CHECK_EQ(fread(&r, 1, sizeof r, f), sizeof r); fclose(f);
    uint32_t v1 = 1, rs = 232; memcpy(hdr + 8, &v1, 4); memcpy(hdr + 12, &rs, 4);
    uint8_t rec1[232]; memcpy(rec1, &r, 224); const uint64_t s1 = vkn_fnv(rec1, 224); memcpy(rec1 + 224, &s1, 8);
    f = fopen(p, "wb"); fwrite(hdr, 1, 48, f); fwrite(rec1, 1, 232, f); fclose(f);
    start("0");
    wait_for(c_built1, 3000);
    vkn_warm_stats w; vkn_warm_stats_get(&w);
    CHECK_EQ(w.loaded, 1); CHECK_EQ(w.upgraded, 1); CHECK_EQ(w.built, 1);
    vkn_pipe_key key; vkn_pipe_src src; build_case(0, &key, &src); VkPipeline pp; CHECK_EQ(vkn_warm_take(&key, &src, &pp), 1);
    CHECK_EQ(file_size("vkn-pipeline-list.bin"), 48 + sizeof(vkn_warm_rec));   // rewritten as version 2
}

// ---- the seed: NR-31 records made from what the backend built on a "source" device (all formats there) ----
static uint8_t *g_seed; static size_t g_nseed;
static void put32(uint8_t **b, size_t *n, size_t *cap, uint32_t v) { if (*n + 4 > *cap) { *cap = *cap * 2 + 4096; *b = (uint8_t *)realloc(*b, *cap); } memcpy(*b + *n, &v, 4); *n += 4; }
static void put_rec(uint8_t **b, size_t *n, size_t *cap, const char *const *keys, uint32_t nk, const nsd_pipe *p) {
    static uint32_t w[NSD_MAX_WORDS]; const size_t nw = nsd_emit(p, w, NSD_MAX_WORDS); CHECK(nw > 0);
    put32(b, n, cap, NSD_REC_MAGIC); put32(b, n, cap, nk);
    for (uint32_t i = 0; i < nk; i++) { const uint32_t l = (uint32_t)strlen(keys[i]); put32(b, n, cap, l); while (*n + l > *cap) { *cap = *cap * 2 + 4096; *b = (uint8_t *)realloc(*b, *cap); } memcpy(*b + *n, keys[i], l); *n += l; }
    put32(b, n, cap, (uint32_t)nw); for (size_t i = 0; i < nw; i++) put32(b, n, cap, w[i]);
}
// the create info of pipeline h (mock capture) as a seed record's content: the old path's create info for the same draw state
static void pipe_to_nsd(VkPipeline h, nsd_pipe *p) {
    memset(p, 0, sizeof *p);
    const mk_obj *o = mk_obj_of((uint64_t)(uintptr_t)h); const mk_gp *g = &o->gp;
    const mk_obj *ro = mk_obj_of((uint64_t)(uintptr_t)g->pass); const mk_rp *rp = &ro->rp;
    p->natt = rp->natt; for (uint32_t i = 0; i < rp->natt; i++) { p->att[i].format = rp->att[i].format; p->att[i].samples = rp->att[i].samples; }
    p->ncol = rp->ncolor; for (uint32_t i = 0; i < rp->ncolor; i++) { p->col[i].att = rp->color[i].attachment; p->col[i].layout = rp->color[i].layout; }
    p->has_depth = (uint32_t)rp->has_depth; p->depth.att = rp->depth.attachment; p->depth.layout = rp->depth.layout;
    p->nst = g->nst; for (uint32_t i = 0; i < g->nst; i++) { p->st[i].stage = g->stage[i]; p->st[i].id = 1 + i; }
    for (uint32_t i = 0; i < g->nbind; i++) { p->vb[p->nvb][0] = g->bind[i].binding; p->vb[p->nvb][1] = g->bind[i].stride; p->vb[p->nvb][2] = g->bind[i].inputRate; p->nvb++; }
    for (uint32_t i = 0; i < g->nattr; i++) { p->va[p->nva][0] = g->attr[i].location; p->va[p->nva][1] = g->attr[i].binding; p->va[p->nva][2] = g->attr[i].format; p->va[p->nva][3] = g->attr[i].offset; p->nva++; }
    p->topology = g->topo;
    p->poly = g->rs.polygonMode; p->cull = g->rs.cullMode; p->front = g->rs.frontFace; p->bias_en = g->rs.depthBiasEnable; p->line_width = 0x3f800000u;
    p->samples = g->samples;
    if (g->has_ds) {
        p->dt = g->ds.depthTestEnable; p->dw = g->ds.depthWriteEnable; p->dcmp = g->ds.depthCompareOp; p->st_en = g->ds.stencilTestEnable;
        const VkStencilOpState *f = &g->ds.front; p->sf = (nsd_stencil){f->failOp, f->passOp, f->depthFailOp, f->compareOp, f->compareMask, f->writeMask, f->reference};
        const VkStencilOpState *b = &g->ds.back; p->sb = (nsd_stencil){b->failOp, b->passOp, b->depthFailOp, b->compareOp, b->compareMask, b->writeMask, b->reference};
    }
    p->nblend = g->natt;
    for (uint32_t i = 0; i < g->natt; i++) { const VkPipelineColorBlendAttachmentState *a = &g->att[i];
        p->blend[i] = (nsd_blend){a->blendEnable, a->srcColorBlendFactor, a->dstColorBlendFactor, a->colorBlendOp, a->srcAlphaBlendFactor, a->dstAlphaBlendFactor, a->alphaBlendOp, a->colorWriteMask}; }
    p->ndyn = 2; p->dyn[0] = VK_DYNAMIC_STATE_VIEWPORT; p->dyn[1] = VK_DYNAMIC_STATE_SCISSOR;
}
static void make_seed(void) {   // a "source" process: everything renders / fetches (built here, before the children change the device)
    fixture();
    vkn_pipe_desc d = desc(VK_FORMAT_D24_UNORM_S8_UINT, g_raster); vkn_pipe_src s = src_of(&d);
    uint64_t ns = 0; VkPipeline p = vkn_pipe_build(&d, &s, &ns); CHECK(p != VK_NULL_HANDLE); if (!p) return;
    static nsd_pipe np; pipe_to_nsd(p, &np);
    for (uint32_t i = 0; i < np.nva; i++) if (np.va[i][0] == 4) CHECK_EQ(np.va[i][2], VK_FORMAT_R8G8B8_UNORM);   // (the source fetches RGB8)
    uint8_t *b = NULL; size_t n = 0, cap = 0;
    for (int i = 0; i < 8 + NSD_POLICY_WORDS; i++) put32(&b, &n, &cap, 0);
    const char *k0[2] = {"a000000000000001", "b000000000000001"};
    const char *k1[2] = {"a0000000000000ff", "b0000000000000ff"};                 // a shader no run makes
    const char *k2[2] = {"a000000000000001", "F1.b000000000000001"};              // a fused post stage of the old path
    put_rec(&b, &n, &cap, k0, 2, &np); put_rec(&b, &n, &cap, k1, 2, &np); put_rec(&b, &n, &cap, k2, 2, &np);
    const uint64_t id = nsd_fnv64(b + 4 * (8 + NSD_POLICY_WORDS), n - 4 * (8 + NSD_POLICY_WORDS));
    const uint32_t h[8] = {NSD_SEED_MAGIC, NSD_SEED_VERSION, 8 + NSD_POLICY_WORDS, 3, 0, (uint32_t)id, (uint32_t)(id >> 32), NSD_POLICY_WORDS};
    memcpy(b, h, sizeof h);
    nsd_seed_hdr sh; CHECK_EQ(nsd_seed_open(b, n, &sh), 0); CHECK_EQ(sh.nrec, 3);
    char path[256]; snprintf(path, sizeof path, "%s.seed", g_dir);   // (handed to the parent: it never touches the backend itself)
    FILE *f = fopen(path, "wb"); CHECK(f != NULL); if (f) { CHECK_EQ(fwrite(b, 1, n, f), n); fclose(f); }
    free(b);
}
static void read_seed(void) {
    char path[256]; snprintf(path, sizeof path, "%s.seed", g_dir);
    FILE *f = fopen(path, "rb"); CHECK(f != NULL); if (!f) return;
    fseek(f, 0, SEEK_END); g_nseed = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    g_seed = (uint8_t *)malloc(g_nseed); CHECK_EQ(fread(g_seed, 1, g_nseed, f), g_nseed); fclose(f); remove(path);
}
static int c_translated(void) { vkn_seed_stats s; vkn_seed_stats_get(&s); return s.translated >= 1; }
static void run4(void) {   // verify on a device without D24 attachments / RGB8 vertex fetch
    mk_d24 = 0; mk_vtx3 = 0;
    fixture();
    vkn_seed_test_setup(2, g_seed, g_nseed);
    { const vkn_vfmt *v = vkn_vfmt_info(g_vfmt); CHECK(v && (v->widened >> 4 & 1)); }
    start("2");
    wait_for(c_translated, 3000);
    vkn_seed_stats s; vkn_seed_stats_get(&s);
    CHECK_EQ(s.file, 1); CHECK_EQ(s.records, 3); CHECK_EQ(s.translated, 1); CHECK_EQ(s.refused[VKN_SEED_R_FUSED], 1); CHECK_EQ(s.waiting, 1);
    CHECK_EQ(s.records_made, 1); CHECK_EQ(s.added, 0);   // verify: nothing enters the list
    // the render thread builds the seed's pipeline here (D32S8: this device's stand-in) and one with another raster state
    vkn_pipe_desc d = desc(VK_FORMAT_D32_SFLOAT_S8_UINT, g_raster); vkn_pipe_src src = src_of(&d); vkn_pipe_key key = vkn_pipe_key_pack(&d);
    CHECK(vkn_pipe_get(&key, &src) != VK_NULL_HANDLE);
    vkn_pipe_desc d2 = desc(VK_FORMAT_D32_SFLOAT_S8_UINT, g_raster2); vkn_pipe_src src2 = src_of(&d2); vkn_pipe_key key2 = vkn_pipe_key_pack(&d2);
    CHECK(vkn_pipe_get(&key2, &src2) != VK_NULL_HANDLE);
    usleep(400000);
    vkn_seed_stats_get(&s);
    CHECK_EQ(s.notes, 2); CHECK_EQ(s.had, 1); CHECK_EQ(s.differ, 1); CHECK_EQ(s.unknown, 0);
    vkn_warm_stats w; vkn_warm_stats_get(&w); CHECK_EQ(w.seed_added, 0);
    vkn_warm_census();
}
static int c_seed_built(void) { vkn_warm_stats w; vkn_warm_stats_get(&w); return w.seed_replayed >= 1; }
static void run5(void) {   // pre-build
    mk_d24 = 0; mk_vtx3 = 0;
    fixture();
    vkn_seed_test_setup(1, g_seed, g_nseed);
    start("1");
    wait_for(c_seed_built, 3000);
    vkn_warm_stats w; vkn_warm_stats_get(&w); CHECK_EQ(w.seed_added, 1); CHECK_EQ(w.seed_replayed, 1);
    vkn_pipe_desc d = desc(VK_FORMAT_D32_SFLOAT_S8_UINT, g_raster); vkn_pipe_src src = src_of(&d); vkn_pipe_key key = vkn_pipe_key_pack(&d);
    VkPipeline p = VK_NULL_HANDLE; CHECK_EQ(vkn_warm_take(&key, &src, &p), 1); CHECK(p != VK_NULL_HANDLE);
    usleep(300000);
    CHECK_EQ(file_size("vkn-pipeline-list.bin"), 48);   // a seed record is not written into the device's own list
}

static int c_loaded(void) { vkn_seed_stats s; vkn_seed_stats_get(&s); return s.file != -2; }
static void run6(void) {   // the APK's real seed (dm/android/dos2/dos2-pipe-seed.bin, the old path's Mi 14 list): every record read
    FILE *f = fopen("dm/android/dos2/dos2-pipe-seed.bin", "rb");
    if (!f) { printf("run 6: no dm/android/dos2/dos2-pipe-seed.bin (run from the repository root): skipped\n"); return; }
    fseek(f, 0, SEEK_END); const size_t n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc(n); CHECK_EQ(fread(b, 1, n, f), n); fclose(f);
    fixture();
    vkn_seed_test_setup(2, b, n); free(b);
    start("2");
    wait_for(c_loaded, 3000);
    vkn_seed_stats s; vkn_seed_stats_get(&s);
    CHECK_EQ(s.file, 1); CHECK_EQ(s.records, 931);
    CHECK_EQ(s.refused[VKN_SEED_R_PARSE], 0); CHECK_EQ(s.refused[VKN_SEED_R_SHAPE], 0); CHECK_EQ(s.refused[VKN_SEED_R_STAGES], 0); CHECK_EQ(s.refused[VKN_SEED_R_KEYS], 0);
    CHECK_EQ(s.refused[VKN_SEED_R_FUSED], 2); CHECK_EQ(s.refused[VKN_SEED_R_CONV], 0); CHECK_EQ(s.refused[VKN_SEED_R_BLEND], 0);
    printf("run 6: the real seed: %u records, refused fused %u, waiting for their shaders (none made here) %u\n", s.records, s.refused[VKN_SEED_R_FUSED], s.waiting);
}
static int fork_run(void (*fn)(void), const char *name) {
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) { fn(); if (g_fails) printf("%s: %d of %d checks FAILED\n", name, g_fails, g_checks); else printf("%s: %d checks passed\n", name, g_checks); fflush(stdout); _exit(g_fails > 250 ? 250 : g_fails); }
    int st = 0; waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}
static void clean_dir(void) { char c[400]; snprintf(c, sizeof c, "rm -rf %s && mkdir -p %s", g_dir, g_dir); CHECK_EQ(system(c), 0); }
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(g_dir, sizeof g_dir, "/tmp/vkn_warm_test_%d", (int)getpid());
    int bad = 0;
    clean_dir();
    bad += fork_run(run1, "run 1 (listing)");
    bad += fork_run(run2, "run 2 (replay)");
    bad += fork_run(run3, "run 3 (version-1 list)");
    bad += fork_run(make_seed, "seed source");
    read_seed();
    clean_dir();
    bad += fork_run(run4, "run 4 (seed verify)");
    clean_dir();
    bad += fork_run(run5, "run 5 (seed pre-build)");
    clean_dir();
    bad += fork_run(run6, "run 6 (the APK's seed)");
    char c[400]; snprintf(c, sizeof c, "rm -rf %s", g_dir); if (system(c)) {}
    bad += g_fails;
    if (bad) { printf("vkn_warm_test: FAILED (%d)\n", bad); return 1; }
    printf("vkn_warm_test: %d checks passed (parent) + the runs above\n", g_checks);
    return 0;
}
