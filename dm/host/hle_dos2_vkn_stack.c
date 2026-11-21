// [DOS2] VK-NATIVE: the host stack of every backend entry (DM_VK_NATIVE 1 and 2).
//
// Why. A hook of hle_dos2_vkn*.c is reached by a jump written over the first 16 bytes of an engine function (dn_hook_chain), so it
// starts on the stack of the GUEST thread that called the engine function: a stack the game sized for the Switch. From there the
// backend calls Vulkan (pipeline compilation on a draw-time miss, descriptor updates, recording, vkQueueSubmit, the presenter's
// dm_present_async) and its own deep code (texture deswizzle / BC decode, SPIR-V variants, vsnprintf of the log). The old path never
// did that: an NVN call enters the host through the slot thunk (nvn_thunks.S) and _nvn_bigstack_entry / _nvn_lean_entry
// (nvn_entry.S), which move sp to the thread's 1 MiB host stack (nvn_bigstack_top, hle_nvn.c: one mmap per thread, a 16 KiB guard
// page under it) before any HLE body runs; the earlier DM_NR hooks did only cache bookkeeping on the guest stack (a few hundred bytes)
// and reached NVN bodies / the translator through nvn_on_bigstack (NR-23 no-call, NR-09S state, NR-09 direct draws).
//
// Numbers (main.raw of update v786432, the import nn::os::CreateThread at stubs 0x1bf4a00 / 0x1bf5b10, the 5th argument = size):
// fixed guest stacks of 16 KiB (0x5074dc, 0xbd3bd4), 32 KiB (0xbd4e5c) and 96 KiB (0xbdfc64); every other thread takes its size at
// run time (the engine's thread class 0x7006b0: object +0x48; 0x37e6e0, 0x4c4294, 0x4c4cdc, 0x542314, 0x5717f8, 0x64f424, 0x8062c8,
// 0xbee73c from their callers) - so the smallest stack a hook can be entered on is 16 KiB and the others are not known statically,
// while a Vulkan driver's pipeline compile is not bounded at all (Android gives its own threads 1 MiB for that reason). The cost on the
// guest stack is now the same as the old path's NVN call: the veneer's 0x110-byte frame + dm_vkn_hs_enter / nvn_bigstack_top (a
// TLS descriptor call; their frames are small). The host stack is the SAME 1 MiB stack the NVN entry uses, so the two nest.
//
// How. dm_dos2_vkn_install (hle_dos2_vkn.c) puts dm_vkn_stack_stub(hook) instead of the hook into every site; the parts use it for
// their other entries (the game's NVN table slots of hle_dos2_vkn_cmd.c, the GDraw table slot of hle_dos2_vkn_ui.c) and
// dm_vkn_on_stack for the packet handlers (they are called by hle_dos2_native.c's ExecuteRenderCommand hook, on the guest stack).
//   stub (32 bytes, one per hook, made at install time): x17 = hook, branch to dm_vkn_hs_veneer.
//   veneer: a 0x110-byte frame on the caller's stack {x29, x30, x0-x7, x8, x17, q0-q7, the TLS slot, the previous caller};
//           dm_vkn_hs_enter(sp) = nvn_bigstack_top(sp) (the top of this thread's host stack, or 0 when sp is on it already: a hook
//           whose engine original reached another hooked function, or an NVN callback) + the address of this thread's caller slot;
//           the caller slot gets the guest x30 (dm_vkn_caller), the previous value is kept in the frame (nesting);
//           new sp = top (or this frame) - 0x60, the 12 stack argument slots copied there (VKN_A reads them and passes them on to
//           `orig`), every argument register reloaded, blr hook; back: the caller slot restored, sp = the frame, return with x0 /
//           x1 / q0-q3 as the hook left them (only x9 / x10 are touched after the call).
// The hook's `orig` (the engine function's trampoline, or an earlier hook of ours) then runs on the host stack too: engine code on
// the host stack is what the NVN path already did for callbacks; nothing in it may depend on sp being inside its guest stack (the one
// such SDK call, nn::os::GetCurrentStackInfo, has a single caller in main: 0x64fa50, a thread registration, not under a renderer hook).
// Frame chain: x29 of the veneer frame (on the guest stack) is the hook's parent frame, so a frame-pointer walk goes host -> guest
// frames as with the lean NVN entry. Not carried, as before: x8 to the hook (restored for it, not an argument of VKN_A), v0-v7 upper
// halves beyond q (all 128 bits are carried). Other targets than aarch64 ELF: no stub (the hook itself), dm_vkn_caller = 0.
//
// [stage 5.5] The default sync of the recording thread (dm/gfx/vkn/vkn_rt.h, docs/dos2/nr/VK-STAGE55-THREAD.md section 3). A stub's
// literal is a descriptor {hook, flags, sync site} instead of the hook. Unless the hook is flagged DM_VKN_SF_ENC ("encodes its backend
// work as records, or does only engine-side work"), the veneer calls dm_vkn_hs_pre(desc) on the host stack before the hook
// (vkn_rt_own_begin: on the engine thread, wait until the recording thread executed everything written, then the recording is this
// thread's for the call) and dm_vkn_hs_post(desc) after it (vkn_rt_own_end; x0 / x1 / q0-q3 kept). A flagged hook costs one load and one
// bit test more than before. dm_vkn_on_stack does the same for the packet handlers (dm_vkn_on_stack_x: a site name and the flag).
// Flags come from dm_vkn_stack_flag (the parts' registration, before the install makes the stubs) or dm_vkn_stack_stub_ex.
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <string.h>
#include <sys/mman.h>
#include "../gfx/vkn/vkn.h"
#include "../gfx/vkn/vkn_rt.h"
#include "hle_dos2_vkn_stack.h"

