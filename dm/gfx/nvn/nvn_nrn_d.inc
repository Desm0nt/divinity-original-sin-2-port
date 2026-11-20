// [DOS2] NR-NATIVE step D, half b (DM_NRN_REC, default 0; docs/dos2/nr/NR-NATIVE-D.md section 5): the record of a CARD draw composed by
// the recorder itself. Included by hle_nvn.c right before nvn_nb_draw.inc (nbd_draw uses the two macros below). The composer is
// dm/gfx/nr/nr_nrn_d.h nrnd_rec_put (pure C; host test tests/nr_nrn_d_test.c holds it against nbd_pack + the tail writers).
//   DM_NRN_REC=0 / unset  nothing: nbd_draw pays one load.
//   DM_NRN_REC=2          verify: every draw record that carries a card is written the old way (nbd_pack, nrd_tail_put, nrn_tail_put,
//                         nrn_short_ext_put, nrd_tail_ext_put) and then composed again from the same values next to it; the two are
//                         compared BYTE FOR BYTE (differ = red line, first 16). The front end times the parts of a direct draw's
//                         big-stack call (hle_dos2_nrn_d.inc nrnd_rec_report).
//   DM_NRN_REC=1          apply: the payload of such a record is written by the composer alone (one pass over the header, the binds
//                         and the three tails). Everything around it is the recorder's own code in its order: rec() / cmd_alloc (the
//                         waiting state records, the foreign flush, command memory), the identity and the card callbacks, NR-09R's
//                         tail word, the counters, the reset of the shadow.
// Not done here (the doc lists each with its reason): the bodies of BindUniformBuffer / BindTexture, the tap's walk and the mirrors
// still run for a card draw - what they leave behind (the tap's tables, the two mirrors, the texture-set generation, the use marks)
// is read by later draws that are not card draws.
#ifdef DM_GAME_DOS2
#include "../nr/nr_nrn_d.h"
_Static_assert(NRND_R_HDR == sizeof(nbd_hdr) && NRND_R_BIND == sizeof(nbd_bind) && NRND_R_NRD == sizeof(nrd_tail), "nr_nrn_d.h repeats the layout of OP_X_NB_DRAW");
static int g_nrnd_rec;                 // DM_NRN_REC as armed (nvn_nrn_rec_enable, install time)
static uint64_t g_nrnd_recs[3];        // atomics: records composed (=1) / compared (=2) / differing (=2)
static unsigned g_nrnd_rec_red;
void nvn_nrn_rec_enable(int mode);
void nvn_nrn_rec_enable(int mode) { g_nrnd_rec = mode == 1 || mode == 2 ? mode : 0; }
void nvn_nrn_rec_stats(uint64_t out[3]);
void nvn_nrn_rec_stats(uint64_t out[3]) { for (int i = 0; i < 3; i++) out[i] = __atomic_exchange_n(&g_nrnd_recs[i], 0, __ATOMIC_RELAXED); }
// What nbd_draw holds when it writes the record of a card draw (the shadow s with its header filled, the identity, the card).
static inline void nrnd_rec_fill(nrnd_rec_in *in, const nbd_shadow *s, uint32_t key, uint32_t ts, unsigned why, unsigned site, uint32_t ext, uint64_t card, unsigned pass,
                                 uint32_t seq, unsigned rsv, const nrn_short_ext *sx) {
    const nbd_hdr *h = &s->h;
    in->engine_op = h->engine_op; in->draw = h->draw; in->flags = (uint8_t)(h->flags & ~(NBD_F_NR | NRN_F_FLAG)); in->cond_mode = h->cond_mode; in->nbind = h->nbind; in->texgen = h->texgen;
    in->prim = h->prim; in->itype = h->itype; in->count = h->count; in->base_vertex = h->base_vertex; in->base_instance = h->base_instance; in->instances = h->instances; in->first = h->first;
    in->ibuf = h->ibuf; in->cond_addr = h->cond_addr; in->binds = (const uint8_t *)s->bind;
    in->nr_key = key; in->nr_texset = ts; in->nr_why = (uint16_t)why; in->nr_site = (uint8_t)site; in->nr_ext = ext;
    in->card = card; in->pass = (uint8_t)pass; in->rsv = (uint16_t)rsv; in->seq = seq; in->sx = sx;
}
// =2, after the old way wrote the record at p (size bytes, NR-09R's word in place): the composed bytes against it.
static void nrnd_rec_check(const uint8_t *p, uint32_t size, const nbd_shadow *s, uint32_t key, uint32_t ts, unsigned why, unsigned site, uint64_t card, unsigned pass, uint32_t seq,
                           unsigned rsv, const nrn_short_ext *sx) {
    uint8_t b[sizeof(nbd_hdr) + sizeof(nbd_bind) * NBD_MAX_BINDS + sizeof(nrd_tail) + NRN_TAIL_BYTES + NRN_SHORT_EXT_BYTES];
    uint32_t ext; memcpy(&ext, p + nrd_tail_off(&s->h) + offsetof(nrd_tail, _pad), 4);   // NR-09R's word: nrd_ext_take ran once (it resets its counters) - its result is read back
    nrnd_rec_in in; nrnd_rec_fill(&in, s, key, ts, why, site, ext, card, pass, seq, rsv, sx);
    memset(b, 0, size <= sizeof b ? size : sizeof b);
    const uint32_t n = nrnd_rec_put(b, sizeof b, &in);
    __atomic_fetch_add(&g_nrnd_recs[1], 1, __ATOMIC_RELAXED);
    if (n == size && !memcmp(b, p, size)) return;
    __atomic_fetch_add(&g_nrnd_recs[2], 1, __ATOMIC_RELAXED);
    if (g_nrnd_rec_red++ < 16) {
        uint32_t at = 0; while (at < size && at < n && b[at] == p[at]) at++;
        LOG("【红】[nr] native rec: the record composed directly is not the record the NVN half wrote - %u / %u bytes, first difference at byte %u (%u binds, card %016llx, pass %u, %s)",
            n, size, at, (unsigned)s->h.nbind, (unsigned long long)card, pass, sx ? "short" : "long");
    }
}
// nbd_draw: 1 = the composer writes the payload (=1, a record with the identity and a card, not a checked bracket).
#define NRND_REC_APPLY(nr, nrn) (__builtin_expect(g_nrnd_rec == 1, 0) && (nr) && (nrn) && !t_nbd_chk)
#define NRND_REC_CHECK(nr, nrn, p, size, key, ts, why, site, card, pass, seq, rsv, sx) do { \
    if (__builtin_expect(g_nrnd_rec == 2, 0) && (nr) && (nrn) && !t_nbd_chk) nrnd_rec_check((p), (size), &t_nbd.s, (key), (ts), (why), (site), (card), (pass), (seq), (rsv), (sx)); } while (0)
#endif
