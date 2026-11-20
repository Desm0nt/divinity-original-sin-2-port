// [DOS2] VK-NATIVE front end (DM_VK_NATIVE, default 0): the hooks that put the native Vulkan backend (dm/gfx/vkn) behind the engine's
// renderer. main = main.raw of update v786432, build 4979b200; r = the renderer ([main+0x2bbdc68]). This file (and its parts hle_dos2_vkn_*.c)
// is the ONLY place of the backend that knows game addresses and engine record offsets; dm/gfx/vkn gets plain arguments.
// Map of the interface: docs/dos2/nr/VK-NATIVE-MAP-1..3.md; design: docs/dos2/nr/VK-NATIVE.md.
//   DM_VK_NATIVE=0 / unset  nothing: no hook, no allocation, no Vulkan call.
//   DM_VK_NATIVE=2          "alongside": the old path draws; every hook calls the function it sits on and then gives the same event
//                           to the backend (objects created next to the old path's; a census line every 10 s).
//   DM_VK_NATIVE=1          native: the backend owns what it covers (a hook may replace its function).
// Hooks go in with dn_hook_chain (hle_dos2_native.c): a site another module of ours hooked already (DM_NR ...) is chained - this
// file's hook runs first and reaches the function through the earlier hook. So this installer runs AFTER every other installer.
// Safe-mode step 2 sets DM_VK_NATIVE=0 (dm/gfx/vulkan/engine/xbg_safe_mode.h).
#ifdef DM_GAME_DOS2
#include "hle.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gfx/vkn/vkn.h"
#include "hle_dos2_vkn.h"

module_t *dn_main(void);
int dn_hook_chain(module_t *m, uint32_t off, const uint32_t pro[4], void *hook, void **orig);
extern void dm_vk_queue_force(int on) __attribute__((weak));   // android/vulkan_present.cpp: the queue lock is real from now on

static module_t *g_vkn_main;
uint8_t *dm_vkn_base(void) { return g_vkn_main ? g_vkn_main->base : NULL; }

// ---- sites: the parts register their hooks here (install time) ----
enum { VKN_SITES_MAX = 160 };
static vkn_site g_site[VKN_SITES_MAX]; static int g_nsite;
void dm_vkn_site(uint32_t off, const uint32_t pro[4], const char *name, void *hook, vkn_fn *orig, int modes) {
    if (g_nsite >= VKN_SITES_MAX) { vkn_logf("\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91site table full: %s not hooked", name); return; }
    vkn_site *s = &g_site[g_nsite++];
    s->off = off; memcpy(s->pro, pro, sizeof s->pro); s->name = name; s->hook = hook; s->orig = orig; s->modes = modes;
}

// Stage 3 parts (weak: absent until built). With the frame part in, mode 1's EndFrame is the frame part's own function and it
// calls dm_vkn_tick itself; this file's EndFrame hook then serves mode 2 only.
void dm_vkn_frame_register(void) __attribute__((weak));    // hle_dos2_vkn_frame.c: recording, submit, present
void dm_vkn_pass_register(void) __attribute__((weak));     // hle_dos2_vkn_pass.c: targets, clears, copies
void dm_vkn_exec_register(void) __attribute__((weak));     // hle_dos2_vkn_exec.c: packets and state functions with NVN inside
void dm_vkn_draw_register(void) __attribute__((weak));     // hle_dos2_vkn_draw.c: constant / texture flushes, draws
void dm_vkn_imm_register(void) __attribute__((weak));      // hle_dos2_vkn_imm.c: immediate draws
void dm_vkn_ui_register(void) __attribute__((weak));       // hle_dos2_vkn_ui.c: the UI entries
// ---- the frame tick: once a presented frame. Census every 10 s; `own` = nobody records frames (mode 2, or mode 1 without the
// frame part): then frames advance here (deferred destruction) and queued uploads are flushed by an own submission. ----
static vkn_fn g_o_endframe; static uint64_t g_census_t;
static vkn_census_fn g_census[16]; static int g_ncensus;
void dm_vkn_census(vkn_census_fn fn) { if (g_ncensus < 16) g_census[g_ncensus++] = fn; }
void dm_vkn_tick(void) {
    const uint64_t now = vkn_now_ns();
    const int own = vkn_mode() == VKN_ALONGSIDE || !dm_vkn_frame_register;
    if (own) { vkn_frame_end(); if (vkn_upload_pending_bytes() > (64ull << 20)) vkn_upload_flush(); }
    if (now - g_census_t < 10000000000ull) return;
    g_census_t = now;
    if (own) vkn_upload_flush();
    uint64_t used[VKN_ARENAS], res[VKN_ARENAS]; vkn_arena_stats(used, res);
    vkn_logf("mode %d frame %llu: arenas static %.1f / %.1f MB, staging %.1f / %.1f MB, ring %.1f / %.1f MB (used / reserved)", vkn_mode(), (unsigned long long)vkn_frame(),
             used[VKN_ARENA_STATIC] / 1048576.0, res[VKN_ARENA_STATIC] / 1048576.0, used[VKN_ARENA_STAGING] / 1048576.0, res[VKN_ARENA_STAGING] / 1048576.0,
             used[VKN_ARENA_RING] / 1048576.0, res[VKN_ARENA_RING] / 1048576.0);
    for (int i = 0; i < g_ncensus; i++) g_census[i]();
}
VKN_HOOK vkn_h_endframe(VKN_A) {   // window slot 8 = 0x93ccd0 EndFrame(window, bit0 present)
    const uint64_t rc = g_o_endframe(VKN_F);
    if (a1 & 1) dm_vkn_tick();
    return rc;
}

