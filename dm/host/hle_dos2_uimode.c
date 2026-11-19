// [DOS2] The in-game UI mode switch: SWITCH / HYBRID / TOUCH (owner 19.11; docs/dos2/TOUCH_MODES_PLAN.md sections 3, 4 stage 1 and 7, and its
// "Implemented: UI mode switch"; the rules are in hle_dos2_uimode.h). Static only when written: nothing here ran on a phone.
// One runtime mode, read from DM_DOS2_UI_MODE at install (main.c host_preload, before hle_dos2_touchui.c and hle_dos2_touch.c read it) and
// changed by the gear through JNI (DmActivity.nativeDos2UiMode -> dm_dos2_uimode_request, any thread). The work is done on the game thread, in
// the pad poll hook of hle_dos2_touch.c (dm_dos2_uimode_poll from dt_drain), where CreateUIObject and the live rebuild run too:
//   menus  dm_dos2_touchui_set_mode: the creators' paths of the new mode's table are written, the others given back (the host buffers live for
//          the run); dm_dos2_ui_touches: touches are dropped in SWITCH (a finger already down still gets its up / cancel)
//   HUD    dm_dos2_touch_ui_hud: the mode byte main+0x2bb8918, the bindings path main+0x1ebfc35, the live pad switch of TOUCH; applied when no
//          player UI set exists (dm_dos2_touch_ui_records() == 0: main menu / loading) so the next load builds the HUD in the new mode
//          (plan 3 A, the default), or at once with DM_DOS2_UI_LIVE=1 through the live rebuild (0x10c4730 + 0x10c4580) when nothing modal is up
//          (dm_dos2_touchui_ui_busy; plan 3 B), else as A.
// Log: one line at install, one per change "[uimode] <old> -> <new> (menus now, HUD at next load | HUD rebuilt in N us)", one when a waiting
// HUD part lands at the next load.
// Switches (testing; players use the gear):
//   DM_DOS2_UI_MODE=0|1|2     switch | hybrid (default) | touch, also the names; what the gear's Os.setenv gives, dm/env wins
//   DM_DOS2_UI_LIVE=1         B: a change in game rebuilds the player UI sets at once when no modal window / dialog is open (default 0 = A)
#ifdef DM_GAME_DOS2
#include "hle.h"
#include "hle_dos2_uimode.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static _Atomic int g_mode = UI_HYBRID;    // the requested mode: menus (paths) and the touch gate follow it at once
static _Atomic int g_hud = UI_HYBRID;     // the mode the HUD part (mode byte, bindings, live pad switch) runs now
static _Atomic int g_req = -1;            // a request from Java not taken by the game thread yet: seq << 2 | mode, -1 = none
static _Atomic unsigned g_req_seq, g_done_seq;
static _Atomic int g_result = UI_RES_NEXT_LOAD;
static int g_live_b, g_installed;

int dm_dos2_ui_mode(void);
int dm_dos2_ui_mode(void) { return atomic_load(&g_mode); }
int dm_dos2_ui_mode_hud(void);
int dm_dos2_ui_mode_hud(void) { return atomic_load(&g_hud); }
int dm_dos2_ui_touches(void);   // hle_dos2_touch.c dm_dos2_touch_event: may touches become mouse records now
int dm_dos2_ui_touches(void) { return ui_mode_touches(atomic_load(&g_mode)); }

void dm_dos2_uimode_install(void);   // main.c host_preload, before dm_dos2_touchui_install / dm_dos2_touch_install
void dm_dos2_uimode_install(void) {
    const char *e = getenv("DM_DOS2_UI_MODE"), *v;
    const int m = ui_mode_parse(e);
    atomic_store(&g_mode, m); atomic_store(&g_hud, m);
    g_live_b = (v = getenv("DM_DOS2_UI_LIVE")) && atoi(v) > 0;
    g_installed = 1;
    LOG("[uimode] UI mode at start: %d (%s)%s; a change in game: menus at once, the HUD %s", m, ui_mode_name(m),
        e && *e ? "" : " (DM_DOS2_UI_MODE unset: the default)",
        g_live_b ? "rebuilt at once when no modal window is open, else at the next load (DM_DOS2_UI_LIVE=1)" : "at the next load / return to the main menu (DM_DOS2_UI_LIVE=1: rebuilt at once)");
}

static uint64_t um_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000ull; }