void *nvn_bigstack_top(uintptr_t sp);                       // gfx/nvn/hle_nvn.c: top of this thread's 1 MiB host stack, NULL = on it
void *nvn_on_bigstack(void *(*fn)(void *), void *arg);      // gfx/nvn/nvn_entry.S: fn(arg) on that stack (in place when on it)

static __thread uint64_t t_caller;                           // the guest x30 of the innermost stub call (0 = none)
static uint64_t g_hs[3];

typedef struct { uint64_t top; uint64_t *slot; } vkn_hs_ret;   // x0 / x1 (AAPCS64: a 16-byte composite comes back in two registers)
vkn_hs_ret dm_vkn_hs_enter(uint64_t sp) __attribute__((visibility("hidden"), used));
vkn_hs_ret dm_vkn_hs_enter(uint64_t sp) {
    vkn_hs_ret r;
    r.top = (uint64_t)(uintptr_t)nvn_bigstack_top((uintptr_t)sp);
    r.slot = &t_caller;
    __atomic_add_fetch(&g_hs[r.top ? 0 : 1], 1, __ATOMIC_RELAXED);
    return r;
}
uint64_t dm_vkn_caller(void) { return t_caller; }
void dm_vkn_stack_stats(uint64_t out[3]) { for (int i = 0; i < 3; i++) out[i] = __atomic_load_n(&g_hs[i], __ATOMIC_RELAXED); }

// ---- [stage 5.5] stub descriptors, flags, the default sync ----
typedef struct vkn_stub_desc { void *hook; uint32_t flags, pad; vkn_rt_site site; } vkn_stub_desc;   // hook at +0, flags at +8 (the veneer)
enum { DESCS = 512, FLAGS = 256 };
static vkn_stub_desc g_desc[DESCS]; static uint32_t g_ndesc;
static struct { void *hook; unsigned flags; } g_sflag[FLAGS]; static uint32_t g_nsflag;
void dm_vkn_stack_flag(void *hook, unsigned flags) {
    for (uint32_t i = 0; i < g_nsflag; i++) if (g_sflag[i].hook == hook) { g_sflag[i].flags = flags; return; }
    if (g_nsflag < FLAGS) { g_sflag[g_nsflag].hook = hook; g_sflag[g_nsflag].flags = flags; g_nsflag++; }
    else vkn_logf("\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91stack: flag table full: a hook keeps the default sync");
}
static unsigned flags_of(void *hook) { for (uint32_t i = 0; i < g_nsflag; i++) if (g_sflag[i].hook == hook) return g_sflag[i].flags; return 0; }
void dm_vkn_hs_pre(vkn_stub_desc *d) __attribute__((visibility("hidden"), used));
void dm_vkn_hs_post(vkn_stub_desc *d) __attribute__((visibility("hidden"), used));
void dm_vkn_hs_pre(vkn_stub_desc *d) { vkn_rt_own_begin(&d->site); }
void dm_vkn_hs_post(vkn_stub_desc *d) { (void)d; vkn_rt_own_end(); }

