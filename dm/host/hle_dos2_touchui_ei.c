// [DOS2] Flash <-> engine calls of the UI: ExternalInterface calls of the movies and the engine's invokes, logged and (EI) answered by the
// host for names the Switch classes do not know (DM_DOS2_TOUCHUI_EI, default 0; docs/dos2/TOUCH_UI_PLAN.md TU-06).
// main = main.raw of update v786432, build 4979b200, offsets = main.raw. Static only (not run on a phone when written).
//   FlashPlayer (Iggy) vtable = main+0x29c3ba0 (slot 0 0xd230d0 destructor; +0x30 0xd23420 OnInputEvent, +0x38 0xd23800 SendEventToFlash,
//   +0x190 0xd26a60 mouse move: the offsets hle_dos2_touch.c uses). Object: +0x30 the Iggy player, +0x48 / +0x50 the invoke name table
//   (16-byte entries {Iggy fast name, const char *name}, count = bits 24..47 of +0x50), +0x60 a lock, +0xa0 / +0xa8 the queue of pending
//   invokes, +0xd0 / +0xd8 EI arguments (0x40 each), +0xe8 / +0xf0 EI call records (0x18 each: {u64 offset of the name in the scratch
//   buffer, u32 name length, u32 first argument, u32 argument count}; count = bits 24..47 of +0xf0), +0xf8 the listener (the UI object,
//   set by vt+0xf0 0xd25ac0), +0x130 the scratch string {char *, u32 length, u32 capacity}.
//   Movie -> engine:
//     0xd20a30  Iggy set-up registers the callbacks: 0xd1f650 warnings (0x3ec8e0), 0xd1f8c0 the AS3 ExternalInterface callback (0x3ec900),
//               0xd1fd70 custom draw (0x3ec910).
//     0xd1f8c0  (userdata, Iggy player, call): finds the FlashPlayer whose +0x30 is that player and calls
//     0xd279c0  FlashPlayer::OnExternalCall(this, call {char *name, +8 length, +0x10 argument count, +0x18 arguments 0x20 each {type, +0x10
//               value}}): only QUEUES it (name into the scratch buffer, arguments converted into +0xd0, a record into +0xe8, byte +0x91 = 1).
//     0xd25d90  FlashPlayer vt+0x130, no direct caller: drains the records: for each `listener->vt[0](listener, name, count, args)`
//               (three unrolled call sites 0xd25df0 / 0xd25e34 / 0xd25fe0: ldr x8,[x0]; ldr x8,[x8]; blr x8), then empties the lists.
//     listener->vt[0] = UIObject::OnFunctionCalled(this, const char *name, unsigned count, InvokeDataValue *args): the base is 0xae2780
//               (vtable main+0x2999a18 slot 0: closeUI, inputFocus, inputFocusLost, UIAssert, moveWindowStart, startMoveWindow,
//               cancelMoveWindow, PlaySound, setMcSize, registerAnchorId, unregisterAnchorId, setAnchor, keepUIinScreen, clearAnchor,
//               setPosition, copy, cut ... by strcmp 0x1bf4600), the game UI base 0x10a5750 (setPosition, hideTooltip ...) calls it; every
//               class has its own strcmp chain in front. A name nobody compares falls through: no effect.
//   InvokeDataValue (0x40 bytes): +0 u32 type (3 bool at +0x28, 4 double at +0x38, 5 string = engine string at +8, 6 wide string at
//               +0x18 (u32 characters: short form from +4, long form pointer at +8)); 0xd27420 copies one into a pending invoke.
//   Engine -> movie: FlashPlayer vt+0x58..+0x88 = Invoke0..Invoke6 (0xd23d10 0xd23f60 0xd241c0 0xd24430 0xd246c0 0xd24960 0xd24c20:
//               (this, invoke id, InvokeDataValue *a1..a6)), vt+0x90 0xd24ef0 InvokeArgs(this, id, InvokeDataValue *args, count); all are
//               called through the vtable only (no bl). Each takes the lock +0x60, makes a 0x28-byte record (0xd286e0) {+0 fast name,
//               +8 u32 count, +0x10 name, +0x20 arguments}, fills it from the name table by id and appends it to +0xa0: the movie is called
//               later (0xd26030 -> IggyPlayerCallMethodRS 0x3ebca0 -> 0x3d00d0; a missing method = warning + error 0x1f8, no crash).
//   Not pinned: SetValue / GetValue (candidates by their Iggy calls: vt+0xe0 0xd25900, vt+0xf8 0xd25ae0; vt+0x110 0xd25ba0 sets rotation /
//               scaleX / scaleY). Not hooked.
// What the host does (DM_DOS2_TOUCHUI_EI >= 1):
//   - 0xd25d90: its first four instructions go to a trampoline, the host drains the records itself the way the game does (same reads, same
//     call), logs each call and asks the host table first; then it sets the record count to 0 and runs the original for the clean-up
//     (with count 0 the original goes straight to its common tail 0xd25e58).
//   - Invoke0..6 / InvokeArgs: the same kind of hook, log only, then the original.
//   - log: the first time a (UI type, name) pair is seen the whole call with its arguments, after that at 10, 100, 1000 ... calls; the full
//     table (every pair with its counter) once the game has been quiet for DM_DOS2_TOUCHUI_EI_DUMP_S seconds after new pairs (default 20).
//   - host handlers: dm_dos2_touchui_ei_register(name, fn, ctx); fn returns 1 = the call is the host's (the game's class does not get it).
//     Built in: the iPad-only names of the `_t` movies (TOUCH_UI_PLAN.md 2.2: no such string in the Switch main, so no class can know them)
//     are taken by a stub that only counts (one line each). DM_DOS2_TOUCHUI_EI=2 = log only, no stubs, everything goes to the game.
//   - dm_dos2_touchui_invoke(player, name, args, count): call a movie method the engine way (the id is looked up in the player's own
//     name table: only names the class registered can be sent).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include "hle_dos2_touchui_ei.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

