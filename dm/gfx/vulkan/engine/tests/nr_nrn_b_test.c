// [DOS2] NR-NATIVE step D half b (DM_NRN_NOBODY, docs/dos2/nr/NR-NATIVE-D.md section 12): host test of the fold of a card draw's binds
// (dm/gfx/nvn/nvn_nrn_b.inc) against the bodies of the NVN entry points it stands for.
// The bodies are the engine's OWN TEXT: tools/dos2_nrnb_bodies.py cuts nvnCommandBufferBindUniformBuffer, nvnDeviceGetTextureHandle,
// bind_handles, nvn_nrd_bind, nvn_nrd_can, the tap's nbt_f / nbt_note / nbt_bind, the shadow's nbd_cb / nbd_take_bind and the two
// mirror hooks out of hle_nvn.c / nvn_nb_tap.inc / nvn_nb_draw.inc / nvn_nr_direct.inc into nr_nrn_b_bodies.gen.inc; this file gives
// them the few things they call (the record stream, the use marks, the tap's counters) and compiles the fold next to them.
//
// Every draw runs three times from ONE saved state: (A) the bodies, (B) nvn_nrnb_binds - the fold, or the bodies when it refuses,
// (C) the bodies inside nvn_nrnb_snap / nvn_nrnb_check, the verify mode of the phone. After A and B every word of state must be equal:
// the two mirrors, the tap's tables / dirty masks / signature / owner, the shadow of the draw and its command buffer, the left-out
// mask and the flag of a short draw, the counts of binds not sent, the texture-set generation, every counter, and the journal - the
// use marks in call order, the records written, the tap's notes, the shadow's flushes, the log lines. C must report no difference.
// Between draws: binds outside a bracket, a new recording, another thread's bind, the tap losing its command buffer, a bind left in
// the shadow (same and another command buffer), command memory running out, every switch combination.
// Mutations (-DNRNB_MUTATE=k, the test must FAIL): 1 a range the backend holds is sent, 2 the signature is not moved, 3 the dirty
// mask is not set, 4 the texture mirror is not asked, 5 a bind not sent is not counted, 6 the generation is not moved, 7 a short
// draw's bind goes into the shadow, 8 a use mark is lost, 9 the gate does not look at the shadow, 10 nor at the mirror's owner.
// Build: python tools/dos2_nrnb_bodies.py --out <dir>/nr_nrn_b_bodies.gen.inc
//        clang -std=gnu11 -O2 -Wall -Wextra -I<dir> nr_nrn_b_test.c -o nr_nrn_b_test -lpthread
#define DM_GAME_DOS2 1
#ifdef _WIN32   // the mirrors' locks are taken only off the paths compared here; one thread runs the test
typedef int pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER 0
#define pthread_mutex_lock(m) ((void)(m))
#define pthread_mutex_unlock(m) ((void)(m))
#else
#include <pthread.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "../../../nvn/nvn_nb_draw.h"
#include "../../../nvn/nvn_nb_tap.h"
#include "../../../nr/nr_direct.h"
#include "../../../nr/nr_ubd.h"
#include "../../../nr/nr_nrn.h"
#include "../../../nvn/nvn_nr_direct.h"
static unsigned long long g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { if (g_fail++ < 12) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)
static uint64_t g_seed = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17; return g_seed; }
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }

