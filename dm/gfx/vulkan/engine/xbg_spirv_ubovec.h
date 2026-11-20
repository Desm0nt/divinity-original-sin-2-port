// [DOS2][NR-27] DM_NR_UBOVEC: scalar uniform-buffer reads of one vec4 merged into one vec4 load (see xbg_spirv_ubovec.cpp)
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// What is known of a 32-bit integer SSA value: value == (S * mul + add) mod 2^bits, zero-extended, where S is the value of the
// SSA id `sym` (sym == 0: the constant `add`; sym with bit 31 set: the quotient of that id by `mul`, a value that exists only
// in the proof). Two ids with equal facts hold equal values wherever both are defined. Exposed for tests/ubovec_test.cpp.
struct XbgUvFact {
    uint32_t sym = 0, mul = 0, add = 0; uint8_t bits = 32;
    bool operator==(const XbgUvFact &o) const { return sym == o.sym && mul == o.mul && add == o.add && bits == o.bits; }
    bool constant() const { return sym == 0; }
};
namespace xbg_ubovec {
XbgUvFact constant(uint32_t value);
XbgUvFact opaque(uint32_t sym);                                   // the value of id sym itself
// Each returns false when the result is not expressible (the caller then treats the result id as opaque).
bool add(const XbgUvFact &a, const XbgUvFact &b, XbgUvFact &r);
bool sub(const XbgUvFact &a, const XbgUvFact &b, XbgUvFact &r);
bool mul(const XbgUvFact &a, const XbgUvFact &b, XbgUvFact &r);
bool shl(const XbgUvFact &a, uint32_t s, XbgUvFact &r);
bool shr(const XbgUvFact &a, uint32_t s, bool arithmetic, XbgUvFact &r);
bool band(const XbgUvFact &a, uint32_t mask, XbgUvFact &r);
// a + b of two different symbols: a multiple of a power of two plus a known remainder (quotient symbol = result | 1 << 31).
bool add_aligned(const XbgUvFact &a, const XbgUvFact &b, uint32_t result, XbgUvFact &r);
}

struct XbgUbovecStats {
    uint32_t candidates = 0;   // scalar loads of a component of a uniform-block vector with a proven constant component
    uint32_t reads = 0;        // of them rewritten to OpCompositeExtract of a vector load
    uint32_t vec_loads = 0;    // vector loads emitted (reads - vec_loads scalar loads fewer for a compiler that merges nothing)
    uint32_t dynamic = 0;      // scalar loads left because the component index is not provably constant
    uint32_t chains_dropped = 0;
};

// Every OpLoad of a scalar through OpAccessChain into a vector that lives in a uniform block (storage class Uniform, struct
// decorated Block), whose component index is provably constant, joins the first such load of the same vector that dominates it
// (same block variable, provably equal indexes): that first load becomes a load of the whole vector, and each read an
// OpCompositeExtract of it under the old result id. Vectors of which only one component is ever read under one leader stay as
// they are. Exactly the same values for every input (uniform blocks are read-only during the invocation).
// Only vectors with a computed index are merged (the LDCs with a computed address: the light loop, skinning); with fixed_too
// also those at a fixed place in the block (a driver keeps those in uniform registers: measured neutral on Mali, NR-27.md).
// poison (DM_NR_UBOVEC=3): the rewritten reads take the next component instead - visibly wrong, to see on screen which
// materials the rewrite reaches. Returns the number of reads rewritten (0: out is empty, use the module unchanged).
size_t xbg_spirv_ubovec(const uint32_t *words, size_t count, std::vector<uint32_t> &out, XbgUbovecStats *stats = nullptr, bool poison = false,
                        bool fixed_too = false);
// Independent structural check of a rewrite (used by DM_NR_UBOVEC=2 and the tests): `out` must equal `in` except for dropped
// uniform access chains, new vector access chain + load pairs, and loads turned into extracts whose vector is a load, earlier in
// the same block or in a dominating block, of the same block variable with pairwise provably equal indexes and the component the
// old chain named. Returns an empty string when it holds, else the first violation.
const char *xbg_spirv_ubovec_check(const uint32_t *in, size_t in_count, const uint32_t *out, size_t out_count, bool poison = false);
