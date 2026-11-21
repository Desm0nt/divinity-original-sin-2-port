// [DOS2] Pure rules of "base A": UI mode TOUCH on the console INPUT mode of the engine (DM_DOS2_TOUCH_BASE=console, default off;
// hle_dos2_touch_base.c; docs/dos2/TOUCH_MODES_PLAN.md "Основа A: касание на консольном режиме движка"). Kept apart so a host test can check
// them without the game (dm/host/tests/dos2_touch_base_test.c). Static only when written: nothing here ran on a phone.
//
// What "the engine's input mode" is (main.raw 4979b200, tools/dos2_mode_readers.py): the byte main+0x2bb8918 has ~800 readers in ~380
// functions. Exactly TWO of them are the input layer - the two copies of the listener filter in the input manager's dispatch (main+0x8440b0,
// main+0x844248): an event's device gives a class (devices 0 / 1 keyboard / mouse = 0, 3..6 pads = 1, else -1) and a listener gets the event
// when   its flags (+4) have bit 4   or   class 0 and [mgr+0x170] != 0   or   class == the mode byte   or   class -1   or   its mask (+0x68)
// has the class bit. Every other reader is UI / game code that picks its PC or console flavour: the per-player UI set builder, the window
// type choices, the PC classes' own branches, picking, tooltips, key name tables, targeting ... (the things TOUCH is built on).
// So TOUCH keeps the byte 0 for all of those - today's TOUCH for every one of them - and the base only gives the input layer the console
// answer: the two filter reads become `mov w, #1`, and the host holds [mgr+0x170] = 1. Then
//   pad records (class 1 == 1) reach every listener, as in the hybrid: console menus, hint taps as real pad pulses, a physical gamepad;
//   keyboard / mouse records (class 0) reach every listener through [mgr+0x170], exactly the listeners they reach today (class == byte 0).
// In SWITCH / HYBRID the byte is 1 anyway, so the pinned reads change nothing there.
#ifndef HLE_DOS2_TOUCH_BASE_H
#define HLE_DOS2_TOUCH_BASE_H
#include <stdint.h>
#include <string.h>

enum { TB_PC = 0 /* today's TOUCH: the input layer in keyboard-and-mouse mode too */, TB_CONSOLE = 1 /* base A */ };
// DM_DOS2_TOUCH_BASE: "console" / "1" = base A; unset, "pc", "0" or anything else = today's TOUCH (byte for byte)
static inline int tb_parse(const char *s) {
    if (!s) return TB_PC;
    while (*s == ' ' || *s == '\t') s++;
    if (!strncmp(s, "console", 7)) return TB_CONSOLE;
    if (s[0] == '1' && (!s[1] || s[1] == ' ' || s[1] == '\t' || s[1] == '\r' || s[1] == '\n')) return TB_CONSOLE;
    return TB_PC;
}
static inline const char *tb_name(int b) { return b == TB_CONSOLE ? "console" : "pc"; }

// ---- the engine's listener filter, as a model (main+0x84407c..0x8440d0 and its copy main+0x84420c..0x84426c) ----
static inline int tb_dev_class(unsigned dev) { return dev < 2 ? 0 : dev - 3 < 4 ? 1 : -1; }
// lflags = the listener's flags (+4), kbm = [mgr+0x170], mode = what the filter reads as the mode byte, mask = the listener's mask (+0x68)
static inline int tb_filter_pass(unsigned lflags, int cls, int kbm, int mode, unsigned mask) {
    if (lflags & 0x10) return 1;
    if (cls == 0 && kbm) return 1;
    if (cls == mode) return 1;
    if (cls == -1) return 1;
    return (mask & (1u << (cls & 31))) != 0;
}
// what the filter reads as the mode: the byte itself, or 1 once the base pinned the two reads
static inline int tb_filter_mode(int base_on, int mode_byte) { return base_on ? 1 : mode_byte; }
// [mgr+0x170] must be 1 while the TOUCH PC UI runs on the base: the filter no longer sees "class 0 == mode 0", so keyboard / mouse
// records pass only through that byte (the hybrid's own door: DM_DOS2_TOUCH_KBM)
static inline int tb_kbm_hold(int base_on, int touch_pc) { return base_on && touch_pc; }

