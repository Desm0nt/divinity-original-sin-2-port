// [DOS2] NR-NATIVE step D half c (DM_NRN_PKT, docs/dos2/nr/NR-NATIVE-D.md section 13): host test of dm/gfx/nr/nr_pkt.h.
//   1 the packet models: the journal (what DM_NRN_PKT=2 compares on the phone) lists exactly the bytes the applying model stores -
//     random renderers, descriptions, texture records, shadow blocks; a packet the journal cannot stand for says so.
//   2 one packet instead of the group: a model of the engine's list (AllocCommand 0x941720: alignment byte, chunks, the chunk's
//     packet count) and of its walker (0xa49c30: count packets a chunk, advance by the returned length) builds the same frame twice
//     - plain, and with every eligible group merged into its note - and executes both: the handler calls (opcode, packet bytes, in
//     order) must be the same, the walker must end exactly at each chunk's end, a group that crosses chunks stays unmerged.
//   --oracle <cases> <expect> <GA>: the models against the machine code's results written by tests/nr_pkt_oracle.py (every byte of
//     the arena afterwards, the returned length).
// Mutations (-DNRPK_MUTATE=k): 1..8 the models (nr_pkt.h lists them) - the ORACLE must fail (this file compares the model with
// its own journal only); 9 a group that crosses chunks is merged (both guards off), 10 the alignment byte is not honoured, 11 the
// chunk's count is not lowered, 12 the merged packet's length is short - this test must fail (a crash of the walker counts).
// Build: clang -std=c11 -O2 -Wall -Wextra nr_pkt_test.c -o nr_pkt_test
#define _CRT_SECURE_NO_WARNINGS 1
#include <stdio.h>
#include <stdlib.h>
static unsigned char *g_bias_host; static unsigned long long g_bias_guest;   // --oracle: guest address -> the arena
#define NRPK_PTR(a) ((uint8_t *)(g_bias_host ? g_bias_host + ((a) - g_bias_guest) : (unsigned char *)(uintptr_t)(a)))
#include "../../../nr/nr_pkt.h"
static unsigned long long g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { if (g_fail++ < 12) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static uint64_t g_seed = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17; return g_seed; }
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }

// ---- 1: journal = stores ----
enum { AR = 0x10000, A_R = 0, A_PKT = 0x3000, A_DESC = 0x4000, A_RECP = 0x4100, A_TREC = 0x4200, A_PAGES = 0x5000, A_PAGE0 = 0x5100, A_BLK = 0x6000, A_DATA = 0x7000 };
static void w64(uint8_t *a, uint32_t o, uint64_t v) { memcpy(a + o, &v, 8); }
static void w32(uint8_t *a, uint32_t o, uint32_t v) { memcpy(a + o, &v, 4); }
static void models(void) {
    static uint8_t a[AR], b[AR], c[AR];
    static nrpk_journal j;
    unsigned long long over = 0, stores = 0;
    for (int n = 0; n < 60000; n++) {
        for (int i = 0; i < AR; i += 8) { const uint64_t v = rnd(); memcpy(a + i, &v, 8); }
        const uint64_t B = (uint64_t)(uintptr_t)a;
        const int tex = rn(2);
        uint32_t gdef = 0;
        a[A_PKT] = tex ? NRPK_OP_TEX : NRPK_OP_PARAM;
        for (int st = 0; st < 3; st++) for (int i = 0; i < 32; i++) { w32(a, k_nrpk_tex[st] + 4 * i, rn(6) - 1); w32(a, k_nrpk_smp[st] + 4 * i, rn(6) - 1); }
        if (tex) {
            const uint32_t per = (uint32_t[]){0, 1, 3, 16}[rn(4)]; gdef = rn((per ? per : 1) * 4);
            w32(a, NRPK_R_SMP_PER, per); w64(a, NRPK_R_SMP_PAGES, B + A_PAGES);
            for (int p = 0; p < 4; p++) { w64(a, A_PAGES + 8 * p, B + A_PAGE0 + 0x200 * p); for (int e = 0; e < 16; e++) w32(a, A_PAGE0 + 0x200 * p + 0x20 * e + 0x10, rn(5)); }
            w64(a, A_PKT + 8, rn(12) ? B + A_DESC : 0); w64(a, A_PKT + 0x10, rn(12) ? B + A_RECP : 0); w32(a, A_PKT + 0x18, rn(3) ? 0xffffffffu : rn(4));
            for (int k = 0; k < 3; k++) a[A_DESC + k] = (uint8_t)(rn(5) ? rn(rn(20) ? 32 : 120) : 0xff);
            w32(a, A_DESC + 4, rn(2) ? rn(5) : 0xffffffffu); w64(a, A_RECP, B + A_TREC); w32(a, A_TREC + 0xc0, rn(5));
            for (int v = 0; v < 4; v++) w32(a, A_TREC + 0xc4 + 4 * v, rn(2) ? 0xffffffffu : rn(5));
            w32(a, A_TREC + 0x100, rn(2) ? rn(5) : 0xfffffff9u);
        } else {
            const uint32_t len = (uint32_t[]){0, 1, 4, 16, 33, 64, 200, 600}[rn(8)];
            w32(a, A_PKT + 0x28, len);
            const int alias = !rn(12);
            for (int k = 0; k < 3; k++) {
                const uint32_t blk = A_BLK + 0x100 * (alias && k == 2 ? 0 : k), off = rn(6) ? rn(0x400) : 0xffff;
                w64(a, A_PKT + 8 + 8 * k, rn(5) ? B + blk : 0); a[A_PKT + 0x20 + 2 * k] = (uint8_t)off; a[A_PKT + 0x21 + 2 * k] = (uint8_t)(off >> 8);
                w64(a, A_BLK + 0x100 * k + 0x20, B + A_DATA + 0x1000 * k); w32(a, A_BLK + 0x100 * k + 0x28, rn(0x800)); a[A_BLK + 0x100 * k + 0x34] = (uint8_t)rn(2);
                if (off != 0xffff && rn(3)) { memcpy(a + A_DATA + 0x1000 * k + off, a + A_PKT + 0x30, len); if (len && rn(2)) a[A_DATA + 0x1000 * k + off + rn(len)] ^= 1; }
            }
        }
        memcpy(b, a, AR);
        j.n = j.used = j.over = 0;
        const uint32_t l1 = nrpk_exec(&j, B + A_R, B + A_PKT, gdef);
        CHECK(!memcmp(a, b, AR), "the journal mode stored into memory");
        memcpy(c, a, AR);
        const uint32_t l2 = nrpk_exec(NULL, B + A_R, B + A_PKT, gdef);
        CHECK(l1 == l2 && l1 == nrnd_pkt_len(a + A_PKT), "length %u / %u / table %u", l1, l2, nrnd_pkt_len(a + A_PKT));
        if (j.over) { over++; continue; }
        CHECK(nrpk_journal_holds(&j, NULL), "packet %#x: a byte the journal lists is not what the model stored", a[A_PKT]);
        for (uint32_t i = 0; i < j.n; i++) memcpy(c + (j.e[i].addr - B), j.b + j.e[i].off, j.e[i].n);   // the journal applied to the state before
        CHECK(!memcmp(c, a, AR), "packet %#x: the model stored a byte the journal does not list", a[A_PKT]);
        stores += j.n;
    }
    CHECK(over > 100 && over < 12000 && stores > 60000, "coverage: %llu packets the journal cannot stand for, %llu stores", over, stores);
}

