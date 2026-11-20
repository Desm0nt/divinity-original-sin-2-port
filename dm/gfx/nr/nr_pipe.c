// [DOS2] NR-07: the warm-up scheduler and the direct pipeline table (nr_pipe.h). Pure C.
#include "nr_pipe.h"
#include <stdlib.h>
#include <string.h>

uint64_t nrp_key_parse(const char *s, size_t n) {
    if (!s || n < 16) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        const char c = s[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = v << 4 | (uint64_t)d;
    }
    if (n > 16 && s[16] != '#') return 0;   // a longer name that is not "<key>#variant"
    return v ? v : 1;                        // 0 means "no key"; the all-zero digest (never seen) maps to 1
}
uint64_t nrp_stage_key(const char *s, size_t n, int *fused) {
    if (fused) *fused = 0;
    const uint64_t k = nrp_key_parse(s, n);
    if (k || !s || n < 3 + 16 || s[0] != 'F' || (s[1] != '1' && s[1] != '2')) return k;
    size_t d = 2;
    while (d < 4 && d < n && s[d] >= 'A' && s[d] <= 'Z') d++;
    if (d >= n || s[d] != '.') return 0;
    const uint64_t h = nrp_key_parse(s + d + 1, n - d - 1);
    if (h && fused) *fused = 1;
    return h;
}

static uint32_t mix32(uint64_t k) { k ^= k >> 33; k *= 0xff51afd7ed558ccdull; k ^= k >> 33; return (uint32_t)k; }
static int kfind(const nrp_sched *s, uint64_t key) {   // kent index, -1 = no record has it
    if (!s->kcap || !key) return -1;
    for (uint32_t h = mix32(key) & (s->kcap - 1);; h = (h + 1) & (s->kcap - 1)) {
        const uint32_t t = s->kidx[h];
        if (!t) return -1;
        if (s->kent[t - 1].key == key) return (int)t - 1;
    }
}
static int cmp_u64(const void *a, const void *b) { const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }

int nrp_init(nrp_sched *s, uint32_t nrec, const uint64_t *keys, const uint8_t *nkeys, int lazy) {
    memset(s, 0, sizeof *s);
    s->lazy = lazy != 0;
    if (!nrec) return 0;
    s->rec = (nrp_rec *)calloc(nrec, sizeof *s->rec);
    s->q = (uint32_t *)calloc(nrec, sizeof *s->q);
    uint64_t *all = (uint64_t *)malloc((size_t)nrec * NRP_STAGES * sizeof *all);
    if (!s->rec || !s->q || !all) { free(all); nrp_free(s); return -1; }
    s->nrec = s->left = nrec;
    size_t na = 0;
    for (uint32_t i = 0; i < nrec; i++) {
        nrp_rec *r = &s->rec[i];
        r->nkey = nkeys[i] > NRP_STAGES ? NRP_STAGES : nkeys[i];
        for (int k = 0; k < r->nkey; k++) { r->key[k] = keys[(size_t)i * NRP_STAGES + k]; if (r->key[k]) all[na++] = r->key[k]; }
    }
    // distinct keys, then each key's records (CSR); a record naming one key twice (VS = FS never happens, but) is listed once per key
    qsort(all, na, sizeof *all, cmp_u64);
    uint32_t nk = 0;
    for (size_t i = 0; i < na; i++) if (!nk || all[i] != all[nk - 1]) all[nk++] = all[i];
    s->kcap = 16; while (s->kcap < 2 * nk + 2) s->kcap <<= 1;
    s->kent = (nrp_kent *)calloc(nk ? nk : 1, sizeof *s->kent);
    s->kidx = (uint32_t *)calloc(s->kcap, sizeof *s->kidx);
    if (!s->kent || !s->kidx) { free(all); nrp_free(s); return -1; }
    s->nkent = nk;
    for (uint32_t i = 0; i < nk; i++) {
        s->kent[i].key = all[i];
        uint32_t h = mix32(all[i]) & (s->kcap - 1);
        while (s->kidx[h]) h = (h + 1) & (s->kcap - 1);
        s->kidx[h] = i + 1;
    }
    free(all);
    size_t total = 0;
    for (uint32_t i = 0; i < nrec; i++) {
        const nrp_rec *r = &s->rec[i];
        for (int k = 0; k < r->nkey; k++) {
            if (!r->key[k]) continue;
            int dup = 0; for (int j = 0; j < k; j++) if (r->key[j] == r->key[k]) dup = 1;
            if (!dup) { s->kent[kfind(s, r->key[k])].n++; total++; }
        }
    }
    s->krec = (uint32_t *)calloc(total ? total : 1, sizeof *s->krec);
    if (!s->krec) { nrp_free(s); return -1; }
    uint32_t at = 0;
    for (uint32_t i = 0; i < nk; i++) { s->kent[i].first = at; at += s->kent[i].n; s->kent[i].n = 0; }
    for (uint32_t i = 0; i < nrec; i++) {
        const nrp_rec *r = &s->rec[i];
        for (int k = 0; k < r->nkey; k++) {
            if (!r->key[k]) continue;
            int dup = 0; for (int j = 0; j < k; j++) if (r->key[j] == r->key[k]) dup = 1;
            if (dup) continue;
            nrp_kent *e = &s->kent[kfind(s, r->key[k])];
            s->krec[e->first + e->n++] = i;
        }
    }
    return 0;
}
void nrp_free(nrp_sched *s) {
    free(s->rec); free(s->q); free(s->kent); free(s->krec); free(s->kidx);
    memset(s, 0, sizeof *s);
}