enum { EI_DRAIN = 0xd25d90, EI_DRAIN_CALL = 0xd25dc4, EI_DRAIN_TAIL = 0xd25e58, EI_QUEUE = 0xd279c0, EI_CB = 0xd1f8f8, EI_REG = 0xd20a98, EI_BASE = 0xae2780,
       EI_INV0 = 0xd23d10, EI_INV1 = 0xd23f60, EI_INV2 = 0xd241c0, EI_INV3 = 0xd24430, EI_INV4 = 0xd246c0, EI_INV5 = 0xd24960, EI_INV6 = 0xd24c20,
       EI_INVN = 0xd24ef0, EI_INV_TAB = 0xd24f98, EI_ARG = 0xd27434,
       FP_IGGY = 0x30, FP_NAMES = 0x48, FP_NAMES_N = 0x50, FP_ARGS = 0xd0, FP_RECS = 0xe8, FP_RECS_N = 0xf0, FP_LISTENER = 0xf8, FP_SCRATCH = 0x130,
       UI_PATH = 0x20, UI_TYPE = 0xf0 };
static const uint32_t k_ei_drain[4] = {0xa9bc5ff8 /* stp x24,x23,[sp,#-0x40]! */, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};
static const uint32_t k_ei_drain_call[14] = {0xf9407e60 /* ldr x0,[x19,#0xf8]: the listener */, 0xb40001a0, 0xf9407668 /* ldr x8,[x19,#0xe8]: records */,
    0xf9400109 /* ldr x9,[x8]: name offset */, 0xb9401102 /* ldr w2,[x8,#0x10]: count */, 0x8b090281 /* add x1,x20,x9: name */, 0xf9000101, 0x34000d82,
    0xb9400d08 /* ldr w8,[x8,#0xc]: first argument */, 0xf9406a69 /* ldr x9,[x19,#0xd0] */, 0x8b081923 /* add x3,x9,x8,lsl #6 */,
    0xf9400008 /* ldr x8,[x0] */, 0xf9400108 /* ldr x8,[x8]: vt[0] = OnFunctionCalled */, 0xd63f0100 /* blr x8 */};
