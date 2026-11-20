// [DOS2] NR-08b: dynamic uniform buffers for the PowerVR class (DM_NR_UBODYN, default 0; docs/dos2/nr/NR-08b.md). Pure C: no Vulkan,
// no engine, no host types. The engine side is dm/gfx/vulkan/engine/xbg_nr_ubodyn.inc; dm/gfx/vulkan/engine/tests/nr_ubodyn_test.c
// tests it on the PC (hand-made modules + every DOS2 bundle).
//
// Why PowerVR has no dynamic uniform buffers in the policy (xbg_ubo_dynamic_default): PPSSPP 73559d15aa (Moto G54, PowerVR BXM-8-256,
// the driver our DOOM player has): at draw time the driver writes address + range of every dynamic uniform buffer a stage can see into
// a table sized for the buffers that stage READS; a declared but unread one writes 16 bytes past it - a page fault in vkCmdDrawIndexed
// when the table ends at a page. Not a missing feature (BXM-8-256 1.15 reports maxDescriptorSetUniformBuffersDynamic = 0xffffffff)
// and not a slow path: a preventive rule. DOOM 2016 (PVR-06, 09.11) answered it with DM_GFX_UBO_DYNAMIC=2: dynamic only for blocks
// the stage's SPIR-V reads. This is that rule for DOS2, stricter:
//   - only per-draw blocks (game uniform blocks = the constant ring, driver blocks) may be dynamic; support block and c1 constants
//     stay at fixed offsets (they never move, a dynamic offset buys nothing and only widens the driver's exposure);
//   - "read" = a load from the binding WHOSE VALUE IS USED, inside the entry point's function (a read only in a called function, or
//     a load whose result nothing uses, does not count: a driver that drops it would see the binding unread). PPSSPP: "declaring
//     it isn't enough".
// Every binding a stage can see is its own (DOS2 layouts: stageFlags = one stage per binding), so "the stage reads it" is a property
// of that stage's module alone.
#ifndef DM_NR_UBODYN_H
#define DM_NR_UBODYN_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Why a module is not understood (0 = understood). Not understood -> the caller treats every binding as unread (nothing dynamic).
enum { NRU_OK = 0, NRU_BAD_HEADER, NRU_TRUNCATED, NRU_ENTRY_POINTS /* none or several */, NRU_NO_ENTRY, NRU_NO_MEMORY, NRU_N };
const char *nru_why_name(int why);

// For bindings[0..nb) (Vulkan binding numbers, any descriptor set): read[i] = 1 when the module reads binding i (rule above), else 0.
// Returns NRU_OK or why the module was not understood (then read[] is all 0). bindings may repeat; nb may be 0.
int nr_spv_binding_reads(const uint32_t *w, size_t n, const uint32_t *bindings, unsigned nb, uint8_t *read);
// The binding numbers the module declares on variables (sorted, unique): at most max written to out; returns how many it declares
// (may be > max), or -1 when the header is bad.
int nr_spv_declared_bindings(const uint32_t *w, size_t n, uint32_t *out, unsigned max);
// The optimizer / a rewrite may only ADD reads: 1 when every declared binding `before` reads, `after` reads too (and `after` is
// understood whenever `before` is), else 0.
int nr_spv_reads_kept(const uint32_t *before, size_t nb, const uint32_t *after, size_t na);

// The rule for one uniform binding: may it be a dynamic uniform buffer on a device class whose policy turned them off?
// type: VkDescriptorType of the bundle table (6 = UNIFORM_BUFFER), source: XBG_SOURCE_* (2 game block, 5 / 6 driver blocks).
static inline int nru_dynamic_ok(uint32_t type, uint32_t source, int read) {
    return type == 6u && (source == 2u || source == 5u || source == 6u) && read;
}

#ifdef __cplusplus
}
#endif
#endif
