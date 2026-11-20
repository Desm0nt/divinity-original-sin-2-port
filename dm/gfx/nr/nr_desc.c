// [DOS2] NR-08: see nr_desc.h.
#include "nr_desc.h"
#include <stdlib.h>
#include <string.h>

const char *nr_dne_name(int why) {
    static const char *const n[NRD_NE_N] = {"eligible", "descriptor type", "source", "driver block", "array", "slot", "too many rows", "no shader", "device limit"};
    return why >= 0 && why < NRD_NE_N ? n[why] : "?";
}

int nr_dplan_build(nr_dplan *p, const nr_dstage st[NRD_STAGES]) {
    memset(p, 0, sizeof *p);
    int why = NRD_OK; unsigned any = 0;
    for (unsigned s = 0; s < NRD_STAGES && why == NRD_OK; s++) {
        if (!st[s].present) continue;
        any = 1;
        for (uint32_t i = 0; i < st[s].n && why == NRD_OK; i++) {
            const nr_dbind *b = &st[s].b[i];
            if (b->count != 1) { why = NRD_NE_ARRAY; break; }
            if (p->nrow >= NRD_MAX_ROWS || st[s].ns > 5) { why = NRD_NE_ROWS; break; }
            nr_drow r; memset(&r, 0, sizeof r);
            r.ns = (uint8_t)st[s].ns; r.st = (uint8_t)s; r.binding = b->binding; r.bytes = b->bytes; r.source = (uint8_t)b->source;
            r.dyn_at = r.stat_at = 0xff; r.samp = 0xff;
            const int dyn = st[s].dyn && st[s].dyn[i];
            if (b->type == NRD_T_UBO) {
                if (b->source == NRD_S_UNIFORM) {
                    if (b->slot >= 16) { why = NRD_NE_SLOT; break; }
                    if (p->nubo >= NRD_MAX_UBO) { why = NRD_NE_ROWS; break; }
                    r.kind = dyn ? NRD_K_RING_DYN : NRD_K_RING_STAT; r.slot = (uint8_t)b->slot;
                    if (!dyn) r.stat_at = p->nstat++;
                    p->ubo_row[p->nubo++] = p->nrow; p->ubo_stages |= (uint8_t)(1u << st[s].ns);
                } else if (b->source == NRD_S_SUPPORT || b->source == NRD_S_C1) r.kind = dyn ? NRD_K_FIXED_DYN : NRD_K_FIXED_STAT;
                else if (b->source == NRD_S_DRIVER0 || b->source == NRD_S_DRIVER2) { why = NRD_NE_DRIVER; break; }
                else { why = NRD_NE_SOURCE; break; }
            } else if (b->type == NRD_T_COMBINED || b->type == NRD_T_SAMPLED) {
                if (b->source != NRD_S_TEXTURE) { why = NRD_NE_SOURCE; break; }
                if (b->slot >= 128 || (b->sampler_slot != 0xffffffffu && b->sampler_slot >= 128)) { why = NRD_NE_SLOT; break; }
                if (p->nimg >= NRD_MAX_IMG) { why = NRD_NE_ROWS; break; }
                r.kind = NRD_K_IMAGE; r.slot = (uint8_t)b->slot; r.samp = b->sampler_slot == 0xffffffffu ? 0xff : (uint8_t)b->sampler_slot;
                p->img_row[p->nimg++] = p->nrow; p->tex_stages |= (uint8_t)(1u << st[s].ns);
            } else { why = NRD_NE_TYPE; break; }
            p->row[p->nrow++] = r;
        }
    }
    if (why == NRD_OK && !any) why = NRD_NE_NOSHADER;
    if (why == NRD_OK) {
        // dynamic offsets are consumed in binding order (one set, every count 1)
        unsigned nd = 0;
        for (unsigned i = 0; i < p->nrow; i++) if (p->row[i].kind == NRD_K_RING_DYN || p->row[i].kind == NRD_K_FIXED_DYN) nd++;
        if (nd > NRD_MAX_DYN) why = NRD_NE_ROWS;
        else {
            for (unsigned i = 0; i < p->nrow; i++) {
                nr_drow *r = &p->row[i];
                if (r->kind != NRD_K_RING_DYN && r->kind != NRD_K_FIXED_DYN) continue;
                unsigned rank = 0;
                for (unsigned j = 0; j < p->nrow; j++) {
                    const nr_drow *o = &p->row[j];
                    if (j != i && (o->kind == NRD_K_RING_DYN || o->kind == NRD_K_FIXED_DYN) && (o->binding < r->binding || (o->binding == r->binding && j < i))) rank++;
                }
                r->dyn_at = (uint8_t)rank;
            }
            p->ndyn = (uint8_t)nd;
        }
    }
    p->why = why;
    return why;
}

int nr_dplan_limits(nr_dplan *p, uint32_t stage_images, uint32_t stage_ubos, uint32_t set_dynamic) {
    if (p->why != NRD_OK) return p->why;
    unsigned img[6] = {0}, ubo[6] = {0};
    for (unsigned i = 0; i < p->nrow; i++) { if (p->row[i].kind == NRD_K_IMAGE) img[p->row[i].ns]++; else ubo[p->row[i].ns]++; }
    for (unsigned s = 0; s < 6; s++) if (img[s] > stage_images || ubo[s] > stage_ubos) p->why = NRD_NE_LIMIT;
    if (p->ndyn > set_dynamic) p->why = NRD_NE_LIMIT;
    return p->why;
}

