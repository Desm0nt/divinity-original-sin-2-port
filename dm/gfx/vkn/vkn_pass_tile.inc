// [DOS2] VK-NATIVE P10 (docs/dos2/nr/VK-P10.md, VK-MOBILE-GPU-PLAN.md П10): the G-buffer -> LinearizeDepth -> CascadedPCF -> lighting chain as
// ONE render pass with subpasses on tile GPUs (DM_VK_NATIVE_TILELIGHT). Part of vkn_pass.c (included before begin_set).
//
// What it does. The engine sets its targets pass by pass; a pass whose only draw reads the targets of earlier passes at the fragment's
// own pixel can be a SUBPASS of one render pass, reading them as input attachments: nothing of the G-buffer has to be stored to memory
// and loaded back. Three things stand in the way, and each has an exact answer here:
//   order     the shadow atlas pass sits between LinearizeDepth and CascadedPCF. The chain's subpasses are recorded into SECONDARY
//             command buffers (vkn_cb_route); the merged pass itself (begin, execute, next subpass, end) is written into the
//             recording only when the chain ends. Everything foreign that came in between (the atlas pass) went into the recording
//             as it came, so it executes BEFORE the merged pass. That reorder is exact while the foreign commands touch nothing the
//             chain writes or has read: every entry point of vkn_pass checks it (tl_touch, stamps on vkn_tex) and a conflict writes
//             the chain out FIRST, in the engine's order (a "miss").
//   misses    a chain that does not complete (other targets, a draw without a twin, a conflict, the recording's end) is written
//             out with the SAFE pass: the same subpass structure - so the same pipelines and secondary buffers - with every
//             attachment stored; the engine's later passes then read the textures as they always did. Render-pass compatibility
//             ignores load / store ops, which is what makes the choice at the END possible.
//   shaders   the three full-screen draws need input-attachment twins of their fragment modules (vkn_shlib_named_tile: built-in
//             modules proven bit for bit against the modules they replace) and the run-time conditions those proofs assume
//             (render scale 1; LinearizeDepth: NEAREST sampler and the coordinate at the own pixel centre, proven from the draw's
//             vertices).
// Which passes form a chain is LEARNED from the frames drawn the classic way (the observer): a set with colours and depth,
// then sets whose single draw has a twin whose inputs are attachments of the chain so far (or textures of the same size, taken in as
// read-only attachments). Two equal frames in a row arm it; the pipelines of the G-buffer draws for subpass 0 are built ahead, a few
// ms a frame, before the first merged frame (no first-sight stall). Stores are discarded by the store history of stage 5.2
// (DM_VK_NATIVE_STORE): an input read inside the pass is not a read of the stored content.
// Modes: 0 off / 1 on / 2 verify (the merged pass with every attachment stored, then the three draws once more the classic way into
// scratch targets, both read back and compared: "differ" must stay 0) / 3 = 1 + poison (discarded attachments magenta after the pass,
// attachments begun without LOAD magenta before their first draw). Recording thread only.

// The switch: resolved once (the texture piece asks at the first target it creates: vkn_tile_usage). Default: the render-pass tilers.
static int g_tl_cfg = -1, g_tl_env;   // (atomic: the texture piece asks from the engine's threads)
static int tl_cfg(void) {
    const int c = __atomic_load_n(&g_tl_cfg, __ATOMIC_ACQUIRE);
    if (c >= 0) return c;
    const vkn_dev *d = vkn_device(); if (!d) return 0;
    const int f = policy_family(); int env = 0;
    const int v = vkn_policy_tilelight(f, vkn_drv()->driver_id, &env);   // [22.11] default 0 everywhere (vkn_policy.h): the first armed chain crashed in the driver (phone run D02); back to the tiler policy once confirmed
    __atomic_store_n(&g_tl_env, env, __ATOMIC_RELEASE); __atomic_store_n(&g_tl_cfg, v, __ATOMIC_RELEASE);   // (the same value from whichever thread comes first)
    return v;
}
int vkn_tile_usage(void) { return tl_cfg() > 0; }
static int g_tl_latched;   // the recording thread took the switch (host tests set the mode themselves: vkn_tile_mode_set)
void vkn_tile_mode_set(int mode) { __atomic_store_n(&g_tl_cfg, mode < 0 ? -1 : mode, __ATOMIC_RELEASE); g_tl_mode = mode > 0 ? mode : 0; g_tl_latched = mode >= 0; pol_apply(); }
static void tl_hook_upd(void) {
    vkn_tile_hook = g_tl_mode && ((g_ch.st == TL_SUSP) || (g_ch.st == TL_REC && g_ch.cur >= 1) || (g_ch.st == TL_NONE && g_ob.active && g_ob.edge));
}
static VkCommandBuffer pcb(void) { return g_tl_outer || g_tl_redraw ? vkn_cb_primary() : vkn_cb(); }   // where commands outside a pass go ([P10 fix 3] the redraw's too)

