// [DOS2] NR-01: the engine object registry of the native renderer (groundwork; DM_NR, default 0). Pure C: no Vulkan, no game addresses,
// no host types - dm/host/hle_dos2_nr.c fills it from hooks one level above NVN, dm/gfx/vulkan/engine/tests/nr_reg_test.cpp tests it on
// the PC, tools/dos2_nr_check.py verifies the embedded engine format table against the game's main.
//
// Every object is keyed by the ENGINE's handle (u64: low 32 bits = index, bits 32..53 = generation; generation 0 = no handle). A table is
// indexed by the handle index; an entry remembers the whole handle, so a put with another generation on a live entry is a reuse (the old
// object counts as replaced = an implicit destroy the hooks did not see). Tables are bounded: an index past the table only bumps
// `overflow`, a zero generation only `bad_handle` (textures, shaders, buffers: the engine checks their generation; samplers, state
// objects and vertex formats are stored under any value); nothing here can fail harder than that.
// Not thread safe by itself: the caller serialises every call (hle_dos2_nr.c: one mutex around all registry calls).
#ifndef DM_NR_REG_H
#define DM_NR_REG_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif

// ---- engine format table (enum 0..62 of the renderer; main.raw: NVN format = switch of 0x919c20, names / sizes = 0x91eea0) ----
enum { NR_FMT_N = 63, NR_NVN_TRAP = 0xffff };
typedef struct nr_fmt_row { const char *name; uint16_t nvn, nvn_srgb; uint8_t channels, bytes, compressed; } nr_fmt_row;   // nvn NR_NVN_TRAP: the engine traps
extern const nr_fmt_row nr_formats[NR_FMT_N];
enum { NR_CLS_NONE, NR_CLS_U8, NR_CLS_HDR, NR_CLS_DEPTH, NR_CLS_BC, NR_CLS_N };   // census classes of an engine format
int nr_fmt_class(unsigned efmt);
const char *nr_cls_name(int cls);
const char *nr_fmt_name(unsigned efmt);
// NVN format the engine asks for. override_nvn: the NVN format the caller forces (CreateTexture: 0 = none; CreateTextureFromImage: -1 =
// none, else the image's own word, which the engine swaps inside its linear / sRGB pair by the sRGB bit). -1 = unknown (no check).
int nr_expect_nvn_format(unsigned efmt, int srgb, int override_nvn, int from_image);

static inline uint32_t nr_h_idx(uint64_t h) { return (uint32_t)h; }
static inline uint32_t nr_h_gen(uint64_t h) { return (uint32_t)(h >> 32) & 0x3fffffu; }

// ---- textures ----
enum { NR_SRC_NONE, NR_SRC_CREATE, NR_SRC_CREATE_DATA, NR_SRC_IMAGE_LINEAR, NR_SRC_IMAGE_PACKAGED, NR_SRC_RT, NR_SRC_WINDOW, NR_SRC_N };
const char *nr_src_name(int src);
// engine texture flags (u16 of CreateTexture / the record's +0x14)
enum { NR_TF_SRGB = 1, NR_TF_CUBE = 2, NR_TF_COLOR_RT = 0x60, NR_TF_DEPTH = 0x300, NR_TF_IMAGE = 0xc00, NR_TF_SHARED_POOL = 0x2000 };
typedef struct nr_nvn_tex {   // what the NVN texture builder really held at nvnTextureInitialize
    uint64_t obj, size;       // NVN texture object, storage bytes of the layout the host computed
    uint32_t w, h, d, levels;
    int32_t target, format, flags, samples;
    uint8_t packaged;         // SetPackagedTextureLayout was called on the builder
} nr_nvn_tex;
typedef struct nr_tex {
    uint64_t handle;          // creates == 0: slot never used
    uint32_t w, h, d;         // as the engine call gave them (0 allowed: the engine clamps)
    int32_t fmt_override;     // NVN format override of the call (CreateTexture 0 = none, image -1 = none)
    uint16_t eflags; uint8_t eflags2, efmt, mips, layers, msaa, src, srgb, alive, nnvn;   // nnvn: NVN textures initialised inside the engine call
    nr_nvn_tex nvn;           // the first of them
    uint32_t creates, destroys, replaced;
    uint32_t bad;             // NR_BAD_* of the last check (mode 2)
} nr_tex;

