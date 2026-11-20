// [DOS2] NR-33C: the surface materials' fragment shaders with a wave-coherent early out (DM_NR_SH_SURF, default 0; docs/dos2/nr/NR-33C.md).
// Pure C: no Vulkan headers, no game addresses, no host types. The variants themselves are NOT in the APK: setup (stage 8,
// tools/dos2_nr_surf.py) transforms the player's own translated SPIR-V into <shaders>/native-nr33c.xgspack:
//   "sr.<fs key>"  record XSR1: FNV-1a 64 of the translated SPIR-V it was made from + the variants present (NR_SURF_V_*)
//   "k.<fs key>"   exact: after the top-level kills, if (subgroupAny(live)) { the rest } else { kill }
//   "a.<fs key>"   exact for a blended draw: also a colour 0 alpha of +-0 is dead (needs nr_surf_alpha_gate == 0)
//   "m.<fs key>"   mobile: 0 <= alpha < 1/255 is dead (same gate) - an owner-visible tolerance
//   "p.<fs key>"   poison: as m, a skipped wave draws magenta where the alpha is dead (shows where the early out fires)
// This file: the record, the variant a draw takes, and the blend / depth gate of the alpha variants.
// Thread safe: pure functions.
#ifndef DM_NR_SURF_H
#define DM_NR_SURF_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NR_SURF_V_K = 1, NR_SURF_V_A = 2, NR_SURF_V_M = 4, NR_SURF_V_P = 8 };
enum { NR_SURF_OFF = 0, NR_SURF_EXACT = 1, NR_SURF_MOBILE = 2, NR_SURF_POISON = 3, NR_SURF_CHECK = 4 };

typedef struct nr_surf_rec {
    uint64_t spirv_fnv;    // FNV-1a 64 of the translated bundle's own SPIR-V bytes
    uint32_t variants;     // NR_SURF_V_*
} nr_surf_rec;
// 0 = parsed; else why not (static string)
const char *nr_surf_rec_parse(const uint8_t *b, size_t n, nr_surf_rec *r);
uint64_t nr_surf_fnv64(const uint8_t *b, size_t n);

// One colour attachment of the pipeline being built, in Vulkan terms (VkBlendFactor / VkBlendOp numbers, colorWriteMask).
typedef struct nr_surf_att {
    int blend;             // blendEnable
    int src_c, dst_c, op_c, src_a, dst_a, op_a;
    unsigned mask;         // colorWriteMask (0 = nothing written)
} nr_surf_att;
enum {
    NR_SURF_G_BLEND = 1,       // colour 0 does not blend to "unchanged" at source alpha 0 (SRC_ALPHA / ONE_MINUS_SRC_ALPHA|ONE, ADD;
                               //   dst alpha factor ONE / ONE_MINUS_SRC_ALPHA, ADD)
    NR_SURF_G_OTHER = 2,       // another enabled attachment is written by the fragment shader (its own alpha, not colour 0's)
    NR_SURF_G_DEPTH = 4,       // depth writes on
    NR_SURF_G_STENCIL = 8,     // stencil test with a write mask
    NR_SURF_G_A2C = 16,        // alpha to coverage
    NR_SURF_G_LOGIC = 32,      // a logic op
    NR_SURF_G_QUERY = 64,      // an occlusion query counts samples (a dead-alpha fragment passes the depth test in the game)
};
// 0 = a fragment with colour 0 alpha +-0 leaves every attachment, depth, stencil and query as it was: killing it is exact.
// fs_out_mask: the Location outputs the fragment shader writes (bit per location).
unsigned nr_surf_alpha_gate(const nr_surf_att *att, int n_att, uint32_t fs_out_mask, int depth_write, int stencil_write, int a2c,
                            int logic_op, int counting);
// The variant a draw takes: mode (NR_SURF_*), variants the record has, gate = nr_surf_alpha_gate. 0 = keep the game's shader.
char nr_surf_choose(int mode, uint32_t variants, unsigned gate);

#ifdef __cplusplus
}
#endif
#endif