// ---- what the bodies call ----
typedef struct NVNcommandBuffer NVNcommandBuffer; typedef struct NVNdevice NVNdevice;
typedef uint64_t NVNtextureHandle, NVNbufferAddress; typedef int NVNshaderStage;
typedef struct { uint32_t magic, recording; uint8_t *cur, *end; } cmd_t;
#define CB(c) ((cmd_t *)(c))
#define MAGIC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define NVN(name, ret, ...) static ret h_##name(__VA_ARGS__)
enum { OP_BIND_TEX = 10, OP_BIND_SAMP, OP_BIND_IMG, OP_BIND_UBO, OP_X_NR_STATE, OP_RESERVE = 24 };
enum { NBT_STAGES = 6, NBT_SLOTS = 64, NBDS_CB = 1, NBDS_FULL = 2 };
#define WT_IDS (1u << 20)
typedef struct { int stage, slot; uint64_t a, b; } p_bind_buf;
typedef struct { int stage, first, n; uint64_t h[1]; } p_bind_handles;
// the journal: everything the bodies do that is not a word of the state below
static struct { uint64_t note[32], wt[256], flush, logs, offthread; int nwt; uint8_t stream[4096]; size_t len; } J;
#define LOG(...) do { J.logs++; } while (0)
int g_dm_nb; static nb_frame g_frame; static int g_render_thread = 1;
nb_frame *dm_nb_frame(void) { if (!g_render_thread) { J.offthread++; return NULL; } return &g_frame; }
void dm_nb_note(int what, unsigned n) { J.note[what & 31] += n; }
static struct { const void *cb; uint64_t handle; uint32_t tex[NBT_STAGES][NBT_SLOTS], img[NBT_STAGES][NBT_SLOTS]; uint64_t tdirty[NBT_STAGES], idirty[NBT_STAGES]; } g_nbt;
static __thread struct { int mode; } t_nd;
static int g_nc_cap, g_nru_cap, g_z5;
#define Z5G_ON() (g_z5)
#define Z5G_UBO(st, i, a) ((void)0)
#define NUB(c, st, i, a, n) ((void)0)
static void wt_use_ids(const uint64_t *h, int n) { for (int i = 0; i < n; i++) if (J.nwt < 256) J.wt[J.nwt++] = h[i]; }
static void *rec(NVNcommandBuffer *c, uint32_t op, size_t payload) {
    void *p = J.stream + J.len; (void)c;
    if (J.len + 8 + payload > sizeof J.stream) { J.len = 0; p = J.stream; }
    memcpy(p, &op, 4); J.len += 8 + ((payload + 7) & ~(size_t)7);
    return (uint8_t *)p + 8;
}
#define REC(c, OP, T) ((T *)rec((c), (OP), sizeof(T)))
#define RECN(c, OP, T, ARR, N) ((T *)rec((c), (OP), offsetof(T, ARR) + sizeof(((T *)0)->ARR[0]) * (size_t)(N)))
#define NRNP_RECORDING(c) (CB(c) && CB(c)->recording)
#define NVN_NRN_PART 1
#include "../../../nvn/nvn_nrn.inc"
#undef NVN_NRN_PART
#define NVN_NRNB_PART 1
#include "../../../nvn/nvn_nrn_b.inc"
#undef NVN_NRNB_PART
static int nbd_ren_pending(void) { return 0; }
static void nbd_flush(int why);
#include "nr_nrn_b_bodies.gen.inc"
static void nbd_flush(int why) { (void)why; J.flush += 1 + t_nbd.s.h.nbind; nbd_reset(); }   // the test only needs to see that it happened
#define NVN_NRN_PART 2
#include "../../../nvn/nvn_nrn.inc"
#undef NVN_NRN_PART
#define NVN_NRNB_PART 2
#include "../../../nvn/nvn_nrn_b.inc"
#undef NVN_NRNB_PART

