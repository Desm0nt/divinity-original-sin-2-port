// [DOS2] NR-08b host test: dm/gfx/nr/nr_ubodyn.h (DM_NR_UBODYN) - which uniform bindings a SPIR-V module reads (the PowerVR rule:
// a dynamic uniform buffer the stage does not read overruns the driver's table, PPSSPP 73559d15aa), on hand-made modules, and with a
// bundle directory on every DOS2 module + every vertex + fragment pair: what the rule makes dynamic, and what NR-08 (nr_dplan_build)
// then has to rewrite per draw on the PowerVR class with DM_NR_UBODYN=0 (today) and =1.
// Build (Windows LLVM or any C11 compiler), from the repository root:
//   clang -std=c11 -O2 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS dm/gfx/vulkan/engine/tests/nr_ubodyn_test.c dm/gfx/nr/nr_ubodyn.c dm/gfx/nr/nr_desc.c -o nr_ubodyn_test
//   ./nr_ubodyn_test [<work/shaders-vk> <work/shaders-jobs/programs.tsv>]
#include "../../../nr/nr_ubodyn.h"
#include "../../../nr/nr_desc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails = 0;
static long checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { if (++fails < 40) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static double now_ns(void) { struct timespec t; timespec_get(&t, TIME_UTC); return t.tv_sec * 1e9 + t.tv_nsec; }

// ---------------- a tiny SPIR-V writer (structure only: what the scanner looks at) ----------------
typedef struct { uint32_t w[512]; unsigned n; } mod;
static void hdr(mod *m, uint32_t bound) { m->n = 0; m->w[m->n++] = 0x07230203u; m->w[m->n++] = 0x00010300u; m->w[m->n++] = 0; m->w[m->n++] = bound; m->w[m->n++] = 0; }
static void ins(mod *m, uint32_t op, unsigned nargs, const uint32_t *a) { m->w[m->n++] = ((nargs + 1) << 16) | op; for (unsigned i = 0; i < nargs; i++) m->w[m->n++] = a[i]; }
#define I(m, op, ...) do { const uint32_t a_[] = {__VA_ARGS__}; ins((m), (op), sizeof a_ / sizeof a_[0], a_); } while (0)
enum { ENTRY = 15, NAME_LIT = 0x6e69616d, DECORATE = 71, VARIABLE = 59, FUNCTION = 54, LABEL = 248, RETURN = 253, FEND = 56, LOAD = 61, CHAIN = 65,
       FADD = 129, STORE = 62, CALL = 57, PARAM = 55, COPYMEM = 63, COPYOBJ = 83, ARRLEN = 68 };
// ids: 1 void, 2 fn type, 3 float, 4 ptr type, 5 main, 10.. variables, 30.. values, 60 helper function
static void prologue(mod *m, unsigned entries) {
    hdr(m, 100);
    for (unsigned e = 0; e < entries; e++) I(m, ENTRY, 0, 5 + e * 55, NAME_LIT, 0);   // second entry = id 60
    // bindings: var 10 -> 4, 11 -> 10, 12 -> 16, 13 -> 22, 14 -> 28 (14 also gets a second decoration: set)
    I(m, DECORATE, 10, 33, 4); I(m, DECORATE, 11, 33, 10); I(m, DECORATE, 12, 33, 16); I(m, DECORATE, 13, 33, 22); I(m, DECORATE, 14, 34, 0); I(m, DECORATE, 14, 33, 28);
    for (uint32_t v = 10; v <= 14; v++) I(m, VARIABLE, 4, v, 2);
    I(m, VARIABLE, 4, 15, 3);   // an output, no binding
}
static void units(void) {
    static const uint32_t binds[6] = {4, 10, 16, 22, 28, 99};
    uint8_t r[6]; mod m;
    // one function: 10 loaded and used; 11 loaded, value unused (dead); 12 through an access chain, used; 13 declared only;
    // 14 used only as a literal-looking word (a FAdd operand equal to its id 14 would be a VALUE id, not a pointer: never a read)
    prologue(&m, 1);
    I(&m, FUNCTION, 1, 5, 0, 2); I(&m, LABEL, 40);
    I(&m, LOAD, 3, 30, 10); I(&m, LOAD, 3, 31, 11); I(&m, CHAIN, 4, 32, 12, 50); I(&m, LOAD, 3, 33, 32);
    I(&m, FADD, 3, 34, 30, 33); I(&m, FADD, 3, 35, 34, 14); I(&m, STORE, 15, 35);
    I(&m, RETURN); I(&m, FEND);
    CHECK(nr_spv_binding_reads(m.w, m.n, binds, 6, r) == NRU_OK);
    CHECK(r[0] == 1 && r[1] == 0 && r[2] == 1 && r[3] == 0 && r[4] == 0 && r[5] == 0);
    {   uint32_t d[8] = {0}; CHECK(nr_spv_declared_bindings(m.w, m.n, d, 8) == 5); CHECK(d[0] == 4 && d[1] == 10 && d[2] == 16 && d[3] == 22 && d[4] == 28);
        CHECK(nr_spv_declared_bindings(m.w, m.n, d, 2) == 5); }
    mod live = m;
    // the dead load of 11 becomes live (its value feeds a store): read
    prologue(&m, 1);
    I(&m, FUNCTION, 1, 5, 0, 2); I(&m, LABEL, 40); I(&m, LOAD, 3, 31, 11); I(&m, STORE, 15, 31); I(&m, RETURN); I(&m, FEND);
    CHECK(nr_spv_binding_reads(m.w, m.n, binds, 6, r) == NRU_OK && r[1] == 1 && r[0] == 0);
    // copies: OpCopyMemory from 13 (a read: it writes memory), OpCopyObject of 10's pointer then a used load = read of 4,
    // OpArrayLength of 12 with its result used = read
    prologue(&m, 1);
    I(&m, FUNCTION, 1, 5, 0, 2); I(&m, LABEL, 40); I(&m, COPYMEM, 15, 13); I(&m, COPYOBJ, 4, 36, 10); I(&m, LOAD, 3, 37, 36); I(&m, STORE, 15, 37);
    I(&m, ARRLEN, 3, 38, 12, 0); I(&m, STORE, 15, 38); I(&m, RETURN); I(&m, FEND);
    CHECK(nr_spv_binding_reads(m.w, m.n, binds, 6, r) == NRU_OK && r[0] == 1 && r[1] == 0 && r[2] == 1 && r[3] == 1 && r[4] == 0);
    // two functions: 10 read in the entry; 11 read only in the helper (not counted: the safe answer); 12 passed as a pointer argument
    prologue(&m, 1);
    I(&m, FUNCTION, 1, 60, 0, 2); I(&m, PARAM, 4, 61); I(&m, LABEL, 41); I(&m, LOAD, 3, 62, 11); I(&m, STORE, 15, 62); I(&m, LOAD, 3, 63, 61); I(&m, STORE, 15, 63); I(&m, RETURN); I(&m, FEND);
    I(&m, FUNCTION, 1, 5, 0, 2); I(&m, LABEL, 40); I(&m, LOAD, 3, 30, 10); I(&m, STORE, 15, 30); I(&m, CALL, 1, 64, 60, 12); I(&m, RETURN); I(&m, FEND);
    CHECK(nr_spv_binding_reads(m.w, m.n, binds, 6, r) == NRU_OK && r[0] == 1 && r[1] == 0 && r[2] == 1 && r[3] == 0);
    // not understood: two entry points, none, no entry function, truncated, bad magic / bound
    prologue(&m, 2); I(&m, FUNCTION, 1, 5, 0, 2); I(&m, LABEL, 40); I(&m, LOAD, 3, 30, 10); I(&m, STORE, 15, 30); I(&m, RETURN); I(&m, FEND);
    memset(r, 7, sizeof r); CHECK(nr_spv_binding_reads(m.w, m.n, binds, 6, r) == NRU_ENTRY_POINTS && !r[0] && !r[5]);
    prologue(&m, 0); I(&m, FUNCTION, 1, 5, 0, 2); I(&m, RETURN); I(&m, FEND); CHECK(nr_spv_binding_reads(m.w, m.n, binds, 6, r) == NRU_ENTRY_POINTS);
    prologue(&m, 1); I(&m, FUNCTION, 1, 6, 0, 2); I(&m, RETURN); I(&m, FEND); CHECK(nr_spv_binding_reads(m.w, m.n, binds, 6, r) == NRU_NO_ENTRY);
    m = live; m.w[m.n - 1] = (9u << 16) | FEND; CHECK(nr_spv_binding_reads(m.w, m.n, binds, 6, r) == NRU_TRUNCATED && !r[0]);
    m = live; m.w[m.n - 2] = 0; CHECK(nr_spv_binding_reads(m.w, m.n, binds, 6, r) == NRU_TRUNCATED);   // word count 0
    m = live; m.w[0] = 0x12345678u; CHECK(nr_spv_binding_reads(m.w, m.n, binds, 6, r) == NRU_BAD_HEADER && nr_spv_declared_bindings(m.w, m.n, NULL, 0) == -1);
    m = live; m.w[3] = 0; CHECK(nr_spv_binding_reads(m.w, m.n, binds, 6, r) == NRU_BAD_HEADER);
    m = live; m.w[3] = 1u << 23; CHECK(nr_spv_binding_reads(m.w, m.n, binds, 6, r) == NRU_BAD_HEADER);
    CHECK(nr_spv_binding_reads(NULL, 0, binds, 6, r) == NRU_BAD_HEADER);
    CHECK(nr_spv_binding_reads(live.w, live.n, NULL, 0, NULL) == NRU_OK);
    // ids past the bound are ignored, not a crash
    prologue(&m, 1); I(&m, FUNCTION, 1, 5, 0, 2); I(&m, LOAD, 3, 300, 10); I(&m, LOAD, 3, 30, 400); I(&m, CHAIN, 4, 500, 10); I(&m, STORE, 15, 300); I(&m, RETURN); I(&m, FEND);
    CHECK(nr_spv_binding_reads(m.w, m.n, binds, 6, r) == NRU_OK && r[0] == 0);
    // the optimizer may only add reads
    {   mod a, b; prologue(&a, 1); I(&a, FUNCTION, 1, 5, 0, 2); I(&a, LOAD, 3, 30, 10); I(&a, STORE, 15, 30); I(&a, LOAD, 3, 31, 11); I(&a, STORE, 15, 31); I(&a, RETURN); I(&a, FEND);
        prologue(&b, 1); I(&b, FUNCTION, 1, 5, 0, 2); I(&b, LOAD, 3, 30, 10); I(&b, STORE, 15, 30); I(&b, LOAD, 3, 31, 11); I(&b, RETURN); I(&b, FEND);   // 11's load is dead now
        CHECK(nr_spv_reads_kept(a.w, a.n, a.w, a.n) == 1);
        CHECK(nr_spv_reads_kept(a.w, a.n, b.w, b.n) == 0);
        CHECK(nr_spv_reads_kept(b.w, b.n, a.w, a.n) == 1);
        mod bad = a; bad.w[0] = 1; CHECK(nr_spv_reads_kept(a.w, a.n, bad.w, bad.n) == 0); CHECK(nr_spv_reads_kept(bad.w, bad.n, a.w, a.n) == 1);
        mod two = a; prologue(&two, 2); I(&two, FUNCTION, 1, 5, 0, 2); I(&two, RETURN); I(&two, FEND);
        CHECK(nr_spv_reads_kept(two.w, two.n, b.w, b.n) == 1);   // `before` not understood: nothing was made dynamic from it
    }
    // the rule for one binding
    for (uint32_t t = 0; t < 12; t++) for (uint32_t s = 0; s < 12; s++) for (int rd = 0; rd < 2; rd++)
        CHECK(nru_dynamic_ok(t, s, rd) == (t == 6 && (s == 2 || s == 5 || s == 6) && rd));
    for (int i = 0; i <= NRU_N; i++) CHECK(nru_why_name(i) != NULL);
    // cost: the scanner runs once per module load (and twice when the optimizer's copy is checked)
    {   const double t0 = now_ns(); unsigned k = 0; for (int i = 0; i < 20000; i++) k += nr_spv_binding_reads(live.w, live.n, binds, 6, r) == NRU_OK;
        printf("  scanner: %.0f ns on a %u-word module (%u)\n", (now_ns() - t0) / 20000, live.n, k); }
}

// ---------------- every DOS2 bundle ----------------
#ifdef _WIN32
#include <io.h>
static void list_dir(const char *dir, void (*fn)(const char *dir, const char *name)) {
    char pat[1024]; snprintf(pat, sizeof pat, "%s/*.xgs", dir);
    struct _finddata_t fd; intptr_t h = _findfirst(pat, &fd);
    if (h == -1) return;
    do fn(dir, fd.name); while (!_findnext(h, &fd));
    _findclose(h);
}
#else
#include <dirent.h>
static void list_dir(const char *dir, void (*fn)(const char *dir, const char *name)) {
    DIR *d = opendir(dir); if (!d) return;
    for (struct dirent *e; (e = readdir(d));) { const size_t n = strlen(e->d_name); if (n > 4 && !strcmp(e->d_name + n - 4, ".xgs")) fn(dir, e->d_name); }
    closedir(d);
}
#endif
typedef struct { char key[20]; uint32_t stage, n; nr_dbind *b; uint8_t *read; int why; } bundle;
static bundle *g_b; static unsigned g_nb;
static unsigned st_mod = 0, st_why[NRU_N], st_decl[8], st_unread[8], st_flags = 0, st_multi = 0; static double st_ns = 0;
static int cmp_bundle(const void *a, const void *b) { return strcmp(((const bundle *)a)->key, ((const bundle *)b)->key); }
static const bundle *find_bundle(const char *key) { bundle k; memset(&k, 0, sizeof k); snprintf(k.key, sizeof k.key, "%s", key); return (const bundle *)bsearch(&k, g_b, g_nb, sizeof *g_b, cmp_bundle); }
static void load_bundle(const char *dir, const char *name) {
    char path[1200]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *fp = fopen(path, "rb"); if (!fp) return;
    uint32_t h[8];
    if (fread(h, 4, 8, fp) == 8 && h[0] == 0x31534758 && h[1] == 1 && h[3] <= 256 && h[6] && !(h[6] & 3)) {
        bundle *b = &g_b[g_nb]; memset(b, 0, sizeof *b);
        snprintf(b->key, sizeof b->key, "%.*s", (int)(strlen(name) - 4 < 19 ? strlen(name) - 4 : 19), name);
        b->stage = h[2]; b->n = h[3]; b->b = (nr_dbind *)malloc(sizeof(nr_dbind) * (h[3] ? h[3] : 1)); b->read = (uint8_t *)calloc(h[3] ? h[3] : 1, 1);
        uint32_t *w = (uint32_t *)malloc(h[6]);
        if (b->b && b->read && w && fread(b->b, sizeof(nr_dbind), h[3], fp) == h[3] && !fseek(fp, (long)(h[4] * 4 + h[5]), SEEK_CUR) && fread(w, 1, h[6], fp) == h[6]) {
            uint32_t bn[256]; for (uint32_t i = 0; i < h[3]; i++) bn[i] = b->b[i].binding;
            const double t0 = now_ns(); b->why = nr_spv_binding_reads(w, h[6] / 4, bn, h[3], b->read); st_ns += now_ns() - t0;
            unsigned fns = 0; for (size_t i = 5; i < h[6] / 4;) { const uint32_t wc = w[i] >> 16; if (!wc) break; if ((w[i] & 0xffff) == 54) fns++; i += wc; }
            st_multi += fns > 1; st_mod++; st_why[b->why]++;
            for (uint32_t i = 0; i < h[3]; i++) if (b->b[i].type == 6) {
                const unsigned s = b->b[i].source < 8 ? b->b[i].source : 7; st_decl[s]++; if (!b->read[i]) st_unread[s]++;
                if (b->b[i].flags) st_flags++;
            }
            g_nb++;
        }
        free(w);
    }
    fclose(fp);
}
// the engine's dyn_mode_for + UBO_DYN on one stage table: `ro` = DM_NR_UBODYN=1 on a device whose policy turned dynamic buffers off
static unsigned dyn_mode(const nr_dstage st[NRD_STAGES], const uint8_t *const rd[NRD_STAGES], unsigned limit, int on, int ro, uint8_t *dyn[NRD_STAGES]) {
    unsigned all = 0, vary = 0, mode;
    for (unsigned s = 0; s < NRD_STAGES; s++) if (st[s].present) for (uint32_t i = 0; i < st[s].n; i++) {
        const nr_dbind *b = &st[s].b[i]; if (b->type != NRD_T_UBO) continue;
        all += b->count; if (b->source == NRD_S_UNIFORM || b->source == NRD_S_DRIVER0 || b->source == NRD_S_DRIVER2) vary += b->count;
    }
    if (!on && !ro) mode = 0;
    else { mode = all && all <= limit ? 1 : (vary && vary <= limit ? 2 : 0); if (ro && mode == 1) mode = 2; }
    for (unsigned s = 0; s < NRD_STAGES; s++) if (st[s].present) for (uint32_t i = 0; i < st[s].n; i++) {
        const nr_dbind *b = &st[s].b[i]; int v = 0;
        if (b->type == NRD_T_UBO) {
            if (mode == 1) v = 1; else if (mode == 2) v = b->source == NRD_S_UNIFORM || b->source == NRD_S_DRIVER0 || b->source == NRD_S_DRIVER2;
            if (ro && !nru_dynamic_ok(b->type, b->source, rd[s][i])) v = 0;
        }
        dyn[s][i] = (uint8_t)v;
    }
    return mode;
}
static void bundles(const char *dir, const char *programs) {
    g_b = (bundle *)calloc(20000, sizeof *g_b); if (!g_b) return;
    list_dir(dir, load_bundle);
    qsort(g_b, g_nb, sizeof *g_b, cmp_bundle);
    printf("  modules %u (%u with more than one function): understood %u, not:", st_mod, st_multi, st_why[NRU_OK]);
    for (int i = 1; i < NRU_N; i++) printf(" %s %u", nru_why_name(i), st_why[i]);
    printf("; scanner %.1f us a module\n", st_mod ? st_ns / st_mod / 1e3 : 0.);
    static const char *const src[8] = {"?", "support", "game block (ring)", "?", "c1 constants", "driver 0", "driver 2", "other"};
    for (int s = 0; s < 8; s++) if (st_decl[s]) printf("    uniform bindings, source %d %-18s declared %6u, not read %6u\n", s, src[s], st_decl[s], st_unread[s]);
    printf("    uniform bindings with flags set in the bundle: %u (NR-08b's XBG_BIND_UNREAD bit 29 must be free)\n", st_flags);
    CHECK(st_flags == 0);
    CHECK(st_unread[2] == 0);   // the measured fact the design leans on: every game block is read by its stage (if not, it stays static)
    FILE *fp = fopen(programs, "rb");
    if (!fp) { printf("  programs %s not found: pairs skipped\n", programs); return; }
    static const struct { const char *name; unsigned limit; int on, ro; } dev[4] = {
        {"PowerVR policy, DM_NR_UBODYN=0 (today)", 0xffffffffu, 0, 0}, {"PowerVR BXM (0xffffffff dynamic), DM_NR_UBODYN=1", 0xffffffffu, 0, 1},
        {"SIM powervr on Adreno (16), DM_NR_UBODYN=1", 16, 0, 1}, {"PowerVR with 8 dynamic, DM_NR_UBODYN=1", 8, 0, 1}};
    unsigned pairs = 0, missing = 0, elig[4] = {0}, nostat[4] = {0}, stat_h[4][NRD_MAX_UBO + 1], dynmax[4] = {0}, unsafe = 0, mode_n[4][4];
    memset(stat_h, 0, sizeof stat_h); memset(mode_n, 0, sizeof mode_n);
    char line[2048]; int first = 1;
    while (fgets(line, sizeof line, fp)) {
        if (first) { first = 0; continue; }
        char *col[6] = {0}; unsigned nc = 0; char *q = line;
        while (nc < 6) { col[nc++] = q; char *tab = strchr(q, '\t'); if (!tab) break; *tab = 0; q = tab + 1; }
        if (nc < 5 || !strcmp(col[1], "-") || !strcmp(col[3], "-")) continue;
        const bundle *vs = find_bundle(col[1]), *ps = find_bundle(col[3]);
        if (!vs || !ps) { missing++; continue; }
        pairs++;
        static uint8_t dv[256], df[256]; uint8_t *dyn[NRD_STAGES] = {dv, dv, dv, dv, df};
        const uint8_t *rd[NRD_STAGES] = {vs->read, vs->read, vs->read, vs->read, ps->read};
        nr_dstage st[NRD_STAGES]; memset(st, 0, sizeof st);
        st[0].b = vs->b; st[0].n = vs->n; st[0].ns = 0; st[0].present = 1; st[0].dyn = dv;
        st[4].b = ps->b; st[4].n = ps->n; st[4].ns = 1; st[4].present = 1; st[4].dyn = df;
        for (unsigned d = 0; d < 4; d++) {
            const unsigned mode = dyn_mode(st, rd, dev[d].limit, dev[d].on, dev[d].ro, dyn);
            mode_n[d][mode & 3]++;
            // the PowerVR rule itself: no dynamic uniform buffer a stage does not read
            for (unsigned s = 0; s < NRD_STAGES; s++) if (st[s].present) for (uint32_t i = 0; i < st[s].n; i++) if (dyn[s][i] && !rd[s][i]) unsafe++;
            nr_dplan p; if (nr_dplan_build(&p, st)) continue;
            elig[d]++; stat_h[d][p.nstat]++; if (!p.nstat) nostat[d]++; if (p.ndyn > dynmax[d]) dynmax[d] = p.ndyn;
        }
    }
    fclose(fp);
    CHECK(unsafe == 0);
    printf("  vertex + fragment pairs %u (%u without a bundle); dynamic uniform buffers a stage does not read: %u\n", pairs, missing, unsafe);
    for (unsigned d = 0; d < 4; d++) {
        printf("  %-52s modes 0/1/2: %u/%u/%u; NR-08 eligible %u: no static ring block (set written once) %u (%.1f%%), at most %u dynamic offsets; static ring words:",
               dev[d].name, mode_n[d][0], mode_n[d][1], mode_n[d][2], elig[d], nostat[d], 100. * nostat[d] / (elig[d] ? elig[d] : 1), dynmax[d]);
        for (unsigned k = 0; k <= NRD_MAX_UBO; k++) if (stat_h[d][k]) printf(" %u:%u", k, stat_h[d][k]);
        printf("\n");
    }
    CHECK(nostat[1] == elig[1] && elig[1] == elig[0]);   // with the rule every eligible pair keeps all its ring blocks dynamic
}

int main(int argc, char **argv) {
    units();
    if (argc >= 3) bundles(argv[1], argv[2]);
    printf("%s: %ld checks, %d failures\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