static const uint32_t k_ei_drain_tail[3] = {0x92509d09 /* and x9,x8,#0xffff000000ffffff: count = 0 */, 0x3902467f /* strb wzr,[x19,#0x91] */, 0xf9007a69 /* str x9,[x19,#0xf0] */};
static const uint32_t k_ei_queue[4] = {0xd10383ff /* sub sp,sp,#0xe0 */, 0xa9086ffc, 0xa90967fa, 0xa90a5ff8};
static const uint32_t k_ei_cb[2] = {0xaa1303e1 /* mov x1,x19: the call */, 0x94002031 /* bl 0xd279c0 */};
static const uint32_t k_ei_reg[4] = {0xf0ffffe0 /* adrp x0,main+0xd1f000 */, 0x91230000 /* add x0,x0,#0x8c0: the EI callback */, 0xaa1303e1, 0x97db2f97 /* bl 0x3ec900 */};
static const uint32_t k_ei_base[4] = {0xd10143ff /* sub sp,sp,#0x50 */, 0xa90257f6, 0xa9034ff4, 0xa9047bfd};
static const uint32_t k_ei_inv0[4] = {0xa9bc5ff8, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};
static const uint32_t k_ei_inv1[4] = {0xa9bc5ff8, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};
static const uint32_t k_ei_inv2[4] = {0xa9bc5ff8, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};
static const uint32_t k_ei_inv3[4] = {0xf81b0ff9 /* str x25,[sp,#-0x50]! */, 0xa9015ff8, 0xa90257f6, 0xa9034ff4};
static const uint32_t k_ei_inv4[4] = {0xa9bb67fa, 0xa9015ff8, 0xa90257f6, 0xa9034ff4};
static const uint32_t k_ei_inv5[4] = {0xf81a0ffb, 0xa90167fa, 0xa9025ff8, 0xa90357f6};
static const uint32_t k_ei_inv6[4] = {0xa9ba6ffc, 0xa90167fa, 0xa9025ff8, 0xa90357f6};
static const uint32_t k_ei_invn[4] = {0xa9bc5ff8, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};
static const uint32_t k_ei_inv_tab[11] = {0xf9402a89 /* ldr x9,[x20,#0x50] */, 0x2a1803e8 /* mov w8,w24: the id */, 0xd358bd29 /* ubfx x9,x9,#24,#24: names */, 0xeb08013f, 0x540000e9,
    0xf9402689 /* ldr x9,[x20,#0x48] */, 0x8b081128 /* add x8,x9,x8,lsl #4 */, 0xf9400109, 0xf90002a9, 0xf9400508 /* ldr x8,[x8,#8]: the name */, 0xf9000aa8};
static const uint32_t k_ei_arg[6] = {0xb9400048 /* ldr w8,[x2]: type */, 0xaa0103f3, 0x71000d1f /* cmp w8,#3 */, 0x54000201, 0x3940a048 /* ldrb w8,[x2,#0x28]: bool */, 0xb8408e69};

