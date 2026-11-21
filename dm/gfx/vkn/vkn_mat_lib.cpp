// [DOS2] VK-NATIVE stage 5.2 B5 (vkn_mat.c, DM_VK_NATIVE_MAT / DM_VK_NATIVE_XLQ): C doors to NR-32's vertex-output trim
// (dm/gfx/vulkan/engine/xbg_spirv_vsout.cpp, the one trim of the tree, linked as it is in the engine archive) for vkn_mat.c. Pure
// functions of their arguments; the words are the library's final words (vkn_shlib_b5_words). No game addresses.
#ifdef DM_GAME_DOS2
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "../vulkan/engine/xbg_spirv_vsout.h"

extern "C" {
// xbg_spirv_fs_input_mask (read, *declared) / xbg_spirv_vs_output_mask of a module (XBG_VSOUT_ALL = not analysable: no trim).
uint64_t vkn_mat_fs_read(const uint32_t *w, size_t n, uint64_t *declared) { return xbg_spirv_fs_input_mask(w, n, declared); }
uint64_t vkn_mat_vs_written(const uint32_t *w, size_t n) { return xbg_spirv_vs_output_mask(w, n); }
// The old path's vsout_module minus its cache and module: xbg_spirv_vsout_trim(keep, declared), accepted only when
// xbg_spirv_vsout_check agrees. 0 = *out (malloc'd, *out_n words; stores / outputs dropped), 1 = nothing to drop, -1 = the check
// refused the trim (*why = its reason), -2 = out of memory.
int vkn_mat_vsout(const uint32_t *w, size_t n, uint64_t keep, uint64_t declared, uint32_t **out, size_t *out_n, uint32_t *stores, uint32_t *vars,
                  const char **why) {
    *out = nullptr; *out_n = 0; if (why) *why = "";
    std::vector<uint32_t> o; XbgVsoutStats st;
    if (!xbg_spirv_vsout_trim(w, n, keep, o, &st, declared)) return 1;
    const char *bad = xbg_spirv_vsout_check(w, n, o.data(), o.size(), keep);
    if (*bad) { if (why) *why = bad; return -1; }
    uint32_t *m = static_cast<uint32_t *>(malloc(o.size() * 4 + 4)); if (!m) return -2;
    memcpy(m, o.data(), o.size() * 4); *out = m; *out_n = o.size();
    if (stores) *stores = st.stores_dropped;
    if (vars) *vars = st.vars_dropped;
    return 0;
}
}
#endif
