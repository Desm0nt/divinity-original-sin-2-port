// [DOS2] NR-02: draw identity tables (nr_key.h). Pure C; the caller serialises.
#include "nr_key.h"
#include <stdlib.h>
#include <string.h>

const char *nr_dsite_name(int kind) {
    static const char *const n[NR_DS_N] = {"unknown", "list-indexed", "indexed-instanced", "list-arrays", "full-state", "direct-indexed", "direct-arrays",
                                           "flush-only", "compute"};
    return kind >= 0 && kind < NR_DS_N ? n[kind] : "?";
}
int nr_dsite_is_draw(int kind) { return kind != NR_DS_FLUSH && kind != NR_DS_COMPUTE && kind >= 0 && kind < NR_DS_N; }
const char *nr_ne_name(int bit) {
    static const char *const n[NR_NE_BITS] = {"shader", "compute", "pack-key", "state", "vformat", "rt", "full-state", "lost"};
    return bit >= 0 && bit < NR_NE_BITS ? n[bit] : "?";
}
int nr_ne_text(unsigned mask, char *out, size_t cap) {
    int k = 0;
    if (!cap) return 0;
    out[0] = 0;
    if (!mask) { snprintf(out, cap, "eligible"); return (int)strlen(out); }
    for (int b = 0; b < NR_NE_BITS; b++) if (mask >> b & 1) { const int n = snprintf(out + k, cap - (size_t)k, "%s%s", k ? "+" : "", nr_ne_name(b)); if (n < 0 || (size_t)(k + n) >= cap) break; k += n; }
    return k;
}

nr_keys *nr_keys_new(void) {
    nr_keys *k = (nr_keys *)calloc(1, sizeof *k);
    if (!k) return NULL;
    k->key = (nr_kent *)calloc(NR_KEY_MAX, sizeof *k->key); k->ts = (nr_texset *)calloc(NR_TEXSET_MAX, sizeof *k->ts);
    k->rtf = (nr_rtfmt *)calloc(NR_RTFMT_MAX, sizeof *k->rtf); k->tid = (nr_texid *)calloc(NR_TEXID_MAX, sizeof *k->tid);
    k->kidx = (uint32_t *)calloc(2 * NR_KEY_MAX, sizeof *k->kidx); k->tsidx = (uint32_t *)calloc(2 * NR_TEXSET_MAX, sizeof *k->tsidx);
    if (!k->key || !k->ts || !k->rtf || !k->tid || !k->kidx || !k->tsidx) { nr_keys_free(k); return NULL; }
    k->win = 1;
    return k;
}
void nr_keys_free(nr_keys *k) {
    if (!k) return;
    free(k->key); free(k->ts); free(k->rtf); free(k->tid); free(k->kidx); free(k->tsidx);
    free(k);
}
void nr_keys_window(nr_keys *k) { k->win++; k->win_keys = k->win_ts = 0; }

static uint64_t nr_mix(uint64_t h, uint64_t v) { h ^= v; h *= 0x100000001b3ull; return h ^ (h >> 29); }

// ---- small registry lookups ----
const nr_state *nr_state_find(const nr_reg *r, int kind, uint64_t handle) {
    if (kind < 0 || kind >= NR_ST_N || !handle) return NULL;
    const uint32_t i = nr_h_idx(handle);
    return i < NR_STATE_MAX && r->state[kind][i].alive && r->state[kind][i].handle == handle ? &r->state[kind][i] : NULL;
}
const nr_vformat *nr_vformat_find(const nr_reg *r, uint64_t handle) {
    if (!handle) return NULL;
    const uint32_t i = nr_h_idx(handle);
    return i < NR_VF_MAX && r->vformat[i].alive && r->vformat[i].handle == handle ? &r->vformat[i] : NULL;
}
const nr_shader *nr_shader_find_name(const nr_reg *r, const char *name) {
    if (!name || !name[0]) return NULL;
    for (uint32_t i = 0; i < r->st[NR_K_SHADER].hi; i++) if (r->shader[i].alive && !strcmp(r->shader[i].name, name)) return &r->shader[i];
    return NULL;
}
const nr_shader *nr_shader_find_key(const nr_reg *r, const char *key, int *stage) {
    if (!key || !key[0]) return NULL;
    for (uint32_t i = 0; i < r->st[NR_K_SHADER].hi; i++) {
        const nr_shader *s = &r->shader[i];
        if (!s->alive) continue;
        for (int st = 0; st < NR_STAGES; st++) if (s->key[st][0] && !strcmp(s->key[st], key)) { if (stage) *stage = st; return s; }
    }
    return NULL;
}