typedef struct { uint32_t off, n; const uint32_t *want; } ei_site;
typedef uint64_t (*ei_fn8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static module_t *g_main;
static int g_on, g_stubs = 1, g_dump_s = 20;
static ei_fn8 g_drain_orig, g_inv_orig[8];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// ---- values ----
static int ei_estr(const uint8_t *s, const char **p, uint32_t *len) {   // the engine string: long {u32 size|1, u32 length, char *} or short {u8 length << 1, chars}
    if (s[0] & 1) { uint32_t cap, l; const char *q; memcpy(&cap, s, 4); memcpy(&l, s + 4, 4); memcpy(&q, s + 8, 8);
                    if (!q || l > 4096 || l + 1 > (cap & ~1u)) return 0; *p = q; *len = l; return 1; }
    *p = (const char *)s + 1; *len = s[0] >> 1; return *len <= 14;
}
const char *dm_ei_str(const dm_ei_val *v, char *buf, size_t n) {
    const uint8_t *b = (const uint8_t *)v; const char *p; uint32_t len;
    if (!n) return "";
    buf[0] = 0;
    if (v->type == 5 && ei_estr(b + 8, &p, &len)) { if (len >= n) len = (uint32_t)n - 1; memcpy(buf, p, len); buf[len] = 0; }
    else if (v->type == 6) {   // wide: u32 characters; anything outside ASCII becomes '?'
        const uint32_t *w; uint32_t l;
        if (b[0x18] & 1) { memcpy(&l, b + 0x1c, 4); memcpy(&w, b + 0x20, 8); if (!w || l > 4096) return buf; }
        else { l = b[0x18] >> 1; w = (const uint32_t *)(const void *)(b + 0x1c); if (l > 4) return buf; }
        if (l >= n) l = (uint32_t)n - 1;
        for (uint32_t i = 0; i < l; i++) buf[i] = w[i] >= 32 && w[i] < 127 ? (char)w[i] : '?';
        buf[l] = 0;
    }
    return buf;
}
static void ei_args(char *o, size_t on, const dm_ei_val *a, unsigned n) {
    size_t k = 0; o[0] = 0;
    for (unsigned i = 0; a && i < n && i < 12 && k + 80 < on; i++) {
        char s[64];
        if (a[i].type == 3) k += (size_t)snprintf(o + k, on - k, "%s%s", i ? ", " : "", a[i].b ? "true" : "false");
        else if (a[i].type == 4) k += (size_t)snprintf(o + k, on - k, "%s%.10g", i ? ", " : "", a[i].d);
        else if (a[i].type == 5 || a[i].type == 6) k += (size_t)snprintf(o + k, on - k, "%s\"%s\"", i ? ", " : "", dm_ei_str(&a[i], s, sizeof s));
        else k += (size_t)snprintf(o + k, on - k, "%s<type %u>", i ? ", " : "", a[i].type);
    }
    if (n > 12 && k + 8 < on) snprintf(o + k, on - k, ", ...");
}
static uint32_t ei_ui_type(const uint8_t *player, char *movie, size_t mn) {   // the UI object behind a player: its type and movie
    const uint8_t *ui = player ? *(const uint8_t *const volatile *)(player + FP_LISTENER) : NULL;
    movie[0] = 0;
    if (!ui) return 0xffff;
    const char *p; uint32_t len;
    if ((ui[UI_PATH] & 1) && ei_estr(ui + UI_PATH, &p, &len) && len >= 5 && len < 300 && ((uintptr_t)p >> 12) && !((uintptr_t)p >> 48)) {
        uint32_t s = len; while (s && p[s - 1] != '/') s--;
        snprintf(movie, mn, "%.*s", (int)(len - s), p + s);
    }
    return *(const volatile uint32_t *)(ui + UI_TYPE);
}

// ---- the log table: (direction, UI type, name) -> counter ----
enum { EI_MAXLOG = 1024 };
typedef struct { uint8_t dir; uint16_t type; uint8_t stub; uint32_t n; char name[44], movie[40]; } ei_row;
static ei_row g_rows[EI_MAXLOG]; static int g_nrows, g_rows_new;
static uint64_t g_rows_t;
static uint64_t ei_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec; }
static void ei_dump_locked(const char *why) {
    LOG("[ei] table (%s): %d distinct calls", why, g_nrows);
    for (int i = 0; i < g_nrows; i++)
        LOG("[ei]   %s type %#x %-28s %-36s x%u%s", g_rows[i].dir ? "invoke" : "EI    ", g_rows[i].type, g_rows[i].movie, g_rows[i].name, g_rows[i].n, g_rows[i].stub ? " (host stub)" : "");
    g_rows_new = 0;
}
void dm_dos2_touchui_ei_dump(const char *why) { if (!g_on) return; pthread_mutex_lock(&g_lock); ei_dump_locked(why ? why : "asked"); pthread_mutex_unlock(&g_lock); }
static void ei_note(int dir, const uint8_t *player, const char *name, unsigned n, const dm_ei_val *a, int stub) {
    char movie[40]; const uint32_t type = ei_ui_type(player, movie, sizeof movie);
    pthread_mutex_lock(&g_lock);
    ei_row *r = NULL;
    for (int i = 0; i < g_nrows; i++) if (g_rows[i].dir == dir && g_rows[i].type == (uint16_t)type && !strncmp(g_rows[i].name, name, sizeof g_rows[i].name - 1)) { r = &g_rows[i]; break; }
    if (!r && g_nrows < EI_MAXLOG) { r = &g_rows[g_nrows++]; memset(r, 0, sizeof *r); r->dir = (uint8_t)dir; r->type = (uint16_t)type; r->stub = (uint8_t)stub;
                                     snprintf(r->name, sizeof r->name, "%s", name); snprintf(r->movie, sizeof r->movie, "%s", movie); g_rows_new++; g_rows_t = ei_s(); }
    if (r) {
        const uint32_t c = ++r->n;
        if (c == 1 || c == 10 || c == 100 || c == 1000 || c == 10000 || c == 100000) {
            char args[512]; ei_args(args, sizeof args, a, n);
            LOG("[ei] %s type %#x %s: %s(%s)%s%s", dir ? "invoke ->" : "EI     <-", type, movie, name, args, stub ? " [host stub: not sent to the game's class]" : "", c > 1 ? "" : " (first)");
            if (c > 1) LOG("[ei]   ... x%u", c);
        }
    }
    if (g_rows_new && g_dump_s > 0 && ei_s() - g_rows_t >= (uint64_t)g_dump_s) ei_dump_locked("quiet");
    pthread_mutex_unlock(&g_lock);
}

