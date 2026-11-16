// [DOS2] NB-04: the NVN tap of the native render backend (docs/dos2/NATIVE_BACKEND_PLAN.md §1 "TAP", §2). Included by hle_nvn.c after the
// DM_NVN_WT_DRAIN tables (it reads g_wt_id_mem: texture pool id -> storage address). Each NVN entry point that matters for the frame calls
// one NBT(...) line; with DM_NB unset (g_dm_nb == 0) that line is a load and a not-taken branch, the NVN records are never changed.
// With DM_NB=1/2 the tap turns the calls into the frame IR of hle_dos2_nb.c (dm_nb_frame): render-target sets, clears, copies, compute,
// barriers, texture reads by the bindings live at each draw, submit and present. It never writes or skips an NVN record (NB-04 = measure).
//
// Image ids = texture storage address (tex_t.mem; the WT_DRAIN key): aliases and views of one storage share an id; a texture handle is
// resolved through the pool registration at draw time (id = handle & 0xfffff, sampler bits dropped).
// Only the game's render thread is tracked (the thread hle_dos2_nb.c saw in BeginFrame / ExecuteCommandList / EndFrame), and only one
// command buffer: the first one that records a tracked call after the IR was reset owns it until its recording is submitted; calls into
// any other command buffer only count ("other cb"). Binding state is kept across submissions (the host's `state` is not reset either).
#ifdef DM_GAME_DOS2
#include "nvn_nb_tap.h"
#include "../nb/nb_slotmask.h"
#include "../nb/nb_post.h"   // NB-18b-4a: DM_NB_POST program table
#define NBT(x) do { if (__builtin_expect(g_dm_nb != 0, 0)) { x; } } while (0)

enum { NBT_STAGES = 6, NBT_SLOTS = 64, NBT_IMGS = 1024 };
static struct {
    const void *cb; uint64_t handle;    // command buffer of the IR / its handle once EndRecording returned it (0 = still recording)
    uint32_t tex[NBT_STAGES][NBT_SLOTS], img[NBT_STAGES][NBT_SLOTS];   // bound pool id + 1 (0 = nothing bound)
    uint64_t tdirty[NBT_STAGES], idirty[NBT_STAGES];                  // slots rebound since the reads of the open pass were taken
    int rd_node; uint32_t gen, rd_gen;                                // pass node that has the reads / IR generation (reset bumps it)
    int nrt; nb_att rt[NB_MAX_ATT]; uint32_t rtw[NB_MAX_ATT], rth[NB_MAX_ATT]; uint8_t rtasp[NB_MAX_ATT], rtfull[NB_MAX_ATT];
    int sx, sy; uint32_t sw, sh;                                      // scissor (NVN default: everything)
    struct { uint64_t image; uint32_t w, h; int fmt; } im[NBT_IMGS];  // RT images seen (dump comments)
    // NB-05: the NB records of the IR's command buffer (back-patched by nvn_nb_tap_patch before the submit is queued)
    p_nb_frame *frec; p_nb_pass *prec[NB_MAX_NODES]; int em_open;      // FRAME record; PASS record per IR node; node with an emitted PASS still open
    uint32_t seq, bytes, lost;                                          // IR submission number; NB record bytes of this IR; records not written
    // NB-07: the program bound per NVN stage (prog_t serial; 0 = none seen yet / no serial) and, once the backend published it, the slots
    // its shader of that stage samples (pknown 0 = unknown: every bound slot is a read, as before NB-07)
    uint64_t prog[NBT_STAGES], ptex[NBT_STAGES], pimg[NBT_STAGES]; uint8_t pknown[NBT_STAGES]; int sm;   // sm: DM_NB_SLOTMASK (1 default)
} g_nbt = { .sx = 0, .sy = 0, .sw = UINT32_MAX, .sh = UINT32_MAX, .em_open = -1 };
static uint8_t *nb_rec_alloc(cmd_t *o, uint32_t op, uint32_t payload);   // hle_nvn.c: a zeroed record straight into the command memory
// NB-07: the backend's table "program -> sampled slots" (xbg_nb.inc nbsm, dm/gfx/nb/nb_slotmask.h); weak: no backend = no masks
nb_sm_table *xbg_nb_slotmask_table(void) __attribute__((weak));
void nvn_nb_tap_slotmask(int on) { g_nbt.sm = on; }