// hle_dos2_touch.c dt_drain, every pad poll (game thread)
void dm_dos2_uimode_poll(void);
void dm_dos2_uimode_poll(void) {
    int dm_dos2_touchui_set_mode(int mode);                         // hle_dos2_touchui.c: paths of the mode's table, -1 = no touch UI in this run
    int dm_dos2_touchui_ui_busy(uint32_t *type);                    // hle_dos2_touchui.c: 1 a modal / blocking window is up, 0 none, -1 cannot tell
    int dm_dos2_touch_ui_records(void);                             // hle_dos2_touch.c: player UI sets now (-1 = cannot tell)
    int dm_dos2_touch_ui_hud(int mode, int rebuild, uint64_t *us);  // hle_dos2_touch.c: the HUD part now; 1 done, 0 refused
    const int rq = atomic_exchange(&g_req, -1);
    if (rq >= 0) {
        const int r = rq & 3; const unsigned sq = (unsigned)rq >> 2;
        const int old = atomic_load(&g_mode), hud = atomic_load(&g_hud);
        int res = UI_RES_NEXT_LOAD;
        if (r != old) {
            atomic_store(&g_mode, r);
            const int np = dm_dos2_touchui_set_mode(r);
            const int recs = dm_dos2_touch_ui_records();
            uint32_t bt = 0; const int busy = g_live_b && recs > 0 ? dm_dos2_touchui_ui_busy(&bt) : 0;
            int step = ui_hud_step(hud, r, recs < 0 ? 1 : recs, g_live_b, 1, busy);
            uint64_t us = 0;
            if (step == UI_HUD_REBUILD) {
                const uint64_t t0 = um_us();
                if (dm_dos2_touch_ui_hud(r, 1, &us)) atomic_store(&g_hud, r);
                else step = UI_HUD_WAIT;
                if (!us) us = um_us() - t0;
            } else if (step == UI_HUD_APPLY) { dm_dos2_touch_ui_hud(r, 0, NULL); atomic_store(&g_hud, r); }   // a refusal is permanent (sites): logged there, not retried
            res = ui_result_of(step);
            char why[96] = "";
            if (g_live_b && step == UI_HUD_WAIT && recs > 0) {
                if (busy > 0) snprintf(why, sizeof why, "; no rebuild: window type %#x is up", bt);
                else snprintf(why, sizeof why, "%s", busy < 0 ? "; no rebuild: the window watch is off" : "; no rebuild: refused");
            }
            if (step == UI_HUD_REBUILD) LOG("[uimode] %s -> %s (HUD rebuilt in %llu us; %d path(s) of the %s table)", ui_mode_name(old), ui_mode_name(r), (unsigned long long)us, np, ui_mode_name(r));
            else LOG("[uimode] %s -> %s (menus now, HUD at next load%s; %d path(s) of the %s table%s%s)", ui_mode_name(old), ui_mode_name(r),
                     step == UI_HUD_APPLY ? " - no player UI set now, the HUD part applied" : step == UI_HUD_NONE ? " - the HUD already runs it" : "", np, ui_mode_name(r),
                     np < 0 ? ": no touch UI in this run" : "", why);
        }
        atomic_store(&g_result, res);
        atomic_store(&g_done_seq, sq);
    }
    // the HUD part waiting for the next load: applied once no player UI set exists (main menu / loading), before the next set is built
    const int m = atomic_load(&g_mode), hud = atomic_load(&g_hud);
    if (m != hud && dm_dos2_touch_ui_records() == 0) {
        const int ok = dm_dos2_touch_ui_hud(m, 0, NULL);
        atomic_store(&g_hud, m);   // a refusal is permanent (the mode byte's sites): not retried every poll
        if (ok) LOG("[uimode] HUD part of %s applied (no player UI set: main menu / loading; mode byte %d): the next load builds the %s HUD", ui_mode_name(m), ui_mode_byte(m), ui_mode_name(m));
        else LOG("【红】[uimode] HUD part of %s NOT applied (hle_dos2_touch.c refused: see the line above); menus follow the mode, the HUD keeps the old one", ui_mode_name(m));
    }
}

// JNI (DmActivity.nativeDos2UiMode, the UI thread): mode < 0 = the requested mode now; else the request, answered by the game thread within
// ~400 ms: UI_RES_* (hle_dos2_uimode.h)
int dm_dos2_uimode_request(int mode);
int dm_dos2_uimode_request(int mode) {
    int dm_dos2_touch_on(void);
    if (mode < 0) return g_installed ? atomic_load(&g_mode) : -1;
    if (!ui_mode_valid(mode)) return -1;
    if (!g_installed || !dm_dos2_touch_on()) {   // no pad poll hook in this run: nothing runs the switch (DM_DOS2_TOUCH=0)
        LOG("[uimode] %s requested: no live switch in this run (DM_DOS2_TOUCH is off): from the next launch", ui_mode_name(mode));
        return UI_RES_OFF;
    }
    const unsigned seq = (atomic_fetch_add(&g_req_seq, 1) + 1) & 0x1fffffffu;   // fits the int with the mode in its low 2 bits
    atomic_store(&g_req, (int)(seq << 2 | (unsigned)mode));
    for (int i = 0; i < 80; i++) { if (atomic_load(&g_done_seq) == seq) return atomic_load(&g_result); usleep(5000); }
    LOG("[uimode] %s requested: the game thread has not taken it yet (paused?): it applies when the game runs on", ui_mode_name(mode));
    return UI_RES_PENDING;
}
#endif
