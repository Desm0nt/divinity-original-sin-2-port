// [DOS2][NR-32] DM_NR_XLQ: vertex-shader output components that the pipeline's fragment shader never reads are not written
// (see xbg_spirv_vsout.cpp and docs/dos2/nr/NR-32.md)
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// A varying mask: bit 4 * location + component, locations 0..15 (generic Location-decorated user varyings only; built-ins are not in it).
constexpr uint64_t XBG_VSOUT_ALL = ~uint64_t(0);

// The components a fragment shader may read, over-approximated: every Input variable with a Location (and Component) counts in
// full unless all its references are OpAccessChain with constant indexes, which count for what they select. A module that cannot
// be analysed (no single Fragment entry point, an input block / struct / matrix / 64-bit input, a location past 15) returns
// XBG_VSOUT_ALL, which disables the trim for that pipeline.
// declared (optional) receives every component of every Location input the module declares, read or not.
uint64_t xbg_spirv_fs_input_mask(const uint32_t *words, size_t count, uint64_t *declared = nullptr);

// The components a vertex shader writes to its Location outputs (0 when it is not a single-entry Vertex module, XBG_VSOUT_ALL
// when an output is of a form the trim does not handle).
uint64_t xbg_spirv_vs_output_mask(const uint32_t *words, size_t count);

struct XbgVsoutStats {
    uint32_t stores_dropped = 0;   // OpStore to output components outside keep
    uint32_t chains_dropped = 0;   // OpAccessChain into an output left without a use
    uint32_t vars_dropped = 0;     // Output variables left without a reference: off the interface, decorations, names, the variable
    uint32_t pinned = 0;           // Location outputs left as they were (read back, a dynamic index, a use the pass does not know)
};

// Vertex modules only. Every OpStore to a Location output (the whole variable or one component through OpAccessChain with a constant
// index) whose components are all outside keep is removed; an output variable left with no reference at all is removed from the
// entry point's interface with its decorations and name. Nothing else changes: the values the stores wrote are still computed (the
// driver's compiler drops them as dead code), and every store that writes a kept component is kept whole. Returns the number of
// stores removed; 0 = out is empty and the module is used unchanged. An output variable overlapping `declared` (the fragment
// shader's declared inputs) stays on the interface even when no store to it is left, so every declared fragment input keeps a
// matching vertex output.
size_t xbg_spirv_vsout_trim(const uint32_t *words, size_t count, uint64_t keep, std::vector<uint32_t> &out, XbgVsoutStats *stats = nullptr,
                            uint64_t declared = XBG_VSOUT_ALL);

// An independent check of a trim: out must be in with instructions removed (and interface ids dropped from OpEntryPoint), each removed
// instruction an OpStore into an output component outside keep, an OpAccessChain into a Location output that out no longer uses, or
// the OpVariable / OpDecorate / OpName of an Output variable that out no longer references. "" = accepted, else the reason.
const char *xbg_spirv_vsout_check(const uint32_t *in, size_t in_count, const uint32_t *out, size_t out_count, uint64_t keep);