// ---- host handlers ----
static int g_quiet;   // handlers only: no log, no invoke hooks (the default with DM_DOS2_TOUCHUI)
enum { EI_MAXH = 96 };
static struct { char name[44]; dm_ei_fn fn; void *ctx; } g_h[EI_MAXH]; static int g_nh;
int dm_dos2_touchui_ei_register(const char *name, dm_ei_fn fn, void *ctx) {
    if (!name || !fn || strlen(name) >= sizeof g_h[0].name) return 0;
    pthread_mutex_lock(&g_lock);
    int k = -1;
    for (int i = 0; i < g_nh; i++) if (!strcmp(g_h[i].name, name)) k = i;
    if (k < 0 && g_nh < EI_MAXH) k = g_nh++;
    if (k >= 0) { strcpy(g_h[k].name, name); g_h[k].fn = fn; g_h[k].ctx = ctx; }
    pthread_mutex_unlock(&g_lock);
    return k >= 0;
}
static int ei_stub(void *ctx, void *player, void *ui, const char *name, unsigned n, const dm_ei_val *a) { (void)ctx; (void)player; (void)ui; (void)name; (void)n; (void)a; return 1; }
// ExternalInterface names of the iPad `_t` movies that are not a string of the Switch main at all (TOUCH_UI_PLAN.md 1 / 2.2)
static const char *const k_ipad_only[] = {"didUse2FingerScrolling", "cancelActiveAction", "clearRuneTarget", "clickedAction", "closeAndDragItem", "didToggleDeckSwitcher", "dropItem",
    "hasBusySlotByItem", "hideAllWindows", "hideFade", "showFade", "openCombineSorter", "openContextWindow", "openMap", "rightBarBtnClicked", "selectSkillSet",
    "showNotificationDetails", "startDraggingFromSlot", "startDraggingRuneFromSlot", "switchDollEquipmentShowing", "switchDollVisibility", "switchItemWaresState",
    "switchWaresModeEnabled", "toggleSidePanel", "slotUpEnd", "diplomacyPressed", "itemSelected", "actionPressed", "hideToolip"};

