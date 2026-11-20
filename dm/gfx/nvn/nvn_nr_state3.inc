// [DOS2] NR-09S3 (DM_NR_STATE=4 / 5, default 0; docs/dos2/nr/NR-09S.md §10): the front end of the native state record. Included at the end
// of hle_nvn.c (after the second nvn_nr_state.inc: it needs the bodies, rec(), snap_prog, the NB tap and the NB-13a shadow's hooks).
// The packet's calls (nrst_op, the model of nr_state.h proven against the machine code) become ONE OP_X_NR_STATE record (nr_state3.h),
// written into the packet's command buffer instead of the 2-30 NVN state records the bodies wrote one by one through the NB-13a shadow.
// What the bodies did besides their records is done here, so nothing downstream sees a difference but the record:
//   - the engine's state objects: the setters ran on copies (nr3_build), the copies are written back (= what the setter bodies leave);
//   - NB-07's per-stage program of the IR tap (nbt_program) and NR-02's tap (dm_nr_tap_bind, DM_NR_KEY=2), per BindProgram as its body;
//   - NB tap notes (counts): NBN_STATE / NBN_VBUF with the bodies' numbers;
//   - the NB-13a shadow: flushed before the record, the record's kinds unknown after it (as around a plain state record);
//   - SetRenderEnable / SetRenderEnableConditional: through their bodies after the record (their effects are not state records).
#ifdef DM_GAME_DOS2
#define NR3_SNAP_PROG(p) ((uint64_t)(uintptr_t)snap_prog((const NVNprogram *)(uintptr_t)(p)))
#define NR3_BLEND_DEF(o) h_nvnBlendStateSetDefaults((NVNblendState *)(void *)(o))
#define NR3_BLEND_TARGET(o, t) h_nvnBlendStateSetBlendTarget((NVNblendState *)(void *)(o), (t))
#define NR3_COLOR_DEF(o) h_nvnColorStateSetDefaults((NVNcolorState *)(void *)(o))
#define NR3_COLOR_ATEST(o, f) h_nvnColorStateSetAlphaTest((NVNcolorState *)(void *)(o), (NVNalphaFunc)(f))
#define NR3_COLOR_BLEND(o, i, en) h_nvnColorStateSetBlendEnable((NVNcolorState *)(void *)(o), (i), (en))
#include "../nr/nr_state3.h"
_Static_assert(sizeof(blend_st) == NR3_SZ_BLEND && sizeof(color_st) == NR3_SZ_COLOR && sizeof(ds_st) == NR3_SZ_DS && sizeof(poly_st) == NR3_SZ_POLY
               && sizeof(va_st) == NR3_SZ_VATTR && sizeof(vs_st) == NR3_SZ_VSTREAM, "NR-09S3: the object sizes the bind bodies copy");
_Static_assert(NR3_K_BLEND == NBS_K_BLEND && NR3_K_COLOR == NBS_K_COLOR && NR3_K_POLY == NBS_K_POLY && NR3_K_DS == NBS_K_DS && NR3_K_VFMT == NBS_K_VFMT
               && NR3_K_VB == NBS_K_VB && NR3_K_PROG == NBS_K_PROG, "NR-09S3: the NB-13a kinds");
static __thread nr3_b t_nr3;
static void nr3_rec_write(NVNcommandBuffer *c, nr3_rec *r) {
    const uint32_t n = nr3_size(r);
    uint8_t *p = (uint8_t *)rec(c, OP_X_NR_STATE, n);
    nr3_put(r, p);
}
uint32_t nvn_nrs3_exec(const nrst_op *op, int n, uint64_t cb, int mode, uint32_t seq) {
    nr3_b *b = &t_nr3;
    if (!nr3_build(b, op, n)) return 0;
    NVNcommandBuffer *c = (NVNcommandBuffer *)(uintptr_t)cb;
    if (mode == 5) {   // two copies: the new record applied to a copy, the old records to the state, compared at CHECK_END
        b->r.flags = NR3_F_CHECK; b->r.seq = seq;
        const uint32_t sz = nr3_size(&b->r);
        nr3_rec_write(c, &b->r);
        nvn_nrs_exec(op, n);
        nr3_rec e; memset(&e, 0, sizeof e); e.flags = NR3_F_CHECK_END; e.seq = seq;
        nr3_rec_write(c, &e);
        return sz;
    }
    b->r.flags = 0; b->r.seq = 0;
    for (int i = 0; i < b->r.nprog; i++) {   // the taps of BindProgram, in its order (NR-02 first, then NB-07)
        const NVNprogram *p = (const NVNprogram *)(uintptr_t)b->praw[i]; const int st = b->r.pstage[i];
        NRT(dm_nr_tap_bind(c, p, st)); NBT(nbt_program(c, p, st)); (void)p; (void)st;
    }
    NBT(if ((b->notes_state || b->notes_vbuf) && nbt_f(c)) { if (b->notes_state) dm_nb_note(NBN_STATE, b->notes_state); if (b->notes_vbuf) dm_nb_note(NBN_VBUF, b->notes_vbuf); });
    nr3_writeback(b);
    const uint32_t sz = nr3_size(&b->r);
    NBS_EXT_BEFORE();
    uint8_t *w = __builtin_expect(g_nrn_defer, 0) ? nrnp_room(c, sz) : NULL;   // [DOS2] NR-NATIVE step B: the record waits for its draw (a short draw drops it)
    if (w) nr3_put(&b->r, w); else nr3_rec_write(c, &b->r);
    NBS_EXT_AFTER(nr3_kinds(&b->r));
    for (int i = 0; i < b->nren; i++) {
        const nrst_op *o = &op[b->ren[i]];
        if (o->fn == NRST_F_REN) h_nvnCommandBufferSetRenderEnable(c, (uint32_t)o->i);
        else h_nvnCommandBufferSetRenderEnableConditional(c, (NVNconditionalRenderMode)o->i, (NVNbufferAddress)o->p);
    }
    return sz;
}
#endif