// ---- twins: the input-attachment form of a shader object (made once per base object; a refusal is kept too) ----
// code: the vertex retype code the twin's own vertex module carries (0 = the base module; a draw whose effect gives it a vertex module
// of its own asks with 0: that module is the vertex stage, the twin's is never drawn).
typedef struct tl_twin { const vkn_shader *base; uint64_t handle, code; uint32_t id; const vkn_shader *tw; vkn_tile_var var; int8_t state; char v; } tl_twin;
static tl_twin g_tw[TL_TWINS]; static uint32_t g_ntw, g_tw_ring;
typedef struct tl_job { const char *key; char v; xbg_vkn_stage *st; uint64_t decl[4]; vkn_tile_var *var; int rc; } tl_job;
static void *tl_job_run(void *a) { tl_job *j = (tl_job *)a; j->rc = vkn_shlib_named_tile(j->key, j->v, j->st, j->decl, j->var); return NULL; }
// Host tests / other sources of twin modules: fn(base, variant, &fs module, declared bindings, var) -> 0 ok. NULL = the shader library.
static int (*g_twin_fn)(const vkn_shader *base, char v, VkShaderModule *fs, uint64_t declared[4], vkn_tile_var *var);
void vkn_tile_twin_fn(int (*fn)(const vkn_shader *base, char v, VkShaderModule *fs, uint64_t declared[4], vkn_tile_var *var)) { g_twin_fn = fn; g_ntw = 0; g_tw_ring = 0; }
static int tl_twin_load(const vkn_shader *base, const char *key, char v, VkShaderModule *fs, uint64_t decl[4], vkn_tile_var *var) {
    if (g_twin_fn) return g_twin_fn(base, v, fs, decl, var);
    if (!vkn_shlib_named_tile) return -1;
    xbg_vkn_stage *st = (xbg_vkn_stage *)calloc(1, sizeof *st); if (!st) return -1;
    tl_job j; memset(&j, 0, sizeof j); j.key = key; j.v = v; j.st = st; j.var = var; j.rc = -1;
    pthread_attr_t at; pthread_t th;   // (the library's load-time rewrites want an 8 MB stack: a thread of its own, once per twin)
    if (!pthread_attr_init(&at)) {
        pthread_attr_setstacksize(&at, 8u << 20);
        if (!pthread_create(&th, &at, tl_job_run, &j)) pthread_join(th, NULL);
        pthread_attr_destroy(&at);
    }
    if (!j.rc) { *fs = st->module; memcpy(decl, j.decl, sizeof j.decl); }
    free(st);
    return j.rc;
}
static tl_twin *tl_twin_get(const vkn_shader *base, char v, uint64_t code) {
    for (uint32_t i = 0; i < g_ntw; i++) if (g_tw[i].base == base && g_tw[i].handle == base->handle && g_tw[i].id == base->id && g_tw[i].v == v && g_tw[i].code == code) return &g_tw[i];
    // a new entry: a free one, else a refusal (cheap to find again), else the oldest (a live twin goes only when 64 of them are live:
    // a twin made again is another object, and the learned chain names the object it was learned with)
    uint32_t k = g_ntw < TL_TWINS ? g_ntw++ : TL_TWINS;
    for (uint32_t i = 0; k == TL_TWINS && i < TL_TWINS; i++) if (g_tw[(g_tw_ring + i) % TL_TWINS].state <= 0) k = (g_tw_ring + i) % TL_TWINS;
    if (k == TL_TWINS) k = g_tw_ring % TL_TWINS;
    g_tw_ring = (k + 1) % TL_TWINS;
    tl_twin *e = &g_tw[k]; memset(e, 0, sizeof *e);
    e->base = base; e->handle = base->handle; e->id = base->id; e->v = v; e->code = code; e->state = -1;
    const vkn_shader *eng = base; char lv = 0; const char *key = base->key[VKN_SRC_FS]; const char *why = NULL;
    VkShaderModule fs = VK_NULL_HANDLE; uint64_t decl[4] = {0, 0, 0, 0};
    vkn_bind tab[64]; uint32_t n = 0;
    if (vkn_fx_shader_is(base)) {   // a piece's shader: only the fused light has twins
        if (!vkn_lf_fused_of || vkn_lf_fused_of(base, &eng, &lv) || !eng) why = "a piece's shader that is not the fused light";
        else key = eng->key[VKN_SRC_FS];
    }
    if (!why && (!key[0] || tl_twin_load(base, key, v, &fs, decl, &e->var) || !fs || !e->var.n || e->var.n > 16)) why = "no twin module (the shader library has none for this module, or refused it)";
    for (uint32_t i = 0; !why && i < base->nbind && n < 64; i++) {
        vkn_bind t = base->bind[i];
        if (t.count != 1) { why = "a binding with more than one descriptor"; break; }
        if (t.bundle_stage == 4 && t.binding < 256 && !(decl[t.binding >> 6] >> (t.binding & 63) & 1)) continue;   // the twin stopped reading it
        for (uint32_t k = 0; k < e->var.n && t.bundle_stage == 4; k++) if (e->var.binding[k] == t.binding) {
            if (t.kind != VKN_BK_TEXTURE && t.kind != VKN_BK_FX_TEX) why = "an input of the twin is not a texture binding of the shader";
            t.type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT; t.flags &= ~VKN_BF_CMP;
        }
        tab[n++] = t;
    }
    for (uint32_t k = 0; !why && k < e->var.n; k++) {   // every input of the twin must be a texture binding of the base table
        int found = 0; for (uint32_t i = 0; i < n; i++) found |= tab[i].bundle_stage == 4 && tab[i].binding == e->var.binding[k] && tab[i].type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        if (!found || e->var.index[k] >= VKN_MP_INPUTS) why = "an input of the twin has no binding in the shader's table";
    }
    if (!why) {
        vkn_fx_shader_src src; memset(&src, 0, sizeof src);
        src.name = v == 'F' || v == 'f' ? "tile-light-f" : v == 'G' || v == 'g' ? "tile-light-g" : "tile-twin";
        // (the vertex module the classic draw has: the retype variant of the engine format, as vkn_draw.c picks it for code)
        src.vs = vkn_fx_shader_is(base) ? base->module[VKN_SRC_VS] : vkn_shader_module(base, VKN_STAGE_VS, 0, 0.0f, code);
        src.fs = fs; src.bind = tab; src.nbind = n; src.c1_from = base;
        src.ninputs = base->ninputs; memcpy(src.inputs, base->inputs, sizeof src.inputs); memcpy(src.input_classes, base->input_classes, sizeof src.input_classes);
        if (!src.vs || !(e->tw = vkn_fx_shader_create(&src))) why = "no vertex module / the binding table was refused";
    }
    if (why) { vkn_logf("tile pass: no twin for shader %s%s%c: %s", key, v ? " variant " : "", v ? v : ' ', why); return e; }
    e->state = 1; g_s6.twins++;
    vkn_logf("tile pass: twin of shader %s%s%c: %u input attachments, %u bindings, conditions %#x, vertex retype %#llx", key, v ? " variant " : "", v ? v : ' ', e->var.n, n, e->var.cond,
             (unsigned long long)code);
    return e;
}
// The texture a binding of `base` will read in this draw (NULL = none / not a texture binding).
static vkn_tex *tl_bind_tex(const vkn_shader *base, uint32_t binding, const vkn_draw_in *in, const vkn_fx_out *fx, int32_t *sid) {
    for (uint32_t i = 0; i < base->nbind; i++) {
        const vkn_bind *b = &base->bind[i]; if (b->binding != binding || b->bundle_stage != 4) continue;   // (a fragment-stage binding)
        if (b->kind == VKN_BK_TEXTURE) {
            const int st = b->stage == 0 ? 0 : b->stage == 1 ? 1 : -1; vkn_tex *t = NULL;
            if (st < 0 || !in->tex[st] || b->slot >= VKN_TEX_SLOTS || in->tex[st][b->slot] < 0) return NULL;
            if (sid) *sid = in->smp[st] ? in->smp[st][b->slot] : -1;
            (void)vkn_tex_view_by_id((uint32_t)in->tex[st][b->slot], &t);
            return t;
        }
        if (b->kind == VKN_BK_FX_TEX) return fx && b->slot < VKN_FX_TEX ? fx->tex[b->slot].tex : NULL;
        return NULL;
    }
    return NULL;
}
// LinearizeDepth's condition: the varying at `loc` is the own pixel's centre in texels at every pixel. Both the position and the
// varying are straight-line programs over the draw's vertex data; a triangle's varying is linear in screen space when every w is 1,
// so equality at the vertices (within 1/64 px) is equality inside; a NEAREST sampler then reads the own texel (margin 0.48 px).
static int tl_eval(const ffacts *f, const vkn_shader *sh, const nr_cv_prog *pr, float out[][4], uint32_t *np) {
    const uint32_t cnt = f->count; uint32_t n = 0;
    if (f->prim_in != 1 && f->prim_in != 3) return -1;
    if (cnt < 3 || cnt > NR_CV_MAX_VERTS) return -1;
    const vkn_vfmt *vf = f->vfmt ? vkn_vfmt_info(f->vfmt) : NULL;
    struct { const uint8_t *base; uint64_t size, stride, off; uint32_t nc; } inp[16]; memset(inp, 0, sizeof inp);
    for (uint32_t m = pr->in_mask; m; m &= m - 1) {
        const uint32_t loc = (uint32_t)__builtin_ctz(m);
        if (loc >= 16 || !vf) return -1;
        const VkVertexInputAttributeDescription *a = NULL;
        for (uint32_t k = 0; k < vf->nattr; k++) if (vf->attr[k].location == loc) a = &vf->attr[k];
        const uint32_t nc = !a ? 0 : a->format == VK_FORMAT_R32_SFLOAT ? 1 : a->format == VK_FORMAT_R32G32_SFLOAT ? 2 : a->format == VK_FORMAT_R32G32B32_SFLOAT ? 3 : a->format == VK_FORMAT_R32G32B32A32_SFLOAT ? 4 : 0;
        if (!nc || a->binding >= VKN_VFMT_STREAMS) return -1;
        const VkVertexInputBindingDescription *bd = NULL;
        for (uint32_t k = 0; k < vf->nbind; k++) if (vf->bind[k].binding == a->binding) bd = &vf->bind[k];
        if (!bd || bd->inputRate != VK_VERTEX_INPUT_RATE_VERTEX || !bd->stride) return -1;
        vkn_buf_info bi; memset(&bi, 0, sizeof bi);
        if (!f->vb[a->binding] || vkn_buf_lookup(VKN_BUF_VB, f->vb[a->binding], &bi) || !bi.blk.cpu) return -1;
        inp[loc].base = (const uint8_t *)bi.blk.cpu; inp[loc].size = bi.size; inp[loc].stride = bd->stride; inp[loc].off = (uint64_t)f->vb_off[a->binding] + a->offset; inp[loc].nc = nc;
    }
    const uint8_t *ub[NR_CV_MAX_UBO]; size_t ubn[NR_CV_MAX_UBO]; memset(ub, 0, sizeof ub); memset(ubn, 0, sizeof ubn);
    for (uint32_t u = 0; u < pr->nubo && u < NR_CV_MAX_UBO; u++) {
        const vkn_bind *b = NULL;
        for (uint32_t k = 0; k < sh->nbind; k++) if (sh->bind[k].binding == pr->ubo[u]) b = &sh->bind[k];
        if (!b || b->stage != 0) return -1;
        if (b->kind == VKN_BK_UNIFORM) {
            uint32_t off = 0, size = 0; const uint8_t *ring = (const uint8_t *)vkn_ring_cpu();
            if (!ring || vkn_draw_ubo_get(0, b->slot, &off, &size) || !size) return -1;
            ub[u] = ring + off; ubn[u] = b->bytes && b->bytes < size ? b->bytes : size;
        } else if (b->kind == VKN_BK_C1 && sh->c1[VKN_SRC_VS].size && sh->c1[VKN_SRC_VS].cpu) {
            ub[u] = (const uint8_t *)sh->c1[VKN_SRC_VS].cpu; ubn[u] = (size_t)sh->c1[VKN_SRC_VS].size;
        } else return -1;
        if (ubn[u] < pr->ubo_bytes[u]) return -1;
    }
    vkn_buf_info ib; memset(&ib, 0, sizeof ib); uint32_t isz = 0;
    if (f->indexed) { if (!f->ib || vkn_buf_lookup(VKN_BUF_IB, f->ib, &ib) || !ib.blk.cpu) return -1; isz = ib.index_size == 4 ? 4 : 2; }
    for (uint32_t k = 0; k < cnt; k++) {
        uint64_t v;
        if (f->indexed) {
            const uint64_t at = ((uint64_t)f->first + k) * isz; if (at + isz > ib.size) return -1;
            const uint8_t *q = (const uint8_t *)ib.blk.cpu + at;
            if (isz == 2) { uint16_t x; memcpy(&x, q, 2); v = x; } else { uint32_t x; memcpy(&x, q, 4); v = x; }
        } else v = (uint64_t)f->first + k;
        float iv[32][4];
        for (uint32_t m = pr->in_mask; m; m &= m - 1) {
            const uint32_t loc = (uint32_t)__builtin_ctz(m); const uint64_t at = inp[loc].off + v * inp[loc].stride;
            if (at + 4u * inp[loc].nc > inp[loc].size) return -1;
            iv[loc][0] = iv[loc][1] = iv[loc][2] = 0.0f; iv[loc][3] = 1.0f; memcpy(iv[loc], inp[loc].base + at, 4u * inp[loc].nc);
        }
        if (!nr_cv_eval(pr, (const float (*)[4])iv, ub, ubn, out[n])) return -1;
        n++;
    }
    *np = n;
    return 0;
}
static int tl_center_ok(const vkn_shader *sh, uint32_t loc, uint32_t W, uint32_t H) {
    const ffacts *f = &g_ff;
    if (!f->armed || f->stamp != draw_stamp() || f->sh != sh || !same_set(&f->set, &g_cur)) return 0;
    vkn_pass_cover_sh sf; memset(&sf, 0, sizeof sf);
    if (!g_shfacts || g_shfacts(sh, &sf) || !sf.pos || !sf.pos->ok) return 0;
    nr_cv_prog uv; memset(&uv, 0, sizeof uv);
    if (g_uv_fn ? g_uv_fn(sh, loc, &uv) : (!vkn_shlib_vs_out_prog || vkn_shlib_vs_out_prog(sh, loc, &uv))) return 0;
    if (!uv.ok) return 0;
    uv.out[2] = uv.out[0]; uv.out[3] = uv.out[0];   // (only x, y are the varying's)
    float P[NR_CV_MAX_VERTS][4], U[NR_CV_MAX_VERTS][4]; uint32_t n = 0, m = 0;
    if (tl_eval(f, sh, sf.pos, P, &n) || tl_eval(f, sh, &uv, U, &m) || n != m || n < 3) return 0;
    const float vx = f->vp[0], vy = f->vp[1] + f->vp[3], vw = f->vp[2], vh = -f->vp[3];   // (the Vulkan viewport the draw path sets)
    double area = 0.0;
    for (uint32_t k = 0; k < n; k++) {
        if (P[k][3] != 1.0f) return 0;
        const double xs = vx + (P[k][0] * 0.5 + 0.5) * vw, ys = vy + (P[k][1] * 0.5 + 0.5) * vh;
        if (!(fabs((double)U[k][0] * W - xs) <= 1.0 / 64) || !(fabs((double)U[k][1] * H - ys) <= 1.0 / 64)) return 0;
        if (k >= 2) area += fabs((double)(P[k - 1][0] - P[0][0]) * (P[k][1] - P[0][1]) - (double)(P[k][0] - P[0][0]) * (P[k - 1][1] - P[0][1]));
    }
    return area > 1e-6;
}
enum { TLV_ALPHA, TLV_PIECE, TLV_NINPUT, TLV_TWIN, TLV_SCALE, TLV_INPUT, TLV_SELF, TLV_SAMPLER, TLV_CENTER, TLV_PLAIN, TLV_FX, TLV_CRETYPE, TLV_N };
static const char *const k_tlv[TLV_N] = {"an alpha test on the draw (its fragment module is the \"#at\" variant)", "a piece's shader that is not the fused light",
    "more input attachments than the device takes in a stage", "no twin for its shader", "render scale is not 1 in the support block",
    "an input texture of another size / without the input usage / dead", "an input is a target of the same pass",
    "LinearizeDepth: the depth sampler is not NEAREST", "LinearizeDepth: the coordinate is not proven to be the own pixel's centre", "a chain texture is also read as a plain sampler",
    "an effect drops the draw's fragment stage or discards it", "LinearizeDepth: a vertex retype (the centre proof reads the engine format as it is)"};