typedef struct { uint64_t (*fn)(void *); void *ctx; uint64_t rc; vkn_rt_site *site; unsigned flags; } vkn_os_job;
static void *on_stack_job(void *p) {
    vkn_os_job *j = p;
    const int own = !(j->flags & DM_VKN_SF_ENC);
    if (own) vkn_rt_own_begin(j->site);   // [stage 5.5] the default sync (on the host stack)
    j->rc = j->fn(j->ctx);
    if (own) vkn_rt_own_end();
    return NULL;
}
static vkn_rt_site g_os_site = VKN_RT_SITE("packet handler");
uint64_t dm_vkn_on_stack_x(uint64_t (*fn)(void *ctx), void *ctx, vkn_rt_site *site, unsigned flags) {
    __atomic_add_fetch(&g_hs[2], 1, __ATOMIC_RELAXED);
    vkn_os_job j = {fn, ctx, 0, site ? site : &g_os_site, flags};
    nvn_on_bigstack(on_stack_job, &j);
    return j.rc;
}
uint64_t dm_vkn_on_stack(uint64_t (*fn)(void *ctx), void *ctx) { return dm_vkn_on_stack_x(fn, ctx, NULL, 0); }

#if defined(__aarch64__) && defined(__ELF__)
void dm_vkn_hs_veneer(void) __attribute__((visibility("hidden")));
__asm__(
    "    .text\n"
    "    .p2align 4\n"
    "    .globl dm_vkn_hs_veneer\n"
    "    .hidden dm_vkn_hs_veneer\n"
    "    .type dm_vkn_hs_veneer, %function\n"
    "dm_vkn_hs_veneer:\n"                  // x17 = the stub's descriptor {hook, flags, site}; x0-x7 / q0-q7 / [sp, #0..#0x5f] = the call's arguments
    "    hint #34\n"                        // bti c (the stub enters with br x16)
    "    sub sp, sp, #0x110\n"
    "    stp x29, x30, [sp]\n"
    "    mov x29, sp\n"
    "    stp x0, x1, [sp, #0x10]\n"
    "    stp x2, x3, [sp, #0x20]\n"
    "    stp x4, x5, [sp, #0x30]\n"
    "    stp x6, x7, [sp, #0x40]\n"
    "    stp x8, x17, [sp, #0x50]\n"
    "    stp q0, q1, [sp, #0x60]\n"
    "    stp q2, q3, [sp, #0x80]\n"
    "    stp q4, q5, [sp, #0xa0]\n"
    "    stp q6, q7, [sp, #0xc0]\n"
    "    mov x0, sp\n"
    "    bl dm_vkn_hs_enter\n"              // x0 = host stack top or 0 (on it already), x1 = &t_caller
    "    ldr x9, [x1]\n"
    "    stp x1, x9, [x29, #0xe0]\n"        // the slot and the enclosing call's caller
    "    ldr x10, [x29, #8]\n"
    "    str x10, [x1]\n"                   // t_caller = the guest x30
    "    cbnz x0, 1f\n"
    "    mov x0, x29\n"                     // in place: below this frame
    "1:  sub x9, x0, #0x60\n"
    "    add x10, x29, #0x110\n"            // the caller's stack arguments
    "    ldp x11, x12, [x10, #0x00]\n"
    "    stp x11, x12, [x9, #0x00]\n"
    "    ldp x11, x12, [x10, #0x10]\n"
    "    stp x11, x12, [x9, #0x10]\n"
    "    ldp x11, x12, [x10, #0x20]\n"
    "    stp x11, x12, [x9, #0x20]\n"
    "    ldp x11, x12, [x10, #0x30]\n"
    "    stp x11, x12, [x9, #0x30]\n"
    "    ldp x11, x12, [x10, #0x40]\n"
    "    stp x11, x12, [x9, #0x40]\n"
    "    ldp x11, x12, [x10, #0x50]\n"
    "    stp x11, x12, [x9, #0x50]\n"
    "    mov sp, x9\n"
    "    ldr x17, [x29, #0x58]\n"           // [stage 5.5] the descriptor: flags bit 0 = DM_VKN_SF_ENC (no default sync)
    "    ldr w10, [x17, #8]\n"
    "    tbnz w10, #0, 2f\n"
    "    mov x0, x17\n"
    "    bl dm_vkn_hs_pre\n"                // (below sp: the copied stack arguments stay; every argument register is reloaded next)
    "2:  ldp x0, x1, [x29, #0x10]\n"
    "    ldp x2, x3, [x29, #0x20]\n"
    "    ldp x4, x5, [x29, #0x30]\n"
    "    ldp x6, x7, [x29, #0x40]\n"
    "    ldp x8, x17, [x29, #0x50]\n"
    "    ldp q0, q1, [x29, #0x60]\n"
    "    ldp q2, q3, [x29, #0x80]\n"
    "    ldp q4, q5, [x29, #0xa0]\n"
    "    ldp q6, q7, [x29, #0xc0]\n"
    "    ldr x17, [x17]\n"                  // the hook
    "    blr x17\n"
    "    ldr x9, [x29, #0x58]\n"
    "    ldr w10, [x9, #8]\n"
    "    tbnz w10, #0, 3f\n"
    "    stp x0, x1, [x29, #0x10]\n"        // the hook's results (x0 / x1 / q0-q3) across dm_vkn_hs_post
    "    stp q0, q1, [x29, #0x60]\n"
    "    stp q2, q3, [x29, #0x80]\n"
    "    mov x0, x9\n"
    "    bl dm_vkn_hs_post\n"
    "    ldp x0, x1, [x29, #0x10]\n"
    "    ldp q0, q1, [x29, #0x60]\n"
    "    ldp q2, q3, [x29, #0x80]\n"
    "3:  ldp x9, x10, [x29, #0xe0]\n"       // (x0 / x1 / q0-q3 as the hook left them: untouched from here on)
    "    str x10, [x9]\n"                   // t_caller back to the enclosing call's
    "    mov sp, x29\n"
    "    ldp x29, x30, [sp]\n"
    "    add sp, sp, #0x110\n"
    "    ret\n"
    "    .size dm_vkn_hs_veneer, . - dm_vkn_hs_veneer\n");

