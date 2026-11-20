// [DOS2] NR-33B: material shaders in depth-only passes and unread varyings (DM_NR_MAT, default 0; docs/dos2/nr/NR-33B.md). Pure C: no
// Vulkan, no game addresses, no host types. The host (xbg_nr_mat.inc) asks it three things:
//  1. nr_mat_scan: what a module is - fragment: kill / demote, fragment built-in outputs (depth, sample mask, stencil), side effects
//     (image / storage writes, atomics), the Location inputs it reads; vertex: the Location outputs it writes. A fragment shader with none
//     of the first three, drawn into a pass without colour attachments with the alpha test off (and no alpha to coverage), has no
//     visible effect at all (Vulkan drops outputs without an attachment): the empty fragment shader (nr_mat_empty_fs) is exact there.
//  2. nr_mat_gate_*: the gate records setup builds per translated fragment key (tools/dos2_nr_mat.py -> <shaders>/native-nr33b.xgspack,
//     pack key "dg.<key>"): paths of the shader decided only by "uniform word OP constant" comparisons that kill nothing, with the
//     constant Location 0 .w they leave. A draw whose uniforms satisfy such a path never kills, and with the NVN alpha test on it passes
//     iff nr_mat_alpha_pass(func, w, ref): then the empty fragment shader is exact for it too (the Base_PBR "fade" depth shaders: the
//     dissolve only runs while c3[0].x == 1.0; every other draw writes .w = 1.0 > 0.5).
//  3. nr_mat_prune: a vertex shader's stores into Location outputs the fragment shader does not read are removed (the outputs stay
//     declared; nothing else changes, gl_Position and the built-ins are untouched): the driver then drops their math, and tilers that
//     store varyings in memory (Mali IDVS, PowerVR parameter buffer) stop writing them. Refused (kept as is) when an output it would
//     prune is read back, copied, passed to a call / extended instruction, or a Location variable is not a plain scalar / vector / array.
//     NRVS: not used by the engine any more - the twin pipelines get NR-32's trim (xbg_spirv_vsout, read 0), which leaves no more stores on
//     any vertex shader of the pack (tests/nrvs_equiv_test.cpp); kept as the reference of nr_mat_test / tools/dos2_nr_mat_proof.py.
// Thread safe: everything is a pure function of its arguments.
#ifndef DM_NR_MAT_H
#define DM_NR_MAT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { NR_MAT_KILL = 1, NR_MAT_FRAGOUT = 2, NR_MAT_SIDE = 4, NR_MAT_COMPLEX_IO = 8, NR_MAT_BAD = 0x80 };
typedef struct nr_mat_facts {
    unsigned flags;        // NR_MAT_*
    uint32_t in_mask;      // Location inputs (bit per location)
    uint32_t out_mask;     // Location outputs
    uint32_t model;        // execution model of the entry point (0 vertex, 4 fragment), ~0 none
} nr_mat_facts;
// 0 = scanned (f filled); -1 = not a well-formed module (f->flags = NR_MAT_BAD)
int nr_mat_scan(const uint32_t *w, size_t n, nr_mat_facts *f);
// 1: a fragment shader that has no effect without colour attachments and with the alpha test off
int nr_mat_fs_pure(const nr_mat_facts *f);

// The empty fragment shader (OpCapability Shader, OriginUpperLeft, main = OpReturn): *n words.
const uint32_t *nr_mat_empty_fs(size_t *n);

// ---- gate records ----
#define NR_MAT_GATE_MAX_PATHS 16
#define NR_MAT_GATE_MAX_TERMS 64
typedef struct nr_mat_term { uint32_t bank_word, op, cbits, flags; } nr_mat_term;   // flags: 1 uniform is the left operand, 2 expected true
typedef struct nr_mat_path { uint32_t w_bits, w_known, first, count; } nr_mat_path;
typedef struct nr_mat_gate {
    uint64_t spirv_fnv;    // FNV-1a 64 of the translated bundle's SPIR-V bytes it was proven on
    uint32_t paths, terms;
    nr_mat_path path[NR_MAT_GATE_MAX_PATHS];
    nr_mat_term term[NR_MAT_GATE_MAX_TERMS];
} nr_mat_gate;
uint64_t nr_mat_fnv64(const void *data, size_t n);
// 0 = parsed; else a reason (static string): bad magic / version / sizes / an unknown opcode / out-of-range term
const char *nr_mat_gate_parse(const uint8_t *b, size_t n, nr_mat_gate *g);
// SPIR-V comparison (opcodes 170..191) on raw 32-bit words: FOrd* false on NaN, FUnord* true on NaN, I / U / S as named. -1 unknown op.
int nr_mat_cmp(uint32_t op, uint32_t a, uint32_t b);
// The first path every term of which holds (read(ctx, bank_word, &word) = 1 when the word is readable), -1 none (an unreadable word fails
// its path).
typedef int (*nr_mat_read_fn)(void *ctx, uint32_t bank_word, uint32_t *out);
int nr_mat_gate_eval(const nr_mat_gate *g, nr_mat_read_fn read, void *ctx);
// NVN alpha test (xbg_spirv_alpha.cpp: ordered comparisons, 1 NEVER .. 7 GEQUAL, 8 ALWAYS): 1 = a fragment with this .w passes
int nr_mat_alpha_pass(uint32_t func, uint32_t a_bits, float ref);

// ---- vertex outputs ----
// out (room for n words) = w without the stores into Location outputs whose locations are all outside keep (*out_n words). Returns the
// number of stores removed (>= 0), *pruned = the output locations no longer written; < 0 = refused (out undefined): -1 malformed, -2 an
// output to prune is read / copied / passed on, -3 a Location variable of another type (struct, matrix, 64-bit), -4 nothing to prune.
int nr_mat_prune(const uint32_t *w, size_t n, uint32_t keep, uint32_t *out, size_t *out_n, uint32_t *pruned);

#ifdef __cplusplus
}
#endif
#endif