// ---- hooks ----
// map view (hle_dos2_touch.c): the ExternalInterface calls of the journal movies (journal_csp 0x8c, journal_c 0x46; PlaySound aside), so a tap can
// tell "a button of the movie answered" (a name that was quiet before the click) from "nothing answered" (= a tap on the map itself)
static struct { uint32_t h; uint64_t t; } g_jr[64]; static unsigned g_jri;
static uint64_t ei_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000ull + (uint64_t)t.tv_nsec / 1000000ull; }
static uint32_t ei_hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
int dm_dos2_ei_journal_reacted(uint64_t since, uint64_t quiet_ms);   // 1 = a journal movie made a call after `since` whose name it did not make in the quiet_ms before
int dm_dos2_ei_journal_reacted(uint64_t since, uint64_t quiet_ms) {
    int hit = 0;
    pthread_mutex_lock(&g_lock);
    for (unsigned i = 0; i < 64 && !hit; i++) {
        if (!g_jr[i].t || g_jr[i].t < since) continue;
        int bg = 0;
        for (unsigned k = 0; k < 64 && !bg; k++) bg = g_jr[k].h == g_jr[i].h && g_jr[k].t < since && g_jr[k].t + quiet_ms >= since;
        hit = !bg;
    }
    pthread_mutex_unlock(&g_lock);
    return hit;
}
static uint64_t ei_drain_hook(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    uint8_t *fp = (uint8_t *)(uintptr_t)a0;
    const uint8_t *scr = fp ? *(uint8_t *const volatile *)(fp + FP_SCRATCH) : NULL;
    const uint64_t cnt = fp ? (*(volatile uint64_t *)(fp + FP_RECS_N) >> 24) & 0xffffff : 0;
    if (!scr || !cnt) return g_drain_orig(a0, a1, a2, a3, a4, a5, a6, a7);
    const char *base = *(const char *const volatile *)scr;   // the game reads it once, before the loop (x20)
    for (uint64_t i = 0; i < ((*(volatile uint64_t *)(fp + FP_RECS_N) >> 24) & 0xffffff); i++) {
        void *ui = *(void *volatile *)(fp + FP_LISTENER);
        if (!ui) continue;
        uint8_t *rec = *(uint8_t *volatile *)(fp + FP_RECS) + i * 0x18;
        const char *name = base + *(uint64_t *)(void *)rec;
        const uint32_t n = *(uint32_t *)(void *)(rec + 0x10);
        *(const char **)(void *)rec = name;   // as the game does (str x1,[x8])
        dm_ei_val *args = n ? (dm_ei_val *)(void *)(*(uint8_t *volatile *)(fp + FP_ARGS) + (uint64_t)*(uint32_t *)(void *)(rec + 0xc) * 0x40) : NULL;
        dm_ei_fn h = NULL; void *ctx = NULL;
        const uint32_t uty = *(volatile uint32_t *)((uint8_t *)ui + UI_TYPE);
        pthread_mutex_lock(&g_lock);
        if ((uty == 0x8c || uty == 0x46) && strcmp(name, "PlaySound")) { g_jr[g_jri & 63].h = ei_hash(name); g_jr[g_jri & 63].t = ei_ms(); g_jri++; }
        for (int k = 0; k < g_nh; k++) if (!strcmp(g_h[k].name, name)) { h = g_h[k].fn; ctx = g_h[k].ctx; break; }
        pthread_mutex_unlock(&g_lock);
        const int taken = h ? h(ctx, fp, ui, name, n, args) : 0;
        if (!g_quiet) ei_note(0, fp, name, n, args, taken);
        if (!taken) { void **vt = *(void ***)ui; ((void (*)(void *, const char *, uint32_t, void *))vt[0])(ui, name, n, args); }
    }
    *(volatile uint64_t *)(fp + FP_RECS_N) &= ~0x0000ffffff000000ull;   // all done: the original only cleans up (count 0 -> its tail main+0xd25e58)
    return g_drain_orig(a0, a1, a2, a3, a4, a5, a6, a7);
}
static const char *ei_inv_name(const uint8_t *fp, uint32_t id) {
    const uint64_t cnt = (*(const volatile uint64_t *)(fp + FP_NAMES_N) >> 24) & 0xffffff;
    const uint8_t *tab = *(const uint8_t *const volatile *)(fp + FP_NAMES);
    const char *nm = tab && id < cnt ? *(const char *const *)(const void *)(tab + (uint64_t)id * 16 + 8) : NULL;
    return nm ? nm : "?";
}
static void ei_inv_note(uint64_t fp, uint64_t id, int n, const uint64_t *a) {
    dm_ei_val v[6];
    for (int i = 0; i < n && i < 6; i++) { if (a[i]) memcpy(&v[i], (const void *)(uintptr_t)a[i], sizeof v[i]); else memset(&v[i], 0, sizeof v[i]); }
    char nm[48]; const char *s = ei_inv_name((const uint8_t *)(uintptr_t)fp, (uint32_t)id);
    if (s[0] == '?') snprintf(nm, sizeof nm, "#%u", (unsigned)id); else snprintf(nm, sizeof nm, "%s", s);
    ei_note(1, (const uint8_t *)(uintptr_t)fp, nm, (unsigned)n, v, 0);
}
#define EI_INV(N) static uint64_t ei_inv##N(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) { \
    const uint64_t a[6] = {a2, a3, a4, a5, a6, a7}; if (a0) ei_inv_note(a0, a1, N, a); return g_inv_orig[N](a0, a1, a2, a3, a4, a5, a6, a7); }