// ---- bindings ----
// PAD = Eoc/controller_1.json (SWITCH / HYBRID), KEYS = Eoc/keyboard_1.json (today's TOUCH), MERGED = the union of both, made at setup
// from the player's own Engine.pak into TouchUI.pak (tools/dos2_touch_bind.py)
enum { TB_BIND_PAD = 0, TB_BIND_KEYS = 1, TB_BIND_MERGED = 2 };
#define TB_BIND_PAD_PATH    "PlayerProfiles/Eoc/controller_1.json"
#define TB_BIND_KEYS_PATH   "PlayerProfiles/Eoc/keyboard_1.json"
#define TB_BIND_MERGED_PATH "PlayerProfiles/Eoc/dmtouch_1.json"
_Static_assert(sizeof(TB_BIND_KEYS_PATH) <= sizeof(TB_BIND_PAD_PATH) && sizeof(TB_BIND_MERGED_PATH) <= sizeof(TB_BIND_PAD_PATH),
               "a bindings path is written over the engine's controller_1.json string: it must fit");
static inline const char *tb_bind_path(int which) { return which == TB_BIND_MERGED ? TB_BIND_MERGED_PATH : which == TB_BIND_KEYS ? TB_BIND_KEYS_PATH : TB_BIND_PAD_PATH; }
// base = DM_DOS2_TOUCH_BASE wanted and its sites matched; touch_pc = the HUD part runs TOUCH (the PC UI); keys_pref = DM_DOS2_TOUCH_PCUI_KEYS
// (default 1); have_merged = TouchUI.pak holds the merged file. Without the base: exactly the old rule (touch_pc && keys_pref -> KEYS).
static inline int tb_bindings(int base, int touch_pc, int keys_pref, int have_merged) {
    if (!touch_pc || !keys_pref) return TB_BIND_PAD;
    return base && have_merged ? TB_BIND_MERGED : TB_BIND_KEYS;
}
// A pad pulse of the host (hint tap, deferred main menu press) while the TOUCH gestures run: a real npad pulse (1) or its keyboard twin (0).
// Real only when the base is in (the filter lets the pad through) and the pad has bindings (not the keyboard-only file); outside the TOUCH
// gestures a pulse is always real, as before.
static inline int tb_pulse_real(int base_on, int touch_t, int bind) {
    if (!touch_t) return 1;
    return base_on && bind != TB_BIND_KEYS;
}
// TOUCH's live pad switch (DM_DOS2_TOUCH_T_PADSWITCH=1: a pad press rebuilds the controller UI): never on the base - there a pad press is
// plain input, not "the player took the pad"
static inline int tb_padswitch(int base, int env) { return env && !base; }

// ---- pinning a reader of the mode byte to the console answer ----
// ldrb wT, [xN, #0x918] -> mov wT, #1 (movz). The page of xN is the caller's business (hle_dos2_touch_base.c checks the adrp).
static inline int tb_is_mode_read(uint32_t w) { return (w & 0xfffffc00u) == 0x39646000u; }
static inline unsigned tb_read_base_reg(uint32_t w) { return (w >> 5) & 31u; }
static inline uint32_t tb_pin_word(uint32_t w) { return 0x52800020u | (w & 31u); }
// adrp xD, <page>: 1 and the page / register, else 0
static inline int tb_adrp(uint32_t w, uint64_t pc, uint64_t *page, unsigned *rd) {
    if ((w & 0x9f000000u) != 0x90000000u) return 0;
    int64_t imm = (int64_t)((((uint64_t)(w >> 5) & 0x7ffffu) << 2) | ((w >> 29) & 3u));
    if (imm & (1 << 20)) imm -= 1 << 21;
    *page = (pc & ~(uint64_t)0xfff) + (uint64_t)(imm * 4096);
    *rd = w & 31u;
    return 1;
}
// Is words[i] a read of the mode byte whose base register was loaded by an `adrp` to `page` at most `back` instructions earlier, with no
// other instruction in between that names the register as its destination in the low 5 bits (a rough, conservative test: a doubtful
// site is refused, never pinned). pc0 = the address of words[0].
static inline int tb_pin_ok(const uint32_t *words, unsigned i, uint64_t pc0, uint64_t page, unsigned back) {
    if (!tb_is_mode_read(words[i])) return 0;
    const unsigned rn = tb_read_base_reg(words[i]);
    for (unsigned k = 1; k <= back && k <= i; k++) {
        uint64_t pg; unsigned rd;
        const uint32_t w = words[i - k];
        if (tb_adrp(w, pc0 + 4ull * (i - k), &pg, &rd)) { if (rd == rn) return pg == page; continue; }
        if ((w & 31u) == rn) return 0;   // something else may have written the register
    }
    return 0;
}
#endif