// ---- render-target format sets ----
uint16_t nr_rtfmt_note(nr_keys *k, const nr_reg *r, const uint64_t color[NR_RT_COLORS], uint64_t depth) {
    nr_rtfmt f; memset(&f, 0, sizeof f);
    f.msaa = 0;
    for (int i = 0; i < NR_RT_COLORS; i++) {
        f.fmt[i] = NR_FMT_NONE;
        if (!color[i]) continue;
        f.n = (uint8_t)(i + 1);
        const nr_tex *t = nr_tex_get(r, color[i]);
        if (!t) { f.fmt[i] = NR_FMT_UNKNOWN; f.unknown = 1; continue; }
        f.fmt[i] = t->efmt;
        if (!f.msaa) f.msaa = t->msaa ? t->msaa : 1;
    }
    f.depth = NR_FMT_NONE;
    if (depth) {
        const nr_tex *t = nr_tex_get(r, depth);
        if (!t) { f.depth = NR_FMT_UNKNOWN; f.unknown = 1; }
        else { f.depth = t->efmt; if (!f.msaa) f.msaa = t->msaa ? t->msaa : 1; }
    }
    if (!f.msaa) f.msaa = 1;
    for (uint32_t i = 0; i < k->nrtf; i++) {
        nr_rtfmt *o = &k->rtf[i];
        if (o->n == f.n && o->depth == f.depth && o->msaa == f.msaa && o->unknown == f.unknown && !memcmp(o->fmt, f.fmt, sizeof f.fmt)) { o->uses++; return (uint16_t)o->id; }
    }
    if (k->nrtf >= NR_RTFMT_MAX) { k->rtf_lost++; return 0; }
    f.id = ++k->nrtf; f.uses = 1;
    k->rtf[f.id - 1] = f;
    return (uint16_t)f.id;
}
const nr_rtfmt *nr_rtfmt_get(const nr_keys *k, uint16_t id) { return id && id <= k->nrtf ? &k->rtf[id - 1] : NULL; }

// ---- draw keys ----
uint64_t nr_dkey_hash(const nr_dkey *key) {
    uint64_t h = 0xcbf29ce484222325ull;
    h = nr_mix(h, key->shader); h = nr_mix(h, key->blend); h = nr_mix(h, key->depth); h = nr_mix(h, key->raster); h = nr_mix(h, key->vformat);
    h = nr_mix(h, (uint64_t)key->blend_arg << 32 | (uint64_t)key->rtfmt << 16 | (uint64_t)key->prim << 8 | key->alpha_test);
    return h ? h : 1;
}
static int nr_dkey_eq(const nr_dkey *a, const nr_dkey *b) {
    return a->shader == b->shader && a->blend == b->blend && a->depth == b->depth && a->raster == b->raster && a->vformat == b->vformat &&
           a->blend_arg == b->blend_arg && a->rtfmt == b->rtfmt && a->prim == b->prim && a->alpha_test == b->alpha_test;
}
unsigned nr_dkey_why(const nr_reg *r, const nr_keys *k, const nr_dkey *key) {
    unsigned why = 0;
    const nr_shader *s = nr_shader_get(r, key->shader);
    if (!s) why |= NR_NE_SHADER;
    else if (s->prog[NR_STAGE_CS] || !s->prog[NR_STAGE_VS] || !s->prog[NR_STAGE_FS]) why |= NR_NE_COMPUTE;
    else if (!s->key[NR_STAGE_VS][0] || !s->key[NR_STAGE_FS][0]) why |= NR_NE_PACKKEY;
    if (!nr_state_find(r, NR_ST_BLEND, key->blend) || !nr_state_find(r, NR_ST_DEPTH, key->depth) || !nr_state_find(r, NR_ST_RASTER, key->raster)) why |= NR_NE_STATE;
    if (!nr_vformat_find(r, key->vformat)) why |= NR_NE_VFORMAT;
    const nr_rtfmt *f = nr_rtfmt_get(k, key->rtfmt);
    if (!f || f->unknown || (!f->n && f->depth == NR_FMT_NONE)) why |= NR_NE_RT;
    return why;
}
const nr_kent *nr_key_note(nr_keys *k, const nr_reg *r, const nr_dkey *key, int *fresh) {
    if (fresh) *fresh = 0;
    const uint64_t h = nr_dkey_hash(key);
    const uint32_t cap = 2 * NR_KEY_MAX;
    for (uint32_t p = (uint32_t)h & (cap - 1), n = 0; n < cap; p = (p + 1) & (cap - 1), n++) {
        const uint32_t id = k->kidx[p];
        if (!id) {
            if (!r) { if (fresh) *fresh = -1; return NULL; }   // new key, no registry at hand: the caller retries with it (not a loss)
            if (k->nkey >= NR_KEY_MAX) break;
            nr_kent *e = &k->key[k->nkey];
            memset(e, 0, sizeof *e);
            e->k = *key; e->h = h; e->id = ++k->nkey; e->why = nr_dkey_why(r, k, key); e->first_win = k->win;
            k->kidx[p] = e->id;
            if (fresh) *fresh = 1;
            e->draws = 1; e->last_win = k->win; k->win_keys++;
            return e;
        }
        nr_kent *e = &k->key[id - 1];
        if (e->h == h && nr_dkey_eq(&e->k, key)) {
            e->draws++;
            if (e->last_win != k->win) { e->last_win = k->win; k->win_keys++; }
            return e;
        }
    }
    k->key_lost++;
    return NULL;
}
const nr_kent *nr_key_get(const nr_keys *k, uint32_t id) { return id && id <= k->nkey ? &k->key[id - 1] : NULL; }

