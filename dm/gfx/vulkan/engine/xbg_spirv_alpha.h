// NVN fixed-function alpha test (nvnColorStateSetAlphaTest + nvnCommandBufferSetAlphaRef) emulated in the fragment shader: Vulkan
// has no alpha test, Maxwell compares the alpha the shader writes to colour target 0 with the reference after the shader and drops
// the fragment (colour, depth and stencil writes) when the comparison fails. The rewrite does the same at the end of the entry point:
// before every OpReturn it loads Location 0 .w, compares it with the reference (a constant: one module per func / ref) and kills.
// Comparisons are ordered (as the Ryujinx translator emits them with its own AlphaTestOp path); NEVER kills every fragment.
// EarlyFragmentTests is removed from the variant: a discarded fragment must not have written depth / stencil already.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

struct xbg_spirv_alpha_info {
    uint32_t returns=0;        // OpReturn sites given the test
    bool early_removed=false;  // the module had EarlyFragmentTests (host early-Z rewrite or translator): removed
    bool demote=false;         // OpDemoteToHelperInvocation (the module already declares the capability) instead of OpKill
};
// func: NVN alpha func (1 NEVER, 2 LESS, 3 EQUAL, 4 LEQUAL, 5 GREATER, 6 NOTEQUAL, 7 GEQUAL); 8 ALWAYS is the caller's no-op and is
// refused here. Returns nullptr and fills out on success; otherwise the reason (out is left empty, the caller keeps the base module).
// A fragment shader without a float vec4 output at Location 0 (Index 0), or one that never writes its .w, is refused: the Ryujinx
// translator skips the test for such shaders too (OmapTargets bit 3).
const char *xbg_spirv_alpha_test(const uint32_t *words,size_t count,uint32_t func,float ref,std::vector<uint32_t> &out,
                                 xbg_spirv_alpha_info *info=nullptr);