static unsigned nbt_aspects(int fmt) {
    if (fmt == 0x31) return NB_ASP_STENCIL;
    if (fmt >= 0x32 && fmt <= 0x34) return NB_ASP_DEPTH;
    if (fmt == 0x35 || fmt == 0x36) return NB_ASP_DEPTH | NB_ASP_STENCIL;
    return NB_ASP_COLOR;
}
static uint64_t nbt_image(const NVNtexture *t) { const tex_t *o = (const tex_t *)t; return o ? (uint64_t)(uintptr_t)o->mem : 0; }
static uint64_t nbt_image_of_id(uint32_t id) { return atomic_load_explicit(&g_wt_id_mem[id & (WT_IDS - 1)], memory_order_relaxed); }
// The view selects the whole storage: one level, one layer (a mip / layer of a bigger texture is never "the whole image" for the IR).
static int nbt_whole(const tex_t *o) { return o && o->d.levels <= 1 && o->d.depth <= 1; }
static void nbt_remember(uint64_t image, const tex_t *o) {
    if (!image || !o) return;
    uint32_t k = (uint32_t)((image >> 8) * 0x9E3779B97F4A7C15ull >> 54) & (NBT_IMGS - 1);
    for (int i = 0; i < 8; i++, k = (k + 1) & (NBT_IMGS - 1))
        if (!g_nbt.im[k].image || g_nbt.im[k].image == image) {
            g_nbt.im[k].image = image; g_nbt.im[k].w = o->d.width; g_nbt.im[k].h = o->d.height; g_nbt.im[k].fmt = o->d.format; return; }
}
int nvn_nb_tap_image(uint64_t image, uint32_t *w, uint32_t *h, int *fmt) {
    uint32_t k = (uint32_t)((image >> 8) * 0x9E3779B97F4A7C15ull >> 54) & (NBT_IMGS - 1);
    for (int i = 0; i < 8; i++, k = (k + 1) & (NBT_IMGS - 1))
        if (g_nbt.im[k].image == image) { *w = g_nbt.im[k].w; *h = g_nbt.im[k].h; *fmt = g_nbt.im[k].fmt; return 1; }
    return 0;
}
void nvn_nb_tap_reset(void) {
    nb_frame *f = dm_nb_frame();
    g_nbt.cb = NULL; g_nbt.handle = 0; g_nbt.gen++;
    g_nbt.frec = NULL; g_nbt.em_open = -1; g_nbt.bytes = 0; g_nbt.lost = 0; memset(g_nbt.prec, 0, sizeof g_nbt.prec);
    if (f && g_nbt.nrt) { f->last_natt = (uint8_t)g_nbt.nrt; memcpy(f->last_att, g_nbt.rt, sizeof g_nbt.rt[0] * (size_t)g_nbt.nrt); }   // still bound
}
// The IR to append to for a call into command buffer c, or NULL (not the render thread / another command buffer).
static nb_frame *nbt_f(NVNcommandBuffer *c) {
    nb_frame *f = dm_nb_frame();
    if (!f) return NULL;
    if ((const void *)c != g_nbt.cb) {
        if (g_nbt.cb) { dm_nb_note(NBN_OTHER_CB, 1); return NULL; }
        g_nbt.cb = c; g_nbt.handle = 0;
    }
    return f;
}
static void nbt_note(NVNcommandBuffer *c, int what) { if (nbt_f(c)) dm_nb_note(what, 1); }
// NB-16: a hazard of the call (NB_HZ_COUNTER: ReportCounter, NB_HZ_COND: SetRenderEnable*) marks the open IR pass or adds / extends a fence
// node between passes (nb_hazard). The pass structure does not change, so no NB record is due (nbt_sync not needed). nb_track stays off
// until every NB_HZ_* source is tapped (UBO update, storage writes, CPU copies are not yet): the merge planner still says "untracked".
static void nbt_hazard(NVNcommandBuffer *c, unsigned hz) { nb_frame *f = nbt_f(c); if (f) nb_hazard(f, hz); }