// ---- samplers / state objects / vertex formats / buffers / shaders ----
typedef struct nr_nvn_smp { uint64_t obj; uint8_t minf, magf, ws, wt, wr, cmp_mode, cmp_func, have; float aniso; } nr_nvn_smp;
typedef struct nr_sampler { uint64_t handle; uint8_t filter, wrap[3], aniso, compare, alive; nr_nvn_smp nvn; uint32_t creates, bad; } nr_sampler;
enum { NR_ST_BLEND, NR_ST_DEPTH, NR_ST_RASTER, NR_ST_N, NR_ST_ARGS = 8 };
const char *nr_state_name(int kind);
typedef struct nr_state { uint64_t handle; uint8_t arg[NR_ST_ARGS], nargs, alive; uint32_t creates, bad; } nr_state;
enum { NR_VF_ELEMS = 32 };   // elements kept per vertex format (6 bytes each: stream, usage, ?, ?, type, components)
typedef struct nr_vformat { uint64_t handle; uint16_t count; uint8_t kept, alive; uint8_t elem[NR_VF_ELEMS][6]; uint32_t creates; } nr_vformat;
enum { NR_BUF_VERTEX, NR_BUF_INDEX, NR_BUF_N, NR_USAGE_DYNAMIC = 3 };
typedef struct nr_buffer { uint64_t handle, size, nvn_obj, nvn_size; uint32_t count; uint8_t usage, index_size, alive; uint32_t creates, destroys, replaced; } nr_buffer;
enum { NR_STAGE_VS, NR_STAGE_FS, NR_STAGE_CS, NR_STAGES, NR_NAME = 128, NR_PATH = 192 };
typedef struct nr_shader {
    uint64_t handle, prog[NR_STAGES];     // NVN program object per stage (0 = none)
    char key[NR_STAGES][17];              // shader pack key of the stage's program ("" = unknown)
    char name[NR_NAME], path[NR_PATH];    // ShaderManager::Get name (.bshd) / LoadShaderFile path ("" = created without them)
    uint8_t alive; uint32_t creates, gets, replaced, bad;
} nr_shader;

// ---- render-target sets, named resource transitions (engine opcode 55), shared RT pool reservations ----
enum { NR_RT_COLORS = 8 };
typedef struct nr_rtset { uint64_t color[NR_RT_COLORS], depth, count; uint32_t n, seq; } nr_rtset;   // seq: order of first sight, from 1 (0 = empty slot)
typedef struct nr_trans { uint64_t name_key, handle, count; uint32_t state, handles, seq; char name[64]; } nr_trans;   // handles: times the handle changed
typedef struct nr_pool { uint64_t key, count; uint32_t efmt, w, h, seq; } nr_pool;

enum { NR_TEX_MAX = 32768, NR_SHADER_MAX = 8192, NR_SAMPLER_MAX = 256, NR_STATE_MAX = 256, NR_VF_MAX = 2048, NR_BUF_MAX = 65536,
       NR_RTSET_MAX = 1024, NR_TRANS_MAX = 256, NR_POOL_MAX = 128 };
typedef struct nr_kind_stat { uint64_t puts, kills, replaced, kill_miss, overflow, bad_handle; uint32_t hi; } nr_kind_stat;   // hi: highest index used + 1
enum { NR_K_TEX, NR_K_SHADER, NR_K_SAMPLER, NR_K_BLEND, NR_K_DEPTH, NR_K_RASTER, NR_K_VFORMAT, NR_K_VB, NR_K_IB, NR_K_N };
const char *nr_kind_name(int kind);
typedef struct nr_reg {
    nr_tex *tex; nr_shader *shader; nr_sampler *sampler; nr_state *state[NR_ST_N]; nr_vformat *vformat; nr_buffer *buf[NR_BUF_N];
    nr_rtset *rtset; nr_trans *trans; nr_pool *pool;
    nr_kind_stat st[NR_K_N];
    uint32_t nrtset, ntrans, npool; uint64_t rtset_calls, rtset_lost, trans_calls, trans_lost, pool_calls, pool_lost, shader_gets, shader_get_miss;
} nr_reg;

// All tables in one allocation (install time; never on a hot path). NULL = no memory. nr_reg_free(NULL) is fine.
nr_reg *nr_reg_new(void);
void nr_reg_free(nr_reg *r);