// ---- texture sets ----
uint64_t nr_texset_hash(const int32_t tex[NR_TS_STAGES][NR_TS_SLOTS], const int32_t smp[NR_TS_STAGES][NR_TS_SLOTS]) {
    uint64_t h = 0x84222325cbf29ce4ull;
    for (int s = 0; s < NR_TS_STAGES; s++) for (int i = 0; i < NR_TS_SLOTS; i++) h = nr_mix(h, (uint64_t)(uint32_t)tex[s][i] << 32 | (uint32_t)smp[s][i]);
    return h ? h : 1;
}
const nr_texset *nr_texset_note(nr_keys *k, const nr_reg *r, uint64_t h, const int32_t tex[NR_TS_STAGES][NR_TS_SLOTS], int *fresh) {
    if (fresh) *fresh = 0;
    const uint32_t cap = 2 * NR_TEXSET_MAX;
    for (uint32_t p = (uint32_t)h & (cap - 1), n = 0; n < cap; p = (p + 1) & (cap - 1), n++) {
        const uint32_t id = k->tsidx[p];
        if (!id) {
            if (!r) { if (fresh) *fresh = -1; return NULL; }
            if (k->nts >= NR_TEXSET_MAX) break;
            nr_texset *e = &k->ts[k->nts];
            memset(e, 0, sizeof *e);
            e->h = h; e->id = ++k->nts; e->first_win = k->win;
            for (int s = 0; s < NR_TS_STAGES; s++) for (int i = 0; i < NR_TS_SLOTS; i++) {
                if (tex[s][i] == -1) continue;
                e->nslots++;
                const nr_tex *t = nr_texid_tex(k, r, (uint32_t)tex[s][i]);
                if (!t) e->nunknown++;
                else if (t->src == NR_SRC_RT || t->src == NR_SRC_WINDOW) e->nrt++;
                else e->nmat++;
            }
            k->tsidx[p] = e->id;
            if (fresh) *fresh = 1;
            e->draws = 1; e->last_win = k->win; k->win_ts++;
            return e;
        }
        nr_texset *e = &k->ts[id - 1];
        if (e->h == h) {
            e->draws++;
            if (e->last_win != k->win) { e->last_win = k->win; k->win_ts++; }
            return e;
        }
    }
    k->ts_lost++;
    return NULL;
}
const nr_texset *nr_texset_get(const nr_keys *k, uint32_t id) { return id && id <= k->nts ? &k->ts[id - 1] : NULL; }
void nr_texset_count(nr_keys *k, uint32_t id) {
    if (!id || id > k->nts) return;
    nr_texset *e = &k->ts[id - 1];
    e->draws++;
    if (e->last_win != k->win) { e->last_win = k->win; k->win_ts++; }
}

