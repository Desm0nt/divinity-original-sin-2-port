// [DOS2] Host test of the base A rules (hle_dos2_touch_base.h; docs/dos2/TOUCH_MODES_PLAN.md "Основа A: касание на консольном режиме движка"):
// DM_DOS2_TOUCH_BASE parsing, the engine's listener filter as a model (who gets keyboard / mouse / pad records in each UI mode with and
// without the base), the bindings file choice, real pad pulses vs keyboard twins, the [mgr+0x170] hold, the pin of a mode byte read.
// Run with any C compiler: cc dos2_touch_base_test.c -o <outside the repo>; run it.
#include "../hle_dos2_touch_base.h"
#include <assert.h>
#include <stdio.h>
#ifdef NDEBUG
#error "Build this test with assertions enabled"
#endif
int main(void) {
    // parsing: only "console" / "1" turn the base on; unset and anything else = today's TOUCH
    assert(tb_parse(NULL) == TB_PC && tb_parse("") == TB_PC && tb_parse("pc") == TB_PC && tb_parse("0") == TB_PC && tb_parse("2") == TB_PC && tb_parse("10") == TB_PC);
    assert(tb_parse("console") == TB_CONSOLE && tb_parse(" console") == TB_CONSOLE && tb_parse("1") == TB_CONSOLE && tb_parse("1\r\n") == TB_CONSOLE && tb_parse("consol") == TB_PC);
    assert(!strcmp(tb_name(TB_CONSOLE), "console") && !strcmp(tb_name(TB_PC), "pc"));

    // device classes of the filter (main+0x844080..0x844098): keyboard 0 / mouse 1 -> 0, pads 3..6 -> 1, anything else -> -1
    assert(tb_dev_class(0) == 0 && tb_dev_class(1) == 0 && tb_dev_class(2) == -1 && tb_dev_class(3) == 1 && tb_dev_class(6) == 1 && tb_dev_class(7) == -1 && tb_dev_class(0xc) == -1);

    // the filter itself. A plain listener: no bit 4, no mask
    // HYBRID (byte 1, [mgr+0x170] held at 1 by the routing): everything passes
    assert(tb_filter_pass(0, 0, 1, 1, 0) && tb_filter_pass(0, 1, 1, 1, 0));
    // SWITCH-like (byte 1, [mgr+0x170] = 0): the pad passes, keyboard / mouse do not
    assert(!tb_filter_pass(0, 0, 0, 1, 0) && tb_filter_pass(0, 1, 0, 1, 0));
    // today's TOUCH (byte 0): keyboard / mouse pass whatever [mgr+0x170] is, the PAD DOES NOT - hint taps needed keyboard twins
    assert(tb_filter_pass(0, 0, 0, 0, 0) && tb_filter_pass(0, 0, 1, 0, 0) && !tb_filter_pass(0, 1, 0, 0, 0) && !tb_filter_pass(0, 1, 1, 0, 0));
    // listeners that ask: bit 4 = everything, a mask = its classes; class -1 always passes
    assert(tb_filter_pass(0x10, 1, 0, 0, 0) && tb_filter_pass(0, 1, 0, 0, 2) && !tb_filter_pass(0, 1, 0, 0, 1) && tb_filter_pass(0, -1, 0, 0, 0) && tb_filter_pass(0, -1, 0, 1, 0));
    // base A: the filter reads the mode as 1 whatever the byte is
    assert(tb_filter_mode(0, 0) == 0 && tb_filter_mode(0, 1) == 1 && tb_filter_mode(1, 0) == 1 && tb_filter_mode(1, 1) == 1);
    // ... and the host holds [mgr+0x170] while the TOUCH PC UI is up - only then
    assert(tb_kbm_hold(1, 1) && !tb_kbm_hold(1, 0) && !tb_kbm_hold(0, 1) && !tb_kbm_hold(0, 0));
    // THE claim of the base, for every kind of listener (bit 4 or not, every mask): in TOUCH on the base
    //   keyboard / mouse records reach exactly the listeners they reach in today's TOUCH (all of them),
    //   pad records reach exactly the listeners they reach in the hybrid (all of them),
    // and SWITCH / HYBRID (byte 1) do not change at all with the pinned reads, whatever [mgr+0x170] is.
    for (unsigned fl = 0; fl <= 0x10; fl += 0x10) for (unsigned mask = 0; mask < 4; mask++) {
        const int kbm_a = tb_kbm_hold(1, 1), mode_a = tb_filter_mode(1, 0);
        for (int kbm_old = 0; kbm_old < 2; kbm_old++)
            assert(tb_filter_pass(fl, 0, kbm_a, mode_a, mask) == tb_filter_pass(fl, 0, kbm_old, 0, mask) && tb_filter_pass(fl, 0, kbm_a, mode_a, mask) == 1);
        assert(tb_filter_pass(fl, 1, kbm_a, mode_a, mask) == tb_filter_pass(fl, 1, 1, 1, mask) && tb_filter_pass(fl, 1, kbm_a, mode_a, mask) == 1);
        assert(tb_filter_pass(fl, -1, kbm_a, mode_a, mask) == 1);
        for (int kbm = 0; kbm < 2; kbm++) for (int cls = -1; cls <= 1; cls++)
            assert(tb_filter_pass(fl, cls, kbm, tb_filter_mode(1, 1), mask) == tb_filter_pass(fl, cls, kbm, tb_filter_mode(0, 1), mask));
    }
    // why the hold matters: on the base WITHOUT [mgr+0x170] a plain listener would lose the keyboard and the mouse
    assert(!tb_filter_pass(0, 0, 0, tb_filter_mode(1, 0), 0));

    // bindings: SWITCH / HYBRID = the pad's file; TOUCH = keyboard_1.json as before; TOUCH on the base = the merged file when the package has it
    assert(tb_bindings(0, 0, 1, 0) == TB_BIND_PAD && tb_bindings(1, 0, 1, 1) == TB_BIND_PAD);
    assert(tb_bindings(0, 1, 1, 0) == TB_BIND_KEYS && tb_bindings(0, 1, 1, 1) == TB_BIND_KEYS);          // base off: the merged file is never used
    assert(tb_bindings(1, 1, 1, 1) == TB_BIND_MERGED && tb_bindings(1, 1, 1, 0) == TB_BIND_KEYS);        // an old package: keyboard_1.json, as before
    assert(tb_bindings(0, 1, 0, 1) == TB_BIND_PAD && tb_bindings(1, 1, 0, 1) == TB_BIND_PAD);            // DM_DOS2_TOUCH_PCUI_KEYS=0
    for (int pc = 0; pc < 2; pc++) for (int kp = 0; kp < 2; kp++) for (int hm = 0; hm < 2; hm++)        // base off = the old rule `pc && keys_pref`
        assert(tb_bindings(0, pc, kp, hm) == (pc && kp));
    assert(!strcmp(tb_bind_path(TB_BIND_PAD), "PlayerProfiles/Eoc/controller_1.json") && !strcmp(tb_bind_path(TB_BIND_KEYS), "PlayerProfiles/Eoc/keyboard_1.json"));
    assert(!strcmp(tb_bind_path(TB_BIND_MERGED), "PlayerProfiles/Eoc/dmtouch_1.json") && strlen(tb_bind_path(TB_BIND_MERGED)) <= strlen(tb_bind_path(TB_BIND_PAD)));

    // pad pulses of the host: outside the TOUCH gestures always real; in TOUCH twins as before; on the base real when the pad is bound
    assert(tb_pulse_real(0, 0, TB_BIND_PAD) && tb_pulse_real(1, 0, TB_BIND_PAD) && tb_pulse_real(0, 0, TB_BIND_KEYS));
    assert(!tb_pulse_real(0, 1, TB_BIND_KEYS) && !tb_pulse_real(0, 1, TB_BIND_PAD) && !tb_pulse_real(0, 1, TB_BIND_MERGED));   // base off: twins, whatever the file
    assert(tb_pulse_real(1, 1, TB_BIND_MERGED) && tb_pulse_real(1, 1, TB_BIND_PAD) && !tb_pulse_real(1, 1, TB_BIND_KEYS));     // an old package: the pad is unbound -> twins
    // TOUCH's live pad switch never runs on the base
    assert(tb_padswitch(0, 1) && !tb_padswitch(0, 0) && !tb_padswitch(1, 1) && !tb_padswitch(1, 0));

    // pinning a read: the filter's two words (main+0x8440b0, main+0x844248)
    assert(tb_is_mode_read(0x3964616bu) && tb_is_mode_read(0x3964614au) && !tb_is_mode_read(0x3945c2abu /* ldrb w11,[x21,#0x170] */) && !tb_is_mode_read(0x39246000u /* a store */));
    assert(tb_read_base_reg(0x3964616bu) == 11 && tb_read_base_reg(0x3964614au) == 10);
    assert(tb_pin_word(0x3964616bu) == 0x5280002bu /* mov w11,#1 */ && tb_pin_word(0x3964614au) == 0x5280002au /* mov w10,#1 */);
    assert(tb_pin_word(0x39646118u /* ldrb w24,[x8,#0x918]: the UI set builder */) == 0x52800038u);
    { uint64_t pg = 0; unsigned rd = 99;
      assert(tb_adrp(0x90011babu, 0x8440ac, &pg, &rd) && pg == 0x2bb8000 && rd == 11);
      assert(tb_adrp(0x90011baau, 0x844244, &pg, &rd) && pg == 0x2bb8000 && rd == 10);
      assert(!tb_adrp(0x3964616bu, 0x8440b0, &pg, &rd)); }
    {   // copy A as it is in main.raw, from 0x844090
        const uint32_t w[9] = {0x54002442, 0x320003ea, 0xb940051c, 0x372001dc, 0x7100015f, 0x7a400964, 0x54000161, 0x90011bab, 0x3964616b};
        assert(tb_pin_ok(w, 8, 0x844090, 0x2bb8000, 4));
        assert(!tb_pin_ok(w, 8, 0x844090, 0x2bb9000, 4));    // another page
        assert(!tb_pin_ok(w, 7, 0x844090, 0x2bb8000, 4));    // not a read of the byte
        assert(!tb_pin_ok(w, 8, 0x844090 + 0x1000, 0x2bb8000, 4));   // the same words elsewhere: the adrp gives another page
        // a register copied or rewritten between the adrp and the read is refused
        const uint32_t x[4] = {0x90011bab, 0xaa0803eb /* mov x11,x8 */, 0x3964616b, 0};
        assert(!tb_pin_ok(x, 2, 0x8440ac, 0x2bb8000, 4));
        const uint32_t y[3] = {0x90011ba8 /* adrp x8 */, 0xd503201f /* nop */, 0x3964616b /* reads through x11 */};
        assert(!tb_pin_ok(y, 2, 0x8440a8, 0x2bb8000, 2));
        const uint32_t z[4] = {0x90011bab, 0xd503201f, 0xd503201f, 0x3964616b};
        assert(tb_pin_ok(z, 3, 0x8440a4, 0x2bb8000, 4) && !tb_pin_ok(z, 3, 0x8440a4, 0x2bb8000, 2));   // the look-back is bounded
    }
    printf("dos2_touch_base_test: PASS\n");
    return 0;
}
