# [DOS2] SPIRV-Tools (optimizer + validator) as a static library for the on-device shader optimizer (DM_GFX_SPV_OPT,
# dm/gfx/vulkan/engine/xbg_spvopt.cpp), built from the NDK's own copy (sources/third_party/shaderc/third_party/spirv-tools,
# v2022.4, Apache-2.0, with its external/spirv-headers, MIT-style Khronos licence). The NDK ships no CMake file for it, only
# Android.mk: the source lists are read from that Android.mk, and the generated tables (*.inc, the DebugInfo headers) are made at
# configure time with the scripts in its utils/ directory, as the Android.mk rules do. -Os, function/data sections (the NDK link
# uses --gc-sections), hidden visibility (nothing exported from libdm.so), no exceptions / RTTI (as the Android.mk builds it).
# Result: target dm_spirv_tools.
if(NOT DM_SPIRV_TOOLS_DIR)
  set(DM_SPIRV_TOOLS_DIR ${ANDROID_NDK}/sources/third_party/shaderc/third_party/spirv-tools)
endif()
set(_spvt ${DM_SPIRV_TOOLS_DIR})
set(_spvh ${_spvt}/external/spirv-headers)
if(NOT EXISTS ${_spvt}/Android.mk OR NOT EXISTS ${_spvh}/include/spirv/unified1/spirv.core.grammar.json)
  message(FATAL_ERROR "SPIRV-Tools sources not found at ${_spvt} (set DM_SPIRV_TOOLS_DIR; needs external/spirv-headers)")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_gen ${CMAKE_BINARY_DIR}/spirv-tools-gen)
file(MAKE_DIRECTORY ${_gen})
set(_g ${_spvh}/include/spirv/unified1)
set(_py ${Python3_EXECUTABLE})
set(_gt ${_spvt}/utils/generate_grammar_tables.py)
macro(_spvt_run)
  execute_process(COMMAND ${_py} ${ARGN} RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "SPIRV-Tools table generation failed: ${ARGN}\n${_err}")
  endif()
endmacro()
if(NOT EXISTS ${_gen}/generators.inc)
  _spvt_run(${_gt} --spirv-core-grammar=${_g}/spirv.core.grammar.json --extinst-glsl-grammar=${_g}/extinst.glsl.std.450.grammar.json
    --extinst-opencl-grammar=${_g}/extinst.opencl.std.100.grammar.json --extinst-debuginfo-grammar=${_g}/extinst.debuginfo.grammar.json
    --extinst-cldebuginfo100-grammar=${_g}/extinst.opencl.debuginfo.100.grammar.json
    --core-insts-output=${_gen}/core.insts-unified1.inc --glsl-insts-output=${_gen}/glsl.std.450.insts.inc
    --opencl-insts-output=${_gen}/opencl.std.insts.inc --operand-kinds-output=${_gen}/operand.kinds-unified1.inc)
  _spvt_run(${_gt} --spirv-core-grammar=${_g}/spirv.core.grammar.json --extinst-debuginfo-grammar=${_g}/extinst.debuginfo.grammar.json
    --extinst-cldebuginfo100-grammar=${_g}/extinst.opencl.debuginfo.100.grammar.json
    --extension-enum-output=${_gen}/extension_enum.inc --enum-string-mapping-output=${_gen}/enum_string_mapping.inc)
  foreach(_v "debuginfo|" "opencl.debuginfo.100|CLDEBUG100_" "nonsemantic.shader.debuginfo.100|SHDEBUG100_" "spv-amd-gcn-shader|"
             "spv-amd-shader-ballot|" "spv-amd-shader-explicit-vertex-parameter|" "spv-amd-shader-trinary-minmax|" "nonsemantic.clspvreflection|")
    string(REPLACE "|" ";" _vl "${_v}")
    list(GET _vl 0 _name)
    list(LENGTH _vl _vn)
    set(_prefix "")
    if(_vn GREATER 1)
      list(GET _vl 1 _prefix)
    endif()
    _spvt_run(${_gt} --extinst-vendor-grammar=${_g}/extinst.${_name}.grammar.json --vendor-insts-output=${_gen}/${_name}.insts.inc
      --vendor-operand-kind-prefix=${_prefix})
  endforeach()
  _spvt_run(${_spvt}/utils/generate_language_headers.py --extinst-grammar=${_g}/extinst.debuginfo.grammar.json --extinst-output-path=${_gen}/DebugInfo.h)
  _spvt_run(${_spvt}/utils/generate_language_headers.py --extinst-grammar=${_g}/extinst.opencl.debuginfo.100.grammar.json --extinst-output-path=${_gen}/OpenCLDebugInfo100.h)
  _spvt_run(${_spvt}/utils/generate_language_headers.py --extinst-grammar=${_g}/extinst.nonsemantic.shader.debuginfo.100.grammar.json --extinst-output-path=${_gen}/NonSemanticShaderDebugInfo100.h)
  _spvt_run(${_spvt}/utils/update_build_version.py ${_spvt}/CHANGES ${_gen}/build-version.inc)
  _spvt_run(${_spvt}/utils/generate_registry_tables.py --xml=${_spvh}/include/spirv/spir-v.xml --generator-output=${_gen}/generators.inc)
endif()
# Source lists: every source/*.cpp named in the Android.mk (SPVTOOLS_SRC_FILES + SPVTOOLS_OPT_SRC_FILES).
file(READ ${_spvt}/Android.mk _mk)
string(REGEX MATCHALL "source/[A-Za-z0-9_/]+\\.cpp" _srcs "${_mk}")
list(REMOVE_DUPLICATES _srcs)
list(TRANSFORM _srcs PREPEND ${_spvt}/)
add_library(dm_spirv_tools STATIC ${_srcs})
target_include_directories(dm_spirv_tools PRIVATE ${_spvt} ${_spvt}/source ${_spvh}/include ${_gen})
target_include_directories(dm_spirv_tools SYSTEM PUBLIC ${_spvt}/include)
target_compile_features(dm_spirv_tools PRIVATE cxx_std_17)
target_compile_options(dm_spirv_tools PRIVATE -Os -ffunction-sections -fdata-sections -fvisibility=hidden -fvisibility-inlines-hidden
  -fno-exceptions -fno-rtti -g0 -w)   # -g0: the APK packs libdm.so unstripped; debug info for SPIRV-Tools would add ~75 MB
set_target_properties(dm_spirv_tools PROPERTIES POSITION_INDEPENDENT_CODE ON)