// ---- 2: the list, its walker, the merged group ----
typedef struct chunk { uint8_t *data; uint32_t cap, used, count; struct chunk *next; } chunk;
typedef struct { chunk *first, *cur; } list_t;
static chunk *chunk_new(uint32_t cap) { chunk *c = (chunk *)calloc(1, sizeof *c); { uint8_t *m = (uint8_t *)malloc(cap + 0x40); c->data = m + (0x40 - (uintptr_t)m % 0x40); } for (uint32_t i = 0; i < cap; i++) c->data[i] = (uint8_t)rnd(); c->cap = cap; return c; }
static uint8_t *alloc_cmd(list_t *l, uint32_t size, uint32_t align) {   // = 0x941720
    for (;;) {
        chunk *c = l->cur;
        const uint32_t r = c->used % align, p = c->used + (r ? align - r : align);
        if (p + size <= c->cap) { c->data[c->used] = (uint8_t)align; c->used = p + size; c->count++; return c->data + p; }
        chunk *n = chunk_new(c->cap); n->next = c->next; c->next = n; l->cur = n;
    }
}
typedef struct { unsigned op; uint32_t len; uint64_t sum; } call_t;
static call_t *g_calls; static size_t g_ncall, g_capcall; static int g_walk_bad, g_group_bad;
static uint64_t handler(void *u, unsigned op, uint64_t pkt);
static uint64_t handler(void *u, unsigned op, uint64_t pkt) {   // = the hook around 0x93e5f0: the note first, every other packet "executed"
    const uint8_t *p = (const uint8_t *)(uintptr_t)pkt;
    if (op == NRND_OP) {
        nrnd_note n; memcpy(&n, p, sizeof n);
        if (!(n.flags & NRND_F_ONE)) return NRND_NOTE_BYTES;
        const uint64_t len = nrpk_group_run(pkt, n.npkt, handler, u, &g_group_bad);
        return NRPK_MUTATE == 12 ? len - 2 : len;
    }
    uint32_t len = nrnd_pkt_len(p);
    if (!len && op == NRND_PK_PARAM) { memcpy(&len, p + 0x28, 4); len += 0x30; }   // the engine's handler returns the length whatever it is; only the scan gives up
    if (!len) { g_walk_bad = 1; return 0; }
    if (g_ncall == g_capcall) { g_capcall = g_capcall ? g_capcall * 2 : 1 << 16; g_calls = (call_t *)realloc(g_calls, g_capcall * sizeof *g_calls); }
    uint64_t s = 0; for (uint32_t i = 0; i < len; i++) s = s * 131 + p[i];
    g_calls[g_ncall++] = (call_t){op, len, s};
    return len;
}
static void walk(list_t *l) {   // = 0xa49c30
    for (chunk *c = l->first; c; c = c->next) {
        uint64_t cur = (uint64_t)(uintptr_t)c->data; const uint64_t end = cur + c->used;
        for (uint32_t k = 0; k < c->count; k++) {
            if (cur >= end) { g_walk_bad = 1; return; }
            const uint64_t a = *(const uint8_t *)(uintptr_t)cur;
            if (!a) { g_walk_bad = 1; return; }
            const uint64_t r = cur % a, p = cur + (r ? a - r : a);
            if (p >= end) { g_walk_bad = 1; return; }
            const uint64_t len = handler(NULL, *(const uint8_t *)(uintptr_t)p, p);
            if (!len) { g_walk_bad = 1; return; }
            cur = p + len;
        }
        if (cur != end) g_walk_bad = 1;   // the walker must have consumed the chunk exactly
    }
}
static unsigned long long g_merged, g_crossed, g_groups;
static void put_pkt(list_t *l, unsigned op, uint32_t n28) {
    const uint32_t len = op == NRND_PK_PARAM ? 0x30 + n28 : k_nrnd_len[op];
    uint8_t *p = alloc_cmd(l, len, (uint32_t[]){1, 4, 8, 16}[rn(4)]);
    for (uint32_t i = 0; i < len; i++) p[i] = (uint8_t)rnd();
    p[0] = (uint8_t)op; if (op == NRND_PK_PARAM) memcpy(p + 0x28, &n28, 4);
}
static void build(list_t *l, int merge, int groups) {
    l->first = l->cur = chunk_new(1 << 18);
    for (int g = 0; g < groups; g++) {
        for (unsigned k = rn(3); k; k--) put_pkt(l, (unsigned[]){0x19, 0x0d, 0x1b, 0x36}[rn(4)], 0);   // packets outside any group
        uint8_t *np = alloc_cmd(l, NRND_NOTE_BYTES, NRND_NOTE_ALIGN);
        chunk *c0 = l->cur; const uint32_t at = c0->used, count_note = c0->count;
        nrnd_note n; nrnd_note_init(&n, 0, 2, 0, 0, 0, 0); memcpy(np, &n, sizeof n);
        const int unknown = !rn(40);
        if (rn(4)) put_pkt(l, 0x3a, 0);
        put_pkt(l, NRND_PK_STATE, 0);
        for (unsigned k = rn(6); k; k--) put_pkt(l, NRND_PK_PARAM, (uint32_t[]){16, 64, 192, 600}[rn(4)]);
        for (unsigned k = rn(4); k; k--) put_pkt(l, rn(3) ? NRND_PK_TEX : NRND_PK_SAMP, 0);
        if (unknown) put_pkt(l, NRND_PK_PARAM, 0x20000);   // a parameter packet longer than the scan accepts: the group is not whole
        put_pkt(l, rn(5) ? NRND_PK_DRAW : NRND_PK_DRAW_INST, 0);
        // nrnd_end: the segments from the byte after the note to the cursor
        nrnd_seg seg[8]; unsigned ns = 0;
        for (chunk *c = c0;; c = c->next) { seg[ns].base = c->data; seg[ns].at = c == c0 ? at : 0; seg[ns].end = c->used; ns++; if (c == l->cur) break; }
        memcpy(&n, np, sizeof n); nrnd_scan(&n, seg, ns);
        g_groups++;
        if (ns > 1) g_crossed++;
        if (merge && nrpk_merge_ok(n.flags & NRND_F_FULL, ns, n.npkt, count_note, ns > 1 && NRPK_MUTATE == 9 ? count_note + n.npkt : c0->count)) {
            n.flags |= NRND_F_ONE; if (NRPK_MUTATE != 11) c0->count -= n.npkt; g_merged++;
        }
        memcpy(np, &n, sizeof n);
    }
}
static void lists(void) {
    for (int round = 0; round < 40; round++) {
        const uint64_t seed = g_seed;
        list_t a, b;
        build(&a, 0, 1500); const uint64_t after = g_seed;
        g_ncall = 0; g_walk_bad = 0; walk(&a);
        CHECK(!g_walk_bad, "the plain list did not walk");
        const size_t na = g_ncall; call_t *ca = (call_t *)malloc(na * sizeof *ca); memcpy(ca, g_calls, na * sizeof *ca);
        g_seed = seed; build(&b, 1, 1500);
        CHECK(g_seed == after, "the two builds diverged");
        g_ncall = 0; g_walk_bad = 0; g_group_bad = 0; walk(&b);
        CHECK(!g_walk_bad && !g_group_bad, "the merged list did not walk to the end of every chunk");
        CHECK(g_ncall == na && !memcmp(ca, g_calls, na * sizeof *ca), "the merged list executed other packets (%zu / %zu)", g_ncall, na);
        uint32_t cnt_a = 0, cnt_b = 0; for (chunk *c = a.first; c; c = c->next) cnt_a += c->count; for (chunk *c = b.first; c; c = c->next) cnt_b += c->count;
        CHECK(cnt_b < cnt_a, "nothing merged");
        free(ca);
    }
    CHECK(g_merged > 20000 && g_crossed > 200, "coverage: %llu merged of %llu groups, %llu crossed a chunk", g_merged, g_groups, g_crossed);
}