EI_INV(0) EI_INV(1) EI_INV(2) EI_INV(3) EI_INV(4) EI_INV(5) EI_INV(6)
static uint64_t ei_invn(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
    if (a0) { char nm[48]; const char *s = ei_inv_name((const uint8_t *)(uintptr_t)a0, (uint32_t)a1);
              if (s[0] == '?') snprintf(nm, sizeof nm, "#%u", (unsigned)a1); else snprintf(nm, sizeof nm, "%s", s);
              ei_note(1, (const uint8_t *)(uintptr_t)a0, nm, (unsigned)a3, (const dm_ei_val *)(uintptr_t)a2, 0); }
    return g_inv_orig[7](a0, a1, a2, a3, a4, a5, a6, a7);
}
// call a movie method the engine way: InvokeArgs through the player's vtable (so it is logged too); 0 = the class never registered that name
int dm_dos2_touchui_invoke(void *player, const char *name, const dm_ei_val *args, unsigned n) {
    const uint8_t *fp = player;
    if (!fp || !name) return 0;
    const uint64_t cnt = (*(const volatile uint64_t *)(fp + FP_NAMES_N) >> 24) & 0xffffff;
    const uint8_t *tab = *(const uint8_t *const volatile *)(fp + FP_NAMES);
    for (uint64_t id = 0; tab && id < cnt; id++) {
        const char *nm = *(const char *const *)(const void *)(tab + id * 16 + 8);
        if (nm && !strcmp(nm, name)) { void **vt = *(void ***)player; return (int)((uint64_t (*)(void *, uint32_t, const dm_ei_val *, uint32_t))vt[0x90 / 8])(player, (uint32_t)id, args, n) & 1; }
    }
    return 0;
}