// ---- NB-05: NB records (OP_X_NB_FRAME / _PASS / _PASS_END, nvn_obj.h) ----
// Called after every tap call that changed the IR's pass structure, before the call's own NVN record: the IR pass that was open with a
// PASS record and is no longer the open one gets PASS_END; a newly open pass gets PASS (the FRAME record goes first, once per submission).
// The host opens its Vulkan pass lazily, so a PASS record before SET_RT / the draw that reopened the set is in time.
static void *nbt_rec(int op, uint32_t size) {
    uint8_t *p = nb_rec_alloc((cmd_t *)g_nbt.cb, (uint32_t)op, size);
    if (!p) { g_nbt.lost++; return NULL; }
    g_nbt.bytes += (size + 8 + 7) & ~7u; return p;
}
// NB-14 (DM_NB_RELOC 1 / 2, nvn_nb_key.inc): the key frame of every PASS (OP_X_NB_KEY right after it); 0 = nothing walked or written.
static int g_nbk_on;
static void nbk_frame(void);
static void nbk_pass(int node);
// NB-17a (DM_NB_ORDER 1 / 2 / 3, nvn_nb_order.inc): the FRAME record carries the order tail (execution order + address of every PASS
// record, filled at submit); 0 = nothing reserved or written.
static int g_nbo_on;
static uint32_t nbo_frame_size(void);
static void nbo_frame(p_nb_frame *fr, uint32_t bytes);
static void nbo_pass(void);
// NB-21 (DM_NB_OWN 1 / 2, nvn_nb_own.inc): the addresses of the FRAME / PASS / KEY records are noted for the frame job; 0 = nothing noted.
// It writes no record at any setting.
static int g_nbw_on;
static void nbw_frame(void);
static void nbw_pass(int node);
static void nbw_key(int node, const uint8_t *payload);
static void nbt_sync(nb_frame *f) {
    if (!g_nbt.cb) return;
    const int open = f->open ? f->nnode - 1 : -1;
    if (g_nbt.em_open >= 0 && g_nbt.em_open != open) {
        p_nb_pass_end *e = (p_nb_pass_end *)nbt_rec(OP_X_NB_PASS_END, sizeof *e);
        if (e) {
            e->seq = g_nbt.seq; e->node = (int16_t)g_nbt.em_open; e->end = f->node[g_nbt.em_open].end;
            const nb_node *l = &f->node[f->nnode - 1];   // the operation that closed it (an op node right after the pass)
            if (f->nnode - 1 > g_nbt.em_open && l->kind != NB_NODE_PASS) e->op = (uint8_t)((l->kind & 0x3f) | (((l->kind & 0xff) == NB_OP_COPY && !l->nref) ? 0x40 : 0));
        }
        g_nbt.em_open = -1;
    }
    if (open >= 0 && open != g_nbt.em_open && open < NB_MAX_NODES && !g_nbt.prec[open]) {
        if (!g_nbt.frec) {
            g_nbt.seq++;
            const uint32_t fsz = g_nbo_on ? nbo_frame_size() : (uint32_t)sizeof(p_nb_frame);   // NB-17a: room for the order tail
            p_nb_frame *fr = (p_nb_frame *)nbt_rec(OP_X_NB_FRAME, fsz);
            if (!fr) return;
            fr->seq = g_nbt.seq; fr->frame_no = f->frame_no; fr->mode = (uint8_t)g_dm_nb; g_nbt.frec = fr;
            if (g_nbk_on) nbk_frame();   // NB-14: the key frames of this IR part are relative to the state here
            if (g_nbo_on) nbo_frame(fr, fsz);   // NB-17a: the tail as reserved (not valid until the submit fills it)
            if (g_nbw_on) nbw_frame();          // NB-21: the recording that holds the FRAME record
        }
        p_nb_pass *r = (p_nb_pass *)nbt_rec(OP_X_NB_PASS, sizeof *r);
        if (!r) return;
        r->seq = g_nbt.seq; r->node = (int16_t)open; g_nbt.prec[open] = r; g_nbt.em_open = open;
        if (g_nbw_on) nbw_pass(open);   // NB-21: the PASS record's address and recording (before nbk_pass: it notes the key's address)
        if (g_nbk_on) nbk_pass(open);   // NB-14: OP_X_NB_KEY, the state this pass starts from
        if (g_nbo_on) nbo_pass();       // NB-17a: a PASS outside the FRAME's recording keeps the stream order
    }
}
// Fill the records of this IR with the finished pass facts and the plan (plan NULL: planner failed). Render thread, before the job is queued.
uint32_t nvn_nb_tap_patch(const nb_frame *f, const nb_plan_out *po, uint32_t part) {
    p_nb_frame *fr = g_nbt.frec;
    if (!fr) return 0;
    int npass = 0;
    for (int i = 0; i < f->nnode && i < NB_MAX_NODES; i++) {
        p_nb_pass *r = g_nbt.prec[i]; const nb_node *n = &f->node[i];
        if (!r || n->kind != NB_NODE_PASS) continue;
        npass++;
        r->natt = n->natt > NB_REC_ATT ? NB_REC_ATT : n->natt;
        for (int k = 0; k < r->natt; k++) { r->att[k] = n->att[k].image; r->slot[k] = n->att[k].slot; }
        r->end = n->end; r->draws = n->draws; r->nclear = (uint32_t)n->nclear;
        uint32_t nr = 0;
        for (int j = 0; j < n->nref; j++) {
            const uint64_t im = f->ref[n->ref_first + j].image;
            r->bloom[nb_rec_bloom_bit(im, 0) >> 6] |= 1ull << (nb_rec_bloom_bit(im, 0) & 63);
            r->bloom[nb_rec_bloom_bit(im, 1) >> 6] |= 1ull << (nb_rec_bloom_bit(im, 1) & 63);
            if (nr < NB_REC_READS) r->read[nr] = im; else r->read_over = 1;
            nr++;
        }
        r->nread = nr;
        if (po && i < po->nnode) {
            const nb_node_plan *np = &po->node[i];
            for (int k = 0; k < r->natt; k++) {
                r->load[k] = np->load[k]; r->store[k] = np->store[k];
                if (np->load[k] == NB_LOAD_CLEAR) r->clear_mask |= (uint16_t)(1u << k);
            }
            r->flags = np->flags; r->cover_dc = np->cover_dc; r->plan_ok = 1;
        }
        __atomic_store_n(&r->patched, 1, __ATOMIC_RELEASE);
    }
    fr->part = part; fr->nnode = (uint32_t)f->nnode; fr->npass = (uint32_t)npass; fr->bytes = g_nbt.bytes; fr->final = f->final; fr->plan_ok = po != NULL;
    __atomic_store_n(&fr->patched, 1, __ATOMIC_RELEASE);
    return g_nbt.bytes;
}