// ---- one world: every word the bodies of a bind may touch ----
typedef struct {
    nrd_mirror texm; nrub_mirror ubm;
    uint32_t tex[NBT_STAGES][NBT_SLOTS], img[NBT_STAGES][NBT_SLOTS]; uint64_t tdirty[NBT_STAGES], idirty[NBT_STAGES]; const void *tapcb; uint64_t taph;
    nbd_shadow s; void *scb; int in, emitting, drew, chk, mirror;
    nrn_short_ext sx; int shrt; uint32_t nf, wf, texgen, tickn; uint64_t tabsig, tx[NVN_NRD_S_N], gx[NVN_NRD_S_N], ps[NRNPS_N];
    uint8_t *cur[2];
} world;
static cmd_t g_cmd[2]; static uint8_t g_mem[2][65536];
static world *g_w[4];
static void save(world *w) {
    memset(w, 0, sizeof *w);
    w->texm = g_nrdm; w->ubm = g_nrub_m; memcpy(w->tex, g_nbt.tex, sizeof w->tex); memcpy(w->img, g_nbt.img, sizeof w->img);
    memcpy(w->tdirty, g_nbt.tdirty, sizeof w->tdirty); memcpy(w->idirty, g_nbt.idirty, sizeof w->idirty); w->tapcb = g_nbt.cb; w->taph = g_nbt.handle;
    w->s.h = t_nbd.s.h; memcpy(w->s.bind, t_nbd.s.bind, sizeof(nbd_bind) * t_nbd.s.h.nbind); w->scb = t_nbd.cb; w->in = t_nbd.in; w->emitting = t_nbd.emitting; w->drew = t_nbd.drew;
    w->chk = t_nbd_chk; w->mirror = t_nbd_mirror; w->sx = t_nrn_sx; w->shrt = t_nrn_short; w->nf = t_nrd_nf; w->wf = t_nrd_wf; w->texgen = g_nbd_texgen; w->tickn = t_nrdx_n;
    w->tabsig = g_nrd_tabsig; memcpy(w->tx, t_nrdx, sizeof w->tx); memcpy(w->gx, g_nrdx, sizeof w->gx); memcpy(w->ps, g_nrnps, sizeof w->ps);
    w->cur[0] = g_cmd[0].cur; w->cur[1] = g_cmd[1].cur;
}
static void load(const world *w) {
    g_nrdm = w->texm; g_nrub_m = w->ubm; memcpy(g_nbt.tex, w->tex, sizeof w->tex); memcpy(g_nbt.img, w->img, sizeof w->img);
    memcpy(g_nbt.tdirty, w->tdirty, sizeof w->tdirty); memcpy(g_nbt.idirty, w->idirty, sizeof w->idirty); g_nbt.cb = w->tapcb; g_nbt.handle = w->taph;
    t_nbd.s.h = w->s.h; memcpy(t_nbd.s.bind, w->s.bind, sizeof(nbd_bind) * w->s.h.nbind); t_nbd.cb = (cmd_t *)w->scb; t_nbd.in = w->in; t_nbd.emitting = w->emitting; t_nbd.drew = w->drew;
    t_nbd_chk = w->chk; t_nbd_mirror = w->mirror; t_nrn_sx = w->sx; t_nrn_short = w->shrt; t_nrd_nf = w->nf; t_nrd_wf = w->wf; g_nbd_texgen = w->texgen; t_nrdx_n = w->tickn;
    g_nrd_tabsig = w->tabsig; memcpy(t_nrdx, w->tx, sizeof w->tx); memcpy(g_nrdx, w->gx, sizeof w->gx); memcpy(g_nrnps, w->ps, sizeof w->ps);
    g_cmd[0].cur = w->cur[0]; g_cmd[1].cur = w->cur[1];
}
static const char *diff(const world *a, const world *b) {
#define D(f, name) if (memcmp(&a->f, &b->f, sizeof a->f)) return name
    D(texm, "the texture mirror"); D(ubm, "the uniform-buffer mirror"); D(tex, "the tap's texture table"); D(img, "the tap's image table"); D(tdirty, "the tap's dirty masks");
    D(idirty, "the tap's image dirty masks"); D(tapcb, "the tap's command buffer"); D(taph, "the tap's handle"); D(s, "the shadow of the draw"); D(scb, "the shadow's command buffer");
    D(in, "bracket"); D(emitting, "emitting"); D(drew, "drew"); D(chk, "check flag"); D(mirror, "mirror flag"); D(sx, "the left-out mask"); D(shrt, "the short flag");
    D(nf, "binds not sent"); D(wf, "would-not-send flags"); D(texgen, "the texture-set generation"); D(tickn, "the counters' tick"); D(tabsig, "the signature of the tap's tables");
    D(tx, "the thread's counters"); D(gx, "the shared counters"); D(ps, "the short record's counters"); D(cur, "command memory");
#undef D
    return NULL;
}
typedef struct { int nub, ntb; nru_call ub[20]; nvn_nrnb_tex tb[100]; } binds_t;
static void bodies(NVNcommandBuffer *c, const binds_t *b) {
    for (int i = 0; i < b->nub; i++) h_nvnCommandBufferBindUniformBuffer((NVNcommandBuffer *)(uintptr_t)b->ub[i].cb, (NVNshaderStage)b->ub[i].stage, (int)b->ub[i].slot, b->ub[i].addr, (size_t)b->ub[i].size);
    for (int i = 0; i < b->ntb; i++) nvn_nrd_bind((uint64_t)(uintptr_t)c, b->tb[i].stage, b->tb[i].slot, b->tb[i].tex, b->tb[i].smp);
}

