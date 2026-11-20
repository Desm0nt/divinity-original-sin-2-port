// 两个公共入口 + 静态桩表。
// 0) unimpl_stubs：未实现导入的记名桩，第 k 个 = movz x9,#k; b unimpl_entry（静态代码，iOS 不许运行时生成）
// 1) unimpl_entry：未实现导入的桩入口，x9 = 桩号；保存 x0-x8+lr 调 unimpl_c(idx, regs)。
// 2) dm_gate：改写器把 svc / cntpct 读换成「bl 到本入口」。进来时 lr = 门块返回点（gate_dispatch 据此查是哪个门），
//    保存 x0-x30 到栈上连续区，调 gate_dispatch(ret, regs)，回写 x0（门返回值），返回。
// Two common entries + a static stub table.
// 0) unimpl_stubs: named stubs for unimplemented imports, the k-th = movz x9,#k; b unimpl_entry (static code; iOS does not allow runtime generation)
// 1) unimpl_entry: stub entry for unimplemented imports, x9 = stub number; save x0-x8+lr and call unimpl_c(idx, regs).
// 2) dm_gate: the rewriter replaces svc / cntpct reads with "bl to this entry". On entry lr = the gate block return point (gate_dispatch uses it to look up which gate this is),
//    save x0-x30 to a contiguous area on the stack, call gate_dispatch(ret, regs), write back x0 (gate return value), return.
    .text
    .globl _unimpl_stubs
    .p2align 3
_unimpl_stubs:
    .set k, 0
    .rept 4096                // = hle.c MAX_STUB
    movz x9, #k
    b _unimpl_entry
    .set k, k + 1
    .endr

    .globl _unimpl_entry
    .p2align 2
