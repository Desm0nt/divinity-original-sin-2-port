// [DOS2] Base A: UI mode TOUCH on the console INPUT mode of the engine (owner 21.11; docs/dos2/TOUCH_MODES_PLAN.md "Основа A: касание на
// консольном режиме движка"; rules and the reasoning in hle_dos2_touch_base.h). Static only when written: nothing here ran on a phone.
// main = main.raw of update v786432, build 4979b200, offsets = main.raw.
//   DM_DOS2_TOUCH_BASE=console   (default off = today's TOUCH byte for byte; =pc the same, said aloud)
// What it does, and nothing else:
//   1. the two reads of the mode byte main+0x2bb8918 in the input manager's listener filter (main+0x8440b0 `ldrb w11,[x11,#0x918]` and its copy
//      main+0x844248 `ldrb w10,[x10,#0x918]`) become `mov w11,#1` / `mov w10,#1`: the filter always answers as in controller mode, so the pad's
//      records (device class 1) reach every listener as they do in the hybrid. SWITCH / HYBRID run with the byte at 1: no change for them.
//   2. hle_dos2_touch.c holds [input manager + 0x170] = 1 while the TOUCH PC UI is up (keyboard / mouse records pass the filter through that
//      byte now, to the same listeners as before), points the bindings path at the merged file PlayerProfiles/Eoc/dmtouch_1.json of
//      TouchUI.pak (keyboard_1.json + controller_1.json of the player's Engine.pak, tools/dos2_touch_bind.py) and sends the host's pad pulses
//      (hint taps of the console `_d` menus, the deferred main menu press) as real npad pulses instead of keyboard twins.
// The mode byte itself stays 0 in TOUCH: the ~800 other readers (UI set builder, window type choices, PC classes, picking, tooltips, key
// names, targeting: tools/dos2_mode_readers.py lists them) see what they see today, so the PC windows, the host-made PC HUD / panels and
// every TOUCH gesture run the same code paths as today. With no pad input at all the game cannot tell the base from today's TOUCH except
// by the bindings file's extra pad entries.
//   DM_DOS2_TOUCH_BASE_PIN=<hex>[,<hex>...]   experiment only (needs =console): more readers of the mode byte answered "controller" (each
//      must be an `ldrb w,[x,#0x918]` right behind its `adrp` to main+0x2bb8000, else it is refused with a red line) - to try a console
//      branch of the game for a physical pad without a rebuild. Not for players.
// Install order (main.c): dm_dos2_touch_base_install before dm_dos2_touch_install (which asks dm_dos2_touch_base_want for the bindings),
// dm_dos2_touch_base_hook after it (hle_dos2_touch.c compares the filter's words for DM_DOS2_TOUCH_KBM before they are changed).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include "hle_dos2_touch_base.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

enum { TB_MODE_PAGE = 0x2bb8000, TB_FILTER_A = 0x84407c, TB_CLASS0_A = 0x844508, TB_CLASSM_A = 0x844518, TB_FILTER_B = 0x84420c, TB_CLASS0_B = 0x8444cc,
       TB_CLASSM_B = 0x8444ec, TB_PIN_A = 0x8440b0, TB_PIN_B = 0x844248, TB_TEXT_MIN = 0x1000, TB_PINS = 32 };
// copy A of the filter: device class (0x84407c..), then who gets the event (0x84409c..0x8440d0)
static const uint32_t k_tb_filter_a[22] = {0x3945c2ab /* ldrb w11,[x21,#0x170]: "keyboard / mouse allowed" */, 0x7100095f /* cmp w10,#2: the device */, 0x54002423 /* b.lo 0x844508: class 0 */,
    0x51000d4a /* sub w10,w10,#3 */, 0x7100115f /* cmp w10,#4 */, 0x54002442 /* b.hs 0x844518: class -1 */, 0x320003ea /* mov w10,#1: a pad */, 0xb940051c /* ldr w28,[x8,#4]: listener flags */,
    0x372001dc /* tbnz w28,#4: deliver */, 0x7100015f /* cmp w10,#0 */, 0x7a400964 /* ccmp w11,#0,#4,eq */, 0x54000161 /* b.ne: class 0 and the byte set: deliver */,
    0x90011bab /* adrp x11,main+0x2bb8000 */, 0x3964616b /* ldrb w11,[x11,#0x918]: THE MODE BYTE (pinned) */, 0x6b0b015f /* cmp w10,w11 */, 0x540000e0 /* b.eq: deliver */,
    0x3100055f /* cmn w10,#1 */, 0x540000a0 /* b.eq: deliver */, 0xb940690b /* ldr w11,[x8,#0x68]: the listener's mask */, 0x1aca236a /* lsl w10,w27,w10 */, 0x0a0a016a, 0x340000aa /* cbz: not delivered */};