// ---- pool id -> handle ----
static nr_texid *nr_texid_slot(const nr_keys *k, uint32_t id) {
    const uint64_t h = nr_mix(0xcbf29ce484222325ull, id);
    for (uint32_t p = (uint32_t)h & (NR_TEXID_MAX - 1), n = 0; n < NR_TEXID_MAX; p = (p + 1) & (NR_TEXID_MAX - 1), n++) {
        nr_texid *t = &k->tid[p];
        if (!t->used || t->id == id) return t;
    }
    return NULL;
}
int nr_texid_put(nr_keys *k, uint32_t id, uint64_t handle) {
    nr_texid *t = nr_texid_slot(k, id);
    if (!t || (!t->used && k->ntid >= NR_TEXID_MAX * 3 / 4)) { k->tid_lost++; return 0; }
    if (!t->used) { t->used = 1; t->id = id; k->ntid++; }
    t->handle = handle;
    return 1;
}
uint64_t nr_texid_get(const nr_keys *k, uint32_t id) { const nr_texid *t = nr_texid_slot(k, id); return t && t->used ? t->handle : 0; }
const nr_tex *nr_texid_tex(const nr_keys *k, const nr_reg *r, uint32_t id) { const uint64_t h = nr_texid_get(k, id); return h ? nr_tex_get(r, h) : NULL; }

// ---- bundle layout word ----
uint32_t nr_layout_word(const uint32_t *b, unsigned n) {
    unsigned ubo = 0, tex = 0, fl = 0;
    for (unsigned i = 0; i < n; i++, b += 8) {
        const uint32_t count = b[2], src = b[4];
        switch (src) {
        case 2: ubo++; if (count > 1) fl |= NR_LW_ARRAYS; break;            // uniform block of the game (NVN slot)
        case 7: tex++; if (count > 1) fl |= NR_LW_ARRAYS; break;            // texture (combined image sampler)
        case 4: fl |= NR_LW_C1; break;                                      // the bundle's own constant bank
        case 1: fl |= NR_LW_SUPPORT; break;                                 // host support buffer
        case 5: case 6: fl |= NR_LW_DRIVER; break;                          // driver constant banks
        case 3: case 8: case 10: case 11: fl |= NR_LW_STORAGE; break;       // storage buffer / image
        default: fl |= NR_LW_OTHER; break;                                  // bindless pools and anything unknown
        }
    }
    if (!(fl & (NR_LW_STORAGE | NR_LW_ARRAYS | NR_LW_OTHER)) && ubo <= NR_LW_MAX_UBO) fl |= NR_LW_FITS;
    if (ubo > 15) { ubo = 15; fl |= NR_LW_CLAMPED; }
    if (tex > 63) { tex = 63; fl |= NR_LW_CLAMPED; }
    return (uint32_t)NR_LW_TAG << 28 | (uint32_t)NR_LW_VERSION << 24 | ubo << 20 | tex << 14 | fl;
}
int nr_layout_valid(uint32_t w) { return w >> 28 == NR_LW_TAG && (w >> 24 & 15) == NR_LW_VERSION && !(w >> 8 & 0x3f); }
unsigned nr_layout_ubos(uint32_t w) { return w >> 20 & 15; }
unsigned nr_layout_textures(uint32_t w) { return w >> 14 & 63; }
unsigned nr_layout_flags(uint32_t w) { return w & 0xff; }
int nr_layout_text(uint32_t w, char *out, size_t cap) {
    if (!cap) return 0;
    if (!nr_layout_valid(w)) return snprintf(out, cap, "no NR layout (%#x)", w);
    static const char *const f[8] = {"c1", "support", "driver", "storage", "arrays", "other", "fits", "clamped"};
    int k = snprintf(out, cap, "NR v%u: %u ubo, %u tex", w >> 24 & 15, nr_layout_ubos(w), nr_layout_textures(w));
    for (int b = 0; b < 8 && k > 0 && (size_t)k < cap; b++) if (w >> b & 1) k += snprintf(out + k, cap - (size_t)k, " %s", f[b]);
    return k;
}