// Stubs: 8 words each in pages of their own (RW while written, then RX; install time only).
enum { STUB_WORDS = 8, STUB_PAGE = 0x4000 };
static uint32_t *g_page; static uint32_t g_used;
void *dm_vkn_stack_stub_ex(void *hook, const char *name, unsigned flags) {
    if (!hook) return NULL;
    if (g_ndesc >= DESCS) { vkn_logf("\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91stack: descriptor table full: %s runs on the guest stack", name ? name : "a hook"); return NULL; }
    vkn_stub_desc *dsc = &g_desc[g_ndesc++];
    dsc->hook = hook; dsc->flags = flags | flags_of(hook); dsc->site.name = name ? name : "unnamed stub";
    if (!g_page || g_used + STUB_WORDS > STUB_PAGE / 4) {
        void *p = mmap(NULL, STUB_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { vkn_logf("\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91stack: no page for the host-stack stubs: hooks run on the guest stack"); return NULL; }
        g_page = p; g_used = 0;
    } else if (mprotect(g_page, STUB_PAGE, PROT_READ | PROT_WRITE)) return NULL;
    uint32_t *s = g_page + g_used;
    const uint64_t h = (uint64_t)(uintptr_t)dsc, v = (uint64_t)(uintptr_t)dm_vkn_hs_veneer;
    s[0] = 0x58000091;   // ldr x17, #16 (the descriptor: [stage 5.5])
    s[1] = 0x580000b0;   // ldr x16, #20 (the veneer)
    s[2] = 0xd61f0200;   // br x16
    s[3] = 0xd503201f;   // nop (the literals 8-aligned)
    s[4] = (uint32_t)h; s[5] = (uint32_t)(h >> 32); s[6] = (uint32_t)v; s[7] = (uint32_t)(v >> 32);
    g_used += STUB_WORDS;
    if (mprotect(g_page, STUB_PAGE, PROT_READ | PROT_EXEC)) die("[vkn] stack stubs: mprotect RX");
    __builtin___clear_cache((char *)s, (char *)(s + STUB_WORDS));
    return s;
}
void *dm_vkn_stack_stub(void *hook) { return dm_vkn_stack_stub_ex(hook, NULL, 0); }
#else
void *dm_vkn_stack_stub(void *hook) { return hook; }   // no veneer on this target: the hook on the caller's stack
void *dm_vkn_stack_stub_ex(void *hook, const char *name, unsigned flags) { (void)name; (void)flags; return hook; }
#endif
#endif
