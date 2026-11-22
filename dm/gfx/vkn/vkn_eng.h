// [DOS2] VK-ENGINE-SIDE stage 1 (docs/dos2/nr/VK-ENGINE-SIDE.md section 11): the backend half of the census APK - the merge census
// ("how many draws of one pass repeat the same mesh + material + pipeline", the ceiling of static merging, doc section 5 step 0). The
// engine half (objects by pass and cascade, size classes, static vs moving, the far-cascade rate, small casters) is
// dm/host/hle_dos2_vkn_eng.c. No game addresses here.
//
// One vkn_fx function "engcensus" (order 5, count only: it never changes a draw). Per draw: its pass class and a key of everything
// that decides what it rasterises and how it is shaded EXCEPT the constant blocks (the World matrix lives there): engine shader,
// blend / depth-stencil / raster / vertex-format states, primitive, vertex buffers with offsets, index buffer, first / count /
// instances, the 32 texture and sampler ids of the vertex and fragment stages, and the pass (target set, viewport for the atlas
// tiles). Draws with an equal key in one frame and one pass are one group. Classes:
//   c0..c3   the sun-shadow atlas (depth only, square, >= 1024): its viewport tiles in order of their first draw in the frame
//   prepass  depth only, not square (the scene pre-pass)
//   gbuffer  >= 3 colour targets
//   other    every other draw with a colour target; "depth" other depth-only targets (square < 1024)
// Switch: DM_VK_ENG_CENSUS (read once by vkn_eng_install; the host part reads the same switch): 0 off (default), 1 the engine
// census only, 2 + this merge census. It costs the recording thread ~0.1 us a draw: census runs are for counts, not for milliseconds.
// Line "[vkn] eng merge census ..." every 10 s. Recording thread only.
#ifndef DM_VKN_ENG_H
#define DM_VKN_ENG_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_ENG_C0 = 0, VKN_ENG_PRE = 4, VKN_ENG_GB = 5, VKN_ENG_OTHER = 6, VKN_ENG_DEPTH = 7, VKN_ENG_CLS = 8 };
typedef struct vkn_eng_stats {
    uint64_t frames, full;                       // frames with a counted draw; draws the key table could not hold (not grouped)
    uint64_t draws[VKN_ENG_CLS], keys[VKN_ENG_CLS];   // draws, distinct keys
    uint64_t g2[VKN_ENG_CLS], g4[VKN_ENG_CLS], g8[VKN_ENG_CLS];   // draws in groups of at least 2 / 4 / 8 draws
} vkn_eng_stats;
void vkn_eng_install(void);                      // idempotent; registers the function for DM_VK_ENG_CENSUS >= 2
int vkn_eng_census_mode(void);                   // DM_VK_ENG_CENSUS as read (-1 = not read yet)
void vkn_eng_stats_get(vkn_eng_stats *s);        // the current window
void vkn_eng_census(void);                       // the line; the window restarts
void vkn_eng_test_setup(int mode);               // host tests: mode as if read, window cleared

#ifdef __cplusplus
}
#endif
#endif