static int ei_poke(module_t *m, uint32_t off, const uint32_t *w, int n) {
    uint32_t *at = (uint32_t *)(void *)(m->base + off);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF);
    const size_t len = (size_t)(((uintptr_t)(at + n) - (uintptr_t)pg + 0x3FFF) & ~(uintptr_t)0x3FFF);
    if (mprotect(pg, len, PROT_READ | PROT_WRITE)) return -1;
    memcpy(at, w, 4 * (size_t)n);
    if (mprotect(pg, len, PROT_READ | PROT_EXEC)) die("[ei] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)(at + n));
    return 0;
}
int dm_dos2_touchui_ei_on(void) { return g_on; }
void dm_dos2_touchui_ei_install(void);   // main.c host_preload, after dm_dos2_touchui_install
void dm_dos2_touchui_ei_install(void) {
    const char *e = getenv("DM_DOS2_TOUCHUI_EI"), *v;
    // console touch (owner 15.11): with DM_DOS2_TOUCHUI on and DM_DOS2_TOUCHUI_EI unset only the ExternalInterface drain is hooked, for the host
    // handlers ("dmPadBtn": a tap on a button hint, hle_dos2_touch.c); no log table, no invoke hooks, no iPad stubs. =0 turns that off too.
    { int dm_dos2_touchui_on(void); if ((!e || !*e) && dm_dos2_touchui_on()) { e = "handlers"; g_quiet = 1; } }
    if (!g_quiet && (!e || atoi(e) <= 0)) return;
    g_stubs = !g_quiet && atoi(e) < 2;
    if ((v = getenv("DM_DOS2_TOUCHUI_EI_DUMP_S")) && *v) g_dump_s = atoi(v);
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_main = &g_mod[i];
    module_t *m = g_main;
    if (!m) { LOG("【红】[ei] no main module: DM_DOS2_TOUCHUI_EI off"); return; }
    static const ei_site k_sites[] = {{EI_DRAIN, 4, k_ei_drain}, {EI_DRAIN_CALL, 14, k_ei_drain_call}, {EI_DRAIN_TAIL, 3, k_ei_drain_tail}, {EI_QUEUE, 4, k_ei_queue}, {EI_CB, 2, k_ei_cb},
        {EI_REG, 4, k_ei_reg}, {EI_BASE, 4, k_ei_base}, {EI_INV0, 4, k_ei_inv0}, {EI_INV1, 4, k_ei_inv1}, {EI_INV2, 4, k_ei_inv2}, {EI_INV3, 4, k_ei_inv3}, {EI_INV4, 4, k_ei_inv4},
        {EI_INV5, 4, k_ei_inv5}, {EI_INV6, 4, k_ei_inv6}, {EI_INVN, 4, k_ei_invn}, {EI_INV_TAB, 11, k_ei_inv_tab}, {EI_ARG, 6, k_ei_arg}};
    for (unsigned i = 0; i < sizeof k_sites / sizeof *k_sites; i++)
        if ((uint64_t)k_sites[i].off + 4ull * k_sites[i].n > m->text_end || memcmp(m->base + k_sites[i].off, k_sites[i].want, 4ull * k_sites[i].n)) {
            LOG("【红】[ei] main+%#x differs: not DOS2 update v786432 main (build 4979b200) at the Flash player: DM_DOS2_TOUCHUI_EI off, nothing changed", k_sites[i].off); return; }
    const struct { uint32_t off; const uint32_t *pro; ei_fn8 host; ei_fn8 *orig; } hk[9] = {{EI_DRAIN, k_ei_drain, ei_drain_hook, &g_drain_orig},
        {EI_INV0, k_ei_inv0, ei_inv0, &g_inv_orig[0]}, {EI_INV1, k_ei_inv1, ei_inv1, &g_inv_orig[1]}, {EI_INV2, k_ei_inv2, ei_inv2, &g_inv_orig[2]}, {EI_INV3, k_ei_inv3, ei_inv3, &g_inv_orig[3]},
        {EI_INV4, k_ei_inv4, ei_inv4, &g_inv_orig[4]}, {EI_INV5, k_ei_inv5, ei_inv5, &g_inv_orig[5]}, {EI_INV6, k_ei_inv6, ei_inv6, &g_inv_orig[6]}, {EI_INVN, k_ei_invn, ei_invn, &g_inv_orig[7]}};
    uint32_t *t = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) { LOG("【红】[ei] mmap failed: DM_DOS2_TOUCHUI_EI off"); return; }
    for (int i = 0; i < 9; i++) {   // trampoline: the four prologue instructions (none PC-relative), then a jump to function + 16
        uint32_t *s = t + 8 * i; const uint64_t rest = (uint64_t)(uintptr_t)(m->base + hk[i].off + 16);
        memcpy(s, hk[i].pro, 16); s[4] = 0x58000050 /* ldr x16,#8 */; s[5] = 0xd61f0200 /* br x16 */; s[6] = (uint32_t)rest; s[7] = (uint32_t)(rest >> 32);
    }
    if (mprotect(t, 0x4000, PROT_READ | PROT_EXEC)) { LOG("【红】[ei] mprotect RX failed: DM_DOS2_TOUCHUI_EI off"); munmap(t, 0x4000); return; }
    __builtin___clear_cache((char *)t, (char *)(t + 72));
    if (g_stubs) for (unsigned i = 0; i < sizeof k_ipad_only / sizeof *k_ipad_only; i++) dm_dos2_touchui_ei_register(k_ipad_only[i], ei_stub, NULL);
    int done = 0;
    for (int i = 0; i < (g_quiet ? 1 : 9); i++) {
        *hk[i].orig = (ei_fn8)(void *)(t + 8 * i);
        const uint64_t to = (uint64_t)(uintptr_t)(void *)hk[i].host;
        const uint32_t jump[4] = {0x58000050, 0xd61f0200, (uint32_t)to, (uint32_t)(to >> 32)};
        if (ei_poke(m, hk[i].off, jump, 4)) { LOG("【红】[ei] mprotect RW failed: main+%#x not hooked", hk[i].off); continue; }
        done++;
    }
    g_on = done > 0;
    if (g_quiet) { LOG("[ei] console touch: ExternalInterface drain main+%#x hooked for the host handlers only (%s; DM_DOS2_TOUCHUI_EI=1 = the full log)", EI_DRAIN, done ? "on" : "FAILED"); return; }
    LOG("[ei] DM_DOS2_TOUCHUI_EI=%s: %d of 9 hooks: ExternalInterface drain main+%#x (movie -> UIObject::OnFunctionCalled, base main+%#x), Invoke0..6 main+%#x.. and InvokeArgs main+%#x "
        "(engine -> movie); %d host handler(s)%s; table dump after %d s of no new names", e, done, EI_DRAIN, EI_BASE, EI_INV0, EI_INVN, g_nh,
        g_stubs ? " (iPad-only names are taken by a counting stub)" : " (log only)", g_dump_s);
}
#endif
