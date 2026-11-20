// [DOS2] T2 DM_NR_IDVS: the translator's unconditional gl_PointSize = 1.0 store is dropped from vertex modules
// (see xbg_spirv_idvs.cpp and docs/dos2/nr/NR-IDVS.md)
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

struct XbgIdvsStats {
    uint32_t stores_dropped = 0;   // OpStore into the PointSize built-in
    uint32_t chains_dropped = 0;   // OpAccessChain to PointSize, left without a use
    const char *why = "";          // why nothing was done ("" when stores were dropped)
};

// Vertex modules only (one entry point, ExecutionModel Vertex). Every OpStore whose pointer is the PointSize built-in (a variable
// decorated BuiltIn PointSize, or an OpAccessChain / OpInBoundsAccessChain with one constant index selecting the PointSize member of
// an Output block) is removed, and so is each such access chain. The variable / block member, its decorations and the entry point's
// interface stay as they were. Refused (0, out empty) when PointSize is read, copied, passed on, or reached any other way, or when the
// module is not a single-entry Vertex module. Nothing else changes: gl_Position and every other output are written by the same
// instructions as before. Returns the number of stores removed.
//
// Why: the translator writes PointSize = 1.0 in every vertex shader (NVN's point size); Vulkan ignores it unless points are rasterized,
// but the Mali compiler keeps the store. On Valhall / 5th gen it is one more load/store cycle in the position shader IDVS runs for every
// vertex while binning; on Bifrost (G52 / G72 / G76) a PointSize write turns IDVS off entirely (malioc: one "Main" variant).
// The engine keeps the store for pipelines that rasterize points or have a tessellation / geometry stage (idvs_point_module).
size_t xbg_spirv_idvs_strip(const uint32_t *words, size_t count, std::vector<uint32_t> &out, XbgIdvsStats *stats = nullptr);

// An independent check of a strip: out must be in with instructions removed and nothing else changed (header, every kept
// instruction word for word, in order); each removed instruction an OpStore whose pointer (resolved in `in`) is PointSize, or an
// access chain to PointSize that out no longer mentions. "" = accepted, else the reason.
const char *xbg_spirv_idvs_check(const uint32_t *in, size_t in_count, const uint32_t *out, size_t out_count);

// Does the module declare a PointSize built-in output but never store to it (a stripped module)? (Used for the point-pipeline fallback.)
bool xbg_spirv_idvs_stripped(const uint32_t *words, size_t count);