_unimpl_entry:
    stp x29, x30, [sp, #-96]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    str x8, [sp, #80]
    mov x0, x9
    add x1, sp, #16
    bl _unimpl_c
    ldp x29, x30, [sp], #96
    ret

    .globl _dm_gate
    .p2align 2
_dm_gate:
    // 开 256 字节帧，保存 x0..x30（x0..x29 成对，x30 单独），门可读改 x0..x30
    // Open a 256-byte frame and save x0..x30 (x0..x29 in pairs, x30 alone); the gate may read and modify x0..x30
    sub sp, sp, #800
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    stp q0, q1, [sp, #256]
    stp q2, q3, [sp, #288]
    stp q4, q5, [sp, #320]
    stp q6, q7, [sp, #352]
    stp q8, q9, [sp, #384]
    stp q10, q11, [sp, #416]
    stp q12, q13, [sp, #448]
    stp q14, q15, [sp, #480]
    stp q16, q17, [sp, #512]
    stp q18, q19, [sp, #544]
    stp q20, q21, [sp, #576]
    stp q22, q23, [sp, #608]
    stp q24, q25, [sp, #640]
    stp q26, q27, [sp, #672]
    stp q28, q29, [sp, #704]
    stp q30, q31, [sp, #736]
    mrs x9, nzcv
    str x9, [sp, #768]
    mrs x9, fpsr
    str x9, [sp, #776]
    mrs x9, fpcr
    str x9, [sp, #784]
    mov x0, x30              // 返回点 = 门标识 | Return point = gate identifier
    mov x1, sp               // regs
    bl _gate_dispatch
    ldr x9, [sp, #784]
    msr fpcr, x9
    ldr x9, [sp, #776]
    msr fpsr, x9
    ldr x9, [sp, #768]
    msr nzcv, x9
    ldp q0, q1, [sp, #256]
    ldp q2, q3, [sp, #288]
    ldp q4, q5, [sp, #320]
    ldp q6, q7, [sp, #352]
    ldp q8, q9, [sp, #384]
    ldp q10, q11, [sp, #416]
    ldp q12, q13, [sp, #448]
    ldp q14, q15, [sp, #480]
    ldp q16, q17, [sp, #512]
    ldp q18, q19, [sp, #544]
    ldp q20, q21, [sp, #576]
    ldp q22, q23, [sp, #608]
    ldp q24, q25, [sp, #640]
    ldp q26, q27, [sp, #672]
    ldp q28, q29, [sp, #704]
    ldp q30, q31, [sp, #736]
    // 门可改任意通用寄存器：svc 出参在 x0..x8，mrs cntpct 的目标可以是任意 xN（sdk 33 处里 14 处是 x19..x23）
    // ⇒ x0..x29 全部回读（x18 是苹果平台寄存器，客体的 x18 已改写到线程槽，跳过不碰）
    // A gate may modify any general-purpose register: svc out-parameters are in x0..x8, and the target of mrs cntpct can be any xN (14 of the 33 sites in sdk are x19..x23)
    // ⇒ read back all of x0..x29 (x18 is the Apple platform register; the guest's x18 has been rewritten to the thread slot, so skip it and leave it alone)
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldr x19, [sp, #152]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #800
    ret

// [NR-16] DM_NR_CPU (hle_cpuprof.c): one round trip through dm_gate with a gate of its own (kind "probe", registered at
// _dm_gate_probe_ret, nothing to do in gate_dispatch), so the cost of a gate frame (800 bytes saved and restored + the lookup) is
// measured on the phone, not guessed. dm_gate gives back every register it saved, so this is an ordinary void function to C.
    .globl _dm_gate_probe
    .globl _dm_gate_probe_ret
    .p2align 2
_dm_gate_probe:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    bl _dm_gate
_dm_gate_probe_ret:
    ldp x29, x30, [sp], #16
    ret

// [NR-24] DM_NR_GATE_TICK=1 (hle_os.c, docs/dos2/nr/NR-24.md): the gate entry of every guest thread's KGATE slot (hle.c dm_gate_entry).
// It looks the gate up by the return point in x30 in hle.c's table (dm_gate_tab, the same hash as gate_dispatch) using x9..x12 saved in
// a 48-byte frame and, for a clock gate (mrs xN, cntpct_el0 / cntvct_el0, kind 1 / 2) whose destination is x0..x28 but not x18, reads
// cntvct_el0 and maps it with the current tick block (dm_tick_cur: valid, c0, base, mul) to 19.2 MHz guest ticks; every other gate
// (SVCs, observation gates, the probe, a frozen clock, a block changed under the read) restores x9..x12 and branches to _dm_gate with
// sp and every register as they came in. No instruction here writes NZCV or a SIMD register, so nothing else needs saving. The value
// goes to xN through a table of 16-byte stubs (bti j; ldr xN; b; nop) entered with br x29: x29 is the guest's own value, which the gate
// block saved at [sp] before the call (stp x29, x30, [sp, #-16]! ... ldr x29, [sp]), so the tail reloads it from there ([sp, #48]).
// Clamp: a counter value read before the block's anchor c0 (speculated early) gives the block's base, never a wrap.
// Ordering: the re-check load of dm_tick_cur takes an address dependency on the counter value (Linux arch_counter_enforce_ordering),
// so a reader whose re-check still sees its block read the counter before a freeze published the next block.
    .globl _dm_gate_fast
    .p2align 2
_dm_gate_fast:
    stp x9, x10, [sp, #-48]!
    stp x11, x12, [sp, #16]
    lsr x9, x30, #2                    // gate_h: ((ret >> 2) * 0x9E3779B97F4A7C15) >> (64 - 12)
    movz x10, #0x7C15
    movk x10, #0x7F4A, lsl #16
    movk x10, #0x79B9, lsl #32
    movk x10, #0x9E37, lsl #48
    mul x9, x9, x10
    lsr x9, x9, #52
    adrp x10, _dm_gate_tab@PAGE
    add x10, x10, _dm_gate_tab@PAGEOFF
Lfg_probe:
    add x11, x9, x9, lsl #1
    add x11, x10, x11, lsl #3          // &dm_gate_tab[i] (24-byte entries)
    ldr x12, [x11]
    cbz x12, Lfg_slow                  // an empty slot: not a known gate (gate_dispatch reports it)
    eor x12, x12, x30
    cbz x12, Lfg_hit
    add x9, x9, #1
    and x9, x9, #0xfff
    b Lfg_probe
Lfg_hit:
    ldr w12, [x11, #8]                 // kind: G_CNTPCT 1 / G_CNTVCT 2
    sub w12, w12, #1
    and w12, w12, #0xfffffffe
    cbnz w12, Lfg_slow
    ldrsw x9, [x11, #12]               // arg = N of mrs xN
    tbnz x9, #63, Lfg_slow
    sub x12, x9, #29
    tbz x12, #63, Lfg_slow             // x29 / x30 come back from the gate block's frame: the full gate
    sub x12, x9, #18
    cbz x12, Lfg_slow                  // x18 is not the guest's register
    str x9, [sp, #40]
    adrp x10, _dm_gate_prof@PAGE       // [NR-16] DM_NR_CPU=1: count the fast reads (by register)
    ldr x10, [x10, _dm_gate_prof@PAGEOFF]
    cbz x10, 1f
    adrp x10, _dm_tick_fast_n@PAGE
    add x10, x10, _dm_tick_fast_n@PAGEOFF
    ldr x12, [x10, x9, lsl #3]
    add x12, x12, #1
    str x12, [x10, x9, lsl #3]
1:  adrp x10, _dm_tick_cur@PAGE
    add x10, x10, _dm_tick_cur@PAGEOFF
    ldr x11, [x10]                     // the current tick block
    cbz x11, Lfg_slow
    ldr x12, [x11]                     // valid (0 = the guest clock is frozen: the full gate waits / takes the frozen value)
    cbz x12, Lfg_slow
    mrs x12, cntvct_el0
    eor x9, x12, x12
    ldr x9, [x10, x9]                  // dm_tick_cur again, ordered after the counter read
    eor x9, x9, x11
    cbnz x9, Lfg_slow                  // a freeze / thaw published another block meanwhile
    ldr x9, [x11, #8]                  // c0
    sub x12, x12, x9
    tbz x12, #63, 2f
    mov x12, #0
2:  ldr x9, [x11, #24]                 // mul: 0 = the counter runs at 19.2 MHz
    cbz x9, 3f
    mul x10, x12, x9
    umulh x12, x12, x9
    extr x12, x12, x10, #32            // (d * mul) >> 32
3:  ldr x9, [x11, #16]                 // base
    add x12, x12, x9
    str x12, [sp, #32]                 // the value
    ldr x9, [sp, #40]
    adr x29, Lfg_tab
    add x29, x29, x9, lsl #4
    ldp x11, x12, [sp, #16]
    ldp x9, x10, [sp]
    br x29
Lfg_slow:
    ldp x11, x12, [sp, #16]
    ldp x9, x10, [sp], #48
    b _dm_gate
    .p2align 4
Lfg_tab:
    hint #36
    ldr x0, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x1, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x2, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x3, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x4, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x5, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x6, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x7, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x8, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x9, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x10, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x11, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x12, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x13, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x14, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x15, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x16, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x17, [sp, #32]
    b Lfg_tail
    nop
    hint #36                           // x18: never taken (checked above)
    b Lfg_tail
    nop
    nop
    hint #36
    ldr x19, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x20, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x21, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x22, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x23, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x24, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x25, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x26, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x27, [sp, #32]
    b Lfg_tail
    nop
    hint #36
    ldr x28, [sp, #32]
    b Lfg_tail
    nop
Lfg_tail:
    ldr x29, [sp, #48]                 // the guest's x29 from the gate block's frame
    add sp, sp, #48
    ret

// [NR-24] DM_NR_CPU (hle_cpuprof.c): one clock-gate read through the full gate and one through the short entry, each with a gate of
// its own (kind "cntvct", arg 0: the value comes back in x0), so the phone's log states what a clock gate costs either way.
    .globl _dm_gate_probe_clk
    .globl _dm_gate_probe_clk_ret
    .p2align 2
_dm_gate_probe_clk:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    bl _dm_gate
_dm_gate_probe_clk_ret:
    ldp x29, x30, [sp], #16
    ret
    .globl _dm_gate_probe_fast
    .globl _dm_gate_probe_fast_ret
    .p2align 2
_dm_gate_probe_fast:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    bl _dm_gate_fast
_dm_gate_probe_fast_ret:
    ldp x29, x30, [sp], #16
    ret

// 3) dm_thread_enter(entry, arg, sp)：客体线程起点。切到 SDK 给的客体栈，x0 = 参数调入口；
//    Horizon 线程须经 ExitThread 退出，入口返回即报错。
// 3) dm_thread_enter(entry, arg, sp): guest thread start. Switch to the guest stack given by the SDK and call entry with x0 = argument;
//    Horizon threads must exit via ExitThread; returning from entry is reported as an error.
    .globl _dm_thread_enter
    .p2align 2
_dm_thread_enter:
    mov sp, x2
    mov x29, #0
    mov x16, x0
    mov x0, x1
    adr x30, 1f
    br x16
1:  bl _dm_thread_returned
    brk #0

// 4) dm_call_on_stack(sp, fn, arg): [DOOM] issues #12/#20. Switch to stack sp (16-byte aligned down) and run fn(arg) there; fn
//    must not return. hle_thread.c thread_exit uses it to leave the guest thread's small stack for the saved host pthread stack
//    before pthread_exit. x29 = x30 = 0 end the frame chain there (dm_backtrace stops); x16 is the branch register as in
//    dm_thread_enter (BTI-compatible with a "bti c" landing pad). x18 and the other registers are left as they are.
    .globl _dm_call_on_stack
    .p2align 2
_dm_call_on_stack:
    and x0, x0, #0xfffffffffffffff0
    mov sp, x0
    mov x29, #0
    mov x30, #0
    mov x16, x1
    mov x0, x2
    br x16