// ---- TSV ----
const char *nr_ktsv_name(int w) {
    static const char *const n[NR_KTSV_N] = {"nr_keys.tsv", "nr_texsets.tsv", "nr_rtfmts.tsv", "nr_names.tsv"};
    return w >= 0 && w < NR_KTSV_N ? n[w] : "nr_unknown.tsv";
}
static void nr_state_args(FILE *o, const nr_reg *r, int kind, uint64_t h) {
    const nr_state *s = nr_state_find(r, kind, h);
    if (!s) { fputs("\t-", o); return; }
    fputc('\t', o);
    for (int i = 0; i < s->nargs; i++) fprintf(o, "%s%u", i ? "," : "", s->arg[i]);
    if (!s->nargs) fputc('-', o);
}
#define H64(x) ((unsigned long long)(x))
int nr_keys_write_tsv(const nr_keys *k, const nr_reg *r, int which, FILE *o) {
    int rows = 0;
    char why[96];
    switch (which) {
    case NR_KTSV_KEYS:
        fprintf(o, "id\tdraws\twhy\tshader\tname\tvs_key\tfs_key\tblend\tblend_args\tblend_arg\tdepth\tdepth_args\traster\traster_args\tvformat\tvf_elems\tprim\talpha_test\trtfmt\tfirst_win\tlast_win\n");
        for (uint32_t i = 0; i < k->nkey; i++, rows++) {
            const nr_kent *e = &k->key[i];
            const nr_shader *s = nr_shader_get(r, e->k.shader); const nr_vformat *v = nr_vformat_find(r, e->k.vformat);
            nr_ne_text(e->why, why, sizeof why);
            fprintf(o, "%u\t%llu\t%s\t%#llx\t%s\t%s\t%s\t%#llx", e->id, H64(e->draws), why, H64(e->k.shader), s && s->name[0] ? s->name : "-",
                    s && s->key[0][0] ? s->key[0] : "-", s && s->key[1][0] ? s->key[1] : "-", H64(e->k.blend));
            nr_state_args(o, r, NR_ST_BLEND, e->k.blend);
            fprintf(o, "\t%#x\t%#llx", e->k.blend_arg, H64(e->k.depth)); nr_state_args(o, r, NR_ST_DEPTH, e->k.depth);
            fprintf(o, "\t%#llx", H64(e->k.raster)); nr_state_args(o, r, NR_ST_RASTER, e->k.raster);
            fprintf(o, "\t%#llx\t%d\t%u\t%u\t%u\t%u\t%u\n", H64(e->k.vformat), v ? (int)v->count : -1, e->k.prim, e->k.alpha_test, e->k.rtfmt, e->first_win, e->last_win);
        }
        break;
    case NR_KTSV_TEXSETS:
        fprintf(o, "id\tdraws\thash\tslots\trt\tmaterial\tunknown\tfirst_win\tlast_win\n");
        for (uint32_t i = 0; i < k->nts; i++, rows++) {
            const nr_texset *e = &k->ts[i];
            fprintf(o, "%u\t%llu\t%016llx\t%u\t%u\t%u\t%u\t%u\t%u\n", e->id, H64(e->draws), H64(e->h), e->nslots, e->nrt, e->nmat, e->nunknown, e->first_win, e->last_win);
        }
        break;
    case NR_KTSV_RTFMTS:
        fprintf(o, "id\tuses\tcolors\tc0\tc1\tc2\tc3\tc4\tc5\tc6\tc7\tdepth\tmsaa\tunknown\n");
        for (uint32_t i = 0; i < k->nrtf; i++, rows++) {
            const nr_rtfmt *f = &k->rtf[i];
            fprintf(o, "%u\t%llu\t%u", f->id, H64(f->uses), f->n);
            for (int c = 0; c < NR_RT_COLORS; c++) {
                if (f->fmt[c] == NR_FMT_NONE) fputs("\t-", o);
                else if (f->fmt[c] == NR_FMT_UNKNOWN) fputs("\t?", o);
                else fprintf(o, "\t%s", nr_fmt_name(f->fmt[c]));
            }
            fprintf(o, "\t%s\t%u\t%u\n", f->depth == NR_FMT_NONE ? "-" : f->depth == NR_FMT_UNKNOWN ? "?" : nr_fmt_name(f->depth), f->msaa, f->unknown);
        }
        break;
    case NR_KTSV_NAMES:   // name <-> pack key: every shader the registry knows with a name or a keyed program
        fprintf(o, "name\tpath\thandle\talive\tvs_key\tfs_key\tcs_key\tgets\n");
        for (uint32_t i = 0; i < r->st[NR_K_SHADER].hi; i++) {
            const nr_shader *s = &r->shader[i];
            if (!s->creates || (!s->name[0] && !s->key[0][0] && !s->key[1][0] && !s->key[2][0])) continue;
            fprintf(o, "%s\t%s\t%#llx\t%u\t%s\t%s\t%s\t%u\n", s->name[0] ? s->name : "-", s->path[0] ? s->path : "-", H64(s->handle), s->alive,
                    s->key[0][0] ? s->key[0] : "-", s->key[1][0] ? s->key[1] : "-", s->key[2][0] ? s->key[2] : "-", s->gets);
            rows++;
        }
        break;
    default: return -1;
    }
    return rows;
}
