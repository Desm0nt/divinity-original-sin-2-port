// [DOS2] VK-NATIVE stage 5 rest: the portable pipeline warm-up seed on the backend (vkn_seed.c; was the old path's DM_NR_SEED, NR-31,
// KB PERF-X32: the first start on a new device built every pipeline in the frame, SIM mali -77 % of those builds with the seed).
//
// The seed is the file the APK already ships (dm/android/dos2/dos2-pipe-seed.bin -> files/dos2-pipe-seed.bin, DmActivity.dos2PipeSeed;
// a test push may put one into the shader directory, which wins): a phone's warm-up list of the OLD path (tools/dos2_pipe_seed.py build),
// one record per pipeline = the stage program keys + the Vulkan create info as content (dm/gfx/nr/nr_seed.h reads and checks it). The
// backend's own list (vkn_warm.h vkn_warm_rec) keys a pipeline by the engine's arguments and the content ids of this run, so a seed
// record is TRANSLATED when it is read ("content -> backend numbers"), on the warm-up worker:
//   * shaders: the record waits until the engine created a shader with its keys (#at = the fragment variant; a #vi code is made again
//     for this device by the draw path's rule, vkn_warm_retype_code; #vi with a conversion and the old path's fused post stages are
//     refused: the backend has its own);
//   * blend / depth-stencil / raster / vertex format: every content id of this run (and 0, the "no state" defaults) whose Vulkan state
//     as the pipeline build applies it (vkn_state.c vkn_pipe_build: blending off = only "off", stencil only with a stencil aspect,
//     depth only with a depth aspect, blend only with a colour target, vertex attributes of streams 0..2 with the 3-component
//     widening undone) equals the record's (id 0, "no state", only when no id of this run matches) - the engine made those states, the seed only says which of them a pipeline used; when the
//     pass makes a state irrelevant, only the "off" ids are candidates; at most VKN_SEED_COMBOS combinations a record (counted);
//   * the pass: the record's VkFormats, each one this device cannot render to replaced as vkn_tex does (D24S8 -> D32S8, X8D24 -> D32F;
//     other gaps: refused); samples, colour write masks and the stencil reference as recorded; the topology back to the engine's
//     primitive as drawn (list 1, strip 3, lines 5, line strip 6, points 0).
// A translated record enters the warm-up list as a pending entry (vkn_warm_seed_add: deduplicated against the device's own list, never
// written to its file) and the worker builds it when the shader exists, as any listed entry. Exact by construction: a draw takes a warm
// pipeline only for its own key (content ids of this run) and modules, so a wrong translation is a build nobody takes, never another
// picture. Records whose state never appears in this run stay waiting (counted).
//
// Switch DM_VK_NATIVE_SEED (read once; mode 1 with the warm-up on, DM_VK_NATIVE_WARM): 0 off; 1 (default, every family) translate +
// pre-build; 2 verify: translate and count only (nothing enters the list), and every pipeline the render thread builds in the frame is
// looked up: "the seed had it" (the same record, byte for byte) / "a seed record for the same shaders differs" / "shaders the seed has no
// record of" - on a phone with the warm-up list moved away this is the proof that the translation hits. The census line "[vkn] seed:".
// No game addresses.
//
// [VK-SEED] THE NEW PATH'S SEED (docs/dos2/nr/VK-SEED.md): the old seed above holds the OLD path's pipelines (T12 on the Mi 14: of 249
// in-frame builds it had 11). dos2-vkn-seed.bin (tools/dos2_vkn_seed.py build: one or more phones' vkn-pipeline-list.bin merged; APK
// asset -> files/, DmActivity.dos2Seeds; the shader directory wins) holds the backend's OWN list records made device-independent: the
// header binds no device, the vertex retype code (and a "mat" recipe's code word) is 0, a depth format of a source without D24 is the
// D24 one with the flag EITHER (pad3 bit 0); everything else is what any device records for that draw (program keys, engine argument
// tuples, vertex elements, pass VkFormats, technique bits and recipes). It is read FIRST; the old file only when it is absent.
// A record waits for an engine shader with its keys (as a list entry does), then gets this device's form: colour / depth formats by the
// texture path's attachment rule (vkn_tex_rt_format: D24 stand-ins; EITHER = both D24's and D32's form), the retype code by the draw
// path's rule for this shader + vertex format (vkn_warm_retype_code; also into a "mat" recipe), the checksum again - and enters the
// list as a pending entry (mode 1) like a translated old record. Exactness as above: a draw takes it only for its own key and modules.
// Mode 2 with this seed: an in-frame build is looked up by its portable identity (retype / recipe code cleared; on a device with D24
// stand-ins also as the D24 form), so the count does not depend on when the worker got to the record. Mode 1: those counts are the
// builds the worker did not have ready in time ("the seed had it" = late, not missing).
#ifndef DM_VKN_SEED_H
#define DM_VKN_SEED_H
#include "vkn_warm.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { VKN_SEED_COMBOS = 4 };
// Refusal reasons of a seed record (vkn_seed_reason_name).
enum { VKN_SEED_R_PARSE, VKN_SEED_R_KEYS, VKN_SEED_R_FUSED, VKN_SEED_R_CONV, VKN_SEED_R_SHAPE, VKN_SEED_R_FORMAT, VKN_SEED_R_PRIM, VKN_SEED_R_STAGES,
       VKN_SEED_R_BLEND, VKN_SEED_R_DS, VKN_SEED_R_RASTER, VKN_SEED_R_VFMT, VKN_SEED_R_SUM, VKN_SEED_R_VTAB, VKN_SEED_R_N };
// [VK-SEED] the new path's seed file (tools/dos2_vkn_seed.py): header 64 bytes, then nrec vkn_warm_rec records
#define VKN_SEED2_FILE "dos2-vkn-seed.bin"
enum { VKN_SEED2_VERSION = 1, VKN_SEED2_HDR = 64, VKN_SEED2_EITHER = 1 };
const char *vkn_seed_reason_name(int r);

typedef struct vkn_seed_stats {
    int mode, file;                      // in effect; file: 1 read, 0 none found, -1 damaged / other version, -2 not looked for yet
    int kind;                            // [VK-SEED] the file read: 2 the new path's dos2-vkn-seed.bin, 1 the old path's dos2-pipe-seed.bin, 0 none
    uint64_t id; uint32_t records;       // the seed's id and record count
    uint32_t waiting, translated, records_made, added, known, combos_cut;   // records waiting for their shader / states; translated; list records made, new, known
    uint32_t refused[VKN_SEED_R_N];
    uint64_t notes, had, differ, unknown;   // render-thread builds seen: the seed had it / same shaders, other content / no seed record of its shaders
    uint64_t tech_notes, tech_had;          // [VK-SEED] of them technique pipelines (fx bits, named stage, piece recipe; the new seed only)
    uint32_t either;                        // [VK-SEED] EITHER records (two depth forms on a device with D24)
} vkn_seed_stats;
void vkn_seed_stats_get(vkn_seed_stats *s);
// The warm-up worker (vkn_warm.c; weak there): dir = the cache directory (the shader directory's parent) or NULL while unknown.
void vkn_seed_tick(const char *dir);
void vkn_seed_note(const vkn_warm_rec *r);   // a pipeline the render thread built (its listable record)
void vkn_seed_census(void);
// Host test: the switch as given, a seed image instead of the file (copied), the state cleared.
void vkn_seed_test_setup(int mode, const uint8_t *seed, size_t bytes);

#ifdef __cplusplus
}
#endif
#endif