int main(void) {
    for (int i = 0; i < 4; i++) g_w[i] = (world *)malloc(sizeof(world));
    static typeof(J) ja, jb;
    for (int k = 0; k < 2; k++) { g_cmd[k].magic = MAGIC('C', 'M', 'D', 'B'); g_cmd[k].recording = 1; g_cmd[k].cur = g_mem[k]; g_cmd[k].end = g_mem[k] + sizeof g_mem[k]; }
    NVNcommandBuffer *c = (NVNcommandBuffer *)&g_cmd[0], *c2 = (NVNcommandBuffer *)&g_cmd[1];
    g_nbd_on = 1;
    uint64_t folds = 0, refused = 0, filt_seen = 0, short_seen = 0, ubskip = 0, why_seen[NVN_NRNB_S_N] = {0}, vbad = 0, vcmp = 0;
    static int other_thread;
    const int DRAWS = 240000;
    for (int n = 0; n < DRAWS; n++) {
        if (!(n % 1500)) {   // a new combination of the switches (most runs are the phone's: everything on)
            const int plain = rn(3) != 0;
            g_nrd_mode = plain || rn(8) ? 1 : 2; g_nrd_texf = plain || rn(3) ? g_nrd_mode : 0; g_nrd_tap = plain || rn(3) ? g_nrd_mode : 0;
            g_nrub = plain ? 1 : (int)rn(3); g_dm_nb = plain || rn(4) ? 1 : 0; g_z5 = !plain && !rn(6); g_nru_cap = !plain && !rn(8); g_nc_cap = !plain && !rn(10);
            g_render_thread = plain || rn(8); t_nd.mode = !plain && !rn(10);
        }
        // between draws
        const unsigned ev = rn(40);
        if (ev == 0) { nrdm_begin(&g_nrdm, c); nrub_begin(&g_nrub_m, c); g_nbd_texgen++; }                                  // BeginRecording
        else if (ev == 1) { const uint64_t h = rn(50); nrdm_bind(&g_nrdm, c, &other_thread, 0, NRDM_TEX, 1, (int)rn(8), 1, &h); }   // another thread into the recording
        else if (ev == 2) { const uint64_t a = 0x1000 * rn(9), z = 256; nrub_bind(&g_nrub_m, c, &other_thread, 0, 1, (int)rn(4), 1, &a, &z, NULL); }
        else if (ev == 3) g_nbt.cb = rn(4) ? NULL : (const void *)c2;                                                        // the tap's IR was reset / follows another buffer
        else if (ev == 4) { uint64_t h[3] = {rn(60), rn(60), rn(60)}; bind_handles(c, rn(4) ? OP_BIND_TEX : OP_BIND_IMG, (int)rn(6), (int)rn(30), 1 + (int)rn(3), h); }   // a bind outside a bracket
        else if (ev == 5) h_nvnCommandBufferBindUniformBuffer(c, (int)rn(2), (int)rn(6), 0x1000 * rn(9), 256);
        else if (ev == 6) g_cmd[0].cur = g_cmd[0].end - (rn(2) ? 100 : 20000);                                                // command memory nearly out / enough again
        else if (ev == 7) g_cmd[0].cur = g_mem[0];
        if (g_nbt.cb == (const void *)c2 && !rn(6)) g_nbt.cb = NULL;                                                         // the submit of the other recording resets the IR
        J.len = 0;
        // the bracket of the engine draw
        t_nbd.in = t_nd.mode != 2; t_nbd.cb = NULL; t_nbd.drew = 0; t_nbd.emitting = 0; nbd_reset(); t_nbd_chk = !rn(60); t_nbd_mirror = 0;
        t_nrub_scope = rn(12) != 0;
        if (!rn(30)) h_nvnCommandBufferBindUniformBuffer(rn(2) ? c : c2, 0, (int)rn(4), 0x40000, 64);   // a bind already in the shadow (the engine's own uniform-block code)
        const int shrt = !rn(3);
        nvn_nrn_short(shrt);
        binds_t b; b.nub = (int)rn(6); b.ntb = (int)rn(5);
        if (!rn(200)) b.ntb = 60 + (int)rn(8);
        for (int i = 0; i < b.nub; i++) {
            b.ub[i].cb = (uint64_t)(uintptr_t)(rn(300) ? c : c2); b.ub[i].stage = rn(400) ? rn(2) : 6; b.ub[i].slot = rn(400) ? rn(6) : 16;
            b.ub[i].addr = 0x1000 * rn(3); b.ub[i].size = rn(5) ? 256 : 512;
        }
        for (int i = 0; i < b.ntb; i++) {
            static const uint8_t st[3] = {0, 1, 5};
            b.tb[i].stage = st[rn(3)]; b.tb[i].slot = (uint8_t)rn(b.ntb > 8 ? 32 : 6); b.tb[i].tex = (int32_t)rn(5); b.tb[i].smp = rn(50) ? (int32_t)rn(4) : -1;
        }
        typeof(J) j0 = J;
        save(g_w[0]);
        // A: the bodies
        bodies(c, &b); save(g_w[1]); ja = J;
        // B: the fold (the bodies when it refuses)
        load(g_w[0]); J = j0;
        g_nrnb = 1;
        uint64_t gate_off = 0;   // the gate's one call of dm_nb_frame on a thread that is not the render thread (counted there; the bodies' own calls follow)
        const int took = nvn_nrnb_binds((uint64_t)(uintptr_t)c, b.ub, b.nub, b.tb, b.ntb);
        if (took) folds++; else { refused++; const char *d0 = diff(g_w[0], (save(g_w[3]), g_w[3])); gate_off = J.offthread - j0.offthread; J.offthread = j0.offthread; CHECK(gate_off <= 1 && (!gate_off || !g_render_thread), "the gate asked for the frame IR %llu times", (unsigned long long)gate_off);
                     CHECK(!d0 && !memcmp(&J, &j0, sizeof J), "a refused fold touched %s", d0 ? d0 : "the journal"); J.offthread += gate_off; bodies(c, &b); }
        save(g_w[2]); jb = J;
        const char *d = diff(g_w[1], g_w[2]);
        CHECK(!d, "draw %d (%d + %d binds, %s, fold %s): %s differs", n, b.nub, b.ntb, shrt ? "short" : "long", took ? "taken" : "refused", d ? d : "");
        CHECK(ja.nwt == jb.nwt && !memcmp(ja.wt, jb.wt, sizeof(uint64_t) * (size_t)ja.nwt), "draw %d: the use marks differ (%d / %d)", n, ja.nwt, jb.nwt);
        CHECK(!memcmp(ja.note, jb.note, sizeof ja.note), "draw %d: the tap's notes differ", n);
        CHECK(ja.len == jb.len && !memcmp(ja.stream, jb.stream, ja.len), "draw %d: the records differ", n);
        CHECK(ja.flush == jb.flush && ja.logs == jb.logs && ja.offthread + gate_off == jb.offthread, "draw %d: flushes / log lines / off-thread calls differ", n);
        if (took) { CHECK(ja.len == 0 && ja.flush == j0.flush, "draw %d: a taken draw's bodies wrote a record or flushed the shadow", n);
                    filt_seen += g_w[2]->nf; ubskip += g_w[2]->tx[NVN_NRD_UB_SKIP] - g_w[0]->tx[NVN_NRD_UB_SKIP]; if (shrt && (g_w[2]->sx.tm[0] | g_w[2]->sx.tm[1])) short_seen++; }
        // C: the verify mode around the bodies
        load(g_w[0]); J = j0;
        g_nrnb = 2;
        nvn_nrnb_snap((uint64_t)(uintptr_t)c, b.ub, b.nub, b.ntb);
        bodies(c, &b);
        nvn_nrnb_check((uint64_t)(uintptr_t)c, b.ub, b.nub, b.tb, b.ntb);
        save(g_w[3]); d = diff(g_w[1], g_w[3]);
        J.offthread -= gate_off; CHECK(!d && !memcmp(&J, &ja, sizeof J), "draw %d: the verify mode changed %s", n, d ? d : "the journal");
        { uint64_t s[NVN_NRNB_S_N]; nvn_nrnb_stats(s); vbad += s[NVN_NRNB_BAD]; vcmp += s[NVN_NRNB_CMP]; for (int k = 0; k < NVN_NRNB_S_N; k++) why_seen[k] += s[k];
          CHECK(!s[NVN_NRNB_BAD], "draw %d: the verify mode reports a difference", n); }
        // the draw record takes the shadow (nbd_draw: nrd_ext_take, the short flag, nbd_reset), the bracket ends
        t_nrd_nf = 0; t_nrn_short = 0; nbd_reset(); t_nbd.in = 0; t_nbd.cb = NULL; t_nbd_chk = 0; t_nbd_mirror = 0;
    }
    CHECK(folds > (uint64_t)DRAWS / 3 && refused > 2000, "coverage: %llu folds, %llu refusals", (unsigned long long)folds, (unsigned long long)refused);
    CHECK(filt_seen > 1000 && short_seen > 1000 && ubskip > 1000, "coverage: binds not sent %llu, short draws with a left-out slot %llu, ranges not sent %llu", (unsigned long long)filt_seen,
          (unsigned long long)short_seen, (unsigned long long)ubskip);
    for (int k = NVN_NRNB_NO_MODE; k < NVN_NRNB_S_N; k++) CHECK(why_seen[k] > 0, "coverage: refusal reason %d never seen", k);
    CHECK(vcmp > (uint64_t)DRAWS / 3 && !vbad, "verify: %llu compared, %llu bad", (unsigned long long)vcmp, (unsigned long long)vbad);
    printf("%s: %llu checks, %llu failed; %llu folds, %llu refusals, binds not sent %llu, ranges not sent %llu, short draws %llu, verify compared %llu bad %llu (mutation %d)\n",
           g_fail ? "FAIL" : "PASS", g_checks, g_fail, (unsigned long long)folds, (unsigned long long)refused, (unsigned long long)filt_seen, (unsigned long long)ubskip,
           (unsigned long long)short_seen, (unsigned long long)vcmp, (unsigned long long)vbad, NRNB_MUTATE);
    return g_fail ? 1 : 0;
}