static const uint32_t k_tb_class0_a[4] = {0x2a1f03ea /* mov w10,wzr */, 0xb940051c, 0x3627dc9c /* tbz w28,#4,0x8440a0 */, 0x17fffef0};
static const uint32_t k_tb_classm_a[4] = {0x1280000a /* mov w10,#-1 */, 0xb940051c, 0x3627dc1c /* tbz w28,#4,0x8440a0 */, 0x17fffeec};
// copy B (the second walk of the same dispatch)
static const uint32_t k_tb_filter_b[25] = {0x794143eb /* ldrh w11,[sp,#0xa0]: the device */, 0xf94037e9, 0x7100097f /* cmp w11,#2 */, 0x3945c2aa /* ldrb w10,[x21,#0x170] */, 0x54001583 /* b.lo 0x8444cc */,
    0x51000d6b, 0x7100117f, 0x54001622 /* b.hs 0x8444ec */, 0x320003eb /* mov w11,#1 */, 0xb9400537 /* ldr w23,[x9,#4] */, 0x372001f7 /* tbnz w23,#4 */, 0x7100017f, 0x7a400944, 0x54000181,
    0x90011baa /* adrp x10,main+0x2bb8000 */, 0x3964614a /* ldrb w10,[x10,#0x918]: THE MODE BYTE (pinned) */, 0x6b0a017f /* cmp w11,w10 */, 0x54000100, 0x3100057f, 0x540000c0,
    0xb940692a /* ldr w10,[x9,#0x68] */, 0x320003ec, 0x1acb218b, 0x0a0b014a, 0x340003ea};
static const uint32_t k_tb_class0_b[4] = {0x2a1f03eb /* mov w11,wzr */, 0xb9400537, 0x3627eb37 /* tbz w23,#4,0x844238 */, 0x17ffff66};
static const uint32_t k_tb_classm_b[4] = {0x1280000b /* mov w11,#-1 */, 0xb9400537, 0x3627ea37 /* tbz w23,#4,0x844238 */, 0x17ffff5e};
typedef struct { uint32_t off, n; const uint32_t *want; } tb_site;
static const tb_site k_tb_sites[] = {{TB_FILTER_A, 22, k_tb_filter_a}, {TB_CLASS0_A, 4, k_tb_class0_a}, {TB_CLASSM_A, 4, k_tb_classm_a},
                                     {TB_FILTER_B, 25, k_tb_filter_b}, {TB_CLASS0_B, 4, k_tb_class0_b}, {TB_CLASSM_B, 4, k_tb_classm_b}};

static module_t *g_main;
static int g_env, g_want, g_on, g_merged;
static uint32_t g_pin[TB_PINS]; static int g_npin;

int dm_dos2_touch_base_want(void);     // DM_DOS2_TOUCH_BASE=console and the filter's sites matched (known before the patch: the bindings choice)
int dm_dos2_touch_base_want(void) { return g_want; }
int dm_dos2_touch_base_on(void);       // the filter reads are pinned: the pad's records pass in TOUCH
int dm_dos2_touch_base_on(void) { return g_on; }
int dm_dos2_touch_base_merged(void);   // TouchUI.pak holds the merged bindings file
int dm_dos2_touch_base_merged(void) { return g_merged; }

static int tb_poke(module_t *m, uint32_t off, uint32_t w) {
    uint32_t *at = (uint32_t *)(m->base + off);
    uint8_t *pg = (uint8_t *)((uintptr_t)at & ~(uintptr_t)0x3FFF);   // 16 KB = the loader's sealing granule
    if (mprotect(pg, 0x4000, PROT_READ | PROT_WRITE)) return -1;
    *at = w;
    if (mprotect(pg, 0x4000, PROT_READ | PROT_EXEC)) die("[base] mprotect RX");
    __builtin___clear_cache((char *)at, (char *)(at + 1));
    return 0;
}