unsigned nr_dsplit(const uint32_t *changes, unsigned n, unsigned limit, uint8_t *dyn) {
    unsigned nstat = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned ahead = 0;   // blocks that go before i: more changes, or as many and a lower index
        for (unsigned j = 0; j < n; j++) if (j != i && (changes[j] > changes[i] || (changes[j] == changes[i] && j < i))) ahead++;
        dyn[i] = ahead < limit;
        if (!dyn[i]) nstat++;
    }
    return nstat;
}

uint64_t nr_dkey_hash64(const uint64_t *key, unsigned n) {
    uint64_t h = 0x9e3779b97f4a7c15ull ^ ((uint64_t)n << 56);
    for (unsigned i = 0; i < n; i++) { h ^= key[i]; h *= 0xff51afd7ed558ccdull; h ^= h >> 32; }
    h *= 0xc4ceb9fe1a85ec53ull; h ^= h >> 29;
    return h;
}

int nr_dtab_init(nr_dtab *t, uint32_t max) {
    memset(t, 0, sizeof *t);
    if (max < 2 || max > (1u << 20)) return -1;
    uint32_t cap = 4; while (cap < max * 2) cap <<= 1;
    t->idx = (uint32_t *)calloc(cap, sizeof *t->idx);
    t->hash = (uint64_t *)calloc(max, sizeof *t->hash);
    t->key = (uint64_t *)calloc((size_t)max * NRD_KEY_WORDS, sizeof *t->key);
    t->nkey = (uint8_t *)calloc(max, 1);
    if (!t->idx || !t->hash || !t->key || !t->nkey) { nr_dtab_free(t); return -1; }
    t->max = max; t->cap = cap;
    return 0;
}
void nr_dtab_free(nr_dtab *t) { free(t->idx); free(t->hash); free(t->key); free(t->nkey); memset(t, 0, sizeof *t); }
void nr_dtab_clear(nr_dtab *t) { if (t->idx) memset(t->idx, 0, (size_t)t->cap * sizeof *t->idx); t->n = 0; }
int32_t nr_dtab_find(const nr_dtab *t, uint64_t h, const uint64_t *key, unsigned n) {
    if (!t->cap) return -1;
    for (uint32_t i = (uint32_t)h & (t->cap - 1);; i = (i + 1) & (t->cap - 1)) {
        const uint32_t id = t->idx[i];
        if (!id) return -1;
        if (t->hash[id - 1] == h && t->nkey[id - 1] == n && !memcmp(t->key + (size_t)(id - 1) * NRD_KEY_WORDS, key, n * sizeof *key)) return (int32_t)(id - 1);
    }
}
int32_t nr_dtab_add(nr_dtab *t, uint64_t h, const uint64_t *key, unsigned n) {
    if (!t->cap || t->n >= t->max || n > NRD_KEY_WORDS) return -1;
    uint32_t i = (uint32_t)h & (t->cap - 1);
    while (t->idx[i]) i = (i + 1) & (t->cap - 1);
    const uint32_t id = t->n++;
    t->idx[i] = id + 1; t->hash[id] = h; t->nkey[id] = (uint8_t)n;
    memcpy(t->key + (size_t)id * NRD_KEY_WORDS, key, n * sizeof *key);
    return (int32_t)id;
}

void nr_dfifo_init(nr_dfifo *f) { memset(f, 0, sizeof *f); f->head = f->tail = -1; }
void nr_dfifo_free(nr_dfifo *f) { free(f->node); nr_dfifo_init(f); }
static void nr_dfifo_unlink(nr_dfifo *f, int32_t id) {
    nr_dnode *nd = &f->node[id];
    if (nd->prev >= 0) f->node[nd->prev].next = nd->next; else f->head = nd->next;
    if (nd->next >= 0) f->node[nd->next].prev = nd->prev; else f->tail = nd->prev;
}
static void nr_dfifo_append(nr_dfifo *f, int32_t id) {
    nr_dnode *nd = &f->node[id];
    nd->prev = f->tail; nd->next = -1;
    if (f->tail >= 0) f->node[f->tail].next = id; else f->head = id;
    f->tail = id;
}
int32_t nr_dfifo_push(nr_dfifo *f, uint64_t set, uint32_t serial) {
    if (f->n >= f->cap) {
        const uint32_t cap = f->cap ? f->cap * 2 : 64;
        nr_dnode *node = (nr_dnode *)realloc(f->node, (size_t)cap * sizeof *node);
        if (!node) return -1;
        f->node = node; f->cap = cap;
        if (!f->n) f->head = f->tail = -1;
    }
    const int32_t id = (int32_t)f->n++;
    f->node[id].set = set; f->node[id].stamp = serial; f->node[id].gen = 1;
    nr_dfifo_append(f, id);
    f->fresh++;
    return id;
}
int32_t nr_dfifo_take(nr_dfifo *f, uint32_t serial, uint32_t age) {
    const int32_t id = f->n ? f->head : -1;
    if (id < 0) return -1;
    nr_dnode *nd = &f->node[id];
    if (age < NRD_AGE_SAFE) age = NRD_AGE_SAFE;
    if (serial < nd->stamp || serial - nd->stamp < age) return -1;   // every later node was bound at least as recently
    nr_dfifo_unlink(f, id); nr_dfifo_append(f, id);
    nd->gen++; nd->stamp = serial; f->taken++;
    return id;
}
int nr_dfifo_use(nr_dfifo *f, int32_t node, uint32_t gen, uint32_t serial) {
    if (node < 0 || (uint32_t)node >= f->n || f->node[node].gen != gen) return 0;
    if (f->node[node].stamp != serial) { f->node[node].stamp = serial; if (f->tail != node) { nr_dfifo_unlink(f, node); nr_dfifo_append(f, node); } }
    return 1;
}