static int oracle(const char *fc, const char *fe, const char *ga) {
    FILE *c = fopen(fc, "rb"), *e = fopen(fe, "rb");
    if (!c || !e) { printf("FAIL: cannot open the case files\n"); return 1; }
    uint32_t n = 0; if (fread(&n, 4, 1, c) != 1) return 1;
    static uint8_t a[AR], x[AR];
    g_bias_guest = strtoull(ga, NULL, 0); g_bias_host = a;
    unsigned long long bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t h[4]; uint64_t want;
        if (fread(h, 4, 4, c) != 4 || fread(a, 1, AR, c) != AR || fread(&want, 8, 1, e) != 1 || fread(x, 1, AR, e) != AR) { printf("FAIL: short case file\n"); return 1; }
        const uint32_t len = nrpk_exec(NULL, g_bias_guest + A_R, g_bias_guest + h[1], h[2]);
        if (len != want || memcmp(a, x, AR)) {
            if (bad++ < 8) { uint32_t at = 0; while (at < AR && a[at] == x[at]) at++; printf("case %u (opcode %#x): length %u / %llu, first different byte at arena+%#x (model %02x, machine code %02x)\n", i, h[0], len, (unsigned long long)want, at, at < AR ? a[at] : 0, at < AR ? x[at] : 0); }
        }
    }
    printf("%s: %u cases against the machine code, %llu differ (mutation %d)\n", bad ? "FAIL" : "PASS", n, bad, NRPK_MUTATE);
    return bad ? 1 : 0;
}
int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[1], "--oracle")) return oracle(argv[2], argv[3], argv[4]);
    models();
    lists();
    printf("%s: %llu checks, %llu failed; %llu groups merged of %llu, %llu crossed a chunk (mutation %d)\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail, g_merged, g_groups, g_crossed, NRPK_MUTATE);
    return g_fail ? 1 : 0;
}
