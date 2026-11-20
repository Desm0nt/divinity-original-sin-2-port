// [DOS2] NR-PRESKIN DM_NR_PRESKIN (docs/dos2/nr/NR-PRESKIN.md): pre-skinning of translated skinning vertex shaders.
//
// A skinning vertex shader of the pack (the Maxwell translator's output after the load-time chain) reads bone matrices from a uniform
// block with an index made of a vertex input (Location 15 bone indices x 48) and blends them with Location 14 weights. Every pass that
// draws the mesh (3 shadow cascades, the depth pre-pass, the G-buffer, FX) repeats that work per vertex. This module finds, per vertex
// shader, the smallest set of values that separates every bone-dependent value from the rest of the shader (a minimum vertex cut of the
// SSA graph between the dynamic bone loads and everything that is not a function of vertex inputs and uniform blocks alone), and builds
//   - the SLICE: the same module reduced to the cut values' computation (the same instructions in the same order, the float ones that
//     fed an Invariant output in the original decorated NoContraction, as the driver treats those), which stores the raw 32-bit words
//     of the cut values into a storage buffer at gl_VertexIndex * m + c (m = record components). Run once per mesh instance and
//     submission as a vertex-only draw with rasterizer discard over the mesh's own vertex buffers (the same fixed-function fetch);
//   - the CONSUMER: the original module whose cut values are read from new uint vertex inputs (bitcast back) instead of computed; the
//     skinning code becomes dead and is removed, inputs only it read leave the interface. Everything else is the original instructions.
// Under SPIR-V semantics (every op rounded on its own) consumer(inputs, slice(inputs)) == original(inputs) bit for bit: the cut values
// are computed by the same instructions from the same inputs, carried as raw bits, and consumed by the same instructions
// (tools/dos2_preskin_check.py proves it with the interpreter on the pack's skinning shaders).
//
// Supported form (anything else: plan.ok = false with the reason): one Vertex entry point, one function, one basic block (OpLabel ...
// OpReturn), no Function-variable loads or stores, stores only to Output variables, uniform blocks in set 0, SPIR-V <= 1.3.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

struct XbgPreskinBind { uint32_t binding, key; };   // descriptor binding -> a stable id of the bound data (e.g. source << 16 | NVN slot)

// ctype: 0 float, 1 sint, 2 uint; first: record component; loc_mask: the Location inputs it is computed from; ubo_mask: bit i = it reads
// the uniform block plan.slice_ubos[i] (its leaves: a record made under other bindings of those holds the same value only if they match)
struct XbgPreskinVal { uint64_t hash; uint32_t comps, ctype, first, loc_mask, ubo_mask; };

struct XbgPreskinPlan {
    bool ok = false;
    const char *why = "";
    uint32_t m = 0;                       // record components (32-bit words a vertex)
    uint32_t attrs = 0;                   // consumer attributes for the plan's own record: ceil(m / 4)
    uint32_t new_locs[4] = {};            // their locations (the lowest free ones)
    uint32_t free_locs = 0;               // locations below 16 the consumer does not read (any layout's groups take the lowest of them)
    std::vector<XbgPreskinVal> vals;      // cut values, sorted by hash (the record order)
    uint64_t sig = 0;                     // hash of vals: equal signatures (same inputs / uniforms) = the same record
    uint32_t slice_locs = 0;              // Location inputs the slice reads (bit per location < 32)
    uint32_t cons_locs = 0;               // Location inputs the consumer still reads
    uint32_t orig_locs = 0;               // Location inputs of the original
    std::vector<uint32_t> slice_ubos;     // descriptor bindings (set 0) of the uniform blocks the slice reads
    std::vector<uint32_t> ubo_keys;       // their XbgPreskinBind keys (same order)
    uint32_t kdep = 0;                    // cut values that depend on a uniform block other than the bone block(s)
    uint32_t bone_binding = 0;            // the bone block (the first uniform block read through a vertex-input index)
    uint32_t bone_loads = 0;              // dynamic bone loads in the original
    uint32_t ops_orig = 0, ops_cons = 0, ops_slice = 0;   // body instructions (a cost model, not cycles)
    uint32_t ssbo_binding = 0;            // the slice's storage buffer: set 0, this binding (one past the module's largest)
    uint32_t nocontract = 0;              // slice instructions decorated NoContraction
};

// Plan for one module. binds maps the module's uniform-block bindings to data keys (hash leaves; unknown bindings = refused).
bool xbg_spirv_preskin_plan(const uint32_t *words, size_t count, const XbgPreskinBind *binds, size_t nbinds, XbgPreskinPlan &plan);
// Where the consumer finds its values: pos[j] = the record word of the plan's component j (plan.m entries; null = 0 .. m-1, its own
// record). Runs of consecutive words (at most 4) become one uint / uvecN input; group g: Location loc[g], record words [first[g],
// first[g] + width[g]) (pipeline: R32..R32G32B32A32_UINT at offset 4 * first[g] of the record binding).
struct XbgPreskinLayout { uint32_t groups = 0; uint32_t loc[16] = {}, first[16] = {}, width[16] = {}; };
bool xbg_spirv_preskin_layout(const XbgPreskinPlan &plan, const uint32_t *pos, XbgPreskinLayout &lay);
// The consumer module (plan from the same words) for a layout (pos as above). false: why holds the reason, out is empty.
bool xbg_spirv_preskin_consumer(const uint32_t *words, size_t count, const XbgPreskinPlan &plan, const uint32_t *pos, std::vector<uint32_t> &out,
                                XbgPreskinLayout *lay = nullptr, const char **why = nullptr);
// The slice module (plan from the same words): storage buffer at set 0 / plan.ssbo_binding.
bool xbg_spirv_preskin_slice(const uint32_t *words, size_t count, const XbgPreskinPlan &plan, std::vector<uint32_t> &out, const char **why = nullptr);
// Quick test at load: a vertex module that reads a uniform block with an index that depends on a vertex input (a skinning candidate).
bool xbg_spirv_preskin_candidate(const uint32_t *words, size_t count);
