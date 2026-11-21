// [DOS2] VK-NATIVE: every entry of the native backend runs on the calling thread's 1 MiB host stack (hle_dos2_vkn_stack.c, where
// the mechanism and the stack numbers are written down).
#ifndef DM_HLE_DOS2_VKN_STACK_H
#define DM_HLE_DOS2_VKN_STACK_H
#include <stdint.h>

// A code address that, called with a hook's arguments (x0-x7, v0-v7, 12 stack slots; the VKN_A signature), runs hook on this
// thread's host stack and returns its x0; in place when the caller is already on it. Install time only (single-threaded: it writes
// and seals executable pages). NULL = no stub (mmap failed): then use the hook itself.
void *dm_vkn_stack_stub(void *hook);
// The guest return address of the innermost stub call on this thread (what __builtin_return_address(0) of the hook would have been
// without the stub), 0 outside a stub call. For hooks that tell their callers apart.
uint64_t dm_vkn_caller(void);
// fn(ctx) on this thread's host stack (in place when already on it): for the entries that are not engine functions (packet
// handlers of the ExecuteRenderCommand hook).
uint64_t dm_vkn_on_stack(uint64_t (*fn)(void *ctx), void *ctx);
// ==== [stage 5.5] the recording thread's default sync (hle_dos2_vkn_stack.c head; dm/gfx/vkn/vkn_rt.h) ====
// DM_VKN_SF_ENC: the hook encodes its backend work as records (vkn_rt_*) or does only engine-side work: no default sync at its entry.
// Every other stub / on-stack call syncs on the engine thread first and owns the recording for the call.
enum { DM_VKN_SF_ENC = 1 };
struct vkn_rt_site;
void dm_vkn_stack_flag(void *hook, unsigned flags);                       // registration time, before the install makes the stubs
void *dm_vkn_stack_stub_ex(void *hook, const char *name, unsigned flags);  // dm_vkn_stack_stub with the census name of its sync site
uint64_t dm_vkn_on_stack_x(uint64_t (*fn)(void *ctx), void *ctx, struct vkn_rt_site *site, unsigned flags);
// Stub calls so far: [0] switched onto the host stack, [1] already on it (nested: an engine function called by a hook reached
// another hooked one), [2] dm_vkn_on_stack calls.
void dm_vkn_stack_stats(uint64_t out[3]);
#endif