static void nbt_set_rt(NVNcommandBuffer *c, int n, const NVNtexture *const *ct, const NVNtextureView *const *cv, const NVNtexture *dt) {
    nb_frame *f = nbt_f(c); if (!f) return;
    (void)cv;
    g_nbt.nrt = 0;
    for (int i = 0; i < n && i < 8; i++) {
        const tex_t *o = ct ? (const tex_t *)ct[i] : NULL; uint64_t im = nbt_image(ct ? ct[i] : NULL);
        if (!im) continue;
        int k = g_nbt.nrt++;
        g_nbt.rt[k] = (nb_att){im, (uint8_t)i}; g_nbt.rtw[k] = o->d.width; g_nbt.rth[k] = o->d.height;
        g_nbt.rtasp[k] = (uint8_t)nbt_aspects(o->d.format); g_nbt.rtfull[k] = (uint8_t)nbt_whole(o); nbt_remember(im, o);
    }
    if (dt && nbt_image(dt)) {
        const tex_t *o = (const tex_t *)dt; int k = g_nbt.nrt++;
        g_nbt.rt[k] = (nb_att){nbt_image(dt), NB_SLOT_DS}; g_nbt.rtw[k] = o->d.width; g_nbt.rth[k] = o->d.height;
        g_nbt.rtasp[k] = (uint8_t)nbt_aspects(o->d.format); g_nbt.rtfull[k] = (uint8_t)nbt_whole(o); nbt_remember(nbt_image(dt), o);
    }
    nb_pass_begin(f, g_nbt.rt, g_nbt.nrt);
    nbt_sync(f);
}
static int nbt_rt_slot(uint8_t slot) { for (int k = 0; k < g_nbt.nrt; k++) if (g_nbt.rt[k].slot == slot) return k; return -1; }
static int nbt_scissor_covers(uint32_t w, uint32_t h) {
    return g_nbt.sx <= 0 && g_nbt.sy <= 0 && (int64_t)g_nbt.sx + g_nbt.sw >= w && (int64_t)g_nbt.sy + g_nbt.sh >= h;
}
static void nbt_clear_color(NVNcommandBuffer *c, int index, const void *rgba, int mask) {
    nb_frame *f = nbt_f(c); if (!f) return;
    int k = index >= 0 && index < 8 ? nbt_rt_slot((uint8_t)index) : -1;
    if (k < 0 || !(mask & 15)) return;   // the host ignores a clear of a missing target / an empty mask too
    unsigned fl = NB_CLEAR_RT | ((mask & 15) == 15 && g_nbt.rtfull[k] && nbt_scissor_covers(g_nbt.rtw[k], g_nbt.rth[k]) ? NB_CLEAR_FULL : 0);
    uint32_t v[4]; memcpy(v, rgba, 16);
    nb_clear(f, g_nbt.rt[k].image, NB_ASP_COLOR, fl, v, 0.f, 0);
    nbt_sync(f);
}
static void nbt_clear_ds(NVNcommandBuffer *c, float depth, uint32_t dmask, int stencil, int smask) {
    nb_frame *f = nbt_f(c); if (!f) return;
    int k = nbt_rt_slot(NB_SLOT_DS); if (k < 0) return;
    const unsigned have = g_nbt.rtasp[k];
    unsigned asp = ((dmask & 0xff) ? NB_ASP_DEPTH : 0) | ((smask & 0xff) ? NB_ASP_STENCIL : 0);
    asp &= have; if (!asp) return;
    const int partial_stencil = (asp & NB_ASP_STENCIL) && (smask & 0xff) != 0xff;
    unsigned fl = NB_CLEAR_RT | (asp == have && !partial_stencil && g_nbt.rtfull[k] && nbt_scissor_covers(g_nbt.rtw[k], g_nbt.rth[k]) ? NB_CLEAR_FULL : 0);
    nb_clear(f, g_nbt.rt[k].image, asp, fl, NULL, depth, (uint32_t)stencil & 0xff);
    nbt_sync(f);
}
// Region covers level 0 of a one-level texture entirely (x/y/z from 0, every texel row and layer).
static int nbt_region_whole(const tex_t *o, const NVNcopyRegion *r) {
    if (!o || o->d.levels > 1) return 0;
    const uint32_t d = o->d.depth ? o->d.depth : 1;
    return r && r->xoffset == 0 && r->yoffset == 0 && r->zoffset == 0 && (uint32_t)r->width >= o->d.width && (uint32_t)r->height >= o->d.height
        && (uint32_t)(r->depth ? r->depth : 1) >= d;
}
static void nbt_clear_tex(NVNcommandBuffer *c, const NVNtexture *t, const NVNcopyRegion *r, const void *rgba, int mask) {
    nb_frame *f = nbt_f(c); if (!f) return;
    const tex_t *o = (const tex_t *)t; uint64_t im = nbt_image(t); if (!im) return;
    const unsigned asp = nbt_aspects(o->d.format);
    unsigned fl = ((mask & 15) == 15 || asp != NB_ASP_COLOR) && nbt_region_whole(o, r) ? NB_CLEAR_FULL : 0;
    uint32_t v[4]; memcpy(v, rgba, 16);
    nb_clear(f, im, asp, fl, v, 0.f, 0);
    nbt_sync(f);
}
static void nbt_op(NVNcommandBuffer *c, int kind, const NVNtexture *s, const NVNtexture *d, const NVNcopyRegion *dr) {
    nb_frame *f = nbt_f(c); if (!f) return;
    uint64_t si = nbt_image(s), di = nbt_image(d);
    if (d && dr && nbt_region_whole((const tex_t *)d, dr)) kind |= NB_OPF_DST_FULL;
    else if (d && !dr && kind == NB_OP_RESOLVE) kind |= NB_OPF_DST_FULL;
    nb_op(f, kind, si ? &si : NULL, si ? 1 : 0, di ? &di : NULL, di ? 1 : 0);
    nbt_sync(f);
}
static void nbt_bind(NVNcommandBuffer *c, int op, int st, int first, int n, const uint64_t *h) {
    if (op != OP_BIND_TEX && op != OP_BIND_IMG) return;
    if (!nbt_f(c) || st < 0 || st >= NBT_STAGES) return;
    uint32_t *tab = op == OP_BIND_TEX ? g_nbt.tex[st] : g_nbt.img[st]; uint64_t *dirty = op == OP_BIND_TEX ? &g_nbt.tdirty[st] : &g_nbt.idirty[st];
    for (int i = 0; i < n; i++) {
        const int s = first + i; if (s < 0 || s >= NBT_SLOTS) continue;
        const uint32_t v = ((uint32_t)h[i] & (WT_IDS - 1)) + 1;
        if (tab[s] != v) { tab[s] = v; *dirty |= 1ull << s; }
    }
}
// NB-07: BindProgram -> the program of each stage in `stages` (NVN stage bits; NULL = no shader there: nothing sampled, as the backend).
static void nbt_program(NVNcommandBuffer *c, const NVNprogram *p, int stages) {
    if (!nbt_f(c)) return;
    dm_nb_note(NBN_PROGRAM, 1);
    uint64_t ser = 0; if (p) memcpy(&ser, &((const prog_t *)p)->serial, sizeof ser);   // unaligned-safe (see snap_prog)
    for (int st = 0; st < NBT_STAGES; st++) if (stages >> st & 1) {
        g_nbt.prog[st] = ser; g_nbt.pknown[st] = !p; g_nbt.ptex[st] = g_nbt.pimg[st] = 0;   // p NULL: known, nothing sampled
    }
}
// Slots of stage st (w: images) the bound program samples; every slot while unknown (DM_NB_SLOTMASK=0, no serial, not published yet).
static uint64_t nbt_mask(int st, int w) {
    if (!g_nbt.sm) return NB_SM_ALL;
    if (!g_nbt.pknown[st]) {
        nb_sm_masks m;
        if (!g_nbt.prog[st] || !xbg_nb_slotmask_table || !nb_sm_lookup(xbg_nb_slotmask_table(), g_nbt.prog[st], &m)) return NB_SM_ALL;
        g_nbt.pknown[st] = 1; g_nbt.ptex[st] = m.tex[st]; g_nbt.pimg[st] = m.img[st];
    }
    return w ? g_nbt.pimg[st] : g_nbt.ptex[st];
}
// Per draw / dispatch: did a stage with bound slots have an unknown program (counted once per call: '[nb] slotmask' unknown)?
static void nbt_mask_note(int s0, int s1) {
    if (!g_nbt.sm) return;
    for (int st = s0; st < s1; st++) for (int w = 0; w < 2; w++) {
        if (nbt_mask(st, w) != NB_SM_ALL) continue;
        const uint32_t *tab = w ? g_nbt.img[st] : g_nbt.tex[st];
        for (int s = 0; s < NBT_SLOTS; s++) if (tab[s]) { dm_nb_note(NBN_SM_UNKNOWN, 1); return; }
    }
    dm_nb_note(NBN_SM_KNOWN, 1);
}
// Bound textures / images of stages [s0, s1) -> reads of the open pass (only slots rebound since the pass got its reads, unless all).
// NB-07: only the slots the bound program samples (nbt_mask); the others stay pending in dirty (nb_sm_take) for a later program of the pass.
static void nbt_reads(nb_frame *f, int s0, int s1, int all) {
    for (int st = s0; st < s1; st++) for (int w = 0; w < 2; w++) {
        const uint32_t *tab = w ? g_nbt.img[st] : g_nbt.tex[st]; uint64_t *dirty = w ? &g_nbt.idirty[st] : &g_nbt.tdirty[st];
        const uint64_t mask = nbt_mask(st, w);
        if (all && ~mask) { unsigned k = 0; for (int s = 0; s < NBT_SLOTS; s++) k += tab[s] && !(mask >> s & 1); if (k) dm_nb_note(NBN_SM_SKIP, k); }
        uint64_t m = nb_sm_take(dirty, all, mask);
        while (m) {
            const int s = __builtin_ctzll(m); m &= m - 1;
            if (!tab[s]) continue;
            uint64_t im = nbt_image_of_id(tab[s] - 1);
            if (im) nb_read(f, -1, im); else dm_nb_note(NBN_UNREG, 1);
        }
    }
}
// NB-05b: a barrier kept inside the open pass (nb_barrier) ends it only when a draw reads one of its attachments: the bound textures /
// images of stages 0-4 (+ the DrawTexture texture) are checked before the draw is counted, so the draw goes to the reopened pass.
static void nbt_barrier_hazards(nb_frame *f, uint64_t handle) {
    if (handle && nb_barrier_hazard(f, nbt_image_of_id((uint32_t)handle & (WT_IDS - 1)))) return;
    for (int st = 0; st < 5; st++) for (int w = 0; w < 2; w++) {
        const uint32_t *tab = w ? g_nbt.img[st] : g_nbt.tex[st]; const uint64_t mask = nbt_mask(st, w);   // NB-07: sampled slots only
        for (int s = 0; s < NBT_SLOTS; s++) if (tab[s] && (mask >> s & 1) && nb_barrier_hazard(f, nbt_image_of_id(tab[s] - 1))) return;
    }
}
static void nbt_barrier(NVNcommandBuffer *c) {
    nb_frame *f = nbt_f(c); if (!f) return;
    nb_barrier(f);
    nbt_sync(f);
}
// NB-18b-4a (DM_NB_POST): at a draw of the open pass, the fs key (nb_post.h table, by the serial NB-07 tracks per stage) + the images the
// bound programs really sample (NB-07's slot masks), exact when every stage's program is known. Independent of DM_NB_SLOTMASK (that only
// decides the IR reads): the masks are looked up here even with it 0 (the cache below is the one nbt_mask fills).
static int nbt_post_mask(int st, uint64_t *tex, uint64_t *img) {
    if (!g_nbt.pknown[st]) {
        nb_sm_masks m;
        if (!g_nbt.prog[st]) { *tex = *img = 0; return 1; }   // nothing bound at this stage yet: the backend has no shader there
        if (!xbg_nb_slotmask_table || !nb_sm_lookup(xbg_nb_slotmask_table(), g_nbt.prog[st], &m)) return 0;
        g_nbt.pknown[st] = 1; g_nbt.ptex[st] = m.tex[st]; g_nbt.pimg[st] = m.img[st];
    }
    *tex = g_nbt.ptex[st]; *img = g_nbt.pimg[st];
    return *tex != NB_SM_ALL && *img != NB_SM_ALL;
}
static void nbt_prog_draw(nb_frame *f, uint64_t handle) {
    uint64_t im[NB_MAX_SREAD + 1]; int n = 0, exact = 1;
    const nb_post_prog *e = g_nbt.prog[1] ? dm_nb_post_prog(g_nbt.prog[1]) : NULL;
    const uint64_t fs = e ? e->fs : 0;
    for (int ns = 0; ns < 5; ns++) {
        uint64_t mt, mi;
        if (!nbt_post_mask(ns, &mt, &mi)) { exact = 0; continue; }
        for (int w = 0; w < 2; w++) {
            uint64_t m = w ? mi : mt;
            const uint32_t *tab = w ? g_nbt.img[ns] : g_nbt.tex[ns];
            while (m) {
                const int sl = __builtin_ctzll(m); m &= m - 1;
                if (sl >= NBT_SLOTS || !tab[sl]) continue;   // nothing bound: the backend reads a null texture
                const uint64_t x = nbt_image_of_id(tab[sl] - 1);
                if (!x) { exact = 0; continue; }
                if (n < NB_MAX_SREAD) im[n++] = x; else exact = 0;
            }
        }
    }
    if (handle) { const uint64_t x = nbt_image_of_id((uint32_t)handle & (WT_IDS - 1)); if (x && n < NB_MAX_SREAD) im[n++] = x; }   // DrawTexture
    nb_draw_prog(f, fs, exact, im, n);
}
static void nbt_draw(NVNcommandBuffer *c, uint64_t handle) {
    nb_frame *f = nbt_f(c); if (!f) return;
    dm_nb_note(NBN_DRAW, 1);
    if (f->bar_pending) { nbt_barrier_hazards(f, handle); nbt_sync(f); }   // PASS_END (op barrier) before the reopened pass's PASS
    nb_draw(f, 0);   // no coverage proof on the front end: the plan's cover_dc marks what the BE's first_draw_covers may discard (NB-05)
    nbt_sync(f);
    if (!f->open) return;
    const int node = f->nnode - 1, all = node != g_nbt.rd_node || g_nbt.rd_gen != g_nbt.gen;
    nbt_mask_note(0, 5);
    nbt_reads(f, 0, 5, all);
    if (handle) { uint64_t im = nbt_image_of_id((uint32_t)handle & (WT_IDS - 1)); if (im) nb_read(f, -1, im); }   // DrawTexture
    if (g_dm_nb_post) nbt_prog_draw(f, handle);   // NB-18b-4a
    g_nbt.rd_node = node; g_nbt.rd_gen = g_nbt.gen;
}
static void nbt_dispatch(NVNcommandBuffer *c) {
    nb_frame *f = nbt_f(c); if (!f) return;
    dm_nb_note(NBN_DISPATCH, 1);
    uint64_t src[2 * NBT_SLOTS], dst[NBT_SLOTS]; int ns = 0, nd = 0;
    nbt_mask_note(5, 6);
    const uint64_t tm = nbt_mask(5, 0), imm = nbt_mask(5, 1);   // NB-07: slots the compute program samples / writes
    for (int s = 0; s < NBT_SLOTS; s++) {
        if (!(tm >> s & 1) && g_nbt.tex[5][s]) dm_nb_note(NBN_SM_SKIP, 1);
        if (!(imm >> s & 1) && g_nbt.img[5][s]) dm_nb_note(NBN_SM_SKIP, 1);
        if (g_nbt.tex[5][s] && (tm >> s & 1)) { uint64_t im = nbt_image_of_id(g_nbt.tex[5][s] - 1); if (im) src[ns++] = im; }
        if (g_nbt.img[5][s] && (imm >> s & 1)) { uint64_t im = nbt_image_of_id(g_nbt.img[5][s] - 1); if (im) { src[ns++] = im; dst[nd++] = im; } }   // storage image: read + write
    }
    g_nbt.tdirty[5] = g_nbt.idirty[5] = 0;
    nb_op(f, NB_OP_COMPUTE, src, ns, dst, nd);
    nbt_sync(f);
}
static void nbt_end_recording(NVNcommandBuffer *c, uint64_t handle) {
    if ((const void *)c == g_nbt.cb && dm_nb_frame()) g_nbt.handle = handle;
}
static void nbt_submit(int n, const NVNcommandHandle *h) {
    if (!dm_nb_frame()) return;
    int mine = 0;
    for (int i = 0; i < n; i++) if (g_nbt.handle && (uint64_t)h[i] == g_nbt.handle) mine = 1;
    if (!mine) { dm_nb_note(NBN_OTHER_SUBMIT, 1); return; }
    dm_nb_submit();   // the front end plans / resets the IR (and calls nvn_nb_tap_reset)
}
static void nbt_present(const NVNtexture *t) {
    nb_frame *f = dm_nb_frame(); if (!f) return;
    uint64_t im = nbt_image(t); if (!im) return;
    nb_op(f, NB_OP_PRESENT, &im, 1, NULL, 0);
}
// ---- NB-09: one engine draw = one OP_X_NB_DRAW (DM_NB_OPS bits 36 / 37; the code is in nvn_nb_draw.inc, after REC / RECN) ----
// State only here: cmd_alloc (before the records' helpers) checks it to flush the shadow before any record that is not modelled.
static int g_nbd_on;                   // DM_NB_OPS has a draw bit (set by the front end at install, before guest code runs)
static uint32_t g_nbd_texgen = 1;      // texture-set generation: every texture / sampler / image bind, pool change, BeginRecording bumps it
static __thread struct { int in, emitting, drew; unsigned op; cmd_t *cb; nbd_shadow s; } t_nbd;   // in = inside a captured bracket
static void nbd_foreign(cmd_t *o);     // nvn_nb_draw.inc: a record that is not modelled is about to be written inside the bracket
#define NBD_FOREIGN(o) do { if (__builtin_expect(g_nbd_on, 0) && t_nbd.in && !t_nbd.emitting) nbd_foreign(o); } while (0)
#define NBD_TAKE(c) (__builtin_expect(g_nbd_on, 0) && t_nbd.in && !t_nbd.emitting)
#define NBD_TEXGEN() do { if (__builtin_expect(g_nbd_on, 0)) __atomic_add_fetch(&g_nbd_texgen, 1, __ATOMIC_RELAXED); } while (0)
#define NBD_QUIET(x) do { const int nbd_e_ = t_nbd.emitting; t_nbd.emitting = 1; x; t_nbd.emitting = nbd_e_; } while (0)   // records that are not foreign (NB records, wrappers)
#define NBD_REN(c, on) do { if (NBD_TAKE(c)) nbd_render_enable((c), (on)); } while (0)
#define NBD_COND(c, m, a) do { if (NBD_TAKE(c)) nbd_render_cond((c), (m), (a)); } while (0)
// ---- NB-15 (DM_NB_FOREIGN=1, default 0): the share of the fallback path, by caller class (classes: nvn_nb_tap.h NBF_*) ----
// cmd_alloc (NBF_REC, after the NB-13a / NB-09 flushes, so their own records are classified first) files every record of every command
// buffer; rec (NBF_DRAW) adds the draw records to the class of the record just filed. The packet bracket comes from the front end
// (hle_dos2_nb.c nb_rc_pre / post -> nvn_nb_foreign_pkt). Outside a packet and in the callback packets 1 / 2 the first caller in main is
// found by walking the frame records of this library (each one ours: its saved lr is checked before its saved fp is followed; the first lr
// in main is the call site of the game; an lr in another guest module stops the walk). Never writes, skips or reorders a record.
// Cost with it on: a few thread-local reads and relaxed adds a record, the walk (<= 10 frame records) only outside packets / in 1 / 2.
static int g_nbf_on;
static const uint8_t *g_nbf_main; static size_t g_nbf_main_sz; static uint32_t g_nbf_ui_lo, g_nbf_ui_hi;
static nbf_stats g_nbf;
static __thread struct { int depth, rt; uint8_t op[16], taken[16]; int last_cls, last_op, last_site; } t_nbf = { .last_site = -1 };
static void nbf_add(uint64_t *p, uint64_t n) { __atomic_fetch_add(p, n, __ATOMIC_RELAXED); }
void nvn_nb_foreign_enable(const void *main_base, size_t main_size, uint32_t ui_lo, uint32_t ui_hi) {
    g_nbf_main = (const uint8_t *)main_base; g_nbf_main_sz = main_size; g_nbf_ui_lo = ui_lo; g_nbf_ui_hi = ui_hi; g_nbf_on = 1;
}
void nvn_nb_foreign_pkt(int op, int taken) {
    t_nbf.rt = 1;   // this thread runs engine packets (the render thread)
    if (op < 0) { if (t_nbf.depth > 0) t_nbf.depth--; return; }
    if (t_nbf.depth < 16) { t_nbf.op[t_nbf.depth] = (uint8_t)op; t_nbf.taken[t_nbf.depth] = (uint8_t)(taken != 0); }
    t_nbf.depth++;
}
void nvn_nb_foreign_stats(nbf_stats *out) {
    uint64_t x[2][3];   // NB and other-thread counters can come from any thread: taken atomically, the rest belongs to the render thread
    for (int c = 0; c < 2; c++) {
        const int k = c ? NBF_THREAD : NBF_NB;
        x[c][0] = __atomic_exchange_n(&g_nbf.rec[k], 0, __ATOMIC_RELAXED); x[c][1] = __atomic_exchange_n(&g_nbf.drw[k], 0, __ATOMIC_RELAXED);
        x[c][2] = __atomic_exchange_n(&g_nbf.bytes[k], 0, __ATOMIC_RELAXED);
    }
    memcpy(out, &g_nbf, sizeof *out);
    memset(&g_nbf, 0, sizeof g_nbf);   // an add from another thread between the exchange and here is lost (counts only)
    for (int c = 0; c < 2; c++) { const int k = c ? NBF_THREAD : NBF_NB; out->rec[k] = x[c][0]; out->drw[k] = x[c][1]; out->bytes[k] = x[c][2]; }
}
// The return address of the game's call into NVN (offset in main), or 0: *other = 1 when an lr in another guest module stopped the walk.
static uintptr_t nbf_caller(int *other) __attribute__((noinline));   // its own frame record starts the walk
static uintptr_t nbf_caller(int *other) {
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    const uintptr_t top = fp + (256u << 10);
    for (int i = 0; i < 10; i++) {
        if ((fp & 7) || fp + 16 > top) return 0;
        const uintptr_t nfp = ((const uintptr_t *)fp)[0], lr = ((const uintptr_t *)fp)[1] & 0x0000ffffffffffffull;   // no PAC / tag bits
        if (lr - (uintptr_t)g_nbf_main < g_nbf_main_sz) return lr;
        if (module_of((const void *)lr)) { *other = 1; return 0; }   // another guest module: its frames are not ours, stop
        if (nfp <= fp) return 0;
        fp = nfp;
    }
    return 0;
}
static int nbf_site_of(uint32_t off, uint8_t op) {
    uint32_t k = (uint32_t)(((uint64_t)off * 0x9E3779B97F4A7C15ull + op) >> 56) & (NBF_SITES - 1);
    for (int i = 0; i < 16; i++, k = (k + 1) & (NBF_SITES - 1)) {
        nbf_site *s = &g_nbf.site[k];
        if (!s->used) { s->used = 1; s->off = off; s->op = op; return (int)k; }
        if (s->off == off && s->op == op) return (int)k;
    }
    g_nbf.site_full++;
    return -1;
}
static void nbf_rec(uint32_t size) {
    int cls, op = -1, site = -1;
    if (t_nbd.emitting) {   // NB's own records (and wrappers) do not touch the stash: the draw count of the outer record keeps its class
        nbf_add(&g_nbf.rec[NBF_NB], 1); nbf_add(&g_nbf.bytes[NBF_NB], size); return;
    }
    if (!t_nbf.rt) { nbf_add(&g_nbf.rec[NBF_THREAD], 1); nbf_add(&g_nbf.bytes[NBF_THREAD], size); t_nbf.last_cls = NBF_THREAD; t_nbf.last_op = -1; t_nbf.last_site = -1; return; }
    if (t_nbd.in) cls = NBF_DRAW_FOREIGN;
    else {
        if (t_nbf.depth > 0) {
            const int k = (t_nbf.depth > 16 ? 16 : t_nbf.depth) - 1; op = t_nbf.op[k];
            cls = t_nbf.taken[k] ? NBF_TAKEN_PKT : (op == 1 || op == 2) ? NBF_CALLBACK : NBF_OFF_MASK;
        } else cls = NBF_DIRECT;
        if (cls == NBF_CALLBACK || cls == NBF_DIRECT) {
            int other = 0; const uintptr_t lr = nbf_caller(&other);
            if (lr) {
                const uint32_t off = (uint32_t)(lr - (uintptr_t)g_nbf_main);
                if (off >= g_nbf_ui_lo && off < g_nbf_ui_hi) cls = NBF_UI;
                site = nbf_site_of(off, op >= 0 ? (uint8_t)op : (uint8_t)NBF_OP_NONE);
            } else {
                if (other) g_nbf.other_module++; else g_nbf.walk_failed++;
                if (cls == NBF_DIRECT) cls = NBF_OTHER;
            }
        }
        if (op >= 0 && op < NBF_OPS) { g_nbf.op_rec[op]++; g_nbf.op_bytes[op] += size; }
    }
    g_nbf.rec[cls]++; g_nbf.bytes[cls] += size;
    if (site >= 0) g_nbf.site[site].rec++;
    t_nbf.last_cls = cls; t_nbf.last_op = op; t_nbf.last_site = site;
}
static void nbf_draw(uint32_t op) {
    if (op == OP_X_NB_DRAW) { nbf_add(&g_nbf.drw[NBF_NB], 1); return; }
    if (op != OP_DRAW && op != OP_DRAW_IDX && op != OP_X_DRAW_CALL && op != OP_X_DRAW_INDIRECT && op != OP_X_MULTI_DRAW_INDIRECT && op != OP_X_DRAW_TEX) return;
    const int cls = t_nbf.last_cls;
    if (cls == NBF_THREAD) { nbf_add(&g_nbf.drw[NBF_THREAD], 1); return; }
    g_nbf.drw[cls]++;
    if (t_nbf.last_op >= 0 && t_nbf.last_op < NBF_OPS) g_nbf.op_drw[t_nbf.last_op]++;
    if (t_nbf.last_site >= 0) g_nbf.site[t_nbf.last_site].drw++;
}
#define NBF_REC(o, size) do { if (__builtin_expect(g_nbf_on, 0)) nbf_rec(size); } while (0)
#define NBF_DRAW(op) do { if (__builtin_expect(g_nbf_on, 0)) nbf_draw(op); } while (0)
#else
#define NBT(x) ((void)0)
#define NBD_FOREIGN(o) ((void)0)
#define NBD_TAKE(c) 0
#define nbd_take_bind(...) 0   // never reached (NBD_TAKE is 0): keeps the entry points compiling outside DOS2
#define nbd_draw(...) 0
#define nbd_take_ren(...) 0
#define NBD_TEXGEN() ((void)0)
#define NBD_QUIET(x) do { x; } while (0)
#define NBD_REN(c, on) ((void)0)
#define NBD_COND(c, m, a) ((void)0)
#define NBF_REC(o, size) ((void)0)
#define NBF_DRAW(op) ((void)0)
#endif
#include "nvn_nb_state.inc"   // [DOS2] DM_NB_OPS state bits (NB-13a): the state shadow, OP_X_NB_STATE (NBS_* hooks; empty outside DOS2)
#include "nvn_nb_key.inc"     // [DOS2] DM_NB_RELOC (NB-14): the key frame of every IR pass, OP_X_NB_KEY (empty outside DOS2)
#include "nvn_nb_order.inc"   // [DOS2] DM_NB_ORDER (NB-17a): the order tail of OP_X_NB_FRAME (empty outside DOS2)
#include "nvn_nb_own.inc"     // [DOS2] DM_NB_OWN (NB-21): the frame job handed to the backend with its submit (empty outside DOS2)