static uint32_t g_tlv_n[TLV_N];
static int tl_no(const vkn_shader *s, int why) {   // a verdict refused: counted, the first two of each kind logged (which draw of the frame has no twin, and why)
    if (g_tlv_n[why]++ < 2 && why != TLV_TWIN) vkn_logf("tile pass: draw of shader %s (first after a target change) is no subpass of a chain: %s", s->key[VKN_SRC_FS][0] ? s->key[VKN_SRC_FS] : s->key[VKN_SRC_VS], k_tlv[why]);
    return -1;
}
// The verdict for the draw being prepared: its twin and what each input index reads, when every condition holds. stamp: the epoch the
// chain's / the candidate's attachments carry. An input whose texture is not of the chain is taken in as a read-only attachment
// (*ext) when it has the chain's size, one level / layer / sample and the input usage. 0 = g_tpend filled.
// The vertex side: the twin replaces the FRAGMENT module only, so whatever vertex stage the classic draw has stays exact with it -
//   * a per-draw vertex module of an effect (vkn_fx_out.vin: PRESKIN's consumer, XLQ's trimmed module; each exact by its own proof, and
//     the draw path draws it whatever object the chain ends with) stays the vertex stage of the twin's draw. PRESKIN plans the vertex
//     shaders of LinearizeDepth (06b17b62), CascadedPCF (54c2aae4) and the light (fc568237) on the tilers: refusing it kept every chain
//     from forming (phone run M03, 22.11);
//   * a vertex retype without such a module: the twin is made with the retype variant of the base vertex module (vkn_shader_module(base,
//     VS, code)), the module the classic draw binds. The centre proof of LinearizeDepth reads the engine format as it is: refused there.
// Refused: the alpha test (the classic draw's fragment module is the "#at" variant, the twin was proven against the base one), an effect
// that drops the fragment stage or discards the draw.
static int tl_verdict(const vkn_shader *s, const vkn_draw_in *in, const vkn_fx_out *fx, uint64_t retype, uint32_t stamp, uint32_t W, uint32_t H) {
    memset(&g_tpend, 0, sizeof g_tpend);
    if (in->alpha_ref >= 0.0f) return tl_no(s, TLV_ALPHA);
    if (fx && (fx->discard || fx->no_fs)) return tl_no(s, TLV_FX);
    const uint64_t code = fx && fx->vin.vs ? 0 : retype;   // (the effect's vertex module was made for the draw's retype code)
    char v = 0;
    if (vkn_fx_shader_is(s)) {
        const vkn_shader *l = NULL;
        if (!vkn_lf_fused_of || vkn_lf_fused_of(s, &l, &v)) return tl_no(s, TLV_PIECE);
        // the short form (Sky reads the Light part's textures) when the pairs are the same images, else the long one if the device takes 11 inputs
        const vkn_tex *a0 = tl_bind_tex(s, 200, in, fx, NULL), *a1 = tl_bind_tex(s, 206, in, fx, NULL), *a2 = tl_bind_tex(s, 212, in, fx, NULL), *a3 = tl_bind_tex(s, 224, in, fx, NULL);
        const int share = a0 && a0 == tl_bind_tex(s, 10, in, fx, NULL) && a1 == tl_bind_tex(s, 22, in, fx, NULL) && a2 == tl_bind_tex(s, 16, in, fx, NULL) && a3 == tl_bind_tex(s, 34, in, fx, NULL);
        const vkn_dev *d = vkn_device();
        if (share) v = (char)(v - 32);
        else if (!d || d->props.limits.maxPerStageDescriptorInputAttachments < 11) return tl_no(s, TLV_NINPUT);
    }
    tl_twin *e = tl_twin_get(s, v, code);
    if (!e || e->state <= 0) return tl_no(s, TLV_TWIN);
    const vkn_tile_var *var = &e->var;
    const vkn_dev *dv = vkn_device();
    if (dv && var->n > dv->props.limits.maxPerStageDescriptorInputAttachments) return tl_no(s, TLV_NINPUT);
    if (var->cond & VKN_TILE_C_SHARE) { if (!(v == 'F' || v == 'G')) return tl_no(s, TLV_TWIN); }
    // the support block of this draw: every render-scale float is 1.0. The block is std140 (common.glsl: `float _m5[73]` at byte 192 in a
    // uniform block of 1392 bytes): _m5[i] is the float at byte 192 + 16 i (xbg_vulkan.cpp xbg_support_defaults writes exactly those).
    // [P10 fix 2, run C1 22.11] the check read 73 floats in a row from byte 192 - _m5[0], then the zero padding after it: every draw of
    // the chain was refused ("render scale is not 1") although the block holds 1.0 in every _m5
    if (var->cond & VKN_TILE_C_SCALE1) {
        float sb[348];
        if (g_scale_fn ? g_scale_fn(in->viewport[2], in->viewport[3]) : (!vkn_shader_support_fill || vkn_shader_support_fill(in->viewport[2], in->viewport[3], sb, sizeof sb))) return tl_no(s, TLV_SCALE);
        if (!g_scale_fn) for (int i = 0; i < 73; i++) if (sb[48 + 4 * i] != 1.0f) return tl_no(s, TLV_SCALE);
    }
    memset(g_tpend.input, 0xff, sizeof g_tpend.input);
    for (uint32_t k = 0; k < var->n; k++) {
        int32_t sid = -1; vkn_tex *t = tl_bind_tex(s, var->binding[k], in, fx, &sid);
        if (!t || t->dead || t->w != W || t->h != H || t->mips != 1 || t->layers != 1 || t->samples > 1 || t->nvn_target == 2 || !(t->usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) || !t->view) return tl_no(s, TLV_INPUT);
        if (in_set(&g_cur, t)) return tl_no(s, TLV_SELF);   // (an input that is a target of its own subpass)
        if ((var->cond & VKN_TILE_C_CENTER) && k == 0) {
            if (retype) return tl_no(s, TLV_CRETYPE);
            if (sid < 0 || !vkn_sampler_nearest_by_id((uint32_t)sid)) return tl_no(s, TLV_SAMPLER);
            if (!tl_center_ok(s, var->center_loc, W, H)) return tl_no(s, TLV_CENTER);
        }
        if (t->tl_ep == stamp && stamp) g_tpend.input[var->index[k]] = t->tl_i;
        else { g_tpend.input[var->index[k]] = 0xfe; g_tpend.ext[var->index[k]] = t; }
        if (var->index[k] + 1u > g_tpend.ninput) g_tpend.ninput = (uint8_t)(var->index[k] + 1);
    }
    for (uint32_t i = 0; i < e->tw->nbind; i++) {   // nothing else of the draw may read a chain texture (it would be a plain sample of an attachment)
        const vkn_bind *b = &e->tw->bind[i];
        if ((b->kind != VKN_BK_TEXTURE && b->kind != VKN_BK_FX_TEX) || b->type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) continue;
        const vkn_tex *t = tl_bind_tex(s, b->binding, in, fx, NULL);
        if (t && stamp && t->tl_ep == stamp) return tl_no(s, TLV_PLAIN);
    }
    g_tpend.tw = e->tw; g_tpend.ok = 1;
    return 0;
}

