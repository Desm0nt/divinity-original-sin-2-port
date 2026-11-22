// [DOS2] VK-SHADOWS-MOVING (docs/dos2/nr/VK-SHADOWS-MOVING.md): shadows cheap while the camera moves - the backend parts that are not
// the cache's state machine (that is vkn_shc.c, which calls these): a shadow caster's bounds on the CPU, the switches, the piece that
// draws the scrolled layer into a cascade tile. The rules and their proofs are in dm/gfx/nr/nr_shs.h. No game addresses.
//
// Bounds (vkn_shs_bounds). A draw gets pixel bounds when
//   - its vertex shader's position is a long program, affine in Location 0 x / y / z (the library's vkn_shlib_vs_posl, built at load
//     while a switch below is on; DOS2's static casters: World (64 B) x ViewProjection, 67 instructions);
//   - it draws triangles (engine primitive 1 or 3) from a per-vertex float / half-float stream of 3 or 4 components at Location 0;
//   - every constant block the program reads is announced (the ring) or the shader's own static block;
//   - w is exactly 1 (an orthographic light);
//   - the mesh's bounding box is known: found once per (vertex buffer, content version, offset, stride, index buffer, version, first,
//     count) by reading the vertices the draw fetches (a budget of DM_VK_NATIVE_SHS_SCAN indices a frame, default 400000; a mesh over
//     the budget waits for a later frame), kept in a table (8192 entries, 4 ways, named by two 64-bit hashes of that key). The bytes are the block the draw binds: the content version the record
//     carries (vkn_rt.h vkn_draw_ver) must be the table's version after the lookup, else no bounds ("version moved").
//   Then the fragments of the draw lie inside nr_sb_px of the box (1 px of margin).
// Switches (read once by vkn_shs_install; both need the shader library):
//   DM_VK_NATIVE_SHCULL    0 off; 1 (default since 22.11, series C) a caster whose bounds do not touch viewport and scissor is skipped
//                          before it is hashed or recorded (exact: it rasterises nothing); 2 counted only ("would cull"); 3 poison - as 1
//                          with the tile shrunk by 64 px on every side (casters at the tile's edge lose their shadows: shows the switch acts).
//   DM_VK_NATIVE_SHSCROLL  0 off; 1 (default since 22.11, series C: verify 0.08 % texels 1 step, x / y only) the scrolling layer (vkn_shc.h, block "VK-SHADOWS-MOVING");
//                          2 verify (forces DM_VK_NATIVE_SHCACHE=2: everything is drawn as the engine asks, the layer is kept and scrolled,
//                          the would-be tile is compared with the drawn one - texels differing and the largest depth step are counted);
//                          3 poison - as 1, but the scrolled layer is not drawn into the tile (static shadows vanish while moving).
//                          Needs DM_VK_NATIVE_SHCACHE=1 (or 2) and a D16 atlas cleared to 1.0.
//   DM_VK_NATIVE_SHSCROLL_Z     [fix 22.11] 0 (default): a layer whose cascade's z translation (the View / ViewProjection z words)
//                          is no longer the one of its build is dropped ("z moved"), and a layer is not built while that z changed
//                          within the last ranges ("builds held"): the z shift cannot be exact (the layer holds depths already rounded
//                          to D16 steps; adding a shift that is not a whole step rounds again - phone M09: 12-31 % of the texels one step
//                          off). 1: the old inexact path (the shift added by the piece), for measurements only.
//   DM_VK_NATIVE_SHSCROLL_EPS   how far (1 / N px, default 64) a shift may be from whole texels and an instance from its place.
//   DM_VK_NATIVE_SHS_SCAN  the bounding-box budget (indices a frame).
#ifndef DM_VKN_SHS_H
#define DM_VKN_SHS_H
#include "vkn.h"
#include "vkn_fx.h"
#include "../nr/nr_shs.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct vkn_shs_ub { uint8_t st, slot; uint32_t off, size; } vkn_shs_ub;   // a constant block of a kept draw (ring offset)
// px: framebuffer pixels; pih: a hash of every constant word the position program reads (the position of every vertex is a function
// of the mesh and these words alone); pio: the same over the words of every block but the cascade's per-view block (vkn_shs_cam_block):
// the object's own position inputs (World), bit for bit the same while a static object stands, whatever the cascade does
typedef struct vkn_shs_b { nr_sb_form f[4]; nr_ss_rect px; float z0, z1; uint64_t pih, pio; } vkn_shs_b;
enum { VKN_SHS_OK = 0, VKN_SHS_NOPROG, VKN_SHS_PRIM, VKN_SHS_FORMAT, VKN_SHS_UBO, VKN_SHS_W, VKN_SHS_BUFFER, VKN_SHS_VERSION, VKN_SHS_BUDGET,
       VKN_SHS_BIG, VKN_SHS_N };
typedef struct vkn_shs_stats {
    uint64_t asked, why[VKN_SHS_N];            // bounds asked / by outcome (VKN_SHS_OK = known)
    uint64_t box_hit, box_scan, box_idx;       // boxes from the table / scanned now, indices read
    uint64_t comp, comp_fail;                  // scrolled layers drawn into a tile / could not be
    uint64_t box_bytes;                        // the table
} vkn_shs_stats;

void vkn_shs_install(void);
int vkn_shs_cull(void);
int vkn_shs_scroll(void);
double vkn_shs_eps(void);                      // px
int vkn_shs_scroll_z(void);                    // DM_VK_NATIVE_SHSCROLL_Z
// The engine's per-view constant block of a shadow cascade (the host file knows the game: DOS2 PerView, vertex uniform slot 12, the
// ViewProjection rows at byte 128, the View rows at byte 0; a row = x y z coefficients + the translation). Nothing here trusts the
// layout: the cache only uses the words as a reference point and checks every member against it by numbers (vkn_shc.c).
void vkn_shs_cam_block(uint32_t slot, uint32_t vp_off, uint32_t view_off);
int vkn_shs_cam(uint32_t *slot, uint32_t *vp_off, uint32_t *view_off);   // 1 = given
int vkn_shs_bounds(const vkn_draw_in *in, const vkn_fx_ctx *ctx, vkn_shs_b *b);   // VKN_SHS_*; recording thread
// The affine position of a kept draw again (its blocks by ring offset), with the block of vertex slot cam_slot at ring offset cam_from
// read at cam_to instead. 1 = ok.
int vkn_shs_forms_kept(const vkn_shader *sh, const vkn_shs_ub *ub, uint32_t nub, int cam_slot, uint32_t cam_from, uint32_t cam_to, nr_sb_form f[4]);
// The piece: the layer (toroidal offset O, z shift dz, clear value) into the current target set's depth target over tile (x, y, w, h),
// only inside keep (framebuffer pixels). base: the engine draw in hand (its vertex buffers and format ride along, unused). 0 = drawn.
int vkn_shs_composite(const vkn_draw_in *base, vkn_tex *layer, const int32_t tile[4], const nr_ss_rect *keep, const int32_t O[2], float dz, float clear);
void vkn_shs_stats_get(vkn_shs_stats *s);
void vkn_shs_stats_reset(void);
const char *vkn_shs_why_name(int why);
// Host tests: the program of a shader (NULL = the library's); the modes without the environment.
void vkn_shs_test_prog(const nr_sb_prog *(*fn)(const vkn_shader *s));
void vkn_shs_test_modes(int cull, int scroll);   // (DM_VK_NATIVE_SHSCROLL_Z stays as read)
#ifdef __cplusplus
}
#endif
#endif