static int rec_hinted(const nrp_sched *s, const nrp_rec *r) {   // every stage named by a hint
    if (!r->nkey) return 0;
    for (int k = 0; k < r->nkey; k++) { const int e = kfind(s, r->key[k]); if (e < 0 || !s->kent[e].hinted) return 0; }
    return 1;
}
uint32_t nrp_hint(nrp_sched *s, uint64_t key, uint64_t now) {
    s->st.hints++;
    const int e = kfind(s, key);
    if (e < 0) { s->st.hint_keys_new++; return 0; }
    nrp_kent *k = &s->kent[e];
    if (k->hinted) { s->st.hint_dup++; return 0; }
    k->hinted = 1; s->st.hint_keys_known++;
    uint32_t q = 0;
    for (uint32_t j = 0; j < k->n; j++) {
        const uint32_t i = s->krec[k->first + j];
        nrp_rec *r = &s->rec[i];
        if (r->t_hint || !rec_hinted(s, r)) continue;
        r->t_hint = now ? now : 1;
        if (s->shadow || r->phase != NRP_FREE || r->queued) continue;
        r->queued = 1; s->q[s->qtail++] = i; q++;
    }
    s->st.queued += q;
    return q;
}
int nrp_next(nrp_sched *s, int *from_hint) {
    *from_hint = 0;
    while (s->qhead < s->qtail) {
        const uint32_t i = s->q[s->qhead++];
        nrp_rec *r = &s->rec[i];
        if (r->phase != NRP_FREE) continue;
        r->phase = NRP_BUILDING; r->from_hint = 1; s->left--; *from_hint = 1;
        return (int)i;
    }
    if (s->lazy) return -1;
    while (s->cursor < s->nrec) {
        const uint32_t i = s->cursor++;
        nrp_rec *r = &s->rec[i];
        if (r->phase != NRP_FREE) { if (r->phase == NRP_CLAIMED) s->st.dup_avoided++; continue; }
        r->phase = NRP_BUILDING; r->from_hint = 0; s->left--;
        return (int)i;
    }
    return -1;
}
void nrp_done(nrp_sched *s, uint32_t i, int ok, uint64_t now) {
    if (i >= s->nrec) return;
    nrp_rec *r = &s->rec[i];
    if (r->phase != NRP_BUILDING) return;
    r->phase = ok ? NRP_READY : NRP_FAILED; r->t_ready = now ? now : 1;
    if (!ok) s->st.failed++;
    else if (r->from_hint) s->st.built_hint++;
    else s->st.built_list++;
}
int nrp_lead_bucket(uint64_t ns) {
    static const uint64_t lim[NRP_LEAD_BUCKETS - 1] = {50000000ull, 250000000ull, 1000000000ull, 5000000000ull, 30000000000ull};
    int b = 0; while (b < NRP_LEAD_BUCKETS - 1 && ns >= lim[b]) b++;
    return b;
}
const char *nrp_lead_name(int b) {
    static const char *n[NRP_LEAD_BUCKETS] = {"<50ms", "<250ms", "<1s", "<5s", "<30s", ">=30s"};
    return b >= 0 && b < NRP_LEAD_BUCKETS ? n[b] : "?";
}
const char *nrp_state_name(int st) {
    static const char *n[NRP_STATES] = {"free", "building", "ready", "failed", "claimed", "taken"};
    return st >= 0 && st < NRP_STATES ? n[st] : "?";
}
int nrp_need(nrp_sched *s, uint32_t i, uint64_t now, int claim) {
    if (i >= s->nrec) return -1;
    nrp_rec *r = &s->rec[i];
    const int was = r->phase;
    if (!r->needed) {
        r->needed = 1; r->t_need = now ? now : 1; r->phase_at_need = (uint8_t)was; r->inflight_at_need = was == NRP_BUILDING;
        s->st.need++;
        switch (was) {
        case NRP_READY: s->st.need_ready++; break;   // a race: built after the ready-set lookup
        case NRP_BUILDING: s->st.need_inflight++; break;
        case NRP_FREE: s->st.need_free++; break;
        case NRP_FAILED: s->st.need_failed++; break;
        default: s->st.need_claimed++; break;
        }
        if (r->t_hint) { s->st.need_hinted++; s->st.lead[nrp_lead_bucket(r->t_need > r->t_hint ? r->t_need - r->t_hint : 0)]++; }
        else s->st.need_unhinted++;
    }
    if (claim && was == NRP_FREE) { r->phase = NRP_CLAIMED; s->left--; s->st.claimed++; }
    return was;
}
void nrp_wait_end(nrp_sched *s, int ready) { s->st.waited++; if (ready) s->st.wait_ready++; else s->st.wait_timeout++; }
void nrp_took(nrp_sched *s, uint32_t i, uint64_t now) {
    if (i >= s->nrec) return;
    nrp_rec *r = &s->rec[i];
    if (r->phase != NRP_READY) return;
    r->phase = NRP_TAKEN; s->st.taken++;
    if (!r->needed) { r->needed = 1; r->t_need = now ? now : 1; }
    if (r->t_hint) { s->st.took_hinted++; s->st.took_lead[nrp_lead_bucket(r->t_need > r->t_hint ? r->t_need - r->t_hint : 0)]++; }
    else s->st.took_unhinted++;
}
uint32_t nrp_count(const nrp_sched *s, int phase) { uint32_t n = 0; for (uint32_t i = 0; i < s->nrec; i++) n += s->rec[i].phase == phase; return n; }

