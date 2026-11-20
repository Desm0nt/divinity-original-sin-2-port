// [DOS2] NB-09: one engine draw = one OP_X_NB_DRAW record (docs/dos2/NATIVE_BACKEND_PLAN.md NB-09; layout nvn_nb_draw.h). Included by
// hle_nvn.c after REC / RECN; the state (t_nbd, g_nbd_on, g_nbd_texgen) and the NBD_* macros are in nvn_nb_tap.inc.
// The front end (hle_dos2_nb.c, DM_NB_OPS bit 36 / 37) opens the bracket around the engine's draw function (hle_dos2_native.c chain);
// inside it the modelled calls (bind_handles with one handle, BindUniformBuffer(s), SetRenderEnable(Conditional), DrawElements /
// DrawElementsBaseVertex / DrawElementsInstanced / DrawArrays) fill the shadow instead of writing records, and the draw writes the shadow
// as ONE OP_X_NB_DRAW (through rec(): inside a DM_NATIVE_DRAW=1 draw it goes into that buffer like any record). Every other record
// written inside the bracket (cmd_alloc -> nbd_foreign) first flushes the shadow as the NVN records it replaced, in call order
// ("foreign"); so do a full shadow, a change of command buffer and the end of the bracket with binds but no draw after them ("orphan").
// The NVN tap of NB-04 / NB-05 runs before all of this, unchanged (its PASS / PASS_END records are written straight, nb_rec_alloc).
// DM_NATIVE_DRAW=2 (check records before the draw) turns the capture off for that draw.
// NB-10, DM_NB_OPS_CHECK (same bits, the front end passes them to nvn_nb_draw_check): in a checked bracket every modelled call goes to the
// shadow AND writes its old NVN record ("mirror": the record right after it is not foreign); the draw writes the shadow as OP_X_NB_DRAW
// flagged NBD_F_CHECK just before the old draw record. Flushes (foreign, full, other command buffer, orphan) only empty the shadow - their
// records are already written - so the record holds exactly the binds an OP_X_NB_DRAW of DM_NB_OPS would carry. The backend executes the
// old records and compares (xbg_nb.inc nbdraw::check).
#ifdef DM_GAME_DOS2
enum { NBDS_BRACKETS, NBDS_DRAWS, NBDS_BINDS, NBDS_BYTES, NBDS_FOREIGN, NBDS_FOREIGN_FLUSH, NBDS_FLUSH_BINDS, NBDS_ORPHAN, NBDS_FULL,
       NBDS_CB, NBDS_NODRAW, NBDS_KINDS };