void dm_dos2_touch_base_install(void);   // main.c host_preload: after dm_dos2_touchui_install (the package's file list), before dm_dos2_touch_install
void dm_dos2_touch_base_install(void) {
    int dm_dos2_touchui_pak_file(const char *name);   // hle_dos2_touchui.c
    const char *e = getenv("DM_DOS2_TOUCH_BASE"), *v;
    g_env = tb_parse(e);
    if (g_env != TB_CONSOLE) {
        if (e && *e) LOG("[base] DM_DOS2_TOUCH_BASE=%s: UI mode TOUCH on the PC input mode, as before (=console: the console input base)", e);
        return;
    }
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_main = &g_mod[i];
    module_t *m = g_main;
    if (!m) { LOG("【红】[base] no main module: DM_DOS2_TOUCH_BASE=console not applied (TOUCH as before)"); return; }
    for (unsigned i = 0; i < sizeof k_tb_sites / sizeof *k_tb_sites; i++) {
        const tb_site *s = &k_tb_sites[i];
        if (m->text_end < s->off + 4 * s->n || memcmp(m->base + s->off, s->want, 4 * (size_t)s->n)) {
            LOG("【红】[base] main+%#x differs (the input manager's listener filter): DM_DOS2_TOUCH_BASE=console not applied (TOUCH as before)", s->off);
            return;
        }
    }
    g_want = 1;
    g_merged = dm_dos2_touchui_pak_file(TB_BIND_MERGED_PATH);
    if ((v = getenv("DM_DOS2_TOUCH_BASE_PIN")) && *v) {
        for (const char *p = v; *p && g_npin < TB_PINS;) {
            char *end; const unsigned long o = strtoul(p, &end, 16);
            if (end == p) break;
            if (o && o < 0xffffffffu) g_pin[g_npin++] = (uint32_t)o;
            p = end; while (*p == ',' || *p == ' ') p++;
        }
    }
    LOG("[base] DM_DOS2_TOUCH_BASE=console: UI mode TOUCH keeps the mode byte 0 (PC UI, every UI / game reader as before); the input layer answers "
        "as in controller mode: the listener filter's two reads main+%#x / main+%#x pinned to 1 after hle_dos2_touch.c is in, [mgr+0x170] held at 1 in TOUCH, "
        "bindings %s, pad pulses real%s", TB_PIN_A, TB_PIN_B, g_merged ? TB_BIND_MERGED_PATH " (keyboard_1.json + controller_1.json)" : TB_BIND_KEYS_PATH,
        g_merged ? "" : " - NO: the package has no " TB_BIND_MERGED_PATH " (an old TouchUI.pak: run setup again), the pad has no bindings, pulses stay keyboard twins");
    if (!g_merged) LOG("【红】[base] %s is not in the touch package: the pad passes the filter but nothing is bound to it (make the package with this build's tools)", TB_BIND_MERGED_PATH);
}

void dm_dos2_touch_base_hook(void);   // main.c host_preload, after dm_dos2_touch_install
void dm_dos2_touch_base_hook(void) {
    int dm_dos2_touch_on(void);
    if (!g_want) return;
    module_t *m = g_main;
    if (!dm_dos2_touch_on()) { LOG("【红】[base] DM_DOS2_TOUCH_BASE=console needs DM_DOS2_TOUCH=1 (the pad poll hook holds [mgr+0x170]): not applied"); g_want = 0; return; }
    const uint32_t a = *(const uint32_t *)(m->base + TB_PIN_A), b = *(const uint32_t *)(m->base + TB_PIN_B);
    const uint32_t *wa = (const uint32_t *)(m->base + TB_PIN_A - 4 * 8), *wb = (const uint32_t *)(m->base + TB_PIN_B - 4 * 8);
    if (!tb_pin_ok(wa, 8, TB_PIN_A - 4 * 8, TB_MODE_PAGE, 4) || !tb_pin_ok(wb, 8, TB_PIN_B - 4 * 8, TB_MODE_PAGE, 4)) {
        LOG("【红】[base] main+%#x / main+%#x are not the filter's mode reads any more: not applied (TOUCH as before)", TB_PIN_A, TB_PIN_B); g_want = 0; return; }
    if (tb_poke(m, TB_PIN_A, tb_pin_word(a))) { LOG("【红】[base] mprotect RW failed: not applied (TOUCH as before)"); g_want = 0; return; }
    if (tb_poke(m, TB_PIN_B, tb_pin_word(b))) {   // one copy alone would deliver the pad in one walk only: put the first back
        tb_poke(m, TB_PIN_A, a); LOG("【红】[base] mprotect RW failed at the second copy: not applied (TOUCH as before)"); g_want = 0; return; }
    g_on = 1;
    LOG("[base] listener filter pinned: main+%#x %#010x -> %#010x, main+%#x %#010x -> %#010x (mov w,#1): pad records reach every listener in every UI mode "
        "(SWITCH / HYBRID: the byte is 1 there, no change)", TB_PIN_A, a, tb_pin_word(a), TB_PIN_B, b, tb_pin_word(b));
    for (int i = 0; i < g_npin; i++) {   // DM_DOS2_TOUCH_BASE_PIN (experiment): more readers answered "controller"
        const uint32_t o = g_pin[i];
        if ((o & 3) || o < TB_TEXT_MIN + 64 || (uint64_t)o + 4 > m->text_end || o == TB_PIN_A || o == TB_PIN_B) { LOG("【红】[base] DM_DOS2_TOUCH_BASE_PIN: main+%#x is not a code offset to pin: skipped", o); continue; }
        const uint32_t *w = (const uint32_t *)(m->base + o - 4 * 16);
        if (!tb_pin_ok(w, 16, (uint64_t)o - 4 * 16, TB_MODE_PAGE, 16)) {
            LOG("【红】[base] DM_DOS2_TOUCH_BASE_PIN: main+%#x (%#010x) is not `ldrb w,[x,#0x918]` behind its adrp to main+%#x: skipped", o, w[16], TB_MODE_PAGE); continue; }
        if (tb_poke(m, o, tb_pin_word(w[16]))) { LOG("【红】[base] DM_DOS2_TOUCH_BASE_PIN: mprotect RW failed at main+%#x", o); continue; }
        LOG("[base] DM_DOS2_TOUCH_BASE_PIN (experiment): main+%#x reads the mode as 1 (controller) in every UI mode", o);
    }
}
#endif