// The parts (each registers its sites and its census line; a part that is not built yet is simply absent: weak).
void dm_vkn_buf_register(void) __attribute__((weak));      // hle_dos2_vkn_buf.c: vertex / index buffers, constant ring
void dm_vkn_tex_register(void) __attribute__((weak));      // hle_dos2_vkn_tex.c: textures, targets, samplers
void dm_vkn_shader_register(void) __attribute__((weak));   // hle_dos2_vkn_shader.c: shaders
void dm_vkn_state_register(void) __attribute__((weak));    // hle_dos2_vkn_state.c: state objects, vertex formats

void dm_dos2_vkn_install(void);   // main.c host_preload, after every other dm_dos2_*_install (before any guest code runs)
void dm_dos2_vkn_install(void) {
    const char *e = getenv("DM_VK_NATIVE");
    const int mode = e ? atoi(e) : 0;
    if (mode != VKN_NATIVE && mode != VKN_ALONGSIDE) return;   // default: nothing
    module_t *m = dn_main();
    if (!m) { vkn_logf("\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91no main module: DM_VK_NATIVE off"); return; }
    g_vkn_main = m;
    vkn_set_mode(mode);   // before the first hook goes in (they read it); guest code is not running yet
    if (dm_vk_queue_force) dm_vk_queue_force(1);
    static const uint32_t k_endframe[4] = {0xf81c0ff7 /* str x23,[sp,#-0x40]! */, 0xa90157f6, 0xa9024ff4, 0xa9037bfd};
    dm_vkn_site(0x93ccd0, k_endframe, "EndFrame", (void *)vkn_h_endframe, &g_o_endframe, dm_vkn_frame_register ? VKN_M_ALONGSIDE : VKN_M_BOTH);
    if (dm_vkn_buf_register) dm_vkn_buf_register();
    if (dm_vkn_tex_register) dm_vkn_tex_register();
    if (dm_vkn_shader_register) dm_vkn_shader_register();
    if (dm_vkn_state_register) dm_vkn_state_register();
    if (dm_vkn_frame_register) dm_vkn_frame_register();
    if (dm_vkn_pass_register) dm_vkn_pass_register();
    if (dm_vkn_exec_register) dm_vkn_exec_register();
    if (dm_vkn_draw_register) dm_vkn_draw_register();
    if (dm_vkn_imm_register) dm_vkn_imm_register();
    if (dm_vkn_ui_register) dm_vkn_ui_register();
    int in = 0, want = 0;
    for (int i = 0; i < g_nsite; i++) {
        vkn_site *s = &g_site[i];
        if (!(s->modes & (mode == VKN_NATIVE ? VKN_M_NATIVE : VKN_M_ALONGSIDE))) continue;
        want++;
        void *o = NULL;
        if (dn_hook_chain(m, s->off, s->pro, s->hook, &o)) { vkn_logf("\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91main+%#x (%s): not the words of DOS2 update v786432 main (build 4979b200) nor a hook of ours: this hook stays off", s->off, s->name); continue; }
        *s->orig = (vkn_fn)o; in++;
    }
    vkn_logf("DM_VK_NATIVE=%d (%s): %d of %d hooks in", mode, mode == VKN_NATIVE ? "native: the backend owns what it covers" : "alongside: the old path draws, the backend creates its resources next to it", in, want);
    if (in != want && mode == VKN_NATIVE) vkn_logf("\xe3\x80\x90\xe7\xba\xa2\xe3\x80\x91native mode with missing hooks: the picture will be wrong");
}
#endif
