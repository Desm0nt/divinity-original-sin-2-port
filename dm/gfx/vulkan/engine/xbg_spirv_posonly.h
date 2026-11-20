// [DOS2] NR-POSONLY DM_NR_POSONLY (docs/dos2/nr/NR-POSONLY.md): does a vertex shader's gl_Position (and every other built-in output)
// depend on the input at Location 0 only? Then a depth-only draw whose fragment shader reads no varying (an NR-33B "#dz" twin, or a
// fragment shader without inputs) can fetch a compact position-only stream: the other inputs are dead, whatever bytes they read.
#pragma once
#include <cstddef>
#include <cstdint>

// The conservative dependence walk (kept in step with tools/dos2_posonly_check.py static_posonly): every result id carries the mask of
// Location inputs it may depend on - through SSA operands (literal operands of OpVectorShuffle / OpCompositeExtract / Insert / OpExtInst
// skipped), through Function / Private variables flow-insensitively (a store taints the whole root variable), and every branch / switch
// condition of the module taints every output. Refused (false): more than one function, a call, a store to any storage class but Output /
// Function / Private, image writes, atomics, OpCopyMemory, no built-in output, no fixpoint in 64 rounds. *dep = the mask gl_Position and
// the other built-in outputs depend on; why = the reason when false.
bool xbg_spirv_posonly(const uint32_t *words, size_t count, uint32_t *dep = nullptr, const char **why = nullptr);
