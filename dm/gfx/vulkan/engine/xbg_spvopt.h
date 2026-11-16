// [DOS2] DM_GFX_SPV_OPT: on-device SPIR-V optimizer with a persistent cache (SPIRV-Tools, dm/android/spirv_tools.cmake).
// The offline translator (dm/gfx/tools/dmsh_vk_xc2.py) never runs spirv-opt -O; on 300 DOS2 bundles -O cut instructions to ~0.35x.
// Only the final words handed to vkCreateShaderModule go through here (after every rewrite: alpha variants, vertex retype,
// warm::bundle_words), so the rewrites never see optimized SPIR-V.
//   lookup(): the optimized module from <dir>/<hash>-<words>.spv when present (cache listed once at init, no file probes per
//   module); otherwise false (the caller creates the module from the original words at once, no stall) and the words are queued
//   to one low-priority background thread that runs Optimizer::RegisterPerformancePasses (= spirv-opt -O) with input validation,
//   validates the result (Vulkan 1.1, or the env of the module's SPIR-V version), and writes the cache file (tmp + rename). Any
//   failure keeps the original and adds the hash to <dir>/bad.txt (never retried). The optimized module is used from the next
//   time that module is created (in practice the next launch), never swapped in under a live pipeline.
// Built only where the library is linked (DM_SPVOPT=1: DM_GAME_DOS2 Android builds); elsewhere the header gives inline stubs.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#ifdef DM_SPVOPT
// mode 0 off, 1 cache + background optimizer, 2 = 1 plus a log line per optimized module (size before/after, validation).
void xbg_spvopt_init(const char *dir,int mode);
int xbg_spvopt_mode();
bool xbg_spvopt_lookup(const uint32_t *words,size_t n,std::vector<uint32_t> &out,const char *what);
// The driver refused the optimized module: remember it as bad (the caller then creates the original).
void xbg_spvopt_reject(const uint32_t *words,size_t n,const char *what);
void xbg_spvopt_summary(const char *when);
#else
inline void xbg_spvopt_init(const char*,int){}
inline int xbg_spvopt_mode(){return 0;}
inline bool xbg_spvopt_lookup(const uint32_t*,size_t,std::vector<uint32_t>&,const char*){return false;}
inline void xbg_spvopt_reject(const uint32_t*,size_t,const char*){}
inline void xbg_spvopt_summary(const char*){}
#endif
