// [DOS2] NR-10b DM_NR_COVER: a vertex shader's gl_Position as a program the CPU can run at the draw (dm/gfx/nr/nr_cover.h, nr_cv_prog).
// The old facts (xbg_spirv_raster.h) only recognise "x, y, z copied from one input"; the translated DOS2 shaders keep registers in
// Function variables, write z / w as constants and (some) scale by a uniform.
#pragma once
#include <cstddef>
#include <cstdint>
#include "../../nr/nr_cover.h"

// words: a vertex-stage module (the final SPIR-V the pipeline gets). pr->ok = 1 when the module is one function of one block (no call,
// no branch), never writes ClipDistance / CullDistance, and each of gl_Position's four components, at the end of the shader, is a
// value built only from float constants, components of float vertex inputs (Location, no Component decoration), floats of uniform blocks
// (set 0; struct / array / vector members with constant indices) and + - * / negate fma abs min max - at most NR_CV_MAX_INS values and
// NR_CV_MAX_UBO blocks. Anything else: ok = 0 (why, when given, gets a short reason).
void xbg_spirv_pos_prog(const uint32_t *words, size_t count, nr_cv_prog *pr, const char **why = nullptr);
// [P10] The same walk for a varying: the Output variable decorated Location `location` (a float vector of 2..4 components, no Component
// decoration) instead of gl_Position. pr->out[0], out[1] = its x, y (both must be followed); out[2], out[3] = z, w where the variable has
// them and they are followed, else copies of out[0]. A ClipDistance write does not refuse it. dm/gfx/vkn: vkn_shlib_vs_out_prog.
void xbg_spirv_out_prog(const uint32_t *words, size_t count, uint32_t location, nr_cv_prog *pr, const char **why = nullptr);
