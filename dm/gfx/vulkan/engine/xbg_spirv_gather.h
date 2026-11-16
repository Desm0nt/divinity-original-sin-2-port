// textureGather footprint of the Switch GPU (xbg_spirv_gather.cpp). Maxwell rounds texture coordinates to 1/256 of a texel before it
// picks the 2x2 gather footprint, so the footprint's first texel is floor(u * size - 0.5 + 1/512), not floor(u * size - 0.5). Game
// shaders that gather and then weight the four texels themselves compute their weights for that footprint: DOS2's surface / cloud
// materials (RS3_Surface_*, RS3_Cloud_*, 396 fragment modules) do fract(fma(u, size, -0.498046905)), i.e. -(0.5 - 1/512) nudged by one
// ulp. On a GPU that picks the footprint at another fraction (Adreno / Turnip, Mali, ...) the two disagree in a band about 1/512 texel
// wide along every texel border: the shader weights texel k+1 with ~1 but the gather returned texel k (bright / dark specks in dotted
// lines along the surface grid, twice the size in the half-resolution particle chain, and wrong OpKill holes from the id compare).
// The rewrite makes the footprint the shader's own on any GPU: before each plain 2D OpImageGather the coordinate is replaced by the
// centre of that footprint, (floor(fma(coord, size, -0.498046905)) + 1) / size, with size = OpImageQuerySizeLod level 0. At the centre
// the hardware fraction is 0.5, half a texel away from any rounding rule. Texels are the ones Maxwell returns; nothing else changes.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

struct xbg_spirv_gather_info {
    uint32_t snapped=0;     // OpImageGather sites given the footprint centre
    uint32_t skipped=0;     // gathers left alone (image operands, not a plain 2D image, coordinate not a float vec2)
    bool signature=false;   // the module has the -(0.5 - 1/512) weight constant (0xbeff0001 or 0xbeff0000)
};
// all: false = only modules with the weight constant (they state the footprint they expect), true = every module that gathers.
// Returns nullptr and fills out when at least one gather was rewritten; otherwise the reason (out is left empty, keep the module).
const char *xbg_spirv_gather_snap(const uint32_t *words,size_t count,bool all,std::vector<uint32_t> &out,xbg_spirv_gather_info *info=nullptr);