// ---- the direct table ----
int nrp_tab_init(nrp_tab *t) {
    memset(t, 0, sizeof *t);
    t->head = (uint32_t *)calloc(NRP_TAB_ROWS, sizeof *t->head);
    t->e = (nrp_tent *)calloc(NRP_TAB_MAX, sizeof *t->e);
    if (!t->head || !t->e) { nrp_tab_free(t); return -1; }
    return 0;
}
void nrp_tab_free(nrp_tab *t) { free(t->head); free(t->e); memset(t, 0, sizeof *t); }
void nrp_tab_clear(nrp_tab *t) { if (t->n) { memset(t->head, 0, NRP_TAB_ROWS * sizeof *t->head); t->n = 0; } t->clears++; }
static int var_eq(const nrp_var *a, const nrp_var *b) {
    uint64_t d = 0;
    for (int i = 0; i < NRP_VAR_WORDS; i++) d |= a->w[i] ^ b->w[i];
    return d == 0;
}
const nrp_tent *nrp_tab_find(nrp_tab *t, uint32_t key, const nrp_var *v) {
    if (key >= NRP_TAB_ROWS) { t->misses++; return NULL; }
    uint32_t at = t->head[key], prev = 0;
    while (at) {
        nrp_tent *e = &t->e[at - 1];
        if (var_eq(&e->v, v)) {
            if (!e->pipe) break;
            if (prev) { t->e[prev - 1].next = e->next; e->next = t->head[key]; t->head[key] = at; } else t->first_hits++;   // move to front
            t->hits++;
            return e;
        }
        prev = at; at = e->next;
    }
    t->misses++;
    return NULL;
}
nrp_tent *nrp_tab_put(nrp_tab *t, uint32_t key, const nrp_var *v) {
    if (key >= NRP_TAB_ROWS) return NULL;
    for (uint32_t at = t->head[key]; at; at = t->e[at - 1].next) if (var_eq(&t->e[at - 1].v, v)) return &t->e[at - 1];
    if (t->n >= NRP_TAB_MAX) { nrp_tab_clear(t); t->full++; }
    nrp_tent *e = &t->e[t->n++];
    memset(e, 0, sizeof *e); e->v = *v; e->key = key; e->zero = -1;
    e->next = t->head[key]; t->head[key] = t->n;
    return e;
}
void nrp_tab_kill(nrp_tab *t, uint64_t pipe) {
    if (!pipe) return;
    for (uint32_t i = 0; i < t->n; i++) if (t->e[i].pipe == pipe) { t->e[i].pipe = 0; t->killed++; }
}
