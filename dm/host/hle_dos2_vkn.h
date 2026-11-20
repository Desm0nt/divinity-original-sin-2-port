// [DOS2] VK-NATIVE front end, shared by hle_dos2_vkn.c and its parts (hle_dos2_vkn_*.c): the hook signature, the site table, guest
// access. See hle_dos2_vkn.c. Guest pointers are host pointers (the image is mapped as it is); main-relative addresses go through
// dm_vkn_base().
#ifndef DM_HLE_DOS2_VKN_H
#define DM_HLE_DOS2_VKN_H
#include <stdint.h>
#include <string.h>

// A hook has the engine function's own signature as far as AAPCS64 carries it: 8 integer registers, 8 double registers, 12 stack
// slots (copied, so a hook may pass them on). Not carried: x8 (indirect result) and the upper halves of v0-v7. disable_tail_calls:
// the call of the original must not reuse this frame (the stack slots are this function's own arguments).
#define VKN_A uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7, \
              double d0, double d1, double d2, double d3, double d4, double d5, double d6, double d7, \
              uint64_t s0, uint64_t s1, uint64_t s2, uint64_t s3, uint64_t s4, uint64_t s5, uint64_t s6, uint64_t s7, uint64_t s8, uint64_t s9, uint64_t s10, uint64_t s11
#define VKN_F a0, a1, a2, a3, a4, a5, a6, a7, d0, d1, d2, d3, d4, d5, d6, d7, s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11
#define VKN_HOOK static __attribute__((disable_tail_calls)) uint64_t
#define VKN_UNUSED (void)a0, (void)a1, (void)a2, (void)a3, (void)a4, (void)a5, (void)a6, (void)a7, (void)d0, (void)d1, (void)d2, (void)d3, (void)d4, (void)d5, (void)d6, (void)d7, \
                   (void)s0, (void)s1, (void)s2, (void)s3, (void)s4, (void)s5, (void)s6, (void)s7, (void)s8, (void)s9, (void)s10, (void)s11
typedef uint64_t (*vkn_fn)(VKN_A);

enum { VKN_M_NATIVE = 1, VKN_M_ALONGSIDE = 2, VKN_M_BOTH = 3 };   // the modes a site is hooked in
typedef struct vkn_site { uint32_t off; uint32_t pro[4]; const char *name; void *hook; vkn_fn *orig; int modes; } vkn_site;
// Registers a hook (called from a part's dm_vkn_*_register, install time only). pro: the function's first 4 instructions (none
// PC-relative; tools/dos2_nr_check.py checks every row "dm_vkn_site(0x..., k_..." against main.raw).
void dm_vkn_site(uint32_t off, const uint32_t pro[4], const char *name, void *hook, vkn_fn *orig, int modes);
typedef void (*vkn_census_fn)(void);
void dm_vkn_census(vkn_census_fn fn);   // a part's line(s) of the 10 s census (render thread, after EndFrame)

uint8_t *dm_vkn_base(void);             // host address of main+0 (NULL before install)
static inline uint64_t vkn_rd64(uint64_t p) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)p, 8); return v; }
static inline uint32_t vkn_rd32(uint64_t p) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)p, 4); return v; }
static inline uint16_t vkn_rd16(uint64_t p) { uint16_t v; memcpy(&v, (const void *)(uintptr_t)p, 2); return v; }
static inline uint8_t vkn_rd8(uint64_t p) { return *(const uint8_t *)(uintptr_t)p; }
static inline uint64_t vkn_main64(uint32_t off) { return vkn_rd64((uint64_t)(uintptr_t)(dm_vkn_base() + off)); }
static inline uint64_t vkn_renderer(void) { return vkn_main64(0x2bbdc68); }   // r
// The record of an engine handle in the table at r + tab (pages at +0, generations at +8, records per page at +0x10; record size
// rec_bytes). 0 = stale / empty handle.
static inline uint64_t vkn_rec(uint64_t r, uint32_t tab, uint32_t rec_bytes, uint64_t h) {
    const uint32_t idx = (uint32_t)h, gen = (uint32_t)(h >> 32) & 0x3fffffu, per = vkn_rd32(r + tab + 0x10);
    if (!gen || !per) return 0;
    const uint32_t page = idx / per, slot = idx % per;
    const uint64_t gens = vkn_rd64(vkn_rd64(r + tab + 8) + 8ull * page);
    if (!gens || vkn_rd32(gens + 4ull * slot) != gen) return 0;
    return vkn_rd64(vkn_rd64(r + tab) + 8ull * page) + (uint64_t)slot * rec_bytes;
}
#endif