// ---- the observer: a frame drawn the classic way teaches the chain ----
static void tl_sh_drop(tl_shape *s) {
    if (s->held) { for (uint32_t j = 0; j < s->natt; j++) vkn_tex_unref(s->att[j]); }
    memset(s, 0, sizeof *s);
}
static int tl_sh_same(const tl_shape *a, const tl_shape *b) {
    if (a->nsub != b->nsub || a->natt != b->natt || a->prefix != b->prefix || a->W != b->W || a->H != b->H) return 0;
    for (uint32_t k = 0; k < a->nsub; k++) {
        if (!same_set(&a->sub[k], &b->sub[k]) || a->ninput[k] != b->ninput[k] || a->twin[k] != b->twin[k] || memcmp(a->input[k], b->input[k], a->ninput[k])) return 0;
    }
    for (uint32_t j = 0; j < a->natt; j++) if (a->att[j] != b->att[j] || a->ext[j] != b->ext[j]) return 0;
    return 1;
}
static int tl_att_add(tl_shape *s, vkn_tex *t, uint32_t stamp, int ext) {
    if (t->tl_ep == stamp) return t->tl_i;
    if (s->natt >= TL_ATTS) return -1;
    t->tl_ep = stamp; t->tl_i = s->natt; s->att[s->natt] = t; s->ext[s->natt] = (uint8_t)ext;
    return s->natt++;
}
static void tl_ob_finalize(void) {
    if (!g_ob.active) return;
    g_ob.active = 0; g_ob.edge = 0;
    tl_shape *c = &g_ob.s;
    if (g_ob.cur >= 1 && !g_ob.cur_draws_ok) c->nsub = (uint8_t)g_ob.cur;
    if (c->nsub >= 2) {
        const uint32_t f = (uint32_t)vkn_frame();
        if (g_tsh.live && tl_sh_same(&g_tsh, c)) { if (g_tsh.frame + 1 == f && g_tsh.streak < 255) g_tsh.streak++; else if (g_tsh.frame != f) g_tsh.streak = 1; g_tsh.frame = f; }
        else {
            int ok = 1; uint32_t nh = 0;
            for (uint32_t j = 0; j < c->natt && ok; j++) { if (vkn_tex_ref(c->att[j])) nh++; else ok = 0; }
            if (!ok) { for (uint32_t j = 0; j < nh; j++) vkn_tex_unref(c->att[j]); }
            else {
                tl_sh_drop(&g_tsh); g_tsh = *c; g_tsh.live = 1; g_tsh.held = 1; g_tsh.streak = 1; g_tsh.frame = f; g_tsh.primed = 0; g_s6.learned++;   // (another chain: no ban, no misses)
            }
        }
    }
    tl_hook_upd();
}
static int tl_plain_set(const tset *s, uint32_t *W, uint32_t *H) {   // every attachment: level 0, layer 0, one sample, one size, not 3D
    uint32_t w = 0, h = 0;
    for (uint32_t i = 0; i <= s->n; i++) {
        const vkn_tex *t = i < s->n ? s->c[i] : s->d; if (!t) continue;
        if (t->dead || t->samples > 1 || t->nvn_target == 2 || (i < s->n && (s->mip[i] || s->layer[i])) || (w && (t->w != w || t->h != h))) return 0;
        w = t->w; h = t->h;
    }
    *W = w; *H = h;
    return w != 0;
}
static uint8_t g_tl_prev_donly; static vkn_tex *g_tl_prev_d;   // the set before the current one was depth-only on this depth target
// begin_set of a classic pass for s (the pass is about to begin)
static void tl_ob_begin(const tset *s) {
    tl_shape *c = &g_ob.s; uint32_t W = 0, H = 0;
    uint64_t d, u; draw_counts(&d, &u);
    int ncol = 0; for (uint32_t i = 0; i < s->n; i++) ncol += s->c[i] != NULL;
    if (g_ob.active) {
        if (g_ob.cur >= 0 && same_set(s, &c->sub[g_ob.cur])) { if (g_ob.cur >= 1) g_ob.cur_draws_ok = 0; return; }   // (resumed: fine for subpass 0 only)
        int touch = 0; for (uint32_t i = 0; i <= s->n; i++) { const vkn_tex *t = i < s->n ? s->c[i] : s->d; if (t && t->tl_ep == g_ob.epoch) touch = 1; }
        if (g_tpend.ok && c->nsub < TL_SUBS && tl_plain_set(s, &W, &H) && W == c->W && H == c->H && !s->d) {   // the next subpass
            const uint32_t k = c->nsub; int ok = 1;
            for (uint32_t i = 0; i < s->n && ok; i++) if (s->c[i]) { const int j = (s->c[i]->usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) ? tl_att_add(c, s->c[i], g_ob.epoch, 0) : -1; ok = j >= 0 && !c->ext[j]; }
            for (uint32_t n = 0; n < g_tpend.ninput && ok; n++) {
                uint8_t a = g_tpend.input[n];
                if (a == 0xfe) { const int j = tl_att_add(c, g_tpend.ext[n], g_ob.epoch, 1); if (j < 0) ok = 0; else a = (uint8_t)j; }
                c->input[k][n] = a;
            }
            g_tpend.ok = 0;
            if (ok) { c->sub[k] = *s; c->ninput[k] = g_tpend.ninput; c->twin[k] = g_tpend.tw; c->nsub++; g_ob.cur = (int)k; g_ob.cur_draws_ok = 1; g_ob.d0 = d + u; return; }
            tl_ob_finalize(); return;
        }
        if (touch) { tl_ob_finalize(); return; }
        if (c->nsub > 1) return;   // foreign, and the candidate has a subpass already: it goes on
    }
    if (!s->d || !ncol || !tl_plain_set(s, &W, &H)) return;   // a new candidate: a set with colours and depth (one without a second subpass is replaced)
    for (uint32_t i = 0; i <= s->n; i++) { const vkn_tex *t = i < s->n ? s->c[i] : s->d; if (t && !(t->usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) return; }
    memset(&g_ob, 0, sizeof g_ob); c = &g_ob.s;
    g_ob.active = 1; g_ob.epoch = ++g_tl_epoch; g_ob.cur = 0; g_ob.cur_draws_ok = 1;
    c->nsub = 1; c->sub[0] = *s; c->W = W; c->H = H; c->prefix = g_tl_prev_donly && g_tl_prev_d == s->d;
    for (uint32_t i = 0; i < s->n; i++) if (s->c[i]) (void)tl_att_add(c, s->c[i], g_ob.epoch, 0);
    (void)tl_att_add(c, s->d, g_ob.epoch, 0);
}
// vkn_pass_targets: the set changes (old -> nw)
static void tl_on_targets(const tset *old, const tset *nw) {
    g_tl_prev_donly = (uint8_t)depth_only(old); g_tl_prev_d = old->d; g_tpend.ok = 0;
    vkn_tile_prime_compat = 0;
    if (g_ob.active) {
        if (g_ob.cur >= 1) { uint64_t d, u; draw_counts(&d, &u); if (d + u != g_ob.d0 + 1) g_ob.cur_draws_ok = 0; if (!g_ob.cur_draws_ok) { tl_ob_finalize(); (void)nw; return; } }
        g_ob.cur = -1; g_ob.edge = 1;
    }
    tl_hook_upd();
}

// ---- the chain at run time ----
static int tl_set_conflict(const tset *s) {
    for (uint32_t i = 0; i <= s->n; i++) { const vkn_tex *t = i < s->n ? s->c[i] : s->d; if (t && (t->tl_ep == g_ch.epoch || t->tl_rd == g_ch.epoch)) return 1; }
    return 0;
}
// A use of t outside the chain's own draws: a chain texture (or, for a write, one the chain's draws sampled) ends the chain first.
static void tl_touch(vkn_tex *t, int write) {
    if (g_ch.st == TL_NONE || !t || g_tl_outer) return;
    if (t->tl_ep == g_ch.epoch || (write && t->tl_rd == g_ch.epoch)) tl_flush(0, TLM_CONFLICT);
}
static void tl_sec_end(void) {   // the subpass buffer being recorded ends (the recording goes on in its own buffer)
    if (g_ch.st != TL_REC) return;
    SORT_FLUSH();
    VkCommandBuffer s = g_ch.nsec ? g_ch.sec[g_ch.nsec - 1] : VK_NULL_HANDLE;
    { uint64_t d, u; draw_counts(&d, &u); g_ch.draws += d + u - g_ch.sec_d0; }
    vkn_cb_route(VK_NULL_HANDLE);
    if (s && vkn_cb2_end(s)) { g_ch.nsec--; if (red_ok(&g_red_tl)) vkn_logf(RED "tile pass: a subpass buffer did not end: its draws are lost"); }
    if (vkn_draw_raw_begin) vkn_draw_raw_begin();
    g_open = 0; g_open_cb = VK_NULL_HANDLE; g_ch.st = TL_SUSP; g_ch.prefix_open = 0;
    b1_pass_closed();
}
// Subpass k begins (or goes on) in a new secondary buffer. prefix: the depth-only set before subpass 0 (colour masks 0). 0 = recording.
static int tl_sub_enter(uint32_t k, int prefix) {
    const tl_shape *sh = &g_tsh;
    if (g_ch.nsec >= TL_SECS) return -1;
    SORT_FLUSH();
    VkCommandBuffer s = vkn_cb2_begin(g_ch.rp, k, g_ch.fb);
    if (!s) return -1;
    g_ch.sec[g_ch.nsec] = s; g_ch.sec_sub[g_ch.nsec] = (uint8_t)k; g_ch.nsec++;
    vkn_cb_route(s);
    if (vkn_draw_raw_begin) vkn_draw_raw_begin();
    const tset *set = &sh->sub[k];
    // first-draw facts of this subpass's draw (the cover proof): a full-screen draw that writes every texel needs no LOAD
    cover_res cr; memset(&cr, 0, sizeof cr); int cw = -1;
    if (k >= 1 && g_pol.cur.load) cw = cover_why(set, sh->W, sh->H, 1, &cr);
    disarm();
    const int covered = cw == VKN_PCV_FULL && (g_pol.cur.load == 1 || g_pol.cur.load == 3);
    VkClearAttachment ca[VKN_PASS_COLORS + 1]; uint32_t nca = 0;
    for (uint32_t i = 0; i <= set->n; i++) {
        vkn_tex *t = i < set->n ? set->c[i] : set->d; const int dp = i == set->n;
        if (!t || (prefix && !dp)) continue;
        const uint32_t j = t->tl_i;
        if (!g_ch.a[j].entered) {   // its first use in the pass: how it begins
            g_ch.a[j].entered = 1;
            float rgba[4];
            if (dp) {
                if (t->pass_dclr & 3) { g_ch.a[j].clr = (uint8_t)(t->pass_dclr & 3); g_ch.a[j].cv.depthStencil.depth = t->pass_depth; g_ch.a[j].cv.depthStencil.stencil = t->pass_stencil; t->pass_dclr = 0; }
            } else if (!g_ch.a[j].clr) {
                if (clr_find(t, 0, rgba, 1)) { g_ch.a[j].clr = 1; g_ch.a[j].cv.color = clear_color(t->format, rgba); }
                else if (covered) g_ch.a[j].dc = 1;
            }
            if (g_tl_mode == 3 && g_ch.a[j].dc && !dp) { ca[nca].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca[nca].colorAttachment = i; ca[nca].clearValue.color = clear_color(t->format, (const float[4]){1.0f, 0.0f, 1.0f, 1.0f}); nca++; g_s6.poison++; }
        } else {   // a clear the engine asked for since its last subpass: now, inside the pass
            float rgba[4];
            if (dp && (t->pass_dclr & 3)) {
                ca[nca].aspectMask = ((t->pass_dclr & 1) && (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((t->pass_dclr & 2) && (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
                ca[nca].colorAttachment = 0; ca[nca].clearValue.depthStencil.depth = t->pass_depth; ca[nca].clearValue.depthStencil.stencil = t->pass_stencil;
                if (ca[nca].aspectMask) nca++;
                t->pass_dclr = 0;
            } else if (!dp && clr_find(t, 0, rgba, 1)) { ca[nca].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca[nca].colorAttachment = i; ca[nca].clearValue.color = clear_color(t->format, rgba); nca++; }
        }
    }
    if (nca) { const VkClearRect r = {{{0, 0}, {sh->W, sh->H}}, 0, 1}; vkCmdClearAttachments(s, nca, ca, 1, &r); g_st.clears_att += nca; }
    g_ch.st = TL_REC; g_ch.cur = (int)k; g_ch.prefix_open = (uint8_t)prefix; if (!prefix && k == 0) g_ch.entered0 = 1;
    g_open = 1; g_open_cb = s; g_open_serial = vkn_cb_serial(); g_open_set = *set;
    g_area = (VkRect2D){{0, 0}, {sh->W, sh->H}}; g_info = g_ch.info[k];
    b1_pass_closed();
    g_st.passes++;
    { uint64_t d, u; draw_counts(&d, &u); g_ch.sub_d0 = g_ch.sec_d0 = d + u; }
    return 0;
}
static uint8_t tl_idx(const tl_shape *sh, const vkn_tex *t) { for (uint32_t j = 0; j < sh->natt; j++) if (sh->att[j] == t) return (uint8_t)j; return 0xff; }
static void tl_key(vkn_mp_key *k, const tl_shape *sh) {   // structure only (ops zero: LOAD / STORE filled by the caller)
    memset(k, 0, sizeof *k);
    k->natt = sh->natt; k->nsub = sh->nsub; k->samples = 1;
    for (uint32_t j = 0; j < sh->natt; j++) { k->fmt[j] = (uint32_t)sh->att[j]->format; k->depth[j] = (uint8_t)is_depth(sh->att[j]); }
    for (uint32_t i = 0; i < sh->nsub; i++) {
        const tset *s = &sh->sub[i];
        k->sub[i].ncolor = (uint8_t)s->n; k->sub[i].ds = s->d ? tl_idx(sh, s->d) : 0xff; k->sub[i].ninput = sh->ninput[i];
        for (uint32_t c = 0; c < 8; c++) k->sub[i].color[c] = c < s->n && s->c[c] ? tl_idx(sh, s->c[c]) : 0xff;
        memcpy(k->sub[i].input, sh->input[i], sizeof k->sub[i].input);
    }
}
static int tl_shape_alive(const tl_shape *sh) {
    for (uint32_t j = 0; j < sh->natt; j++) { const vkn_tex *t = sh->att[j]; if (t->dead || t->w != sh->W || t->h != sh->H || !(t->usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) return 0; }
    return 1;
}
// Makes the shape's pass objects (any ops). 0 = ok.
static int tl_objects(tl_shape *sh, VkRenderPass *rp, VkFramebuffer *fb, uint16_t compat[TL_SUBS]) {
    vkn_mp_key k; tl_key(&k, sh);
    for (uint32_t j = 0; j < sh->natt; j++) { k.load[j] = k.sload[j] = VK_ATTACHMENT_LOAD_OP_LOAD; k.store[j] = k.sstore[j] = VK_ATTACHMENT_STORE_OP_STORE; }
    if (!vkn_mp_get(&k, rp, compat) || !*rp) return -1;
    VkImageView views[TL_ATTS];
    for (uint32_t j = 0; j < sh->natt; j++) if (!(views[j] = vkn_tex_att_view(sh->att[j], 0, 0))) return -1;
    *fb = vkn_drv()->fb_exact ? vkn_fb_get_rp(compat[0], *rp, views, sh->natt, sh->W, sh->H, 1) : vkn_fb_get(compat[0], views, sh->natt, sh->W, sh->H, 1);
    return *fb ? 0 : -1;
}
static void tl_infos(const tl_shape *sh) {
    for (uint32_t i = 0; i < sh->nsub; i++) {
        vkn_pass_info *f = &g_ch.info[i]; memset(f, 0, sizeof *f); const tset *s = &sh->sub[i];
        f->pass = g_ch.rp; f->compat = g_ch.compat[i]; f->ncolor = s->n; f->width = sh->W; f->height = sh->H; f->samples = 1;
        for (uint32_t c = 0; c < s->n; c++) if (s->c[c]) f->write_mask[c] = vkn_write_mask_nvn(s->c[c]->nvn_fmt);
        if (s->d) { f->has_depth = (s->d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0; f->has_stencil = (s->d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0; f->depth_format = s->d->format; }
    }
    g_ch.info_prefix = g_ch.info[0]; memset(g_ch.info_prefix.write_mask, 0, sizeof g_ch.info_prefix.write_mask);
}
// [frame F1] region clears wait for the pass of g_cur (DM_VK_NATIVE_LAZYCLR): that pass begins the classic way (the clears run in it);
// counted when it would have begun the learned chain.
static void tl_lazy_refuse(void) {
    const tl_shape *sh = &g_tsh;
    if (sh->live && (same_set(&g_cur, &sh->sub[0]) || (sh->prefix && depth_only(&g_cur) && g_cur.d == sh->sub[0].d))) { g_s6.tried++; g_s6.refused[TLR_LAZY]++; }
}
// vkn_pass_ready, no chain: the pass about to begin for g_cur starts the learned chain. 1 = recording in subpass 0 (*info filled).
static int tl_try_begin(vkn_pass_info *info) {
    tl_shape *sh = &g_tsh;
    if (!sh->live) return 0;
    const int pre = sh->prefix && depth_only(&g_cur) && g_cur.d == sh->sub[0].d;
    if (!pre && !same_set(&g_cur, &sh->sub[0])) return 0;
    const uint32_t f = (uint32_t)vkn_frame();
    int why = -1;
    if (sh->streak < 2 || sh->frame + 1 < f) why = TLR_HIST;
    else if (sh->ban_until > f) why = TLR_BAN;
    else if (sh->cool >= f) why = TLR_COOL;
    else if (!vkn_rec_presented()) why = TLR_OFFFRAME;
    else if (g_hint_on) why = TLR_HINT;
    else if (vkn_query_mode && vkn_query_mode() != 0) why = TLR_QUERY;
    else if (!tl_shape_alive(sh)) why = TLR_TARGET;
    g_s6.tried++;
    if (why >= 0) { g_s6.refused[why]++; return 0; }
    VkRenderPass rp = VK_NULL_HANDLE; VkFramebuffer fb = VK_NULL_HANDLE; uint16_t compat[TL_SUBS];
    if (tl_objects(sh, &rp, &fb, compat)) { g_s6.refused[TLR_PASS]++; return 0; }
    if (!sh->primed) {   // the G-buffer draws' pipelines for subpass 0 are built ahead while the frames are drawn the classic way
        if (g_tl_prime_frame != f) {   // one verdict a frame: a whole frame without a missing pipeline arms the chain
            if (g_tl_prime_frame + 1 == f && !sh->prime_missing && sh->prime_frames) sh->primed = 1;
            sh->prime_frames++; sh->prime_missing = 0; g_tl_prime_frame = f; g_tl_prime_ns = 0;
        }
        if (!sh->primed) { vkn_tile_prime_compat = compat[0]; vkn_tile_prime_rp = rp; g_s6.refused[TLR_PRIME]++; return 0; }
        vkn_logf("tile pass: armed - %u subpasses, %u attachments, %ux%u, %u pipelines built ahead in %u frames", sh->nsub, sh->natt, sh->W, sh->H, sh->prime_built, sh->prime_frames);
    }
    if (g_ob.active) { g_ob.active = 0; g_ob.edge = 0; }
    if (vkn_upload_pending_bytes()) (void)vkn_upload_run(vkn_cb());
    const uint32_t ep = ++g_tl_epoch;
    for (uint32_t j = 0; j < sh->natt; j++) { sh->att[j]->tl_ep = ep; sh->att[j]->tl_i = (uint8_t)j; }   // the chain's attachments, from here to its end
    memset(&g_ch, 0, sizeof g_ch);
    g_ch.epoch = ep; g_ch.rp = rp; g_ch.fb = fb; memcpy(g_ch.compat, compat, sizeof compat);
    g_vs.n = 0; g_vs.nscr = 0; vkn_tile_redraw_on = g_tl_mode == 2 && g_s6.begun % g_tl_vevery == 0;   // mode 2: this chain's full-screen draws are drawn twice
    tl_infos(sh);
    draw_counts(&g_ch.d0, &g_ch.u0);
    g_ch.st = TL_SUSP;
    if (tl_sub_enter(0, pre)) { g_ch.st = TL_NONE; g_ch.epoch = 0; g_s6.refused[TLR_PASS]++; return 0; }
    g_s6.begun++;
    if (info) *info = pre ? g_ch.info_prefix : g_ch.info[0];
    tl_hook_upd();
    return 1;
}
// vkn_pass_ready while a subpass is being recorded. 1 = this draw belongs to it.
static int tl_rec_ready(vkn_pass_info *info) {
    const tl_shape *sh = &g_tsh;
    if (g_ch.prefix_open) { if (depth_only(&g_cur) && g_cur.d == sh->sub[0].d) { disarm(); if (info) *info = g_ch.info_prefix; return 1; } }
    else if (same_set(&g_cur, &sh->sub[g_ch.cur])) {
        if (g_ch.cur >= 1) { uint64_t d, u; draw_counts(&d, &u); if (d + u != g_ch.sub_d0) { tl_flush(0, TLM_DRAWS); return 0; } }
        disarm(); if (info) *info = g_ch.info[g_ch.cur]; return 1;
    }
    tl_flush(0, TLM_OTHER);
    return 0;
}
// vkn_pass_ready while the chain waits: 1 = g_cur is its next (or resumed) subpass and the draw goes there; 0 = a foreign pass begins
// in the recording itself (the chain goes on waiting), or the chain was written out.
static int tl_susp_ready(vkn_pass_info *info) {
    const tl_shape *sh = &g_tsh; int k = -1, pre = 0;
    if (g_open && open_here() && same_set(&g_open_set, &g_cur)) return 0;   // (a foreign pass that is open already: checked when it began)
    if (g_ch.cur == 0 && same_set(&g_cur, &sh->sub[0])) k = 0;
    else if (g_ch.cur == 0 && !g_ch.entered0 && sh->prefix && depth_only(&g_cur) && g_cur.d == sh->sub[0].d) { k = 0; pre = 1; }
    else if (g_ch.cur + 1 < (int)sh->nsub && same_set(&g_cur, &sh->sub[g_ch.cur + 1])) {
        if (!g_ch.entered0) { tl_flush(0, TLM_TARGETS); return 0; }
        k = g_ch.cur + 1;
        if (!g_tpend.ok) { tl_flush(0, TLM_TWIN); return 0; }   // (a draw the hook did not approve: drawn the classic way)
    }
    if (k < 0) {
        if (tl_set_conflict(&g_cur)) tl_flush(0, TLM_TARGETS); else g_ch.foreign++;
        return 0;
    }
    if (tl_sub_enter((uint32_t)k, pre)) { const int twin = k >= 1; tl_flush(0, TLM_OTHER); return twin ? -1 : 0; }   // (-1: the draw is bound to its twin, no subpass: dropped)
    if (info) *info = pre ? g_ch.info_prefix : g_ch.info[k];
    tl_hook_upd();
    return 1;
}
// end_open while a subpass is being recorded: the targets changed / an unrelated command outside a pass comes -> the chain waits;
// the last subpass done -> written out complete; anything else -> written out as it stands.
static void tl_end_open(int why) {
    const int last = g_ch.cur + 1 == (int)g_tsh.nsub && !g_ch.prefix_open;
    if (why == VKN_PE_TARGETS || why == VKN_PE_LAYOUT || why == VKN_PE_COPY || why == VKN_PE_STORAGE || why == VKN_PE_SAMPLE || why == VKN_PE_BARRIER) {
        uint64_t d, u; draw_counts(&d, &u);
        tl_sec_end();
        if (last && d + u == g_ch.sub_d0 + 1) tl_flush(1, 0);
        else tl_hook_upd();
        return;
    }
    tl_flush(0, why == VKN_PE_END ? TLM_END : TLM_OTHER);
}
// A draw of the chain reads attachment t in place (the draw path, for an INPUT_ATTACHMENT binding of the twin). 0 = ok.
int vkn_pass_input(vkn_tex *t, VkImageView *view, VkImageLayout *layout) {
    if (!t || !g_tpend.ok || g_ch.st == TL_NONE || t->tl_ep != g_ch.epoch || !t->view) return -1;
    if (view) *view = t->view;
    if (layout) *layout = is_depth(t) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return 0;
}
// The draw path, before a draw's textures are bound (only while vkn_tile_hook): the twin to draw with, NULL = the shader as it is.
const struct vkn_shader *vkn_tile_twin(const struct vkn_shader *s, const vkn_draw_in *in, const struct vkn_fx_out *fx, uint64_t retype) {
    g_tpend.ok = 0;
    if (!g_tl_mode || !s || !in) return NULL;
    if (g_ch.st == TL_NONE) {   // the observer: the first draw after the targets changed
        if (!g_ob.active || !g_ob.edge) return NULL;
        g_ob.edge = 0; tl_hook_upd();
        // only a set that can be the candidate's next subpass needs a verdict (tl_ob_begin: no depth, colours only, the chain's size). A
        // set with depth - the G-buffer pass itself, a forward pass - is never a full-screen subpass: its draws are drawn as they are
        // (subpass 0 takes every draw of its set, alpha-tested, retyped or with an effect's vertex module), so no verdict, no twin asked
        uint32_t W = 0, H = 0;
        if (g_cur.d || !tl_plain_set(&g_cur, &W, &H) || W != g_ob.s.W || H != g_ob.s.H) return NULL;
        (void)tl_verdict(s, in, fx, retype, g_ob.epoch, g_ob.s.W, g_ob.s.H);
        return NULL;
    }
    const tl_shape *sh = &g_tsh;
    if (g_tl_redraw) return NULL;   // (mode 2: the classic form of the draw just recorded)
    if (g_ch.st == TL_REC) { if (g_ch.cur >= 1 && same_set(&g_cur, &sh->sub[g_ch.cur])) tl_flush(0, TLM_DRAWS); return NULL; }
    if (g_ch.cur + 1 >= (int)sh->nsub || !same_set(&g_cur, &sh->sub[g_ch.cur + 1])) return NULL;
    const int k = g_ch.cur + 1;
    if (g_tl_redraw || !g_ch.entered0 || tl_verdict(s, in, fx, retype, g_ch.epoch, sh->W, sh->H) || g_tpend.tw != sh->twin[k] || g_tpend.ninput != sh->ninput[k] ||
        memcmp(g_tpend.input, sh->input[k], g_tpend.ninput)) { g_tpend.ok = 0; if (!g_tl_redraw) tl_flush(0, TLM_TWIN); return NULL; }   // (0xfe: an input outside the learned attachments differs too)
    return g_tpend.tw;
}
int vkn_tile_prime_budget(void) { return g_tl_prime_ns < 3000000ull; }   // 3 ms of builds ahead a frame
void vkn_tile_prime_note(int built, uint64_t ns) { if (built) { g_tsh.prime_built++; g_tl_prime_ns += ns; g_s6.primed_pipes++; } else g_tsh.prime_missing++; }

// ---- verify (mode 2): the full-screen draws once more, the classic way, into scratch targets; both read back and compared ----
// At the draw (the draw path calls vkn_tile_redraw_begin right after it recorded a twin's draw, draws the same draw again under
// vkn_fx_force with the same effect output, then vkn_tile_redraw_end): the hook hands out no twin, the chain's textures are bound as
// plain samplers (their layout is made right when the chain is written out: everything is stored in mode 2), vkn_pass_ready answers
// the scratch pass, and the commands go into a secondary buffer of that pass. After the merged pass: the scratch pass runs, the
// subpass's targets and their scratch copies are read back, a retired job compares the bytes. A target the merged pass LOADs (the draw
// may not write all of it) is not compared (counted "skipped").
void vkn_tile_verify_every(uint32_t n) { g_tl_vevery = n ? n : 1; }
static tl_scr *tl_scratch(uint32_t i, VkFormat f, uint32_t w, uint32_t h) {
    const vkn_dev *d = vkn_device();
    if (i >= TL_SCR || !d) return NULL;
    tl_scr *s = &g_scr[i];
    if (s->img && s->f == f && s->w == w && s->h == h) return s;
    if (s->img) return NULL;   // (another size: verify of this target is skipped until the process ends - no destroy on the recording thread)
    const VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(d->dev, &ci, NULL, &s->img) != VK_SUCCESS || vkn_image_mem(s->img, &s->mem, NULL)) { s->img = VK_NULL_HANDLE; return NULL; }
    const VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = s->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(d->dev, &vi, NULL, &s->view) != VK_SUCCESS) { s->view = VK_NULL_HANDLE; return NULL; }
    s->f = f; s->w = w; s->h = h;
    return s;
}
int vkn_tile_redraw_begin(void) {
    if (!vkn_tile_redraw_on || g_tl_redraw || g_ch.st != TL_REC || g_ch.cur < 1 || !g_tpend.ok || g_vs.n >= TL_SUBS) return 0;
    const tl_shape *sh = &g_tsh; const tset *s = &sh->sub[g_ch.cur];
    tl_vsub *v = &g_vs.v[g_vs.n]; memset(v, 0, sizeof *v);
    vkn_rp_key k; vkn_rp_key_init(&k); VkImageView views[VKN_PASS_COLORS]; uint32_t nv = 0; const uint32_t scr0 = g_vs.nscr;
    for (uint32_t c = 0; c < s->n; c++) {
        vkn_tex *t = s->c[c]; tl_scr *q = NULL;
        const uint32_t j = t ? t->tl_i : 0;
        if (!t || (!g_ch.a[j].clr && !g_ch.a[j].dc && has_content(t)) || !(q = tl_scratch(g_vs.nscr, t->format, sh->W, sh->H))) { g_vs.nscr = scr0; g_s6.verify_skipped++; return 0; }
        g_vs.nscr++;
        k.color[c] = (uint32_t)t->format; k.load[c] = VK_ATTACHMENT_LOAD_OP_CLEAR; k.store[c] = VK_ATTACHMENT_STORE_OP_STORE;
        if (g_ch.a[j].clr) v->cv[c] = g_ch.a[j].cv;
        views[nv++] = q->view; v->real[c] = t; v->scr[c] = q;
    }
    k.ncolor = (uint8_t)s->n;
    uint16_t compat = 0;
    if (!nv || !vkn_rp_get(&k, &v->rp, &compat) || !v->rp || !(v->fb = vkn_fb_get(compat, views, nv, sh->W, sh->H, 1))) { g_vs.nscr = scr0; g_s6.verify_skipped++; return 0; }
    SORT_FLUSH();
    if (!(v->sec = vkn_cb2_begin(v->rp, 0, v->fb))) { g_vs.nscr = scr0; g_s6.verify_skipped++; return 0; }
    g_vs.back = g_ch.sec[g_ch.nsec - 1];
    vkn_cb_route(v->sec); if (vkn_draw_raw_begin) vkn_draw_raw_begin();
    v->sub = (uint8_t)g_ch.cur; v->n = (uint8_t)s->n;
    g_vs.info = g_ch.info[g_ch.cur]; g_vs.info.pass = v->rp; g_vs.info.compat = compat;
    { uint64_t d, u; draw_counts(&d, &u); v->d0 = d + u; }
    g_tl_redraw = 1;
    return 1;
}
void vkn_tile_redraw_end(void) {
    if (!g_tl_redraw) return;
    tl_vsub *v = &g_vs.v[g_vs.n];
    uint64_t d, u; draw_counts(&d, &u);
    SORT_FLUSH();
    if (vkn_cb2_end(v->sec) || d + u != v->d0 + 1) v->bad = 1;   // (the classic draw was dropped: nothing to compare)
    g_ch.sub_d0 += d + u - v->d0; g_ch.sec_d0 += d + u - v->d0;  // (the draw count of the subpass and of the journal: the engine's draws only)
    vkn_cb_route(g_vs.back); if (vkn_draw_raw_begin) vkn_draw_raw_begin();
    g_tl_redraw = 0; g_vs.n++;
}
typedef struct tl_vjob { vkn_block a, b; uint64_t n; uint32_t sub, slot; } tl_vjob;
static void tl_verify_done(void *ctx) {
    tl_vjob *j = (tl_vjob *)ctx;
    const uint8_t *pa = (const uint8_t *)j->a.cpu, *pb = (const uint8_t *)j->b.cpu; uint64_t diff = 0;
    if (pa && pb) for (uint64_t i = 0; i < j->n; i++) diff += pa[i] != pb[i];
    g_s6.verify_targets++; g_s6.verify_bytes += j->n; g_s6.verify_differ += diff;
    if (diff && red_ok(&g_red_tl)) vkn_logf(RED "tile pass verify: subpass %u colour %u: %llu of %llu bytes differ between the merged pass and the classic draw", j->sub, j->slot, (unsigned long long)diff, (unsigned long long)j->n);
    vkn_block_free(&j->a); vkn_block_free(&j->b); free(j);
}
static void tl_raw_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    const VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = from == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void tl_verify_emit(VkCommandBuffer cb) {
    const tl_shape *sh = &g_tsh; int any = 0;
    for (uint32_t i = 0; i < g_vs.n; i++) any |= !g_vs.v[i].bad;
    if (any) for (uint32_t j = 0; j < sh->natt; j++) {   // what the classic draws sample: readable (the merged pass stored everything)
        vkn_tex *t = sh->att[j];
        if (t->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) barrier(cb, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    for (uint32_t i = 0; i < g_vs.n; i++) {
        tl_vsub *v = &g_vs.v[i];
        if (v->bad) { g_s6.verify_skipped++; continue; }
        for (uint32_t c = 0; c < v->n; c++) tl_raw_barrier(cb, v->scr[c]->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        const VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = v->rp, .framebuffer = v->fb, .renderArea = {{0, 0}, {sh->W, sh->H}}, .clearValueCount = v->n, .pClearValues = v->cv};
        vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        vkCmdExecuteCommands(cb, 1, &v->sec);
        vkCmdEndRenderPass(cb);
        for (uint32_t c = 0; c < v->n; c++) {
            vkn_tex *t = v->real[c]; const uint64_t n = (uint64_t)sh->W * sh->H * fmt_bytes(t->format);
            tl_vjob *j = (tl_vjob *)calloc(1, sizeof *j);
            if (!j || vkn_block_alloc(VKN_ARENA_STAGING, n, 256, &j->a)) { free(j); g_s6.verify_skipped++; continue; }
            if (vkn_block_alloc(VKN_ARENA_STAGING, n, 256, &j->b)) { vkn_block_free(&j->a); free(j); g_s6.verify_skipped++; continue; }
            j->n = n; j->sub = v->sub; j->slot = c;
            barrier(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            tl_raw_barrier(cb, v->scr[c]->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy r = {.bufferOffset = j->a.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {sh->W, sh->H, 1}};
            vkCmdCopyImageToBuffer(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, j->a.buf, 1, &r);
            r.bufferOffset = j->b.off;
            vkCmdCopyImageToBuffer(cb, v->scr[c]->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, j->b.buf, 1, &r);
            vkn_retire(tl_verify_done, j);
        }
    }
    if (g_vs.n) {
        const VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        if (vkn_draw_raw_begin) vkn_draw_raw_begin();
    }
}

// ---- [P10 fix 6] the merged pass recorded at the submit: its stores decided by what the rest of the recording read ----
// The store history discards a store after 8 frames in which nothing read it; a reader that comes only sometimes (phone run G01: a
// forward draw sampled the shadow mask in frame 5113) then read undefined content - one wrong frame before the rule banned itself.
// Now a merged pass with a discarded store is not written into the recording at the chain's end: the recording is split there
// (vkn_rec_split) and the pass itself is recorded at the submit, between the part before and the part after. Every read of a discarded
// attachment recorded meanwhile (hist_read: sampled, loaded, copied, laid out for a transfer) turns its store back to STORE first
// (tl_dfr_revoke). So within the recording that holds the pass no reader ever sees a discarded attachment; what reaches the next
// recording is what the chain overwrites before anything reads it (the store history's own rule, which still bans on a violation).
static struct {
    int on; vkn_mp_key k; VkFramebuffer fb; uint32_t W, H, nsub, natt, nsec;
    VkClearValue cv[TL_ATTS]; vkn_tex *att[TL_ATTS]; uint8_t dead[TL_ATTS], poison[TL_ATTS]; uint64_t bytes[TL_ATTS];
    VkCommandBuffer sec[TL_SECS]; uint8_t sec_sub[TL_SECS];
    tset js;
} g_dfr;
static int tl_dfr_revoke(vkn_tex *t) {
    if (!g_dfr.on || !t) return 0;
    for (uint32_t j = 0; j < g_dfr.natt; j++) if (g_dfr.att[j] == t && g_dfr.dead[j]) {
        g_dfr.dead[j] = 0; g_dfr.poison[j] = 0; g_dfr.k.store[j] = VK_ATTACHMENT_STORE_OP_STORE; if (is_depth(t)) g_dfr.k.sstore[j] = VK_ATTACHMENT_STORE_OP_STORE;
        g_s6.store_revoked++; g_s6.merged_bytes += g_dfr.bytes[j]; if (g_s6.saved_bytes >= g_dfr.bytes[j]) g_s6.saved_bytes -= g_dfr.bytes[j];
        g_s3.store_bytes += g_dfr.bytes[j];
        return 1;
    }
    return 0;
}
// The commands of the merged pass (begin, its subpass buffers, end) and, mode 3, the poison of what it discarded. `restore`: the image
// layouts after the poison go back to the attachment layouts by raw barriers (the tail was recorded against those; the tracked layout
// does not change).
static void tl_record_pass(VkCommandBuffer cb, VkRenderPass rp, VkFramebuffer fb, uint32_t W, uint32_t H, uint32_t natt, const VkClearValue *cv, uint32_t nsub,
                           const VkCommandBuffer *sec, const uint8_t *sec_sub, uint32_t nsec, const tset *js, vkn_tex *const *att, const uint8_t *poison, int restore) {
    const VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = {{0, 0}, {W, H}}, .clearValueCount = natt, .pClearValues = cv};
    vkn_prof_pass_begin(cb, js->c, js->n, js->d, W, H);
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    for (uint32_t i = 0; i < nsub; i++) {
        if (i) vkCmdNextSubpass(cb, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
        VkCommandBuffer list[TL_SECS]; uint32_t n = 0;
        for (uint32_t q = 0; q < nsec; q++) if (sec_sub[q] == i) list[n++] = sec[q];
        if (n) vkCmdExecuteCommands(cb, n, list);
    }
    vkCmdEndRenderPass(cb);
    vkn_prof_pass_end(cb);
    for (uint32_t j = 0; j < natt; j++) {
        vkn_tex *t = att[j];
        if (!poison[j] || is_depth(t)) continue;   // mode 3: what was not stored is magenta for whoever reads it from memory
        const VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}; const VkClearColorValue m = clear_color(t->format, (const float[4]){1.0f, 0.0f, 1.0f, 1.0f});
        if (restore) tl_raw_barrier(cb, t->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        else barrier(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &m, 1, &rg); g_s6.poison++;
        if (restore) tl_raw_barrier(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
}
static void tl_fill(VkCommandBuffer mid, void *ctx) {   // (vkn_rec_submit, after the close listeners)
    (void)ctx;
    if (!g_dfr.on) return;
    g_dfr.on = 0;
    VkRenderPass rp = VK_NULL_HANDLE; uint16_t compat[TL_SUBS];
    if (!vkn_mp_get(&g_dfr.k, &rp, compat) || !rp) { g_st2.rp_fail++; if (red_ok(&g_red_tl)) vkn_logf(RED "tile pass: no render pass for the chain's final store ops: %u subpasses of draws lost", g_dfr.nsub); return; }
    tl_record_pass(mid, rp, g_dfr.fb, g_dfr.W, g_dfr.H, g_dfr.natt, g_dfr.cv, g_dfr.nsub, g_dfr.sec, g_dfr.sec_sub, g_dfr.nsec, &g_dfr.js, g_dfr.att, g_dfr.poison, 1);
    for (uint32_t j = 0; j < g_dfr.natt; j++) if (g_dfr.dead[j]) g_s6.stores_dropped++;
}

// ---- writing the chain out ----
static uint64_t tl_hash(const tl_shape *sh) { uint64_t h = 0x7110; for (uint32_t j = 0; j < sh->natt; j++) h = mix64(h, (uint64_t)(uintptr_t)sh->att[j]); return mix64(h, sh->nsub); }
static void tl_flush(int hit, int why) {
    if (g_ch.st == TL_NONE) return;
    tl_shape *sh = &g_tsh;
    if (g_ch.st == TL_REC) tl_sec_end();
    else if (g_open) { g_ch.st = TL_SUSP; END_W(VKN_PE_OTHER); }   // (a foreign pass open in the recording ends first)
    g_ch.st = TL_NONE;   // (from here every helper below runs as outside a chain)
    VkCommandBuffer cb = vkn_cb_primary();
    const uint32_t nsub = sh->nsub, W = sh->W, H = sh->H;
    const int safe = !hit || g_tl_mode == 2;
    vkn_mp_key k; tl_key(&k, sh);
    VkClearValue cv[TL_ATTS]; memset(cv, 0, sizeof cv);
    uint8_t stored[TL_ATTS], poison[TL_ATTS]; memset(stored, 0, sizeof stored); memset(poison, 0, sizeof poison);
    const uint64_t prefix0 = g_prefix; { const uint64_t h = tl_hash(sh); g_prefix = g_pol.cur.store_key ? mix64(mix64(g_rec_key, h), ord_take(h)) : mix64(g_prefix, h); }
    uint64_t lb = 0, sb = 0, base = 0;
    g_store_shared = 1;   // [P10 fix 4] (store_eligible: the chain's targets on shared storage are eligible)
    for (uint32_t j = 0; j < sh->natt; j++) {
        vkn_tex *t = sh->att[j]; const int dp = is_depth(t), ent = g_ch.a[j].entered; const uint64_t bytes = att_bytes(t, W, H);
        const int content = has_content(t);
        uint8_t ld = VK_ATTACHMENT_LOAD_OP_LOAD, sl = VK_ATTACHMENT_LOAD_OP_LOAD;
        if (sh->ext[j] || !ent) { if (!content) { ld = sl = VK_ATTACHMENT_LOAD_OP_CLEAR; } }   // read-only, or never reached: as it is (never written: zeros, as a reader would get them)
        else if (dp) {
            const int hd = (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0, hs = (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
            ld = (g_ch.a[j].clr & 1) && hd ? VK_ATTACHMENT_LOAD_OP_CLEAR : content ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
            sl = (g_ch.a[j].clr & 2) && hs ? VK_ATTACHMENT_LOAD_OP_CLEAR : content ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
            cv[j].depthStencil.depth = (g_ch.a[j].clr & 1) ? g_ch.a[j].cv.depthStencil.depth : 0.0f; cv[j].depthStencil.stencil = (g_ch.a[j].clr & 2) ? g_ch.a[j].cv.depthStencil.stencil : 0u;
        } else {
            if (g_ch.a[j].clr) { ld = VK_ATTACHMENT_LOAD_OP_CLEAR; cv[j] = g_ch.a[j].cv; }
            else if (g_ch.a[j].dc && hit && g_tl_mode != 2) ld = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            else if (!content) ld = VK_ATTACHMENT_LOAD_OP_CLEAR;   // never written: zeros, as fresh memory on the Switch
        }
        const int loads = ld == VK_ATTACHMENT_LOAD_OP_LOAD || (dp && sl == VK_ATTACHMENT_LOAD_OP_LOAD);
        if (loads) { hist_read(t, RD_LOAD); lb += bytes; g_st.load++; g_s3.att_load++; g_s3.load_bytes += bytes; } else if (ent && !sh->ext[j]) hist_over(t);
        uint8_t so = VK_ATTACHMENT_STORE_OP_STORE;
        if (sh->ext[j] || !ent) { so = VK_ATTACHMENT_STORE_OP_STORE; g_s3.att_store++; g_s3.store_bytes += bytes; }
        else if (!safe) { int sp = 0; so = (uint8_t)store_decide(t, j, 0, &sp, W, H); if (sp || (g_tl_mode == 3 && so == VK_ATTACHMENT_STORE_OP_DONT_CARE && !dp)) poison[j] = 1; }
        else { st_ent *e = st_get(t, 0); if (e) e->state = HS_NONE; g_s3.att_store++; g_s3.store_bytes += bytes; }
        if (so == VK_ATTACHMENT_STORE_OP_STORE) sb += bytes;
        stored[j] = so == VK_ATTACHMENT_STORE_OP_STORE;
        k.load[j] = ld; k.sload[j] = dp ? sl : VK_ATTACHMENT_LOAD_OP_DONT_CARE; k.store[j] = so; k.sstore[j] = dp ? so : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        const VkImageLayout want = dp ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (t->layout != want) barrier(cb, t, want);
    }
    g_store_shared = 0;
    g_prefix = prefix0;
    // the classic frame's model bytes for the same work: every attachment of every subpass stored at its pass end, the depth stored by
    // the pre-pass and loaded by the G-buffer pass
    for (uint32_t i = 0; i < nsub; i++) { const tset *s = &sh->sub[i]; for (uint32_t c = 0; c <= s->n; c++) { const vkn_tex *t = c < s->n ? s->c[c] : s->d; if (t) base += att_bytes(t, W, H); } }
    if (sh->prefix && sh->sub[0].d) base += 2 * att_bytes(sh->sub[0].d, W, H);
    // [P10 fix 6] a discarded store: the pass is recorded at the submit (tl_fill), when every later read of this recording is known;
    // without a split (none possible) every store is kept
    uint32_t ndead = 0; for (uint32_t j = 0; j < sh->natt; j++) ndead += !stored[j];
    int deferred = 0;
    if (ndead && !g_dfr.on) {
        memset(&g_dfr, 0, sizeof g_dfr);
        g_dfr.k = k; g_dfr.fb = g_ch.fb; g_dfr.W = W; g_dfr.H = H; g_dfr.nsub = nsub; g_dfr.natt = sh->natt; g_dfr.nsec = g_ch.nsec;
        memcpy(g_dfr.cv, cv, sizeof cv); memcpy(g_dfr.sec, g_ch.sec, sizeof g_dfr.sec); memcpy(g_dfr.sec_sub, g_ch.sec_sub, sizeof g_dfr.sec_sub);
        for (uint32_t j = 0; j < sh->natt; j++) { g_dfr.att[j] = sh->att[j]; g_dfr.dead[j] = !stored[j]; g_dfr.poison[j] = poison[j]; g_dfr.bytes[j] = att_bytes(sh->att[j], W, H); }
        g_dfr.on = 1;
        if (!vkn_rec_split(tl_fill, NULL)) { deferred = 1; g_s6.deferred++; }
        else g_dfr.on = 0;
    }
    if (ndead && !deferred) {   // (no split: kept, as before the history decided)
        for (uint32_t j = 0; j < sh->natt; j++) if (!stored[j]) {
            vkn_tex *t = sh->att[j]; st_ent *e = st_get(t, 0); if (e && e->state == HS_DEAD) e->state = HS_PENDING;
            k.store[j] = VK_ATTACHMENT_STORE_OP_STORE; if (is_depth(t)) k.sstore[j] = VK_ATTACHMENT_STORE_OP_STORE;
            stored[j] = 1; poison[j] = 0; sb += att_bytes(t, W, H); g_s6.store_kept_nosplit++;
        }
    }
    VkRenderPass rp = VK_NULL_HANDLE; uint16_t compat[TL_SUBS];
    if (!deferred && (!vkn_mp_get(&k, &rp, compat) || !rp)) {   // (cannot happen after tl_objects made the shape; the draws are lost)
        g_st2.rp_fail++; if (red_ok(&g_red_tl)) vkn_logf(RED "tile pass: no render pass for the chain's load / store ops: %u subpasses of draws lost", nsub);
    } else {
        tset js; memset(&js, 0, sizeof js); vkn_rp_key jk; vkn_rp_key_init(&jk);   // the journal's one line: the colours (first 8), the depth
        for (uint32_t j = 0; j < sh->natt; j++) {
            vkn_tex *t = sh->att[j];
            if (is_depth(t)) { if (!js.d) { js.d = t; jk.depth_load = k.load[j]; jk.depth_store = k.store[j]; } }
            else if (js.n < VKN_PASS_COLORS) { js.c[js.n] = t; jk.load[js.n] = k.load[j]; jk.store[js.n] = k.store[j]; js.n++; }
        }
        const VkRect2D ra = {{0, 0}, {W, H}};
        if (deferred) g_dfr.js = js;
        else tl_record_pass(cb, rp, g_ch.fb, W, H, sh->natt, cv, nsub, g_ch.sec, g_ch.sec_sub, g_ch.nsec, &js, sh->att, poison, 0);
        if (vkn_draw_raw_begin) vkn_draw_raw_begin();
        pj_begin(&js, &jk, W, H, ra, lb, sb, 2 + (int)nsub);
        g_pj.d0 = g_pj.d0 >= g_ch.draws ? g_pj.d0 - g_ch.draws : 0;   // (its draws: those of its subpass buffers, not the foreign passes run meanwhile)
        pj_end(hit ? VKN_PE_TARGETS : VKN_PE_OTHER);
        g_dump.passes_frame++;
        for (uint32_t j = 0; j < sh->natt; j++) {
            vkn_tex *t = sh->att[j];
            if (!g_ch.a[j].entered || sh->ext[j]) { if (!t->pass_ever) written(t); continue; }   // (never written before: it holds the pass's zeros now)
            written(t); if (g_dump.active && !is_depth(t)) dump_note(t);
        }
    }
    const uint32_t f = (uint32_t)vkn_frame();
    if (hit) {
        if (g_pol.cur.farea == 1) g_s6.farea_full++;   // [frame F1] DM_VK_NATIVE_FRAMEAREA is not applied to a merged pass: its render area is the whole target
        g_s6.hits++; g_s6.subpasses += nsub; g_s6.foreign += g_ch.foreign; sh->frame = f; sh->misses = 0;
        g_s6.base_bytes += base; g_s6.merged_bytes += lb + sb; if (base > lb + sb) g_s6.saved_bytes += base - (lb + sb);
        g_s6.passes_saved += (sh->prefix ? nsub : nsub - 1);
        if (vkn_tile_redraw_on) tl_verify_emit(cb);
    } else {
        g_s6.miss[why >= 0 && why < TLM_N ? why : TLM_OTHER]++; sh->frame = f; sh->cool = f + 1;
        if (++sh->misses >= 8) { sh->ban_until = f + 600; sh->misses = 0; sh->streak = 0; g_s6.bans++; vkn_logf("tile pass: 8 chains in a row did not complete (last: %s): off for 600 frames", k_tlm[why >= 0 && why < TLM_N ? why : TLM_OTHER]); }
    }
    for (uint32_t j = 0; j < sh->natt; j++) { sh->att[j]->tl_ep = 0; }
    g_ch.epoch = 0; g_ch.nsec = 0; g_tpend.ok = 0; vkn_tile_redraw_on = 0; g_vs.n = 0; g_vs.nscr = 0; g_tl_redraw = 0;
    tl_hook_upd();
}
void vkn_tile_test_fns(int (*uv)(const struct vkn_shader *s, uint32_t loc, struct nr_cv_prog *out), int (*scale1)(float w, float h)) { g_uv_fn = uv; g_scale_fn = scale1; }
static void tl_rec_begin(void) {   // (begin listener) a new recording
    if (!g_tl_latched && vkn_device()) { g_tl_latched = 1; g_tl_mode = tl_cfg(); if (g_tl_mode) { pol_apply(); vkn_logf("tile pass (P10): DM_VK_NATIVE_TILELIGHT=%d (%s): G-buffer -> LinearizeDepth -> CascadedPCF -> lighting as one render pass with input attachments%s", g_tl_mode, g_tl_env ? "env" : "family default", g_tl_mode == 2 ? " (verify: every store kept, the full-screen draws compared with their classic form)" : g_tl_mode == 3 ? " (+ poison)" : ""); } }
    if (g_tsh.live && !tl_shape_alive(&g_tsh)) { tl_sh_drop(&g_tsh); g_s6.dropped++; }   // (a target died or changed: the references go, the next frames teach again)
    g_dfr.on = 0;   // [P10 fix 6] (a recording that ended without its submit: nothing to fill)
    g_ch.st = TL_NONE; g_ch.epoch = 0; g_ch.nsec = 0; g_ob.active = 0; g_ob.edge = 0; g_tpend.ok = 0; g_tl_outer = 0; g_tl_prev_donly = 0; vkn_tile_prime_compat = 0;
    vkn_tile_redraw_on = 0; g_tl_redraw = 0; g_vs.n = 0; g_vs.nscr = 0;
    tl_hook_upd();
}