// put: the object the engine call created under `handle`. Returns the entry, NULL if the handle cannot be stored (counted).
typedef struct nr_tex_desc { uint32_t w, h, d; int32_t fmt_override; uint16_t eflags; uint8_t eflags2, efmt, mips, layers, msaa, src, srgb; } nr_tex_desc;
nr_tex *nr_tex_put(nr_reg *r, uint64_t handle, const nr_tex_desc *e, const nr_nvn_tex *nvn, int nnvn);
int nr_tex_kill(nr_reg *r, uint64_t handle);       // 1 = was alive under this very handle
int nr_tex_kill_obj(nr_reg *r, uint64_t nvn_obj);  // by NVN texture object (the engine's deferred destroy knows only that); 1 = found
const nr_tex *nr_tex_get(const nr_reg *r, uint64_t handle);   // alive entry of this very handle, else NULL
nr_sampler *nr_sampler_put(nr_reg *r, uint64_t handle, unsigned filter, unsigned wu, unsigned wv, unsigned ww, unsigned aniso, unsigned compare, const nr_nvn_smp *nvn);
nr_state *nr_state_put(nr_reg *r, int kind, uint64_t handle, const uint8_t *arg, int nargs);
nr_vformat *nr_vformat_put(nr_reg *r, uint64_t handle, const uint8_t *elems, unsigned count);   // the engine returns an old handle for equal elements: counted in creates
nr_buffer *nr_buffer_put(nr_reg *r, int kind, uint64_t handle, uint64_t size, uint32_t count, unsigned index_size, unsigned usage, uint64_t nvn_obj, uint64_t nvn_size);
int nr_buffer_kill(nr_reg *r, int kind, uint64_t handle);
nr_shader *nr_shader_put(nr_reg *r, uint64_t handle, const char *name, const char *path, const uint64_t prog[NR_STAGES], const char key[NR_STAGES][17]);
const nr_shader *nr_shader_get(const nr_reg *r, uint64_t handle);
// ShaderManager::Get returned `value` for `name` without creating a shader (cache hit): counts it, names an unnamed shader. 1 = known handle.
int nr_shader_note_get(nr_reg *r, uint64_t value, const char *name);
int nr_rtset_note(nr_reg *r, unsigned n, const uint64_t *color, uint64_t depth);   // 1 = a set not seen before, 0 = known, -1 = not stored
int nr_trans_note(nr_reg *r, uint64_t name_key, const char *name, uint64_t handle, unsigned state);   // key = (name_key, state); same returns
const nr_trans *nr_trans_find(const nr_reg *r, uint64_t name_key, unsigned state);
int nr_pool_note(nr_reg *r, uint64_t key, unsigned efmt, unsigned w, unsigned h);

// ---- checks (DM_NR=2). A mask of NR_BAD_*; 0 = consistent. ----
enum { NR_BAD_FORMAT = 1, NR_BAD_DIMS = 2, NR_BAD_MIPS = 4, NR_BAD_TARGET = 8, NR_BAD_PACKAGED = 16, NR_BAD_NO_NVN = 32, NR_BAD_SAMPLES = 64,
       NR_BAD_RECORD = 128, NR_BAD_ENUM = 256, NR_BAD_PROGRAM = 512, NR_BAD_EFMT = 1024, NR_BAD_FILTER = 2048, NR_BAD_BITS = 12 };
const char *nr_bad_name(int bit);            // bit index 0..NR_BAD_BITS-1
int nr_bad_text(unsigned mask, char *out, size_t cap);   // "format+dims"
int nr_expect_target(const nr_tex *t);       // NVN texture target the engine picks (-1 = unknown shape)
unsigned nr_check_tex(const nr_tex *t);
unsigned nr_check_sampler(const nr_sampler *s);
unsigned nr_check_state(int kind, const uint8_t *arg, int nargs);
unsigned nr_check_shader(const nr_shader *s);

// ---- census, one-line summary, TSV ----
typedef struct nr_census {
    uint32_t alive[NR_K_N];
    uint32_t tex_src[NR_SRC_N]; uint64_t tex_src_bytes[NR_SRC_N], tex_cls_bytes[NR_CLS_N], tex_bytes;
    uint32_t tex_no_nvn, shaders_named, programs, programs_keyed, keys_distinct_hint;
    uint32_t buf_n[NR_BUF_N][2]; uint64_t buf_bytes[NR_BUF_N][2];   // [kind][0 static, 1 dynamic]
} nr_census;
void nr_census_take(const nr_reg *r, nr_census *c);
int nr_summary(const nr_reg *r, char *out, size_t cap);   // one line, no newline; returns its length
enum { NR_TSV_FORMATS, NR_TSV_TEXTURES, NR_TSV_SHADERS, NR_TSV_SAMPLERS, NR_TSV_STATES, NR_TSV_VFORMATS, NR_TSV_BUFFERS, NR_TSV_RTSETS,
       NR_TSV_TRANSITIONS, NR_TSV_POOLS, NR_TSV_N };
const char *nr_tsv_name(int which);                       // file name, e.g. "nr_textures.tsv"
int nr_write_tsv(const nr_reg *r, int which, FILE *o);    // header line + one row per object ever seen (alive column); returns the rows

#ifdef __cplusplus
}
#endif
#endif