_Static_assert(NBDS_KINDS == NBDS_N, "nvn_nb_tap.h NBDS_N");
static uint64_t g_nbds[NBDS_KINDS];   // render thread mostly; atomics because a second thread only makes them approximate
static void nbds_add(int k, uint64_t n) { __atomic_fetch_add(&g_nbds[k], n, __ATOMIC_RELAXED); }
void nvn_nb_draw_stats(uint64_t out[NBDS_KINDS]);
void nvn_nb_draw_stats(uint64_t out[NBDS_KINDS]) { for (int i = 0; i < NBDS_KINDS; i++) out[i] = __atomic_exchange_n(&g_nbds[i], 0, __ATOMIC_RELAXED); }
static void nbd_reset(void) { memset(&t_nbd.s.h, 0, sizeof t_nbd.s.h); NRD_WF_RESET(); }   // NRD_WF_RESET: [DOS2] NR-09R, the flags of the binds that left
static uint64_t g_nbd_check;               // NB-10: DM_NB_OPS_CHECK opcode mask (set by the front end before nvn_nb_draw_enable)
static __thread int t_nbd_chk, t_nbd_mirror;   // this bracket is checked; the next record is the mirror of a modelled call
void nvn_nb_draw_check(uint64_t mask);
void nvn_nb_draw_check(uint64_t mask) { g_nbd_check = mask; }
// DM_GFX_COND_RENDER != 0 (hle_nvn.c condr_rec): a SetRenderEnable(Conditional) the shadow holds (nbd_take_ren) and no modelled draw has
// carried yet. Without the switch the flags are only notes, as before (never pending, never written).
static int nbd_ren_pending(void) { return condr_rec() && !t_nbd_chk && (t_nbd.s.h.flags & (NBD_F_REN | NBD_F_COND)); }
// A pending condition goes out as the OP_X_RENDER_COND record it replaced (into the shadow's command buffer) and leaves the shadow: whatever
// comes next in the stream (an unmodelled draw, the next bracket, another command buffer) runs under it, as NVN state does.
static void nbd_ren_emit(void) {
    nbd_hdr *h = &t_nbd.s.h;
    if (!nbd_ren_pending()) return;
    const int kind = (h->flags & NBD_F_COND) ? 2 : (h->flags & NBD_F_REN_ON) ? 0 : 1;
    if (t_nbd.cb) { const int e = t_nbd.emitting; t_nbd.emitting = 1; condr_write((NVNcommandBuffer *)t_nbd.cb, kind, kind == 2 ? h->cond_mode : 0, kind == 2 ? h->cond_addr : 0); t_nbd.emitting = e; }
    h->flags &= (uint8_t)~(NBD_F_REN | NBD_F_REN_ON | NBD_F_COND); h->cond_mode = 0; h->cond_addr = 0;
}
// The shadow's binds as the NVN records bind_handles / BindUniformBuffer(s) would have written (one handle each), into its command buffer.
static void nbd_flush(int why) {
    cmd_t *o = t_nbd.cb; const unsigned n = t_nbd.s.h.nbind;
    if (o && n && t_nbd_chk) nbds_add(why, 1);   // NB-10 check: the records are written already
    else if (o && n) {
        NVNcommandBuffer *c = (NVNcommandBuffer *)o;
        const int e = t_nbd.emitting; t_nbd.emitting = 1;
        for (unsigned i = 0; i < n; i++) {
            const nbd_bind *b = &t_nbd.s.bind[i];
            if (b->kind == NBD_B_UBO) *REC(c, OP_BIND_UBO, p_bind_buf) = (p_bind_buf){b->stage, b->slot, b->a, b->b};
            else {
                const int op = b->kind == NBD_B_TEX ? OP_BIND_TEX : b->kind == NBD_B_SAMP ? OP_BIND_SAMP : OP_BIND_IMG;
                p_bind_handles *r = RECN(c, op, p_bind_handles, h, 1); r->stage = b->stage; r->first = b->slot; r->n = 1; r->h[0] = b->a;
            }
        }
        t_nbd.emitting = e;
        nbds_add(why, 1); nbds_add(NBDS_FLUSH_BINDS, n);
    }
    nbd_ren_emit();   // DM_GFX_COND_RENDER: a pending condition goes out after the binds (independent state, order between them is free)
    // the render-enable flags stay: they belong to the next draw of the bracket (no record either way)
    const uint8_t fl = t_nbd.s.h.flags, cm = t_nbd.s.h.cond_mode; const uint64_t ca = t_nbd.s.h.cond_addr;
    nbd_reset(); t_nbd.s.h.flags = fl; t_nbd.s.h.cond_mode = cm; t_nbd.s.h.cond_addr = ca;
}
static void nbd_foreign(cmd_t *o) {
    if (t_nbd_mirror) { t_nbd_mirror = 0; return; }   // NB-10 check: the old record of a modelled call, not foreign
    nbds_add(NBDS_FOREIGN, 1);
    (void)o;   // a record for another command buffer flushes too: the shadow's records go to their own buffer, the order there is kept
    if (t_nbd.s.h.nbind || nbd_ren_pending()) nbd_flush(NBDS_FOREIGN_FLUSH);
}
// The command buffer the shadow belongs to; a call into another one flushes the shadow first.
static void nbd_cb(NVNcommandBuffer *c) {
    cmd_t *o = CB(c);
    if (t_nbd.cb != o) { if (t_nbd.s.h.nbind || nbd_ren_pending()) nbd_flush(NBDS_CB); t_nbd.cb = o; }
}
// A bind into the shadow. 0 = not modelled (slot outside the backend's limits): the caller writes its record (cmd_alloc flushes first).
static int nbd_take_bind(NVNcommandBuffer *c, unsigned kind, int stage, int slot, uint64_t a, uint64_t b) {
    if (!nbd_bind_ok(kind, stage, slot)) return 0;
    nbd_cb(c);
    if (!nbd_add(&t_nbd.s, kind, stage, slot, a, b)) { nbd_flush(NBDS_FULL); nbd_add(&t_nbd.s, kind, stage, slot, a, b); }
    if (t_nbd_chk) { t_nbd_mirror = 1; return 0; }   // NB-10 check: the caller writes the old record too
    return 1;
}
static void nbd_render_enable(NVNcommandBuffer *c, int on) {
    if (!NBD_TAKE(c)) return;
    t_nbd.s.h.flags = (uint8_t)((t_nbd.s.h.flags & ~NBD_F_REN_ON) | NBD_F_REN | (on ? NBD_F_REN_ON : 0));
}
static void nbd_render_cond(NVNcommandBuffer *c, int mode, uint64_t addr) {
    if (!NBD_TAKE(c)) return;
    t_nbd.s.h.flags |= NBD_F_COND; t_nbd.s.h.cond_mode = (uint8_t)mode; t_nbd.s.h.cond_addr = addr;
}
// DM_GFX_COND_RENDER != 0: SetRenderEnable (kind 0 on / 1 off) or SetRenderEnableConditional (kind 2, mode, address) into the shadow; the
// last call of the bracket wins (one condition is in force at a time; without the switch the flags accumulate as notes, unchanged). The
// modelled draw carries it in its flags (xbg_nb.inc nbdraw applies it before the draw); any flush writes it out as OP_X_RENDER_COND.
// 1 = taken; 0 = the caller writes OP_X_RENDER_COND (NB-10 check: the mirror of the call, the backend executes it).
static int nbd_take_ren(NVNcommandBuffer *c, int kind, int mode, uint64_t addr) {
    nbd_cb(c);
    nbd_hdr *h = &t_nbd.s.h;
    h->flags &= (uint8_t)~(NBD_F_REN | NBD_F_REN_ON | NBD_F_COND);
    if (kind == 2) { h->flags |= NBD_F_COND; h->cond_mode = (uint8_t)mode; h->cond_addr = addr; }
    else { h->flags |= (uint8_t)(NBD_F_REN | (kind == 0 ? NBD_F_REN_ON : 0)); h->cond_mode = 0; h->cond_addr = 0; }
    if (t_nbd_chk) { t_nbd_mirror = 1; return 0; }
    return 1;
}
// NR-09 (DM_NR_DRAW, armed by the front end hle_dos2_nr.c before guest code runs): the record of a draw whose engine identity the NR-02
// hook took right before this NVN draw (dm_nr_draw_ident, same thread) becomes an OP_X_NR_DRAW: NBD_F_NR + the nrd_tail after the binds.
// No identity (the hook is off / did not run for this draw) -> the plain OP_X_NB_DRAW, as before. g_nrd_on == 0: one load, nothing else.
static int g_nrd_on;
static uint64_t g_nrds[2];   // records with the tail / without an identity (atomics)
int dm_nr_draw_ident(uint32_t *key, uint32_t *texset, unsigned *why, unsigned *site) __attribute__((weak));
void nvn_nr_draw_enable(int on);
void nvn_nr_draw_enable(int on) { g_nrd_on = on && dm_nr_draw_ident; }
void nvn_nr_draw_stats(uint64_t out[2]);
void nvn_nr_draw_stats(uint64_t out[2]) { for (int i = 0; i < 2; i++) out[i] = __atomic_exchange_n(&g_nrds[i], 0, __ATOMIC_RELAXED); }
// The draw: the shadow + the draw arguments as one OP_X_NB_DRAW. Always 1 (the record is written).
static int nbd_draw(NVNcommandBuffer *c, int kind, int prim, int itype, int count, int bv, int bi, int inst, uint64_t ib, int first) {
    nbd_cb(c);
    nbd_hdr *h = &t_nbd.s.h;
    h->engine_op = (uint8_t)t_nbd.op; h->draw = (uint8_t)kind; h->texgen = __atomic_load_n(&g_nbd_texgen, __ATOMIC_RELAXED);
    h->prim = prim; h->itype = itype; h->count = count; h->base_vertex = bv; h->base_instance = bi; h->instances = inst; h->ibuf = ib; h->first = first;
    if (t_nbd_chk) h->flags |= NBD_F_CHECK;
    uint32_t nr_key = 0, nr_ts = 0; unsigned nr_why = 0, nr_site = 0xff;   // [DOS2] NR-09 DM_NR_DRAW
    const int nr = __builtin_expect(g_nrd_on, 0) && dm_nr_draw_ident(&nr_key, &nr_ts, &nr_why, &nr_site);
    if (nr) h->flags |= NBD_F_NR;
    const uint32_t size = nbd_size(&t_nbd.s) + (nr ? (uint32_t)sizeof(nrd_tail) : 0u);
    const int e = t_nbd.emitting; t_nbd.emitting = 1;
    uint8_t *p = (uint8_t *)rec(c, OP_X_NB_DRAW, size);
    t_nbd.emitting = e;
    nbd_pack(&t_nbd.s, p, size);
    if (nr) nrd_tail_put(p, size, nr_key, nr_ts, nr_why, nr_site);
    NRD_EXT_PUT(nr, p, size);   // [DOS2] NR-09R DM_NR_DIRECT: the tail's spare word (nothing without the switch)
    if (__builtin_expect(g_nrd_on, 0)) __atomic_fetch_add(&g_nrds[nr ? 0 : 1], 1, __ATOMIC_RELAXED);
    nbds_add(NBDS_DRAWS, 1); nbds_add(NBDS_BINDS, h->nbind); nbds_add(NBDS_BYTES, (size + 8 + 7) & ~7u);
    nbd_reset(); t_nbd.drew = 1;
    if (t_nbd_chk) { t_nbd_mirror = 1; return 0; }   // NB-10 check: the caller writes the old draw record right after this one
    return 1;
}
void nvn_nb_draw_enable(void);
void nvn_nb_draw_begin(unsigned op);
void nvn_nb_draw_end(void);
void nvn_nb_draw_enable(void) { g_nbd_on = 1; }
void nvn_nb_draw_begin(unsigned op) {
    if (!g_nbd_on) return;
    t_nbd_chk = op < 64 && (g_nbd_check >> op & 1); t_nbd_mirror = 0;   // NB-10: DM_NB_OPS_CHECK wins over DM_NB_OPS for the opcode
    t_nbd.in = t_nd.mode != 2; t_nbd.op = op; t_nbd.cb = NULL; t_nbd.drew = 0; t_nbd.emitting = 0; nbd_reset();
    nbds_add(NBDS_BRACKETS, 1);
}
void nvn_nb_draw_end(void) {
    if (!t_nbd.in) return;
    if (t_nbd.s.h.nbind || nbd_ren_pending()) nbd_flush(NBDS_ORPHAN);
    if (!t_nbd.drew) nbds_add(NBDS_NODRAW, 1);
    t_nbd.in = 0; t_nbd.cb = NULL; nbd_reset(); t_nbd_chk = 0; t_nbd_mirror = 0;
}
#endif
